/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2008 by Akio Idehara, Andrew Mahone
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

/*
 * Implementation of area average and linear row and vertical scalers, and
 * nearest-neighbor grey scaler (C) 2008 Andrew Mahone
 *
 * All files in this archive are subject to the GNU General Public License.
 * See the file COPYING in the source tree root for full license agreement.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "general.h"
#include "kernel.h"
#include "system.h"
#ifndef PLUGIN
#include "debug.h"
#endif
#include "lcd.h"
#include "file.h"
#ifdef HAVE_REMOTE_LCD
#include "lcd-remote.h"
#endif
#ifdef ROCKBOX_DEBUG_SCALERS
#define SDEBUGF DEBUGF
#else
#define SDEBUGF(...)
#endif
#ifndef __PCTOOL__
#include "config.h"
#include "system.h"
#include <bmp.h>
#include "resize.h"
#else
#undef DEBUGF
#define DEBUGF(...)
#endif
#include <jpeg_load.h>
#ifdef JPEG_JDEC
#include "jhook.h"      /* jdec's colour hook (jdec_yuv565) */
#endif

#define MULUQ(a, b) ((a) * (b))
#define MULQ(a, b) ((a) * (b))

#ifdef HAVE_LCD_COLOR
#define CHANNEL_BYTES (sizeof(struct uint32_argb)/sizeof(uint32_t))
#else
#define CHANNEL_BYTES (sizeof(uint32_t)/sizeof(uint32_t)) /* packed */
#endif

/* calculate the maximum dimensions which will preserve the aspect ration of
   src while fitting in the constraints passed in dst, and store result in dst,
   returning 0 if rounding and 1 if not rounding.
*/
int recalc_dimension(struct dim *dst, struct dim *src)
{
    /* This only looks backwards. The input image size is being pre-scaled by
     * the inverse of the pixel aspect ratio, so that once the size it scaled
     * to meet the output constraints, the scaled image will have appropriate
     * proportions.
     */
    int sw = src->width * LCD_PIXEL_ASPECT_HEIGHT;
    int sh = src->height * LCD_PIXEL_ASPECT_WIDTH;
    int tmp;
    if (dst->width <= 0)
        dst->width = LCD_WIDTH;
    if (dst->height <= 0)
        dst->height = LCD_HEIGHT;
#ifndef HAVE_UPSCALER
    if (dst->width > sw || dst->height > sh)
    {
        dst->width = sw;
        dst->height = sh;
    }
    if (sw == dst->width && sh == dst->height)
        return 1;
#endif
    tmp = (sw * dst->height + (sh >> 1)) / sh;
    if (tmp > dst->width)
        dst->height = (sh * dst->width + (sw >> 1)) / sw;
    else
        dst->width = tmp;
    return src->width == dst->width && src->height == dst->height;
}

/* All of these scalers use variations of Bresenham's algorithm to convert from
   their input to output coordinates.  The error value is shifted from the
   "classic" version such that it is a useful input to the scaling calculation.
*/

#ifdef HAVE_LCD_COLOR
/* dither + pack on channel of RGB565, R an B share a packing macro */
#define PACKRB(v, delta)  ((31 * v + (v >> 3) + delta) >> 8)
#define PACKG(g, delta) ((63 * g + (g >> 2) + delta) >> 8)
#endif

/* read new img_part unconditionally, return false on failure */
#define FILL_BUF_INIT(img_part, store_part, args) { \
    img_part = store_part(args); \
    if (img_part == NULL) \
        return false; \
}

/* read new img_part if current one is empty, return false on failure */
#define FILL_BUF(img_part, store_part, args) { \
    if (img_part->len == 0) \
        img_part = store_part(args); \
    if (img_part == NULL) \
        return false; \
}

#if defined(CPU_COLDFIRE)
#define MAC(op1, op2, num) \
    asm volatile( \
        "mac.l %0, %1, %%acc" #num \
        : \
        : "%d" (op1), "d" (op2)\
    )
#define MAC_OUT(dest, num) \
    asm volatile( \
        "movclr.l %%acc" #num ", %0" \
        : "=d" (dest) \
    )
#endif

#if defined(HAVE_IPOD_UI) && !defined(PLUGIN) && !defined(JDEC_ALONE)
static void resize_boost_drop(bool end);
#endif

/* horizontal area average scaler */
static bool scale_h_area(void *out_line_ptr,
                         struct scaler_context *ctx, bool accum)
{
    SDEBUGF("scale_h_area\n");
    unsigned int ix, ox, oxe, mul;
    const uint32_t h_i_val = ctx->h_i_val,
                   h_o_val = ctx->h_o_val;
#ifdef HAVE_LCD_COLOR
    struct uint32_argb rgbvalacc = { 0, 0, 0, 0 },
                       rgbvaltmp = { 0, 0, 0, 0 },
                      *out_line = (struct uint32_argb *)out_line_ptr;
#else
    uint32_t acc = 0, tmp = 0, *out_line = (uint32_t*)out_line_ptr;
#endif
    struct img_part *part;
    FILL_BUF_INIT(part,ctx->store_part,ctx->args);
    ox = 0;
    oxe = 0;
    mul = 0;
    /* give other tasks a chance to run */
    yield();
#if defined(HAVE_IPOD_UI) && !defined(PLUGIN) && !defined(JDEC_ALONE)
    resize_boost_drop(false);
    resize_pause_point();
#endif
    for (ix = 0; ix < (unsigned int)ctx->src->width; ix++)
    {
        oxe += h_o_val;
        /* end of current area has been reached */
        /* fill buffer if needed */
        FILL_BUF(part,ctx->store_part,ctx->args);
#ifdef HAVE_LCD_COLOR
        if (oxe >= h_i_val)
        {
            /* "reset" error, which now represents partial coverage of next
               pixel by the next area
            */
            oxe -= h_i_val;

#if defined(CPU_COLDFIRE)
/* Coldfire EMAC math */
            /* add saved partial pixel from start of area */
            MAC(rgbvalacc.r, h_o_val, 0);
            MAC(rgbvalacc.g, h_o_val, 1);
            MAC(rgbvalacc.b, h_o_val, 2);
            MAC(rgbvalacc.a, h_o_val, 3);
            MAC(rgbvaltmp.r, mul, 0);
            MAC(rgbvaltmp.g, mul, 1);
            MAC(rgbvaltmp.b, mul, 2);
            MAC(rgbvaltmp.a, mul, 3);
            /* get new pixel , then add its partial coverage to this area */
            mul = h_o_val - oxe;
            rgbvaltmp.r = part->buf->red;
            rgbvaltmp.g = part->buf->green;
            rgbvaltmp.b = part->buf->blue;
            rgbvaltmp.a = part->buf->alpha;
            MAC(rgbvaltmp.r, mul, 0);
            MAC(rgbvaltmp.g, mul, 1);
            MAC(rgbvaltmp.b, mul, 2);
            MAC(rgbvaltmp.a, mul, 3);
            MAC_OUT(rgbvalacc.r, 0);
            MAC_OUT(rgbvalacc.g, 1);
            MAC_OUT(rgbvalacc.b, 2);
            MAC_OUT(rgbvalacc.a, 3);
#else
/* generic C math */
            /* add saved partial pixel from start of area */
            rgbvalacc.r = rgbvalacc.r * h_o_val + rgbvaltmp.r * mul;
            rgbvalacc.g = rgbvalacc.g * h_o_val + rgbvaltmp.g * mul;
            rgbvalacc.b = rgbvalacc.b * h_o_val + rgbvaltmp.b * mul;
            rgbvalacc.a = rgbvalacc.a * h_o_val + rgbvaltmp.a * mul;

            /* get new pixel , then add its partial coverage to this area */
            rgbvaltmp.r = part->buf->red;
            rgbvaltmp.g = part->buf->green;
            rgbvaltmp.b = part->buf->blue;
            rgbvaltmp.a = part->buf->alpha;
            mul = h_o_val - oxe;
            rgbvalacc.r += rgbvaltmp.r * mul;
            rgbvalacc.g += rgbvaltmp.g * mul;
            rgbvalacc.b += rgbvaltmp.b * mul;
            rgbvalacc.a += rgbvaltmp.a * mul;
#endif /* CPU */
            rgbvalacc.r = (rgbvalacc.r + (1 << 21)) >> 22;
            rgbvalacc.g = (rgbvalacc.g + (1 << 21)) >> 22;
            rgbvalacc.b = (rgbvalacc.b + (1 << 21)) >> 22;
            rgbvalacc.a = (rgbvalacc.a + (1 << 21)) >> 22;
            /* store or accumulate to output row */
            if (accum)
            {
                rgbvalacc.r += out_line[ox].r;
                rgbvalacc.g += out_line[ox].g;
                rgbvalacc.b += out_line[ox].b;
                rgbvalacc.a += out_line[ox].a;
            }
            out_line[ox].r = rgbvalacc.r;
            out_line[ox].g = rgbvalacc.g;
            out_line[ox].b = rgbvalacc.b;
            out_line[ox].a = rgbvalacc.a;
            /* reset accumulator */
            rgbvalacc.r = 0;
            rgbvalacc.g = 0;
            rgbvalacc.b = 0;
            rgbvalacc.a = 0;
            mul = oxe;
            ox += 1;
        /* inside an area */
        } else {
            /* add pixel value to accumulator */
            rgbvalacc.r += part->buf->red;
            rgbvalacc.g += part->buf->green;
            rgbvalacc.b += part->buf->blue;
            rgbvalacc.a += part->buf->alpha;
        }
#else
        if (oxe >= h_i_val)
        {
            /* "reset" error, which now represents partial coverage of next
               pixel by the next area
            */
            oxe -= h_i_val;
#if defined(CPU_COLDFIRE)
/* Coldfire EMAC math */
            /* add saved partial pixel from start of area */
            MAC(acc, h_o_val, 0);
            MAC(tmp, mul, 0);
            /* get new pixel , then add its partial coverage to this area */
            tmp = *(part->buf);
            mul = h_o_val - oxe;
            MAC(tmp, mul, 0);
            MAC_OUT(acc, 0);
#else
/* generic C math */
            /* add saved partial pixel from start of area */
            acc = (acc * h_o_val) + (tmp * mul);

            /* get new pixel , then add its partial coverage to this area */
            tmp = *(part->buf);
            mul = h_o_val - oxe;
            acc += tmp * mul;
#endif /* CPU */
            /* round, divide, and either store or accumulate to output row */
            acc = (acc + (1 << 21)) >> 22;
            if (accum)
            {
                acc += out_line[ox];
            }
            out_line[ox] = acc;
            /* reset accumulator */
            acc = 0;
            mul = oxe;
            ox += 1;
        /* inside an area */
        } else {
            /* add pixel value to accumulator */
            acc += *(part->buf);
        }
#endif
        part->buf++;
        part->len--;
    }
    return true;
}

/* vertical area average scaler */
static inline bool scale_v_area(struct rowset *rset, struct scaler_context *ctx)
{
    uint32_t mul, oy, iy, oye;
    const uint32_t v_i_val = ctx->v_i_val,
                   v_o_val = ctx->v_o_val;

    /* Set up rounding and scale factors */
    mul = 0;
    oy = rset->rowstart;
    oye = 0;
    uint32_t *rowacc = (uint32_t *) ctx->buf,
             *rowtmp = rowacc + ctx->bm->width * CHANNEL_BYTES,
             *rowacc_px, *rowtmp_px;
    memset((void *)ctx->buf, 0, ctx->bm->width * 2 * sizeof(uint32_t)*CHANNEL_BYTES);
    SDEBUGF("scale_v_area\n");
    /* zero the accumulator and temp rows */
    for (iy = 0; iy < (unsigned int)ctx->src->height; iy++)
    {
        oye += v_o_val;
        /* end of current area has been reached */
        if (oye >= v_i_val)
        {
            /* "reset" error, which now represents partial coverage of the next
               row by the next area
            */
            oye -= v_i_val;
            /* add stored partial row to accumulator */
            for(rowacc_px = rowacc, rowtmp_px = rowtmp; rowacc_px != rowtmp;
                rowacc_px++, rowtmp_px++)
                *rowacc_px = *rowacc_px * v_o_val + *rowtmp_px * mul;
            /* store new scaled row in temp row */
            if(!ctx->h_scaler(rowtmp, ctx, false))
                return false;
            /* add partial coverage by new row to this area, then round and
               scale to final value
            */
            mul = v_o_val - oye;
            for(rowacc_px = rowacc, rowtmp_px = rowtmp; rowacc_px != rowtmp;
                rowacc_px++, rowtmp_px++)
                *rowacc_px += mul * *rowtmp_px;
            ctx->output_row(oy, (void*)rowacc, ctx);
            /* clear accumulator row, store partial coverage for next row */
            memset((void *)rowacc, 0, ctx->bm->width * sizeof(uint32_t) * CHANNEL_BYTES);
            mul = oye;
            oy += rset->rowstep;
        /* inside an area */
        } else {
            /* accumulate new scaled row to rowacc */
            if (!ctx->h_scaler(rowacc, ctx, true))
                return false;
        }
    }
    return true;
}

#ifdef HAVE_UPSCALER
/* horizontal linear scaler */
static bool scale_h_linear(void *out_line_ptr, struct scaler_context *ctx,
                           bool accum)
{
    unsigned int ix, ox, ixe;
    const uint32_t h_i_val = ctx->h_i_val,
                   h_o_val = ctx->h_o_val;
    /* type x = x is an ugly hack for hiding an unitialized data warning. The
       values are conditionally initialized before use, but other values are
       set such that this will occur before these are used.
    */
#ifdef HAVE_LCD_COLOR
    struct uint32_argb rgbval=rgbval, rgbinc=rgbinc,
                      *out_line = (struct uint32_argb*)out_line_ptr;
#else
    uint32_t val=val, inc=inc, *out_line = (uint32_t*)out_line_ptr;
#endif
    struct img_part *part;
    SDEBUGF("scale_h_linear\n");
    FILL_BUF_INIT(part,ctx->store_part,ctx->args);
    ix = 0;
    /* The error is set so that values are initialized on the first pass. */
    ixe = h_o_val;
    /* give other tasks a chance to run */
    yield();
#if defined(HAVE_IPOD_UI) && !defined(PLUGIN) && !defined(JDEC_ALONE)
    resize_pause_point();
#endif
    for (ox = 0; ox < (uint32_t)ctx->bm->width; ox++)
    {
#ifdef HAVE_LCD_COLOR
        if (ixe >= h_o_val)
        {
            /* Store the new "current" pixel value in rgbval, and the color
               step value in rgbinc.
            */
            ixe -= h_o_val;
            rgbinc.r = -(part->buf->red);
            rgbinc.g = -(part->buf->green);
            rgbinc.b = -(part->buf->blue);
            rgbinc.a = -(part->buf->alpha);
#if defined(CPU_COLDFIRE)
/* Coldfire EMAC math */
            MAC(part->buf->red, h_o_val, 0);
            MAC(part->buf->green, h_o_val, 1);
            MAC(part->buf->blue, h_o_val, 2);
            MAC(part->buf->alpha, h_o_val, 3);
#else
/* generic C math */
            rgbval.r = (part->buf->red) * h_o_val;
            rgbval.g = (part->buf->green) * h_o_val;
            rgbval.b = (part->buf->blue) * h_o_val;
            rgbval.a = (part->buf->alpha) * h_o_val;
#endif /* CPU */
            ix += 1;
            /* If this wasn't the last pixel, add the next one to rgbinc. */
            if (LIKELY(ix < (uint32_t)ctx->src->width)) {
                part->buf++;
                part->len--;
                /* Fetch new pixels if needed */
                FILL_BUF(part,ctx->store_part,ctx->args);
                rgbinc.r += part->buf->red;
                rgbinc.g += part->buf->green;
                rgbinc.b += part->buf->blue;
                rgbinc.a += part->buf->alpha;
                /* Add a partial step to rgbval, in this pixel isn't precisely
                   aligned with the new source pixel
                */
#if defined(CPU_COLDFIRE)
/* Coldfire EMAC math */
                MAC(rgbinc.r, ixe, 0);
                MAC(rgbinc.g, ixe, 1);
                MAC(rgbinc.b, ixe, 2);
                MAC(rgbinc.a, ixe, 3);
#else
/* generic C math */
                rgbval.r += rgbinc.r * ixe;
                rgbval.g += rgbinc.g * ixe;
                rgbval.b += rgbinc.b * ixe;
                rgbval.a += rgbinc.a * ixe;
#endif
            }
#if defined(CPU_COLDFIRE)
/* get final EMAC result out of ACC registers */
            MAC_OUT(rgbval.r, 0);
            MAC_OUT(rgbval.g, 1);
            MAC_OUT(rgbval.b, 2);
            MAC_OUT(rgbval.a, 3);
#endif
            /* Now multiply the color increment to its proper value */
            rgbinc.r *= h_i_val;
            rgbinc.g *= h_i_val;
            rgbinc.b *= h_i_val;
            rgbinc.a *= h_i_val;
        } else {
            rgbval.r += rgbinc.r;
            rgbval.g += rgbinc.g;
            rgbval.b += rgbinc.b;
            rgbval.a += rgbinc.a;
        }
        /* round and scale values, and accumulate or store to output */
        if (accum)
        {
            out_line[ox].r += (rgbval.r + (1 << 21)) >> 22;
            out_line[ox].g += (rgbval.g + (1 << 21)) >> 22;
            out_line[ox].b += (rgbval.b + (1 << 21)) >> 22;
            out_line[ox].a += (rgbval.a + (1 << 21)) >> 22;
        } else {
            out_line[ox].r = (rgbval.r + (1 << 21)) >> 22;
            out_line[ox].g = (rgbval.g + (1 << 21)) >> 22;
            out_line[ox].b = (rgbval.b + (1 << 21)) >> 22;
            out_line[ox].a = (rgbval.a + (1 << 21)) >> 22;
        }
#else
        if (ixe >= h_o_val)
        {
            /* Store the new "current" pixel value in rgbval, and the color
               step value in rgbinc.
            */
            ixe -= h_o_val;
            val = *(part->buf);
            inc = -val;
#if defined(CPU_COLDFIRE)
/* Coldfire EMAC math */
            MAC(val, h_o_val, 0);
#else
/* generic C math */
            val = val * h_o_val;
#endif
            ix += 1;
            /* If this wasn't the last pixel, add the next one to rgbinc. */
            if (LIKELY(ix < (uint32_t)ctx->src->width)) {
                part->buf++;
                part->len--;
                /* Fetch new pixels if needed */
                FILL_BUF(part,ctx->store_part,ctx->args);
                inc += *(part->buf);
                /* Add a partial step to rgbval, in this pixel isn't precisely
                   aligned with the new source pixel
                */
#if defined(CPU_COLDFIRE)
/* Coldfire EMAC math */
                MAC(inc, ixe, 0);
#else
/* generic C math */
                val += inc * ixe;
#endif
            }
#if defined(CPU_COLDFIRE)
/* get final EMAC result out of ACC register */
            MAC_OUT(val, 0);
#endif
            /* Now multiply the color increment to its proper value */
/* generic C math */
            inc *= h_i_val;
        } else
            val += inc;
        /* round and scale values, and accumulate or store to output */
        if (accum)
        {
            out_line[ox] += (val + (1 << 21)) >> 22;
        } else {
            out_line[ox] = (val + (1 << 21)) >> 22;
        }
#endif
        ixe += h_i_val;
    }
    return true;
}

/* vertical linear scaler */
static inline bool scale_v_linear(struct rowset *rset,
                                  struct scaler_context *ctx)
{
    uint32_t iy, iye;
    int32_t oy;
    const uint32_t v_i_val = ctx->v_i_val,
                   v_o_val = ctx->v_o_val;
    /* Set up our buffers, to store the increment and current value for each
       column, and one temp buffer used to read in new rows.
    */
    uint32_t *rowinc = (uint32_t *)(ctx->buf),
             *rowval = rowinc + ctx->bm->width * CHANNEL_BYTES,
             *rowtmp = rowval + ctx->bm->width * CHANNEL_BYTES,
             *rowinc_px, *rowval_px, *rowtmp_px;

    SDEBUGF("scale_v_linear\n");
    iy = 0;
    iye = v_o_val;
    /* get first scaled row in rowtmp */
    if(!ctx->h_scaler((void*)rowtmp, ctx, false))
        return false;
    for (oy = rset->rowstart; oy != rset->rowstop; oy += rset->rowstep)
    {
        if (iye >= v_o_val)
        {
            iye -= v_o_val;
            iy += 1;
            for(rowinc_px = rowinc, rowtmp_px = rowtmp, rowval_px = rowval;
                rowinc_px < rowval; rowinc_px++, rowtmp_px++, rowval_px++)
            {
                *rowinc_px = -*rowtmp_px;
                *rowval_px = *rowtmp_px * v_o_val;
            }
            if (iy < (uint32_t)ctx->src->height)
            {
                if (!ctx->h_scaler((void*)rowtmp, ctx, false))
                    return false;
                for(rowinc_px = rowinc, rowtmp_px = rowtmp, rowval_px = rowval;
                    rowinc_px < rowval; rowinc_px++, rowtmp_px++, rowval_px++)
                {
                    *rowinc_px += *rowtmp_px;
                    *rowval_px += *rowinc_px * iye;
                    *rowinc_px *= v_i_val;
                }
            }
        } else
            for(rowinc_px = rowinc, rowval_px = rowval; rowinc_px < rowval;
                rowinc_px++, rowval_px++)
                *rowval_px += *rowinc_px;
        ctx->output_row(oy, (void*)rowval, ctx);
        iye += v_i_val;
    }
    return true;
}
#endif /* HAVE_UPSCALER */

#if defined(HAVE_LCD_COLOR) && (defined(HAVE_JPEG) || defined(PLUGIN))
static void output_row_32_native_fromyuv(uint32_t row, void * row_in,
                               struct scaler_context *ctx)
{
#if LCD_STRIDEFORMAT == VERTICAL_STRIDE
#define DEST_STEP   (ctx->bm->height)
#define Y_STEP      (1)
#else
#define DEST_STEP   (1)
#define Y_STEP      (BM_WIDTH(ctx->bm->width,FORMAT_NATIVE,0))
#endif

    int col;
    uint8_t dy = DITHERY(row);
    struct uint32_argb *qp = (struct uint32_argb *)row_in;
    SDEBUGF("output_row: y: %lu in: %p\n",row, row_in);
    fb_data *dest = (fb_data *)ctx->bm->data + Y_STEP * row;
    int delta = 127;
    unsigned r, g, b, y, u, v;
    
#if defined(JPEG_JDEC) && LCD_STRIDEFORMAT == HORIZONTAL_STRIDE
    if (jdec_yuv565 && ctx->dither)
    {   /* the same Y, Cb, Cr, 16 at a time, through jdec's colour stage */
        struct uint8_rgb px[16] __attribute__((aligned(4)));

        for (col = 0; col < ctx->bm->width; col += 16)
        {
            int i, n = MIN(16, ctx->bm->width - col);

            for (i = 0; i < n; i++, qp++)
            {
                px[i].blue = SC_OUT(qp->b, ctx);
                px[i].green = SC_OUT(qp->g, ctx);
                px[i].red = SC_OUT(qp->r, ctx);
            }
            jdec_yuv565(dest + col, px, n, dy);
        }
        return;
    }
#endif
    for (col = 0; col < ctx->bm->width; col++) {
        (void) delta;
        if (ctx->dither)
            delta = DITHERXDY(col,dy);
        y = SC_OUT(qp->b, ctx);
        u = SC_OUT(qp->g, ctx);
        v = SC_OUT(qp->r, ctx);
        qp++;
        yuv_to_rgb(y, u, v, &r, &g, &b);
#if LCD_DEPTH < 24
        r = (31 * r + (r >> 3) + delta) >> 8;
        g = (63 * g + (g >> 2) + delta) >> 8;
        b = (31 * b + (b >> 3) + delta) >> 8;
#endif
        *dest = FB_RGBPACK_LCD(r, g, b);
        dest += DEST_STEP;
    }
}
#endif

#if !defined(PLUGIN) || LCD_DEPTH > 1
static void output_row_32_native(uint32_t row, void * row_in,
                              struct scaler_context *ctx)
{
    int col;
    int fb_width = BM_WIDTH(ctx->bm->width,FORMAT_NATIVE,0);
    uint8_t dy = DITHERY(row);
#ifdef HAVE_LCD_COLOR
    struct uint32_argb *qp = (struct uint32_argb*)row_in;
#else
    uint32_t *qp = (uint32_t*)row_in;
#endif
    SDEBUGF("output_row: y: %lu in: %p\n",row, row_in);
#if LCD_DEPTH == 2
#if LCD_PIXELFORMAT == HORIZONTAL_PACKING
                /* greyscale iPods */
                fb_data *dest = (fb_data *)ctx->bm->data + fb_width * row;
                int shift = 6;
                int delta = 127;
                unsigned bright;
                unsigned data = 0;

                for (col = 0; col < ctx->bm->width; col++) {
                    if (ctx->dither)
                        delta = DITHERXDY(col,dy);
                    bright = SC_OUT(*qp++, ctx);
                    bright = (3 * bright + (bright >> 6) + delta) >> 8;
                    data |= (~bright & 3) << shift;
                    shift -= 2;
                    if (shift < 0) {
                        *dest++ = data;
                        data = 0;
                        shift = 6;
                    }
                }
                if (shift < 6)
                    *dest++ = data;
#elif LCD_PIXELFORMAT == VERTICAL_PACKING
                /* iriver H1x0 */
                fb_data *dest = (fb_data *)ctx->bm->data + fb_width *
                                (row >> 2);
                int shift = 2 * (row & 3);
                int delta = 127;
                unsigned bright;

                for (col = 0; col < ctx->bm->width; col++) {
                    if (ctx->dither)
                        delta = DITHERXDY(col,dy);
                    bright = SC_OUT(*qp++, ctx);
                    bright = (3 * bright + (bright >> 6) + delta) >> 8;
                    *dest++ |= (~bright & 3) << shift;
                }
#elif LCD_PIXELFORMAT == VERTICAL_INTERLEAVED
                /* iAudio M3 */
                fb_data *dest = (fb_data *)ctx->bm->data + fb_width *
                                (row >> 3);
                int shift = row & 7;
                int delta = 127;
                unsigned bright;

                for (col = 0; col < ctx->bm->width; col++) {
                    if (ctx->dither)
                        delta = DITHERXDY(col,dy);
                    bright = SC_OUT(*qp++, ctx);
                    bright = (3 * bright + (bright >> 6) + delta) >> 8;
                    *dest++ |= vi_pattern[bright] << shift;
                }
#endif /* LCD_PIXELFORMAT */
#elif LCD_DEPTH >= 16
                /* iriver h300, colour iPods, X5 */
                (void)fb_width;
                fb_data *dest = STRIDE_MAIN((fb_data *)ctx->bm->data + fb_width * row,
                                            (fb_data *)ctx->bm->data + row);
                int delta = 127;
                unsigned r, g, b;
                struct uint32_argb q0;
                /* setup alpha channel buffer */
                unsigned char *bm_alpha = NULL;
                if (ctx->bm->alpha_offset > 0)
                    bm_alpha = ctx->bm->data + ctx->bm->alpha_offset;
                if (bm_alpha)
                    bm_alpha += ALIGN_UP(ctx->bm->width, 2)*row/2;

                for (col = 0; col < ctx->bm->width; col++) {
                    (void) delta;
                    if (ctx->dither)
                        delta = DITHERXDY(col,dy);
                    q0 = *qp++;
                    r = SC_OUT(q0.r, ctx);
                    g = SC_OUT(q0.g, ctx);
                    b = SC_OUT(q0.b, ctx);
#if LCD_DEPTH < 24
                    r = (31 * r + (r >> 3) + delta) >> 8;
                    g = (63 * g + (g >> 2) + delta) >> 8;
                    b = (31 * b + (b >> 3) + delta) >> 8;
#endif
                    *dest = FB_RGBPACK_LCD(r, g, b);
                    dest += STRIDE_MAIN(1, ctx->bm->height);
                    if (bm_alpha) {
                        /* pack alpha channel for 2 pixels into 1 byte */
                        unsigned alpha = SC_OUT(q0.a, ctx);
                        if (col%2)
                            *bm_alpha++ |= alpha&0xf0;
                        else
                            *bm_alpha = alpha>>4;
                    }
                }
#endif /* LCD_DEPTH */
}
#endif

#if defined(PLUGIN) && LCD_DEPTH > 1
unsigned int get_size_native(struct bitmap *bm)
{
    return BM_SIZE(bm->width,bm->height,FORMAT_NATIVE,0);
}

const struct custom_format format_native = {
    .output_row_8 = output_row_8_native,
#if defined(HAVE_LCD_COLOR) && (defined(HAVE_JPEG) || defined(PLUGIN))
    .output_row_32 = {
        output_row_32_native,
        output_row_32_native_fromyuv
    },
#else
    .output_row_32 = output_row_32_native,
#endif
    .get_size = get_size_native
};
#endif

#if defined(HAVE_IPOD_UI) && !defined(PLUGIN) && !defined(JDEC_ALONE)
/* iPod UI shell: the album art builder (apps/ipodui/artcache.c) decodes at
 * background priority and must never boost (phase4b-brief.md, Revision 1,
 * decision 2). resize_on_load() skips its boost on that one thread and
 * counts the skips, which the builder traces. */
static unsigned int noboost_thread;
static bool noboost_on;
static unsigned long noboost_skips;
/* resize_boost_cap(): while 'noboost_on', that thread may still boost
 * one decode at a time for at most cap_ticks (0: never); boost_held says
 * whether its boost is still taken, boost_until when it is dropped. */
static long cap_ticks, boost_until;
static bool boost_held;

void resize_set_noboost_thread(unsigned int thread_id, bool on)
{
    noboost_thread = thread_id;
    noboost_on = on;
}

void resize_boost_cap(long ticks)
{
    cap_ticks = ticks;
}

/* Drops a decode's boost: at its end ('end'), or at a row once the art
 * thread's capped boost has run out (only that thread holds one; the
 * scheduler is cooperative, so the count stays balanced). */
static void resize_boost_drop(bool end)
{
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    if (end)
    {
        if (noboost_on && thread_self() == noboost_thread)
        {
            if (!boost_held)
                return;         /* the cap dropped it already */
            boost_held = false;
        }
    }
    else
    {
        if (!boost_held || !TIME_AFTER(current_tick, boost_until))
            return;
        boost_held = false;
    }
    cpu_boost(false);
#else
    (void)end;
#endif
}

static void (*pause_hook)(void);

void resize_set_pause_hook(void (*fn)(void))
{
    pause_hook = fn;
}

void resize_pause_point(void)
{
    if (pause_hook)
        pause_hook();
}

void resize_boost_release(void)
{
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    if (boost_held && noboost_on && thread_self() == noboost_thread)
    {
        boost_held = false;
        cpu_boost(false);
    }
#endif
}

unsigned long resize_noboost_skips(void)
{
    return noboost_skips;
}

static bool resize_may_boost(void)
{
    if (noboost_on && thread_self() == noboost_thread)
    {
        if (cap_ticks > 0)
        {
            boost_held = true;
            boost_until = current_tick + cap_ticks;
            return true;
        }
        noboost_skips++;
        return false;
    }
    return true;
}
#endif /* HAVE_IPOD_UI */

int resize_on_load(struct bitmap *bm, bool dither, struct dim *src,
                   struct rowset *rset, unsigned char *buf, unsigned int len,
                   const struct custom_format *format,
                   IF_PIX_FMT(int format_index,)
                   struct img_part* (*store_part)(void *args),
                   void *args)
{
    const int sw = src->width;
    const int sh = src->height;
    const int dw = bm->width;
    const int dh = bm->height;
    int ret;
    /* buffer for 1 line + 2 spare lines */
#ifdef HAVE_LCD_COLOR
    unsigned int needed = sizeof(struct uint32_argb) * 3 * bm->width;
#else
    unsigned int needed = sizeof(uint32_t) * 3 * bm->width;
#endif
#if MAX_SC_STACK_ALLOC
    uint8_t sc_buf[(needed <= len  || needed > MAX_SC_STACK_ALLOC) ?
                   0 : needed];
#endif
    ALIGN_BUFFER(buf, len, sizeof(uint32_t));
    if (needed > len)
    {
#if MAX_SC_STACK_ALLOC
        if (needed > MAX_SC_STACK_ALLOC)
        {
            DEBUGF("unable to allocate required buffer: %d needed, "
                   "%d available, %d permitted from stack\n",
                   needed, len, MAX_SC_STACK_ALLOC);
            return 0;
        }
        if (sizeof(sc_buf) < needed)
        {
            DEBUGF("failed to allocate large enough buffer on stack: "
                   "%d needed, only got %d",
                   needed, MAX_SC_STACK_ALLOC);
            return 0;
        }
#else
        DEBUGF("unable to allocate required buffer: %d needed, "
               "%d available\n", needed, len);
        return 0;
#endif
    }

    struct scaler_context ctx;
#if defined(HAVE_IPOD_UI) && !defined(PLUGIN) && !defined(JDEC_ALONE)
    const bool boost = resize_may_boost();
    (void)boost;                /* no CPU frequency control (simulator) */
#endif
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
#if defined(HAVE_IPOD_UI) && !defined(PLUGIN) && !defined(JDEC_ALONE)
    if (boost)
#endif
    cpu_boost(true);
#endif
    ctx.store_part = store_part;
    ctx.args = args;
#if MAX_SC_STACK_ALLOC
    ctx.buf = needed > len ? sc_buf : buf;
#else
    ctx.buf = buf;
#endif
    ctx.len = len;
    ctx.bm = bm;
    ctx.src = src;
    ctx.dither = dither;
#if !defined(PLUGIN)
#if defined(HAVE_LCD_COLOR) && defined(HAVE_JPEG)
    ctx.output_row = format_index ? output_row_32_native_fromyuv
                                  : output_row_32_native;
#else
    ctx.output_row = output_row_32_native;
#endif
    if (format)
#endif
#ifdef HAVE_LCD_COLOR
        ctx.output_row = format->output_row_32[format_index];
#else
        ctx.output_row = format->output_row_32;
#endif
#ifdef HAVE_UPSCALER
    if (sw > dw)
    {
#endif
        ctx.h_scaler = scale_h_area;
        uint32_t h_div = (1U << 24) / sw;
        ctx.h_i_val = sw * h_div;
        ctx.h_o_val = dw * h_div;
#ifdef HAVE_UPSCALER
    } else {
        ctx.h_scaler = scale_h_linear;
        uint32_t h_div = (1U << 24) / (dw - 1);
        ctx.h_i_val = (sw - 1) * h_div;
        ctx.h_o_val = (dw - 1) * h_div;
    }
#endif
#ifdef CPU_COLDFIRE
    unsigned old_macsr = coldfire_get_macsr();
    coldfire_set_macsr(EMAC_UNSIGNED);
#endif
#ifdef HAVE_UPSCALER
    if (sh > dh)
#endif
    {
        uint32_t v_div = (1U << 22) / sh;
        ctx.v_i_val = sh * v_div;
        ctx.v_o_val = dh * v_div;
        ret = scale_v_area(rset, &ctx);
    }
#ifdef HAVE_UPSCALER
    else
    {
        uint32_t v_div = (1U << 22) / dh;
        ctx.v_i_val = (sh - 1) * v_div;
        ctx.v_o_val = (dh - 1) * v_div;
        ret = scale_v_linear(rset, &ctx);
    }
#endif
#ifdef CPU_COLDFIRE
    /* Restore emac status; other modules like tone control filter
     * calculation may rely on it. */
    coldfire_set_macsr(old_macsr);
#endif
#if defined(HAVE_IPOD_UI) && !defined(PLUGIN) && !defined(JDEC_ALONE)
    if (boost)
        resize_boost_drop(true);
#elif defined(HAVE_ADJUSTABLE_CPU_FREQ)
    cpu_boost(false);
#endif
    if (!ret)
        return 0;
    return 1;
}

#if defined(JPEG_JDEC) && defined(HAVE_LCD_COLOR) && defined(HAVE_UPSCALER) \
    && LCD_STRIDEFORMAT == HORIZONTAL_STRIDE && !defined(PLUGIN)
/* jdec (bcmfw/jobs/jdec.c; bcmfw/jpeg/jhook.h): resize_on_load's scalers
 * fed one source row at a time, so one decoder pass feeds several
 * outputs.  The vertical scalers are scale_v_area and scale_v_linear with
 * their loops turned inside out, the same arithmetic in the same order;
 * the horizontal scalers and the output rows are resize_on_load's own.
 * An output of the rows' own size takes clip_jpeg_fd's unscaled path
 * instead (YUV to RGB per pixel, then output_row_8_native), as a single
 * decode at that size does. */
#include "jhook.h"

static struct img_part *push_part(void *args)
{
    struct resize_push *rp = args;

    if (rp->fed)
        return NULL;            /* one source row per scaler call */
    rp->fed = 1;
    return &rp->part;
}

/* each output row into row_px (row_bm points 'row' rows before it, so
   the output function's own row offset lands there), then to emit */
static void push_out(uint32_t row, void *row_in, struct scaler_context *ctx)
{
    struct resize_push *rp = (struct resize_push *)ctx;

    rp->row_bm.data = (unsigned char *)((uintptr_t)rp->row_px
                      - (uintptr_t)row * rp->bm.width * sizeof(fb_data));
    ctx->bm = &rp->row_bm;
    rp->out_row(row, row_in, ctx);
    ctx->bm = &rp->bm;
    rp->emit(rp->emit_arg, row, rp->row_px);
    rp->rows_out++;
}

bool resize_push_init(struct resize_push *rp, int dw, int dh, bool dither,
                      const struct dim *src, unsigned char *buf,
                      unsigned int len, int yuv,
                      void (*emit)(void *arg, int y, const fb_data *px),
                      void *emit_arg)
{
    struct scaler_context *ctx = &rp->ctx;
    const int sw = src->width, sh = src->height;
    unsigned int rows = sizeof(struct uint32_argb) * 3 * dw;

    memset(rp, 0, sizeof(*rp));
    if (len < RESIZE_PUSH_BUF(dw) || dw < 1 || dh < 1 || sw < 1 || sh < 1
        || ((uintptr_t)buf & 3) || (sw <= dw && dw < 2)
        || (sh <= dh && dh < 2))
        return false;
    rp->bm.width = rp->row_bm.width = dw;
    rp->bm.height = rp->row_bm.height = dh;
    rp->bm.format = rp->row_bm.format = FORMAT_NATIVE;
    rp->src = *src;
    rp->row_px = (fb_data *)(buf + rows);
    rp->out_row = yuv ? output_row_32_native_fromyuv : output_row_32_native;
    rp->emit = emit;
    rp->emit_arg = emit_arg;
    if (sw == dw && sh == dh)
    {
        rp->same = yuv ? 2 : 1;
        rp->out_row = output_row_8_native;
    }
    ctx->store_part = push_part;
    ctx->args = rp;
    ctx->buf = buf;
    ctx->len = rows;
    ctx->bm = &rp->bm;
    ctx->src = &rp->src;
    ctx->dither = dither;
    ctx->output_row = push_out;
    if (sw > dw)        /* resize_on_load's factors */
    {
        uint32_t h_div = (1U << 24) / sw;
        ctx->h_scaler = scale_h_area;
        ctx->h_i_val = sw * h_div;
        ctx->h_o_val = dw * h_div;
    } else {
        uint32_t h_div = (1U << 24) / (dw - 1);
        ctx->h_scaler = scale_h_linear;
        ctx->h_i_val = (sw - 1) * h_div;
        ctx->h_o_val = (dw - 1) * h_div;
    }
    if (sh > dh)
    {
        uint32_t v_div = (1U << 22) / sh;
        ctx->v_i_val = sh * v_div;
        ctx->v_o_val = dh * v_div;
        /* scale_v_area: the accumulator and temp rows zeroed */
        memset(buf, 0, dw * 2 * sizeof(uint32_t) * CHANNEL_BYTES);
    } else {
        uint32_t v_div = (1U << 22) / dh;
        ctx->v_i_val = (sh - 1) * v_div;
        ctx->v_o_val = (dh - 1) * v_div;
        rp->linear = 1;
        rp->oye = ctx->v_o_val;     /* scale_v_linear's iye */
    }
    return true;
}

/* scale_v_linear from where it waits for a row: on until the next output
   row wants one, or the last */
static void push_linear_on(struct resize_push *rp)
{
    struct scaler_context *ctx = &rp->ctx;
    const uint32_t v_i_val = ctx->v_i_val, v_o_val = ctx->v_o_val;
    uint32_t *rowinc = (uint32_t *)ctx->buf,
             *rowval = rowinc + rp->bm.width * CHANNEL_BYTES,
             *rowtmp = rowval + rp->bm.width * CHANNEL_BYTES,
             *rowinc_px, *rowval_px, *rowtmp_px;

    while (rp->oy != (uint32_t)rp->bm.height)
    {
        if (rp->oye >= v_o_val)
        {
            rp->oye -= v_o_val;
            rp->iy += 1;
            for(rowinc_px = rowinc, rowtmp_px = rowtmp, rowval_px = rowval;
                rowinc_px < rowval; rowinc_px++, rowtmp_px++, rowval_px++)
            {
                *rowinc_px = -*rowtmp_px;
                *rowval_px = *rowtmp_px * v_o_val;
            }
            if (rp->iy < (uint32_t)rp->src.height)
            {
                rp->waiting = 1;
                return;
            }
        } else
            for(rowinc_px = rowinc, rowval_px = rowval; rowinc_px < rowval;
                rowinc_px++, rowval_px++)
                *rowval_px += *rowinc_px;
        ctx->output_row(rp->oy, (void*)rowval, ctx);
        rp->oye += v_i_val;
        rp->oy++;
    }
}

bool resize_push_row(struct resize_push *rp, const struct img_part *row)
{
    struct scaler_context *ctx = &rp->ctx;
    const uint32_t v_i_val = ctx->v_i_val, v_o_val = ctx->v_o_val;
    uint32_t *row0 = (uint32_t *)ctx->buf,
             *row1 = row0 + rp->bm.width * CHANNEL_BYTES,
             *row2 = row1 + rp->bm.width * CHANNEL_BYTES,
             *a_px, *b_px;

    if (rp->rows_in >= (uint32_t)rp->src.height)
        return true;
    if (rp->same)
    {   /* clip_jpeg_fd's unscaled rows, on a copy (the primary's row is
           still to be used) */
        struct uint8_rgb *qp = (struct uint8_rgb *)row0, *end = qp + rp->bm.width;
        const struct uint8_rgb *in = row->buf;

        if (row->len < rp->bm.width)
            return false;
        if (rp->same == 2 && jdec_yuv565 && ctx->dither)
        {   /* push_out's work, the colour stage at once */
            jdec_yuv565(rp->row_px, in, rp->bm.width, DITHERY(rp->rows_in));
            rp->emit(rp->emit_arg, rp->rows_in++, rp->row_px);
            rp->rows_out++;
            return true;
        }
        memcpy(qp, in, sizeof(*qp) * rp->bm.width);
        if (rp->same == 2)
            for (; qp < end; qp++)
            {
                uint8_t y = qp->blue, u = qp->green, v = qp->red;
                unsigned r, g, b;
                yuv_to_rgb(y, u, v, &r, &g, &b);
                qp->red = r;
                qp->blue = b;
                qp->green = g;
            }
        ctx->output_row(rp->rows_in++, (void *)row0, ctx);
        return true;
    }
    rp->part = *row;
    rp->fed = 0;
    if (rp->linear)
    {   /* row0..2: rowinc, rowval, rowtmp */
        if (rp->rows_in++ == 0)
        {
            if (!ctx->h_scaler((void*)row2, ctx, false))
                return false;
        }
        else if (rp->waiting)
        {
            rp->waiting = 0;
            if (!ctx->h_scaler((void*)row2, ctx, false))
                return false;
            for(a_px = row0, b_px = row2; a_px < row1; a_px++, b_px++)
            {
                uint32_t *val_px = a_px + (row1 - row0);
                *a_px += *b_px;
                *val_px += *a_px * rp->oye;
                *a_px *= v_i_val;
            }
            ctx->output_row(rp->oy, (void*)row1, ctx);
            rp->oye += v_i_val;
            rp->oy++;
        }
        else
            return true;        /* past the last row it reads */
        push_linear_on(rp);
        return true;
    }
    /* scale_v_area's loop body; row0, row1: rowacc, rowtmp */
    rp->rows_in++;
    rp->oye += v_o_val;
    if (rp->oye >= v_i_val)
    {
        rp->oye -= v_i_val;
        for(a_px = row0, b_px = row1; a_px != row1; a_px++, b_px++)
            *a_px = *a_px * v_o_val + *b_px * rp->mul;
        if (!ctx->h_scaler(row1, ctx, false))
            return false;
        rp->mul = v_o_val - rp->oye;
        for(a_px = row0, b_px = row1; a_px != row1; a_px++, b_px++)
            *a_px += rp->mul * *b_px;
        ctx->output_row(rp->oy, (void*)row0, ctx);
        memset((void *)row0, 0, rp->bm.width * sizeof(uint32_t) * CHANNEL_BYTES);
        rp->mul = rp->oye;
        rp->oy += 1;
    } else {
        if (!ctx->h_scaler(row0, ctx, true))
            return false;
    }
    return true;
}

bool resize_push_end(struct resize_push *rp)
{
    return !rp->waiting && rp->rows_out == (uint32_t)rp->bm.height;
}
#endif /* JPEG_JDEC */
