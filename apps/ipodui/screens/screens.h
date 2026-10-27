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
 * iPod UI shell: library screens (browse, audiobooks, playlists)
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
#ifndef _IPODUI_SCREENS_H_
#define _IPODUI_SCREENS_H_

#include <stdbool.h>
#include "ipodui/library.h"
#include "ipodui/screen_stack.h"

/* First row of a browse list that stands for the whole list's children */
enum ui_browse_all {
    UI_ALL_NONE = 0,
    UI_ALL_SONGS,               /* "All Songs": the songs of every row */
    UI_ALL_ALBUMS,              /* "All Albums": the albums of every row */
};

/* What a browse screen shows. Passed to ui_push(&ui_browse_screen, arg);
 * copied by enter(), so it may live on the caller's stack. */
struct ui_browse_arg {
    const char *title;          /* header title (copied) */
    enum lib_cat cat;
    struct lib_filter f;
    enum ui_browse_all all;
    bool progress;              /* unused since P11 dropped the episode
                                   progress bar (⚠️ P1); kept so the menu
                                   tables' initialisers stay valid */
};

extern const struct ui_screen_ops ui_browse_screen;    /* arg: ui_browse_arg */
extern const struct ui_screen_ops ui_playlists_screen; /* arg: title */
/* about.c (P11, U2b; the stub.c one before it), .name "about"; devmode
 * arms on this pointer (docs/phase8a-brief.md Revision 1 M6; hook H3
 * points the About row here) */
extern const struct ui_screen_ops ui_about_screen;     /* arg: ignored */

/* audiobooks.c: the Audiobooks / Podcasts lists (browse screens; shows
 * and episodes carry the unplayed dot) */
extern struct ui_browse_arg ui_audiobooks_arg;   /* titles: labels_init() */
extern struct ui_browse_arg ui_podcasts_arg;
/* A book's page (Resume + entries, Phase 6): arg ui_book_page_arg, copied
 * by enter(). enter() fails (the push returns < 0) when the book has no
 * page (ab_page_open() < 0): the caller then plays the book itself. */
struct ui_book_page_arg {
    int view, idx;              /* the LIB_AUDIOBOOKS row */
    const char *title;          /* header title (copied) */
};
extern const struct ui_screen_ops ui_book_page_screen;

/* P11 screens (docs/phase11-brief.md section 4.8):
 *  U1:  ui_search_screen (screens/search.c; arg: the header title, NULL
 *       for "Search"), ui_otg_screen (otglist.c; arg: the header title,
 *       NULL for "On-The-Go")
 *  U2a: ui_checklist_screen (checklist.c)
 *  U2b: ui_datetime_menu_screen (datetime.c), ui_brightness_screen,
 *       ui_legal_screen, ui_reset_screen, ui_language_screen, ui_eq_screen
 *       (brightness.c, legal.c, reset.c, language.c, eq.c)
 * Elsewhere: ui_hold_screen (holdmenu.h), ui_vlimit_screen (vlimit.h),
 * ui_sys_charging_screen (sysscreen.h); ui_about_screen above (about.c). */
extern const struct ui_screen_ops ui_search_screen, ui_otg_screen,
    ui_checklist_screen, ui_datetime_menu_screen, ui_brightness_screen,
    ui_legal_screen, ui_reset_screen, ui_language_screen, ui_eq_screen,
    ui_newsw_screen;     /* newsw.c: #68, pushed by ui_bootfile_changed() */

/* -- U1 shared helpers (P11) -------------------------------------------- */

/* A screen's state (browse.c): from the screen arena (screen_stack.h),
 * released with the screen. If the arena is full, a pinned core_alloc
 * block, but only while audio is stopped (re-checked with no yield before
 * the allocation; it would shrink the audio buffer); s->handle then holds
 * it and ui_screen_free() (call it from leave()) frees it and lets the
 * audio buffer grow back. NULL if neither works. Zeroed. */
void *ui_screen_alloc(struct ui_screen *s, size_t size);
void  ui_screen_free(struct ui_screen *s);

/* A single-line label centred in a row 'h' px tall at x (row viewport,
 * the list's font), for rows whose text the list does not draw (a grey
 * "More...", the On-The-Go actions): a draw_extra hook body. */
void  ui_row_text(const char *text, int x, int h, unsigned fg);

/* playlists.c: the .m3u / .m3u8 files directly under /Playlists, read
 * once (one opendir), names packed into names[] (at most 'bytes'), their
 * offsets in off[] (at most 'max'), sorted case-insensitively. Returns
 * the count. ui_pl_play() replaces the playlist with one of them (a name
 * from that list) and shows Now Playing; false if empty or unreadable. */
#define UI_PL_DIR "/Playlists"
int   ui_pl_read_dir(short *off, char *names, int max, int bytes);
bool  ui_pl_play(const char *name);
/* /Playlists changed (On-The-Go Save): the Playlists screen re-reads it
 * when it next draws in full. No I/O here. */
void  ui_pl_changed(void);

/* otglist.c: the On-The-Go store changed under the On-The-Go list (the
 * hold menu's Add or Remove): an open list reopens its view when it
 * next draws in full. */
void  ui_otg_changed(void);

/* U2b's screens: arg (the Settings row passes its label) is ignored by
 * all of them, which set their own title; ui_about_screen keeps that
 * contract too (devmode arms on its pointer). */

/* U2b (screens/language.c): the split value list of the Settings leaf
 * screens (Language, EQ, Time Zone region and city). SELECT calls set();
 * its result says how many screens to leave: 0 stay, 1 this list, 2 this
 * list and the value list below it, UI_VLIST_ROOT the whole stack (hold
 * MENU's slide). arg: const struct ui_vlist *, which must outlive the
 * screen. The panel is the tile of 'art' (panels.h) titled 'title', with
 * caption(selected) below it. State in the screen arena. */
#define UI_VLIST_ROOT (-1)
struct ui_vlist {
    short title;                        /* LANG_* (header and panel) */
    short art;                          /* panelart glyph or UI_TILE_* */
    int  (*count)(void);
    int  (*get)(void);                  /* highlighted row on entry */
    int  (*set)(int idx);
    const char *(*label)(int idx, char *buf, size_t len);
    const char *(*caption)(int idx, char *buf, size_t len);   /* or NULL */
};
extern const struct ui_screen_ops ui_vlist_screen;
/* A named screen that is the value list 'listp' (a const struct ui_vlist
 * *): forwards to ui_vlist_screen (EQ, Language). */
#define UI_VLIST_SCREEN(sym, nm, listp)                                    \
    static int sym##_enter(struct ui_screen *s, const void *arg)           \
    { (void)arg; return ui_vlist_screen.enter(s, (listp)); }               \
    static void sym##_draw(struct ui_screen *s, unsigned d)                \
    { ui_vlist_screen.draw(s, d); }                                        \
    static enum ui_result sym##_input(struct ui_screen *s,                 \
                                      const struct ui_input *in)           \
    { return ui_vlist_screen.input(s, in); }                               \
    static int sym##_tick(struct ui_screen *s)                             \
    { return ui_vlist_screen.tick(s); }                                    \
    static void sym##_panel(struct ui_screen *s, const struct ui_panel *p) \
    { ui_vlist_screen.draw_into_panel(s, p); }                             \
    const struct ui_screen_ops sym = {                                     \
        .name = nm, .enter = sym##_enter, .leave = NULL,                   \
        .draw = sym##_draw, .input = sym##_input, .timeout = NULL,         \
        .tick = sym##_tick, .draw_into_panel = sym##_panel }
/* Reset Settings pushes the language list with this: SELECT then lands
 * on the main menu (COMP-7) */
extern const struct ui_vlist ui_language_after_reset;
/* U2b (screens/eq.c): applies the stored EQ preset (ipodui_eq_preset) to
 * the bands, precut and enable (Reset), and the preset's name (the
 * Settings EQ caption, U2a) */
void ui_eq_apply(void);
const char *ui_eq_name(void);

#endif /* _IPODUI_SCREENS_H_ */
