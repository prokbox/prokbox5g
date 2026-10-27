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

#ifndef _IPODUI_DRIFT_JOBS_H_
#define _IPODUI_DRIFT_JOBS_H_

/* P19 19-2: the drift as BCM2722 display jobs (jobs.md section 5).
 * preview.c's drift policy (nav_core.c) chooses each frame as before;
 * this file only turns a frame into jobs: the cover (pv.slot, RGB565,
 * 300 x 300 at a 600 B pitch) uploaded once per cover into a J_CLS_DRIFT
 * slot, then BLIT/AVG + SHADE or FADE (J_FADE_SHADE_B) into the front
 * frame's panel rect and a PRESENT.  While it does, the BCM owns that
 * rect (screen truth, bcmjob_owned_set()); preview.c's drift_settle()
 * writes the CPU's own framing back and ends it.
 *
 * Fallback (D-1): a missing op, a slow or failed job, a BCM failure or
 * the developer's "still" give the still cover from pv.slot, no disk,
 * logged "drift still why=...". */

#include <stdbool.h>
#include <stdint.h>
#include "config.h"

#if defined(HAVE_BCM_JOBS) || defined(HAVE_BCM_JOBS_MODEL)
#define HAVE_DRIFT_JOBS

enum dj_path
{
    DJ_PATH_CPU = 0,    /* today's CPU drift */
    DJ_PATH_JOBS,       /* the frame as jobs */
    DJ_PATH_STILL,      /* the developer row's still cover (a BCM
                           failure takes the CPU path instead) */
};

/* The developer switch says jobs (auto: the device's verified image;
 * never the simulator's model unless a script asks) */
bool dj_jobs_wanted(void);

/* The path the drift's next frame takes (logs the still once) */
enum dj_path dj_path(void);

/* One frame as jobs from the cover in core_alloc block h (key: its
 * content).  true: sent (the BCM owns the panel); false: take the CPU
 * path for this frame (preview.c settles first); after a failure
 * dj_path() is CPU for the boot (the drift keeps moving). */
bool dj_pan(int h, uint64_t key, int x, int y);
bool dj_half(int h, uint64_t key, int x0, int y0, int x1, int y1);
bool dj_fade(int h, uint64_t key, int x, int y, int weight);

/* ---- The resident panel (album-render.md sections 6 and 14, the end
 * state; P19 19-6) ---------------------------------------------------------
 * The J_CLS_DRIFT pool holds two 300 x 300 covers: the drift's own (an
 * upload from pv.slot) and the resident panel, the playing album's cover
 * the BCM decoded itself (art_bcm.c), kept READY for the epoch.  Nothing
 * of it is uploaded or read back for display: the drift and the Now
 * Playing row BLIT it by key.  One resident at a time; a new album's
 * decode drops the old one first; an epoch change forgets it (the next
 * decode makes it again, never an upload).  The job owner's thread only
 * (art_bcm_service() installs and drops it for the art thread). */
/* The resident panel is album 'key' (the panel set's key) this epoch */
bool dj_resident(uint64_t key);
/* bcmjob_epoch(), for callers without the job headers */
uint32_t dj_epoch(void);
/* The resident's album key, 0: none this epoch */
uint64_t dj_resident_key(void);
/* Install slot (READY, 300 x 300, decoded) as album key's resident panel:
 * the previous resident (another slot) is freed in ring order, after any
 * queued job that reads it; the drift stops naming it */
void dj_resident_set(uint64_t key, uint32_t slot, uint32_t epoch);
/* Free the resident panel (a new album's decode needs its place) */
void dj_resident_drop(void);
/* Free the drift's own cover slot (not the resident): the merged image's
 * DECODE overlay borrows a J_CLS_DRIFT slot as its work area
 * (album-render.md 5.3), so a decode there needs the pool's second place.
 * The drift uploads its cover again when it next needs it */
void dj_cover_park(void);
/* The Now Playing row's still frame from the resident panel: the framing
 * (x, y) of album key's slot BLITted into the panel rect, shadowed and
 * presented; 'weight' < 256: one cross-fade step toward it from the front
 * frame (the row's arrival, the CPU's ui_panel_fade(raw) on the BCM).
 * false: not resident, or the jobs refused (the CPU's picture stays) */
bool dj_resident_show(uint64_t key, int x, int y, int weight);

/* The BCM shows the Now Playing row's still frame (dj_resident_show() at
 * full weight) and nothing has moved since: no frame is due, so a DECODE
 * may hold the ring behind it */
bool dj_still(void);

/* The BCM holds the panel's pixels */
bool dj_owned(void);
/* The simulator's parity mode: the CPU draws each jobs frame too */
bool dj_parity_mode(void);
/* The panel rect from the framebuffer to the BCM's front frame and the
 * glass through the ring, after anything queued (M1): false, the BCM
 * can't take it (the caller pushes) */
bool dj_resync(void);
/* End the ownership: the queued jobs retired (FENCE, J_T_* deadline),
 * the rect handed back.  The caller writes the CPU's framing into the
 * framebuffer and resyncs it (dj_resync()), never a plain push a late
 * PRESENT could overwrite.  false: the wait timed out (logged). */
bool dj_release(void);

#ifdef HAVE_BCM_JOBS_MODEL
/* B3 parity in the simulator: after a frame sent as jobs, the CPU path
 * drew the same frame into the framebuffer; the model's panel must equal
 * it pixel for pixel ("drift parity bad ..." logged, counted) */
void dj_parity(const char *kind);
#endif

/* The developer row (Developer > Advanced > Video chip, devmode.c):
 * cycles auto, on, off, still */
bool dj_dev_row(void);
#define DRIFT_JOBS_DEV_ITEM \
        { "BCM jobs: drift on video chip (auto/on/off/still)", dj_dev_row },
#endif

#endif
