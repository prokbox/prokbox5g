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

/* The first-use video check (vcheck.h; docs/research/bcm2722/
 * first-use-check.md).  Pure logic over the VP client. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vcheck.h"
#include "vclk.h"
#include "../../bcmfw/hostproto.h"

const char *const vcheck_bad_name[VCHECK_BADS] =
{
    "none", "decode-error", "concealed", "clock-fallback", "fault", "jobs",
};

const char *const vcheck_why_name[VCHECK_WHYS] =
{
    "ok", "no-vclk", "open", "switch", "fallback", "sdram", "decode", "sum",
    "clock", "slow", "chip", "addr", "stream", "nopic", "feed",
};

/* ---- the token ---------------------------------------------------------- */

/* the next line of text[*pos, n) into line (NUL-ended, cut at cap) */
static bool next_line(const char *text, size_t n, size_t *pos, char *line,
                      size_t cap)
{
    size_t k = 0;

    if (*pos >= n)
        return false;
    while (*pos < n && text[*pos] != '\n')
    {
        if (k + 1 < cap)
            line[k++] = text[*pos];
        (*pos)++;
    }
    if (*pos < n)
        (*pos)++;               /* the newline */
    while (k && line[k - 1] == '\r')
        k--;
    line[k] = '\0';
    return true;
}

static void copy_field(char *dst, size_t cap, const char *src)
{
    size_t k;

    for (k = 0; k + 1 < cap && src[k] && (unsigned char)src[k] >= ' '; k++)
        dst[k] = src[k];
    dst[k] = '\0';
}

int vcheck_token_parse(const char *text, size_t n, struct vcheck_token *t)
{
    char line[64], *end;
    size_t pos = 0;
    bool image = false, band = false;
    unsigned long v;

    memset(t, 0, sizeof(*t));
    if (!text || n > VCHECK_TOKEN_MAX ||
        !next_line(text, n, &pos, line, sizeof(line)) ||
        strcmp(line, "vcheck 1"))
        return -1;
    while (next_line(text, n, &pos, line, sizeof(line)))
    {
        if (!strncmp(line, "image ", 6))
        {
            v = strtoul(line + 6, &end, 16);
            if (end != line + 14 || *end)
                return -1;
            t->image_fnv = (uint32_t)v;
            image = true;
        }
        else if (!strncmp(line, "band ", 5))
        {
            v = strtoul(line + 5, &end, 10);
            if (end == line + 5 || *end || v < 1 || v >= VCLK_BANDS)
                return -1;
            t->band = (int)v;
            band = true;
        }
        else if (!strncmp(line, "build ", 6))
            copy_field(t->build, sizeof(t->build), line + 6);
        else if (!strncmp(line, "date ", 5))
            copy_field(t->date, sizeof(t->date), line + 5);
        else if (line[0])
            return -1;
    }
    return image && band && t->image_fnv ? 0 : -1;
}

int vcheck_token_format(const struct vcheck_token *t, char *buf, size_t n)
{
    int len = snprintf(buf, n, "vcheck 1\nimage %08lx\nband %d\nbuild %s\n"
                       "date %s\n", (unsigned long)t->image_fnv, t->band,
                       t->build[0] ? t->build : "-",
                       t->date[0] ? t->date : "-");

    return len < 0 || (size_t)len >= n || len > VCHECK_TOKEN_MAX ? -1 : len;
}

int vcheck_unit_band(const struct vcheck_token *t, uint32_t image_fnv)
{
    if (!t || !image_fnv || t->image_fnv != image_fnv || t->band < 1 ||
        t->band >= VCLK_BANDS)
        return -1;
    return t->band;
}

/* ---- the ladder --------------------------------------------------------- */

int vcheck_ladder_next(const uint8_t *ladder, int n, int band)
{
    int k;

    if (band < 0)
        return n > 0 ? ladder[0] : -1;
    for (k = 0; k < n; k++)
        if (ladder[k] < band)
            return ladder[k];
    return -1;
}

/* ---- the stream --------------------------------------------------------- */

static uint32_t le32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

uint32_t vcheck_fnv_bytes(const uint8_t *p, uint32_t n)
{
    uint32_t h = VCHECK_SUM_START;

    while (n--)
        h = (h ^ *p++) * 0x01000193u;
    return h;
}

int vcheck_stream_parse(struct vcheck_stream *s, const uint8_t *buf,
                        uint32_t bytes)
{
    uint32_t k, at, len;

    memset(s, 0, sizeof(*s));
    if (!buf || bytes < 16 || le32(buf) != VCHECK_MAGIC)
        return -1;
    s->w = buf[4] | buf[5] << 8;
    s->h = buf[6] | buf[7] << 8;
    s->pics = le32(buf + 8);
    s->frame_us = le32(buf + 12);
    if (!s->pics || s->pics > VCHECK_MAX_PICS || s->w & 7 || s->h & 15 ||
        !s->w || !s->h || s->w > VCHECK_PANEL_W || s->h > VCHECK_PANEL_H ||
        16 + 4 * s->pics > bytes)
        return -1;
    at = 16 + 4 * s->pics;
    for (k = 0; k < s->pics; k++)
    {
        len = le32(buf + 16 + 4 * k);
        if (!len || len > bytes - at)
            return -1;
        at += (len + 3) & ~3u;
        if (at > bytes)
            return -1;
    }
    if (at != bytes)
        return -1;
    s->base = buf;
    s->bytes = bytes;
    return 0;
}

const uint8_t *vcheck_stream_pkt(const struct vcheck_stream *s, uint32_t k,
                                 uint32_t *len)
{
    uint32_t at = 16 + 4 * s->pics, j;

    for (j = 0; j < k; j++)
        at += (le32(s->base + 16 + 4 * j) + 3) & ~3u;
    *len = le32(s->base + 16 + 4 * k);
    return s->base + at;
}

/* ---- the check ---------------------------------------------------------- */

static void say(const struct vcheck_io *io, const char *line)
{
    if (io->log)
        io->log(io->ctx, line);
}

/* the chip's clock record (clock.h struct clock_gov, HP_VCLK_R_*) */
static int vclk_call(const struct vcheck_io *io, uint32_t sub, uint32_t *r)
{
    uint32_t a[2] = { HP_VCLK_MAGIC, sub }, n = 0;
    int rc = bvp_call(io->bvp, HP_OP_VCLK, a, 2, r, BVP_REPLY_WORDS, &n,
                      VCHECK_CALL_US);

    if (rc == HP_OK && (n < HP_VCLK_WORDS || r[HP_VCLK_R_BAND] >= VCLK_BANDS))
        rc = HP_E_INVAL;
    return rc;
}

/* a failure that means no answer from the chip */
static bool dead_rc(int rc)
{
    return rc == BVP_E_PORT || rc == BVP_E_TIMEOUT || rc == BVP_E_PROTO;
}

static uint32_t sdc_word(uint32_t off)
{
    return 0xA55A0000u ^ off ^ (off << 13);     /* vpcore.c's pattern */
}

/* stage 6's readback over the session's FIFO (vpcore.c sdram_check) */
static int sdram_readback(const struct vcheck_io *io, struct vcheck_band *b)
{
    const struct bvp_port *pt = io->port;
    uint16_t *hw = io->scratch;
    uint32_t step = io->bvp->sess_fifo_size / VCHECK_SDC_BLOCKS, i, k, off,
             got;

    for (i = 0; i < VCHECK_SDC_BLOCKS; i++)
    {
        off = i * step;
        for (k = 0; k < 256; k++)
        {
            uint32_t w = sdc_word(off + 4 * k);

            hw[2 * k] = w & 0xFFFF;
            hw[2 * k + 1] = w >> 16;
        }
        if (pt->wr_block(io->bvp->sess_fifo + off, hw, 512) < 0)
            return BVP_E_PORT;
    }
    for (i = 0; i < VCHECK_SDC_BLOCKS; i++)
        for (k = 0; k < VCHECK_SDC_READS; k++)
        {
            off = i * step + k * 340 / 4 * 4;
            if (pt->rd(io->bvp->sess_fifo + off, &got) < 0)
                return BVP_E_PORT;
            b->sd_words++;
            if (got != sdc_word(off) && !b->sd_bad++)
            {
                b->got = got;
                b->want = sdc_word(off);
            }
        }
    return 0;
}

static int overlay(const struct vcheck_io *io)
{
    uint32_t r;

    if (!io->screen)
        return 0;
    for (r = 0; r < VCHECK_PANEL_H; r++)
        if (io->port->wr_block(VCHECK_HOST_FB + 2 * r * VCHECK_PANEL_W,
                               io->screen + r * io->pitch,
                               VCHECK_PANEL_W) < 0)
            return BVP_E_PORT;
    return bvp_overlay(io->bvp, 0, 0, VCHECK_PANEL_W, VCHECK_PANEL_H);
}

/* where the decoded pictures are: from the guard's first-picture words
   (the planes' offsets and the stride, the same for every frame) */
struct planes
{
    uint32_t y0, du, dv, stride, frame;
};

static int first_planes(const struct vcheck_io *io,
                        const struct vcheck_stream *s, uint32_t y,
                        struct planes *p)
{
    uint32_t g[4], k;

    for (k = 0; k < 4; k++)
        if (io->port->rd(VCHECK_GUARD + 4 * (8 + k), &g[k]) < 0)
            return BVP_E_PORT;
    /* video.c present(): GUARD(8..11) = Y, U, V, the luma stride; h264.c
       h264_layout(): U at Y + 256 a padded macroblock, V at + 320, a
       frame every 384 */
    if (g[0] != y || g[3] < s->w || g[3] & 3 || g[1] <= g[0] ||
        g[2] <= g[1] || (g[1] - g[0]) % 256 || (g[1] - g[0]) / 256 * 320 !=
        g[2] - g[0] || g[1] - g[0] < g[3] * s->h)
        return -1;
    p->y0 = g[0];
    p->du = g[1] - g[0];
    p->dv = g[2] - g[0];
    p->stride = g[3];
    p->frame = p->du / 256 * 384;
    return 0;
}

static int pic_sum(const struct vcheck_io *io, const struct vcheck_stream *s,
                   const struct planes *p, uint32_t y, uint32_t k,
                   uint32_t *sum)
{
    const uint32_t base[3] = { y, y + p->du, y + p->dv };
    uint32_t h = VCHECK_SUM_START, pl, m, r, c, w;

    /* the sample (vcheck_sample_row): one row of each plane a macroblock
       row, not the whole planes (sitting 4c: 8 s of reads) */
    for (pl = 0; pl < 3; pl++)
    {
        uint32_t words = (pl ? s->w / 2 : s->w) / 4;
        uint32_t stride = pl ? p->stride / 2 : p->stride;

        for (m = 0; m < VCHECK_MB_ROWS(s->h); m++)
        {
            r = vcheck_sample_row(pl, m, k);
            for (c = 0; c < words; c++)
            {
                if (io->port->rd(base[pl] + r * stride + 4 * c, &w) < 0)
                    return BVP_E_PORT;
                h = vcheck_sum_word(h, w);
            }
        }
    }
    *sum = h;
    return 0;
}

/* The whole stream into the FIFO before the first FEED, as the player
   pushes ahead of its posts (vpcore.c): every packet's bytes are in
   SDRAM well before the image is told to read them.  Then each packet's
   first and last word are read back through the port (bvp_wire's
   verify, proven on silicon): the bytes are where the FEED will say,
   and the host's SDRAM writes are behind a read of SDRAM before any
   FEED goes out.  Sitting 4b: a FEED sent the moment its packet was
   written gave no picture; how the port's SDRAM writes order against
   the image's reads is not proven on silicon, so vbcm_model.c refuses a
   FEED of bytes not yet behind such a read or a time */
static int push_all(const struct vcheck_io *io, const struct vcheck_stream *s,
                    struct vcheck_band *b)
{
    struct bvp *v = io->bvp;
    uint32_t k, len, i, at, got, want;
    const uint8_t *pkt;
    int rc;

    for (k = 0; k < s->pics; k++)
    {
        pkt = vcheck_stream_pkt(s, k, &len);
        rc = bvp_pkt_begin(v, len);
        if (rc)
            return rc == BVP_AGAIN ? (b->why = VCHECK_W_STREAM, -1) : rc;
        if ((rc = bvp_pkt_write(v, pkt, len)) < 0)
        {
            bvp_pkt_abort(v);
            return rc;
        }
        bvp_pkt_end(v);
    }
    for (k = 0; k < s->pics; k++)
    {
        const struct bvp_pkt *p = &v->pkt[(v->head + k) % BVP_MAX_PKTS];

        pkt = vcheck_stream_pkt(s, k, &len);
        for (i = 0; i < 2; i++)
        {
            /* the last word as written: the file pads each packet to 4 */
            at = i ? (len - 1) & ~3u : 0;
            want = le32(pkt + at);
            if (io->port->rd(v->sess_fifo + ((p->off + at) &
                                             (v->sess_fifo_size - 1)),
                             &got) < 0)
                return BVP_E_PORT;
            b->sd_words++;
            if (got != want && !b->sd_bad++)
            {
                b->got = got;
                b->want = want;
            }
        }
    }
    return 0;
}

/* The image's own words when a picture didn't come (vcheck_band_line's
   parts 2 and 3); best effort, the port may be gone */
static void diag(const struct vcheck_io *io, struct vcheck_band *b,
                 const uint32_t *r, uint32_t n, uint32_t replies)
{
    static const uint8_t guard[3] = { 59, 56, 48 };
    uint32_t k;

    memset(b->diag, 0, sizeof(b->diag));
    if (n >= 11)
    {
        b->diag[0] = r[6];
        b->diag[1] = r[10];
        b->diag[2] = r[2];
    }
    io->port->rd(VCHECK_MB_PROG, &b->diag[3]);
    for (k = 0; k < 3; k++)
        io->port->rd(VCHECK_GUARD + 4 * guard[k], &b->diag[4 + k]);
    io->port->rd(HP_FIFO_RD, &b->diag[7]);
    b->diag[8] = replies;
    b->diag_n = 9;
}

/* FEED picture k (its bytes already in the FIFO: push_all) and wait for
   the image to have finished its packet, the player's way (vpcore.c
   status()): FIFO_RD, which the image moves only once the packet and its
   picture are through, and one STATUS in flight, collected, at most
   every VCHECK_POLL_US; the STATUS sent after FIFO_RD moved says how
   many pictures came.  r gets those STATUS words, *y VIDEO_MB_PROG_A
   (the picture's luma, written at its CONVERT and left there while the
   image is idle) */
static int feed_one(const struct vcheck_io *io, const struct vcheck_stream *s,
                    uint32_t k, uint32_t *r, uint32_t *y, struct vcheck_band *b)
{
    struct bvp *v = io->bvp;
    uint32_t n = 0, t0, polled, replies = 0, now;
    int rc, tag = 0;
    bool taken = false, sent_after = false;

    rc = bvp_feed(v, (int64_t)k * s->frame_us, k ? 0 : HP_FEED_KEY);
    if (rc)
        return rc == BVP_AGAIN || rc == BVP_E_STATE
               ? (b->why = VCHECK_W_STREAM, -1) : rc;
    t0 = io->port->now_us();
    polled = t0 - VCHECK_POLL_US;
    memset(r, 0, 16 * sizeof(*r));
    for (;;)
    {
        now = io->port->now_us();
        if (now - t0 > VCHECK_PIC_WAIT_US)
        {
            diag(io, b, r, n, replies);
            /* answered and never took the FEED: not the unit's; else
               the decode never ended or the image stopped answering:
               the chip */
            if (replies && !taken && n >= 3 && r[2] < k + 1)
            {
                b->why = VCHECK_W_FEED;
                return -1;
            }
            return BVP_E_TIMEOUT;
        }
        if (now - polled < VCHECK_POLL_US)
        {
            io->port->yield();
            continue;
        }
        polled = now;
        if (bvp_poll(v) < 0)
            return BVP_E_PORT;
        if (v->last_error)
        {
            b->why = VCHECK_W_DECODE;
            b->rc = v->last_error;
            return -1;
        }
        if (v->lost)
            return BVP_E_PROTO;     /* the session ended under us */
        if (tag)
        {
            rc = bvp_take(v, tag, r, BVP_REPLY_WORDS, &n);
            if (rc != BVP_AGAIN)
            {
                tag = 0;
                replies++;
                if (rc != HP_OK)
                    return rc > 0 ? (b->why = VCHECK_W_CHIP, b->rc = rc, -1)
                                  : rc;
                if (n >= 1 && r[0] == HP_ST_ERROR)
                {
                    b->why = VCHECK_W_DECODE;
                    b->rc = HP_ST_ERROR;
                    return -1;
                }
                if (sent_after)
                    break;          /* asked once the packet was through */
            }
        }
        if (bvp_refresh(v))
            return BVP_E_PORT;
        if (!v->in_flight)
            taken = true;
        if (!tag)
        {
            rc = bvp_send(v, HP_OP_STATUS, &v->session, 1, &tag);
            if (rc < 0)
                return rc;
            if (rc == BVP_AGAIN)
                tag = 0;
            else
                sent_after = taken;
        }
    }
    if (n < 16 || r[6] > k + 1)
    {
        b->why = VCHECK_W_ADDR;     /* no words, or more pictures than
                                       packets */
        return -1;
    }
    if (r[6] < k + 1)
    {
        /* the packet went through, no picture came, no error said */
        diag(io, b, r, n, replies);
        b->why = VCHECK_W_NOPIC;
        return -1;
    }
    if (io->port->rd(VCHECK_MB_PROG_A, y) < 0)
        return BVP_E_PORT;
    return 0;
}

static void band_run(const struct vcheck_io *io, const struct vcheck_stream *s,
                     int band, struct vcheck_band *b)
{
    struct bvp *v = io->bvp;
    uint32_t r[BVP_REPLY_WORDS], st[BVP_REPLY_WORDS], k, y = 0, sum,
             fails0 = 0, sum_us = 0, sum_n = 0;
    uint32_t want_mhz = vclk_band_mhz[band];
    struct planes p = { 0, 0, 0, 0, 1 };
    bool top = band == VCLK_TOP, vclk = v->caps & HP_CAP_VCLK;
    int rc;

    b->why = VCHECK_OK;
    v->last_error = 0;
    if (!vclk && !top)
    {
        b->why = VCHECK_W_NOVCLK;
        return;
    }
    if (vclk)
    {
        if ((rc = vclk_call(io, HP_VCLK_STAT, r)) != HP_OK)
            goto failed;
        fails0 = r[HP_VCLK_R_FAILS];
    }
    rc = bvp_open(v, s->w, s->h, HP_MODE_FIT | HP_OPEN_OWN_DISPLAY |
                  (top ? HP_OPEN_FAST_CLOCK : 0));
    if (rc != HP_OK)
    {
        if (!dead_rc(rc))
        {
            b->why = VCHECK_W_OPEN;
            b->rc = rc;
            return;
        }
        goto failed;
    }
    /* the band, then the chip's own record of it */
    if (vclk)
    {
        if (!top && (rc = vclk_call(io, 1 + band, r)) != HP_OK)
            goto failed_open;
        if ((rc = vclk_call(io, HP_VCLK_STAT, r)) != HP_OK)
            goto failed_open;
        b->mhz = r[HP_VCLK_R_MHZ] ? r[HP_VCLK_R_MHZ] : 27;
        b->fails = r[HP_VCLK_R_FAILS] - fails0;
        if (b->fails)
        {
            b->why = VCHECK_W_FALLBACK;
            goto close;
        }
        if ((int)r[HP_VCLK_R_BAND] != band || b->mhz != want_mhz)
        {
            b->why = VCHECK_W_SWITCH;
            b->got = r[HP_VCLK_R_BAND];
            goto close;
        }
    }
    if ((rc = overlay(io)) != HP_OK && rc != HP_E_UNKNOWN)
        goto failed_open;
    if ((rc = sdram_readback(io, b)) < 0)
        goto failed_open;
    if (b->sd_bad)
    {
        b->why = VCHECK_W_SDRAM;
        goto close;
    }
    rc = push_all(io, s, b);
    if (rc == -1)
        goto close;                 /* why set */
    if (rc)
        goto failed_open;
    if (b->sd_bad)
    {
        b->why = VCHECK_W_SDRAM;    /* a packet read back wrong */
        goto close;
    }
    for (k = 0; k < s->pics; k++)
    {
        b->pic = (uint8_t)k;
        rc = feed_one(io, s, k, st, &y, b);
        if (rc == -1)
            goto close;             /* why set */
        if (rc)
            goto failed_open;
        b->errors = st[8];
        b->concealed = st[9];
        if (st[8] || st[9])
        {
            b->why = VCHECK_W_DECODE;
            goto close;
        }
        b->mhz = st[15];
        if (st[15] != want_mhz)
        {
            b->why = VCHECK_W_CLOCK;
            goto close;
        }
        if (st[13] > b->work_max_us)
            b->work_max_us = st[13];
        if (k)
        {
            sum_us += st[13];
            sum_n++;
        }
        if (st[13] > VCHECK_PIC_MAX_US)
        {
            b->why = VCHECK_W_SLOW;
            goto close;
        }
        if (!k && (rc = first_planes(io, s, y, &p)) != 0)
        {
            if (rc != -1)
                goto failed_open;
            b->why = VCHECK_W_ADDR;
            b->got = y;
            goto close;
        }
        if ((y > p.y0 ? y - p.y0 : p.y0 - y) % p.frame)
        {
            b->why = VCHECK_W_ADDR;
            b->got = y;
            goto close;
        }
        if ((rc = pic_sum(io, s, &p, y, k, &sum)) < 0)
            goto failed_open;
        if (sum != io->sums[k])
        {
            b->why = VCHECK_W_SUM;
            b->got = sum;
            b->want = io->sums[k];
            goto close;
        }
        b->pics++;
    }
    b->work_mean_us = sum_n ? sum_us / sum_n : 0;
    if (b->work_mean_us > VCHECK_MEAN_MAX_US)
    {
        b->why = VCHECK_W_SLOW;
        goto close;
    }
    /* still at the band, and no fallback since */
    if (vclk)
    {
        if ((rc = vclk_call(io, HP_VCLK_STAT, r)) != HP_OK)
            goto failed_open;
        if (r[HP_VCLK_R_FAILS] != fails0 || (int)r[HP_VCLK_R_BAND] != band)
        {
            b->fails = r[HP_VCLK_R_FAILS] - fails0;
            b->why = b->fails ? VCHECK_W_FALLBACK : VCHECK_W_SWITCH;
            b->got = r[HP_VCLK_R_BAND];
        }
    }
close:
    if ((rc = bvp_close(v, 0)) != HP_OK && dead_rc(rc))
        goto failed;
    return;
failed_open:
    if (!dead_rc(rc) && rc > 0)
    {
        /* the image answered, with a refusal: not a dead chip */
        b->why = VCHECK_W_CHIP;
        b->rc = rc;
        bvp_close(v, 0);
        return;
    }
    v->session = 0;
failed:
    b->why = VCHECK_W_CHIP;
    b->rc = rc;
}

int vcheck_band_line(const struct vcheck_band *b, int part, char *buf,
                     size_t n)
{
    const uint32_t *d = b->diag;

    if (part >= 2 && !b->diag_n)
        return 0;
    if (part == 2)
        return snprintf(buf, n, "vck x d%lu q%lu p%lu ph%08lx g59 %08lx",
                        (unsigned long)d[0], (unsigned long)d[1],
                        (unsigned long)d[2], (unsigned long)d[3],
                        (unsigned long)d[4]);
    if (part == 3)
        return snprintf(buf, n, "vck y g56 %08lx g48 %08lx rd%lx a%lu",
                        (unsigned long)d[5], (unsigned long)d[6],
                        (unsigned long)(d[7] & 0xFFFF), (unsigned long)d[8]);
    if (!part)
        return snprintf(buf, n, "vck %u %s %s p%u rc%d %lu/%lu MHz %lums",
                        vclk_band_mhz[b->band % VCLK_BANDS],
                        b->why == VCHECK_OK ? "PASS" : "FAIL",
                        vcheck_why_name[b->why < VCHECK_WHYS ? b->why : 0],
                        b->pic, b->rc, (unsigned long)b->pics,
                        (unsigned long)b->mhz, (unsigned long)(b->us / 1000));
    return snprintf(buf, n, "vck e%lu c%lu f%lu sd%lu/%lu w%lu/%lu %08lx/%08lx",
                    (unsigned long)b->errors, (unsigned long)b->concealed,
                    (unsigned long)b->fails, (unsigned long)b->sd_words,
                    (unsigned long)b->sd_bad, (unsigned long)b->work_mean_us,
                    (unsigned long)b->work_max_us, (unsigned long)b->got,
                    (unsigned long)b->want);
}

int vcheck_run(const struct vcheck_io *io, const struct vcheck_stream *s,
               const uint8_t *ladder, int n, struct vcheck_result *res)
{
    uint32_t t0 = io->port->now_us(), t;
    char line[200];
    int band = vcheck_ladder_next(ladder, n, -1), reboots = 0;

    memset(res, 0, sizeof(*res));
    res->band = -1;
    res->verdict = VCHECK_FAIL;
    while (band > 0 && band < VCLK_BANDS && res->n < VCHECK_MAX_TRIES)
    {
        struct vcheck_band *b = &res->tried[res->n++];

        memset(b, 0, sizeof(*b));
        b->band = (uint8_t)band;
        t = io->port->now_us();
        band_run(io, s, band, b);
        b->us = io->port->now_us() - t;
        vcheck_band_line(b, 0, line, sizeof(line));
        say(io, line);
        vcheck_band_line(b, 1, line, sizeof(line));
        say(io, line);
        if (vcheck_band_line(b, 2, line, sizeof(line)) > 0)
            say(io, line);
        if (vcheck_band_line(b, 3, line, sizeof(line)) > 0)
            say(io, line);
        if (b->why == VCHECK_OK)
        {
            res->verdict = VCHECK_PASS;
            res->band = band;
            break;
        }
        if (b->why == VCHECK_W_ADDR || b->why == VCHECK_W_STREAM ||
            b->why == VCHECK_W_NOPIC || b->why == VCHECK_W_FEED)
        {
            res->verdict = VCHECK_INCONCLUSIVE;
            break;
        }
        if (b->why == VCHECK_W_CHIP && dead_rc(b->rc))
        {
            /* the band left the image dead: start it again for the next,
               once a check.  A second death is not the unit's verdict to
               give (a check that kills the chip twice is wrong itself):
               inconclusive, so the film plays and the screen never blinks
               band after band (sitting 4b, stage 6) */
            if (reboots++ || !io->reboot || io->reboot(io->ctx) < 0)
            {
                say(io, reboots > 1 ? "vck stopped: the image died twice"
                                    : "vck the image did not start again");
                res->verdict = VCHECK_INCONCLUSIVE;
                break;
            }
        }
        if (io->port->now_us() - t0 > VCHECK_BUDGET_US)
        {
            /* the ladder may be long: the user waits at most this */
            say(io, "vck stopped: over its time budget");
            res->verdict = VCHECK_INCONCLUSIVE;
            break;
        }
        band = vcheck_ladder_next(ladder, n, band);
    }
    res->us = io->port->now_us() - t0;
    snprintf(line, sizeof(line), "vck %s band %d %d MHz tried %d %lums",
             res->verdict == VCHECK_PASS ? "PASS" : res->verdict == VCHECK_FAIL
             ? "FAIL" : "INCONCLUSIVE", res->band,
             res->band > 0 ? vclk_band_mhz[res->band] : 0, res->n,
             (unsigned long)(res->us / 1000));
    say(io, line);
    return res->verdict;
}
