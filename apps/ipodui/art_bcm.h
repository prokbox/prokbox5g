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

#ifndef _IPODUI_ART_BCM_H_
#define _IPODUI_ART_BCM_H_

/* P19 19-6: the art builder's covers decoded on the BCM2722 (DECODE,
 * docs/research/bcm2722/album-render.md section 14, the shell recipe).
 *
 * artcache.c's decode_run() step 2 hands one output here (n = 1: the
 * builder's own single decode at dw x dh, cropped to the window, so the
 * pixels equal the CPU path's bit for bit) when the work is marked for
 * the BCM (np-sharp's panel, the Now Playing box).  The answer says what
 * the caller does next, exactly as bcmjob_dec_verdict() maps it:
 *   ART_BCM_OK     the window is in a BCM slot: read it back
 *                  (art_bcm_read(), box-sized bands), check the sums
 *                  (art_bcm_sums_ok()), then art_bcm_release();
 *   ART_BCM_BAD    the source's own verdict (corrupt, truncated): the
 *                  next source, as a failed CPU decode;
 *   ART_BCM_TWIN   no verdict: the CPU code below the splice, unchanged;
 *   ART_BCM_DEFER  cancelled (the user came back, USB, a power-off) or
 *                  the BCM went down mid-call: neither side decodes now,
 *                  the render waits for its next chance.
 * Only the art thread calls art_bcm_decode() and art_bcm_read(); the
 * slot is freed on the job owner's thread (the UI thread's pass,
 * art_bcm_service()), never on the art thread.
 *
 * Mutual exclusion with the UI's own jobs (the drift's frames, the Now
 * Playing marquee's PAN): a DECODE holds the BCM's command ring for its
 * whole run, so a UI job queued behind it would miss its frame (the drift
 * would go still for the boot) and a UI wait would block the UI thread.
 * So a decode starts only while no UI job client is active, and the UI
 * clients take their CPU paths while art_bcm_busy(). */

#include <stdbool.h>
#include <stdint.h>
#include "config.h"

#if defined(HAVE_BCM_JOBS) || defined(HAVE_BCM_JOBS_MODEL)
#define HAVE_ART_BCM

enum art_bcm_verdict
{
    ART_BCM_OK = 0,
    ART_BCM_BAD,
    ART_BCM_TWIN,
    ART_BCM_DEFER,
};

/* The slot's class (album-render.md section 6) */
enum art_bcm_kind
{
    ART_BCM_BOX = 0,                /* the Now Playing box: J_CLS_THUMB */
    ART_BCM_PANEL,                  /* np-sharp's 300 x 300: J_CLS_DRIFT */
};

struct art_bcm_out
{
    int dw, dh;                     /* the source resized to dw x dh */
    int cx, cy, cw, ch;             /* the window kept: the slot */
    enum art_bcm_kind kind;
    uint64_t key;                   /* the album's art key (the output's
                                       tag is added here: section 4.3) */
};

/* The DECODE job is there now (the image offers it, the BCM is up, not
 * marked this boot) and no UI job client holds the BCM */
bool art_bcm_ready(void);
/* The art thread, before a decode it would send to the BCM: as
 * art_bcm_ready(), but when the merged image's overlay is due (DECODE
 * missing until dec-ovl.bin is loaded: album-render.md section 5.3) it
 * first loads it, a second at most in all: it reads the file a piece at a
 * time (its own buffer, the load only) and the job owner's pass puts each
 * to the BCM (woken at once; 'cancel' polled; no file access on the UI
 * thread).  false: the caller's PP path; a load that failed or never came
 * is logged once a boot per cause ("FAULT bcm decode overlay
 * why=wait|refused|gone|buf") and is tried again at the next decode */
bool art_bcm_ready_ovl(bool (*cancel)(void *ctx), void *ctx);

/* One decode of the source at 'off' of 'fd' ('len' bytes; the art
 * thread's fd), blocking.  'sw' x 'sh': the source's size from the
 * caller's probe (the deadline: a big cover passes its own, never the
 * driver's 3 s cap).  'cancel' (polled about every millisecond): true
 * ends it (ART_BCM_DEFER).  *read_err: the PP's own read failed (with
 * ART_BCM_BAD: the caller's source_retry). */
enum art_bcm_verdict art_bcm_decode(int fd, long off, uint32_t len,
                                    int sw, int sh,
                                    const struct art_bcm_out *o,
                                    bool (*cancel)(void *ctx), void *ctx,
                                    bool *read_err);

/* Rows [y, y + h) of the decoded window into px (pitch pixels a row),
 * summed as they come (COMMIT's F16w).  false: the BCM is gone (another
 * epoch) or the read failed: take the CPU path */
bool art_bcm_read(int y, int h, uint16_t *px, int pitch);
/* The rows read since the decode (or art_bcm_rewind()) sum to what the
 * BCM committed: the whole window, read once, intact */
bool art_bcm_sums_ok(void);
void art_bcm_rewind(void);
/* Done with the slot: freed at the UI thread's next pass; a resident
 * panel (art_bcm_keep()) stays READY for the epoch */
void art_bcm_release(void);

/* ---- The resident panel (album-render.md sections 6 and 14, the end
 * state) -------------------------------------------------------------------
 * np-sharp's 300 x 300 panel stays on the BCM after its decode: the
 * decode's J_CLS_DRIFT slot becomes the drift pool's resident panel
 * (drift_jobs.h), and the drift and the Now Playing row BLIT it by key,
 * with no read-back at all.  The PP keeps only what must outlive a cut,
 * npcover.pak, read back from the slot in the background (the art thread,
 * idle, never while the row shows it: art_bcm_np_shown()).
 *
 * art_bcm_keep(): after an ART_BCM_OK panel (the art thread), the slot is
 * installed as the resident at the job owner's next pass (art_bcm_service())
 * and art_bcm_release() no longer frees it.  A portrait panel (fewer than
 * 300 columns) is never resident: its bars are the PP's.
 * A panel decode of another album first drops the old resident (its place
 * in the pool).  An epoch change loses it; the next render decodes again,
 * never an upload. */
void art_bcm_keep(void);
/* album 'key' should be decoded again only to be resident (its panel file
 * exists, the BCM lost it with an epoch): DECODE offered, the panel on the
 * BCM, and it isn't the resident now (any thread: volatile reads) */
bool art_bcm_resident_wanted(uint64_t key);
/* the resident's album (published by the owner's pass), 0: none */
uint64_t art_bcm_resident_key(void);
/* moves at every resident installed (the preview's instant swap) */
int art_bcm_resident_seq(void);
/* The preview tells: the Now Playing row is on the lit screen (the
 * background read-back waits, and stops between bands, while it is) */
void art_bcm_np_shown(bool shown);
bool art_bcm_np_is_shown(void);

/* np-sharp's 300 x 300 panel on the BCM: on in every build since it stays
 * resident (album-render.md 14, the end state: the drift and the Now
 * Playing row BLIT it; its read-back, about 45 k port words, 2.5-3.8 s of
 * PIO under load [silicon rate, host-driver.md], runs only for the file,
 * in the background).  The developer row and the simulator's bcm_decode
 * set it; each panel logs its CPU or BCM ms (bench rows PANELAB,
 * RESPANEL) */
void art_bcm_panel_set(bool on);
bool art_bcm_panel_on(void);

/* A decode is in flight (the UI job clients take their CPU paths) */
bool art_bcm_busy(void);
/* The visualizer's overlay (visualizer.md 8.5): SPECTRUM and BARS on the
 * BCM.  The merged image takes them as an overlay in the region DECODE's
 * uses, so the two swap: the art thread loads DECODE's for each decode
 * (evicting the visualizer's) and, asked, the visualizer's between
 * decodes (evicting DECODE's); the file is read on the art thread, put
 * piece by piece by the job owner's pass, never read on the UI thread.
 * art_bcm_viz_ask(), the UI thread (the job owner), each frame the ops
 * are missing: ART_VIZ_LOADED (offered now), ART_VIZ_WAIT (a load is
 * owed or under way: the visualizer holds its frame, no CPU path) or
 * ART_VIZ_NONE (no overlay for this image, or refused this epoch: the
 * CPU's, logged).  art_bcm_viz_serve(): the art thread, from its pump
 * (artcache.c), the owed load */
enum
{
    ART_VIZ_LOADED = 0,
    ART_VIZ_WAIT,
    ART_VIZ_NONE,
};
int art_bcm_viz_ask(void);
void art_bcm_viz_serve(void);
/* 0.8.0, sitting 4c ("the visualiser is bursty"): while the visualizer's
 * view is open it owns the shared region and its frame cadence.  The art
 * side neither loads DECODE's overlay nor runs a DECODE then: the
 * background passes wait (artcache.c blocked(): "viz"), a decode marked
 * for the BCM (the box, np-sharp's panel, a resident's re-decode) is
 * deferred to its next chance, never handed to the PP, and one in flight
 * is cancelled (bcm_cancel()).  On close the work resumes; DECODE's
 * overlay loads when a decode next needs it.
 *  - art_bcm_viz_view(): the view's open and close (viz.c viz_start(),
 *    viz_stop(); the UI thread).  The close logs the deferrals once
 *    ("art chip work paused: visualizer open (n deferred)") and wakes the
 *    art thread;
 *  - art_bcm_viz_holds(): the view is open and the BCM would take the
 *    art's decodes (DECODE offered, or its overlay due): any thread;
 *  - art_bcm_viz_defer(): the art thread turned work away for it (a
 *    count; the first of a view traced "art bcm viz defer what=...") */
void art_bcm_viz_view(bool open);
bool art_bcm_viz_holds(void);
void art_bcm_viz_defer(const char *what);
/* The UI pass's time in BCM work (sitting 4c: the Now Playing counter
 * stalled while a cover went to the BCM): each piece of it notes its us
 * here, and the longest a minute is logged ("bcmjob pass max us=N
 * what=W"), for the next device log.  UI thread only */
void art_bcm_pass_note(const char *what, uint32_t us);
uint32_t art_bcm_pass_now(void);    /* the clock it takes: us */
/* DECODE is the merged image's overlay and borrows a J_CLS_DRIFT slot per
 * decode (bcmjob_dec_lends(), as last seen by art_bcm_service()) */
bool art_bcm_lends(void);
/* The UI thread's pass (ui_main.c): the overlay's load when asked or due
 * (through the strip: 'rest', no slide, glide or fade in flight, unless
 * Now Playing holds the strip), a released slot's FREE, on the job
 * owner's thread */
void art_bcm_service(bool rest);
#endif

#endif
