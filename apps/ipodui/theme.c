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
 * iPod UI shell
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
#include "lcd.h"
#include "font.h"
#include "rbpaths.h"
#include "logf.h"
#include "settings.h"
#include "audio.h"
#include "tagcache.h"
#include "core_alloc.h"
#include "rbunicode.h"
#include "lang.h"
#include "ipodui/libglyph.h"
#include "ipodui/uilog.h"
#include "ipodui/theme.h"
#include "ipodui/screens/panels.h"
#include "ipodui/paint.h"
#include "ipodui/material.h"
#include "ipodui_classa_lang.h"   /* generated: gen_classa_lang.py */

/* Header, y 0..19: the material kit's header rows (art/materials.py
 * HEADER, the main.png profile measured there). */
typedef char header_rows_fit[(UI_MAT_HEADER_ROWS == UI_HEADER_ROWS) ? 1 : -1];

/* Glossy surfaces (paint.h, docs/design/7g-spec.md "Visual language").
 * Every selection blue (the list bar, the speed prompt's segment) is the
 * material kit's sky gel (material.h UI_MAT_GEL_SKY, art/materials.py). */

/* Scrollbar thumb (no reference, ~): a grey rounded rod lit from the
 * left, so the gloss runs across its width. */
static const struct ui_gloss sbar_gloss = {
    .top    = LCD_RGBPACK(0x8c, 0x8c, 0x8c),
    .bot    = LCD_RGBPACK(0x66, 0x66, 0x66),
    .edge   = LCD_RGBPACK(0x58, 0x58, 0x58),
    .hi_top = 120,
    .hi_mid = 64,
    .split  = 128,
    .flags  = UI_GLOSS_EDGE | UI_GLOSS_HORIZ,
};

/* Writable only for the three font ids filled in by ui_theme_init();
 * the colour tables above stay in flash/rodata. */
static struct ui_theme theme = {
    .header_h         = 20,
    .header_title_x   = 6,      /* cell x; "iPod" ink starts at x=7 */
    .header_baseline  = 14,     /* "iPod" cap rows 5-13 in main.png */
    .header_title_pad = 2,      /* P20: "Einstellungen" (86 px) fits a
                                 * split header; 2 px clear of the lock
                                 * when lock and play both show */
    .header_side_w    = 66,     /* lock + play + battery + gaps + margin */
    .header_margin_r  = 5,      /* nub ends at x 154 (155 edge) */
    .header_glyph_gap = 7,      /* play -> battery (audit fix #7) */
    .header_rows      = ui_mat_header_rows,
    .header_fg        = LCD_RGBPACK(0x00, 0x00, 0x00),
    .header_glyph_fg  = LCD_RGBPACK(0x3a, 0x3a, 0x3a),
    /* Owner photos (artists-list-b, songs-list-scrollbar-b,
     * albums-list-twoline-b, music-menu-b), rectified to 320 x 240:
     * 9 single-line rows (cap rows 6/7-17 of each row, also on the
     * selection), 5 two-line song rows of 44 px, 4 album rows of 55 px
     * with a 55 px art box and text at x 65; header 20 + body 220. */
    .list_rows        = 9,
    .list_rows_two    = 5,
    .list_rows_art    = 4,
    .row_baseline     = 18,
    .text_pad_x       = 5,      /* cell x; ink at x 6 (audit fix #10) */
    .two_block_h      = 29,
    .two_title_base   = 12,
    .two_sub_dy       = 17,
    .art_box          = 55,
    .art_text_x       = 65,
    .dot_gutter       = 14,     /* ~ no reference */
    /* second line: ~#707070 regular (thin strokes read lighter in the
     * photos); on the selection about as bright as the title: white */
    .list_fg_sub      = LCD_RGBPACK(0x70, 0x70, 0x70),
    .sel_fg_sub       = LCD_RGBPACK(0xff, 0xff, 0xff),
    .list_sep         = LCD_RGBPACK(0xd6, 0xd6, 0xd6),
    .chevron_pad      = 6,      /* chevron ink x 145-153, values end at
                                 * 153.5 (audit fixes #8, #9) */
    .split_x          = 160,
    .list_bg          = LCD_RGBPACK(0xff, 0xff, 0xff),
    .list_fg          = LCD_RGBPACK(0x00, 0x00, 0x00),
    .list_fg_dim      = LCD_RGBPACK(0x3c, 0x3c, 0x3c),
    .list_fg_off      = LCD_RGBPACK(0xa8, 0xa8, 0xa8),  /* ~ no reference */
    .sel_fg           = LCD_RGBPACK(0xff, 0xff, 0xff),
    .sel_bg           = LCD_RGBPACK(34, 141, 219), /* sky gel 12/24 */
    .sbar_gloss       = &sbar_gloss,
    .panel_bg         = LCD_RGBPACK(46, 47, 52),
    .panel_top        = LCD_RGBPACK(0x4b, 0x56, 0x6c),
    .panel_mid        = LCD_RGBPACK(0x1e, 0x26, 0x38),
    .panel_bot        = LCD_RGBPACK(0x07, 0x09, 0x0f),
    .panel_mid_y      = 110,
    .panel_fg         = LCD_RGBPACK(0xff, 0xff, 0xff),
    .tile_top         = LCD_RGBPACK(0x3a, 0x3d, 0x45),
    .tile_bot         = LCD_RGBPACK(0x04, 0x05, 0x08),
    .tile_rim         = LCD_RGBPACK(0x6a, 0x6e, 0x78),
    .value_fg         = LCD_RGBPACK(0x1f, 0x6c, 0xd6),
    .font_ui          = FONT_UI,
    .font_title       = FONT_UI,
    .font_small       = FONT_UI,
};

const struct ui_theme *ui_theme = &theme;

/* Shell fonts, installed from apps/ipodui/fonts/ (TeX Gyre Heros, GUST
 * license, converted with tools/convttf -A). font_load_ex(path, 0, 0)
 * keeps the whole file (up to MAX_FONT_SIZE, 60000 B) in one movable
 * buflib allocation: no glyph cache, no .gc file and no disk access while
 * drawing. RAM = the file sizes, 102,114 B for the three (2026-11-06). */
static int load_font(const char *name, int cur)
{
    char path[64];
    int id;

    if (cur != FONT_UI)
        return cur;     /* already loaded by an earlier ui_theme_init() */
    snprintf(path, sizeof(path), FONT_DIR "/%s.fnt", name);
    id = font_load_ex(path, 0, 0);
#ifdef HAVE_UI_THREAD_SPLIT
    /* P19 S3: pinned for the shell's life (loaded once, before playback):
     * the COP draws from it while a compaction on the CPU could move it */
    if (id >= 0)
        font_lock(id, true);
#endif
    if (id < 0)
    {
        logf("ipodui: font %s failed (%d), using FONT_UI", path, id);
        return FONT_UI;
    }
    return id;
}

/* -- ShellCJK companions (#235, #266) -----------------------------------
 * Kana, kanji and Hangul come from three companion fonts of the same
 * height and ascent as their ShellSans partner (apps/ipodui/fonts/
 * gen_cjk.py, the sparse form of firmware/export/font_sparse.h;
 * firmware/font.c draws a code point the companion holds from it). They
 * are large files (cut A, 1.9-3.0 MB each), so only their index (5.8 KB,
 * shared by the three) and a movable glyph LRU are resident; a miss is
 * one read of one record. Two tiers (design 5.3):
 *  - base, every library: 32/16/16 slots (~15 KB with the index), so a
 *    Japanese title, subtitle or Notes page draws on any library;
 *  - full, while the UI is Japanese or the library holds CJK text (its
 *    glyph file says so, libglyph.h): 256/64/64 slots, the library glyph
 *    file as the list font's page source (libglyph_fill.c) and the
 *    letter cards' companion (35-ShellCJK-Index, in RAM): ~70-90 KB.
 * A tier change allocates, so it waits for a moment when nothing plays
 * (ui_theme_cjk_poll(); a paused song counts as quiet). Missing files:
 * logged, CJK draws as the ShellSans default glyph, and Settings >
 * Language does not offer Japanese (ui_theme_has_cjk()). */
static const struct {
    const char *name;
    short glyphs[2];        /* LRU slots: base tier, full tier */
} cjk_fonts[3] = {
    { "14-ShellCJK-Bold", { 32, 256 } },        /* for font_ui */
    { "11-ShellCJK-Bold", { 16,  64 } },        /* for font_title */
    { "11-ShellCJK",      { 16,  64 } },        /* for font_small */
};
#define CJK_INDEX_FONT  "35-ShellCJK-Index"     /* the letter cards' */
#define CJK_LETTERS     "35-ShellSans-Letters"  /* qscroll.c QS_FONT */
#define CJK_MAP_MAX     8192    /* resident glyph-file table, 4 B each */
static int cjk_id[3] = { -1, -1, -1 };
static int cjk_index_id = -1;
static int cjk_big = -1;            /* the tier loaded, -1 none yet */
static bool cjk_pinned;             /* jaglyph: cjk_pin() */
static int cjk_lib_glyphs;          /* the library glyph file's count */
static int32_t cjk_seen_commit = -2;
static int cjk_seen_count = -2;

bool ui_theme_has_cjk(void)
{
    return cjk_id[0] >= 0;
}

static int cjk_load(int i, int big)
{
    char path[64];

    snprintf(path, sizeof(path), FONT_DIR "/%s.fnt", cjk_fonts[i].name);
    return font_load_ex(path, 0, cjk_fonts[i].glyphs[big]);
}

/* the letter cards: the index font as the companion of qscroll's letters
 * font, in the full tier only (a Latin library never shows a kana card) */
static void cjk_index(bool on)
{
    char path[64];
    int id;

    if (on && cjk_index_id < 0)
    {
        snprintf(path, sizeof(path), FONT_DIR "/%s.fnt", CJK_INDEX_FONT);
        cjk_index_id = font_load_ex(path, 0, 0);
        if (cjk_index_id < 0)
            logf("ipodui: cjk index font failed (%d)", cjk_index_id);
#ifdef HAVE_UI_THREAD_SPLIT
        else
            font_lock(cjk_index_id, true);
#endif
    }
    snprintf(path, sizeof(path), FONT_DIR "/%s.fnt", CJK_LETTERS);
    for (id = 0; id < MAXFONTS; id++)
        if (font_filename_matches_loaded_id(id, path))
            font_set_companion(id, on ? cjk_index_id : -1);
    if (!on && cjk_index_id >= 0)
    {
        font_unload(cjk_index_id);
        cjk_index_id = -1;
    }
}

static void cjk_apply(int big)
{
    const int base[3] = { theme.font_ui, theme.font_title, theme.font_small };
    int i;

    if (big == cjk_big)
        return;
    cjk_pinned = false;     /* the reload drops each font's pinned set */
    for (i = 0; i < 3; i++)
    {
        int id;

        if (base[i] == FONT_UI)
            continue;               /* no ShellSans partner to attach to */
        if (cjk_id[i] >= 0)
        {
            /* a reload: font_load_ex() only grows a loaded font, and
             * counts a second load as a second user */
            font_set_companion(base[i], -1);
            font_unload(cjk_id[i]);
            cjk_id[i] = -1;
        }
        id = cjk_load(i, big);
        if (id < 0 && big)
        {
            logf("ipodui: cjk font %s full tier failed (%d)",
                 cjk_fonts[i].name, id);
            id = cjk_load(i, 0);    /* the base tier, rather than none */
        }
        if (id < 0)
        {
            logf("ipodui: cjk font %s failed (%d)", cjk_fonts[i].name, id);
            continue;
        }
#ifdef HAVE_UI_THREAD_SPLIT
        font_lock(id, true);    /* the COP draws from it (as load_font) */
#endif
        cjk_id[i] = id;
        font_set_companion(base[i], id);
    }
    cjk_big = big;
}

static bool cjk_japanese(void)
{
    return !strcmp((const char *)global_settings.lang_file, "japanese");
}

/* jaglyph (p090-int4): a Class A Extra may not read storage while it runs
 * (docs/design/sdk.md 1.2, 6.1a), but in Japanese its kana and
 * kanji are sparse-font glyphs, one read each on a cache miss. So while
 * the shell is Japanese the glyphs of every string a Class A module can
 * draw (ipodui_classa_lang[], generated from the modules' sources) stay
 * in RAM in each companion (font_sparse_pin()): a miss copies, never
 * reads. Loaded with the tier (a quiet moment), in a forward pass of
 * 4 KB reads; ~250 glyphs, ~94 KB for the three sizes (measured in the
 * "cjk pin" line). Text from the user's files (notes, contacts) still
 * draws through the cache. */
#define CJK_PIN_MAX 1024                /* code points collected (2 KB) */

static int cjk_pin_collect(uint16_t *cps)
{
    int n = 0;

    for (size_t k = 0; k < ARRAYLEN(ipodui_classa_lang); k++)
    {
        const unsigned char *s = (const unsigned char *)
                                 str(ipodui_classa_lang[k]);
        while (s && *s)
        {
            ucschar_t c;
            int lo = 0, hi = n;

            s = utf8decode(s, &c);
            if (c < 0x3000 || c > 0xffff)
                continue;
            while (lo < hi)             /* the insertion point */
            {
                int mid = (lo + hi) >> 1;
                if (cps[mid] < c)
                    lo = mid + 1;
                else
                    hi = mid;
            }
            if (lo < n && cps[lo] == c)
                continue;
            if (n == CJK_PIN_MAX)
                return n;               /* logged by the caller */
            memmove(&cps[lo + 1], &cps[lo], (n - lo) * sizeof(*cps));
            cps[lo] = c;
            n++;
        }
    }
    return n;
}

static void cjk_pin(bool on)
{
    long bytes = 0;
    int reads = 0, n = 0, h, i;
    uint16_t *cps;

    if (on == cjk_pinned)
        return;
    if (!on)
    {
        for (i = 0; i < 3; i++)
            if (cjk_id[i] >= 0)
                font_sparse_pin(cjk_id[i], NULL, 0, NULL);
        cjk_pinned = false;
        ui_log("cjk", "pin off");
        return;
    }
    h = core_alloc_ex(CJK_PIN_MAX * sizeof(uint16_t), NULL);
    if (h <= 0)
    {
        ui_log("cjk", "pin failed alloc");
        return;
    }
    cps = core_get_data_pinned(h);
    n = cjk_pin_collect(cps);
    for (i = 0; i < 3; i++)
    {
        int r = 0;
        long b;

        if (cjk_id[i] < 0)
            continue;
        b = font_sparse_pin(cjk_id[i], cps, n, &r);
        reads += r;
        if (b < 0)
            ui_log("cjk", "pin failed font=%s", cjk_fonts[i].name);
        else
            bytes += b;
    }
    core_put_data_pinned(cps);
    core_free(h);
    cjk_pinned = true;
    ui_log("cjk", "pin glyphs=%d%s strings=%d bytes=%ld reads=%d", n,
           n == CJK_PIN_MAX ? " (capped)" : "",
           (int)ARRAYLEN(ipodui_classa_lang), bytes, reads);
}

/* At boot and after Settings > Language. The tier and the Class A pin
 * allocate (~70-90 KB and ~94 KB), past the 64 KB playing reserve, so
 * while a song plays they are owed: the seen stamp is cleared and
 * ui_theme_cjk_poll() does both at its next quiet moment (stopped or
 * paused), as after a library commit. Settings > Language stops the
 * song first (#487, its settle step), so there both run here, timed
 * ("cjk settle"). */
void ui_theme_cjk_for_lang(void)
{
    int status = audio_status();
    long t0, t1;

    if (cjk_big >= 0 && (status & AUDIO_STATUS_PLAY)
        && !(status & AUDIO_STATUS_PAUSE))
    {
        cjk_seen_commit = -2;
        cjk_seen_count = -2;
        ui_log("cjk", "tier owed (playing) japanese=%d", cjk_japanese());
        return;
    }
    t0 = current_tick;
    cjk_apply(cjk_japanese() || cjk_lib_glyphs > 0);
    t1 = current_tick;
    cjk_pin(cjk_japanese());
    ui_log("cjk", "settle tier=%d apply ms=%ld pin ms=%ld", cjk_big,
           (t1 - t0) * (1000 / HZ), (current_tick - t1) * (1000 / HZ));
}

void ui_theme_cjk_poll(void)
{
    struct libglyph_stamp st;
    size_t bytes = 0;
    int status, big;

    if (cjk_big < 0)
        return;                     /* before ui_theme_init() */
    st.commitid = tagcache_ui_commitid();
    st.entries = tagcache_ui_count();
    if (st.commitid == cjk_seen_commit && st.entries == cjk_seen_count)
        return;
    status = audio_status();
    if ((status & AUDIO_STATUS_PLAY) && !(status & AUDIO_STATUS_PAUSE))
        return;                     /* an allocation now is a gap: later */
    cjk_seen_commit = st.commitid;
    cjk_seen_count = st.entries;
    libglyph_attach(NULL, &st, -1, 0, &bytes);          /* detach */
    cjk_lib_glyphs = 0;
    if (st.entries > 0)
    {
        struct libglyph_hdr h;
        if (libglyph_check(tagcache_ui_db_path(), &st, &h) == LIBGLYPH_OK)
            cjk_lib_glyphs = h.count;
    }
    big = cjk_japanese() || cjk_lib_glyphs > 0;
    cjk_apply(big);
    cjk_pin(cjk_japanese());
    if (big && cjk_lib_glyphs > 0 && cjk_id[0] >= 0
        && font_cache_slots(cjk_id[0]) >= 128)
        libglyph_attach(tagcache_ui_db_path(), &st, cjk_id[0], CJK_MAP_MAX,
                        &bytes);
    cjk_index(big);
    ui_log("cjk", "tier %s glyphs=%d map=%ld slots=%d",
           big ? "full" : "base", cjk_lib_glyphs, (long)bytes,
           font_cache_slots(cjk_id[0]));
}

void ui_theme_init(void)
{
    theme.font_ui    = load_font("15-ShellSans-Bold", theme.font_ui);
    theme.font_title = load_font("12-ShellSans-Bold", theme.font_title);
    theme.font_small = load_font("12-ShellSans", theme.font_small);
    ui_theme_cjk_for_lang();
    ui_theme = &theme;
    ui_settings_init();             /* localised Settings labels (#38) */
}
