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

#include "config.h"
#include "ipodui/drift_jobs.h"

#ifdef HAVE_DRIFT_JOBS
#include <string.h>
#include "lcd.h"
#include "kernel.h"
#include "bcmjob_pp.h"
#include "bcmjob.h"
#include "ipodui/anim.h"            /* UPDATE_TRACE */
#include "ipodui/art_bcm.h"
#include "ipodui/material.h"
#include "ipodui/nav_core.h"
#include "ipodui/preview.h"
#include "ipodui/uilog.h"
#ifdef HAVE_BCMFW_DEVTEST
#include "ipodui/devbcm.h"
#endif

/* The cover's size in its block (preview.c's slot) */
#define DJ_SRC      NAV_DRIFT_SRC
#define DJ_BYTES    ((uint32_t)DJ_SRC * DJ_SRC * 2)
/* one frame's jobs retire within this (jobs.md section 6: a FENCE, its
 * draws and the PRESENT) */
#define DJ_T_FRAME  (J_T_FENCE_BASE + 3 * J_T_DRAW + J_T_PRESENT)
/* a BCM that answers OFF this many frames in a row while lit has failed */
#define DJ_OFF_MAX  3
/* a FENCE this late (a cold start is 0.37-0.62 s on silicon) is a slow
 * job: the CPU's drift for the boot */
#define DJ_LATE_MAX (HZ * 2)

static struct
{
    struct bcmjob_slot slot;
    uint64_t key;               /* the cover in .slot (0: none) */
    uint32_t epoch;             /* .slot's and the owned rect's */
    uint32_t fence;             /* the last frame's FENCE, 0 none */
    uint32_t errs;              /* bcmjob_errors() before this frame's jobs */
    uint32_t errs_ep;           /* ... read in this epoch */
    long late;                  /* tick its FENCE first ran late, 0 not */
    long down;                  /* tick jobs first went unavailable, 0 */
    long down_seen;             /* ... last seen so */
    uint32_t fence_ep;          /* the epoch it was sent in */
    uint32_t own_ep;            /* the epoch the panel was last taken in */
    const char *still;          /* why the still cover, NULL: not */
    const char *cpu;            /* why the CPU's drift for the boot, NULL:
                                   jobs (a failure never stills it) */
    const char *last_op;        /* the last frame's drawing job */
    bool owned;                 /* the BCM holds the panel's pixels */
    bool have;                  /* .slot is valid */
    bool borrowed;              /* .slot is the resident panel's (rp): never
                                   freed here */
    bool rstill;                /* the last frame: the row's still one */
    unsigned char offs;         /* OFF answers in a row */
} dj;

/* The resident panel (drift_jobs.h): the BCM's own decode of the playing
 * album's cover, READY in the J_CLS_DRIFT pool beside the drift's cover */
static struct
{
    uint64_t key;               /* the album (the panel set's key) */
    uint32_t slot, epoch;
    bool valid;
} rp;

static const char *const mode_name[BCMJOB_DRIFT_MODES] = {
    "auto", "on", "off", "still", "parity"
};

#ifdef HAVE_BCM_JOBS_MODEL
static void poison(void);
#else
#define poison() do { } while (0)
#endif

#ifdef HAVE_IPOD_UI_DEVMODE
static void go_still(const char *why)
{
    if (dj.still)
        return;
    dj.still = why;
    ui_log("panel", "drift still why=%s", why);
}
#endif

/* The last resort, never the plan (owner, 2026-10-25: the drift belongs
 * on the video chip; a chip failure is a bug to fix there).  On a BCM
 * failure (missing op, slow or failed job, BCM gone) the drift keeps
 * moving on the CPU for the rest of the boot, the same frames as with the
 * jobs off (pv.slot is PP RAM, Q13), instead of freezing.  It says so
 * loudly in the log, so it never hides a chip failure (bench_ingest
 * flags it).  Never retried, so a failing BCM costs no more jobs; the
 * developer row resets it. */
static void go_cpu(const char *why)
{
    if (dj.cpu)
        return;
    dj.cpu = why;
    ui_log("panel", "drift cpu why=%s", why);
    ui_log("panel", "FALLBACK drift left the video chip (%s): last resort, "
           "a chip bug to fix", why);
}

/* The developer switch (also the Now Playing marquee's, mq_jobs.c) */
bool dj_jobs_wanted(void)
{
    switch (bcmjob_drift_mode())
    {
        case BCMJOB_DRIFT_ON:
        case BCMJOB_DRIFT_PARITY:
            return true;
        case BCMJOB_DRIFT_AUTO:
#ifdef HAVE_BCM_JOBS_MODEL
            return false;       /* the model stands for an unverified image */
#else
            /* the driver allows jobs this boot (verified.txt or the
             * developer marker): FENCE always exists when it does */
            return bcmjob_have(J_OP_FENCE);
#endif
        default:
            return false;
    }
}

enum dj_path dj_path(void)
{
#ifdef HAVE_IPOD_UI_DEVMODE
    /* the developer row's "still" (a sealed build has no such row) */
    if (bcmjob_drift_mode() == BCMJOB_DRIFT_STILL)
    {
        go_still("developer");
        return DJ_PATH_STILL;
    }
    if (dj.still && !strcmp(dj.still, "developer"))
        dj.still = NULL;        /* the row moved off "still" */
#endif
    if (!dj_jobs_wanted() || dj.cpu)
        return DJ_PATH_CPU;
    if (dj.still)
        return DJ_PATH_STILL;
    /* P19 19-6: a cover decoding on the BCM holds its ring (art_bcm.h):
     * the CPU's frames until it ends, never a job queued behind it (a
     * late FENCE would make the drift still for the boot); the decode
     * starts only while the drift doesn't own the panel */
    if ((!dj.owned || dj.rstill) && art_bcm_busy())
        return DJ_PATH_CPU;     /* (the row's still frame let it start) */
    if (!bcmjob_have(J_OP_FENCE))
    {
        /* the BCM isn't taking jobs: starting after a cut (OFF until its
         * HELLO), or gone.  The CPU's frames meanwhile; down past
         * DJ_LATE_MAX, a BCM failure: the CPU's drift for the boot */
        if (!dj.down || TIME_AFTER(current_tick, dj.down_seen + HZ / 2))
            dj.down = current_tick;     /* a new stretch (not across a
                                           dark spell: no ticks then) */
        dj.down_seen = current_tick;
        if (!TIME_BEFORE(current_tick, dj.down + DJ_LATE_MAX))
            go_cpu("off");
        return DJ_PATH_CPU;
    }
    dj.down = 0;
    if (!bcmjob_have(J_OP_BLIT) || !bcmjob_have(J_OP_AVG)
        || !bcmjob_have(J_OP_FADE) || !bcmjob_have(J_OP_SHADE)
        || !bcmjob_have(J_OP_PRESENT) || !bcmjob_have(J_OP_ALLOC))
    {
        /* jobs on, an op missing (J_OPS) or marked slow/failed */
        go_cpu("noop");
        return DJ_PATH_CPU;
    }
    return DJ_PATH_JOBS;
}

/* A job's answer: true sent.  A failure ends the jobs for this boot (the
 * CPU's drift); BUSY drops to the CPU for this frame; OFF does too, until
 * it repeats (a BCM failure). */
static bool sent(int r, const char *what)
{
    if (r == BCMJOB_OK)
    {
        dj.offs = 0;
        return true;
    }
    if (r == BCMJOB_BUSY)
        return false;
    if (r == BCMJOB_OFF && ++dj.offs < DJ_OFF_MAX)
        return false;
    ui_log("panel", "drift job fault at=%s rc=%d", what, r);
    go_cpu(r == BCMJOB_OFF ? "off" : r == BCMJOB_TIMEOUT ? "slow"
           : r == BCMJOB_NOJOB ? "noop" : "failed");
    return false;
}

/* Before a frame: the last frame retired (a slow job shows here), the
 * cover in its slot this epoch, and the panel rect owned */
/* The last frame's FENCE retired (at most DJ_T_FRAME: a slow job), and
 * none of its jobs failed (they retire the FENCE all the same) */
static bool frame_retired(uint32_t ms)
{
    uint32_t e;
    int r;

    if (!dj.fence)
        return true;
    if (bcmjob_epoch() != dj.fence_ep)
    {
        /* the BCM restarted since: that frame's jobs are gone with the
         * old epoch (its FENCE never retires); the CPU's frame, then
         * jobs again from a new upload */
        dj.fence = 0;
        dj.late = 0;
        return false;
    }
    r = bcmjob_wait(dj.fence, ms);
    if (r == BCMJOB_TIMEOUT)
    {
        /* late: a BCM still starting after a cut (C1), or a slow job.
         * The CPU draws meanwhile; past DJ_LATE_MAX it is slow for the
         * boot (jobs.md section 6) */
        if (!dj.late)
        {
            dj.late = current_tick;
            ui_log("panel", "drift job late at=%s",
                   dj.last_op ? dj.last_op : "?");
        }
        if (TIME_BEFORE(current_tick, dj.late + DJ_LATE_MAX))
            return false;
    }
    dj.late = 0;
    dj.fence = 0;
    if (!sent(r, "wait"))
        return false;
    /* J_ERRORS against the baseline taken before this frame's jobs, in
     * the same epoch (bcmjob.h): a failure from before the frame (the
     * bench's self-tests, another client between frames) is never this
     * frame's.  L1: J_ERRORS is the BCM's, not per client, so another
     * client's failure while this frame ran still counts here (the CPU's
     * drift: it moves all the same); the driver's per-op marks
     * (bcmjob_have(), dj_path()) are the lasting signal */
    e = bcmjob_errors();
    if (bcmjob_epoch() == dj.errs_ep && e - dj.errs != 0
        && e - dj.errs < 0x80000000u)
    {
        ui_log("panel", "drift job fault at=%s errors=%lu",
               dj.last_op ? dj.last_op : "?", (unsigned long)(e - dj.errs));
        go_cpu("failed");
        return false;
    }
    return true;
}

static bool frame_begin(int h, uint64_t key)
{
    uint32_t ep = bcmjob_epoch();

    /* late: only a look, the CPU draws this frame if it still is */
    if (!frame_retired(dj.late ? 0 : DJ_T_FRAME))
        return false;
    /* this frame's baseline: its upload, resync and draws to come */
    dj.errs = bcmjob_errors();
    dj.errs_ep = ep;
    if (dj.have && dj.epoch != ep)
    {
        /* the BCM restarted: slots and its front frame are gone */
        dj.have = false;
        if (dj.owned)
            return false;       /* preview.c settles, then jobs again */
    }
    if (!key)
        return false;           /* an unnamed cover: the CPU frame */
    if (dj.have && dj.borrowed && !dj_resident(dj.key))
    {
        /* the resident it named was dropped or replaced (its FREE runs
         * after the frames that read it): never named again */
        dj.have = false;
        dj.borrowed = false;
    }
    if (dj_resident(key) && (!dj.have || dj.slot.slot != rp.slot))
    {
        uint32_t p[1];

        /* the album's panel is on the BCM already (its own decode): the
         * slot by key, no upload (album-render.md section 14, the end
         * state); the drift's own slot goes back to the pool */
        if (dj.have && !dj.borrowed)
        {
            p[0] = dj.slot.slot;
            if (!sent(bcmjob_submit(J_OP_FREE, 0, p, 1), "free"))
                return false;
        }
        dj.slot.slot = rp.slot;
        dj.slot.epoch = rp.epoch;
        dj.slot.hit = true;
        dj.have = true;
        dj.borrowed = true;
        dj.key = key;
        dj.epoch = rp.epoch;
        UPDATE_TRACE("drift resident key=%08lx%08lx slot=%lu",
                     (unsigned long)(key >> 32), (unsigned long)(uint32_t)key,
                     (unsigned long)rp.slot);
    }
    if (!dj.have || dj.key != key)
    {
        uint32_t p[1];

        if (dj.have && dj.borrowed)
        {
            dj.have = false;        /* the resident stays: not the drift's */
            dj.borrowed = false;
        }
        if (h <= 0)
            return false;           /* no PP copy (the resident went): the
                                       caller's CPU picture */
#ifdef HAVE_ART_BCM
        /* a decode on the merged image's overlay is setting up or running:
         * its output and its lent work slot need the pool's room, so no
         * new cover slot now; the CPU's frame, jobs again after */
        if (art_bcm_busy() && art_bcm_lends())
            return false;
#endif
        if (dj.have)
        {
            /* one cover slot at a time: a FADE blends from the front */
            p[0] = dj.slot.slot;
            if (!sent(bcmjob_submit(J_OP_FREE, 0, p, 1), "free"))
                return false;
            dj.have = false;
        }
        if (!sent(bcmjob_slot(J_CLS_DRIFT, DJ_SRC, DJ_SRC, (uint32_t)key,
                              (uint32_t)(key >> 32), &dj.slot), "alloc"))
            return false;
        /* straight from pv.slot, pinned for the whole upload (D-4); the
         * slot's rows land at its pitch (J_DRIFT_PITCH 304: 608 B, the
         * PP's are 600 B) */
        {
#ifndef SIMULATOR
            unsigned long t0 = USEC_TIMER;
#endif
            int r = bcmjob_upload_handle(&dj.slot, h, DJ_BYTES, 0, DJ_BYTES);

#ifndef SIMULATOR
            /* engine 2 for the 300 rows, ~16 ms at 11 MB/s with the
             * core ~97 % free [silicon S37]: bus-bound, so no boost; the
             * bench reads this line (L4) */
            ui_log("panel", "drift upload us=%lu rc=%d",
                   (unsigned long)(USEC_TIMER - t0), r);
#endif
            if (!sent(r, "upload"))
            {
                /* ALLOC took the slot: give it back (M3) */
                p[0] = dj.slot.slot;
                bcmjob_submit(J_OP_FREE, 0, p, 1);
                return false;
            }
        }
        dj.have = true;
        dj.borrowed = false;
        dj.key = key;
        dj.epoch = dj.slot.epoch;
    }
    if (!dj.owned)
    {
        /* the front frame's panel rect = the framebuffer's (a FADE reads
         * it), then the BCM's */
        if (!sent(bcmjob_resync(UI_PANEL_X, 0, UI_PANEL_W, UI_PANEL_H),
                  "resync"))
            return false;
        dj.owned = true;
        if (dj.own_ep && dj.own_ep != ep)
            ui_log("panel", "drift jobs resume epoch=%lu", (unsigned long)ep);
        dj.own_ep = ep;
        bcmjob_owned_set(UI_PANEL_X, 0, UI_PANEL_W, UI_PANEL_H);
    }
    return true;
}

static void put_alpha(uint32_t *w)
{
    int c;

    memset(w, 0, ((UI_SHADOW_W + 3) / 4) * 4);
    for (c = 0; c < UI_SHADOW_W; c++)
        w[c / 4] |= (uint32_t)ui_shadow_a[c] << (8 * (c & 3));
}

/* the split shadow, PRESENT (J_PRES_WAIT: the next frame's draws come
 * after the transfer), FENCE */
static bool frame_end(bool shade)
{
    uint32_t p[3 + (UI_SHADOW_W + 3) / 4];

    p[0] = J_SURF_FRONT;
    p[1] = J_XY(UI_PANEL_X, 0);
    if (shade)
    {
        p[2] = J_XY(UI_SHADOW_W, UI_PANEL_H);
        put_alpha(&p[3]);
        if (!sent(bcmjob_submit(J_OP_SHADE, 0, p,
                                3 + (UI_SHADOW_W + 3) / 4), "shade"))
            return false;
    }
    p[2] = J_XY(UI_PANEL_W, UI_PANEL_H);
    p[3] = J_PRES_WAIT;
    if (!sent(bcmjob_submit(J_OP_PRESENT, 0, p, 4), "present"))
        return false;
    if (!sent(bcmjob_fence(&dj.fence), "fence"))
        return false;
    dj.fence_ep = bcmjob_epoch();
    poison();
#ifdef HAVE_BCM_JOBS_MODEL
    /* the simulator: the frame on the model's panel now, so a shot sees
     * what the CPU path would (B3 parity); no real time passes */
    if (!frame_retired(DJ_T_FRAME))
        return false;
#endif
    return true;
}

#ifdef HAVE_BCM_JOBS_MODEL
/* The device's behaviour in the simulator (bcm_drift on): while the BCM
 * owns the panel the framebuffer's copy is not the truth, so it is
 * poisoned with a pattern no picture has (bcmjob_pp.h BCMJOB_POISON_*):
 * a reader or writer that skips the settle shows it in a shot, or leaves
 * the owned rect unpoisoned (sim_bcm_job.c, "STALE" / "POISONED") */
static void poison(void)
{
    int x, y;

    if (dj_parity_mode())
        return;                 /* parity: the CPU drew the same frame */
    for (y = 0; y < UI_PANEL_H; y++)
    {
        /* not ownable: the owner's own poison of its stale copy (simulator) */
        fb_data *row = FBADDR(UI_PANEL_X, y);

        for (x = 0; x < UI_PANEL_W; x++)
            row[x] = BCMJOB_POISON(UI_PANEL_X + x, y);
    }
}
#endif

bool dj_pan(int h, uint64_t key, int x, int y)
{
    uint32_t p[6];

    dj.rstill = false;
    if (!frame_begin(h, key))
        return false;
    p[0] = dj.slot.slot;
    p[1] = J_XY(x, y);
    p[2] = J_XY(UI_PANEL_W, UI_PANEL_H);
    p[3] = J_SURF_FRONT;
    p[4] = J_XY(UI_PANEL_X, 0);
    p[5] = 0;
    dj.last_op = "blit";
    return sent(bcmjob_submit(J_OP_BLIT, 0, p, 6), "blit")
           && frame_end(true);
}

bool dj_half(int h, uint64_t key, int x0, int y0, int x1, int y1)
{
    uint32_t p[7];

    dj.rstill = false;
    if (!frame_begin(h, key))
        return false;
    p[0] = J_SURF_FRONT;
    p[1] = J_XY(UI_PANEL_X, 0);
    p[2] = J_XY(UI_PANEL_W, UI_PANEL_H);
    p[3] = dj.slot.slot;
    p[4] = J_XY(x0, y0);
    p[5] = dj.slot.slot;
    p[6] = J_XY(x1, y1);
    dj.last_op = "avg";
    return sent(bcmjob_submit(J_OP_AVG, 0, p, 7), "avg")
           && frame_end(true);
}

bool dj_fade(int h, uint64_t key, int x, int y, int weight)
{
    uint32_t p[9 + (UI_SHADOW_W + 3) / 4];

    dj.rstill = false;
    if (!frame_begin(h, key))
        return false;
    /* the shell's ui_panel_fade(raw): b's shadow columns shaded first */
    p[0] = J_SURF_FRONT;
    p[1] = J_XY(UI_PANEL_X, 0);
    p[2] = J_XY(UI_PANEL_W, UI_PANEL_H);
    p[3] = J_SURF_FRONT;
    p[4] = J_XY(UI_PANEL_X, 0);
    p[5] = dj.slot.slot;
    p[6] = J_XY(x, y);
    p[7] = weight;
    p[8] = J_FADE_SHADE_B | UI_SHADOW_W;
    put_alpha(&p[9]);
    dj.last_op = "fade";
    return sent(bcmjob_submit(J_OP_FADE, 0, p, 9 + (UI_SHADOW_W + 3) / 4),
                "fade")
           && frame_end(false);
}

/* ---- the resident panel (drift_jobs.h) ------------------------------- */
/* A FREE the ring refused (full): put again at the next call here, so the
 * pool never keeps a dead resident for the epoch */
static uint32_t rp_free_slot, rp_free_ep;
static bool rp_free_owed;

static void rp_free(uint32_t slot, uint32_t epoch)
{
    uint32_t p[1];

    if (rp_free_owed && rp_free_ep == bcmjob_epoch())
    {
        p[0] = rp_free_slot;
        if (bcmjob_submit(J_OP_FREE, 0, p, 1) == BCMJOB_BUSY)
            return;             /* still full: this one waits as well */
    }
    rp_free_owed = false;
    if (epoch != bcmjob_epoch())
        return;                 /* gone with its epoch */
    p[0] = slot;
    if (bcmjob_submit(J_OP_FREE, 0, p, 1) == BCMJOB_BUSY)
    {
        rp_free_slot = slot;
        rp_free_ep = epoch;
        rp_free_owed = true;
    }
}

void dj_cover_park(void)
{
    uint32_t p[1];

    if (!dj.have || dj.borrowed)
        return;                 /* none, or the resident's (kept) */
    if (dj.epoch == bcmjob_epoch())
    {
        /* in ring order: after any queued frame that reads it */
        p[0] = dj.slot.slot;
        if (bcmjob_submit(J_OP_FREE, 0, p, 1) == BCMJOB_BUSY)
            return;             /* the ring is full: the decode's ALLOC
                                   may then fail, and the CPU decodes */
    }
    dj.have = false;
    UPDATE_TRACE("drift cover park slot=%lu", (unsigned long)dj.slot.slot);
}

bool dj_resident(uint64_t key)
{
    if (rp_free_owed)
        rp_free(0, 0);          /* the owed FREE only (epoch 0: none new) */
    return key && rp.valid && rp.key == key && rp.epoch == bcmjob_epoch();
}

uint32_t dj_epoch(void)
{
    return bcmjob_epoch();
}

uint64_t dj_resident_key(void)
{
    return rp.valid && rp.epoch == bcmjob_epoch() ? rp.key : 0;
}

/* The drift stops naming the resident (it was dropped or replaced) */
static void rp_unborrow(void)
{
    if (dj.have && dj.borrowed)
    {
        dj.have = false;
        dj.borrowed = false;
    }
}

void dj_resident_set(uint64_t key, uint32_t slot, uint32_t epoch)
{
    if (epoch != bcmjob_epoch())
    {
        UPDATE_TRACE("drift resident stale key=%08lx%08lx",
                     (unsigned long)(key >> 32), (unsigned long)(uint32_t)key);
        return;                 /* gone with its epoch: decoded again */
    }
    if (rp.valid && rp.slot != slot)
    {
        rp_unborrow();
        rp_free(rp.slot, rp.epoch);
    }
    rp.key = key;
    rp.slot = slot;
    rp.epoch = epoch;
    rp.valid = true;
    ui_log("panel", "resident set key=%08lx%08lx slot=%lu epoch=%lu",
           (unsigned long)(key >> 32), (unsigned long)(uint32_t)key,
           (unsigned long)slot, (unsigned long)epoch);
}

void dj_resident_drop(void)
{
    if (!rp.valid)
        return;
    rp_unborrow();
    rp_free(rp.slot, rp.epoch);
    rp.valid = false;
    UPDATE_TRACE("drift resident drop key=%08lx%08lx",
                 (unsigned long)(rp.key >> 32), (unsigned long)(uint32_t)rp.key);
}

bool dj_resident_show(uint64_t key, int x, int y, int weight)
{
    if (dj_path() != DJ_PATH_JOBS || !dj_resident(key))
        return false;
    /* frame_begin() takes the slot by key (no upload: no handle) */
    if (weight < 256)
        return dj_fade(0, key, x, y, weight);
    {
#ifndef SIMULATOR
        unsigned long t0 = USEC_TIMER;
#endif
        if (!dj_pan(0, key, x, y))
            return false;
#ifndef SIMULATOR
        /* bench row RESPANEL (bench-p19.md): the still frame's BLIT,
         * shadow and PRESENT on the BCM, from the PP's submit to its FENCE
         * retired (no upload, no read-back: none of either is timed here
         * because none happens) */
        {
            int r = dj.fence ? bcmjob_wait(dj.fence, DJ_T_FRAME) : BCMJOB_OK;

            if (r == BCMJOB_OK)
                dj.fence = 0;
            ui_log("panel", "resident show us=%lu rc=%d",
                   (unsigned long)(USEC_TIMER - t0), r);
        }
#endif
    }
    dj.rstill = true;
    return true;
}

bool dj_still(void)
{
    return dj.owned && dj.rstill;
}

bool dj_owned(void)
{
    return dj.owned;
}

bool dj_parity_mode(void)
{
#ifdef HAVE_BCM_JOBS_MODEL
    return bcmjob_drift_mode() == BCMJOB_DRIFT_PARITY;
#else
    return false;
#endif
}

bool dj_resync(void)
{
    return bcmjob_resync(UI_PANEL_X, 0, UI_PANEL_W, UI_PANEL_H)
           == BCMJOB_OK;
}

bool dj_release(void)
{
    bool ok = true;

    if (!dj.owned)
        return true;
    if (dj.fence)
    {
        /* FENCE before the CPU redraws a presented rect (host-driver.md) */
        int r = bcmjob_wait(dj.fence, dj.late ? 0 : DJ_T_FRAME);

        dj.fence = 0;
        dj.late = 0;
        ok = r == BCMJOB_OK;
        if (!ok)
            ui_log("panel", "drift settle wait rc=%d", r);
    }
    dj.owned = false;
    dj.rstill = false;
    bcmjob_owned_set(0, 0, 0, 0);
    return ok;
}

#ifdef HAVE_BCM_JOBS_MODEL
void dj_parity(const char *kind)
{
    static unsigned long frames, bad;
    uint32_t d = bcmjob_model_diff(UI_PANEL_X, 0, UI_PANEL_W, UI_PANEL_H);

    frames++;
    if (d)
    {
        bad++;
        ui_log("panel", "drift parity bad kind=%s px=%lu", kind,
               (unsigned long)d);
    }
    if (frames % 32 == 1 || d)
        ui_log("panel", "drift parity frames=%lu bad=%lu", frames, bad);
}
#endif

bool dj_dev_row(void)
{
    enum bcmjob_drift_mode m = bcmjob_drift_mode() + 1;

    if (m >= BCMJOB_DRIFT_DEV_MODES)
        m = BCMJOB_DRIFT_AUTO;
    bcmjob_drift_mode_set(m);
    dj.cpu = NULL;              /* the developer's retry after a failure */
    ui_log("panel", "drift jobs mode=%s", mode_name[m]);
#ifdef HAVE_BCMFW_DEVTEST
    devbcm_cue("Drift backend", mode_name[m],
               "auto: jobs if this image is verified");
#endif
    return false;
}
#endif /* HAVE_DRIFT_JOBS */
