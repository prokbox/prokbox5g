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
 *  fe_be: the boundary between the UI front end and the playback back end
 * (P19 S1, 19-8a; docs/design/p19-ui-on-cop.md sections 2.3, 4 and 7).
 *
 * The standing split (owner, 2026-10-24): the CPU is the playback back
 * end, the COP the UI front end, the BCM2722 the display.  This header is
 * the contract: what the front end (the shell's loop, its screens and
 * their drawing) may ask of the back end (playback, the playlist, the
 * database, settings files, storage, allocation, power), and how.
 *
 * Three ways across, and no other:
 *  1. **Published state**: fe_now_get() reads the "now" record, a seqlocked
 *     record in uncached memory that only CPU threads write (the playback
 *     event callbacks, in fe_be.c).  Identity (track_id) is separate from
 *     validity (the seqlock's sequence).
 *  2. **Async posts**: be_post(kind, value): playback control and other
 *     fire-and-forget requests; small values only, never a pointer.
 *  3. **Sync calls**: be_call(kind, flags, args, n): the front end's slot
 *     (an uncached record) carries the arguments and results; the cache
 *     operations at the post and the reply come from fe_be_core.c's table.
 *
 * Who serves them (fe_be_mode()):
 *  - FE_INLINE (the default, today's shell): the shell runs on the main
 *    thread and every request is served inline, on the caller's thread.
 *  - FE_CPU_THREAD (HAVE_UI_THREAD_SPLIT, developer switch): the shell's
 *    loop runs on its own "ui" thread on the CPU; the main thread becomes
 *    the back end's service thread be_svc and serves posts and calls.
 *  - FE_COP (developer switch; refused until the boundary is complete):
 *    the "ui" thread on the COP.  fe_be_start() refuses it, loudly, while
 *    any rule it needs is not in place (fe_be.c, fe_cop_refusal()), and
 *    runs the ui thread on the CPU instead.
 * Only the ui thread posts or calls; any other thread (be_svc itself, the
 * art thread, a callback) runs the request inline, so be_svc can never
 * wait on itself and the audio back end never waits on the UI.
 *
 * Copyright (C) 2026 prokopto-dev
 * Copyright (C) 2026 prokopto-dev
 *
 * Portions derived from Rockbox (rockbox.org).
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
#ifndef FE_BE_H
#define FE_BE_H

#include <stdbool.h>
#include <stdint.h>
#include "gcc_extensions.h"
#include "kernel.h"
#include "fe_be_core.h"

enum fe_mode {
    FE_INLINE = 0,          /* the shell on the main thread (default) */
    FE_CPU_THREAD,          /* the shell on its own CPU thread */
    FE_COP,                 /* the shell on the COP */
};

/* Where the shell runs, read once at boot (fe_be.c mode_read()), first
 * match wins:
 *  1. the boot guard: FE_GUARD_FILE names this build, so a COP attempt of
 *     this build stalled or never held: the CPU thread (release and
 *     developer builds; a new build starts without it);
 *  2. developer builds: the developer switch, FE_MODE_MARKER;
 *  3. the hidden config.cfg key "ipodui ui core" (global_settings.
 *     ipodui_ui_core): 0 the build default, 1 inline, 2 cpu, 3 cop;
 *  4. the build default, IPODUI_UI_CORE_DEFAULT (config/ipodvideo-
 *     variant.h): the line the 0.8.0 flip changes. */
#define FE_MODE_MARKER  ROCKBOX_DIR "/ui_split"
#define FE_GUARD_FILE   ROCKBOX_DIR "/ui_guard"
enum fe_mode fe_be_mode(void);          /* this boot's, as running */
enum fe_mode fe_be_mode_next(void);     /* what the next boot will run */
void fe_be_mode_set_next(enum fe_mode m);
/* the developer switch removed: the next boot asks the hidden setting,
 * then the build default (the boot guard still applies) */
void fe_be_mode_clear_next(void);
const char *fe_be_mode_name(enum fe_mode m);

/* main thread, at shell start, before anything posts or reads fe_now:
 * the now record, the event callbacks, the switch */
void fe_be_init(void);
/* main thread: runs shell() as the front end.  FE_INLINE: calls it on
 * this thread (never returns).  Otherwise starts the ui thread running
 * shell() and turns this thread into be_svc (never returns).  A ui thread
 * that cannot start falls back to FE_INLINE, logged. */
void fe_be_start(void (*shell)(void)) NORETURN_ATTR;
/* true on the thread that runs the shell's loop */
bool fe_is_ui_thread(void);

/* ---- the now record -------------------------------------------------- */
#define FE_NOW_STOPPED  0x01    /* the queue stopped (or nothing played
                                 * this boot): Now Playing's row hides */
struct fe_now {
    uint32_t flags;             /* FE_NOW_* */
    uint32_t track_id;          /* identity: bumps on every track change */
    uint32_t status;            /* audio_status() at the last event */
    uint32_t events;            /* events applied (diagnostic) */
    uint32_t sum;               /* canary: febe_sum over the words above */
};
#define FE_NOW_WORDS    (sizeof(struct fe_now) / 4)

/* the front end: a consistent copy.  false: no consistent copy within the
 * retry budget, or a canary mismatch (logged, counted); *out then holds
 * the last good copy */
bool fe_now_get(struct fe_now *out);
/* back end only (CPU, not from an interrupt): marks played this boot */
void be_now_mark_played(void);

/* ---- requests ----------------------------------------------------------- */
enum be_kind {
    BE_NOP = 0,             /* a round trip (the latency probe) */
    BE_AUDIO_PAUSE,
    BE_AUDIO_RESUME,
    BE_NOW_PLAYED,          /* the simulator's "played this boot" seed */
    BE_SETTINGS_SAVE,       /* settings_save(): the config file write */
    BE_PASS,                /* S2: the back end's per-pass services */
    BE_LOG_FLUSH,           /* ui_log_flush() */
    BE_IDLE_FLUSH,          /* call_storage_idle_notifys(true) */
    BE_RCALL,               /* S3: a back-end function called from the COP
                             * (febe_rpc_stubs.S, febe_remote_c()) */
    /* posts (async, no sync point): the per-input and audio-output work
     * a ui thread on the COP must not run itself (ucop-fix, 2026-10-26) */
    BE_SOUND,               /* system_sound_play(value) on the CPU */
    BE_INPUT_NOTE,          /* a key or wheel event: the art builder's and
                             * the clock policy's notes; value: 1 when the
                             * UI's frames compete with decode */
    BE_RCALL_REPORT,        /* log the remote-call profile (bench) */
    BE_KIND_COUNT
};

/* async: never blocks the caller beyond a queue post */
void be_post(enum be_kind kind, intptr_t value);
/* sync: returns the handler's value; args (n <= FEBE_ARG_WORDS) in and
 * out.  flags: FEBE_F_IN_BULK / FEBE_F_WROTE (fe_be_core.h) */
intptr_t be_call(enum be_kind kind, uint32_t flags, uint32_t *args,
                 unsigned n);

/* true on a thread that runs shell code which owns a back-end service's
 * state: be_svc, or the ui thread in FE_INLINE (one thread).  The storage
 * thread and the other back-end threads: false (batcal, ab_store) */
bool fe_be_is_service_thread(void);
/* A shell service's own entry point routes itself: true when the caller
 * is the split-off ui thread, which must ask be_svc instead */
bool fe_be_route(void);

/* the shell's wrappers (one per family; call sites stay readable) */
static inline int fe_settings_save(void)
{
    return (int)be_call(BE_SETTINGS_SAVE, 0, 0, 0);
}
/* the storage idle callbacks now (ab_store, the power log, batcal) */
static inline void fe_idle_flush(void)
{
    (void)be_call(BE_IDLE_FLUSH, 0, 0, 0);
}

/* ---- the pixel hand-off (MF-4; firmware/export/fe_fb.h) ------------- */
/* the fence request a CPU caller of fe_fb_publish() posts to button_queue;
 * the ui thread's input path answers it with fe_fb_serve() */
#define SYS_FE_FB_FENCE MAKE_SYS_EVENT(SYS_EVENT_CLS_MISC, 0x23)
void fe_fb_serve(void);
/* the ui thread, around a wait (input, a call to the back end): on the COP
 * its cache is written back before it blocks, so a CPU reader of the
 * framebuffer needs no fence while it waits */
void fe_wait_begin(void);
void fe_wait_end(void);

/* ---- the boundary's counters (the bench's UI-on-COP soak) ------------ */
struct fe_be_stats {
    uint32_t calls;             /* be_calls from the ui thread */
    uint32_t call_max_us;       /* the slowest round trip */
    uint32_t rcalls;            /* remote calls served (COP mode) */
    uint32_t stalls;            /* watchdog stalls */
    uint32_t coh_fails;         /* canary mismatches: now, slot, call */
    uint32_t fb_timeouts;       /* pixel fences not answered in time */
    uint32_t call_max_nostor_us;/* the slowest round trip that powered no
                                 * storage up on the way (a power-up is as
                                 * slow inline: not the boundary's cost) */
    uint32_t stor_calls;        /* round trips that powered storage up */
    uint32_t post_drops;        /* clicks or input notes not posted: be_svc
                                 * had FE_POSTS_MAX in hand */
    uint32_t offcore;           /* CPU-only entry points reached from the
                                 * COP (xcore.h xcore_offcore; developer
                                 * builds, else 0) */
};
/* a snapshot (any thread; the counters are uncached, each one writer) */
void fe_be_stats_get(struct fe_be_stats *st);

/* the ui thread, once per key or wheel event: the back end's input notes
 * (artcache_note_input(), clockpol_note_input()) by post, not by call */
void fe_note_input(bool ui_competes);

/* uilog.c's COP ring got its first line: be_svc drains it (P19 S7) */
void fe_be_log_kick(void);

#endif /* FE_BE_H */
