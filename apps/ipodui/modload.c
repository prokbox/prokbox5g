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
#include "config.h"
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "xcore.h"                  /* P19 S0: XCORE_BSS */
#include "file.h"
#include "core_alloc.h"
#include "storage.h"
#include "lcd.h"
#include "lang.h"
#include "settings.h"
#include "rbpaths.h"
#include "ipodui/anim.h"
#include "ipodui/theme.h"
#include "ipodui/header.h"
#include "ipodui/uilog.h"
#include "ipodui/screen_stack.h"
#include "ipodui/modload.h"
#include "ipodui/sdk/sdk_int.h"     /* sdk_screen_mod() */
#include "ipodui/modload_core.h"
#include "ipodui/modules/devloader.h"
#include "ipodui/screens/setwidgets.h"
#include "ipodui/extras.h"
#include "ipodui/screens/panels.h"   /* the Clocks stand-in's backdrop */
#include "ipodui/modclass.h"
#include "ipodui/xindex.h"         /* H1: the build yields to a user */
#include "ipodui/notice.h"         /* the notice's headerless stand-in */

extern struct frame_buffer_t lcd_framebuffer_default;

static const char *const mod_names[MOD__N] = {
    "settings", "devloader", "clocks", "alarms", "stopwatch", "lockset",
    "notes", "contacts", "calendars", "xindex", "solitaire", "badgame",
    "quadrus", "bricks", "chute", "chess",
};

/* A module's words hold 32-bit run addresses: the arena byte p's, and an
 * export slot read as a pointer. Plain casts on the target; the host test
 * (tests/modload/host/modhost.h) maps its 64-bit arena to a 32-bit run
 * window to run this file's load path on a real modules.pak. */
#ifndef MOD_RUN_ADDR
#define MOD_RUN_ADDR(p)           ((uint32_t)(uintptr_t)(p))
#define MOD_EXPORT_AT(base, slot) (((const void *const *)(base))[slot])
#endif

#ifdef USEC_TIMER
#define NOW_US() ((uint32_t)USEC_TIMER)
#else
#define NOW_US() ((uint32_t)current_tick * (1000000u / HZ))
#endif

/* P12 (M1, W1-F): the loader's one table, every build. A module's pack
 * entry compacted (the index itself is never kept, nor put on a stack),
 * plus its runtime state. 40 B x MOD__N (lean rule 9: can't share
 * because it is the loader's own state, read on every load). In a static
 * build (the simulator, a resident build) each entry comes from the
 * module's header, export slot 0. */
static struct mod_rt rt[MOD__N];

/* The unit's RAM variant (rule 7: a build-time dimension) */
#define UNIT_RAM ((MEMORYSIZE) >= 64 ? MOD_RAM_64 : MOD_RAM_32)

static void rt_hide(int id, int why)
{
    static const char *const whys[] = { "ok", "abi", "ram", "class", "hdr",
                                        "arena" };
    const char *w = whys[(unsigned)why < ARRAYLEN(whys) ? why : 0];

    rt[id].flags |= MOD_RT_HIDDEN;
    ui_log("mod", "mod hide %s why=%s", mod_names[id], w);
    UPDATE_TRACE("mod hide %s why=%s", mod_names[id], w);
}

/* the labels checked once, when the entry is filled */
static void rt_check(int id)
{
    struct modpak_ent e;
    int why;

    rt[id].flags &= ~MOD_RT_HIDDEN;
    mod_rt_ent(&rt[id], &e);
    why = mod_why_hidden(&e, UNIT_RAM, MOD_CLASSES_BUILT);
    if (why != MOD_WHY_OK)
        rt_hide(id, why);
}

const char *mod_name(int id)
{
    return (unsigned)id < MOD__N ? mod_names[id] : "-";
}

#ifndef IPODUI_MODULES
static void rt_init_once(void);
#else
#define rt_init_once() do { } while (0)
#endif

const struct mod_rt *mod_rt_get(enum mod_id id)
{
    rt_init_once();
    if ((unsigned)id >= MOD__N || !(rt[id].flags & MOD_RT_INPAK))
        return NULL;
    return &rt[id];
}

bool mod_runnable(const struct mod_rt *e)
{
    struct modpak_ent pe;

    if (!e || (e->flags & MOD_RT_HIDDEN))
        return false;
    mod_rt_ent(e, &pe);
    return mod_why_hidden(&pe, UNIT_RAM, MOD_CLASSES_BUILT) == MOD_WHY_OK;
}

void mod_pin(enum mod_id id)
{
    if ((unsigned)id >= MOD__N)
        return;
    if (rt[id].flags & MOD_RT_PINNED)
        ui_log("mod", "pin twice id=%s", mod_names[id]);
    rt[id].flags |= MOD_RT_PINNED;
    rt[id].flags &= ~MOD_RT_DISCARD;    /* in use again */
}

void mod_unpin(enum mod_id id)
{
    if ((unsigned)id < MOD__N)
        rt[id].flags &= ~MOD_RT_PINNED;
}

void mod_discard(enum mod_id id)
{
    if ((unsigned)id < MOD__N)
        rt[id].flags |= MOD_RT_DISCARD;
}

bool mod_game_live(void)
{
    int i;

    for (i = 0; i < MOD__N; i++)
        if (rt[i].kind == MOD_KIND_SDK && (rt[i].flags & MOD_RT_PINNED))
            return true;
    return false;
}

/* Class B and C are defined, not built, in P12 (brief 3.1, M4) */
bool mod_borrow(enum mod_id id, size_t bytes)
{
    (void)id; (void)bytes;
    return false;                       /* refused: reduced mode */
}

void mod_borrow_return(enum mod_id id)
{
    (void)id;
}

int mod_session_enter(enum mod_id id, struct mod_session **s)
{
    (void)id;
    if (s)
        *s = NULL;
    return -1;                          /* no Class C module is built */
}

void mod_session_exit(struct mod_session *s, int status)
{
    (void)s; (void)status;
}

/* -- static modules (the simulator, a resident build) ---------------------- */
#ifndef IPODUI_MODLOAD
#if defined(IPODUI_MODULES)
extern const void *const mod_exports_settings[MOD_SET__N];
#ifdef HAVE_IPOD_UI_DEVMODE
extern const void *const mod_exports_devloader[MOD_DEV__N];
#endif
#endif

/* A module's static export table, or NULL (with its length) */
static const void *const *static_tab(int id, int *n)
{
#if defined(IPODUI_MODULES)
    if (id == MOD_SETTINGS)
    {
        *n = MOD_SET__N;
        return mod_exports_settings;
    }
#ifdef HAVE_IPOD_UI_DEVMODE
    if (id == MOD_DEVLOADER)
    {
        *n = MOD_DEV__N;
        return mod_exports_devloader;
    }
#endif
#endif
    return extras_static_exports(id, n);
}

/* rt[] from each module's header (slot 0), once */
static void static_init(void)
{
    int id, n;

    for (id = 0; id < MOD__N; id++)
    {
        const void *const *t = static_tab(id, &n);
        const struct mod_desc *d = t && n > 0 ? t[MOD_SLOT_DESC] : NULL;

        memset(&rt[id], 0, sizeof(rt[id]));
        rt[id].arena_off = ~0u;
        if (!d || d->magic != MOD_HDR_MAGIC)
            continue;
        rt[id].id = id;
        rt[id].nexport = n;
        rt[id].mclass = d->mclass;
        rt[id].min_ram = d->min_ram;
        rt[id].kind = d->kind;
        rt[id].exit_policy = d->exit_policy;
        rt[id].abi_major = d->abi_major;
        rt[id].abi_minor = d->abi_minor;
        rt[id].work_kb = d->work_kb;
        rt[id].flags = MOD_RT_INPAK;
        rt_check(id);
#ifndef IPODUI_MODULES
        /* L3: a game's working block (6-12 KB, sdk.c) comes from the
         * module arena. A build without one (a resident build: today's
         * release, and sim-ui) has only the 9 KB screen arena, and a
         * core_alloc at game entry would take the bytes from playback (a
         * stop and restart while music plays: playback.c's shrink). So
         * no game is offered there: its row is hidden as on a unit that
         * can't run it. */
        if (d->kind == MOD_KIND_SDK)
            rt_hide(id, MOD_WHY_ARENA);
#endif
    }
}

const void *mod_export_now(enum mod_id id, int slot)
{
    int n;
    const void *const *t;

    if ((unsigned)id >= MOD__N)
        return NULL;
    t = static_tab(id, &n);
    return t && (unsigned)slot < (unsigned)n ? t[slot] : NULL;
}
#endif /* !IPODUI_MODLOAD */

#ifndef IPODUI_MODULES
/* -- a resident build: no arena ------------------------------------------ */
/* ui_main.c calls mod_init()/mod_pass() only in a modules build: the
 * table fills at its first use */
static void rt_init_once(void)
{
    static bool done;

    if (!done)
    {
        done = true;
        static_init();
    }
}

/* The working block comes from the screen arena (screen_stack.h), not a
 * core_alloc: nothing is allocated while music plays, and the arena's
 * mark returns it when the screen whose enter() asked leaves. An Extra
 * asks from its root screen's enter(), so the block lives exactly as long
 * as the Extra (its last screen to leave). */
void *mod_work_alloc(enum mod_id id, size_t bytes)
{
    void *p;

    rt_init_once();
    if ((unsigned)id >= MOD__N || bytes > (size_t)rt[id].work_kb * 1024u)
    {
        ui_log("mod", "work refused id=%d need=%lu", (int)id,
               (unsigned long)bytes);
        return NULL;
    }
    p = ui_arena_alloc(bytes);
    UPDATE_TRACE("mod work id=%s bytes=%lu ok=%d", mod_names[id],
                 (unsigned long)bytes, p ? 1 : 0);
    return p;
}

bool mod_image_range(enum mod_id id, uintptr_t *lo, uintptr_t *hi)
{
    (void)id; (void)lo; (void)hi;
    return false;                       /* resident: no module image */
}

bool mod_room_for(enum mod_id id)
{
    return (unsigned)id < MOD__N;       /* resident: nothing to place */
}

void mod_init(void)
{
    rt_init_once();
}

int mod_owner(const struct ui_screen_ops *ops)
{
    int id, n;

    for (id = 0; ops && id < MOD__N; id++)
    {
        const void *const *t = static_tab(id, &n);

        if (t && n > MOD_SLOT_FIRST && t[MOD_SLOT_FIRST] == ops)
            return id;
    }
    return -1;
}

void mod_pass(void)
{
    struct ui_screen *top = ui_top();
    int m = top ? mod_owner(top->ops) : -1;

    if (m < 0 && top)
        m = sdk_screen_mod(top->ops);   /* a game: its own visit */
    ui_boost_burst_poll();
    modclass_top(m >= 0 ? m : extras_top_module());
    modclass_pass();
}

void mod_usb_connect(void) {}
void mod_usb_release(void) {}
const void *mod_export(enum mod_id id, int slot)
{
    return mod_export_now(id, slot);
}
void mod_warm(enum mod_id id) { (void)id; }
#else /* IPODUI_MODULES */

/* The module arena: reserved once at shell start, pinned, counted in the
 * floor (the simulator reserves it too, so its trace is the device's).
 * Modules use [0, MOD_ARENA_USE); the top MOD_IDX_STAGE bytes are where
 * the pack index is read and checked. */
static uint8_t *arena;
#ifdef IPODUI_MODLOAD
static uint8_t use_clock;               /* LRU clock (mod_rt.lru) */
#endif
static uint32_t mount_gen = 1;

/* Class A working blocks: at most two Extras hold one at a time (one
 * open, one under a ring or a pushed Extra) */
#define WORK_SLOTS 2
static struct { uint32_t off, len; int8_t id; } work[WORK_SLOTS] = {
    { 0, 0, -1 }, { 0, 0, -1 },
};

#ifdef HAVE_IPOD_UI_DEVMODE
/* developer builds only: the load statistics (devloader's view) */
static struct mod_stat {
    uint32_t loads, moves, evictions, last_us, max_us, t0;
} mstat[MOD__N];
#define STAT_INC(id, f) (mstat[id].f++)
#else
#define STAT_INC(id, f) ((void)(id))
#endif

#ifdef IPODUI_MODLOAD
static void touch(int id)
{
    rt[id].lru = ++use_clock;
}
#endif

static uint32_t rt_len(int id)
{
    return rt[id].arena_off == ~0u ? 0 : mod_rt_footprint(&rt[id]);
}

static int used_spans(struct mod_span *sp, int skip)
{
    int i, n = 0;

    for (i = 0; i < MOD__N; i++)
        if (i != skip && rt[i].arena_off != ~0u)
        {
            sp[n].off = rt[i].arena_off;
            sp[n].len = rt_len(i);
            n++;
        }
    for (i = 0; i < WORK_SLOTS; i++)
        if (work[i].id >= 0)
        {
            sp[n].off = work[i].off;
            sp[n].len = work[i].len;
            n++;
        }
    return n;
}
#define SPANS_MAX (MOD__N + WORK_SLOTS)

/* -- the core's build ID ------------------------------------------------- */
/* tools/modpak.py hashes the core's image (this word zeroed) and every
 * module image into the build ID, writes it into the pack and, after the
 * objcopy, into this word of rockbox.bin (found by the marker before it;
 * rockbox.elf is never changed). volatile: nothing may fold the
 * placeholder. */
#ifdef IPODUI_MODLOAD
const volatile uint32_t mod_core_build_id[2]
    __attribute__((used, aligned(4))) = { 0x6b6f7250u, 0x4d4f4442u };
#define CORE_BUILD_ID() (mod_core_build_id[1])
#else
#define CORE_BUILD_ID() 1u              /* static modules: no pack */
#endif

#ifdef IPODUI_MODLOAD
/* -- the pack and the read worker ---------------------------------------- */
#define MODPAK_PATH ROCKBOX_DIR "/modules.pak"
#define LOAD_WAIT   (HZ * 6)            /* a spin-up and a retry */
#define STAGE()     ((struct modpak_hdr *)(arena + MOD_ARENA_USE))

static uint32_t link_base;              /* the pack's, valid while ok */
static int pak_fd = -1;                 /* the storage thread's */
static int8_t pak_state;                /* 0 closed, 1 ok, <0 refused */
static uint32_t pak_bytes, fletcher_us;
static bool pak_drop;                   /* close once the worker is idle */

enum { JOB_OPEN, JOB_MOD, JOB_SNAP };

/* One job in flight, on the storage thread (storage_post_job()). The UI
 * thread never touches pak_fd, the index stage or the job's buffer while
 * busy.  P19 S0: the record (its semaphore, busy and rc) is uncached, so
 * a front end on the COP can wait on it (xcore.h). */
static struct {
    struct semaphore done;
    volatile int8_t busy;               /* 1 posted, 0 finished */
    volatile int8_t reaped;             /* the UI took the result */
    int8_t kind, id;
    volatile int rc;
    uint32_t off, bytes, sum, sum_us;
    unsigned stack_pct;                 /* the storage thread's high-water */
    bool verify;
    void *dst;
} job XCORE_DATA = {   /* uncached (P19 S0), initialised: .ncdata */
    /* Nothing to reap until a job is posted. Zeroed, the struct would
     * read as a finished, unreaped JOB_OPEN with rc MODPAK_OK: a reap()
     * before mod_init() (the Settings warm-up in ui_theme_init()) then
     * "opened" the pack, compacted an index read through the NULL arena
     * and left every module absent until a reboot (P12 device bench,
     * 2026-10-24: n=6656 bytes=0, every load rc=-4). */
    .reaped = 1, .id = -1,
};

/* C1 (rev 2): the 2 KB index is read in one read into the arena's top
 * MOD_IDX_STAGE bytes and checked there; the UI thread compacts it into
 * rt[] when it reaps the job. This frame holds only scalars. */
static void job_open(void)              /* the storage thread */
{
    struct modpak_hdr *h = STAGE();
    off_t size;
    int rc = MODPAK_EBOUNDS;

    pak_fd = open(MODPAK_PATH, O_RDONLY);
    if (pak_fd < 0)
    {
        job.rc = -100;                  /* missing */
        return;
    }
    size = lseek(pak_fd, 0, SEEK_END);
    if (size > 0 && lseek(pak_fd, 0, SEEK_SET) == 0
        && read(pak_fd, h, MODPAK_INDEX) == MODPAK_INDEX)
        rc = modpak_check(h, (uint32_t)size, CORE_BUILD_ID());
    pak_bytes = size > 0 ? (uint32_t)size : 0;
    if (rc != MODPAK_OK)
    {
        close(pak_fd);
        pak_fd = -1;
    }
    job.rc = rc;
}

static void job_run(void)               /* the storage thread */
{
    int rc = -1;

    switch (job.kind)
    {
    case JOB_OPEN:
        job_open();
        rc = job.rc;
        break;
    case JOB_MOD:
        if (pak_fd >= 0 && lseek(pak_fd, job.off, SEEK_SET) == (off_t)job.off)
            rc = read(pak_fd, job.dst, job.bytes);
        if (rc == (int)job.bytes && job.verify)
        {
            uint32_t t = NOW_US();

            if (mod_fletcher32(job.dst, job.bytes) != job.sum)
                rc = MODPAK_ESUM;
            job.sum_us = NOW_US() - t;
        }
        break;
    case JOB_SNAP:
    {
        int fd = open(ROCKBOX_DIR "/snap.pak", O_RDONLY);

        if (fd >= 0)
        {
            if (lseek(fd, job.off, SEEK_SET) == (off_t)job.off)
                rc = read(fd, job.dst, job.bytes);
            close(fd);
        }
        break;
    }
    }
    {
        struct thread_debug_info info;

        if (thread_get_debug_info(thread_self(), &info) >= 0)
            job.stack_pct = info.stack_usage;
    }
    job.rc = rc;
    job.busy = 0;
    semaphore_release(&job.done);
}

static bool job_post(int kind, int id, uint32_t off, void *dst,
                     uint32_t bytes)
{
    if (job.busy || !job.reaped)
        return false;
    job.kind = kind;
    job.id = id;
    job.off = off;
    job.dst = dst;
    job.bytes = bytes;
    job.rc = -1;
    job.busy = 1;
    job.reaped = 0;
    if (!storage_post_job(job_run))
    {
        job.busy = 0;
        job.reaped = 1;
        return false;
    }
    return true;
}

/* Waits while a job runs (the UI thread sleeps). False: still running
 * after the wait; the caller then leaves everything the job uses alone. */
static bool job_wait(long ticks)
{
    long until = current_tick + ticks;

    while (job.busy)
    {
        if (TIME_AFTER(current_tick, until))
            return false;
        semaphore_wait(&job.done, HZ / 10);
    }
    return true;
}
#endif /* IPODUI_MODLOAD */

/* -- the arena ------------------------------------------------------------ */
#ifdef IPODUI_MODLOAD
static bool evictable(int i)
{
    return rt[i].arena_off != ~0u && rt[i].refs == 0
           && rt[i].state != MOD_LOADING
           && !(rt[i].flags & (MOD_RT_RELEASED | MOD_RT_PINNED));
}

static void evict(int i)
{
    ui_log("mod", "evict id=%s", mod_names[i]);
    rt[i].arena_off = ~0u;
    rt[i].state = MOD_ABSENT;
    STAT_INC(i, evictions);
}

/* Slides idle modules down to the lowest free offsets: memmove, the
 * relocations again by the move's delta, .bss zeroed */
static void compact(void)
{
    bool moved = false;

    for (;;)
    {
        struct mod_span sp[SPANS_MAX];
        int i, lo = -1;
        long to = -1;

        for (i = 0; i < MOD__N; i++)
        {
            long t;

            if (!evictable(i) || rt[i].state != MOD_READY
                || (lo >= 0 && rt[lo].arena_off < rt[i].arena_off))
                continue;
            t = mod_fit(sp, used_spans(sp, i), MOD_ARENA_USE, rt_len(i));
            if (t >= 0 && (uint32_t)t < rt[i].arena_off)
            {
                lo = i;
                to = t;
            }
        }
        if (lo < 0)
            break;
        {
            struct mod_rt *m = &rt[lo];
            struct modpak_ent e;
            uint8_t *dst = arena + to;
            uint32_t at;

            mod_rt_ent(m, &e);
            at = modpak_reloc_at(&e);
            memmove(dst, arena + m->arena_off, rt_len(lo));
            memset(dst + e.image, 0, at - e.image);
            mod_relocate(dst, e.image, (const uint16_t *)(dst + at),
                         e.nreloc, (int32_t)(to - (long)m->arena_off));
            ui_log("mod", "move id=%s from=%lu to=%ld", mod_names[lo],
                   (unsigned long)m->arena_off, to);
            m->arena_off = (uint32_t)to;
            STAT_INC(lo, moves);
            moved = true;
        }
    }
    if (moved)
        commit_discard_idcache();
}

/* The arena as mod_room_find() sees it (modload_core.c, host-tested in
 * tests/modload): the policy for images and working blocks alike */
static bool room_span(void *ctx, int i, struct mod_span *sp)
{
    (void)ctx;
    if (rt[i].arena_off == ~0u)
        return false;
    sp->off = rt[i].arena_off;
    sp->len = rt_len(i);
    return true;
}

static int room_age(void *ctx, int i)
{
    (void)ctx;
    if (evictable(i))
        return (uint8_t)(use_clock - rt[i].lru);
    /* H1: the sync indexer, pinned by a build running on the storage
     * worker, lets go when asked (xindex_cancel()): interactive first */
    if (i == MOD_XINDEX && (rt[i].flags & MOD_RT_PINNED) && xindex_busy())
        return MOD_AGE_YIELD;
    return MOD_AGE_HELD;
}

static void room_evict(void *ctx, int i)
{
    (void)ctx;
    evict(i);
}

static void room_compact(void *ctx)
{
    (void)ctx;
    compact();
}
#define ROOM_EVICT   room_evict
#define ROOM_COMPACT room_compact
#else /* the simulator: static modules, never in the arena */
static bool room_span(void *ctx, int i, struct mod_span *sp)
{
    (void)ctx; (void)i; (void)sp;
    return false;
}

static int room_age(void *ctx, int i)
{
    (void)ctx; (void)i;
    return MOD_AGE_HELD;
}
#define ROOM_EVICT   NULL
#define ROOM_COMPACT NULL
#endif /* IPODUI_MODLOAD */

/* the working blocks: never moved, never evicted while they live */
static int room_extra(void *ctx, struct mod_span *sp, int max)
{
    int i, n = 0;

    (void)ctx;
    for (i = 0; i < WORK_SLOTS && n < max; i++)
        if (work[i].id >= 0)
        {
            sp[n].off = work[i].off;
            sp[n].len = work[i].len;
            n++;
        }
    return n;
}

static const struct mod_room room = {
    .n = MOD__N, .ctx = NULL, .span = room_span, .age = room_age,
    .extra = room_extra, .evict = ROOM_EVICT, .compact = ROOM_COMPACT,
};
_Static_assert(MOD__N + WORK_SLOTS <= MOD_ROOM_SPANS,
               "the room policy sees every span");

/* H1: a user opened module id while a sync build holds the indexer (or
 * the storage worker, which would queue the read behind the build): the
 * build stops at its next file and is owed again (xindex_poll()) */
static void user_wants(int id)
{
    if (id != MOD_XINDEX && xindex_busy())
    {
        ui_log("mod", "yield build for id=%s", mod_names[id]);
        UPDATE_TRACE("mod yield build id=%s", mod_names[id]);
        xindex_cancel();
    }
}

#ifdef IPODUI_MODLOAD
/* An arena offset for need bytes (mod_room_find()); never evicts id */
static long place(int id, uint32_t need)
{
    return mod_room_find(&room, MOD_ARENA_USE, need, id);
}

static void fail(int id, int rc)
{
    if (id < 0 || id >= MOD__N)
        return;
    ui_log("mod", "load id=%s failed rc=%d", mod_names[id], rc);
    rt[id].arena_off = ~0u;
    rt[id].state = MOD_FAILED;
}

/* The pack's index, checked in the stage: compacted into rt[] (UI
 * thread). Runtime state (a module in RAM, its holds) is kept: one build
 * ID means the same images. */
static void compact_index(void)
{
    const struct modpak_hdr *h = STAGE();
    int id;

    link_base = h->link_base;
    for (id = 0; id < MOD__N; id++)
    {
        const struct modpak_ent *e = modpak_find(h, id);

        rt[id].flags &= ~(MOD_RT_INPAK | MOD_RT_HIDDEN);
        if (!e || mod_rt_fill(&rt[id], e) != MODPAK_OK)
            continue;
        rt[id].flags |= MOD_RT_INPAK;
        rt_check(id);
    }
}

/* The UI thread takes a finished job's result */
static void reap(void)
{
    struct mod_rt *m;
    struct modpak_ent e;
    const struct mod_desc *d;
    uint8_t *base;
    int id = job.id;

    if (!arena || job.busy)
        return;                         /* before mod_init(): nothing ran */
    if (pak_drop)
    {
        pak_drop = false;               /* a USB connect timed out on a job */
        if (pak_fd >= 0)
            close(pak_fd);
        pak_fd = -1;
        pak_state = 0;                  /* reopened at the next attempt */
    }
    if (job.reaped)
        return;
    job.reaped = 1;
    if (job.kind == JOB_OPEN)
    {
        pak_state = job.rc == MODPAK_OK ? 1 : (int8_t)MAX(job.rc, -100);
        if (pak_state > 0)
        {
            compact_index();
            /* D-M3: the storage thread's high-water at pack open */
            ui_log("mod", "job_open stk=%u%% n=%u bytes=%lu gen=%lu",
                   job.stack_pct, STAGE()->count, (unsigned long)pak_bytes,
                   (unsigned long)mount_gen);
            UPDATE_TRACE("mod job_open stk=%u", job.stack_pct);
        }
        else
        {
            ui_log("mod", "pak refused rc=%d", job.rc);
            fail(id, job.rc);
        }
        return;
    }
    if (job.kind != JOB_MOD || id < 0)
        return;
    m = &rt[id];
    mod_rt_ent(m, &e);
    if (job.rc != (int)modpak_read_bytes(&e))
    {
        fail(id, job.rc < 0 ? job.rc : MODPAK_EBOUNDS);
        return;
    }
    if (job.verify)
    {
        m->flags |= MOD_RT_VERIFIED;
        fletcher_us = job.sum_us;
    }
    base = arena + m->arena_off;
    if (mod_settle(base, &e, link_base, MOD_RUN_ADDR(base)) != MODPAK_OK)
    {
        fail(id, MODPAK_ERELOC);
        return;
    }
    /* slot 0: the module header, inside the image, equal to the entry */
    d = e.nexport > MOD_SLOT_DESC ? MOD_EXPORT_AT(base, MOD_SLOT_DESC) : NULL;
    if (!d || (const uint8_t *)d < base
        || (const uint8_t *)d + sizeof(*d) > base + e.image
        || mod_desc_check(d, &e) != MODPAK_OK)
    {
        rt_hide(id, MOD_WHY_HDR);
        fail(id, MODPAK_EHDR);
        return;
    }
    /* The words the CPU just wrote must reach memory before the I-cache
     * sees them. PP5022 cache maintenance is whole-cache only (data-paths
     * "PP cache maintenance"), so this is the range op on this chip. No
     * COP flush: the COP never runs module code, and the arena was carved
     * before any codec ran there. */
    commit_discard_idcache();
    m->state = MOD_READY;
    STAT_INC(id, loads);
#ifdef HAVE_IPOD_UI_DEVMODE
    mstat[id].last_us = NOW_US() - mstat[id].t0;
    if (mstat[id].last_us > mstat[id].max_us)
        mstat[id].max_us = mstat[id].last_us;
#endif
    UPDATE_TRACE("mod load id=%s", mod_names[id]);
    ui_log("mod", "load id=%s base=%lu bytes=%lu relocs=%lu sum=%d"
           " sum_us=%lu stk=%u%%", mod_names[id], (unsigned long)m->arena_off,
           (unsigned long)modpak_read_bytes(&e), (unsigned long)e.nreloc,
           job.verify ? 1 : 0, (unsigned long)(job.verify ? job.sum_us : 0),
           job.stack_pct);
}

/* Moves module id one step towards READY without waiting: opens the pack
 * or starts the read on the storage worker */
static void kick(int id)
{
    struct mod_rt *m = &rt[id];
    struct modpak_ent e;
    long at;

    if (!arena)
        return;                         /* before mod_init(): the boot's
                                           Settings warm-up is ignored */
    reap();
    if (m->state == MOD_READY || m->state == MOD_LOADING
        || m->state == MOD_FAILED || job.busy)
        return;
    if (pak_state <= 0)
    {
#ifdef HAVE_IPOD_UI_DEVMODE
        if (job_post(JOB_OPEN, id, 0, NULL, 0))
            mstat[id].t0 = NOW_US();
#else
        job_post(JOB_OPEN, id, 0, NULL, 0);
#endif
        return;
    }
    if (!(m->flags & MOD_RT_INPAK) || (m->flags & MOD_RT_HIDDEN))
    {
        fail(id, MODPAK_EBOUNDS);
        return;
    }
    mod_rt_ent(m, &e);
    /* H1: the image where its working block also fits (first fit leaves
     * the block's room right above it), so the module never opens into
     * its reduced view for want of a block another idle tenant held */
    at = place(id, modpak_footprint(&e)
                   + (((uint32_t)m->work_kb * 1024u + MOD_ALIGN - 1)
                      & ~(uint32_t)(MOD_ALIGN - 1)));
    if (at == MOD_ROOM_YIELD)
    {
        user_wants(id);                 /* the stand-in kicks again */
        return;
    }
    if (at < 0)
        at = place(id, modpak_footprint(&e));   /* the image alone */
    if (at == MOD_ROOM_YIELD)
    {
        user_wants(id);
        return;
    }
    if (at < 0)
    {
        ui_log("mod", "no room id=%s need=%lu", mod_names[id],
               (unsigned long)modpak_footprint(&e));
        fail(id, MODPAK_EBOUNDS);
        return;
    }
    job.verify = !(m->flags & MOD_RT_VERIFIED);
    job.sum = e.fletcher;
    if (!job_post(JOB_MOD, id, e.offset, arena + at, modpak_read_bytes(&e)))
        return;
    m->arena_off = (uint32_t)at;
#ifdef HAVE_IPOD_UI_DEVMODE
    if (m->state == MOD_ABSENT)
        mstat[id].t0 = NOW_US();
#endif
    m->state = MOD_LOADING;
}

/* A failed module gets another try (the pack is opened again) */
static void retry(int id)
{
    if (rt[id].state != MOD_FAILED)
        return;
    rt[id].state = MOD_ABSENT;
    if (pak_state < 0)
        pak_state = 0;
}

static const void *export_of(int id, int slot)
{
    if ((unsigned)id >= MOD__N || (job.busy && job.kind == JOB_OPEN))
        return NULL;                    /* rt[] may be changing */
    if (rt[id].state != MOD_READY || (unsigned)slot >= rt[id].nexport)
        return NULL;
    touch(id);
    return MOD_EXPORT_AT(arena + rt[id].arena_off, slot);
}

const void *mod_export_now(enum mod_id id, int slot)
{
    return export_of(id, slot);
}

/* Blocking: for the snapshot batch only (the boot hold, after USB) */
const void *mod_export(enum mod_id id, int slot)
{
    long until = current_tick + LOAD_WAIT;

    if ((unsigned)id >= MOD__N)
        return NULL;
    retry(id);
    if (rt[id].state != MOD_READY)
        user_wants(id);                 /* H1: a game's load, not a build's */
    while (rt[id].state != MOD_READY && rt[id].state != MOD_FAILED
           && !TIME_AFTER(current_tick, until))
    {
        kick(id);
        if (!job_wait(until - current_tick))
            break;
        reap();
    }
    return export_of(id, slot);
}

static int owner_of(const void *p)
{
    int i;

    if (!arena || (const uint8_t *)p < arena
        || (const uint8_t *)p >= arena + MOD_ARENA_USE)
        return -1;
    for (i = 0; i < MOD__N; i++)
        if (rt[i].arena_off != ~0u
            && (const uint8_t *)p >= arena + rt[i].arena_off
            && (const uint8_t *)p < arena + rt[i].arena_off + rt_len(i))
            return i;
    return -1;
}

void mod_screen_hold(const struct ui_screen_ops *ops)
{
    int i = owner_of(ops);

    if (i >= 0 && rt[i].refs < 0xff)
    {
        rt[i].refs++;
        touch(i);
    }
}

void mod_screen_drop(const struct ui_screen_ops *ops)
{
    int i = owner_of(ops);

    if (i >= 0 && rt[i].refs)
    {
        rt[i].refs--;
        rt[i].flags |= MOD_RT_RELEASED; /* its code may be on the stack */
    }
}

/* The panel scheduler may still call the last two keys it was given */
void mod_panel_key(const void *key)
{
    static const void *held[2];

    if (key == held[0])
        return;
    mod_screen_drop(held[1]);
    held[1] = held[0];
    held[0] = owner_of(key) >= 0 ? key : NULL;
    mod_screen_hold(held[0]);
}

void mod_usb_connect(void)
{
    /* the UI thread, before its ack: the job in flight ends first (the
     * storage thread runs it before its own USB event); no writes. The
     * fd and the job's buffer are left alone if it hasn't. */
    pak_drop = true;                    /* reap() closes when idle */
    if (!job_wait(LOAD_WAIT))
        ui_log("mod", "usb: job still running, pack closes after it");
    reap();
}

bool mod_image_range(enum mod_id id, uintptr_t *lo, uintptr_t *hi)
{
    if ((unsigned)id >= MOD__N || !arena || rt[id].state != MOD_READY
        || rt[id].arena_off == ~0u)
        return false;
    *lo = (uintptr_t)(arena + rt[id].arena_off);
    *hi = *lo + rt[id].image + rt[id].bss;
    return true;
}

static bool module_busy(int id)
{
    return rt[id].refs > 0 || (rt[id].flags & MOD_RT_PINNED)
           || extras_module_live(id);
}

#else /* !IPODUI_MODLOAD: the simulator links the modules statically */

/* `mod_delay <ms>` (sim_cmd_mod.c): a load lands that much later, so the
 * stand-in and its swap run in UI scripts; -1: it never lands (the error
 * page) */
static int sim_delay_ticks;
static long sim_land[MOD__N];

void mod_sim_delay(int ms)
{
    sim_delay_ticks = ms < 0 ? -1 : (ms * HZ + 999) / 1000;
}

static void reap(void)
{
    int i;

    for (i = 0; i < MOD__N; i++)
        if (rt[i].state == MOD_LOADING
            && !TIME_BEFORE(current_tick, sim_land[i]))
        {
            if (sim_delay_ticks < 0)
            {
                rt[i].state = MOD_FAILED;
                ui_log("mod", "load id=%s failed rc=-100", mod_names[i]);
                continue;
            }
            rt[i].state = MOD_READY;
            STAT_INC(i, loads);
            UPDATE_TRACE("mod load id=%s", mod_names[i]);
        }
}

static void kick(int id)
{
    if (!arena)
        return;                         /* before mod_init(): the boot */
    if (rt[id].state != MOD_ABSENT)
        return reap();
    if (!(rt[id].flags & MOD_RT_INPAK) || (rt[id].flags & MOD_RT_HIDDEN))
    {
        rt[id].state = MOD_FAILED;      /* not in this build */
        return;
    }
    rt[id].state = MOD_LOADING;
    sim_land[id] = current_tick + MAX(sim_delay_ticks, 0);
    reap();
}

static void retry(int id)
{
    if (rt[id].state == MOD_FAILED)
        rt[id].state = MOD_ABSENT;
}

static const void *export_of(int id, int slot)
{
    if ((unsigned)id >= MOD__N || rt[id].state != MOD_READY)
        return NULL;
    return mod_export_now(id, slot);
}

const void *mod_export(enum mod_id id, int slot)
{
    if ((unsigned)id >= MOD__N)
        return NULL;
    retry(id);
    kick(id);
    while (rt[id].state == MOD_LOADING)
    {
        sleep(1);
        reap();
    }
    return export_of(id, slot);
}
bool mod_image_range(enum mod_id id, uintptr_t *lo, uintptr_t *hi)
{
    (void)id; (void)lo; (void)hi;
    return false;                       /* linked statically */
}
void mod_screen_hold(const struct ui_screen_ops *ops) { (void)ops; }
void mod_screen_drop(const struct ui_screen_ops *ops) { (void)ops; }
void mod_panel_key(const void *key) { (void)key; }
void mod_usb_connect(void) {}

static bool module_busy(int id)
{
    return (rt[id].flags & MOD_RT_PINNED) || extras_module_live(id);
}
#endif /* IPODUI_MODLOAD */

int mod_owner(const struct ui_screen_ops *ops)
{
#ifdef IPODUI_MODLOAD
    return owner_of(ops);
#else
    int id, n, k;

    for (id = 0; ops && id < MOD__N; id++)
    {
        const void *const *t = static_tab(id, &n);

        for (k = MOD_SLOT_FIRST; t && k < n; k++)
            if (t[k] == ops)
                return id;
    }
    return -1;
#endif
}

/* -- Class A working blocks ------------------------------------------------- */
/* From the arena, beside the module (never moved: its module is held
 * while the block lives); freed when the module's last screen has left
 * (module_busy() false, checked once per pass). H1: room is made as for
 * an image (mod_room_find(): idle modules evicted, least recently used
 * first, never the asking one, a held or a pinned one); a sync build in
 * the way is asked to stop and waited for, up to WORK_YIELD_WAIT. */
#define WORK_YIELD_WAIT (HZ / 2)
void *mod_work_alloc(enum mod_id id, size_t bytes)
{
    uint32_t need = (bytes + MOD_ALIGN - 1) & ~(uint32_t)(MOD_ALIGN - 1);
    long at;
    int i, w = -1;

    if ((unsigned)id >= MOD__N || !arena
        || bytes > (size_t)rt[id].work_kb * 1024u)
    {
        ui_log("mod", "work refused id=%d need=%lu", (int)id,
               (unsigned long)bytes);
        return NULL;
    }
    for (i = 0; i < WORK_SLOTS; i++)
    {
        if (work[i].id == (int8_t)id)
            return work[i].len >= need ? arena + work[i].off : NULL;
        if (work[i].id < 0 && w < 0)
            w = i;
    }
    at = w < 0 ? -1 : mod_room_find(&room, MOD_ARENA_USE, need, id);
    if (at == MOD_ROOM_YIELD)
    {
        long until = current_tick + WORK_YIELD_WAIT;

        user_wants(id);
        while (xindex_busy() && TIME_BEFORE(current_tick, until))
            sleep(1);                   /* the build reaches its next file */
        at = mod_room_find(&room, MOD_ARENA_USE, need, id);
    }
    UPDATE_TRACE("mod work id=%s bytes=%lu ok=%d", mod_names[id],
                 (unsigned long)bytes, at >= 0 ? 1 : 0);
    if (at < 0)
        return NULL;
    work[w].id = id;
    work[w].off = (uint32_t)at;
    work[w].len = need;
    memset(arena + at, 0, need);
    return arena + at;
}

bool mod_room_for(enum mod_id id)
{
#ifdef IPODUI_MODLOAD
    struct modpak_ent e;

    if ((unsigned)id >= MOD__N || !arena)
        return false;
    if (rt[id].state == MOD_READY)
        return true;
    if (!(rt[id].flags & MOD_RT_INPAK))
        return pak_state <= 0;          /* no index yet: the load decides */
    mod_rt_ent(&rt[id], &e);
    return mod_room_possible(&room, MOD_ARENA_USE, modpak_footprint(&e)
                             + (uint32_t)rt[id].work_kb * 1024u, id) == 1;
#else
    return (unsigned)id < MOD__N && arena; /* static: nothing to place */
#endif
}

static void work_release_idle(void)
{
    int i;

    for (i = 0; i < WORK_SLOTS; i++)
        if (work[i].id >= 0 && !module_busy(work[i].id))
        {
            UPDATE_TRACE("mod work free id=%s", mod_names[work[i].id]);
            work[i].id = -1;
        }
}

#ifdef IPODUI_STANDINS
/* An Extra's stand-in: preload on highlight */
void mod_preload_id(enum mod_id id)
{
    if ((unsigned)id < MOD__N && storage_disk_is_active())
        kick(id);
}
#endif

/* -- the stand-ins and the error page ------------------------------------- */
static int  stub_enter(struct ui_screen *s, const void *arg);
static void stub_draw(struct ui_screen *s, unsigned dirty);
static enum ui_result stub_input(struct ui_screen *s,
                                 const struct ui_input *in);
static int  stub_tick(struct ui_screen *s);

#define STUB(n, label) \
    const struct ui_screen_ops n = { .name = label, .enter = stub_enter, \
        .draw = stub_draw, .input = stub_input, .tick = stub_tick }
STUB(mod_stub_legal, "legal");
STUB(mod_stub_datetime, "datetime");
STUB(mod_stub_checklist, "checklist");
#ifdef HAVE_IPOD_UI_DEVMODE
STUB(mod_stub_devloader, "devloader");
#endif
/* P12: every Extra's root, arg (intptr_t)enum extra_id (extras.c) */
STUB(mod_stub_extra, "extra");

/* The stand-in has the real screen's title and header shape, so the push
 * slides the right geometry and only the content changes at the swap */
enum { GEOM_LIGHT, GEOM_SPLIT, GEOM_DARK, GEOM_NONE };
static const struct {
    const struct ui_screen_ops *stub;
    uint8_t mod, slot;
    int8_t snap;                        /* enum snap_id, or -1 */
    uint8_t geom;
    short title;                        /* LANG id; -1: from the arg */
} stub_map[] = {
    { &mod_stub_legal,     MOD_SETTINGS, MOD_SET_LEGAL, -1, GEOM_LIGHT,
      LANG_IPODUI_LEGAL },
    { &mod_stub_datetime,  MOD_SETTINGS, MOD_SET_DATETIME, -1, GEOM_SPLIT,
      LANG_IPODUI_DATE_TIME },
    { &mod_stub_checklist, MOD_SETTINGS, MOD_SET_CHECKLIST, -1, GEOM_SPLIT,
      -1 },
#ifdef HAVE_IPOD_UI_DEVMODE
    { &mod_stub_devloader, MOD_DEVLOADER, MOD_DEV_SCREEN, SNAP_DEVLOADER,
      GEOM_DARK, -1 },
#endif
};

/* What a stand-in entry stands for */
struct stub_info {
    int mod, slot, snap, geom;
    const void *real_arg;               /* passed to the real enter() */
};

static bool stub_info(const struct ui_screen_ops *ops, const void *arg,
                      struct stub_info *o)
{
    unsigned i;

    if (ops == &mod_stub_extra)
    {
        int x = (int)(intptr_t)arg;

        if ((unsigned)x >= XT__N || extras_table[x].module == XT_NO_MODULE)
            return false;
        o->mod = extras_table[x].module;
        o->slot = extras_table[x].slot;
        o->snap = -1;
        /* P12 W2-W: the reveal's page (Clocks) is headerless */
        o->geom = extras_table[x].split ? GEOM_SPLIT
                : extras_table[x].transition == UI_T_REVEAL ? GEOM_NONE
                : GEOM_LIGHT;
        o->real_arg = NULL;
        return true;
    }
    for (i = 0; i < ARRAYLEN(stub_map); i++)
        if (stub_map[i].stub == ops)
        {
            o->mod = stub_map[i].mod;
            o->slot = stub_map[i].slot;
            o->snap = stub_map[i].snap;
            o->geom = stub_map[i].geom;
            o->real_arg = arg;
            return true;
        }
    return false;
}

static const char *stub_title(const struct ui_screen_ops *ops,
                              const void *arg)
{
    unsigned i;

    if (ops == &mod_stub_extra)
        return str(extras_table[(intptr_t)arg].title_lang);
    for (i = 0; i < ARRAYLEN(stub_map); i++)
        if (stub_map[i].stub == ops && stub_map[i].title >= 0)
            return str(stub_map[i].title);
#ifdef HAVE_IPOD_UI_DEVMODE
    if (ops == &mod_stub_devloader)
        return "Loader & storage";      /* developer only (devloader.c) */
#endif
    /* the checklists: checklist.h's two args */
    return str(arg == (const void *)(intptr_t)2 ? LANG_IPODUI_MUSIC_MENU
                                                : LANG_IPODUI_MAIN_MENU);
}

/* The styled page a module screen becomes when its module can't load
 * (no pack, another build's pack, a bad read): our words, LANG_* */
static void err_draw(struct ui_screen *s, unsigned dirty)
{
    (void)s;
    if (dirty & UI_DIRTY_BODY)
        ui_prompt_draw(LANG_IPODUI_MOD_ERR_HEAD, LANG_IPODUI_MOD_ERR_BODY);
}

static enum ui_result err_input(struct ui_screen *s, const struct ui_input *in)
{
    (void)s;
    if ((in->key == UI_KEY_MENU || in->key == UI_KEY_SELECT) && in->release)
        return UI_POP;
    return UI_HANDLED;
}

static const struct ui_screen_ops mod_err_screen = {
    .name = "moderr", .draw = err_draw, .input = err_input,
};

static void become_error(struct ui_screen *s, int mod)
{
    ui_log("mod", "screen unavailable id=%s", mod_names[mod]);
    s->ops = &mod_err_screen;
    s->title = str(LANG_IPODUI_MOD_ERR_TITLE);
    s->header_w = 0;
    s->priv = NULL;
    ui_invalidate(UI_DIRTY_ALL);
}

/* The module is in: the real screen takes over this stack entry */
static void become_real(struct ui_screen *s, const struct stub_info *k)
{
    const struct ui_screen_ops *real = export_of(k->mod, k->slot);

    if (!real)
    {
        become_error(s, k->mod);
        return;
    }
    s->ops = real;
    s->priv = NULL;
    /* the real enter() starts from a fresh entry's geometry, as after
     * ui_push(): a screen that leaves header_w alone is full width. Kept
     * from the stand-in, a split stand-in made a full-width list "split"
     * (half header, the panel scheduler on) and its scrolled rows painted
     * over the panel (owner, device, 2026-10-24: Extras > Alarms) */
    s->header_w = 0;
    mod_screen_hold(real);              /* screen_stack drops it on leave */
    if (real->enter && real->enter(s, k->real_arg) < 0)
    {
        mod_screen_drop(real);
        become_error(s, k->mod);
        return;
    }
    /* the whole screen again: header (title, width), body and, on a split
     * screen, the panel the real screen picks (its full draw selects it) */
    UPDATE_TRACE("mod swap id=%s", mod_names[k->mod]);
    ui_clear_dirty();
    ui_invalidate(UI_DIRTY_ALL);
}

/* A stand-in's enter never waits: a module already in swaps at once;
 * otherwise this entry shows the stand-in frame (the snapshot, or the
 * list background) and swaps from tick() when the read lands. */
static int stub_enter(struct ui_screen *s, const void *arg)
{
    struct stub_info k;

    if (!stub_info(s->ops, arg, &k))
        return -1;
    if (s->ops == &mod_stub_legal && arg == UI_LEGAL_NOTICE)
        k.geom = GEOM_NONE;             /* the first-boot notice, headerless */
    s->priv = (void *)arg;
    s->title = stub_title(s->ops, arg);
    s->header_w = k.geom == GEOM_DARK ? UI_HEADER_DARK
                : k.geom == GEOM_NONE ? UI_HEADER_NONE
                : k.geom == GEOM_SPLIT ? ui_theme->split_x : 0;
    retry(k.mod);
    if (rt[k.mod].state != MOD_READY)
        user_wants(k.mod);              /* H1: its read waits for no build */
    kick(k.mod);
    reap();
    if (rt[k.mod].state == MOD_READY)
        become_real(s, &k);
    else
        UPDATE_TRACE("mod standin id=%s", mod_names[k.mod]);
    return 0;
}

static void stub_draw(struct ui_screen *s, unsigned dirty)
{
    struct stub_info k;

    if (!(dirty & UI_DIRTY_BODY) || !stub_info(s->ops, s->priv, &k))
        return;
#ifdef HAVE_IPOD_UI_DEVMODE
    if (k.snap >= 0 && mod_snap_show(k.snap))
        return;
#endif
    if (k.geom == GEOM_NONE)
    {
        ui_extras_page_backdrop(0, LCD_HEIGHT, LCD_WIDTH);  /* the map */
        return;
    }
    /* the list's empty background (black under a dark header), the panel
     * of a split screen left as it is */
    {
        int y = s->header_w == UI_HEADER_DARK ? DEVLOADER_SNAP_Y
                                              : ui_theme->header_h;
        int w = s->header_w > 0 ? s->header_w : LCD_WIDTH;

        lcd_set_viewport(NULL);
        lcd_set_drawmode(DRMODE_SOLID);
        lcd_set_foreground(s->header_w == UI_HEADER_DARK ? LCD_BLACK
                                                        : ui_theme->list_bg);
        lcd_fillrect(0, y, w, LCD_HEIGHT - y);
    }
}

static enum ui_result stub_input(struct ui_screen *s, const struct ui_input *in)
{
    (void)s;
    return in->key == UI_KEY_MENU && in->release ? UI_POP : UI_HANDLED;
}

static int stub_tick(struct ui_screen *s)
{
    struct stub_info k;

    if (!stub_info(s->ops, s->priv, &k))
        return TIMEOUT_BLOCK;
    kick(k.mod);
    reap();
    if (rt[k.mod].state == MOD_READY)
        become_real(s, &k);
    else if (rt[k.mod].state == MOD_FAILED)
        become_error(s, k.mod);
    else
        return 1;                       /* poll the read each tick */
    return 0;
}

bool mod_ops_is(const struct ui_screen_ops *have,
                const struct ui_screen_ops *want)
{
    struct stub_info k;

    if (have == want)
        return true;
    return want != &mod_stub_extra && stub_info(want, NULL, &k) && have
           && have == export_of(k.mod, k.slot);
}

void mod_preload(const struct ui_screen_ops *target)
{
    struct stub_info k;

    /* a hint never spins a sleeping disk up */
    if (target == &mod_stub_extra || !stub_info(target, NULL, &k)
        || !storage_disk_is_active())
        return;
    kick(k.mod);
}

void mod_warm(enum mod_id id)
{
    /* entering the menu that leads to the module: spin up if need be
     * (#104), so the select finds it in */
    if ((unsigned)id < MOD__N)
        kick(id);
}


/* -- common ----------------------------------------------------------------- */
static long batch_due = -1;             /* tick of the next batch */
#ifdef HAVE_IPOD_UI_DEVMODE
static bool snap_first;                 /* a first visit wants a capture */
static void snap_batch(const char *why, bool create);
#endif

void mod_init(void)
{
    int h = core_alloc(MOD_ARENA_SIZE + MOD_ALIGN - 1), i;

#ifdef IPODUI_MODLOAD
    for (i = 0; i < MOD__N; i++)
        rt[i].arena_off = ~0u;          /* every entry absent until the pack */
#else
    (void)i;
    static_init();
#endif

    if (h > 0)
    {
        core_pin(h);
        arena = (uint8_t *)(((uintptr_t)core_get_data(h) + MOD_ALIGN - 1)
                            & ~(uintptr_t)(MOD_ALIGN - 1));
    }
#ifdef IPODUI_MODLOAD
    semaphore_init(&job.done, 1, 0);
    job.reaped = 1;
    job.id = -1;
#endif
    UPDATE_TRACE("mod arena reserve size=%u ok=%d",
                 (unsigned)(MOD_ARENA_SIZE + MOD_ALIGN - 1), arena ? 1 : 0);
    /* at=: the run address, so a panic pc maps to module + offset (each
     * "mod load" line's base= is an offset into the arena) */
    ui_log("mod", "arena size=%u ok=%d at=%08lx", (unsigned)MOD_ARENA_SIZE,
           arena ? 1 : 0, (unsigned long)MOD_RUN_ADDR(arena));
    batch_due = current_tick;           /* the boot hold */
}

void mod_pass(void)
{
    int i;

    /* a hold released in the last pass is no longer on the call stack */
    for (i = 0; i < MOD__N; i++)
        rt[i].flags &= ~MOD_RT_RELEASED;
#ifdef IPODUI_MODLOAD
    reap();                             /* a preload landed */
    /* H1: a builder done with (mod_discard()) leaves the arena now */
    for (i = 0; i < MOD__N; i++)
        if ((rt[i].flags & MOD_RT_DISCARD) && evictable(i))
        {
            rt[i].flags &= ~MOD_RT_DISCARD;
            evict(i);
        }
#endif
    work_release_idle();
    ui_boost_burst_poll();
    {
        struct ui_screen *top = ui_top();
        int m = top ? mod_owner(top->ops) : -1;

        if (m < 0 && top)
            m = sdk_screen_mod(top->ops);
        modclass_top(m >= 0 ? m : extras_top_module());
    }
    modclass_pass();
#ifdef HAVE_IPOD_UI_DEVMODE
    if (batch_due >= 0 && !TIME_BEFORE(current_tick, batch_due))
    {
        batch_due = -1;
        snap_batch(mount_gen > 1 ? "usb" : "boot", false);
    }
    if (snap_first)
    {
        snap_first = false;
        snap_batch("first", true);
    }
#endif
}

void mod_usb_release(void)
{
    /* no disk access here: the pack reopens at the next load, the batch
     * runs a moment later from the main loop (the post-USB moment) */
    int i;

    mount_gen++;
    for (i = 0; i < MOD__N; i++)
        rt[i].flags &= ~MOD_RT_VERIFIED;    /* a new mount generation */
    batch_due = current_tick + HZ;
}

#ifdef HAVE_IPOD_UI_DEVMODE
/* The developer view and the snapshots: the only snapshot tenant is a
 * developer screen, so a release build carries neither */
int mod_count(void)
{
    return MOD__LOADER;
}

void mod_get_info(int i, struct mod_info *out)
{
    memset(out, 0, sizeof(*out));
    if ((unsigned)i >= MOD__LOADER)
        return;
    out->name = mod_names[i];
    out->state = rt[i].state;
    out->verified = !!(rt[i].flags & MOD_RT_VERIFIED);
    out->refs = rt[i].refs;
    out->offset = rt[i].arena_off;
    out->bytes = rt_len(i);
    out->loads = mstat[i].loads;
    out->moves = mstat[i].moves;
    out->evictions = mstat[i].evictions;
    out->last_us = mstat[i].last_us;
    out->max_us = mstat[i].max_us;
    out->nreloc = (rt[i].flags & MOD_RT_INPAK) ? rt[i].nreloc : 0;
}

void mod_arena_get(struct mod_arena_info *out)
{
    struct mod_span sp[SPANS_MAX];
    int i, n = used_spans(sp, -1);
    uint32_t largest = 0, at = 0;

    memset(out, 0, sizeof(*out));
    out->size = arena ? MOD_ARENA_USE : 0;
    for (i = 0; i < n; i++)
        out->used += sp[i].len;
    for (;;)                            /* the largest gap, in address order */
    {
        uint32_t next = out->size;
        int j, k = -1;

        for (j = 0; j < n; j++)
            if (sp[j].off >= at && sp[j].off < next)
            {
                next = sp[j].off;
                k = j;
            }
        if (next - at > largest)
            largest = next - at;
        if (k < 0)
            break;
        at = sp[k].off + sp[k].len;
    }
    out->largest_free = arena ? largest : 0;
    out->build_id = CORE_BUILD_ID();
    out->mount_gen = mount_gen;
#ifdef IPODUI_MODLOAD
    out->pak_bytes = pak_bytes;
    out->fletcher_us = fletcher_us;
    out->pak_open = pak_fd >= 0;
    out->pak_ok = pak_state > 0;
    out->modload = 1;
#endif
}

/* -- snapshots --------------------------------------------------------------- */
#define SNAP_PATH   ROCKBOX_DIR "/snap.pak"
#define SNAP_MAGIC  0x50414e53u         /* "SNAP" */
#define SNAP_HDR    2048                /* key table, one sector */

struct snap_key { uint32_t build_id, lang, y, h, valid; };
struct snap_def { uint8_t mod, slot; uint16_t y, h; };

static const struct snap_def snap_defs[SNAP__N] = {
    /* tenant B: the body under the standard dark header (devloader.h) */
    [SNAP_DEVLOADER] = { MOD_DEVLOADER, MOD_DEV_SCREEN, DEVLOADER_SNAP_Y,
                         DEVLOADER_SNAP_H },
};
static struct snap_key snap_keys[SNAP__N];
static bool snap_keys_read, snap_file;

static uint32_t slot_off(int id)
{
    uint32_t off = SNAP_HDR;
    int i;

    for (i = 0; i < id; i++)
        off += (uint32_t)snap_defs[i].h * LCD_WIDTH * sizeof(fb_data);
    return off;
}

static uint32_t slot_bytes(int id)
{
    return (uint32_t)snap_defs[id].h * LCD_WIDTH * sizeof(fb_data);
}

static uint32_t lang_key(void)
{
    const unsigned char *p = global_settings.lang_file;
    uint32_t h = 2166136261u;

    while (*p)
        h = (h ^ *p++) * 16777619u;
    return h;
}

static void snap_key_now(int id, struct snap_key *k)
{
    k->build_id = CORE_BUILD_ID();
    k->lang = lang_key();
    k->y = snap_defs[id].y;
    k->h = snap_defs[id].h;
    k->valid = 1;
}

static void snap_read_keys(void)
{
    int fd;
    uint32_t magic = 0;
    bool ok = false;

    if (snap_keys_read)
        return;
    fd = open(SNAP_PATH, O_RDONLY);
    if (fd >= 0)
    {
        ok = read(fd, &magic, 4) == 4 && magic == SNAP_MAGIC
             && read(fd, snap_keys, sizeof(snap_keys))
                == (ssize_t)sizeof(snap_keys);
        close(fd);
    }
    snap_file = fd >= 0;
    if (!ok)
        memset(snap_keys, 0, sizeof(snap_keys));
    snap_keys_read = true;
}

static bool snap_fresh(int id)
{
    struct snap_key k;

    snap_read_keys();
    snap_key_now(id, &k);
    return memcmp(&k, &snap_keys[id], sizeof(k)) == 0;
}

bool mod_snap_show(enum snap_id id)
{
    fb_data *dst;
    bool ok = false;

    if ((unsigned)id >= SNAP__N)
        return false;
    if (!snap_fresh(id))
    {
        /* no snap.pak yet (only developer screens have snapshots, so a
         * normal unit never gets one): capture after this first visit */
        if (!snap_file)
            snap_first = true;
        return false;
    }
    dst = (fb_data *)lcd_framebuffer_default.fb_ptr
          + snap_defs[id].y * LCD_WIDTH;
#ifdef IPODUI_MODLOAD
    /* full framebuffer rows are one contiguous, aligned run: one DMA
     * command on the storage worker. A job still running after the wait
     * keeps the framebuffer: the screen then draws live over it. */
    /* never waits on another job or a spin-up: then the screen draws
     * live. Its own read, once posted, must finish before the frame is
     * used (a DMA into the framebuffer can't be abandoned): one command,
     * ~7 ms on flash */
    reap();
    if (!job.busy && storage_disk_is_active()
        && job_post(JOB_SNAP, -1, slot_off(id), dst, slot_bytes(id)))
    {
        while (job.busy)
            semaphore_wait(&job.done, HZ / 10);
        ok = job.rc == (int)slot_bytes(id);
        reap();
    }
#else
    {
        int fd = open(SNAP_PATH, O_RDONLY);

        if (fd >= 0)
        {
            if (lseek(fd, slot_off(id), SEEK_SET) == (off_t)slot_off(id))
                ok = read(fd, dst, slot_bytes(id)) == (ssize_t)slot_bytes(id);
            close(fd);
        }
    }
#endif
    UPDATE_TRACE("snap show id=%d ok=%d", (int)id, ok ? 1 : 0);
    return ok;
}

/* The file layer for mod_file_put() (modload_core.c) */
static long io_seek(void *h, uint32_t off)
{
    return lseek(*(int *)h, off, SEEK_SET);
}
static long io_write(void *h, const void *p, uint32_t n)
{
    return write(*(int *)h, p, n);
}
static const struct mod_fileio snap_io = { io_seek, io_write };

/* Renders snapshot id into the framebuffer (no lcd_update) and writes it */
static bool snap_render(int id, int *fd, uint32_t *size)
{
    const struct snap_def *d = &snap_defs[id];
    const struct ui_screen_ops *ops = mod_export(d->mod, d->slot);
    struct ui_screen scr;

    if (!ops || !ops->draw)
        return false;
    memset(&scr, 0, sizeof(scr));
    scr.ops = ops;
    scr.dirty = UI_DIRTY_ALL;
    scr.arena_mark = ui_arena_mark();
    mod_screen_hold(ops);
    if (ops->enter && ops->enter(&scr, DEVLOADER_SNAP_ARG) < 0)
    {
        mod_screen_drop(ops);
        ui_arena_reset(scr.arena_mark);
        return false;
    }
    lcd_set_viewport(NULL);
    ops->draw(&scr, UI_DIRTY_ALL);
    if (ops->leave)
        ops->leave(&scr);
    ui_arena_reset(scr.arena_mark);
    mod_screen_drop(ops);
    return mod_file_put(&snap_io, fd, size, slot_off(id),
                        (fb_data *)lcd_framebuffer_default.fb_ptr
                        + d->y * LCD_WIDTH, slot_bytes(id)) == 0;
}

/* create: make snap.pak (a first visit); else only an existing one is
 * brought up to date, so a unit that never opened a snapshot screen never
 * writes one. In place: a new file is grown with zeros in order (an
 * lseek past the end fails on this file system). */
static void snap_batch(const char *why, bool create)
{
    long t0 = current_tick;
    int i, n = 0, fd = -1;
    uint32_t size = 0, magic = SNAP_MAGIC;

    snap_keys_read = false;
    snap_read_keys();
    for (i = 0; i < SNAP__N; i++)
    {
        struct snap_key k;

        snap_key_now(i, &k);
        if (memcmp(&k, &snap_keys[i], sizeof(k)) == 0)
            continue;
        if (fd < 0)
        {
            fd = open(SNAP_PATH, O_RDWR);
            if (fd < 0 && create)
                fd = open(SNAP_PATH, O_RDWR | O_CREAT, 0666);
            if (fd < 0)
                break;
            size = (uint32_t)MAX(lseek(fd, 0, SEEK_END), 0);
            if (size < SNAP_HDR
                && mod_file_put(&snap_io, &fd, &size, 0, &magic, 4) != 0)
                break;
        }
        snap_keys[i].valid = 0;
        if (snap_render(i, &fd, &size))
        {
            snap_keys[i] = k;
            n++;
        }
    }
    if (fd >= 0)
    {
        mod_file_put(&snap_io, &fd, &size, 0, &magic, 4);
        mod_file_put(&snap_io, &fd, &size, 4, snap_keys, sizeof(snap_keys));
        close(fd);
        ui_invalidate(UI_DIRTY_ALL);    /* the framebuffer was scratch */
    }
    snap_keys_read = false;
    UPDATE_TRACE("snap batch why=%s n=%d", why, n);
    ui_log("mod", "snap batch why=%s n=%d ms=%ld", why, n,
           (current_tick - t0) * (1000 / HZ));
}

#endif /* HAVE_IPOD_UI_DEVMODE */
#endif /* IPODUI_MODULES */
