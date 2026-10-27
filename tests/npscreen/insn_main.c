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

/* Bare-metal ARM workloads for tests/npscreen/insn.sh: dynamic
 * instruction counts of Now Playing's per-event pixel work (np_draw.c,
 * np_state.c, np_scrub.c, np_lyrics.c, compiled -mcpu=arm7tdmi -Os as in
 * the firmware) under qemu-system-arm + the risk tool's TCG plugin
 * (scripts/risk/qemu-harness). Each WL runs the same sequence of pure
 * calls that screens/nowplaying.c makes for that event, REPS times; WL 0
 * is the empty baseline to subtract. Not counted: Rockbox font rendering
 * (lcd_putsxy), lcd_update_rect (the LCD bus), the facade's reads. The
 * marquee blit and the lyrics move use the harness's C memcpy/memmove,
 * an upper bound of the firmware's assembler ones. */
#include <stdint.h>
#include <stddef.h>
/* no libc headers in the bare-metal toolchain: the harness's shim */
void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
#include "np_draw.h"
#include "np_state.h"
#include "np_scrub.h"
#include "np_lyrics.h"
#include "ab_book.h"

#ifndef WL
#define WL 0
#endif
#define REPS 8
#if WL == 12
#undef WL
#define WL 4
#define VEL 30      /* fast: chapter snapping over 648 starts */
#else
#define VEL 5       /* slow: 1 s per click */
#endif

static np_px fb[320 * 240];
static np_px art[142 * 142];
static np_px comp[NP_COMPOSED_W * NP_COMPOSED_H];   /* QW2 */
static np_px atlas_px[96 * 12];
static np_px strip[1392 * 18];
static unsigned long chs[648];
static uint8_t ticks[28];
static uint8_t bticks[28];
static unsigned long bstarts[1024];     /* P6: a 1,024-file book */
static struct ab_book_map bmap;
static uint16_t starts[512];
static char lyr[8192];
volatile int sink;

static const struct np_canvas cv = { fb, 320, 320, 240 };
static struct np_bar_style bst;
static struct np_atlas atl;

static void setup(void)
{
    int i;
    for (i = 0; i < NP_BAR_H; i++)
    {
        bst.track[i] = (np_px)(0x8410 + i);
        bst.fill[i] = (np_px)(0x001f + i);
    }
    atl.px = atlas_px;
    atl.stride = 96;
    atl.h = 12;
    for (i = 0; i < NP_ATLAS_N; i++)
    {
        atl.x[i] = (uint8_t)(i * 7);
        atl.w[i] = 7;
    }
    for (i = 0; i < 648; i++)
        chs[i] = (unsigned long)i * 65000UL;
    bmap.file_count = 1024;
    bmap.entries = 1024 + 647;
    for (i = 0; i < 1024; i++)
    {
        bstarts[i] = (unsigned long)i * 56250UL;
        bmap.file_start_ms[i] = (uint32_t)bstarts[i];
        bmap.file_first[i] = (uint16_t)(i <= 600 ? i : i + 647);
    }
    for (i = 0; i < 28; i++)
        ticks[i] = (uint8_t)(0x11 << (i & 3));
    for (i = 0; i < (int)sizeof(lyr) - 1; i++)
        lyr[i] = (i % 37 == 36) ? '\n' : (i % 6 == 5) ? ' ' : 'a' + i % 26;
    lyr[sizeof(lyr) - 1] = 0;
}

static int adv(void *ctx, unsigned cp)
{
    (void)ctx;
    return cp == ' ' ? 4 : 7;
}

/* the 1 Hz labels (draw_labels): only the characters that changed */
static char lbl_l[12], lbl_r[12];
static void labels(unsigned long pos, unsigned long len)
{
    char b[12];
    int w, x, i;
    np_fmt_time(b, pos, false);
    x = np_atlas_update(&cv, &atl, 23, 209, lbl_l, b);
    np_fill(&cv, x, 209, 23 + 58 - x, 12, 0xffff);
    for (i = 0; i < 12; i++)
        lbl_l[i] = b[i];
    np_fmt_time(b, len - pos, true);
    w = np_atlas_width(&atl, b);
    if (w != np_atlas_width(&atl, lbl_r))
        np_fill(&cv, 302 - 66, 209, 302 - w - (302 - 66), 12, 0xffff);
    np_atlas_update(&cv, &atl, 302 - w, 209, lbl_r, b);
    for (i = 0; i < 12; i++)
        lbl_r[i] = b[i];
}

int main(void)
{
    struct np_state st;
    struct np_event ev;
    struct np_result r;
    struct np_chapters ch = { chs, sizeof(chs[0]), 648 };
    struct np_scrub_ctx sx = { 648UL * 65000UL, { chs, sizeof(chs[0]), 648 },
                               true, { NULL, sizeof(uint32_t), 0 }, 0 };
    int k;

    setup();
    np_init(&st, 100, NP_SHUFFLE_OFF);
    np_set_track(&st, true, false, 0, 0, &sx, 0);
    memset(&ev, 0, sizeof(ev));
    for (k = 0; k < REPS; k++)
    {
#if WL == 1     /* art: slant + reflection of a 142 x 142 bundle */
        np_art_render(&cv, art, 142, 142, 0xffff);
#elif WL == 2   /* the placeholder image (once per no-art track) */
        np_placeholder_render(art, 142);
#elif WL == 3   /* 1 Hz tick, audiobook: state tick, chapter step, the
                 * bar's 1 px delta, both labels */
        ev.type = NP_EV_TICK;
        ev.tick = 100 * k;
        ev.elapsed_ms = 1000UL * k + 9000000UL;
        r = np_event(&st, &ev);
        sink += r.dirty;
        sink += np_chapter_step(&ch, ev.elapsed_ms, 138);
        np_bar_cols(&cv, 77, 206, 163, 40 + k, 41 + k, 41 + k, ticks, &bst);
        labels(ev.elapsed_ms, sx.length_ms);
#elif WL == 4   /* a scrub event: wheel in Scrubber, diamond off and on,
                 * labels */
        if (k == 0)
        {
            ev.type = NP_EV_SELECT;
            np_event(&st, &ev);
        }
        ev.type = NP_EV_WHEEL;
        ev.clicks = 1;
        ev.velocity = VEL;
        ev.tick = 10 * k;
        r = np_event(&st, &ev);
        /* the column the diamond left, then the 15 x 15 tile copy */
        np_fill(&cv, 100 + k, 205, 1, 1 + NP_BAR_H + NP_BAR_REFL, 0xffff);
        np_bar_cols(&cv, 77, 206, 163, 23 + k, 24 + k, 0, ticks, &bst);
        {
            int rr, cc;
            for (rr = 0; rr < 15; rr++)
                for (cc = 0; cc < 15; cc++)
                {
                    int xx = 101 + k + cc;
                    fb[(205 + rr) * 320 + xx] = (xx >= 77 && xx < 240)
                        ? art[rr * 15 + cc] : art[300 + rr * 15 + cc];
                }
        }
        labels(np_shown_pos(&st, 0), sx.length_ms);
        sink += (int)r.seek;
#elif WL == 5   /* a volume event: wheel in Normal, the bar's delta */
        ev.type = NP_EV_WHEEL;
        ev.clicks = 1;
        ev.velocity = 5;
        ev.tick = 10 * k;
        r = np_event(&st, &ev);
        np_bar_cols(&cv, 44, 206, 233, 100 + 3 * k, 103 + 3 * k, 103 + 3 * k,
                    NULL, &bst);
        sink += r.volume_clicks;
#elif WL == 6   /* a marquee step: the 152 x 18 window blit */
        {
            int y;
            for (y = 0; y < 18; y++)
                memcpy(fb + (58 + y) * 320 + 160, strip + y * 1392 + 2 * k,
                       152 * sizeof(np_px));
        }
#elif WL == 7   /* a rating event: the one cell that flipped */
        ev.type = NP_EV_WHEEL;
        ev.clicks = 1;
        r = np_event(&st, &ev);
        np_fill(&cv, 133 - 8 + 20 * (k % 5), 204, 17, 17, 0xffff);
        np_glyph_draw(&cv, NPG_STAR_BIG, 133 - 7 + 20 * (k % 5), 205,
                      bst.fill, 0, 0);
        sink += r.dirty;
#elif WL == 8   /* a lyrics line step: move 8 lines of 21 px (the page)
                 * up one line; the new line's text is font work */
        {
            int y;
            for (y = 28; y < 28 + 9 * 21 - 21; y++)
                memcpy(fb + y * 320, fb + (y + 21) * 320,
                       320 * sizeof(np_px));
        }
#elif WL == 9   /* lyrics wrap of 8 KB (once per Lyrics entry) */
        {
            struct np_lyr_font f = { adv, 0, 296 };
            sink += np_lyrics_wrap(lyr, 8191, &f, starts, 512);
        }
#elif WL == 13  /* P6 multi-file book, fast scrub click: the screen
                 * re-points the book context (np_set_scrub), then the
                 * wheel snaps over the union of 1,024 file starts and
                 * the playing file's 648 chapters (the worst case) */
        if (k == 0)
        {
            ev.type = NP_EV_SELECT;
            np_event(&st, &ev);
        }
        {
            struct np_scrub_ctx bx;
            memset(&bx, 0, sizeof(bx));
            bx.length_ms = 1024UL * 3600000UL / 64;
            bx.snap = true;
            bx.ch.start = (const uint32_t *)bstarts;
            bx.ch.stride = sizeof(uint32_t);
            bx.ch.count = 1024;
            bx.sub = ch;
            bx.sub_base = 512UL * 56250UL;
            np_set_scrub(&st, &bx);
        }
        ev.type = NP_EV_WHEEL;
        ev.clicks = 1;
        ev.velocity = 30;
        ev.tick = 10 * k;
        r = np_event(&st, &ev);
        sink += (int)r.seek + (int)np_shown_pos(&st, 0);
#elif WL == 14  /* P6: the map's ticks rescaled to the bar (per track) */
        sink += ab_ticks_scale(ticks, 204, bticks);
#elif WL == 15  /* P6 1 Hz tick, multi-file book: the P5 tick (WL 3)
                 * plus the book's extras: the position add, the counter
                 * entry (only drawn at a chapter change; counted every
                 * tick as the bound) */
        ev.type = NP_EV_TICK;
        ev.tick = 100 * k;
        ev.elapsed_ms = 1000UL * k + 9000000UL;
        r = np_event(&st, &ev);
        sink += r.dirty;
        sink += np_chapter_step(&ch, ev.elapsed_ms, 138);
        sink += ab_book_entry_at(&bmap, 600, 138);
        sink += (int)(ev.elapsed_ms + bmap.file_start_ms[600]);
        np_bar_cols(&cv, 77, 206, 163, 40 + k, 41 + k, 41 + k, ticks, &bst);
        labels(ev.elapsed_ms, sx.length_ms);
#elif WL == 10  /* audiobook ticks: 648 chapters on the bar (per track) */
        sink += np_chapter_ticks(&ch, 648UL * 65000UL, 163, ticks);
#elif WL == 16  /* QW2 track change: draw_art()'s blit of the composed
                 * art (white around it, 171 row copies; the read is I/O) */
        {
            int y;
            np_fill(&cv, 0, 20, 160, 14, 0xffff);
            np_fill(&cv, 0, 34, 10, 171, 0xffff);
            np_fill(&cv, 148, 34, 12, 171, 0xffff);
            for (y = 0; y < NP_COMPOSED_H; y++)
                memcpy(fb + (34 + y) * 320 + 10, comp + y * NP_COMPOSED_W,
                       NP_COMPOSED_W * sizeof(np_px));
        }
#elif WL == 17  /* QW2 placeholder: the rodata card into the art area */
        np_ph_decode(NPG_NOTE, comp);
#elif WL == 18  /* QW2 builder: the composed render (once per bundle) */
        {
            struct np_canvas cc;
            np_comp_canvas(&cc, comp);
            np_art_render(&cc, art, 142, 142, 0xffff);
        }
#elif WL == 11  /* a slide frame: two row-band strip copies (34 rows) */
        {
            int y;
            for (y = 0; y < 34; y++)
            {
                memcpy(fb + (196 + y) * 320, strip + y * 320 + 10 * k,
                       (320 - 10 * k) * sizeof(np_px));
                memcpy(fb + (196 + y) * 320 + 320 - 10 * k,
                       strip + 5000 + y * 320, 10 * k * sizeof(np_px));
            }
        }
#endif
    }
    (void)ev;
    (void)r;
    return 0;
}
