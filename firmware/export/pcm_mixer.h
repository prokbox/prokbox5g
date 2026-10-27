/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2011 by Michael Sevakis
 * Modified for ProkBox5g (2026); see docs/changes-from-rockbox.md.
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

#ifndef PCM_MIXER_H
#define PCM_MIXER_H

#include <sys/types.h>
#include "pcm.h"
#include "pcm_sink.h"

/** Simple config **/

/* Length of PCM frames (always) */
#if CONFIG_CPU == PP5002
/* There's far less time to do mixing because HW FIFOs are short */
#define MIX_FRAME_SAMPLES 64
#elif (CONFIG_CPU == JZ4760B) || (CONFIG_CPU == JZ4732)
/* These MIPS32r1 targets have a very high interrupt latency, which
   unfortunately causes a lot of audio underruns under even moderate load */
#define MIX_FRAME_SAMPLES 2048
#elif defined(DX50) || defined(DX90)
/* iBasso Devices: Match Rockbox PCM buffer size to ALSA PCM buffer size
   to minimize memory transfers. */
#define MIX_FRAME_SAMPLES 2048
#elif defined(SIMULATOR) && defined(SIM_DEVICE_MIXER)
/* ProkBox: opt-in (-DSIM_DEVICE_MIXER) device-sized frames in the simulator,
   so the sim mixes in the same 256-sample steps as the iPod (docs/sim-fidelity.md) */
#define MIX_FRAME_SAMPLES 256
#elif (CONFIG_PLATFORM & PLATFORM_HOSTED)
/* Hosted targets need larger buffers for decent performance due to
   OS locking/scheduling overhead */
#define MIX_FRAME_SAMPLES 1024
#else
/* Assume HW DMA engine is available or sufficient latency exists in the
   PCM pathway */
#define MIX_FRAME_SAMPLES 256
#endif

#if defined(CPU_COLDFIRE) ||  defined(CPU_PP)
/* For Coldfire, it's just faster
   For PortalPlayer, this also avoids more expensive cache coherency */
#define DOWNMIX_BUF_IBSS        IBSS_ATTR
#else
/* Otherwise can't DMA from IRAM, IRAM is pointless or worse */
#define DOWNMIX_BUF_IBSS
#endif

#if defined(CPU_COLDFIRE) || defined(CPU_PP)
#define MIXER_CALLBACK_ICODE    ICODE_ATTR
#else
#define MIXER_CALLBACK_ICODE
#endif


/** Definitions **/

/* Channels are preassigned for simplicity */
enum pcm_mixer_channel
{
    PCM_MIXER_CHAN_PLAYBACK = 0,
#if defined(USB_ENABLE_AUDIO) || defined(HAVE_HOST_USB_AUDIO)
    PCM_MIXER_CHAN_USBAUDIO,
#endif
    PCM_MIXER_CHAN_VOICE,
#ifndef HAVE_HARDWARE_BEEP
    PCM_MIXER_CHAN_BEEP,
#endif
    /* Add new channel indexes above this line */
    PCM_MIXER_NUM_CHANNELS,
};

/* Channel playback states */
enum channel_status
{
    CHANNEL_STOPPED = 0,
    CHANNEL_PLAYING,
    CHANNEL_PAUSED,
};

#define MIX_AMP_UNITY    0x00010000
#define MIX_AMP_MUTE     0x00000000


/** Public interfaces **/

/* Start playback on a channel */
struct mixer_play_cbs {
    void (*get_more)(const void **start, size_t *size);
    void (*sampr_changed)(uint32_t sampr);
};

void mixer_channel_play_data(enum pcm_mixer_channel channel,
                             const struct mixer_play_cbs* cbs,
                             const void *start, size_t size);

/* Pause or resume a channel (when started) */
void mixer_channel_play_pause(enum pcm_mixer_channel channel, bool play);

/* Stop playback on a channel */
void mixer_channel_stop(enum pcm_mixer_channel channel);

/* Switch playback sink */
bool mixer_switch_sink(enum pcm_sink_ids sink);

/* Set channel's amplitude factor */
void mixer_channel_set_amplitude(enum pcm_mixer_channel channel,
                                 unsigned int amplitude);

/* Return channel's playback status */
enum channel_status mixer_channel_status(enum pcm_mixer_channel channel);

/* Returns amount data remaining in channel before next callback */
size_t mixer_channel_get_bytes_waiting(enum pcm_mixer_channel channel);

/* Return pointer to channel's playing audio data and the size remaining */
const void * mixer_channel_get_buffer(enum pcm_mixer_channel channel,
                                      int *count);

/* Calculate peak values for channel */
void mixer_channel_calculate_peaks(enum pcm_mixer_channel channel,
                                   struct pcm_peaks *peaks);

/* Adjust channel pointer by a given offset to support movable buffers */
void mixer_adjust_channel_address(enum pcm_mixer_channel channel,
                                  off_t offset);

struct mixer_buffer_cbs {
    /* Called for each buffer, not each mixer chunk */
    void (*next_buffer)(const void *start, size_t size);
    void (*sampr_changed)(uint32_t sampr);
};

/* Set a hook that is called upon getting a new source buffer for a channel */
void mixer_channel_set_buffer_hook(enum pcm_mixer_channel channel,
                                   const struct mixer_buffer_cbs* cbs);

/* Stop ALL channels and PCM and reset state */
void mixer_reset(void);

/* Set output samplerate */
void mixer_set_frequency(unsigned int samplerate);

/* Get output samplerate */
unsigned int mixer_get_frequency(void);

#if defined(HAVE_IPOD_UI_DEVMODE) || defined(HAVE_VIZ_TAP)
/* The shell's "Audio path test" (docs/phase15-brief.md section 3.5) and
 * the visualizer's PCM tap (apps/ipodui/viz_tap.c, HAVE_VIZ_TAP): while
 * a tap is set, it sees every frame the mixer sends to the PCM sink
 * (interleaved 16-bit stereo, size in bytes), from the PCM callback (FIQ
 * on PP5022), just after the frame is mixed and one frame before it
 * plays: it must be quick and must not block. One tap at a time (the
 * test runs from the developer menu, the visualizer from Now Playing).
 * Set under the PCM lock: once mixer_set_tap(NULL) returns on the CPU,
 * no callback is in the old tap. */
typedef void (*mixer_tap_fn)(const void *frames, size_t size);
void mixer_set_tap(mixer_tap_fn fn);
#endif

#endif /* PCM_MIXER_H */
