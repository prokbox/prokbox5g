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

/* tests/fencepcm: the one stub environment behind every stub header that
 * the real apps/pcmbuf.c includes (config.h, system.h, kernel.h, pcm.h,
 * ...). The device's configuration for the paths under test: layout A,
 * the codec on the COP (the write lease), the shell, crossfade compiled
 * in, the 24-bit output path. The cache operations and the scheduling
 * points are the test's (test_fencepcm.c). */
#ifndef FENCE_ENV_H
#define FENCE_ENV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>

/* ---- config ---------------------------------------------------------- */
#define HAVE_COP_CODEC
#define HAVE_CORE_LAYOUT_A
#define HAVE_IPOD_UI
#define HAVE_CROSSFADE
#define HAVE_PCM_24BIT_OUT
#define HAVE_SCHED_BANDS
#define MEMORYSIZE          64
#define PLATFORM_NATIVE     1
#define CONFIG_PLATFORM     0       /* no cpu.h */
#define NUM_CORES           2
#define CPU                 0
#define COP                 1
#define HZ                  100

/* ---- system ---------------------------------------------------------- */
#define CACHEALIGN_SIZE     16      /* the PP5022's line */
#define MEM_ALIGN_SIZE      16
#define UNCACHED_BASE_ADDR  0       /* one alias on the host: the model */
#define UNCACHED_ADDR(a)    (a)     /* keeps the cores' views apart */
#define FORCE_INLINE        inline __attribute__((always_inline))
#define ALIGN_DOWN(n, a)    ((typeof(n))((uintptr_t)(n) / (a) * (a)))
#define MIN(a, b)           ((a) < (b) ? (a) : (b))
#define MAX(a, b)           ((a) > (b) ? (a) : (b))
#define TIME_AFTER(a, b)    ((long)(b) - (long)(a) < 0)
#define TIME_BEFORE(a, b)   TIME_AFTER(b, a)
#define IBSS_ATTR
#define XCORE_BSS
#define XCORE_DATA
#define MEM_ALIGN_ATTR
#define logf(...)           do { } while (0)
#define DEBUGF(...)         do { } while (0)

extern long current_tick;
void commit_dcache(void);
void commit_discard_dcache(void);
void trigger_cpu_boost(void);
void sleep(int ticks);
int tick_add_task(void (*f)(void));
int tick_remove_task(void (*f)(void));
void pcm_play_lock(void);
void pcm_play_unlock(void);
#define SKIPBYTES(p, n)     ((typeof(p))((uint8_t *)(p) + (n)))
static inline int16_t clip_sample_16(int32_t s)
{
    return s > 32767 ? 32767 : s < -32768 ? -32768 : (int16_t)s;
}
void fence_assert_cpu(const char *where);
#define XCORE_ASSERT_CPU(w) fence_assert_cpu(w)

/* the instrumented copy's scheduling points (run.sh adds the two defines
 * after the declarations; nothing else in the file changes) */
struct pcmbuf_lease;
struct pcmbuf_lease *fence_lease_at(struct pcmbuf_lease *l);
size_t *fence_widx_at(size_t *p);

/* ---- mixer, pcm ------------------------------------------------------ */
#define MIX_AMP_UNITY       0x00010000
#define MIX_AMP_MUTE        0x00000000
enum pcm_mixer_channel { PCM_MIXER_CHAN_PLAYBACK = 0, PCM_MIXER_CHAN_VOICE,
                         PCM_MIXER_NUM_CHANNELS };
enum channel_status { CHANNEL_STOPPED = 0, CHANNEL_PLAYING, CHANNEL_PAUSED };
struct mixer_play_cbs {
    void (*get_more)(const void **start, size_t *size);
    void (*sampr_changed)(uint32_t sampr);
};
enum channel_status mixer_channel_status(enum pcm_mixer_channel ch);
void mixer_channel_play_data(enum pcm_mixer_channel ch,
                             const struct mixer_play_cbs *cbs,
                             const void *start, size_t size);
void mixer_channel_stop(enum pcm_mixer_channel ch);
void mixer_channel_play_pause(enum pcm_mixer_channel ch, bool play);
void mixer_channel_set_amplitude(enum pcm_mixer_channel ch, unsigned int amp);
unsigned int mixer_get_frequency(void);
unsigned int mixer_get_depth(void);
bool pcm_is_playing(void);

/* ---- dsp, playback, settings ----------------------------------------- */
#define CODEC_IDX_AUDIO     0
#define CODEC_IDX_VOICE     1
#define DSP_SET_OUT_FREQUENCY 1
struct dsp_config;
struct dsp_config *dsp_get_config(int idx);
intptr_t dsp_configure(struct dsp_config *dsp, unsigned int setting,
                       intptr_t value);
void dsp_set_output_depth(struct dsp_config *dsp, int bits);

enum { CROSSFADE_ENABLE_OFF = 0, CROSSFADE_ENABLE_AUTOSKIP,
       CROSSFADE_ENABLE_MANSKIP, CROSSFADE_ENABLE_SHUFFLE,
       CROSSFADE_ENABLE_SHUFFLE_OR_MANSKIP, CROSSFADE_ENABLE_ALWAYS };
struct user_settings {
    int crossfade_fade_out_delay, crossfade_fade_out_duration;
    int crossfade_fade_in_delay, crossfade_fade_in_duration;
    int crossfade_fade_out_mixmode;
    bool playlist_shuffle;
};
extern struct user_settings global_settings;

void audio_start_trace_mark(int step);
void audio_start_trace_pcm(void);
void ui_log(const char *tag, const char *fmt, ...);
void codec_thread_set_priority(int prio);
void voice_thread_set_priority(int prio);

#endif /* FENCE_ENV_H */
