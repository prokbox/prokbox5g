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

/* The core layout probe's rows (devbench_cop.h): the device side.  Called
 * from devbench.c's step table on the UI thread (the CPU); each returns
 * an enum bench_res.  Evidence: what each touches is [rockbox-derived]
 * (pp5020.h, pcm-pp.c, timer-pp.c) or [apple-RE] (Apple's COP enables
 * source 26 at start-up, apple-dual-core.md section 1); every route to
 * the COP is [hypothesis] until these rows pass on silicon. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "system.h"                 /* HAVE_PP_IRQ_TALLY */

#if !defined(SIMULATOR) && defined(HAVE_COP_JOBS) && \
    defined(HAVE_PP_IRQ_TALLY) && defined(HAVE_DEVSTATS) && \
    defined(HAVE_BCMFW_VIDEO)   /* devbcm_log, devbench_ask (devbcm.h) */
#include "system.h"
#include "kernel.h"
#include "button.h"
#include "lcd.h"
#include "backlight.h"
#include "powermgmt.h"
#include "timer.h"
#include "audio.h"
#include "playlist.h"
#include "pcm.h"
#include "pcm-internal.h"
#include "pcm_sink.h"
#include "dac_ext.h"
#include "devstats.h"
#include "cop_job.h"
#include "core_alloc.h"
#include "fixedpoint.h"
#include "codec_cop.h"
#include "codec_place_core.h"
#include "ipodui/powerlog.h"
#include "devbcm.h"
#include "devbench.h"
#include "devbench_cop.h"

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

/* ---- a worker on the COP (cop_job.h), for the rows' COP-side steps ---- */

#define COP_STACK       1024
#define COP_JOB_MS      1000        /* a COP job's bound */

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
    if (!cop_worker_start(&cw, core_get_data(cw_h), COP_STACK, "coprobe",
                          PRIORITY_USER_INTERFACE) || !cop_jobs_cross_core())
    {
        cop_worker_stop(&cw);       /* a no-op unless it started */
        cw_hung = true;             /* its stack is kept until the restart */
        return false;
    }
    cw_up = true;
    return true;
}

static void cw_close(void)
{
    if (!cw_up || cw_hung)
        return;                     /* a hung job keeps its stack */
    cop_worker_stop(&cw);
    core_unpin(cw_h);
    core_free(cw_h);
    cw_h = 0;
    cw_up = false;
}

/* fn(arg) on the COP, waited for up to COP_JOB_MS.  0, -1 no worker,
   -2 never finished (the worker is then left alone) */
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

/* ---- the playback around a row --------------------------------------- */

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

/* A video's soundtrack in the audio playlist (#154: 4gh's playing rows
 * played the film's sound): the rows want an album's songs */
static bool track_is_video(void)
{
    struct mp3entry *id3 = audio_current_track();
    const char *dot = id3 ? strrchr(id3->path, '.') : NULL;

    return dot && (!strcasecmp(dot, ".m4v") || !strcasecmp(dot, ".mov") ||
                   !strcasecmp(dot, ".mp4"));
}

/* Starts the current playlist at the spot (a codec load: the decode
 * place in force then is the run's, codec_cop.h), waiting up to secs for
 * the PCM.  true: it plays */
static bool play_from_wait(const struct play_spot *p, int secs)
{
    long end;

    if (playlist_amount() <= 0)
        return false;
    playlist_start(p->index < 0 ? 0 : p->index, p->elapsed, p->offset);
    end = current_tick + secs * HZ;
    while (TIME_BEFORE(current_tick, end))
    {
        if ((audio_status() & AUDIO_STATUS_PLAY) && pcm_is_playing())
            return true;
        sleep(HZ / 10);
    }
    return false;
}

static bool play_from(const struct play_spot *p)
{
    return play_from_wait(p, 5);
}

/* Stops playback and waits for the PCM (the mixer's idle tail) to stop.
   true: the PCM is stopped */
static bool play_stop(void)
{
    long end = current_tick + 3 * HZ;

    audio_stop();
    while (pcm_is_playing() && TIME_BEFORE(current_tick, end))
        sleep(HZ / 20);
    return !pcm_is_playing();
}

/* ---- p2: Timer 2 on the COP ------------------------------------------ */

#define P2_HZ       100
#define P2_SECS     10
#define P2_EXPECT   (P2_HZ * P2_SECS)

static struct
{
    volatile uint32_t cop_n, cpu_n;
} p2 SHAREDBSS_ATTR;

static void p2_isr(void)
{
    if (CURRENT_CORE == COP)
        p2.cop_n++;
    else
        p2.cpu_n++;
}

int devbench_cop_t2(void)
{
    uint32_t t0, us, cop, cpu;
    int lo = P2_EXPECT - P2_EXPECT / 50, hi = P2_EXPECT + P2_EXPECT / 50;
    bool ok;

    if (!cop_jobs_cross_core())
    {
        say("p2: Not available: no second core running");
        return BENCH_SKIP;
    }
    p2.cop_n = p2.cpu_n = 0;
    /* [rockbox-derived] timer_register's core argument unmasks Timer 2
       in COP_INT_EN (timer-pp.c timer_start); irq_handler's COP branch
       serves it (system-pp502x.c).  On the COP: [hypothesis] */
    backlight_on();                 /* no fade: the fade owns Timer 2 */
    if (!timer_register(1, NULL, TIMER_FREQ / P2_HZ, p2_isr, COP))
    {
        say("p2: Not run: Timer 2 is taken");
        return BENCH_SKIP;
    }
    t0 = USEC_TIMER;
    sleep(P2_SECS * HZ);
    timer_unregister();             /* stops it, both cores' masks off */
    us = USEC_TIMER - t0;
    cop = p2.cop_n;
    cpu = p2.cpu_n;
    ok = (int)cop >= lo && (int)cop <= hi && cpu == 0;
    say("p2 cop_t2=%lu cpu_t2=%lu expect=%d us=%lu", (unsigned long)cop,
        (unsigned long)cpu, P2_EXPECT, (unsigned long)us);
    say("p2: %s: Timer 2 on the COP (pass %d..%d, 0 on the CPU)",
        ok ? "PASS" : "FAIL", lo, hi);
    return ok ? BENCH_PASS : BENCH_FAIL;
}

/* ---- p1: the PCM DMA's FIQ on the COP --------------------------------- */

#define P1_BUF      65536           /* the tone, looped by the FIQ */
#define P1_PIECE    8192            /* one DMA0 run: one FIQ */
#define P1_SECS     10
#define P1_FRAMES   (P1_BUF / 4)    /* 16-bit stereo */
#define P1_LEVEL    3277            /* about -20 dBFS */
#define P1_DOG_S    (P1_SECS + 5)   /* the tick's restore, if the row hangs */
/* pcm-pp.c's DMA_PLAY_CONFIG [rockbox-derived] */
#define P1_CMD      ((DMA_REQ_IIS << DMA_CMD_REQ_ID_POS) | \
                     DMA_CMD_RAM_TO_PER | DMA_CMD_SINGLE | \
                     DMA_CMD_WAIT_REQ | DMA_CMD_INTR)
#define LDR_PC_PC_M4 0xe51ff004     /* ldr pc, [pc, #-4] */

static struct
{
    volatile uint32_t cop_n, cpu_n;
    volatile uint32_t addr, base, end;
    volatile uint32_t run;          /* the FIQ re-arms DMA0 */
    volatile uint32_t routed;       /* the route is the lab's */
    volatile uint32_t cop_cpsr, cop_fiq_sp, dog;
    /* step 3, on the COP: the CPSR before and after it clears its own F
       bit, and the FIQs taken by the time the clear returns */
    volatile uint32_t un_before, un_after, n_at_unmask;
    volatile uint32_t en_seen, pri_seen;    /* step 2's set-up, read back */
} p1 SHAREDBSS_ATTR;

/* Step 2, read by the CPU with the COP's F bit still set and one DMA0
   piece played: where source 26 shows */
static struct
{
    uint32_t raw, cop_irq, cop_fiq, cpu_irq, cpu_fiq, dma_cmd, cop_n;
} p1_s2;

/* What the lab found, put back on every exit */
static void **fiq_slot;
static void *fiq_was;
static uint32_t cpu_en_was, cop_en_was, cop_pri_was;
static long p1_deadline;
#ifndef HAVE_CORE_LAYOUT_A
/* the COP's own FIQ stack for the lab: crt0-pp.S reserves none (its
   cop_fiq_stack is the CPU's fiq_stack top, the space under "#if 0").
   With core layout (A) step 0b the COP has its own 1 KB FIQ stack from
   start-up, and the lab runs on it (p1_cop_start) */
static uint32_t p1_fiq_stack[64] __attribute__((aligned(32)));
#endif

/* Both cores may land here; only the COP is routed.  No locals spill
   (pcm-pp.c's note on FIQ handlers) */
static void __attribute__((interrupt("FIQ"), noinline)) p1_fiq(void)
{
    uint32_t a;

    (void)DMA0_STATUS;              /* acknowledge */
    if (CURRENT_CORE == COP)
        p1.cop_n++;
    else
        p1.cpu_n++;
    if (!p1.run)
        return;
    a = p1.addr + P1_PIECE;
    if (a >= p1.end)
        a = p1.base;
    p1.addr = a;
    DMA0_RAM_ADDR = a;
    DMA0_CMD = P1_CMD | (P1_PIECE - 4) | DMA_CMD_START;
}

/* The way back, from the thread (IRQs off) or the tick's dog: the COP's
   source off first, DMA0 stopped, then the priority, the vector's target
   and the CPU's enable as found.  Idempotent */
static void p1_unroute(bool wait)
{
    int n = 20000;

    if (!p1.routed)
        return;
    p1.run = 0;
    COP_INT_DIS = DMA_MASK;
    DMA0_CMD = DMA0_CMD & ~(DMA_CMD_START | DMA_CMD_INTR);
    while (wait && (DMA0_STATUS & (DMA_STATUS_BUSY | DMA_STATUS_INTR)) &&
           --n > 0)
        ;
    (void)DMA0_STATUS;
    /* a FIQ the COP took just before its mask went has read the vector's
       target already; let it finish before the target changes */
    udelay(50);
    if (!cop_pri_was)
        COP_INT_PRIORITY &= ~DMA_MASK;
    if (cop_en_was)
        COP_INT_EN = DMA_MASK;
    *fiq_slot = fiq_was;
    if (cpu_en_was)
        CPU_INT_EN = DMA_MASK;
    p1.routed = 0;
}

static void p1_dog(void)
{
    if (p1.routed && TIME_AFTER(current_tick, p1_deadline))
    {
        p1.dog = 1;
        p1_unroute(false);
    }
}

#ifndef HAVE_CORE_LAYOUT_A
/* The COP's FIQ stack pointer: set (the lab's) or put back.  The asm
   names low registers only: FIQ mode banks r8-r14 */
static uint32_t cop_fiq_sp_swap(uint32_t sp)
{
    register uint32_t r0 asm("r0") = sp;
    register uint32_t r1 asm("r1");

    asm volatile (
        "mrs    r2, cpsr        \n"
        "msr    cpsr_c, #0xd1   \n"     /* FIQ mode, IRQ and FIQ off */
        "mov    r1, sp          \n"
        "mov    sp, r0          \n"
        "msr    cpsr_c, r2      \n"
        : "=&r"(r1) : "r"(r0) : "r2", "memory");
    return r1;
}
#else
/* The COP's FIQ stack pointer, read only (logged): step 0b's own stack */
static uint32_t cop_fiq_sp_read(void)
{
    register uint32_t r1 asm("r1");

    asm volatile (
        "mrs    r2, cpsr        \n"
        "msr    cpsr_c, #0xd1   \n"     /* FIQ mode, IRQ and FIQ off */
        "mov    r1, sp          \n"
        "msr    cpsr_c, r2      \n"
        : "=&r"(r1) : : "r2", "memory");
    return r1;
}
#endif

/* Sitting 4gh read no FIQ with the COP's F bit set (cop_cpsr 0x5f): the
   F bit is the COP's own and only code on the COP can clear it.  Apple
   never takes source 26 on the COP (apple-cop-audio.md: the CPU owns I2S,
   DMA and the FIQ), so this is our extension and the probe an
   experiment, run as steps that each log their own line:
     1  the CPU routes source 26 to the COP (priority bit, vector target)
     2  the COP, F masked, enables 26 and starts ONE 8 KB piece; the CPU
        reads where 26 shows (raw, the COP's and the CPU's IRQ and FIQ
        status)
     3  the COP clears its own F bit (0x5f -> 0x1f); the pending FIQ is
        taken at once (1), and its handler starts the loop
     4  10 s with the COP idle: ~215 FIQs, all on the COP (a sleeping COP
        wakes for its FIQ)
   Step 0 is the F bit as the COP's start-up left it (layout (A) clears
   it there, system-pp502x.c). */

/* Step 2, COP: F masked, its stack, its source on, one piece started */
static void p1_cop_start(void *arg)
{
    uint32_t cpsr;

    (void)arg;
    asm volatile ("mrs %0, cpsr" : "=r"(cpsr));
    p1.cop_cpsr = cpsr;             /* step 0: as start-up left it */
    disable_fiq();                  /* step 2 runs masked */
#ifdef HAVE_CORE_LAYOUT_A
    /* step 0b: the COP's FIQ already has its own stack (crt0-pp.S) */
    p1.cop_fiq_sp = cop_fiq_sp_read();
#else
    p1.cop_fiq_sp = cop_fiq_sp_swap((uint32_t)&p1_fiq_stack[64]);
#endif
    /* A request left from the last playback is cleared first (reading
       the status acknowledges it), so what step 2 sees is this piece's */
    (void)DMA0_STATUS;
    COP_INT_EN = DMA_MASK;
    DMA0_PER_ADDR = (unsigned long)&IISFIFO_WR;
    DMA0_FLAGS = DMA_FLAGS_UNK26;
    DMA0_INCR = DMA_INCR_RANGE_FIXED | DMA_INCR_WIDTH_32BIT;
    p1.addr = p1.base;
    p1.run = 1;                     /* the FIQ, once taken, re-arms */
    DMA0_RAM_ADDR = p1.base;
    DMA0_CMD = P1_CMD | (P1_PIECE - 4) | DMA_CMD_START;
    p1.en_seen = COP_INT_EN_STAT & DMA_MASK;
    p1.pri_seen = COP_INT_PRIORITY & DMA_MASK;
}

/* Step 3, COP: its own F bit cleared; a pending FIQ is taken here */
static void p1_cop_unmask(void *arg)
{
    uint32_t cpsr;

    (void)arg;
    asm volatile ("mrs %0, cpsr" : "=r"(cpsr));
    p1.un_before = cpsr;
    enable_fiq();
    asm volatile ("nop\n\tnop\n\tnop\n\tnop");
    asm volatile ("mrs %0, cpsr" : "=r"(cpsr));
    p1.un_after = cpsr;
    p1.n_at_unmask = p1.cop_n;
}

static void p1_cop_end(void *arg)
{
    (void)arg;
    if (p1.cop_cpsr & FIQ_DISABLED)
        disable_fiq();              /* as start-up left it */
#ifndef HAVE_CORE_LAYOUT_A
    cop_fiq_sp_swap(p1.cop_fiq_sp);
#endif
}

/* A whole number of cycles in the buffer, so the loop is seamless:
   k cycles in P1_FRAMES frames, about 1 kHz */
static unsigned p1_fill(int16_t *s, unsigned rate)
{
    unsigned k = (1000u * P1_FRAMES + rate / 2) / rate, n;

    for (n = 0; n < P1_FRAMES; n++)
    {
        unsigned long ph = (unsigned long)((k * n) % P1_FRAMES) << 18;
        long c, v = fp_sincos(ph, &c);          /* Q31 */
        int16_t x = (int16_t)(((v >> 16) * P1_LEVEL) >> 15);

        s[2 * n] = x;
        s[2 * n + 1] = x;
    }
    return k;
}

int devbench_cop_fiq(void)
{
    struct play_spot spot;
    unsigned rate, k, hz;
    uint32_t t0 = 0, us = 0, cop, cpu, expect, n4 = 0;
    bool s0, s2, s3, s4;
    bool dac_was, ok, stack_back = false, disturbed;
    int h, rc;
    const char *why = NULL;
    int res = BENCH_FAIL;

    if (!cop_jobs_cross_core())
    {
        say("p1: Not available: no second core running");
        return BENCH_SKIP;
    }
    /* the FIQ vector's one patchable target (pcm-pp.c fiq_handler:
       "ldr pc, [pc, #-4]" then the word) [rockbox-derived]; refused if
       the code is not that shape */
    {
        extern void fiq_handler(void);
        uint32_t *code = (uint32_t *)(uintptr_t)fiq_handler;

        if (code[0] != LDR_PC_PC_M4)
        {
            say("p1: Not run: the FIQ vector is not the expected shape "
                "(0x%08lx)", (unsigned long)code[0]);
            return BENCH_SKIP;
        }
        fiq_slot = (void **)&code[1];
    }
    if (pcm_get_current_sink() != &builtin_pcm_sink)
    {
        say("p1: Not run: audio goes to another output (USB or dock)");
        return BENCH_SKIP;
    }
    if (!cw_open())
    {
        say("p1: Not available: no worker on the COP");
        return BENCH_SKIP;
    }
    h = core_alloc(P1_BUF + 32);
    if (h <= 0)
    {
        cw_close();
        say("p1: Not run: no memory for the tone");
        return BENCH_SKIP;
    }
    core_pin(h);
    play_note(&spot);
    if (!play_stop())
    {
        say("p1: FAIL: the PCM did not stop; nothing was rerouted");
        goto out_mem;
    }
    rate = pcm_get_frequency();
    k = p1_fill((int16_t *)CACHEALIGN_UP((uintptr_t)core_get_data(h)), rate);
    hz = k * rate / P1_FRAMES;
    commit_dcache();                /* the CPU wrote it; DMA0 reads SDRAM */
    dac_was = dac_standby_allowed();
    dac_standby_allow(false);       /* the DAC awake (wm8758.c) */

    memset((void *)&p1, 0, sizeof(p1));
    memset(&p1_s2, 0, sizeof(p1_s2));
    p1.base = UNCACHED_ADDR(CACHEALIGN_UP((uintptr_t)core_get_data(h)));
    p1.end = p1.base + P1_BUF;

    /* the route, in Apple's order where Apple shows one [apple-RE]: the
       COP's priority bit then its enable (the COP's own start-up), the
       CPU's source off first (it never serves this run) */
    {
        int ost = disable_irq_save();

        fiq_was = *fiq_slot;
        cpu_en_was = CPU_INT_EN_STAT & DMA_MASK;
        cop_en_was = COP_INT_EN_STAT & DMA_MASK;
        cop_pri_was = COP_INT_PRIORITY & DMA_MASK;
        CPU_INT_DIS = DMA_MASK;
        COP_INT_PRIORITY |= DMA_MASK;
        *fiq_slot = (void *)p1_fiq;
        p1_deadline = current_tick + P1_DOG_S * HZ;
        p1.routed = 1;
        restore_irq(ost);
    }
    tick_add_task(p1_dog);
    rc = cw_run(p1_cop_start, NULL);
    if (rc)
        why = rc == -2 ? "step 2: the COP did not finish its set-up (stuck)"
                       : "step 2: the COP did not take the job";
    else
    {
        /* one piece is 46 ms at 44.1 kHz; 100 ms is past its end.  Only
           side-effect-free reads: a DMA0 status read would acknowledge */
        sleep(HZ / 10);
        p1_s2.raw = INT_STAT & DMA_MASK;
        p1_s2.cop_irq = COP_INT_STAT & DMA_MASK;
        p1_s2.cop_fiq = COP_FIQ_STAT & DMA_MASK;
        p1_s2.cpu_irq = CPU_INT_STAT & DMA_MASK;
        p1_s2.cpu_fiq = CPU_FIQ_STAT & DMA_MASK;
        p1_s2.dma_cmd = DMA0_CMD;
        p1_s2.cop_n = p1.cop_n;
        rc = cw_run(p1_cop_unmask, NULL);
        if (rc)
            why = rc == -2 ? "step 3: the COP did not return from its "
                             "unmask (stuck)"
                           : "step 3: the COP did not take the job";
        else
        {
            t0 = USEC_TIMER;
            n4 = p1.cop_n;
            sleep(P1_SECS * HZ);
            us = USEC_TIMER - t0;
            n4 = p1.cop_n - n4;
        }
    }
    {
        int ost = disable_irq_save();

        p1_unroute(true);
        restore_irq(ost);
    }
    tick_remove_task(p1_dog);
    /* the FIFO drains (pcm-pp.c's stop waits for it too) */
    {
        int n = 100000;

        while (!IIS_TX_IS_EMPTY && --n > 0)
            ;
    }
    if (rc != -2 && !cw_hung)
        stack_back = cw_run(p1_cop_end, NULL) == 0;
    dac_standby_allow(dac_was);
    disturbed = pcm_is_playing();   /* something started the PCM meanwhile */

    cop = p1.cop_n;
    cpu = p1.cpu_n;
    /* step 4's pieces: a 10 s run at 44.1 kHz is 215 */
    expect = (uint32_t)((uint64_t)us * rate * 4 / P1_PIECE / 1000000);
    s0 = !(p1.cop_cpsr & FIQ_DISABLED);
    s2 = p1_s2.raw && p1_s2.cop_fiq && !p1_s2.cpu_fiq && !p1_s2.cpu_irq &&
         p1_s2.cop_n == 0;
    s3 = (p1.un_before & FIQ_DISABLED) && !(p1.un_after & FIQ_DISABLED) &&
         p1.n_at_unmask >= 1;
    s4 = us > 0 && cpu == 0 && n4 * 20 >= expect * 19 &&
         n4 * 20 <= expect * 21;
    say("p1 step0 start-up cpsr=0x%02lx fbit=%d: %s",
        (unsigned long)(p1.cop_cpsr & 0xff),
        (int)((p1.cop_cpsr >> 6) & 1),
        s0 ? "ok (the COP's start-up opened its FIQ)"
           : "masked (start-up did not clear F)");
    say("p1 step1 route: pri_bit=%d en_bit=%d (read on the COP), the "
        "vector the lab's: %s", p1.pri_seen != 0, p1.en_seen != 0,
        p1.pri_seen && p1.en_seen ? "ok" : "FAIL");
    say("p1 step2 one piece, F masked: raw=%d cop_fiq=%d cop_irq=%d "
        "cpu_fiq=%d cpu_irq=%d fiqs=%lu dma_cmd=%08lx: %s",
        p1_s2.raw != 0, p1_s2.cop_fiq != 0, p1_s2.cop_irq != 0,
        p1_s2.cpu_fiq != 0, p1_s2.cpu_irq != 0,
        (unsigned long)p1_s2.cop_n, (unsigned long)p1_s2.dma_cmd,
        s2 ? "ok (routed to the COP's FIQ)"
           : !p1_s2.raw ? "FAIL (no source 26: DMA0 did not complete)"
           : p1_s2.cpu_fiq || p1_s2.cpu_irq ? "FAIL (it reached the CPU)"
           : !p1_s2.cop_fiq ? "FAIL (not a FIQ on the COP: the select)"
           : "FAIL (taken while masked)");
    say("p1 step3 unmask on the COP: cpsr 0x%02lx -> 0x%02lx fiqs=%lu: %s",
        (unsigned long)(p1.un_before & 0xff),
        (unsigned long)(p1.un_after & 0xff),
        (unsigned long)p1.n_at_unmask,
        s3 ? "ok (the pending FIQ taken at once)"
           : (p1.un_after & FIQ_DISABLED) ? "FAIL (F still set)"
           : "FAIL (no FIQ taken)");
    say("p1 step4 10 s, COP idle: cop_fiq=%lu cpu_fiq=%lu expect=%lu "
        "(%lu..%lu) rate=%u tone=%uHz us=%lu: %s",
        (unsigned long)n4, (unsigned long)cpu, (unsigned long)expect,
        (unsigned long)(expect * 19 / 20), (unsigned long)(expect * 21 / 20),
        rate, hz, (unsigned long)us, s4 ? "ok" : "FAIL");
    say("p1 stack=%s dog=%d total_cop=%lu%s",
        stack_back ? "back" : "not back (COP)", (int)p1.dog,
        (unsigned long)cop, disturbed ? " pcm=started meanwhile" : "");
    /* step 2's IRQ bit on the COP is logged, not judged: whether the
       controller also shows a FIQ-select source in COP_INT_STAT is itself
       unknown */
    ok = !why && !p1.dog && !disturbed && s2 && s3 && s4;
#ifdef HAVE_CORE_LAYOUT_A
    ok = ok && s0;                  /* (A)'s start-up fix, 4gh */
#endif
    if (why)
        say("p1: FAIL: %s; the route is back on the CPU", why);
    else
        say("p1: %s: the audio DMA's FIQ on the COP (pass: steps 2, 3 and "
            "4 ok%s)", ok ? "PASS" : "FAIL",
#ifdef HAVE_CORE_LAYOUT_A
            ", and step 0"
#else
            ""
#endif
            );
    res = ok ? BENCH_PASS : BENCH_FAIL;
out_mem:
    core_unpin(h);
    core_free(h);
    cw_close();
    if (spot.playing && !play_from(&spot))
        say("p1: playback did not restart (it was playing before)");
    return res;
}

/* ---- duty: the CPU's duty, screen off, music on ----------------------- */

#define DUTY_ARMS       4           /* C F C F */
#define DUTY_SETTLE_S   30
#define DUTY_WIN_S      60
#define DUTY_WINS       3
#define DUTY_TOP        3

struct duty_snap
{
    uint32_t us, ticks;
    uint32_t idle_us[2], exits[2];
    uint32_t irq[PP_IRQ_N];
    uint32_t thr[DEVSTATS_SLOTS];
    uint32_t disk_wakes;
#ifdef HAVE_PLL_IDLE
    struct cpu_pll_stats ps;
#endif
};

struct duty_win
{
    uint8_t arm, w, cop_dec, screen, hold;
    uint8_t top[DUTY_TOP];
    uint16_t cpu_idle_pm, cop_idle_pm;
    uint16_t mv, mhz;
    uint32_t ms, cpu_exits, cop_exits, ticks, disk_wakes;
    uint32_t irq[PP_IRQ_N];
    uint32_t topn[DUTY_TOP], thr_all;
    uint32_t at24_ticks, ent, bfi, locks;
};

static void duty_snap(struct duty_snap *s)
{
    int i;

    s->us = USEC_TIMER;
    s->ticks = current_tick;
    for (i = 0; i < 2; i++)
    {
        s->idle_us[i] = devstats.idle_us[i];
        s->exits[i] = pp_idle_exits[i];
    }
    memcpy(s->irq, pp_irq_n, sizeof(s->irq));
    for (i = 0; i < DEVSTATS_SLOTS; i++)
        s->thr[i] = devstats.t[i].thr_wakes;
    s->disk_wakes = devstats.wake_head;
#ifdef HAVE_PLL_IDLE
    cpu_pll_get_stats(&s->ps);
#endif
}

static uint16_t pm(uint32_t part, uint32_t whole)
{
    return whole ? (uint16_t)MIN(1000, (uint64_t)part * 1000 / whole) : 0;
}

static void duty_diff(struct duty_win *w, const struct duty_snap *a,
                      const struct duty_snap *b)
{
    uint32_t wall = b->us - a->us, d[DEVSTATS_SLOTS];
    int i, j, n = 0;

    w->ms = wall / 1000;
    w->ticks = b->ticks - a->ticks;
    w->cpu_idle_pm = pm(b->idle_us[CPU] - a->idle_us[CPU], wall);
    w->cop_idle_pm = pm(b->idle_us[COP] - a->idle_us[COP], wall);
    w->cpu_exits = b->exits[CPU] - a->exits[CPU];
    w->cop_exits = b->exits[COP] - a->exits[COP];
    for (i = 0; i < PP_IRQ_N; i++)
        w->irq[i] = b->irq[i] - a->irq[i];
    w->disk_wakes = b->disk_wakes - a->disk_wakes;
    w->thr_all = 0;
    for (i = 0; i < DEVSTATS_SLOTS; i++)
    {
        d[i] = b->thr[i] - a->thr[i];
        w->thr_all += d[i];
    }
    for (j = 0; j < DUTY_TOP; j++)
    {
        int best = -1;

        for (i = 0; i < DEVSTATS_SLOTS; i++)
            if (d[i] && (best < 0 || d[i] > d[best]))
                best = i;
        w->top[j] = best < 0 ? 0xff : (uint8_t)best;
        w->topn[j] = best < 0 ? 0 : d[best];
        if (best >= 0)
        {
            d[best] = 0;
            n++;
        }
    }
    (void)n;
#ifdef HAVE_PLL_IDLE
    w->at24_ticks = b->ps.idle_ticks - a->ps.idle_ticks;
    w->ent = b->ps.idle_entries - a->ps.idle_entries;
    w->bfi = b->ps.boosts_from_idle - a->ps.boosts_from_idle;
    w->locks = b->ps.lock_count - a->ps.lock_count;
#endif
    w->mhz = (uint16_t)(cpu_frequency / 1000000);
    w->mv = (uint16_t)battery_voltage();
    w->cop_dec = codec_cop_active();
    w->screen = is_backlight_on(true) ? 2 : lcd_active() ? 1 : 0;
    w->hold = button_hold();
}

/* A wait that a button can't shorten (Hold is on); USB or power-off are
   put back for the shell */
static void duty_wait(long ticks)
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

static const char *const duty_screen[3] = { "dark", "dim", "lit" };

static void duty_print(const struct duty_win *w)
{
    char names[DUTY_TOP][16];
    unsigned s10 = w->ms ? w->ms / 100 : 1;     /* tenths of a second */
    int j;

    for (j = 0; j < DUTY_TOP; j++)
    {
        if (w->top[j] == 0xff)
            strcpy(names[j], "-");
        else
            devstats_slot_name(w->top[j], names[j], sizeof(names[j]));
    }
    say("duty %c%d w%d s=%lu cpu_idle=%u cop_idle=%u wakes=%lu.%lu/s "
        "cop_wakes=%lu.%lu/s tick=%lu.%lu/s dec=%s scr=%s hold=%d mv=%u",
        "CF"[w->arm & 1], w->arm / 2 + 1, w->w + 1,
        (unsigned long)(w->ms / 1000), w->cpu_idle_pm, w->cop_idle_pm,
        (unsigned long)(w->cpu_exits * 10 / s10),
        (unsigned long)(w->cpu_exits * 100 / s10 % 10),
        (unsigned long)(w->cop_exits * 10 / s10),
        (unsigned long)(w->cop_exits * 100 / s10 % 10),
        (unsigned long)(w->ticks * 10 / s10),
        (unsigned long)(w->ticks * 100 / s10 % 10),
        w->cop_dec ? "cop" : "cpu", duty_screen[w->screen], w->hold, w->mv);
    say("duty %c%d w%d irq tick=%lu t2=%lu usb=%lu ide=%lu wheel=%lu "
        "gpio=%lu other=%lu disk_wakes=%lu",
        "CF"[w->arm & 1], w->arm / 2 + 1, w->w + 1,
        (unsigned long)w->irq[PP_IRQ_TICK], (unsigned long)w->irq[PP_IRQ_T2],
        (unsigned long)w->irq[PP_IRQ_USB], (unsigned long)w->irq[PP_IRQ_IDE],
        (unsigned long)w->irq[PP_IRQ_WHEEL],
        (unsigned long)w->irq[PP_IRQ_GPIO],
        (unsigned long)w->irq[PP_IRQ_OTHER], (unsigned long)w->disk_wakes);
    say("duty %c%d w%d thr all=%lu %s=%lu %s=%lu %s=%lu clk at24=%u "
        "ent=%lu bfi=%lu lock=%lu mhz=%u",
        "CF"[w->arm & 1], w->arm / 2 + 1, w->w + 1,
        (unsigned long)w->thr_all, names[0], (unsigned long)w->topn[0],
        names[1], (unsigned long)w->topn[1], names[2],
        (unsigned long)w->topn[2], pm(w->at24_ticks, w->ms / 10),
        (unsigned long)w->ent, (unsigned long)w->bfi,
        (unsigned long)w->locks, w->mhz);
}

int devbench_cop_duty(void)
{
    struct play_spot spot;
    struct duty_win *win;
    struct duty_snap a, b;
    int h, arm, k, n = 0, place_was = codec_cop_place();
    bool log_was = powerlog_is_on(), cop_ok = codec_cop_available() &&
                   codec_cop_enabled();
    int log_secs = powerlog_interval();
    unsigned long sum_idle[2] = { 0, 0 }, sum_exits[2] = { 0, 0 };
    unsigned long sum_cop[2] = { 0, 0 }, sum_s[2] = { 0, 0 };
    char tag[16];

    play_note(&spot);
    if (!spot.playing)
    {
        /* the session stopped it at its start: the same spot again */
        spot.playing = playlist_amount() > 0;
        spot.elapsed = spot.offset = 0;
        playlist_get_resume_info(&spot.index);
    }
    if (!spot.playing)
    {
        say("duty: Not run: no music to play (play an album before the "
            "session)");
        return BENCH_SKIP;
    }
    if (track_is_video())
    {
        /* sitting 4gh: arm F's codec load on the COP never played a
           film's soundtrack; the reading wants songs either way */
        say("duty: Not run: a video's sound is playing (play an album: "
            "the Play an album step)");
        return BENCH_SKIP;
    }
    h = core_alloc(sizeof(*win) * DUTY_ARMS * DUTY_WINS);
    if (h <= 0)
    {
        say("duty: Not run: no memory for the windows");
        return BENCH_SKIP;
    }
    core_pin(h);
    win = core_get_data(h);
    powerlog_set_interval(5);
    if (!log_was)
        powerlog_set(true);
    say("duty: arms C F C F, %d s settle + %d x %d s; F %s", DUTY_SETTLE_S,
        DUTY_WINS, DUTY_WIN_S, cop_ok ? "on the COP" : "not available");
    for (arm = 0; arm < DUTY_ARMS; arm++)
    {
        bool f = arm & 1;
        struct play_spot here;

        if (f && !cop_ok)
            continue;
        /* a codec load at the arm's place (codec_cop.h: the place is a
           run's, decided at the load) */
        play_note(&here);
        if (!here.playing)
            here = spot;
        play_stop();
        codec_cop_set_place(f ? CPL_MODE_COP : CPL_MODE_CPU);
        /* the COP's first run waits on its worker's start and every
           read crosses the cores: 15 s there, 5 s on the CPU; a failure
           says what it saw */
        if (!play_from_wait(&here, f ? 15 : 5))
        {
            uint32_t cop_runs, cpu_runs;

            codec_cop_place_stats(&cop_runs, &cpu_runs);
            say("duty: arm %c%d: playback did not start (status %d pcm %d "
                "runs cop %lu cpu %lu place %d)", f ? 'F' : 'C', arm / 2 + 1,
                audio_status(), (int)pcm_is_playing(),
                (unsigned long)cop_runs, (unsigned long)cpu_runs,
                codec_cop_place());
            break;
        }
        backlight_off();            /* dark: the panel sleeps by itself */
        snprintf(tag, sizeof(tag), "duty %c%d", f ? 'F' : 'C', arm / 2 + 1);
        powerlog_mark(tag);
        duty_wait(DUTY_SETTLE_S * HZ);
        for (k = 0; k < DUTY_WINS; k++)
        {
            struct duty_win *w = &win[n];

            duty_snap(&a);
            duty_wait(DUTY_WIN_S * HZ);
            duty_snap(&b);
            memset(w, 0, sizeof(*w));
            w->arm = (uint8_t)arm;
            w->w = (uint8_t)k;
            duty_diff(w, &a, &b);
            sum_idle[f] += w->cpu_idle_pm;
            sum_cop[f] += w->cop_idle_pm;
            sum_exits[f] += w->cpu_exits;
            sum_s[f] += w->ms;
            n++;
        }
    }
    powerlog_mark("duty");
    backlight_on();
    /* back to the build's place, at a codec load */
    {
        struct play_spot here;

        play_note(&here);
        play_stop();
        codec_cop_set_place(place_was);
        if (here.playing)
            play_from(&here);
    }
    if (!log_was)
        powerlog_set(false);
    powerlog_set_interval(log_secs);
    /* the lines only now: a log write in a window would wake storage */
    for (k = 0; k < n; k++)
        duty_print(&win[k]);
    for (k = 0; k < 2; k++)
    {
        unsigned wins = 0;
        int j;

        for (j = 0; j < n; j++)
            wins += (win[j].arm & 1) == k;
        if (!wins)
            continue;
        say("duty arm %c: cpu_idle=%lu cop_idle=%lu wakes=%lu/s over %lu s",
            "CF"[k], sum_idle[k] / wins, sum_cop[k] / wins,
            sum_s[k] ? sum_exits[k] * 1000 / sum_s[k] : 0,
            sum_s[k] / 1000);
    }
    core_unpin(h);
    core_free(h);
    say("duty: %d windows logged (a reading, no verdict)", n);
    return n ? BENCH_DONE : BENCH_FAIL;
}

/* ---- heap: the heap's headers while the decode runs on each core ------
 * Sitting 4gh-b5: sd's first allocation set off a compaction that met a
 * corrupt header (buflib "handle unaligned"), most likely damaged during
 * duty's arm F.  The A/B: an album plays with its decode on the COP (arm
 * F), then on the CPU (arm C); every HEAP_EVERY an allocation of the next
 * size in heap_sizes (the oldest of HEAP_LIVE freed first, so holes and
 * compactions come, and the audio buffer is shrunk: playback restarts, as
 * in sd), each block filled with its own byte; then the walk
 * (heapchk_walk: every header's length, handle, back-pointer and CRC) and
 * every live block's bytes checked.  The verdict: the first bad block (or
 * damaged pattern) and its arm, else PASS on both arms. */
#ifdef HAVE_BUFLIB_HEAPCHECK
#include "ipodui/heapchk.h"
#include "playback.h"               /* audio_get_filebuflen */

#define HEAP_ARM_S      180
#define HEAP_EVERY      (2 * HZ)
#define HEAP_LIVE       3

static const uint32_t heap_sizes[] =
    { 1024, 7000, 33000, 130000, 3000, 65548, 513, 20000, 96, 48000 };

struct heap_arm
{
    bool ran;
    uint32_t cycles, allocs, fails, walks, shrinks, on_cop, max_us, sum_us;
    uint32_t checked_kb;
    size_t buf0, buf1;
};

struct heap_live
{
    int h;
    uint32_t size;
    uint8_t fill;
};

/* Every live block's bytes are its fill byte: -1, or the first damaged
   one's index in live[] (*off its offset, *got the byte found) */
static int heap_check(const struct heap_live *live, int n, uint32_t *off,
                      uint8_t *got, uint32_t *bytes)
{
    int i;
    uint32_t j;

    for (i = 0; i < n; i++)
    {
        const uint8_t *p = core_get_data(live[i].h);

        for (j = 0; j < live[i].size; j++)
            if (p[j] != live[i].fill)
            {
                *off = j;
                *got = p[j];
                return i;
            }
        *bytes += live[i].size;
    }
    return -1;
}

static void heap_say(const char *line)
{
    say("heap: %s", line);
}

int devbench_cop_heap(void)
{
    struct play_spot spot;
    struct heap_arm arm[2];
    struct heap_live live[HEAP_LIVE];
    struct buflib_walk w;
    int nlive = 0, a, k = 0, i, bad_arm = -1, place_was = codec_cop_place();
    bool cop_ok = codec_cop_available() && codec_cop_enabled();
    uint32_t bad_cycle = 0;

    play_note(&spot);
    if (!spot.playing)
    {
        spot.playing = playlist_amount() > 0;
        spot.elapsed = spot.offset = 0;
        playlist_get_resume_info(&spot.index);
    }
    if (!spot.playing)
    {
        say("heap: Not run: no music to play (play an album before the "
            "session)");
        return BENCH_SKIP;
    }
    if (track_is_video())
    {
        say("heap: Not run: a video's sound is playing (play an album)");
        return BENCH_SKIP;
    }
    memset(arm, 0, sizeof(arm));
    memset(&w, 0, sizeof(w));
    say("heap: arms F (COP decode) then C (CPU), %d s each; every %d s an "
        "allocation (%d sizes, %d live), the walk, the live bytes checked",
        HEAP_ARM_S,
        HEAP_EVERY / HZ, (int)ARRAYLEN(heap_sizes), HEAP_LIVE);
    for (a = 0; a < 2 && bad_arm < 0; a++)
    {
        bool f = a == 0;
        struct heap_arm *r = &arm[a];
        struct play_spot here;
        long end;

        if (f && !cop_ok)
        {
            say("heap: arm F: Not run: no decode on the COP in this build");
            continue;
        }
        play_note(&here);
        if (!here.playing)
            here = spot;
        play_stop();
        codec_cop_set_place(f ? CPL_MODE_COP : CPL_MODE_CPU);
        if (!play_from_wait(&here, f ? 15 : 5))
        {
            say("heap: arm %c: playback did not start (status %d pcm %d)",
                f ? 'F' : 'C', audio_status(), (int)pcm_is_playing());
            continue;
        }
        r->ran = true;
        r->buf0 = audio_get_filebuflen();
        end = current_tick + HEAP_ARM_S * HZ;
        while (TIME_BEFORE(current_tick, end))
        {
            uint32_t us = 0, off = 0, size = heap_sizes[k++ %
                                                     ARRAYLEN(heap_sizes)];
            size_t before = audio_get_filebuflen();
            uint8_t got = 0;
            int bad, dmg, h;

            r->cycles++;
            r->on_cop += codec_cop_active() ? 1 : 0;
            if (nlive == HEAP_LIVE)
            {
                core_free(live[0].h);
                memmove(&live[0], &live[1], sizeof(live[0]) * --nlive);
            }
            /* no yield from here to the check: the block may move at the
               next allocation, its handle is what's kept */
            h = core_alloc(size);
            if (h > 0)
            {
                live[nlive].h = h;
                live[nlive].size = size;
                live[nlive].fill = (uint8_t)(0x5A ^ (r->cycles * 37));
                memset(core_get_data(h), live[nlive].fill, size);
                nlive++;
                r->allocs++;
            }
            else
                r->fails++;
            if (audio_get_filebuflen() < before)
                r->shrinks++;
            bad = heapchk_walk(f ? "heap F" : "heap C", &w, &us);
            r->walks++;
            r->sum_us += us;
            if (us > r->max_us)
                r->max_us = us;
            dmg = heap_check(live, nlive, &off, &got, &r->checked_kb);
            if (bad || dmg >= 0)
            {
                bad_arm = a;
                bad_cycle = r->cycles;
                if (bad)
                    heapchk_describe(f ? "heap F" : "heap C", &w, heap_say);
                if (dmg >= 0)
                    say("heap: arm %c cycle %lu: block %d (handle %d, %lu "
                        "bytes) damaged at +%lu: 0x%02x, not 0x%02x",
                        f ? 'F' : 'C', (unsigned long)r->cycles, dmg,
                        live[dmg].h, (unsigned long)live[dmg].size,
                        (unsigned long)off, got, live[dmg].fill);
                break;
            }
            duty_wait(HEAP_EVERY);
        }
        r->buf1 = audio_get_filebuflen();
        say("heap arm %c: cycles=%lu allocs=%lu fails=%lu walks=%lu "
            "blocks=%d us_max=%lu us_avg=%lu", f ? 'F' : 'C',
            (unsigned long)r->cycles, (unsigned long)r->allocs,
            (unsigned long)r->fails, (unsigned long)r->walks, w.blocks,
            (unsigned long)r->max_us,
            (unsigned long)(r->walks ? r->sum_us / r->walks : 0));
        say("heap arm %c: shrinks=%lu on_cop=%lu of %lu checked_kb=%lu "
            "buf=%lu->%lu", f ? 'F' : 'C', (unsigned long)r->shrinks,
            (unsigned long)r->on_cop, (unsigned long)r->cycles,
            (unsigned long)(r->checked_kb / 1024),
            (unsigned long)r->buf0, (unsigned long)r->buf1);
    }
    for (i = 0; i < nlive; i++)
        core_free(live[i].h);
    /* back to the build's place, at a codec load */
    {
        struct play_spot here;

        play_note(&here);
        play_stop();
        codec_cop_set_place(place_was);
        if (here.playing)
            play_from(&here);
    }
    if (bad_arm >= 0)
    {
        say("heap: FAIL: the first bad block on arm %c, cycle %lu (the "
            "lines above)", bad_arm ? 'C' : 'F', (unsigned long)bad_cycle);
        return BENCH_FAIL;
    }
    if (!arm[0].ran || !arm[1].ran)
    {
        say("heap: Not run: arm %s didn't play", !arm[0].ran ? "F" : "C");
        return BENCH_SKIP;
    }
    say("heap: PASS: every header and every live block good on both arms "
        "(%lu walks)", (unsigned long)(arm[0].walks + arm[1].walks));
    return BENCH_PASS;
}
#else
int devbench_cop_heap(void)
{
    say("heap: Not available: no heap check in this build");
    return BENCH_SKIP;
}
#endif

#elif defined(HAVE_BCMFW_VIDEO) /* not this build: stubs; the step table (devbench.c, the device or the bench sim) exists only with BCMFW_VIDEO */

#include "devbcm.h"
#include "devbench.h"
#include "devbench_cop.h"

#ifndef DEVBENCH_SIM
int devbench_cop_t2(void)
{
    devbcm_log("p2: Not available in this build");
    return BENCH_SKIP;
}

int devbench_cop_fiq(void)
{
    devbcm_log("p1: Not available in this build");
    return BENCH_SKIP;
}

int devbench_cop_duty(void)
{
    devbcm_log("duty: Not available in this build");
    return BENCH_SKIP;
}

int devbench_cop_heap(void)
{
    devbcm_log("heap: Not available in this build");
    return BENCH_SKIP;
}
#endif
#endif
