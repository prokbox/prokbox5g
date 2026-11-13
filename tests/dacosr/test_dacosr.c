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

/* p083-dacosr64: host test of the WM8758 driver's R10 (DAC control)
 * oversampling bit per output rate.  Compiles the real
 * firmware/drivers/audio/wm8758.c against tests/dacosr/stub and keeps a
 * model of the chip: the last value written to each register (the I2C
 * bus is write-only, so this is all the chip ever sees).
 *
 * Rule under test: R10 DACOSR (bit 3) is 0 (64x) at 88.2/96 kHz and 1
 * (128x) at 48 kHz and below, on every path that sets a rate: playback,
 * the self-test's 88.2 -> 96 -> back, the runtime fallback's return to a
 * rated rate, a rate set while soft-muted (applied at the unmute) and
 * while in DAC standby (applied at the wake, muted until then).
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

static int chip[0x40];
static int r10_writes;
static bool playing;

void wmcodec_write(int reg, int data)
{
    chip[reg & 0x3f] = data & 0x1ff;
    if (reg == DACCTRL)
        r10_writes++;
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

#define R10_OSR128  0x008       /* the literal bits, not the header's */
#define R10_MUTE    0x040

static void expect_r10(const char *what, int want)
{
    checks++;
    if (chip[DACCTRL] != want) {
        printf("FAIL %s: R10 0x%03x, want 0x%03x\n", what, chip[DACCTRL],
               want);
        fails++;
    }
}

static void expect_stats(const char *what, int osr, unsigned long hz)
{
    struct dac_stats st;

    dac_get_stats(&st);
    checks++;
    if (st.osr != osr || st.rate_hz != hz) {
        printf("FAIL %s: stats osr=%d rate=%lu, want osr=%d rate=%lu\n",
               what, st.osr, st.rate_hz, osr, hz);
        fails++;
    }
}

static int want_play(int fsel)
{
    return hw_freq_sampr[fsel] > 48000 ? 0 : R10_OSR128;
}

static void enter_standby(void)
{
    playing = false;
    current_tick += DAC_IDLE_MS * HZ / 1000 + 1;
    dac_idle_poll();
}

int main(void)
{
    char what[96];

    /* boot: preinit, the rated rate, postinit's unmute */
    audiohw_preinit();
    audiohw_set_frequency(HW_FREQ_44);
    audiohw_postinit();
    expect_r10("boot 44.1k", R10_OSR128);
    expect_stats("boot 44.1k", 128, 44100);

    /* every rate, unmuted, PCM stopped (as pcm_apply_settings runs) */
    for (int f = 0; f < HW_NUM_FREQ; f++) {
        dac_wake();
        audiohw_set_frequency(f);
        snprintf(what, sizeof what, "play %lu", hw_freq_sampr[f]);
        expect_r10(what, want_play(f));
        expect_stats(what, want_play(f) ? 128 : 64, hw_freq_sampr[f]);
    }

    /* the self-test: 88.2, 96, then the rated rate back */
    dac_wake();
    audiohw_set_frequency(HW_FREQ_44);
    audiohw_set_frequency(HW_FREQ_88);
    expect_r10("self-test 88.2k", 0);
    audiohw_set_frequency(HW_FREQ_96);
    expect_r10("self-test 96k", 0);
    audiohw_set_frequency(HW_FREQ_44);
    expect_r10("self-test back to 44.1k", R10_OSR128);

    /* the runtime fallback: playing 96k, the repick goes to 48k */
    audiohw_set_frequency(HW_FREQ_96);
    playing = true;
    dac_idle_poll();
    expect_r10("fallback: playing 96k", 0);
    playing = false;
    audiohw_set_frequency(HW_FREQ_48);
    expect_r10("fallback: repicked 48k", R10_OSR128);
    expect_stats("fallback: repicked 48k", 128, 48000);

    /* standby at 96k: muted, the OSR kept; wake: 64x unmuted */
    audiohw_set_frequency(HW_FREQ_96);
    enter_standby();
    expect_r10("standby at 96k", R10_MUTE);
    dac_wake();
    expect_r10("wake at 96k", 0);

    /* a rate set while in standby: stays muted, the wake applies it */
    enter_standby();
    audiohw_set_frequency(HW_FREQ_44);
    expect_r10("standby, 44.1k set", R10_MUTE | R10_OSR128);
    dac_wake();
    expect_r10("wake at 44.1k", R10_OSR128);
    enter_standby();
    audiohw_set_frequency(HW_FREQ_96);
    expect_r10("standby, 96k set", R10_MUTE);
    dac_wake();
    expect_r10("wake at 96k after standby set", 0);
    dac_wake();
    audiohw_set_frequency(HW_FREQ_44);

    /* stop: audiohw_close() mutes; a later rate set writes no R10 */
    audiohw_close();
    expect_r10("close", R10_MUTE);
    int before = r10_writes;
    audiohw_set_frequency(HW_FREQ_96);
    checks++;
    if (r10_writes != before || chip[DACCTRL] != R10_MUTE) {
        printf("FAIL rate set while muted wrote R10 0x%03x\n",
               chip[DACCTRL]);
        fails++;
    }
    /* the next start (preinit sets 44.1k itself): the unmute writes 128x,
       not the 64x left from the muted 96k set */
    audiohw_preinit();
    audiohw_postinit();
    expect_r10("restart after a muted 96k set", R10_OSR128);
    expect_stats("restart after a muted 96k set", 128, 44100);
    audiohw_set_frequency(HW_FREQ_96);
    expect_r10("restarted, 96k", 0);

    if (checks < 40) {
        printf("FAIL only %d checks ran\n", checks);
        return 1;
    }
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
