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
 *    codec's buffer (COPJ_CALL_WROTE); id3: its uncached alias.
 *  - out: PCM: pcmbuf_insert() reads the codec's output through its
 *    uncached alias after the COP wrote back its cache (every call does).
 *  - the api struct the codec sees (ci_cop) is uncached; the CPU copies
 *    the playback fields into it after every call.
 *
 * P19 19-1c (docs/phase19-brief.md): decoding is pinned to the CPU (owner,
 * 2026-10-24): by default no load is bound here, the codec gets the plain
 * api, no stack is kept back and MP3's synthesis runs inline. Two
 * developer A/B modes for M-2 bind the load: the COP always (Fit-2), or
 * the core chosen per run by the screen state (apps/codec_place_core.c:
 * dark on the CPU, lit on the COP), moved at a track boundary without a
 * reload; a run on the CPU then calls straight through and keeps ci_cop's
 * fields in step.
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
#include "codec_place_core.h"   /* P19 19-1c: the core per run */
#include "backlight.h"
#include "lcd.h"
#include "ipodui/uilog.h"
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
/* Owner, 2026-10-24: decoding is pinned to the CPU; the screen rule and
 * the COP are developer A/B modes for M-2 only */
static int place_mode = CPL_MODE_CPU;
static struct cpl_state place;      /* the codec thread's only */
static uint32_t runs, cpu_runs;

void codec_cop_set_enabled(bool on)
{
    enabled = on;
}

bool codec_cop_enabled(void)
{
    return enabled;
}

bool codec_cop_available(void)
{
    return cop_jobs_cross_core();
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
    if (bound)                  /* the pinned default: nothing to watch */
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
    /* The COP reads id3 from memory; the CPU (a run on the CPU, the entry
     * points) through its own cache, as without the switch (19-1c (i) 4:
     * per run, not per worker) */
    ci_cop.id3       = on_cop ? cop_uncached(real->id3) : real->id3;
    ci_cop.audio_hid = real->audio_hid;
    ci_cop.dsp       = real->dsp;
}

#define FROM_COP() cop_job_in_worker(&worker)

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
    p = (void *)cop_job_call(&worker, h_get_buffer, io, 0);
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

static void t_pcmbuf_insert(const void *ch1, const void *ch2, int count)
{
    intptr_t io[COP_CALL_IO] = { (intptr_t)ch1, (intptr_t)ch2, count };

    if (!FROM_COP())
    {
        real->pcmbuf_insert(ch1, ch2, count);
        sync_fields();
    }
    else
        cop_job_call(&worker, h_pcmbuf_insert, io, 0);
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
    else
        cop_job_call(&worker, h_set_elapsed, io, 0);
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

    if (!FROM_COP())
    {
        size_t n = real->read_filebuf(ptr, size);
        sync_fields();          /* 19-1c (i) 3: curpos moved */
        return n;
    }
    /* The CPU wrote the codec's buffer through the uncached alias: the
     * COP drops its lines after */
    return (size_t)cop_job_call(&worker, h_read_filebuf, io,
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
    p = (void *)cop_job_call(&worker, h_request_buffer, io, 0);
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
    else
        cop_job_call(&worker, h_advance_buffer, io, 0);
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
    return cop_job_call(&worker, h_seek_buffer, io, 0) != 0;
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
        cop_job_call(&worker, h_seek_complete, io, 0);
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
        cop_job_call(&worker, h_set_offset, io, 0);
}

static intptr_t h_configure(intptr_t *io)
{
    /* Every setting a decoder passes is a value, not a pointer */
    real->configure((int)io[0], io[1]);
    return 0;
}

static void t_configure(int setting, intptr_t value)
{
    intptr_t io[COP_CALL_IO] = { setting, value };

    if (!FROM_COP())
        real->configure(setting, value);
    else
        cop_job_call(&worker, h_configure, io, 0);
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
    if (param)
        io[0] = *param;
    action = cop_job_call(&worker, h_get_command, io, 0);
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
    return cop_job_call(&worker, h_loop_track, io, 0) != 0;
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
        cop_job_call(&worker, h_strip_filesize, io, 0);
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
        cop_job_call(&worker, h_panic, io, 0);
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
        cop_job_call(&worker, h_logf, io, 0);
}
#endif

/** codec_thread.c's side **/

static bool fresh;                  /* no run since the load */
static bool fresh_log;              /* no place line since the load */

struct codec_api *codec_cop_select(struct codec_api *api, bool encoder)
{
    real = api;
    fresh = true;
    fresh_log = true;
    on_cop = false;
    /* 19-1c (i) 1: bound whenever the COP could take a run, apart from
     * which core the first run takes */
    bound = enabled && !encoder && codec_cop_available()
            && place_mode != CPL_MODE_CPU;
#ifdef HAVE_BCMFW_VIDEO
    /* A video's soundtrack stays on the CPU: with the switch on, session
     * 36's stage 7 saw it stand still under the picture (the bcm session,
     * 2026-10-15; cause open, fit2-brief.md). The video's sound is the
     * VideoCore's in the end; the COP was only ever its interim. */
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
 * pinned default, or no COP): the plain path, synthesis inline */
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
    }
    fresh_log = false;
    last_cop = on_cop;
}

static void run_job(void *arg)
{
    (void)arg;
    run_status = codec_run_proc();
}

/* Runs on the CPU, through the struct the codec was given */
static int run_here(void)
{
    on_cop = false;
    cpu_runs++;
    if (bound)
    {
        sync_fields();          /* the cached id3, the fields run_codec set */
        log_place();
    }
    return codec_run_proc();
}

int codec_cop_run(bool track_start)
{
    int t;

    choose(track_start);
    if (!on_cop)
        return run_here();

    if (!worker.started
        && !cop_worker_start(&worker, codec_slot_top(COP_CODEC_STACK),
                             COP_CODEC_STACK, "codec cop", COP_CODEC_PRIO))
    {
        /* No COP: the codec runs here, through the same struct */
        return run_here();
    }

    sync_fields();
    log_place();
    /* In: the loaded image and statics, id3 and the fields (the CPU writes
     * back). Out: the codec's statics, for its unload on the CPU or the
     * next run there */
    t = cop_job_post(&worker, run_job, NULL, COPJ_CODE | COPJ_IN | COPJ_OUT);
    if (t < 0)
        return run_here();
    cop_job_wait(&worker, t);
    runs++;
    return run_status;
}

void codec_cop_unload(void)
{
    cop_worker_stop(&worker);
    bound = false;
    on_cop = false;
}

void codec_cop_place_stats(uint32_t *cop, uint32_t *cpu)
{
    *cop = runs;
    *cpu = cpu_runs;
}

#endif /* HAVE_COP_CODEC */
