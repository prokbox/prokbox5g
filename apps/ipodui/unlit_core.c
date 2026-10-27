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
 * iPod UI shell: the unlit clock face, pure core (#51, Phase 8a U2;
 * docs/phase8a-brief.md §3.5, Revision 1 M1, M2; docs/design/unlit.md).
 *
 * No Rockbox calls: the state machine and the painting (through a fill
 * callback) are host-tested by tests/unlit, which also requires the
 * painted pixels to match the art generator's PNGs exactly.
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
#include <stdint.h>
#include "kernel.h"                 /* HZ */
#include <string.h>
#include "ipodui/unlit_core.h"

struct unlit_glyph {
    unsigned char w, h;
    short first;                /* unlit_runs index of the outer shape */
    short ink;                  /* of the ink, -1: the shape's erosion */
};
#include "ipodui/unlit_glyphs.h"

typedef uint64_t umask;         /* one glyph row, bit x = pixel x (w <= 64) */

/* -- state machine (unlit.h; Revision 1 M1, M2) ------------------------- */

void unlit_state_init(struct ui_unlit_state *s, long now)
{
    memset(s, 0, sizeof(*s));
    s->shown_min = -1;
    s->swallow_since = now - 2 * UNLIT_SWALLOW_MAX; /* no quiet window */
}

/* leaves FACE; 'swallow': drop keys until the dismissing release */
static void leave_face(struct ui_unlit_state *s, long now, bool swallow)
{
    s->face = false;
    s->fb_valid = false;
    s->shown_min = -1;
    s->swallowing = swallow;
    s->swallow_since = now;
}

enum unlit_rel unlit_release_route(const struct ui_unlit_state *s,
                                   unsigned long bits,
                                   unsigned long dismiss_bits)
{
    if (!bits)
        return UNLIT_REL_PASS;
    if (s->swallowing && !(bits & dismiss_bits))
        return UNLIT_REL_SWALLOW;
    return UNLIT_REL_STEP;
}

unsigned ui_unlit_step(struct ui_unlit_state *s, enum ui_unlit_ev ev,
                       long now, bool lit)
{
    switch (ev)
    {
        case UE_SLEEP_NEXT:
            s->skip_next = true;
            s->skip_stamp = now;
            return UA_NONE;

        case UE_BL_OFF:
            if (s->skip_next)
            {
                bool fresh = now - s->skip_stamp <= UNLIT_SKIP_TICKS;
                s->skip_next = false;
                if (fresh)
                    return UA_SKIP;
            }
            if (s->face || lit)         /* M1: lit again by then: no face */
                return UA_NONE;
            s->face = true;
            s->swallowing = false;
            s->fb_valid = false;
            s->shown_min = -1;
            return UA_DRAW_FULL;

        case UE_BL_ON_NOKEY:            /* Hold on, charger: stays, lit */
            return UA_NONE;

        case UE_KEY:                    /* a press or a repeat */
            if (s->face)
            {
                leave_face(s, now, true);
                return UA_DISMISS | UA_SWALLOW;
            }
            if (s->swallowing)
            {
                if (now - s->swallow_since >= UNLIT_SWALLOW_MAX)
                {
                    s->swallowing = false;
                    return UA_NONE;
                }
                return UA_SWALLOW;
            }
            return UA_NONE;

        case UE_KEY_RELEASE:
            if (s->face)                /* a lone release also dismisses */
            {
                leave_face(s, now, false);
                return UA_DISMISS | UA_SWALLOW;
            }
            if (s->swallowing)          /* the dismissing key's release */
            {
                s->swallowing = false;
                return UA_SWALLOW;
            }
            return UA_NONE;

        case UE_WHEEL:
            if (s->face)                /* no release follows a wheel */
            {
                leave_face(s, now, false);
                return UA_DISMISS | UA_SWALLOW;
            }
            if (s->swallowing
                || now - s->swallow_since < UNLIT_WHEEL_SWALLOW)
                return UA_SWALLOW;
            return UA_NONE;

        case UE_BUTTONS_UP:
            s->swallowing = false;
            return UA_NONE;

        case UE_TICK:
            if (s->swallowing && now - s->swallow_since >= UNLIT_SWALLOW_MAX)
                s->swallowing = false;
            if (s->skip_next && now - s->skip_stamp > UNLIT_SKIP_TICKS)
                s->skip_next = false;
            return UA_NONE;

        case UE_HOLD_ON:                /* #65: the lock brings the face up
                                         * at once, lit or dark */
            if (!s->face)
            {
                s->face = true;
                s->swallowing = false;
                s->fb_valid = false;
                s->shown_min = -1;
                return UA_DRAW_FULL;
            }
            /* already up: the padlock appears */
            /* fall through */
        case UE_STATUS:
            if (!s->face)
                return UA_NONE;
            return s->fb_valid ? UA_DRAW_STATUS : UA_DRAW_FULL;

        case UE_MINUTE:
            if (!s->face)
                return UA_NONE;
            return s->fb_valid ? UA_DRAW_DIGITS : UA_DRAW_FULL;

        case UE_HOLD_OFF:
        case UE_USB:
        case UE_DEVMODE:
            if (!s->face)
                return UA_NONE;
            leave_face(s, now, false);
            return UA_DISMISS;
    }
    return UA_NONE;
}

int ui_unlit_ticks_to_minute(int sec_of_minute)
{
    int t = (60 - sec_of_minute) * HZ;
    return t > 0 ? t : 1;
}

/* -- painting ----------------------------------------------------------- */

/* stock metrics (art/unlit/unlit.py M['stock']) */
#define CELL_GAP    7           /* digit to digit */
#define COLON_GAP   5           /* either side of the colon */
#define ROW_GAP     38          /* after play / pause */
#define LOCK_GAP    19          /* after the padlock */
#define GLYPH_H     36          /* status row glyph height */
#define PAUSE_BAR   12
#define PAUSE_W     35
#define BAT_OUT_W   (UNLIT_BAT_W + 4)   /* with the nub */

/* Runs of set bits of 'm' as rects of height h. */
static void spans(umask m, int x0, int y, int h, int g,
                  unlit_fill_fn *fill, void *ctx)
{
    int x = 0;

    while (m)
    {
        int n = 0;
        while (!(m & 1))
        {
            m >>= 1;
            x++;
        }
        while (m & 1)
        {
            m >>= 1;
            n++;
        }
        fill(ctx, x0 + x, y, n, h, g);
        x += n;
    }
}

/* Row sources, read top to bottom one row per call: a glyph's run-length
 * table (unlit_glyphs.h) or a small array. Rows are expanded as they are
 * painted, so no glyph-height array sits on the stack (fix round: two
 * 68-row umask arrays made paint_table() a 1,104 B frame). */
typedef umask row_fn(void *src);

struct run_rd { int r, n; umask m; };

static umask run_next(void *p)
{
    struct run_rd *rd = p;

    while (rd->n <= 0)
    {
        rd->n = unlit_runs[rd->r][0];
        rd->m = unlit_pal[unlit_runs[rd->r][1]];
        rd->r++;
    }
    rd->n--;
    return rd->m;
}

static umask arr_next(void *p)
{
    const umask **a = p;
    return *(*a)++;
}

/* Paints rows S (the outer shape) in the rim grey, then the ink: rows I,
 * or (I NULL) the 4-neighbour erosion of S, from a one-row look-ahead.
 * Equal consecutive rows merge into one rect per span (a digit is ~10
 * row groups). */
static void paint_mask(int x0, int y0, int w, int h, row_fn *S, void *sp,
                       row_fn *I, void *ip, unlit_fill_fn *fill, void *ctx)
{
    umask full = w >= 64 ? ~(umask)0 : ((umask)1 << w) - 1;
    umask ps = 0, pe = 0;
    umask up = 0, cur = h > 0 ? S(sp) : 0, dn = 0;
    int y, start = 0;

    for (y = 0; y <= h; y++)
    {
        umask s = 0, e = 0;

        if (y < h)
        {
            dn = y + 1 < h ? S(sp) : 0;
            s = cur;
            if (I)
                e = I(ip);
            else
                e = s & up & dn & ((s << 1) & full) & (s >> 1);
        }
        if (y == h || s != ps || e != pe)
        {
            if (y > start)
            {
                spans(ps, x0, y0 + start, y - start, UNLIT_RIM, fill, ctx);
                spans(pe, x0, y0 + start, y - start, UNLIT_INK, fill, ctx);
            }
            start = y;
            ps = s;
            pe = e;
        }
        up = cur;
        cur = dn;
    }
}

static void paint_table(int g, int x, int y, unlit_fill_fn *fill, void *ctx)
{
    const struct unlit_glyph *gl = &unlit_glyphs[g];
    struct run_rd S = { gl->first, 0, 0 }, I = { gl->ink, 0, 0 };

    paint_mask(x, y, gl->w, gl->h, run_next, &S,
               gl->ink >= 0 ? run_next : NULL, &I, fill, ctx);
}

static umask bits(int x0, int x1)      /* [x0, x1) */
{
    if (x1 <= x0)
        return 0;
    return (((umask)1 << (x1 - x0)) - 1) << x0;
}

/* The battery's ink: the 4-neighbour erosion of its shape, corrected
 * while charging by the generated unlit_bolt_rim[] pixels, where the art
 * erodes the analytic bolt and its 45-degree steps come out differently
 * (Phase 8b O2: match the approved mockup). */
enum { BAT_AIR = 2 };
struct bat_ink { const umask *s; int r, fill_px; bool charging; };

static umask bat_ink_next(void *p)
{
    struct bat_ink *k = p;
    const umask full = ((umask)1 << BAT_OUT_W) - 1;
    int r = k->r++, i;
    umask s = k->s[r];
    umask up = r > 0 ? k->s[r - 1] : 0;
    umask dn = r + 1 < UNLIT_BAT_H ? k->s[r + 1] : 0;
    umask e = s & up & dn & ((s << 1) & full) & (s >> 1);

    for (i = 0; k->charging && i < UNLIT_BOLT_RIMS; i++)
    {
        const unsigned char *b = unlit_bolt_rim[i];
        umask m = (umask)1 << (b[0] & 0x7f);

        if (b[1] != r || k->fill_px < b[2] || k->fill_px > b[3])
            continue;
        e = (b[0] & 0x80) ? e | m : e & ~m;
    }
    return e;
}

void unlit_paint_battery(int x, int y, int fill_px, bool charging,
                         unlit_fill_fn *fill, void *ctx)
{
    enum { W = UNLIT_BAT_W, H = UNLIT_BAT_H, O = UNLIT_BAT_O, AIR = BAT_AIR };
    umask S[UNLIT_BAT_H];
    int r;

    for (r = 0; r < H; r++)
    {
        umask m;
        if (r < O || r >= H - O)
        {
            int t = 0;
            if (r < UNLIT_BAT_TRIMS)
                t = unlit_bat_trim[r];
            else if (H - 1 - r < UNLIT_BAT_TRIMS)
                t = unlit_bat_trim[H - 1 - r];
            m = bits(t, W - t);
        }
        else
        {
            umask f = 0;
            if (r >= O + AIR && r < H - O - AIR)
                f = bits(O + AIR, O + AIR + fill_px);
            if (charging)
            {
                const unsigned char *b = unlit_bolt[r - O];
                f = (f & ~bits(b[2], b[3])) | (bits(b[0], b[1]) & ~f);
            }
            m = bits(0, O) | bits(W - O, W) | f;
        }
        if (r >= (H - UNLIT_BAT_NUB) / 2 && r < (H + UNLIT_BAT_NUB) / 2)
            m |= bits(W, W + 4);
        S[r] = m;
    }
    {
        const umask *rd = S;
        struct bat_ink ink = { S, 0, fill_px, charging };
        paint_mask(x, y, BAT_OUT_W, H, arr_next, &rd, bat_ink_next, &ink,
                   fill, ctx);
    }
}

int unlit_glyph_w(int id)
{
    switch (id)
    {
        case UNLIT_G_PAUSE: return PAUSE_W;
        case UNLIT_G_PLAY:  return unlit_glyphs[UG_PLAY].w;
        case UNLIT_G_LOCK:  return unlit_glyphs[UG_LOCK].w;
        case UNLIT_G_COLON: return unlit_glyphs[UG_COLON].w;
        default:            return unlit_glyphs[UG_D0 + id].w;
    }
}

void unlit_paint_glyph(int id, int x, int y, unlit_fill_fn *fill, void *ctx)
{
    switch (id)
    {
        case UNLIT_G_PAUSE:
        {
            int bx;
            for (bx = 0; bx < 2; bx++)
            {
                int x0 = x + bx * (PAUSE_W - PAUSE_BAR);
                fill(ctx, x0, y, PAUSE_BAR, GLYPH_H, UNLIT_RIM);
                fill(ctx, x0 + 1, y + 1, PAUSE_BAR - 2, GLYPH_H - 2, UNLIT_INK);
            }
            break;
        }
        case UNLIT_G_PLAY:  paint_table(UG_PLAY, x, y, fill, ctx); break;
        case UNLIT_G_LOCK:  paint_table(UG_LOCK, x, y, fill, ctx); break;
        case UNLIT_G_COLON: paint_table(UG_COLON, x, y, fill, ctx); break;
        default:            paint_table(UG_D0 + id, x, y, fill, ctx); break;
    }
}

int unlit_fill_px(int percent)
{
    int num = UNLIT_BAT_FILL * percent;     /* fill = num / 100 */
    int q = num / 100, rem = num % 100;

    if (percent <= 0)
        return 0;
    if (percent >= 100)
        return UNLIT_BAT_FILL;
    if (rem > 50 || (rem == 50 && (q & 1)))  /* half to even, as Python */
        q++;
    return q > 0 ? q : 1;
}

void unlit_clock_layout(int minute, bool h12, struct unlit_clock *c)
{
    int h = minute / 60, m = minute % 60;
    int i, n = 0, width = 0, x;

    if (h12)
        h = h % 12 ? h % 12 : 12;
    if (h >= 10 || !h12)
        c->ch[n++] = '0' + h / 10;
    c->ch[n++] = '0' + h % 10;
    c->ch[n++] = ':';
    c->ch[n++] = '0' + m / 10;
    c->ch[n++] = '0' + m % 10;
    c->n = n;

    for (i = 0; i < n; i++)
    {
        width += c->ch[i] == ':' ? unlit_glyphs[UG_COLON].w
                                 : UNLIT_CELL_W;
        if (i + 1 < n)
            width += (c->ch[i] == ':' || c->ch[i + 1] == ':')
                     ? COLON_GAP : CELL_GAP;
    }
    x = (LCD_WIDTH - width) / 2;
    for (i = 0; i < n; i++)
    {
        c->x[i] = x;
        x += c->ch[i] == ':' ? unlit_glyphs[UG_COLON].w : UNLIT_CELL_W;
        if (i + 1 < n)
            x += (c->ch[i] == ':' || c->ch[i + 1] == ':')
                 ? COLON_GAP : CELL_GAP;
    }
}

static void paint_cell(const struct unlit_clock *c, int i,
                       unlit_fill_fn *fill, void *ctx)
{
    int id = c->ch[i] == ':' ? UNLIT_G_COLON : c->ch[i] - '0';
    unlit_paint_glyph(id, c->x[i], UNLIT_DIGIT_Y, fill, ctx);
}

int unlit_paint_clock(const struct unlit_clock *c, const struct unlit_clock *old,
                      unlit_fill_fn *fill, void *ctx)
{
    int i, painted = 0;
    bool same = old && old->n == c->n;

    for (i = 0; same && i < c->n; i++)
        same = old->x[i] == c->x[i];
    if (!same)
        fill(ctx, 0, UNLIT_DIGIT_Y, LCD_WIDTH, UNLIT_DIGIT_H, UNLIT_PAPER);
    for (i = 0; i < c->n; i++)
    {
        if (same && old->ch[i] == c->ch[i])
            continue;
        if (same)
            fill(ctx, c->x[i], UNLIT_DIGIT_Y, unlit_glyph_w(
                     c->ch[i] == ':' ? UNLIT_G_COLON : c->ch[i] - '0'),
                 UNLIT_DIGIT_H, UNLIT_PAPER);
        paint_cell(c, i, fill, ctx);
        painted++;
    }
    return painted;
}

void unlit_paint_status(const struct ui_unlit_status *st,
                        unlit_fill_fn *fill, void *ctx)
{
    int id[3], gap[3], n = 0, i, rw = 0, x;

    if (st->play == 1)
    {
        id[n] = UNLIT_G_PLAY;
        gap[n++] = ROW_GAP;
    }
    else if (st->play == 2)
    {
        id[n] = UNLIT_G_PAUSE;
        gap[n++] = ROW_GAP;
    }
    if (st->hold)
    {
        id[n] = UNLIT_G_LOCK;
        gap[n++] = LOCK_GAP;
    }
    for (i = 0; i < n; i++)
        rw += unlit_glyph_w(id[i]) + gap[i];
    rw += BAT_OUT_W;

    fill(ctx, 0, UNLIT_STATUS_Y, LCD_WIDTH, UNLIT_STATUS_H, UNLIT_PAPER);
    x = (LCD_WIDTH - rw) / 2;
    for (i = 0; i < n; i++)
    {
        unlit_paint_glyph(id[i], x, UNLIT_STATUS_Y, fill, ctx);
        x += unlit_glyph_w(id[i]) + gap[i];
    }
    unlit_paint_battery(x, UNLIT_STATUS_Y + (GLYPH_H - UNLIT_BAT_H) / 2,
                        st->fill_px, st->charging, fill, ctx);
}
