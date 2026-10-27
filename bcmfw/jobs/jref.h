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

#ifndef BCMFW_JREF_H
#define BCMFW_JREF_H

/* The display job interface's reference executor and record codec
 * (contract: bcmfw/jobs.h, docs/research/bcm2722/jobs.md).
 *
 * - jref_*: runs job records against surfaces in ordinary memory.  It
 *   defines what every job means; it is the PP's CPU fallback and the
 *   tests' oracle, and the BCM's implementation must match it bit for
 *   bit.  Plain C, no allocation, no hardware: it builds for the Mac,
 *   the PP and the BCM (vc2cc) alike.
 * - jring_*: writes and reads records in a ring of 32-bit words, exactly
 *   as the PP driver writes the BCM's command ring (hostproto.h). */

#include <stdint.h>
#include "jobs.h"

/* What a build compiles in.  The PP's CPU path needs neither:
 * - JREF_DECODE: J_OP_DECODE and its hook (the host model, the decode
 *   tests; HP_E_UNKNOWN without it);
 * - JREF_HOOKS (implies JREF_DECODE): the vector hooks too.  The BCM
 *   image defines it (bcmfw/build.py).
 * They change struct jref, so every file of one program that includes
 * this header must see the same ones: the entry point's name carries
 * them, so a mixed program fails to link instead of misreading it. */
#if defined(JREF_HOOKS) && !defined(JREF_DECODE)
#define JREF_DECODE 1
#endif
/* - JREF_VIZ (proposed, sim only): J_OP_SPECTRUM and J_OP_BARS (jviz.h),
 *   and the spectrum hook; the visualizer image and the host model
 *   define it. */
/* - JREF_EXT (the display-only image): ops this build doesn't define go
 *   to the ext hook (an overlay's; jobs_bcm.c), else HP_E_UNKNOWN. */
#if defined(JREF_EXT) && defined(JREF_VIZ)
#error "JREF_EXT builds take SPECTRUM and BARS from an overlay, not JREF_VIZ"
#endif
#if defined(JREF_HOOKS) && defined(JREF_VIZ)
#define jref_setup jref_setup_hooks_viz
#elif defined(JREF_HOOKS) && defined(JREF_EXT)
#define jref_setup jref_setup_hooks_ext
#elif defined(JREF_HOOKS)
#define jref_setup jref_setup_hooks
#elif defined(JREF_DECODE) && defined(JREF_VIZ)
#define jref_setup jref_setup_decode_viz
#elif defined(JREF_DECODE)
#define jref_setup jref_setup_decode
#elif defined(JREF_VIZ)
#define jref_setup jref_setup_viz
#endif
#ifdef JREF_DECODE
#include "jdec.h"
#endif
#include "jviz.h"         /* declarations only: jref_bars' hook type */

#define JREF_SLOTS      64
#define JREF_GROUP      0x10000     /* copy: one group of 16 rows */

struct jref_surf
{
    uint16_t *px;           /* RGB565 */
    uint32_t w, h;
    uint32_t pitch;         /* in pixels */
};

struct jref_slot
{
    uint32_t key_lo, key_hi;
    uint32_t bytes, epoch, sum;     /* sum: COMMIT's F16w B */
    uint16_t w, h;
    uint8_t fmt, cls, state, pad;
    struct jref_surf surf;
};

/* A pool: one class's memory, handed out first-fit-from-the-end
 * (rev 0.1: slots of a class are freed and refilled whole) */
struct jref_pool
{
    uint8_t *base;
    uint32_t size, used;
    uint32_t bus;           /* base as the BCM's map has it (ALLOC's addr) */
};

#define JREF_CLASSES    J_CLASSES

struct jref
{
    struct jref_surf front, back, panel;    /* panel: what PRESENT shows */
    struct jref_slot slot[JREF_SLOTS];
    struct jref_pool pool[JREF_CLASSES];    /* by J_CLS_* */
    uint32_t epoch, fence, mode;
    uint32_t done, errors, last_err;
    uint32_t presents;
    uint32_t anim_off;      /* the offset a SLIDE drew last (0 when an ANIM
                               is taken): a slide out of the frame it draws
                               on moves that frame on from there */
    /* NULL: PRESENT copies into 'panel' (the model).  The BCM image sets
     * this to push the rect to the real panel (lcd_blit) instead. */
    void (*present)(const struct jref_surf *s, uint32_t xy, uint32_t wh,
                    uint32_t flags);
#ifdef JREF_HOOKS
    /* The BCM image's vector paths (video/jobs_bcm.c); NULL (the host
     * model, the PP): every pixel in C.  Each does the columns [lo, hi) of
     * the rows it is given that suit it (alignment) and returns
     * lo | hi << 16, or 0 for none; jref does the other columns in C.
     * Pitches are in pixels.
     *
     * copy: BLIT, SCROLL and the animations.  rows rows of w pixels from
     * s to d, row by row in order (a negative pitch: upward), each row
     * read before it is written; or, rows JREF_GROUP | 16, one group of
     * 16 rows (d and s: its first row in that order), every row of it
     * read before any is written.  key: 0, or J_BLIT_KEY | colour << 16
     * (pixels of that colour not copied; groups only). */
    uint32_t (*copy)(uint16_t *d, int32_t dp, const uint16_t *s, int32_t sp,
                     uint32_t w, uint32_t rows, uint32_t key);
    /* fill: FILL, h rows of w pixels at d set to c */
    uint32_t (*fill)(uint16_t *d, uint32_t pitch, uint32_t w, uint32_t h,
                     uint32_t c);
    /* mix16: FADE and AVG, 16 rows of w pixels, d = jmix_fade_px(a, b',
     * weight), or jmix_avg_px(a, b) with weight 0; every row read before
     * it is written.  b' is b with its first ncols columns shaded by the
     * alpha bytes at alpha (jmix_shade_px: FADE's J_FADE_SHADE_B), b
     * elsewhere.  alpha is given on a job's first group only, NULL on the
     * later ones: a hook may keep what it built from it until then. */
    uint32_t (*mix16)(uint16_t *d, uint32_t dp, const uint16_t *a,
                      uint32_t ap, const uint16_t *b, uint32_t bp,
                      uint32_t w, uint32_t weight, uint32_t ncols,
                      const uint32_t *alpha);
    /* shade16: SHADE, 16 rows of w pixels in place, column c darkened by
     * alpha byte c of the words at alpha (jmix_shade_px); alpha NULL on a
     * job's later groups, as mix16's.  It may rewrite pixels past w in the
     * same rows with their own values. */
    uint32_t (*shade16)(uint16_t *p, uint32_t pitch, uint32_t w,
                        const uint32_t *alpha);
#endif
#ifdef JREF_DECODE
    /* NULL: DECODE is not offered (HP_E_UNKNOWN).  Else jdec_run() with
     * the caller's work area (jdec.h): JDEC_OK or a JDEC_E_* code. */
    int (*decode)(const uint8_t *jpg, uint32_t len,
                  const struct jdec_out *out, int n, struct jdec_info *info);
#endif
#ifdef JREF_VIZ
    /* NULL: SPECTRUM's FFT in C (jviz_power_ref).  Else the BCM's vector
     * kernel: the powers of n samples (split order) into p, returning 1,
     * or 0 to leave them to C (the slot not aligned for it) */
    int (*spectrum)(uint32_t *p, const int16_t *pcm, uint32_t n);
    /* NULL: BARS in C (jviz_bars_draw).  Else the BCM's vector bars
     * (jviz.h jviz_bars_fn), the C taking any frame it returns 0 for */
    jviz_bars_fn bars;
#endif
};

/* Set up: two frames, the panel model (copied), and a pool per class
 * (size 0: the class is not offered).  The epoch starts at 1.
 * jref_setup() takes them by pointer: three structs by value cost the BCM
 * image about 60 bytes (jobs_bcm.c calls it).  jref_init() takes them by
 * value, as every other caller does (the PP's bcmjob-cpu.c, the host
 * tests, the model): an inline wrapper, so it costs an image nothing
 * unless called */
void jref_setup(struct jref *j, const struct jref_surf *front,
                const struct jref_surf *back, const struct jref_surf *panel);
static inline void jref_init(struct jref *j, struct jref_surf front,
                             struct jref_surf back, struct jref_surf panel)
{
    jref_setup(j, &front, &back, &panel);
}
void jref_pool(struct jref *j, uint32_t cls, void *mem, uint32_t bytes);
/* The pool's base in the BCM's map (default 0): ALLOC's reply word addr
 * is it plus the slot's offset in the pool (jobs.h J_OP_ALLOC) */
void jref_pool_addr(struct jref *j, uint32_t cls, uint32_t bus_base);

/* Run one record (header word and its payload).  Returns its status
 * (HP_OK or an error), and counts it in done (and in errors if it
 * failed: jobs.h J_DONE); *nreply words go to reply (status first) when the
 * record asks for a reply, fails, or is a FENCE; 0 otherwise.  reply must
 * hold HP_MAX_PAYLOAD + 1 words. */
uint32_t jref_exec(struct jref *j, const uint32_t *rec, uint32_t *reply,
                   uint32_t *nreply);

/* A slot's pixels, for the PP's write after ALLOC (NULL if not
 * ALLOCATED or READY) */
uint16_t *jref_slot_px(struct jref *j, uint32_t slot);

/* An animation's frame at t_ms into J_SURF_FRONT (the BCM runs the same
 * function each frame, then pushes its rect).  Returns the microseconds
 * to its next frame while it runs (J_ANIM_FRAME_US, or a stepped PAN's
 * step: jobs.h J_PAN_STEP), 0 when this frame was its last.  Every part
 * is a BLIT (rev 0.4).  A SLIDE from J_SURF_FRONT slides the frame as it
 * stood when the ANIM was taken: each frame moves it on in place from the
 * offset drawn last (anim_off), then draws the new picture's part, so
 * frames may be skipped (the offsets never decrease) but each equals the
 * slide from that first frame */
uint32_t jref_anim_frame(struct jref *j, const uint32_t *rec, uint32_t t_ms);

/* COMMIT's F16w (jobs.h j_f16w) of a surface's w x h pixels, row by row,
 * its padding left out */
void jref_f16w(const struct jref_surf *s, uint32_t *a, uint32_t *b);

/* JREF_EXT: every op in 0x20-0x3F this build doesn't define goes to
 * jref_ext(), which the program defines (the display-only image:
 * jobs_bcm.c, for its overlay).  It runs it as jref_exec's own,
 * checking its payload's length itself, and returns its status
 * (HP_E_UNKNOWN for none) and *nr reply words in rep (status first; *nr
 * is 1 on entry); jref_exec counts it */
uint32_t jref_ext(struct jref *j, uint32_t op, const uint32_t *a, uint32_t n,
                  uint32_t *rep, uint32_t *nr);

/* What every job checks its surfaces with: J_SURF_*, or a slot of this
 * epoch in one of the states allowed (bit per J_ST_*) */
#define JREF_READ_OK    (1u << J_ST_READY)
#define JREF_WRITE_OK   (1u << J_ST_ALLOCATED | 1u << J_ST_READY)

/* The visualizer's jobs (proposed, sim only; jref_viz.c, jviz.h): what
 * jref_exec runs for J_OP_SPECTRUM (hook: jref's spectrum hook, NULL for
 * the C FFT) and J_OP_BARS (hook: jref's bars hook, NULL for the C)
 * with JREF_VIZ, and what the display-only
 * image's overlay runs (video/viz_ovl.c).  a: the payload, n its words
 * (at least J_SPEC_ARGS(1) and J_BARS_ARGS(1)) */
uint32_t jref_spectrum(struct jref *j, const uint32_t *a, uint32_t n, uint32_t *rep,
                       uint32_t *nr, int (*hook)(uint32_t *p, const int16_t *pcm,
                                                 uint32_t n));
uint32_t jref_bars(struct jref *j, const uint32_t *a, uint32_t n, jviz_bars_fn hook);
/* POLYLINE, SEGMENTS and PALROW (jobs.h; jviz.h jviz_poly_draw,
 * jviz_segs_draw, jviz_pal_draw: plain C on every runner, no hooks); n at
 * least J_POLY_ARGS, J_SEGS_ARGS(1), J_PAL_ARGS(1) */
uint32_t jref_polyline(struct jref *j, const uint32_t *a, uint32_t n);
uint32_t jref_segments(struct jref *j, const uint32_t *a, uint32_t n);
uint32_t jref_palrow(struct jref *j, const uint32_t *a, uint32_t n);

/* An overlay's header (jobs.h J_OP_OVERLAY), at its base: words 0-4 are
 * checked before anything in it runs.  init: once a load is checked;
 * returns the ops it offers (J_OPS bits).  exec: as jref's ext hook,
 * for those ops only */
struct jref_ovl
{
    uint32_t magic;             /* J_OVL_MAGIC */
    uint32_t abi;               /* vc2cc's __vc2_abi (J_OVL_ABI) */
    uint32_t base;              /* where it is linked: the region's base */
    uint32_t load_end;          /* base + the bytes the PP writes */
    uint32_t bss_end;           /* the end of its .bss, in the region */
    uint32_t (*init)(struct jref *j);
    uint32_t (*exec)(struct jref *j, uint32_t op, const uint32_t *a, uint32_t n,
                     uint32_t *rep, uint32_t *nr);
};

/* ---- The ring ------------------------------------------------------- */
struct jring
{
    uint32_t *w;            /* the ring's words */
    uint32_t size;          /* a power of two */
    uint32_t wr, rd;        /* word indices, wrapping at 2^16 as the
                               shared HP_ENC indices do */
};

/* Append one record; 0, or HP_E_BUSY if it does not fit, HP_E_TOOBIG */
uint32_t jring_put(struct jring *r, uint32_t op, uint32_t tag,
                   const uint32_t *payload, uint32_t n);
/* Take the next record into rec (header and payload): its word count, or
 * 0 if the ring is empty */
uint32_t jring_get(struct jring *r, uint32_t *rec);

#endif /* BCMFW_JREF_H */
