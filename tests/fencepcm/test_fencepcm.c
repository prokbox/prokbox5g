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

/* Host test of the 0.8.3 co-location's write lease (#290): the REAL
 * apps/pcmbuf.c (run.sh copies it here as pcmbuf_fence.c with two
 * scheduling-point #defines and, in mutation mode, one mutant) against
 * stub headers, under a two-cache model of the PP5022:
 *
 *  - the audio region (the ring and its guard) has an SDRAM copy and one
 *    write-back cache per core, 16-byte lines, write-allocate; a line is
 *    written back whole (so a stale clean line a core writes into puts
 *    stale bytes back over the other core's); commit_dcache() writes the
 *    calling core's dirty lines back, commit_discard_dcache() also drops
 *    them. The chunk descriptors and the lease are uncached on the device
 *    (XCORE_BSS, UNCACHED_ADDR) and plain shared memory here;
 *  - two threads, the CPU (the codec thread: request, write, commit,
 *    grant, and the reclaim inside every write-side entry point) and the
 *    COP (codec_cop.c lease_frames(): window, write, commit, release),
 *    run one at a time; at every access to the lease or chunk_widx, every
 *    cache operation and every sleep, a seeded schedule may switch core
 *    or run the PCM FIQ (the real pcmbuf_pcm_callback(), which reads the
 *    audio from SDRAM);
 *  - checked: at every scheduling point, no published byte (between the
 *    FIQ's chunk and chunk_widx) is dirty in either cache; every frame the
 *    FIQ takes is the next of the stream, and still is when its chunk
 *    finishes; whenever the COP may run under the lease its fields are
 *    the grant's; no window is given out with a revoke pending; the CPU
 *    runs the CPU-only calls.
 *
 * Usage: test_fencepcm [seeds] [depth 16|24]. Prints the counts it
 * checked and one verdict line, "VERDICT: PASS" or "VERDICT: FAIL n". */

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "fence_env.h"

/* ---- the real file --------------------------------------------------- */
#include "pcmbuf_fence.c"
/* the test's own reads: no scheduling point */
#undef lease
#undef chunk_widx
#define lease_raw() lease

/* ---- results --------------------------------------------------------- */
static int fails;
static char first_fail[256];

static void fail(const char *fmt, ...)
{
    va_list ap;

    if (fails++ < 6)
    {
        char b[256];

        va_start(ap, fmt);
        vsnprintf(b, sizeof(b), fmt, ap);
        va_end(ap);
        fprintf(stderr, "  FAIL: %s\n", b);
        if (!first_fail[0])
            snprintf(first_fail, sizeof(first_fail), "%s", b);
    }
}

/* ---- the PRNG (per seed, reproducible) ------------------------------- */
static uint64_t rng;

static unsigned rnd(unsigned n)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return n ? (unsigned)(rng % n) : 0;
}

/* ---- the two-cache model --------------------------------------------- */
#define LINE        16
#define M_MAX       (300 * 1024)
#define L_MAX       (M_MAX / LINE)

struct line { uint8_t valid, dirty; uint8_t d[LINE]; };
static struct line cache[2][L_MAX];
static uint32_t dirty_list[2][L_MAX];   /* the lines each core has dirty */
static unsigned n_dirty[2];
static uint8_t sdram[M_MAX], snap[M_MAX];
static uint8_t *view;           /* pcmbuf_buffer: what the running core sees */
static size_t m_len;            /* the ring and its guard */
static size_t ring_len;         /* pcmbuf_size */
static int cur_core = CPU;

/* The real code itself touches the audio only in the wrap copy (the
 * guard's bytes to the ring's start; crossfade, which mixes, never runs
 * under the lease and is off here): those two ranges of the view are the
 * running core's, the rest is written by the producers through
 * core_write() */
#define GUARD   PCMBUF_GUARD_SIZE

static void mark_dirty(int c, size_t l)
{
    if (!cache[c][l].dirty)
    {
        cache[c][l].dirty = 1;
        dirty_list[c][n_dirty[c]++] = (uint32_t)l;
    }
}

/* the running core's writes the model didn't see (the wrap memcpy) into
 * its cache: bytes of the view that differ from the snap */
static void sync_one(size_t from, size_t to)
{
    for (size_t l = from / LINE; l < to / LINE; l++)
    {
        uint8_t *v = view + l * LINE, *s = snap + l * LINE;
        struct line *c = &cache[cur_core][l];

        if (!memcmp(v, s, LINE))
            continue;
        if (!c->valid)
        {
            memcpy(c->d, sdram + l * LINE, LINE);
            c->valid = 1;
        }
        for (int b = 0; b < LINE; b++)
            if (v[b] != s[b])
                c->d[b] = v[b];
        mark_dirty(cur_core, l);
        memcpy(s, v, LINE);
    }
}

static void sync_view(void)
{
    sync_one(0, GUARD);
    sync_one(ring_len, ring_len + GUARD);
}

/* the view of core c (the two ranges): its lines, else SDRAM */
static void mat_one(int c, size_t from, size_t to)
{
    for (size_t l = from / LINE; l < to / LINE; l++)
    {
        const uint8_t *src = cache[c][l].valid ? cache[c][l].d
                                               : sdram + l * LINE;
        memcpy(view + l * LINE, src, LINE);
        memcpy(snap + l * LINE, src, LINE);
    }
}

static void materialize(int c)
{
    mat_one(c, 0, GUARD);
    mat_one(c, ring_len, ring_len + GUARD);
}

/* a producer's write of n bytes at off, on the running core */
static void core_write(size_t off, const uint8_t *src, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        size_t a = off + i, l = a / LINE;
        struct line *c = &cache[cur_core][l];

        if (!c->valid)
        {
            memcpy(c->d, sdram + l * LINE, LINE);
            c->valid = 1;
        }
        c->d[a % LINE] = src[i];
        mark_dirty(cur_core, l);
        view[a] = snap[a] = src[i];
    }
}

static void write_back(int c, bool drop)
{
    for (unsigned i = 0; i < n_dirty[c]; i++)
    {
        size_t l = dirty_list[c][i];

        memcpy(sdram + l * LINE, cache[c][l].d, LINE);
        cache[c][l].dirty = 0;
    }
    n_dirty[c] = 0;
    if (drop)
        for (size_t l = 0; l < m_len / LINE; l++)
            cache[c][l].valid = 0;
}

/* ---- the stream ------------------------------------------------------ */
static unsigned bpf;            /* bytes a frame (pcmbuf_bpf) */
static uint32_t produced;       /* the next frame's number */
static uint32_t consumed;       /* the next frame the FIQ must see */
static size_t play_off, play_len; /* the chunk the FIQ is on */
static uint32_t play_first;
static bool playing_chunk;

static void frame_bytes(uint32_t n, uint8_t *b)
{
    uint32_t w[2] = { n, n ^ 0xa5a5a5a5u };

    memcpy(b, w, bpf);
}

/* k frames of the stream at p (in the ring or its guard); the part in
 * the guard is what the wrap copy will bring to the ring's start */
static void produce(void *p, int k)
{
    size_t off = (uint8_t *)p - view;
    uint8_t b[8];

    if (off + (size_t)k * bpf > m_len)
        fail("window past the guard: off %zu + %d frames", off, k);
    for (int i = 0; i < k; i++)
    {
        frame_bytes(produced++, b);
        core_write(off + (size_t)i * bpf, b, bpf);
    }
}

/* ---- statistics (non-vacuity) ---------------------------------------- */
static unsigned long st_points, st_fiq_chunks, st_frames, st_switches,
                     st_cop_commits, st_cop_wraps, st_cop_windows,
                     st_cpu_wraps, st_grants, st_revoke_waits,
                     st_backs_revoke, st_backs_other, st_lease_checks,
                     st_published_checked, st_stale_hits;

/* ---- the mixer and the FIQ ------------------------------------------- */
static enum channel_status chan = CHANNEL_STOPPED;
static struct mixer_play_cbs play_cbs;
static bool in_fiq;

enum channel_status mixer_channel_status(enum pcm_mixer_channel ch)
{
    return ch == PCM_MIXER_CHAN_PLAYBACK ? chan : CHANNEL_STOPPED;
}

bool pcm_is_playing(void)
{
    return chan == CHANNEL_PLAYING;
}

void mixer_channel_play_data(enum pcm_mixer_channel ch,
                             const struct mixer_play_cbs *cbs,
                             const void *start, size_t size)
{
    (void)ch; (void)start; (void)size;
    play_cbs = *cbs;
    chan = CHANNEL_PLAYING;
    playing_chunk = false;
}

void mixer_channel_stop(enum pcm_mixer_channel ch)
{
    (void)ch;
    chan = CHANNEL_STOPPED;
    playing_chunk = false;
}

static void check_frames(size_t off, size_t len, uint32_t first,
                         const char *when)
{
    uint8_t b[8];

    for (size_t i = 0; i < len / bpf; i++)
    {
        frame_bytes(first + (uint32_t)i, b);
        if (memcmp(sdram + off + i * bpf, b, bpf))
        {
            uint32_t got;

            memcpy(&got, sdram + off + i * bpf, 4);
            fail("%s: frame %u at %zu reads %u from SDRAM", when,
                 (unsigned)(first + i), off + i * bpf, (unsigned)got);
            return;
        }
    }
}

/* The PCM FIQ (on the COP in layout A): the real callback; it and the
 * mixer read the audio uncached, so from SDRAM */
static void fiq_run(void)
{
    const void *start = NULL;
    size_t size = 0;

    if (chan != CHANNEL_PLAYING || !play_cbs.get_more || in_fiq)
        return;
    in_fiq = true;
    if (playing_chunk)      /* the chunk ending now: untouched while played */
        check_frames(play_off, play_len, play_first, "chunk overwritten while played");
    play_cbs.get_more(&start, &size);
    in_fiq = false;
    if (!size)
    {
        chan = CHANNEL_STOPPED;     /* the mixer stops a dry channel */
        playing_chunk = false;
        return;
    }
    play_off = (const uint8_t *)start - view;
    play_len = size;
    play_first = consumed;
    playing_chunk = true;
    check_frames(play_off, play_len, play_first, "FIQ");
    consumed += size / bpf;
    st_fiq_chunks++;
    st_frames += size / bpf;
}

/* ---- invariants at a scheduling point -------------------------------- */
static bool published(size_t off)
{
    /* [the FIQ's chunk, chunk_widx) in ring offsets, with the wrap */
    size_t r = chunk_ridx, w = chunk_widx;

    if (off >= pcmbuf_size)
        return false;           /* the guard: never read by the FIQ */
    if (r <= w)
        return off >= r && off < w;
    return off >= r || off < w;
}

static void check_published_clean(const char *where)
{
    for (int c = 0; c < 2; c++)
        for (unsigned i = 0; i < n_dirty[c]; i++)
        {
            size_t l = dirty_list[c][i];

            if (!published(l * LINE))
                continue;

            if (memcmp(cache[c][l].d, sdram + l * LINE, LINE))
            {
                fail("%s: published bytes at %zu dirty in the %s's cache "
                     "(the FIQ reads stale SDRAM)", where, l * LINE,
                     c == CPU ? "CPU" : "COP");
                return;
            }
        }
}

/* what the grant must have set before the COP may see LEASE_COP */
static struct { bool armed; size_t waiting; unsigned key, bpf;
                size_t byterate, watermark; } grant_expect;

static void check_lease_fields(void)
{
    struct pcmbuf_lease *l = &lease_raw();

    if (l->owner != LEASE_COP || !grant_expect.armed)
        return;
    st_lease_checks++;
    if (l->waiting != grant_expect.waiting || l->key != grant_expect.key
        || l->bpf != grant_expect.bpf || l->byterate != grant_expect.byterate
        || l->watermark != grant_expect.watermark)
        fail("the COP may run under the lease with its fields unset "
             "(waiting %zu/%zu bpf %u/%u)", l->waiting, grant_expect.waiting,
             l->bpf, grant_expect.bpf);
    else
        grant_expect.armed = false;     /* settled: the COP owns them now */
}

/* ---- the two cores --------------------------------------------------- */
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int running = -1;        /* the core whose thread runs */
static bool done[2], started, aborted;
static unsigned long steps, max_steps;
static unsigned p_switch, p_fiq;        /* per 1000 */
static __thread int self_core = CPU;

static void switch_to(int other)
{
    sync_view();
    running = other;
    pthread_cond_broadcast(&cv);
    while (running != self_core && !aborted)
        pthread_cond_wait(&cv, &mtx);
    if (aborted)
    {
        pthread_mutex_unlock(&mtx);
        pthread_exit(NULL);
    }
    cur_core = self_core;
    materialize(cur_core);
}

/* forced: the caller can't go on until the other core moves */
static void point(const char *where, bool forced, bool drain)
{
    int other = self_core ^ 1;

    if (!started || in_fiq)
        return;
    st_points++;
    if (fails || ++steps > max_steps)
    {
        if (!fails)
            fail("stuck at %s after %lu steps (a wait the other core "
                 "never ends)", where, steps);
        aborted = true;
        pthread_cond_broadcast(&cv);
        pthread_mutex_unlock(&mtx);
        pthread_exit(NULL);
    }
    /* the real code's own writes since the last point (the wrap copy
       lands at the ring's start) */
    sync_view();
    check_published_clean(where);
    check_lease_fields();
    if (drain || rnd(1000) < p_fiq)
        fiq_run();
    check_published_clean(where);
    if (!done[other] && (forced || rnd(1000) < p_switch))
    {
        st_switches++;
        switch_to(other);
        check_lease_fields();
    }
}

static void thread_end(void)
{
    done[self_core] = true;
    if (!done[self_core ^ 1])
    {
        sync_view();
        running = self_core ^ 1;
        pthread_cond_broadcast(&cv);
    }
    else
        pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mtx);
}

/* ---- the hooks the real file calls ----------------------------------- */
struct pcmbuf_lease *fence_lease_at(struct pcmbuf_lease *l)
{
    point("lease", false, false);
    return l;
}

size_t *fence_widx_at(size_t *p)
{
    point("chunk_widx", false, false);
    return p;
}

void commit_dcache(void)
{
    if (!started)
        return;         /* the setup between seeds: the model is reset */
    point("commit_dcache", false, false);
    sync_view();
    write_back(cur_core, false);
    point("commit_dcache done", false, false);
}

void commit_discard_dcache(void)
{
    if (!started)
        return;
    point("commit_discard_dcache", false, false);
    sync_view();
    write_back(cur_core, true);
    materialize(cur_core);
    point("commit_discard_dcache done", false, false);
}

void sleep(int ticks)
{
    (void)ticks;
    if (self_core == CPU)
        st_revoke_waits++;
    point("sleep", true, false);
}

void fence_assert_cpu(const char *where)
{
    if (started && self_core != CPU)
        fail("%s on the COP", where);
}

long current_tick;
struct user_settings global_settings;
unsigned int mixer_get_frequency(void) { return 11025; }
static unsigned depth = 16;
unsigned int mixer_get_depth(void) { return depth; }
void mixer_channel_play_pause(enum pcm_mixer_channel ch, bool play)
{ (void)ch; (void)play; }
void mixer_channel_set_amplitude(enum pcm_mixer_channel ch, unsigned int amp)
{ (void)ch; (void)amp; }
void trigger_cpu_boost(void) { }
int tick_add_task(void (*f)(void)) { (void)f; return 0; }
int tick_remove_task(void (*f)(void)) { (void)f; return 0; }
void pcm_play_lock(void) { }
void pcm_play_unlock(void) { }
struct dsp_config *dsp_get_config(int idx) { (void)idx; return NULL; }
intptr_t dsp_configure(struct dsp_config *dsp, unsigned int s, intptr_t v)
{ (void)dsp; (void)s; (void)v; return 0; }
void dsp_set_output_depth(struct dsp_config *dsp, int bits)
{ (void)dsp; (void)bits; }
void audio_start_trace_mark(int step) { (void)step; }
void audio_start_trace_pcm(void) { }
void ui_log(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
void codec_thread_set_priority(int p) { (void)p; }
void voice_thread_set_priority(int p) { (void)p; }
void audio_pcmbuf_position_callback(unsigned long e, off_t o, unsigned int k)
{ (void)e; (void)o; (void)k; }
void audio_pcmbuf_track_change(bool pcmbuf) { (void)pcmbuf; }
void audio_pcmbuf_track_change_fiq(void) { }
bool audio_pcmbuf_may_play(void) { return true; }
void audio_pcmbuf_sync_position(void) { }

/* ---- the CPU: the codec thread's write path, grants, reclaims -------- */
static bool cpu_finished;
static int rounds;

/* one window on the CPU: pcmbuf_request_buffer() reclaims first */
static bool cpu_window(bool try_grant)
{
    int n = 1 + (int)rnd(1500);
    void *p = pcmbuf_request_buffer(&n);
    int k;

    if (!p)
    {
        point("cpu full", true, true);
        return false;
    }
    /* codec_thread.c: after a full-buffer sleep, the grant comes in place
       of the window */
    if (try_grant && rnd(3) == 0)
    {
        struct pcmbuf_lease *l = &lease_raw();

        /* the lease's fields are dead while the CPU owns it: anything
           (so a grant that publishes before it sets them shows) */
        if (l->owner == LEASE_CPU)
        {
            l->waiting = 12345;
            l->key = 0xdead;
            l->bpf = 3;
            l->byterate = 7;
            l->watermark = 9;
        }

        grant_expect.waiting = pcmbuf_bytes_waiting;
        grant_expect.key = position_key;
        grant_expect.bpf = pcmbuf_bpf;
        grant_expect.byterate = BYTERATE;
        grant_expect.watermark = pcmbuf_watermark;
        grant_expect.armed = true;
        if (pcmbuf_lease_grant())
        {
            st_grants++;
            return true;
        }
        grant_expect.armed = false;
    }
    k = 1 + (int)rnd((unsigned)n);
    {
        size_t off = (uint8_t *)p - view;
        if (off + (size_t)k * bpf > pcmbuf_size)
            st_cpu_wraps++;
    }
    produce(p, k);
    pcmbuf_write_complete(k, produced, 0);
    return false;
}

static void *cpu_main(void *arg)
{
    (void)arg;
    self_core = CPU;
    pthread_mutex_lock(&mtx);
    while (running != CPU && !aborted)
        pthread_cond_wait(&cv, &mtx);
    cur_core = CPU;
    materialize(CPU);
    for (int r = 0; r < rounds && !aborted; r++)
    {
        int tries = 0;

        /* produce until the write side goes to the COP */
        while (!cpu_window(chan == CHANNEL_PLAYING) && ++tries < 4000)
            ;
        /* the CPU's own work while the COP has it */
        for (unsigned i = rnd(300); i; i--)
            point("cpu idle", false, false);
        /* any write-side call takes it back (and may wait) */
        for (int i = 1 + (int)rnd(6); i; i--)
            cpu_window(false);
    }
    cpu_finished = true;
    /* the end: the CPU takes the write side back for good */
    pcmbuf_lease_reclaim();
    thread_end();
    return NULL;
}

/* ---- the COP: codec_cop.c lease_frames() ----------------------------- */
static void *cop_main(void *arg)
{
    (void)arg;
    self_core = COP;
    pthread_mutex_lock(&mtx);
    while (running != COP && !aborted)
        pthread_cond_wait(&cv, &mtx);
    cur_core = COP;
    materialize(COP);
    while (!cpu_finished)
    {
        unsigned still = 0;

        if (lease_raw().owner != LEASE_COP)
        {
            point("cop waits for a grant", true, false);
            continue;
        }
        /* codec_cop.c dsp_xcore_lock(): the COP drops its lines the CPU's
           writes made stale before it writes (not pcmbuf.c's fence) */
        commit_discard_dcache();
        for (;;)
        {
            int n = 1 + (int)rnd(1500), why = 0, k;
            unsigned revoked = lease_raw().revoke;
            void *p = pcmbuf_lease_window(&n, &why);

            if (!p)
            {
                if (why == PCMBUF_LEASE_FULL && ++still < 400)
                {
                    point("cop full", true, true);
                    continue;
                }
                if (lease_raw().revoke)
                    st_backs_revoke++;
                else
                    st_backs_other++;
                break;
            }
            still = 0;
            st_cop_windows++;
            if (revoked)
                fail("a window with a revoke pending (%u)", revoked);
            k = 1 + (int)rnd((unsigned)n);
            {
                size_t off = (uint8_t *)p - view;
                if (off + (size_t)k * bpf > pcmbuf_size)
                    st_cop_wraps++;
            }
            produce(p, k);
            st_cop_commits++;
            pcmbuf_lease_commit(k, produced, 0);
        }
        pcmbuf_lease_release();
    }
    thread_end();
    return NULL;
}

/* ---- one seed -------------------------------------------------------- */
static uint8_t region[300 * 1024] __attribute__((aligned(64)));

static void run_seed(unsigned seed)
{
    pthread_t tc, tp;

    rng = 0x9e3779b97f4a7c15ull ^ ((uint64_t)seed * 0x2545f4914f6cdd1dull);
    rnd(1);
    p_switch = 20 + rnd(300);
    p_fiq = 1 + rnd(25);
    rounds = 2 + (int)rnd(4);

    /* a fresh buffer: the real init on the CPU, caches empty */
    started = false;
    cur_core = CPU;
    self_core = CPU;
    chan = CHANNEL_STOPPED;
    memset(&play_cbs, 0, sizeof(play_cbs));
    pcmbuf_play_stop();
    pcmbuf_init(region + sizeof(region));
    bpf = pcmbuf_bpf;
    view = pcmbuf_buffer;
    m_len = pcmbuf_size + PCMBUF_GUARD_SIZE;
    ring_len = pcmbuf_size;
    n_dirty[CPU] = n_dirty[COP] = 0;
    if (m_len > M_MAX || (uint8_t *)view < region)
    {
        fail("model too small: %zu", m_len);
        return;
    }
    memset(cache, 0, sizeof(cache));
    memset(view, 0, m_len);
    memset(sdram, 0, m_len);
    memset(snap, 0, m_len);
    produced = consumed = 0;
    playing_chunk = false;
    grant_expect.armed = false;
    done[CPU] = done[COP] = false;
    cpu_finished = false;
    aborted = false;
    steps = 0;
    max_steps = 400000;

    running = CPU;
    started = true;
    pthread_create(&tc, NULL, cpu_main, NULL);
    pthread_create(&tp, NULL, cop_main, NULL);
    pthread_join(tc, NULL);
    pthread_join(tp, NULL);
    started = false;
    self_core = cur_core = CPU;
    if (aborted)
    {
        lease_raw().owner = LEASE_CPU;  /* the harness's, after a fail */
        lease_raw().revoke = 0;
        return;
    }
    /* drain: the CPU writes back its partial chunk's bytes, the FIQ
       plays everything published */
    for (int i = 0; i < 4000 && chan == CHANNEL_PLAYING; i++)
        fiq_run();
    if (lease_raw().owner != LEASE_CPU)
        fail("seed %u ends with the lease not the CPU's (%u)", seed,
             lease_raw().owner);
    if (consumed + (uint32_t)(pcmbuf_bytes_waiting / bpf) > produced)
        fail("seed %u: more frames played than made (%u > %u)", seed,
             (unsigned)consumed, (unsigned)produced);
}

int main(int argc, char **argv)
{
    unsigned seeds = argc > 1 ? (unsigned)atoi(argv[1]) : 200;
    unsigned s;

    if (argc > 2)
        depth = (unsigned)atoi(argv[2]);
    for (s = 1; s <= seeds && !fails; s++)
        run_seed(s);
    s--;
    printf("     depth %u: %u seeds, %lu points, %lu switches; FIQ %lu chunks "
           "%lu frames; grants %lu, COP windows %lu commits %lu wraps %lu, "
           "CPU wraps %lu; reclaim waits %lu; COP backs: revoke %lu other "
           "%lu; lease checks %lu\n",
           depth, s, st_points, st_switches, st_fiq_chunks, st_frames,
           st_grants, st_cop_windows, st_cop_commits, st_cop_wraps,
           st_cpu_wraps, st_revoke_waits, st_backs_revoke, st_backs_other,
           st_lease_checks);
    (void)st_stale_hits;
    /* a run that never reached the paths under test proves nothing */
    if (!fails && (st_grants == 0 || st_cop_commits == 0 || st_cop_wraps == 0
                   || st_cpu_wraps == 0 || st_revoke_waits == 0
                   || st_backs_revoke == 0 || st_fiq_chunks == 0
                   || st_lease_checks == 0))
        fail("vacuous: a path under test was never reached");
    if (fails)
        printf("VERDICT: FAIL %d (%s)\n", fails, first_fail);
    else
        printf("VERDICT: PASS\n");
    return fails ? 1 : 0;
}
