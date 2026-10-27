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

/* Bricks: the brick-breaker on the SDK (Class A, docs/design/sdk.md 13.3).
 * Our own art: gel bricks in four colours by row with our bevel, a metal
 * paddle, a silver ball, on a deep blue field in a metal frame.
 *
 * Input (Class A): the raw wheel (one step per 15-degree click) moves
 * the paddle, every click pending in the frame at once, by the Paddle
 * Speed level's curve (bricks_core.c: a slow turn is precise, a fast one
 * crosses the field); SELECT launches the ball. Short MENU: the
 * pause menu (Resume, New Game, Settings); MENU with it open leaves (the
 * game is kept). PLAY, NEXT and PREV stay the music's. Re-entering a
 * game in progress, or coming back from a suspend, opens the pause menu
 * first.
 *
 * Drawing: only what moved: the ball's old and new rows, the paddle's
 * rows when it moves, a brick's rows when it goes, the score line when
 * it changes; each a full-width band (typically 15-25 rows a frame). */
#include "ipodui/sdk/sdk.h"
#include "lang.h"
#include "bricks_core.h"

enum { BS_NAME, BS_SCORE, BS_LEVEL, BS_BEST, BS_OVER, BS_NEW_BEST,
       BS_SPEED, BS_SLOW, BS_NORMAL, BS_FAST, BS_BALLS, BS_N3, BS_N5,
       BS_PADDLE, BS_FASTER, BS_FASTEST,
       BS__N };
static const uint16_t bs_lang[BS__N] = {
    LANG_IPODUI_X_BRICKS, LANG_IPODUI_X_G_SCORE, LANG_IPODUI_X_G_LEVEL,
    LANG_IPODUI_X_G_BEST, LANG_IPODUI_X_G_GAME_OVER,
    LANG_IPODUI_X_G_NEW_BEST, LANG_IPODUI_X_G_SPEED, LANG_IPODUI_X_G_SLOW,
    LANG_IPODUI_X_G_NORMAL, LANG_IPODUI_X_G_FAST, LANG_IPODUI_X_G_BALLS,
    LANG_IPODUI_X_G_N3, LANG_IPODUI_X_G_N5, LANG_IPODUI_X_G_PADDLE,
    LANG_IPODUI_X_G_FASTER, LANG_IPODUI_X_G_FASTEST,
};
static const struct sdk_strings bs_strings = { BS__N, bs_lang };

enum { BS_SET_SPEED, BS_SET_BALLS, BS_SET_PADDLE };
static const uint16_t bs_speeds[BK_SPEED__N] = { BS_SLOW, BS_NORMAL,
                                                 BS_FAST };
static const uint16_t bs_balls[2] = { BS_N3, BS_N5 };
static const uint16_t bs_paddles[BK_PADDLE_N] = { BS_SLOW, BS_NORMAL,
                                                  BS_FAST, BS_FASTER,
                                                  BS_FASTEST };
static const struct sdk_setting bs_set[] = {
    [BS_SET_SPEED] = { .label = BS_SPEED, .kind = SDK_SET_CHOICE, .n = 3,
                       .def = BK_SPEED_NORMAL, .apply = SDK_APPLY_NEXT,
                       .values = bs_speeds },
    [BS_SET_BALLS] = { .label = BS_BALLS, .kind = SDK_SET_CHOICE, .n = 2,
                       .def = 0, .apply = SDK_APPLY_NEXT,
                       .values = bs_balls },
    [BS_SET_PADDLE] = { .label = BS_PADDLE, .kind = SDK_SET_CHOICE,
                        .n = BK_PADDLE_N, .def = BK_PADDLE_DEF,
                        .apply = SDK_APPLY_NOW, .values = bs_paddles },
};
static const struct sdk_settings bs_settings = { .version = 2, .n = 3,
                                                 .s = bs_set };

/* Exit is the SDK's row: it runs the graceful exit itself */
static const uint16_t bs_pause_rows[] = { SDK_STR_RESUME, SDK_STR_NEW_GAME,
                                          SDK_STR_SETTINGS,
                                          SDK_STR_EXIT };
static const struct sdk_menu bs_pause = { 4, bs_pause_rows, 0 };
enum { PICK_RESUME, PICK_NEW, PICK_SETTINGS };

/* -- geometry ------------------------------------------------------------- */
#define FX      6                       /* the field's top-left */
#define FY      40
#define FRAME   3
#define HUD_Y   20
#define HUD_H   (FY - FRAME - HUD_Y)

/* -- colours (ours) ------------------------------------------------------- */
#define C_KEY     SDK_RGB(0xff, 0x00, 0xff)
#define C_LABEL   SDK_RGB(0x8c, 0x96, 0xa6)
#define C_VALUE   SDK_RGB(0xf2, 0xf4, 0xf8)
#define C_PANEL   SDK_RGB(0x1c, 0x22, 0x2e)
#define C_PANLN   SDK_RGB(0x9a, 0xa4, 0xb4)
#define C_GOLD    SDK_RGB(0xff, 0xc8, 0x2a)

static const uint8_t brick_rgb[4][3] = {
    { 0xe0, 0x50, 0x48 }, { 0xe8, 0xa8, 0x30 }, { 0x4c, 0xb8, 0x6a },
    { 0x3a, 0x8c, 0xe8 },
};

/* -- the working block ---------------------------------------------------- */
struct bw_work {
    sdk_px bg[SDK_LCD_H];               /* the backdrop and the field */
    sdk_px brick[4][BK_BW * BK_BH];
    sdk_px pad[BK_PW * BK_PH];
    sdk_px ball[BK_BALL * BK_BALL];
    uint8_t drawn[BK_ROWS][BK_COLS];
};

#define BW_VER 1
#define BW_BEST_N 5
struct bw_save {
    uint8_t  ver, pad[3];
    uint32_t best[BW_BEST_N];
    struct bk_game g;
};

static struct bw_save *sv;
static struct bw_work *wk;
static int ay;
static uint32_t last_ms;
static int16_t dbx, dby, dpx;           /* the ball and paddle on screen */
static int8_t new_best;
static bool over_armed, pause_next;
static uint32_t shown_score;
static uint8_t shown_level, shown_lives;

/* -- pre-rendering -------------------------------------------------------- */
static sdk_px shade(const uint8_t *c, int t)
{
    int r = c[0], g = c[1], b = c[2];

    if (t > 0)
    {
        r += (255 - r) * t >> 8;
        g += (255 - g) * t >> 8;
        b += (255 - b) * t >> 8;
    }
    else
    {
        r = r * (256 + t) >> 8;
        g = g * (256 + t) >> 8;
        b = b * (256 + t) >> 8;
    }
    return SDK_RGB(r, g, b);
}

static void prerender(void)
{
    static const uint8_t metal[3] = { 0xa8, 0xb0, 0xbc };
    static const uint8_t silver[3] = { 0xd8, 0xdc, 0xe4 };
    int i, x, y;

    for (i = 0; i < SDK_LCD_H; i++)
    {
        int k = i * 0x18 / SDK_LCD_H;

        wk->bg[i] = i < FY - FRAME ? SDK_RGB(0x34 - k, 0x3a - k, 0x48 - k)
                  : SDK_RGB(0x14, 0x20 - k / 2, 0x3c - k);
    }
    for (i = 0; i < 4; i++)
        for (y = 0; y < BK_BH; y++)
            for (x = 0; x < BK_BW; x++)
            {
                int t;

                if (x == BK_BW - 1 || y == BK_BH - 1)
                {
                    wk->brick[i][y * BK_BW + x] = C_KEY;    /* the gap */
                    continue;
                }
                if (y == 0 || x == 0)
                    t = 100;
                else if (y == BK_BH - 2 || x == BK_BW - 2)
                    t = -90;
                else if (y <= 3)
                    t = 60 - y * 14;
                else
                    t = -(y - 3) * 10;
                wk->brick[i][y * BK_BW + x] = shade(brick_rgb[i], t);
            }
    for (y = 0; y < BK_PH; y++)
        for (x = 0; x < BK_PW; x++)
        {
            /* rounded ends: two corner pixels each */
            bool corner = (y == 0 || y == BK_PH - 1)
                          && (x == 0 || x == BK_PW - 1);
            int t = y == 0 ? 120 : y == 1 ? 70 : y >= BK_PH - 2 ? -110
                    : 20 - y * 12;

            wk->pad[y * BK_PW + x] = corner ? C_KEY : shade(metal, t);
        }
    for (y = 0; y < BK_BALL; y++)
        for (x = 0; x < BK_BALL; x++)
        {
            int dx = 2 * x - (BK_BALL - 1), dy = 2 * y - (BK_BALL - 1);
            int d = dx * dx + dy * dy;      /* (2r)^2 = 36 */

            wk->ball[y * BK_BALL + x] = d > 34 ? C_KEY
                : shade(silver, d < 8 && x < 3 && y < 3 ? 160
                                : y >= 4 ? -70 : 0);
        }
}

/* -- drawing -------------------------------------------------------------- */
static void bg_rect(struct sdk_ctx *c, int x, int y, int w, int h)
{
    sdk_rows(c, x, y, w, h, &wk->bg[y]);
}

static void draw_brick(struct sdk_ctx *c, int r, int k)
{
    int x = FX + k * BK_BW, y = FY + BK_BY + r * BK_BH, v = wk->drawn[r][k];

    bg_rect(c, x, y, BK_BW, BK_BH);
    if (v)
        sdk_bitmap_key(c, wk->brick[(v - 1) & 3], 0, 0, BK_BW, x, y, BK_BW,
                       BK_BH, C_KEY);
}

/* the drawn bricks under a screen rect, redrawn (an erased ball) */
static void bricks_under(struct sdk_ctx *c, int x, int y, int w, int h)
{
    int r, k, fy = y - FY - BK_BY;

    if (fy + h <= 0 || fy >= BK_ROWS * BK_BH)
        return;
    for (r = 0; r < BK_ROWS; r++)
    {
        if (r * BK_BH >= fy + h || (r + 1) * BK_BH <= fy)
            continue;
        for (k = 0; k < BK_COLS; k++)
            if (FX + k * BK_BW < x + w && FX + (k + 1) * BK_BW > x)
                draw_brick(c, r, k);
    }
}

static void draw_hud(struct sdk_ctx *c, bool force)
{
    const struct bk_game *g = &sv->g;
    int x, i;

    if (!force && g->score == shown_score && g->level == shown_level
        && g->lives == shown_lives)
        return;
    bg_rect(c, 0, HUD_Y, SDK_LCD_W, HUD_H);
    x = 8 + sdk_text(c, 8, HUD_Y + 1, BS_SCORE, SDK_FONT_SMALL,
                     C_LABEL) + 6;
    sdk_text_num(c, x, HUD_Y + 1, (int32_t)g->score, SDK_FONT_SMALL, C_VALUE);
    x = 130 + sdk_text(c, 130, HUD_Y + 1, BS_LEVEL, SDK_FONT_SMALL,
                       C_LABEL) + 6;
    sdk_text_num(c, x, HUD_Y + 1, g->level, SDK_FONT_SMALL, C_VALUE);
    /* the balls in hand: the one in play is on the field */
    for (i = 0; i < g->lives - (g->phase == BK_PH_OVER ? 0 : 1) && i < 8;
         i++)
        sdk_bitmap_key(c, wk->ball, 0, 0, BK_BALL, SDK_LCD_W - 14 - i * 10,
                       HUD_Y + 5, BK_BALL, BK_BALL, C_KEY);
    sdk_dirty(c, HUD_Y, HUD_H);
    shown_score = g->score;
    shown_level = g->level;
    shown_lives = g->lives;
}

static void draw_bricks(struct sdk_ctx *c)
{
    int r, k;

    for (r = 0; r < BK_ROWS; r++)
    {
        bool ch = false;

        for (k = 0; k < BK_COLS; k++)
            if (wk->drawn[r][k] != sv->g.brick[r][k])
            {
                wk->drawn[r][k] = sv->g.brick[r][k];
                draw_brick(c, r, k);
                ch = true;
            }
        if (ch)
            sdk_dirty(c, FY + BK_BY + r * BK_BH, BK_BH);
    }
}

static void draw_paddle(struct sdk_ctx *c, bool force)
{
    int px = bk_pad_x(&sv->g);

    if (!force && px == dpx)
        return;
    if (dpx >= 0)
        bg_rect(c, FX + dpx, FY + BK_PY, BK_PW, BK_PH);
    sdk_bitmap_key(c, wk->pad, 0, 0, BK_PW, FX + px, FY + BK_PY, BK_PW,
                   BK_PH, C_KEY);
    sdk_dirty(c, FY + BK_PY, BK_PH);
    dpx = (int16_t)px;
}

static void draw_ball(struct sdk_ctx *c, bool force)
{
    int bx = bk_ball_x(&sv->g), by = bk_ball_y(&sv->g);
    bool show = sv->g.phase != BK_PH_OVER;

    if (!force && bx == dbx && by == dby)
        return;
    if (dby >= 0)
    {
        bg_rect(c, FX + dbx, FY + dby, BK_BALL, BK_BALL);
        bricks_under(c, FX + dbx, FY + dby, BK_BALL, BK_BALL);
        sdk_dirty(c, FY + dby, BK_BALL);
        if (dby + BK_BALL > BK_PY)
            draw_paddle(c, true);
    }
    dbx = (int16_t)bx;
    dby = (int16_t)(show ? by : -1);
    if (!show)
        return;
    sdk_bitmap_key(c, wk->ball, 0, 0, BK_BALL, FX + bx, FY + by, BK_BALL,
                   BK_BALL, C_KEY);
    sdk_dirty(c, FY + by, BK_BALL);
}

static void outline(struct sdk_ctx *c, int x, int y, int w, int h,
                    sdk_px col)
{
    sdk_fill(c, x, y, w, 1, col);
    sdk_fill(c, x, y + h - 1, w, 1, col);
    sdk_fill(c, x, y, 1, h, col);
    sdk_fill(c, x + w - 1, y, 1, h, col);
}

static void draw_over(struct sdk_ctx *c)
{
    int w = 160, h = new_best >= 0 ? 70 : 54, x = (SDK_LCD_W - w) / 2;
    int y = FY + 110, tw;

    sdk_fill(c, x, y, w, h, C_PANEL);
    outline(c, x, y, w, h, C_PANLN);
    tw = sdk_text_width(c, BS_OVER, SDK_FONT_LIST);
    sdk_text(c, x + (w - tw) / 2, y + 8, BS_OVER, SDK_FONT_LIST, C_VALUE);
    tw = sdk_num_width(c, (int32_t)sv->g.score, SDK_FONT_LIST);
    sdk_text_num(c, x + (w - tw) / 2, y + 30, (int32_t)sv->g.score,
                 SDK_FONT_LIST, C_GOLD);
    if (new_best >= 0)
    {
        tw = sdk_text_width(c, BS_NEW_BEST, SDK_FONT_SMALL);
        sdk_text(c, x + (w - tw) / 2, y + 50, BS_NEW_BEST, SDK_FONT_SMALL,
                 C_LABEL);
    }
    sdk_dirty(c, y, h);
}

static void draw_all(struct sdk_ctx *c)
{
    int r;

    bg_rect(c, 0, ay, SDK_LCD_W, SDK_LCD_H - ay);
    sdk_material(c, SDK_MAT_METAL, FX - FRAME, FY - FRAME,
                 BK_FW + 2 * FRAME, FRAME);
    sdk_material(c, SDK_MAT_METAL, FX - FRAME, FY, FRAME, SDK_LCD_H - FY);
    sdk_material(c, SDK_MAT_METAL, FX + BK_FW, FY, FRAME, SDK_LCD_H - FY);
    for (r = 0; r < BK_ROWS; r++)
        memset(wk->drawn[r], 0, BK_COLS);
    draw_bricks(c);
    dpx = dbx = dby = -1;
    draw_paddle(c, true);
    draw_ball(c, true);
    draw_hud(c, true);
    if (sv->g.phase == BK_PH_OVER)
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
    int sp = sdk_setting(c, BS_SET_SPEED);
    int balls = sdk_setting(c, BS_SET_BALLS) ? 5 : 3;

    bk_new(&sv->g, sp, balls);
    new_best = -1;
    over_armed = false;
    commit(c);
    sdk_log(c, "new speed=%d balls=%d", sp, balls);
}

/* -- the SDK's entry points ----------------------------------------------- */
static int bw_init(struct sdk_ctx *c)
{
    int x, w, h;
    uint16_t len;

    sdk_area(c, &x, &ay, &w, &h);
    wk = sdk_work(c, sizeof(*wk));
    sv = sdk_state(c, &len);
    if (!wk || !sv)
        return SDK_E_NOMEM;
    prerender();
    sdk_wheel_mode(c, SDK_WHEEL_REL_RAW);   /* our own curve (bricks_core) */
    last_ms = 0;
    new_best = -1;
    over_armed = pause_next = false;
    dpx = dbx = dby = -1;
    if (len != sizeof(*sv) || sv->ver != BW_VER)
    {
        memset(sv, 0, sizeof(*sv));
        sv->ver = BW_VER;
        new_game(c);
    }
    else if (!bk_valid(&sv->g))
        new_game(c);
    else
    {
        pause_next = sv->g.phase == BK_PH_PLAY;
        sdk_log(c, "resume score=%u level=%u", (unsigned)sv->g.score,
                sv->g.level);
    }
    return SDK_OK;
}

static int bw_step(struct sdk_ctx *c, const struct sdk_input *in,
                   uint32_t now_ms)
{
    struct bk_game *g = &sv->g;
    int i, ev = 0;
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
    if (pause_next && g->phase == BK_PH_PLAY)
    {
        pause_next = false;
        sdk_menu_open(c, &bs_pause);
    }
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

    if (in->steps)                      /* all the clicks since last frame */
        bk_paddle(g, bk_wheel_px(sdk_setting(c, BS_SET_PADDLE), in->steps,
                                 in->velocity));
    for (i = 0; i < in->n; i++)
    {
        const struct sdk_btn_ev *e = &in->ev[i];

        if (e->btn != SDK_BTN_SELECT)
            continue;
        if (e->kind == SDK_IN_PRESS)
        {
            over_armed = g->phase == BK_PH_OVER;
            ev |= bk_launch(g);
        }
        else if (e->kind == SDK_IN_RELEASE && over_armed
                 && g->phase == BK_PH_OVER)
        {
            new_game(c);
            draw_all(c);
            last_ms = now_ms;
            return SDK_FRAME_MIN_MS;
        }
        else if (e->kind == SDK_IN_CANCEL)
            over_armed = false;
    }
    ev |= bk_tick(g, dt);
    if (ev & (BK_EV_BRICK | BK_EV_LOST))
        sdk_click(c);
    if (ev & BK_EV_OVER)
    {
        new_best = (int8_t)bk_best_insert(sv->best, BW_BEST_N, g->score);
        sdk_log(c, "over score=%u level=%u best=%d", (unsigned)g->score,
                g->level, new_best);
    }
    if (ev & (BK_EV_BRICK | BK_EV_LOST | BK_EV_CLEARED | BK_EV_OVER
              | BK_EV_LAUNCH))
        commit(c);
    draw_bricks(c);
    draw_paddle(c, false);
    draw_ball(c, false);
    draw_hud(c, false);
    if (ev & BK_EV_OVER)
        draw_over(c);
    return g->phase == BK_PH_OVER ? SDK_IDLE : SDK_FRAME_MIN_MS;
}

static bool bw_back(struct sdk_ctx *c)
{
    if (sdk_menu_is_open(c))
        return false;
    sdk_menu_open(c, &bs_pause);
    return true;
}

static void bw_suspend(struct sdk_ctx *c, enum sdk_why why)
{
    (void)why;
    pause_next = true;
    commit(c);
}

static void bw_resume(struct sdk_ctx *c)
{
    last_ms = 0;
    draw_all(c);
}

static void bw_exit(struct sdk_ctx *c, enum sdk_why why)
{
    (void)why;
    commit(c);
}

SDK_DESC_A(bricks, 6144, 256, 64, MOD_EXIT_GRACEFUL, 200);
SDK_GAME(bricks, bs_strings) = {
    .init = bw_init, .step = bw_step, .back = bw_back,
    .suspend = bw_suspend, .resume = bw_resume, .exit = bw_exit,
    .settings = &bs_settings,
};
