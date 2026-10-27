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

/* The first-use video check (apps/video/vcheck.h, vcheck_store.h,
 * vclock_ladder.h; docs/research/bcm2722/first-use-check.md):
 *   - the token: format and parse, another image, corrupt files, and the
 *     store's atomic replace (a cut left at each step reads as the old
 *     token or the new one);
 *   - the ladder: the next band down, data-driven;
 *   - the stream file: the committed one parses, damaged ones don't;
 *   - the check against the BCM model with our own decoder behind it
 *     (bcmfw/h264): every picture's sum equals vcheck_stream.h's table
 *     (computed from ffmpeg's decode: two decoders agree), and a pass at
 *     150 MHz; a top band whose switch falls back passes at the next; a
 *     unit that decodes wrong fails every band; a dead image is rebooted
 *     and the next band checked; words not where they should be are
 *     inconclusive;
 *   - live invalidation's names;
 *   - the time: the check's length with the device's read cost and the
 *     decoder's fitted FEED time (decoder.md) in the model.
 * Usage: test_vcheck bcmfw/video/vcheck.bin <scratch dir> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "vcheck.h"
#include "vcheck_store.h"
#include "vclock_ladder.h"
#include "vcheck_stream.h"
#include "vclk.h"
#include "vbcm_model.h"
#include "../../bcmfw/hostproto.h"
#include "../../bcmfw/h264/h264.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", \
    __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- the model and the port, on a fake clock ---- */

static struct vbm M;
static uint8_t *sram, *sdram, *mscratch, *arena;
static size_t arena_size;
static struct h264_dec *dec;
static uint32_t now, rd_ns = 2000, rd_acc, reads;
static uint32_t now_fn(void) { return now; }
static int p_rd(uint32_t a, uint32_t *v)
{
    rd_acc += rd_ns;
    now += rd_acc / 1000;
    rd_acc %= 1000;
    reads++;
    return vbm_rd(&M, a, v);
}
static int p_wr(uint32_t a, uint32_t v) { now += 2; return vbm_wr(&M, a, v); }
static int p_wrb(uint32_t a, const uint16_t *s, uint32_t n)
{
    now += 1 + n / 16;
    return vbm_wr_block(&M, a, s, n);
}
static void p_yield(void) { now += 100; vbm_run(&M); }
static const struct bvp_port port =
{
    .rd = p_rd, .wr = p_wr, .wr_block = p_wrb, .now_us = now_fn,
    .yield = p_yield,
};

/* the image's present: our decoder, the picture laid out as video.c */
static void show(const struct h264_picture *p)
{
    vbm_picture(&M, p->y, p->u, p->v, p->stride_y, p->stride_c, p->width,
                p->height);
}

static void present(void *ctx, const uint8_t *pkt, uint32_t len,
                    int64_t pts_us, uint32_t flags)
{
    int pos = 0, nal, next, nlen, r;

    (void)ctx;
    (void)pts_us;
    (void)flags;
    while (h264_annexb_next(pkt, (int)len, pos, &nal, &nlen, &next))
    {
        do
        {
            r = h264_decode_nal(dec, (uint8_t *)pkt + nal, nlen);
            if (r == H264_FRAME || r == H264_FRAME_AGAIN)
                show(h264_frame(dec));
        } while (r == H264_FRAME_AGAIN);
        pos = next;
    }
    if (h264_flush(dec) == H264_FRAME)
        show(h264_frame(dec));
}

static struct bvp bvp;
static int boots;

static int boot(void)
{
    struct vbm k = M;           /* the knobs survive the power cycle */

    vbm_init(&M, sram, sdram, mscratch, now_fn);
    M.present = present;
    M.vclk_fail_mask = k.vclk_fail_mask;
    M.pic_bad = k.pic_bad;
    M.pic_addr_skew = k.pic_addr_skew;
    M.vclk = k.vclk;
    M.hang_fast = k.hang_fast;
    M.land_us = k.land_us;
    M.land_bytes = k.land_bytes;
    M.no_pic = k.no_pic;
    if (k.decode_us)
    {
        M.decode_us = k.decode_us;
        M.panel_us = k.panel_us;
    }
    vbm_boot(&M);
    boots++;
    dec = h264_open(arena, arena_size);
    if (bvp_attach(&bvp, &port) || bvp_hello(&bvp) != HP_OK)
        return -1;
    return 0;
}

static int re_boot(void *ctx)
{
    (void)ctx;
    vbm_cut(&M);
    return boot();
}

static char lastlog[200];
static void log_line(void *ctx, const char *line)
{
    (void)ctx;
    snprintf(lastlog, sizeof(lastlog), "%s", line);
    printf("     %s\n", line);
}

static uint16_t scratch[512] __attribute__((aligned(4)));
static uint16_t screen[VCHECK_PANEL_W * VCHECK_PANEL_H];

static int run(const struct vcheck_stream *s, struct vcheck_result *r,
               const uint8_t *ladder, int n)
{
    struct vcheck_io io =
    {
        .port = &port, .bvp = &bvp, .reboot = re_boot, .log = log_line,
        .screen = screen, .pitch = VCHECK_PANEL_W, .scratch = scratch,
        .sums = vcheck_sums,
    };

    if (boot())
        return -100;
    return vcheck_run(&io, s, ladder, n, r);
}

static void reset_knobs(void)
{
    M.vclk_fail_mask = M.pic_bad = M.pic_addr_skew = 0;
    M.vclk = true;
    M.hang_fast = false;
    M.land_us = VBM_LAND_US;
    M.land_bytes = VBM_LAND_BYTES;
    M.no_pic = false;
    M.decode_us = 11000;        /* vbm_init's */
    M.panel_us = 6000;
}

/* ---- the token ---- */

static void t_token(void)
{
    struct vcheck_token t, u;
    char buf[VCHECK_TOKEN_MAX + 1];
    int n;
    static const char *const bad[] =
    {
        "",
        "vcheck 2\nimage 2c6dc03a\nband 11\n",
        "vcheck 1\nimage 2c6dc03\nband 11\n",          /* 7 digits */
        "vcheck 1\nimage 2c6dc03a\nband 12\n",
        "vcheck 1\nimage 2c6dc03a\nband 0\n",
        "vcheck 1\nimage 2c6dc03a\n",                   /* cut */
        "vcheck 1\nband 11\n",
        "vcheck 1\nimage 00000000\nband 11\n",
        "vcheck 1\nimage 2c6dc03a\nband 11x\n",
        "vcheck 1\nimage 2c6dc03a\nband 11\nwhat 1\n",
        "garbage",
    };
    unsigned k;

    memset(&t, 0, sizeof(t));
    t.image_fnv = 0x2c6dc03a;
    t.band = 11;
    strcpy(t.build, "0.8.0-abc123");
    strcpy(t.date, "2026-10-26");
    n = vcheck_token_format(&t, buf, sizeof(buf));
    CHECK(n > 0, "format %d", n);
    CHECK(vcheck_token_parse(buf, n, &u) == 0 && u.image_fnv == t.image_fnv
          && u.band == 11 && !strcmp(u.build, t.build) &&
          !strcmp(u.date, t.date), "round trip: %s", buf);
    CHECK(vcheck_unit_band(&u, 0x2c6dc03a) == 11, "own image");
    CHECK(vcheck_unit_band(&u, 0x80bc8611) == -1, "another image: due");
    CHECK(vcheck_unit_band(NULL, 0x2c6dc03a) == -1, "no token: due");
    u.band = 9;
    CHECK(vcheck_unit_band(&u, 0x2c6dc03a) == 9, "a lower band");
    CHECK(vcheck_token_parse("vcheck 1\r\nimage 2C6DC03A\r\nband 10\r\n",
                             36, &u) == 0 && u.band == 10, "CRLF, caps");
    for (k = 0; k < sizeof(bad) / sizeof(bad[0]); k++)
        CHECK(vcheck_token_parse(bad[k], strlen(bad[k]), &u) < 0,
              "corrupt token %u accepted", k);
    memset(buf, 'x', sizeof(buf));
    CHECK(vcheck_token_parse(buf, sizeof(buf), &u) < 0, "oversize");
    CHECK(vcheck_token_format(&t, buf, 10) < 0, "format into too little");
}

static void put_file(const char *p, const char *text)
{
    FILE *f = fopen(p, "wb");

    if (f)
    {
        fputs(text, f);
        fclose(f);
    }
}

static bool exists(const char *p)
{
    return access(p, F_OK) == 0;
}

static void t_store(const char *dir)
{
    char path[512], tmp[512], prev[512];
    struct vcheck_files f = { path, tmp, prev };
    struct vcheck_token t, u;

    snprintf(path, sizeof(path), "%s/vcheck.tok", dir);
    snprintf(tmp, sizeof(tmp), "%s/vcheck.tmp", dir);
    snprintf(prev, sizeof(prev), "%s/vcheck.old", dir);
    remove(path);
    remove(tmp);
    remove(prev);
    CHECK(vcheck_store_read(&f, &u) == VCHECK_STORE_NONE, "none at first");
    memset(&t, 0, sizeof(t));
    t.image_fnv = 0x2c6dc03a;
    t.band = 11;
    strcpy(t.build, "b1");
    strcpy(t.date, "2026-10-26");
    CHECK(vcheck_store_write(&f, &t) == 0, "write");
    CHECK(!exists(tmp) && !exists(prev), "no leftovers");
    CHECK(vcheck_store_read(&f, &u) == 0 && u.band == 11, "read back");
    t.band = 10;
    CHECK(vcheck_store_write(&f, &t) == 0 &&
          vcheck_store_read(&f, &u) == 0 && u.band == 10, "replaced");
    /* a cut after the old token went to prev, before tmp went in: the old
       one comes back */
    rename(path, prev);
    put_file(tmp, "vcheck 1\nimage 2c6dc03a\nband 9\n");
    CHECK(vcheck_store_read(&f, &u) == 0 && u.band == 10 && !exists(tmp) &&
          !exists(prev), "cut between 3 and 4: the old token");
    /* a cut after tmp went in, before prev went: the new one, prev goes */
    put_file(prev, "vcheck 1\nimage 2c6dc03a\nband 8\n");
    CHECK(vcheck_store_read(&f, &u) == 0 && u.band == 10 && !exists(prev),
          "cut between 4 and 5: the new token");
    /* a half-written token (a cut while tmp was written never reaches
       path; a damaged path reads as corrupt) */
    put_file(path, "vcheck 1\nimage 2c6d");
    CHECK(vcheck_store_read(&f, &u) == VCHECK_STORE_CORRUPT, "corrupt");
    put_file(path, "vcheck 1\nimage 80bc8611\nband 11\n");
    CHECK(vcheck_store_read(&f, &u) == 0 &&
          vcheck_unit_band(&u, 0x2c6dc03a) == -1, "another image's: due");
    /* invalidation: gone, and a prev left from a cut doesn't bring it
       back */
    put_file(prev, "vcheck 1\nimage 2c6dc03a\nband 11\n");
    CHECK(vcheck_store_drop(&f) == 1 && !exists(path) && !exists(prev),
          "dropped");
    CHECK(vcheck_store_read(&f, &u) == VCHECK_STORE_NONE, "stays gone");
    CHECK(vcheck_store_drop(&f) == 0, "drop of none");
}

static void t_ladder(void)
{
    static const uint8_t l2[] = { 11, 7, 3 };

    CHECK(VCLOCK_LADDER_N >= 1 && vclock_ladder[0] == VCLK_TOP,
          "the ladder starts at the top band");
    CHECK(vcheck_ladder_next(vclock_ladder, VCLOCK_LADDER_N, -1) == VCLK_TOP,
          "first");
    CHECK(vcheck_ladder_next(vclock_ladder, VCLOCK_LADDER_N, 11) == 10,
          "11 -> 10");
    CHECK(vcheck_ladder_next(l2, 3, 11) == 7 && vcheck_ladder_next(l2, 3, 7)
          == 3 && vcheck_ladder_next(l2, 3, 3) == -1, "data-driven");
    CHECK(vcheck_ladder_next(l2, 3, 9) == 7, "from a band not on it");
    CHECK(vcheck_ladder_next(l2, 0, -1) == -1, "empty");
    {
        int k;

        for (k = 1; k < VCLOCK_LADDER_N; k++)
            CHECK(vclock_ladder[k] < vclock_ladder[k - 1] &&
                  vclock_ladder[k] > 0, "descending, never the crystal");
    }
    printf("info ladder pending vbv: %d, %d rows\n", VCLOCK_LADDER_PENDING,
           VCLOCK_LADDER_N);
}

static void t_names(void)
{
    CHECK(!strcmp(vcheck_bad_name[VCHECK_BAD_CLOCK], "clock-fallback") &&
          !strcmp(vcheck_why_name[VCHECK_W_SUM], "sum"), "names");
}

/* ---- the check against the model ---- */

static uint8_t *load(const char *p, uint32_t *n)
{
    FILE *f = fopen(p, "rb");
    uint8_t *b;
    long len;

    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc(len);
    if (b && fread(b, 1, len, f) != (size_t)len)
    {
        free(b);
        b = NULL;
    }
    fclose(f);
    *n = (uint32_t)len;
    return b;
}

static void t_stream(const uint8_t *buf, uint32_t n, struct vcheck_stream *s)
{
    uint8_t *bad = malloc(n);

    CHECK(n == VCHECK_FILE_BYTES && vcheck_fnv_bytes(buf, n) ==
          VCHECK_FILE_FNV, "the committed stream matches vcheck_stream.h");
    CHECK(vcheck_stream_parse(s, buf, n) == 0 && s->pics == VCHECK_PICS &&
          s->w == VCHECK_WIDTH && s->h == VCHECK_HEIGHT, "parse");
    CHECK(vcheck_stream_parse(s, buf, n - 4) < 0, "truncated");
    memcpy(bad, buf, n);
    bad[0] ^= 1;
    CHECK(vcheck_stream_parse(s, bad, n) < 0, "bad magic");
    memcpy(bad, buf, n);
    bad[16] = 0xFF;
    bad[17] = 0xFF;
    CHECK(vcheck_stream_parse(s, bad, n) < 0, "a length past the end");
    free(bad);
    vcheck_stream_parse(s, buf, n);
}

static void t_check(const struct vcheck_stream *s)
{
    struct vcheck_result r;
    int v, k;

    /* a good unit: 150 MHz, every sum (our decoder) as the table
       (ffmpeg's) */
    reset_knobs();
    v = run(s, &r, vclock_ladder, VCLOCK_LADDER_N);
    CHECK(v == VCHECK_PASS && r.band == VCLK_TOP && r.n == 1 &&
          r.tried[0].pics == VCHECK_PICS && r.tried[0].sd_words ==
          VCHECK_SDC_BLOCKS * VCHECK_SDC_READS + 2 * VCHECK_PICS &&
          !r.tried[0].sd_bad && M.stale_feeds == 0 &&
          r.tried[0].mhz == 150, "good unit: %s", lastlog);
    CHECK(!bvp.session && M.closes == M.opens, "the session closed");
    CHECK(M.overlays >= 1 && M.ovl_wh == (240u << 16 | 320),
          "the frame over the picture");

    /* the top band falls back (its speed check): the next band passes */
    reset_knobs();
    M.vclk_fail_mask = 1u << 11;
    v = run(s, &r, vclock_ladder, VCLOCK_LADDER_N);
    CHECK(v == VCHECK_PASS && r.band == 10 && r.n == 2 &&
          r.tried[0].why == VCHECK_W_FALLBACK, "150 falls back: %s", lastlog);

    /* two bands fall back */
    reset_knobs();
    M.vclk_fail_mask = 1u << 11 | 1u << 10;
    v = run(s, &r, vclock_ladder, VCLOCK_LADDER_N);
    CHECK(v == VCHECK_PASS && r.band == 9, "two fall back: %s", lastlog);

    /* a unit that decodes wrong: every band fails on a sum */
    reset_knobs();
    M.pic_bad = 6;
    v = run(s, &r, vclock_ladder, VCLOCK_LADDER_N);
    CHECK(v == VCHECK_FAIL && r.band == -1 && r.n == VCLOCK_LADDER_N,
          "decodes wrong: %s", lastlog);
    for (k = 0; k < r.n; k++)
        CHECK(r.tried[k].why == VCHECK_W_SUM && r.tried[k].pic == 5 &&
              r.tried[k].pics == 5, "band %d: why %d pic %d", k,
              r.tried[k].why, r.tried[k].pic);

    /* the image dies at 150 MHz: rebooted, the next band passes */
    reset_knobs();
    M.hang_fast = true;
    boots = 0;
    v = run(s, &r, vclock_ladder, VCLOCK_LADDER_N);
    CHECK(v == VCHECK_PASS && r.band == 10 && r.tried[0].why ==
          VCHECK_W_CHIP && boots == 2, "dead at 150: %s (boots %d)",
          lastlog, boots);
    M.hang_fast = false;

    /* the words not where they should be: inconclusive, not the unit */
    reset_knobs();
    M.pic_addr_skew = 4;
    v = run(s, &r, vclock_ladder, VCLOCK_LADDER_N);
    CHECK(v == VCHECK_INCONCLUSIVE && r.n == 1 && r.tried[0].why ==
          VCHECK_W_ADDR, "skewed address: %s", lastlog);

    /* an image without HP_OP_VCLK: the top band by OPEN alone */
    reset_knobs();
    M.vclk = false;
    v = run(s, &r, vclock_ladder, VCLOCK_LADDER_N);
    CHECK(v == VCHECK_PASS && r.band == VCLK_TOP, "no VCLK: %s", lastlog);
}

/* Sitting 4b (2026-10-26): the check's first build sent each FEED the
   moment its packet was written, then asked STATUS back to back; on the
   chip every band's first picture never came (vck 150 FAIL chip p0 rc-2,
   about 435 ms: the image answered, took the packet, decoded nothing).
   The same sequence against the real image in vc2sim with the host's
   SDRAM writes landing behind a read or 2 ms later gives the same line
   (vbcm_model.h VBM_LAND_US).  Here: that sequence, replayed against the
   model, gets no picture and times out; the check as built now passes
   even when the writes land only behind a read; and a packet taken with
   no picture is inconclusive at once, with the image's words logged. */
static int old_feed(const struct vcheck_stream *s, uint32_t k)
{
    uint32_t len, n = 0, t0, prog, r[BVP_REPLY_WORDS];
    const uint8_t *pkt = vcheck_stream_pkt(s, k, &len);

    if (bvp_pkt_begin(&bvp, len) || bvp_pkt_write(&bvp, pkt, len) < 0)
        return -10;
    bvp_pkt_end(&bvp);
    {
        /* bvp_feed as it was then: the FEED record straight after the
           writes (bvp_feed now reads the newest packet back first) */
        struct bvp_pkt *p = &bvp.pkt[(bvp.head + bvp.in_flight) %
                                     BVP_MAX_PKTS];
        uint32_t a[6] = { bvp.session, p->off, p->len,
                          (uint32_t)(k * s->frame_us), 0,
                          k ? 0 : HP_FEED_KEY };

        if (bvp_send(&bvp, HP_OP_FEED, a, 6, NULL))
            return -11;
        p->posted = true;
        bvp.in_flight++;
    }
    t0 = now;
    for (;;)
    {
        if (bvp_call(&bvp, HP_OP_STATUS, &bvp.session, 1, r, BVP_REPLY_WORDS,
                     &n, VCHECK_CALL_US) != HP_OK)
            return -12;
        if (n >= 16 && r[6] >= k + 1 && p_rd(VCHECK_MB_PROG, &prog) == 0 &&
            prog >> 24 == VCHECK_PH_IDLE)
            return 0;
        if (now - t0 > VCHECK_PIC_WAIT_US)
            return BVP_E_TIMEOUT;
        p_yield();
    }
}

static void t_order(const struct vcheck_stream *s)
{
    struct vcheck_result r;
    int v, rc;
    uint32_t t0;

    /* the old sequence: no picture, the packet taken, a timeout */
    reset_knobs();
    CHECK(boot() == 0 && bvp_open(&bvp, s->w, s->h, HP_MODE_FIT |
          HP_OPEN_OWN_DISPLAY | HP_OPEN_FAST_CLOCK) == HP_OK, "open");
    rc = old_feed(s, 0);
    bvp_refresh(&bvp);
    CHECK(rc == BVP_E_TIMEOUT && M.decoded == 0 && M.stale_feeds == 1 &&
          bvp.in_flight == 0, "the old sequence: rc %d, decoded %lu, stale "
          "%lu, in flight %lu", rc, (unsigned long)M.decoded,
          (unsigned long)M.stale_feeds, (unsigned long)bvp.in_flight);
    bvp_close(&bvp, 0);

    /* the same packet through bvp_feed, which reads the newest packet's
       last word back before its FEED, writes landing only behind a
       read: the picture comes */
    reset_knobs();
    M.land_us = M.land_bytes = 0xFFFFFFFFu;
    CHECK(boot() == 0 && bvp_open(&bvp, s->w, s->h, HP_MODE_FIT |
          HP_OPEN_OWN_DISPLAY) == HP_OK, "open");
    {
        uint32_t len;
        const uint8_t *pkt = vcheck_stream_pkt(s, 0, &len);

        CHECK(!bvp_pkt_begin(&bvp, len) && bvp_pkt_write(&bvp, pkt, len) == 0,
              "write");
        bvp_pkt_end(&bvp);
        CHECK(!bvp_feed(&bvp, 0, HP_FEED_KEY), "feed");
        t0 = now;
        while (!M.decoded && now - t0 < VCHECK_PIC_WAIT_US)
            p_yield();
        CHECK(M.decoded == 1 && M.stale_feeds == 0, "read back first: "
              "decoded %lu", (unsigned long)M.decoded);
    }
    bvp_close(&bvp, 0);

    /* writes that land only behind a read: the check still passes */
    reset_knobs();
    M.land_us = M.land_bytes = 0xFFFFFFFFu;
    v = run(s, &r, vclock_ladder, VCLOCK_LADDER_N);
    CHECK(v == VCHECK_PASS && r.band == VCLK_TOP && r.n == 1 &&
          M.stale_feeds == 0, "writes land at a read: %s", lastlog);

    /* the image takes every packet and gives no picture: inconclusive
       at the first, well inside the picture's wait, the words logged */
    reset_knobs();
    M.no_pic = true;
    t0 = now;
    v = run(s, &r, vclock_ladder, VCLOCK_LADDER_N);
    CHECK(v == VCHECK_INCONCLUSIVE && r.n == 1 && r.tried[0].why ==
          VCHECK_W_NOPIC && r.tried[0].pic == 0 && r.tried[0].diag_n &&
          r.tried[0].diag[0] == 0 && r.tried[0].us < VCHECK_PIC_WAIT_US / 2,
          "no picture: %s (%lu us)", lastlog,
          (unsigned long)r.tried[0].us);
    {
        char line[80];

        CHECK(vcheck_band_line(&r.tried[0], 2, line, sizeof(line)) > 0 &&
              strlen(line) <= 55 && !strncmp(line, "vck x d0 q0 p1 ph0f", 19),
              "the words' line: %s", line);
        CHECK(vcheck_band_line(&r.tried[0], 3, line, sizeof(line)) > 0 &&
              strlen(line) <= 55, "the guard's line: %s", line);
    }
    M.no_pic = false;
}

/* the time on the device: the model's decode at the fitted FEED time and
   the host port's reads at the old hypothesis's 0.5 and 1 us a word
   (data-paths.md R2), then at silicon's (VBM_RD_NS, sitting 4c: the
   whole-plane check, 864k reads, took 7993 ms with about 0.2 s of decode
   and 66 ms of OPEN, overlay and readback; the simulator then charged
   nothing and measured 887 ms; sim-fidelity.md "Device surprises" 2);
   the sampled check must stay near 1 s there (VCHECK_SAMPLE_MAX_MS) */
#define VCHECK_SAMPLE_MAX_MS    1000
static void t_time(const struct vcheck_stream *s)
{
    struct vcheck_result r;
    static const uint32_t cost[3] = { 500, 1000, VBM_RD_NS };   /* ns */
    int k;

    for (k = 0; k < 3; k++)
    {
        reset_knobs();
        rd_ns = cost[k];
        reads = 0;
        /* FEED = 1.3 ms + cycles / 125.7 per us (decoder.md); the
           stream's mean by vclk.h's model: 9 cycles a pixel, 94 a bit */
        {
            uint32_t bits = (VCHECK_FILE_BYTES * 8) / VCHECK_PICS;
            uint32_t cyc = 320 * 240 * VCLK_CYC_PER_PX + bits * VCLK_CYC_PER_BIT;

            M.decode_us = 1300 + cyc * 10 / 1257;
            M.panel_us = 0;
            /* sitting 4c's own: w6226/23855 (mean of the P pictures, the
               key picture), 6814 us a picture over the stream */
            if (cost[k] == 8900)
                M.decode_us = 6814;
        }
        {
            uint32_t t0 = now;

            run(s, &r, vclock_ladder, VCLOCK_LADDER_N);
            printf("info check time (model, FEED %lu us, %lu ns a read, "
                   "%lu reads): %lu ms\n", (unsigned long)M.decode_us,
                   (unsigned long)rd_ns, (unsigned long)reads,
                   (unsigned long)((now - t0) / 1000));
            /* the sample (2400 words a picture) and the polls, never
               the whole planes (28800 a picture) */
            CHECK(r.verdict == VCHECK_PASS && reads < VCHECK_PICS *
                  (VCHECK_MB_ROWS(VCHECK_HEIGHT) * VCHECK_WIDTH / 4 * 2 +
                   600), "the sampled reads: %lu", (unsigned long)reads);
            if (rd_ns == VBM_RD_NS)
                CHECK((now - t0) / 1000 <= VCHECK_SAMPLE_MAX_MS,
                      "the check at silicon's read cost: %lu ms",
                      (unsigned long)((now - t0) / 1000));
        }
    }
    rd_ns = 2000;
}

int main(int argc, char **argv)
{
    struct vcheck_stream s;
    uint8_t *buf;
    uint32_t n = 0;

    if (argc < 3)
    {
        fprintf(stderr, "usage: %s vcheck.bin scratch-dir\n", argv[0]);
        return 2;
    }
    sram = calloc(1, VBM_SRAM_SIZE);
    sdram = calloc(1, VBM_SDRAM_SIZE);
    mscratch = calloc(1, VBM_SCRATCH);
    arena_size = h264_arena_size(20, 15, 16);
    arena = malloc(arena_size);
    buf = load(argv[1], &n);
    if (!sram || !sdram || !mscratch || !arena || !buf)
    {
        printf("FAIL test_vcheck: no memory or no %s\n", argv[1]);
        return 1;
    }
    t_token();
    t_store(argv[2]);
    t_ladder();
    t_names();
    t_stream(buf, n, &s);
    t_check(&s);
    t_order(&s);
    t_time(&s);
    printf("test_vcheck: %s (%d failures)\n", fails ? "FAIL" : "ok", fails);
    return fails ? 1 : 0;
}
