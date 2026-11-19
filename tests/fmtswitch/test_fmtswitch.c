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

/* Host test of the seamless output-format switch (#214) after the b15
 * sitting (b16): the REAL apps/pcmbuf.c (run.sh copies it here as
 * pcmbuf_fmt.c, in mutation mode with one mutant) on tests/fencepcm's
 * stub headers, one thread: the codec's writes, the PCM callback (the
 * FIQ), the CPU's tick and the audio thread's queue, each called in turn.
 *
 * For 44.1 kHz/16 -> 96 kHz/24 and back, each with and without a flush
 * between the next track's first write and the output reaching it:
 *  - rate: from the audio thread's pcmbuf_set_write_format() on, the
 *    rate the codec's DSP is set to at its run's start
 *    (pcmbuf_dsp_out_frequency(), codec_thread.c run_codec()) is the
 *    next track's, while the output still plays the old one; it stays
 *    so across a flush, the re-clock and the mixer's rate callback;
 *  - the switch: the output stops at the first chunk of the new format,
 *    the tick (or, after a flush, the start) asks the audio thread once,
 *    and the switch re-clocks to the new format;
 *  - the request flag: a request dropped with the audio queue by a stop
 *    (Q_AUDIO_STOP clears it) must not stop the next boundary, or the
 *    next start, from asking again.
 * Prints what it checked and one verdict line, "VERDICT: PASS" or
 * "VERDICT: FAIL n (first failure)". */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "fence_env.h"

/* the switch's calls into the mixer and playback, which fence_env.h
 * leaves out (its suite runs without HAVE_PLAY_FREQ) */
void mixer_set_frequency(unsigned int samplerate);
void mixer_set_depth(int bits);

#include "pcmbuf_fmt.c"

/* ---- results --------------------------------------------------------- */
static int fails, checks;
static char first_fail[256];

static void fail(const char *fmt, ...)
{
    va_list ap;

    if (!fails)
    {
        va_start(ap, fmt);
        vsnprintf(first_fail, sizeof(first_fail), fmt, ap);
        va_end(ap);
    }
    fails++;
}

#define CHECK(cond, ...) do { checks++; if (!(cond)) fail(__VA_ARGS__); } \
                         while (0)

/* ---- the mixer, the DSP, the audio thread's queue -------------------- */
long current_tick;
struct user_settings global_settings;
static enum channel_status chan = CHANNEL_STOPPED;
static struct mixer_play_cbs play_cbs;
static unsigned int mix_freq = 44100, mix_depth = 16;
static intptr_t dsp_out_freq;
static int dsp_out_depth;
static int queued;          /* Q_AUDIO_FMT_SWITCH posts waiting */
static int posts;           /* every post */

enum channel_status mixer_channel_status(enum pcm_mixer_channel ch)
{
    return ch == PCM_MIXER_CHAN_PLAYBACK ? chan : CHANNEL_STOPPED;
}

bool pcm_is_playing(void) { return chan == CHANNEL_PLAYING; }

void mixer_channel_play_data(enum pcm_mixer_channel ch,
                             const struct mixer_play_cbs *cbs,
                             const void *start, size_t size)
{
    (void)ch; (void)start; (void)size;
    play_cbs = *cbs;
    chan = CHANNEL_PLAYING;
}

void mixer_channel_stop(enum pcm_mixer_channel ch)
{
    (void)ch;
    chan = CHANNEL_STOPPED;
}

void mixer_channel_play_pause(enum pcm_mixer_channel ch, bool play)
{
    (void)ch; (void)play;
}

void mixer_channel_set_amplitude(enum pcm_mixer_channel ch, unsigned int a)
{
    (void)ch; (void)a;
}

unsigned int mixer_get_frequency(void) { return mix_freq; }
unsigned int mixer_get_depth(void) { return mix_depth; }

/* the real mixer tells the playing channel of a new rate */
void mixer_set_frequency(unsigned int f)
{
    if (f == mix_freq)
        return;
    mix_freq = f;
    if (play_cbs.sampr_changed)
        play_cbs.sampr_changed(f);
}

void mixer_set_depth(int bits) { mix_depth = (unsigned)bits; }

struct dsp_config *dsp_get_config(int idx) { (void)idx; return NULL; }

intptr_t dsp_configure(struct dsp_config *dsp, unsigned int s, intptr_t v)
{
    (void)dsp;
    if (s == DSP_SET_OUT_FREQUENCY)
        dsp_out_freq = v;
    return 0;
}

void dsp_set_output_depth(struct dsp_config *dsp, int bits)
{
    (void)dsp;
    dsp_out_depth = bits;
}

void audio_pcmbuf_fmt_switch_post(void) { queued++; posts++; }
size_t audio_pcmbuf_size_byterate(void) { return 96000 * 8; }
void audio_pcmbuf_position_callback(unsigned long e, off_t o, unsigned int k)
{
    (void)e; (void)o; (void)k;
}
void audio_pcmbuf_track_change(bool pcmbuf) { (void)pcmbuf; }
void audio_pcmbuf_track_change_fiq(void) { }
bool audio_pcmbuf_may_play(void) { return true; }
void audio_pcmbuf_sync_position(void) { }

void commit_dcache(void) { }
void commit_discard_dcache(void) { }
void trigger_cpu_boost(void) { }
void sleep(int ticks) { current_tick += ticks; }
int tick_add_task(void (*f)(void)) { (void)f; return 0; }
int tick_remove_task(void (*f)(void)) { (void)f; return 0; }
void pcm_play_lock(void) { }
void pcm_play_unlock(void) { }
void fence_assert_cpu(const char *where) { (void)where; }
struct pcmbuf_lease *fence_lease_at(struct pcmbuf_lease *l) { return l; }
size_t *fence_widx_at(size_t *p) { return p; }
void audio_start_trace_mark(int step) { (void)step; }
void audio_start_trace_pcm(void) { }
void ui_log(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
void codec_thread_set_priority(int p) { (void)p; }
void voice_thread_set_priority(int p) { (void)p; }

/* ---- the actors ------------------------------------------------------ */
struct fmt { unsigned long rate; int depth; };

/* the codec: ms of audio at the write side's format */
static void codec_write(int ms)
{
    int depth;
    unsigned long rate = pcmbuf_write_format(&depth);
    long frames = (long)(rate * ms / 1000);

    while (frames > 0)
    {
        int count = (int)MIN(frames, 1024);
        void *p = pcmbuf_request_buffer(&count);

        if (!p || count <= 0)
        {
            fail("no room for the codec's write");
            return;
        }
        if (count > frames)
            count = (int)frames;    /* it offers all it has */
        memset(p, 0x11, (size_t)count * PCMBUF_SAMPLE_SIZE);
        pcmbuf_write_complete(count, 0, 0);
        frames -= count;
    }
}

/* the PCM callback until the output stops (a boundary or dry); returns
 * the chunks taken */
static int fiq_until_stop(void)
{
    int n = 0;

    while (chan == CHANNEL_PLAYING && n < 100000)
    {
        const void *start = NULL;
        size_t size = 0;

        play_cbs.get_more(&start, &size);
        if (!size)
        {
            chan = CHANNEL_STOPPED;     /* the mixer stops a dry channel */
            break;
        }
        n++;
    }
    return n;
}

/* the audio thread takes its queue (Q_AUDIO_FMT_SWITCH) */
static void audio_thread(void)
{
    while (queued > 0)
    {
        queued--;
        pcmbuf_fmt_switch();
    }
}

/* the codec thread's run start (codec_thread.c run_codec()) */
static void codec_run_start(void)
{
    dsp_configure(NULL, DSP_SET_OUT_FREQUENCY, pcmbuf_dsp_out_frequency());
}

static uint8_t region[4 * 1024 * 1024] __attribute__((aligned(64)));

static void fresh(struct fmt a)
{
    chan = CHANNEL_STOPPED;
    memset(&play_cbs, 0, sizeof(play_cbs));
    queued = 0;
    mix_freq = (unsigned)a.rate;
    mix_depth = (unsigned)a.depth;
    pcmbuf_play_stop();
    pcmbuf_init(region + sizeof(region));
}

/* one switch a -> b; flush: a flush (a seek's) after the next track's
 * first write, before the output reaches it */
static void run_switch(struct fmt a, struct fmt b, bool flush)
{
    unsigned long sw0;
    int depth, p0;
    char tag[64];

    snprintf(tag, sizeof(tag), "%lu/%d -> %lu/%d%s", a.rate, a.depth,
             b.rate, b.depth, flush ? " (flush)" : "");
    fresh(a);
    codec_run_start();
    CHECK(dsp_out_freq == (intptr_t)a.rate, "%s: first run at %ld",
          tag, (long)dsp_out_freq);
    codec_write(400);
    pcmbuf_play_start();
    CHECK(chan == CHANNEL_PLAYING, "%s: did not start", tag);

    /* the codec moves on: the audio thread sets the next format, then
       the codec thread starts the next track's run */
    pcmbuf_set_write_format(b.rate, b.depth);
    codec_run_start();
    CHECK(mix_freq == a.rate, "%s: the output moved early", tag);
    CHECK(dsp_out_freq == (intptr_t)b.rate,
          "%s: the next track's run converts to %ld Hz, not %lu "
          "(the output's rate, the b15 bug)", tag, (long)dsp_out_freq,
          b.rate);
    CHECK(dsp_out_depth == b.depth, "%s: DSP depth %d", tag,
          dsp_out_depth);
    codec_write(300);

    sw0 = pcmbuf_fmt_switches();
    p0 = posts;
    if (flush)
    {
        pcmbuf_play_stop();             /* a seek in the next track */
        codec_run_start();              /* its run goes on at its format */
        CHECK(dsp_out_freq == (intptr_t)b.rate, "%s: after the flush "
              "the DSP converts to %ld Hz", tag, (long)dsp_out_freq);
        codec_write(300);
        pcmbuf_play_start();            /* asks for the switch */
        CHECK(chan == CHANNEL_STOPPED, "%s: started at the old format",
              tag);
    }
    else
    {
        fiq_until_stop();               /* stops at the boundary */
        fmt_tick();
        fmt_tick();                     /* asks once */
    }
    CHECK(posts == p0 + 1, "%s: %d switch requests, want 1", tag,
          posts - p0);
    audio_thread();
    CHECK(pcmbuf_fmt_switches() == sw0 + 1, "%s: no switch", tag);
    CHECK(mix_freq == b.rate && mix_depth == (unsigned)b.depth,
          "%s: output at %u/%u", tag, mix_freq, mix_depth);
    CHECK(chan == CHANNEL_PLAYING, "%s: not restarted", tag);
    CHECK(dsp_out_freq == (intptr_t)b.rate, "%s: the re-clock moved the "
          "DSP to %ld Hz", tag, (long)dsp_out_freq);
    CHECK(pcmbuf_write_format(&depth) == b.rate && depth == b.depth,
          "%s: write side moved", tag);
    fiq_until_stop();
}

/* a stop drops a queued request with the audio queue: the next boundary
 * must ask again */
static void run_dropped(struct fmt a, struct fmt b)
{
    char tag[64];
    int p0;

    snprintf(tag, sizeof(tag), "dropped request %lu -> %lu", a.rate,
             b.rate);
    fresh(a);
    codec_write(400);
    pcmbuf_play_start();
    pcmbuf_set_write_format(b.rate, b.depth);
    codec_write(300);
    fiq_until_stop();
    fmt_tick();                         /* posted ... */
    CHECK(queued == 1, "%s: no request at the boundary", tag);
    pcmbuf_play_stop();                 /* ... and a stop: Q_AUDIO_STOP */
    queued = 0;                         /* clears the queue */

    /* a fresh start at the old format, then the same boundary again */
    pcmbuf_set_write_format(a.rate, a.depth);
    codec_write(400);
    pcmbuf_play_start();
    CHECK(chan == CHANNEL_PLAYING, "%s: the next start did not play", tag);
    pcmbuf_set_write_format(b.rate, b.depth);
    codec_write(300);
    fiq_until_stop();
    p0 = posts;
    fmt_tick();
    CHECK(posts == p0 + 1, "%s: the next boundary never asked (the output "
          "would wait there forever)", tag);
    audio_thread();
    CHECK(chan == CHANNEL_PLAYING && mix_freq == b.rate,
          "%s: not switched (%u Hz)", tag, mix_freq);

    /* and a start at another format after such a stop asks too */
    fresh(a);
    codec_write(400);
    pcmbuf_play_start();
    pcmbuf_set_write_format(b.rate, b.depth);
    codec_write(300);
    fiq_until_stop();
    fmt_tick();
    pcmbuf_play_stop();
    queued = 0;
    codec_write(300);                   /* the write side stays at b */
    p0 = posts;
    pcmbuf_play_start();
    CHECK(posts == p0 + 1, "%s: a start at another format never asked",
          tag);
    audio_thread();
    CHECK(chan == CHANNEL_PLAYING && mix_freq == b.rate,
          "%s: the start did not switch (%u Hz)", tag, mix_freq);
}

int main(void)
{
    static const struct fmt f44 = { 44100, 16 }, f96 = { 96000, 24 },
                            f48 = { 48000, 16 };
    int flush;

    for (flush = 0; flush <= 1; flush++)
    {
        run_switch(f44, f96, flush);
        run_switch(f96, f44, flush);
        run_switch(f48, f96, flush);    /* the b15 seam96 row's */
    }
    run_dropped(f44, f96);
    run_dropped(f96, f44);

    printf("     %d checks, %lu switches (44.1/16 <-> 96/24, 48/16 -> "
           "96/24, with and without a flush; a dropped request)\n",
           checks, pcmbuf_fmt_switches());
    if (!fails && (checks < 80 || pcmbuf_fmt_switches() < 10))
        fail("vacuous: %d checks, %lu switches", checks,
             pcmbuf_fmt_switches());
    if (fails)
        printf("VERDICT: FAIL %d (%s)\n", fails, first_fail);
    else
        printf("VERDICT: PASS\n");
    return fails ? 1 : 0;
}
