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

/* The game SDK's cold half (docs/design/sdk.md rev 0): the entry and
 * exit paths, the .state file, the overlay menu and the Settings screen.
 * Code that runs on a press or once, never per frame: compiled Thumb
 * (tools/thumb-cold.txt). The per-frame half is sdk.c (ARM). */
/* not ownable (file): a full-screen page or takeover; the drift panel was settled on the way in (ui_panel_hide(), or the takeover's own ui_fb_settle()), and a full lcd_update() releases it (bcmjob_owned_hook) */
#include "config.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "button.h"
#include "lcd.h"
#include "font.h"
#include "file.h"
#include "dir.h"
#include "lang.h"
#include "misc.h"
#include "audio.h"
#include "rbpaths.h"
#include "ipodui/uilog.h"
#include "ipodui/anim.h"
#include "ipodui/theme.h"
#include "ipodui/paint.h"
#include "ipodui/material.h"
#include "ipodui/input.h"
#include "ipodui/list.h"
#include "ipodui/screen_stack.h"
#include "ipodui/safefile.h"
#include "ipodui/modload.h"
#include "ipodui/modclass.h"
#include "ipodui/artcache.h"     /* artcache_disk_awake() */
#include "ipodui/alarm_ring.h"   /* ui_alarm_ring_screen */
#include "playlist.h"
#include "settings.h"
#include "ipodui/sdk/sdk.h"
#include "ipodui/sdk/sdk_core.h"
#include "ipodui/sdk/sdk_int.h"
#include "ipodui/sdk/sdk_priv.h"

/* -- files ----------------------------------------------------------------- */
static void game_dir(const struct sdk_ctx *c, char *buf, size_t len,
                     const char *leaf)
{
    snprintf(buf, len, GAMEDATA "/%s%s%s", c->name, leaf ? "/" : "",
             leaf ? leaf : "");
}

static void state_read(struct sdk_ctx *c)
{
    char path[MAX_PATH];
    uint8_t lim[SDK_SETTINGS_MAX], def[SDK_SETTINGS_MAX];
    uint16_t cap = c->state_cap - SDK_STATE_HDR - c->set_n, len = 0;
    int fd, n = 0, i, f;

    for (i = 0; i < c->set_n; i++)
    {
        lim[i] = c->sets->s[i].kind == SDK_SET_TOGGLE ? 2 : c->sets->s[i].n;
        def[i] = c->sets->s[i].def < lim[i] ? c->sets->s[i].def : 0;
    }
    game_dir(c, path, sizeof(path), ".state");
    fd = open(path, O_RDONLY);
    if (fd >= 0)
    {
        n = read(fd, c->state_img, c->state_cap);  /* one range read */
        close(fd);
    }
    /* unpacked in place: the record lands where the game keeps it */
    f = sdk_state_unpack(n > 0 ? c->state_img : NULL, n > 0 ? n : 0,
                         c->sets ? c->sets->version : 0, c->set_val,
                         c->set_n, lim, def,
                         c->state_img + SDK_REC_OFF(c->set_n), cap, &len);
    c->rec_len = len;
    if (f & SDK_SU_SET_RESET)
        ui_log("sdk", "settings reset %s why=%s", c->name,
               f & SDK_SU_CRC ? "crc" : "version");
    if (f & SDK_SU_CRC)
        ui_log("sdk", "state reset %s why=crc", c->name);
    UPDATE_TRACE("sdk state read %s bytes=%d rec=%u flags=%d", c->name, n,
                 (unsigned)len, f);
}

/* the .state file through safefile's replace; never on a kill */
void sdki_state_write(struct sdk_ctx *c, const char *why)
{
    char path[MAX_PATH], tmp[MAX_PATH], prev[MAX_PATH];
    uint8_t *rec = c->state_img + SDK_REC_OFF(c->set_n);
    int fd, n;

    (void)why;
    if (c->killed || c->usb || (!c->rec_dirty && !c->set_dirty))
        return;
    n = sdk_state_pack(c->state_img, c->state_cap,
                       c->sets ? c->sets->version : 0, c->set_val,
                       c->set_n, rec, c->rec_len);
    if (n < 0)
        return;
    modclass_save(true);                /* the firmware's write, not the
                                           game's reads (its folder too) */
    mkdir(GAMEDATA);
    game_dir(c, path, sizeof(path), NULL);
    mkdir(path);
    game_dir(c, path, sizeof(path), ".state");
    game_dir(c, tmp, sizeof(tmp), ".state.tmp");
    game_dir(c, prev, sizeof(prev), ".state.old");
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0)
    {
        bool ok = write(fd, c->state_img, n) == n;

        ok = fsync(fd) >= 0 && ok;
        close(fd);
        if (ok && safefile_replace(tmp, path, prev) == 0)
        {
            c->rec_dirty = c->set_dirty = false;
            UPDATE_TRACE("sdk state write %s bytes=%d why=%s", c->name, n,
                         why);
        }
        else
            ui_log("sdk", "state write %s failed", c->name);
    }
    /* the pack moved the record down onto the file image's layout; the
     * game keeps using it at its aligned place */
    memmove(rec, c->state_img + SDK_STATE_HDR + c->set_n, c->rec_len);
    modclass_save(false);
}

/* -- the overlay menu (section 5.4): the shell's look ---------------------- */
#define MENU_W      220
#define MENU_PITCH  26
static void menu_geom(const struct sdk_ctx *c, int *x, int *y, int *w,
                      int *h)
{
    *w = MENU_W;
    *h = c->menu_n * MENU_PITCH + 12;
    *x = (LCD_WIDTH - *w) / 2;
    *y = sdki_area_top() + (LCD_HEIGHT - sdki_area_top() - *h) / 2;
}

void sdki_menu_draw(struct sdk_ctx *c, bool dim)
{
    const struct ui_theme *t = ui_theme;
    int x, y, w, h, i, r, k;

    menu_geom(c, &x, &y, &w, &h);
    if (dim)
    {
        /* the play area dimmed to half, once, as the shell's sheets */
        for (r = sdki_area_top(); r < LCD_HEIGHT; r++)
        {
            fb_data *d = FBADDR(0, r);

            for (k = 0; k < LCD_WIDTH; k++)
                d[k] = (fb_data)((d[k] >> 1) & 0x7bef);
        }
    }
    lcd_set_viewport(NULL);
    lcd_set_drawmode(DRMODE_SOLID);
    lcd_set_foreground(LCD_RGBPACK(0x9a, 0xa0, 0xa8));
    lcd_drawrect(x - 1, y - 1, w + 2, h + 2);
    lcd_set_foreground(t->list_bg);
    lcd_fillrect(x, y, w, h);
    for (i = 0; i < c->menu_n; i++)
    {
        int ry = y + 6 + i * MENU_PITCH;
        const char *s = sdki_str_of(c, c->menu_rows[i]);
        int tw = 0, th = 0;

        if (i == c->menu_sel)
        {
            lcd_set_viewport(NULL);
            ui_paint_selection(x + 4, ry, w - 8, MENU_PITCH);
        }
        if (!s)
            continue;
        lcd_setfont(t->font_ui);
        font_getstringsize((const unsigned char *)s, &tw, &th, t->font_ui);
        lcd_set_drawmode(DRMODE_FG);
        lcd_set_foreground(i == c->menu_sel ? t->sel_fg : t->list_fg);
        lcd_putsxy(x + 14, ry + (MENU_PITCH - th) / 2,
                   (const unsigned char *)s);
        lcd_set_drawmode(DRMODE_SOLID);
    }
    sdki_mark(c, dim ? sdki_area_top() : y - 1, dim ? LCD_HEIGHT : h + 2);
}

int sdk_menu_open(struct sdk_ctx *c, const struct sdk_menu *m)
{
    int i;

    API(c, SDK_E_ARG);
    if (!m || m->n < 1 || m->n > SDK_MENU_ROWS_MAX
        || !sdki_src_ok(c, m->rows, m->n * 2u))
    {
        sdki_viol(c, "arg", 0);
        sdki_leave_api(c);
        return SDK_E_ARG;
    }
    c->menu_n = m->n;
    c->menu_sel = m->sel < m->n ? m->sel : 0;
    for (i = 0; i < m->n; i++)
        c->menu_rows[i] = m->rows[i];
    c->menu_open = true;
    if (c->draw_ok)
        sdki_menu_draw(c, true);
    UPDATE_TRACE("sdk menu open %s rows=%d", c->name, m->n);
    sdki_leave_api(c);
    return SDK_OK;
}

void sdki_menu_close(struct sdk_ctx *c)
{
    if (!c->menu_open)
        return;
    c->menu_open = false;
    c->redraw = true;                   /* the game redraws under it */
}

void sdk_menu_close(struct sdk_ctx *c)
{
    API_V(c);
    sdki_menu_close(c);
    sdki_leave_api(c);
}

bool sdk_menu_is_open(struct sdk_ctx *c)
{
    bool r;

    API(c, false);
    r = c->menu_open;
    sdki_leave_api(c);
    return r;
}

bool sdki_settings_wanted;

int sdk_settings_open(struct sdk_ctx *c)
{
    API(c, SDK_E_ARG);
    if (!c->set_n)
    {
        sdki_leave_api(c);
        return SDK_E_NOENT;
    }
    sdki_settings_wanted = true;             /* pushed after the call returns */
    sdki_leave_api(c);
    return SDK_OK;
}

/* -- the game screen ---------------------------------------------------------- */
static void reclaim(struct sdk_ctx *c)
{
    if (c->boosted)
    {
        ui_boost_burst_end();
        c->boosted = false;
    }
    tick_remove_task(sdki_wd_tick);
    ui_set_cover_hook(NULL);
    modclass_save(true);                /* the games file: the firmware's */
    sdk_games_note(c->mod, false, true, c->killed);
    modclass_save(false);
    modclass_top(-1);                   /* the visit ends here: the Games
                                           list's redraw is not the game's */
    mod_unpin((enum mod_id)c->mod);     /* the arena frees the block */
    UPDATE_TRACE("sdk reclaim %s killed=%d", c->name, (int)c->killed);
    c->phase = P_DONE;
    sdk_cur = NULL;
}

/* the exit policy (section 2.4), then the reclaim and the pop slide */
void sdki_run_exit(struct sdk_ctx *c, enum sdk_why why)
{
    if (c->phase != P_RUN || c->exiting)
        return;
    c->exiting = true;
    sdki_menu_close(c);
    UPDATE_TRACE("sdk exit %s why=%d policy=%s", c->name, (int)why,
                 c->policy == MOD_EXIT_GRACEFUL ? "graceful" : "immediate");
    if (c->policy == MOD_EXIT_GRACEFUL && c->g->exit)
    {
        uint32_t ms = c->exit_ms < SDK_EXIT_MS_MAX ? c->exit_ms
                                                   : SDK_EXIT_MS_MAX;
        c->draw_ok = false;
        if (sdki_call_game(c, E_EXIT, ms * 1000u, why) == KILLED)
        {
            ui_log("sdk", "exit deadline %s ms=%lu", c->name,
                   (unsigned long)ms);
            c->killed = false;          /* a late exit: not a crash */
            c->rec_dirty = c->set_dirty = false;    /* torn: not saved */
        }
    }
    sdki_state_write(c, "exit");
    c->phase = P_EXIT;
    ui_pop_request(ui_top());
}

void sdki_kill_now(struct sdk_ctx *c)
{
    c->killed = true;
    c->phase = P_EXIT;
    if (ui_top() == c->scr)
        ui_pop_request(c->scr);         /* covered: at its next draw */
}

/* -- suspend, resume and the deferred save (sdk.md 2.2, 8.2) -------------- */
bool sdki_suspend(struct sdk_ctx *c, enum sdk_why why)
{
    if (c->phase != P_RUN || c->suspended)
        return c->phase == P_RUN;
    sdki_push_bands(c);
    if (sdki_call_game(c, E_SUSPEND, SLICE_A_US, why) == KILLED)
    {
        sdki_kill_now(c);
        return false;
    }
    c->suspended = true;
    c->save_due = true;                 /* never a spin-up of its own */
    UPDATE_TRACE("sdk suspend %s why=%d", c->name, (int)why);
    return true;
}

bool sdki_hold(struct sdk_ctx *c)
{
    c->hold_susp = button_hold();
    if (c->hold_susp)
        return sdki_suspend(c, SDK_WHY_HOLD);
    if (!c->suspended)
        return true;
    c->suspended = false;
    UPDATE_TRACE("sdk resume %s", c->name);
    sdki_full_frame(c);
    if (c->phase != P_RUN)
        return false;
    ui_lcd_push("sdk", 0, sdki_area_top(), LCD_WIDTH,
                LCD_HEIGHT - sdki_area_top());
    return true;
}

/* the record of a suspend: written when the disk is awake anyway (the
 * music's refill, a list's read), never at USB entry or during a session */
void sdki_save_poll(struct sdk_ctx *c)
{
    if (!c->save_due || c->usb || !artcache_disk_awake())
        return;
    c->save_due = false;
    sdki_state_write(c, "deferred");
}

/* screen_stack.c: a screen was pushed over the game */
static void covered(const struct ui_screen *under,
                 const struct ui_screen_ops *by)
{
    struct sdk_ctx *c = sdk_cur;

    if (!c || under != c->scr || by == &sdk_settings_screen)
        return;
    sdki_suspend(c, by == &ui_alarm_ring_screen ? SDK_WHY_ALARM
                                                : SDK_WHY_SYSTEM);
}

/* usbscreen.c, after the ack (on) and at the session's end (off). No
 * call into the game while the host owns the disk: a game that spun
 * there would meet the watchdog's reboot with the volume mounted. The
 * game is marked suspended at entry and gets its suspend() after the
 * session, just before the resume; its record is written at the next
 * natural disk wake after that. */
void sdk_usb(bool on)
{
    struct sdk_ctx *c = sdk_cur;

    if (!c || c->phase != P_RUN)
        return;
    if (on)
    {
        c->usb = true;                  /* first: no file I/O from here */
        if (!c->suspended)
        {
            c->suspended = true;
            c->usb_owed = true;         /* suspend() after the session */
        }
        UPDATE_TRACE("sdk usb %s 1", c->name);
        return;
    }
    c->usb = false;
    UPDATE_TRACE("sdk usb %s 0", c->name);
    if (c->usb_owed)
    {
        c->usb_owed = false;
        if (sdki_call_game(c, E_SUSPEND, SLICE_A_US, SDK_WHY_USB) == KILLED)
            return sdki_kill_now(c);
        c->save_due = true;
        UPDATE_TRACE("sdk suspend %s why=%d", c->name, (int)SDK_WHY_USB);
    }
}

/* sleep.c, before the quiesce: suspend only (no I/O while the screen is
 * still lit, and none if the sleep is abandoned) */
void sdk_sleep_enter(bool timer)
{
    struct sdk_ctx *c = sdk_cur;

    if (c && c->phase == P_RUN)
        sdki_suspend(c, timer ? SDK_WHY_TIMER : SDK_WHY_SLEEP);
}

/* sleep.c's persist() and charging_sleep_prepare(): the record goes with
 * their own saves */
void sdk_persist(void)
{
    struct sdk_ctx *c = sdk_cur;

    if (!c || c->phase != P_RUN || c->usb)
        return;
    if (!sdki_suspend(c, SDK_WHY_SLEEP))
        return;
    c->save_due = false;
    sdki_state_write(c, "sleep");
}

int sdki_game_enter(struct ui_screen *s, const void *arg)
{
    int mod = (int)(intptr_t)arg, i;
    const struct mod_desc *d;
    const struct sdk_game *g;
    const struct mod_rt *rt;
    struct sdk_ctx *c;
    uint8_t *blk;
    uint32_t len, state, at;

    if (sdk_cur || !sdk_game_offered(mod))
        return -1;
    rt = mod_rt_get((enum mod_id)mod);
    if (!rt || !mod_runnable(rt) || rt->kind != MOD_KIND_SDK)
        return -1;
    d = mod_export((enum mod_id)mod, SDK_SLOT_DESC);
    g = mod_export((enum mod_id)mod, SDK_SLOT_GAME);
    if (!d || !g || !g->init || !g->step || d->kind != MOD_KIND_SDK)
    {
        ui_log("sdk", "mod load fail %s", mod_name(mod));
        return -1;
    }
    mod_pin((enum mod_id)mod);
    len = (uint32_t)d->work_kb * 1024u;
    blk = mod_work_alloc((enum mod_id)mod, len);
    state = d->state;
    at = (sizeof(*c) + 15) & ~15u;
    if (!blk || at + SDK_REC_OFF(SDK_SETTINGS_MAX) + state > len)
    {
        ui_log("sdk", "init refused %s rc=nomem", mod_name(mod));
        mod_unpin((enum mod_id)mod);
        return -1;
    }
    c = (struct sdk_ctx *)blk;
    memset(c, 0, sizeof(*c));
    c->mod = (int8_t)mod;
    c->name = mod_name(mod);
    c->g = g;
    c->policy = d->exit_policy;
    c->exit_ms = d->exit_ms;
    c->strs = mod_export((enum mod_id)mod, SDK_SLOT_STRINGS);
    c->cell = (void **)mod_export((enum mod_id)mod, SDK_SLOT_CELL);
    if (!mod_image_range((enum mod_id)mod, &c->img_lo, &c->img_hi))
        c->img_lo = c->img_hi = 0;
    c->sets = g->settings;
    c->set_n = c->sets && c->sets->n <= SDK_SETTINGS_MAX ? c->sets->n : 0;
    c->state_img = blk + at;
    c->state_cap = (uint16_t)(SDK_STATE_HDR + c->set_n + state);
    /* the file image (state_cap) or the aligned record, the larger */
    i = (int)((at + SDK_REC_OFF(c->set_n) + state + 15) & ~15u);
    c->work = blk + i;
    c->work_cap = len - (uint32_t)i;
#ifdef SIMULATOR
    c->rnd = 0x5eed1234u;               /* UI scripts: the same deals */
#else
    c->rnd = NOW_US() ^ (uint32_t)current_tick * 2654435761u;
#endif
    c->first_frame = true;
    c->in.menu_pick = -1;
    c->in.pos = -1;
#ifdef SIMULATOR
    c->ui_thread = pthread_self();
    signal(SIGUSR2, sdki_wd_signal);
#endif
    state_read(c);
    /* the watchdog's last resort reboots without the shutdown path's
     * saves (sdki_wd_tick, from the tick interrupt: no file I/O there):
     * the resume point and the status go out now, with this entry's own
     * reads (an idle-time flush, no spin-up of its own) */
    if (audio_status() & AUDIO_STATUS_PLAY)
        playlist_update_resume_info(audio_current_track());
    status_save(false);
    sdk_cur = c;
    c->scr = s;
    ui_set_cover_hook(covered);
    s->priv = c;
    s->title = sdki_str_of(c, 0) ? sdki_str_of(c, 0) : c->name;
    s->header_w = 0;
    sdk_games_note(mod, true, false, false);
    tick_add_task(sdki_wd_tick);
    modclass_entry(mod);
    /* init runs before the push draws: its drawing waits for the first
     * full frame (resume(), from draw()) */
    c->phase = P_INIT;
    c->draw_ok = false;
    i = sdki_call_game(c, E_INIT, SLICE_INIT_US, 0);
    if (i < 0)
    {
        if (i != KILLED)
            ui_log("sdk", "init refused %s rc=%d", c->name, i);
        c->rec_dirty = c->set_dirty = false;
        reclaim(c);
        s->priv = NULL;
        return -1;
    }
    c->phase = P_RUN;
    c->redraw = true;
    UPDATE_TRACE("sdk start %s class=A work=%lu state=%u", c->name,
                 (unsigned long)c->work_cap, (unsigned)c->state_cap);
    return 0;
}

void sdki_game_leave(struct ui_screen *s)
{
    struct sdk_ctx *c = s->priv;

    if (c && c == sdk_cur)
    {
        if (c->phase == P_RUN)
        {
            /* popped from outside (USB, the main menu): immediate */
            c->exiting = true;
            sdki_state_write(c, "leave");
        }
        reclaim(c);
    }
    s->priv = NULL;
}

void sdki_menu_move(struct sdk_ctx *c, int d)
{
    int sel = c->menu_sel + d;

    sel = sel < 0 ? 0 : sel >= c->menu_n ? c->menu_n - 1 : sel;
    if (sel != c->menu_sel)
    {
        c->menu_sel = (uint8_t)sel;
        sdki_menu_draw(c, false);
        sdki_push_bands(c);
    }
}

void sdk_poweroff(void)
{
    if (sdk_cur && sdk_cur->phase == P_RUN)
        sdki_state_write(sdk_cur, "poweroff");
}

/* -- the SDK's Settings screen (section 9.2) --------------------------------- */
/* One list state for both levels: the rows (idx < 0), or setting idx's
 * value list (CHOICE with more than 3 values), pushed over them */
struct set_state {
    struct ui_list list;
    struct sdk_ctx *c;
    int8_t idx;
};

static int set_lim(const struct sdk_setting *st)
{
    return st->kind == SDK_SET_TOGGLE ? 2 : st->n;
}

/* setting idx is greyed out: another one's value turns it off */
static bool set_off(const struct sdk_ctx *c, int idx)
{
    const struct sdk_setting *st = &c->sets->s[idx];

    return st->off_if && st->off_if <= c->set_n
           && c->set_val[st->off_if - 1] == st->off_val;
}

static bool set_row_off(int row, void *data)
{
    struct set_state *ss = data;

    return ss->idx < 0 && set_off(ss->c, row);
}

static const char *set_value(struct sdk_ctx *c, int idx, int v)
{
    const struct sdk_setting *st = &c->sets->s[idx];

    if (st->kind == SDK_SET_TOGGLE)
        return sdki_str_of(c, v ? SDK_STR_ON : SDK_STR_OFF);
    return st->values && v < st->n ? sdki_str_of(c, st->values[v]) : NULL;
}

static const char *set_label(int row, void *data, char *buf, size_t len)
{
    struct set_state *ss = data;
    struct sdk_ctx *c = ss->c;
    const char *lab, *val;

    if (ss->idx >= 0)                   /* the value list: the values */
    {
        val = set_value(c, ss->idx, row);
        return val ? val : "";
    }
    lab = sdki_str_of(c, c->sets->s[row].label);
    val = set_value(c, row, c->set_val[row]);
    snprintf(buf, len, "%s%c%s", lab ? lab : "", UI_VALUE_SEP,
             val ? val : "");
    return buf;
}

static int set_enter(struct ui_screen *s, const void *arg)
{
    struct set_state *ss;
    struct viewport vp;
    struct sdk_ctx *c = sdk_cur;
    int idx = -1, n;

    if (!c || !arg)
        return -1;
    if (arg != c)
        idx = (int)(intptr_t)arg - 1;   /* the value list of setting idx */
    if (idx >= c->set_n)
        return -1;
    ss = ui_arena_alloc(sizeof(*ss));
    if (!ss)
        return -1;
    ss->c = c;
    ss->idx = (int8_t)idx;
    s->priv = ss;
    s->title = idx < 0 ? (const char *)str(LANG_SETTINGS)
                       : sdki_str_of(c, c->sets->s[idx].label);
    s->header_w = 0;
    memset(&vp, 0, sizeof(vp));
    vp.y = ui_theme->header_h;
    vp.width = LCD_WIDTH;
    vp.height = LCD_HEIGHT - ui_theme->header_h;
    n = idx < 0 ? c->set_n : set_lim(&c->sets->s[idx]);
    ui_list_init(&ss->list, &vp, n, set_label, ss);
    ss->list.is_off = set_row_off;
    if (idx < 0 && set_off(c, 0))
        ss->list.selected = ui_list_skip(&ss->list, 1);  /* the first live row */
    if (idx >= 0)
    {
        /* as the stock value lists (Backlight): the current value is
         * the selected row on entry, no check mark */
        ss->list.selected = c->set_val[idx] < n ? c->set_val[idx] : 0;
        ui_list_set_count(&ss->list, n);    /* keeps it visible */
    }
    return 0;
}

static void set_leave(struct ui_screen *s)
{
    s->priv = NULL;
}

static void set_draw(struct ui_screen *s, unsigned dirty)
{
    struct set_state *ss = s->priv;
    int y[UI_BANDS_MAX], h[UI_BANDS_MAX], i, n;

    if (!ss)
        return;
    ss->list.data = ss;
    if (dirty & UI_DIRTY_BODY)
        ui_list_draw(&ss->list, ss->list.vp.y, ss->list.vp.height);
    else if (dirty & UI_DIRTY_BAND)
    {
        n = ui_dirty_bands(y, h);
        for (i = 0; i < n; i++)
            ui_list_draw(&ss->list, y[i], h[i]);
    }
}

static void set_pick(struct sdk_ctx *c, int idx, int v)
{
    if (c->set_val[idx] == v)
        return;
    c->set_val[idx] = (uint8_t)v;
    c->set_changed |= (uint16_t)(1u << idx);
    c->set_dirty = c->set_news = true;  /* saved with the record, later */
    UPDATE_TRACE("sdk setting %s %d=%d", c->name, idx, v);
}

static enum ui_result set_input(struct ui_screen *s,
                                const struct ui_input *in)
{
    struct set_state *ss = s->priv;
    struct sdk_ctx *c;
    int y[2], h[2], i, n, idx, lim;

    if (!ss || ss->c != sdk_cur)
        return UI_IGNORED;
    c = ss->c;
    ss->list.data = ss;
    switch (in->key)
    {
        case UI_KEY_WHEEL:
            n = ui_list_move(&ss->list,
                             ui_list_skip(&ss->list, in->wheel_steps), y, h);
            for (i = 0; i < n; i++)
                ui_invalidate_band(y[i], h[i]);
            return UI_HANDLED;
        case UI_KEY_SELECT:
            if (!in->release || in->long_press)
                return UI_HANDLED;
            if (ss->idx >= 0)
            {
                set_pick(c, ss->idx, ss->list.selected);
                return UI_POP;          /* picks and goes back */
            }
            idx = ss->list.selected;
            if (set_off(c, idx))
                return UI_HANDLED;      /* greyed out: nothing to change */
            lim = set_lim(&c->sets->s[idx]);
            if (lim > 3)
            {
                /* more than 3 values: their own list (section 9.2) */
                ui_push_t(&sdk_settings_screen, (const void *)(intptr_t)
                          (idx + 1), UI_T_PUSH);
                return UI_HANDLED;
            }
            /* 2 or 3 values step in place, as the stock Shuffle and
             * Repeat rows */
            set_pick(c, idx, (c->set_val[idx] + 1) % lim);
            for (i = 0; i < c->set_n; i++)
                if ((i == idx || c->sets->s[i].off_if == idx + 1)
                    && ui_list_row_band(&ss->list, i, &y[0], &h[0]))
                    ui_invalidate_band(y[0], h[0]);    /* and its dependents */
            return UI_HANDLED;
        default:
            return UI_IGNORED;          /* MENU goes back */
    }
}

static int set_tick(struct ui_screen *s)
{
    struct set_state *ss = s->priv;

    return ss ? ui_list_tick(&ss->list) : TIMEOUT_BLOCK;
}

const struct ui_screen_ops sdk_settings_screen = {
    .name  = "sdk settings",
    .enter = set_enter,
    .leave = set_leave,
    .draw  = set_draw,
    .input = set_input,
    .tick  = set_tick,
};

void sdk_area(struct sdk_ctx *c, int *x, int *y, int *w, int *h)
{
    API_V(c);
    *x = 0;
    *y = sdki_area_top();
    *w = LCD_WIDTH;
    *h = LCD_HEIGHT - sdki_area_top();
    sdki_leave_api(c);
}

/* -- memory, time, feedback ------------------------------------------------- */
void *sdk_work(struct sdk_ctx *c, size_t bytes)
{
    void *p = NULL;

    API(c, NULL);
    if (c->phase != P_INIT || c->work_taken || bytes > c->work_cap)
        sdki_viol(c, "work", (long)bytes);
    else
    {
        c->work_taken = true;
        p = c->work;
    }
    sdki_leave_api(c);
    return p;
}

void *sdk_borrow(struct sdk_ctx *c, size_t bytes)
{
    (void)c; (void)bytes;
    return NULL;                        /* Class B: #108 + P19 */
}

bool sdk_boost_burst(struct sdk_ctx *c, int ms)
{
    bool ok = false;

    API(c, false);
    if (!c->boosted && ms > 0 && ui_boost_burst(ms))
        ok = c->boosted = true;
    sdki_leave_api(c);
    return ok;
}

void sdk_boost_end(struct sdk_ctx *c)
{
    API_V(c);
    if (c->boosted)
    {
        ui_boost_burst_end();
        c->boosted = false;
    }
    sdki_leave_api(c);
}

void sdk_click(struct sdk_ctx *c)
{
    API_V(c);
    if (sdk_rate_take(&c->click_rate, NOW_MS(), 1000, CLICKS_MAX))
        keyclick_click(true, BUTTON_SELECT);
    sdki_leave_api(c);
}

bool sdk_music_playing(struct sdk_ctx *c)
{
    bool r;

    API(c, false);
    r = (audio_status() & (AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE))
        == AUDIO_STATUS_PLAY;
    sdki_leave_api(c);
    return r;
}

int sdk_wheel_mode(struct sdk_ctx *c, enum sdk_wheel m)
{
    int rc = SDK_E_CLASS;

    API(c, SDK_E_ARG);
    if (m == SDK_WHEEL_REL || m == SDK_WHEEL_REL_RAW)
    {
        c->wheel_raw = m == SDK_WHEEL_REL_RAW;
        rc = SDK_OK;
    }
    sdki_leave_api(c);
    return rc;
}

int sdk_long_menu(struct sdk_ctx *c, enum sdk_longmenu m)
{
    int rc = SDK_E_ARG;

    API(c, SDK_E_ARG);
    if (m == SDK_LONGMENU_EXIT || m == SDK_LONGMENU_PAUSE)
    {
        c->long_pause = m == SDK_LONGMENU_PAUSE;
        rc = SDK_OK;
    }
    sdki_leave_api(c);
    return rc;
}

#ifdef SIMULATOR
int sim_wheel_status(void);             /* sim_cmd_extras.c: `wheel_at` */
#endif

int sdki_wheel_raw(void)
{
#ifdef HAVE_WHEEL_POSITION
    return sdk_wheel_norm(wheel_status());
#elif defined(SIMULATOR)
    return sdk_wheel_norm(sim_wheel_status());
#else
    return -1;
#endif
}

uint32_t sdki_wheel_touches(void)
{
    return button_wheel_touches();
}

/* Q30's default (owner, 2026-10-24, #132): a polled, read-only position
 * in every class; the driver and the relative stream are untouched */
int sdk_wheel_pos(struct sdk_ctx *c, int *pos, bool *touched)
{
    int p;

    API(c, SDK_E_ARG);
    if (!c->wheel_watch)
    {
        c->wheel_watch = true;
        c->ww.touches = sdki_wheel_touches();
        UPDATE_TRACE("sdk wheel pos %s", c->name);
    }
    p = sdki_wheel_raw();
    if (pos)
        *pos = p;
    if (touched)
        *touched = p >= 0;
    sdki_leave_api(c);
    return SDK_OK;
}

void sdk_log(struct sdk_ctx *c, const char *fmt, ...)
{
    char buf[101];
    va_list ap;

    API_V(c);
    if (sdk_rate_take(&c->log_rate, NOW_MS(), 1000, LOGS_MAX))
    {
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        ui_log("sdk", "%s %s", c->name, buf);
    }
    sdki_leave_api(c);
}

/* -- the state record and settings ----------------------------------------- */
void *sdk_state(struct sdk_ctx *c, uint16_t *len)
{
    void *p;

    API(c, NULL);
    if (len)
        *len = c->rec_len;
    p = c->state_img + SDK_REC_OFF(c->set_n);
    sdki_leave_api(c);
    return p;
}

int sdk_state_commit(struct sdk_ctx *c, uint16_t len)
{
    int rc = SDK_OK;

    API(c, SDK_E_ARG);
    if (len > c->state_cap - SDK_STATE_HDR - c->set_n)
    {
        sdki_viol(c, "state", len);
        rc = SDK_E_ARG;
    }
    else
    {
        c->rec_len = len;
        c->rec_dirty = true;            /* written at suspend/exit */
    }
    sdki_leave_api(c);
    return rc;
}

int sdk_setting(struct sdk_ctx *c, unsigned i)
{
    int v = 0;

    API(c, 0);
    if (i < c->set_n)
        v = c->set_val[i];
    sdki_leave_api(c);
    return v;
}

bool sdk_setting_changed(struct sdk_ctx *c, unsigned i)
{
    bool r = false;

    API(c, false);
    if (i < c->set_n)
    {
        r = (c->set_changed >> i) & 1;
        c->set_changed &= (uint16_t)~(1u << i);
    }
    sdki_leave_api(c);
    return r;
}

/* button events for the game (SELECT; MENU's long press, opted in) */
void sdki_ev_add_btn(struct sdk_ctx *c, int btn, int kind)
{
    if (c->in.n >= SDK_IN_EV_MAX)
    {
        memmove(&c->in.ev[0], &c->in.ev[1],
                sizeof(c->in.ev[0]) * (SDK_IN_EV_MAX - 1));
        c->in.n--;
        c->in.flags |= SDK_INF_DROPPED;
    }
    c->in.ev[c->in.n].btn = (uint8_t)btn;
    c->in.ev[c->in.n].kind = (uint8_t)kind;
    c->in.ev[c->in.n].at_ms = (uint16_t)NOW_MS();
    c->in.n++;
    c->in_pending = true;
}

void sdki_ev_add(struct sdk_ctx *c, int kind)
{
    sdki_ev_add_btn(c, SDK_BTN_SELECT, kind);
}

enum ui_result sdki_game_input(struct ui_screen *s,
                                 const struct ui_input *in)
{
    struct sdk_ctx *c = s->priv;
    bool sel_ev = in->key == UI_KEY_SELECT
                  || (in->key == UI_KEY_NONE && (in->raw & BUTTON_SELECT));

    if (!c || c != sdk_cur || c->phase != P_RUN)
        return UI_HANDLED;
    /* the exit chord withholds SELECT and PREV, and every event while
     * they overlap (a reserved input never reaches the game) */
    if (sdki_chord_feed(c) && (sel_ev || in->key == UI_KEY_PREV
                          || in->key == UI_KEY_NONE))
        return UI_HANDLED;
    if (c->phase != P_RUN)
        return UI_HANDLED;
    /* a button's press (the firmware's too): a finger on the ring now is
     * that press, not a tap (SDK_INF_PRESSED) */
    if (in->key != UI_KEY_WHEEL && in->key != UI_KEY_NONE && !in->release
        && !in->repeat)
        c->in_flags |= SDK_INF_PRESSED;
    switch (in->key)
    {
        case UI_KEY_MENU:
            /* Class A: short MENU is the game's back(), at release; a
             * long MENU (500 ms) leaves (Q29), or, opted in
             * (SDK_LONGMENU_PAUSE), is the game's pause event */
            if (in->long_press && !in->release && !c->menu_long)
            {
                c->menu_long = true;
                UPDATE_TRACE("sdk long menu %s%s", c->name,
                             c->long_pause ? " pause" : "");
                if (c->long_pause)
                    sdki_ev_add_btn(c, SDK_BTN_MENU, SDK_IN_LONG);
                else
                    sdki_run_exit(c, SDK_WHY_USER);
            }
            else if (in->release)
            {
                if (!c->menu_long)
                {
                    int r = sdki_call_game(c, E_BACK, SLICE_A_US, 0);

                    if (r == KILLED)
                        sdki_kill_now(c);
                    else if (!r)
                        sdki_run_exit(c, SDK_WHY_USER);
                    else
                        c->in_pending = true;
                }
                c->menu_long = false;
            }
            return UI_HANDLED;
        case UI_KEY_SELECT:
            if (c->menu_open)
            {
                if (in->release && !in->long_press)
                {
                    UPDATE_TRACE("sdk menu pick %s row=%d", c->name,
                                 c->menu_sel);
                    if (c->menu_rows[c->menu_sel] == SDK_STR_EXIT)
                    {
                        /* the SDK's own row: the graceful exit */
                        sdki_run_exit(c, SDK_WHY_USER);
                        return UI_HANDLED;
                    }
                    c->in.menu_pick = (int8_t)c->menu_sel;
                    sdki_menu_close(c);
                    c->in_pending = true;
                }
                return UI_HANDLED;
            }
            if (in->release)
            {
                c->sel_down = false;
                c->long_sent = false;
                sdki_ev_add(c, SDK_IN_RELEASE);
            }
            else if (in->repeat)
            {
                /* SDK_IN_LONG once per hold, at 500 ms; the repeats
                 * that follow are SDK_IN_REPEAT */
                bool lg = in->long_press && !c->long_sent;

                c->long_sent = c->long_sent || lg;
                sdki_ev_add(c, lg ? SDK_IN_LONG : SDK_IN_REPEAT);
            }
            else
            {
                c->sel_down = true;
                c->long_sent = false;
                sdki_ev_add(c, SDK_IN_PRESS);
            }
            return UI_HANDLED;
        case UI_KEY_WHEEL:
            if (c->menu_open)
                sdki_menu_move(c, in->wheel_steps > 0 ? 1 : -1);
            else
            {
                /* SDK_WHEEL_REL_RAW: one driver click per event, no
                 * acceleration (the game scales it itself) */
                int d = !c->wheel_raw ? in->wheel_steps
                                      : in->wheel_steps > 0 ? 1 : -1;

                c->in.steps = (int16_t)(c->in.steps + d);
                c->in.velocity = (uint16_t)in->wheel_velocity;
                c->in_pending = true;
            }
            return UI_HANDLED;
        case UI_KEY_PREV:
            /* L5: its long press starts the firmware's seek (ui_main.c
             * skip_or_seek()) only with SELECT up (the chord swallowed
             * it above otherwise); from then on this PREV is no part of
             * a chord (sdki_chord_feed()) */
            if (in->long_press && !in->release && audio_status())
            {
                c->prev_seek = true;
                UPDATE_TRACE("sdk prev seek %s", c->name);
            }
            else if (in->release)
                c->prev_seek = false;
            return UI_IGNORED;
        default:
            /* PLAY, NEXT and PREV stay the firmware's (Class A) */
            return UI_IGNORED;
    }
}

int sdk_screen_mod(const struct ui_screen_ops *ops)
{
    return sdk_cur && ops == &sdk_game_screen ? sdk_cur->mod : -1;
}

bool sdk_takes_long_menu(void)
{
    struct ui_screen *t = ui_top();

    return sdk_cur && t && t->ops == &sdk_game_screen && sdk_cur->phase == P_RUN;
}
