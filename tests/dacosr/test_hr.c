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

/* p090-osr128: host test of the WM8758 driver's two high-rate switches,
 * against the real firmware/drivers/audio/wm8758.c and a model of the
 * write-only chip (the last value written to each register).
 *
 * The EQ guard (dac_set_hirate_eq_guard(), off by default; the test
 * turns it on as the bench does for its baseline arms): at 88.2 and
 * 96 kHz no EQ band (R18..R22) holds a gain, on every path that could set
 * one: a preset or custom curve applied while a high rate plays, a rate
 * change into 88.2/96 with the EQ on, bass and treble writes, the EQ off
 * and on again, DAC standby and its wake, a restart, Hi-Res 128x moving
 * the OSR.  No EQ precut or make-up there either (the DAC volume is the
 * plain volume) and the limiter is off.  Below 88.2 kHz, or with the
 * guard off, the shell's last request comes back whole.
 * The 3D stage is NOT held (the owner's Stereo Width plays at every rate).
 *
 * The M128ENB user (dac_set_m128_hirate(), off by default): R7 bit 8 set
 * while the output is 88.2/96 kHz at 128x, clear at 64x and at 48 kHz and
 * below unless EQ band 4 or the 3D stage hold it (#265's OR).  A live
 * change while music plays goes in with R10 soft-muted.
 *
 * Register numbers and bits are literals (the datasheet's).
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
#define R10         0x0a        /* DACCTRL */
#define R11         0x0b        /* LDACVOL */
#define R18         0x12        /* EQ1 .. R22 EQ5 */
#define R24         0x18        /* DACLIMITER1 */
#define R41         0x29        /* 3D control */
#define M128ENB     0x100
#define R10_MUTE    0x040
#define GAIN_MASK   0x01f       /* EQn bits 4:0: 12 - dB */
#define GAIN_FLAT   12

static int chip[0x40];
static bool playing;
static int r7_unmuted;          /* R7 writes while R10 played (unmuted) */
static int eq_hot_hi;           /* EQ gains written while at 88.2/96 */
static bool at_hi;              /* the test's own view of the rate */
static int fails, checks;

void wmcodec_write(int reg, int data)
{
    reg &= 0x3f;
    chip[reg] = data & 0x1ff;
    if (reg == R7 && playing && !(chip[R10] & R10_MUTE))
        r7_unmuted++;
    if (at_hi && reg >= R18 && reg <= R18 + 4
        && (chip[reg] & GAIN_MASK) != GAIN_FLAT)
        eq_hot_hi++;
}

void wmcodec_clocks_gate(bool gate)
{
    (void)gate;
}

bool pcm_is_playing(void)
{
    return playing;
}

static void expect(const char *what, bool ok)
{
    checks++;
    if (!ok) {
        printf("FAIL %s\n", what);
        fails++;
    }
}

static int eq_hot(void)         /* bands holding a gain now */
{
    int n = 0;
    for (int r = R18; r <= R18 + 4; r++)
        if ((chip[r] & GAIN_MASK) != GAIN_FLAT)
            n++;
    return n;
}

static void expect_flat(const char *what)
{
    char w[128];
    struct dac_stats st;

    dac_get_stats(&st);
    snprintf(w, sizeof w, "%s: EQ flat (bands with a gain: %d)", what,
             eq_hot());
    expect(w, eq_hot() == 0);
    snprintf(w, sizeof w, "%s: no precut (R11 0x%03x)", what, chip[R11]);
    expect(w, st.precut_db == 0 && chip[R11] == 0xff);
    snprintf(w, sizeof w, "%s: limiter off", what);
    expect(w, !(chip[R24] & 0x100));
    snprintf(w, sizeof w, "%s: stats hr_bypass", what);
    expect(w, st.hr_bypass && !st.hw_eq);
}

static void expect_m128(const char *what, bool on)
{
    char w[128];
    snprintf(w, sizeof w, "%s: R7 0x%03x, M128ENB want %d", what, chip[R7],
             (int)on);
    expect(w, !!(chip[R7] & M128ENB) == on);
}

static void rate(int fsel)
{
    playing = false;            /* a rate set always stops the PCM */
    at_hi = fsel == HW_FREQ_88 || fsel == HW_FREQ_96;
    audiohw_set_frequency(fsel);
    playing = true;
}

static void boost(struct dac_eq_band *b, int g)
{
    for (int i = 0; i < DAC_EQ_BANDS; i++) {
        b[i].gain = g;
        b[i].freq = 1;
        b[i].wide = 0;
    }
}

static void enter_standby(void)
{
    playing = false;
    current_tick += DAC_IDLE_MS * HZ / 1000 + 1;
    dac_idle_poll();
}

int main(void)
{
    struct dac_eq_band b[DAC_EQ_BANDS];
    struct dac_stats st;

    audiohw_preinit();
    audiohw_set_frequency(HW_FREQ_44);
    audiohw_postinit();
    audiohw_set_volume(0, 0);               /* 0 dB: R11 0xff, no precut */

    /* defaults (b18): the guard off (the owner's custom slots play at
     * 88.2/96), the M128ENB user on (half rate crackled in the tail) */
    dac_get_stats(&st);
    expect("default: EQ guard off", !st.hr_guard);
    expect("default: M128ENB user on", st.m128_hr_allow);
    dac_set_hirate_eq_guard(true);          /* the bench's baseline */
    dac_get_stats(&st);
    expect("guard on at 44.1: nothing held", st.hr_guard && !st.hr_bypass);

    /* EQ on at 44.1: plays (the base table) */
    boost(b, 6);
    dac_eq_set(b, 6, 30);
    expect("44.1 EQ: all five bands hold a gain", eq_hot() == 5);

    /* into 96 kHz with the EQ on: flat, no precut, the request kept */
    rate(HW_FREQ_96);
    expect_flat("44.1 -> 96 with the EQ on");

    /* a preset applied while 96 plays: still flat */
    boost(b, 9);
    dac_eq_set(b, 9, 40);
    expect_flat("preset at 96");
    /* EQ off then on again at 96 */
    dac_eq_set(NULL, 0, 0);
    expect_flat("EQ off at 96");
    boost(b, -6);
    dac_eq_set(b, 0, 0);
    expect_flat("a cut curve at 96");
    boost(b, 9);
    dac_eq_set(b, 9, 40);
    /* bass and treble (upstream's tone controls) at 96 */
    audiohw_set_bass(6);
    audiohw_set_treble(-6);
    expect_flat("bass/treble at 96");
    audiohw_set_bass(0);
    audiohw_set_treble(0);

    /* standby, a rate set in standby, the wake */
    enter_standby();
    dac_wake();
    playing = true;
    expect_flat("96 after standby");
    rate(HW_FREQ_88);
    expect_flat("96 -> 88.2");

    /* Hi-Res 128x on and off while 88.2 plays */
    dac_set_osr128(DAC_OSR128_88 | DAC_OSR128_96);
    expect_flat("88.2, 128x on live");
    dac_set_osr128(0);
    expect_flat("88.2, 128x off live");

    /* back to 44.1: the last request whole (+9 on five bands) */
    rate(HW_FREQ_44);
    dac_get_stats(&st);
    expect("back to 44.1: the request back", eq_hot() == 5 &&
           (chip[R18 + 2] & GAIN_MASK) == GAIN_FLAT - 9 && st.hw_eq &&
           !st.hr_bypass && st.precut_db == 9);

    /* the guard off for a row: the EQ plays at 96 */
    rate(HW_FREQ_96);
    expect_flat("96 again");
    at_hi = false;              /* gains at 96 allowed from here */
    dac_set_hirate_eq_guard(false);
    dac_get_stats(&st);
    expect("guard off at 96: EQ plays", eq_hot() == 5 && st.hw_eq &&
           !st.hr_guard && !st.hr_bypass);
    dac_set_hirate_eq_guard(true);
    at_hi = true;
    expect_flat("guard back on at 96");

    /* a restart (close, preinit, postinit) at 96; preinit itself sets
     * 44.1 kHz first, where the request comes back */
    at_hi = false;
    audiohw_close();
    audiohw_preinit();
    rate(HW_FREQ_96);
    audiohw_postinit();
    audiohw_set_volume(0, 0);
    expect_flat("restart at 96");

    /* the 3D stage is not held: Stereo Width plays at 96 */
    dac_set_width3d(10);
    dac_get_stats(&st);
    expect("3D at 96: R41 depth 10", chip[R41] == 10 && st.width3d == 10);
    expect_m128("3D at 96 (64x)", true);
    dac_set_width3d(0);
    expect_m128("3D off at 96 (64x)", false);
    dac_eq_set(NULL, 0, 0);
    rate(HW_FREQ_44);
    expect("EQ off back at 44.1: flat", eq_hot() == 0);

    /* ---- the M128ENB user ---------------------------------------- */
    dac_set_m128_hirate(false);             /* off from here: on by default */
    dac_set_osr128(DAC_OSR128_88 | DAC_OSR128_96);
    rate(HW_FREQ_96);
    expect_m128("switch off, 96 at 128x", false);
    r7_unmuted = 0;
    dac_set_m128_hirate(true);              /* live, 96 at 128x */
    dac_get_stats(&st);
    expect_m128("switch on live, 96 at 128x", true);
    expect("switch on live: R7 written muted", r7_unmuted == 0);
    expect("switch on live: unmuted after", !(chip[R10] & R10_MUTE));
    expect("switch on: stats", st.m128_hr_allow && st.m128);
    rate(HW_FREQ_44);
    expect_m128("switch on, 44.1", false);
    rate(HW_FREQ_88);
    expect_m128("switch on, 88.2 at 128x", true);
    r7_unmuted = 0;
    dac_set_osr128(DAC_OSR128_96);          /* 88.2 drops to 64x live */
    expect_m128("switch on, 88.2 at 64x", false);
    expect("128x -> 64x live: R7 written muted", r7_unmuted == 0);
    rate(HW_FREQ_96);
    expect_m128("switch on, 96 at 128x again", true);
    enter_standby();
    dac_wake();
    playing = true;
    expect_m128("switch on, 96 after standby", true);
    /* EQ band 4 at 44.1 holds it; the high-rate user leaves it alone */
    rate(HW_FREQ_44);
    boost(b, 0);
    b[3].gain = 6;
    dac_eq_set(b, 6, 0);
    expect_m128("44.1, band 4 +6", true);
    rate(HW_FREQ_96);
    expect_m128("96 at 128x, band 4 guarded", true);
    dac_set_m128_hirate(false);
    expect_m128("switch off, 96, band 4 guarded", false);
    rate(HW_FREQ_44);
    expect_m128("back at 44.1, band 4 +6 again", true);
    dac_eq_set(NULL, 0, 0);
    expect_m128("EQ off at 44.1", false);

    if (checks < 60) {
        printf("FAIL only %d checks ran\n", checks);
        return 1;
    }
    expect("no EQ gain ever written at 88.2/96 with the guard on",
           eq_hot_hi == 0);
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
