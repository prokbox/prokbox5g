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

/* A model of our video image's host side (see vbcm_model.h). */

#include <string.h>
#include "vbcm_model.h"
#include "../../bcmfw/hostproto.h"

#define FIFO_SIZE   0x100000            /* VIDEO_FIFO_SIZE */
#define LCD_CMD     0x1F8

static uint32_t get32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = v;
    p[1] = v >> 8;
    p[2] = v >> 16;
    p[3] = v >> 24;
}

#define MB(a)       get32(m->sram + (a))
#define SET(a, v)   put32(m->sram + (a), (v))
#define CMD(i)      MB(HP_CMD_RING + 4 * ((i) % HP_CMD_WORDS))
#define EVT_SET(i, v) SET(HP_EVT_RING + 4 * ((i) % HP_EVT_WORDS), (v))

void vbm_init(struct vbm *m, uint8_t *sram, uint8_t *sdram, uint8_t *scratch,
              uint32_t (*now_us)(void))
{
    memset(m, 0, sizeof(*m));
    m->sram = sram;
    m->sdram = sdram;
    m->scratch = scratch;
    m->now_us = now_us;
    m->decode_us = 11000;
    m->panel_us = 6000;
    m->land_us = VBM_LAND_US;
    m->land_bytes = VBM_LAND_BYTES;
}

/* ---- the host's SDRAM writes, landing (VBM_LAND_US) ---- */

/* the oldest writes still in flight land: land_us after them, or once
   land_bytes more have been written to SDRAM behind them */
static void land(struct vbm *m)
{
    uint32_t now = m->now_us();

    while (m->pend_n)
    {
        const struct vbm_pend *p = &m->pend[m->pend_first];

        if (m->land_us && now - p->at < m->land_us &&
            m->sd_bytes - p->mark < m->land_bytes)
            break;
        m->pend_first = (m->pend_first + 1) % VBM_PENDS;
        m->pend_n--;
    }
}

static void sdram_written(struct vbm *m, uint32_t off, uint32_t len)
{
    struct vbm_pend *p;

    m->sd_bytes += len;
    if (!m->land_us || !len)
        return;
    land(m);
    p = m->pend_n ? &m->pend[(m->pend_first + m->pend_n - 1) % VBM_PENDS]
                  : NULL;
    if (!p || p->hi != off)
    {
        if (m->pend_n == VBM_PENDS)
        {
            /* the oldest lands (a stricter model keeps more) */
            m->pend_first = (m->pend_first + 1) % VBM_PENDS;
            m->pend_n--;
        }
        p = &m->pend[(m->pend_first + m->pend_n++) % VBM_PENDS];
        p->lo = off;
    }
    p->hi = off + len;          /* a run lands with its last write */
    p->at = m->now_us();
    p->mark = m->sd_bytes;
}

/* a packet at FIFO offset off (the FIFO is SDRAM's start), len bytes,
   wrapping the FIFO's end: any of it still in flight? */
static bool in_flight(struct vbm *m, uint32_t off, uint32_t len,
                      uint32_t fifo)
{
    uint32_t k, end = off + len;

    land(m);
    for (k = 0; k < m->pend_n; k++)
    {
        const struct vbm_pend *p = &m->pend[(m->pend_first + k) % VBM_PENDS];

        if (end > fifo ? (off < p->hi && fifo > p->lo) || end - fifo > p->lo
                       : off < p->hi && end > p->lo)
            return true;
    }
    return false;
}

static void publish_state(struct vbm *m)
{
    uint32_t beat = (m->now_us() >> 10) & 0xFF;

    SET(HP_STATE, HP_ENC(beat << 8 | m->state));
}

void vbm_boot(struct vbm *m)
{
    memset(m->sram, 0, VBM_SRAM_SIZE);
    m->powered = m->running = true;
    m->hang = false;            /* a reload revives a hung image */
    m->torn = false;
    m->pend_n = 0;              /* SDRAM's contents are gone anyway */
    m->boots++;
    m->cmd_rd = m->evt_wr = 0;
    m->session = m->own_display = 0;
    m->q_rd = m->q_wr = 0;
    m->state = HP_ST_IDLE;
    m->shown_pts = 0;
    if (m->no_protocol)
        return;
    SET(HP_CAPS, HP_CAP_NULL | HP_CAP_H264_BP | HP_CAP_SCALE | HP_CAP_LCD_DMA |
                 HP_CAP_OVERLAY | HP_CAP_OVERLAY_SLIDE | HP_CAP_AUDIO |
                 (m->vclk ? HP_CAP_VCLK : 0));
    SET(HP_CMD_BASE, HP_CMD_RING);
    SET(HP_CMD_SIZE, HP_CMD_WORDS);
    SET(HP_EVT_BASE, HP_EVT_RING);
    SET(HP_EVT_SIZE, HP_EVT_WORDS);
    SET(HP_FIFO_BASE, VBM_SDRAM_HOST);
    SET(HP_FIFO_SIZE, FIFO_SIZE);
    SET(HP_CMD_WR, HP_ENC(0));
    SET(HP_CMD_RD, HP_ENC(0));
    SET(HP_EVT_WR, HP_ENC(0));
    SET(HP_EVT_RD, HP_ENC(0));
    SET(HP_FIFO_RD, HP_ENC(0));
    publish_state(m);
    SET(HP_VERSION, HP_MAGIC << 16 | (m->major ? m->major : HP_MAJOR) << 8 |
                    HP_MINOR);
    SET(LCD_CMD, 0x00010001);           /* ready, as hello reports it */
}

void vbm_cut(struct vbm *m)
{
    m->powered = m->running = false;
    m->cuts++;
}

/* ---- events ---- */

static void post(struct vbm *m, uint32_t op, uint32_t tag, uint32_t status,
                 const uint32_t *p, uint32_t n)
{
    uint32_t w = MB(HP_EVT_RD), i, k = m->evt_wr;

    if (!HP_VALID(w) || HP_EVT_WORDS - ((m->evt_wr - w) & 0xFFFF) < n + 2)
    {
        m->events_lost++;
        return;
    }
    EVT_SET(k++, HP_REC(op, n + 1, tag));
    EVT_SET(k++, status);
    for (i = 0; i < n; i++)
        EVT_SET(k++, p[i]);
    m->evt_wr = k & 0xFFFF;
    SET(HP_EVT_WR, HP_ENC(m->evt_wr));
}

static void reply(struct vbm *m, uint32_t op, uint32_t tag, uint32_t st,
                  const uint32_t *p, uint32_t n)
{
    post(m, op | HP_REPLY, tag, st, p, n);
}

/* ---- decoding, by the clock ---- */

static void present(struct vbm *m, const struct vbm_pkt *p)
{
    uint32_t first = p->len, off = p->off % FIFO_SIZE;

    if (off + first > FIFO_SIZE)
        first = FIFO_SIZE - off;
    memcpy(m->scratch, m->sdram + off, first);
    memcpy(m->scratch + first, m->sdram, p->len - first);
    if (p->flags & HP_FEED_HIDE)
    {
        /* decoded, never shown (the image's present() returns early;
           the picture before it still goes up); the callback decodes it
           (flags say so), as later pictures refer to it */
        if (m->lag_one && m->held)
        {
            m->shown++;
            m->shown_pts = m->held_pts;
        }
        m->held = false;
        m->hidden++;
    }
    else if (m->lag_one)
    {
        /* the last packet's picture goes up now; this one waits for the
           next packet or EOS */
        if (m->held)
        {
            m->shown++;
            m->shown_pts = m->held_pts;
        }
        m->held = !(p->flags & HP_FEED_EOS);
        m->held_pts = p->pts_us;
        if (!m->held)
        {
            m->shown++;
            m->shown_pts = p->pts_us;
        }
    }
    else
    {
        m->shown++;
        m->shown_pts = p->pts_us;
    }
    SET(0x1D0, m->diag ? 0xFE100000u : 0x0002A920u);    /* VIDEO_MB_DPB */
    if (m->present)
        m->present(m->ctx, m->scratch, p->len, p->pts_us, p->flags);
}

void vbm_picture(struct vbm *m, const uint8_t *y, const uint8_t *u,
                 const uint8_t *v, uint32_t stride_y, uint32_t stride_c,
                 uint32_t w, uint32_t h)
{
    uint32_t frame = stride_y * h * 3 / 2, n = m->decoded;
    uint32_t at = VBM_FRAMES + (n ? n - 1 : 0) % 3 * frame, r;
    uint8_t *d = m->sram + at;

    if (VBM_FRAMES + 3 * frame > HP_CMD_RING || stride_y < w)
        return;
    for (r = 0; r < h; r++)
        memcpy(d + r * stride_y, y + r * stride_y, w);
    for (r = 0; r < h / 2; r++)
    {
        memcpy(d + stride_y * h + r * (stride_y / 2), u + r * stride_c, w / 2);
        memcpy(d + stride_y * h * 5 / 4 + r * (stride_y / 2), v + r * stride_c,
               w / 2);
    }
    if (m->pic_bad && n >= m->pic_bad)
        for (r = 0; r < h; r++)
            d[r * stride_y] ^= 0x01;
    if (!MB(0xDF000 + 4 * 8))           /* video.c present(): GUARD(8..11) */
    {
        SET(0xDF000 + 4 * 9, at + stride_y * h);
        SET(0xDF000 + 4 * 10, at + stride_y * h * 5 / 4);
        SET(0xDF000 + 4 * 11, stride_y);
        SET(0xDF000 + 4 * 8, at);
    }
    SET(0x1CC, at + m->pic_addr_skew);  /* VIDEO_MB_PROG_A at CONVERT */
    SET(0x1C4, 0x0F000000u | (n & 0xFFFFFF));   /* VIDEO_PH_IDLE */
}

void vbm_run(struct vbm *m)
{
    uint32_t now = m->now_us();

    if (!m->running || m->hang)
        return;
    while (m->session && m->q_rd != m->q_wr)
    {
        struct vbm_pkt *p = &m->q[m->q_rd % VBM_QUEUE];

        if ((int32_t)(now - p->done_at) < (int32_t)m->panel_us ||
            m->opens <= m->stuck_opens || m->boots <= m->stuck_boots)
            break;
        if (p->stale)
        {
            /* what silicon gave sitting 4b's check: the packet taken,
               no picture, no error (the bytes it read were not ours) */
            m->stale_feeds++;
            SET(0x1C4, 0x0F000000u | ((m->q_rd + 1) & 0xFFFFFF));
        }
        else
        {
            m->decoded++;
            present(m, p);
        }
        m->fifo_rd = (m->fifo_rd + ((p->len + 63) >> 6)) & 0xFFFF;
        SET(HP_FIFO_RD, HP_ENC(m->fifo_rd));
        m->q_rd++;
        m->total_packets++;
        if (p->flags & HP_FEED_EOS)
        {
            uint32_t ev[3] = { m->session, (uint32_t)p->pts_us,
                               (uint32_t)((uint64_t)p->pts_us >> 32) };

            post(m, HP_EV_EOS, 0, HP_OK, ev, 3);
        }
        if (m->die_after && m->total_packets >= m->die_after)
        {
            m->hang = true;
            return;
        }
    }
    publish_state(m);
}

/* P19 PW4: bcmfw/clock.c's bands; the decode scales with the clock */
static const uint8_t vbm_band_mhz[12] =
    { 27, 46, 59, 74, 87, 102, 116, 123, 130, 140, 147, 150 };

static uint32_t vbm_mhz(const struct vbm *m)
{
    return m->mhz ? m->mhz : vbm_band_mhz[m->band < 12 ? m->band : 11];
}

static uint32_t vbm_decode_us(const struct vbm *m)
{
    if (!m->vclk)
        return m->decode_us;
    return m->decode_us * 150u / vbm_mhz(m);
}

/* ---- commands ---- */

static void cmd(struct vbm *m, uint32_t op, uint32_t tag, const uint32_t *a,
                uint32_t n)
{
    uint32_t r[16];

    switch (op)
    {
    case HP_OP_HELLO:
        r[0] = MB(HP_VERSION);
        r[1] = MB(HP_CAPS);
        r[2] = HP_MAX_PAYLOAD;
        reply(m, op, tag, n >= 1 && (a[0] >> 8 & 0xFF) != HP_MAJOR ?
              HP_E_VERSION : HP_OK, r, 3);
        return;
    case HP_OP_OPEN:
        if (n < 3 || m->session)
        {
            reply(m, op, tag, n < 3 ? HP_E_INVAL : HP_E_BUSY, 0, 0);
            return;
        }
        if (a[0] != HP_CODEC_H264)
        {
            reply(m, op, tag, HP_E_UNSUPPORTED, 0, 0);
            return;
        }
        if (m->nomem_fast && (a[2] & HP_OPEN_FAST_CLOCK))
        {
            /* the image's words (video.h): probe failed after 4
               attempts, bit 15 seen, on PLL A; back on the crystal */
            r[0] = 0x534424F3;
            r[1] = 0x00008007;
            r[2] = 0x87654321;
            r[3] = 0x87654321;
            reply(m, op, tag, HP_E_NOMEM, r, 4);
            return;
        }
        m->opens++;
        m->session = 1;
        m->own_display = !!(a[2] & HP_OPEN_OWN_DISPLAY);
        m->fast = !!(a[2] & HP_OPEN_FAST_CLOCK);
        if (m->fast)
        {
            m->band = 11;               /* video.c video_clock */
            m->mhz = 0;
            if (m->vclk_fail_mask >> 11 & 1)
            {
                m->band = 0;            /* clock.c xtal_off */
                m->vclk_fails++;
            }
        }
        {
            uint32_t g;

            for (g = 0; g < 16; g++)    /* video.c guard_open */
                SET(0xDF000 + 4 * g, 0);
        }
        m->diag = !!(a[2] & HP_OPEN_SDRAM_DPB);
        if (m->fast)
            m->fast_opens++;
        m->fifo_next = m->fifo_rd = 0;
        m->q_rd = m->q_wr = 0;
        m->last_done = m->now_us();
        m->packets = m->bytes = m->errors = 0;
        m->decoded = m->shown = 0;
        m->held = false;
        m->ovl_xy = m->ovl_wh = 0;  /* as video_open() */
        SET(HP_FIFO_RD, HP_ENC(0));
        m->state = HP_ST_OPEN;
        r[0] = m->session;
        r[1] = VBM_SDRAM_HOST;
        r[2] = FIFO_SIZE;
        r[3] = VBM_QUEUE;
        r[4] = 0x53440111 | (m->fast ? 2 : 0);  /* minor 2: SDRAM up */
        r[5] = m->diag ? 0x4D400083u : 0;       /* minor 3: memory test */
        reply(m, op, tag, HP_OK, r, 6);
        return;
    case HP_OP_FEED:
        if (n < 6 || !m->session || a[0] != m->session)
        {
            m->errors++;
            reply(m, op, tag, n < 6 ? HP_E_INVAL : HP_E_NOSESSION, 0, 0);
            return;
        }
        m->feeds++;
        if (m->stall_after && m->feeds == m->stall_after)
            m->stall_left = m->stall_n;
        if (m->tear_after && m->feeds == m->tear_after)
            m->torn = true;
        if (m->refuse || m->state == HP_ST_ERROR)
        {
            if (m->state != HP_ST_ERROR)
            {
                uint32_t ev[2] = { a[3], a[4] };

                post(m, HP_EV_ERROR, 0, HP_E_UNSUPPORTED, ev, 2);
                m->state = HP_ST_ERROR;
            }
            return;
        }
        if (m->hang_fast && m->fast)
        {
            SET(0x1C4, 0x31000007);     /* video.h: macroblock 7 */
            m->hang = true;
            return;
        }
        if (a[1] != m->fifo_next || !a[2] || a[2] > FIFO_SIZE)
        {
            m->errors++;
            m->fifo_errors++;
            reply(m, op, tag, HP_E_FIFO, &m->fifo_next, 1);
            return;
        }
        if (m->q_wr - m->q_rd >= VBM_QUEUE)
        {
            m->errors++;
            reply(m, op, tag, HP_E_BUSY, &m->fifo_next, 1);
            return;
        }
        m->q[m->q_wr % VBM_QUEUE].off = a[1];
        m->q[m->q_wr % VBM_QUEUE].len = a[2];
        m->q[m->q_wr % VBM_QUEUE].pts_us =
            (int64_t)((uint64_t)a[4] << 32 | a[3]);
        m->q[m->q_wr % VBM_QUEUE].flags = a[5];
        m->q[m->q_wr % VBM_QUEUE].stale = m->no_pic ||
                                          in_flight(m, a[1], a[2], FIFO_SIZE);
        {
            /* decodes run back to back, each from its FEED at the
               earliest */
            uint32_t now = m->now_us();
            uint32_t start = (int32_t)(now - m->last_done) > 0 ? now
                                                               : m->last_done;

            m->last_done = start + vbm_decode_us(m);
            m->vclk_busy += vbm_decode_us(m);
            m->q[m->q_wr % VBM_QUEUE].done_at = m->last_done;
        }
        m->q_wr++;
        m->fifo_next = ((a[1] + a[2] + 63) & ~63u) % FIFO_SIZE;
        m->packets++;
        m->bytes += a[2];
        m->state = HP_ST_PLAYING;
        if (a[5] & HP_FEED_ACK)
            reply(m, op, tag, HP_OK, 0, 0);
        return;
    case HP_OP_CLOSE:
        if (n < 1 || !m->session || a[0] != m->session)
        {
            reply(m, op, tag, HP_E_NOSESSION, 0, 0);
            return;
        }
        m->closes++;
        m->band = 0;                    /* video.c video_close: the crystal */
        m->mhz = 0;
        r[0] = m->packets;
        r[1] = m->bytes;
        r[2] = m->decoded;
        r[3] = m->shown;
        r[4] = m->errors;
        m->session = m->own_display = 0;
        m->q_rd = m->q_wr;
        m->state = HP_ST_IDLE;
        reply(m, op, tag, HP_OK, r, 5);
        return;
    case HP_OP_STATUS:
        m->statuses++;
        r[0] = m->state;
        r[1] = m->session;
        r[2] = m->packets;
        r[3] = m->bytes;
        r[4] = m->errors;
        r[5] = m->events_lost;
        r[6] = m->decoded;
        r[7] = m->shown;
        r[8] = m->st_errors;
        r[9] = m->st_concealed;
        r[10] = m->q_wr - m->q_rd;
        r[11] = (uint32_t)m->shown_pts;
        r[12] = (uint32_t)((uint64_t)m->shown_pts >> 32);
        r[13] = vbm_decode_us(m);       /* the image's words 13-15 */
        r[14] = vbm_decode_us(m);
        r[15] = m->vclk ? vbm_mhz(m) : m->fast ? 150 : 27;
        reply(m, op, tag, HP_OK, r, m->session ? 16 : 6);
        return;
    case HP_OP_AUDIO:
        /* as hostproto.c cmd_audio: the rings, then a block at a gain */
        if (n == 5 && m->session && a[0] == m->session &&
            a[1] == HP_AUDIO_CONFIG && a[4] && !(a[4] & 3) &&
            a[2] + a[4] <= VBM_SDRAM_SIZE && a[3] + a[4] <= VBM_SDRAM_SIZE)
        {
            m->au_in = a[2];
            m->au_out = a[3];
            m->au_size = a[4];
            reply(m, op, tag, HP_OK, 0, 0);
            return;
        }
        if (n == 5 && m->session && a[0] == m->session &&
            a[1] == HP_AUDIO_PROCESS && m->au_size && !(a[2] & 3) &&
            !(a[3] & 3) && a[2] + a[3] <= m->au_size)
        {
            const uint8_t *src = m->sdram + m->au_in + a[2];
            uint8_t *dst = m->sdram + m->au_out + a[2];
            uint32_t k;

            for (k = 0; k < a[3]; k += 2)
            {
                int16_t x = (int16_t)(src[k] | src[k + 1] << 8);
                int16_t y = (int16_t)((x * (int32_t)a[4]) >> 16);

                dst[k] = (uint8_t)y;
                dst[k + 1] = (uint8_t)((uint16_t)y >> 8);
            }
            m->audio_blocks++;
            r[0] = 1;
            reply(m, op, tag, HP_OK, r, 1);
            return;
        }
        reply(m, op, tag, HP_E_INVAL, 0, 0);
        return;
    case HP_OP_OVERLAY:
        if (n < 3 || !m->session || a[0] != m->session)
        {
            reply(m, op, tag, HP_E_NOSESSION, 0, 0);
            return;
        }
        /* as video.c: the host frame's rectangle over the picture; a
           slide (n 4) lands at once here: down is off, up is on */
        if (n >= 4 && (a[3] & HP_OVL_SLIDE_DOWN))
            m->ovl_xy = m->ovl_wh = 0;
        else
        {
            m->ovl_xy = (a[2] & 0xFFFF) && (a[2] >> 16) ? a[1] : 0;
            m->ovl_wh = (a[2] & 0xFFFF) && (a[2] >> 16) ? a[2] : 0;
        }
        if (n >= 4)
            m->slides++;
        m->overlays++;
        if (m->overlay)
            m->overlay(m->ctx, m->sram + 0xE0000, m->ovl_xy, m->ovl_wh);
        reply(m, op, tag, HP_OK, 0, 0);
        return;
    case HP_OP_PING:
        reply(m, op, tag, HP_OK, a, n);
        return;
    case HP_OP_VCLK:
        /* as hostproto.c cmd_vclk: STAT, or 1 + a band */
        if (!m->vclk || n < 2 || n > 3 || a[0] != HP_VCLK_MAGIC ||
            (a[1] != HP_VCLK_STAT && a[1] - 1 >= 12))
        {
            if (m->vclk)
                m->vclk_bad++;
            reply(m, op, tag, m->vclk ? HP_E_INVAL : HP_E_UNKNOWN, 0, 0);
            return;
        }
        if (a[1] != HP_VCLK_STAT)
        {
            uint32_t b = a[1] - 1, f = n == 3 ? a[2] : 0;

            /* clock.c clock_band: a clock in the band, else its top */
            if (!b || f > vbm_band_mhz[b] || f <= vbm_band_mhz[b - 1])
                f = vbm_band_mhz[b];
            if (b != m->band || f != vbm_mhz(m))
            {
                m->band = b;
                m->mhz = f;
                m->vclk_seq++;
                m->vclk_sets++;
                if (m->vclk_fail_mask >> b & 1)
                {
                    m->band = 0;        /* clock.c: the speed check */
                    m->mhz = 0;
                    m->vclk_fails++;
                }
            }
        }
        r[0] = m->band;
        r[1] = vbm_mhz(m);
        r[2] = m->vclk_busy;
        r[3] = m->now_us();
        r[4] = r[5] = 0;
        r[6] = m->vclk_fails;           /* HP_VCLK_R_FAILS */
        reply(m, op, tag, HP_OK, r, 7);
        return;
    default:
        reply(m, op, tag, HP_E_UNKNOWN, 0, 0);
        return;
    }
}

static void poll_ring(struct vbm *m)
{
    uint32_t w = MB(HP_CMD_WR), args[HP_MAX_PAYLOAD];

    if (!HP_VALID(w))
        return;
    w &= 0xFFFF;
    while (m->cmd_rd != w)
    {
        uint32_t hdr = CMD(m->cmd_rd), n = HP_REC_LEN(hdr), i;

        if (n + 1 > ((w - m->cmd_rd) & 0xFFFF))
            break;
        if (n > HP_MAX_PAYLOAD)
            reply(m, HP_REC_OP(hdr), HP_REC_TAG(hdr), HP_E_TOOBIG, 0, 0);
        else
        {
            for (i = 0; i < n; i++)
                args[i] = CMD(m->cmd_rd + 1 + i);
            cmd(m, HP_REC_OP(hdr), HP_REC_TAG(hdr), args, n);
        }
        m->cmd_rd = (m->cmd_rd + 1 + n) & 0xFFFF;
        SET(HP_CMD_RD, HP_ENC(m->cmd_rd));
    }
}

static void step(struct vbm *m)
{
    if (!m->running || m->hang)
        return;
    vbm_run(m);
    /* Still more permissive than the image: commands are answered while
       a packet decodes, where bcmfw/main.c reads its ring only between
       packets (a reply waits up to a decode).  Making the model wait
       moves the player's timing rows (test_vpcore's sync bounds were
       set against this model); left for after 0.8.0 */
    poll_ring(m);
    vbm_run(m);
    if (m->torn)
        SET(HP_EVT_WR, 0x23232323);     /* session 12's junk */
}

/* ---- the host port ---- */

static uint8_t *at(struct vbm *m, uint32_t addr, uint32_t len)
{
    if (addr < VBM_SRAM_SIZE && addr + len <= VBM_SRAM_SIZE)
        return m->sram + addr;
    if (addr >= VBM_SDRAM_HOST && addr - VBM_SDRAM_HOST + len <= VBM_SDRAM_SIZE)
        return m->sdram + (addr - VBM_SDRAM_HOST);
    return 0;
}

int vbm_rd(struct vbm *m, uint32_t addr, uint32_t *val)
{
    uint8_t *p;

    step(m);
    if (m->stall_left && !m->stall_writes)
    {
        m->stall_left--;
        return -1;
    }
    if (!m->powered || (m->hang && m->hang_port) || !(p = at(m, addr, 4)))
        return -1;
    if (addr >= VBM_SDRAM_HOST)
        m->pend_n = 0;          /* behind the host's writes (VBM_LAND_US) */
    *val = get32(p);
    return 0;
}

int vbm_wr(struct vbm *m, uint32_t addr, uint32_t val)
{
    uint8_t *p;

    if (m->stall_left)
    {
        m->stall_left--;
        return -1;
    }
    if (!m->powered || (m->hang && m->hang_port) || !(p = at(m, addr, 4)))
        return -1;
    put32(p, val);
    if (addr >= VBM_SDRAM_HOST)
        sdram_written(m, addr - VBM_SDRAM_HOST, 4);
    step(m);
    return 0;
}

int vbm_wr_block(struct vbm *m, uint32_t addr, const uint16_t *src,
                 uint32_t n)
{
    uint8_t *p;

    /* lcd_write_data(): 32-bit stores from a word-aligned buffer to a
       word-aligned port address */
    if ((n & 1) || (addr & 3) || ((uintptr_t)src & 3))
    {
        m->port_misuse++;
        return -1;
    }
    if (m->stall_left)
    {
        m->stall_left--;
        return -1;
    }
    if (!m->powered || (m->hang && m->hang_port) ||
        !(p = at(m, addr, 2 * n)))
        return -1;
    memcpy(p, src, 2 * n);
    if (addr >= VBM_SDRAM_HOST)
        sdram_written(m, addr - VBM_SDRAM_HOST, 2 * n);
    return 0;
}
