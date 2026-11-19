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

/* tests/mixerfirst: everything firmware/pcm_mixer.c needs from outside,
 * for the host. Every stub header of this directory includes this one;
 * run.sh copies them beside pcm_mixer.c so its quoted includes find them
 * before the firmware's own. The PP's two cores are one variable
 * (stub_core), set by the test: the CPU starts channels, the COP runs the
 * PCM's FIQ (layout A) */
#ifndef MIXERFIRST_STUB_H
#define MIXERFIRST_STUB_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>

/* config: a PP5022 in layout A, the generic C mix (no CPU_ARM) */
#define PP5002 5002
#define PP5022 5022
#define CONFIG_CPU PP5022
#define CPU_PP
#define NUM_CORES 2
#define HAVE_CORE_LAYOUT_A
#define HW_SAMPR_CAPS 0
#define SAMPR_44 44100
#define SAMPR_48 48000
#define SAMPR_96 96000
#define HW_SAMPR_DEFAULT SAMPR_44
#define PLAY_SAMPR_MAX SAMPR_48
#define IBSS_ATTR
#define ICODE_ATTR
#define MEM_ALIGN_ATTR __attribute__((aligned(16)))
#define FORCE_INLINE inline __attribute__((always_inline))
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define UNCACHED_BASE_ADDR 0x10000000u
/* the host has no alias: the mix reads the buffer as it is */
#define UNCACHED_ADDR(p) (p)

enum { STUB_CPU = 0, STUB_COP = 1 };
extern int stub_core;               /* the core running now */

/* pcm.h / pcm_sink.h / pcm-internal.h, as much as the mixer uses */
enum pcm_dma_status {
    PCM_DMAST_ERR_DMA = -1,
    PCM_DMAST_OK = 0,
    PCM_DMAST_STARTED = 1,
};
typedef void (*pcm_play_callback_type)(const void **start, size_t *size);
typedef enum pcm_dma_status (*pcm_status_callback_type)(
    enum pcm_dma_status status);
struct pcm_peaks { uint32_t left, right; long period, tick; };
enum pcm_sink_ids { PCM_SINK_BUILTIN = 0, PCM_SINK_NUM };
#define ALIGN_AUDIOBUF(start, size) \
    ({ (start) = (void *)(((uintptr_t)(start) + 3) & ~3); \
       (size) &= ~3; })

void pcm_play_lock(void);
void pcm_play_unlock(void);
void pcm_play_data(pcm_play_callback_type get_more,
                   pcm_status_callback_type status_cb,
                   const void *start, size_t size);
void pcm_play_stop(void);
bool pcm_is_playing(void);
bool pcm_is_recording(void);
bool pcm_is_initialized(void);
unsigned int pcm_get_frequency(void);
void pcm_set_frequency(unsigned int samplerate);
void pcm_apply_settings(void);
bool pcm_switch_sink(enum pcm_sink_ids sink);
enum pcm_sink_ids pcm_current_sink(void);
void pcm_do_peak_calculation(struct pcm_peaks *peaks, bool active,
                             const void *addr, int count);

/* system / general / dac */
void commit_dcache(void);
void **find_array_ptr(void **arr, void *ptr);
int remove_array_ptr(void **arr, void *ptr);
#define HAVE_DAC_STANDBY
void dac_wake(void);

/* dsp-util.h */
static inline int32_t clip_sample_16(int32_t s)
{
    return s > 32767 ? 32767 : s < -32768 ? -32768 : s;
}
#endif
