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

#ifndef IPODUI_UPDSCREEN_H
#define IPODUI_UPDSCREEN_H

#include <stdbool.h>

/* The library page (#237, #157; owner 2026-11-09: whenever the menu
 * shows after a cold boot or an unplug, all the music is there and
 * current). One page in two places, the 7G's "OK to Disconnect" look
 * (usb-ok-to-disconnect.png): the dark USB body, the orange disc, a
 * title, a rounded bar (200 x 11 at x 60, y 197) that fills with the
 * real library work (the count of a first build's tree, the walk's
 * entries, the commit's index steps, the load and the lists; never
 * back, full only when the work is over) and the step in plain words
 * under it, then the menu (a plain slide). A soft light travels along
 * the bar while work runs (#276: no step looks frozen). It redraws only
 * its band (between steps only the bar's rows), at most UPD_FPS times a
 * second; no allocation beyond its 48 B of the
 * screen arena, no storage access of its own.
 *
 * ui_upd_boot(): ui_main.c, once, at shell start. With no library on the
 * disk (a first install, or one that must be made again from nothing:
 * a cut first build) the page comes up at once over the main menu:
 * "Preparing Your Library", the disc with the sync arrows standing
 * still. It stays until the library is loaded (or, if nothing runs and
 * no library comes for 10 s, logs it and leaves). Owner 2026-11-09: on
 * battery below TAGCACHE_UI_BUILD_MIN_PCT (15 %) the build waits for a
 * charger ("Connect to power to finish your library."): one not started
 * does not start (tagcache.c), one under way stops at its next folder
 * (tagcache_ui_sleep_cut(), the sleep entry's cut) and runs again once
 * power is back; the bar holds meanwhile.
 *
 * ui_upd_after_usb(): input.c, right after the USB screen returned (the
 * framebuffer still shows it). Still no library: the boot page again.
 * Otherwise, for a session the computer wrote in: "OK to Disconnect"
 * with the ejected pictogram; for its first second it draws nothing of
 * its own (the USB frame stays) and work over by then pops it with no
 * slide, so a short update shows no page.
 *
 * Keys, the wheel and hold MENU do nothing while it is up (ui_upd_up());
 * hold PLAY still sleeps or powers off. While it is up the library build
 * is a dedicated job (upd_hold_sync(): one hold, given back for a sleep
 * or a USB session, ui_upd_yield()). A new USB session replaces it. UI
 * thread only.
 * Trace: "upd push mode=", "upd show ms=", "upd skip ms=", "upd draw
 * bar= step=", "upd done ms=", "upd stuck ms=", "upd usb ms=", "upd key
 * ignored", "upd hold take|give why="; never "upd bar back". */
#define UPD_FPS 8

/* p081-pageboost: the page's build hold (a boost) given back before a
 * sleep's entry (sleep.c) and before the USB handler runs (input.c), and
 * taken again at the wake, or after a plug that ran no session, if the
 * page is still up. UI thread only; no I/O (the USB entry's rule) */
#define UPD_YIELD_SLEEP 0x01
#define UPD_YIELD_USB   0x02
void ui_upd_yield(unsigned what, bool yield);

void ui_upd_boot(void);
void ui_upd_after_usb(void);
bool ui_upd_up(void);

#endif /* IPODUI_UPDSCREEN_H */
