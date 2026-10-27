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
 * iPod UI shell: developer screen "Loader & storage" (#106, tenant B)
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

/* snapshot rows: 22..239 (below the dark header, UI_DARK_HDR_H) */

/* A read-only developer page in the black-and-white "disk mode" style:
 * white on black, 1 px rules, one 5 x 8 bitmap face of our own
 * (devfont.h). It shows the module arena (with a map bar), each module's
 * state and load times, the pack, and cheap storage counters. Nothing on
 * it is selectable; MENU returns.
 *
 * Module rules (modload.h): no writable .data (every table is const), no
 * IRAM, no timers; the state lives in the screen arena and goes with the
 * pop.
 *
 * First frame in two layers (#106 snapshot-first): the chrome (title
 * rules, labels, the map outline) and the values, each in a rectangle the
 * chrome leaves black. enter(s, DEVLOADER_SNAP_ARG) renders the chrome
 * only (deterministic: no live call at all). Live, the first full draw
 * asks mod_snap_show() for the chrome; without it the chrome is drawn
 * here. Either way the values follow, so both frames are identical.
 * Then once a second the values are read again and only the rows whose
 * text changed are redrawn and pushed (full-width bands). */

/* #106: a module tenant defines its real screen names (the core sees the
 * stand-ins, modload.h); the target's module build passes this itself */
#ifndef IPODUI_IN_MODULE
#define IPODUI_IN_MODULE
#endif
/* not ownable (file): a full-screen page or takeover; the drift panel was settled on the way in (ui_panel_hide(), or the takeover's own ui_fb_settle()), and a full lcd_update() releases it (bcmjob_owned_hook) */
#include "config.h"
#include <stdio.h>
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "storage.h"
#if defined(HAVE_ATA_XFER_STATS) && !defined(SIMULATOR)
#include "ata.h"
#define DL_XFER 1
#endif
#include "ipodui/anim.h"
#include "ipodui/header.h"
#include "ipodui/usbscreen.h"           /* UI_DARK_HDR_H */
#include "ipodui/screen_stack.h"
#include "ipodui/modload.h"
#include "ipodui/modules/devloader.h"
#include "ipodui/modules/devfont.h"

#define C_BG        LCD_BLACK
#define C_FG        LCD_WHITE

#define BODY_Y      UI_DARK_HDR_H
#define MARGIN      4
#define COLS        52                  /* (320 - 2 * 4) / 6 */
#define COL_X(c)    (MARGIN + (c) * DEVFONT_ADV)
#define CELL_H      DEVFONT_PITCH       /* a value rectangle's height */

/* chrome rows (text cell tops) */
#define Y_T_ARENA   26
#define Y_A1        41
#define Y_MAP       53                  /* outline, MAP_H rows */
#define MAP_H       10
#define Y_A2        66
#define Y_A3        78
#define Y_T_MOD     92
#define Y_MOD0      107
#define MOD_PITCH   24                  /* two lines a module */
#define MOD_LINE2   11
#define Y_T_STO     158
#define Y_S1        173
#define Y_S2        185
#define Y_S3        197
#define Y_RULE_END  214
#define Y_FOOT      221
#define RULE_GAP    11                  /* title cell top to its rule */

#define MAP_X       MARGIN
#define MAP_W       (LCD_WIDTH - 2 * MARGIN)
#define MAP_IN_W    (MAP_W - 2)

_Static_assert(MOD__LOADER <= 2, "devloader: the layout holds two modules");
_Static_assert(DEVLOADER_SNAP_Y == UI_DARK_HDR_H, "devloader: snap rows");

/* the rows that hold values, top to bottom */
enum {
    R_A1, R_MAP, R_A2, R_A3,
    R_MOD,                              /* 2 per module: line 1, line 2 */
    R_S1 = R_MOD + 2 * MOD__LOADER, R_S2, R_S3,
    R__N
};

struct dl_data {
    struct mod_arena_info a;
    struct mod_info m[MOD__LOADER];
    int  nmod;
    bool disk_active;
    long quiet_s;                       /* since the last disk activity */
    long up_s;
    unsigned long ui_free;
#ifdef DL_XFER
    struct ata_xfer_stats x;
#endif
};

struct dl {
    struct dl_data d;
    uint32_t hash[R__N];                /* the text each row shows */
    uint32_t acc;                       /* row_values() in hash mode */
    long next;                          /* next refresh (tick) */
    bool snapshot;                      /* enter(s, DEVLOADER_SNAP_ARG) */
    bool shown;                         /* the first full draw happened */
    bool hashing;
};
_Static_assert(sizeof(struct dl) <= UI_ARENA_B_SETTING,
               "devloader: arena bound");

/* -- drawing --------------------------------------------------------------- */

static void fill(int x, int y, int w, int h, unsigned c)
{
    lcd_set_viewport(NULL);
    lcd_set_drawmode(DRMODE_SOLID);
    lcd_set_foreground(c);
    lcd_fillrect(x, y, w, h);
}

/* text in the cell row at y (glyph rows y+1 .. y+8), ink only */
static void text(int x, int y, const char *s, int max_chars)
{
    lcd_set_viewport(NULL);
    for (; *s && max_chars > 0; s++, max_chars--, x += DEVFONT_ADV)
    {
        unsigned ch = (unsigned char)*s;
        const unsigned char *g;
        int r, b;

        if (ch < DEVFONT_FIRST || ch > DEVFONT_LAST)
            ch = '?';
        g = devfont[ch - DEVFONT_FIRST];
        for (r = 0; r < DEVFONT_H; r++)
        {
            fb_data *row = FBADDR(x, y + 1 + r);

            for (b = 0; b < DEVFONT_W; b++)
                if (g[r] & (0x10 >> b))
                    row[b] = (fb_data)C_FG;
        }
    }
}

static void label(int col, int y, const char *s)
{
    text(COL_X(col), y, s, COLS - col);
}

static void rule(int y)
{
    fill(MARGIN, y, LCD_WIDTH - 2 * MARGIN, 1, C_FG);
}

static void section(int y, const char *title, const char *note)
{
    label(0, y, title);
    if (note)
        label(COLS - (int)strlen(note), y, note);
    rule(y + RULE_GAP);
}

/* The chrome: everything but the values; deterministic. */
static void draw_chrome(void)
{
    int i;

    fill(0, BODY_Y, LCD_WIDTH, LCD_HEIGHT - BODY_Y, C_BG);

    section(Y_T_ARENA, "ARENA", NULL);
    label(0, Y_A1, "size");
    label(17, Y_A1, "used");
    label(35, Y_A1, "lfree");
    /* the map's outline; its inside is a value */
    fill(MAP_X, Y_MAP, MAP_W, 1, C_FG);
    fill(MAP_X, Y_MAP + MAP_H - 1, MAP_W, 1, C_FG);
    fill(MAP_X, Y_MAP, 1, MAP_H, C_FG);
    fill(MAP_X + MAP_W - 1, Y_MAP, 1, MAP_H, C_FG);
    label(0, Y_A2, "pak");
    label(17, Y_A2, "bytes");
    label(35, Y_A2, "build");
    label(0, Y_A3, "mount");
    label(17, Y_A3, "fletch");
    label(35, Y_A3, "mode");

    section(Y_T_MOD, "MODULES", "times in ms");
    for (i = 0; i < MOD__LOADER; i++)
    {
        int y = Y_MOD0 + i * MOD_PITCH;

        label(18, y, "refs");
        label(27, y, "bytes");
        label(41, y, "rel");
        y += MOD_LINE2;
        label(1, y, "ld");
        label(9, y, "mv");
        label(16, y, "ev");
        label(23, y, "last");
        label(36, y, "max");
        label(48, y, "fl");
    }

    section(Y_T_STO, "STORAGE", NULL);
    label(0, Y_S1, "disk");
    label(17, Y_S1, "quiet");
    label(35, Y_S1, "resets");
    label(0, Y_S2, "dma rd");
    label(17, Y_S2, "dma wr");
    label(35, Y_S2, "pio rd");
    label(0, Y_S3, "pio wr");
    label(17, Y_S3, "uptime");
    label(35, Y_S3, "ui free");

    rule(Y_RULE_END);
    label(0, Y_FOOT, "MENU back");
}

static int row_y(int r)
{
    switch (r)
    {
        case R_A1:  return Y_A1;
        case R_MAP: return Y_MAP + 1;
        case R_A2:  return Y_A2;
        case R_A3:  return Y_A3;
        case R_S1:  return Y_S1;
        case R_S2:  return Y_S2;
        case R_S3:  return Y_S3;
        default:
            r -= R_MOD;
            return Y_MOD0 + (r >> 1) * MOD_PITCH + (r & 1) * MOD_LINE2;
    }
}

static int row_h(int r)
{
    return r == R_MAP ? MAP_H - 2 : CELL_H;
}

static uint32_t mix32(uint32_t h, uint32_t v)
{
    return (h ^ v) * 16777619u;
}

/* One value in columns c0..c1 (inclusive), left or right aligned. Hash
 * mode only folds the text into dl->acc. */
static void val(struct dl *dl, int y, int c0, int c1, bool right,
                const char *s)
{
    int n = c1 - c0 + 1, len = strlen(s);

    if (len > n)
        len = n;
    if (dl->hashing)
    {
        int i;

        dl->acc = mix32(dl->acc, (uint32_t)(c0 << 8 | len));
        for (i = 0; i < len; i++)
            dl->acc = mix32(dl->acc, (unsigned char)s[i]);
        return;
    }
    fill(COL_X(c0), y, n * DEVFONT_ADV, CELL_H, C_BG);
    text(COL_X(right ? c1 + 1 - len : c0), y, s, len);
}

static void val_ul(struct dl *dl, int y, int c0, int c1, unsigned long v)
{
    char b[24];

    snprintf(b, sizeof(b), "%lu", v);
    val(dl, y, c0, c1, true, b);
}

static void val_ms(struct dl *dl, int y, int c0, int c1, unsigned long us)
{
    char b[24];

    snprintf(b, sizeof(b), "%lu.%02lu", us / 1000, (us % 1000) / 10);
    val(dl, y, c0, c1, true, b);
}

/* The map: each placed module a run (solid, then a checker), free black */
static void map_values(struct dl *dl)
{
    const struct dl_data *d = &dl->d;
    int i;

    if (dl->hashing)
    {
        dl->acc = mix32(dl->acc, d->a.size);
        for (i = 0; i < d->nmod; i++)
        {
            dl->acc = mix32(dl->acc, d->m[i].offset);
            dl->acc = mix32(dl->acc, d->m[i].bytes);
        }
        return;
    }
    fill(MAP_X + 1, Y_MAP + 1, MAP_IN_W, MAP_H - 2, C_BG);
    if (!d->a.size)
        return;
    lcd_set_viewport(NULL);
    for (i = 0; i < d->nmod; i++)
    {
        const struct mod_info *m = &d->m[i];
        unsigned long end;
        int x0, x1, x, y;

        if (m->offset == ~0u || !m->bytes || m->offset >= d->a.size)
            continue;
        end = MIN((unsigned long)m->offset + m->bytes, d->a.size);
        x0 = (int)((uint64_t)m->offset * MAP_IN_W / d->a.size);
        x1 = (int)((uint64_t)end * MAP_IN_W / d->a.size);
        if (x1 <= x0)
            x1 = x0 + 1;
        for (y = 0; y < MAP_H - 2; y++)
        {
            fb_data *row = FBADDR(MAP_X + 1, Y_MAP + 1 + y);

            for (x = x0; x < x1 && x < MAP_IN_W; x++)
                if (!(i & 1) || ((x + y) & 1))
                    row[x] = (fb_data)C_FG;
        }
    }
}

/* fixed-width names, not a pointer table: no relocated rodata */
static const char *state_name(unsigned st)
{
    static const char names[][7] = { "absent", "load", "ready", "failed" };

    return st < ARRAYLEN(names) ? names[st] : "?";
}

static void mod_values(struct dl *dl, int i, int line, int y)
{
    const struct mod_info *m = &dl->d.m[i];

    if (i >= dl->d.nmod)
    {
        if (line == 0)
            val(dl, y, 0, 9, false, "-");
        return;
    }
    if (line == 0)
    {
        val(dl, y, 0, 9, false, m->name ? m->name : "?");
        val(dl, y, 11, 16, false, state_name(m->state));
        val_ul(dl, y, 23, 25, m->refs);
        val_ul(dl, y, 33, 39, m->bytes);
        val_ul(dl, y, 45, 51, m->nreloc);
    }
    else
    {
        val_ul(dl, y, 4, 7, m->loads);
        val_ul(dl, y, 12, 14, m->moves);
        val_ul(dl, y, 19, 21, m->evictions);
        val_ms(dl, y, 28, 34, m->last_us);
        val_ms(dl, y, 40, 46, m->max_us);
        val(dl, y, 51, 51, true, m->verified ? "y" : "n");
    }
}

static void row_values(struct dl *dl, int r)
{
    const struct dl_data *d = &dl->d;
    int y = row_y(r);
    char b[24];

    switch (r)
    {
        case R_A1:
            val_ul(dl, y, 6, 15, d->a.size);
            val_ul(dl, y, 23, 32, d->a.used);
            val_ul(dl, y, 41, 51, d->a.largest_free);
            break;
        case R_MAP:
            map_values(dl);
            break;
        case R_A2:
            val(dl, y, 4, 15, false, !d->a.pak_open ? "shut"
                                     : d->a.pak_ok ? "open ok" : "open bad");
            val_ul(dl, y, 23, 32, d->a.pak_bytes);
            snprintf(b, sizeof(b), "%08lx", (unsigned long)d->a.build_id);
            val(dl, y, 41, 51, true, b);
            break;
        case R_A3:
            val_ul(dl, y, 6, 15, d->a.mount_gen);
            val_ms(dl, y, 24, 32, d->a.fletcher_us);
            val(dl, y, 41, 51, true, d->a.modload ? "load" : "static");
            break;
        case R_S1:
            val(dl, y, 5, 15, false, d->disk_active ? "active" : "asleep");
            if (d->quiet_s < 0)
                val(dl, y, 23, 32, true, "-");
            else
            {
                snprintf(b, sizeof(b), "%lds", d->quiet_s);
                val(dl, y, 23, 32, true, b);
            }
#ifdef DL_XFER
            val_ul(dl, y, 42, 51, d->x.resets);
#else
            val(dl, y, 42, 51, true, "-");
#endif
            break;
        case R_S2:
#ifdef DL_XFER
            val_ul(dl, y, 7, 15, d->x.dma_rd);
            val_ul(dl, y, 24, 32, d->x.dma_wr);
            val_ul(dl, y, 42, 51, d->x.pio_rd);
#else
            val(dl, y, 7, 15, true, "-");
            val(dl, y, 24, 32, true, "-");
            val(dl, y, 42, 51, true, "-");
#endif
            break;
        case R_S3:
#ifdef DL_XFER
            val_ul(dl, y, 7, 15, d->x.pio_wr);
#else
            val(dl, y, 7, 15, true, "-");
#endif
            snprintf(b, sizeof(b), "%lds", d->up_s);
            val(dl, y, 24, 32, true, b);
            val_ul(dl, y, 43, 51, d->ui_free);
            break;
        default:
            r -= R_MOD;
            mod_values(dl, r >> 1, r & 1, y);
            break;
    }
}

static uint32_t row_hash(struct dl *dl, int r)
{
    dl->hashing = true;
    dl->acc = 2166136261u;
    row_values(dl, r);
    dl->hashing = false;
    return dl->acc;
}

/* -- data ------------------------------------------------------------------ */

/* Only cheap reads: RAM counters, no disk access, no free-space count. */
static void fetch(struct dl_data *d)
{
    long last = storage_last_disk_activity();
    int i;

    mod_arena_get(&d->a);
    d->nmod = MIN(mod_count(), MOD__LOADER);
    for (i = 0; i < d->nmod; i++)
        mod_get_info(i, &d->m[i]);
    d->disk_active = storage_disk_is_active();
    d->quiet_s = last < 0 ? -1 : (current_tick - last) / HZ;
    d->up_s = current_tick / HZ;
    d->ui_free = ui_arena_free();
#ifdef DL_XFER
    ata_get_xfer_stats(&d->x);
#endif
}

/* -- the screen ------------------------------------------------------------ */

static int dl_enter(struct ui_screen *s, const void *arg)
{
    struct dl *dl = ui_arena_alloc(sizeof(*dl));

    if (!dl)
        return -1;
    s->priv = dl;
    s->title = "Loader & storage";      /* developer only: no LANG_ */
    s->header_w = UI_HEADER_DARK;
    dl->snapshot = arg == DEVLOADER_SNAP_ARG;
    return 0;
}

static void dl_draw(struct ui_screen *s, unsigned dirty)
{
    struct dl *dl = s->priv;
    int r;

    if (dirty & UI_DIRTY_BODY)
    {
        bool have = false;

        if (dl->snapshot)
        {
            draw_chrome();
            return;
        }
        if (!dl->shown)
        {
            dl->shown = true;
            have = mod_snap_show(SNAP_DEVLOADER);
        }
        if (!have)
            draw_chrome();
        fetch(&dl->d);
        for (r = 0; r < R__N; r++)
        {
            dl->hash[r] = row_hash(dl, r);
            row_values(dl, r);
        }
        dl->next = current_tick + HZ;
        UPDATE_TRACE("devloader draw snap=%d", (int)have);
    }
    else if ((dirty & UI_DIRTY_BAND) && !dl->snapshot)
    {
        int y[UI_BANDS_MAX], h[UI_BANDS_MAX];
        int i, n = ui_dirty_bands(y, h);

        for (r = 0; r < R__N; r++)
        {
            int ry = row_y(r), rh = row_h(r);

            for (i = 0; i < n; i++)
                if (ry < y[i] + h[i] && y[i] < ry + rh)
                {
                    row_values(dl, r);
                    break;
                }
        }
    }
}

/* Once a second: the rows whose text changed become dirty bands; the
 * main loop's refresh draws and pushes only those. */
static int dl_tick(struct ui_screen *s)
{
    struct dl *dl = s->priv;
    long left;
    int r;

    if (dl->snapshot || !dl->shown)
        return TIMEOUT_BLOCK;
    left = dl->next - current_tick;
    if (left > 0)
        return left;
    dl->next = current_tick + HZ;
    fetch(&dl->d);
    for (r = 0; r < R__N; r++)
    {
        uint32_t hv = row_hash(dl, r);

        if (hv != dl->hash[r])
        {
            dl->hash[r] = hv;
            ui_invalidate_band(row_y(r), row_h(r));
        }
    }
    return HZ;
}

static enum ui_result dl_input(struct ui_screen *s, const struct ui_input *in)
{
    (void)s;
    switch (in->key)
    {
        case UI_KEY_MENU:
            return in->release && !in->long_press ? UI_POP : UI_IGNORED;
        case UI_KEY_WHEEL:
        case UI_KEY_SELECT:
        case UI_KEY_PREV:
        case UI_KEY_NEXT:
            return UI_HANDLED;          /* a read-only page */
        default:
            return UI_IGNORED;          /* PLAY plays */
    }
}

const struct ui_screen_ops ui_devloader_screen = {
    .name    = "devloader",
    .enter   = dl_enter,
    .leave   = NULL,                    /* arena: released by the pop */
    .draw    = dl_draw,
    .input   = dl_input,
    .timeout = NULL,                    /* the tick returns its wait */
    .tick    = dl_tick,
};
