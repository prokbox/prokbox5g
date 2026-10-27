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
 *  fe_be: the UI front end / playback back end boundary, the Rockbox side
 * (P19 S1 and the S3/S8 start; fe_be.h is the contract).
 *
 * Copyright (C) 2026 prokopto-dev
 * Copyright (C) 2026 prokopto-dev
 *
 * Portions derived from Rockbox (rockbox.org).
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
#include <string.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "thread.h"
#include "xcore.h"
#include "core_alloc.h"
#include "audio.h"
#include "appevents.h"
#include "events.h"
#include "settings.h"
#include "file.h"
#include "clockpol.h"
#include "uilog.h"
#include "button.h"
#include "fe_be.h"
#include "cop_job_core.h"
#include "storage.h"
#include "ata_idle_notify.h"
#include "powerlog.h"
#include "battlog.h"
#include "batcal.h"
#include "bootbatt.h"
#include "library.h"
#include "fe_fb.h"
#include "misc.h"               /* system_sound_play() (BE_SOUND) */
#include "anim.h"               /* UPDATE_TRACE (the simulator's trace) */
#include "artcache.h"           /* artcache_note_input() (BE_INPUT_NOTE) */
#include "version.h"            /* rbversion: the boot guard's build */
#ifdef HAVE_STORAGE_POWEROFF
#include "stor_pwr.h"           /* the power-ups a remote call caused */
#endif
#include "hang_watch.h"         /* the hang watcher */

#if NUM_CORES > 1 && !defined(SIMULATOR)
#define CROSS_POSSIBLE 1
#else
#define CROSS_POSSIBLE 0
#endif

#if (CONFIG_PLATFORM & PLATFORM_NATIVE)
#define FE_STACK_SIZE   0x2000      /* 8 KB, as main's (design 3.4) */
#else
/* the simulator: a signal stack (MINSIGSTKSZ is 32 KB on macOS) and
 * 64-bit frames */
#define FE_STACK_SIZE   (DEFAULT_STACK_SIZE + 0x10000)
#endif
#if defined(HAVE_UI_THREAD_SPLIT) && CROSS_POSSIBLE
/* S3: the shell's direct back-end calls cross by the linker's --wrap
 * (febe_rpc_stubs.S, generated; apps/apps.make adds the options) */
#define FEBE_RPC
uint64_t febe_invoke(void *fn, const uint32_t *words);
uint64_t febe_remote_c(void *fn, const uint32_t *words);
/* the real allocators (--wrap names them so): auto-pinned for the COP */
extern int __real_core_alloc(size_t size) __attribute__((weak));
extern int __real_core_alloc_ex(size_t size, struct buflib_callbacks *ops)
    __attribute__((weak));
extern int __real_core_alloc_maximum(size_t *size,
                                     struct buflib_callbacks *ops)
    __attribute__((weak));
#endif

#if defined(FEBE_RPC) && defined(HAVE_RECORDING)
/* febe_rpc.list reads audio_status() directly from the COP: its playback
 * words are uncached, a recording status would not be */
#error "the UI split's direct audio_status() needs a build without recording"
#endif

#define FE_STALL_TICKS  (20*HZ)
#define FE_COP_HOLD_TICKS (120*HZ)  /* a COP attempt that ran this long with
                                     * no stall is kept for the next boot */     /* busy this long without waiting */
#define FE_SLOW_CALL_US 50000       /* a be_call this slow is logged */

/* ---- state: who runs where ---------------------------------------------
 * mode and the thread ids are written once by the main thread before the
 * ui thread starts, read by both: uncached */
static uint32_t fe_mode_now XCORE_BSS;      /* enum fe_mode, this boot */
static uint32_t fe_mode_req;                /* the next boot's (be_svc and
                                             * main only) */
static unsigned int fe_ui_tid XCORE_BSS;    /* the shell's loop */
static unsigned int be_tid XCORE_BSS;       /* be_svc (main) */

/* the pixel hand-off's state (MF-4; the functions below) */
static volatile uint32_t fb_clean XCORE_BSS;
static volatile uint32_t fb_req XCORE_BSS, fb_ack XCORE_BSS;
static struct semaphore fb_sem XCORE_BSS;
static struct mutex fb_mtx XCORE_BSS;
static uint32_t fb_timeouts XCORE_BSS;      /* CPU callers' (fb_mtx) */

/* ---- the now record: the CPU writes, the front end reads ---------------- */
static struct {
    uint32_t seq;
    struct fe_now now;
} now_rec XCORE_BSS;
static struct fe_now now_last;              /* the reader's last good copy
                                             * (front end only) */
static uint32_t now_bad XCORE_BSS;          /* the front end's */
static uint32_t coh_bad XCORE_BSS;          /* the front end's: slot, call */
static uint32_t stalls XCORE_BSS;           /* be_svc's */

/* CPU threads only, never an interrupt, and no yield inside: Rockbox's
 * threads on one core switch only when one yields or blocks, so the CPU's
 * writers never interleave and the record has one writer at a time */
static void now_update(uint32_t set, uint32_t clear, bool track,
                       uint32_t status)
{
    uint32_t w[FE_NOW_WORDS];
    struct fe_now *n = (struct fe_now *)w;

    XCORE_ASSERT_CPU("fe_now write");
    n->flags    = (FEBE_LD(now_rec.now.flags) | set) & ~clear;
    n->track_id = FEBE_LD(now_rec.now.track_id) + (track ? 1 : 0);
    n->status   = status;
    n->events   = FEBE_LD(now_rec.now.events) + 1;
    n->sum      = febe_sum(w, FE_NOW_WORDS - 1);
    febe_seq_write_begin(&now_rec.seq);
    FEBE_ST(now_rec.now.flags, n->flags);
    FEBE_ST(now_rec.now.track_id, n->track_id);
    FEBE_ST(now_rec.now.status, n->status);
    FEBE_ST(now_rec.now.events, n->events);
    FEBE_ST(now_rec.now.sum, n->sum);
    febe_seq_write_end(&now_rec.seq);
}

bool fe_now_get(struct fe_now *out)
{
    uint32_t w[FE_NOW_WORDS];
    int tries = febe_seq_copy(&now_rec.seq, (const uint32_t *)&now_rec.now,
                              w, FE_NOW_WORDS, 32);

    if (tries > 0 && febe_sum(w, FE_NOW_WORDS - 1) == w[FE_NOW_WORDS - 1])
    {
        memcpy(&now_last, w, sizeof(now_last));
        *out = now_last;
        return true;
    }
    if ((now_bad++ & 63) == 0)      /* loud, but not a flood */
        ui_log("fe", "coh fail what=now tries=%d n=%lu", tries,
               (unsigned long)now_bad);
    *out = now_last;
    return false;
}

void be_now_mark_played(void)
{
    now_update(0, FE_NOW_STOPPED, false, (uint32_t)audio_status());
}

/* The playback events, on the poster's thread (audio, buffering: CPU).
 * Back-end code: they write only the now record (formerly ui_main.c's
 * queue_event(), which wrote a cached shell static) */
static void playback_event(unsigned short id, void *data)
{
    uint32_t status = (uint32_t)audio_status();

    (void)data;
    if (id == PLAYBACK_EVENT_TRACK_CHANGE)
    {
        now_update(0, FE_NOW_STOPPED, true, status);
        clockpol_track_changed();   /* Fit-1 C6: the new track's needs */
    }
    else if (!(status & AUDIO_STATUS_PLAY))
        now_update(FE_NOW_STOPPED, 0, false, status);  /* a stop */
    else
        now_update(0, 0, false, status);               /* a pause */
}

/* ---- requests ----------------------------------------------------------- */
static struct event_queue be_queue XCORE_BSS;
static struct semaphore be_reply XCORE_BSS;
static struct febe_slot be_slot XCORE_BSS;  /* the ui thread's (Q-U7) */
static uint32_t be_call_seq;                /* front end only */
/* the latency probe (front end only): be_call round trips */
static uint32_t call_n XCORE_BSS, call_max_us XCORE_BSS; /* the ui thread's */
#ifdef FEBE_RPC
static uint32_t rcalls XCORE_BSS, rpins XCORE_BSS; /* be_svc's */
#endif
/* the reply's storage note: be_svc writes it before the release, the ui
 * thread reads it after its wait (one writer each way, uncached) */
static uint32_t call_stor XCORE_BSS;
static uint32_t call_max_nostor_us XCORE_BSS, stor_calls XCORE_BSS;
#ifdef HAVE_UI_THREAD_SPLIT
/* The ui thread's clicks in flight (BE_SOUND): be_queue (16 events)
 * overwrites its oldest event when full, which could be a BE_Q_CALL the
 * ui thread then waits on for ever. So at most FE_POSTS_MAX are in
 * flight; past that a click is dropped (the next detent posts again) and
 * counted. post_sent: the ui thread's; post_done, the drops: be_svc's and
 * the ui thread's own, one writer each, uncached. */
#define FE_POSTS_MAX    4
static uint32_t post_sent XCORE_BSS, post_done XCORE_BSS;
static uint32_t post_drops XCORE_BSS;
/* The input notes, latched (ucop-fix2, review N2): at most one
 * BE_INPUT_NOTE in be_queue, so the click slots stay the clicks'. A note
 * posts only when none is waiting (note_posted == note_taken); be_svc
 * counts it taken BEFORE it runs the notes, so a note that finds one
 * waiting is covered by that one, and a note after the take posts again.
 * note_posted, note_comp: the ui thread's; note_taken: be_svc's. */
static uint32_t note_posted XCORE_BSS, note_taken XCORE_BSS;
static uint32_t note_comp XCORE_BSS;    /* the latest "UI competes" */
#endif

/* be_svc's priority while the shell runs on another thread (ucop-fix,
 * 2026-10-26). The ui thread blocks on every call and remote call, so its
 * latency is be_svc's wait for the CPU. At the UI's own 16, below the
 * codec band (13; HAVE_SCHED_BANDS), each call waited for the codec to
 * yield up to sched_age_limit() (20 ms) while it decodes: the panel and
 * the lists made dozens of calls a second, each a wait the inline shell
 * paid once per frame. At the codec's own priority the two take turns at
 * every yield (round robin), so neither starves the other: the codec's
 * bound is unchanged (its PCM buffer is seconds deep) and a call waits one
 * codec slice at most.
 * ucop-fix2 (review S3): only while it waits for, and serves, what the ui
 * thread blocks on or hears at once: a call (BE_Q_CALL) or a click
 * (BE_SOUND). Everything else (the input notes, pause and resume, the
 * log drain, the stall report) runs at be_svc's own priority, behind the
 * codec. svc_prio() changes it only on a change. */
#ifdef HAVE_SCHED_BANDS
#define BE_SVC_SPLIT_PRIORITY   PRIORITY_CODEC
#else
#define BE_SVC_SPLIT_PRIORITY   PRIORITY_USER_INTERFACE
#endif

#ifdef FEBE_RPC
/* The remote-call profile (be_svc only; developer builds): which back-end
 * functions the COP calls, how often and how long they run on the CPU.
 * Logged by BE_RCALL_REPORT (the ucop2 bench row) and at a slow call.
 * 16 functions; the rest are counted together. */
#define RC_PROF_N   16
static struct rc_prof {
    uint32_t fn, n, us, max_us;
} rc_prof[RC_PROF_N];
static uint32_t rc_prof_rest;

static void rc_prof_note(void *fn, uint32_t us)
{
    struct rc_prof *p = rc_prof;
    int i;

    for (i = 0; i < RC_PROF_N; i++, p++)
    {
        if (p->fn == (uint32_t)(uintptr_t)fn || p->fn == 0)
        {
            p->fn = (uint32_t)(uintptr_t)fn;
            p->n++;
            p->us += us;
            if (us > p->max_us)
                p->max_us = us;
            return;
        }
    }
    rc_prof_rest++;
}

/* the profile, the most called first (be_svc) */
static void rc_prof_report(void)
{
    bool done[RC_PROF_N] = { false };
    int k, i;

    ui_log("fe", "rcall profile n=%lu other=%lu", (unsigned long)rcalls,
           (unsigned long)rc_prof_rest);
    for (k = 0; k < 8; k++)
    {
        int best = -1;
        for (i = 0; i < RC_PROF_N; i++)
            if (!done[i] && rc_prof[i].n &&
                (best < 0 || rc_prof[i].n > rc_prof[best].n))
                best = i;
        if (best < 0)
            break;
        done[best] = true;
        ui_log("fe", "rcall fn=%08lx n=%lu ms=%lu max_us=%lu",
               (unsigned long)rc_prof[best].fn,
               (unsigned long)rc_prof[best].n,
               (unsigned long)(rc_prof[best].us / 1000),
               (unsigned long)rc_prof[best].max_us);
    }
}
#endif

#if defined(HAVE_UI_THREAD_SPLIT) && defined(HAVE_PRIORITY_SCHEDULING)
static int svc_prio_base = -1;          /* be_svc's own; -1: unchanged */
static bool svc_prio_hi;

static void svc_prio(bool hi)
{
    if (svc_prio_base < 0 || hi == svc_prio_hi)
        return;
    svc_prio_hi = hi;
    thread_set_priority(thread_self(),
                        hi ? BE_SVC_SPLIT_PRIORITY : svc_prio_base);
#ifdef SIMULATOR
    {
        /* tests/ui/ipodui-p19-svc-prio.script: both directions seen */
        static int traced;
        if (traced < 4)
        {
            traced++;
            UPDATE_TRACE("fe svc prio=%s", hi ? "codec" : "own");
        }
    }
#endif
}
#else
#define svc_prio(hi)    do { } while (0)
#endif

static uint32_t now_us(void)
{
#if CROSS_POSSIBLE
    return USEC_TIMER;
#else
    return (uint32_t)current_tick * (1000000 / HZ);
#endif
}

#define BE_Q_CALL   0x7f00              /* the slot is FILLED */
#define BE_Q_STALL  0x7f01              /* the watchdog fired */
#define BE_Q_LOGDRAIN 0x7f02            /* the COP's log ring has lines */

static bool split_active(void)
{
#ifdef HAVE_UI_THREAD_SPLIT
    return fe_mode_now != FE_INLINE;
#else
    return false;               /* the shell runs inline, always */
#endif
}

bool fe_be_is_service_thread(void)
{
    unsigned int t = thread_self();
    return t == be_tid || (t == fe_ui_tid && !split_active());
}

bool fe_be_route(void)
{
    return split_active() && thread_self() == fe_ui_tid;
}

bool fe_is_ui_thread(void)
{
    return thread_self() == fe_ui_tid;
}

static void cache_ops(unsigned ops)
{
#if CROSS_POSSIBLE
    if (ops & COPJ_C_COMMIT_DISCARD_I)
        commit_discard_idcache();
    else if (ops & COPJ_C_COMMIT_DISCARD)
        commit_discard_dcache();
    else if (ops & COPJ_C_COMMIT)
        commit_dcache();
#else
    (void)ops;
#endif
}

static bool cross_core(void)
{
    return CROSS_POSSIBLE && fe_mode_now == FE_COP;
}

/* The back end's handlers: CPU, be_svc (or the caller, inline) */
static intptr_t be_dispatch(uint32_t kind, uint32_t *args, unsigned n)
{
    (void)args; (void)n;
    switch (kind)
    {
    case BE_NOP:
        return 0;
    case BE_AUDIO_PAUSE:
        audio_pause();
        return 0;
    case BE_AUDIO_RESUME:
        audio_resume();
        return 0;
    case BE_NOW_PLAYED:
        be_now_mark_played();
        return 0;
    case BE_SETTINGS_SAVE:
        return settings_save();
    case BE_PASS:
        /* S2: the services that ran at the top of every UI pass, in
         * their old order; the ui thread waits, as it did */
        ui_log_service();           /* a due log rotation, disk spinning */
        powerlog_service();         /* #10, devmode: rotation, overflow */
        battlog_service();          /* P19 PW4: the battery session line */
        batcal_service();           /* #70: a run's fit, writes, disk up */
        bootbatt_service();         /* the drained latch at 0 % */
        clockpol_service();         /* Fit-1 C6: track needs, verdicts */
        lib_pool_idle();            /* fb-stall: a view pool size owed */
        settings_save_service();    /* cfgsafe: a changed setting, ~2 s on */
        return 0;
    case BE_LOG_FLUSH:
        ui_log_flush();
        return 0;
    case BE_IDLE_FLUSH:
        call_storage_idle_notifys(true);
        return 0;
#ifdef HAVE_UI_THREAD_SPLIT
    case BE_SOUND:
        /* ucop-fix: the mixer, the PCM driver's lock and the beep buffers
         * are CPU-only (click.h); here fe_be_route() is false, so this
         * plays */
#ifdef SIMULATOR
        /* tests/ui/ipodui-p19-ucop-click.script: the route was taken */
        UPDATE_TRACE("fe sound=%lu on=svc", (unsigned long)args[0]);
#endif
        system_sound_play((enum system_sound)args[0]);
        return 0;
    case BE_INPUT_NOTE:
        /* taken first: a note the ui thread latches from here on posts
         * again (fe_note_input()) */
        note_taken++;
        artcache_note_input();      /* the art builder keeps off for 2 s */
        clockpol_note_input(note_comp != 0);
        return 0;
#endif
    case BE_RCALL_REPORT:
#ifdef FEBE_RPC
        rc_prof_report();
#endif
        return 0;
#ifdef FEBE_RPC
    case BE_RCALL:
    {
        /* S3: a function the shell called on the COP (serve_call() did
         * the CPU's cache operations) */
        void *fn = (void *)(uintptr_t)args[0];
        uint32_t t0 = USEC_TIMER;
        uint64_t v = febe_invoke(fn, &args[1]);
        rc_prof_note(fn, USEC_TIMER - t0);
        /* every block the shell allocates from the COP is pinned for its
         * life: a compaction on this core must never move memory the
         * COP holds a pointer into (design 2.4); core_free() ignores the
         * pin.  The shell's own pin/unpin pairs nest on top. */
        if ((fn == (void *)__real_core_alloc ||
             fn == (void *)__real_core_alloc_ex ||
             fn == (void *)__real_core_alloc_maximum) && (int)v > 0)
        {
            core_pin((int)v);
            rpins++;
        }
        args[13] = (uint32_t)v;
        args[14] = (uint32_t)(v >> 32);
        rcalls++;
        return 0;
    }
#endif
    }
    ui_log("fe", "be kind=%lu unknown", (unsigned long)kind);
    return -1;
}

#ifdef FEBE_RPC
/* S3 (febe_rpc_stubs.S): a back-end function the COP called.  words: r0-r3
 * and the caller's first four stack words.  Returns its r0:r1. */
uint64_t febe_remote_c(void *fn, const uint32_t *words)
{
    uint32_t a[15];
    int i;

    if (!fe_be_route())
        return febe_invoke(fn, words);  /* another COP thread: as before */
    a[0] = (uint32_t)(uintptr_t)fn;
    for (i = 0; i < 12; i++)
        a[1 + i] = words[i];
    a[13] = a[14] = 0;
    (void)be_call(BE_RCALL, FEBE_F_IN_BULK | FEBE_F_WROTE, a, 15);
    return (uint64_t)a[13] | ((uint64_t)a[14] << 32);
}
#endif

/* inline: the default mode, and any thread but the ui thread */
static bool serve_inline(void)
{
    return !split_active() || !fe_is_ui_thread();
}

#ifdef HAVE_UI_THREAD_SPLIT
static bool bounded_post(uint32_t kind)
{
    return kind == BE_SOUND;
}
#endif

void be_post(enum be_kind kind, intptr_t value)
{
    if (serve_inline())
    {
        uint32_t a = (uint32_t)value;
        be_dispatch(kind, &a, 1);
        return;
    }
#ifdef HAVE_UI_THREAD_SPLIT
    if (bounded_post(kind))
    {
        if (post_sent - post_done >= FE_POSTS_MAX)
        {
            post_drops++;
            return;
        }
        post_sent++;
    }
#endif
    queue_post(&be_queue, kind, value);
}

intptr_t be_call(enum be_kind kind, uint32_t flags, uint32_t *args,
                 unsigned n)
{
    uint32_t ret = 0, t0;
    int waits = 0, hw;

    if (serve_inline())
        return be_dispatch(kind, args, n);
    if (!febe_slot_fill(&be_slot, ++be_call_seq, kind, flags, args, n))
    {
        /* only the ui thread fills, and it waits for each reply: a busy
         * slot is a bug; served inline so nothing waits for ever */
        coh_bad++;
        ui_log("fe", "coh fail what=slot kind=%d state=%lu", (int)kind,
               (unsigned long)be_slot.state);
        return be_dispatch(kind, args, n);
    }
    cache_ops(febe_ops_post(flags, cross_core()));
    if (cross_core())
        fb_clean = 1;               /* written back above; blocked below */
    t0 = now_us();
    queue_post(&be_queue, BE_Q_CALL, 0);
    /* never withdrawn: the back end may write the slot and our buffers
     * until DONE, so we wait however long it takes, and say so */
    /* the hang watcher: waiting on be_svc (watched), which, if stuck too,
     * is the thread named */
    hw = hang_peer_begin();
    while (semaphore_wait(&be_reply, 10*HZ) != OBJ_WAIT_SUCCEEDED)
        ui_log("fe", "call stall kind=%d waited=%ds", (int)kind,
               10 * ++waits);
    hang_idle_end(hw);
    /* the handler ran shell services and back-end code that may have
     * written anything this core reads: always a drop on the COP */
    if (cross_core())
        fb_clean = 0;
    cache_ops(febe_ops_reply(flags | FEBE_F_WROTE, cross_core()));
    febe_slot_collect(&be_slot, args, n, &ret);
    t0 = now_us() - t0;
    call_n++;
    if (t0 > call_max_us)
        call_max_us = t0;
    if (call_stor)
        stor_calls++;               /* storage powered up on the way */
    else if (t0 > call_max_nostor_us)
        call_max_nostor_us = t0;
    if (t0 > FE_SLOW_CALL_US)
        ui_log("fe", "call slow kind=%d us=%lu n=%lu fn=%08lx stor=%lu",
               (int)kind, (unsigned long)t0, (unsigned long)call_n,
               (unsigned long)(kind == BE_RCALL && n ? args[0] : 0),
               (unsigned long)call_stor);
    if (ret == FEBE_RET_BADSUM)
        coh_bad++;
    if (ret == FEBE_RET_BADSUM)
        ui_log("fe", "coh fail what=call kind=%d seq=%lu", (int)kind,
               (unsigned long)be_call_seq);
    return (intptr_t)(int32_t)ret;
}

static void serve_call(void)
{
    uint32_t a[FEBE_ARG_WORDS];
    uint32_t ret;
    bool bad;
    int i;

    if (!febe_slot_take(&be_slot, &bad))
        return;
    for (i = 0; i < FEBE_ARG_WORDS; i++)
        a[i] = FEBE_LD(be_slot.arg[i]);
    /* S3: every call from the COP is a full sync point on this side too.
     * The COP wrote its cache back before posting and is blocked until
     * the reply; this core drops its own lines first (the handler reads
     * the COP's memory fresh, and no stale line of this core lies over
     * it), and writes back after (the COP drops before reading).  One
     * core: nothing (cross_core() false). */
    cache_ops(cross_core() ? COPJ_C_COMMIT_DISCARD : 0);
#ifdef HAVE_STORAGE_POWEROFF
    uint32_t ons = storage_power_state()->ons;
#endif
    ret = bad ? FEBE_RET_BADSUM
              : (uint32_t)be_dispatch(FEBE_LD(be_slot.kind), a,
                                      FEBE_ARG_WORDS);
#ifdef HAVE_STORAGE_POWEROFF
    call_stor = storage_power_state()->ons != ons;
#endif
    cache_ops(cross_core() ? COPJ_C_COMMIT : 0);
    for (i = 0; i < FEBE_ARG_WORDS; i++)
        FEBE_ST(be_slot.arg[i], a[i]);
    febe_slot_finish(&be_slot, ret);
    semaphore_release(&be_reply);
}

/* ---- the boot guard (R7, Q-U5) ------------------------------------------
 * FE_GUARD_FILE holds the rbversion of a build whose COP attempt has not
 * held (it is armed for the attempt's first FE_COP_HOLD_TICKS) or that
 * stalled: that build's next boots run the ui thread on the CPU.  A new
 * build ignores (and removes) another's guard, so the 0.8.0 flip, or any
 * update, tries the COP afresh.  Read only when the boot would ask for the
 * COP; written twice by a COP boot (armed, then cleared once it holds);
 * be_svc and main only. */
#ifdef HAVE_UI_THREAD_SPLIT
static bool guard_this_build(void)
{
    char b[96];
    size_t len = strlen(rbversion);
    int n, fd = open(FE_GUARD_FILE, O_RDONLY);

    if (fd < 0)
        return false;
    n = read(fd, b, sizeof(b));
    close(fd);
    if (n >= 0 && (size_t)n == len && !memcmp(b, rbversion, len))
        return true;
    remove(FE_GUARD_FILE);          /* another build's: not ours to keep */
    return false;
}

static void guard_arm(void)
{
    int fd = open(FE_GUARD_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);

    if (fd >= 0)
    {
        write(fd, rbversion, strlen(rbversion));
        close(fd);
    }
    fe_mode_req = FE_CPU_THREAD;
}

static void guard_clear(void)
{
    remove(FE_GUARD_FILE);
}

/* what a boot asks for before the guard: the developer switch, the
 * hidden setting, the build default (fe_be.h) */
static enum fe_mode mode_asked(void)
{
#ifdef HAVE_IPOD_UI_DEVMODE
    char b[4] = { 0 };
    int fd = open(FE_MODE_MARKER, O_RDONLY);

    if (fd >= 0)
    {
        read(fd, b, 3);
        close(fd);
        if (!memcmp(b, "cop", 3))
            return FE_COP;
        if (!memcmp(b, "cpu", 3))
            return FE_CPU_THREAD;
        if (!memcmp(b, "inl", 3))
            return FE_INLINE;
    }
#endif
    switch (global_settings.ipodui_ui_core)
    {
        case 1: return FE_INLINE;
        case 2: return FE_CPU_THREAD;
        case 3: return FE_COP;
        default: break;
    }
    return (enum fe_mode)IPODUI_UI_CORE_DEFAULT;
}
#else
static inline void guard_arm(void) { }
static inline void guard_clear(void) { }
#endif /* HAVE_UI_THREAD_SPLIT */

/* ---- the watchdog (R7): input the ui thread never takes ----------------
 * A front end that has stopped (a hung or dead COP, a deadlock) stops
 * taking input; every live loop of the shell (screens, prompts, the USB
 * screen, Extras) takes it.  So the CPU's tick watches button_queue: not
 * empty for FE_STALL_TICKS on end means stalled.  One compare per tick,
 * no wake, and nothing at all while the queue is empty (the dark).
 * busy_since and stall_seen: the tick task is their only writer. */
static long busy_since, stall_seen;

static void fe_tick(void)
{
    if (button_queue_empty())
    {
        busy_since = 0;
        return;
    }
    if (!busy_since)
        busy_since = current_tick | 1;
    else if (busy_since != stall_seen &&
             TIME_AFTER(current_tick, busy_since + FE_STALL_TICKS))
    {
        stall_seen = busy_since;
        queue_post(&be_queue, BE_Q_STALL, busy_since);
    }
}

static void stall_report(long since)
{
    stalls++;
    long held = (long)(current_tick - since) / HZ;

    ui_log("fe", "stall mode=%s input_waiting_s=%ld calls=%lu",
           fe_be_mode_name((enum fe_mode)fe_mode_now), held,
           (unsigned long)call_n);
#ifdef FEBE_RPC
    ui_log("fe", "stall rcalls=%lu rpins=%lu", (unsigned long)rcalls,
           (unsigned long)rpins);
#endif
    if (fe_mode_now == FE_COP)
    {
        /* the release fallback (Q-U5): the next boots of this build run
         * the ui thread on the CPU; logged so the bench and the owner
         * see it */
        guard_arm();
        ui_log("fe", "FALLBACK next boot ui on cpu (cop stall)");
    }
}

/* ---- the pixel hand-off (MF-4) -------------------------------------------
 * fb_clean: the ui thread (on the COP) is blocked and wrote its cache back
 * first; its only writer is the ui thread.  fb_req: the fences asked, CPU
 * callers' (serialised by fb_mtx); fb_ack: the fences answered, the ui
 * thread's.  A late answer to a timed-out fence never satisfies a newer
 * one (the request number). */

static bool on_cop_mode(void)
{
    return fe_mode_now == FE_COP;
}

void fe_wait_begin(void)
{
    if (on_cop_mode() && CURRENT_CORE != CPU)
    {
        commit_dcache();
        fb_clean = 1;
    }
}

void fe_wait_end(void)
{
    if (on_cop_mode() && CURRENT_CORE != CPU)
        fb_clean = 0;
}

void fe_fb_serve(void)
{
    if (CURRENT_CORE != CPU)
        commit_dcache();
    fb_ack = fb_req;
    semaphore_release(&fb_sem);
}

int fe_fb_publish(const struct fe_fb_rect *rect, int timeout_ms)
{
    uint32_t req;
    long end;
    int res = FE_FB_PUBLISHED;

    if (!on_cop_mode())
        return FE_FB_LOCAL;
    if (CURRENT_CORE != CPU)
    {
        commit_dcache();            /* the COP's own pixels */
        return FE_FB_LOCAL;
    }
    if (fb_clean)
        return FE_FB_CLEAN;
    mutex_lock(&fb_mtx);
    req = fb_req + 1;
    fb_req = req;
    button_queue_post(SYS_FE_FB_FENCE, 0);
    end = current_tick + MAX(1, timeout_ms * HZ / 1000);
    while ((int32_t)(fb_ack - req) < 0 && !fb_clean)
    {
        long left = end - current_tick;
        if (left <= 0)
        {
            res = FE_FB_TIMEOUT;
            fb_timeouts++;
            break;
        }
        semaphore_wait(&fb_sem, left);
    }
    if (res == FE_FB_PUBLISHED && fb_clean && (int32_t)(fb_ack - req) < 0)
        res = FE_FB_CLEAN;          /* it blocked (and wrote back) first */
    mutex_unlock(&fb_mtx);
    if (res == FE_FB_TIMEOUT)
        ui_log("fe", "fb fence TIMEOUT rect=%d,%d,%d,%d n=%lu",
               rect ? rect->x : 0, rect ? rect->y : 0,
               rect ? rect->w : LCD_WIDTH, rect ? rect->h : LCD_HEIGHT,
               (unsigned long)fb_timeouts);
    return res;
}

void fe_be_stats_get(struct fe_be_stats *st)
{
    st->calls = call_n;
    st->call_max_us = call_max_us;
#ifdef FEBE_RPC
    st->rcalls = rcalls;
#else
    st->rcalls = 0;
#endif
    st->stalls = stalls;
    st->coh_fails = now_bad + coh_bad;
    st->fb_timeouts = fb_timeouts;
    st->call_max_nostor_us = call_max_nostor_us;
    st->stor_calls = stor_calls;
#ifdef HAVE_UI_THREAD_SPLIT
    st->post_drops = post_drops;
#else
    st->post_drops = 0;
#endif
#ifdef XCORE_CHECKS
    st->offcore = xcore_offcore;
#else
    st->offcore = 0;
#endif
}

void fe_note_input(bool ui_competes)
{
#ifdef HAVE_UI_THREAD_SPLIT
    if (!serve_inline())
    {
        /* latched (N2): one note in be_queue at most */
        note_comp = ui_competes;
        if (note_posted == note_taken)
        {
            note_posted++;
            queue_post(&be_queue, BE_INPUT_NOTE, 0);
        }
        return;
    }
#endif
    artcache_note_input();          /* the art builder keeps off for 2 s */
    clockpol_note_input(ui_competes);
}

/* the COP's first log line since the last drain: be_svc drains the ring
 * (uilog.c).  Any core; a post only when the ui thread is split off */
void fe_be_log_kick(void)
{
    if (split_active())
        queue_post(&be_queue, BE_Q_LOGDRAIN, 0);
}

/* ---- the switch ------------------------------------------------------- */
const char *fe_be_mode_name(enum fe_mode m)
{
    return m == FE_CPU_THREAD ? "cpu" : m == FE_COP ? "cop" : "inline";
}

enum fe_mode fe_be_mode(void)
{
    return (enum fe_mode)fe_mode_now;
}

enum fe_mode fe_be_mode_next(void)
{
    return (enum fe_mode)fe_mode_req;
}

static bool guard_held;             /* this boot: the guard said cpu */

static enum fe_mode mode_read(void)
{
#ifdef HAVE_UI_THREAD_SPLIT
    enum fe_mode m = mode_asked();

    if (m == FE_COP && guard_this_build())
    {
        guard_held = true;
        m = FE_CPU_THREAD;
    }
    return m;
#else
    return FE_INLINE;
#endif
}

/* The developer switch (devmode.c, devbench.c): the marker, and an
 * explicit choice starts without a guard.  Release builds have no
 * switch: the hidden setting is edited in config.cfg. */
void fe_be_mode_set_next(enum fe_mode m)
{
#if defined(HAVE_UI_THREAD_SPLIT) && defined(HAVE_IPOD_UI_DEVMODE)
    int fd = open(FE_MODE_MARKER, O_WRONLY | O_CREAT | O_TRUNC, 0666);

    if (fd >= 0)
    {
        write(fd, fe_be_mode_name(m), 3);
        close(fd);
    }
    guard_clear();
    fe_mode_req = m;
    ui_log("fe", "switch next=%s", fe_be_mode_name(m));
#else
    (void)m;
#endif
}

/* The developer switch taken away (devbench.c: the second-core soak not
 * passed): no marker, so the next boot runs what the hidden setting or the
 * build default says.  The guard is left as it is: it only ever holds a
 * stalled COP attempt back. */
void fe_be_mode_clear_next(void)
{
#if defined(HAVE_UI_THREAD_SPLIT) && defined(HAVE_IPOD_UI_DEVMODE)
    enum fe_mode m;

    remove(FE_MODE_MARKER);
    m = mode_asked();               /* now the setting or the default */
    if (m == FE_COP && guard_this_build())
        m = FE_CPU_THREAD;          /* as mode_read(), without its flag */
    fe_mode_req = m;
    ui_log("fe", "switch cleared next=%s", fe_be_mode_name(fe_be_mode_next()));
#endif
}

/* Why the COP may not run the shell yet: every rule the move needs that is
 * not in place (design section 7.1).  NULL: none (the flip may go ahead).
 * A loud refusal, never a silent one: the boot log says which. */
static const char *fe_cop_refusal(void)
{
#if !CROSS_POSSIBLE
    return "one core";
#else
    /* What is in place (S1-S4): the shell's direct back-end calls cross
     * by remote call (febe_rpc_stubs.S; febe_link_check --strict: 0
     * direct), and so does every front-end call into the shell's CPU-only
     * objects (the art cache with its builder's buffers, the services
     * with CPU callbacks); each is a full sync point on both cores; every
     * block allocated from the COP is pinned for its life, the fonts
     * too; the shell's and the drawing code's data on whole cache lines
     * of their own (xcore_line_check: 0 shared); the global settings and
     * the few objects the shell and CPU callbacks both touch uncached;
     * the job ring's owner claimed by the ui thread.  Still open:
     *  - the pixel hand-off (BCM MF-4): a CPU push of the COP's frame
     *    (lcd_awake's wake frame, the video overlay) needs the COP's
     *    write-back fence (fe_fb_publish(), S7);
     * (S10, the job driver's DECODE hand-off across cores, is merged:
     * p19-s10-handoff bb620db556; its owner hooks follow the claiming
     * thread's core.)
     *  - silicon: the stubs, and engine 2 started from the COP (B10). */
#ifdef HAVE_FE_FB_HANDOFF
    /* MF-4 merged: every rule is in place; what is left is silicon (the
     * stubs, engine 2 from the COP: the bench's B10 row and the soak).
     * Developer and release builds, with the boot guard (an attempt that
     * does not hold runs the CPU on this build's next boots). */
    return NULL;
#else
    return "pixel hand-off (MF-4), silicon (B10)";
#endif
#endif
}

/* ---- start ----------------------------------------------------------- */
static void (*fe_shell)(void);

#if CROSS_POSSIBLE
/* The COP start handshake: the starter gives up after a second (an old
 * bootloader leaves the COP parked, cop_job.c) and the COP's thread must
 * then never run the shell too.  Check-and-claim under one corelock. */
static struct corelock fe_start_cl XCORE_BSS;
static volatile uint32_t fe_started XCORE_BSS, fe_abandoned XCORE_BSS;
#endif

static void ui_thread_entry(void)
{
#if CROSS_POSSIBLE
    if (fe_mode_now == FE_COP)
    {
        bool run;
        corelock_lock(&fe_start_cl);
        run = !fe_abandoned;
        if (run)
            fe_started = 1;
        corelock_unlock(&fe_start_cl);
        if (!run)
            return;                 /* the CPU took over: end quietly */
    }
#endif
    fe_ui_tid = thread_self();
    fe_shell();                     /* never returns */
}

static void be_svc_run(void) NORETURN_ATTR;
static void be_svc_run(void)
{
    struct queue_event ev;
    long hold_at = 0;               /* FE_COP: when the attempt is kept */

    ui_log("fe", "be_svc up mode=%s", fe_be_mode_name(fe_mode_now));
    hang_watch_register(HANG_BE);   /* the hang watcher: a pass a request */
#if defined(HAVE_UI_THREAD_SPLIT) && defined(HAVE_PRIORITY_SCHEDULING)
    if (split_active())
    {
        /* waits at the codec's priority (a call wakes it at once), serves
         * only calls and clicks there (svc_prio()) */
        svc_prio_base = thread_get_priority(thread_self());
        svc_prio(true);
    }
#endif
    if (fe_mode_now == FE_COP)
    {
        /* the boot guard: until this attempt has held, this build's next
         * boot runs the ui thread on the CPU (a hang, a crash or a reset
         * now never brings the COP back by itself) */
        guard_arm();
        hold_at = current_tick + FE_COP_HOLD_TICKS;
        ui_log("fe", "cop attempt: next boot cpu until it holds %ds",
               FE_COP_HOLD_TICKS / HZ);
    }
    for (;;)
    {
        int hw;

        hang_beat(HANG_BE);
        if (hold_at)
        {
            long left = hold_at - current_tick;
            if (left <= 0)
            {
                if (stall_seen == 0)    /* no stall in the attempt */
                {
                    guard_clear();
                    fe_mode_req = FE_COP;
                    ui_log("fe", "cop held %ds: next boot cop",
                           FE_COP_HOLD_TICKS / HZ);
                }
                hold_at = 0;
                continue;
            }
            if (queue_empty(&be_queue))
                svc_prio(true);         /* the wait: ready for a call */
            hw = hang_idle_begin();
            queue_wait_w_tmo(&be_queue, &ev, left);
            hang_idle_end(hw);
            if (ev.id == SYS_TIMEOUT)
                continue;
        }
        else
        {
            if (queue_empty(&be_queue))
                svc_prio(true);
            hw = hang_idle_begin();
            queue_wait(&be_queue, &ev);
            hang_idle_end(hw);
        }
#ifdef HAVE_UI_THREAD_SPLIT
        svc_prio(ev.id == BE_Q_CALL || ev.id == BE_SOUND);
#endif
        if (ev.id == BE_Q_CALL)
            serve_call();
        else if (ev.id == BE_Q_STALL)
            stall_report((long)ev.data);
        else if (ev.id == BE_Q_LOGDRAIN)
            ui_log_drain();
        else
        {
            uint32_t a = (uint32_t)ev.data;
            be_dispatch(ev.id, &a, 1);
#ifdef HAVE_UI_THREAD_SPLIT
            if (bounded_post(ev.id))
                post_done++;
#endif
        }
    }
}

void fe_be_init(void)
{
    XCORE_ASSERT_CPU("fe_be_init");
    be_tid = thread_self();
    fe_ui_tid = be_tid;             /* FE_INLINE until a ui thread runs */
    fe_mode_now = FE_INLINE;
    /* nothing played this boot: Now Playing's row hidden (Rev 3, #58) */
    now_update(FE_NOW_STOPPED, 0, false, (uint32_t)audio_status());
    add_event(PLAYBACK_EVENT_TRACK_CHANGE, playback_event);
    add_event(PLAYBACK_EVENT_STOP_OR_PAUSE, playback_event);
    queue_init(&be_queue, false);   /* no broadcasts: be_svc never acks */
    semaphore_init(&be_reply, 1, 0);
    semaphore_init(&fb_sem, 1, 0);
    mutex_init(&fb_mtx);
    fe_mode_req = mode_read();
}

/* shell() never returns; if it ever did, nothing else may run here */
static void run_shell(void (*shell)(void)) NORETURN_ATTR;
static void run_shell(void (*shell)(void))
{
    shell();
    ui_log("fe", "shell returned");
    for (;;)
        sleep(HZ*60);
}

void fe_be_start(void (*shell)(void))
{
    enum fe_mode want = (enum fe_mode)fe_mode_req;
    unsigned int core = CPU;
    int h;
    void *stack;
    const char *why;

    fe_shell = shell;
#ifndef HAVE_UI_THREAD_SPLIT
    want = FE_INLINE;               /* mode_read() said so: no thread */
#endif
    if (guard_held)
        ui_log("fe", "guard: this build's cop attempt did not hold; "
               "ui thread on the cpu");
    if (want == FE_INLINE)
    {
        run_shell(shell);
    }
    if (want == FE_COP && (why = fe_cop_refusal()) != NULL)
    {
        ui_log("fe", "cop refused: %s; ui thread on the cpu", why);
        want = FE_CPU_THREAD;
    }
#if CROSS_POSSIBLE
    if (want == FE_COP)
        core = COP;
#endif
    (void)core;
    /* the ui stack: 8 KB, pinned for the shell's life, before playback */
    h = core_alloc(FE_STACK_SIZE);
    if (h <= 0)
    {
        ui_log("fe", "FALLBACK inline (no stack)");
        run_shell(shell);
    }
    core_pin(h);
    stack = core_get_data(h);
    fe_mode_now = want;
    tick_add_task(fe_tick);
#if CROSS_POSSIBLE
    corelock_init(&fe_start_cl);
    fe_started = fe_abandoned = 0;
#endif
    if ((fe_ui_tid = create_thread(ui_thread_entry, stack, FE_STACK_SIZE, 0,
                      "ui" IF_PRIO(, PRIORITY_USER_INTERFACE)
                      IF_COP(, core))) == 0)
    {
        fe_ui_tid = be_tid;
        tick_remove_task(fe_tick);
        fe_mode_now = FE_INLINE;
        ui_log("fe", "FALLBACK inline (no thread slot)");
        run_shell(shell);
    }
#if CROSS_POSSIBLE
    if (want == FE_COP)
    {
        /* the COP must pick it up within a second, or the CPU runs it */
        long end = current_tick + HZ;
        bool up;
        while (!fe_started && TIME_BEFORE(current_tick, end))
            sleep(1);
        corelock_lock(&fe_start_cl);
        up = fe_started;
        if (!up)
            fe_abandoned = 1;
        corelock_unlock(&fe_start_cl);
        if (!up)
        {
            ui_log("fe", "FALLBACK cpu thread (the cop never started)");
            guard_arm();            /* this build's next boots: no wait */
            fe_mode_now = FE_CPU_THREAD;
            /* a second stack: the abandoned thread may still own the
             * first if the COP wakes later */
            h = core_alloc(FE_STACK_SIZE);
            if (h > 0)
                core_pin(h);
            if (h <= 0 || (fe_ui_tid = create_thread(ui_thread_entry,
                        core_get_data(h), FE_STACK_SIZE, 0, "ui" IF_PRIO(,
                        PRIORITY_USER_INTERFACE) IF_COP(, CPU))) == 0)
            {
                fe_ui_tid = be_tid;
                fe_mode_now = FE_INLINE;
                tick_remove_task(fe_tick);
                ui_log("fe", "FALLBACK inline");
                run_shell(shell);
            }
        }
    }
#endif
    be_svc_run();
}
