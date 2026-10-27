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

/* S2/S3 on the Mac: the art builder's decode (bcmfw/jobs/jdec.c), alone
 * or as the BCM's J_OP_DECODE through the executor (bcmfw/jobs/jref.c),
 * printing what host/jpegref prints so jpeg_test.py compares both with
 * the golden tables.  One output: rev 1's single decode; several: one
 * decode-once call for all of them (a line each, in order).  --oracle:
 * the decode-once definition computed the plain way (jdoracle.c).
 * --tight: as jdec, in a work area of exactly jdec_work() bytes, and on
 * stderr "spare N of W", the longest run of it the decode left untouched
 * (its margin; jpeg-spike.md section 15).
 * Errors print "0 0 <code> 0" for each output: a JDEC_E_* code, or with
 * --jref the job's status (J_E_DEC_*).
 * JDTEST_DUMP=PATH (jdec, --tight, --oracle): the outputs' pixels too,
 * packed one after another, RGB565 little-endian (decode_image_test.py's
 * expected windows).
 * Usage: jdtest [--jref|--oracle|--tight] FILE.jpg W H [W H ...] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hostproto.h"
#include "jref.h"
#include "jdec.h"

#define WORK        (8u << 20)

int jdec_many(const uint8_t *jpg, uint32_t len, uint16_t *out,
              const uint32_t *wh, int n, void *work, uint32_t work_bytes);
int jdec_oracle(const uint8_t *jpg, uint32_t len, uint16_t *out,
                const uint32_t *wh, int n, void *work, uint32_t work_bytes);

static void *work;

/* jref's DECODE hook: jdec over the source slot, from memory */
static int decode(const uint8_t *jpg, uint32_t len, const struct jdec_out *out,
                  int n, struct jdec_info *info)
{
    struct jdec_src src;
    struct jdec_mem m;

    jdec_mem_src(&src, &m, jpg, len);
    return jdec_run(&src, out, n, work, WORK, info);
}

static uint32_t call(struct jref *j, uint32_t op, const uint32_t *a, uint32_t n,
                     uint32_t *rep)
{
    uint32_t rec[HP_MAX_PAYLOAD + 1], nr;

    rec[0] = HP_REC(op, n, J_TAG_REPLY);
    memcpy(rec + 1, a, 4 * n);
    return jref_exec(j, rec, rep, &nr);
}

static int via_jref(const uint8_t *jpg, uint32_t len, const uint32_t *wh,
                    int n, uint32_t *fnv)
{
    static uint16_t fb[2][320 * 240];
    static struct jref j;
    struct jref_surf front = { fb[0], 320, 240, 320 }, back = { fb[1], 320, 240, 320 };
    uint32_t a[J_DEC_ARGS(J_DEC_MAX)], rep[HP_MAX_PAYLOAD + 1], src, dst[J_DEC_MAX], st;
    uint32_t rows = (len + 2047) / 2048;
    int k;

    jref_init(&j, front, back, front);
    jref_pool(&j, J_CLS_PANEL, malloc(4u << 20), 4u << 20);
    jref_pool(&j, J_CLS_DRIFT, malloc(1u << 20), 1u << 20);
    j.decode = decode;
    a[0] = J_CLS_PANEL; a[1] = 1024 | rows << 16; a[2] = J_FMT_RGB565; a[3] = 1; a[4] = 0;
    if ((st = call(&j, J_OP_ALLOC, a, 5, rep)) != HP_OK)
        return -100 - (int)st;
    src = rep[1];
    memcpy(jref_slot_px(&j, src), jpg, len);
    a[0] = src; a[1] = a[2] = 0; a[3] = J_COMMIT_NOCHECK;    /* the file's bytes */
    if ((st = call(&j, J_OP_COMMIT, a, 4, rep)) != HP_OK)
        return -100 - (int)st;
    for (k = 0; k < n; k++)
    {
        a[0] = J_CLS_DRIFT; a[1] = wh[k]; a[2] = J_FMT_RGB565; a[3] = 2; a[4] = 0;
        if ((st = call(&j, J_OP_ALLOC, a, 5, rep)) != HP_OK)
            return -100 - (int)st;
        dst[k] = rep[1];
    }
    a[0] = src; a[1] = len; a[2] = n;
    for (k = 0; k < n; k++)
    {
        uint32_t *o = a + 3 + 5 * k;

        o[0] = wh[k]; o[1] = 0; o[2] = wh[k]; o[3] = dst[k]; o[4] = 0;
    }
    if ((st = call(&j, J_OP_DECODE, a, J_DEC_ARGS(n), rep)) != HP_OK)
        return (int)st;
    for (k = 0; k < n; k++)
    {
        const struct jref_surf *o = &j.slot[dst[k]].surf;
        uint32_t x, y;

        a[0] = dst[k]; a[1] = a[2] = 0; a[3] = J_COMMIT_NOCHECK;
        if ((st = call(&j, J_OP_COMMIT, a, 4, rep)) != HP_OK)
            return -100 - (int)st;
        fnv[k] = 2166136261u;           /* FNV-1a of the w x h pixels */
        for (y = 0; y < o->h; y++)
            for (x = 0; x < o->w; x++)
            {
                uint16_t v = o->px[y * o->pitch + x];

                fnv[k] = (fnv[k] ^ (v & 0xFF)) * 16777619u;
                fnv[k] = (fnv[k] ^ (v >> 8)) * 16777619u;
            }
    }
    return 0;
}

int main(int argc, char **argv)
{
    int jr = argc > 1 && !strcmp(argv[1], "--jref");
    int orc = argc > 1 && !strcmp(argv[1], "--oracle");
    int tight = argc > 1 && !strcmp(argv[1], "--tight");
    int a0 = 1 + (jr || orc || tight), n = (argc - a0 - 1) / 2, rc, k;
    uint8_t *jpg;
    uint16_t *out, *px;
    uint32_t wh[JDEC_MAX_OUT], fnv[JDEC_MAX_OUT], i, total = 0;
    long len;
    FILE *f;

    if (n < 1 || n > JDEC_MAX_OUT || argc != a0 + 1 + 2 * n
        || !(f = fopen(argv[a0], "rb")))
        return 2;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    jpg = malloc(len + 4);
    work = malloc(WORK);    /* no guard before it: the restart bound (p19-jpeg-oob) keeps
                               the bit-serial decoder inside its state */
    for (k = 0; k < n; k++)
    {
        wh[k] = atoi(argv[a0 + 1 + 2 * k]) | atoi(argv[a0 + 2 + 2 * k]) << 16;
        total += (wh[k] & 0xFFFF) * (wh[k] >> 16);
    }
    out = malloc(2 * total + 2);
    if (!jpg || !work || !out || fread(jpg, 1, len, f) != (size_t)len)
        return 2;
    fclose(f);
    if (jr)
        rc = via_jref(jpg, len, wh, n, fnv);
    else
    {
        uint32_t wb = WORK;

        if (tight)
        {   /* jdec_many's outputs, for jdec_work() */
            struct jdec_out o[JDEC_MAX_OUT];

            for (k = 0; k < n; k++)
            {
                o[k].dw = wh[k] & 0xFFFF;
                o[k].dh = wh[k] >> 16;
            }
            wb = jdec_work(o, n);
            memset(work, 0xA5, wb);
        }
        rc = (orc ? jdec_oracle : jdec_many)(jpg, len, out, wh, n, work, wb);
        if (tight)
        {
            const uint8_t *w = work;
            uint32_t run = 0, best = 0;

            for (i = 0; i < wb; i++)
                if (w[i] == 0xA5)
                    best = ++run > best ? run : best;
                else
                    run = 0;
            fprintf(stderr, "spare %u of %u\n", best, wb);
        }
        if (!rc && getenv("JDTEST_DUMP") && (f = fopen(getenv("JDTEST_DUMP"), "wb")))
        {
            fwrite(out, 2, total, f);
            fclose(f);
        }
        for (k = 0, px = out; !rc && k < n; k++)
        {
            uint32_t nb = 2 * (wh[k] & 0xFFFF) * (wh[k] >> 16);

            fnv[k] = 2166136261u;
            for (i = 0; i < nb; i++)
                fnv[k] = (fnv[k] ^ ((uint8_t *)px)[i]) * 16777619u;
            px += nb / 2;
        }
    }
    for (k = 0; k < n; k++)
    {
        uint32_t w = wh[k] & 0xFFFF, h = wh[k] >> 16;

        if (rc)
            printf("0 0 %d 0\n", rc);
        else
            printf("%u %u %u %08x\n", w, h, 2 * w * h, fnv[k]);
    }
    return rc != 0;
}
