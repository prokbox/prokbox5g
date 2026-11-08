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

#ifndef _IPODUI_DEVBENCH_VCPWR_H_
#define _IPODUI_DEVBENCH_VCPWR_H_

/* p19-vcpwr: the video chip's awake-time power levers as bench rows and a
 * developer-build watch (docs/bcm2722-reference/power-apple-vs-ours.md,
 * "The top five"; bench-p19.md "The VC power rows").  Apple shows how to
 * drive each (the orders and the settles, [apple-RE] rows cited below);
 * our design decides what runs.  None writes the merged image: each goes
 * through the port (registers, SDRAM words) or the 0x16C gate word, as
 * slp and lvl5 do.
 *
 *   s1  the resident image's idle sleep in the lit shell, A B A B; PASS
 *       saves "idle on" (bcmres.cfg: the daily build's switch)
 *   l5  its core at level 5 on the crystal, A B A B, each B with Apple's
 *       10 ms settle, the read-back, the job self-test and a 2 s
 *       heartbeat; PASS saves "level 5"
 *   vs  idle sleep inside a video session, clips 1 and 2, A B A B
 *   ck  the block clocks: a census, then ids 2 and 3 off, then id 6 last
 *   sd  Apple's SDRAM idle (command register 0x1000_3420) after a video
 *
 * The watch (vcpwr_service(), developer builds, UI thread): while S1 or
 * L5 is on, it checks them every second and after every wake, and on a
 * failure latches the lever off for the boot (and saves it off: L5 at
 * once, S1 after VCP_S1_STRIKES boots), logging one loud line.
 *
 * This header is the pure part (rules, decodes, the picks), host-tested
 * in tests/autochecks; devbench_vcpwr.c is the device side and the
 * simulator's stubs (which never model a pass). */

#include <stdbool.h>
#include <stdint.h>
#include "devbench.h"

/* ---- the phases ---- */
#define VCP_PHASES          4       /* A B A B */
#define VCP_PHASE_S         20      /* s1, l5 */
#define VCP_VS_S            20      /* each vs play */
#define VCP_CK_HOLD_S       40      /* ck: the baseline and each step */
#define VCP_SD_PLAY_S       4       /* sd: the play that brings SDRAM up */

static inline bool vcp_phase_b(int k)
{
    return k & 1;
}

/* ---- S1: the idle counters over a window (the watch: 1 s; s1: 20 s) ----
 * Returns NULL when the window is healthy, else the reason (the log's
 * why=).  ticks: the 1 ms timer-compare wakes; ms: the window; sleeps,
 * bad: the image's counters (bad: a sleep that disturbed r0-r5).
 * Batch 5's adaptive tick (bcmfw config.h BCMFW_IDLE_ADAPT, merged image
 * b9bcbe57 on): with the gate on, the doorbell seen and nothing pending,
 * the tick is 10 ms, so an idle lit shell counts about ms / 10 ticks and
 * a window that mixes the two rates lands between.  The rule takes any
 * count from 90 % of the 10 ms rate to 110 % of the 1 ms rate: too few
 * still says a lost re-arm, too many a runaway tick */
#define VCP_S1_STRIKES      3       /* failing boots before it is saved off */
#define VCP_READ_FAILS      2       /* reads in a row before a failure */

/* in_session: a video session is open over the whole window, so the
   tick stays at 1 ms (the adaptive tick's "pending"): 90 % of ms up */
static inline const char *vcp_idle_why(uint32_t ticks, uint32_t sleeps,
                                       uint32_t bad, uint32_t ms,
                                       bool in_session)
{
    if (bad)
        return "bad";
    if (!ms || ticks * (in_session ? 10ull : 100ull) < ms * 9ull ||
        ticks * 10ull > ms * 11ull)
        return "ticks";
    if (!sleeps)
        return "nosleep";
    return NULL;
}

static inline const char *vcp_s1_why(uint32_t ticks, uint32_t sleeps,
                                     uint32_t bad, uint32_t ms)
{
    return vcp_idle_why(ticks, sleeps, bad, ms, false);
}

/* ---- L5: one application (the swap, a wake, a B phase) ----
 * rb: the level read back after the settle (-1 unread); st: the job
 * self-test (0 passed, 1 jobs not up: not a failure by itself, < 0
 * failed); hb: the heartbeat held for VCP_HB_MS (no chip failure, the
 * image not stopped, a FENCE answered) */
#define VCP_HB_MS           2000

static inline const char *vcp_l5_why(int rb, int st, bool hb)
{
    if (rb != 5)
        return "readback";
    if (st < 0)
        return "selftest";
    if (!hb)
        return "heartbeat";
    return NULL;
}

/* ---- VS: a B phase against its A (power-apple-vs-ours.md row VS) ----
 * In a B phase: bad 0, slept, and ticks within 10 % of ms times the
 * chip's idle share (the image takes interrupts only inside its sleep,
 * so a busy chip takes no tick: sitting 4gh, quirk #57, ticks = sleeps
 * and ticks / ms = idle share within 3 % in all four B phases); no
 * skipped or dropped picture; late pictures at most A's + 1; A/V and
 * band moves no worse than A's (the row's own seek and pause move both
 * in A too: 4gh's A phases read 70-87 ms and 24-33 moves) beyond stage
 * 6's 50 ms and 20 a minute; the top band again after the seek.  The
 * gate's effect is the sleeps (B) against none (A): the idle share is
 * the chip's work, which sleeping does not change */
#define VCP_VS_AV_US        50000
#define VCP_VS_MOVES_MIN    20

struct vcp_vs_phase
{
    uint32_t ticks, sleeps, bad, bells, ms;     /* the idle counters */
    uint32_t skipped, dropped, late;            /* the trace's */
    int32_t av_us;                              /* its A/V max */
    uint32_t moves, gov_ms, idle_pm;            /* the governor's */
    bool raced, failed;
};

static inline const char *vcp_vs_why(const struct vcp_vs_phase *b,
                                     const struct vcp_vs_phase *a)
{
    if (b->failed)
        return "play";
    uint64_t want = (uint64_t)b->ms * b->idle_pm;     /* ticks x 1000 */

    if (b->bad)
        return "bad";
    if (!b->ms || !b->idle_pm || (uint64_t)b->ticks * 10000 < want * 9 ||
        (uint64_t)b->ticks * 10000 > want * 11)
        return "ticks";
    if (!b->sleeps)
        return "nosleep";
    if (b->skipped || b->dropped)
        return "skipped";
    if (b->late > a->late + 1)
        return "late";
    if (b->av_us > VCP_VS_AV_US && b->av_us > a->av_us + VCP_VS_AV_US / 2)
        return "av";
    if (!b->gov_ms ||
        ((uint64_t)b->moves * 60000 >
         (uint64_t)VCP_VS_MOVES_MIN * b->gov_ms &&
         b->moves > a->moves + a->moves / 4))
        return "moves";
    if (!b->raced)
        return "top";
    return NULL;
}

/* the gate took effect: B slept, A did not */
static inline bool vcp_vs_effect(const struct vcp_vs_phase *b,
                                 const struct vcp_vs_phase *a)
{
    return b->sleeps && !a->sleeps;
}

/* ---- CK: the clock words (brief-map-power.md section 5, [apple-RE]
 * @4d96, @4be6, @5334): [7:0] the divider, [10:8] the source; the core
 * word +0x28 reads 0 as the crystal ---- */
#define VCP_CLK_BASE        0x10001C00u
#define VCP_CLK_FIRST       0x28    /* +28 .. +58, one word each 4 */
#define VCP_CLK_LAST        0x58
#define VCP_CLK_N           ((VCP_CLK_LAST - VCP_CLK_FIRST) / 4 + 1)
#define VCP_CLK_COUNTER     0x4C    /* the 1 MHz counter: never written */

static inline uint32_t vcp_clk_src(uint32_t w)
{
    return (w >> 8) & 7;
}

static inline uint32_t vcp_clk_div(uint32_t w)
{
    return w & 0xFF;
}

/* off: source "none" (or a zero word); ids 0, 1, 4, 5, 7 are the batch's
 * BCMFW_JOBS_CLK_OFF 0xE3 */
static inline bool vcp_clk_off(uint32_t off, uint32_t w)
{
    return off != 0x28 && off != 0x58 && vcp_clk_src(w) == 0;
}

/* a step's hold: its FENCEs within 5 % of the unwritten baseline's, all
 * answered; the self-test passed; the read-back what was written */
static inline bool vcp_ck_step_ok(uint32_t wrote, uint32_t read, int st,
                                  uint32_t fences, uint32_t fences_base,
                                  uint32_t lost)
{
    return read == wrote && st >= 0 && !lost && fences_base &&
           fences * 100ull >= fences_base * 95ull;
}

/* ---- SD: Apple's SDRAM idle (@14892: the cache flushed, cache-control
 * bits 30 and 16 cleared, command register +0x20 = 0, bit 15 polled up
 * to 8,000 times) ---- */
#define VCP_SD_CMD          0x10003420u
#define VCP_SD_BUSY         0x8000u
#define VCP_SD_POLLS        8000
#define VCP_SD_PATTERN      0xFE3F6000u     /* PW5's window */
#define VCP_SD_WORDS        1024            /* 4 KB */

static inline uint32_t vcp_sd_word(uint32_t seed, uint32_t i)
{
    uint32_t x = seed ^ (i * 0x9E3779B9u);

    x ^= x >> 15;
    return x * 0x2C1B3C6Du;
}

static inline const char *vcp_sd_why(bool cleared, bool hb1, bool hb10,
                                     bool back, uint32_t wrong)
{
    if (!cleared)
        return "stuck";
    if (!hb1 || !hb10)
        return "heartbeat";
    if (!back)
        return "bringup";
    if (wrong)
        return "pattern";
    return NULL;
}

/* ---- the picks the integrator splices into bench_reverify_picks ----
 * (devbench.h).  s1, l5 and ck want the merged image as the display
 * driver with its jobs up: after vizsoak, before jvpu (everything after
 * jvpu may hand the chip to Apple's program).  vs plays as vgov does:
 * after vgov.  sd ends with a cut (the restore), so it goes after the
 * last step that needs the resident image: after vlong. */
static const struct bench_pick bench_vcpwr_picks[] =
{
    { "s1",     BENCH_R_ALSO, { "jobs" } },
    { "l5",     BENCH_R_ALSO, { "jobs" } },
    { "ck",     BENCH_R_ALSO, { "jobs" } },
    { "vs",     BENCH_R_ALSO, { "s6" } },
    { "sd",     BENCH_R_ALSO, { "jobs" } },
};

/* devbench_vcpwr.c: each row returns an enum bench_res */
int devbench_vcpwr_s1(void);
int devbench_vcpwr_l5(void);
int devbench_vcpwr_vs(void);
int devbench_vcpwr_ck(void);
int devbench_vcpwr_sd(void);
/* the watch: UI thread, every main-loop pass (cheap when off) */
void vcpwr_service(void);

#endif /* _IPODUI_DEVBENCH_VCPWR_H_ */
