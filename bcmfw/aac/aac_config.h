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
 * bcmfw/aac/aac_config.h: the build environment of Rockbox's libfaad
 * (lib/rbcodec/codecs/libfaad) and its IMDCT (codecs/lib/mdct.c,
 * fft-ffmpeg.c) when they are compiled for our VideoCore II image, and
 * identically for the host reference.  Every libfaad file gets it with
 * -include, before anything else, so the upstream files stay untouched:
 *
 *  - Rockbox's target config.h is replaced (bcmfw/aac/sys/config.h);
 *    no CPU_ARM/CPU_COLDFIRE, so every libfaad and IMDCT multiply is the
 *    generic C one (fixed.h, codeclib_misc.h, mdct.h);
 *  - codeclib.h (codec API, malloc wrappers, platform headers) is
 *    replaced by the few declarations libfaad needs from it;
 *  - FIXED_POINT, LC_ONLY_DECODER, no SBR/PS (common.h's own defaults
 *    without CODEC_AAC_SBR_DEC and with CODEC_SIZE below 512 KB).
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
#ifndef BCMFW_AAC_CONFIG_H
#define BCMFW_AAC_CONFIG_H

#define BCMFW_AAC 1

#include <stddef.h>
#include <stdint.h>

/* --- Rockbox config.h stand-ins ------------------------------------- */
#define ROCKBOX_LITTLE_ENDIAN 1
#define CONFIG_CPU 0x2722           /* none of Rockbox's CPUs */
#define MCF5250 1
#define PP5022 2
#define PP5024 3
#define S5L8700 4
#define S5L8701 5
#define S5L8702 6
#define CODEC_SIZE 0x40000          /* below 512 KB: no PS_DEC */
/* no CODEC_AAC_SBR_DEC: no SBR */

/* One flat memory as far as the decoder knows; placement is the image's.
 * IBSS_ATTR keeps one property of Rockbox's: word alignment.  bits.c's
 * static_buffer (a uint8_t array) is read as words (bits.h getdword), and
 * a misaligned word load on VC II reads the rounded-down address and
 * faults (quirks.md #2); GCC does not promise to align a byte array. */
#define IBSS_ATTR __attribute__((aligned(4)))
#define ICONST_ATTR
#define ICODE_ATTR
#define IDATA_ATTR
#define MEM_ALIGN_ATTR __attribute__((aligned(16)))
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

/* --- codeclib.h stand-in --------------------------------------------- */
#define __CODECLIB_H__              /* the real one is skipped */
#include "mdct.h"                   /* codecs/lib: ff_imdct_calc, fixed32 */

void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);

/* M3: the decoder's copies and clears go to our own word-unrolled
 * versions (aac.c), on the host too.  libfaad clears about 41 KB and
 * copies about 11 KB per stereo AU (the channel-pair element, the IMDCT
 * buffer, the second ic_stream), mostly in blocks whose addresses are
 * only 2-aligned, which bcmfw's libc copies a byte at a time. */
void *aac_memcpy(void *dst, const void *src, size_t n);
void *aac_memset(void *dst, int c, size_t n);
#define memcpy aac_memcpy
#define memset aac_memset

/* Stage 9's parse checkpoints in libfaad's syntax.c (aac.h AAC_T_*) */
void aac_trace_put(uint32_t tag, uint32_t value);
void aac_trace_hash(uint32_t tag, const void *p, uint32_t bytes);
#define AAC_TRACE(tag, v)           aac_trace_put((tag), (uint32_t)(v))
#define AAC_TRACE_HASH(tag, p, n)   aac_trace_hash((tag), (p), (n))
#define AAC_T_ELE       1
#define AAC_T_CPE       2
#define AAC_T_ICS1      3
#define AAC_T_ICS2      4
#define AAC_T_INFO      5
#define AAC_T_GAIN      6
#define AAC_T_SECT      7
#define AAC_T_SF        8
#define AAC_T_SFHASH    9
#define AAC_T_FLAGS     10
#define AAC_T_SPEC      11
#define AAC_T_SPECHASH  12
#define AAC_T_ERR       13

/* Rockbox system.h's generic swap32 (bits.h's BSWAP) */
static inline uint32_t swap32(uint32_t v)
{
    return (v >> 24) | ((v >> 8) & 0xff00) | ((v << 8) & 0xff0000) | (v << 24);
}

#define BS_LOG2  0
#define BS_CLZ   1
#define BS_SHORT 2
#define BS_0_0   4
/* floor(log2(v)), 0 for v = 0: codeclib.h's bs_generic() in its BS_LOG2
 * personality (the only one libfaad uses, in common.c's log2_int/fix),
 * without the lookup table */
static inline unsigned int bs_generic(unsigned int v, int mode)
{
    unsigned int r = 0;
    (void)mode;
    if (v >= 0x10000) { v >>= 16; r += 16; }
    if (v >= 0x100)   { v >>= 8;  r += 8; }
    if (v >= 0x10)    { v >>= 4;  r += 4; }
    if (v >= 0x4)     { v >>= 2;  r += 2; }
    if (v >= 0x2)     { r += 1; }
    return r;
}

/* --- fixed.h's generic multiplies, without the 64-bit add ------------
 * libfaad's portable MUL_R/MUL_C/MUL_F/MUL_Q2/_MulHigh round a 64-bit
 * product: (a * b + 2^(s-1)) >> s.  vc4-elf-gcc builds that as mul,
 * mulhd.ss, an add with a carry test (a cs/cc branch) and three more
 * instructions.  The same value, exactly, from the two halves of the
 * product without a carry (M3; audio-spike.md section 7):
 *   with lo = the low word (unsigned), hi = the high word (signed),
 *   (hi * 2^32 + lo + 2^(s-1)) >> s
 *     = hi * 2^(32-s) + floor((floor(lo / 2^(s-1)) + 1) / 2)
 * (for 2 <= s <= 32; the result is taken modulo 2^32, as the cast in
 * fixed.h takes it).  fixed.h is read here first, its macros replaced;
 * its own #include then finds its guard. */
#define INLINE __inline
#include "fixed.h"
#undef MUL_R
#undef MUL_C
#undef MUL_F
#undef MUL_Q2
#undef _MulHigh
static inline int32_t aac_mul_round(int32_t a, int32_t b, const unsigned s)
{
    uint32_t lo = (uint32_t)a * (uint32_t)b;
    uint32_t hi = (uint32_t)(int32_t)(((int64_t)a * b) >> 32);

    return (int32_t)((hi << (32 - s)) + (((lo >> (s - 1)) + 1) >> 1));
}
#define MUL_R(A, B)    aac_mul_round((A), (B), REAL_BITS)
#define MUL_C(A, B)    aac_mul_round((A), (B), COEF_BITS)
#define MUL_F(A, B)    aac_mul_round((A), (B), FRAC_BITS)
#define MUL_Q2(A, B)   aac_mul_round((A), (B), Q2_BITS)
#define _MulHigh(A, B) aac_mul_round((A), (B), FRAC_SIZE)

/* No allocator: Rockbox's libfaad keeps all its state in statics
 * (FAAD_STATIC_ALLOC), and in the LC-only build nothing calls
 * faad_malloc (bits.c's only caller is under ERROR_RESILIENCE).  A call
 * that appears in a future version fails to link, by design. */

#endif /* BCMFW_AAC_CONFIG_H */
