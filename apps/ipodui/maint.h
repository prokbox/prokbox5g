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
 * iPod UI shell: the paused maintenance window (Phase 16-1, #88)
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
#ifndef _IPODUI_MAINT_H_
#define _IPODUI_MAINT_H_

#include <stdbool.h>
#include <stddef.h>

/* The paused maintenance window (Phase 16-1, #88; docs/phase16-brief.md
 * 3.1, 4.1). One policy for every job that needs memory or a disk wake
 * while the user is not listening: the library commit and reload, the
 * chapter-image books (artcache.c CHAP_LATER).
 *
 * The window takes memory from the paused audio buffer by one partial
 * shrink (playback.c shrink_callback: the paused song restarts at its
 * exact position, silently, and stays paused; the refill happens then,
 * in the dark). It never takes the buffer below the floor (Q11), never
 * asks for a hard shrink, never uses core_alloc_maximum(). The space
 * comes back at the next audio_reset_buffer() (a cold start). */

#define MAINT_PAUSE_TICKS (180*HZ)  /* Q1: paused >= 3 min */
#define MAINT_JOB_TICKS   (300*HZ)  /* a job not done by then lapses */

/* Each holder of the window's memory is its own tenant: one ending
 * (maint_done()) never ends another's, and the regrow waits for all of
 * them. COMMIT: the library commit, its reload and publication (the
 * candidate's view pool, library.c lib_generation_pool_prepare()), on
 * the tagcache thread; CHAPTERS: the chapter-image books (artcache.c);
 * DBQ: a database queue's transient (dbqueue.c job_alloc()); LISTS: the
 * library precompute's transient block (library.c lib_precompute_ctx(),
 * freed before it returns); PANELS (#100): the panel pool's top-up
 * (no allocation of its own: the art builder's reserved decode block;
 * taken for the window's rules and the floor check). */
enum maint_job { MAINT_COMMIT, MAINT_CHAPTERS, MAINT_PANELS, MAINT_DBQ,
                 MAINT_LISTS,
                 /* P12 (W1-N): the sync-time indexes' build block
                  * (xindex.c), stopped or in the paused window */
                 MAINT_XIDX,
                 MAINT__N };

/* Q1: paused >= MAINT_PAUSE_TICKS, the user idle (artcache_user_idle()),
 * no fade, no video; stopped: true; playing: false. Re-check right
 * before each allocation, never across a yield. */
bool maint_window_open(void);
void maint_want(enum maint_job j);     /* a job is owed (any thread) */
bool maint_wanted(void);
/* the job finished (or gave up): logs "maint job=… ok=… ms=…
 * play_during=…"; a no-op for a job that never took the window */
void maint_done(enum maint_job j, bool ok);
long maint_paused_since(void);         /* tick of the pause, -1 otherwise */
/* P19 19-1a: paused, the tick at which the pause is old enough for the
 * window (Q1, MAINT_PAUSE_TICKS); false when not paused. The tagcache
 * thread's dark wait ends there. */
bool maint_window_due(long *at);
/* P18 (#86): the sleep-entry window (Q7), opened and closed by sleep.c
 * on the UI thread: while open, a pause of any age counts and the user is
 * idle. And the pause age restarted at a wake (the tick was frozen). */
void maint_sleep_window(bool open);
bool maint_sleep_window_open(void);    /* any thread */
/* P18 (#86, review M1): from the window's close to the wake (or an
 * abandoned entry), the paused window stays shut: no job takes memory and
 * no regrow restarts the paused queue (a refill) while storage goes off.
 * UI thread. */
void maint_sleep_hold(bool hold);
/* job j is owed (maint_wanted() for one job: the sleep window's log) */
bool maint_wanted_job(enum maint_job j);
void maint_pause_restart(void);

/* The audio file buffer floor of this unit (MEMORYSIZE) */
size_t maint_floor(void);
/* Would taking 'bytes' from the paused buffer keep it >= the floor? */
bool maint_fits(size_t bytes);
/* Could maint_take(j, bytes) succeed now, with no allocation and no
 * disk access: the free heap has them already (counted once: a take
 * made earlier in this window is free memory now, not a second take),
 * or the one shrink it would make keeps the floor. The window itself
 * is not checked. */
bool maint_take_fits(size_t bytes);
/* Makes 'bytes' free for job 'j': true at once when stopped or when the
 * free heap has them already; otherwise, only with the window open and
 * the floor kept, one partial shrink (waits for the silent restart to
 * settle, a few hundred ms) and true. False: the job waits (logged once
 * per pause). Callers allocate right after, with no yield between. */
bool maint_take(enum maint_job j, size_t bytes);
/* job j's take was refused in this pause (the floor): a retry would be
 * refused again until the pause ends (false when stopped) */
bool maint_declined(enum maint_job j);
/* a job holds the window's memory (from maint_take() until maint_done()
 * or MAINT_JOB_TICKS): its steps may use free memory while paused, and
 * PLAY does not stop them */
bool maint_active(void);
/* job j itself holds the window's memory (maint_active() for one tenant) */
bool maint_holds(enum maint_job j);

/* tagcache thread, once a second: pause tracking, PLAY-during notes, and
 * a kick for the art builder when the window opens with books owed */
void maint_poll(void);

#ifdef SIMULATOR
void sim_maint_pause_age(long seconds); /* the pause began this long ago */
/* maint_try: maint_take(MAINT_COMMIT, bytes) from the script, then a
 * core_alloc(bytes) that must not shrink the audio buffer; logged */
void sim_maint_try(long bytes);
int sim_maint_title(char *buf, int len);    /* expect_title */
void sim_maint_hold(const char *job, long bytes);   /* maint_hold */
void sim_maint_drop(const char *job, bool ok);      /* maint_drop */
unsigned long sim_maint_audio_size(void);   /* audio_mark, expect_audio_back */
#endif

#endif /* _IPODUI_MAINT_H_ */
