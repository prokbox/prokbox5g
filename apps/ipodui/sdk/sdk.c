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

/* The game SDK's runtime, Class A (docs/design/sdk.md rev 0, W4-0).
 *
 * One game runs at a time, as the screen sdk_game_screen. Its module is
 * pinned in the arena while it runs; its one working block (the
 * manifest's work) holds, in order: this file's struct sdk_ctx, the
 * .state image (header, settings, the game's record) and the game's own
 * sdk_work() block. Nothing else is allocated, and the screen arena
 * keeps only a pointer. Every call into the game goes through
 * sdki_call_game(): a restore point, the slice clock (game time only: it
 * stops inside SDK calls) and the watchdog's tick task. A kill unwinds to
 * the restore point from an SDK boundary (the flag path), or, in the
 * simulator, from a signal while the game's own code runs (the model of
 * the target's IRQ unwind, which is the device prototype's job). */
/* not ownable (file): a full-screen page or takeover; the drift panel was settled on the way in (ui_panel_hide(), or the takeover's own ui_fb_settle()), and a full lcd_update() releases it (bcmjob_owned_hook) */
#include "config.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "button.h"
#include "backlight.h"
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
#include "ipodui/sdk/sdk.h"
#include "ipodui/sdk/sdk_core.h"
#include "ipodui/sdk/sdk_int.h"
#include "ipodui/sdk/sdk_priv.h"

struct sdk_ctx *sdk_cur;
static const char *const entry_names[] = { "init", "step", "back",
                                           "suspend", "resume", "exit" };

/* -- time ------------------------------------------------------------------- */
static void clock_stop(struct sdk_ctx *c)
{
    if (c->in_game && c->sdk_depth == 0)
        c->game_us += NOW_US() - c->seg_t0;
}

static void clock_go(struct sdk_ctx *c)
{
    c->seg_t0 = NOW_US();
}

static void unwind(struct sdk_ctx *c)
{
    c->sdk_depth = 0;
    SDK_LONGJMP(c->jb);
}

/* Every SDK call made by the game: the clock stops while the SDK runs,
 * and a raised kill flag unwinds here (an SDK boundary: no lock held) */
bool sdki_enter_api(struct sdk_ctx *c)
{
    if (!c || c != sdk_cur)
        return false;
    if (c->in_game && c->kill)
        unwind(c);
    clock_stop(c);
    c->sdk_depth++;
    return true;
}

void sdki_leave_api(struct sdk_ctx *c)
{
    if (c->sdk_depth)
        c->sdk_depth--;
    if (c->in_game && c->kill)
        unwind(c);
    if (c->sdk_depth == 0)
        clock_go(c);
}

/* -- the watchdog (section 11.3) ------------------------------------------ */
#ifdef SIMULATOR
void sdki_wd_signal(int sig)
{
    struct sdk_ctx *c = sdk_cur;

    (void)sig;
    /* the model of the IRQ unwind: only on the UI thread, only while
     * the game's own code runs (never inside an SDK call) */
    if (c && c->in_game && c->kill && c->sdk_depth == 0
        && pthread_equal(pthread_self(), c->ui_thread))
        unwind(c);
}
#endif

void sdki_wd_tick(void)
{
    struct sdk_ctx *c = sdk_cur;
    uint32_t used, limit;

    if (!c || !c->in_game)
        return;
    used = c->game_us + (c->sdk_depth ? 0 : NOW_US() - c->seg_t0);
    limit = c->entry == E_EXIT ? c->budget_us : c->budget_us * 10u;
    if (!c->kill && used > limit)
    {
        c->kill = 1;
        c->kill_rule = c->entry;
        c->kill_t0 = NOW_MS();
    }
    if (!c->kill)
        return;
#ifdef SIMULATOR
    if (c->sdk_depth == 0)
        pthread_kill(c->ui_thread, SIGUSR2);
#endif
    if (NOW_MS() - c->kill_t0 > REBOOT_MS)
    {
        /* the flag unanswered with interrupts on: a game spinning
         * outside every SDK boundary. The controlled reboot (the resume
         * point is saved by the shutdown path's own status save). */
        c->kill_t0 = NOW_MS();
        ui_log("sdk", "kill %s watchdog reboot", c->name);
#ifndef SIMULATOR
        system_reboot();
#endif
    }
}

/* -- calls into the game ---------------------------------------------------- */
static void set_cell(struct sdk_ctx *c)
{
    if (c->cell)
        *c->cell = (void *)sdk_table;   /* again before every call: .bss
                                           is zeroed when a module moves */
}

/* returns: the entry's result (init/back), 0, or -1000 killed */
int sdki_call_game(struct sdk_ctx *c, int entry, uint32_t budget,
                     int arg)
{
    volatile int rc = 0;
    int j;

    set_cell(c);
    c->entry = (uint8_t)entry;
    c->budget_us = budget;
    c->game_us = 0;
    c->kill = 0;
    c->sdk_depth = 0;
    c->call_t0 = NOW_US();
    if (SDK_SETJMP(c->jb))
    {
        c->in_game = 0;
        c->killed = true;
        ui_log("sdk", "kill %s %s us=%lu", c->name,
               entry_names[c->kill_rule < 6 ? c->kill_rule : 1],
               (unsigned long)(NOW_US() - c->call_t0));
        return KILLED;
    }
    clock_go(c);
    c->in_game = 1;
    switch (entry)
    {
        case E_INIT:    rc = c->g->init(c); break;
        case E_STEP:    rc = c->g->step(c, &c->in, NOW_MS()); break;
        case E_BACK:    rc = c->g->back ? c->g->back(c) : 0; break;
        case E_SUSPEND: if (c->g->suspend) c->g->suspend(c, arg); break;
        case E_RESUME:  if (c->g->resume) c->g->resume(c); break;
        case E_EXIT:    if (c->g->exit) c->g->exit(c, arg); break;
    }
    c->in_game = 0;
    c->game_us += NOW_US() - c->seg_t0;
    if (entry == E_EXIT)
        return rc;                      /* the deadline is the watchdog's */
    j = sdk_slice_judge(c->game_us, budget, &c->viols);
    if (j != SDK_J_OK)
    {
        ui_log("sdk", "%s %s %s us=%lu", j == SDK_J_KILL ? "kill" : "viol",
               c->name, entry_names[entry], (unsigned long)c->game_us);
        if (j == SDK_J_KILL)
        {
            c->killed = true;
            return KILLED;
        }
    }
    return rc;
}

void sdki_viol(struct sdk_ctx *c, const char *rule, long v)
{
    ui_log("sdk", "viol %s %s %ld", c->name, rule, v);
    if (c->viols++ >= 1 && c->in_game)
    {
        c->kill = 1;                    /* a repeat: the next boundary */
        c->kill_rule = c->entry;
        c->kill_t0 = NOW_MS();
    }
}

/* -- drawing --------------------------------------------------------------- */
int sdki_area_top(void) { return ui_theme->header_h; }

void sdki_mark(struct sdk_ctx *c, int y, int h)
{
    sdk_bands_add(&c->bands, y, h, sdki_area_top(), LCD_HEIGHT);
}

/* clips a rect to the play area; false if nothing is left */
static bool clip(int *x, int *y, int *w, int *h, int *dx, int *dy)
{
    int top = sdki_area_top();

    *dx = *dy = 0;
    if (*x < 0) { *dx = -*x; *w += *x; *x = 0; }
    if (*y < top) { *dy = top - *y; *h -= *dy; *y = top; }
    if (*x + *w > LCD_WIDTH) *w = LCD_WIDTH - *x;
    if (*y + *h > LCD_HEIGHT) *h = LCD_HEIGHT - *y;
    return *w > 0 && *h > 0;
}

/* a source must lie in the game's image or its working block */
bool sdki_src_ok(struct sdk_ctx *c, const void *p, size_t bytes)
{
    uintptr_t a = (uintptr_t)p;

    if (a < 0x10000u)                   /* never memory a game owns */
        return false;
    if (a >= (uintptr_t)c->work && a + bytes <= (uintptr_t)c->work
                                                 + c->work_cap)
        return true;
    if (!c->img_hi)
        return true;                    /* static build: no image range */
    return a >= c->img_lo && a + bytes <= c->img_hi;
}

void sdki_push_bands(struct sdk_ctx *c)
{
    uint32_t now = NOW_MS();
    int i;

    if (now - c->push_t0 >= 1000)
    {
        if (c->push_bytes > PUSH_BPS_MAX)
            sdki_viol(c, "pushbytes", (long)c->push_bytes);
        if (c->pushes > PUSHES_MAX)
            sdki_viol(c, "pushes", c->pushes);
        c->push_t0 = now;
        c->push_bytes = 0;
        c->pushes = 0;
    }
    if (!c->bands.n)
        return;
    for (i = 0; i < c->bands.n; i++)
        ui_lcd_push(i ? "sdk+" : "sdk", 0, c->bands.y[i], LCD_WIDTH,
                    c->bands.h[i]);
    c->push_bytes += (uint32_t)sdk_bands_rows(&c->bands) * LCD_WIDTH * 2;
    c->pushes++;
    c->bands.n = 0;
}

void sdk_fill(struct sdk_ctx *c, int x, int y, int w, int h, sdk_px col)
{
    int dx, dy;

    API_V(c);
    if (c->draw_ok && clip(&x, &y, &w, &h, &dx, &dy))
    {
        lcd_set_viewport(NULL);
        lcd_set_drawmode(DRMODE_SOLID);
        lcd_set_foreground(col);
        lcd_fillrect(x, y, w, h);
    }
    sdki_leave_api(c);
}

void sdk_rows(struct sdk_ctx *c, int x, int y, int w, int h,
              const sdk_px *row_col)
{
    int dx, dy, r;

    API_V(c);
    if (!sdki_src_ok(c, row_col, (size_t)(h > 0 ? h : 0) * 2))
        sdki_viol(c, "arg", 0);
    else if (c->draw_ok && clip(&x, &y, &w, &h, &dx, &dy))
    {
        lcd_set_viewport(NULL);
        lcd_set_drawmode(DRMODE_SOLID);
        for (r = 0; r < h; r++)
        {
            lcd_set_foreground(row_col[dy + r]);
            lcd_hline(x, x + w - 1, y + r);
        }
    }
    sdki_leave_api(c);
}

static void blit(struct sdk_ctx *c, const sdk_px *src, int sx, int sy,
                 int stride, int x, int y, int w, int h, bool keyed,
                 sdk_px key)
{
    int dx, dy, r, k;

    if (stride < w || sx < 0 || sy < 0
        || !sdki_src_ok(c, src + sy * stride + sx,
                   (size_t)((h - 1) * stride + w) * 2))
    {
        sdki_viol(c, "arg", 0);
        return;
    }
    if (!c->draw_ok || !clip(&x, &y, &w, &h, &dx, &dy))
        return;
    for (r = 0; r < h; r++)
    {
        const sdk_px *s = src + (sy + dy + r) * stride + sx + dx;
        fb_data *d = FBADDR(x, y + r);

        if (!keyed)
            memcpy(d, s, (size_t)w * 2);
        else
            for (k = 0; k < w; k++)
                if (s[k] != key)
                    d[k] = s[k];
    }
}

void sdk_bitmap(struct sdk_ctx *c, const sdk_px *src, int src_x, int src_y,
                int stride, int x, int y, int w, int h)
{
    API_V(c);
    blit(c, src, src_x, src_y, stride, x, y, w, h, false, 0);
    sdki_leave_api(c);
}

void sdk_bitmap_key(struct sdk_ctx *c, const sdk_px *src, int src_x,
                    int src_y, int stride, int x, int y, int w, int h,
                    sdk_px key)
{
    API_V(c);
    blit(c, src, src_x, src_y, stride, x, y, w, h, true, key);
    sdki_leave_api(c);
}

static unsigned mat_row(enum sdk_mat m, int r, int h)
{
    static const uint8_t map[SDK_MAT__N] = {
        UI_MAT_GEL_BLUE, UI_MAT_GEL_GREEN, UI_MAT_GEL_RED, UI_MAT_GEL_SKY,
        UI_MAT_METAL, UI_MAT_GROOVE, 0,
    };

    if (m == SDK_MAT_SELECT)
        return ui_paint_selection_row(r, h);
    return ui_mat_color((enum ui_mat)map[m], r, h);
}

void sdk_material(struct sdk_ctx *c, enum sdk_mat m, int x, int y,
                  int w, int h)
{
    int dx, dy, r, h0 = h;

    API_V(c);
    if ((unsigned)m >= SDK_MAT__N)
        sdki_viol(c, "arg", m);
    else if (c->draw_ok && clip(&x, &y, &w, &h, &dx, &dy))
    {
        lcd_set_viewport(NULL);
        lcd_set_drawmode(DRMODE_SOLID);
        for (r = 0; r < h; r++)
        {
            lcd_set_foreground(mat_row(m, dy + r, h0));
            lcd_hline(x, x + w - 1, y + r);
        }
    }
    sdki_leave_api(c);
}

void sdk_mask(struct sdk_ctx *c, const uint8_t *mask, int stride_bytes,
              int x, int y, int w, int h, enum sdk_mat m)
{
    int dx, dy, r, k, h0 = h;

    API_V(c);
    if ((unsigned)m >= SDK_MAT__N || stride_bytes * 8 < w
        || !sdki_src_ok(c, mask, (size_t)stride_bytes * (h > 0 ? h : 0)))
        sdki_viol(c, "arg", 0);
    else if (c->draw_ok && clip(&x, &y, &w, &h, &dx, &dy))
        for (r = 0; r < h; r++)
        {
            const uint8_t *s = mask + (dy + r) * stride_bytes;
            fb_data col = (fb_data)mat_row(m, dy + r, h0);
            fb_data *d = FBADDR(x, y + r);

            for (k = 0; k < w; k++)
                if (s[(dx + k) >> 3] & (0x80 >> ((dx + k) & 7)))
                    d[k] = col;
        }
    sdki_leave_api(c);
}

const char *sdki_str_of(struct sdk_ctx *c, int id)
{
    static const uint16_t std_words[SDK_STR__END - SDK_STR_BASE] = {
        LANG_IPODUI_X_RESUME, LANG_IPODUI_X_NEW_GAME, LANG_SETTINGS,
        LANG_IPODUI_X_EXIT, LANG_ON, LANG_OFF,
    };

    if (id >= SDK_STR_BASE && id < SDK_STR__END)
        return str(std_words[id - SDK_STR_BASE]);
    if (c->strs && id >= 0 && id < c->strs->n)
        return str(c->strs->lang[id]);
    return NULL;
}

static int font_of(enum sdk_font f)
{
    return f == SDK_FONT_SMALL ? ui_theme->font_title : ui_theme->font_ui;
}

static int text_at(struct sdk_ctx *c, int x, int y, const char *s,
                   enum sdk_font f, sdk_px col)
{
    int w = 0, h = 0, fnt = font_of(f);

    font_getstringsize((const unsigned char *)s, &w, &h, fnt);
    if (!c->draw_ok || y < sdki_area_top() || y + h > LCD_HEIGHT || x < 0
        || x >= LCD_WIDTH)
        return w;
    lcd_set_viewport(NULL);
    lcd_setfont(fnt);
    lcd_set_drawmode(DRMODE_FG);
    lcd_set_foreground(col);
    lcd_putsxy(x, y, (const unsigned char *)s);
    lcd_set_drawmode(DRMODE_SOLID);
    return w;
}

int sdk_text(struct sdk_ctx *c, int x, int y, int str_id, enum sdk_font f,
             sdk_px col)
{
    const char *s;
    int w = 0;

    API(c, 0);
    s = sdki_str_of(c, str_id);
    if (!s)
        sdki_viol(c, "arg", str_id);
    else
        w = text_at(c, x, y, s, f, col);
    sdki_leave_api(c);
    return w;
}

int sdk_text_num(struct sdk_ctx *c, int x, int y, int32_t n,
                 enum sdk_font f, sdk_px col)
{
    char buf[12];
    int w;

    API(c, 0);
    snprintf(buf, sizeof(buf), "%ld", (long)n);
    w = text_at(c, x, y, buf, f, col);
    sdki_leave_api(c);
    return w;
}

int sdk_text_fmt(struct sdk_ctx *c, int x, int y, int str_id, int32_t arg,
                 enum sdk_font f, sdk_px col)
{
    char buf[64];
    const char *s;
    int w = 0;

    API(c, 0);
    s = sdki_str_of(c, str_id);
    if (!s)
        sdki_viol(c, "arg", str_id);
    else
    {
        const char *p = strstr(s, "%d");

        /* one %d, nothing else from the table is a format */
        if (p)
            snprintf(buf, sizeof(buf), "%.*s%ld%s", (int)(p - s), s,
                     (long)arg, p + 2);
        else
            snprintf(buf, sizeof(buf), "%s", s);
        w = text_at(c, x, y, buf, f, col);
    }
    sdki_leave_api(c);
    return w;
}

int sdk_text_width(struct sdk_ctx *c, int str_id, enum sdk_font f)
{
    const char *s;
    int w = 0, h;

    API(c, 0);
    s = sdki_str_of(c, str_id);
    if (s)
        font_getstringsize((const unsigned char *)s, &w, &h, font_of(f));
    sdki_leave_api(c);
    return w;
}

int sdk_num_width(struct sdk_ctx *c, int32_t n, enum sdk_font f)
{
    char buf[12];
    int w = 0, h;

    API(c, 0);
    snprintf(buf, sizeof(buf), "%ld", (long)n);
    font_getstringsize((const unsigned char *)buf, &w, &h, font_of(f));
    sdki_leave_api(c);
    return w;
}

void sdk_dirty(struct sdk_ctx *c, int y, int h)
{
    API_V(c);
    sdki_mark(c, y, h);
    sdki_leave_api(c);
}

uint32_t sdk_now_ms(struct sdk_ctx *c)
{
    uint32_t t;

    API(c, 0);
    t = NOW_MS();
    sdki_leave_api(c);
    return t;
}

uint32_t sdk_now_us(struct sdk_ctx *c)
{
    uint32_t t;

    API(c, 0);
    t = NOW_US();
    sdki_leave_api(c);
    return t;
}

uint32_t sdk_rand32(struct sdk_ctx *c)
{
    uint32_t x;

    API(c, 0);
    x = c->rnd;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    c->rnd = x;
    sdki_leave_api(c);
    return x;
}

int sdk_music_pause(struct sdk_ctx *c) { (void)c; return SDK_E_CLASS; }
int sdk_music_resume(struct sdk_ctx *c) { (void)c; return SDK_E_CLASS; }
int sdk_exit_request(struct sdk_ctx *c) { (void)c; return SDK_E_CLASS; }
int sdk_header(struct sdk_ctx *c, bool shown)
{ (void)c; return shown ? SDK_OK : SDK_E_CLASS; }

/* a full frame: the game redraws everything (resume) */
void sdki_full_frame(struct sdk_ctx *c)
{
    c->draw_ok = true;
    lcd_set_viewport(NULL);
    lcd_set_foreground(LCD_BLACK);
    lcd_fillrect(0, sdki_area_top(), LCD_WIDTH,
                 LCD_HEIGHT - sdki_area_top());
    if (sdki_call_game(c, E_RESUME, SLICE_A_US, 0) == KILLED)
        return sdki_kill_now(c);
    c->in.flags |= SDK_INF_RESUMED;
    if (c->set_news)
        c->in.flags |= SDK_INF_SETTINGS;    /* back from the Settings */
    c->set_news = false;
    c->in_pending = true;               /* a step follows every resume */
    if (c->menu_open)
        sdki_menu_draw(c, true);
    c->redraw = false;
    c->bands.n = 0;                     /* the caller pushes the frame */
}

static void game_draw(struct ui_screen *s, unsigned dirty)
{
    struct sdk_ctx *c = s->priv;

    if (!c || c != sdk_cur)
        return;
    if (c->phase == P_EXIT)
    {
        ui_pop_request(s);              /* killed while covered */
        return;
    }
    if (c->phase == P_RUN && (dirty & UI_DIRTY_BODY))
    {
        c->hold_susp = button_hold();   /* Hold moved while covered */
        if (c->suspended && !c->hold_susp)
        {
            c->suspended = false;       /* uncovered: the resume */
            UPDATE_TRACE("sdk resume %s", c->name);
        }
        sdki_full_frame(c);
    }
}

bool sdki_chord_feed(struct sdk_ctx *c)
{
    int st = button_status();
    int r;

    /* L5: a PREV that became a seek (held past the long press with no
     * SELECT) is the firmware's until it is let go: SELECT joining it
     * later is no exit chord, so a chord exit never lands a seek */
    if (!(st & BUTTON_LEFT))
        c->prev_seek = false;
    r = sdk_chord_feed(&c->chord, (st & BUTTON_SELECT) != 0,
                       (st & BUTTON_LEFT) && !c->prev_seek, NOW_MS(),
                       SDK_EXIT_CHORD_MS);

    if (r & SDK_CHR_BEGIN)
    {
        UPDATE_TRACE("sdk chord begin %s", c->name);
        if (c->sel_down && !c->menu_open)
            sdki_ev_add(c, SDK_IN_CANCEL);  /* drop that press's action */
        c->sel_down = false;
    }
    if (r & SDK_CHR_EXIT)
    {
        UPDATE_TRACE("sdk chord exit %s", c->name);
        sdki_run_exit(c, SDK_WHY_USER);
    }
    return (r & SDK_CHR_ACTIVE) != 0;
}

static void do_step(struct sdk_ctx *c)
{
    int ms, rc;

    c->in.held = (button_status() & BUTTON_SELECT) && !c->menu_open
                 ? SDK_BTN_SELECT : 0;
    c->in.flags |= c->in_flags;
    c->in_flags = 0;
    if (!is_backlight_on(true))
        c->in.flags |= SDK_INF_DARK;
    c->draw_ok = true;
    rc = sdki_call_game(c, E_STEP, SLICE_A_US, 0);
    if (rc == KILLED)
        return sdki_kill_now(c);
    memset(&c->in, 0, offsetof(struct sdk_input, rg));
    c->in.menu_pick = -1;
    c->in.pos = -1;
    c->in_pending = false;
    c->last_step_ms = NOW_MS();
    if (sdki_settings_wanted)
    {
        sdki_settings_wanted = false;
        sdki_push_bands(c);
        if (sdki_suspend(c, SDK_WHY_SETTINGS))
            ui_push_t(&sdk_settings_screen, c, UI_T_PUSH);
        return;
    }
    if (c->redraw)
    {
        sdki_full_frame(c);
        ui_lcd_push("sdk", 0, sdki_area_top(), LCD_WIDTH,
                    LCD_HEIGHT - sdki_area_top());
    }
    sdki_push_bands(c);
    ms = rc;
    c->idle = ms == SDK_IDLE || ms < 0;
    if (!c->idle)
    {
        if (ms < SDK_FRAME_MIN_MS)
            ms = SDK_FRAME_MIN_MS;
        c->next_tick = current_tick + (ms * HZ + 999) / 1000;
    }
}

static int game_tick(struct ui_screen *s)
{
    struct sdk_ctx *c = s->priv;
    int due, wait = TIMEOUT_BLOCK;

    if (!c || c != sdk_cur || c->phase != P_RUN)
        return TIMEOUT_BLOCK;
    if (button_hold() != c->hold_susp && !sdki_hold(c))
        return TIMEOUT_BLOCK;
    if (c->save_due)
        sdki_save_poll(c);
    if (c->suspended)
        return HZ / 4;
    sdki_chord_feed(c);
    if (c->phase != P_RUN)
        return TIMEOUT_BLOCK;
    if (c->wheel_watch)
    {
        /* the ring, for a game that reads it (sdk.md 3.3): a touch posts
         * no event, so poll it */
        int r = sdk_ww_poll(&c->ww, is_backlight_on(true), sdki_wheel_raw(),
                            sdki_wheel_touches());

        if (r & SDK_WW_LIT)
            c->in_flags |= SDK_INF_LIT;
        if ((r & SDK_WW_WAKE) && c->idle)
            c->in_pending = true;
    }
    if (c->in_pending || (!c->idle && !TIME_BEFORE(current_tick,
                                                    c->next_tick)))
        do_step(c);
    if (c->phase != P_RUN || c->suspended)
        return TIMEOUT_BLOCK;
    if (!c->idle)
        wait = MAX(1, c->next_tick - current_tick);
    due = sdk_chord_due(&c->chord, NOW_MS(), SDK_EXIT_CHORD_MS);
    if (due >= 0)
    {
        int t = MAX(1, (due * HZ + 999) / 1000);

        wait = wait == TIMEOUT_BLOCK ? t : MIN(wait, t);
    }
    if (c->save_due)
        wait = wait == TIMEOUT_BLOCK ? HZ : MIN(wait, HZ);
    if (c->wheel_watch)
    {
        int t = (int)((is_backlight_on(true) ? SDK_WW_LIT_MS : SDK_WW_DARK_MS)
                      * HZ / 1000);

        wait = wait == TIMEOUT_BLOCK ? t : MIN(wait, t);
    }
    return wait;
}

const struct ui_screen_ops sdk_game_screen = {
    .name  = "sdk game",
    .enter = sdki_game_enter,
    .leave = sdki_game_leave,
    .draw  = game_draw,
    .input = sdki_game_input,
    .tick  = game_tick,
};
