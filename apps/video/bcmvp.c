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

/* The PP side of the video host protocol (see bcmvp.h). */

#include <string.h>
#include "bcmvp.h"
#include "../../bcmfw/hostproto.h"

#define LCD_CMD_WORD    0x1F8           /* Rockbox's BCMA_COMMAND */
#define LCD_FAULT       0xFA170000      /* our image's fault word there */

/* Host-port cost in 16-bit transfers: an address setup (two), then two
   per 32-bit word.  A read is not a write's cost in time: it waits for
   the chip, 35-85 us under a decode's load (docs/data-paths.md R2), so
   reads are also counted on their own */
static int rd(struct bvp *v, uint32_t a, uint32_t *val)
{
    v->xfers += 4;
    v->reads++;
    return v->port->rd(a, val) < 0 ? BVP_E_PORT : 0;
}

static int wr(struct bvp *v, uint32_t a, uint32_t val)
{
    v->xfers += 4;
    return v->port->wr(a, val) < 0 ? BVP_E_PORT : 0;
}

static int wr_block(struct bvp *v, uint32_t a, const uint16_t *src,
                    uint32_t n)
{
    uint32_t piece = v->wire && v->wire->pace ? v->wire->pace : n;

    while (n)
    {
        uint32_t m = n < piece ? n : piece;

        v->xfers += 2 + m;
        if (v->port->wr_block(a, src, m) < 0)
            return BVP_E_PORT;
        a += 2 * m;
        src += m;
        n -= m;
    }
    return 0;
}

/* The word at a against the halfwords written there (bvp_wire) */
static int verify_word(struct bvp *v, uint32_t a, const uint16_t *src)
{
    struct bvp_wire *w = v->wire;
    uint32_t want = src[0] | (uint32_t)src[1] << 16, got;

    if (rd(v, a, &got))
        return BVP_E_PORT;
    w->words++;
    if (got != want && !w->bad++)
    {
        w->first[0] = a;
        w->first[1] = got;
        w->first[2] = want;
    }
    return 0;
}

/* An encoded index word: its 16-bit value, or BVP_TORN while it isn't
   a valid word (half-written, or overwritten: session 12).  Before dev14
   that was -1, BVP_E_PORT's value, so a torn index read as a dead port. */
#define BVP_TORN (-16)
static int32_t index_at(struct bvp *v, uint32_t a)
{
    uint32_t w;

    if (rd(v, a, &w))
        return BVP_E_PORT;
    v->idx_raw = w;
    return HP_VALID(w) ? (int32_t)(w & 0xFFFF) : BVP_TORN;
}

int bvp_attach(struct bvp *v, const struct bvp_port *port)
{
    uint32_t hdr[(HP_FIFO_SIZE - HP_VERSION) / 4 + 1];
    unsigned i;
    int32_t a, b;

    struct bvp_wire *wire = v->wire;

    memset(v, 0, sizeof(*v));
    v->port = port;
    v->wire = wire;
    for (i = 0; i < sizeof(hdr) / sizeof(hdr[0]); i++)
        if (rd(v, HP_VERSION + 4 * i, &hdr[i]))
            return BVP_E_PORT;
    v->version = hdr[0];
    if (v->version >> 16 != HP_MAGIC || (v->version >> 8 & 0xFF) != HP_MAJOR)
        return BVP_E_PROTO;
    v->caps = hdr[(HP_CAPS - HP_VERSION) / 4];
    v->cmd_base = hdr[(HP_CMD_BASE - HP_VERSION) / 4];
    v->cmd_size = hdr[(HP_CMD_SIZE - HP_VERSION) / 4];
    v->evt_base = hdr[(HP_EVT_BASE - HP_VERSION) / 4];
    v->evt_size = hdr[(HP_EVT_SIZE - HP_VERSION) / 4];
    v->fifo_host = hdr[(HP_FIFO_BASE - HP_VERSION) / 4];
    v->fifo_size = hdr[(HP_FIFO_SIZE - HP_VERSION) / 4];
    /* rings: powers of two that fit the 16-bit indices */
    if (!v->cmd_size || v->cmd_size & (v->cmd_size - 1) ||
        v->cmd_size > 0x8000 || !v->evt_size ||
        v->evt_size & (v->evt_size - 1) || v->evt_size > 0x8000)
        return BVP_E_PROTO;
    a = index_at(v, HP_CMD_WR);
    b = index_at(v, HP_EVT_RD);
    if (a == BVP_E_PORT || b == BVP_E_PORT)
        return BVP_E_PORT;
    if (a < 0 || b < 0)
        return BVP_E_PROTO;     /* we are the only writer: never torn */
    v->cmd_wr = a;
    v->evt_rd = b;
    v->beat_at_us = port->now_us();
    return 0;
}

/* ---- commands ---------------------------------------------------------- */

static int ring_write(struct bvp *v, const uint32_t *words, uint32_t n)
{
    uint32_t k = 0;

    while (k < n)
    {
        uint32_t pos = (v->cmd_wr + k) & (v->cmd_size - 1);
        uint32_t m = n - k;

        if (m > v->cmd_size - pos)
            m = v->cmd_size - pos;
        if (wr_block(v, v->cmd_base + 4 * pos, (const uint16_t *)(words + k),
                     2 * m))
            return BVP_E_PORT;
        k += m;
    }
    return 0;
}

int bvp_send(struct bvp *v, uint32_t op, const uint32_t *args, uint32_t n,
             int *tagp)
{
    uint32_t words[1 + HP_MAX_PAYLOAD];
    int32_t rdi = index_at(v, HP_CMD_RD);
    uint16_t tag;

    if (rdi == BVP_E_PORT)
        return BVP_E_PORT;
    if (n > HP_MAX_PAYLOAD)
        return BVP_E_STATE;
    if (rdi < 0 || v->cmd_size - ((v->cmd_wr - rdi) & 0xFFFF) < n + 1)
        return BVP_AGAIN;       /* half-written index or no room */
    tag = ++v->tag;
    if (!tag)
        tag = v->tag = 1;
    words[0] = HP_REC(op, n, tag);
    if (n)
        memcpy(&words[1], args, 4 * n);
    if (ring_write(v, words, n + 1))
        return BVP_E_PORT;
    /* the data before the index: the host port keeps writes in order */
    v->cmd_wr = (v->cmd_wr + n + 1) & 0xFFFF;
    if (wr(v, HP_CMD_WR, HP_ENC(v->cmd_wr)))
    {
        /* never published (a failed handshake writes no data): the
           record is withdrawn and the next one overwrites it, so no
           reply comes for a tag nobody waits on (dev13) */
        v->cmd_wr = (v->cmd_wr - n - 1) & 0xFFFF;
        return BVP_E_PORT;
    }
    if (tagp)
        *tagp = tag;
    return 0;
}

/* ---- events and replies ------------------------------------------------ */

static void keep_reply(struct bvp *v, uint32_t op, uint32_t tag,
                       const uint32_t *body, uint32_t n)
{
    struct bvp_reply *r = NULL;
    unsigned i;

    /* FEED replies only on an error, and nobody waits for one */
    if (op == HP_OP_FEED)
    {
        v->errors++;
        v->last_error = n ? (int32_t)body[0] : HP_E_INVAL;
        return;
    }
    for (i = 0; i < BVP_REPLIES; i++)
        if (!v->replies[i].tag)
        {
            r = &v->replies[i];
            break;
        }
    if (!r)
        r = &v->replies[tag % BVP_REPLIES];     /* nobody collected it */
    r->tag = tag;
    r->op = op;
    r->status = n ? (int32_t)body[0] : HP_E_INVAL;
    r->n = n > 1 ? (n - 1 > BVP_REPLY_WORDS ? BVP_REPLY_WORDS : n - 1) : 0;
    memcpy(r->p, body + 1, 4 * r->n);
}

static void event(struct bvp *v, uint32_t op, const uint32_t *body,
                  uint32_t n)
{
    v->events++;
    switch (op)
    {
    case HP_EV_EOS:
        v->eos = true;
        break;
    case HP_EV_LOST:
        v->lost = true;
        v->session = 0;
        break;
    case HP_EV_ERROR:
        v->errors++;
        v->last_error = n ? (int32_t)body[0] : HP_E_INVAL;
        break;
    }
}

int bvp_poll(struct bvp *v)
{
    int32_t w = index_at(v, HP_EVT_WR);
    int got = 0;

    if (w == BVP_E_PORT)
        return BVP_E_PORT;
    if (w < 0)
    {
        /* half-written: next time; vpcore gives up on one that stays
           torn (evt_torn) */
        v->evt_torn++;
        v->evt_raw = v->idx_raw;
        return 0;
    }
    v->evt_torn = 0;
    while (v->evt_rd != (uint32_t)w)
    {
        uint32_t hdr, n, i, body[HP_MAX_PAYLOAD];

        if (rd(v, v->evt_base + 4 * (v->evt_rd & (v->evt_size - 1)), &hdr))
            return BVP_E_PORT;
        n = HP_REC_LEN(hdr);
        if (n > HP_MAX_PAYLOAD || n + 1 > (((uint32_t)w - v->evt_rd) & 0xFFFF))
            break;              /* not all there yet */
        for (i = 0; i < n; i++)
            if (rd(v, v->evt_base +
                   4 * ((v->evt_rd + 1 + i) & (v->evt_size - 1)), &body[i]))
                return BVP_E_PORT;
        if (HP_REC_OP(hdr) & HP_REPLY)
            keep_reply(v, HP_REC_OP(hdr) & ~HP_REPLY, HP_REC_TAG(hdr), body, n);
        else
            event(v, HP_REC_OP(hdr), body, n);
        v->evt_rd = (v->evt_rd + 1 + n) & 0xFFFF;
        got++;
    }
    /* the records are ours once read; an index the port failed to
       write goes again at the next poll (dev13) */
    if ((got || v->evt_rd_stale) && wr(v, HP_EVT_RD, HP_ENC(v->evt_rd)))
    {
        v->evt_rd_stale = true;
        return BVP_E_PORT;
    }
    v->evt_rd_stale = false;
    return got;
}

int bvp_take(struct bvp *v, int tag, uint32_t *out, uint32_t cap,
             uint32_t *n)
{
    unsigned i;

    for (i = 0; i < BVP_REPLIES; i++)
    {
        struct bvp_reply *r = &v->replies[i];

        if (r->tag && r->tag == (uint16_t)tag)
        {
            uint32_t k = r->n < cap ? r->n : cap;

            if (out && k)
                memcpy(out, r->p, 4 * k);
            if (n)
                *n = k;
            r->tag = 0;
            return r->status;
        }
    }
    return BVP_AGAIN;
}

int bvp_call(struct bvp *v, uint32_t op, const uint32_t *args, uint32_t n,
             uint32_t *out, uint32_t cap, uint32_t *got, uint32_t timeout_us)
{
    uint32_t t0 = v->port->now_us();
    int tag = 0, st;

    if (got)
        *got = 0;
    for (;;)
    {
        st = bvp_send(v, op, args, n, &tag);
        if (st != BVP_AGAIN)
            break;
        if (bvp_poll(v) < 0)
            return BVP_E_PORT;
        if (v->port->now_us() - t0 > timeout_us)
            return BVP_E_TIMEOUT;
        v->port->yield();
    }
    if (st < 0)
        return st;
    for (;;)
    {
        if (bvp_poll(v) < 0)
            return BVP_E_PORT;
        st = bvp_take(v, tag, out, cap, got);
        if (st != BVP_AGAIN)
            return st;
        if (v->port->now_us() - t0 > timeout_us)
            return BVP_E_TIMEOUT;
        v->port->yield();
    }
}

/* ---- session ----------------------------------------------------------- */

#define T_SHORT 50000           /* HELLO, STATUS (hostproto.md section 4) */
#define T_OPEN  200000
#define T_CLOSE 100000

int bvp_hello(struct bvp *v)
{
    uint32_t a = HP_MAJOR << 8 | HP_MINOR, r[3], n;
    int st = bvp_call(v, HP_OP_HELLO, &a, 1, r, 3, &n, T_SHORT);

    if (st == HP_OK && (n < 2 || !(r[1] & HP_CAP_H264_BP)))
        return HP_E_UNSUPPORTED;
    return st;
}

static void reset_fifo(struct bvp *v)
{
    v->head = v->n = 0;
    v->wpos = v->fill = 0;
    v->rd_seen = 0;
    v->done_units = 0;
    v->in_flight = 0;
    v->eos = false;
}

int bvp_open(struct bvp *v, uint32_t w, uint32_t h, uint32_t mode_flags)
{
    uint32_t a[5], r[6], n;
    int st;

    a[0] = HP_CODEC_H264;
    a[1] = w | h << 16;
    a[2] = mode_flags;
    a[3] = 0;
    a[4] = 320 | 240 << 16;
    v->sd_n = 0;
    memset(v->sd, 0, sizeof(v->sd));
    st = bvp_call(v, HP_OP_OPEN, a, 5, r, 6, &n, T_OPEN);
    if (st != HP_OK)
    {
        if (st == HP_E_NOMEM && n >= 4)
        {
            v->sd_n = n < 5 ? n : 5;
            memcpy(v->sd, r, 4 * v->sd_n);
        }
        return st;
    }
    if (n >= 5)
    {
        v->sd[0] = r[4];
        v->sd[4] = n >= 6 ? r[5] : 0;
        v->sd_n = 1;
    }
    if (n < 4 || !r[2] || r[2] & (r[2] - 1) || r[1] < v->fifo_host ||
        !r[3])
        return BVP_E_PROTO;
    v->session = r[0];
    v->sess_fifo = r[1];
    v->sess_fifo_size = r[2];
    v->depth = r[3];
    v->lost = false;
    reset_fifo(v);
    return HP_OK;
}

int bvp_overlay_slide(struct bvp *v, uint32_t x, uint32_t y, uint32_t w,
                      uint32_t h, uint32_t slide)
{
    uint32_t a[4], r[1], n, k = 3;

    if (!v->session)
        return BVP_E_STATE;
    if (!(v->caps & HP_CAP_OVERLAY))
        return HP_E_UNKNOWN;
    a[0] = v->session;
    a[1] = x | y << 16;
    a[2] = w | h << 16;
    if (slide && (v->caps & HP_CAP_OVERLAY_SLIDE))
        a[k++] = slide;
    else if (slide & HP_OVL_SLIDE_DOWN)
        a[1] = a[2] = 0;                /* an image without slides: off */
    return bvp_call(v, HP_OP_OVERLAY, a, k, r, 1, &n, T_SHORT);
}

int bvp_overlay(struct bvp *v, uint32_t x, uint32_t y, uint32_t w,
                uint32_t h)
{
    return bvp_overlay_slide(v, x, y, w, h, 0);
}

int bvp_close(struct bvp *v, uint32_t flags)
{
    uint32_t a[2];
    int st;

    if (!v->session)
        return BVP_E_STATE;
    a[0] = v->session;
    a[1] = flags;
    st = bvp_call(v, HP_OP_CLOSE, a, 2, NULL, 0, NULL, T_CLOSE);
    v->session = 0;
    reset_fifo(v);
    return st;
}

/* ---- the FIFO ---------------------------------------------------------- */

#define UNITS(len)  (((len) + 63) >> 6)

int bvp_refresh(struct bvp *v)
{
    int32_t r = index_at(v, HP_FIFO_RD);

    if (r == BVP_E_PORT)
        return BVP_E_PORT;
    if (r < 0)
        return 0;
    v->done_units += (r - v->rd_seen) & 0xFFFF;
    v->rd_seen = r;
    /* the BCM consumes packets whole and in order */
    while (v->n && v->pkt[v->head].posted &&
           UNITS(v->pkt[v->head].len) <= v->done_units)
    {
        v->done_units -= UNITS(v->pkt[v->head].len);
        v->head = (v->head + 1) % BVP_MAX_PKTS;
        v->n--;
        v->in_flight--;
    }
    return 0;
}

static uint32_t used_bytes(const struct bvp *v)
{
    uint32_t i, u = 0;

    for (i = 0; i < v->n; i++)
        u += UNITS(v->pkt[(v->head + i) % BVP_MAX_PKTS].len);
    return u * 64;
}

int bvp_pkt_begin(struct bvp *v, uint32_t len)
{
    struct bvp_pkt *p;

    if (!v->session || !len || len > v->sess_fifo_size)
        return BVP_E_STATE;
    if (v->n == BVP_MAX_PKTS ||
        v->sess_fifo_size - used_bytes(v) < UNITS(len) * 64)
    {
        if (bvp_refresh(v))
            return BVP_E_PORT;
        if (v->n == BVP_MAX_PKTS ||
            v->sess_fifo_size - used_bytes(v) < UNITS(len) * 64)
            return BVP_AGAIN;
    }
    p = &v->pkt[(v->head + v->n) % BVP_MAX_PKTS];
    p->off = v->wpos;
    p->len = len;
    p->posted = false;
    v->n++;
    v->fill = 0;
    return 0;
}

int bvp_pkt_write(struct bvp *v, const void *buf, uint32_t len)
{
    const struct bvp_pkt *p = &v->pkt[(v->head + v->n - 1) % BVP_MAX_PKTS];
    const uint16_t *src = buf;
    uint32_t size = v->sess_fifo_size;

    if (!v->n || v->fill + len > p->len)
        return BVP_E_STATE;
    while (len)
    {
        uint32_t pos = (p->off + v->fill) & (size - 1);
        uint32_t m = len, hw;

        if (m > size - pos)
            m = size - pos;     /* the packet wraps the FIFO's end */
        hw = ((m + 3) >> 2) << 1;
        if (wr_block(v, v->sess_fifo + pos, src, hw))
            return BVP_E_PORT;
        if (v->wire && v->wire->verify &&
            (verify_word(v, v->sess_fifo + pos, src) ||
             (hw > 2 && verify_word(v, v->sess_fifo + pos + 2 * hw - 4,
                                    src + hw - 2))))
            return BVP_E_PORT;
        src += m >> 1;          /* m is a multiple of 4 unless it's the last */
        v->fill += m;
        len -= m;
    }
    return 0;
}

void bvp_pkt_abort(struct bvp *v)
{
    if (v->n && !v->pkt[(v->head + v->n - 1) % BVP_MAX_PKTS].posted)
    {
        v->n--;
        v->fill = 0;
    }
}

void bvp_pkt_end(struct bvp *v)
{
    const struct bvp_pkt *p = &v->pkt[(v->head + v->n - 1) % BVP_MAX_PKTS];

    v->wpos = (p->off + UNITS(p->len) * 64) & (v->sess_fifo_size - 1);
}

uint32_t bvp_unposted(const struct bvp *v)
{
    return v->n - v->in_flight;
}

int bvp_feed(struct bvp *v, int64_t pts_us, uint32_t flags)
{
    struct bvp_pkt *p;
    uint32_t a[6];
    int rc;

    if (!v->session || bvp_unposted(v) == 0)
        return BVP_E_STATE;
    p = &v->pkt[(v->head + v->in_flight) % BVP_MAX_PKTS];
    if (bvp_unposted(v) == 1)
    {
        /* the newest packet, posted as soon as written (a buffer behind,
           the first-use check): its last word read back first, so the
           port's SDRAM writes are behind a read of SDRAM before the
           image reads them.  Their order against the image's reads is
           not proven on silicon (sitting 4b: the check's FEEDs right
           after their writes gave no picture); one read, and only when
           nothing was pushed after it (vbcm_model.h VBM_LAND_US) */
        uint32_t w;

        if (rd(v, v->sess_fifo + ((p->off + ((p->len - 1) & ~3u)) &
                                  (v->sess_fifo_size - 1)), &w))
            return BVP_E_PORT;
    }
    a[0] = v->session;
    a[1] = p->off;
    a[2] = p->len;
    a[3] = (uint32_t)pts_us;
    a[4] = (uint32_t)((uint64_t)pts_us >> 32);
    a[5] = flags & ~HP_FEED_ACK;    /* errors reply anyway */
    rc = bvp_send(v, HP_OP_FEED, a, 6, NULL);
    if (rc)
        return rc;
    p->posted = true;
    v->in_flight++;
    return 0;
}

/* ---- liveness ---------------------------------------------------------- */

bool bvp_alive(struct bvp *v)
{
    uint32_t now = v->port->now_us(), w, cmd;

    if (rd(v, HP_STATE, &w) || rd(v, LCD_CMD_WORD, &cmd) || cmd == LCD_FAULT)
        return false;
    if (HP_VALID(w))
    {
        uint32_t beat = w >> 8 & 0xFF;

        if (!v->beat_valid || beat != v->beat)
        {
            v->beat = beat;
            v->beat_valid = true;
            v->beat_at_us = now;
            v->beat_same = 0;
        }
        else
            v->beat_same++;
    }
    return v->beat_same < 2 || now - v->beat_at_us < BVP_DEAD_US;
}
