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
 * iPod UI shell: the first-boot legal notice
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
#ifndef IPODUI_NOTICE_H
#define IPODUI_NOTICE_H

#include <stdbool.h>
#include <stdint.h>

/* The first-boot notice (docs/legal.md section 2, "On device: the
 * first-boot notice"; GPL v2 section 2(c)): once after each new build,
 * the Legal screen in its notice mode (screens/legal.c) is the shell's
 * first frame (over the Main Menu, which never shows behind it; nothing
 * plays at boot) until Select dismisses it. Select is ignored for its
 * first 3 s; every other button does nothing.
 *
 * Keyed on a hash of the build id (rbversion), stored in the hidden
 * setting ipodui_notice_ack (0: never acknowledged). Nothing is written
 * until the dismissal, and that write is the normal deferred settings
 * write (settings_save(), with the next idle flush). A USB session while
 * the notice is up acknowledges nothing: it comes back next boot.
 *
 * Simulator: a scripted run (tests/ui) never shows it, so no script's
 * first frame changes, unless "# env: IPODUI_SIM_NOTICE=1" (the stored
 * ack decides) or "=clear" (the first boot of the run clears the stored
 * ack; a "reboot" part keeps what was written). The hash then uses the
 * pinned build id About shows, so it doesn't follow commits. */

/* the arg that opens ui_legal_screen in notice mode */
#define UI_LEGAL_NOTICE ((const void *)(intptr_t)1)

/* At shell start, after the root (and a locked screen): push the notice
 * when this build has not been acknowledged. Up at once, no slide. */
void ui_notice_boot(void);

/* true while the notice is the top screen: the main loop keeps the
 * global holds (MENU to the root, PLAY to sleep) off it */
bool ui_notice_up(void);

/* The notice's Select: record the acknowledgement (deferred write),
 * unless a USB session ran while it was up. The caller pops. */
void ui_notice_ack(void);

#endif /* IPODUI_NOTICE_H */
