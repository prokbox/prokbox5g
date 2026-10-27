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
 * Host harness for bcmfw/h264: decode an Annex B stream and compare every
 * frame with ffmpeg's raw yuv420p decode (bit-exact), or fuzz the decoder
 * with mutated streams (no crash, no hang).
 *
 *   h264_test [-v] [-o out.yuv] [-n frames] clip.264 ref.yuv
 *   h264_test --fuzz N SEED clip.264 [clip.264 ...]
 *
 * Exit status 0 when every frame matches (or every fuzz case survived).
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "h264.h"

#ifdef H264_DEBUG
#include "h264_int.h"
extern int h264_debug_no_deblock;
extern void (*h264_debug_trace)(const struct h264_dec *, const struct h264_mb *, int);
static int trace_pic = -1, trace_mb = -1;
static long hist[8], hist_refs;
static int do_hist;
static void trace(const struct h264_dec *d, const struct h264_mb *m, int i16)
{
    int k;
    hist[m->mb_type]++;
    if (!(m->flags & MBF_INTRA))
        for (k = 0; k < 16; k++)
            if (m->ref[C(k & 3, k >> 2)] > 0)
            {
                hist_refs++;
                break;
            }
    if ((int)d->stats.pictures != trace_pic || (trace_mb >= 0 && m->addr != trace_mb))
        return;
    printf("pic %d mb %d (%d,%d) type %d flags %x cbp %x qp %d chroma %d i16 %d iav %x\n",
           (int)d->stats.pictures, m->addr, m->x, m->y, m->mb_type, m->flags, m->cbp,
           m->qp, m->chroma_mode, i16, m->iavail);
    if (m->flags & MBF_I4x4) {
        printf("  ipm:");
        for (k = 0; k < 16; k++) printf(" %d", m->ipm[C(k & 3, k >> 2)]);
        printf("\n");
    }
    printf("  coded %08x dconly %08x\n", m->coded, m->dconly);
}
#endif

static uint8_t *load(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    if (!f)
    {
        perror(path);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc(*len + 1);
    if (!buf || fread(buf, 1, *len, f) != (size_t)*len)
    {
        fprintf(stderr, "%s: read error\n", path);
        exit(2);
    }
    fclose(f);
    return buf;
}

/* Arena for the largest stream we accept (640x480, 16 references). */
#define ARENA_SIZE (h264_arena_size(H264_MAX_MB_W, H264_MAX_MB_H, \
                                    H264_MAX_REF_FRAMES))

struct cmp {
    const uint8_t *ref;
    long ref_len, pos;
    int frames, bad, verbose;
    FILE *out;
};

/* Compare one picture with the next frame of the reference. */
static void check_frame(struct cmp *c, const struct h264_picture *p)
{
    int w = p->width, h = p->height, plane, x, y;
    long fsize = (long)w * h * 3 / 2;
    const uint8_t *r = c->ref + c->pos;
    int n = c->frames++;

    if (c->out)
    {
        for (y = 0; y < h; y++)
            fwrite(p->y + y * p->stride_y, 1, w, c->out);
        for (y = 0; y < h / 2; y++)
            fwrite(p->u + y * p->stride_c, 1, w / 2, c->out);
        for (y = 0; y < h / 2; y++)
            fwrite(p->v + y * p->stride_c, 1, w / 2, c->out);
    }
    if (!c->ref)
        return;
    if (c->pos + fsize > c->ref_len)
    {
        if (!c->bad++)
            printf("  frame %d: beyond the reference (%ld frames)\n", n,
                   c->ref_len / fsize);
        return;
    }
    c->pos += fsize;
    for (plane = 0; plane < 3; plane++)
    {
        int pw = plane ? w / 2 : w, ph = plane ? h / 2 : h;
        int stride = plane ? p->stride_c : p->stride_y;
        const uint8_t *src = plane == 0 ? p->y : plane == 1 ? p->u : p->v;
        const uint8_t *rp = r + (plane == 0 ? 0 : plane == 1 ? (long)w * h
                                              : (long)w * h * 5 / 4);
        for (y = 0; y < ph; y++)
            for (x = 0; x < pw; x++)
                if (src[y * stride + x] != rp[y * pw + x])
                {
                    int mbx = plane ? x / 8 : x / 16, mby = plane ? y / 8 : y / 16;
                    if (!c->bad++ || c->verbose)
                        printf("  frame %d (frame_num %d%s): first mismatch "
                               "plane %c at (%d,%d), MB (%d,%d) = addr %d: "
                               "got %d, want %d\n", n, p->frame_num,
                               p->idr ? ", IDR" : "", "YUV"[plane], x, y,
                               mbx, mby, mby * ((w + 15) / 16) + mbx,
                               src[y * stride + x], rp[y * pw + x]);
                    return;
                }
    }
    if (c->verbose)
        printf("  frame %d: ok\n", n);
}

/* --split: context in one region, frames in another (bcmfw's layout) */
static int split_frames;

static int decode_stream(uint8_t *buf, long len, struct cmp *c, int max_frames,
                         int quiet)
{
    static uint8_t *arena, *frames_raw, *frames;
    struct h264_dec *d;
    int pos = 0, nal, nal_len, next, r, errors = 0;
    size_t fsize = h264_frames_size(H264_MAX_MB_W, H264_MAX_MB_H,
                                    H264_MAX_REF_FRAMES);

    if (!arena)
        arena = malloc(ARENA_SIZE);
    if (split_frames && !frames)
    {
        frames_raw = malloc(fsize + 32);
        frames = frames_raw + ((32 - ((size_t)frames_raw & 31)) & 31);
    }
    if (split_frames)
    {
        d = h264_open(arena, h264_ctx_size(H264_MAX_MB_W, H264_MAX_MB_H));
        if (!d || h264_set_frames(d, frames, fsize))
            return -1;
    }
    else
        d = h264_open(arena, ARENA_SIZE);
    if (!d)
        return -1;
    while (h264_annexb_next(buf, (int)len, pos, &nal, &nal_len, &next))
    {
        pos = next;
        if (nal_len <= 0)
            continue;
        do
        {
            r = h264_decode_nal(d, buf + nal, nal_len);
            if (r == H264_FRAME || r == H264_FRAME_AGAIN)
            {
                if (c)
                    check_frame(c, h264_frame(d));
                if (max_frames && c && c->frames >= max_frames)
                    return errors;
            }
            else if (r < 0)
            {
                errors++;
                if (!quiet)
                    printf("  NAL at %d (type %d): error %d\n", nal,
                           buf[nal] & 31, r);
            }
        } while (r == H264_FRAME_AGAIN);
    }
    if (h264_flush(d) == H264_FRAME && c)
        check_frame(c, h264_frame(d));
    return errors;
}

static uint32_t rng;
static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static int fuzz(int n, uint32_t seed, char **clips, int nclips)
{
    long lens[16];
    uint8_t *bufs[16];
    int k, i, worst_ms = 0;
    double total = 0;

    if (nclips > 16)
        nclips = 16;
    for (k = 0; k < nclips; k++)
        bufs[k] = load(clips[k], &lens[k]);
    rng = seed ? seed : 2722;
    for (i = 0; i < n; i++)
    {
        int c = rnd() % nclips, m, edits;
        long len = lens[c];
        uint8_t *b;
        clock_t t0;
        int ms;

        /* a random prefix keeps cases fast; mutate a copy */
        if (len > 4096 && (rnd() & 1))
            len = 4096 + rnd() % (len - 4096);
        b = malloc(len + 64);
        memcpy(b, bufs[c], len);
        edits = 1 + rnd() % 8;
        for (m = 0; m < edits; m++)
        {
            long at = rnd() % len;
            switch (rnd() % 6)
            {
            case 0: b[at] ^= 1 << (rnd() & 7); break;          /* bit flip */
            case 1: b[at] = rnd(); break;                       /* byte */
            case 2: b[at] = 0; break;
            case 3: b[at] = 0xff; break;
            case 4:                                             /* run */
            {
                long n2 = 1 + rnd() % 32, j;
                for (j = 0; j < n2 && at + j < len; j++)
                    b[at + j] = rnd();
                break;
            }
            case 5:                                             /* cut */
                if (at > 16)
                    len = at;
                break;
            }
        }
        t0 = clock();
        decode_stream(b, len, NULL, 0, 1);
        ms = (int)((clock() - t0) * 1000 / CLOCKS_PER_SEC);
        total += ms;
        if (ms > worst_ms)
            worst_ms = ms;
        if (ms > 5000)
        {
            printf("fuzz case %d (clip %s): %d ms: HANG\n", i, clips[c], ms);
            return 1;
        }
        free(b);
    }
    printf("fuzz: %d cases, seed %u, no crash, no hang (worst %d ms, "
           "mean %.1f ms)\n", n, seed, worst_ms, total / n);
    return 0;
}

int main(int argc, char **argv)
{
    struct cmp c;
    long len;
    uint8_t *buf;
    int a = 1, errors, max_frames = 0;
    const char *outp = NULL;

    if (argc > 4 && !strcmp(argv[1], "--fuzz"))
        return fuzz(atoi(argv[2]), (uint32_t)strtoul(argv[3], NULL, 0),
                    argv + 4, argc - 4);
    memset(&c, 0, sizeof(c));
    for (; a < argc && argv[a][0] == '-'; a++)
    {
        if (!strcmp(argv[a], "-v"))
            c.verbose = 1;
        else if (!strcmp(argv[a], "--split"))
            split_frames = 1;
        else if (!strcmp(argv[a], "-o") && a + 1 < argc)
            outp = argv[++a];
        else if (!strcmp(argv[a], "-n") && a + 1 < argc)
            max_frames = atoi(argv[++a]);
#ifdef H264_DEBUG
        else if (!strcmp(argv[a], "--no-deblock"))
            h264_debug_no_deblock = 1;
        else if (!strcmp(argv[a], "--trace") && a + 2 < argc)
        {
            trace_pic = atoi(argv[++a]);
            trace_mb = atoi(argv[++a]);
            h264_debug_trace = trace;
        }
        else if (!strcmp(argv[a], "--hist"))
        {
            do_hist = 1;
            h264_debug_trace = trace;
        }
#endif
    }
    if (argc - a < 1)
    {
        fprintf(stderr, "usage: %s [-v] [-o out.yuv] [-n frames] clip.264 "
                "[ref.yuv]\n       %s --fuzz N SEED clip.264...\n",
                argv[0], argv[0]);
        return 2;
    }
    buf = load(argv[a], &len);
    if (argc - a > 1)
        c.ref = load(argv[a + 1], &c.ref_len);
    if (outp)
        c.out = fopen(outp, "wb");
    errors = decode_stream(buf, len, &c, max_frames, 0);
    if (c.out)
        fclose(c.out);
#ifdef H264_DEBUG
    if (do_hist)
        printf("  mb types: P16x16 %ld P16x8 %ld P8x16 %ld P8x8 %ld P8x8ref0 %ld "
               "I4x4 %ld I16 %ld PCM %ld; with ref_idx > 0: %ld\n",
               hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], hist[6],
               hist[7], hist_refs);
#endif
    if (c.ref && !max_frames && c.pos != c.ref_len && !c.bad)
    {
        printf("  decoded %d frames, the reference has more\n", c.frames);
        c.bad++;
    }
    printf("%s: %d frames, %d mismatched, %d errors: %s\n", argv[a], c.frames,
           c.bad, errors, (c.bad || errors || !c.frames) ? "FAIL" : "bit-exact");
    return (c.bad || errors || !c.frames) ? 1 : 0;
}
