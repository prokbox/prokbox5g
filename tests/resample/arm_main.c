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

/* Bare-metal ARM harness for the half-band stages (P19 19-13), run under
 * qemu-system-arm with semihosting by tests/resample/run.sh.
 *
 * WL=0: bit-exactness. Every coefficient set, both directions, runs through
 * the ARM code and the C reference in the same irregular block sizes
 * (1..97 samples, so every call boundary and state hand-over is crossed);
 * outputs and states must match exactly. Prints "ARM==C ok <checksum>".
 * WL=1..6: one kernel over NS samples (ARM), for the instruction and cycle
 * counts; WL=101..106: the same with the C reference compiled for ARM.
 * WL=99: baseline (setup only).
 * WL=7..15 (with HERMITE=1, linked against a firmware build's dsp_arm.o):
 * the upstream resample_hermite() over NS mono inputs at the step in
 * hermite_delta[], printing "OUT <hex count>" for the per-output figure. */
#include <stdint.h>
#include "resample_hb.h"
#include "resample_hb_coefs.h"

#define NS 4032                 /* 42 blocks of 96 */

static void __attribute__((unused)) sh_write0(const char *s)
{
    register int r0 __asm__("r0") = 4;
    register const char *r1 __asm__("r1") = s;
    __asm__ volatile("svc 0x123456" : "+r"(r0) : "r"(r1) : "memory");
}

static void __attribute__((unused)) put_hex(char *p, uint32_t v)
{
    for (int i = 7; i >= 0; i--, v <<= 4)
        p[7 - i] = "0123456789abcdef"[v >> 28];
}

static int32_t in[2 * NS];
static int32_t __attribute__((unused)) out_a[2 * NS];
static int32_t __attribute__((unused)) out_c[2 * NS];
static uint32_t rng = 12345;

static int32_t noise(void)
{
    rng = rng * 1664525u + 1013904223u;
    return (int32_t)rng >> 4;           /* about +/-1.0 in Q27 */
}

__attribute__((unused)) static const struct hb_coefs *const sets[3] = { &hb_t44, &hb_t48, &hb_r };

#if WL % 100 >= 20 && WL % 100 <= 23
#include "resample_fir.h"
#if WL > 100
#define FIR_ROW   rs_fir_row_c
#define FIR_BATCH rs_fir_batch_c
#else
#define FIR_ROW   rs_fir_row
#define FIR_BATCH rs_fir_batch
#endif
/* FIR workloads: NS outputs in batches of 16 at the step of 44.1 -> 48 kHz
 * after the x2 (88.2 -> 48: 147/80), every row interpolated (WL 21: the
 * uncached path, row + one channel; WL 22: row only; WL 23: dot only, rows
 * cached as for x3, x6 and 3:2). */
static int32_t rows[16][RS_FIR_TAPS];
static uintptr_t tags[16];
#endif

#if defined(HERMITE) && WL >= 7 && WL <= 15
/* The leading fields resample_hermite() uses (resample.c, dsp_core.h) */
struct rs_data { uint32_t delta, phase; int32_t history[2][3]; };
struct dsp_buf { int32_t remcount; int32_t *p32[2]; int32_t bufcount;
                 uint8_t format[12]; };
int resample_hermite(struct rs_data *data, struct dsp_buf *src,
                     struct dsp_buf *dst);
/* x2, /2, 4:3 (x3 path), 2:3 (x6 path; plain 32 -> 48), 8:3, x4, /4,
 * x3, x6 */
static const uint32_t hermite_delta[9] =
    { 0x8000, 0x20000, 0x15555, 0xaaaa, 0x2aaaa, 0x4000, 0x40000,
      0x5555, 0x2aaa };
#endif

int main(void)
{
    for (int i = 0; i < 2 * NS; i++)
        in[i] = (i & 1024) ? noise() : noise() >> 9;  /* loud and quiet */
    /* keep the init in the WL=99 baseline too, so it cancels out */
    __asm__ volatile("" : : "r"(in) : "memory");
#if WL == 0
    uint32_t sum = 0;
    int bad = 0;
    for (int s = 0; s < 3; s++)
    {
        for (int dir = 0; dir < 2; dir++)
        {
            int32_t st_a[HB_STATE] = { 0 }, st_c[HB_STATE] = { 0 };
            int pos = 0, len = 1;
            while (pos + len <= NS)
            {
                if (dir == 0)
                {
                    hb_up2(out_a + 2 * pos, in + pos, len, st_a, sets[s]);
                    hb_up2_c(out_c + 2 * pos, in + pos, len, st_c, sets[s]);
                }
                else
                {
                    hb_down2(out_a + pos, in + 2 * pos, len, st_a, sets[s]);
                    hb_down2_c(out_c + pos, in + 2 * pos, len, st_c, sets[s]);
                }
                pos += len;
                len = len % 97 + 1 + (len & 3);
            }
            int nout = dir == 0 ? 2 * pos : pos;
            for (int i = 0; i < nout; i++)
            {
                bad |= out_a[i] != out_c[i];
                sum = (sum ^ (uint32_t)out_a[i]) * 16777619u;
            }
            for (int i = 0; i < HB_STATE; i++)
                bad |= st_a[i] != st_c[i];
        }
    }
    static char msg[] = "ARM==C ok xxxxxxxx\n";
    put_hex(msg + 10, sum);
    sh_write0(bad ? "ARM==C FAIL\n" : msg);
#elif defined(HERMITE) && WL >= 7 && WL <= 15
    static struct rs_data d;
    static int32_t obuf[192];
    struct dsp_buf src = { 0 }, dst = { 0 };
    uint32_t outs = 0;
    d.delta = hermite_delta[WL - 7];
    src.format[1] = 1;                      /* mono */
    for (int pos = 0; pos < NS; pos += 96)
    {
        src.remcount = 96;
        src.p32[0] = src.p32[1] = in + pos;
        while (src.remcount > 0)
        {
            dst.p32[0] = dst.p32[1] = obuf;
            dst.bufcount = 192;
            int took = resample_hermite(&d, &src, &dst);
            outs += dst.remcount;
            src.remcount -= took;
            src.p32[0] += took;
            src.p32[1] = src.p32[0];
            if (took == 0 && dst.remcount == 0)
                break;
        }
    }
    static char msg[] = "OUT xxxxxxxx\n";
    put_hex(msg + 4, outs);
    sh_write0(msg);
#elif WL == 20
    /* FIR kernels: rows at many phases (every interpolation weight, the
     * last row), batches of 1..16 with every advance 0..2, ARM vs C */
    uint32_t sum = 0;
    int bad = 0;
    static int32_t rc[16][RS_FIR_TAPS];
    uint32_t frac = 0x9e3779b9u;
    int pos = 0;
    for (int it = 0; it < 600; it++)
    {
        int n = 1 + it % 16;
        for (int k = 0; k < n; k++)
        {
            frac = frac * 2654435761u + 0x7f4a7c15u;
            if (it % 50 == 0)
                frac |= 0xfc000000u;            /* the last rows */
            FIR_ROW(rows[k], frac);
            rs_fir_row_c(rc[k], frac);
            for (int i = 0; i < RS_FIR_TAPS; i++)
                bad |= rows[k][i] != rc[k][i];
            tags[k] = (uintptr_t)rows[k] | ((frac >> 7) % 3);
        }
        if (pos + 2 * n + RS_FIR_TAPS > 2 * NS)
            pos = 0;
        FIR_BATCH(out_a, in + pos, tags, n);
        rs_fir_batch_c(out_c, in + pos, tags, n);
        for (int k = 0; k < n; k++)
        {
            bad |= out_a[k] != out_c[k];
            sum = (sum ^ (uint32_t)out_a[k]) * 16777619u;
        }
        pos += n;
    }
    static char msg[] = "FIR ARM==C ok xxxxxxxx\n";
    put_hex(msg + 14, sum);
    sh_write0(bad ? "FIR ARM==C FAIL\n" : msg);
#elif WL % 100 >= 21 && WL % 100 <= 23
    uint32_t ph = 0, w = 0;
    for (int k = 0; k < 16; k++)    /* WL 23: rows made once, up front */
        FIR_ROW(rows[k], (uint32_t)(((uint64_t)k << 32) / 16));
    for (int o = 0; o < NS; o += 16)
    {
        uint32_t wl = w;
        int __attribute__((unused)) w0 = w;
        for (int k = 0; k < 16; k++)
        {
#if WL % 100 != 23
            FIR_ROW(rows[k], (uint32_t)(((uint64_t)ph * 13743895348ull) >> 8));
#endif
            tags[k] = (uintptr_t)rows[k] | (w - wl);
            wl = w;
            ph += 67;               /* 147 / 80 = 1 + 67/80 */
            w += 1;
            if (ph >= 80)
            {
                ph -= 80;
                w++;
            }
        }
#if WL % 100 != 22
        FIR_BATCH(out_a + o, in + w0, tags, 16);
#endif
    }
#elif WL != 99
    int32_t st[HB_STATE] = { 0 };
    const struct hb_coefs *c = sets[(WL % 100 - 1) % 3];
    int down = (WL % 100) > 3;
#if WL > 100
#define UP hb_up2_c
#define DOWN hb_down2_c
#else
#define UP hb_up2
#define DOWN hb_down2
#endif
    /* blocks of 96 inputs (x2) or 96 outputs (/2), as resample.c calls */
    for (int pos = 0; pos < NS; pos += 96)
    {
        if (down)
            DOWN(out_a + pos, in + 2 * pos, 96, st, c);
        else
            UP(out_a + 2 * pos, in + pos, 96, st, c);
    }
#endif
    return 0;
}
