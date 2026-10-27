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

#include "config.h"
#include <stdio.h>
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "backlight.h"
#include "powermgmt.h"
#include "audio.h"
#include "metadata.h"
#include "power.h"
#include "lang.h"
#include "settings.h"
#include "ipodui/anim.h"
#include "ipodui/uilog.h"
#include "ipodui/panelart.h"
#include "ipodui/screens/screens.h"
#include "ipodui/np_layout.h"
#include "ipodui/np_playback.h"
#include "ipodui/fe_be.h"        /* fe_settings_save (P19) */
#include "ipodui/viz.h"
#include "ipodui/viz_core.h"
#include "ipodui/viz_tap.h"
#include "jviz.h"
/* The BCM path (VC first: the owner, 2026-10-26, for 0.8.0): SPECTRUM,
 * BARS and the line ops come from an overlay, the merged image's
 * viz-ovl-m.bin (or a developer build's display-only image's viz-ovl.bin),
 * which the art thread reads and the job owner's pass puts to the BCM
 * (art_bcm_viz_ask(); visualizer.md 8.5).  The merged image's DECODE
 * shares the overlay's region, so a decode (a track change's art) evicts
 * it: the view holds its last frame while the load is owed (VIZ_WAIT_MAX
 * at most), never the CPU.  The CPU path is the last-resort net, logged
 * loudly ("CPU FALLBACK why=..."): an image with no overlay for it
 * ("why=noop"), a refused load (the driver's lines say why: "bcmjob
 * overlay refused why=..."), a wait past the bound ("why=wait"). */
#if defined(HAVE_BCM_JOBS_MODEL) || defined(HAVE_BCM_JOBS)
#define VIZ_BCM
#include "jobs.h"
#include "bcmjob.h"
#include "bcmjob_pp.h"
#include "ipodui/art_bcm.h"         /* art_bcm_viz_ask(), art_bcm_busy() */
#endif

/* Colours (owner, 2026-10-25: a dark theme): the bars glow on near-black,
 * a deep blue foot to an ice-blue top, white caps, each bar in a halo a
 * third lit (GLOW_W / 256) */
#define C_BG        LCD_RGBPACK(0x04, 0x06, 0x0c)
#define C_BOT       LCD_RGBPACK(0x10, 0x48, 0xd8)
#define C_TOP       LCD_RGBPACK(0x86, 0xe6, 0xff)
#define C_CAP       LCD_RGBPACK(0xf0, 0xf8, 0xff)
#define GLOW_W      96
#define BAR_W       10
#define BAR_GAP     3
#define CAP_H       2
#define TOP_PAD     12              /* the loudest bar's cap stays this far
                                       under the panel's top edge */
#define SPEC_PX_S   60              /* Spectrogram: rows a second (4 s of
                                       history on the panel) */
#define SPEC_ROWS   3               /* ... at most this many new rows a
                                       frame (the BCM's row slot) */
#define RAYS        48              /* Radial: rays, 7.5 degrees apart */
#define RAY_R0      34              /* ... the ring's radius */
#define RAY_L       78              /* ... the longest ray */
#define SCOPE_A     88              /* Scope: px at full scale */

#define FPS_BCM     30
#define FPS_CPU     15
/* the BCM path held (its overlay owed, a decode in flight) this long at
 * most before the CPU draws (logged "why=wait") */
#define VIZ_WAIT_MAX (3 * HZ)
#define N_BCM       512
#define N_CPU       256

/* The view's state in the screen arena (struct + the tap's ring) */
struct viz_arena {
    struct viz_dyn dyn;
    uint16_t edges[VIZ_BANDS_MAX + 1];
    uint16_t l2[VIZ_BANDS_MAX];
    uint8_t  height[VIZ_BANDS_MAX], peak[VIZ_BANDS_MAX];
    uint32_t rate;                  /* the edges' rate, 0 none */
    uint16_t n;                     /* the edges' window */
    uint8_t  nb;
    uint8_t  pad;
};
#define VIZ_ARENA_STATE ((sizeof(struct viz_arena) + 31u) & ~31u)
#ifndef SIMULATOR
/* the arena bound (tests/arena sums it): the state and the device's ring
 * (viz_tap_bytes(512) = 4,143 B: visualizer.md 11.2) */
_Static_assert(VIZ_ARENA_STATE + 4143 <= UI_ARENA_B_VIZ, "viz: arena bound");
#endif

/* the bars' rect is the panel (heights are uint8_t: at most 255 rows) */
_Static_assert(VIZ_Y == 0 && VIZ_H == LCD_HEIGHT && VIZ_H <= 255,
               "viz: full-screen rect");

/* the Spectrogram's new rows (draw_bcm) in the same scratch */
_Static_assert(VIZ_W * SPEC_ROWS * 2 <= NP_STRIP_VIZ_SIZE, "viz rows");

/* the per-frame scratch in the strip: the window, then the powers */
_Static_assert(N_BCM * 2 + JVIZ_M_MAX * 4 <= NP_STRIP_VIZ_SIZE,
               "viz scratch");

static struct viz_arena *va;        /* NULL: none this push */
static uint8_t clip_h = VIZ_H;      /* viz_clip(): the rows drawn */
static size_t va_bytes;
static struct {
    bool on;                        /* the view runs */
    bool bcm;                       /* SPECTRUM on the BCM this frame */
    bool cpu_logged;
    bool spec_clear;                /* the Spectrogram's history restarts */
    bool hold;                      /* Scope: no new samples for a moment
                                       (between the mixer's chunks): the
                                       last line stays, not a flat one */
    bool scope_have;                /* Scope drew a window since its start */
    uint8_t stale_n;                /* frames in a row with no new samples */
    uint8_t style_logged;           /* bit s: style s's CPU line logged */
    unsigned spec_acc;              /* Spectrogram: row time, px x 1000 */
    unsigned n;                     /* the tap's window */
    long next, last, idle;
#ifdef VIZ_BCM
    bool owned, slot_ok, rslot_ok, lslot_ok;
    bool quiet, window_now;         /* this frame: no samples; the window
                                       put to its slot (Scope reads it) */
    bool scope_q_ok;                /* scope_q drew a window in the slot */
    struct bcmjob_poly scope_q;     /* Scope's last POLYLINE (its halo) */
    struct bcmjob_slot rslot;       /* Spectrogram: its new rows */
    struct bcmjob_slot lslot;       /* Spectrogram: PALROW's LUT (heat) */
    uint8_t owned_h;                /* the owned rect's rows (the clip) */
    uint32_t fence;
    uint32_t spec_bad;              /* SPECTRUM failed in this epoch: the
                                       CPU's until the epoch moves */
    bool wait;                      /* bcm_ops(): its overlay is owed */
    uint32_t ep;                    /* the epoch the fence is of */
    long wait_since;                /* the hold's start (0: none) */
    struct bcmjob_slot slot;
#endif
} vz;

/* the playing track is a book or a podcast episode (np_pb_is_book(), the
 * audiobook Now Playing's own test) */
static bool playing_book(void)
{
    struct mp3entry *id3 = audio_current_track();
    bool podcast;

    return id3 && id3->path[0] && np_pb_is_book(id3, &podcast);
}

int viz_style(void)
{
    int s = global_settings.ipodui_viz_style;

    return viz_style_shown(s) ? s : VIZ_STYLE_BARS;
}

static void hold_light(bool on)
{
    UPDATE_TRACE("viz light hold=%d", (int)on);
    if (on)
    {
        backlight_set_timeout(0);
#if CONFIG_CHARGING
        backlight_set_timeout_plugged(0);
#endif
        backlight_on();
    }
    else
    {
        backlight_set_timeout(global_settings.backlight_timeout);
#if CONFIG_CHARGING
        backlight_set_timeout_plugged(global_settings.backlight_timeout_plugged);
#endif
    }
}

static void draw_cpu(int style, const int16_t *pcm, unsigned step);
#if defined(HAVE_BCM_JOBS_MODEL) || defined(HAVE_BCM_JOBS)
static int window_put(const int16_t *pcm);
#endif

#ifdef VIZ_BCM
/* SPECTRUM (and with bars, BARS and PRESENT) offered and not marked: the
 * overlay loaded (the device's and the simulator's model alike).  load:
 * missing, ask the art thread for it (art_bcm_viz_ask(): at most a try
 * an epoch, the file read off the UI thread); vz.wait: it is owed */
static bool bcm_ops(bool bars, bool load)
{
    if (load)
        vz.wait = false;
    if (bcmjob_have(J_OP_SPECTRUM))
    {
        /* failed in this epoch with the op still offered: the CPU's
           until the epoch moves (an overlay that went is loaded again
           below instead: its op is no longer offered) */
        if (vz.spec_bad == bcmjob_epoch())
            return false;
    }
    else
    {
        if (!load)
            return false;
        switch (art_bcm_viz_ask())
        {
        case ART_VIZ_LOADED:
            break;
        case ART_VIZ_WAIT:
            vz.wait = true;
            return false;
        default:
            /* the BCM restarting under a running view (no jobs at all
               for now: a cut, a restart): held as an owed load, its
               bound the same */
            if (vz.bcm && !bcmjob_have(J_OP_FENCE))
                vz.wait = true;
            return false;
        }
    }
    return !bars || (bcmjob_have(J_OP_BARS) && bcmjob_have(J_OP_PRESENT));
}

/* Hand the rect back: its last PRESENT retired, then the CPU's again */
static void bcm_release(void)
{
    if (vz.owned)
    {
        if (vz.fence)
        {
            int r = bcmjob_wait(vz.fence, 50);

            if (r != BCMJOB_OK)
                ui_log("viz", "release wait rc=%d", r);
        }
        vz.fence = 0;
        vz.owned = false;
        bcmjob_owned_set(0, 0, 0, 0);
        /* the framebuffer's copy of the rect is stale (the simulator
         * poisons it): the same frame by the CPU, so a capture or a
         * band push shows what the panel shows */
        vz.spec_clear = true;       /* the history was the BCM's */
        if (va)
            draw_cpu(viz_style(), NULL, 0);
    }
}
#endif

static void cpu_fallback(const char *why)
{
    if (!vz.cpu_logged)
    {
        vz.cpu_logged = true;
        ui_log("viz", "CPU FALLBACK why=%s n=%u fps=%d", why, N_CPU, FPS_CPU);
    }
}

static bool tap_open(unsigned n)
{
    /* the ring after the state in the arena, or (the arena short of it:
     * the simulator's 1,024-frame mixer needs 16 KB) the tap's own
     * fallback rule: a core_alloc only if it leaves playback alone */
    size_t room = va_bytes - VIZ_ARENA_STATE;
    void *ring = room ? (char *)va + VIZ_ARENA_STATE : NULL;
    int r;

    if (viz_tap_is_open())
    {
        if (vz.n == n)
            return true;
        viz_tap_close();
    }
    r = viz_tap_open(n, ring, room);
    if (r != VIZ_TAP_OK)
    {
        ui_log("viz", "tap open rc=%d n=%u", r, n);
        return false;
    }
    vz.n = n;
    return true;
}

bool viz_start(void)
{
    if (playing_book())
        return false;               /* never for audiobooks (owner) */
    if (!va)
    {
        size_t need = VIZ_ARENA_STATE + viz_tap_bytes(N_BCM);

        /* asked first, so a short arena is no "arena full" line */
        if (ui_arena_free() >= need)
            va_bytes = need;
        else
        {
            /* the state only; the ring by the tap's fallback rule */
            va_bytes = VIZ_ARENA_STATE;
            ui_log("viz", "ring outside the arena need=%u free=%u",
                   (unsigned)viz_tap_bytes(N_BCM),
                   (unsigned)ui_arena_free());
        }
        va = ui_arena_free() >= va_bytes ? ui_arena_alloc(va_bytes) : NULL;
        if (!va)
            return false;
        viz_dyn_init(&va->dyn);
    }
    memset(&vz, 0, sizeof(vz));
#ifdef VIZ_BCM
    /* the view owns the BCM's overlay region and its cadence while open
       (sitting 4c: the art's decodes swapped DECODE's overlay in and out
       under it): the art side's chip work waits for the close */
    art_bcm_viz_view(true);
    /* owed: the BCM's window and rate from the start; the frames hold
       until it lands */
    vz.bcm = bcm_ops(false, true) || vz.wait;
#endif
    if (!tap_open(vz.bcm ? N_BCM : N_CPU))
    {
        viz_stop();                 /* the arena part back */
        return false;
    }
    vz.on = true;
    vz.spec_clear = true;
    vz.next = vz.last = vz.idle = current_tick;
    hold_light(true);
#ifdef VIZ_BCM
    UPDATE_TRACE("viz start style=%d path=%s n=%u%s", viz_style(),
                 vz.bcm ? "bcm" : "cpu", vz.n, vz.wait ? " owed" : "");
#else
    UPDATE_TRACE("viz start style=%d path=%s n=%u", viz_style(), "cpu", vz.n);
#endif
#ifdef VIZ_BCM
    if (!vz.bcm)
        cpu_fallback(bcmjob_have(J_OP_FENCE) ? "noop" : "nojobs");
#else
    cpu_fallback("nodriver");
#endif
    return true;
}

void viz_stop(void)
{
#ifdef VIZ_BCM
    art_bcm_viz_view(false);        /* the art's chip work resumes */
#endif
    if (vz.on)
    {
        vz.on = false;
#ifdef VIZ_BCM
        bcm_release();
#endif
        viz_tap_close();
        hold_light(false);
        UPDATE_TRACE("viz stop%s", "");
    }
    /* the view's arena part goes back at once (Now Playing is on top
     * whenever the view stops: its tick, its input, its mode change), so
     * a list pushed over Now Playing has the room */
    if (va)
        ui_arena_pop(va, va_bytes);
    va = NULL;
    va_bytes = 0;
}

bool viz_on(void)
{
    return vz.on;
}

void viz_forget(void)
{
    /* Now Playing leaves: the stack resets the arena after leave() */
    viz_stop();
}

unsigned viz_bg(void)
{
    return C_BG;
}

void viz_paint_bg(void)
{
    lcd_set_foreground(C_BG);
    lcd_fillrect(0, 0, LCD_WIDTH, LCD_HEIGHT);
}

void viz_clip(int h)
{
    h = MAX(1, MIN(h, VIZ_H));
    if (h == clip_h)
        return;
#ifdef VIZ_BCM
    if (vz.owned && h < vz.owned_h)
    {
        /* the foot goes to the CPU before it draws there: the last
         * PRESENT retired, then the smaller rect owned */
        if (vz.fence && bcmjob_wait(vz.fence, 50) != BCMJOB_OK)
            ui_log("viz", "clip wait failed");
        vz.fence = 0;
        vz.owned_h = (uint8_t)h;
        bcmjob_owned_set(VIZ_X, VIZ_Y, VIZ_W, h);
    }
#endif
    UPDATE_TRACE("viz clip h=%d", h);
    clip_h = (uint8_t)h;
}

/* The clip's heights and peaks: the full-height bars less the hidden
 * foot (off rows), so the rows drawn match the full-height bars */
static void clip_of(const uint8_t *h, const uint8_t *p, unsigned nb,
                    unsigned off, uint8_t *ho, uint8_t *po)
{
    unsigned i;

    for (i = 0; i < nb; i++)
    {
        ho[i] = h[i] > off ? (uint8_t)(h[i] - off) : 0;
        po[i] = p[i] > off ? (uint8_t)(p[i] - off) : 0;
    }
}

/* the gradient's foot colour for a rect clipped by off rows of rows: the
 * colour the full rect has at that row */
static uint16_t clip_bot(uint16_t bot, uint16_t top, unsigned off,
                         unsigned rows)
{
    return off ? (uint16_t)jviz_mix565(bot, top, off * 256u / rows) : bot;
}

static void bars_of(struct jviz_bars *b, unsigned nb, uint8_t *h, uint8_t *p,
                    bool fade)
{
    b->n = nb;
    b->barw = BAR_W;
    b->gap = BAR_GAP;
    b->cap_h = CAP_H;
    b->flags = JVIZ_BARS_CAP | JVIZ_BARS_ROUND;
    b->bot = fade ? viz_fade565(C_BOT, C_BG) : C_BOT;
    b->top = fade ? viz_fade565(C_TOP, C_BG) : C_TOP;
    b->cap = fade ? viz_fade565(C_CAP, C_BG) : C_CAP;
    b->bg = C_BG;
    b->height = h;
    b->peak = p;
}

/* The glow: each bar's halo, one pixel wider on each side and one row
 * taller, the bar's colours a third lit over the background (drawn
 * first, with the background fill) */
static void glow_of(struct jviz_bars *b, unsigned nb, uint8_t *h,
                    uint8_t *zero, uint16_t bot)
{
    b->n = nb;
    b->barw = BAR_W + 2;
    b->gap = BAR_GAP - 2;
    b->cap_h = 0;
    b->flags = JVIZ_BARS_BG | JVIZ_BARS_ROUND;
    b->bot = (uint16_t)jviz_mix565(C_BG, bot, GLOW_W);
    b->top = (uint16_t)jviz_mix565(C_BG, C_TOP, GLOW_W);
    b->cap = C_BG;
    b->bg = C_BG;
    b->height = h;
    b->peak = zero;
}

static void glow_heights(const uint8_t *h, unsigned nb, unsigned rows,
                         uint8_t *g, uint8_t *zero)
{
    unsigned i;

    for (i = 0; i < nb; i++)
    {
        g[i] = h[i] ? (uint8_t)MIN(h[i] + 1u, rows) : 0;
        zero[i] = 0;
    }
}

/* the background over the rect's first rows (the CPU's framebuffer) */
static void fill_rows(fb_data *fb, unsigned rows)
{
    unsigned i, n = rows * LCD_WIDTH;

    for (i = 0; i < n; i++)
        fb[i] = C_BG;
}

/* Bars on the CPU: the halo with the background, then the bars, rows
 * high (heights from the rect's bottom), the gradient's foot bot */
static void bars_cpu(fb_data *fb, unsigned rows, uint8_t *h, uint8_t *p,
                     unsigned nb, uint16_t bot)
{
    uint8_t g[VIZ_BANDS_MAX], z[VIZ_BANDS_MAX];
    struct jviz_bars b;

    glow_heights(h, nb, rows, g, z);
    glow_of(&b, nb, g, z, bot);
    jviz_bars_draw(fb, LCD_WIDTH, VIZ_W, rows, &b, NULL);
    bars_of(&b, nb, h, p, false);
    b.bot = bot;
    jviz_bars_draw(fb, LCD_WIDTH, VIZ_W, rows, &b, NULL);
}

/* The spectrogram's heat: v 0..256 through five stops, the background
 * up to white (a glow on black) */
static uint16_t heat(unsigned v)
{
    static const uint16_t stop[5] = {
        C_BG, LCD_RGBPACK(0x0c, 0x24, 0x7c), LCD_RGBPACK(0x14, 0x62, 0xe0),
        LCD_RGBPACK(0x5c, 0xd0, 0xff), LCD_RGBPACK(0xf4, 0xfc, 0xff),
    };
    unsigned k = v >= 256 ? 3 : v / 64;

    return (uint16_t)jviz_mix565(stop[k], stop[k + 1], (v - 64 * k) * 4);
}

/* band i's heat as a byte: PALROW's level, the LUT's index (heat(0..255)) */
static uint8_t band_lv(unsigned i)
{
    return (uint8_t)MIN(va->height[i] * 256u / (VIZ_H - TOP_PAD), 255u);
}

static uint16_t band_heat(unsigned i)
{
    return heat(band_lv(i));
}

/* band i's columns: the full width split nb ways (no gaps) */
static void band_x(unsigned i, unsigned nb, unsigned *x, unsigned *w)
{
    unsigned x0 = i * VIZ_W / nb;

    *x = x0;
    *w = (i + 1) * VIZ_W / nb - x0;
}

/* Radial: 48 rays round a ring, the 24 bands down the right and mirrored
 * down the left (lows at the top), each a 3-pixel ray from the ring out,
 * the gradient along it; a dim dotted ring inside; the peak a bright dot
 * just past it.  One definition for the BCM and the CPU: SEGMENTS'
 * (jviz.h jviz_segs_draw).  len, pk: RAYS bytes each */
static void radial_of(struct jviz_segs *s, uint8_t *len, uint8_t *pk)
{
    unsigned k, b, p, nb = va->nb, hmax = VIZ_H - TOP_PAD;

    for (k = 0; k < RAYS; k++)
    {
        b = (k < RAYS / 2 ? k : RAYS - 1 - k) * nb / (RAYS / 2);
        len[k] = nb ? (uint8_t)(va->height[b] * RAY_L / hmax) : 0;
        p = nb ? va->peak[b] * RAY_L / hmax : 0;
        pk[k] = p ? (uint8_t)(p + 3) : 0;       /* the dot clear of the tip */
    }
    s->n = RAYS;
    s->a0 = 0;
    s->astep = 1;
    s->width = 3;
    s->flags = JVIZ_SEG_CAP | JVIZ_SEG_RING;
    s->r0 = RAY_R0;
    s->lmax = RAY_L;
    s->rring = RAY_R0 - 7;
    s->capd = 3;
    s->ringd = 2;
    s->cx = VIZ_W / 2;
    s->cy = VIZ_H / 2;
    s->c0 = C_BOT;
    s->c1 = C_TOP;
    s->cap = C_CAP;
    s->ring = (uint16_t)jviz_mix565(C_BG, C_BOT, 160);
    s->len = len;
    s->peak = pk;
}

static void radial_cpu(fb_data *fb, unsigned rows)
{
    uint8_t len[RAYS], pk[RAYS];
    struct jviz_segs s;

    fill_rows(fb, rows);
    radial_of(&s, len, pk);
    jviz_segs_draw(fb, LCD_WIDTH, VIZ_W, rows, &s);
}

/* Scope: the window's newest half as one line across the panel, held
 * still by a rising zero crossing; a dim three-pixel halo, then a bright
 * two-pixel core.  One definition for the BCM and the CPU: POLYLINE's
 * (jviz.h jviz_poly_draw), the samples read in the window's split order
 * (viz_tap_read; on the BCM SPECTRUM's slot as it stands).  The rect is
 * the clip's rows, the line through their middle.  pcm NULL (quiet): a
 * flat line, two points of one element at gain 0 */
static int scope_s(const int16_t *pcm, unsigned n, unsigned k)
{
    return (k & 1) ? pcm[n / 2 + k / 2] : pcm[k / 2];
}

static void scope_of(struct jviz_poly *p, const int16_t *pcm)
{
    static const int16_t flat;
    unsigned n = vz.n, trig = 0, k;
    int lo = 0, s;

    if (pcm)
        for (k = 1; k < n / 2; k++)
        {
            s = scope_s(pcm, n, k);
            lo = MIN(lo, s);
            if (lo < -1024 && scope_s(pcm, n, k - 1) < 0 && s >= 0)
            {
                trig = k;
                break;
            }
        }
    p->src = pcm ? (const void *)pcm : (const void *)&flat;
    p->len = pcm ? n : 1;
    p->n = pcm ? n / 2 : 2;
    p->off = trig;
    p->step = pcm ? 1 : 0;
    p->half = pcm ? n / 2 : 0;
    p->gain = pcm ? 2 * SCOPE_A : 0;    /* SCOPE_A rows at full scale */
    p->flags = 0;
    p->width = 3;
    p->colour = (uint16_t)jviz_mix565(C_BG, C_TOP, GLOW_W);
}

static void scope_cpu(fb_data *fb, unsigned rows, const int16_t *pcm)
{
    struct jviz_poly p;

    fill_rows(fb, rows);
    scope_of(&p, pcm);
    jviz_poly_draw(fb, LCD_WIDTH, VIZ_W, rows, &p);
    p.width = 2;
    p.colour = C_TOP;
    jviz_poly_draw(fb, LCD_WIDTH, VIZ_W, rows, &p);
}

/* Spectrogram on the CPU: the rect's rows move down step rows, the bands'
 * heat fills the new top rows (the history lives in the framebuffer;
 * vz.spec_clear starts it again) */
static void spectro_cpu(fb_data *fb, unsigned rows, unsigned step)
{
    unsigned i, r, x, w, nb = va->nb;

    if (vz.spec_clear)
    {
        fill_rows(fb, rows);
        vz.spec_clear = false;
    }
    if (!step || !nb)
        return;
    step = MIN(step, rows);
    memmove(fb + step * LCD_WIDTH, fb,
            (rows - step) * LCD_WIDTH * sizeof(fb_data));
    for (i = 0; i < nb; i++)
    {
        uint16_t c = band_heat(i);

        band_x(i, nb, &x, &w);
        for (r = 0; r < step; r++)
        {
            fb_data *d = fb + r * LCD_WIDTH + x;
            unsigned j;

            for (j = 0; j < w; j++)
                d[j] = c;
        }
    }
}

/* Mirror's rows (viz_mirror_split): the bars bh, the reflection rh under
 * a gap row; returned, the reflection's rows above the clip ch (the clip
 * only ever cuts the reflection: NP_ROW_Y 198 > bh 167) */
static unsigned mirror_rows(unsigned ch, unsigned *bh, unsigned *rh)
{
    viz_mirror_split(VIZ_H, bh, rh);
    return ch > *bh + 1 ? MIN(*rh, ch - *bh - 1) : 0;
}

/* The reflection, rv of its rh rows shown: h (the bars' heights, scaled)
 * clamped to rv, faded, no cap or rounding, upside down (JVIZ_BARS_FLIP:
 * the rect's rows addressed bottom up, on the CPU and the BCM alike), the
 * gradient's top at row rv of the full rh; p zeroed */
static void refl_of(struct jviz_bars *b, unsigned nb, uint8_t *h, uint8_t *p,
                    unsigned rv, unsigned rh)
{
    unsigned i;

    for (i = 0; i < nb; i++)
    {
        h[i] = (uint8_t)MIN(h[i], rv);
        p[i] = 0;
    }
    bars_of(b, nb, h, p, true);
    b->top = (uint16_t)jviz_mix565(b->bot, b->top, rv * 256u / rh);
    b->flags = JVIZ_BARS_BG | JVIZ_BARS_FLIP;
}

/* The CPU's draw into the framebuffer, rows 0 to the clip */
static void draw_cpu(int style, const int16_t *pcm, unsigned step)
{
    /* not ownable: called only with the BCM's rect handed back (viz_frame() releases it first; bcm_release() clears it before) or never owned */
    fb_data *fb = FBADDR(VIZ_X, VIZ_Y);
    struct jviz_bars b;
    unsigned nb = va->nb, i, bh, rh, rv, ch = clip_h;

    switch (style)
    {
    case VIZ_STYLE_SCOPE:
        scope_cpu(fb, ch, pcm);
        return;
    case VIZ_STYLE_SPECTRO:
        spectro_cpu(fb, ch, step);
        return;
    case VIZ_STYLE_RADIAL:
        radial_cpu(fb, ch);
        return;
    case VIZ_STYLE_MIRROR:
        break;
    default:
    {
        uint8_t h[VIZ_BANDS_MAX], p[VIZ_BANDS_MAX];

        clip_of(va->height, va->peak, nb, VIZ_H - ch, h, p);
        bars_cpu(fb, ch, h, p, nb, clip_bot(C_BOT, C_TOP, VIZ_H - ch, VIZ_H));
        return;
    }
    }
    /* Mirror: the clip (NP_ROW_Y while the volume shows) only ever cuts
     * the reflection, whose visible rows rv are its top ones: the bars'
     * lowest rv rows, flipped */
    rv = mirror_rows(ch, &bh, &rh);
    {
        uint8_t h[VIZ_BANDS_MAX], p[VIZ_BANDS_MAX];
        fb_data *r = fb + (bh + 1) * LCD_WIDTH;

        for (i = 0; i < nb; i++)
        {
            h[i] = (uint8_t)(va->height[i] * bh / VIZ_H);
            p[i] = (uint8_t)(va->peak[i] * bh / VIZ_H);
        }
        bars_cpu(fb, bh, h, p, nb, C_BOT);
        /* the gap row */
        if (ch > bh)
            for (i = 0; i < VIZ_W; i++)
                fb[bh * LCD_WIDTH + i] = C_BG;
        if (!rv)
            return;
        refl_of(&b, nb, h, p, rv, rh);
        jviz_bars_draw(r, LCD_WIDTH, VIZ_W, rv, &b, NULL);
    }
}

#ifdef VIZ_BCM
/* What each style needs from the BCM (the VC-first rule): Bars, BARS
 * twice (the halo, then the bars) and PRESENT; Mirror, those two, the
 * gap row's FILL and a flipped BARS (J_BARS_FLIP: bcmjob_bars_flip());
 * Spectrogram, SCROLL, PALROW (the new rows from the levels, a LUT slot
 * uploaded once an epoch; else one BLIT of them from a row slot) and
 * PRESENT, FILL once to start its history; Scope, FILL and POLYLINE
 * twice (from SPECTRUM's slot); Radial, FILL and SEGMENTS.  An image
 * without an op: that style on the CPU, logged once (why=style) */
static bool style_on_bcm(int style)
{
    switch (style)
    {
    case VIZ_STYLE_BARS:
        return bcm_ops(true, false);
    case VIZ_STYLE_MIRROR:
        return bcm_ops(true, false) && bcmjob_have(J_OP_FILL)
               && bcmjob_bars_flip();
    case VIZ_STYLE_SPECTRO:
        return bcmjob_have(J_OP_SCROLL) && bcmjob_have(J_OP_FILL)
               && bcmjob_have(J_OP_PRESENT)
               && (bcmjob_have_palrow() || bcmjob_have(J_OP_BLIT));
    case VIZ_STYLE_SCOPE:
        return bcm_ops(false, false) && bcmjob_have_polyline()
               && bcmjob_have(J_OP_FILL) && bcmjob_have(J_OP_PRESENT);
    case VIZ_STYLE_RADIAL:
        return bcm_ops(false, false) && bcmjob_have_segments()
               && bcmjob_have(J_OP_FILL) && bcmjob_have(J_OP_PRESENT);
    default:
        return false;
    }
}

_Static_assert(JVIZ_POLY_U8 == J_POLY_U8 && JVIZ_SEG_CAP == J_SEG_CAP &&
               JVIZ_SEG_RING == J_SEG_RING && JVIZ_SEG_ROUND == J_SEG_ROUND &&
               RAYS <= J_SEG_N_MAX && VIZ_BANDS_MAX <= J_PAL_N_MAX,
               "viz: the line ops' flags and bounds are the wire's");
_Static_assert(JVIZ_BARS_BG == J_BARS_BG && JVIZ_BARS_CAP == J_BARS_CAP &&
               JVIZ_BARS_ROUND == J_BARS_ROUND && JVIZ_BARS_FLIP == J_BARS_FLIP,
               "viz: the bars' flags are BARS'");

static bool bcm_bars(uint32_t *p, unsigned y, unsigned nb, unsigned ch,
                     const uint8_t *h,
                     const uint8_t *pk, unsigned barw, unsigned gap,
                     unsigned cap_h, uint16_t bot, uint16_t top, uint16_t cap,
                     uint32_t flags)
{
    unsigned i;

    p[0] = J_SURF_FRONT;
    p[1] = J_XY(VIZ_X, y);
    p[2] = J_XY(VIZ_W, ch);
    p[3] = nb | barw << 8 | gap << 16 | cap_h << 24;
    p[4] = bot | (uint32_t)top << 16;
    p[5] = cap | (uint32_t)C_BG << 16;
    p[6] = flags;
    memset(&p[7], 0, 2 * ((nb + 3) / 4) * 4);
    for (i = 0; i < nb; i++)
    {
        p[7 + i / 4] |= (uint32_t)h[i] << (8 * (i & 3));
        p[7 + (nb + 3) / 4 + i / 4] |= (uint32_t)pk[i] << (8 * (i & 3));
    }
    return bcmjob_submit(J_OP_BARS, 0, p, J_BARS_ARGS(nb)) == BCMJOB_OK;
}

static bool bcm_fill(unsigned x, unsigned y, unsigned w, unsigned h,
                     uint16_t c)
{
    uint32_t p[4];

    p[0] = J_SURF_FRONT;
    p[1] = J_XY(x, y);
    p[2] = J_XY(w, h);
    p[3] = c;
    return bcmjob_submit(J_OP_FILL, 0, p, 4) == BCMJOB_OK;
}

/* A frame on the BCM into the front frame, PRESENT, FENCE; the rect is
 * the BCM's while the view shows */
static bool draw_bcm(int style, unsigned step, char *scratch)
{
    uint32_t p[J_BARS_ARGS(VIZ_BANDS_MAX)];
    uint8_t hc[VIZ_BANDS_MAX], pc[VIZ_BANDS_MAX];
    uint8_t g[VIZ_BANDS_MAX], z[VIZ_BANDS_MAX];
    unsigned nb = va->nb, i, ch = clip_h, x, w;
    uint16_t bot = clip_bot(C_BOT, C_TOP, VIZ_H - ch, VIZ_H);

    if (vz.fence)
    {
        /* the last frame's PRESENT before the next frame's draw (the
         * front frame is read by its transfer) */
        int r = bcmjob_wait(vz.fence, 40);

        if (r != BCMJOB_OK)
        {
            ui_log("viz", "bcm frame wait rc=%d", r);
            return false;
        }
        vz.fence = 0;
    }
    if (style == VIZ_STYLE_SPECTRO)
    {
        if (!vz.owned || vz.spec_clear)
        {
            /* a new history: the front frame's rect held the CPU's */
            if (!bcm_fill(VIZ_X, VIZ_Y, VIZ_W, ch, C_BG))
                return false;
            vz.spec_clear = false;
        }
        step = MIN(step, ch);
        if (step && nb && bcmjob_have_palrow())
        {
            /* SCROLL, then PALROW: the new rows from the levels (no row
               upload); its LUT (heat) uploaded once an epoch */
            uint16_t *lut = (uint16_t *)scratch; /* align-ok: NP_STRIP_VIZ_OFF, a multiple of 32 in a word-aligned block */
            uint8_t lv[VIZ_BANDS_MAX];
            struct bcmjob_palrow q;
            int rc;

            if (!vz.lslot_ok || vz.lslot.epoch != bcmjob_epoch())
            {
                for (i = 0; i < 256; i++)
                    lut[i] = heat(i);
                rc = bcmjob_slot(J_CLS_STRIP, 16, 16, 0x56495a32, 0, &vz.lslot);
                if (rc == BCMJOB_OK)
                    rc = bcmjob_upload(&vz.lslot, lut);
                if (rc != BCMJOB_OK)
                {
                    ui_log("viz", "lut slot rc=%d", rc);
                    return false;
                }
                vz.lslot_ok = true;
            }
            for (i = 0; i < nb; i++)
                lv[i] = band_lv(i);
            p[0] = J_SURF_FRONT;
            p[1] = J_XY(VIZ_X, VIZ_Y);
            p[2] = J_XY(VIZ_W, ch);
            p[3] = step;
            q.surf = J_SURF_FRONT;
            q.x = VIZ_X;
            q.y = VIZ_Y;
            q.w = VIZ_W;
            q.h = (uint16_t)step;
            q.lut = vz.lslot.slot;
            q.nb = (uint8_t)nb;
            q.lv = lv;
            if (bcmjob_submit(J_OP_SCROLL, 0, p, 4) != BCMJOB_OK ||
                bcmjob_viz_palrow(&q) != BCMJOB_OK)
                return false;
        }
        else if (step && nb)
        {
            /* the new rows built here (the frame's scratch: the window
             * and powers are spent), uploaded to a row slot, then
             * SCROLL and one BLIT: four jobs, not one FILL a band */
            uint16_t *row = (uint16_t *)scratch; /* align-ok: NP_STRIP_VIZ_OFF, a multiple of 32 in a word-aligned block */
            unsigned r, j;
            int rc;

            for (i = 0; i < nb; i++)
            {
                uint16_t c = band_heat(i);

                band_x(i, nb, &x, &w);
                for (j = 0; j < w; j++)
                    row[x + j] = c;
            }
            for (r = 1; r < SPEC_ROWS; r++)
                memcpy(row + r * VIZ_W, row, VIZ_W * 2);
            if (!vz.rslot_ok || vz.rslot.epoch != bcmjob_epoch())
            {
                rc = bcmjob_slot(J_CLS_STRIP, VIZ_W, SPEC_ROWS, 0x56495a31,
                                 0, &vz.rslot);
                if (rc != BCMJOB_OK)
                {
                    ui_log("viz", "row slot rc=%d", rc);
                    return false;
                }
                vz.rslot_ok = true;
            }
            vz.rslot.hit = false;   /* new rows every frame */
            rc = bcmjob_upload(&vz.rslot, row);
            if (rc != BCMJOB_OK)
            {
                ui_log("viz", "row upload rc=%d", rc);
                vz.rslot_ok = false;
                return false;
            }
            p[0] = J_SURF_FRONT;
            p[1] = J_XY(VIZ_X, VIZ_Y);
            p[2] = J_XY(VIZ_W, ch);
            p[3] = step;
            if (bcmjob_submit(J_OP_SCROLL, 0, p, 4) != BCMJOB_OK)
                return false;
            p[0] = vz.rslot.slot;
            p[1] = J_XY(0, 0);
            p[2] = J_XY(VIZ_W, step);
            p[3] = J_SURF_FRONT;
            p[4] = J_XY(VIZ_X, VIZ_Y);
            p[5] = J_BLIT_OPAQUE;
            if (bcmjob_submit(J_OP_BLIT, 0, p, 6) != BCMJOB_OK)
                return false;
        }
    }
    else if (style == VIZ_STYLE_SCOPE)
    {
        /* FILL, the halo, the core: the samples are the slot's (put this
           frame by spectrum_bcm; quiet, any: gain 0) */
        struct jviz_poly jp;
        struct bcmjob_poly q;
        bool held = vz.hold && vz.scope_q_ok && vz.slot_ok &&
                    vz.slot.epoch == bcmjob_epoch();

        if (held)                   /* the slot still holds that window */
        {
            q = vz.scope_q;
            q.h = (uint16_t)ch;
        }
        else
        {
            scope_of(&jp, vz.quiet ? NULL : (const int16_t *)scratch); /* align-ok: NP_STRIP_VIZ_OFF, a multiple of 32 in a word-aligned block */
            if (!vz.slot_ok || vz.slot.epoch != bcmjob_epoch() ||
                (!vz.quiet && !vz.window_now))
            {
                if (window_put((const int16_t *)scratch) != BCMJOB_OK) /* align-ok: as above */
                    return false;
            }
            q.surf = J_SURF_FRONT;
            q.x = VIZ_X;
            q.y = VIZ_Y;
            q.w = VIZ_W;
            q.h = (uint16_t)ch;
            q.src = vz.slot.slot;
            q.off = (uint16_t)jp.off;
            q.step = (uint16_t)jp.step;
            q.n = (uint16_t)jp.n;
            q.flags = 0;
            q.gain = (uint16_t)jp.gain;
            q.half = (uint16_t)jp.half;
            q.colour = jp.colour;
            q.width = 3;
            vz.scope_q = q;
            vz.scope_q_ok = !vz.quiet;
        }
        if (!bcm_fill(VIZ_X, VIZ_Y, VIZ_W, ch, C_BG) ||
            bcmjob_viz_polyline(&q) != BCMJOB_OK)
            return false;
        q.colour = C_TOP;
        q.width = 2;
        if (bcmjob_viz_polyline(&q) != BCMJOB_OK)
            return false;
    }
    else if (style == VIZ_STYLE_RADIAL)
    {
        uint8_t len[RAYS], pk[RAYS];
        struct jviz_segs js;
        struct bcmjob_segs q;

        radial_of(&js, len, pk);
        q.surf = J_SURF_FRONT;
        q.x = VIZ_X;
        q.y = VIZ_Y;
        q.w = VIZ_W;
        q.h = (uint16_t)ch;
        q.cx = (int16_t)js.cx;
        q.cy = (int16_t)js.cy;
        q.n = (uint8_t)js.n;
        q.a0 = (uint8_t)js.a0;
        q.astep = (uint8_t)js.astep;
        q.width = (uint8_t)js.width;
        q.flags = (uint8_t)js.flags;
        q.r0 = (uint8_t)js.r0;
        q.lmax = (uint8_t)js.lmax;
        q.rring = (uint8_t)js.rring;
        q.capd = (uint8_t)js.capd;
        q.ringd = (uint8_t)js.ringd;
        q.c0 = js.c0;
        q.c1 = js.c1;
        q.cap = js.cap;
        q.ring = js.ring;
        q.len = len;
        q.peak = pk;
        if (!bcm_fill(VIZ_X, VIZ_Y, VIZ_W, ch, C_BG) ||
            bcmjob_viz_segments(&q) != BCMJOB_OK)
            return false;
    }
    else if (style == VIZ_STYLE_MIRROR)
    {
        /* the bars' pair in bh rows, the gap row's FILL, the reflection's
         * flipped BARS (rv rows above the clip) */
        struct jviz_bars b;
        unsigned bh, rh, rv = mirror_rows(ch, &bh, &rh);

        for (i = 0; i < nb; i++)
        {
            hc[i] = (uint8_t)(va->height[i] * bh / VIZ_H);
            pc[i] = (uint8_t)(va->peak[i] * bh / VIZ_H);
        }
        glow_heights(hc, nb, bh, g, z);
        if (!bcm_bars(p, VIZ_Y, nb, bh, g, z, BAR_W + 2, BAR_GAP - 2, 0,
                      (uint16_t)jviz_mix565(C_BG, C_BOT, GLOW_W),
                      (uint16_t)jviz_mix565(C_BG, C_TOP, GLOW_W), C_BG,
                      J_BARS_BG | J_BARS_ROUND)
            || !bcm_bars(p, VIZ_Y, nb, bh, hc, pc, BAR_W, BAR_GAP, CAP_H,
                         C_BOT, C_TOP, C_CAP, J_BARS_CAP | J_BARS_ROUND)
            || !bcm_fill(VIZ_X, VIZ_Y + bh, VIZ_W, 1, C_BG))
            return false;
        if (rv)
        {
            refl_of(&b, nb, hc, pc, rv, rh);
            if (!bcm_bars(p, VIZ_Y + bh + 1, nb, rv, b.height, b.peak,
                          BAR_W, BAR_GAP, 0, b.bot, b.top, b.cap, b.flags))
                return false;
        }
    }
    else
    {
        clip_of(va->height, va->peak, nb, VIZ_H - ch, hc, pc);
        glow_heights(hc, nb, ch, g, z);
        if (!bcm_bars(p, VIZ_Y, nb, ch, g, z, BAR_W + 2, BAR_GAP - 2, 0,
                      (uint16_t)jviz_mix565(C_BG, bot, GLOW_W),
                      (uint16_t)jviz_mix565(C_BG, C_TOP, GLOW_W), C_BG,
                      J_BARS_BG | J_BARS_ROUND)
            || !bcm_bars(p, VIZ_Y, nb, ch, hc, pc, BAR_W, BAR_GAP, CAP_H, bot,
                         C_TOP, C_CAP, J_BARS_CAP | J_BARS_ROUND))
            return false;
    }
    if (!vz.owned || vz.owned_h != ch)
    {
        vz.owned = true;
        vz.owned_h = (uint8_t)ch;
        bcmjob_owned_set(VIZ_X, VIZ_Y, VIZ_W, ch);
    }
    p[0] = J_SURF_FRONT;
    p[1] = J_XY(VIZ_X, VIZ_Y);
    p[2] = J_XY(VIZ_W, ch);
    p[3] = 0;
    if (bcmjob_submit(J_OP_PRESENT, 0, p, 4) != BCMJOB_OK)
        return false;
    return bcmjob_fence(&vz.fence) == BCMJOB_OK;
}

/* The window to its slot (SPECTRUM's, and Scope's POLYLINE reads it as it
 * stands): BCMJOB_OK or the failure */
static int window_put(const int16_t *pcm)
{
    int r;

    if (!vz.slot_ok || vz.slot.epoch != bcmjob_epoch())
    {
        r = bcmjob_slot(J_CLS_STRIP, N_BCM, 1, 0x56495a30, 0, &vz.slot);
        if (r != BCMJOB_OK)
        {
            ui_log("viz", "slot rc=%d", r);
            return r;
        }
        vz.slot_ok = true;
    }
    /* the last frame's POLYLINEs read the slot: retired first */
    if (vz.fence)
    {
        r = bcmjob_wait(vz.fence, 40);
        if (r != BCMJOB_OK)
        {
            ui_log("viz", "bcm frame wait rc=%d", r);
            return r;
        }
        vz.fence = 0;
    }
    vz.slot.hit = false;            /* new samples every frame */
    return bcmjob_upload(&vz.slot, (const uint16_t *)pcm);
}

/* The window to the slot, SPECTRUM, the levels back: false, the CPU */
static bool spectrum_bcm(const int16_t *pcm)
{
    int r = window_put(pcm);

    if (r == BCMJOB_OK)
        r = bcmjob_spectrum(vz.slot.slot, vz.n, va->edges, va->nb, va->l2);
    if (r != BCMJOB_OK)
    {
        ui_log("viz", "spectrum rc=%d", r);
        vz.slot_ok = false;
        return false;
    }
    return true;
}
#endif

int viz_frame(bool *lost)
{
    long now = current_tick;
    struct viz_tap_frame f;
    unsigned fl, i, ms;
    int16_t *pcm;
    uint32_t *pw;
    char *strip;
    bool kept, quiet, drawn_bcm = false, on_bcm = false;
    int style = viz_style(), period;
    unsigned step;

    if (!vz.on || !va)
        return TIMEOUT_BLOCK;
    if (TIME_BEFORE(now, vz.next))
        return vz.next - now;
#ifdef VIZ_BCM
    {
        bool want = bcm_ops(false, true);

        /* a new epoch (a cut, a restart): the last frame's fence went
           with the ring, the history restarts */
        if (vz.ep != bcmjob_epoch())
        {
            vz.ep = bcmjob_epoch();
            vz.fence = 0;
            vz.spec_clear = true;
        }
        /* its overlay owed (evicted by a decode, or not loaded yet) or a
           decode holding the BCM: the last frame stays, the CPU draws
           nothing; past VIZ_WAIT_MAX the CPU's, loudly */
        if ((vz.bcm || vz.wait) && (vz.wait || (want && art_bcm_busy())))
        {
            if (!vz.wait_since)
            {
                vz.wait_since = now;
                UPDATE_TRACE("viz hold why=%s", vz.wait ? "owed" : "decode");
            }
            if (TIME_BEFORE(now, vz.wait_since + VIZ_WAIT_MAX))
            {
                vz.next = now + HZ / FPS_BCM;
                return HZ / FPS_BCM;
            }
            want = false;
            cpu_fallback("wait");
        }
        else if (vz.wait_since)
        {
            UPDATE_TRACE("viz hold end ms=%ld",
                         (long)(now - vz.wait_since) * (1000 / HZ));
            vz.wait_since = 0;
        }
        if (want != vz.bcm)
        {
            vz.bcm = want;
            if (!want)
                cpu_fallback("lost");
            if (!tap_open(want ? N_BCM : N_CPU))
            {
                viz_stop();
                return TIMEOUT_BLOCK;
            }
        }
    }
#endif
#ifdef VIZ_BCM
    on_bcm = vz.bcm && style_on_bcm(style);
    if (vz.bcm && !on_bcm && !(vz.style_logged & (1u << style)))
    {
        /* VC first: a style the image can't draw yet is the CPU's, said
         * once a start (the analysis stays on the BCM) */
        vz.style_logged |= 1u << style;
        if (style == VIZ_STYLE_MIRROR)  /* BARS without J_BARS_FLIP */
            ui_log("viz", "FALLBACK mirror on CPU why=noflip");
        else
            ui_log("viz", "CPU FALLBACK why=style style=%d fps=%d", style,
                   FPS_CPU);
    }
#endif
    /* the frame rate of the draw: the CPU's draws are 15 fps */
    period = HZ / (on_bcm ? FPS_BCM : FPS_CPU);
    step = 0;                       /* below: the Spectrogram's new rows */
    ms = (unsigned)(now - vz.last) * (1000 / HZ);
    vz.last = now;
    vz.next = now + period;
    strip = ui_strip_claim(UI_STRIP_OWNER_NP, &kept);
    if (!strip)
        return period;
    if (!kept)
        *lost = true;               /* this claim took it back: Now
                                       Playing's parts there are gone */
    pcm = (int16_t *)(strip + NP_STRIP_VIZ_OFF); /* align-ok: the strip is a core_alloc block (word aligned), NP_STRIP_VIZ_OFF a multiple of 32 */
    pw = (uint32_t *)(strip + NP_STRIP_VIZ_OFF + N_BCM * 2); /* align-ok: + 1,024 B, a multiple of 4 */
    fl = viz_tap_read(pcm, VIZ_RD_NORM, &f);
    quiet = (fl & (VIZ_TAP_STALE | VIZ_TAP_SILENT | VIZ_TAP_TORN
                   | VIZ_TAP_BOOK | VIZ_TAP_CLOSED)) != 0;
    /* only stale, for at most 3 frames (100 ms): the Scope holds its line
       (at 30 fps half the reads fall between two of the mixer's chunks);
       longer (paused), silent or closed: flat */
    vz.stale_n = (fl & VIZ_TAP_STALE) ? (uint8_t)MIN(vz.stale_n + 1u, 255u) : 0;
    vz.hold = quiet && vz.scope_have && vz.stale_n <= 3
              && !(fl & ~(VIZ_TAP_STALE | VIZ_TAP_UNDERRUN));
    if (f.rate != va->rate || vz.n != va->n)
    {
        va->nb = (uint8_t)viz_edges(va->edges, VIZ_BANDS, vz.n, f.rate);
        va->rate = f.rate;
        va->n = (uint16_t)vz.n;
    }
    if (!va->nb)
        quiet = true;
    if (!quiet)
    {
        bool done = false;

#ifdef VIZ_BCM
        vz.window_now = false;
        if (vz.bcm)
        {
            done = vz.window_now = spectrum_bcm(pcm);
            if (!done)
            {
                /* the CPU's from the next frame, until the epoch moves;
                   this frame holds (its window is 512 samples, and the
                   PP's build has only the 256-sample tables) */
                cpu_fallback("spectrum");
                vz.spec_bad = bcmjob_epoch();
                quiet = true;
            }
        }
        if (!done && !quiet)
#else
        if (!done)
#endif
        {
            jviz_power_ref(pw, pcm, vz.n);
            jviz_bands(va->l2, pw, vz.n / 2, va->edges, va->nb);
        }
        for (i = 0; !quiet && i < va->nb; i++)  /* undo the tap's shift */
            va->l2[i] = va->l2[i] > 32u * f.shift
                        ? va->l2[i] - 32u * f.shift : 0;
    }
    /* the Spectrogram's rows by the time gone (60 a second), at most
     * SPEC_ROWS a frame */
    vz.spec_acc += SPEC_PX_S * (ms ? ms : 33);
    step = MIN(vz.spec_acc / 1000, SPEC_ROWS);
    vz.spec_acc = MIN(vz.spec_acc - step * 1000, 1000);
    viz_dyn_step(&va->dyn, va->l2, va->nb, quiet, VIZ_H - TOP_PAD,
                 ms ? ms : 33,
                 va->height, va->peak);
#ifdef VIZ_BCM
    if (on_bcm)
    {
        vz.quiet = quiet;
        drawn_bcm = draw_bcm(style, step, strip + NP_STRIP_VIZ_OFF);
        if (!drawn_bcm)
            cpu_fallback("bars");
    }
    if (!drawn_bcm)
        bcm_release();
#endif
    if (!drawn_bcm)
        if (!(style == VIZ_STYLE_SCOPE && vz.hold))  /* held: the frame stays */
            draw_cpu(style, quiet ? NULL : pcm, step);
    vz.scope_have = style == VIZ_STYLE_SCOPE && (vz.scope_have || !quiet);
    ui_strip_put();
    if (!drawn_bcm)
        ui_lcd_push("np:viz", 0, VIZ_Y, LCD_WIDTH, clip_h);
    if (TIME_AFTER(now, vz.idle + HZ))
    {
        vz.idle = now;
        reset_poweroff_timer();     /* no idle sleep while it shows */
    }
    return period;
}

/* ---- the hold list (long SELECT in the view) ----------------------------- */

/* The styles shown (viz_core.h), then While Charging (owner, 2026-10-25:
 * the charger's face is chosen here, not in Settings): its own short
 * list, Clock or Visualizer (ipodui_charge_face, unlit.c).  A style
 * returns to the view; While Charging opens its list, and its choice
 * returns to the view too. */

static const short style_lang[VIZ_STYLES] = {
    LANG_IPODUI_VIZ_BARS, LANG_IPODUI_VIZ_MIRROR, LANG_IPODUI_VIZ_SCOPE,
    LANG_IPODUI_VIZ_SPECTRO, LANG_IPODUI_VIZ_RADIAL, LANG_IPODUI_VIZ_COVER,
    LANG_IPODUI_VIZ_AURA,
};

/* row i of the list: the i-th shown style */
static int style_at(int i)
{
    int s;

    for (s = 0; s < VIZ_STYLES; s++)
        if (viz_style_shown(s) && i-- == 0)
            return s;
    return VIZ_STYLE_BARS;
}

static int styles_n(void)
{
    int s, n = 0;

    for (s = 0; s < VIZ_STYLES; s++)
        n += viz_style_shown(s);
    return n;
}

static int vs_count(void)
{
#if CONFIG_CHARGING
    return styles_n() + 1;          /* + While Charging */
#else
    return styles_n();
#endif
}

static int vs_get(void)
{
    int i, cur = viz_style();

    for (i = 0; i < styles_n(); i++)
        if (style_at(i) == cur)
            return i;
    return 0;
}

#if CONFIG_CHARGING
static const char *cf_label(int idx, char *buf, size_t len)
{
    (void)buf;
    (void)len;
    return str(idx ? LANG_IPODUI_VISUALIZER : LANG_IPODUI_FACE_CLOCK);
}

static int cf_count(void)
{
    return 2;
}

static int cf_get(void)
{
    return global_settings.ipodui_charge_face ? 1 : 0;
}

static int cf_set(int idx)
{
    global_settings.ipodui_charge_face = idx;
    fe_settings_save();
    UPDATE_TRACE("viz charge face=%d", idx);
    return 2;                       /* back to the view, as a style does */
}

static const struct ui_vlist charge_faces = {
    LANG_IPODUI_CHARGE_FACE, PANELART_TIMER, cf_count, cf_get, cf_set,
    cf_label, NULL,
};
#endif

static int vs_set(int idx)
{
#if CONFIG_CHARGING
    if (idx >= styles_n())
    {
        ui_push(&ui_vlist_screen, &charge_faces);
        return 0;                   /* the hold list stays under it */
    }
#endif
    global_settings.ipodui_viz_style = style_at(idx);
    fe_settings_save();
    UPDATE_TRACE("viz style=%d", global_settings.ipodui_viz_style);
    return 1;
}

static const char *vs_label(int idx, char *buf, size_t len)
{
    (void)buf;
    (void)len;
#if CONFIG_CHARGING
    if (idx >= styles_n())
        return str(LANG_IPODUI_CHARGE_FACE);
#endif
    return str(style_lang[style_at(idx)]);
}

/* the panel's caption: the charger's face on its row */
static const char *vs_caption(int idx, char *buf, size_t len)
{
#if CONFIG_CHARGING
    if (idx >= styles_n())
        return cf_label(cf_get(), buf, len);
#endif
    (void)idx;
    (void)buf;
    (void)len;
    return NULL;
}

static const struct ui_vlist viz_styles = {
    LANG_IPODUI_VISUALIZER, PANELART_EQ, vs_count, vs_get, vs_set, vs_label,
    vs_caption,
};
UI_VLIST_SCREEN(ui_viz_style_screen, "viz-style", &viz_styles);

bool viz_face_wanted(void)
{
#if CONFIG_CHARGING
    return global_settings.ipodui_charge_face == 1 && charger_inserted()
           && (audio_status() & (AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE))
              == AUDIO_STATUS_PLAY
           && !playing_book();
#else
    return false;
#endif
}
