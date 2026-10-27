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

/* P19 19-6: covers decoded on the BCM2722 (art_bcm.h; the recipe:
 * docs/research/bcm2722/album-render.md section 14) */

#include "config.h"
#include "ipodui/art_bcm.h"

#ifdef HAVE_ART_BCM
#include <string.h>
#include "file.h"
#include "kernel.h"
#include "bcmjob_pp.h"
#include "bcmjob.h"
#include "ipodui/anim.h"            /* UPDATE_TRACE */
#include "ipodui/artcache.h"        /* artcache_aux_wake() */
#include "ipodui/drift_jobs.h"
#include "ipodui/library.h"         /* np_pb_ab_wake() */
#include "ipodui/mq_jobs.h"
#include "ipodui/nav_core.h"        /* NAV_DRIFT_SRC */
#include "ipodui/uilog.h"
#include "ipodui/unlit.h"           /* ui_unlit_active() */
#include "core_alloc.h"             /* the overlay's piece buffer */
#include "system.h"
#include "xcore.h"                  /* the piece's hand-off across cores */
#include "video/bcmvideo_image.h"   /* BCMVIDEO_DECOVL_*: dec-ovl.bin's record */
#include "backlight.h"
#ifdef SIMULATOR
#include "ipodui/simclock.h"        /* art_bcm_pass_now() */
#endif

/* The output's tag in the key's top byte (section 4.3) */
#define TAG_BOX     0xA1ull
#define TAG_PANEL   0xA2ull
/* The art thread waits this long at most for the UI thread to free the
 * last decode's slot before it takes the CPU path (a full pool would
 * fail the ALLOC, and a failed job counts in J_ERRORS) */
#define FREE_WAIT   (HZ * 2)
/* A deadline past this is no use: the CPU twin, or the next chance */
#define DEADLINE_MAX 60000

/* The last decode's slot (the art thread's while held) */
static struct
{
    uint32_t slot, epoch;
    uint32_t a, b;                  /* COMMIT's F16w */
    uint32_t sa, sb;                /* the rows read so far */
    uint16_t cw, ch;
    uint64_t key;                   /* the album (no tag) */
    bool held;
    bool panel;                     /* a J_CLS_DRIFT panel */
    bool resident;                  /* kept: art_bcm_keep() */
} ab;

/* The resident panel's mailbox (art thread -> the owner's pass): one
 * install or one drop at a time, each done before the next is posted */
static volatile bool inst_owed, drop_owed;
static volatile uint32_t inst_slot, inst_epoch;
static volatile uint64_t inst_key;
/* published by the owner's pass: the resident's album, its epoch */
static volatile uint64_t pub_key;
static volatile uint32_t pub_epoch;
static volatile int res_seq;
static volatile bool np_shown;
/* the thread that decodes (the art thread): the only reader */
static unsigned int dec_thread;

/* A released slot, for the UI thread's FREE (one at a time) */
static volatile uint32_t free_slot, free_epoch;
static volatile bool free_owed;
/* DECODE offered now (the UI thread's look, art_bcm_service()) */
static volatile bool have_decode;
/* ... from the merged image's overlay, whose work area is a J_CLS_DRIFT
 * slot the driver lends per decode (bcmjob_dec_lends()): the drift's own
 * cover leaves the pool first (the UI thread's dj_cover_park(), asked by
 * the art thread through park_owed) */
static volatile bool lends, park_owed;
/* The overlay's load for a decode due now (album-render.md section 5.3;
 * the freeze audit, 2026-10-26, moved the file's read off the UI thread,
 * where an open() or read() that never came back froze the shell): the
 * art thread reads dec-ovl.bin a piece at a time into its own buffer
 * (BCMJOB_DEC_PIECE from the core allocator for the load only, as a
 * decode's piece buffer) and posts each piece (ovl_post); the UI thread's
 * pass, the job owner, puts it to the BCM (bcmjob_ovl_put(): the port
 * and the BCM's checks, every wait bounded, no file) and answers (ovl_rc,
 * then ovl_done).  One writer a word: the pass sets ovl_take before it
 * looks at ovl_post again, the art thread withdraws a piece by clearing
 * ovl_post before it looks at ovl_take, so at least one of them sees the
 * other (the job driver's take, bcmjob-core.c S10) and a withdrawn
 * piece's buffer is freed only once the pass is done with it.  The strip
 * is no longer used.  ovl_due: the pass's look, a load is due (DECODE
 * missing, the image's region the record's, a try left this epoch) */
static volatile bool ovl_due;
static volatile uint32_t ovl_post, ovl_take, ovl_done; /* piece numbers */
static uint32_t ovl_seq;                /* the art thread's last */
static const void *volatile ovl_buf;
static volatile uint32_t ovl_off, ovl_n, ovl_size;
static volatile int ovl_rc;             /* the pass's answer to ovl_done */
static volatile unsigned int ovl_core;  /* the reader's core */
/* the posted piece's kind (bcmjob_pp.h BCMJOB_OVL_*) and file's record:
 * the art thread loads DECODE's for its decode and, between decodes, the
 * visualizer's when the visualizer asks (art_bcm_viz_ask(); one load at
 * a time, both on the art thread, so the merged image's shared region
 * swaps between them in order: visualizer.md 8.5) */
static volatile int ovl_kind;
static const struct bcmjob_ovl_file *volatile ovl_fp;
/* the epoch whose loads were refused (0: none) and how many: two (the
 * driver's tries, a torn upload's second) and no more in it (a missing
 * file stays "due" to the driver, and each try is an open()); the art
 * thread's */
static volatile uint32_t ovl_bad_ep;
static volatile int ovl_bad_n;
/* A load's end, as the art thread sees it */
enum
{
    OVL_OK = 0,
    OVL_NET_WAIT,                   /* not loaded within OVL_WAIT */
    OVL_NET_REFUSED,                /* the load refused (the driver says why) */
    OVL_NET_GONE,                   /* no longer due (tries spent, the BCM down) */
    OVL_NET_BUF,                    /* no memory for the piece buffer */
    OVL_NET_BUSY,                   /* a DECODE held the BCM: later */
    OVL_NET_N,
    OVL_CANCELLED = OVL_NET_N,      /* the user came back: the caller's */
};
/* The art thread waits this long at most for the load: the pass is woken
 * at once for each piece (about 50 ms in all: three reads, the BCM's
 * start-up checks); a slide or a glide in the way takes a few hundred ms
 * more.  Past it the PP decodes (a panel on the PP: 2-20 s [silicon,
 * sitting 3]; a second's wait on the art thread shows nowhere) */
#define OVL_WAIT    HZ
/* ... and a piece the pass took when the art thread gave up: its port
 * work is bounded (the driver's OVERLAY call, 100 ms; a dead BCM's
 * restart about a second), so the buffer is freed after it; past this
 * it is kept, never freed under the pass, and said once (FAULT) */
#define OVL_TAKEN_WAIT (3 * HZ)
/* ... and while a taken piece's wait on the BCM is posted (the FENCE, the
 * OVERLAY reply: BCMJOB_OVL_AGAIN), the pass is woken this often to ask
 * again (the OVERLAY's checks take about 30 ms) */
#define OVL_AGAIN_TICKS 1
/* each PP net's cause, logged once a boot (the art thread's) */
static unsigned ovl_net_logged;
/* dec-ovl.bin, as the build that made the merged image recorded it
 * (mkvideo.py; album-render.md section 5.3); a header without one (an
 * image before the overlay) names no file, so no load is ever due */
#ifndef BCMVIDEO_DECOVL_BYTES
#define BCMVIDEO_DECOVL_BYTES  0
#define BCMVIDEO_DECOVL_FNV    0
#define BCMVIDEO_DECOVL_ABI    0
#define BCMVIDEO_DECOVL_ABI_AT 0
#define BCMVIDEO_DECOVL_BASE   0
#endif
static const struct bcmjob_ovl_file dec_file =
{
    BCMVIDEO_DECOVL_BYTES, BCMVIDEO_DECOVL_FNV, BCMVIDEO_DECOVL_ABI,
    BCMVIDEO_DECOVL_ABI_AT, BCMVIDEO_DECOVL_BASE
};
/* ... and viz-ovl-m.bin, the visualizer's for the same image */
#ifndef BCMVIDEO_VIZOVL_BYTES
#define BCMVIDEO_VIZOVL_BYTES  0
#define BCMVIDEO_VIZOVL_FNV    0
#define BCMVIDEO_VIZOVL_ABI    0
#define BCMVIDEO_VIZOVL_ABI_AT 0
#define BCMVIDEO_VIZOVL_BASE   0
#endif
static const struct bcmjob_ovl_file viz_file =
{
    BCMVIDEO_VIZOVL_BYTES, BCMVIDEO_VIZOVL_FNV, BCMVIDEO_VIZOVL_ABI,
    BCMVIDEO_VIZOVL_ABI_AT, BCMVIDEO_VIZOVL_BASE
};
/* The visualizer's ask (the UI thread) and the art thread's answer: the
 * record due named (viz_req: the merged image's, or a developer build's
 * display-only one), the load owed, its refusals in an epoch */
static struct bcmjob_ovl_file viz_req;
static volatile bool viz_owed;
static volatile uint32_t viz_bad_ep;
static volatile int viz_bad_n;
/* The visualizer's view is open: it owns the region (art_bcm.h,
 * art_bcm_viz_view(); the UI thread writes it), and the art work turned
 * away meanwhile (the art thread counts, the close logs and clears) */
static volatile bool viz_view;
static volatile unsigned int viz_deferred;
/* A piece the pass took and answered BCMJOB_OVL_AGAIN (its wait posted):
 * asked again at the next passes (0: none); ovl_bg: a last piece's
 * answer still owed after its piece was withdrawn, looked for by
 * bcmjob_ovl_poll() at each pass until it comes */
static uint32_t ovl_pend_s;
static bool ovl_bg;
/* The pass's longest BCM work in this minute, and what it was */
static uint32_t pm_max;
static const char *pm_what;
static long pm_t0;
/* the panel on the BCM (art_bcm.h): on in every build since the panel
 * stays resident (the owner's rule, 2026-10-25: on the BCM once a design
 * keeps it there and BLITs it, no read-back on the UI's path); a build
 * whose image lacks DECODE decodes it on the CPU as before */
static volatile bool panel_on = true;
/* Odd while a decode is in flight (the art thread writes it) */
static volatile uint32_t busy_seq;

/* ---- the source: the art thread's fd ------------------------------------ */
struct ajs
{
    int fd;
    long off;
};

static int ajs_start(void *c)
{
    struct ajs *s = c;

    return lseek(s->fd, s->off, SEEK_SET) == s->off ? 0 : -1;
}

static long ajs_fetch(void *c, void *buf, uint32_t n)
{
    return read(((struct ajs *)c)->fd, buf, n);
}

static int ajs_skip(void *c, uint32_t n)
{
    return lseek(((struct ajs *)c)->fd, n, SEEK_CUR) < 0 ? -1 : 0;
}

/* ---- the UI thread -------------------------------------------------------- */
/* The pass's answer, as an event (budget review: no 20 ms sleep-polls):
 * the owner's pass releases it whenever it finished something the art
 * thread waits for (a piece put, a park, drop, install or FREE done); the
 * art thread waits on it in slices of PASS_SLICE, so the cancel hook is
 * still looked at that often and every wait keeps its bound.  A release
 * nobody waits for only shortens the next slice (each waiter re-checks
 * its own word).  The two ends can be on different cores (the art
 * thread on the CPU, the pass on the UI thread, the COP's in a split
 * build), so the semaphore and its flag are uncached (XCORE_BSS,
 * docs/design/p19-ui-on-cop.md section 3.2) and the flag has one writer:
 * only the waiter initialises (before it first waits; the flag is stored
 * after the semaphore's words), and the pass releases only once the flag
 * is set (a release before the first wait has nobody to wake) */
#define PASS_SLICE  (HZ / 20)
static struct semaphore pass_sem XCORE_BSS;
static volatile bool pass_sem_ready XCORE_BSS;

static void pass_sem_init(void)
{
    if (!pass_sem_ready)
    {
        semaphore_init(&pass_sem, 1, 0);
        pass_sem_ready = true;
    }
}

/* The art thread: until the pass's next answer, at most a slice and never
 * past deadline (a tick) */
static void pass_wait(long deadline)
{
    long left = deadline - current_tick;

    pass_sem_init();
    if (left > PASS_SLICE)
        left = PASS_SLICE;
    if (left > 0)
        semaphore_wait(&pass_sem, left);
}

/* The pass's clock and its longest BCM work a minute (art_bcm.h) */
uint32_t art_bcm_pass_now(void)
{
#ifdef SIMULATOR
    return (uint32_t)sim_clock_now_us();
#else
    return USEC_TIMER;
#endif
}

void art_bcm_pass_note(const char *what, uint32_t us)
{
    long now = current_tick;

    if (us >= pm_max)
    {
        pm_max = us;
        pm_what = what;
    }
    if (!pm_t0)
        pm_t0 = now ? now : 1;
    else if (TIME_AFTER(now, pm_t0 + 60 * HZ))
    {
        ui_log("bcmjob", "pass max us=%lu what=%s", (unsigned long)pm_max,
               pm_what ? pm_what : "-");
        pm_max = 0;
        pm_what = NULL;
        pm_t0 = now ? now : 1;
    }
}

/* DECODE as the merged image's overlay, at most a try an epoch (the
 * driver's tries): the art thread reads it when a decode is due on the
 * BCM (art_bcm_ready_ovl()); each piece it posts is put here, the job
 * owner's side.  Never in a slide's or glide's frames (the piece's port
 * work, the last one's BCM checks): on Now Playing's own screen any pass,
 * else at rest; a piece waits for the next pass meanwhile.  A pass never
 * waits for the BCM (sitting 4c): a piece whose FENCE or OVERLAY reply is
 * owed answers BCMJOB_OVL_AGAIN and is put again at the next passes (the
 * art thread wakes them while it waits); one the art thread withdrew is
 * answered at once and a last piece's reply still looked for
 * (bcmjob_ovl_poll(), ovl_bg).  While the visualizer's view is open a
 * DECODE load's first piece is never taken (the region is the view's).
 * true: a piece answered */
static bool dec_ovl_service(bool rest)
{
    uint32_t s, t0, us;
    int rc;

    if (bcmjob_have(J_OP_DECODE))
        ovl_due = false;
    else
    {
        struct bcmjob_ovl_file f = dec_file;

        ovl_due = bcmjob_ovl_due(BCMJOB_OVL_DEC, &f);
        if (ovl_due && ovl_bad_ep == bcmjob_epoch() && ovl_bad_n >= 2)
            ovl_due = false;        /* refused in this epoch: the PP's */
    }
    if (ovl_bg)
    {
        t0 = art_bcm_pass_now();
        rc = bcmjob_ovl_poll();
        art_bcm_pass_note("ovl-poll", art_bcm_pass_now() - t0);
        if (rc != BCMJOB_OVL_AGAIN)
        {
            ovl_bg = false;
            UPDATE_TRACE("art bcm overlay late rc=%d", rc);
        }
    }
    s = ovl_post;
    if (ovl_pend_s && s != ovl_pend_s)
    {
        /* withdrawn while its wait was owed: the buffer back now */
        ovl_bg = true;
        UPDATE_TRACE("art bcm overlay piece k=%d off=%lu n=%lu rc=%d",
                     ovl_kind, (unsigned long)ovl_off, (unsigned long)ovl_n,
                     BCMJOB_OFF);
        ovl_rc = BCMJOB_OFF;
        ovl_done = ovl_pend_s;
        ovl_pend_s = 0;
        return true;
    }
    if (!s || s == ovl_done)
        return false;
    if (!rest && !ui_strip_owned_by(UI_STRIP_OWNER_NP))
        return false;
    if (viz_view && s != ovl_pend_s && ovl_kind == BCMJOB_OVL_DEC
        && ovl_off == 0)
        return false;               /* the view's region: the art thread's
                                       cancel withdraws it */
    ovl_take = s;                   /* before the second look (above) */
    if (ovl_post != s)
        rc = BCMJOB_OFF;            /* withdrawn meanwhile: untouched */
    else
    {
#if NUM_CORES > 1
        if (ovl_core != CURRENT_CORE)
            xcore_acquire();        /* the reader's piece, not our lines */
#endif
        t0 = art_bcm_pass_now();
        rc = bcmjob_ovl_put(ovl_kind, ovl_buf, ovl_off, ovl_n, ovl_size,
                            ovl_fp);
        us = art_bcm_pass_now() - t0;
        art_bcm_pass_note(ovl_kind == BCMJOB_OVL_DEC ? "ovl-dec" : "ovl-viz",
                          us);
        UPDATE_TRACE("art bcm overlay pass k=%d off=%lu us=%lu rc=%d",
                     ovl_kind, (unsigned long)ovl_off, (unsigned long)us, rc);
        if (rc == BCMJOB_OVL_AGAIN)
        {
            ovl_pend_s = s;         /* again at the next pass */
            return false;
        }
        /* the look the art thread reads once its piece is done */
        have_decode = bcmjob_have(J_OP_DECODE);
        lends = have_decode && bcmjob_dec_lends();
    }
    ovl_pend_s = 0;
    UPDATE_TRACE("art bcm overlay piece k=%d off=%lu n=%lu rc=%d",
                 ovl_kind, (unsigned long)ovl_off, (unsigned long)ovl_n, rc);
    ovl_rc = rc;
    ovl_done = s;
    return true;
}

void art_bcm_service(bool rest)
{
    bool answered = dec_ovl_service(rest);
    uint32_t t0 = art_bcm_pass_now();
    const char *what = NULL;

    have_decode = bcmjob_have(J_OP_DECODE);
    lends = have_decode && bcmjob_dec_lends();
#ifdef HAVE_DRIFT_JOBS
    if (park_owed)
    {
        dj_cover_park();
        park_owed = false;
        answered = true;
        what = "park";
    }
    if (drop_owed)
    {
        dj_resident_drop();
        drop_owed = false;
        answered = true;
        what = "drop";
    }
    if (inst_owed)
    {
        dj_resident_set(inst_key, inst_slot, inst_epoch);
        inst_owed = false;
        res_seq++;
        answered = true;
        what = "resident";
    }
    pub_key = dj_resident_key();
    pub_epoch = bcmjob_epoch();
#endif
    if (free_owed)
    {
        uint32_t p[1];

        /* a slot of an earlier epoch is gone already (and its id may be
         * another's now): only this epoch's is freed */
        p[0] = free_slot;
        if (free_epoch == bcmjob_epoch())
            bcmjob_submit(J_OP_FREE, 0, p, 1);
        free_owed = false;
        answered = true;
        what = what ? what : "free";
    }
    if (what)
        art_bcm_pass_note(what, art_bcm_pass_now() - t0);
    if (answered && pass_sem_ready)     /* the waiter's init (above) */
        semaphore_release(&pass_sem);   /* the art thread's wait ends */
}

void art_bcm_panel_set(bool on)
{
    panel_on = on;
}

bool art_bcm_panel_on(void)
{
    return panel_on;
}

bool art_bcm_busy(void)
{
    return busy_seq & 1;
}

bool art_bcm_lends(void)
{
    return lends;
}

bool art_bcm_ready(void)
{
    /* the Now Playing row's still frame (dj_still()) sends nothing more
     * while it stands: a decode may run behind it */
    return have_decode && bcmjob_power_gen() != 0
#ifdef HAVE_DRIFT_JOBS
           && (!dj_owned() || dj_still())
#endif
           && !mqj_active();
}

/* The PP's net instead of the chip's decode, once a boot per cause */
static void ovl_net(int why)
{
    static const char *const name[OVL_NET_N] = {
        "ok", "wait", "refused", "gone", "buf", "busy"
    };

    if (why <= OVL_OK || why >= OVL_NET_N || (ovl_net_logged & (1u << why)))
        return;
    ovl_net_logged |= 1u << why;
    ui_log("art", "FAULT bcm decode overlay why=%s: the PP decodes", name[why]);
}

/* The art thread: the kind's file (f: its record) read and posted a
 * piece at a time, each put by the owner's pass, within OVL_WAIT in all.
 * OVL_OK, an OVL_NET_* cause, or OVL_CANCELLED; *pieces: those the pass
 * put */
static int ovl_load(int kind, const struct bcmjob_ovl_file *f,
                    bool (*cancel)(void *ctx), void *ctx, long t0, int *pieces)
{
    static bool leak_said;
    uint32_t off = 0, size = 0, s;
    int h, n, res = OVL_NET_WAIT;
    bool leak = false;
    void *buf;

    *pieces = 0;
    h = core_alloc(BCMJOB_DEC_PIECE + XCORE_LINE);
    if (h <= 0)
        return OVL_NET_BUF;
    core_pin(h);                    /* the pass reads it across yields */
    buf = (void *)ALIGN_UP((uintptr_t)core_get_data(h), XCORE_LINE);
    for (;;)
    {
        if (kind == BCMJOB_OVL_DEC && viz_view)
        {
            res = OVL_CANCELLED;    /* the visualizer's region now */
            break;
        }
        n = bcmjob_ovl_read(kind, buf, off, BCMJOB_DEC_PIECE, &size, f);
        if (n <= 0)
        {
            res = OVL_NET_REFUSED;  /* no file, a short read: logged */
            break;
        }
#if NUM_CORES > 1
        xcore_publish();            /* the piece, for the other core */
#endif
        s = ++ovl_seq;
        if (!s)
            s = ++ovl_seq;
        ovl_buf = buf;
        ovl_off = off;
        ovl_n = n;
        ovl_size = size;
        ovl_core = CURRENT_CORE;
        ovl_kind = kind;
        ovl_fp = f;
        ovl_post = s;               /* after its fields */
        np_pb_ab_wake();            /* the pass now, not at its timeout */
        while (ovl_done != s && TIME_BEFORE(current_tick, t0 + OVL_WAIT))
        {
            if (cancel && cancel(ctx))
            {
                res = OVL_CANCELLED;
                break;
            }
            if (ovl_take == s)
            {
                /* taken, its wait on the BCM posted (BCMJOB_OVL_AGAIN):
                   the pass asks again soon, never waiting itself */
                np_pb_ab_wake();
                pass_wait(MIN(t0 + OVL_WAIT, current_tick + OVL_AGAIN_TICKS));
            }
            else
                pass_wait(t0 + OVL_WAIT);
        }
        if (ovl_done != s)
        {
            long t1 = current_tick;

            ovl_post = 0;           /* withdrawn, before the look at take */
            if (ovl_take == s)
            {
                /* the pass has it: wait for its bounded end (a posted
                   wait's piece is answered at its next pass) */
                while (ovl_done != s
                       && TIME_BEFORE(current_tick, t1 + OVL_TAKEN_WAIT))
                {
                    np_pb_ab_wake();
                    pass_wait(MIN(t1 + OVL_TAKEN_WAIT,
                                  current_tick + OVL_AGAIN_TICKS));
                }
                leak = ovl_done != s;
            }
            if (leak && !leak_said)
            {
                leak_said = true;
                ui_log("art", "FAULT art_bcm_ready_ovl wait=pass-piece "
                       "ms=%ld: its buffer kept",
                       (long)(current_tick - t0) * (1000 / HZ));
            }
            break;
        }
        (*pieces)++;
        if (ovl_rc != BCMJOB_OK)
        {
            /* none due at the first piece; a DECODE holding the BCM
               (no try spent); else refused (logged) */
            res = ovl_rc == BCMJOB_NOJOB && off == 0 ? OVL_NET_GONE
                  : ovl_rc == BCMJOB_BUSY ? OVL_NET_BUSY : OVL_NET_REFUSED;
            break;
        }
        off += n;
        if (off >= size)
        {
            res = OVL_OK;
            break;
        }
    }
    if (!leak)
    {
        core_unpin(h);
        core_free(h);
    }
    return res;
}

bool art_bcm_ready_ovl(bool (*cancel)(void *ctx), void *ctx)
{
    long t0 = current_tick;
    int res, pieces;

    if (viz_view)
        return false;               /* the visualizer's: the caller defers
                                       (artcache.c bcm_gate()) */
    if (have_decode)
        return art_bcm_ready();
    if (!ovl_due)
    {
        /* refused in this epoch already (an earlier ask): the PP's, said
         * once */
        if (ovl_bad_ep && ovl_bad_ep == bcmjob_epoch())
            ovl_net(OVL_NET_REFUSED);
        return false;               /* no DECODE: art_bcm_ready()'s too */
    }
    res = ovl_load(BCMJOB_OVL_DEC, &dec_file, cancel, ctx, t0, &pieces);
    if (res == OVL_CANCELLED)
        return false;               /* the user came back: the caller's */
    UPDATE_TRACE("art bcm overlay load by=art pieces=%d ok=%d", pieces,
                 (int)(res == OVL_OK));
    if (res == OVL_NET_REFUSED)
    {
        if (ovl_bad_ep != bcmjob_epoch())
            ovl_bad_n = 0;
        ovl_bad_ep = bcmjob_epoch();
        ovl_bad_n++;
        ui_log("art", "bcm decode overlay not loaded (the CPU decodes)");
    }
    UPDATE_TRACE("art bcm overlay wait ms=%ld decode=%d res=%d",
                 (long)(current_tick - t0) * (1000 / HZ), (int)have_decode,
                 res);
    if (have_decode)
        return art_bcm_ready();
    /* still due: loaded at the next ask, for the next decode */
    ovl_net(res == OVL_OK ? OVL_NET_GONE : res);
    return false;
}

/* ---- the visualizer's overlay (visualizer.md 8.5) ------------------------ */
void art_bcm_viz_view(bool open)
{
    unsigned int n;

    if (open == viz_view)
        return;
    viz_view = open;
    UPDATE_TRACE("art bcm viz view open=%d", (int)open);
    if (open)
        return;
    n = viz_deferred;
    viz_deferred = 0;
    if (n)
        ui_log("art", "chip work paused: visualizer open (%u deferred)", n);
    artcache_aux_wake();            /* the paused art work resumes */
}

bool art_bcm_viz_holds(void)
{
    /* the BCM would take the art's decodes: DECODE offered, or its
     * overlay due (evicted by the view's); else they are the PP's anyway */
    return viz_view && (have_decode || ovl_due);
}

void art_bcm_viz_defer(const char *what)
{
    (void)what;                     /* (the trace only) */
    if (!viz_deferred)
        UPDATE_TRACE("art bcm viz defer what=%s", what);
    viz_deferred++;
}

int art_bcm_viz_ask(void)
{
    struct bcmjob_ovl_file f = viz_file;

    if (bcmjob_have(J_OP_SPECTRUM) && bcmjob_have(J_OP_BARS))
        return ART_VIZ_LOADED;
    if (viz_owed)
        return ART_VIZ_WAIT;            /* the art thread has it */
    if (viz_bad_ep && viz_bad_ep == bcmjob_epoch() && viz_bad_n >= 2)
        return ART_VIZ_NONE;            /* refused in this epoch */
    if (!bcmjob_ovl_due(BCMJOB_OVL_VIZ, &f))
        return ART_VIZ_NONE;            /* no region for it, tries spent */
    viz_req = f;
    viz_owed = true;                    /* after its record */
    UPDATE_TRACE("art bcm viz ask base=%08lx bytes=%lu",
                 (unsigned long)f.base, (unsigned long)f.bytes);
    artcache_aux_wake();
    return ART_VIZ_WAIT;
}

void art_bcm_viz_serve(void)
{
    long t0 = current_tick;
    int res, pieces;

    if (!viz_owed)
        return;
    res = ovl_load(BCMJOB_OVL_VIZ, &viz_req, NULL, NULL, t0, &pieces);
    UPDATE_TRACE("art bcm viz load pieces=%d res=%d ms=%ld", pieces, res,
                 (long)(current_tick - t0) * (1000 / HZ));
    if (res == OVL_NET_REFUSED)
    {
        if (viz_bad_ep != bcmjob_epoch())
            viz_bad_n = 0;
        viz_bad_ep = bcmjob_epoch();
        viz_bad_n++;
        ui_log("viz", "FAULT bcm viz overlay not loaded (the driver says why)");
    }
    viz_owed = false;                   /* the next ask may post again */
}

uint64_t art_bcm_resident_key(void)
{
    return pub_epoch == bcmjob_epoch() ? pub_key : 0;
}

bool art_bcm_resident_wanted(uint64_t key)
{
    /* or due as the merged image's overlay: the art thread asks for it
     * before the decode (art_bcm_ready_ovl()) */
    return key && (have_decode || ovl_due) && panel_on
           && art_bcm_resident_key() != key;
}

int art_bcm_resident_seq(void)
{
    return res_seq;
}

void art_bcm_np_shown(bool shown)
{
    np_shown = shown;
}

bool art_bcm_np_is_shown(void)
{
    /* lit and not the unlit face: the preview's flag is only refreshed
     * while it ticks (lit) */
    return np_shown && is_backlight_on(false) && !ui_unlit_active();
}

/* The owner's pass does it: wait for its answer, the cancel hook looked
 * at each slice (FREE_WAIT at most; false: not done, the caller takes its
 * CPU path) */
static bool owner_did(volatile bool *owed, bool (*cancel)(void *ctx),
                      void *ctx)
{
    long t0 = current_tick;

    while (*owed && TIME_BEFORE(current_tick, t0 + FREE_WAIT))
    {
        if (cancel && cancel(ctx))
            return false;
        pass_wait(t0 + FREE_WAIT);
    }
    return !*owed;
}

/* The deadline (section 8's estimate, 4 M + 36 M instructions a source
 * Mpx at 27 MHz, doubled, plus a second): a big cover's own, never the
 * driver's 3 s cap (bcmjob.h) */
static uint32_t deadline_ms(uint32_t len, int sw, int sh)
{
    uint32_t px_k = (uint32_t)sw * (uint32_t)sh / 1000;
    uint32_t est = 148 + px_k / 3 * 4;
    uint32_t ms = 2 * est + 1000, floor = BCMJOB_T_DECODE(len, 27);

    if (ms < floor)
        ms = floor;
    return ms > DEADLINE_MAX ? DEADLINE_MAX : ms;
}

enum art_bcm_verdict art_bcm_decode(int fd, long off, uint32_t len,
                                    int sw, int sh,
                                    const struct art_bcm_out *o,
                                    bool (*cancel)(void *ctx), void *ctx,
                                    bool *read_err)
{
    struct ajs a = { fd, off };
    struct jdec_src src = { len, ajs_start, ajs_fetch, ajs_skip, &a };
    struct bcmjob_dec_out d;
    struct bcmjob_dec_req q;
    struct bcmjob_dec_res res;
    uint64_t key = o->key ^ ((o->kind == ART_BCM_PANEL ? TAG_PANEL
                                                       : TAG_BOX) << 56);
    long t0 = current_tick;
    int r, v;

    dec_thread = thread_self();     /* the art thread: the reader */
    *read_err = false;
    if (ab.held)
        art_bcm_release();          /* a caller that forgot: never two */
    /* the last resident's install first (pub_key is the owner's) */
    if (!owner_did(&inst_owed, cancel, ctx))
        return cancel && cancel(ctx) ? ART_BCM_DEFER : ART_BCM_TWIN;
    /* the last decode's slot freed first (the UI thread's pass) */
    while (free_owed && TIME_BEFORE(current_tick, t0 + FREE_WAIT))
    {
        if (cancel && cancel(ctx))
            return ART_BCM_DEFER;
        pass_wait(t0 + FREE_WAIT);
    }
    if (free_owed)
    {
        UPDATE_TRACE("art bcm skip why=%s", "free");
        return ART_BCM_TWIN;
    }
    /* a panel of another album: the old resident leaves the pool first
     * (the drift's cover and the resident fill it); the same album's is a
     * hit (ALLOC: its key, size and class), nothing decoded */
    if (o->kind == ART_BCM_PANEL && art_bcm_resident_key()
        && art_bcm_resident_key() != o->key)
    {
        drop_owed = true;
        if (!owner_did(&drop_owed, cancel, ctx))
        {
            if (cancel && cancel(ctx))
                return ART_BCM_DEFER;
            UPDATE_TRACE("art bcm skip why=%s", "drop");
            return ART_BCM_TWIN;
        }
    }
#ifdef HAVE_DRIFT_JOBS
    /* the merged image's overlay borrows a J_CLS_DRIFT slot for its work
     * area: the drift's own cover leaves the pool first (it is uploaded
     * again when the drift next needs it; the resident panel stays) */
    if (lends)
    {
        park_owed = true;
        if (!owner_did(&park_owed, cancel, ctx))
        {
            if (cancel && cancel(ctx))
                return ART_BCM_DEFER;
            UPDATE_TRACE("art bcm skip why=%s", "park");
            return ART_BCM_TWIN;
        }
    }
#endif
    memset(&d, 0, sizeof(d));
    d.dw = o->dw;
    d.dh = o->dh;
    d.cx = o->cx;
    d.cy = o->cy;
    d.cw = o->cw;
    d.ch = o->ch;
    d.cls = o->kind == ART_BCM_PANEL ? J_CLS_DRIFT : J_CLS_THUMB;
    d.key_lo = (uint32_t)key;
    d.key_hi = (uint32_t)(key >> 32);
    /* over the work area (a fill wider than about 980 x 300): the CPU at
     * once, no traffic (the driver would refuse it unsent too) */
    if (bcmjob_dec_work(&d, 1) > J_DEC_WORK_MIN)
    {
        UPDATE_TRACE("art bcm skip why=%s dw=%d", "work", o->dw);
        return ART_BCM_TWIN;
    }
    /* the UI's job clients: none while this runs (art_bcm.h) */
    busy_seq++;
    if (!art_bcm_ready() && o->kind == ART_BCM_PANEL
        && art_bcm_resident_key() == o->key)
    {
        /* its pixels are on the BCM already (the resident): only the
         * read-back is left, which waits for the UI's job to end, never
         * the CPU's decode of the same panel */
        busy_seq++;
        UPDATE_TRACE("art bcm defer why=%s", "ui");
        return ART_BCM_DEFER;
    }
    if (!art_bcm_ready())
    {
        busy_seq++;
        UPDATE_TRACE("art bcm skip why=%s", "ui");
        return ART_BCM_TWIN;
    }
    memset(&q, 0, sizeof(q));
    q.src = &src;
    q.out = &d;
    q.n = 1;                        /* the builder's single decode */
    q.flags = 0;                    /* lit only, always (bcmjob.h) */
    q.ms = deadline_ms(len, sw, sh);
    q.cancel = cancel;
    q.ctx = ctx;
    r = bcmjob_decode(&q, &res);
    busy_seq++;
    v = bcmjob_dec_verdict(r, &res);
    UPDATE_TRACE("art bcm decode key=%08lx%08lx dw=%d dh=%d cw=%d ch=%d "
                 "rc=%d v=%d ms=%ld", (unsigned long)(o->key >> 32),
                 (unsigned long)(uint32_t)o->key, o->dw, o->dh, o->cw,
                 o->ch, r, v, (long)(current_tick - t0) * (1000 / HZ));
    if (v == JDEC_OK)
    {
        ab.slot = res.out[0].slot;
        ab.epoch = res.epoch;
        ab.a = res.out[0].a;
        ab.b = res.out[0].b;
        ab.cw = o->cw;
        ab.ch = o->ch;
        ab.key = o->key;
        ab.panel = o->kind == ART_BCM_PANEL;
        ab.resident = false;
        ab.sa = ab.sb = 0;
        ab.held = true;
        return ART_BCM_OK;
    }
    if (v == JDEC_E_CORRUPT || v == JDEC_E_TRUNCATED)
    {
        *read_err = res.read_err;
        return ART_BCM_BAD;
    }
    if (v == BCMJOB_DEC_DEFER)
        return ART_BCM_DEFER;
    if (r != BCMJOB_BUSY)
        ui_log("art", "bcm decode twin rc=%d err=%lu", r,
               (unsigned long)res.err);
    return ART_BCM_TWIN;
}

bool art_bcm_read(int y, int h, uint16_t *px, int pitch)
{
    int k;

    /* the proof (album-render.md section 14): no read-back on the UI's
     * path.  Only the decoding thread reads, never while the Now Playing
     * row shows the panel; either line is a bug (the gate's scripts fail
     * on it, bench_ingest flags it) */
    if (thread_self() != dec_thread)
        ui_log("art", "FAULT art bcm read off thread rows=%d", h);
    if (art_bcm_np_is_shown())
        ui_log("art", "FAULT art bcm read while shown rows=%d", h);
    if (!ab.held || bcmjob_read(ab.slot, 0, y, ab.cw, h, px, pitch)
                    != BCMJOB_OK)
        return false;
    for (k = 0; k < h; k++)
        j_f16w(&ab.sa, &ab.sb, px + k * pitch, ab.cw);
    return true;
}

bool art_bcm_sums_ok(void)
{
    return ab.held && ab.sa == ab.a && ab.sb == ab.b;
}

void art_bcm_rewind(void)
{
    ab.sa = ab.sb = 0;
}

void art_bcm_release(void)
{
    if (!ab.held)
        return;
    ab.held = false;
    if (ab.resident)
        return;                     /* the pool's resident: kept */
    free_slot = ab.slot;
    free_epoch = ab.epoch;
    free_owed = true;
}

void art_bcm_keep(void)
{
    if (!ab.held || !ab.panel || ab.resident)
        return;
    if (ab.cw != NAV_DRIFT_SRC || ab.ch != NAV_DRIFT_SRC)
    {
        /* portrait: the PP adds its bars; the drift's frames need the
         * whole 300 x 300 (read back, freed, as before) */
        UPDATE_TRACE("art bcm resident skip why=%s cw=%d", "portrait",
                     (int)ab.cw);
        return;
    }
    ab.resident = true;
    if (pub_key == ab.key && pub_epoch == ab.epoch)
        return;                     /* a hit: installed already */
    inst_key = ab.key;
    inst_slot = ab.slot;
    inst_epoch = ab.epoch;
    inst_owed = true;
    UPDATE_TRACE("art bcm resident key=%08lx%08lx slot=%lu",
                 (unsigned long)(ab.key >> 32),
                 (unsigned long)(uint32_t)ab.key, (unsigned long)ab.slot);
}
#endif /* HAVE_ART_BCM */
