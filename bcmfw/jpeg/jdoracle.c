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

/* The decode-once definition (jdec.h) computed the plain way, as the
 * test's oracle for jdec_run (Mac only; bcmfw/tests/jpeg_test.py): the
 * primary output alone through jdec_run, with every row of the picture at
 * its IDCT scale kept whole in memory (jdec_tap_also); then every output,
 * the primary too, by the shell's unchanged resize_on_load, pulling those
 * rows, or, at the rows' own size, by a single decode at that size (which
 * takes the same IDCT scale and clip_jpeg_fd's unscaled path; for the
 * primary, the one just made, which jpeg_test.py holds to golden.txt).
 * jdec_run's streamed resizers (resize_push) must give the same pixels.
 * Outputs as jdone.c's jdec_many. */
#include <stdlib.h>
#include "jenv.h"
#include "jdec.h"
#include "jhook.h"
#include "jpeg_load.h"

struct keep
{
    struct uint8_rgb *rows;
    struct dim d;
    int yuv, y, bad;
    struct img_part part;
};

static void keep_begin(void *arg, const struct dim *rows, int yuv)
{
    struct keep *k = arg;

    k->d = *rows;
    k->yuv = yuv;
    k->rows = malloc(sizeof(struct uint8_rgb) * rows->width * rows->height);
    k->bad |= !k->rows;
}

static void keep_row(void *arg, const struct img_part *row, int y)
{
    struct keep *k = arg;

    if (k->rows && y < k->d.height && row->len >= k->d.width)
        memcpy(k->rows + (size_t)y * k->d.width, row->buf,
               sizeof(struct uint8_rgb) * k->d.width);
    else
        k->bad = 1;
}

static struct img_part *serve(void *args)
{
    struct keep *k = args;

    if (k->y >= k->d.height)
        return NULL;
    k->part.buf = k->rows + (size_t)k->y++ * k->d.width;
    k->part.len = k->d.width;
    return &k->part;
}

int jdec_oracle(const uint8_t *jpg, uint32_t len, uint16_t *out,
                const uint32_t *wh, int n, void *work, uint32_t work_bytes)
{
    struct keep k = { 0 };
    struct jpeg_rows_tap tap = { keep_begin, keep_row, &k };
    struct jdec_src src;
    struct jdec_mem m;
    struct jdec_out o[JDEC_MAX_OUT];
    int i, p, e;

    if (n < 1 || n > JDEC_MAX_OUT)
        return JDEC_E_RANGE;
    for (i = 0; i < n; i++)
    {
        o[i].dw = o[i].cw = wh[i] & 0xFFFF;
        o[i].dh = o[i].ch = wh[i] >> 16;
        o[i].cx = o[i].cy = 0;
        o[i].px = out;
        o[i].pitch = o[i].dw;
        out += (uint32_t)o[i].dw * o[i].dh;
    }
    p = jdec_primary(o, n);
    jdec_mem_src(&src, &m, jpg, len);
    jdec_tap_also = &tap;
    e = jdec_run(&src, &o[p], 1, work, work_bytes, 0);
    jdec_tap_also = NULL;
    if (!e && k.bad)
        e = JDEC_E_NOMEM;
    for (i = 0; i < n && !e; i++)
    {
        struct bitmap bm = { 0 };
        struct rowset rset;

        if (o[i].dw == k.d.width && o[i].dh == k.d.height)
        {
            if (i != p)
                e = jdec_run(&src, &o[i], 1, work, work_bytes, 0);
            continue;
        }
        bm.width = o[i].dw;
        bm.height = o[i].dh;
        bm.data = (unsigned char *)o[i].px;
        rset.rowstart = 0;
        rset.rowstop = o[i].dh;
        rset.rowstep = 1;
        k.y = 0;
        if (!resize_on_load(&bm, true, &k.d, &rset, work, work_bytes, NULL,
                            k.yuv, serve, &k))
            e = JDEC_E_CORRUPT;
    }
    free(k.rows);
    return e;
}
