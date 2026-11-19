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

/* #283 (b): a beep's first frame is made by the mix, on the COP.
 * firmware/pcm_mixer.c itself (layout A), on stubs (stub/stub.h): the CPU
 * starts the beep channel with no first buffer while the PCM plays; the
 * channel's get_more must not run on the CPU, and the next mix (the PCM's
 * FIQ, on the COP) must fetch it and mix it in. With the PCM stopped (owner
 * Q6, 2026-11-12) the PCM starts on two silent frames and the FIQ mixes
 * from the third: no get_more and no mix on the CPU, for a beep, for music
 * (pcmbuf's start) and for a click given its first buffer. A channel given
 * its first buffer while the PCM plays plays as before. */

#include <stdio.h>
#include <stdlib.h>
#include "pcm_mixer.c"          /* the real one, copied beside the stubs */
#include "beep.c"               /* apps/beep.c, the real one too */

static int fails;
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
    fails++; } } while (0)

/* ---- stubs ------------------------------------------------------------- */

int stub_core;
static int lock_depth, lock_bad;
static bool playing;
static pcm_play_callback_type pl_cb;
static pcm_status_callback_type st_cb;
static const void *dma_start;       /* the buffer pcm_play_data() starts on */
static size_t dma_size;

void pcm_play_lock(void) { lock_depth++; }
void pcm_play_unlock(void) { if (--lock_depth < 0) lock_bad++; }
void pcm_play_data(pcm_play_callback_type get_more,
                   pcm_status_callback_type status_cb,
                   const void *start, size_t size)
{
    dma_start = start;
    dma_size = size;
    pl_cb = get_more;
    st_cb = status_cb;
    playing = true;
}
void pcm_play_stop(void) { playing = false; }
bool pcm_is_playing(void) { return playing; }
bool pcm_is_recording(void) { return false; }
bool pcm_is_initialized(void) { return true; }
unsigned int pcm_get_frequency(void) { return SAMPR_44; }
void pcm_set_frequency(unsigned int s) { (void)s; }
void pcm_apply_settings(void) { }
bool pcm_switch_sink(enum pcm_sink_ids s) { (void)s; return true; }
enum pcm_sink_ids pcm_current_sink(void) { return PCM_SINK_BUILTIN; }
void pcm_do_peak_calculation(struct pcm_peaks *p, bool a, const void *s,
                             int n)
{
    (void)p; (void)a; (void)s; (void)n;
}
void commit_dcache(void) { }
void dac_wake(void) { }
void **find_array_ptr(void **arr, void *ptr)
{
    while (*arr && *arr != ptr)
        arr++;
    return arr;
}
int remove_array_ptr(void **arr, void *ptr)
{
    void **p = find_array_ptr(arr, ptr);

    if (!*p)
        return -1;
    do
        p[0] = p[1];
    while (*p++);
    return 0;
}

/* ---- sources ----------------------------------------------------------- */

#define TONE 1000
static int16_t tone[MIX_FRAME_SAMPLES * 2];        /* one frame's worth */
static int more_cpu, more_cop, more_left;
static void tone_more(const void **s, size_t *n)
{
    if (stub_core == STUB_CPU)
        more_cpu++;
    else
        more_cop++;
    if (lock_depth && stub_core == STUB_COP)
        lock_bad++;                 /* the FIQ never runs inside the lock */
    if (more_left <= 0)
        return;
    more_left--;
    *s = tone;
    *n = sizeof(tone);
}
static const struct mixer_play_cbs tone_cbs = { tone_more, NULL };

static int16_t music[MIX_FRAME_SAMPLES * 8];        /* silence */
static void music_more(const void **s, size_t *n)
{
    *s = music;
    *n = sizeof(music);
}
static const struct mixer_play_cbs music_cbs = { music_more, NULL };

/* the FIQ: one mix (the next frame), on the COP; its first sample */
static int16_t fiq_mix(void)
{
    const void *addr = NULL;
    size_t size = 0;
    int save = stub_core;

    stub_core = STUB_COP;
    st_cb(PCM_DMAST_STARTED);
    pl_cb(&addr, &size);
    stub_core = save;
    return addr && size ? ((const int16_t *)addr)[0] : -1;
}

/* The driver's order (pcm-pp.c fiq_playback()): the piece ended, so
 * get_more names the next buffer, then the status callback mixes the one
 * after it; on the COP. Returns the next buffer's first sample */
static int16_t dma_next(void)
{
    const void *addr = NULL;
    size_t size = 0;
    int save = stub_core;

    stub_core = STUB_COP;
    pl_cb(&addr, &size);
    st_cb(PCM_DMAST_STARTED);
    stub_core = save;
    return addr && size ? ((const int16_t *)addr)[0] : -1;
}

/* every sample of a buffer is v */
static bool all_are(const void *buf, size_t size, int16_t v)
{
    const int16_t *p = buf;

    for (size_t i = 0; i < size / 2; i++)
        if (p[i] != v)
            return false;
    return true;
}

static void reset(void)
{
    stub_core = STUB_CPU;
    mixer_channel_stop(PCM_MIXER_CHAN_BEEP);
    mixer_channel_stop(PCM_MIXER_CHAN_PLAYBACK);
    playing = false;
    more_cpu = more_cop = 0;
    mixer_channel_set_amplitude(PCM_MIXER_CHAN_BEEP, MIX_AMP_UNITY);
    mixer_channel_set_amplitude(PCM_MIXER_CHAN_PLAYBACK, MIX_AMP_UNITY);
}

int main(void)
{
    int16_t s;
    int i;

    for (i = 0; i < MIX_FRAME_SAMPLES * 2; i++)
        tone[i] = TONE;

    /* warm: music plays; a beep with no first buffer is the mix's */
    reset();
    mixer_channel_play_data(PCM_MIXER_CHAN_PLAYBACK, &music_cbs, music,
                            sizeof(music));
    CHECK(playing);
    more_left = 3;
    mixer_channel_play_data(PCM_MIXER_CHAN_BEEP, &tone_cbs, NULL, 0);
    printf("warm: get_more on the CPU %d, on the COP %d at the start\n",
           more_cpu, more_cop);
    CHECK(more_cpu == 0);           /* nothing made on the CPU */
    CHECK(mixer_channel_status(PCM_MIXER_CHAN_BEEP) == CHANNEL_PLAYING);
    s = fiq_mix();
    CHECK(more_cop == 1);           /* the first frame, in the FIQ */
    CHECK(s == TONE);               /* mixed in at once: no frame lost */
    for (i = 0; i < 8; i++)
        fiq_mix();
    CHECK(more_cpu == 0 && more_cop == 4);  /* 3 frames, then "no more" */
    CHECK(mixer_channel_status(PCM_MIXER_CHAN_BEEP) == CHANNEL_STOPPED);
    CHECK(fiq_mix() == 0);          /* the music alone again */

    /* cold (Q6): the PCM is stopped; it starts on two silent frames and
     * the beep's first frame is fetched and mixed by the FIQ, on the COP,
     * into the third */
    reset();
    more_left = 2;
    mixer_channel_play_data(PCM_MIXER_CHAN_BEEP, &tone_cbs, NULL, 0);
    CHECK(playing);
    CHECK(mixer_channel_status(PCM_MIXER_CHAN_BEEP) == CHANNEL_PLAYING);
    printf("cold beep: get_more on the CPU %d at the start\n", more_cpu);
    CHECK(more_cpu == 0);
    CHECK(dma_start && dma_size && all_are(dma_start, dma_size, 0));
    s = dma_next();                 /* frame 2: silent, the COP mixes 3 */
    CHECK(s == 0 && more_cop == 1);
    s = dma_next();                 /* frame 3: the beep */
    CHECK(s == TONE);
    CHECK(more_cpu == 0);

    /* cold music (pcmbuf_play_start(): no first buffer): the same */
    reset();
    more_left = 4;
    mixer_channel_play_data(PCM_MIXER_CHAN_PLAYBACK, &tone_cbs, NULL, 0);
    CHECK(playing && more_cpu == 0);
    CHECK(all_are(dma_start, dma_size, 0));
    CHECK(dma_next() == 0);
    CHECK(dma_next() == TONE && more_cpu == 0 && more_cop >= 1);
    printf("cold music: get_more on the CPU %d, on the COP %d\n",
           more_cpu, more_cop);

    /* cold click (a first buffer given): not mixed on the CPU either */
    reset();
    mixer_channel_play_data(PCM_MIXER_CHAN_BEEP, NULL, tone, sizeof(tone));
    CHECK(playing && more_cpu == 0);
    CHECK(all_are(dma_start, dma_size, 0));
    CHECK(dma_next() == 0);
    CHECK(dma_next() == TONE);

    /* a first buffer given (the click): plays as before, nothing asked */
    reset();
    mixer_channel_play_data(PCM_MIXER_CHAN_PLAYBACK, &music_cbs, music,
                            sizeof(music));
    more_left = 0;
    mixer_channel_play_data(PCM_MIXER_CHAN_BEEP, NULL, tone, sizeof(tone));
    CHECK(more_cpu == 0 && more_cop == 0);
    s = fiq_mix();
    CHECK(s == TONE);

    /* apps/beep.c's beep_play(): no frame on the CPU (beep_count is
     * whole after the call), the FIQ makes the first; warm and cold */
    {
        const int want = BEEP_COUNT(SAMPR_44, 20);

        reset();
        mixer_channel_play_data(PCM_MIXER_CHAN_PLAYBACK, &music_cbs, music,
                                sizeof(music));
        beep_play(1000, 20, 3000);
        printf("beep.c warm: %d of %d samples made on the CPU\n",
               want - beep_count, want);
        CHECK(beep_count == want);
        CHECK(mixer_channel_status(PCM_MIXER_CHAN_BEEP) == CHANNEL_PLAYING);
        dma_next();                 /* the COP's mix makes the first */
        CHECK(beep_count < want);

        reset();
        beep_play(1000, 20, 3000);
        printf("beep.c cold: %d of %d samples made on the CPU\n",
               want - beep_count, want);
        CHECK(playing && beep_count == want);
        CHECK(all_are(dma_start, dma_size, 0));
        CHECK(dma_next() == 0 && beep_count < want);
        CHECK(dma_next() == 3000);  /* the square wave's first half */
        mixer_channel_stop(PCM_MIXER_CHAN_BEEP);
    }

    CHECK(lock_bad == 0 && lock_depth == 0);
    if (fails)
    {
        fprintf(stderr, "%d check(s) failed\n", fails);
        return 1;
    }
    return 0;
}
