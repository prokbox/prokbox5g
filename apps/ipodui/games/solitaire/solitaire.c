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

/* Solitaire: Klondike on the SDK (Class A, docs/design/sdk.md 13.1).
 *
 * Board: the stock and the waste top left, four foundations top right,
 * seven tableau piles below, on a green felt. Cards are drawn by us:
 * rounded white faces with the rank top left and the suit top right,
 * both inside the strip a stacked card still shows (so a column reads
 * rank and suit card by card), and one big suit in the middle; backs
 * in the shell's blue gel with a light inner frame.
 *
 * Wheel: two shelves (owner, 2026-10-25). The cursor slides along a
 * row of piles the way the finger goes: clockwise right, counter-
 * clockwise left. The top shelf is the stock, the waste and the four
 * foundations; the bottom shelf the seven tableau piles. Past the right
 * end of a shelf it carries on at the left end of the other, like
 * reading lines (and back the other way), so clockwise always means
 * right. The piles' numbers are already in that order (klondike.h).
 * SELECT (at release): on the stock, draw; on a card, pick it up (a
 * tableau pile's whole face-up run comes with it); with cards held, the
 * gold frame shows the part of the run that fits the pile under the
 * cursor (only one part ever can), SELECT drops it, on the same pile
 * puts them back. A long SELECT, or a double press (two releases
 * within 350 ms on the same pile, wheel still), sends the top card
 * under the cursor to its foundation. Short MENU: put held cards back,
 * else the pause menu (Resume, New Game, Undo, Settings, Exit); MENU
 * with the menu open leaves the game (the deal is kept). PLAY, NEXT and
 * PREV stay the music's. */
#include "ipodui/sdk/sdk.h"
#include "lang.h"                   /* the string table's language ids */
#include "klondike.h"

enum { SO_STR_NAME, SO_STR_DRAW, SO_STR_ONE_CARD, SO_STR_THREE_CARDS,
       SO_STR_UNDO, SO_STR_WON, SO_STR__N };
static const uint16_t so_lang[SO_STR__N] = {
    LANG_IPODUI_X_SOLITAIRE, LANG_IPODUI_X_DRAW, LANG_IPODUI_X_ONE_CARD,
    LANG_IPODUI_X_THREE_CARDS, LANG_IPODUI_X_UNDO, LANG_IPODUI_X_SO_WON,
};
static const struct sdk_strings so_strings = { SO_STR__N, so_lang };

enum { SO_SET_DRAW };
enum { SO_DRAW_ONE, SO_DRAW_THREE };
static const uint16_t so_draw_values[] = { SO_STR_ONE_CARD,
                                           SO_STR_THREE_CARDS };
static const struct sdk_setting so_set[] = {
    [SO_SET_DRAW] = { .label = SO_STR_DRAW, .kind = SDK_SET_CHOICE,
                      .n = 2, .def = SO_DRAW_THREE,     /* Q-SDK-3 */
                      .apply = SDK_APPLY_NEXT, .values = so_draw_values },
};
static const struct sdk_settings so_settings = { .version = 1, .n = 1,
                                                 .s = so_set };

/* Exit is the SDK's row: it runs the graceful exit itself */
static const uint16_t so_pause_rows[] = { SDK_STR_RESUME, SDK_STR_NEW_GAME,
                                          SO_STR_UNDO, SDK_STR_SETTINGS,
                                          SDK_STR_EXIT };
static const struct sdk_menu so_pause = { 5, so_pause_rows, 0 };
enum { PICK_RESUME, PICK_NEW, PICK_UNDO, PICK_SETTINGS };

/* -- geometry --------------------------------------------------------------- */
#define CW      40
#define CH      54
#define COLX(i) (5 + (i) * 45)
#define FAN     14              /* draw three: the waste's fan (a "10" shows) */
#define DOWN_DY 5               /* tableau: a face-down card's step */
#define DOWN_MIN 3              /* ... squeezed, in a long pile */
#define UP_DY   14              /* ... a face-up card's: the whole strip */
#define UP_MIN  5               /* ... squeezed (the middle of a long run) */
#define STRIP_Y 3               /* the rank and the suit: rows 3..12 */

/* -- colours ------------------------------------------------------------- */
#define C_FACE    SDK_RGB(0xff, 0xff, 0xfc)
#define C_EDGE    SDK_RGB(0x48, 0x4c, 0x54)
#define C_RED     SDK_RGB(0xd8, 0x00, 0x0c)   /* 5.2:1 on the face */
#define C_BLACK   SDK_RGB(0x08, 0x08, 0x0c)   /* 19:1 */
#define C_NOFIT   SDK_RGB(0x9a, 0xa4, 0xb0)   /* the cursor: no drop here */
#define C_SLOT    SDK_RGB(0x0a, 0x3c, 0x1c)
#define C_CURSOR  SDK_RGB(0x2e, 0x9c, 0xff)
#define C_HELD    SDK_RGB(0xff, 0xc8, 0x2a)
#define C_BACKLN  SDK_RGB(0xc8, 0xe0, 0xff)
#define C_KEY     SDK_RGB(0xff, 0x00, 0xff)
#define C_PANEL   SDK_RGB(0x0c, 0x30, 0x18)

/* -- our own glyph art (1 = ink) ------------------------------------------ */
/* Ranks: bold (2 px strokes), 10 rows, 8 px wide ("10" 11 px); MSB left */
#define RG_H   10
#define RG_WMAX 11
static const uint8_t rank_w[13] = { 8, 8, 8, 8, 8, 8, 8, 8, 8, 11, 8, 8, 8 };
static const uint16_t rank_art[13][RG_H] = {
    { 0x18, 0x3c, 0x66, 0x66, 0xc3, 0xff, 0xff, 0xc3, 0xc3, 0xc3 },  /* A */
    { 0x7e, 0xc3, 0x03, 0x06, 0x0c, 0x18, 0x30, 0x60, 0xff, 0xff },  /* 2 */
    { 0xfe, 0xff, 0x03, 0x03, 0x3e, 0x3e, 0x03, 0x03, 0xff, 0xfe },  /* 3 */
    { 0x0e, 0x1e, 0x36, 0x66, 0xc6, 0xff, 0xff, 0x06, 0x06, 0x06 },  /* 4 */
    { 0xff, 0xff, 0xc0, 0xfe, 0xff, 0x03, 0x03, 0xc3, 0xff, 0x7e },  /* 5 */
    { 0x7e, 0xff, 0xc0, 0xfe, 0xff, 0xc3, 0xc3, 0xc3, 0xff, 0x7e },  /* 6 */
    { 0xff, 0xff, 0x03, 0x06, 0x0c, 0x0c, 0x18, 0x18, 0x30, 0x30 },  /* 7 */
    { 0x7e, 0xff, 0xc3, 0xc3, 0x7e, 0x7e, 0xc3, 0xc3, 0xff, 0x7e },  /* 8 */
    { 0x7e, 0xff, 0xc3, 0xc3, 0xc3, 0xff, 0x7f, 0x03, 0xff, 0x7e },  /* 9 */
    { 0x33e, 0x77f, 0x363, 0x363, 0x363, 0x363, 0x363, 0x363, 0x37f,
      0x33e },                                                        /* 10 */
    { 0x1f, 0x1f, 0x06, 0x06, 0x06, 0x06, 0xc6, 0xc6, 0xfe, 0x7c },  /* J */
    { 0x7e, 0xff, 0xc3, 0xc3, 0xc3, 0xdb, 0xcf, 0xc6, 0xff, 0x7b },  /* Q */
    { 0xc3, 0xc6, 0xcc, 0xd8, 0xf0, 0xf0, 0xd8, 0xcc, 0xc6, 0xc3 },  /* K */
};
/* Corner suits, 9 x 9, told apart by shape as well as colour: the
 * spade pointed with a stem, the heart notched on top, the diamond
 * pointed both ends, the club three round lobes on a stem */
#define SS_W 9
static const uint16_t suit_small[4][SS_W] = {
    { 0x010, 0x038, 0x07c, 0x0fe, 0x1ff, 0x1ff, 0x0d6, 0x010, 0x07c },
    { 0x000, 0x0c6, 0x1ef, 0x1ff, 0x1ff, 0x0fe, 0x07c, 0x038, 0x010 },
    { 0x010, 0x038, 0x07c, 0x0fe, 0x1ff, 0x0fe, 0x07c, 0x038, 0x010 },
    { 0x038, 0x07c, 0x07c, 0x1bb, 0x1ff, 0x1ff, 0x1d7, 0x010, 0x07c },
};
#define SB_W 15                 /* big suits, 15 x 15 */
static const uint16_t suit_big[4][SB_W] = {
    { 0x0080, 0x01c0, 0x03e0, 0x07f0, 0x0ff8, 0x1ffc, 0x3ffe, 0x7fff,
      0x7fff, 0x7fff, 0x3ebe, 0x1c9c, 0x01c0, 0x03e0, 0x07f0 },
    { 0x1c1c, 0x3e3e, 0x7f7f, 0x7fff, 0x7fff, 0x7fff, 0x3ffe, 0x3ffe,
      0x1ffc, 0x0ff8, 0x07f0, 0x03e0, 0x01c0, 0x0080, 0x0000 },
    { 0x0080, 0x01c0, 0x03e0, 0x07f0, 0x0ff8, 0x1ffc, 0x3ffe, 0x7fff,
      0x3ffe, 0x1ffc, 0x0ff8, 0x07f0, 0x03e0, 0x01c0, 0x0080 },
    { 0x01c0, 0x03e0, 0x03e0, 0x03e0, 0x01c0, 0x1dde, 0x3fff, 0x7fff,
      0x3fff, 0x1dde, 0x0080, 0x01c0, 0x03e0, 0x07f0, 0x0000 },
};

/* -- the working block -------------------------------------------------- */
#define FELT_H (SDK_LCD_H)
struct so_work {
    sdk_px felt[FELT_H];                    /* the felt, a row table */
    sdk_px rank[2][13][RG_WMAX * RG_H];     /* black, red */
    sdk_px small[4][SS_W * SS_W];
    sdk_px big[4][SB_W * SB_W];
};

/* the game's statics: rebuilt at every init (.bss is zeroed per load) */
static struct kl_state *st;
static struct so_work *wk;
static int ax, ay, aw, ah;                  /* the play area */
static int8_t cursor;                       /* the pile (shelf order) */
static int8_t held;                         /* the pile held, or -1 */
static bool sel_long, finishing, dirty_all;
#define SO_DBL_MS 350
static uint32_t pick_ms;                    /* when the last pick-up came */
static int8_t pick_cur;                     /* ...and on which cursor, or -1 */
static int16_t prev_y0, prev_y1;            /* the old cursor's rows */

/* -- pre-rendering (once, at init) ---------------------------------------- */
static void glyph(sdk_px *dst, int w, int h, const uint16_t *bits,
                  sdk_px ink)
{
    int r, k;

    for (r = 0; r < h; r++)
        for (k = 0; k < w; k++)
            dst[r * w + k] = (bits[r] >> (w - 1 - k)) & 1 ? ink : C_KEY;
}

static void prerender(void)
{
    int i, s;

    for (i = 0; i < FELT_H; i++)
    {
        int g = 0x6c - i * 0x1c / FELT_H;
        wk->felt[i] = SDK_RGB(0x10, g, 0x30);
    }
    for (s = 0; s < 2; s++)
        for (i = 0; i < 13; i++)
            glyph(wk->rank[s][i], rank_w[i], RG_H, rank_art[i],
                  s ? C_RED : C_BLACK);
    for (i = 0; i < 4; i++)
    {
        sdk_px ink = i == 1 || i == 2 ? C_RED : C_BLACK;

        glyph(wk->small[i], SS_W, SS_W, suit_small[i], ink);
        glyph(wk->big[i], SB_W, SB_W, suit_big[i], ink);
    }
}

/* -- where things are ------------------------------------------------------ */
static int top_y(void) { return ay + 6; }
static int tab_y(void) { return ay + 6 + CH + 8; }

static int pile_x(int p)
{
    if (p == KL_STOCK) return COLX(0);
    if (p == KL_WASTE) return COLX(1);
    if (p < KL_TAB)    return COLX(3 + p - KL_FOUND);
    return COLX(p - KL_TAB);
}

/* Tableau pile p's steps, so that it fits the screen: every face-up
 * card shows its whole rank-and-suit strip (UP_DY) when there is room.
 * A long pile first squeezes its face-down cards, then the face-up
 * cards just after the run's first one (it stays whole: the run's top
 * rank) down to UP_MIN, keeping the newest cards whole; only when even
 * that is not enough is the whole run squeezed evenly. *nsq: how many
 * steps after the first are squeezed, to *sq px. */
static void tab_steps(int p, int *ddy, int *nsq, int *sq, int *udy)
{
    int nd = kl_first_up(st, p), nu = kl_len(st, p) - nd;
    int room = SDK_LCD_H - 2 - tab_y() - CH, gaps = nu > 1 ? nu - 1 : 0;

    *ddy = DOWN_DY;
    *nsq = 0;
    *sq = *udy = UP_DY;
    if (nd * DOWN_DY + gaps * UP_DY <= room)
        return;
    *ddy = DOWN_MIN;
    room -= nd * DOWN_MIN;
    if (gaps * UP_DY <= room)
        return;
    if (gaps >= 2)
    {
        /* squeeze steps 1..k (k <= gaps - 1) to UP_MIN */
        int over = gaps * UP_DY - room, k = (over + UP_DY - UP_MIN - 1)
                                            / (UP_DY - UP_MIN);

        if (k <= gaps - 1)
        {
            *nsq = k;
            *sq = UP_MIN;
            return;
        }
    }
    *udy = gaps ? room / gaps : UP_DY;
    if (*udy < 2)
        *udy = 2;
    *sq = *udy;
}

/* card i of pile p: its rect's top-left */
static void card_xy(int p, int i, int *x, int *y)
{
    *x = pile_x(p);
    *y = top_y();
    if (p >= KL_TAB)
    {
        int nd = kl_first_up(st, p), ddy, nsq, sq, udy, j;

        tab_steps(p, &ddy, &nsq, &sq, &udy);
        if (i <= nd)
        {
            *y = tab_y() + i * ddy;
            return;
        }
        *y = tab_y() + nd * ddy;
        for (j = 1; j <= i - nd; j++)       /* step j: card nd+j-1 -> nd+j */
            *y += j >= 2 && j <= nsq + 1 ? sq : udy;
    }
    else if (p == KL_WASTE && st->draw == 3)
    {
        int n = kl_len(st, p), first = n > 3 ? n - 3 : 0;

        *x += (i > first ? i - first : 0) * FAN;
    }
}

/* the rows a pile covers (for its band) */
static void pile_rows(int p, int *y0, int *y1)
{
    int x, y;

    if (p < KL_TAB)
    {
        *y0 = top_y() - 4;
        *y1 = top_y() + CH + 4;
        return;
    }
    card_xy(p, kl_len(st, p) ? kl_len(st, p) - 1 : 0, &x, &y);
    *y0 = tab_y() - 4;
    *y1 = y + CH + 4;
}

/* -- drawing --------------------------------------------------------------- */
static void outline(struct sdk_ctx *c, int x, int y, int w, int h,
                    sdk_px col, int t)
{
    sdk_fill(c, x, y, w, t, col);
    sdk_fill(c, x, y + h - t, w, t, col);
    sdk_fill(c, x, y, t, h, col);
    sdk_fill(c, x + w - t, y, t, h, col);
}

static void card_shape(struct sdk_ctx *c, int x, int y, sdk_px edge)
{
    /* rounded: the corner pixels stay felt (drawn before) */
    sdk_fill(c, x + 2, y, CW - 4, 1, edge);
    sdk_fill(c, x + 2, y + CH - 1, CW - 4, 1, edge);
    sdk_fill(c, x, y + 2, 1, CH - 4, edge);
    sdk_fill(c, x + CW - 1, y + 2, 1, CH - 4, edge);
    sdk_fill(c, x + 1, y + 1, 1, 1, edge);
    sdk_fill(c, x + CW - 2, y + 1, 1, 1, edge);
    sdk_fill(c, x + 1, y + CH - 2, 1, 1, edge);
    sdk_fill(c, x + CW - 2, y + CH - 2, 1, 1, edge);
}

static void draw_card(struct sdk_ctx *c, int card, int x, int y)
{
    if (!(card & KL_UP))
    {
        card_shape(c, x, y, C_EDGE);
        sdk_material(c, SDK_MAT_GEL_BLUE, x + 1, y + 1, CW - 2, CH - 2);
        outline(c, x + 4, y + 4, CW - 8, CH - 8, C_BACKLN, 1);
        sdk_material(c, SDK_MAT_GEL_SKY, x + CW / 2 - 6, y + CH / 2 - 6,
                     12, 12);
        return;
    }
    card_shape(c, x, y, C_EDGE);
    sdk_fill(c, x + 1, y + 1, CW - 2, CH - 2, C_FACE);
    {
        int r = KL_RANK(card) - 1, s = KL_SUIT(card), red = KL_RED(card);

        int w = rank_w[r];

        /* the strip: rank top left, suit top right (rows 3..12) */
        sdk_bitmap_key(c, wk->rank[red][r], 0, 0, w, x + 3, y + STRIP_Y,
                       w, RG_H, C_KEY);
        sdk_bitmap_key(c, wk->small[s], 0, 0, SS_W, x + CW - 3 - SS_W,
                       y + STRIP_Y + RG_H - SS_W, SS_W, SS_W, C_KEY);
        sdk_bitmap_key(c, wk->big[s], 0, 0, SB_W, x + (CW - SB_W) / 2,
                       y + 25, SB_W, SB_W, C_KEY);
    }
}

static void draw_slot(struct sdk_ctx *c, int x, int y)
{
    card_shape(c, x, y, C_SLOT);
    outline(c, x + 3, y + 3, CW - 6, CH - 6, C_SLOT, 1);
}

static void draw_pile(struct sdk_ctx *c, int p)
{
    int i, n = kl_len(st, p), x, y, first = 0;

    if (n == 0)
    {
        draw_slot(c, pile_x(p), p < KL_TAB ? top_y() : tab_y());
        return;
    }
    if (p == KL_STOCK || p == KL_FOUND || (p >= KL_FOUND && p < KL_TAB))
        first = n - 1;                      /* only the top shows */
    else if (p == KL_WASTE)
        first = st->draw == 3 && n > 3 ? n - 3 : (st->draw == 3 ? 0 : n - 1);
    for (i = first; i < n; i++)
    {
        card_xy(p, i, &x, &y);
        draw_card(c, st->card[p][i], x, y);
    }
}

/* how many cards SELECT picks up from pile p: a tableau pile's whole
 * face-up run, else the top card if it is face up (the stock: none) */
static int run_n(int p)
{
    int n = kl_len(st, p);

    if (p == KL_STOCK || !n || !(st->card[p][n - 1] & KL_UP))
        return 0;
    return p >= KL_TAB ? n - kl_first_up(st, p) : 1;
}

/* how many of the cards held from pile from go onto pile to: only one
 * count ever can (the ranks decide it), 0 if none */
static int fit_n(int from, int to)
{
    int k, n = run_n(from);

    for (k = 1; k <= n; k++)
        if (kl_can(st, from, k, to))
            return k;
    return 0;
}

/* the rect from card i of pile p to its top card (an empty pile: its
 * slot) */
static void pile_rect(int p, int i, int *x, int *y, int *w, int *h)
{
    int n = kl_len(st, p), xx, y1;

    *w = CW;
    if (!n)
    {
        *x = pile_x(p);
        *y = p < KL_TAB ? top_y() : tab_y();
        *h = CH;
        return;
    }
    if (i > n - 1)
        i = n - 1;
    card_xy(p, i, x, y);
    card_xy(p, n - 1, &xx, &y1);
    if (p == KL_WASTE)
        *x = xx;
    *h = y1 + CH - *y;
}

/* the most either frame can cover on pile p: its face-up run */
static void spot_max(int p, int *x, int *y, int *w, int *h)
{
    int r = run_n(p);

    pile_rect(p, kl_len(st, p) - (r ? r : 1), x, y, w, h);
}

/* The cursor's frame: with nothing held, what SELECT would pick up (a
 * tableau pile's face-up run, else the top card); with cards held, the
 * drop spot, blue where part of the run fits, grey where none does.
 * The held run's gold frame: the part that would go to the pile under
 * the cursor, or the whole run when none fits. */
static void draw_frames(struct sdk_ctx *c)
{
    int x, y, w, h, n = kl_len(st, cursor);
    sdk_px col = C_CURSOR;

    if (held < 0)
    {
        int r = run_n(cursor);

        pile_rect(cursor, n - (r ? r : 1), &x, &y, &w, &h);
    }
    else
    {
        pile_rect(cursor, n - 1, &x, &y, &w, &h);
        if (cursor != held && !fit_n(held, cursor))
            col = C_NOFIT;
    }
    outline(c, x - 2, y - 2, w + 4, h + 4, col, 2);
    prev_y0 = (int16_t)(y - 2);
    prev_y1 = (int16_t)(y + h + 2);
    if (held >= 0)
    {
        int k = cursor != held ? fit_n(held, cursor) : 0;

        if (!k)
            k = run_n(held);
        pile_rect(held, kl_len(st, held) - k, &x, &y, &w, &h);
        outline(c, x - 4, y - 4, w + 8, h + 8, C_HELD, 2);
    }
}

static void draw_won(struct sdk_ctx *c)
{
    int w = sdk_text_width(c, SO_STR_WON, SDK_FONT_LIST) + 40;
    int x = (SDK_LCD_W - w) / 2, y = tab_y() + 40;

    if (w > SDK_LCD_W - 20)
        w = SDK_LCD_W - 20, x = 10;
    sdk_fill(c, x, y, w, 36, C_PANEL);
    outline(c, x, y, w, 36, C_HELD, 1);
    sdk_text_fmt(c, x + 20, y + 10, SO_STR_WON, st->moves, SDK_FONT_LIST,
                 C_FACE);
}

/* rows [y0, y1): the felt, then every pile that crosses them, the
 * frames, and the band marked dirty */
static void redraw_rows(struct sdk_ctx *c, int y0, int y1)
{
    int p, a, b;

    if (y0 < ay)
        y0 = ay;
    if (y1 > SDK_LCD_H)
        y1 = SDK_LCD_H;
    if (y1 <= y0)
        return;
    sdk_rows(c, 0, y0, SDK_LCD_W, y1 - y0, &wk->felt[y0]);
    for (p = 0; p < KL_PILES; p++)
    {
        pile_rows(p, &a, &b);
        if (a < y1 && b > y0)
            draw_pile(c, p);
    }
    draw_frames(c);
    if (st->won && !finishing)
        draw_won(c);
    sdk_dirty(c, y0, y1 - y0);
}

static void redraw_all(struct sdk_ctx *c)
{
    redraw_rows(c, ay, SDK_LCD_H);
}

/* a change to piles a and b (-1: none) and the cursor */
static void redraw_piles(struct sdk_ctx *c, int a, int b, int a0, int a1)
{
    int y0 = prev_y0, y1 = prev_y1, t0, t1, x, y, w, h;

    if (a0 < y0) y0 = a0;
    if (a1 > y1) y1 = a1;
    if (a >= 0) { pile_rows(a, &t0, &t1); if (t0 < y0) y0 = t0;
                  if (t1 > y1) y1 = t1; }
    if (b >= 0) { pile_rows(b, &t0, &t1); if (t0 < y0) y0 = t0;
                  if (t1 > y1) y1 = t1; }
    spot_max(cursor, &x, &y, &w, &h);
    if (y - 2 < y0) y0 = y - 2;
    if (y + h + 2 > y1) y1 = y + h + 2;
    if (held >= 0)
    {
        spot_max(held, &x, &y, &w, &h);
        if (y - 4 < y0) y0 = y - 4;
        if (y + h + 4 > y1) y1 = y + h + 4;
    }
    redraw_rows(c, y0, y1);
}

/* -- play ------------------------------------------------------------------ */
static void commit(struct sdk_ctx *c)
{
    sdk_state_commit(c, sizeof(*st));
}

static void new_deal(struct sdk_ctx *c)
{
    int draw = sdk_setting(c, SO_SET_DRAW) == SO_DRAW_ONE ? 1 : 3;

    kl_deal(st, sdk_rand32(c), draw);
    held = -1;
    cursor = 0;
    pick_cur = -1;
    finishing = false;
    commit(c);
    sdk_log(c, "deal draw=%d", draw);
}

static void pile_band(int p, int *y0, int *y1)
{
    pile_rows(p, y0, y1);
}

static void act_select(struct sdk_ctx *c)
{
    int p = cursor, a0, a1, k;

    if (st->won)
    {
        new_deal(c);
        dirty_all = true;
        return;
    }
    if (held < 0)
    {
        if (p == KL_STOCK)
        {
            pile_band(KL_WASTE, &a0, &a1);
            if (kl_draw(st) == 0)
            {
                sdk_click(c);
                commit(c);
                redraw_piles(c, KL_STOCK, KL_WASTE, a0, a1);
            }
            return;
        }
        if (kl_top(st, p) >= 0 && (kl_top(st, p) & KL_UP))
        {
            held = (int8_t)p;
            pick_cur = cursor;
            redraw_piles(c, p, -1, prev_y0, prev_y1);
        }
        return;
    }
    if (held == p)
    {
        held = -1;                          /* put back */
        redraw_piles(c, p, -1, prev_y0, prev_y1);
        return;
    }
    /* the part of the held run that fits here (the gold frame) */
    k = fit_n(held, p);
    pile_band(held, &a0, &a1);
    {
        int from = held;

        held = -1;
        if (k && kl_move(st, from, k, p) == 0)
        {
            sdk_click(c);
            commit(c);
        }
        redraw_piles(c, from, p, a0, a1);
    }
    if (kl_solvable_now(st))
        finishing = true;
}

static void send_home(struct sdk_ctx *c, int p)
{
    int f, a0, a1;

    pile_band(p, &a0, &a1);
    f = kl_send_home(st, p);
    if (f < 0)
        return;
    sdk_click(c);
    commit(c);
    redraw_piles(c, p, f, a0, a1);
    if (kl_solvable_now(st))
        finishing = true;
}

static void act_long(struct sdk_ctx *c)
{
    if (held >= 0 || st->won)
        return;
    send_home(c, cursor);
}

/* along the shelves: the piles in reading order, wrapping (klondike.h
 * numbers them that way: the stock, the waste, the foundations, then
 * the tableau left to right) */
static void move_cursor(struct sdk_ctx *c, int steps)
{
    cursor = (int8_t)(((cursor + steps) % KL_PILES + KL_PILES) % KL_PILES);
    redraw_piles(c, -1, -1, prev_y0, prev_y1);
}

/* -- the SDK's entry points ----------------------------------------------- */
static int so_init(struct sdk_ctx *c)
{
    uint16_t len;

    sdk_area(c, &ax, &ay, &aw, &ah);
    wk = sdk_work(c, sizeof(*wk));
    st = sdk_state(c, &len);
    if (!wk || !st)
        return SDK_E_NOMEM;
    prerender();
    held = -1;
    cursor = 0;
    pick_cur = -1;
    sel_long = finishing = dirty_all = false;
    prev_y0 = prev_y1 = (int16_t)ay;
    if (len != sizeof(*st) || !kl_valid(st))
        new_deal(c);                        /* nothing saved: deal */
    else
    {
        finishing = kl_solvable_now(st);
        sdk_log(c, "resume moves=%u draw=%u", st->moves, st->draw);
    }
    return SDK_OK;
}

static int so_step(struct sdk_ctx *c, const struct sdk_input *in,
                   uint32_t now_ms)
{
    int i;

    switch (in->menu_pick)
    {
        case PICK_NEW:
            new_deal(c);
            dirty_all = true;
            break;
        case PICK_UNDO:
            if (kl_undo(st))
                commit(c);
            held = -1;
            finishing = false;
            dirty_all = true;
            break;
        case PICK_SETTINGS:
            sdk_settings_open(c);           /* Draw applies next deal */
            break;
        default:
            break;
    }
    if (in->steps)
        pick_cur = -1;                      /* the wheel moved: no double */
    if (in->steps && !finishing)
        move_cursor(c, in->steps);
    for (i = 0; i < in->n && !finishing; i++)
    {
        const struct sdk_btn_ev *e = &in->ev[i];

        if (e->btn != SDK_BTN_SELECT)
            continue;
        switch (e->kind)
        {
            case SDK_IN_PRESS:
                sel_long = false;
                break;
            case SDK_IN_LONG:
                sel_long = true;
                act_long(c);
                break;
            case SDK_IN_RELEASE:
                if (!sel_long && pick_cur == cursor && held == cursor
                    && now_ms - pick_ms <= SO_DBL_MS)
                {
                    /* second press: cancel the pick-up, send it home */
                    pick_cur = -1;
                    held = -1;
                    redraw_piles(c, cursor, -1, prev_y0, prev_y1);
                    send_home(c, cursor);
                }
                else if (!sel_long)
                {
                    act_select(c);
                    if (pick_cur == cursor && held >= 0)
                        pick_ms = now_ms;
                    else
                        pick_cur = -1;
                }
                sel_long = false;
                break;
            case SDK_IN_CANCEL:
                sel_long = true;            /* nothing acted yet: drop */
                break;
            default:
                break;
        }
    }
    if (finishing)
    {
        /* the auto-finish: one card a frame up to the foundations */
        int p = kl_finish_step(st), a0, a1;

        if (p >= 0)
        {
            pile_band(p, &a0, &a1);
            commit(c);
            redraw_piles(c, p, KL_FOUND, ay, ay + CH + 8);
            sdk_click(c);
        }
        if (p < 0 || st->won)
        {
            finishing = false;
            dirty_all = true;
            sdk_log(c, "won moves=%u", st->moves);
        }
    }
    if (dirty_all)
    {
        dirty_all = false;
        redraw_all(c);
    }
    return finishing ? SDK_FRAME_MIN_MS : SDK_IDLE;
}

static bool so_back(struct sdk_ctx *c)
{
    if (sdk_menu_is_open(c))
        return false;                       /* leave; the deal is kept */
    if (held >= 0)
    {
        int p = held;

        held = -1;
        redraw_piles(c, p, -1, prev_y0, prev_y1);
        return true;
    }
    sdk_menu_open(c, &so_pause);
    return true;
}

static void so_suspend(struct sdk_ctx *c, enum sdk_why why)
{
    (void)why;
    commit(c);
}

static void so_resume(struct sdk_ctx *c)
{
    redraw_all(c);
}

static void so_exit(struct sdk_ctx *c, enum sdk_why why)
{
    (void)why;
    held = -1;
    commit(c);
}

SDK_DESC_A(solitaire, 12288, 640, 64, MOD_EXIT_GRACEFUL, 200);
SDK_GAME(solitaire, so_strings) = {
    .init = so_init, .step = so_step, .back = so_back,
    .suspend = so_suspend, .resume = so_resume, .exit = so_exit,
    .settings = &so_settings,
};
