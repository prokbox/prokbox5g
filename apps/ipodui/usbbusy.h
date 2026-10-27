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

#ifndef IPODUI_USBBUSY_H
#define IPODUI_USBBUSY_H

#include <stdbool.h>
#include "ipodui/header.h"

/* #84: the busy moment after a USB disconnect. The post-USB library scan
 * (and the read-only housekeeping that rides in its awake window) shows
 * the 8-dot activity glyph in the header (Q7) and nothing else: no
 * screen, no splash; the library stays browsable on the published
 * generation (#85) the whole time. UI thread only; RAM only, no storage
 * access of its own.
 *
 * ui_usbbusy_begin(): usbscreen.c, at the disconnect, before
 *   system_restore_db() posts the scan. Starts a grace window for the
 *   scan to begin.
 * 3.20 (2026-10-25): the same glyph for any library update, whatever
 *   started it (tagcache_ui_updating(), dbq_working(), Build Artwork):
 *   up once the work has run 1 s, then at least 1 s; phases under 1 s
 *   apart are one run. Seen by the poll only (no event, no extra wake).
 *   Lit work only: dark drops the glyph (ui_usbbusy_dark()), and the
 *   first lit pass after restarts the 1 s count (a wake into work about
 *   to end shows nothing).
 * ui_usbbusy_poll(): ui_main.c draw_dirty(), each header pass. Sets
 *   st->busy (0 none, 1..8 the glyph's lead dot) and returns the ticks
 *   until the glyph next steps, or TIMEOUT_BLOCK. When the moment ends it
 *   runs the housekeeping once: the "._" sidecar count the scan saw and
 *   the FSInfo plausibility check, both only logged (Q6: nothing is
 *   deleted, nothing written to the volume). */
void ui_usbbusy_begin(void);
int  ui_usbbusy_poll(struct ui_header_state *st, bool lit);
/* the screen went dark: input.c (the backlight-off event), sleep.c (a
 * sleep's dark entry); UI thread */
void ui_usbbusy_dark(void);

/* The snapshot batch (modload.h, #106; docs/phase16-brief.md 4.2) is the
 * same routine the boot window runs. It does not exist yet: this is the
 * hook, called once when the busy moment ends. */
#endif /* IPODUI_USBBUSY_H */
