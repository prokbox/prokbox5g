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
 *  tools/convttf.c with extra knobs for the shell fonts (gen.sh).
 *
 * The upstream converter is included unchanged; these environment
 * variables adjust what it gets from FreeType:
 *
 *   CONVTTF_HDPI=<dpi>   horizontal resolution for FT_Set_Char_Size()
 *                        (default: the -X value), scales glyphs HDPI / -X
 *   CONVTTF_VDPI=<dpi>   vertical resolution (default: -X); with -X 72 and
 *                        -p N the em is N * VDPI / 72 px, so this gives
 *                        fractional pixel sizes
 *   CONVTTF_TRACK=<px>   tracking: added to every advance before rounding
 *                        to whole pixels, computed from the unhinted
 *                        (linear) advance so fractional values average out
 *                        across a word (needs -A)
 *   CONVTTF_HINT=<mode>  normal (default) | light | auto | none
 *   CONVTTF_WEIGHT=<px>  stem weight change (see shell_render())
 *   CONVTTF_GAP=1        keep i/j dots and accents clear of the letter
 *                        (see gap_fix()): moved by whole pixels, height
 *                        snapped, one empty pixel row between
 *   CONVTTF_GAP=soft     same parts, natural height and fractional
 *                        edges, CONVTTF_GAP_PX (default 0.75) px from the
 *                        letter, so the mark anti-aliases like the rest
 *   CONVTTF_CP1252=1     curly quotes etc. in U+0080..U+009F
 *                        (see shell_cp1252[])
 *   CONVTTF_LSB=clip     negative left bearings (see shell_lsb()): ink
 *                        left of the pen origin is dropped instead of the
 *                        whole glyph moving right; CONVTTF_LSB_REPORT=1
 *                        lists each glyph and the ink dropped (stderr)
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

static FT_UInt env_res(const char *name, FT_UInt res)
{
    const char *s = getenv(name);
    return (s && atoi(s) > 0) ? (FT_UInt)atoi(s) : res;
}

static FT_Int32 shell_load_flags(FT_Int32 flags)
{
    const char *s = getenv("CONVTTF_HINT");
    if (!s || !strcmp(s, "normal"))
        return flags;
    if (!strcmp(s, "light"))
        return flags | FT_LOAD_TARGET_LIGHT;
    if (!strcmp(s, "auto"))
        return flags | FT_LOAD_FORCE_AUTOHINT;
    if (!strcmp(s, "autolight"))
        return flags | FT_LOAD_FORCE_AUTOHINT | FT_LOAD_TARGET_LIGHT;
    if (!strcmp(s, "none"))
        return flags | FT_LOAD_NO_HINTING;
    return flags;
}

/* Replace the (rounded, hinted) advance with round(linear + track). */
static FT_Error shell_track(FT_Face face, FT_Error err)
{
    const char *s = getenv("CONVTTF_TRACK");
    if (!err && s) {
        FT_Pos lin = face->glyph->linearHoriAdvance >> 10; /* 16.16 -> 26.6 */
        FT_Pos adv = lin + (FT_Pos)(atof(s) * 64.0);
        face->glyph->metrics.horiAdvance = (adv + 32) & ~63;
        face->glyph->advance.x = face->glyph->metrics.horiAdvance;
    }
    return err;
}

/* CONVTTF_GAP: keep separate parts separate (i/j dots, accents, rings,
 * the dot of '!').
 *
 * At a 73/72 em the hinter rounds Heros' x-height up (8.2 -> 9 px at 15,
 * 6.6 -> 7 at 12) but aligns the i dot to the cap-height zone with its
 * height kept, so the dot's bottom lands on or below the stem's top
 * (15 px bold: stem 0..9.00, dot 8.86..11.00) and the two render as one
 * bar: "Muslc", "Settlngs". Accents (e, u, n with marks) end a fraction
 * of a pixel above the bowl and leave a grey row instead of a gap.
 *
 * Fix, on the hinted outline before rendering: group the contours into
 * parts (a contour inside another's box belongs to it). A part that the
 * unhinted outline shows as clear of a larger part below it (or above,
 * for '!') but that the hinter left without one whole empty pixel row
 * between them is moved away until there is one, and its height snapped
 * to whole pixels so both edges stay crisp. The larger part (stems,
 * bowls, capitals) never moves, so cap height, x-height and advances
 * are unchanged. A part below would only move under the baseline, so
 * those are left alone ('?'). */
#define GAP_MAXC 64
#define GAP_MIN  16     /* parts at least 0.25 px apart unhinted */

struct gap_box { FT_Pos xmin, xmax, ymin, ymax; };

static int gap_n;                         /* parts of the current glyph */
static int gap_part[GAP_MAXC];            /* contour -> part */
static struct gap_box gap_un[GAP_MAXC];   /* unhinted part boxes */
static FT_Pos gap_top;                    /* cell ascent, 26.6 */
static int gap_soft;                      /* CONVTTF_GAP=soft */
static FT_Pos gap_px;                     /* soft gap, 26.6 */

static void gap_boxes(const FT_Outline *o, struct gap_box *b)
{
    int k, p, s = 0;
    for (k = 0; k < o->n_contours; k++) {
        b[k].xmin = b[k].ymin = 1L << 30;
        b[k].xmax = b[k].ymax = -(1L << 30);
        for (p = s; p <= o->contours[k]; p++) {
            const FT_Vector *v = &o->points[p];
            if (v->x < b[k].xmin) b[k].xmin = v->x;
            if (v->x > b[k].xmax) b[k].xmax = v->x;
            if (v->y < b[k].ymin) b[k].ymin = v->y;
            if (v->y > b[k].ymax) b[k].ymax = v->y;
        }
        s = o->contours[k] + 1;
    }
}

static void gap_union(struct gap_box *d, const struct gap_box *s)
{
    if (s->xmin < d->xmin) d->xmin = s->xmin;
    if (s->xmax > d->xmax) d->xmax = s->xmax;
    if (s->ymin < d->ymin) d->ymin = s->ymin;
    if (s->ymax > d->ymax) d->ymax = s->ymax;
}

/* From the unhinted outline: the parts and their boxes. */
static void gap_record(const FT_Outline *o)
{
    struct gap_box c[GAP_MAXC];
    int k, j, n = o->n_contours;

    gap_n = 0;
    if (n < 2 || n > GAP_MAXC)
        return;
    gap_boxes(o, c);
    for (k = 0; k < n; k++) {
        int root = k;
        for (j = 0; j < n; j++) {
            if (j != k && c[j].xmin <= c[k].xmin && c[j].xmax >= c[k].xmax
                && c[j].ymin <= c[k].ymin && c[j].ymax >= c[k].ymax
                && (c[j].xmax - c[j].xmin) + (c[j].ymax - c[j].ymin) >
                   (c[k].xmax - c[k].xmin) + (c[k].ymax - c[k].ymin)) {
                root = j;
                break;
            }
        }
        gap_part[k] = root;
    }
    /* renumber roots 0..parts-1 */
    for (k = 0; k < n; k++) {
        if (gap_part[k] == k) {
            for (j = 0; j < n; j++)
                if (j != k && gap_part[j] == k)
                    gap_part[j] = gap_n + GAP_MAXC;
            gap_part[k] = gap_n + GAP_MAXC;
            gap_un[gap_n] = c[k];
            gap_n++;
        }
    }
    for (k = 0; k < n; k++) {
        gap_part[k] -= GAP_MAXC;
        gap_union(&gap_un[gap_part[k]], &c[k]);
    }
}

/* Map part p's y range [ob, ot] onto [nb, nb + nh]. */
static void gap_move(FT_Outline *o, int p, FT_Pos ob, FT_Pos ot,
                     FT_Pos nb, FT_Pos nh)
{
    int k, i, s = 0;
    for (k = 0; k < o->n_contours; k++) {
        if (gap_part[k] == p) {
            for (i = s; i <= o->contours[k]; i++) {
                FT_Pos y = o->points[i].y - ob;
                o->points[i].y = nb + (ot > ob ? y * nh / (ot - ob) : 0);
            }
        }
        s = o->contours[k] + 1;
    }
}

static int gap_xover(int a, int b)
{
    return gap_un[a].xmin < gap_un[b].xmax && gap_un[b].xmin < gap_un[a].xmax;
}

#define GAP_FLOOR(x) ((x) & ~63)
#define GAP_CEIL(x)  (((x) + 63) & ~63)
#define GAP_ROUND(x) (((x) + 32) & ~63)

static void gap_fix(FT_Outline *o)
{
    struct gap_box c[GAP_MAXC], h[GAP_MAXC];
    int k, s, b, pass;

    if (gap_n < 2 || o->n_contours > GAP_MAXC)
        return;
    for (pass = 0; pass < 2; pass++) {
        /* 0: parts above a larger part, bottom-up so stacked marks
         *    (dieresis + macron) move together; 1: parts below one. */
        int order[GAP_MAXC], i, j;
        for (i = 0; i < gap_n; i++)
            order[i] = i;
        for (i = 1; i < gap_n; i++)
            for (j = i; j > 0; j--) {
                FT_Pos a = gap_un[order[j - 1]].ymin;
                FT_Pos z = gap_un[order[j]].ymin;
                if (pass ? a >= z : a <= z)
                    break;
                k = order[j]; order[j] = order[j - 1]; order[j - 1] = k;
            }

        for (i = 0; i < gap_n; i++) {
            FT_Pos sb, st, sh, nb, nh;
            int best = -1;

            gap_boxes(o, c);  /* hinted, current */
            for (k = 0; k < gap_n; k++)
                h[k].ymin = 1L << 30, h[k].ymax = -(1L << 30),
                h[k].xmin = h[k].xmax = 0;
            for (k = 0; k < o->n_contours; k++)
                gap_union(&h[gap_part[k]], &c[k]);

            s = order[i];
            sb = h[s].ymin; st = h[s].ymax; sh = st - sb;
            for (b = 0; b < gap_n; b++) {
                if (b == s || !gap_xover(s, b)
                    || 2 * (h[b].ymax - h[b].ymin) < 3 * sh)  /* b 1.5x taller */
                    continue;
                if (pass == 0 && gap_un[b].ymax + GAP_MIN <= gap_un[s].ymin
                    && (best < 0 || h[b].ymax > h[best].ymax))
                    best = b;
                if (pass == 1 && gap_un[s].ymax + GAP_MIN <= gap_un[b].ymin
                    && (best < 0 || h[b].ymin < h[best].ymin))
                    best = b;
            }
            if (best < 0)
                continue;
            if (gap_soft) {
                /* natural (unhinted) height, gap_px from the larger part,
                   edges left fractional so the mark anti-aliases */
                FT_Pos uh = gap_un[s].ymax - gap_un[s].ymin;
                if (pass == 0) {
                    nb = h[best].ymax + gap_px;
                    if (sb >= nb)
                        continue;
                    if (nb + uh > gap_top)
                        nb = gap_top - uh;
                    if (nb <= sb)
                        continue;
                } else {
                    nb = h[best].ymin - gap_px - uh;
                    if (st <= nb + uh || nb < GAP_ROUND(sb))
                        continue;
                }
                gap_move(o, s, sb, st, nb, uh);
                continue;
            }
            nh = GAP_ROUND(sh);
            if (nh < 64)
                nh = 64;
            if (pass == 0) {
                if (GAP_CEIL(h[best].ymax) + 64 <= sb)
                    continue;               /* already a clean row */
                nb = GAP_ROUND(sb);
                if (nb < GAP_CEIL(h[best].ymax) + 64)
                    nb = GAP_CEIL(h[best].ymax) + 64;
                if (nb + nh > gap_top)      /* stay inside the cell */
                    nb = gap_top - nh;
                if (nb <= sb)
                    continue;
            } else {
                FT_Pos nt = GAP_ROUND(st);
                if (GAP_FLOOR(h[best].ymin) - 64 >= st)
                    continue;
                if (nt > GAP_FLOOR(h[best].ymin) - 64)
                    nt = GAP_FLOOR(h[best].ymin) - 64;
                nb = nt - nh;
                if (nb < GAP_ROUND(sb))
                    continue;               /* would sink below its line */
            }
            gap_move(o, s, sb, st, nb, nh);
        }
    }
}

/* Render after the outline fixes: CONVTTF_GAP (above) and the stem
 * weight change CONVTTF_WEIGHT=<px> (negative = lighter), the total
 * horizontal width change of a stem. */
static FT_Error shell_render(FT_Face face, FT_Error err, FT_Int32 flags)
{
    const char *s = getenv("CONVTTF_WEIGHT");
    if (err || !(flags & FT_LOAD_RENDER)
        || !(s || getenv("CONVTTF_GAP")))
        return err;
    if (face->glyph->format == FT_GLYPH_FORMAT_OUTLINE) {
        if (getenv("CONVTTF_GAP"))
            gap_fix(&face->glyph->outline);
        if (s) {
            FT_Pos str = (FT_Pos)(atof(s) * 64.0);
            FT_Outline_EmboldenXY(&face->glyph->outline, str, 0);
        }
    }
    return FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL);
}

static FT_Int32 shell_pre(FT_Int32 flags)
{
    flags = shell_load_flags(flags);
    if (getenv("CONVTTF_WEIGHT") || getenv("CONVTTF_GAP"))
        flags &= ~FT_LOAD_RENDER;
    return flags;
}

/* The unhinted outline, for CONVTTF_GAP's part boxes. */
static void shell_unhinted(FT_Face face, FT_UInt idx, FT_Int32 flags)
{
    gap_n = 0;
    if (!getenv("CONVTTF_GAP") || !(flags & FT_LOAD_RENDER))
        return;
    gap_top = face->size->metrics.ascender & ~63;   /* as convttf.c */
    gap_soft = !strcmp(getenv("CONVTTF_GAP"), "soft");
    gap_px = (FT_Pos)((getenv("CONVTTF_GAP_PX") ?
                       atof(getenv("CONVTTF_GAP_PX")) : 0.75) * 64.0);
    if (!FT_Load_Glyph(face, idx, (shell_pre(flags) | FT_LOAD_NO_HINTING))
        && face->glyph->format == FT_GLYPH_FORMAT_OUTLINE)
        gap_record(&face->glyph->outline);
}

/* CONVTTF_LSB=clip: a .fnt glyph is a cell that starts at the pen
 * origin, and the renderer cannot draw left of it. With -A, convttf.c
 * clamps a negative bitmap_left to 0, which moves the whole glyph right
 * by that many pixels while its advance stays put: the 'j' of Regular 12
 * (left -1) stands 1 px off the letter before it ("E ject") and 1 px too
 * close to the one after. Here the columns left of the origin are
 * dropped instead, so every stem lands where FreeType put it and the
 * spacing on both sides is right; what is lost is the overhang (the tip
 * of j's hook), listed by CONVTTF_LSB_REPORT. */
static void shell_lsb(FT_Face face, FT_Error err, FT_Int32 flags)
{
    FT_GlyphSlot g = face->glyph;
    FT_Bitmap *b = &g->bitmap;
    const char *s = getenv("CONVTTF_LSB");
    unsigned row, col, c0, lost = 0, kept = 0;

    if (err || !s || strcmp(s, "clip") || !(flags & FT_LOAD_RENDER)
        || g->format != FT_GLYPH_FORMAT_BITMAP
        || b->pixel_mode != FT_PIXEL_MODE_GRAY || g->bitmap_left >= 0)
        return;
    c0 = (unsigned)-g->bitmap_left;
    if (c0 > b->width)
        c0 = b->width;
    for (row = 0; row < b->rows; row++) {
        unsigned char *p = b->buffer + (long)row * b->pitch;
        for (col = 0; col < b->width; col++)
            *(col < c0 ? &lost : &kept) += p[col];
        memmove(p, p + c0, b->width - c0);
    }
    if (getenv("CONVTTF_LSB_REPORT"))
        fprintf(stderr, "lsb glyph %u left %d cols %u ink dropped %u.%02u px"
                " of %u.%02u\n", (unsigned)g->glyph_index, g->bitmap_left,
                c0, lost / 255, lost % 255 * 100 / 255,
                (lost + kept) / 255, (lost + kept) % 255 * 100 / 255);
    b->width -= c0;
    g->bitmap_left = 0;
}

static FT_Error shell_load_glyph(FT_Face face, FT_UInt idx, FT_Int32 flags)
{
    FT_Error err;
    shell_unhinted(face, idx, flags);
    err = FT_Load_Glyph(face, idx, shell_pre(flags));
    err = shell_track(face, shell_render(face, err, flags));
    shell_lsb(face, err, flags);
    return err;
}

static FT_Error shell_load_char(FT_Face face, FT_ULong code, FT_Int32 flags)
{
    return shell_load_glyph(face, FT_Get_Char_Index(face, code), flags);
}

/* CONVTTF_CP1252: fill the C1 control slots U+0080..U+009F with the
 * Windows-1252 punctuation (curly quotes, en and em dash, ellipsis) so the
 * font stays one contiguous range; firmware/font.c (HAVE_IPOD_UI) maps
 * those code points onto the slots. Only these seven, at their
 * Windows-1252 positions (each costs a full-height cell, about 100 B at
 * 15 px); the other slots stay empty. Must match the table there. */
static const unsigned short shell_cp1252[32] = {
    0, 0, 0, 0, 0, 0x2026, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0x2018, 0x2019, 0x201C, 0x201D, 0, 0x2013, 0x2014,
    0, 0, 0, 0, 0, 0, 0, 0,
};

static FT_UInt shell_char_index(FT_Face face, FT_ULong code)
{
    if (code >= 0x80 && code <= 0x9F && getenv("CONVTTF_CP1252")) {
        if (!shell_cp1252[code - 0x80])
            return 0;
        code = shell_cp1252[code - 0x80];
    }
    return FT_Get_Char_Index(face, code);
}

#define FT_Set_Char_Size(face, w, h, hres, vres) \
    FT_Set_Char_Size(face, w, h, env_res("CONVTTF_HDPI", hres), \
                     env_res("CONVTTF_VDPI", vres))
#define FT_Load_Glyph shell_load_glyph
#define FT_Load_Char  shell_load_char
#define FT_Get_Char_Index shell_char_index

#include "../../../tools/convttf.c"
