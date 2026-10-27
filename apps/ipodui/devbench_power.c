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

/* The bench session's battery rows (devbench_power.h): the device side.
 * Called from devbench.c's step table on the UI thread; each returns an
 * enum bench_res.  Lines go through devbcm_log() (bcmtest.log and the
 * step's hook, so the results file keeps them); power.csv gets a mark at
 * every phase.  Every row puts back what it found.
 *
 * The owner's script: the step's start screen (devbench.c's table) says
 * what to set up; the texts below are the phase screen and the questions.
 * scripts/bench_script.py copies them into bench-p19.md, "Owner's
 * script" (keep them string literals: it reads them from this file). */
#include <stdio.h>
#include <string.h>
#include "config.h"

#if defined(HAVE_BCMFW_RESIDENT) && defined(HAVE_BCMFW_VIDEO) && \
    !defined(SIMULATOR)
#include "system.h"
#include "kernel.h"
#include "button.h"
#include "lcd.h"
#include "backlight.h"
#include "bcm-devtest.h"
#include "devbcm.h"
#include "devbench.h"
#include "devbench_power.h"
#include "ipodui/powerlog.h"
#include "ipodui/clockpol.h"
#ifdef HAVE_BCM_JOBS
#include "bcmjob_pp.h"
#include "bcmjob.h"
#endif

/* ---- the phases --------------------------------------------------------- */

struct bpw_run
{
    const char *title;          /* the screen's title */
    const char *tag;            /* the power.csv marks' prefix, <= 8 */
    const char *what[2];        /* each half's name: A, B */
    int (*apply)(bool b, void *ctx);    /* 0 ok */
    void (*sample)(int k, bool b, uint32_t ms, void *ctx);
    void *ctx;
    /* optional: called in the phase's wait loop; returns the ms to wait
       for a button at most (0: the usual 2 s) */
    int (*poll)(bool b, void *ctx);
};

#ifdef HAVE_IPOD_UI_DEVMODE
static bool log_was_on;
static int log_was_secs;
#endif

/* power.csv on at 5 s rows for the row; put back after */
static void log_begin(void)
{
#ifdef HAVE_IPOD_UI_DEVMODE
    log_was_on = powerlog_is_on();
    log_was_secs = powerlog_interval();
    powerlog_set_interval(5);
    if (!log_was_on)
        powerlog_set(true);
#endif
}

static void log_end(void)
{
#ifdef HAVE_IPOD_UI_DEVMODE
    if (!log_was_on)
        powerlog_set(false);
    powerlog_set_interval(log_was_secs);
#endif
}

/* The phase screen, drawn once a phase (still: nothing else runs).  Its
   first lines are built: the title, "Phase 2 of 4 (B), 20 s:" and the
   phase's state; then these, one a row */
static const char *const bpw_phase_lines[] =
{
    "Leave it alone until the",
    "row ends. Keep the iPod",
    "unplugged (on battery).",
    "",
    "MENU: ends this row early",
    "(skipped; the session goes",
    "on). Other buttons: nothing.",
};

static void draw(const struct bpw_run *r, int k, int left_s)
{
    char line[40];
    int j;

    lcd_clear_display();
    lcd_puts(0, 0, r->title);
    snprintf(line, sizeof(line), "Phase %d of %d (%s), %d s:", k + 1,
             BPW_PHASES, bpw_phase_b(k) ? "B" : "A", left_s);
    lcd_puts(0, 2, line);
    lcd_puts(0, 3, r->what[bpw_phase_b(k)]);
    for (j = 0; j < (int)ARRAYLEN(bpw_phase_lines); j++)
        lcd_puts(0, 5 + j, bpw_phase_lines[j]);
    lcd_update();
}

/* A B A B, BPW_PHASE_S each, the screen drawn once a phase (a still
 * screen: nothing but the measured state runs).  false: MENU, or a phase
 * could not be applied */
static bool run_phases(const struct bpw_run *r)
{
    char tag[24], line[80];
    int k;

    for (k = 0; k < BPW_PHASES; k++)
    {
        bool b = bpw_phase_b(k);
        long t0, end;
        int rc = r->apply(b, r->ctx);

        snprintf(tag, sizeof(tag), "%s %c%d", r->tag, b ? 'B' : 'A',
                 k / 2 + 1);
        if (rc)
        {
            snprintf(line, sizeof(line), "%s: could not apply %c (rc %d)",
                     r->tag, b ? 'B' : 'A', rc);
            devbcm_log(line);
            return false;
        }
        powerlog_mark(tag);
        draw(r, k, BPW_PHASE_S);
        t0 = current_tick;
        end = t0 + BPW_PHASE_S * HZ;
        while (TIME_BEFORE(current_tick, end))
        {
            int ms = r->poll ? r->poll(b, r->ctx) : 0;
            int btn = button_get_w_tmo(ms > 0 ? ms * HZ / 1000 + 1 : 2 * HZ);

            backlight_on();         /* lit throughout, no redraw */
            if (btn == BUTTON_MENU)
            {
                snprintf(line, sizeof(line), "%s: stopped (MENU)", r->tag);
                devbcm_log(line);
                return false;
            }
        }
        r->sample(k, b, (uint32_t)(current_tick - t0) * (1000 / HZ), r->ctx);
    }
    powerlog_mark(r->tag);          /* the end */
    return true;
}

/* ---- slp: the idle sleep ------------------------------------------------ */

struct slp_ctx
{
    uint32_t c0[4];
    bool ok, read_failed;
    int b_ok;
    /* the PP's doorbells in this B phase: rung, and when the next is due */
    uint32_t rings, seed;
    long next;
    bool by_fence;
    /* each timed FENCE's completion (us, ring to J_FENCE), this phase;
     * the A phases' median is the polling baseline */
    uint16_t lat[BPW_LAT_MAX];
    uint32_t nlat, p50_a;
    bool have_a;
};

/* A known number of doorbells at random intervals (0.1 to 0.8 s) through
 * each B phase, to count against the image's bells.  With the job driver
 * up, each ring is a FENCE record (the driver's own doorbell path; no
 * ring is left without a record, and its completion is counted); else
 * one bare doorbell (BCM_CONTROL = 0x31: the image wakes, finds no new
 * command and sleeps again).  Returns the ms to the next ring */
static int slp_poll(bool b, void *v)
{
    struct slp_ctx *c = v;
    long now = current_tick;
    bool sent = false;
#ifdef HAVE_BCM_JOBS
    uint32_t seq;
#endif

    if (TIME_BEFORE(now, c->next))
        return (int)((c->next - now) * (1000 / HZ));
    c->seed = c->seed * 1664525u + 1013904223u;
    c->next = now + HZ / 10 + (long)((c->seed >> 16) % (7 * HZ / 10));
#ifdef HAVE_BCM_JOBS
    {
        uint32_t t0 = USEC_TIMER;

        if (bcmjob_have(J_OP_FENCE) && bcmjob_fence(&seq) == BCMJOB_OK)
        {
            uint32_t us;

            c->by_fence = true;
            sent = true;
            if (bcmjob_wait(seq, J_T_FENCE_BASE) != BCMJOB_OK)
                c->ok = false;      /* a no-op that never completed */
            us = USEC_TIMER - t0;
            if (c->nlat < BPW_LAT_MAX)
                c->lat[c->nlat++] = us > 0xFFFF ? 0xFFFF : (uint16_t)us;
        }
    }
#endif
    if (!b && !sent)
        return (int)((c->next - current_tick) * (1000 / HZ));
    if (!sent && bcm_boot_res_bell() == 0)
        sent = true;
    if (sent && b)
        c->rings++;
    return (int)((c->next - current_tick) * (1000 / HZ));
}

static int slp_apply(bool b, void *v)
{
    struct slp_ctx *c = v;
    uint32_t reg;

    if (bcm_boot_res_live(b ? 1 : 0, 0, NULL) < 0)
        return -1;
    if (bcm_boot_res_read(c->c0, &reg) < 0)
        return -2;
    c->rings = 0;
    c->nlat = 0;
    c->next = current_tick + HZ / 2;
    return 0;
}

static void slp_sample(int k, bool b, uint32_t ms, void *v)
{
    struct slp_ctx *c = v;
    uint32_t c1[4], reg, d[4];
    char line[100];
    int j;

    if (bcm_boot_res_read(c1, &reg) < 0)
    {
        c->read_failed = true;
        c->ok = false;
        snprintf(line, sizeof(line), "slp %c%d: FAIL: the image no longer "
                 "answers", b ? 'B' : 'A', k / 2 + 1);
        devbcm_log(line);
        return;
    }
    for (j = 0; j < 4; j++)
        d[j] = c1[j] - c->c0[j];
    snprintf(line, sizeof(line), "slp %c%d ms=%lu ticks=%lu bells=%lu "
             "sleeps=%lu bad=%lu", b ? 'B' : 'A', k / 2 + 1,
             (unsigned long)ms, (unsigned long)d[0], (unsigned long)d[1],
             (unsigned long)d[2], (unsigned long)d[3]);
    devbcm_log(line);
    if (!b)
    {
        /* the polling baseline: the A phases' FENCE median */
        uint32_t p50 = bpw_quantile(c->lat, c->nlat, 50);

        snprintf(line, sizeof(line), "slp A%d lat_p50=%lu lat_p90=%lu n=%lu",
                 k / 2 + 1, (unsigned long)p50,
                 (unsigned long)bpw_quantile(c->lat, c->nlat, 90),
                 (unsigned long)c->nlat);
        devbcm_log(line);
        if (c->nlat)
        {
            c->p50_a = c->have_a && c->p50_a > p50 ? c->p50_a : p50;
            c->have_a = true;
        }
    }
    if (b)
    {
        bool bells = bpw_bells_ok(c->rings, d[1], d[3]);
        uint32_t i, under = 0, p50, p90;
        bool lat;

        for (i = 0; i < c->nlat; i++)
            under += c->lat[i] < BPW_LAT_UNDER_US;
        p50 = bpw_quantile(c->lat, c->nlat, 50);
        p90 = bpw_quantile(c->lat, c->nlat, 90);
        lat = c->have_a && bpw_lat_ok(c->nlat, under, p50, c->p50_a);
        snprintf(line, sizeof(line), "slp B%d lat_p50=%lu lat_p90=%lu "
                 "under_tick=%lu/%lu base_p50=%lu", k / 2 + 1,
                 (unsigned long)p50, (unsigned long)p90,
                 (unsigned long)under, (unsigned long)c->nlat,
                 (unsigned long)c->p50_a);
        devbcm_log(line);
        if (!lat)
        {
            snprintf(line, sizeof(line), "slp B%d: FAIL: %s", k / 2 + 1,
                     !c->by_fence || c->nlat < BPW_LAT_MIN
                     ? "too few timed FENCEs (needs the job driver)"
                     : "FENCEs wait for the tick: the doorbell didn't wake "
                       "the chip");
            devbcm_log(line);
            c->ok = false;
        }

        snprintf(line, sizeof(line), "slp B%d rings=%lu bells=%lu lost=%lu "
                 "via=%s", k / 2 + 1, (unsigned long)c->rings,
                 (unsigned long)d[1],
                 (unsigned long)bpw_bells_lost(c->rings, d[1]),
                 c->by_fence ? "fence" : "bell");
        devbcm_log(line);
        if (!bells)
        {
            snprintf(line, sizeof(line), "slp B%d: FAIL: %s", k / 2 + 1,
                     !c->rings ? "no doorbell could be rung"
                     : d[3] ? "the image's bad sleeps went up"
                            : "a doorbell was lost");
            devbcm_log(line);
        }
        if (bpw_idle_ok(d[0], d[2], d[3], ms) && bells && lat)
            c->b_ok++;
        else
            c->ok = false;
    }
}

int devbench_pwr_idle(void)
{
    struct slp_ctx c;
    struct bpw_run r = { "Chip idle sleep", "slp",
                         { "A: chip polls (today)",
                           "B: chip sleeps between ticks" },
                         slp_apply, slp_sample, NULL, slp_poll };
    bool idle0, lvl0, done;
    char line[80];

    if (!bcm_boot_resident_active())
    {
        devbcm_log("slp: Not available: the resident video image isn't "
                   "the display driver");
        return BENCH_SKIP;
    }
    memset(&c, 0, sizeof(c));
    c.ok = true;
    c.seed = USEC_TIMER | 1;
    r.ctx = &c;
    bcm_boot_res_now(&idle0, &lvl0);
    log_begin();
    done = run_phases(&r);
    if (!c.read_failed)
        bcm_boot_res_live(idle0 ? 1 : 0, 0, NULL);
    log_end();
    snprintf(line, sizeof(line), "slp: %s (B phases woken on the 1 ms "
             "tick and by the doorbell, none lost: %d of 2; gate back %s)",
             done && c.ok && c.b_ok == 2 ? "PASS" : done ? "FAIL" : "Not run",
             c.b_ok, idle0 ? "on" : "off");
    devbcm_log(line);
    if (!done && !c.read_failed)
        return BENCH_SKIP;
    return c.ok && c.b_ok == 2 ? BENCH_PASS : BENCH_FAIL;
}

/* ---- lvl5: the core level ----------------------------------------------- */

struct lvl_ctx
{
    int level0;                 /* the level found (11 after a host start) */
    bool ok;
    int self_ok;
};

static int lvl_apply(bool b, void *v)
{
    struct lvl_ctx *c = v;
    int rb;

    if (bcm_boot_res_live(-1, b ? 5 : c->level0, &rb) < 0)
        return -1;
    return 0;
}

static void lvl_sample(int k, bool b, uint32_t ms, void *v)
{
    struct lvl_ctx *c = v;
    uint32_t cnt[4], reg;
    char line[100];
    int st = 1;

    (void)ms;
    if (bcm_boot_res_read(cnt, &reg) < 0)
    {
        c->ok = false;
        snprintf(line, sizeof(line), "lvl5 %c%d: FAIL: the image no longer "
                 "answers", b ? 'B' : 'A', k / 2 + 1);
        devbcm_log(line);
        return;
    }
#ifdef HAVE_BCM_JOBS
    if (b)
        st = bcmjob_selftest();     /* jobs and the vector unit at level 5 */
#endif
    if ((int)(reg & 0xF) != (b ? 5 : c->level0) || st < 0)
        c->ok = false;
    else if (b)
        c->self_ok++;
    snprintf(line, sizeof(line), "lvl5 %c%d reg %08lx (level %lu) jobs "
             "self-test %d", b ? 'B' : 'A', k / 2 + 1, (unsigned long)reg,
             (unsigned long)(reg & 0xF), b ? st : 99);
    devbcm_log(line);
}

/* lvl5's go: the risk in one line, and what each answer does */
#define BPW_GO_TEXT "Run the core voltage test? For half of its 80 s the " \
    "video chip's core runs at level 5 (Apple's value), lower than " \
    "today's 11.\n\nRisk: the screen may garble or freeze; holding MENU " \
    "and SELECT restarts the iPod and undoes it.\n\nYes, run it: starts " \
    "it. Not now (or MENU): skips this row; the session goes on."

int devbench_pwr_level(void)
{
    static const char *const go[2] = { "Yes, run it", "Not now" };
    struct lvl_ctx c;
    struct bpw_run r = { "Chip core level 5", "lvl5",
                         { "A: core at level 11 (today)",
                           "B: core at level 5" },
                         lvl_apply, lvl_sample, NULL, NULL };
    uint32_t cnt[4], reg;
    bool idle0, lvl0, done;
    char line[96];

    if (!bcm_boot_resident_active() || bcm_boot_res_read(cnt, &reg) < 0)
    {
        devbcm_log("lvl5: Not available: the resident video image isn't "
                   "the display driver");
        return BENCH_SKIP;
    }
    if (devbench_ask(r.title, BPW_GO_TEXT, go, 2) != 0)
    {
        devbcm_log("lvl5: Not run (no go)");
        return BENCH_SKIP;
    }
    memset(&c, 0, sizeof(c));
    c.ok = true;
    c.level0 = (reg & 0xF) >= 5 ? (int)(reg & 0xF) : 11;
    r.ctx = &c;
    bcm_boot_res_now(&idle0, &lvl0);
    log_begin();
    bcm_boot_res_live(0, 0, NULL);  /* polling: the level's own step */
    done = run_phases(&r);
    /* back as found: the level (and the setting's RAM copy), the gate */
    bcm_boot_res_live(idle0 ? 1 : 0, lvl0 ? 5 : c.level0, NULL);
    log_end();
    snprintf(line, sizeof(line), "lvl5: %s (level 5 held with the self-test "
             "%d of 2; found %d, back to %d)",
             done && c.ok && c.self_ok == 2 ? "PASS" : done ? "FAIL"
                                                      : "Not run",
             c.self_ok, c.level0, lvl0 ? 5 : c.level0);
    devbcm_log(line);
    if (!done && c.ok)
        return BENCH_SKIP;
    return c.ok && c.self_ok == 2 ? BENCH_PASS : BENCH_FAIL;
}

/* ---- lit24: the PP's lit clock ------------------------------------------ */

#ifdef HAVE_PLL_IDLE
struct lit_ctx
{
    bool ok;
};

static int lit_apply(bool b, void *v)
{
    (void)v;
    clockpol_lit24_set(b);
    return 0;
}

static void lit_sample(int k, bool b, uint32_t ms, void *v)
{
    struct lit_ctx *c = v;
    struct cpu_pll_stats ps;
    long mhz = cpu_frequency / 1000000;
    char line[96];

    (void)ms;
    cpu_pll_get_stats(&ps);
    if (!bpw_lit_ok(b, mhz))
        c->ok = false;
    snprintf(line, sizeof(line), "lit24 %c%d MHz=%ld holds=%x policy=%s",
             b ? 'B' : 'A', k / 2 + 1, mhz, ps.holds,
             clockpol_enabled() ? "on" : "off");
    devbcm_log(line);
}
#endif

int devbench_pwr_lit(void)
{
#ifdef HAVE_PLL_IDLE
    struct lit_ctx c = { true };
    struct bpw_run r = { "Lit clock 24 MHz", "lit24",
                         { "A: clock at 30 MHz (today)", "B: clock at 24 MHz" },
                         lit_apply, lit_sample, NULL, NULL };
    bool sw0 = clockpol_lit24(), done;
    char line[96];

    r.ctx = &c;
    log_begin();
    done = run_phases(&r);
    clockpol_lit24_set(sw0);
    log_end();
    /* a FAIL with holds set is the setup (USB data, a heavy format), not
       the policy: the line says which */
    snprintf(line, sizeof(line), "lit24: %s (B at 24 MHz, A at 30; switch "
             "back to %d)", done && c.ok ? "PASS" : done ? "FAIL" : "Not run",
             sw0 ? 24 : 30);
    devbcm_log(line);
    if (!done)
        return BENCH_SKIP;
    return c.ok ? BENCH_PASS : BENCH_FAIL;
#else
    devbcm_log("lit24: Not available: no lit clock policy in this build");
    return BENCH_SKIP;
#endif
}
#endif /* HAVE_BCMFW_RESIDENT && !SIMULATOR */
