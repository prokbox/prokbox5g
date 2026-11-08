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

/* The "Core layout A" rows (devbench_core.h): the device side, following
 * devbench_cop.c's pattern.  Developer builds on the device only
 * (HAVE_CORE_LAYOUT_A, HAVE_PP_IRQ_TALLY); elsewhere each row is a
 * "Not run" skip. */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "system.h"                 /* HAVE_PP_IRQ_TALLY */

#if !defined(SIMULATOR) && defined(HAVE_CORE_LAYOUT_A) && \
    defined(HAVE_BCMFW_VIDEO) && /* devbcm_log, pcmbuf_underruns */ \
    defined(HAVE_COP_JOBS) && defined(HAVE_PP_IRQ_TALLY)
#include "kernel.h"
#include "button.h"
#include "backlight.h"
#include "audio.h"
#include "playlist.h"
#include "pcm.h"
#include "pcm-internal.h"
#include "pcm_sink.h"
#include "pcmbuf.h"
#include "dac_ext.h"
#include "cop_job.h"
#include "core_alloc.h"
#include "fixedpoint.h"
#include "xcore.h"
#include "corelayout.h"
#include "codec_cop.h"
#include "codec_place_core.h"
#include "devbcm.h"
#include "devbench.h"
#include "devbench_core.h"

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
    char line[160];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    devbcm_log(line);
}

/* ---- the playback around a row (devbench_cop.c's) -------------------- */

struct play_spot
{
    bool playing;
    int index;
    unsigned long elapsed, offset;
};

static void play_note(struct play_spot *p)
{
    struct mp3entry *id3 = audio_current_track();

    p->playing = (audio_status() & AUDIO_STATUS_PLAY) && id3;
    p->elapsed = p->playing ? id3->elapsed : 0;
    p->offset = p->playing ? id3->offset : 0;
    playlist_get_resume_info(&p->index);
}

static bool play_from(const struct play_spot *p)
{
    long end;

    if (playlist_amount() <= 0)
        return false;
    playlist_start(p->index < 0 ? 0 : p->index, p->elapsed, p->offset);
    end = current_tick + 5 * HZ;
    while (TIME_BEFORE(current_tick, end))
    {
        if ((audio_status() & AUDIO_STATUS_PLAY) && pcm_is_playing())
            return true;
        sleep(HZ / 10);
    }
    return false;
}

static bool play_stop(void)
{
    long end = current_tick + 3 * HZ;

    audio_stop();
    while (pcm_is_playing() && TIME_BEFORE(current_tick, end))
        sleep(HZ / 20);
    return !pcm_is_playing();
}

/* A wait no button shortens (Hold is on); USB and power-off go back to the
   shell's queue */
static void hands_off_wait(long ticks)
{
    long end = current_tick + ticks;

    while (TIME_BEFORE(current_tick, end))
    {
        long left = end - current_tick;
        long btn = button_get_w_tmo(MAX(1, MIN(left, 10 * HZ)));

        if (btn != BUTTON_NONE && (btn & SYS_EVENT) &&
            (btn == SYS_USB_CONNECTED || btn == SYS_POWEROFF))
        {
            button_queue_post(btn, 0);
            return;
        }
    }
}

/* ---- a worker on the COP (cop_job.h) ---------------------------------- */

#define COP_STACK       1024
#define COP_JOB_MS      1000

static struct cop_worker cw SHAREDBSS_ATTR;
static int cw_h;
static bool cw_up, cw_hung;

static bool cw_open(void)
{
    if (cw_hung)
        return false;
    if (cw_up)
        return true;
    cw_h = core_alloc(COP_STACK);
    if (cw_h <= 0)
    {
        cw_h = 0;
        return false;
    }
    core_pin(cw_h);
    if (!cop_worker_start(&cw, core_get_data(cw_h), COP_STACK, "corelay",
                          PRIORITY_USER_INTERFACE) || !cop_jobs_cross_core())
    {
        cop_worker_stop(&cw);
        cw_hung = true;             /* its stack is kept until the restart */
        return false;
    }
    cw_up = true;
    return true;
}

static void cw_close(void)
{
    if (!cw_up || cw_hung)
        return;
    cop_worker_stop(&cw);
    core_unpin(cw_h);
    core_free(cw_h);
    cw_h = 0;
    cw_up = false;
}

/* fn(arg) on the COP, waited for up to COP_JOB_MS: 0, -1 no worker, -2
   never finished */
static int cw_run(cop_job_fn fn, void *arg)
{
    long end;
    int t;

    if (!cw_up || cw_hung)
        return -1;
    t = cop_job_post(&cw, fn, arg, 0);
    if (t < 0)
        return -1;
    end = current_tick + COP_JOB_MS * HZ / 1000 + 1;
    while (!copj_is_done(&cw.ring, t))
    {
        if (TIME_AFTER(current_tick, end))
        {
            cw_hung = true;
            return -2;
        }
        semaphore_wait(&cw.poster, HZ / 50);
    }
    copj_release(&cw.ring, t);
    return 0;
}

/* ---- k1: the parked COP, A/B ------------------------------------------ */

#define K1_SETTLE_S     30
#define K1_WIN_S        150

struct k1_arm
{
    uint32_t ms, cop, cpu, sent, held;
    unsigned long under, rescue;
    bool playing, hold;
};

static void k1_measure(struct k1_arm *r)
{
    uint32_t t0, c0, p0, s0, h0, s1, h1;
    unsigned long u0 = pcmbuf_underruns(), r0 = pcmbuf_rescues();

    corelayout_park_stats(&s0, &h0);
    c0 = pp_idle_exits[COP];
    p0 = pp_idle_exits[CPU];
    t0 = USEC_TIMER;
    hands_off_wait(K1_WIN_S * HZ);
    r->ms = (USEC_TIMER - t0) / 1000;
    r->cop = pp_idle_exits[COP] - c0;
    r->cpu = pp_idle_exits[CPU] - p0;
    corelayout_park_stats(&s1, &h1);
    r->sent = s1 - s0;
    r->held = h1 - h0;
    r->under = pcmbuf_underruns() - u0;
    r->rescue = pcmbuf_rescues() - r0;
    r->playing = (audio_status() & AUDIO_STATUS_PLAY) && pcm_is_playing();
    r->hold = button_hold();
}

/* per second, in tenths */
static unsigned long per_s10(uint32_t n, uint32_t ms)
{
    return ms ? (unsigned long)((uint64_t)n * 10000 / ms) : 0;
}

static void k1_print(const char *arm, const struct k1_arm *r)
{
    unsigned long c = per_s10(r->cop, r->ms), p = per_s10(r->cpu, r->ms);

    say("k1 arm=%s cop=%lu.%lu/s cpu=%lu.%lu/s ticks_sent=%lu held=%lu "
        "underruns=%lu rescues=%lu playing=%d hold=%d s=%lu", arm,
        c / 10, c % 10, p / 10, p % 10, (unsigned long)r->sent,
        (unsigned long)r->held, r->under, r->rescue, (int)r->playing,
        (int)r->hold, (unsigned long)(r->ms / 1000));
}

int devbench_core_k1(void)
{
    struct play_spot spot;
    struct k1_arm arm[2];
    bool park_was = corelayout_park(), ok;
    int a;

    if (!cop_jobs_cross_core())
    {
        say("k1: Not available: no second core running");
        return BENCH_SKIP;
    }
    play_note(&spot);
    if (!spot.playing)
    {
        spot.playing = playlist_amount() > 0;
        spot.elapsed = spot.offset = 0;
        playlist_get_resume_info(&spot.index);
        if (!spot.playing || !play_from(&spot))
        {
            say("k1: Not run: no music to play (play an album before the "
                "session)");
            return BENCH_SKIP;
        }
    }
    say("k1: arms pulse then park, %d s settle + %d s each; decode on the "
        "%s", K1_SETTLE_S, K1_WIN_S,
        codec_cop_place() == CPL_MODE_COP ? "COP" : "CPU (or by state)");
    memset(arm, 0, sizeof(arm));
    for (a = 0; a < 2; a++)
    {
        corelayout_set_park(a == 1);
        backlight_off();            /* dark: the panel sleeps by itself */
        hands_off_wait(K1_SETTLE_S * HZ);
        k1_measure(&arm[a]);
    }
    corelayout_set_park(park_was);  /* the default: the pulse */
    backlight_on();
    /* the lines only now: a log write in a window would wake storage */
    k1_print("pulse", &arm[0]);
    k1_print("park", &arm[1]);
    /* K-1's question is A/B: does parking the COP cost the audio
       anything the pulse arm doesn't?  So the parked arm's rescues may not
       exceed the pulse arm's, and neither arm may underrun.  Sitting 4gh
       read 44 rescues in each arm (decode on the CPU, the COP idle in
       both): the old rule wanted 0 in the baseline too, which judges the
       refill pacing (pcmbuf_full_wait_ticks), not the park, and failed a
       clean park.  Baseline rescues are logged as their own finding */
    {
        bool few = arm[1].ms > 0 && per_s10(arm[1].sent, arm[1].ms) <= 20;
        bool fewer = arm[1].cop < arm[0].cop;
        bool clean = arm[0].under == 0 && arm[1].under == 0 &&
                     arm[1].rescue <= arm[0].rescue && arm[1].playing;
        unsigned long saved = per_s10(arm[0].cop, arm[0].ms) -
            MIN(per_s10(arm[0].cop, arm[0].ms),
                per_s10(arm[1].cop, arm[1].ms));

        ok = few && fewer && clean;
        say("k1: %s: the parked COP (saved %lu.%lu wakes/s; pass: <= 2.0 "
            "tick pulses/s parked, fewer COP wakes, no underrun, no more "
            "rescues than the pulse arm%s)%s%s%s",
            ok ? "PASS" : "FAIL", saved / 10, saved % 10,
            arm[0].hold && arm[1].hold ? "" : "; Hold was off",
            few ? "" : " [pulses]", fewer ? "" : " [COP wakes]",
            clean ? "" : " [audio]");
        if (arm[0].rescue || arm[1].rescue)
            say("k1 note: rescues %lu/%lu (pulse/park): the level fell "
                "under the boost watermark in dark play; a refill-pacing "
                "finding, not the park's", arm[0].rescue, arm[1].rescue);
    }
    return ok ? BENCH_PASS : BENCH_FAIL;
}

/* ---- pcop: pcm-pp.c's driver with the FIQ on the COP ------------------- */

#define PC_BUF      16384           /* the tone, four pieces */
#define PC_PIECE    4096            /* one get_more: one buffer */
#define PC_SECS     60
#define PC_FRAMES   (PC_BUF / 4)
#define PC_LEVEL    3277            /* about -20 dBFS */

/* What the COP's FIQ touches: uncached, one writer per word (the FIQ's
   core writes more/late/last_us; the CPU writes the rest before start) */
static struct
{
    volatile uint32_t base, end, next;  /* uncached addresses */
    volatile uint32_t more, late, last_us, piece_us, max_gap_us;
    volatile uint32_t status_err;
} pc SHAREDBSS_ATTR;

/* pcm.c's get_more, on the COP's FIQ: the next piece of the loop */
static void pc_more(const void **start, size_t *size)
{
    uint32_t now = USEC_TIMER, a = pc.next, gap;

    if (pc.more)
    {
        gap = now - pc.last_us;
        if (gap > pc.max_gap_us)
            pc.max_gap_us = gap;
        /* a piece's worth of time and a half: the FIFO ran dry */
        if (gap > pc.piece_us + pc.piece_us / 2)
            pc.late++;
    }
    pc.last_us = now;
    pc.more++;
    *start = (const void *)(uintptr_t)a;
    *size = PC_PIECE;
    a += PC_PIECE;
    pc.next = a >= pc.end ? pc.base : a;
}

static enum pcm_dma_status pc_status(enum pcm_dma_status status)
{
    if (status < PCM_DMAST_OK)
        pc.status_err++;
    return status;
}

/* The COP drops its own cache: no stale line of the CPU's PCM state */
static void pc_acquire(void *arg)
{
    (void)arg;
    xcore_acquire();
}

/* The probe form, if the CPU's write to the COP's enable does not take:
   the COP enables its own source (P-1's form) */
static void pc_cop_enable(void *arg)
{
    (void)arg;
    COP_INT_EN = DMA_MASK;
}

static void pc_cop_disable(void *arg)
{
    (void)arg;
    COP_INT_DIS = DMA_MASK;
}

static unsigned pc_fill(int16_t *s, unsigned rate)
{
    unsigned k = (1000u * PC_FRAMES + rate / 2) / rate, n;

    for (n = 0; n < PC_FRAMES; n++)
    {
        unsigned long ph = (unsigned long)((k * n) % PC_FRAMES) << 20;
        long c, v = fp_sincos(ph, &c);          /* Q31 */
        int16_t x = (int16_t)(((v >> 16) * PC_LEVEL) >> 15);

        s[2 * n] = x;
        s[2 * n + 1] = x;
    }
    return k;
}

int devbench_core_pcop(void)
{
    struct play_spot spot;
    struct pcm_pp_stats a, b;
    unsigned rate, k, hz;
    uint32_t t0, us, expect, in_lock = 0, locks = 0, fiq_cpu0;
    bool dac_was, ok, cop_self = false;
    int h, res = BENCH_FAIL;
    void *tone;
    long end;
    const char *why = NULL;

    if (!cop_jobs_cross_core())
    {
        say("pcop: Not available: no second core running");
        return BENCH_SKIP;
    }
    if (pcm_get_current_sink() != &builtin_pcm_sink)
    {
        say("pcop: Not run: audio goes to another output (USB or dock)");
        return BENCH_SKIP;
    }
    if (!cw_open())
    {
        say("pcop: Not available: no worker on the COP");
        return BENCH_SKIP;
    }
    h = core_alloc(PC_BUF + 32);
    if (h <= 0)
    {
        cw_close();
        say("pcop: Not run: no memory for the tone");
        return BENCH_SKIP;
    }
    core_pin(h);
    play_note(&spot);
    if (!play_stop())
    {
        say("pcop: FAIL: the PCM did not stop; nothing was moved");
        goto out_mem;
    }
    rate = pcm_get_frequency();
    tone = (void *)CACHEALIGN_UP((uintptr_t)core_get_data(h));
    k = pc_fill((int16_t *)tone, rate);
    hz = k * rate / PC_FRAMES;
    commit_dcache();                /* the CPU wrote it; DMA0 reads SDRAM */

    memset((void *)&pc, 0, sizeof(pc));
    pc.base = UNCACHED_ADDR((uintptr_t)tone);
    pc.end = pc.base + PC_BUF;
    pc.next = pc.base + PC_PIECE;   /* the first piece goes with the start */
    pc.piece_us = (uint32_t)((uint64_t)PC_PIECE * 1000000 / (rate * 4));

    if (cw_run(pc_acquire, NULL) != 0)
    {
        why = "the COP did not take the job";
        goto out_play;
    }
    if (pcm_pp_set_core(COP) != 0)
    {
        why = "the PCM would not move (still playing)";
        goto out_play;
    }
    dac_was = dac_standby_allowed();
    dac_standby_allow(false);
    pcm_pp_get_stats(&a);
    fiq_cpu0 = a.fiq[CPU];

    /* the start, and its lock, from the CPU (step 2's callers): the
       unlock enables the COP's source by a CPU write [hypothesis] */
    pcm_play_data(pc_more, pc_status, (const void *)pc.base, PC_PIECE);
    sleep(HZ / 5);
    pcm_pp_get_stats(&b);
    if (b.fiq[COP] == a.fiq[COP])
    {
        /* no FIQ on the COP: try P-1's form, the COP's own enable */
        cop_self = true;
        if (cw_run(pc_cop_enable, NULL) == 0)
        {
            sleep(HZ / 5);
            pcm_pp_get_stats(&b);
        }
    }
    t0 = USEC_TIMER;
    end = current_tick + PC_SECS * HZ;
    while (TIME_BEFORE(current_tick, end) && pcm_is_playing())
    {
        uint32_t n0, n1;

        /* the CPU's lock: no FIQ on the COP may run inside it */
        pcm_play_lock();
        pcm_pp_get_stats(&b);
        n0 = b.fiq[COP];
        udelay(200);
        pcm_pp_get_stats(&b);
        n1 = b.fiq[COP];
        pcm_play_unlock();
        locks++;
        in_lock += n1 - n0;
        sleep(1);
    }
    us = USEC_TIMER - t0;
    /* if the CPU's writes did not reach the COP's mask, its lock did not
       hold either: the COP masks its own source before the stop */
    if (cop_self)
        cw_run(pc_cop_disable, NULL);
    pcm_play_stop();                /* the CPU's lock again, then stop */
    {
        int n = 100000;

        while (!IIS_TX_IS_EMPTY && --n > 0)
            ;
    }
    pcm_pp_get_stats(&b);
    if (pcm_pp_set_core(CPU) != 0)
        say("pcop: the PCM did not move back to the CPU (playing?)");
    dac_standby_allow(dac_was);

    /* get_more runs once per piece: a 60 s run at 44.1 kHz is 2,584 */
    expect = (uint32_t)((uint64_t)us * rate * 4 / PC_PIECE / 1000000);
    ok = pc.more * 20 >= expect * 19 && pc.more * 20 <= expect * 21 &&
         b.fiq[CPU] == fiq_cpu0 && in_lock == 0 && pc.late == 0 &&
         b.lock_stuck == a.lock_stuck && pc.status_err == 0;
    say("pcop fiq_cop=%lu fiq_cpu=%lu more=%lu expect=%lu late=%lu "
        "gap_max=%luus piece=%luus rate=%u tone=%uHz",
        (unsigned long)(b.fiq[COP] - a.fiq[COP]),
        (unsigned long)(b.fiq[CPU] - fiq_cpu0), (unsigned long)pc.more,
        (unsigned long)expect, (unsigned long)pc.late,
        (unsigned long)pc.max_gap_us, (unsigned long)pc.piece_us, rate, hz);
    say("pcop locks=%lu in_lock=%lu waits=%lu spin_max=%lu stuck=%lu "
        "held_off=%lu xcore=%lu spurious=%lu/%lu enable=%s",
        (unsigned long)locks, (unsigned long)in_lock,
        (unsigned long)(b.lock_waits - a.lock_waits),
        (unsigned long)b.lock_spin_max,
        (unsigned long)(b.lock_stuck - a.lock_stuck),
        (unsigned long)(b.held_off - a.held_off),
        (unsigned long)(b.lock_xcore - a.lock_xcore),
        (unsigned long)b.spurious[CPU], (unsigned long)b.spurious[COP],
        cop_self ? "the COP's own (the CPU's write did not take)"
                 : "the CPU's write");
    say("pcop: %s: pcm-pp.c's driver with the FIQ on the COP (pass: "
        "more %lu..%lu, nothing on the CPU or inside a lock, no late "
        "refill)", ok ? "PASS" : "FAIL", (unsigned long)(expect * 19 / 20),
        (unsigned long)(expect * 21 / 20));
    res = ok ? BENCH_PASS : BENCH_FAIL;
out_play:
    if (why)
    {
        pcm_pp_set_core(CPU);
        say("pcop: FAIL: %s; the route is the CPU's", why);
    }
    if (spot.playing)
        play_from(&spot);
out_mem:
    core_unpin(h);
    core_free(h);
    cw_close();
    return res;
}

#elif defined(HAVE_BCMFW_VIDEO) /* not this build: stubs; the step table (devbench.c, the device or the bench sim) exists only with BCMFW_VIDEO */

#include "devbcm.h"
#include "devbench.h"
#include "devbench_core.h"
#include "devbench_sim.h"           /* DEVBENCH_SIM: the bench simulator */
#ifdef DEVBENCH_SIM
#include "devbench_sim_calls.h"     /* devbcm_log, the sim's */
#endif

#ifdef SIMULATOR
#define CORE_NOT_RUN(id) \
    devbcm_log(id ": Not run (sim): the simulator has one core; the " \
               "route is [hypothesis] until the device runs it")
#else
#define CORE_NOT_RUN(id) devbcm_log(id ": Not available in this build")
#endif

int devbench_core_k1(void)
{
    CORE_NOT_RUN("k1");
    return BENCH_SKIP;
}

int devbench_core_pcop(void)
{
    CORE_NOT_RUN("pcop");
    return BENCH_SKIP;
}
#endif
