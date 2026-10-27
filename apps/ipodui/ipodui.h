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
#ifndef _IPODUI_H_
#define _IPODUI_H_

#include <stdbool.h>
#include "gcc_extensions.h"

/* Entry point of the iPod UI shell. Called from main() instead of
 * root_menu() when HAVE_IPOD_UI is defined. Never returns (noreturn, so
 * main(), itself noreturn on target, does not warn). */
void ipodui_main(void) NORETURN_ATTR;

/* Shell-internal (ui_main.c), used by the menus. */

/* Hands the screen to the stock WPS until it returns. Resumes playback
 * first if nothing is playing; does nothing if there is nothing to resume. */
void ui_show_wps(void);
/* Starts the saved resume point if audio is stopped, silently (no prompts).
 * Returns true if audio is (now) playing or paused. */
bool ui_playback_resume(void);
/* Hold NEXT/PREV: the audio ms a seek covers after 'held' ticks (the
 * video screen runs its own hold, with the strip, on the same curve) */
unsigned long ui_seek_span_ms(long held);
/* "Now Playing" is shown while audio runs or a resume point exists. */
bool ui_now_playing_visible(void);
/* Shuffles all music in the library (lib_shuffle_all) and shows the WPS. */
void ui_shuffle_songs(void);

/* apps/misc.c check_bootfile(): the firmware file changed while the computer
 * had the disk. Pushes the native "New Software" prompt (Restart | Later;
 * screens/newsw.c); replaces the stock yes/no prompt. */
void ui_bootfile_changed(void);

#endif /* _IPODUI_H_ */
