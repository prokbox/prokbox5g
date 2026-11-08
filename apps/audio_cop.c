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

/* Audio output on the COP (0.8.0-rc.6, owner 2026-11-07): Apple's core
 * layout for the audio pipeline's end.  The PCM DMA's FIQ (source 26) and
 * pcm-pp.c's refill run on the COP, as bench rows p1 and pcop proved on
 * silicon (sitting 4gh-b8); the CPU keeps the UI, the file system, storage,
 * USB and the BCM driver.  docs/design/cop-shared-state.md (layout A) lists
 * what the two cores share.
 *
 * At boot, after audio_init() and before anything can play, the PCM is
 * moved to the COP and proved there: a few milliseconds of silence play
 * and the COP must take their FIQs.  If the move is refused or no FIQ
 * arrives, the PCM goes back to the CPU for the boot and the log says
 * "COP FAILED (pcm ...): audio output on the CPU": a loud safety net,
 * never the plan. */
#include "config.h"

#ifdef HAVE_CORE_LAYOUT_A
#include <stdbool.h>
#include <stdint.h>
#include "system.h"
#include "kernel.h"
#include "logf.h"
#include "panic.h"
#include "pcm.h"
#include "pcm-internal.h"
#include "dac_ext.h"
#include "corelayout.h"
#include "xcore.h"
#include "audio_cop.h"
#ifdef HAVE_IPOD_UI
#include "ipodui/uilog.h"
#endif

#define AC_PIECE    256             /* bytes: 1.45 ms at 44.1 kHz */
#define AC_WANT     8               /* FIQs on the COP to call it proved */
#define AC_WAIT_MS  200

/* Zero silence, uncached: DMA0 reads SDRAM and no core writes it */
static uint8_t ac_silence[AC_PIECE] NOCACHEBSS_ATTR __attribute__((aligned(16)));
static volatile int ac_more XCORE_BSS;   /* the FIQ (COP) writes it */

static void ac_get_more(const void **start, size_t *size)
{
    ac_more++;
    *start = ac_silence;
    *size = AC_PIECE;
}

static enum pcm_dma_status ac_status(enum pcm_dma_status status)
{
    return status;
}

static enum audio_cop_state ac_state = AUDIO_COP_CPU;

static void ac_say(const char *fmt, unsigned long a, unsigned long b)
{
#ifdef HAVE_IPOD_UI
    ui_log("pcm", fmt, a, b);
#endif
    logf(fmt, a, b);
}

void audio_cop_init(void)
{
    struct pcm_pp_stats s0, s1;
    bool dac_was;
    long end;

    if (pcm_pp_set_core(COP) != 0)
    {
        ac_state = AUDIO_COP_FAILED;
        ac_say("COP FAILED (pcm move refused %lu %lu): audio output on the CPU",
               0, 0);
        return;
    }
    dac_was = dac_standby_allowed();
    dac_standby_allow(false);       /* the codec clocks I2S: it must run */
    pcm_pp_get_stats(&s0);
    ac_more = 0;
    pcm_play_data(ac_get_more, ac_status, ac_silence, AC_PIECE);
    end = current_tick + (AC_WAIT_MS * HZ + 999) / 1000;
    do
    {
        sleep(1);
        pcm_pp_get_stats(&s1);
    }
    while (s1.fiq[COP] - s0.fiq[COP] < AC_WANT &&
           TIME_BEFORE(current_tick, end));
    pcm_play_stop();
    dac_standby_allow(dac_was);

    if (s1.fiq[COP] - s0.fiq[COP] >= AC_WANT &&
        s1.fiq[CPU] == s0.fiq[CPU])
    {
        ac_state = AUDIO_COP_ON;
        ac_say("pcm on cop: proved fiq=%lu more=%lu",
               (unsigned long)(s1.fiq[COP] - s0.fiq[COP]),
               (unsigned long)ac_more);
        return;
    }
    ac_state = AUDIO_COP_FAILED;
    if (pcm_pp_set_core(CPU) != 0)
        panicf("pcm: neither core takes the FIQ");
    ac_say("COP FAILED (pcm no fiq: cop=%lu cpu=%lu): audio output on the CPU",
           (unsigned long)(s1.fiq[COP] - s0.fiq[COP]),
           (unsigned long)(s1.fiq[CPU] - s0.fiq[CPU]));
}

enum audio_cop_state audio_cop_state(void)
{
    return ac_state;
}
#endif /* HAVE_CORE_LAYOUT_A */
