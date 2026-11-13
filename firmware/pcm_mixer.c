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
#include "config.h"
#include "system.h"
#include "general.h"
#include "kernel.h"
#include "pcm.h"
#include "pcm-internal.h"
#include "pcm_mixer.h"
#include "pcm_sampr.h"
#if defined(HAVE_IPOD_UI_DEVMODE) || defined(HAVE_VIZ_TAP) || \
    defined(HAVE_COP_COHERENCE)
#include "xcore.h"      /* ProkBox: XCORE_BSS for the tap pointer; P19
                         * ucop-fix: XCORE_ASSERT_CPU (the channel calls
                         * are CPU-only: their control words are cached and
                         * the PCM lock masks the CPU's FIQ only) */
#else
#define XCORE_ASSERT_CPU(what) do { } while (0)
#define XCORE_BSS               /* stock: one core touches the mixer */
#define XCORE_DATA
#endif
#ifdef HAVE_DAC_STANDBY
#include "dac_ext.h"
#include "devstats.h"   /* ProkBox Fit-1 M0: empty unless HAVE_DEVSTATS */
#endif

/* Channels use standard-style PCM callback interface but a latency of one
   frame by double-buffering is introduced in order to facilitate mixing and
   keep the hardware fed. There must be sufficient time to perform operations
   before the last samples are sent to the codec and so things are done in
   parallel (as much as possible) with sending-out data. */

static unsigned int mix_frame_size XCORE_DATA = MIX_FRAME_SAMPLES*4;

/* Define this to nonzero to add a marker pulse at each frame start */
#define FRAME_BOUNDARY_MARKERS 0

/* Descriptor for each channel */
struct mixer_channel
{
    const void *start;                      /* Buffer pointer */
    size_t size;                            /* Bytes remaining */
    size_t last_size;                       /* Size of consumed data in prev. cycle */
    const struct mixer_play_cbs* play_cbs;  /* Registered callbacks */
    enum channel_status status;             /* Playback status */
    uint32_t amplitude;                     /* Amp. factor: 0x0000 = mute, 0x10000 = unity */
    const struct mixer_buffer_cbs* buf_cbs; /* Callback for new buffer */
};

#if defined(HAVE_PCM_24BIT_OUT)
/* ProkBox #268/#270: one IRAM frame size at every rate and depth (1 KB a
   buffer): 88.2/96 kHz and 24-bit frames mix in more, shorter frames
   (96 kHz 24-bit: 128 frames, 1.3 ms) instead of 2 KB more IRAM */
#define FRAME_SIZE_MULT  1
#elif (defined(HW_HAVE_192) || defined(HW_HAVE_176))
#define FRAME_SIZE_MULT  4
#elif (defined(HW_HAVE_96) || defined(HW_HAVE_88))
#define FRAME_SIZE_MULT  2
#else
#define FRAME_SIZE_MULT  1
#endif

#define MAX_MIX_FRAME_SAMPLES  (MIX_FRAME_SAMPLES * FRAME_SIZE_MULT)

/* ProkBox rc.6 (layout A): the PCM's FIQ, and so the mix, runs on the COP
   while the CPU starts, stops and fades channels.  Every mixer variable
   both reach is uncached: the buffers and channel tables in IRAM, the
   scalars XCORE_BSS/DATA (docs/design/cop-shared-state.md, layout A) */

/* Because of the double-buffering, playback is always from here, otherwise a
   mechanism for the channel callbacks not to free buffers too early would be
   needed (if we _really_ want it and it's worth it, we _can_ do that ;-) ) */
static uint32_t downmix_buf[2][MAX_MIX_FRAME_SAMPLES] DOWNMIX_BUF_IBSS MEM_ALIGN_ATTR;
static int downmix_index XCORE_BSS;    /* Which downmix_buf? */
static size_t next_size XCORE_BSS;     /* Size of buffer to play next time */

/* Descriptors for all available channels */
static struct mixer_channel channels[PCM_MIXER_NUM_CHANNELS] IBSS_ATTR;

/* Packed pointer array of all playing (active) channels in "channels" array */
static struct mixer_channel * active_channels[PCM_MIXER_NUM_CHANNELS+1] IBSS_ATTR;

/* Number of silence frames to play after all data has played */
static unsigned int idle_counter XCORE_BSS;

#ifdef HAVE_PCM_24BIT_OUT
/* ProkBox #268: the output's frame format.  mix_s24 (read by the mix, on
   the COP in layout A, so uncached): 8-byte frames, two int32 with the
   24 bits at the top; the playback channel carries that format, every
   other channel (clicks, beeps, voice) stays 16-bit and is widened as it
   is mixed.  mix_depth_want: what playback asked for (mixer_set_depth),
   applied at the next PCM start; the PCM is stopped on every change */
static uint8_t mix_s24 XCORE_BSS;
static uint8_t mix_depth_want XCORE_DATA = 16;
#define MIX_BPF (mix_s24 ? 8 : 4)
#else
#define MIX_BPF 4
#endif

#if defined(HAVE_IPOD_UI_DEVMODE) || defined(HAVE_VIZ_TAP)
/* NULL unless the audio path test or the visualizer's tap is armed;
 * uncached (xcore.h), so an opener on either core sets the copy the FIQ
 * reads */
static mixer_tap_fn mixer_tap XCORE_BSS;

void mixer_set_tap(mixer_tap_fn fn)
{
    /* under the PCM lock: once this returns, no callback is in the old
     * tap (on the PP the lock masks the FIQ on this core; the simulator's
     * audio thread mixes holding it).  CPU only: the FIQ is the CPU's,
     * and the lock's depth is a plain increment (P19 ucop-fix2; a COP
     * caller crosses by remote call, febe_rpc.list) */
    XCORE_ASSERT_CPU("mixer_set_tap");
    pcm_play_lock();
    mixer_tap = fn;
    pcm_play_unlock();
}
#endif

#ifdef CONFIG_SAMPR_TYPES
#define SAMPR_NUM(sampr) (sampr & ~SAMPR_TYPE_MASK)
#else
#define SAMPR_NUM(sampr) (sampr)
#endif

#ifdef HAVE_CORE_LAYOUT_A
/* rc.6: the FIQ (on the COP) must not read the sink's rate through its
   cache (the CPU writes it); the CPU sets this before every start and at
   each rate change, uncached */
static unsigned int mix_idle_max XCORE_BSS;

static void mix_idle_max_set(void)
{
    mix_idle_max = SAMPR_NUM(pcm_get_frequency()) * 3 /
                   (mix_frame_size / MIX_BPF);
}

static inline unsigned int max_idle_frames(void)
{
    return mix_idle_max;
}
#else
static inline unsigned int max_idle_frames(void)
{
    return SAMPR_NUM(pcm_get_frequency()) * 3 / (mix_frame_size / MIX_BPF);
}
#define mix_idle_max_set() do { } while (0)
#endif

/** Mixing routines, CPU optmized **/
#include "asm/pcm-mixer.c"

#ifdef HAVE_PCM_24BIT_OUT
/* #268: the 24-bit mix.  out: n 32-bit samples (24 bits at the top).
   src: the channel's samples, 16-bit (is16) or the same 32-bit form;
   amp: 16.16, at most unity.  The first channel writes, the rest add
   with clipping at 24 bits; the playback channel alone at unity (the
   usual case: volume is the codec's) is a plain copy */
static inline int32_t s24_in(const void *src, int i, bool is16,
                             unsigned int amp)
{
    int32_t x = is16 ? (int32_t)((const int16_t *)src)[i] << 8
                     : ((const int32_t *)src)[i] >> 8;

    if (amp != MIX_AMP_UNITY)
        x = (int32_t)(((int64_t)x * amp) >> 16);
    return x;
}

/* out of IRAM: only 24-bit tracks run them */
static void __attribute__((noinline)) s24_write(void *out, const void *src,
                                                bool is16, unsigned int amp,
                                                size_t size)
{
    int32_t *d = out;
    int n = size / 4;

    if (!is16 && amp == MIX_AMP_UNITY)
    {
        memcpy(out, src, size);
        return;
    }
    for (int i = 0; i < n; i++)
        d[i] = s24_in(src, i, is16, amp) << 8;
}

static void __attribute__((noinline)) s24_add(void *out, const void *src,
                                              bool is16, unsigned int amp,
                                              size_t size)
{
    int32_t *d = out;
    int n = size / 4;

    for (int i = 0; i < n; i++)
    {
        int32_t v = (d[i] >> 8) + s24_in(src, i, is16, amp);

        if (v > 0x7fffff)
            v = 0x7fffff;
        else if (v < -0x800000)
            v = -0x800000;
        d[i] = v << 8;
    }
}

/* A channel's samples are 16-bit in a 24-bit mix: every channel but
   playback's (1: its byte counts are half the output's).  s24: the
   callback's one read of mix_s24 (an uncached word) */
#define chan_shift(chan) \
    (s24 && (chan) != &channels[PCM_MIXER_CHAN_PLAYBACK])
#else
#define chan_shift(chan) 0
#endif

/** Private generic routines **/

/* Mark channel active to mix its data */
static void mixer_activate_channel(struct mixer_channel *chan)
{
    void **elem = find_array_ptr((void **)active_channels, chan);

    if (!*elem)
    {
        idle_counter = 0;
        *elem = chan;
    }
}

/* ProkBox rc.6 (layout A): a channel's source as the mix reads it
   (pcm_xcore.h pcm_mix_src()) */
#ifdef HAVE_CORE_LAYOUT_A
#include "pcm_xcore.h"
#define mix_src(p) pcm_mix_src(p)
#else
#define mix_src(p) (p)
#endif

/* Stop channel from mixing */
static void mixer_deactivate_channel(struct mixer_channel *chan)
{
    remove_array_ptr((void **)active_channels, chan);
}

/* Deactivate channel and change it to stopped state */
static void channel_stopped(struct mixer_channel *chan)
{
    mixer_deactivate_channel(chan);
    chan->size = 0;
    chan->start = NULL;
    chan->status = CHANNEL_STOPPED;
}

/* Main PCM callback - sends the current prepared frame to play */
static void mixer_pcm_callback(const void **addr, size_t *size)
{
    *addr = downmix_buf[downmix_index];
    *size = next_size;
}

static inline void chan_call_buffer_hook(struct mixer_channel *chan)
{
    if (UNLIKELY(chan->buf_cbs && chan->buf_cbs->next_buffer))
        chan->buf_cbs->next_buffer(chan->start, chan->size);
}

/* Buffering callback - calls sub-callbacks and mixes the data for next
   buffer to be sent from mixer_pcm_callback() */
static enum pcm_dma_status MIXER_CALLBACK_ICODE
mixer_buffer_callback(enum pcm_dma_status status)
{
    if (status != PCM_DMAST_STARTED)
        return status;

    downmix_index ^= 1; /* Next buffer */
#ifdef HAVE_PCM_24BIT_OUT
    const bool s24 = mix_s24;
#endif

    void *mixptr = downmix_buf[downmix_index];
    size_t mixsize = mix_frame_size;
    struct mixer_channel **chan_p;

    next_size = 0;

    /* "Loop" back here if one round wasn't enough to fill a frame */
fill_frame:
    chan_p = active_channels;

    while (*chan_p)
    {
        /* Find the active channel with the least data remaining and call any
           callbacks for channels that ran out - stopping whichever report
           "no more" */
        struct mixer_channel *chan = *chan_p;
        chan->start += chan->last_size;
        chan->size -= chan->last_size;

        if (chan->size == 0)
        {
            if (chan->play_cbs && chan->play_cbs->get_more)
            {
                chan->play_cbs->get_more(&chan->start, &chan->size);
                ALIGN_AUDIOBUF(chan->start, chan->size);
                chan->start = mix_src(chan->start);
            }

            if (!(chan->start && chan->size))
            {
                /* Channel is stopping */
                channel_stopped(chan);
                continue;
            }

            chan_call_buffer_hook(chan);
        }

        /* Channel will play for at least part of this frame */

        /* Channel with least amount of data remaining determines the downmix
           size */
        if ((chan->size << chan_shift(chan)) < mixsize)
            mixsize = chan->size << chan_shift(chan);

        chan_p++;
    }

    /* Add all still-active channels to the downmix */
    chan_p = active_channels;

    if (LIKELY(*chan_p))
    {
        struct mixer_channel *chan = *chan_p++;

#ifdef HAVE_PCM_24BIT_OUT
        if (UNLIKELY(s24))
        {
            /* #268: the first channel writes, each other adds; each
               consumes its own bytes for the frame's mixsize */
            s24_write(mixptr, chan->start, chan_shift(chan),
                      chan->amplitude, mixsize);
            chan->last_size = mixsize >> chan_shift(chan);
            while (*chan_p)
            {
                chan = *chan_p++;
                s24_add(mixptr, chan->start, chan_shift(chan),
                        chan->amplitude, mixsize);
                chan->last_size = mixsize >> chan_shift(chan);
            }
        }
        else
#endif
        if (LIKELY(!*chan_p))
        {
            write_samples(mixptr, chan->start, chan->amplitude, mixsize);
        }
        else
        {
            const void *src0, *src1;
            unsigned int amp0, amp1;

            /* Mix first two channels with each other as the downmix */
            src0 = chan->start;
            amp0 = chan->amplitude;
            chan->last_size = mixsize;

            chan = *chan_p++;
            src1 = chan->start;
            amp1 = chan->amplitude;

            while (1)
            {
                mix_samples(mixptr, src0, amp0, src1, amp1, mixsize);

                if (!*chan_p)
                    break;

                /* More channels to mix - mix each with existing downmix */
                chan->last_size = mixsize;
                chan = *chan_p++;
                src0 = mixptr;
                amp0 = MIX_AMP_UNITY;
                src1 = chan->start;
                amp1 = chan->amplitude;
            }
        }

#ifdef HAVE_PCM_24BIT_OUT
        if (!s24)
#endif
        chan->last_size = mixsize;
        next_size += mixsize;

        if (next_size < mix_frame_size)
        {
            /* There is still space remaining in this frame */
            mixptr += mixsize;
            mixsize = mix_frame_size - next_size;
            goto fill_frame;
        }
    }
    else if (idle_counter++ < max_idle_frames())
    {
        /* Pad incomplete frames with silence */
        if (idle_counter <= 3)
            memset(mixptr, 0, mix_frame_size - next_size);

        next_size = mix_frame_size;
    }
    /* else silence period ran out - go to sleep */

#if defined(HAVE_IPOD_UI_DEVMODE) || defined(HAVE_VIZ_TAP)
    if (UNLIKELY(mixer_tap) && next_size)
        mixer_tap(downmix_buf[downmix_index], next_size);
#endif

#if FRAME_BOUNDARY_MARKERS != 0
    if (next_size)
        *downmix_buf[downmix_index] = downmix_index ? 0x7fff7fff : 0x80008000;
#endif

    /* Certain SoC's have to do cleanup */
    mixer_buffer_callback_exit();

    return PCM_DMAST_OK;
}

/* Start PCM driver if it's not currently playing */
static void mixer_start_pcm(void)
{
    if (pcm_is_playing())
        return;

#if defined(HAVE_RECORDING)
    if (pcm_is_recording())
        return;
#endif

#ifdef HAVE_PCM_24BIT_OUT
    /* #268: the PCM is stopped: the format playback asked for, on the
       built-in sink only (another sink takes 16-bit frames) */
    mix_s24 = mix_depth_want > 16 && pcm_current_sink() == PCM_SINK_BUILTIN;
    if (pcm_current_sink() == PCM_SINK_BUILTIN)
        pcm_builtin_set_depth(mix_s24 ? 24 : 16);
#endif
    mix_idle_max_set();

    /* Prepare initial frames and set up the double buffer */
    mixer_buffer_callback(PCM_DMAST_STARTED);

    /* Save the previous call's output */
    void *start = downmix_buf[downmix_index];

    mixer_buffer_callback(PCM_DMAST_STARTED);

    pcm_play_data(mixer_pcm_callback, mixer_buffer_callback,
                  start, mix_frame_size);
}

/* Notify users of samplerate change */
static  void mixer_handle_sampr_change(unsigned int sampr)
{
    int i = 0;
    struct mixer_channel* chan;
    while ((chan = active_channels[i]))
    {
        bool stop_channel = true;
        /* Notify upstreams */
        if (chan->play_cbs)
        {
            if (chan->play_cbs->sampr_changed)
            {
                chan->play_cbs->sampr_changed(sampr);
            }
            if (chan->play_cbs->get_more)
            {
                /* Remake buffer */
                const void *start = NULL;
                size_t size;
                chan->play_cbs->get_more(&start, &size);
                if (start && size) {
                    chan->start = mix_src(start);
                    chan->size = size;
                    chan->last_size = 0;

                    stop_channel = false;
                    ++i;
                }
            }
        }

        if (stop_channel)
        {
            channel_stopped(chan);
        }
        /* Notify buffer monitor */
        else if (chan->buf_cbs)
        {
            if (chan->buf_cbs->sampr_changed)
            {
                chan->buf_cbs->sampr_changed(sampr);
            }
        }
    }

    /* Work out how much space we really need */
#ifdef HAVE_PCM_24BIT_OUT
    (void)sampr;
    mix_frame_size = 1; /* FRAME_SIZE_MULT above: one size for all */
#else
    if (sampr > SAMPR_96)
        mix_frame_size = 4;
    else if (sampr > SAMPR_48)
        mix_frame_size = 2;
    else
        mix_frame_size = 1;
#endif

    mix_frame_size *= MIX_FRAME_SAMPLES * 4;
    mix_idle_max_set();
}

/** Public interfaces **/

/* Start playback on a channel */
void mixer_channel_play_data(enum pcm_mixer_channel channel,
                             const struct mixer_play_cbs* cbs,
                             const void *start, size_t size)
{
    struct mixer_channel *chan = &channels[channel];

    XCORE_ASSERT_CPU("mixer_channel_play_data");
#ifdef HAVE_DAC_STANDBY
    dac_wake();     /* codec clocks back before any PCM start; may sleep on
                     * I2C, so never under pcm_play_lock (P15 review H3) */
#endif
    ALIGN_AUDIOBUF(start, size);

    if (!(start && size) && cbs && cbs->get_more)
    {
        /* Initial buffer not passed - call the callback now */
        pcm_play_lock();
        mixer_deactivate_channel(chan); /* Protect chan struct if active;
                                           may also be same callback which
                                           must not be reentered */
        pcm_play_unlock(); /* Allow playback while doing callback */

        size = 0;
        cbs->get_more(&start, &size);
        ALIGN_AUDIOBUF(start, size);
    }

    pcm_play_lock();

#ifdef HAVE_CORE_LAYOUT_A
    /* rc.6: the first buffer (a click, a beep, a voice frame) was written
       through the CPU's cache; the COP's mix reads SDRAM */
    commit_dcache();
#endif

    if (start && size)
    {
        /* We have data - start the channel */
        chan->status = CHANNEL_PLAYING;
        chan->start = mix_src(start);
        chan->size = size;
        chan->last_size = 0;
        chan->play_cbs = cbs;

        mixer_activate_channel(chan);
        chan_call_buffer_hook(chan);
        mixer_start_pcm();
#ifdef HAVE_DEVSTATS
        if (channel == PCM_MIXER_CHAN_PLAYBACK)
            DEVSTATS_PCM_START();   /* M0: tap to first PCM */
#endif
    }
    else
    {
        /* Never had anything - stop it now */
        channel_stopped(chan);
    }

    pcm_play_unlock();
}

/* Pause or resume a channel (when started) */
void mixer_channel_play_pause(enum pcm_mixer_channel channel, bool play)
{
    struct mixer_channel *chan = &channels[channel];

#ifdef HAVE_DAC_STANDBY
    if (play)
        dac_wake(); /* as in mixer_channel_play_data() */
#endif
    pcm_play_lock();

    if (play == (chan->status == CHANNEL_PAUSED) &&
        chan->status != CHANNEL_STOPPED)
    {
        if (play)
        {
            chan->status = CHANNEL_PLAYING;
            mixer_activate_channel(chan);
            mixer_start_pcm();
#ifdef HAVE_DEVSTATS
            if (channel == PCM_MIXER_CHAN_PLAYBACK)
                DEVSTATS_PCM_START();
#endif
        }
        else
        {
            mixer_deactivate_channel(chan);
            chan->status = CHANNEL_PAUSED;
        }
    }

    pcm_play_unlock();
}

/* Stop playback on a channel */
void mixer_channel_stop(enum pcm_mixer_channel channel)
{
    struct mixer_channel *chan = &channels[channel];

    XCORE_ASSERT_CPU("mixer_channel_stop");
    pcm_play_lock();
    channel_stopped(chan);
    pcm_play_unlock();
}

/* Switch playback sink */
bool mixer_switch_sink(enum pcm_sink_ids sink)
{
    if(pcm_current_sink() == sink)
        return true;

    if(!pcm_switch_sink(sink))
        return false;

    mixer_handle_sampr_change(SAMPR_NUM(pcm_get_frequency()));
    return true;
}

/* Set channel's amplitude factor */
void mixer_channel_set_amplitude(enum pcm_mixer_channel channel,
                                 unsigned int amplitude)
{
    channels[channel].amplitude = MIN(amplitude, MIX_AMP_UNITY);
}

/* Return channel's playback status */
enum channel_status mixer_channel_status(enum pcm_mixer_channel channel)
{
    return channels[channel].status;
}

/* Returns amount data remaining in channel before next callback */
size_t mixer_channel_get_bytes_waiting(enum pcm_mixer_channel channel)
{
    return channels[channel].size;
}

/* Return pointer to channel's playing audio data and the size remaining */
const void * mixer_channel_get_buffer(enum pcm_mixer_channel channel, int *count)
{
    struct mixer_channel *chan = &channels[channel];
    const void * buf = *(const void * volatile *)&chan->start;
    size_t size = *(size_t volatile *)&chan->size;
    const void * buf2 = *(const void * volatile *)&chan->start;

    /* Still same buffer? */
    if (buf == buf2)
    {
#ifdef HAVE_PCM_24BIT_OUT
        /* #268: 16-bit frames only (the peak meter reads int16 pairs) */
        if (mix_s24 && channel == PCM_MIXER_CHAN_PLAYBACK)
        {
            *count = 0;
            return NULL;
        }
#endif
        *count = size >> 2;
        return buf;
    }
    /* else can't be sure buf and size are related */

    *count = 0;
    return NULL;
}

/* Calculate peak values for channel */
void mixer_channel_calculate_peaks(enum pcm_mixer_channel channel,
                                   struct pcm_peaks *peaks)
{
    int count;
    const void *addr = mixer_channel_get_buffer(channel, &count);

    pcm_do_peak_calculation(peaks,
                            channels[channel].status == CHANNEL_PLAYING,
                            addr, count);
}

/* Adjust channel pointer by a given offset to support movable buffers */
void mixer_adjust_channel_address(enum pcm_mixer_channel channel,
                                  off_t offset)
{
    pcm_play_lock();
    /* Makes no difference if it's stopped */
    channels[channel].start += offset;
    pcm_play_unlock();
}

/* Set a hook that is called upon getting a new source buffer for a channel */
void mixer_channel_set_buffer_hook(enum pcm_mixer_channel channel,
                                   const struct mixer_buffer_cbs* cbs)
{
    struct mixer_channel *chan = &channels[channel];

#ifdef HAVE_CORE_LAYOUT_A
    /* ProkBox (0.8.3 review): the hook runs in the PCM's FIQ, which is on
       the COP in layout A, and its only user (the oscilloscope plugin,
       built only with --ipodui-plugins) writes cached plugin statics from
       it: never on the COP (docs/data-paths.md R7 item 8). Refused */
    cbs = NULL;
#endif
    pcm_play_lock();
    chan->buf_cbs = cbs;
    pcm_play_unlock();
}

/* Stop ALL channels and PCM and reset state */
void mixer_reset(void)
{
    pcm_play_stop();

    while (*active_channels)
        channel_stopped(*active_channels);

    idle_counter = 0;
}

/* Set output samplerate */
void mixer_set_frequency(unsigned int samplerate)
{
    if(pcm_get_frequency() == samplerate)
        return;

    pcm_play_stop();
    idle_counter = 0;

    pcm_set_frequency(samplerate);
    mixer_handle_sampr_change(SAMPR_NUM(pcm_get_frequency()));
    if (pcm_is_initialized())
        pcm_apply_settings();
}

#ifdef HAVE_PCM_24BIT_OUT
/* #268: the playback channel's frame format from its next start (16 or
   24); a change stops the PCM, as a rate change does, and the caller
   remakes the playback buffer (playback.c).  CPU */
void mixer_set_depth(int bits)
{
    uint8_t want = bits > 16 ? 24 : 16;

    XCORE_ASSERT_CPU("mixer_set_depth");
    if (want == mix_depth_want)
        return;
    pcm_play_stop();
    idle_counter = 0;
    mix_depth_want = want;
    mixer_handle_sampr_change(SAMPR_NUM(pcm_get_frequency()));
}

int mixer_get_depth(void)
{
    return mix_depth_want;
}

/* The format the output runs in now (the mix's own; 16 when stopped
   before any 24-bit start) */
bool mixer_output_s24(void)
{
    return mix_s24;
}
#endif

/* Get output samplerate */
unsigned int mixer_get_frequency(void)
{
    return pcm_get_frequency();
}
