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

/* The brief probes: wave 1 of brief-map-peripherals.md section 11 (PB-1
 * to PB-4; PB-5 is stage 8a-1's extra words) and of brief-map-power.md
 * section 9 (PW1, PW2, PW7, PW8), as Developer > Debug rows.
 *
 * Each row boots the probe image (VPLAYER_PROBE_IMAGE: the plain video
 * image, the only one with HP_OP_PEEK and friends),
 * runs its steps, then cuts the BCM's power and brings the display back
 * by the stock wake path (vplayer_probe), so every run ends in the same
 * recovery.  A row whose steps need the owner (a cable, a state) runs one
 * boot per step, with the prompt on the lit screen in between.  In wave 1
 * nothing drives a pin or an output: PEEK reads, the census samples, PB-4
 * pulls weakly on allowlisted inputs and leaves them pulled down, PW2
 * writes only Apple's cache values and puts ours back, PW7 writes free
 * SRAM and the host frame, PW8 copies between free regions.  PB-7 to
 * PB-9 (the TV, below) do switch a clock on and drive the TV output, each
 * behind its own confirm and the row before it; PW3 and PW4 (the core's
 * voltage and clock, at Apple's values) likewise, PW4 behind PW3; PW5
 * (Apple's hibernate pins) behind PW1, PW6 (the port and the panel with
 * the chip cut, read only) behind PW5.  PB-6 (Apple's own TV pattern,
 * host side only, on Apple's NOR vmcs) has its own confirm and no lock.
 *
 * Lines go to bcmtest.log under the specs' prefixes (pb1: ... pw8);
 * docs/research/bcm2722/bench-p19.md lists what the owner does.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "action.h"
#include "gui/list.h"
#include "kernel.h"
#include "system.h"
#include "usb.h"
#include "version.h"
#include "bcm-devtest.h"
#include "devbcm.h"

#ifdef HAVE_BCMFW_VIDEO
#include "audio.h"
#include "video/vplayer.h"
#include "video/bcmvp.h"
#include "../../bcmfw/hostproto.h"
#include "core_alloc.h"
#include "bcm-e2lab.h"          /* the patterns and the tally (C1) */
#include "bcm-coplab.h"
#include "devbcm_pb6.h"           /* PB-6 */
#include "devbcm_tv.h"            /* PB-7 to PB-9 */
#include "devbcm_pw.h"            /* PW3, PW4 */
#include "devbcm_hib.h"           /* PW5, PW6 */
#include "powerlog.h"

#define PB_HANG_US      2000000     /* the spec's watchdog (2 s) */
#define PB_PULL_US      50000       /* PB-4: one request of 8 pins */
#define PB_CENSUS_US    500000      /* PB-2: the sampling window */
#define A_COUNTER       0x10001804
#define A_LEV1          0x10002824

static char pb_line[200];
static struct pwh_lock hib_lock = { false, false };   /* this boot only */

static void pb_say(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(pb_line, sizeof(pb_line), fmt, ap);
    va_end(ap);
    devbcm_log(pb_line);
}

static int panel_of(uint32_t lev1)
{
    return ((lev1 >> 19) & 1) + 2 * ((lev1 >> 18) & 1);
}

/* PEEK of n addresses; HP_OK only with all 2n words back.  < 0: no reply
 * in 2 s (a hang) or the port died; > 0: the image's refusal */
static int pk(struct bvp *b, uint32_t gap, const uint32_t *a, uint32_t n,
              uint32_t *out)
{
    uint32_t args[2 + HP_PEEK_MAX], got = 0;
    int st;

    args[0] = HP_PEEK_MAGIC;
    args[1] = gap;
    memcpy(&args[2], a, 4 * n);
    st = bvp_call(b, HP_OP_PEEK, args, n + 2, out, 2 * n, &got, PB_HANG_US);
    if (st == HP_OK && got != 2 * n)
        st = HP_E_TOOBIG;
    return st;
}

/* The common canary: the counter 100 us apart (90-110), the panel ID as
 * the host saw it before the boot (host < 0: not read) */
static bool canary(struct bvp *b, const char *pfx, const char *ctx, int host)
{
    uint32_t a = A_COUNTER, o[2], d = 0;
    int st, id = -1;
    bool ok;

    st = pk(b, 100, &a, 1, o);
    if (st == HP_OK)
        d = o[1] - o[0];
    ok = st == HP_OK && d >= 90 && d <= 110;
    a = A_LEV1;
    if (pk(b, 0, &a, 1, o) == HP_OK)
        id = panel_of(o[0]);
    else
        ok = false;
    if (host >= 0 && id != host)
        ok = false;
    pb_say("%s canary %s: counter +%lu us in 100, panel %d (host %d): %s",
           pfx, ctx, (unsigned long)d, id, host, ok ? "ok" : "FAIL (run void)");
    return ok;
}

/* pre(): the panel ID as the running firmware leaves GPIO 50/51 */
static int read_panel(void *ctx)
{
    uint32_t v;

    if (bcm_video_rd(A_LEV1, &v) < 0)
        return -1;
    *(int *)ctx = panel_of(v);
    return 0;
}

static const char *two(uint32_t v1, uint32_t v2, char *buf)
{
    if (v1 == v2)
        snprintf(buf, 12, "%08lx", (unsigned long)v2);
    else
        strcpy(buf, "volatile");
    return buf;
}

/* ---- PB-1: the census of the brief's blocks --------------------------- */

static const uint32_t pb1a[] =
{
    0x10002C18, 0x10005404, 0x10006400, 0x10001C3C, 0x10004800,
    0x10002820, 0x10002824,
};
#define PB1A_N  ARRAYLEN(pb1a)
#define PB1B_N  (1 + 4 + 23 + 13 + 2 + 4 + 1)

static uint32_t pb1b_addr(int i)
{
    if (i == 0)
        return 0x10002C00;
    i -= 1;
    if (i < 4)
        return 0x10002C08 + 4 * i;
    i -= 4;
    if (i < 23)
        return 0x10005484 + 4 * i;
    i -= 23;
    if (i < 13)
        return 0x10001C28 + 4 * i;
    i -= 13;
    if (i < 2)
        return 0x10004810 + 4 * i;
    i -= 2;
    if (i < 4)
        return 0x10002800 + 4 * i;
    return 0x10002860;
}

struct pb1_run
{
    int panel;
    int clocks;
    int n1a[2], n1b[2];
    bool canary[2];
    int hang;                   /* 0, or the step that hung: 1 1a, 2 1b */
    uint32_t hang_addr;
    uint32_t v5404, v2c18[2], v1c30, v6400;
};

static int pb1_pass(struct bvp *b, struct pb1_run *r, int c)
{
    const char *clk = c ? "150MHz" : "27MHz";
    uint32_t a, o[2];
    char buf[12];
    int i, st;

    r->canary[c] = canary(b, "pb1:", clk, r->panel);
    if (!r->canary[c])
        return -1;
    for (i = 0; i < (int)PB1A_N + PB1B_N; i++)
    {
        bool in_a = i < (int)PB1A_N;

        a = in_a ? pb1a[i] : pb1b_addr(i - PB1A_N);
        st = pk(b, 0, &a, 1, o);
        if (st < 0)
        {
            r->hang = in_a ? 1 : 2;
            r->hang_addr = a;
            pb_say("pb1: hang at step %s %08lx (%d)", in_a ? "1a" : "1b",
                   (unsigned long)a, st);
            return -2;
        }
        if (st != HP_OK)
        {
            pb_say("pb1: %s %08lx refused (%d)", clk, (unsigned long)a, st);
            continue;
        }
        pb_say("pb1: %s %08lx %08lx %s", clk, (unsigned long)a,
               (unsigned long)o[0], two(o[0], o[1], buf));
        if (in_a)
            r->n1a[c]++;
        else
            r->n1b[c]++;
        if (c == 0)
        {
            if (a == 0x10005404)
                r->v5404 = o[0];
            if (a == 0x10002C18)
                r->v2c18[0] = o[0], r->v2c18[1] = o[1];
            if (a == 0x10001C30)
                r->v1c30 = o[0];
            if (a == 0x10006400)
                r->v6400 = o[0];
        }
    }
    pb_say("pb1: done %s 1a=%d 1b=%d", clk, r->n1a[c], r->n1b[c]);
    return 0;
}

static int pb1_fn(struct bvp *b, uint32_t fast, void *ctx)
{
    struct pb1_run *r = ctx;
    int rc = pb1_pass(b, r, 0);

    r->clocks = 1;
    if (rc == 0 && fast)
    {
        if (bvp_open(b, 320, 240, fast) != HP_OK)
        {
            pb_say("pb1: OPEN at 150 MHz failed: the 27 MHz pass only");
            return 0;
        }
        rc = pb1_pass(b, r, 1);
        r->clocks = 2;
        bvp_close(b, 0);
    }
    return rc;
}

bool devbcm_pb1(void)
{
    static const char *const lines[] = {
        "Read-only, under a minute,",
        "hands off: the video chip's",
        "TV, DAC, USB, clock and audio",
        "registers, read twice each." };
    struct simplelist_info info;
    struct pb1_run r;
    int rc, c;
    bool pass;

    if (!devbcm_lab_open(&info, "BCM probe PB-1", lines, ARRAYLEN(lines)))
        return false;
    memset(&r, 0, sizeof(r));
    r.panel = -1;
    pb_say("=== BCM probe PB-1 (census, read-only): build %s ===", rbversion);
    rc = vplayer_probe(read_panel, pb1_fn, &r);
    pass = rc == 0 && r.canary[0] && r.n1a[0] == (int)PB1A_N &&
           r.n1b[0] == PB1B_N &&
           (r.clocks < 2 || (r.canary[1] && r.n1a[1] == (int)PB1A_N &&
                             r.n1b[1] == PB1B_N));
    pb_say("pb1: %s (rc %d, %d clock%s)", pass ? "PASS" : "FAIL", rc,
           r.clocks, r.clocks == 1 ? "" : "s");
    simplelist_addline("PB-1: %s", pass ? "PASS" : "FAIL");
    if (r.hang)
        simplelist_addline("Hang at %s %08lx", r.hang == 1 ? "1a" : "1b",
                           (unsigned long)r.hang_addr);
    for (c = 0; c < r.clocks; c++)
        simplelist_addline("%s: 1a %d/%d, 1b %d/%d%s", c ? "150 MHz" : "27 MHz",
                           r.n1a[c], (int)PB1A_N, r.n1b[c], PB1B_N,
                           r.canary[c] ? "" : " (canary FAIL)");
    if (r.n1a[0] == (int)PB1A_N)
    {
        simplelist_addline("DAC 5404 bit 0: %lu", (unsigned long)(r.v5404 & 1));
        simplelist_addline("TV 2C18 bit 31: %s",
                           (r.v2c18[0] ^ r.v2c18[1]) >> 31 ? "toggles" : "static");
        simplelist_addline("USB 6400 bit 24: %lu",
                           (unsigned long)(r.v6400 >> 24 & 1));
    }
    if (r.n1b[0] == PB1B_N)
        simplelist_addline("TV clock 1C30: %08lx", (unsigned long)r.v1c30);
    if (rc < 0 && !r.hang)
        simplelist_addline("Failed (%d)", rc);
    return devbcm_show(&info);
}

/* ---- PB-2: the GPIO level census, the owner on cue -------------------- */

enum { S_IDLE, S_MUSIC, S_USB_IN, S_USB_OUT, S_AV, S_PHONES, S_HOLD,
       S_COUNT };
static const char *const pb2_name[S_COUNT] =
{
    "idle", "music", "usb-in", "usb-out", "av-cable", "headphones", "hold",
};
static const char *const pb2_cue[S_COUNT][2] =
{
    { "Idle: nothing plugged in,", "Hold off." },
    { "Music: the iPod plays silence", "itself (no headphones needed)." },
    { "Plug the USB-C cable in", "(charger or computer)." },
    { "Unplug the USB-C cable.", NULL },
    { "Plug the AV cable into the", "headphone jack." },
    { "Unplug the AV cable; plug", "headphones in." },
    { "Unplug headphones. After SELECT", "slide Hold on: sampled in 5 s." },
};

struct pb2_snap
{
    bool done, canary;
    uint32_t w[HP_CENSUS_WORDS];
};

struct pb2_run
{
    int panel;
    struct pb2_snap *s;
};

static int pb2_fn(struct bvp *b, uint32_t fast, void *ctx)
{
    struct pb2_run *r = ctx;
    uint32_t a[4] = { 0xFFFFFFFF, PB_CENSUS_US, 0, 0xFFFFF }, n = 0;
    int st;

    (void)fast;
    r->s->canary = canary(b, "pb2:", "boot", r->panel);
    if (!r->s->canary)
        return -1;
    st = bvp_call(b, HP_OP_PINS, a, 4, r->s->w, HP_CENSUS_WORDS, &n,
                  PB_CENSUS_US + PB_HANG_US);
    if (st != HP_OK || n != HP_CENSUS_WORDS)
    {
        pb_say("pb2: census %s (%d, %lu words)", st < 0 ? "hang" : "refused",
               st, (unsigned long)n);
        return -2;
    }
    r->s->done = true;
    return 0;
}

static uint32_t edges_of(const uint32_t *w, int p)
{
    return w[6 + p / 4] >> (8 * (p % 4)) & 255;
}

static bool lev_and(const uint32_t *w, int p)
{
    return p < 32 ? w[2] >> p & 1 : w[3] >> (p - 32) & 1;
}

static bool lev_or(const uint32_t *w, int p)
{
    return p < 32 ? w[4] >> p & 1 : w[5] >> (p - 32) & 1;
}

static void pb2_log(int k, const uint32_t *w)
{
    int p, len;

    len = snprintf(pb_line, sizeof(pb_line), "pb2: %s and=%08lx,%05lx "
                   "or=%08lx,%05lx edges=", pb2_name[k], (unsigned long)w[2],
                   (unsigned long)w[3], (unsigned long)w[4],
                   (unsigned long)w[5]);
    for (p = 0; p < HP_CENSUS_PINS && len < (int)sizeof(pb_line) - 12; p++)
        if (edges_of(w, p))
            len += snprintf(pb_line + len, sizeof(pb_line) - len, "%d:%lu,",
                            p, (unsigned long)edges_of(w, p));
    devbcm_log(pb_line);
}

/* A pin's signature in a state: level low/high/both, edges or not */
static int sig(const uint32_t *w, int p)
{
    return lev_and(w, p) | lev_or(w, p) << 1 | (edges_of(w, p) != 0) << 2;
}

bool devbcm_pb2(void)
{
    static const char *const lines[] = {
        "Read-only, about 5 min, on cue:",
        "the video chip's 52 pin levels",
        "in 7 states (cables, music,",
        "Hold). Each prompt says what",
        "to do; the iPod charges only." };
    struct simplelist_info info;
    struct pb2_snap snaps[S_COUNT];
    struct pb2_run r;
    int k, p, rc, n = 0, wired = 0;
    bool known = true;

    if (!devbcm_lab_open(&info, "BCM probe PB-2", lines, ARRAYLEN(lines)))
        return false;
    memset(snaps, 0, sizeof(snaps));
    r.panel = -1;
    pb_say("=== BCM probe PB-2 (pin census, read-only): build %s ===",
           rbversion);
#ifdef HAVE_USB_POWER
    usb_set_mode(USB_MODE_CHARGE);      /* a cable in must not take the disk */
#endif
    for (k = 0; k < S_COUNT; k++)
    {
        if (!devbcm_cue("BCM probe PB-2", pb2_cue[k][0], pb2_cue[k][1]))
            break;
        if (k == S_MUSIC)
            devbcm_pcm_silence(true);
        if (k == S_HOLD)
            sleep(5 * HZ);
        r.s = &snaps[k];
        rc = vplayer_probe(read_panel, pb2_fn, &r);
        if (k == S_MUSIC)
            devbcm_pcm_silence(false);
        if (k == S_HOLD)
            devbcm_cue("BCM probe PB-2", "Slide Hold off,", "then SELECT.");
        if (rc || !snaps[k].done)
        {
            pb_say("pb2: %s failed (%d)", pb2_name[k], rc);
            continue;
        }
        pb2_log(k, snaps[k].w);
        n++;
    }
    pb_say("pb2: backlight-off skipped (this build cuts the BCM with the "
           "backlight)");
#ifdef HAVE_USB_POWER
    usb_set_mode(USBMODE_DEFAULT);
#endif
    /* the known pins: 2/3 move only with music, 49 toggles (TE), 50/51
       are the panel ID, 48 is high */
    for (k = 0; k < S_COUNT; k++)
    {
        const uint32_t *w = snaps[k].w;
        bool i2s = edges_of(w, 2) || edges_of(w, 3);

        if (!snaps[k].done)
            continue;
        if (i2s != (k == S_MUSIC) || !edges_of(w, 49) || !lev_and(w, 48) ||
            lev_and(w, 50) != lev_or(w, 50) || lev_and(w, 51) != lev_or(w, 51) ||
            (r.panel >= 0 && (int)(lev_and(w, 51) + 2 * lev_and(w, 50)) !=
             r.panel))
        {
            known = false;
            pb_say("pb2: %s: known pins off (2/3 edges %lu/%lu, 49 %lu, "
                   "48 %d, 50/51 %d%d)", pb2_name[k],
                   (unsigned long)edges_of(w, 2), (unsigned long)edges_of(w, 3),
                   (unsigned long)edges_of(w, 49), lev_and(w, 48),
                   lev_and(w, 50), lev_and(w, 51));
        }
    }
    /* a pin whose signature differs from idle's in exactly one state is
       wired to that state's stimulus */
    if (snaps[S_IDLE].done)
        for (p = 0; p < HP_CENSUS_PINS; p++)
        {
            int moved = -1, count = 0;

            if (p == 2 || p == 3 || p == 49)
                continue;
            for (k = 1; k < S_COUNT; k++)
                if (snaps[k].done &&
                    sig(snaps[k].w, p) != sig(snaps[S_IDLE].w, p))
                {
                    moved = k;
                    count++;
                }
            if (count == 1)
            {
                pb_say("pb2: wired gpio%d by %s", p, pb2_name[moved]);
                if (wired < 6)
                    simplelist_addline("gpio%d: %s", p, pb2_name[moved]);
                wired++;
            }
        }
    pb_say("pb2: %s (%d of %d states, %d wired pin%s)",
           n == S_COUNT && known ? "PASS" : "FAIL", n, S_COUNT, wired,
           wired == 1 ? "" : "s");
    simplelist_addline("PB-2: %s, %d/%d states",
                       n == S_COUNT && known ? "PASS" : "FAIL", n, S_COUNT);
    simplelist_addline("Known pins: %s", known ? "as expected" : "OFF");
    simplelist_addline("Wired pins found: %d", wired);
    return devbcm_show(&info);
}

/* ---- PB-3: the USB block's status bit, the cable on cue --------------- */

struct pb3_run
{
    int panel;
    bool ok;
    uint32_t v[2];              /* 0x10001C3C, 0x10006400 */
};

static int pb3_fn(struct bvp *b, uint32_t fast, void *ctx)
{
    static const uint32_t a[2] = { 0x10001C3C, 0x10006400 };
    struct pb3_run *r = ctx;
    uint32_t o[4];
    char b1[12], b2[12];
    int st;

    (void)fast;
    if (!canary(b, "pb3:", "boot", r->panel))
        return -1;
    st = pk(b, 0, a, 2, o);
    if (st != HP_OK)
    {
        pb_say("pb3: read %s (%d)", st < 0 ? "hang" : "refused", st);
        return -2;
    }
    r->v[0] = o[0];
    r->v[1] = o[2];
    r->ok = true;
    pb_say("pb3: 1c3c %s 6400 %s", two(o[0], o[1], b1), two(o[2], o[3], b2));
    return 0;
}

bool devbcm_pb3(void)
{
    static const char *const lines[] = {
        "Read-only, about 1 min, on cue:",
        "the video chip's USB status bit",
        "with the USB-C cable in, then",
        "out. The iPod charges only." };
    struct simplelist_info info;
    struct pb3_run r[2];
    int k, rc;
    bool both;

    if (!devbcm_lab_open(&info, "BCM probe PB-3", lines, ARRAYLEN(lines)))
        return false;
    memset(r, 0, sizeof(r));
    pb_say("=== BCM probe PB-3 (USB status, read-only): build %s ===",
           rbversion);
#ifdef HAVE_USB_POWER
    usb_set_mode(USB_MODE_CHARGE);
#endif
    for (k = 0; k < 2; k++)
    {
        if (!devbcm_cue("BCM probe PB-3", k ? "Unplug the USB-C cable."
                        : "Plug the USB-C cable in", k ? NULL
                        : "(charger or computer)."))
            break;
        r[k].panel = -1;
        rc = vplayer_probe(read_panel, pb3_fn, &r[k]);
        if (r[k].ok)
            pb_say("pb3: usb=%s step=1 1c3c=%08lx 6400=%08lx (pp sees %s)",
                   k ? "out" : "in", (unsigned long)r[k].v[0],
                   (unsigned long)r[k].v[1], usb_inserted() ? "in" : "out");
        else
            pb_say("pb3: usb=%s failed (%d)", k ? "out" : "in", rc);
    }
#ifdef HAVE_USB_POWER
    usb_set_mode(USBMODE_DEFAULT);
#endif
    both = r[0].ok && r[1].ok;
    pb_say("pb3: step 2 not built (it writes the USB clock gate 1c3c)");
    pb_say("pb3: %s: bit 24 %s", both ? "PASS" : "FAIL",
           !both ? "not measured" :
           (r[0].v[1] ^ r[1].v[1]) >> 24 & 1 ? "follows the cable (wired)"
           : "unchanged (not wired)");
    simplelist_addline("PB-3: %s", both ? "done" : "FAIL");
    for (k = 0; k < 2; k++)
        if (r[k].ok)
            simplelist_addline("USB %s: 6400 %08lx", k ? "out" : "in",
                               (unsigned long)r[k].v[1]);
    if (both)
        simplelist_addline("Bit 24: %s", (r[0].v[1] ^ r[1].v[1]) >> 24 & 1 ?
                           "follows the cable" : "unchanged");
    return devbcm_show(&info);
}

/* ---- PB-4: the GPIO pull test ----------------------------------------- */

struct pb4_run
{
    int panel;
    int counts[5];              /* float, high, low, volatile, skipped */
    int sent, answered;
    uint32_t ctl[2];            /* GPIO 50/51 levels (no pull) at the end */
};

static int pb4_fn(struct bvp *b, uint32_t fast, void *ctx)
{
    static const char *const cls[4] = { "float", "high", "low", "volatile" };
    struct pb4_run *r = ctx;
    uint32_t a[3], o[HP_PULL_MAX], n, first, i, w;
    int st;

    (void)fast;
    if (!canary(b, "pb4:", "boot", r->panel))
        return -1;
    for (first = 0; first < 48; first += HP_PULL_MAX)
    {
        a[0] = HP_PULL_MAGIC;
        a[1] = first;
        a[2] = HP_PULL_MAX;
        r->sent++;
        st = bvp_call(b, HP_OP_PULL, a, 3, o, HP_PULL_MAX, &n, PB_PULL_US);
        if (st != HP_OK || n != HP_PULL_MAX)
        {
            pb_say("pb4: pins %lu-%lu %s (%d)", (unsigned long)first,
                   (unsigned long)first + 7, st < 0 ? "hang" : "refused", st);
            return -2;
        }
        r->answered++;
        for (i = 0; i < n; i++)
        {
            uint32_t up, down;
            int c;

            w = o[i];
            if (w & (HP_PULL_SKIP_PIN | HP_PULL_SKIP_FSEL))
            {
                pb_say("pb4: gpio%lu skipped (%s)", (unsigned long)(w & 0xFF),
                       w & HP_PULL_SKIP_PIN ? "never pulled"
                                            : "not an input");
                r->counts[4]++;
                continue;
            }
            up = HP_PULL_UP(w);
            down = HP_PULL_DOWN(w);
            c = (up == 1 || up == 2 || down == 1 || down == 2) ? 3 :
                up == 3 && down == 0 ? 0 : up == 3 ? 1 : down == 0 ? 2 : 3;
            r->counts[c]++;
            pb_say("pb4: gpio%lu up=%lu,%lu down=%lu,%lu -> %s",
                   (unsigned long)(w & 0xFF), (unsigned long)(up & 1),
                   (unsigned long)(up >> 1), (unsigned long)(down & 1),
                   (unsigned long)(down >> 1), cls[c]);
        }
    }
    /* the control: 50/51 (straps, never pulled) read, as the panel ID */
    a[0] = A_LEV1;
    if (pk(b, 0, a, 1, o) == HP_OK)
    {
        r->ctl[0] = o[0];
        r->ctl[1] = o[1];
        pb_say("pb4: control gpio50=%lu gpio51=%lu (no pull), panel %d "
               "(host %d)", (unsigned long)(o[0] >> 18 & 1),
               (unsigned long)(o[0] >> 19 & 1), panel_of(o[0]), r->panel);
    }
    return 0;
}

bool devbcm_pb4(void)
{
    static const char *const lines[] = {
        "Under a minute, hands off. It",
        "WRITES the video chip's weak",
        "pulls: up for 1 ms, then down",
        "(as at every boot), one input",
        "at a time; never 1-3, 19, 23,",
        "48-51; nothing driven." };
    struct simplelist_info info;
    struct pb4_run r;
    int rc;
    bool pass;

    if (!devbcm_lab_open(&info, "BCM probe PB-4", lines, ARRAYLEN(lines)))
        return false;
    memset(&r, 0, sizeof(r));
    r.panel = -1;
    pb_say("=== BCM probe PB-4 (pull test): build %s ===", rbversion);
    rc = vplayer_probe(read_panel, pb4_fn, &r);
    pass = rc == 0 && r.answered == r.sent && r.sent == 6 &&
           r.ctl[0] == r.ctl[1] && panel_of(r.ctl[0]) == r.panel;
    pb_say("pb4: %s (rc %d): float %d, high %d, low %d, volatile %d, "
           "skipped %d", pass ? "PASS" : "FAIL", rc, r.counts[0],
           r.counts[1], r.counts[2], r.counts[3], r.counts[4]);
    simplelist_addline("PB-4: %s", pass ? "PASS" : "FAIL");
    simplelist_addline("Float %d, high %d, low %d", r.counts[0], r.counts[1],
                       r.counts[2]);
    simplelist_addline("Volatile %d, skipped %d", r.counts[3], r.counts[4]);
    simplelist_addline("Control 50/51: %s", r.ctl[0] == r.ctl[1] &&
                       panel_of(r.ctl[0]) == r.panel ? "steady" : "OFF");
    if (rc)
        simplelist_addline("Failed (%d)", rc);
    return devbcm_show(&info);
}

/* ---- PW1: the power census -------------------------------------------- */

static const uint32_t pw1_regs[] =
{
    0x10000C00, 0x10000400, 0x10000408, 0x10003410, 0x10003414,
    0x10003420, 0x10003424, 0x10003428, 0x10003430,
};

struct pw1_run
{
    int contexts, panel;
    bool fail;
    int32_t ctr[3];             /* the counter's delta per context */
    uint32_t peek_diff;         /* B and C: words the port and PEEK read
                                   differently */
    uint32_t vals[24 + 9];      /* the last context's port reads */
};

static const char *src_name(uint32_t off, uint32_t src)
{
    static const char *const n[8] = { "none", "PLL B", "PLL C", "PLL D",
                                      "src 4", "crystal", "src 6", "PLL A" };

    if (off == 0x28 && src == 0)
        return "crystal";
    if (off == 0x58 && src == 1)
        return "PLL A or crystal";
    return n[src & 7];
}

static bool pw1_rd(uint32_t a, uint32_t *v, struct pw1_run *r)
{
    if (bcm_video_rd(a, v) < 0 || *v == 0xFFFFFFFF)
    {
        pb_say("pw1 %08lx: %s", (unsigned long)a,
               *v == 0xFFFFFFFF ? "dead port" : "port timeout");
        r->fail = true;
        return false;
    }
    return true;
}

/* One context's reads through the port (spec steps 1-6); the values in
 * vals[] for the PEEK cross-check, in pw1_list() order */
static uint32_t pw1_list(int i)
{
    if (i == 0)
        return 0x10001400;
    if (i <= 23)
        return 0x10001C00 + 4 * (i - 1);
    return pw1_regs[i - 24];
}
#define PW1_N   (24 + (int)ARRAYLEN(pw1_regs))

static int pw1_context(char ctx, struct pw1_run *r, uint32_t *vals)
{
    uint32_t v, c0, c1, pp[5];
    unsigned long t0;
    int i;

    for (i = 0; i < PW1_N; i++)
    {
        uint32_t a = pw1_list(i), off = a - 0x10001C00;

        if (!pw1_rd(a, &v, r))
            return -1;
        vals[i] = v;
        if (a == 0x10001400)
            pb_say("pw1 %c reg 10001400 = %08lX (level %lu, hi %lX)", ctx,
                   (unsigned long)v, (unsigned long)(v & 15),
                   (unsigned long)(v >> 4 & 15));
        else if (off >= 0x28 && off <= 0x58)
            pb_say("pw1 %c clk +%02lx = %08lX src %lu (%s) div %lu%s", ctx,
                   (unsigned long)off, (unsigned long)v,
                   (unsigned long)(v >> 8 & 7), src_name(off, v >> 8 & 7),
                   (unsigned long)(v & 0xFF),
                   (v & 0x7FF) == 0 ? " (off)" : "");
        else
            pb_say("pw1 %c reg %08lx = %08lX", ctx, (unsigned long)a,
                   (unsigned long)v);
    }
    /* the PLLs: divider +0x00-0x0C, control +0x14-0x20 ([9:0] N, bit 10 on) */
    for (i = 0; i < 4; i++)
    {
        uint32_t m = vals[1 + i], n = vals[1 + 5 + i];
        uint32_t khz = m ? (uint32_t)(27000ull * (n & 0x3FF) / m) : 0;

        pb_say("pw1 %c pll +%02x%s m=%08lX n=%08lX %s f=%lu.%lu MHz", ctx,
               4 * i, i == 0 ? " (A)" : "", (unsigned long)m, (unsigned long)n,
               n & 0x400 ? "on" : "off", (unsigned long)khz / 1000,
               (unsigned long)khz % 1000 / 100);
    }
    /* step 5: the counter, 1,000 us apart on the PP's timer */
    t0 = USEC_TIMER;
    if (!pw1_rd(A_COUNTER, &c0, r))
        return -1;
    while (USEC_TIMER - t0 < 1000)
        ;
    if (!pw1_rd(A_COUNTER, &c1, r))
        return -1;
    r->ctr[r->contexts] = (int32_t)(c1 - c0);
    pb_say("pw1 %c ctr delta %ld us", ctx, (long)(int32_t)(c1 - c0));
    /* step 6: the PP's side */
    bcm_video_pp_regs(pp);
    pb_say("pw1 %c gpo32 val=%08lx en=%08lx rb=%08lx strap=%08lx 40=%08lx",
           ctx, (unsigned long)pp[0], (unsigned long)pp[1],
           (unsigned long)pp[2], (unsigned long)pp[3], (unsigned long)pp[4]);
    return 0;
}

/* Context A: what the take found (Apple's vmcs, or our resident image:
 * then the context is logged R), with step 7's samples */
static int pw1_pre(void *ctx)
{
    struct pw1_run *r = ctx;
    char c = bcm_video_took_ours() ? 'R' : 'A';
    uint32_t lo[3] = { ~0u, ~0u, ~0u }, hi[3] = { 0, 0, 0 }, v;
    static const uint32_t sa[3] = { 0x10001400, 0x10001C28, 0x10001C14 };
    int i, k;

    if (read_panel(&r->panel) < 0 || pw1_context(c, r, r->vals) < 0)
        return -1;
    r->contexts++;
    if (c == 'A')
    {
        /* 200 samples, 10 ms apart (the scripted scroll is not built:
           the display driver is stopped while the BCM is held) */
        for (i = 0; i < 200; i++)
        {
            for (k = 0; k < 3; k++)
            {
                if (!pw1_rd(sa[k], &v, r))
                    return -1;
                if (v < lo[k])
                    lo[k] = v;
                if (v > hi[k])
                    hi[k] = v;
            }
            sleep(HZ / 100);
        }
        pb_say("pw1 A samples (idle, 2 s): level %lu..%lu, sel %04lx..%04lx, "
               "pllA %04lx..%04lx", (unsigned long)(lo[0] & 15),
               (unsigned long)(hi[0] & 15), (unsigned long)lo[1],
               (unsigned long)hi[1], (unsigned long)lo[2],
               (unsigned long)hi[2]);
    }
    return 0;
}

/* B and C: the same reads by PEEK (the core's view), against the port's */
static void pw1_peek_check(struct bvp *b, char ctx, struct pw1_run *r)
{
    uint32_t a[HP_PEEK_MAX], o[2 * HP_PEEK_MAX];
    int i, k, n;

    for (i = 0; i < PW1_N; i += n)
    {
        n = MIN(HP_PEEK_MAX, PW1_N - i);
        for (k = 0; k < n; k++)
            a[k] = pw1_list(i + k);
        if (pk(b, 0, a, n, o) != HP_OK)
        {
            pb_say("pw1 %c peek failed at %08lx", ctx, (unsigned long)a[0]);
            r->fail = true;
            return;
        }
        for (k = 0; k < n; k++)
            if (o[2 * k] != r->vals[i + k] && a[k] != A_COUNTER)
            {
                r->peek_diff++;
                pb_say("pw1 %c port/peek differ %08lx: port %08lx, core "
                       "%08lx", ctx, (unsigned long)a[k],
                       (unsigned long)r->vals[i + k],
                       (unsigned long)o[2 * k]);
            }
    }
}

static int pw1_fn(struct bvp *b, uint32_t fast, void *ctx)
{
    struct pw1_run *r = ctx;

    if (pw1_context('B', r, r->vals) < 0)
        return -1;
    pw1_peek_check(b, 'B', r);
    r->contexts++;
    if (fast)
    {
        if (bvp_open(b, 320, 240, fast) != HP_OK)
        {
            pb_say("pw1 C: OPEN at 150 MHz failed: no context C");
            return 0;
        }
        if (pw1_context('C', r, r->vals) == 0)
        {
            pw1_peek_check(b, 'C', r);
            r->contexts++;
        }
        bvp_close(b, 0);
    }
    return 0;
}

bool devbcm_pw1(void)
{
    static const char *const lines[] = {
        "Read-only, under a minute,",
        "hands off: the video chip's",
        "voltage, clock and pin words",
        "under Apple's firmware and ours." };
    struct simplelist_info info;
    struct pw1_run r;
    int rc, k;
    bool pass;

    if (!devbcm_lab_open(&info, "BCM probe PW1", lines, ARRAYLEN(lines)))
        return false;
    memset(&r, 0, sizeof(r));
    r.panel = -1;
    pb_say("=== BCM probe PW1 (power census, read-only): build %s ===",
           rbversion);
    rc = vplayer_probe(pw1_pre, pw1_fn, &r);
    pass = rc == 0 && !r.fail && r.contexts >= 2;
    for (k = 1; k < r.contexts; k++)        /* B and C: 1,000 +- 5 us */
        if (r.ctr[k] < 995 || r.ctr[k] > 1005)
            pass = false;
    pwh_note_pw1(&hib_lock, pass);          /* PW5's lock */
    pb_say("pw1: %s (%d contexts, rc %d, port/peek differ %lu)",
           pass ? "PASS" : "FAIL", r.contexts, rc,
           (unsigned long)r.peek_diff);
    simplelist_addline("PW1: %s, %d contexts", pass ? "PASS" : "FAIL",
                       r.contexts);
    for (k = 0; k < r.contexts; k++)
        simplelist_addline("Counter %c: %ld us in 1000", "ABC"[k],
                           (long)r.ctr[k]);
    simplelist_addline("Port/PEEK differ: %lu", (unsigned long)r.peek_diff);
    if (rc)
        simplelist_addline("Failed (%d)", rc);
    return devbcm_show(&info);
}

/* ---- PW2: caches, timing and coherence -------------------------------- */

#define PW2_SDRAM_HOST(a)   ((a) - 0xFE000000u + 0xC0000000u)
static const uint32_t pw2_cc[3] = { 0, 0x80000000u, 0x80010000u };
static const char *const pw2_rname[3] = { "SRAM16K", "SDRAM16K", "SDRAM1M" };

struct pw2_run
{
    bool sums_bad, fail;
    uint32_t want[3];
    uint32_t us[3][3][2];       /* [cc][region][cold, warm] */
    int coh[3][2][3];           /* [cc][mem][core, host, flush]: 1 fresh */
};

/* Fill a region by the port with the LCG pattern; the model's sum */
static int pw2_fill(uint32_t host, uint32_t bytes, uint32_t stride_w,
                    uint32_t *sum)
{
    uint32_t buf[256], x = 0x2722C0DEu ^ host, i, k, s = 0;

    for (i = 0; i < bytes / 4; i += 256)
    {
        for (k = 0; k < 256; k++)
        {
            x = x * 1664525u + 1013904223u;
            buf[k] = x;
            if (((i + k) % stride_w) == 0)
                s += x;
        }
        if (bcm_video_wr_block(host + 4 * i, (const uint16_t *)buf, 512) < 0)
            return -1;
    }
    if (bcm_video_flush() < 0)
        return -1;
    /* 262,144 loads: each word in a 16 KB region 64 times, each 32-byte
       line's first word of the 1 MB one 8 times */
    *sum = s * (HP_CACHE_LOADS / (bytes / 4 / stride_w));
    return 0;
}

static int pw2_coh(struct bvp *b, uint32_t cc, int mem, int res[3],
                   uint32_t *to)
{
    uint32_t a[4] = { HP_CACHE_MAGIC, HP_CACHE_COH, cc, mem }, out[3], n = 0;
    uint32_t addr = mem ? PW2_SDRAM_HOST(HP_CACHE_COH_SDRAM)
                        : HP_CACHE_COH_SRAM;
    uint32_t s, v, t0;
    int tag, st, next = 1;

    /* the step word from a known state: SRAM holds anything after power-up */
    if (bcm_video_wr(HP_CACHE_STEP, 0) < 0 ||
        bvp_send(b, HP_OP_CACHE, a, 4, &tag))
        return -1;
    t0 = bcm_video_now_us();
    while (next <= 5)
    {
        if (bcm_video_rd(HP_CACHE_STEP, &s) < 0)
            return -1;
        if (s == (uint32_t)next)
        {
            if (next == 1)
                bcm_video_wr(addr, HP_CACHE_V1);
            else if (bcm_video_rd(addr + 4, &v) == 0)
                res[next == 3 ? 1 : 2] = v == HP_CACHE_V2;
            bcm_video_wr(HP_CACHE_STEP, next + 1);
            next += 2;
            t0 = bcm_video_now_us();
        }
        else if (bcm_video_now_us() - t0 > HP_CACHE_STEP_US)
        {
            pb_say("pw2 coh: step %d never came (step word %08lx)", next,
                   (unsigned long)s);
            break;
        }
    }
    t0 = bcm_video_now_us();
    for (;;)
    {
        if (bvp_poll(b) < 0)
            return -1;
        st = bvp_take(b, tag, out, 3, &n);
        if (st != BVP_AGAIN)
            break;
        if (bcm_video_now_us() - t0 > PB_HANG_US)
            return -2;
        yield();
    }
    if (st != HP_OK || n < 3)
        return st ? st : -3;
    res[0] = out[0] == HP_CACHE_V1;
    *to = out[1];
    return 0;
}

static int pw2_fn(struct bvp *b, uint32_t fast, void *ctx)
{
    static const uint32_t host[3] = {
        HP_CACHE_SRAM, PW2_SDRAM_HOST(HP_CACHE_SDRAM),
        PW2_SDRAM_HOST(HP_CACHE_SDRAM1M) };
    static const uint32_t bytes[3] = { 16384, 16384, 1u << 20 };
    static const uint32_t stride[3] = { 1, 1, 8 };
    static const char *const fr[2] = { "stale", "fresh" };
    struct pw2_run *r = ctx;
    uint32_t a[4], o[5], n, to;
    int c, k, m, st;

    (void)fast;
    if (bvp_open(b, 320, 240, 0) != HP_OK)     /* SDRAM up, 27 MHz */
    {
        pb_say("pw2: OPEN failed");
        return -1;
    }
    for (k = 0; k < 3; k++)
        if (pw2_fill(host[k], bytes[k], stride[k], &r->want[k]) < 0)
        {
            pb_say("pw2: fill %s failed", pw2_rname[k]);
            return -2;
        }
    for (c = 0; c < 3; c++)
    {
        for (k = 0; k < 3; k++)
        {
            a[0] = HP_CACHE_MAGIC;
            a[1] = HP_CACHE_TIME;
            a[2] = pw2_cc[c];
            a[3] = k;
            st = bvp_call(b, HP_OP_CACHE, a, 4, o, 5, &n, 4 * PB_HANG_US);
            if (st != HP_OK || n < 5)
            {
                pb_say("pw2 cc=%08lx %s: %d (word %08lx)",
                       (unsigned long)pw2_cc[c], pw2_rname[k], st,
                       (unsigned long)(n ? o[0] : 0));
                r->fail = true;
                return -3;
            }
            r->us[c][k][0] = o[0];
            r->us[c][k][1] = o[1];
            pb_say("pw2 cc=%08lx %s cold %lu us warm %lu us, sums %s, flush "
                   "polls %lu", (unsigned long)pw2_cc[c], pw2_rname[k],
                   (unsigned long)o[0], (unsigned long)o[1],
                   o[2] == r->want[k] && o[3] == r->want[k] ? "ok" : "WRONG",
                   (unsigned long)o[4]);
            if (o[2] != r->want[k] || o[3] != r->want[k])
            {
                r->sums_bad = true;     /* the cache corrupted data: cut */
                return -4;
            }
        }
        for (m = 0; m < 2; m++)
        {
            int *res = r->coh[c][m];

            res[0] = res[1] = res[2] = 0;
            to = 0;
            st = pw2_coh(b, pw2_cc[c], m, res, &to);
            if (st)
            {
                pb_say("pw2 coh cc=%08lx %s: failed (%d)",
                       (unsigned long)pw2_cc[c], m ? "SDRAM" : "SRAM", st);
                r->fail = true;
                return -5;
            }
            pb_say("pw2 coh cc=%08lx %s core-after-host %s, host-after-core "
                   "%s, after-flush %s (timeouts %lx)",
                   (unsigned long)pw2_cc[c], m ? "SDRAM" : "SRAM",
                   fr[res[0]], fr[res[1]], fr[res[2]], (unsigned long)to);
        }
    }
    bvp_close(b, 0);
    return 0;
}

bool devbcm_pw2(void)
{
    static const char *const lines[] = {
        "About 1 min, hands off. It",
        "WRITES the video chip's cache",
        "word, Apple's three values",
        "only, and puts ours back." };
    struct simplelist_info info;
    struct pw2_run r;
    int rc, c, k;
    bool pass;

    if (!devbcm_lab_open(&info, "BCM probe PW2", lines, ARRAYLEN(lines)))
        return false;
    memset(&r, 0, sizeof(r));
    pb_say("=== BCM probe PW2 (caches, Apple's values): build %s ===",
           rbversion);
    rc = vplayer_probe(NULL, pw2_fn, &r);
    pass = rc == 0 && !r.sums_bad && !r.fail;
    pb_say("pw2: %s (rc %d)", pass ? "PASS" : "FAIL", rc);
    simplelist_addline("PW2: %s%s", pass ? "PASS" : "FAIL",
                       r.sums_bad ? " (data corrupted)" : "");
    for (c = 0; c < 3 && rc == 0; c++)
    {
        simplelist_addline("cc %08lx:", (unsigned long)pw2_cc[c]);
        for (k = 0; k < 3; k++)
            simplelist_addline(" %s %lu/%lu us", pw2_rname[k],
                               (unsigned long)r.us[c][k][0],
                               (unsigned long)r.us[c][k][1]);
        simplelist_addline(" coherent: SRAM %d%d%d SDRAM %d%d%d",
                           r.coh[c][0][0], r.coh[c][0][1], r.coh[c][0][2],
                           r.coh[c][1][0], r.coh[c][1][1], r.coh[c][1][2]);
    }
    if (rc)
        simplelist_addline("Failed (%d)", rc);
    return devbcm_show(&info);
}

/* ---- PW7: the ALT bank ------------------------------------------------- */

static int pw7_fn(struct bvp *b, uint32_t fast, void *ctx)
{
    (void)b;
    (void)fast;
    return bcm_video_alt_lab(ctx);
}

bool devbcm_pw7(void)
{
    static const char *const lines[] = {
        "Under a minute, hands off. It",
        "WRITES free video chip memory",
        "(4 KB) and its frame through",
        "the second port bank, then",
        "reads it back by both banks." };
    struct simplelist_info info;
    struct bcm_alt_result r;
    int rc, k;
    bool pass;

    if (!devbcm_lab_open(&info, "BCM probe PW7", lines, ARRAYLEN(lines)))
        return false;
    memset(&r, 0, sizeof(r));
    pb_say("=== BCM probe PW7 (the ALT bank): build %s ===", rbversion);
    rc = vplayer_probe(NULL, pw7_fn, &r);
    pb_say("pw7 main N/256, alt N/256, interleave main N/256 alt N/256, by "
           "the main bank: %lu %lu %lu %lu; by the ALT bank: %lu %lu %lu "
           "%lu; alt ready seen %s, frame main %lu us alt %lu us",
           (unsigned long)r.main_ok[0], (unsigned long)r.main_ok[1],
           (unsigned long)r.main_ok[2], (unsigned long)r.main_ok[3],
           (unsigned long)r.alt_ok[0], (unsigned long)r.alt_ok[1],
           (unsigned long)r.alt_ok[2], (unsigned long)r.alt_ok[3],
           r.alt_ready ? "yes" : "no", (unsigned long)r.frame_us[0],
           (unsigned long)r.frame_us[1]);
    pass = rc == 0;
    for (k = 0; k < 4; k++)
        if (r.main_ok[k] != 256 || r.alt_ok[k] != 256)
            pass = false;
    pb_say("pw7: %s (rc %d, step %d%s)", pass ? "PASS" : "FAIL", rc,
           r.fail_step, r.dead ? ", dead port" : "");
    simplelist_addline("PW7: %s", pass ? "PASS" : "FAIL");
    simplelist_addline("Main read: %lu %lu %lu %lu",
                       (unsigned long)r.main_ok[0], (unsigned long)r.main_ok[1],
                       (unsigned long)r.main_ok[2], (unsigned long)r.main_ok[3]);
    simplelist_addline("ALT read: %lu %lu %lu %lu",
                       (unsigned long)r.alt_ok[0], (unsigned long)r.alt_ok[1],
                       (unsigned long)r.alt_ok[2], (unsigned long)r.alt_ok[3]);
    simplelist_addline("ALT ready seen: %s", r.alt_ready ? "yes" : "no");
    simplelist_addline("Frame: main %lu us, ALT %lu us",
                       (unsigned long)r.frame_us[0],
                       (unsigned long)r.frame_us[1]);
    if (rc)
        simplelist_addline("Failed at step %d (%d)", r.fail_step, rc);
    return devbcm_show(&info);
}

/* ---- PW8: DMA copies --------------------------------------------------- */

struct pw8_run
{
    uint32_t o[HP_DMACPY_CASES][6];
    bool got[HP_DMACPY_CASES];
};

static const char *const pw8_name[HP_DMACPY_CASES] =
{
    "SRAM>SRAM", "SRAM>SDRAM", "SDRAM>SRAM",
};

static int pw8_fn(struct bvp *b, uint32_t fast, void *ctx)
{
    struct pw8_run *r = ctx;
    uint32_t a[3], n;
    int k, st;

    (void)fast;
    if (bvp_open(b, 320, 240, 0) != HP_OK)     /* SDRAM up */
    {
        pb_say("pw8: OPEN failed");
        return -1;
    }
    for (k = 0; k < HP_DMACPY_CASES; k++)
    {
        a[0] = HP_DMACPY_MAGIC;
        a[1] = k;
        a[2] = 0x2722 + k;
        st = bvp_call(b, HP_OP_DMACPY, a, 3, r->o[k], 6, &n, PB_HANG_US);
        if (st != HP_OK || n < 6)
        {
            pb_say("pw8 %s: %s (%d)", pw8_name[k], st < 0 ? "hang" : "refused",
                   st);
            return -2;
        }
        r->got[k] = true;
        pb_say("pw8 %s dma %lu us core %lu us, wrong %lu (core %lu), status "
               "%08lx%s", pw8_name[k], (unsigned long)r->o[k][0],
               (unsigned long)r->o[k][1], (unsigned long)r->o[k][2],
               (unsigned long)r->o[k][3], (unsigned long)r->o[k][4],
               r->o[k][5] ? ", DMA TIMED OUT" : "");
    }
    bvp_close(b, 0);
    return 0;
}

bool devbcm_pw8(void)
{
    static const char *const lines[] = {
        "Under a minute, hands off. The",
        "video chip's DMA channel 7",
        "WRITES 16 KB copies into its",
        "free memory, then checks them." };
    struct simplelist_info info;
    struct pw8_run r;
    int rc, k;
    bool pass;

    if (!devbcm_lab_open(&info, "BCM probe PW8", lines, ARRAYLEN(lines)))
        return false;
    memset(&r, 0, sizeof(r));
    pb_say("=== BCM probe PW8 (DMA copies, channel 7): build %s ===",
           rbversion);
    rc = vplayer_probe(NULL, pw8_fn, &r);
    pass = rc == 0;
    for (k = 0; k < HP_DMACPY_CASES; k++)
        if (!r.got[k] || r.o[k][2] || r.o[k][3] || r.o[k][5])
            pass = false;
    pb_say("pw8: %s (rc %d)", pass ? "PASS" : "FAIL", rc);
    simplelist_addline("PW8: %s", pass ? "PASS" : "FAIL");
    for (k = 0; k < HP_DMACPY_CASES; k++)
        if (r.got[k])
            simplelist_addline("%s %lu/%lu us, %lu wrong", pw8_name[k],
                               (unsigned long)r.o[k][0],
                               (unsigned long)r.o[k][1],
                               (unsigned long)r.o[k][2]);
    if (rc)
        simplelist_addline("Failed (%d)", rc);
    return devbcm_show(&info);
}
/* ---- PB-6: Apple's own TV pattern (devbcm_pb6.h; bench-p19.md card PB-6)
 *
 * Host side only, on Apple's NOR vmcs (the image the stock wake path
 * loads; ours is dropped first if it drives the display): a colour-bar
 * BMP to 0xC020_0000, TV_NTSCBMP or TV_PALBMP, TV_MVOFF, a 10 s hold,
 * then a power cut and the stock wake (bcm_devtest_tv_pattern()).  The
 * owner answers on the lit screen after the wake.  Not locked: it runs
 * before PB-7, and "bars" says PB-8 is worth running. */
bool devbcm_pb6(void)
{
    static const char *const lines[] = {
        "Needs the AV cable in the",
        "headphone jack (no headphones)",
        "and a TV on its AV input.",
        "The video chip's own firmware",
        "DRIVES the TV output with",
        "colour bars for 10 s (SELECT",
        "ends it), then you say what",
        "the TV showed." };
    static const char *const stds[2] = { "NTSC", "PAL" };
    static const char *const saw[PB6_SAWS] =
    {
        "Colour bars", "A grey or blank picture", "No signal",
        "Snow or a rolling picture",
    };
    struct simplelist_info info;
    struct bcm_tvpat_result r;
    enum pb6_cmd cmd = PB6_TIMEOUT, mv = PB6_TIMEOUT;
    int std, h, seen = -1, rc;
    uint32_t bytes;
    void *buf;

    if (!devbcm_confirm(&info, "BCM probe PB-6", lines, ARRAYLEN(lines)))
        return false;
    std = devbcm_choose("BCM probe PB-6", "Which standard is your TV?",
                        "Watch the TV after SELECT.", stds, 2);
    if (std < 0)
        return false;
    simplelist_info_init(&info, "BCM probe PB-6", 0, NULL);
    simplelist_reset_lines();
    h = core_alloc(PB6_BUF + 4);
    if (h <= 0)
    {
        pb_say("pb6: std=%s not run: no memory for the BMP",
               std ? "pal" : "ntsc");
        simplelist_addline("Not available: no memory");
        return devbcm_show(&info);
    }
    core_pin(h);                    /* the port reads it in place */
    buf = (void *)ALIGN_UP((uintptr_t)core_get_data(h), 4);
    bytes = pb6_bmp_build(buf, PB6_BUF);
    pb_say("=== BCM probe PB-6 (Apple's TV pattern, %s, %d ms): build %s ===",
           std ? "pal" : "ntsc", PB6_HOLD_MS, rbversion);
    rc = bcm_devtest_tv_pattern(buf, bytes, std == 1, PB6_CMD_US,
                                PB6_HOLD_MS, &r);
    core_unpin(h);
    core_free(h);
    if (r.forced)
        pb_say("pb6: our image drove the display: Apple's vmcs for the "
               "rest of this boot");
    if (rc < 0 || !r.ran)
    {
        pb_say("pb6: std=%s not run: %s (rc %d, upload %d, head %08lx "
               "%08lx)", std ? "pal" : "ntsc",
               rc == -1 ? "the display driver did not give up the chip" :
               rc == -3 ? "our image could not be dropped" :
               rc < 0 ? "bad arguments" : "the BMP upload failed",
               rc, r.upload, (unsigned long)r.head[0],
               (unsigned long)r.head[1]);
        simplelist_addline("PB-6: not run (%d)", rc < 0 ? rc : r.upload);
        return devbcm_show(&info);
    }
    cmd = pb6_cmd_verdict(r.cmd_port, pb6_bmp_cmd(std == 1), r.cmd_st,
                          r.cmd_us);
    if (r.cmd_port)
        mv = pb6_cmd_verdict(r.mv_port, PB6_CMD(PB6_IDX_MVOFF), r.mv_st,
                             r.mv_us);
    seen = devbcm_choose("BCM probe PB-6", "What did the TV show?",
                         stds[std], saw, PB6_SAWS);
    pb_say("pb6: head %08lx %08lx (%s); bmp 0x1F8=%08lx in %lu us; mvoff "
           "%s 0x1F8=%08lx in %lu us; held %lu ms",
           (unsigned long)r.head[0], (unsigned long)r.head[1],
           pb6_head_ok(r.head[0], r.head[1]) ? "ok" : "MISMATCH",
           (unsigned long)r.cmd_st, (unsigned long)r.cmd_us,
           r.cmd_port ? pb6_cmd_name(mv) : "not sent",
           (unsigned long)r.mv_st, (unsigned long)r.mv_us,
           (unsigned long)r.held_ms);
    pb_say("pb6: std=%s cmd=%s owner=%s", std ? "pal" : "ntsc",
           pb6_cmd_name(cmd), pb6_saw_name(seen));
    pb_say("pb6: %s", pb6_verdict_name(pb6_verdict(cmd, seen)));
    simplelist_info_init(&info, "BCM probe PB-6", 0, NULL);
    simplelist_reset_lines();
    simplelist_addline("PB-6: %s", stds[std]);
    simplelist_addline("Command: %s", pb6_cmd_name(cmd));
    simplelist_addline("TV showed: %s", pb6_saw_name(seen));
    simplelist_addline("%s", seen == PB6_SAW_BARS ? "PB-8 is worth running"
                                                  : "See the log for next");
    return devbcm_show(&info);
}

/* ---- PB-7 to PB-9: TV out (devbcm_tv.h; bench-p19.md cards PB-7-PB-9) --
 *
 * HP_OP_TV in the video image: an UNLOCK for a one-time code, then one
 * TIMING or OUTPUT step with it.  The step blocks the image's loop for its
 * window, so the PP waits for the window plus a margin (not the 2 s of the
 * other probes) and calls it a hang only when neither the progress word
 * (HP_TV_PROGRESS) nor the heartbeat has moved for 2 s.  Nothing touches
 * the LCD in the window: the BCM holds the display from the boot to the
 * power cut, and every prompt is on the lit screen before or after.  The
 * image puts back every register it wrote; vplayer_probe() then cuts the
 * BCM's power, frees the image and brings the display back.  PB-8 opens
 * once PB-7 passed in this boot, PB-9 once a PB-8 arm showed bars. */
#define TV_REQS 2

static struct tvp_lock tv_lock = { false, -1 };    /* this boot only */

struct tv_req
{
    bool out;
    uint32_t std, fmt, w704, pat, ms;
};

struct tv_ctx
{
    const char *pfx;            /* the log's prefix: "pb7:" ... */
    int panel, n;
    struct tv_req q[TV_REQS];
    struct tvp_reply r[TV_REQS];
    bool ran[TV_REQS];          /* the step request went out */
};

static const char *tv_what(const struct tv_ctx *c, int k, char *buf)
{
    const struct tv_req *q = &c->q[k];

    if (!q->out)
        snprintf(buf, 40, "std=%s", tvp_std_name(q->std));
    else
        snprintf(buf, 40, "std=%s fmt=%s in=%d %s", tvp_std_name(q->std),
                 tvp_fmt_name(q->fmt), q->w704 ? 704 : 640,
                 q->pat == HP_TV_PAT_LINES ? "lines" : "bars");
    return buf;
}

/* Every reply word, in three lines */
static void tv_log(const struct tv_ctx *c, int k)
{
    const struct tvp_reply *r = &c->r[k];
    const uint32_t *w = r->w;
    char what[40];
    int i, j, len;

    tv_what(c, k, what);
    pb_say("%s %s st=%d words=%lu exit=%lu phase=%lu: %s", c->pfx, what,
           r->status, (unsigned long)r->n, (unsigned long)w[HP_TV_R_EXIT],
           (unsigned long)w[HP_TV_R_PHASE], tvp_check_name(tvp_check(r)));
    pb_say("%s %s dac0=%08lx mode0=%08lx fields=%lu us=%lu clk=%08lx "
           "flags=%08lx mismatch=%03lx", c->pfx, what,
           (unsigned long)w[HP_TV_R_DAC0], (unsigned long)w[HP_TV_R_MODE0],
           (unsigned long)w[HP_TV_R_FIELDS], (unsigned long)w[HP_TV_R_US],
           (unsigned long)w[HP_TV_R_CLK], (unsigned long)w[HP_TV_R_FLAGS],
           (unsigned long)w[HP_TV_R_MISMATCH]);
    for (i = 0; i < HP_TV_SAMPLES; i += HP_TV_SAMPLES / 2)
    {
        len = snprintf(pb_line, sizeof(pb_line), "%s %s ctl/dma %d-%d:",
                       c->pfx, what, i, i + HP_TV_SAMPLES / 2 - 1);
        for (j = i; j < i + HP_TV_SAMPLES / 2 &&
                    len < (int)sizeof(pb_line) - 20; j++)
            len += snprintf(pb_line + len, sizeof(pb_line) - len,
                            " %08lx/%08lx",
                            (unsigned long)w[HP_TV_R_SAMPLES + 2 * j],
                            (unsigned long)w[HP_TV_R_SAMPLES + 2 * j + 1]);
        devbcm_log(pb_line);
    }
}

/* One step: UNLOCK, then the request, waited on with the watchdog.
 * 0 with a reply in c->r[k] (HP_OK or a refusal), < 0 without one */
static int tv_step(struct bvp *b, struct tv_ctx *c, int k)
{
    const struct tv_req *q = &c->q[k];
    struct tvp_reply *r = &c->r[k];
    struct tvp_dog d;
    uint32_t a[8], code = 0, n = 0, t0, now, look, prog = 0, beat = 0;
    char what[40];
    int st, tag = 0;
    bool hung = false;

    tv_what(c, k, what);
    a[0] = HP_TV_MAGIC;
    a[1] = HP_TV_UNLOCK;
    st = bvp_call(b, HP_OP_TV, a, 2, &code, 1, &n, PB_HANG_US);
    if (st != HP_OK || n != 1 || !code)
    {
        r->status = st != HP_OK ? st : HP_E_INVAL;
        pb_say("%s %s unlock %s (%d, %lu words)", c->pfx, what,
               st < 0 ? "hang" : "refused", st, (unsigned long)n);
        return -1;
    }
    n = tvp_request(a, q->out, q->std, code, q->ms, q->fmt, q->w704, q->pat);
    if (!n)
    {
        r->status = HP_E_INVAL;
        pb_say("%s %s: a request out of bounds", c->pfx, what);
        return -2;
    }
    t0 = b->port->now_us();
    while ((st = bvp_send(b, HP_OP_TV, a, n, &tag)) == BVP_AGAIN)
    {
        if (bvp_poll(b) < 0)
            st = BVP_E_PORT;
        else if (b->port->now_us() - t0 > PB_HANG_US)
            st = BVP_E_TIMEOUT;
        if (st != BVP_AGAIN)
            break;
        b->port->yield();
    }
    if (st != 0)
    {
        r->status = st;
        pb_say("%s %s: not sent (%d)", c->pfx, what, st);
        return -3;
    }
    c->ran[k] = true;
    t0 = b->port->now_us();
    look = t0 - TVP_LOOK_US;
    tvp_dog_start(&d, t0);
    for (;;)
    {
        if (bvp_poll(b) < 0)
        {
            st = BVP_E_PORT;
            break;
        }
        st = bvp_take(b, tag, r->w, HP_TV_REPLY_WORDS, &r->n);
        if (st != BVP_AGAIN)
            break;
        now = b->port->now_us();
        if (now - look >= TVP_LOOK_US)
        {
            look = now;
            if (b->port->rd(HP_TV_PROGRESS, &prog) < 0 ||
                b->port->rd(HP_STATE, &beat) < 0)
            {
                st = BVP_E_PORT;
                break;
            }
            if (tvp_dog_hung(&d, now, prog, beat))
            {
                hung = true;
                st = BVP_E_TIMEOUT;
                break;
            }
        }
        if (now - t0 > tvp_deadline_us(q->ms))
        {
            st = BVP_E_TIMEOUT;
            break;
        }
        b->port->yield();
    }
    r->status = st;
    if (st < 0)
    {
        pb_say("%s %s: %s at phase %lu, %lu ms (progress %08lx)", c->pfx,
               what, hung ? "hang" : st == BVP_E_PORT ? "port lost"
               : "no reply by the deadline",
               (unsigned long)tvp_prog_phase(prog),
               (unsigned long)tvp_prog_ms(prog), (unsigned long)prog);
        return -4;
    }
    tv_log(c, k);
    return 0;
}

static int tv_fn(struct bvp *b, uint32_t fast, void *ctx)
{
    struct tv_ctx *c = ctx;
    int k, rc = 0;

    (void)fast;
    if (!canary(b, c->pfx, "boot", c->panel))
        return -1;
    for (k = 0; k < c->n && rc == 0; k++)
        rc = tv_step(b, c, k);
    return rc;
}

static void tv_init(struct tv_ctx *c, const char *pfx, int n)
{
    int k;

    memset(c, 0, sizeof(*c));
    c->pfx = pfx;
    c->panel = -1;
    c->n = n;
    for (k = 0; k < TV_REQS; k++)
        c->r[k].status = BVP_E_STATE;       /* not run */
}

/* One boot: the image, the canary, the steps; then the power cut and the
 * display back (vplayer_probe), whatever happened */
static int tv_boot(struct tv_ctx *c)
{
    return vplayer_probe(read_panel, tv_fn, c);
}

static void tv_rate(const struct tvp_reply *r, char *buf)
{
    uint32_t mhz = tvp_rate_mhz(r->w[HP_TV_R_FIELDS], r->w[HP_TV_R_US]);

    snprintf(buf, 16, "%lu.%lu/s", (unsigned long)mhz / 1000,
             (unsigned long)mhz / 100 % 10);
}

bool devbcm_pb7(void)
{
    static const char *const lines[] = {
        "Under a minute, hands off. The",
        "video chip STARTS its TV timing",
        "(a clock, no output) for half",
        "a second, NTSC then PAL, and",
        "counts the fields. Nothing",
        "reaches the jack." };
    struct simplelist_info info;
    struct tv_ctx c;
    char rate[16];
    bool pass[2];
    int k, rc;

    if (!devbcm_lab_open(&info, "BCM probe PB-7", lines, ARRAYLEN(lines)))
        return false;
    tv_init(&c, "pb7:", 2);
    for (k = 0; k < 2; k++)
    {
        c.q[k].std = k ? HP_TV_PAL : HP_TV_NTSC;
        c.q[k].ms = TVP_PB7_MS;
    }
    pb_say("=== BCM probe PB-7 (TV timing, no output, %d ms each): "
           "build %s ===", TVP_PB7_MS, rbversion);
    rc = tv_boot(&c);
    for (k = 0; k < 2; k++)
    {
        const struct tvp_reply *r = &c.r[k];
        enum tvp_check ch = tvp_check(r);

        pass[k] = c.ran[k] && tvp_pb7_pass(c.q[k].std, r);
        tv_rate(r, rate);
        pb_say("pb7: std=%s: %s, %lu changes in %lu us (%s, %s)",
               tvp_std_name(c.q[k].std), pass[k] ? "PASS" : "FAIL",
               (unsigned long)r->w[HP_TV_R_FIELDS],
               (unsigned long)r->w[HP_TV_R_US],
               ch != TVP_OK ? tvp_check_name(ch) : rate,
               tvp_rate_name(tvp_rate_class(c.q[k].std, r->w[HP_TV_R_FIELDS],
                                            r->w[HP_TV_R_US])));
    }
    tvp_note_pb7(&tv_lock, pass[0], pass[1]);
    pb_say("pb7: %s (rc %d); PB-8 %s", pass[0] && pass[1] ? "PASS" : "FAIL",
           rc, tvp_pb8_open(&tv_lock) ? "unlocked" : "stays locked");
    simplelist_addline("PB-7: %s", pass[0] && pass[1] ? "PASS" : "FAIL");
    for (k = 0; k < 2; k++)
    {
        const struct tvp_reply *r = &c.r[k];
        enum tvp_check ch = tvp_check(r);

        tv_rate(r, rate);
        if (!c.ran[k])
            simplelist_addline("%s: not run", k ? "PAL" : "NTSC");
        else if (ch != TVP_OK)
            simplelist_addline("%s: %s", k ? "PAL" : "NTSC",
                               tvp_check_name(ch));
        else
            simplelist_addline("%s: %lu fields, %s, %s", k ? "PAL" : "NTSC",
                               (unsigned long)r->w[HP_TV_R_FIELDS], rate,
                               tvp_rate_name(tvp_rate_class(c.q[k].std,
                                   r->w[HP_TV_R_FIELDS], r->w[HP_TV_R_US])));
    }
    simplelist_addline("PB-8: %s", tvp_pb8_open(&tv_lock) ? "unlocked"
                                                          : "locked");
    if (rc)
        simplelist_addline("Failed (%d)", rc);
    return devbcm_show(&info);
}

static const char *const tv_arm_label[TVP_ARMS] =
{
    "NTSC, UYVY", "NTSC, YUYV", "NTSC, RGB565", "NTSC, Y word",
    "PAL, UYVY", "PAL, YUYV", "PAL, RGB565", "PAL, Y word",
};

bool devbcm_pb8(void)
{
    static const char *const lines[] = {
        "Needs the AV cable in the",
        "headphone jack (no headphones)",
        "and a TV on its AV input.",
        "Pick one format: the video",
        "chip DRIVES the TV output with",
        "colour bars for 4 s, then you",
        "say what the TV showed." };
    static const char *const saw[TVP_SAWS] =
    {
        "Colour bars", "A garbled picture", "Nothing",
    };
    struct simplelist_info info;
    struct tv_ctx c;
    int arm, seen = -1, rc;
    enum tvp_check ch;

    simplelist_info_init(&info, "BCM probe PB-8", 0, NULL);
    simplelist_reset_lines();
    if (!tvp_pb8_open(&tv_lock))
    {
        simplelist_addline("Locked.");
        simplelist_addline("Run PB-7 first, in this boot:");
        simplelist_addline("NTSC and PAL must both pass.");
        return devbcm_show(&info);
    }
    if (!devbcm_lab_open(&info, "BCM probe PB-8", lines, ARRAYLEN(lines)))
        return false;
    arm = devbcm_choose("BCM probe PB-8", "Which standard and format?",
                        "Watch the TV after SELECT.", tv_arm_label, TVP_ARMS);
    if (arm < 0)
        return false;
    tv_init(&c, "pb8:", 1);
    c.q[0].out = true;
    c.q[0].std = tvp_arm_std(arm);
    c.q[0].fmt = tvp_arm_fmt(arm);
    c.q[0].w704 = HP_TV_W640;
    c.q[0].pat = HP_TV_PAT_BARS;
    c.q[0].ms = TVP_OUT_MS;
    pb_say("=== BCM probe PB-8 (TV output, %s/%s, bars, %d ms): build %s ===",
           tvp_std_name(c.q[0].std), tvp_fmt_name(c.q[0].fmt), TVP_OUT_MS,
           rbversion);
    rc = tv_boot(&c);
    ch = tvp_check(&c.r[0]);
    if (c.ran[0])
        seen = devbcm_choose("BCM probe PB-8", "What did the TV show?",
                             tv_arm_label[arm], saw, TVP_SAWS);
    tvp_note_pb8(&tv_lock, arm, &c.r[0], seen);
    pb_say("pb8: std=%s arm=%s run=%s owner=%s (rc %d); PB-9 %s",
           tvp_std_name(c.q[0].std), tvp_fmt_name(c.q[0].fmt),
           c.ran[0] ? tvp_check_name(ch) : "not run", tvp_saw_name(seen), rc,
           tvp_pb9_open(&tv_lock) ? "unlocked" : "stays locked");
    simplelist_info_init(&info, "BCM probe PB-8", 0, NULL);
    simplelist_reset_lines();
    simplelist_addline("PB-8: %s", tv_arm_label[arm]);
    simplelist_addline("Run: %s", c.ran[0] ? tvp_check_name(ch) : "not run");
    simplelist_addline("TV showed: %s", tvp_saw_name(seen));
    simplelist_addline("PB-9: %s", tvp_pb9_open(&tv_lock) ? "unlocked"
                                                          : "locked");
    if (rc)
        simplelist_addline("Failed (%d)", rc);
    return devbcm_show(&info);
}

bool devbcm_pb9(void)
{
    static const char *const lines[] = {
        "Needs the AV cable and the TV",
        "as in PB-8. The video chip",
        "DRIVES the TV output twice for",
        "4 s: thin lines from a 640-",
        "wide line, then a 704-wide",
        "one, in PB-8's bars format.",
        "Say how each looked." };
    static const char *const edge[TVP_EDGES] =
    {
        "Lines reach both edges", "A band or border at a side",
        "A garbled picture", "Nothing",
    };
    static const char *const pick[TVP_PICKS] =
    {
        "640 looked right", "704 looked right", "Both looked the same",
        "Neither",
    };
    struct simplelist_info info;
    struct tv_ctx c[2];
    int k, arm = tv_lock.bars_arm, seen[2] = { -1, -1 }, chose = -1, rc = 0;

    simplelist_info_init(&info, "BCM probe PB-9", 0, NULL);
    simplelist_reset_lines();
    if (!tvp_pb9_open(&tv_lock))
    {
        simplelist_addline("Locked.");
        simplelist_addline("Run PB-8 first, in this boot,");
        simplelist_addline("and answer \"Colour bars\" for");
        simplelist_addline("a format.");
        return devbcm_show(&info);
    }
    if (!devbcm_lab_open(&info, "BCM probe PB-9", lines, ARRAYLEN(lines)))
        return false;
    pb_say("=== BCM probe PB-9 (TV width 640 vs 704, %s/%s, lines, %d ms): "
           "build %s ===", tvp_std_name(tvp_arm_std(arm)),
           tvp_fmt_name(tvp_arm_fmt(arm)), TVP_OUT_MS, rbversion);
    for (k = 0; k < 2; k++)
    {
        tv_init(&c[k], "pb9:", 1);
        if (!devbcm_cue("BCM probe PB-9", k ? "Next: lines, 704 wide."
                        : "First: lines, 640 wide.",
                        "Watch the TV after SELECT."))
            break;
        c[k].q[0].out = true;
        c[k].q[0].std = tvp_arm_std(arm);
        c[k].q[0].fmt = tvp_arm_fmt(arm);
        c[k].q[0].w704 = k ? HP_TV_W704 : HP_TV_W640;
        c[k].q[0].pat = HP_TV_PAT_LINES;
        c[k].q[0].ms = TVP_OUT_MS;
        rc = tv_boot(&c[k]);
        if (c[k].ran[0])
            seen[k] = devbcm_choose("BCM probe PB-9", "What did the TV show?",
                                    k ? "Input 704 wide" : "Input 640 wide",
                                    edge, TVP_EDGES);
        pb_say("pb9: in=%d run=%s owner=%s (rc %d)", k ? 704 : 640,
               c[k].ran[0] ? tvp_check_name(tvp_check(&c[k].r[0]))
               : "not run", tvp_edge_name(seen[k]), rc);
        if (!c[k].ran[0])
            break;
    }
    if (c[0].ran[0] && c[1].ran[0])
        chose = devbcm_choose("BCM probe PB-9", "Which one looked right?",
                              NULL, pick, TVP_PICKS);
    pb_say("pb9: pick=%s verdict=%s (640 %s, 704 %s)", tvp_pick_name(chose),
           tvp_scale_name(tvp_scale_verdict(seen[0], seen[1])),
           tvp_edge_name(seen[0]), tvp_edge_name(seen[1]));
    simplelist_info_init(&info, "BCM probe PB-9", 0, NULL);
    simplelist_reset_lines();
    simplelist_addline("PB-9: %s", tv_arm_label[arm]);
    for (k = 0; k < 2; k++)
        simplelist_addline("%d wide: %s", k ? 704 : 640,
                           tvp_edge_name(seen[k]));
    simplelist_addline("Looked right: %s", tvp_pick_name(chose));
    simplelist_addline("Path: %s", tvp_scale_name(tvp_scale_verdict(seen[0],
                                                                    seen[1])));
    if (rc)
        simplelist_addline("Failed (%d)", rc);
    return devbcm_show(&info);
}

/* ---- PW3, PW4: core voltage and clock (devbcm_pw.h; bench-p19.md cards
 * PW3 and PW4) --------------------------------------------------------------
 *
 * HP_OP_PW in the video image: an UNLOCK for a one-time code, then one
 * LEVEL (PW3: the regulator at 27 MHz) or one POINT (PW4: a clock of
 * Apple's ladder at its level) with it, held while the image runs its
 * memory self-check over and over.  Every value is Apple's; the image
 * writes only the regulator, PLL A with the core word and the SDRAM
 * refresh, and puts each back (read back) before it replies.  The PP
 * waits for the hold plus a margin, with the TV rows' watchdog on the
 * progress word and the heartbeat.  The steps go top down and stop at the
 * first that isn't clean; vplayer_probe() then cuts the BCM's power,
 * whatever happened.  PW4 opens once PW3 passed in this boot, and runs
 * only with stage 4b's pass flag (PLL A proven on this unit). */
static struct pwp_lock pw_lock = { false };     /* this boot only */

struct pw_ctx
{
    const char *pfx;            /* "pw3:" or "pw4:" */
    bool ladder;
    int panel, n, done;         /* steps planned, steps with a reply */
    uint32_t hold_ms;
    bool no4b, sdram;
    struct pwp_reply r[PWP_PW3_STEPS > PWP_PW4_STEPS ? PWP_PW3_STEPS
                                                      : PWP_PW4_STEPS];
};

static uint32_t pw_what(const struct pw_ctx *c, int k)
{
    return c->ladder ? (uint32_t)k : pwp_pw3_level(k);
}

static void pw_log(const struct pw_ctx *c, int k)
{
    const struct pwp_reply *r = &c->r[k];
    const uint32_t *w = r->w;
    char at[24];

    if (c->ladder)
        snprintf(at, sizeof(at), "f=%lu level %lu", (unsigned long)pwp_mhz(k),
                 (unsigned long)pwp_level(k));
    else
        snprintf(at, sizeof(at), "L=%lu", (unsigned long)pw_what(c, k));
    pb_say("%s %s st=%d exit=%lu phase=%lu: %s", c->pfx, at, r->status,
           (unsigned long)w[HP_PW_R_EXIT], (unsigned long)w[HP_PW_R_PHASE],
           pwp_check_name(pwp_check(r)));
    pb_say("%s %s reg %08lx (was %08lx) core %08lx plla %08lx refresh "
           "%08lx (was %08lx) flags %02lx mismatch %02lx", c->pfx, at,
           (unsigned long)w[HP_PW_R_REG], (unsigned long)w[HP_PW_R_REG0],
           (unsigned long)w[HP_PW_R_CORE], (unsigned long)w[HP_PW_R_PLLA],
           (unsigned long)w[HP_PW_R_REF], (unsigned long)w[HP_PW_R_REF0],
           (unsigned long)w[HP_PW_R_FLAGS], (unsigned long)w[HP_PW_R_MISMATCH]);
    pb_say("%s %s runs %lu wrong %lu sums %08lx/%08lx ref %08lx/%08lx "
           "us %lu (ref %lu) hold %lu us frame %lu us", c->pfx, at,
           (unsigned long)pwp_runs(r), (unsigned long)pwp_wrong(r),
           (unsigned long)w[HP_PW_R_SUM], (unsigned long)w[HP_PW_R_VSUM],
           (unsigned long)w[HP_PW_R_SUM0], (unsigned long)w[HP_PW_R_VSUM0],
           (unsigned long)w[HP_PW_R_US], (unsigned long)w[HP_PW_R_US0],
           (unsigned long)w[HP_PW_R_HOLD], (unsigned long)w[HP_PW_R_FRAME]);
    if (c->ladder)
    {
        uint32_t got = pwp_speed_milli(r), want = pwp_want_milli(k);

        pb_say("%s f=%lu speed x%lu.%03lu (clock alone x%lu.%03lu): %s",
               c->pfx, (unsigned long)pwp_mhz(k), (unsigned long)got / 1000,
               (unsigned long)got % 1000, (unsigned long)want / 1000,
               (unsigned long)want % 1000,
               pwp_speed_name(pwp_speed_class(r, k)));
    }
}

/* One step: UNLOCK, then the request, waited on with the watchdog.
 * 0 with a reply in c->r[k], < 0 without one */
static int pw_step(struct bvp *b, struct pw_ctx *c, int k)
{
    struct pwp_reply *r = &c->r[k];
    struct tvp_dog d;
    uint32_t a[5], code = 0, n = 0, t0, now, look, prog = 0, beat = 0;
    int st, tag = 0;
    bool hung = false;

    a[0] = HP_PW_MAGIC;
    a[1] = HP_PW_UNLOCK;
    st = bvp_call(b, HP_OP_PW, a, 2, &code, 1, &n, PB_HANG_US);
    if (st != HP_OK || n != 1 || !code)
    {
        r->status = st != HP_OK ? st : HP_E_INVAL;
        pb_say("%s step %d unlock %s (%d, %lu words)", c->pfx, k,
               st < 0 ? "hang" : "refused", st, (unsigned long)n);
        return -1;
    }
    n = pwp_request(a, c->ladder, code, pw_what(c, k), c->hold_ms);
    if (!n)
    {
        r->status = HP_E_INVAL;
        pb_say("%s step %d: a request out of bounds", c->pfx, k);
        return -2;
    }
    t0 = b->port->now_us();
    while ((st = bvp_send(b, HP_OP_PW, a, n, &tag)) == BVP_AGAIN)
    {
        if (bvp_poll(b) < 0)
            st = BVP_E_PORT;
        else if (b->port->now_us() - t0 > PB_HANG_US)
            st = BVP_E_TIMEOUT;
        if (st != BVP_AGAIN)
            break;
        b->port->yield();
    }
    if (st != 0)
    {
        r->status = st;
        pb_say("%s step %d: not sent (%d)", c->pfx, k, st);
        return -3;
    }
    c->done = k + 1;
    t0 = b->port->now_us();
    look = t0 - TVP_LOOK_US;
    tvp_dog_start(&d, t0);
    for (;;)
    {
        if (bvp_poll(b) < 0)
        {
            st = BVP_E_PORT;
            break;
        }
        st = bvp_take(b, tag, r->w, HP_PW_REPLY_WORDS, &r->n);
        if (st != BVP_AGAIN)
            break;
        now = b->port->now_us();
        if (now - look >= TVP_LOOK_US)
        {
            look = now;
            if (b->port->rd(HP_PW_PROGRESS, &prog) < 0 ||
                b->port->rd(HP_STATE, &beat) < 0)
            {
                st = BVP_E_PORT;
                break;
            }
            if (tvp_dog_hung(&d, now, prog, beat))
            {
                hung = true;
                st = BVP_E_TIMEOUT;
                break;
            }
        }
        if (now - t0 > pwp_deadline_us(c->hold_ms))
        {
            st = BVP_E_TIMEOUT;
            break;
        }
        b->port->yield();
    }
    r->status = st;
    if (st < 0)
    {
        pb_say("%s step %d: %s at phase %lu, %lu ms (progress %08lx)", c->pfx,
               k, hung ? "hang" : st == BVP_E_PORT ? "port lost"
               : "no reply by the deadline",
               (unsigned long)pwp_prog_phase(prog),
               (unsigned long)pwp_prog_ms(prog), (unsigned long)prog);
        return -4;
    }
    pw_log(c, k);
    return 0;
}

static int pw_fn(struct bvp *b, uint32_t fast, void *ctx)
{
    struct pw_ctx *c = ctx;
    int k, rc = 0;

    if (!canary(b, c->pfx, "boot", c->panel))
        return -1;
    if (c->ladder)
    {
        /* PLL A proven on this unit (stage 4b) before any point */
        if (!fast)
        {
            c->no4b = true;
            pb_say("%s no stage 4b pass flag: PLL A not proven here, not run",
                   c->pfx);
            return -5;
        }
        /* SDRAM up, so the refresh follows the clock (it stays up after
         * the CLOSE; the probe needs no session) */
        c->sdram = bvp_open(b, 320, 240, 0) == HP_OK;
        if (c->sdram)
            bvp_close(b, 0);
        else
            pb_say("%s OPEN failed: SDRAM down, the refresh is not stepped",
                   c->pfx);
    }
    for (k = 0; k < c->n && rc == 0; k++)
    {
        rc = pw_step(b, c, k);
        if (rc == 0 && pwp_check(&c->r[k]) != PWP_OK)
            break;              /* restored by the image; no lower step */
    }
    return rc;
}

static void pw_init(struct pw_ctx *c, const char *pfx, bool ladder)
{
    int k;

    memset(c, 0, sizeof(*c));
    c->pfx = pfx;
    c->ladder = ladder;
    c->panel = -1;
    c->n = ladder ? PWP_PW4_STEPS : PWP_PW3_STEPS;
    c->hold_ms = ladder ? PWP_PW4_HOLD_MS : PWP_PW3_HOLD_MS;
    for (k = 0; k < (int)ARRAYLEN(c->r); k++)
        c->r[k].status = BVP_E_STATE;       /* not run */
}

bool devbcm_pw3(void)
{
    static const char *const lines[] = {
        "About 90 s, hands off. The",
        "video chip LOWERS its core",
        "voltage to Apple's levels 11,",
        "9, 7 then 5 (20 s each) at",
        "27 MHz and checks its memory",
        "throughout." };
    struct simplelist_info info;
    struct pw_ctx c;
    int k, rc, lowest;
    bool pass;

    if (!devbcm_lab_open(&info, "BCM probe PW3", lines, ARRAYLEN(lines)))
        return false;
    pw_init(&c, "pw3:", false);
    pb_say("=== BCM probe PW3 (core voltage, levels 11 9 7 5 at 27 MHz, "
           "%d ms each): build %s ===", PWP_PW3_HOLD_MS, rbversion);
    rc = vplayer_probe(read_panel, pw_fn, &c);
    lowest = pwp_pw3_lowest(c.r, c.done);
    pass = rc == 0 && pwp_pw3_pass(c.r, c.done);
    pwp_note_pw3(&pw_lock, pass);
    pb_say("pw3: %s at %d, lowest good level %d (rc %d); PW4 %s",
           pass ? "PASS" : "FAIL", c.done ? (int)pwp_pw3_level(c.done - 1)
                                          : 0, lowest, rc,
           pwp_pw4_open(&pw_lock) ? "unlocked" : "stays locked");
    simplelist_addline("PW3: %s", pass ? "PASS" : "FAIL");
    simplelist_addline("Lowest good level: %d", lowest);
    for (k = 0; k < c.done; k++)
        simplelist_addline("Level %lu: %s, %lu runs",
                           (unsigned long)pwp_pw3_level(k),
                           pwp_check_name(pwp_check(&c.r[k])),
                           (unsigned long)pwp_runs(&c.r[k]));
    simplelist_addline("PW4: %s", pwp_pw4_open(&pw_lock) ? "unlocked"
                                                         : "locked");
    if (rc)
        simplelist_addline("Failed (%d)", rc);
    return devbcm_show(&info);
}

bool devbcm_pw4(void)
{
    static const char *const lines[] = {
        "About a minute, hands off.",
        "The video chip STEPS its core",
        "clock and voltage up Apple's",
        "ladder: 46, 74, 102, 150 MHz",
        "(10 s each), checking its",
        "memory, then back to 27 MHz." };
    struct simplelist_info info;
    struct pw_ctx c;
    int k, rc;
    bool pass;

    simplelist_info_init(&info, "BCM probe PW4", 0, NULL);
    simplelist_reset_lines();
    if (!pwp_pw4_open(&pw_lock))
    {
        simplelist_addline("Locked.");
        simplelist_addline("Run PW3 first, in this boot:");
        simplelist_addline("level 5 must pass.");
        return devbcm_show(&info);
    }
    if (!devbcm_lab_open(&info, "BCM probe PW4", lines, ARRAYLEN(lines)))
        return false;
    pw_init(&c, "pw4:", true);
    pb_say("=== BCM probe PW4 (clock ladder 46 74 102 150 MHz at levels "
           "5 7 9 15, %d ms each): build %s ===", PWP_PW4_HOLD_MS, rbversion);
    rc = vplayer_probe(read_panel, pw_fn, &c);
    pass = rc == 0 && pwp_pw4_pass(c.r, c.done);
    pb_say("pw4: %s (%d of %d points, rc %d%s%s)", pass ? "PASS" : "FAIL",
           c.done, PWP_PW4_STEPS, rc, c.no4b ? ", no 4b flag" : "",
           c.sdram ? "" : ", SDRAM down");
    simplelist_addline("PW4: %s", pass ? "PASS" : "FAIL");
    if (c.no4b)
        simplelist_addline("Needs stage 4b's pass first");
    for (k = 0; k < c.done; k++)
    {
        uint32_t x = pwp_speed_milli(&c.r[k]);

        simplelist_addline("%lu MHz: %s, x%lu.%02lu",
                           (unsigned long)pwp_mhz(k),
                           pwp_check_name(pwp_check(&c.r[k])),
                           (unsigned long)x / 1000,
                           (unsigned long)x % 1000 / 10);
    }
    if (rc)
        simplelist_addline("Failed (%d)", rc);
    return devbcm_show(&info);
}

/* ---- PW5 and PW6: hibernate and the bus with the chip off ------------
 *
 * PW5 (bench-p19.md card PW5): the video image parks as Apple's
 * videocore=1 does (HP_OP_HIB), then bcmdt_hib_cycle drives Apple's
 * retain pins (GPO32 bit 15 high, 5 ms, bit 14 low), holds with no port
 * access, drives the on pins back and wakes the image through the 0x1F0
 * word.  Holds of 1 s, then 10 s, then 60 s, each only after the one
 * before passed.  Any failure after the first pin is cut there and then
 * (bit 15 low, bit 14 low, 100 ms); vplayer_probe() then cuts again and
 * brings the display back by the stock wake path (a cold start), as
 * after every probe.  Locked until PW1 passed in this boot.
 *
 * PW6 (card PW6): read only.  Nothing on the record says how bypass is
 * entered (brief-map-power.md section 8: no register, no Apple use), so
 * no byte is written into the port of a powered-down chip.  B1 reads the
 * port's four registers raw with the image idle (no address written),
 * B2 cuts the chip by bit 14 alone (Rockbox's own cut) and reads them
 * again while the owner watches what the panel keeps.  Locked until PW5
 * has driven bit 15 in this boot. */
#define PW6_LOOK_MS     10000       /* the owner's look with the chip cut */

struct pw5_ctx
{
    struct bvp *b;
    int panel, done, tag;           /* cycles with a verdict */
    bool pins;                      /* bit 15 driven at least once */
    uint32_t a[4], n;
    bool pass[PWH_HOLDS];
    struct bcmdt_hib_result res[PWH_HOLDS];
    struct pwh_reply r[PWH_HOLDS];
};

static int pw5_send(void *ctx)
{
    struct pw5_ctx *c = ctx;
    uint32_t t0 = c->b->port->now_us();
    int st;

    while ((st = bvp_send(c->b, HP_OP_HIB, c->a, c->n, &c->tag)) == BVP_AGAIN)
    {
        if (bvp_poll(c->b) < 0 || c->b->port->now_us() - t0 > PB_HANG_US)
            return -1;
        c->b->port->yield();
    }
    return st == 0 ? 0 : -1;
}

static uint32_t pw5_now(void)
{
    return USEC_TIMER;
}

static void pw5_udelay(uint32_t us)
{
    udelay(us);
}

static void pw5_sleep(uint32_t ms)
{
    sleep((ms * HZ + 999) / 1000);
}

static void pw5_yield(void)
{
    yield();
}

/* The reply to tag, waited on with the TV rows' watchdog on HIB's
 * progress word and the heartbeat, up to deadline_us */
static int pw5_take(struct pw5_ctx *c, struct pwh_reply *r, uint32_t deadline_us)
{
    struct bvp *b = c->b;
    struct tvp_dog d;
    uint32_t t0 = b->port->now_us(), now, look = t0 - TVP_LOOK_US, prog = 0,
             beat = 0;
    int st;

    tvp_dog_start(&d, t0);
    for (;;)
    {
        if (bvp_poll(b) < 0)
        {
            st = BVP_E_PORT;
            break;
        }
        st = bvp_take(b, c->tag, r->w, HP_HIB_REPLY_WORDS, &r->n);
        if (st != BVP_AGAIN)
            break;
        now = b->port->now_us();
        if (now - look >= TVP_LOOK_US)
        {
            look = now;
            if (b->port->rd(HP_HIB_PROGRESS, &prog) < 0 ||
                b->port->rd(HP_STATE, &beat) < 0)
            {
                st = BVP_E_PORT;
                break;
            }
            if (tvp_dog_hung(&d, now, prog, beat))
            {
                st = BVP_E_TIMEOUT;
                break;
            }
        }
        if (now - t0 > deadline_us)
        {
            st = BVP_E_TIMEOUT;
            break;
        }
        b->port->yield();
    }
    r->status = st;
    if (st < 0)
        pb_say("pw5: no reply (%d) at phase %lu, %lu ms (progress %08lx)", st,
               (unsigned long)pwh_prog_phase(prog),
               (unsigned long)pwh_prog_ms(prog), (unsigned long)prog);
    return st;
}

static void pw5_log(const struct pw5_ctx *c, int k)
{
    const struct bcmdt_hib_result *s = &c->res[k];
    const struct pwh_reply *r = &c->r[k];
    const uint32_t *w = r->w;
    uint32_t x = pwh_speedup_x10(s->wake_us);
    static const char *const kept[3] = { "kept", "LOST", "not laid" };

    pb_say("pw5: hold %lu ms: %s; park %lu us, held %lu us, both pins high "
           "%lu/%lu us, pins end %04lx%s", (unsigned long)pwh_hold_ms(k),
           pwh_end_name(s->end), (unsigned long)s->park_us,
           (unsigned long)s->hold_us, (unsigned long)s->both_us[0],
           (unsigned long)s->both_us[1], (unsigned long)s->gpo_end,
           s->cut ? ", cut" : "");
    if (s->pins)
        pb_say("pw5: hold %lu ms: 1F0 after = %08lx (%s), wake %lu us "
               "(answer %lu us): x%lu.%lu a cold start's %lu us",
               (unsigned long)pwh_hold_ms(k), (unsigned long)s->word_after,
               pwh_word_name(pwh_word(s->word_after)),
               (unsigned long)s->wake_us, (unsigned long)s->answer_us,
               (unsigned long)x / 10, (unsigned long)x % 10,
               (unsigned long)PWH_COLD_US);
    if (r->status < 0 || r->n != HP_HIB_REPLY_WORDS)
    {
        pb_say("pw5: hold %lu ms: image st=%d, %lu words: %s",
               (unsigned long)pwh_hold_ms(k), r->status, (unsigned long)r->n,
               pwh_check_name(pwh_check(r)));
        return;
    }
    pb_say("pw5: hold %lu ms: image exit %lu phase %lu: %s; sram bad %lu "
           "(first %ld), sdram %s (bad %ld), frame pre/post %08lx/%08lx",
           (unsigned long)pwh_hold_ms(k), (unsigned long)w[HP_HIB_R_EXIT],
           (unsigned long)w[HP_HIB_R_PHASE], pwh_check_name(pwh_check(r)),
           (unsigned long)w[HP_HIB_R_SRAM_BAD], (long)w[HP_HIB_R_SRAM_FIRST],
           kept[pwh_sdram_kept(r)], (long)w[HP_HIB_R_SDRAM_BAD],
           (unsigned long)w[HP_HIB_R_FRAME_PRE],
           (unsigned long)w[HP_HIB_R_FRAME_POST]);
    pb_say("pw5: hold %lu ms: counter %lu us parked (%s), polls %lu, "
           "restore %lu us, sd %lx, reg %08lx core %08lx flags %lx "
           "mismatch %lx", (unsigned long)pwh_hold_ms(k),
           (unsigned long)w[HP_HIB_R_HELD_US],
           pwh_counter_ran(r, s->hold_us) ? "ran" : "STOPPED or reset",
           (unsigned long)w[HP_HIB_R_POLLS],
           (unsigned long)w[HP_HIB_R_RESTORE_US], (unsigned long)w[HP_HIB_R_SD],
           (unsigned long)w[HP_HIB_R_REG], (unsigned long)w[HP_HIB_R_CORE],
           (unsigned long)w[HP_HIB_R_FLAGS],
           (unsigned long)w[HP_HIB_R_MISMATCH]);
}

/* One cycle: UNLOCK, the pins (bcmdt_hib_cycle), then the image's reply.
 * 0 with the cycle passed; < 0 otherwise (the BCM is then cut if the
 * pins moved) */
static int pw5_cycle(struct pw5_ctx *c, int k)
{
    struct bcmdt_hib_hal hal = {
        .rd = bcm_video_rd, .wr = bcm_video_wr, .send = pw5_send,
        .gpo = bcm_video_gpo32, .gpo_enable = bcm_video_gpo32_enable,
        .now_us = pw5_now, .udelay = pw5_udelay, .sleep_ms = pw5_sleep,
        .yield = pw5_yield, .ctx = c,
    };
    struct pwh_reply *r = &c->r[k];
    uint32_t code = 0, n = 0;
    int st, end;

    r->status = BVP_E_STATE;
    r->n = 0;
    c->a[0] = HP_HIB_MAGIC;
    c->a[1] = HP_HIB_UNLOCK;
    st = bvp_call(c->b, HP_OP_HIB, c->a, 2, &code, 1, &n, PB_HANG_US);
    if (st != HP_OK || n != 1 || !code)
    {
        pb_say("pw5: hold %lu ms: unlock %s (%d)", (unsigned long)pwh_hold_ms(k),
               st < 0 ? "hang" : "refused", st);
        return -1;
    }
    c->n = pwh_request(c->a, code, PWH_WAIT_MS);
    pb_say("pw5: hold %lu ms: the park, then the retain pins (watch the "
           "bars)", (unsigned long)pwh_hold_ms(k));
    powerlog_mark("pb5_hold");
    end = bcmdt_hib_cycle(&hal, pwh_hold_ms(k), &c->res[k]);
    powerlog_mark("pb5_wake");
    c->done = k + 1;
    if (c->res[k].pins)
    {
        c->pins = true;
        pwh_note_pw5(&hib_lock, true);
    }
    /* awake: the reply follows; never parked: the refusal may be there */
    if (end == 0 || end == -3)
        pw5_take(c, r, end == 0 ? PWH_REPLY_US : TVP_HANG_US);
    pw5_log(c, k);
    c->pass[k] = pwh_cycle_pass(end, c->res[k].word_after, r);
    return c->pass[k] ? 0 : -2;
}

static int pw5_fn(struct bvp *b, uint32_t fast, void *ctx)
{
    struct pw5_ctx *c = ctx;
    uint32_t pp[5];
    int k, rc = 0;

    (void)fast;
    c->b = b;
    if (!canary(b, "pw5:", "boot", c->panel))
        return -1;
    bcm_video_pp_regs(pp);
    pb_say("pw5: gpo32 val=%08lx en=%08lx", (unsigned long)pp[0],
           (unsigned long)pp[1]);
    /* SDRAM up, so its retention is read too (it stays up after CLOSE) */
    if (bvp_open(b, 320, 240, 0) == HP_OK)
        bvp_close(b, 0);
    else
        pb_say("pw5: OPEN failed: SDRAM down, only SRAM is checked");
    for (k = 0; k < PWH_HOLDS && rc == 0; k++)
        rc = pw5_cycle(c, k);
    return rc;
}

bool devbcm_pw5(void)
{
    static const char *const lines[] = {
        "About 2 minutes, hands off.",
        "The video chip HIBERNATES by",
        "Apple's pins for 1 s, 10 s",
        "and 60 s. Watch the colour",
        "bars: kept in the dark? They",
        "come back mirrored after",
        "each wake." };
    struct simplelist_info info;
    struct pw5_ctx c;
    int k, rc;
    bool pass;

    simplelist_info_init(&info, "BCM probe PW5", 0, NULL);
    simplelist_reset_lines();
    if (!pwh_pw5_open(&hib_lock))
    {
        simplelist_addline("Locked.");
        simplelist_addline("Run PW1 first, in this boot:");
        simplelist_addline("it must pass.");
        return devbcm_show(&info);
    }
    if (!devbcm_lab_open(&info, "BCM probe PW5", lines, ARRAYLEN(lines)))
        return false;
    memset(&c, 0, sizeof(c));
    c.panel = -1;
    pb_say("=== BCM probe PW5 (hibernate: Apple's pins, holds 1 s, 10 s, "
           "60 s): build %s ===", rbversion);
    rc = vplayer_probe(read_panel, pw5_fn, &c);
    pass = pwh_pw5_pass(c.pass, c.done);
    pb_say("pw5: %s (%d of %d holds, rc %d); gpo32 now %04lx; PW6 %s",
           pass ? "PASS" : "FAIL", c.done, PWH_HOLDS, rc,
           (unsigned long)(bcm_video_gpo32(0, 0) & 0xC000),
           pwh_pw6_open(&hib_lock) ? "unlocked" : "stays locked");
    simplelist_addline("PW5: %s", pass ? "PASS" : "FAIL");
    for (k = 0; k < c.done; k++)
    {
        uint32_t x = pwh_speedup_x10(c.res[k].wake_us);

        simplelist_addline("%lu s: %s", (unsigned long)pwh_hold_ms(k) / 1000,
                           c.pass[k] ? "kept" :
                           c.res[k].end ? pwh_end_name(c.res[k].end)
                                        : pwh_check_name(pwh_check(&c.r[k])));
        if (c.res[k].wake_us)
            simplelist_addline("  wake %lu ms, x%lu faster",
                               (unsigned long)c.res[k].wake_us / 1000,
                               (unsigned long)x / 10);
    }
    simplelist_addline("PW6: %s", pwh_pw6_open(&hib_lock) ? "unlocked"
                                                         : "locked");
    if (rc)
        simplelist_addline("Failed (%d)", rc);
    return devbcm_show(&info);
}

struct pw6_ctx
{
    int panel;
    bool b1, b2;                    /* the steps that ran */
    uint16_t raw[3][PWH_RAW];       /* B1, B2 at 100 ms, B2 after the look */
};

static void pw6_raw(const char *step, const uint16_t *v)
{
    pb_say("pw6: %s data %04x %04x %04x %04x ctl %04x alt %04x %04x %04x "
           "%04x altctl %04x (%s)", step, v[0], v[1], v[2], v[3], v[4], v[5],
           v[6], v[7], v[8], v[9], pwh_raw_name(pwh_raw_class(v)));
}

static int pw6_fn(struct bvp *b, uint32_t fast, void *ctx)
{
    struct pw6_ctx *c = ctx;

    (void)fast;
    if (!canary(b, "pw6:", "boot", c->panel))
        return -1;
    /* B1, the control: the image idle, pins as they are */
    bcm_video_port_raw(c->raw[0]);
    pw6_raw("B1 (chip on)", c->raw[0]);
    if (!canary(b, "pw6:", "after B1's raw reads", c->panel))
        return -2;
    c->b1 = true;
    /* B2: Rockbox's cut, bit 14 alone (bit 15 low, as found) */
    if (bcm_video_gpo32(0, 0) & 0x8000)
    {
        pb_say("pw6: GPO32 bit 15 high: not run");
        return -3;
    }
    pb_say("pw6: B2: bit 14 low (the chip cut): look at the panel now "
           "and after %d s", PW6_LOOK_MS / 1000);
    powerlog_mark("pb6_cut");
    bcm_video_gpo32(0x4000, 0);
    sleep(HZ / 10);
    bcm_video_port_raw(c->raw[1]);
    pw6_raw("B2 (cut, 100 ms)", c->raw[1]);
    sleep(PW6_LOOK_MS * HZ / 1000);
    bcm_video_port_raw(c->raw[2]);
    pw6_raw("B2 (cut, after the look)", c->raw[2]);
    c->b2 = true;
    return 0;
}

bool devbcm_pw6(void)
{
    static const char *const lines[] = {
        "About 20 s. READ ONLY: the",
        "video chip is cut (bit 14,",
        "as every sleep does) and its",
        "port read. Photograph the",
        "screen just before SELECT,",
        "then 1 s and 10 s after the",
        "screen stops changing." };
    struct simplelist_info info;
    struct pw6_ctx c;
    int rc;

    simplelist_info_init(&info, "BCM probe PW6", 0, NULL);
    simplelist_reset_lines();
    if (!pwh_pw6_open(&hib_lock))
    {
        simplelist_addline("Locked.");
        simplelist_addline("Run PW5 first, in this boot:");
        simplelist_addline("its pins must have run.");
        return devbcm_show(&info);
    }
    if (!devbcm_lab_open(&info, "BCM probe PW6", lines, ARRAYLEN(lines)))
        return false;
    memset(&c, 0, sizeof(c));
    c.panel = -1;
    pb_say("=== BCM probe PW6 (the port and the panel with the chip cut, "
           "read only): build %s ===", rbversion);
    rc = vplayer_probe(read_panel, pw6_fn, &c);
    pb_say("pw6: %s (rc %d): B1 %s, B2 %s / %s; owner looks recorded",
           c.b1 && c.b2 ? "ran" : "FAIL", rc,
           c.b1 ? pwh_raw_name(pwh_raw_class(c.raw[0])) : "-",
           c.b2 ? pwh_raw_name(pwh_raw_class(c.raw[1])) : "-",
           c.b2 ? pwh_raw_name(pwh_raw_class(c.raw[2])) : "-");
    simplelist_addline("PW6: %s", c.b1 && c.b2 ? "ran" : "FAIL");
    simplelist_addline("Chip on: %s", c.b1 ? pwh_raw_name(pwh_raw_class(c.raw[0]))
                                           : "-");
    simplelist_addline("Chip cut: %s", c.b2 ? pwh_raw_name(pwh_raw_class(c.raw[1]))
                                            : "-");
    simplelist_addline("Was the picture kept?");
    if (rc)
        simplelist_addline("Failed (%d)", rc);
    return devbcm_show(&info);
}

/* ---- P19 19-9's COP rows (bcm-coplab.h; bench-p19.md cards C1-C3) ----
 *
 * C1 boots the video image (vplayer_probe) and moves one engine-2 piece
 * into its free SRAM twice: started and polled by this thread (the
 * control), then by a job on the COP; the CPU reads every word back.
 * C2 and C3 need no video chip.  Each row stops the audio, starts its
 * own worker on the COP and stops it after; lines go to bcmtest.log
 * under c1:, c2:, c3:. */
#define C1_RUNS 2                   /* the CPU's control, then the COP */

struct c1_ctx
{
    uint32_t *win;                  /* G | T | G staging: the whole window */
    uint32_t *src;                  /* S */
    uint32_t seed;
    int pass[C1_RUNS];              /* -1 not run, 0 fail, 1 pass */
    struct coplab_e2 r[C1_RUNS];
    struct e2_tally t[C1_RUNS];
    uint32_t ghits[C1_RUNS];
};

static const char *const c1_who[C1_RUNS] = { "CPU", "COP" };

/* One run: the window filled by the CPU and checked on a sample, S moved
 * by engine 2 from the run's core, every word of the window read back */
static int c1_run(struct c1_ctx *c, int k)
{
    const uint32_t g = COPLAB_GUARD_WORDS, n = COPLAB_WORDS,
                   all = COPLAB_WIN_SIZE / 4;
    uint32_t gb[COPLAB_GUARD_WORDS], ga[COPLAB_GUARD_WORDS], i, got, bad = 0;
    int rc;

    c->seed = e2_seed(USEC_TIMER ^ (uint32_t)current_tick << 9 ^ k);
    e2_fill(c->win, g, E2_PAT_G, c->seed, 0);
    e2_fill(c->win + g, n, E2_PAT_T, c->seed, 0);
    e2_fill(c->win + g + n, g, E2_PAT_G, c->seed, g);
    if (bcm_video_wr_block(COPLAB_WIN, (const uint16_t *)c->win, all * 2) ||
        bcm_video_flush())
    {
        pb_say("c1: %s run: the CPU's fill: port error", c1_who[k]);
        return -1;
    }
    for (i = 0; i < all; i += 64)
    {
        if (bcm_video_rd(COPLAB_WIN + 4 * i, &got) < 0)
            return -1;
        bad += got != c->win[i];
    }
    if (bad)
    {
        pb_say("c1: %s run: the fill did not read back (%lu wrong): void",
               c1_who[k], (unsigned long)bad);
        return -1;
    }
    e2_fill(c->src, n, E2_PAT_S, c->seed, 0);
    rc = bcm_cop_e2_piece(COPLAB_TARGET, c->src, COPLAB_BYTES, k == 1,
                          &c->r[k]);
    pb_say("c1: %s run: rc %d on core %lu, start %lu us, run %lu us "
           "(%lu polls), wall %lu us, %lu B", c1_who[k], rc,
           (unsigned long)c->r[k].core, (unsigned long)c->r[k].start_us,
           (unsigned long)c->r[k].run_us, (unsigned long)c->r[k].polls,
           (unsigned long)c->r[k].wall_us, (unsigned long)COPLAB_BYTES);
    if (rc == -4)
        return rc;                  /* the COP is stuck: no more port */
    e2_tally_init(&c->t[k]);
    for (i = 0; i < all; i++)
    {
        if (bcm_video_rd(COPLAB_WIN + 4 * i, &got) < 0)
        {
            pb_say("c1: %s run: readback: port error at word %lu",
                   c1_who[k], (unsigned long)i);
            return -1;
        }
        if (i < g)
            gb[i] = got;
        else if (i >= g + n)
            ga[i - g - n] = got;
        else
            e2_tally_add(&c->t[k], got, c->seed, i - g, n);
    }
    c->ghits[k] = e2_guard_hits(gb, ga, c->seed);
    pb_say("c1: %s run: ok %lu untouched %lu shifted %lu other %lu of %lu, "
           "first bad %ld, guard hits %lu, seed %03lx", c1_who[k],
           (unsigned long)c->t[k].ok, (unsigned long)c->t[k].untouched,
           (unsigned long)c->t[k].shifted,
           (unsigned long)(c->t[k].other + c->t[k].guardlike),
           (unsigned long)c->t[k].n, (long)c->t[k].first_bad,
           (unsigned long)c->ghits[k], (unsigned long)c->seed);
    c->pass[k] = rc == 0 && e2_tally_pass(&c->t[k]) && !c->ghits[k];
    return 0;
}

static int c1_fn(struct bvp *b, uint32_t fast, void *ctx)
{
    struct c1_ctx *c = ctx;
    int k, rc = 0;

    (void)b; (void)fast;
    for (k = 0; k < C1_RUNS && rc == 0; k++)
        rc = c1_run(c, k);
    return rc;
}

bool devbcm_c1(void)
{
    static const char *const lines[] = {
        "Under a minute, hands off.",
        "Engine 2 copies 32 KB into the",
        "video chip's free memory, from",
        "the CPU, then from the COP." };
    struct simplelist_info info;
    struct c1_ctx c;
    int h, rc, k;

    if (!devbcm_lab_open(&info, "COP row C1", lines, ARRAYLEN(lines)))
        return false;
    memset(&c, 0, sizeof(c));
    c.pass[0] = c.pass[1] = -1;
    pb_say("=== COP row C1 (engine 2 from the COP): build %s ===", rbversion);
    if (bcm_cop_stuck())
    {
        simplelist_addline("COP stuck earlier: REBOOT first");
        return devbcm_show(&info);
    }
    h = core_alloc(COPLAB_WIN_SIZE + COPLAB_BYTES + 32);
    if (h <= 0)
    {
        simplelist_addline("Not available: no memory");
        return devbcm_show(&info);
    }
    core_pin(h);                    /* the engine reads physical memory */
    c.win = (uint32_t *)ALIGN_UP((uintptr_t)core_get_data(h), 16);
    c.src = c.win + COPLAB_WIN_SIZE / 4;
    if (bcm_cop_open() < 0)
    {
        pb_say("c1: no worker on the COP");
        simplelist_addline("No COP worker: not run");
        rc = -3;
    }
    else
    {
        rc = vplayer_probe(NULL, c1_fn, &c);
        bcm_cop_close();
    }
    core_unpin(h);
    core_free(h);
    pb_say("c1: %s (rc %d): CPU %s, COP %s",
           c.pass[1] == 1 ? "PASS" : "FAIL", rc,
           c.pass[0] < 0 ? "-" : c.pass[0] ? "pass" : "fail",
           c.pass[1] < 0 ? "-" : c.pass[1] ? "pass" : "fail");
    simplelist_addline("C1: %s", c.pass[1] == 1 ? "PASS" : "FAIL");
    for (k = 0; k < C1_RUNS; k++)
        if (c.pass[k] >= 0)
            simplelist_addline("%s: %s, %lu us (%lu ok)", c1_who[k],
                               c.pass[k] ? "pass" : "fail",
                               (unsigned long)c.r[k].run_us,
                               (unsigned long)c.t[k].ok);
    if (rc)
        simplelist_addline("Failed (%d)%s", rc,
                           bcm_cop_stuck() ? ": REBOOT" : "");
    return devbcm_show(&info);
}

#define C2_TRIALS   8               /* per direction */
#define C2_LINES    4               /* lines in turn, 16 bytes apart */

bool devbcm_c2(void)
{
    static const char *const lines[] = {
        "A few seconds, hands off.",
        "Does a cached write on one",
        "core show on the other's",
        "cached view? (no video chip)" };
    static const char *const dir[2] = { "CPU>COP", "COP>CPU" };
    struct simplelist_info info;
    struct coplab_cache_trial t;
    uint32_t n[2][COPLAB_C_N], all[COPLAB_C_N], *line;
    int h, d, k, c, rc = 0, v;

    if (!devbcm_confirm(&info, "COP row C2", lines, ARRAYLEN(lines)))
        return false;
    pb_say("=== COP row C2 (shared cache data array): build %s ===",
           rbversion);
    memset(n, 0, sizeof(n));
    memset(all, 0, sizeof(all));
    if (bcm_cop_stuck())
    {
        simplelist_addline("COP stuck earlier: REBOOT first");
        return devbcm_show(&info);
    }
    h = core_alloc(16 * C2_LINES + 16);
    if (h <= 0 || bcm_cop_open() < 0)
    {
        if (h > 0)
            core_free(h);
        pb_say("c2: no %s", h > 0 ? "worker on the COP" : "memory");
        simplelist_addline("Not run: no %s", h > 0 ? "COP worker" : "memory");
        return devbcm_show(&info);
    }
    core_pin(h);
    line = (uint32_t *)ALIGN_UP((uintptr_t)core_get_data(h), 16);
    for (d = 0; d < 2 && rc == 0; d++)
        for (k = 0; k < C2_TRIALS && rc == 0; k++)
        {
            uint32_t *l = line + 4 * (k % C2_LINES);

            rc = bcm_cop_cache_trial(l, d == 0, (uint32_t)(d * 256 + k), &t);
            if (rc)
                break;
            c = coplab_cache_class(&t);
            n[d][c]++;
            all[c]++;
            pb_say("c2: %s trial %d at %08lx: %s (pre %08lx u1 %08lx "
                   "r %08lx p %08lx; v0 %08lx v1 %08lx v9 %08lx)", dir[d], k,
                   (unsigned long)(uintptr_t)l, coplab_cache_name(c),
                   (unsigned long)t.pre, (unsigned long)t.u1,
                   (unsigned long)t.r, (unsigned long)t.p,
                   (unsigned long)t.v0, (unsigned long)t.v1,
                   (unsigned long)t.v9);
        }
    bcm_cop_close();
    core_unpin(h);
    core_free(h);
    for (d = 0; d < 2; d++)
    {
        v = coplab_cache_verdict(n[d]);
        pb_say("c2: %s: %s; separate %lu shared %lu miss %lu setup %lu "
               "through %lu evict %lu odd %lu", dir[d], coplab_verdict_name(v),
               (unsigned long)n[d][0], (unsigned long)n[d][1],
               (unsigned long)n[d][2], (unsigned long)n[d][3],
               (unsigned long)n[d][4], (unsigned long)n[d][5],
               (unsigned long)n[d][6]);
        simplelist_addline("%s: %s", dir[d], coplab_verdict_name(v));
        simplelist_addline("  sep %lu shr %lu void %lu", (unsigned long)n[d][0],
                           (unsigned long)n[d][1],
                           (unsigned long)(n[d][2] + n[d][3] + n[d][4] +
                                           n[d][5] + n[d][6]));
    }
    v = coplab_cache_verdict(all);
    pb_say("c2: %s (rc %d)", coplab_verdict_name(v), rc);
    simplelist_addline("C2: %s", coplab_verdict_name(v));
    if (rc)
        simplelist_addline("Failed (%d)%s", rc,
                           bcm_cop_stuck() ? ": REBOOT" : "");
    return devbcm_show(&info);
}

#define C3_MS   1000                /* each window */

bool devbcm_c3(void)
{
    static const char *const lines[] = {
        "About 5 s, hands off; the",
        "screen freezes while it counts.",
        "Does the COP sleep while it",
        "polls with yield()?" };
    static const char *const how[COPLAB_W_N] = { "yield", "sleep(1)",
                                                 "blocking" };
    struct simplelist_info info;
    struct coplab_sleep s[COPLAB_W_N];
    int k, rc = 0, v;

    if (!devbcm_confirm(&info, "COP row C3", lines, ARRAYLEN(lines)))
        return false;
    pb_say("=== COP row C3 (COP sleep under yield polling): build %s ===",
           rbversion);
    for (k = 0; k < COPLAB_W_N; k++)
        coplab_sleep_init(&s[k]);
    if (bcm_cop_stuck() || bcm_cop_open() < 0)
    {
        pb_say("c3: no worker on the COP");
        simplelist_addline("Not run: no COP worker%s",
                           bcm_cop_stuck() ? " (REBOOT)" : "");
        return devbcm_show(&info);
    }
    for (k = 0; k < COPLAB_W_N && rc == 0; k++)
    {
        uint32_t idle;

        rc = bcm_cop_sleep_window(k, C3_MS, &s[k]);
        idle = coplab_idle_permille(&s[k]);
        pb_say("c3: %s: rc %d, asleep %lu.%lu %% of %lu samples, %lu sleep "
               "entries, %lu waits, kernel idle %s%lu.%lu %%, window %lu us",
               how[k], rc, (unsigned long)(coplab_sleep_permille(&s[k]) / 10),
               (unsigned long)(coplab_sleep_permille(&s[k]) % 10),
               (unsigned long)s[k].samples, (unsigned long)s[k].entries,
               (unsigned long)s[k].loops, idle == ~0u ? "n/a " : "",
               (unsigned long)(idle == ~0u ? 0 : idle / 10),
               (unsigned long)(idle == ~0u ? 0 : idle % 10),
               (unsigned long)s[k].window_us);
        simplelist_addline("%s: asleep %lu %%, %lu entries", how[k],
                           (unsigned long)(coplab_sleep_permille(&s[k]) / 10),
                           (unsigned long)s[k].entries);
    }
    bcm_cop_close();
    v = coplab_sleep_verdict(&s[COPLAB_W_YIELD], &s[COPLAB_W_BLOCK]);
    pb_say("c3: %s (rc %d)", v == COPLAB_S_AWAKE ? "yield keeps the COP awake"
           : v == COPLAB_S_SLEEPS ? "the COP sleeps under yield" : "unclear",
           rc);
    simplelist_addline("C3: %s", v == COPLAB_S_AWAKE ? "yield: COP awake"
                       : v == COPLAB_S_SLEEPS ? "yield: COP sleeps"
                       : "unclear");
    if (rc)
        simplelist_addline("Failed (%d)%s", rc,
                           bcm_cop_stuck() ? ": REBOOT" : "");
    return devbcm_show(&info);
}

#endif /* HAVE_BCMFW_VIDEO */
