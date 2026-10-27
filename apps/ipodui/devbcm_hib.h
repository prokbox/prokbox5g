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
 * The hibernate and bypass probe rows' pure logic (PW5, PW6: docs/
 * bcm2722-reference/brief-map-power.md section 9; bench-p19.md, cards PW5
 * and PW6): the holds, the request, the image's reply and its checks,
 * what the 0x1F0 word said after the pins, the cycle's verdict, the wake
 * against today's cold start, PW6's raw port reads, and the locks (PW5
 * needs PW1 in this boot, PW6 needs PW5).  The pin sequence itself is
 * bcmdt_hib_cycle (bcm-devtest-core.c); the device half is in
 * devbcm_probes.c; tests/hibprobe/ checks this on the host.  The hang
 * watchdog is the TV rows' (devbcm_tv.h).
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
#ifndef _IPODUI_DEVBCM_HIB_H_
#define _IPODUI_DEVBCM_HIB_H_

#include <stdbool.h>
#include <stdint.h>
#include "../../bcmfw/hostproto.h"
#include "devbcm_tv.h"            /* the watchdog: tvp_dog, TVP_HANG_US */

/* ---- PW5: the holds and the bounds -------------------------------------
 * The spec's holds: 1 s, then (on a pass) 10 s, then 60 s.  The image's
 * own bound on its park covers the longest hold, the pins and a margin;
 * the PP's bounds are bcmdt_hib_cycle's (300 ms to park, 200 ms each for
 * the word and the answer), then the reply's below. */
#define PWH_HOLDS           3
#define PWH_WAIT_MS         HP_HIB_WAIT_MS      /* the image's bound */
#define PWH_COLD_US         370000  /* today's fastest cold start to the
                                       first frame (power.md section 2) */

static inline uint32_t pwh_hold_ms(int k)
{
    static const uint32_t ms[PWH_HOLDS] = { 1000, 10000, 60000 };

    return k >= 0 && k < PWH_HOLDS ? ms[k] : 0;
}

/* After HP_HIB_AWAKE the image checks 64 KB and the frame, pushes the
 * mirrored bars and replies: the 2 s hang rule bounds it, twice over */
#define PWH_REPLY_US        (2u * TVP_HANG_US)

/* The request after HP_HIB_MAGIC, its word count back (0: out of
 * bounds) */
static inline uint32_t pwh_request(uint32_t *a, uint32_t code, uint32_t wait_ms)
{
    a[0] = HP_HIB_MAGIC;
    a[1] = HP_HIB_PARK;
    a[2] = code;
    a[3] = wait_ms;
    if (wait_ms == 0 || wait_ms > HP_HIB_WAIT_MS)
        return 0;
    return 4;
}

/* The progress word's parts (0 phase when the tag isn't 'H') */
static inline uint32_t pwh_prog_phase(uint32_t w)
{
    return w >> 24 == 0x48 ? w >> 16 & 0xFF : 0;
}

static inline uint32_t pwh_prog_ms(uint32_t w)
{
    return w >> 24 == 0x48 ? w & 0xFFFF : 0;
}

/* ---- What the word said after the pins (bcmdt_hib_cycle's word_after,
 * spec step 10) */
enum pwh_word
{
    PWH_W_HELD = 0,     /* 0xA5A50000: SRAM and the parked core held */
    PWH_W_RESTARTED,    /* all ones: the chip is back in its boot ROM */
    PWH_W_LOST,         /* anything else: SRAM not kept */
};

static inline enum pwh_word pwh_word(uint32_t w)
{
    return w == HP_HIB_PARKED ? PWH_W_HELD :
           w == 0xFFFFFFFFu ? PWH_W_RESTARTED : PWH_W_LOST;
}

static inline const char *pwh_word_name(enum pwh_word w)
{
    static const char *const s[] = {
        "held (parked)", "all ones (restarted)", "lost",
    };
    return (unsigned)w < sizeof(s) / sizeof(s[0]) ? s[w] : "?";
}

/* bcmdt_hib_cycle's ends (bcm-devtest.h BCMDT_HIB_*: 0 to -6) */
static inline const char *pwh_end_name(int end)
{
    static const char *const s[] = {
        "awake", "refused (pins)", "not sent", "never parked",
        "port dead after the pins", "word lost", "no answer to the wake",
    };
    return end <= 0 && end > -(int)(sizeof(s) / sizeof(s[0])) ? s[-end] : "?";
}

/* ---- The image's reply ------------------------------------------------ */
struct pwh_reply
{
    int status;         /* bvp's: HP_OK, HP_E_* >= 0, or < 0 no reply */
    uint32_t n;
    uint32_t w[HP_HIB_REPLY_WORDS];
};

enum pwh_check
{
    PWH_OK = 0,         /* woken, all put back, SRAM kept */
    PWH_NOREPLY,        /* a hang, the port, or the deadline */
    PWH_REFUSED,        /* the image refused */
    PWH_SHORT,          /* not HP_HIB_REPLY_WORDS words */
    PWH_RESTORE,        /* a word read back wrong after the restore */
    PWH_DENIED,         /* the image denied a write (the allowlist hit) */
    PWH_GAVEUP,         /* no wake word in the image's bound */
    PWH_STALL,          /* its counter stopped while parked */
    PWH_SDIDLE,         /* SDRAM never idled: not parked */
    PWH_SDRAM,          /* SDRAM's bring-up failed after the wake */
    PWH_SRAM_LOST,      /* the SRAM pattern came back wrong */
};

static inline uint32_t pwh_denied(const struct pwh_reply *r)
{
    return r->w[HP_HIB_R_FLAGS] >> 8;
}

/* The checks, in that order: a reply, everything back (the safety half),
 * then the park's exit, then SRAM (the pass condition) */
static inline enum pwh_check pwh_check(const struct pwh_reply *r)
{
    if (r->status < 0)
        return PWH_NOREPLY;
    if (r->status != HP_OK && r->status != HP_E_STATE)
        return PWH_REFUSED;
    if (r->n != HP_HIB_REPLY_WORDS)
        return r->status == HP_E_STATE ? PWH_REFUSED : PWH_SHORT;
    if (r->w[HP_HIB_R_MISMATCH])
        return PWH_RESTORE;
    if (pwh_denied(r))
        return PWH_DENIED;
    switch (r->w[HP_HIB_R_EXIT])
    {
    case HP_HIB_X_OK:
        break;
    case HP_HIB_X_GAVEUP:
        return PWH_GAVEUP;
    case HP_HIB_X_STALL:
        return PWH_STALL;
    case HP_HIB_X_SDIDLE:
        return PWH_SDIDLE;
    default:
        return PWH_SDRAM;
    }
    if (r->status != HP_OK)
        return PWH_REFUSED;
    if (r->w[HP_HIB_R_SRAM_BAD])
        return PWH_SRAM_LOST;
    return PWH_OK;
}

static inline const char *pwh_check_name(enum pwh_check c)
{
    static const char *const s[] = {
        "ok", "no reply", "refused", "short reply", "restore mismatch",
        "write denied", "gave up waiting", "counter stopped",
        "SDRAM never idled", "SDRAM did not come back", "SRAM lost",
    };
    return (unsigned)c < sizeof(s) / sizeof(s[0]) ? s[c] : "?";
}

/* What the dark kept, reported, not required: SDRAM (0 kept, 1 lost, 2
 * not laid) and the host frame (its sum unchanged) */
static inline int pwh_sdram_kept(const struct pwh_reply *r)
{
    uint32_t bad = r->w[HP_HIB_R_SDRAM_BAD];

    return bad == 0xFFFFFFFFu ? 2 : bad ? 1 : 0;
}

static inline bool pwh_frame_kept(const struct pwh_reply *r)
{
    return r->w[HP_HIB_R_FRAME_PRE] == r->w[HP_HIB_R_FRAME_POST];
}

/* Did the image's counter run through the hold?  Its parked-to-wake time
 * against the PP's hold: within 10 % (or 20 ms) it ran; well under, it
 * stopped or was reset in the dark */
static inline bool pwh_counter_ran(const struct pwh_reply *r, uint32_t hold_us)
{
    uint32_t held = r->w[HP_HIB_R_HELD_US], tol = hold_us / 10;

    if (tol < 20000)
        tol = 20000;
    return held + tol >= hold_us;
}

/* ---- One cycle's verdict (the spec's pass: the word held, the image
 * answered within the bound, SRAM unchanged) */
static inline bool pwh_cycle_pass(int end, uint32_t word_after,
                                  const struct pwh_reply *r)
{
    return end == 0 && pwh_word(word_after) == PWH_W_HELD &&
           pwh_check(r) == PWH_OK;
}

/* The wake against today's cold start, in tenths ("x 37.0 faster");
 * 0 when not timed */
static inline uint32_t pwh_speedup_x10(uint32_t wake_us)
{
    return wake_us ? (uint32_t)((uint64_t)PWH_COLD_US * 10u / wake_us) : 0;
}

/* PW5 passes when the first hold passed (the later, longer ones are
 * readings: how long the chip keeps) */
static inline bool pwh_pw5_pass(const bool *cycle_pass, int n)
{
    return n > 0 && cycle_pass[0];
}

/* ---- PW6: the raw port reads (DATA x4, CONTROL, ALT_DATA x4,
 * ALT_CONTROL), by state */
#define PWH_RAW             10

enum pwh_raw
{
    PWH_R_ONES = 0,     /* every halfword 0xFFFF: nothing drives the bus */
    PWH_R_ZEROS,        /* every halfword 0 */
    PWH_R_MIXED,        /* something answers */
};

static inline enum pwh_raw pwh_raw_class(const uint16_t *v)
{
    int i, ones = 0, zeros = 0;

    for (i = 0; i < PWH_RAW; i++)
    {
        ones += v[i] == 0xFFFF;
        zeros += v[i] == 0;
    }
    return ones == PWH_RAW ? PWH_R_ONES : zeros == PWH_RAW ? PWH_R_ZEROS
                                                           : PWH_R_MIXED;
}

static inline const char *pwh_raw_name(enum pwh_raw c)
{
    static const char *const s[] = { "all ones", "all zeros", "mixed" };

    return (unsigned)c < sizeof(s) / sizeof(s[0]) ? s[c] : "?";
}

/* ---- The locks, for this boot ------------------------------------------
 * PW5 opens once PW1 passed (the pins' census: GPO32 as found); PW6 once
 * PW5 has driven bit 15 and recovered (whatever it found: PW6 asks what
 * a powered-down chip leaves on the port and the panel, which PW5's
 * answer about bit 15 explains). */
struct pwh_lock
{
    bool pw1, pw5;
};

static inline void pwh_lock_init(struct pwh_lock *l)
{
    l->pw1 = l->pw5 = false;
}

static inline void pwh_note_pw1(struct pwh_lock *l, bool pass)
{
    if (pass)
        l->pw1 = true;
}

static inline void pwh_note_pw5(struct pwh_lock *l, bool pins_driven)
{
    if (pins_driven)
        l->pw5 = true;
}

static inline bool pwh_pw5_open(const struct pwh_lock *l)
{
    return l->pw1;
}

static inline bool pwh_pw6_open(const struct pwh_lock *l)
{
    return l->pw1 && l->pw5;
}

#endif /* _IPODUI_DEVBCM_HIB_H_ */
