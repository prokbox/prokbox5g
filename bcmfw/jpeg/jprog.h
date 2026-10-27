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

/* bcmfw/jpeg/jprog.h: progressive JPEG (SOF2) for jdec, included once by
 * apps/recorder/jpeg_load.c inside its JPEG_JDEC guard, after its Huffman
 * macros (jpeg-spike.md section 9).
 *
 * Every scan is decoded into a store that keeps, per block, only the
 * coefficients the scaled IDCT reads: zig-zag 0 .. K-1, K = max(k_need, 1)
 * for the component's IDCT scale, exactly the ones the baseline path keeps.
 * The other coefficients only need a "nonzero" bit each, for the
 * refinement scans' correction bits; a component that keeps the DC alone
 * (K = 1) needs none, since every one of its AC scans is skipped whole.
 * Then the rows are made from the store by the baseline path's own IDCT,
 * colour and resize, so a progressive file gives the pixels of the
 * baseline file with the same coefficients.  The store is placed at the
 * end of the decoder's buffer; past JPEG_PROG_MAX it is refused (-4, as
 * before), and short of buffer it fails JPEG_E_PROGMEM. */

#ifdef JPEG_FROM_MEM
#error "jprog.h: jpeg_load.c's file mode only (jdec's)"
#endif

#ifndef JDEC_NO_PROGRESSIVE
/* the store of component c's block (bx, by) */
#define PCOEF(pc, bx, by) ((pc)->coef + ((uint32_t)(by) * (pc)->bw + (bx)) * (pc)->k)
#define PHIST(pc, bx, by) ((pc)->hist + ((uint32_t)(by) * (pc)->bw + (bx)) * (pc)->hb)

/* the bit reader in a progressive scan: fill_bit_buffer's, but a marker
   other than RSTn (or the end of the data) stops it, and zeros follow */
static void prog_fill(struct jpeg *p)
{
    int i;

    if (p->marker_val)
        p->marker_ind += 16;
    for (i = 0; i < 2; i++)
    {
        unsigned char byte = 0, marker, *cp;

        if (!p->prog_marker)
        {
            if (!(cp = jpeg_getc(p)))
                p->prog_marker = 0xD9;      /* the end: as an EOI */
            else if ((byte = *cp) == 0xFF)
            {
                do
                    marker = (cp = jpeg_getc(p)) ? *cp : 0xD9;
                while (marker == 0xFF);
                if ((marker & ~7) == 0xD0)
                {
                    p->marker_val = marker;
                    p->marker_ind = i ? 0 : 8;
                }
                else if (marker)
                {
                    p->prog_marker = marker;
                    byte = 0;
                }
            }
        }
        p->bitbuf = (p->bitbuf << 8) | byte;
    }
    p->bitbuf_bits += 16;
}

/* a scan not decoded: on to the next marker */
static void prog_skip(struct jpeg *p)
{
    unsigned char *cp, m;

    while ((cp = jpeg_getc(p)))
    {
        if (*cp != 0xFF)
            continue;
        do
            m = (cp = jpeg_getc(p)) ? *cp : 0xD9;
        while (m == 0xFF);
        if (m && (m & ~7) != 0xD0)
        {
            p->prog_marker = m;
            return;
        }
    }
    p->prog_marker = 0xD9;
}

static inline int prog_nz(const struct jpeg_prog *pc, const int16_t *cf,
                          const uint8_t *hi, int k)
{
    return k < pc->k ? cf[k] != 0 : (hi[(k - pc->k) >> 3] >> ((k - pc->k) & 7)) & 1;
}

static inline void prog_set(const struct jpeg_prog *pc, int16_t *cf,
                            uint8_t *hi, int k, int v)
{
    if (k < pc->k)
        cf[k] = v;
    else
        hi[(k - pc->k) >> 3] |= 1 << ((k - pc->k) & 7);
}

/* one block of one scan (T.81 G.1.2; the refinement as libjpeg's) */
static void prog_unit(struct jpeg *p, int c, int bx, int by, int ss, int se,
                      int ah, int al)
{
    struct jpeg_prog *pc = &p->prog[c];
    int16_t *cf = PCOEF(pc, bx, by);
    uint8_t *hi = PHIST(pc, bx, by);
    int s, r, k;

    if (ss == 0)
    {   /* DC */
        if (!ah)
        {
            struct derived_tbl *dctbl =
                &p->dc_derived_tbls[p->scanheader[c].DC_select];
            huff_decode_dc(p, dctbl, s, r);
            s = s ? HUFF_EXTEND(r, s) : 0;  /* a zero difference: no shift
                                               by -1 (UBSan; the value is 0
                                               either way) */
            p->last_dc_val[c] += s;
            cf[0] = p->last_dc_val[c] * (1 << al);
        }
        else
        {
            check_bit_buffer(p, 1);
            if (get_bits(p, 1))
                cf[0] |= 1 << al;
        }
        return;
    }
    struct derived_tbl *actbl = &p->ac_derived_tbls[p->scanheader[c].AC_select];
    if (!ah)
    {   /* AC, first */
        if (p->eobrun)
        {
            p->eobrun--;
            return;
        }
        for (k = ss; k <= se; k++)
        {
            huff_decode_ac(p, actbl, s);
            r = s >> 4;
            s &= 15;
            if (s)
            {
                k += r;
                check_bit_buffer(p, s);
                r = get_bits(p, s);
                r = HUFF_EXTEND(r, s);
                if (k > se)
                {
                    HUFF_BAD(p);
                    break;
                }
                prog_set(pc, cf, hi, k, r * (1 << al));
            }
            else if (r == 15)
                k += 15;
            else
            {
                p->eobrun = 1 << r;
                if (r)
                {
                    check_bit_buffer(p, r);
                    p->eobrun += get_bits(p, r);
                }
                p->eobrun--;
                break;
            }
        }
        return;
    }
    /* AC, refinement */
    int p1 = 1 << al, m1 = -p1;
    k = ss;
    if (!p->eobrun)
    {
        for (; k <= se; k++)
        {
            huff_decode_ac(p, actbl, s);
            r = s >> 4;
            s &= 15;
            if (s)
            {
                if (s != 1)
                    HUFF_BAD(p);
                check_bit_buffer(p, 1);
                s = get_bits(p, 1) ? p1 : m1;
            }
            else if (r != 15)
            {
                p->eobrun = 1 << r;
                if (r)
                {
                    check_bit_buffer(p, r);
                    p->eobrun += get_bits(p, r);
                }
                break;
            }
            do
            {
                if (prog_nz(pc, cf, hi, k))
                {
                    check_bit_buffer(p, 1);
                    if (get_bits(p, 1) && k < pc->k && !(cf[k] & p1))
                        cf[k] += cf[k] >= 0 ? p1 : m1;
                }
                else if (--r < 0)
                    break;
                k++;
            } while (k <= se);
            if (s)
            {
                if (k > se)
                {
                    HUFF_BAD(p);
                    break;
                }
                prog_set(pc, cf, hi, k, s);
            }
        }
    }
    if (p->eobrun)
    {
        for (; k <= se; k++)
            if (prog_nz(pc, cf, hi, k))
            {
                check_bit_buffer(p, 1);
                if (get_bits(p, 1) && k < pc->k && !(cf[k] & p1))
                    cf[k] += cf[k] >= 0 ? p1 : m1;
            }
        p->eobrun--;
    }
}

/* the restart after a unit, if one is due (none after the scan's last) */
static void prog_restart(struct jpeg *p, int last)
{
    if (p->restart_interval && --p->restart == 0 && !last)
    {
        p->restart = p->restart_interval;
        search_restart(p);
        p->last_dc_val[0] = p->last_dc_val[1] = p->last_dc_val[2] = 0;
        p->eobrun = 0;
    }
}

/* one scan, from its SOS segment's length on: 0, or < 0 */
static int prog_scan(struct jpeg *p, int ncomp)
{
    int len, ns, i, j, c, prev = -1, ss, se, ah, al, sc[3];

    len = e_getc(p, -1) << 8;
    len |= e_getc(p, -1);
    ns = e_getc(p, -1);
    if (ns < 1 || ns > ncomp || len != 6 + 2 * ns)
        return -7;
    for (i = 0; i < ns; i++)
    {
        int id = e_getc(p, -1), t = e_getc(p, -1);

        for (c = prev + 1; c < ncomp && p->frameheader[c].ID != id; c++)
            ;
        if (c == ncomp || (t >> 4) > 1 || (t & 15) > 1)
            return -7;          /* not in the frame, out of order, a table */
        p->scanheader[c].DC_select = t >> 4;
        p->scanheader[c].AC_select = t & 15;
        sc[i] = prev = c;
    }
    ss = e_getc(p, -1);
    se = e_getc(p, -1);
    al = e_getc(p, -1);
    ah = al >> 4;
    al &= 15;
    if ((ss == 0 ? se != 0 : ns != 1 || se < ss || se > 63) || al > 13)
        return -7;
    p->bitbuf = 0;
    p->bitbuf_bits = 0;
    p->marker_val = p->marker_ind = 0;
    p->prog_marker = 0;
    p->eobrun = 0;
    p->last_dc_val[0] = p->last_dc_val[1] = p->last_dc_val[2] = 0;
    p->restart = p->restart_interval;
    if (ss > 0 && p->prog[sc[0]].k == 1)
    {   /* no AC coefficient of this component is used */
        prog_skip(p);
        return 0;
    }
    if (ns > 1)
    {   /* interleaved (DC only): MCUs, each scan component's H x V blocks */
        int mx, my;
        for (my = 0; my < p->y_mbl; my++)
            for (mx = 0; mx < p->x_mbl; mx++)
            {
                for (i = 0; i < ns; i++)
                {
                    int h = p->frameheader[sc[i]].horizontal_sampling;
                    int v = p->frameheader[sc[i]].vertical_sampling;
                    for (j = 0; j < h * v; j++)
                        prog_unit(p, sc[i], mx * h + j % h, my * v + j / h,
                                  ss, se, ah, al);
                }
                prog_restart(p, my == p->y_mbl - 1 && mx == p->x_mbl - 1);
            }
    }
    else
    {   /* one component: its own blocks in raster order (T.81 A.2.2) */
        int hmax = p->frameheader[0].horizontal_sampling;
        int vmax = p->frameheader[0].vertical_sampling;
        int h = p->frameheader[sc[0]].horizontal_sampling;
        int v = p->frameheader[sc[0]].vertical_sampling;
        int cw = ((p->x_size * h + hmax - 1) / hmax + 7) / 8;
        int ch = ((p->y_size * v + vmax - 1) / vmax + 7) / 8;
        int bx, by;
        for (by = 0; by < ch; by++)
            for (bx = 0; bx < cw; bx++)
            {
                prog_unit(p, sc[0], bx, by, ss, se, ah, al);
                prog_restart(p, by == ch - 1 && bx == cw - 1);
            }
    }
    return 0;
}

/* Every scan into the store, from just after the first SOS marker; the
 * store at the end of buf (*size bytes, 'keep' of them left for the
 * resizer).  0, or < 0. */
static int prog_decode(struct jpeg *p, unsigned char *buf, int *size, int keep)
{
    int ncomp = p->blocks > 1 ? 3 : 1, c, st;
    uint32_t bytes = 0;
    unsigned char *at;

    for (c = 0; c < ncomp; c++)
    {
        struct jpeg_prog *pc = &p->prog[c];
        pc->bw = p->x_mbl * p->frameheader[c].horizontal_sampling;
        pc->bh = p->y_mbl * p->frameheader[c].vertical_sampling;
        pc->k = MAX(p->k_need[SCALE_IDX(c)], 1);
        pc->hb = pc->k == 1 ? 0 : (64 - pc->k + 7) / 8;
        /* each component's part 8-aligned: its coefficients are int16 */
        bytes += ((uint32_t)pc->bw * pc->bh * (2 * pc->k + pc->hb) + 7) & ~7u;
    }
    if (bytes > JPEG_PROG_MAX)
        return -4;              /* past the shared cap: not supported */
    if (*size < keep + 8 || bytes > (uint32_t)(*size - keep - 8))
        return JPEG_E_PROGMEM;
    at = (unsigned char *)ALIGN_DOWN((uintptr_t)(buf + *size - bytes), 8);
    *size = at - buf;
    memset(at, 0, bytes);
    for (c = 0; c < ncomp; c++)
    {   /* the coefficients (int16), then the nonzero bits */
        struct jpeg_prog *pc = &p->prog[c];
        uint32_t nb = (uint32_t)pc->bw * pc->bh;
        pc->coef = (int16_t *)at;
        at += nb * 2 * pc->k;
        pc->hist = at;
        at += (nb * pc->hb + nb * 2 * pc->k + 7) / 8 * 8 - nb * 2 * pc->k;
    }
    for (;;)
    {
        if ((st = prog_scan(p, ncomp)) < 0)
            return st;
        st = process_markers(p);
        if (st < 0)
        {   /* the data ends between scans: decode what came (jdec finds
               no EOI and says so) */
            if (p->len == 0 && p->buf_left <= 0)
                break;
            return st;
        }
        if (st & DQT)
            return -4;          /* tables redefined between scans */
        if (st & DHT)
            fix_huff_tables(p);
        if (st & PROG_EOI)
            break;
    }
    p->prog_mcu_y = 0;
    p->restart_interval = 0;    /* the rows come from the store */
    return 0;
}

#else /* JDEC_NO_PROGRESSIVE */
/* A build without the store (the BCM's display-only image: no room for
 * it and no SDRAM): a progressive file is refused as past the cap is, so
 * jdec says UNSUPPORTED and the caller's CPU path (the same jdec, with
 * the store) makes the same pixels. */
static void prog_fill(struct jpeg *p)
{
    (void)p;                    /* never reached: prog_decode refuses */
}

static int prog_decode(struct jpeg *p, unsigned char *buf, int *size, int keep)
{
    (void)p; (void)buf; (void)size; (void)keep;
    return -4;
}
#endif /* JDEC_NO_PROGRESSIVE */

/* store_row_jpeg's block from the store: what the baseline path puts in
   'block' for the same coefficients */
#ifndef JDEC_NO_PROGRESSIVE
static void prog_block(struct jpeg *p, int ci, int x, int blkn,
                       int16_t *block, const int16_t *qt, bool transpose)
{
    const struct jpeg_prog *pc = &p->prog[ci];
    int h = p->frameheader[ci].horizontal_sampling;
    int i = p->mcu_pos[blkn], k;
    const int16_t *cf = PCOEF(pc, x * h + i % h,
                              p->prog_mcu_y * p->frameheader[ci].vertical_sampling + i / h);

    (void)transpose;
    block[0] = MULTIPLY16(cf[0], qt[0]);
    MEMSET(block+1, 0, p->zero_need[SCALE_IDX(ci)] * sizeof(int));
    for (k = 1; k < pc->k; k++)
        if (cf[k])
#ifdef JPEG_IDCT_TRANSPOSE
            block[zag[transpose ? k : k + 64]] = MULTIPLY16(cf[k], qt[k]);
#else
            block[zag[k]] = MULTIPLY16(cf[k], qt[k]);
#endif
}
#else
static void prog_block(struct jpeg *p, int ci, int x, int blkn,
                       int16_t *block, const int16_t *qt, bool transpose)
{
    (void)p; (void)ci; (void)x; (void)blkn; (void)block; (void)qt; (void)transpose;
}
#endif
