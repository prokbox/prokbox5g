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
 * bcmfw/aac: AAC-LC on the VideoCore II, a thin adapter over Rockbox's
 * fixed-point libfaad (lib/rbcodec/codecs/libfaad, LC only, no SBR/PS).
 * The same sources build for the host as the bit-exact reference
 * (bcmfw/aac/test).  One decoder per image: libfaad's state is static.
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
#ifndef BCMFW_AAC_H
#define BCMFW_AAC_H

#include <stdint.h>

#define AAC_FRAME_LEN   1024    /* PCM frames per access unit (AAC-LC) */
#define AAC_MAX_AU      2036    /* libfaad's bit buffer (2048) less 12 */
#define AAC_PCM_BYTES   (AAC_FRAME_LEN * 2 * 2)    /* stereo, 16-bit */

/* Errors (negative); libfaad's own error codes are reported as
 * AAC_ERR_FAAD - code. */
#define AAC_ERR_ARG     (-1)    /* bad pointer or size */
#define AAC_ERR_CONFIG  (-2)    /* AudioSpecificConfig refused (not LC,
                                   more than 2 channels, 960-sample frames) */
#define AAC_ERR_STATE   (-3)    /* aac_decode before a successful aac_open */
#define AAC_ERR_FAAD    (-100)  /* -100 - libfaad error (1..32) */

/* Open the decoder from an AudioSpecificConfig (the esds
 * DecoderSpecificInfo bytes).  *rate and *channels (1 or 2) may be NULL.
 * Returns 0 or an error. */
int aac_open(const uint8_t *asc, uint32_t len, uint32_t *rate,
             uint32_t *channels);

/* Decode one raw access unit (no ADTS header) into interleaved 16-bit
 * stereo PCM, AAC_FRAME_LEN frames at most (mono is written to both
 * channels).  Returns the frames written: 0 for the decoder's first AU
 * after open or reset (libfaad's priming frame, as Rockbox sees it),
 * else AAC_FRAME_LEN; or an error, after which the next AU decodes.
 * The rounding is Rockbox's undithered output stage: (x + 2^13) >> 14,
 * clipped, from libfaad's samples (Rockbox sample depth 29:
 * frac_bits 29, output_scale 29 + 1 - 16). */
int aac_decode(const uint8_t *au, uint32_t len, int16_t *pcm);

/* After a seek: the overlap and window history are cleared and the next
 * AU primes again (returns 0 frames), as after aac_open. */
void aac_reset(void);

/* libfaad's last output before 16-bit conversion (time_out[ch], 1024
 * real_t each), for the bit-exactness tests. */
const int32_t *aac_raw_output(int ch);

/* Stage 9's dump-and-diff (session 36c: 155 of 200 AUs refused on the
 * chip, all exact in vc2sim): the last AU's parse checkpoints, as (tag,
 * value) pairs, cleared at each aac_decode.  The hooks are in libfaad's
 * syntax.c (AAC_TRACE, empty outside bcmfw/aac); the host build, vc2sim
 * and the chip record the same pairs, so their dumps diff line by line
 * (bcmfw/aac/test/aac_dev_sim.py). */
#define AAC_TRACE_PAIRS 40
#define AAC_T_ELE       1   /* element id << 16 | bit position after it */
#define AAC_T_CPE       2   /* ms_mask_present << 16 | bits */
#define AAC_T_ICS1      3   /* FNV-1a of the first ic_stream after the copy */
#define AAC_T_ICS2      4   /* ... of the second (the copy): equal to 3 */
#define AAC_T_INFO      5   /* window_sequence << 24 | shape << 20 |
                               max_sfb << 12 | groups << 8 | num_swb */
#define AAC_T_GAIN      6   /* global_gain << 16 | bits */
#define AAC_T_SECT      7   /* num_sec[0] << 16 | bits */
#define AAC_T_SF        8   /* bits after the scale factors */
#define AAC_T_SFHASH    9   /* FNV-1a of scale_factors[8][51] */
#define AAC_T_FLAGS     10  /* pulse << 22 | tns << 21 | gain control << 20
                               | bits */
#define AAC_T_SPEC      11  /* bits after the spectral data */
#define AAC_T_SPECHASH  12  /* FNV-1a of the channel's 1024 coefficients */
#define AAC_T_ERR       13  /* libfaad's error << 16 | bits */
#define AAC_T_DROPPED   15  /* pairs that did not fit */
struct aac_trace
{
    uint32_t n;                         /* pairs recorded */
    uint32_t pair[AAC_TRACE_PAIRS][2];
};
const struct aac_trace *aac_trace_get(void);

/* aac_open's flags (stage 9's A/B): AAC_FLAG_BYTE_COPY makes the decoder's
 * memcpy and memset go a byte at a time (session 36c's suspect: the
 * halfword copy of the second ic_stream into SDRAM, aac.c) */
#define AAC_FLAG_BYTE_COPY  0x1
/* AAC_FLAG_TRACE records the checkpoints above; off, the hooks return at
 * once (the hashes alone cost about 100 k instructions a stereo AU) */
#define AAC_FLAG_TRACE      0x2
void aac_set_flags(uint32_t flags);

#endif /* BCMFW_AAC_H */
