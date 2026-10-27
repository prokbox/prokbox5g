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

/* Shared by the SDK's two halves (sdk.c, ARM; sdk_cold.c, Thumb-cold):
 * the per-game state and the calls between them. Not seen by games. */
#ifndef IPODUI_SDK_PRIV_H
#define IPODUI_SDK_PRIV_H

#ifdef SIMULATOR
#include <setjmp.h>
#include <signal.h>
#include <pthread.h>
typedef sigjmp_buf sdk_jb;
#define SDK_SETJMP(b)   sigsetjmp(b, 1)
#define SDK_LONGJMP(b)  siglongjmp(b, 1)
#else
typedef void *sdk_jb[5];
#define SDK_SETJMP(b)   __builtin_setjmp(b)
#define SDK_LONGJMP(b)  __builtin_longjmp(b, 1)
#endif

#ifdef USEC_TIMER
#define NOW_US() ((uint32_t)USEC_TIMER)
#else
#define NOW_US() ((uint32_t)current_tick * (1000000u / HZ))
#endif
#define NOW_MS() ((uint32_t)current_tick * (1000u / HZ))

/* Class A budgets (section 6.1) */
#define SLICE_A_US      20000u
#define SLICE_INIT_US   100000u
#define PUSH_BPS_MAX    (25u * 1024u * 30u)     /* 25 KB a frame at 30 fps */
#define PUSHES_MAX      30
#define CLICKS_MAX      20
#define LOGS_MAX        10
#define REBOOT_MS       2000                    /* the flag unanswered */
#define GAMEDATA        ROCKBOX_DIR "/gamedata"

enum { E_INIT, E_STEP, E_BACK, E_SUSPEND, E_RESUME, E_EXIT };
enum { P_INIT, P_RUN, P_EXIT, P_DONE };

struct sdk_ctx {
    int8_t   mod;
    uint8_t  phase, policy;
    uint16_t exit_ms;
    const char *name;                   /* mod_name(): logs */
    const struct sdk_game *g;
    const struct sdk_strings *strs;
    const struct sdk_settings *sets;
    void   **cell;                      /* the stubs' table cell, or NULL */
    uintptr_t img_lo, img_hi;           /* the module image (target) */
    /* the working block */
    uint8_t *state_img;                 /* header + settings + record */
    uint16_t state_cap, rec_len;
    uint8_t *work;
    uint32_t work_cap;
    bool     work_taken, rec_dirty, set_dirty, killed, exiting;
    uint8_t  set_n;
    uint8_t  set_val[SDK_SETTINGS_MAX];
    uint16_t set_changed;
    /* the call in progress */
    volatile uint8_t in_game, kill, sdk_depth;
    uint8_t  entry, viols, kill_rule;
    uint32_t budget_us, game_us, seg_t0, call_t0, kill_t0;
    sdk_jb   jb;
    /* the loop */
    bool     idle, in_pending, suspended, hold_susp, first_frame;
    bool     redraw;                    /* a full frame is owed (resume) */
    bool     save_due;                  /* a suspend's record: written at
                                           the next natural disk wake */
    bool     usb;                       /* a USB session: no file I/O */
    bool     usb_owed;                  /* suspend() due at its end */
    bool     wheel_raw;                 /* SDK_WHEEL_REL_RAW: unaccelerated */
    bool     long_sent;                 /* this hold's SDK_IN_LONG went */
    bool     set_news;                  /* a setting changed: the first
                                           step after carries
                                           SDK_INF_SETTINGS */
    struct ui_screen *scr;              /* the game's screen */
    long     next_tick;
    uint32_t last_step_ms;
    struct sdk_input in;
    struct sdk_chord chord;
    bool     sel_down;                  /* a SELECT press was delivered */
    bool     cancel_sel;
    bool     menu_long;                 /* the long MENU fired */
    bool     long_pause;                /* SDK_LONGMENU_PAUSE: a long MENU
                                           is the game's pause */
    bool     prev_seek;                 /* L5: PREV's hold went to the
                                           firmware's seek: not a chord */
    bool     wheel_watch;               /* the game reads sdk_wheel_pos():
                                           the SDK polls the ring for it */
    uint8_t  in_flags;                  /* SDK_INF_PRESSED/LIT for the
                                           next step (c->in is cleared
                                           after each) */
    struct sdk_wwatch ww;
    /* drawing */
    struct sdk_bands bands;
    bool     draw_ok;
    uint32_t push_t0, push_bytes;
    uint8_t  pushes;
    /* the overlay menu */
    bool     menu_open;
    uint8_t  menu_n, menu_sel;
    uint16_t menu_rows[SDK_MENU_ROWS_MAX];
    /* misc */
    struct sdk_rate click_rate, log_rate;
    uint32_t rnd;
    bool     boosted;
#ifdef SIMULATOR
    pthread_t ui_thread;
#endif
};

/* the one running game (lean rule 9: can't share because the tick task
 * and the core's hooks must find it); 4 B, in sdk.c */
extern struct sdk_ctx *sdk_cur;

#define KILLED (-1000)              /* sdki_call_game(): the game was killed */

/* sdk.c (ARM, per frame) */
bool sdki_enter_api(struct sdk_ctx *c);
void sdki_leave_api(struct sdk_ctx *c);
#define API(c, ret) do { if (!sdki_enter_api(c)) return ret; } while (0)
#define API_V(c) do { if (!sdki_enter_api(c)) return; } while (0)
void sdki_wd_tick(void);
#ifdef SIMULATOR
void sdki_wd_signal(int sig);
#endif
int  sdki_call_game(struct sdk_ctx *c, int entry, uint32_t budget, int arg);
void sdki_viol(struct sdk_ctx *c, const char *rule, long v);
int  sdki_area_top(void);
void sdki_mark(struct sdk_ctx *c, int y, int h);
bool sdki_src_ok(struct sdk_ctx *c, const void *p, size_t bytes);
void sdki_push_bands(struct sdk_ctx *c);
const char *sdki_str_of(struct sdk_ctx *c, int id);
bool sdki_chord_feed(struct sdk_ctx *c);
/* the driver's absolute wheel position (0..95, else -1) and its touch
 * count; the simulator's come from the script (sim_cmd_extras.c) */
int  sdki_wheel_raw(void);
uint32_t sdki_wheel_touches(void);
void sdki_full_frame(struct sdk_ctx *c);

/* sdk_cold.c (Thumb, on a press or once) */
extern bool sdki_settings_wanted;
void sdki_state_write(struct sdk_ctx *c, const char *why);
void sdki_menu_draw(struct sdk_ctx *c, bool dim);
void sdki_menu_close(struct sdk_ctx *c);
void sdki_menu_move(struct sdk_ctx *c, int d);
void sdki_run_exit(struct sdk_ctx *c, enum sdk_why why);
void sdki_kill_now(struct sdk_ctx *c);
int  sdki_game_enter(struct ui_screen *s, const void *arg);
void sdki_game_leave(struct ui_screen *s);
enum ui_result sdki_game_input(struct ui_screen *s, const struct ui_input *in);
/* suspend() once, paired with the resume() of the next full frame; the
 * record is written at the next natural disk wake (sdki_save_poll()).
 * false: the game was killed */
bool sdki_suspend(struct sdk_ctx *c, enum sdk_why why);
/* the Hold switch moved: suspend, or resume with a full frame; false:
 * killed */
bool sdki_hold(struct sdk_ctx *c);
void sdki_ev_add(struct sdk_ctx *c, int kind);
void sdki_ev_add_btn(struct sdk_ctx *c, int btn, int kind);
void sdki_save_poll(struct sdk_ctx *c);

#endif /* IPODUI_SDK_PRIV_H */
