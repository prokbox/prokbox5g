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

/* Boot battery guard (bootbatt_core.h has the why and the numbers).
 *
 * apps/main.c, HAVE_IPOD_UI only:
 *   bootbatt_sample()   target: right after the tick starts (i2c and the
 *                       PMU are up), before cpu_boost(true), the LCD's
 *                       re-init, storage or any write. Reads the raw
 *                       voltage (BB_SAMPLES fresh ADC conversions, a tick
 *                       apart) and the power inputs, decides stage 1 (the
 *                       floor), and seeds the power thread's filter with
 *                       that resting reading (powermgmt_seed_voltage).
 *                       RAM only.
 *   bootbatt_enforce()  right after lcd_init()/font_init(), before the
 *                       boot logo, storage, the mount, fonts, settings
 *                       and the VideoCore image. Below the floor on
 *                       battery: refuse (below). Otherwise a no-op.
 *   bootbatt_boost_allowed()  false after a stage-1 refusal: init() then
 *                       skips its boot-time cpu_boost(true) (a refusal
 *                       rescued by a cable takes it).
 *   bootbatt_latch_check()  stage 2, right after the mount and the #91
 *                       intent resolution (the sim: after the boot
 *                       logo), before any app opens a file: reads the
 *                       drained latch (one sector, no write). Drained on
 *                       battery: refuse. External power: clears it. A
 *                       missing record is made here, power present or
 *                       the cell over disk-safe.
 * Refusing: the glyph-only low-battery image (ui_sys_dead_battery())
 * held 2 s while the power inputs are polled; a cable plugged in
 * meanwhile boots (to charging; the latch is then cleared), else
 * backlight and LCD off (and the disk to standby after stage 2) and
 * power_off(), never returning. Nothing is written.
 *
 * apps/ipodui/ui_main.c, every main-loop pass (at least once a minute):
 *   bootbatt_service()  the display at 0 % (the filtered voltage at or
 *                       under percent_to_volt_discharge[0], the table in
 *                       use) on battery while writes are allowed and not
 *                       latched yet: writes "drained" (one sector in
 *                       place), when the disk is awake anyway or after
 *                       30 s (one wake). Once per discharge.
 *
 * apps/misc.c, HAVE_IPOD_UI only:
 *   bootbatt_latch_drained()  clean_shutdown(), the low-battery shutdown
 *                       (query_force_shutdown()), before its frame: the
 *                       backup, written only if not latched and still
 *                       over disk-safe (at the shutoff, normally skipped).
 *   bootbatt_power_seen()  SYS_CHARGER_CONNECTED (charger or USB): a
 *                       drained record still on disk is cleared, unless
 *                       a USB host owns the disk (bootbatt_service()
 *                       clears it whenever power is present and the
 *                       disk is ours, and bootbatt_usb_exit() at once).
 *
 * apps/ipodui/usbscreen.c, at the end of a USB session (the disk ours):
 *   bootbatt_usb_exit()  clears a drained record (USB was power) and
 *                       makes good a record the host deleted or
 *                       truncated.
 *
 * A missing or broken record is made again at boot, at USB exit, and
 * while the disk is awake anyway (backoff 1 min doubling to 64 min).
 * A charge the firmware never saw (Apple's firmware, a charger that did
 * not boot ours) clears it at stage 2: rest median at least
 * BB_RECHARGE_MV over the 0 % point (bootbatt_core.h).
 * The simulator calls sample and enforce back to back after font_init(). */
#ifndef _IPODUI_BOOTBATT_H_
#define _IPODUI_BOOTBATT_H_

#include <stdbool.h>

void bootbatt_sample(void);
bool bootbatt_boost_allowed(void);
void bootbatt_enforce(void);
void bootbatt_latch_check(void);
void bootbatt_latch_drained(void);
void bootbatt_service(void);
void bootbatt_usb_exit(void);
void bootbatt_power_seen(void);

#endif
