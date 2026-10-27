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

/* The PP side of the instant wake after dark: the hibernate park
 * (docs/research/bcm2722/power-contract.md section 6.6; the BCM side is
 * bcmfw/hibwake.h, bcmfw/hibstub.c and HP_OP_PARK's HP_PARK_HIB form).
 *
 * PROPOSAL, header only, not wired: the driver's lcd_sleep()/lcd_awake()
 * path (lcd-video.c, bcmjob-*.c) would call it.  Everything sits behind
 * HAVE_BCM_HIBWAKE, which no build defines: it stays off until PW5 has
 * passed on this unit AND a bench row has run this stub on it (bhw_gate).
 * Until then dark means cut (C0), as today.
 *
 * Pure: no hardware, no kernel, no globals.  The port, the pins and the
 * clock come in through struct bhw_hal (bcmdt_hib_cycle's shape), so the
 * same code runs against vc2sim in bcmfw/tests/hibwake_test.py and, on
 * the device, against the driver's port and GPO32 accessors.
 *
 * The three parts:
 *   1. the decision (bhw_gate, bhw_choose, bhw_drop_now): whether to hold
 *      the BCM in hibernate at all, for how long, and when to cut;
 *   2. the park (bhw_prepare, bhw_park): the stub and its parameter block
 *      uploaded, the PARK request, then Apple's retain pins;
 *   3. the wake (bhw_wake) and the drop to C0 (bhw_drop).
 * Every failure ends in the cut and a cold boot (a new epoch): a bad hold
 * costs the cold frame we have today, never a wrong frame. */
#ifndef BCM_HIBWAKE_H
#define BCM_HIBWAKE_H

#include <stdbool.h>
#include <stdint.h>

/* ---- The BCM's words (mirrors of bcmfw/hibwake.h and hostproto.h; the
 * test checks they agree) ---------------------------------------------- */
#define BHW_STUB            0x0DEA00u
#define BHW_STUB_MAX        0x600u
#define BHW_STUB_MAGIC      0x48575342u     /* "HWSB" */
#define BHW_PARAM           0x0DF400u
#define BHW_PARAM_MAGIC     0x48575041u     /* "HWPA" */
#define BHW_REGIONS         8
#define BHW_P_WAIT_MS       0x04u
#define BHW_P_NREG          0x08u
#define BHW_P_REG           0x0Cu
#define BHW_P_EXIT          0x6Cu
#define BHW_P_PRE_A         0x70u
#define BHW_P_SUM_US        0x8Cu
#define BHW_WORD            0x1F0u
#define BHW_W_PARKED        0xA5A50000u
#define BHW_W_WAKE          0xA5A50001u
#define BHW_W_AWAKE         0xA5A50002u
#define BHW_W_GAVEUP        0xA5A500FFu
#define BHW_W_SPOILED       0xA5A500EEu
#define BHW_W_REFUSED       0xA5A500FEu
#define BHW_PARK_MAGIC      0x5041524Bu     /* HP_OP_PARK's "PARK" */
#define BHW_PARK_HIB        0x48494252u     /* its hibernate form, "HIBR" */
#define BHW_WAIT_MS_MAX     3600000u

/* ---- The pins and the deadlines (PW5's, bcm-devtest.h BCMDT_HIB_*) --- */
#define BHW_GPO_RUN         0x4000u         /* GPO32 bit 14: the BCM's power */
#define BHW_GPO_RETAIN      0x8000u         /* GPO32 bit 15: Apple's retain */
#define BHW_PIN_US          5000u           /* Apple's wait between the pins */
#define BHW_PARK_MS         300u            /* the request to PARKED: the
                                               first sums (~20 ms at 27 MHz
                                               [sim]) and the ring pass */
#define BHW_WAKE_MS         200u            /* the word after the pins, then
                                               WAKE to AWAKE (the second
                                               sums, ~20 ms [sim]) */
#define BHW_OFF_MS          100u            /* the standard cut's wait */

/* ---- 1. The decision -------------------------------------------------- */

/* May the release line hibernate at all?  Every one must hold: the
 * build's switch, PW5's PASS recorded on this unit (its 1 s hold came
 * back parked, the image answered, SRAM intact: bench-p19.md), and this
 * stub's FNV recorded as having run its own cycle on the unit (bench row
 * HW1, the same discipline as bcmfw/verified.txt: never before silicon
 * says yes). */
static inline bool bhw_gate(bool built, bool pw5_pass, bool stub_verified)
{
    return built && pw5_pass && stub_verified;
}

/* The energy numbers the rule needs, in battery microamps and
 * microamp-seconds (power-contract.md section 6.6 gives them, each tagged;
 * the meter readings replace the estimates):
 *   i_hold_ua   what the hold draws above C0: the BCM in hibernate, plus
 *               the panel if it stays powered meanwhile;
 *   e_cold_uas  what a cold wake costs above a hibernate wake: the
 *               upload, the bootstrap, the panel's power-on and the PP
 *               waiting, then the slots uploaded again. */
struct bhw_power
{
    uint32_t i_hold_ua;
    uint32_t e_cold_uas;
};

/* The break-even hold in ms: past it, holding has cost more than one
 * cold wake.  0 means never hold (a hold that costs nothing to keep gets
 * the maximum, about 49 days). */
static inline uint32_t bhw_breakeven_ms(const struct bhw_power *p)
{
    uint64_t ms;

    if (!p->e_cold_uas)
        return 0;
    if (!p->i_hold_ua)
        return 0xFFFFFFFFu;
    ms = (uint64_t)p->e_cold_uas * 1000u / p->i_hold_ua;
    return ms > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)ms;
}

/* The last dark spells (from the end of the unlit face window to the
 * press), newest at pos - 1.  The driver adds one at each wake from dark
 * (C0 or hibernate) and keeps it in RAM only. */
#define BHW_HIST_N          16
#define BHW_HIST_MIN        4           /* fewer: no prediction yet */
struct bhw_hist
{
    uint32_t ms[BHW_HIST_N];
    uint8_t n, pos;
};

static inline void bhw_hist_add(struct bhw_hist *h, uint32_t ms)
{
    h->ms[h->pos] = ms;
    h->pos = (uint8_t)((h->pos + 1) % BHW_HIST_N);
    if (h->n < BHW_HIST_N)
        h->n++;
}

/* What holding with the cap costs if the spell lasts t ms (uA.ms): the
 * hold's current while it lasts, plus the cold wake if the cap ends it
 * first.  Cutting at once costs e_cold. */
static inline uint64_t bhw_hold_cost(const struct bhw_power *p, uint32_t cap_ms,
                                     uint32_t t_ms)
{
    uint64_t held = t_ms < cap_ms ? t_ms : cap_ms;
    uint64_t c = held * p->i_hold_ua;

    return t_ms < cap_ms ? c : c + (uint64_t)p->e_cold_uas * 1000u;
}

enum { BHW_CUT = 0, BHW_HOLD = 1 };

/* The rule, at dark entry (the end of the unlit face window), for an
 * allowed unit: hold only if the expected cost of holding (capped at the
 * break-even, below) is less than one cold wake, the expectation taken
 * over the recent spells.  With too few spells to judge, hold: the cap
 * alone bounds the loss to one cold wake's energy (the ski-rental bound:
 * never more than twice the better choice in hindsight). */
static inline int bhw_choose(const struct bhw_power *p, const struct bhw_hist *h)
{
    uint32_t cap = bhw_breakeven_ms(p), k;
    uint64_t sum = 0;

    if (!cap)
        return BHW_CUT;
    if (h->n < BHW_HIST_MIN)
        return BHW_HOLD;
    for (k = 0; k < h->n; k++)
        sum += bhw_hold_cost(p, cap, h->ms[k]);
    return sum < (uint64_t)p->e_cold_uas * 1000u * h->n ? BHW_HOLD : BHW_CUT;
}

/* While held: drop to C0 once the hold has lasted the break-even (each
 * further second costs more than the wake it saves) */
static inline bool bhw_drop_now(const struct bhw_power *p, uint32_t held_ms)
{
    return held_ms >= bhw_breakeven_ms(p);
}

/* ---- 2 and 3. The park and the wake ------------------------------------ */

struct bhw_hal
{
    int  (*rd)(uint32_t addr, uint32_t *val);   /* bounded; < 0: port */
    int  (*wr)(uint32_t addr, uint32_t val);
    /* HP_OP_PARK with BHW_PARK_MAGIC, BHW_PARK_HIB, then its reply:
     * 0 OK, < 0 refused, not sent or no reply in time */
    int  (*park)(void *ctx);
    /* GPO32_VAL: clear, then set, bits 14 and 15 only, read-modify-write
     * with interrupts off; returns GPO32_VAL after.  (0, 0) only reads */
    uint32_t (*gpo)(uint32_t clear, uint32_t set);
    uint32_t (*gpo_enable)(void);
    uint32_t (*now_us)(void);
    void (*udelay)(uint32_t us);        /* busy: nothing else runs */
    void (*sleep_ms)(uint32_t ms);
    void (*yield)(void);
    void *ctx;
};

/* A region the stub sums before and after: base and bytes word-aligned,
 * stride in words (1: every word) */
struct bhw_region
{
    uint32_t base, bytes, stride;
};

/* The default regions (bcmfw/config.h's maps, memory-contract.md
 * section 2): everything the image needs to trust its own state and the
 * frames, every word; the slot pools and scratch one word in 16 (a lost
 * SRAM is lost everywhere; a spot of decay in a cover is not worth 30 ms
 * on every wake).  FRONT holds the unlit face at the park, so it is
 * sampled; BACK holds the screen the user left (section 6.3), the frame
 * the wake shows, so it is summed whole.  merged: the merged image's
 * map, else the display-only image's.  Returns the count. */
static inline int bhw_default_regions(bool merged, struct bhw_region *r)
{
    static const struct bhw_region m[] =
    {
        { 0x000200, 0x035400 - 0x000200, 1 },   /* the image and .bss */
        { 0x035400, 0x0CB400 - 0x035400, 16 },  /* the UI pools */
        { 0x0D4000, 0x0DC000 - 0x0D4000, 16 },  /* J_CLS_SCRATCH */
        { 0x0DE000, 0x0DE880 - 0x0DE000, 1 },   /* the job block, slot table */
        { 0x0E0000, 0x105800 - 0x0E0000, 16 },  /* FRONT (the face) */
        { 0x105800, 0x12B000 - 0x105800, 1 },   /* BACK (the screen left) */
        { 0x12B000, 0x140000 - 0x12B000, 1 },   /* spare, the parked AAC tables */
    };
    static const struct bhw_region d[] =
    {
        { 0x000200, 0x00CC00 - 0x000200, 1 },   /* the image and .bss (to the
                                                   decode work area, 0xCC00
                                                   since the 2026-10-25 batch) */
        { 0x046000, 0x0CC000 - 0x046000, 16 },  /* drift, strip, app pools */
        { 0x0D4000, 0x0DC000 - 0x0D4000, 16 },  /* J_CLS_SCRATCH */
        { 0x0DE000, 0x0DE880 - 0x0DE000, 1 },   /* the job block, slot table */
        { 0x0E0000, 0x105800 - 0x0E0000, 16 },  /* FRONT (the face) */
        { 0x105800, 0x12B000 - 0x105800, 1 },   /* BACK (the screen left) */
        { 0x12B000, 0x13B000 - 0x12B000, 16 },  /* J_CLS_THUMB */
        { 0x13B000, 0x140000 - 0x13B000, 1 },   /* the overlay region */
    };
    const struct bhw_region *s = merged ? m : d;
    int n = merged ? (int)(sizeof(m) / sizeof(m[0])) : (int)(sizeof(d) / sizeof(d[0]));
    int k;

    for (k = 0; k < n; k++)
        r[k] = s[k];
    return n;
}

enum
{
    BHW_OK = 0,             /* parked (bhw_park) or awake (bhw_wake) */
    BHW_REFUSED = -1,       /* the pins not as expected: nothing done */
    BHW_E_UPLOAD = -2,      /* the stub or block didn't read back: nothing driven */
    BHW_E_PARK = -3,        /* refused, or no PARKED in time: pins untouched */
    BHW_E_DEAD = -4,        /* after the pins: no port, or all ones */
    BHW_E_LOST = -5,        /* after the pins: the word not PARKED (SRAM lost) */
    BHW_E_WAKE = -6,        /* no AWAKE in time (or GAVEUP) */
    BHW_E_SPOILED = -7,     /* the stub's sums differ: SRAM didn't hold */
};

struct bhw_result
{
    int end;                /* BHW_* */
    uint32_t park_us;       /* the request to PARKED */
    uint32_t hold_us;       /* bit 14 low to bit 14 high */
    uint32_t word_after;    /* the word's first read after the pins */
    uint32_t wake_us;       /* bit 14 high to AWAKE */
    uint32_t word_end;      /* the word's last read */
    bool pins;              /* bit 15 was driven */
    bool cut;               /* the pins' cut ran (bhw_wake, bhw_drop) */
};

/* The word until it reads want, any of the stop values, a port error or
 * the deadline: 0 when it read want */
static inline int bhw_until(const struct bhw_hal *h, uint32_t want, uint32_t ms,
                            uint32_t *last)
{
    uint32_t t0 = h->now_us(), v;

    for (;;)
    {
        if (h->rd(BHW_WORD, &v) < 0)
            return -1;
        *last = v;
        if (v == want)
            return 0;
        if (v == BHW_W_SPOILED || v == BHW_W_REFUSED || v == BHW_W_GAVEUP ||
            h->now_us() - t0 > ms * 1000u)
            return -1;
        h->yield();
    }
}

/* The cut from any pin state: bit 15 low, then bit 14 low, then the wait.
 * The caller cold-boots the BCM next (a new epoch). */
static inline int bhw_cut(const struct bhw_hal *h, struct bhw_result *r, int end)
{
    h->gpo(BHW_GPO_RETAIN, 0);
    h->gpo(BHW_GPO_RUN, 0);
    h->sleep_ms(BHW_OFF_MS);
    r->cut = true;
    return r->end = end;
}

/* Before the park: the stub (once per BCM start; checked word by word)
 * and the parameter block (every park).  Nothing is driven; a failure
 * leaves the BCM running as it was. */
static inline int bhw_prepare(const struct bhw_hal *h, const uint32_t *stub,
                              uint32_t words, const struct bhw_region *reg,
                              int n, uint32_t wait_ms)
{
    uint32_t k, v;
    int i;

    if (!words || words * 4 > BHW_STUB_MAX || stub[0] != BHW_STUB_MAGIC ||
        n < 1 || n > BHW_REGIONS || !wait_ms || wait_ms > BHW_WAIT_MS_MAX)
        return BHW_REFUSED;
    if (h->rd(BHW_STUB, &v) < 0)
        return BHW_E_UPLOAD;
    for (k = 0; k < words; k++)
    {
        if (h->rd(BHW_STUB + 4 * k, &v) < 0)
            return BHW_E_UPLOAD;
        if (v == stub[k])
            continue;
        if (h->wr(BHW_STUB + 4 * k, stub[k]) < 0 ||
            h->rd(BHW_STUB + 4 * k, &v) < 0 || v != stub[k])
            return BHW_E_UPLOAD;
    }
    if (h->wr(BHW_PARAM, BHW_PARAM_MAGIC) < 0 ||
        h->wr(BHW_PARAM + BHW_P_WAIT_MS, wait_ms) < 0 ||
        h->wr(BHW_PARAM + BHW_P_NREG, (uint32_t)n) < 0)
        return BHW_E_UPLOAD;
    for (i = 0; i < n; i++)
        if (h->wr(BHW_PARAM + BHW_P_REG + 12u * i, reg[i].base) < 0 ||
            h->wr(BHW_PARAM + BHW_P_REG + 12u * i + 4, reg[i].bytes) < 0 ||
            h->wr(BHW_PARAM + BHW_P_REG + 12u * i + 8, reg[i].stride) < 0)
            return BHW_E_UPLOAD;
    if (h->rd(BHW_PARAM, &v) < 0 || v != BHW_PARAM_MAGIC)
        return BHW_E_UPLOAD;
    return BHW_OK;
}

/* The park, from a running BCM with nothing in flight (the drift and the
 * marquee stopped, the last fence retired, BACK holding the screen left):
 * the request, PARKED, then Apple's retain order.  On BHW_OK the BCM is
 * held and the port must not be touched until bhw_wake() or bhw_drop().
 * Before the pins every failure leaves them as they were (the caller
 * cuts as today: dark means C0). */
static inline int bhw_park(const struct bhw_hal *h, struct bhw_result *r)
{
    const uint32_t both = BHW_GPO_RUN | BHW_GPO_RETAIN;
    uint32_t t0, v = 0;

    r->end = BHW_REFUSED;
    r->pins = r->cut = false;
    r->park_us = r->hold_us = r->word_after = r->wake_us = r->word_end = 0;
    /* outputs, bit 14 high, bit 15 low: never set bit 15 from a state
     * with bit 14 already low */
    if ((h->gpo_enable() & both) != both || (h->gpo(0, 0) & both) != BHW_GPO_RUN)
        return r->end = BHW_REFUSED;
    t0 = h->now_us();
    if (h->wr(BHW_WORD, 0) < 0 || h->park(h->ctx) < 0)
        return r->end = BHW_E_PARK;
    if (bhw_until(h, BHW_W_PARKED, BHW_PARK_MS, &v))
    {
        r->word_end = v;
        return r->end = BHW_E_PARK;
    }
    r->park_us = h->now_us() - t0;
    r->pins = true;
    h->gpo(0, BHW_GPO_RETAIN);
    h->udelay(BHW_PIN_US);
    h->gpo(BHW_GPO_RUN, 0);
    r->hold_us = h->now_us();           /* the hold's start, for bhw_wake */
    return r->end = BHW_OK;
}

/* The wake: Apple's on order, the word must still be PARKED, the wake
 * word, then AWAKE (the stub's second sums matched).  BHW_OK: J_EPOCH,
 * FRONT, BACK and the slots are as they were; show BACK (BLIT BACK ->
 * FRONT, PRESENT with J_PRES_WAIT, FENCE), then the backlight.  Anything
 * else has already cut: cold-boot, new epoch, the PP framebuffer first. */
static inline int bhw_wake(const struct bhw_hal *h, struct bhw_result *r)
{
    uint32_t t_on, v = 0, t0;

    h->gpo(0, BHW_GPO_RUN);
    t_on = h->now_us();
    r->hold_us = t_on - r->hold_us;
    h->udelay(BHW_PIN_US);
    h->gpo(BHW_GPO_RETAIN, 0);
    t0 = h->now_us();
    for (;;)
    {
        if (h->rd(BHW_WORD, &v) >= 0)
            break;
        if (h->now_us() - t0 > BHW_WAKE_MS * 1000u)
            return bhw_cut(h, r, BHW_E_DEAD);
        h->yield();
    }
    r->word_after = r->word_end = v;
    if (v == 0xFFFFFFFFu)
        return bhw_cut(h, r, BHW_E_DEAD);
    if (v != BHW_W_PARKED)
        return bhw_cut(h, r, BHW_E_LOST);
    if (h->wr(BHW_WORD, BHW_W_WAKE) < 0)
        return bhw_cut(h, r, BHW_E_DEAD);
    if (bhw_until(h, BHW_W_AWAKE, BHW_WAKE_MS, &v))
    {
        r->word_end = v;
        return bhw_cut(h, r, v == BHW_W_SPOILED ? BHW_E_SPOILED : BHW_E_WAKE);
    }
    r->word_end = v;
    r->wake_us = h->now_us() - t_on;
    return r->end = BHW_OK;
}

/* From the hold to C0, when bhw_drop_now() says so: bit 15 low (bit 14
 * is already low), so both pins are low: Apple's "off".  The next wake is
 * cold. */
static inline void bhw_drop(const struct bhw_hal *h, struct bhw_result *r)
{
    h->gpo(BHW_GPO_RETAIN, 0);
    r->cut = true;
}

#endif /* BCM_HIBWAKE_H */
