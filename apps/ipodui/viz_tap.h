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

#ifndef VIZ_TAP_H
#define VIZ_TAP_H

/* The Now Playing visualizer's PCM tap (HAVE_VIZ_TAP; docs/research/
 * bcm2722/visualizer.md section 11).  The samples are the mixer's output
 * frame by frame, as the DAC gets them (after the DSP, the volume ramps
 * and any click or beep), taken at the PCM callback: the window a read
 * returns ends at the frame now playing, so the picture follows what is
 * heard (within one mixer frame, 5.8 ms at 44.1 kHz).
 *
 * Closed, it costs nothing: the mixer's tap pointer is NULL (one load and
 * a not-taken branch per mixer frame, as before) and no memory is held.
 * Open, a ring of 1,024 stereo frames (4 KB on the device; the simulator's
 * mixer frames are 4x longer, so 16 KB there) takes a copy of every mixer
 * frame in the callback; the downmix, decimation and split order are the
 * reader's.  Stereo is kept so the XY scope (styles doc) can read L and R.
 *
 * Memory: the caller's (viz_tap_bytes(n), pinned, until close), or with
 * mem NULL a core_alloc made only when it can't take memory from playback
 * (audio stopped, or core_available() covers it): any core_alloc beside a
 * full audio buffer stops and restarts playback to shrink it (a measured
 * 340 ms gap, docs/hardware.md "Library view pool"), the screen arena's
 * fallback rule (screen_stack.h).  While a track plays the shell passes
 * its own memory (visualizer.md 11.3).
 *
 * Audiobooks never get a visualizer (owner, 2026-10-24): open refuses one
 * and a read while one plays returns zeros and VIZ_TAP_BOOK.  "A book" is
 * np_pb_is_book(), the flag the audiobook Now Playing uses.
 *
 * Cores: open and close run on the CPU (core_alloc, the mixer's hook and
 * its PCM lock are CPU-side): with the UI on the COP they are remote calls
 * (febe_rpc.list; P19 ucop-fix2).  viz_tap_read() may run on either core: the ring and its
 * head live behind the uncached alias, the head has one writer (the PCM
 * callback, CPU FIQ) and the reader only loads.  See the cross-core note
 * in visualizer.md 11.4 for the 19-9 hand-off. */

#include <stdbool.h>
#include <stdint.h>
#include "viz_tap_core.h"

/* viz_tap_open()'s results */
#define VIZ_TAP_OK          0
#define VIZ_TAP_E_SIZE      (-1)    /* n is not 256 or 512 */
#define VIZ_TAP_E_BOOK      (-2)    /* an audiobook is playing */
#define VIZ_TAP_E_NOMEM     (-3)    /* mem NULL and no core_alloc that
                                       leaves playback alone; or bytes
                                       short of viz_tap_bytes(n) */
#define VIZ_TAP_E_BUSY      (-4)    /* already open */

#include <stddef.h>

/* The memory a tap for windows of n samples needs (the ring, its header
 * and the cache-line alignment) */
size_t viz_tap_bytes(unsigned n);
/* Starts the tap for windows of n samples (256 or 512), in mem (bytes,
 * pinned, in SDRAM: the tap reaches it through the uncached alias, which
 * IRAM has none of; not touched by anyone else until viz_tap_close()) or,
 * mem NULL, in a core_alloc as above.  CPU only. */
int viz_tap_open(unsigned n, void *mem, size_t bytes);
/* The newest n samples (n as opened) in split order into out, int16_t
 * aligned; opts VIZ_RD_NORM or 0; f may be NULL.  Returns the
 * VIZ_TAP_* flags (0: a fresh window of sound); with VIZ_TAP_BOOK or
 * VIZ_TAP_CLOSED out holds zeros. */
unsigned viz_tap_read(int16_t *out, unsigned opts, struct viz_tap_frame *f);
/* Stops the tap and frees the ring; no callback runs into it after. */
void viz_tap_close(void);
bool viz_tap_is_open(void);

#endif /* VIZ_TAP_H */
