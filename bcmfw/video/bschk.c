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

/*
 * The merged image's start-up checks of the vector forms a loop filter on
 * the vector unit would need (batch 4; docs/research/bcm2722/decoder.md,
 * "The loop filter in vectors"), and the bitstream's memory probe (SDRAM
 * against SRAM load latency, open question #7).  Information only: no
 * decoder code uses the forms yet, and nothing reads the results but the
 * PP's bench (devbench.c s_jvpu, which logs them; its verdict ignores
 * them).  The forms join proven_forms.txt only once silicon has run them.
 */
#include "config.h"
#include "hw.h"
#include "clock.h"
#include "jobs.h"
#include "bschk.h"

#define JB(off)     MB(BCMFW_JOBS_BLOCK + (off))

#if BCMFW_BSCHK
void jv_bschk_l1(const void *in, void *out);
void jv_bschk_f1(const void *in, void *out);
void jv_bschk_m1(const void *in, void *out, uint32_t thr);
void jv_bschk_r1(const void *in, void *out);
void jv_bschk_x1(const void *in, void *out);
void jv_bschk_s1(const void *in, void *out);
void jv_bschk_z1(const void *in, void *out, uint32_t thr, uint32_t val);

/* in: two 32-byte rows plus a 32-byte tail (BSL1 reads up to +8 + 32);
 * out: one 16-halfword result row plus a 32-byte guard (BSS1) */
static uint16_t in16[48] __attribute__((aligned(32)));
static uint16_t out16[32] __attribute__((aligned(32)));
#define IN8 ((uint8_t *)in16)
#define OUT8 ((uint8_t *)out16)

/* one form's result: the first lane whose halfword differs from exp[],
 * recorded with its got and expected words when it is the first form to
 * fail; returns the form's pass bit or 0 */
static uint32_t verdict(int form, const uint16_t *exp, int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (out16[i] != exp[i])
        {
            if (!JB(J_VPU2_FAIL))
            {
                JB(J_VPU2_FAIL) = (uint32_t)form | (uint32_t)i << 8;
                JB(J_VPU2_GOT) = out16[i];
                JB(J_VPU2_EXP) = exp[i];
            }
            return 0;
        }
    return J_VPU2_BIT(form);
}

static void clear_out(void)
{
    int i;
    for (i = 0; i < 32; i++)
        out16[i] = 0xA5A5;
}

void bschk_run(void)
{
    uint16_t exp[32];
    uint32_t v = J_VPU2_CHECKED;
    int i, p;

    JB(J_VPU2_FAIL) = 0;
    JB(J_VPU2_GOT) = 0;
    JB(J_VPU2_EXP) = 0;

    /* BSL1: an H16 row at line phases 2 and 8 (vldp16 + vldq16) */
    for (i = 0; i < 48; i++)
        in16[i] = (uint16_t)(0x1100 + 0x0101 * i);
    {
        uint32_t ok = J_VPU2_BIT(1);
        for (p = 1; p <= 4; p += 3)             /* halfwords 1 and 4 */
        {
            clear_out();
            jv_bschk_l1(in16 + p, out16);
            for (i = 0; i < 16; i++)
                exp[i] = in16[p + i];
            ok &= verdict(1, exp, 16);
        }
        v |= ok;
    }

    /* BSF1: A - B < 0 per 16-bit lane: |dMV| against 3, around 4 */
    for (i = 0; i < 16; i++)
    {
        in16[i] = 3;                            /* A */
        in16[16 + i] = (uint16_t)(i - 5);       /* B: -5 .. 10 */
        exp[i] = 3 - (i - 5) < 0;
    }
    clear_out();
    jv_bschk_f1(in16, out16);
    v |= verdict(2, exp, 16);

    /* BSM1: #7 into the lanes where v - thr < 0, the background elsewhere */
    for (i = 0; i < 16; i++)
    {
        in16[i] = (uint16_t)(i * 3);
        IN8[32 + i] = (uint8_t)(0x40 + i);
        exp[i] = i * 3 < 20 ? 7 : 0x40 + i;
    }
    clear_out();
    jv_bschk_m1(in16, out16, 20);
    v |= verdict(3, exp, 16);

    /* BSR1: bytes differ (the references of p and q) */
    for (i = 0; i < 16; i++)
    {
        IN8[i] = (uint8_t)(i & 3);
        IN8[32 + i] = (uint8_t)((i >> 2) & 3);
        exp[i] = (i & 3) != ((i >> 2) & 3);
    }
    clear_out();
    jv_bschk_r1(in16, out16);
    v |= verdict(4, exp, 16);

    /* BSX1: the larger of two bS rows (0..4, and 0..100 apart) */
    for (i = 0; i < 16; i++)
    {
        IN8[i] = (uint8_t)(i % 5);
        IN8[32 + i] = (uint8_t)((15 - i) * 6 % 101);
        exp[i] = IN8[i] > IN8[32 + i] ? IN8[i] : IN8[32 + i];
    }
    clear_out();
    jv_bschk_x1(in16, out16);
    v |= verdict(5, exp, 16);

    /* BSS1: 16 bytes stored, the 16 after them untouched */
    for (i = 0; i < 16; i++)
        IN8[i] = (uint8_t)(0x30 + 7 * i);
    clear_out();
    jv_bschk_s1(in16, out16);
    for (i = 0; i < 8; i++)
        exp[i] = (uint16_t)(IN8[2 * i] | IN8[2 * i + 1] << 8);
    for (; i < 16; i++)
        exp[i] = 0xA5A5;
    v |= verdict(6, exp, 16);

    /* BSZ1: r3 into the lanes where v - thr == 0 */
    for (i = 0; i < 16; i++)
    {
        in16[i] = (uint16_t)(i & 7);
        IN8[32 + i] = (uint8_t)(0x60 + i);
        exp[i] = (i & 7) == 5 ? 9 : 0x60 + i;
    }
    clear_out();
    jv_bschk_z1(in16, out16, 5, 9);
    v |= verdict(7, exp, 16);

    JB(J_VPU2) = v;
}
#endif /* BCMFW_BSCHK */

/* ns x 10 per load over n loads at p: 8 loads an iteration, the loop's
 * overhead included (the same for every target); the 1 MHz counter */
static uint32_t lat_bytes(const volatile uint8_t *p, uint32_t n)
{
    uint32_t t0, t, i, s = 0;
    t0 = REG(TIMER_CLO);
    for (i = 0; i < n; i += 8, p += 8)
        s += p[0] + p[1] + p[2] + p[3] + p[4] + p[5] + p[6] + p[7];
    t = REG(TIMER_CLO) - t0;
    JB(J_LAT_SINK) = s;
    return t * 10000u / n;
}

static uint32_t lat_words(const volatile uint32_t *p, uint32_t n)
{
    uint32_t t0, t, i, s = 0;
    t0 = REG(TIMER_CLO);
    for (i = 0; i < n; i += 8, p += 8)
        s += p[0] + p[1] + p[2] + p[3] + p[4] + p[5] + p[6] + p[7];
    t = REG(TIMER_CLO) - t0;
    JB(J_LAT_SINK) = s;
    return t * 10000u / n;
}

#define CLIP16(x)   ((x) > 0xFFFF ? 0xFFFF : (x))

void bschk_latency(const void *sdram, const void *sram)
{
    /* 8 KB of each: 8,192 byte loads and 2,048 word loads */
    uint32_t sb = lat_bytes(sdram, 8192), sw = lat_words(sdram, 2048);
    uint32_t rb = lat_bytes(sram, 8192), rw = lat_words(sram, 2048);
    JB(J_LAT_SDRAM) = CLIP16(sb) | CLIP16(sw) << 16;
    JB(J_LAT_SRAM) = CLIP16(rb) | CLIP16(rw) << 16;
    JB(J_LAT_MHZ) = clock_mhz() | J_LAT_DONE;
}
