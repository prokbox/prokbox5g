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
 * iPod UI shell: the now-playing marker in lists (#69)
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
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "audio.h"
#include "backlight.h"
#include "metadata.h"
#include "ipodui/theme.h"
#include "ipodui/anim.h"        /* UPDATE_TRACE */
#include "ipodui/list.h"
#include "ipodui/np_draw.h"     /* the glyph's coverage */
#include "ipodui/np_playback.h"
#include "ipodui/screen_stack.h"
#include "ipodui/npmark.h"
#include "ipodui/ipodui.h"     /* ui_show_wps */
#include "playlist.h"

/* The anti-aliased font path's blit (firmware/drivers/lcd-color-common.c;
 * the core exports it, lcd.h leaves it out; as screens/search.c). */
extern void lcd_alpha_bitmap_part(const unsigned char *src, int src_x,
                                  int src_y, int stride, int x, int y,
                                  int width, int height);

#define ROWS_MAX    32          /* visible rows tracked (a view shows <= 9) */

/* The glyph in Rockbox's alpha format: 4 bits a pixel, 0 opaque .. 15
 * clear, the first pixel in the low nibble, rows packed (18 px: 9 bytes
 * a row). Built once from npglyphs.h, 117 B. */
static unsigned char mark_alpha[UI_NPMARK_W * UI_NPMARK_H / 2];
static bool mark_built;

static void build_mark(void)
{
    int x, y, i = 0;

    _Static_assert((UI_NPMARK_W & 1) == 0, "npmark: whole bytes per row");
    for (y = 0; y < UI_NPMARK_H; y++)
        for (x = 0; x < UI_NPMARK_W; x += 2, i++)
            mark_alpha[i] = (15 - np_glyph_cov(NPG_SPEAKER_WAVES, x, y))
                          | (15 - np_glyph_cov(NPG_SPEAKER_WAVES, x + 1, y))
                            << 4;
    mark_built = true;
}

void ui_npmark_draw(int x_right, int h, bool sel)
{
    const struct ui_theme *t = ui_theme;

    if (!mark_built)
        build_mark();
    lcd_set_foreground(sel ? t->sel_fg : t->list_fg);
    lcd_set_drawmode(DRMODE_FG);
    lcd_alpha_bitmap_part(mark_alpha, 0, 0, UI_NPMARK_W,
                          x_right - UI_NPMARK_W, (h - UI_NPMARK_H) / 2,
                          UI_NPMARK_W, UI_NPMARK_H);
    lcd_set_drawmode(DRMODE_SOLID);
}

bool ui_npmark_get(struct ui_npmark_now *n, unsigned what)
{
    struct ui_npmark_now v = { 0, 0, -1 };

    if (audio_status() & AUDIO_STATUS_PLAY)     /* paused too */
    {
        if (what & UI_NPMARK_TC)
        {
            struct mp3entry *id3 = audio_current_track();

            v.tc = id3 ? id3->tagcache_idx : 0;
        }
        if (what & UI_NPMARK_BOOK)
        {
            v.entry = np_pb_book_entry(&v.book);
            if (v.entry == -2)
                return false;
        }
    }
    *n = v;
    return true;
}

void ui_npmark_sync(struct ui_list *l, struct ui_npmark_now *cur,
                    unsigned what)
{
    struct ui_npmark_now now;
    uint32_t was = 0;
    int i, n, y, h;

    if (!l->has_mark || ui_list_gliding(l) || !is_backlight_on(true)
        || !ui_npmark_get(&now, what)
        || !memcmp(&now, cur, sizeof(now)))
        return;
    for (n = 0; n < ROWS_MAX && ui_list_row_band(l, l->top + n, &y, &h); n++)
        if (l->has_mark(l->top + n, l->data))
            was |= 1u << n;
    UPDATE_TRACE("npmark tc=%ld->%ld entry=%d->%d", cur->tc, now.tc,
                 cur->entry, now.entry);
    *cur = now;
    for (i = 0; i < n; i++)
    {
        bool is = l->has_mark(l->top + i, l->data);

        if (is != ((was >> i) & 1) && ui_list_row_band(l, l->top + i, &y, &h))
            ui_invalidate_band(y, h);
    }
}

static uint32_t origin_key;
static unsigned long origin_tick;

void ui_npmark_origin(uint32_t key)
{
    origin_key = key;
    origin_tick = playlist_get_current()->created_tick;
}

uint32_t ui_npmark_key(const void *p, size_t n)
{
    const unsigned char *b = p;
    uint32_t h = 2166136261u;           /* FNV-1a */

    while (n--)
        h = (h ^ *b++) * 16777619u;
    return h | 2;                       /* never 0, never the OTG key */
}

bool ui_npmark_continue(struct ui_list *l, struct ui_npmark_now *cur,
                        unsigned what, uint32_t key, int qpos)
{
    struct ui_npmark_now was = *cur;

    if (!(what & UI_NPMARK_BOOK)
        && (!key || key != origin_key
            || playlist_get_current()->created_tick != origin_tick
            || (qpos >= 0 && qpos != playlist_get_display_index() - 1)))
        return false;                   /* another list: its own queue */
    if (!l->has_mark || !ui_npmark_get(cur, what))
        return false;
    if (!l->has_mark(l->selected, l->data))
    {
        *cur = was;     /* ui_npmark_sync() still sees the change */
        return false;
    }
    if (audio_status() & AUDIO_STATUS_PAUSE)
        audio_resume();
    ui_show_wps();
    return true;
}
