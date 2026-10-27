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

/* Chess on the SDK (Class A, docs/design/sdk.md 13.5; #132): you against
 * the engine, at three levels, or two people passing the device (the
 * Opponent setting: Player; owner, 2026-10-25).
 *
 * Board: 8 x 8 squares of 26 px at the left, from your side; the panel at
 * the right says whose move it is, check and the result, the level and
 * the move number. Pieces are our own pixel art (chess_glyphs below),
 * drawn in two tones with a dark outline.
 *
 * Controls (owner, 2026-10-24):
 *  - the wheel turned: the cursor steps through the squares that matter,
 *    in reading order: your pieces that can move, or, with one picked up,
 *    the squares it can go to (and its own, to put it back);
 *  - the ring tapped at the top, right, bottom or left (the absolute
 *    position, sdk_wheel_pos()): the cursor one square that way; held
 *    still, it keeps going (chess_pad.c);
 *  - SELECT: pick the piece under the cursor, then SELECT on a square to
 *    move it there (a pawn reaching the last rank asks which piece);
 *  - MENU tap: put a picked piece back, else take back your last move
 *    (and the reply to it); while the engine thinks, stop it and take
 *    back the move it was answering; two players: the last move (a ply);
 *  - MENU held: the pause menu (Resume, New Game, Settings, Exit).
 * PLAY, NEXT and PREV stay the music's (Class A).
 *
 * The engine (chess_engine.c) searches in slices of a few milliseconds a
 * frame and never while the screen is dark; the game is saved as its
 * list of moves in the SDK's state record after every move. */
#include "ipodui/sdk/sdk.h"
#include "lang.h"                   /* the string table's language ids */
#include "chess_core.h"
#include "chess_engine.h"
#include "chess_pad.h"

enum { CH_STR_NAME, CH_STR_LEVEL, CH_STR_EASY, CH_STR_MEDIUM, CH_STR_HARD,
       CH_STR_PLAY_AS, CH_STR_WHITE, CH_STR_BLACK, CH_STR_YOUR_MOVE,
       CH_STR_THINKING, CH_STR_CHECK, CH_STR_CHECKMATE, CH_STR_STALEMATE,
       CH_STR_DRAW, CH_STR_WHITE_WINS, CH_STR_BLACK_WINS, CH_STR_REPETITION,
       CH_STR_FIFTY, CH_STR_NO_MATE, CH_STR_QUEEN, CH_STR_ROOK,
       CH_STR_BISHOP, CH_STR_KNIGHT, CH_STR_MOVE_N, CH_STR_OPPONENT,
       CH_STR_CPU, CH_STR_PLAYER, CH_STR_WHITE_MOVE, CH_STR_BLACK_MOVE,
       CH_STR_TAG_HUMAN, CH_STR_TAG_CPU, CH_STR__N };
static const uint16_t ch_lang[CH_STR__N] = {
    LANG_IPODUI_X_CHESS, LANG_IPODUI_X_C_DIFFICULTY, LANG_IPODUI_X_C_EASY,
    LANG_IPODUI_X_C_MEDIUM, LANG_IPODUI_X_C_HARD, LANG_IPODUI_X_C_PLAY_AS,
    LANG_IPODUI_X_C_WHITE, LANG_IPODUI_X_C_BLACK, LANG_IPODUI_X_C_YOUR_MOVE,
    LANG_IPODUI_X_C_THINKING, LANG_IPODUI_X_C_CHECK,
    LANG_IPODUI_X_C_CHECKMATE, LANG_IPODUI_X_C_STALEMATE,
    LANG_IPODUI_X_C_DRAW, LANG_IPODUI_X_C_WHITE_WINS,
    LANG_IPODUI_X_C_BLACK_WINS, LANG_IPODUI_X_C_REPETITION,
    LANG_IPODUI_X_C_FIFTY, LANG_IPODUI_X_C_NO_MATE, LANG_IPODUI_X_C_QUEEN,
    LANG_IPODUI_X_C_ROOK, LANG_IPODUI_X_C_BISHOP, LANG_IPODUI_X_C_KNIGHT,
    LANG_IPODUI_X_C_MOVE_N, LANG_IPODUI_X_C_OPPONENT, LANG_IPODUI_X_C_CPU,
    LANG_IPODUI_X_C_PLAYER, LANG_IPODUI_X_C_WHITE_MOVE,
    LANG_IPODUI_X_C_BLACK_MOVE,
    LANG_IPODUI_X_C_TAG_HUMAN, LANG_IPODUI_X_C_TAG_CPU,
};
static const struct sdk_strings ch_strings = { CH_STR__N, ch_lang };

/* -- settings ---------------------------------------------------------------- */
/* Opponent first; Player greys Level and Play As out (the SDK's off_if) */
enum { CH_SET_OPP, CH_SET_LEVEL, CH_SET_SIDE, CH_SET__N };
enum { CH_EASY, CH_MEDIUM, CH_HARD };
static const uint16_t ch_opp_values[CS_MODE__N] = { CH_STR_CPU,
                                                    CH_STR_PLAYER };
static const uint16_t ch_level_values[] = { CH_STR_EASY, CH_STR_MEDIUM,
                                            CH_STR_HARD };
static const uint16_t ch_side_values[] = { CH_STR_WHITE, CH_STR_BLACK };
static const struct sdk_setting ch_set[CH_SET__N] = {
    [CH_SET_OPP] = { .label = CH_STR_OPPONENT, .kind = SDK_SET_CHOICE,
                     .n = CS_MODE__N, .def = CS_VS_CPU,
                     .apply = SDK_APPLY_NEXT, .values = ch_opp_values },
    [CH_SET_LEVEL] = { .label = CH_STR_LEVEL, .kind = SDK_SET_CHOICE,
                       .n = 3, .def = CH_MEDIUM, .apply = SDK_APPLY_NOW,
                       .off_if = CH_SET_OPP + 1, .off_val = CS_VS_PLAYER,
                       .values = ch_level_values },
    [CH_SET_SIDE] = { .label = CH_STR_PLAY_AS, .kind = SDK_SET_CHOICE,
                      .n = 2, .def = 0, .apply = SDK_APPLY_NEXT,
                      .off_if = CH_SET_OPP + 1, .off_val = CS_VS_PLAYER,
                      .values = ch_side_values },
};
/* version 2: Opponent added in front (a version 1 table resets) */
static const struct sdk_settings ch_settings = { .version = 2,
                                                 .n = CH_SET__N,
                                                 .s = ch_set };

/* the levels: the deepest iteration, the time a move may take (the
 * device stops at the time on harder positions), the root's noise */
static const struct { int8_t depth; uint16_t cap_ms; int8_t noise; }
    levels[3] = {
    { 2, 1500, 60 },                    /* Easy: shallow and loose */
    { 4, 4000, 6 },                     /* Medium */
    { 6, 10000, 0 },                    /* Hard */
};
#define THINK_SLICE_US  10000           /* of the SDK's 20 ms slice */
#define THINK_NODES     16              /* nodes between two clock reads */
#define THINK_MIN_MS    350             /* a reply never lands at once */

/* the menus; Exit is the SDK's row */
static const uint16_t ch_pause_rows[] = { SDK_STR_RESUME, SDK_STR_NEW_GAME,
                                          SDK_STR_SETTINGS, SDK_STR_EXIT };
static const struct sdk_menu ch_pause = { 4, ch_pause_rows, 0 };
enum { PICK_RESUME, PICK_NEW, PICK_SETTINGS };
static const uint16_t ch_promo_rows[] = { CH_STR_QUEEN, CH_STR_ROOK,
                                          CH_STR_BISHOP, CH_STR_KNIGHT };
static const struct sdk_menu ch_promo = { 4, ch_promo_rows, 0 };

/* -- geometry and colours ---------------------------------------------- */
#define SQ          26
#define BOARD       (8 * SQ)
#define BX          8
#define PX          (BX + BOARD + 8)    /* the panel */
#define PW          (SDK_LCD_W - PX - 4)
#define GW          20                  /* a piece glyph */
#define GH          18

#define C_LIGHT     SDK_RGB(0xe9, 0xed, 0xf1)
#define C_DARK      SDK_RGB(0x7c, 0x96, 0xb4)
#define C_LIGHT_LM  SDK_RGB(0xd5, 0xe6, 0xa9)  /* the last move's squares */
#define C_DARK_LM   SDK_RGB(0x93, 0xb4, 0x80)
#define C_PICK      SDK_RGB(0xf6, 0xcf, 0x5e)  /* the piece picked up */
#define C_CHECK     SDK_RGB(0xe8, 0x7c, 0x6e)  /* a king in check */
#define C_CURSOR    SDK_RGB(0x1e, 0x7c, 0xe8)
#define C_DOT       SDK_RGB(0x3a, 0x4c, 0x60)
#define C_FRAME     SDK_RGB(0x4a, 0x55, 0x62)
#define C_INK       SDK_RGB(0x1c, 0x20, 0x26)
#define C_INK_DIM   SDK_RGB(0x5a, 0x63, 0x6e)
#define C_OUT_W     SDK_RGB(0x24, 0x28, 0x2e)  /* white pieces' outline */
#define C_OUT_B     SDK_RGB(0x0c, 0x0c, 0x10)

/* -- our own piece art: '#' body, a detail in the outline's colour --------- */
static const uint32_t glyph_sil[6][GH] = {
    { /* pawn */
      0x00000, 0x00000, 0x00000, 0x00f00, 0x01f80, 0x01f80, 0x01f80, 0x00f00,
      0x01f80, 0x00f00, 0x00f00, 0x01f80, 0x03fc0, 0x07fe0, 0x07fe0, 0x0fff0,
      0x1fff8, 0x1fff8 },
    { /* knight */
      0x00300, 0x00780, 0x00fc0, 0x01fe0, 0x03ff0, 0x07ff0, 0x0fff0, 0x1fff0,
      0x1fbf0, 0x0f3f0, 0x003f0, 0x007f0, 0x00ff0, 0x01ff0, 0x03ff0, 0x07ff8,
      0x0fff8, 0x1fff8 },
    { /* bishop */
      0x00600, 0x00f00, 0x01f80, 0x03fc0, 0x03fc0, 0x03fc0, 0x03fc0, 0x01f80,
      0x00f00, 0x01f80, 0x00f00, 0x00f00, 0x01f80, 0x03fc0, 0x07fe0, 0x0fff0,
      0x1fff8, 0x1fff8 },
    { /* rook */
      0x00000, 0x0ef70, 0x0ef70, 0x0ef70, 0x0fff0, 0x07fe0, 0x03fc0, 0x03fc0,
      0x03fc0, 0x03fc0, 0x03fc0, 0x03fc0, 0x07fe0, 0x0fff0, 0x0fff0, 0x1fff8,
      0x1fff8, 0x1fff8 },
    { /* queen */
      0x10608, 0x18f18, 0x1cf38, 0x1ef78, 0x1fff8, 0x0fff0, 0x0fff0, 0x07fe0,
      0x03fc0, 0x03fc0, 0x01f80, 0x01f80, 0x03fc0, 0x07fe0, 0x0fff0, 0x1fff8,
      0x1fff8, 0x1fff8 },
    { /* king */
      0x00600, 0x01f80, 0x01f80, 0x00600, 0x03fc0, 0x0fff0, 0x1fff8, 0x1fff8,
      0x0fff0, 0x07fe0, 0x03fc0, 0x01f80, 0x01f80, 0x03fc0, 0x07fe0, 0x0fff0,
      0x1fff8, 0x1fff8 },
};
/* the details: (piece, row, bits) */
static const struct { uint8_t piece, row; uint32_t bits; } glyph_det[] = {
    { CS_N - 1, 4, 0x00800 }, { CS_N - 1, 8, 0x00800 },
    { CS_B - 1, 3, 0x00200 }, { CS_B - 1, 4, 0x00400 },
    { CS_B - 1, 5, 0x00800 }, { CS_K - 1, 7, 0x00600 },
};

/* -- the game's memory ------------------------------------------------------ */
#define CH_TT_BITS      9               /* 512 entries, 4 KB */
#define CH_PLIES_MAX    1020
#define CH_REC_VER      1
struct ch_rec {                         /* the state record (8-aligned) */
    uint8_t  ver;
    uint8_t  human;                     /* the side you play (two
                                           players: 0, White at the bottom) */
    uint8_t  cursor;                    /* a square */
    uint8_t  mode;                      /* enum cs_mode (0 in version 1
                                           records: the engine) */
    uint16_t n;                         /* plies played */
    uint16_t rsv2;
    uint16_t mv[CH_PLIES_MAX];
};
#define REC_LEN(n)      ((uint16_t)(8 + 2 * (n)))

struct ch_work {
    struct ce eng;
    struct ce_tt tt[1 << CH_TT_BITS];
    int32_t pst[6][64];
    uint32_t rep[CS_REP_N];
    struct cs_pos pos;                  /* the game */
    uint16_t legal[CS_MOVES_MAX];
    sdk_px bg[SDK_LCD_H];               /* the panel's backdrop, by row */
    sdk_px fill_w[GH], fill_b[GH];      /* the pieces' two tones, by row */
    uint16_t shown[64];                 /* what each square shows (look()) */
};

/* the working block (the manifest's work): this struct, plus the SDK's
 * context and the state record's image (state = sizeof(struct ch_rec)) */
_Static_assert(sizeof(struct ch_rec) == 2048, "the manifest's state");
_Static_assert(sizeof(struct ch_work) + 2048 + 1024 <= 17408,
               "the manifest's work: the struct, the record, the SDK's own");

enum { PH_REPLAY, PH_HUMAN, PH_THINK, PH_OVER };

/* the game's statics: rebuilt at every init (.bss is zeroed per load) */
static struct ch_work *wk;
static struct ch_rec *rec;
static int ax, ay, aw, ah, by;          /* the play area, the board's top */
static uint8_t phase, n_legal, picked, end_kind, level_now;
static uint16_t replay_to, last_move, promo_move;
static bool promo_open, dirty_all, dirty_panel;
static uint32_t think_t0;
static struct cp_pad pad;

/* -- squares on screen ------------------------------------------------------ */
#define NOSQ 0xff
static int col_of(int s) { return rec->human ? 7 - CS_FILE(s) : CS_FILE(s); }
static int row_of(int s) { return rec->human ? CS_RANK(s) : 7 - CS_RANK(s); }
static int sq_x(int s) { return BX + col_of(s) * SQ; }
static int sq_y(int s) { return by + row_of(s) * SQ; }
static int sq_at(int col, int row)
{
    return rec->human ? CS_SQ(7 - col, row) : CS_SQ(col, 7 - row);
}

/* -- pre-rendering (once, at init) ---------------------------------------- */
static void prerender(void)
{
    int i;

    for (i = 0; i < SDK_LCD_H; i++)
    {
        int g = 0xf4 - i * 0x16 / SDK_LCD_H;

        wk->bg[i] = SDK_RGB(g, g + 2 > 0xff ? 0xff : g + 2,
                            g + 5 > 0xff ? 0xff : g + 5);
    }
    for (i = 0; i < GH; i++)
    {
        int w = 0xff - i * 0x30 / GH, b = 0x62 - i * 0x40 / GH;

        wk->fill_w[i] = SDK_RGB(w, w, w > 0xfc ? 0xff : w + 3);
        wk->fill_b[i] = SDK_RGB(b, b + 2, b + 6);
    }
}

/* -- drawing ------------------------------------------------------------------ */
static void runs(struct sdk_ctx *c, int x, int y, uint32_t bits,
                 const sdk_px *row_col, sdk_px flat)
{
    int k = 0;

    while (k < GW)
    {
        int n;

        if (!(bits & (1u << (GW - 1 - k))))
        {
            k++;
            continue;
        }
        for (n = 1; k + n < GW && (bits & (1u << (GW - 1 - k - n))); n++)
            ;
        if (row_col)
            sdk_rows(c, x + k, y, n, 1, row_col);
        else
            sdk_fill(c, x + k, y, n, 1, flat);
        k += n;
    }
}

static void draw_piece(struct sdk_ctx *c, int x, int y, int pc)
{
    const uint32_t *g = glyph_sil[CS_TYPE(pc) - 1];
    const sdk_px *tone = CS_COLOR(pc) ? wk->fill_b : wk->fill_w;
    sdk_px out = CS_COLOR(pc) ? C_OUT_B : C_OUT_W;
    unsigned r, i;

    for (r = 0; r < GH; r++)
    {
        uint32_t s = g[r], up = r ? g[r - 1] : 0, dn = r + 1 < GH ? g[r + 1] : 0;
        uint32_t in = s & (s << 1) & (s >> 1) & up & dn, det = 0;

        for (i = 0; i < sizeof(glyph_det) / sizeof(glyph_det[0]); i++)
            if (glyph_det[i].piece == CS_TYPE(pc) - 1 && glyph_det[i].row == r)
                det |= glyph_det[i].bits;
        in &= ~det;
        runs(c, x, y + (int)r, s & ~in, NULL, out);
        runs(c, x, y + (int)r, in, &tone[r], 0);
    }
}

static bool is_target(int s)
{
    int i;

    if (picked == NOSQ)
        return false;
    for (i = 0; i < n_legal; i++)
        if (CS_FROM(wk->legal[i]) == picked && CS_TO(wk->legal[i]) == s)
            return true;
    return false;
}

/* what square s should show: the piece, the backdrop (L_*), a target
 * mark, the cursor; drawn again only when it changes (sync_board()) */
enum { L_PLAIN, L_LAST, L_PICK, L_CHECK };
#define LK_PC(k)        ((k) & 15)
#define LK_BG(k)        (((k) >> 4) & 3)
#define LK_TARGET       0x40
#define LK_CURSOR       0x80
static uint16_t look(int s)
{
    struct cs_pos *p = &wk->pos;
    int pc, bg = L_PLAIN;
    uint16_t k;

    if (phase == PH_REPLAY)
        return 0;                       /* empty squares until it is done */
    pc = p->b[CS_TO88(s)];
    if (last_move && (CS_FROM(last_move) == s || CS_TO(last_move) == s))
        bg = L_LAST;
    if (s == picked)
        bg = L_PICK;
    if (CS_TYPE(pc) == CS_K && CS_TO88(s) == p->king[p->side]
        && cs_in_check(p))
        bg = L_CHECK;
    k = (uint16_t)(pc | bg << 4);
    if (is_target(s))
        k |= LK_TARGET;
    if (s == rec->cursor && (phase == PH_HUMAN || phase == PH_OVER))
        k |= LK_CURSOR;
    return k;
}

static void paint_square(struct sdk_ctx *c, int s, uint16_t k)
{
    static const sdk_px bg_l[4] = { C_LIGHT, C_LIGHT_LM, C_PICK, C_CHECK };
    static const sdk_px bg_d[4] = { C_DARK, C_DARK_LM, C_PICK, C_CHECK };
    int x = sq_x(s), y = sq_y(s), pc = LK_PC(k);
    bool light = (CS_FILE(s) + CS_RANK(s)) & 1;

    sdk_fill(c, x, y, SQ, SQ, (light ? bg_l : bg_d)[LK_BG(k)]);
    if (pc)
        draw_piece(c, x + (SQ - GW) / 2, y + (SQ - GH) / 2 + 1, pc);
    if ((k & LK_TARGET) && pc)
    {
        /* a capture: the four corners */
        sdk_fill(c, x, y, 5, 2, C_DOT);
        sdk_fill(c, x, y, 2, 5, C_DOT);
        sdk_fill(c, x + SQ - 5, y, 5, 2, C_DOT);
        sdk_fill(c, x + SQ - 2, y, 2, 5, C_DOT);
        sdk_fill(c, x, y + SQ - 2, 5, 2, C_DOT);
        sdk_fill(c, x, y + SQ - 5, 2, 5, C_DOT);
        sdk_fill(c, x + SQ - 5, y + SQ - 2, 5, 2, C_DOT);
        sdk_fill(c, x + SQ - 2, y + SQ - 5, 2, 5, C_DOT);
    }
    else if (k & LK_TARGET)
    {
        sdk_fill(c, x + SQ / 2 - 3, y + SQ / 2 - 2, 6, 4, C_DOT);
        sdk_fill(c, x + SQ / 2 - 2, y + SQ / 2 - 3, 4, 6, C_DOT);
    }
    if (k & LK_CURSOR)
    {
        sdk_fill(c, x, y, SQ, 2, C_CURSOR);
        sdk_fill(c, x, y + SQ - 2, SQ, 2, C_CURSOR);
        sdk_fill(c, x, y + 2, 2, SQ - 4, C_CURSOR);
        sdk_fill(c, x + SQ - 2, y + 2, 2, SQ - 4, C_CURSOR);
    }
    sdk_dirty(c, y, SQ);
}

/* every square whose look changed (force: all of them) */
static void sync_board(struct sdk_ctx *c, bool force)
{
    int s;

    for (s = 0; s < 64; s++)
    {
        uint16_t k = look(s);

        if (force || k != wk->shown[s])
        {
            wk->shown[s] = k;
            paint_square(c, s, k);
        }
    }
}

static int text_line(struct sdk_ctx *c, int y, int str, enum sdk_font f,
                     sdk_px col)
{
    int w = sdk_text_width(c, str, f);

    sdk_text(c, PX + (PW - w) / 2, y, str, f, col);
    return y + (f == SDK_FONT_LIST ? 19 : 15);
}

static void draw_panel(struct sdk_ctx *c)
{
    struct cs_pos *p = &wk->pos;
    int y = by + 4, k;

    sdk_rows(c, PX - 4, ay, SDK_LCD_W - PX + 4, ah, &wk->bg[ay]);
    if (phase == PH_REPLAY)
    {
        sdk_dirty(c, ay, ah);
        return;
    }
    /* the side to move: its king */
    k = (p->side ? CS_BLACK : 0) | CS_K;
    sdk_fill(c, PX + (PW - SQ) / 2 - 1, y - 1, SQ + 2, SQ + 2, C_FRAME);
    sdk_fill(c, PX + (PW - SQ) / 2, y, SQ, SQ, p->side ? C_DARK : C_LIGHT);
    draw_piece(c, PX + (PW - GW) / 2, y + (SQ - GH) / 2 + 1, k);
    y += SQ + 8;
    if (phase == PH_OVER)
    {
        int head = end_kind == CS_MATE ? CH_STR_CHECKMATE
                   : end_kind == CS_STALEMATE ? CH_STR_STALEMATE : CH_STR_DRAW;
        int sub = end_kind == CS_MATE ? (p->side ? CH_STR_WHITE_WINS
                                                 : CH_STR_BLACK_WINS)
                  : end_kind == CS_DRAW_REP ? CH_STR_REPETITION
                  : end_kind == CS_DRAW_50 ? CH_STR_FIFTY
                  : end_kind == CS_DRAW_DEAD ? CH_STR_NO_MATE : -1;

        y = text_line(c, y, head, SDK_FONT_LIST, C_INK);
        if (sub >= 0)
            y = text_line(c, y, sub, SDK_FONT_SMALL, C_INK);
        else
            y += 15;
    }
    else
    {
        int str = p->side ? CH_STR_BLACK_MOVE : CH_STR_WHITE_MOVE;
        /* who controls the side to move: the person, or the engine */
        int who = rec->mode == CS_VS_CPU && (p->side ? 1 : 0) != rec->human
                  ? CH_STR_TAG_CPU : CH_STR_TAG_HUMAN;

        /* the list font when it fits the panel, else the small one */
        y = text_line(c, y, str, sdk_text_width(c, str, SDK_FONT_LIST) <= PW
                      ? SDK_FONT_LIST : SDK_FONT_SMALL, C_INK);
        y = text_line(c, y, who, SDK_FONT_SMALL, C_INK_DIM);
        if (cs_in_check(p))
            y = text_line(c, y, CH_STR_CHECK, SDK_FONT_SMALL, C_CHECK);
        else
            y += 15;
    }
    y += 10;
    if (rec->mode == CS_VS_CPU)         /* two players: no level */
        y = text_line(c, y, ch_level_values[level_now], SDK_FONT_SMALL,
                      C_INK_DIM);
    else
        y += 15;
    {
        int str = CH_STR_MOVE_N, mv = p->ply / 2 + 1;
        int w = 0;

        /* centred: measure by drawing nothing (the clip refuses x < 0) */
        w = sdk_text_fmt(c, SDK_LCD_W, y, str, mv, SDK_FONT_SMALL, C_INK_DIM);
        sdk_text_fmt(c, PX + (PW - w) / 2, y, str, mv, SDK_FONT_SMALL,
                     C_INK_DIM);
    }
    sdk_dirty(c, ay, ah);
}

static void draw_all(struct sdk_ctx *c)
{
    sdk_rows(c, 0, ay, PX - 4, ah, &wk->bg[ay]);
    sdk_fill(c, BX - 2, by - 2, BOARD + 4, BOARD + 4, C_FRAME);
    sync_board(c, true);
    draw_panel(c);
    sdk_dirty(c, ay, ah);
}

/* -- play ------------------------------------------------------------------- */
static void commit(struct sdk_ctx *c)
{
    sdk_state_commit(c, REC_LEN(rec->n));
}

static int level(struct sdk_ctx *c)
{
    int l = sdk_setting(c, CH_SET_LEVEL);

    return l >= 0 && l < 3 ? l : CH_MEDIUM;
}

/* one saved move onto the board; false: not legal there */
static bool replay_one(uint16_t m)
{
    struct cs_undo u;
    uint16_t mv[CS_MOVES_MAX];
    int n = cs_gen(&wk->pos, mv, false), i;

    for (i = 0; i < n; i++)
        if (mv[i] == m)
            return cs_make_legal(&wk->pos, m, &u);
    return false;
}

static void turn_begin(struct sdk_ctx *c, uint32_t now_ms)
{
    struct cs_pos *p = &wk->pos;

    picked = NOSQ;
    n_legal = (uint8_t)cs_legal(p, wk->legal);
    end_kind = (uint8_t)cs_end(p, n_legal);
    if (end_kind == CS_PLAYING && rec->n >= CH_PLIES_MAX)
        end_kind = CS_DRAW_50;          /* the record is full: a draw */
    if (end_kind != CS_PLAYING)
    {
        phase = PH_OVER;
        sdk_log(c, "over %d plies=%u", end_kind, rec->n);
    }
    else if (cs_person_moves(p->side, rec->human, rec->mode))
        phase = PH_HUMAN;
    else
    {
        const int l = level(c);

        phase = PH_THINK;
        think_t0 = now_ms;
        ce_start(&wk->eng, p, levels[l].depth, levels[l].noise,
                 sdk_rand32(c));
    }
    dirty_panel = true;
}

/* plays m (legal) for whoever moves, saves it */
static void play_move(struct sdk_ctx *c, uint16_t m, uint32_t now_ms)
{
    struct cs_undo u;

    cs_make(&wk->pos, m, &u);
    rec->mv[rec->n++] = m;
    last_move = m;
    commit(c);
    sdk_click(c);
    turn_begin(c, now_ms);              /* the board syncs after the step */
}

static void new_game(struct sdk_ctx *c, uint32_t now_ms)
{
    rec->ver = CH_REC_VER;
    rec->mode = (uint8_t)(sdk_setting(c, CH_SET_OPP) == CS_VS_PLAYER
                          ? CS_VS_PLAYER : CS_VS_CPU);
    /* two players: the board stays White at the bottom (games.md 5.6) */
    rec->human = (uint8_t)(rec->mode == CS_VS_CPU
                           && sdk_setting(c, CH_SET_SIDE) ? 1 : 0);
    rec->cursor = (uint8_t)(rec->human ? CS_SQ(4, 6) : CS_SQ(4, 1));
    rec->n = 0;
    rec->rsv2 = 0;
    cs_start(&wk->pos, wk->rep);
    last_move = 0;
    commit(c);
    turn_begin(c, now_ms);
    dirty_all = true;
    sdk_log(c, "new game side=%u level=%d mode=%u", rec->human, level(c),
            rec->mode);
}

/* replay the saved moves up to replay_to, a slice at a time */
static void replay_step(struct sdk_ctx *c, uint32_t budget_us, uint32_t now_ms)
{
    uint32_t t0 = sdk_now_us(c);
    struct cs_pos *p = &wk->pos;

    while (p->ply < replay_to)
    {
        if (!replay_one(rec->mv[p->ply]))
        {
            sdk_log(c, "bad record at %u", p->ply);
            new_game(c, now_ms);
            return;
        }
        if (sdk_now_us(c) - t0 > budget_us)
            return;
    }
    rec->n = replay_to;
    last_move = rec->n ? rec->mv[rec->n - 1] : 0;
    commit(c);
    turn_begin(c, now_ms);
    dirty_all = true;
}

static void replay_from_start(uint16_t to)
{
    cs_start(&wk->pos, wk->rep);
    replay_to = to;
    picked = NOSQ;
    phase = PH_REPLAY;
}

/* MENU tap without a piece picked up: take your last move back */
static void take_back(struct sdk_ctx *c)
{
    uint16_t n = cs_take_back(rec->n, rec->human, rec->mode,
                              phase == PH_THINK);

    if (phase == PH_THINK)
        ce_stop(&wk->eng);              /* and the move it was answering */
    if (n == rec->n)
        return;
    sdk_log(c, "take back %u -> %u", rec->n, n);
    rec->cursor = (uint8_t)CS_FROM(rec->mv[n]);     /* where it came from */
    replay_from_start(n);
}

/* -- the cursor ---------------------------------------------------------------- */
static void move_cursor(struct sdk_ctx *c, int s)
{
    (void)c;
    rec->cursor = (uint8_t)s;           /* the board syncs after the step */
}

/* a tap on the ring: one square that way on screen */
static void cursor_dir(struct sdk_ctx *c, int dir)
{
    int col = col_of(rec->cursor), row = row_of(rec->cursor);

    if (dir == CP_UP && row > 0) row--;
    else if (dir == CP_DOWN && row < 7) row++;
    else if (dir == CP_LEFT && col > 0) col--;
    else if (dir == CP_RIGHT && col < 7) col++;
    else return;
    move_cursor(c, sq_at(col, row));
}

static bool has_moves(int s)
{
    int i;

    for (i = 0; i < n_legal; i++)
        if (CS_FROM(wk->legal[i]) == s)
            return true;
    return false;
}

/* the squares the wheel steps through, in reading order */
static bool stop_at(int s)
{
    if (picked == NOSQ)
        return has_moves(s);
    return s == picked || is_target(s);
}

static void cursor_steps(struct sdk_ctx *c, int steps)
{
    int idx = row_of(rec->cursor) * 8 + col_of(rec->cursor), k, guard;
    int d = steps > 0 ? 1 : -1;

    if (phase != PH_HUMAN)
        return;
    for (k = steps > 0 ? steps : -steps; k > 0; k--)
        for (guard = 0; guard < 64; guard++)
        {
            idx = (idx + d + 64) & 63;
            if (stop_at(sq_at(idx & 7, idx >> 3)))
                break;
        }
    move_cursor(c, sq_at(idx & 7, idx >> 3));
}

static void act_select(struct sdk_ctx *c, uint32_t now_ms)
{
    int s = rec->cursor, i, n = 0;
    uint16_t m = 0;

    if (phase != PH_HUMAN)
        return;
    if (picked == NOSQ || (s != picked && !is_target(s)))
    {
        if (has_moves(s))
            picked = (uint8_t)s;        /* pick up (or switch pieces) */
        return;
    }
    if (s == picked)
    {
        picked = NOSQ;                  /* put it back */
        return;
    }
    for (i = 0; i < n_legal; i++)
        if (CS_FROM(wk->legal[i]) == picked && CS_TO(wk->legal[i]) == s)
        {
            m = wk->legal[i];
            n++;
        }
    if (n > 1)
    {
        /* a promotion: which piece (the queen first) */
        promo_move = (uint16_t)(m & 0x0fff);
        promo_open = true;
        sdk_menu_open(c, &ch_promo);
        return;
    }
    picked = NOSQ;
    play_move(c, m, now_ms);
}

static void promote(struct sdk_ctx *c, int row, uint32_t now_ms)
{
    static const uint8_t kinds[4] = { 3, 2, 1, 0 };   /* Q, R, B, N */
    int i;

    promo_open = false;
    for (i = 0; i < n_legal; i++)
        if ((wk->legal[i] & 0x0fff) == promo_move
            && ((wk->legal[i] >> 12) & 3) == kinds[row & 3])
        {
            picked = NOSQ;
            play_move(c, wk->legal[i], now_ms);
            return;
        }
}

/* the engine's slice of this frame */
static void think(struct sdk_ctx *c, uint32_t now_ms)
{
    uint32_t t0 = sdk_now_us(c);
    const int l = level(c);
    int st = CE_RUN;
    uint16_t m;

    while (st == CE_RUN && sdk_now_us(c) - t0 < THINK_SLICE_US)
        st = ce_run(&wk->eng, THINK_NODES);
    if (st == CE_RUN && now_ms - think_t0 < levels[l].cap_ms)
        return;
    if (now_ms - think_t0 < THINK_MIN_MS)
        return;
    m = ce_stop(&wk->eng);
    if (!m || !cs_is_legal(&wk->pos, m))
        m = n_legal ? wk->legal[0] : 0;
    sdk_log(c, "engine d=%d nodes=%u ms=%u", wk->eng.done_depth,
            (unsigned)wk->eng.nodes, (unsigned)(now_ms - think_t0));
    if (m)
        play_move(c, m, now_ms);
}

/* -- the SDK's entry points ----------------------------------------------- */
static int ch_init(struct sdk_ctx *c)
{
    uint16_t len;
    uint32_t now = sdk_now_ms(c);
    int pos;
    bool t;

    sdk_area(c, &ax, &ay, &aw, &ah);
    by = ay + (ah - BOARD) / 2;
    wk = sdk_work(c, sizeof(*wk));
    rec = sdk_state(c, &len);
    if (!wk || !rec)
        return SDK_E_NOMEM;
    sdk_wheel_mode(c, SDK_WHEEL_REL_RAW);  /* one square a click */
    sdk_long_menu(c, SDK_LONGMENU_PAUSE);  /* long MENU pauses */
    sdk_wheel_pos(c, &pos, &t);            /* the ring as a pad */
    prerender();
    ce_build_pst(wk->pst);
    cs_set_pst((const int32_t (*)[64])wk->pst);
    ce_init(&wk->eng, wk->tt, CH_TT_BITS);
    cp_reset(&pad);
    level_now = (uint8_t)level(c);
    picked = NOSQ;
    promo_open = false;
    if (len < 8 || rec->ver != CH_REC_VER || rec->n > CH_PLIES_MAX
        || len != REC_LEN(rec->n) || rec->human > 1 || rec->cursor > 63
        || rec->mode >= CS_MODE__N || (rec->mode && rec->human))
        new_game(c, now);
    else
    {
        sdk_log(c, "resume plies=%u side=%u mode=%u", rec->n, rec->human,
                rec->mode);
        replay_from_start(rec->n);
        replay_step(c, 40000, now);     /* the rest in the next steps */
    }
    draw_all(c);
    dirty_all = dirty_panel = false;
    return SDK_OK;
}

static int ch_step(struct sdk_ctx *c, const struct sdk_input *in,
                   uint32_t now_ms)
{
    int i, pos, dir;
    bool t, other;

    if (in->flags & SDK_INF_SETTINGS || level_now != level(c))
    {
        level_now = (uint8_t)level(c);
        dirty_panel = true;
    }
    if (in->menu_pick >= 0)
    {
        if (promo_open)
            promote(c, in->menu_pick, now_ms);
        else
        {
            if (in->menu_pick == PICK_NEW)
                new_game(c, now_ms);
            else if (in->menu_pick == PICK_SETTINGS)
                sdk_settings_open(c);
            /* the SDK redraws the board under the closed menu (resume) */
        }
    }
    for (i = 0; i < in->n; i++)
    {
        const struct sdk_btn_ev *e = &in->ev[i];

        if (e->btn == SDK_BTN_MENU && e->kind == SDK_IN_LONG)
        {
            if (sdk_menu_is_open(c))
                continue;
            picked = NOSQ;
            sdk_menu_open(c, &ch_pause);
        }
        else if (e->btn == SDK_BTN_SELECT && e->kind == SDK_IN_RELEASE
                 && !sdk_menu_is_open(c))
            act_select(c, now_ms);
    }
    /* the ring: a tap or a hold steps the cursor (not over a menu, not a
     * press, not a turn, not the touch that lit the screen) */
    sdk_wheel_pos(c, &pos, &t);
    other = (in->flags & (SDK_INF_PRESSED | SDK_INF_LIT)) || in->steps
            || in->n || sdk_menu_is_open(c);
    dir = cp_feed(&pad, pos, other, now_ms);
    if (dir != CP_NONE && (phase == PH_HUMAN || phase == PH_OVER))
        cursor_dir(c, dir);
    if (in->steps && !sdk_menu_is_open(c))
        cursor_steps(c, in->steps);
    if (phase == PH_REPLAY)
        replay_step(c, 8000, now_ms);
    else if (phase == PH_THINK && !sdk_menu_is_open(c)
             && !(in->flags & SDK_INF_DARK))
        think(c, now_ms);
    if (dirty_all)
        draw_all(c);
    else
    {
        sync_board(c, false);
        if (dirty_panel)
            draw_panel(c);
    }
    dirty_all = dirty_panel = false;
    if (phase == PH_REPLAY)
        return SDK_FRAME_MIN_MS;
    if (phase == PH_THINK)
        return (in->flags & SDK_INF_DARK) ? SDK_IDLE : SDK_FRAME_MIN_MS;
    return cp_active(&pad) ? 40 : SDK_IDLE;
}

/* short MENU (a tap, at release): put a piece back, else take a move back */
static bool ch_back(struct sdk_ctx *c)
{
    if (sdk_menu_is_open(c))
    {
        if (promo_open)
        {
            promo_open = false;         /* no promotion: the pawn stays */
            sdk_menu_close(c);
            return true;
        }
        return false;                   /* leave; the game is kept */
    }
    if (picked != NOSQ)
    {
        picked = NOSQ;                  /* the board syncs at the step */
        return true;
    }
    if (phase != PH_REPLAY)
        take_back(c);
    return true;
}

static void ch_suspend(struct sdk_ctx *c, enum sdk_why why)
{
    (void)why;
    cp_reset(&pad);
    commit(c);
}

static void ch_resume(struct sdk_ctx *c)
{
    level_now = (uint8_t)level(c);
    draw_all(c);
}

static void ch_exit(struct sdk_ctx *c, enum sdk_why why)
{
    (void)why;
    commit(c);
}

SDK_DESC_A(chess, 17408, 2048, 64, MOD_EXIT_GRACEFUL, 200);
SDK_GAME(chess, ch_strings) = {
    .init = ch_init, .step = ch_step, .back = ch_back,
    .suspend = ch_suspend, .resume = ch_resume, .exit = ch_exit,
    .settings = &ch_settings,
};
