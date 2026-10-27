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

/* The art builder's JPEG decode, one definition for the CPU path and the
 * BCM (jdec.h; jpeg-spike.md section 8).  Over the shell's own decoder
 * and resizer, unchanged, in their file mode (clip_jpeg_fd, as artcache.c
 * calls them): the decoder's read() and lseek() come here (jenv.h maps
 * them) and go to the caller's source. */
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "jdec.h"
#include "jenv.h"
#include "jpeg_load.h"
#include "jhook.h"
#include "jscan.h"

#define JDEC_FD     3

void (*jdec_yield)(void);

/* the source the decoder reads now (one decode at a time), where it is,
   and what has passed (jscan.h) */
static const struct jdec_src *cur;
static uint32_t pos;
static int io_err;
static struct jscan sc;

int jdec_io_open(const char *path, int flags, ...)
{
    (void)path;
    (void)flags;
    return -1;
}

int jdec_io_close(int fd)
{
    return fd == JDEC_FD ? 0 : -1;
}

long jdec_io_read(int fd, void *buf, unsigned long n)
{
    long got;

    if (fd != JDEC_FD || !cur)
        return -1;
    if (n > cur->len - pos)
        n = cur->len - pos;
    got = n ? cur->fetch(cur->ctx, buf, n) : 0;
    if (got < 0)
    {
        io_err = 1;
        return -1;
    }
    jscan_bytes(&sc, buf, got);
    pos += got;
    return got;
}

long jdec_io_lseek(int fd, long off, int whence)
{
    if (fd != JDEC_FD || !cur)
        return -1;
    if (whence == SEEK_CUR && off >= 0 && (uint32_t)off <= cur->len - pos)
    {
        if (off && cur->skip(cur->ctx, off) < 0)
        {
            io_err = 1;
            return -1;
        }
        sc.ff = 0;              /* a skip never crosses a marker pair */
        pos += off;
        return pos;
    }
    return -1;                  /* the decoder never seeks back */
}

long jdec_io_size(int fd)
{
    return fd == JDEC_FD && cur ? (long)cur->len : -1;
}

static int status_of(int rc)
{
    if (rc > 0)
        return JDEC_OK;
    if (rc == -3 || rc == -4)       /* odd subsampling; not baseline nor
                                       progressive, or past JPEG_PROG_MAX */
        return JDEC_E_UNSUPPORTED;
    if (rc == JPEG_E_PROGMEM || rc == JPEG_E_NOMEM) /* the work area: a
                                       progressive file's store, the rows */
        return JDEC_E_NOMEM;
    return JDEC_E_CORRUPT;          /* JPEG_E_BADHUFF among them */
}

/* One pass of the decoder over the source into bm: its status */
static int pass(const struct jdec_src *src, struct bitmap *bm, void *work,
                uint32_t work_bytes, int format)
{
    int rc;

    if (src->start(src->ctx) < 0)
        return JDEC_E_CORRUPT;
    cur = src;
    pos = 0;
    io_err = sc.after_sos = sc.eoi = sc.ff = 0;
    bm->data = work;
    rc = clip_jpeg_fd(JDEC_FD, 0, src->len, bm, (int)work_bytes, format, NULL);
    cur = NULL;
    if (io_err)
        return JDEC_E_CORRUPT;
    return status_of(rc);
}

/* After a whole decode: the rest of the stream, up to its EOI (the
   decoder may stop just before it) */
static int ended(const struct jdec_src *src)
{
    uint8_t tail[64];

    cur = src;
    while (!sc.eoi && pos < src->len && !io_err)
        jdec_io_read(JDEC_FD, tail, sizeof(tail));
    cur = NULL;
    return sc.eoi ? JDEC_OK : JDEC_E_TRUNCATED;
}

static int mem_start(void *ctx)
{
    ((struct jdec_mem *)ctx)->pos = 0;
    return 0;
}

static long mem_read(void *ctx, void *buf, uint32_t n)
{
    struct jdec_mem *m = ctx;

    if (n > m->len - m->pos)
        n = m->len - m->pos;
    memcpy(buf, m->p + m->pos, n);
    m->pos += n;
    return n;
}

static int mem_skip(void *ctx, uint32_t n)
{
    struct jdec_mem *m = ctx;

    if (n > m->len - m->pos)
        return -1;
    m->pos += n;
    return 0;
}

void jdec_mem_src(struct jdec_src *src, struct jdec_mem *m, const uint8_t *p,
                  uint32_t len)
{
    m->p = p;
    m->len = len;
    m->pos = 0;
    src->len = len;
    src->start = mem_start;
    src->fetch = mem_read;
    src->skip = mem_skip;
    src->ctx = m;
}

int jdec_probe(const struct jdec_src *src, void *work, uint32_t work_bytes,
               struct jdec_info *info)
{
    struct bitmap bm;
    int e;

    memset(&bm, 0, sizeof(bm));
    e = pass(src, &bm, work, work_bytes, FORMAT_NATIVE | FORMAT_RETURN_SIZE);
    if (e)
        return e;
    if (bm.width <= 0 || bm.height <= 0 || bm.width > 0xFFFF || bm.height > 0xFFFF)
        return JDEC_E_CORRUPT;
    if (info)
    {
        info->sw = bm.width;
        info->sh = bm.height;
    }
    return JDEC_OK;
}

/* Decode-once (jdec.h), streamed: every output's resizer, fed by the
   decoder's rows tap (rows only: the decoder makes no bitmap of its own),
   each output row written into its destination as it is made.  Each
   output's state and rows live at the end of the work area
   (JDEC_WORK_OUT). */
struct fan_out
{
    struct resize_push rp;
    const struct jdec_out *o;
};
struct fan
{
    struct fan_out *f[JDEC_MAX_OUT];
    int n, bad;
};

#ifndef __vc4__
const struct jpeg_rows_tap *jdec_tap_also;  /* the Mac's oracle only */
#define TAP_ALSO(f, ...) (jdec_tap_also ? jdec_tap_also->f(jdec_tap_also->arg, __VA_ARGS__) : (void)0)
#else
#define TAP_ALSO(f, ...) ((void)0)
#endif

/* the row sink: the window's part of output row y, to the destination's
   row y - cy at its pitch */
static void fan_emit(void *arg, int y, const fb_data *px)
{
    const struct jdec_out *o = arg;

    if (y >= o->cy && y < o->cy + o->ch)
        memcpy(o->px + (uint32_t)(y - o->cy) * o->pitch, px + o->cx,
               2u * o->cw);
}

static void fan_begin(void *arg, const struct dim *rows, int yuv)
{
    struct fan *fan = arg;
    int k;

    for (k = 0; k < fan->n; k++)
    {
        struct fan_out *f = fan->f[k];

        if (!resize_push_init(&f->rp, f->o->dw, f->o->dh, true, rows,
                              (unsigned char *)(f + 1),
                              RESIZE_PUSH_BUF(f->o->dw), yuv, fan_emit,
                              (void *)f->o))
            fan->bad = JDEC_E_RANGE;
    }
    TAP_ALSO(begin, rows, yuv);
}

static void fan_row(void *arg, const struct img_part *row, int y)
{
    struct fan *fan = arg;
    int k;

    (void)y;
    for (k = 0; k < fan->n && !fan->bad; k++)
        if (!resize_push_row(&fan->f[k]->rp, row))
            fan->bad = JDEC_E_CORRUPT;
    TAP_ALSO(row, row, y);
}

/* the output decoded at its own IDCT scale: the largest by area, the
   first of equals */
int jdec_primary(const struct jdec_out *out, int n)
{
    int k, p = 0;

    for (k = 1; k < n; k++)
        if ((uint32_t)out[k].dw * out[k].dh > (uint32_t)out[p].dw * out[p].dh)
            p = k;
    return p;
}

uint32_t jdec_work(const struct jdec_out *out, int n)
{
    int k;
    uint32_t w = JDEC_WORK_BASE + JDEC_WORK_ROWS(out[jdec_primary(out, n)].dw);

    for (k = 0; k < n; k++)
        w += JDEC_WORK_OUT(out[k].dw);
    return w;
}

int jdec_run(const struct jdec_src *src, const struct jdec_out *out, int n,
             void *work, uint32_t work_bytes, struct jdec_info *info)
{
    struct jdec_info si;
    struct fan fan;
    struct jpeg_rows_tap tap = { fan_begin, fan_row, &fan, 1 };
    const struct jdec_out *o;
    struct bitmap bm;
    uint32_t dec_bytes, at;
    int k, e;

    _Static_assert(sizeof(struct fan_out) + 8 <= JDEC_WORK_OUT(0),
                   "JDEC_WORK_OUT's fixed part");
    _Static_assert(JDEC_PROG_MAX == JPEG_PROG_MAX, "one progressive cap");
    _Static_assert(JDEC_WORK_BASE == JPEG_JDEC_STATE, "one state bound");
    if (n < 1 || n > JDEC_MAX_OUT)
        return JDEC_E_RANGE;
    for (k = 0; k < n; k++)
    {
        o = &out[k];
        if (!o->dw || !o->dh || !o->cw || !o->ch || !o->px ||
            o->cx + o->cw > o->dw || o->cy + o->ch > o->dh || o->pitch < o->cw)
            return JDEC_E_RANGE;
    }
    if (jdec_work(out, n) > work_bytes)
        return JDEC_E_NOMEM;
    /* every output's resizer at the end of the work area, 8-aligned; the
       decoder's state and rows from its start */
    fan.n = fan.bad = 0;
    at = work_bytes & ~7u;
    for (k = 0; k < n; k++)
    {
        at -= (sizeof(struct fan_out) + RESIZE_PUSH_BUF(out[k].dw) + 7) & ~7u;
        fan.f[fan.n] = (struct fan_out *)((uint8_t *)work + at);
        fan.f[fan.n++]->o = &out[k];
    }
    dec_bytes = at;
    if ((e = jdec_probe(src, work, dec_bytes, &si)))
        return e;
    if (info)
        *info = si;
    /* the primary's size picks the IDCT scale (decode-once, jdec.h) */
    o = &out[jdec_primary(out, n)];
    memset(&bm, 0, sizeof(bm));
    bm.width = o->dw;
    bm.height = o->dh;
    jpeg_set_rows_tap(&tap);
    e = pass(src, &bm, work, dec_bytes,
             FORMAT_NATIVE | FORMAT_DITHER | FORMAT_RESIZE);
    jpeg_set_rows_tap(NULL);
    if (!e)
        e = ended(src);
    if (e)
        return e;
    if (bm.width != o->dw || bm.height != o->dh)
        return JDEC_E_CORRUPT;
    if (fan.bad)
        return fan.bad;
    for (k = 0; k < fan.n; k++)
        if (!resize_push_end(&fan.f[k]->rp))
            return JDEC_E_CORRUPT;
    return JDEC_OK;
}
