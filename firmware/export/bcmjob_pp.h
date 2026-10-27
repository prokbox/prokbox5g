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

#ifndef BCMJOB_PP_H
#define BCMJOB_PP_H

/* The PP side of the BCM2722 display job interface beyond the contract
 * (bcmfw/jobs/bcmjob.h): set-up and the CPU path.  The device driver
 * (HAVE_BCM_JOBS, firmware/target/arm/ipod/video/bcmjob-dev.c) and the
 * simulator's host model (HAVE_BCM_JOBS_MODEL, uisimulator/common/
 * sim_bcm_job.c) both implement it; this header needs no bcmfw/ include
 * path, so the shell can include it anywhere. */

#include <stdbool.h>
#include <stdint.h>

#if defined(HAVE_BCM_JOBS) || defined(HAVE_BCM_JOBS_MODEL)
/* Shell start, once, UI thread: the job client's state (the reference
 * executor's ~3 KB, a core_alloc block, never a static) and, in the
 * simulator, the resident image's reservation (BCM_RESIDENT_BYTES).
 * trace: where the driver's lines go ("bcmjob ...", "bcm power ...");
 * NULL: nowhere.  Nothing is sent to the BCM. */
void bcmjob_init(void (*trace)(const char *line));
/* The calling thread becomes the job ring's owner (19-9: the UI thread
 * moves off main); see bcmjob-dev.c */
void bcmjob_claim_owner(void);

/* ---- Cores, threads and memory (P19 S0; design sections 2.4, 6.1) -------
 * - bcmjob_init() runs on the CPU (it allocates and pins); its thread
 *   becomes the ring owner, the only writer of the driver's state.
 * - Every other thread's suspend or resume is a request the owner applies
 *   (bcmjob-core.h, bjc_suspend_request()); the owner learns of it by
 *   SYS_BCMJOB_REQ on button_queue, answered with bcmjob_apply().
 * - Upload pixels must not move during an upload: the engine-2 wait
 *   yields, and a core_alloc compaction can run meanwhile.  bcmjob_upload()
 *   takes static, IRAM or already-pinned memory only; a movable block goes
 *   by bcmjob_upload_handle(), which pins it for the whole upload (the
 *   copy and the checksum after it).  The same holds for bcmjob_cpu_slot()'s
 *   px, for as long as the slot is named. */
/* MISC 0x20 NP_PB_WAKE, 0x21 UI_LIST_SIM_WAKE (apps/ipodui) */
#define SYS_BCMJOB_REQ MAKE_SYS_EVENT(SYS_EVENT_CLS_MISC, 0x22)
/* The owner's budget (P19 19-6): fn() true while a scroll or animation
 * frame is due, and the owner's pass then writes no DECODE ring piece and
 * reads no slot rows (otherwise at most one 8 KB piece, or about 2 ms of
 * rows, a pass).  The shell sets it once at start (ui_main.c); NULL, the
 * default: never due.  The owner's thread only. */
void bcmjob_frame_due_hook(bool (*fn)(void));
/* USB never waits on a decode (owner, 2026-10-24): on SYS_USB_CONNECTED
 * and SYS_POWEROFF (misc.c's default event handler, on the owner's
 * thread) the decode or read the art thread posted ends at once, ABORTED
 * (bcmjob_dec_verdict(): DEFER), and the art thread is free to ack */
void bcmjob_decode_stop(void);
#ifdef HAVE_BCM_JOBS
/* The owner's input pump, on SYS_BCMJOB_REQ: apply pending requests and
 * run one pass of a decode or read posted by another thread (the art
 * thread's bcmjob_decode(), bcmjob_read()) */
void bcmjob_apply(void);
#ifdef HAVE_BCMFW_DEVTEST
/* Developer (the bench drill "forced job failure"): the next n jobs of op
 * reply as failed, through the driver's real reply path (bcmjob-core.h
 * bjc_inject_fail); n = 0 disarms and forgives the op (its failed mark).  The owner's thread only: 0, or -1
 * (no driver, or another thread) */
int bcmjob_dev_inject_fail(uint32_t op, uint32_t n);
#define HAVE_BCMJOB_INJECT
#endif
#endif
struct bcmjob_slot;
/* bcmjob_upload() of the core_alloc block h (block_bytes long) from
 * offset on, pinned for the whole upload (CPU only: the pin is buflib's).
 * px_bytes (the slot's w * h * 2) must fit in the block after offset,
 * offset even; otherwise BCMJOB_FAILED and nothing is touched. */
int bcmjob_upload_handle(const struct bcmjob_slot *s, int h,
                         uint32_t block_bytes, uint32_t offset,
                         uint32_t px_bytes);

/* ---- Screen truth (phase19-brief.md 3.6; 19-2) ---------------------------
 * The one rect whose pixels live in the BCM's front frame, not the PP
 * framebuffer, while a BCM animation owns it (today: the drift's panel).
 * The owner sets it after a resync of the rect and clears it when it
 * settles (the CPU's own framing written back to the framebuffer).  On
 * the device every push (lcd_update_rect) clips against it; the
 * simulator's shots take it from the model's panel.  w 0: none.  One
 * writer, the UI thread (bcmjob-cpu.c). */
void bcmjob_owned_set(int x, int y, int w, int h);
/* false: nothing owned, or owned in an earlier power-up of the BCM (its
 * front frame is gone: the restart showed the framebuffer) */
bool bcmjob_owned_get(int *x, int *y, int *w, int *h);
/* J_VERSION's minor of the image taking jobs now (bcmfw/jobs.h's
 * revisions; 0 while none is): what it understands, e.g. the stepped PAN
 * from 3 (19-5).  The job owner's thread, as bcmjob_have() */
uint32_t bcmjob_minor(void);
/* BARS is offered and honours J_BARS_FLIP (jobs.h J_VPU_FLIP: the
 * visualizer's Mirror drawn on the BCM, its reflection upside down by
 * addressing).  false: an image or overlay before the flag, which would
 * draw it upright; the caller then draws the Mirror on the CPU.  The job
 * owner's thread, as bcmjob_have() */
bool bcmjob_bars_flip(void);
/* The overlays (#126; album-render.md section 5.3; visualizer.md section
 * 8.5): an image that offers OVERLAY has one region, and a file from the
 * system folder adds ops there, one file at a time.  The kinds:
 *  - BCMJOB_OVL_VIZ: the visualizer's SPECTRUM and BARS (and POLYLINE,
 *    SEGMENTS, PALROW where their start-up checks pass): viz-ovl-m.bin
 *    with the merged video image (the caller's record: mkvideo.py's
 *    apps/video/bcmvideo_image.h BCMVIDEO_VIZOVL_*); in a developer build
 *    running the display-only image, viz-ovl.bin with the record that
 *    build embeds (bcmjob_ovl_due() puts it in *f);
 *  - BCMJOB_OVL_DEC: DECODE: dec-ovl.bin with the merged image
 *    (BCMVIDEO_DECOVL_*); bcmjob_dec_lends(): the loaded overlay borrows
 *    its work area, a J_CLS_DRIFT slot bcmjob_decode() lends per job
 *    (J_CAP_DEC_LENT): the drift's own cover leaves the pool first.
 * The merged image's two kinds share its region: a load evicts the other
 * kind (its ops go; its load is due again at once).  The shell decides
 * who loads when (art_bcm.c: the art thread's decode, and the
 * visualizer's ask).
 *  - bcmjob_ovl_due(): the job owner's thread: a load of kind is wanted
 *    now (the running image's region is the record's, the kind's ops
 *    missing, a try left this epoch); no port access but discovery's;
 *  - bcmjob_ovl_read(): any thread but the owner's UI thread (the freeze
 *    audit, 2026-10-26: no file read there; the art thread): bytes [off,
 *    off + n) of the kind's file (f: the record due gave) into buf (word
 *    aligned; n a multiple of 4), the file's size in *size; no port
 *    access.  The bytes read (the file's tail may be shorter), or -1: no
 *    file or a short read, logged once a boot ("bcmjob overlay refused
 *    why=nofile|read");
 *  - bcmjob_ovl_put(): the job owner's thread: that piece to the BCM, in
 *    order from off 0 (which starts the load: the header's checks) to the
 *    file's end (OVERLAY, its start-up checks, J_OPS read back); no file
 *    access, every wait bounded (J_T_OVERLAY).  BCMJOB_OK: the piece is
 *    in (after the last: the kind's ops offered, or offered already);
 *    BCMJOB_NOJOB: no load due, or the last piece added no ops;
 *    BCMJOB_BUSY: a DECODE holds the BCM (try again after it);
 *    BCMJOB_OVL_AGAIN: the piece's wait on the BCM is posted, not waited
 *    for (the first piece's FENCE, the last piece's OVERLAY reply and its
 *    checks, about 30 ms): put the same piece again on a later pass (0.8.0,
 *    sitting 4c: no UI pass blocks on an overlay load); any other code
 *    ends the load (logged by the driver);
 *  - bcmjob_ovl_poll(): the job owner's thread: a last piece's answer
 *    still owed (BCMJOB_OVL_AGAIN before) looked for without its buffer,
 *    for a caller that withdrew the piece: AGAIN, the answer put() would
 *    have given, or BCMJOB_NOJOB (none owed).  A first piece's FENCE
 *    owed ends there (BCMJOB_OFF: the next load begins afresh).
 * Lines: "bcmjob overlay load|ok|evict|refused why=nofile|read|file|
 * region|image|sum|version|wire".  The simulator's model holds the same
 * gates (sim_bcm_job.c: "bcm_decode ovl", "bcm_viz ovl"). */
enum
{
    BCMJOB_OVL_VIZ = 0,         /* = bcmjob-core.h BJC_OVL_VIZ */
    BCMJOB_OVL_DEC,             /* = BJC_OVL_DEC */
};
struct bcmjob_ovl_file
{
    uint32_t bytes, fnv, abi, abi_at, base;
};
bool bcmjob_ovl_due(int kind, struct bcmjob_ovl_file *f);
int bcmjob_ovl_read(int kind, void *buf, uint32_t off, uint32_t n, uint32_t *size,
                    const struct bcmjob_ovl_file *f);
int bcmjob_ovl_put(int kind, const void *buf, uint32_t off, uint32_t n,
                   uint32_t size, const struct bcmjob_ovl_file *f);
#define BCMJOB_OVL_AGAIN 1      /* = bcmjob-core.h BJC_OVL_AGAIN */
int bcmjob_ovl_poll(void);
bool bcmjob_dec_lends(void);

/* The BCM's power-up generation (device: the job driver's jd_gen(), 0
 * while it can't take jobs; simulator: the model's cold starts) */
uint32_t bcmjob_power_gen(void);
/* The owner's settle (the shell's drift_settle()), registered once: a
 * full-screen lcd_update() runs it first while a rect is owned, so a
 * takeover the shell doesn't know of (a firmware screen) never pushes a
 * stale panel.  Only on the registering thread (it touches the UI's
 * state); on any other thread the push is clipped as before. */
void bcmjob_owned_hook(void (*settle)(void));
void bcmjob_owned_settle(void);     /* the LCD drivers' call */

#if defined(HAVE_BCM_JOBS_MODEL) && defined(HAVE_IPOD_UI_DEVMODE)
/* Developer (the sim's twin of the device's hook above): the next n jobs
 * of op fail in the model, then it runs again; n = 0 disarms and
 * forgives the op (its failed mark).  Compiled
 * out of a release build.  0, or -1 (the model is not running) */
int bcmjob_dev_inject_fail(uint32_t op, uint32_t n);
#define HAVE_BCMJOB_INJECT
#endif

#ifdef HAVE_BCM_JOBS_MODEL
/* The simulator's screen-truth poison: the framebuffer's copy of a
 * BCM-owned rect, a checkerboard no picture has */
#define BCMJOB_POISON(x, y) ((((x) + (y)) & 1) ? 0x07E0 : 0xF81F)
/* The simulator's B3 parity: pixels in the rect where the model's panel
 * (what PRESENT showed) differs from the PP framebuffer */
uint32_t bcmjob_model_diff(int x, int y, int w, int h);
#endif

/* ---- The drift's backend: the developer A/B switch (B3) -------------------
 * AUTO: jobs when the driver allows them this boot (the image's FNV-1a in
 * bcmfw/verified.txt or the developer marker; in the simulator: never
 * unless a script asks), else today's CPU drift.  ON: jobs (a missing op
 * gives the still cover).  OFF: the CPU drift.  STILL: D-1's still cover,
 * to see the fallback on the bench.  Per boot, not saved. */
enum bcmjob_drift_mode
{
    BCMJOB_DRIFT_AUTO = 0,
    BCMJOB_DRIFT_ON,
    BCMJOB_DRIFT_OFF,
    BCMJOB_DRIFT_STILL,
    BCMJOB_DRIFT_DEV_MODES,     /* the developer row cycles these */
    /* the simulator's B3 parity: jobs, and the CPU path draws each frame
     * too and is compared (ON is the device's behaviour: jobs only) */
    BCMJOB_DRIFT_PARITY = BCMJOB_DRIFT_DEV_MODES,
    BCMJOB_DRIFT_MODES
};
void bcmjob_drift_mode_set(enum bcmjob_drift_mode m);
enum bcmjob_drift_mode bcmjob_drift_mode(void);

/* ---- The CPU path (bcmjob-cpu.c, both builds) --------------------------
 * Every job's fallback is the reference executor (bcmfw/jobs/jref.c) run
 * on PP memory: J_SURF_FRONT is the PP framebuffer, PRESENT pushes the
 * rect through the LCD contract (lcd_update_rect).  A slot on the CPU
 * path is PP memory the caller owns (its pv.slot, a strip ...). */

/* bcmjob_init()'s part: the executor's block (false: no memory) */
bool bcmjob_cpu_init(void);

/* Name PP memory as slot id (< 64) for the CPU path's jobs; px NULL
 * forgets it.  0, or -1 (no state: bcmjob_init failed, or a bad id). */
int bcmjob_cpu_slot(uint32_t slot, uint16_t *px, uint32_t w, uint32_t h,
                    uint32_t pitch);

/* Run one job on the CPU path now: the job's status (HP_OK = 0, or an
 * HP_E_* / J_E_* code), -1 without state */
int bcmjob_cpu_run(uint32_t op, uint32_t tag, const uint32_t *payload,
                   uint32_t n);

/* The developer self-test: a FILL into J_SURF_BACK and a FENCE through
 * the job ring, checked against the CPU path; 0 passed, 1 jobs not
 * available (the reason in the trace), < 0 failed.  Logged either way. */
int bcmjob_selftest(void);
/* The device: why jobs can't run now (the self-test's 1), in words for
 * the developer screens; NULL: they can.  *restart: a restart may fix it
 * (no display driver or job driver in this start, a stopped chip).  The
 * job owner's thread, as bcmjob_have(); not in the simulator. */
const char *bcmjob_off_why(bool *restart);
/* The device: how often the job driver found the chip failed this boot
 * (its heartbeat stopped, or the image's fault word): the first-use
 * video check's token goes when it moves (apps/video/vcheck.h) */
uint32_t bcmjob_chip_failures(void);
#endif

/* ---- Scope, Radial, Spectrogram on the BCM (#126; proposed, sim only) ---
 * (outside the build guard: the structs are plain, and tests/bcmjob
 * builds bcmjob-viz.c without HAVE_BCM_JOBS)
 * POLYLINE, SEGMENTS and PALROW (bcmfw/jobs.h, the overlay's J_OPS bits
 * 22-24): one record each, no reply, queued as bcmjob_submit() (OK, BUSY,
 * OFF, NOJOB).  The pixels are bcmfw/jobs/jviz.h's jviz_poly_draw,
 * jviz_segs_draw and jviz_pal_draw, which the CPU fallback calls with the
 * same values.  bcmjob_have_*(): the op is offered now (on the device the
 * overlay's start-up check took it, once its load: BCMJOB_OVL_VIZ
 * above).  The job owner's thread, as bcmjob_have(). */
struct bcmjob_poly
{
    uint32_t surf;              /* J_SURF_FRONT, ... */
    uint16_t x, y, w, h;        /* the rect, w <= 320 */
    uint32_t src;               /* the READY slot holding the points */
    uint16_t off, step;         /* point i: element off + i step */
    uint16_t n;                 /* 2 .. 1024 points across the rect */
    uint16_t flags;             /* J_POLY_U8: bytes, each a row; else int16 */
    uint16_t gain;              /* int16: row h/2 - (v gain >> 16) */
    uint16_t half;              /* split-order window (SPECTRUM's slot): its
                                   n / 2; 0: plain order */
    uint16_t colour;            /* RGB565 */
    uint16_t width;             /* 1-3 */
};
struct bcmjob_segs
{
    uint32_t surf;
    uint16_t x, y, w, h;
    int16_t cx, cy;             /* the centre, in the rect */
    uint8_t n;                  /* 1 .. 48 segments */
    uint8_t a0, astep;          /* segment i at angle index a0 + i astep
                                   of 48 (0 up, clockwise) */
    uint8_t width;              /* 1-3 */
    uint8_t flags;              /* J_SEG_CAP | J_SEG_RING | J_SEG_ROUND */
    uint8_t r0, lmax, rring;    /* base radius, longest segment (and the
                                   gradient's span), the dot ring's radius */
    uint8_t capd, ringd;        /* dot sizes, 0-15 */
    uint16_t c0, c1;            /* the gradient, base to tip */
    uint16_t cap, ring;         /* the dots' colours */
    const uint8_t *len, *peak;  /* n each, in pixels */
};
struct bcmjob_palrow
{
    uint32_t surf;
    uint16_t x, y, w, h;        /* the rows drawn (after a SCROLL) */
    uint32_t lut;               /* a READY slot of 256 RGB565 entries */
    uint8_t nb;                 /* 1 .. 96 cells, nb <= w */
    const uint8_t *lv;          /* nb levels, each a LUT index */
};
bool bcmjob_have_polyline(void);
bool bcmjob_have_segments(void);
bool bcmjob_have_palrow(void);
int bcmjob_viz_polyline(const struct bcmjob_poly *p);
int bcmjob_viz_segments(const struct bcmjob_segs *s);
int bcmjob_viz_palrow(const struct bcmjob_palrow *p);

/* P19 19-6 review (lost kicks): every loop on the owner's thread that
 * takes events from button_queue passes each one here, so a decode posted
 * by the art thread keeps its passes inside modal loops (the USB screen,
 * the developer loops) as in the main loop: true if it was the job
 * owner's kick (applied: port work only, never the disk, so it is safe at
 * the USB screen's entry).  default_event_handler() does it too.  A kick
 * dropped anyway (a cleared queue) is posted again within
 * BJC_KICK_STALE_MS; the caller also has its own bound. */
#ifdef HAVE_BCM_JOBS
#include "button.h"
static inline bool bcmjob_owner_event(long btn)
{
    if (btn != SYS_BCMJOB_REQ)
        return false;
    bcmjob_apply();
    return true;
}
#else
static inline bool bcmjob_owner_event(long btn)
{
    (void)btn;
    return false;
}
#endif

#endif /* BCMJOB_PP_H */
