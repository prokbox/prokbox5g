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

/* The visualiser's FFT kernel on a model of the BCM2722's vector unit
 * (vfft.h; docs/design/viz-fft-vc.md section 5).
 *
 * This file is the kernel's program written as C calls, one call per
 * vector instruction, against a small model of the vector register file
 * (VRF).  It exists so the layout can be proved before any assembly is
 * generated: the tests require its output to equal vfft_ref.c's on every
 * bit, and every call names its instruction's form key (the key
 * tools/bcmre/proven_forms.txt files it under), so the tests can show
 * which forms silicon has already run.  A generator (gen_viz.py's
 * successor) emits these same instructions in this same order.
 *
 * The model, from docs/bcm2722-reference/isa-vector.md:
 *   - the VRF is 64 rows of 64 bytes.  An H16 vector at (y, x) is the
 *     16 halfwords of row y at x 0 (bytes 0-31) or x 32 (bytes 32-63);
 *     here v[y][x / 32][lane].  A V16 vector at (y, x) is column x of rows
 *     y .. y + 15: element i is row y + i's halfword in lane x & 15 of half
 *     x / 32.  H rows and V columns alias the same bytes, which is what
 *     makes the transpose one instruction [apple; silicon: bench GTHR];
 *   - REP n runs the instruction n times, every vector operand stepping a
 *     row (H) or a column (V) per repetition;
 *   - each lane has a 32-bit accumulator.  clra clears it; acc adds the
 *     result at bit 16 and writes the high half; x15 adds the result's low
 *     16 bits, zero-extended, at bit 0 and writes the low half.  Silicon's
 *     accumulator saturates at +-2^31 (sat32, quirks #31, #32);
 *   - setf sets each enabled lane's N flag from the result; ifn runs only
 *     the lanes whose N is set;
 *   - vmulhd is (a b) >> 16, floored; adds and subtracts wrap to 16 bits.
 *
 * VRF plan (blocks of 16 rows; real parts at x 0, imaginary at x 32):
 *   B0 rows 0-15, B1 16-31, B2 32-47, B3 48-63.  Section 5 of the design
 *   doc has the phase-by-phase table of what each block holds. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "vfft.h"

#define B0      0
#define B1      16
#define B2      32
#define B3      48
#define RE      0               /* x of the real parts */
#define IM      32              /* x of the imaginary parts */

enum op { VADD, VSUB, VRSUB, VASR, VSHL, VMUL, VMULHD, VMOV };
static const char *const op_name[] = {
    "vadd", "vsub", "vrsub", "vasr", "vshl", "vmul", "vmulhd", "vmov"
};

#define M_CLRA  0x1
#define M_ACC   0x2
#define M_X15   0x4
#define M_SETF  0x8
#define M_IFN   0x10

enum kind { K_H16, K_V16, K_IMM, K_REG, K_DISC };
struct opd
{
    enum kind k;
    int y, x;                   /* H16: row, x 0/32; V16: first row, column */
    int32_t v;                  /* an immediate's or a scalar register's value */
};

static struct opd H(int y, int x) { struct opd o = { K_H16, y, x, 0 }; return o; }
static struct opd V(int y, int x) { struct opd o = { K_V16, y, x, 0 }; return o; }
static struct opd IMM(int32_t v) { struct opd o = { K_IMM, 0, 0, v }; return o; }
static struct opd REG(int32_t v) { struct opd o = { K_REG, 0, 0, v }; return o; }
static const struct opd DISC = { K_DISC, 0, 0, 0 };

struct vpu
{
    int16_t v[64][2][16];       /* the VRF: row, half (x 0, x 32), lane */
    int64_t acc[16];            /* per lane, kept within 32 bits (sat32) */
    uint8_t n[16];              /* per lane N flag */
    int phase;
    struct vfft_stats *st;
    vfft_form_fn forms;
    void *arg;
};

/* ---- the model -------------------------------------------------------- */

/* where element i of operand o sits at repetition k */
static int16_t *cell(struct vpu *p, struct opd o, int k, int i)
{
    int y, x;

    if (o.k == K_H16)
    {
        assert(o.x == 0 || o.x == 32);
        return &p->v[(o.y + k) & 63][o.x / 32][i];
    }
    assert(o.k == K_V16);
    x = o.x + k;                        /* a V16 operand steps a column */
    y = o.y + i;
    assert((o.y & 15) == 0);            /* a vertical operand's y is a
                                           multiple of 16 (quirks #39) */
    assert((x >= 0 && x < 16) || (x >= 32 && x < 48));
    return &p->v[y & 63][x / 32][x & 15];
}

static int32_t rd(struct vpu *p, struct opd o, int k, int i)
{
    if (o.k == K_IMM || o.k == K_REG)
        return o.v;
    return *cell(p, o, k, i);
}

static int64_t sat32(int64_t s)
{
    return s > INT32_MAX ? INT32_MAX : s < INT32_MIN ? INT32_MIN : s;
}

static void count(struct vpu *p, uint32_t rep, uint32_t bytes)
{
    if (p->st)
    {
        p->st->insns[p->phase]++;
        p->st->rows[p->phase] += rep;
        p->st->bytes[p->phase] += bytes;
    }
}

static void scalar(struct vpu *p, uint32_t n)
{
    if (p->st)
        p->st->scalar[p->phase] += n;
}

static const char *opd_key(char *b, char role, enum op op, struct opd o, int rep)
{
    const char *step = rep > 1 ? "++" : "";

    if (role == 'A' && op == VMOV)
        return "AH8";                   /* vmov's A is unused: H8 in the
                                           encoding, never stepped */
    switch (o.k)
    {
    case K_H16:  sprintf(b, "%cH16%s", role, step); break;
    case K_V16:  sprintf(b, "%cV16%s", role, step); break;
    case K_IMM:  sprintf(b, "%cimm", role); break;
    case K_REG:  sprintf(b, "%creg", role); break;
    case K_DISC: sprintf(b, "%cdiscard", role); break;
    }
    return b;
}

/* One ALU instruction (80-bit), every repetition and lane as the hardware
 * runs them: lanes from the predicate; read A and B; compute; clra; acc or
 * x15; setf; write D (16 bits) */
static void alu(struct vpu *p, enum op op, struct opd d, struct opd a,
                struct opd b, int rep, int mods)
{
    char key[96], kd[16], ka[16], kb[16], m[48] = "";
    int k, i;
    int64_t r, out;

    for (k = 0; k < rep; k++)
        for (i = 0; i < 16; i++)
        {
            int32_t av, bv;

            if ((mods & M_IFN) && !p->n[i])
                continue;
            av = op == VMOV ? 0 : rd(p, a, k, i);
            bv = rd(p, b, k, i);
            switch (op)
            {
            case VADD:   r = (int64_t)av + bv; break;
            case VSUB:   r = (int64_t)av - bv; break;
            case VRSUB:  r = (int64_t)bv - av; break;
            case VASR:   r = av >> bv; break;
            case VSHL:   r = (int64_t)av << bv; break;
            case VMUL:   r = (int64_t)av * bv; break;
            case VMULHD: r = ((int64_t)av * bv) >> 16; break;
            default:     r = bv; break;
            }
            if (mods & M_CLRA)
                p->acc[i] = 0;
            out = r;
            if ((mods & M_ACC) && !(mods & M_X15))
            {
                p->acc[i] = sat32(p->acc[i] + (int64_t)(int16_t)r * 65536);
                out = p->acc[i] >> 16;
            }
            else if ((mods & M_X15) && !(mods & M_ACC))
            {
                p->acc[i] = sat32(p->acc[i] + (uint16_t)r);
                out = (uint16_t)p->acc[i];
            }
            else
                assert(!(mods & (M_ACC | M_X15)));  /* the signed acc x15
                                                       form: not used */
            if (mods & M_SETF)
                p->n[i] = r < 0;
            if (d.k != K_DISC)
                *cell(p, d, k, i) = (int16_t)(uint16_t)out;
        }
    count(p, (uint32_t)rep, 10);
    if (p->forms)
    {
        if (mods & M_CLRA) strcat(m, " clra");
        if (mods & M_ACC)  strcat(m, " acc");
        if (mods & M_X15)  strcat(m, " x15");
        if (mods & M_SETF) strcat(m, " setf");
        if (mods & M_IFN)  strcat(m, " ifn");
        if (rep > 1)       strcat(m, " rep");
        sprintf(key, "%s %s %s %s %s 80", op_name[op], opd_key(kd, 'D', op, d, rep),
                opd_key(ka, 'A', op, a, rep), opd_key(kb, 'B', op, b, rep),
                m[0] ? m + 1 : "-");
        p->forms(key, (uint32_t)rep, p->arg);
    }
}

/* vld16 (48-bit): rep rows from mem, stride halfwords apart, into H16
 * rows y .. at x.  The address must be 32-byte aligned (quirks #17a, #38:
 * silicon rounds a misaligned one down) */
static void vld16(struct vpu *p, int y, int x, const int16_t *mem, int rep,
                  int stride)
{
    int k;

    assert(((uintptr_t)mem & 31) == 0 && (stride & 15) == 0);
    for (k = 0; k < rep; k++)
        memcpy(p->v[(y + k) & 63][x / 32], mem + k * stride, 32);
    count(p, (uint32_t)rep, 6);
    if (p->forms)
        p->forms(rep > 1 ? "mem48 vld16 rep stride nooff" :
                 "mem48 vld16 single nostride nooff", (uint32_t)rep, p->arg);
}

/* vst16 (48-bit): rows 0 .. rep - 1 at x 0 to mem (the store's source row
 * field addresses block 0's rows: isa-vector.md "Stores"; silicon ran REP
 * 16 from rows 0-15) */
static void vst16(struct vpu *p, uint16_t *mem, int rep, int stride)
{
    int k;

    assert(((uintptr_t)mem & 31) == 0);
    for (k = 0; k < rep; k++)
        memcpy(mem + k * stride, p->v[k][0], 32);
    count(p, (uint32_t)rep, 6);
    if (p->forms)
        p->forms("mem48 vst16 rep stride nooff", (uint32_t)rep, p->arg);
}

static void phase(struct vpu *p, int ph, const char *name)
{
    p->phase = ph;
    if (p->st)
        p->st->name[ph] = name;
}

/* ---- the program ------------------------------------------------------ */

static int brev4(int i)
{
    return (i & 1) << 3 | (i & 2) << 1 | (i & 4) >> 1 | (i & 8) >> 3;
}

/* d = a W / 2 by the immediates (wr, wi) = W_16^e, one component: four
 * instructions, the exact sum a_r w1 + a_i w2 floored once.  Low halves
 * first (x15), then high halves (acc): the last writes the result */
static void chain_imm(struct vpu *p, struct opd d, struct opd a1, int32_t w1,
                      struct opd a2, int32_t w2)
{
    alu(p, VMUL, DISC, a1, IMM(w1), 1, M_CLRA | M_X15);
    alu(p, VMUL, DISC, a2, IMM(w2), 1, M_X15);
    alu(p, VMULHD, DISC, a1, IMM(w1), 1, M_ACC);
    alu(p, VMULHD, d, a2, IMM(w2), 1, M_ACC);
}

/* The same with vector B operands (VFFT_EXACT's step 2 and split: the
 * forms that need a bench row) */
static void chain_vec(struct vpu *p, struct opd d, struct opd a1, struct opd b1,
                      struct opd a2, struct opd b2, int seed_clra)
{
    alu(p, VMUL, DISC, a1, b1, 1, (seed_clra ? M_CLRA : 0) | M_X15);
    alu(p, VMUL, DISC, a2, b2, 1, M_X15);
    alu(p, VMULHD, DISC, a1, b1, 1, M_ACC);
    alu(p, VMULHD, d, a2, b2, 1, M_ACC);
}

/* A 16-point radix-2 DIF down the rows of block x, all 16 lanes at once
 * (16 independent transforms); d's temporaries in block tmp (h rows at a
 * time); the last stage into block y's rows in bit-reversed order (y ends
 * in natural order), then halved if half.  Steps 1 and 3 both run it */
static void dif16(struct vpu *p, const struct vfft_tabs *t, int x, int tmp,
                  int y, int half, int mut_brev)
{
    int h, g, j, c, e, i, a0, b0, ri, rj;

    for (h = 8; h >= 2; h >>= 1)
        for (g = 0; g < 16; g += 2 * h)
        {
            a0 = x + g;
            b0 = x + g + h;
            /* whole-row butterflies, REP h: d = a - b; a = (a + b) / 2 */
            for (c = RE; c <= IM; c += 32)
            {
                alu(p, VSUB, H(tmp, c), H(a0, c), H(b0, c), h, 0);
                alu(p, VADD, H(a0, c), H(a0, c), H(b0, c), h, 0);
                alu(p, VASR, H(a0, c), H(a0, c), IMM(1), h, 0);
            }
            /* b = d W_16^e / 2, one row at a time (each row its own e) */
            for (j = 0; j < h; j++)
            {
                struct opd dr = H(tmp + j, RE), di = H(tmp + j, IM);
                int b = b0 + j;
                int32_t wr, wi;

                e = j * (8 / h);
                if (e == 0)
                {
                    alu(p, VASR, H(b, RE), dr, IMM(1), 1, 0);
                    alu(p, VASR, H(b, IM), di, IMM(1), 1, 0);
                }
                else if (e == 4)            /* W = -i: (di, -dr) / 2 */
                {
                    alu(p, VASR, H(b, RE), di, IMM(1), 1, 0);
                    alu(p, VRSUB, H(b, IM), dr, IMM(0), 1, 0);
                    alu(p, VASR, H(b, IM), H(b, IM), IMM(1), 1, 0);
                }
                else
                {
                    wr = t->w16[2 * e];
                    wi = t->w16[2 * e + 1];
                    chain_imm(p, H(b, RE), dr, wr, di, -wi);
                    chain_imm(p, H(b, IM), dr, wi, di, wr);
                }
            }
        }
    /* the last stage (h = 1) into natural order */
    for (i = 0; i < 16; i += 2)
    {
        ri = brev4(i);
        rj = brev4(i + 1);
        if (mut_brev && (ri == 1 || ri == 2))
            ri ^= 3;
        for (c = RE; c <= IM; c += 32)
        {
            alu(p, VADD, H(y + ri, c), H(x + i, c), H(x + i + 1, c), 1, 0);
            alu(p, VSUB, H(y + rj, c), H(x + i, c), H(x + i + 1, c), 1, 0);
        }
    }
    if (half)
        for (c = RE; c <= IM; c += 32)
            alu(p, VASR, H(y, c), H(y, c), IMM(1), 16, 0);
}

/* lane 0's N flag, for the partner's lane-0 fix */
static const int16_t lane0_neg[16] __attribute__((aligned(32))) = { -1 };

void vfft_vpu(uint16_t *raw, const int16_t *pcm, const struct vfft_tabs *t,
              enum vfft_mode mode, enum vfft_mutation mut,
              struct vfft_stats *st, vfft_form_fn forms, void *arg)
{
    static struct vpu vp;
    struct vpu *p = &vp;
    int r, b, a, c, lo;

    memset(p->v, 0x5A, sizeof(p->v));   /* the VRF holds whatever ran before */
    memset(p->acc, 0, sizeof(p->acc));
    memset(p->n, 0, sizeof(p->n));
    p->st = st;
    p->forms = forms;
    p->arg = arg;
    if (st)
        memset(st, 0, sizeof(*st));

    /* 0. Load and window: B0 = the samples (evens real, odds imaginary),
       B1 = the window; B0 = (B0 B1) >> 16 a row at a time */
    phase(p, 0, "load, window");
    scalar(p, 5);                       /* win address, stride, two +512 B,
                                           the call */
    vld16(p, B0, RE, pcm, 16, 16);
    vld16(p, B0, IM, pcm + VFFT_M, 16, 16);
    vld16(p, B1, RE, t->win, 16, 16);
    vld16(p, B1, IM, t->win + VFFT_M, 16, 16);
    for (r = 0; r < 16; r++)
        for (c = RE; c <= IM; c += 32)
            alu(p, VMULHD, H(B0 + r, c), H(B0 + r, c), H(B1 + r, c), 1, 0);

    /* 1. Step 1: B0 down the rows, d in B3, the result in B1 (natural:
       row k2, lane n2), its last halving left to step 2 */
    phase(p, 1, "step 1");
    dif16(p, t, B0, B3, B1, 0, 0);

    /* 2. Step 2: B1 times the twiddles (B2) into B0 */
    phase(p, 2, "step 2");
    scalar(p, 2);
    vld16(p, B2, RE, t->tw, 16, 16);
    vld16(p, B2, IM, t->tw + VFFT_M, 16, 16);
    if (mode == VFFT_SEP)
    {
        /* four products a row, each floored (vmulhd), then two whole-block
           sums: zr = yr wr - yi wi, zi = yr wi + yi wr */
        for (r = 0; r < 16; r++)
        {
            alu(p, VMULHD, H(B0 + r, RE), H(B1 + r, RE), H(B2 + r, RE), 1, 0);
            alu(p, VMULHD, H(B0 + r, IM), H(B1 + r, IM), H(B2 + r, IM), 1, 0);
            alu(p, VMULHD, H(B3 + r, RE), H(B1 + r, RE), H(B2 + r, IM), 1, 0);
            alu(p, VMULHD, H(B3 + r, IM), H(B1 + r, IM), H(B2 + r, RE), 1, 0);
        }
        if (mut == VFFT_MUT_TWSIGN)
        {
            alu(p, VADD, H(B0, RE), H(B0, RE), H(B0, IM), 16, 0);
            alu(p, VSUB, H(B0, IM), H(B3, IM), H(B3, RE), 16, 0);
        }
        else
        {
            alu(p, VSUB, H(B0, RE), H(B0, RE), H(B0, IM), 16, 0);
            alu(p, VADD, H(B0, IM), H(B3, RE), H(B3, IM), 16, 0);
        }
    }
    else
    {
        /* -wi in B3, then per row two exact chains with vector B */
        scalar(p, 1);                   /* a register holding 0 */
        alu(p, VRSUB, H(B3, RE), H(B2, IM), REG(0), 16, 0);
        for (r = 0; r < 16; r++)
        {
            chain_vec(p, H(B0 + r, RE), H(B1 + r, RE), H(B2 + r, RE),
                      H(B1 + r, IM), H(B3 + r, RE), 1);
            chain_vec(p, H(B0 + r, IM), H(B1 + r, RE), H(B2 + r, IM),
                      H(B1 + r, IM), H(B2 + r, RE), 1);
        }
    }

    /* 3. The transpose: B1's row n2 = B0's column n2 (lane k2 = B0's row);
       one REP 16 vmov a component, H16 rows from V16 columns */
    phase(p, 3, "transpose");
    alu(p, VMOV, H(B1, RE), DISC, V(B0, 0), 16, 0);
    alu(p, VMOV, H(B1, IM), DISC, V(B0, 32), 16, 0);

    /* 4. Step 3: B1 down the rows, d in B3, halved into B0: Z[k] at row
       k >> 4, lane k & 15 */
    phase(p, 4, "step 3");
    dif16(p, t, B1, B3, B0, 1, mut == VFFT_MUT_BREV);

    /* 5. The partner block P[a][b] = Z[(256 - 16 a - b) & 255], in B1:
       lane b's partner is lane 16 - b (a lane reversal, which the VRF does
       by transposing, reversing rows, transposing back), row 15 - a; lane
       0's is row (16 - a) & 15, one row further: a predicated fix */
    phase(p, 5, "partner");
    alu(p, VMOV, H(B2, RE), DISC, V(B0, 0), 16, 0);     /* T = Z^t */
    alu(p, VMOV, H(B2, IM), DISC, V(B0, 32), 16, 0);
    for (b = 0; b < 16; b++)                            /* T2[b] = T[-b] */
        for (c = RE; c <= IM; c += 32)
            alu(p, VADD, H(B3 + b, c), H(B2 + ((16 - b) & 15), c), IMM(0), 1, 0);
    alu(p, VMOV, H(B1, RE), DISC, V(B3, 0), 16, 0);     /* LR = T2^t: */
    alu(p, VMOV, H(B1, IM), DISC, V(B3, 32), 16, 0);    /* LR[a][b] = Z[a][-b] */
    for (c = RE; c <= IM; c += 32)
    {
        /* rows 33 + a = LR[15 - a]; row 32 = LR[0] (= row 48), so rows
           32 + a read one row back: lane 0's partner */
        alu(p, VADD, H(32, c), H(B1, c), IMM(0), 1, 0);
        for (a = 0; a < 16; a++)
            alu(p, VADD, H(33 + a, c), H(B1 + 15 - a, c), IMM(0), 1, 0);
        alu(p, VADD, H(B1, c), H(33, c), IMM(0), 16, 0);
    }
    scalar(p, 2);                       /* the table's address, a zero reg */
    vld16(p, 63, RE, lane0_neg, 1, 16);
    alu(p, VSUB, DISC, H(63, RE), REG(0), 1, M_SETF);  /* N: lane 0 only */
    if (mut != VFFT_MUT_LANE0)
        for (c = RE; c <= IM; c += 32)
            alu(p, VADD, H(B1, c), H(32, c), IMM(0), 16, M_IFN);

    /* 6. The split: Z (B0) and P (B1) with W_512^k (B2) to X */
    phase(p, 6, "split");
    scalar(p, 2);
    vld16(p, B2, RE, t->ws, 16, 16);
    vld16(p, B2, IM, t->ws + VFFT_M, 16, 16);
    alu(p, VADD, H(B3, RE), H(B0, RE), H(B1, RE), 16, 0);      /* S */
    alu(p, VSUB, H(B3, IM), H(B0, RE), H(B1, RE), 16, 0);      /* D */
    alu(p, VADD, H(B1, RE), H(B0, IM), H(B1, IM), 16, 0);      /* T */
    alu(p, VSUB, H(B1, IM), H(B0, IM), H(B1, IM), 16, 0);      /* U */
    if (mode == VFFT_SEP)
    {
        /* Xr = S/2 + T Wr + D Wi, Xi = U/2 + T Wi - D Wr, each product
           floored (vmulhd) and the sums wrapping 16-bit adds */
        alu(p, VASR, H(B3, RE), H(B3, RE), IMM(1), 16, 0);
        alu(p, VASR, H(B1, IM), H(B1, IM), IMM(1), 16, 0);
        for (r = 0; r < 16; r++)
        {
            alu(p, VMULHD, H(B0 + r, RE), H(B1 + r, RE), H(B2 + r, RE), 1, 0);
            alu(p, VMULHD, H(B0 + r, IM), H(B3 + r, IM), H(B2 + r, IM), 1, 0);
        }
        alu(p, VADD, H(B3, RE), H(B3, RE), H(B0, RE), 16, 0);
        alu(p, VADD, H(B3, RE), H(B3, RE), H(B0, IM), 16, 0);  /* Xr */
        for (r = 0; r < 16; r++)
        {
            alu(p, VMULHD, H(B0 + r, RE), H(B1 + r, RE), H(B2 + r, IM), 1, 0);
            alu(p, VMULHD, H(B0 + r, IM), H(B3 + r, IM), H(B2 + r, RE), 1, 0);
        }
        alu(p, VADD, H(B1, IM), H(B1, IM), H(B0, RE), 16, 0);
        alu(p, VSUB, H(B1, IM), H(B1, IM), H(B0, IM), 16, 0);  /* Xi */
    }
    else
    {
        /* per row, S 2^15 seeded as vshl #15 x15 + vasr #1 acc, then two
           exact products: Xi first (it reads -Wr, which Xr's row then
           takes over), Xr into B0 x 0, Xi into B0 x 32 */
        scalar(p, 1);
        alu(p, VRSUB, H(B0, RE), H(B2, RE), REG(0), 16, 0);    /* -Wr */
        for (r = 0; r < 16; r++)
        {
            alu(p, VSHL, DISC, H(B1 + r, IM), IMM(15), 1, M_CLRA | M_X15);
            alu(p, VASR, DISC, H(B1 + r, IM), IMM(1), 1, M_ACC);
            chain_vec(p, H(B0 + r, IM), H(B1 + r, RE), H(B2 + r, IM),
                      H(B3 + r, IM), H(B0 + r, RE), 0);
            alu(p, VSHL, DISC, H(B3 + r, RE), IMM(15), 1, M_CLRA | M_X15);
            alu(p, VASR, DISC, H(B3 + r, RE), IMM(1), 1, M_ACC);
            chain_vec(p, H(B0 + r, RE), H(B1 + r, RE), H(B2 + r, RE),
                      H(B3 + r, IM), H(B2 + r, IM), 0);
        }
    }

    /* 7. The power |X|^2, exact: the products' low halves by vmul (REP
       16), high halves by vmulhd (a row at a time), summed per row in the
       accumulator by vmov (four adds), read back as low and high halves */
    phase(p, 7, "power");
    {
        struct opd xr = mode == VFFT_SEP ? H(B3, RE) : H(B0, RE);
        struct opd xi = mode == VFFT_SEP ? H(B1, IM) : H(B0, IM);

        lo = mode == VFFT_SEP ? B0 : B1;
        alu(p, VMUL, H(lo, RE), xr, xr, 16, 0);
        alu(p, VMUL, H(lo, IM), xi, xi, 16, 0);
        for (r = 0; r < 16; r++)
        {
            struct opd xrr = H(xr.y + r, xr.x), xir = H(xi.y + r, xi.x);

            alu(p, VMULHD, H(B2 + r, RE), xrr, xrr, 1, 0);
            alu(p, VMULHD, H(B2 + r, IM), xir, xir, 1, 0);
        }
        for (r = 0; r < 16; r++)
        {
            alu(p, VMOV, DISC, DISC, H(B2 + r, RE), 1, M_CLRA | M_ACC);
            alu(p, VMOV, DISC, DISC, H(lo + r, RE), 1, M_X15);
            alu(p, VMOV, DISC, DISC, H(lo + r, IM), 1, M_X15);
            alu(p, VMOV, DISC, DISC, H(B2 + r, IM), 1, M_ACC);
            alu(p, VMOV, H(lo + r, RE), DISC, IMM(0), 1, M_X15);  /* low */
            alu(p, VMOV, H(lo + r, IM), DISC, IMM(0), 1, M_ACC);  /* high */
        }
    }

    /* 8. The stores, from B0's rows at x 0 (the store form silicon ran):
       the low halves, then the high halves moved there */
    phase(p, 8, "store");
    scalar(p, 3);                       /* raw + 512 B, the return */
    if (lo != B0)
        alu(p, VADD, H(B0, RE), H(lo, RE), IMM(0), 16, 0);
    vst16(p, raw, 16, 16);
    alu(p, VADD, H(B0, RE), H(lo, IM), IMM(0), 16, 0);
    vst16(p, raw + VFFT_M, 16, 16);
}
