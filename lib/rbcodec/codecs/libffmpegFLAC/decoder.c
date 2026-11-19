/*
 * FLAC (Free Lossless Audio Codec) decoder
 * Copyright (c) 2003 Alex Beregszaszi
 * Modified for ProkBox5g (2026); see docs/changes-from-rockbox.md.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/**
 * @file flac.c
 * FLAC (Free Lossless Audio Codec) decoder
 * @author Alex Beregszaszi
 *
 * For more information on the FLAC format, visit:
 *  http://flac.sourceforge.net/
 *
 * This decoder can be used in 1 of 2 ways: Either raw FLAC data can be fed
 * through, starting from the initial 'fLaC' signature; or by passing the
 * 34-byte streaminfo structure through avctx->extradata[_size] followed
 * by data starting with the 0xFFF8 marker.
 */

#include <inttypes.h>
#include <stdbool.h>
#ifndef BUILD_STANDALONE
#include "codeclib.h"
#endif
 
#include "bitstream.h"
#include "golomb.h"

#include "decoder.h"
#include "dither.h"

#if defined(CPU_COLDFIRE)
#include "coldfire.h"
#elif defined(CPU_ARM)
#include "arm.h"
#endif

/* get_bits() and get_sbits() always inline here: GCC's inlining budget
   otherwise makes out-of-line copies of them in .text, out of IRAM, which
   the verbatim and escaped-partition loops would call once per sample. */
static av_always_inline unsigned int flac_get_bits(GetBitContext *s, int n)
{
    register int tmp;
    OPEN_READER(re, s)
    UPDATE_CACHE(re, s)
    tmp= SHOW_UBITS(re, s, n);
    LAST_SKIP_BITS(re, s, n)
    CLOSE_READER(re, s)
    return tmp;
}

static av_always_inline int flac_get_sbits(GetBitContext *s, int n)
{
    register int tmp;
    OPEN_READER(re, s)
    UPDATE_CACHE(re, s)
    tmp= SHOW_SBITS(re, s, n);
    LAST_SKIP_BITS(re, s, n)
    CLOSE_READER(re, s)
    return tmp;
}
#define get_bits flac_get_bits
#define get_sbits flac_get_sbits

/* The bit reader (ffmpeg_get_bits.h, ALT_BITSTREAM_READER) returns at most
   MIN_CACHE_BITS (25) bits a read; a wider one comes back wrong.  FLAC has
   wider fields: escaped Rice partitions up to 31 bits, and sample widths
   (warm-up, constant and verbatim samples) of bps + 1 on a side channel,
   up to 33 in the format.  Those reads go through here, in two parts above
   25 bits.  n is 1..32. */
static av_always_inline int flac_get_sbits_wide(GetBitContext *s, int n)
{
    if (n <= MIN_CACHE_BITS)
        return get_sbits(s, n);
    int hi = get_sbits(s, n - 16);
    return (int)(((unsigned)hi << 16) | get_bits(s, 16));
}

/* A 33-bit sample (a 32-bit stream's side channel): n is 17..33 */
static int64_t flac_get_sbits33(GetBitContext *s, int n)
{
    int hi = get_sbits(s, n - 16);
    return (int64_t)hi * 65536 + get_bits(s, 16);
}

/* arm.S's Rice reader (decode_residuals) and stereo decorrelation
   (flac_decode_frame); FLAC_NO_RICE_ASM keeps the C ones, for an A/B */
#if defined(CPU_ARM_CLASSIC) && !defined(FLAC_NO_RICE_ASM)
#define FLAC_RICE_ASM
#define FLAC_DECORRELATE_ASM
#endif

static const int sample_rate_table[] ICONST_ATTR =
{ 0, 88200, 176400, 192000,
  8000, 16000, 22050, 24000, 32000, 44100, 48000, 96000,
  0, 0, 0, 0 };

static const int sample_size_table[] ICONST_ATTR = 
{ 0, 8, 12, 0, 16, 20, 24, 32 };  /* 7: 32 bits (RFC 9639) */

static const int blocksize_table[] ICONST_ATTR = {
     0,    192, 576<<0, 576<<1, 576<<2, 576<<3,      0,      0, 
256<<0, 256<<1, 256<<2, 256<<3, 256<<4, 256<<5, 256<<6, 256<<7 
};

static const uint8_t table_crc8[256] ICONST_ATTR = {
    0x00, 0x07, 0x0e, 0x09, 0x1c, 0x1b, 0x12, 0x15,
    0x38, 0x3f, 0x36, 0x31, 0x24, 0x23, 0x2a, 0x2d,
    0x70, 0x77, 0x7e, 0x79, 0x6c, 0x6b, 0x62, 0x65,
    0x48, 0x4f, 0x46, 0x41, 0x54, 0x53, 0x5a, 0x5d,
    0xe0, 0xe7, 0xee, 0xe9, 0xfc, 0xfb, 0xf2, 0xf5,
    0xd8, 0xdf, 0xd6, 0xd1, 0xc4, 0xc3, 0xca, 0xcd,
    0x90, 0x97, 0x9e, 0x99, 0x8c, 0x8b, 0x82, 0x85,
    0xa8, 0xaf, 0xa6, 0xa1, 0xb4, 0xb3, 0xba, 0xbd,
    0xc7, 0xc0, 0xc9, 0xce, 0xdb, 0xdc, 0xd5, 0xd2,
    0xff, 0xf8, 0xf1, 0xf6, 0xe3, 0xe4, 0xed, 0xea,
    0xb7, 0xb0, 0xb9, 0xbe, 0xab, 0xac, 0xa5, 0xa2,
    0x8f, 0x88, 0x81, 0x86, 0x93, 0x94, 0x9d, 0x9a,
    0x27, 0x20, 0x29, 0x2e, 0x3b, 0x3c, 0x35, 0x32,
    0x1f, 0x18, 0x11, 0x16, 0x03, 0x04, 0x0d, 0x0a,
    0x57, 0x50, 0x59, 0x5e, 0x4b, 0x4c, 0x45, 0x42,
    0x6f, 0x68, 0x61, 0x66, 0x73, 0x74, 0x7d, 0x7a,
    0x89, 0x8e, 0x87, 0x80, 0x95, 0x92, 0x9b, 0x9c,
    0xb1, 0xb6, 0xbf, 0xb8, 0xad, 0xaa, 0xa3, 0xa4,
    0xf9, 0xfe, 0xf7, 0xf0, 0xe5, 0xe2, 0xeb, 0xec,
    0xc1, 0xc6, 0xcf, 0xc8, 0xdd, 0xda, 0xd3, 0xd4,
    0x69, 0x6e, 0x67, 0x60, 0x75, 0x72, 0x7b, 0x7c,
    0x51, 0x56, 0x5f, 0x58, 0x4d, 0x4a, 0x43, 0x44,
    0x19, 0x1e, 0x17, 0x10, 0x05, 0x02, 0x0b, 0x0c,
    0x21, 0x26, 0x2f, 0x28, 0x3d, 0x3a, 0x33, 0x34,
    0x4e, 0x49, 0x40, 0x47, 0x52, 0x55, 0x5c, 0x5b,
    0x76, 0x71, 0x78, 0x7f, 0x6a, 0x6d, 0x64, 0x63,
    0x3e, 0x39, 0x30, 0x37, 0x22, 0x25, 0x2c, 0x2b,
    0x06, 0x01, 0x08, 0x0f, 0x1a, 0x1d, 0x14, 0x13,
    0xae, 0xa9, 0xa0, 0xa7, 0xb2, 0xb5, 0xbc, 0xbb,
    0x96, 0x91, 0x98, 0x9f, 0x8a, 0x8d, 0x84, 0x83,
    0xde, 0xd9, 0xd0, 0xd7, 0xc2, 0xc5, 0xcc, 0xcb,
    0xe6, 0xe1, 0xe8, 0xef, 0xfa, 0xfd, 0xf4, 0xf3
};

static int64_t get_utf8(GetBitContext *gb) ICODE_ATTR_FLAC;
static int64_t get_utf8(GetBitContext *gb)
{
    uint64_t val;
    int ones=0, bytes;
    
    /* up to seven bytes code 36 bits */
    while(get_bits1(gb))
        if (++ones > 7)
            return -1;

    if     (ones==0) bytes=0;
    else if(ones==1) return -1;
    else             bytes= ones - 1;
    
    val= ones < 7 ? get_bits(gb, 7-ones) : 0;
    while(bytes--){
        const int tmp = get_bits(gb, 8);
        
        if((tmp>>6) != 2)
            return -2;
        val<<=6;
        val|= tmp&0x3F;
    }
    return val;
}

static int get_crc8(const uint8_t *buf, int count) ICODE_ATTR_FLAC;
static int get_crc8(const uint8_t *buf, int count)
{
    int crc=0;
    int i;
    
    for(i=0; i<count; i++){
        crc = table_crc8[crc ^ buf[i]];
    }

    return crc;
}

/* Rice codes decoded between checks for the end of the data (upstream
   7b7a2dde98). A code is at most 56 bits, so a corrupt frame is caught
   within 1 KiB of its end. */
#define RICE_CHUNK 128

static int decode_residuals(FLACContext *s, int32_t* decoded, int pred_order) ICODE_ATTR_FLAC;
static int decode_residuals(FLACContext *s, int32_t *decoded, int pred_order)
{
    GetBitContext gb = s->gb;
    int i, tmp, partition, method_type, rice_order;
    int rice_bits, rice_esc;
    int samples;

    method_type = get_bits(&gb, 2);
    rice_order  = get_bits(&gb, 4);

    samples   = s->blocksize >> rice_order;
    rice_bits = 4 + method_type;
    rice_esc  = (1 << rice_bits) - 1;

    decoded += pred_order;
    i        = pred_order;

    if (method_type > 1 ||  (samples << rice_order != s->blocksize) || pred_order > samples)
    {
        return -3;
    }

    for (partition = 0; partition < (1 << rice_order); partition++) {
        tmp = get_bits(&gb, rice_bits);
        if (tmp == rice_esc) {
            tmp = get_bits(&gb, 5);
            /* every bit the partition needs must be in the data */
            if ((samples - i) * tmp > get_bits_left(&gb))
                return -3;
            /* An escape width of 0 means all residuals are zero; widths
               above 25 bits need two reads (flac_get_sbits_wide), chosen
               once a partition so the common loop keeps one. */
            if (tmp > MIN_CACHE_BITS)
                for (; i < samples; i++)
                    *decoded++ = flac_get_sbits_wide(&gb, tmp);
            else
                for (; i < samples; i++)
                    *decoded++ = tmp ? get_sbits(&gb, tmp) : 0;
        } else {
            unsigned lim = tmp ? (UINT_MAX >> tmp) + 2 : INT_MAX;
            int real_limit = lim > INT_MAX ? INT_MAX : lim;
            while (i < samples) {
                /* A corrupt partition can run past the end of the data, so
                   check every RICE_CHUNK codes that it has not, which
                   bounds how far it can read beyond. */
                int end = samples - i > RICE_CHUNK ? i + RICE_CHUNK : samples;
                if (get_bits_left(&gb) < 0)
                    return -3;
#ifdef FLAC_RICE_ASM
                /* The assembler takes every code that fits 32 bits and
                   stops in front of any other; the C reader below decodes
                   that one, with its limits and checks, and hands the rest
                   back. */
                for (;;) {
                    int n = flac_rice_arm(&gb, decoded, end - i, tmp);
                    decoded += n;
                    i += n;
                    if (i >= end)
                        break;
                    int v = get_sr_golomb_flac(&gb, tmp, real_limit, 0);
                    if ((unsigned) v == 0x80000000){
                        return -3;
                    }
                    *decoded++ = v;
                    i++;
                }
#endif
                for (; i < end; i++) {
                    int v = get_sr_golomb_flac(&gb, tmp, real_limit, 0);
                    if ((unsigned) v == 0x80000000){
                        return -3;
                    }

                    *decoded++ = v;
                }
            }
        }
        i= 0;
    }

    s->gb = gb;

    return 0;
}

/* In IRAM too: fixed predictors are what an encoder picks at transients */
int decode_subframe_fixed(FLACContext *s, int32_t* decoded, int pred_order, int bps) ICODE_ATTR_FLAC;
int decode_subframe_fixed(FLACContext *s, int32_t* decoded, int pred_order, int bps)
{
    const int blocksize = s->blocksize;
    unsigned a, b, c, d;
    int i;

    /* warm up samples */
    for (i = 0; i < pred_order; i++)
    {
        decoded[i] = flac_get_sbits_wide(&s->gb, bps);
    }

    if (decode_residuals(s, decoded, pred_order) < 0)
        return -4;

    switch(pred_order)
    {
        case 0:
            break;
        case 1:
            a = decoded[pred_order-1];
            for (i = pred_order; i < blocksize; i++)
                decoded[i] = a += decoded[i];
            break;
        case 2:
            a = decoded[pred_order-1];
            b = a - decoded[pred_order-2];
            for (i = pred_order; i < blocksize; i++)
                decoded[i] = a += b += decoded[i];
            break;
        case 3:
            a = decoded[pred_order-1];
            b = a - decoded[pred_order-2];
            c = b - decoded[pred_order-2] + decoded[pred_order-3];
            for (i = pred_order; i < blocksize; i++)
                decoded[i] = a += b += c += decoded[i];
            break;
        case 4:
            a = decoded[pred_order-1];
            b = a - decoded[pred_order-2];
            c = b - decoded[pred_order-2] + decoded[pred_order-3];
            d = c - decoded[pred_order-2] + 2U*decoded[pred_order-3] - decoded[pred_order-4];
            for (i = pred_order; i < blocksize; i++)
                decoded[i] = a += b += c += d += decoded[i];
            break;
        default:
            return -5;
    }

    return 0;
}

#if !defined(CPU_COLDFIRE)
/* The assembler kernels in arm.S implement flac_lpc_32_c exactly; keep the C
   version compiled anyway when they are in use, so that a build can A/B the
   two by defining FLAC_NO_LPC32_ASM. */
#if defined(CPU_ARM_CLASSIC) && !defined(FLAC_NO_LPC32_ASM)
#define FLAC_LPC32_ASM
#endif

static void flac_lpc_32_c(int32_t *decoded, int coeffs[],
                          int pred_order, int qlevel, int len)
                          ICODE_ATTR_FLAC UNUSED_ATTR;
static void flac_lpc_32_c(int32_t *decoded, int coeffs[],
                          int pred_order, int qlevel, int len)
{
    int i, j;
    /*NOTE coeffs[] is in reverse order compared to upstream*/
    for (i = pred_order; i < len; i++, decoded++) {
        int64_t sum = 0;
        for (j = 0; j < pred_order; j++)
            sum += (int64_t)coeffs[pred_order-j-1] * decoded[j];
        decoded[j] += sum >> qlevel;
    }

}

static void lpc_analyze_remodulate(int32_t *decoded, int coeffs[],
                                   int order, int qlevel, int len, int bps)
{
    /*NOTE coeffs[] is in reverse order compared to upstream*/
    int i, j;
    int ebps = 1 << (bps-1);
    unsigned sigma = 0;

#ifdef FLAC_RICE_ASM
    /* arm.S: four samples an ldm, 4.5 cycles a sample against 9 */
    sigma = flac_range_or_arm(decoded + order, len - order, ebps);
#else
    for (i = order; i < len; i++)
        sigma |= (unsigned)decoded[i] + ebps;
#endif

    if (sigma < 2U*ebps)
        return;

    for (i = len - 1; i >= order; i--) {
        int64_t p = 0;
        for (j = 0; j < order; j++)
            p += coeffs[order-j-1] * (int64_t)(int32_t)decoded[i-order+j];
        decoded[i] -= p >> qlevel;
    }
    for (i = order; i < len; i++, decoded++) {
        int32_t p = 0;
        for (j = 0; j < order; j++)
            p += coeffs[order-j-1] * (uint32_t)decoded[j];
        decoded[j] += p >> qlevel;
    }
}
#endif

static int decode_subframe_lpc(FLACContext *s, int32_t* decoded, int pred_order, int bps) ICODE_ATTR_FLAC;
static int decode_subframe_lpc(FLACContext *s, int32_t* decoded, int pred_order, int bps)
{
    int sum, i, j;
    int coeff_prec, qlevel;
    int coeffs[pred_order];

    /* warm up samples */
    for (i = 0; i < pred_order; i++)
    {
        decoded[i] = flac_get_sbits_wide(&s->gb, bps);
    }

    coeff_prec = get_bits(&s->gb, 4) + 1;
    if (coeff_prec == 16)
    {
        //fprintf(stderr,"invalid coeff precision\n");
        return -6;
    }
    qlevel = get_sbits(&s->gb, 5);
    if (qlevel < 0) 
    {
        //fprintf(stderr,"qlevel %d not supported, maybe buggy stream\n", qlevel);
        return -7;
    }

    for (j = 0; j < pred_order; j++)
    {
        coeffs[j] = get_sbits(&s->gb, coeff_prec);
    }
    
    if (decode_residuals(s, decoded, pred_order) < 0)
        return -8;

    /* The 32-bit filter is exact when no prediction sum can leave 32 bits:
       every history sample fits bps bits (the warm-up by construction, the
       outputs in any valid stream), so |sum| <= sum(|c|) * 2^(bps-1), and
       that is below 2^31 when sum(|c|) < 2^(32-bps).  This is the bound
       libFLAC's decoder uses, a bit less conservative; it holds wherever
       the old test (bps + coeff_prec + log2(order) <= 32) did, now with
       this channel's own bps (a side channel has one more), and also for
       many subframes that test sent to the 64-bit filter.  For bps <= 16
       the result is also what the 64-bit filter plus
       lpc_analyze_remodulate gives, valid stream or not: that pass hands
       back the 32-bit wrapping decode whenever an output leaves bps bits,
       and until one does the two filters agree.  Ask tests/flacarm. */
    for (sum = 0, j = 0; j < pred_order; j++)
        sum += coeffs[j] < 0 ? -coeffs[j] : coeffs[j];
    if (bps >= 1 && bps <= 31 && (unsigned)sum < (1u << (32 - bps))) {
        #if defined(CPU_COLDFIRE)
        (void)sum;
        lpc_decode_emac(s->blocksize - pred_order, qlevel, pred_order,
                        decoded + pred_order, coeffs);
        #elif defined(CPU_ARM_CLASSIC)
        (void)sum;
        lpc_decode_arm(s->blocksize - pred_order, qlevel, pred_order,
                       decoded + pred_order, coeffs);
        #else
        /* unsigned, so that a corrupt stream wraps instead of overflowing */
        for (i = pred_order; i < s->blocksize; i++)
        {
            sum = 0;
            for (j = 0; j < pred_order; j++)
                sum += (unsigned)coeffs[j] * decoded[i-j-1];
            decoded[i] = (unsigned)decoded[i] + (sum >> qlevel);
        }
        #endif
    } else {
        #if defined(CPU_COLDFIRE)
        lpc_decode_emac_wide(s->blocksize - pred_order, qlevel, pred_order,
                             decoded + pred_order, coeffs);
        #else
        #if defined(FLAC_LPC32_ASM)
        /* The 16-bit multiply kernel needs every history sample to fit a
           signed halfword, which bps <= 16 guarantees, and checks its other
           precondition itself. */
        #if (ARM_ARCH >= 5) && defined(FLAC_LPC32_NARROW_ASM)
        if (bps <= 16)
            flac_lpc_32_arm_narrow(decoded, coeffs, pred_order, qlevel,
                                   s->blocksize);
        else
        #endif
        flac_lpc_32_arm(decoded, coeffs, pred_order, qlevel, s->blocksize);
        #else
        flac_lpc_32_c(decoded, coeffs, pred_order, qlevel, s->blocksize);
        #endif

        if (bps <= 16)
            lpc_analyze_remodulate(decoded, coeffs, pred_order, qlevel, s->blocksize, bps);
        #endif
    }

    return 0;
}

/* #318: one subframe of 33-bit samples (a 32-bit stereo stream's side
   channel, no wasted bits), into w; its residuals go through res, the
   channel's own 32-bit buffer.  The arithmetic is libFLAC's for this case:
   64-bit history, 32-bit residuals, prediction sums in 64 bits.  Not in
   IRAM (noinline keeps it out of its IRAM callers): only 32-bit streams
   come here, and only for the side channel. */
static int __attribute__((noinline))
decode_subframe_33(FLACContext *s, int type, int32_t *res, int64_t *w)
{
    GetBitContext *gb = &s->gb;
    const int n = s->blocksize;
    int i, j, order;

    if (type == 0) {
        int64_t c = flac_get_sbits33(gb, 33);
        for (i = 0; i < n; i++)
            w[i] = c;
        return 0;
    }
    if (type == 1) {
        for (i = 0; i < n; i++)
            w[i] = flac_get_sbits33(gb, 33);
        return 0;
    }
    if (type >= 8 && type <= 12) {
        order = type & 7;
        for (i = 0; i < order; i++)
            w[i] = flac_get_sbits33(gb, 33);
        if (decode_residuals(s, res, order) < 0)
            return -4;
        switch (order) {
        case 0:
            for (i = 0; i < n; i++)
                w[i] = res[i];
            break;
        case 1:
            for (i = 1; i < n; i++)
                w[i] = res[i] + w[i-1];
            break;
        case 2:
            for (i = 2; i < n; i++)
                w[i] = res[i] + 2*w[i-1] - w[i-2];
            break;
        case 3:
            for (i = 3; i < n; i++)
                w[i] = res[i] + 3*(w[i-1] - w[i-2]) + w[i-3];
            break;
        case 4:
            for (i = 4; i < n; i++)
                w[i] = res[i] + 4*(w[i-1] + w[i-3]) - 6*w[i-2] - w[i-4];
            break;
        }
        return 0;
    }
    if (type >= 32) {
        int coeffs[32];
        int coeff_prec, qlevel;

        order = (type & ~0x20) + 1;
        for (i = 0; i < order; i++)
            w[i] = flac_get_sbits33(gb, 33);
        coeff_prec = get_bits(gb, 4) + 1;
        if (coeff_prec == 16)
            return -6;
        qlevel = get_sbits(gb, 5);
        if (qlevel < 0)
            return -7;
        for (j = 0; j < order; j++)
            coeffs[j] = get_sbits(gb, coeff_prec);
        if (decode_residuals(s, res, order) < 0)
            return -8;
        /* A side channel's samples nearly always fit 32 bits (a left and a
           right more than half of full scale apart, of opposite signs, do
           not): while they do, the history is read from res[] as 32-bit
           values, overwriting each residual with its sample (res[0..order)
           is free), which makes a tap one 32x32->64 multiply-accumulate;
           from the first sample that does not, the 64-bit history in w[]
           (kept complete all along) takes over. */
        for (i = 0; i < order; i++) {
            if (w[i] != (int32_t)w[i])
                goto wide64;
            res[i] = (int32_t)w[i];
        }
        for (i = order; i < n; i++) {
            int64_t sum = 0, v;
            for (j = 0; j < order; j++)
                sum += (int64_t)coeffs[j] * res[i-j-1];
            v = res[i] + (sum >> qlevel);
            w[i] = v;
            if (v != (int32_t)v) {
                i++;
                goto wide64;
            }
            res[i] = (int32_t)v;
        }
        return 0;
    wide64:
        if (i < order)
            i = order;
        for (; i < n; i++) {
            int64_t sum = 0;
            for (j = 0; j < order; j++)
                sum += (int64_t)coeffs[j] * w[i-j-1];
            w[i] = res[i] + (sum >> qlevel);
        }
        return 0;
    }
    return -12;
}

static inline int decode_subframe(FLACContext *s, int channel, int32_t* decoded)
{
    int type, wasted = 0;
    int i, tmp;
    int bps = s->bps;
    if(channel == 0){
        if(s->ch_mode == RIGHT_SIDE)
            bps++;
    }else{
        if(s->ch_mode == LEFT_SIDE || s->ch_mode == MID_SIDE)
            bps++;
    }

    /* stop a corrupt frame that has run past the end of the data */
    if (get_bits_left(&s->gb) < 0 || get_bits1(&s->gb))
    {
        //fprintf(stderr,"invalid subframe padding\n");
        return -9;
    }
    type = get_bits(&s->gb, 6);

    if (get_bits1(&s->gb))
    {
        /* at least one bit must be left to read */
        wasted = 1;
        while (!get_bits1(&s->gb))
            if (++wasted >= bps)
                return -13;
        bps -= wasted;
        if (bps < 1)
            return -19;     /* malformed: no bits left to read a sample */
        //fprintf(stderr,"%d wasted bits\n", wasted);
    }

    /* upstream 7b7a2dde98: a verbatim subframe's bits must all be in the
       data, and a fixed or LPC one's warm-up samples must fit in the
       block. Checked before the 33-bit path below, which takes them too. */
    if (type == 1 && s->blocksize * bps > get_bits_left(&s->gb))
        return -12;
    if (type >= 8 && type <= 12 && (type & ~0x8) > s->blocksize)
        return -10;
    if (type >= 32 && (type & ~0x20) + 1 > s->blocksize)
        return -11;

    /* #318: a side channel of a 32-bit stream has 33 bits a sample.  Its
       samples go to the wide buffer: decoded there in 64 bits, or, with
       wasted bits (32 or fewer left), decoded below as usual and widened
       at the end. */
    if (bps + wasted > 32) {
        if (!s->wide)
            return -20;
        s->wide_ch = channel;
        if (!wasted)
            return decode_subframe_33(s, type, decoded, s->wide);
    }

//FIXME use av_log2 for types
    if (type == 0)
    {
        //fprintf(stderr,"coding type: constant\n");
        tmp = flac_get_sbits_wide(&s->gb, bps);
        for (i = 0; i < s->blocksize; i++)
            decoded[i] = tmp;
    }
    else if (type == 1)
    {
        //fprintf(stderr,"coding type: verbatim\n");
        if (bps > MIN_CACHE_BITS)
            for (i = 0; i < s->blocksize; i++)
                decoded[i] = flac_get_sbits_wide(&s->gb, bps);
        else
            for (i = 0; i < s->blocksize; i++)
                decoded[i] = get_sbits(&s->gb, bps);
    }
    else if ((type >= 8) && (type <= 12))
    {
        //fprintf(stderr,"coding type: fixed\n");
        if (decode_subframe_fixed(s, decoded, type & ~0x8, bps) < 0)
            return -10;
    }
    else if (type >= 32)
    {
        //fprintf(stderr,"coding type: lpc\n");
        if (decode_subframe_lpc(s, decoded, (type & ~0x20)+1, bps) < 0)
            return -11;
    }
    else
    {
        //fprintf(stderr,"Unknown coding type: %d\n",type);
        return -12;
    }

    if (wasted && s->wide_ch == channel)
    {
        int64_t *w = s->wide;
        for (i = 0; i < s->blocksize; i++)
            w[i] = (int64_t)decoded[i] * (1 << wasted);
    }
    else if (wasted && wasted < 32)
    {
        int i;
        for (i = 0; i < s->blocksize; i++)
            decoded[i] = (unsigned)decoded[i] << wasted;
    }

    return 0;
}

static int decode_frame(FLACContext *s, int variable_blocksize,
                        void (*yield)(void)) ICODE_ATTR_FLAC;
static int decode_frame(FLACContext *s, int variable_blocksize,
                        void (*yield)(void))
{
    int blocksize_code, sample_rate_code, sample_size_code, crc8;
    int ch_mode, bps, blocksize, samplerate;
    int res, ch;
    int64_t number;
    GetBitContext *gb = &s->gb;

    blocksize_code = get_bits(gb, 4);

    sample_rate_code = get_bits(gb, 4);

    ch_mode = get_bits(gb, 4); /* channel assignment */
    if (ch_mode < 8 && s->channels == ch_mode+1)
        ch_mode = INDEPENDENT;
    else if (ch_mode >=8 && ch_mode < 11 && s->channels == 2)
        ch_mode = LEFT_SIDE + ch_mode - 8;
    else
    {
        return -13;
    }

    sample_size_code = get_bits(gb, 3);
    if(sample_size_code == 0)
        bps= s->bps;
    else if(sample_size_code != 3)
        bps = sample_size_table[sample_size_code];
    else 
    {
        return -14;
    }

    if (get_bits1(gb))
    {
        return -15;
    }

    /* The number of the first sample in this block, or of the frame */
    number = get_utf8(gb);
    if (number < 0)
        return -19;

    if (blocksize_code == 0)
        blocksize = s->min_blocksize;
    else if (blocksize_code == 6)
        blocksize = get_bits(gb, 8)+1;
    else if (blocksize_code == 7)
        blocksize = get_bits(gb, 16)+1;
    else 
        blocksize = blocksize_table[blocksize_code];

    if(blocksize > s->max_blocksize){
        return -16;
    }

    /* The header's blocking strategy bit tells which number it holds. A
       stream with a fixed block size numbers its frames instead, and every
       frame but a shorter last one has the maximum block size. Using the
       maximum rather than this frame's size keeps that last frame's number
       right. Streams written before the bit was defined (Flake 0.11) leave
       it clear and number their samples, so as libFLAC does, a stream
       whose STREAMINFO block sizes differ is taken as variable too. */
    if (!variable_blocksize)
        number *= s->max_blocksize;
    s->samplenumber = number;

    if (sample_rate_code == 0){
        samplerate= s->samplerate;
    }else if ((sample_rate_code < 12))
        samplerate = sample_rate_table[sample_rate_code];
    else if (sample_rate_code == 12)
        samplerate = get_bits(gb, 8) * 1000;
    else if (sample_rate_code == 13)
        samplerate = get_bits(gb, 16);
    else if (sample_rate_code == 14)
        samplerate = get_bits(gb, 16) * 10;
    else{
        return -17;
    }

    skip_bits(gb, 8);
    crc8= get_crc8(s->gb.buffer, get_bits_count(gb)/8);
    if(crc8){
        return -18;
    }
    
    s->blocksize    = blocksize;
    s->samplerate   = samplerate;
    s->bps          = bps;
    s->ch_mode      = ch_mode;
    s->wide_ch      = -1;

    for (ch=0; ch<s->channels; ++ch) {
        yield();
        if ((res=decode_subframe(s, ch, s->decoded[ch])) < 0)
            return res-100;
    }
    
    yield();
    align_get_bits(gb);

    /* frame footer */
    skip_bits(gb, 16); /* data crc */

    return 0;
}

static int flac_downmix(FLACContext *s, int scale) ICODE_ATTR_FLAC;
static int flac_downmix(FLACContext *s, int scale)
{
    int32_t *FL, *FR, *FC, *SB, *RL, *RR, *RC;
    int32_t *outL = s->decoded[0];
    int32_t *outR = s->decoded[1];
    int i;

    switch(s->channels)
    {
        case 3: /* 3.0 channel order: FL FR FC */
            FL = s->decoded[0];
            FR = s->decoded[1];
            FC = s->decoded[2];
            /* LF = 0.66 LF + 0.33 FC
               LR = 0.66 LR + 0.33 FC */
            for (i=0; i<s->blocksize; ++i) {
                uint32_t a = (uint32_t)*(FL)*2 + *(FC);
                uint32_t b = (uint32_t)*(FR)*2 + *(FC);
                /* 1/3 ~= 5>>4 */
                *outL++ = (uint32_t)((int32_t)(a + (a<<2)) >> 4) << scale;
                *outR++ = (uint32_t)((int32_t)(b + (b<<2)) >> 4) << scale;
                FL++; FR++; FC++;
            }
            break;
        case 4: /* 4.0 channel order: FL FR RL RR */
            FL = s->decoded[0];
            FR = s->decoded[1];
            RL = s->decoded[2];
            RR = s->decoded[3];
            /* LF = 0.50 LF + 0.50 RL + 0.00 RR
               LR = 0.50 LR + 0.00 RL + 0.50 RR */
            for (i=0; i<s->blocksize; ++i) {
                uint32_t a = (uint32_t)*(FL) + *(RL);
                uint32_t b = (uint32_t)*(FR) + *(RR);
                *outL++ = (uint32_t)((int32_t)a >> 1) << scale;
                *outR++ = (uint32_t)((int32_t)b >> 1) << scale;
                FL++; FR++; RL++; RR++;
            }
            break;
        case 5: /* 5.0 channel order: FL FR FC RL RR */
            FL = s->decoded[0];
            FR = s->decoded[1];
            FC = s->decoded[2];
            RL = s->decoded[3];
            RR = s->decoded[4];
            /* LF = 0.40 LF + 0.20 FC + 0.40 RL + 0.00 RR
               LR = 0.40 LR + 0.20 FC + 0.00 RL + 0.40 RR */
            for (i=0; i<s->blocksize; ++i) {
                uint32_t a = (uint32_t)*(FL)*2 + *(FC) + *(RL)*2;
                uint32_t b = (uint32_t)*(FR)*2 + *(FC) + *(RR)*2;
                /* 3>>4 ~= 1/5 */
                *outL++ = (uint32_t)((int32_t)(a + (a<<1)) >> 4) << scale;
                *outR++ = (uint32_t)((int32_t)(b + (b<<1)) >> 4) << scale;
                FL++; FR++; FC++; RL++; RR++;
            }
            break;
        case 6: /* 5.1 channel order: FL FR FC SUB RL RR */
            FL = s->decoded[0];
            FR = s->decoded[1];
            FC = s->decoded[2];
            SB = s->decoded[3];
            RL = s->decoded[4];
            RR = s->decoded[5];
            /* LF = 0.33 LF + 0.16 SUB + 0.16 FC + 0.33 RL + 0.00 RR
               LR = 0.33 LR + 0.16 SUB + 0.16 FC + 0.00 RL + 0.33 RR */
            for (i=0; i<s->blocksize; ++i) {
                uint32_t a = (uint32_t)*(FL)*2 + *(SB) + *(FC) + *(RL)*2;
                uint32_t b = (uint32_t)*(FR)*2 + *(SB) + *(FC) + *(RR)*2;
                /* 5>>5 ~= 1/6 */
                *outL++ = (uint32_t)((int32_t)(a + (a<<2)) >> 5) << scale;
                *outR++ = (uint32_t)((int32_t)(b + (b<<2)) >> 5) << scale;
                FL++; FR++; SB++; FC++; RL++; RR++;
            }
            break;
        case 7: /* 6.1 channel order: FL FR FC SUB RL RR RC */
            FL = s->decoded[0];
            FR = s->decoded[1];
            FC = s->decoded[2];
            SB = s->decoded[3];
            RL = s->decoded[4];
            RR = s->decoded[5];
            RC = s->decoded[6];
            /* LF = 0.28 LF + 0.14 SUB + 0.14 FC + 0.28 RL + 0.00 RR + 0.14 CR
               LR = 0.28 LR + 0.14 SUB + 0.14 FC + 0.00 RL + 0.28 RR + 0.14 CR */
            for (i=0; i<s->blocksize; ++i) {
                uint32_t a = (uint32_t)*(FL)*2 + *(SB) + *(FC) + *(RL)*2 +
                             *(RC);
                uint32_t b = (uint32_t)*(FR)*2 + *(SB) + *(FC) + *(RR)*2 +
                             *(RC);
                /* 9>>6 ~= 1/7 */
                *outL++ = (uint32_t)((int32_t)(a + (a<<4)) >> 6) << scale;
                *outR++ = (uint32_t)((int32_t)(b + (b<<4)) >> 6) << scale;
                FL++; FR++; SB++; FC++; RL++; RR++; RC++;
            }
            break;
        default: /* 1.0 and 2.0 do not need downmix, other formats unknown. */
            return -501;
            break;
    }
    return 0;
}

/* #318: a frame whose side channel has 33 bits (in s->wide): left and
   right into decoded[0] and decoded[1], at their own width (not in IRAM) */
static void __attribute__((noinline)) flac_decorrelate_33(FLACContext *s)
{
    int32_t *d0 = s->decoded[0], *d1 = s->decoded[1];
    const int64_t *w = s->wide;
    int i, n = s->blocksize;

    switch (s->ch_mode) {
    case LEFT_SIDE:         /* left, side */
        for (i = 0; i < n; i++)
            d1[i] = (int32_t)(d0[i] - w[i]);
        break;
    case RIGHT_SIDE:        /* side, right */
        for (i = 0; i < n; i++)
            d0[i] = (int32_t)(d1[i] + w[i]);
        break;
    case MID_SIDE:          /* mid, side */
        for (i = 0; i < n; i++) {
            int64_t a = d0[i] - (w[i] >> 1);
            d0[i] = (int32_t)(a + w[i]);
            d1[i] = (int32_t)a;
        }
        break;
    default:
        break;
    }
}

/* #318: every channel of a multichannel frame wider than
   FLAC_OUTPUT_DEPTH, to FLAC_DOWNMIX_DEPTH bits, for the downmix */
static void __attribute__((noinline)) flac_reduce_channels(FLACContext *s)
{
    int ch;
    for (ch = 0; ch < s->channels; ch++)
        flac_reduce_tpdf(s->decoded[ch], s->blocksize,
                         s->bps - FLAC_DOWNMIX_DEPTH, FLAC_DOWNMIX_DEPTH,
                         &s->dither);
}

int flac_decode_frame(FLACContext *s,
                      uint8_t *buf, int buf_size,
                      void (*yield)(void))
{
    int tmp;
    int i, done;
    int framesize;
    int scale, wide;

    /* check that there is at least the smallest decodable amount of data.
       this amount corresponds to the smallest valid FLAC frame possible.
       this amount corresponds to the smallest valid FLAC frame possible.
       FF F8 69 02 00 00 9A 00 00 34 46 */
    if (buf_size < MIN_FRAME_SIZE)
        return buf_size;

    init_get_bits(&s->gb, buf, buf_size*8);

    tmp = get_bits(&s->gb, 16);
    if ((tmp & 0xFFFE) != 0xFFF8){
        return -41;
    }

    if ((framesize=decode_frame(s, (tmp & 1) ||
                                s->min_blocksize != s->max_blocksize,
                                yield)) < 0){
       s->bitstream_size=0;
       s->bitstream_index=0;
       return framesize;
    }

    yield();
    
#define DECORRELATE(left, right)\
             for (i = done; i < s->blocksize; i++) {\
                int32_t a = s->decoded[0][i];\
                int32_t b = s->decoded[1][i];\
                s->decoded[0][i] = (uint32_t)(left)  << scale;\
                s->decoded[1][i] = (uint32_t)(right) << scale;\
             }\

    /* #318: wider than FLAC_OUTPUT_DEPTH: a stereo or mono frame is
       decorrelated at the stream's own width (scale 0) and left there, for
       flac_reduce_frame(); a multichannel one is reduced here, to what the
       downmix holds, and downmixed to FLAC_OUTPUT_DEPTH */
    wide = s->bps > FLAC_OUTPUT_DEPTH;
    scale = wide ? 0 : FLAC_OUTPUT_DEPTH - s->bps;
    done = 0;
    if (s->wide_ch >= 0) {
        flac_decorrelate_33(s);
        done = s->blocksize;    /* the switch below has nothing left */
    }
#ifdef FLAC_DECORRELATE_ASM
    else if (s->channels <= 2)  /* arm.S: four samples a pass; C does the rest */
        done = flac_decorrelate_arm(s->decoded[0], s->decoded[1],
                                    s->blocksize, s->ch_mode, scale);
#endif
    switch(s->ch_mode)
    {
        case INDEPENDENT:
            if (s->channels <= 2) {
                DECORRELATE(a, b) /* Always decorrelate exactly the two supported channels. */
            } else {
                int dscale = scale;
                if (wide) {
                    flac_reduce_channels(s);
                    dscale = FLAC_OUTPUT_DEPTH - FLAC_DOWNMIX_DEPTH;
                }
                if ((tmp=flac_downmix(s, dscale)) != 0)
                    return tmp;
            }
            break;
        case LEFT_SIDE:
            DECORRELATE(a, (uint32_t)a-b)
            break;
        case RIGHT_SIDE:
            DECORRELATE((uint32_t)a+b, b)
            break;
        case MID_SIDE:
            DECORRELATE((a = (uint32_t)a - (b>>1)) + (uint32_t)b, a)
            break;
    }

    s->framesize = (get_bits_count(&s->gb)+7)>>3;

    return 0;
}

void flac_reduce_frame(FLACContext *s)
{
    if (s->bps <= FLAC_OUTPUT_DEPTH || s->channels > 2)
        return;
    flac_reduce_tpdf(s->decoded[0], s->blocksize,
                     s->bps - FLAC_OUTPUT_DEPTH, FLAC_OUTPUT_DEPTH,
                     &s->dither);
    if (s->channels == 2)
        flac_reduce_tpdf(s->decoded[1], s->blocksize,
                         s->bps - FLAC_OUTPUT_DEPTH, FLAC_OUTPUT_DEPTH,
                         &s->dither);
}
