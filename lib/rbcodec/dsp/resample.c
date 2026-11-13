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
 * Copyright (C) 2013 Michael Giacomelli
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
#include "rbcodecconfig.h"
#include "platform.h"
#include "fracmul.h"
#include "fixedpoint.h"
#include "dsp_proc_entry.h"
#include "dsp_misc.h"
#include "resample.h"
#include "resample_hb.h"
#include "resample_hb_coefs.h"
#include "resample_fir.h"
#include <string.h>

/**
 * Linear interpolation resampling that introduces a one sample delay because
 * of our inability to look into the future at the end of a frame.
 */

#if 1 /* Set to '0' to enable debug messages */
#undef DEBUGF
#define DEBUGF(...)
#endif

#define RESAMPLE_BUF_COUNT 192 /* Per channel, per DSP */

/* CODEC_IDX_AUDIO = left and right, CODEC_IDX_VOICE = mono */
static int32_t resample_out_bufs[3][RESAMPLE_BUF_COUNT] IBSS_ATTR;

/* Data for each resampler on each DSP */
static struct resample_data
{
    uint32_t delta;         /* 00h: Phase delta for each step in s15.16*/
    uint32_t phase;         /* 04h: Current phase [pos16|frac16] */
    int32_t  history[2][3]; /* 08h: Last samples for interpolation (L+R)
                                    0 = oldest, 2 = newest */
                            /* 20h */
    unsigned int frequency;         /* Virtual input samplerate */
    unsigned int frequency_out;     /* Resampler output samplerate */
    struct dsp_buffer resample_buf; /* Buffer descriptor for resampled data */
    int32_t *resample_out_p[2];     /* Actual output buffer pointers */
} resample_data[DSP_COUNT] IBSS_ATTR;

/* Actual worker function. Implemented here or in target assembly code. */
int resample_hermite(struct resample_data *data, struct dsp_buffer *src,
                     struct dsp_buffer *dst);

/** Band-limited conversion for every ratio (#271; the whole-number ratios
 * came first in P19 19-13; docs/design/resample-plan.md sections 8 and 9).
 * - /2, /4, /8: a cascade of half-band polyphase IIR decimators;
 * - x2, x4: half-band interpolators;
 * - anything else (x3, x6, 3:2, 44.1 <-> 48 kHz, pitch, odd rates): the
 *   fractional path. Half-band /2 stages while the rate is still at least
 *   twice the output's; a half-band x2, which leaves nothing above 0.2268 of
 *   its output rate A; then a short polyphase FIR (resample_fir.h) from A to
 *   B, where B is the output rate, or twice it followed by one more
 *   half-band /2 when the source is faster than the output. The half-bands
 *   do the sharp filtering, so the FIR only has to remove its own images
 *   (10 taps for 110 dB). Its phase is exact rational arithmetic: no drift.
 * Only the audio DSP uses it; the voice DSP keeps Hermite, as does
 * RESAMPLE_Q_HERMITE (the fallback for CPU starvation). */
enum
{
    RS_HERMITE = 0, /* plain Hermite (voice DSP, fallback) */
    RS_DOWN,        /* nst half-band decimators */
    RS_UP,          /* nst half-band interpolators */
    RS_FRAC,        /* npre decimators, x2, FIR, post decimators */
};

#define RS_TMP_COUNT   96   /* cascade scratch, per channel */
#define RS_MAX_STAGES  5    /* RS_FRAC: 3 before, x2, 1 after */
#define RS_FBUF_COUNT  (RS_FIR_TAPS + RS_TMP_COUNT) /* FIR input, per ch */
#define RS_FIR_BATCH   16   /* FIR outputs per kernel call */

static unsigned int rs_quality = RESAMPLE_Q_INTEGER;

static struct resample_bl
{
    uint8_t mode;
    uint8_t nst;                    /* RS_DOWN, RS_UP: stages, 1..3 */
    uint8_t npre;                   /* RS_FRAC: decimators before the x2 */
    uint8_t post;                   /* RS_FRAC: one decimator after */
    uint8_t npend;                  /* inputs held for a /2^n group */
    uint8_t nodd;                   /* RS_FRAC post: FIR output held */
    uint8_t cached;                 /* RS_FRAC: rs_rows[ph] are the rows */
    int16_t fill;                   /* RS_FRAC: samples in rs_fbuf */
    int16_t w;                      /* RS_FRAC: FIR window start */
    uint32_t ph;                    /* RS_FRAC: phase, ph / L sample */
    uint32_t L, mint, mfrac;        /* RS_FRAC: step = mint + mfrac / L */
    uint32_t recip_lo, recip_hi;    /* RS_FRAC: ceil(2^40 / L) */
    const struct hb_coefs *hb[RS_MAX_STAGES];   /* [0] meets the input */
    int32_t pend[2][8];
    int32_t odd[2];
} rs_bl IBSS_ATTR;

/* Half-band states [stage][channel]: each stage call loads and saves its
 * state once, so they need not take IRAM. Cascade scratch, FIR input queue
 * and FIR coefficient rows (the cached rows of a ratio with
 * L <= RS_FIR_BATCH, or one batch's rows). All of it is touched only by the
 * core that runs the audio DSP. */
static int32_t rs_st[RS_MAX_STAGES][2][HB_STATE];
static int32_t rs_tmp_a[2][RS_TMP_COUNT];
static int32_t rs_tmp_b[2][RS_TMP_COUNT];
static int32_t rs_fbuf[2][RS_FBUF_COUNT];
static int32_t rs_rows[RS_FIR_BATCH][RS_FIR_TAPS];

static void rs_bl_flush(void)
{
    rs_bl.npend = 0;
    rs_bl.nodd = 0;
    rs_bl.ph = 0;
    rs_bl.w = 0;
    /* the first output is centred on the first input sample */
    rs_bl.fill = RS_FIR_TAPS / 2 - 1;
    memset(rs_fbuf, 0, sizeof (rs_fbuf));
    memset(rs_st, 0, sizeof (rs_st));
}

/* The FIR's fractional position for phase ph, as a Q32 fraction */
static inline uint32_t rs_frac_q32(uint32_t ph)
{
    uint64_t r = ((uint64_t)rs_bl.recip_hi << 32) | rs_bl.recip_lo;
    return (uint32_t)(((uint64_t)ph * r) >> 8);
}

static unsigned int rs_gcd(unsigned int a, unsigned int b)
{
    while (b)
    {
        unsigned int t = a % b;
        a = b;
        b = t;
    }
    return a;
}

/* Decimator for a /2 stage whose input runs at fs. The last stage before a
 * 44.1/48 kHz output is the tight design for its family; the others only
 * have to stay flat to 20 kHz, and the faster their rate, the gentler. */
static const struct hb_coefs *rs_down_design(unsigned int fs, bool last,
                                             unsigned int fout)
{
    if (last && fout <= 48000)
        return (fout % 11025) == 0 ? &hb_t44 : &hb_t48;
    if (fs >= 174000)
        return &hb_r;
    if (fs >= 96000)
        return &hb_t48;
    return &hb_t44;
}

/* Pick the band-limited path for fin -> fout, or RS_HERMITE */
static void rs_bl_setup(unsigned int fin, unsigned int fout)
{
    struct resample_bl *b = &rs_bl;

    b->mode = RS_HERMITE;
    rs_bl_flush();

    if (rs_quality == RESAMPLE_Q_HERMITE || fin == 0 || fout == 0 ||
        fin == fout)
        return;

    for (int k = 1; k <= 3; k++)
    {
        if (fin == fout << k)
        {
            b->mode = RS_DOWN;
            b->nst = k;
            for (int s = 0; s < k; s++)
                b->hb[s] = rs_down_design(fin >> s, s == k - 1, fout);
            return;
        }
    }

    if (fout == 2*fin || fout == 4*fin)
    {
        b->mode = RS_UP;
        b->nst = fout == 2*fin ? 1 : 2;
        b->hb[0] = &hb_t44;
        b->hb[1] = &hb_r;
        return;
    }

    /* Fractional: decimate while the source is at least twice the output */
    int a = 0;
    while ((uint64_t)fin >= ((uint64_t)fout << (a + 1)))
    {
        if (a == 3)
        {
            DEBUGF("RESAMPLE: %u -> %u beyond the band-limited path\n",
                   fin, fout);
            return; /* 16x and more: Hermite */
        }
        b->hb[a] = rs_down_design(fin >> a, false, fout);
        a++;
    }
    b->npre = a;
    b->hb[a] = &hb_t44;                        /* x2 */
    b->post = (uint64_t)fin > ((uint64_t)fout << a);
    if (b->post)
        b->hb[a + 1] = rs_down_design(2*fout, true, fout);

    /* FIR from A = 2 fin / 2^a to B = fout (x2 with post): step A / B */
    uint64_t num = 2 * (uint64_t)fin;
    uint64_t den = (uint64_t)(fout << b->post) << a;
    uint64_t g = rs_gcd((uint32_t)num, (uint32_t)den); /* both < 2^22 */
    b->L = den / g;
    b->mint = (num / g) / b->L;
    b->mfrac = (num / g) % b->L;
    uint64_t r = ((1ull << 40) + b->L - 1) / b->L;
    b->recip_lo = (uint32_t)r;
    b->recip_hi = (uint32_t)(r >> 32);

    b->cached = b->L <= RS_FIR_BATCH;
    if (b->cached)
    {
        for (uint32_t p = 0; p < b->L; p++)
            rs_fir_row(rs_rows[p], rs_frac_q32(p));
    }
    b->mode = RS_FRAC;
}

/* n outputs of the /2^nst cascade hb[0..nst-1] on one channel. dst is
 * never rs_tmp_a/b; the stages in between alternate between them. */
static void rs_cascade(int32_t *dst, const int32_t *src, int n, int ch,
                       int nst)
{
    struct resample_bl *b = &rs_bl;
    const int32_t *in = src;
    for (int s = 0; s < nst; s++)
    {
        int32_t *o = s == nst - 1 ? dst :
                     ((nst - 2 - s) & 1) ? rs_tmp_b[ch] : rs_tmp_a[ch];
        hb_down2(o, in, n << (nst - 1 - s), rs_st[s][ch], b->hb[s]);
        in = o;
    }
}

/* Up to maxn outputs of the /2^nst cascade into d[ch] + 0.., taking input
 * from src at *usedp; a partial group waits in pend for the next buffer. */
static int rs_down_pull(int32_t *const *d, int maxn, struct dsp_buffer *src,
                        int *usedp, int nch, int nst)
{
    struct resample_bl *b = &rs_bl;
    int g = 1 << nst, rem = src->remcount, used = *usedp, out = 0;

    if (b->npend)
    {
        /* finish the group begun in the last buffer */
        int need = MIN(g - b->npend, rem - used);
        for (int ch = 0; ch < nch; ch++)
            for (int i = 0; i < need; i++)
                b->pend[ch][b->npend + i] = src->p32[ch][used + i];
        b->npend += need;
        used += need;
        if (b->npend < g)
        {
            *usedp = used;
            return 0;
        }
        for (int ch = 0; ch < nch; ch++)
            rs_cascade(d[ch], b->pend[ch], 1, ch, nst);
        b->npend = 0;
        out = 1;
    }

    while (out < maxn)
    {
        int n = MIN((rem - used) >> nst, maxn - out);
        n = MIN(n, RS_TMP_COUNT >> (nst - 1));
        if (n <= 0)
            break;
        for (int ch = 0; ch < nch; ch++)
            rs_cascade(d[ch] + out, src->p32[ch] + used, n, ch, nst);
        used += n << nst;
        out += n;
    }

    if (out < maxn && rem - used < g)
    {
        /* keep the tail (less than a group) for the next buffer */
        int left = rem - used;
        for (int ch = 0; ch < nch; ch++)
            for (int i = 0; i < left; i++)
                b->pend[ch][i] = src->p32[ch][used + i];
        b->npend = left;
        used = rem;
    }

    *usedp = used;
    return out;
}

/* RS_FRAC: FIR outputs while they fit, refilling the FIR's queue */
static int rs_frac_run(struct dsp_buffer *src, struct dsp_buffer *dst,
                       int nch)
{
    struct resample_bl *b = &rs_bl;
    const int up = b->npre, post = b->npre + 1;
    int rem = src->remcount, used = 0, out = 0;

    for (;;)
    {
        while (out < RESAMPLE_BUF_COUNT && b->w + RS_FIR_TAPS <= b->fill)
        {
            uintptr_t tag[RS_FIR_BATCH];   /* row | samples to move on */
            int space = RESAMPLE_BUF_COUNT - out;
            int kmax = MIN(b->post ? 2*space - b->nodd : space,
                           RS_FIR_BATCH);
            int w0 = b->w, wl = w0, k = 0;

            while (k < kmax && b->w + RS_FIR_TAPS <= b->fill)
            {
                const int32_t *row = rs_rows[k];
                if (b->cached)
                    row = rs_rows[b->ph];
                else
                    rs_fir_row(rs_rows[k], rs_frac_q32(b->ph));
                tag[k] = (uintptr_t)row | (uintptr_t)(b->w - wl);
                wl = b->w;
                b->ph += b->mfrac;
                b->w += b->mint;
                if (b->ph >= b->L)
                {
                    b->ph -= b->L;
                    b->w++;
                }
                k++;
            }

            if (!b->post)
            {
                for (int ch = 0; ch < nch; ch++)
                    rs_fir_batch(dst->p32[ch] + out, rs_fbuf[ch] + w0,
                                 tag, k);
                out += k;
            }
            else
            {
                /* pairs at twice the output rate through the last /2 */
                int tot = b->nodd + k, np = tot >> 1;
                for (int ch = 0; ch < nch; ch++)
                {
                    int32_t *stg = rs_tmp_b[ch];
                    stg[0] = b->odd[ch];
                    rs_fir_batch(stg + b->nodd, rs_fbuf[ch] + w0, tag, k);
                    if (np)
                        hb_down2(dst->p32[ch] + out, stg, np,
                                 rs_st[post][ch], b->hb[post]);
                    b->odd[ch] = stg[tot - 1];
                }
                b->nodd = tot & 1;
                out += np;
            }
        }

        if (out >= RESAMPLE_BUF_COUNT)
            break;

        /* refill: drop what the FIR has passed, then x2 more input in */
        if (b->w > 0)
        {
            for (int ch = 0; ch < nch; ch++)
                memmove(rs_fbuf[ch], rs_fbuf[ch] + b->w,
                        (b->fill - b->w) * sizeof (int32_t));
            b->fill -= b->w;
            b->w = 0;
        }

        int n;
        if (b->npre == 0)
        {
            n = MIN((RS_FBUF_COUNT - b->fill) / 2, rem - used);
            if (n <= 0)
                break;
            for (int ch = 0; ch < nch; ch++)
                hb_up2(rs_fbuf[ch] + b->fill, src->p32[ch] + used, n,
                       rs_st[up][ch], b->hb[up]);
            used += n;
        }
        else
        {
            /* the decimators' output waits at the queue's far end, clear
             * of the x2's output */
            int nmax = (RS_FBUF_COUNT - b->fill) / 3;
            int32_t *d[2] = { rs_fbuf[0] + RS_FBUF_COUNT - nmax,
                              rs_fbuf[1] + RS_FBUF_COUNT - nmax };
            n = rs_down_pull(d, nmax, src, &used, nch, b->npre);
            if (n <= 0)
                break; /* all input taken (a partial group is held) */
            for (int ch = 0; ch < nch; ch++)
                hb_up2(rs_fbuf[ch] + b->fill, d[ch], n, rs_st[up][ch],
                       b->hb[up]);
        }
        b->fill += 2*n;
    }

    dst->remcount = out;
    return used;
}

/* Band-limited worker; same contract as resample_hermite() */
static int resample_bl(struct resample_data *data, struct dsp_buffer *src,
                       struct dsp_buffer *dst)
{
    struct resample_bl *b = &rs_bl;
    int nch = src->format.num_channels;
    int used = 0, out = 0;
    (void)data;

    if (b->mode == RS_DOWN)
    {
        out = rs_down_pull(dst->p32, RESAMPLE_BUF_COUNT, src, &used, nch,
                           b->nst);
    }
    else if (b->mode == RS_UP)
    {
        int n = MIN(src->remcount, b->nst == 1 ? RESAMPLE_BUF_COUNT / 2
                                               : RS_TMP_COUNT / 2);
        for (int ch = 0; ch < nch; ch++)
        {
            if (b->nst == 1)
            {
                hb_up2(dst->p32[ch], src->p32[ch], n, rs_st[0][ch], b->hb[0]);
            }
            else
            {
                hb_up2(rs_tmp_a[ch], src->p32[ch], n, rs_st[0][ch],
                       b->hb[0]);
                hb_up2(dst->p32[ch], rs_tmp_a[ch], 2*n, rs_st[1][ch],
                       b->hb[1]);
            }
        }
        used = n;
        out = n << b->nst;
    }
    else /* RS_FRAC */
    {
        return rs_frac_run(src, dst, nch);
    }

    dst->remcount = out;
    return used;
}

void dsp_resample_set_quality(unsigned int quality)
{
    rs_quality = quality;
}
static void resample_flush_data(struct resample_data *data)
{
    data->phase = 0;
    memset(&data->history, 0, sizeof (data->history));
}

static void resample_flush(struct dsp_proc_entry *this)
{
    struct resample_data *data = (void *)this->data;
    data->resample_buf.remcount = 0;
    resample_flush_data(data);
    if (data == &resample_data[CODEC_IDX_AUDIO])
        rs_bl_flush();
}

static bool resample_new_delta(struct resample_data *data,
                               struct sample_format *format,
                               unsigned int fout)
{
    unsigned int frequency = format->frequency; /* virtual samplerate */

    data->frequency = frequency;
    data->frequency_out = fout;
    data->delta = fp_div(frequency, fout, 16);

    if (data == &resample_data[CODEC_IDX_AUDIO])
        rs_bl_setup(frequency, fout);

    if (frequency == data->frequency_out)
    {
        /* NOTE: If fully glitch-free transistions from no resampling to
           resampling are desired, history should be maintained even when
           not resampling. */
        resample_flush_data(data);
        return false;
    }

    return true;
}

#if (!defined(CPU_COLDFIRE) && !defined(CPU_ARM)) || defined(CPU_ARM_MICRO)
int resample_hermite(struct resample_data *data, struct dsp_buffer *src,
                     struct dsp_buffer *dst)
{
    int ch = src->format.num_channels - 1;
    uint32_t count = MIN(src->remcount, 0x8000);
    uint32_t delta = data->delta;
    uint32_t phase, pos;
    int32_t *d;

    do
    {
        const int32_t *s = src->p32[ch];

        d = dst->p32[ch];
        int32_t *dmax = d + dst->bufcount;

        /* Restore state */
        phase = data->phase;
        pos = phase >> 16;
        pos = MIN(pos, count);

        while (pos < count && d < dmax)
        {
            int x0, x1, x2, x3;

            if (pos < 3)
            {
                x3 = data->history[ch][pos+0];
                x2 = pos < 2 ? data->history[ch][pos+1] : s[pos-2];
                x1 = pos < 1 ? data->history[ch][pos+2] : s[pos-1];
            }
            else
            {
                x3 = s[pos-3];
                x2 = s[pos-2];
                x1 = s[pos-1];
            }

            x0 = s[pos];

            int32_t frac = (phase & 0xffff) << 15;

            /* 4-point, 3rd-order Hermite/Catmull-Rom spline (x-form):
             * c1 = -0.5*x3 + 0.5*x1
             *    = 0.5*(x1 - x3)                <--
             *
             * v = x1 - x2, -v = x2 - x1
             * c2 = x3 - 2.5*x2 + 2*x1 - 0.5*x0
             *    = x3 + 2*(x1 - x2) - 0.5*(x0 + x2)
             *    = x3 + 2*v - 0.5*(x0 + x2)     <--
             *
             * c3 = -0.5*x3 + 1.5*x2 - 1.5*x1 + 0.5*x0
             *    = 0.5*x0 - 0.5*x3 + 0.5*(x2 - x1) + (x2 - x1)
             *    = 0.5*(x0 - x3 - v) - v        <--
             *
             * polynomial coefficients */
            int32_t c1 = (x1 - x3) >> 1;
            int32_t v = x1 - x2;
            int32_t c2 = x3 + 2*v - ((x0 + x2) >> 1);
            int32_t c3 = ((x0 - x3 - v) >> 1) - v;

            /* Evaluate polynomial at time 'frac'; Horner's rule. */
            int32_t acc;
            acc = FRACMUL(c3, frac) + c2;
            acc = FRACMUL(acc, frac) + c1;
            acc = FRACMUL(acc, frac) + x2;

            *d++ = acc;

            phase += delta;
            pos = phase >> 16;
        }

        pos = MIN(pos, count);

        /* Save delay samples for next time. Must do this even if pos was
         * clamped before loop in order to keep record up to date. */
        data->history[ch][0] = pos < 3 ? data->history[ch][pos+0] : s[pos-3];
        data->history[ch][1] = pos < 2 ? data->history[ch][pos+1] : s[pos-2];
        data->history[ch][2] = pos < 1 ? data->history[ch][pos+2] : s[pos-1];
    }
    while (--ch >= 0);

    /* Wrap phase accumulator back to start of next frame. */
    data->phase = phase - (pos << 16);

    dst->remcount = d - dst->p32[0];
    return pos;
}
#endif /* CPU */

/* Resample count stereo samples or stop when the destination is full.
 * Updates the src buffer and changes to its own output buffer to refer to
 * the resampled data. */
static void resample_process(struct dsp_proc_entry *this,
                             struct dsp_buffer **buf_p)
{
    struct resample_data *data = (void *)this->data;
    struct dsp_buffer *src = *buf_p;
    struct dsp_buffer *dst = &data->resample_buf;

    *buf_p = dst;

    if (dst->remcount > 0)
        return; /* data still remains */

    dst->remcount = 0;
    dst->p32[0] = data->resample_out_p[0];
    dst->p32[1] = data->resample_out_p[1];

    if (src->remcount > 0)
    {
        dst->bufcount = RESAMPLE_BUF_COUNT;

        int consumed = (data == &resample_data[CODEC_IDX_AUDIO] &&
                        rs_bl.mode != RS_HERMITE) ?
            resample_bl(data, src, dst) : resample_hermite(data, src, dst);

        /* Advance src by consumed amount */
        if (consumed > 0)
            dsp_advance_buffer32(src, consumed);
    }
    /* else purged resample_buf */

    /* Inherit in-place processed mask from source buffer */
    dst->proc_mask = src->proc_mask;
}

/* Finish draining old samples then switch format or shut off */
static intptr_t resample_new_format(struct dsp_proc_entry *this,
                                    struct dsp_config *dsp,
                                    struct sample_format *format)
{
    struct resample_data *data = (void *)this->data;
    struct dsp_buffer *dst = &data->resample_buf;

    if (dst->remcount > 0)
        return PROC_NEW_FORMAT_TRANSITION;

    DSP_PRINT_FORMAT(DSP_PROC_RESAMPLE, *format);

    unsigned int frequency = data->frequency;
    unsigned int fout = dsp_get_output_frequency(dsp);
    bool active = dsp_proc_active(dsp, DSP_PROC_RESAMPLE);

    if ((unsigned int)format->frequency != frequency ||
        data->frequency_out != fout)
    {
        DEBUGF("  DSP_PROC_RESAMPLE- new settings: %u %u\n",
               format->frequency, fout);
        active = resample_new_delta(data, format, fout);
        dsp_proc_activate(dsp, DSP_PROC_RESAMPLE, active);
    }

    /* Everything after us is fout */
    dst->format = *format;
    dst->format.frequency = fout;
    dst->format.codec_frequency = fout;

    if (active)
        return PROC_NEW_FORMAT_OK;

    /* No longer needed */
    DEBUGF("  DSP_PROC_RESAMPLE- deactivated\n");
    return PROC_NEW_FORMAT_DEACTIVATED;
}

void dsp_resample_init(struct dsp_config *dsp, unsigned int dsp_id)
{
    int32_t *lbuf, *rbuf;

    switch (dsp_id)
    {
    case CODEC_IDX_AUDIO:
        lbuf = resample_out_bufs[0];
        rbuf = resample_out_bufs[1];
        break;

    case CODEC_IDX_VOICE:
        lbuf = rbuf = resample_out_bufs[2]; /* Always mono */
        break;

    default:
        /* huh? */
        DEBUGF("DSP_PROC_RESAMPLE- unknown DSP %u\n", dsp_id);
        return;
    }

    /* Always enable resampler so that format changes may be monitored and
     * it self-activated when required */
    dsp_proc_enable(dsp, DSP_PROC_RESAMPLE, true);
    resample_data[dsp_id].resample_out_p[0] = lbuf;
    resample_data[dsp_id].resample_out_p[1] = rbuf;
}

static void resample_proc_init(struct dsp_proc_entry *this,
                               struct dsp_config *dsp)
{
    struct resample_data *data = &resample_data[dsp_get_id(dsp)];
    this->data = (intptr_t)data;
    dsp_proc_set_in_place(dsp, DSP_PROC_RESAMPLE, false);
    data->frequency_out = DSP_OUT_DEFAULT_HZ;
    this->process = resample_process;
}

/* DSP message hook */
static intptr_t resample_configure(struct dsp_proc_entry *this,
                                   struct dsp_config *dsp,
                                   unsigned int setting,
                                   intptr_t value)
{
    intptr_t retval = 0;

    switch (setting)
    {
    case DSP_FLUSH:
        resample_flush(this);
        break;

    case DSP_PROC_INIT:
        resample_proc_init(this, dsp);
        break;

    case DSP_PROC_CLOSE:
        /* This stage should be enabled at all times */
        DEBUGF("DSP_PROC_RESAMPLE- Error: Closing!\n");
        break;

    case DSP_PROC_NEW_FORMAT:
        retval = resample_new_format(this, dsp, (struct sample_format *)value);
        break;

    case DSP_SET_OUT_FREQUENCY:
        dsp_proc_want_format_update(dsp, DSP_PROC_RESAMPLE);
        break;
    }

    return retval;
}

/* Database entry */
DSP_PROC_DB_ENTRY(RESAMPLE,
                  resample_configure);
