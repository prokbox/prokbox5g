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
 * iPod UI shell: list-row art (thumbnail LRU, glyph tiles, unplayed dot)
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
#include "lcd.h"
#include "core_alloc.h"
#include "logf.h"
#include "ipodui/theme.h"
#include "ipodui/paint.h"
#include "ipodui/artcache.h"
#include "ipodui/anim.h"
#include "ipodui/ptrace.h"
#include "ipodui/rowart.h"
#include "ipodui/rowglyphs.h"

/* Keys remembered: thumbnails held (state OK, a pixel slot each) plus the
 * answers that need no pixels (no art, not built yet), so a row never
 * asks twice. */
#define ROWART_KEYS     32
#define ST_NONE         (-128)

struct key_ent {
    uint64_t key;
    uint32_t used;              /* clock stamp of the last draw */
    int8_t   state;             /* enum artcache_result, ST_NONE = free */
    int8_t   slot;              /* pixel slot (state OK), else -1 */
};

struct rowart {
    uint32_t clock;
    int      commit_seq;        /* artcache_commit_seq() at last poll */
    int      alt;               /* glyph in the second tile, -1 none */
    struct key_ent k[ROWART_KEYS];
    /* then: 2 glyph tiles of box x box (the note; podcast or book),
     * ROWART_THUMBS thumbs of tw x th */
};

static int handle;              /* 0: none */
static int box, tw, th;         /* art box; thumbnail size (art cache) */

static size_t hdr_bytes(void)
{
    return ALIGN_UP(sizeof(struct rowart), sizeof(fb_data) * 2);
}

static fb_data *tile_px(struct rowart *r, int tile)
{
    return (fb_data *)((char *)r + hdr_bytes()) + (size_t)tile * box * box;
}

static fb_data *thumb_px(struct rowart *r, int slot)
{
    return tile_px(r, 0) + 2 * (size_t)box * box + (size_t)slot * tw * th;
}

/* -- glyph tiles ---------------------------------------------------------- */

/* The tile: a near-black slate square with a faint gloss on its top half
 * (owner photo artist-albums-allsongs-b: the All Songs tile). The glyphs
 * are glossy gradients: a sky-blue note, a violet broadcast mark. All ~.
 * A coverless audiobook instead gets a light grey book on the list's own
 * background, like the 7G (P6 owner; render_booktile()). */
static const struct ui_gloss tile_gloss = {
    .top    = LCD_RGBPACK(0x48, 0x4b, 0x50),
    .bot    = LCD_RGBPACK(0x10, 0x12, 0x14),
    .hi_top = 30,
    .hi_mid = 10,
    .split  = 116,
    .flags  = 0,
};
static const struct ui_gloss note_gloss = {
    .top    = LCD_RGBPACK(0x86, 0xd2, 0xff),
    .bot    = LCD_RGBPACK(0x16, 0x6a, 0xd6),
    .hi_top = 70,
    .hi_mid = 24,
    .split  = 116,
    .flags  = 0,
};
static const struct ui_gloss podcast_gloss = {
    .top    = LCD_RGBPACK(0xe0, 0xa8, 0xff),
    .bot    = LCD_RGBPACK(0x76, 0x2c, 0xc8),
    .hi_top = 70,
    .hi_mid = 24,
    .split  = 116,
    .flags  = 0,
};
#ifdef HAVE_BCMFW_VIDEO
/* a movie or show with no poster: a light grey film frame */
static const struct ui_gloss film_gloss = {
    .top    = LCD_RGBPACK(0xc4, 0xc9, 0xd3),
    .bot    = LCD_RGBPACK(0x84, 0x89, 0x93),
    .hi_top = 50,
    .hi_mid = 16,
    .split  = 116,
    .flags  = 0,
};
#endif
/* the dot: sky blue, lit from above */
static const struct ui_gloss dot_gloss = {
    .top    = LCD_RGBPACK(0x7e, 0xcc, 0xff),
    .bot    = LCD_RGBPACK(0x12, 0x6c, 0xdc),
    .hi_top = 90,
    .hi_mid = 30,
    .split  = 110,
    .flags  = 0,
};

static int cov_at(const unsigned char *cov, int w, int x, int y)
{
    int i = y * w + x;
    return (i & 1) ? (cov[i >> 1] & 0x0f) : (cov[i >> 1] >> 4);
}

static void render_tile(fb_data *px, const unsigned char *cov, int gw, int gh,
                        const struct ui_gloss *fg)
{
    int x0 = (box - gw) / 2, y0 = (box - gh) / 2;
    int x, y;

    for (y = 0; y < box; y++)
    {
        unsigned bg = ui_gloss_color(&tile_gloss, y, box);
        int gy = y - y0;
        unsigned ink = (gy >= 0 && gy < gh) ? ui_gloss_color(fg, gy, gh) : 0;

        for (x = 0; x < box; x++)
        {
            int gx = x - x0, a = 0;

            if (gy >= 0 && gy < gh && gx >= 0 && gx < gw)
                a = cov_at(cov, gw, gx, gy);
            px[y * box + x] = (fb_data)(a ? ui_paint_blend(bg, ink, a) : bg);
        }
    }
}

/* The grey book tile (rowglyphs.h booktile: alpha and grey-level planes)
 * centred on the list background */
static void render_booktile(fb_data *px)
{
    const int x0 = (box - BOOKTILE_W) / 2, y0 = (box - BOOKTILE_H) / 2;
    const unsigned bg = ui_theme->list_bg;
    unsigned grey[16];
    int x, y, i;

    for (i = 0; i < 16; i++)
    {
        int v = BOOKTILE_LO + i * (BOOKTILE_HI - BOOKTILE_LO) / 15;
        grey[i] = LCD_RGBPACK(v, v, v);
    }
    for (y = 0; y < box; y++)
    {
        int gy = y - y0;

        for (x = 0; x < box; x++)
        {
            int gx = x - x0, a = 0;

            if (gy >= 0 && gy < BOOKTILE_H && gx >= 0 && gx < BOOKTILE_W)
                a = cov_at(booktile_cov, BOOKTILE_W, gx, gy);
            px[y * box + x] = (fb_data)(a ? ui_paint_blend(bg,
                grey[cov_at(booktile_tone_cov, BOOKTILE_W, gx, gy)], a)
                : bg);
        }
    }
}

bool ui_rowart_init(void)
{
    const struct artcache_dim *d = &artcache_sizes[ARTCACHE_THUMB];
    struct rowart *r;
    size_t bytes;
    int i;

    if (handle > 0)
        return true;
    box = ui_theme->art_box;
    tw = d->w;
    th = d->h;
    bytes = hdr_bytes() + (2 * (size_t)box * box
                           + (size_t)ROWART_THUMBS * tw * th) * sizeof(fb_data);
    handle = core_alloc(bytes);
    if (handle <= 0)
    {
        handle = 0;
        logf("ipodui: rowart: no memory (%lu B), glyphs only",
             (unsigned long)bytes);
        return false;
    }
    r = core_get_data(handle);
    memset(r, 0, sizeof(*r));
    for (i = 0; i < ROWART_KEYS; i++)
    {
        r->k[i].state = ST_NONE;
        r->k[i].slot = -1;
    }
    r->commit_seq = artcache_commit_seq();
    render_tile(tile_px(r, 0), note_cov, NOTE_W, NOTE_H, &note_gloss);
    r->alt = -1;                /* drawn on first use (blit_glyph()) */
    logf("ipodui: rowart %lu B (box %d, thumbs %dx%d x %d)",
         (unsigned long)bytes, box, tw, th, ROWART_THUMBS);
    return true;
}

/* -- key table ------------------------------------------------------------ */

static struct key_ent *find(struct rowart *r, uint64_t key)
{
    int i;
    for (i = 0; i < ROWART_KEYS; i++)
        if (r->k[i].state != ST_NONE && r->k[i].key == key)
            return &r->k[i];
    return NULL;
}

static void drop(struct key_ent *e)
{
    e->state = ST_NONE;
    e->slot = -1;
}

/* A free entry, else the least recently drawn one (forgotten). */
static struct key_ent *new_entry(struct rowart *r)
{
    struct key_ent *best = NULL;
    int i;

    for (i = 0; i < ROWART_KEYS; i++)
    {
        struct key_ent *e = &r->k[i];
        if (e->state == ST_NONE)
            return e;
        if (!best || (int32_t)(e->used - best->used) < 0)
            best = e;
    }
    drop(best);
    return best;
}

/* A free pixel slot, else the one of the least recently drawn thumb
 * (whose key is forgotten). */
static int new_slot(struct rowart *r)
{
    bool taken[ROWART_THUMBS];
    struct key_ent *lru = NULL;
    int i;

    memset(taken, 0, sizeof(taken));
    for (i = 0; i < ROWART_KEYS; i++)
    {
        struct key_ent *e = &r->k[i];
        if (e->state == ARTCACHE_OK && e->slot >= 0)
        {
            taken[e->slot] = true;
            if (!lru || (int32_t)(e->used - lru->used) < 0)
                lru = e;
        }
    }
    for (i = 0; i < ROWART_THUMBS; i++)
        if (!taken[i])
            return i;
    i = lru->slot;
    drop(lru);
    return i;
}

/* -- drawing -------------------------------------------------------------- */

static void blit_glyph(struct rowart *r, enum ui_rowart_glyph g, int x, int y,
                       int size)
{
    int n = MIN(size, box), tile = g == UI_GLYPH_NOTE ? 0 : 1;

    if (tile && r->alt != (int)g)
    {
        /* the list's own glyph: once per switch between the Podcasts and
         * Audiobooks lists (~3k blends), never per frame */
        if (g == UI_GLYPH_BOOK)
            render_booktile(tile_px(r, 1));
#ifdef HAVE_BCMFW_VIDEO
        else if (g == UI_GLYPH_FILM)
            render_tile(tile_px(r, 1), film_cov, FILM_W, FILM_H, &film_gloss);
#endif
        else
            render_tile(tile_px(r, 1), podcast_cov, PODCAST_W, PODCAST_H,
                        &podcast_gloss);
        r->alt = g;
    }
    lcd_bitmap_part(tile_px(r, tile), 0, 0, STRIDE_MAIN(box, box), x, y, n, n);
}

/* The thumbnail is the art box's size (artfmt.c's table, theme art_box) */
static void blit_thumb(struct rowart *r, int slot, int x, int y, int size)
{
    lcd_bitmap_part(thumb_px(r, slot), 0, 0, STRIDE_MAIN(tw, th), x, y,
                    MIN(tw, size), MIN(th, size));
}

/* Fills e (state, slot) from the art cache for its key: one lookup, and
 * one read when there is art. May yield; returns the block again. */
static struct rowart *fetch(struct key_ent **ep)
{
    struct rowart *r = core_get_data(handle);
    struct key_ent *e = *ep;
    uint64_t key = e->key;
    int idx = e - r->k;
    int st;

    /* never boosted while blocked on I/O (lean rule 6): a slide composes
     * its first frame boosted (F4), and that draw may land here; the
     * slide's first frame boosts again */
    ui_anim_idle();
    PT_VAR(t_look);
    st = artcache_state(key);
    PT_LAP(t_look);

    r = core_get_data(handle);              /* a lookup may read the index */
    e = &r->k[idx];
    PT_VAR(t_read);
    if (st == ARTCACHE_OK)
    {
        struct bitmap bm;
        int slot = new_slot(r);     /* never ours: e isn't OK yet */

        core_pin(handle);
        r = core_get_data(handle);
        st = artcache_get(key, ARTCACHE_THUMB, &bm, thumb_px(r, slot),
                          (size_t)tw * th * sizeof(fb_data));
        core_unpin(handle);
        r = core_get_data(handle);
        e = &r->k[idx];
        if (st == ARTCACHE_OK && (bm.width != tw || bm.height != th))
            st = ARTCACHE_ERR;
        e->slot = st == ARTCACHE_OK ? slot : -1;
    }
    e->state = st;
    *ep = e;
    PT_LAP(t_read);
    /* thumbnails are stored ready to blit: no decode step */
    PT_TRACE("rowart fetch key=%08lx%08lx res=%d lookup_us=%lu read_us=%lu "
             "decode_us=0", (unsigned long)(key >> 32),
             (unsigned long)(key & 0xffffffffu), st, t_look, t_read);
    return r;
}

bool ui_rowart_draw(uint64_t key, enum ui_rowart_glyph glyph, int x, int y,
                    int size, bool fetch_ok)
{
    struct rowart *r;
    struct key_ent *e;

    if (handle <= 0)
    {
        /* no block: a plain dark square stands in */
        lcd_set_foreground(ui_gloss_color(&tile_gloss, size / 2, size));
        lcd_fillrect(x, y, size, size);
        return true;
    }
    r = core_get_data(handle);
    if (!key)
    {
        blit_glyph(r, glyph, x, y, size);
        return true;
    }
    e = find(r, key);
    if (!e)
    {
        if (!fetch_ok)
        {
            blit_glyph(r, glyph, x, y, size);
            return false;
        }
        e = new_entry(r);
        e->key = key;
        e->slot = -1;
        e->state = ARTCACHE_PENDING;
        r = fetch(&e);
    }
    e->used = ++r->clock;
    PT_VAR(t_draw);
    if (e->state == ARTCACHE_OK && e->slot >= 0)
        blit_thumb(r, e->slot, x, y, size);
    else
        blit_glyph(r, glyph, x, y, size);
    PT_LAP(t_draw);
    PT_TRACE("rowart draw key=%08lx%08lx thumb=%d draw_us=%lu",
             (unsigned long)(key >> 32), (unsigned long)(key & 0xffffffffu),
             e->state == ARTCACHE_OK && e->slot >= 0, t_draw);
    return true;
}

bool ui_rowart_poll(void)
{
    struct rowart *r;
    bool dropped = false;
    int i, seq;

    if (handle <= 0)
        return false;
    /* F9: the reader only sees new art once the builder has committed
     * the index (every 32 albums or 30 s), so build progress alone
     * means nothing visible changed */
    seq = artcache_commit_seq();
    r = core_get_data(handle);
    if (seq == r->commit_seq)
        return false;
    r->commit_seq = seq;
    for (i = 0; i < ROWART_KEYS; i++)
        if (r->k[i].state == ARTCACHE_PENDING || r->k[i].state == ARTCACHE_ERR)
        {
            drop(&r->k[i]);
            dropped = true;
        }
    return dropped;
}

void ui_rowart_flush(void)
{
    struct rowart *r;
    int i;

    if (handle <= 0)
        return;
    r = core_get_data(handle);
    for (i = 0; i < ROWART_KEYS; i++)
        drop(&r->k[i]);
}

void ui_rowart_dot(int x, int y, int row_h, bool sel)
{
    const struct ui_theme *t = ui_theme;
    int cx, cy;

    for (cy = 0; cy < DOT_H; cy++)
    {
        unsigned bg = sel ? ui_paint_selection_row(y + cy, row_h) : t->list_bg;
        unsigned ink = sel ? t->sel_fg : ui_gloss_color(&dot_gloss, cy, DOT_H);

        for (cx = 0; cx < DOT_W; cx++)
        {
            int a = cov_at(dot_cov, DOT_W, cx, cy);
            if (!a)
                continue;
            lcd_set_foreground(a == 15 ? ink : ui_paint_blend(bg, ink, a));
            lcd_drawpixel(x + cx, y + cy);
        }
    }
}
