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
 * bcmfw video: the vector kernels against their C versions, on the chip
 * (dev14).  Session 12's pictures came out green on silicon where vc2sim
 * decodes them right; each kernel runs on the same input as its C twin
 * and every byte of both 64 x 24 areas is compared, so a kernel that
 * computes wrong or writes outside its block shows.  Writes far away are
 * the guard's (video.c).
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
#include "bcmfw.h"
#include "video.h"
#include "h264_int.h"     /* memcpy, memset */
#include "h264_dsp.h"
#include "disp.h"

#define ST_PITCH  64
#define ST_BYTES  (ST_PITCH * 24)
#define ST_SRC    ((uint8_t *)VIDEO_ST_AREA)
#define ST_A      (ST_SRC + ST_BYTES)
#define ST_B      (ST_A + ST_BYTES)

static int16_t coef[2][16] __attribute__((aligned(32)));
const void *video_selftest_coef = coef;
static uint32_t seed;
static uint32_t st_fail, st_first, st_got, st_want, st_run;
static uint32_t *st_each;
uint32_t video_st_dumped;
static uint32_t vrf_done, st_clean;
static uint32_t st_calls[11];   /* comparisons made per test so far */   /* per test: offset | vector << 16 | C << 24 */

/* Low SRAM (vectors, VP header, mailbox) around every test (dev15):
 * session 13's self-test left the event index reading d4d4d4d4 and OPEN
 * never answered.  A test that changes a word there is recorded and the
 * word put back, so OPEN still replies with the verdict. */
#define LOW_WORDS 127             /* 0x004-0x1FF: word 0 is the reset branch */
#define LOW ((volatile uint32_t *)4)
#if BCMFW_JOBS_STAGED
/* the staged merged image (config.h BCMFW_JOBS_LOW_SNAP): out of the
 * image, in spare SRAM, for the staged upload's code below the arena */
#define low_snap ((uint32_t *)BCMFW_JOBS_LOW_SNAP)
#else
static uint32_t low_snap[LOW_WORDS];
#endif
static uint32_t low_hit, low_addr, low_val;

static void low_take(void)
{
    uint32_t k;

    for (k = 0; k < LOW_WORDS; k++)
        low_snap[k] = LOW[k];
}

static void low_check(uint32_t test)
{
    uint32_t k;

    for (k = 0; k < LOW_WORDS; k++)
        if (LOW[k] != low_snap[k])
        {
            if (!low_hit)
            {
                low_addr = 4 + 4 * k;
                low_val = LOW[k];
            }
            low_hit |= 1u << test;
            LOW[k] = low_snap[k];
        }
}

static uint32_t rnd(void)
{
    seed = seed * 1664525u + 1013904223u;
    return seed >> 24;
}

/* A smooth field (the loop filter acts on it), the same in A and B */
static void fill(void)
{
    uint32_t k;

    for (k = 0; k < ST_BYTES; k++)
        ST_SRC[k] = (uint8_t)(96 + (k % ST_PITCH) / 4 + (rnd() & 7));
    memcpy(ST_A, ST_SRC, ST_BYTES);
    memcpy(ST_B, ST_SRC, ST_BYTES);
    low_take();
    if (st_clean)
        video_vrf_clear();      /* dev22: the same start as vc2sim's */
}

static void compare(uint32_t test)
{
    uint32_t k;

    /* the VRF as the vector kernel left it (the C twin runs scalar),
     * once per test, for the host to compare with vc2sim (dev21) */
    if (!(vrf_done & (1u << test)))
    {
        vrf_done |= 1u << test;
        video_vrf_dump((uint8_t *)(VIDEO_SDRAM + VIDEO_DUMP_VRF) + 2048 * test);
    }
    low_check(test);
    st_run++;
    st_calls[test]++;
    for (k = 0; k < ST_BYTES; k++)
        if (ST_A[k] != ST_B[k])
        {
            if (!(st_fail & (1u << test)))
            {
                /* each failing test's areas as they are now, for the host
                 * (dev23: every test, not only the first): a 64-byte
                 * header (the failing call's index within the test),
                 * then source, vector's, C's */
                uint8_t *d = (uint8_t *)(VIDEO_SDRAM + VIDEO_DUMP_ST)
                           + VIDEO_DUMP_ST_STRIDE * test;

                memset(d, 0, 64);
                memcpy(d, &st_calls[test], 4);
                memcpy(d + 64, ST_SRC, 3 * ST_BYTES);
                video_st_dumped = 1;
            }
            if (!st_fail)
            {
                st_first = test << 24 | k;
                st_got = ST_A[k];
                st_want = ST_B[k];
            }
            if (!(st_fail & (1u << test)))
                st_each[test] = k | (uint32_t)ST_A[k] << 16
                              | (uint32_t)ST_B[k] << 24;
            st_fail |= 1u << test;
            return;
        }
}

static void idct(int dc)
{
    uint32_t k;

    for (k = 0; k < 16; k++)
        coef[0][k] = coef[1][k] = dc && k ? 0 : (int16_t)(rnd() & 127) - 64;
    if (dc)
    {
        h264_vidct4_dc_add(ST_A + 8 * ST_PITCH + 16, ST_PITCH, coef[0]);
        h264_idct4_dc_add_c(ST_B + 8 * ST_PITCH + 16, ST_PITCH, coef[1]);
    }
    else
    {
        h264_vidct4_add(ST_A + 8 * ST_PITCH + 16, ST_PITCH, coef[0]);
        h264_idct4_add_c(ST_B + 8 * ST_PITCH + 16, ST_PITCH, coef[1]);
    }
}

static void disp(int ramp)
{
    uint8_t *y = ST_SRC, *u = ST_SRC + 512, *v = ST_SRC + 640;
    uint32_t k;

    for (k = 0; k < 512; k++)
        y[k] = ramp ? (uint8_t)(16 + (k % 32) * 7) : 128;
    for (k = 0; k < 128; k++)
    {
        u[k] = ramp ? (uint8_t)(16 + (k % 16) * 14) : 128;
        v[k] = ramp ? (uint8_t)(240 - (k / 16) * 28) : 128;
    }
    memset(ST_A, 0, ST_BYTES);
    memset(ST_B, 0, ST_BYTES);
    low_take();
    if (st_clean)
        video_vrf_clear();
    disp_use_c = 0;
    disp_convert((uint16_t *)ST_A, ST_PITCH, y, u, v, 32, 32, 16);
    disp_use_c = 1;
    disp_convert((uint16_t *)ST_B, ST_PITCH, y, u, v, 32, 32, 16);
}

/* Bits of failing tests (VIDEO_ST_*); out: the first wrong byte (test
 * << 24 | offset), the vector's and C's bytes, comparisons made, the
 * tests that changed low SRAM, and the first such word's address and
 * value as the test left it */
uint32_t video_selftest(uint32_t out[7], uint32_t each[11], int clean)
{
    static const uint8_t bs4[4] = { 4, 4, 4, 4 }, bs2[4] = { 2, 2, 2, 2 };
    static const int8_t tc0[4] = { 2, 2, 2, 2 };
    uint32_t off_save = h264_vpu_off, disp_save = disp_use_c,
             bad_save = h264_vpu_bad;
    int m, k;

    seed = 2722;
    st_fail = st_first = st_got = st_want = st_run = 0;
    low_hit = low_addr = low_val = 0;
    vrf_done = 0;
    st_clean = clean;
    memset(st_calls, 0, sizeof(st_calls));
    st_each = each;
    for (k = 0; k < 11; k++)
        each[k] = 0;
    h264_vpu_off = 0;
    h264_vpu_bad = 0;           /* test the vector versions, all of them */

    fill(); idct(0); compare(VIDEO_ST_IDCT);
    fill(); idct(1); compare(VIDEO_ST_IDCT_DC);
    for (m = 0; m < 9; m++)
    {
        fill();
        h264_pred4x4(ST_A + 8 * ST_PITCH + 16, ST_PITCH, m, 15);
        h264_pred4x4_c(ST_B + 8 * ST_PITCH + 16, ST_PITCH, m, 15);
        compare(VIDEO_ST_PRED4);
    }
    for (m = 0; m < 4; m++)
    {
        fill();
        h264_pred16x16(ST_A + 4 * ST_PITCH + 16, ST_PITCH, m, 15);
        h264_pred16x16_c(ST_B + 4 * ST_PITCH + 16, ST_PITCH, m, 15);
        compare(VIDEO_ST_PRED16);
        fill();
        h264_pred_chroma8x8(ST_A + 8 * ST_PITCH + 16, ST_PITCH, m, 15);
        h264_pred_chroma8x8_c(ST_B + 8 * ST_PITCH + 16, ST_PITCH, m, 15);
        compare(VIDEO_ST_PREDC);
    }
    for (k = 0; k < 16; k += 5)
    {
        int fx = k & 3, fy = k >> 2;

        fill();
        h264_mc_luma(ST_A + 4 * ST_PITCH + 16, ST_PITCH,
                     ST_SRC + 4 * ST_PITCH + 8, ST_PITCH, 16, 16, fx, fy);
        h264_mc_luma_c(ST_B + 4 * ST_PITCH + 16, ST_PITCH,
                       ST_SRC + 4 * ST_PITCH + 8, ST_PITCH, 16, 16, fx, fy);
        compare(VIDEO_ST_MC_LUMA);
        fill();
        h264_mc_chroma(ST_A + 8 * ST_PITCH + 16, ST_PITCH,
                       ST_SRC + 8 * ST_PITCH + 8, ST_PITCH, 8, 8,
                       2 * fx + 1, 2 * fy);
        h264_mc_chroma_c(ST_B + 8 * ST_PITCH + 16, ST_PITCH,
                         ST_SRC + 8 * ST_PITCH + 8, ST_PITCH, 8, 8,
                         2 * fx + 1, 2 * fy);
        compare(VIDEO_ST_MC_CHROMA);
    }
    for (k = 0; k < 2; k++)
    {
        const uint8_t *bs = k ? bs2 : bs4;

        fill();
        h264_dbk_luma(ST_A + 4 * ST_PITCH + 16, 1, ST_PITCH, 40, 10, bs, tc0);
        h264_dbk_luma_c(ST_B + 4 * ST_PITCH + 16, 1, ST_PITCH, 40, 10, bs,
                        tc0);
        h264_dbk_luma(ST_A + 12 * ST_PITCH + 16, ST_PITCH, 1, 40, 10, bs,
                      tc0);
        h264_dbk_luma_c(ST_B + 12 * ST_PITCH + 16, ST_PITCH, 1, 40, 10, bs,
                        tc0);
        compare(VIDEO_ST_DBK_LUMA);
        fill();
        h264_dbk_chroma(ST_A + 8 * ST_PITCH + 16, 1, ST_PITCH, 40, 10, bs,
                        tc0);
        h264_dbk_chroma_c(ST_B + 8 * ST_PITCH + 16, 1, ST_PITCH, 40, 10, bs,
                          tc0);
        h264_dbk_chroma(ST_A + 12 * ST_PITCH + 32, ST_PITCH, 1, 40, 10, bs,
                        tc0);
        h264_dbk_chroma_c(ST_B + 12 * ST_PITCH + 32, ST_PITCH, 1, 40, 10, bs,
                          tc0);
        compare(VIDEO_ST_DBK_CHROMA);
    }
    disp(0); compare(VIDEO_ST_DISP_FLAT);
    disp(1); compare(VIDEO_ST_DISP_RAMP);

    h264_vpu_off = off_save;
    h264_vpu_bad = bad_save;
    disp_use_c = disp_save;
    out[0] = st_first;
    out[1] = st_got;
    out[2] = st_want;
    out[3] = st_run;
    out[4] = low_hit;
    out[5] = low_addr;
    out[6] = low_val;
    return st_fail;
}

/* Scalar arithmetic on the chip against the compiler's constant folding
 * (dev17): session 15's colours were wrong the same way whether the
 * kernels ran on the vectors or in C, and the C transform saturated where
 * the vector one didn't.  Each case's inputs come through volatiles, so
 * the chip computes them; the expected values are folded on the host.
 * Returns the failing cases' bits; out: the first failing case and what
 * the chip computed. */
static volatile int32_t vi_m100 = -100, vi_m7 = -7, vi_2 = 2, vi_3 = 3,
                        vi_6 = 6, vi_200 = 200, vi_300 = 300,
                        vi_70000 = 70000, vi_m1 = -1, vi_1 = 1,
                        vi_m40000 = -40000;
static volatile uint32_t vu_big = 0xF0000000u, vu_4 = 4;
static volatile int8_t vs8 = -5;
static volatile int16_t vs16 = -300;
static volatile uint8_t vu8 = 250;

uint32_t video_alutest(uint32_t out[2])
{
    int32_t got[16], want[16];
    uint32_t fail = 0, k;

    got[0] = vi_m100 >> vi_3;          want[0] = -100 >> 3;
    got[1] = vi_m100 >> 6;             want[1] = -100 >> 6;
    got[2] = (int32_t)(vu_big >> vu_4); want[2] = (int32_t)(0xF0000000u >> 4);
    got[3] = vi_200 * -1604;           want[3] = 200 * -1604;
    got[4] = (vi_200 * -1604) >> 8;    want[4] = (200 * -1604) >> 8;
    got[5] = vi_300 * 320;             want[5] = 300 * 320;
    got[6] = vi_70000 * vi_3;          want[6] = 70000 * 3;
    got[7] = vi_m7 / vi_2;             want[7] = -7 / 2;
    got[8] = vi_m7 % vi_2;             want[8] = -7 % 2;
    got[9] = vs8;                      want[9] = -5;
    got[10] = vs16;                    want[10] = -300;
    got[11] = vu8;                     want[11] = 250;
    got[12] = vi_m1 < vi_1;            want[12] = 1;
    got[13] = vi_m40000 * vi_6 >> 4;   want[13] = -40000 * 6 >> 4;
    got[14] = (vi_m100 * 4769) >> 8;   want[14] = (-100 * 4769) >> 8;
    got[15] = vi_m100 < 0 ? 0 : vi_m100 > 255 ? 255 : vi_m100;
    want[15] = 0;
    out[0] = out[1] = 0;
    for (k = 0; k < 16; k++)
        if (got[k] != want[k])
        {
            if (!fail)
            {
                out[0] = k;
                out[1] = (uint32_t)got[k];
            }
            fail |= 1u << k;
        }
    return fail;
}
