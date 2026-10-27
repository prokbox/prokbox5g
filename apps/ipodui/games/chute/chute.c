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

/* Chute: a catch game in the spirit of the old handheld LCD games, on
 * the SDK (Class A, docs/design/sdk.md 13.3). Our own theme and art: a
 * light plane crosses the top of a pale LCD, jumpers come down three
 * lanes in fixed steps, and a rower catches them before they reach the
 * lake. As on a real LCD, every position's figure is printed faintly
 * ("unlit") and the live ones are dark.
 *
 * Input (Class A): the relative wheel rows the boat a lane per step,
 * clockwise to the right; SELECT starts a new game after Game Over.
 * Short MENU: the pause menu (Resume, New Game, Settings); MENU with it
 * open leaves (the game is kept). PLAY, NEXT and PREV stay the music's.
 *
 * Drawing: only the figures whose state changed, each a small band;
 * a beat moves at most four jumpers (about 100 rows). */
#include "ipodui/sdk/sdk.h"
#include "lang.h"
#include "chute_core.h"

enum { PS_NAME, PS_BEST, PS_OVER, PS_NEW_BEST, PS_SPEED, PS_SLOW,
       PS_NORMAL, PS_FAST, PS__N };
static const uint16_t ps_lang[PS__N] = {
    LANG_IPODUI_X_CHUTE, LANG_IPODUI_X_G_BEST,
    LANG_IPODUI_X_G_GAME_OVER, LANG_IPODUI_X_G_NEW_BEST,
    LANG_IPODUI_X_G_SPEED, LANG_IPODUI_X_G_SLOW, LANG_IPODUI_X_G_NORMAL,
    LANG_IPODUI_X_G_FAST,
};
static const struct sdk_strings ps_strings = { PS__N, ps_lang };

enum { PS_SET_SPEED };
static const uint16_t ps_speeds[PC_SPEED__N] = { PS_SLOW, PS_NORMAL,
                                                 PS_FAST };
static const struct sdk_setting ps_set[] = {
    [PS_SET_SPEED] = { .label = PS_SPEED, .kind = SDK_SET_CHOICE, .n = 3,
                       .def = PC_SPEED_NORMAL, .apply = SDK_APPLY_NEXT,
                       .values = ps_speeds },
};
static const struct sdk_settings ps_settings = { .version = 1, .n = 1,
                                                 .s = ps_set };

/* Exit is the SDK's row: it runs the graceful exit itself */
static const uint16_t ps_pause_rows[] = { SDK_STR_RESUME, SDK_STR_NEW_GAME,
                                          SDK_STR_SETTINGS,
                                          SDK_STR_EXIT };
static const struct sdk_menu ps_pause = { 4, ps_pause_rows, 0 };
enum { PICK_RESUME, PICK_NEW, PICK_SETTINGS };

/* -- our art (# = ink) ---------------------------------------------------- */
static const char *const art_jumper[] = {
    ".....#####.....",
    "...#########...",
    "..###########..",
    ".#############.",
    "###############",
    "#.#.#.#.#.#.#.#",
    ".#.....#.....#.",
    "..#....#....#..",
    "....#..#..#....",
    "......###......",
    "......###......",
    "....#######....",
    "......###......",
    "......###......",
    ".....##.##.....",
    ".....#...#.....",
    "....##...##....",
};
static const char *const art_boat[] = {
    "....................####....................",
    "....................####....................",
    "....................####....................",
    "...................######...................",
    "..................########..................",
    "................##.######.##................",
    ".............###...######...###.............",
    "...........##......######......##...........",
    "..########################################..",
    "...######################################...",
    "....####################################....",
    "...##..##############################..##...",
};
static const char *const art_plane[] = {
    "###.....................................",
    "###..................................#..",
    "###.............#####................#..",
    "#######.........#####................#..",
    "##################################...#..",
    "######################################..",
    "###...################################..",
    "###...############################...#..",
    "..............############...........#..",
    "..............############...........#..",
    "............##..........##..............",
    "............##..........##..............",
};
static const char *const art_splash[] = {
    "...........##...........",
    "........##.##.##........",
    "........##....##........",
    "....##............##....",
    "....##....####....##....",
    "..........####..........",
    ".##....##########....##.",
    ".##....##########....##.",
    "....################....",
    "....################....",
};
static const char *const art_miss[] = {
    "...#####...",
    ".#########.",
    "###########",
    "#.#.#.#.#.#",
    ".#...#...#.",
    "...#.#.#...",
    ".....#.....",
};
enum { SP_JUMPER, SP_BOAT, SP_PLANE, SP_SPLASH, SP_MISS, SP__N };
static const struct { const char *const *a; uint8_t w, h; } spr[SP__N] = {
    { art_jumper, 15, 17 }, { art_boat, 44, 12 }, { art_plane, 40, 12 },
    { art_splash, 24, 10 }, { art_miss, 11, 7 },
};
#define SPR_PX (15 * 17 + 44 * 12 + 40 * 12 + 24 * 10 + 11 * 7)

/* -- geometry ------------------------------------------------------------- */
#define LX      8                       /* the LCD */
#define LY      24
#define LW      304
#define LH      212
#define J_X(l, p) (70 + (l) * 64 + (p) * 3)
#define J_Y(p)    (34 + (p) * 24)
#define LANE_CX(l) (J_X(l, PC_STEPS - 1) + 7)
#define BOAT_Y  178
#define WAVE_Y  192
#define SPL_Y   200
#define SCORE_X 250
#define SCORE_Y 30
#define MISS_Y  54
#define BEST_Y  68

/* -- colours (ours) ------------------------------------------------------- */
#define C_KEY     SDK_RGB(0xff, 0x00, 0xff)
#define C_LIT     SDK_RGB(0x26, 0x2c, 0x2a)
#define C_UNLIT   SDK_RGB(0xb2, 0xbb, 0xa2)

struct pw_work {
    sdk_px bg[SDK_LCD_H];               /* the bezel's rows */
    sdk_px lcd[SDK_LCD_H];              /* the LCD's rows */
    sdk_px px[2][SPR_PX];               /* lit, unlit */
    uint16_t off[SP__N];
};

#define PW_VER 1
#define PW_BEST_N 5
struct pw_save {
    uint8_t  ver, pad[3];
    uint32_t best[PW_BEST_N];
    struct pc_game g;
};

static struct pw_save *sv;
static struct pw_work *wk;
static int ay;
static uint32_t last_ms;
static int8_t new_best;
static bool over_armed, pause_next;
static uint8_t shown_occ[PC_LANES], shown_boat, shown_splash, shown_misses;
static uint32_t shown_score;

/* -- pre-rendering -------------------------------------------------------- */
static void prerender(void)
{
    int i, s, x, y, at = 0;

    for (i = 0; i < SDK_LCD_H; i++)
    {
        int k = i * 0x10 / SDK_LCD_H;

        wk->bg[i] = SDK_RGB(0x3a - k, 0x3e - k, 0x44 - k);
        wk->lcd[i] = SDK_RGB(0xc6 - k / 2, 0xce - k / 2, 0xb6 - k / 2);
    }
    for (s = 0; s < SP__N; s++)
    {
        wk->off[s] = (uint16_t)at;
        for (y = 0; y < spr[s].h; y++)
            for (x = 0; x < spr[s].w; x++, at++)
            {
                bool ink = spr[s].a[y][x] == '#';

                wk->px[0][at] = ink ? C_LIT : C_KEY;
                wk->px[1][at] = ink ? C_UNLIT : C_KEY;
            }
    }
}

/* -- drawing -------------------------------------------------------------- */
static void lcd_rect(struct sdk_ctx *c, int x, int y, int w, int h)
{
    sdk_rows(c, x, y, w, h, &wk->lcd[y]);
}

static void sprite(struct sdk_ctx *c, int s, int x, int y, bool lit)
{
    lcd_rect(c, x, y, spr[s].w, spr[s].h);
    sdk_bitmap_key(c, &wk->px[lit ? 0 : 1][wk->off[s]], 0, 0, spr[s].w, x,
                   y, spr[s].w, spr[s].h, C_KEY);
    sdk_dirty(c, y, spr[s].h);
}

static void draw_waves(struct sdk_ctx *c)
{
    int x, r;

    for (r = 0; r < 2; r++)
        for (x = LX + 6; x < LX + LW - 6; x += 2)
        {
            int ph = (x / 2 + r * 4) & 7, dy = ph < 4 ? ph : 8 - ph;

            sdk_fill(c, x, WAVE_Y + r * 30 + dy, 2, 1, C_LIT);
        }
}

static void draw_figures(struct sdk_ctx *c, bool force)
{
    const struct pc_game *g = &sv->g;
    int l, p;

    for (l = 0; l < PC_LANES; l++)
    {
        uint8_t ch = force ? 0xff : (uint8_t)(g->occ[l] ^ shown_occ[l]);

        for (p = 0; p < PC_STEPS; p++)
            if (ch & (1u << p))
                sprite(c, SP_JUMPER, J_X(l, p), J_Y(p),
                       (g->occ[l] >> p) & 1);
        shown_occ[l] = g->occ[l];
        if (force || g->boat != shown_boat)
        {
            if (force || l == g->boat || l == shown_boat)
                sprite(c, SP_BOAT, LANE_CX(l) - 22, BOAT_Y, l == g->boat);
        }
        if (force || g->splash != shown_splash)
            sprite(c, SP_SPLASH, LANE_CX(l) - 12, SPL_Y,
                   g->splash == l + 1);
    }
    shown_boat = g->boat;
    shown_splash = g->splash;
    if (force || g->misses != shown_misses)
    {
        for (p = 0; p < PC_MISSES; p++)
            sprite(c, SP_MISS, SCORE_X + p * 15, MISS_Y, p < g->misses);
        shown_misses = g->misses;
    }
    if (force || g->score != shown_score)
    {
        lcd_rect(c, SCORE_X, SCORE_Y, LX + LW - 4 - SCORE_X, 20);
        sdk_text_num(c, SCORE_X, SCORE_Y, (int32_t)g->score,
                     SDK_FONT_LIST, C_LIT);
        sdk_dirty(c, SCORE_Y, 20);
        shown_score = g->score;
    }
}

static void draw_best(struct sdk_ctx *c)
{
    int x;

    lcd_rect(c, SCORE_X, BEST_Y, LX + LW - 4 - SCORE_X, 16);
    x = SCORE_X + sdk_text(c, SCORE_X, BEST_Y, PS_BEST, SDK_FONT_SMALL,
                           C_UNLIT) + 5;
    sdk_text_num(c, x, BEST_Y, (int32_t)sv->best[0], SDK_FONT_SMALL,
                 C_UNLIT);
    sdk_dirty(c, BEST_Y, 16);
}

static void draw_over(struct sdk_ctx *c)
{
    int w = 140, h = new_best >= 0 ? 44 : 30, x = 98, y = 96, tw;

    lcd_rect(c, x, y, w, h);
    sdk_fill(c, x, y, w, 1, C_LIT);
    sdk_fill(c, x, y + h - 1, w, 1, C_LIT);
    sdk_fill(c, x, y, 1, h, C_LIT);
    sdk_fill(c, x + w - 1, y, 1, h, C_LIT);
    tw = sdk_text_width(c, PS_OVER, SDK_FONT_LIST);
    sdk_text(c, x + (w - tw) / 2, y + 6, PS_OVER, SDK_FONT_LIST, C_LIT);
    if (new_best >= 0)
    {
        tw = sdk_text_width(c, PS_NEW_BEST, SDK_FONT_SMALL);
        sdk_text(c, x + (w - tw) / 2, y + 26, PS_NEW_BEST, SDK_FONT_SMALL,
                 C_LIT);
    }
    sdk_dirty(c, y, h);
}

static void draw_all(struct sdk_ctx *c)
{
    sdk_rows(c, 0, ay, SDK_LCD_W, SDK_LCD_H - ay, &wk->bg[ay]);
    sdk_material(c, SDK_MAT_METAL, LX - 2, LY - 2, LW + 4, 2);
    sdk_material(c, SDK_MAT_METAL, LX - 2, LY + LH, LW + 4, 2);
    sdk_material(c, SDK_MAT_METAL, LX - 2, LY, 2, LH);
    sdk_material(c, SDK_MAT_METAL, LX + LW, LY, 2, LH);
    lcd_rect(c, LX, LY, LW, LH);
    sdk_bitmap_key(c, &wk->px[0][wk->off[SP_PLANE]], 0, 0, 40, LX + 10,
                   LY + 8, 40, 12, C_KEY);
    draw_waves(c);
    draw_figures(c, true);
    draw_best(c);
    if (sv->g.phase == PC_PH_OVER)
        draw_over(c);
    sdk_dirty(c, ay, SDK_LCD_H - ay);
}

/* -- play ----------------------------------------------------------------- */
static void commit(struct sdk_ctx *c)
{
    sdk_state_commit(c, sizeof(*sv));
}

static void new_game(struct sdk_ctx *c)
{
    int sp = sdk_setting(c, PS_SET_SPEED);

    pc_new(&sv->g, sdk_rand32(c), sp);
    new_best = -1;
    over_armed = false;
    commit(c);
    sdk_log(c, "new speed=%d", sp);
}

/* -- the SDK's entry points ----------------------------------------------- */
static int pw_init(struct sdk_ctx *c)
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
    new_best = -1;
    over_armed = pause_next = false;
    if (len != sizeof(*sv) || sv->ver != PW_VER)
    {
        memset(sv, 0, sizeof(*sv));
        sv->ver = PW_VER;
        new_game(c);
    }
    else if (!pc_valid(&sv->g))
        new_game(c);
    else
    {
        pause_next = sv->g.phase == PC_PH_PLAY;
        sdk_log(c, "resume score=%u misses=%u", (unsigned)sv->g.score,
                sv->g.misses);
    }
    return SDK_OK;
}

static int pw_step(struct sdk_ctx *c, const struct sdk_input *in,
                   uint32_t now_ms)
{
    struct pc_game *g = &sv->g;
    int i, n, ev = 0;
    uint32_t dt;

    switch (in->menu_pick)
    {
        case PICK_NEW:
            new_game(c);
            break;
        case PICK_SETTINGS:
            sdk_settings_open(c);
            break;
        default:
            break;
    }
    if (pause_next && g->phase == PC_PH_PLAY)
        sdk_menu_open(c, &ps_pause);
    pause_next = false;
    if (sdk_menu_is_open(c))
    {
        last_ms = 0;
        return SDK_IDLE;
    }
    dt = last_ms ? now_ms - last_ms : 0;
    if (dt > 100)
        dt = 100;
    last_ms = now_ms;

    for (n = in->steps; n > 0; n--)
        ev |= pc_move(g, 1);
    for (n = in->steps; n < 0; n++)
        ev |= pc_move(g, -1);
    for (i = 0; i < in->n; i++)
    {
        const struct sdk_btn_ev *e = &in->ev[i];

        if (e->btn != SDK_BTN_SELECT)
            continue;
        if (e->kind == SDK_IN_PRESS)
            over_armed = g->phase == PC_PH_OVER;
        else if (e->kind == SDK_IN_CANCEL)
            over_armed = false;
        else if (e->kind == SDK_IN_RELEASE && over_armed
                 && g->phase == PC_PH_OVER)
        {
            new_game(c);
            draw_all(c);
            last_ms = now_ms;
            return SDK_FRAME_MIN_MS;
        }
    }
    ev |= pc_tick(g, dt);
    if (ev & (PC_EV_CATCH | PC_EV_MISS))
        sdk_click(c);
    if (ev & PC_EV_OVER)
    {
        new_best = (int8_t)pc_best_insert(sv->best, PW_BEST_N, g->score);
        sdk_log(c, "over score=%u best=%d", (unsigned)g->score, new_best);
    }
    if (ev & (PC_EV_BEAT | PC_EV_OVER))
        commit(c);
    draw_figures(c, false);
    if (ev & PC_EV_OVER)
    {
        draw_best(c);
        draw_over(c);
    }
    return g->phase == PC_PH_OVER ? SDK_IDLE : SDK_FRAME_MIN_MS;
}

static bool pw_back(struct sdk_ctx *c)
{
    if (sdk_menu_is_open(c))
        return false;
    sdk_menu_open(c, &ps_pause);
    return true;
}

static void pw_suspend(struct sdk_ctx *c, enum sdk_why why)
{
    (void)why;
    pause_next = true;
    commit(c);
}

static void pw_resume(struct sdk_ctx *c)
{
    last_ms = 0;
    draw_all(c);
}

static void pw_exit(struct sdk_ctx *c, enum sdk_why why)
{
    (void)why;
    commit(c);
}

SDK_DESC_A(chute, 9216, 128, 64, MOD_EXIT_GRACEFUL, 200);
SDK_GAME(chute, ps_strings) = {
    .init = pw_init, .step = pw_step, .back = pw_back,
    .suspend = pw_suspend, .resume = pw_resume, .exit = pw_exit,
    .settings = &ps_settings,
};
