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
 * Video player sim script commands (HAVE_BCMFW_VIDEO builds; #64).
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
#ifndef SIM_CMD_VIDEO_H
#define SIM_CMD_VIDEO_H

#include <stdbool.h>

/* Handles these sim_script.c commands (documented in tests/ui/README.md):
 *
 *   video_start <path>    starts the video player on a sim path (before
 *                         play_files <path> starts its audio)
 *   audio_pause on|off    pauses or resumes the audio (which the video
 *                         follows) directly, not through the shell's keys
 *   video_stop            ends it; the normal screen comes back
 *   video_shot <name>     the picture on the panel, as shot <name> does
 *                         for the framebuffer
 *   expect_video <min> [state]   at least <min> pictures shown, no decode
 *                         or protocol errors, and the player's state
 *                         (playing, asleep, done, failed, idle);
 *                         "expect_video ok|failed"
 *
 *   video_screen <path> [tc]   ui_video_play(path, tc): the shell's video
 *                         screen (tc = 0-based tagcache master index,
 *                         default -1); HAVE_IPOD_UI builds
 *   setvideos             ui_setvideos_push(): Videos > Settings
 *
 * Returns false for any other command (and for all of them in builds
 * without HAVE_BCMFW_VIDEO). */
bool sim_cmd_video(const char *cmd, char *argv[], int argc, int lineno);

#endif /* SIM_CMD_VIDEO_H */
