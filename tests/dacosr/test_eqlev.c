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
 * Host test: the WM8758 driver's EQ level writes (#203) and AMUTE (#327)
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

/* The real firmware/drivers/audio/wm8758.c on the stub register model.
 * After EVERY register write the test decodes the chip's state and checks:
 *  - headroom: the DAC attenuation (R11) is at least the peak of the EQ
 *    now in R18-R22 (eqlab_precut() of the decoded bands): no step plays a
 *    boost without its cut;
 *  - never louder: the analogue amp (R52) is at most the volume plus the
 *    trim the test asked Sound Check for plus the DAC cut: no step plays
 *    a make-up its cut doesn't cover.
 * And in the end states: the make-up reaches the amp, summed with Sound
 * Check's trim and capped by the Volume Limit; the limiter (R24/R25) is
 * allowed by default (b18), on only while the EQ is on and allowed, its threshold
 * written before LIMEN; AMUTE (R10 bit 2) only when asked, kept across a
 * rate change and a mute. Registers and bits are literals. */
#include <stdio.h>
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "audiohw.h"
#include "dac_ext.h"
#include <stdlib.h>
#include "ipodui/eqlab_core.h"
#include "ipodui/eq_hw.h"

long current_tick;
unsigned long stub_usec;
const unsigned long hw_freq_sampr[HW_NUM_FREQ] = {
    [HW_FREQ_96] = 96000, [HW_FREQ_88] = 88200, [HW_FREQ_48] = 48000,
    [HW_FREQ_44] = 44100, [HW_FREQ_32] = 32000, [HW_FREQ_24] = 24000,
    [HW_FREQ_22] = 22050, [HW_FREQ_16] = 16000, [HW_FREQ_12] = 12000,
    [HW_FREQ_11] = 11025, [HW_FREQ_8] = 8000,
};

#define R10         0x0a
#define R11         0x0b        /* LDACVOL: 255 = 0 dB, 0.5 dB a step */
#define R18         0x12        /* EQ1 .. R22 EQ5 */
#define R24         0x18
#define R25         0x19
#define R52         0x34        /* LOUT1VOL: 57 = 0 dB, 1 dB a step */
#define AMUTE       0x004
#define SOFTMUTE    0x040
#define LIMEN       0x100

static int chip[0x40];
static int seq, last_at[0x40];
static bool playing, watching;
static int vol_db = -20, sc_db10;       /* what the test asked for */
static int fails, checks, watched;
/* inside apply(): the larger of the old and new curves' peaks; the cut
 * must cover it at every write. A half-written EQ (old and new bands
 * mixed, at most 4 writes) can peak higher: measured, not failed */
static int span_peak = -1, mix_excess;
static unsigned short span_new[5];      /* R18-R22 once the EQ is written */
static bool span_off;                   /* the EQ is being turned off */

static void expect(bool ok, const char *what)
{
    checks++;
    if (!ok)
    {
        printf("FAIL %s\n", what);
        fails++;
    }
}

/* the EQ now in R18-R22 */
static void chip_bands(struct dac_eq_band *b)
{
    for (int i = 0; i < 5; i++)
    {
        int r = chip[R18 + i];
        b[i].gain = 12 - (r & 0x1f);
        b[i].freq = (r >> 5) & 3;
        b[i].wide = i > 0 && i < 4 && (r & 0x100);
    }
}

static int dac_cut_half_db(void)    /* R11's attenuation, 0.5 dB units */
{
    return 255 - (chip[R11] & 0xff);
}

static void check_state(void)
{
    struct dac_eq_band b[5];
    char what[96];
    int amp_db = (chip[R52] & 0x3f) - 57;
    int cut2 = dac_cut_half_db();

    watched++;
    chip_bands(b);
    bool done = true;
    for (int i = 0; i < 5; i++)     /* off: every gain at 0 dB */
        done &= span_off ? (chip[R18 + i] & 0x1f) == 12
                         : chip[R18 + i] == span_new[i];
    int need = span_peak >= 0 && !done ? span_peak : eqlab_precut(b);

    if (2 * eqlab_precut(b) - cut2 > mix_excess)
        mix_excess = 2 * eqlab_precut(b) - cut2;
    snprintf(what, sizeof(what), "write %d: R11 cut %d.%d dB under the EQ "
             "peak %d dB", seq, cut2 / 2, cut2 % 2 * 5, need);
    expect(cut2 >= 2 * need, what);
    /* the amp may carry the volume, Sound Check's trim (rounded up to a
     * whole dB) and at most the DAC cut, never more */
    snprintf(what, sizeof(what), "write %d: amp %+d dB over volume %d, "
             "trim %d.%d, cut %d.%d", seq, amp_db, vol_db, sc_db10 / 10,
             sc_db10 % 10, cut2 / 2, cut2 % 2 * 5);
    expect(2 * amp_db <= 2 * vol_db + (sc_db10 + 9) / 5 + cut2 + 1, what);
}

void wmcodec_write(int reg, int data)
{
    reg &= 0x3f;
    chip[reg] = data & 0x1ff;
    last_at[reg] = ++seq;
    if (getenv("EQLEV_DEBUG")) printf("w%d R%d=0x%03x\n", seq, reg, chip[reg]);
    if (watching && reg != R25 && reg != R24)
        check_state();
}

void wmcodec_clocks_gate(bool gate)
{
    (void)gate;
}

bool pcm_is_playing(void)
{
    return playing;
}

static void bands_of(int p, struct dac_eq_band *b)
{
    for (int i = 0; i < 5; i++)
    {
        b[i].gain = eq_hw_presets[p].band[i].gain;
        b[i].freq = eq_hw_presets[p].band[i].freq;
        b[i].wide = eq_hw_presets[p].band[i].wide;
    }
}

/* one curve through the shell's path: eqlab_level() then dac_eq_set() */
static struct eqlab_level apply(const struct dac_eq_band *b)
{
    struct eqlab_level lv = { 0, 0, 0 };
    struct dac_eq_band old[5];

    if (b)
        eqlab_level(b, &lv);
    chip_bands(old);
    span_peak = eqlab_precut(old) > lv.precut_db ? eqlab_precut(old)
                                                 : lv.precut_db;
    for (int i = 0; i < 5; i++)     /* the registers this curve ends in */
        span_new[i] = !b ? 0 :
            ((12 - b[i].gain) & 0x1f) | (b[i].freq & 3) << 5 |
            (i == 0 || (i < 4 && b[i].wide) ? 0x100 : 0);
    span_off = !b;
    if (getenv("EQLEV_DEBUG"))
        printf("apply %s precut %d makeup %d\n", b ? "curve" : "off",
               lv.precut_db, lv.makeup_db10);
    dac_eq_set(b, lv.precut_db, lv.makeup_db10);
    span_peak = -1;
    return lv;
}

int main(void)
{
    struct dac_eq_band b[5];
    struct eqlab_level lv;
    struct dac_stats st;
    char what[96];
    /* Off -> boost presets -> a cut preset -> boosts -> Off, both ways */
    static const int walk[] = { 4, 1, 2, 18, 0, 20, 11, 4, 7, 15, -1, 13 };

    /* the chip's EQ reset values (0 dB): R18 0x12c, R19-R22 0x02c */
    chip[R18] = 0x12c;
    for (int i = 1; i < 5; i++)
        chip[R18 + i] = 0x02c;
    audiohw_preinit();
    audiohw_set_frequency(HW_FREQ_44);
    audiohw_postinit();
    audiohw_set_volume(vol_db * 10, vol_db * 10);
    dac_set_gain_db10(0, 60);
    expect(!(chip[R24] & LIMEN) && last_at[R24] == 0,
           "boot: R24 never written");

    watching = true;
    for (unsigned k = 0; k < sizeof(walk) / sizeof(walk[0]); k++)
    {
        int amp_db, cut2;

        if (walk[k] < 0)
        {
            apply(NULL);
            continue;
        }
        bands_of(walk[k], b);
        lv = apply(b);
        amp_db = (chip[R52] & 0x3f) - 57;
        cut2 = dac_cut_half_db();
        /* the precut on R11, plus the make-up's half dB (dac_gain.h:
         * the amp carries whole dB, the DAC the half below) */
        snprintf(what, sizeof(what), "preset %d: R11 cut %d half-dB, want %d",
                 walk[k] + 1, cut2, 2 * lv.precut_db);
        expect(cut2 == 2 * lv.precut_db || cut2 == 2 * lv.precut_db + 1,
               what);
        /* the total, tenth dB: volume - precut + make-up, to 0.5 dB */
        snprintf(what, sizeof(what), "preset %d: amp %+d dB, cut %d half-dB,"
                 " want volume %d - precut %d + make-up %d.%d", walk[k] + 1,
                 amp_db, cut2, vol_db, lv.precut_db, lv.makeup_db10 / 10,
                 lv.makeup_db10 % 10);
        expect(abs(10 * amp_db - 5 * cut2 - (10 * vol_db
                   - 10 * lv.precut_db + lv.makeup_db10)) <= 5, what);
        dac_get_stats(&st);
        expect(st.makeup_db10 == lv.makeup_db10 &&
               st.precut_db == lv.precut_db, "stats: precut and make-up");
        /* b18: the limiter allowed by default (DAC_LIMITER_DEFAULT 1) */
        expect(chip[R24] == 0x1a2, "limiter off by default with the EQ on");
    }
    expect(watched > 40, "the walk watched too few writes");

    /* Sound Check's trim and the make-up are one trim: -3 dB + Dance */
    sc_db10 = -30;
    dac_set_gain_db10(sc_db10, 60);
    bands_of(4, b);
    lv = apply(b);
    dac_get_stats(&st);
    snprintf(what, sizeof(what), "summed trim %d, want %d",
             st.gain_applied_db10, sc_db10 + lv.makeup_db10);
    expect(abs(st.gain_applied_db10 - (sc_db10 + lv.makeup_db10)) <= 5,
           what);
    /* at the Volume Limit the make-up is capped; the cut stays */
    sc_db10 = 0;
    dac_set_gain_db10(0, vol_db * 10);
    dac_get_stats(&st);
    expect(st.gain_applied_db10 == 0, "make-up capped at the Volume Limit");
    expect(dac_cut_half_db() == 2 * lv.precut_db,
           "the precut kept at the Volume Limit");
    expect((chip[R52] & 0x3f) - 57 == vol_db, "amp at the Volume Limit");
    dac_set_gain_db10(0, 60);
    watching = false;

    /* the limiter: not allowed, then allowed with the EQ on -> R25 then
     * R24 with LIMEN */
    dac_set_limiter_allow(false);
    expect(chip[R24] == 0x032, "limiter turned off with the EQ on");
    dac_set_limiter_allow(true);
    expect(chip[R24] == 0x1a2, "limiter allowed: R24 LIMEN, decay 768 ms");
    expect(chip[R25] == 0x000 && last_at[R25] && last_at[R25] < last_at[R24],
           "limiter: the -1 dB threshold before LIMEN");
    apply(NULL);
    expect(chip[R24] == 0x032, "EQ off: limiter off (register default)");
    bands_of(1, b);
    apply(b);
    expect(chip[R24] == 0x1a2, "EQ on again: limiter on");
    dac_set_limiter_allow(false);
    expect(chip[R24] == 0x032, "limiter not allowed: off");
    dac_get_stats(&st);
    expect(!st.limiter, "stats: limiter off");
    apply(NULL);

    /* AMUTE (#327): off by default, on when asked, kept across a rate
     * change and a mute/unmute, off again */
    expect(!(chip[R10] & AMUTE), "AMUTE off by default");
    dac_set_amute(true);
    expect(chip[R10] & AMUTE, "AMUTE on");
    audiohw_set_frequency(HW_FREQ_96);
    expect((chip[R10] & AMUTE) && !(chip[R10] & 0x008),
           "AMUTE kept at 96 kHz (64x)");
    audiohw_set_frequency(HW_FREQ_44);
    expect((chip[R10] & AMUTE) && (chip[R10] & 0x008),
           "AMUTE kept at 44.1 kHz (128x)");
    dac_set_amute(false);
    expect(!(chip[R10] & AMUTE) && (chip[R10] & 0x008), "AMUTE off again");

    printf("%d checks, %d failed (%d writes watched; a half-written EQ "
           "peaked %d.%d dB over its cut at most)\n", checks, fails, watched,
           mix_excess / 2, mix_excess % 2 * 5);
    return fails ? 1 : 0;
}
