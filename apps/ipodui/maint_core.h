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
 * iPod UI shell: the paused maintenance window, pure policy (Phase 16-1)
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
#ifndef _IPODUI_MAINT_CORE_H_
#define _IPODUI_MAINT_CORE_H_

#include <stdbool.h>
#include <stddef.h>

/* The pure half of the paused maintenance window (maint.h; host test
 * tests/maint). No kernel, no audio: the glue (maint.c) samples the
 * state and passes it in. */

/* audio_status() bits, as audio.h has them (checked in maint.c) */
#define MAINT_ST_PLAY   0x0001
#define MAINT_ST_PAUSE  0x0002

/* The audio file buffer floor (owner, 2026-10-20; docs/hardware.md):
 * a paused window never takes the buffer below it (Q11) */
#define MAINT_FLOOR_32MB  7500000UL
#define MAINT_FLOOR_64MB 39000000UL

struct maint_in {
    int status;             /* audio_status() */
    long paused_ticks;      /* how long paused; < 0: not known yet */
    bool user_idle;         /* artcache_user_idle(): no key held/recent */
    bool dark;              /* backlight off or the unlit face up (M4:
                               a lit screen on the charger is not dark) */
    bool fading;            /* a fade (P15) or crossfade in progress */
    bool video;             /* the current track is a video (BCM) */
};

enum maint_why {
    MAINT_OPEN = 0,         /* the window is open */
    MAINT_NO_PLAYING,       /* playing (not paused): never */
    MAINT_NO_YOUNG,         /* paused for less than the threshold */
    MAINT_NO_LIT,           /* the user is not idle, or the screen lit */
    MAINT_NO_FADE,          /* a fade or crossfade */
    MAINT_NO_VIDEO,         /* video playback */
};

/* Q1: stopped is always open; paused >= pause_ticks with the user idle
 * and the screen dark, no fade and no video; playing never. */
enum maint_why maint_core_window(const struct maint_in *in,
                                 long pause_ticks);

/* Q11: may 'bytes' come out of an audio file buffer of 'buf' bytes and
 * leave it >= 'floor'? (overflow-safe) */
bool maint_core_fits(size_t buf, size_t bytes, size_t floor);

/* Would a take of 'bytes' succeed (maint.c maint_take()): 'avail' free
 * in the heap covers them already (+ 64: buflib's header), else one
 * shrink of 'bytes' + 'margin' from a buffer of 'buf' keeps 'floor'.
 * Memory an earlier take freed counts once, as free heap (P16 gate
 * fix: not a second shrink on the already shrunk buffer). */
bool maint_core_take_fits(size_t buf, size_t avail, size_t bytes,
                          size_t margin, size_t floor);

#endif /* _IPODUI_MAINT_CORE_H_ */
