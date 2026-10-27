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
 * #38 integration fix-up (docs/phase-nav-brief.md): sim script command
 * "audio_stop", kept apart from sim_script.c (docs/process.md: new sim
 * commands get their own sim_cmd_*.c file). Registered from
 * sim_script.c's run_line() via sim_cmd_audio(); see sim_cmd_audio.c.
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
#ifndef SIM_CMD_AUDIO_H
#define SIM_CMD_AUDIO_H

#include <stdbool.h>

/* Handles this sim_script.c command:
 *
 *   audio_stop            calls audio_stop() directly on the script
 *                         thread, the same stop a real "Stop" action (or
 *                         the stock WPS noticing playback has already
 *                         ended) makes, without going through a button
 *                         press at all. Revision 3's global buttons
 *                         (apps/ipodui/ui_main.c) made a long-held PLAY
 *                         mean sleep from every screen, including the
 *                         shell screen a stop leaves on top once the
 *                         stock WPS's own long-press-to-stop exits (it
 *                         watches audio_status() every HZ/5 and returns
 *                         as soon as playback stops, well inside a
 *                         scripted "hold PLAY 1500"): the remainder of
 *                         that hold is then read by the shell's own
 *                         input loop, which sees a long-held PLAY and
 *                         calls sys_poweroff(), quitting the sim. A
 *                         script that only wants playback stopped uses
 *                         "audio_stop" instead of holding PLAY.
 *
 *   viz_tap <ms> [n] [sound|book|quiet]
 *                         (HAVE_VIZ_TAP) opens the Now Playing
 *                         visualizer's PCM tap (apps/ipodui/viz_tap.h) for
 *                         windows of n samples (512), reads it about every
 *                         30 ms for ms, closes it, and prints one summary
 *                         line, then "viz_tap ok" or "viz_tap failed":
 *                         "sound" wants at least half the reads fresh, none
 *                         torn and a peak of 100 or more; "book" wants the
 *                         open refused or every read flagged as a book;
 *                         "quiet" (stopped) an open tap and no fresh read
 *                         (every one silent or stale).
 *
 * Returns false for any other command. */
bool sim_cmd_audio(const char *cmd, char *argv[], int argc, int lineno);

#endif /* SIM_CMD_AUDIO_H */
