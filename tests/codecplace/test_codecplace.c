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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "codecs.h"
#include "cop_job.h"
#include "codec_cop.h"
#include "codec_place_core.h"

static int fails;

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
    fails++; } } while (0)

/* ---- stubs for codec_cop.c ------------------------------------------- */

long current_tick;
static bool bl_on = true, panel_on = true, cross = true, in_worker;
static unsigned char slot[0x10000];
static int worker_starts, posts;

void commit_dcache(void) { }
void yield(void) { }
void panicf(const char *fmt, ...) { (void)fmt; abort(); }
void ui_log(const char *tag, const char *fmt, ...)
{
    (void)tag; (void)fmt;
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
    w->started = 1;
    worker_starts++;
    return true;
}
void cop_worker_stop(struct cop_worker *w) { w->started = 0; }
int cop_job_post(struct cop_worker *w, cop_job_fn fn, void *arg,
                 unsigned flags)
{
    (void)flags;
    if (!w->started)
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
bool codec_queue_idle(void) { return true; }

/* ---- the real api (codecs.c's ci) and a fake codec --------------------- */

static struct codec_api real;
static struct codec_api *seen;      /* what the codec was handed */
static char id3_obj[64];

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
static long r_command(intptr_t *param) { (void)param; return 0; }
static void r_strip(off_t v) { real.filesize = v; }
static void r_seek_complete(void) { }

static void real_init(void)
{
    memset(&real, 0, sizeof(real));
    real.filesize = 100000;
    real.id3 = (struct mp3entry *)id3_obj;
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

int codec_run_proc(void)
{
    size_t got;
    bool ok = true;

    last.in_worker = in_worker;
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
    seen->pcmbuf_insert(NULL, NULL, 10);
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
    int starts;

    /* the default (owner, 2026-10-24): decoding pinned to the CPU, the
     * plain path: the real api, nothing reserved, synthesis inline, no
     * worker, lit or dark */
    CHECK(codec_cop_place() == CPL_MODE_CPU);
    at(10, true, true);
    load();
    CHECK(seen == &real);
    CHECK(codec_cop_reserve() == 0);
    codec_cop_run(true);
    CHECK(!last.in_worker && !codec_cop_active() && !last.assist);
    CHECK(last.fields_ok);
    at(20, false, false);
    current_tick = 20 + 10 * CPL_HOLD_S * HZ;
    codec_cop_run(true);
    CHECK(!last.in_worker && !last.assist);
    codec_cop_unload();
    CHECK(worker_starts == 0 && posts == 0);

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
    CHECK(cop == 4 && cpu == 5);
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
}

int main(void)
{
    test_rule();
    test_full_wait();
    test_cop_runs();
    if (fails)
    {
        fprintf(stderr, "%d check(s) failed\n", fails);
        return 1;
    }
    return 0;
}
