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
#ifndef IPODUI_SYSSCREEN_H
#define IPODUI_SYSSCREEN_H

#include <stdbool.h>
#include "ipodui/screen_stack.h"

/* Phase 11 system screens (docs/phase11-brief.md sections 3.7, 4.6).
 * Contract, committed before fan-out; sysscreen.c (U2b) implements them.
 *
 * Hooks U2b adds after the contract commit:
 *   apps/ipodui/ui_main.c global_hold(), hold PLAY with charger_inserted():
 *     charging_sleep_prepare();
 *     ui_push_t(&ui_sys_charging_screen, NULL, UI_T_SYSTEM) (8b-4a: a
 *     system screen keeps the plain slide, never the split parting);
 *     the sleep_on_release branch: clear the flag, swallow the release; no
 *     backlight_off() / ui_unlit_suppress_next() any more. (The Time in
 *     Title apply at shell start, Rev 1 M-7, is already in ui_main.c from
 *     the contract commit.)
 *   apps/misc.c, guarded #ifdef HAVE_IPOD_UI, the ONLY misc.c edits
 *     (Rev 1 L-10): in clean_shutdown()'s two low/empty DEBUGF branches,
 *     ui_sys_low_battery(empty). charging_splash() and list_stop_handler()
 *     are not touched (dead in the shell).
 *   firmware/usbstack/usb_storage.c, guarded #ifdef HAVE_IPOD_UI: the
 *     one read-only getter, usb_storage_all_ejected() below (U2b may add
 *     the same prototype to firmware/usbstack/usb_storage.h, guarded).
 *     The sim has no USB stack: its getter is in
 *     uisimulator/common/sim_cmd_p11.c, set by the "usb_eject" command.
 *
 * Trace: "sys charging state=<charging|charged>",
 *        "sys charging pop=<key|unplug|usb>",
 *        "sys lowbatt empty=<0|1> drawn=<0|1>", "usb ejected". */

/* An ordinary pushed screen (Rev 1 H-1): timeout() 10 s lit,
 * TIMEOUT_BLOCK dark; pops on a key (swallowed), charger removal, USB.
 * No nested loop. arg: NULL. */
extern const struct ui_screen_ops ui_sys_charging_screen;
/* UI thread (clean_shutdown via sys_poweroff): backlight on first, skip
 * the draw if the LCD stays asleep; else draw + lcd_update + 2 s; never
 * allocates. */
void ui_sys_low_battery(bool empty);   /* draws, returns at once */
void ui_sys_low_battery_hold(void);    /* after the save: keep it up ~2 s */
/* The boot guard's screen (apps/ipodui/bootbatt.c): right after
 * lcd_init(), glyphs only (no font, no disk, no backlight driver); draws
 * and returns, the guard holds it. Trace: "sys deadbatt drawn=1". The
 * one call that picks the design, so a new "dead battery" screen drops
 * in here. */
void ui_sys_dead_battery(void);

/* usb_storage.c (target) / sim_cmd_p11.c (sim): every LUN has
 * ejected[lun] set. Read-only, no lock (a stale read costs one HZ/2 poll
 * of usbscreen.c's idle wake). */
bool usb_storage_all_ejected(void);

/* -- U2b's own additions (sysscreen.c; Brightness uses the moon and sun) -- */

/* The system glyphs (art/sys/sysscreen.py, 4-bit coverage) */
enum ui_sys_glyph { UI_SYS_BOLT, UI_SYS_PLUG, UI_SYS_WARN, UI_SYS_MOON,
                    UI_SYS_SUN, UI_SYS_GLYPHS };
/* Blends glyph g in 'colour' over the framebuffer at x, y (no push) */
void ui_sys_glyph(enum ui_sys_glyph g, int x, int y, unsigned colour);
int  ui_sys_glyph_w(enum ui_sys_glyph g);
int  ui_sys_glyph_h(enum ui_sys_glyph g);

#endif /* IPODUI_SYSSCREEN_H */
