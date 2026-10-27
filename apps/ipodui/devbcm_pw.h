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
 * The power probe rows' pure logic (PW3, PW4: docs/bcm2722-reference/
 * brief-map-power.md section 9; bench-p19.md, cards PW3 and PW4): the
 * PP's deadline for HP_OP_PW, the requests, the reply's checks and
 * verdicts, the clock ladder's speed reading, and the lock that keeps PW4
 * shut until PW3 passed in this boot.  The device half is in
 * devbcm_probes.c; tests/pwprobe/ checks this on the host.  The hang
 * watchdog is the TV rows' (devbcm_tv.h): the same two words, the same
 * 2 s.
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
#ifndef _IPODUI_DEVBCM_PW_H_
#define _IPODUI_DEVBCM_PW_H_

#include <stdbool.h>
#include <stdint.h>
#include "../../bcmfw/hostproto.h"
#include "devbcm_tv.h"            /* the watchdog: tvp_dog, TVP_HANG_US */

/* ---- Holds and the deadline --------------------------------------------
 * PW3 holds each level 20 s (the spec's meter reading); PW4 holds each
 * point 10 s, long enough for a meter to settle.  The self-check runs
 * over and over through the hold. */
#define PWP_PW3_HOLD_MS     20000
#define PWP_PW4_HOLD_MS     10000
#define PWP_PW3_STEPS       4
#define PWP_PW4_STEPS       HP_PW_POINTS
/* The reply's bound: the hold, then the 2 s hang rule for the rest (the
 * two reference runs, up to four 10 ms settles, the PLL's 1 ms, the last
 * run past the hold, the frame, the reply) */
#define PWP_MARGIN_US       (TVP_HANG_US + 4 * HP_PW_SETTLE_US + HP_PW_PLL_US)

static inline uint32_t pwp_deadline_us(uint32_t hold_ms)
{
    return hold_ms * 1000u + PWP_MARGIN_US;
}

/* The progress word's parts (0 phase when the tag isn't 'P') */
static inline uint32_t pwp_prog_phase(uint32_t w)
{
    return w >> 24 == 0x50 ? w >> 16 & 0xFF : 0;
}

static inline uint32_t pwp_prog_ms(uint32_t w)
{
    return w >> 24 == 0x50 ? w & 0xFFFF : 0;
}

/* ---- The steps ---------------------------------------------------------
 * PW3: the spec's levels, top down; PW4: Apple's ladder points */
static inline uint32_t pwp_pw3_level(int k)
{
    return k >= 0 && k < PWP_PW3_STEPS ? 11u - 2u * (uint32_t)k : 0;
}

/* the image's pw_ladder (bcmfw/hostproto.c): MHz and level at a point */
static inline uint32_t pwp_mhz(int point)
{
    static const uint16_t mhz[HP_PW_POINTS] = { 46, 74, 102, 150 };

    return point >= 0 && point < HP_PW_POINTS ? mhz[point] : 0;
}

static inline uint32_t pwp_level(int point)
{
    static const uint8_t lv[HP_PW_POINTS] = { 5, 7, 9, 15 };

    return point >= 0 && point < HP_PW_POINTS ? lv[point] : 0;
}

/* The request after HP_PW_MAGIC, its word count back (0: out of bounds) */
static inline uint32_t pwp_request(uint32_t *a, bool ladder, uint32_t code,
                                   uint32_t what, uint32_t hold_ms)
{
    a[0] = HP_PW_MAGIC;
    a[1] = ladder ? HP_PW_POINT : HP_PW_LEVEL;
    a[2] = code;
    a[3] = what;
    a[4] = hold_ms;
    if (hold_ms > HP_PW_HOLD_MS)
        return 0;
    if (ladder ? what >= HP_PW_POINTS
               : what < HP_PW_LEVEL_MIN || what > HP_PW_LEVEL_MAX)
        return 0;
    return 5;
}

/* ---- Replies -----------------------------------------------------------
 * status: bvp's (HP_OK, HP_E_* >= 0, or < 0 for no reply); n words */
struct pwp_reply
{
    int status;
    uint32_t n;
    uint32_t w[HP_PW_REPLY_WORDS];
};

enum pwp_check
{
    PWP_OK = 0,         /* held, every run right, all put back */
    PWP_NOREPLY,        /* a hang, the port, or the deadline */
    PWP_REFUSED,        /* the image refused (HP_E_INVAL, _BUSY, _STATE ...) */
    PWP_SHORT,          /* not HP_PW_REPLY_WORDS words */
    PWP_RESTORE,        /* a register read back wrong after the restore */
    PWP_DENIED,         /* the image denied a write (the allowlist hit) */
    PWP_REFBAD,         /* core and vector disagreed before any write */
    PWP_READBACK,       /* the level or the clock didn't take */
    PWP_WRONG,          /* a self-check came out wrong at the point */
    PWP_STALL,          /* the counter stopped in a wait */
};

static inline uint32_t pwp_denied(const struct pwp_reply *r)
{
    return r->w[HP_PW_R_FLAGS] >> 8;
}

static inline uint32_t pwp_runs(const struct pwp_reply *r)
{
    return r->w[HP_PW_R_RUNS] & 0xFFFF;
}

static inline uint32_t pwp_wrong(const struct pwp_reply *r)
{
    return r->w[HP_PW_R_RUNS] >> 16;
}

/* The checks, in that order: whether a reply came, whether everything
 * went back (the safety half), then how the point went */
static inline enum pwp_check pwp_check(const struct pwp_reply *r)
{
    if (r->status < 0)
        return PWP_NOREPLY;
    if (r->status != HP_OK && r->status != HP_E_STATE)
        return PWP_REFUSED;
    if (r->n != HP_PW_REPLY_WORDS)
        return r->status == HP_E_STATE ? PWP_REFUSED : PWP_SHORT;
    if (r->w[HP_PW_R_MISMATCH])
        return PWP_RESTORE;
    if (pwp_denied(r))
        return PWP_DENIED;
    switch (r->w[HP_PW_R_EXIT])
    {
    case HP_PW_X_OK:
        break;
    case HP_PW_X_REFBAD:
        return PWP_REFBAD;
    case HP_PW_X_READBACK:
        return PWP_READBACK;
    case HP_PW_X_CHECK:
        return PWP_WRONG;
    default:
        return PWP_STALL;
    }
    if (r->status != HP_OK || pwp_runs(r) == 0 || pwp_wrong(r) ||
        r->w[HP_PW_R_SUM0] == 0)
        return PWP_WRONG;
    return PWP_OK;
}

static inline const char *pwp_check_name(enum pwp_check c)
{
    static const char *const s[] = {
        "ok", "no reply", "refused", "short reply", "restore mismatch",
        "write denied", "reference bad", "did not take", "self-check wrong",
        "counter stopped",
    };
    return (unsigned)c < sizeof(s) / sizeof(s[0]) ? s[c] : "?";
}

/* ---- PW3: the levels ---------------------------------------------------
 * The steps run top down and stop at the first that isn't ok; the lowest
 * good level is the last ok one (0: none).  PW3 passes when level 5 was
 * ok: our UI image holds at Apple's level for 27 MHz. */
static inline int pwp_pw3_lowest(const struct pwp_reply *r, int n)
{
    int k, lowest = 0;

    for (k = 0; k < n && k < PWP_PW3_STEPS; k++)
    {
        if (pwp_check(&r[k]) != PWP_OK)
            break;
        lowest = (int)pwp_pw3_level(k);
    }
    return lowest;
}

static inline bool pwp_pw3_pass(const struct pwp_reply *r, int n)
{
    return pwp_pw3_lowest(r, n) == HP_PW_LEVEL_MIN;
}

/* ---- PW4: the ladder ---------------------------------------------------
 * The self-check's speed at a point against the start, in thousandths
 * (0: not timed), and what the clock alone predicts: f / 27 MHz */
static inline uint32_t pwp_speed_milli(const struct pwp_reply *r)
{
    uint32_t us0 = r->w[HP_PW_R_US0], us = r->w[HP_PW_R_US];

    return us ? (uint32_t)((uint64_t)us0 * 1000u / us) : 0;
}

static inline uint32_t pwp_want_milli(int point)
{
    return pwp_mhz(point) * 1000u / 27u;
}

enum pwp_speed
{
    PWP_S_CLOCK = 0,    /* within 1 % of f / 27: the core runs at f */
    PWP_S_SLOWER,       /* below: memory or the bus doesn't scale */
    PWP_S_FASTER,       /* above: the start wasn't 27 MHz, or a timing slip */
    PWP_S_NONE,         /* not timed */
};

static inline enum pwp_speed pwp_speed_class(const struct pwp_reply *r,
                                             int point)
{
    uint32_t got = pwp_speed_milli(r), want = pwp_want_milli(point);
    uint32_t tol = want / 100;

    if (!got || !want)
        return PWP_S_NONE;
    if (got + tol < want)
        return PWP_S_SLOWER;
    if (got > want + tol)
        return PWP_S_FASTER;
    return PWP_S_CLOCK;
}

static inline const char *pwp_speed_name(enum pwp_speed s)
{
    static const char *const n[] = {
        "with the clock", "slower than the clock", "faster than the clock",
        "not timed",
    };
    return (unsigned)s < sizeof(n) / sizeof(n[0]) ? n[s] : "?";
}

/* PW4 passes when every point was ok (the speed is a reading, not a
 * condition: the self-check is memory-bound, unlike stage 4's kernels) */
static inline bool pwp_pw4_pass(const struct pwp_reply *r, int n)
{
    int k;

    if (n != PWP_PW4_STEPS)
        return false;
    for (k = 0; k < n; k++)
        if (pwp_check(&r[k]) != PWP_OK)
            return false;
    return true;
}

/* ---- The lock, for this boot -------------------------------------------
 * PW4 opens once PW3 passed; the boot itself also needs stage 4b's pass
 * flag (PLL A proven on this unit), which the row checks at run time. */
struct pwp_lock
{
    bool pw3;
};

static inline void pwp_lock_init(struct pwp_lock *l)
{
    l->pw3 = false;
}

static inline bool pwp_pw4_open(const struct pwp_lock *l)
{
    return l->pw3;
}

static inline void pwp_note_pw3(struct pwp_lock *l, bool pass)
{
    if (pass)
        l->pw3 = true;
}

#endif /* _IPODUI_DEVBCM_PW_H_ */
