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
 * bcmfw video host protocol, skeleton: the header and both rings, the
 * version handshake, open/close with the null codec, feed (checked and
 * consumed, not decoded) and status.  Everything else is answered with
 * HP_E_UNKNOWN, so the host never waits on a command this image lacks.
 * Design: docs/research/bcm2722/hostproto.md.
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
#include <stddef.h>
#include <stdint.h>
#include "bcmfw.h"
#include "hw.h"
#include "hostproto.h"
#if BCMFW_CLOCK
#include "clock.h"
#endif
#if BCMFW_VIDEO
#include "video/video.h"
#endif
#if BCMFW_AAC
#include "aac/aac.h"
#include "aac/aac_image.h"
#endif

#if BCMFW_HOSTPROTO

#if BCMFW_JOBS
#include "jobs.h"
#if BCMFW_HIB_PARK
#include "hibwake.h"
#endif
#define CAP_JOBS    HP_CAP_JOBS_2D
#else
#define CAP_JOBS    0
#endif
#if BCMFW_VIDEO
#define FIFO_HOST   (VIDEO_SDRAM_HOST + VIDEO_FIFO_OFF)
#define FIFO_SIZE   VIDEO_FIFO_SIZE
#if BCMFW_AAC
#define CAP_AAC     HP_CAP_AAC
#else
#define CAP_AAC     0
#endif
#if BCMFW_VCLK
#define CAP_VCLK    HP_CAP_VCLK
#else
#define CAP_VCLK    0
#endif
#define CAPS        (HP_CAP_NULL | HP_CAP_H264_BP | HP_CAP_SCALE | HP_CAP_LCD_DMA | \
                     HP_CAP_OVERLAY | HP_CAP_OVERLAY_SLIDE | HP_CAP_AUDIO | \
                     CAP_AAC | CAP_JOBS | HP_CAP_PARK | CAP_VCLK)
#else
#define FIFO_HOST   HP_SRAM_FIFO
#define FIFO_SIZE   HP_SRAM_FIFO_SIZE
#define CAPS        (HP_CAP_NULL | CAP_JOBS | HP_CAP_PARK)
#endif

/* The brief probes (HP_OP_PEEK ... HP_OP_DMACPY): the plain video image */
#define HP_PROBES   (BCMFW_VIDEO && !BCMFW_JOBS)

#define CMD ((volatile uint32_t *)HP_CMD_RING)
#define EVT ((volatile uint32_t *)HP_EVT_RING)

static uint32_t cmd_rd;         /* our copies of the indices we own */
static uint32_t evt_wr;
static uint32_t state;
static uint32_t session;        /* 0: none, else the open session's id */
static uint32_t codec;          /* the open session's HP_CODEC_* */
static uint32_t own_display;
static uint32_t fifo_next;      /* bytes: where the next packet must start */
static uint32_t fifo_rd;        /* 64-byte units consumed */
static uint32_t stat_packets, stat_bytes, stat_errors, stat_events_lost;
static uint32_t beat;

int hp_owns_display(void)
{
    return own_display;
}

static void publish_state(void)
{
    MB(HP_STATE) = HP_ENC((beat & 0xFF) << 8 | state);
}

/* The heartbeat steps every 1.024 ms of the 1 MHz counter, or every
 * poll when the counter doesn't run, so the host can tell a live image
 * from a hung one without a command round trip. */
static void heartbeat(void)
{
    static uint32_t last;

    if (MB(BCMFW_MB_INFO) & BCMFW_INFO_TIMER_OK)
    {
        uint32_t t = REG(TIMER_CLO) >> 10;

        if (t == last)
            return;
        last = t;
    }
    beat++;
    publish_state();
}

void hp_init(void)
{
    MB(HP_VERSION) = 0;         /* not ready while we set up */
    MB(HP_CAPS) = CAPS;
    MB(HP_CMD_BASE) = HP_CMD_RING;
    MB(HP_CMD_SIZE) = HP_CMD_WORDS;
    MB(HP_EVT_BASE) = HP_EVT_RING;
    MB(HP_EVT_SIZE) = HP_EVT_WORDS;
    MB(HP_FIFO_BASE) = FIFO_HOST;
    MB(HP_FIFO_SIZE) = FIFO_SIZE;
    cmd_rd = evt_wr = 0;
    MB(HP_CMD_WR) = HP_ENC(0);
    MB(HP_CMD_RD) = HP_ENC(0);
    MB(HP_EVT_WR) = HP_ENC(0);
    MB(HP_EVT_RD) = HP_ENC(0);
    MB(HP_FIFO_RD) = HP_ENC(0);
    state = HP_ST_IDLE;
    publish_state();
#if BCMFW_JOBS
    jobs_init();
#endif
    MB(HP_VERSION) = HP_VERSION_WORD;
}

/* ---- event ring ---- */

static uint32_t evt_free(void)
{
    uint32_t w = MB(HP_EVT_RD);

    if (!HP_VALID(w))
        return 0;               /* half-written by the host: try later */
    return HP_EVT_WORDS - ((evt_wr - w) & 0xFFFF);
}

/* Post one record: header, status, then n payload words.  Returns 0 if
 * there is no room (the host has stopped reading): the record is lost
 * and counted, the BCM never waits. */
static int post(uint32_t op, uint32_t tag, uint32_t status,
                const uint32_t *p, uint32_t n)
{
    uint32_t i, k = evt_wr;

    if (evt_free() < n + 2)
    {
        stat_events_lost++;
        return 0;
    }
    EVT[k++ % HP_EVT_WORDS] = HP_REC(op, n + 1, tag);
    EVT[k++ % HP_EVT_WORDS] = status;
    for (i = 0; i < n; i++)
        EVT[k++ % HP_EVT_WORDS] = p[i];
    evt_wr = k & 0xFFFF;
    MB(HP_EVT_WR) = HP_ENC(evt_wr);
    return 1;
}

static void reply(uint32_t op, uint32_t tag, uint32_t status,
                  const uint32_t *p, uint32_t n)
{
    post(op | HP_REPLY, tag, status, p, n);
}

/* An unsolicited event (tag 0) */
void hp_event(uint32_t op, uint32_t status, const uint32_t *p, uint32_t n)
{
    post(op, 0, status, p, n);
}

/* ---- commands ---- */

static void cmd_hello(uint32_t tag, const uint32_t *a, uint32_t n)
{
    uint32_t r[3];

    r[0] = HP_VERSION_WORD;
    r[1] = MB(HP_CAPS);
    r[2] = HP_MAX_PAYLOAD;
    /* a different major on the host side: we still answer with ours, the
     * host decides; the status says it can't work as is */
    if (n >= 1 && (a[0] >> 8 & 0xFF) != HP_MAJOR)
        reply(HP_OP_HELLO, tag, HP_E_VERSION, r, 3);
    else
        reply(HP_OP_HELLO, tag, HP_OK, r, 3);
}

static void cmd_open(uint32_t tag, const uint32_t *a, uint32_t n)
{
    uint32_t w, h, r[6];

    if (session)
    {
        reply(HP_OP_OPEN, tag, HP_E_BUSY, 0, 0);
        return;
    }
    w = a[1] & 0xFFFF;
    h = a[1] >> 16;
    if (!w || !h || w > HP_MAX_W || h > HP_MAX_H || (w | h) & 1
        || (a[2] & 0xFF) > HP_MODE_NATIVE)
    {
        reply(HP_OP_OPEN, tag, HP_E_INVAL, 0, 0);
        return;
    }
#if BCMFW_VIDEO
    if (a[0] == HP_CODEC_H264)
    {
        uint32_t e;

#if BCMFW_JOBS
        jobs_mode(J_MODE_VIDEO);    /* the decoder takes the arena */
#endif
        e = video_open(w, h, a[2] & 0xFF, a[2]);

        if (e != HP_OK)
        {
            /* NOMEM says which step (minor 2): the SDRAM set-up words */
            reply(HP_OP_OPEN, tag, e, video_sdram_diag(),
                  e == HP_E_NOMEM ? VIDEO_SD_WORDS : 0);
            return;
        }
    }
    else
#endif
    if (a[0] != HP_CODEC_NULL)
    {
        reply(HP_OP_OPEN, tag, HP_E_UNSUPPORTED, 0, 0);
        return;
    }
    codec = a[0];
    session = 1;
    own_display = !!(a[2] & HP_OPEN_OWN_DISPLAY);
    fifo_next = 0;
    fifo_rd = 0;
    MB(HP_FIFO_RD) = HP_ENC(0);
    stat_packets = stat_bytes = stat_errors = 0;
    state = HP_ST_OPEN;
    r[0] = session;
    r[1] = FIFO_HOST;
    r[2] = FIFO_SIZE;
#if BCMFW_VIDEO
    r[3] = VIDEO_QUEUE_DEPTH;           /* minor 1 */
    r[4] = video_sdram_diag()[0];       /* minor 2: how SDRAM came up */
    r[5] = video_sdram_diag()[4];       /* minor 3: the memory test */
    reply(HP_OP_OPEN, tag, HP_OK, r, codec == HP_CODEC_H264 ? 6 : 4);
#else
    reply(HP_OP_OPEN, tag, HP_OK, r, 3);
#endif
}

static int check_session(uint32_t op, uint32_t tag, const uint32_t *a, uint32_t n)
{
    if (n < 1 || !session || a[0] != session)
    {
        stat_errors++;
        reply(op, tag, HP_E_NOSESSION, 0, 0);
        return 0;
    }
    return 1;
}

#if BCMFW_AAC
static uint32_t aac_live;       /* an AAC OPEN succeeded in this session */
#endif

static void end_session(void)
{
#if BCMFW_AAC
    aac_live = 0;
#endif
#if BCMFW_VIDEO
    if (codec == HP_CODEC_H264)
        video_close();
#endif
    session = 0;
    own_display = 0;
    state = HP_ST_IDLE;
#if BCMFW_JOBS
    jobs_mode(J_MODE_UI);           /* the pools are the UI's again, empty */
#endif
}

static void cmd_close(uint32_t tag, const uint32_t *a, uint32_t n)
{
    uint32_t r[5];

    if (!check_session(HP_OP_CLOSE, tag, a, n))
        return;
    r[0] = stat_packets;
    r[1] = stat_bytes;
#if BCMFW_VIDEO
    if (codec == HP_CODEC_H264)
    {
        const struct video_stats *v = video_get_stats();

        r[2] = v->decoded;              /* minor 1 */
        r[3] = v->shown;
        r[4] = v->errors;
        end_session();
        reply(HP_OP_CLOSE, tag, HP_OK, r, 5);
        return;
    }
#endif
    end_session();
    reply(HP_OP_CLOSE, tag, HP_OK, r, 2);
}

/* ---- The warm swap (HP_OP_PARK; jobs.md G9, bench row B13) ----
 * PARK replies, and the main loop's next hp_poll() parks: the reply is in
 * the event ring by then.  The core leaves the image for a stub outside
 * every image (park.S) and waits there for the next one; the panel stays
 * as it is, lit and showing its last frame. */
#if BCMFW_MB_WARM != HP_WARM_WORD || BCMFW_WARM_PARKED != HP_WARM_PARKED || \
    BCMFW_WARM_GO != HP_WARM_GO || BCMFW_WARM_UP != HP_WARM_UP || \
    BCMFW_WARM_PANEL_ON != HP_WARM_PANEL_ON || BCMFW_WARM_TIMING1 != HP_WARM_TIMING1 || \
    BCMFW_MB_WARM_PROG != HP_WARM_PROG || BCMFW_PARK_STUB != HP_WARM_STUB_ADDR
#error "the warm words differ between config.h and hostproto.h"
#endif
extern const char bcmfw_park_stub[], bcmfw_park_stub_end[];
static uint32_t park_flags;     /* BCMFW_WARM_PARKED | flags: park next */
#define HIB_PENDING     1       /* park_flags: hibernate next (BCMFW_HIB_PARK) */

/* rare: out of line and compact (the merged image has little room
 * below its arena) */
#define PARK_FN         __attribute__((noinline, optimize("Os")))
static PARK_FN void cmd_park(uint32_t tag, const uint32_t *a, uint32_t n)
{
    uint32_t f = 0;

    if (n < 1 || a[0] != HP_PARK_MAGIC)
    {
        reply(HP_OP_PARK, tag, HP_E_INVAL, 0, 0);
        return;
    }
    if (session)
    {
        reply(HP_OP_PARK, tag, HP_E_BUSY, 0, 0);    /* CLOSE first */
        return;
    }
#if BCMFW_CLOCK
    /* the next image starts on the boot clock's bus timing */
    if (clock_is_fast())
    {
        reply(HP_OP_PARK, tag, HP_E_STATE, 0, 0);
        return;
    }
#endif
#if BCMFW_LCD_STANDBY
    if (lcd_standby_state())
    {
        reply(HP_OP_PARK, tag, HP_E_STATE, 0, 0);   /* wake it first */
        return;
    }
#endif
    if (lcd_is_on())
        f = BCMFW_WARM_PANEL_ON | (lcd_timing_word() ? BCMFW_WARM_TIMING1 : 0);
    park_flags = BCMFW_WARM_PARKED | f;
#if BCMFW_HIB_PARK
    /* the hibernate form (hibwake.h): only with the PP's stub in place,
     * and never with SDRAM up (the stub has no SDRAM step) */
    if (n > 1 && a[1] == HP_PARK_HIB)
    {
        park_flags = HIB_PENDING;
        if (MB(HW_STUB + HW_S_MAGIC) != HW_STUB_MAGIC
#if BCMFW_VIDEO
            || video_sdram_ready()
#endif
            )
        {
            park_flags = 0;
            reply(HP_OP_PARK, tag, HP_E_STATE, 0, 0);
            return;
        }
    }
#endif
    reply(HP_OP_PARK, tag, HP_OK, &f, 1);
}

#if BCMFW_HIB_PARK
static PARK_FN void park(void);
#else
static PARK_FN void park(void) __attribute__((noreturn));
#endif
#define PARK_PROG(st, d) (MB(BCMFW_MB_WARM_PROG) = HP_WARM_PROG_W(st, d))

/* Apple's whole-cache flush (vmcs.bin @25a), run after code is written
 * and before it runs (@88a0, @91a4): the request (bit 30) over the word
 * with both enables cleared; if bit 0 already reads set (silicon leaves it
 * set: PW1 read 0x00010001), the word back without the request and the
 * request again; then bit 0 polled, up to 4,096 reads; then the word with
 * the enables cleared.  Apple's re-request has no bound; ours stops after
 * 15 and goes on (the caller reports how it went).  The caller has
 * interrupts off.  Returns retries << 12 | polls. */
__attribute__((optimize("Os"))) uint32_t hp_code_flush(void)
{
    uint32_t keep = REG(BCMFW_CACHE_CTL) & 0x3FFFFFFF, t, k;

    for (t = 0; t != 15; t++)
    {
        REG(BCMFW_CACHE_CTL) = keep | 0x40000000;
        if (!(REG(BCMFW_CACHE_CTL) & 1))
            break;
        REG(BCMFW_CACHE_CTL) = keep;
    }
    for (k = 0; k != 4095; k++)
        if (REG(BCMFW_CACHE_CTL) & 1)
            break;
    REG(BCMFW_CACHE_CTL) = keep;
    return t << 12 | k;
}

static void park(void)
{
    const uint32_t *s = (const uint32_t *)bcmfw_park_stub;
    volatile uint32_t *d = (volatile uint32_t *)BCMFW_PARK_STUB;
    uint32_t i, n = (bcmfw_park_stub_end - bcmfw_park_stub + 3) / 4;
    void (*stub)(uint32_t) = (void (*)(uint32_t))BCMFW_PARK_STUB;

#if BCMFW_HIB_PARK
    /* hibernate: into the PP's stub, which parks, waits for the wake and
     * comes back only if every region's sum held (else it spins and the
     * PP cuts) */
    if (park_flags == HIB_PENDING)
    {
        park_flags = 0;
        lcd_blit_wait();
        /* code the PP has written: Apple's flush before the first call
         * into it (quirks.md #40; interrupts are off outside idle_wait) */
        hp_code_flush();
        ((void (*)(void))MB(HW_STUB + HW_S_ENTRY))();
        return;
    }
#endif

    PARK_PROG(HP_WARM_PH_ENTER, 0);
#if BCMFW_LCD_DMA
    lcd_blit_wait();            /* no transfer runs into the next image */
#endif
    PARK_PROG(HP_WARM_PH_BLIT, 0);
    asm volatile("! di" : : : "memory");
    for (i = 0; i < 4; i++)
        REG(IC_PRIO(i)) = 0;    /* every source off; the next image's
                                   irq_init sets its own */
    PARK_PROG(HP_WARM_PH_IRQ, 0);
    for (i = 0; i < n; i++)
        d[i] = s[i];
    PARK_PROG(HP_WARM_PH_COPIED, n);
#if BCMFW_PARK_FLUSH
    /* code just written by data stores: Apple's flush before it runs */
    i = hp_code_flush();
    PARK_PROG(HP_WARM_PH_FLUSHED, i);
#else
    i = 0;                      /* tests only: the park silicon refused */
#endif
    PARK_PROG(HP_WARM_PH_CALL, i);
    stub(park_flags);
    for (;;)
        ;
}

#if BCMFW_VIDEO
/* ---- The memory lab (HP_OP_LAB; bench rows B6 and B7, open questions
 * 7, 44 and 54).  Each kind runs count times over 2^log2 bytes at base
 * and reports its time from the 1 MHz counter.  RAND16 less EMPTY is the
 * cost of one texel fetch at a computed address; the others give the
 * core's bandwidth in order.  The host may stream through the port
 * during a run, to see whether its writes slow the core (#44). */
static uint32_t lab_run(uint32_t kind, uint32_t base, uint32_t log2,
                        uint32_t count, uint32_t *sum)
{
    volatile uint16_t *h = (volatile uint16_t *)base;
    volatile uint32_t *w = (volatile uint32_t *)base;
    uint32_t hmask = (1u << (log2 - 1)) - 1, wmask = (1u << (log2 - 2)) - 1;
    uint32_t half = 1u << (log2 - 3), x = 0x2722C0DEu, s = 0, i, t0;

    t0 = REG(TIMER_CLO);
    switch (kind)
    {
    case HP_LAB_EMPTY:
        for (i = 0; i < count; i++)
        {
            x = x * 1664525u + 1013904223u;
            s += (x >> 8) & hmask;
        }
        break;
    case HP_LAB_RAND16:
        for (i = 0; i < count; i++)
        {
            x = x * 1664525u + 1013904223u;
            s += h[(x >> 8) & hmask];
        }
        break;
    case HP_LAB_SEQ32:
        for (i = 0; i < count; i++)
            s += w[i & wmask];
        break;
    case HP_LAB_STORE32:
        for (i = 0; i < count; i++)
            w[i & wmask] = i;
        break;
    default:                    /* HP_LAB_COPY32 */
        for (i = 0; i < count; i++)
            w[half + (i & (half - 1))] = w[i & (half - 1)];
        break;
    }
    *sum = s;
    return REG(TIMER_CLO) - t0;
}

static void cmd_lab(uint32_t tag, const uint32_t *a, uint32_t n)
{
    uint32_t r[3];
    int sram, sdram;

    if (n < 5 || a[0] != HP_LAB_MAGIC || a[1] >= HP_LAB_KINDS ||
        a[3] < 6 || a[4] == 0 || a[4] > HP_LAB_COUNT_MAX)
    {
        reply(HP_OP_LAB, tag, HP_E_INVAL, 0, 0);
        return;
    }
    sram = a[2] == HP_LAB_SRAM && a[3] <= HP_LAB_SRAM_LOG2;
    sdram = a[2] == HP_LAB_SDRAM && a[3] <= HP_LAB_SDRAM_LOG2;
    if (!sram && !sdram)
    {
        reply(HP_OP_LAB, tag, HP_E_INVAL, 0, 0);
        return;
    }
    /* SDRAM must be up (the first H.264 OPEN; it stays up after CLOSE),
       and a session may be open (the 150 MHz runs need one) only before
       its first packet: the DPB is the lab's */
    if ((sdram && !video_sdram_ready()) || (session && fifo_next != 0))
    {
        reply(HP_OP_LAB, tag, HP_E_STATE, 0, 0);
        return;
    }
    r[0] = lab_run(a[1], a[2], a[3], a[4], &r[1]);
#if BCMFW_CLOCK
    r[2] = clock_is_fast();
#else
    r[2] = 0;
#endif
    reply(HP_OP_LAB, tag, HP_OK, r, 3);
}
#endif

/* The null codec: check the packet's place in the FIFO, then consume it */
static void cmd_feed(uint32_t tag, const uint32_t *a, uint32_t n)
{
    uint32_t off, len, flags;

    if (!check_session(HP_OP_FEED, tag, a, n))
        return;
    off = a[1];
    len = a[2];
    flags = a[5];
    if (off != fifo_next || !len || len > FIFO_SIZE)
    {
        stat_errors++;
        reply(HP_OP_FEED, tag, HP_E_FIFO, &fifo_next, 1);
        return;
    }
#if BCMFW_VIDEO
    if (codec == HP_CODEC_H264)
    {
        /* decoded by hp_work(), which then advances FIFO_RD */
        uint32_t e = video_queue(off, len, a[3], a[4], flags);

        if (e != HP_OK)
        {
            stat_errors++;
            reply(HP_OP_FEED, tag, e, &fifo_next, 1);
            return;
        }
        fifo_next = ((off + len + 63) & ~63u) % FIFO_SIZE;
        stat_packets++;
        stat_bytes += len;
        if (state == HP_ST_OPEN)
            state = HP_ST_PLAYING;
        if (flags & HP_FEED_ACK)
            reply(HP_OP_FEED, tag, HP_OK, 0, 0);
        return;
    }
#endif
    /* packets are 64-byte aligned in the FIFO and may wrap its end */
    fifo_next = ((off + len + 63) & ~63u) % FIFO_SIZE;
    fifo_rd = (fifo_rd + ((len + 63) >> 6)) & 0xFFFF;
    MB(HP_FIFO_RD) = HP_ENC(fifo_rd);
    stat_packets++;
    stat_bytes += len;
    state = HP_ST_PLAYING;
    if (flags & HP_FEED_ACK)
        reply(HP_OP_FEED, tag, HP_OK, 0, 0);
}

static void cmd_status(uint32_t tag, const uint32_t *a, uint32_t n)
{
    uint32_t r[16];

    r[0] = state;
    r[1] = session;
    r[2] = stat_packets;
    r[3] = stat_bytes;
    r[4] = stat_errors;
    r[5] = stat_events_lost;
#if BCMFW_VIDEO
    if (session && codec == HP_CODEC_H264)
    {
        const struct video_stats *v = video_get_stats();

        r[6] = v->decoded;              /* minor 1 */
        r[7] = v->shown;
        r[8] = v->errors;
        r[9] = v->concealed;
        r[10] = v->queued;
        r[11] = v->pts_lo;
        r[12] = v->pts_hi;
        r[13] = v->work_us;             /* minor 1, words 13-15 (p14-player) */
        r[14] = v->work_max_us;
        r[15] = v->mhz;
        reply(HP_OP_STATUS, tag, HP_OK, r, 16);
        return;
    }
#endif
    reply(HP_OP_STATUS, tag, HP_OK, r, 6);
}

#if BCMFW_VIDEO && BCMFW_VCLK
/* P19 PW4: one band and a clock in it (clock.c clock_gov_set: Apple's
 * order, the speed check, the crystal on any failure), or none
 * (HP_VCLK_STAT); the reply is the switch's record and the busy meter */
static void cmd_vclk(uint32_t tag, const uint32_t *a, uint32_t n)
{
    if (n < 2 || n > 3 || a[0] != HP_VCLK_MAGIC ||
        (a[1] != HP_VCLK_STAT && a[1] - 1 >= CLOCK_BANDS))
    {
        reply(HP_OP_VCLK, tag, HP_E_INVAL, 0, 0);
        return;
    }
    if (a[1] != HP_VCLK_STAT)
        clock_gov_set(a[1] - 1, n == 3 ? a[2] : 0);
    clock_gov.now = REG(TIMER_CLO);
    reply(HP_OP_VCLK, tag, HP_OK, (const uint32_t *)&clock_gov,
          CLOCK_GOV_WORDS);
}
#endif

/* Payload words each command needs at least (shorter: HP_E_INVAL) */
static const uint8_t min_args[HP_OP_PING + 1] =
{
    [HP_OP_OPEN] = 3,
    [HP_OP_FEED] = 6,
};

/* PINS: GPIO levels sampled in a tight loop, edges counted per pin
 * (stage 8a).  Blocks the loop for up to 1 s: the host waits on it. */
#define GPIO_FSEL0  0x10002800
#define GPIO_LEV0   0x10002820
#define GPIO_PUD    0x10002860  /* pull mode: 0 off, 1 down, 2 up [apple] */
#define GPIO_PUDCLK 0x10002864  /* the pins the mode is clocked into */

/* A pull on the masked pins, Apple's way (@13F7A, as BCM2835's GPPUD):
 * the mode, a wait, the pins clocked, a wait, both back to 0 */
static void pins_pull(uint32_t mask, uint32_t mode)
{
    volatile uint32_t i;

    *(volatile uint32_t *)GPIO_PUD = mode;
    for (i = 150; i != 0; i--)
        ;
    *(volatile uint32_t *)GPIO_PUDCLK = mask;
    for (i = 150; i != 0; i--)
        ;
    *(volatile uint32_t *)GPIO_PUD = 0;
    *(volatile uint32_t *)GPIO_PUDCLK = 0;
}

static void cmd_pins(uint32_t tag, const uint32_t *a, uint32_t n)
{
    uint32_t mask, us, flags, pin[4], fsel = 0, t0, now, lim;
    int32_t np = 0, k;
    uint32_t r[8] = { 0 }, prev, v, all_and = ~0u, all_or = 0;

    /* exactly three words: "n < 3" compiles to a bls, a branch form the
       self-test hasn't proven against Apple's code */
    if (n != 3 || !a[0] || (int32_t)a[1] < 0 || (int32_t)a[1] > 1000000)
    {
        reply(HP_OP_PINS, tag, HP_E_INVAL, 0, 0);
        return;
    }
    mask = a[0];
    us = a[1];
    flags = a[2];
    for (k = 0; k < 32 && np < 4; k++)
        if (mask & (1u << k))
            pin[np++] = k;
    if (flags & 1)
    {
        /* inputs for the probe (pins 0-15 only: the first fsel word) */
        uint32_t m = 0;

        fsel = *(volatile uint32_t *)GPIO_FSEL0;
        for (k = 0; k < 16; k++)
            if (mask & (1u << k))
                m |= 3u << (2 * k);
        *(volatile uint32_t *)GPIO_FSEL0 = fsel & ~m;
    }
    /* flags bits 3:2: a pull for the probe (stage 8a-0b: does a pin
       float?), taken off again after; a weak pull drives nothing */
    if ((flags >> 2 & 3) != 0)
    {
        pins_pull(mask, flags >> 2 & 3);
        for (k = 0; k < 2000; k++)      /* the line settles */
            (void)*(volatile uint32_t *)GPIO_LEV0;
    }
    /* the bound in samples too, if the counter doesn't run */
    lim = MB(BCMFW_MB_INFO) & BCMFW_INFO_TIMER_OK ? 0x7FFFFFFFu : 8 * us;
    t0 = REG(TIMER_CLO);
    prev = *(volatile uint32_t *)GPIO_LEV0;
    for (;;)
    {
        uint32_t d;

        v = *(volatile uint32_t *)GPIO_LEV0;
        d = (v ^ prev) & mask;
        if (d)
            for (k = 0; k < np; k++)
                r[2 + k] += d >> pin[k] & 1;
        all_and &= v;
        all_or |= v;
        prev = v;
        r[0]++;
        now = REG(TIMER_CLO);
        if ((int32_t)(now - t0 - us) >= 0 || r[0] == lim)
            break;
    }
    r[1] = now - t0;
    r[6] = all_and & mask;
    r[7] = all_or & mask;
    if ((flags >> 2 & 3) != 0)
        pins_pull(mask, 0);
    if (flags & 1)
        *(volatile uint32_t *)GPIO_FSEL0 = fsel;
    reply(HP_OP_PINS, tag, HP_OK, r, 8);
}

#if HP_PROBES
/* ---- The brief probes (brief-map-peripherals.md section 11: PB-1 to
 * PB-4; brief-map-power.md section 9: PW2, PW8).  The plain video image
 * only: the merged image (BCMFW_JOBS) has no room for them.  Nothing
 * here drives a pin or an output: PEEK only reads, the census only
 * samples, PULL changes weak pulls on allowlisted inputs and leaves each
 * pulled down (Apple's boot state), CACHE writes Apple's cache values
 * and puts ours back, DMACPY copies between free regions. */
#define GPIO_LEV1    0x10002824
#define GPIO_PUDCLK1 0x10002868     /* the clock word for pins 32-51 */

/* Busy-waits us on the 1 MHz counter; bounded in loops too (8 a us) if
 * the counter doesn't run */
static void pb_wait_us(uint32_t us)
{
    uint32_t t0 = REG(TIMER_CLO), k,
             lim = MB(BCMFW_MB_INFO) & BCMFW_INFO_TIMER_OK ? 0x7FFFFFFFu
                                                           : 8 * us;

    for (k = 0; k != lim; k++)
        if ((int32_t)(REG(TIMER_CLO) - t0 - us) >= 0)
            break;
}

/* us on the counter (PW3, PW4, PW5): 0, or 1 if the counter read the same
 * HP_PW_STALL_READS times in a row (stopped: on the chip it moves every
 * us, far slower than this loop) */
static uint32_t pw_wait(uint32_t us)
{
    uint32_t t0 = REG(TIMER_CLO), last = t0, now, same = 0;

    for (;;)
    {
        now = REG(TIMER_CLO);
        if (now - t0 >= us)
            return 0;
        if (now != last)
        {
            last = now;
            same = 0;
        }
        else if (++same == HP_PW_STALL_READS)
            return 1;
    }
}

/* PEEK's allowlist, inclusive word ranges: the registers PB-1, PB-3 and
 * PW1 name, nothing else.  Never on it: the FIFOs (0x10002C04,
 * 0x1000400C, 0x10004804), the doorbell acknowledge (0x10003800), the
 * DMA channels, the SDRAM controller, the pull clocks.  Reading a
 * register here has no side effect Apple's code relies on: Apple reads
 * each of PB-1a's; the rest Apple only writes [apple], read back here. */
static const uint32_t peek_allow[][2] =
{
    { 0x10000400, 0x10000400 },     /* PW1 step 3 */
    { 0x10000408, 0x10000408 },
    { 0x10000C00, 0x10000C00 },     /* cache control (PW1) */
    { 0x10001400, 0x10001400 },     /* regulator (PW1) */
    { 0x10001804, 0x10001804 },     /* the 1 MHz counter (the canary) */
    { 0x10001C00, 0x10001C58 },     /* PLLs (PW1), block clocks (PB-1b) */
    { 0x10002800, 0x1000280C },     /* GPIO function select */
    { 0x10002820, 0x10002824 },     /* GPIO levels (panel ID: 2824 19:18) */
    { 0x10002860, 0x10002860 },     /* GPIO pull mode */
    { 0x10002C00, 0x10002C00 },     /* TV control */
    { 0x10002C08, 0x10002C18 },     /* TV timing, status (not 2C04) */
    { 0x10003410, 0x10003414 },     /* PW1 step 4 */
    { 0x10003420, 0x10003428 },
    { 0x10003430, 0x10003430 },
    { 0x10004800, 0x10004800 },     /* audio control (not 4804) */
    { 0x10004810, 0x10004814 },     /* audio +0x10, rate */
    { 0x10005404, 0x10005404 },     /* video DAC */
    { 0x10005484, 0x100054DC },     /* the Macrovision set */
    { 0x10006400, 0x10006400 },     /* USB status (PB-1a, PB-3) */
};
#define PEEK_RANGES  (sizeof(peek_allow) / sizeof(peek_allow[0]))

static int peek_ok(uint32_t a)
{
    uint32_t i;

    if (a & 3)
        return 0;
    for (i = 0; i != PEEK_RANGES; i++)
        if (a - peek_allow[i][0] <= peek_allow[i][1] - peek_allow[i][0])
            return 1;
    return 0;
}

static void cmd_peek(uint32_t tag, const uint32_t *a, uint32_t n)
{
    uint32_t r[2 * HP_PEEK_MAX], i, k;

    if (n < 3 || n > 2 + HP_PEEK_MAX || a[0] != HP_PEEK_MAGIC ||
        a[1] > HP_PEEK_GAP_MAX)
    {
        reply(HP_OP_PEEK, tag, HP_E_INVAL, 0, 0);
        return;
    }
    /* the whole list first: a refused address means nothing is read */
    for (i = 2; i != n; i++)
        if (!peek_ok(a[i]))
        {
            reply(HP_OP_PEEK, tag, HP_E_INVAL, &a[i], 1);
            return;
        }
    for (i = 2, k = 0; i != n; i++, k += 2)
    {
        r[k] = REG(a[i]);
        if (a[1])
            pb_wait_us(a[1]);
        r[k + 1] = REG(a[i]);
    }
    reply(HP_OP_PEEK, tag, HP_OK, r, k);
}

/* TV (PB-7 to PB-9; brief-map-peripherals.md section 2.3 for every value
 * written, section 11 for the steps).  Off by default: a step runs only
 * with the one-time code the last HP_TV_UNLOCK handed out.
 *
 * Every write goes through tv_set/tv_poke/tv_pin19, which check it
 * against the step's allowlist (TV_G_*): TIMING may write the pixel
 * clock and the timing generator only; OUTPUT adds the DAC with the
 * Macrovision words, GPIO 19 and DMA channel 6.  tv_set saves a
 * register before its first write; on every exit (done, a start wait
 * timed out, the chain stopped) the off order runs, then every saved
 * register is written back, in the reverse of the on order, and read
 * back.  Every wait is bounded on the 1 MHz counter, with a loop count
 * behind it; HP_TV_PROGRESS says where a run is. */
#define TV_CTL       0x10002C00     /* control */
#define TV_FIFO      0x10002C04     /* DMA channel 6's destination */
#define TV_MODE      0x10002C18     /* bit 31 the field, on read */
#define TV_DAC       0x10005404
#define TV_MV        0x10005484     /* 23 words to 0x100054DC */
#define TV_MV_WORDS  23
#define TV_PCLK      0x10001C30
#define TV_FSEL1     0x10002804     /* pins 16-31; pin 19 in bits 7:6 */
#define TV_GSET0     0x10002810
#define TV_GCLR0     0x10002818
#define TV_PIN19     0x00080000
#define TV_DMA6      (0x10000800 + 6 * 0x80)

#define TV_G_TIMING  1              /* the clock, 0x2C00-0x2C18 but 2C04 */
#define TV_G_DAC     2              /* 0x5404, the Macrovision set */
#define TV_G_GPIO    4              /* fsel word 1, set/clear bit 19 */
#define TV_G_DMA     8              /* channel 6 */

/* The registers a run may save and restore, in the on order (section
 * 2.3's table, Apple's @5A32/@5BCE run): restore walks it backwards */
enum { TV_S_MV0, TV_S_PCLK = TV_S_MV0 + TV_MV_WORDS, TV_S_FSEL, TV_S_DAC,
       TV_S_IN, TV_S_OSZ, TV_S_HT, TV_S_VT, TV_S_MODE, TV_S_CTL, TV_S_DMA,
       TV_SLOTS };

static const uint32_t tv_reg[TV_SLOTS - TV_S_PCLK] =
{
    TV_PCLK, TV_FSEL1, TV_DAC, 0x10002C08, 0x10002C0C, 0x10002C10,
    0x10002C14, TV_MODE, TV_CTL, TV_DMA6,
};

/* Section 2.3, steps 6-9, NTSC then PAL: input (lines a field, samples a
 * line), output (lines, samples), horizontal and vertical timing */
static const uint32_t tv_timing[2][4] =
{
    { 0x00F00280, 0x01E002C0, 0x09A2183F, 0x01709406 },
    { 0x01200280, 0x024002C0, 0x0A02383F, 0x0180B005 },
};

/* Colour bars, BT.601 Y Cb Cr (white, yellow, cyan, green, magenta, red,
 * blue, black), and the same as RGB565: the pattern, not registers */
static const uint8_t tv_bar_ycc[8][3] =
{
    { 235, 128, 128 }, { 210, 16, 146 }, { 170, 166, 16 }, { 145, 54, 34 },
    { 106, 202, 222 }, { 81, 90, 240 }, { 41, 240, 110 }, { 16, 128, 128 },
};
static const uint16_t tv_bar_565[8] =
{
    0xFFFF, 0xFFE0, 0x07FF, 0x07E0, 0xF81F, 0xF800, 0x001F, 0x0000,
};

static uint32_t tv_code;            /* 0: none handed out */

struct tv_run
{
    uint32_t allow, denied, std, phase;
    uint32_t saved[TV_SLOTS];
    uint8_t touched[TV_SLOTS];
    uint32_t pin19, lev19;          /* pin 19 driven; its level before */
};

static uint32_t tv_addr(uint32_t s)
{
    return s < TV_S_PCLK ? TV_MV + 4 * s : tv_reg[s - TV_S_PCLK];
}

static uint32_t tv_group(uint32_t s)
{
    return s < TV_S_PCLK || s == TV_S_DAC ? TV_G_DAC :
           s == TV_S_FSEL ? TV_G_GPIO : s == TV_S_DMA ? TV_G_DMA : TV_G_TIMING;
}

static void tv_progress(struct tv_run *t, uint32_t ph, uint32_t ms)
{
    if (ph <= HP_TV_P_HOLD)
        t->phase = ph;
    MB(HP_TV_PROGRESS) = HP_TV_PROG(ph, ms);
}

/* A state register: saved before its first write */
static void tv_set(struct tv_run *t, uint32_t s, uint32_t v)
{
    uint32_t a = tv_addr(s);

    if (!(t->allow & tv_group(s)))
    {
        t->denied++;
        return;
    }
    if (!t->touched[s])
    {
        t->saved[s] = REG(a);
        t->touched[s] = 1;
    }
    REG(a) = v;
}

/* A one-shot write (channel 6's status clear or chain start): nothing to
 * save; DMA group by its address */
static void tv_poke(struct tv_run *t, uint32_t a, uint32_t v)
{
    if (!(t->allow & TV_G_DMA) || a - TV_DMA6 >= 0x80)
    {
        t->denied++;
        return;
    }
    REG(a) = v;
}

/* GPIO 19 through the set and clear words; its level saved first */
static void tv_pin19(struct tv_run *t, int high)
{
    if (!(t->allow & TV_G_GPIO))
    {
        t->denied++;
        return;
    }
    if (!t->pin19)
    {
        t->lev19 = REG(GPIO_LEV0) & TV_PIN19;
        t->pin19 = 1;
    }
    REG(high ? TV_GSET0 : TV_GCLR0) = TV_PIN19;
}

/* Bit `bit` of `a` within us, on the counter (and a loop count): 0 if seen */
static int tv_wait(uint32_t a, uint32_t bit, uint32_t us)
{
    uint32_t t0 = REG(TIMER_CLO), k;

    for (k = 0; k != 64 * us; k++)
    {
        if (REG(a) & bit)
            return 0;
        if ((int32_t)(REG(TIMER_CLO) - t0 - us) >= 0)
            break;
    }
    return 1;
}

/* One line of the pattern in the request's format, at HP_TV_BUF; returns
 * its length in 4-byte units */
static uint32_t tv_fill(uint32_t fmt, uint32_t w, uint32_t pat)
{
    volatile uint32_t *p = (volatile uint32_t *)HP_TV_BUF;
    uint32_t x, b0, b1, y0, y1, u, v;

    for (x = 0; x < w; x += fmt == HP_TV_Y32 ? 1 : 2)
    {
        if (pat == HP_TV_PAT_LINES)
        {
            b0 = x % 32 == 0 ? 0 : 7;           /* white line, else black */
            b1 = 7;
        }
        else
        {
            b0 = x * 8 / w;
            b1 = (x + 1) * 8 / w;
        }
        if (fmt == HP_TV_Y32)
            b1 = b0;                            /* one pixel a word */
        y0 = tv_bar_ycc[b0][0];
        y1 = tv_bar_ycc[b1][0];
        u = tv_bar_ycc[b0][1];
        v = tv_bar_ycc[b0][2];
        /* the bytes in memory order: UYVY U Y0 V Y1, YUYV Y0 U Y1 V */
        if (fmt == HP_TV_UYVY)
            *p++ = u | y0 << 8 | v << 16 | y1 << 24;
        else if (fmt == HP_TV_YUYV)
            *p++ = y0 | u << 8 | y1 << 16 | v << 24;
        else if (fmt == HP_TV_RGB565)
            *p++ = tv_bar_565[b0] | (uint32_t)tv_bar_565[b1] << 16;
        else
            *p++ = y0 << 24;
    }
    return fmt == HP_TV_Y32 ? w : w / 2;
}

/* The off order (the reverse of the on order; section 2.3 "Off" and
 * PB-8 step 7 give each value), then every saved register written back,
 * backwards, then read back: returns the HP_TV_M_* that differ */
static uint32_t tv_restore(struct tv_run *t)
{
    uint32_t s, a, v, mask, bad = 0;

    tv_progress(t, HP_TV_P_OFF, 0);
    if (t->touched[TV_S_DMA])
    {
        tv_set(t, TV_S_DMA, 0);                     /* channel 6 stopped */
        tv_poke(t, TV_DMA6 + 0x04, 0xFFFFFFFF);  /* status cleared */
    }
    if (t->touched[TV_S_CTL])
        tv_set(t, TV_S_CTL, 0);                     /* the generator off */
    if (t->touched[TV_S_DAC])                       /* bit 5 (the DAC) off */
        tv_set(t, TV_S_DAC, t->std == HP_TV_PAL ? 0xC0 : 0x80);
    if (t->pin19)
        tv_pin19(t, 0);                             /* GPIO 19 low */
    if (t->touched[TV_S_PCLK])
        tv_set(t, TV_S_PCLK, 0xA5A50000);           /* the pixel clock off */
    tv_progress(t, HP_TV_P_RESTORE, 0);
    for (s = TV_SLOTS; s-- != 0; )
    {
        if (s == TV_S_FSEL && t->pin19)
        {
            /* the level before the function, so the pin never glitches */
            REG(t->lev19 ? TV_GSET0 : TV_GCLR0) = TV_PIN19;
            if ((REG(GPIO_LEV0) & TV_PIN19) != t->lev19 &&
                (!t->touched[TV_S_FSEL] || (t->saved[TV_S_FSEL] >> 6 & 3) == 1))
                bad |= HP_TV_M_GPIO19;
        }
        if (!t->touched[s])
            continue;
        a = tv_addr(s);
        v = t->saved[s];
        mask = 0xFFFFFFFF;
        if (s == TV_S_PCLK)
        {
            /* a clock word reads back without its 0xA5A5 password
             * (quirks.md #25): put it back, compare the low half */
            v = 0xA5A50000 | (v & 0xFFFF);
            mask = 0xFFFF;
        }
        else if (s == TV_S_MODE)
            v &= 0x7FFFFFFF, mask = 0x7FFFFFFF;     /* bit 31: the field */
        else if (s == TV_S_DMA)
            v &= 0x1FFFFFFF, mask = 0x1FFFFFFF;     /* 31:29: the state */
        REG(a) = v;
        if ((REG(a) ^ v) & mask)
            bad |= s < TV_S_PCLK ? HP_TV_M_MV : 1u << (s - TV_S_PCLK + 1);
    }
    tv_progress(t, HP_TV_P_DONE, 0);
    return bad;
}

static void cmd_tv(uint32_t tag, const uint32_t *a, uint32_t n)
{
    struct tv_run t;
    uint32_t r[HP_TV_REPLY_WORDS], code = tv_code, i, k, out, ms, us;
    uint32_t t0, now, step, next, f, prev, last_ms, x = HP_TV_X_OK;

    if (n < 2 || a[0] != HP_TV_MAGIC || a[1] >= HP_TV_STEPS)
    {
        reply(HP_OP_TV, tag, HP_E_INVAL, 0, 0);
        return;
    }
    if (a[1] == HP_TV_UNLOCK)
    {
        code = (REG(TIMER_CLO) ^ beat << 13 ^ tag << 7 ^ tv_code) * 0x9E3779B1u;
        code ^= code >> 15;
        tv_code = code ? code : 1;
        reply(HP_OP_TV, tag, n == 2 ? HP_OK : HP_E_INVAL, &tv_code, n == 2);
        if (n != 2)
            tv_code = 0;
        return;
    }
    tv_code = 0;                    /* used up, whatever follows */
    out = a[1] == HP_TV_OUTPUT;
    if (code == 0)
    {
        reply(HP_OP_TV, tag, HP_E_UNSUPPORTED, 0, 0);
        return;
    }
    if (n != (out ? 8u : 5u) || a[3] != code || a[2] > HP_TV_PAL ||
        a[4] == 0 || a[4] > (out ? HP_TV_OUTPUT_MS : HP_TV_TIMING_MS) ||
        (out && (a[5] >= HP_TV_ARMS || a[6] > HP_TV_W704 ||
                 a[7] > HP_TV_PAT_LINES)))
    {
        reply(HP_OP_TV, tag, HP_E_INVAL, 0, 0);
        return;
    }
    if (session)
    {
        reply(HP_OP_TV, tag, HP_E_BUSY, 0, 0);
        return;
    }
    if (!(MB(BCMFW_MB_INFO) & BCMFW_INFO_TIMER_OK))
    {
        reply(HP_OP_TV, tag, HP_E_STATE, 0, 0);
        return;
    }
    for (i = 0; i != TV_SLOTS; i++)
        t.touched[i] = 0;
    for (i = 0; i != HP_TV_REPLY_WORDS; i++)
        r[i] = 0;
    t.allow = out ? TV_G_TIMING | TV_G_DAC | TV_G_GPIO | TV_G_DMA : TV_G_TIMING;
    t.denied = t.pin19 = t.lev19 = 0;
    t.std = a[2];
    ms = a[4];

    /* PB-7 step 1: the DAC and the mode word as they are */
    tv_progress(&t, HP_TV_P_SAVE, 0);
    r[HP_TV_R_DAC0] = REG(TV_DAC);
    r[HP_TV_R_MODE0] = REG(TV_MODE);
    if (out)
    {
        volatile uint32_t *cb = (volatile uint32_t *)HP_TV_CB;

        /* PB-8 step 6: a chain of one block that links to itself, the
         * line buffer to the FIFO; transfer information 0x1900A001 with
         * the source width (4 bytes, 2 << 4), as Apple's @7C9A builds */
        cb[0] = HP_TV_CB;
        cb[1] = HP_TV_BUF;
        cb[2] = TV_FIFO;
        cb[3] = tv_fill(a[5], a[6] == HP_TV_W704 ? 704 : 640, a[7]);
        cb[4] = 0x1900A001 | 2 << 4;
        cb[5] = cb[6] = cb[7] = 0;

        /* Section 2.3 step 2: the Macrovision words only with 0x5404
         * bit 0 set; the off set is all zero but word 19, which is 4 */
        tv_progress(&t, HP_TV_P_MV, 0);
        if (r[HP_TV_R_DAC0] & 1)
        {
            for (i = 0; i != TV_MV_WORDS; i++)
                tv_set(&t, TV_S_MV0 + i, i == 19 ? 4 : 0);
            r[HP_TV_R_FLAGS] |= HP_TV_F_MV;
        }
    }
    /* Step 3: the pixel clock, the 27 MHz crystal divided by 1 */
    tv_progress(&t, HP_TV_P_CLOCK, 0);
    tv_set(&t, TV_S_PCLK, 0xA5A50501);
    if (out)
    {
        /* Step 4 (and PB-8 step 3): GPIO 19 an output, as Apple's boot
         * leaves it, then high */
        tv_progress(&t, HP_TV_P_GPIO, 0);
        f = REG(TV_FSEL1);
        if ((f >> 6 & 3) != 1)
        {
            tv_set(&t, TV_S_FSEL, (f & ~0xC0u) | 0x40);
            r[HP_TV_R_FLAGS] |= HP_TV_F_FSEL;
        }
        tv_pin19(&t, 1);
        /* Step 5: the DAC on, bit 6 the 625-line mode, bit 7 Apple's
         * S-Video default */
        tv_progress(&t, HP_TV_P_DAC, 0);
        tv_set(&t, TV_S_DAC, t.std == HP_TV_PAL ? 0xE0 : 0xA0);
    }
    /* Steps 6-10; PB-9 changes only the input's samples a line to 704 */
    tv_progress(&t, HP_TV_P_TIMING, 0);
    tv_set(&t, TV_S_IN, tv_timing[t.std][0] +
           (out && a[6] == HP_TV_W704 ? 704 - 640 : 0));
    tv_set(&t, TV_S_OSZ, tv_timing[t.std][1]);
    tv_set(&t, TV_S_HT, tv_timing[t.std][2]);
    tv_set(&t, TV_S_VT, tv_timing[t.std][3]);
    tv_set(&t, TV_S_MODE, t.std == HP_TV_PAL ? 0x43 : 0x41);
    /* Step 11: the control word, the same for both standards */
    tv_progress(&t, HP_TV_P_CTL, 0);
    tv_set(&t, TV_S_CTL, 0x00033125);
    r[HP_TV_R_CLK] = REG(TV_PCLK);
    if (out)
    {
        /* PB-8 step 5: bit 31, then bit 30, 10 ms each at most */
        tv_progress(&t, HP_TV_P_WAIT31, 0);
        if (tv_wait(TV_CTL, 0x80000000, HP_TV_WAIT_US))
            x = HP_TV_X_TIMEOUT31;
        else
        {
            tv_progress(&t, HP_TV_P_WAIT30, 0);
            if (tv_wait(TV_CTL, 0x40000000, HP_TV_WAIT_US))
                x = HP_TV_X_TIMEOUT30;
        }
        if (x == HP_TV_X_OK)
        {
            /* Channel 6 as Apple configures it (config 0x0200FF01), the
             * status cleared, the chain started on sub-channel 0 (the
             * LCD's on silicon; Apple's TV sub-channel is not read) */
            tv_progress(&t, HP_TV_P_DMA, 0);
            tv_set(&t, TV_S_DMA, 0x0200FF01);
            tv_poke(&t, TV_DMA6 + 0x04, 0xFFFFFFFF);
            tv_poke(&t, TV_DMA6 + 0x38, HP_TV_CB);
        }
    }
    if (x == HP_TV_X_OK)
    {
        /* The window: field changes, ten samples, the chain alive */
        tv_progress(&t, HP_TV_P_HOLD, 0);
        us = ms * 1000;
        step = us / HP_TV_SAMPLES;
        next = step;
        last_ms = 0;
        t0 = REG(TIMER_CLO);
        prev = REG(TV_MODE) >> 31;
        r[HP_TV_R_FIELDS] = 0;
        for (i = k = 0; k != 64 * us; k++)
        {
            now = REG(TIMER_CLO) - t0;
            f = REG(TV_MODE) >> 31;
            r[HP_TV_R_FIELDS] += f != prev;
            prev = f;
            if (out && REG(TV_DMA6 + 0x38) == 0)
            {
                x = HP_TV_X_CHAIN;
                break;
            }
            if (now >= next && i != HP_TV_SAMPLES)
            {
                r[HP_TV_R_SAMPLES + 2 * i] = REG(TV_CTL);
                r[HP_TV_R_SAMPLES + 2 * i + 1] = out ? REG(TV_DMA6 + 0x04) : 0;
                i++;
                next += step;
            }
            if (now >> 10 != last_ms)
            {
                last_ms = now >> 10;
                tv_progress(&t, HP_TV_P_HOLD, last_ms);
                heartbeat();
            }
            if (now >= us)
                break;
        }
        r[HP_TV_R_US] = REG(TIMER_CLO) - t0;
    }
    r[HP_TV_R_EXIT] = x;
    r[HP_TV_R_PHASE] = t.phase;
    r[HP_TV_R_MISMATCH] = tv_restore(&t);
    r[HP_TV_R_FLAGS] |= t.denied << 8;
    reply(HP_OP_TV, tag, x == HP_TV_X_OK ? HP_OK : HP_E_STATE, r,
          HP_TV_REPLY_WORDS);
}

/* PINS' four-word form (PB-2): all 52 levels sampled for us, nothing
 * changed (flags must be 0), edges counted per pin */
static void cmd_census(uint32_t tag, const uint32_t *a)
{
    uint32_t m0 = a[0], m1 = a[3] & 0xFFFFF, us = a[1];
    uint32_t c[HP_CENSUS_PINS], r[HP_CENSUS_WORDS], k, t0, now, lim;
    uint32_t p0, p1, v0, v1, d0, d1;
    uint32_t and0 = ~0u, and1 = ~0u, or0 = 0, or1 = 0, samples = 0;

    if (a[2] != 0 || (m0 | m1) == 0 || (int32_t)us < 0 ||
        (int32_t)us > 1000000)
    {
        reply(HP_OP_PINS, tag, HP_E_INVAL, 0, 0);
        return;
    }
    for (k = 0; k != HP_CENSUS_PINS; k++)
        c[k] = 0;
    lim = MB(BCMFW_MB_INFO) & BCMFW_INFO_TIMER_OK ? 0x7FFFFFFFu : 8 * us;
    t0 = REG(TIMER_CLO);
    p0 = REG(GPIO_LEV0);
    p1 = REG(GPIO_LEV1);
    for (;;)
    {
        v0 = REG(GPIO_LEV0);
        v1 = REG(GPIO_LEV1);
        d0 = (v0 ^ p0) & m0;
        d1 = (v1 ^ p1) & m1;
        if (d0)
            for (k = 0; k != 32; k++)
                c[k] += d0 >> k & 1;
        if (d1)
            for (k = 0; k != 20; k++)
                c[32 + k] += d1 >> k & 1;
        and0 &= v0;
        and1 &= v1;
        or0 |= v0;
        or1 |= v1;
        p0 = v0;
        p1 = v1;
        samples++;
        now = REG(TIMER_CLO);
        if ((int32_t)(now - t0 - us) >= 0 || samples == lim)
            break;
    }
    r[0] = samples;
    r[1] = now - t0;
    r[2] = and0 & m0;
    r[3] = and1 & m1;
    r[4] = or0 & m0;
    r[5] = or1 & m1;
    for (k = 0; k != HP_CENSUS_EDGE_WORDS; k++)
        r[6 + k] = 0;
    for (k = 0; k != HP_CENSUS_PINS; k++)
        r[6 + (k >> 2)] |= (c[k] > 255 ? 255 : c[k]) << (8 * (k & 3));
    reply(HP_OP_PINS, tag, HP_OK, r, HP_CENSUS_WORDS);
}

/* A pull on one pin, Apple's way (@13F7A, as pins_pull): the mode, a
 * wait, the pin clocked on its word, a wait, both back to 0 */
static void pull_one(uint32_t clk, uint32_t bit, uint32_t mode)
{
    volatile uint32_t i;

    REG(GPIO_PUD) = mode;
    for (i = 150; i != 0; i--)
        ;
    REG(clk) = bit;
    for (i = 150; i != 0; i--)
        ;
    REG(GPIO_PUD) = 0;
    REG(clk) = 0;
}

/* PULL (PB-4): per pin, up then down, 1 ms each, the level read twice
 * under each; the down pull is also the restore (Apple's boot state).
 * Never pins 1-3, 19, 23 or 48-51 (the allowlist), never a pin whose
 * function isn't input; no fsel word is written */
static void cmd_pull(uint32_t tag, const uint32_t *a, uint32_t n)
{
    uint32_t r[HP_PULL_MAX], i, p, w, fs, clk, bit, lev, sh, allow;

    if (n != 3 || a[0] != HP_PULL_MAGIC || a[2] == 0 ||
        a[2] > HP_PULL_MAX || a[1] > 47)
    {
        reply(HP_OP_PULL, tag, HP_E_INVAL, 0, 0);
        return;
    }
    for (i = 0; i != a[2]; i++)
    {
        p = a[1] + i;
        w = p;
        sh = p & 31;
        allow = p > 47 ? 0 : p < 32 ? HP_PULL_ALLOW_LO : HP_PULL_ALLOW_HI;
        if (!(allow >> sh & 1))
        {
            r[i] = w | HP_PULL_SKIP_PIN;
            continue;
        }
        fs = REG(GPIO_FSEL0 + 4 * (p >> 4)) >> (2 * (p & 15)) & 3;
        if (fs)
        {
            r[i] = w | HP_PULL_SKIP_FSEL | fs << 12;
            continue;
        }
        clk = p < 32 ? GPIO_PUDCLK : GPIO_PUDCLK1;
        lev = p < 32 ? GPIO_LEV0 : GPIO_LEV1;
        bit = 1u << sh;
        pull_one(clk, bit, 2);              /* up */
        pb_wait_us(1000);
        w |= (REG(lev) >> sh & 1) << 16;
        w |= (REG(lev) >> sh & 1) << 17;
        pull_one(clk, bit, 1);              /* down: and restored */
        pb_wait_us(1000);
        w |= (REG(lev) >> sh & 1) << 18;
        w |= (REG(lev) >> sh & 1) << 19;
        r[i] = w;
    }
    reply(HP_OP_PULL, tag, HP_OK, r, a[2]);
}

/* CACHE (PW2): Apple's flush (@296: bit 30, poll bit 0 up to 4,096
 * reads), then the word; returns the polls */
#define PB_CACHE_CTL 0x10000C00
static uint32_t cache_set(uint32_t cc)
{
    uint32_t k;

    REG(PB_CACHE_CTL) = REG(PB_CACHE_CTL) | 0x40000000;
    for (k = 0; k != 4096; k++)
        if (REG(PB_CACHE_CTL) & 1)
            break;
    REG(PB_CACHE_CTL) = cc;
    return k;
}

static uint32_t cache_loads(uint32_t base, uint32_t wmask, uint32_t step,
                            uint32_t *sum)
{
    volatile uint32_t *w = (volatile uint32_t *)base;
    uint32_t i, s = 0, t0 = REG(TIMER_CLO);

    for (i = 0; i != HP_CACHE_LOADS; i++)
        s += w[(i * step) & wmask];
    *sum = s;
    return REG(TIMER_CLO) - t0;
}

/* The host's answer on the step word, 100 ms at most: 1 if it never came */
static uint32_t coh_wait(uint32_t want)
{
    volatile uint32_t *step = (volatile uint32_t *)HP_CACHE_STEP;
    uint32_t t0 = REG(TIMER_CLO), k;

    for (k = 0; k != 0x1000000; k++)
    {
        if (*step == want)
            return 0;
        if ((int32_t)(REG(TIMER_CLO) - t0 - HP_CACHE_STEP_US) >= 0)
            break;
    }
    return 1;
}

static void cmd_cache(uint32_t tag, const uint32_t *a, uint32_t n)
{
    uint32_t r[5], cc, was, polls, to = 0;

    if (n != 4 || a[0] != HP_CACHE_MAGIC || a[1] > HP_CACHE_COH ||
        (a[2] != 0 && a[2] != 0x80000000u && a[2] != 0x80010000u) ||
        a[3] > (a[1] == HP_CACHE_COH ? 1u : HP_CACHE_R_SDRAM1M))
    {
        reply(HP_OP_CACHE, tag, HP_E_INVAL, 0, 0);
        return;
    }
    was = REG(PB_CACHE_CTL);
    if (!video_sdram_ready() || (session && fifo_next != 0) ||
        was != HP_CACHE_OURS)
    {
        reply(HP_OP_CACHE, tag, HP_E_STATE, &was, 1);
        return;
    }
    cc = a[2];
    if (a[1] == HP_CACHE_TIME)
    {
        uint32_t base = a[3] == HP_CACHE_R_SRAM ? HP_CACHE_SRAM :
                        a[3] == HP_CACHE_R_SDRAM ? HP_CACHE_SDRAM :
                        HP_CACHE_SDRAM1M;
        uint32_t wmask = a[3] == HP_CACHE_R_SDRAM1M ? 0x3FFFF : 0xFFF;
        uint32_t step = a[3] == HP_CACHE_R_SDRAM1M ? 8 : 1;

        polls = cache_set(cc);
        r[0] = cache_loads(base, wmask, step, &r[2]);   /* cold */
        r[1] = cache_loads(base, wmask, step, &r[3]);   /* warm */
        r[4] = polls + cache_set(HP_CACHE_OURS);
        reply(HP_OP_CACHE, tag, HP_OK, r, 5);
        return;
    }
    {
        volatile uint32_t *p = (volatile uint32_t *)
            (a[3] ? HP_CACHE_COH_SDRAM : HP_CACHE_COH_SRAM);
        volatile uint32_t *step = (volatile uint32_t *)HP_CACHE_STEP;

        p[0] = HP_CACHE_V0;
        p[1] = 0;
        *step = 0;
        polls = cache_set(cc);
        (void)p[0];                 /* the line in, if it is cached */
        *step = 1;
        to |= coh_wait(2);          /* the host wrote V1 */
        r[0] = p[0];
        p[1] = HP_CACHE_V2;
        *step = 3;
        to |= coh_wait(4) << 1;     /* the host read it */
        polls += cache_set(cc);     /* written back */
        *step = 5;
        to |= coh_wait(6) << 2;     /* the host read it again */
        polls += cache_set(HP_CACHE_OURS);
        r[1] = to;
        r[2] = polls;
        reply(HP_OP_CACHE, tag, HP_OK, r, 3);
    }
}

/* DMACPY (PW8): channel 7 sub-channel 1, Apple's dma_memcpy words
 * (dma.md, "Examples"): config 0x0000E802, a block of 32-byte units with
 * transfer information 0x01025151, done on status bit 14 (13 + 1) */
#define DMA7         (0x10000800 + 7 * 0x80)
static uint32_t pw8_cb[8] __attribute__((aligned(32)));

static void pw8_fill(uint32_t at, uint32_t x)
{
    volatile uint32_t *w = (volatile uint32_t *)at;
    uint32_t i;

    for (i = 0; i != HP_DMACPY_BYTES / 4; i++)
    {
        x = x * 1664525u + 1013904223u;
        w[i] = x;
    }
}

static uint32_t pw8_wrong(uint32_t dst, uint32_t src)
{
    volatile uint32_t *d = (volatile uint32_t *)dst,
                      *s = (volatile uint32_t *)src;
    uint32_t i, bad = 0;

    for (i = 0; i != HP_DMACPY_BYTES / 4; i++)
        bad += d[i] != s[i];
    return bad;
}

static void cmd_dmacpy(uint32_t tag, const uint32_t *a, uint32_t n)
{
    uint32_t r[6], src, dst, t0, i;

    if (n != 3 || a[0] != HP_DMACPY_MAGIC || a[1] >= HP_DMACPY_CASES)
    {
        reply(HP_OP_DMACPY, tag, HP_E_INVAL, 0, 0);
        return;
    }
    if ((a[1] != HP_DMACPY_SS && !video_sdram_ready()) ||
        (session && fifo_next != 0))
    {
        reply(HP_OP_DMACPY, tag, HP_E_STATE, 0, 0);
        return;
    }
    src = a[1] == HP_DMACPY_DS ? HP_DMACPY_SDRAM : HP_DMACPY_SRC;
    dst = a[1] == HP_DMACPY_SD ? HP_DMACPY_SDRAM : HP_DMACPY_DST;
    pw8_fill(src, a[2]);
    pw8_fill(dst, ~a[2]);
    pw8_cb[0] = 0;
    pw8_cb[1] = src;
    pw8_cb[2] = dst;
    pw8_cb[3] = HP_DMACPY_BYTES / 32;
    pw8_cb[4] = 0x01025151;
    pw8_cb[5] = pw8_cb[6] = pw8_cb[7] = 0;
    REG(DMA7) = 0x0000E802;
    REG(DMA7 + 0x04) = 0xFFFFFFFF;
    t0 = REG(TIMER_CLO);
    REG(DMA7 + 0x58) = (uint32_t)pw8_cb;
    r[5] = 1;
    for (i = 0; i != 0x1000000; i++)
    {
        if (REG(DMA7 + 0x04) & 0x4000)
        {
            r[5] = 0;
            break;
        }
        if ((int32_t)(REG(TIMER_CLO) - t0 - HP_DMACPY_US) >= 0)
            break;
    }
    r[0] = REG(TIMER_CLO) - t0;
    r[4] = REG(DMA7 + 0x04);
    REG(DMA7 + 0x04) = 0xFFFFFFFF;
    REG(DMA7) = 0;
    r[2] = pw8_wrong(dst, src);
    /* the core's word loop over the same copy */
    pw8_fill(dst, ~a[2]);
    {
        volatile uint32_t *d = (volatile uint32_t *)dst,
                          *s = (volatile uint32_t *)src;

        t0 = REG(TIMER_CLO);
        for (i = 0; i != HP_DMACPY_BYTES / 4; i++)
            d[i] = s[i];
        r[1] = REG(TIMER_CLO) - t0;
    }
    r[3] = pw8_wrong(dst, src);
    reply(HP_OP_DMACPY, tag, HP_OK, r, 6);
}

#if BCMFW_CLOCK
/* PW3 and PW4 (brief-map-power.md section 9; section 4 for the levels,
 * section 5 for the PLL rule).  Off by default: a step runs only with the
 * one-time code the last HP_PW_UNLOCK handed out.  One request is one
 * level (PW3, on the crystal) or one clock point (PW4); nothing outlives
 * it.
 *
 * Every value comes from Apple's code: the levels 5-15 (@141ba's table,
 * @141f0's write: the regulator's bits 7:4 kept), PLL A by Apple's rule
 * (@4cb8: M 108, so 250 kHz steps, the VCO at 75 MHz or more, enabled
 * 1 ms after N), the core word 0x100 | divider, the SDRAM refresh f /
 * 128,000 (@4d18).  The order is @5134's: a higher level before the
 * clock goes up, a lower one after; 10 ms after each regulator change.
 *
 * Every write goes through pw_set, which checks it against the step's
 * allowlist (PW_G_*: LEVEL the regulator only; POINT adds PLL A with the
 * core word, and the refresh) and against the compiled values, and saves
 * the register before its first write.  Every exit (done, a wrong
 * read-back, a wrong self-check, a stopped counter) runs pw_restore: the
 * refresh back, a higher start level before the clock goes down, the
 * core onto its start word, PLL A's words back, a lower start level
 * after; then every saved register read back.  Every wait is bounded on
 * the 1 MHz counter, and ends if the counter stops; HP_PW_PROGRESS says
 * where a run is.
 *
 * The self-check (the stage 4 kernels aren't in this image): 32 KB of
 * SRAM at HP_PW_WIN filled by the core with one of two patterns by turns,
 * then summed by the core (every word) and by the vector unit (the low
 * byte of each halfword, pw_sumr: bench row SUMR's three instructions,
 * device-checked in session 4), against the sums taken at the start. */
#define PW_REG       0x10001400
#define PW_PLLA_M    0x10001C00
#define PW_PLLA_N    0x10001C14
#define PW_CORE      0x10001C28
#define PW_REFRESH   0x10003424     /* SDRAM controller +0x24, 31:16 */
#define PW_PASSWORD  0xA5A50000
#define PW_PLL_ON    0x400
#define PW_WORDS     (HP_PW_WIN_BYTES / 4)

/* vc2sim's mutation check only (peek_test.py t_pw): 1 raises a level
 * after the clock instead of before, 2 skips the restore.  Never set in
 * an image for the device. */
#ifndef PW_MUTANT
#define PW_MUTANT    0
#endif

#define PW_G_REG     1
#define PW_G_CLK     2              /* PLL A's two words, the core word */
#define PW_G_REF     4

enum { PW_S_REG, PW_S_PLLM, PW_S_PLLN, PW_S_CORE, PW_S_REF, PW_SLOTS };

static const uint32_t pw_reg[PW_SLOTS] =
{
    PW_REG, PW_PLLA_M, PW_PLLA_N, PW_CORE, PW_REFRESH,
};

/* Apple's ladder: the clock, PLL A's N (VCO / 250 kHz), the core
 * divider, the level @141ba's table gives the clock, the refresh in core
 * cycles (f / 128,000, rounded down as Apple's 0x493 at 150 MHz) */
static const struct
{
    uint16_t mhz, n, div, level, refresh;
} pw_ladder[HP_PW_POINTS] =
{
    {  46, 368, 2,  5,  359 },      /* VCO  92 MHz */
    {  74, 592, 2,  7,  578 },      /* VCO 148 MHz (74 is below 75) */
    { 102, 408, 1,  9,  796 },
    { 150, 600, 1, 15, 1171 },      /* clock.c's 0x258 and video.c's 0x493 */
};

static uint32_t pw_code;            /* 0: none handed out */

struct pw_run
{
    uint32_t allow, denied, phase, level, stalled;
    uint32_t ladder, point, target;     /* the request */
    uint32_t lcd;                       /* the 150 MHz frame changed the
                                           panel's bus words */
    uint32_t saved[PW_SLOTS];
    uint8_t touched[PW_SLOTS];
};

/* the bench's SUMR (pwsum.S): r5 = the 16 low bytes of a 32-byte row */
void pw_sumr(const void *row, uint32_t *out);

static void pw_progress(struct pw_run *t, uint32_t ph, uint32_t ms)
{
    if (ph < HP_PW_PH_BACK)
        t->phase = ph;
    MB(HP_PW_PROGRESS) = HP_PW_PROG(ph, ms);
}

/* Is v the value this step may write to slot s?  The step's own values
 * from Apple's tables, nothing else (pw_restore writes the saved ones) */
static int pw_value_ok(const struct pw_run *t, uint32_t s, uint32_t v)
{
    uint32_t lo = v & 0xFFFF;

    if (s == PW_S_REF)
        return t->ladder && !((v ^ t->saved[PW_S_REF]) & 0xFFFF) &&
               v >> 16 == pw_ladder[t->point].refresh;
    if (v >> 16 != PW_PASSWORD >> 16)
        return 0;
    switch (s)
    {
    case PW_S_REG:      /* bits 7:4 as found, the step's level */
        return lo == ((t->saved[PW_S_REG] & 0xF0) | t->target);
    case PW_S_PLLM:
        return t->ladder && lo == 108;
    case PW_S_PLLN:     /* N, then N with the enable */
        return t->ladder && (lo & ~PW_PLL_ON) == pw_ladder[t->point].n;
    default:            /* PW_S_CORE: PLL A / divider */
        return t->ladder && lo == (0x100u | pw_ladder[t->point].div);
    }
}

/* A write through the allowlist: saved before its first write */
static void pw_set(struct pw_run *t, uint32_t s, uint32_t v)
{
    uint32_t a = pw_reg[s],
             g = s == PW_S_REG ? PW_G_REG : s == PW_S_REF ? PW_G_REF : PW_G_CLK;

    if (!t->touched[s])
        t->saved[s] = REG(a);
    if (!(t->allow & g) || !pw_value_ok(t, s, v))
    {
        t->denied++;
        return;
    }
    t->touched[s] = 1;
    REG(a) = v;
}

/* The regulator to level l (bits 7:4 as found), then the settle */
static void pw_level(struct pw_run *t, uint32_t l)
{
    uint32_t d = t->denied;

    pw_set(t, PW_S_REG, PW_PASSWORD | (t->saved[PW_S_REG] & 0xF0) | l);
    if (t->denied != d)
        return;
    t->level = l;
    t->stalled |= pw_wait(HP_PW_SETTLE_US);
}

/* One self-check: the window filled with pattern p, then both sums */
static uint32_t pw_check(uint32_t p, uint32_t *sum, uint32_t *vsum,
                         uint32_t *lsum)
{
    volatile uint32_t *w = (volatile uint32_t *)HP_PW_WIN;
    uint32_t x = p ? HP_PW_SEED_B : HP_PW_SEED_A, s = 0, l = 0, v = 0, r5;
    uint32_t t0 = REG(TIMER_CLO), i;

    for (i = 0; i != PW_WORDS; i++)
    {
        x = x * 1664525u + 1013904223u;
        w[i] = x;
    }
    for (i = 0; i != PW_WORDS; i++)
    {
        x = w[i];
        s = (s << 1 | s >> 31) + x;
        l += (x & 0xFF) + (x >> 16 & 0xFF);
    }
    for (i = 0; i != PW_WORDS; i += 8)
    {
        pw_sumr((const void *)&w[i], &r5);
        v += r5;
    }
    *sum = s;
    *vsum = v;
    if (lsum)
        *lsum = l;
    return REG(TIMER_CLO) - t0;
}

/* Every exit: back to the start's refresh, clock and level, in the safe
 * order, then each saved register read back: the HP_PW_M_* that differ */
static uint32_t pw_restore(struct pw_run *t, uint32_t lcd0, uint32_t lcd1)
{
    uint32_t bad = 0, l0 = t->saved[PW_S_REG] & 0xF;

    pw_progress(t, HP_PW_PH_BACK, 0);
    if (PW_MUTANT == 2)
        return 0;
    if (t->touched[PW_S_REF])                   /* more often first */
        REG(PW_REFRESH) = t->saved[PW_S_REF];
    if (t->touched[PW_S_REG] && l0 > t->level)  /* up before the clock */
    {
        REG(PW_REG) = PW_PASSWORD | (t->saved[PW_S_REG] & 0xFF);
        bad |= pw_wait(HP_PW_SETTLE_US) ? HP_PW_M_STALL : 0;
        t->level = l0;
    }
    if (t->touched[PW_S_CORE])                  /* the clock down */
        REG(PW_CORE) = PW_PASSWORD | (t->saved[PW_S_CORE] & 0xFFFF);
    if (t->touched[PW_S_PLLN])
        REG(PW_PLLA_N) = PW_PASSWORD | (t->saved[PW_S_PLLN] & 0xFFFF);
    if (t->touched[PW_S_PLLM])
        REG(PW_PLLA_M) = PW_PASSWORD | (t->saved[PW_S_PLLM] & 0xFFFF);
    if (t->touched[PW_S_REG] && l0 != t->level) /* down after it */
    {
        REG(PW_REG) = PW_PASSWORD | (t->saved[PW_S_REG] & 0xFF);
        bad |= pw_wait(HP_PW_SETTLE_US) ? HP_PW_M_STALL : 0;
        t->level = l0;
    }
    if (t->lcd)
        lcd_clock_changed();                    /* the words for 27 MHz */
    /* read back: the clock words and the regulator without the password
     * (quirks.md #25) */
    if (t->touched[PW_S_REG] && (REG(PW_REG) ^ t->saved[PW_S_REG]) & 0xFF)
        bad |= HP_PW_M_REG;
    if (t->touched[PW_S_REF] && REG(PW_REFRESH) != t->saved[PW_S_REF])
        bad |= HP_PW_M_REFRESH;
    if (t->touched[PW_S_CORE] && (REG(PW_CORE) ^ t->saved[PW_S_CORE]) & 0xFFFF)
        bad |= HP_PW_M_CORE;
    if (t->touched[PW_S_PLLN] && (REG(PW_PLLA_N) ^ t->saved[PW_S_PLLN]) & 0xFFFF)
        bad |= HP_PW_M_PLLN;
    if (t->touched[PW_S_PLLM] && (REG(PW_PLLA_M) ^ t->saved[PW_S_PLLM]) & 0xFFFF)
        bad |= HP_PW_M_PLLM;
    if (MB(BCMFW_MB_TIMING0) != lcd0 || MB(BCMFW_MB_TIMING1) != lcd1)
        bad |= HP_PW_M_LCD;
    pw_progress(t, HP_PW_PH_DONE, 0);
    return bad;
}

/* PW4's way up to its point (@5134's order); stops at a stalled wait */
static void pw_up(struct pw_run *t)
{
    uint32_t n = pw_ladder[t->point].n;

    if (t->target > t->level && PW_MUTANT != 1)
    {
        pw_progress(t, HP_PW_PH_VUP, 0);
        pw_level(t, t->target);
        if (t->stalled)
            return;
    }
    pw_progress(t, HP_PW_PH_PLL, 0);
    pw_set(t, PW_S_PLLM, PW_PASSWORD | 108);
    pw_set(t, PW_S_PLLN, PW_PASSWORD | n);
    t->stalled |= pw_wait(HP_PW_PLL_US);
    if (t->stalled)
        return;
    pw_set(t, PW_S_PLLN, PW_PASSWORD | n | PW_PLL_ON);
    pw_progress(t, HP_PW_PH_CORE, 0);
    pw_set(t, PW_S_CORE, PW_PASSWORD | 0x100 | pw_ladder[t->point].div);
    if (t->target < t->level || (PW_MUTANT == 1 && t->target > t->level))
    {
        pw_progress(t, HP_PW_PH_VDOWN, 0);
        pw_level(t, t->target);
        if (t->stalled)
            return;
    }
    if (t->allow & PW_G_REF)
    {
        pw_progress(t, HP_PW_PH_REFRESH, 0);
        pw_set(t, PW_S_REF, (t->saved[PW_S_REF] & 0xFFFF) |
                            (uint32_t)pw_ladder[t->point].refresh << 16);
    }
}

static void cmd_pw(uint32_t tag, const uint32_t *a, uint32_t n)
{
    struct pw_run t;
    uint32_t r[HP_PW_REPLY_WORDS], code = pw_code, i, x = HP_PW_X_OK;
    uint32_t sum[2], vsum[2], lsum, s, v, us, t0, now, last, ms, sel;
    uint32_t lcd0, lcd1;

    if (n < 2 || a[0] != HP_PW_MAGIC || a[1] >= HP_PW_STEPS)
    {
        reply(HP_OP_PW, tag, HP_E_INVAL, 0, 0);
        return;
    }
    if (a[1] == HP_PW_UNLOCK)
    {
        code = (REG(TIMER_CLO) ^ beat << 13 ^ tag << 7 ^ pw_code) * 0x9E3779B1u;
        code ^= code >> 15;
        pw_code = code ? code : 1;
        reply(HP_OP_PW, tag, n == 2 ? HP_OK : HP_E_INVAL, &pw_code, n == 2);
        if (n != 2)
            pw_code = 0;
        return;
    }
    pw_code = 0;                    /* used up, whatever follows */
    t.ladder = a[1] == HP_PW_POINT;
    if (code == 0)
    {
        reply(HP_OP_PW, tag, HP_E_UNSUPPORTED, 0, 0);
        return;
    }
    if (n != 5 || a[2] != code || a[4] > HP_PW_HOLD_MS ||
        (t.ladder ? a[3] >= HP_PW_POINTS
                  : a[3] < HP_PW_LEVEL_MIN || a[3] > HP_PW_LEVEL_MAX))
    {
        reply(HP_OP_PW, tag, HP_E_INVAL, 0, 0);
        return;
    }
    if (session)
    {
        reply(HP_OP_PW, tag, HP_E_BUSY, 0, 0);
        return;
    }
    /* the counter running, the core on the crystal as the host started
     * it (source field 10:8 = 0; never from 150 MHz), the regulator at a
     * level of Apple's table; else nothing is written.  The spec asks for
     * 0x0001 exactly; a host start's own value is not read on silicon yet,
     * and the restore writes back whatever was found */
    t.saved[PW_S_REG] = REG(PW_REG);
    t.saved[PW_S_CORE] = REG(PW_CORE);
    if (!(MB(BCMFW_MB_INFO) & BCMFW_INFO_TIMER_OK) || clock_is_fast() ||
        (t.saved[PW_S_CORE] & 0x700) != 0 ||
        (t.saved[PW_S_REG] & 0xF) < HP_PW_LEVEL_MIN ||
        (t.saved[PW_S_REG] & 0xF) > HP_PW_LEVEL_MAX)
    {
        r[0] = t.saved[PW_S_REG];
        r[1] = t.saved[PW_S_CORE];
        reply(HP_OP_PW, tag, HP_E_STATE, r, 2);
        return;
    }
    for (i = 0; i != PW_SLOTS; i++)
        t.touched[i] = 0;
    for (i = 0; i != HP_PW_REPLY_WORDS; i++)
        r[i] = 0;
    t.point = a[3];
    t.target = t.ladder ? pw_ladder[t.point].level : a[3];
    t.allow = t.ladder ? PW_G_REG | PW_G_CLK : PW_G_REG;
    t.denied = t.stalled = t.phase = t.lcd = 0;
    t.level = t.saved[PW_S_REG] & 0xF;
    ms = a[4];
    lcd_blit_wait();                /* no transfer across a switch */
    lcd0 = MB(BCMFW_MB_TIMING0);    /* the bus words as lcd.c last set them */
    lcd1 = MB(BCMFW_MB_TIMING1);
    sel = MB(BCMFW_MB_TIMING_SEL);

    pw_progress(&t, HP_PW_PH_SAVE, 0);
    r[HP_PW_R_REG0] = t.saved[PW_S_REG];
    r[HP_PW_R_CORE] = t.saved[PW_S_CORE] & 0xFFFF;
    if (t.ladder && video_sdram_ready())
    {
        /* the refresh follows the clock only with SDRAM up (an OPEN) */
        t.saved[PW_S_REF] = r[HP_PW_R_REF0] = REG(PW_REFRESH);
        t.allow |= PW_G_REF;
        r[HP_PW_R_FLAGS] |= HP_PW_F_SDRAM;
    }

    /* The references at the start level: both patterns, each vector sum
     * equal to the core's sum of the same bytes */
    pw_progress(&t, HP_PW_PH_REF, 0);
    r[HP_PW_R_US0] = pw_check(0, &sum[0], &vsum[0], &lsum);
    if (lsum != vsum[0])
        x = HP_PW_X_REFBAD;
    pw_check(1, &sum[1], &vsum[1], &lsum);
    if (lsum != vsum[1])
        x = HP_PW_X_REFBAD;
    r[HP_PW_R_SUM0] = sum[0];
    r[HP_PW_R_VSUM0] = vsum[0];

    if (x == HP_PW_X_OK)
    {
        if (t.ladder)
            pw_up(&t);
        else
        {
            /* PW3: on the crystal every level of the table is at or above
             * its 5 for 27 MHz, so no order arises */
            pw_progress(&t, t.target > t.level ? HP_PW_PH_VUP
                                               : HP_PW_PH_VDOWN, 0);
            pw_level(&t, t.target);
        }
        r[HP_PW_R_REG] = REG(PW_REG);
        r[HP_PW_R_CORE] |= (REG(PW_CORE) & 0xFFFF) << 16;
        if (t.ladder)
        {
            r[HP_PW_R_PLLA] = (REG(PW_PLLA_M) & 0xFFFF) << 16 |
                              (REG(PW_PLLA_N) & 0xFFFF);
            if (t.allow & PW_G_REF)
                r[HP_PW_R_REF] = REG(PW_REFRESH);
        }
        if (t.stalled)
            x = HP_PW_X_STALL;
        else if (t.denied || (r[HP_PW_R_REG] & 0xF) != t.target ||
                 (t.ladder &&
                  ((r[HP_PW_R_CORE] >> 16) != (0x100u | pw_ladder[t.point].div) ||
                   (r[HP_PW_R_PLLA] & 0x7FF) != (pw_ladder[t.point].n | PW_PLL_ON) ||
                   (r[HP_PW_R_PLLA] >> 16) != 108 ||
                   ((t.allow & PW_G_REF) &&
                    r[HP_PW_R_REF] >> 16 != pw_ladder[t.point].refresh))))
            x = HP_PW_X_READBACK;
    }

    /* The self-check at the point, by turns, for the hold; the first wrong
     * one ends it */
    if (x == HP_PW_X_OK)
    {
        pw_progress(&t, HP_PW_PH_CHECK, 0);
        t0 = last = REG(TIMER_CLO);
        for (i = 0; ; i++)
        {
            us = pw_check(i & 1, &s, &v, &lsum);
            if (i == 0)
                r[HP_PW_R_US] = us;
            r[HP_PW_R_SUM] = s;
            r[HP_PW_R_VSUM] = v;
            r[HP_PW_R_RUNS] = i + 1;
            if (s != sum[i & 1] || v != vsum[i & 1])
            {
                r[HP_PW_R_RUNS] |= 1u << 16;
                x = HP_PW_X_CHECK;
                break;
            }
            now = REG(TIMER_CLO);
            if (now == last)
            {
                x = HP_PW_X_STALL;          /* a whole run, no tick */
                break;
            }
            last = now;
            pw_progress(&t, HP_PW_PH_CHECK, (now - t0) >> 10);
            heartbeat();
            if (now - t0 >= ms * 1000)
                break;
        }
        r[HP_PW_R_HOLD] = REG(TIMER_CLO) - t0;
    }
    /* PW4's 150 MHz point: one frame with Apple's 150 MHz bus words (lcd.c
     * takes them for BCMFW_TIMING_APPLE_MAGIC); not with the panel off,
     * which this probe never turns on */
    if (x == HP_PW_X_OK && t.ladder && t.point == HP_PW_P_150)
    {
        if (!lcd_is_on())
            r[HP_PW_R_FLAGS] |= HP_PW_F_DARK;
        else
        {
            pw_progress(&t, HP_PW_PH_FRAME, 0);
            MB(BCMFW_MB_TIMING_SEL) = BCMFW_TIMING_APPLE_MAGIC;
            lcd_clock_changed();
            t.lcd = 1;
            t0 = REG(TIMER_CLO);
            lcd_update();
            r[HP_PW_R_FRAME] = REG(TIMER_CLO) - t0;
            r[HP_PW_R_FLAGS] |= HP_PW_F_FRAME;
            MB(BCMFW_MB_TIMING_SEL) = sel;  /* pw_restore re-applies */
        }
    }
    r[HP_PW_R_EXIT] = x;
    r[HP_PW_R_PHASE] = t.phase;
    r[HP_PW_R_MISMATCH] = pw_restore(&t, lcd0, lcd1);
    r[HP_PW_R_FLAGS] |= t.denied << 8;
    reply(HP_OP_PW, tag, x == HP_PW_X_OK ? HP_OK : HP_E_STATE, r,
          HP_PW_REPLY_WORDS);
}
#endif /* BCMFW_CLOCK */

/* PW5 (brief-map-power.md section 3 for Apple's videocore=1 park, @14288,
 * section 9 for the stage).  Off by default: a park runs only with the
 * one-time code the last HP_HIB_UNLOCK handed out.
 *
 * Apple's order: the PLLs parked (B, C, D, then A: divider and control
 * pw 0; the core is already on the crystal, which the entry check
 * demands, so its word is never written), the 14 GPIO event words saved
 * and cleared, HP_HIB_WORD = 0, SDRAM idled (video_sdram_idle), then
 * HP_HIB_PARKED and a busy poll of the word for HP_HIB_WAKE.  Interrupts
 * are already off here (the image only enables them inside its idle
 * sleep), so Apple's di and ei have nothing to do.  Ours on top: the
 * patterns (32 KB of SRAM, 32 KB of SDRAM when it is up) and the bars in
 * the host frame, laid before and checked after; the poll bounded by the
 * request's ms on the counter, and by HP_PW_STALL_READS reads of a
 * counter that doesn't move.
 *
 * Every PLL and GPIO write goes through hib_set, which checks it against
 * the step's allowlist (HB_G_*) and its value (the park's 0, or what was
 * saved) and saves the register before its first write.  Every exit
 * after the first write (woken, given up, stalled, SDRAM's bit 15 stuck)
 * runs the same restore: PLLs (each divider, its control without the
 * enable, 1 ms, then the enable; Apple's rule @4cb8), SDRAM brought up
 * again, the event words back, then every saved word read back. */
#define HB_PW        0xA5A50000
#define HB_PLL_ON    0x400
#define HB_GPIO_EV   0x10002828     /* 14 words to 0x1000285C */
#define HB_GPIO_N    14
#define HB_CACHE     0x10000C00
#define HB_REG       0x10001400
#define HB_CORE      0x10001C28
#define HB_FB_WORDS  (320 * 240 / 2)

#define HB_G_PLL     1
#define HB_G_GPIO    2

/* The slots: the eight PLL words in the park's order (B, C, D, A; each
 * divider then control), then the event words */
enum { HB_S_PLL = 0, HB_S_GPIO = 8, HB_SLOTS = 8 + HB_GPIO_N };

static uint32_t hib_code;           /* 0: none handed out */

struct hib_run
{
    uint32_t allow, denied, phase;
    uint32_t saved[HB_SLOTS];
    uint8_t touched[HB_SLOTS];
};

static uint32_t hib_addr(uint32_t s)
{
    if (s >= HB_S_GPIO)
        return HB_GPIO_EV + 4 * (s - HB_S_GPIO);
    /* Apple's PLL index i (0-3: B, C, D, A) is at +4 * ((i + 1) % 4) */
    return 0x10001C00 + 4 * ((s / 2 + 1) % 4) + (s & 1 ? 0x14 : 0);
}

static void hib_progress(struct hib_run *t, uint32_t ph, uint32_t ms)
{
    if (ph < HP_HIB_PH_WAKE)
        t->phase = ph;
    MB(HP_HIB_PROGRESS) = HP_HIB_PROG(ph, ms);
}

/* A write through the allowlist: the park's value (PLL pw 0, event word
 * 0) or the saved one (a PLL control also without its enable); saved
 * before the first write */
static void hib_set(struct hib_run *t, uint32_t s, uint32_t v)
{
    uint32_t a = hib_addr(s), g = s < HB_S_GPIO ? HB_G_PLL : HB_G_GPIO;
    uint32_t sv, ok;

    if (!t->touched[s])
        t->saved[s] = REG(a);
    sv = t->saved[s];
    if (g == HB_G_PLL)
        ok = v == HB_PW || v == (HB_PW | (sv & 0xFFFF)) ||
             ((s & 1) && v == (HB_PW | (sv & 0xFFFF & ~HB_PLL_ON)));
    else
        ok = v == 0 || v == sv;
    if (!(t->allow & g) || !ok)
    {
        t->denied++;
        return;
    }
    t->touched[s] = 1;
    REG(a) = v;
}

/* LCG words into a window */
static void hib_fill(uint32_t base, uint32_t words, uint32_t x)
{
    volatile uint32_t *w = (volatile uint32_t *)base;
    uint32_t i;

    for (i = 0; i != words; i++)
    {
        x = x * 1664525u + 1013904223u;
        w[i] = x;
    }
}

/* The words that differ from the pattern; the first one's index */
static uint32_t hib_verify(uint32_t base, uint32_t words, uint32_t x,
                           uint32_t *first)
{
    volatile uint32_t *w = (volatile uint32_t *)base;
    uint32_t i, bad = 0;

    *first = 0xFFFFFFFF;
    for (i = 0; i != words; i++)
    {
        x = x * 1664525u + 1013904223u;
        if (w[i] != x && bad++ == 0)
            *first = i;
    }
    return bad;
}

/* Eight vertical bars into the host frame (mirrored: the wake's look) */
static void hib_bars(uint32_t mirror)
{
    volatile uint32_t *p = (volatile uint32_t *)BCMFW_FB;
    uint32_t x, y, b, c;

    for (y = 0; y != 240; y++)
        for (x = 0; x != 320; x += 2)
        {
            b = x / 40;
            c = tv_bar_565[mirror ? 7 - b : b];
            *p++ = c | c << 16;
        }
}

static uint32_t hib_sum(void)
{
    volatile uint32_t *p = (volatile uint32_t *)BCMFW_FB;
    uint32_t i, s = 0;

    for (i = 0; i != HB_FB_WORDS; i++)
        s = (s << 1 | s >> 31) + p[i];
    return s;
}

/* Every exit after the first write: PLLs back (A, D, C, B), SDRAM up
 * again, the event words back; then the saved words read back (the PLL
 * words without the password, quirks.md #25; the enables, not the
 * write-1-to-clear status words).  The HP_HIB_M_* that differ. */
static uint32_t hib_restore(struct hib_run *t, uint32_t sdram, uint32_t cache0,
                            uint32_t *sd)
{
    uint32_t s, k, bad = 0, wait = 0;

    hib_progress(t, HP_HIB_PH_RESTORE, 0);
    for (k = 4; k-- != 0; )
    {
        s = 2 * k;
        if (t->touched[s])
            hib_set(t, s, HB_PW | (t->saved[s] & 0xFFFF));
        if (t->touched[s + 1])
        {
            hib_set(t, s + 1, HB_PW | (t->saved[s + 1] & 0xFFFF & ~HB_PLL_ON));
            wait |= t->saved[s + 1] & HB_PLL_ON;
        }
    }
    if (wait)
    {
        pw_wait(HP_PW_PLL_US);          /* N before the enable (@4cb8); a
                                           stopped counter ends it */
        for (k = 4; k-- != 0; )
            if (t->touched[2 * k + 1] && (t->saved[2 * k + 1] & HB_PLL_ON))
                hib_set(t, 2 * k + 1, HB_PW | (t->saved[2 * k + 1] & 0xFFFF));
    }
    *sd = sdram ? video_sdram_rewake() : 0xFFFFFFFF;
    for (s = HB_SLOTS; s-- != HB_S_GPIO; )
        if (t->touched[s])
            hib_set(t, s, t->saved[s]);
    for (s = 0; s != HB_SLOTS; s++)
    {
        if (!t->touched[s])
            continue;
        if (s < HB_S_GPIO)
        {
            if ((REG(hib_addr(s)) ^ t->saved[s]) & 0xFFFF)
                bad |= HP_HIB_M_PLL << s;
        }
        else if (s >= HB_S_GPIO + 2 && REG(hib_addr(s)) != t->saved[s])
            bad |= HP_HIB_M_GPIO;
    }
    if (sdram && !*sd && ((REG(HB_CACHE) ^ cache0) & 0x80010000u))
        bad |= HP_HIB_M_CACHE;
    return bad;
}

static void cmd_hib(uint32_t tag, const uint32_t *a, uint32_t n)
{
    struct hib_run t;
    uint32_t r[HP_HIB_REPLY_WORDS], code = hib_code, i, x = HP_HIB_X_OK;
    uint32_t t0, now, last, same, pms, polls, limit, w, sdram, cache0, sd;

    if (n < 2 || a[0] != HP_HIB_MAGIC || a[1] >= HP_HIB_STEPS)
    {
        reply(HP_OP_HIB, tag, HP_E_INVAL, 0, 0);
        return;
    }
    if (a[1] == HP_HIB_UNLOCK)
    {
        code = (REG(TIMER_CLO) ^ beat << 13 ^ tag << 7 ^ hib_code) * 0x9E3779B1u;
        code ^= code >> 15;
        hib_code = code ? code : 1;
        reply(HP_OP_HIB, tag, n == 2 ? HP_OK : HP_E_INVAL, &hib_code, n == 2);
        if (n != 2)
            hib_code = 0;
        return;
    }
    hib_code = 0;                   /* used up, whatever follows */
    if (code == 0)
    {
        reply(HP_OP_HIB, tag, HP_E_UNSUPPORTED, 0, 0);
        return;
    }
    if (n != 4 || a[2] != code || a[3] == 0 || a[3] > HP_HIB_WAIT_MS)
    {
        reply(HP_OP_HIB, tag, HP_E_INVAL, 0, 0);
        return;
    }
    if (session)
    {
        reply(HP_OP_HIB, tag, HP_E_BUSY, 0, 0);
        return;
    }
    /* the counter running (it bounds the park), the core on the crystal
     * (Apple's park puts it there first; we never start from PLL A) */
    r[0] = REG(HB_REG);
    r[1] = REG(HB_CORE);
    if (!(MB(BCMFW_MB_INFO) & BCMFW_INFO_TIMER_OK) ||
#if BCMFW_CLOCK
        clock_is_fast() ||
#endif
        (r[1] & 0x700) != 0)
    {
        reply(HP_OP_HIB, tag, HP_E_STATE, r, 2);
        return;
    }
    for (i = 0; i != HB_SLOTS; i++)
        t.touched[i] = 0;
    for (i = 0; i != HP_HIB_REPLY_WORDS; i++)
        r[i] = 0;
    t.allow = HB_G_PLL | HB_G_GPIO;
    t.denied = t.phase = 0;
    limit = a[3] * 1000;
    sdram = video_sdram_ready();
    cache0 = REG(HB_CACHE);
    t0 = REG(TIMER_CLO);
    MB(HP_HIB_WORD) = 0;
    hib_progress(&t, HP_HIB_PH_SAVE, 0);
    r[HP_HIB_R_REG] = REG(HB_REG) & 0xFFFF;
    r[HP_HIB_R_CORE] = REG(HB_CORE) & 0xFFFF;
    r[HP_HIB_R_FLAGS] = sdram ? HP_HIB_F_SDRAM : 0;

    /* the patterns, and the bars (look 1); no transfer across the park */
    hib_progress(&t, HP_HIB_PH_PATTERN, 0);
    hib_fill(HP_HIB_SRAM, HP_HIB_SRAM_BYTES / 4, HP_HIB_SEED_S);
    if (sdram)
        hib_fill(HP_HIB_SDRAM, HP_HIB_SDRAM_BYTES / 4, HP_HIB_SEED_D);
    hib_progress(&t, HP_HIB_PH_LOOK1, 0);
    lcd_blit_wait();
    hib_bars(0);
    r[HP_HIB_R_FRAME_PRE] = hib_sum();
    if (lcd_is_on())
    {
        r[HP_HIB_R_FLAGS] |= HP_HIB_F_PANEL;
        lcd_update();
        lcd_blit_wait();
    }

    /* Apple's park: PLLs, event words, SDRAM */
    hib_progress(&t, HP_HIB_PH_PLL, 0);
    for (i = HB_S_PLL; i != HB_S_GPIO; i++)
        hib_set(&t, i, HB_PW);
    hib_progress(&t, HP_HIB_PH_GPIO, 0);
    for (i = HB_S_GPIO; i != HB_SLOTS; i++)
        hib_set(&t, i, 0);
    if (sdram)
    {
        hib_progress(&t, HP_HIB_PH_SDRAM, 0);
        if (video_sdram_idle())
            x = HP_HIB_X_SDIDLE;
    }

    /* parked: the host's pins, then its wake word (or our bound) */
    polls = 0;
    now = REG(TIMER_CLO);
    if (x == HP_HIB_X_OK)
    {
        hib_progress(&t, HP_HIB_PH_PARKED, 0);
        r[HP_HIB_R_PARK_US] = now - t0;
        r[HP_HIB_R_CLO_PARK] = t0 = last = now;
        same = pms = 0;
        MB(HP_HIB_WORD) = HP_HIB_PARKED;
        for (;;)
        {
            w = MB(HP_HIB_WORD);
            if (w == HP_HIB_WAKE)
                break;
            if (polls != 0xFFFFFFFF)
                polls++;
            now = REG(TIMER_CLO);
            if (now != last)
            {
                last = now;
                same = 0;
                if (now - t0 >= limit)
                {
                    x = HP_HIB_X_GAVEUP;
                    break;
                }
                if ((now - t0) >> 10 != pms)
                {
                    pms = (now - t0) >> 10;
                    MB(HP_HIB_PROGRESS) = HP_HIB_PROG(HP_HIB_PH_PARKED, pms);
                }
            }
            else if (++same == HP_PW_STALL_READS)
            {
                x = HP_HIB_X_STALL;
                break;
            }
        }
        now = REG(TIMER_CLO);
        r[HP_HIB_R_CLO_WAKE] = now;
        r[HP_HIB_R_HELD_US] = now - t0;
        r[HP_HIB_R_POLLS] = polls;
        hib_progress(&t, HP_HIB_PH_WAKE, 0);
    }

    /* everything back, then the word: awake, or given up */
    r[HP_HIB_R_MISMATCH] = hib_restore(&t, sdram, cache0, &sd);
    r[HP_HIB_R_SD] = sd;
    if (x == HP_HIB_X_OK && sd && sdram)
        x = HP_HIB_X_SDRAM;
    MB(HP_HIB_WORD) = x == HP_HIB_X_OK ? HP_HIB_AWAKE : HP_HIB_GAVEUP;
    r[HP_HIB_R_RESTORE_US] = REG(TIMER_CLO) - now;
    r[HP_HIB_R_REG] |= REG(HB_REG) << 16;
    r[HP_HIB_R_CORE] |= REG(HB_CORE) << 16;

    /* what the dark kept */
    hib_progress(&t, HP_HIB_PH_CHECK, 0);
    r[HP_HIB_R_SRAM_BAD] = hib_verify(HP_HIB_SRAM, HP_HIB_SRAM_BYTES / 4,
                                      HP_HIB_SEED_S, &r[HP_HIB_R_SRAM_FIRST]);
    r[HP_HIB_R_SDRAM_BAD] = r[HP_HIB_R_SDRAM_FIRST] = 0xFFFFFFFF;
    if (sdram && !sd)
        r[HP_HIB_R_SDRAM_BAD] = hib_verify(HP_HIB_SDRAM, HP_HIB_SDRAM_BYTES / 4,
                                           HP_HIB_SEED_D,
                                           &r[HP_HIB_R_SDRAM_FIRST]);
    r[HP_HIB_R_FRAME_POST] = hib_sum();

    /* look 2: the bars mirrored, so a picture the panel merely kept is
     * told from one the image pushed */
    hib_progress(&t, HP_HIB_PH_LOOK2, 0);
    hib_bars(1);
    if (lcd_is_on())
    {
        lcd_update();
        lcd_blit_wait();
    }
    r[HP_HIB_R_EXIT] = x;
    r[HP_HIB_R_PHASE] = t.phase;
    r[HP_HIB_R_FLAGS] |= t.denied << 8;
    hib_progress(&t, HP_HIB_PH_DONE, 0);
    reply(HP_OP_HIB, tag, x == HP_HIB_X_OK ? HP_OK : HP_E_STATE, r,
          HP_HIB_REPLY_WORDS);
}
#endif /* HP_PROBES */

#if BCMFW_VIDEO
/* AUDIO (stage 8b): PCM blocks the host wrote into an SDRAM ring, through
 * the core (a Q16 gain, so the host hears the chip's hand on them) into a
 * second ring the host reads back.  The PROCESS reply is its time. */
static uint32_t au_in, au_out, au_size;

static void cmd_audio(uint32_t tag, const uint32_t *a, uint32_t n)
{
    uint32_t r[1];

    if (!check_session(HP_OP_AUDIO, tag, a, n))
        return;
    if (n == 5 && a[1] == HP_AUDIO_CONFIG)
    {
        /* both rings inside the free SDRAM (video.h VIDEO_AUDIO_*) */
        if (a[4] & 3 || a[2] < VIDEO_AUDIO_OFF || a[3] < VIDEO_AUDIO_OFF ||
            a[2] + a[4] > VIDEO_AUDIO_OFF + VIDEO_AUDIO_SIZE ||
            a[3] + a[4] > VIDEO_AUDIO_OFF + VIDEO_AUDIO_SIZE)
        {
            reply(HP_OP_AUDIO, tag, HP_E_INVAL, 0, 0);
            return;
        }
        au_in = a[2];
        au_out = a[3];
        au_size = a[4];
        reply(HP_OP_AUDIO, tag, HP_OK, 0, 0);
        return;
    }
    if (n == 5 && a[1] == HP_AUDIO_PROCESS && au_size &&
        !(a[2] & 3) && !(a[3] & 3) && a[2] + a[3] <= au_size)
    {
        const int16_t *src = (const int16_t *)(VIDEO_SDRAM + au_in + a[2]);
        int16_t *dst = (int16_t *)(VIDEO_SDRAM + au_out + a[2]);
        int32_t g = (int32_t)a[4], k, cnt = (int32_t)(a[3] >> 1);
        uint32_t t0 = REG(TIMER_CLO);

        for (k = 0; k < cnt; k++)
            dst[k] = (int16_t)((src[k] * g) >> 16);
        r[0] = REG(TIMER_CLO) - t0;
        reply(HP_OP_AUDIO, tag, HP_OK, r, 1);
        return;
    }
    reply(HP_OP_AUDIO, tag, HP_E_INVAL, 0, 0);
}

#if BCMFW_AAC
/* AAC (M4, stage 9): the decoder on the host's access units in SDRAM.
 * A range must lie inside the audio rings or the AAC test I/O (video.h
 * VIDEO_AUDIO_*, VIDEO_AAC_IO*), without wrapping. */
static int aac_window(uint32_t off, uint32_t len)
{
    if (len == 0)
        return 0;
    if (off >= VIDEO_AUDIO_OFF && off < VIDEO_AUDIO_OFF + VIDEO_AUDIO_SIZE)
        return len <= VIDEO_AUDIO_OFF + VIDEO_AUDIO_SIZE - off;
    if (off >= VIDEO_AAC_IO && off < VIDEO_AAC_IO + VIDEO_AAC_IO_SIZE)
        return len <= VIDEO_AAC_IO + VIDEO_AAC_IO_SIZE - off;
    return 0;
}

/* MEMPROBE (dev42): word fills, then byte and halfword stores at p, read
 * back by words and by bytes and halfwords (hostproto.h HP_MEMPROBE_*).
 * Session 35's stage 9 failed in ways that point at the decoder's channel
 * elements in SDRAM, written mostly by byte stores no session had checked
 * there. */
static void memprobe(volatile uint8_t *p, uint32_t *r)
{
    volatile uint32_t *w = (volatile uint32_t *)p;
    volatile uint16_t *h = (volatile uint16_t *)p;
    uint32_t k;

    for (k = 0; k < 4; k++)
        w[k] = 0xA5A5A5A5u;
    p[1] = 0x11;
    p[6] = 0x22;
    h[4] = 0x3344;
    h[7] = 0x5566;
    for (k = 0; k < 4; k++)
        r[k] = w[k];
    r[4] = (uint32_t)p[1] | (uint32_t)p[6] << 8 | (uint32_t)h[4] << 16;
}

static uint32_t memprobe_sram[4];

/* MEMSTRESS (session 36c): what memprobe never did, and what the AAC
 * decoder does all the time to its channel elements in SDRAM: a store and
 * at once a load of the same place, at every width, over a window, and
 * the decoder's own halfword copy and clear.  Counts per pattern; the
 * first failure's pattern, index, got and want. */
#define MS_PATTERNS 7
#define MS_PASSES   4
#define MS_SRAM     512             /* bytes: the control */

static uint32_t ms_sram[MS_SRAM / 4];
void *aac_memcpy(void *dst, const void *src, size_t n);    /* aac.c */
void *aac_memset(void *dst, int c, size_t n);

struct ms_result
{
    uint32_t bad[MS_PATTERNS];
    uint32_t first[4];              /* pattern + 1, index, got, want */
};

static void ms_fail(struct ms_result *m, uint32_t p, uint32_t i,
                    uint32_t got, uint32_t want)
{
    m->bad[p]++;
    if (!m->first[0])
    {
        m->first[0] = p + 1;
        m->first[1] = i;
        m->first[2] = got;
        m->first[3] = want;
    }
}

static void memstress(uint8_t *base, uint32_t bytes, struct ms_result *m)
{
    volatile uint32_t *w = (volatile uint32_t *)base;
    volatile uint16_t *h = (volatile uint16_t *)base;
    volatile uint8_t *b = base;
    uint32_t words = bytes >> 2, i, pass, v = 0x2722C0DEu, g, want, k;

    for (pass = 0; pass != MS_PASSES; pass++)
    {
        for (i = 0; i != words; i++)
        {
            v = v * 1664525u + 1013904223u;
            /* 0: word store, word load */
            w[i] = v;
            g = w[i];
            if (g != v)
                ms_fail(m, 0, i, g, v);
            /* 1: byte store, byte load */
            k = (i << 2) + (v >> 30);
            b[k] = (uint8_t)(v >> 8);
            g = b[k];
            if (g != ((v >> 8) & 0xFF))
                ms_fail(m, 1, k, g, (v >> 8) & 0xFF);
            /* 2: halfword store, halfword load */
            k = (i << 1) + (v >> 31);
            h[k] = (uint16_t)(v >> 12);
            g = h[k];
            if (g != ((v >> 12) & 0xFFFF))
                ms_fail(m, 2, k, g, (v >> 12) & 0xFFFF);
            /* 3: a word, a byte into it, the word */
            w[i] = v;
            b[(i << 2) + 1] = 0x5A;
            g = w[i];
            want = (v & 0xFFFF00FFu) | 0x5A00u;
            if (g != want)
                ms_fail(m, 3, i, g, want);
            /* 4: a word, a halfword into its upper half, the word */
            w[i] = v;
            h[(i << 1) + 1] = 0xC3A5;
            g = w[i];
            want = (v & 0xFFFFu) | 0xC3A50000u;
            if (g != want)
                ms_fail(m, 4, i, g, want);
        }
    }
    /* 5: aac_memcpy's halfword path (source and destination 2 apart in
     * their words), as channel_pair_element copies its ic_stream */
    for (i = 0; i != words / 2; i++)
        w[i] = i * 0x9E3779B9u;
    aac_memcpy(base + bytes / 2 + 2, base + 4, bytes / 2 - 8);
    for (i = 0; i != bytes / 2 - 8; i++)
    {
        g = b[bytes / 2 + 2 + i];
        want = b[4 + i];
        if (g != want)
            ms_fail(m, 5, i, g, want);
    }
    /* 6: aac_memset from a halfword boundary */
    aac_memset(base + 2, 0x6B, bytes - 4);
    for (i = 0; i != bytes - 4; i++)
    {
        g = b[2 + i];
        if (g != 0x6B)
            ms_fail(m, 6, i, g, 0x6B);
    }
}

static void cmd_aac(uint32_t tag, const uint32_t *a, uint32_t n)
{
    uint32_t r[HP_AAC_STATE_WORDS], t0;
    int e;

    if (n == 2 && a[1] == HP_AAC_PROBE)
    {
        /* registers only: no session needed */
        r[0] = aac_image_probe();
        r[1] = AAC_PROBE_WANT;
        r[2] = AAC_PROBE_CASES;
        reply(HP_OP_AAC, tag, HP_OK, r, 3);
        return;
    }
    if (n == 2 && a[1] == HP_AAC_MEMPROBE)
    {
        /* SDRAM only once an OPEN has brought it up */
        memprobe((volatile uint8_t *)memprobe_sram, &r[0]);
        if (video_sdram_ready())
            memprobe((volatile uint8_t *)(VIDEO_SDRAM + VIDEO_AAC_MEMPROBE),
                     &r[5]);
        reply(HP_OP_AAC, tag, HP_OK, r, video_sdram_ready() ? 10 : 5);
        return;
    }
    if (n == 2 && a[1] == HP_AAC_MEMSTRESS)
    {
        /* SRAM (the control), then SDRAM once an OPEN brought it up */
        struct ms_result s, d;
        uint32_t k;

        for (k = 0; k != MS_PATTERNS; k++)
            s.bad[k] = d.bad[k] = 0;
        for (k = 0; k != 4; k++)
            s.first[k] = d.first[k] = 0;
        memstress((uint8_t *)ms_sram, MS_SRAM, &s);
        if (video_sdram_ready())
            memstress((uint8_t *)(VIDEO_SDRAM + VIDEO_AAC_STRESS),
                      VIDEO_AAC_STRESS_SIZE, &d);
        for (k = 0; k != MS_PATTERNS; k++)
        {
            r[k] = s.bad[k];
            r[MS_PATTERNS + k] = d.bad[k];
        }
        for (k = 0; k != 4; k++)
            r[2 * MS_PATTERNS + k] = d.first[k];
        r[2 * MS_PATTERNS + 4] = video_sdram_ready();
        reply(HP_OP_AAC, tag, HP_OK, r, 2 * MS_PATTERNS + 5);
        return;
    }
    if (n == 3 && a[1] == HP_AAC_STATE)
    {
        /* the last DECODE's parse checkpoints from pair a[2] on (no
         * session needed: read after a refusal too) */
        const struct aac_trace *t = aac_trace_get();
        int k, w = 1, last = (int)t->n;     /* signed: no bcs/bcc */

        r[0] = t->n;
        for (k = (int)(a[2] & 0xFF); k < last && w + 2 <= HP_AAC_STATE_WORDS;
             k++)
        {
            r[w++] = t->pair[k][0];
            r[w++] = t->pair[k][1];
        }
        reply(HP_OP_AAC, tag, HP_OK, r, w);
        return;
    }
    if (!check_session(HP_OP_AAC, tag, a, n))
        return;
    if (codec != HP_CODEC_H264)
    {
        reply(HP_OP_AAC, tag, HP_E_STATE, 0, 0);
        return;
    }
    if ((n == 4 || n == 5) && a[1] == HP_AAC_OPEN)
    {
        if (!aac_window(a[2], a[3]))
        {
            reply(HP_OP_AAC, tag, HP_E_INVAL, 0, 0);
            return;
        }
        /* the tables as the first OPEN copied them (a diagnostic OPEN's
         * memory test since then overwrote them: boot again) */
        if (!aac_image_ready())
        {
            reply(HP_OP_AAC, tag, HP_E_STATE, 0, 0);
            return;
        }
        r[0] = r[1] = 0;
        /* an optional flags word (aac.h AAC_FLAG_*: stage 9's A/B) */
        aac_set_flags(n == 5 ? a[4] : 0);
        e = aac_open((const uint8_t *)(VIDEO_SDRAM + a[2]), a[3], &r[0], &r[1]);
        r[2] = (uint32_t)e;
        r[3] = aac_image_info(&r[4], &r[5], &r[6]);
        aac_live = e == 0;
        reply(HP_OP_AAC, tag, e ? HP_E_UNSUPPORTED : HP_OK, r, 7);
        return;
    }
    if (n == 5 && a[1] == HP_AAC_DECODE)
    {
        if (!aac_live)
        {
            reply(HP_OP_AAC, tag, HP_E_STATE, 0, 0);
            return;
        }
        if (!aac_window(a[2], a[3]) || a[4] & 3 || !aac_window(a[4], AAC_PCM_BYTES))
        {
            reply(HP_OP_AAC, tag, HP_E_INVAL, 0, 0);
            return;
        }
        t0 = REG(TIMER_CLO);
        e = aac_decode((const uint8_t *)(VIDEO_SDRAM + a[2]), a[3],
                       (int16_t *)(VIDEO_SDRAM + a[4]));
        r[1] = REG(TIMER_CLO) - t0;
        r[0] = e > 0 ? (uint32_t)e : 0;
        r[2] = video_get_stats()->mhz;
        r[3] = (uint32_t)e;
        reply(HP_OP_AAC, tag, e < 0 ? HP_E_CORRUPT : HP_OK, r, 4);
        return;
    }
    if (n == 2 && a[1] == HP_AAC_RESET && aac_live)
    {
        aac_reset();
        reply(HP_OP_AAC, tag, HP_OK, 0, 0);
        return;
    }
    reply(HP_OP_AAC, tag, HP_E_INVAL, 0, 0);
}
#endif

/* TONE (stage 8a-1): the BCM's own audio output, Apple's way (audio.md
 * sections 2-4): the block's start-up and rate, DMA channel 5 looping a
 * buffer of whole sine periods; then all of it off and the pins back to
 * inputs.  If the block never reports ready (bit 23: it has no clocks)
 * nothing is played.  The pins: cmd_tone. */
#define AU_CTL      0x10004800
#define AU_FIFO     0x10004804
#define AU_X10      0x10004810
#define AU_DIV      0x10004814
#define DMA5        (0x10000800 + 5 * 0x80)
#define TONE_BUF    0x12C000            /* free SRAM above VIDEO_FB */

static uint32_t tone_cb[8] __attribute__((aligned(32)));

/* The tone's samples, in one of four layouts (dev42; the block's sample
 * format isn't known, one of two unproven readings of session 35's
 * screech, data-paths.md D6, the other its uncapped level): 0 16-bit
 * stereo as is, 1 the
 * same byte-swapped, 2 32-bit slots with the sample in the high half, 3 in
 * the low half.  Always 35,280 bytes of whole periods; the amplitude is
 * capped at TONE_AMP (-40 dBFS) whatever the host asks. */
#define TONE_AMP    327
#define TONE_BYTES  35280

static void tone_fill(uint32_t fmt, uint32_t step)
{
    uint32_t k, ph = 0, frames = fmt >= 2 ? TONE_BYTES / 8 : TONE_BYTES / 4;
    uint16_t *h = (uint16_t *)TONE_BUF;

    for (k = 0; k < frames; k++)
    {
        /* a parabola per half period, 256 steps a period */
        int32_t i = (int32_t)(ph >> 16 & 255), x = (i & 127) - 64;
        int32_t y = TONE_AMP - (TONE_AMP * x * x) / 4096;
        uint16_t v;

        if (i & 128)
            y = -y;
        v = (uint16_t)y;
        if (fmt == 1)
            v = (uint16_t)(v << 8 | v >> 8);
        if (fmt >= 2)
        {
            uint16_t hi = fmt == 2 ? v : (uint16_t)(y < 0 ? 0xFFFF : 0);
            uint16_t lo = fmt == 2 ? 0 : v;

            h[4 * k] = h[4 * k + 2] = lo;       /* little-endian words */
            h[4 * k + 1] = h[4 * k + 3] = hi;
        }
        else
            h[2 * k] = h[2 * k + 1] = v;
        ph += step;
    }
}

/* PB-5 (brief-map-peripherals.md section 11): Apple's two audio clock
 * words, read only */
#define TONE_CLK2   0x10001C34
#define TONE_CLK3   0x10001C38

/* Loops the buffer for ms; *mid = the block's control word halfway,
 * clk[0..1] the two audio clock words then */
static int tone_play(uint32_t ms, uint32_t start, uint32_t *mid,
                     uint32_t *clk)
{
    uint32_t t0 = REG(TIMER_CLO);

    tone_cb[0] = (uint32_t)tone_cb;            /* next: itself (a loop) */
    tone_cb[1] = TONE_BUF;
    tone_cb[2] = AU_FIFO;
    tone_cb[3] = TONE_BYTES;
    tone_cb[4] = 0x1000A041;                    /* Apple's data block */
    tone_cb[5] = tone_cb[6] = tone_cb[7] = 0;
    REG(DMA5) = 0x06076002;                     /* DREQ 7 */
    REG(DMA5 + 0x04) = 0xFFFFFFFF;
    REG(DMA5 + 0x38) = (uint32_t)tone_cb;
    REG(AU_CTL) |= start;
    while ((int32_t)(REG(TIMER_CLO) - t0 - ms * 500) < 0)
        ;
    *mid = REG(AU_CTL);
    clk[0] = REG(TONE_CLK2);
    clk[1] = REG(TONE_CLK3);
    while ((int32_t)(REG(TIMER_CLO) - t0 - ms * 1000) < 0)
        ;
    REG(AU_CTL) = 0;
    REG(DMA5) = 0;
    return 0;
}

/* Edges on GPIO 2 (r[0]) and 3 (r[1]) over us microseconds, the pins as
 * they are; bounded in samples too if the counter doesn't run */
static void tone_edges(uint32_t us, uint32_t r[2])
{
    uint32_t t0 = REG(TIMER_CLO), prev, v, d, k = 0,
             lim = MB(BCMFW_MB_INFO) & BCMFW_INFO_TIMER_OK ? 0x7FFFFFFFu
                                                           : 8 * us;

    r[0] = r[1] = 0;
    prev = *(volatile uint32_t *)GPIO_LEV0;
    for (;;)
    {
        v = *(volatile uint32_t *)GPIO_LEV0;
        d = v ^ prev;
        r[0] += d >> 2 & 1;
        r[1] += d >> 3 & 1;
        prev = v;
        k++;
        if ((int32_t)(REG(TIMER_CLO) - t0 - us) >= 0 || k == lim)
            break;
    }
}

/* The block takes its bit and frame clocks from the codec on GPIO 2 and 3
 * (audio.md section 5, "Clock direction"): session 35 had them on
 * function 2 and the block reported ready within seconds of the PP's
 * stop, the codec's clocks still running; session 36c left them inputs
 * and it never did.  So: listen first, and only with the codec's frame
 * clock on GPIO 3 put 2 and 3 on Apple's function 2 (the clocks into the
 * block; nothing here asks for Apple's clock-manager clocks, so the block
 * has no clock of its own to drive them with), wait for ready, then GPIO
 * 1 (the data, which floats: 8a-0b) and play.  Without the codec's clocks
 * nothing is muxed or driven. */
#define TONE_LISTEN_US  3000        /* 132 frames at 44.1 kHz */
#define TONE_START_OK   0x6208      /* Apple's start bits (@138E0) */

static void cmd_tone(uint32_t tag, const uint32_t *a, uint32_t n)
{
    uint32_t r[14] = { 0 }, fsel, t, e[2], start = 0x6209;

    /* magic, ms (at most 1500), divider, layout (0-3), phase step (Q16,
       256 a period), and optionally the start bits (a subset of Apple's
       0x6209, bit 0 always): one tone a call, so the host labels each */
    if ((n != 5 && n != 6) || a[0] != HP_TONE_MAGIC || a[1] == 0 ||
        (int32_t)a[1] > 1500 || (int32_t)a[2] < 16 ||
        (int32_t)a[2] > 0xFFFF || a[3] > 3 || a[4] == 0 ||
        (int32_t)a[4] > 0x100000 ||
        (n == 6 && (a[5] & ~(TONE_START_OK | 1u)) != 0))
    {
        reply(HP_OP_TONE, tag, HP_E_INVAL, 0, 0);
        return;
    }
#if BCMFW_JOBS && BCMFW_JOBS_OVL
    /* the buffer (0x12C000-0x1349CF) is inside the merged image's overlay
     * region (0x12B000-0x133FFF): refused while an overlay is loaded,
     * whose code it would overwrite */
    if (jobs_ovl_live())
    {
        reply(HP_OP_TONE, tag, HP_E_BUSY, 0, 0);
        return;
    }
#endif
    if (n == 6)
        start = a[5] | 1;
    fsel = *(volatile uint32_t *)GPIO_FSEL0;
    r[3] = REG(AU_CTL);                         /* before we touch it */
    r[8] = REG(TONE_CLK2);                      /* PB-5: before the rate */
    r[9] = REG(TONE_CLK3);
    /* GPIO 1-3 inputs, then listen for the codec's clocks */
    *(volatile uint32_t *)GPIO_FSEL0 = fsel & ~0xFCu;
    tone_edges(TONE_LISTEN_US, e);
    r[6] = e[0];
    r[7] = e[1];
    if (e[1] != 0)
    {
        /* GPIO 2 and 3 to function 2: the clocks into the block */
        *(volatile uint32_t *)GPIO_FSEL0 = (fsel & ~0xFCu) | 0xA0u;
        REG(AU_CTL) = 0x6001;                   /* Apple's start-up */
        REG(AU_X10) = 1;
        REG(AU_DIV) = 0xA5080010;
        REG(AU_CTL) = 0;
        REG(AU_DIV) = 0xA5080000 | a[2];        /* the rate */
        REG(AU_CTL) |= 1;
        for (t = REG(TIMER_CLO); (int32_t)(REG(TIMER_CLO) - t - 65000) < 0; )
            if (REG(AU_CTL) & 0x800000)
            {
                r[1] = 1;
                break;
            }
        r[0] = REG(AU_CTL);
        r[5] = *(volatile uint32_t *)GPIO_FSEL0;
        if (r[1])
        {
            /* GPIO 1 too: the data out */
            *(volatile uint32_t *)GPIO_FSEL0 = (fsel & ~0xFCu) | 0xA8u;
            tone_fill(a[3], a[4]);
            tone_play(a[1], start, &r[4], &r[10]);
            r[5] = *(volatile uint32_t *)GPIO_FSEL0;
            r[2] = a[1];
        }
    }
    REG(AU_CTL) = 0;
    REG(DMA5) = 0;
    *(volatile uint32_t *)GPIO_FSEL0 = fsel & ~0xFCu;   /* 1-3: inputs */
    r[12] = REG(TONE_CLK2);                     /* PB-5: after the close */
    r[13] = REG(TONE_CLK3);
    reply(HP_OP_TONE, tag, HP_OK, r, 14);
}

static void cmd_overlay(uint32_t tag, const uint32_t *a, uint32_t n)
{
    if (!check_session(HP_OP_OVERLAY, tag, a, n))
        return;
    if (n < 3 || !own_display)
    {
        stat_errors++;
        reply(HP_OP_OVERLAY, tag, HP_E_INVAL, 0, 0);
        return;
    }
    video_overlay(a[1], a[2], n >= 4 ? a[3] : 0);
    reply(HP_OP_OVERLAY, tag, HP_OK, 0, 0);
}
#endif

static void dispatch(uint32_t op, uint32_t tag, const uint32_t *a, uint32_t n)
{
    if (op <= HP_OP_PING && n < min_args[op])
    {
        stat_errors++;
        reply(op, tag, HP_E_INVAL, 0, 0);
        return;
    }
#if BCMFW_JOBS
    if (op >= 0x20 && op <= 0x3F)
    {
        uint32_t rep[HP_MAX_PAYLOAD + 1], nrep;

        jobs_dispatch(op, tag, a, n, rep, &nrep);
        if (nrep)
            reply(op, tag, rep[0], rep + 1, nrep - 1);
        return;
    }
#endif
    switch (op)
    {
    case HP_OP_PINS:
#if HP_PROBES
        if (n == 4)
        {
            cmd_census(tag, a);
            break;
        }
#endif
        cmd_pins(tag, a, n);
        break;
    case HP_OP_HELLO:
        cmd_hello(tag, a, n);
        break;
    case HP_OP_OPEN:
        cmd_open(tag, a, n);
        break;
    case HP_OP_FEED:
        cmd_feed(tag, a, n);
        break;
    case HP_OP_CLOSE:
        cmd_close(tag, a, n);
        break;
    case HP_OP_STATUS:
        cmd_status(tag, a, n);
        break;
    case HP_OP_PING:
        reply(HP_OP_PING, tag, HP_OK, a, n);
        break;
    case HP_OP_PARK:
        cmd_park(tag, a, n);
        break;
#if BCMFW_VIDEO
    case HP_OP_OVERLAY:
        cmd_overlay(tag, a, n);
        break;
    case HP_OP_AUDIO:
        cmd_audio(tag, a, n);
        break;
    case HP_OP_TONE:
        cmd_tone(tag, a, n);
        break;
    case HP_OP_LAB:
        cmd_lab(tag, a, n);
        break;
#if BCMFW_VCLK
    case HP_OP_VCLK:
        cmd_vclk(tag, a, n);
        break;
#endif
#endif
#if HP_PROBES
    case HP_OP_PEEK:
        cmd_peek(tag, a, n);
        break;
    case HP_OP_TV:
        cmd_tv(tag, a, n);
        break;
    case HP_OP_PULL:
        cmd_pull(tag, a, n);
        break;
    case HP_OP_CACHE:
        cmd_cache(tag, a, n);
        break;
    case HP_OP_DMACPY:
        cmd_dmacpy(tag, a, n);
        break;
#if BCMFW_CLOCK
    case HP_OP_PW:
        cmd_pw(tag, a, n);
        break;
#endif
    case HP_OP_HIB:
        cmd_hib(tag, a, n);
        break;
#endif
#if BCMFW_AAC
    case HP_OP_AAC:
        cmd_aac(tag, a, n);
        break;
#endif
    default:
        reply(op, tag, HP_E_UNKNOWN, 0, 0);
        break;
    }
}

/* One pass over the command ring.  Called from the main loop between
 * LCD commands; never waits. */
void hp_poll(void)
{
    uint32_t w = MB(HP_CMD_WR);
    uint32_t args[HP_MAX_PAYLOAD];

    heartbeat();
    if (!HP_VALID(w))
        return;                 /* half-written index: next time */
    w &= 0xFFFF;
    while (cmd_rd != w)
    {
        uint32_t hdr = CMD[cmd_rd % HP_CMD_WORDS];
        uint32_t n = HP_REC_LEN(hdr), avail = (w - cmd_rd) & 0xFFFF, i;

        if (n + 1 > avail)
            break;              /* inconsistent: wait for the rest */
        if (n > HP_MAX_PAYLOAD)
        {
            reply(HP_REC_OP(hdr), HP_REC_TAG(hdr), HP_E_TOOBIG, 0, 0);
        }
        else
        {
            for (i = 0; i < n; i++)
                args[i] = CMD[(cmd_rd + 1 + i) % HP_CMD_WORDS];
            dispatch(HP_REC_OP(hdr), HP_REC_TAG(hdr), args, n);
        }
        cmd_rd = (cmd_rd + 1 + n) & 0xFFFF;
        MB(HP_CMD_RD) = HP_ENC(cmd_rd);
        if (park_flags)
            break;              /* nothing after a PARK runs here */
    }
    publish_state();
    if (park_flags)
        park();
}

#if BCMFW_JOBS && (BCMFW_JOBS_DECODE || BCMFW_JOBS_OVL)
void hp_heartbeat(void)
{
    heartbeat();
}
#endif

/* LCD_SLEEP while a session is open: it ends here, the host is told */
void hp_lcd_sleep(void)
{
    uint32_t r[1];

    if (!session)
        return;
    r[0] = session;
    end_session();
    post(HP_EV_LOST, 0, HP_OK, r, 1);
    publish_state();
}

#if BCMFW_VIDEO
/* Decoder work between command passes: one packet, then FIFO_RD moves
 * on by the space it held.  Returns 1 if there may be more to do. */
int hp_work(void)
{
    uint32_t units;

    if (!session || codec != HP_CODEC_H264)
        return 0;
    units = video_work();
    if (!units)
        return 0;
    fifo_rd = (fifo_rd + units) & 0xFFFF;
    MB(HP_FIFO_RD) = HP_ENC(fifo_rd);
    if (video_get_stats()->fatal)
        state = HP_ST_ERROR;
    heartbeat();
    publish_state();
    return 1;
}
#endif

#endif /* BCMFW_HOSTPROTO */
