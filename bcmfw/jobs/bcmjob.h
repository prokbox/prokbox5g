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

#ifndef BCMFW_BCMJOB_H
#define BCMFW_BCMJOB_H

/* The PP-side API the shell uses for display jobs (P19 contract 4,
 * proposed; docs/research/bcm2722/jobs.md section 10).
 *
 * One API, two implementations:
 *   - the device driver (P19 19-L2: firmware/target/arm/ipod/video/),
 *     over the BCM's command ring, engine 2 and the port lock;
 *   - the host model (bcmfw/jobs/jmodel.c), over the reference executor,
 *     with modelled time, faults and power, which the simulator wraps
 *     (uisimulator/common/sim_bcm_job.c) and the host tests drive.
 * Both run every job through the same definition (jref.c, jmix.h), so a
 * fallback frame equals a chip frame. */

#include <stdbool.h>
#include <stdint.h>
#include "jobs.h"
#include "jdec.h"       /* bcmjob_decode(): the CPU twin's source and codes */

/* Results (negative) */
#define BCMJOB_OK         0
#define BCMJOB_BUSY      -1     /* the ring is full: returned at once (gap
                                   G1); the caller takes the CPU path for
                                   this frame, or drops it; never queues */
#define BCMJOB_OFF       -2     /* the BCM is not up (C0 or a standby),
                                   still starting (a cold start, until
                                   its HELLO and the job block are read:
                                   jobs.md section 2), or jobs are off
                                   this boot: CPU path.  Nothing is
                                   queued to run after the start */
#define BCMJOB_NOJOB     -3     /* this image lacks the job (J_OPS) */
#define BCMJOB_TIMEOUT   -4     /* a deadline passed: the job is marked
                                   slow; its CPU path from now on */
#define BCMJOB_FAILED    -5     /* the job failed: .err has the code */
#define BCMJOB_REFUSED   -6     /* a power request refused (on battery,
                                   dark, a decode) */
#define BCMJOB_ABORTED   -7     /* bcmjob_decode(): its cancel hook said
                                   stop (a newer track, the user back) */

/* ---- Jobs ------------------------------------------------------------- */
/* Whether op may be sent now: the image has it, it is not marked slow or
 * failed this boot, and the BCM is up (started).  The shell checks this
 * per frame and takes its CPU path otherwise.
 *
 * Marked slow: a deadline passed with the heartbeat live (jobs.md
 * section 6).  Marked failed: BCMJOB_FAIL_MARK failures of that op in one
 * boot, across restarts (the first is logged and the op tried again: the
 * slow rule's "retry once").  A failure is a job's reply with a status
 * other than HP_OK, except the caller's normal outcomes, which never
 * count: J_E_NOSLOT (free a slot), J_E_SLOT (a stale slot), J_E_SUM (a
 * torn upload: upload again), J_E_MODE (video or a game holds the pool)
 * and J_E_DEC_* (that source; the PP's twin decides).  Each failure
 * traces "bcmjob fault op=0xNN st=N why=failed" (the driver, with the
 * status word; a COMMIT's adds "slot=N WxH"; jmodel.c's leaves st= out). */
#define BCMJOB_FAIL_MARK  2
bool bcmjob_have(uint32_t op);

/* Queue one job; never blocks.  BCMJOB_OK, BUSY, OFF or NOJOB. */
int bcmjob_submit(uint32_t op, uint32_t tag, const uint32_t *payload, uint32_t n);

/* Queue a FENCE; its sequence in *seq */
int bcmjob_fence(uint32_t *seq);

/* Wait until fence seq retires, at most ms: OK, TIMEOUT or OFF (OFF also
 * when the BCM was cut or restarted meanwhile: the fence is gone).  A
 * failed job still retires its fence; bcmjob_errors() tells. */
int bcmjob_wait(uint32_t seq, uint32_t ms);

/* The job block's J_ERRORS (jobs.h 0x24): the jobs failed since the BCM's
 * start, out of J_DONE.  Monotonic while the BCM runs; a restart (a cold
 * start, or a developer stage's restore) starts it again from 0, and
 * always brings a new epoch.  While the BCM is cut, the last value read.
 * To see whether a frame's jobs failed, save bcmjob_epoch() and this
 * before submitting; after its fence, the same epoch and a larger count
 * (unsigned difference) means that many failed.  A different epoch means
 * the count may have restarted: take the new value as the new baseline
 * (the frame is lost with the epoch anyway: resync).  Which job failed is
 * in its reply and J_LAST_ERR; the driver marks ops (bcmjob_have). */
uint32_t bcmjob_errors(void);

/* ---- Slots ------------------------------------------------------------ */
struct bcmjob_slot
{
    uint32_t slot;          /* the id jobs name it by */
    bool hit;               /* it already holds the key: no upload */
    uint32_t epoch;         /* the epoch it belongs to */
};

/* ALLOC (and wait): a slot of class cls for a w x h RGB565 picture with
 * this content key.  OK, OFF, NOJOB, FAILED (.err: J_E_NOSLOT ...). */
int bcmjob_slot(uint32_t cls, uint32_t w, uint32_t h, uint32_t key_lo,
                uint32_t key_hi, struct bcmjob_slot *out);

/* The pixels into the slot (engine 2 for 16 rows or more, else the CPU),
 * under the port lock, then COMMIT with their F16w (jobs.h j_f16w).
 * Skipped on a hit. */
int bcmjob_upload(const struct bcmjob_slot *s, const uint16_t *px);

/* The PP's pixels into the second frame (rev 0.4, the retained screen:
 * jobs.md "Shell recipe"): the rect x, y, w, h of J_SURF_BACK (surf; no
 * other surface) from px, pitch pixels a row, at BACK's fixed address
 * (memory-contract.md section 2: 0x105800, 320 pixels a row), by engine
 * 2 for 16 rows or more, else the CPU, under the port lock.  Not a job:
 * the bytes land at once, outside ring order, so no queued job may still
 * read that rect (wait for the fence of the frame that last used it).
 * OK; OFF (as bcmjob_submit); FAILED (another surface, a rect outside
 * the frame, or pitch < w); BUSY: the call's time budget (about 4 ms:
 * BJC_RECT_US in the driver, JM_RECT_MS in the model) ran out with rows
 * left, or a DECODE is in flight (its ring owns the port's time).  On
 * BUSY the rows written are undefined (BACK only) and the caller draws
 * that frame by its CPU path; BUSY is never counted (no fault mark).  A
 * larger rect goes up in bands that each fit the budget (about 44 KB at
 * engine 2's 11 MB/s: 68 full rows).  A slide's new page (the PP
 * framebuffer's rect after the new screen drew), a glide's newly shown
 * rows; the jobs then read it from J_SURF_BACK in ring order. */
int bcmjob_upload_rect(uint32_t surf, uint32_t x, uint32_t y, uint32_t w,
                       uint32_t h, const uint16_t *px, uint32_t pitch);

/* The frame's CPU path (review M3): the PP framebuffer's rect to
 * J_SURF_FRONT and the panel through the LCD contract (today's push).
 * After a BUSY, OFF or TIMEOUT inside a frame's jobs, or an epoch change
 * during a SCROLL or an ANIM, the shell calls this for the whole frame:
 * the BCM's front frame and the panel then equal the PP's again.  A
 * slide cut short jumps to the PP's final frame. */
int bcmjob_resync(uint32_t x, uint32_t y, uint32_t w, uint32_t h);

/* Developer stages own the BCM (review M4): bcm-devtest's take()
 * suspends the job client (every call answers OFF, nothing is queued),
 * its restore() resumes it; the driver then re-runs HELLO, reads the job
 * block again and reports the new epoch (on_epoch) */
void bcmjob_suspend(bool on);

/* The current epoch (slots of another epoch are gone) */
uint32_t bcmjob_epoch(void);

/* Called (thread context, not an interrupt) whenever the driver cuts or
 * restarts the BCM, with the new epoch (gap G13): the shell forgets its
 * slots and re-uploads lazily.  One listener; NULL removes it. */
void bcmjob_on_epoch(void (*fn)(uint32_t epoch));

/* ---- Shell recipe: the marquee (19-5, rev 0.3) -------------------------
 * The whole text: jobs.md section 5, "Shell recipe: the marquee".
 * F2 (list.c, nowplaying.c): 1 s at rest, then 2 px every 50 ms; at the
 * lap L (label + 40 px gap) back to rest for 1 s.  On the BCM:
 *  1. At F2's first step, if bcmjob_have(J_OP_ANIM) and (J_OP_ANIM_STOP),
 *     J_VERSION's minor >= 3, and the scrollbar is clear of the cell:
 *  2. bcmjob_slot(J_CLS_STRIP, W, ch, key) and bcmjob_upload(): columns
 *     [0, tw) the cell at rest (from the PP framebuffer), then F2's
 *     strip rows [ty, y1) (bar, label at 0 and at L); W = tw + L + tw
 *     rounded up to 16 (the vector unit).  A hit uploads nothing.
 *  3. bcmjob_submit(J_OP_ANIM, tag, { J_ANIM_PAN, slot, J_XY(tw, 0),
 *     J_XY(tw + L, 0), J_XY(tw, ch), cell xy, 50, J_PAN_STEP | 1,
 *     2 | 1000 << 16, J_XY(0, 0), 1000 }, J_PAN_ARGS): the BCM draws
 *     F2's frames from step 1 on; the shell has no marquee wakeups.
 *     Now Playing: no rest cell, x0 = 0 and the rest (0, 0).
 *  4. While it runs the cell is the BCM's: nothing is pushed over it.
 *  5. To stop (selection, glide, slide, a band redraw, backlight off,
 *     sleep): ANIM_STOP, bcmjob_fence(), bcmjob_wait(seq,
 *     J_T_FENCE_BASE + J_T_SMALL), then the CPU's drawing (the row at
 *     rest).  To keep it: push first, then the ANIM again with start =
 *     the phase.
 *  6. OFF, NOJOB, FAILED, an error after its fence, or an epoch change:
 *     F2 from the phase (start + now - sent, mod the lap).  BUSY: F2's
 *     step now, the ANIM again at the next.  J_E_NOSLOT: free the old
 *     marquee slot, once.  TIMEOUT: bcmjob_resync() of the row band. */

/* ---- DECODE: a JPEG to up to J_DEC_MAX slots (P19 19-6) ---------------- */
/* The shell's call for J_OP_DECODE (jobs.h; album-render.md sections 4
 * and 14, the recipe; the wire steps: section 15).  A thin layer over the
 * existing wire ops: the input ring (a J_CLS_SCRATCH slot, ALLOC + COMMIT
 * J_COMMIT_NOCHECK once per epoch), an ALLOC per output, one DECODE
 * record with J_DEC_RING, the file streamed through the ring as the BCM
 * asks for each pass, then COMMIT J_COMMIT_NOCHECK per output.  No wire
 * change.
 *
 * Where the pixels go: always into BCM slots, one per output, cw x ch,
 * the class and key the caller names (album-render.md sections 4.3 and 6:
 * the NP box in J_CLS_THUMB, the panel window in J_CLS_DRIFT or
 * J_CLS_STRIP).  The slot is the instant display (a BLIT, no upload) for
 * this epoch; bcmjob_read() brings back what the art cache must keep (the
 * box to compose, the panel to write to npcover.pak), because every slot
 * dies at the next cut and the file is what the next lit spell shows.
 *
 * Hits and misses: an output whose READY slot (its key, size and class)
 * already exists this epoch is a hit.  It keeps its pixels: it is never a
 * DECODE destination and never freed by the call.  The DECODE is sent
 * with the missed outputs only, in request order (res->miss[]), so a
 * failed, aborted or timed-out decode never touches a hit slot.  Every
 * output a hit: nothing is decoded or streamed.
 *
 * The exactness promise: each missed output's slot holds exactly what
 * jdec_run() (jdec.h, the CPU twin on the PP) writes for the same source
 * given the same reduced set, the misses in request order
 * (bcmjob_dec_set() builds it from req and res).  Decode-once makes every
 * output but the primary from the primary's rows, so the twin must be
 * given that set, not the request's, to agree.  The primary is the
 * reduced set's own: jdec_primary() of the misses (the largest dw x dh,
 * the first of equals in request order; bcmjob_dec_primary()), so when
 * the request's largest output hits, the largest miss is the primary,
 * decoded as a single decode at its own size.  A one-output set (n = 1,
 * or one miss) is the shell's single decode at dw x dh (clip_jpeg_fd,
 * FORMAT_NATIVE | FORMAT_DITHER | FORMAT_RESIZE) cropped to the window:
 * today's art builder's pixels.  Bit for bit [sim: tests/jpeg golden tables,
 * jpeg_test.py --jref, decode_image_test.py; the reduced set: jmtest,
 * tests/bcmjob].  A hit's pixels are whatever its earlier decode (or
 * upload) left; its res->out[k].a, b are its own sums.
 *
 * A background decode of a large source passes its own ms rather than
 * the BCMJOB_DEC_CAP_MS default (a 3000 px cover at 27 MHz is about 8.8
 * s; two deadline misses mark DECODE slow for the boot). */

/* One output: the shell's sizes (its fill/fit rules, never the BCM's) */
struct bcmjob_dec_out
{
    uint16_t dw, dh;                /* the source resized to dw x dh */
    uint16_t cx, cy, cw, ch;        /* the window kept: the slot is cw x ch */
    uint8_t cls;                    /* its slot's class (J_CLS_*) */
    uint32_t key_lo, key_hi;        /* its slot's content key (ALLOC): a
                                       READY slot of this key, size and
                                       class this epoch is a hit */
};

struct bcmjob_dec_req
{
    /* The file: the same source object the CPU twin reads (jdec.h).  Its
     * bytes as stored: an embedded picture with AA_FLAG_ID3_UNSYNC or
     * AA_FLAG_VORBIS_BASE64 is not a JPEG stream and never comes here.
     * Read only from the calling thread, from 0 at each pass the BCM asks
     * for, in order, in pieces of at most 8 KB. */
    const struct jdec_src *src;
    const struct bcmjob_dec_out *out;
    int n;                          /* 1 .. J_DEC_MAX */
    uint32_t flags;                 /* BCMJOB_DEC_* */
    uint32_t ms;                    /* the deadline from the record's queueing
                                       to its reply; 0: BCMJOB_T_DECODE(len,
                                       the BCM's clock), capped at
                                       BCMJOB_DEC_CAP_MS (a longer decode
                                       passes its own ms) */
    /* Polled between ring pieces and at every wait step (about 1 ms):
     * true stops the decode, BCMJOB_ABORTED.  A file still streaming gets
     * J_DEC_ABORT (the BCM fails it at its next read); one wholly in the
     * ring runs to its end, waited for, and is dropped.  NULL: never. */
    bool (*cancel)(void *ctx);
    void *ctx;
};

/* The input ring both implementations use: the scratch class's one slot,
 * 128 x 128 RGB565 = 32 KB (a power of two in 4-128 KB, jobs.h), key
 * "RING" (lo; hi 0), allocated and committed J_COMMIT_NOCHECK at the first
 * decode of an epoch and kept.  The scratch slot has one owner at a time:
 * another user frees it first, and the next decode allocates it again. */
#define BCMJOB_DEC_RING_W   128
#define BCMJOB_DEC_RING_KEY 0x52494E47
#define BCMJOB_DEC_PIECE    8192    /* the PP's largest ring write */

/* flags */
#define BCMJOB_DEC_NO_WAKE  0x1     /* kept for source compatibility; every
                                       decode is lit-only now: DECODE never
                                       powers the BCM (the owner,
                                       2026-10-24: a cold start is 0.4-0.6 s
                                       on the UI thread).  When the BCM
                                       isn't up the call answers OFF, which
                                       bcmjob_dec_verdict() maps to DEFER:
                                       the art builder retries when the BCM
                                       is up (lit), or decides to use the
                                       CPU twin by its own policy */
#define BCMJOB_DEC_HOLD_MS  2000    /* unused since the rule above */

struct bcmjob_dec_res
{
    uint32_t err;                   /* FAILED: the job's J_E_* / HP_E_* */
    uint16_t sw, sh;                /* the source's size (OK; 0 when every
                                       output hit: nothing decoded) */
    uint32_t epoch;                 /* the epoch of the slots below */
    struct
    {
        uint32_t slot;              /* READY, cw x ch (OK only; freed on any
                                       other answer) */
        bool hit;                   /* already held this epoch: not decoded,
                                       never written or freed */
        uint32_t a, b;              /* its F16w, the BCM's (COMMIT's reply):
                                       bcmjob_read()'s pixels must sum to it */
    } out[J_DEC_MAX];
    /* The reduced set: the missed outputs (req->out indices, in request
     * order), the DECODE's out[] and the set the twin must be given
     * (bcmjob_dec_set()).  Filled as the ALLOCs answer: complete once
     * they all have (OK, and any later answer); miss_n 0: every output
     * hit, or the call ended before its first miss */
    uint8_t miss_n;
    uint8_t miss[J_DEC_MAX];
    uint32_t passes, bytes;         /* the ring passes served, bytes streamed */
    bool read_err;                  /* the PP's read of src failed: the job
                                       was aborted (err J_E_DEC_CORRUPT, as
                                       jdec's own read error) */
    bool stalled;                   /* the PP fed nothing for J_DEC_STALL_MS:
                                       a CORRUPT is then not the file's */
};

/* The decode-once primary of a set (jdec_primary(): the largest area,
 * the first of equals) */
static inline int bcmjob_dec_primary(const struct bcmjob_dec_out *o, int n)
{
    int k, p = 0;

    for (k = 1; k < n; k++)
        if ((uint32_t)o[k].dw * o[k].dh > (uint32_t)o[p].dw * o[p].dh)
            p = k;
    return p;
}

/* The work a set needs on the BCM (jdec.h jdec_work(): the decoder's
 * state, its rows at the primary's scale, and JDEC_WORK_OUT for every
 * output; jdec streams its outputs, so none is held whole).  The NP box
 * (142 x 142) needs 114,172 B, the panel's fill 137,240 B (300 x 300) to
 * 171,404 B (534 x 300, a 16:9 cover), and the whole 16:9 art set with
 * it 184,428 B: all fit (album-render.md section 5.2; before the
 * streaming, the panel needed 378,160 B and up and went to the twin).
 *
 * bcmjob_decode() checks it against J_DEC_WORK_MIN for the set it would
 * send, the reduced set (the misses), and never sends one over: FAILED,
 * err HP_E_NOMEM (a NOMEM sent would count toward the op's failed mark).
 * Before any traffic it refuses a request none of whose outputs fits
 * even alone (no reduced set could, and every set holds its primary's
 * whole need: so an n = 1 request over the minimum is refused unsent, as
 * before); otherwise the check runs once the ALLOCs have answered, and a
 * reduced set over the minimum frees the misses' slots, the hits kept,
 * with no DECODE sent.  A set that fits only once a hit is dropped is
 * decoded. */
static inline uint32_t bcmjob_dec_work(const struct bcmjob_dec_out *o, int n)
{
    int k, p = bcmjob_dec_primary(o, n);
    uint32_t w = JDEC_WORK_BASE + JDEC_WORK_ROWS(o[p].dw);

    for (k = 0; k < n; k++)
        w += JDEC_WORK_OUT(o[k].dw);
    return w;
}

/* The reduced set (the misses, in request order) of a request into set[]
 * (J_DEC_MAX entries): the DECODE's out[], and what the CPU twin is given
 * for the exactness promise.  Returns its size, res->miss_n */
static inline int bcmjob_dec_set(const struct bcmjob_dec_out *out,
                                 const struct bcmjob_dec_res *res,
                                 struct bcmjob_dec_out *set)
{
    int k;

    for (k = 0; k < res->miss_n; k++)
        set[k] = out[res->miss[k]];
    return res->miss_n;
}

/* The default deadline: J_T_DECODE per started MB at 150 MHz, scaled to
 * the BCM's clock (27 in UI mode: x 150/27) */
#define BCMJOB_T_DECODE(len, mhz) \
    ((uint32_t)J_T_DECODE * (1 + ((uint32_t)(len) >> 20)) * 150 / (mhz))
/* The default's cap (owner, 2026-10-24): a decode never holds its caller
 * (the art thread, which must ack USB and a shutdown) longer than a few
 * seconds; past it TIMEOUT, the CPU path.  USB and a shutdown end a
 * decode at once, ABORTED (DEFER), whatever its deadline. */
#define BCMJOB_DEC_CAP_MS   3000

/* One decode, blocking: the art thread's (one in flight; a second caller
 * gets BUSY).  Returns
 *   OK       every output's slot READY with its pixels (res->out): the
 *            misses decoded (res->miss), the hits as they were;
 *   FAILED   res->err: J_E_DEC_UNSUPPORTED, _CORRUPT, _TRUNCATED (the
 *            source; never counted toward the failed mark), HP_E_NOMEM
 *            (bcmjob_dec_work() of the reduced set over J_DEC_WORK_MIN:
 *            no DECODE sent, see above), J_E_RANGE
 *            (a window outside dw x dh, n outside 1..J_DEC_MAX: not
 *            sent), J_E_NOSLOT (an output's or the ring's pool is full),
 *            J_E_MODE (video or a game holds the pools), or the job's own;
 *   REFUSED  dark on battery (P19 D-3; "bcmjob fault op=0x30 why=refused");
 *   OFF      the BCM not up (dark, even on the charger: DECODE never
 *            powers it), starting, cut or
 *            restarted meanwhile (a new epoch: the outputs are gone),
 *            suspended, jobs off;
 *   NOJOB    the image lacks DECODE (the merged image), or it is marked
 *            slow or failed this boot;
 *   BUSY     the command ring is full, or another decode is in flight;
 *   TIMEOUT  the deadline passed: the stream is aborted, and DECODE is
 *            marked slow on its second deadline miss this boot
 *            (jobs.md section 6);
 *   ABORTED  req->cancel said stop.
 * On every answer but OK the misses this call allocated are freed (or
 * gone with their epoch); no partial pixels are ever left READY, and a
 * hit is untouched: READY, its pixels and sums as before the call.
 * Traces: "bcmjob decode len=N n=N" at the start, "bcmjob decode done
 * st=N err=N passes=N bytes=N ms=N" at the end, and the fault lines. */
int bcmjob_decode(const struct bcmjob_dec_req *req, struct bcmjob_dec_res *res);

/* A slot's rect back into PP memory by port reads (data-paths.md R2:
 * about 1 us a word idle [hypothesis], 55-85 us under load [silicon
 * S12]: a 142 x 142 box 5-10 ms idle; read as little as possible), in
 * bands of any height: the caller sums them with j_f16w() row by row and
 * compares with res->out[k].a, b when it has read the whole slot.  OK,
 * OFF (another epoch, the BCM down), FAILED (not a slot, the rect outside
 * it).  Takes the port lock per row. */
int bcmjob_read(uint32_t slot, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                uint16_t *px, uint32_t pitch);

/* ---- The visualizer (PROPOSED, SIM ONLY; #126) --------------- */
/* SPECTRUM, and wait for its reply: the nb band levels (jobs.h
 * J_OP_SPECTRUM: 16 log2 of each band's largest power, jviz.h jviz_l2) of
 * the n samples (256 or 512, split order) the caller wrote to slot (a
 * READY slot, committed once with J_COMMIT_NOCHECK; it may be rewritten
 * after this returns), bands edges[0..nb].  OK, OFF, NOJOB (the CPU path:
 * jviz_power_ref and jviz_bands, the same levels), FAILED.  BARS has no
 * reply: bcmjob_submit(J_OP_BARS, ...).  The host model and the device
 * driver (bcmjob-core.c) both implement it; on the device the ops come
 * from the display-only image's overlay (bcmjob_pp.h bcmjob_viz_ops()). */
int bcmjob_spectrum(uint32_t slot, uint32_t n, const uint16_t *edges, uint32_t nb,
                    uint16_t *l2);

/* What a bcmjob_decode() answer means for the source, in jdec's terms, so
 * the shell does the same thing whichever side decoded:
 *   JDEC_OK            the pixels are in the slots;
 *   JDEC_E_CORRUPT,
 *   JDEC_E_TRUNCATED   the file's verdict, the one the twin would reach
 *                      (one definition, jdec.h): the next source, as on the
 *                      CPU.  read_err: the PP's own read failed, which the
 *                      twin also calls CORRUPT; the shell treats it as its
 *                      read failures (source_retry);
 *   BCMJOB_DEC_TWIN    no verdict from the BCM: run the CPU twin on this
 *                      source now, if the shell's own CPU rules allow
 *                      (UNSUPPORTED: progressive may decode on the PP, and
 *                      its answer is the sticky one; NOMEM, RANGE, OFF,
 *                      NOJOB, BUSY, TIMEOUT, REFUSED, a stalled CORRUPT,
 *                      any other failure);
 *   BCMJOB_DEC_DEFER   ABORTED: neither side; the render waits for its
 *                      next chance (the caller's own deferral). */
#define BCMJOB_DEC_TWIN     1
#define BCMJOB_DEC_DEFER    2
static inline int bcmjob_dec_verdict(int r, const struct bcmjob_dec_res *res)
{
    if (r == BCMJOB_OK)
        return JDEC_OK;
    if (r == BCMJOB_ABORTED || r == BCMJOB_OFF)
        return BCMJOB_DEC_DEFER;   /* cancelled, or the BCM not up: later */
    if (r == BCMJOB_FAILED && res->err == J_E_DEC_CORRUPT && !res->stalled)
        return JDEC_E_CORRUPT;
    if (r == BCMJOB_FAILED && res->err == J_E_DEC_TRUNCATED)
        return JDEC_E_TRUNCATED;
    return BCMJOB_DEC_TWIN;
}

/* ---- The port lock (gap G3) ------------------------------------------- */
/* ONE lock for every writer to the host port, on either core: the
 * command ring, engine 2's slot pieces, the LCD contract's commands and
 * pushes, and the video feed.  Held for a whole piece (a record, an
 * engine-2 piece, a push), recursive, cross-core (a corelock under a
 * mutex).  On the device it is lcd-video.c's port mutex and lcd_state's
 * corelock, renamed; the job driver takes it, never a second lock. */
void bcm_port_lock(void);
void bcm_port_unlock(void);

/* ---- Power (gaps G11, G12) ---------------------------------------------- */
/* The platform derives the BCM's state from the backlight and the mode
 * (power-contract.md): lit means up, dark means cut.  The shell never
 * asks for that.  The only request is to power the BCM while DARK,
 * for a reason, for at most hold_ms: */
enum bcm_want
{
    BCM_WANT_DECODE = 1,    /* the art builder's JPEG jobs: on the
                               charger only (refused on battery) */
    BCM_WANT_GAME,          /* a Class C session with the screen off
                               (paused game): its slots kept */
    BCM_WANT_VIDEO,         /* a video session's own sleep handling */
    BCM_WANT_N
};
int bcm_power_want(enum bcm_want why, uint32_t hold_ms);
void bcm_power_release(enum bcm_want why);

/* Power-ups while dark since boot, by reason (index 0: any).  The driver
 * logs "bcm power ups dark=N decode=a game=b video=c" once an hour of
 * dark time; in dark playback it must read 0 (the brief's section 8). */
uint32_t bcmjob_dark_ups(enum bcm_want why);

/* ---- Trace (the gate matches these lines) ----------------------------- */
/* bcmjob submit op=0xNN tag=N n=N
 * bcmjob fence seq=N
 * bcmjob fault op=0xNN why=busy|off|nojob|timeout|refused|aborted
 * bcmjob fault op=0xNN st=N why=failed [slot=N WxH]
 * bcmjob decode len=N n=N
 * bcmjob decode done st=N err=N passes=N bytes=N ms=N
 * bcm power state=C0|C1|C2|C3|C4|C5 why=lit|dark|want-<reason>|fault
 * drift still why=...           (the shell's, D-1's degraded path)
 * coverflow unavailable why=... (the shell's) */

#endif /* BCMFW_BCMJOB_H */
