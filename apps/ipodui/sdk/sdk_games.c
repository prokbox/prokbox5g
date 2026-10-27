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

/* Extras > Games (docs/design/sdk.md section 8.5, brief 3.2 M2): the
 * resident Games list, our games registry and the firmware's games file
 * (play counts and kills in a row, per module). A game the unit can't
 * run, or one killed three times in a row since its last update, has no
 * row (logged, never an error). */
#include "config.h"
#include <stdio.h>
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "file.h"
#include "dir.h"
#include "lang.h"
#include "rbpaths.h"
#include "ipodui/uilog.h"
#include "ipodui/anim.h"
#include "ipodui/theme.h"
#include "ipodui/list.h"
#include "ipodui/input.h"
#include "ipodui/screen_stack.h"
#include "ipodui/safefile.h"
#include "ipodui/modload.h"
#include "ipodui/sdk/sdk.h"
#include "ipodui/sdk/sdk_core.h"
#include "ipodui/sdk/sdk_int.h"
#include "ipodui/preview.h"
#include "ipodui/extras.h"              /* the covers (UI_COVER_*) */

#define GAMES_FILE      ROCKBOX_DIR "/gamedata/.games"
#define GAMES_TMP       ROCKBOX_DIR "/gamedata/.games.tmp"
#define GAMES_PREV      ROCKBOX_DIR "/gamedata/.games.old"

/* Our games, in the stock list's alphabetical order (rev 0: their
 * names are firmware language ids), each with its cover in the list's
 * panel (owner, 2026-10-24; the test game the plain Games icon) */
static const struct { int8_t mod; uint8_t cover; uint16_t name; } games[] = {
    { MOD_BRICKS, UI_COVER_BRICKS, LANG_IPODUI_X_BRICKS },
    { MOD_CHESS, UI_COVER_CHESS, LANG_IPODUI_X_CHESS },
    { MOD_CHUTE, UI_COVER_CHUTE, LANG_IPODUI_X_CHUTE },
#ifdef HAVE_IPOD_UI_DEVMODE  /* its name exists only there (english.lang) */
    { MOD_BADGAME, UI_COVER_NONE, LANG_IPODUI_X_SDK_TESTS },
#endif
    { MOD_QUADRUS, UI_COVER_QUADRUS, LANG_IPODUI_X_QUADRUS },
    { MOD_SOLITAIRE, UI_COVER_SOLITAIRE, LANG_IPODUI_X_SOLITAIRE },
};
#define N_GAMES ((int)ARRAYLEN(games))

#ifdef SIMULATOR
extern const void *const mod_exports_solitaire[SDK_SLOT__N];
#ifdef HAVE_IPOD_UI_DEVMODE
extern const void *const mod_exports_badgame[SDK_SLOT__N];
#endif
extern const void *const mod_exports_quadrus[SDK_SLOT__N];
extern const void *const mod_exports_bricks[SDK_SLOT__N];
extern const void *const mod_exports_chute[SDK_SLOT__N];
extern const void *const mod_exports_chess[SDK_SLOT__N];
#endif

const void *const *sdk_games_static_exports(int mod, int *n)
{
    *n = 0;
#ifdef SIMULATOR
    *n = SDK_SLOT__N;
    if (mod == MOD_SOLITAIRE)
        return mod_exports_solitaire;
#ifdef HAVE_IPOD_UI_DEVMODE
    if (mod == MOD_BADGAME)
        return mod_exports_badgame;
#endif
    if (mod == MOD_QUADRUS)
        return mod_exports_quadrus;
    if (mod == MOD_BRICKS)
        return mod_exports_bricks;
    if (mod == MOD_CHUTE)
        return mod_exports_chute;
    if (mod == MOD_CHESS)
        return mod_exports_chess;
    *n = 0;
#else
    (void)mod;
#endif
    return NULL;
}

/* -- the games file: one struct sdk_gamerec per enum mod_id ---------------- */
static void games_load(struct sdk_gamerec r[MOD__N])
{
    int fd = open(GAMES_FILE, O_RDONLY);
    ssize_t n;

    memset(r, 0, sizeof(struct sdk_gamerec) * MOD__N);
    if (fd < 0)
        return;
    /* min(file, MOD__N) records, the rest zero: a firmware with more
     * (or fewer) module ids keeps every game's plays and kills */
    n = read(fd, r, sizeof(struct sdk_gamerec) * MOD__N);
    n = n > 0 ? n / (ssize_t)sizeof(struct sdk_gamerec) : 0;
    memset(&r[n], 0, sizeof(struct sdk_gamerec) * (MOD__N - n));
    close(fd);
}

static void games_save(const struct sdk_gamerec r[MOD__N])
{
    int fd;
    bool ok;

    mkdir(ROCKBOX_DIR "/gamedata");
    fd = open(GAMES_TMP, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return;
    ok = write(fd, r, sizeof(struct sdk_gamerec) * MOD__N)
         == (ssize_t)(sizeof(struct sdk_gamerec) * MOD__N);
    ok = fsync(fd) >= 0 && ok;
    close(fd);
    if (!ok || safefile_replace(GAMES_TMP, GAMES_FILE, GAMES_PREV) != 0)
        ui_log("sdk", "games file write failed");
}

/* the module's version key: a firmware or module update resets kills */
static uint32_t key_of(int mod)
{
    const struct mod_rt *rt = mod_rt_get((enum mod_id)mod);

    return rt && rt->fletcher ? rt->fletcher : 1u;
}

bool sdk_game_offered(int mod)
{
    struct sdk_gamerec r[MOD__N];

    if ((unsigned)mod >= MOD__N)
        return false;
    games_load(r);
    if (sdk_rec_hidden(&r[mod], key_of(mod)))
    {
        UPDATE_TRACE("sdk hide %s kills=%d", mod_name(mod), r[mod].kills);
        return false;
    }
    return true;
}

uint16_t sdk_game_plays(int mod)
{
    struct sdk_gamerec r[MOD__N];

    if ((unsigned)mod >= MOD__N)
        return 0;
    games_load(r);
    return r[mod].plays;
}

/* one write per session, at its end (launch counts ride along) */
static int8_t launched = -1;

void sdk_games_note(int mod, bool launch, bool ended, bool killed)
{
    struct sdk_gamerec r[MOD__N];

    if ((unsigned)mod >= MOD__N)
        return;
    if (launch)
    {
        launched = (int8_t)mod;
        return;
    }
    if (!ended)
        return;
    games_load(r);
    sdk_rec_sync(&r[mod], key_of(mod));
    if (launched == mod)
        sdk_rec_launch(&r[mod]);
    launched = -1;
    sdk_rec_end(&r[mod], killed);
    if (killed && r[mod].kills >= SDK_KILLS_HIDE)
    {
        ui_log("sdk", "hide %s kills=%d", mod_name(mod), r[mod].kills);
    }
    games_save(r);
}

#ifdef SIMULATOR
void sdk_sim_clear(void)
{
    remove(ROCKBOX_DIR "/gamedata/solitaire/.state");
    remove(ROCKBOX_DIR "/gamedata/badgame/.state");
    remove(ROCKBOX_DIR "/gamedata/quadrus/.state");
    remove(ROCKBOX_DIR "/gamedata/bricks/.state");
    remove(ROCKBOX_DIR "/gamedata/chute/.state");
    remove(ROCKBOX_DIR "/gamedata/chess/.state");
    remove(GAMES_FILE);
    UPDATE_TRACE("sdk_clear%s", "");
}

/* `sdk_bad <mode>`: the test game's record (its first byte the mode) */
void sdk_sim_badmode(int mode)
{
    uint8_t img[SDK_STATE_HDR + 4];
    uint8_t rec[4] = { (uint8_t)mode, 0, 0, 0 };
    int n = sdk_state_pack(img, sizeof(img), 0, NULL, 0, rec, sizeof(rec));
    int fd;

    mkdir(ROCKBOX_DIR "/gamedata");
    mkdir(ROCKBOX_DIR "/gamedata/badgame");
    fd = open(ROCKBOX_DIR "/gamedata/badgame/.state",
              O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0)
    {
        if (n > 0)
            write(fd, img, n);
        close(fd);
    }
    UPDATE_TRACE("sdk_bad mode=%d", mode);
}
#endif

/* -- the Games list -------------------------------------------------------- */
struct games_state {
    struct ui_list list;
    uint8_t n;
    int8_t row_mod[N_GAMES];
    uint8_t row_game[N_GAMES];          /* its games[] entry */
    uint16_t row_name[N_GAMES];
};

/* the highlighted row's cover in the panel (Extras' split screen) */
static void games_panel(const struct games_state *gs)
{
    ui_panel_select(&ui_preview_x_gamecover,
                    gs->n ? &games[gs->row_game[gs->list.selected]].cover
                          : NULL);
}

static const char *games_label(int idx, void *data, char *buf, size_t len)
{
    struct games_state *gs = data;

    (void)buf; (void)len;
    return idx < gs->n ? (const char *)str(gs->row_name[idx]) : "";
}

static void games_rows(struct games_state *gs)
{
    struct sdk_gamerec r[MOD__N];
    int i, m;

    gs->n = 0;
    games_load(r);                      /* one read for every row */
    for (i = 0; i < N_GAMES; i++)
    {
        const struct mod_rt *rt = mod_rt_get((enum mod_id)games[i].mod);

        m = games[i].mod;
        if (!rt || !mod_runnable(rt))
            continue;
        if (sdk_rec_hidden(&r[m], key_of(m)))
        {
            UPDATE_TRACE("sdk hide %s kills=%d", mod_name(m), r[m].kills);
            continue;
        }
        gs->row_mod[gs->n] = games[i].mod;
        gs->row_game[gs->n] = i;
        gs->row_name[gs->n] = games[i].name;
        gs->n++;
    }
}

static int games_enter(struct ui_screen *s, const void *arg)
{
    struct games_state *gs;
    struct viewport vp;

    (void)arg;
    gs = ui_arena_alloc(sizeof(*gs));
    if (!gs)
        return -1;
    s->priv = gs;
    s->title = str(LANG_IPODUI_X_GAMES);
    /* split, like Extras (owner, 2026-10-24): the list at the left, the
     * highlighted game's cover at the right */
    s->header_w = ui_theme->split_x;
    games_rows(gs);
    memset(&vp, 0, sizeof(vp));
    vp.y = ui_theme->header_h;
    vp.width = ui_theme->split_x;
    vp.height = LCD_HEIGHT - ui_theme->header_h;
    ui_list_init(&gs->list, &vp, gs->n, games_label, gs);
    games_panel(gs);
    UPDATE_TRACE("sdk games list rows=%d", gs->n);
    return 0;
}

static void games_leave(struct ui_screen *s)
{
    s->priv = NULL;
}

static void games_draw(struct ui_screen *s, unsigned dirty)
{
    struct games_state *gs = s->priv;
    int y[UI_BANDS_MAX], h[UI_BANDS_MAX], i, n;

    if (!gs)
        return;
    gs->list.data = gs;
    if (dirty & UI_DIRTY_BODY)
    {
        /* back from a game: a killed one may have gone */
        int sel = gs->list.selected;

        games_rows(gs);
        ui_list_set_count(&gs->list, gs->n);
        if (sel >= gs->n && gs->n)
            ui_list_move(&gs->list, gs->n - 1 - sel, (int[2]){0},
                         (int[2]){0});
        ui_list_draw(&gs->list, gs->list.vp.y, gs->list.vp.height);
        games_panel(gs);
        ui_panel_draw_now();
    }
    else if (dirty & UI_DIRTY_BAND)
    {
        n = ui_dirty_bands(y, h);
        for (i = 0; i < n; i++)
            ui_list_draw(&gs->list, y[i], h[i]);
    }
}

static enum ui_result games_input(struct ui_screen *s,
                                  const struct ui_input *in)
{
    struct games_state *gs = s->priv;
    int y[2], h[2], i, n;

    if (!gs)
        return UI_IGNORED;
    gs->list.data = gs;
    switch (in->key)
    {
        case UI_KEY_WHEEL:
            n = ui_list_move(&gs->list, in->wheel_steps, y, h);
            for (i = 0; i < n; i++)
                ui_invalidate_band(y[i], h[i]);
            if (n)
                games_panel(gs);
            if (gs->n)
                mod_preload_id((enum mod_id)gs->row_mod[gs->list.selected]);
            return UI_HANDLED;
        case UI_KEY_SELECT:
            if (!in->release || in->long_press || !gs->n)
                return UI_HANDLED;
            i = gs->row_mod[gs->list.selected];
            UPDATE_TRACE("sdk games open %s", mod_name(i));
            ui_push_t(&sdk_game_screen, (const void *)(intptr_t)i,
                      UI_T_PUSH);
            return UI_HANDLED;
        default:
            return UI_IGNORED;
    }
}

static int games_tick(struct ui_screen *s)
{
    struct games_state *gs = s->priv;

    if (!gs)
        return TIMEOUT_BLOCK;
    gs->list.data = gs;
    return ui_list_tick(&gs->list);
}

const struct ui_screen_ops sdk_games_screen = {
    .name  = "games",
    .enter = games_enter,
    .leave = games_leave,
    .draw  = games_draw,
    .input = games_input,
    .tick  = games_tick,
};
