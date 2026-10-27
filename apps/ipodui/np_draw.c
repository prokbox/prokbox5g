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
 * iPod UI shell: Now Playing drawing core (Phase 5)
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
/* Pure C on purpose: no Rockbox headers (host-tested by tests/npscreen/,
 * instruction-counted bare metal). See np_draw.h. No divisions in the
 * per-pixel loops: the ARM7TDMI has no divide instruction. */
#include <stddef.h>
#include "np_draw.h"
#include "npglyphs.h"
#include "npph_data.h"

#define BAR_W_MAX 216               /* np_layout.h NP_BAR_W */

np_px np_blend256(np_px bg, np_px fg, unsigned a)
{
    int br = bg >> 11, bgn = (bg >> 5) & 63, bb = bg & 31;
    int fr = fg >> 11, fgn = (fg >> 5) & 63, fb = fg & 31;

    br += ((fr - br) * (int)a) >> 8;
    bgn += ((fgn - bgn) * (int)a) >> 8;
    bb += ((fb - bb) * (int)a) >> 8;
    return (np_px)((br << 11) | (bgn << 5) | bb);
}

/* fg over bg with a = 0..32, all three channels in one multiply: the
 * classic 565 trick, green spread into the high half-word. ~8
 * instructions; the per-pixel loops use this. */
static inline __attribute__((always_inline))
np_px blend32(np_px bg, np_px fg, unsigned a)
{
    uint32_t b = ((uint32_t)bg | ((uint32_t)bg << 16)) & 0x07e0f81fu;
    uint32_t f = ((uint32_t)fg | ((uint32_t)fg << 16)) & 0x07e0f81fu;
    uint32_t r = (b + (((f - b) * a) >> 5)) & 0x07e0f81fu;
    return (np_px)(r | (r >> 16));
}

np_px np_blend16(np_px bg, np_px fg, unsigned a)
{
    return a >= 16 ? fg : blend32(bg, fg, a << 1);
}

/* ---- slant ---------------------------------------------------------------- */

#define HL (NP_SLANT_BOT_L - NP_SLANT_TOP_L + 1)    /* 135 */
#define HR (NP_SLANT_BOT_R - NP_SLANT_TOP_R + 1)    /* 119 */
#define SW (NP_SLANT_W - 1)                          /* 136 */

int np_slant_top256(int c)
{
    return NP_SLANT_TOP_L * 256
           + ((NP_SLANT_TOP_R - NP_SLANT_TOP_L) * 256 * c + SW / 2) / SW;
}

int np_slant_h256(int c)
{
    return HL * 256 - ((HL - HR) * 256 * c + SW / 2) / SW;
}

int np_slant_u256(int c)
{
    /* box * c * hR / (W * hL - (hL - hR) * c), in 1/256: at most
     * 142 * 137 * 125 * 256 = 6.2e8, fits 32 bits unsigned */
    unsigned num = (unsigned)NP_SLANT_BOX * 256u * (unsigned)c * HR;
    unsigned den = (unsigned)(SW * HL - (HL - HR) * c);

    if (c <= 0)
        return 0;
    if (c >= SW)
        return NP_SLANT_BOX * 256;
    return (int)(num / den);
}

void np_fit_rect(int src_w, int src_h, int box, struct np_fit *f)
{
    int w = box, h = box;

    if (src_w > 0 && src_h > 0)
    {
        if (src_w >= src_h)         /* a square box */
            h = (box * src_h * 2 + src_w) / (2 * src_w);
        else
            w = (box * src_w * 2 + src_h) / (2 * src_h);
    }
    if (w < 1)
        w = 1;
    if (h < 1)
        h = 1;
    if (w > box)
        w = box;
    if (h > box)
        h = box;
    f->w = w;
    f->h = h;
    f->x = (box - w) / 2;
    f->y = (box - h) / 2;
}

/* The hot loops of np_art_render(), kept out of line so they get the
 * registers to themselves: n rows down one screen column, sampling the
 * image column 'col' (rows 'pitch' bytes apart) at v (16.16) per row. */
static void __attribute__((noinline, optimize("O2", "unroll-loops")))
col_samples(np_px *dst, int stride, const np_px *col, unsigned pitch,
            unsigned v, unsigned vstep, int n)
{
    const char *base = (const char *)col;

    while (n-- > 0)
    {
        *dst = *(const np_px *)(base + (v >> 16) * pitch);
        dst += stride;
        v += vstep;
    }
}

#define SPREAD(p) ((((uint32_t)(p)) | ((uint32_t)(p) << 16)) & 0x07e0f81fu)

/* The reflection of one column: rl rows below the contact line (screen
 * row y0 first), v (16.16, source rows) counting down by vstep. Alpha
 * starts at A0 and is multiplied by q (16.16) per row, q = exp(-K / h)
 * for this column's image height h (np-reflection.md §3, tau = d / h);
 * the last NP_REFL_FEATHER rows above the limit are feathered, and the
 * 5-bit blend is dithered (a half step on every other pixel, in a
 * checkerboard: 'dith' is this column's starting phase) so the faint far
 * rows fade without steps. Rows d < 15 take 1 source row, d < 29 three
 * (1 2 1), the rest five (1 2 2 2 1); 'blur' false (a short image) keeps
 * 1. The caller guarantees the taps stay in the image wherever the blur
 * is on. */
#define SPREADV(p) ((((uint32_t)(p)) | ((uint32_t)(p) << 16)) & 0x07e0f81fu)
#define PX(p, o)   SPREADV(*(const np_px *)((p) + (o)))

static inline __attribute__((always_inline))
np_px refl_blend(uint32_t sb, uint32_t s, unsigned a32)
{
    s = (sb + (((s - sb) * a32) >> 5)) & 0x07e0f81fu;
    return (np_px)(s | (s >> 16));
}

static void __attribute__((noinline, optimize("O2")))
col_reflect(np_px *dst, int stride, const np_px *col, int pitch,
            unsigned v, unsigned vstep, int rl, unsigned q, int y0,
            bool blur, unsigned dith, np_px bg)
{
    /* (rows_left / 12)^2 in 1/64, rows_left = 12 .. 1 (the last kept
     * at 1/64, not 0) */
    static const uint8_t feather[NP_REFL_FEATHER] = {
        64, 54, 44, 36, 28, 22, 16, 11, 7, 4, 2, 1,
    };
    const char *base = (const char *)col;
    const int p2 = 2 * pitch;
    uint32_t sb = SPREADV(bg), s;
    unsigned a = NP_REFL_A0 << 8;           /* /256, 8 fraction bits */
    unsigned t = dith ? 1536u : 512u;       /* the dither, alternating */
    int d = 0, fd = NP_REFL_LIMIT - NP_REFL_FEATHER - y0;
    int d1 = rl < 15 ? rl : 15, d2 = rl < 29 ? rl : 29;

    if (fd < 0)
        fd = 0;
    if (fd > rl)
        fd = rl;
    if (!blur)
        d1 = d2 = rl;
    if (d1 > fd)
        d1 = fd;
    if (d2 > fd)
        d2 = fd;
    for (; d < d1; d++)
    {
        const char *p = base + (v >> 16) * (unsigned)pitch;
        *dst = refl_blend(sb, PX(p, 0), (a + t) >> 11);
        dst += stride;
        v -= vstep;
        a = (a * q) >> 16;
        t ^= 512u ^ 1536u;
    }
    for (; d < d2; d++)
    {
        const char *p = base + (v >> 16) * (unsigned)pitch;
        s = ((2 * PX(p, 0) + PX(p, -pitch) + PX(p, pitch)) >> 2)
            & 0x07e0f81fu;
        *dst = refl_blend(sb, s, (a + t) >> 11);
        dst += stride;
        v -= vstep;
        a = (a * q) >> 16;
        t ^= 512u ^ 1536u;
    }
    for (; d < fd; d++)
    {
        const char *p = base + (v >> 16) * (unsigned)pitch;
        s = ((2 * (PX(p, 0) + PX(p, -pitch) + PX(p, pitch)) + PX(p, -p2)
              + PX(p, p2)) >> 3) & 0x07e0f81fu;
        *dst = refl_blend(sb, s, (a + t) >> 11);
        dst += stride;
        v -= vstep;
        a = (a * q) >> 16;
        t ^= 512u ^ 1536u;
    }
    /* the feather: at most NP_REFL_FEATHER rows, unblurred */
    for (; d < rl; d++)
    {
        const char *p = base + (v >> 16) * (unsigned)pitch;
        *dst = refl_blend(sb, PX(p, 0),
                          ((a + t) * feather[d - fd]) >> 17);
        dst += stride;
        v -= vstep;
        a = (a * q) >> 16;
        t ^= 512u ^ 1536u;
    }
}
#undef PX
#undef SPREADV

/* np_slant_u256(c) >> 8 (clamped to the box), np_slant_top256(c) and
 * np_slant_h256(c) for every slant column, generated from those
 * functions (the host test checks they agree): no divisions per column
 * on the ARM7 (685 B of const data). */
static const uint8_t slant_u[NP_SLANT_W] = {
    0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 13,
    14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29,
    30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45,
    46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61,
    62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 77, 78,
    79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 92, 93, 94, 95,
    96, 97, 98, 99, 100, 101, 103, 104, 105, 106, 107, 108, 109, 110, 111, 113,
    114, 115, 116, 117, 118, 119, 121, 122, 123, 124, 125, 126, 127, 129, 130, 131,
    132, 133, 134, 136, 137, 138, 139, 140, 141,
};
static const uint16_t slant_top[NP_SLANT_W] = {
    8448, 8469, 8489, 8510, 8531, 8552, 8572, 8593, 8614, 8634,
    8655, 8676, 8696, 8717, 8738, 8759, 8779, 8800, 8821, 8841,
    8862, 8883, 8904, 8924, 8945, 8966, 8986, 9007, 9028, 9048,
    9069, 9090, 9111, 9131, 9152, 9173, 9193, 9214, 9235, 9256,
    9276, 9297, 9318, 9338, 9359, 9380, 9400, 9421, 9442, 9463,
    9483, 9504, 9525, 9545, 9566, 9587, 9608, 9628, 9649, 9670,
    9690, 9711, 9732, 9752, 9773, 9794, 9815, 9835, 9856, 9877,
    9897, 9918, 9939, 9960, 9980, 10001, 10022, 10042, 10063, 10084,
    10104, 10125, 10146, 10167, 10187, 10208, 10229, 10249, 10270, 10291,
    10312, 10332, 10353, 10374, 10394, 10415, 10436, 10456, 10477, 10498,
    10519, 10539, 10560, 10581, 10601, 10622, 10643, 10664, 10684, 10705,
    10726, 10746, 10767, 10788, 10808, 10829, 10850, 10871, 10891, 10912,
    10933, 10953, 10974, 10995, 11016, 11036, 11057, 11078, 11098, 11119,
    11140, 11160, 11181, 11202, 11223, 11243, 11264,
};
static const uint16_t slant_h[NP_SLANT_W] = {
    34560, 34530, 34500, 34470, 34440, 34409, 34379, 34349, 34319, 34289,
    34259, 34229, 34199, 34168, 34138, 34108, 34078, 34048, 34018, 33988,
    33958, 33928, 33897, 33867, 33837, 33807, 33777, 33747, 33717, 33687,
    33656, 33626, 33596, 33566, 33536, 33506, 33476, 33446, 33416, 33385,
    33355, 33325, 33295, 33265, 33235, 33205, 33175, 33144, 33114, 33084,
    33054, 33024, 32994, 32964, 32934, 32904, 32873, 32843, 32813, 32783,
    32753, 32723, 32693, 32663, 32632, 32602, 32572, 32542, 32512, 32482,
    32452, 32422, 32392, 32361, 32331, 32301, 32271, 32241, 32211, 32181,
    32151, 32120, 32090, 32060, 32030, 32000, 31970, 31940, 31910, 31880,
    31849, 31819, 31789, 31759, 31729, 31699, 31669, 31639, 31608, 31578,
    31548, 31518, 31488, 31458, 31428, 31398, 31368, 31337, 31307, 31277,
    31247, 31217, 31187, 31157, 31127, 31096, 31066, 31036, 31006, 30976,
    30946, 30916, 30886, 30856, 30825, 30795, 30765, 30735, 30705, 30675,
    30645, 30615, 30584, 30554, 30524, 30494, 30464,
};

void np_art_render(const struct np_canvas *cv, const np_px *art,
                   int art_w, int art_h, np_px bg)
{
    np_art_render_cols(cv, art, art_w, art_h, bg, 0, NP_SLANT_W);
}

void np_art_render_cols(const struct np_canvas *cv, const np_px *art,
                        int art_w, int art_h, np_px bg, int c0, int c1)
{
    struct np_fit f;
    unsigned pitch = (unsigned)art_w * sizeof(np_px);
    int c;

    if (!art || art_w <= 0 || art_h <= 0)
        return;
    /* the bundle is already the fitted image: place it in the box */
    f.w = art_w;
    f.h = art_h;
    f.x = (NP_SLANT_BOX - art_w) / 2;
    f.y = (NP_SLANT_BOX - art_h) / 2;

    if (c0 < 0)
        c0 = 0;
    if (c1 > NP_SLANT_W)
        c1 = NP_SLANT_W;
    for (c = c0; c < c1; c++)
    {
        int x = NP_SLANT_X + c;
        int u = slant_u[c];
        int top = slant_top[c], h = slant_h[c];
        int y0f, y1f, y0, y1, rl, stride = cv->stride;
        unsigned v, vstep, vmax, x16, q, a0;
        bool blur;
        const np_px *col;
        np_px *dst, p;

        if (u < f.x || u >= f.x + f.w || x >= cv->w)
            continue;
        col = art + (u - f.x);

        /* the image's top and bottom edges on this column, 1/256 px */
        y0f = top + (int)(((unsigned)f.y * (unsigned)h) / NP_SLANT_BOX);
        y1f = top + (int)(((unsigned)(f.y + f.h) * (unsigned)h)
                          / NP_SLANT_BOX);
        y0 = y0f >> 8;
        y1 = (y1f + 255) >> 8;          /* exclusive */
        if (y1 - y0 < 2)
            continue;
        /* image rows per screen row, 16.16 (142 box rows over h / 256
         * screen rows); v from the image's own top edge, sampled at
         * each row's centre */
        vstep = ((unsigned)NP_SLANT_BOX << 24) / (unsigned)h;
        vmax = ((unsigned)f.h << 16) - 1;
        v = (unsigned)((y0 * 256 + 128 - y0f) > 0 ? (y0 * 256 + 128 - y0f)
                                                  : 0) * (vstep >> 8);
        dst = cv->px + y0 * stride + x;

        /* top row: antialiased against bg */
        p = col[(v >> 16) * art_w];
        *dst = (y0f & 255) ? blend32(bg, p, (256 - (y0f & 255)) >> 3) : p;
        dst += stride;
        v += vstep;
        /* the rows between: plain samples, no branches */
        col_samples(dst, stride, col, pitch, v, vstep, y1 - y0 - 2);
        dst += (y1 - y0 - 2) * stride;
        v += (unsigned)(y1 - y0 - 2) * vstep;
        /* bottom row: antialiased against the reflection under it */
        if (v > vmax)
            v = vmax;
        p = col[(v >> 16) * art_w];
        a0 = (NP_REFL_A0 + 4) >> 3;
        *dst = (y1f & 255) ? blend32(blend32(bg, p, a0), p,
                                     (y1f & 255) >> 3)
                           : p;
        dst += stride;

        /* the reflection (np-reflection.md): mirrored about this
         * column's own bottom edge, compressed by its own scale, alpha
         * A0 exp(-K d / h) with h the image's height on this column
         * (tau index step inc = 256 * 256 / h px, in 1/256), blurred
         * more with distance, feathered into the limit */
        rl = NP_REFL_LIMIT - y1;
        if (rl > NP_REFL_LEN)
            rl = NP_REFL_LEN;
        if (y1 + rl > cv->h)
            rl = cv->h - y1;
        if (rl > 0 && (unsigned)rl * vstep > vmax)
            rl = (int)(vmax / vstep);
        if (rl <= 0)
            continue;
        /* q = exp(-K / h) in 16.16, h = (y1f - y0f) / 256 px:
         * x = K / h (16.16), q = 1 - x + x^2 / 2 (x < 0.07: exact to
         * 2e-5 per row) */
        x16 = (unsigned)NP_REFL_K_Q8 * 65536u / (unsigned)(y1f - y0f);
        q = 65536u - x16 + ((x16 * x16) >> 17);
        v = vmax - (vstep >> 1);
        /* the 5-tap blur reads 2 rows past the last sample: only where
         * the image is tall enough (always, but for strip-like art) */
        blur = ((v - (unsigned)(rl - 1) * vstep) >> 16) >= 2;
        col_reflect(dst, stride, col, (int)pitch, v, vstep, rl, q, y1, blur,
                    (x + y1) & 1, bg);
    }
}

void np_comp_canvas(struct np_canvas *cv, np_px *comp)
{
    /* screen (x, y) is comp[(y - Y) * W + (x - X)]; computed as an
     * integer so no out-of-object pointer is ever formed by arithmetic
     * the compiler could reason about */
    cv->px = (np_px *)((uintptr_t)comp
                       - (uintptr_t)(NP_COMPOSED_Y * NP_COMPOSED_W
                                     + NP_COMPOSED_X) * sizeof(np_px));
    cv->stride = NP_COMPOSED_W;
    cv->w = NP_COMPOSED_X + NP_COMPOSED_W;
    cv->h = NP_COMPOSED_Y + NP_COMPOSED_H;
}

typedef char npph_size_ok[(NPPH_W == NP_COMPOSED_W
                           && NPPH_H == NP_COMPOSED_H) ? 1 : -1];

bool np_ph_decode(enum np_glyph_id id, np_px *comp)
{
    const unsigned short *pal;
    const unsigned char *s, *e;
    np_px *d = comp, *end = comp + NP_COMPOSED_W * NP_COMPOSED_H;

    if (id == NPG_NOTE)
    {
        pal = npph_note_pal;
        s = npph_note_data;
        e = s + sizeof(npph_note_data);
    }
    else if (id == NPG_BOOK)
    {
        pal = npph_book_pal;
        s = npph_book_data;
        e = s + sizeof(npph_book_data);
    }
    else
        return false;
    while (d < end && s < e)
    {
        unsigned n = *s++;

        if (n < 128)            /* n + 1 literal indices */
        {
            if (n + 1 > (unsigned)(end - d) || n + 1 > (unsigned)(e - s))
                return false;
            for (n++; n; n--)
                *d++ = pal[*s++];
        }
        else                    /* one index, n - 126 times */
        {
            np_px c;

            n -= 126;
            if (n > (unsigned)(end - d) || s >= e)
                return false;
            c = pal[*s++];
            while (n--)
                *d++ = c;
        }
    }
    return d == end;
}

void np_placeholder_render(np_px *art, int box)
{
    np_placeholder_render_glyph(art, box, NPG_NOTE);
}

void np_placeholder_render_glyph(np_px *art, int box, enum np_glyph_id id)
{
    const struct np_glyph *g = &np_glyphs[id];
    int rb = (g->w + 1) / 2;
    /* upscaled 5/2: 36 x 42 -> 90 x 105 */
    int gw = g->w * 5 / 2, gh = g->h * 5 / 2;
    int ox = (box - gw) / 2 + 2, oy = (box - gh) / 2;
    /* slate blue, lighter at the top with a soft sheen (era look) */
    const np_px top = NP_RGB(0x6c, 0x7e, 0x9c), bot = NP_RGB(0x3c, 0x4a, 0x66);
    const np_px ink = NP_RGB(0xd6, 0xda, 0xe2);
    int x, y;

    for (y = 0; y < box; y++)
    {
        np_px c = np_blend256(top, bot, (unsigned)(y * 256 / box));
        np_px *row = art + y * box;
        int gy = y - oy;

        if (y < box * 2 / 5)        /* sheen on the top two fifths */
            c = np_blend256(c, NP_WHITE, (unsigned)(28 - y * 28 * 5
                                                     / (box * 2)));
        for (x = 0; x < box; x++)
            row[x] = c;
        if (gy < 0 || gy >= gh)
            continue;
        for (x = 0; x < gw; x++)
        {
            /* bilinear from the 4-bit map, sample (x + 0.5) / 2.5 - 0.5,
             * in 1/16 px */
            int sxf = ((2 * x + 1) * 16) / 5 - 8;
            int syf = ((2 * gy + 1) * 16) / 5 - 8;
            int sx = sxf >> 4, sy = syf >> 4;
            int fx = sxf & 15, fy = syf & 15;
            int i, j, a, acc = 0;

            for (j = 0; j < 2; j++)
                for (i = 0; i < 2; i++)
                {
                    int px = sx + i, py = sy + j, d = 15;   /* outside */
                    int wx = i ? fx : 16 - fx, wy = j ? fy : 16 - fy;

                    if (px >= 0 && px < g->w && py >= 0 && py < g->h)
                    {
                        unsigned char b = g->cov[py * rb + (px >> 1)];
                        d = (px & 1) ? (b & 15) : (b >> 4);
                    }
                    acc += d * wx * wy;
                }
            /* acc / 256 = 7.5 + 4 * distance (source px, < 0 inside);
             * on screen the distance is 5/2 of that, and the coverage
             * 1/2 - distance: 128 - (acc - 1920) * 5 / 8, in 1/256 */
            a = 128 - (acc * 5 - 9600) / 8;
            if (a > 256)
                a = 256;
            if (a > 0)
                row[ox + x] = blend32(row[ox + x], ink, (unsigned)a >> 3);
        }
    }
}

/* ---- glyphs --------------------------------------------------------------- */

void np_glyph_size(enum np_glyph_id id, int *w, int *h)
{
    *w = np_glyphs[id].w;
    *h = np_glyphs[id].h;
}

unsigned np_glyph_cov(enum np_glyph_id id, int x, int y)
{
    const struct np_glyph *g = &np_glyphs[id];
    unsigned char b;

    if (x < 0 || y < 0 || x >= g->w || y >= g->h)
        return 0;
    b = g->cov[y * ((g->w + 1) / 2) + (x >> 1)];
    return (x & 1) ? (b & 15u) : (b >> 4);
}

void np_glyph_draw(const struct np_canvas *cv, enum np_glyph_id id,
                   int x, int y, const np_px *rows, np_px fg, np_px edge)
{
    const struct np_glyph *g = &np_glyphs[id];
    int rb = (g->w + 1) / 2;
    int i, j;

    for (j = 0; j < g->h; j++)
    {
        int yy = y + j;
        np_px col = rows ? rows[j] : fg;
        np_px *row;

        if (yy < 0 || yy >= cv->h)
            continue;
        row = cv->px + yy * cv->stride;
        for (i = 0; i < g->w; i++)
        {
            int xx = x + i;
            unsigned char b = g->cov[j * rb + (i >> 1)];
            unsigned a = (i & 1) ? (b & 15u) : (b >> 4);
            np_px c = col;

            if (!a || xx < 0 || xx >= cv->w)
                continue;
            if (g->inner)
            {
                unsigned char ib = g->inner[j * rb + (i >> 1)];
                unsigned in = (i & 1) ? (ib & 15u) : (ib >> 4);
                if (in < 15)
                    c = blend32(edge, col, (in << 1) + (in >> 3));
            }
            row[xx] = a >= 15 ? c : blend32(row[xx], c, (a << 1) + (a >> 3));
        }
    }
}

/* ---- bars ----------------------------------------------------------------- */

void np_fill(const struct np_canvas *cv, int x, int y, int w, int h, np_px c)
{
    int i, j;

    if (x < 0)
    {
        w += x;
        x = 0;
    }
    if (y < 0)
    {
        h += y;
        y = 0;
    }
    if (x + w > cv->w)
        w = cv->w - x;
    if (y + h > cv->h)
        h = cv->h - y;
    for (j = 0; j < h; j++)
    {
        np_px *row = cv->px + (y + j) * cv->stride + x;
        for (i = 0; i < w; i++)
            row[i] = c;
    }
}

void np_bar_cols(const struct np_canvas *cv, int x0, int y, int w,
                 int from, int to, int fill_px, const uint8_t *ticks,
                 const struct np_bar_style *st)
{
    int c, r;

    if (from < 0)
        from = 0;
    if (to > w)
        to = w;
    for (c = from; c < to; c++)
    {
        np_px *dst = cv->px + y * cv->stride + x0 + c;
        bool filled = c < fill_px;
        bool tick = ticks && (ticks[c >> 3] & (1u << (c & 7)));
        const np_px *rows = filled ? (c == 0 ? st->fill_end : st->fill)
                                   : st->track;
        const np_px *refl = filled ? st->fill_refl : st->track_refl;

        if (x0 + c < 0 || x0 + c >= cv->w)
            continue;
        if (!filled && (c == 0 || c == w - 1))
            rows = st->side;            /* the groove's square ends */
        if (tick && rows != st->side)
        {
            /* a short, subtle chapter tick inside the track (photo 1221):
             * rows NP_TICK_TOP .. NP_BAR_H - 1 - NP_TICK_TOP */
            np_px t = filled ? st->tick_fill : st->tick;
            for (r = 0; r < NP_BAR_H; r++, dst += cv->stride)
                *dst = (r >= NP_TICK_TOP && r < NP_BAR_H - NP_TICK_TOP)
                       ? t : rows[r];
        }
        else
        {
            for (r = 0; r < NP_BAR_H; r++, dst += cv->stride)
                *dst = rows[r];
        }
        for (r = 0; r < NP_BAR_REFL; r++, dst += cv->stride)
            *dst = refl[r];
    }
}

void np_blend_rect(const struct np_canvas *cv, int x, int y, int w, int h,
                   np_px c, unsigned a)
{
    int i, j;

    if (x < 0)
    {
        w += x;
        x = 0;
    }
    if (y < 0)
    {
        h += y;
        y = 0;
    }
    if (x + w > cv->w)
        w = cv->w - x;
    if (y + h > cv->h)
        h = cv->h - y;
    for (j = 0; j < h; j++)
    {
        np_px *row = cv->px + (y + j) * cv->stride + x;
        for (i = 0; i < w; i++)
            row[i] = np_blend256(row[i], c, a);
    }
}

/* Coverage (0..256) of corner pixel (i, j) of a radius-r quarter disc
 * centred at (r, r): 4 x 4 samples, integer maths */
static unsigned corner_cov(int i, int j, int r)
{
    int sx, sy, n = 0, rr = r * 8 * r * 8;

    for (sy = 0; sy < 4; sy++)
        for (sx = 0; sx < 4; sx++)
        {
            int dx = r * 8 - (i * 8 + sx * 2 + 1);
            int dy = r * 8 - (j * 8 + sy * 2 + 1);
            if (dx < 0)
                dx = 0;
            if (dy < 0)
                dy = 0;
            if (dx * dx + dy * dy <= rr)
                n++;
        }
    return (unsigned)n * 16;
}

void np_rrect(const struct np_canvas *cv, int x, int y, int w, int h, int r,
              const np_px *rows)
{
    int i, j;

    if (r > 6)
        r = 6;
    if (w < 2 * r || h < 2 * r || x < 0 || y < 0 || x + w > cv->w
        || y + h > cv->h)
        return;
    for (j = 0; j < h; j++)
    {
        np_px *row = cv->px + (y + j) * cv->stride + x;
        int cj = j < r ? j : (j >= h - r ? h - 1 - j : -1);

        for (i = 0; i < w; i++)
        {
            int ci = i < r ? i : (i >= w - r ? w - 1 - i : -1);
            unsigned a;

            if (cj < 0 || ci < 0)
            {
                row[i] = rows[j];
                continue;
            }
            a = corner_cov(ci, cj, r);
            row[i] = a >= 256 ? rows[j] : np_blend256(row[i], rows[j], a);
        }
    }
}
/* ---- labels --------------------------------------------------------------- */

static char *put_uint(char *p, unsigned v, int digits)
{
    char tmp[10];
    int n = 0;

    do
    {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && n < 10);
    while (n < digits)
        tmp[n++] = '0';
    while (n)
        *p++ = tmp[--n];
    return p;
}

int np_fmt_time(char *buf, unsigned long ms, bool neg)
{
    unsigned long s = ms / 1000;
    unsigned h = (unsigned)(s / 3600), m = (unsigned)(s / 60 % 60),
             sec = (unsigned)(s % 60);
    char *p = buf;

    if (neg)
        *p++ = '-';
    if (h)
    {
        p = put_uint(p, h, 1);
        *p++ = ':';
        p = put_uint(p, m, 2);
    }
    else
        p = put_uint(p, m, 1);
    *p++ = ':';
    p = put_uint(p, sec, 2);
    *p = '\0';
    return (int)(p - buf);
}

static int strlen_(const char *s)
{
    int n = 0;
    while (s[n])
        n++;
    return n;
}

static int atlas_index(char ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch == ':')
        return 10;
    if (ch == '-')
        return 11;
    return -1;
}

int np_atlas_width(const struct np_atlas *a, const char *s)
{
    int w = 0;

    for (; *s; s++)
    {
        int i = atlas_index(*s);
        if (i >= 0)
            w += a->w[i];
    }
    return w;
}

int np_atlas_draw(const struct np_canvas *cv, const struct np_atlas *a,
                  int x, int y, const char *s)
{
    for (; *s; s++)
    {
        int i = atlas_index(*s), r, n;

        if (i < 0)
            continue;
        n = a->w[i];
        if (x >= 0 && x + n <= cv->w)
        {
            for (r = 0; r < a->h && y + r < cv->h; r++)
            {
                const np_px *src = a->px + r * a->stride + a->x[i];
                np_px *dst = cv->px + (y + r) * cv->stride + x;
                int k;
                for (k = 0; k < n; k++)
                    dst[k] = src[k];
            }
        }
        x += n;
    }
    return x;
}

int np_atlas_draw_mul(const struct np_canvas *cv, const struct np_atlas *a,
                      int x, int y, const char *s)
{
    for (; *s; s++)
    {
        int i = atlas_index(*s), r, n;

        if (i < 0)
            continue;
        n = a->w[i];
        if (x >= 0 && x + n <= cv->w)
        {
            for (r = 0; r < a->h && y + r < cv->h; r++)
            {
                const np_px *src = a->px + r * a->stride + a->x[i];
                np_px *dst = cv->px + (y + r) * cv->stride + x;
                int k;
                for (k = 0; k < n; k++)
                {
                    unsigned p = src[k], q = dst[k];
                    if (p == NP_WHITE)
                        continue;
                    dst[k] = (np_px)(
                        ((((p >> 11) * (q >> 11) + 15) / 31) << 11)
                        | (((((p >> 5) & 63) * ((q >> 5) & 63) + 31) / 63)
                           << 5)
                        | (((p & 31) * (q & 31) + 15) / 31));
                }
            }
        }
        x += n;
    }
    return x;
}

int np_atlas_update(const struct np_canvas *cv, const struct np_atlas *a,
                    int x, int y, const char *old, const char *s)
{
    const char *o = old, *n = s;

    if (strlen_(old) == strlen_(s)
        && np_atlas_width(a, old) == np_atlas_width(a, s))
    {
        while (*n && *n == *o)
        {
            int i = atlas_index(*n);
            if (i >= 0)
                x += a->w[i];
            n++;
            o++;
        }
    }
    else
        n = s;
    return np_atlas_draw(cv, a, x, y, n);
}

void np_bar_span(int left_w, int right_w, int *x0, int *w)
{
    /* centred: the remaining-time slot mirrored on the left */
    int a = NP_TIME_MARGIN + right_w + NP_TIME_GAP;
    int b = NP_TIME_R_X - right_w - NP_TIME_GAP;

    (void)left_w;
    if (b - a > BAR_W_MAX)
    {
        /* never wider than the song layout: centre it in the space */
        a += (b - a - BAR_W_MAX) / 2;
        b = a + BAR_W_MAX;
    }
    if (b - a < 40)
        b = a + 40;
    *x0 = a;
    *w = b - a;
}
