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

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hostproto.h"
#include "jmodel.h"

/* Host tests of the job interface's host model (jmodel.c): the bcmjob.h
 * API's results, ordering, slots and epochs, the faults the simulator
 * injects, the power states and the dark power-up count, and the trace
 * lines the gate matches.  Prints FAIL lines, then "jmtest: N cases,
 * M failed". */

static int cases, failed;

#define CHECK(c, ...) do { cases++; if (!(c)) { failed++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint16_t F[96 * 64], B[96 * 64], P[96 * 64], PIC[48 * 32], PPFB[96 * 64];
static uint8_t drift[2][4096], strip[8192];
static char log_[4096][112];
static int nlog;
static uint32_t epochs_seen;

static void trace(const char *line)
{
    if (nlog < 4096)
        snprintf(log_[nlog++], sizeof(log_[0]), "%s", line);
}

static int logged(const char *prefix)
{
    int i;

    for (i = 0; i < nlog; i++)
        if (!strncmp(log_[i], prefix, strlen(prefix)))
            return 1;
    return 0;
}

static void on_epoch(uint32_t e)
{
    epochs_seen = e;
}

static void init(void)
{
    struct jmodel_cfg c;

    memset(&c, 0, sizeof(c));
    c.front = (struct jref_surf){ F, 96, 64, 96 };
    c.back = (struct jref_surf){ B, 96, 64, 96 };
    c.panel = (struct jref_surf){ P, 96, 64, 96 };
    c.pool_mem[J_CLS_DRIFT] = drift;
    c.pool_bytes[J_CLS_DRIFT] = sizeof(drift);
    c.pool_mem[J_CLS_STRIP] = strip;
    c.pool_bytes[J_CLS_STRIP] = sizeof(strip);
    c.trace = trace;
    c.pp_fb = PPFB;
    jmodel_init(&c);
    bcmjob_on_epoch(on_epoch);
    nlog = 0;
    epochs_seen = 0;
}

/* The shell's F2 marquee in ms (as jtest.c's f2_at; apps/ipodui/list.c):
 * mq_x at 'now', the selection having settled at 0 */
static uint32_t f2_at(uint32_t now, uint32_t lap, uint32_t px, uint32_t step,
                      uint32_t delay, uint32_t pause)
{
    uint32_t x = 0, next = delay, t;

    for (t = 0; t <= now; t++)
        if (t >= next)
        {
            x += px;
            next = t + step;
            if (x >= lap)
            {
                x = 0;
                next = t + pause;
            }
        }
    return x;
}

/* The cell on the panel equals the slot's window at sx (rows RH) */
#define MQ_TW   40
#define MQ_RH   12
#define MQ_DX   10
#define MQ_DY   20
static uint16_t MQ[(2 * MQ_TW + 64) * MQ_RH];

static uint32_t cell_differs(uint32_t sw, uint32_t sx)
{
    uint32_t x, y, bad = 0;

    for (y = 0; y < MQ_RH; y++)
        for (x = 0; x < MQ_TW; x++)
            bad += P[(MQ_DY + y) * 96 + MQ_DX + x] != MQ[y * sw + sx + x];
    return bad;
}

/* The shell's recipe for the marquee (jobs.md, "Shell recipe: the
 * marquee"): the strip uploaded once, PAN's stepped form sent at F2's
 * first step, its frames F2's own; ANIM_STOP and the row redrawn on the
 * CPU; and the faults: a full ring, a cut (the epoch), dark, a cleared
 * op, a stale slot, every job OFF while the BCM starts */
static void marquee_seq(void)
{
    enum { L = 53, PX = 2, STEP = 50, PAUSE = 1000 };
    uint32_t sw = MQ_TW + L + MQ_TW, x, y, k, t0, bad, seq, e, errs, q[J_PAN_ARGS];
    struct bcmjob_slot s;

    init();
    jmodel_set_lit(true);
    jmodel_advance(JM_COLD_START_MS);
    /* the slot: [the cell at rest][the label's lap][its head again]; the
     * PP's frame has the row at rest, the cell as the slot's rest */
    for (y = 0; y < MQ_RH; y++)
        for (x = 0; x < sw; x++)
            MQ[y * sw + x] = (uint16_t)(0x100 + x + 0x400 * y);
    for (k = 0; k < 96 * 64; k++)
        PPFB[k] = 0x5555;
    for (y = 0; y < MQ_RH; y++)
        for (x = 0; x < MQ_TW; x++)
            PPFB[(MQ_DY + y) * 96 + MQ_DX + x] = MQ[y * sw + x];
    bcmjob_resync(0, 0, 96, 64);
    CHECK(bcmjob_slot(J_CLS_STRIP, sw, MQ_RH, 0x4D51, 0, &s) == BCMJOB_OK && !s.hit &&
          bcmjob_upload(&s, MQ) == BCMJOB_OK, "the strip's upload");
    CHECK(bcmjob_have(J_OP_ANIM) && bcmjob_have(J_OP_ANIM_STOP), "no ANIM");
    q[0] = J_ANIM_PAN; q[1] = s.slot; q[2] = J_XY(MQ_TW, 0); q[3] = J_XY(MQ_TW + L, 0);
    q[4] = J_XY(MQ_TW, MQ_RH); q[5] = J_XY(MQ_DX, MQ_DY); q[6] = STEP;
    q[7] = J_PAN_STEP | 1; q[8] = PX | (uint32_t)PAUSE << 16; q[9] = J_XY(0, 0);
    q[10] = PAUSE;                      /* sent at F2's first step */
    CHECK(bcmjob_submit(J_OP_ANIM, 12, q, J_PAN_ARGS) == BCMJOB_OK, "the marquee's ANIM");
    jmodel_advance(1);                  /* it runs, and draws its first frame */
    t0 = jmodel_now();
    for (bad = 0, k = 0; k <= 2 * (PAUSE + L / PX * STEP); k += 10)
    {
        uint32_t m = f2_at(PAUSE + k, L, PX, STEP, PAUSE, PAUSE);

        bad += cell_differs(sw, m ? MQ_TW + m : 0);
        jmodel_advance(10);
    }
    CHECK(bad == 0, "the model's frames differ from F2's in %u pixels", bad);
    CHECK(jmodel_anim_tag() == 12 && jmodel_anim_frames() == (jmodel_now() - t0) / STEP + 1,
          "frames %u in %u ms", jmodel_anim_frames(), jmodel_now() - t0);
    for (bad = 0, y = 0; y < 64; y++)
        for (x = 0; x < 96; x++)
            if (y < MQ_DY || y >= MQ_DY + MQ_RH || x < MQ_DX || x >= MQ_DX + MQ_TW)
                bad += P[y * 96 + x] != 0x5555;
    CHECK(bad == 0, "the pan drew outside its cell: %u", bad);

    /* the selection moves: ANIM_STOP, a fence, then the CPU's row */
    CHECK(bcmjob_submit(J_OP_ANIM_STOP, 12, q, 1) == BCMJOB_OK &&
          bcmjob_fence(&seq) == BCMJOB_OK && bcmjob_wait(seq, J_T_FENCE_BASE + J_T_SMALL) ==
          BCMJOB_OK && jmodel_anim_tag() < 0, "ANIM_STOP");
    k = P[MQ_DY * 96 + MQ_DX];
    jmodel_advance(200);
    CHECK(P[MQ_DY * 96 + MQ_DX] == k, "a frame after the stop");
    bcmjob_resync(MQ_DX, MQ_DY, MQ_TW, MQ_RH);
    CHECK(cell_differs(sw, 0) == 0, "the row at rest after the stop");

    /* the same title again: a hit, nothing uploaded */
    {
        struct bcmjob_slot s2;

        CHECK(bcmjob_slot(J_CLS_STRIP, sw, MQ_RH, 0x4D51, 0, &s2) == BCMJOB_OK && s2.hit &&
              s2.slot == s.slot, "the same strip again is not a hit");
    }

    /* a full ring: BUSY at once, nothing runs (the shell steps F2 itself) */
    jmodel_fault(JM_FAULT_RING_FULL, 1);
    CHECK(bcmjob_submit(J_OP_ANIM, 12, q, J_PAN_ARGS) == BCMJOB_BUSY, "no busy");
    jmodel_advance(100);
    CHECK(jmodel_anim_tag() < 0 && cell_differs(sw, 0) == 0, "a busy ANIM ran");

    /* a mid-lap start (the shell takes over from F2 at step 5) */
    q[10] = PAUSE + 4 * STEP;
    CHECK(bcmjob_submit(J_OP_ANIM, 12, q, J_PAN_ARGS) == BCMJOB_OK, "a mid-lap ANIM");
    jmodel_advance(1);
    CHECK(cell_differs(sw, MQ_TW + 5 * PX) == 0, "the mid-lap start is not step 5");

    /* a cut (an epoch change) mid-pan: the animation is gone, the restart
     * shows the PP's frame (the row at rest), the shell hears the epoch;
     * its old slot is stale (J_E_SLOT: not counted against the op) */
    e = bcmjob_epoch();
    jmodel_fault(JM_FAULT_CUT, 0);
    CHECK(jmodel_anim_tag() < 0 && epochs_seen != e && cell_differs(sw, 0) == 0,
          "a cut mid-pan: anim %d, epoch %u", jmodel_anim_tag(), epochs_seen);
    CHECK(bcmjob_submit(J_OP_ANIM, 12, q, J_PAN_ARGS) == BCMJOB_OFF,
          "an ANIM while the BCM starts");
    CHECK(bcmjob_upload(&s, MQ) == BCMJOB_OFF, "an upload to the old epoch's slot");
    jmodel_advance(JM_COLD_START_MS);
    errs = bcmjob_errors();
    CHECK(bcmjob_submit(J_OP_ANIM, 12, q, J_PAN_ARGS) == BCMJOB_OK &&
          bcmjob_fence(&seq) == BCMJOB_OK && bcmjob_wait(seq, 30) == BCMJOB_OK &&
          bcmjob_errors() == errs + 1 && bcmjob_have(J_OP_ANIM) && jmodel_anim_tag() < 0,
          "a stale slot's ANIM: errors %u -> %u", errs, bcmjob_errors());
    CHECK(bcmjob_submit(J_OP_ANIM, 12, q, J_PAN_ARGS) == BCMJOB_OK &&
          bcmjob_fence(&seq) == BCMJOB_OK && bcmjob_wait(seq, 30) == BCMJOB_OK &&
          bcmjob_have(J_OP_ANIM), "a second stale ANIM marked the op");

    /* re-uploaded by key, it runs again; dark stops it (the BCM is cut),
     * and lit again the panel starts from the PP's frame */
    CHECK(bcmjob_slot(J_CLS_STRIP, sw, MQ_RH, 0x4D51, 0, &s) == BCMJOB_OK && !s.hit &&
          bcmjob_upload(&s, MQ) == BCMJOB_OK, "the re-upload");
    q[1] = s.slot;
    q[10] = PAUSE;
    CHECK(bcmjob_submit(J_OP_ANIM, 12, q, J_PAN_ARGS) == BCMJOB_OK, "the ANIM again");
    jmodel_advance(120);
    CHECK(jmodel_anim_tag() == 12 && cell_differs(sw, MQ_TW + 3 * PX) == 0,
          "the marquee again: step 3 at 100 ms");
    jmodel_set_lit(false);
    CHECK(jmodel_anim_tag() < 0, "dark: the marquee still runs");
    jmodel_set_lit(true);
    CHECK(cell_differs(sw, 0) == 0, "lit again: not the PP's frame");

    /* the op cleared from the image: NOJOB, the CPU path */
    jmodel_advance(JM_COLD_START_MS);
    jmodel_fault(JM_FAULT_CLEAR_OP, J_OP_ANIM);
    CHECK(!bcmjob_have(J_OP_ANIM) && bcmjob_submit(J_OP_ANIM, 12, q, J_PAN_ARGS) ==
          BCMJOB_NOJOB, "a cleared ANIM");
    jmodel_fault(JM_FAULT_NONE, 0);
}

static int fill_present(uint16_t colour)
{
    uint32_t p[4] = { J_SURF_FRONT, 0, J_XY(96, 64), colour }, seq;
    int r;

    if ((r = bcmjob_submit(J_OP_FILL, 0, p, 4)) != BCMJOB_OK)
        return r;
    p[3] = 0;
    if ((r = bcmjob_submit(J_OP_PRESENT, 0, p, 4)) != BCMJOB_OK)
        return r;
    if ((r = bcmjob_fence(&seq)) != BCMJOB_OK)
        return r;
    return bcmjob_wait(seq, 100);
}

/* ---- the retained screen (jobs.md "Shell recipe"), on the model ------- */

/* The shell's slide easing (ui_main.c slide_frame(), cubic ease-out in
 * 1/1024 units) in ms, and the ANIM SLIDE table for 33 ms frames: frame k
 * at the shell's first frame (10 ms) plus k x 33.3 ms, to the frame that
 * lands on w */
static uint32_t slide_want(uint32_t el, uint32_t dur, uint32_t w)
{
    uint32_t inv, rest;

    if (el >= dur)
        return w;
    inv = 1024 - el * 1024 / dur;
    rest = inv * inv / 1024 * inv / 1024;
    return w - w * rest / 1024;
}

static uint32_t slide_table(uint32_t dur, uint32_t w, uint32_t *rec)
{
    uint32_t k = 0, o = 0;

    while (o < w && k < J_SLIDE_MAX)
    {
        o = slide_want(10 + 100 * k / 3, dur, w);
        if (k & 1)
            rec[8 + k / 2] |= o << 16;
        else
            rec[8 + k / 2] = o;
        k++;
    }
    return k;
}

#define VY 8                /* the list's view: y 8, 48 rows, rows of 8 */
#define VH 48
#define RH 8
#define SBX 88              /* the scrollbar column, x 88-95 */
static uint16_t LST[96 * RH * 20];

/* The CPU's glide frame at pos into the PP framebuffer (glide_frame()) */
static void cpu_glide(uint32_t pos)
{
    uint32_t r, x;

    for (r = 0; r < VH; r++)
        for (x = 0; x < 96; x++)
            PPFB[(VY + r) * 96 + x] = x >= SBX ? (uint16_t)(pos * 977 + r * 31 + x) :
                                      LST[(pos + r) * 96 + x];
}

/* The rect of the panel (and of the front frame) equals the PP's */
static int shows_pp(uint32_t x0, uint32_t y0, uint32_t w, uint32_t h)
{
    uint32_t x, y;

    for (y = y0; y < y0 + h; y++)
        for (x = x0; x < x0 + w; x++)
            if (P[y * 96 + x] != PPFB[y * 96 + x] || F[y * 96 + x] != PPFB[y * 96 + x])
                return 0;
    return 1;
}

/* One glide frame by the recipe: SCROLL the view, the new rows and the
 * scrollbar column into BACK and BLITted, PRESENT (WAIT), FENCE.  The
 * previous frame's fence first (BACK is reused).  BCMJOB_OK, or the
 * fault: the caller resyncs.  A step of the whole view or more is no
 * scroll: the CPU's frame (BCMJOB_NOJOB) */
static int glide_jobs(int32_t d, uint32_t *seq)
{
    uint32_t q[6], nh = d > 0 ? (uint32_t)d : (uint32_t)-d, ny = d > 0 ? VY + VH - nh : VY;
    int r;

    if (nh >= VH)
        return BCMJOB_NOJOB;
    if (*seq && (r = bcmjob_wait(*seq, J_T_FENCE_BASE + J_T_PRESENT)) != BCMJOB_OK)
        return r;
    q[0] = J_SURF_FRONT; q[1] = J_XY(0, VY); q[2] = J_XY(96, VH); q[3] = (uint16_t)-d;
    if ((r = bcmjob_submit(J_OP_SCROLL, 0, q, 4)) != BCMJOB_OK)
        return r;
    if ((r = bcmjob_upload_rect(J_SURF_BACK, 0, ny, 96, nh, PPFB + ny * 96, 96)) != BCMJOB_OK ||
        (r = bcmjob_upload_rect(J_SURF_BACK, SBX, VY, 96 - SBX, VH, PPFB + VY * 96 + SBX,
                                96)) != BCMJOB_OK)
        return r;
    q[0] = J_SURF_BACK; q[1] = J_XY(0, ny); q[2] = J_XY(96, nh); q[3] = J_SURF_FRONT;
    q[4] = q[1]; q[5] = 0;
    if ((r = bcmjob_submit(J_OP_BLIT, 0, q, 6)) != BCMJOB_OK)
        return r;
    q[1] = J_XY(SBX, VY); q[2] = J_XY(96 - SBX, VH); q[4] = q[1];
    if ((r = bcmjob_submit(J_OP_BLIT, 0, q, 6)) != BCMJOB_OK)
        return r;
    q[0] = J_SURF_FRONT; q[1] = J_XY(0, VY); q[2] = J_XY(96, VH); q[3] = J_PRES_WAIT;
    if ((r = bcmjob_submit(J_OP_PRESENT, 0, q, 4)) != BCMJOB_OK)
        return r;
    return bcmjob_fence(seq);
}

/* The landing of a slide (its end, or input mid-slide): ANIM_STOP, the
 * new page from BACK, PRESENT (WAIT), FENCE and its wait; any fault: the
 * PP's frame (the final page) by resync */
static int slide_land(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    uint32_t q[6] = { 0 }, seq, e = bcmjob_epoch(), n = bcmjob_errors();
    int r;

    if ((r = bcmjob_submit(J_OP_ANIM_STOP, 0, q, 1)) == BCMJOB_OK)
    {
        q[0] = J_SURF_BACK; q[1] = J_XY(x, y); q[2] = J_XY(w, h); q[3] = J_SURF_FRONT;
        q[4] = q[1]; q[5] = 0;
        if ((r = bcmjob_submit(J_OP_BLIT, 0, q, 6)) == BCMJOB_OK)
        {
            q[0] = J_SURF_FRONT; q[3] = J_PRES_WAIT;
            if ((r = bcmjob_submit(J_OP_PRESENT, 0, q, 4)) == BCMJOB_OK &&
                (r = bcmjob_fence(&seq)) == BCMJOB_OK)
                r = bcmjob_wait(seq, J_T_FENCE_BASE + J_T_SMALL + J_T_DRAW + J_T_PRESENT);
        }
    }
    if (r == BCMJOB_OK && bcmjob_epoch() == e && bcmjob_errors() == n)
        return 0;
    bcmjob_resync(x, y, w, h);
    return 1;
}

/* ---- DECODE (bcmjob_decode(), P19 19-6) -------------------------------- */
/* A stand-in for jdec (the model links no decoder; jpeg_test.py and
 * decode_image_test.py check jdec itself, and the job against it): a
 * "JPEG" of FF D8, a kind byte ('B' baseline, 'P' progressive, 'C' a
 * broken marker), sw and sh (16 bits each, low first), any body, FF D9.
 * Each output pixel hashes the whole file and its place in the dw x dh
 * picture, so a byte lost, doubled or moved in the ring, a wrong window or
 * a wrong pitch changes the pixels.  As decode-once (jdec.h), an output
 * other than the set's primary (the largest area, the first of equals)
 * also hashes the primary's size: its pixels depend on the set it was
 * decoded with, the primary's (and a one-output set's) don't. */
static int tj_decode(const uint8_t *p, uint32_t len, const struct jdec_out *out,
                     int n, struct jdec_info *info)
{
    uint32_t h = 2166136261u, i, x, y, hk;
    int k, pr = 0;

    if (len < 9 || p[0] != 0xFF || p[1] != 0xD8)
        return JDEC_E_CORRUPT;
    if (p[2] == 'P')
        return JDEC_E_UNSUPPORTED;
    if (p[2] != 'B')
        return JDEC_E_CORRUPT;
    if (p[len - 2] != 0xFF || p[len - 1] != 0xD9)
        return JDEC_E_TRUNCATED;
    for (i = 0; i < len; i++)
        h = (h ^ p[i]) * 16777619u;
    for (k = 1; k < n; k++)
        if ((uint32_t)out[k].dw * out[k].dh > (uint32_t)out[pr].dw * out[pr].dh)
            pr = k;
    for (k = 0; k < n; k++)
    {
        const struct jdec_out *o = &out[k];

        hk = k == pr ? h : (h ^ (out[pr].dw << 16 | out[pr].dh)) * 16777619u;
        for (y = 0; y < o->ch; y++)
            for (x = 0; x < o->cw; x++)
                o->px[y * o->pitch + x] = (uint16_t)(((hk ^ (o->dw << 20 | o->dh << 8)) +
                    (o->cx + x) * 2654435761u + (o->cy + y) * 40503u) >> 7);
    }
    if (info)
    {
        info->sw = (uint16_t)(p[3] | p[4] << 8);
        info->sh = (uint16_t)(p[5] | p[6] << 8);
    }
    return JDEC_OK;
}

static uint8_t *tj_make(uint32_t len, char kind, uint32_t sw, uint32_t sh, bool eoi)
{
    uint8_t *p = malloc(len);
    uint32_t i;

    for (i = 0; i < len; i++)
        p[i] = (uint8_t)(i * 131 + (i >> 9));
    p[0] = 0xFF; p[1] = 0xD8; p[2] = (uint8_t)kind;
    p[3] = sw & 0xFF; p[4] = sw >> 8; p[5] = sh & 0xFF; p[6] = sh >> 8;
    p[len - 2] = eoi ? 0xFF : 0x00;
    p[len - 1] = eoi ? 0xD9 : 0x00;
    return p;
}

/* A source over memory (the shell's: over the file at off), counting its
 * starts, failing its reads past fail_at if set */
struct tsrc
{
    const uint8_t *p;
    uint32_t len, pos, starts, fail_at;
};

static int ts_start(void *c) { ((struct tsrc *)c)->pos = 0; ((struct tsrc *)c)->starts++; return 0; }
static long ts_fetch(void *c, void *buf, uint32_t n)
{
    struct tsrc *t = c;

    if (t->fail_at && t->pos + n > t->fail_at)
        return -1;
    if (n > t->len - t->pos)
        n = t->len - t->pos;
    memcpy(buf, t->p + t->pos, n);
    t->pos += n;
    return n;
}
static int ts_skip(void *c, uint32_t n)
{
    struct tsrc *t = c;

    if (n > t->len - t->pos)
        return -1;
    t->pos += n;
    return 0;
}

static void ts_init(struct jdec_src *src, struct tsrc *t, const uint8_t *p, uint32_t len)
{
    memset(t, 0, sizeof(*t));
    t->p = p;
    t->len = len;
    src->len = len;
    src->start = ts_start;
    src->fetch = ts_fetch;
    src->skip = ts_skip;
    src->ctx = t;
}

static uint8_t thumbs[65536], scratch[32768], dstrip[131072], ddrift[J_BUDGET_DRIFT];
static uint16_t RB[300 * 300], TW[300 * 300], TJ[300 * 300];

static void init_dec(bool offer)
{
    struct jmodel_cfg c;

    memset(&c, 0, sizeof(c));
    c.front = (struct jref_surf){ F, 96, 64, 96 };
    c.back = (struct jref_surf){ B, 96, 64, 96 };
    c.panel = (struct jref_surf){ P, 96, 64, 96 };
    c.pool_mem[J_CLS_THUMB] = thumbs;
    c.pool_bytes[J_CLS_THUMB] = sizeof(thumbs);
    c.pool_mem[J_CLS_SCRATCH] = scratch;
    c.pool_bytes[J_CLS_SCRATCH] = sizeof(scratch);
    c.pool_mem[J_CLS_STRIP] = dstrip;
    c.pool_bytes[J_CLS_STRIP] = sizeof(dstrip);
    c.pool_mem[J_CLS_DRIFT] = ddrift;          /* the panel's fill */
    c.pool_bytes[J_CLS_DRIFT] = sizeof(ddrift);
    c.trace = trace;
    c.pp_fb = PPFB;
    c.decode = offer ? tj_decode : NULL;
    jmodel_init(&c);
    bcmjob_on_epoch(on_epoch);
    nlog = 0;
    jmodel_set_charger(false);
    jmodel_set_lit(true);
    jmodel_advance(JM_COLD_START_MS);
}

/* The art set's outputs (album-render.md section 4.2), from a sw x sh
 * source: the NP box (fit into 142), the thumbnail (fill 55, centre
 * crop), the panel (fill 300, centre crop) */
static struct bcmjob_dec_out art_out(int which, uint32_t sw, uint32_t sh, uint32_t key)
{
    struct bcmjob_dec_out o;
    uint32_t box = which == 0 ? 142 : which == 1 ? 55 : 300;

    memset(&o, 0, sizeof(o));
    if (which == 0)
    {
        o.dw = sw >= sh ? box : (uint16_t)((sw * box + sh / 2) / sh);
        o.dh = sw >= sh ? (uint16_t)((sh * box + sw / 2) / sw) : box;
        o.cw = o.dw;
        o.ch = o.dh;
        o.cls = J_CLS_THUMB;
    }
    else
    {
        /* artcache.c decode_run(): the short side fills, rounded up */
        o.dw = sw >= sh ? (uint16_t)((sw * box + sh - 1) / sh) : box;
        o.dh = sw >= sh ? box : (uint16_t)((sh * box + sw - 1) / sw);
        o.cx = (o.dw - box) / 2;
        o.cy = (o.dh - box) / 2;
        o.cw = o.ch = box;
        o.cls = which == 1 ? J_CLS_THUMB : J_CLS_DRIFT;
    }
    o.key_lo = key;
    o.key_hi = 0xA1000000u + ((uint32_t)which << 24);
    return o;
}

/* The CPU twin's pixels for set[ks] of the set (the stand-in over the
 * file, as jdec_run() over the same source and the same set), into TW;
 * the set's other outputs go to TJ */
static int twin_set(const uint8_t *p, uint32_t len, const struct bcmjob_dec_out *set,
                    int n, int ks)
{
    struct jdec_out j[J_DEC_MAX];
    int k;

    for (k = 0; k < n; k++)
    {
        const struct bcmjob_dec_out *o = &set[k];

        j[k].dw = o->dw; j[k].dh = o->dh; j[k].cx = o->cx; j[k].cy = o->cy;
        j[k].cw = o->cw; j[k].ch = o->ch;
        j[k].px = k == ks ? TW : TJ;
        j[k].pitch = o->cw;
    }
    return tj_decode(p, len, j, n, NULL);
}

static int twin(const uint8_t *p, uint32_t len, const struct bcmjob_dec_out *o)
{
    return twin_set(p, len, o, 1, 0);
}

/* A slot read back in bands of 37 rows into RB: its F16w */
static int read_sum(uint32_t slot, uint32_t cw, uint32_t ch, uint32_t *a, uint32_t *b)
{
    uint32_t y, band = 37, r;

    *a = *b = 0;
    for (y = 0; y < ch; y += band)
    {
        uint32_t h = ch - y < band ? ch - y : band;

        if (bcmjob_read(slot, 0, y, cw, h, RB + y * cw, cw) != BCMJOB_OK)
            return 0;
        for (r = 0; r < h; r++)
            j_f16w(a, b, RB + (y + r) * cw, cw);
    }
    return 1;
}

/* Output k read back, its F16w against the BCM's, and equal to the twin's
 * set[ks] given that set */
static int same_set(const struct bcmjob_dec_res *res, int k, const struct bcmjob_dec_out *set,
                    int n, int ks, const uint8_t *p, uint32_t len)
{
    const struct bcmjob_dec_out *o = &set[ks];
    uint32_t a, b;

    if (twin_set(p, len, set, n, ks) != JDEC_OK ||
        !read_sum(res->out[k].slot, o->cw, o->ch, &a, &b))
        return 0;
    return a == res->out[k].a && b == res->out[k].b &&
           !memcmp(RB, TW, 2u * o->cw * o->ch);
}

/* Output k equal to the twin's single decode of o */
static int same_as_twin(const struct bcmjob_dec_res *res, int k,
                        const struct bcmjob_dec_out *o, const uint8_t *p, uint32_t len)
{
    return same_set(res, k, o, 1, 0, p, len);
}

/* A DECODE record of n outputs was submitted (since nlog = 0) */
static int sent_n(int n)
{
    char want[24];
    int i;

    snprintf(want, sizeof(want), " n=%d", J_DEC_ARGS(n));
    for (i = 0; i < nlog; i++)
        if (!strncmp(log_[i], "bcmjob submit op=0x30", 21) && strstr(log_[i], want))
            return 1;
    return 0;
}

static int decode1(const uint8_t *p, uint32_t len, const struct bcmjob_dec_out *o, int n,
                   uint32_t flags, bool (*cancel)(void *), struct bcmjob_dec_res *res,
                   struct tsrc *ts)
{
    struct jdec_src src;
    struct bcmjob_dec_req q;

    ts_init(&src, ts, p, len);
    memset(&q, 0, sizeof(q));
    q.src = &src;
    q.out = o;
    q.n = n;
    q.flags = flags;
    q.cancel = cancel;
    return bcmjob_decode(&q, res);
}

static uint32_t cancel_at, cut_at, dark_at;
static bool cancel_cb(void *ctx)
{
    (void)ctx;
    if (dark_at && jmodel_now() >= dark_at)
    {
        dark_at = 0;
        jmodel_set_lit(false);
    }
    if (cut_at && jmodel_now() >= cut_at)
    {
        cut_at = 0;
        jmodel_fault(JM_FAULT_CUT, 0);
    }
    return cancel_at && jmodel_now() >= cancel_at;
}

/* bcmjob_upload_rect while a DECODE is in flight: BUSY (bcmjob.h) */
static int rect_during = 1;
static bool rect_cb(void *ctx)
{
    (void)ctx;
    if (rect_during == 1)
        rect_during = bcmjob_upload_rect(J_SURF_BACK, 0, 0, 8, 2, PPFB, 96);
    return false;
}

/* The shell is done with a render's slots (album-render.md section 6) */
static void drop(const struct bcmjob_dec_res *res, int n)
{
    uint32_t p[1];
    int k;

    for (k = 0; k < n; k++)
    {
        p[0] = res->out[k].slot;
        bcmjob_submit(J_OP_FREE, 0, p, 1);
    }
    jmodel_advance(5);
}

static int slot_state(uint32_t slot)
{
    return slot < JREF_SLOTS ? jmodel_ref()->slot[slot].state : -1;
}

static void decode_tests(void)
{
    struct bcmjob_dec_res res;
    struct bcmjob_dec_out o[3];
    struct tsrc ts;
    uint8_t *small = tj_make(24000, 'B', 600, 600, true);
    uint8_t *big = tj_make(100000, 'B', 1280, 720, true);
    uint8_t *wide = tj_make(20000, 'B', 600, 400, true);
    uint32_t e, d;
    int r;

    /* the art set's sizes against the work area (jobs.h J_DEC_WORK_MIN) */
    o[0] = art_out(0, 600, 600, 1);
    o[1] = art_out(1, 600, 400, 1);
    o[2] = art_out(2, 600, 600, 1);
    /* jdec streams its outputs (jdec.h): the NP box, the panel's fill and
       the whole set all inside the minimum */
    CHECK(o[0].dw == 142 && o[0].dh == 142 && o[1].dw == 83 && o[1].cx == 14 &&
          o[2].dw == 300 && o[2].dh == 300 &&
          bcmjob_dec_work(o, 1) == 114172 &&
          bcmjob_dec_work(o, 1) == JDEC_WORK_BASE + JDEC_WORK_ROWS(142) + JDEC_WORK_OUT(142) &&
          bcmjob_dec_work(o + 2, 1) == 137240 &&
          bcmjob_dec_work(o, 2) == bcmjob_dec_work(o, 1) + JDEC_WORK_OUT(83) &&
          bcmjob_dec_work(o, 3) == 137240 + JDEC_WORK_OUT(142) + JDEC_WORK_OUT(83) &&
          bcmjob_dec_work(o, 3) <= J_DEC_WORK_MIN,
          "the art set's work: %u %u %u", bcmjob_dec_work(o, 1), bcmjob_dec_work(o + 2, 1),
          bcmjob_dec_work(o, 3));

    /* the NP box from a file the ring holds: one pass, the twin's pixels */
    init_dec(true);
    o[0] = art_out(0, 600, 600, 0x1001);
    e = jmodel_jobs_done();
    r = decode1(small, 24000, o, 1, 0, NULL, &res, &ts);
    CHECK(r == BCMJOB_OK && res.sw == 600 && res.sh == 600 && res.passes == 1 &&
          res.bytes == 24000 && !res.out[0].hit && res.epoch == bcmjob_epoch() &&
          slot_state(res.out[0].slot) == J_ST_READY && ts.starts == 1,
          "the NP box: %d err %u, %ux%u, passes %u, bytes %u", r, res.err, res.sw, res.sh,
          res.passes, res.bytes);
    CHECK(same_as_twin(&res, 0, o, small, 24000), "the NP box differs from the twin's");
    CHECK(bcmjob_dec_verdict(r, &res) == JDEC_OK && jmodel_jobs_done() > e, "verdict");
    CHECK(logged("bcmjob decode len=24000 n=1") && logged("bcmjob submit op=0x30") &&
          logged("bcmjob decode done st=0 err=0 passes=1 bytes=24000"), "decode traces");
    /* the same album again this epoch: a hit, nothing decoded or streamed */
    nlog = 0;
    r = decode1(small, 24000, o, 1, 0, NULL, &res, &ts);
    CHECK(r == BCMJOB_OK && res.out[0].hit && res.passes == 0 && ts.starts == 0 &&
          !logged("bcmjob submit op=0x30") && same_as_twin(&res, 0, o, small, 24000),
          "a second render of the same album: %d hit %d passes %u", r, res.out[0].hit,
          res.passes);
    drop(&res, 1);

    /* a file longer than the ring: a pass each for the probe and the
       decode, the bytes again in order; the 16:9 box */
    o[0] = art_out(0, 1280, 720, 0x1002);
    r = decode1(big, 100000, o, 1, 0, NULL, &res, &ts);
    CHECK(r == BCMJOB_OK && o[0].dw == 142 && o[0].dh == 80 && res.passes == 2 &&
          res.bytes == 200000 && ts.starts == 2 && same_as_twin(&res, 0, o, big, 100000),
          "a 100 KB file: %d passes %u bytes %u starts %u", r, res.passes, res.bytes,
          ts.starts);
    drop(&res, 1);

    /* the thumbnail: a window of the fill */
    o[1] = art_out(1, 600, 400, 0x1003);
    r = decode1(wide, 20000, o + 1, 1, 0, NULL, &res, &ts);
    CHECK(r == BCMJOB_OK && same_as_twin(&res, 0, o + 1, wide, 20000),
          "the thumbnail's window: %d", r);
    drop(&res, 1);

    /* a frame's rows into BACK while the decode streams: BUSY (its ring
       has the port), never counted; after it, OK */
    o[1] = art_out(1, 600, 400, 0x1013);
    r = decode1(wide, 20000, o + 1, 1, 0, rect_cb, &res, &ts);
    CHECK(r == BCMJOB_OK && rect_during == BCMJOB_BUSY && bcmjob_have(J_OP_BLIT) &&
          bcmjob_upload_rect(J_SURF_BACK, 0, 0, 8, 2, PPFB, 96) == BCMJOB_OK,
          "upload_rect during a decode: %d (decode %d)", rect_during, r);
    drop(&res, 1);

    /* the panel's 300 fill, alone and with the box and thumbnail in one
       job: inside the work area since jdec streams (jdec.h), so sent */
    o[0] = art_out(0, 600, 600, 0x1004);
    o[1] = art_out(1, 600, 600, 0x1004);
    o[2] = art_out(2, 600, 600, 0x1004);
    r = decode1(small, 24000, o + 2, 1, 0, NULL, &res, &ts);
    CHECK(r == BCMJOB_OK && same_as_twin(&res, 0, o + 2, small, 24000),
          "the panel's fill: %d err %u", r, res.err);
    drop(&res, 1);
    o[2].key_lo = 0x1005;
    nlog = 0;
    r = decode1(small, 24000, o, 3, 0, NULL, &res, &ts);
    CHECK(r == BCMJOB_OK && res.miss_n == 3 && res.miss[0] == 0 && res.miss[1] == 1 &&
          res.miss[2] == 2 && sent_n(3) && same_set(&res, 0, o, 3, 0, small, 24000) &&
          same_set(&res, 1, o, 3, 1, small, 24000) &&
          same_set(&res, 2, o, 3, 2, small, 24000),
          "the art set in one job: %d err %u", r, res.err);
    /* the stand-in is decode-once: the box of the set is not its single
       decode (so a wrong set shows) */
    CHECK(twin_set(small, 24000, o, 3, 0) == JDEC_OK && memcpy(RB, TW, 2 * 142 * 142) &&
          twin(small, 24000, o) == JDEC_OK && memcmp(RB, TW, 2 * 142 * 142),
          "the stand-in's box doesn't depend on its set");
    /* the same set again: every output a hit, nothing sent or streamed */
    nlog = 0;
    r = decode1(small, 24000, o, 3, 0, NULL, &res, &ts);
    CHECK(r == BCMJOB_OK && res.out[0].hit && res.out[1].hit && res.out[2].hit &&
          !res.miss_n && !res.passes && !ts.starts && !res.sw &&
          !logged("bcmjob submit op=0x30") && same_set(&res, 2, o, 3, 2, small, 24000),
          "the art set again: %d, misses %u passes %u", r, res.miss_n, res.passes);
    drop(&res, 3);

    /* one hit of three, the largest (the panel, from another file): the
       DECODE gets the box and the thumbnail, each equal to the twin given
       those two (the box the primary now); the hit's pixels untouched */
    {
        struct bcmjob_dec_out set[J_DEC_MAX];
        uint32_t ha, hb, a2, b2, hs;

        o[0] = art_out(0, 600, 600, 0x1008);
        o[1] = art_out(1, 600, 600, 0x1008);
        o[2] = art_out(2, 600, 600, 0x1008);
        r = decode1(wide, 20000, o + 2, 1, 0, NULL, &res, &ts);
        hs = res.out[0].slot;
        CHECK(r == BCMJOB_OK && read_sum(hs, 300, 300, &ha, &hb) && ha == res.out[0].a &&
              hb == res.out[0].b, "the held panel: %d", r);
        nlog = 0;
        r = decode1(small, 24000, o, 3, 0, NULL, &res, &ts);
        CHECK(r == BCMJOB_OK && !res.out[0].hit && !res.out[1].hit && res.out[2].hit &&
              res.out[2].slot == hs && res.miss_n == 2 && res.miss[0] == 0 &&
              res.miss[1] == 1 && bcmjob_dec_set(o, &res, set) == 2 && sent_n(2) &&
              !sent_n(3) && same_set(&res, 0, set, 2, 0, small, 24000) &&
              same_set(&res, 1, set, 2, 1, small, 24000),
              "one hit of three: %d err %u, misses %u", r, res.err, res.miss_n);
        CHECK(read_sum(hs, 300, 300, &a2, &b2) && a2 == ha && b2 == hb &&
              res.out[2].a == ha && res.out[2].b == hb && slot_state(hs) == J_ST_READY &&
              same_as_twin(&res, 2, o + 2, wide, 20000),
              "the hit's pixels changed: %08x %08x, was %08x %08x", a2, b2, ha, hb);
        /* the full set's thumbnail differs from the reduced set's: a
           driver that sent the hit too would fail above */
        CHECK(twin_set(small, 24000, o, 3, 1) == JDEC_OK && memcpy(RB, TW, 2 * 55 * 55) &&
              twin_set(small, 24000, set, 2, 1) == JDEC_OK && memcmp(RB, TW, 2 * 55 * 55),
              "the reduced set's thumbnail is the full set's");
        drop(&res, 3);

        /* a failed decode (a broken file) and a cancelled one with a hit:
           the hit READY with its pixels, the miss freed */
        {
            uint8_t *brk = tj_make(24000, 'C', 600, 600, true);
            int rnd;

            o[0] = art_out(0, 600, 600, 0x1009);
            o[1] = art_out(1, 600, 600, 0x1009);
            r = decode1(small, 24000, o, 1, 0, NULL, &res, &ts);
            hs = res.out[0].slot;
            CHECK(r == BCMJOB_OK && read_sum(hs, 142, 142, &ha, &hb), "the held box: %d", r);
            for (rnd = 0; rnd < 2; rnd++)
            {
                uint32_t ms;

                if (rnd)
                    cancel_at = jmodel_now() + 3;
                r = rnd ? decode1(big, 100000, o, 2, 0, cancel_cb, &res, &ts) :
                          decode1(brk, 24000, o, 2, 0, NULL, &res, &ts);
                cancel_at = 0;
                ms = res.out[1].slot;
                jmodel_advance(5);
                CHECK(r == (rnd ? BCMJOB_ABORTED : BCMJOB_FAILED) && res.out[0].hit &&
                      res.out[0].slot == hs && res.miss_n == 1 && res.miss[0] == 1 &&
                      slot_state(hs) == J_ST_READY && slot_state(ms) == J_ST_EMPTY &&
                      read_sum(hs, 142, 142, &a2, &b2) && a2 == ha && b2 == hb,
                      "a %s decode with a hit: %d, hit %d, states %d %d, sums %08x/%08x",
                      rnd ? "cancelled" : "failed", r, res.out[0].hit, slot_state(hs),
                      slot_state(ms), a2, ha);
            }
            free(brk);
            res.out[0].slot = hs;
            drop(&res, 1);
        }

        /* the work check is on the reduced set: the 16:9 art set (184,428
           B, the minimum) and a fourth output over it; with the fourth a
           hit the rest is decoded, with it a miss NOMEM after the ALLOCs,
           no DECODE sent, the misses freed, the hit kept */
        o[0] = art_out(0, 1280, 720, 0x100A);
        o[1] = art_out(1, 1280, 720, 0x100A);
        o[2] = art_out(2, 1280, 720, 0x100A);
        memset(&set[0], 0, sizeof(set[0]));
        set[0].dw = set[0].dh = 300;
        set[0].cw = set[0].ch = 100;
        set[0].cls = J_CLS_STRIP;
        set[0].key_lo = 0x100A;
        set[0].key_hi = 0xA4000000u;
        {
            struct bcmjob_dec_out four[4] = { o[0], o[1], o[2], set[0] };
            uint32_t s4, d0 = jmodel_ref()->done;

            CHECK(bcmjob_dec_work(four, 3) == J_DEC_WORK_MIN &&
                  bcmjob_dec_work(four, 4) > J_DEC_WORK_MIN, "the fourth's work: %u %u",
                  bcmjob_dec_work(four, 3), bcmjob_dec_work(four, 4));
            nlog = 0;
            e = bcmjob_errors();
            r = decode1(big, 100000, four, 4, 0, NULL, &res, &ts);
            jmodel_advance(5);
            CHECK(r == BCMJOB_FAILED && res.err == HP_E_NOMEM && res.miss_n == 4 &&
                  !logged("bcmjob submit op=0x30") && ts.starts == 0 &&
                  slot_state(res.out[0].slot) == J_ST_EMPTY &&
                  slot_state(res.out[3].slot) == J_ST_EMPTY && bcmjob_errors() == e &&
                  bcmjob_have(J_OP_DECODE) && bcmjob_dec_verdict(r, &res) == BCMJOB_DEC_TWIN &&
                  jmodel_ref()->done > d0,
                  "four misses over the work area: %d err %u", r, res.err);
            r = decode1(wide, 20000, four + 3, 1, 0, NULL, &res, &ts);
            s4 = res.out[0].slot;
            CHECK(r == BCMJOB_OK, "the fourth alone: %d", r);
            nlog = 0;
            r = decode1(big, 100000, four, 4, 0, NULL, &res, &ts);
            CHECK(r == BCMJOB_OK && res.out[3].hit && res.out[3].slot == s4 &&
                  res.miss_n == 3 && sent_n(3) && same_set(&res, 0, four, 3, 0, big, 100000) &&
                  same_set(&res, 1, four, 3, 1, big, 100000) &&
                  same_set(&res, 2, four, 3, 2, big, 100000) &&
                  same_set(&res, 3, four + 3, 1, 0, wide, 20000),
                  "fits once the hit is dropped: %d err %u", r, res.err);
            drop(&res, 4);
        }
    }

    /* past the work area: refused before anything is sent (a 16:1 cover's
       fill, 4,800 x 300), the twin's to do */
    nlog = 0;
    e = jmodel_ref()->done;
    o[2] = art_out(2, 9600, 600, 0x1006);
    r = decode1(small, 24000, o + 2, 1, 0, NULL, &res, &ts);
    CHECK(o[2].dw == 4800 && bcmjob_dec_work(o + 2, 1) > J_DEC_WORK_MIN &&
          r == BCMJOB_FAILED && res.err == HP_E_NOMEM && jmodel_ref()->done == e &&
          ts.starts == 0 && !logged("bcmjob submit") && bcmjob_have(J_OP_DECODE) &&
          bcmjob_dec_verdict(r, &res) == BCMJOB_DEC_TWIN,
          "a panorama's fill: %d err %u", r, res.err);
    /* a window outside its picture: never sent */
    o[0].cx = 1;
    r = decode1(small, 24000, o, 1, 0, NULL, &res, &ts);
    CHECK(r == BCMJOB_FAILED && res.err == J_E_RANGE && jmodel_ref()->done == e,
          "a window past dw: %d err %u", r, res.err);
    o[0].cx = 0;

    /* the source's verdicts, the same as the twin's; never counted
       toward the failed mark; the outputs freed */
    {
        static const struct { char kind; bool eoi; uint32_t err; int verdict; } v[] =
        {
            { 'P', true,  J_E_DEC_UNSUPPORTED, BCMJOB_DEC_TWIN },
            { 'C', true,  J_E_DEC_CORRUPT,     JDEC_E_CORRUPT },
            { 'B', false, J_E_DEC_TRUNCATED,   JDEC_E_TRUNCATED },
        };
        uint32_t i, rnd;

        for (rnd = 0; rnd < 2; rnd++)
            for (i = 0; i < 3; i++)
            {
                uint8_t *f = tj_make(40000, v[i].kind, 600, 600, v[i].eoi);
                struct jdec_out j = { 142, 142, 0, 0, 142, 142, TW, 142 };
                uint32_t s;

                o[0] = art_out(0, 600, 600, 0x2000 + i);
                r = decode1(f, 40000, o, 1, 0, NULL, &res, &ts);
                s = res.out[0].slot;
                jmodel_advance(5);
                CHECK(r == BCMJOB_FAILED && res.err == v[i].err && !res.stalled &&
                      bcmjob_dec_verdict(r, &res) == v[i].verdict &&
                      (v[i].verdict == BCMJOB_DEC_TWIN ||
                       tj_decode(f, 40000, &j, 1, NULL) == v[i].verdict) &&
                      slot_state(s) == J_ST_EMPTY,
                      "kind %c: %d err %u, verdict %d, slot %d", v[i].kind, r, res.err,
                      bcmjob_dec_verdict(r, &res), slot_state(s));
                free(f);
            }
        CHECK(bcmjob_have(J_OP_DECODE), "the source's errors marked DECODE failed");
    }
    /* the PP's read fails: J_DEC_ABORT, CORRUPT as the twin's read error */
    {
        struct jdec_src src;
        struct bcmjob_dec_req q;

        o[0] = art_out(0, 1280, 720, 0x2100);
        ts_init(&src, &ts, big, 100000);
        ts.fail_at = 50000;
        memset(&q, 0, sizeof(q));
        q.src = &src;
        q.out = o;
        q.n = 1;
        r = bcmjob_decode(&q, &res);
        CHECK(r == BCMJOB_FAILED && res.err == J_E_DEC_CORRUPT && res.read_err &&
              res.bytes < 50000 && bcmjob_dec_verdict(r, &res) == JDEC_E_CORRUPT,
              "a read error: %d err %u read_err %d", r, res.err, res.read_err);
    }

    /* called off while streaming: J_DEC_ABORT, the job ends CORRUPT on the
       BCM, the call answers ABORTED, the outputs go; the next decode is
       fine */
    o[0] = art_out(0, 1280, 720, 0x3000);
    nlog = 0;
    cancel_at = jmodel_now() + 3;
    r = decode1(big, 100000, o, 1, 0, cancel_cb, &res, &ts);
    cancel_at = 0;
    jmodel_advance(5);
    CHECK(r == BCMJOB_ABORTED && res.bytes < 100000 && slot_state(res.out[0].slot) == J_ST_EMPTY &&
          bcmjob_dec_verdict(r, &res) == BCMJOB_DEC_DEFER && logged("bcmjob fault op=0x30 why=aborted") &&
          bcmjob_have(J_OP_DECODE), "a cancel mid-stream: %d, %u bytes", r, res.bytes);
    /* called off once the whole file is in the ring: it runs to its end,
       waited for, and is dropped */
    o[0] = art_out(0, 600, 600, 0x3001);
    cancel_at = jmodel_now() + 4;
    d = jmodel_ref()->done;
    e = bcmjob_errors();
    r = decode1(small, 24000, o, 1, 0, cancel_cb, &res, &ts);
    cancel_at = 0;
    CHECK(r == BCMJOB_ABORTED && res.bytes == 24000 && jmodel_cur_op() == 0 &&
          jmodel_ref()->done == d + 2 && bcmjob_errors() == e,
          "a cancel after the stream: %d, cur op %02x, done +%u", r, jmodel_cur_op(),
          jmodel_ref()->done - d);
    o[0] = art_out(0, 1280, 720, 0x3002);
    r = decode1(big, 100000, o, 1, 0, NULL, &res, &ts);
    CHECK(r == BCMJOB_OK && same_as_twin(&res, 0, o, big, 100000), "after a cancel: %d", r);
    drop(&res, 1);

    /* a cut mid-decode: OFF, a new epoch, the slots gone; after the
       start the ring is allocated again and a decode runs */
    e = bcmjob_epoch();
    o[0] = art_out(0, 1280, 720, 0x4000);
    cut_at = jmodel_now() + 6;
    r = decode1(big, 100000, o, 1, 0, cancel_cb, &res, &ts);
    CHECK(r == BCMJOB_OFF && bcmjob_epoch() != e && epochs_seen == bcmjob_epoch() &&
          bcmjob_dec_verdict(r, &res) == BCMJOB_DEC_DEFER, "a cut mid-decode: %d", r);
    r = decode1(big, 100000, o, 1, 0, NULL, &res, &ts);
    CHECK(r == BCMJOB_OFF, "a decode inside the cold start: %d", r);
    jmodel_advance(JM_COLD_START_MS);
    r = decode1(big, 100000, o, 1, 0, NULL, &res, &ts);
    CHECK(r == BCMJOB_OK && !res.out[0].hit && same_as_twin(&res, 0, o, big, 100000),
          "a decode after the cut: %d", r);
    drop(&res, 1);
    /* the screen goes dark mid-decode on battery: cut, OFF */
    o[0] = art_out(0, 1280, 720, 0x4001);
    dark_at = jmodel_now() + 6;
    r = decode1(big, 100000, o, 1, 0, cancel_cb, &res, &ts);
    CHECK(r == BCMJOB_OFF && jmodel_state() == JM_C0 && bcmjob_dark_ups(0) == 0,
          "dark mid-decode on battery: %d", r);
    jmodel_set_lit(true);
    jmodel_advance(JM_COLD_START_MS);

    /* power, D-3: dark on battery REFUSED (the CPU twin); DECODE never
       powers the BCM, so dark on the charger, with or without NO_WAKE,
       answers OFF (DEFER: retry when lit), nothing powered or counted */
    jmodel_set_lit(false);
    jmodel_advance(10);
    o[0] = art_out(0, 600, 600, 0x5000);
    nlog = 0;
    r = decode1(small, 24000, o, 1, 0, NULL, &res, &ts);
    CHECK(r == BCMJOB_REFUSED && jmodel_state() == JM_C0 && bcmjob_dark_ups(0) == 0 &&
          logged("bcmjob fault op=0x30 why=refused") && ts.starts == 0 &&
          bcmjob_dec_verdict(r, &res) == BCMJOB_DEC_TWIN, "dark on battery: %d", r);
    jmodel_set_charger(true);
    r = decode1(small, 24000, o, 1, BCMJOB_DEC_NO_WAKE, NULL, &res, &ts);
    CHECK(r == BCMJOB_OFF && jmodel_state() == JM_C0 && bcmjob_dark_ups(0) == 0 &&
          bcmjob_dec_verdict(r, &res) == BCMJOB_DEC_DEFER,
          "dark, the charger, lit only: %d", r);
    r = decode1(small, 24000, o, 1, 0, NULL, &res, &ts);
    CHECK(r == BCMJOB_OFF && jmodel_state() == JM_C0 && bcmjob_dark_ups(0) == 0 &&
          ts.starts == 0 && bcmjob_dec_verdict(r, &res) == BCMJOB_DEC_DEFER,
          "dark on the charger powered the BCM for a decode: %d, ups %u", r,
          bcmjob_dark_ups(0));
    jmodel_set_charger(false);

    /* the image lacks DECODE (the merged image), or it is cleared */
    init_dec(false);
    r = decode1(small, 24000, o, 1, 0, NULL, &res, &ts);
    CHECK(r == BCMJOB_NOJOB && bcmjob_dec_verdict(r, &res) == BCMJOB_DEC_TWIN,
          "no DECODE in the image: %d", r);
    init_dec(true);
    jmodel_fault(JM_FAULT_CLEAR_OP, J_OP_DECODE);
    CHECK(decode1(small, 24000, o, 1, 0, NULL, &res, &ts) == BCMJOB_NOJOB, "a cleared DECODE");
    jmodel_fault(JM_FAULT_NONE, 0);

    /* a full command ring: BUSY at once */
    jmodel_fault(JM_FAULT_RING_FULL, 1);
    r = decode1(small, 24000, o, 1, 0, NULL, &res, &ts);
    jmodel_advance(5);
    CHECK(r == BCMJOB_BUSY && slot_state(res.out[0].slot) == J_ST_EMPTY, "ring full: %d", r);

    /* slow: TIMEOUT past the deadline, marked on the second miss */
    jmodel_fault(JM_FAULT_SLOW_OP, J_OP_DECODE);
    o[0] = art_out(0, 600, 600, 0x6000);
    r = decode1(small, 24000, o, 1, 0, NULL, &res, &ts);
    CHECK(r == BCMJOB_TIMEOUT && bcmjob_have(J_OP_DECODE) &&
          logged("bcmjob fault op=0x30 why=timeout"), "a slow decode: %d", r);
    jmodel_advance(100000);
    r = decode1(small, 24000, o, 1, 0, NULL, &res, &ts);
    CHECK(r == BCMJOB_TIMEOUT && !bcmjob_have(J_OP_DECODE) &&
          decode1(small, 24000, o, 1, 0, NULL, &res, &ts) == BCMJOB_NOJOB,
          "a slow decode not marked on its second miss: %d", r);
    jmodel_fault(JM_FAULT_NONE, 0);

    /* a ring DECODE nobody feeds: the BCM gives up after J_DEC_STALL_MS,
       CORRUPT, not counted */
    init_dec(true);
    {
        struct bcmjob_slot rs, os;
        uint32_t p[8], seq = 0, errs;

        bcmjob_slot(J_CLS_SCRATCH, 128, 128, BCMJOB_DEC_RING_KEY, 0, &rs);
        p[0] = rs.slot; p[1] = 0; p[2] = 0; p[3] = J_COMMIT_NOCHECK;
        bcmjob_submit(J_OP_COMMIT, 0, p, 4);
        bcmjob_slot(J_CLS_THUMB, 142, 142, 0x7000, 0, &os);
        p[0] = rs.slot | J_DEC_RING; p[1] = 24000; p[2] = 1;
        p[3] = J_XY(142, 142); p[4] = 0; p[5] = J_XY(142, 142); p[6] = os.slot; p[7] = 0;
        errs = bcmjob_errors();
        CHECK(bcmjob_submit(J_OP_DECODE, 0, p, 8) == BCMJOB_OK && bcmjob_fence(&seq) == BCMJOB_OK,
              "a bare DECODE");
        jmodel_advance(J_DEC_STALL_MS - 10);
        CHECK(jmodel_cur_op() == J_OP_DECODE, "J_CUR_OP %02x while the ring waits",
              jmodel_cur_op());
        CHECK(bcmjob_wait(seq, 100) == BCMJOB_OK && bcmjob_errors() == errs + 1 &&
              (jmodel_ref()->last_err & 0xFF) == J_OP_DECODE &&
              (jmodel_ref()->last_err >> 8 & 0xFF) == J_E_DEC_CORRUPT && bcmjob_have(J_OP_DECODE),
              "a stalled ring: errors %u, last %08x", bcmjob_errors() - errs,
              jmodel_ref()->last_err);
    }
    free(small);
    free(big);
    free(wide);
}

/* ---- the visualizer (proposed, sim only) ----------------------- */
#ifdef JREF_VIZ
/* SPECTRUM through the model equals the CPU path's levels (jviz_power_ref
 * and jviz_bands); BARS on the front frame equals jviz_bars_draw on a
 * copy; bad records fail as jobs.h says */
static void viz_tests(void)
{
    static int16_t pcm[512];
    static uint32_t pw[256];
    static uint16_t want_fb[96 * 64], flip_fb[96 * 64];
    struct bcmjob_slot s;
    uint16_t edges[25], l2[24], want[24], *px;
    uint32_t p[HP_MAX_PAYLOAD], i, n, seq;
    struct jviz_bars b;
    uint8_t ht[12], pk[12];
    int r;

    init();
    jmodel_set_lit(true);
    jmodel_advance(JM_COLD_START_MS + 10);
    CHECK(bcmjob_have(J_OP_SPECTRUM) && bcmjob_have(J_OP_BARS), "the model lacks the viz ops");
    CHECK(bcmjob_slot(J_CLS_STRIP, 32, 16, 0x5EC, 0, &s) == BCMJOB_OK, "the pcm slot");
    px = jref_slot_px(jmodel_ref(), s.slot);
    p[0] = s.slot; p[1] = 0; p[2] = 0; p[3] = J_COMMIT_NOCHECK;
    CHECK(px && bcmjob_submit(J_OP_COMMIT, 0, p, 4) == BCMJOB_OK, "commit");
    jmodel_advance(5);
    for (i = 0; i <= 24; i++)
        edges[i] = (uint16_t)(1 + i * i * 255 / 576);      /* to bin 256 */
    for (n = 256; n <= 512; n += 256)
    {
        /* a two-tone in split order: x[2k] then x[2k + 1] */
        for (i = 0; i < n; i++)
        {
            uint32_t t = (i & 1 ? n / 2 : 0) + i / 2;
            pcm[t] = (int16_t)(((i * 2654435761u) >> 20) - 2048 + ((i * 37) % 64) * 300);
            px[t] = (uint16_t)pcm[t];
        }
        if (n == 256)                   /* 12 bands to bin 128 */
            for (i = 0; i <= 12; i++)
                edges[i] = (uint16_t)(edges[2 * i] / 2);
        r = bcmjob_spectrum(s.slot, n, edges, n == 512 ? 24 : 12, l2);
        jviz_power_ref(pw, pcm, n);
        jviz_bands(want, pw, n / 2, edges, n == 512 ? 24 : 12);
        CHECK(r == BCMJOB_OK && !memcmp(l2, want, 2 * (n == 512 ? 24 : 12)),
              "SPECTRUM %u: %d, band 3 %u want %u", n, r, l2[3], want[3]);
        for (i = 0; i <= 24; i++)
            edges[i] = (uint16_t)(1 + i * i * 255 / 576);
    }
    edges[5] = 300;                                     /* past n / 2 */
    CHECK(bcmjob_spectrum(s.slot, 512, edges, 24, l2) == BCMJOB_FAILED, "bad edges taken");
    CHECK(bcmjob_spectrum(s.slot, 384, edges, 4, l2) == BCMJOB_FAILED, "n 384 taken");

    /* BARS: 12 bars, a gradient, caps, rounded tops, on the front frame */
    for (i = 0; i < 12; i++)
    {
        ht[i] = (uint8_t)(i * 7 % 61);
        pk[i] = (uint8_t)(ht[i] + 3 + i % 4);
    }
    memset(p, 0, sizeof(p));
    p[0] = J_SURF_FRONT; p[1] = J_XY(2, 1); p[2] = J_XY(90, 60);
    p[3] = 12 | 5 << 8 | 2 << 16 | 2u << 24;
    p[4] = 0x001F | 0xFFE0u << 16; p[5] = 0xF800 | 0x0841u << 16;
    p[6] = J_BARS_BG | J_BARS_CAP | J_BARS_ROUND;
    for (i = 0; i < 12; i++)
    {
        p[7 + i / 4] |= (uint32_t)ht[i] << (8 * (i & 3));
        p[10 + i / 4] |= (uint32_t)pk[i] << (8 * (i & 3));
    }
    memcpy(want_fb, F, sizeof(F));
    b.n = 12; b.barw = 5; b.gap = 2; b.cap_h = 2; b.flags = p[6];
    b.bot = 0x001F; b.top = 0xFFE0; b.cap = 0xF800; b.bg = 0x0841;
    b.height = ht; b.peak = pk;
    jviz_bars_draw(want_fb + 96 + 2, 96, 90, 60, &b, 0);
    CHECK(bcmjob_submit(J_OP_BARS, 0, p, J_BARS_ARGS(12)) == BCMJOB_OK &&
          bcmjob_fence(&seq) == BCMJOB_OK && bcmjob_wait(seq, 100) == BCMJOB_OK &&
          !memcmp(F, want_fb, sizeof(F)), "BARS differs from jviz_bars_draw");
    /* J_BARS_FLIP (the Mirror's reflection): the frame above, its 60 rows
     * upside down in the same rect, the rest of the panel untouched */
    p[6] |= J_BARS_FLIP;
    for (i = 0; i < 60; i++)
        memcpy(flip_fb + 96 * (1 + i), want_fb + 96 * (60 - i), 2 * 96);
    memcpy(want_fb, F, sizeof(F));
    CHECK(bcmjob_submit(J_OP_BARS, 0, p, J_BARS_ARGS(12)) == BCMJOB_OK &&
          bcmjob_fence(&seq) == BCMJOB_OK && bcmjob_wait(seq, 100) == BCMJOB_OK,
          "flipped BARS refused");
    for (i = 0; i < 60; i++)
        memcpy(want_fb + 96 * (1 + i) + 2, flip_fb + 96 * (1 + i) + 2, 2 * 90);
    CHECK(!memcmp(F, want_fb, sizeof(F)), "flipped BARS is not the frame upside down");
    p[6] &= ~J_BARS_FLIP;
    p[3] = 12 | 9 << 8 | 2u << 16;                      /* 130 px: too wide */
    i = jmodel_ref()->errors;
    bcmjob_submit(J_OP_BARS, 0, p, J_BARS_ARGS(12));
    jmodel_advance(10);
    CHECK(jmodel_ref()->errors == i + 1 &&
          (jmodel_ref()->last_err & 0xFFFF) == (J_OP_BARS | J_E_RANGE << 8),
          "too-wide BARS: last_err %x", jmodel_ref()->last_err);
}
#else
static void viz_tests(void)
{
}
#endif

int main(void)
{
    struct bcmjob_slot s, s2;
    uint32_t p[8], i, e;
    int r;

    /* dark: nothing is up, nothing is sent */
    init();
    jmodel_set_lit(false);
    CHECK(!bcmjob_have(J_OP_FILL) && fill_present(1) == BCMJOB_OFF, "jobs while dark");
    CHECK(jmodel_state() == JM_C0, "dark is not C0");
    CHECK(logged("bcmjob fault op=0x25 why=off"), "no 'off' trace");

    /* lit: a cold start, then jobs run in order to the panel */
    jmodel_set_lit(true);
    CHECK(jmodel_cold_starts() == 1 && epochs_seen == bcmjob_epoch() && epochs_seen != 0,
          "cold start %u, epoch %u/%u", jmodel_cold_starts(), epochs_seen, bcmjob_epoch());
    CHECK(logged("bcm power state=C1 why=lit"), "no power trace");
    /* a cold start (jmodel's C1, before the HELLO): every job answers
     * OFF at once, nothing queued to run late (bcmjob.h BCMJOB_OFF) */
    e = jmodel_jobs_done();
    r = fill_present(0x1234);
    CHECK(r == BCMJOB_OFF && !bcmjob_have(J_OP_FILL) && jmodel_state() == JM_C1,
          "a frame inside the 400 ms cold start: %d", r);
    CHECK(bcmjob_slot(J_CLS_DRIFT, 48, 32, 0xAB, 0, &s) == BCMJOB_OFF,
          "an ALLOC inside the cold start");
    jmodel_advance(JM_COLD_START_MS - 1);
    CHECK(bcmjob_submit(J_OP_FILL, 0, p, 0) == BCMJOB_OFF, "a job 1 ms before the start ends");
    jmodel_advance(1);
    CHECK(jmodel_jobs_done() == e && bcmjob_have(J_OP_FILL),
          "the start: %u jobs ran late", jmodel_jobs_done() - e);
    jmodel_fault(JM_FAULT_NONE, 0);
    CHECK(fill_present(0x1234) == BCMJOB_OK && P[0] == 0x1234 && P[96 * 64 - 1] == 0x1234,
          "fill and present: %04x", P[0]);
    CHECK(logged("bcmjob submit op=0x25 tag=0 n=4") && logged("bcmjob fence seq="),
          "submit/fence traces");
    jmodel_advance(50);
    CHECK(jmodel_state() == JM_C2, "idle is not C2");

    /* a full ring answers busy at once (gap G1) */
    p[0] = J_SURF_FRONT; p[1] = 0; p[2] = J_XY(96, 64); p[3] = 7;
    e = jmodel_now();
    for (i = 0; i < 1000 && (r = bcmjob_submit(J_OP_FILL, 0, p, 4)) == BCMJOB_OK; i++)
        ;
    CHECK(r == BCMJOB_BUSY && jmodel_now() == e, "ring full: %d after %u, time moved %u",
          r, i, jmodel_now() - e);
    CHECK(logged("bcmjob fault op=0x25 why=busy"), "no busy trace");
    jmodel_advance(5000);

    /* slots: upload once, a hit the next time, gone after a cut */
    for (i = 0; i < 48 * 32; i++)
        PIC[i] = (uint16_t)(i * 7);
    CHECK(bcmjob_slot(J_CLS_DRIFT, 48, 32, 0xAB, 0, &s) == BCMJOB_OK && !s.hit, "alloc");
    CHECK(bcmjob_upload(&s, PIC) == BCMJOB_OK, "upload");
    CHECK(bcmjob_slot(J_CLS_DRIFT, 48, 32, 0xAB, 0, &s2) == BCMJOB_OK && s2.hit &&
          s2.slot == s.slot, "no hit for the same key");
    p[0] = s.slot; p[1] = J_XY(0, 0); p[2] = J_XY(48, 32); p[3] = J_SURF_FRONT;
    p[4] = J_XY(10, 10); p[5] = 0;
    CHECK(bcmjob_submit(J_OP_BLIT, 0, p, 6) == BCMJOB_OK, "blit");
    e = bcmjob_epoch();
    jmodel_fault(JM_FAULT_CUT, 0);
    CHECK(bcmjob_epoch() != e && epochs_seen == bcmjob_epoch() && jmodel_cold_starts() == 2,
          "a cut while lit: epoch %u -> %u, starts %u", e, bcmjob_epoch(),
          jmodel_cold_starts());
    jmodel_advance(JM_COLD_START_MS);
    CHECK(bcmjob_slot(J_CLS_DRIFT, 48, 32, 0xAB, 0, &s2) == BCMJOB_OK && !s2.hit,
          "a slot survived the cut");

    /* J_CUR_OP while a job runs, J_DONE for failures too, ALLOC's address
     * in the BCM's map, a drift slot's 16-pixel pitch, COMMIT's deadline */
    {
        struct jref *ref = jmodel_ref();
        uint32_t q[5], rep[HP_MAX_PAYLOAD + 1], nrep, done = ref->done, errs = ref->errors;
        uint32_t rec[6], k, x, y;
        int ok;

        jmodel_advance(50);
        q[0] = J_SURF_FRONT; q[1] = 0; q[2] = J_XY(96, 64); q[3] = 5;
        bcmjob_submit(J_OP_FILL, 0, q, 4);
        jmodel_advance(1);
        CHECK(jmodel_cur_op() == J_OP_FILL, "J_CUR_OP %02x while a FILL runs", jmodel_cur_op());
        jmodel_advance(10);
        CHECK(jmodel_cur_op() == 0, "J_CUR_OP %02x between jobs", jmodel_cur_op());
        q[1] = J_XY(90, 0); q[2] = J_XY(8, 1);          /* off the frame */
        bcmjob_submit(J_OP_FILL, 0, q, 4);
        jmodel_advance(10);
        CHECK(ref->done == done + 2 && ref->errors == errs + 1,
              "J_DONE %u (+%u), J_ERRORS +%u: a failed job not counted", ref->done,
              ref->done - done, ref->errors - errs);
        rec[0] = HP_REC(J_OP_ALLOC, 5, J_TAG_REPLY);
        rec[1] = J_CLS_DRIFT; rec[2] = J_XY(40, 8); rec[3] = J_FMT_RGB565;
        rec[4] = 0xD1; rec[5] = 0;
        CHECK(jref_exec(ref, rec, rep, &nrep) == HP_OK && rep[3] == 2 * 48 &&
              rep[2] == 0x35400 + (uint32_t)((uint8_t *)jref_slot_px(ref, rep[1]) -
                                             (uint8_t *)drift),
              "ALLOC: addr %x pitch %u (the merged image's pool, J_SLOT_PITCH 48)",
              rep[2], rep[3]);
        CHECK(bcmjob_slot(J_CLS_DRIFT, 40, 8, 0xD2, 0, &s2) == BCMJOB_OK &&
              bcmjob_upload(&s2, PIC) == BCMJOB_OK, "a 40-wide drift slot");
        rec[0] = s2.slot; rec[1] = 0; rec[2] = J_XY(40, 8); rec[3] = J_SURF_FRONT;
        rec[4] = J_XY(3, 2); rec[5] = 0;
        bcmjob_submit(J_OP_BLIT, 0, rec, 6);
        jmodel_advance(10);
        for (ok = 1, y = 0; y < 8; y++)
            for (x = 0; x < 40; x++)
                ok &= F[(2 + y) * 96 + 3 + x] == PIC[y * 40 + x];
        CHECK(ok, "the upload at a 48-pixel pitch, blitted back, differs");
        k = J_T_COMMIT(J_BUDGET_DRIFT / 2);
        CHECK(J_SLOT_PITCH(J_CLS_DRIFT, 300) == J_DRIFT_PITCH && k > 2 * 40 && k < 120 &&
              J_T_COMMIT(1024) == J_T_SMALL + 1, "COMMIT's deadline for a cover: %u ms", k);
    }

    /* jmodel_fail_next (the developer's forced failure): the next n jobs
     * of an op fail, then it runs again; jmodel_forgive clears the failed
     * mark of BCMJOB_FAIL_MARK failures */
    {
        uint32_t q[4] = { J_SURF_FRONT, J_XY(0, 0), J_XY(8, 1), 0 }, seq, n0;
        int i;

        jmodel_advance(50);
        n0 = bcmjob_errors();
        jmodel_fail_next(J_OP_SCROLL, 2);
        for (i = 0; i < 2; i++)
        {
            CHECK(bcmjob_submit(J_OP_SCROLL, 0, q, 4) == BCMJOB_OK &&
                  bcmjob_fence(&seq) == BCMJOB_OK && bcmjob_wait(seq, 50) == BCMJOB_OK,
                  "forced failure: job %d did not retire", i);
        }
        CHECK(bcmjob_errors() == n0 + 2, "forced failure: %u failed of 2 armed",
              bcmjob_errors() - n0);
        CHECK(!bcmjob_have(J_OP_SCROLL) &&
              bcmjob_submit(J_OP_SCROLL, 0, q, 4) == BCMJOB_NOJOB,
              "two failures did not mark SCROLL failed");
        jmodel_fail_next(J_OP_SCROLL, 0);
        jmodel_forgive(J_OP_SCROLL);
        CHECK(bcmjob_have(J_OP_SCROLL), "forgiving did not clear SCROLL's mark");
    }

    /* J_ERRORS (bcmjob_errors): a failed job still retires its fence; the
     * count shows it, within one epoch; a restart brings a new epoch and
     * the count from 0 */
    {
        uint32_t q[4] = { J_SURF_FRONT, J_XY(90, 0), J_XY(8, 1), 0 }, seq, e0, n0;

        jmodel_advance(50);
        e0 = bcmjob_epoch();
        n0 = bcmjob_errors();
        /* a SCROLL off the frame (J_E_RANGE) */
        CHECK(bcmjob_submit(J_OP_SCROLL, 0, q, 4) == BCMJOB_OK && bcmjob_fence(&seq) == BCMJOB_OK &&
              bcmjob_wait(seq, 50) == BCMJOB_OK, "a failing frame's fence did not retire");
        CHECK(bcmjob_epoch() == e0 && bcmjob_errors() == n0 + 1 &&
              jmodel_ref()->errors == bcmjob_errors(),
              "J_ERRORS %u after one failure (was %u)", bcmjob_errors(), n0);
        CHECK(logged("bcmjob fault op=0x28 why=failed"), "no 'failed' trace");
        CHECK(bcmjob_have(J_OP_SCROLL), "one failure marked SCROLL failed");
        n0 = bcmjob_errors();
        jmodel_fault(JM_FAULT_CUT, 0);
        CHECK(bcmjob_errors() == 0 && bcmjob_epoch() != e0 && n0 != 0,
              "after a restart J_ERRORS %u, epoch %u -> %u", bcmjob_errors(), e0,
              bcmjob_epoch());
        /* a fence lost to a restart answers OFF, not a late TIMEOUT */
        jmodel_advance(JM_COLD_START_MS);
        bcmjob_fence(&seq);
        jmodel_fault(JM_FAULT_CUT, 0);
        CHECK(bcmjob_wait(seq, 50) == BCMJOB_OFF, "a wait across a restart");
        jmodel_advance(JM_COLD_START_MS);
    }

    /* the failed mark (bcmjob.h BCMJOB_FAIL_MARK): a failure is logged and
     * retried; the second in the boot, across a restart, marks the op; the
     * caller's normal outcomes (J_E_NOSLOT ...) never count */
    {
        uint32_t q[4] = { J_SURF_FRONT, 0, J_XY(96, 64), J_SURF_BACK }, seq, k;

        jmodel_fault(JM_FAULT_FAIL_OP, J_OP_AVG);
        CHECK(bcmjob_submit(J_OP_AVG, 0, q, 4) == BCMJOB_OK && bcmjob_fence(&seq) == BCMJOB_OK &&
              bcmjob_wait(seq, 50) == BCMJOB_OK && bcmjob_have(J_OP_AVG),
              "AVG marked after one failure");
        jmodel_fault(JM_FAULT_CUT, 0);
        jmodel_advance(JM_COLD_START_MS);
        CHECK(bcmjob_submit(J_OP_AVG, 0, q, 4) == BCMJOB_OK && bcmjob_fence(&seq) == BCMJOB_OK &&
              bcmjob_wait(seq, 50) == BCMJOB_OK, "the retried AVG");
        CHECK(!bcmjob_have(J_OP_AVG) && bcmjob_submit(J_OP_AVG, 0, q, 4) == BCMJOB_NOJOB &&
              bcmjob_have(J_OP_FILL) && bcmjob_have(J_OP_FENCE),
              "AVG not marked failed after its second failure");
        jmodel_fault(JM_FAULT_NONE, 0);
        jmodel_fault(JM_FAULT_CUT, 0);
        jmodel_advance(JM_COLD_START_MS);
        CHECK(!bcmjob_have(J_OP_AVG), "the failed mark did not last the boot");
        /* a full pool, many times over: ALLOC is never marked */
        for (k = 0; k < 64 && bcmjob_slot(J_CLS_STRIP, 48, 32, 0x100 + k, 0, &s2) == BCMJOB_OK; k++)
            ;
        CHECK(k < 64 && bcmjob_slot(J_CLS_STRIP, 48, 32, 0x200, 0, &s2) == BCMJOB_FAILED &&
              bcmjob_slot(J_CLS_STRIP, 48, 32, 0x201, 0, &s2) == BCMJOB_FAILED &&
              bcmjob_have(J_OP_ALLOC), "a full pool marked ALLOC failed (%u slots)", k);
    }

    /* faults: a missing job, a slow job, a dead chip (twice) */
    jmodel_fault(JM_FAULT_CLEAR_OP, J_OP_FADE);
    CHECK(!bcmjob_have(J_OP_FADE) && bcmjob_submit(J_OP_FADE, 0, p, 8) == BCMJOB_NOJOB,
          "a cleared op ran");
    jmodel_fault(JM_FAULT_NONE, 0);
    jmodel_fault(JM_FAULT_SLOW_OP, J_OP_FILL);
    CHECK(fill_present(1) == BCMJOB_TIMEOUT && bcmjob_have(J_OP_FILL),
          "a slow op marked on its first miss (jobs.md 6: retry once)");
    jmodel_advance(5000);
    CHECK(fill_present(1) == BCMJOB_TIMEOUT && !bcmjob_have(J_OP_FILL) &&
          bcmjob_have(J_OP_PRESENT), "a slow op not marked on its second miss");
    CHECK(logged("bcmjob fault op=0x25 why=timeout"), "no timeout trace");
    jmodel_fault(JM_FAULT_NONE, 0);
    jmodel_advance(5000);
    e = jmodel_cold_starts();
    jmodel_fault(JM_FAULT_DEAD, 0);
    p[0] = J_SURF_FRONT; p[1] = 0; p[2] = J_XY(96, 64); p[3] = 0;
    {
        uint32_t seq;

        bcmjob_submit(J_OP_PRESENT, 0, p, 4);
        bcmjob_fence(&seq);
        CHECK(bcmjob_wait(seq, 100) == BCMJOB_TIMEOUT && jmodel_cold_starts() == e + 1,
              "a dead chip was not restarted");
        jmodel_advance(JM_COLD_START_MS);
        jmodel_fault(JM_FAULT_DEAD, 0);
        bcmjob_submit(J_OP_PRESENT, 0, p, 4);
        bcmjob_fence(&seq);
        bcmjob_wait(seq, 100);
        jmodel_advance(JM_COLD_START_MS);
        CHECK(!bcmjob_have(J_OP_PRESENT), "jobs still on after a second failure");
    }

    /* review M3: a slide cut short by a BCM death jumps to the PP's final
     * frame; a full ring or a cut mid-scroll resyncs from the PP frame */
    {
        uint32_t q[16], seq, k;
        int all;

        init();
        jmodel_set_lit(true);
        jmodel_advance(JM_COLD_START_MS);
        for (k = 0; k < 96 * 64; k++)
            PPFB[k] = 0x3333;               /* the page the PP has drawn */
        q[0] = J_SURF_FRONT; q[1] = 0; q[2] = J_XY(96, 64); q[3] = 0x1111;
        bcmjob_submit(J_OP_FILL, 0, q, 4);
        q[0] = J_SURF_BACK; q[3] = 0x3333;
        bcmjob_submit(J_OP_FILL, 0, q, 4);
        q[0] = J_ANIM_SLIDE; q[1] = J_SURF_FRONT; q[2] = J_SURF_BACK; q[3] = 0;
        q[4] = J_XY(96, 64); q[5] = 0; q[6] = 33; q[7] = 6;
        q[8] = 0 | 30 << 16; q[9] = 55 | 75 << 16; q[10] = 88 | 96 << 16;
        CHECK(bcmjob_submit(J_OP_ANIM, 9, q, 11) == BCMJOB_OK, "slide refused");
        bcmjob_fence(&seq);
        bcmjob_wait(seq, 20);
        jmodel_advance(70);
        CHECK(jmodel_anim_tag() == 9 && P[0] != P[95], "the slide is not running (%04x %04x)",
              P[0], P[95]);
        e = bcmjob_epoch();
        jmodel_fault(JM_FAULT_CUT, 0);
        for (all = 1, k = 0; k < 96 * 64; k++)
            all &= P[k] == 0x3333 && F[k] == 0x3333;
        CHECK(all && jmodel_anim_tag() < 0 && bcmjob_epoch() != e,
              "a death mid-slide: panel %04x, anim %d", P[0], jmodel_anim_tag());
        jmodel_advance(JM_COLD_START_MS);

        /* a full ring mid-frame: the SCROLL went, the PRESENT did not */
        for (k = 0; k < 96 * 64; k++)
            PPFB[k] = (uint16_t)k;
        q[0] = J_SURF_FRONT; q[1] = 0; q[2] = J_XY(96, 64); q[3] = (uint16_t)-8;
        CHECK(bcmjob_submit(J_OP_SCROLL, 0, q, 4) == BCMJOB_OK, "scroll");
        jmodel_fault(JM_FAULT_RING_FULL, 1);
        q[3] = 0;
        CHECK(bcmjob_submit(J_OP_PRESENT, 0, q, 4) == BCMJOB_BUSY, "no injected busy");
        CHECK(bcmjob_resync(0, 0, 96, 64) == BCMJOB_OK, "resync");
        jmodel_advance(20);
        for (all = 1, k = 0; k < 96 * 64; k++)
            all &= P[k] == (uint16_t)k && F[k] == (uint16_t)k;
        CHECK(all, "after a busy mid-frame the panel or the front frame is not the PP's "
              "(the resync before the queued SCROLL)");
        CHECK(logged("bcmjob resync x=0 y=0 w=96 h=64"), "no resync trace");

        /* a cut between the SCROLL and its PRESENT */
        bcmjob_submit(J_OP_SCROLL, 0, q, 4);
        jmodel_fault(JM_FAULT_CUT, 0);
        for (all = 1, k = 0; k < 96 * 64; k++)
            all &= P[k] == (uint16_t)k;
        CHECK(all, "a cut mid-scroll did not restart on the PP's frame");
        jmodel_advance(JM_COLD_START_MS);

        /* review M4: a developer stage takes the BCM, then gives it back */
        e = bcmjob_epoch();
        bcmjob_suspend(true);
        CHECK(!bcmjob_have(J_OP_FILL) && bcmjob_submit(J_OP_FILL, 0, q, 4) == BCMJOB_OFF,
              "jobs while a stage owns the BCM");
        bcmjob_suspend(false);
        CHECK(bcmjob_epoch() != e && epochs_seen == bcmjob_epoch() && bcmjob_have(J_OP_FILL),
              "resume: epoch %u -> %u", e, bcmjob_epoch());
        CHECK(logged("bcmjob suspend") && logged("bcmjob resume epoch="), "suspend traces");
    }

    /* 19-4, the retained screen by the recipe (jobs.md "Shell recipe"):
     * a push slide out of the front frame at the shell's offsets, each
     * frame on the panel equal to the CPU's; a death mid-slide and a full
     * ring at the landing end on the PP's final page; a glide cut mid-way
     * (OFF) continues on the CPU, then resyncs and goes on with jobs */
    {
        uint32_t a[8 + J_SLIDE_MAX / 2], seq = 0, k, n, x, y, t0, pos, prev, drawn, after;
        static const uint32_t durs[2] = { 190, 290 };
        int all, r, d, synced;

        for (d = 0; d < 2; d++)
        {
            init();
            jmodel_set_lit(true);
            jmodel_advance(JM_COLD_START_MS);
            for (k = 0; k < 96 * 64; k++)
            {
                PPFB[k] = (uint16_t)(k * 3 + 7);        /* the new page, drawn */
                F[k] = P[k] = (uint16_t)(k * 5 + 1);    /* the old, shown */
            }
            /* the push (d 0) or pop (d 1) of the body below a header of 8 */
            CHECK(bcmjob_upload_rect(J_SURF_BACK, 0, 8, 96, 56, PPFB + 8 * 96, 96) == BCMJOB_OK,
                  "the new page into BACK");
            memset(a, 0, sizeof(a));
            a[0] = J_ANIM_SLIDE; a[1] = J_SURF_FRONT; a[2] = J_SURF_BACK;
            a[3] = J_XY(0, 8); a[4] = J_XY(96, 56); a[5] = d; a[6] = 33;
            a[7] = n = slide_table(durs[d], 96, a);
            CHECK(n > 3 && n <= J_SLIDE_MAX && ((a[8 + (n - 1) / 2] >> (16 * ((n - 1) & 1))) &
                                                0xFFFF) == 96, "the table: %u frames", n);
            CHECK(bcmjob_submit(J_OP_ANIM, 40 + d, a, 8 + (n + 1) / 2) == BCMJOB_OK &&
                  bcmjob_fence(&seq) == BCMJOB_OK && bcmjob_wait(seq, 20) == BCMJOB_OK,
                  "the slide refused");
            t0 = jmodel_now();
            /* each frame the model shows: offset k, the CPU's slide of the
             * old page at that offset (left: old from off, new after it;
             * right: new first, old after it) */
            for (k = 0; k < n; k++)
            {
                uint32_t off = (a[8 + k / 2] >> (16 * (k & 1))) & 0xFFFF;

                while (jmodel_now() < t0 + 33 * k + 1)
                    jmodel_advance(1);
                for (all = 1, y = 8; y < 64; y++)
                    for (x = 0; x < 96; x++)
                    {
                        uint32_t sx = d ? (x < off ? x + 96 - off : x - off) : x + off;
                        uint16_t want = (d ? x < off : sx >= 96) ?
                                        PPFB[y * 96 + (sx % 96)] : (uint16_t)((y * 96 + sx) * 5 + 1);

                        all &= P[y * 96 + x] == want && F[y * 96 + x] == want;
                    }
                CHECK(all, "slide %d frame %u (offset %u) differs from the CPU's", d, k, off);
            }
            jmodel_advance(40);
            CHECK(jmodel_anim_tag() < 0 && !slide_land(0, 8, 96, 56) && shows_pp(0, 8, 96, 56),
                  "slide %d did not land on the new page", d);
        }

        /* a death mid-slide: the landing's wait times out, the restart's
         * first frame is the PP's: the final page */
        init();
        jmodel_set_lit(true);
        jmodel_advance(JM_COLD_START_MS);
        for (k = 0; k < 96 * 64; k++)
            PPFB[k] = (uint16_t)(k * 3 + 7);
        bcmjob_upload_rect(J_SURF_BACK, 0, 8, 96, 56, PPFB + 8 * 96, 96);
        bcmjob_submit(J_OP_ANIM, 42, a, 8 + (a[7] + 1) / 2);
        jmodel_advance(70);
        CHECK(jmodel_anim_tag() == 42 && !shows_pp(0, 8, 96, 56), "the slide is not running");
        jmodel_fault(JM_FAULT_DEAD, 0);
        CHECK(slide_land(0, 8, 96, 56) == 1 && jmodel_cold_starts() == 2 &&
              shows_pp(0, 8, 96, 56) && jmodel_anim_tag() < 0,
              "a death mid-slide did not end on the final page");
        jmodel_fault(JM_FAULT_NONE, 0);
        jmodel_advance(JM_COLD_START_MS);

        /* a full ring at the landing: resync now (the final page); the
         * animation runs on to its own last frame, the same page, and the
         * landing tried again then finds it ended */
        bcmjob_upload_rect(J_SURF_BACK, 0, 8, 96, 56, PPFB + 8 * 96, 96);
        bcmjob_submit(J_OP_ANIM, 43, a, 8 + (a[7] + 1) / 2);
        jmodel_advance(70);
        jmodel_fault(JM_FAULT_RING_FULL, 1);
        CHECK(slide_land(0, 8, 96, 56) == 1 && shows_pp(0, 8, 96, 56),
              "a busy landing was not resynced to the final page");
        jmodel_advance(400);
        CHECK(!slide_land(0, 8, 96, 56) && shows_pp(0, 8, 96, 56) && jmodel_anim_tag() < 0,
              "after a busy landing the slide did not end on the final page");

        /* bcmjob_upload_rect's answers: pitch < w FAILED; past its time
         * budget (JM_RECT_MS of engine 2) BUSY, never counted; the same
         * rect in bands that fit, OK */
        {
            static uint16_t big[320 * 240], src[320 * 240];
            struct jmodel_cfg c;
            uint32_t b0, nb;

            memset(&c, 0, sizeof(c));
            c.front = (struct jref_surf){ F, 96, 64, 96 };
            c.back = (struct jref_surf){ big, 320, 240, 320 };
            c.panel = (struct jref_surf){ P, 96, 64, 96 };
            c.trace = trace;
            c.pp_fb = PPFB;
            jmodel_init(&c);
            jmodel_set_lit(true);
            jmodel_advance(JM_COLD_START_MS);
            for (k = 0; k < 320 * 240; k++)
                src[k] = (uint16_t)(k * 7 + 3);
            CHECK(bcmjob_upload_rect(J_SURF_BACK, 0, 0, 96, 4, src, 95) == BCMJOB_FAILED,
                  "pitch < w accepted");
            CHECK(bcmjob_upload_rect(J_SURF_BACK, 0, 20, 320, 220, src + 20 * 320, 320) ==
                  BCMJOB_BUSY && bcmjob_have(J_OP_BLIT), "a page past the budget: not BUSY");
            nb = JM_RECT_MS * JM_E2_KB_PER_MS * 1024 / (320 * 2);
            for (all = 1, b0 = 20; b0 < 240; b0 += nb)
            {
                uint32_t bh = 240 - b0 < nb ? 240 - b0 : nb;

                all &= bcmjob_upload_rect(J_SURF_BACK, 0, b0, 320, bh, src + b0 * 320, 320) ==
                       BCMJOB_OK;
            }
            CHECK(all && !memcmp(big + 20 * 320, src + 20 * 320, 2 * 320 * 220),
                  "the page in bands of %u rows", nb);
            init();
            jmodel_set_lit(true);
            jmodel_advance(JM_COLD_START_MS);
        }

        /* a glide: frames by jobs; the BCM cut mid-glide (OFF): the CPU's
         * frames meanwhile (the LCD contract's push: the model's resync),
         * then, once jobs are back, one resync of the view and jobs again */
        for (k = 0; k < 96 * RH * 20; k++)
            LST[k] = (uint16_t)(k * 11 + (k >> 7));
        pos = prev = 0;
        cpu_glide(0);
        bcmjob_resync(0, 0, 96, 64);
        seq = 0;
        for (k = drawn = after = 0, all = synced = 1; k < 72; k++)
        {
            uint32_t target = (k / 6) % 2 ? 13 * RH : RH, m_, s3;

            for (s3 = 0; s3 < 3 && pos != target; s3++)
            {
                m_ = ((pos > target ? pos - target : target - pos) + 2) / 3;
                pos = pos < target ? pos + m_ : pos - m_;
            }
            if (pos == prev)
                continue;
            cpu_glide(pos);                         /* the PP's frame, as today */
            r = BCMJOB_OFF;
            if (++drawn == 4)
                jmodel_fault(JM_FAULT_CUT, 0);      /* the BCM is cut mid-glide */
            if (!synced && bcmjob_have(J_OP_SCROLL))
                synced = bcmjob_resync(0, VY, 96, VH) == BCMJOB_OK;  /* jobs again */
            else if (synced && (r = glide_jobs((int32_t)(pos - prev), &seq)) == BCMJOB_OK)
            {
                if (bcmjob_wait(seq, 60) == BCMJOB_OK)
                    all &= shows_pp(0, VY, 96, VH);
                after += drawn > 4;
            }
            else
            {
                /* OFF, BUSY or TIMEOUT (or not synced yet, or a step of a
                 * whole view): the CPU's frame, nothing retained until a
                 * resync */
                CHECK(drawn >= 4 || r == BCMJOB_NOJOB, "glide frame %u fell back", drawn);
                bcmjob_resync(0, VY, 96, VH);
                synced = 0;
                seq = 0;
            }
            jmodel_advance(30);
            prev = pos;
        }
        CHECK(all, "a glide frame by jobs differs from the CPU's");
        CHECK(jmodel_cold_starts() == 2 && after > 3 && shows_pp(0, VY, 96, VH) &&
              logged("bcm power state=C0 why=fault"),
              "after the cut the glide did not end on the PP's frame by jobs (starts %u, "
              "frames %u, %u by jobs after the cut)", jmodel_cold_starts(), drawn, after);
    }

    /* power-contract.md section 6: the last screen kept in BACK through the
     * unlit face (lit for the BCM), shown again at the wake from the BCM's
     * own memory: BLIT, PRESENT WAIT, FENCE, nothing over the port */
    {
        uint32_t q[6], seq, k, e0;
        int all;

        init();
        jmodel_set_lit(true);
        jmodel_advance(JM_COLD_START_MS);
        for (k = 0; k < 96 * 64; k++)
            PPFB[k] = (uint16_t)(k * 13 + 5);       /* the screen left */
        bcmjob_resync(0, 0, 96, 64);
        q[0] = J_SURF_FRONT; q[1] = 0; q[2] = J_XY(96, 64); q[3] = J_SURF_BACK; q[4] = 0;
        q[5] = 0;
        bcmjob_submit(J_OP_BLIT, 0, q, 6);         /* before the face's first push */
        q[0] = J_SURF_FRONT; q[3] = 0x0841;
        bcmjob_submit(J_OP_FILL, 0, q, 4);         /* the face */
        q[3] = 0;
        bcmjob_submit(J_OP_PRESENT, 0, q, 4);
        bcmjob_fence(&seq);
        CHECK(bcmjob_wait(seq, 100) == BCMJOB_OK && P[0] == 0x0841, "the face");
        e0 = bcmjob_epoch();
        jmodel_advance(5000);                       /* seconds in the window */
        q[0] = J_SURF_BACK; q[1] = 0; q[2] = J_XY(96, 64); q[3] = J_SURF_FRONT; q[4] = 0;
        q[5] = 0;
        bcmjob_submit(J_OP_BLIT, 0, q, 6);         /* the wake: the last screen first */
        q[0] = J_SURF_FRONT; q[3] = J_PRES_WAIT;
        bcmjob_submit(J_OP_PRESENT, 0, q, 4);
        bcmjob_fence(&seq);
        CHECK(bcmjob_wait(seq, J_T_FENCE_BASE + J_T_DRAW + J_T_PRESENT) == BCMJOB_OK &&
              bcmjob_epoch() == e0, "the wake's frame");
        for (all = 1, k = 0; k < 96 * 64; k++)
            all &= P[k] == PPFB[k];
        CHECK(all, "the wake did not show the screen left");
    }

    marquee_seq();

    /* power: dark playback for an hour powers nothing; a decode on the
     * battery is refused, on the charger it powers up, counted (G11, G12) */
    init();
    jmodel_set_lit(true);
    jmodel_advance(1000);
    jmodel_set_lit(false);
    jmodel_advance(3600 * 1000);
    CHECK(bcmjob_dark_ups(0) == 0 && jmodel_state() == JM_C0 &&
          jmodel_state_ms(JM_C0) >= 3600 * 1000, "dark playback: ups %u, C0 %u ms",
          bcmjob_dark_ups(0), jmodel_state_ms(JM_C0));
    CHECK(bcm_power_want(BCM_WANT_DECODE, 2000) == BCMJOB_REFUSED && jmodel_state() == JM_C0,
          "a decode on battery powered the BCM");
    jmodel_set_charger(true);
    CHECK(bcm_power_want(BCM_WANT_DECODE, 2000) == BCMJOB_OK && jmodel_state() != JM_C0 &&
          bcmjob_dark_ups(BCM_WANT_DECODE) == 1 && bcmjob_dark_ups(0) == 1,
          "a charger decode: ups %u", bcmjob_dark_ups(0));
    jmodel_advance(2500);
    CHECK(jmodel_state() == JM_C0, "the hold did not end");
    CHECK(logged("bcm power state=C0 why=dark") && logged("bcm power want why=decode"),
          "power traces");

    decode_tests();
    viz_tests();

    for (i = 0; i < (unsigned)nlog; i++)
        if (strlen(log_[i]) >= 111)
            CHECK(0, "a trace line too long: %s", log_[i]);
    if (failed) for (i = 0; i < (unsigned)nlog && i < 30; i++) printf("  log: %s\n", log_[i]);
    printf("jmtest: %d cases, %d failed\n", cases, failed);
    return failed != 0;
}
