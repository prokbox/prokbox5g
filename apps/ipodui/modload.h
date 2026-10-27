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
 * iPod UI shell: shell modules (#106, docs/design/module-loader.md)
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
#ifndef IPODUI_MODLOAD_H
#define IPODUI_MODLOAD_H

/* Shell modules: cold screens that live in /.rockbox/modules.pak and run
 * from one arena reserved at shell start (counted in the audio-buffer
 * floor), instead of the resident image.
 *
 * ON in the 0.8.0 release: --ipodui-release on the iPod Video adds
 * -DIPODUI_WITH_MODULES (tools/configure), and the Extras apps, games and
 * cold Settings screens load from modules.pak so the resident image stays
 * under the 1.5 MB cap (docs/hardware.md, "Shell modules"). OFF in a plain
 * --ipodui configure: EXTRA_DEFINES=-DIPODUI_WITH_MODULES turns it on
 * (IPODUI_MODULES) for development and tests, and then the modules build
 * leaves 12-18 KB less audio buffer than the resident one
 * (docs/design/module-loader.md, measured costs). Off, the build is the
 * resident one, byte for byte.
 *
 * With it on, the core's callers see stand-in screens (IPODUI_STANDINS).
 * IPODUI_MODLOAD is the target, which really loads from the pack; in the
 * simulator the module sources link statically under their real names
 * and the stand-ins land them at once, or after `mod_delay <ms>` (the
 * swap's UI script), or never (`mod_delay -1`: the error page).
 *
 * Rules for module code (enforced by apps/ipodui/modules/module.lds):
 *  - Thumb, no writable .data, no IRAM; .bss is zeroed at every load and
 *    every move, so nothing in it may outlive a visit to the screen (the
 *    tenants keep none: Date & Time's region pick and menu rows, rebuilt
 *    at every enter, are the only statics, accepted as per-visit);
 *  - no threads, tick tasks, timers or callbacks that outlive a screen
 *    of the module;
 *  - calls the core by name (linked --just-symbols against rockbox.elf),
 *    so a module only runs with the core of the same build (build ID).
 *
 * A module is held while any of its screens is on the stack. A held
 * module never moves or goes away; an idle one may be evicted (forgotten)
 * or moved (memmove + relocations again), only from the UI thread, and
 * never in the main-loop pass that released it (its code may still be
 * on the call stack). */

#include "config.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ipodui/modload_core.h"

#if defined(IPODUI_MODULES) && !defined(IPODUI_IN_MODULE)
#define IPODUI_STANDINS
#ifndef SIMULATOR
#define IPODUI_MODLOAD
#endif
#endif

/* The resident stand-ins (modload.c) carry other names than the module's
 * own screens, so the module (built with IPODUI_IN_MODULE) can define the
 * real ones while the core's callers keep their spelling. */
#ifdef IPODUI_STANDINS
#define ui_legal_screen         mod_stub_legal
#define ui_datetime_menu_screen mod_stub_datetime
#define ui_checklist_screen     mod_stub_checklist
#define ui_devloader_screen     mod_stub_devloader
#endif

/* The arena, counted in the floor trace. Sizing rule (P12 W1-F): the
 * largest single tenant (footprint = image + .bss + relocations, plus its
 * Class A working block) + MOD_IDX_STAGE (the pack index is read into the
 * arena's top 2 KB, never onto a stack) + 25 % headroom, rounded up to
 * 1 KB; the developer build also keeps the devloader beside it. `modpak.py
 * check` prints each tenant and the release figure (modules/check.ok)
 * and refuses a module over MOD_ARENA_USE.
 * Tenants share the arena one at a time: idle ones are evicted, least
 * recently used first, for an image or a working block alike
 * (mod_room_find(), modload_core.c); the indexer leaves at the end of its
 * build (mod_discard()), and a running build gives way to whatever the
 * user opens (it stops and is owed: interactive before background), so
 * the arena never has to hold the indexer and a screen side by side.
 * Measured 2026-10-24 (P12 integration, fw-ui-mod / fw-rel-mod check.ok):
 * xindex 24,720 B + 0 work is the largest (notes 6,640 + 16,384 = 23,024,
 * contacts 5,680 + 16,384, calendars 8,224 + 12,288, solitaire 7,168 +
 * 12,288, devloader 4,512); release (24,720 + 2,048) x 1.25 = 33,460 ->
 * 33,792 B; developer (24,720 + 4,512 + 2,048) x 1.25 = 39,100 ->
 * 39,936 B. */
#ifdef HAVE_IPOD_UI_DEVMODE
#define MOD_ARENA_SIZE  39936
#else
#define MOD_ARENA_SIZE  33792           /* xindex + the index stage + 25 % */
#endif

/* what modules may use: the top MOD_IDX_STAGE bytes are the index's */
#define MOD_ARENA_USE   (MOD_ARENA_SIZE - MOD_IDX_STAGE)
#define MOD_LINK_BASE   0x03000000u     /* far: > 4 MB from all core code */

enum mod_id {
    MOD_SETTINGS = 0,   /* tenant A: the cold Settings screens */
    MOD_DEVLOADER,      /* tenant B: "Loader & storage" (developer) */
    /* The P16 loader's tables (modload.c slots, devloader.c's layout)
     * cover the ids below MOD__LOADER until W1-F's mod_rt[MODPAK_MAX]
     * replaces them; the loader treats a later id as absent. */
    MOD__LOADER,
    /* P12 tenants (docs/phase12-brief.md 3.1), all Class A */
    MOD_CLOCKS = MOD__LOADER,   /* W1-C */
    MOD_ALARMS,         /* W1-A2: Alarms list, Create Alarm, Sleep Timer */
    MOD_STOPWATCH,      /* W1-S */
    MOD_LOCKSET,        /* W1-L: New/Confirm Combination, Lock/Reset */
    MOD_NOTES,          /* W1-N */
    MOD_CONTACTS,       /* W3-K */
    MOD_CALENDARS,      /* W3-C */
    MOD_XINDEX,         /* W1-N: the sync-time indexers (3.5) */
    /* wave 4: SDK modules (kind = sdk, docs/design/sdk.md) */
    MOD_SOLITAIRE,      /* W4-1 */
    MOD_BADGAME,        /* W4-0: the misbehaving test game (sim, devmode) */
    MOD_QUADRUS,        /* W4: the falling-block game */
    MOD_BRICKS,         /* W4: the brick-breaker */
    MOD_CHUTE,      /* W4: catch the jumpers */
    MOD_CHESS,          /* #132: chess against the engine */
    /* the other games append here (<= MODPAK_MAX in all) */
    MOD__N
};
_Static_assert(MOD__N <= MODPAK_MAX, "every module fits the pack index");

/* Export slots of each module (the module's .modexports table, in order).
 * P12: slot 0 is the module header (struct mod_desc, MOD_SLOT_DESC). */
enum { MOD_SET_LEGAL = MOD_SLOT_FIRST, MOD_SET_DATETIME, MOD_SET_CHECKLIST,
       MOD_SET__N };
enum { MOD_DEV_SCREEN = MOD_SLOT_FIRST, MOD_DEV__N };

/* A module's export table and header (P12, W1-F). Built as a module
 * (IPODUI_IN_MODULE) the table is the image's first words; linked
 * statically (the simulator, a resident build) it is mod_exports_<name>,
 * which the core finds by name (extras_roots.h, modload.c). Slot 0 must
 * be &mod_desc_<name>; `modpak.py check` compares it with the manifest.
 *   MOD_DESC_A(clocks, LANG_IPODUI_X_CLOCKS, 0);   (work KB)
 *   MOD_EXPORTS(clocks, 2) = { &mod_desc_clocks, &ui_x_clocks_screen };
 */
#ifdef IPODUI_IN_MODULE
#define MOD_EXPORTS(name, n) const void *const mod_exports[n] \
    __attribute__((section(".modexports"), used))
#else
#define MOD_EXPORTS(name, n) const void *const mod_exports_##name[n]
#endif
#define MOD_DESC_A(name, lang, work_kib) \
    MOD_DESC_A_PAGED(name, lang, work_kib, 0)
/* A Class A module that browses a paged list: pages = the manifest's
 * page loads a second (brief 2.1, modclass_page()) */
#define MOD_DESC_A_PAGED(name, lang, work_kib, pages_per_s) \
    const struct mod_desc mod_desc_##name = { \
        .magic = MOD_HDR_MAGIC, .mclass = MOD_CLASS_A, \
        .min_ram = MOD_RAM_32, .kind = MOD_KIND_SHELL, \
        .exit_policy = MOD_EXIT_IMMEDIATE, .work_kb = (work_kib), \
        .pages = (pages_per_s), .name_lang = (lang) }

struct ui_screen_ops;

/* Shell start, after the screen arena: reserves the module arena (also in
 * the simulator, so the floor trace is honest) and opens the pack. */
void mod_init(void);
/* The UI thread's main loop, once per pass */
void mod_pass(void);
/* USB: close the pack on the ack path (no write), reopen after release;
 * a new mount generation re-verifies each module once. */
void mod_usb_connect(void);
void mod_usb_release(void);

/* P12 (W1-F): the module's compacted pack entry (the simulator's static
 * modules: built from slot 0); NULL for an id the build has no module
 * for. UI thread. */
const struct mod_rt *mod_rt_get(enum mod_id id);
/* The module whose code a screen's ops are (a module in RAM on the
 * target, a static export table elsewhere), or -1 (resident) */
int mod_owner(const struct ui_screen_ops *ops);
/* Short name for logs ("settings", "clocks", ...) */
const char *mod_name(int id);
/* The export of a module that is in RAM now (no load, no wait), or NULL;
 * resident builds: the static table */
const void *mod_export_now(enum mod_id id, int slot);
#ifdef IPODUI_STANDINS
/* Intent preload of a module by id (an Extra's row; never spins up) */
void mod_preload_id(enum mod_id id);
#else
static inline void mod_preload_id(enum mod_id id) { (void)id; }
#endif
#ifdef IPODUI_STANDINS
/* An Extra's root screen in a modules build: a generic stand-in, pushed
 * with arg (intptr_t)enum extra_id (extras.c); it becomes the module's
 * export slot of extras_table[x] when the read lands */
extern const struct ui_screen_ops mod_stub_extra;
#endif

#ifdef IPODUI_MODULES
/* Push and pop (screen_stack.c): a screen whose ops lie in a module is
 * held while it is on the stack. No-ops for resident ops. */
void mod_screen_hold(const struct ui_screen_ops *ops);
void mod_screen_drop(const struct ui_screen_ops *ops);
/* have == want, or want is a stand-in that resolved to have */
bool mod_ops_is(const struct ui_screen_ops *have,
                const struct ui_screen_ops *want);
/* Intent preload: a row whose target is a module screen was highlighted.
 * Starts the load on the storage worker if the disk is awake (never a
 * spin-up for a hint). */
void mod_preload(const struct ui_screen_ops *target);
/* The panel scheduler took key (a preview's or a screen's ops): the last
 * two keys are held, so a module's preview is never called after it
 * moved or went away (preview.c) */
void mod_panel_key(const void *key);
#else /* today's resident build: the hooks vanish */
static inline void mod_screen_hold(const struct ui_screen_ops *ops)
{ (void)ops; }
static inline void mod_screen_drop(const struct ui_screen_ops *ops)
{ (void)ops; }
static inline bool mod_ops_is(const struct ui_screen_ops *have,
                              const struct ui_screen_ops *want)
{ return have == want; }
static inline void mod_preload(const struct ui_screen_ops *target)
{ (void)target; }
static inline void mod_panel_key(const void *key) { (void)key; }
#endif
/* The export of a module, loading it if needed (waits for the worker:
 * the snapshot batch only); NULL on failure (logged). Takes no hold.
 * Resident ops in the sim. */
const void *mod_export(enum mod_id id, int slot);
/* Entering the menu that leads to a module (Settings): start its load,
 * spinning the disk up if need be (#104) */
void mod_warm(enum mod_id id);

/* -- P12 class ABI (docs/phase12-brief.md 3.1; W1-F implements) ------- */
struct mod_rt;
/* Runnable on this unit: mod_why_hidden() over the probed RAM and
 * MOD_CLASSES_BUILT. A module that fails is HIDDEN: no row in the Games
 * or the Extras list, no page, no splash; logged "mod hide <name>
 * why=abi|ram|class|hdr". */
bool mod_runnable(const struct mod_rt *e);
/* M3: keep a module resident and unmoved while code outside the UI
 * thread runs it (the indexer on the storage worker). No eviction, no
 * move while pinned; balanced by mod_unpin(). */
void mod_pin(enum mod_id id);
void mod_unpin(enum mod_id id);
/* H1: a module with no more use for now (the indexer after its build,
 * any thread): it leaves the arena at the next UI pass that finds it
 * idle, instead of crowding the next tenant's room. mod_pin() undoes it. */
void mod_discard(enum mod_id id);
/* H1 (L2): could module id be placed now, with its working block, if
 * only idle modules were evicted (no held, pinned or live one)? No
 * eviction, no read. Static and resident builds: true. UI thread. */
bool mod_room_for(enum mod_id id);
/* A game (an SDK module) is running: pinned from its entry to its
 * reclaim (sdk.c) */
bool mod_game_live(void);
/* Class A: the module's one working block, from the arena, freed when
 * its last screen leaves (mod_screen_drop). NULL if it does not fit: the
 * caller shows its reduced view and never allocates elsewhere. */
void *mod_work_alloc(enum mod_id id, size_t bytes);
/* W4-0: a loaded module's image + .bss in RAM [lo, hi) (the SDK's source
 * checks and the watchdog's PC test); false in a static build */
bool mod_image_range(enum mod_id id, uintptr_t *lo, uintptr_t *hi);
/* Class B (defined, not built in P12; M4): a borrow from the audio buffer
 * WHILE PLAYING, through #108's shrink-without-stop (not maint_take()).
 * false = refused (reduced mode or "Not Available"). */
bool mod_borrow(enum mod_id id, size_t bytes);
void mod_borrow_return(enum mod_id id);
/* Class C (defined, not built in P12): the handover. Pauses or stops
 * playback with the resume point kept and lends the buffer; exit restores
 * the queue paused. A failure stops audio and pushes the styled error
 * screen (the video-failure rule). */
struct mod_session;                     /* opaque, P14/P19 */
int  mod_session_enter(enum mod_id id, struct mod_session **s);
void mod_session_exit(struct mod_session *s, int status);

/* -- developer view (tenant B) ------------------------------------------ */
enum mod_state { MOD_ABSENT, MOD_LOADING, MOD_READY, MOD_FAILED };
struct mod_info {
    const char *name;
    uint8_t  state;         /* enum mod_state */
    uint8_t  verified;      /* Fletcher-32 checked this mount generation */
    uint16_t refs;
    uint32_t offset;        /* in the arena, or ~0 */
    uint32_t bytes;         /* image + bss + relocations */
    uint32_t nreloc;
    uint32_t loads, moves, evictions;
    uint32_t last_us;       /* last load: request to ready */
    uint32_t max_us;
};
struct mod_arena_info {
    uint32_t size, used, largest_free;
    uint32_t pak_bytes;
    uint32_t build_id;      /* the core's */
    uint32_t mount_gen;
    uint32_t fletcher_us;   /* last Fletcher-32 pass */
    uint8_t  pak_open;      /* 1: fd held */
    uint8_t  pak_ok;        /* 1: header, magic and build ID match */
    uint8_t  modload;       /* 0: the simulator's static modules */
};
int  mod_count(void);
void mod_get_info(int i, struct mod_info *out);
void mod_arena_get(struct mod_arena_info *out);

/* -- snapshot-first screens -------------------------------------------- */
/* A snapshot is rows [y, y + h) of a full-width first frame, kept in
 * /.rockbox/snap.pak (fixed slots, written in place) and keyed on the
 * build ID and the language (the one tenant's frame shows no library or
 * setting state). Snapshots are only rebuilt by the batch: the boot hold
 * and the post-USB moment (and a first visit creates the file); a stale
 * one is never shown: the screen draws live. "Updating Language..." is a
 * later step: a language change only makes the key stale. */
enum snap_id { SNAP_DEVLOADER, SNAP__N };
#if defined(IPODUI_MODULES) && defined(SIMULATOR)
/* sim_cmd_mod.c: `mod_delay <ms>` (-1: loads fail) */
void mod_sim_delay(int ms);
#endif
/* Reads the snapshot straight into the framebuffer rows (on the storage
 * worker on the target); false: none, stale, or no disk (draw live). */
bool mod_snap_show(enum snap_id id);

#endif /* IPODUI_MODLOAD_H */
