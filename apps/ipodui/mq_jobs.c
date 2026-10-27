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

/* P19 19-5: a marquee as the BCM2722's stepped PAN (mq_jobs.h) */

#include "config.h"
#include "ipodui/mq_jobs.h"

#ifdef HAVE_MQ_JOBS
#include <string.h>
#include "kernel.h"
#include "lcd.h"
#include "bcmjob_pp.h"
#include "bcmjob.h"
#include "ipodui/anim.h"            /* UPDATE_TRACE */
#include "ipodui/art_bcm.h"
#include "ipodui/drift_jobs.h"
#include "ipodui/uilog.h"

#define MQJ_TAG     0x4D51          /* 'MQ': the ANIM's (no reply) */
#define MQJ_T_STOP  (J_T_FENCE_BASE + J_T_SMALL)

static struct
{
    struct bcmjob_slot slot;
    uint64_t key;                   /* the strip in .slot, 0 none */
    bool have;                      /* .slot is ours in .slot.epoch */
    bool running;
    uint32_t epoch;                 /* the PAN's */
    long sent;                      /* tick it was sent */
    uint32_t start;                 /* its start word (ms into the lap) */
    uint32_t lap_ms;                /* pause + (K - 1) steps */
    int x, y, w, h;                 /* the window on the screen */
    long last_phase;
    void (*settle)(long phase);
} mq = { .last_phase = -1 };

static long ms_since(long tick)
{
    return (long)(current_tick - tick) * (1000 / HZ);
}

bool mqj_available(const char **why)
{
    const char *w = NULL;

    if (!dj_jobs_wanted())
        w = "mode";
    else if (art_bcm_busy())
        w = "decode";
    else if (!bcmjob_have(J_OP_ANIM) || !bcmjob_have(J_OP_ANIM_STOP)
             || !bcmjob_have(J_OP_ALLOC) || !bcmjob_have(J_OP_FENCE))
        w = "nojob";
    else if (bcmjob_minor() < 3)
        w = "minor";                /* an older image: a linear pan */
    if (why)
        *why = w;
    return !w;
}

long mqj_phase(void)
{
    if (!mq.lap_ms)
        return -1;
    if (!mq.running)
        return mq.last_phase;
    return (long)((mq.start + (uint32_t)ms_since(mq.sent)) % mq.lap_ms);
}

static void forget_slot(void)
{
    mq.have = false;
    mq.key = 0;
}

bool mqj_start(const uint16_t *px, int w, int h, uint64_t key, int lap,
               int win_w, int dst_x, int dst_y, uint32_t start_ms,
               void (*settle)(long phase))
{
    uint32_t p[J_PAN_ARGS], seq, errs, ep;
    const char *why = NULL;
    int r, k, steps;
    bool up = false;                /* the strip went up (not kept, no hit) */

    if (mq.running)
        mqj_stop("restart");
    if (!mqj_available(&why))
        goto cpu;
    ep = bcmjob_epoch();
    if (mq.have && (mq.slot.epoch != ep || mq.key != key))
    {
        if (mq.slot.epoch == ep)
        {
            p[0] = mq.slot.slot;    /* the last label's: the pool is small */
            bcmjob_submit(J_OP_FREE, 0, p, 1);
        }
        forget_slot();
    }
    if (!mq.have)
    {
        /* ALLOC (a hit: this label is there already), then the pixels */
        r = bcmjob_slot(J_CLS_STRIP, w, h, (uint32_t)key,
                        (uint32_t)(key >> 32), &mq.slot);
        if (r != BCMJOB_OK)
        {
            why = "alloc";
            goto cpu;
        }
        for (k = 0, r = BCMJOB_OK; !mq.slot.hit && k < 2; k++)
            if ((r = bcmjob_upload(&mq.slot, px)) == BCMJOB_OK)
                break;              /* J_E_SUM (torn): once more */
        if (r != BCMJOB_OK)
        {
            p[0] = mq.slot.slot;
            bcmjob_submit(J_OP_FREE, 0, p, 1);
            why = "upload";
            goto cpu;
        }
        mq.have = true;
        mq.key = key;
        up = !mq.slot.hit;
    }
    /* the stepped PAN: F2 word for word (jobs.md section 5) */
    p[0] = J_ANIM_PAN;
    p[1] = mq.slot.slot;
    p[2] = J_XY(0, 0);              /* no rest cell: the strip's head */
    p[3] = J_XY(lap, 0);
    p[4] = J_XY(win_w, h);
    p[5] = J_XY(dst_x, dst_y);
    p[6] = MQJ_STEP_MS;
    p[7] = J_PAN_STEP | 1;          /* lap after lap until ANIM_STOP */
    p[8] = MQJ_STEP_PX | MQJ_PAUSE_MS << 16;
    p[9] = J_XY(0, 0);
    p[10] = start_ms;
    errs = bcmjob_errors();
    r = bcmjob_submit(J_OP_ANIM, MQJ_TAG, p, J_PAN_ARGS);
    if (r != BCMJOB_OK)
    {
        why = r == BCMJOB_BUSY ? "busy" : "submit";
        goto cpu;
    }
    /* a refused record shows in J_ERRORS once its FENCE retires */
    if (bcmjob_fence(&seq) != BCMJOB_OK
        || bcmjob_wait(seq, MQJ_T_STOP) != BCMJOB_OK
        || bcmjob_epoch() != ep || bcmjob_errors() != errs)
    {
        p[0] = MQJ_TAG;
        bcmjob_submit(J_OP_ANIM_STOP, MQJ_TAG, p, 1);
        why = "refused";
        goto cpu;
    }
    steps = (lap + MQJ_STEP_PX - 1) / MQJ_STEP_PX;      /* K */
    mq.running = true;
    mq.epoch = ep;
    mq.sent = current_tick;
    mq.start = start_ms;
    mq.lap_ms = MQJ_PAUSE_MS + (uint32_t)(steps - 1) * MQJ_STEP_MS;
    mq.x = dst_x;
    mq.y = dst_y;
    mq.w = win_w;
    mq.h = h;
    mq.settle = settle;
    mq.last_phase = -1;
    (void)up;                       /* trace only */
    UPDATE_TRACE("np mq bcm start key=%08lx%08lx lap=%d w=%d up=%d",
                 (unsigned long)(key >> 32), (unsigned long)(uint32_t)key,
                 lap, w, (int)up);
    return true;
cpu:
    UPDATE_TRACE("np mq cpu why=%s", why ? why : "?");
    return false;
}

bool mqj_running(void)
{
    if (mq.running && bcmjob_epoch() != mq.epoch)
    {
        /* cut or restarted: the PAN went with the epoch, and the restart
         * showed the framebuffer */
        mq.last_phase = mqj_phase();
        mq.running = false;
        forget_slot();
        UPDATE_TRACE("np mq bcm stop why=%s phase=%ld", "epoch",
                     mq.last_phase);
    }
    return mq.running;
}

bool mqj_active(void)
{
    return mq.running;
}

long mqj_stop(const char *why)
{
    uint32_t p[1], seq;
    int r = BCMJOB_OK;

    if (!mqj_running())
        return -1;
    mq.last_phase = mqj_phase();
    mq.running = false;
    p[0] = MQJ_TAG;
    if (bcmjob_submit(J_OP_ANIM_STOP, MQJ_TAG, p, 1) == BCMJOB_OK
        && (r = bcmjob_fence(&seq)) == BCMJOB_OK)
        r = bcmjob_wait(seq, MQJ_T_STOP);
    if (r == BCMJOB_TIMEOUT)
    {
        /* the ring is behind: the band from the framebuffer, after
         * whatever is still queued (review M3); the caller's frame
         * follows it */
        bcmjob_resync(0, mq.y, LCD_WIDTH, mq.h);
        ui_log("np", "mq stop timeout why=%s", why);
    }
    UPDATE_TRACE("np mq bcm stop why=%s phase=%ld rc=%d", why,
                 mq.last_phase, r);
    return mq.last_phase;
}

void mqj_settle(int x, int y, int w, int h)
{
    long u;

    if (!mq.running || x >= mq.x + mq.w || x + w <= mq.x
        || y >= mq.y + mq.h || y + h <= mq.y)
        return;
    u = mqj_stop("settle");
    if (u >= 0 && mq.settle)
        mq.settle(u);
}
#endif /* HAVE_MQ_JOBS */
