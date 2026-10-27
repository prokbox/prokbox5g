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

/* Quadrus: the falling-block game on the SDK (Class A, docs/design/sdk.md
 * 13.2). Our own name, palette and bevel; a 12 x 18 well of 11 px cells.
 *
 * Input (Class A owns SELECT, the relative wheel, short MENU through
 * back() and, opted in, the long MENU; owner, 2026-10-24): the wheel
 * moves the piece a column per step, clockwise to the right. A SELECT
 * press turns it clockwise at once; a second press within TAP_DBL_MS of
 * the first one's release takes that turn back and drops the piece hard;
 * SELECT held HOLD_SOFT_MS takes its press's turn back and soft-drops
 * until released. A MENU tap puts the piece in the hold slot (or swaps
 * it), once a piece. A long MENU opens the pause menu (Resume, New Game,
 * Settings, Exit); MENU with the menu open leaves (the game is kept).
 * PLAY, NEXT and PREV stay the music's. Re-entering a game in progress,
 * or coming back from a suspend, opens the pause menu first: nothing
 * falls while the player isn't looking.
 *
 * Drawing: the well is diffed cell by cell against what is on the
 * screen, and only the changed well rows are redrawn, as full-width
 * bands; a small row allowance (well under the 25 KB a frame average)
 * drops a frame rather than go over it. */
#include "ipodui/sdk/sdk.h"
#include "lang.h"
#include "quadrus_core.h"

enum { QS_NAME, QS_NEXT, QS_SCORE, QS_LEVEL, QS_LINES, QS_BEST, QS_OVER,
       QS_NEW_BEST, QS_START_LEVEL, QS_GHOST, QS_N1, QS_HOLD = QS_N1 + 10,
       QS__N };
static const uint16_t qs_lang[QS__N] = {
    LANG_IPODUI_X_QUADRUS, LANG_IPODUI_X_G_NEXT, LANG_IPODUI_X_G_SCORE,
    LANG_IPODUI_X_G_LEVEL, LANG_IPODUI_X_G_LINES, LANG_IPODUI_X_G_BEST,
    LANG_IPODUI_X_G_GAME_OVER, LANG_IPODUI_X_G_NEW_BEST,
    LANG_IPODUI_X_START_LEVEL, LANG_IPODUI_X_GHOST_PIECE,
    LANG_IPODUI_X_G_N1, LANG_IPODUI_X_G_N2, LANG_IPODUI_X_G_N3,
    LANG_IPODUI_X_G_N4, LANG_IPODUI_X_G_N5, LANG_IPODUI_X_G_N6,
    LANG_IPODUI_X_G_N7, LANG_IPODUI_X_G_N8, LANG_IPODUI_X_G_N9,
    LANG_IPODUI_X_G_N10, LANG_IPODUI_X_G_HOLD,
};
static const struct sdk_strings qs_strings = { QS__N, qs_lang };

enum { QS_SET_LEVEL, QS_SET_GHOST };
static const uint16_t qs_levels[10] = {
    QS_N1, QS_N1 + 1, QS_N1 + 2, QS_N1 + 3, QS_N1 + 4, QS_N1 + 5, QS_N1 + 6,
    QS_N1 + 7, QS_N1 + 8, QS_N1 + 9,
};
static const struct sdk_setting qs_set[] = {
    [QS_SET_LEVEL] = { .label = QS_START_LEVEL, .kind = SDK_SET_CHOICE,
                       .n = 10, .def = 0, .apply = SDK_APPLY_NEXT,
                       .values = qs_levels },
    [QS_SET_GHOST] = { .label = QS_GHOST, .kind = SDK_SET_TOGGLE,
                       .n = 2, .def = 1, .apply = SDK_APPLY_NOW },
};
static const struct sdk_settings qs_settings = { .version = 1, .n = 2,
                                                 .s = qs_set };

/* Exit is the SDK's row: it runs the graceful exit itself */
static const uint16_t qs_pause_rows[] = { SDK_STR_RESUME, SDK_STR_NEW_GAME,
                                          SDK_STR_SETTINGS, SDK_STR_EXIT };
static const struct sdk_menu qs_pause = { 4, qs_pause_rows, 0 };
enum { PICK_RESUME, PICK_NEW, PICK_SETTINGS };

/* SELECT's timing (owner, 2026-10-24): a second press this soon after a
 * tap's release is a double tap; a press held this long is the soft drop
 * (well before SDK_IN_LONG's 500 ms, and longer than a tap, ~100 ms) */
#define TAP_DBL_MS      300
#define HOLD_SOFT_MS    250

/* -- geometry ------------------------------------------------------------- */
#define CELL    11
#define WX      28                      /* the well's inside */
#define WY      31
#define WW      (QD_COLS * CELL)
#define WH      (QD_ROWS * CELL)
#define FRAME   3
#define PX      186                     /* the side panel */
#define NBX     (PX + 2)                /* the Next box's inside */
#define NBY     50
#define NBW     52
#define NBH     30
#define HBX     (NBX + NBW + 12)        /* the Hold box's inside */
#define VAL_W   (SDK_LCD_W - PX)
#define VAL_H   17

/* -- colours (ours) ------------------------------------------------------- */
#define C_WELL    SDK_RGB(0x10, 0x13, 0x1a)
#define C_GRID    SDK_RGB(0x1b, 0x20, 0x2a)
#define C_LABEL   SDK_RGB(0x8c, 0x96, 0xa6)
#define C_VALUE   SDK_RGB(0xf2, 0xf4, 0xf8)
#define C_PANEL   SDK_RGB(0x1c, 0x22, 0x2e)
#define C_PANLN   SDK_RGB(0x9a, 0xa4, 0xb4)
#define C_GOLD    SDK_RGB(0xff, 0xc8, 0x2a)

static const uint8_t piece_rgb[QD_PIECES][3] = {
    { 0xe0, 0x50, 0x48 },               /* the long one: coral */
    { 0x3a, 0x8c, 0xe8 },               /* the square: blue */
    { 0x2c, 0xb8, 0x8a },               /* teal */
    { 0xe8, 0xa8, 0x30 },               /* amber */
    { 0x8e, 0x6a, 0xd8 },               /* violet */
    { 0x9c, 0xc8, 0x3c },               /* lime */
    { 0xe0, 0x6c, 0xb0 },               /* rose */
};

/* tile codes: 0 empty, 1..7 a block, 8..14 a ghost, 15 a flashing row */
#define T_GHOST 8
#define T_FLASH 15
#define T_N     16
#define T_NONE  0xff

/* -- the working block ---------------------------------------------------- */
struct qg_work {
    sdk_px bg[SDK_LCD_H];               /* the backdrop, a row table */
    sdk_px tile[T_N][CELL * CELL];
    uint8_t drawn[QD_ROWS][QD_COLS];    /* the tile on the screen */
};

/* the state record */
#define QG_VER 1
#define QG_BEST_N 5
struct qg_save {
    uint8_t  ver, pad[3];
    uint32_t best[QG_BEST_N];
    struct qd_game g;
};

static struct qg_save *sv;
static struct qg_work *wk;
static int ay;                          /* the play area's top */
static uint32_t last_ms;
static int budget;                      /* rows we may push now */
static int8_t new_best;                 /* this game's place, or -1 */
static bool over_armed, pause_next, ghost_on, hold_req;
/* this SELECT press: down, became the soft drop, was a double tap */
static bool sel_down, sel_soft, sel_dbl, tap_ok;
static uint16_t sel_t0, tap_rel;        /* ms, low 16 bits (sdk_btn_ev) */
static uint32_t shown_score, shown_best;
static uint16_t shown_lines;
static uint8_t shown_level, shown_next, shown_hold;

/* -- pre-rendering (once, at init) ---------------------------------------- */
static sdk_px mix(int r, int g, int b, int t)
{
    /* t > 0: toward white by t/256; t < 0: toward black */
    if (t > 0)
    {
        r += (255 - r) * t >> 8;
        g += (255 - g) * t >> 8;
        b += (255 - b) * t >> 8;
    }
    else if (t < 0)
    {
        r = r * (256 + t) >> 8;
        g = g * (256 + t) >> 8;
        b = b * (256 + t) >> 8;
    }
    return SDK_RGB(r, g, b);
}

static void prerender(void)
{
    int i, p, x, y;

    for (i = 0; i < SDK_LCD_H; i++)
    {
        int k = i * 0x18 / SDK_LCD_H;

        wk->bg[i] = SDK_RGB(0x34 - k, 0x3a - k, 0x48 - k);
    }
    for (i = 0; i < T_N; i++)
        for (y = 0; y < CELL; y++)
            for (x = 0; x < CELL; x++)
            {
                sdk_px *d = &wk->tile[i][y * CELL + x];
                bool gap = x == CELL - 1 || y == CELL - 1;

                if (gap)
                {
                    *d = C_GRID;
                    continue;
                }
                if (i == 0)
                    *d = C_WELL;
                else if (i < T_GHOST)
                {
                    const uint8_t *c = piece_rgb[i - 1];
                    int t;

                    /* our bevel: a lit top-left edge, a shaded
                     * bottom-right one, and a gel sheen on the upper
                     * half */
                    if (y == 0 || x == 0)
                        t = 110;
                    else if (y == CELL - 2 || x == CELL - 2)
                        t = -90;
                    else if (y <= 4)
                        t = 70 - y * 12;
                    else
                        t = -(y - 4) * 8;
                    if (x >= 2 && x <= 3 && y >= 2 && y <= 3)
                        t = 170;    /* the glint */
                    *d = mix(c[0], c[1], c[2], t);
                }
                else if (i < T_FLASH)
                {
                    const uint8_t *c = piece_rgb[i - T_GHOST];
                    bool edge = x == 0 || y == 0 || x == CELL - 2
                                || y == CELL - 2;

                    *d = edge ? mix(c[0], c[1], c[2], -110) : C_WELL;
                }
                else
                    *d = (x == 0 || y == 0) ? SDK_RGB(0xff, 0xff, 0xff)
                                            : SDK_RGB(0xe4, 0xe8, 0xf0);
            }
    for (p = 0; p < QD_ROWS; p++)
        for (x = 0; x < QD_COLS; x++)
            wk->drawn[p][x] = T_NONE;
}

/* -- drawing -------------------------------------------------------------- */
static void backdrop(struct sdk_ctx *c, int x, int y, int w, int h)
{
    sdk_rows(c, x, y, w, h, &wk->bg[y]);
}

static void outline(struct sdk_ctx *c, int x, int y, int w, int h,
                    sdk_px col)
{
    sdk_fill(c, x, y, w, 1, col);
    sdk_fill(c, x, y + h - 1, w, 1, col);
    sdk_fill(c, x, y, 1, h, col);
    sdk_fill(c, x + w - 1, y, 1, h, col);
}

/* what tile cell (r, c) of the visible well should show */
static void compose(uint8_t want[QD_ROWS][QD_COLS])
{
    const struct qd_game *g = &sv->g;
    int8_t cl[4][2];
    int r, k, gy;

    for (r = 0; r < QD_ROWS; r++)
    {
        bool flash = g->phase == QD_PH_CLEAR
                     && (g->clear_rows >> (r + QD_HIDDEN)) & 1;

        for (k = 0; k < QD_COLS; k++)
            want[r][k] = flash ? T_FLASH : g->cell[r + QD_HIDDEN][k];
    }
    if (g->phase != QD_PH_FALL)
        return;
    qd_cells(g->cur, g->rot, cl);
    if (ghost_on)
    {
        gy = qd_ghost_y(g);
        for (k = 0; k < 4; k++)
        {
            int rr = gy + cl[k][1] - QD_HIDDEN, cc = g->px + cl[k][0];

            if (rr >= 0 && !want[rr][cc])
                want[rr][cc] = (uint8_t)(T_GHOST + g->cur);
        }
    }
    for (k = 0; k < 4; k++)
    {
        int rr = g->py + cl[k][1] - QD_HIDDEN, cc = g->px + cl[k][0];

        if (rr >= 0)
            want[rr][cc] = (uint8_t)(g->cur + 1);
    }
}

/* well rows r0..r1-1 from want[], every cell */
static void draw_well_rows(struct sdk_ctx *c, uint8_t want[QD_ROWS][QD_COLS],
                           int r0, int r1)
{
    int r, k;

    for (r = r0; r < r1; r++)
        for (k = 0; k < QD_COLS; k++)
        {
            sdk_bitmap(c, wk->tile[want[r][k]], 0, 0, CELL,
                       WX + k * CELL, WY + r * CELL, CELL, CELL);
            wk->drawn[r][k] = want[r][k];
        }
    sdk_dirty(c, WY + r0 * CELL, (r1 - r0) * CELL);
}

static bool row_changed(uint8_t want[QD_ROWS][QD_COLS], int r)
{
    int k;

    for (k = 0; k < QD_COLS; k++)
        if (wk->drawn[r][k] != want[r][k])
            return true;
    return false;
}

/* the changed rows, in runs; false if the allowance says wait */
static bool draw_well(struct sdk_ctx *c, bool force)
{
    uint8_t want[QD_ROWS][QD_COLS];
    int r, need = 0, r0 = -1;

    compose(want);
    for (r = 0; r < QD_ROWS; r++)
        if (row_changed(want, r))
            need += CELL;
    if (!need)
        return true;
    if (!force && need > budget)
        return false;                   /* drop this frame */
    budget -= need;
    for (r = 0; r <= QD_ROWS; r++)
    {
        bool ch = r < QD_ROWS && row_changed(want, r);

        if (ch && r0 < 0)
            r0 = r;
        else if (!ch && r0 >= 0)
        {
            draw_well_rows(c, want, r0, r);
            r0 = -1;
        }
    }
    return true;
}

static void draw_value(struct sdk_ctx *c, int y, uint32_t v)
{
    backdrop(c, PX, y, VAL_W, VAL_H);
    sdk_text_num(c, PX, y, (int32_t)v, SDK_FONT_LIST, C_VALUE);
    sdk_dirty(c, y, VAL_H);
}

#define Y_SCORE 94
#define Y_LEVEL 128
#define Y_LINES 162
#define Y_BEST  196

/* piece p (or none, p < 0) centred in the box at x */
static void draw_box(struct sdk_ctx *c, int bx, int p)
{
    int8_t cl[4][2];
    int k, minx = 9, maxx = 0, miny = 9, maxy = 0, ox, oy;

    sdk_fill(c, bx, NBY, NBW, NBH, C_WELL);
    if (p < 0)
    {
        sdk_dirty(c, NBY, NBH);
        return;
    }
    qd_cells(p, 0, cl);
    for (k = 0; k < 4; k++)
    {
        minx = cl[k][0] < minx ? cl[k][0] : minx;
        maxx = cl[k][0] > maxx ? cl[k][0] : maxx;
        miny = cl[k][1] < miny ? cl[k][1] : miny;
        maxy = cl[k][1] > maxy ? cl[k][1] : maxy;
    }
    /* centred; the tiles' own gap makes the right/bottom one px light */
    ox = bx + (NBW - (maxx - minx + 1) * CELL + 1) / 2 - minx * CELL;
    oy = NBY + (NBH - (maxy - miny + 1) * CELL + 1) / 2 - miny * CELL;
    for (k = 0; k < 4; k++)
        sdk_bitmap(c, wk->tile[p + 1], 0, 0, CELL, ox + cl[k][0] * CELL,
                   oy + cl[k][1] * CELL, CELL - 1, CELL - 1);
    sdk_dirty(c, NBY, NBH);
}

static void draw_stats(struct sdk_ctx *c, bool force)
{
    const struct qd_game *g = &sv->g;

    if (force || g->nxt != shown_next)
        draw_box(c, NBX, shown_next = g->nxt);
    if (force || g->hold != shown_hold)
        draw_box(c, HBX, (int)(shown_hold = g->hold) - 1);
    if (force || g->score != shown_score)
        draw_value(c, Y_SCORE + 14, shown_score = g->score);
    if (force || g->level != shown_level)
        draw_value(c, Y_LEVEL + 14, shown_level = g->level);
    if (force || g->lines != shown_lines)
        draw_value(c, Y_LINES + 14, shown_lines = g->lines);
    if (force || sv->best[0] != shown_best)
        draw_value(c, Y_BEST + 14, shown_best = sv->best[0]);
}

static int num_width(struct sdk_ctx *c, uint32_t v, enum sdk_font f)
{
    return sdk_num_width(c, (int32_t)v, f);
}

static void draw_over(struct sdk_ctx *c)
{
    int w = WW - 16, h = new_best >= 0 ? 70 : 54, x = WX + 8;
    int y = WY + (WH - h) / 2, tw;

    sdk_fill(c, x, y, w, h, C_PANEL);
    outline(c, x, y, w, h, C_PANLN);
    tw = sdk_text_width(c, QS_OVER, SDK_FONT_LIST);
    sdk_text(c, x + (w - tw) / 2, y + 8, QS_OVER, SDK_FONT_LIST, C_VALUE);
    tw = num_width(c, sv->g.score, SDK_FONT_LIST);
    sdk_text_num(c, x + (w - tw) / 2, y + 30, (int32_t)sv->g.score,
                 SDK_FONT_LIST, C_GOLD);
    if (new_best >= 0)
    {
        tw = sdk_text_width(c, QS_NEW_BEST, SDK_FONT_SMALL);
        sdk_text(c, x + (w - tw) / 2, y + 50, QS_NEW_BEST, SDK_FONT_SMALL,
                 C_LABEL);
    }
    sdk_dirty(c, y, h);
}

/* the whole scene: the backdrop, the frames, the labels, the well */
static void draw_all(struct sdk_ctx *c)
{
    int i;

    backdrop(c, 0, ay, SDK_LCD_W, SDK_LCD_H - ay);
    sdk_material(c, SDK_MAT_METAL, WX - FRAME, WY - FRAME, WW + 2 * FRAME,
                 FRAME);
    sdk_material(c, SDK_MAT_METAL, WX - FRAME, WY + WH, WW + 2 * FRAME,
                 FRAME);
    sdk_material(c, SDK_MAT_METAL, WX - FRAME, WY, FRAME, WH);
    sdk_material(c, SDK_MAT_METAL, WX + WW, WY, FRAME, WH);
    sdk_material(c, SDK_MAT_METAL, NBX - 2, NBY - 2, NBW + 4, NBH + 4);
    sdk_material(c, SDK_MAT_METAL, HBX - 2, NBY - 2, NBW + 4, NBH + 4);
    sdk_text(c, PX, WY - 2, QS_NEXT, SDK_FONT_SMALL, C_LABEL);
    sdk_text(c, HBX - 2, WY - 2, QS_HOLD, SDK_FONT_SMALL, C_LABEL);
    sdk_text(c, PX, Y_SCORE, QS_SCORE, SDK_FONT_SMALL, C_LABEL);
    sdk_text(c, PX, Y_LEVEL, QS_LEVEL, SDK_FONT_SMALL, C_LABEL);
    sdk_text(c, PX, Y_LINES, QS_LINES, SDK_FONT_SMALL, C_LABEL);
    sdk_text(c, PX, Y_BEST, QS_BEST, SDK_FONT_SMALL, C_LABEL);
    for (i = 0; i < QD_ROWS; i++)
        memset(wk->drawn[i], T_NONE, QD_COLS);
    draw_stats(c, true);
    draw_well(c, true);
    if (sv->g.phase == QD_PH_OVER)
        draw_over(c);
    sdk_dirty(c, ay, SDK_LCD_H - ay);
}

/* -- play ----------------------------------------------------------------- */
static void commit(struct sdk_ctx *c)
{
    sdk_state_commit(c, sizeof(*sv));
}

/* SELECT and MENU forgotten (a pause, a suspend, a new game): no soft
 * drop runs on under a menu */
static void input_reset(void)
{
    if (sv)
        qd_soft(&sv->g, false);
    sel_down = sel_soft = sel_dbl = tap_ok = hold_req = false;
}

static void pause_open(struct sdk_ctx *c)
{
    input_reset();
    sdk_menu_open(c, &qs_pause);
}

static void new_game(struct sdk_ctx *c)
{
    int lv = sdk_setting(c, QS_SET_LEVEL) + 1;

    qd_new(&sv->g, sdk_rand32(c), lv);
    new_best = -1;
    over_armed = false;
    input_reset();
    commit(c);
    sdk_log(c, "new level=%d", lv);
}

static void game_over(struct sdk_ctx *c)
{
    new_best = (int8_t)qd_best_insert(sv->best, QG_BEST_N, sv->g.score);
    commit(c);
    sdk_log(c, "over score=%u lines=%u best=%d", (unsigned)sv->g.score,
            sv->g.lines, new_best);
}

/* -- the SDK's entry points ----------------------------------------------- */
static int qg_init(struct sdk_ctx *c)
{
    int x, w, h;
    uint16_t len;

    sdk_area(c, &x, &ay, &w, &h);
    wk = sdk_work(c, sizeof(*wk));
    sv = sdk_state(c, &len);
    if (!wk || !sv)
        return SDK_E_NOMEM;
    prerender();
    last_ms = 0;
    budget = 0;
    new_best = -1;
    over_armed = pause_next = false;
    input_reset();
    shown_hold = 0xff;
    sdk_long_menu(c, SDK_LONGMENU_PAUSE);  /* long MENU pauses, not exits */
    ghost_on = sdk_setting(c, QS_SET_GHOST) != 0;
    if (len != sizeof(*sv) || sv->ver != QG_VER)
    {
        memset(sv, 0, sizeof(*sv));     /* nothing saved: a first game */
        sv->ver = QG_VER;
        new_game(c);
    }
    else if (!qd_valid(&sv->g))
        new_game(c);                    /* the best scores stay */
    else
    {
        pause_next = sv->g.phase != QD_PH_OVER;
        sdk_log(c, "resume score=%u level=%u", (unsigned)sv->g.score,
                sv->g.level);
    }
    return SDK_OK;
}

static int qg_step(struct sdk_ctx *c, const struct sdk_input *in,
                   uint32_t now_ms)
{
    struct qd_game *g = &sv->g;
    int i, ev = 0, n;
    uint32_t dt;

    switch (in->menu_pick)
    {
        case PICK_NEW:
            new_game(c);                /* the SDK redraws on close */
            break;
        case PICK_SETTINGS:
            sdk_settings_open(c);       /* the level applies next game */
            break;
        default:
            break;
    }
    if (pause_next && g->phase != QD_PH_OVER)
    {
        pause_next = false;
        pause_open(c);
    }
    pause_next = false;                 /* a suspend over Game Over is spent */
    if (sdk_menu_is_open(c))
    {
        last_ms = 0;                    /* no game time passes */
        input_reset();
        return SDK_IDLE;
    }
    for (i = 0; i < in->n; i++)
        if (in->ev[i].btn == SDK_BTN_MENU && in->ev[i].kind == SDK_IN_LONG)
        {
            pause_open(c);              /* the long MENU: pause at once */
            last_ms = 0;
            return SDK_IDLE;
        }
    dt = last_ms ? now_ms - last_ms : 0;
    if (dt > 100)
        dt = 100;                       /* a late frame never jumps far */
    last_ms = now_ms;
    budget += (int)dt;                  /* 1,000 rows a second at most */
    if (budget > SDK_LCD_H)
        budget = SDK_LCD_H;

    if (hold_req && g->phase != QD_PH_CLEAR)  /* kept through a flash */
    {
        hold_req = false;
        ev |= qd_hold(g);               /* a MENU tap (back()) */
        if (ev & QD_EV_HOLD)
            sdk_log(c, "hold slot=%u cur=%u", g->hold - 1u, g->cur);
    }
    /* the wheel: a column a step, clockwise to the right */
    for (n = in->steps; n > 0; n--)
        ev |= qd_move(g, 1);
    for (n = in->steps; n < 0; n++)
        ev |= qd_move(g, -1);
    for (i = 0; i < in->n; i++)
    {
        const struct sdk_btn_ev *e = &in->ev[i];

        if (e->btn != SDK_BTN_SELECT)
            continue;
        switch (e->kind)
        {
            case SDK_IN_PRESS:
                sel_down = true;
                sel_soft = sel_dbl = false;
                sel_t0 = e->at_ms;
                over_armed = g->phase == QD_PH_OVER;
                if (g->phase == QD_PH_FALL)
                {
                    if (tap_ok && qd_tap_pending(g)
                        && (uint16_t)(e->at_ms - tap_rel) <= TAP_DBL_MS)
                    {
                        sel_dbl = true; /* the first tap's turn goes back */
                        ev |= qd_double_tap(g);
                        sdk_log(c, "dbltap score=%u",
                                (unsigned)g->score);
                    }
                    else
                        ev |= qd_tap(g);    /* the turn, at once */
                }
                tap_ok = false;
                break;
            case SDK_IN_RELEASE:
                if (g->phase == QD_PH_OVER)
                {
                    if (over_armed && sel_down
                        && (uint16_t)(e->at_ms - sel_t0) < HOLD_SOFT_MS)
                    {
                        new_game(c);
                        draw_all(c);
                        last_ms = now_ms;
                        return SDK_FRAME_MIN_MS;
                    }
                }
                else if (sel_soft)
                {
                    qd_soft(g, false);
                    sdk_log(c, "soft off score=%u", (unsigned)g->score);
                }
                else if (sel_down && !sel_dbl)
                {
                    tap_ok = true;      /* a double tap may follow */
                    tap_rel = e->at_ms;
                }
                sel_down = sel_soft = sel_dbl = false;
                break;
            case SDK_IN_CANCEL:
                /* the exit chord: this press's action goes back */
                if (sel_soft)
                    qd_soft(g, false);
                else if (!sel_dbl)
                    ev |= qd_untap(g);
                sel_down = sel_soft = sel_dbl = tap_ok = false;
                break;
            default:
                break;                  /* LONG, REPEAT: timed here */
        }
    }
    /* held past a tap: its turn goes back, and the soft drop runs. The
     * button must still be down now: a late step must not read a tap
     * whose release is still queued as a hold */
    if (sel_down && !sel_soft && !sel_dbl && g->phase == QD_PH_FALL
        && (in->held & SDK_BTN_SELECT)
        && (uint16_t)((uint16_t)now_ms - sel_t0) >= HOLD_SOFT_MS)
    {
        sel_soft = true;
        ev |= qd_untap(g);
        qd_soft(g, true);
        sdk_log(c, "soft on rot=%u", g->rot);
    }
    ev |= qd_tick(g, dt);
    if (ev & (QD_EV_LOCK | QD_EV_COLLAPSE))
        sdk_click(c);
    if (ev & QD_EV_OVER)
        game_over(c);
    if (ev & (QD_EV_LOCK | QD_EV_COLLAPSE | QD_EV_OVER))
        commit(c);
    draw_well(c, (ev & QD_EV_COLLAPSE) != 0);
    draw_stats(c, false);
    if (ev & QD_EV_OVER)
        draw_over(c);
    return g->phase == QD_PH_OVER ? SDK_IDLE : SDK_FRAME_MIN_MS;
}

/* short MENU (a tap, at release): the hold slot; over Game Over, where
 * nothing is falling, the pause menu */
static bool qg_back(struct sdk_ctx *c)
{
    if (sdk_menu_is_open(c))
        return false;                   /* leave; the game is kept */
    if (sv->g.phase == QD_PH_OVER)
        pause_open(c);
    else
        hold_req = true;                /* the next step holds */
    return true;
}

static void qg_suspend(struct sdk_ctx *c, enum sdk_why why)
{
    (void)why;
    pause_next = true;                  /* back to the pause menu */
    input_reset();
    commit(c);
}

static void qg_resume(struct sdk_ctx *c)
{
    sdk_setting_changed(c, QS_SET_LEVEL);
    if (sdk_setting_changed(c, QS_SET_GHOST))
        sdk_log(c, "ghost=%d", sdk_setting(c, QS_SET_GHOST));
    ghost_on = sdk_setting(c, QS_SET_GHOST) != 0;
    last_ms = 0;
    draw_all(c);
}

static void qg_exit(struct sdk_ctx *c, enum sdk_why why)
{
    (void)why;
    commit(c);
}

SDK_DESC_A(quadrus, 8192, 384, 64, MOD_EXIT_GRACEFUL, 200);
SDK_GAME(quadrus, qs_strings) = {
    .init = qg_init, .step = qg_step, .back = qg_back,
    .suspend = qg_suspend, .resume = qg_resume, .exit = qg_exit,
    .settings = &qs_settings,
};
