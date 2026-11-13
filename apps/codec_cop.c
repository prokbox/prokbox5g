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
 * ProkBox (Fit-2, docs/fit2-brief.md section 5): the codec's decode on the
 * second core.
 *
 * Upstream ran the codec thread itself on the COP for three weeks in 2007
 * and moved it back (66258a30a4: "more protection on file system level is
 * necessary"). Today the same thread still owns state the CPU shares in
 * cached memory: pcmbuf's indices with the PCM interrupt, buffering's
 * handles with the buffering thread, and pcm_play_lock(), which masks an
 * interrupt on the CPU only. So the codec *thread* stays on the CPU and
 * keeps every one of those; only the codec's own code (codec_run(), the
 * decode) runs on a COP worker. Each playback callback it makes is
 * carried back to the codec thread with cop_job_call(), and runs there
 * exactly as before. What crosses:
 *  - in: the codec image and its statics (written by the loader on the
 *    CPU): COPJ_CODE at the run; the bitstream: request_buffer() hands
 *    the COP the ring's uncached alias, after the CPU writes back its
 *    cache; read_filebuf() copies through the uncached alias of the
 *    codec's buffer (COPJ_CALL_WROTE); id3: its uncached alias; a
 *    configure() setting that is a pointer (cfg_ptrs): copied through its
 *    uncached alias on the CPU (codec_cop_cfg.h).
 *  - out: PCM: pcmbuf_insert() reads the codec's output through its
 *    uncached alias after the COP wrote back its cache (every call does).
 *  - the api struct the codec sees (ci_cop) is uncached; the CPU copies
 *    the playback fields into it after every call.
 * 0.8.3 colocate (owner, 2026-11-10): in steady state no call crosses at
 * all. The COP reads the compressed ring itself under a lease on the
 * codec's handle (request_buffer, advance_buffer, set_elapsed; #294:
 * read_filebuf and a short forward seek_buffer too; R7 item 11), and once
 * the PCM buffer has been full the write side of pcmbuf is leased to it
 * (it requests, commits and sleeps on a full buffer itself; R7 item 12).
 * Other seeks, track ends, commands, a refill the COP caught up with and
 * a request across the ring's wrap still make one call each.
 *
 * Owner, 2026-11-07 (0.8.0, Apple's core layout): decoding is on the COP
 * by default in every shell build, the release included: every load is
 * bound here and every run goes to the COP worker (CPL_MODE_COP, Fit-2),
 * with MP3's synthesis on the COP too. If the COP will not start or take
 * the run, the CPU runs it and the log says "COP FAILED" (cop_refused()).
 * This supersedes 2026-10-24's pin to the CPU (P19 19-1c,
 * docs/phase19-brief.md), which stays, with the screen rule, as one of
 * two developer A/B arms: the CPU always (the plain path: the real api,
 * nothing kept back, synthesis inline), or the core chosen per run by the
 * screen state (apps/codec_place_core.c: dark on the CPU, lit on the
 * COP), moved at a track boundary without a reload; a run on the CPU then
 * calls straight through and keeps ci_cop's fields in step.
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
#include "config.h"

#ifdef HAVE_COP_CODEC
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "thread.h"
#include "panic.h"
#include "logf.h"
#include "cop_job.h"
#include "codecs.h"
#include "codec_cop.h"
#include "dsp_proc_entry.h"     /* DSP_PROC_MISC_HANDLER, for REPLAYGAIN_SET_GAINS */
#include "codec_cop_cfg.h"     /* pointer settings across the cores */
#include "codec_place_core.h"   /* P19 19-1c: the core per run */
#include "dsp_core.h"          /* #282 step 4: the DSP on the COP */
#include "metadata.h"          /* road test: the track's format line */
#include "buffering.h"         /* 0.8.3 colocate: the ring's reader */
#include "playback.h"          /* p083-codecload: the start's steps */
#include "pcm_mixer.h"         /* road test: the output's rate and depth */
#include "backlight.h"
#include "lcd.h"
#include "ipodui/uilog.h"
#include "ipodui/heapchk.h"     /* 4gh-b5: the heap walk at a place */
#ifdef HAVE_CORE_LAYOUT_A
#include "corelayout.h"        /* 0.8.3: the FIQ's FIFO and gap stats */
#endif
#ifdef HAVE_BCMFW_VIDEO
#include "video/vplayer.h"
#endif

/* As the CPU's codec stack (codec_thread.c), less its IRAM */
#define COP_CODEC_STACK ((DEFAULT_STACK_SIZE + 0x2000) & ~15)

#ifdef HAVE_SCHED_BANDS
#define COP_CODEC_PRIO  PRIORITY_CODEC
#else
#define COP_CODEC_PRIO  PRIORITY_PLAYBACK
#endif

static struct cop_worker worker COP_SHARED_ATTR;
static struct codec_api ci_cop COP_SHARED_ATTR;    /* what the codec sees */
static volatile int run_status COP_SHARED_ATTR;
static char msg[96] COP_SHARED_ATTR;               /* panicf, logf */

static struct codec_api *real;      /* the CPU's api (codecs.c's ci) */
static bool enabled = true;         /* default on; the developer row clears it */
/* P19 19-1c (i): the COP's view is handed to every load the COP could
 * take (bound), and each run picks its core (on_cop) without a reload */
static bool bound;                  /* this load sees ci_cop */
static bool on_cop;                 /* this run decodes on the COP */
/* Owner, 2026-11-07: decoding is on the COP in every shell build, the
 * release included (Apple's core layout: the CPU keeps the UI, file
 * system, disk, USB and the BCM driver). It supersedes 2026-10-24's pin
 * to the CPU, which stays as a developer A/B arm with the screen rule */
static int place_mode = CPL_MODE_COP;
static struct cpl_state place;      /* the codec thread's only */
static uint32_t runs, cpu_runs;

/* #282 step 4: the DSP runs on the COP with the decode. The codec's
 * pcmbuf_insert() no longer crosses per frame: the COP runs dsp_process()
 * itself, into a window of pcmbuf the CPU granted, and calls the CPU only
 * to publish a window and take the next (codec_thread.c
 * codec_pcm_window()), about every WIN_PUBLISH samples of output. The
 * window is published before any call that can let playback reset the
 * write side (a command, a seek) and at the run's end. */
#define WIN_PUBLISH 4096    /* samples out: ~85 ms at 48 kHz, ~93 at 44.1 */

/* 0.8.3 colocate, the output side: with the PCM's FIQ on the COP (layout
 * A) the write side of pcmbuf is leased to the COP after a full-buffer
 * sleep, and no window crosses until something takes it back
 * (pcmbuf.h, docs/data-paths.md R7 item 12). pcmbuf.h is not included
 * here (the host model compiles this file without the firmware's
 * headers): its lease calls are declared below. COP_LEASE_MODEL: the
 * host model's stand-in. */
#if defined(HAVE_CORE_LAYOUT_A) || defined(COP_LEASE_MODEL)
#define CODEC_COP_LEASE
enum { PCMBUF_LEASE_FULL = 1, PCMBUF_LEASE_BACK };
void pcmbuf_lease_reclaim(void);
void *pcmbuf_lease_window(int *count, int *why);
void pcmbuf_lease_commit(int count, unsigned long elapsed, off_t offset);
long pcmbuf_lease_full_ticks(void);
size_t pcmbuf_lease_ridx(void);
void pcmbuf_lease_release(void);
void pcmbuf_lease_stats(uint32_t *grants, uint32_t *revokes,
                        uint32_t *commits, uint32_t *sleeps);
#endif

#if NUM_CORES > 1 && !defined(SIMULATOR)
#define DSP_NOW_US()    USEC_TIMER
#else
#define DSP_NOW_US()    0
#endif

struct cop_win {
    struct dsp_buffer dst;  /* the window: p16out, bufcount left, remcount
                             * written (samples) */
    unsigned long elapsed;  /* at the window's first sample */
    off_t offset;
    uint8_t hold;           /* may be held across frames (no crossfade) */
    uint8_t on;             /* this run's inserts run the DSP here */
    uint8_t leased;         /* 0.8.3: the COP holds pcmbuf's write side */
    uint8_t may_lease;      /* 0.8.3: this run may take it (CPU sets) */
    uint32_t lsleep_us;     /* 0.8.3: the COP's own full-buffer sleeps */
    uint32_t backs;         /* 0.8.3: leases the COP gave back */
    /* counters since boot (the COP writes, the CPU reads) */
    uint32_t frames, windows, dsp_us;
    /* road test (0.8.2): per block (one pcmbuf_insert, the codec's frame)
     * for this run; the COP writes, the CPU reads and resets them at the
     * run's start. dec: COP time from the last block's end to this one's
     * start less the COP's waits for CPU calls (buffer reads): the codec's
     * own work plus whatever else ran on the COP meanwhile; dsp: the
     * block's dsp_process() time. Both wall time (USEC_TIMER) */
    uint32_t blk_n, blk_max, dec_min, dec_max, dsp_min, dsp_max, worst;
    uint64_t dec_sum, dsp_sum, samples;
    uint32_t t_exit, cw_exit;
    uint8_t t_valid;
};
static struct cop_win win COP_SHARED_ATTR;

/* p083-copclock: the COP's clock demand (codec_place_core.h, "The COP's
 * clock demand"). The COP writes all of it per block (busy cycles, output
 * samples) and per period (the decision); the CPU sets out_rate at a
 * run's start and its tick reads gen (a change of want: a UI wake, the
 * UI thread takes or gives the boost: np_playback.c). Uncached, so no
 * cache operation is needed on either side (R7 item 1) */
static struct {
    struct cpl_demand d;
    uint32_t cyc;           /* MHz x us busy this period */
    uint32_t busy;          /* us busy this period */
    uint32_t out;           /* output samples this period */
    uint32_t out_rate;      /* the output rate (Hz), the CPU's */
    uint32_t gen;           /* bumped at each change of d.want */
} dm COP_SHARED_ATTR;
static uint32_t dm_seen;            /* the CPU tick's view of gen */
static void (*dm_wake)(void);       /* the UI's wake (the CPU tick calls) */
static bool dm_tick_on;

static bool dsp_enabled = true;     /* the developer A/B switch */
static bool dsp_off_said;           /* "COP FAILED (dsp off)" once a load */

void codec_cop_set_dsp(bool on)
{
    dsp_enabled = on;
}

bool codec_cop_dsp(void)
{
    return dsp_enabled;
}

void codec_cop_dsp_stats(uint32_t *frames, uint32_t *windows,
                         uint32_t *dsp_us)
{
    *frames = win.frames;
    *windows = win.windows;
    *dsp_us = win.dsp_us;
}

/* The DSP's cross-core lock (dsp_core.h). Whichever core changes a DSP's
 * state holds it: the COP around each dsp_process(), a CPU thread around
 * each setting, the codec thread's own configure() calls and a buflib
 * move of a stage's buffer. The cache hand-off rides on it: the holder
 * writes its cache back before letting go, and a core taking it drops
 * its own lines first if the other core let go since it last held it.
 * A binary semaphore, as cop_job's hand-offs (proven cross-core on
 * silicon); recursive for its holder. Docs: data-paths.md "DSP state
 * across the cores" */
#if NUM_CORES > 1 && !defined(SIMULATOR)
static struct semaphore dspx_sem SHAREDBSS_ATTR;
static volatile unsigned int dspx_owner SHAREDBSS_ATTR;
static volatile int dspx_depth SHAREDBSS_ATTR;
static volatile uint32_t dspx_gen[NUM_CORES] SHAREDBSS_ATTR;
static volatile uint32_t dspx_seen[NUM_CORES] SHAREDBSS_ATTR;
static volatile bool dspx_ready SHAREDBSS_ATTR;

static void dspx_init(void)
{
    if (!dspx_ready)
    {
        semaphore_init(&dspx_sem, 1, 1);
        dspx_ready = true;
    }
}

void dsp_xcore_lock(void)
{
    unsigned int self;
    unsigned int core;

    if (!dspx_ready)
        return;             /* boot: one thread, no COP run yet */
    self = thread_self();
    if (dspx_depth > 0 && dspx_owner == self)
    {
        dspx_depth++;
        return;
    }
    semaphore_wait(&dspx_sem, TIMEOUT_BLOCK);
    dspx_owner = self;
    dspx_depth = 1;
    core = CURRENT_CORE;
    if (dspx_gen[core ^ 1] != dspx_seen[core])
    {
        /* the other core held it since: its writes are in SDRAM (it
         * wrote back); drop any line of ours they made stale */
        dspx_seen[core] = dspx_gen[core ^ 1];
        commit_discard_dcache();
    }
}

void dsp_xcore_unlock(void)
{
    if (!dspx_ready || dspx_depth <= 0 || dspx_owner != thread_self())
        return;
    if (--dspx_depth > 0)
        return;
    commit_dcache();        /* ours to SDRAM before the other core reads */
    dspx_gen[CURRENT_CORE]++;
    dspx_owner = 0;
    semaphore_release(&dspx_sem);
}
#else
static void dspx_init(void)
{
}
#endif /* NUM_CORES > 1 && !SIMULATOR */

void codec_cop_set_enabled(bool on)
{
    enabled = on;
}

bool codec_cop_enabled(void)
{
    return enabled;
}

/* The COP's worker would not start this boot (it never ran the thread
 * within a second: an old bootloader, or a dead core). Each retry would
 * stall the track start for that second and leave one more thread queued
 * on the core, its stack in the codec slot: so the rest of the boot
 * decodes on the CPU, each load logged as "COP FAILED (down)" */
static bool cop_down;

bool codec_cop_available(void)
{
    return cop_jobs_cross_core() && !cop_down;
}

bool codec_cop_active(void)
{
    return on_cop;
}

void codec_cop_set_place(int mode)
{
    if (mode >= CPL_MODE_SCREEN && mode <= CPL_MODE_CPU)
        place_mode = mode;
}

int codec_cop_place(void)
{
    return place_mode;
}

/* The screen state: lit, dim (the backlight off, the panel awake) or dark
 * (the panel asleep too, after lcd_sleep()) */
static int screen_now(void)
{
    if (is_backlight_on(true))
        return CPL_LIT;
#ifdef HAVE_LCD_SLEEP
    if (lcd_active())
        return CPL_DIM;
#endif
    return CPL_DARK;
}

void codec_cop_observe(void)
{
    if (bound)                  /* unbound (the CPU arm): nothing to watch */
        cpl_observe(&place, screen_now(), current_tick);
}

void codec_cop_stats(uint32_t *r, uint32_t *calls, uint32_t *wait_ms)
{
    *r = runs;
    *calls = worker.calls;
    *wait_ms = worker.call_wait_us / 1000;
}

size_t codec_cop_reserve(void)
{
    /* 19-1c (i) 2: any run of this load may go to the COP */
    return bound ? COP_CODEC_STACK : 0;
}

/* The playback fields, CPU to the codec's view (CPU side) */
static void sync_fields(void)
{
    ci_cop.filesize  = real->filesize;
    ci_cop.curpos    = real->curpos;
    /* The COP reads id3 from memory (19-1c (i) 4: per run, not per
     * worker). Playback's CODEC_ID3 is already the uncached view for both
     * cores (#196, codec_id3.c), so this is the same pointer either way;
     * the alias stays for any other id3 handed in */
    ci_cop.id3       = on_cop ? cop_uncached(real->id3) : real->id3;
    ci_cop.audio_hid = real->audio_hid;
    ci_cop.dsp       = real->dsp;
}

#define FROM_COP() cop_job_in_worker(&worker)

/* 0.8.3 road-test fix: the COP's call wait split by kind, for this run
 * (the COP writes, the CPU resets at the run's start and reads at its
 * end). win: the window calls, of which full: the CPU slept on a full
 * pcmbuf (healthy); rd: the bitstream calls (read_filebuf,
 * request_buffer, advance_buffer); oth: the rest. lat_max: the longest
 * single wait that was not a full-buffer sleep (the CPU's latency in
 * serving the COP, plus the handler's own work), in us */
enum { CW_WIN = 0, CW_RD, CW_OTH, CW_KINDS };
struct cop_cw {
    uint32_t us[CW_KINDS];
    uint32_t n[CW_KINDS];
    uint32_t full_us;
    uint32_t lat_max;
};
static struct cop_cw cw COP_SHARED_ATTR;

static void cw_note(int kind, uint32_t wait, uint32_t full)
{
    cw.us[kind] += wait;
    cw.n[kind]++;
    cw.full_us += full;
    wait = wait > full ? wait - full : 0;
    if (wait > cw.lat_max)
        cw.lat_max = wait;
}

/* 0.8.3 colocate (owner, 2026-11-10: no synchronous COP->CPU call in
 * steady-state playback): the input side. The COP reads the compressed
 * ring itself, through its uncached alias, inside [pos, end) of a lease
 * (buffering.h struct buf_remote_reader): request_buffer, advance_buffer
 * and set_elapsed make no call while the bytes are there. The CPU's
 * buffering thread publishes end after each fill chunk (its cache written
 * back first) and applies the COP's pos when it next looks; the lease is
 * detached for every call that remains (the trampoline below), so a
 * handler always sees the handle where the COP left it, and re-attached
 * after it, so the COP sees whatever the handler did (a seek, a
 * rebuffer, a track's new handle). A request that is not buffered yet,
 * crosses the ring's wrap (the guard copy is the CPU's), or an advance
 * past end, a big skip or past the file falls back to one call, as do
 * seeks, track ends and commands. Rules: docs/data-paths.md R7 item 11 */
static struct buf_remote_reader rin COP_SHARED_ATTR;
static bool colocate = true;        /* the developer A/B switch */
static bool colocate_said;          /* "COP FAILED (colocate ...)" once a load */
/* this run reads the ring itself (the CPU sets it before the post and
 * clears it after the collect; the COP reads it) */
static volatile bool rin_on COP_SHARED_ATTR;
static uint32_t rin_pos0;           /* rin.pos at the attach (CPU's) */
/* counted by the COP, read by the CPU at the run's end */
struct cop_in_stats { uint32_t req, adv, el, rd, sk, fall; };
static struct cop_in_stats ins COP_SHARED_ATTR;

/* A big forward skip goes through the CPU, which pokes buffering's level
 * watch (buffering.c seek_handle) */
#define IN_BIG_SKIP     (64 * 1024)

void codec_cop_set_colocate(bool on)
{
    colocate = on;
}

bool codec_cop_colocate(void)
{
    return colocate;
}

/* CPU: the COP's reads onto the handle and the codec's curpos, then no
 * lease (before a handler, at the run's end) */
static void in_detach(void)
{
    if (!rin_on)
        return;
    if (buf_remote_detach(&rin) >= 0)
        real->curpos += (off_t)(rin.pos - rin_pos0);
    rin_pos0 = rin.pos;
}

/* CPU: a lease on the codec's handle as it is now (after a handler, at
 * the run's start) */
static void in_attach(void)
{
    if (!rin_on)
        return;
    if (buf_remote_attach(real->audio_hid, &rin))
        rin_pos0 = rin.pos;
}

/* The CPU's side of every call: detached around the handler */
static cop_call_fn xfn COP_SHARED_ATTR;     /* the one call in flight */

static intptr_t h_tramp(intptr_t *io)
{
    intptr_t r;

    in_detach();
    r = xfn(io);
    in_attach();
    return r;
}

#ifdef CODEC_COP_LEASE
/* COP: pcmbuf's write side back to the CPU before a call */
static void NO_INLINE lease_drop(void)
{
    pcmbuf_lease_release();
    win.leased = 0;
    win.backs++;
}
#endif

/* COP: the call, through the trampoline when the input lease is in use.
 * keep: the handler touches neither pcmbuf nor anything that can wait
 * (a request across the ring's wrap with its bytes buffered: the guard
 * copy; set_elapsed with A-B repeat: it posts at most), so pcmbuf's
 * write side may stay the COP's across it: a CPU thread reclaiming it
 * meanwhile waits only for this short call and the frame after it. Any
 * call that may wait on buffering (a request not yet buffered, an
 * advance into a rebuffer, read_filebuf) gives the lease back first, or
 * a skip or stop would wait in pcmbuf_lease_reclaim() for the fill
 * (review of d184744599, major 1). Inlined, as xcall() is, so
 * each handler's address sits in the function that calls cop_job_call()
 * (scripts/cop_shared_check.py ends a path there: the handler runs on
 * the CPU) */
static FORCE_INLINE intptr_t cop_call_k(cop_call_fn fn, intptr_t *io,
                                        unsigned flags, bool keep)
{
#ifdef CODEC_COP_LEASE
    /* no other call while the COP holds pcmbuf's write side: its handler
     * may need it, and the CPU would then wait on this core */
    if (win.leased && !keep)
        lease_drop();
#else
    (void)keep;
#endif
    if (!rin_on)
        return cop_job_call(&worker, fn, io, flags);
    xfn = fn;
    return cop_job_call(&worker, h_tramp, io, flags);
}

static FORCE_INLINE intptr_t cop_call(cop_call_fn fn, intptr_t *io,
                                      unsigned flags)
{
    return cop_call_k(fn, io, flags, false);
}

/* cop_job_call(), counted by kind (the window's own counts in win_call);
 * keep as cop_call_k()'s */
static FORCE_INLINE intptr_t xcall_k(int kind, cop_call_fn fn, intptr_t *io,
                                     unsigned flags, bool keep)
{
    uint32_t w0 = worker.call_wait_us;
    intptr_t r = cop_call_k(fn, io, flags, keep);

    if (FROM_COP())
        cw_note(kind, worker.call_wait_us - w0, 0);
    return r;
}

static FORCE_INLINE intptr_t xcall(int kind, cop_call_fn fn, intptr_t *io,
                                   unsigned flags)
{
    return xcall_k(kind, fn, io, flags, false);
}

/* COP: the lease is for the codec's handle now */
static inline bool in_ok(void)
{
    return rin_on && rin.live && rin.hid == ci_cop.audio_hid;
}

/* COP: request_buffer from the ring. IN_LOCAL: done here; IN_WAIT: one
 * call, which may wait for the fill; IN_WRAP: one call for the guard
 * copy, the bytes already buffered (it cannot wait) */
enum { IN_WAIT = 0, IN_LOCAL, IN_WRAP };
static int in_request(size_t reqsize, size_t *got, void **p)
{
    uint32_t pos, filesize, end, idx, len;
    size_t n = reqsize;

    if (!in_ok())
        return IN_WAIT;
    pos = rin.pos;
    filesize = rin.filesize;
    if (pos >= filesize)
    {
        *got = 0;                   /* the file is read: as bufgetdata */
        *p = NULL;
        return IN_LOCAL;
    }
    if (n == 0 || n > filesize - pos)
        n = filesize - pos;         /* clip to the end of the file */
    end = rin.end;                  /* once: the bytes below it are there */
    if (end < pos + n)
        return IN_WAIT;             /* not buffered yet: the CPU waits */
    len = rin.len;
    /* pos - map_pos < len while buffering keeps the handle whole (a fill
     * stops at the handle's own struct); the modulo holds whatever */
    idx = (rin.map_idx + (pos - rin.map_pos)) % len;
    if (idx + n > len)
        return IN_WRAP;             /* across the wrap: the guard copy */
    *p = cop_uncached(rin.base + idx);
    *got = n;
    ins.req++;
    return IN_LOCAL;
}

/* COP: advance_buffer inside the buffered bytes. False: one call */
static bool in_advance(size_t amount)
{
    uint32_t pos, np;

    if (!in_ok() || amount >= IN_BIG_SKIP)
        return false;
    pos = rin.pos;
    np = pos + amount;
    if (np > rin.filesize || np > rin.end)
        return false;               /* the CPU's error or rebuffer path */
    /* The codec read [pos, np) before it advanced: only now may the CPU
     * count those bytes as free */
    rin.pos = np;
    ci_cop.curpos += (off_t)amount;
    ci_cop.id3->offset = ci_cop.curpos;     /* audio_codec_update_offset */
    ins.adv++;
    return true;
}

/* COP: read_filebuf from the buffered bytes (#294: a container's header
 * read a few bytes at a time, an MP4's sample tables 4 bytes a read, an
 * ASF packet's fields 1 byte a read, was one call each: thousands before
 * an M4B's first sample, several a WMA packet). A copy
 * needs no guard, so the wrap is two copies. Then the position moves as
 * codec_filebuf_callback() moves it: curpos only, no offset. The codec's
 * buffer is its own cached memory, written here by the core that reads
 * it: no cache operation. False: one call */
static bool in_read(void *ptr, size_t size, size_t *got)
{
    uint32_t pos, filesize, end, n, idx, len, first;
    const char *ring;

    if (!in_ok() || size == 0 || size >= IN_BIG_SKIP)
        return false;
    pos = rin.pos;
    filesize = rin.filesize;
    if (pos >= filesize)
        return false;               /* the file is read: the CPU's answer */
    n = size;
    if (n > filesize - pos)
        n = filesize - pos;         /* as bufread: clipped to the file */
    end = rin.end;                  /* once: the bytes below it are there */
    if (end < pos + n)
        return false;               /* not buffered yet: the CPU waits */
    len = rin.len;
    idx = (rin.map_idx + (pos - rin.map_pos)) % len;
    ring = cop_uncached(rin.base);
    first = len - idx < n ? len - idx : n;
    memcpy(ptr, ring + idx, first);
    if (n > first)
        memcpy((char *)ptr + first, ring, n - first);
    /* only now may the CPU count those bytes as free */
    rin.pos = pos + n;
    ci_cop.curpos += (off_t)n;
    ins.rd++;
    *got = n;
    return true;
}

/* COP: seek_buffer forward inside the buffered bytes (#294: an MP4's
 * next audio chunk, past other streams' samples between them), as
 * codec_seek_buffer_callback() moves it: curpos only. Back, or as far as
 * a big skip (buffering's level watch), stays a call */
static bool in_seek(size_t newpos)
{
    uint32_t pos;

    if (!in_ok())
        return false;
    pos = rin.pos;
    if (newpos < pos || newpos - pos >= IN_BIG_SKIP)
        return false;
    if (newpos > rin.filesize || newpos > rin.end)
        return false;               /* the CPU's rebuffer path */
    rin.pos = (uint32_t)newpos;
    ci_cop.curpos = (off_t)newpos;
    ins.sk++;
    return true;
}

/** The callbacks: on the COP, carried to the codec thread **/

static intptr_t h_get_buffer(intptr_t *io)
{
    size_t size = (size_t)io[0];
    void *p = real->codec_get_buffer(&size);

    io[0] = (intptr_t)size;
    return (intptr_t)p;         /* the codec's own memory: stays cached */
}

static void *t_get_buffer(size_t *size)
{
    intptr_t io[COP_CALL_IO] = { (intptr_t)*size };
    void *p;

    if (!FROM_COP())
        return real->codec_get_buffer(size);
    p = (void *)xcall(CW_OTH, h_get_buffer, io, 0);
    *size = (size_t)io[0];
    return p;
}

static intptr_t h_pcmbuf_insert(intptr_t *io)
{
    /* The COP wrote its cache back before the call; read the samples
     * from memory, not from stale lines of an earlier frame */
    real->pcmbuf_insert(cop_uncached((const void *)io[0]),
                        cop_uncached((const void *)io[1]), (int)io[2]);
    sync_fields();
    return 0;
}

/* #282 step 4: the window calls (CPU side). io: used, elapsed, offset,
 * want; back: count in io[0], hold in io[1], the window as the value */
/* road test (0.8.2): the CPU clock seen at each window call this run
 * (the CPU's; the COP runs from the same PLL) */
static long ck_min, ck_max;

static void ck_sample(void)
{
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    long f = FREQ;

    if (ck_min == 0 || f < ck_min)
        ck_min = f;
    if (f > ck_max)
        ck_max = f;
#endif
}

static intptr_t h_window(intptr_t *io)
{
    int n;
    bool hold, leased;
    uint32_t f0 = codec_pcm_window_full_us();

    ck_sample();
    void *p = codec_pcm_window((int)io[0], (unsigned long)io[1],
                               (off_t)io[2], (int)io[3], win.may_lease,
                               &n, &hold, &leased);

    io[0] = n;
    io[1] = hold;
    io[2] = (intptr_t)(codec_pcm_window_full_us() - f0);  /* 0.8.3 */
    io[3] = leased;
    sync_fields();
    return (intptr_t)p;
}

/* COP: publish the window (want 0) or publish and take the next. The CPU
 * may write pcmbuf in the call (the wrap copy, a crossfade mix): the COP
 * drops its lines after (COPJ_CALL_WROTE). False: a message waits */
static bool win_call(int want)
{
    intptr_t io[COP_CALL_IO] = { win.dst.remcount, (intptr_t)win.elapsed,
                                 (intptr_t)win.offset, want };
    void *p;

    if (want <= 0 && win.dst.remcount <= 0)
    {
        win.dst.bufcount = 0;   /* nothing written: just let it go */
        return true;
    }
    {
        uint32_t w0 = worker.call_wait_us;

        p = (void *)cop_call(h_window, io, COPJ_CALL_WROTE);
        cw_note(CW_WIN, worker.call_wait_us - w0, (uint32_t)io[2]);
    }
    win.windows++;
    win.dst.remcount = 0;
    win.dst.p16out = p;
    win.dst.bufcount = p ? (int)io[0] : 0;
    win.hold = p ? (uint8_t)io[1] : 0;
    if (io[3])
    {
        win.leased = 1;     /* 0.8.3: no window: the write side itself */
        return true;
    }
    return p != NULL;
}

#ifdef CODEC_COP_LEASE
/* COP, under the lease: the codec thread's own loop, here (request,
 * process, commit, frame by frame) with its full-buffer sleep in slices,
 * so a message, a revoke or a stopped channel is seen within one. True:
 * the input is used up. False: the lease went back (a message, a revoke,
 * a stopped channel, a level under the watermark, or a full buffer that
 * played nothing for a second: a pause) and the window calls go on */
static bool lease_frames(struct dsp_buffer *src, uint32_t *dsp,
                         uint32_t *out)
{
    size_t ridx = pcmbuf_lease_ridx();
    long still = 0;             /* ticks asleep with nothing played */

    while (codec_queue_idle())
    {
        int n = MAX(src->remcount, 1024), why;
        struct dsp_buffer dst;
        void *p = pcmbuf_lease_window(&n, &why);
        uint32_t t0;

        if (p == NULL)
        {
            long t;

            if (why != PCMBUF_LEASE_FULL)
                break;
            if (pcmbuf_lease_ridx() != ridx)
            {
                ridx = pcmbuf_lease_ridx();
                still = 0;
            }
            else if (still >= HZ)
                break;          /* paused: the CPU sleeps on its queue */
            t = pcmbuf_lease_full_ticks();
            still += t;
            t0 = DSP_NOW_US();
            sleep(t);
            win.lsleep_us += DSP_NOW_US() - t0;
            continue;
        }
        dst.remcount = 0;
        dst.bufcount = n;
        dst.p16out = p;
        t0 = DSP_NOW_US();
        dsp_xcore_lock();
        dsp_process(ci_cop.dsp, src, &dst, true);
        dsp_xcore_unlock();
        t0 = DSP_NOW_US() - t0;
        win.dsp_us += t0;
        *dsp += t0;
        win.frames++;
        if (dst.remcount > 0)
        {
            *out += (uint32_t)dst.remcount;     /* p083-copclock */
            pcmbuf_lease_commit(dst.remcount, ci_cop.id3->elapsed,
                                ci_cop.id3->offset);
        }
        else if (src->remcount <= 0)
            return true;        /* no input left and the DSP purged */
    }
    pcmbuf_lease_release();
    win.leased = 0;
    win.backs++;
    return false;
}
#endif

/* p083-copclock, COP: one block's busy time and output into the period;
 * a period of 1 s of output or more decides */
static void dm_block(uint32_t busy_us, uint32_t out)
{
#if NUM_CORES > 1 && !defined(SIMULATOR)
    uint32_t rate = dm.out_rate, mhz = 30, us;

#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    mhz = (uint32_t)cpu_frequency / 1000000u;   /* uncached (SHAREDBSS) */
#endif
    if (rate == 0)
        return;
    if (busy_us > 1000000u)
        busy_us = 1000000u;     /* one block: a second at most (a stall) */
    dm.cyc += busy_us * mhz;
    dm.busy += busy_us;
    dm.out += out;
    if (dm.out < rate && dm.cyc < 0xC0000000u)
        return;
    /* ms resolution: dm.out stays far under 2^32 / 1000 samples */
    us = dm.out / rate * 1000000u + dm.out % rate * 1000u / rate * 1000u;
    if (cpl_demand_period(&dm.d, dm.cyc, dm.busy, us))
        dm.gen++;               /* last: the CPU's tick reads it */
    dm.cyc = 0;
    dm.busy = 0;
    dm.out = 0;
#else
    /* no clock to time a block by (DSP_NOW_US() is 0): the simulator's
     * demand comes from its "cop_demand" command */
    (void)busy_us;
    (void)out;
#endif
}

/* p083-copclock, the CPU's tick: a change of the COP's demand wakes the
 * UI thread, which takes or gives the boost. One uncached load a tick */
static void dm_tick(void)
{
    uint32_t g = dm.gen;

    if (g != dm_seen)
    {
        dm_seen = g;
        if (dm_wake)
            dm_wake();
    }
}

bool codec_cop_demand(unsigned int *ema_x10, unsigned int *last_x10)
{
    if (ema_x10)
        *ema_x10 = dm.d.ema_x10;
    if (last_x10)
        *last_x10 = dm.d.last_x10;
    return dm.d.want;
}

void codec_cop_set_demand_wake(void (*wake)(void))
{
    dm_wake = wake;
    if (!dm_tick_on)
    {
        dm_tick_on = true;      /* once a boot, from a CPU thread */
        tick_add_task(dm_tick);
    }
}

#ifdef SIMULATOR
/* sim command "cop_demand <tenths of a MHz>": one period of that demand,
 * as if the COP measured it (the simulator times nothing) */
void codec_cop_sim_demand(unsigned int x10)
{
    /* busy as at 30 MHz */
    if (cpl_demand_period(&dm.d, x10 * 100000u, x10 * 100000u / 30u,
                          1000000u))
        dm.gen++;
}
#endif

/* COP: before a call that can let playback reset the write side */
static void win_publish(void)
{
    if (win.on)
        win_call(0);
}

/* The codec thread's pcmbuf_insert loop (codec_thread.c), run on the COP:
 * the same order, request (here: a window), process, publish */
static void cop_pcmbuf_insert(const void *ch1, const void *ch2, int count)
{
    struct dsp_buffer src;
    uint32_t t_in = DSP_NOW_US();
    uint32_t cw_in = worker.call_wait_us + win.lsleep_us;
    uint32_t dec = 0, dsp = 0;
    uint32_t out = 0;               /* p083-copclock: output samples */

    /* road test: the decode time since the last block (CPU calls out) */
    if (win.t_valid)
    {
        dec = (t_in - win.t_exit) - (cw_in - win.cw_exit);
        if (dec < win.dec_min)
            win.dec_min = dec;
        if (dec > win.dec_max)
            win.dec_max = dec;
        win.dec_sum += dec;
    }

    src.remcount  = count;
    src.pin[0]    = ch1;
    src.pin[1]    = ch2;
    src.proc_mask = 0;

    while (1)
    {
        int before;
        uint32_t t0;

#ifdef CODEC_COP_LEASE
        if (win.leased && lease_frames(&src, &dsp, &out))
            break;              /* all of it, with no call */
#endif
        /* No room left, or a message for the codec: the CPU publishes and
         * answers it (the original checked its queue at every turn) */
        if (win.dst.bufcount <= 0 || !codec_queue_idle())
        {
            if (!win_call(MAX(src.remcount, 1024)))
            {
                win.t_valid = 0;    /* a message: the next block starts over */
                return;
            }
        }
        if (win.dst.remcount == 0)
        {
            win.elapsed = ci_cop.id3->elapsed;
            win.offset = ci_cop.id3->offset;
        }
        before = win.dst.remcount;
        t0 = DSP_NOW_US();
        dsp_xcore_lock();
        dsp_process(ci_cop.dsp, &src, &win.dst, true);
        dsp_xcore_unlock();
        t0 = DSP_NOW_US() - t0;
        win.dsp_us += t0;
        dsp += t0;
        win.frames++;
        out += (uint32_t)(win.dst.remcount - before);
        if (win.dst.remcount == before && src.remcount <= 0)
            break;              /* no input left and the DSP purged */
    }
    /* Publish: past the mark (and take the next window in the same call),
     * or after every frame while a crossfade mixes (publish only, so the
     * crossfade's window is taken right before it is written, as before) */
    if (win.leased)
        ;                       /* 0.8.3: committed frame by frame */
    else if (!win.hold)
        win_call(0);
    else if (win.dst.remcount >= WIN_PUBLISH)
        win_call(1024);

    /* road test: this block's numbers */
    win.blk_n++;
    win.samples += (uint32_t)count;
    if ((uint32_t)count > win.blk_max)
        win.blk_max = count;
    if (dsp < win.dsp_min)
        win.dsp_min = dsp;
    if (dsp > win.dsp_max)
        win.dsp_max = dsp;
    win.dsp_sum += dsp;
    if (dec + dsp > win.worst)
        win.worst = dec + dsp;
    dm_block(dec + dsp, out);       /* p083-copclock */
    win.t_exit = DSP_NOW_US();
    win.cw_exit = worker.call_wait_us + win.lsleep_us;
    win.t_valid = 1;
}

static void t_pcmbuf_insert(const void *ch1, const void *ch2, int count)
{
    intptr_t io[COP_CALL_IO] = { (intptr_t)ch1, (intptr_t)ch2, count };

    if (!FROM_COP())
    {
        real->pcmbuf_insert(ch1, ch2, count);
        sync_fields();
    }
    else if (win.on)
        cop_pcmbuf_insert(ch1, ch2, count);
    else
        xcall(CW_OTH, h_pcmbuf_insert, io, 0);
}

static intptr_t h_set_elapsed(intptr_t *io)
{
    real->set_elapsed((unsigned long)io[0]);
    return 0;
}

static void t_set_elapsed(unsigned long value)
{
    intptr_t io[COP_CALL_IO] = { (intptr_t)value };

    if (!FROM_COP())
        real->set_elapsed(value);
#ifndef AB_REPEAT_ENABLE
    else if (rin_on)
    {
        /* 0.8.3 colocate: all audio_codec_update_elapsed() does without
         * A-B repeat; the codec's id3 is uncached for both cores (#196) */
        ci_cop.id3->elapsed = value;
        ins.el++;
    }
#endif
    else
    {
        /* with A-B repeat (developer builds) a call each frame: it posts
         * at most (ab_jump_to_A_marker() -> audio_ff_rewind()) and never
         * touches pcmbuf, so the write side's lease stays */
        xcall_k(CW_OTH, h_set_elapsed, io, 0, true);
    }
}

static intptr_t h_read_filebuf(intptr_t *io)
{
    size_t n = real->read_filebuf(cop_uncached((void *)io[0]),
                                  (size_t)io[1]);
    sync_fields();
    return (intptr_t)n;
}

static size_t t_read_filebuf(void *ptr, size_t size)
{
    intptr_t io[COP_CALL_IO] = { (intptr_t)ptr, (intptr_t)size };
    size_t got;

    if (!FROM_COP())
    {
        size_t n = real->read_filebuf(ptr, size);
        sync_fields();          /* 19-1c (i) 3: curpos moved */
        return n;
    }
    if (in_read(ptr, size, &got))
        return got;
    if (rin_on)
        ins.fall++;
    /* The CPU wrote the codec's buffer through the uncached alias: the
     * COP drops its lines after */
    return (size_t)xcall(CW_RD, h_read_filebuf, io,
                                COPJ_CALL_WROTE);
}

static intptr_t h_request_buffer(intptr_t *io)
{
    size_t realsize = 0;
    void *p = real->request_buffer(&realsize, (size_t)io[0]);

    /* The ring is DMA'd or copied in by the CPU (the guard copy across
     * the wrap): write the CPU's lines back, then hand the COP the
     * uncached alias */
    commit_dcache();
    io[0] = (intptr_t)realsize;
    sync_fields();
    return (intptr_t)cop_uncached(p);
}

static void *t_request_buffer(size_t *realsize, size_t reqsize)
{
    intptr_t io[COP_CALL_IO] = { (intptr_t)reqsize };
    void *p;

    if (!FROM_COP())
    {
        p = real->request_buffer(realsize, reqsize);
        sync_fields();
        return p;
    }
    int how = in_request(reqsize, realsize, &p);

    if (how == IN_LOCAL)
        return p;
    if (rin_on)
        ins.fall++;
    /* keep pcmbuf's lease only for the guard copy, which cannot wait */
    p = (void *)xcall_k(CW_RD, h_request_buffer, io, 0, how == IN_WRAP);
    *realsize = (size_t)io[0];
    return p;
}

static intptr_t h_advance_buffer(intptr_t *io)
{
    real->advance_buffer((size_t)io[0]);
    sync_fields();
    return 0;
}

static void t_advance_buffer(size_t amount)
{
    intptr_t io[COP_CALL_IO] = { (intptr_t)amount };

    if (!FROM_COP())
    {
        real->advance_buffer(amount);
        sync_fields();          /* 19-1c (i) 3: was left stale */
    }
    else if (!in_advance(amount))
    {
        if (rin_on)
            ins.fall++;
        xcall(CW_RD, h_advance_buffer, io, 0);
    }
}

static intptr_t h_seek_buffer(intptr_t *io)
{
    bool ok = real->seek_buffer((size_t)io[0]);

    sync_fields();
    return ok;
}

static bool t_seek_buffer(size_t newpos)
{
    intptr_t io[COP_CALL_IO] = { (intptr_t)newpos };

    if (!FROM_COP())
    {
        bool ok = real->seek_buffer(newpos);
        sync_fields();          /* 19-1c (i) 3: was left stale */
        return ok;
    }
    /* a short forward seek in the buffered bytes touches neither pcmbuf
     * nor playback: no call, so the window and the lease stay */
    if (in_seek(newpos))
        return true;
    win_publish();
    return xcall(CW_OTH, h_seek_buffer, io, 0) != 0;
}

static intptr_t h_seek_complete(intptr_t *io)
{
    (void)io;
    real->seek_complete();
    sync_fields();
    return 0;
}

static void t_seek_complete(void)
{
    intptr_t io[COP_CALL_IO] = { 0 };

    if (!FROM_COP())
    {
        real->seek_complete();
        sync_fields();
    }
    else
    {
        win_publish();
        xcall(CW_OTH, h_seek_complete, io, 0);
    }
}

static intptr_t h_set_offset(intptr_t *io)
{
    real->set_offset((size_t)io[0]);
    return 0;
}

static void t_set_offset(size_t value)
{
    intptr_t io[COP_CALL_IO] = { (intptr_t)value };

    if (!FROM_COP())
        real->set_offset(value);
    else
        xcall(CW_OTH, h_set_offset, io, 0);
}

/* The settings whose value points at the caller's memory (the COP
 * worker's stack): every other one a decoder passes is a value.
 * tests/codecid3/run.sh fails on a setting the codecs pass that is in
 * neither list */
static const struct cop_cfg_ptr cfg_ptrs[] = {
    { REPLAYGAIN_SET_GAINS, sizeof(struct dsp_replay_gains) },
};
_Static_assert(sizeof(struct dsp_replay_gains) <= COP_CFG_COPY_MAX,
               "a pointer setting's pointee must fit the CPU-side copy");

static intptr_t h_configure(intptr_t *io)
{
    /* A pointer setting is read through its uncached alias into a copy
     * here: the CPU's lines of the worker's stack can be stale
     * (codec_cop_cfg.h; the laptop's cache model, 2026-11-08) */
    long copy[COP_CFG_COPY_MAX / sizeof(long)];
    int setting = (int)io[0];
    size_t size = cop_cfg_ptr_size(cfg_ptrs, ARRAYLEN(cfg_ptrs), setting);

    real->configure(setting, cop_cfg_arg(size, io[1], copy,
                                         cop_uncached((const void *)io[1])));
    return 0;
}

static void t_configure(int setting, intptr_t value)
{
    intptr_t io[COP_CALL_IO] = { setting, value };

    if (!FROM_COP())
        real->configure(setting, value);
    else
        xcall(CW_OTH, h_configure, io, 0);
}

static intptr_t h_get_command(intptr_t *io)
{
    intptr_t param = io[0];
    long action = real->get_command(&param);

    io[0] = param;
    sync_fields();
    return action;
}

static long t_get_command(intptr_t *param)
{
    intptr_t io[COP_CALL_IO] = { 0 };
    long action;

    if (!FROM_COP())
    {
        action = real->get_command(param);
        sync_fields();
        return action;
    }
    /* Once a frame: answer "nothing" here while the codec's queue is
     * empty (it lives in uncached memory), as the CPU side would */
    yield();
    if (codec_queue_idle())
        return CODEC_ACTION_NULL;
    win_publish();          /* #282: before playback may reset pcmbuf */
    if (param)
        io[0] = *param;
    action = xcall(CW_OTH, h_get_command, io, 0);
    if (param)
        *param = io[0];
    return action;
}

static intptr_t h_loop_track(intptr_t *io)
{
    (void)io;
    return real->loop_track();
}

static bool t_loop_track(void)
{
    intptr_t io[COP_CALL_IO] = { 0 };

    if (!FROM_COP())
        return real->loop_track();
    return xcall(CW_OTH, h_loop_track, io, 0) != 0;
}

static intptr_t h_strip_filesize(intptr_t *io)
{
    int64_t v = (int64_t)(((uint64_t)(uint32_t)io[1] << 32)
                          | (uint32_t)io[0]);

    real->strip_filesize((off_t)v);
    sync_fields();
    return 0;
}

static void t_strip_filesize(off_t value)
{
    int64_t v = value;
    intptr_t io[COP_CALL_IO] = { (intptr_t)(uint32_t)v,
                                 (intptr_t)(uint32_t)((uint64_t)v >> 32) };

    if (!FROM_COP())
    {
        real->strip_filesize(value);
        sync_fields();
    }
    else
        xcall(CW_OTH, h_strip_filesize, io, 0);
}

/* panicf and logf: format on the COP into the shared line, then show or
 * log it from the codec thread */
static intptr_t h_panic(intptr_t *io)
{
    (void)io;
    panicf("%s", msg);
    return 0;
}

static void t_panicf(const char *fmt, ...)
{
    intptr_t io[COP_CALL_IO] = { 0 };
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (!FROM_COP())
        panicf("%s", msg);
    else
        xcall(CW_OTH, h_panic, io, 0);
}

#ifdef ROCKBOX_HAS_LOGF
static intptr_t h_logf(intptr_t *io)
{
    (void)io;
    logf("%s", msg);
    return 0;
}

static void t_logf(const char *fmt, ...)
{
    intptr_t io[COP_CALL_IO] = { 0 };
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (!FROM_COP())
        logf("%s", msg);
    else
        xcall(CW_OTH, h_logf, io, 0);
}
#endif

/* #281: ui_log() drops a COP caller: the line is made here and logged on
 * the CPU. io[0]: the tag (a string in the codec's image) */
static intptr_t h_ui_log(intptr_t *io)
{
    ui_log((const char *)io[0], "%s", msg);
    return 0;
}

static void t_ui_log(const char *tag, const char *fmt, ...)
{
    intptr_t io[COP_CALL_IO] = { 0 };
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (!FROM_COP())
        ui_log(tag, "%s", msg);
    else
    {
        io[0] = (intptr_t)tag;
        xcall(CW_OTH, h_ui_log, io, 0);
    }
}

/** codec_thread.c's side **/

static bool fresh;                  /* no run since the load */
static bool fresh_log;              /* no place line since the load */

struct codec_api *codec_cop_select(struct codec_api *api, bool encoder)
{
    dspx_init();
    dsp_off_said = false;
    colocate_said = false;
    real = api;
    fresh = true;
    fresh_log = true;
    on_cop = false;
    /* 19-1c (i) 1: bound whenever the COP could take a run, apart from
     * which core the first run takes */
    bound = enabled && !encoder && codec_cop_available()
            && place_mode != CPL_MODE_CPU;
    if (cop_down && enabled && !encoder && place_mode != CPL_MODE_CPU)
    {
        /* meant for the COP: never silently on the CPU */
        ui_log("codec", "COP FAILED (down): decoding on the CPU");
        logf("codec cop: COP FAILED (down), decoding on the CPU");
    }
#ifdef HAVE_BCMFW_VIDEO
    /* A video's soundtrack stays on the CPU in 0.8.3: with the switch on,
     * session 36's stage 7 saw it stand still under the picture (the bcm
     * session, 2026-10-15; cause open, fit2-brief.md). Moving it is #283,
     * 0.9.0. One likely cause, an MP4's sample tables read 4 bytes a call,
     * is served from the ring now (in_read(), in_seek()) for every
     * MP4/M4A/M4B track */
    if (bound && vplayer_running())
    {
        logf("codec cop: a video plays, decoding on the CPU");
        bound = false;
    }
#endif
    if (!bound)
        return api;

    /* Kernel, cache, string and memory calls run on the COP as they are
     * (core-safe or pure); the playback callbacks go back to the CPU */
    ci_cop = *api;
    ci_cop.codec_get_buffer = t_get_buffer;
    ci_cop.pcmbuf_insert    = t_pcmbuf_insert;
    ci_cop.set_elapsed      = t_set_elapsed;
    ci_cop.read_filebuf     = t_read_filebuf;
    ci_cop.request_buffer   = t_request_buffer;
    ci_cop.advance_buffer   = t_advance_buffer;
    ci_cop.seek_buffer      = t_seek_buffer;
    ci_cop.seek_complete    = t_seek_complete;
    ci_cop.set_offset       = t_set_offset;
    ci_cop.configure        = t_configure;
    ci_cop.get_command      = t_get_command;
    ci_cop.loop_track       = t_loop_track;
    ci_cop.strip_filesize   = t_strip_filesize;
    ci_cop.panicf           = t_panicf;
    ci_cop.ui_log           = t_ui_log;
#ifdef ROCKBOX_HAS_LOGF
    ci_cop.logf             = t_logf;
#endif
    sync_fields();
    /* The codec's entry points (load, unload) run on the CPU through the
     * same struct and call straight through */
    return &ci_cop;
}

/* 19-1c: the core for this run (the first after a load, a track
 * boundary, or a seek's re-run, which keeps the core it had), and the
 * codec's switch for its own helper work on the COP (iii). Unbound (the
 * CPU arm, a video, or no COP): the plain path, synthesis inline */
static bool last_cop;               /* the core the last run took */

static void choose(bool track_start)
{
    int core;
    bool assist = false;

    if (!bound)
    {
        fresh = false;
        on_cop = false;
        real->cop_assist = false;
        return;
    }
    codec_cop_observe();
    if (fresh)
        core = cpl_at_load(&place, place_mode, current_tick);
    else if (track_start)
        core = cpl_at_boundary(&place, place_mode, current_tick,
                               CPL_HOLD_S * HZ);
    else
        core = place.core;
    fresh = false;
    on_cop = core == CPL_COP;
    assist = cpl_cop_assist(core);
    real->cop_assist = assist;
    ci_cop.cop_assist = assist;
}

/* After the run's core is settled (a worker that cannot start leaves it
 * on the CPU): log a load or a move, with the core it really took */
static void log_place(void)
{
    static const char *const name[] = { "cpu", "cop" };

    if (fresh_log || on_cop != last_cop)
    {
        long held = place.seen ? (long)(current_tick - place.since) / HZ
                               : -1;
        /* one 64 B line: "place cpu<-cop load=0 scr=2 held=117 m=1" */
        ui_log("codec", "place %s<-%s load=%d scr=%d held=%ld m=%d",
               name[on_cop], name[last_cop], fresh_log ? 1 : 0,
               place.screen, held, place_mode);
        /* 4gh-b5: the heap walked at each place or move between cores */
        heapchk_at(on_cop ? "place cop" : "place cpu");
    }
    fresh_log = false;
    last_cop = on_cop;
}

static void run_job(void *arg)
{
    (void)arg;
    run_status = codec_run_proc();
#ifdef CODEC_COP_LEASE
    if (win.leased)
    {
        pcmbuf_lease_release();     /* the CPU reclaims after the collect */
        win.leased = 0;
        win.backs++;
    }
#endif
}

/* The COP would not take a run that was meant for it: the CPU runs it,
 * and says so loudly (never silently, owner 2026-11-07). MP3's synthesis
 * then stays inline: its thread would wait on a COP that is not there */
static void cop_refused(const char *why)
{
    ui_log("codec", "COP FAILED (%s): decoding on the CPU", why);
    logf("codec cop: COP FAILED (%s), decoding on the CPU", why);
    real->cop_assist = false;
    ci_cop.cop_assist = false;
}

/* Runs on the CPU, through the struct the codec was given */
static int run_here(void)
{
    on_cop = false;
    cpu_runs++;
    /* p083-copclock: no COP measure for a CPU run: no COP demand */
    if (dm.d.want)
    {
        cpl_demand_init(&dm.d);
        dm.gen++;
    }
    if (bound)
    {
        sync_fields();          /* the cached id3, the fields run_codec set */
        log_place();
    }
#ifdef HAVE_IPOD_UI
    audio_start_trace_step(STS_POST);   /* p083-codecload: on the CPU */
#endif
    return codec_run_proc();
}

/* road test (0.8.2): one line a track (a run that starts one): the
 * source's format, depth and rate, and the output's depth and rate as
 * set for it (playback set them before the codec started), and where the
 * decode runs. "trk FLAC 24/192000>24/96000 cop" */
static void log_track(bool track_start, bool cop)
{
    /* through the uncached alias: the COP may have written it */
    const struct mp3entry *id3 = real && real->id3 ?
        cop_uncached(real->id3) : NULL;

    if (!track_start || !id3)
        return;
    ui_log("codec", "trk %s %u/%lu>%d/%u %s",
           get_codec_string(id3->codectype),
           (unsigned)id3->bits_per_sample, id3->frequency,
#ifdef HAVE_PCM_24BIT_OUT
           mixer_get_depth(),
#else
           16,
#endif
           mixer_get_frequency(),
           cop ? (dsp_enabled ? "cop" : "cop/dsp-cpu") : "cpu");
}

/* road test: the run's per-block numbers (COP runs with the DSP there).
 * "us dec 2100/2950/9800 dsp 900/1200/2400" (min/avg/max a block), then
 * "blk n=520 4096smp=21333us worst=12400us mhz=30-80": the largest block,
 * its time at the source rate (the budget a block must beat), the worst
 * decode+DSP block and the CPU clock range seen */
static void log_blocks(void)
{
    const struct mp3entry *id3 = real && real->id3 ?
        cop_uncached(real->id3) : NULL;
    unsigned long n = win.blk_n, budget = 0;

    if (n == 0)
        return;
    if (id3 && id3->frequency)
        budget = (unsigned long)((uint64_t)win.blk_max * 1000000u /
                                 id3->frequency);
    ui_log("codec", "us dec %lu/%lu/%lu dsp %lu/%lu/%lu",
           n > 1 ? (unsigned long)win.dec_min : 0UL,
           n > 1 ? (unsigned long)(win.dec_sum / (n - 1)) : 0UL,
           (unsigned long)win.dec_max,
           (unsigned long)win.dsp_min,
           (unsigned long)(win.dsp_sum / n),
           (unsigned long)win.dsp_max);
    ui_log("codec", "blk n=%lu %lusmp=%luus worst=%luus mhz=%ld-%ld",
           n, (unsigned long)win.blk_max, budget,
           (unsigned long)win.worst, ck_min / 1000000, ck_max / 1000000);
}

/* 0.8.3 road-test fix: what tells the click's causes apart on the device,
 * two lines a run (a track, or what is left of it after a seek or a
 * setting that restarts the decode):
 * "cw win=46290 full=40100 rd=5800 oth=90 lat=12500": the COP's call
 *   wait in ms by kind (full: the CPU slept on a full pcmbuf, healthy),
 *   and the longest single wait that was no full-buffer sleep, in us;
 * "fiq e=0 f=3 gap=1450 m=60/0 lvl=2810": FIQs that found the I2S FIFO
 *   empty (each a click) and the most free FIFO words seen (16: empty);
 *   the longest time between two FIQs (us); the longest stretch the
 *   COP's cache drop held the FIQ off (us) and how many ran over 80 us;
 *   the lowest pcmbuf level (ms) the FIQ saw (-1: none) */
/* pcmbuf.c (pcmbuf.h is not included here: the host model compiles this
 * file without the firmware's headers) */
long pcmbuf_level_min_take(void);
unsigned long pcmbuf_mid_underruns(void);
static unsigned long diag_und0;     /* p083-copclock: at the run's start */
#ifdef HAVE_CORE_LAYOUT_A
static struct pcm_pp_stats diag_pp0;
#endif

static void run_diag_start(void)
{
    memset(&cw, 0, sizeof(cw));
    (void)pcmbuf_level_min_take();
    diag_und0 = pcmbuf_mid_underruns();
#ifdef HAVE_CORE_LAYOUT_A
    {
        uint32_t a, b, c;

        cache_discard_take_stats(COP, &a, &b, &c);
        pcm_pp_get_stats(&diag_pp0);
        pcm_pp_reset_max();
    }
#endif
}

static void run_diag_log(void)
{
    ui_log("codec", "cw win=%lu full=%lu rd=%lu oth=%lu lat=%lu",
           (unsigned long)cw.us[CW_WIN] / 1000,
           (unsigned long)cw.full_us / 1000,
           (unsigned long)cw.us[CW_RD] / 1000,
           (unsigned long)cw.us[CW_OTH] / 1000,
           (unsigned long)cw.lat_max);
#ifdef HAVE_CORE_LAYOUT_A
    {
        struct pcm_pp_stats s;
        uint32_t mmax, mover, mn;

        pcm_pp_get_stats(&s);
        cache_discard_take_stats(COP, &mmax, &mover, &mn);
        (void)mn;
        /* p083-copclock: und= the run's mid-track underruns (their own
         * lines are rate-limited), dm= the COP's demand (MHz x 10) */
        ui_log("codec", "fiq e=%lu f=%lu gap=%lu m=%lu/%lu lvl=%ld und=%lu"
               " dm=%u", (unsigned long)(s.fifo_empty - diag_pp0.fifo_empty),
               (unsigned long)s.fifo_free_max,
               (unsigned long)s.gap_max_us,
               (unsigned long)mmax, (unsigned long)mover,
               pcmbuf_level_min_take(),
               pcmbuf_mid_underruns() - diag_und0, dm.d.ema_x10);
        /* 0.8.3 fifo96: late or long?  q= the two-channel queue on (1);
         * late= the latest FIQ past its piece's end (us) and how many
         * came later than the FIFO lasts; pc=/psr= where the core was for
         * the latest, opc=/opsr= for the last over the FIFO (symbolise
         * with addr2line on this build's rockbox.elf); rst= entry to the
         * next piece armed, hnd= entry to exit (the mix), lk= the longest
         * PCM lock (us); dry= queue completions that found the queued
         * piece done too (each a gap), odd= the queued one done first
         * (never, on a proved engine), slack= the least time left when a
         * FIQ ended (us) */
        ui_log("codec", "fiq2 q=%u late=%lu/%lu pc=%08lx psr=%02lx "
               "opc=%08lx opsr=%02lx rst=%lu hnd=%lu lk=%lu dry=%lu odd=%lu "
               "fell=%lu slack=%ld", pcm_pp_queue_on() ? 1u : 0u,
               (unsigned long)s.late_max_us,
               (unsigned long)(s.late_over - diag_pp0.late_over),
               (unsigned long)s.late_pc, (unsigned long)(s.late_psr & 0xff),
               (unsigned long)s.over_pc, (unsigned long)(s.over_psr & 0xff),
               (unsigned long)s.restart_max_us,
               (unsigned long)s.handler_max_us,
               (unsigned long)s.lock_hold_max_us,
               (unsigned long)(s.q_dry - diag_pp0.q_dry),
               (unsigned long)(s.q_odd - diag_pp0.q_odd),
               (unsigned long)(s.q_fell - diag_pp0.q_fell),
               s.q_slack_min_us == ~0u ? -1L : (long)s.q_slack_min_us);
        /* fifo96 review: the queue given up in this run, said loudly */
        if (s.q_fell != diag_pp0.q_fell)
            ui_log("codec", "pcm queue: FAILED in use (dry=%lu odd=%lu): "
                   "one channel for this boot",
                   (unsigned long)(s.q_dry - diag_pp0.q_dry),
                   (unsigned long)(s.q_odd - diag_pp0.q_odd));
    }
#else
    ui_log("codec", "fiq lvl=%ld und=%lu dm=%u", pcmbuf_level_min_take(),
           pcmbuf_mid_underruns() - diag_und0, dm.d.ema_x10);
#endif
}

int codec_cop_run(bool track_start)
{
    int t;

    choose(track_start);
    log_track(track_start, on_cop);
    if (!on_cop)
        return run_here();

    if (!worker.started
        && !cop_worker_start(&worker, codec_slot_top(COP_CODEC_STACK),
                             COP_CODEC_STACK, "codec cop", COP_CODEC_PRIO))
    {
        /* No COP: the codec runs here, through the same struct, and
         * every later load of this boot too (cop_down) */
        cop_down = true;
        cop_refused("start");
        return run_here();
    }

    sync_fields();
    log_place();
    /* 0.8.3 colocate: this run reads the ring on the COP, unless the
     * developer switch keeps the old calls, said loudly */
    memset(&ins, 0, sizeof(ins));
    rin_on = colocate;
    if (!colocate && !colocate_said)
    {
        colocate_said = true;
        ui_log("codec", "COP FAILED (colocate off): input by calls");
        logf("codec cop: COP FAILED (colocate off), input by calls");
    }
    in_attach();
    if (rin_on && !rin.live && !colocate_said
        && buf_handle_data_type(real->audio_hid) == TYPE_PACKET_AUDIO)
    {
        colocate_said = true;   /* streamed audio, and still no lease */
        ui_log("codec", "COP FAILED (colocate attach): input by calls");
        logf("codec cop: COP FAILED (colocate attach), input by calls");
    }
    /* #282 step 4: this run's DSP goes with it, unless the developer
     * switch keeps it on the CPU: the old per-frame crossing, said loudly */
    memset(&win.dst, 0, sizeof(win.dst));
    win.hold = 0;
    win.on = dsp_enabled;
    /* 0.8.3 colocate: the output side goes with the input side's switch */
    win.leased = 0;
#ifdef CODEC_COP_LEASE
    win.may_lease = dsp_enabled && rin_on;
#else
    win.may_lease = 0;
#endif
    /* road test: this run's per-block numbers start over */
    win.blk_n = win.blk_max = win.dec_max = win.dsp_max = win.worst = 0;
    win.dec_min = win.dsp_min = UINT32_MAX;
    win.dec_sum = win.dsp_sum = win.samples = 0;
    win.t_valid = 0;
    ck_min = ck_max = 0;
    ck_sample();
    run_diag_start();
    /* p083-copclock: this run's output rate, a fresh average (the boost
     * asked for stays until a period of this run lets it go) */
    dm.cyc = 0;
    dm.busy = 0;
    dm.out = 0;
    dm.out_rate = mixer_get_frequency();
    cpl_demand_restart(&dm.d);
    if (!dsp_enabled && !dsp_off_said)
    {
        dsp_off_said = true;
        ui_log("codec", "COP FAILED (dsp off): DSP on the CPU");
        logf("codec cop: COP FAILED (dsp off), DSP on the CPU");
    }
    /* In: the loaded image and statics, id3 and the fields (the CPU writes
     * back). Out: the codec's statics, for its unload on the CPU or the
     * next run there */
    t = cop_job_post(&worker, run_job, NULL, COPJ_CODE | COPJ_IN | COPJ_OUT);
#ifdef HAVE_IPOD_UI
    if (t >= 0)
        audio_start_trace_step(STS_POST);   /* p083-codecload */
#endif
    if (t < 0)
    {
        win.on = 0;
        in_detach();
        rin_on = false;
        cop_refused("post");
        return run_here();
    }
    {
        uint32_t f0 = win.frames, w0 = win.windows, us0 = win.dsp_us;
        uint32_t c0 = worker.calls, cw0 = worker.call_wait_us;
        uint32_t b0 = win.backs, ls0 = win.lsleep_us;
#ifdef CODEC_COP_LEASE
        uint32_t lg0, lr0, lc0, lsl0, lg1, lr1, lc1, lsl1;

        pcmbuf_lease_stats(&lg0, &lr0, &lc0, &lsl0);
#endif

        cop_job_wait(&worker, t);
        /* the COP's last reads onto the handle and curpos; no lease */
        in_detach();
        rin_on = false;
#ifdef CODEC_COP_LEASE
        /* the write side back from the COP (it gave it back at the end) */
        pcmbuf_lease_reclaim();
#endif
        /* What the COP wrote at its end (the COP wrote its cache back at
         * the finish, the CPU dropped its own at the collect) */
        if (win.on && win.dst.remcount > 0)
        {
            int n;
            bool hold, leased;
            codec_pcm_window(win.dst.remcount, win.elapsed, win.offset, 0,
                             false, &n, &hold, &leased);
        }
        win.on = 0;
        win.may_lease = 0;
        win.dst.remcount = 0;
        win.dst.bufcount = 0;
        /* #282: the run's numbers for the emulator's A/B, one line a track:
         * frames (dsp_process calls), windows, DSP us, calls, call wait us */
        ui_log("codec", "dsp f=%lu w=%lu us=%lu c=%lu cw=%lu",
               (unsigned long)(win.frames - f0),
               (unsigned long)(win.windows - w0),
               (unsigned long)(win.dsp_us - us0),
               (unsigned long)(worker.calls - c0),
               (unsigned long)(worker.call_wait_us - cw0));
        /* 0.8.3 colocate: what the COP did itself instead of a call:
         * requests, advances, elapsed, reads (read_filebuf), forward
         * seeks; fall: the reads that still called */
        ui_log("codec", "in req=%lu adv=%lu el=%lu rd=%lu sk=%lu fall=%lu",
               (unsigned long)ins.req, (unsigned long)ins.adv,
               (unsigned long)ins.el, (unsigned long)ins.rd,
               (unsigned long)ins.sk, (unsigned long)ins.fall);
#ifdef CODEC_COP_LEASE
        /* the output side: leases taken, given back by the COP, taken
         * back by the CPU; chunks committed and full-buffer sleeps on the
         * COP, and the ms it slept */
        pcmbuf_lease_stats(&lg1, &lr1, &lc1, &lsl1);
        ui_log("codec", "out g=%lu back=%lu rv=%lu cm=%lu sl=%lu/%lums",
               (unsigned long)(lg1 - lg0), (unsigned long)(win.backs - b0),
               (unsigned long)(lr1 - lr0), (unsigned long)(lc1 - lc0),
               (unsigned long)(lsl1 - lsl0),
               (unsigned long)((win.lsleep_us - ls0) / 1000));
#else
        (void)b0; (void)ls0;
#endif
        ck_sample();
        log_blocks();       /* blk_n counts only DSP-on-COP blocks */
        run_diag_log();
    }
    runs++;
    heapchk_at("cop done");         /* 4gh-b5: the COP's run is over */
    return run_status;
}

void codec_cop_unload(void)
{
    /* p083-copclock review: the end of playback (or a codec change) ends
     * the demand too; the next run is judged fresh */
    if (dm.d.want)
    {
        cpl_demand_init(&dm.d);
        dm.gen++;
    }
    cop_worker_stop(&worker);
    if (bound)
        heapchk_at("unplace");      /* 4gh-b5: the codec leaves the cores */
    bound = false;
    on_cop = false;
}

void codec_cop_place_stats(uint32_t *cop, uint32_t *cpu)
{
    *cop = runs;
    *cpu = cpu_runs;
}

#endif /* HAVE_COP_CODEC */
