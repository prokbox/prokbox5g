/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2005 Miika Pekkarinen
 * Copyright (C) 2012 Michael Sevakis
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
#include "rbcodecconfig.h"
#include "platform.h"
#include "dsp_core.h"
#include "dsp_sample_io.h"
#include "dsp_proc_entry.h"
#include "dsp-util.h"
#include <string.h>

#if 0
#include <debug.h>
#else
#undef DEBUGF
#define DEBUGF(...)
#endif

/* May be implemented in here or externally.*/
void sample_output_mono(struct sample_io_data *this,
                        struct dsp_buffer *src, struct dsp_buffer *dst);
void sample_output_stereo(struct sample_io_data *this,
                          struct dsp_buffer *src, struct dsp_buffer *dst);
void sample_output_dithered(struct sample_io_data *this,
                            struct dsp_buffer *src, struct dsp_buffer *dst);

/** Sample output **/

#if !defined(CPU_COLDFIRE) && !defined(CPU_ARM)
/* write mono internal format to output format */
void sample_output_mono(struct sample_io_data *this,
                        struct dsp_buffer *src, struct dsp_buffer *dst)
{
    int count = this->outcount;
    const int32_t *s0 = src->p32[0];
    int16_t *d = dst->p16out;
    int scale = src->format.output_scale;
    int32_t dc_bias = 1L << (scale - 1);

    do
    {
        int32_t lr = clip_sample_16((*s0++ + dc_bias) >> scale);
        *d++ = lr;
        *d++ = lr;
    }
    while (--count > 0);
}
#endif /* CPU */

#if (!defined(CPU_COLDFIRE) && !defined(CPU_ARM)) || defined(CPU_ARM_MICRO)
/* write stereo internal format to output format */
void sample_output_stereo(struct sample_io_data *this,
                          struct dsp_buffer *src, struct dsp_buffer *dst)
{
    int count = this->outcount;
    const int32_t *s0 = src->p32[0];
    const int32_t *s1 = src->p32[1];
    int16_t *d = dst->p16out;
    int scale = src->format.output_scale;
    int32_t dc_bias = 1L << (scale - 1);

    do
    {
        *d++ = clip_sample_16((*s0++ + dc_bias) >> scale);
        *d++ = clip_sample_16((*s1++ + dc_bias) >> scale);
    }
    while (--count > 0);
}
#endif /* CPU */

/**
 * The "dither" code to convert the 24-bit samples produced by libmad was
 * taken from the coolplayer project - coolplayer.sourceforge.net
 *
 * This function handles mono and stereo outputs.
 */
static struct dither_data
{
    struct dither_state
    {
        long error[3];  /* 00h: error term history */
        long random;    /* 0ch: last random value */
    } state[2];         /* 0=left, 1=right */
    bool enabled;       /* 20h: dithered output enabled */
                        /* 24h */
} dither_data IBSS_ATTR;

void sample_output_dithered(struct sample_io_data *this,
                            struct dsp_buffer *src, struct dsp_buffer *dst)
{
    int count = this->outcount;
    int channels = src->format.num_channels;
    int scale = src->format.output_scale;
    int32_t dc_bias = 1L << (scale - 1); /* 1/2 bit of significance */
    int32_t mask = (1L << scale) - 1; /* Mask of bits quantized away */

    for (int ch = 0; ch < channels; ch++)
    {
        struct dither_state *dither = &dither_data.state[ch];

        const int32_t *s = src->p32[ch];
        int16_t *d = &dst->p16out[ch];

        for (int i = 0; i < count; i++, s++, d += 2)
        {
            /* Noise shape and bias (for correct rounding later) */
            int32_t sample = *s;

            sample += dither->error[0] - dither->error[1] + dither->error[2];
            dither->error[2] = dither->error[1];
            dither->error[1] = dither->error[0] / 2;

            int32_t output = sample + dc_bias;

            /* Dither, highpass triangle PDF */
            int32_t random = dither->random*0x0019660dL + 0x3c6ef35fL;
            output += (random & mask) - (dither->random & mask);
            dither->random = random;

            /* Quantize sample to output range */
            output >>= scale;

            /* Error feedback of quantization */
            dither->error[0] = sample - (output << scale);

            /* Clip and store */
            *d = clip_sample_16(output);
        }
    }

    if (channels > 1)
        return;

    /* Have to duplicate left samples into the right channel since
       output is interleaved stereo */
    int16_t *d = dst->p16out;

    do
    {
        int16_t s = *d++;
        *d++ = s;
    }
    while (--count > 0);
}

#if defined(HAVE_IPOD_UI) || defined(WARBLE)
/* Automatic dither (iPod UI shell): dither only when the samples carry
 * more than 16 bits. Lossless and PCM decoders (FLAC, WAV, ALAC...)
 * deliver 16-bit sources shifted up into a 28-bit word, so the format's
 * depth can't tell; the block's own bits can. If no sample has a bit set
 * below the 16-bit LSB, the source is 16-bit and no DSP stage changed it:
 * the exact copy keeps it bit-perfect. Otherwise (a 24-bit or lossy
 * source, or EQ, ReplayGain, timestretch, resampling...) it is dithered.
 * Decided per block, so a stage turning on or off takes effect at once. */
static bool dither_auto_active;

static void sample_output_auto(struct sample_io_data *this,
                               struct dsp_buffer *src, struct dsp_buffer *dst)
{
    int count = this->outcount;
    int channels = src->format.num_channels;
    int32_t mask = (1L << src->format.output_scale) - 1;
    int32_t bits = 0;

    for (int ch = 0; ch < channels && !bits; ch++)
    {
        const int32_t *s = src->p32[ch];
        int i = 0;

        /* 8 at a time; a lossy or 24-bit source stops at the first group */
        for (; i + 8 <= count && !bits; i += 8)
            bits = (s[i] | s[i + 1] | s[i + 2] | s[i + 3] |
                    s[i + 4] | s[i + 5] | s[i + 6] | s[i + 7]) & mask;

        for (; i < count && !bits; i++)
            bits = s[i] & mask;
    }

    if (bits)
    {
        dither_auto_active = true;
        sample_output_dithered(this, src, dst);
        return;
    }

    if (dither_auto_active)
    {
        /* exact samples quantize with no error: drop the old history */
        dither_auto_active = false;
        for (int ch = 0; ch < 2; ch++)
            memset(dither_data.state[ch].error, 0,
                   sizeof (dither_data.state[ch].error));
    }

    if (channels > 1)
        sample_output_stereo(this, src, dst);
    else
        sample_output_mono(this, src, dst);
}
#endif /* HAVE_IPOD_UI || WARBLE */

/* Initialize the output function for settings and format */
void dsp_sample_output_format_change(struct sample_io_data *this,
                                     struct sample_format *format)
{
    static const sample_output_fn_type fns[2][2] =
    {
        { sample_output_mono,        /* DC-biased quantizing */
          sample_output_stereo },
        { sample_output_dithered,    /* Tri-PDF dithering */
          sample_output_dithered },
    };

    bool dither = dsp_get_id((void *)this) == CODEC_IDX_AUDIO &&
                  dither_data.enabled;
    int channels = format->num_channels;

    DSP_PRINT_FORMAT(DSP Output, *format);

    this->output_samples = fns[dither ? 1 : 0][channels - 1];
#if defined(HAVE_IPOD_UI) || defined(WARBLE)
    if (!dither && dsp_get_id((void *)this) == CODEC_IDX_AUDIO)
        this->output_samples = sample_output_auto;
#endif
    this->output_version = format->version;
}

void dsp_sample_output_init(struct sample_io_data *this)
{
    this->output_version = 0;
    this->output_samples = sample_output_stereo;
}

/* Flush the dither history */
void dsp_sample_output_flush(struct sample_io_data *this)
{
    if (dsp_get_id((void *)this) == CODEC_IDX_AUDIO)
        memset(dither_data.state, 0, sizeof (dither_data.state));
}


/** Output settings **/

/* Set the tri-pdf dithered output */
void dsp_dither_enable(bool enable)
{
    if (enable == dither_data.enabled)
        return;

    dither_data.enabled = enable;
    struct sample_io_data *data = (void *)dsp_get_config(CODEC_IDX_AUDIO);

    if (enable)
        dsp_sample_output_flush(data);

    data->output_version = 0; /* Force format update */
}
