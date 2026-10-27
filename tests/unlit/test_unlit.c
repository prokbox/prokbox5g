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

/* Host test of apps/ipodui/unlit_core.c (Phase 8a U2): the unlit face's
 * state machine (docs/phase8a-brief.md §3.5 + Revision 1 M1/M2, one
 * check per transition row) and its painting. The painting renders the
 * four design scenes and the glyph sheet as grey rasters into OUTDIR;
 * tests/unlit/compare.py requires them to equal docs/design/unlit/. GPL v2+ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "ipodui/unlit_core.h"

#define BAT_OUT_W 58            /* battery + nub (art/unlit/unlit.py) */

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; \
        printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
        printf("\n"); } } while (0)

/* -- state machine ------------------------------------------------------ */

static void test_states(void)
{
    struct ui_unlit_state s;
    long t = 1000;
    unsigned a;

    /* NORMAL + BL_OFF (dark) -> FACE, full draw */
    unlit_state_init(&s, t);
    a = ui_unlit_step(&s, UE_BL_OFF, t, false);
    CHECK(a == UA_DRAW_FULL && s.face, "bl_off -> face (a=%u)", a);
    /* a second BL_OFF while FACE: nothing */
    CHECK(ui_unlit_step(&s, UE_BL_OFF, t, false) == UA_NONE, "bl_off again");

    /* M1: BL_OFF handled while lit again -> no face */
    unlit_state_init(&s, t);
    a = ui_unlit_step(&s, UE_BL_OFF, t, true);
    CHECK(a == UA_NONE && !s.face, "bl_off while lit");

    /* FACE: minute / status redraws; full when fb_valid is false */
    unlit_state_init(&s, t);
    ui_unlit_step(&s, UE_BL_OFF, t, false);
    CHECK(ui_unlit_step(&s, UE_MINUTE, t, false) == UA_DRAW_FULL,
          "minute before fb_valid -> full");
    s.fb_valid = true;
    CHECK(ui_unlit_step(&s, UE_MINUTE, t, false) == UA_DRAW_DIGITS, "minute");
    CHECK(ui_unlit_step(&s, UE_STATUS, t, false) == UA_DRAW_STATUS, "status");
    CHECK(ui_unlit_step(&s, UE_HOLD_ON, t, true) == UA_DRAW_STATUS,
          "hold on -> status (padlock), stays");
    CHECK(s.face, "hold on keeps face");
    /* backlight back on without a key: stays FACE, lit */
    CHECK(ui_unlit_step(&s, UE_BL_ON_NOKEY, t, true) == UA_NONE && s.face,
          "bl on nokey");
    /* foreign draw: next minute is a full draw */
    s.fb_valid = false;
    CHECK(ui_unlit_step(&s, UE_MINUTE, t, false) == UA_DRAW_FULL,
          "minute after foreign draw");
    /* NORMAL: minute/status/hold-on do nothing */
    unlit_state_init(&s, t);
    CHECK(ui_unlit_step(&s, UE_MINUTE, t, true) == UA_NONE, "normal minute");
    CHECK(ui_unlit_step(&s, UE_STATUS, t, true) == UA_NONE, "normal status");
    CHECK(ui_unlit_step(&s, UE_HOLD_OFF, t, true) == UA_NONE, "normal hold off");
    /* #65: NORMAL + Hold on -> FACE at once, lit or dark; Hold off leaves */
    a = ui_unlit_step(&s, UE_HOLD_ON, t, true);
    CHECK(a == UA_DRAW_FULL && s.face && !s.swallowing,
          "normal hold on -> face (a=%u)", a);
    CHECK(ui_unlit_step(&s, UE_BL_OFF, t, false) == UA_NONE && s.face,
          "bl_off after hold on: stays");
    a = ui_unlit_step(&s, UE_HOLD_OFF, t, true);
    CHECK(a == UA_DISMISS && !s.face, "hold off dismisses (a=%u)", a);

    /* key press dismisses and swallows until its release */
    unlit_state_init(&s, t);
    ui_unlit_step(&s, UE_BL_OFF, t, false);
    a = ui_unlit_step(&s, UE_KEY, t + 1, true);
    CHECK(a == (UA_DISMISS | UA_SWALLOW) && !s.face && s.swallowing,
          "key dismisses (a=%u)", a);
    CHECK(s.shown_min == -1 && !s.fb_valid, "dismiss clears identity");
    CHECK(ui_unlit_step(&s, UE_KEY, t + 20, true) == UA_SWALLOW,
          "repeat swallowed");
    CHECK(ui_unlit_step(&s, UE_WHEEL, t + 50, true) == UA_SWALLOW,
          "wheel while held swallowed");
    CHECK(ui_unlit_step(&s, UE_KEY_RELEASE, t + 60, true) == UA_SWALLOW
          && !s.swallowing, "release swallowed, ends");
    CHECK(ui_unlit_step(&s, UE_KEY, t + 70, true) == UA_NONE, "next key acts");
    CHECK(ui_unlit_step(&s, UE_KEY_RELEASE, t + 80, true) == UA_NONE,
          "next release acts");

    /* swallowing ends on buttons-up (release lost) */
    unlit_state_init(&s, t);
    ui_unlit_step(&s, UE_BL_OFF, t, false);
    ui_unlit_step(&s, UE_KEY, t, true);
    ui_unlit_step(&s, UE_BUTTONS_UP, t + 5, true);
    CHECK(!s.swallowing, "buttons up ends swallowing");
    CHECK(ui_unlit_step(&s, UE_KEY, t + 40, true) == UA_NONE,
          "key after buttons up acts");

    /* fallback: UNLIT_SWALLOW_MAX after the dismiss (tick and key) */
    unlit_state_init(&s, t);
    ui_unlit_step(&s, UE_BL_OFF, t, false);
    ui_unlit_step(&s, UE_KEY, t, true);
    ui_unlit_step(&s, UE_TICK, t + UNLIT_SWALLOW_MAX - 1, true);
    CHECK(s.swallowing, "still swallowing before max");
    ui_unlit_step(&s, UE_TICK, t + UNLIT_SWALLOW_MAX, true);
    CHECK(!s.swallowing, "tick timeout ends swallowing");
    unlit_state_init(&s, t);
    ui_unlit_step(&s, UE_BL_OFF, t, false);
    ui_unlit_step(&s, UE_KEY, t, true);
    CHECK(ui_unlit_step(&s, UE_KEY, t + UNLIT_SWALLOW_MAX, true) == UA_NONE
          && !s.swallowing, "key after max passes");

    /* lone release (no press seen while FACE) dismisses, no swallowing */
    unlit_state_init(&s, t);
    ui_unlit_step(&s, UE_BL_OFF, t, false);
    a = ui_unlit_step(&s, UE_KEY_RELEASE, t, true);
    CHECK(a == (UA_DISMISS | UA_SWALLOW) && !s.face && !s.swallowing,
          "lone release dismisses (a=%u)", a);

    /* wheel dismisses; wheel quiet for UNLIT_WHEEL_SWALLOW, keys act */
    unlit_state_init(&s, t);
    ui_unlit_step(&s, UE_BL_OFF, t, false);
    a = ui_unlit_step(&s, UE_WHEEL, t, true);
    CHECK(a == (UA_DISMISS | UA_SWALLOW) && !s.face && !s.swallowing,
          "wheel dismisses (a=%u)", a);
    CHECK(ui_unlit_step(&s, UE_WHEEL, t + UNLIT_WHEEL_SWALLOW - 1, true)
          == UA_SWALLOW, "wheel in quiet window");
    CHECK(ui_unlit_step(&s, UE_WHEEL, t + UNLIT_WHEEL_SWALLOW, true)
          == UA_NONE, "wheel after quiet window");
    CHECK(ui_unlit_step(&s, UE_KEY, t + 2, true) == UA_NONE,
          "key after wheel dismiss acts");
    /* no quiet window at start */
    unlit_state_init(&s, t);
    CHECK(ui_unlit_step(&s, UE_WHEEL, t, true) == UA_NONE, "wheel at init");

    /* Hold off -> NORMAL, nothing swallowed */
    unlit_state_init(&s, t);
    ui_unlit_step(&s, UE_BL_OFF, t, false);
    a = ui_unlit_step(&s, UE_HOLD_OFF, t, true);
    CHECK(a == UA_DISMISS && !s.face && !s.swallowing, "hold off (a=%u)", a);

    /* USB -> NORMAL first */
    unlit_state_init(&s, t);
    ui_unlit_step(&s, UE_BL_OFF, t, false);
    CHECK(ui_unlit_step(&s, UE_USB, t, false) == UA_DISMISS && !s.face,
          "usb leaves face");
    CHECK(ui_unlit_step(&s, UE_USB, t, false) == UA_NONE, "usb normal");
    unlit_state_init(&s, t);
    ui_unlit_step(&s, UE_BL_OFF, t, false);
    CHECK(ui_unlit_step(&s, UE_DEVMODE, t, false) == UA_DISMISS && !s.face,
          "devmode leaves face");

    /* M2: the shell's sleep path skips one BL_OFF, within the lifetime */
    unlit_state_init(&s, t);
    ui_unlit_step(&s, UE_SLEEP_NEXT, t, true);
    a = ui_unlit_step(&s, UE_BL_OFF, t + UNLIT_SKIP_TICKS, false);
    CHECK(a == UA_SKIP && !s.face && !s.skip_next, "sleep skip (a=%u)", a);
    CHECK(ui_unlit_step(&s, UE_BL_OFF, t + UNLIT_SKIP_TICKS + 1, false)
          == UA_DRAW_FULL, "only one skipped");
    /* expired: the face shows */
    unlit_state_init(&s, t);
    ui_unlit_step(&s, UE_SLEEP_NEXT, t, true);
    a = ui_unlit_step(&s, UE_BL_OFF, t + UNLIT_SKIP_TICKS + 1, false);
    CHECK(a == UA_DRAW_FULL && !s.skip_next, "expired skip (a=%u)", a);
    /* expired by a tick */
    unlit_state_init(&s, t);
    ui_unlit_step(&s, UE_SLEEP_NEXT, t, true);
    ui_unlit_step(&s, UE_TICK, t + UNLIT_SKIP_TICKS + 1, true);
    CHECK(!s.skip_next, "tick expires skip");

    /* fix M2: a key-less release (bare BUTTON_REL) is never a key */
    unlit_state_init(&s, t);
    ui_unlit_step(&s, UE_BL_OFF, t, false);
    CHECK(unlit_release_route(&s, 0, 0) == UNLIT_REL_PASS,
          "bare release over the face passes (no dismiss)");
    ui_unlit_step(&s, UE_KEY, t, false);            /* SELECT dismisses */
    CHECK(s.swallowing, "swallowing after the wake press");
    CHECK(unlit_release_route(&s, 0, 0x2) == UNLIT_REL_PASS,
          "bare release while swallowing passes");
    CHECK(s.swallowing, "bare release leaves the swallowing on");
    CHECK(unlit_release_route(&s, 0x4, 0x2) == UNLIT_REL_SWALLOW,
          "other key's release while swallowing");
    CHECK(unlit_release_route(&s, 0x2, 0x2) == UNLIT_REL_STEP,
          "the dismissing release is stepped");
    ui_unlit_step(&s, UE_KEY_RELEASE, t + 5, false);
    CHECK(!s.swallowing, "dismissing release ends the swallowing");
    CHECK(unlit_release_route(&s, 0x4, 0x2) == UNLIT_REL_STEP,
          "normal release");

    /* timer */
    CHECK(ui_unlit_ticks_to_minute(0) == 60 * HZ, "ttm 0");
    CHECK(ui_unlit_ticks_to_minute(59) == HZ, "ttm 59");
    CHECK(ui_unlit_ticks_to_minute(60) == 1, "ttm 60 (leap)");
}

/* -- painting ----------------------------------------------------------- */

struct canvas {
    int w, h;
    unsigned char *px;
    long rects, pixels;
};

static void cfill(void *ctx, int x, int y, int w, int h, int g)
{
    struct canvas *c = ctx;
    int i, j;

    CHECK(w > 0 && h > 0 && x >= 0 && y >= 0 && x + w <= c->w
          && y + h <= c->h, "rect %d,%d %dx%d out of %dx%d", x, y, w, h,
          c->w, c->h);
    if (!(w > 0 && h > 0 && x >= 0 && y >= 0 && x + w <= c->w
          && y + h <= c->h))
        return;
    c->rects++;
    c->pixels += (long)w * h;
    for (j = y; j < y + h; j++)
        for (i = x; i < x + w; i++)
            c->px[j * c->w + i] = g;
}

static void canvas_init(struct canvas *c, int w, int h)
{
    c->w = w;
    c->h = h;
    c->px = malloc(w * h);
    memset(c->px, UNLIT_PAPER, w * h);
    c->rects = c->pixels = 0;
}

static void canvas_save(struct canvas *c, const char *dir, const char *name)
{
    char path[512];
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s.grey", dir, name);
    f = fopen(path, "wb");
    if (!f)
    {
        CHECK(0, "cannot write %s", path);
        return;
    }
    fprintf(f, "%d %d\n", c->w, c->h);
    fwrite(c->px, 1, c->w * c->h, f);
    fclose(f);
}

static void paint_face(struct canvas *c, int minute, bool h12,
                       const struct ui_unlit_status *st)
{
    struct unlit_clock clk;

    unlit_clock_layout(minute, h12, &clk);
    cfill(c, 0, 0, LCD_WIDTH, LCD_HEIGHT, UNLIT_PAPER);
    unlit_paint_clock(&clk, NULL, cfill, c);
    unlit_paint_status(st, cfill, c);
}

static void test_scenes(const char *dir)
{
    static const struct {
        const char *name;
        int minute;
        bool h12;
        int play, pct, charging, hold;
    } sc[] = {
        { "12h-playing",        4 * 60 + 1,   true,  1, 55,  0, 0 },
        { "24h-paused",         9 * 60 + 41,  false, 2, 100, 0, 0 },
        { "24h-stopped",        15 * 60 + 3,  false, 0, 8,   0, 0 },
        { "12h-hold-charging",  12 * 60 + 58, true,  1, 45,  1, 1 },
    };
    unsigned i;

    for (i = 0; i < sizeof(sc) / sizeof(sc[0]); i++)
    {
        struct canvas c;
        struct ui_unlit_status st = { sc[i].play, sc[i].hold, sc[i].charging,
                                      unlit_fill_px(sc[i].pct) };
        canvas_init(&c, LCD_WIDTH, LCD_HEIGHT);
        c.rects = c.pixels = 0;
        paint_face(&c, sc[i].minute, sc[i].h12, &st);
        printf("     scene %-18s %3ld rects %6ld px\n", sc[i].name,
               c.rects, c.pixels);
        canvas_save(&c, dir, sc[i].name);
        free(c.px);
    }
}

/* the minute step: only changed cells; a width change repaints the band */
static void test_minute_step(void)
{
    struct canvas a, b;
    struct unlit_clock o, n;
    int cells;
    struct ui_unlit_status st = { 1, 0, 0, 20 };

    canvas_init(&a, LCD_WIDTH, LCD_HEIGHT);
    canvas_init(&b, LCD_WIDTH, LCD_HEIGHT);
    paint_face(&a, 4 * 60 + 1, true, &st);
    unlit_clock_layout(4 * 60 + 1, true, &o);
    unlit_clock_layout(4 * 60 + 2, true, &n);
    a.rects = a.pixels = 0;
    cells = unlit_paint_clock(&n, &o, cfill, &a);
    printf("     minute 4:01->4:02: %d cell, %ld rects, %ld px\n", cells,
           a.rects, a.pixels);
    CHECK(cells == 1, "4:01 -> 4:02 repaints one cell (%d)", cells);
    paint_face(&b, 4 * 60 + 2, true, &st);
    CHECK(!memcmp(a.px, b.px, LCD_WIDTH * LCD_HEIGHT),
          "patched 4:02 == fresh 4:02");

    /* 9:59 -> 10:00 (12 h): the row widens and re-centres */
    paint_face(&a, 9 * 60 + 59, true, &st);
    unlit_clock_layout(9 * 60 + 59, true, &o);
    unlit_clock_layout(10 * 60, true, &n);
    a.rects = a.pixels = 0;
    cells = unlit_paint_clock(&n, &o, cfill, &a);
    printf("     minute 9:59->10:00: %d cells, %ld rects, %ld px\n", cells,
           a.rects, a.pixels);
    CHECK(cells == 5, "9:59 -> 10:00 repaints the row (%d)", cells);
    paint_face(&b, 10 * 60, true, &st);
    CHECK(!memcmp(a.px, b.px, LCD_WIDTH * LCD_HEIGHT),
          "patched 10:00 == fresh 10:00");

    /* every minute of a day, both formats: patch == fresh */
    {
        int m, f, bad = 0;
        for (f = 0; f < 2; f++)
        {
            paint_face(&a, 0, f, &st);
            for (m = 1; m < 24 * 60; m++)
            {
                unlit_clock_layout(m - 1, f, &o);
                unlit_clock_layout(m, f, &n);
                unlit_paint_clock(&n, &o, cfill, &a);
                if (m % 97 == 0)
                {
                    paint_face(&b, m, f, &st);
                    bad += memcmp(a.px, b.px, LCD_WIDTH * LCD_HEIGHT) != 0;
                }
            }
        }
        CHECK(!bad, "%d patched minutes differ from fresh", bad);
    }
    /* 12 h: "12:05", "1:00"; 24 h: "00:00" */
    unlit_clock_layout(5, true, &n);
    CHECK(n.n == 5 && !memcmp(n.ch, "12:05", 5), "12h midnight");
    unlit_clock_layout(13 * 60, true, &n);
    CHECK(n.n == 4 && !memcmp(n.ch, "1:00", 4), "12h 13:00");
    unlit_clock_layout(0, false, &n);
    CHECK(n.n == 5 && !memcmp(n.ch, "00:00", 5), "24h midnight");
    free(a.px);
    free(b.px);
}

/* art/unlit/unlit.py glyph_sheet(): 1x, the letters left out */
static void test_sheet(const char *dir)
{
    static const int lv[] = { 0, 8, 25, 50, 75, 100 };
    struct canvas c;
    int i, x;

    canvas_init(&c, 607, 122);
    for (x = 6, i = 0; i < 10; i++)
    {
        unlit_paint_glyph(i, x, 6, cfill, &c);
        x += unlit_glyph_w(i) + 6;
    }
    unlit_paint_glyph(UNLIT_G_COLON, x, 6, cfill, &c);
    x = 6;
    unlit_paint_glyph(UNLIT_G_PLAY, x, 80, cfill, &c);
    x += unlit_glyph_w(UNLIT_G_PLAY) + 10;
    unlit_paint_glyph(UNLIT_G_PAUSE, x, 80, cfill, &c);
    x += unlit_glyph_w(UNLIT_G_PAUSE) + 10;
    unlit_paint_glyph(UNLIT_G_LOCK, x, 80, cfill, &c);
    x += unlit_glyph_w(UNLIT_G_LOCK) + 10;
    for (i = 0; i < 6; i++)
    {
        unlit_paint_battery(x, 80, unlit_fill_px(lv[i]), false, cfill, &c);
        x += BAT_OUT_W + 10;
    }
    unlit_paint_battery(x, 80, unlit_fill_px(45), true, cfill, &c);
    canvas_save(&c, dir, "glyphs");
    free(c.px);

    CHECK(unlit_fill_px(0) == 0 && unlit_fill_px(1) == 1
          && unlit_fill_px(25) == 10 && unlit_fill_px(75) == 32
          && unlit_fill_px(100) == 42 && unlit_fill_px(55) == 23,
          "fill_px rounding");
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";

    test_states();
    test_minute_step();
    test_scenes(dir);
    test_sheet(dir);
    if (fails)
    {
        printf("%d failure(s)\n", fails);
        return 1;
    }
    return 0;
}
