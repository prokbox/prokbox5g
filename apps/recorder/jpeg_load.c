/***************************************************************************
*             __________               __   ___.
*   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
*   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
*   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
*   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
*                     \/            \/     \/    \/            \/
*
* JPEG image viewer
* (This is a real mess if it has to be coded in one single C file)
*
* Copyright (C) 2009 Andrew Mahone fractional decode, split IDCT - 16-point
*   IDCT based on IJG jpeg-7 pre-release
* File scrolling addition (C) 2005 Alexander Spyridakis
* Copyright (C) 2004 Jörg Hohensohn aka [IDC]Dragon
* Heavily borrowed from the IJG implementation (C) Thomas G. Lane
* Small & fast downscaling IDCT (C) 2002 by Guido Vollbeding  JPEGclub.org
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
#include "metadata_common.h"
#include "plugin.h"
#include "debug.h"
#include "jpeg_load.h"
#ifdef JPEG_JDEC
/* built for jdec (bcmfw/jobs/jdec.c, jpeg-spike.md sections 8-9): bad
   Huffman codes fail the decode, the rows can feed more outputs, and
   progressive files decode (bcmfw/jpeg/jprog.h); baseline scans take a
   table-driven entropy decoder with the same results (huff_block) */
#include "jhook.h"
#define HUFF_FAST 9 /* huff_block's lookahead */
static const struct jpeg_rows_tap *rows_tap;

void jpeg_set_rows_tap(const struct jpeg_rows_tap *tap)
{
    rows_tap = tap;
}

void (*jdec_yuv565)(fb_data *dst, const struct uint8_rgb *src, int n,
                    unsigned int dy);
#ifdef JDEC_ALONE
/* jdec's own builds (bcmfw/jpeg/env: the BCM image, its tests, the Mac's
   reference): the IDCT hook and its batching.  The PP's twin of jdec has
   no vector unit, so neither */
#define JPEG_IDCT8_HOOK
void (*jdec_idct)(uint8_t *px, int16_t *coef, int size);
void (*jdec_put)(const uint8_t *px, unsigned char *const *to, int n, int rowstep,
                 int size, int grey);
void (*jdec_fill)(unsigned int v, unsigned char *out, int grey, int rowstep,
                  int size);
#endif
#endif
#if defined(HAVE_IPOD_UI) && !defined(PLUGIN) && !defined(JDEC_ALONE)
/* iPod UI shell: run at each per-MCU yield (the art builder may park
 * there; apps/ipodui/artcache.c art_gate()) */
static void (*pause_hook)(void);

void jpeg_set_pause_hook(void (*fn)(void))
{
    pause_hook = fn;
}

/* ... and asked before each MCU row: true calls the decode off (it fails,
 * as a read error would; the Now Playing cover's render, np-sharp) */
static bool (*abort_hook)(void);

void jpeg_set_abort_hook(bool (*fn)(void))
{
    abort_hook = fn;
}
#endif
/*#define JPEG_BS_DEBUG*/
//#define ROCKBOX_DEBUG_JPEG
/* for portability of below JPEG code */
#define MEMSET(p,v,c) memset(p,v,c)
#define MEMCPY(d,s,c) memcpy(d,s,c)
#define INLINE static inline
#define ENDIAN_SWAP16(n) n /* only for poor little endian machines */
#ifdef ROCKBOX_DEBUG_JPEG
#define JDEBUGF DEBUGF
#else
#define JDEBUGF(...)
#endif

#if (__GNUC__ >= 6)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshift-negative-value"
#endif

/**************** begin JPEG code ********************/

#ifdef HAVE_LCD_COLOR
typedef struct uint8_rgb jpeg_pix_t;
#else
typedef uint8_t jpeg_pix_t;
#endif
#define JPEG_IDCT_TRANSPOSE
#define JPEG_PIX_SZ (sizeof(jpeg_pix_t))
/* index into the per-scale arrays: luma or chroma in colour builds; a
   greyscale build only decodes chroma for RGB, which is at the luma scale */
#ifdef HAVE_LCD_COLOR
#define SCALE_IDX(ci) (!!(ci))
#else
#define SCALE_IDX(ci) 0
#endif
#ifdef HAVE_LCD_COLOR
#define COLOR_EXTRA_IDCT_WS 64
#else
#define COLOR_EXTRA_IDCT_WS 0
#endif
#ifdef JPEG_IDCT_TRANSPOSE
#define V_OUT(n) ws2[8*n]
#define V_IN_ST 1
#define TRANSPOSE_EXTRA_IDCT_WS 64
#else
#define V_OUT(n) ws[8*n]
#define V_IN_ST 8
#define TRANSPOSE_EXTRA_IDCT_WS 0
#endif
#define IDCT_WS_SIZE (64 + TRANSPOSE_EXTRA_IDCT_WS + COLOR_EXTRA_IDCT_WS)

/* This can't be in jpeg_load.h because plugin.h includes it, and it conflicts
 * with the definition in jpeg_decoder.h
 */
struct jpeg
{
#ifdef JPEG_FROM_MEM
    unsigned char *data;
#else
    int fd;
    int buf_left;
    int buf_index;

    int (*read_buf)(struct jpeg* p_jpeg, size_t count);
    bool (*skip_bytes_seek)(struct jpeg* p_jpeg);
    void* custom_param;
#endif
    unsigned long len;
    unsigned long int bitbuf;
    int bitbuf_bits;
    int marker_ind;
    int marker_val;
    unsigned char marker;
    int x_size, y_size; /* size of image (can be less than block boundary) */
    int x_phys, y_phys; /* physical size, block aligned */
    int x_mbl; /* x dimension of MBL */
    int y_mbl; /* y dimension of MBL */
    int blocks; /* blocks per MB */
    int restart_interval; /* number of MCUs between RSTm markers */
    int restart; /* blocks until next restart marker */
    int mcu_row; /* current row relative to first row of this row of MCUs */
    unsigned char *out_ptr; /* pointer to current row to output */
    int cur_row; /* current row relative to top of image */
    int set_rows;
    int store_pos[4]; /* for Y block ordering */
    bool jfif; /* saw a JFIF APP0 marker */
    unsigned char adobe; /* Adobe APP14 transform flag + 1, 0 if none */
    bool rgb; /* the components are R, G, B rather than Y, Cb, Cr */
    int last_dc_val[3];
#ifdef HAVE_LCD_COLOR
    int h_scale[2]; /* horizontal scalefactor = (2**N) / 8 */
    int v_scale[2]; /* same as above, for vertical direction */
    int k_need[2]; /* per component zig-zag index of last needed coefficient */
    int zero_need[2]; /* per compenent number of coefficients to zero */
#else
    int h_scale[1]; /* horizontal scalefactor = (2**N) / 8 */
    int v_scale[1]; /* same as above, for vertical direction */
    int k_need[1]; /* per component zig-zag index of last needed coefficient */
    int zero_need[1]; /* per compenent number of coefficients to zero */
#endif
    jpeg_pix_t *img_buf;

    int16_t quanttable[4][QUANT_TABLE_LENGTH];/* raw quantization tables 0-3 */

    struct huffman_table hufftable[2]; /* Huffman tables  */
    struct derived_tbl dc_derived_tbls[2]; /* Huffman-LUTs */
    struct derived_tbl ac_derived_tbls[2];

    struct frame_component frameheader[3]; /* Component descriptor */
    struct scan_component scanheader[3]; /* Huffman tables per component */

    int mcu_membership[6]; /* component of each block */
    unsigned char mcu_pos[6]; /* position of each block in its component */
    bool resize;
#ifdef JPEG_JDEC
    /* the baseline entropy decoder's 9-bit tables (huff_block) */
    uint32_t dc_fast[2][1 << HUFF_FAST];
    uint32_t ac_fast[2][1 << HUFF_FAST];
    uint16_t ac_skip[2][1 << HUFF_FAST]; /* the skip loop's (fix_huff_fast) */
    int huff_errors; /* codes no table has (each decoded as a zero) */
    int tap_rows; /* rows given to the rows tap */
    bool progressive; /* SOF2 (jprog.h) */
    unsigned char prog_marker; /* the marker a progressive scan stopped at */
    int prog_mcu_y; /* the MCU row being made from the store */
    int eobrun;
    struct jpeg_prog {
        int16_t *coef; /* k coefficients a block */
        uint8_t *hist; /* hb bytes a block: coefficients k.. nonzero */
        int k, hb, bw, bh; /* bw x bh blocks */
    } prog[3];
#endif
#ifdef JPEG_IDCT8_HOOK
    /* jdec_idct's blocks (mcu_row_fast), a batch for each component
       scale (0 luma, 1 chroma): their coefficients and pixels, each from
       the first 32-byte boundary in its buffer (coef 0 between calls),
       where each block's pixels go, the next block of the batch's ring
       (the only one whose place is set), how many are taken, the size */
    struct idct_batch {
        int16_t coef[JDEC_IDCT_COEF + 16];
        uint8_t px[JDEC_IDCT_PX_SIZE + 32];
        unsigned char *to[4];
        struct hb_blk *cur;
        int n, size;
        int pad;        /* 1,632 bytes: what follows keeps its alignment,
                           32 bytes (the colour stage's rows) */
    } idct[2];
#endif
#ifndef JPEG_BULK_READ
    unsigned char buf[JPEG_READ_BUF_SIZE];
#endif
    struct img_part part;
#ifdef JPEG_BULK_READ
    /* last, so the other members keep short offsets; aligned for ATA DMA */
    unsigned char buf[JPEG_READ_BUF_SIZE] __attribute__((aligned(16)));
#endif
};

#ifdef JPEG_BULK_READ
#define JPEG_STRUCT_ALIGN 16
#else
#define JPEG_STRUCT_ALIGN sizeof(long)
#endif

#ifdef JPEG_JDEC
_Static_assert(sizeof(struct jpeg) + JPEG_STRUCT_ALIGN <= JPEG_JDEC_STATE,
               "jhook.h JPEG_JDEC_STATE");
#endif
#ifdef JPEG_FROM_MEM
static struct jpeg jpeg;
#endif

INLINE unsigned range_limit(int value)
{
#if defined(CPU_COLDFIRE)
    /* Note: Uses knowledge that only the low byte of the result is used */
    asm (
        "cmp.l   #255,%[v]   \n"  /* overflow? */
        "bls.b   1f          \n"  /* no: return value */
        /* yes: set low byte to appropriate boundary */
        "spl.b   %[v]        \n"
    "1:                      \n"
        : /* outputs */
        [v]"+d"(value)
    );
    return value;
#elif defined(CPU_ARM)
    /* Note: Uses knowledge that only the low byte of the result is used */
    asm (
        "cmp     %[v], #255          \n"  /* out of range 0..255? */
        "mvnhi   %[v], %[v], asr #31 \n"  /* yes: set all bits to ~(sign_bit) */
        : /* outputs */
        [v]"+r"(value)
    );
    return value;
#else
    if ((unsigned)value <= 255)
        return value;

    if (value < 0)
        return 0;

    return 255;
#endif
}

INLINE unsigned scale_output(int value)
{
#if defined(CPU_ARM) && ARM_ARCH >= 6
    asm (
        "usat %[v], #8, %[v], asr #18\n"
        : [v] "+r" (value)
    );
    return value;
#else
    return range_limit(value >> 18);
#endif
}

/* IDCT implementation */


#define CONST_BITS 13
#define PASS1_BITS 2


/* Some C compilers fail to reduce "FIX(constant)" at compile time, thus
* causing a lot of useless floating-point operations at run time.
* To get around this we use the following pre-calculated constants.
* If you change CONST_BITS you may want to add appropriate values.
* (With a reasonable C compiler, you can just rely on the FIX() macro...)
*/
#define FIX_0_298631336  2446 /* FIX(0.298631336) */
#define FIX_0_390180644  3196 /* FIX(0.390180644) */
#define FIX_0_541196100  4433 /* FIX(0.541196100) */
#define FIX_0_765366865  6270 /* FIX(0.765366865) */
#define FIX_0_899976223  7373 /* FIX(0.899976223) */
#define FIX_1_175875602  9633 /* FIX(1.175875602) */
#define FIX_1_501321110 12299 /* FIX(1.501321110) */
#define FIX_1_847759065 15137 /* FIX(1.847759065) */
#define FIX_1_961570560 16069 /* FIX(1.961570560) */
#define FIX_2_053119869 16819 /* FIX(2.053119869) */
#define FIX_2_562915447 20995 /* FIX(2.562915447) */
#define FIX_3_072711026 25172 /* FIX(3.072711026) */



/* Multiply an long variable by an long constant to yield an long result.
* For 8-bit samples with the recommended scaling, all the variable
* and constant values involved are no more than 16 bits wide, so a
* 16x16->32 bit multiply can be used instead of a full 32x32 multiply.
* For 12-bit samples, a full 32-bit multiplication will be needed.
*/
#define MULTIPLY(var1, var2) ((var1) * (var2))

#if defined(CPU_COLDFIRE) || \
    (defined(CPU_ARM) && ARM_ARCH > 4)
#define MULTIPLY16(var,const)  (((short) (var)) * ((short) (const)))
#else
#define MULTIPLY16 MULTIPLY
#endif

/*
 * Macros for handling fixed-point arithmetic; these are used by many
 * but not all of the DCT/IDCT modules.
 *
 * All values are expected to be of type INT32.
 * Fractional constants are scaled left by CONST_BITS bits.
 * CONST_BITS is defined within each module using these macros,
 * and may differ from one module to the next.
 */
#define ONE ((long)1)
#define CONST_SCALE (ONE << CONST_BITS)

/* Convert a positive real constant to an integer scaled by CONST_SCALE.
 * Caution: some C compilers fail to reduce "FIX(constant)" at compile time,
 * thus causing a lot of useless floating-point operations at run time.
 */
#define FIX(x) ((long) ((x) * CONST_SCALE + 0.5))
#define RIGHT_SHIFT(x,shft)     ((x) >> (shft))

/* Descale and correctly round an int value that's scaled by N bits.
* We assume RIGHT_SHIFT rounds towards minus infinity, so adding
* the fudge factor is correct for either sign of X.
*/
#define DESCALE(x,n) (((x) + (1l << ((n)-1))) >> (n))

#define DS_OUT ((CONST_BITS)+(PASS1_BITS)+3)

/*
 * Conversion of full 0-255 range YCrCb to RGB:
 *   |R|   |1.000000 -0.000001  1.402000| |Y'|
 *   |G| = |1.000000 -0.334136 -0.714136| |Pb|
 *   |B|   |1.000000  1.772000  0.000000| |Pr|
 * Scaled (yields s15-bit output):
 *   |R|   |128    0  179| |Y       |
 *   |G| = |128  -43  -91| |Cb - 128|
 *   |B|   |128  227    0| |Cr - 128|
 */
#define YFAC            128
#define RVFAC           179
#define GUFAC           (-43)
#define GVFAC           (-91)
#define BUFAC           227
#define COMPONENT_SHIFT  15

#ifndef CPU_ARM
/* horizontal-pass 1-point IDCT */
static void jpeg_idct1h(int16_t *ws, unsigned char *out, int16_t *end, int rowstep)
{
    for (; ws < end; ws += 8)
    {
        *out = range_limit(128 + (int) DESCALE(*ws, 3 + PASS1_BITS));
        out += rowstep;
    }
}

/* vertical-pass 2-point IDCT */
static void jpeg_idct2v(int16_t *ws, int16_t *end)
{
    for (; ws < end; ws++)
    {
        int tmp1 = ws[0*8];
        int tmp2 = ws[1*8];
        ws[0*8] = tmp1 + tmp2;
        ws[1*8] = tmp1 - tmp2;
    }
}

/* horizontal-pass 2-point IDCT */
static void jpeg_idct2h(int16_t *ws, unsigned char *out, int16_t *end, int rowstep)
{
    for (; ws < end; ws += 8, out += rowstep)
    {
        int tmp1 = ws[0] + (ONE << (PASS1_BITS + 2))
                   + (128 << (PASS1_BITS + 3));
        int tmp2 = ws[1];
        out[JPEG_PIX_SZ*0] = range_limit((int) RIGHT_SHIFT(tmp1 + tmp2,
            PASS1_BITS + 3));
        out[JPEG_PIX_SZ*1] = range_limit((int) RIGHT_SHIFT(tmp1 - tmp2,
            PASS1_BITS + 3));
    }
}

/* vertical-pass 4-point IDCT */
static void jpeg_idct4v(int16_t *ws, int16_t *end)
{
    for (; ws < end; ws++)
    {
        int tmp0, tmp2, tmp10, tmp12;
        int z1, z2, z3;
        /* Even part */

        tmp0 = ws[8*0];
        tmp2 = ws[8*2];

        tmp10 = (tmp0 + tmp2) << PASS1_BITS;
        tmp12 = (tmp0 - tmp2) << PASS1_BITS;

        /* Odd part */
        /* Same rotation as in the even part of the 8x8 LL&M IDCT */

        z2 = ws[8*1];
        z3 = ws[8*3];

        z1 = MULTIPLY16(z2 + z3, FIX_0_541196100) +
            (ONE << (CONST_BITS - PASS1_BITS - 1));
        tmp0 = RIGHT_SHIFT(z1 + MULTIPLY16(z3, - FIX_1_847759065),
            CONST_BITS-PASS1_BITS);
        tmp2 = RIGHT_SHIFT(z1 + MULTIPLY16(z2, FIX_0_765366865),
            CONST_BITS-PASS1_BITS);

        /* Final output stage */
        ws[8*0] = (int) (tmp10 + tmp2);
        ws[8*3] = (int) (tmp10 - tmp2);
        ws[8*1] = (int) (tmp12 + tmp0);
        ws[8*2] = (int) (tmp12 - tmp0);
    }
}

/* horizontal-pass 4-point IDCT */
static void jpeg_idct4h(int16_t *ws, unsigned char *out, int16_t *end, int rowstep)
{
    for (; ws < end; out += rowstep, ws += 8)
    {
        int tmp0, tmp2, tmp10, tmp12;
        int z1, z2, z3;
        /* Even part */

        tmp0 = (int) ws[0] + (ONE << (PASS1_BITS + 2))
               + (128 << (PASS1_BITS + 3));
        tmp2 = (int) ws[2];

        tmp10 = (tmp0 + tmp2) << CONST_BITS;
        tmp12 = (tmp0 - tmp2) << CONST_BITS;

        /* Odd part */
        /* Same rotation as in the even part of the 8x8 LL&M IDCT */

        z2 = (int) ws[1];
        z3 = (int) ws[3];

        z1 = MULTIPLY16(z2 + z3, FIX_0_541196100);
        tmp0 = z1 - MULTIPLY16(z3, FIX_1_847759065);
        tmp2 = z1 + MULTIPLY16(z2, FIX_0_765366865);

        /* Final output stage */

        out[JPEG_PIX_SZ*0] = range_limit((int) RIGHT_SHIFT(tmp10 + tmp2,
            DS_OUT));
        out[JPEG_PIX_SZ*3] = range_limit((int) RIGHT_SHIFT(tmp10 - tmp2,
            DS_OUT));
        out[JPEG_PIX_SZ*1] = range_limit((int) RIGHT_SHIFT(tmp12 + tmp0,
            DS_OUT));
        out[JPEG_PIX_SZ*2] = range_limit((int) RIGHT_SHIFT(tmp12 - tmp0,
            DS_OUT));
    }
}

/* vertical-pass 8-point IDCT */
static void jpeg_idct8v(int16_t *ws, int16_t *end)
{
    long tmp0, tmp1, tmp2, tmp3;
    long tmp10, tmp11, tmp12, tmp13;
    long z1, z2, z3, z4, z5;
#ifdef JPEG_IDCT_TRANSPOSE
    int16_t *ws2 = ws + 64;
    for (; ws < end; ws += 8, ws2++)
    {
#else
    for (; ws < end; ws++)
    {
#endif
    /* Due to quantization, we will usually find that many of the input
    * coefficients are zero, especially the AC terms.  We can exploit this
    * by short-circuiting the IDCT calculation for any column in which all
    * the AC terms are zero.  In that case each output is equal to the
    * DC coefficient (with scale factor as needed).
    * With typical images and quantization tables, half or more of the
    * column DCT calculations can be simplified this way.
    */
        if ((ws[V_IN_ST*1] | ws[V_IN_ST*2] | ws[V_IN_ST*3]
           | ws[V_IN_ST*4] | ws[V_IN_ST*5] | ws[V_IN_ST*6] | ws[V_IN_ST*7]) == 0)
        {
            /* AC terms all zero */
            int dcval = ws[V_IN_ST*0] << PASS1_BITS;

            V_OUT(0) = V_OUT(1) = V_OUT(2) = V_OUT(3) = V_OUT(4) = V_OUT(5) =
                       V_OUT(6) = V_OUT(7) = dcval;
            continue;
        }

        /* Even part: reverse the even part of the forward DCT. */
        /* The rotator is sqrt(2)*c(-6). */

        z2 = ws[V_IN_ST*2];
        z3 = ws[V_IN_ST*6];

        z1 = MULTIPLY16(z2 + z3, FIX_0_541196100);
        tmp2 = z1 + MULTIPLY16(z3, - FIX_1_847759065);
        tmp3 = z1 + MULTIPLY16(z2, FIX_0_765366865);

        z2 = ws[V_IN_ST*0] << CONST_BITS;
        z2 += ONE << (CONST_BITS - PASS1_BITS - 1);
        z3 = ws[V_IN_ST*4] << CONST_BITS;

        tmp0 = (z2 + z3);
        tmp1 = (z2 - z3);

        tmp10 = tmp0 + tmp3;
        tmp13 = tmp0 - tmp3;
        tmp11 = tmp1 + tmp2;
        tmp12 = tmp1 - tmp2;

        /* Odd part per figure 8; the matrix is unitary and hence its
           transpose is its inverse.  i0..i3 are y7,y5,y3,y1 respectively. */

        tmp0 = ws[V_IN_ST*7];
        tmp1 = ws[V_IN_ST*5];
        tmp2 = ws[V_IN_ST*3];
        tmp3 = ws[V_IN_ST*1];

        z1 = tmp0 + tmp3;
        z2 = tmp1 + tmp2;
        z3 = tmp0 + tmp2;
        z4 = tmp1 + tmp3;
        z5 = MULTIPLY16(z3 + z4, FIX_1_175875602); /* sqrt(2) * c3 */

        tmp0 = MULTIPLY16(tmp0, FIX_0_298631336); /* sqrt(2) * (-c1+c3+c5-c7) */
        tmp1 = MULTIPLY16(tmp1, FIX_2_053119869); /* sqrt(2) * ( c1+c3-c5+c7) */
        tmp2 = MULTIPLY16(tmp2, FIX_3_072711026); /* sqrt(2) * ( c1+c3+c5-c7) */
        tmp3 = MULTIPLY16(tmp3, FIX_1_501321110); /* sqrt(2) * ( c1+c3-c5-c7) */
        z1 = MULTIPLY16(z1, - FIX_0_899976223); /* sqrt(2) * (c7-c3) */
        z2 = MULTIPLY16(z2, - FIX_2_562915447); /* sqrt(2) * (-c1-c3) */
        z3 = MULTIPLY16(z3, - FIX_1_961570560); /* sqrt(2) * (-c3-c5) */
        z4 = MULTIPLY16(z4, - FIX_0_390180644); /* sqrt(2) * (c5-c3) */

        z3 += z5;
        z4 += z5;

        tmp0 += z1 + z3;
        tmp1 += z2 + z4;
        tmp2 += z2 + z3;
        tmp3 += z1 + z4;

        /* Final output stage: inputs are tmp10..tmp13, tmp0..tmp3 */

        V_OUT(0) = (int) RIGHT_SHIFT(tmp10 + tmp3, CONST_BITS-PASS1_BITS);
        V_OUT(7) = (int) RIGHT_SHIFT(tmp10 - tmp3, CONST_BITS-PASS1_BITS);
        V_OUT(1) = (int) RIGHT_SHIFT(tmp11 + tmp2, CONST_BITS-PASS1_BITS);
        V_OUT(6) = (int) RIGHT_SHIFT(tmp11 - tmp2, CONST_BITS-PASS1_BITS);
        V_OUT(2) = (int) RIGHT_SHIFT(tmp12 + tmp1, CONST_BITS-PASS1_BITS);
        V_OUT(5) = (int) RIGHT_SHIFT(tmp12 - tmp1, CONST_BITS-PASS1_BITS);
        V_OUT(3) = (int) RIGHT_SHIFT(tmp13 + tmp0, CONST_BITS-PASS1_BITS);
        V_OUT(4) = (int) RIGHT_SHIFT(tmp13 - tmp0, CONST_BITS-PASS1_BITS);
    }
}

/* horizontal-pass 8-point IDCT */
static void jpeg_idct8h(int16_t *ws, unsigned char *out, int16_t *end, int rowstep)
{
    long tmp0, tmp1, tmp2, tmp3;
    long tmp10, tmp11, tmp12, tmp13;
    long z1, z2, z3, z4, z5;
    for (; ws < end; out += rowstep, ws += 8)
    {
        /* Rows of zeroes can be exploited in the same way as we did with
         * columns. However, the column calculation has created many nonzero AC
         * terms, so the simplification applies less often (typically 5% to 10%
         * of the time). On machines with very fast multiplication, it's
         * possible that the test takes more time than it's worth.  In that
         * case this section may be commented out.
        */

#ifndef NO_ZERO_ROW_TEST
        if ((ws[1] | ws[2] | ws[3]
           | ws[4] | ws[5] | ws[6] | ws[7]) == 0)
        {
            /* AC terms all zero */
            unsigned char dcval = range_limit(128 + (int) DESCALE((long) ws[0],
                PASS1_BITS+3));

            out[JPEG_PIX_SZ*0] = dcval;
            out[JPEG_PIX_SZ*1] = dcval;
            out[JPEG_PIX_SZ*2] = dcval;
            out[JPEG_PIX_SZ*3] = dcval;
            out[JPEG_PIX_SZ*4] = dcval;
            out[JPEG_PIX_SZ*5] = dcval;
            out[JPEG_PIX_SZ*6] = dcval;
            out[JPEG_PIX_SZ*7] = dcval;
            continue;
        }
#endif

        /* Even part: reverse the even part of the forward DCT. */
        /* The rotator is sqrt(2)*c(-6). */

        z2 = (long) ws[2];
        z3 = (long) ws[6];

        z1 = MULTIPLY16(z2 + z3, FIX_0_541196100);
        tmp2 = z1 + MULTIPLY16(z3, - FIX_1_847759065);
        tmp3 = z1 + MULTIPLY16(z2, FIX_0_765366865);

        z4 = (long) ws[0] + (ONE << (PASS1_BITS + 2))
             + (128 << (PASS1_BITS + 3));
        z4 <<= CONST_BITS;
        z5 = (long) ws[4] << CONST_BITS;
        tmp0 = z4 + z5;
        tmp1 = z4 - z5;

        tmp10 = tmp0 + tmp3;
        tmp13 = tmp0 - tmp3;
        tmp11 = tmp1 + tmp2;
        tmp12 = tmp1 - tmp2;

        /* Odd part per figure 8; the matrix is unitary and hence its
        * transpose is its inverse. i0..i3 are y7,y5,y3,y1 respectively. */

        tmp0 = (long) ws[7];
        tmp1 = (long) ws[5];
        tmp2 = (long) ws[3];
        tmp3 = (long) ws[1];

        z1 = tmp0 + tmp3;
        z2 = tmp1 + tmp2;
        z3 = tmp0 + tmp2;
        z4 = tmp1 + tmp3;
        z5 = MULTIPLY16(z3 + z4, FIX_1_175875602); /* sqrt(2) * c3 */

        tmp0 = MULTIPLY16(tmp0, FIX_0_298631336); /* sqrt(2) * (-c1+c3+c5-c7) */
        tmp1 = MULTIPLY16(tmp1, FIX_2_053119869); /* sqrt(2) * ( c1+c3-c5+c7) */
        tmp2 = MULTIPLY16(tmp2, FIX_3_072711026); /* sqrt(2) * ( c1+c3+c5-c7) */
        tmp3 = MULTIPLY16(tmp3, FIX_1_501321110); /* sqrt(2) * ( c1+c3-c5-c7) */
        z1 = MULTIPLY16(z1, - FIX_0_899976223); /* sqrt(2) * (c7-c3) */
        z2 = MULTIPLY16(z2, - FIX_2_562915447); /* sqrt(2) * (-c1-c3) */
        z3 = MULTIPLY16(z3, - FIX_1_961570560); /* sqrt(2) * (-c3-c5) */
        z4 = MULTIPLY16(z4, - FIX_0_390180644); /* sqrt(2) * (c5-c3) */

        z3 += z5;
        z4 += z5;

        tmp0 += z1 + z3;
        tmp1 += z2 + z4;
        tmp2 += z2 + z3;
        tmp3 += z1 + z4;

        /* Final output stage: inputs are tmp10..tmp13, tmp0..tmp3 */

        out[JPEG_PIX_SZ*0] = range_limit((int) RIGHT_SHIFT(tmp10 + tmp3,
            DS_OUT));
        out[JPEG_PIX_SZ*7] = range_limit((int) RIGHT_SHIFT(tmp10 - tmp3,
            DS_OUT));
        out[JPEG_PIX_SZ*1] = range_limit((int) RIGHT_SHIFT(tmp11 + tmp2,
            DS_OUT));
        out[JPEG_PIX_SZ*6] = range_limit((int) RIGHT_SHIFT(tmp11 - tmp2,
            DS_OUT));
        out[JPEG_PIX_SZ*2] = range_limit((int) RIGHT_SHIFT(tmp12 + tmp1,
            DS_OUT));
        out[JPEG_PIX_SZ*5] = range_limit((int) RIGHT_SHIFT(tmp12 - tmp1,
            DS_OUT));
        out[JPEG_PIX_SZ*3] = range_limit((int) RIGHT_SHIFT(tmp13 + tmp0,
            DS_OUT));
        out[JPEG_PIX_SZ*4] = range_limit((int) RIGHT_SHIFT(tmp13 - tmp0,
            DS_OUT));
    }
}

#else
extern void jpeg_idct1h(int16_t *ws, unsigned char *out, int16_t *end, int rowstep);
extern void jpeg_idct2v(int16_t *ws, int16_t *end);
extern void jpeg_idct2h(int16_t *ws, unsigned char *out, int16_t *end, int rowstep);
extern void jpeg_idct4v(int16_t *ws, int16_t *end);
extern void jpeg_idct4h(int16_t *ws, unsigned char *out, int16_t *end, int rowstep);
extern void jpeg_idct8v(int16_t *ws, int16_t *end);
extern void jpeg_idct8h(int16_t *ws, unsigned char *out, int16_t *end, int rowstep);
#endif

#ifdef HAVE_LCD_COLOR
/* vertical-pass 16-point IDCT */
static void jpeg_idct16v(int16_t *ws, int16_t *end)
{
    long tmp0, tmp1, tmp2, tmp3, tmp10, tmp11, tmp12, tmp13;
    long tmp20, tmp21, tmp22, tmp23, tmp24, tmp25, tmp26, tmp27;
    long z1, z2, z3, z4;
#ifdef JPEG_IDCT_TRANSPOSE
    int16_t *ws2 = ws + 64;
    for (; ws < end; ws += 8, ws2++)
    {
#else
    for (; ws < end; ws++)
    {
#endif
        /* Even part */

        tmp0 = ws[V_IN_ST*0] << CONST_BITS;
        /* Add fudge factor here for final descale. */
        tmp0 += 1 << (CONST_BITS-PASS1_BITS-1);

        z1 = ws[V_IN_ST*4];
        tmp1 = MULTIPLY(z1, FIX(1.306562965));      /* c4[16] = c2[8] */
        tmp2 = MULTIPLY(z1, FIX_0_541196100);       /* c12[16] = c6[8] */

        tmp10 = tmp0 + tmp1;
        tmp11 = tmp0 - tmp1;
        tmp12 = tmp0 + tmp2;
        tmp13 = tmp0 - tmp2;

        z1 = ws[V_IN_ST*2];
        z2 = ws[V_IN_ST*6];
        z3 = z1 - z2;
        z4 = MULTIPLY(z3, FIX(0.275899379));        /* c14[16] = c7[8] */
        z3 = MULTIPLY(z3, FIX(1.387039845));        /* c2[16] = c1[8] */

        /* (c6+c2)[16] = (c3+c1)[8] */
        tmp0 = z3 + MULTIPLY(z2, FIX_2_562915447);
        /* (c6-c14)[16] = (c3-c7)[8] */
        tmp1 = z4 + MULTIPLY(z1, FIX_0_899976223);
        /* (c2-c10)[16] = (c1-c5)[8] */
        tmp2 = z3 - MULTIPLY(z1, FIX(0.601344887));
        /* (c10-c14)[16] = (c5-c7)[8] */
        tmp3 = z4 - MULTIPLY(z2, FIX(0.509795579));

        tmp20 = tmp10 + tmp0;
        tmp27 = tmp10 - tmp0;
        tmp21 = tmp12 + tmp1;
        tmp26 = tmp12 - tmp1;
        tmp22 = tmp13 + tmp2;
        tmp25 = tmp13 - tmp2;
        tmp23 = tmp11 + tmp3;
        tmp24 = tmp11 - tmp3;

        /* Odd part */

        z1 = ws[V_IN_ST*1];
        z2 = ws[V_IN_ST*3];
        z3 = ws[V_IN_ST*5];
        z4 = ws[V_IN_ST*7];

        tmp11 = z1 + z3;

        tmp1  = MULTIPLY(z1 + z2, FIX(1.353318001));   /* c3 */
        tmp2  = MULTIPLY(tmp11,   FIX(1.247225013));   /* c5 */
        tmp3  = MULTIPLY(z1 + z4, FIX(1.093201867));   /* c7 */
        tmp10 = MULTIPLY(z1 - z4, FIX(0.897167586));   /* c9 */
        tmp11 = MULTIPLY(tmp11,   FIX(0.666655658));   /* c11 */
        tmp12 = MULTIPLY(z1 - z2, FIX(0.410524528));   /* c13 */
        tmp0  = tmp1 + tmp2 + tmp3 -
            MULTIPLY(z1, FIX(2.286341144));        /* c7+c5+c3-c1 */
        tmp13 = tmp10 + tmp11 + tmp12 -
            MULTIPLY(z1, FIX(1.835730603));        /* c9+c11+c13-c15 */
        z1    = MULTIPLY(z2 + z3, FIX(0.138617169));   /* c15 */
        tmp1  += z1 + MULTIPLY(z2, FIX(0.071888074));  /* c9+c11-c3-c15 */
        tmp2  += z1 - MULTIPLY(z3, FIX(1.125726048));  /* c5+c7+c15-c3 */
        z1    = MULTIPLY(z3 - z2, FIX(1.407403738));   /* c1 */
        tmp11 += z1 - MULTIPLY(z3, FIX(0.766367282));  /* c1+c11-c9-c13 */
        tmp12 += z1 + MULTIPLY(z2, FIX(1.971951411));  /* c1+c5+c13-c7 */
        z2    += z4;
        z1    = MULTIPLY(z2, - FIX(0.666655658));      /* -c11 */
        tmp1  += z1;
        tmp3  += z1 + MULTIPLY(z4, FIX(1.065388962));  /* c3+c11+c15-c7 */
        z2    = MULTIPLY(z2, - FIX(1.247225013));      /* -c5 */
        tmp10 += z2 + MULTIPLY(z4, FIX(3.141271809));  /* c1+c5+c9-c13 */
        tmp12 += z2;
        z2    = MULTIPLY(z3 + z4, - FIX(1.353318001)); /* -c3 */
        tmp2  += z2;
        tmp3  += z2;
        z2    = MULTIPLY(z4 - z3, FIX(0.410524528));   /* c13 */
        tmp10 += z2;
        tmp11 += z2;

        /* Final output stage */
        V_OUT(0)  = (int) RIGHT_SHIFT(tmp20 + tmp0,  CONST_BITS-PASS1_BITS);
        V_OUT(15) = (int) RIGHT_SHIFT(tmp20 - tmp0,  CONST_BITS-PASS1_BITS);
        V_OUT(1)  = (int) RIGHT_SHIFT(tmp21 + tmp1,  CONST_BITS-PASS1_BITS);
        V_OUT(14) = (int) RIGHT_SHIFT(tmp21 - tmp1,  CONST_BITS-PASS1_BITS);
        V_OUT(2)  = (int) RIGHT_SHIFT(tmp22 + tmp2,  CONST_BITS-PASS1_BITS);
        V_OUT(13) = (int) RIGHT_SHIFT(tmp22 - tmp2,  CONST_BITS-PASS1_BITS);
        V_OUT(3)  = (int) RIGHT_SHIFT(tmp23 + tmp3,  CONST_BITS-PASS1_BITS);
        V_OUT(12) = (int) RIGHT_SHIFT(tmp23 - tmp3,  CONST_BITS-PASS1_BITS);
        V_OUT(4)  = (int) RIGHT_SHIFT(tmp24 + tmp10, CONST_BITS-PASS1_BITS);
        V_OUT(11) = (int) RIGHT_SHIFT(tmp24 - tmp10, CONST_BITS-PASS1_BITS);
        V_OUT(5)  = (int) RIGHT_SHIFT(tmp25 + tmp11, CONST_BITS-PASS1_BITS);
        V_OUT(10) = (int) RIGHT_SHIFT(tmp25 - tmp11, CONST_BITS-PASS1_BITS);
        V_OUT(6)  = (int) RIGHT_SHIFT(tmp26 + tmp12, CONST_BITS-PASS1_BITS);
        V_OUT(9)  = (int) RIGHT_SHIFT(tmp26 - tmp12, CONST_BITS-PASS1_BITS);
        V_OUT(7)  = (int) RIGHT_SHIFT(tmp27 + tmp13, CONST_BITS-PASS1_BITS);
        V_OUT(8)  = (int) RIGHT_SHIFT(tmp27 - tmp13, CONST_BITS-PASS1_BITS);
    }
}

/* horizontal-pass 16-point IDCT */
static void jpeg_idct16h(int16_t *ws, unsigned char *out, int16_t *end, int rowstep)
{
    long tmp0, tmp1, tmp2, tmp3, tmp10, tmp11, tmp12, tmp13;
    long tmp20, tmp21, tmp22, tmp23, tmp24, tmp25, tmp26, tmp27;
    long z1, z2, z3, z4;
    for (; ws < end; out += rowstep, ws += 8)
    {
        /* Even part */

        /* Add fudge factor here for final descale. */
        tmp0 = (long) ws[0] + (ONE << (PASS1_BITS+2))
               + (128 << (PASS1_BITS + 3));
        tmp0 <<= CONST_BITS;

        z1 = (long) ws[4];
        tmp1 = MULTIPLY(z1, FIX(1.306562965));      /* c4[16] = c2[8] */
        tmp2 = MULTIPLY(z1, FIX_0_541196100);       /* c12[16] = c6[8] */

        tmp10 = tmp0 + tmp1;
        tmp11 = tmp0 - tmp1;
        tmp12 = tmp0 + tmp2;
        tmp13 = tmp0 - tmp2;

        z1 = (long) ws[2];
        z2 = (long) ws[6];
        z3 = z1 - z2;
        z4 = MULTIPLY(z3, FIX(0.275899379));        /* c14[16] = c7[8] */
        z3 = MULTIPLY(z3, FIX(1.387039845));        /* c2[16] = c1[8] */

        /* (c6+c2)[16] = (c3+c1)[8] */
        tmp0 = z3 + MULTIPLY(z2, FIX_2_562915447);
        /* (c6-c14)[16] = (c3-c7)[8] */
        tmp1 = z4 + MULTIPLY(z1, FIX_0_899976223);
        /* (c2-c10)[16] = (c1-c5)[8] */
        tmp2 = z3 - MULTIPLY(z1, FIX(0.601344887));
        /* (c10-c14)[16] = (c5-c7)[8] */
        tmp3 = z4 - MULTIPLY(z2, FIX(0.509795579));

        tmp20 = tmp10 + tmp0;
        tmp27 = tmp10 - tmp0;
        tmp21 = tmp12 + tmp1;
        tmp26 = tmp12 - tmp1;
        tmp22 = tmp13 + tmp2;
        tmp25 = tmp13 - tmp2;
        tmp23 = tmp11 + tmp3;
        tmp24 = tmp11 - tmp3;

        /* Odd part */

        z1 = (long) ws[1];
        z2 = (long) ws[3];
        z3 = (long) ws[5];
        z4 = (long) ws[7];

        tmp11 = z1 + z3;

        tmp1  = MULTIPLY(z1 + z2, FIX(1.353318001));   /* c3 */
        tmp2  = MULTIPLY(tmp11,   FIX(1.247225013));   /* c5 */
        tmp3  = MULTIPLY(z1 + z4, FIX(1.093201867));   /* c7 */
        tmp10 = MULTIPLY(z1 - z4, FIX(0.897167586));   /* c9 */
        tmp11 = MULTIPLY(tmp11,   FIX(0.666655658));   /* c11 */
        tmp12 = MULTIPLY(z1 - z2, FIX(0.410524528));   /* c13 */
        tmp0  = tmp1 + tmp2 + tmp3 -
            MULTIPLY(z1, FIX(2.286341144));        /* c7+c5+c3-c1 */
        tmp13 = tmp10 + tmp11 + tmp12 -
            MULTIPLY(z1, FIX(1.835730603));        /* c9+c11+c13-c15 */
        z1    = MULTIPLY(z2 + z3, FIX(0.138617169));   /* c15 */
        tmp1  += z1 + MULTIPLY(z2, FIX(0.071888074));  /* c9+c11-c3-c15 */
        tmp2  += z1 - MULTIPLY(z3, FIX(1.125726048));  /* c5+c7+c15-c3 */
        z1    = MULTIPLY(z3 - z2, FIX(1.407403738));   /* c1 */
        tmp11 += z1 - MULTIPLY(z3, FIX(0.766367282));  /* c1+c11-c9-c13 */
        tmp12 += z1 + MULTIPLY(z2, FIX(1.971951411));  /* c1+c5+c13-c7 */
        z2    += z4;
        z1    = MULTIPLY(z2, - FIX(0.666655658));      /* -c11 */
        tmp1  += z1;
        tmp3  += z1 + MULTIPLY(z4, FIX(1.065388962));  /* c3+c11+c15-c7 */
        z2    = MULTIPLY(z2, - FIX(1.247225013));      /* -c5 */
        tmp10 += z2 + MULTIPLY(z4, FIX(3.141271809));  /* c1+c5+c9-c13 */
        tmp12 += z2;
        z2    = MULTIPLY(z3 + z4, - FIX(1.353318001)); /* -c3 */
        tmp2  += z2;
        tmp3  += z2;
        z2    = MULTIPLY(z4 - z3, FIX(0.410524528));   /* c13 */
        tmp10 += z2;
        tmp11 += z2;

        /* Final output stage */

        out[JPEG_PIX_SZ*0]  = scale_output(tmp20 + tmp0);
        out[JPEG_PIX_SZ*15] = scale_output(tmp20 - tmp0);
        out[JPEG_PIX_SZ*1]  = scale_output(tmp21 + tmp1);
        out[JPEG_PIX_SZ*14] = scale_output(tmp21 - tmp1);
        out[JPEG_PIX_SZ*2]  = scale_output(tmp22 + tmp2);
        out[JPEG_PIX_SZ*13] = scale_output(tmp22 - tmp2);
        out[JPEG_PIX_SZ*3]  = scale_output(tmp23 + tmp3);
        out[JPEG_PIX_SZ*12] = scale_output(tmp23 - tmp3);
        out[JPEG_PIX_SZ*4]  = scale_output(tmp24 + tmp10);
        out[JPEG_PIX_SZ*11] = scale_output(tmp24 - tmp10);
        out[JPEG_PIX_SZ*5]  = scale_output(tmp25 + tmp11);
        out[JPEG_PIX_SZ*10] = scale_output(tmp25 - tmp11);
        out[JPEG_PIX_SZ*6]  = scale_output(tmp26 + tmp12);
        out[JPEG_PIX_SZ*9]  = scale_output(tmp26 - tmp12);
        out[JPEG_PIX_SZ*7]  = scale_output(tmp27 + tmp13);
        out[JPEG_PIX_SZ*8]  = scale_output(tmp27 - tmp13);
    }
}
#endif

#ifdef JPEG_IDCT8_HOOK
/* jdec_idct's reference (bcmfw/video/jdec_vpu.c checks the vector unit
   against it at start): the C's IDCT of one block, given in natural order
   (F[v][u] at 8 v + u), each size in the layout the decoder gives it */
void jdec_idct_c(int size, const int16_t *ws, unsigned char *out, int rowstep)
{
    int16_t t[192];
    int i;

    for (i = 0; i < 64; i++)        /* transposed (8 and 16), or not (4) */
        t[size == 4 ? i : 8 * (i & 7) + (i >> 3)] = ws[i];
    if (size == 4)
    {
        jpeg_idct4v(t, t + 4);
        jpeg_idct4h(t, out, t + 32, rowstep);
    }
    else if (size == 8)
    {
        jpeg_idct8v(t, t + 64);
        jpeg_idct8h(t + 64, out, t + 128, rowstep);
    }
    else
    {
        jpeg_idct16v(t, t + 64);
        jpeg_idct16h(t + 64, out, t + 192, rowstep);
    }
}
#endif

struct idct_entry {
    int scale;
    void (*v_idct)(int16_t *ws, int16_t *end);
    void (*h_idct)(int16_t *ws, unsigned char *out, int16_t *end, int rowstep);
};

static const struct idct_entry idct_tbl[] = {
    { PASS1_BITS, NULL, jpeg_idct1h },
    { PASS1_BITS, jpeg_idct2v, jpeg_idct2h },
    { 0, jpeg_idct4v, jpeg_idct4h },
    { 0, jpeg_idct8v, jpeg_idct8h },
#ifdef HAVE_LCD_COLOR
    { 0, jpeg_idct16v, jpeg_idct16h },
#endif
};

/* JPEG decoder implementation */

#ifdef JPEG_FROM_MEM
INLINE unsigned char *jpeg_getc(struct jpeg* p_jpeg)
{
    if (LIKELY(p_jpeg->len))
    {
        p_jpeg->len--;
        return p_jpeg->data++;
    } else
        return NULL;
}

INLINE bool skip_bytes(struct jpeg* p_jpeg, int count)
{
    if (p_jpeg->len >= (unsigned)count)
    {
        p_jpeg->len -= count;
        p_jpeg->data += count;
        return true;
    } else {
        p_jpeg->data += p_jpeg->len;
        p_jpeg->len = 0;
        return false;
    }
}

INLINE void jpeg_putc(struct jpeg* p_jpeg)
{
    p_jpeg->len++;
    p_jpeg->data--;
}
#else

static int read_buf(struct jpeg* p_jpeg, size_t count)
{
#ifdef JPEG_BULK_READ
    /* a full refill from mid-sector ends on a sector boundary, so every
     * later one is whole sectors from a boundary into the aligned buffer */
    if (count == JPEG_READ_BUF_SIZE)
        count -= lseek(p_jpeg->fd, 0, SEEK_CUR) & (JPEG_READ_ALIGN - 1);
#endif
    return read(p_jpeg->fd, p_jpeg->buf, count);
}

INLINE void fill_buf(struct jpeg* p_jpeg)
{
    p_jpeg->buf_left = p_jpeg->read_buf(p_jpeg, MIN(JPEG_READ_BUF_SIZE, p_jpeg->len));
    p_jpeg->buf_index = 0;
    if (p_jpeg->buf_left > 0)
        p_jpeg->len -= p_jpeg->buf_left;
}

#ifdef HAVE_ALBUMART
static int read_buf_id3_unsync(struct jpeg* p_jpeg, size_t count)
{
    count = read_buf(p_jpeg, count);
    return id3_unsynchronize(p_jpeg->buf, count, (bool*) &p_jpeg->custom_param);
}

static int read_buf_vorbis_base64(struct jpeg* p_jpeg, size_t count)
{
    struct ogg_file* ogg = p_jpeg->custom_param;
    unsigned char* buf = p_jpeg->buf;
    count = ogg_file_read(ogg, buf, count);
    if (count == (size_t) -1)
        return 0;

    return base64_decode(buf, count, buf);
}

/* when pjpeg->read_buf involves additional data processing (like base64 decoding)
 * we can't use lseek and have to call pjpeg->read_buf for proper seek */
static bool skip_bytes_read_buf(struct jpeg* p_jpeg)
{
    do
    {
        int count = -p_jpeg->buf_left;
        fill_buf(p_jpeg);
        if (p_jpeg->buf_left < 0)
            return false;
        p_jpeg->buf_left -= count;
        p_jpeg->buf_index += count;
    } while (p_jpeg->buf_left < 0);
    return true;
}

#endif /* HAVE_ALBUMART */

static unsigned char *jpeg_getc(struct jpeg* p_jpeg)
{
    if (UNLIKELY(p_jpeg->buf_left < 1))
        fill_buf(p_jpeg);
    if (UNLIKELY(p_jpeg->buf_left < 1))
        return NULL;
    p_jpeg->buf_left--;
    return (p_jpeg->buf_index++) + p_jpeg->buf;
}

static bool skip_bytes_seek(struct jpeg* p_jpeg)
{
    if (UNLIKELY(lseek(p_jpeg->fd, -p_jpeg->buf_left, SEEK_CUR) < 0))
        return false;
    p_jpeg->buf_left = 0;
    return true;
}

static bool skip_bytes(struct jpeg* p_jpeg, int count)
{
    p_jpeg->buf_left -= count;
    p_jpeg->buf_index += count;
    return p_jpeg->buf_left >= 0 || p_jpeg->skip_bytes_seek(p_jpeg);
}

static void jpeg_putc(struct jpeg* p_jpeg)
{
    p_jpeg->buf_left++;
    p_jpeg->buf_index--;
}
#endif

#define e_skip_bytes(jpeg, count) \
do {\
    if (UNLIKELY(!skip_bytes((jpeg),(count)))) \
        return -1; \
} while (0)

#define e_getc(jpeg, code) \
({ \
    unsigned char *c; \
    if (UNLIKELY(!(c = jpeg_getc(jpeg)))) \
        return (code); \
    *c; \
})

#define d_getc(jpeg, def) \
({ \
    unsigned char *cp = jpeg_getc(jpeg); \
    unsigned char c = LIKELY(cp) ? *cp : (def); \
    c; \
})

/* Preprocess the JPEG JFIF file */
static int process_markers(struct jpeg* p_jpeg)
{
    unsigned char c;
    int marker_size; /* variable length of marker segment */
    int i, j, n;
    int ret = 0; /* returned flags */
    bool done = false;

    while (!done)
    {
#ifdef JPEG_JDEC
        if (p_jpeg->prog_marker)
        {   /* the marker a progressive scan's bit reader stopped at */
            c = p_jpeg->prog_marker;
            p_jpeg->prog_marker = 0;
        }
        else
#endif
        {
        c = e_getc(p_jpeg, -1);
        if (c != 0xFF) /* no marker? */
        {
            JDEBUGF("Non-marker data\n");
            continue; /* discard */
        }

        c = e_getc(p_jpeg, -1);
        }
        JDEBUGF("marker value %X\n",c);
        switch (c)
        {
        case 0xFF: /* Previous FF was fill byte */
            jpeg_putc(p_jpeg); /* This FF could be start of a marker */
            continue;
        case 0x00: /* Zero stuffed byte */
            break; /* discard */

#ifdef JPEG_JDEC
        case 0xC2: /* SOF Huff  - Progressive DCT (jprog.h) */
#endif
        case 0xC0: /* SOF Huff  - Baseline DCT */
            {
#ifdef JPEG_JDEC
                p_jpeg->progressive = c == 0xC2;
#endif
                JDEBUGF("SOF marker ");
                ret |= SOF0;
                marker_size = e_getc(p_jpeg, -1) << 8; /* Highbyte */
                marker_size |= e_getc(p_jpeg, -1); /* Lowbyte */
                JDEBUGF("len: %d\n", marker_size);
                n = e_getc(p_jpeg, -1); /* sample precision (= 8 or 12) */
                if (n != 8)
                {
                    return(-1); /* Unsupported sample precision */
                }
                p_jpeg->y_size = e_getc(p_jpeg, -1) << 8; /* Highbyte */
                p_jpeg->y_size |= e_getc(p_jpeg, -1); /* Lowbyte */
                p_jpeg->x_size = e_getc(p_jpeg, -1) << 8; /* Highbyte */
                p_jpeg->x_size |= e_getc(p_jpeg, -1); /* Lowbyte */
                JDEBUGF("  dimensions: %dx%d\n", p_jpeg->x_size,
                    p_jpeg->y_size);

                n = (marker_size-2-6)/3;
                if (e_getc(p_jpeg, -1) != n || (n != 1 && n != 3))
                {
                    return(-2); /* Unsupported SOF0 component specification */
                }
                for (i=0; i<n; i++)
                {
                    /* Component info */
                    p_jpeg->frameheader[i].ID = e_getc(p_jpeg, -1);
                    p_jpeg->frameheader[i].horizontal_sampling =
                        (c = e_getc(p_jpeg, -1)) >> 4;
                    p_jpeg->frameheader[i].vertical_sampling = c & 0x0F;
                    p_jpeg->frameheader[i].quanttable_select =
                        e_getc(p_jpeg, -1);
                    if (p_jpeg->frameheader[i].quanttable_select > 3)
                        return -8; /* Unsupported quantization table */
                    if (p_jpeg->frameheader[i].horizontal_sampling > 2
                     || p_jpeg->frameheader[i].vertical_sampling > 2)
                    return -3; /* Unsupported SOF0 subsampling */
                }
                p_jpeg->blocks = n;
                for (i=1; i<n; i++)
                {   /* chroma must be one block per MCU, or sampled like
                       luma when that is 1x2 or 2x1 (2x2 on all components
                       would exceed 10 blocks per MCU) */
                    int h = p_jpeg->frameheader[i].horizontal_sampling;
                    int v = p_jpeg->frameheader[i].vertical_sampling;
                    if ((h != 1 || v != 1)
                     && (h != p_jpeg->frameheader[0].horizontal_sampling
                      || v != p_jpeg->frameheader[0].vertical_sampling
                      || h == v))
                        return -3; /* Unsupported SOF0 subsampling */
                }
                if (p_jpeg->x_size == 0 || p_jpeg->y_size == 0)
                    return -12; /* Height defined by DNL not supported */
                /* A single-component scan is non-interleaved: the MCU is one
                   8x8 block regardless of the sampling factors (T.81 A.2.2) */
                if (n == 1)
                {
                    p_jpeg->frameheader[0].horizontal_sampling = 1;
                    p_jpeg->frameheader[0].vertical_sampling = 1;
                }
            }
            break;

        case 0xC1: /* SOF Huff  - Extended sequential DCT*/
#ifndef JPEG_JDEC
        case 0xC2: /* SOF Huff  - Progressive DCT*/
#endif
        case 0xC3: /* SOF Huff  - Spatial (sequential) lossless*/
        case 0xC5: /* SOF Huff  - Differential sequential DCT*/
        case 0xC6: /* SOF Huff  - Differential progressive DCT*/
        case 0xC7: /* SOF Huff  - Differential spatial*/
        case 0xC8: /* SOF Arith - Reserved for JPEG extensions*/
        case 0xC9: /* SOF Arith - Extended sequential DCT*/
        case 0xCA: /* SOF Arith - Progressive DCT*/
        case 0xCB: /* SOF Arith - Spatial (sequential) lossless*/
        case 0xCD: /* SOF Arith - Differential sequential DCT*/
        case 0xCE: /* SOF Arith - Differential progressive DCT*/
        case 0xCF: /* SOF Arith - Differential spatial*/
            {
                return (-4); /* other DCT model than baseline not implemented */
            }

        case 0xC4: /* Define Huffman Table(s) */
            {
                ret |= DHT;
                marker_size = e_getc(p_jpeg, -1) << 8; /* Highbyte */
                marker_size |= e_getc(p_jpeg, -1); /* Lowbyte */
                marker_size -= 2;

                while (marker_size > 17) /* another table */
                {
                    c = e_getc(p_jpeg, -1);
                    marker_size--;
                    int sum = 0;
                    i = c & 0x0F; /* table index */
                    if (i > 1)
                    {
                        return (-5); /* Huffman table index out of range */
                    } else {
                        if (c & 0xF0) /* AC table */
                        {
                            for (j=0; j<16; j++)
                            {
                                p_jpeg->hufftable[i].huffmancodes_ac[j] =
                                    (c = e_getc(p_jpeg, -1));
                                sum += c;
                                marker_size -= 1;
                            }
                            if(16 + sum > AC_LEN)
                                return -10; /* longer than allowed */

                            for (; j < 16 + sum; j++)
                            {
                                p_jpeg->hufftable[i].huffmancodes_ac[j] =
                                    e_getc(p_jpeg, -1);
                                marker_size--;
                            }
                        }
                        else /* DC table */
                        {
                            for (j=0; j<16; j++)
                            {
                                p_jpeg->hufftable[i].huffmancodes_dc[j] =
                                    (c = e_getc(p_jpeg, -1));
                                sum += c;
                                marker_size--;
                            }
                            if(16 + sum > DC_LEN)
                                return -11; /* longer than allowed */

                            for (; j < 16 + sum; j++)
                            {
                                p_jpeg->hufftable[i].huffmancodes_dc[j] =
                                    e_getc(p_jpeg, -1);
                                marker_size--;
                            }
                        }
                    }
                } /* while */
                e_skip_bytes(p_jpeg, marker_size);
            }
            break;

        case 0xCC: /* Define Arithmetic coding conditioning(s) */
            return(-6); /* Arithmetic coding not supported */

        case 0xD8: /* Start of Image */
            JDEBUGF("SOI\n");
            break;
        case 0xD9: /* End of Image */
            JDEBUGF("EOI\n");
#ifdef JPEG_JDEC
            if (p_jpeg->progressive)
                return ret | PROG_EOI; /* the last scan's end */
#endif
            break;
        case 0x01: /* for temp private use arith code */
            JDEBUGF("private\n");
            break; /* skip parameterless marker */


        case 0xDA: /* Start of Scan */
            {
                ret |= SOS;
#ifdef JPEG_JDEC
                if (p_jpeg->progressive)
                {   /* each scan's header is read by prog_scan */
                    done = true;
                    break;
                }
#endif
                marker_size = e_getc(p_jpeg, -1) << 8; /* Highbyte */
                marker_size |= e_getc(p_jpeg, -1); /* Lowbyte */
                marker_size -= 2;

                n = (marker_size-1-3)/2;
                if (e_getc(p_jpeg, -1) != n || (n != 1 && n != 3)
                    /* one scan with all components; multi-scan files and
                       SOS before SOF are not supported (blocks = Nf here) */
                    || n != p_jpeg->blocks)
                {
                    return (-7); /* Unsupported SOS component specification */
                }
                marker_size--;
                for (i=0; i<n; i++)
                {
                    p_jpeg->scanheader[i].ID = e_getc(p_jpeg, -1);
                    if (p_jpeg->scanheader[i].ID != p_jpeg->frameheader[i].ID)
                    {
                        return (-7); /* components out of frame order */
                    }
                    p_jpeg->scanheader[i].DC_select = (c = e_getc(p_jpeg, -1))
                        >> 4;
                    p_jpeg->scanheader[i].AC_select = c & 0x0F;
                    marker_size -= 2;
                    if (p_jpeg->scanheader[i].DC_select > 1
                     || p_jpeg->scanheader[i].AC_select > 1)
                    {
                        return (-5); /* Huffman table index out of range */
                    }
                }
                p_jpeg->rgb = n == 3 && jpeg_is_rgb(p_jpeg->jfif, p_jpeg->adobe,
                    p_jpeg->frameheader[0].ID, p_jpeg->frameheader[1].ID,
                    p_jpeg->frameheader[2].ID);
#ifndef HAVE_LCD_COLOR
                /* greyscale RGB is combined per block, so all components
                   must be one block per MCU (chroma is then 1x1 too) */
                if (p_jpeg->rgb
                 && (p_jpeg->frameheader[0].horizontal_sampling != 1
                  || p_jpeg->frameheader[0].vertical_sampling != 1))
                    return -3; /* Unsupported SOF0 subsampling */
#endif
                /* skip spectral information */
                e_skip_bytes(p_jpeg, marker_size);
                done = true;
            }
            break;

        case 0xDB: /* Define quantization Table(s) */
            {
                ret |= DQT;
                marker_size = e_getc(p_jpeg, -1) << 8; /* Highbyte */
                marker_size |= e_getc(p_jpeg, -1); /* Lowbyte */
                marker_size -= 2;

                n = (marker_size)/(QUANT_TABLE_LENGTH+1); /* # of tables */
                for (i=0; i<n; i++)
                {
                    int id = e_getc(p_jpeg, -1); /* ID */
                    marker_size--;
                    if (id >= 4)
                    {
                        return (-8); /* Unsupported quantization table */
                    }
                    /* Read Quantisation table: */
                    for (j=0; j<QUANT_TABLE_LENGTH; j++)
                    {
                        p_jpeg->quanttable[id][j] = e_getc(p_jpeg, -1);
                        marker_size--;
                    }
                }
                e_skip_bytes(p_jpeg, marker_size);
            }
            break;

        case 0xDD: /* Define Restart Interval */
            {
                marker_size = e_getc(p_jpeg, -1) << 8; /* Highbyte */
                marker_size |= e_getc(p_jpeg, -1); /* Lowbyte */
                marker_size -= 4;
                /* Highbyte */
                p_jpeg->restart_interval = e_getc(p_jpeg, -1) << 8;
                p_jpeg->restart_interval |= e_getc(p_jpeg, -1); /* Lowbyte */
                e_skip_bytes(p_jpeg, marker_size); /* skip segment */
            }
            break;

        case 0xDC: /* Define Number of Lines */
        case 0xDE: /* Define Hierarchical progression */
        case 0xDF: /* Expand Reference Component(s) */
        case 0xE0: /* Application Field 0*/
        case 0xE1: /* Application Field 1*/
        case 0xE2: /* Application Field 2*/
        case 0xE3: /* Application Field 3*/
        case 0xE4: /* Application Field 4*/
        case 0xE5: /* Application Field 5*/
        case 0xE6: /* Application Field 6*/
        case 0xE7: /* Application Field 7*/
        case 0xE8: /* Application Field 8*/
        case 0xE9: /* Application Field 9*/
        case 0xEA: /* Application Field 10*/
        case 0xEB: /* Application Field 11*/
        case 0xEC: /* Application Field 12*/
        case 0xED: /* Application Field 13*/
        case 0xEE: /* Application Field 14*/
        case 0xEF: /* Application Field 15*/
        case 0xFE: /* Comment */
            {
                unsigned char id[12];
                int marker = c;
                marker_size = e_getc(p_jpeg, -1) << 8; /* Highbyte */
                marker_size |= e_getc(p_jpeg, -1); /* Lowbyte */
                marker_size -= 2;
                n = 0;
                if (marker == 0xE0 || marker == 0xEE)
                {   /* JFIF or Adobe: may say which colour space is used */
                    n = MIN(marker_size, (int)sizeof(id));
                    for (i = 0; i < n; i++)
                        id[i] = e_getc(p_jpeg, -1);
                    jpeg_app_colorspace(marker, id, n, &p_jpeg->jfif,
                                        &p_jpeg->adobe);
                }
                JDEBUGF("unhandled marker len %d\n", marker_size);
                e_skip_bytes(p_jpeg, marker_size - n); /* skip segment */
            }
            break;

        case 0xF0: /* Reserved for JPEG extensions */
        case 0xF1: /* Reserved for JPEG extensions */
        case 0xF2: /* Reserved for JPEG extensions */
        case 0xF3: /* Reserved for JPEG extensions */
        case 0xF4: /* Reserved for JPEG extensions */
        case 0xF5: /* Reserved for JPEG extensions */
        case 0xF6: /* Reserved for JPEG extensions */
        case 0xF7: /* Reserved for JPEG extensions */
        case 0xF8: /* Reserved for JPEG extensions */
        case 0xF9: /* Reserved for JPEG extensions */
        case 0xFA: /* Reserved for JPEG extensions */
        case 0xFB: /* Reserved for JPEG extensions */
        case 0xFC: /* Reserved for JPEG extensions */
        case 0xFD: /* Reserved for JPEG extensions */
        case 0x02: /* Reserved */
        default:
            return (-9); /* Unknown marker */
        } /* switch */
    } /* while */

    return (ret); /* return flags with seen markers */
}

#ifdef JPEG_JDEC
/* jdec: the default tables as bytes (every entry fits one), widened as
   default_huff_tbl copies them: 1.2 KB less in the BCM image */
struct huffman_table_def
{
    unsigned char huffmancodes_dc[DC_LEN];
    unsigned char huffmancodes_ac[AC_LEN];
};
#define HUFFMAN_TABLE_DEF struct huffman_table_def
#else
#define HUFFMAN_TABLE_DEF struct huffman_table
#endif
static const HUFFMAN_TABLE_DEF luma_table =
{
    {
        0x00,0x01,0x05,0x01,0x01,0x01,0x01,0x01,0x01,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B
    },
    {
        0x00,0x02,0x01,0x03,0x03,0x02,0x04,0x03,0x05,0x05,0x04,0x04,0x00,0x00,
        0x01,0x7D,0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,
        0x13,0x51,0x61,0x07,0x22,0x71,0x14,0x32,0x81,0x91,0xA1,0x08,0x23,0x42,
        0xB1,0xC1,0x15,0x52,0xD1,0xF0,0x24,0x33,0x62,0x72,0x82,0x09,0x0A,0x16,
        0x17,0x18,0x19,0x1A,0x25,0x26,0x27,0x28,0x29,0x2A,0x34,0x35,0x36,0x37,
        0x38,0x39,0x3A,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4A,0x53,0x54,0x55,
        0x56,0x57,0x58,0x59,0x5A,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6A,0x73,
        0x74,0x75,0x76,0x77,0x78,0x79,0x7A,0x83,0x84,0x85,0x86,0x87,0x88,0x89,
        0x8A,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9A,0xA2,0xA3,0xA4,0xA5,
        0xA6,0xA7,0xA8,0xA9,0xAA,0xB2,0xB3,0xB4,0xB5,0xB6,0xB7,0xB8,0xB9,0xBA,
        0xC2,0xC3,0xC4,0xC5,0xC6,0xC7,0xC8,0xC9,0xCA,0xD2,0xD3,0xD4,0xD5,0xD6,
        0xD7,0xD8,0xD9,0xDA,0xE1,0xE2,0xE3,0xE4,0xE5,0xE6,0xE7,0xE8,0xE9,0xEA,
        0xF1,0xF2,0xF3,0xF4,0xF5,0xF6,0xF7,0xF8,0xF9,0xFA
    }
};

static const HUFFMAN_TABLE_DEF chroma_table =
{
    {
        0x00,0x03,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x00,0x00,0x00,
        0x00,0x00,0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B
    },
    {
        0x00,0x02,0x01,0x02,0x04,0x04,0x03,0x04,0x07,0x05,0x04,0x04,0x00,0x01,
        0x02,0x77,0x00,0x01,0x02,0x03,0x11,0x04,0x05,0x21,0x31,0x06,0x12,0x41,
        0x51,0x07,0x61,0x71,0x13,0x22,0x32,0x81,0x08,0x14,0x42,0x91,0xA1,0xB1,
        0xC1,0x09,0x23,0x33,0x52,0xF0,0x15,0x62,0x72,0xD1,0x0A,0x16,0x24,0x34,
        0xE1,0x25,0xF1,0x17,0x18,0x19,0x1A,0x26,0x27,0x28,0x29,0x2A,0x35,0x36,
        0x37,0x38,0x39,0x3A,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4A,0x53,0x54,
        0x55,0x56,0x57,0x58,0x59,0x5A,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6A,
        0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7A,0x82,0x83,0x84,0x85,0x86,0x87,
        0x88,0x89,0x8A,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9A,0xA2,0xA3,
        0xA4,0xA5,0xA6,0xA7,0xA8,0xA9,0xAA,0xB2,0xB3,0xB4,0xB5,0xB6,0xB7,0xB8,
        0xB9,0xBA,0xC2,0xC3,0xC4,0xC5,0xC6,0xC7,0xC8,0xC9,0xCA,0xD2,0xD3,0xD4,
        0xD5,0xD6,0xD7,0xD8,0xD9,0xDA,0xE2,0xE3,0xE4,0xE5,0xE6,0xE7,0xE8,0xE9,
        0xEA,0xF2,0xF3,0xF4,0xF5,0xF6,0xF7,0xF8,0xF9,0xFA
    }
};

static void default_huff_tbl(struct jpeg* p_jpeg)
{
#ifdef JPEG_JDEC
    /* both structs are their two arrays back to back, hufftable[1] right
       after hufftable[0] and chroma_table's bytes after luma_table's */
    int *d = (int *)p_jpeg->hufftable;
    const unsigned char *s = (const unsigned char *)&luma_table;
    int i;

    _Static_assert(sizeof(struct huffman_table) == (DC_LEN + AC_LEN) * sizeof(int)
                   && sizeof(struct huffman_table_def) == DC_LEN + AC_LEN,
                   "the default tables' layout");
    for (i = 0; i < DC_LEN + AC_LEN; i++)
        d[i] = s[i];
    s = (const unsigned char *)&chroma_table;
    for (; i < 2 * (DC_LEN + AC_LEN); i++)
        d[i] = s[i - (DC_LEN + AC_LEN)];
#else

    MEMCPY(&p_jpeg->hufftable[0], &luma_table, sizeof(luma_table));
    MEMCPY(&p_jpeg->hufftable[1], &chroma_table, sizeof(chroma_table));
#endif

    return;
}

/* Compute the derived values for a Huffman table */
static void fix_huff_tbl(int* htbl, struct derived_tbl* dtbl)
{
    int p, i, l, si;
    int lookbits, ctr;
    char huffsize[257];
    unsigned int huffcode[257];
    unsigned int code;

    dtbl->pub = htbl; /* fill in back link */

    /* Figure C.1: make table of Huffman code length for each symbol */
    /* Note that this is in code-length order. */

    p = 0;
    for (l = 1; l <= 16; l++)
    {    /* all possible code length */
        for (i = 1; i <= (int) htbl[l-1]; i++)  /* all codes per length */
            huffsize[p++] = (char) l;
    }
    huffsize[p] = 0;

    /* Figure C.2: generate the codes themselves */
    /* Note that this is in code-length order. */

    code = 0;
    si = huffsize[0];
    p = 0;
    while (huffsize[p])
    {
        while (((int) huffsize[p]) == si)
        {
            huffcode[p++] = code;
            code++;
        }
        code <<= 1;
        si++;
    }

    /* Figure F.15: generate decoding tables for bit-sequential decoding */

    p = 0;
    for (l = 1; l <= 16; l++)
    {
        if (htbl[l-1])
        {
            /* huffval[] index of 1st symbol of code length l */
            dtbl->valptr[l] = p;
            dtbl->mincode[l] = huffcode[p]; /* minimum code of length l */
            p += htbl[l-1];
            dtbl->maxcode[l] = huffcode[p-1]; /* maximum code of length l */
        }
        else
        {
            dtbl->maxcode[l] = -1;  /* -1 if no codes of this length */
        }
    }
    dtbl->maxcode[17] = 0xFFFFFL; /* ensures huff_DECODE terminates */

    /* Compute lookahead tables to speed up decoding.
    * First we set all the table entries to 0, indicating "too long";
    * then we iterate through the Huffman codes that are short enough and
    * fill in all the entries that correspond to bit sequences starting
    * with that code.
    */

    MEMSET(dtbl->look_nbits, 0, sizeof(dtbl->look_nbits));

    p = 0;
    for (l = 1; l <= HUFF_LOOKAHEAD; l++)
    {
        for (i = 1; i <= (int) htbl[l-1]; i++, p++)
        {
            /* l = current code's length, p = its index in huffcode[] &
             * huffval[]. Generate left-justified code followed by all possible
             * bit sequences
             */
            lookbits = huffcode[p] << (HUFF_LOOKAHEAD-l);
#ifdef JPEG_JDEC
            if (huffcode[p] >= BIT_N(l))
                continue; /* more codes than fit: no write past the table
                             (fix_huff_fast fails the decode) */
#endif
            for (ctr = 1 << (HUFF_LOOKAHEAD-l); ctr > 0; ctr--)
            {
                dtbl->look_nbits[lookbits] = l;
                dtbl->look_sym[lookbits] = htbl[16+p];
                lookbits++;
            }
        }
    }
}


/* zag[i] is the natural-order position of the i'th element of zigzag order. */
static const unsigned char zag[] =
{
#ifdef JPEG_IDCT_TRANSPOSE
      0,   8,   1,   2,   9,  16,  24,  17,
     10,   3,   4,  11,  18,  25,  32,  40,
     33,  26,  19,  12,   5,   6,  13,  20,
     27,  34,  41,  48,  56,  49,  42,  35,
     28,  21,  14,   7,  15,  22,  29,  36,
     43,  50,  57,  58,  51,  44,  37,  30,
     23,  31,  38,  45,  52,  59,  60,  53,
     46,  39,  47,  54,  61,  62,  55,  63,
#endif
      0,   1,   8,  16,   9,   2,   3,  10,
     17,  24,  32,  25,  18,  11,   4,   5,
     12,  19,  26,  33,  40,  48,  41,  34,
     27,  20,  13,   6,   7,  14,  21,  28,
     35,  42,  49,  56,  57,  50,  43,  36,
     29,  22,  15,  23,  30,  37,  44,  51,
     58,  59,  52,  45,  38,  31,  39,  46,
     53,  60,  61,  54,  47,  55,  62,  63,
};

/* zig[i] is the the zig-zag order position of the i'th element of natural
 * order, reading left-to-right then top-to-bottom.
 */
static const unsigned char zig[] =
{
     0,  1,  5,  6, 14, 15, 27, 28,
     2,  4,  7, 13, 16, 26, 29, 42,
     3,  8, 12, 17, 25, 30, 41, 43,
     9, 11, 18, 24, 31, 40, 44, 53,
    10, 19, 23, 32, 39, 45, 52, 54,
    20, 22, 33, 38, 46, 51, 55, 60,
    21, 34, 37, 47, 50, 56, 59, 61,
    35, 36, 48, 49, 57, 58, 62, 63
};

/* Reformat some image header data so that the decoder can use it properly. */
INLINE void fix_headers(struct jpeg* p_jpeg)
{
    int h = p_jpeg->frameheader[0].horizontal_sampling;
    int v = p_jpeg->frameheader[0].vertical_sampling;
    int ncomp = p_jpeg->blocks; /* still the component count here */
    int i, c, b;

    for (i=0; i<4; i++)
        p_jpeg->store_pos[i] = i; /* default ordering */
    if (h == 1 && v == 2)
    {   /* the second block of a component goes below the first */
        p_jpeg->store_pos[1] = 2;
        p_jpeg->store_pos[2] = 1;
    }

    p_jpeg->x_mbl = (p_jpeg->x_size + 8*h - 1) / (8*h);
    p_jpeg->x_phys = p_jpeg->x_mbl * 8*h;
    p_jpeg->y_mbl = (p_jpeg->y_size + 8*v - 1) / (8*v);
    p_jpeg->y_phys = p_jpeg->y_mbl * 8*v;

    /* Blocks in MCU order: each component's blocks in turn, H x V of them.
       Chroma is either 1x1 or sampled like luma (checked in SOF), so an MCU
       holds at most 6 blocks. */
    b = 0;
    for (c = 0; c < ncomp; c++)
    {
        int n = p_jpeg->frameheader[c].horizontal_sampling
              * p_jpeg->frameheader[c].vertical_sampling;
        for (i = 0; i < n; i++, b++)
        {
            p_jpeg->mcu_membership[b] = c;
            p_jpeg->mcu_pos[b] = i;
        }
    }
    p_jpeg->blocks = b;
}

INLINE void fix_huff_tables(struct jpeg *p_jpeg)
{
    fix_huff_tbl(p_jpeg->hufftable[0].huffmancodes_dc,
        &p_jpeg->dc_derived_tbls[0]);
    fix_huff_tbl(p_jpeg->hufftable[0].huffmancodes_ac,
        &p_jpeg->ac_derived_tbls[0]);
    fix_huff_tbl(p_jpeg->hufftable[1].huffmancodes_dc,
        &p_jpeg->dc_derived_tbls[1]);
    fix_huff_tbl(p_jpeg->hufftable[1].huffmancodes_ac,
        &p_jpeg->ac_derived_tbls[1]);
}

#ifdef JPEG_JDEC
/* huff_block's table for one Huffman table, indexed by the next HUFF_FAST
 * bits: value << 16 | symbol << 8 | len << 4 | nb, where nb (1..9) is the
 * length of the code those bits start with, 0 if it is longer or no code;
 * len = nb + (symbol & 15), the code and its magnitude bits together, if
 * that is at most HUFF_FAST (and, for a DC table, the symbol is a
 * magnitude category, below 16), else 0; value (16 bits, signed) the
 * magnitude bits sign-extended (HUFF_EXTEND), when len is not 0.
 * For an AC table, skip (NULL for a DC table) gets the skip loop's
 * table (huff_block), for the coefficients a scaled IDCT doesn't keep:
 * only how far each symbol goes matters, not its value.  adv << 8 | t:
 * t (1..24) the code's and its magnitude bits' length together; adv how
 * far k moves: run + 1, 16 for ZRL, 64 for an end of block (any other
 * zero size, as huff_decode_ac takes it); HB_SKIP_NONE (t 255) for no
 * code of up to HUFF_FAST bits.
 * false for a table with more codes than fit their lengths (up to
 * HUFF_FAST bits), which no encoder makes: the shell's decoder writes past
 * its lookahead table for one; jdec fails the decode as for a bad code */
#define HB_SKIP_NONE 0xFFFF
static bool fix_huff_fast(const int *htbl, uint32_t *fast, uint16_t *skip)
{
    int p = 0, l, i, idx;
    unsigned int code = 0;

    MEMSET(fast, 0, sizeof(uint32_t) << HUFF_FAST);
    if (skip)
        MEMSET(skip, 0xFF, sizeof(uint16_t) << HUFF_FAST); /* HB_SKIP_NONE */
    for (l = 1; l <= HUFF_FAST; l++)
    {   /* the canonical codes of length l (Figure C.2) */
        for (i = 0; i < htbl[l-1]; i++, p++, code++)
        {
            int sym = htbl[16+p], s = sym & 15, r = sym >> 4, len = l + s;
            int first = code << (HUFF_FAST - l), n = 1 << (HUFF_FAST - l);

            if (code >= BIT_N(l))
                return false;
            for (idx = first; idx < first + n; idx++)
            {
                uint32_t e = sym << 8 | l;

                if (skip)
                    skip[idx] = (s ? r + 1 : r == 15 ? 16 : 64) << 8 | len;
                if (len <= HUFF_FAST && (skip || sym < 16))
                {
                    int v = s ? (idx >> (HUFF_FAST - len)) & (BIT_N(s) - 1) : 0;

                    if (s && !(v & BIT_N(s - 1)))
                        v += (-1 << s) + 1; /* HUFF_EXTEND */
                    e |= (uint32_t)(v & 0xFFFF) << 16 | len << 4;
                }
                fast[idx] = e;
            }
        }
        code <<= 1;
    }
    return true;
}

INLINE bool fix_huff_fast_tables(struct jpeg *p_jpeg)
{
    bool ok = fix_huff_fast(p_jpeg->hufftable[0].huffmancodes_dc,
                            p_jpeg->dc_fast[0], NULL);

    ok &= fix_huff_fast(p_jpeg->hufftable[0].huffmancodes_ac,
                        p_jpeg->ac_fast[0], p_jpeg->ac_skip[0]);
    ok &= fix_huff_fast(p_jpeg->hufftable[1].huffmancodes_dc,
                        p_jpeg->dc_fast[1], NULL);
    ok &= fix_huff_fast(p_jpeg->hufftable[1].huffmancodes_ac,
                        p_jpeg->ac_fast[1], p_jpeg->ac_skip[1]);
    return ok;
}
#endif

/* Because some of the IDCT routines never multiply by any constants, and
 * therefore do not produce shifted output, we add the shift into the
 * quantization table when one of these IDCT routines is used, rather than
 * have the IDCT shift each value it processes.
 */
/* Pre-scale the quantization tables for the IDCT. Luma and chroma can use
 * different IDCT scales, so a table that both select is copied to a slot no
 * component selects before scaling; each component's quanttable_select is
 * rewritten to the slot it should use. There are 4 slots and at most 3
 * components, so when a table is shared a free slot always exists. */
INLINE void fix_quant_tables(struct jpeg *p_jpeg)
{
    int want[4] = { -1, -1, -1, -1 }; /* scale shift wanted for each slot */
    int orig[4] = { 0, 1, 2, 3 }; /* table each slot holds */
    int c, c2, f, j;

#ifdef HAVE_LCD_COLOR
    const int nc = p_jpeg->blocks > 1 ? 3 : 1;
#else
    /* chroma is not decoded; RGB is, at the luma scale */
    const int nc = p_jpeg->rgb ? 3 : 1;
#endif

    for (c = 0; c < nc; c++)
    {
        int t = p_jpeg->frameheader[c].quanttable_select;
        int shift = idct_tbl[p_jpeg->v_scale[SCALE_IDX(c)]].scale;

        for (f = 0; f < 4; f++) /* a slot already holding t at this scale? */
            if (orig[f] == t && want[f] == shift)
                break;
        if (f == 4 && want[t] < 0)
            f = t; /* first use of t */
        if (f == 4)
        {   /* t is in use at another scale: copy it to a free slot that no
               later component still needs */
            for (f = 0; f < 4; f++)
            {
                if (want[f] >= 0)
                    continue;
                for (c2 = c + 1; c2 < nc; c2++)
                    if (p_jpeg->frameheader[c2].quanttable_select == f)
                        break;
                if (c2 == nc)
                    break;
            }
            MEMCPY(p_jpeg->quanttable[f], p_jpeg->quanttable[t],
                sizeof(p_jpeg->quanttable[f]));
            orig[f] = t;
        }
        want[f] = shift;
        p_jpeg->frameheader[c].quanttable_select = f;
    }

    for (f = 0; f < 4; f++)
    {
        if (want[f] > 0)
        {
            for (j = 0; j < 64; j++)
                p_jpeg->quanttable[f][j] <<= want[f];
        }
    }
}

/*
* These functions/macros provide the in-line portion of bit fetching.
* Use check_bit_buffer to ensure there are N bits in get_buffer
* before using get_bits, peek_bits, or drop_bits.
*  check_bit_buffer(state,n,action);
*    Ensure there are N bits in get_buffer; if suspend, take action.
*  val = get_bits(n);
*    Fetch next N bits.
*  val = peek_bits(n);
*    Fetch next N bits without removing them from the buffer.
*  drop_bits(n);
*    Discard next N bits.
* The value N should be a simple variable, not an expression, because it
* is evaluated multiple times.
*/

#ifdef JPEG_JDEC
static void prog_fill(struct jpeg *p);
#endif

static void fill_bit_buffer(struct jpeg* p_jpeg)
{
    unsigned char byte, marker;
#ifdef JPEG_JDEC
    if (p_jpeg->progressive)
    {
        prog_fill(p_jpeg);
        return;
    }
#endif

    if (p_jpeg->marker_val)
        p_jpeg->marker_ind += 16;
    byte = d_getc(p_jpeg, 0);
    if (UNLIKELY(byte == 0xFF)) /* legal marker can be byte stuffing or RSTm */
    {   /* simplification: just skip the (one-byte) marker code */
        marker = d_getc(p_jpeg, 0);
        if ((marker & ~7) == 0xD0)
        {
            p_jpeg->marker_val = marker;
            p_jpeg->marker_ind = 8;
        }
    }
    p_jpeg->bitbuf = (p_jpeg->bitbuf << 8) | byte;

    byte = d_getc(p_jpeg, 0);
    if (UNLIKELY(byte == 0xFF)) /* legal marker can be byte stuffing or RSTm */
    {   /* simplification: just skip the (one-byte) marker code */
        marker = d_getc(p_jpeg, 0);
        if ((marker & ~7) == 0xD0)
        {
            p_jpeg->marker_val = marker;
            p_jpeg->marker_ind = 0;
        }
    }
    p_jpeg->bitbuf = (p_jpeg->bitbuf << 8) | byte;
    p_jpeg->bitbuf_bits += 16;
#ifdef JPEG_BS_DEBUG
    DEBUGF("read in: %04X\n", p_jpeg->bitbuf & 0xFFFF);
#endif
}

INLINE void check_bit_buffer(struct jpeg *p_jpeg, int nbits)
{
    if (nbits > p_jpeg->bitbuf_bits)
        fill_bit_buffer(p_jpeg);
}

INLINE int get_bits(struct jpeg *p_jpeg, int nbits)
{
#ifdef JPEG_BS_DEBUG
    if (nbits > p_jpeg->bitbuf_bits)
        DEBUGF("bitbuffer underrun\n");
    int mask = BIT_N(p_jpeg->bitbuf_bits - 1);
    int i;
    DEBUGF("get %d bits: ", nbits);
    for (i = 0; i < nbits; i++)
        DEBUGF("%d",!!(p_jpeg->bitbuf & (mask >>= 1)));
    DEBUGF("\n");
#endif
    return ((int) (p_jpeg->bitbuf >> (p_jpeg->bitbuf_bits -= nbits))) &
        (BIT_N(nbits)-1);
}

INLINE int peek_bits(struct jpeg *p_jpeg, int nbits)
{
#ifdef JPEG_BS_DEBUG
    int mask = BIT_N(p_jpeg->bitbuf_bits - 1);
    int i;
    DEBUGF("peek %d bits: ", nbits);
    for (i = 0; i < nbits; i++)
        DEBUGF("%d",!!(p_jpeg->bitbuf & (mask >>= 1)));
    DEBUGF("\n");
#endif
    return ((int) (p_jpeg->bitbuf >> (p_jpeg->bitbuf_bits - nbits))) &
        (BIT_N(nbits)-1);
}

INLINE void drop_bits(struct jpeg *p_jpeg, int nbits)
{
#ifdef JPEG_BS_DEBUG
    int mask = BIT_N(p_jpeg->bitbuf_bits - 1);
    int i;
    DEBUGF("drop %d bits: ", nbits);
    for (i = 0; i < nbits; i++)
        DEBUGF("%d",!!(p_jpeg->bitbuf & (mask >>= 1)));
    DEBUGF("\n");
#endif
    p_jpeg->bitbuf_bits -= nbits;
}

#ifdef JPEG_JDEC
#define HUFF_BAD(p_jpeg) ((p_jpeg)->huff_errors++)
#else
#define HUFF_BAD(p_jpeg) ((void)0)
#endif

/* re-synchronize to entropy data (skip restart marker) */
static void search_restart(struct jpeg *p_jpeg)
{
    if (p_jpeg->marker_val)
    {
        p_jpeg->marker_val = 0;
        /* keep the marker_ind bits read after the marker.  More than the
           buffer holds only in a corrupt stream (bits past the marker were
           taken, or shifted out): keep none, so the count stays within
           the buffer and never goes negative */
        if (p_jpeg->marker_ind <= p_jpeg->bitbuf_bits)
            p_jpeg->bitbuf_bits = p_jpeg->marker_ind;
        else
        {
            HUFF_BAD(p_jpeg);
            p_jpeg->bitbuf_bits = 0;
        }
        p_jpeg->marker_ind = 0;
        return;
    }
    unsigned char byte;
    p_jpeg->bitbuf_bits = 0;
    while ((byte = d_getc(p_jpeg, 0xFF)))
    {
        if (byte == 0xff)
        {
            byte = d_getc(p_jpeg, 0xD0);
            if ((byte & ~7) == 0xD0)
            {
                return;
            }
            else
                jpeg_putc(p_jpeg);
        }
    }
}

/* Figure F.12: extend sign bit. */
/* This saves some code and data size, benchmarks about the same on RAM */
#define HUFF_EXTEND(x,s) \
({ \
    int x__ = x; \
    int s__ = s; \
    x__ & BIT_N(s__- 1) ? x__ : x__ + (-1 << s__) + 1; \
})

/* Decode a single value */
#define huff_decode_dc(p_jpeg, tbl, s, r) \
{ \
    int nb, look; \
\
    check_bit_buffer((p_jpeg), HUFF_LOOKAHEAD); \
    look = peek_bits((p_jpeg), HUFF_LOOKAHEAD); \
    if ((nb = (tbl)->look_nbits[look]) != 0) \
    { \
        drop_bits((p_jpeg), nb); \
        s = (tbl)->look_sym[look]; \
        check_bit_buffer((p_jpeg), s); \
        r = get_bits((p_jpeg), s); \
    } else { \
        /*  slow_DECODE(s, HUFF_LOOKAHEAD+1)) < 0); */ \
        long code; \
        nb=HUFF_LOOKAHEAD+1; \
        check_bit_buffer((p_jpeg), nb); \
        code = get_bits((p_jpeg), nb); \
        while (code > (tbl)->maxcode[nb]) \
        { \
            code <<= 1; \
            check_bit_buffer((p_jpeg), 1); \
            code |= get_bits((p_jpeg), 1); \
            nb++; \
        } \
        if (nb > 16) /* error in Huffman */ \
        { \
            HUFF_BAD(p_jpeg); \
            r = 0; s = 0; /* fake a zero, this is most safe */ \
        } else { \
            s = (tbl)->pub[16 + (tbl)->valptr[nb] + \
                ((int) (code - (tbl)->mincode[nb]))]; \
            check_bit_buffer((p_jpeg), s); \
            r = get_bits((p_jpeg), s); \
        } \
    } /* end slow decode */ \
}

#define huff_decode_ac(p_jpeg, tbl, s) \
{ \
    int nb, look; \
\
    check_bit_buffer((p_jpeg), HUFF_LOOKAHEAD); \
    look = peek_bits((p_jpeg), HUFF_LOOKAHEAD); \
    if ((nb = (tbl)->look_nbits[look]) != 0) \
    { \
        drop_bits((p_jpeg), nb); \
        s = (tbl)->look_sym[look]; \
    } else { \
        /*  slow_DECODE(s, HUFF_LOOKAHEAD+1)) < 0); */ \
        long code; \
        nb=HUFF_LOOKAHEAD+1; \
        check_bit_buffer((p_jpeg), nb); \
        code = get_bits((p_jpeg), nb); \
        while (code > (tbl)->maxcode[nb]) \
        { \
            code <<= 1; \
            check_bit_buffer((p_jpeg), 1); \
            code |= get_bits((p_jpeg), 1); \
            nb++; \
        } \
        if (nb > 16) /* error in Huffman */ \
        { \
            HUFF_BAD(p_jpeg); \
            s = 0; /* fake a zero, this is most safe */ \
        } else { \
            s = (tbl)->pub[16 + (tbl)->valptr[nb] + \
                ((int) (code - (tbl)->mincode[nb]))]; \
        } \
    } /* end slow decode */ \
}

/* JDEC_BITSERIAL (bcmfw/tests/jskip_test.py only): the shell's own
   bit-serial loop instead, the reference the fast one must equal */
#if defined(JPEG_JDEC) && !defined(JPEG_FROM_MEM) && defined(HAVE_LCD_COLOR) \
    && !defined(JDEC_BITSERIAL)
#define HB_ROWS /* store_row_jpeg's baseline rows come from mcu_row_fast */
/* The baseline scan's MCU rows for jdec (algorithms.md section 2): what
 * store_row_jpeg's loop, huff_decode_dc and huff_decode_ac make, with the
 * same bit buffer, so the same symbols, coefficients, bad-code count and
 * stream position: restart markers, stuffed bytes and the end of the data
 * go exactly as before.
 *   - The bit buffer lives in registers, and the input buffer is read in
 *     place: 16 bits are added exactly when the bit-serial decoder adds
 *     them (a check finding fewer bits than it asks for), straight from
 *     buf when the next two bytes are there, neither is 0xFF and no
 *     restart marker is pending; otherwise by fill_bit_buffer itself.
 *   - One look at the next 9 bits (dc_fast, ac_fast: fix_huff_fast) gives
 *     the code's length and symbol, and for a short code with its
 *     magnitude bits, the coefficient too.  An entry is used only when
 *     its bits are in the buffer (the 9th may be missing after a check
 *     for 8), and then the bit-serial decoder would not have added any
 *     before taking them either.  Longer codes and bad ones take the
 *     bit-serial walk, verbatim.
 *   - The coefficients the scaled IDCT doesn't keep (from k_need on) are
 *     only skipped: one look at ac_skip (fix_huff_fast) gives how many
 *     bits a symbol takes, code and magnitude together, and how far it
 *     moves k, so no value is made (jpeg-spike.md section 12).
 *   - What a block needs from its component (tables, scale, IDCT, place)
 *     is worked out once per MCU row, not per block. */
typedef uint32_t __attribute__((may_alias)) hb_word;

struct hb_reader
{
    unsigned long buf;
    int bits;
    unsigned char *bp;
    uintptr_t lim; /* bp's last place with two bytes after it; 0 while a
                      restart marker is pending (marker_val) */
};

struct hb_blk
{
    struct derived_tbl *dctbl, *actbl;
    const uint32_t *dcf, *acf;
    const uint16_t *skf;            /* ac_skip */
    const int16_t *qt;
    const unsigned char *zg;        /* zag, in the IDCT's order */
    int *last_dc;
    int k_need, zero_bytes;
    void (*v_idct)(int16_t *ws, int16_t *end);
    void (*h_idct)(int16_t *ws, unsigned char *out, int16_t *end, int rowstep);
    int v_end, h_start, h_end;      /* in coefficients from block */
#ifdef JPEG_IDCT8_HOOK
    int16_t *c;                     /* where the coefficients go: block,
                                       or jdec_idct's next place
                                       (idct_take); the IDCT's offsets
                                       above are from there */
#endif
    int out_off;                    /* from the MCU's first pixel */
};

INLINE void hb_sync(struct jpeg *p_jpeg, const struct hb_reader *rd)
{
    p_jpeg->bitbuf = rd->buf;
    p_jpeg->bitbuf_bits = rd->bits;
    /* buf_index + buf_left, the buffer's end, is as hb_load found it */
    p_jpeg->buf_left += p_jpeg->buf_index - (rd->bp - p_jpeg->buf);
    p_jpeg->buf_index = rd->bp - p_jpeg->buf;
}

INLINE void hb_load(struct jpeg *p_jpeg, struct hb_reader *rd)
{
    rd->buf = p_jpeg->bitbuf;
    rd->bits = p_jpeg->bitbuf_bits;
    rd->bp = p_jpeg->buf + p_jpeg->buf_index;
    rd->lim = p_jpeg->marker_val ? 0 :
              (uintptr_t)rd->bp + p_jpeg->buf_left - 2;
}

/* fill_bit_buffer's 16 bits: from buf in place when the next two bytes
   are there, neither is 0xFF and no restart marker is pending, else by
   fill_bit_buffer itself */
#define HB_FAST_FILL(bp, lim) \
    (LIKELY((uintptr_t)(bp) <= (lim) && (bp)[0] != 0xFF && (bp)[1] != 0xFF))
static void __attribute__((noinline)) hb_fill(struct jpeg *p_jpeg,
                                              struct hb_reader *rd)
{
    if (HB_FAST_FILL(rd->bp, rd->lim))
    {
        rd->buf = (rd->buf << 16) | rd->bp[0] << 8 | rd->bp[1];
        rd->bp += 2;
        rd->bits += 16;
        return;
    }
    hb_sync(p_jpeg, rd);
    fill_bit_buffer(p_jpeg);
    hb_load(p_jpeg, rd);
}

/* huff_decode_dc's and huff_decode_ac's bit-serial walk, for a code longer
   than the lookahead, or none: its symbol, or 0 and a bad code counted */
static int __attribute__((noinline)) hb_slow(struct jpeg *p_jpeg,
                                             struct hb_reader *rd,
                                             const struct derived_tbl *tbl)
{
    int nb = HUFF_LOOKAHEAD + 1;
    long code;

    if (nb > rd->bits)
        hb_fill(p_jpeg, rd);
    code = ((int)(rd->buf >> (rd->bits -= nb))) & (BIT_N(nb) - 1);
    while (code > tbl->maxcode[nb])
    {
        code <<= 1;
        if (1 > rd->bits)
            hb_fill(p_jpeg, rd);
        code |= ((int)(rd->buf >> (rd->bits -= 1))) & 1;
        nb++;
    }
    if (nb > 16) /* error in Huffman */
    {
        HUFF_BAD(p_jpeg);
        return 0;
    }
    return tbl->pub[16 + tbl->valptr[nb] + ((int) (code - tbl->mincode[nb]))];
}

/* huff_block's skip loop for one symbol it can't take by ac_skip: the
   old skip code (huff_decode_ac's steps, the coefficient not made); k
   after it, 64 at an end of block.  A short code with its magnitude bits
   takes the same bits by the code alone and a check that adds none. */
static int __attribute__((noinline)) hb_skip_one(struct jpeg *p_jpeg,
                                                 struct hb_reader *rd,
                                                 const uint32_t *acf,
                                                 const struct derived_tbl *tbl,
                                                 int k)
{
    uint32_t e;
    int nb, s, r;

    if (HUFF_LOOKAHEAD > rd->bits)
        hb_fill(p_jpeg, rd);
    e = acf[((int)((rd->buf << 1) >> (rd->bits - 8))) & (BIT_N(HUFF_FAST) - 1)];
    nb = e & 15;
    if ((unsigned)nb - 1 < (unsigned)rd->bits)
    {
        rd->bits -= nb;
        s = (e >> 8) & 255;
    } else
        s = hb_slow(p_jpeg, rd, tbl);
    r = s >> 4;
    s &= 15;
    if (s)
    {
        if (s > rd->bits)
            hb_fill(p_jpeg, rd);
        rd->bits -= s;
    }
    else if (r != 15)
        return 64;
    return k + r + 1;
}

/* One block's coefficients into block (zeroed as far as the IDCT reads) */
INLINE void huff_block(struct jpeg *p_jpeg, struct hb_reader *rd,
                       const struct hb_blk *d, int16_t *block)
{
    unsigned long buf = rd->buf;
    int bits = rd->bits;
    const uint32_t *acf = d->acf;
    const unsigned char *zg = d->zg;
    const int16_t *qt = d->qt;
    int k = 1, k_need = d->k_need;
    int nb, s, r, len;
    uint32_t e;

    /* check_bit_buffer(n): the 16 bits, inline when they come from buf
       (HB_CHECK8, at each symbol), else by hb_fill */
#define HB_CALL(fn) (rd->buf = buf, rd->bits = bits, fn, \
                     buf = rd->buf, bits = rd->bits)
#define HB_CHECK(n) do { \
    if ((n) > bits) \
        HB_CALL(hb_fill(p_jpeg, rd)); \
    } while (0)
#define HB_CHECK8() do { \
    if (HUFF_LOOKAHEAD > bits) \
    { \
        if (HB_FAST_FILL(rd->bp, rd->lim)) \
        { \
            buf = (buf << 16) | rd->bp[0] << 8 | rd->bp[1]; \
            rd->bp += 2; \
            bits += 16; \
        } else \
            HB_CALL(hb_fill(p_jpeg, rd)); \
    } } while (0)
#define HB_GET(n) (((int)(buf >> (bits -= (n)))) & (BIT_N(n) - 1))
    /* an entry's n bits (0: none) are in the buffer */
#define HB_HAVE(n) ((unsigned)(n) - 1 < (unsigned)bits)
    /* the next 9 bits; after HB_CHECK(8) there are at least 8 */
#define HB_PEEK9() ((int)((buf << 1) >> (bits - 8)) & (BIT_N(HUFF_FAST) - 1))
#define HB_SLOW(tbl, s) HB_CALL(s = hb_slow(p_jpeg, rd, tbl))

    /* Section F.2.2.1: the DC coefficient difference */
    HB_CHECK8();
    e = d->dcf[HB_PEEK9()];
    len = (e >> 4) & 15;
    if (HB_HAVE(len))
    {
        bits -= len;
        s = (int32_t)e >> 16;
    } else {
        nb = e & 15;
        if (HB_HAVE(nb))
        {
            bits -= nb;
            s = (e >> 8) & 255;
        } else
            HB_SLOW(d->dctbl, s);
        if (s)
        {
            HB_CHECK(s);
            r = HB_GET(s);
            s = HUFF_EXTEND(r, s);
        }
    }
    *d->last_dc += s;
    /* output it (assumes zag[0] = 0) */
    block[0] = MULTIPLY16(*d->last_dc, qt[0]);
    /* coefficient buffer must be cleared: MEMSET(block+1, 0, zero_bytes)
       with no call; zero_bytes is 4 * zero_need */
    if (d->zero_bytes)
    {
        hb_word *w = (hb_word *)(block + 2);
        int n;

        block[1] = 0;
        for (n = d->zero_bytes >> 2; n > 1; n--)
            *w++ = 0;
        block[d->zero_bytes >> 1] = 0;
    }
    if (k_need <= 1)    /* DC only: every AC coefficient is skipped */
        goto skip;
    /* Section F.2.2.2: the AC coefficients kept */
    while (true)
    {
        HB_CHECK8();
        e = acf[HB_PEEK9()];
        len = (e >> 4) & 15;
        if (HB_HAVE(len))
        {   /* the code and its magnitude bits at once */
            bits -= len;
            s = (e >> 8) & 255;
            k += s >> 4;
            if (s & 15)
            {
                if (k >= k_need)
                    goto skip_on;
                block[zg[k]] = MULTIPLY16((int32_t)e >> 16, qt[k]);
            }
            else if (s != 0xF0)
                goto block_end;
        } else {
            nb = e & 15;
            if (HB_HAVE(nb))
            {
                bits -= nb;
                s = (e >> 8) & 255;
            } else
                HB_SLOW(d->actbl, s);
            r = s >> 4;
            s &= 15;
            k += r;
            if (s)
            {
                HB_CHECK(s);
                if (k >= k_need)
                {
                    bits -= s;
                    goto skip_on;
                }
                r = HB_GET(s);
                r = HUFF_EXTEND(r, s);
                block[zg[k]] = MULTIPLY16(r, qt[k]);
            }
            else if (r != 15)
                goto block_end;
        }
        if ((++k) & 64)
            goto block_end;
    }
    /* the rest: only skipped (fix_huff_fast).  With u = 32 - bits, the
       next HUFF_FAST bits are (buf << u) >> (32 - HUFF_FAST) and a
       symbol's t bits are all in the buffer when u + t <= 32; then the
       bit-serial decoder takes them with no fill either.  A symbol that
       isn't, a fill fill_bit_buffer must make (near a 0xFF, a restart
       marker or the buffer's end), and a buffer that says it holds more
       than 32 bits (none since search_restart's bound; a guard) go the
       general way below. */
skip:
    if ((unsigned)bits <= 32)
    {
        const uint16_t *skf = d->skf;
        unsigned char *bp = rd->bp;     /* no calls in here */
        uintptr_t lim = rd->lim;
        int u = 32 - bits;

        while (true)
        {
            if (u > 32 - HUFF_LOOKAHEAD) /* HB_CHECK8 */
            {
                if (!HB_FAST_FILL(bp, lim))
                    break;          /* fill_bit_buffer's: below */
                buf = (buf << 16) | bp[0] << 8 | bp[1];
                bp += 2;
                u -= 16;
            }
            len = skf[((uint32_t)buf << u) >> (32 - HUFF_FAST)];
            if ((u += len & 255) > 32)
            {
                u -= len & 255;
                break;
            }
            if ((k += len >> 8) >= 64)
                break;
        }
        rd->bp = bp;
        bits = 32 - u;
        if (k >= 64)
            goto block_end;
    }
    HB_CALL(k = hb_skip_one(p_jpeg, rd, acf, d->actbl, k));
    if (k < 64)
        goto skip;
    goto block_end;
skip_on:
    if (++k < 64)
        goto skip;
block_end:
    rd->buf = buf;
    rd->bits = bits;
#undef HB_CALL
#undef HB_CHECK
#undef HB_CHECK8
#undef HB_GET
#undef HB_HAVE
#undef HB_PEEK9
#undef HB_SLOW
}

#ifdef JPEG_IDCT8_HOOK
/* jdec_idct's coefficient places: zag in natural order, rows 16 apart
   (jhook.h JDEC_IDCT_AT) */
static const unsigned char zag16[64] =
{
      0,   1,  16,  32,  17,   2,   3,  18,
     33,  48,  64,  49,  34,  19,   4,   5,
     20,  35,  50,  65,  80,  96,  81,  66,
     51,  36,  21,   6,   7,  22,  37,  52,
     67,  82,  97, 112, 113,  98,  83,  68,
     53,  38,  23,  39,  54,  69,  84,  99,
    114, 115, 100,  85,  70,  55,  71,  86,
    101, 116, 117, 102,  87, 103, 118, 119,
};

/* jdec_idct's blocks.  A block of the MCU row being made (mcu_row_fast)
 * whose two scales are equal and 4, 8 or 16 points has idct_take for its
 * IDCT and no vertical pass; its coefficients go straight to the next of
 * its batch's places (c), which start clear.  A batch's blocks follow one
 * another round a ring (v_end: the next one's index).  idct_jpeg: the
 * row's decoder, if it has such blocks (else NULL); idct_blk: its blocks */
static struct jpeg *idct_jpeg;
static struct hb_blk *idct_blk;

#define IDCT_ALIGN(p) ((void *)(((uintptr_t)(p) + 31) & ~31))

/* jdec_idct on batch b's n blocks taken (n > 0), their pixels put
   (jdec_put); then none are taken */
static void __attribute__((noinline)) idct_run(struct jpeg *p_jpeg,
                                               struct idct_batch *b, int n,
                                               int rowstep)
{
    uint8_t *px = IDCT_ALIGN(b->px);

    jdec_idct(px, IDCT_ALIGN(b->coef), b->size);
    jdec_put(px, b->to, n, rowstep, b->size, p_jpeg->blocks == 1);
    b->n = 0;
}

/* The block just decoded (its batch's cur, its coefficients at ws, place
   n): taken, its pixels to out (four taken: jdec_idct); one whose AC
   coefficients are all 0 is made at once, what the C's all-zero
   shortcuts make of it, one value (jdec_fill), and its place is the next
   block's.
   Then the next block's place */
static void idct_take(int16_t *ws, unsigned char *out, int16_t *end,
                      int rowstep)
{
    struct jpeg *p_jpeg = idct_jpeg;
    struct idct_batch *b = &p_jpeg->idct[ws >= p_jpeg->idct[1].coef];
    struct hb_blk *d = idct_blk + b->cur->v_end;
    uint32_t *w = (uint32_t *)ws, any = 0;
    int dc = ws[0], y;

    (void)end;
    ws[0] = 0;
    for (y = 0; y < 8; y += 2, w += 16)     /* two rows a turn */
        any |= w[0] | w[1] | w[2] | w[3] | w[8] | w[9] | w[10] | w[11];
    if (any)
    {
        ws[0] = dc;
        b->to[b->n++] = out;
        if (b->n == 4)
            idct_run(p_jpeg, b, 4, rowstep);
    }
    else
    {
        jdec_fill(range_limit(128 + (int) DESCALE((long) (int16_t)
                                                  (dc << PASS1_BITS),
                                                  PASS1_BITS+3)),
                  out, p_jpeg->blocks == 1, rowstep, b->size);
    }
    d->c = (int16_t *)IDCT_ALIGN(b->coef) + JDEC_IDCT_AT(b->n);
    b->cur = d;
}

/* The MCU row about to be made (blk: its blocks, as mcu_row_fast set them
   up), or just made (rowstep > 0): the blocks taken, if any, run.  At a
   row's start, each block whose scales are equal and 2-4 (4, 8, 16
   points) takes idct_take, its batch the component's scale's; each batch
   a ring, its first block at place 0; idct_jpeg set if there are any */
static void __attribute__((noinline)) idct_row(struct jpeg *p_jpeg,
                                               struct hb_blk *blk, int rowstep)
{
    struct hb_blk *prev[2] = { NULL, NULL };
    int first[2], k, t;

    if (rowstep)
    {
        for (t = 0; t < 2; t++)
            if (p_jpeg->idct[t].n)
                idct_run(p_jpeg, &p_jpeg->idct[t], p_jpeg->idct[t].n, rowstep);
        return;
    }
    idct_blk = blk;
    for (k = 0; k < p_jpeg->blocks; k++)
    {
        int sc = SCALE_IDX(p_jpeg->mcu_membership[k]);
        int s = p_jpeg->v_scale[sc];
        struct hb_blk *d = &blk[k];

        if (s == p_jpeg->h_scale[sc] && s >= 2 && s <= 4)
        {
            if (prev[sc])
                prev[sc]->v_end = k;
            else
            {
                first[sc] = k;
                p_jpeg->idct[sc].size = 1 << s;
            }
            prev[sc] = d;
            d->v_idct = NULL;
            d->h_idct = idct_take;
            d->h_start = 0;
            d->zg = zag16;
            d->zero_bytes = 0;
        }
    }
    for (t = 0; t < 2; t++)
        if (prev[t])
        {
            struct idct_batch *b = &p_jpeg->idct[t];

            prev[t]->v_end = first[t];      /* the ring */
            idct_jpeg = p_jpeg;
            b->cur = blk + first[t];
            b->n = 0;
            b->cur->c = IDCT_ALIGN(b->coef);
        }
}
#endif /* JPEG_IDCT8_HOOK */

/* store_row_jpeg's loop over one row of MCUs, for a baseline scan */
static void mcu_row_fast(struct jpeg *p_jpeg, unsigned char *out,
                         const int *store_offs, unsigned int b_width,
                         int mcu_offset, unsigned int mcu_width, int height,
                         int16_t *block)
{
    struct hb_blk blk[6];
    struct hb_reader rd;
    int x, blkn;

    for (blkn = 0; blkn < p_jpeg->blocks; blkn++)
    {
        struct hb_blk *d = &blk[blkn];
        int ci = p_jpeg->mcu_membership[blkn]; /* component index */
        int sc = SCALE_IDX(ci);
        int dcs = p_jpeg->scanheader[ci].DC_select;
        int acs = p_jpeg->scanheader[ci].AC_select;
        int idct_cols = BIT_N(MIN(p_jpeg->h_scale[sc], 3));
        int idct_rows = BIT_N(p_jpeg->v_scale[sc]);
#ifdef JPEG_IDCT_TRANSPOSE
        bool transpose = p_jpeg->v_scale[sc] > 2;
#else
        bool transpose = false;
#endif

        d->dctbl = &p_jpeg->dc_derived_tbls[dcs];
        d->actbl = &p_jpeg->ac_derived_tbls[acs];
        d->dcf = p_jpeg->dc_fast[dcs];
        d->acf = p_jpeg->ac_fast[acs];
        d->skf = p_jpeg->ac_skip[acs];
        d->qt = p_jpeg->quanttable[p_jpeg->frameheader[ci].quanttable_select];
#ifdef JPEG_IDCT_TRANSPOSE
        d->zg = zag + (transpose ? 0 : 64);
#else
        d->zg = zag;
#endif
        d->last_dc = &p_jpeg->last_dc_val[ci];
        d->k_need = p_jpeg->k_need[sc];
        d->zero_bytes = p_jpeg->zero_need[sc] * sizeof(int);
        d->v_idct = idct_tbl[p_jpeg->v_scale[sc]].v_idct;
        d->h_idct = idct_tbl[p_jpeg->h_scale[sc]].h_idct;
        d->v_end = transpose ? 8 * idct_cols : idct_cols;
        d->h_start = transpose ? 64 : 0;
        d->h_end = d->h_start + idct_rows * 8;
#ifdef JPEG_IDCT8_HOOK
        d->c = block;
#endif
        /* Y, Cb, Cr go to bytes 0-2 for the YUV conversion;
           R, G, B to their places in struct uint8_rgb */
        d->out_off = (p_jpeg->rgb ? 2 - ci : ci)
            + store_offs[p_jpeg->mcu_pos[blkn]];
    }
#ifdef JPEG_IDCT8_HOOK
    idct_jpeg = NULL;
    /* the row's batches, if any (jdec_put: the rows 32-byte aligned) */
    if (jdec_idct && !(((uintptr_t)out | b_width) & 31))
        idct_row(p_jpeg, blk, 0);
#endif
    hb_load(p_jpeg, &rd);
    for (x = 0; x < p_jpeg->x_mbl; x++)
    {
        for (blkn = 0; blkn < p_jpeg->blocks; blkn++)
        {
            const struct hb_blk *d = &blk[blkn];

#ifdef JPEG_IDCT8_HOOK
            int16_t *c = d->c;
#else
            int16_t *c = block;
#endif

            huff_block(p_jpeg, &rd, d, c);
            if (d->v_idct)
                d->v_idct(c, c + d->v_end);
            d->h_idct(c + d->h_start, out + d->out_off, c + d->h_end, b_width);
        }
        /* don't starve other threads while an MCU row decodes */
        yield();
#if defined(HAVE_IPOD_UI) && !defined(PLUGIN) && !defined(JDEC_ALONE)
        if (pause_hook)
            pause_hook();       /* the art builder may park here */
#endif
        if (p_jpeg->blocks == 1)
        {
            unsigned int xp;
            int yp;
            unsigned char *row = out;

            for (yp = 0; yp < height; yp++, row += b_width)
            {
                unsigned char *px = row;
                for (xp = 0; xp < mcu_width; xp++, px += JPEG_PIX_SZ)
                {
                    px[1] = px[2] = px[0];
                }
            }
        }
        out += mcu_offset;
        if (p_jpeg->restart_interval && --p_jpeg->restart == 0)
        {   /* if a restart marker is due: */
            p_jpeg->restart = p_jpeg->restart_interval; /* count again */
            hb_sync(p_jpeg, &rd);
            search_restart(p_jpeg); /* align the bitstream */
            hb_load(p_jpeg, &rd);
            p_jpeg->last_dc_val[0] = p_jpeg->last_dc_val[1] =
                             p_jpeg->last_dc_val[2] = 0; /* reset decoder */
        }
    }
#ifdef JPEG_IDCT8_HOOK
    if (idct_jpeg)
        idct_row(p_jpeg, blk, b_width);
#endif
    hb_sync(p_jpeg, &rd);
}
#endif

#ifdef JPEG_JDEC
#include "jprog.h"
#endif

static struct img_part *store_row_jpeg(void *jpeg_args)
{
    struct jpeg *p_jpeg = (struct jpeg*) jpeg_args;
    int mcu_hscale = (p_jpeg->h_scale[0] +
        p_jpeg->frameheader[0].horizontal_sampling - 1);
    int mcu_vscale = (p_jpeg->v_scale[0] +
        p_jpeg->frameheader[0].vertical_sampling - 1);
    unsigned int width = p_jpeg->x_mbl << mcu_hscale;
#ifdef JDEC_ALONE
    /* rows 32-byte aligned, for jdec_put (clip_jpeg_fd's decode_buf_size) */
    unsigned int b_width = (width * JPEG_PIX_SZ + 31) & ~31u;
#else
    unsigned int b_width = width * JPEG_PIX_SZ;
#endif
    int height = BIT_N(mcu_vscale);
    int x;
    if (!p_jpeg->mcu_row) /* Need to decode a new row of MCUs */
    {
#if defined(HAVE_IPOD_UI) && !defined(PLUGIN) && !defined(JDEC_ALONE)
        if (abort_hook && abort_hook())
            return NULL;            /* called off: the caller fails */
#endif
        p_jpeg->out_ptr = (unsigned char *)p_jpeg->img_buf;
        int store_offs[4];
#ifdef HAVE_LCD_COLOR
        unsigned mcu_width = BIT_N(mcu_hscale);
#endif
        int mcu_offset = JPEG_PIX_SZ << mcu_hscale;
        unsigned char *out = p_jpeg->out_ptr;
        store_offs[p_jpeg->store_pos[0]] = 0;
        store_offs[p_jpeg->store_pos[1]] = JPEG_PIX_SZ << p_jpeg->h_scale[0];
        store_offs[p_jpeg->store_pos[2]] = b_width << p_jpeg->v_scale[0];
        store_offs[p_jpeg->store_pos[3]] = store_offs[1] + store_offs[2];
        /* decoded DCT coefficients */
        int16_t block[IDCT_WS_SIZE] __attribute__((aligned(8)));
#ifndef HAVE_LCD_COLOR
        unsigned char rgb_tmp[2][64]; /* R and G of a greyscale RGB MCU */
#endif
#ifdef HB_ROWS
        if (!p_jpeg->progressive)
            mcu_row_fast(p_jpeg, out, store_offs, b_width, mcu_offset,
                         mcu_width, height, block);
        else
#endif
        for (x = 0; x < p_jpeg->x_mbl; x++)
        {
            int blkn;
            for (blkn = 0; blkn < p_jpeg->blocks; blkn++)
            {
                int ci = p_jpeg->mcu_membership[blkn]; /* component index */
#ifdef JPEG_IDCT_TRANSPOSE
                bool transpose = p_jpeg->v_scale[SCALE_IDX(ci)] > 2;
#endif
#ifndef HB_ROWS
                int k = 1; /* coefficient index */
                int s, r; /* huffman values */
                struct derived_tbl* dctbl =
                    &p_jpeg->dc_derived_tbls[p_jpeg->scanheader[ci].DC_select];
                struct derived_tbl* actbl =
                    &p_jpeg->ac_derived_tbls[p_jpeg->scanheader[ci].AC_select];
#endif
                const int16_t *qt = p_jpeg->quanttable[
                    p_jpeg->frameheader[ci].quanttable_select];

#ifdef JPEG_JDEC
                if (p_jpeg->progressive)
                {   /* the block from the store */
#ifdef JPEG_IDCT_TRANSPOSE
                    prog_block(p_jpeg, ci, x, blkn, block, qt, transpose);
#else
                    prog_block(p_jpeg, ci, x, blkn, block, qt, false);
#endif
                    goto block_end;
                }
#endif
#ifndef HB_ROWS
                /* Section F.2.2.1: decode the DC coefficient difference */
                huff_decode_dc(p_jpeg, dctbl, s, r);

#ifndef HAVE_LCD_COLOR
                if (!ci || p_jpeg->rgb)
#endif
                {
                    if (s) /* HUFF_EXTEND shifts by s - 1 */
                        s = HUFF_EXTEND(r, s);
                    p_jpeg->last_dc_val[ci] += s;
                    /* output it (assumes zag[0] = 0) */
                    block[0] = MULTIPLY16(p_jpeg->last_dc_val[ci], qt[0]);
                    /* coefficient buffer must be cleared */
                    MEMSET(block+1, 0,
                           p_jpeg->zero_need[SCALE_IDX(ci)] * sizeof(int));
                    /* Section F.2.2.2: decode the AC coefficients */
                    while(true)
                    {
                        huff_decode_ac(p_jpeg, actbl, s);
                        r = s >> 4;
                        s &= 15;
                        k += r;
                        if (s)
                        {
                            check_bit_buffer(p_jpeg, s);
                            if (k >= p_jpeg->k_need[SCALE_IDX(ci)])
                                goto skip_rest;
                            r = get_bits(p_jpeg, s);
                            r = HUFF_EXTEND(r, s);
                            r = MULTIPLY16(r, qt[k]);
#ifdef JPEG_IDCT_TRANSPOSE
                            block[zag[transpose ? k : k + 64]] = r ;
#else
                            block[zag[k]] = r ;
#endif
                        }
                        else
                        {
                            if (r != 15)
                                goto block_end;
                        }
                        if ((++k) & 64)
                            goto block_end;
                    }  /* for k */
                }
                for (; k < 64; k++)
                {
                    huff_decode_ac(p_jpeg, actbl, s);
                    r = s >> 4;
                    s &= 15;

                    if (s)
                    {
                        k += r;
                        check_bit_buffer(p_jpeg, s);
skip_rest:
                        drop_bits(p_jpeg, s);
                    }
                    else
                    {
                        if (r != 15)
                            break;
                        k += r;
                    }
                }  /* for k */
#endif /* HB_ROWS */
block_end:
#ifndef HAVE_LCD_COLOR
                if (!ci || p_jpeg->rgb)
#endif
                {
                    int sc = SCALE_IDX(ci);
                    int idct_cols = BIT_N(MIN(p_jpeg->h_scale[sc], 3));
                    int idct_rows = BIT_N(p_jpeg->v_scale[sc]);
                    int rowstep = b_width;
#ifdef HAVE_LCD_COLOR
                    /* Y, Cb, Cr go to bytes 0-2 for the YUV conversion;
                       R, G, B to their places in struct uint8_rgb */
                    unsigned char *b_out = out + (p_jpeg->rgb ? 2 - ci : ci)
                        + store_offs[p_jpeg->mcu_pos[blkn]];
#else
                    unsigned char *b_out = out
                        + store_offs[p_jpeg->mcu_pos[blkn]];
                    if (ci < 2 && p_jpeg->rgb)
                    {   /* keep R and G until B is decoded */
                        b_out = rgb_tmp[ci];
                        rowstep = 8;
                    }
#endif
                    if (idct_tbl[p_jpeg->v_scale[sc]].v_idct)
#ifdef JPEG_IDCT_TRANSPOSE
                        idct_tbl[p_jpeg->v_scale[sc]].v_idct(block,
                            transpose ? block + 8 * idct_cols
                                      : block + idct_cols);
                    uint16_t * h_block = transpose ? block + 64 : block;
                    idct_tbl[p_jpeg->h_scale[sc]].h_idct(h_block, b_out,
                        h_block + idct_rows * 8, rowstep);
#else
                        idct_tbl[p_jpeg->v_scale[sc]].v_idct(block,
                            block + idct_cols);
                    idct_tbl[p_jpeg->h_scale[sc]].h_idct(block, b_out,
                        block + idct_rows * 8, rowstep);
#endif
#ifndef HAVE_LCD_COLOR
                    if (ci == 2)
                    {   /* luma from R, G and B (JFIF weights) */
                        int w = BIT_N(p_jpeg->h_scale[0]);
                        int xi, yi;
                        for (yi = 0; yi < idct_rows; yi++, b_out += b_width)
                            for (xi = 0; xi < w; xi++)
                                b_out[xi] = (77 * rgb_tmp[0][yi * 8 + xi]
                                    + 150 * rgb_tmp[1][yi * 8 + xi]
                                    + 29 * b_out[xi] + 128) >> 8;
                    }
#endif
                }
            } /* for blkn */
            /* don't starve other threads while an MCU row decodes */
            yield();
#if defined(HAVE_IPOD_UI) && !defined(PLUGIN) && !defined(JDEC_ALONE)
            if (pause_hook)
                pause_hook();       /* the art builder may park here */
#endif
#ifdef HAVE_LCD_COLOR
            unsigned int xp;
            int yp;
            unsigned char *row = out;
            if (p_jpeg->blocks == 1)
            {
                for (yp = 0; yp < height; yp++, row += b_width)
                {
                    unsigned char *px = row;
                    for (xp = 0; xp < mcu_width; xp++, px += JPEG_PIX_SZ)
                    {
                        px[1] = px[2] = px[0];
                    }
                }
            }
#endif
            out += mcu_offset;
            if (p_jpeg->restart_interval && --p_jpeg->restart == 0)
            {   /* if a restart marker is due: */
                p_jpeg->restart = p_jpeg->restart_interval; /* count again */
                search_restart(p_jpeg); /* align the bitstream */
                p_jpeg->last_dc_val[0] = p_jpeg->last_dc_val[1] =
                                 p_jpeg->last_dc_val[2] = 0; /* reset decoder */
            }
        }
#ifdef JPEG_JDEC
        p_jpeg->prog_mcu_y++;
#endif
    } /* if !p_jpeg->mcu_row */
    p_jpeg->mcu_row = (p_jpeg->mcu_row + 1) & (height - 1);
    p_jpeg->part.len = width;
    p_jpeg->part.buf = (jpeg_pix_t *)p_jpeg->out_ptr;
    p_jpeg->out_ptr += b_width;
#ifdef JPEG_JDEC
    if (rows_tap)
    {
        struct img_part row = p_jpeg->part;
        rows_tap->row(rows_tap->arg, &row, p_jpeg->tap_rows++);
    }
#endif
    return &(p_jpeg->part);
}

#ifdef JPEG_JDEC
/* After the output's last row: the tap gets every row of the picture
   (an upscaling output may stop short), and a bad code fails it */
static int decode_end(struct jpeg *p_jpeg, int rows, int bm_size)
{
    while (rows_tap && p_jpeg->tap_rows < rows)
        store_row_jpeg(p_jpeg);
    return p_jpeg->huff_errors ? JPEG_E_BADHUFF : bm_size;
}
#define DECODE_END(p_jpeg, rows, bm_size) decode_end(p_jpeg, rows, bm_size)
#else
#define DECODE_END(p_jpeg, rows, bm_size) (bm_size)
#endif

/******************************************************************************
 * read_jpeg_file()
 *
 * Reads a JPEG file and puts the data in rockbox format in *bitmap.
 *
 *****************************************************************************/
#if !defined(JPEG_FROM_MEM) && !defined(JDEC_ALONE)
/* (jdec's own build, bcmfw/jpeg/env, calls clip_jpeg_fd alone: these are
   left out of it) */
int clip_jpeg_file(const char* filename,
                   int offset,
                   unsigned long jpeg_size,
                   struct bitmap *bm,
                   int maxsize,
                   int format,
                   const struct custom_format *cformat)
{
    int fd, ret;
    fd = open(filename, O_RDONLY);
    JDEBUGF("read_jpeg_file: filename: %s buffer len: %d cformat: %p\n",
        filename, maxsize, cformat);
    /* Exit if file opening failed */
    if (fd < 0) {
        DEBUGF("read_jpeg_file: can't open '%s', rc: %d\n", filename, fd);
        return fd * 10 - 1;
    }
    lseek(fd, offset, SEEK_SET);
    ret = clip_jpeg_fd(fd, 0, jpeg_size, bm, maxsize, format, cformat);
    close(fd);
    return ret;
}

int read_jpeg_file(const char* filename,
                   struct bitmap *bm,
                   int maxsize,
                   int format,
                   const struct custom_format *cformat)
{
    return clip_jpeg_file(filename, 0, 0, bm, maxsize, format, cformat);
}
#endif

static int calc_scale(int in_size, int out_size)
{
    int scale = 0;
    out_size <<= 3;
    for (scale = 0; scale < 3; scale++)
    {
        if (out_size <= in_size)
            break;
        else
            in_size <<= 1;
    }
    return scale;
}

#ifdef JPEG_FROM_MEM
int get_jpeg_dim_mem(unsigned char *data, unsigned long len,
                     struct dim *size)
{
    struct jpeg *p_jpeg = &jpeg;
    memset(p_jpeg, 0, sizeof(struct jpeg));
    p_jpeg->data = data;
    p_jpeg->len = len;
    int status = process_markers(p_jpeg);
    if (status < 0)
        return status;
    if ((status & (DQT | SOF0)) != (DQT | SOF0))
        return -(status * 16);
    size->width = p_jpeg->x_size;
    size->height = p_jpeg->y_size;
    return 0;
}

int decode_jpeg_mem(unsigned char *data,
#else
int clip_jpeg_fd(int fd, int flags,
#endif
                 unsigned long len,
                 struct bitmap *bm,
                 int maxsize,
                 int format,
                 const struct custom_format *cformat)
{
    bool resize = false, dither = false;
    struct rowset rset;
    struct dim src_dim;
    int status;
    int bm_size;
#ifdef JPEG_FROM_MEM
    struct jpeg *p_jpeg = &jpeg;
#else
    struct jpeg *p_jpeg = (struct jpeg*)bm->data;
    int tmp_size = maxsize;
    ALIGN_BUFFER(p_jpeg, tmp_size, JPEG_STRUCT_ALIGN);
    /* not enough memory for our struct jpeg */
    if ((size_t)tmp_size < sizeof(struct jpeg))
        return -1;
#endif
    memset(p_jpeg, 0, sizeof(struct jpeg));
    p_jpeg->len = len;
#ifdef JPEG_FROM_MEM
    p_jpeg->data = data;
#else
    p_jpeg->fd = fd;
    if (p_jpeg->len == 0)
        p_jpeg->len = ffilesize(p_jpeg->fd);

    p_jpeg->read_buf = read_buf;
    p_jpeg->skip_bytes_seek = skip_bytes_seek;

#ifdef HAVE_ALBUMART
    if (flags & AA_FLAG_ID3_UNSYNC)
    {
        p_jpeg->read_buf = read_buf_id3_unsync;
        p_jpeg->custom_param = false;
    }
    else if (flags & AA_FLAG_VORBIS_BASE64)
    {
        struct ogg_file* ogg = alloca(sizeof(*ogg));
        off_t pic_pos = lseek(fd, 0, SEEK_CUR);

        // we need 92 bytes for format probing, reuse some available space
        unsigned char* buf_format = (unsigned char*) p_jpeg->quanttable;
        if (!ogg_file_open_art(ogg, fd, pic_pos, buf_format))
            return -1;

        p_jpeg->read_buf = read_buf_vorbis_base64;
        p_jpeg->skip_bytes_seek = skip_bytes_read_buf;
        p_jpeg->custom_param = ogg;
    }
#else
    (void)flags;
#endif /* HAVE_ALBUMART */

#endif
    status = process_markers(p_jpeg);
#ifndef JPEG_FROM_MEM
    JDEBUGF("position in file: %d buffer fill: %d\n",
        (int)lseek(p_jpeg->fd, 0, SEEK_CUR), p_jpeg->buf_left);
#endif
    if (status < 0)
        return status;
    if ((status & (DQT | SOF0)) != (DQT | SOF0))
        return -(status * 16);
    if (!(status & DHT)) /* if no Huffman table present: */
        default_huff_tbl(p_jpeg); /* use default */
    fix_headers(p_jpeg); /* derive Huffman and other lookup-tables */

    /*the dim array in rockbox is limited to 2^15-1 pixels, so we cannot resize
      images larger than this without overflowing */
    if(p_jpeg->x_size > 32767 || p_jpeg->y_size > 32767)
    {
        JDEBUGF("Aborting resize of image > 32767 pixels\n");
        return -1;
    }

    src_dim.width = p_jpeg->x_size;
    src_dim.height = p_jpeg->y_size;
    if (format & FORMAT_RESIZE)
        resize = true;
    if (format & FORMAT_DITHER)
        dither = true;
#ifdef HAVE_LCD_COLOR
    bm->alpha_offset = 0; /* no alpha channel */
#endif
    if (resize) {
        struct dim resize_dim = {
            .width = bm->width,
            .height = bm->height,
        };
        if (format & FORMAT_KEEP_ASPECT)
            recalc_dimension(&resize_dim, &src_dim);
        bm->width = resize_dim.width;
        bm->height = resize_dim.height;
    } else {
        bm->width = p_jpeg->x_size;
        bm->height = p_jpeg->y_size;
    }
    p_jpeg->h_scale[0] = calc_scale(p_jpeg->x_size, bm->width);
    p_jpeg->v_scale[0] = calc_scale(p_jpeg->y_size, bm->height);
    JDEBUGF("luma IDCT size: %dx%d\n", BIT_N(p_jpeg->h_scale[0]),
        BIT_N(p_jpeg->v_scale[0]));
    if ((p_jpeg->x_size << p_jpeg->h_scale[0]) >> 3 == bm->width &&
        (p_jpeg->y_size << p_jpeg->v_scale[0]) >> 3 == bm->height)
        resize = false;
#ifdef HAVE_LCD_COLOR
    {   /* chroma blocks are scaled up by the luma:chroma sampling ratio */
        struct frame_component *chroma =
            &p_jpeg->frameheader[p_jpeg->blocks > 1 ? 1 : 0];
        p_jpeg->h_scale[1] = p_jpeg->h_scale[0] +
            p_jpeg->frameheader[0].horizontal_sampling -
            chroma->horizontal_sampling;
        p_jpeg->v_scale[1] = p_jpeg->v_scale[0] +
            p_jpeg->frameheader[0].vertical_sampling -
            chroma->vertical_sampling;
    }
    JDEBUGF("chroma IDCT size: %dx%d\n", BIT_N(p_jpeg->h_scale[1]),
        BIT_N(p_jpeg->v_scale[1]));
#endif
    JDEBUGF("scaling from %dx%d -> %dx%d\n",
        (p_jpeg->x_size << p_jpeg->h_scale[0]) >> 3,
        (p_jpeg->y_size << p_jpeg->v_scale[0]) >> 3,
        bm->width, bm->height);
    fix_quant_tables(p_jpeg);
    int decode_w = BIT_N(p_jpeg->h_scale[0]) - 1;
    int decode_h = BIT_N(p_jpeg->v_scale[0]) - 1;
    src_dim.width = (p_jpeg->x_size << p_jpeg->h_scale[0]) >> 3;
    src_dim.height = (p_jpeg->y_size << p_jpeg->v_scale[0]) >> 3;
#ifdef JPEG_IDCT_TRANSPOSE
    if (p_jpeg->v_scale[0] > 2)
        p_jpeg->zero_need[0] = (decode_w << 3) + decode_h;
    else
#endif
        p_jpeg->zero_need[0] = (decode_h << 3) + decode_w;
    p_jpeg->k_need[0] = zig[(decode_h << 3) + decode_w];
    JDEBUGF("need luma components to %d\n", p_jpeg->k_need[0]);
#ifdef HAVE_LCD_COLOR
    decode_w = BIT_N(MIN(p_jpeg->h_scale[1],3)) - 1;
    decode_h = BIT_N(MIN(p_jpeg->v_scale[1],3)) - 1;
    if (p_jpeg->v_scale[1] > 2)
        p_jpeg->zero_need[1] = (decode_w << 3) + decode_h;
    else
        p_jpeg->zero_need[1] = (decode_h << 3) + decode_w;
    p_jpeg->k_need[1] = zig[(decode_h << 3) + decode_w];
    JDEBUGF("need chroma components to %d\n", p_jpeg->k_need[1]);
#endif
    if (cformat)
        bm_size = cformat->get_size(bm);
    else
        bm_size = BM_SIZE(bm->width,bm->height,FORMAT_NATIVE,false);
#ifdef JPEG_JDEC
    if (rows_tap && rows_tap->only)
        bm_size = 0; /* rows only (jhook.h): no bitmap of our own */
#endif

    char *buf_start = (char *)bm->data + bm_size;
    char *buf_end = (char *)bm->data + maxsize;
    bool return_size = format & FORMAT_RETURN_SIZE;
#ifndef JPEG_FROM_MEM
    ALIGN_BUFFER(buf_start, maxsize, JPEG_STRUCT_ALIGN);
    if (!return_size)
    {
#ifdef JPEG_JDEC
        if (rows_tap && rows_tap->only && maxsize < (int)sizeof(struct jpeg))
            return JPEG_E_NOMEM;
#endif
        if (maxsize < (int)sizeof(struct jpeg))
            return -1;
        memmove(buf_start, p_jpeg, sizeof(struct jpeg));
        p_jpeg = (struct jpeg *)buf_start;
    }
    buf_start += sizeof(struct jpeg);
#endif
    maxsize = buf_end - buf_start;
    int decode_buf_size = (p_jpeg->x_mbl << p_jpeg->h_scale[0])
        << p_jpeg->v_scale[0];
    decode_buf_size <<= p_jpeg->frameheader[0].horizontal_sampling +
        p_jpeg->frameheader[0].vertical_sampling - 2;
    decode_buf_size *= JPEG_PIX_SZ;
#ifdef JDEC_ALONE
    /* store_row_jpeg's rows, 32-byte aligned */
    decode_buf_size = (((p_jpeg->x_mbl << (p_jpeg->h_scale[0] +
        p_jpeg->frameheader[0].horizontal_sampling - 1)) * JPEG_PIX_SZ + 31) & ~31)
        << (p_jpeg->v_scale[0] + p_jpeg->frameheader[0].vertical_sampling - 1);
#endif
    JDEBUGF("decode buffer size: %d\n", decode_buf_size);
    if (return_size)
    {
        return (buf_start - (char *) bm->data) + decode_buf_size
               + (resize
                      ?
                      /* buffer for 1 line + 2 spare lines */
#ifdef HAVE_LCD_COLOR
                      sizeof(struct uint32_argb)
#else
                      sizeof(uint32_t)
#endif
                      * 3 * bm->width
                      : 0);
    }

#ifdef JPEG_JDEC
    if (rows_tap && rows_tap->only && buf_end - buf_start < decode_buf_size)
        return JPEG_E_NOMEM;
#endif
    if (buf_end - buf_start < decode_buf_size)
        return -1;

    fix_huff_tables(p_jpeg);
#ifdef JPEG_JDEC
#if !defined(JPEG_FROM_MEM) && defined(HAVE_LCD_COLOR)
    if (!p_jpeg->progressive && !fix_huff_fast_tables(p_jpeg))
        HUFF_BAD(p_jpeg);
#endif
#endif

    p_jpeg->img_buf = (jpeg_pix_t *)buf_start;
    buf_start += decode_buf_size;
    maxsize = buf_end - buf_start;
    memset(p_jpeg->img_buf, 0, decode_buf_size);
#ifdef JPEG_JDEC
    if (p_jpeg->progressive)
    {   /* every scan into the store (jprog.h), at the end of the buffer */
        status = prog_decode(p_jpeg, (unsigned char *)buf_start, &maxsize,
                             resize && !(rows_tap && rows_tap->only)
                             ? (int)(sizeof(struct uint32_argb) * 3
                                     * bm->width) : 0);
        if (status < 0)
            return status;
    }
#endif
    p_jpeg->mcu_row = 0;
    p_jpeg->restart = p_jpeg->restart_interval;
    rset.rowstart = 0;
    rset.rowstop = bm->height;
    rset.rowstep = 1;
    p_jpeg->resize = resize;
#ifdef JPEG_JDEC
    if (rows_tap)
        rows_tap->begin(rows_tap->arg, &src_dim,
                        p_jpeg->blocks > 1 && !p_jpeg->rgb);
    if (rows_tap && rows_tap->only) /* every row to the tap, nothing more */
        return DECODE_END(p_jpeg, src_dim.height, 1);
#endif
    if (resize)
    {
        if (resize_on_load(bm, dither, &src_dim, &rset, buf_start, maxsize,
            cformat, IF_PIX_FMT(p_jpeg->blocks == 1 || p_jpeg->rgb ? 0 : 1,)
            store_row_jpeg,
            p_jpeg))
            return DECODE_END(p_jpeg, src_dim.height, bm_size);
    } else {
        int row;
        struct scaler_context ctx = {
            .bm = bm,
            .dither = dither,
        };
#if LCD_DEPTH > 1
        void (*output_row_8)(uint32_t, void*, struct scaler_context*) =
            output_row_8_native;
#elif defined(PLUGIN)
        void (*output_row_8)(uint32_t, void*, struct scaler_context*) = NULL;
#endif
#if LCD_DEPTH > 1 || defined(PLUGIN)
        if (cformat)
            output_row_8 = cformat->output_row_8;
#endif
        struct img_part *part;
        for (row = 0; row < bm->height; row++)
        {
            part = store_row_jpeg(p_jpeg);
#if defined(HAVE_IPOD_UI) && !defined(PLUGIN)
            if (!part)
                return 0;           /* called off (abort_hook) */
#endif
#if defined(JPEG_JDEC) && defined(HAVE_LCD_COLOR)
            if (jdec_yuv565 && dither && !cformat && p_jpeg->blocks > 1 &&
                !p_jpeg->rgb)
            {   /* the colour stage below and output_row_8, at once */
                jdec_yuv565((fb_data *)bm->data + row * bm->width, part->buf,
                            bm->width, DITHERY(row));
                continue;
            }
#endif
#ifdef HAVE_LCD_COLOR
            if (p_jpeg->blocks > 1 && !p_jpeg->rgb)
            {
                struct uint8_rgb *qp = part->buf;
                struct uint8_rgb *end = qp + bm->width;
                uint8_t y, u, v;
                unsigned r, g, b;
                for (; qp < end; qp++)
                {
                    y = qp->blue;
                    u = qp->green;
                    v = qp->red;
                    yuv_to_rgb(y, u, v, &r, &g, &b);
                    qp->red = r;
                    qp->blue = b;
                    qp->green = g;
                }
            }
#endif
            output_row_8(row, part->buf, &ctx);
        }
        return DECODE_END(p_jpeg, src_dim.height, bm_size);
    }
    return 0;
}

#if !defined(JPEG_FROM_MEM) && !defined(JDEC_ALONE)
int read_jpeg_fd(int fd,
                 struct bitmap *bm,
                 int maxsize,
                 int format,
                 const struct custom_format *cformat)
{
    return clip_jpeg_fd(fd, 0, 0, bm, maxsize, format, cformat);
}
#endif

const size_t JPEG_DECODE_OVERHEAD =
    /* Reserve an arbitrary amount for the decode buffer
     * FIXME: Somebody who knows what they're doing should look at this */
    (38 * 1024)
#ifndef JPEG_FROM_MEM
    /* Unless the struct jpeg is defined statically, we need to allocate
     * it in the bitmap buffer as well */
    + sizeof(struct jpeg)
#endif
    ;

/**************** end JPEG code ********************/
