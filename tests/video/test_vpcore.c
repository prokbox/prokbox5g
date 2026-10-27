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

/* Host test of the video player's core (apps/video/vpcore.c) with the VP
   client (bcmvp.c) and the engine (vplay.c), against the model of our
   image (vbcm_model.c) on real corpus files: their .vix is built here
   with vix_write(), and every picture the model shows is checked byte for
   byte against the reference Annex B (mp4_annexb(), with the parameter
   sets in front of the first packet after every OPEN).  The kernel tick
   is 10 ms as on the PP; the clock is simulated.

     test_vpcore FILE.mp4...   (the scenarios run on the first file; the
                               "real" scenario plays every file)

   Scenarios: normal play to the end, pause, a seek, slow decoding (GOP
   drops: session restarts), 2.5x (key frames only), the display going
   dark and waking, the buffer behind the video, the image dying once
   (reloaded) and twice (video stops, the BCM given back), and images
   that offer no protocol or another major (video never starts). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "video/vpcore.h"
#include "video/vcheck.h"
#include "video/vbcm_model.h"
#include "video/mp4demux.h"
#include "video/vix.h"
#include "../../bcmfw/hostproto.h"

#define ARRAYLEN(a) (sizeof(a) / sizeof((a)[0]))

#define TICK_US     10000
#define MAXS        4096

static int fails;

static void check(int cond, const char *what)
{
    printf("%s %s\n", cond ? "ok  " : "FAIL", what);
    fails += !cond;
}

/* ---- the file, its index, the reference packets ---- */

struct file
{
    uint8_t *data;
    long size;
    uint8_t *vix;
    long vix_size;
    uint32_t n;
    int64_t pts[MAXS];
    uint32_t crc[MAXS], len[MAXS];
    uint64_t off[MAXS];         /* the sample's bytes in the file */
    uint32_t raw[MAXS];
    bool key[MAXS];
    uint8_t params[512];
    long params_len;
    int64_t frame_us;
};

static struct file F;

static uint32_t crc32(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0xFFFFFFFF, i;
    int k;

    for (i = 0; i < n; i++)
    {
        c ^= p[i];
        for (k = 0; k < 8; k++)
            c = c >> 1 ^ (0xEDB88320 & -(c & 1));
    }
    return ~c;
}

static long mem_read(void *ctx, uint64_t off, void *buf, unsigned long len)
{
    struct file *f = ctx;

    if (off >= (uint64_t)f->size)
        return 0;
    if (off + len > (uint64_t)f->size)
        len = f->size - off;
    memcpy(buf, f->data + off, len);
    return (long)len;
}

static uint8_t *vix_out;
static long vix_len, vix_cap;

static int vix_put(void *ctx, const void *buf, unsigned long len)
{
    (void)ctx;
    if (vix_len + (long)len > vix_cap)
    {
        vix_cap = 2 * (vix_len + len);
        vix_out = realloc(vix_out, vix_cap);
    }
    memcpy(vix_out + vix_len, buf, len);
    vix_len += len;
    return 0;
}

static int load(const char *path)
{
    FILE *fp = fopen(path, "rb");
    struct mp4_io io;
    struct mp4_movie m;
    struct mp4_iter it;
    struct mp4_sample s;
    static uint8_t scratch[8192], pkt[1 << 20];
    int rc;

    memset(&F, 0, sizeof(F));
    if (!fp)
        return -1;
    fseek(fp, 0, SEEK_END);
    F.size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    F.data = malloc(F.size);
    if (fread(F.data, 1, F.size, fp) != (size_t)F.size)
        return -1;
    fclose(fp);
    io.read_at = mem_read;
    io.ctx = &F;
    io.size = F.size;
    if (mp4_open(&m, &io) < 0)
        return -1;
    vix_out = NULL;
    vix_len = vix_cap = 0;
    if (vix_write(&m, scratch, sizeof(scratch), vix_put, NULL) < 0)
        return -1;
    F.vix = vix_out;
    F.vix_size = vix_len;
    F.params_len = mp4_params(&m, F.params, sizeof(F.params));
    if (F.params_len <= 0 || mp4_iter_init(&it, &m, scratch, sizeof(scratch)))
        return -1;
    while ((rc = mp4_iter_next(&it, &s)) == 1 && F.n < MAXS)
    {
        long n;

        memcpy(pkt, F.data + s.offset, s.size);
        n = mp4_annexb(pkt, s.size, m.nal_length, pkt, sizeof(pkt));
        if (n < 0)
            return -1;
        F.pts[F.n] = s.pts_us;
        F.off[F.n] = s.offset;
        F.raw[F.n] = s.size;
        F.key[F.n] = s.key;
        F.len[F.n] = n;
        F.crc[F.n] = crc32(pkt, n);
        F.n++;
    }
    F.frame_us = F.n > 1 ? F.pts[1] - F.pts[0] : 33333;
    return rc < 0 ? -1 : 0;
}

/* ---- the world: clock, audio, buffer, model ---- */

static uint32_t now;            /* us */
static uint32_t now_fn(void) { return now; }

static struct vbm M;
static uint8_t *sram, *sdram, *mscratch;

struct world
{
    /* audio: a = a_base + (t - t_base) * speed / 100 while playing */
    int64_t t_base, a_base;
    int speed;
    bool playing, display_on;
    /* buffered window of the file, in seconds of audio ahead/behind */
    int64_t ahead_us;           /* < 0: the whole file */
    int64_t keep_us;            /* behind the audio, then reclaimed */
    uint32_t again, direct;
    /* dev42's faults: the audio's buffer reset between a sample's probe
       and its pieces (race_sample, once), and reads of a sample's bytes
       failing (bad_from: every sample from there; bad_left reads, < 0
       for ever) */
    int race_sample, bad_from, bad_left;
    bool race_armed;
    uint32_t races, bad_reads;

    /* what the model showed */
    uint32_t shown, hidden, bad_bytes, order_bad, params_missing;
    int64_t last_pts, err_max, settle_until;
    bool want_first;            /* the next shown picture's error: */
    int64_t first_err;          /* a seek's first picture */
    bool expect_params;         /* next packet after an OPEN */
    uint32_t opens_seen;
    uint32_t takes, releases;
    uint32_t handbacks, boot_skips;    /* p14-resident */
    bool verbose;
};

static struct world W;

static int64_t audio_now(void)
{
    if (!W.playing)
        return W.a_base;
    return W.a_base + ((int64_t)now - W.t_base) * W.speed / 100;
}

static void rebase(void)
{
    W.a_base = audio_now();
    W.t_base = now;
}

static int find_pts(int64_t pts)
{
    uint32_t i;

    /* the index keeps milliseconds */
    for (i = 0; i < F.n; i++)
        if (F.pts[i] - pts < 1000 && pts - F.pts[i] < 1000)
            return (int)i;
    return -1;
}

static void present(void *ctx, const uint8_t *pkt, uint32_t len,
                    int64_t pts_us, uint32_t flags)
{
    int k = find_pts(pts_us);
    int64_t err;

    (void)ctx;
    if (M.opens != W.opens_seen)
    {
        W.opens_seen = M.opens;
        W.expect_params = true;
    }
    if (W.expect_params)
    {
        W.expect_params = false;
        if (len < (uint32_t)F.params_len ||
            memcmp(pkt, F.params, F.params_len))
            W.params_missing++;
        else
        {
            pkt += F.params_len;
            len -= F.params_len;
        }
    }
    if (k < 0 || len != F.len[k] || crc32(pkt, len) != F.crc[k])
    {
        W.bad_bytes++;
    }
    if (k >= 0 && W.last_pts >= 0 && pts_us <= W.last_pts && M.shown > 1 &&
        !F.key[k])
        W.order_bad++;          /* backwards without a restart at a key */
    W.last_pts = pts_us;
    if (flags & HP_FEED_HIDE)
    {
        W.hidden++;             /* a seek's catch-up: decoded, not shown */
        return;
    }
    W.shown++;
    err = audio_now() - pts_us;
    if (W.want_first)
    {
        W.want_first = false;
        W.first_err = err;      /* > 0: late */
    }
    if (err < 0)
        err = -err;
    if ((int64_t)now >= W.settle_until && err > W.err_max)
        W.err_max = err;
}

/* the video file through the "audio buffer" */
static long src_read(void *ctx, uint64_t off, void *buf, uint32_t n)
{
    int64_t a = audio_now();
    long hi, lo;

    (void)ctx;
    if (W.race_sample >= 0)
    {
        uint64_t o = F.off[W.race_sample];

        if (n == 1 && off == o + F.raw[W.race_sample] - 1)
            W.race_armed = true;
        else if (W.race_armed && off >= o && off < o + F.raw[W.race_sample])
        {
            W.race_armed = false;
            W.race_sample = -1;
            W.races++;
            return VPC_NOT_BUFFERED;
        }
    }
    if (W.bad_from >= 0 && W.bad_left && off + n > F.off[W.bad_from])
    {
        if (W.bad_left > 0)
            W.bad_left--;
        W.bad_reads++;
        return -1;
    }
    if (W.ahead_us >= 0)
    {
        /* the buffer holds the file from (audio - keep) to (audio +
           ahead), proportionally to its length */
        int64_t dur = F.pts[F.n - 1] + F.frame_us;

        hi = (long)((a + W.ahead_us) * F.size / dur);
        lo = (long)((a - W.keep_us) * F.size / dur);
        if ((long)(off + n) > hi)
        {
            W.again++;
            return VPC_NOT_BUFFERED;
        }
        if ((long)off < lo)
            W.direct++;         /* reclaimed: read() reads the file */
    }
    return mem_read(&F, off, buf, n);
}

static long idx_read(void *ctx, uint64_t off, void *buf, uint32_t n)
{
    (void)ctx;
    if ((long)(off + n) > F.vix_size)
        return -1;
    memcpy(buf, F.vix + off, n);
    return n;
}

static int p_rd(uint32_t a, uint32_t *v) { now += 2; return vbm_rd(&M, a, v); }
static int p_wr(uint32_t a, uint32_t v) { now += 2; return vbm_wr(&M, a, v); }
static int p_wrb(uint32_t a, const uint16_t *s, uint32_t n)
{
    now += 1 + n / 16;          /* about 0.06 us a halfword */
    return vbm_wr_block(&M, a, s, n);
}
static void p_yield(void) { now += 100; vbm_run(&M); }

static const struct bvp_port port =
{
    .rd = p_rd, .wr = p_wr, .wr_block = p_wrb, .now_us = now_fn,
    .yield = p_yield,
};

static uint32_t boot_extra_us;  /* the device's boot is 300-600 ms */
static bool preroll;            /* the audio starts once vpc_start returns */
/* p14-resident: the image is the display driver's own (booted once, as
   the boot path does): a start with it running boots nothing, a stop
   after CLOSE hands the panel back with it running (bcm-devtest-hal.c's
   bcm_video_boot and bcm_video_hand_back).  Off: neither, as before. */
static bool resident;
static bool res_taken;          /* the take found the resident image up */
static int be_take(void)
{
    W.takes++;
    res_taken = resident && M.running;
    return 0;
}
static int be_boot(void)
{
    if (res_taken && M.running)
    {
        W.boot_skips++;
        now += 300;             /* one read of the command word */
        return 0;
    }
    vbm_boot(&M);
    now += 5000 + boot_extra_us;
    res_taken = resident;       /* the same bytes: it may stay at the stop */
    return 0;
}
static void be_cut(void) { vbm_cut(&M); res_taken = false; }
static void be_release(void) { W.releases++; res_taken = false; }
static int be_hand_back(void)
{
    if (!res_taken || !M.running || M.session)
        return -1;
    W.handbacks++;
    res_taken = false;
    return 0;
}
/* P19 PW4: the governor's lines, counted */
static uint32_t vclk_gov_n, vclk_sum_n, vclk_fail_n;
static char vclk_sum[160];
static void be_log(const char *what, long a, long b)
{
    if (!strncmp(what, "vclk band ", 10))
    {
        vclk_gov_n++;
        if (strstr(what, "why=fail"))
            vclk_fail_n++;
    }
    if (!strncmp(what, "vclk sum ", 9))
    {
        vclk_sum_n++;
        snprintf(vclk_sum, sizeof(vclk_sum), "%s", what);
    }
    if (W.verbose)
        printf("     [%u ms] %s (%ld, %ld)\n", now / 1000, what, a, b);
}

static uint32_t vmask;          /* the bands verified (0: no governor) */
static unsigned vpolicy;        /* the in-band policy */
static uint32_t be_vclk(unsigned *xtal_level, unsigned *policy)
{
    *xtal_level = 11;
    *policy = vpolicy;
    return vmask;
}

static uint32_t fast;            /* what the 4b/4c gate answered */
static uint32_t be_open_flags(void) { return fast; }

/* stage 6's trace (vtrace.c), when a scenario sets tr */
static struct vtrace *tr;
static void be_frame(const struct vpc_frame *f)
{
    if (tr)
        vtrace_add(tr, f);
}

static int trace_lines;
static void count_line(void *ctx, const char *text)
{
    (void)ctx;
    trace_lines++;
    if (getenv("VERBOSE") || strncmp(text, "V6 ", 3))
        printf("     %s\n", text);
}

static struct vpc V;            /* (the scenarios' core, below) */
/* the first-use check's hooks (vcheck.h): the check's calls and what it
   answers, and the live invalidations, with a step-down as vplayer.c's */
static int check_calls, check_rc, check_session, bad_n, bad_why, cap_rc;
static int be_check(struct bvp *b)
{
    check_calls++;
    check_session = (int)b->session;    /* none may be open */
    return check_rc;
}
static void be_unit_bad(int why, long a)
{
    (void)a;
    bad_n++;
    bad_why = why;
    if (why != VCHECK_BAD_FAULT)
        cap_rc = vpc_vclk_cap(&V, 10);
}

static const struct vpc_backend be =
{
    .port = &port, .take = be_take, .boot = be_boot, .cut = be_cut,
    .release = be_release, .log = be_log, .open_flags = be_open_flags,
    .frame = be_frame, .hand_back = be_hand_back, .vclk = be_vclk,
    .check = be_check, .unit_bad = be_unit_bad,
};

static const struct vpc_source src =
{
    .ctx = NULL, .read = src_read, .index_read = idx_read,
};

/* ---- running a scenario ---- */

static uint32_t tick_us = TICK_US;
static bool honor_wait;         /* C5: advance by the player's own wait */
static uint32_t ticks;
static struct vpc V;
static int step;                /* the scenario hook's progress */
static uint8_t vmem[VPC_MEM_SIZE + 8];
static struct vp_key keys[MAXS];
static struct vtrace trace;
static struct vtrace_rec trace_rec[4096];

typedef void (*hook_fn)(int64_t t_ms);

static int setup(void)
{
    long nk;

    now = 1000000;
    step = 0;
    memset(&W, 0, sizeof(W));
    W.speed = 100;
    W.playing = true;
    W.display_on = true;
    W.ahead_us = -1;
    W.keep_us = 3000000;
    W.last_pts = -1;
    W.t_base = now;
    W.race_sample = W.bad_from = -1;
    W.verbose = getenv("VERBOSE") != NULL;
    vbm_init(&M, sram, sdram, mscratch, now_fn);
    M.present = present;
    nk = vpc_init(&V, &src, &be, vmem, sizeof(vmem));
    if (nk <= 0 || vpc_keys(&V, keys, (uint32_t)nk))
        return -1;
    return 0;
}

/* Runs until the core is done, failed, or max_ms of simulated time;
   hook(t) runs before each tick. */
static enum vpc_state run(uint32_t max_ms, hook_fn hook)
{
    uint32_t t0 = now, wait = 0;
    enum vpc_state st;

    W.settle_until = now + 300000;      /* the first status corrects */
    st = vpc_start(&V, audio_now());
    if (preroll)
    {
        /* vplayer_start's pre-roll: the audio starts now, from 0 */
        W.t_base = now;
        W.settle_until = now + 300000;
    }
    while (st != VPC_FAILED && now - t0 < max_ms * 1000)
    {
        if (hook)
            hook((now - t0) / 1000);
        now += honor_wait && wait ? wait * 1000 : tick_us;
        vbm_run(&M);
        ticks++;
        st = vpc_tick(&V, audio_now(), W.playing, W.display_on, &wait);
        if (st == VPC_DONE && M.q_rd == M.q_wr)
            break;
    }
    return st;
}

static void report(const char *name)
{
    printf("     %s: %u/%u shown, A/V error after settling max %lld us; "
           "restarts %u, wakes %u, recoveries %u, opens %u, boots %u; "
           "%u statuses; buffer waits %u, direct reads %u; %u FIFO errors; "
           "%u host-port transfers (%u word reads); bad packets %u, "
           "missing parameter sets %u\n", name, W.shown, F.n,
           (long long)W.err_max, V.stats.restarts, V.stats.wakes,
           V.stats.recoveries, M.opens, M.boots, M.statuses, W.again,
           W.direct, M.fifo_errors, V.bvp.xfers, V.bvp.reads, W.bad_bytes,
           W.params_missing);
}

static int64_t frame(void) { return F.frame_us; }

/* hooks */
/* hooks: each step once, at the first tick at or after its time */
static void h_pause(int64_t t)
{
    if (step == 0 && t >= 3000) { step++; rebase(); W.playing = false; }
    if (step == 1 && t >= 4000) { step++; W.t_base = now; W.playing = true;
                                  W.settle_until = now + 300000; }
}

static void h_pause3(int64_t t)
{
    if (step == 0 && t >= 3000) { step++; rebase(); W.playing = false; }
    if (step == 1 && t >= 6000) { step++; W.t_base = now; W.playing = true;
                                  W.settle_until = now + 300000; }
}

/* the shell's scrub bar while paused (dev34): up, kept through a seek's
   restart, then away */
static uint16_t ovl_strip[320 * 24];
static uint32_t ovl_after_restart, ovl_bytes_ok, ovl_opens_before;

static void h_overlay(int64_t t)
{
    uint32_t k;

    if (step == 0 && t >= 2000)
    {
        step++;
        rebase();
        W.playing = false;
        for (k = 0; k < 320 * 24; k++)
            ovl_strip[k] = (uint16_t)(k * 2654435761u >> 16);
        vpc_overlay(&V, 0, 200, 320, 24, ovl_strip, 320);
    }
    if (step == 1 && t >= 2500)
    {
        step++;
        ovl_opens_before = M.opens;
        vpc_seek(&V, F.pts[F.n / 2]);   /* a scrub: the session restarts */
    }
    if (step == 2 && t >= 3000)
    {
        step++;
        ovl_after_restart = M.opens > ovl_opens_before ? M.ovl_wh : 0;
        ovl_bytes_ok = !memcmp(sram + 0xE0000 + 2 * 200 * 320, ovl_strip,
                               sizeof(ovl_strip));
        vpc_overlay(&V, 0, 0, 0, 0, NULL, 0);
    }
}

/* a pause after which the audio resumes 300 ms on (session 34: the PCM
   ran past the pictures): the player seeks instead of dropping a GOP */
static int64_t jump_us = 300000;

static void h_pause_jump(int64_t t)
{
    if (step == 0 && t >= 3000) { step++; rebase(); W.playing = false; }
    if (step == 1 && t >= 5000)
    {
        step++;
        W.a_base += jump_us;
        W.t_base = now;
        W.playing = true;
        W.settle_until = now + 300000;
    }
}

static void h_seek(int64_t t)
{
    if (step == 0 && t >= 2000)
    {
        step++;
        W.a_base = F.pts[3 * F.n / 4];
        W.t_base = now;
        W.settle_until = now + 300000;
        vpc_seek(&V, audio_now());
    }
}

/* to a picture well into a GOP: the key's way to it is hidden */
static uint32_t seek_mid;

/* session 36c, stage 7's step 7: a seek into a GOP (MENU and back, a
   resume past the pictures), then the audio's clock stands still for
   4 s though "playing" (the codec never started), then runs */
static void h_audio_stall(int64_t t)
{
    if (step == 0 && t >= 2000)
    {
        step++;
        W.a_base = F.pts[seek_mid];
        W.t_base = now;
        W.speed = 0;
        vpc_seek(&V, audio_now());
    }
    else if (step == 1 && t >= 6000)
    {
        step++;
        rebase();
        W.speed = 100;
        W.settle_until = now + 300000;
    }
}

static void h_seek_mid(int64_t t)
{
    if (step == 0 && t >= 2000)
    {
        step++;
        W.a_base = F.pts[seek_mid];
        W.t_base = now;
        W.settle_until = now + 300000;
        W.want_first = true;
        vpc_seek(&V, audio_now());
    }
}

static void h_dark(int64_t t)
{
    if (step == 0 && t >= 3000)
    {
        step++;
        W.display_on = false;
    }
    if (step == 1 && t >= 5000)
    {
        step++;
        W.display_on = true;
        W.settle_until = now + boot_extra_us + 300000;  /* after the boot */
    }
}

/* paused, the display times out, wakes still paused, then plays on */
static int64_t paused_at, shown_on_wake;
static uint32_t shown_before_wake;
static void h_dark_paused(int64_t t)
{
    if (step == 0 && t >= 3000) { step++; rebase(); W.playing = false;
                                  paused_at = audio_now(); }
    if (step == 1 && t >= 3500) { step++; W.display_on = false; }
    if (step == 2 && t >= 5000) { step++; W.display_on = true;
                                  shown_before_wake = W.shown; }
    if (step == 3 && t >= 5600) { step++; shown_on_wake = W.last_pts; }
    if (step == 4 && t >= 6000) { step++; W.t_base = now; W.playing = true;
                                  W.settle_until = now + 300000; }
}

static void h_fast(int64_t t)
{
    (void)t;
    if (step == 0)
    {
        step++;
        vpc_set_speed(&V, 250);
    }
}

/* the first-use check's live invalidation: at the 60th packet the
   image reports a decode error, or the chip falls back to the crystal */
static int bad_kind;
static void h_bad(int64_t t)
{
    (void)t;
    if (M.total_packets < 60 || step)
        return;
    step++;
    if (bad_kind == VCHECK_BAD_ERROR)
        M.st_errors = 1;
    else if (bad_kind == VCHECK_BAD_CONCEAL)
        M.st_concealed = 3;
    else
    {
        M.band = 0;             /* clock.c xtal_off */
        M.mhz = 0;
    }
}

static int dies;
static void h_die(int64_t t)
{
    (void)t;
    if (dies && M.total_packets >= 100 && !M.hang && M.boots == 1)
        M.hang = true;
    if (dies > 1 && M.total_packets >= 30 && !M.hang && M.boots == 2)
        M.hang = true;
}

/* Stage 6's pacing figures on a made-up 24 fps run: three seconds steady
   with a poll's worth of noise (the first VTRACE_SETTLE_MS, 2 s, not
   counted), then one picture 30 ms late and the next back on time; then
   a run whose pictures only ever come a poll late */
static void trace_pacing(void)
{
    static struct vtrace_rec rec[128];
    struct vtrace t;
    struct vtrace_sum s;
    struct vpc_frame f;
    int k;

    memset(&f, 0, sizeof(f));
    vtrace_init(&t, rec, ARRAYLEN(rec));
    for (k = 0; k < 96; k++)
    {
        f.pts_us = k * 1000000LL / 24;
        f.audio_us = f.pts_us + (k & 1) * 8000 + (k == 74) * 30000;
        f.shown = k + 1;
        f.decoded = k + 1;
        f.mhz = 150;
        vtrace_add(&t, &f);
    }
    vtrace_summary(&t, true, false, &s);
    check(s.pass && s.hitches == 1 && s.hitch_ms == 74 * 1000 / 24 &&
          s.jitter_max_us == 22000 && s.intervals == 48,
          "stage 6 pacing: one late picture is one hitch, at its picture");
    vtrace_init(&t, rec, ARRAYLEN(rec));
    for (k = 0; k < 72; k++)
    {
        f.pts_us = k * 1000000LL / 24;
        f.audio_us = f.pts_us + (k % 3) * 5000;
        f.shown = k + 1;
        vtrace_add(&t, &f);
    }
    vtrace_summary(&t, true, false, &s);
    check(s.hitches == 0 && s.jitter_max_us == 10000,
          "stage 6 pacing: a poll's worth of jitter is no hitch");
    /* sitting 4c's vfeed of stage7.m4v (24 fps): the start's A/V
       transient (62 ms at 0.54 s, 58 ms at 1.08 s, 51 ms at 1.29 s), then
       under 45 ms for 10 s; the owner saw it smooth: settled at 2 s, it
       passes, and a 51 ms picture later still fails */
    for (k = 0; k < 2; k++)
    {
        int i;

        vtrace_init(&t, rec, ARRAYLEN(rec));
        for (i = 0; i < 120; i++)
        {
            int32_t d = (i % 5) * 9000;          /* 0 .. 36 ms */

            f.pts_us = i * 1000000LL / 24;
            if (i == 13)
                d = 62000;
            else if (i == 26)
                d = 58236;
            else if (i == 31)
                d = 50834;
            else if (i == 34)
                d = 47329;
            else if (i == 100 && k)
                d = 51000;
            f.audio_us = f.pts_us - d;
            f.shown = i + 1;
            f.decoded = i + 1;
            vtrace_add(&t, &f);
        }
        vtrace_summary(&t, true, false, &s);
        check(k ? !s.pass && s.drift_max_us == 51000
                : s.pass && s.drift_max_us == 36000,
              k ? "stage 6 A/V: a late 51 ms still fails"
                : "stage 6 A/V: sitting 4c's start transient settles");
    }
}

int main(int argc, char **argv)
{
    enum vpc_state st;
    int i;

    sram = calloc(1, VBM_SRAM_SIZE);
    sdram = calloc(1, VBM_SDRAM_SIZE);
    mscratch = calloc(1, VBM_SCRATCH);
    if (argc < 2 || load(argv[1]) < 0)
    {
        printf("FAIL loading %s\n", argc > 1 ? argv[1] : "(no file)");
        return 1;
    }
    printf("     %s: %u samples, frame %lld us, params %ld bytes\n", argv[1],
           F.n, (long long)F.frame_us, F.params_len);

    /* 0. the profile check at open */
    check(vpc_baseline_ok(66, 0x00) && vpc_baseline_ok(66, 0xc0)
          && vpc_baseline_ok(77, 0xc0) && vpc_baseline_ok(77, 0x80)
          && !vpc_baseline_ok(77, 0x40) && !vpc_baseline_ok(100, 0x00),
          "profile: Baseline, or Main declaring Baseline conformance");

    /* 1. normal, with stage 6's trace */
    check(setup() == 0, "normal: the index opens and its keys load");
    vtrace_init(&trace, trace_rec, ARRAYLEN(trace_rec));
    tr = &trace;
    st = run(30000, NULL);
    tr = NULL;
    report("normal");
    {
        struct vtrace_sum ts;

        vtrace_summary(&trace, st == VPC_DONE, st == VPC_FAILED, &ts);
        trace_lines = 0;
        vtrace_lines(&trace, &ts, count_line, NULL);
        check(ts.pass && ts.records > F.n / 2 && ts.mhz_lo == 27 &&
              ts.mhz_hi == 27 && ts.drift_max_us <= frame() &&
              trace_lines == (int)(4 + ts.records) && ts.intervals > 0 &&
              ts.hitches == 0,
              "stage 6 trace: a clean run passes, 27 MHz, no hitches, one line "
              "a record");
    }
    trace_pacing();
    check(st == VPC_DONE && W.shown == F.n && W.bad_bytes == 0 &&
          W.params_missing == 0 && M.fifo_errors == 0 && !M.port_misuse,
          "normal: every sample shown, byte for byte, parameter sets first, "
          "whole words through the port");
    check(W.err_max <= frame(), "normal: every picture within a frame of its "
          "time");
    vpc_stop(&V);
    check(W.releases == 1 && !M.powered, "normal: stop cuts and gives the "
          "BCM back");

    /* 1b. C5: the thread sleeps until the next record is due (between a
       tick and four), not a tick at a time: every picture still shown in
       sync, far fewer wakes */
    {
        uint32_t fixed_ticks;

        setup();
        ticks = 0;
        st = run(30000, NULL);
        fixed_ticks = ticks;
        vpc_stop(&V);
        setup();
        ticks = 0;
        honor_wait = true;
        st = run(30000, NULL);
        honor_wait = false;
        report("deadline waits");
        printf("     %u wakes against %u a tick at a time\n", ticks,
               fixed_ticks);
        check(st == VPC_DONE && W.shown == F.n && W.err_max <= frame() &&
              ticks * 2 < fixed_ticks,
              "deadline waits: every picture in sync, under half the wakes");
        vpc_stop(&V);
    }

    /* 2. pause */
    setup();
    st = run(30000, h_pause);
    report("pause");
    check(st == VPC_DONE && W.shown == F.n && W.bad_bytes == 0 &&
          W.err_max <= frame(), "pause: in sync after a 1 s pause");
    vpc_stop(&V);

    /* 2b. a pause longer than VPC_STILL_US (session 29: the watchdog
       restarted the session at every resume) */
    setup();
    st = run(30000, h_pause3);
    report("pause 3 s");
    check(st == VPC_DONE && V.stats.stills == 0 && V.stats.restarts == 0 &&
          W.shown == F.n && W.err_max <= frame(),
          "pause 3 s: no watchdog restart on resume, in sync after");
    vpc_stop(&V);

    /* 2b2. resumed 300 ms past the pictures (dev41); dev42: the key
       before the audio is behind the engine, so no seek: the pictures up
       to the audio are decoded unshown from where the engine is */
    setup();
    st = run(30000, h_pause_jump);
    report("resume past the pictures");
    check(st == VPC_DONE && V.eng.stats.dropped_gops == 0 &&
          V.stats.restarts == 0 && W.hidden > 0 && W.err_max <= frame() &&
          W.bad_bytes == 0 && W.order_bad == 0,
          "resume past the pictures: decoded on unshown, no restart, no GOP "
          "dropped, in sync");
    vpc_stop(&V);
    /* 2b3. resumed past a key: the seek is the shorter way (its catch-up
       aim leaves pictures up to 50 ms early for a moment, as a wake's
       seek does: within two frames, as "resident, dark") */
    {
        uint32_t i0 = (uint32_t)(3000000 / F.frame_us), i;

        for (i = i0 + 1; i < F.n && !F.key[i]; i++)
            ;
        jump_us = i < F.n ? F.pts[i] - F.pts[i0] + 200000 : 300000;
    }
    setup();
    st = run(30000, h_pause_jump);
    report("resume past a key");
    check(st == VPC_DONE && V.eng.stats.dropped_gops == 0 &&
          V.stats.restarts == 1 && W.err_max <= 2 * frame() &&
          W.bad_bytes == 0,
          "resume past a key: one seek, no GOP dropped, in sync");
    vpc_stop(&V);
    jump_us = 300000;

    /* 2c. the shell's overlay over a paused picture (dev34) */
    setup();
    st = run(4000, h_overlay);
    check(M.overlays >= 3 && ovl_after_restart == (320u | 24u << 16) &&
          ovl_bytes_ok && M.ovl_wh == 0,
          "overlay: up while paused, put back after a restart's OPEN, "
          "the pixels byte for byte, then away");
    vpc_stop(&V);

    /* 2d. stage 8b's PCM round trip (dev39): a block through the chip at
       half gain and back, every sample */
    setup();
    st = run(1000, NULL);
    {
        static uint16_t pcm[2048];
        struct vpc_audio_times t;
        uint32_t k, bad = 0;
        int rc;

        for (k = 0; k < 2048; k++)
            pcm[k] = (uint16_t)(k * 977);
        rc = vpc_audio_config(&V, 0x340000, 0x360000, 0x8000);
        if (!rc)
            rc = vpc_audio_block(&V, 0x340000, 0x360000, 0x1000, pcm,
                                 sizeof(pcm), 0x8000, &t);
        for (k = 0; k < 2048; k++)
            bad += (int16_t)pcm[k] != (int16_t)(((int16_t)(k * 977) *
                                                  (int32_t)0x8000) >> 16);
        check(rc == 0 && bad == 0 && M.audio_blocks == 1,
              "audio: a PCM block through the chip and back, every sample");
    }
    vpc_stop(&V);

    /* 3. seek */
    setup();
    st = run(30000, h_seek);
    report("seek");
    check(st == VPC_DONE && W.bad_bytes == 0 && W.params_missing == 0 &&
          M.fifo_errors == 0 && W.err_max <= frame(),
          "seek: restarts at a key with the parameter sets, in sync after");
    vpc_stop(&V);

    /* 3b. seek into a GOP (dev32): the way from the key is decoded and
       not shown, and the first picture shown is the audio's */
    setup();
    for (seek_mid = F.n / 2; seek_mid < F.n && F.key[seek_mid]; seek_mid++)
        ;
    while (seek_mid + 1 < F.n && !F.key[seek_mid + 1] &&
           (seek_mid < 8 || !F.key[seek_mid - 8]))
        seek_mid++;
    st = run(30000, h_seek_mid);
    report("seek into a GOP");
    printf("     %u hidden; the first picture shown %lld us off\n", W.hidden,
           (long long)W.first_err);
    /* the lead's first correction after the seek leaves pictures up to
       a frame early for a few hundred ms (dev32: 40 ms at most) */
    check(st == VPC_DONE && W.hidden > 0 && W.bad_bytes == 0 &&
          W.first_err <= frame() && W.first_err >= -frame() &&
          W.err_max <= frame() + frame() / 4,
          "seek into a GOP: the key's way hidden, the first picture on time");
    vpc_stop(&V);

    /* 3c. a session that takes records and shows nothing (session 28's
       start): the watchdog restarts it where the audio is */
    setup();
    M.stuck_opens = 1;
    st = run(8000, NULL);
    report("still start");
    check(st != VPC_FAILED && V.stats.stills == 1 && W.shown > 100,
          "still start: restarted once after VPC_STILL_US, then pictures");
    vpc_stop(&V);

    /* 3c2. stuck until the image is booted again (session 30: a stall a
       restart didn't cure, MENU and back did): the watchdog reloads */
    setup();
    M.stuck_boots = 1;
    st = run(10000, NULL);
    report("stuck until a reboot");
    check(st != VPC_FAILED && V.stats.stills == 1 && V.stats.reloads == 1 &&
          M.boots == 2 && W.shown > 100,
          "stuck until a reboot: a restart, then a reload 1 s on, then "
          "pictures");
    vpc_stop(&V);

    /* 3c3. the audio stands still while playing (session 36c): the
       pictures wait for it, the watchdog stands down (no restart, no
       reload), and they follow once it runs */
    setup();
    step = 0;
    st = run(12000, h_audio_stall);
    report("audio stands still");
    check(st != VPC_FAILED && V.stats.stills == 0 && V.stats.reloads == 0 &&
          V.stats.audio_stills == 1 && M.boots == 1 && W.shown > 60,
          "audio stands still: no restart or reload, pictures once it runs");
    vpc_stop(&V);

    /* 3d. a session that never shows anything, however often restarted:
       given up after VPC_STILL_MAX restarts, with its own reason */
    setup();
    M.stuck_opens = 100;
    st = run(20000, NULL);
    report("never a picture");
    check(st == VPC_FAILED && V.fail_why == BVP_E_STILL &&
          V.stats.stills == VPC_STILL_MAX && W.shown == 0,
          "never a picture: given up after VPC_STILL_MAX restarts "
          "(BVP_E_STILL)");
    vpc_stop(&V);

    /* 4. slow decoding: GOP drops restart the session */
    setup();
    M.decode_us = 50000;
    st = run(60000, NULL);
    report("slow");
    check(st == VPC_DONE && W.bad_bytes == 0 && W.params_missing == 0 &&
          M.fifo_errors == 0 && V.eng.stats.dropped_gops > 0 &&
          V.stats.restarts > 0 && W.shown < F.n, "slow: GOPs dropped through restarts, no FIFO "
          "order errors, every packet intact");
    vpc_stop(&V);

    /* 4b. the 150 MHz gate: every OPEN, restarts included, carries
       HP_OPEN_FAST_CLOCK when the backend allows it, and none otherwise */
    check(M.opens > 1 && M.fast_opens == 0,
          "clock: without the 4b/4c flags no OPEN asks for 150 MHz");
    setup();
    fast = HP_OPEN_FAST_CLOCK;
    M.decode_us = 50000;
    vtrace_init(&trace, trace_rec, ARRAYLEN(trace_rec));
    tr = &trace;
    st = run(60000, NULL);
    tr = NULL;
    report("slow, 150 MHz");
    check(st == VPC_DONE && M.opens > 1 && M.fast_opens == M.opens,
          "clock: with the flags every OPEN, restarts too, asks for 150 MHz");
    {
        struct vtrace_sum ts;

        vtrace_summary(&trace, st == VPC_DONE, st == VPC_FAILED, &ts);
        trace_lines = 0;
        vtrace_lines(&trace, &ts, count_line, NULL);
        check(!ts.pass && ts.dropped_gops > 0 && ts.mhz_hi == 150 &&
              ts.work_max_us == 50000,
              "stage 6 trace: dropped GOPs fail it; 150 MHz and the chip's "
              "time are reported");
    }
    vpc_stop(&V);
    fast = 0;

    /* 4c. SDRAM doesn't come up on PLL A (session 9): the same image,
       still running, is opened again at 27 MHz and stays there, restarts
       included; the stage 6 readback runs once and finds the pattern */
    setup();
    fast = HP_OPEN_FAST_CLOCK;
    M.nomem_fast = true;
    M.decode_us = 50000;
    V.sd_check = true;
    st = run(60000, NULL);
    report("SDRAM fails on PLL A");
    check(st == VPC_DONE && M.boots == 1 && M.opens > 1 &&
          M.fast_opens == 0 && (V.open_drop & HP_OPEN_FAST_CLOCK),
          "SDRAM: a NOMEM at 150 MHz opens again at 27 MHz, no reboot, "
          "and later OPENs stay at 27 MHz");
    check(V.sdc.done && V.sdc.words == VPC_SDC_BLOCKS * VPC_SDC_READS &&
          V.sdc.bad == 0 && V.sdc.port_err == 0,
          "SDRAM: the stage 6 readback reads its pattern back");
    check(V.bvp.sd_n == 1 && (V.bvp.sd[0] & 0xFFFF0000) == 0x53440000,
          "SDRAM: the OPEN reply's set-up word is kept");
    vpc_stop(&V);
    fast = 0;

    /* 4d. session 10: the image locks the bus at its first FEED on PLL
       A.  The port reads nothing, power is cut, the reload runs at
       27 MHz and plays to the end */
    setup();
    fast = HP_OPEN_FAST_CLOCK;
    M.hang_fast = true;
    M.hang_port = true;
    st = run(30000, NULL);
    report("bus lock at 150 MHz");
    check(st == VPC_DONE && V.pm_losses == 1 && V.pm_n == 0 && V.pm_fast &&
          (V.open_drop & HP_OPEN_FAST_CLOCK) && M.boots == 2,
          "lost on PLL A: nothing read, reloaded at 27 MHz, played on");
    vpc_stop(&V);

    /* 4e. the same with the port alive (a software hang): the progress
       word comes back */
    setup();
    M.hang_fast = true;
    M.hang_port = false;
    st = run(30000, NULL);
    check(st == VPC_DONE && V.pm_losses == 1 && V.pm_n == VPC_PM_WORDS &&
          V.pm[0] == 0x31000007,
          "lost, port alive: the progress words are read back");
    vpc_stop(&V);

    /* 4f. the diagnostic pass: the memory test word, the frames' place,
       and the readback over all 4 MB */
    setup();
    fast = 0;
    V.sd_check = V.sd_whole = true;
    st = run(30000, NULL);
    check(st == VPC_DONE && V.dpb == 0x0002A920u && V.sdc.done &&
          V.sdc.span == VPC_SDC_SDRAM && V.sdc.words == VPC_SDC_WHOLE *
          VPC_SDC_READS && V.sdc.bad == 0,
          "readback over 4 MB; the frames' place read at the first picture");
    vpc_stop(&V);

    /* 4g. session 11: the port fails for a few accesses while the
       image plays on; the stall is ridden out, no reload */
    setup();
    M.stall_after = 3;
    M.stall_n = 5;
    st = run(30000, NULL);
    report("port stall");
    check(st == VPC_DONE && V.stalls >= 1 && V.stats.recoveries == 0 &&
          M.boots == 1 && W.bad_bytes == 0,
          "port stall: ridden out while the image beats, no reload");
    vpc_stop(&V);

    /* 4g2. a stall on writes (dev13): a packet or a command part
       written, or the event index not; ridden out, nothing is left half
       done */
    setup();
    M.stall_after = 5;
    M.stall_n = 3;
    M.stall_writes = true;
    st = run(30000, NULL);
    report("port stall, writes too");
    {
        unsigned k, held = 0;

        for (k = 0; k < BVP_REPLIES; k++)
            held += V.bvp.replies[k].tag != 0;
        check(st == VPC_DONE && V.stalls >= 1 && V.stats.recoveries == 0 &&
              M.boots == 1 && W.bad_bytes == 0 && M.fifo_errors == 0 &&
              W.params_missing == 0 && held <= 1 && !V.bvp.evt_rd_stale,
              "port stall on writes: ridden out, no packet or reply left "
              "half done");
    }
    vpc_stop(&V);

    /* 4g3. session 12: the event index overwritten on the chip.  It is
       not a port stall (dev12 rode it out eight times, -1 being both
       "torn" and BVP_E_PORT): after VPC_TORN_MAX polls the image is
       reloaded, and plays on */
    setup();
    M.tear_after = 4;
    st = run(30000, NULL);
    report("event index torn");
    check(st == VPC_DONE && V.stalls == 0 && V.stats.recoveries == 1 &&
          M.boots == 2 && V.bvp.evt_torn == 0,
          "torn event index: reloaded as a protocol failure, no ride-outs");
    vpc_stop(&V);

    /* 4h. stage 6's paced writes with readback (dev12) */
    setup();
    V.wire.pace = 128;
    V.wire.verify = true;
    st = run(30000, NULL);
    report("paced, read back");
    check(st == VPC_DONE && W.bad_bytes == 0 && W.params_missing == 0 &&
          V.wire.words > 0 && V.wire.bad == 0 && W.shown == F.n,
          "paced writes: every packet intact and read back right");
    vpc_stop(&V);

    /* 4i. lockstep: nothing written while a posted packet is undecoded */
    setup();
    V.lockstep = true;
    V.wire.verify = true;
    st = run(30000, NULL);
    report("lockstep");
    printf("     lockstep held %u pushes; %u skipped, %u late posts\n",
           V.lock_waits, V.eng.stats.skipped, V.eng.stats.late_posts);
    check(st == VPC_DONE && W.bad_bytes == 0 && V.lock_waits > 0 &&
          V.wire.bad == 0,
          "lockstep: plays to the end, pushes held while the chip decodes");
    vpc_stop(&V);

    /* 4j. an unsupported stream: the image's EV_ERROR stops the video,
       the BCM is given back and the audio plays alone */
    setup();
    M.refuse = true;
    st = run(10000, NULL);
    check(st == VPC_FAILED && V.fail_why == HP_E_UNSUPPORTED &&
          W.releases == 1 && V.stats.recoveries == 0,
          "unsupported stream: the video gives up, the BCM is given back");
    vpc_stop(&V);

    /* 5. 2.5x: key frames only */
    setup();
    W.speed = 250;
    st = run(30000, h_fast);
    report("2.5x");
    check(st == VPC_DONE && W.bad_bytes == 0 && M.fifo_errors == 0 &&
          W.params_missing == 0 && W.shown == V.nkeys,
          "2.5x: key frames only, through restarts");
    vpc_stop(&V);

    /* 6. dark, then awake */
    setup();
    st = run(30000, h_dark);
    report("dark");
    check(st == VPC_DONE && V.stats.wakes == 1 && M.boots == 2 &&
          M.cuts >= 1 && W.bad_bytes == 0 && W.params_missing == 0 &&
          W.err_max <= frame(),
          "dark: BCM off while dark, back in sync with the audio on waking");
    vpc_stop(&V);

    /* 6a. session 22: the image's boot takes 500 ms.  With the pre-roll
       (the audio starts after vpc_start) nothing at the start is late;
       on waking from dark the seek allows for the boot */
    setup();
    boot_extra_us = 500000;
    preroll = true;
    st = run(30000, NULL);
    report("500 ms boot, pre-roll");
    check(st == VPC_DONE && V.eng.stats.skipped == 0 &&
          V.eng.stats.dropped_gops == 0 && W.shown == F.n,
          "500 ms boot with the pre-roll: no picture skipped at the start");
    vpc_stop(&V);
    setup();
    st = run(30000, h_dark);
    report("500 ms boot, dark");
    printf("     dropped GOPs %u, skipped %u, wakes %u\n",
           V.eng.stats.dropped_gops, V.eng.stats.skipped, V.stats.wakes);
    check(st == VPC_DONE && V.stats.wakes == 1 &&
          V.eng.stats.dropped_gops == 0 && W.err_max <= frame(),
          "500 ms boot: waking from dark drops no GOP, in sync after");
    vpc_stop(&V);
    boot_extra_us = 0;
    preroll = false;

    /* 6b. dark while paused, awake still paused */
    setup();
    st = run(30000, h_dark_paused);
    report("dark paused");
    printf("     paused at %lld us, last picture %lld us on waking; %u shown "
           "before, %u after\n", (long long)paused_at,
           (long long)shown_on_wake, shown_before_wake, W.shown);
    check(st == VPC_DONE && V.stats.wakes == 1 && W.bad_bytes == 0 &&
          W.shown > shown_before_wake &&
          shown_on_wake - paused_at <= frame() &&
          paused_at - shown_on_wake <= frame(),
          "dark paused: waking still paused shows the paused picture again");
    check(W.err_max <= frame(), "dark paused: in sync once it plays on");
    vpc_stop(&V);

    /* 6c. a long pause on slow ticks: the 8-bit heartbeat comes round
       every 262 ms, and looks spaced at a multiple of that see one value
       on a live image (the sim found this) */
    setup();
    W.playing = false;
    tick_us = 131072;
    st = run(5000, NULL);
    tick_us = TICK_US;
    check(st == VPC_PLAYING && V.stats.recoveries == 0,
          "slow ticks: a paused, live image is never taken for dead");
    vpc_stop(&V);

    /* 7. the buffer behind */
    setup();
    W.ahead_us = 150000;
    W.keep_us = 50000;
    st = run(30000, NULL);
    report("buffer");
    check(st == VPC_DONE && W.again > 0 && W.bad_bytes == 0 &&
          W.shown == F.n, "buffer: waits for data, every sample shown");
    vpc_stop(&V);

    /* 8. the image dies once: reloaded, back in sync */
    setup();
    dies = 1;
    st = run(30000, h_die);
    dies = 0;
    report("die once");
    check(st == VPC_DONE && V.stats.recoveries == 1 && M.boots == 2 &&
          W.releases == 0 && W.bad_bytes == 0,
          "die once: the image is reloaded and video goes on");
    vpc_stop(&V);

    /* the first-use check (vcheck.h): once, between HELLO and the play's
       OPEN, never at a reload; its failure fails the play; the play's
       errors, a fallback and a loss each report once */
    setup();
    V.check_due = true;
    check_calls = check_rc = bad_n = 0;
    check_session = -1;
    dies = 1;
    st = run(30000, h_die);
    dies = 0;
    check(st == VPC_DONE && check_calls == 1 && check_session == 0 &&
          V.stats.recoveries == 1 && bad_n == 1 &&
          bad_why == VCHECK_BAD_FAULT,
          "check: once at the start, none at the reload; the loss reported");
    vpc_stop(&V);
    setup();
    V.check_due = true;
    check_calls = bad_n = 0;
    check_rc = BVP_E_CHECK;
    st = run(2000, NULL);
    check_rc = 0;
    check(st == VPC_FAILED && V.fail_why == BVP_E_CHECK && check_calls == 1
          && W.releases == 1 && M.opens == 0,
          "check failed: no OPEN, the play fails, the BCM goes back");
    {
        static const int kinds[3] = { VCHECK_BAD_ERROR, VCHECK_BAD_CONCEAL,
                                      VCHECK_BAD_CLOCK };
        int k;

        for (k = 0; k < 3; k++)
        {
            setup();
            M.vclk = true;
            fast = HP_OPEN_FAST_CLOCK;
            bad_kind = kinds[k];
            bad_n = 0;
            cap_rc = 99;
            st = run(30000, h_bad);
            check(st == VPC_DONE && bad_n == 1 && bad_why == kinds[k] &&
                  cap_rc == 0 && M.band == 10,
                  "invalidated once by an error, a concealed block or a "
                  "fallback; the play stepped down a band");
            vpc_stop(&V);
            fast = 0;
            M.vclk = false;
            M.st_errors = M.st_concealed = 0;
        }
        /* a refused stream is the file's: no invalidation */
        setup();
        M.refuse = true;
        bad_n = 0;
        st = run(3000, NULL);
        check(st == VPC_FAILED && bad_n == 0,
              "a refused stream invalidates nothing");
        M.refuse = false;
    }

    /* 9. dies twice: video stops, the BCM back to the display driver */
    setup();
    dies = 2;
    st = run(30000, h_die);
    dies = 0;
    report("die twice");
    check(st == VPC_FAILED && W.releases == 1 && !M.powered,
          "die twice: video stops and the normal screen comes back");

    /* 10. no protocol, 11. another major */
    setup();
    M.no_protocol = true;
    st = run(1000, NULL);
    check(st == VPC_FAILED && W.releases == 1 && V.fail_why == BVP_E_PROTO,
          "no protocol: video never starts, the BCM goes back");
    setup();
    M.major = HP_MAJOR + 1;
    st = run(1000, NULL);
    check(st == VPC_FAILED && W.releases == 1 && V.fail_why == BVP_E_PROTO,
          "another major: video never starts, the BCM goes back");

    /* 9. p14-resident: the image stays between videos.  Booted once (the
       boot path), a play boots nothing (HELLO and OPEN only), the stop
       hands the panel back with no cut, the next play boots nothing
       either; a dark spell still cuts and boots again, and that stop
       hands back too (the same bytes run).  The device's boot takes
       300-600 ms: the resident start is that much quicker. */
    resident = true;
    boot_extra_us = 500000;
    setup();
    vbm_boot(&M);               /* the boot path's swap */
    preroll = true;
    st = run(30000, NULL);
    report("resident, first play");
    check(st == VPC_DONE && M.boots == 1 && W.boot_skips == 1 &&
          W.shown == F.n && W.bad_bytes == 0 && V.boot_us < 50000,
          "resident: a play boots nothing, HELLO and OPEN only");
    vpc_stop(&V);
    check(W.handbacks == 1 && W.releases == 0 && M.cuts == 0 && M.powered &&
          M.running && M.closes >= 1 && !M.session,
          "resident: the stop closes and hands the panel back, no cut");
    {
        uint32_t boots = M.boots, cuts = M.cuts;
        long nk = vpc_init(&V, &src, &be, vmem, sizeof(vmem));

        /* the next play: the same model, still running */
        check(nk > 0 && !vpc_keys(&V, keys, (uint32_t)nk),
              "resident: the index again");
        W.shown = W.bad_bytes = 0;
        W.last_pts = -1;
        W.t_base = now;
        W.a_base = 0;
        st = run(30000, NULL);
        report("resident, second play");
        check(st == VPC_DONE && M.boots == boots && M.cuts == cuts &&
              W.boot_skips == 2 && W.shown == F.n && W.bad_bytes == 0 &&
              M.fifo_errors == 0,
              "resident: a second play, no boot and no cut in between");
        vpc_stop(&V);
        check(W.handbacks == 2 && W.releases == 0,
              "resident: the second stop hands back too");
    }
    /* The skipped boot moves the dark spell about 5 ms against the clip
       (h_dark counts from vpc_start's call).  At this phase the wake's
       seek leaves pictures up to 38 ms early (50 ms with a 500 ms boot)
       for about a second: the same happens without the resident image
       (a 5 ms longer start gives 14.7 ms), so it is the wake's catch-up
       aim, not this path; within two frames here, and noted for the
       player (p14-resident report). */
    boot_extra_us = 0;
    setup();
    vbm_boot(&M);
    st = run(30000, h_dark);
    report("resident, dark");
    check(st == VPC_DONE && V.stats.wakes == 1 && M.boots == 2 &&
          M.cuts == 1 && W.boot_skips == 1 && W.err_max <= 2 * frame(),
          "resident: dark still cuts, the wake boots again, in sync after");
    vpc_stop(&V);
    check(W.handbacks == 1 && W.releases == 0 && M.running,
          "resident: after a wake's boot the stop still hands back");
    /* a failed CLOSE (the port gone) never hands back: cut and release */
    setup();
    vbm_boot(&M);
    st = run(2000, NULL);
    M.hang = M.hang_port = true;
    vpc_stop(&V);
    check(W.handbacks == 0 && W.releases == 1 && !M.powered,
          "resident: a stop whose CLOSE fails cuts and releases");
    M.hang = M.hang_port = false;
    resident = false;
    preroll = false;
    boot_extra_us = 0;

    /* dev42: session 35's "sample source failed" at a resume far into
       the file.  (a) the audio's buffer reset between the probe and the
       pieces: the packet goes, the push waits, every sample still shown */
    setup();
    W.race_sample = (int)(F.n / 2);
    st = run(30000, NULL);
    report("buffer reset mid-sample");
    check(st == VPC_DONE && W.races == 1 && V.stats.piece_waits == 1 &&
          V.stats.src_skips == 0 && W.shown == F.n && W.bad_bytes == 0 &&
          M.fifo_errors == 0,
          "source: a piece not buffered after its probe waits, nothing lost");
    vpc_stop(&V);
    /* (b) one failed read: why is kept, on from the next key */
    setup();
    W.bad_from = (int)(F.n / 2);
    W.bad_left = 1;
    st = run(30000, NULL);
    report("one failed read");
    check(st == VPC_DONE && V.stats.src_skips == 1 && W.bad_reads == 1 &&
          W.bad_bytes == 0 && W.order_bad == 0 && W.shown > F.n / 2,
          "source: a failed read skips to the next key and plays on");
    vpc_stop(&V);
    /* (c) the file fails from there on: three skips, then the video
       gives up (the audio plays on) */
    setup();
    W.bad_from = (int)(F.n / 3);
    W.bad_left = -1;
    st = run(30000, NULL);
    report("file fails for good");
    check(st == VPC_FAILED && V.fail_why == BVP_E_STATE &&
          V.stats.src_skips == VPC_SRC_TRIES &&
          V.src_fail.why == 0 && W.bad_bytes == 0,
          "source: VPC_SRC_TRIES skips without a picture, then it gives up");
    vpc_stop(&V);

    /* P19 PW4: the chip's clock governor (vclk.h).  The model decodes a
       packet in 11 ms x 150 / the band's MHz.  (a) It starts at the top
       (the OPEN's flag), lowers the session's floor after a steady 3 s,
       keeps every picture in time, and sums up at the stop */
    vmask = 0xFFF;
    fast = HP_OPEN_FAST_CLOCK;
    setup();
    M.vclk = true;
    vclk_gov_n = vclk_sum_n = vclk_fail_n = 0;
    st = run(30000, NULL);
    report("vclk governor");
    printf("     bands asked %u, the last %u MHz; on %d, misses %u, lines %u, "
           "refused %u\n", M.vclk_sets, vclk_band_mhz[M.band], V.vclk_on,
           V.vclk_misses, vclk_gov_n, M.vclk_bad);
    check(st == VPC_DONE && W.shown == F.n && W.err_max <= frame() &&
          V.vclk_on && V.vclk_misses == 0 && M.vclk_sets >= 1 &&
          M.band < VCLK_TOP && M.band > 0 && vclk_gov_n >= 1 &&
          !M.vclk_bad, "vclk: from the top to a lower band, every picture "
          "in time, no miss, nothing refused");
    vpc_stop(&V);
    check(vclk_sum_n == 1 && !strncmp(vclk_sum, "vclk sum policy=top mhz=", 24) &&
          strstr(vclk_sum, "misses=0") != NULL,
          "vclk: one summary line at the stop, no misses");
    /* (a2) the proportional in-band policy: the same play, in time, and
       its summary names the policy */
    vpolicy = VCLK_POLICY_PROP;
    setup();
    M.vclk = true;
    vclk_sum_n = 0;
    st = run(30000, NULL);
    report("vclk governor, proportional");
    printf("     the last %u MHz in band %u; %s\n", (unsigned)M.mhz, M.band,
           vclk_sum);
    check(st == VPC_DONE && W.shown == F.n && W.err_max <= frame() &&
          V.vclk_misses == 0 && M.band < VCLK_TOP && !M.vclk_bad,
          "vclk prop: a lower band, every picture in time, no miss");
    vpc_stop(&V);
    check(vclk_sum_n == 1 && strstr(vclk_sum, "policy=prop") != NULL,
          "vclk prop: the summary names the policy");
    vpolicy = VCLK_POLICY_TOP;
    /* (b) a seek puts the floor back at the top, and it comes down again */
    setup();
    M.vclk = true;
    {
        uint32_t sets0;

        st = run(1, NULL);
        while (now < 1000000 + 6000000 && st != VPC_FAILED)
        {
            uint32_t wait;

            now += tick_us;
            vbm_run(&M);
            st = vpc_tick(&V, audio_now(), W.playing, W.display_on, &wait);
        }
        sets0 = M.vclk_sets;
        check(M.band < VCLK_TOP, "vclk: lower before the seek");
        W.a_base = F.pts[F.n / 4];
        W.t_base = now;
        W.settle_until = now + 300000;
        vpc_seek(&V, audio_now());
        for (i = 0; i < 20; i++)
        {
            uint32_t wait;

            now += tick_us;
            vbm_run(&M);
            st = vpc_tick(&V, audio_now(), W.playing, W.display_on, &wait);
        }
        check(M.band == VCLK_TOP && V.vfl.floor == VCLK_TOP && sets0 >= 1,
              "vclk: a seek races to the top");
    }
    vpc_stop(&V);
    /* (c) the dark: the BCM cut, the sum so far; the wake starts again at
       the top */
    /* (d) no verified band but the crystal, or an image without the op:
       no HP_OP_VCLK at all, today's session */
    vmask = 0;
    setup();
    M.vclk = true;
    st = run(30000, NULL);
    check(st == VPC_DONE && !V.vclk_on && M.vclk_sets == 0 && !M.vclk_bad &&
          M.band == VCLK_TOP, "vclk: no mask, no governor (150 MHz as today)");
    vpc_stop(&V);
    vmask = 0xFFF;
    setup();
    M.vclk = false;
    st = run(30000, NULL);
    check(st == VPC_DONE && !V.vclk_on && W.shown == F.n,
          "vclk: an image without HP_OP_VCLK plays as today");
    vpc_stop(&V);
    vmask = 0;
    fast = 0;

    /* every file, played through */
    for (i = 1; i < argc; i++)
    {
        char name[160];

        if (i > 1 && load(argv[i]) < 0)
        {
            printf("FAIL loading %s\n", argv[i]);
            fails++;
            continue;
        }
        setup();
        st = run(60000, NULL);
        snprintf(name, sizeof(name), "real %s", argv[i]);
        report(name);
        check(st == VPC_DONE && W.shown == F.n && W.bad_bytes == 0 &&
              !M.port_misuse && W.err_max <= frame(), name);
        vpc_stop(&V);
    }

    printf("vpcore: %d failures\n", fails);
    return fails ? 1 : 0;
}
