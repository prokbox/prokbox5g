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
 * iPod UI shell: Now Playing memory layout (Phase 5)
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
#ifndef _IPODUI_NP_LAYOUT_H_
#define _IPODUI_NP_LAYOUT_H_

/* Contract (docs/phase5-brief.md §6, Revision 1 items 5 and 13). Fixed
 * offsets, so nothing is allocated after shell start and the host test
 * (tests/nowplaying/test_np_layout.c) checks that nothing overlaps.
 * Pure: no Rockbox headers (the checks against anim.h's strip size are at
 * the bottom, compiled only where anim.h was included first). */

#include "np_playback.h"    /* struct np_track */
#include "np_state.h"       /* struct np_state */

#define NP_ALIGN4(x)        (((x) + 3u) & ~3u)
#define NP_ALIGN32(x)       (((x) + 31u) & ~31u)
#define NP_CAT_(a, b)       a##b
#define NP_CAT(a, b)        NP_CAT_(a, b)
#define NP_STATIC_ASSERT(c) \
    typedef char NP_CAT(np_static_assert_, __LINE__)[(c) ? 1 : -1] \
        __attribute__((unused))

/* ---- art (frozen default 9.1, Revision 1 item 3) ----------------------- */
#define NP_ART_BOX          142     /* ARTCACHE_NOWPLAYING box (artfmt.c);
                                       a fitted bundle is w x h <= this */

/* ---- the strip at rest, owner UI_STRIP_OWNER_NP (Revision 1 item 5) ----
 * Claimed with ui_strip_claim(UI_STRIP_OWNER_NP, &kept); 'kept' false
 * (a push/pop capture or a glide used the strip) means every part below
 * is rebuilt before use. While Now Playing is on top no other owner but
 * SLIDE (its own pushes, pops and mode slides) claims it (preview.h). */
#define NP_LCD_W            320

/* F2 marquee: the title rendered once, Bold 15, capped at 1,200 px,
 * + 40 px gap + the 152 px window it is shown through (x 160-311 of the
 * text column's 160-315 less the clip margin), 18 rows. */
#define NP_MARQUEE_W        1392
#define NP_MARQUEE_H        18
#define NP_STRIP_MARQUEE_OFF    0u
#define NP_STRIP_MARQUEE_SIZE   (NP_MARQUEE_W * NP_MARQUEE_H * 2)  /* 50,112 */

/* Mode slides: the old row and the new row (bar row y 198-231), full
 * width. The row starts at 198 (Rev 3 fix B; was 196) so the art's
 * reflection band inside it (rows 198-204, NP_REFL_BAND_*) fits the
 * block's spare bytes; nothing in the row is drawn above y 203. */
#define NP_ROW_Y            198
#define NP_ROW_H            34
#define NP_STRIP_ROW_SIZE   (NP_LCD_W * NP_ROW_H * 2)              /* 21,760 */
#define NP_STRIP_ROW_OLD_OFF \
    NP_ALIGN4(NP_STRIP_MARQUEE_OFF + NP_STRIP_MARQUEE_SIZE)
#define NP_STRIP_ROW_NEW_OFF \
    NP_ALIGN4(NP_STRIP_ROW_OLD_OFF + NP_STRIP_ROW_SIZE)

/* Body pushes (Lyrics in and out): the new body, 320 x 220, at the
 * strip's start: the body capture of Lyrics -> Normal, or the lyrics page
 * pre-rendered once for Lyrics' slide-in (end review low: no text
 * rendering per frame). Overlaps the marquee and the row captures (both
 * rebuilt after it), never the lyrics text below. */
#define NP_STRIP_PAGE_OFF   0u
#define NP_STRIP_BODY_SIZE  (NP_LCD_W * 220 * 2)                   /* 140,800 */

/* Lyrics (frozen default 9.9): the UTF-8 text (cut at 8 KB, logged) and
 * the start offset of each wrapped line, after the page (Rev 3 fix B; was
 * after the row captures). */
#define NP_LYRICS_MAX       8192
#define NP_LYRICS_LINES_MAX 512     /* uint16_t line starts */
#define NP_STRIP_LYRICS_OFF \
    NP_ALIGN4(NP_STRIP_PAGE_OFF + NP_STRIP_BODY_SIZE)              /* 140,800 */
#define NP_STRIP_LYRICS_SIZE NP_LYRICS_MAX
#define NP_STRIP_LINES_OFF \
    NP_ALIGN4(NP_STRIP_LYRICS_OFF + NP_STRIP_LYRICS_SIZE)
#define NP_STRIP_LINES_SIZE (NP_LYRICS_LINES_MAX * 2)              /*  1,024 */

/* #126, the visualizer's per-frame scratch (viz.c): the window (512
 * int16) and the CPU path's powers (256 uint32), filled and used inside
 * one frame, so a slide's capture over it between frames costs nothing.
 * Past every Now Playing part above; never kept across a yield. */
#define NP_STRIP_VIZ_OFF    NP_ALIGN32(NP_STRIP_LINES_OFF + NP_STRIP_LINES_SIZE)
#define NP_STRIP_VIZ_SIZE   2048
#define NP_STRIP_END        (NP_STRIP_VIZ_OFF + NP_STRIP_VIZ_SIZE)
#define NP_STRIP_LIMIT      180000  /* UI_STRIP_SIZE (anim.h) on this target */
NP_STATIC_ASSERT(NP_STRIP_END <= NP_STRIP_LIMIT);                  /* 152,064 */
NP_STATIC_ASSERT(NP_STRIP_BODY_SIZE <= NP_STRIP_LYRICS_OFF);
NP_STATIC_ASSERT(NP_STRIP_ROW_NEW_OFF + NP_STRIP_ROW_SIZE
                 <= NP_STRIP_LYRICS_OFF);

/* ---- the "np" block (core_alloc'd once at shell start, movable, pinned
 * only inside a draw and across artcache_get(); Revision 1 item 13) -----
 * QW2 (docs/phase11-brief.md sections 3.9, 4.8; Rev 1 M-4): the art area
 * holds the COMPOSED art, the slanted card and its reflection against
 * NP_WHITE, x 9-145 x y 33-219 (np_draw.h NP_COMPOSED_*), exactly what
 * the screen shows there: a composed bundle as read, the placeholder as
 * decoded from rodata, or a legacy (fitted) bundle after its runtime
 * render. NP_BLK_HDR (the bundle's 512 B header) lies right before it,
 * so a bundle is ONE read() of header + pixels into the block. */
#define NP_BLK_NAME         "np"
#define NP_COMP_X           9
#define NP_COMP_Y           33
#define NP_COMP_W           137
#define NP_COMP_H           187
#define NP_COMP_SIZE        (NP_COMP_W * NP_COMP_H * 2)            /* 51,238 */

#define NP_BLK_HDR_OFF      0u      /* artbundle_hdr landing (512 B) */
#define NP_BLK_HDR_SIZE     512     /* = ARTBUNDLE_HDR_BYTES (artfmt.h) */
#define NP_BLK_ART_OFF      (NP_BLK_HDR_OFF + NP_BLK_HDR_SIZE)
#define NP_BLK_ART_SIZE     NP_COMP_SIZE
#define NP_BLK_DIGITS_OFF   NP_ALIGN4(NP_BLK_ART_OFF + NP_BLK_ART_SIZE)
#define NP_BLK_DIGITS_SIZE  2304    /* Bold 12 atlas "0-9 : -" */
#define NP_BLK_GLYPHS_OFF   NP_ALIGN4(NP_BLK_DIGITS_OFF + NP_BLK_DIGITS_SIZE)
#define NP_BLK_GLYPHS_SIZE  2624    /* struct np_styles (2,578 B; asserted
                                       in nowplaying.c) */
#define NP_BLK_TRACK_OFF    NP_ALIGN4(NP_BLK_GLYPHS_OFF + NP_BLK_GLYPHS_SIZE)
#define NP_BLK_TRACK_SIZE   576     /* struct np_track (P6: + book, 528 B host) */
#define NP_BLK_STATE_OFF    NP_ALIGN4(NP_BLK_TRACK_OFF + NP_BLK_TRACK_SIZE)
#define NP_BLK_STATE_SIZE   256     /* struct np_state */
#define NP_BAR_W            216     /* the longest bar (np_bar_span) */
#define NP_BLK_TICKS_OFF    NP_ALIGN4(NP_BLK_STATE_OFF + NP_BLK_STATE_SIZE)
#define NP_BLK_TICKS_SIZE   NP_ALIGN4((NP_BAR_W + 7) / 8)          /*     28 */
#define NP_BLK_END          (NP_BLK_TICKS_OFF + NP_BLK_TICKS_SIZE)  /* 57,540 */
/* + nowplaying.c's struct np_scr at NP_BLK_END rounded up to 8, asserted there
 * to end inside NP_BLK_SIZE (P11 geometry + glass-floor reflection:
 * 58,368 B, was 54,272; docs/hardware.md, orchestrator) */
#define NP_BLK_SIZE         58368

NP_STATIC_ASSERT(NP_BLK_END <= NP_BLK_SIZE);

/* The art reflection inside the row band (np-reflection.md §6): rows
 * NP_ROW_Y .. 219 of x 9-145 (P11: it runs behind the elapsed time and
 * the bar). Every row redraw and every slide frame
 * composes over it instead of flat white. QW2 (Rev 1 M-4): it is not a
 * copy but an ALIAS of the composed art's rows NP_ROW_Y - NP_COMP_Y ..
 * (165-186), whose stride is the same 137 px. */
#define NP_REFL_BAND_X      9
#define NP_REFL_BAND_W      137
#define NP_REFL_BAND_H      (220 - NP_ROW_Y)                       /*     22 */
#define NP_REFL_BAND_SIZE   (NP_REFL_BAND_W * NP_REFL_BAND_H * 2)  /*  6,028 */
#define NP_REFL_BAND_ROW    (NP_ROW_Y - NP_COMP_Y)                 /*    165 */
NP_STATIC_ASSERT(NP_REFL_BAND_X == NP_COMP_X && NP_REFL_BAND_W == NP_COMP_W);
NP_STATIC_ASSERT(NP_REFL_BAND_ROW + NP_REFL_BAND_H == NP_COMP_H);
NP_STATIC_ASSERT(sizeof(struct np_track) <= NP_BLK_TRACK_SIZE);
NP_STATIC_ASSERT(sizeof(struct np_state) <= NP_BLK_STATE_SIZE);

#ifdef UI_STRIP_SIZE        /* anim.h included first: the real strip */
NP_STATIC_ASSERT(NP_STRIP_END <= UI_STRIP_SIZE);
NP_STATIC_ASSERT(NP_STRIP_LIMIT <= UI_STRIP_SIZE);  /* the overlay loads
                                     read up to it (art_bcm.c, viz.c) */
NP_STATIC_ASSERT(NP_STRIP_BODY_SIZE <= UI_STRIP_SIZE);
#endif

#endif /* _IPODUI_NP_LAYOUT_H_ */
