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

/* Host test of P19 19-1c, codec placement by screen state:
 *  - the pure rule (apps/codec_place_core.c): the core at a load, at a
 *    track boundary under the 30 s hold, never for a dim screen; the
 *    codec's helper switch; D's full-buffer sleep;
 *  - apps/codec_cop.c itself, on stub headers (stub/): one load, runs on
 *    both cores with no reload, the reserved stack on every run, the
 *    playback fields the codec reads kept current on a CPU run (the
 *    stale ci_cop.curpos after advance_buffer/seek_buffer), the id3 alias
 *    per run, a seek's re-run keeping its core, cop_assist set before
 *    each run. */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "codecs.h"
#include "cop_job.h"
#include "codec_cop.h"
#include "dsp_core.h"
#include "codec_place_core.h"
#include "playback.h"         /* p083-codecload: STS_POST */
#include "buffering.h"

static int fails;

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
    fails++; } } while (0)

/* ---- stubs for codec_cop.c ------------------------------------------- */

long current_tick;
static bool bl_on = true, panel_on = true, cross = true, in_worker;
static unsigned char slot[0x10000];
static int worker_starts, posts;
static bool start_fails, post_fails;    /* the COP will not take the run */
static int cop_fail_logs;               /* "codec" lines saying so */
/* 0.8.3 colocate: the fake write lease's owner (test_lease) */
enum { L_CPU, L_COP, L_BACK };
enum { PCMBUF_LEASE_FULL = 1, PCMBUF_LEASE_BACK };  /* as codec_cop.c */
static int l_owner;
static long l_bad, l_calls_leased, l_wait_leased;

void commit_dcache(void) { }
/* road test (codec_cop.c's track line) */
const char *get_codec_string(int type) { (void)type; return "FLAC"; }
unsigned int mixer_get_frequency(void) { return 44100; }
void yield(void) { }
void panicf(const char *fmt, ...) { (void)fmt; abort(); }
/* p083-copclock: the run summaries ("fiq ... und= dm="), one per run */
static int fiq_lines;
static char fiq_last[96];
void ui_log(const char *tag, const char *fmt, ...)
{
    if (!strcmp(tag, "codec") && strstr(fmt, "COP FAILED"))
        cop_fail_logs++;
    if (!strcmp(tag, "codec") && !strncmp(fmt, "fiq ", 4))
    {
        va_list ap;

        va_start(ap, fmt);
        vsnprintf(fiq_last, sizeof(fiq_last), fmt, ap);
        va_end(ap);
        fiq_lines++;
    }
}
/* p083-codecload: the start trace's "pst" step (playback.c), counted:
 * one per run, on the COP at the post or on the CPU at its start */
static unsigned int trace_post;
void audio_start_trace_step(int step)
{
    if (step == STS_POST)
        trace_post++;
}
bool is_backlight_on(bool ignore_always_off)
{
    (void)ignore_always_off;
    return bl_on;
}
bool lcd_active(void) { return panel_on; }
bool cop_jobs_cross_core(void) { return cross; }
bool cop_worker_start(struct cop_worker *w, void *stack, size_t size,
                      const char *name, int prio)
{
    (void)stack; (void)size; (void)name; (void)prio;
    if (start_fails)
        return false;
    w->started = 1;
    worker_starts++;
    return true;
}
void cop_worker_stop(struct cop_worker *w) { w->started = 0; }
int cop_job_post(struct cop_worker *w, cop_job_fn fn, void *arg,
                 unsigned flags)
{
    (void)flags;
    if (!w->started || post_fails)
        return -1;
    posts++;
    in_worker = true;
    fn(arg);
    in_worker = false;
    return 0;
}
void cop_job_wait(struct cop_worker *w, int ticket)
{
    (void)w; (void)ticket;
}
intptr_t cop_job_call(struct cop_worker *w, cop_call_fn fn, intptr_t *io,
                      unsigned flags)
{
    intptr_t r;
    (void)flags;
    w->calls++;
    if (l_owner == L_COP)
        l_calls_leased++;   /* 0.8.3: only the input's may come now */
    in_worker = false;      /* the handler runs on the codec thread */
    r = fn(io);
    in_worker = true;
    return r;
}
bool cop_job_in_worker(const struct cop_worker *w)
{
    return w->started && in_worker;
}
void *codec_slot_top(size_t size) { return &slot[sizeof(slot) - size]; }
static bool queue_busy;
bool codec_queue_idle(void) { return !queue_busy; }

/* ---- the real api (codecs.c's ci) and a fake codec --------------------- */

static struct codec_api real;
static struct codec_api *seen;      /* what the codec was handed */
static char id3_obj[64] __attribute__((aligned(8)));

static void r_advance(size_t n) { real.curpos += n; }
static bool r_seek(size_t pos) { real.curpos = pos; return true; }
static size_t r_read(void *p, size_t n) { (void)p; real.curpos += n; return n; }
static void *r_request(size_t *got, size_t want)
{
    *got = want;
    return slot;
}
static void r_insert(const void *a, const void *b, int n)
{
    (void)a; (void)b; (void)n;
}
static bool r_halt;                 /* 0.8.3: a stop (USB) waits for the codec */
static struct buf_remote_reader *fremote;   /* (test_colocate's, below) */
static long colo_applies_bad;
static long r_command(intptr_t *param)
{
    (void)param;
    if (fremote)
        colo_applies_bad++;     /* the stop is handled with the lease off */
    return r_halt ? CODEC_ACTION_HALT : CODEC_ACTION_NULL;
}
static void r_strip(off_t v) { real.filesize = v; }
static void r_seek_complete(void) { }

static void real_init(void)
{
    memset(&real, 0, sizeof(real));
    real.filesize = 100000;
    /* #282: the COP reads the id3 through its alias (the stub's offset):
     * id3_obj is that alias, so a read there lands in real memory */
    real.id3 = (struct mp3entry *)((uintptr_t)id3_obj - STUB_UNCACHED_OFS);
    real.advance_buffer = r_advance;
    real.seek_buffer = r_seek;
    real.read_filebuf = r_read;
    real.request_buffer = r_request;
    real.pcmbuf_insert = r_insert;
    real.get_command = r_command;
    real.strip_filesize = r_strip;
    real.seek_complete = r_seek_complete;
    real.cop_assist = true;
}

/* One run of the fake codec: what it saw */
struct run {
    bool in_worker;         /* ran on the COP */
    bool fields_ok;         /* curpos and filesize matched after each call */
    bool id3_cached;        /* id3 was the cached pointer */
    bool assist;            /* ci->cop_assist at the run's start */
};
static struct run last;

/* #282 step 4: the DSP on the COP. dsp_process() as the real one moves
 * samples (one out per one in, held back none); codec_pcm_window() as
 * codec_thread.c's: publish, then a window of win_grant samples */
static int ins_frames = 1, ins_n = 10;  /* the fake codec's inserts */
static uint32_t dsp_cop_runs, dsp_cpu_runs; /* test_dsp_window's runs */
static long dsp_in, published, window_calls, publish_calls;
static int lock_depth, lock_bad, win_grant = 1000;
static bool win_hold = true;
static unsigned long pub_elapsed_first;
static int16_t pcm[1 << 16];

void dsp_xcore_lock(void) { lock_depth++; }
void dsp_xcore_unlock(void) { if (--lock_depth < 0) lock_bad++; }
void dsp_process(struct dsp_config *dsp, struct dsp_buffer *src,
                 struct dsp_buffer *dst, bool thread_yield)
{
    int n;
    (void)dsp; (void)thread_yield;
    if (lock_depth != 1)
        lock_bad++;                     /* the COP runs it under the lock */
    if (dst->bufcount <= 0)
        return;
    n = src->remcount < dst->bufcount ? src->remcount : dst->bufcount;
    if (n <= 0)
        return;
    src->remcount -= n;
    dsp_in += n;
    dst->remcount += n;
    dst->bufcount -= n;
    dst->p16out += 2 * n;
}
/* 0.8.3 diagnostics: no full-buffer sleeps and no level in this model */
long pcmbuf_level_min_take(void) { return -1; }
/* p083-copclock */
static unsigned long stub_mid_underruns;
static unsigned long run_underruns;     /* added by each fake run */
unsigned long pcmbuf_mid_underruns(void) { return stub_mid_underruns; }
static int stub_ticks;
int tick_add_task(void (*f)(void)) { (void)f; stub_ticks++; return 0; }
uint32_t codec_pcm_window_full_us(void) { return 0; }
/* ---- 0.8.3 colocate, the output side: a fake pcmbuf write lease ------ */

static bool l_grant_ok, slept_cpu;             /* codec_pcm_window may hand it over */
static bool l_revoke, l_paused;
static long l_level, l_cap = 50000, l_wm = 5000, l_drain = 3000;
static long l_committed, l_grants, l_releases, l_sleeps;
static long l_drained;              /* the fake FIQ's progress (ridx) */
static long l_back_at = -1;         /* revoke once this much is committed */
static int l_queue_at = -1;         /* a message once this much is committed */
static long l_pause_at = -1;        /* a pause once this much is committed */

void pcmbuf_lease_reclaim(void)
{
    if (l_owner == L_COP)
        l_bad++;            /* the CPU never runs while the COP holds it */
    l_owner = L_CPU;
}
void *pcmbuf_lease_window(int *count, int *why)
{
    if (l_owner != L_COP)
        l_bad++;
    if (l_revoke)
    {
        *why = PCMBUF_LEASE_BACK;
        return NULL;
    }
    if (l_level + *count > l_cap)
    {
        *why = PCMBUF_LEASE_FULL;
        return NULL;
    }
    if (l_level < l_wm)
    {
        *why = PCMBUF_LEASE_BACK;
        return NULL;
    }
    *count = (int)(l_cap - l_level);
    return pcm;
}
static void ffill(void);
static void fapply(void);
static bool l_fill_stalled;         /* the disk is slow: no fill meanwhile */
void pcmbuf_lease_commit(int count, unsigned long elapsed, off_t offset)
{
    (void)elapsed; (void)offset;
    /* the buffering thread runs on the CPU meanwhile: it applies the
     * COP's reads and fills ahead (remote_apply(), remote_publish()) */
    if (!l_fill_stalled)
    {
        fapply();
        ffill();
    }
    if (l_owner != L_COP)
        l_bad++;
    l_level += count;
    l_committed += count;
    if (l_back_at >= 0 && l_committed >= l_back_at)
    {
        l_revoke = true;
        l_back_at = -1;
    }
    if (l_pause_at >= 0 && l_committed >= l_pause_at)
    {
        l_paused = true;
        l_pause_at = -1;
    }
    if (l_queue_at >= 0 && l_committed >= l_queue_at)
    {
        queue_busy = true;
        l_queue_at = -1;
    }
}
long pcmbuf_lease_full_ticks(void) { return 2; }
size_t pcmbuf_lease_ridx(void) { return (size_t)l_drained; }
void pcmbuf_lease_release(void)
{
    if (l_owner != L_COP)
        l_bad++;
    l_owner = L_BACK;
    l_releases++;
}
void pcmbuf_lease_stats(uint32_t *g, uint32_t *r, uint32_t *c, uint32_t *sl)
{
    *g = l_grants; *r = 0; *c = l_committed; *sl = l_sleeps;
}
/* the COP's full-buffer sleep: the FIQ plays meanwhile (unless paused) */
void sleep(long ticks)
{
    l_sleeps++;
    long d = l_paused ? 0 : l_drain * ticks;
    if (d > l_level)
        d = l_level;
    l_level -= d;
    l_drained += d;
}

void *codec_pcm_window(int used, unsigned long elapsed, off_t offset,
                       int want, bool may_lease, int *count, bool *hold,
                       bool *leased)
{
    (void)offset;
    window_calls++;
    *count = 0;
    *hold = false;
    *leased = false;
    pcmbuf_lease_reclaim();
    if (used > 0)
    {
        if (published == 0)
            pub_elapsed_first = elapsed;
        published += used;
        publish_calls++;
        if (l_grant_ok)
            l_level += used;        /* the lease tests count the level */
    }
    if (want <= 0 || queue_busy)
        return NULL;
    if (l_grant_ok && l_level + want > l_cap)
    {
        /* the CPU's full-buffer sleep; a pause ends here (the user) */
        if (l_paused)
            l_paused = false;
        else
        {
            long d = l_level < 15000 ? l_level : 15000;
            l_level -= d;
            l_drained += d;
        }
        slept_cpu = true;
    }
    if (may_lease && l_grant_ok && slept_cpu && l_level >= l_wm)
    {
        slept_cpu = false;
        l_owner = L_COP;
        l_revoke = false;
        l_grants++;
        *leased = true;
        return NULL;
    }
    *count = win_grant;
    *hold = win_hold;
    return pcm;
}

/* ---- 0.8.3 colocate: a fake buffering ring and one audio handle ------- */

#define RLEN    4096                /* the ring */
#define RGUARD  512                 /* bufgetdata's guard past its end */
#define FHID    7
static unsigned char fring[RLEN + RGUARD];
static struct {
    uint32_t start, data, pos, end, filesize;
} fh;
static struct buf_remote_reader *fremote;
static bool colo_codec;             /* codec_run_proc runs colo_run() */
static long colo_bad, colo_frames, colo_applies_bad;
static long colo_halt_at = -1;      /* a stop (a USB plug) at this frame */
static uint32_t colo_seek_at, colo_seek_to;
static size_t colo_skip;            /* one advance this big, mid-run */
static int colo_ins = 10;           /* samples the codec makes a frame */

static unsigned char pat(uint32_t o) { return (unsigned char)(o * 7 + 3); }
static uint32_t fidx(uint32_t pos) { return (fh.data + pos - fh.start) % RLEN; }
/* the ring's cached address (the alias is +STUB_UNCACHED_OFS: the real
 * bytes), so a read that skips cop_uncached() reads nothing of ours */
static char *ring_cached(void)
{
    return (char *)((uintptr_t)fring - STUB_UNCACHED_OFS);
}

static void fpublish(void)
{
    if (fremote)
    {
        fremote->filesize = fh.filesize;
        fremote->end = fh.end;
    }
}

/* the buffering thread: as much as fits behind the applied read pos */
static void ffill(void)
{
    uint32_t lim = fh.pos + RLEN - 1;

    if (lim > fh.filesize)
        lim = fh.filesize;
    while (fh.end < lim)
    {
        fring[fidx(fh.end)] = pat(fh.end);
        fh.end++;
    }
    fpublish();
}

static void fapply(void)
{
    if (!fremote)
        return;
    if (fremote->pos < fh.start || fremote->pos > fh.end)
        colo_applies_bad++;
    else
        fh.pos = fremote->pos;
}

bool buf_remote_attach(int hid, struct buf_remote_reader *r)
{
    r->live = 0;
    fapply();
    if (fremote)
        fremote->live = 0;
    fremote = NULL;
    if (hid != FHID)
        return false;
    r->hid = hid;
    r->base = ring_cached();
    r->len = RLEN;
    r->map_pos = fh.start;
    r->map_idx = fh.data;
    r->pos = fh.pos;
    fremote = r;
    fpublish();
    r->live = 1;
    return true;
}

off_t buf_remote_detach(struct buf_remote_reader *r)
{
    off_t p = -1;

    if (fremote == r)
    {
        fapply();
        p = fh.pos;
        fremote = NULL;
    }
    r->live = 0;
    return p;
}

int buf_handle_data_type(int hid)
{
    return hid == FHID ? TYPE_PACKET_AUDIO : TYPE_CODEC;
}

/* codec_thread.c's callbacks over the fake handle (the CPU's side) */
static void *f_request(size_t *got, size_t want)
{
    uint32_t n = want, idx;

    if (fremote)
        colo_applies_bad++;         /* a handler ran with the lease held */
    if (fh.pos >= fh.filesize)
    {
        *got = 0;
        return NULL;
    }
    if (n == 0 || n > fh.filesize - fh.pos)
        n = fh.filesize - fh.pos;
    if (fh.end < fh.pos + n)
    {
        /* prep_bufdata's wait: a skip or stop now reclaims pcmbuf's
         * write side; with the COP holding it, that reclaim would wait
         * for this fill (review of d184744599, major 1): never */
        if (l_owner == L_COP)
            l_wait_leased++;
        ffill();
    }
    idx = fidx(fh.pos);
    CHECK(n <= RGUARD);             /* a test's request fits the guard */
    if (n > RGUARD)
        n = RGUARD;
    if (idx + n > RLEN)             /* the guard copy */
        memcpy(&fring[RLEN], fring, idx + n - RLEN);
    *got = n;
    return ring_cached() + idx;
}

static void f_advance(size_t n)
{
    if (fremote)
        colo_applies_bad++;
    fh.pos += n;
    real.curpos += n;
    ((struct mp3entry *)id3_obj)->offset = real.curpos;
    if (fh.end < fh.pos)
        ffill();
}

static bool f_seek(size_t np)
{
    if (fremote)
        colo_applies_bad++;
    if (np < fh.start || np > fh.end)
    {
        /* rebuffer_handle: the data starts over somewhere else */
        fh.data = (fh.data + 1000) % RLEN;
        fh.start = fh.pos = fh.end = np;
        ffill();
    }
    fh.pos = np;
    real.curpos = np;
    return true;
}

static void f_set_elapsed(unsigned long v)
{
    ((struct mp3entry *)id3_obj)->elapsed = v;
}

/* codec_filebuf_callback over the fake handle: bufread (waits for the
 * fill), then the position as an advance without the offset. dst is the
 * alias h_read_filebuf() made of the codec's buffer; the codec's buffers
 * are plain memory here, so the bytes go back below the alias */
static size_t f_read(void *dst, size_t n)
{
    unsigned char *d = (unsigned char *)((uintptr_t)dst - STUB_UNCACHED_OFS);

    if (fremote)
        colo_applies_bad++;
    if (fh.pos >= fh.filesize)
        return 0;
    if (n > fh.filesize - fh.pos)
        n = fh.filesize - fh.pos;
    if (fh.end < fh.pos + n)
        ffill();
    for (size_t i = 0; i < n; i++)
        d[i] = fring[fidx(fh.pos + (uint32_t)i)];
    fh.pos += n;
    real.curpos += n;
    if (fh.end < fh.pos)
        ffill();
    return n;
}

/* #294: an MP4/M4A/M4B (AAC, lib/rbcodec/codecs/aac.c and libm4a): the
 * sample tables read 4 bytes a read_filebuf (demux.c stsz, stco), after
 * one odd-sized read (so reads land across the ring's wrap); then each
 * audio chunk is a forward seek_buffer past the bytes between chunks (a
 * film's picture, an interleaved stream) and a few frames (request,
 * insert, advance); then reads to the file's end and past it. Every byte
 * checked */
#define MP4_HDR     2000            /* 4-byte table reads */
#define MP4_GAP     600             /* the picture's bytes between chunks */
#define MP4_FRAMES  4
#define MP4_FRAME   150
static bool mp4_codec;              /* codec_run_proc runs mp4_run() */
static long mp4_bad, mp4_tail;

static void mp4_check(const unsigned char *p, uint32_t off, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (p[i] != pat(off + (uint32_t)i))
        {
            mp4_bad++;
            return;
        }
}

static void mp4_run(void)
{
    unsigned char b[16];
    uint32_t off;
    size_t n, got;

    off = (uint32_t)seen->curpos;
    n = seen->read_filebuf(b, 7);
    if (n != 7)
        mp4_bad++;
    mp4_check(b, off, n);
    for (int i = 0; i < MP4_HDR; i++)
    {
        off = (uint32_t)seen->curpos;
        n = seen->read_filebuf(b, 4);
        if (n != 4 || (uint32_t)seen->curpos != off + 4)
            mp4_bad++;
        mp4_check(b, off, n);
    }
    while ((uint32_t)seen->curpos + MP4_GAP + MP4_FRAMES * MP4_FRAME + 16
           <= (uint32_t)seen->filesize)
    {
        uint32_t to = (uint32_t)seen->curpos + MP4_GAP;

        if (!seen->seek_buffer(to) || (uint32_t)seen->curpos != to)
            mp4_bad++;
        for (int f = 0; f < MP4_FRAMES; f++)
        {
            unsigned char *p = seen->request_buffer(&got, MP4_FRAME);

            off = (uint32_t)seen->curpos;
            if (got != MP4_FRAME)
                mp4_bad++;
            mp4_check(p, off, got);
            seen->pcmbuf_insert(NULL, NULL, colo_ins);
            seen->advance_buffer(got);
            colo_frames++;
        }
    }
    /* to the end in 9-byte reads; the last is short, then nothing */
    mp4_tail = 0;
    while (1)
    {
        off = (uint32_t)seen->curpos;
        n = seen->read_filebuf(b, 9);
        if (n == 0)
            break;
        if (mp4_tail > (long)seen->filesize)
        {
            mp4_bad++;              /* the reads never reach the end */
            break;
        }
        mp4_check(b, off, n);
        mp4_tail += (long)n;
    }
    if ((uint32_t)seen->curpos != (uint32_t)seen->filesize)
        mp4_bad++;
}

/* #294: WMA (lib/rbcodec/codecs/libasf/asf.c asf_read_packet(), wma.c):
 * each packet reads its fields one byte per read_filebuf (the sync byte,
 * the flags, the property byte, the payload count when there are several
 * payloads), skips the error-correction bytes with advance_buffer(2),
 * reads the length/sequence/padding/time fields in one read, then takes
 * the payload with request_buffer and advance_buffer. 5 read_filebuf a
 * packet; on 623a303f10 each one was a cross-core call. Every byte is
 * checked, the packets laid so that some fields straddle the ring's wrap */
#define WMA_PACKET  400             /* the packet size (the payload request stays inside RGUARD) */
#define WMA_FIELDS  10              /* flags 0x5d: 1 + 2 + 1 + 4 + 2 bytes */
static bool wma_codec;              /* codec_run_proc runs wma_run() */
static long wma_bad, wma_packets, wma_reads;

static void wma_byte(uint32_t *off)
{
    unsigned char c;

    *off = (uint32_t)seen->curpos;
    if (seen->read_filebuf(&c, 1) != 1 || c != pat(*off))
        wma_bad++;
    wma_reads++;
}

static void wma_run(void)
{
    unsigned char d[16];
    uint32_t off, start;
    size_t got;

    while ((uint32_t)seen->curpos + WMA_PACKET <= (uint32_t)seen->filesize)
    {
        start = (uint32_t)seen->curpos;
        wma_byte(&off);                         /* 0x82: ec flags */
        seen->advance_buffer(2);                /* the ec data */
        wma_byte(&off);                         /* packet_flags */
        wma_byte(&off);                         /* packet_property */
        off = (uint32_t)seen->curpos;
        if (seen->read_filebuf(d, WMA_FIELDS) != WMA_FIELDS)
            wma_bad++;
        mp4_check(d, off, WMA_FIELDS);
        wma_reads++;
        wma_byte(&off);                         /* the payload count */
        /* the payloads: the rest of the packet, where it is buffered */
        {
            uint32_t rest = WMA_PACKET - ((uint32_t)seen->curpos - start);
            unsigned char *p = seen->request_buffer(&got, rest);

            off = (uint32_t)seen->curpos;
            if (got != rest)
                wma_bad++;
            mp4_check(p, off, got);
            seen->pcmbuf_insert(NULL, NULL, colo_ins);
            seen->advance_buffer(got);
        }
        if ((uint32_t)seen->curpos != start + WMA_PACKET)
            wma_bad++;
        wma_packets++;
    }
}

/* #294 and the Opus start fix: a header scan a byte a read. Upstream's
 * seek_opus_tags()/seek_ogg_page() read one byte per read_filebuf until
 * "OpusTags"; p083-codecload's scan_to() looks at request_buffer blocks
 * instead, but any byte scan of the kind (a codec's own, or a later
 * upstream merge) is now served from the ring too. Modelled: seek to 0,
 * scan a comment header with embedded art a byte at a time, then
 * get_more_data()'s chunk reads to the end (16 KB on the device's MB
 * ring; 1 KB here, so the 4 KB ring still holds a chunk) */
#define OPUS_TAGS_AT  12000         /* the byte the scan stops on */
#define OPUS_CHUNK    1024
static bool opus_codec;             /* codec_run_proc runs opus_run() */
static long opus_bad, opus_bytes, opus_reads;

static void opus_run(void)
{
    unsigned char c, b[OPUS_CHUNK];
    uint32_t off;
    size_t n;

    if (!seen->seek_buffer(0) || seen->curpos != 0)
        opus_bad++;
    while ((uint32_t)seen->curpos < OPUS_TAGS_AT)
    {
        off = (uint32_t)seen->curpos;
        if (seen->read_filebuf(&c, 1) != 1 || c != pat(off))
        {
            opus_bad++;
            break;
        }
        opus_reads++;
    }
    while (1)
    {
        off = (uint32_t)seen->curpos;
        n = seen->read_filebuf(b, OPUS_CHUNK);
        if (n == 0)
            break;
        mp4_check(b, off, n);
        opus_bytes += (long)n;
        opus_reads++;
        seen->pcmbuf_insert(NULL, NULL, colo_ins);
        if (opus_bytes > (long)seen->filesize)
        {
            opus_bad++;             /* the reads never reach the end */
            break;
        }
    }
    if ((uint32_t)seen->curpos != (uint32_t)seen->filesize)
        opus_bad++;
}

/* A codec reading the ring: request 300, check every byte, consume 200 */
static void colo_run(void)
{
    while (1)
    {
        size_t got, use;
        unsigned char *p;

        if (colo_seek_at && (uint32_t)seen->curpos >= colo_seek_at)
        {
            colo_seek_at = 0;
            seen->seek_buffer(colo_seek_to);
            continue;
        }
        if (colo_skip && seen->curpos > 3000)
        {
            seen->advance_buffer(colo_skip);
            colo_skip = 0;
            continue;
        }
        if (colo_frames == colo_halt_at)
        {
            queue_busy = true;  /* the audio thread's stop, queued */
            r_halt = true;
        }
        if (seen->get_command(NULL) == CODEC_ACTION_HALT)
            break;              /* a stop: no read after it */
        p = seen->request_buffer(&got, 300);
        if (got == 0)
            break;
        for (size_t i = 0; i < got; i++)
            if (p[i] != pat((uint32_t)seen->curpos + i))
            {
                colo_bad++;
                break;
            }
        use = got < 200 ? got : 200;
        seen->set_elapsed((unsigned long)seen->curpos);
        seen->pcmbuf_insert(NULL, NULL, colo_ins);
        seen->advance_buffer(use);
        colo_frames++;
    }
}

int codec_run_proc(void)
{
    size_t got;
    bool ok = true;

    if (mp4_codec)
    {
        mp4_run();
        return 0;
    }
    if (wma_codec)
    {
        wma_run();
        return 0;
    }
    if (opus_codec)
    {
        opus_run();
        return 0;
    }
    if (colo_codec)
    {
        colo_run();
        return 0;
    }

    last.in_worker = in_worker;
    stub_mid_underruns += run_underruns;    /* p083-copclock */
    last.assist = seen->cop_assist;
    last.id3_cached = seen->id3 == real.id3;
    ok &= seen->curpos == real.curpos;
    seen->advance_buffer(100);
    ok &= seen->curpos == real.curpos;
    seen->seek_buffer(5000);
    ok &= seen->curpos == real.curpos && seen->curpos == 5000;
    seen->read_filebuf(NULL, 7);
    ok &= seen->curpos == real.curpos;
    seen->request_buffer(&got, 64);
    seen->strip_filesize(90000);
    ok &= seen->filesize == real.filesize && seen->filesize == 90000;
    for (int i = 0; i < ins_frames; i++)
        seen->pcmbuf_insert(NULL, NULL, ins_n);
    last.fields_ok = ok;
    return 0;
}

/* codec_thread.c's order: select at Q_CODEC_LOAD, run at Q_CODEC_RUN */
static void load(void)
{
    real_init();
    seen = codec_cop_select(&real, false);
}

static void at(long t, bool bl, bool panel)
{
    current_tick = t;
    bl_on = bl;
    panel_on = panel;
    codec_cop_observe();
}

/* ---- the pure rule ------------------------------------------------------ */

static void test_rule(void)
{
    struct cpl_state s;
    long hold = CPL_HOLD_S * HZ;

    cpl_init(&s);
    /* no observation yet: the COP (lit is the default) */
    CHECK(cpl_at_load(&s, CPL_MODE_SCREEN, 0) == CPL_COP);
    /* dark at a load: the CPU at once, no hold */
    cpl_observe(&s, CPL_DARK, 100);
    CHECK(cpl_at_load(&s, CPL_MODE_SCREEN, 101) == CPL_CPU);
    /* lit again; a boundary 29.99 s later keeps the CPU, 30 s moves */
    cpl_observe(&s, CPL_LIT, 1000);
    CHECK(cpl_at_boundary(&s, CPL_MODE_SCREEN, 1000 + hold - 1, hold)
          == CPL_CPU);
    CHECK(cpl_at_boundary(&s, CPL_MODE_SCREEN, 1000 + hold, hold)
          == CPL_COP);
    /* a repeat observation of the same state keeps its start */
    cpl_observe(&s, CPL_DARK, 5000);
    cpl_observe(&s, CPL_DARK, 5000 + hold - 5);
    CHECK(s.since == 5000);
    CHECK(cpl_at_boundary(&s, CPL_MODE_SCREEN, 5000 + hold, hold)
          == CPL_CPU);
    /* dim never moves the decode, however long */
    cpl_observe(&s, CPL_DIM, 9000);
    CHECK(cpl_at_boundary(&s, CPL_MODE_SCREEN, 9000 + 10 * hold, hold)
          == CPL_CPU);
    CHECK(cpl_at_load(&s, CPL_MODE_SCREEN, 9000 + 10 * hold) == CPL_CPU);
    /* the fixed modes */
    CHECK(cpl_at_boundary(&s, CPL_MODE_COP, 0, hold) == CPL_COP);
    CHECK(cpl_at_load(&s, CPL_MODE_CPU, 0) == CPL_CPU);
    /* the helper switch: on the COP only with the decode there */
    CHECK(cpl_cop_assist(CPL_COP));
    CHECK(!cpl_cop_assist(CPL_CPU));
}

/* ---- D ------------------------------------------------------------------ */

static void test_full_wait(void)
{
    const size_t br = 44100 * 4;            /* 16-bit stereo, 44.1 kHz */
    size_t wm = 2 * br, level;
    long w;

    /* the usual band: full at ~2.8 s, the boost line at 2 s: sleep the
     * band's upper half, 0.4 s */
    w = cpl_full_wait(wm + br * 8 / 10, wm, br, HZ);
    CHECK(w == 40);
    /* crossfade's line, 1 s under a 6 s buffer, full at 5.8 s: the same */
    CHECK(cpl_full_wait(5 * br + br * 8 / 10, 5 * br, br, HZ) == 40);
    /* never past the boost line less 0.25 s, at any level; at least a
     * tick; at most a second */
    for (level = wm; level < wm + 4 * br; level += br / 50)
    {
        w = cpl_full_wait(level, wm, br, HZ);
        CHECK(w >= 1 && w <= HZ);
        if (level > wm + br / 4 && w != HZ / 20)
            CHECK((unsigned long long)w * br / HZ
                  <= level - wm - br / 4);
    }
    /* a thin band (under 0.25 s above the line), or under it: the old
     * poll */
    CHECK(cpl_full_wait(wm + br / 5, wm, br, HZ) == HZ / 20);
    CHECK(cpl_full_wait(wm - 1, wm, br, HZ) == HZ / 20);
    CHECK(cpl_full_wait(wm, 0, 0, HZ) == HZ / 20);
    /* 96 kHz output: the same time, not the same bytes */
    CHECK(cpl_full_wait(2 * 96000 * 4 + 96000 * 4 * 8 / 10, 2 * 96000 * 4,
                        96000 * 4, HZ) == 40);
    /* the fallback: the old poll everywhere */
    cpl_full_legacy = true;
    CHECK(cpl_full_wait(wm + br * 8 / 10, wm, br, HZ) == HZ / 20);
    cpl_full_legacy = false;
}

/* ---- codec_cop.c -------------------------------------------------------- */

static void test_cop_runs(void)
{
    uint32_t cop, cpu;
    int starts, n;

    /* the default (owner, 2026-11-07, Apple's core layout): decoding on
     * the COP in every shell build, lit or dark: bound, the stack kept
     * back, the run in the worker, MP3's synthesis on the COP too */
    CHECK(codec_cop_place() == CPL_MODE_COP);
    at(10, true, true);
    load();
    CHECK(seen != &real);
    CHECK(codec_cop_reserve() > 0);
    codec_cop_run(true);
    CHECK(last.in_worker && codec_cop_active() && last.assist);
    CHECK(last.fields_ok && !last.id3_cached);
    at(20, false, false);
    current_tick = 20 + 10 * CPL_HOLD_S * HZ;
    codec_cop_run(true);                /* dark held: still the COP */
    CHECK(last.in_worker && last.assist);
    codec_cop_run(false);               /* a seek's re-run: the COP */
    CHECK(last.in_worker && last.fields_ok);
    codec_cop_unload();
    CHECK(worker_starts == 1 && posts == 3);
    CHECK(cop_fail_logs == 0);

    /* p083-copclock: a summary for every run, so for every track of an
     * album in one format (the codec stays loaded; each track is its own
     * run: playback.c codec_go() -> Q_CODEC_RUN -> codec_cop_run()), with
     * the run's own mid-track underruns, the earlier runs' not counted */
    load();
    n = fiq_lines;
    run_underruns = 0;
    codec_cop_run(true);
    CHECK(fiq_lines == n + 1 && strstr(fiq_last, " und=0 ") != NULL);
    run_underruns = 3;
    codec_cop_run(true);                /* the next track, same format */
    CHECK(fiq_lines == n + 2 && strstr(fiq_last, " und=3 ") != NULL);
    run_underruns = 0;
    codec_cop_run(true);
    CHECK(fiq_lines == n + 3 && strstr(fiq_last, " und=0 ") != NULL);
    CHECK(strstr(fiq_last, " dm=") != NULL);
    codec_cop_unload();

    /* the worker started but would not take the job: the CPU, through
     * the same struct, with a loud log line, never silently; the next
     * load goes to the COP again */
    post_fails = true;
    load();
    codec_cop_run(true);
    CHECK(!last.in_worker && !codec_cop_active() && last.fields_ok);
    CHECK(!last.assist);                /* MP3's synthesis inline */
    CHECK(cop_fail_logs == 1);
    codec_cop_unload();
    post_fails = false;
    load();
    codec_cop_run(true);
    CHECK(last.in_worker && last.assist);
    codec_cop_unload();
    CHECK(cop_fail_logs == 1);

    /* the developer A/B arm: decoding pinned to the CPU (the 0.8.0-rc
     * default until 2026-11-07), the plain path: the real api, nothing
     * reserved, synthesis inline, no worker, lit or dark */
    codec_cop_set_place(CPL_MODE_CPU);
    starts = worker_starts;
    at(30, true, true);
    load();
    CHECK(seen == &real);
    CHECK(codec_cop_reserve() == 0);
    codec_cop_run(true);
    CHECK(!last.in_worker && !codec_cop_active() && !last.assist);
    CHECK(last.fields_ok);
    codec_cop_unload();
    CHECK(worker_starts == starts);

    /* the A/B mode: by the screen */
    codec_cop_set_place(CPL_MODE_SCREEN);
    /* lit at playback start: bound, the COP, the stack reserved */
    at(100, true, true);
    load();
    CHECK(seen != &real);
    CHECK(codec_cop_reserve() > 0);
    codec_cop_run(true);
    CHECK(last.in_worker && codec_cop_active());
    CHECK(last.fields_ok);
    CHECK(!last.id3_cached);            /* the COP reads id3 uncached */
    CHECK(last.assist);
    starts = worker_starts;

    /* dark (the panel asleep) for 10 s at the next boundary: held */
    at(200, false, true);               /* dim first: no move */
    at(300, false, false);
    current_tick = 300 + 10 * HZ;
    codec_cop_run(true);
    CHECK(last.in_worker);

    /* dark held 30 s: the CPU, same load, no new worker, no reload */
    current_tick = 300 + CPL_HOLD_S * HZ;
    codec_cop_run(true);
    CHECK(!last.in_worker && !codec_cop_active());
    CHECK(last.fields_ok);              /* review N4: curpos not stale */
    CHECK(last.id3_cached);
    CHECK(!last.assist);                /* MP3's synthesis inline */
    CHECK(codec_cop_reserve() > 0);     /* the stack stays kept back */

    /* lit for 5 s, then a seek's re-run: keeps the CPU (mid-track) */
    at(5000, true, true);
    current_tick = 5000 + 60 * HZ;
    codec_cop_run(false);
    CHECK(!last.in_worker);
    /* the next boundary, lit held 60 s: back on the COP, same worker */
    codec_cop_run(true);
    CHECK(last.in_worker && last.fields_ok && last.assist);
    CHECK(worker_starts == starts);

    /* the fallback modes */
    at(20000, false, false);
    current_tick = 20000 + 60 * HZ;
    codec_cop_set_place(CPL_MODE_COP);
    codec_cop_run(true);
    CHECK(last.in_worker && last.assist);
    codec_cop_set_place(CPL_MODE_CPU);     /* the load stays bound */
    codec_cop_run(true);
    CHECK(!last.in_worker && !last.assist && last.fields_ok);
    codec_cop_set_place(CPL_MODE_SCREEN);

    codec_cop_place_stats(&cop, &cpu);
    /* COP: 3 default + 3 summaries (p083) + 1 after the refusal + 4 by
     * screen; CPU: the refusal + 1 CPU arm + 3 by screen */
    CHECK(cop - dsp_cop_runs == 11 && cpu - dsp_cpu_runs == 5);
    codec_cop_unload();
    CHECK(codec_cop_reserve() == 0);

    /* a codec change while dark: the CPU from the first run */
    load();
    codec_cop_run(true);
    CHECK(!last.in_worker && last.fields_ok && !last.assist);
    codec_cop_unload();

    /* no second core: the real api, nothing reserved, the CPU */
    cross = false;
    at(40000, true, true);
    load();
    CHECK(seen == &real);
    CHECK(codec_cop_reserve() == 0);
    codec_cop_run(true);
    CHECK(!last.in_worker && last.fields_ok);
    codec_cop_unload();
    cross = true;

    /* last (it latches for the boot): the COP will not start. This run
     * on the CPU, synthesis inline, a loud line; every later load the
     * plain path with nothing kept back, no new start (no second's
     * stall), and a loud line each load */
    codec_cop_set_place(CPL_MODE_COP);
    n = cop_fail_logs;
    starts = worker_starts;
    start_fails = true;
    load();
    CHECK(seen != &real);
    codec_cop_run(true);
    CHECK(!last.in_worker && !codec_cop_active() && last.fields_ok);
    CHECK(!last.assist);
    CHECK(cop_fail_logs == n + 1);
    codec_cop_unload();
    load();
    CHECK(seen == &real && codec_cop_reserve() == 0);
    CHECK(cop_fail_logs == n + 2);
    codec_cop_run(true);
    CHECK(!last.in_worker && !last.assist && last.fields_ok);
    codec_cop_unload();
    CHECK(worker_starts == starts);
    start_fails = false;
}

/* #282 step 4: on the COP the insert runs the DSP there into a window and
 * crosses to the CPU only to publish: every sample the DSP made reaches
 * pcmbuf (the run's end publishes the rest), a window is held across
 * frames only when the CPU says it may (no crossfade), a message for the
 * codec makes the COP publish and stop, and the developer switch falls
 * back to the per-frame crossing, said loudly */
static void dsp_reset(void)
{
    dsp_in = published = window_calls = publish_calls = 0;
    lock_bad = lock_depth = 0;
    pub_elapsed_first = 0;
}

static void test_dsp_window(void)
{
    int fails_logged;

    codec_cop_set_place(CPL_MODE_COP);
    at(10, true, true);

    /* 20 frames of 300: held windows, published every >= 4096 and at the end */
    ins_frames = 20; ins_n = 300; win_grant = 100000; win_hold = true;
    dsp_reset();
    ((struct mp3entry *)id3_obj)->elapsed = 1234;
    load();
    codec_cop_run(true);
    CHECK(last.in_worker);
    CHECK(dsp_in == 6000 && published == 6000);
    CHECK(publish_calls == 2);          /* 4200 at frame 14, 1800 at the end */
    CHECK(window_calls == 3);           /* the grant, the publish+grant, the end */
    CHECK(pub_elapsed_first == 1234);
    CHECK(lock_bad == 0 && lock_depth == 0);
    codec_cop_unload();

    /* small windows: a full window is published and the next one taken
     * in the same call; nothing is lost across the turn */
    win_grant = 128;
    dsp_reset();
    load();
    codec_cop_run(true);
    CHECK(dsp_in == 6000 && published == 6000);
    CHECK(window_calls >= 6000 / 128);
    codec_cop_unload();

    /* a crossfade (hold false): published after every frame, as before */
    win_grant = 100000; win_hold = false;
    dsp_reset();
    load();
    codec_cop_run(true);
    CHECK(published == 6000 && publish_calls == 20);
    codec_cop_unload();

    /* a message waits: the COP publishes, gets no window, stops inserting */
    win_hold = true; queue_busy = true;
    dsp_reset();
    load();
    codec_cop_run(true);
    CHECK(dsp_in == 0 && published == 0);
    queue_busy = false;
    codec_cop_unload();

    /* the developer switch: the per-frame crossing, loudly, once a load */
    fails_logged = cop_fail_logs;
    codec_cop_set_dsp(false);
    dsp_reset();
    load();
    codec_cop_run(true);
    codec_cop_run(true);
    CHECK(window_calls == 0 && dsp_in == 0);
    CHECK(cop_fail_logs == fails_logged + 1);
    codec_cop_set_dsp(true);
    codec_cop_unload();

    ins_frames = 1; ins_n = 10;
    worker_starts = posts = cop_fail_logs = 0;  /* test_cop_runs counts its own */
    codec_cop_place_stats(&dsp_cop_runs, &dsp_cpu_runs);
}

/* 0.8.3 colocate: the COP reads the ring itself. Every byte the codec
 * sees is the file's; a call is made only at a ring wrap, a refill (the
 * bytes are not there yet), a seek and the window; the CPU's handle and
 * curpos end where the COP's reads did; the developer switch goes back
 * to a call per read, loudly */
static void colo_setup(uint32_t filesize)
{
    memset(&fh, 0, sizeof(fh));
    fh.filesize = filesize;
    fh.data = 3000;                 /* the first wrap comes early */
    fremote = NULL;
    ffill();
    colo_bad = colo_frames = colo_applies_bad = 0;
    load();
    real.audio_hid = FHID;
    real.filesize = filesize;
    real.request_buffer = f_request;
    real.advance_buffer = f_advance;
    real.seek_buffer = f_seek;
    real.read_filebuf = f_read;
    real.set_elapsed = f_set_elapsed;
    seen = codec_cop_select(&real, false);
}

static void test_colocate(void)
{
    uint32_t r0, c0, c1, wms;
    int fails_logged;

    codec_cop_set_place(CPL_MODE_COP);
    at(10, true, true);
    colo_codec = true;
    win_grant = 1 << 20; win_hold = true;

    /* steady state: a call only per wrap or refill (and the windows) */
    colo_setup(40000);
    codec_cop_stats(&r0, &c0, &wms);
    dsp_reset();
    codec_cop_run(true);
    codec_cop_stats(&r0, &c1, &wms);
    CHECK(colo_bad == 0 && colo_frames >= 40000 / 200 - 1);
    CHECK(colo_applies_bad == 0);
    CHECK(fh.pos == 40000 && real.curpos == 40000);
    CHECK(fremote == NULL);                     /* detached at the end */
    CHECK(((struct mp3entry *)id3_obj)->offset == 40000);
    /* 40000 bytes through a 4096 ring: about 10 refills, 10 wraps; the
     * old path made 3 calls a frame (600+); 26 seen when written */
    printf("colocate: frames=%ld calls=%u windows=%ld\n", colo_frames,
           (unsigned)(c1 - c0), window_calls);
    CHECK(c1 - c0 - window_calls <= 3 * (40000 / RLEN + 2));
    CHECK(c1 - c0 > 0);
    codec_cop_unload();

    /* a seek mid-run (a rebuffer: the data moves): detached for the call,
     * the new place re-attached, every byte still right */
    colo_setup(30000);
    colo_seek_at = 9000; colo_seek_to = 2500;
    codec_cop_run(true);
    CHECK(colo_bad == 0 && colo_applies_bad == 0);
    CHECK(fh.pos == 30000 && real.curpos == 30000);
    colo_setup(30000);
    colo_seek_at = 5000; colo_seek_to = 25000;
    codec_cop_run(true);
    CHECK(colo_bad == 0 && colo_applies_bad == 0);
    CHECK(fh.pos == 30000 && real.curpos == 30000);
    codec_cop_unload();

    /* a skip past end goes through the CPU (its rebuffer) */
    colo_setup(30000);
    colo_skip = 9000;
    codec_cop_run(true);
    CHECK(colo_bad == 0 && colo_applies_bad == 0 && colo_skip == 0);
    CHECK(fh.pos == 30000 && real.curpos == 30000);
    codec_cop_unload();

    /* a stop mid-track (a USB plug: the audio thread stops the codec, then
     * buffering closes the handles): the stop is handled with the lease
     * off, the codec reads nothing after it, the run ends detached with
     * the handle and curpos where the COP's reads ended */
    colo_setup(30000);
    colo_halt_at = 40;
    codec_cop_run(true);
    CHECK(colo_bad == 0 && colo_applies_bad == 0 && colo_frames == 40);
    CHECK(fremote == NULL);                 /* no lease after the run */
    CHECK(fh.pos == 40 * 200 && real.curpos == 40 * 200);
    colo_halt_at = -1; queue_busy = false; r_halt = false;
    codec_cop_unload();

    /* the developer switch: a call per read, said once a load */
    fails_logged = cop_fail_logs;
    codec_cop_set_colocate(false);
    colo_setup(20000);
    codec_cop_stats(&r0, &c0, &wms);
    codec_cop_run(true);
    codec_cop_stats(&r0, &c1, &wms);
    CHECK(colo_bad == 0 && fh.pos == 20000 && real.curpos == 20000);
    CHECK(c1 - c0 >= 3 * (uint32_t)colo_frames);
    CHECK(cop_fail_logs == fails_logged + 1);
    codec_cop_set_colocate(true);
    codec_cop_unload();

    colo_codec = false;
    win_grant = 1000;
    real_init();
}

/* 0.8.3 colocate, the output side: once the CPU hands the write side over
 * the COP requests, commits and sleeps on a full buffer itself, with no
 * call at all; it gives the lease back at a revoke, a message, a level
 * under the watermark, a pause (a full buffer that plays nothing for a
 * second) and before any call (here, a seek's); the run's end gives it
 * back; every sample the DSP made is committed or published */
static void lease_reset(void)
{
    l_owner = L_CPU; l_revoke = l_paused = false;
    l_level = l_cap - 500;      /* the first window call finds it full */
    l_committed = l_grants = l_releases = l_sleeps = 0;
    l_bad = 0; l_wait_leased = l_calls_leased = 0; l_drained = 0; l_back_at = -1; l_queue_at = -1;
    l_pause_at = -1; slept_cpu = false;
    dsp_reset();
}

static void test_lease(void)
{
    uint32_t r0, c0, c1, wms;

    codec_cop_set_place(CPL_MODE_COP);
    at(10, true, true);
    colo_codec = true;
    win_grant = 2000; win_hold = true;
    l_grant_ok = true;
    colo_ins = 400;             /* 80000 samples a run: past the cap */

    /* steady state: one window call grants it, then no call at all but
     * the input's own (wraps, refills) */
    colo_setup(40000);
    lease_reset();
    codec_cop_stats(&r0, &c0, &wms);
    codec_cop_run(true);
    codec_cop_stats(&r0, &c1, &wms);
    CHECK(l_wait_leased == 0);          /* no wait on a fill while leased */
    CHECK(l_calls_leased > 0);          /* the wrap's guard copy kept it */
    printf("lease: grants=%ld commits=%ld sleeps=%ld windows=%ld calls=%u\n",
           l_grants, l_committed, l_sleeps, window_calls,
           (unsigned)(c1 - c0));
    CHECK(l_bad == 0 && colo_bad == 0);
    CHECK(l_grants == 1 && window_calls == 1 && published == 0);
    CHECK(l_committed == dsp_in && dsp_in == colo_ins * colo_frames);
    CHECK(l_sleeps > 0);                /* the COP slept on a full buffer */
    CHECK(l_releases == 1 && l_owner == L_CPU);     /* back at the end */
    CHECK(c1 - c0 <= 1 + 3 * (40000 / RLEN + 2));
    codec_cop_unload();

    /* the fill falls behind under the lease (a slow disk): the request
     * that must wait for it gives the lease back first, so a skip or a
     * stop meanwhile never waits on buffering in its reclaim */
    colo_setup(40000);
    lease_reset();
    l_fill_stalled = true;
    codec_cop_run(true);
    l_fill_stalled = false;
    CHECK(l_bad == 0 && colo_bad == 0 && l_wait_leased == 0);
    CHECK(l_releases >= 2);             /* given back at the wait */
    CHECK(l_committed + published == dsp_in);
    codec_cop_unload();

    /* a revoke (the CPU wants the write side): given back at the next
     * frame, taken again at the next window call */
    colo_setup(40000);
    lease_reset();
    l_back_at = 300;
    codec_cop_run(true);
    CHECK(l_bad == 0 && colo_bad == 0);
    CHECK(l_grants == 2 && l_releases == 2);
    CHECK(l_committed + published == dsp_in);
    codec_cop_unload();

    /* a pause: full and nothing played for a second, given back */
    colo_setup(40000);
    lease_reset();
    l_pause_at = 2000;
    codec_cop_run(true);
    CHECK(l_bad == 0 && l_grants >= 2 && l_releases >= 2);
    CHECK(l_sleeps >= HZ / 2);          /* a second of 2-tick slices */
    CHECK(l_committed + published == dsp_in);
    codec_cop_unload();

    /* a message for the codec mid-lease: given back, the call path stops */
    colo_setup(40000);
    lease_reset();
    l_queue_at = 500;
    codec_cop_run(true);
    CHECK(l_bad == 0 && l_releases >= 1 && l_owner == L_CPU);
    CHECK(dsp_in < colo_ins * 40000 / 200); /* stopped at the message */
    queue_busy = false;
    codec_cop_unload();

    /* a seek mid-run: the lease goes back before its call */
    colo_setup(30000);
    lease_reset();
    colo_seek_at = 9000; colo_seek_to = 2500;
    codec_cop_run(true);
    CHECK(l_bad == 0 && colo_bad == 0 && l_releases >= 2);
    CHECK(l_committed + published == dsp_in);
    codec_cop_unload();

    /* a low level: the rescue boost is the CPU's: given back */
    colo_setup(80000);
    lease_reset();
    l_drain = 30000;            /* a sleep drains it under the watermark */
    codec_cop_run(true);
    printf("lease low: grants=%ld releases=%ld published=%ld bad=%ld\n",
           l_grants, l_releases, published, l_bad);
    CHECK(l_bad == 0 && l_grants >= 2 && l_releases >= 2 && published > 0);
    l_drain = 3000;
    CHECK(l_committed + published == dsp_in);
    codec_cop_unload();

    /* the input switch off: no lease either */
    codec_cop_set_colocate(false);
    colo_setup(20000);
    lease_reset();
    codec_cop_run(true);
    CHECK(l_grants == 0 && published == dsp_in);
    codec_cop_set_colocate(true);
    codec_cop_unload();

    l_grant_ok = false;
    colo_ins = 10;
    colo_codec = false;
    win_grant = 1000;
    real_init();
}

/* #294: the reads a codec makes a few bytes at a time are served from
 * the ring with no call: an M4B's tables and chunk seeks, a WMA packet's
 * fields, a byte scan and chunk reads (Opus). Every byte is the file's,
 * across the wrap and to the end; the CPU's handle and curpos end where
 * the COP's reads did. Calls: about one per ring's worth (a refill:
 * size / RLEN), the final read at the end and the windows; a call per
 * read was what 623a303f10 made */
static bool video_on;
bool vplayer_running(void) { return video_on; }

/* one run of the codec pattern; false when the load was not bound */
static bool read_run(uint32_t size, uint32_t *calls)
{
    uint32_t r0, c0, c1, wms, cop0, cpu0, cop1, cpu1;

    colo_setup(size);
    CHECK(seen != &real);           /* bound: the COP's view */
    if (seen == &real)
    {
        codec_cop_unload();
        return false;
    }
    dsp_reset();
    codec_cop_place_stats(&cop0, &cpu0);
    codec_cop_stats(&r0, &c0, &wms);
    codec_cop_run(true);
    codec_cop_stats(&r0, &c1, &wms);
    codec_cop_place_stats(&cop1, &cpu1);
    CHECK(cop1 == cop0 + 1 && cpu1 == cpu0);    /* the run took the COP */
    CHECK(colo_applies_bad == 0);
    CHECK(fh.pos == size && real.curpos == size);
    CHECK(fremote == NULL);
    *calls = c1 - c0 - (uint32_t)window_calls;
    codec_cop_unload();
    return true;
}

static void test_small_reads(void)
{
    const uint32_t size = 40000;
    const uint32_t limit = 3 * (size / RLEN + 2);   /* 33 */
    uint32_t calls;

    codec_cop_set_place(CPL_MODE_COP);
    at(10, true, true);
    win_grant = 1 << 20; win_hold = true;

    /* an M4B: 2,001 table reads, ~45 chunk seeks, reads to the end */
    mp4_codec = true;
    mp4_bad = 0;
    if (read_run(size, &calls))
    {
        printf("mp4 reads: frames=%ld calls=%u (limit %u) tail=%ld\n",
               colo_frames, (unsigned)calls, (unsigned)limit, mp4_tail);
        CHECK(mp4_bad == 0 && colo_frames > 0 && mp4_tail > 0);
        CHECK(calls <= limit);
    }
    mp4_codec = false;

    /* a WMA: 5 read_filebuf a packet */
    wma_codec = true;
    wma_bad = wma_packets = wma_reads = 0;
    if (read_run(size, &calls))
    {
        printf("wma reads: packets=%ld reads=%ld calls=%u (limit %u)\n",
               wma_packets, wma_reads, (unsigned)calls, (unsigned)limit);
        CHECK(wma_bad == 0 && mp4_bad == 0);
        CHECK(wma_packets == size / WMA_PACKET);
        CHECK(wma_reads == 5 * wma_packets);    /* the pattern ran whole */
        CHECK(calls <= limit);
    }
    wma_codec = false;

    /* Opus: a byte scan of 12,000 bytes, then 1 KB chunks to the end */
    opus_codec = true;
    opus_bad = opus_bytes = opus_reads = 0;
    mp4_bad = 0;
    if (read_run(size, &calls))
    {
        printf("opus reads: reads=%ld bytes=%ld calls=%u (limit %u)\n",
               opus_reads, opus_bytes, (unsigned)calls, (unsigned)limit);
        CHECK(opus_bad == 0 && mp4_bad == 0);
        CHECK(opus_bytes == (long)(size - OPUS_TAGS_AT));
        CHECK(opus_reads >= OPUS_TAGS_AT);
        CHECK(calls <= limit);
    }
    opus_codec = false;

    /* 0.8.3: a video's soundtrack stays on the CPU (moving it is #283,
     * 0.9.0): a load while a video plays is not bound */
    video_on = true;
    colo_codec = true;
    colo_setup(size);
    CHECK(seen == &real);
    codec_cop_unload();
    colo_codec = false;
    video_on = false;

    win_grant = 1000;
    real_init();
}

int main(void)
{
    test_rule();
    test_full_wait();
    test_colocate();
    test_lease();
    test_small_reads();
    test_dsp_window();      /* before test_cop_runs: it takes the COP down */
    test_cop_runs();
    {
        uint32_t cop, cpu;

        codec_cop_place_stats(&cop, &cpu);
        /* every run, on either core, marks the start trace's step once */
        CHECK(cop + cpu > 0 && trace_post == cop + cpu);
        printf("     start trace: %u run(s), %u marked\n",
               (unsigned)(cop + cpu), trace_post);
    }
    if (fails)
    {
        fprintf(stderr, "%d check(s) failed\n", fails);
        return 1;
    }
    return 0;
}
