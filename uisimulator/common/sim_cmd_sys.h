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
 * Phase 8a system-screen sim commands (docs/phase8a-brief.md section 8).
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
#ifndef SIM_CMD_SYS_H
#define SIM_CMD_SYS_H

#include <stdbool.h>

/* Handles these sim_script.c commands (all log "sim_script: <cmd> ..."):
 *
 *   hold_switch on|off   set the Hold switch to a state (idempotent; the
 *                        script key HOLD toggles). button-sdl.c's
 *                        button_read_device() drops every key while Hold
 *                        is on and calls backlight_hold_changed() on the
 *                        change, as clickwheel hardware does; the command
 *                        then waits 3 ticks so that poll has run.
 *   backlight_expire     backlight_off(): the same BACKLIGHT_OFF path as
 *                        the idle timeout (firmware/backlight.c). A timeout
 *                        of 0 (Always On, the sim default) or Off is first
 *                        set to 15 s (battery and plugged), since the
 *                        SYS_IPODUI_BACKLIGHT_OFF hook only fires for a
 *                        positive timeout (brief Rev 1 M2).
 *   usb_glyph usbc|dock30|auto
 *                        pins which idle connector glyph the USB screen
 *                        draws (sim_usb_glyph_pin below).
 *   splash_probe <text>  calls splashf(0, "%s", text) from the script
 *                        thread ("%20" stands for a space). In a shell
 *                        build the funnel suppresses and logs it.
 *   yesno_probe <text>   calls gui_syncyesno_run() with <text> from the
 *                        script thread (shell builds only). The funnel
 *                        logs "yesno-suppressed <text>" and answers No;
 *                        prints "yesno_probe <text> -> no|yes|other".
 *   capture <name>       sim_capture_screen(name): the call show_logo_boot()
 *                        makes; writes <outdir>/<name>.png when the script
 *                        has "# boot_shot", else does nothing.
 *   storage_idle         call_storage_idle_notifys(true): the stock idle
 *                        path (the uilog flush hangs off
 *                        DISK_EVENT_SPINUP, not this).
 *   powerlog_mark <tag>  powerlog_mark(tag) (developer builds, tracker
 *                        #10): one tagged row in the power log, as test
 *                        code on the device would mark a phase; prints
 *                        "powerlog_mark <tag> on=0|1".
 *   commit_hold <point>  the next tagcache commit waits at <point> (0:
 *                        before its USB check, nothing changed yet; 1: just
 *                        after the dirty mark; k + 1: after the tag file of
 *                        commit step k is rewritten) until commit_release.
 *   commit_hold_wait [ms] wait (default 30000) until the commit is held
 *                        there: "commit_hold_wait ok", else
 *                        "expect_commit_hold failed".
 *   commit_release       lets a held commit go on.
 *   commit_cancel        the cancel a power-off raises in a commit, without
 *                        the power-off (as one that is then aborted).
 *   db_count_mark        remember the database's entry count.
 *   expect_db_count <d> [ms]
 *                        wait (default 60000) until the database is usable
 *                        with exactly mark + d entries: "expect_db_count ok
 *                        <n>", else "expect_db_count failed".
 *
 * "chord" lives in sim_script.c (it predates this file). */
bool sim_cmd_sys(const char *cmd, char *argv[], int argc, int lineno);

/* Sim-only pin for the USB idle glyph, defined in sim_cmd_sys.c and read
 * by apps/ipodui/usbscreen.c (U1) under SIMULATOR:
 *   -1 = not pinned (use UI_USB_IDLE_GLYPH)
 *    0 = UI_USB_GLYPH_DOCK30, 1 = UI_USB_GLYPH_USBC (enum ui_usb_glyph). */
extern int sim_usb_glyph_pin;

#endif /* SIM_CMD_SYS_H */
