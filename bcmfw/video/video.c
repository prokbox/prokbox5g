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
 * bcmfw video sessions (BCMFW_VIDEO): FEED packets from the SDRAM
 * bitstream FIFO through our H.264 decoder, each picture converted to
 * RGB565 on the vector unit (2:1 box above 320x240) and pushed to the
 * panel by DMA.  Design: docs/research/bcm2722/pipeline.md.
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
#include "bcmfw.h"
#include "hw.h"
#include "hostproto.h"
#include "video.h"
#include "h264.h"
#include "disp.h"
#include "h264_dsp.h"
#include "clock.h"
#include "aac/aac_image.h"
#if BCMFW_JOBS
#include "jobs.h"
#include "bschk.h"
#endif

void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);

#if BCMFW_VIDEO

#define PROG(ph, detail) \
    (MB(VIDEO_MB_PROG) = (uint32_t)(ph) << 24 | ((detail) & 0xFFFFFF))
#define PROG_A(a)        (MB(VIDEO_MB_PROG_A) = (uint32_t)(a))

/* disp_kernels.S: one 32-byte row through d00 (vld16, vst16) */
void video_vpu_row(void *dst, const void *src);

struct packet
{
    uint32_t off, len, pts_lo, pts_hi, flags;
};

static struct packet queue[VIDEO_QUEUE_DEPTH];
static uint32_t q_rd, q_wr;                 /* free-running counts */
static struct h264_dec *dec;
static struct video_stats st;
static int sdram_ready;
static uint32_t mode;
static uint32_t pic_pts_lo, pic_pts_hi;     /* PTS of the picture in progress */
static int pic_open;
static uint32_t pic_flags;               /* its packet's FEED flags */
static uint32_t ovl_x, ovl_y, ovl_w, ovl_h;     /* HP_OP_OVERLAY; ovl_w 0: none */
/* the band's slide (HP_OVL_SLIDE_*): ovl_shift rows lowered, so panel rows
 * ovl_y + ovl_shift on show the rectangle's top ovl_h - ovl_shift rows;
 * sl_mode 0 none, 1 up, 2 down (then away) */
static uint32_t ovl_shift, sl_mode, sl_t0, sl_us, sl_from, sl_to;
#define VIS_Y   (ovl_y + ovl_shift)
#define VIS_H   (ovl_h - ovl_shift)
static void overlay_blit(void);
static uint32_t last_err_us, err_events;
/* the displayed geometry: source crop (cx, cy), output ow x oh at
 * (ox, oy) on the panel, scale 1 or 2; gw x gh = the picture it is for */
static uint32_t gw, gh, cx, cy, ow, oh, ox, oy, scale;

/* ---- SDRAM controller: Apple's order, our code (hardware.md, "SDRAM
 * controller"; field meanings [M]/[L]) ---- */

/* What the last set-up did (hostproto.md, "OPEN"): VIDEO_SD_* word,
 * the last +0x20 read, the probe words read back */
static uint32_t sd_diag[VIDEO_SD_WORDS];

/* The guard (video.h VIDEO_GUARD): the header's fixed words as the
 * session opened */
#define GUARD(k) (((volatile uint32_t *)VIDEO_GUARD)[k])
static uint32_t guard_want[3];
static uint32_t dump_pic;       /* HP_OPEN_DUMP */
/* the dump and the fingerprints come from the first session after the
 * image starts, not from a restart (session 18's dump was picture 60) */
static uint32_t first_done, first_fp[7];
static uint32_t selftest_done;  /* HP_OPEN_SELFTEST ran since the start */

static uint32_t fnv_rows(const uint8_t *p, uint32_t stride, uint32_t w,
                         uint32_t h)
{
    uint32_t hsh = 0x811C9DC5u, x, y;

    for (y = 0; y < h; y++, p += stride)
        for (x = 0; x < w; x++)
            hsh = (hsh ^ p[x]) * 0x01000193u;
    return hsh;
}

static void guard_open(uint32_t flags)
{
    uint32_t k;

    /* words 16 on (the self-test, the dumps, the fingerprints) belong to
       the first session after the image starts and survive later OPENs
       (a restart must not re-run them: session 23) */
    for (k = 0; k < 16; k++)
        GUARD(k) = 0;
    for (k = 48; k < VIDEO_GUARD_WORDS; k++)
        GUARD(k) = 0;
    if (!selftest_done)
        for (k = 16; k < 48; k++)
            GUARD(k) = 0;
    guard_want[0] = MB(HP_VERSION);
    guard_want[1] = MB(HP_CMD_BASE);
    guard_want[2] = MB(HP_EVT_BASE);
    GUARD(12) = flags & HP_OPEN_C_MASK;
    if (first_done & 1)                 /* the first session's dump; the */
        GUARD(39) |= VIDEO_DUMP_MAGIC | 1;  /* self-test's bits stay (s. 26) */
    GUARD(0) = VIDEO_GUARD_MAGIC;
}

static void guard_check(uint32_t where)
{
    uint32_t evt = MB(HP_EVT_WR);

    GUARD(14) += 1;
    if (GUARD(1) || (MB(HP_VERSION) == guard_want[0] &&
                     MB(HP_CMD_BASE) == guard_want[1] &&
                     MB(HP_EVT_BASE) == guard_want[2] && HP_VALID(evt)))
        return;
    GUARD(2) = where;
    GUARD(3) = q_rd;
    GUARD(4) = MB(HP_VERSION);
    GUARD(5) = MB(HP_CMD_BASE);
    GUARD(6) = MB(HP_EVT_BASE);
    GUARD(7) = evt;
    GUARD(13) = st.decoded;
    GUARD(1) = MB(VIDEO_MB_PROG) | 0x80000000u;    /* never 0 */
    GUARD(15) = GUARD(1);
}

#include "sdram_up.h"

/* A diagnostic OPEN's memory test (dev11, session 10's lost image): the
 * size by address aliasing, a grid every 64 KB, then one vector row
 * loaded from and one stored to SDRAM.  A progress word goes up before
 * each access, so a hang names it.  Returns the VIDEO_MT_* word. */
static uint32_t sdram_memtest(void)
{
    volatile uint32_t *sd = (volatile uint32_t *)VIDEO_SDRAM;
    uint32_t size = 0x400000, p, a, bad = 0, w = VIDEO_MT_RAN, k;
    uint32_t *row = (uint32_t *)VIDEO_STRIP;     /* 32-byte aligned SRAM */

    for (p = 0x400; p < 0x400000; p <<= 1)
    {
        PROG(VIDEO_PH_MT_SIZE, 0);
        PROG_A(VIDEO_SDRAM + p);
        sd[0] = 0xA5A5A5A5;
        sd[p >> 2] = p;
        if (sd[0] != 0xA5A5A5A5)
        {
            size = p;            /* address bit p is not decoded */
            break;
        }
    }
    for (a = 0; a < size; a += 0x10000)
    {
        PROG(VIDEO_PH_MT_GRID, a >> 16);
        PROG_A(VIDEO_SDRAM + a);
        sd[a >> 2] = a ^ 0x5A5A0000u;
    }
    for (a = 0; a < size; a += 0x10000)
    {
        PROG(VIDEO_PH_MT_GRID, a >> 16 | 0x800000);
        if (sd[a >> 2] != (a ^ 0x5A5A0000u))
            bad++;
    }
    /* a vector row: SDRAM to SRAM, then SRAM to SDRAM */
    for (k = 0; k < 8; k++)
        sd[0x1000 + k] = 0x01020304u * (k + 1);
    PROG(VIDEO_PH_MT_VLD, 0);
    PROG_A(VIDEO_SDRAM + 0x4000);
    video_vpu_row(row, (const void *)(VIDEO_SDRAM + 0x4000));
    for (k = 0; k < 8 && row[k] == 0x01020304u * (k + 1); k++)
        ;
    if (k == 8)
        w |= VIDEO_MT_VLD;
    for (k = 0; k < 8; k++)
        row[k] = 0x0A0B0C0Du ^ k;
    PROG(VIDEO_PH_MT_VST, 0);
    PROG_A(VIDEO_SDRAM + 0x4040);
    video_vpu_row((void *)(VIDEO_SDRAM + 0x4040), row);
    for (k = 0; k < 8 && sd[0x1010 + k] == (0x0A0B0C0Du ^ k); k++)
        ;
    if (k == 8)
        w |= VIDEO_MT_VST;
    PROG(VIDEO_PH_OPEN, 0);
    return VIDEO_MT_MAGIC | (size >> 16 & 0xFF) << 16
         | (bad > 0xFF ? 0xFF : bad) << 8 | w;
}

const uint32_t *video_sdram_diag(void)
{
    return sd_diag;
}

#if !BCMFW_JOBS
/* PW5 (hostproto.c cmd_hib; brief-map-power.md section 3): Apple's SDRAM
 * idle before its hibernate park (@14892: the cache flushed, bits 30 and
 * 16 cleared, command 0, bit 15 polled up to 8,000 times) and its
 * bring-up after the wake (@147ca, the two probe words kept: read here
 * while SDRAM still answers).  The plain video image only: the merged
 * image is unchanged. */
static uint32_t hib_probe[2];

uint32_t video_sdram_idle(void)
{
    uint32_t k;

    hib_probe[0] = MB(SDRAM_PROBE_A);
    hib_probe[1] = MB(SDRAM_PROBE_B);
    cache_flush(0);
    REG(CACHE_CTL) &= ~0x40010000u;
    REG(SDRAM_CTL(0x20)) = 0;
    for (k = 0; k < HP_HIB_SDIDLE_POLLS; k++)
        if (!(REG(SDRAM_CTL(0x20)) & 0x8000))
            return 0;
    return 1;
}

uint32_t video_sdram_rewake(void)
{
    uint32_t r = sdram_init();

    if (r)
    {
        sdram_ready = 0;            /* the next OPEN tries again */
        return r;
    }
    MB(SDRAM_PROBE_A) = hib_probe[0];
    MB(SDRAM_PROBE_B) = hib_probe[1];
    return 0;
}
#endif

/* ---- sessions ---- */

/* ---- the core clock (pipeline.md, "The 150 MHz clock") ---- */

#if BCMFW_VCLK
/* P19 PW4: clock.c's changes keep SDRAM refreshed often enough: 27 MHz's
 * interval before (the most often, right at every clock, so a fallback to
 * the crystal is never refreshed too slowly), the clock's own after */
void video_clock_refresh(int after)
{
    if (!sdram_ready)
        return;
    if (after)
        sdram_refresh();
    else
        REG(SDRAM_CTL(0x24)) = (REG(SDRAM_CTL(0x24)) & 0xFFFF)
                             | (uint32_t)BCMFW_SDRAM_REFRESH << 16;
}

/* OPEN with HP_OPEN_FAST_CLOCK: 150 MHz (the top band) for the session's
 * start; CLOSE: the crystal.  Between, the host's governor moves it
 * (HP_OP_VCLK) */
static void video_clock(int up)
{
    clock_gov_set(up ? CLOCK_BANDS - 1 : 0, 0);
}
#elif BCMFW_CLOCK
/* No transfer runs across a switch; SDRAM refresh is never too slow for
 * the clock; the panel gets the bus timing for the new clock */
static void video_clock(int up)
{
    struct clock_readback rb;

    if (up == clock_is_fast())
        return;
    lcd_blit_wait();
    if (up)
    {
        clock_fast(0, &rb);
        if (sdram_ready)
            sdram_refresh();
    }
    else
    {
        if (sdram_ready)
        {
            REG(SDRAM_CTL(0x24)) = (REG(SDRAM_CTL(0x24)) & 0xFFFF)
                                 | (uint32_t)BCMFW_SDRAM_REFRESH << 16;
        }
        clock_slow();
    }
    lcd_clock_changed();
}
#else
#define video_clock(up) do { } while (0)
#endif

int video_open(uint32_t w, uint32_t h, uint32_t m, uint32_t flags)
{
    if (m != HP_MODE_FIT && m != HP_MODE_NATIVE)
        return HP_E_UNSUPPORTED;      /* FILL needs a non-integer scale */
    /* the clock first: SDRAM is set up for the clock it will run at */
    if (flags & HP_OPEN_FAST_CLOCK)
        video_clock(1);
    PROG(VIDEO_PH_OPEN, 0);
    MB(VIDEO_MB_PROG_N) = 0;
    MB(VIDEO_MB_DPB) = 0;
    if (!sdram_ready)
    {
        if (sdram_init())
        {
            video_clock(0);     /* a retry without the flag runs at 27 MHz */
            return HP_E_NOMEM;
        }
        sdram_ready = 1;
    }
#if BCMFW_JOBS
    /* the merged image, once per boot: SDRAM against SRAM load timings
       for the bench (bschk.c; the FIFO's start and the arena, read only) */
    if (!(MB(BCMFW_JOBS_BLOCK + J_LAT_MHZ) & J_LAT_DONE))
        bschk_latency((const void *)(VIDEO_SDRAM + VIDEO_FIFO_OFF),
                      (const void *)VIDEO_ARENA);
#endif
    sd_diag[4] = 0;
    if (flags & HP_OPEN_SDRAM_DPB)
        sd_diag[4] = sdram_memtest();
#if BCMFW_AAC
    /* the AAC tables crt0 parked at the arena's top: into SDRAM before
     * the decoder takes the arena (once per boot) */
    aac_image_place();
#endif
    /* the frames in SRAM after the context when they fit (dev11: the
     * vector unit's SDRAM access is session 10's suspect), in SDRAM
     * when they don't, or always for a diagnostic OPEN */
    dec = h264_open((void *)VIDEO_ARENA, VIDEO_ARENA_SIZE);
    h264_frames_arena_first(dec, !(flags & HP_OPEN_SDRAM_DPB));
    if (!dec || h264_set_frames(dec, (void *)(VIDEO_SDRAM + VIDEO_DPB_OFF),
                                VIDEO_DPB_SIZE))
    {
        sd_diag[0] = (sd_diag[0] & ~0xF000u)
                   | (dec ? VIDEO_SD_FRAMES : VIDEO_SD_CTX) << 12;
        dec = 0;
        video_clock(0);
        return HP_E_NOMEM;
    }
    memset(&st, 0, sizeof(st));
    q_rd = q_wr = 0;
    mode = m;
    /* the kernels: vectors, or scalar C per family (dev14's A/B) */
    h264_vpu_off = (flags & HP_OPEN_C_MASK & ~HP_OPEN_C_DISP)
                 >> HP_OPEN_C_SHIFT;
    disp_use_c = !!(flags & HP_OPEN_C_DISP);
    dump_pic = !!(flags & HP_OPEN_DUMP);
    video_st_dumped = 0;
    guard_open(flags);
    if ((flags & HP_OPEN_SELFTEST) && !selftest_done)
    {
        selftest_done = 1;
        uint32_t out[7], each[11], k, fail = video_selftest(out, each, 0);

        GUARD(17) = out[0];
        GUARD(18) = out[1];
        GUARD(19) = out[2];
        GUARD(20) = out[3];
        GUARD(21) = out[4];
        GUARD(22) = out[5];
        GUARD(23) = out[6];
        for (k = 0; k < 11; k++)
            GUARD(24 + k) = each[k];
        GUARD(35) = (uint32_t)video_selftest_coef;
        if (video_st_dumped)
            GUARD(39) |= VIDEO_DUMP_MAGIC | 2;
        {
            /* again from a cleared VRF: the dumps compare with vc2sim's
               byte for byte, and a pass here would mean the kernels
               read lanes silicon never wrote (dev22) */
            uint32_t o2[7], e2[11];

            GUARD(47) = 0x53430000u | video_selftest(o2, e2, 1);
        }
        GUARD(39) |= VIDEO_DUMP_MAGIC | 4;      /* the VRF dumps */
        {
            uint32_t a[2], af = video_alutest(a);

            GUARD(37) = a[0];
            GUARD(38) = a[1];
            GUARD(36) = 0x414C0000u | af;
        }
        GUARD(16) = VIDEO_ST_MAGIC | fail;
        guard_check(VIDEO_GC_SELFTEST);
    }
    gw = gh = 0;
    ovl_w = 0;
    ovl_shift = 0;
    sl_mode = 0;
    pic_open = 0;
    err_events = 0;
    last_err_us = REG(TIMER_CLO) - 2000000;
    return HP_OK;
}

void video_close(void)
{
    lcd_blit_wait();
    dec = 0;
    q_rd = q_wr = 0;
    video_clock(0);
}

int video_sdram_ready(void)
{
    return sdram_ready;
}

#if BCMFW_JOBS_STAGED
int video_sdram_up(void)
{
    if (!sdram_ready && sdram_init() == VIDEO_SD_OK)
        sdram_ready = 1;
    return sdram_ready;
}
#endif

const struct video_stats *video_get_stats(void)
{
    st.queued = q_wr - q_rd;
#if BCMFW_VCLK
    st.mhz = clock_mhz();
#elif BCMFW_VIDEO_PLL
    st.mhz = clock_is_fast() ? 150 : 27;
#else
    st.mhz = 27;
#endif
    return &st;
}

int video_queue(uint32_t off, uint32_t len, uint32_t pts_lo, uint32_t pts_hi,
                uint32_t flags)
{
    struct packet *p;

    if (q_wr - q_rd >= VIDEO_QUEUE_DEPTH)
        return HP_E_BUSY;
    p = &queue[q_wr++ % VIDEO_QUEUE_DEPTH];
    p->off = off;
    p->len = len;
    p->pts_lo = pts_lo;
    p->pts_hi = pts_hi;
    p->flags = flags;
    return HP_OK;
}

/* Errors: concealed by the decoder and counted; reported at most once
 * a second (or every 64 when the counter doesn't run).  Unsupported
 * streams and streams too large for the DPB, before any picture: the
 * session stops decoding (STATUS shows HP_ST_ERROR). */
static void decode_error(int r, const struct packet *p)
{
    uint32_t ev[2], now = REG(TIMER_CLO);
    int timer = MB(BCMFW_MB_INFO) & BCMFW_INFO_TIMER_OK;

    st.errors++;
    ev[0] = p->pts_lo;
    ev[1] = p->pts_hi;
    if ((r == H264_E_UNSUPPORTED || r == H264_E_NOMEM) && !st.decoded)
    {
        st.fatal = r == H264_E_NOMEM ? HP_E_NOMEM : HP_E_UNSUPPORTED;
        hp_event(HP_EV_ERROR, st.fatal, ev, 2);
        return;
    }
    if (timer ? now - last_err_us >= 1000000 : !(err_events++ & 63))
    {
        last_err_us = now;
        hp_event(HP_EV_ERROR, HP_E_CORRUPT, ev, 2);
    }
}

/* Where a picture goes: FIT shows it whole (2:1 above 320x240), NATIVE
 * 1:1 centred, cropped to the panel.  The borders are cleared once. */
static void geometry(uint32_t w, uint32_t h)
{
    gw = w;
    gh = h;
    cx = cy = 0;
    if (mode == HP_MODE_FIT && (w > BCMFW_LCD_WIDTH || h > BCMFW_LCD_HEIGHT))
    {
        scale = 2;
        ow = (w >> 1) & ~1u;
        oh = (h >> 1) & ~1u;
    }
    else
    {
        scale = 1;
        ow = w < BCMFW_LCD_WIDTH ? w : BCMFW_LCD_WIDTH;
        oh = h < BCMFW_LCD_HEIGHT ? h : BCMFW_LCD_HEIGHT;
        cx = ((w - ow) >> 1) & ~1u;
        cy = ((h - oh) >> 1) & ~1u;
    }
    if (ow > BCMFW_LCD_WIDTH)
        ow = BCMFW_LCD_WIDTH;
    if (oh > BCMFW_LCD_HEIGHT)
        oh = BCMFW_LCD_HEIGHT;
    ox = ((BCMFW_LCD_WIDTH - ow) >> 1) & ~1u;
    oy = (BCMFW_LCD_HEIGHT - oh) >> 1;
    lcd_blit_wait();
    memset((void *)VIDEO_FB, 0, VIDEO_FB_PITCH * BCMFW_LCD_HEIGHT);
    /* black borders; a picture that fills the panel needs none (dev39:
       what was on the panel stays until the first picture, no flash) */
    if (ow < BCMFW_LCD_WIDTH || oh < BCMFW_LCD_HEIGHT)
    {
        lcd_blit((const uint16_t *)VIDEO_FB, BCMFW_LCD_WIDTH, 0, 0,
                 BCMFW_LCD_WIDTH, BCMFW_LCD_HEIGHT);
        overlay_blit();         /* the clear took the host's strip too */
    }
}

/* ---- the host's overlay (HP_OP_OVERLAY) ------------------------------ */

static void overlay_blit(void)
{
    if (ovl_w && VIS_H)
        lcd_blit((const uint16_t *)BCMFW_FB + ovl_y * BCMFW_LCD_WIDTH + ovl_x,
                 BCMFW_LCD_WIDTH, ovl_x, VIS_Y, ovl_w, VIS_H);
}

/* The picture to the panel.  Under an overlay band as wide as the picture
 * (the shell's scrub bar), only the rows around it go out, so the strip
 * is never overdrawn (no flicker while playing, dev39); any other overlay
 * goes back on top after the picture */
static void picture_blit(const uint16_t *fb)
{
    uint32_t top, bot;

    if (!ovl_w || ovl_x > ox || ovl_x + ovl_w < ox + ow)
    {
        lcd_blit(fb, BCMFW_LCD_WIDTH, ox, oy, ow, oh);
        overlay_blit();
        return;
    }
    top = VIS_Y > oy ? VIS_Y : oy;                  /* the band in */
    bot = ovl_y + ovl_h < oy + oh ? ovl_y + ovl_h : oy + oh;  /* panel rows */
    if (top >= bot)
    {
        lcd_blit(fb, BCMFW_LCD_WIDTH, ox, oy, ow, oh);  /* off the picture */
        return;
    }
    if (top > oy)
        lcd_blit(fb, BCMFW_LCD_WIDTH, ox, oy, ow, top - oy);
    if (bot < oy + oh)
        lcd_blit(fb + (bot - oy) * BCMFW_LCD_WIDTH, BCMFW_LCD_WIDTH, ox, bot,
                 ow, oy + oh - bot);
}

/* back to the picture: black where the band was, then the last picture
 * shown (VIDEO_FB still holds it) over its part */
static void overlay_off(void)
{
    uint32_t r;

    if (!ovl_w)
        return;
    lcd_blit_wait();
    for (r = 0; r < ovl_h; r++)
        memset((uint16_t *)BCMFW_FB + (ovl_y + r) * BCMFW_LCD_WIDTH + ovl_x,
               0, 2 * ovl_w);
    overlay_blit();
    ovl_w = 0;
    ovl_shift = 0;
    sl_mode = 0;
    if (gw)
        lcd_blit((const uint16_t *)VIDEO_FB, BCMFW_LCD_WIDTH, ox, oy, ow, oh);
    lcd_blit_wait();            /* on the panel before the reply */
}

/* Panel rows y .. y + n - 1 that a band sliding down has left: black, then
 * the picture's part.  The black comes one row at a time from a zero row,
 * only when the picture does not cover them (letter- or pillarbox) */
static void restore_rows(uint32_t y, uint32_t n)
{
    static uint16_t zero_row[BCMFW_LCD_WIDTH];
    uint32_t top, bot, r;

    if (!gw || oy > y || oy + oh < y + n || ox > ovl_x
        || ox + ow < ovl_x + ovl_w)
        for (r = 0; r < n; r++)
            lcd_blit(zero_row, BCMFW_LCD_WIDTH, ovl_x, y + r, ovl_w, 1);
    if (!gw)
        return;
    top = y > oy ? y : oy;
    bot = y + n < oy + oh ? y + n : oy + oh;
    if (top < bot)
        lcd_blit((const uint16_t *)VIDEO_FB + (top - oy) * BCMFW_LCD_WIDTH,
                 BCMFW_LCD_WIDTH, ox, top, ow, bot - top);
}

static int clock_ok(void)
{
    return (MB(BCMFW_MB_INFO) & BCMFW_INFO_TIMER_OK) != 0;
}

static void slide_to(uint32_t mode, uint32_t to, uint32_t ms)
{
    sl_mode = mode;
    sl_from = ovl_shift;
    sl_to = to;
    sl_us = ms * 1000;
    sl_t0 = REG(TIMER_CLO);
}

void video_overlay(uint32_t xy, uint32_t wh, uint32_t slide)
{
    uint32_t x = xy & 0xFFFF, y = xy >> 16, w = wh & 0xFFFF, h = wh >> 16;
    uint32_t ms = HP_OVL_SLIDE_MS(slide), was;

    if (x >= BCMFW_LCD_WIDTH || y >= BCMFW_LCD_HEIGHT)
        w = 0;
    if (w > BCMFW_LCD_WIDTH - x)
        w = BCMFW_LCD_WIDTH - x;
    if (h > BCMFW_LCD_HEIGHT - y)
        h = BCMFW_LCD_HEIGHT - y;
    if (slide & HP_OVL_SLIDE_DOWN)
    {
        if (!ovl_w)
            return;
        if (!ms || !clock_ok())
            overlay_off();
        else
            slide_to(2, ovl_h, ms);
        return;
    }
    if (!w || !h)
    {
        overlay_off();
        return;
    }
    /* the rows it lowers from: wherever it is, or all the way down when
       it was not up */
    was = ovl_w ? ovl_shift : h;
    ovl_x = x;
    ovl_y = y;
    ovl_w = w;
    ovl_h = h;
    ovl_shift = was > h ? h : was;
    if ((slide & HP_OVL_SLIDE_UP) && ms && clock_ok())
    {
        if (ovl_shift)
            slide_to(1, 0, ms);
    }
    else if (sl_mode == 2 && ms && clock_ok())
        slide_to(1, 0, ms);             /* back up from a slide down */
    else if (sl_mode != 1)
    {
        ovl_shift = 0;                  /* at once */
        sl_mode = 0;
    }
    overlay_blit();
}

void video_anim(void)
{
    uint32_t t, f, r, e, sh, old_y;

    if (!sl_mode || !ovl_w)
        return;
    t = REG(TIMER_CLO) - sl_t0;
    f = t >= sl_us ? 256 : t / ((sl_us >> 8) ? (sl_us >> 8) : 1);
    if (f > 256)
        f = 256;
    r = 256 - f;
    e = 256 - r * r * r / 65536;        /* ease out: 1 - (1 - f)^3 */
    sh = sl_to >= sl_from ? sl_from + (sl_to - sl_from) * e / 256
                          : sl_from - (sl_from - sl_to) * e / 256;
    if (sh != ovl_shift)
    {
        old_y = VIS_Y;
        ovl_shift = sh;
        if (VIS_Y > old_y)
            restore_rows(old_y, VIS_Y - old_y);
        overlay_blit();
    }
    if (f < 256)
        return;
    if (sl_mode == 2)
        overlay_off();
    else
        lcd_blit_wait();        /* the last step out, not left pending */
    sl_mode = 0;
}

static void present(void)
{
    const struct h264_picture *pic = h264_frame(dec);
    uint16_t *fb = (uint16_t *)VIDEO_FB;
    uint32_t ys = pic->stride_y;

    st.decoded++;
    st.concealed += pic->concealed;
    if (pic->width != (int)gw || pic->height != (int)gh)
        geometry(pic->width, pic->height);
    MB(VIDEO_MB_DPB) = (uint32_t)h264_frame_base(dec);
    if (st.decoded == 1 && dump_pic && !(first_done & 1))
    {
        first_done |= 1;
        /* the first picture, tightly packed, for the host (dev18) */
        uint8_t *d = (uint8_t *)(VIDEO_SDRAM + VIDEO_DUMP_PIC);
        uint32_t w = pic->width, h = pic->height, r;

        for (r = 0; r < h; r++, d += w)
            memcpy(d, pic->y + r * ys, w);
        for (r = 0; r < h / 2; r++, d += w / 2)
            memcpy(d, pic->u + r * (ys >> 1), w / 2);
        for (r = 0; r < h / 2; r++, d += w / 2)
            memcpy(d, pic->v + r * (ys >> 1), w / 2);
        GUARD(39) |= VIDEO_DUMP_MAGIC | 1;
    }
    if (st.decoded <= 2 && !(first_done & 2))
    {
        uint32_t g = 3 * (st.decoded - 1);

        first_fp[g] = fnv_rows(pic->y, ys, pic->width, pic->height);
        first_fp[g + 1] = fnv_rows(pic->u, ys >> 1, pic->width >> 1,
                                   pic->height >> 1);
        first_fp[g + 2] = fnv_rows(pic->v, ys >> 1, pic->width >> 1,
                                   pic->height >> 1);
        if (st.decoded == 2)
            first_done |= 2;
    }
    if (!GUARD(8))
    {
        GUARD(9) = (uint32_t)pic->u;
        GUARD(10) = (uint32_t)pic->v;
        GUARD(11) = ys;
        GUARD(8) = (uint32_t)pic->y;
    }
    if (pic_flags & HP_FEED_HIDE)
    {
        /* decoded for the pictures after it (a seek's way from its key
           to the target), never shown (dev32: stage 7 saw a GOP's head
           play out late after every seek) */
        st.hidden++;
        pic_open = 0;
        return;
    }
    PROG(VIDEO_PH_CONVERT, st.decoded);
    PROG_A(pic->y);
    lcd_blit_wait();             /* the previous picture is still going out */
    if (scale == 2)
        disp_scale2(fb, VIDEO_FB_PITCH, pic->y, pic->u, pic->v, ys, ow, oh,
                    (uint8_t *)VIDEO_STRIP);
    else
        disp_convert(fb, VIDEO_FB_PITCH, pic->y + cy * ys + cx,
                     pic->u + (cy >> 1) * (ys >> 1) + (cx >> 1),
                     pic->v + (cy >> 1) * (ys >> 1) + (cx >> 1), ys, ow, oh);
    if (st.decoded == 1 && !(first_done & 4))
    {
        first_fp[6] = fnv_rows((const uint8_t *)fb, VIDEO_FB_PITCH, 2 * ow, oh);
        first_done |= 4;
    }
    {
        uint32_t k;

        for (k = 0; k < 7; k++)         /* every OPEN clears the guard */
            GUARD(40 + k) = first_fp[k];
    }
    guard_check(VIDEO_GC_CONVERT);
    PROG(VIDEO_PH_BLIT, st.decoded);
    picture_blit(fb);
    guard_check(VIDEO_GC_BLIT);
    st.shown++;
    PROG(VIDEO_PH_SHOWN, st.shown);
    st.pts_lo = pic_pts_lo;
    st.pts_hi = pic_pts_hi;
    pic_open = 0;
}

static uint32_t be32(const uint8_t *b, int n)
{
    uint32_t w = 0;
    int k;

    for (k = 0; k < 4; k++)
        w = w << 8 | (k < n ? b[k] : 0);
    return w;
}

/* What the session refused, for the host (the guard's words 48-55) */
static void record(const uint8_t *buf, int nal, int nal_len,
                   const struct packet *p, uint32_t magic, uint32_t site,
                   int r)
{
    GUARD(49) = site;
    GUARD(50) = nal_len;
    GUARD(51) = be32(buf + nal, nal_len);
    GUARD(52) = nal_len > 4 ? be32(buf + nal + 4, nal_len - 4) : 0;
    GUARD(53) = p->off;
    GUARD(54) = p->len;
    GUARD(55) = be32(buf, p->len);
    GUARD(48) = magic | (uint32_t)(-r & 0xFF) << 8 | (buf[nal] & 31);
}

/* What the session refused (it wins over a first error), or its first
   decode error, for the host (the guard's words 48-55) */
static void refusal(const uint8_t *buf, int nal, int nal_len,
                    const struct packet *p)
{
    if ((GUARD(48) & 0xFFFF0000u) != VIDEO_RF_MAGIC)
        record(buf, nal, nal_len, p, VIDEO_RF_MAGIC, h264_refused_at,
               H264_E_UNSUPPORTED);
}

static void first_error(const uint8_t *buf, int nal, int nal_len,
                        const struct packet *p, int r)
{
    if (!GUARD(48))
    {
        uint32_t sps;

        GUARD(57) = h264_ps_valid(dec, &sps);
        GUARD(58) = sps;
        record(buf, nal, nal_len, p, VIDEO_ER_MAGIC, h264_error_at, r);
    }
}

/* Every NAL unit of one Annex B packet */
static void decode_packet(const struct packet *p)
{
    uint8_t *buf = (uint8_t *)(VIDEO_SDRAM + VIDEO_FIFO_OFF) + p->off;
    int len = p->len, pos = 0, nal, nal_len, next, r;

    if (p->off + p->len > VIDEO_FIFO_SIZE)
    {
        /* the packet wraps the FIFO's end: decode a linear copy */
        uint32_t first = VIDEO_FIFO_SIZE - p->off;
        uint8_t *lin = (uint8_t *)(VIDEO_SDRAM + VIDEO_WRAP_OFF);

        if (p->len > VIDEO_WRAP_SIZE)
        {
            decode_error(H264_E_BITSTREAM, p);
            return;
        }
        PROG(VIDEO_PH_WRAP, p->len);
        memcpy(lin, buf, first);
        memcpy(lin + first, (uint8_t *)(VIDEO_SDRAM + VIDEO_FIFO_OFF),
               p->len - first);
        buf = lin;
    }
    while (h264_annexb_next(buf, len, pos, &nal, &nal_len, &next))
    {
        int type;

        pos = next;
        if (nal_len <= 0)
            continue;
        type = buf[nal] & 31;
        do
        {
            if ((type == 1 || type == 5) && !pic_open)
            {
                pic_pts_lo = p->pts_lo;
                pic_pts_hi = p->pts_hi;
                pic_flags = p->flags;
                pic_open = 1;
            }
            PROG(VIDEO_PH_NAL, type);
            PROG_A(buf + nal);
            r = h264_decode_nal(dec, buf + nal, nal_len);
            if (type == 7 || type == 8)
            {
                /* [56]: the parameter sets seen and how they parsed */
                uint32_t g = GUARD(56), sh = type == 7 ? 0 : 8;

                if (((g >> sh) & 0xFF) != 0xFF)
                    g += 1u << sh;
                g = (g & ~(0xFFu << (16 + sh))) |
                    ((uint32_t)r & 0xFF) << (16 + sh);
                GUARD(56) = g;
            }
            if (!(GUARD(59) & 0xFFFF) && type < 16)
                GUARD(59) |= 1u << (16 + type);     /* the first packet's */
            guard_check(VIDEO_GC_NAL);
            if (r == H264_FRAME || r == H264_FRAME_AGAIN)
                present();
            else if (r < 0)
            {
                decode_error(r, p);
                if (!st.fatal)
                    first_error(buf, nal, nal_len, p, r);
            }
            if (st.fatal)
            {
                refusal(buf, nal, nal_len, p);
                return;
            }
        } while (r == H264_FRAME_AGAIN);
    }
    if ((GUARD(59) & 0xFFFF) != 0xFFFF)
        GUARD(59)++;                /* [59]: packets decoded */
}

uint32_t video_work(void)
{
    struct packet *p;
    uint32_t ev[3];

    if (q_rd == q_wr)
        return 0;
    p = &queue[q_rd % VIDEO_QUEUE_DEPTH];
    if (dec && !st.fatal)
    {
        /* the packet's whole cost on the chip, for STATUS (stage 6) */
        uint32_t t0 = REG(TIMER_CLO);

        MB(VIDEO_MB_PROG_N) = q_rd + 1;
        PROG(VIDEO_PH_PACKET, q_rd);
        PROG_A(VIDEO_SDRAM + VIDEO_FIFO_OFF + p->off);
        decode_packet(p);
        st.work_us = REG(TIMER_CLO) - t0;
        if (st.work_us > st.work_max_us)
            st.work_max_us = st.work_us;
    }
    if (p->flags & HP_FEED_EOS)
    {
        if (dec && !st.fatal && h264_flush(dec) == H264_FRAME)
            present();
        /* EV_EOS goes out before hp_work advances FIFO_RD past this
         * packet (same pass): the host may see EOS one packet short.  The
         * protocol promises no order; the PP doesn't rely on one
         * (hostproto.md, "End of stream": bvp_refresh catches up, vpcore
         * ends on EOS without a FIFO wait, CLOSE resets the FIFO) */
        lcd_blit_wait();         /* EV_EOS: the last picture is on screen */
        ev[0] = 1;
        ev[1] = st.pts_lo;
        ev[2] = st.pts_hi;
        hp_event(HP_EV_EOS, HP_OK, ev, 3);
    }
    q_rd++;
    PROG(VIDEO_PH_IDLE, q_rd);
    if (dec)
        guard_check(VIDEO_GC_IDLE);
    return (p->len + 63) >> 6;
}

#endif /* BCMFW_VIDEO */
