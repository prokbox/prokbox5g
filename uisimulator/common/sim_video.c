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
 * The simulator's BCM for the video player (see sim_video.h).
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

#ifdef HAVE_BCMFW_VIDEO
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "lcd.h"
#include "sim_video.h"
#include "video/vbcm_model.h"
#ifdef HAVE_BCM_JOBS_MODEL
void bcmjob_suspend(bool on);        /* bcmfw/jobs/bcmjob.h */
#endif
#include "../../bcmfw/h264/h264.h"
#include "../../bcmfw/hostproto.h"

/* lcd-bitmap.c (HAVE_BCMFW_VIDEO) */
extern bool sim_lcd_panel_owned;
void sim_lcd_show_panel(const fb_data *panel);

static struct vbm model;
static uint8_t *sram, *sdram, *scratch, *arena;
static size_t arena_size;
static struct h264_dec *dec;
static fb_data panel[LCD_WIDTH * LCD_HEIGHT];
static uint32_t frames, errors;

uint32_t bcm_video_now_us(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (uint32_t)(tv.tv_sec * 1000000ULL + tv.tv_usec);
}

/* ---- pictures to the panel ---- */

static inline int clamp8(int v)
{
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

/* BT.601, studio range, as the BCM's conversion (pipeline.md) */
static fb_data rgb565(int y, int u, int v)
{
    int c = 298 * (y - 16), d = u - 128, e = v - 128;
    int r = clamp8((c + 409 * e + 128) >> 8);
    int g = clamp8((c - 100 * d - 208 * e + 128) >> 8);
    int b = clamp8((c + 516 * d + 128) >> 8);

    return (fb_data)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
}

/* FIT: 1:1 centred up to the panel's size, 2:1 up to twice it
   (hostproto.md's modes; the BCM scales the same way) */
/* The panel as the image leaves it: the picture, and the host frame's
   rectangle over it while an OVERLAY is up (video.c video_overlay) */
static fb_data out[LCD_WIDTH * LCD_HEIGHT];

static void push(void)
{
    uint32_t x = model.ovl_xy & 0xFFFF, y = model.ovl_xy >> 16;
    uint32_t w = model.ovl_wh & 0xFFFF, h = model.ovl_wh >> 16, r;
    const fb_data *frame = (const fb_data *)(sram + 0xE0000);

    memcpy(out, panel, sizeof(out));
    if (w && h && x + w <= LCD_WIDTH && y + h <= LCD_HEIGHT)
        for (r = 0; r < h; r++)
            memcpy(out + (y + r) * LCD_WIDTH + x,
                   frame + (y + r) * LCD_WIDTH + x, w * sizeof(fb_data));
    sim_lcd_show_panel(out);
}

static void overlay(void *ctx, const uint8_t *frame, uint32_t xy,
                    uint32_t wh)
{
    (void)ctx;
    (void)frame;
    (void)xy;
    (void)wh;
    push();
}

static void show(const struct h264_picture *p)
{
    int s = (p->width > LCD_WIDTH || p->height > LCD_HEIGHT) ? 2 : 1;
    int w = p->width / s, h = p->height / s;
    int x0 = (LCD_WIDTH - w) / 2, y0 = (LCD_HEIGHT - h) / 2, x, y;

    if (w > LCD_WIDTH)
        w = LCD_WIDTH;
    if (h > LCD_HEIGHT)
        h = LCD_HEIGHT;
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    memset(panel, 0, sizeof(panel));
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
        {
            int ly = y * s, lx = x * s, yy;

            if (s == 2)
                yy = (p->y[ly * p->stride_y + lx] +
                      p->y[ly * p->stride_y + lx + 1] +
                      p->y[(ly + 1) * p->stride_y + lx] +
                      p->y[(ly + 1) * p->stride_y + lx + 1] + 2) >> 2;
            else
                yy = p->y[ly * p->stride_y + lx];
            panel[(y0 + y) * LCD_WIDTH + x0 + x] =
                rgb565(yy, p->u[(ly / 2) * p->stride_c + lx / 2],
                       p->v[(ly / 2) * p->stride_c + lx / 2]);
        }
    frames++;
    /* the picture where the image leaves it, for the first-use check's
       readback (vbcm_model.h vbm_picture) */
    vbm_picture(&model, p->y, p->u, p->v, p->stride_y, p->stride_c,
                p->width, p->height);
    push();
}

static void present(void *ctx, const uint8_t *pkt, uint32_t len,
                    int64_t pts_us, uint32_t flags)
{
    int pos = 0, nal, next, nlen, r;

    (void)ctx;
    (void)pts_us;
    if (!dec)
        return;
    while (h264_annexb_next(pkt, (int)len, pos, &nal, &nlen, &next))
    {
        do
        {
            r = h264_decode_nal(dec, (uint8_t *)pkt + nal, nlen);
            if ((r == H264_FRAME || r == H264_FRAME_AGAIN) &&
                !(flags & HP_FEED_HIDE))
                show(h264_frame(dec));
            else if (r < 0 && !errors++)
                fprintf(stderr, "sim_video: first decode error %d at NAL "
                        "type %d (%d bytes), %lu pictures in\n", r,
                        pkt[nal] & 31, nlen, (unsigned long)frames);
        } while (r == H264_FRAME_AGAIN);
        pos = next;
    }
    /* stage A shows each packet's picture when it is done */
    if (h264_flush(dec) == H264_FRAME && !(flags & HP_FEED_HIDE))
        show(h264_frame(dec));
}

/* ---- the calls vplayer.c makes ---- */

int bcm_video_take(void)
{
    if (!sram)
    {
        sram = calloc(1, VBM_SRAM_SIZE);
        sdram = calloc(1, VBM_SDRAM_SIZE);
        scratch = calloc(1, VBM_SCRATCH);
        arena_size = h264_arena_size(40, 30, 16);
        arena = malloc(arena_size);
        if (!sram || !sdram || !scratch || !arena)
            return -1;
    }
    vbm_init(&model, sram, sdram, scratch, bcm_video_now_us);
    model.present = present;
    model.lag_one = true;       /* as the image: up at the next packet */
    model.overlay = overlay;
    /* the image's clock bands (HP_OP_VCLK) for the first-use check's
       scripts only: the rest keep today's model */
    model.vclk = getenv("SIM_VCHECK") != NULL;
    frames = errors = 0;
    memset(panel, 0, sizeof(panel));
    sim_lcd_panel_owned = true;
    return 0;
}

int bcm_video_boot(const uint8_t *img, uint32_t bytes)
{
    (void)img;
    (void)bytes;
    vbm_boot(&model);
    dec = h264_open(arena, arena_size);
    push();
    return dec ? 0 : -1;
}

void bcm_video_off(void)
{
    vbm_cut(&model);
    dec = NULL;
}

int bcm_video_hand_back(void)
{
    return -1;                  /* cut and released, as on the device
                                   without the resident image */
}

/* Which program the chip runs after a stop (quirks #48, data-paths.md
   D17).  On the device a stop either hands the resident image back
   (bcm_video_hand_back: the jobs go on) or takes the stock wake path to
   Apple's vmcs (dt_restore(false)), where our job driver finds no jobs
   until our image starts again.  The simulator's hand-back always says
   no, so its release stands for both: after the resident image's own
   play it is the hand-back, after any other play the handback to vmcs. */
static bool play_resident = true, on_vmcs;

void sim_video_resident(bool res)
{
    play_resident = res;
}

void bcm_video_release(void)
{
    sim_lcd_panel_owned = false;
#ifdef HAVE_BCM_JOBS_MODEL
    if (!play_resident && !on_vmcs)
    {
        on_vmcs = true;
        bcmjob_suspend(true);
        fprintf(stderr, "sim_video: stop: the chip on Apple's vmcs (not the "
                "resident image's play): no jobs until our image starts\n");
    }
    else if (play_resident && on_vmcs)
    {
        on_vmcs = false;
        bcmjob_suspend(false);
        fprintf(stderr, "sim_video: stop: our image handed back (the "
                "resident image's play): jobs again\n");
    }
#endif
    play_resident = true;       /* until the next play says */
    lcd_update();               /* the normal screen, as the wake path does */
}

/* Each port read costs what silicon charged (vbcm_model.h VBM_RD_NS,
   sitting 4c): the reading thread is held, as the PP is, without a
   yield, so a check or poll that reads a lot takes the device's time on
   every clock the shell looks at.  Paid in steps of 1 ms or more against
   a running deadline, so a few reads cost no system call and the host's
   oversleeps don't add up over a long readback.  SIM_PORT_RD_NS=<ns>
   overrides (0: the old free port, for A/B only: more permissive than
   silicon). */
static uint64_t mono_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static void port_charge(void)
{
    static long rd_ns = -1;
    static uint64_t owed_ns, paid_to;

    if (rd_ns < 0)
    {
        const char *e = getenv("SIM_PORT_RD_NS");

        rd_ns = e ? atol(e) : VBM_RD_NS;
        if (rd_ns < 0)
            rd_ns = 0;
    }
    owed_ns += (uint64_t)rd_ns;
    if (owed_ns >= 1000000)
    {
        uint64_t now = mono_ns(), to;

        /* reads in a run (the last payment due less than 50 ms ago) owe
           from where it was due, so an oversleep counts toward the next */
        to = (paid_to <= now && now - paid_to < 50000000u ? paid_to : now)
             + owed_ns;
        owed_ns = 0;
        paid_to = to;
        if (to > now)
        {
            struct timespec ts;

            ts.tv_sec = (time_t)((to - now) / 1000000000u);
            ts.tv_nsec = (long)((to - now) % 1000000000u);
            nanosleep(&ts, NULL);
        }
    }
}

int bcm_video_rd(uint32_t addr, uint32_t *val)
{
    port_charge();
    return vbm_rd(&model, addr, val);
}

int bcm_video_wr(uint32_t addr, uint32_t val)
{
    return vbm_wr(&model, addr, val);
}

static struct bcm_video_port_load load;

void bcm_video_set_dma(bool on)
{
    (void)on;
}

void bcm_video_set_dma_e2(bool on)
{
    (void)on;
}

void bcm_video_port_load(struct bcm_video_port_load *out, bool reset)
{
    *out = load;
    if (reset)
        memset(&load, 0, sizeof(load));
}

void bcm_video_set_dma_fixup(bool on)
{
    (void)on;
}

int bcm_video_flush(void)
{
    return 0;
}

int bcm_video_wr_block(uint32_t addr, const uint16_t *src, uint32_t n)
{
    load.bytes += 2 * n;
    load.cpu_blocks++;
    return vbm_wr_block(&model, addr, src, n);
}

/* the model's port never stalls */
void bcm_video_port_stats(uint32_t out[8], bool reset)
{
    (void)reset;
    memset(out, 0, 8 * sizeof(uint32_t));
}

void sim_video_vcheck_faults(uint32_t vclk_fail_mask, uint32_t pic_bad,
                             uint32_t decode_us)
{
    model.vclk_fail_mask = vclk_fail_mask;
    model.pic_bad = pic_bad;
    model.decode_us = decode_us ? decode_us : 11000;    /* vbm_init's */
}

void sim_video_vcheck_nopic(bool on)
{
    model.no_pic = on;
}

uint32_t sim_video_frames(void)
{
    return frames;
}

uint32_t sim_video_errors(void)
{
    return errors + model.port_misuse + model.fifo_errors;
}

extern const char *sim_shot_dir;   /* --shotdir, as the shot command */

bool sim_video_shot(const char *name)
{
    const char *dir = sim_shot_dir;
    char path[1024];
    uint8_t hdr[54] = { 'B', 'M' };
    uint32_t row = LCD_WIDTH * 3, size = 54 + row * LCD_HEIGHT;
    FILE *f;
    int x, y;

    if (!dir || !dir[0])
        return false;
    snprintf(path, sizeof(path), "%s/%s.bmp", dir, name);
    f = fopen(path, "wb");
    if (!f)
        return false;
#define LE32(o, v) (hdr[o] = (uint8_t)(v), hdr[o + 1] = (uint8_t)((v) >> 8), \
                    hdr[o + 2] = (uint8_t)((v) >> 16), \
                    hdr[o + 3] = (uint8_t)((v) >> 24))
    LE32(2, size);
    LE32(10, 54);
    LE32(14, 40);
    LE32(18, LCD_WIDTH);
    LE32(22, LCD_HEIGHT);
    hdr[26] = 1;
    hdr[28] = 24;
    LE32(34, row * LCD_HEIGHT);
#undef LE32
    fwrite(hdr, 1, sizeof(hdr), f);
    for (y = LCD_HEIGHT - 1; y >= 0; y--)
        for (x = 0; x < LCD_WIDTH; x++)
        {
            fb_data p = panel[y * LCD_WIDTH + x];
            uint8_t bgr[3] = { (p & 31) << 3 | (p & 31) >> 2,
                               ((p >> 5) & 63) << 2 | ((p >> 5) & 63) >> 4,
                               (p >> 11) << 3 | (p >> 11) >> 2 };

            fwrite(bgr, 1, 3, f);
        }
    fclose(f);
    printf("sim_script: shot %s\n", path);
    fflush(stdout);
    return true;
}
#endif /* HAVE_BCMFW_VIDEO */
