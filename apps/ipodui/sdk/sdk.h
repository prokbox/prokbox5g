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

/* ProkBox game SDK, ABI 0.1 (docs/design/sdk.md rev 0).
 *
 * A game is written against this header alone. It links only against the
 * SDK table (sdk.sym) plus libgcc; any other symbol fails the link and
 * `modpak.py check` (the allow list). Every call is made on the UI thread
 * by the SDK, returns within its slice, and never blocks.
 *
 * Built in P12 (W4-0): Class A. Class B/C calls are declared with their
 * final signatures and return SDK_E_CLASS or SDK_E_UNAVAIL until their
 * phase lands (sdk.md section 0). */
#ifndef IPODUI_SDK_H
#define IPODUI_SDK_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "ipodui/modload_core.h"    /* struct mod_desc (export slot 0) */

struct sdk_ctx;                     /* opaque: the SDK's per-game state */

/* -- lifecycle (section 2) ---------------------------------------------- */
enum sdk_why {
    SDK_WHY_USER,       /* the exit chord, back() == false, a long MENU */
    SDK_WHY_HOLD,       /* the Hold switch (suspend) */
    SDK_WHY_ALARM,      /* an alarm rings on top (suspend) */
    SDK_WHY_SLEEP,      /* soft sleep (suspend) or power-off (exit) */
    SDK_WHY_TIMER,      /* the Sleep Timer ended (suspend) */
    SDK_WHY_SETTINGS,   /* the SDK's settings screen is on top (suspend) */
    SDK_WHY_VIDEO,      /* a cutscene plays (suspend) */
    SDK_WHY_USB,        /* a USB data session (exit) */
    SDK_WHY_BATTERY,    /* low battery (exit) */
    SDK_WHY_SYSTEM,     /* another system screen covers the game, e.g.
                           the lock screen (suspend); append only */
};
#define SDK_IDLE            (-1)
#define SDK_FRAME_MIN_MS    33      /* 30 fps ceiling */
#define SDK_EXIT_CHORD_MS   2000    /* SELECT + PREV held */

struct sdk_input;
struct sdk_settings;

struct sdk_game {
    int  (*init)(struct sdk_ctx *c);
    int  (*step)(struct sdk_ctx *c, const struct sdk_input *in,
                 uint32_t now_ms);
    bool (*back)(struct sdk_ctx *c);
    void (*suspend)(struct sdk_ctx *c, enum sdk_why why);
    void (*resume)(struct sdk_ctx *c);
    void (*exit)(struct sdk_ctx *c, enum sdk_why why);
    const struct sdk_settings *settings;
};

/* The game's string table (section 4.3), export slot 3. Rev 0 (our own
 * games, built with the firmware): each entry is a firmware language id,
 * so every word goes through the language system; entry 0 is the name. */
struct sdk_strings {
    uint16_t n;
    const uint16_t *lang;
};

/* Export slots of an SDK module */
enum { SDK_SLOT_DESC = 0, SDK_SLOT_GAME = 1, SDK_SLOT_CELL = 2,
       SDK_SLOT_STRINGS = 3, SDK_SLOT__N };

/* SDK_DESC_A(name, work bytes, state bytes, quota KB, exit policy,
 * exit ms): export slot 0, the same values as the manifest (`modpak.py
 * check` refuses a mismatch). state rides in mod_desc.state. */
#define SDK_DESC_A(name, work, st, quota, policy, ms) \
    const struct mod_desc mod_desc_##name = { \
        .magic = MOD_HDR_MAGIC, .abi_major = SDK_ABI_MAJOR, \
        .abi_minor = SDK_ABI_MINOR, .mclass = MOD_CLASS_A, \
        .min_ram = MOD_RAM_32, .kind = MOD_KIND_SDK, \
        .exit_policy = (policy), .exit_ms = (ms), \
        .work_kb = (uint16_t)(((work) + 1023) / 1024), \
        .quota_kb = (quota), \
        .state = (uint16_t)(st) }

/* SDK_GAME(name, strings) = { .init = ..., ... };
 * Places the game at export slot 1 (a module's .modexports; the
 * simulator links games statically as mod_exports_<name>). */
#ifdef IPODUI_IN_MODULE
extern void *sdk_tab_cell;          /* the stubs' table pointer (sdk_stubs.S) */
#define SDK_EXPORTS_(name) const void *const mod_exports[SDK_SLOT__N] \
    __attribute__((section(".modexports"), used))
#define SDK_CELL_ (&sdk_tab_cell)
#else
#define SDK_EXPORTS_(name) const void *const mod_exports_##name[SDK_SLOT__N]
#define SDK_CELL_ NULL
#endif
#define SDK_GAME(name, strings) \
    extern const struct sdk_game sdk_game_##name; \
    SDK_EXPORTS_(name) = { &mod_desc_##name, &sdk_game_##name, SDK_CELL_, \
                           &(strings) }; \
    const struct sdk_game sdk_game_##name

/* B/C: leave the game; Class A: SDK_E_CLASS (it exits through MENU) */
int      sdk_exit_request(struct sdk_ctx *c);
/* B/C: hide the header bar; Class A: SDK_E_CLASS */
int      sdk_header(struct sdk_ctx *c, bool shown);
uint32_t sdk_now_ms(struct sdk_ctx *c);
uint32_t sdk_now_us(struct sdk_ctx *c);
uint32_t sdk_rand32(struct sdk_ctx *c);
/* Class A: one boost burst of <= 100 ms; false = refused */
bool     sdk_boost_burst(struct sdk_ctx *c, int ms);
void     sdk_boost_end(struct sdk_ctx *c);

/* -- input (section 3) --------------------------------------------------- */
enum sdk_btn {
    SDK_BTN_SELECT = 1 << 0,
    SDK_BTN_MENU   = 1 << 1,        /* B/C; A: SDK_IN_LONG only, with
                                       SDK_LONGMENU_PAUSE */
    SDK_BTN_PLAY   = 1 << 2,        /* B/C only, SDK_IN_TAP only */
    SDK_BTN_NEXT   = 1 << 3,        /* B/C only */
    SDK_BTN_PREV   = 1 << 4,        /* B/C only */
};
/* SDK_IN_LONG: once per hold, at 500 ms; the hold's later repeats are
 * SDK_IN_REPEAT (sdk.md 3.2) */
enum sdk_in_kind { SDK_IN_PRESS, SDK_IN_RELEASE, SDK_IN_REPEAT, SDK_IN_LONG,
                   SDK_IN_TAP, SDK_IN_CANCEL };
struct sdk_btn_ev { uint8_t btn, kind; uint16_t at_ms; };
struct sdk_region_ev { uint8_t region, kind, late, pad; uint32_t at_us; };

#define SDK_IN_EV_MAX   8
#define SDK_RG_EV_MAX   16
struct sdk_input {
    uint8_t  n;
    struct sdk_btn_ev ev[SDK_IN_EV_MAX];
    uint8_t  held;
    uint8_t  flags;                 /* SDK_INF_* */
    int16_t  steps;                 /* relative wheel, clockwise + */
    uint16_t velocity;              /* clicks/s */
    int8_t   pos, dpos;             /* absolute (B/C); pos -1 otherwise */
    uint8_t  touched;
    int8_t   menu_pick;             /* the row picked in an SDK menu, -1 */
    uint8_t  rg_n;                  /* wheel regions (C) */
    struct sdk_region_ev rg[SDK_RG_EV_MAX];
};
#define SDK_INF_DROPPED   0x01
#define SDK_INF_SETTINGS  0x02
#define SDK_INF_RESUMED   0x04
#define SDK_INF_VIDEO     0x08
#define SDK_INF_SONG_END  0x10
/* appended for Chess (#132, 2026-10-25; still ABI 0.1): what a game that
 * reads the absolute wheel (sdk_wheel_pos) needs to tell a tap on the
 * ring from a press of a button under it, and to rest while dark */
#define SDK_INF_PRESSED   0x20  /* a button went down since the last step
                                   (any, the firmware's too): a finger on
                                   the ring now is a press, not a tap */
#define SDK_INF_LIT       0x40  /* the screen lit again since the last
                                   step: the touch that lit it is no tap */
#define SDK_INF_DARK      0x80  /* the screen is dark now: no search, no
                                   animation; return SDK_IDLE (the SDK
                                   steps the game again when it lights) */

/* REL: the shell's accelerated steps (as its lists); REL_RAW: one step
 * per driver click, no acceleration (the game scales by velocity itself);
 * ABS, REGIONS: B/C (planned). Append only. */
enum sdk_wheel { SDK_WHEEL_REL, SDK_WHEEL_ABS, SDK_WHEEL_REGIONS,
                 SDK_WHEEL_REL_RAW };
int sdk_wheel_mode(struct sdk_ctx *c, enum sdk_wheel m);  /* A: REL, REL_RAW */
/* The finger's position on the ring, polled: 0..95 clockwise from the
 * top (MENU), or -1 with no finger; read-only, the event stream stays
 * relative (Q30's default; owner, 2026-10-24, #132). Every class. A game
 * that calls it is woken while idle when a finger lands or lifts, and
 * when the screen lights again (sdk.md 3.3). */
int sdk_wheel_pos(struct sdk_ctx *c, int *pos, bool *touched);

/* What a long MENU (500 ms) does in Class A. EXIT, the default: the game
 * leaves (its exit policy). PAUSE: the game stays, and its next step gets
 * one { SDK_BTN_MENU, SDK_IN_LONG } event, on which it opens its pause
 * menu (with the overlay already open the event still comes; the game may
 * ignore it). Short MENU stays back(). The reserved inputs stay the
 * firmware's either way. At init or any time; B/C: SDK_E_CLASS (they own
 * MENU outright). Append only. */
enum sdk_longmenu { SDK_LONGMENU_EXIT, SDK_LONGMENU_PAUSE };
int sdk_long_menu(struct sdk_ctx *c, enum sdk_longmenu m);

/* -- drawing, Class A (section 5.2) -------------------------------------- */
#define SDK_LCD_W 320
#define SDK_LCD_H 240
typedef uint16_t sdk_px;            /* RGB565 */
#define SDK_RGB(r, g, b) ((sdk_px)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) \
                                   | ((b) >> 3)))
void sdk_area(struct sdk_ctx *c, int *x, int *y, int *w, int *h);
void sdk_fill(struct sdk_ctx *c, int x, int y, int w, int h, sdk_px col);
void sdk_rows(struct sdk_ctx *c, int x, int y, int w, int h,
              const sdk_px *row_col);
void sdk_bitmap(struct sdk_ctx *c, const sdk_px *src, int src_x, int src_y,
                int stride, int x, int y, int w, int h);
void sdk_bitmap_key(struct sdk_ctx *c, const sdk_px *src, int src_x,
                    int src_y, int stride, int x, int y, int w, int h,
                    sdk_px key);
enum sdk_mat { SDK_MAT_GEL_BLUE, SDK_MAT_GEL_GREEN, SDK_MAT_GEL_RED,
               SDK_MAT_GEL_SKY, SDK_MAT_METAL, SDK_MAT_GROOVE,
               SDK_MAT_SELECT, SDK_MAT__N };
/* a 1-bit mask (MSB first) filled with a material, coloured per row */
void sdk_mask(struct sdk_ctx *c, const uint8_t *mask, int stride_bytes,
              int x, int y, int w, int h, enum sdk_mat m);
void sdk_material(struct sdk_ctx *c, enum sdk_mat m, int x, int y,
                  int w, int h);
enum sdk_font { SDK_FONT_LIST, SDK_FONT_SMALL };    /* 15 px, 12 px */
int  sdk_text(struct sdk_ctx *c, int x, int y, int str, enum sdk_font f,
              sdk_px col);
int  sdk_text_num(struct sdk_ctx *c, int x, int y, int32_t n,
                  enum sdk_font f, sdk_px col);
int  sdk_text_fmt(struct sdk_ctx *c, int x, int y, int str, int32_t arg,
                  enum sdk_font f, sdk_px col);
int  sdk_text_width(struct sdk_ctx *c, int str, enum sdk_font f);
/* the width sdk_text_num() would draw n at (nothing drawn) */
int  sdk_num_width(struct sdk_ctx *c, int32_t n, enum sdk_font f);
/* rows y..y+h-1 changed: pushed after the call as one full-width band */
void sdk_dirty(struct sdk_ctx *c, int y, int h);

/* Standard words (ids >= SDK_STR_BASE), localised by the firmware */
#define SDK_STR_BASE      0x8000
enum { SDK_STR_RESUME = SDK_STR_BASE, SDK_STR_NEW_GAME, SDK_STR_SETTINGS,
       SDK_STR_EXIT, SDK_STR_ON, SDK_STR_OFF, SDK_STR__END };

/* -- standard overlays (section 5.4) -------------------------------------
 * A row of SDK_STR_EXIT is the SDK's own: picking it runs the game's exit
 * (SDK_WHY_USER, its policy: exit() saves the record), as the exit chord
 * does; the game never sees that pick. Every game's pause menu ends with
 * one (owner, 2026-10-24). */
#define SDK_MENU_ROWS_MAX 6
struct sdk_menu {
    uint8_t n;                      /* rows, <= 6 */
    const uint16_t *rows;           /* string ids */
    uint8_t sel;                    /* the row selected at open */
};
int  sdk_menu_open(struct sdk_ctx *c, const struct sdk_menu *m);
void sdk_menu_close(struct sdk_ctx *c);
bool sdk_menu_is_open(struct sdk_ctx *c);

/* -- memory (section 5.6) ------------------------------------------------ */
void *sdk_work(struct sdk_ctx *c, size_t bytes);    /* A: once, in init */
void *sdk_borrow(struct sdk_ctx *c, size_t bytes);  /* B: NULL until #108 */

/* -- feedback and music (section 7) -------------------------------------- */
void sdk_click(struct sdk_ctx *c);
bool sdk_music_playing(struct sdk_ctx *c);
int  sdk_music_pause(struct sdk_ctx *c);            /* B only */
int  sdk_music_resume(struct sdk_ctx *c);           /* B only */

/* -- the state record (section 8.2) -------------------------------------- */
void *sdk_state(struct sdk_ctx *c, uint16_t *len);
int   sdk_state_commit(struct sdk_ctx *c, uint16_t len);

/* -- per-game settings (section 9) --------------------------------------- */
enum sdk_set_kind { SDK_SET_CHOICE, SDK_SET_TOGGLE, SDK_SET_AV_OFFSET };
enum sdk_set_apply { SDK_APPLY_NOW, SDK_APPLY_NEXT };
struct sdk_setting {
    uint16_t label;
    uint8_t  kind, n, def, apply;
    /* greyed out and not selectable while setting off_if - 1 holds the
     * value off_val (off_if 0: always on). In what was padding: the
     * struct keeps its size and an older module reads 0 here */
    uint8_t  off_if, off_val;
    const uint16_t *values;
};
#define SDK_SETTINGS_MAX 16
struct sdk_settings {
    uint8_t version;
    uint8_t n;
    const struct sdk_setting *s;
};
int  sdk_setting(struct sdk_ctx *c, unsigned i);
int  sdk_settings_open(struct sdk_ctx *c);
bool sdk_setting_changed(struct sdk_ctx *c, unsigned i);

/* -- errors (section 12) ------------------------------------------------- */
enum sdk_err {
    SDK_OK = 0, SDK_E_ARG = -1, SDK_E_CLASS = -2, SDK_E_PHASE = -3,
    SDK_E_QUOTA = -4, SDK_E_NOMEM = -5, SDK_E_REFUSED = -6,
    SDK_E_NOENT = -7, SDK_E_IO = -8, SDK_E_BUSY = -9, SDK_E_UNAVAIL = -10,
};
/* "sdk <name> ..." into the log, <= 100 chars, <= 10 lines a second;
 * %d %u %x %s %c only */
void sdk_log(struct sdk_ctx *c, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* -- libc helpers from the SDK table (section 11.1) ---------------------- */
#ifdef IPODUI_IN_MODULE
void  *memcpy(void *d, const void *s, size_t n);
void  *memset(void *d, int v, size_t n);
void  *memmove(void *d, const void *s, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
#else
#include <string.h>
#endif

#endif /* IPODUI_SDK_H */
