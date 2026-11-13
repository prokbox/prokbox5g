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

/* #265: host test of the WM8758 driver's R7 (ADDCTRL) bit 8, M128ENB, the
 * DSP's full-rate mode.  Silicon (b11 row m128): EQ4 has no effect with
 * M128ENB = 0.  Compiles the real firmware/drivers/audio/wm8758.c against
 * tests/dacosr/stub, with a model of the chip: the last value written to
 * each register and the order of the writes (the bus is write-only, so
 * this is all the chip ever sees).
 *
 * Rules under test:
 *  - dac_eq_set() with band 4 at a gain sets R7 bit 8, before EQ4 (R21)
 *    gets its gain;
 *  - band 4 flat, or the EQ off (NULL), clears it, after EQ4 is flat;
 *  - the bit is kept across a rate change, a DAC standby and its wake,
 *    and a restart (audiohw_preinit() rewrites R7 from the shadow);
 *  - the 3D stage (dac_set_width3d(), Settings > Audiophile) needs the bit
 *    too: neither user clears it while the other still holds it;
 *  - only R7 bit 8 moves: R7's rate bits stay the rate's, and an unchanged
 *    request writes no R7.
 * Register numbers and bits are literals (the datasheet's), not the
 * driver's header names.
 */
#include <stdio.h>
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "audiohw.h"
#include "dac_ext.h"

long current_tick;
unsigned long stub_usec;
const unsigned long hw_freq_sampr[HW_NUM_FREQ] = {
    [HW_FREQ_96] = 96000, [HW_FREQ_88] = 88200, [HW_FREQ_48] = 48000,
    [HW_FREQ_44] = 44100, [HW_FREQ_32] = 32000, [HW_FREQ_24] = 24000,
    [HW_FREQ_22] = 22050, [HW_FREQ_16] = 16000, [HW_FREQ_12] = 12000,
    [HW_FREQ_11] = 11025, [HW_FREQ_8] = 8000,
};

#define R7          0x07        /* ADDCTRL */
#define R21         0x15        /* EQ4 */
#define R41         0x29        /* 3D control */
#define M128ENB     0x100       /* R7 bit 8 */
#define R7_44K      0x001       /* SR 48 kHz code (000), SLOWCLKEN */
#define R7_32K      0x003       /* SR 32 kHz code (001), SLOWCLKEN */
#define GAIN_MASK   0x01f       /* EQn bits 4:0: 12 - dB */

static int chip[0x40];
static int seq;                 /* writes so far */
static int last_at[0x40];       /* the sequence number of each register's last write */
static int r7_writes;
static bool playing;

void wmcodec_write(int reg, int data)
{
    reg &= 0x3f;
    chip[reg] = data & 0x1ff;
    last_at[reg] = ++seq;
    if (reg == R7)
        r7_writes++;
}

void wmcodec_clocks_gate(bool gate)
{
    (void)gate;
}

bool pcm_is_playing(void)
{
    return playing;
}

static int fails, checks;

static void expect_r7(const char *what, int want)
{
    checks++;
    if (chip[R7] != want) {
        printf("FAIL %s: R7 0x%03x, want 0x%03x\n", what, chip[R7], want);
        fails++;
    }
}

static void expect(const char *what, bool ok)
{
    checks++;
    if (!ok) {
        printf("FAIL %s\n", what);
        fails++;
    }
}

static void enter_standby(void)
{
    playing = false;
    current_tick += DAC_IDLE_MS * HZ / 1000 + 1;
    dac_idle_poll();
}

/* five bands, all flat, then the ones the case asks for */
static void bands(struct dac_eq_band *b, int g2, int g3, int g4)
{
    memset(b, 0, sizeof(*b) * DAC_EQ_BANDS);
    for (int i = 0; i < DAC_EQ_BANDS; i++)
        b[i].freq = 1;
    b[1].gain = g2;
    b[2].gain = g3;
    b[3].gain = g4;
}

int main(void)
{
    struct dac_eq_band b[DAC_EQ_BANDS];
    int before;

    /* boot: preinit, the rated rate, postinit's unmute; EQ never set */
    audiohw_preinit();
    audiohw_set_frequency(HW_FREQ_44);
    audiohw_postinit();
    expect_r7("boot 44.1k", R7_44K);

    /* EQ2 and EQ3 only: the low-power mode keeps them (S1) */
    bands(b, 6, -4, 0);
    dac_eq_set(b, 6);
    expect_r7("EQ2/EQ3 only", R7_44K);

    /* band 4 at +6 dB: M128ENB, written before EQ4's gain */
    bands(b, 6, -4, 6);
    dac_eq_set(b, 6);
    expect_r7("band 4 +6", R7_44K | M128ENB);
    expect("band 4 +6: EQ4 holds the gain", (chip[R21] & GAIN_MASK) == 6);
    expect("band 4 +6: R7 written before EQ4", last_at[R7] < last_at[R21]);

    /* the same bands again: nothing to write to R7 */
    before = r7_writes;
    dac_eq_set(b, 6);
    expect("same bands: no R7 write", r7_writes == before);

    /* a cut on band 4 counts too */
    bands(b, 0, 0, -3);
    dac_eq_set(b, 3);
    expect_r7("band 4 -3", R7_44K | M128ENB);

    /* rate changes keep it, with the new rate's bits */
    audiohw_set_frequency(HW_FREQ_32);
    expect_r7("band 4, 32k", R7_32K | M128ENB);
    audiohw_set_frequency(HW_FREQ_44);
    expect_r7("band 4, back to 44.1k", R7_44K | M128ENB);

    /* DAC standby and its wake keep it */
    enter_standby();
    expect_r7("band 4, standby", R7_44K | M128ENB);
    dac_wake();
    expect_r7("band 4, wake", R7_44K | M128ENB);

    /* a rate set during standby keeps it */
    enter_standby();
    audiohw_set_frequency(HW_FREQ_32);
    expect_r7("band 4, 32k set in standby", R7_32K | M128ENB);
    dac_wake();
    audiohw_set_frequency(HW_FREQ_44);

    /* a restart (close, preinit, postinit) writes R7 from the shadow */
    audiohw_close();
    audiohw_preinit();
    audiohw_postinit();
    expect_r7("band 4, restart", R7_44K | M128ENB);

    /* band 4 flat again: cleared, after EQ4 is flat */
    bands(b, 6, -4, 0);
    dac_eq_set(b, 6);
    expect_r7("band 4 flat", R7_44K);
    expect("band 4 flat: EQ4 flat", (chip[R21] & GAIN_MASK) == 12);
    expect("band 4 flat: R7 written after EQ4", last_at[R7] > last_at[R21]);

    /* EQ off clears it */
    bands(b, 0, 0, 9);
    dac_eq_set(b, 9);
    expect_r7("band 4 +9", R7_44K | M128ENB);
    dac_eq_set(NULL, 0);
    expect_r7("EQ off", R7_44K);
    audiohw_set_frequency(HW_FREQ_32);
    expect_r7("EQ off, 32k", R7_32K);
    audiohw_set_frequency(HW_FREQ_44);

    /* the 3D stage holds it while band 4 goes flat ... */
    dac_set_width3d(8);
    expect_r7("3D on", R7_44K | M128ENB);
    expect("3D on: depth written", chip[R41] == 8);
    bands(b, 0, 0, 4);
    dac_eq_set(b, 4);
    bands(b, 0, 0, 0);
    dac_eq_set(b, 0);
    expect_r7("3D on, band 4 flat", R7_44K | M128ENB);
    dac_eq_set(NULL, 0);
    expect_r7("3D on, EQ off", R7_44K | M128ENB);
    dac_set_width3d(0);
    expect_r7("3D off, EQ off", R7_44K);

    /* ... and band 4 holds it while the 3D stage goes off */
    bands(b, 0, 0, 5);
    dac_eq_set(b, 5);
    dac_set_width3d(10);
    dac_set_width3d(0);
    expect_r7("band 4 +5, 3D on then off", R7_44K | M128ENB);
    expect("3D off: depth 0", chip[R41] == 0);
    dac_eq_set(NULL, 0);
    expect_r7("EQ off after 3D off", R7_44K);

    if (checks < 27) {
        printf("FAIL only %d checks ran\n", checks);
        return 1;
    }
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
