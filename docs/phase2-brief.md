# Phase 2 brief: shell skeleton (for the ui-fidelity agent)

**Goal:** replace the Phase 1 placeholder (`ipodui_main()` → `root_menu()`) with the shell's own main loop, screen stack, header bar, static list, theme and input layer. Booting the `--ipodui` simulator must land in a 7G-style main menu. The user can go into a Music stub and back, and Now Playing hands off to the stock WPS.

**Out of scope:**
- smooth pixel scrolling, gradient highlight, chevron polish and anti-aliased fonts (Phase 3)
- real tagcache menus (Phase 4)
- our own Now Playing (Phase 5)
- the final font (see below)
- Cover Flow and the art cache (Phase 7)

Read first: `CLAUDE.md`, `docs/hardware.md`, `docs/design/spec/index.md`, `docs/design/devmode.md`, and `docs/upstream-sync.md`. The upstream sync is **done** (2026-10-06; main now sits on upstream `2890d4a40d`). Scripts named `ipodui-*` run against `build/sim-ui` in the `scripts/validate.sh` phase gate.

## 1. What exists (verified against the code)

- **`apps/main.c:250-256`**
  - Under `#ifdef HAVE_IPOD_UI`, `main()` calls `ipodui_main()` instead of `root_menu()`.
  - By then `init()` is complete: settings loaded, `playlist_init()`, `audio_init()`, tagcache init, `settings_apply_skins()`. Also done: `list_init()`, `tree_init()`, `usb_start_monitoring()`, `allocate_playback_log()`.
  - Nothing after `init()` needs `root_menu`.
- **`apps/ipodui/ipodui.c`:** a placeholder that calls `root_menu()`. `ipodui.h` declares `void ipodui_main(void);` with the comment "Never returns". `apps/SOURCES:60` compiles it under `#ifdef HAVE_IPOD_UI`. `tools/configure --ipodui` adds `-DHAVE_IPOD_UI`.
- **What `root_menu()` sets up** (`apps/root_menu.c`), and what the shell does with each part:

| `root_menu` does | Shell action |
|---|---|
| `push_current_activity(ACTIVITY_MAINMENU)` | **Replicate.** The skin engine and the "refresh on pop" behavior key off the activity stack. Push `ACTIVITY_MAINMENU` once at startup. Push and pop `ACTIVITY_WPS` around the WPS hand-off. |
| `root_menu_setup_screens()`: picks the start screen from `global_settings.start_in_screen` and `global_status.last_screen`, registers `PLAYBACK_EVENT_TRACK_CHANGE` (for `current_track_path`, used only by the stock browser), handles the RTC alarm wake screen and headphone autoresume | **Don't call it** (it's static anyway). The shell always starts at its main menu. Phase 8 adds "restore last screen". Ignore `global_status.last_screen`; it belongs to stock. No track-change event is needed yet. |
| `wpsscrn()`: if audio is stopped and `global_status.resume_index != -1`, it calls `playlist_resume()` + `playlist_resume_track(index, crc32, elapsed, offset)`, then `gui_wps_show()`. Otherwise it `splash()`es "Nothing to resume" or asks a `yesno_pop()` "replay finished playlist?". | **Replicate without splash or yesno** in `ui_playback_resume()` (about 15 lines copied from `wpsscrn`, plus the `HAVE_PITCHCONTROL` resume pitch and speed). With nothing to resume, it just returns false. The "Now Playing" item is hidden in that case. |
| `browser()` / `GO_TO_DBBROWSER`: waits for `tagcache_is_usable()` with a splash progress screen | **Don't replicate.** Phase 4 shows the 7G "Updating Library…" row instead. |
| `do_menu(&root_menu_)` and the `GO_TO_*` dispatch loop | Replaced by the shell's screen stack. |
| `load_screen()` writes `global_status.last_screen` and calls `status_save()` | Don't touch `global_status.last_screen`. |

- **The WPS:** `long gui_wps_show(void)` (`apps/gui/wps.h:37`) runs its own loop and returns a `GO_TO_*` code (`GO_TO_ROOT`, `GO_TO_PREVIOUS`, `GO_TO_PREVIOUS_BROWSER`, `GO_TO_PLAYLIST_VIEWER`, `GO_TO_FILEBROWSER`, `GO_TO_PLUGIN`, …). In Phase 2, **every return code means "pop back to the shell".** Log the code with `logf` so Phase 5 knows which ones occur.
- **Input** (`firmware/export/button.h`, `firmware/target/arm/ipod/button-clickwheel.c`, `ipodvideo.h`):
  - `long button_get_w_tmo(int ticks)` blocks with a timeout and returns `BUTTON_NONE` when it expires. `intptr_t button_get_data(void)` returns the data of the last event.
  - Buttons: `BUTTON_SELECT`, `BUTTON_MENU`, `BUTTON_PLAY`, `BUTTON_LEFT`, `BUTTON_RIGHT`, `BUTTON_SCROLL_FWD`, `BUTTON_SCROLL_BACK`. They come with `BUTTON_REPEAT` and `BUTTON_REL` modifiers, and chords are ORed bits.
  - `HAVE_SCROLLWHEEL` and `HAVE_WHEEL_ACCELERATION` are set. Wheel events carry `(1<<31) | (1<<24) | wheel_velocity` (clicks/s, filtered 15/16), and `button_apply_acceleration(data)` (`firmware/drivers/button.c:631`) turns that into an item step count (this is what `apps/gui/list.c:632` uses). `WHEEL_SENSITIVITY` = 6 raw clicks per event, and 96 raw clicks make a turn. `HAVE_WHEEL_POSITION` gives `wheel_status()`, the absolute position.
  - The first-keypress-wakes-backlight filter lives in the firmware (`button.c` `filter_first_keypress_*`), so the shell gets it for free.
  - **System events** (`SYS_USB_CONNECTED`, power, charger, filesystem) arrive through the same `button_get*` queue. The shell **must** pass any value with `SYS_EVENT` set to `default_event_handler(button)` (`apps/misc.h:128`). Otherwise USB and shutdown break. In Phase 2, USB shows the stock USB screen; Phase 8 replaces it.
- **LCD:** `lcd_set_viewport()`, `lcd_fillrect`, `lcd_gradient_fillrect(x,y,w,h,start,end)`, `lcd_bitmap_part()`, `lcd_putsxy()`, `font_getstringsize()`, `lcd_update_rect()`, and `screens[SCREEN_MAIN]` (`apps/screen_access.h`). Before drawing, turn the stock theme and status bar off with `viewportmanager_theme_enable(SCREEN_MAIN, false, NULL)` (`apps/gui/viewport.h:55`), and undo that around the WPS hand-off (`viewportmanager_theme_undo`).
- **Simulator scripts** (`uisimulator/common/sim_script.c`): the commands are `wait`, `key`, `hold`, `wheel`, `shot` and `quit`. There is no chord command yet (devmode adds it). Run them with `SIM_BUILD=build/sim-ui scripts/shots.sh tests/ui/<name>.script`.

## 2. Files to create

All new files go in `apps/ipodui/`, carry the GPL header, use Rockbox style, and contain no `splash()`.

| File | Contents |
|---|---|
| `ui_main.c` | `ipodui_main()`, moved here. Delete `ipodui.c` and keep `ipodui.h` as the public header. Startup, the main loop, the WPS hand-off and `ui_playback_resume()`. |
| `screen_stack.{c,h}` | Push, pop and replace; dirty flags; dispatch. |
| `header.{c,h}` | The 22 px header bar with cached state and repaint-on-change. |
| `list.{c,h}` | A static list widget: row-snapped, no animation, solid highlight. |
| `theme.{c,h}` | All metrics, colours and font IDs in one place. |
| `input.{c,h}` | `button_get_w_tmo` wrapper: maps keys, handles release and long-press, wheel steps, and forwards `SYS_*` events. |
| `menu.{c,h}` | A data-driven menu screen built on `list` (main menu, Music stub, Settings stub). |
| `screens/stub.c` | An "empty" screen (header + blank list) for every not-yet-built item. |
| `apps/SOURCES` | Replace `ipodui/ipodui.c` with the files above, inside the existing `#ifdef HAVE_IPOD_UI`. |

Upstream files: **none** besides `apps/SOURCES`. If a hook seems necessary, stop and ask; it must be a `#ifdef HAVE_IPOD_UI` one-liner.

## 3. Interfaces

```c
/* theme.h */
struct ui_theme {
    int header_h;               /* 22 (≈, spec) */
    int row_h;                  /* 26 (≈) */
    int text_pad_x;             /* 8 */
    int split_x;                /* 160: main menu list width; panel is x>=split_x */
    unsigned header_top, header_bottom, header_line;   /* LCD_RGBPACK values */
    unsigned list_bg, list_fg, sel_bg, sel_fg;          /* Phase 2: solid sel_bg */
    unsigned panel_bg;
    int font_ui;                /* placeholder, see Fonts */
    int font_title;
};
extern const struct ui_theme *ui_theme;      /* const data, no RAM table copies */
void ui_theme_init(void);                    /* loads fonts, sets ui_theme */

/* input.h */
enum ui_key { UI_KEY_NONE = 0, UI_KEY_MENU, UI_KEY_SELECT, UI_KEY_PLAY,
              UI_KEY_PREV, UI_KEY_NEXT, UI_KEY_WHEEL };
struct ui_input {
    enum ui_key key;
    bool release;               /* button released (act on this for SELECT/MENU) */
    bool repeat;                /* held, repeating */
    bool long_press;            /* first repeat past ~HZ/2, fired once */
    int  wheel_steps;           /* UI_KEY_WHEEL: signed, after acceleration */
    unsigned wheel_velocity;    /* raw clicks/s, for Phase 3 */
    long raw;                   /* original button code */
};
/* Blocks up to 'timeout' ticks (TIMEOUT_BLOCK allowed). Handles SYS_*
 * events internally via default_event_handler(). Returns false on timeout. */
bool ui_input_get(struct ui_input *in, int timeout);

/* screen_stack.h */
#define UI_DIRTY_HEADER  0x1
#define UI_DIRTY_BODY    0x2
#define UI_DIRTY_ALL     (UI_DIRTY_HEADER|UI_DIRTY_BODY)

enum ui_result { UI_HANDLED = 0, UI_IGNORED, UI_POP };

struct ui_screen;
struct ui_screen_ops {
    const char *name;                                    /* logs and tests */
    int  (*enter)(struct ui_screen *s, const void *arg); /* <0 aborts the push */
    void (*leave)(struct ui_screen *s);                  /* free all core_alloc */
    void (*draw)(struct ui_screen *s, unsigned dirty);   /* draw only; no lcd_update */
    enum ui_result (*input)(struct ui_screen *s, const struct ui_input *in);
    int  (*timeout)(struct ui_screen *s);                /* optional: ticks until
                                                            next wakeup, or
                                                            TIMEOUT_BLOCK */
};
struct ui_screen {
    const struct ui_screen_ops *ops;
    const char *title;          /* shown in header */
    unsigned dirty;
    int  handle;                /* core_alloc handle for screen state, 0 if none */
    void *priv;                 /* small inline state */
};
#define UI_STACK_MAX 8          /* static: 8 * sizeof(struct ui_screen) < 256 B */
int  ui_push(const struct ui_screen_ops *ops, const void *arg);
void ui_pop(void);
struct ui_screen *ui_top(void);
void ui_invalidate(unsigned dirty);   /* mark top screen dirty */

/* header.h */
struct ui_header_state {
    const char *title;
    enum { UI_PLAY_NONE, UI_PLAY_PLAYING, UI_PLAY_PAUSED } play;
    int  battery_pct;           /* battery_level() */
    bool charging;              /* charging_state() */
    bool hold;                  /* button_hold() */
};
/* Draws only if 'st' differs from the last drawn state (or force).
 * Returns true if pixels changed (caller adds header band to the update). */
bool ui_header_draw(const struct ui_header_state *st, bool force);
void ui_header_poll(struct ui_header_state *st);   /* fill from system state */

/* list.h -- Phase 2: static, row-snapped, no animation */
struct ui_list {
    struct viewport vp;         /* list area (header excluded) */
    int count, selected, top;   /* top = first visible row */
    const char *(*get_label)(int idx, void *data, char *buf, size_t len);
    bool (*has_chevron)(int idx, void *data);   /* may be NULL */
    void *data;
};
void ui_list_init(struct ui_list *l, const struct viewport *vp, int count,
                  const char *(*get_label)(int, void *, char *, size_t),
                  void *data);
/* Moves selection (clamped, no wrap -- 7G doesn't wrap). Returns the
 * union of rows that need redraw as a y-band, or false if unchanged. */
bool ui_list_move(struct ui_list *l, int delta, int *dirty_y, int *dirty_h);
void ui_list_draw(struct ui_list *l, int y, int h);  /* draw rows in band */

/* menu.h */
struct ui_menu_item {
    const char *label;
    const struct ui_screen_ops *target;   /* NULL: action only */
    const void *arg;
    bool (*visible)(void);                /* NULL = always */
    void (*action)(void);                 /* run on select if target NULL */
};
struct ui_menu {
    const char *title;
    const struct ui_menu_item *items;
    int count;
    bool split;                           /* main menu: list left, panel right */
};
extern const struct ui_screen_ops ui_menu_screen;    /* arg: const struct ui_menu* */
extern const struct ui_screen_ops ui_stub_screen;    /* arg: const char *title */
```

**Main loop (`ui_main.c`):**

```c
void ipodui_main(void)
{
    push_current_activity(ACTIVITY_MAINMENU);
    viewportmanager_theme_enable(SCREEN_MAIN, false, NULL);
    ui_theme_init();
    ui_push(&ui_menu_screen, &main_menu);
    for (;;) {
        /* 1. header: poll state, redraw only on change */
        /* 2. top->ops->draw(top, top->dirty); lcd_update_rect() on the
              dirty full-width bands only; top->dirty = 0 */
        /* 3. ui_input_get(&in, timeout) where timeout = top->ops->timeout
              or HZ*5 (battery/hold poll); HZ/2 while audio is playing */
        /* 4. dispatch: screen->input(); UI_IGNORED + MENU release -> ui_pop()
              (never pop the root); PLAY release -> pause/resume (7G) */
    }
}
```

**Menus (Phase 2 content):**
- **Main** (split: true): Music, Podcasts, Extras, Settings, Shuffle Songs, and Now Playing (visible when `audio_status()` or resumable). Videos and Photos are hidden, per the spec.
- **Music:** Cover Flow, Playlists, Artists, Albums, Compilations, Songs, Genres, Composers, Audiobooks, Search. Each item opens `ui_stub_screen`.
- **Settings:** About, plus stubs.
- **Shuffle Songs** is a **temporary** action, to be replaced by tagcache in Phase 4: `playlist_create("/Music", NULL)`, `playlist_insert_directory(NULL, "/Music", PLAYLIST_INSERT_LAST, false, true, NULL)`, `playlist_shuffle(current_tick, -1)`, `playlist_start(0, 0, 0)`, then the WPS hand-off. Mark it `/* PHASE4: replace */`. This exists so the WPS hand-off can be tested.

**WPS hand-off:**

```c
static void ui_show_wps(void)
{
    if (!audio_status() && !ui_playback_resume())
        return;
    viewportmanager_theme_undo(SCREEN_MAIN, false);
    push_current_activity(ACTIVITY_WPS);
    long rc = gui_wps_show();
    pop_current_activity_without_refresh();
    logf("ipodui: wps returned %ld", rc);
    viewportmanager_theme_enable(SCREEN_MAIN, false, NULL);
    ui_header_draw(..., true);
    ui_invalidate(UI_DIRTY_ALL);      /* one full lcd_update() here only */
}
```

## 4. Rules specific to this phase

- **LCD**
  - The header repaints only when its state changes.
  - A selection move redraws and updates only the old and new rows, as full-width `lcd_update_rect` bands. A full `lcd_update()` happens only on push, pop and return from the WPS.
- **No polling:** use `button_get_w_tmo` with a timeout of HZ×5 when idle and HZ/2 while playing (for the play glyph). Never use `yield()` loops.
- **No `cpu_boost`** in Phase 2. Nothing animates.
- **Memory**
  - The screen stack is a static array of 8, which is under 256 B.
  - A list's state lives inside its screen's `priv` or a small `core_alloc`.
  - **Phase 2 adds at most 4 KB of statics and .bss.** Check with `arm-none-eabi-nm --size-sort build/fw-ui/rockbox.elf | grep -i ui_` and record the numbers in the budget table.
  - On this target `.rodata` sits in DRAM too, so keep glyph bitmaps small.
- **Strings:** use plain English literals in Phase 2, with no `LANG_` IDs and no "Rockbox" anywhere. Add `/* i18n: later */` once at the top of `menu.c`.
- **Glyphs:** battery (outline plus fill, green, with a bolt while charging), play and pause triangles, and a hold lock. Draw them with `lcd_fillrect` or `lcd_hline`, or use tiny 1-bit arrays you made yourself. No Apple assets.

## 5. Fonts

`docs/design/fonts.md` is being written in parallel, and the font decision is **not** a dependency.
- Use **`FONT_UI`** (the stock UI font from settings) for both `font_ui` and `font_title`, set in `ui_theme_init()` only.
- Every text call takes its font from `ui_theme`, so swapping fonts later means editing one function.
- Don't size rows from the font. Rows use the theme constant `row_h` (26) and text is vertically centred, so the screenshots stay layout-stable when the font changes.

## 6. Acceptance criteria

1. **The builds pass** (`build` skill), with no new warnings in `apps/ipodui/`:
   - `build/sim-ui` (`--ipodui`, sim)
   - `build/fw-ui` (`--ipodui`, fw)
   - stock `build/sim` and `build/fw`
   - the stock builds must be unaffected: with `HAVE_IPOD_UI` off, no `apps/ipodui/` objects are built.
2. **Simulator screenshots**, compared against `docs/reference/7g/` where references exist:
   - `boot`: the main menu with the header bar ("iPod" title centred, battery on the right), the list on the left 160 px with "Music" selected, and the right panel filled with `panel_bg`.
   - `menu_down`: after `wheel +2` the selection is on "Extras", and only two rows were redrawn. Show this with a `logf` of the update rects, or by inspection.
   - `music`: after SELECT on Music, the header says "Music" and the stub items are listed full width.
   - `back`: after MENU, the main menu returns with "Music" still selected.
   - `nowplaying`: Shuffle Songs → stock WPS showing the fixture track; the play glyph appears after returning with MENU.
   - `wps_back`: back in the main menu, "Now Playing" is visible and the header shows the play glyph.
3. No `splash` and no "Rockbox" string on any shell screen (`grep -rn 'splash\|Rockbox' apps/ipodui/` is empty).
4. **budget-reviewer** passes on the branch: statics, no IRAM, balanced boost (none), partial updates, `#ifdef` hygiene.
5. `scripts/test.sh` is still green.

## 7. Tests to add

| File | Purpose |
|---|---|
| `tests/ui/ipodui-p2-menu.script` | boot → shot `boot`; `wheel +2` → shot `menu_down`; `wheel -2`; `key SELECT` → shot `music`; `wheel +3` → shot `music_down`; `key MENU` → shot `back`; quit |
| `tests/ui/ipodui-p2-wps.script` | boot; `wheel +4` (Shuffle Songs); `key SELECT`; `wait 2000` → shot `nowplaying`; `key MENU`; `wait 700` → shot `wps_back`; quit |
| `tests/ui/setup_music.sh` | Generates a 60 s tone MP3 (reusing the `make_fixtures.sh` ffmpeg recipe) into `$SIM_BUILD/simdisk/Music/`, so the WPS test has something to play. Make it idempotent, and call it from `scripts/shots.sh` only when the script name starts with `ipodui-p2-wps`, or document running it once. |
| `tests/ui/README.md` | Add the two scripts and the setup step. |
| `tests/ui/smoke.script` | Leave it alone. It drives the stock build. |

Report back with:
- the screenshot paths
- the list of remaining pixel differences against the spec
- the static RAM delta (`nm`)
- the binary size delta for `fw-ui` against `fw`

## 8. Optional, if time allows: devmode

Implement `docs/design/devmode.md` (`root_menu_dev()`, the About chord, and the `chord` command in `sim_script.c`). It makes the debug menu reachable from the `--ipodui` build, which later phases need for RAM and boost measurements. If skipped, it moves to Phase 8.
