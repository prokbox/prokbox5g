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
 * iPod UI shell: the Clicker's headphone click (click.h, clicktone.h)
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
#include "config.h"
#include "system.h"
#include "settings.h"     /* global_status.volume: dB */
#include "pcm_mixer.h"
#include "ipodui/click.h"
#include "ipodui/clicktone.h"
#include "ipodui/fe_be.h"     /* fe_be_route(), be_post() */

/* Rendered once per mixer rate (768 B at 48 kHz). One caller thread: the
 * UI's, or with the UI split off (P19) be_svc on the CPU, through
 * ipodui_sound_route(); the channel is stopped before a re-render, so the
 * mixer never reads a half-written buffer. CPU only in the split: the
 * mixer's FIQ reads the buffer through the CPU's cache. */
static int16_t click_buf[CLICK_FRAMES_MAX * 2] __attribute__((aligned(4)));
static unsigned int click_fs;
static int click_frames;

void ipodui_click_play(void)
{
    unsigned int fs = mixer_get_frequency();

    mixer_channel_stop(PCM_MIXER_CHAN_BEEP);

    if (fs != click_fs)
    {
        click_frames = click_render(click_buf, CLICK_FRAMES_MAX, fs);
        click_fs = fs;
    }
    if (click_frames <= 0)
        return;

    mixer_channel_set_amplitude(PCM_MIXER_CHAN_BEEP,
                                click_amp(global_status.volume));
    mixer_channel_play_data(PCM_MIXER_CHAN_BEEP, NULL, click_buf,
                            click_frames * 2 * sizeof (int16_t));
}

#ifdef HAVE_UI_THREAD_SPLIT
bool ipodui_sound_route(int sound)
{
    if (!fe_be_route())
        return false;
    be_post(BE_SOUND, sound);
    return true;
}
#endif
