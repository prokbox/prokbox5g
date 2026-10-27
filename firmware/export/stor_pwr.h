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

/* Drive power (#114, P18 A3 and 18-2): the one power-down / power-up path
 * for the storage adapter, used by the idle policy in the driver, the
 * shell's sleep entry and USB connect. HAVE_STORAGE_POWEROFF builds only
 * (the shell target); docs/storage-choices.md has the tuning's reasons.
 *
 *   storage_power_down(reason, flags)
 *       Waits for any transfer under way (the driver's lock), then FLUSH
 *       CACHE, STANDBY IMMEDIATE and the power off. Returns
 *          0  off;
 *          1  standby only (the developer switch says so; still flushed);
 *         -1  not cut: the cache is not known safe (the flush failed, or
 *             a write cache with no FLUSH CACHE offered; #114 M2); the
 *             power stays on, logged as a "dirty" event;
 *         -2  neither the flush nor STANDBY IMMEDIATE worked: powered;
 *         -3  refused: USB owns the drive.
 *       With STOR_PWR_KEEP_LOCK the lock stays held whatever the result
 *       (the sleep entry: nothing may start a transfer in the dark), until
 *       storage_power_release(). Without it (playback, idle) the lock is
 *       dropped and the next access from any thread powers the drive up
 *       transparently.
 *   storage_power_release()
 *       Ends a STOR_PWR_KEEP_LOCK power-down (the wake, or an abandoned
 *       entry). Does not power on: the first access does.
 *   storage_power_up(reason)
 *       Powers on and re-initialises at once (>= 250 ms, reset, IDENTIFY,
 *       SET FEATURES, timings), one retry. 0, or < 0 on failure: the
 *       power is then cut again and the drive left off, so the next
 *       access tries afresh (a read or write fails with the same error,
 *       after the two attempts, never a wait on a drive half up). After
 *       STOR_PWR_FAIL_MAX failed power-ups in a row the idle cut is
 *       switched off for the session (storage_poweroff_allowed() false;
 *       logged, nothing shown). Never call it with the lock held by a
 *       KEEP_LOCK power-down.
 *   storage_powered()
 *       The adapter has power (on or in standby).
 *   storage_power_warm()
 *       An access now costs no power-up and no spin-up: the drive is on,
 *       or a solid one is in standby with its power (inside the hold).
 *       Work that may wait for a drive awake anyway (the statistics
 *       drain) asks this, not storage_disk_is_active(), whose "active"
 *       ends a second after the last transfer, long before the cut.
 *   storage_set_poweroff(bool) / storage_poweroff_allowed()
 *       The developer switch: false keeps the power on (standby only),
 *       both awake and at sleep entry. Not saved by the driver.
 *   storage_power_state()
 *       The counters and event ring (stor_pwr_core.h), for the log; read
 *       without a lock (a torn read costs one log line). */
#ifndef STOR_PWR_H
#define STOR_PWR_H

#include <stdbool.h>
#include "config.h"
#include "stor_pwr_core.h"

#ifdef HAVE_STORAGE_POWEROFF

#define STOR_PWR_KEEP_LOCK  0x1

/* Flash (solid) tuning, in seconds; a rotating drive keeps upstream's
 * policy (standby after the spin-down time, power off 2 s later).
 * HOLD_DARK: counted from the last access of any thread (the driver's
 *   last activity), so reads that follow a fill (art read-ahead, the
 *   statistics drain, the idle notices' writes) extend it. Longer than
 *   any gap inside a refill or a track change's reads (well under 1 s)
 *   and than the 1 s storage tick plus the tagcache thread's 1 s poll
 *   (the drain must see it powered: storage_power_warm()); far shorter
 *   than a FLAC refill interval (5-6 min with the 39 MB buffer). 3 s
 *   (owner, 2026-10-23; was 10 s): 7 s less powered per cycle; a skip
 *   past the buffered end soon after a refill may cost one extra
 *   power-up.
 * HOLD_LIT: browsing reads come in bursts tens of seconds apart; a
 *   0.3-0.5 s power-up in the middle of a scroll would show.
 * MIN_OFF: an off period shorter than this cost more than it saved
 *   [hypothesis: power-up ~0.4 s at ~100 mA against a standby draw of a
 *   few mA]; each one doubles the hold, up to HOLD_MAX.
 * RELAX: an off period this long clears the backoff.
 * PWRUP_BUDGET: the time a power-up and re-init may take, with its one
 *   retry, added to the refill margin [est 0.3-0.5 s, retry ~1 s].
 * READY: a solid drive's BSY must clear this soon after the power-on
 *   wait, or the attempt fails (#114 H1: a dead adapter fails the
 *   power-up in ~7 s with the retry, not upstream's 30 s per attempt);
 *   a rotating drive keeps 30 s for its spin-up [hypothesis: the 4-card
 *   adapter is ready well inside 3 s; the device round's "pwr on ms="
 *   measures it]. */
#define STOR_PWR_HOLD_DARK_S    3
#define STOR_PWR_HOLD_LIT_S     60
#define STOR_PWR_MIN_OFF_S      30
#define STOR_PWR_HOLD_MAX_S     600
#define STOR_PWR_RELAX_S        120
#define STOR_PWR_PWRUP_BUDGET_S 2
#define STOR_PWR_READY_S        3

#ifndef SIMULATOR
/* firmware/drivers/ata.c */
int  ata_power_down(int reason, unsigned flags);
void ata_power_release(void);
int  ata_power_up(int reason);
bool ata_powered(void);
bool ata_power_warm(void);
void ata_set_poweroff(bool allow);
bool ata_poweroff_allowed(void);
const struct stor_pwr *ata_power_state(void);
#define storage_power_down      ata_power_down
#define storage_power_release   ata_power_release
#define storage_power_up        ata_power_up
#define storage_powered         ata_powered
#define storage_power_warm      ata_power_warm
#define storage_set_poweroff    ata_set_poweroff
#define storage_poweroff_allowed ata_poweroff_allowed
#define storage_power_state     ata_power_state
#else
/* the simulator's model: uisimulator/common/filesystem-sim.c */
int  storage_power_down(int reason, unsigned flags);
void storage_power_release(void);
int  storage_power_up(int reason);
bool storage_powered(void);
bool storage_power_warm(void);
void storage_set_poweroff(bool allow);
bool storage_poweroff_allowed(void);
const struct stor_pwr *storage_power_state(void);

/* the sim's model (filesystem-sim.c): every file call is an access */
void sim_stor_access(void);
void sim_stor_tune(long hold_dark_ms, long min_off_ms);
void sim_stor_usb(bool connected);
/* npfix: the modelled power-up wait (default 300 ms); the UI thread's
 * file calls traced ("sim io ui pwr=0|1") while the watch is on */
void sim_stor_pwrup(long ms);
void sim_stor_ui_thread(unsigned int id);
void sim_stor_watch_ui(bool on);
#endif

/* P19 19-1a (review M1): called (any thread, the drive's lock held; it
 * must only post) each time the drive comes out of off or standby for an
 * access, so work that rides an awake drive (the statistics drain, the
 * panel rotation) runs while it is up, on flash and disk alike, instead
 * of finding it asleep after the idle notice. One hook (the tagcache
 * thread's kick). */
void storage_set_awake_hook(void (*fn)(void));

#endif /* HAVE_STORAGE_POWEROFF */
#endif /* STOR_PWR_H */
