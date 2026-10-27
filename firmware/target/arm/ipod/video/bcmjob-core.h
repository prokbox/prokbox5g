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

#ifndef BCMJOB_CORE_H
#define BCMJOB_CORE_H

/* The BCM2722 job driver's logic (P19 W0-6): bcmfw/jobs/bcmjob.h over a
 * host port, with no Rockbox headers, so the same code runs on the PP
 * (bcmjob-dev.c, #included by lcd-video.c) and in the host test
 * (tests/bcmjob, against a chip built on the reference executor).
 *
 * The rules it keeps (jobs.md sections 2, 3, 6; brief sections 3.1, 3.3):
 *  - nothing touches the port while the BCM is not up with our image
 *    (gen() == 0: C0, a standby, the developer stages, a hung image) or
 *    jobs are off: every call answers OFF at once, never polls;
 *  - discovery once per power-up, lazily, in the caller's thread: the
 *    header (HP_VERSION, HP_CAP_JOBS_2D), HELLO, HP_JOBS, J_VERSION (magic
 *    and major), J_OPS; any mismatch is the CPU path for that power-up;
 *  - submit never blocks: BUSY when the ring is full;
 *  - a missed deadline with a live heartbeat is a slow job (logged, its
 *    second miss marks it: its CPU path for the boot); a dead heartbeat or
 *    the fault word is a failure: restart() at once (G2), and after the
 *    second failure jobs stay off for the boot;
 *  - the epoch is the driver's own: it moves at every power-up, restart,
 *    resume and BCM-side epoch change; on_epoch runs in the next API
 *    caller's thread, never in the backlight thread or an interrupt. */

#include <stdbool.h>
#include <stdint.h>
#include "bcmjob.h"

struct bjc_port
{
    /* the host port, every call bounded: < 0 when it gave up */
    int (*rd)(uint32_t addr, uint32_t *val);
    int (*wr)(uint32_t addr, uint32_t val);
    int (*wr_block)(uint32_t addr, const uint16_t *src, uint32_t n);
    /* engine 2: as many bytes as it moved (whole pieces), 0 if none */
    uint32_t (*upload)(uint32_t addr, const uint16_t *src, uint32_t bytes);
    void (*doorbell)(void);
    void (*lock)(void);         /* bcm_port_lock(): one piece at a time */
    void (*unlock)(void);
    uint32_t (*now_us)(void);   /* waits (wraps: short spans only) */
    uint32_t (*now_ms)(void);   /* accounting (the dark hours) */
    void (*yield)(void);
    /* 0: not up with our image; otherwise a number that changes at every
     * power-up and every hand-back.  Never touches the port. */
    uint32_t (*gen)(void);
    bool (*lit)(void);          /* the panel is on (power-contract.md) */
    /* the LCD contract's push of the PP framebuffer's rect to the front
     * frame and the panel (the port lock is not held) */
    int (*push)(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
    /* a failed BCM: cut, our image again, the PP framebuffer first */
    void (*restart)(void);
    /* a dark want: the BCM up with the panel dark, or cut again */
    int (*power)(bool up);
    bool (*charging)(void);
    /* bcmjob_decode()'s ring pieces (P19 19-6): engine 2 with no minimum
     * size, as many bytes as it moved (whole pieces), 0 if none (NULL:
     * the CPU's copy); the port lock is held */
    uint32_t (*upload_any)(uint32_t addr, const void *src, uint32_t bytes);
    /* BCMJOB_DEC_PIECE bytes, word-aligned and pinned, for one decode's
     * pieces (get), then given back (!get); NULL from get: no decode
     * (HP_E_NOMEM).  Called on the decoding (art) thread: memory only,
     * never the port */
    void *(*dec_buf)(bool get);
};

/* bcmjob_upload_rect() (bcmjob.h, jobs rev 0.4) on the device: BACK's
 * fixed address (memory-contract.md section 2), the ring owner only
 * (another thread: FAILED), pieces of whole rows each under the port lock
 * (engine 2 for whole-width bands of 16 rows, else the CPU; an odd first
 * or last pixel by a read-modify-write of its word), OFF if the BCM
 * restarts between pieces.  The time budget: a piece starts only if the
 * time spent plus the last piece's fits BJC_RECT_US */
#define BJC_BACK_ADDR   0x105800    /* J_SURF_BACK, 320 pixels a row */
#define BJC_FRAME_W     320
#define BJC_FRAME_H     240
#define BJC_RECT_US     4000

/* Per-slot facts from ALLOC's reply, kept for the upload */
struct bjc_slot
{
    uint32_t addr;              /* host-visible address of pixel 0 */
    uint16_t pitch, w, h;       /* pitch in bytes */
    uint16_t ep;                /* the driver's epoch at its ALLOC (low
                                   16 bits: bcmjob_read's check) */
    bool staged;                /* addr is the image's staging window
                                   (jobs.h J_ALLOC_STAGED, rev 0.5) */
};
#define BJC_SLOTS 64

/* trace: the gate's lines (NULL: none).  slots: BJC_SLOTS entries owned
 * by the caller (the device's are in the job client's allocation). */
void bjc_init(const struct bjc_port *port, struct bjc_slot *slots,
              void (*trace)(const char *line));
/* The panel went dark (lcd_sleep) or lit again: accounting only, no
 * port access, safe on the backlight thread */
void bjc_dark(bool dark);
/* Log every submit and fence (developer: the gate's lines on the device
 * cost a ui_log each); faults and power lines always */
void bjc_verbose(bool on);
/* For the host test and the debug screen: 0 not tried, 1 jobs, 2 the CPU
 * path this power-up, 3 jobs off this boot */
int bjc_state(void);
/* The chip failures seen this boot (chip_failed: no heartbeat, or the
 * image's fault word) */
uint32_t bjc_chip_failures(void);
/* The J_VPU (jobs.h 0x44) an image with these J_OPS reports when its
 * vector unit passed every start-up check: CHECKED | SHIFT (3), plus MAC
 * (7) if it decodes (J_OP_DECODE).  Discovery logs the word read against
 * it ("bcmjob vpu=0x3 expect=0x3"); a smaller word adds "why=degraded"
 * (a check failed: those jobs take the image's exact C paths, slower, not
 * a fault) or, 0, "why=unchecked" (an image before the word). */
uint32_t bjc_vpu_expected(uint32_t ops);

/* ---- The overlay loads (jobs.md section 5, "Overlays"; #126) ------------
 * An image that offers J_OP_OVERLAY (J_OPS bit 21) has one overlay
 * region (J_OVL: its base and size) and takes there an overlay the PP
 * writes and starts with OVERLAY.  Each overlay is an instance of the
 * loader (BJC_OVL_*), with its own record, ops, epoch and tries:
 *  - BJC_OVL_VIZ: the visualizer's SPECTRUM and BARS (and the line ops
 *    beside them), viz-ovl.bin, linked at the display-only image's
 *    region (0x13B000);
 *  - BJC_OVL_DEC: DECODE, dec-ovl.bin, linked at the merged image's
 *    region (0x12B000; album-render.md section 5.3).  Its init sets
 *    J_CAPS J_CAP_DEC_LENT: bcmjob_decode() then lends the work slot.
 * The file comes from the build that linked the image: rec is that
 * build's record of it (mkdevtest.py for the visualizer's, mkvideo.py for
 * DECODE's: its size, FNV-1a, ABI word, where the image's own copy of the
 * ABI word sits (the stub's 48-bit immediate), and the region's base it
 * was linked at).  bjc_ovl_set(k, rec) names it (copied; a new record
 * starts the instance afresh); an instance is due only while the running
 * image's region is the record's base, so the PP loads the overlay of the
 * image that runs and never reads the other's file.  Then three calls on
 * the owner's thread, the file streamed in between:
 *  - bjc_ovl_begin(k, hdr, bytes): hdr is the file's first 7 words.
 *    NOJOB, no line, unless bjc_ovl_due(k).  Then checks, each refusal
 *    logged ("bcmjob overlay refused why=..."): the file against rec (its
 *    size, magic and ABI word: why=file, never again this boot); the
 *    header against J_OVL (its base, load end, .bss end inside the
 *    region: why=region); the running image's ABI word, read at
 *    rec->abi_at (why=image).  Then drops the other instance's ops (no
 *    more of them queued) and retires every queued job (a FENCE, polled:
 *    BJC_OVL_AGAIN below), so no overlay op runs while the region is
 *    rewritten (jobs.md: only while no overlay op is queued).  BCMJOB_OK:
 *    the region's base is the file's offset 0.
 *  - bjc_ovl_piece(k, off, src, bytes): bytes of the file at off (whole
 *    words) to the region, engine 2 for whole pieces and the CPU for the
 *    rest, under the port lock.  OFF if the epoch moved since begin.
 *  - bjc_ovl_end(k, fnv, a, b): fnv, the streamed file's FNV-1a (against
 *    rec: why=file); a and b, its F16w as halfwords (jobs.h j_f16w):
 *    OVERLAY words, A, B (posted, its reply polled: BJC_OVL_AGAIN), then
 *    J_OPS, J_VPU and J_CAPS read back
 *    ("bcmjob overlay ok ops=... vpu=..."); the ops it added last until
 *    the epoch moves.  On J_E_SUM (a torn upload) the next begin in this
 *    epoch may try once more; HP_E_VERSION (another build of the image)
 *    is never retried this boot; any other failure waits for the next
 *    epoch.
 * A load replaces whatever the region held (the BCM drops the other
 * instance's ops first; that instance's load is due again at once, so
 * the merged image's DECODE and visualizer swap in their one region:
 * "bcmjob overlay evict k=K by=L"), an epoch change drops every
 * instance's ops (a
 * restart, a J_EV_EPOCH: a mode change, a RESET) and J_CAP_DEC_LENT with
 * DECODE's, and bjc_ovl_due(k) asks for the load again: the region is a
 * cache, as the slots are. */
enum
{
    BJC_OVL_VIZ = 0,            /* the visualizer's ops */
    BJC_OVL_DEC,                /* DECODE (the merged image) */
    BJC_OVL_N
};
struct bjc_ovl_rec
{
    uint32_t bytes;             /* the file's size (whole words) */
    uint32_t fnv;               /* its FNV-1a 32 */
    uint32_t abi;               /* its header's ABI word (vc2cc __vc2_abi) */
    uint32_t abi_at;            /* the image's own copy of the word: its
                                   byte offset in the image (loaded at
                                   0); 0: not checked */
    uint32_t base;              /* the region it was linked at (its
                                   header's word 2): the image's J_OVL */
};
#define BJC_OVL_HDR_WORDS 7     /* jref.h struct jref_ovl */
/* bjc_ovl_begin() and bjc_ovl_end() never wait for the BCM inside a call
 * (0.8.0, sitting 4c: the UI pass, the job owner, stalled on them): the
 * begin's FENCE (only while jobs are in flight) and the end's OVERLAY are
 * posted, and the call answers BJC_OVL_AGAIN until they are done.  The
 * caller calls again on a later pass with the same arguments (begin: the
 * same hdr; end: its arguments are not read again) and gets the answer
 * the blocking call gave, within the same bounds (the FENCE's J_T_FENCE_BASE
 * + J_T_PRESENT, OVERLAY's J_T_FENCE_BASE + J_T_OVERLAY: TIMEOUT past
 * them) and with the same lines.  A caller that stops asking leaves
 * nothing behind: the next load of either kind, a new record, an epoch
 * change or the bound ends the posted wait (its reply box freed) */
#define BJC_OVL_AGAIN     1
/* Name instance k's file (rec copied; bytes 0: none, never due) */
void bjc_ovl_set(int k, const struct bjc_ovl_rec *rec);
/* A load of instance k is wanted now: its record names the running
 * image's region, the image offers OVERLAY, the ops k adds are missing,
 * and this epoch (this boot, after a refused file or version) has a try
 * left.  No port access but discovery's. */
bool bjc_ovl_due(int k);
/* The running image's overlay region (J_OVL's base), 0: none (no jobs,
 * no OVERLAY): which record's file is the one to load */
uint32_t bjc_ovl_region(void);
int bjc_ovl_begin(int k, const uint32_t *hdr, uint32_t bytes);
int bjc_ovl_piece(int k, uint32_t off, const void *src, uint32_t bytes);
int bjc_ovl_end(int k, uint32_t fnv, uint32_t a, uint32_t b);
/* DECODE lends its work slot now (J_CAP_DEC_LENT: the merged image's
 * overlay is loaded this epoch): bcmjob_decode() ALLOCs a J_CLS_DRIFT
 * slot keyed J_DEC_WORK_KEY behind the outputs and FREEs it after.  The
 * caller frees the pool's room first (the drift's own cover) */
bool bjc_dec_lends(void);

#ifdef HAVE_BCMFW_DEVTEST
/* Developer only (the bench drill "forced job failure"): the next n jobs
 * of op (0x20-0x3F) go out asking for a reply (J_TAG_REPLY), and those
 * replies, if the BCM answered HP_OK, are read as BJC_INJECT_ERR.  The
 * real reply path then runs: a failure traced and counted toward the
 * failed mark (bcmjob.h BCMJOB_FAIL_MARK), or a call's status to its
 * caller (ALLOC).  The BCM really ran the jobs; only their status says
 * failed (J_ERRORS, read by bcmjob_errors(), does not move).
 * Injections in flight at a new power-up are dropped with the ring.
 * n = 0 disarms.  The ring owner's thread only. */
#define BJC_INJECT_ERR HP_E_INVAL   /* a counted failure, not a normal outcome */
void bjc_inject_fail(uint32_t op, uint32_t n);
void bjc_inject_forgive(uint32_t op);
#endif

/* ---- one writer for the driver's state (P19 S0, design MF-2) ------------
 * The driver's state is the ring owner's alone (the thread that calls the
 * job API: the shell's UI thread, the COP after 19-9).  Other threads ask
 * through this record, which lives in uncached shared memory on the
 * device; each word has one writer:
 *  - want: the suspend requester's (one requester at a time: the device
 *    test's take and restore, the video player through them), seq << 1 |
 *    on;
 *  - done: the owner's, the last want it applied;
 *  - dseq, dark, since, total: the backlight thread's dark record (a
 *    seqlock: dseq is odd while it writes), read by the owner's
 *    accounting.  A dark change never wakes the owner: nothing needs it
 *    before the owner's next call. */

/* A cache line of the PP5022 (xcore.h XCORE_LINE): the unit a hand-off
 * buffer's ends keep to (P19 S10) */
#define BJC_LINE 16

/* One bcmjob_decode() or bcmjob_read() call (P19 19-6; S10, the hand-off
 * across cores).  It lives in the request record, so in uncached shared
 * memory on the device and never in the caller's frame: the caller fills
 * it before it posts a_seq; the owner copies it once when it takes the
 * call, and writes ret, res and the edge lines before it sets o_seq; the
 * caller reads them after it sees o_seq.  One call at a time: one block.
 * The only memory of the caller's that the owner touches is buf (read)
 * and px (written), each inside a pass (o_busy) and never once the caller
 * has had its acknowledgement (bcmjob-core.c's state table). */
struct bjc_call
{
    uint8_t kind;               /* 1 decode, 2 read */
    bool cross;                 /* caller and owner on two cores
                                   (own->cross() at the post): the
                                   hand-off steps run */
    int32_t ret;                /* the answer, when o_seq says done */
    uint32_t bound_ms;          /* the caller's own bound once taken */
    /* a decode: the request by value, and the answer */
    uint32_t n, len, ms;
    struct bcmjob_dec_out out[J_DEC_MAX];
    struct bcmjob_dec_res res;
    uint8_t *buf;               /* the caller's piece buffer (its cached
                                   address, BJC_LINE-aligned at both ends) */
    /* a read */
    uint32_t slot, x, y, w, h, pitch;
    uint16_t *px;
    /* a read across cores: px's first and last cache lines when they hold
     * bytes outside the read (memory the caller's core may write
     * meanwhile).  The owner puts its pixels for those lines here, never
     * in px; the caller copies in the bytes edge_set marks, after the end,
     * on its own core. */
    uintptr_t edge_at[2];       /* the line's address (0: none) */
    uint16_t edge_set[2];       /* bit k: byte k of the line written */
    uint8_t edge[2][BJC_LINE];
};

struct bjc_req
{
    volatile uint32_t want, done;
    volatile uint32_t dseq, dark, since, total;
    /* bcmjob_decode() and bcmjob_read() (P19 19-6) from another thread
     * (the art thread): a call is posted here and the owner runs its wire
     * steps in its own passes.  a_*: the caller's words; o_*: the owner's.
     *  - a_seq: the call posted (c holds it); o_take: the call the owner
     *    accepted; o_seq: the call it finished (== a_seq: idle);
     *  - o_want counts the owner's asks for a piece of the file (o_want_n
     *    bytes, after the source's start() if o_want_start); the caller
     *    fetches it into its piece buffer, puts the count in a_got (< 0 a
     *    read error) and a_have = o_want;
     *  - a_stop: the call whose cancel hook said stop;
     *  - a_quit: the call its caller gave up on (never taken within
     *    BJC_ACK_MS, or past the caller's own bound): the owner ends it
     *    without touching the caller's memory again.  a_seq never goes
     *    back;
     *  - o_busy: the call the owner is inside a pass of.  Once the caller
     *    has set a_quit and then seen o_busy differ (the acknowledgement),
     *    the owner never touches its memory again (bcmjob-core.c's state
     *    table);
     *  - a_kick / o_kick: a kick is outstanding while they differ (one
     *    post to the owner's queue at a time); a_kick_t: when the caller
     *    last posted one (a kick dropped on the way is posted again after
     *    BJC_KICK_STALE_MS);
     *  - pass: the owner's pass, set by the first decode or read before its
     *    post (so an image that never decodes links none of it); the owner
     *    reads it here, never from a static cached on the other core. */
    volatile uint32_t a_seq, o_take, o_seq;
    volatile uint32_t o_want, o_want_n, o_want_start, a_have;
    volatile int32_t a_got;
    volatile uint32_t a_stop, a_quit, o_busy, a_kick, o_kick, a_kick_t;
    void (*volatile pass)(void);
    struct bjc_call c;
};

struct bjc_owner
{
    struct bjc_req *req;        /* NULL: the driver's own (one thread) */
    bool (*is_owner)(void);     /* the calling thread is the owner */
    void (*kick)(void);         /* prompt the owner (a post to its queue) */
    int (*wait)(uint32_t ms);   /* block for an ack; < 0: none in ms */
    void (*ack)(void);          /* the owner applied a request */
    void (*mode)(bool jobs);    /* job mode on or off (lcd_tick) */
    /* a decode's caller (bcmjob_decode(), bcmjob_read() on the art
     * thread): block until the owner wakes it or ms pass (NULL: yield),
     * and the owner's wake */
    void (*art_wait)(uint32_t ms);
    void (*art_wake)(void);
    /* P19 S10, the hand-off across cores.  NULL, or cross() false: the
     * caller and the owner share one core's cache and none of this runs
     * (today's build).  cross: asked on the caller at each post, true when
     * the owner runs on the other core.  Then:
     *  - publish: the caller, after writing [p, p + n) and before the
     *    owner or a DMA engine reads it, or before the owner writes it:
     *    its own dirty lines written back (xcore_publish());
     *  - acquire: the caller, after the owner wrote [p, p + n): its stale
     *    lines dropped (xcore_acquire());
     *  - uncached: the owner's alias of the caller's memory
     *    (UNCACHED_ADDR): the owner reads a piece and writes a read's
     *    rows through it, so its own cache never holds the caller's
     *    lines. */
    bool (*cross)(void);
    void (*publish)(const void *p, uint32_t n);
    void (*acquire)(void *p, uint32_t n);
    void *(*uncached)(void *p);
};

#define BJC_ACK_MS 500          /* a suspend's acknowledgement, at most;
                                   a call's take, and a give-up's
                                   acknowledgement, at most */
#define BJC_KICK_STALE_MS 50    /* an unanswered kick is posted again */

/* After bjc_init(), on the owner's thread */
void bjc_set_owner(const struct bjc_owner *o);
/* The owner: apply pending requests now (its queue's kick) and run one
 * pass of a posted decode or read; any other thread: nothing (every job
 * API call applies the requests too, but only this runs the pass) */
void bjc_apply(void);
/* The owner's budget (P19 19-6): while fn() says a scroll or animation
 * frame is due, the owner's pass writes no DECODE ring piece and reads no
 * slot rows; otherwise at most one 8 KB piece, or about 2 ms of rows, a
 * pass.  A pass never waits for the BCM: its ALLOCs, COMMITs and FREEs
 * are posted and their replies collected in later passes.  NULL (the
 * default): never due. */
void bjc_frame_due(bool (*fn)(void));
/* USB or a shutdown: the decode or read posted or running ends now,
 * ABORTED (bcmjob_decode_stop(), the owner's thread only) */
void bjc_decode_stop(void);
/* Calls given up on whose acknowledgement never came within BJC_ACK_MS,
 * this boot (P19 S10: the caller kept their memory; a decode's piece
 * buffer stays pinned until the owner acknowledges).  Each is traced
 * "bcmjob withdraw unacked". */
uint32_t bjc_leaks(void);
/* Any thread: suspend (on) or resume the driver.  The owner applies it at
 * once.  Another thread posts it, kicks the owner and waits for the
 * acknowledgement up to ms: BCMJOB_OK once applied, BCMJOB_TIMEOUT if
 * not (the request is withdrawn; a suspend's caller must then refuse to
 * touch the port, e.g. the video player's error screen).  Never call it
 * holding the port lock from a thread other than the owner: the owner may
 * be waiting for that lock. */
int bjc_suspend_request(bool on, uint32_t ms);

#endif /* BCMJOB_CORE_H */
