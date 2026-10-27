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
 * iPod UI shell
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
#ifndef IPODUI_CLICK_H
#define IPODUI_CLICK_H

#include <stdbool.h>
#include "config.h"

/* The Clicker's headphone click (sound and level: clicktone.h). Plays on
 * the mixer's beep channel, like upstream's keyclick, so it wakes the DAC
 * from standby the same way (pcm_mixer.c dac_wake()). Called by
 * system_sound_play(SOUND_KEYCLICK) (apps/misc.c) in place of beep_play(). */
void ipodui_click_play(void);

/* P19 (UI on the COP): the audio output stack (the mixer, the PCM driver
 * and its lock, the beep buffers) is CPU-only code, so a system sound the
 * split-off ui thread asks for is posted to the back end's service thread
 * (be_svc, CPU) and played there. Called first thing by
 * system_sound_play(): true when the sound was posted (the caller returns),
 * false when it is to be played here (inline, or already on the CPU's
 * service thread). Asynchronous: the ui thread never waits for it.
 * Builds without the split (release) play every sound where asked. */
#ifdef HAVE_UI_THREAD_SPLIT
bool ipodui_sound_route(int sound);
#else
static inline bool ipodui_sound_route(int sound)
{
    (void)sound;
    return false;
}
#endif

#endif /* IPODUI_CLICK_H */
