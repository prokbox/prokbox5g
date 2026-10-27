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
