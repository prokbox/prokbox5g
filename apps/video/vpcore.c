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

/* The video player's core (see vpcore.h). */

#include <string.h>
#include "vpcore.h"
#include "vcheck.h"
#include "mp4demux.h"
#include "../../bcmfw/hostproto.h"
#include "../../bcmfw/jobs.h"       /* HP_JOBS, J_LAT_* (batch 4's timings) */
#ifdef ROCKBOX
#include "devstats.h"   /* Fit-3 step 0: empty unless HAVE_DEVSTATS */
#else                   /* the host tests (tests/video/run.sh) */
#define DEVSTATS_OVL(bytes) do { } while (0)
#define DEVSTATS_RD_POLL()  do { } while (0)
#endif

#define ALIGN4(p)   ((uint8_t *)(((uintptr_t)(p) + 3) & ~(uintptr_t)3))

static void say(struct vpc *v, const char *what, long a, long b)
{
    if (v->be->log)
        v->be->log(what, a, b);
}

static int ovl_apply(struct vpc *v);

/* ---- P19 PW4: the chip's clock (vclk.h) ------------------------------- */

#define VPC_VCLK_POLL_US    100000      /* the meter read: 10 a second */

/* one of vclk.c's lines, as it is (vplayer's be_log keeps "vclk " lines
   verbatim) */
static void vclk_say(struct vpc *v,
                     int (*fmt)(const struct vclk_gov *, char *, size_t))
{
    char line[128];

    fmt(&v->gov, line, sizeof(line));
    say(v, line, 0, 0);
}

static void vclk_send(struct vpc *v, uint32_t sub, uint32_t mhz)
{
    uint32_t a[3] = { HP_VCLK_MAGIC, sub, mhz };

    if (bvp_send(&v->bvp, HP_OP_VCLK, a, sub == HP_VCLK_STAT ? 2 : 3,
                 &v->vclk_tag) != 0)
        v->vclk_tag = 0;
}

/* the governor wants another band or clock (want_band, want_mhz): asked
   now, or after the command in flight; a band move is logged */
static void vclk_want(struct vpc *v, int changed)
{
    if (v->vclk_force && v->vclk_on)
    {
        /* the bench's held band (vbv), asked outright.  Through the
           governor it was only a floor, which never lowers: after a
           re-open at 150 MHz (the clip's loop), the held band waited for
           the dwell and a load low enough for it, so sitting 4c's 46 and
           59 MHz runs ended at 150 ("ran 150 MHz", 8.3 s at the band
           then 1 s at 150, no fallback) */
        v->gov.want_band = (uint8_t)(v->vclk_force - 1);
        v->gov.want_mhz = vclk_band_mhz[v->vclk_force - 1];
        changed = 1;
    }
    if (!changed || !v->vclk_on)
        return;
    if (v->vclk_tag)
    {
        v->vclk_pending = 1;
        return;
    }
    v->vclk_pending = 0;
    if (v->gov.want_band == v->gov.band && v->gov.want_mhz == v->gov.mhz)
        return;
    if (v->gov.want_band != v->gov.band)
        vclk_say(v, vclk_band_line);
    v->vclk_asked = v->gov.want_band;
    vclk_send(v, (uint32_t)v->gov.want_band + 1, v->gov.want_mhz);
}

/* the session (re)opened: the governor starts, or picks up after a wake
   (the dark time isn't counted) with the floor back at the top */
static void vclk_start(struct vpc *v)
{
    uint32_t a[2] = { HP_VCLK_MAGIC, HP_VCLK_STAT };
    uint32_t r[BVP_REPLY_WORDS], n = 0, mask, now;
    unsigned lvl = 0, policy = VCLK_POLICY_TOP;

    v->vclk_on = false;
    v->vclk_tag = 0;
    v->vclk_asked = -1;
    v->vclk_pending = 0;
    if (!v->be->vclk || !(v->bvp.caps & HP_CAP_VCLK) ||
        !(mask = v->be->vclk(&lvl, &policy)) ||
        bvp_call(&v->bvp, HP_OP_VCLK, a, 2, r, BVP_REPLY_WORDS, &n,
                 200000) != HP_OK || n < HP_VCLK_WORDS ||
        r[HP_VCLK_R_BAND] >= VCLK_BANDS)
        return;
    now = v->be->port->now_us();
    if (!v->vclk_inited)
    {
        vclk_gov_init(&v->gov, mask, (int)r[HP_VCLK_R_BAND],
                      r[HP_VCLK_R_MHZ], (enum vclk_policy)policy, lvl, now);
        vclk_video_start(&v->vfl, mask, 0, now);
        v->vclk_inited = true;
        say(v, "video: chip clock governor (MHz, verified mask)",
            (long)v->gov.mhz, (long)mask);
    }
    else
    {
        v->gov.band = (uint8_t)r[HP_VCLK_R_BAND];
        v->gov.mhz = (uint8_t)(r[HP_VCLK_R_MHZ] ? r[HP_VCLK_R_MHZ] : 27);
        v->gov.last_us = v->gov.changed_us = now;
        vclk_video_race(&v->vfl, now);
    }
    v->vclk_busy = r[HP_VCLK_R_BUSY];
    v->vclk_now = r[HP_VCLK_R_NOW];
    v->vclk_poll_at = now;
    v->vclk_miss_mark = v->eng.stats.skipped + v->eng.stats.dropped_gops;
    v->vclk_on = true;
    vclk_want(v, vclk_gov_floor(&v->gov, now, v->vclk_force ?
                                (int)v->vclk_force - 1 : v->vfl.floor));
}

/* a seek, a resume: the floor back at the top */
static void vclk_race(struct vpc *v)
{
    uint32_t now = v->be->port->now_us();

    if (v->vclk_on && !v->vclk_force)
        vclk_want(v, vclk_gov_floor(&v->gov, now,
                                    vclk_video_race(&v->vfl, now)));
}

/* a picture moved (STATUS words 11-15): its FEED time into the floor, and
   the frame time from the pictures' times */
static void vclk_frame(struct vpc *v, const uint32_t *r)
{
    uint32_t now = v->be->port->now_us();
    int64_t pts = (int64_t)((uint64_t)r[12] << 32 | r[11]);

    if (!v->vclk_on)
        return;
    if (v->vclk_shown && r[7] > v->vclk_shown && pts > v->vclk_pts)
    {
        uint32_t f = (uint32_t)((pts - v->vclk_pts) / (r[7] - v->vclk_shown));

        if (f >= 16000 && f <= 100000)
            v->vfl.frame_us = (v->vfl.frame_us * 3 + f) / 4;
    }
    v->vclk_pts = pts;
    v->vclk_shown = r[7];
    if (r[13] && !v->vclk_force)
        vclk_want(v, vclk_gov_floor(&v->gov, now,
                                    vclk_video_feed(&v->vfl, now, r[13],
                                                    v->gov.mhz)));
}

/* every tick: misses into the floor; the command in flight answered; the
   next one; the meter read 10 times a second */
static void vclk_poll(struct vpc *v)
{
    uint32_t r[BVP_REPLY_WORDS], n, now, m;
    int rc;

    if (!v->vclk_on)
        return;
    now = v->be->port->now_us();
    /* the chip's misses: pictures skipped, GOPs dropped (a late post is
       the host's, not the chip's) */
    m = v->eng.stats.skipped + v->eng.stats.dropped_gops;
    if (m != v->vclk_miss_mark)
    {
        v->vclk_misses += m - v->vclk_miss_mark;
        v->vclk_miss_mark = m;
        if (!v->vclk_force)
            vclk_want(v, vclk_gov_floor(&v->gov, now,
                                        vclk_video_miss(&v->vfl, now,
                                                        v->gov.band)));
    }
    if (v->vclk_tag)
    {
        rc = bvp_take(&v->bvp, v->vclk_tag, r, BVP_REPLY_WORDS, &n);
        if (rc == BVP_AGAIN)
            return;
        v->vclk_tag = 0;
        if (rc == HP_OK && n >= HP_VCLK_WORDS && r[HP_VCLK_R_BAND] < VCLK_BANDS)
        {
            uint32_t busy = r[HP_VCLK_R_BUSY] - v->vclk_busy;
            uint32_t el = r[HP_VCLK_R_NOW] - v->vclk_now;
            int asked = v->vclk_asked;

            v->vclk_busy = r[HP_VCLK_R_BUSY];
            v->vclk_now = r[HP_VCLK_R_NOW];
            vclk_gov_ran(&v->gov, now, asked, (int)r[HP_VCLK_R_BAND],
                         r[HP_VCLK_R_MHZ]);
            if (asked >= 0 && asked != (int)r[HP_VCLK_R_BAND])
            {
                vclk_say(v, vclk_band_line);    /* why=fail */
                if (!v->ub_why)
                {
                    v->ub_why = VCHECK_BAD_CLOCK;
                    v->ub_a = (long)r[HP_VCLK_R_MHZ];
                }
            }
            v->vclk_asked = -1;
            if (asked < 0 && vclk_gov_meter(&v->gov, now, busy, el))
                v->vclk_pending = 1;
        }
    }
    if (v->vclk_pending)
    {
        vclk_want(v, 1);
        if (v->vclk_tag)
            return;
    }
    if (now - v->vclk_poll_at >= VPC_VCLK_POLL_US)
    {
        v->vclk_poll_at = now;
        v->vclk_asked = -1;
        vclk_send(v, HP_VCLK_STAT, 0);
    }
}

/* the play's end, or the display going dark: the means and the time at
   each band */
static void vclk_sum(struct vpc *v)
{
    char line[128];

    if (!v->vclk_inited)
        return;
    vclk_gov_account(&v->gov, v->be->port->now_us());
    vclk_sum_line(&v->gov, v->vclk_misses, line, sizeof(line));
    say(v, line, 0, 0);
    vclk_say(v, vclk_time_line);
}

/* the first-use check's live invalidation, once a play: to the backend,
   outside any reply's handling */
static void unit_bad_report(struct vpc *v)
{
    if (!v->ub_why || v->ub_said)
        return;
    v->ub_said = true;
    say(v, "video: unit check invalidated (why, value)", v->ub_why, v->ub_a);
    if (v->be->unit_bad)
        v->be->unit_bad(v->ub_why, v->ub_a);
}

int vpc_vclk_cap(struct vpc *v, int b)
{
    uint32_t a[2] = { HP_VCLK_MAGIC, 0 }, r[BVP_REPLY_WORDS], n = 0;
    int rc;

    if (b < 1 || b >= VCLK_BANDS)
        return -1;
    if (v->vclk_on)
    {
        /* the governor: no band above b from now on (its floor and its
           rises stay inside the mask) */
        v->gov.mask &= (2u << b) - 1;
        v->vfl.mask &= (2u << b) - 1;
        vclk_want(v, vclk_gov_floor(&v->gov, v->be->port->now_us(),
                                    v->vfl.floor > b ? b : v->vfl.floor));
        return 0;
    }
    if (!v->bvp.session || !(v->bvp.caps & HP_CAP_VCLK))
        return -1;
    a[1] = 1 + (uint32_t)b;
    rc = bvp_call(&v->bvp, HP_OP_VCLK, a, 2, r, BVP_REPLY_WORDS, &n, 200000);
    if (rc != HP_OK || n < HP_VCLK_WORDS)
        return rc < 0 ? rc : -1;
    v->sess_mhz = vclk_band_mhz[b];
    say(v, "video: unit check: the band capped (band, MHz)", b,
        (long)r[HP_VCLK_R_MHZ]);
    return 0;
}

bool vpc_vclk_means(const struct vpc *v, uint32_t *mhz, uint32_t *lvl10,
                    uint32_t *idle_pm, uint32_t *ms)
{
    int b;

    *ms = 0;
    if (!v->vclk_inited)
        return false;
    vclk_gov_means(&v->gov, mhz, lvl10, idle_pm);
    for (b = 0; b < VCLK_BANDS; b++)
        *ms += v->gov.at_ms[b];
    return true;
}

/* ---- the index --------------------------------------------------------- */

/* A source failure, kept for vpc_tick's log (dev42: session 35's
   "sample source failed" said only where) */
static int src_failed(struct vpc *v, uint32_t why, uint32_t i, uint32_t size,
                      long rc, uint32_t want, uint32_t at)
{
    v->src_fail.why = why;
    v->src_fail.sample = i;
    v->src_fail.size = size;
    v->src_fail.rc = rc;
    v->src_fail.want = want;
    v->src_fail.at = at;
    return -1;
}

static int window(struct vpc *v, uint32_t i)
{
    uint32_t first, n;
    long want, got;

    if (i >= v->hdr.count)
        return -1;
    if (i >= v->win_first && i < v->win_first + v->win_n)
        return 0;
    /* a little behind too: the engine reads the oldest unposted sample
       and pushes up to two seconds ahead of it */
    first = i > 64 ? i - 64 : 0;
    n = v->hdr.count - first;
    if (n > VPC_WINDOW_RECORDS)
        n = VPC_WINDOW_RECORDS;
    want = (long)n * VIX_RECORD_SIZE;
    v->win_n = 0;
    got = v->src->index_read(v->src->ctx,
                             VIX_HEADER_SIZE + (uint64_t)first * VIX_RECORD_SIZE,
                             v->win, (uint32_t)want);
    if (got != want)
        return src_failed(v, VPC_SRC_WINDOW, i, 0, got, (uint32_t)want,
                          first);
    v->win_first = first;
    v->win_n = n;
    return 0;
}

static int record(struct vpc *v, uint32_t i, struct mp4_sample *s)
{
    if (window(v, i) < 0)
        return -1;
    vix_read_record(v->win + (i - v->win_first) * VIX_RECORD_SIZE, s);
    return 0;
}

static int op_sample(void *ctx, uint32_t i, struct vp_sample *out)
{
    struct mp4_sample s;

    if (record(ctx, i, &s) < 0)
        return -1;
    out->pts_us = s.pts_us;
    out->size = s.size;
    out->key = s.key;
    return 0;
}

/* ---- samples into the FIFO --------------------------------------------- */

static int stage_flush(struct vpc *v, bool last)
{
    uint32_t w = last ? v->stage_n : v->stage_n & ~3u;

    if (w && bvp_pkt_write(&v->bvp, v->stage, w))
    {
        v->push_port_err = true;    /* bcmvp: only the port fails here */
        return -1;
    }
    if (w)
    {
        /* the port may still be reading this one (when the lab's PP DMA
           is on; a dead end, data-paths.md D1): the next piece goes into
           the other, the <= 3 waiting bytes first */
        uint8_t *other = v->stage == v->stage_buf[0] ? v->stage_buf[1]
                                                     : v->stage_buf[0];

        memcpy(other, v->stage + w, v->stage_n - w);
        v->stage = other;
    }
    v->stage_n -= w;
    v->stats.pieces++;
    v->stats.bytes += w;
    return 0;
}

/* Pieces go out in whole words (the host port moves 32 bits) */
static int stage_put(struct vpc *v, const uint8_t *p, uint32_t n)
{
    while (n)
    {
        uint32_t k = VPC_PIECE + 4 - v->stage_n;

        if (k > n)
            k = n;
        memcpy(v->stage + v->stage_n, p, k);
        v->stage_n += k;
        p += k;
        n -= k;
        if (v->stage_n >= VPC_PIECE && stage_flush(v, false))
            return -1;
    }
    return 0;
}

/* [4-byte big-endian length][NAL]... to Annex B in place: each length
   becomes 00 00 00 01, the size unchanged (mp4_annexb(), in pieces) */
static void annexb_piece(struct vpc *v, uint8_t *p, uint32_t n)
{
    uint32_t i = 0;

    while (i < n)
    {
        if (v->nal_left)
        {
            uint32_t k = n - i < v->nal_left ? n - i : v->nal_left;

            i += k;
            v->nal_left -= k;
            continue;
        }
        v->nal_acc = v->nal_acc << 8 | p[i];
        p[i++] = v->len_pos < 3 ? 0 : 1;
        if (++v->len_pos == 4)
        {
            v->nal_left = v->nal_acc;
            v->nal_acc = 0;
            v->len_pos = 0;
        }
    }
}

static int push_body(struct vpc *v, uint32_t i, bool params);

/* The NAL types in the parameter sets as sent (Annex B start codes):
   bit 7 SPS, bit 8 PPS (session 36c: slices found no PPS) */
static uint32_t params_nal_mask(const struct vpc *v)
{
    uint32_t k, mask = 0;

    for (k = 0; k + 3 < v->params_len; k++)
        if (!v->params[k] && !v->params[k + 1] && v->params[k + 2] == 1)
            mask |= 1u << (v->params[k + 3] & 31);
    return mask;
}

/* A port failure part way through a packet isn't the source failing:
   the packet is dropped, the push waits, and vpc_tick rides the stall
   out or reloads (dev13) */
static int push_sample(struct vpc *v, uint32_t i, bool params)
{
    int rc = push_body(v, i, params);

    /* a packet begun and not ended (a failure, or VP_AGAIN part way
       through: a piece not buffered) is dropped, whole */
    if (rc != 0 && v->pkt_open)
    {
        bvp_pkt_abort(&v->bvp);
        /* session 36c: a first start whose decoder never had a PPS */
        if (!v->stats.aborts++ || params)
            say(v, "video: packet dropped part way (sample, with params)",
                i, params);
    }
    if (rc < 0 && v->push_port_err)
        rc = VP_AGAIN;
    v->pkt_open = false;
    return rc;
}

static int push_body(struct vpc *v, uint32_t i, bool params)
{
    const struct vpc_source *src = v->src;
    struct mp4_sample s;
    uint32_t pos, total;
    long rc;
    uint8_t probe;

    if (record(v, i, &s) < 0)
        return -1;
    if (!s.size)
        return src_failed(v, VPC_SRC_EMPTY, i, 0, 0, 0, 0);
    /* buffered up to its last byte?  The buffer fills in order, so then
       all of it is (or it was reclaimed, and read() reads the file) */
    rc = src->read(src->ctx, s.offset + s.size - 1, &probe, 1);
    if (rc == VPC_NOT_BUFFERED)
    {
        v->stats.data_waits++;
        return VP_AGAIN;
    }
    if (rc != 1)
        return src_failed(v, VPC_SRC_PROBE, i, s.size, rc, 1, s.size - 1);
    total = s.size + (params ? v->params_len : 0);
    if (v->lockstep && v->bvp.in_flight)
    {
        /* stage 6 (dev12): the PP writes only while the BCM is idle */
        if (bvp_refresh(&v->bvp))
        {
            v->push_port_err = true;
            return -1;
        }
        if (v->bvp.in_flight)
        {
            v->lock_waits++;
            return VP_AGAIN;
        }
    }
    rc = bvp_pkt_begin(&v->bvp, total);
    if (rc == BVP_AGAIN)
        return VP_AGAIN;
    if (rc)
    {
        v->push_port_err = rc == BVP_E_PORT;
        return v->push_port_err ? -1
               : src_failed(v, VPC_SRC_BEGIN, i, s.size, rc, total, 0);
    }
    v->pkt_open = true;
    if (params && !v->first_set && v->params_len >= 16)
    {
        /* as words read back from the FIFO (little-endian halfwords
           through the port): the first 16 bytes the host writes */
        uint32_t k;

        for (k = 0; k < 4; k++)
            v->first_bytes[k] = (uint32_t)v->params[4 * k] |
                                (uint32_t)v->params[4 * k + 1] << 8 |
                                (uint32_t)v->params[4 * k + 2] << 16 |
                                (uint32_t)v->params[4 * k + 3] << 24;
        v->first_off = v->bvp.pkt[(v->bvp.head + v->bvp.n - 1) %
                                  BVP_MAX_PKTS].off;
        v->first_set = true;
    }
    v->stage_n = 0;
    v->nal_left = v->len_pos = v->nal_acc = 0;
    if (params && stage_put(v, v->params, v->params_len))
        return -1;
    for (pos = 0; pos < s.size; )
    {
        uint32_t m = s.size - pos < VPC_PIECE ? s.size - pos : VPC_PIECE;

        rc = src->read(src->ctx, s.offset + pos, v->piece, m);
        if (rc == VPC_NOT_BUFFERED)
        {
            /* the probe found the last byte buffered, and this piece
               isn't: the audio's buffer was reset under us (its seek's
               rebuffer, as at a resume far into the file: session 35's
               plays at 18:15).  The packet goes, the push waits */
            if (!v->stats.piece_waits++)
                say(v, "video: sample's bytes left the buffer (sample, at)",
                    i, pos);
            return VP_AGAIN;
        }
        if (rc != (long)m)
            return src_failed(v, VPC_SRC_PIECE, i, s.size, rc, m, pos);
        annexb_piece(v, v->piece, m);
        if (stage_put(v, v->piece, m))
            return -1;
        pos += m;
    }
    if (stage_flush(v, true))
        return -1;
    bvp_pkt_end(&v->bvp);
    if (v->nal_left || v->len_pos)
        say(v, "video: sample's NAL lengths overrun", i, v->nal_left);
    return 0;
}

static int op_push(void *ctx, uint32_t i, bool params)
{
    return push_sample(ctx, i, params);
}

/* ---- the session ------------------------------------------------------- */

static uint32_t open_mode(const struct vpc *v)
{
    return HP_MODE_FIT | HP_OPEN_OWN_DISPLAY |
           ((v->be->open_flags ? v->be->open_flags() : 0) & ~v->open_drop);
}

static uint32_t sdc_word(uint32_t off)
{
    return 0xA55A0000u ^ off ^ (off << 13);
}

/* Stage 6's readback (device-session-1.md, "6s"): before the first FEED
   the FIFO holds nothing we need, so a pattern goes in through the host
   port and comes back word by word through the read path */
static void sdram_check(struct vpc *v)
{
    const struct bvp_port *pt = v->be->port;
    uint16_t *hw = (uint16_t *)v->stage;     /* 4-aligned, 2 KB */
    struct vpc_sdcheck *c = &v->sdc;
    uint32_t nb = v->sd_whole ? VPC_SDC_WHOLE : VPC_SDC_BLOCKS;
    uint32_t span = v->sd_whole ? VPC_SDC_SDRAM : v->bvp.sess_fifo_size;
    uint32_t step = span / nb, i, k, off, got;

    memset(c, 0, sizeof(*c));
    c->done = 1;
    c->span = span;
    for (i = 0; i < nb; i++)
    {
        off = i * step;
        for (k = 0; k < 256; k++)
        {
            uint32_t w = sdc_word(off + 4 * k);

            hw[2 * k] = w & 0xFFFF;
            hw[2 * k + 1] = w >> 16;
        }
        if (pt->wr_block(v->bvp.sess_fifo + off, hw, 512) < 0)
        {
            c->port_err = BVP_E_PORT;
            return;
        }
        c->last_ok = off;
    }
    for (i = 0; i < nb; i++)
        for (k = 0; k < VPC_SDC_READS; k++)
        {
            off = i * step + k * 340 / 4 * 4;      /* spread over 1 KB */
            if (pt->rd(v->bvp.sess_fifo + off, &got) < 0)
            {
                c->port_err = BVP_E_PORT;
                return;
            }
            c->words++;
            if (got != sdc_word(off) && !c->bad++)
            {
                c->first_off = off;
                c->first_got = got;
                c->first_want = sdc_word(off);
            }
        }
}

/* OPEN, and once more at 27 MHz if SDRAM didn't come up on PLL A (the
   image is back on the crystal after a failed OPEN); the SDRAM words go
   to the log either way (hostproto.md, "OPEN") */
static int open_session(struct vpc *v)
{
    int rc = bvp_open(&v->bvp, v->hdr.width, v->hdr.height, open_mode(v));

    if (rc == HP_E_NOMEM && (open_mode(v) & HP_OPEN_FAST_CLOCK))
    {
        say(v, "sdram: 150 MHz failed", v->bvp.sd[0], v->bvp.sd[1]);
        say(v, "sdram: probe read", v->bvp.sd[2], v->bvp.sd[3]);
        memcpy(v->sd_pll, v->bvp.sd, sizeof(v->sd_pll));
        v->open_drop |= HP_OPEN_FAST_CLOCK;
        rc = bvp_open(&v->bvp, v->hdr.width, v->hdr.height, open_mode(v));
    }
    if (v->bvp.sd_n)
        say(v, rc == HP_OK ? "sdram: up" : "sdram: failed",
            v->bvp.sd[0], v->bvp.sd_n > 1 ? v->bvp.sd[1] : 0);
    if (v->bvp.sd_n > 1)
        say(v, "sdram: probe read", v->bvp.sd[2], v->bvp.sd[3]);
    if (v->bvp.sd[4])
        say(v, "sdram: memory test", v->bvp.sd[4], 0);
    if (rc == HP_OK && v->sd_check && !v->sdc.done)
    {
        sdram_check(v);
        say(v, "sdram: readback words/bad", v->sdc.words, v->sdc.bad);
    }
    return rc;
}

static int bring_up(struct vpc *v)
{
    int rc;
    uint32_t t0 = v->be->port->now_us();

    v->status_tag = 0;
    v->shown_seen = 0;
    v->first_set = false;
    memset(&v->st, 0, sizeof(v->st));
    if (v->be->boot() < 0)
        return BVP_E_PORT;
    v->bvp.wire = &v->wire;
    if ((rc = bvp_attach(&v->bvp, v->be->port)) ||
        (rc = bvp_hello(&v->bvp)) != HP_OK)
        return rc;
    if (v->check_due && v->be->check)
    {
        /* the first-use check, once (vcheck.h): its own sessions, closed
           again; then the play's, at the band it found */
        v->check_due = false;
        if ((rc = v->be->check(&v->bvp)) != 0)
            return rc;
    }
    if ((rc = open_session(v)) != HP_OK)
        return rc;
    v->sess_mhz = open_mode(v) & HP_OPEN_FAST_CLOCK ? 150 : 0;
    v->boot_us = v->be->port->now_us() - t0;    /* boot, hello, OPEN */
    vclk_start(v);
    if (v->ovl_w)
        ovl_apply(v);
    v->fresh = true;
    v->restart_pending = false;
    v->eos_posted = false;
    return 0;
}

/* The shell's overlay into the image's host frame and on (or off); a
   slide up only from vpc_overlay_slide(), a restart's put it up at once */
static int ovl_apply_slide(struct vpc *v, uint32_t slide)
{
    uint32_t r;

    if (!v->ovl_w)
        return bvp_overlay(&v->bvp, 0, 0, 0, 0);
    for (r = 0; r < v->ovl_h; r++)
        if (v->be->port->wr_block(VPC_HOST_FB +
                                  2 * ((v->ovl_y + r) * VPC_PANEL_W + v->ovl_x),
                                  v->ovl_src + r * v->ovl_pitch, v->ovl_w) < 0)
            return BVP_E_PORT;
    DEVSTATS_OVL(v->ovl_w * v->ovl_h * 2);
    return bvp_overlay_slide(&v->bvp, v->ovl_x, v->ovl_y, v->ovl_w, v->ovl_h,
                             slide);
}

static int ovl_apply(struct vpc *v)
{
    return ovl_apply_slide(v, 0);
}

/* CLOSE and OPEN: the stage A image's only way to drop what it holds or
   to start again at another place in the FIFO */
static int session_restart(struct vpc *v)
{
    int rc;

    v->stats.restarts++;
    v->status_tag = 0;
    v->eos_posted = false;
    v->first_set = false;
    bvp_close(&v->bvp, 0);
    rc = bvp_open(&v->bvp, v->hdr.width, v->hdr.height, open_mode(v));
    v->st.in_flight = 0;
    if (rc == HP_OK && v->ovl_w)
        ovl_apply(v);           /* an OPEN clears the image's overlay */
    return rc == HP_OK ? 0 : rc;
}

static void op_restart(void *ctx)
{
    struct vpc *v = ctx;

    if (v->fresh)
    {
        v->fresh = false;       /* opened just now: nothing to drop */
        return;
    }
    v->restart_pending = false;
    if (session_restart(v))
        v->resync = true;
}

/* Samples were skipped (a dropped GOP, the slideshow above 2x) and sit in
   the FIFO unposted: the image would decode them, in order, before
   anything after them.  Restart and push again from sample i on. */
static void restart_from(struct vpc *v, uint32_t i)
{
    uint32_t k;

    v->restart_pending = false;
    if (session_restart(v))
    {
        v->resync = true;
        return;
    }
    for (k = i; k < v->eng.pushed; k++)
        if (push_sample(v, k, k == i) != 0)
        {
            v->resync = true;   /* reseek at the next tick */
            return;
        }
}

static void op_post(void *ctx, uint32_t i, int64_t pts_us, bool show)
{
    struct vpc *v = ctx;
    struct mp4_sample s;
    uint32_t flags = 0;
    int rc;

    if (v->resync)
        return;
    if (v->restart_pending)
    {
        uint32_t t0 = v->be->port->now_us();

        restart_from(v, i);
        if (v->resync)
            return;
        /* the re-push ran after the engine took this record's lateness:
           the record went out that much later (session 29: a dropped
           GOP left every picture 50-70 ms early) */
        vp_add_lateness(&v->eng, pts_us,
                        (int64_t)(v->be->port->now_us() - t0));
    }
    if (record(v, i, &s) == 0 && s.key)
        flags |= HP_FEED_KEY;
    if (i + 1 == v->hdr.count)
        flags |= HP_FEED_EOS;
    if (!show)
        flags |= HP_FEED_HIDE;  /* a seek's catch-up: decoded, not shown */
    rc = bvp_feed(&v->bvp, pts_us, flags);
    if (!rc && (flags & HP_FEED_EOS))
        v->eos_posted = true;
    if (rc)
    {
        /* a full ring (the image has stopped reading) or a lost packet:
           start again where the audio is */
        if (rc == BVP_AGAIN)
            v->stats.feed_again++;
        v->resync = true;
    }
}

static void op_skip(void *ctx, uint32_t i)
{
    struct vpc *v = ctx;

    (void)i;
    v->restart_pending = true;
}

/* ---- avcC -------------------------------------------------------------- */

static long direct_read(void *ctx, uint64_t off, void *buf, unsigned long len)
{
    const struct vpc_source *src = ctx;
    long rc;
    int tries = 0;

    /* read() may answer "not buffered yet": the parameter sets are in
       moov, which the audio has long passed or not reached */
    /* retried until buffered, yielding so the buffering thread can get
       there (a busy loop at the video thread's priority could starve it),
       at most VPC_DIRECT_TRIES times (dev33: a stop must get through) */
    while ((rc = src->read(src->ctx, off, buf, len)) == VPC_NOT_BUFFERED &&
           src->wait && ++tries < VPC_DIRECT_TRIES)
        src->wait();
    return rc;
}

static int load_params(struct vpc *v)
{
    struct mp4_io io;
    struct mp4_movie m;
    uint8_t head[5];   /* avcC: version, profile, constraints, level, size */
    long rc;

    if (v->hdr.avcc_len < 7 || 2 * v->hdr.avcc_len > VPC_PARAMS_MAX)
        return MP4_E_BAD_AVCC;
    memset(&m, 0, sizeof(m));
    io.read_at = direct_read;
    io.ctx = (void *)v->src;
    io.size = ~0ULL;
    m.io = &io;
    m.avcc.pos = v->hdr.avcc_off;
    m.avcc.end = (uint64_t)v->hdr.avcc_off + v->hdr.avcc_len;
    if (direct_read((void *)v->src, v->hdr.avcc_off, head, 5) != 5)
        return MP4_E_IO;
    if ((head[4] & 3) != 3)
        return MP4_E_BAD_AVCC;  /* stage A converts 4-byte lengths only */
    if (!vpc_baseline_ok(head[1], head[2]))
        return MP4_E_BAD_AVCC;  /* not Baseline (avcC profile, player.md 8):
                                   refused at open, the audio plays alone */
    rc = mp4_params(&m, v->params, VPC_PARAMS_MAX);
    if (rc <= 0)
        return rc < 0 ? (int)rc : MP4_E_BAD_AVCC;
    v->params_len = (uint32_t)rc;
    return 0;
}

/* ---- public ------------------------------------------------------------ */

long vpc_init(struct vpc *v, const struct vpc_source *src,
              const struct vpc_backend *be, void *mem, size_t len)
{
    uint8_t *p = ALIGN4(mem), hdr[VIX_HEADER_SIZE];
    uint32_t first, nkeys = 0;
    int rc;

    memset(v, 0, sizeof(*v));
    v->src = src;
    v->be = be;
    if (len < VPC_MEM_SIZE)
        return MP4_E_SPACE;
    v->win = p;
    p += VPC_WINDOW_RECORDS * VIX_RECORD_SIZE;
    v->piece = p;
    p += VPC_PIECE;
    v->stage = v->stage_buf[0] = p;
    p += VPC_PIECE + 4 + 4;
    v->stage_buf[1] = p;
    p += VPC_PIECE + 4 + 4;
    v->params = p;

    if (src->index_read(src->ctx, 0, hdr, VIX_HEADER_SIZE) != VIX_HEADER_SIZE)
        return MP4_E_IO;
    if ((rc = vix_read_header(hdr, &v->hdr)) < 0)
        return rc;
    if (!v->hdr.count || !v->hdr.width || !v->hdr.height)
        return MP4_E_FORMAT;
    if ((rc = load_params(v)) < 0)
        return rc;
    for (first = 0; first < v->hdr.count; first += v->win_n)
    {
        uint32_t i;

        if (window(v, first) < 0 || v->win_first != first)
        {
            /* window() starts a little behind: read this one exactly */
            uint32_t n = v->hdr.count - first;
            long want;

            if (n > VPC_WINDOW_RECORDS)
                n = VPC_WINDOW_RECORDS;
            want = (long)n * VIX_RECORD_SIZE;
            if (src->index_read(src->ctx, VIX_HEADER_SIZE +
                                (uint64_t)first * VIX_RECORD_SIZE,
                                v->win, (uint32_t)want) != want)
                return MP4_E_IO;
            v->win_first = first;
            v->win_n = n;
        }
        for (i = 0; i < v->win_n; i++)
        {
            struct mp4_sample s;

            vix_read_record(v->win + i * VIX_RECORD_SIZE, &s);
            nkeys += s.key;
        }
    }
    if (!nkeys)
        return MP4_E_TABLE;
    v->ops.ctx = v;
    v->ops.sample = op_sample;
    v->ops.push = op_push;
    v->ops.post = op_post;
    v->ops.skip = op_skip;
    v->ops.restart = op_restart;
    return nkeys;
}

int vpc_keys(struct vpc *v, struct vp_key *keys, uint32_t nkeys)
{
    uint32_t i, k = 0;

    for (i = 0; i < v->hdr.count; i++)
    {
        struct mp4_sample s;

        if (record(v, i, &s) < 0)
            return MP4_E_IO;
        if (!s.key)
            continue;
        if (k == nkeys)
            return MP4_E_TABLE;
        keys[k].index = i;
        keys[k].pts_us = s.pts_us;
        k++;
    }
    v->keys = keys;
    v->nkeys = k;
    return k == nkeys ? 0 : MP4_E_TABLE;
}

static void read_guard(struct vpc *v);

static void give_up(struct vpc *v, int why)
{
    say(v, "video: stopped, BCM given back", why, v->failures);
    read_guard(v);      /* session 13: a give-up at the first OPEN */
    v->be->cut();
    v->be->release();
    v->state = VPC_FAILED;
    v->fail_why = why;
}

/* The image's guard block (dev14), word by word until the port fails */
static void read_guard(struct vpc *v)
{
    uint32_t got[VPC_GUARD_WORDS], k;

    for (k = 0; k < VPC_GUARD_WORDS; k++)
        if (v->be->port->rd(VPC_GUARD_AT + 4 * k, &got[k]) < 0)
            break;
    /* a later read that the port cut short keeps the fuller one
       (session 14: "0 words" over a good read) */
    if (k < v->guard_n)
        return;
    memcpy(v->guard, got, 4 * k);
    v->guard_n = k;
    if (k == VPC_GUARD_WORDS)
    {
        /* one line for the shell's log (dev32: the detail below goes to
           stage 6's own log only, vplayer.c be_log) */
        if (v->guard[1] != v->guard_said[0] ||
            v->guard[14] < v->guard_said[1])
            say(v, "video: guard (hit at, checks)", (long)v->guard[1],
                (long)v->guard[14]);
        v->guard_said[0] = v->guard[1];
        v->guard_said[1] = v->guard[14];
        if ((v->guard[48] >> 16) == (VPC_RF_MAGIC >> 16) && !v->refused_at)
        {
            /* the image refused the stream: which check, on what */
            v->refused_at = v->guard[49];
            v->refused_nal = v->guard[48] & 31;
            say(v, "video: refused (NAL type, check file<<16|line)",
                (long)v->refused_nal, (long)v->refused_at);
            say(v, "sdram: refused: NAL length, packet length",
                v->guard[50], v->guard[54]);
            say(v, "sdram: refused: NAL bytes", v->guard[51], v->guard[52]);
            say(v, "sdram: refused: packet offset, first bytes",
                v->guard[53], v->guard[55]);
        }
        if ((v->guard[48] >> 16) == (VPC_ER_MAGIC >> 16) &&
            v->guard[49] != v->error_said)
        {
            /* the session's first decode error: which check, on what */
            v->error_said = v->guard[49];
            say(v, "video: first decode error (code, NAL type)",
                -(long)(v->guard[48] >> 8 & 0xFF), (long)(v->guard[48] & 31));
            say(v, "sdram: error: check (file<<16|line), NAL length",
                v->guard[49], v->guard[50]);
            say(v, "sdram: error: NAL bytes", v->guard[51], v->guard[52]);
            say(v, "sdram: error: packet offset, length", v->guard[53],
                v->guard[54]);
            /* the host's side of it (session 36c: a start's slices found
               no PPS): packets dropped part way, the first params packet
               at what FIFO offset */
            say(v, "video: error: host aborts, params packet at",
                v->stats.aborts, v->first_set ? (long)v->first_off : -1L);
            /* the chip's side (session 37): parameter sets seen and how
               they parsed, which ids it held at the error, the first
               packet's NAL types */
            say(v, "sdram: error: SPS|PPS<<8 seen, rc SPS|PPS<<8",
                v->guard[56] & 0xFFFF, v->guard[56] >> 16);
            say(v, "sdram: error: valid PPS ids, SPS ids", v->guard[57],
                v->guard[58]);
            say(v, "sdram: error: packets, first packet's NAL types",
                v->guard[59] & 0xFFFF, v->guard[59] >> 16);
        }
        say(v, "sdram: guard: hit at, by check", v->guard[1], v->guard[2]);
        say(v, "sdram: guard: header, event index", v->guard[4],
            v->guard[7]);
        say(v, "sdram: guard: planes y, u", v->guard[8], v->guard[9]);
        say(v, "sdram: guard: plane v, stride", v->guard[10], v->guard[11]);
        say(v, "sdram: guard: C kernels, checks", v->guard[12],
            v->guard[14]);
        /* guard[16] is "ST" << 16 | the failed checks (video.c): the
           magic alone is a pass (bench 2026-10-25 logged it as failed) */
        if (v->guard[16] && !(v->guard[16] & 0xFFFF))
            say(v, "sdram: self-test: passed", v->guard[16], 0);
        else if (v->guard[16])
        {
            say(v, "sdram: self-test: failed, first", v->guard[16],
                v->guard[17]);
            say(v, "sdram: self-test: vector, C", v->guard[18],
                v->guard[19]);
            say(v, "sdram: self-test: low SRAM hit by, at", v->guard[21],
                v->guard[22]);
            say(v, "sdram: self-test: low SRAM value", v->guard[23], 0);
            /* each failing test's failing call (its dump's header, SDRAM
               0x3E0000 + 8 KB x test), in the log even without the dump */
            {
                uint32_t t, idx;

                for (t = 0; t < 11; t++)
                    if ((v->guard[16] & (1u << t)) &&
                        v->be->port->rd(0xC03E0000u + 0x2000 * t, &idx) == 0)
                        say(v, "sdram: self-test: test, failing call", t, idx);
            }
        }
    }
}

/* The image stopped answering: what it last said it was doing (dev11),
   read word by word until the port fails; then power is cut */
static void post_mortem(struct vpc *v)
{
    static const uint32_t at[VPC_PM_WORDS] =
    {
        0x1C4, 0x1C8, 0x1CC, 0x1D0,     /* video.h VIDEO_MB_PROG.. DPB */
        0x1E0, 0x1E4, 0x1E8,            /* fault pc, sp; info */
        0x130, 0x1F8,                   /* HP_STATE; the status word */
    };
    uint32_t k;

    v->pm_losses++;
    v->pm_fast = !!(open_mode(v) & HP_OPEN_FAST_CLOCK);
    memset(v->pm, 0, sizeof(v->pm));
    for (k = 0; k < VPC_PM_WORDS; k++)
        if (v->be->port->rd(at[k], &v->pm[k]) < 0)
            break;
    v->pm_n = k;
    read_guard(v);
    say(v, "sdram: lost: progress, packets", v->pm[0], v->pm[1]);
    say(v, "sdram: lost: address, frames", v->pm[2], v->pm[3]);
    say(v, "sdram: lost: fault pc, sp", v->pm[4], v->pm[5]);
    say(v, "sdram: lost: info, state", v->pm[6], v->pm[7]);
    say(v, "sdram: lost: status, words read", v->pm[8], v->pm_n);
}

/* A host-port failure: ridden out when the image still answers with a
   valid HP_STATE within VPC_STALL_US (true), else the image is lost */
static bool ride_out(struct vpc *v)
{
    uint32_t t0 = v->be->port->now_us(), us, w;

    if (v->stalls >= VPC_STALL_MAX)
        return false;
    do
    {
        if (v->be->port->rd(HP_STATE, &w) == 0 && HP_VALID(w) &&
            (w & 0xFF) >= HP_ST_IDLE && (w & 0xFF) <= HP_ST_ERROR)
        {
            us = v->be->port->now_us() - t0;
            v->stalls++;
            if (us > v->stall_max_us)
                v->stall_max_us = us;
            say(v, "video: port stall ridden out (us, count)", (long)us,
                v->stalls);
            return true;
        }
        v->be->port->yield();
    } while (v->be->port->now_us() - t0 < VPC_STALL_US);
    return false;
}

/* What the image says about the session (dev34): its last STATUS's
   counters, its progress words, and the session's first packet as the
   FIFO holds it beside what the host wrote (session 29: 33 packets taken
   at a file's start and none shown; session 28: the first NAL refused) */
static void chip_report(struct vpc *v, bool refused)
{
    static const char *const line[2][7] =
    {
        { "sdram: still: decoded, errors", "sdram: still: concealed, queued",
          "sdram: still: progress, packet",
          "sdram: still: first packet +0, host",
          "sdram: still: first packet +0, FIFO",
          "sdram: still: first packet +8, host",
          "sdram: still: first packet +8, FIFO" },
        { "sdram: refused: decoded, errors",
          "sdram: refused: concealed, queued",
          "sdram: refused: progress, packet",
          "sdram: refused: first packet +0, host",
          "sdram: refused: first packet +0, FIFO",
          "sdram: refused: first packet +8, host",
          "sdram: refused: first packet +8, FIFO" },
    };
    const char *const *l = line[refused];
    uint32_t w[2] = { 0, 0 }, k;

    say(v, l[0], v->st_words[6], v->st_words[8]);
    say(v, l[1], v->st_words[9], v->st_words[10]);
    v->be->port->rd(0x1C4, &w[0]);      /* video.h VIDEO_MB_PROG */
    v->be->port->rd(0x1C8, &w[1]);      /* VIDEO_MB_PROG_N */
    say(v, l[2], w[0], w[1]);
    say(v, "video: chip: MHz, last packet's work us", v->st_words[15],
        v->st_words[13]);
    say(v, "video: port words read back, wrong", v->wire.words,
        v->wire.bad);
    read_guard(v);              /* a first decode error, if any */
    if (!v->first_set)
        return;
    for (k = 0; k < 2; k++)
    {
        uint32_t got[2] = { 0, 0 };

        v->be->port->rd(v->bvp.sess_fifo + v->first_off + 8 * k, &got[0]);
        v->be->port->rd(v->bvp.sess_fifo + v->first_off + 8 * k + 4,
                        &got[1]);
        say(v, l[3 + 2 * k], v->first_bytes[2 * k],
            v->first_bytes[2 * k + 1]);
        say(v, l[4 + 2 * k], got[0], got[1]);
    }
}

/* The still-picture watchdog starts again: the picture moved, or the
   session (re)started */
static void still_reset(struct vpc *v)
{
    v->shown_wall_us = v->be->port->now_us();
    v->still_mark = v->eng.stats.posted;
}

/* How long the image takes from a seek's key to its target (decoded,
   not shown: HP_FEED_HIDE), at the work per picture STATUS last said;
   playing, the video aims that far past the audio so its first picture
   isn't late (a late one drops the GOP's tail, up to 3 s still) */
/* the same from a picture at from_us on (no key: the engine decodes on) */
static int64_t catchup_from(struct vpc *v, int64_t from_us, int64_t target_us)
{
    int64_t per = v->work_avg_us ? v->work_avg_us : VPC_WORK_GUESS_US;
    int64_t fr = v->eng.frame_us ? v->eng.frame_us : 33333, s_us;

    if (from_us >= target_us)
        return 0;
    /* the pictures the aim passes need decoding too: s = (n0 + s / fr) per,
       so s = n0 per fr / (fr - per) */
    if (4 * per >= 3 * fr)
        return VPC_CATCHUP_MAX_US;
    s_us = (target_us - from_us) * per / (fr - per);
    return s_us > VPC_CATCHUP_MAX_US ? VPC_CATCHUP_MAX_US : s_us;
}

static int64_t catchup_us(struct vpc *v, int64_t target_us)
{
    uint32_t k = vp_key_before(v->keys, v->nkeys, target_us);
    struct mp4_sample s;

    if (v->eng.paused || record(v, k, &s) < 0)
        return 0;
    return catchup_from(v, s.pts_us, target_us);
}

/* "sample source failed": what failed, then on from the key after it
   (the audio plays on; the picture holds until that key's time), at most
   VPC_SRC_TRIES times without a picture between.  False: give up. */
static bool src_skip(struct vpc *v)
{
    const struct vpc_src_fail *f = &v->src_fail;
    uint32_t at = f->why ? f->sample : v->eng.pushed, k;

    say(v, "video: sample source failed", v->eng.posted, v->eng.pushed);
    say(v, "video: source: why, sample", (long)f->why, (long)f->sample);
    say(v, "video: source: size, rc", (long)f->size, f->rc);
    say(v, "video: source: bytes asked, from", (long)f->want, (long)f->at);
    v->src_fail.why = 0;
    if (++v->src_run > VPC_SRC_TRIES)
        return false;
    for (k = 0; k < v->nkeys && v->keys[k].index <= at; k++)
        ;
    if (k == v->nkeys)
        return false;           /* no key after it: nothing more to show */
    v->stats.src_skips++;
    say(v, "video: source: on from the next key (sample, try)",
        (long)v->keys[k].index, (long)v->src_run);
    v->fresh = false;
    vp_seek(&v->eng, v->keys[k].pts_us);
    still_reset(v);
    return true;
}

static void recover(struct vpc *v, int why)
{
    int rc;

    v->failures++;
    v->stats.recoveries++;
    say(v, "video: image lost, reloading", why, v->failures);
    if (!v->ub_why)
    {
        v->ub_why = VCHECK_BAD_FAULT;   /* the first-use check's token goes */
        v->ub_a = why;
    }
    unit_bad_report(v);
    post_mortem(v);
    v->be->cut();
    /* lost on PLL A: the reload runs at 27 MHz, to tell the clock apart
       (session 10) */
    if (v->pm_fast)
    {
        v->open_drop |= HP_OPEN_FAST_CLOCK;
        say(v, "video: reloading at 27 MHz", 0, 0);
    }
    if (v->failures >= 2)
    {
        give_up(v, why);
        return;
    }
    if ((rc = bring_up(v)))
    {
        give_up(v, rc);
        return;
    }
    v->resync = false;
    {
        int64_t at = v->audio_us + (v->eng.paused ? 0 : v->boot_us);

        vp_seek(&v->eng, at + catchup_us(v, at));
        still_reset(v);
    }
}

enum vpc_state vpc_start(struct vpc *v, int64_t audio_us)
{
    int rc;

    if (v->state != VPC_IDLE || !v->keys)
        return v->state;
    v->audio_us = audio_us;
    if (v->be->take() < 0)
    {
        say(v, "video: the BCM is busy", 0, 0);
        v->state = VPC_FAILED;
        v->fail_why = BVP_E_STATE;
        return v->state;
    }
    if ((rc = bring_up(v)))
    {
        give_up(v, rc);
        return v->state;
    }
    vp_open(&v->eng, &v->ops, v->hdr.count, v->keys, v->nkeys,
            v->hdr.width > 320 ? VPC_LEAD_BIG_US : VPC_LEAD_US);
    vp_seek(&v->eng, audio_us);
    still_reset(v);
    v->audio_us = audio_us;
    v->audio_moved_us = v->be->port->now_us();
    v->state = VPC_PLAYING;
    say(v, "video: playing", v->hdr.width, v->hdr.height);
    say(v, "video: params: bytes, NAL types (bit n: type n)",
        v->params_len, params_nal_mask(v));
    return v->state;
}

/* The STATUS round trip, never waited on: the reply of the last tick's
   request is read now, and a new one goes out */
static int status(struct vpc *v)
{
    uint32_t r[BVP_REPLY_WORDS], n;
    int rc;

    if (v->status_tag)
    {
        rc = bvp_take(&v->bvp, v->status_tag, r, BVP_REPLY_WORDS, &n);
        if (rc != BVP_AGAIN)
        {
            v->status_tag = 0;
            v->stats.statuses++;
            if (rc == HP_OK)
                memcpy(v->st_words, r, 4 * (n < 16 ? n : 16));
            /* the first-use check's live invalidation: a decode error or
               a concealed block (not a refused stream: the file's, not
               the unit's) */
            if (rc == HP_OK && n >= 10 && (r[8] || r[9]) && !v->ub_why &&
                r[0] != HP_ST_ERROR &&
                v->bvp.last_error != HP_E_UNSUPPORTED)
            {
                v->ub_why = r[8] ? VCHECK_BAD_ERROR : VCHECK_BAD_CONCEAL;
                v->ub_a = (long)(r[8] ? r[8] : r[9]);
            }
            if (rc == HP_OK && n >= 16 && r[15] && v->sess_mhz &&
                !v->vclk_on && r[15] < v->sess_mhz && !v->ub_why)
            {
                v->ub_why = VCHECK_BAD_CLOCK;   /* the clock fell back */
                v->ub_a = (long)r[15];
            }
            /* the session stopped decoding (an unsupported stream, its
               EV_ERROR lost): vpc_tick gives up */
            if (rc == HP_OK && n >= 1 && r[0] == HP_ST_ERROR &&
                v->bvp.last_error != HP_E_NOMEM)
                v->bvp.last_error = HP_E_UNSUPPORTED;
            if (rc == HP_OK && n >= 13 && r[7] && !v->shown_seen &&
                !v->dpb)
            {
                /* the first picture: where its frames are (video.h
                   VIDEO_MB_DPB; stage 6 says SRAM or SDRAM) */
                v->be->port->rd(0x1D0, &v->dpb);
                say(v, "sdram: frames at", v->dpb, r[7]);
            }
            if (rc == HP_OK && n >= 13 && r[7] != v->shown_seen)
            {
                /* the guard once the first pictures are up (their
                   fingerprints are in it), as stage 6 reads the
                   player's report before stopping it (dev17) */
                if (v->shown_seen < 3 && r[7] >= 3)
                    read_guard(v);
                if (!v->shown_seen && v->start_us)
                {
                    /* the first picture of the play (dev37) */
                    say(v, "video: first picture (ms after the start, boot ms)",
                        (long)((v->be->port->now_us() - v->start_us) / 1000),
                        (long)(v->boot_us / 1000));
                    v->start_us = 0;
                }
                if (v->read_direct && !v->shown_seen)
                    say(v, "video: the refused stream plays from the file "
                        "(NAL type, check)", v->refused_nal, v->refused_at);
                if (n >= 16 && r[15] != v->mhz_said)
                {
                    /* the chip's clock, when it changes (dev38) */
                    say(v, "video: chip MHz (now, was)", r[15], v->mhz_said);
                    v->mhz_said = r[15];
                }
                v->shown_seen = r[7];
                v->still_run = 0;
                v->src_run = 0;
                v->still_reload_at = 0;
                still_reset(v);
                if (v->seek_wall_us && n >= 13 &&
                    (int64_t)((uint64_t)r[12] << 32 | r[11]) >=
                        v->eng.catchup_until_us)
                {
                    /* a seek's first picture: how long, how late, and
                       how often the data wasn't there yet */
                    int64_t pts = (int64_t)((uint64_t)r[12] << 32 | r[11]);

                    say(v, "video: seek's first picture (ms, late ms)",
                        (long)((v->be->port->now_us() - v->seek_wall_us)
                               / 1000),
                        (long)((v->audio_us - pts) / 1000));
                    say(v, "video: seek's push waits (data not yet read, "
                        "FIFO full)",
                        (long)(v->stats.data_waits - v->seek_data_waits),
                        (long)(v->eng.stats.push_waits - v->seek_waits -
                               (v->stats.data_waits - v->seek_data_waits)));
                    v->seek_wall_us = 0;
                }
                if (n >= 16 && r[13])
                    v->work_avg_us = v->work_avg_us
                        ? (7 * v->work_avg_us + r[13]) / 8 : r[13];
                if (n >= 16)
                    vclk_frame(v, r);
                v->st.shown_valid = true;
                v->st.shown_pts_us = (int64_t)((uint64_t)r[12] << 32 | r[11]);
                v->st.shown_at_us = v->audio_us;
                if (v->be->frame)
                {
                    struct vpc_frame f;

                    f.pts_us = v->st.shown_pts_us;
                    f.audio_us = v->audio_us;
                    f.decoded = r[6];
                    f.shown = r[7];
                    /* words 13-15: images before p14-player send 13 */
                    f.work_us = n >= 16 ? r[13] : 0;
                    f.work_max_us = n >= 16 ? r[14] : 0;
                    f.mhz = n >= 16 ? r[15] : 0;
                    f.skipped = v->eng.stats.skipped;
                    f.dropped_gops = v->eng.stats.dropped_gops;
                    f.late_posts = v->eng.stats.late_posts;
                    v->be->frame(&f);
                }
            }
        }
    }
    if (bvp_refresh(&v->bvp))
        return BVP_E_PORT;
    v->st.in_flight = v->bvp.in_flight;
    if (!v->status_tag && v->bvp.session)
    {
        rc = bvp_send(&v->bvp, HP_OP_STATUS, &v->bvp.session, 1,
                      &v->status_tag);
        if (rc < 0)
            return rc;
        if (rc == BVP_AGAIN)
            v->status_tag = 0;
    }
    return 0;
}

enum vpc_state vpc_tick(struct vpc *v, int64_t audio_us, bool playing,
                        bool display_on, uint32_t *wait_ms)
{
    int rc;

    if (audio_us != v->audio_us || !playing)
    {
        v->audio_moved_us = v->be->port->now_us();
        v->audio_still_said = false;
    }
    v->audio_us = audio_us;
    *wait_ms = 250;
    switch (v->state)
    {
    case VPC_PLAYING:
    case VPC_DONE:
        if (!display_on)
        {
            /* dark: the picture can't be seen, so the BCM's power goes
               (FS#9890's saving); the audio plays on */
            bvp_close(&v->bvp, 0);
            v->be->cut();
            vclk_sum(v);
            v->vclk_on = false;
            v->state = VPC_ASLEEP;
            say(v, "video: dark, BCM off", (long)(audio_us / 1000), 0);
            *wait_ms = 100;
            return v->state;
        }
        if (v->bvp.lost)
        {
            recover(v, HP_EV_LOST);
            return v->state;
        }
        DEVSTATS_RD_POLL();
        if (bvp_poll(&v->bvp) < 0)
        {
            if (!ride_out(v))
                recover(v, BVP_E_PORT);
            return v->state;
        }
        if (v->bvp.evt_torn >= VPC_TORN_MAX)
        {
            say(v, "sdram: event index torn: word, polls", v->bvp.evt_raw,
                v->bvp.evt_torn);
            recover(v, BVP_E_PROTO);
            return v->state;
        }
        if (v->bvp.lost)
        {
            recover(v, HP_EV_LOST);
            return v->state;
        }
        if (v->bvp.last_error == HP_E_UNSUPPORTED && !v->shown_seen &&
            !v->read_direct)
        {
            /* refused before any picture (session 28: an episode's very
               first NAL, the same parameter sets as a file that plays):
               once more, every byte from the file, none from the audio's
               buffer, in a reloaded image */
            read_guard(v);
            chip_report(v, true);
            say(v, "video: refused, again from the file", v->refused_nal,
                v->refused_at);
            v->read_direct = true;
            v->bvp.last_error = 0;
            v->be->cut();
            if ((rc = load_params(v)) < 0 || (rc = bring_up(v)))
            {
                give_up(v, rc);
                return v->state;
            }
            vp_seek(&v->eng, audio_us + catchup_us(v, audio_us));
            still_reset(v);
            return v->state;
        }
        if (v->bvp.last_error == HP_E_UNSUPPORTED ||
            v->bvp.last_error == HP_E_NOMEM)
        {
            /* the image refused the stream (video.c decode_error: before
               any picture; or STATUS in HP_ST_ERROR, at any time): the
               audio plays alone */
            give_up(v, v->bvp.last_error);
            return v->state;
        }
        /* every tick: the beat comes round every 262 ms (bcmvp.h) */
        if (!bvp_alive(&v->bvp))
        {
            recover(v, BVP_E_TIMEOUT);
            return v->state;
        }
        if (v->state == VPC_DONE)
            return v->state;
        if ((rc = status(v)))
        {
            if (rc != BVP_E_PORT || !ride_out(v))
                recover(v, rc);
            return v->state;
        }
        vclk_poll(v);
        unit_bad_report(v);
        if (v->eng.paused == playing)
        {
            vp_pause(&v->eng, !playing);
            if (playing)
                vclk_race(v);
            /* resumed with the audio well past the last picture sent (the
               PCM ran on: session 34's step 2, 280 ms, and a GOP dropped
               30 ms later): start again there, the way a seek does, the
               key's way hidden, instead of dropping to the next key */
            if (playing && v->eng.posted &&
                audio_us - v->eng.last_posted_pts_us > VP_DROP_LATE_US)
            {
                int64_t last = v->eng.last_posted_pts_us;
                uint32_t k = vp_key_before(v->keys, v->nkeys, audio_us);

                /* the key before the audio isn't past the engine: decode
                   on from here, hidden, up to the audio (session 35's
                   step 2: the seek went back to a key 90 pictures away,
                   its first picture 512 ms late, and a GOP dropped);
                   past it, a seek is the shorter way */
                if (k < v->eng.posted)
                {
                    say(v, "video: resumed past the pictures (ms), "
                        "decoding on", (long)((audio_us - last) / 1000),
                        (long)(audio_us / 1000));
                    vp_catch_up(&v->eng, audio_us +
                                catchup_from(v, last, audio_us));
                    still_reset(v);
                    v->seek_wall_us = v->be->port->now_us() | 1;
                    v->seek_waits = v->eng.stats.push_waits;
                    v->seek_data_waits = v->stats.data_waits;
                }
                else
                {
                    say(v, "video: resumed past the pictures (ms), seeking",
                        (long)((audio_us - last) / 1000),
                        (long)(audio_us / 1000));
                    vpc_seek(v, audio_us);
                    return v->state;
                }
            }
        }
        if (v->resync)
        {
            v->resync = false;
            v->fresh = false;
            vp_seek(&v->eng, audio_us + catchup_us(v, audio_us));
            still_reset(v);
        }
        if (!vp_tick(&v->eng, audio_us, &v->st) &&
            v->eng.posted < v->hdr.count && !v->resync &&
            !v->push_port_err)
        {
            /* the index or the file failed under us */
            if (!src_skip(v))
                give_up(v, BVP_E_STATE);
            return v->state;
        }
        if (v->push_port_err)
        {
            v->push_port_err = false;
            if (!ride_out(v))
                recover(v, BVP_E_PORT);
            return v->state;
        }
        /* paused, the picture rightly stands still: the watchdog's clock
           starts again (session 29: it fired at every resume) */
        if (!playing || v->eng.paused)
            still_reset(v);
        else if (v->be->port->now_us() - v->audio_moved_us >
                 VPC_AUDIO_STILL_US)
        {
            /* the audio stands still while playing: so must the
               pictures; neither a restart nor a reload would help */
            if (!v->audio_still_said)
            {
                v->audio_still_said = true;
                v->stats.audio_stills++;
                say(v, "video: audio stands still (audio ms, posted)",
                    (long)(audio_us / 1000), v->eng.posted);
            }
            still_reset(v);
            v->still_reload_at = 0;
        }
        else if (!v->resync &&
                 v->eng.stats.posted >= v->still_mark + 3 &&
                 v->be->port->now_us() - v->shown_wall_us > VPC_STILL_US)
        {
            /* records went out and the picture stood still: say where
               the session is, then start again where the audio is */
            v->stats.stills++;
            say(v, "video: still: posted, pushed", v->eng.posted,
                v->eng.pushed);
            say(v, "video: still: in flight, statuses", v->bvp.in_flight,
                v->stats.statuses);
            say(v, "video: still: audio ms, ms since it moved",
                (long)(audio_us / 1000),
                (long)((v->be->port->now_us() - v->audio_moved_us) / 1000));
            chip_report(v, false);
            if (++v->still_run >= VPC_STILL_MAX)
            {
                /* restarts don't help: the stream can't be shown (too
                   slow to decode, or the session never moves) */
                give_up(v, BVP_E_STILL);
                return v->state;
            }
            vpc_seek(v, audio_us);
            /* and if the restart doesn't move the picture, a reload */
            v->still_reload_at = (v->be->port->now_us() +
                                  VPC_STILL_RELOAD_US) | 1;
            return v->state;
        }
        if (playing && !v->eng.paused && v->still_reload_at &&
            (int32_t)(v->be->port->now_us() - v->still_reload_at) >= 0)
        {
            /* the restart didn't help (session 30: a stall a pause didn't
               cure and leaving and coming back did): what MENU and back
               does, the image off, booted again, a new session where the
               audio is */
            v->still_reload_at = 0;
            v->stats.reloads++;
            say(v, "video: still after a restart: reloading the image",
                v->eng.posted, v->bvp.in_flight);
            chip_report(v, false);
            v->be->cut();
            if ((rc = bring_up(v)))
            {
                give_up(v, rc);
                return v->state;
            }
            {
                int64_t at = audio_us + v->boot_us;

                vp_seek(&v->eng, at + catchup_us(v, at));
            }
            still_reset(v);
            return v->state;
        }
        /* the end: the image reports the last picture up, or the last
           samples were skipped (no EOS goes out) and the rest is done */
        if (v->bvp.eos || (v->eng.posted >= v->hdr.count &&
                           !v->eos_posted && !v->bvp.in_flight))
        {
            v->state = VPC_DONE;
            say(v, "video: last frame up", v->stats.restarts, 0);
            read_guard(v);
        }
        if (!playing)
            *wait_ms = 100;
        else
        {
            /* C5 (the hardware-fit audit): sleep until the next record is
               due, not a tick at a time; at least a tick, at most four,
               so STATUS, the push-ahead and the watchdogs still turn
               (records go at 30 fps: about one wake a picture, not three) */
            int64_t due = vp_next_due_us(&v->eng, audio_us);

            /* rounded up, a millisecond over: waking a hair early only
               means a second, short wait */
            *wait_ms = due >= VPC_WAIT_MAX_MS * 1000 ? VPC_WAIT_MAX_MS :
                       due <= VPC_WAIT_MIN_MS * 1000 ? VPC_WAIT_MIN_MS :
                       (uint32_t)((due + 999) / 1000) + 1;
        }
        return v->state;

    case VPC_ASLEEP:
        *wait_ms = 100;
        if (!display_on)
            return v->state;
        v->stats.wakes++;
        if ((rc = bring_up(v)))
        {
            give_up(v, rc);
            return v->state;
        }
        v->state = VPC_PLAYING;
        /* the audio ran on through the boot (300-600 ms): seek to where
           it is now, not where it was (else the first pictures are late
           and a GOP's tail is dropped, as at start before the pre-roll) */
        if (playing)
            audio_us += v->boot_us;
        vp_seek(&v->eng, audio_us + catchup_us(v, audio_us));
        still_reset(v);
        say(v, "video: awake", (long)(audio_us / 1000), 0);
        *wait_ms = 10;
        return v->state;

    default:
        return v->state;
    }
}

void vpc_seek(struct vpc *v, int64_t audio_us)
{
    v->audio_us = audio_us;
    v->audio_moved_us = v->be->port->now_us();
    if (v->state == VPC_DONE)
        v->state = VPC_PLAYING;
    if (v->state == VPC_PLAYING)
    {
        v->fresh = false;
        vp_seek(&v->eng, audio_us + catchup_us(v, audio_us));
        still_reset(v);
        v->seek_wall_us = v->be->port->now_us() | 1;
        v->seek_waits = v->eng.stats.push_waits;
        v->seek_data_waits = v->stats.data_waits;
        vclk_race(v);
    }
}

void vpc_set_speed(struct vpc *v, int percent)
{
    vp_set_speed(&v->eng, percent);
}

int vpc_overlay_slide(struct vpc *v, uint32_t x, uint32_t y, uint32_t w,
                      uint32_t h, const uint16_t *src, uint32_t pitch,
                      uint32_t slide)
{
    bool live = (v->state == VPC_PLAYING || v->state == VPC_DONE) &&
                v->bvp.session;

    if (slide & HP_OVL_SLIDE_DOWN)
    {
        /* the band up goes down and away by the image's clock; off for
           every later OPEN at once */
        uint32_t ox = v->ovl_x, oy = v->ovl_y, ow = v->ovl_w, oh = v->ovl_h;

        if (!ow)
            return 0;
        v->ovl_w = v->ovl_h = 0;
        return live ? bvp_overlay_slide(&v->bvp, ox, oy, ow, oh, slide) : 0;
    }
    if (w && h && x < VPC_PANEL_W && y < VPC_PANEL_H)
    {
        if (x & 1)              /* whole words: even x and w */
        {
            x--;
            w++;
            src--;
        }
        w = (w + 1) & ~1u;
        v->ovl_x = x;
        v->ovl_y = y;
        v->ovl_w = w > VPC_PANEL_W - x ? VPC_PANEL_W - x : w;
        v->ovl_h = h > VPC_PANEL_H - y ? VPC_PANEL_H - y : h;
        v->ovl_src = src;
        v->ovl_pitch = pitch;
    }
    else
        v->ovl_w = v->ovl_h = 0;
    /* asleep or between sessions: the next OPEN puts it up */
    if (!live)
        return 0;
    return ovl_apply_slide(v, slide);
}

int vpc_overlay(struct vpc *v, uint32_t x, uint32_t y, uint32_t w,
                uint32_t h, const uint16_t *src, uint32_t pitch)
{
    return vpc_overlay_slide(v, x, y, w, h, src, pitch, 0);
}

int vpc_audio_config(struct vpc *v, uint32_t in, uint32_t out,
                     uint32_t size)
{
    uint32_t a[5], r[1], n;

    if (!v->bvp.session || !(v->bvp.caps & HP_CAP_AUDIO))
        return BVP_E_STATE;
    a[0] = v->bvp.session;
    a[1] = HP_AUDIO_CONFIG;
    a[2] = in;
    a[3] = out;
    a[4] = size;
    return bvp_call(&v->bvp, HP_OP_AUDIO, a, 5, r, 1, &n, 50000);
}

int vpc_audio_block(struct vpc *v, uint32_t in, uint32_t out, uint32_t off,
                    uint16_t *pcm, uint32_t bytes, uint32_t gain,
                    struct vpc_audio_times *t)
{
    uint32_t a[5], r[1], n, k, t0 = v->be->port->now_us(), t1, t2;
    int rc;

    if (!v->bvp.session)
        return BVP_E_STATE;
    /* in: written like a packet (the CPU's copy) */
    if (v->be->port->wr_block(v->bvp.fifo_host + in + off, pcm, bytes / 2) < 0)
        return BVP_E_PORT;
    a[0] = v->bvp.session;
    a[1] = HP_AUDIO_PROCESS;
    a[2] = off;
    a[3] = bytes;
    a[4] = gain;
    t1 = v->be->port->now_us();
    rc = bvp_call(&v->bvp, HP_OP_AUDIO, a, 5, r, 1, &n, 50000);
    if (rc)
        return rc;
    t2 = v->be->port->now_us();
    /* back: word by word (no read burst is known; untested, BCM #43;
       a read costs 35-85 us under load, data-paths.md R2) */
    for (k = 0; k < bytes / 4; k++)
    {
        uint32_t w;

        if (v->be->port->rd(v->bvp.fifo_host + out + off + 4 * k, &w) < 0)
            return BVP_E_PORT;
        pcm[2 * k] = (uint16_t)w;
        pcm[2 * k + 1] = (uint16_t)(w >> 16);
    }
    t->write_us = t1 - t0;
    t->call_us = t2 - t1;
    t->chip_us = n >= 1 ? r[0] : 0;
    t->read_us = v->be->port->now_us() - t2;
    return 0;
}

void vpc_stop(struct vpc *v)
{
    if (v->state != VPC_IDLE)
    {
        /* the session's errors, as the image and the port saw them */
        say(v, "video: end: decode errors, concealed blocks",
            v->st_words[8], v->st_words[9]);
        say(v, "video: end: port words read back, wrong", v->wire.words,
            v->wire.bad);
        if (v->wire.bad)
        {
            say(v, "sdram: end: first wrong word at", v->wire.first[0], 0);
            say(v, "sdram: end: it read, was written", v->wire.first[1],
                v->wire.first[2]);
        }
        /* batch 4's load timings (jobs.h J_LAT_*, open question #7): the
           merged image measures them at its first OPEN; read here while
           this session's image still runs (a later probe may restart it).
           ns x 10 a load: bytes, then words */
        {
            uint32_t blk, sd, sr, mhz;

            if (v->be->port->rd(HP_JOBS, &blk) == 0 && blk && !(blk & 3) &&
                blk < 0x140000 &&
                v->be->port->rd(blk + J_LAT_MHZ, &mhz) == 0 &&
                (mhz & J_LAT_DONE) &&
                v->be->port->rd(blk + J_LAT_SDRAM, &sd) == 0 &&
                v->be->port->rd(blk + J_LAT_SRAM, &sr) == 0)
            {
                say(v, "video: end: sdram byte, word ns x10", sd & 0xFFFF,
                    sd >> 16);
                say(v, "video: end: sram byte, word ns x10", sr & 0xFFFF,
                    sr >> 16);
                say(v, "video: end: load timings at MHz", mhz & 0xFFFF, 0);
            }
        }
        say(v, "video: end: audio stalls", v->stats.audio_stills, 0);
        if (v->vclk_on)
            vclk_sum(v);
        say(v, "video: end: stills, reloads", v->stats.stills,
            v->stats.reloads);
        if (v->stats.src_skips || v->stats.piece_waits)
            say(v, "video: end: source skips, piece waits",
                v->stats.src_skips, v->stats.piece_waits);
    }
    switch (v->state)
    {
    case VPC_PLAYING:
    case VPC_DONE:
        read_guard(v);
        /* only a session the image closed hands back: an open one keeps
           the panel (HP_OPEN_OWN_DISPLAY drops the driver's updates) */
        if (bvp_close(&v->bvp, 0) == HP_OK && v->be->hand_back &&
            v->be->hand_back() == 0)
        {
            /* the resident image stays: no power cut, no dark panel */
            say(v, "video: panel handed back, image kept", 0, 0);
            break;
        }
        /* fall through */
    case VPC_ASLEEP:
        v->be->cut();
        v->be->release();       /* the normal screen comes back */
        break;
    default:
        break;
    }
    v->state = VPC_IDLE;
}
