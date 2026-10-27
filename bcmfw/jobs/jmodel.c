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

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hostproto.h"
#include "jmodel.h"

/* The host model: bcmjob.h over the reference executor (jmodel.h). */

static struct
{
    struct jmodel_cfg cfg;
    struct jref ref;
    struct jring ring;
    uint32_t ring_w[JM_RING_WORDS];
    uint32_t now, busy_until;       /* model ms; the BCM busy until */
    uint32_t ready_at;              /* a cold start's end: jobs from then */
    bool lit, charger, up, dead;
    uint32_t wants;                 /* bit per enum bcm_want holding */
    uint32_t want_until[BCM_WANT_N];
    uint32_t dark_ups[BCM_WANT_N];
    uint32_t state_ms[JM_STATES], last_job_ms, cold, done;
    uint32_t cleared, slow, failed_op;  /* fault ops (0x20 + bit) */
    uint8_t fail_left[32];          /* jmodel_fail_next: jobs still to fail, per op */
    uint32_t slow_mark, fail_mark;  /* ops marked this boot */
    uint32_t slow_miss;             /* ops past their deadline once this boot */
    uint8_t fail_n[32];             /* counted failures per op this boot */
    uint32_t failures;              /* chip failures this boot */
    bool jobs_off;                  /* after the second failure */
    uint32_t next_seq, retired;
    uint32_t cur_op, cur_start;     /* the job running (0 none) and its start */
    uint32_t cur_len;               /* a DECODE's file bytes (its deadline) */
    uint32_t busy_fault;            /* submits still to answer BUSY */
    bool suspended;
    uint32_t anim[HP_MAX_PAYLOAD + 1];  /* the running animation's record */
    bool anim_on;
    uint32_t anim_start, anim_next;
    uint32_t anim_frames;           /* frames the last animation drew */
    void (*on_epoch)(uint32_t);
    /* J_OP_DECODE's input ring as the BCM sees it, and bcmjob_decode()'s
       PP side.  The BCM half: a ring DECODE waits (J_CUR_OP = DECODE)
       until the PP has fed every pass, or aborted, or stalled
       J_DEC_STALL_MS; then it runs over the whole file at once (the bytes
       the PP streamed, in order), as jobs_bcm.c's glue hands jdec the
       ring */
    struct
    {
        bool active;                /* bcmjob_decode() is feeding */
        bool started;               /* the BCM took a ring DECODE (pass 0) */
        bool fed, aborted, stalled; /* every pass in; J_DEC_ABORT; stalled */
        bool ended;                 /* the active call's record ran */
        uint32_t t_start, last_feed;    /* model ms */
        uint32_t tag;               /* the active call's record */
        uint32_t st, sw_sh, end_at; /* its status, reply, end (busy_until) */
        uint8_t *file;              /* pass 0's bytes, len of them */
        uint32_t len, need, sent;   /* bytes: the file, every pass, so far */
        uint32_t ring_len;          /* the running record's file (glue) */
        uint32_t ring_slot, ring_epoch; /* the ring, allocated in this epoch */
        uint32_t seq;
    } dec;
} m;

static void trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#include <stdarg.h>
static void trace(const char *fmt, ...)
{
    char line[112];
    va_list ap;

    if (!m.cfg.trace)
        return;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    m.cfg.trace(line);
}

static const char *const want_name[BCM_WANT_N] = { "any", "decode", "game", "video" };

static uint32_t bcm_pool_addr(uint32_t cls, bool display_only);

static void epoch_changed(void)
{
    if (m.on_epoch)
        m.on_epoch(m.ref.epoch);
}

/* Power the BCM up (cold) or down (cut), with the reason traced */
/* The LCD contract's push: the PP framebuffer's rect to the front frame
 * and the panel */
static void push_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    const struct jref_surf *f = &m.ref.front, *p = &m.ref.panel;
    uint32_t r, c;

    if (!m.cfg.pp_fb)
        return;
    for (r = y; r < y + h && r < f->h; r++)
        for (c = x; c < x + w && c < f->w; c++)
        {
            uint16_t v = m.cfg.pp_fb[r * f->w + c];

            f->px[r * f->pitch + c] = v;
            if (r < p->h && c < p->w)
                p->px[r * p->pitch + c] = v;
        }
}

/* The front frame's rect to the panel (an animation frame's push) */
static void show(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    const struct jref_surf *f = &m.ref.front, *p = &m.ref.panel;
    uint32_t r, c;

    for (r = y; r < y + h && r < f->h && r < p->h; r++)
        for (c = x; c < x + w && c < f->w && c < p->w; c++)
            p->px[r * p->pitch + c] = f->px[r * f->pitch + c];
}

static void resync_all(void)
{
    push_rect(0, 0, m.ref.front.w, m.ref.front.h);
}

/* Our image starts afresh (a cold start, or a developer stage's
 * restore): every slot gone, a new epoch, and the job block's counters
 * (J_DONE, J_ERRORS, J_LAST_ERR) from 0, as a fresh image's are */
static void image_started(void)
{
    uint32_t rec[1] = { HP_REC(J_OP_RESET, 0, 0) }, rep[HP_MAX_PAYLOAD + 1], n;

    jref_exec(&m.ref, rec, rep, &n);
    m.ref.done = m.ref.errors = m.ref.last_err = 0;
}

static void power(bool up, const char *why)
{
    if (up == m.up)
        return;
    m.up = up;
    if (up)
    {
        m.cold++;
        m.dead = false;
        /* everything on the BCM was lost while it was off */
        image_started();
        m.ring.rd = m.ring.wr;
        m.busy_until = m.ready_at = m.now + JM_COLD_START_MS;
        m.cur_op = 0;                   /* starting, not a job */
        m.anim_on = false;
        trace("bcm power state=C1 why=%s", why);
        resync_all();                   /* the ladder's first frame */
        epoch_changed();
    }
    else
    {
        m.ring.rd = m.ring.wr;          /* the queue dies with the BCM */
        m.anim_on = false;
        m.dec.started = false;          /* and a DECODE waiting on its ring */
        trace("bcm power state=C0 why=%s", why);
    }
}

/* Lit, or a want held: up.  Otherwise cut. */
static void settle(void)
{
    uint32_t k;

    for (k = 1; k < BCM_WANT_N; k++)
        if ((m.wants & (1u << k)) && m.now >= m.want_until[k])
        {
            m.wants &= ~(1u << k);
            trace("bcm power want end why=%s", want_name[k]);
        }
    if (m.lit || m.wants)
    {
        /* the reason the BCM is up while dark, as the driver traces it */
        static char why[24];
        uint32_t w;

        for (w = 1; w < BCM_WANT_N && !(m.wants & (1u << w)); w++)
            ;
        snprintf(why, sizeof(why), "want-%s", w < BCM_WANT_N ? want_name[w] : "any");
        power(true, m.lit ? "lit" : why);
    }
    else
        power(false, "dark");
}

static uint32_t bcm_dec_work(void);

/* jdec_run()'s checks before it decodes (jdec.h): an output's sizes and
 * window, then the work area; the rest is the caller's hook */
static int dec_check(const struct jdec_out *out, int n, uint32_t work_bytes)
{
    uint32_t w, a, best = 0;
    int k, p = 0;

    if (n < 1 || n > JDEC_MAX_OUT)
        return JDEC_E_RANGE;
    for (k = 0; k < n; k++)
    {
        const struct jdec_out *o = &out[k];

        if (!o->dw || !o->dh || !o->cw || !o->ch || !o->px ||
            o->cx + o->cw > o->dw || o->cy + o->ch > o->dh || o->pitch < o->cw)
            return JDEC_E_RANGE;
        a = (uint32_t)o->dw * o->dh;
        if (a > best)
            best = a, p = k;
    }
    w = JDEC_WORK_BASE + JDEC_WORK_ROWS(out[p].dw);      /* jdec_work() */
    for (k = 0; k < n; k++)
        w += JDEC_WORK_OUT(out[k].dw);
    return w > work_bytes ? JDEC_E_NOMEM : JDEC_OK;
}

/* jref's decode hook: the image's work area, then the caller's decoder,
 * over the ring's whole file for a ring DECODE (an abort or a stall: the
 * ring reader's error, jdec's CORRUPT) */
static int model_decode(const uint8_t *jpg, uint32_t len, const struct jdec_out *out,
                        int n, struct jdec_info *info)
{
    int e = dec_check(out, n, m.cfg.dec_work ? m.cfg.dec_work : bcm_dec_work());

    if (e)
        return e;
    if (m.dec.ring_len)
    {
        if (m.dec.aborted || !m.dec.file || !m.dec.fed)
            return JDEC_E_CORRUPT;
        return m.cfg.decode(m.dec.file, m.dec.len, out, n, info);
    }
    return m.cfg.decode(jpg, len, out, n, info);
}

/* A ring DECODE record before jref sees it, as jobs_bcm.c's
 * decode_prepare(): the ring must be a READY scratch slot of a power of
 * two in 4-128 KB; jref gets the slot and len clipped to it, the hook the
 * file's length */
static void dec_prepare(uint32_t *rec)
{
    uint32_t slot = rec[1] & 0xFF, b;

    m.dec.ring_len = 0;
    if (!(rec[1] & J_DEC_RING))
        return;
    b = slot < JREF_SLOTS ? m.ref.slot[slot].bytes : 0;
    if (!jref_slot_px(&m.ref, slot) || m.ref.slot[slot].cls != J_CLS_SCRATCH ||
        b < 4096 || b > 131072 || (b & (b - 1)) || rec[2] < 4)
    {
        rec[1] = 0xEF;                  /* not a slot: jref's J_E_SLOT */
        return;
    }
    m.dec.ring_len = rec[2];
    rec[1] = slot;
    if (rec[2] > b)
        rec[2] = b;
}

void jmodel_init(const struct jmodel_cfg *cfg)
{
    uint32_t k;

    memset(&m, 0, sizeof(m));
    m.cfg = *cfg;
    jref_init(&m.ref, cfg->front, cfg->back, cfg->panel);
    m.ref.decode = cfg->decode ? model_decode : NULL;
    for (k = 0; k < J_CLASSES; k++)
        if (cfg->pool_mem[k])
        {
            jref_pool(&m.ref, k, cfg->pool_mem[k], cfg->pool_bytes[k]);
            jref_pool_addr(&m.ref, k, cfg->pool_addr[k] ? cfg->pool_addr[k] :
                           bcm_pool_addr(k, cfg->decode != NULL));
        }
    m.ring.w = m.ring_w;
    m.ring.size = JM_RING_WORDS;
}

/* A COMMIT's slot bytes (its deadline, jobs.h J_T_COMMIT) */
static uint32_t commit_bytes(const uint32_t *rec)
{
    return HP_REC_LEN(rec[0]) >= 1 && rec[1] < JREF_SLOTS ? m.ref.slot[rec[1]].bytes : 0;
}

static uint32_t cost(uint32_t op, const uint32_t *rec)
{
    uint32_t c = op == J_OP_DECODE && HP_REC_LEN(rec[0]) >= 3 ?
                     (uint32_t)((uint64_t)rec[2] * JM_DECODE_MS_PER_MB >> 20) +
                     JM_DECODE_MS_OUT * rec[3] :
                 op == J_OP_PRESENT ? JM_COST_PRESENT :
                 op == J_OP_FENCE ? 0 :
                 (op >= J_OP_BLIT && op <= J_OP_SCROLL) || op == J_OP_SPECTRUM ||
                 op == J_OP_BARS || (op >= J_OP_POLYLINE && op <= J_OP_PALROW) ?
                 JM_COST_DRAW : JM_COST_SMALL;

    if (op == m.slow)
        c = 40 * (op == J_OP_PRESENT ? J_T_PRESENT :
                  op == J_OP_DECODE ? J_T_DECODE :
                  op == J_OP_SPECTRUM ? J_T_SPECTRUM :
                  op == J_OP_COMMIT ? J_T_COMMIT(commit_bytes(rec)) : J_T_DRAW);
    return c;
}

/* A job's failure as the PP sees it (its reply carries op and status,
 * jobs.md section 3).  Statuses that are the caller's normal outcomes
 * (a full pool, a stale slot, a torn upload, a pool another mode holds,
 * a source the decoder can't take) don't count; any other is the BCM
 * failing that job.  BCMJOB_FAIL_MARK of them in one boot mark the op
 * failed: its CPU path for the rest of the boot (bcmjob.h). */
static void job_failed(uint32_t op, uint32_t st)
{
    trace("bcmjob fault op=0x%02lx why=failed", (unsigned long)op);
    if (st == J_E_NOSLOT || st == J_E_SLOT || st == J_E_SUM || st == J_E_MODE ||
        (st >= J_E_DEC_UNSUPPORTED && st <= J_E_DEC_TRUNCATED) ||
        op < 0x20 || op > 0x3F)
        return;
    if (m.fail_n[op - 0x20] < 255 && ++m.fail_n[op - 0x20] >= BCMJOB_FAIL_MARK)
        m.fail_mark |= 1u << (op - 0x20);
}

/* Run queued jobs while the BCM has time before 'until' */
static void run_until(uint32_t until)
{
    uint32_t rec[HP_MAX_PAYLOAD + 1], rep[HP_MAX_PAYLOAD + 1], nrep, st;
    bool fails;

    while (m.up && !m.dead && m.ring.rd != m.ring.wr)
    {
        uint32_t save = m.ring.rd, op, start = m.busy_until > m.now ? m.busy_until : m.now;

        jring_get(&m.ring, rec);
        op = HP_REC_OP(rec[0]);
        if (start > until)
        {
            m.ring.rd = save;           /* the BCM is still on earlier work */
            break;
        }
        fails = op == m.failed_op ||
                (op >= 0x20 && op < 0x40 && m.fail_left[op - 0x20]);
        if (fails && op != m.failed_op)
            m.fail_left[op - 0x20]--;   /* a counted failure: spent when it runs */
        if (op == J_OP_DECODE && HP_REC_LEN(rec[0]) >= 3 && (rec[1] & J_DEC_RING) &&
            !fails)
        {
            /* the BCM reads the ring as the PP fills it: the job holds
               the loop until every pass is in, an abort, or a stall */
            if (!m.dec.started)
            {
                m.dec.started = true;
                m.dec.t_start = m.dec.last_feed = start;
                if (!m.dec.active)
                    m.dec.fed = m.dec.aborted = m.dec.stalled = false;
            }
            if (!m.dec.fed && !m.dec.aborted)
            {
                if (start - m.dec.last_feed < J_DEC_STALL_MS)
                {
                    m.ring.rd = save;
                    m.cur_op = op;
                    m.cur_start = m.dec.t_start;
                    m.cur_len = rec[2];
                    m.busy_until = start + 1;
                    break;
                }
                m.dec.aborted = m.dec.stalled = true;
            }
            m.dec.started = false;
            start = m.dec.t_start;      /* the job's own start (its deadline) */
            if (m.busy_until < m.now)
                m.busy_until = m.now;
            m.busy_until = (m.busy_until > start ? m.busy_until : start) + cost(op, rec);
            m.cur_op = op;
            m.cur_len = rec[2];
            m.cur_start = start;
            dec_prepare(rec);
            st = jref_exec(&m.ref, rec, rep, &nrep);
            m.dec.ring_len = 0;
            if (st != HP_OK)
                job_failed(op, st);
            if (m.dec.active && HP_REC_TAG(rec[0]) == m.dec.tag)
            {
                m.dec.ended = true;
                m.dec.st = st;
                m.dec.sw_sh = st == HP_OK ? rep[1] : 0;
                m.dec.end_at = m.busy_until;
            }
            m.last_job_ms = m.busy_until;
            m.done++;
            continue;
        }
        m.busy_until = start + cost(op, rec);
        m.cur_op = op;
        m.cur_len = op == J_OP_DECODE && HP_REC_LEN(rec[0]) >= 2 ? rec[2] :
                    op == J_OP_COMMIT ? commit_bytes(rec) : 0;
        m.cur_start = start;
        if (fails)
            rec[0] = HP_REC(op, 0, HP_REC_TAG(rec[0]));     /* a short record */
        st = jref_exec(&m.ref, rec, rep, &nrep);
        if (op == J_OP_DECODE && m.dec.active && HP_REC_TAG(rec[0]) == m.dec.tag)
        {
            m.dec.ended = true;         /* (a failing DECODE: JM_FAULT_FAIL_OP) */
            m.dec.st = st;
            m.dec.sw_sh = 0;
            m.dec.end_at = m.busy_until;
        }
        if (st != HP_OK)
            job_failed(op, st);
        else if (op == J_OP_ANIM)
        {
            memcpy(m.anim, rec, sizeof(m.anim));
            m.anim_on = true;
            m.anim_start = m.anim_next = start;
            m.anim_frames = 0;
        }
        else if (op == J_OP_ANIM_STOP)
            m.anim_on = false;
        if (op == J_OP_FENCE)
            m.retired = rep[1];
        m.last_job_ms = m.busy_until;
        m.done++;
    }
}

void jmodel_advance(uint32_t ms)
{
    uint32_t end = m.now + ms;

    while (m.now < end)
    {
        uint32_t step = 1;
        enum jmodel_state s = !m.up ? JM_C0 :
                              (m.now < m.busy_until || m.ring.rd != m.ring.wr) ? JM_C1 : JM_C2;

        m.state_ms[s] += step;
        m.now += step;
        run_until(m.now);
        if (m.anim_on && m.up && !m.dead && m.now >= m.anim_next)
        {
            /* as the BCM's jobs_tick: the frame into J_SURF_FRONT (a
             * slide out of the front frame moves it on in place, rev 0.4),
             * then its rect to the panel, a stepped PAN's widened to whole
             * pixel pairs as the BCM presents it */
            const uint32_t *a = m.anim + 1;
            uint32_t next = jref_anim_frame(&m.ref, m.anim, m.now - m.anim_start);
            uint32_t xy = a[0] == J_ANIM_SLIDE ? a[3] : a[5], wh = a[4];

            if (a[0] == J_ANIM_PAN && (a[7] & J_PAN_STEP))
            {
                wh += xy & 1;
                xy &= ~1u;
                wh += wh & 1;
            }
            show(J_X(xy), J_Y(xy), J_X(wh), J_Y(wh));
            if (!next)
            {
                m.anim_on = false;
                trace("bcmjob anim end tag=%lu", (unsigned long)HP_REC_TAG(m.anim[0]));
            }
            m.anim_frames++;
            /* the BCM's interval, in whole ms (J_ANIM_FRAME_US: 33) */
            m.anim_next += next / 1000;
        }
        settle();
    }
}

uint32_t jmodel_now(void) { return m.now; }

void jmodel_set_lit(bool lit)
{
    m.lit = lit;
    settle();
}

void jmodel_set_charger(bool on) { m.charger = on; }

void jmodel_fault(enum jmodel_fault f, uint32_t op)
{
    switch (f)
    {
    case JM_FAULT_CLEAR_OP: m.cleared = op; break;
    case JM_FAULT_SLOW_OP:  m.slow = op; break;
    case JM_FAULT_FAIL_OP:  m.failed_op = op; break;
    case JM_FAULT_DEAD:     m.dead = true; break;
    case JM_FAULT_CUT:
        power(false, "fault");
        settle();
        break;
    case JM_FAULT_RING_FULL:
        m.busy_fault = op;
        trace("jmodel fault armed ring-full op=0x%02lx", (unsigned long)op);
        break;
    default:
        m.cleared = m.slow = m.failed_op = m.busy_fault = 0;
        memset(m.fail_left, 0, sizeof(m.fail_left));
        m.dead = false;
        break;
    }
}

void jmodel_fail_next(uint32_t op, uint32_t n)
{
    if (op >= 0x20 && op < 0x40)
    {
        m.fail_left[op - 0x20] = n > 255 ? 255 : n;
    }
}

void jmodel_forgive(uint32_t op)
{
    if (op >= 0x20 && op < 0x40)
    {
        m.fail_mark &= ~(1u << (op - 0x20));
        m.fail_n[op - 0x20] = 0;
    }
}

enum jmodel_state jmodel_state(void)
{
    return !m.up ? JM_C0 : (m.now < m.busy_until || m.ring.rd != m.ring.wr) ? JM_C1 : JM_C2;
}

uint32_t jmodel_state_ms(enum jmodel_state s) { return s < JM_STATES ? m.state_ms[s] : 0; }
uint32_t jmodel_cold_starts(void) { return m.cold; }
uint32_t jmodel_jobs_done(void) { return m.done; }
struct jref *jmodel_ref(void) { return &m.ref; }
uint32_t jmodel_epoch(void) { return m.ref.epoch; }
uint32_t jmodel_dark_ups(enum bcm_want why)
{   /* bcmjob_dark_ups() itself: the simulator renames that one (sim_bcm_job.c) */
    return why < BCM_WANT_N ? m.dark_ups[why] : 0;
}

/* ---- bcmjob.h ------------------------------------------------------------ */

/* Up and past its cold start: the job block is read (jobs.md section 2) */
static bool ready(void)
{
    return m.up && m.now >= m.ready_at;
}

static bool op_ok(uint32_t op)
{
    return op >= 0x20 && op <= 0x3F && op != m.cleared &&
           (op < J_OP_DECODE || (op == J_OP_DECODE && m.ref.decode) ||
#ifdef JREF_VIZ
            op == J_OP_SPECTRUM || op == J_OP_BARS ||   /* proposed, sim only */
            (op >= J_OP_POLYLINE && op <= J_OP_PALROW) ||
#endif
            0) &&
           !(m.slow_mark & (1u << (op - 0x20))) && !(m.fail_mark & (1u << (op - 0x20)));
}

bool bcmjob_have(uint32_t op)
{
    return ready() && !m.jobs_off && !m.suspended && op_ok(op);
}

int bcmjob_resync(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    uint32_t end = m.now + J_T_FENCE_BASE + J_T_PRESENT;

    if (!m.up)
        return BCMJOB_OFF;
    /* after the jobs already queued (jobs.md section 6): the ring drains
     * first, within a fence's deadline and a PRESENT's; a BCM too slow or
     * dead for that gets the push anyway, and the caller resyncs again
     * after its next fence (the recipe's "synced") */
    while (m.ring.rd != m.ring.wr && m.up && !m.dead && m.now < end)
        jmodel_advance(1);
    push_rect(x, y, w, h);
    trace("bcmjob resync x=%lu y=%lu w=%lu h=%lu", (unsigned long)x, (unsigned long)y,
          (unsigned long)w, (unsigned long)h);
    return BCMJOB_OK;
}

void bcmjob_suspend(bool on)
{
    if (m.suspended == on)
        return;
    m.suspended = on;
    if (on)
    {
        m.ring.rd = m.ring.wr;
        m.anim_on = false;
        trace("bcmjob suspend");
    }
    else
    {
        /* the stage swapped images and wrote over the merged map: a new
         * HELLO, a new epoch, the PP's frame back */
        image_started();
        resync_all();
        trace("bcmjob resume epoch=%lu", (unsigned long)m.ref.epoch);
        epoch_changed();
    }
}

int jmodel_anim_tag(void)
{
    return m.anim_on ? (int)HP_REC_TAG(m.anim[0]) : -1;
}

uint32_t jmodel_anim_frames(void)
{
    return m.anim_frames;
}

/* J_CUR_OP: the BCM sets it when a job starts and clears it when it ends;
 * a job here runs from cur_start to busy_until in model time */
uint32_t jmodel_cur_op(void)
{
    return m.up && m.now < m.busy_until && m.cur_op >= 0x20 && m.cur_op <= 0x3F ?
           m.cur_op : 0;
}

int bcmjob_submit(uint32_t op, uint32_t tag, const uint32_t *payload, uint32_t n)
{
    int r;

    if (!ready() || m.jobs_off || m.suspended)
        r = BCMJOB_OFF;
    else if (!op_ok(op))
        r = BCMJOB_NOJOB;
    else if (m.busy_fault && m.busy_fault--)
        r = BCMJOB_BUSY;
    else
    {
        if (jring_put(&m.ring, op, tag, payload, n) != HP_OK)
        {
            r = BCMJOB_BUSY;
            goto refused;
        }
        trace("bcmjob submit op=0x%02lx tag=%lu n=%lu", (unsigned long)op,
              (unsigned long)tag, (unsigned long)n);
        return BCMJOB_OK;
    }
refused:
    trace("bcmjob fault op=0x%02lx why=%s", (unsigned long)op,
          r == BCMJOB_OFF ? "off" : r == BCMJOB_NOJOB ? "nojob" : "busy");
    return r;
}

int bcmjob_fence(uint32_t *seq)
{
    uint32_t s = (m.next_seq + 1) & 0xFFFF;
    int r = bcmjob_submit(J_OP_FENCE, 0, &s, 1);

    if (r != BCMJOB_OK)
        return r;
    m.next_seq = s;
    *seq = s;
    trace("bcmjob fence seq=%lu", (unsigned long)s);
    return BCMJOB_OK;
}

/* A failure: the BCM is cut and started again (the boot ladder); after
 * the second in a boot, jobs stay off */
static void chip_failed(const char *why)
{
    m.failures++;
    trace("bcmjob fault op=0x00 why=%s", why);
    power(false, "fault");
    if (m.failures >= 2)
        m.jobs_off = true;
    settle();
}

/* An op past its deadline with the heartbeat live: jobs.md section 6,
 * retry once, so marked on its second miss this boot (the driver's rule,
 * W0-5/6) */
static void slow_missed(uint32_t op)
{
    uint32_t bit = 1u << (op - 0x20);

    if (m.slow_miss & bit)
        m.slow_mark |= bit;
    m.slow_miss |= bit;
    trace("bcmjob fault op=0x%02lx why=timeout", (unsigned long)op);
}

int bcmjob_wait(uint32_t seq, uint32_t ms)
{
    uint32_t end = m.now + ms;

    while (m.now < end)
    {
        if (!ready())
            return BCMJOB_OFF;          /* cut, or restarted: the fence is gone */
        if (((m.retired - seq) & 0x8000) == 0 && m.retired != 0)
            return BCMJOB_OK;
        jmodel_advance(1);
    }
    if (((m.retired - seq) & 0x8000) == 0 && m.retired != 0)
        return BCMJOB_OK;
    if (m.dead)
    {
        chip_failed("dead");
        return BCMJOB_TIMEOUT;
    }
    /* alive but late: the job running has passed its own deadline (a
     * cold start, or a queue behind a long job, is not a slow job) */
    if (m.now < m.busy_until && m.cur_op >= 0x20 && m.cur_op <= 0x3F)
    {
        uint32_t op = m.cur_op;
        uint32_t dl = op == J_OP_PRESENT ? J_T_PRESENT :
                      op == J_OP_DECODE ? J_T_DECODE * (1 + (m.cur_len >> 20)) :
                      op == J_OP_SPECTRUM ? J_T_SPECTRUM :
                      op == J_OP_COMMIT ? J_T_COMMIT(m.cur_len) :
                      (op >= J_OP_BLIT && op <= J_OP_SCROLL) || op == J_OP_BARS ||
                      (op >= J_OP_POLYLINE && op <= J_OP_PALROW) ? J_T_DRAW : J_T_SMALL;

        if (m.now - m.cur_start > dl)
            slow_missed(op);
    }
    return BCMJOB_TIMEOUT;
}

int bcmjob_slot(uint32_t cls, uint32_t w, uint32_t h, uint32_t key_lo,
                uint32_t key_hi, struct bcmjob_slot *out)
{
    uint32_t rec[6], rep[HP_MAX_PAYLOAD + 1], nrep, st;

    if (!bcmjob_have(J_OP_ALLOC))
        return ready() && !m.jobs_off && !m.suspended ? BCMJOB_NOJOB : BCMJOB_OFF;
    /* the ring runs in order: everything before the ALLOC first */
    while (m.ring.rd != m.ring.wr && m.up && !m.dead)
        jmodel_advance(1);
    rec[0] = HP_REC(J_OP_ALLOC, 5, J_TAG_REPLY);
    rec[1] = cls; rec[2] = J_XY(w, h); rec[3] = J_FMT_RGB565;
    rec[4] = key_lo; rec[5] = key_hi;
    trace("bcmjob submit op=0x%02x tag=%u n=5", J_OP_ALLOC, J_TAG_REPLY);
    st = jref_exec(&m.ref, rec, rep, &nrep);
    if (st != HP_OK)
    {
        job_failed(J_OP_ALLOC, st);
        return BCMJOB_FAILED;
    }
    out->slot = rep[1];
    out->hit = (rep[4] & J_ALLOC_HIT) != 0;
    out->epoch = m.ref.epoch;
    return BCMJOB_OK;
}

#ifdef JREF_VIZ
int bcmjob_spectrum(uint32_t slot, uint32_t n, const uint16_t *edges, uint32_t nb,
                    uint16_t *l2)
{
    uint32_t rec[HP_MAX_PAYLOAD + 1], rep[HP_MAX_PAYLOAD + 1], nrep, st, b;

    if (!bcmjob_have(J_OP_SPECTRUM))
        return ready() && !m.jobs_off && !m.suspended ? BCMJOB_NOJOB : BCMJOB_OFF;
    if (nb < 1 || nb > J_SPEC_BANDS_MAX)
        return BCMJOB_FAILED;
    /* the ring runs in order: everything before it first */
    while (m.ring.rd != m.ring.wr && m.up && !m.dead)
        jmodel_advance(1);
    rec[0] = HP_REC(J_OP_SPECTRUM, J_SPEC_ARGS(nb), J_TAG_REPLY);
    rec[1] = slot;
    rec[2] = n | nb << 16;
    for (b = 0; b <= nb; b += 2)
        rec[3 + b / 2] = edges[b] | (b + 1 <= nb ? (uint32_t)edges[b + 1] << 16 : 0);
    trace("bcmjob submit op=0x%02x tag=%u n=%u", J_OP_SPECTRUM, J_TAG_REPLY,
          J_SPEC_ARGS(nb));
    st = jref_exec(&m.ref, rec, rep, &nrep);
    jmodel_advance(JM_COST_DRAW);
    if (st != HP_OK)
    {
        job_failed(J_OP_SPECTRUM, st);
        return BCMJOB_FAILED;
    }
    for (b = 0; b < nb; b++)
        l2[b] = rep[1 + b / 2] >> (16 * (b & 1));
    return BCMJOB_OK;
}
#endif

int bcmjob_upload(const struct bcmjob_slot *s, const uint16_t *px)
{
    uint16_t *dst;
    struct jref_slot *sl;
    struct jref_surf src;
    uint32_t rec[4], rep[HP_MAX_PAYLOAD + 1], nrep, bytes, r;

    if (s->hit)
        return BCMJOB_OK;
    if (!ready() || m.jobs_off || m.suspended || s->epoch != m.ref.epoch ||
        s->slot >= JREF_SLOTS)
        return BCMJOB_OFF;
    dst = jref_slot_px(&m.ref, s->slot);
    if (!dst)
        return BCMJOB_FAILED;
    sl = &m.ref.slot[s->slot];
    bytes = sl->w * sl->h * 2;
    for (r = 0; r < sl->h; r++)         /* row by row, at the slot's pitch */
        memcpy(dst + r * sl->surf.pitch, px + r * sl->w, sl->w * 2);
    jmodel_advance(bytes / (JM_E2_KB_PER_MS * 1024) + 1);  /* engine 2 */
    src.px = (uint16_t *)px;
    src.w = sl->w;
    src.h = sl->h;
    src.pitch = sl->w;
    rec[0] = HP_REC(J_OP_COMMIT, 3, 0);
    rec[1] = s->slot;
    jref_f16w(&src, &rec[2], &rec[3]);  /* the driver's: j_f16w per row */
    r = jref_exec(&m.ref, rec, rep, &nrep);
    if (r == HP_OK)
        return BCMJOB_OK;
    job_failed(J_OP_COMMIT, r);
    return BCMJOB_FAILED;
}

int bcmjob_upload_rect(uint32_t surf, uint32_t x, uint32_t y, uint32_t w,
                       uint32_t h, const uint16_t *px, uint32_t pitch)
{
    const struct jref_surf *b = &m.ref.back;
    uint32_t r, fit;

    if (!ready() || m.jobs_off || m.suspended)
        return BCMJOB_OFF;
    if (surf != J_SURF_BACK || !w || !h || x >= b->w || w > b->w - x || y >= b->h ||
        h > b->h - y || pitch < w)
        return BCMJOB_FAILED;
    if (m.dec.active)
        return BCMJOB_BUSY;             /* a DECODE's ring has the port */
    /* the rows engine 2 moves within the call's budget (never counted) */
    fit = JM_RECT_MS * JM_E2_KB_PER_MS * 1024 / (w * 2);
    for (r = 0; r < h && r < fit; r++)
        memcpy(b->px + (y + r) * b->pitch + x, px + r * pitch, w * 2);
    if (r < h)
    {
        jmodel_advance(JM_RECT_MS);
        return BCMJOB_BUSY;
    }
    jmodel_advance(w * h * 2 / (JM_E2_KB_PER_MS * 1024) + 1);  /* engine 2 */
    return BCMJOB_OK;
}

uint32_t bcmjob_epoch(void) { return m.ref.epoch; }

/* J_ERRORS: the BCM's failed jobs since its start.  While cut, the last
 * value read (the model's executor keeps it until the next start). */
uint32_t bcmjob_errors(void) { return m.ref.errors; }

void bcmjob_on_epoch(void (*fn)(uint32_t epoch)) { m.on_epoch = fn; }

/* The model has one core: the lock only marks where the driver takes it */
void bcm_port_lock(void) { }
void bcm_port_unlock(void) { }

int bcm_power_want(enum bcm_want why, uint32_t hold_ms)
{
    if (why < 1 || why >= BCM_WANT_N)
        return BCMJOB_FAILED;
    if (why == BCM_WANT_DECODE && !m.charger && !m.lit)
    {
        trace("bcmjob fault op=0x%02x why=refused", J_OP_DECODE);
        return BCMJOB_REFUSED;
    }
    if (!m.lit && !m.up)
    {
        m.dark_ups[why]++;
        m.dark_ups[0]++;
    }
    m.wants |= 1u << why;
    m.want_until[why] = m.now + hold_ms;
    trace("bcm power want why=%s hold=%lu", want_name[why], (unsigned long)hold_ms);
    settle();
    return BCMJOB_OK;
}

void bcm_power_release(enum bcm_want why)
{
    if (why >= 1 && why < BCM_WANT_N)
    {
        m.wants &= ~(1u << why);
        settle();
    }
}

uint32_t bcmjob_dark_ups(enum bcm_want why)
{
    return why < BCM_WANT_N ? m.dark_ups[why] : 0;
}

/* ---- DECODE (bcmjob.h; album-render.md sections 14-15) ------------------ */

/* A record run now, after everything queued before it (ring order), as
 * bcmjob_slot() runs its ALLOC: its status, the reply in rep */
static uint32_t exec_now(uint32_t *rec, uint32_t *rep)
{
    uint32_t nrep, st;

    while (m.ring.rd != m.ring.wr && m.up && !m.dead)
        jmodel_advance(1);
    st = jref_exec(&m.ref, rec, rep, &nrep);
    if (st != HP_OK)
        job_failed(HP_REC_OP(rec[0]), st);
    return st;
}

static uint32_t dec_alloc(uint32_t cls, uint32_t w, uint32_t h, uint32_t lo,
                          uint32_t hi, uint32_t *slot, bool *hit)
{
    uint32_t rec[6], rep[HP_MAX_PAYLOAD + 1], st;

    rec[0] = HP_REC(J_OP_ALLOC, 5, J_TAG_REPLY);
    rec[1] = cls; rec[2] = J_XY(w, h); rec[3] = J_FMT_RGB565;
    rec[4] = lo; rec[5] = hi;
    st = exec_now(rec, rep);
    if (st == HP_OK)
    {
        *slot = rep[1];
        *hit = (rep[4] & J_ALLOC_HIT) != 0;
    }
    return st;
}

/* COMMIT J_COMMIT_NOCHECK: READY, and the BCM's F16w */
static uint32_t dec_commit(uint32_t slot, uint32_t *a, uint32_t *b)
{
    uint32_t rec[5] = { HP_REC(J_OP_COMMIT, 4, J_TAG_REPLY), slot, 0, 0, J_COMMIT_NOCHECK };
    uint32_t rep[HP_MAX_PAYLOAD + 1], st = exec_now(rec, rep);

    if (st == HP_OK && a)
    {
        *a = rep[1];
        *b = rep[2];
    }
    return st;
}

/* The PP's side of the ring for one model ms: engine 2 writes up to
 * JM_E2_KB_PER_MS KB in pieces of at most BCMJOB_DEC_PIECE, each pass from
 * the file's byte 0 (src->start), once the BCM has asked for it (its
 * DECODE started).  Pass 0's bytes are kept: they are what the BCM reads. */
static void dec_feed(const struct bcmjob_dec_req *q, struct bcmjob_dec_res *res,
                     uint8_t *piece)
{
    const struct jdec_src *src = q->src;
    uint32_t budget = JM_E2_KB_PER_MS * 1024, at, n;
    long got;

    while (budget && m.dec.sent < m.dec.need && !m.dec.aborted)
    {
        at = m.dec.sent % m.dec.len;
        if (at == 0)
        {
            res->passes++;
            if (src->start(src->ctx) < 0)
                goto read_err;
        }
        n = m.dec.len - at;
        if (n > BCMJOB_DEC_PIECE)
            n = BCMJOB_DEC_PIECE;
        if (n > budget)
            n = budget;
        got = src->fetch(src->ctx, m.dec.sent < m.dec.len ? m.dec.file + at : piece, n);
        if (got != (long)n ||
            (m.dec.sent >= m.dec.len && memcmp(piece, m.dec.file + at, n)))
            goto read_err;
        m.dec.sent += n;
        res->bytes += n;
        budget -= n;
        m.dec.last_feed = m.now;
    }
    if (m.dec.sent == m.dec.need)
        m.dec.fed = true;
    return;
read_err:
    res->read_err = true;               /* J_DEC_ABORT: the BCM's CORRUPT */
    m.dec.aborted = true;
}

int bcmjob_decode(const struct bcmjob_dec_req *q, struct bcmjob_dec_res *res)
{
    uint32_t pay[J_DEC_ARGS(J_DEC_MAX)], rec[1], len, dl, t0, epoch = 0, k, st;
    uint8_t *piece = NULL;
    bool want = false, cancelled = false, have[J_DEC_MAX] = { false };
    struct bcmjob_dec_out set[J_DEC_MAX];
    int r, n = q->n;

    memset(res, 0, sizeof(*res));
    t0 = m.now;
    len = q->src ? q->src->len : 0;
    if (m.dec.active)
    {
        trace("bcmjob fault op=0x%02x why=busy", J_OP_DECODE);
        return BCMJOB_BUSY;
    }
    /* what jdec_run() would refuse, and what the work area can't take
       whichever outputs hit (none fits alone): never sent */
    r = n < 1 || n > J_DEC_MAX || len < 4 || !q->out;
    for (k = 0; !r && k < (uint32_t)n; k++)
    {
        const struct bcmjob_dec_out *o = &q->out[k];

        r = !o->dw || !o->dh || !o->cw || !o->ch || o->cx + o->cw > o->dw ||
            o->cy + o->ch > o->dh;
    }
    if (r)
    {
        res->err = J_E_RANGE;
        return BCMJOB_FAILED;
    }
    for (k = 0, r = 1; r && k < (uint32_t)n; k++)
        r = bcmjob_dec_work(&q->out[k], 1) > J_DEC_WORK_MIN;
    if (r)
    {
        res->err = HP_E_NOMEM;
        return BCMJOB_FAILED;
    }
    trace("bcmjob decode len=%lu n=%d", (unsigned long)len, n);
    dl = q->ms ? q->ms : BCMJOB_T_DECODE(len, 150);     /* the model's clock */
    if (!q->ms && dl > BCMJOB_DEC_CAP_MS)
        dl = BCMJOB_DEC_CAP_MS;                         /* bcmjob.h's cap */

    /* power, P19 D-3: dark on battery REFUSED; DECODE never powers the
     * BCM (the owner, 2026-10-24: a cold start is 0.4-0.6 s on the UI
     * thread), so any other time it isn't up answers OFF (DEFER) */
    if (!m.lit && !m.charger)
    {
        trace("bcmjob fault op=0x%02x why=refused", J_OP_DECODE);
        r = BCMJOB_REFUSED;
        goto done;
    }
    if (!bcmjob_have(J_OP_DECODE))
    {
        r = ready() && !m.jobs_off && !m.suspended ? BCMJOB_NOJOB : BCMJOB_OFF;
        trace("bcmjob fault op=0x%02x why=%s", J_OP_DECODE, r == BCMJOB_OFF ? "off" : "nojob");
        goto done;
    }
    epoch = m.ref.epoch;
    res->epoch = epoch;

    /* the ring, once per epoch */
    if (m.dec.ring_epoch != epoch || !jref_slot_px(&m.ref, m.dec.ring_slot) ||
        m.ref.slot[m.dec.ring_slot].key_lo != BCMJOB_DEC_RING_KEY)
    {
        bool hit;

        st = dec_alloc(J_CLS_SCRATCH, BCMJOB_DEC_RING_W, BCMJOB_DEC_RING_W,
                       BCMJOB_DEC_RING_KEY, 0, &m.dec.ring_slot, &hit);
        if (st == HP_OK && !hit)
            st = dec_commit(m.dec.ring_slot, NULL, NULL);
        if (st != HP_OK)
        {
            res->err = st;
            r = BCMJOB_FAILED;
            goto done;
        }
        m.dec.ring_epoch = epoch;
    }

    /* the outputs' slots; a hit is another holder's (never written or
       freed here); the misses are the reduced set, the DECODE's out[] */
    for (k = 0; k < (uint32_t)n; k++)
    {
        const struct bcmjob_dec_out *o = &q->out[k];

        st = dec_alloc(o->cls, o->cw, o->ch, o->key_lo, o->key_hi,
                       &res->out[k].slot, &res->out[k].hit);
        if (st != HP_OK)
        {
            res->err = st;
            r = BCMJOB_FAILED;
            goto fail;
        }
        have[k] = !res->out[k].hit;
        if (have[k])
            res->miss[res->miss_n++] = (uint8_t)k;
    }
    /* the work of the set sent: never over (bcmjob.h) */
    if (res->miss_n &&
        bcmjob_dec_work(set, bcmjob_dec_set(q->out, res, set)) > J_DEC_WORK_MIN)
    {
        res->err = HP_E_NOMEM;
        r = BCMJOB_FAILED;
        goto fail;
    }
    /* all of them held already: nothing to decode */
    if (!res->miss_n)
    {
        for (k = 0, r = BCMJOB_OK; k < (uint32_t)n && r == BCMJOB_OK; k++)
            if (dec_commit(res->out[k].slot, &res->out[k].a, &res->out[k].b) != HP_OK)
                r = BCMJOB_OFF;
        goto done;
    }

    /* the record */
    pay[0] = m.dec.ring_slot | J_DEC_RING;
    pay[1] = len;
    pay[2] = res->miss_n;
    for (k = 0; k < res->miss_n; k++)
    {
        const struct bcmjob_dec_out *o = &set[k];
        uint32_t *w = pay + 3 + 5 * k;

        w[0] = J_XY(o->dw, o->dh);
        w[1] = J_XY(o->cx, o->cy);
        w[2] = J_XY(o->cw, o->ch);
        w[3] = res->out[res->miss[k]].slot;
        w[4] = 0;
    }
    m.dec.started = m.dec.fed = m.dec.aborted = m.dec.stalled = m.dec.ended = false;
    m.dec.sent = m.dec.st = m.dec.sw_sh = 0;
    m.dec.file = malloc(len);
    piece = malloc(BCMJOB_DEC_PIECE);
    if (!m.dec.file || !piece)
    {
        res->err = HP_E_NOMEM;
        r = BCMJOB_FAILED;
        goto fail;
    }
    m.dec.len = len;
    /* rev 2 jdec reads the file twice (the probe, the decode): a file the
       ring holds streams once, a longer one once a pass */
    m.dec.need = len <= 2u * BCMJOB_DEC_RING_W * BCMJOB_DEC_RING_W ? len : 2 * len;
    m.dec.tag = J_TAG_REPLY | (++m.dec.seq & 0x7FFF);
    m.dec.ring_epoch = epoch;
    r = bcmjob_submit(J_OP_DECODE, m.dec.tag, pay, J_DEC_ARGS(res->miss_n));
    if (r != BCMJOB_OK)
        goto fail;
    m.dec.active = true;
    t0 = m.now;
    for (;;)
    {
        if (!ready() || m.ref.epoch != epoch || m.suspended)
        {
            r = BCMJOB_OFF;             /* cut or restarted: the job is gone */
            trace("bcmjob fault op=0x%02x why=off", J_OP_DECODE);
            break;
        }
        if (m.dec.ended && m.now >= m.dec.end_at)
            break;
        if (!cancelled && q->cancel && q->cancel(q->ctx))
        {
            cancelled = true;
            if (!m.dec.fed)
                m.dec.aborted = true;   /* J_DEC_ABORT */
            trace("bcmjob fault op=0x%02x why=aborted", J_OP_DECODE);
            continue;                   /* (the hook may have cut the BCM) */
        }
        if (m.dec.started && !m.dec.fed && !m.dec.aborted)
            dec_feed(q, res, piece);
        if (m.now - t0 >= dl)
        {
            if (!m.dec.fed)
                m.dec.aborted = true;
            if (m.dead)
            {
                m.dec.active = false;
                chip_failed("dead");
            }
            else
                slow_missed(J_OP_DECODE);
            r = BCMJOB_TIMEOUT;
            break;
        }
        jmodel_advance(1);
    }
    if (r == BCMJOB_OK)
    {
        st = m.dec.st;
        if (cancelled)
            r = BCMJOB_ABORTED;
        else if (st != HP_OK)
        {
            res->err = st;
            r = BCMJOB_FAILED;
            res->stalled = m.dec.stalled;
        }
        else
        {
            res->sw = J_X(m.dec.sw_sh);
            res->sh = J_Y(m.dec.sw_sh);
            for (k = 0; k < (uint32_t)n && r == BCMJOB_OK; k++)
                if (dec_commit(res->out[k].slot, &res->out[k].a, &res->out[k].b) != HP_OK)
                    r = ready() ? BCMJOB_FAILED : BCMJOB_OFF;
        }
    }
fail:
    /* no partial pixels left READY: FREE queued behind the job (ring
       order: after it ends, even past a timeout) */
    if (r != BCMJOB_OK && ready() && m.ref.epoch == epoch)
        for (k = 0; k < (uint32_t)n; k++)
            if (have[k])
            {
                rec[0] = res->out[k].slot;
                bcmjob_submit(J_OP_FREE, 0, rec, 1);
            }
done:
    m.dec.active = false;
    free(m.dec.file);
    m.dec.file = NULL;
    free(piece);
    if (want && r == BCMJOB_OK)
        bcm_power_want(BCM_WANT_DECODE, BCMJOB_DEC_HOLD_MS);    /* the read-back */
    else if (want)
        bcm_power_release(BCM_WANT_DECODE);
    trace("bcmjob decode done st=%d err=%lu passes=%lu bytes=%lu ms=%lu", r,
          (unsigned long)res->err, (unsigned long)res->passes, (unsigned long)res->bytes,
          (unsigned long)(m.now - t0));
    return r;
}

int bcmjob_read(uint32_t slot, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                uint16_t *px, uint32_t pitch)
{
    const struct jref_slot *sl;
    uint32_t r;

    if (!ready() || m.jobs_off || m.suspended ||
        (slot < JREF_SLOTS && m.ref.slot[slot].epoch != m.ref.epoch))
        return BCMJOB_OFF;
    if (slot >= JREF_SLOTS || !jref_slot_px(&m.ref, slot))
        return BCMJOB_FAILED;
    sl = &m.ref.slot[slot];
    if (!w || !h || x + w > sl->w || y + h > sl->h || pitch < w)
        return BCMJOB_FAILED;
    for (r = 0; r < h; r++)
        memcpy(px + r * pitch, sl->surf.px + (y + r) * sl->surf.pitch + x, 2 * w);
    jmodel_advance(w * h / 2 / JM_READ_WORDS_PER_MS + 1);  /* port reads */
    return BCMJOB_OK;
}

/* ---- the BCM's pool addresses ------------------------------------------ */

/* config.h's, for both images; last in the file, as that header is the
 * BCM image's own (its integer types, and float poisoned) */
#undef BCMFW_HOSTPROTO
#undef BCMFW_JOBS
#undef BCMFW_VIDEO
#define BCMFW_HOSTPROTO 1
#define BCMFW_JOBS 1
#define BCMFW_VIDEO 1
#include "../config.h"

/* the display-only image's decode work area (the only image with DECODE) */
static uint32_t bcm_dec_work(void)
{
    return BCMFW_JOBS_DO_DRIFT - BCMFW_JOBS_DEC_WORK;
}

static uint32_t bcm_pool_addr(uint32_t cls, bool display_only)
{
    switch (cls)
    {
    case J_CLS_DRIFT:   return display_only ? BCMFW_JOBS_DO_DRIFT : BCMFW_JOBS_DRIFT;
    case J_CLS_STRIP:   return display_only ? BCMFW_JOBS_DO_STRIP : BCMFW_JOBS_STRIP;
    case J_CLS_THUMB:   return display_only ? BCMFW_JOBS_DO_THUMB : BCMFW_JOBS_THUMB;
    case J_CLS_APP:     return display_only ? BCMFW_JOBS_DO_APP : BCMFW_JOBS_APP;
    case J_CLS_SCRATCH: return BCMFW_JOBS_SCRATCH;
    default:            return 0;
    }
}
