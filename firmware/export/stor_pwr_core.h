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

/* Drive power policy (#114, P18 A3): when the storage adapter may lose its
 * power while idle, and the counters that show how often it does.
 * Pure logic, no Rockbox calls (host test: tests/storpwr/). */
#ifndef STOR_PWR_CORE_H
#define STOR_PWR_CORE_H

#include <stdbool.h>
#include <stdint.h>

/* Why the power changed (the log's reason= field) */
enum stor_pwr_reason {
    STOR_PWR_R_IDLE = 0,    /* off: the idle hold ran out */
    STOR_PWR_R_ACCESS,      /* on: a read or write found it off */
    STOR_PWR_R_SLEEP,       /* off: storage_power_down() at sleep entry */
    STOR_PWR_R_USB,         /* on: USB connect, before the host sees it */
    STOR_PWR_R_API,         /* on or off: storage_power_up()/_down() */
    STOR_PWR_R_FAIL,        /* off: a power-up failed twice, cut again */
    STOR_PWR_R_GIVEUP,      /* on: STOR_PWR_FAIL_MAX failed power-ups in a
                             * row; the power stays on for the session */
    STOR_PWR_R_DIRTY,       /* on: the cache could not be made safe (a
                             * failed FLUSH CACHE, or a write cache with
                             * no flush), so no cut */
    STOR_PWR_R_COUNT
};

/* Tuning, in ticks (the caller's units). The firmware's values are in
 * firmware/export/stor_pwr.h; docs/storage-choices.md says why. */
struct stor_pwr_tune {
    long hold_dark;     /* powered this long after the last access, dark */
    long hold_lit;      /* the same while the screen is lit (browsing) */
    long min_off;       /* an off period shorter than this wasted a cycle */
    long hold_max;      /* the backoff never makes the hold longer */
    long relax;         /* an off period this long clears the backoff */
};

struct stor_pwr_event {
    uint32_t tick;      /* when */
    uint16_t ms;        /* off: how long it had been on (saturates);
                         * on: how long the re-initialisation took */
    uint8_t on;         /* 1 = powered on, 0 = cut */
    uint8_t reason;     /* enum stor_pwr_reason */
};

#define STOR_PWR_EVENTS 16  /* a power of two */

/* failed power-ups in a row (each with its one retry) after which the
 * idle power-off gives up for the session (stor_pwr_power_on()) */
#define STOR_PWR_FAIL_MAX 3

struct stor_pwr {
    long changed;       /* tick of the last on/off change */
    long backoff;       /* extra hold after wasted cycles; 0 normally */
    uint32_t on_ticks;  /* closed on periods, summed */
    uint32_t ons;       /* power-ons since boot */
    uint32_t offs;      /* power cuts since boot */
    uint32_t wasted;    /* power-ons that came within min_off of the cut */
    uint32_t ev_head;   /* events ever recorded */
    uint32_t fails;     /* failed power-ups (both attempts) since boot */
    uint32_t dirty;     /* cuts refused: the cache not safe */
    uint8_t fail_run;   /* failed power-ups in a row */
    bool dirty_run;     /* refusing now (one event per run) */
    bool gave_up;       /* the run reached STOR_PWR_FAIL_MAX */
    bool on;
    struct stor_pwr_event ev[STOR_PWR_EVENTS];
};

void stor_pwr_init(struct stor_pwr *s, long now, bool on);

/* the hold in force now: hold_lit or hold_dark, plus the backoff */
long stor_pwr_hold(const struct stor_pwr *s, const struct stor_pwr_tune *t,
                   bool lit);

/* powered, and no access since last_access + the hold */
bool stor_pwr_off_due(const struct stor_pwr *s,
                      const struct stor_pwr_tune *t, long now,
                      long last_access, bool lit);

/* the cut happened at 'now'; returns the on period it closed (ticks) */
long stor_pwr_note_off(struct stor_pwr *s, long now, int reason,
                       int tick_ms);

/* power came back at 'now' after a re-init of reinit_ticks; returns the
 * off period it closed. A short off period doubles the backoff (the
 * hysteresis: a pattern of short gaps soon stops cycling); a long one
 * clears it. */
long stor_pwr_note_on(struct stor_pwr *s, const struct stor_pwr_tune *t,
                      long now, long reinit_ticks, int reason, int tick_ms);

/* The power-up with its retry (#114 H1), the hardware behind callbacks
 * so the host test can inject failures. power_on: power, wait, reset and
 * re-initialise, 0 or < 0; power_off: cut and let the rail fall; now: the
 * tick. A failed attempt is cut and tried once more. Returns 0 (event
 * "on", the failure run cleared), or the last error with the power cut
 * (event "off reason=fail"): the caller marks the drive off, so the next
 * access tries a fresh power-up. *give_up is set once, when the run of
 * failed power-ups reaches STOR_PWR_FAIL_MAX (event "on reason=giveup"):
 * the caller then keeps the power on for the session. */
struct stor_pwr_ops {
    int  (*power_on)(void *ctx);
    void (*power_off)(void *ctx);
    long (*now)(void *ctx);
    void *ctx;
};
int stor_pwr_power_on(struct stor_pwr *s, const struct stor_pwr_tune *t,
                      const struct stor_pwr_ops *ops, int reason,
                      int tick_ms, bool *give_up);

/* #114 M2: may the power be cut without losing written data? Upstream's
 * guard first (the drive claims power management, or offers FLUSH CACHE:
 * "various SD adapters" do neither), then the cache: FLUSH CACHE offered
 * and it worked, or no write cache enabled (IDENTIFY word 85 bit 5). */
bool stor_pwr_cut_safe(bool can_flush, bool flush_ok, bool wcache_on,
                       bool pm);

/* a cut refused (stor_pwr_cut_safe() false): counted, one "dirty" event
 * per run of refusals; stor_pwr_note_clean() ends the run */
void stor_pwr_note_dirty(struct stor_pwr *s, long now);
void stor_pwr_note_clean(struct stor_pwr *s);

/* on time since boot, the open period included */
uint32_t stor_pwr_on_ticks(const struct stor_pwr *s, long now);

/* event k (k < ev_head, and not older than STOR_PWR_EVENTS), or NULL */
const struct stor_pwr_event *stor_pwr_event_at(const struct stor_pwr *s,
                                               uint32_t k);

const char *stor_pwr_reason_name(int reason);

/* Refill margin: the seconds of audio the buffer must still hold when it
 * asks for a refill, so that a power-up and re-initialisation of up to
 * pwrup_s finish first, at a playback speed of speed_pct (100 = normal;
 * an audiobook at 2x drains the buffer twice as fast). */
int stor_pwr_refill_secs(int base_s, int pwrup_s, int speed_pct);

#endif /* STOR_PWR_CORE_H */
