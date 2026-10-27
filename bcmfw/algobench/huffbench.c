/***************************************************************************
 *              __________                   __    __________
 *   Open       \______   \_______    ____  |  | __\______   \  ____  ___  ___
 *   Source      |     ___/\_  __ \  /  _ \ |  |/ / |    |  _/ /  _ \ \  \/  /
 *   Player      |    |     |  | \/ (  <_> )|    <  |    |   \(  <_> ) >    <
 *   Firmware    |____|     |__|     \____/ |__|_ \ |______  / \____/ /__/\_ \
 *                                               \/        \/               \/
 *
 * Custom firmware for the iPod Video, 5th generation (5G and 5.5G).
 * Not affiliated with or endorsed by Apple Inc.
 *
 * Copyright (C) 2026 prokopto-dev
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

/* Entropy-decode bench: jpeg_load.c's Huffman path (replicated) against an
 * exact table-driven form.  Baseline, 8-bit, Huffman, no restarts.  Both
 * write the same dequantised coefficients (positions < k_need) and the
 * same per-block checksum; the scaffolding (headers) is shared.  Scratch
 * code for docs/research/bcm2722/algorithms.md, not firmware. */
#include <stdint.h>
#include <stddef.h>
void *memset(void *, int, size_t); void *memcpy(void *, const void *, size_t);

typedef struct {
    int maxcode[18], valptr[17], mincode[17];
    int look_nbits[256];
    unsigned char look_sym[256];
    unsigned char bits[17], val[256];
    /* fast form */
    uint16_t look[512];     /* len << 8 | sym, 0 = slow (9-bit window) */
    int16_t fac[512];       /* AC: value << 8 | run << 4 | total len, 0 = no */
    uint16_t fdc[512];      /* DC: total len << 8 | s, 0 = no (value: bits) */
    uint8_t skp[512];       /* AC skip: total len | eob << 7 ... see build */
    uint8_t sadv[512];      /* AC skip: coefficient advance (r + 1) */
    uint32_t ms[4096];      /* AC multi-skip (12 bits): bits | adv << 8 | eob << 16 */
} htab;

static htab dct[2], act[2];
static int16_t qt[4][64];
static int ncomp, hs[3], vs[3], tq[3], td[3], ta[3], W, H;
static const uint8_t *ecs, *ecs_end;

static const unsigned char zag[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63 };

static void derive(htab *t)
{
    int p = 0, l, i, code = 0, si, huffsize[257], huffcode[257];
    for (l = 1; l <= 16; l++)
        for (i = 0; i < t->bits[l]; i++)
            huffsize[p++] = l;
    huffsize[p] = 0;
    p = 0; si = huffsize[0];
    while (huffsize[p]) {
        while (huffsize[p] == si) huffcode[p++] = code++;
        code <<= 1; si++;
    }
    p = 0;
    for (l = 1; l <= 16; l++) {
        if (t->bits[l]) {
            t->valptr[l] = p; t->mincode[l] = huffcode[p];
            p += t->bits[l]; t->maxcode[l] = huffcode[p - 1];
        } else t->maxcode[l] = -1;
    }
    t->maxcode[17] = 0xFFFFF;
    memset(t->look_nbits, 0, sizeof t->look_nbits);
    memset(t->look, 0, sizeof t->look);
    p = 0;
    for (l = 1; l <= 16; l++)
        for (i = 0; i < t->bits[l]; i++, p++) {
            if (l <= 8) {
                int lb = huffcode[p] << (8 - l), c;
                for (c = 1 << (8 - l); c > 0; c--, lb++) {
                    t->look_nbits[lb] = l; t->look_sym[lb] = t->val[p];
                }
            }
            if (l <= 9) {
                int lb = huffcode[p] << (9 - l), c;
                for (c = 1 << (9 - l); c > 0; c--, lb++)
                    t->look[lb] = l << 8 | t->val[p];
            }
        }
    /* combined tables over the 9-bit window */
    for (i = 0; i < 512; i++) {
        int e = t->look[i], len = e >> 8, sym = e & 255, s = sym & 15, r = sym >> 4;
        t->fac[i] = 0; t->fdc[i] = 0; t->skp[i] = 0; t->sadv[i] = 0;
        if (!len) continue;
        if (len + s <= 9) {
            int v = s ? (i >> (9 - len - s)) & ((1 << s) - 1) : 0;
            if (s && !(v & (1 << (s - 1)))) v += (-1 << s) + 1;
            if (s && v >= -128 && v <= 127)
                t->fac[i] = (int16_t)(v * 256 + r * 16 + len + s);
            t->fdc[i] = (len + sym) << 8 | sym;   /* DC: sym = s */
            /* skip: total bits, advance; eob flagged by adv 0 */
            t->skp[i] = len + s;
            t->sadv[i] = s ? r + 1 : (r == 15 ? 16 : 0);
        }
    }
    /* multi-symbol skip over 12 bits: as many whole symbols (code +
     * magnitude) as fit, stopping after an EOB */
    for (i = 0; i < 4096; i++) {
        int used = 0, adv = 0, eob = 0;
        for (;;) {
            int rem = 12 - used, idx, e, len, sym, s, r;
            if (rem < 1) break;
            idx = rem >= 9 ? (i >> (rem - 9)) & 511 : (i << (9 - rem)) & 511;
            e = t->look[idx]; len = e >> 8; sym = e & 255; s = sym & 15; r = sym >> 4;
            if (!len || len + s > rem) break;
            used += len + s;
            if (s) adv += r + 1;
            else if (r == 15) adv += 16;
            else { eob = 1; break; }
        }
        t->ms[i] = used | adv << 8 | eob << 16;
    }
}

static int u16(const uint8_t *p) { return p[0] << 8 | p[1]; }

int parse(const uint8_t *d, uint32_t n)
{
    const uint8_t *p = d + 2, *e = d + n;
    while (p + 4 <= e) {
        int m = p[1], len = u16(p + 2), i;
        const uint8_t *q = p + 4;
        if (p[0] != 0xFF) return -1;
        if (m == 0xC0) {
            H = u16(q + 1); W = u16(q + 3); ncomp = q[5];
            for (i = 0; i < ncomp; i++) {
                hs[i] = q[7 + 3 * i] >> 4; vs[i] = q[7 + 3 * i] & 15; tq[i] = q[8 + 3 * i];
            }
        } else if (m == 0xC4) {
            const uint8_t *r = q;
            while (r < p + 2 + len) {
                int tc = r[0] >> 4, th = r[0] & 15, tot = 0;
                htab *t = tc ? &act[th] : &dct[th];
                for (i = 1; i <= 16; i++) tot += (t->bits[i] = r[i]);
                memcpy(t->val, r + 17, tot);
                derive(t);
                r += 17 + tot;
            }
        } else if (m == 0xDB) {
            const uint8_t *r = q;
            while (r < p + 2 + len) {
                int t = r[0] & 15;
                for (i = 0; i < 64; i++) qt[t][zag[i]] = r[1 + i];  /* natural order */
                r += 65;
            }
        } else if (m == 0xDA) {
            for (i = 0; i < q[0]; i++) { td[i] = q[2 + 2 * i] >> 4; ta[i] = q[2 + 2 * i] & 15; }
            ecs = p + 2 + len; ecs_end = e;
            return 0;
        } else if (m == 0xDD) return -2;  /* no restarts in this bench */
        p += 2 + len;
    }
    return -1;
}

/* ---- the reference: jpeg_load.c's reader and block loop ---------------- */
static const uint8_t *rp;
static unsigned long bitbuf; static int bitbuf_bits;

static inline unsigned d_getc(void) { return rp < ecs_end ? *rp++ : 0; }
static void fill_bit_buffer(void)
{
    unsigned b = d_getc();
    if (b == 0xFF) d_getc();          /* stuffing (no RSTm in the bench) */
    bitbuf = (bitbuf << 8) | b;
    b = d_getc();
    if (b == 0xFF) d_getc();
    bitbuf = (bitbuf << 8) | b;
    bitbuf_bits += 16;
}
#define check_bit_buffer(n) do { if ((n) > bitbuf_bits) fill_bit_buffer(); } while (0)
#define get_bits(n) (((int)(bitbuf >> (bitbuf_bits -= (n)))) & ((1 << (n)) - 1))
#define peek_bits(n) (((int)(bitbuf >> (bitbuf_bits - (n)))) & ((1 << (n)) - 1))
#define drop_bits(n) (bitbuf_bits -= (n))
#define HUFF_EXTEND(x, s) ({ int x__ = (x), s__ = (s); x__ & (1 << (s__ - 1)) ? x__ : x__ + (-1 << s__) + 1; })

#define huff_decode_dc(tbl, s, r) { int nb, look; \
    check_bit_buffer(8); look = peek_bits(8); \
    if ((nb = (tbl)->look_nbits[look]) != 0) { drop_bits(nb); s = (tbl)->look_sym[look]; \
        check_bit_buffer(s); r = get_bits(s); \
    } else { long code; nb = 9; check_bit_buffer(nb); code = get_bits(nb); \
        while (code > (tbl)->maxcode[nb]) { code <<= 1; check_bit_buffer(1); code |= get_bits(1); nb++; } \
        if (nb > 16) { r = 0; s = 0; } else { \
            s = (tbl)->val[(tbl)->valptr[nb] + (int)(code - (tbl)->mincode[nb])]; \
            check_bit_buffer(s); r = get_bits(s); } } }
#define huff_decode_ac(tbl, s) { int nb, look; \
    check_bit_buffer(8); look = peek_bits(8); \
    if ((nb = (tbl)->look_nbits[look]) != 0) { drop_bits(nb); s = (tbl)->look_sym[look]; \
    } else { long code; nb = 9; check_bit_buffer(nb); code = get_bits(nb); \
        while (code > (tbl)->maxcode[nb]) { code <<= 1; check_bit_buffer(1); code |= get_bits(1); nb++; } \
        if (nb > 16) s = 0; else s = (tbl)->val[(tbl)->valptr[nb] + (int)(code - (tbl)->mincode[nb])]; } }

static int16_t block[192] __attribute__((aligned(4)));
static int zn[2];
static void zero(int16_t *p, int n) { uint32_t *w = (uint32_t *)p; for (n >>= 2; n; n--) *w++ = 0; }
#define ZERO(ci) zero(block, 4 * zn[!!(ci)] + 4)  /* word-aligned; block[0] set after */
static int last_dc[3];

static uint32_t mix(uint32_t h)
{   /* what the IDCT would read: the kept coefficients */
    int i;
    for (i = 0; i < 4; i++)
        h = h * 31 + (uint16_t)block[zag[i]];
    return h;
}

static int zfor(int kn) { return kn == 0 ? 0 : kn == 4 ? 9 : kn == 24 ? 27 : 63; }
uint32_t dec_ref(int kn_y, int kn_c)
{
    zn[0] = zfor(kn_y); zn[1] = zfor(kn_c);
    int mcux = (W + 8 * hs[0] - 1) / (8 * hs[0]), mcuy = (H + 8 * vs[0] - 1) / (8 * vs[0]);
    int x, y, ci, b; uint32_t h = 0;
    rp = ecs; bitbuf = 0; bitbuf_bits = 0; last_dc[0] = last_dc[1] = last_dc[2] = 0;
    for (y = 0; y < mcuy; y++)
        for (x = 0; x < mcux; x++)
            for (ci = 0; ci < ncomp; ci++)
                for (b = 0; b < hs[ci] * vs[ci]; b++) {
                    htab *dt = &dct[td[ci]], *at = &act[ta[ci]];
                    const int16_t *q = qt[tq[ci]];
                    int kn = ci ? kn_c : kn_y, k = 1, s, r;
                    huff_decode_dc(dt, s, r);
                    s = s ? HUFF_EXTEND(r, s) : 0;
                    last_dc[ci] += s;
                    block[0] = last_dc[ci] * q[0];
                    { int16_t d0 = block[0]; ZERO(ci); block[0] = d0; }
                    for (;;) {
                        huff_decode_ac(at, s);
                        r = s >> 4; s &= 15; k += r;
                        if (s) {
                            check_bit_buffer(s);
                            if (k >= kn) goto skip_rest;
                            r = get_bits(s);
                            r = HUFF_EXTEND(r, s);
                            block[zag[k]] = r * q[zag[k]];
                        } else if (r != 15) goto block_end;
                        if ((++k) & 64) goto block_end;
                    }
                    for (; k < 64; k++) {
                        huff_decode_ac(at, s);
                        r = s >> 4; s &= 15;
                        if (s) { k += r; check_bit_buffer(s);
skip_rest:                  drop_bits(s);
                        } else { if (r != 15) break; k += r; }
                    }
block_end:
                    h = mix(h);
                }
    return h;
}

/* ---- the fast form: same symbols, same coefficients -------------------- */
/* 32-bit buffer, left-aligned, in registers; refilled a byte at a time to
 * >= 25 bits; bytes past the end read as 0 (as d_getc), 0xFF 0x00 as 0xFF */
#define REFILL() do { while (fn <= 24) { unsigned c__ = fp < ee ? *fp++ : 0; \
    if (c__ == 0xFF) fp++; fb |= c__ << (24 - fn); fn += 8; } } while (0)
#define PEEK(n) (fb >> (32 - (n)))
#define DROP(n) (fb <<= (n), fn -= (n))

static int slow_sym(const htab *t, uint32_t fb)
{   /* codes longer than 9 bits: the reference's walk; len << 8 | sym */
    uint32_t code = fb >> 23; int nb = 9;
    while ((int)code > t->maxcode[nb] && nb < 17) { nb++; code = fb >> (32 - nb); }
    if (nb > 16) return 17 << 8;           /* a bad code: the reference eats 17 */
    return nb << 8 | t->val[t->valptr[nb] + (int)(code - t->mincode[nb])];
}

#define GETV(v, s) do { int v__ = (int)PEEK(s); DROP(s); \
    v = v__ & (1 << ((s) - 1)) ? v__ : v__ - (1 << (s)) + 1; } while (0)
#define SYM(t, s) do { int l__ = (t)->look[PEEK(9)]; \
    if (!l__) l__ = slow_sym(t, fb); DROP(l__ >> 8); s = l__ & 255; } while (0)

uint32_t dec_fast(int kn_y, int kn_c)
{
    int mcux = (W + 8 * hs[0] - 1) / (8 * hs[0]), mcuy = (H + 8 * vs[0] - 1) / (8 * vs[0]);
    int x, y, ci, b; uint32_t h = 0;
    uint32_t fb = 0; int fn = 0; const uint8_t *fp = ecs, *ee = ecs_end;
    last_dc[0] = last_dc[1] = last_dc[2] = 0;
    for (y = 0; y < mcuy; y++)
        for (x = 0; x < mcux; x++)
            for (ci = 0; ci < ncomp; ci++)
                for (b = 0; b < hs[ci] * vs[ci]; b++) {
                    const htab *dt = &dct[td[ci]], *at = &act[ta[ci]];
                    const int16_t *q = qt[tq[ci]];
                    int kn = ci ? kn_c : kn_y, k = 1, s, r, e;
                    REFILL();
                    SYM(dt, s);
                    if (s) { if (fn < s) REFILL(); GETV(r, s); } else r = 0;
                    last_dc[ci] += r;
                    ZERO(ci);
                    block[0] = last_dc[ci] * q[0];
                    /* kept ACs */
                    while (k < kn) {
                        REFILL();
                        e = at->fac[PEEK(9)];
                        if (e) {           /* run, value and length at once */
                            int nk = k + ((e >> 4) & 15);
                            if (nk >= kn) break;  /* the skip loop takes it */
                            k = nk;
                            DROP(e & 15);
                            block[zag[k]] = (e >> 8) * q[zag[k]];
                        } else {
                            SYM(at, s);
                            r = s >> 4; s &= 15;
                            if (s) {
                                k += r;
                                if (fn < s) REFILL();
                                if (k >= kn) { DROP(s); goto skip_on; }
                                GETV(r, s);
                                block[zag[k]] = r * q[zag[k]];
                            } else if (r != 15) goto done;
                            else k += 15;
                        }
                        if (++k >= 64) goto done;
                    }
                    if (0) {
skip_on:                if (++k >= 64) goto done;
                    }
                    /* skipped ACs: several symbols a lookup */
                    for (;;) {
                        uint32_t m;
                        REFILL();
                        m = at->ms[PEEK(12)];
                        if ((m & 255) && k + (int)((m >> 8) & 255) < 64) {
                            DROP(m & 255);
                            if (m >> 16) goto done;
                            k += (m >> 8) & 255;
                            continue;
                        }
                        SYM(at, s);
                        r = s >> 4; s &= 15;
                        if (s) { if (fn < s) REFILL(); DROP(s); k += r + 1; }
                        else if (r != 15) goto done;
                        else k += 16;
                        if (k >= 64) goto done;
                    }
done:
                    h = mix(h);
                }
    return h;
}
