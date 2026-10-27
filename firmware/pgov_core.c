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
 * The PP5022 band governor's policy (docs/design/pp-governor.md sections
 * 3, 4 and 9): the band selector, the in-band policy, the voltage layer,
 * the tables and the persistent record.  Pure C, no kernel calls: the
 * firmware (pgov.c), the host tests and the simulator (tests/ppgov/) all
 * run this file.
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
#ifndef PGOV_HOST
#include "config.h"
#endif
#include "pgov.h"

/* The voltage layer is built for the host (tests, simulator) and with
 * HAVE_PP_GOV_DVS; G1's firmware never arms it, so it is left out there */
#if defined(PGOV_HOST) || defined(HAVE_PP_GOV_DVS)
#define PGOV_DVS 1
#endif

/* A raise waits this long before any clock above band 1 (section 3.4) */
#define PGOV_SETTLE_MS  10

/* Band 2's operating points for the proportional arm (section 3.1) */
static const uint8_t b2_points[] = { 30, 48, 60, 80 };

void pgov_params_default(struct pgov_params *p)
{
    /* the simulator's choice (tests/ppgov/sim.c --grid, brief section 5):
     * see docs/design/pp-governor.md section 5.4 */
    p->dwell_ms[PGOV_B0] = 0;
    p->dwell_ms[PGOV_B1] = 250;
    p->dwell_ms[PGOV_B2] = 100;
    p->dwell_ms[PGOV_B3] = 50;
    p->vdwell_ms = 2000;
    p->sat_pm = 950;
    p->headroom_pct = 95;
    p->down_pct = 110;
    p->ewma_shift = 3;
    p->sat_samples = 16;
    p->inband = PGOV_INBAND_PROP;
    p->b2_mask = 0x0f;
}

void pgov_core_init(struct pgov_core *c, const struct pgov_params *p,
                    unsigned int live, unsigned int start_mhz)
{
    c->p = *p;
    c->need_khz = 0;
    c->below_ms = c->vok_ms = c->settle_ms = 0;
    c->live = live & (PGOV_LIVE_B1 | PGOV_LIVE_B3);
    c->sat_run = 0;
    c->mhz = start_mhz;
    c->vcode = PGOV_V_NORM;
    c->vstate = PGOV_VS_NORM;
    c->dvs_ok = 0;
    c->why = PGOV_WHY_NONE;
    c->from = start_mhz;
}

unsigned int pgov_band_of(unsigned int live, unsigned int mhz)
{
    if (mhz <= PGOV_MHZ_XTAL)
        return PGOV_B0;
    if (mhz <= PGOV_MHZ_LOW && (live & PGOV_LIVE_B1))
        return PGOV_B1;
    if (mhz <= PGOV_MHZ_TOP)
        return PGOV_B2;
    return PGOV_B3;
}

unsigned int pgov_vceil_mhz(unsigned int live, unsigned int vcode)
{
    if (vcode == PGOV_V_LOW)
        return PGOV_MHZ_LOW;
    return (live & PGOV_LIVE_B3) ? PGOV_MHZ_B3 : PGOV_MHZ_TOP;
}

unsigned int pgov_need_mhz10(const struct pgov_core *c)
{
    return c->need_khz > 0 ? (unsigned int)c->need_khz / 100 : 0;
}

unsigned int pgov_busy_pm(uint32_t idle_us, uint32_t elapsed_us)
{
    if (elapsed_us == 0 || idle_us >= elapsed_us)
        return 0;
    /* 1000 x 10 ms fits easily; a late tick may stretch elapsed */
    while (elapsed_us > 4000000u)   /* x 1000 stays in 32 bits */
    {
        elapsed_us >>= 1;
        idle_us >>= 1;
    }
    return 1000u - idle_us * 1000u / elapsed_us;
}

/* need <= pct% of the point */
static bool fits(const struct pgov_core *c, unsigned int mhz,
                 unsigned int pct)
{
    return (uint32_t)c->need_khz * 100u <= (uint32_t)pct * mhz * 1000u;
}

/* The measured need's point: the lowest live band that fits (band 2 by
 * the in-band policy), band 3 only when saturated or already there */
static unsigned int load_mhz(const struct pgov_core *c, bool sat,
                             unsigned int pct)
{
    if (fits(c, PGOV_MHZ_XTAL, pct))
        return PGOV_MHZ_XTAL;
    if ((c->live & PGOV_LIVE_B1) && fits(c, PGOV_MHZ_LOW, pct))
        return PGOV_MHZ_LOW;
    if (c->p.inband == PGOV_INBAND_PROP)
    {
        for (unsigned int i = 0; i < sizeof(b2_points); i++)
        {
            unsigned int m = b2_points[i];
            if (m == PGOV_MHZ_LOW && (c->live & PGOV_LIVE_B1))
                continue;           /* that point is band 1 */
            if (!(c->p.b2_mask & (1u << i)) && m != PGOV_MHZ_TOP)
                continue;           /* not run by this build */
            if (fits(c, m, pct))
                return m;
        }
    }
    else if (fits(c, PGOV_MHZ_TOP, pct))
        return PGOV_MHZ_TOP;
    if ((c->live & PGOV_LIVE_B3) && (sat || c->mhz > PGOV_MHZ_TOP))
        return PGOV_MHZ_B3;
    return PGOV_MHZ_TOP;
}

/* A floor or cap as an operating point this unit has */
static unsigned int norm_point(const struct pgov_core *c, unsigned int m)
{
    if (m > PGOV_MHZ_TOP)
        return (c->live & PGOV_LIVE_B3) ? PGOV_MHZ_B3 : PGOV_MHZ_TOP;
    if (m > PGOV_MHZ_LOW)
        return PGOV_MHZ_TOP;
    if (m > PGOV_MHZ_XTAL)
        return PGOV_MHZ_LOW;
    return PGOV_MHZ_XTAL;
}

/* The top of the next live band below b */
static unsigned int lower_top(const struct pgov_core *c, unsigned int b)
{
    if (b == PGOV_B3)
        return PGOV_MHZ_TOP;
    if (b == PGOV_B2 && (c->live & PGOV_LIVE_B1))
        return PGOV_MHZ_LOW;
    return PGOV_MHZ_XTAL;
}

#ifdef PGOV_DVS
static void raise_voltage(struct pgov_core *c)
{
    c->vcode = PGOV_V_NORM;
    c->vstate = PGOV_VS_RAISING;
    c->settle_ms = PGOV_SETTLE_MS;
    c->vok_ms = 0;
}
#endif

static void move(struct pgov_core *c, unsigned int mhz, unsigned int why)
{
    if (mhz != c->mhz)
    {
        c->from = c->mhz;
        c->mhz = mhz;
        c->why = why;
    }
}

/* A load point raised to the floors and boosts, lowered to the caps */
static unsigned int bound(struct pgov_core *c, const struct pgov_in *in,
                          unsigned int want, unsigned int *why)
{
    if (in->floor_mhz && norm_point(c, in->floor_mhz) > want)
    {
        want = norm_point(c, in->floor_mhz);
        *why = PGOV_WHY_FLOOR;
    }
    if (in->boost && want < PGOV_MHZ_TOP)
    {
        want = PGOV_MHZ_TOP;
        *why = PGOV_WHY_BOOST;
    }
    if (in->cap_mhz && want > norm_point(c, in->cap_mhz))
    {
        want = norm_point(c, in->cap_mhz);
        *why = PGOV_WHY_CAP;
        c->sat_run = 0;             /* a new saturated run to come back */
    }
    return want;
}

bool pgov_core_step(struct pgov_core *c, const struct pgov_in *in)
{
    unsigned int busy = in->busy_pm[0] > in->busy_pm[1]
                        ? in->busy_pm[0] : in->busy_pm[1];
    int32_t x = (int32_t)(busy > 1000 ? 1000 : busy) * in->cur_mhz;
    unsigned int old_mhz = c->mhz, old_v = c->vcode;
    unsigned int want, why, dpct, now = c->mhz, dt = in->dt_ms;
    bool sat = false;

    /* the need: an EWMA, and a saturated run resets it to the sample (the
     * EWMA lags a step; saturation doesn't wait for it) */
    if (busy >= c->p.sat_pm)
    {
        if (++c->sat_run >= c->p.sat_samples)
        {
            sat = true;
            c->sat_run = c->p.sat_samples;
        }
    }
    else
        c->sat_run = 0;
    c->need_khz += (x - c->need_khz) / (1 << c->p.ewma_shift);
    if (sat && c->need_khz < x)
        c->need_khz = x;

    /* what is wanted now: load, then floors and boosts, then caps; up by
     * the headroom, down by the (wider) down threshold */
    dpct = c->p.down_pct > c->p.headroom_pct ? c->p.down_pct
                                             : c->p.headroom_pct;
    why = sat ? PGOV_WHY_SAT : PGOV_WHY_LOAD;
    want = bound(c, in, load_mhz(c, sat, c->p.headroom_pct), &why);
    if (want > now)
    {
        c->below_ms = 0;
        now = want;
    }
    else if ((want = bound(c, in, load_mhz(c, sat, dpct), &why)) < now)
    {
        /* between and inside bands: down one band per dwell; a cap at
         * once */
        unsigned int b = pgov_band_of(c->live, now);
        bool capped = why == PGOV_WHY_CAP;

        c->below_ms += dt;
        if (capped || c->below_ms >= c->p.dwell_ms[b])
        {
            if (capped || pgov_band_of(c->live, want) == b)
                now = want;
            else
            {
                unsigned int lo = lower_top(c, b);
                now = want > lo ? want : lo;
            }
            why = capped ? PGOV_WHY_CAP : PGOV_WHY_DWELL;
            c->below_ms = 0;
        }
    }
    else
        c->below_ms = 0;

#ifdef PGOV_DVS
    /* the voltage layer (section 4.3) */
    if (c->vstate == PGOV_VS_RAISING)
    {
        if (c->settle_ms > dt)
            c->settle_ms -= dt;
        else
        {
            c->settle_ms = 0;
            c->vstate = PGOV_VS_NORM;
        }
    }
    if (c->vstate == PGOV_VS_LOW && (in->vwrite || !c->dvs_ok))
        raise_voltage(c);           /* a write, or a fallback since */
    if (c->vstate != PGOV_VS_NORM && now > PGOV_MHZ_LOW)
    {
        if (c->vstate == PGOV_VS_LOW)
            raise_voltage(c);
        now = PGOV_MHZ_LOW;         /* band 1's top until it settles */
        why = PGOV_WHY_VOLT;
    }
    if (c->vstate == PGOV_VS_NORM)
    {
        if (c->dvs_ok && now <= PGOV_MHZ_LOW && want <= PGOV_MHZ_LOW
            && in->floor_mhz <= PGOV_MHZ_LOW && !in->boost && !in->vwrite)
        {
            c->vok_ms += dt;
            if (c->vok_ms >= c->p.vdwell_ms)
            {
                c->vcode = PGOV_V_LOW;
                c->vstate = PGOV_VS_LOW;
                c->vok_ms = 0;
            }
        }
        else
            c->vok_ms = 0;
    }

#endif
    move(c, now, why);
    return c->mhz != old_mhz || c->vcode != old_v;
}

unsigned int pgov_core_boost(struct pgov_core *c)
{
#ifdef PGOV_DVS
    if (c->vstate != PGOV_VS_NORM)
    {
        if (c->vstate == PGOV_VS_LOW)
            raise_voltage(c);
        move(c, PGOV_MHZ_LOW, PGOV_WHY_VOLT);
    }
    else
#endif
    if (c->mhz < PGOV_MHZ_TOP)
        move(c, PGOV_MHZ_TOP, PGOV_WHY_BOOST);
    c->below_ms = 0;
    return c->mhz;
}

unsigned int pgov_core_floor(struct pgov_core *c, unsigned int floor_mhz)
{
    unsigned int m = norm_point(c, floor_mhz);
#ifdef PGOV_DVS
    unsigned int ceil = pgov_vceil_mhz(c->live, c->vcode);

    if (c->vstate == PGOV_VS_RAISING && ceil > PGOV_MHZ_LOW)
        ceil = PGOV_MHZ_LOW;
    if (m > ceil)
        m = ceil;
#endif
    if (m > c->mhz)
    {
        move(c, m, PGOV_WHY_FLOOR);
        c->below_ms = 0;
    }
    return c->mhz;
}

void pgov_core_arm(struct pgov_core *c, bool armed)
{
    c->dvs_ok = armed && (c->live & PGOV_LIVE_B1);
    c->vok_ms = 0;
#ifdef PGOV_DVS
    /* disarmed while low: the raise is decided now, not at the next
     * sample (the apply layer must never see a low code it may not keep) */
    if (!c->dvs_ok && c->vstate == PGOV_VS_LOW)
        raise_voltage(c);
#endif
}

bool pgov_core_fallback(struct pgov_core *c)
{
    bool raise = c->vcode != PGOV_V_NORM;

    c->dvs_ok = 0;
    c->live &= ~PGOV_LIVE_B1;
    c->vcode = PGOV_V_NORM;
    c->vstate = PGOV_VS_NORM;      /* the raw write is synchronous */
    c->settle_ms = 0;
    c->vok_ms = 0;
    return raise;
}

bool pgov_core_valid(const struct pgov_core *c, unsigned int mhz,
                     unsigned int vcode)
{
    if (vcode != PGOV_V_NORM && vcode != PGOV_V_LOW)
        return false;
    if (mhz > pgov_vceil_mhz(c->live, vcode))
        return false;
    if (vcode == PGOV_V_LOW && !(c->live & PGOV_LIVE_B1))
        return false;
    return mhz == PGOV_MHZ_XTAL || pgov_pll_word(mhz) != 0;
}

/* ---- Tables ------------------------------------------------------------ */

static const struct { uint8_t mhz, m, n, post; } pll_points[] = {
    /* PLL_CONTROL = 0x8a02_0000 | post << 20 | M << 8 | N (the constant
     * bits as in every Rockbox word); f = 24 MHz x M / N / 2^post; the VCO (24 x M / N) stays >= 96 MHz.
     * 30 and 80 MHz: Rockbox's words, run daily [silicon]; the same VCOs
     * as Apple's.  48, 60 and 100 MHz: Apple's PP5022 recipe, N = 12, /4
     * for 27-60 MHz and /2 for 60-125 MHz [apple, mac-and-dvs.md 3.1] */
    {  30,   5,  1, 2 },        /* VCO 120 [rockbox-derived] */
    {  48,  96, 12, 2 },        /* VCO 192 [apple] */
    {  60,  60, 12, 1 },        /* VCO 120 [apple] */
    {  80,  20,  3, 1 },        /* VCO 160 [rockbox-derived] */
    { 100, 100, 12, 1 },        /* VCO 200 [apple] */
};

uint32_t pgov_pll_word(unsigned int mhz)
{
    for (unsigned int i = 0; i < sizeof(pll_points)/sizeof(pll_points[0]);
         i++)
    {
        if (pll_points[i].mhz == mhz)
            return 0x8a020000u | ((uint32_t)pll_points[i].post << 20)
                 | ((uint32_t)pll_points[i].m << 8) | pll_points[i].n;
    }
    return 0;
}

uint32_t pgov_dev_timing1(unsigned int mhz)
{
    /* Apple's rule [apple, mac-and-dvs.md 3.3]: period = floor(10^6 /
     * f_kHz) ns, n = floor(89 / period), capped at 15, written when
     * n > 3.  Never less than Rockbox's value at or below the clock (run
     * daily [silicon]): 0x0303, and 0x0808 from 60 MHz (where Apple has
     * 0x0505-0x0707; at 100 MHz both are 0x0808) */
    uint32_t period = 1000000u / (mhz * 1000u);
    uint32_t n = period ? 89u / period : 15u;
    uint32_t floor = mhz >= 60 ? 8 : 3;

    if (n > 15)
        n = 15;
    if (n < floor)
        n = floor;
    return (n << 8) | n;
}

unsigned int pgov_grade_mhz(uint32_t strap_opt_a)
{
    static const uint8_t grade[4] = { 133, 100, 80, 66 };
    return grade[(strap_opt_a >> 5) & 3];
}

/* ---- The persistent record (section 9.4) ------------------------------- */

static unsigned int bits5(unsigned int h)
{
    unsigned int n = 0;
    for (h &= 0x1f; h; h &= h - 1)
        n++;
    return n;
}

bool pgov_rec_boot(struct pgov_record *r, bool prev_clean)
{
    if (r->armed)
    {
        /* the last boot ran armed and never reached its clean end */
        r->armed = 0;
        r->hist = ((r->hist << 1) | (prev_clean ? 0 : 1)) & 0x1f;
        if (!prev_clean)
        {
            if (bits5(r->hist) >= 2)
            {
                r->dvs = PGOV_REC_OFF;
                r->code = PGOV_FB_UNCLEAN;
            }
            else
                r->skip = 1;
        }
    }
    if (r->dvs == PGOV_REC_OFF)
        return false;
    if (r->skip)
    {
        r->skip = 0;
        return false;
    }
    return true;
}

void pgov_rec_armed(struct pgov_record *r)
{
    r->armed = 1;
    if (r->dvs == PGOV_REC_UNTRIED)
        r->dvs = PGOV_REC_TRIAL;
}

void pgov_rec_fail(struct pgov_record *r, unsigned int code)
{
    r->dvs = PGOV_REC_OFF;
    r->code = code;
    r->armed = 0;
}

void pgov_rec_clean_end(struct pgov_record *r, unsigned long low_s)
{
    if (!r->armed)
        return;
    r->armed = 0;
    r->hist = (r->hist << 1) & 0x1f;
    if (low_s >= 3600 && r->ok_boots < 255 && ++r->ok_boots >= 10
        && r->dvs == PGOV_REC_TRIAL)
        r->dvs = PGOV_REC_OK;
}

void pgov_rec_reset(struct pgov_record *r)
{
    r->dvs = PGOV_REC_UNTRIED;
    r->b3 = PGOV_REC_UNTRIED;
    r->code = PGOV_FB_NONE;
    r->hist = r->armed = r->skip = r->ok_boots = 0;
}

/* ---- Stage 2: the apply layer (section 16) ----------------------------- */
#ifdef PGOV_DVS     /* the host and developer builds; release has none */

unsigned int pgov_core_cap(struct pgov_core *c, unsigned int cap_mhz)
{
    unsigned int m = norm_point(c, cap_mhz);

    if (cap_mhz && c->mhz > m)
    {
        move(c, m, PGOV_WHY_CAP);
        c->below_ms = 0;
        c->sat_run = 0;
    }
    return c->mhz;
}

unsigned int pgov_hw_ceil(const struct pgov_hw *hw, uint32_t now_us)
{
    if (hw->vcode != PGOV_V_NORM
        || (uint32_t)(now_us - hw->v_at_us) < PGOV_SETTLE_US)
        return PGOV_MHZ_LOW;
    return pgov_vceil_mhz(hw->live, PGOV_V_NORM);
}

void pgov_hw_next(const struct pgov_hw *hw, unsigned int mhz,
                  unsigned int vcode, uint32_t now_us, struct pgov_op *op)
{
    unsigned int ceil = pgov_vceil_mhz(hw->live, vcode);

    op->op = PGOV_OP_NONE;
    op->arg = 0;
    op->wait_us = 0;
    if (vcode != PGOV_V_NORM && vcode != PGOV_V_LOW)
        vcode = PGOV_V_NORM;            /* never a third code */
    if (mhz > ceil)
        mhz = ceil;                     /* invariant 1, whatever is asked */

    /* 1. the voltage up first (a raise is always safe) */
    if (vcode == PGOV_V_NORM && hw->vcode != PGOV_V_NORM)
    {
        op->op = PGOV_OP_VUP;
        op->arg = PGOV_V_NORM;
        return;
    }
    /* 2. the clock up, once the rail carries it (the settle) */
    if (mhz > hw->mhz)
    {
        unsigned int now_ceil = pgov_hw_ceil(hw, now_us);

        if (mhz > now_ceil && hw->vcode == PGOV_V_NORM)
        {
            uint32_t left = PGOV_SETTLE_US - (now_us - hw->v_at_us);

            if (now_ceil > hw->mhz)
            {
                op->op = PGOV_OP_CLOCK;     /* what it carries now */
                op->arg = now_ceil;
                return;
            }
            op->op = PGOV_OP_WAIT;
            op->wait_us = left > 0xffff ? 0xffff : left;
            return;
        }
        op->op = PGOV_OP_CLOCK;
        op->arg = mhz > now_ceil ? now_ceil : mhz;
        if (op->arg <= hw->mhz)
            op->op = PGOV_OP_NONE;      /* nothing more it may do now */
        return;
    }
    /* 3. the clock down (always safe) */
    if (mhz < hw->mhz)
    {
        op->op = PGOV_OP_CLOCK;
        op->arg = mhz;
        return;
    }
    /* 4. the voltage down last, the clock already within the low code */
    if (vcode == PGOV_V_LOW && hw->vcode != PGOV_V_LOW
        && (hw->live & PGOV_LIVE_B1)
        && hw->mhz <= pgov_vceil_mhz(hw->live, PGOV_V_LOW))
    {
        op->op = PGOV_OP_VDOWN;
        op->arg = PGOV_V_LOW;
    }
}

unsigned int pgov_dog_step(struct pgov_dog *d, bool owed, uint32_t now_ms)
{
    if (!owed)
    {
        d->owed = 0;
        d->fired = 0;
        return PGOV_DOG_OK;
    }
    if (!d->owed)
    {
        d->owed = 1;
        d->fired = 0;
        d->t0_ms = now_ms;
        return PGOV_DOG_OK;
    }
    if (d->fired || (uint32_t)(now_ms - d->t0_ms) < PGOV_DOG_MS)
        return PGOV_DOG_OK;
    d->fired = 1;
    return PGOV_DOG_FIRE;
}

bool pgov_vdown_recheck(unsigned int mode, const struct pgov_hw *hw,
                        unsigned int want_vcode, unsigned int stor_busy,
                        unsigned int cur_mhz)
{
    return mode == PGOV2_APPLY && hw->vcode == PGOV_V_LOW
        && want_vcode == PGOV_V_LOW && !stor_busy
        && (hw->live & PGOV_LIVE_B1)
        && cur_mhz <= pgov_vceil_mhz(hw->live, PGOV_V_LOW);
}

unsigned int pgov_cop_boost_mhz(const struct pgov_hw *hw, uint32_t now_us)
{
    unsigned int ceil = pgov_hw_ceil(hw, now_us);

    return ceil < PGOV_MHZ_TOP ? ceil : PGOV_MHZ_TOP;
}

unsigned int pgov_reboot_action(bool rec_rw, unsigned int hw_vcode,
                                bool can_lock)
{
    if (!rec_rw || hw_vcode == PGOV_V_NORM)
        return PGOV_RB_NONE;
    return can_lock ? PGOV_RB_LOCKED : PGOV_RB_RAW;
}

bool pgov_b3_check(uint32_t loop_us, uint32_t crc80, uint32_t crc100)
{
    return loop_us >= PGOV_B3_LOOP_MIN_US && loop_us <= PGOV_B3_LOOP_MAX_US
        && crc80 == crc100;
}

#define PGOV_REC_MAGIC  0x56474f50u     /* "POGV" little-endian */
#define PGOV_REC_VER    1

static uint32_t rec_sum(const uint8_t *b, unsigned int n)
{
    uint32_t s = 0x9e3779b9u;           /* a checksum, not a CRC: 12 B */

    while (n--)
        s = (s ^ *b++) * 16777619u;
    return s;
}

static void put32(uint8_t *b, uint32_t v)
{
    b[0] = v; b[1] = v >> 8; b[2] = v >> 16; b[3] = v >> 24;
}

static uint32_t get32(const uint8_t *b)
{
    return b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16
         | (uint32_t)b[3] << 24;
}

unsigned int pgov_rec_pick(const uint8_t *main, unsigned int main_len,
                           const uint8_t *tmp, unsigned int tmp_len,
                           struct pgov_record *r)
{
    if (pgov_rec_unpack(tmp, tmp_len, r))
        return 2;
    if (pgov_rec_unpack(main, main_len, r))
        return 1;
    return 0;                       /* reset by the last unpack */
}

uint32_t pgov_hw_settle_left(const struct pgov_hw *hw, uint32_t now_us)
{
    uint32_t el = now_us - hw->v_at_us;

    if (hw->vcode != PGOV_V_NORM || el >= PGOV_SETTLE_US)
        return 0;
    return PGOV_SETTLE_US - el;
}

void pgov_rec_pack(const struct pgov_record *r,
                   uint8_t buf[PGOV_REC_FILE_SIZE])
{
    put32(buf, PGOV_REC_MAGIC);
    buf[4] = r->dvs; buf[5] = r->b3; buf[6] = r->code; buf[7] = r->hist;
    buf[8] = r->armed; buf[9] = r->skip; buf[10] = r->ok_boots;
    buf[11] = PGOV_REC_VER;
    put32(buf + 12, rec_sum(buf, 12));
}

bool pgov_rec_unpack(const uint8_t *buf, unsigned int len,
                     struct pgov_record *r)
{
    if (len != PGOV_REC_FILE_SIZE || get32(buf) != PGOV_REC_MAGIC
        || buf[11] != PGOV_REC_VER || get32(buf + 12) != rec_sum(buf, 12)
        || buf[4] > PGOV_REC_OFF || buf[5] > PGOV_REC_OFF)
    {
        pgov_rec_reset(r);
        return false;
    }
    r->dvs = buf[4]; r->b3 = buf[5]; r->code = buf[6]; r->hist = buf[7];
    r->armed = buf[8]; r->skip = buf[9]; r->ok_boots = buf[10];
    r->pad = 0;
    return true;
}
#endif /* PGOV_DVS */
