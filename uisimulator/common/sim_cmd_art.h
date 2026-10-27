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
 * Phase 4b sim script commands (docs/phase4b-brief.md), kept apart from
 * sim_script.c as its own file (docs/process.md: "new sim commands get
 * their own sim_cmd_*.c file where practical").
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
#ifndef SIM_CMD_ART_H
#define SIM_CMD_ART_H

#include <stdbool.h>

/* Handles these sim_script.c commands (see its header for the full
 * command list):
 *
 *   usb_connect / usb_disconnect   SYS_USB_CONNECTED / SYS_USB_DISCONNECTED
 *                                  (sim_trigger_usb(), the same path the
 *                                  SDL window's own USB key toggle uses,
 *                                  firmware/target/hosted/sdl/button-sdl.c)
 *   art_wait [ms]                  block until the art cache builder is
 *                                  idle (default 10000 ms). Calls the
 *                                  weak hook sim_art_builder_idle(), so
 *                                  this links before Agent B's builder
 *                                  (apps/ipodui/artcache.c) exists; until
 *                                  then (or in a build without it) this
 *                                  logs "art_wait unavailable" and
 *                                  returns at once
 *   power_off_now                  sys_poweroff() on the script thread:
 *                                  the same SYS_POWEROFF broadcast a
 *                                  power-button press makes
 *                                  (firmware/powermgmt.c), reaching
 *                                  apps/misc.c's clean_shutdown() mid-
 *                                  operation rather than only at "quit"
 *
 *   art_request                    artcache_build_request(), as a
 *                                  database load or precompute makes it
 *   disk_active <ms>               the art builder sees storage awake
 *                                  for 'ms' (sim_disk_active_until; the
 *                                  hostfs sim has no disk state), as if
 *                                  an audio refill had woken the flash:
 *                                  it may do I/O while playing only then
 *   expect_art <manifest>          every album of the art builder's table
 *                                  (sim_art_report() in apps/ipodui/
 *                                  artcache.c) has its manifest state
 *                                  (tests/ui/art_manifest.txt: path, OK /
 *                                  NOART / FAILED; path relative to the
 *                                  script) and none is still pending:
 *                                  "expect_art ok", else "expect_art
 *                                  failed" (a failure for shots.sh)
 *
 *   art_panel_hold <ms>            the art builder keeps its next panel
 *                                  decode block 'ms' longer (a playback
 *                                  start while it is held)
 *   audio_buf [mark|expect]        logs the audio buffer's size
 *                                  (audio_get_filebuflen()); "mark"
 *                                  remembers it, "expect" fails
 *                                  ("expect_audio_buf failed") when it is
 *                                  smaller now
 *
 * Returns false (and does nothing) for any other command, so
 * sim_script.c's run_line() falls through to its own "bad command".
 */
bool sim_cmd_art(const char *cmd, char *argv[], int argc, int lineno);

#endif /* SIM_CMD_ART_H */
