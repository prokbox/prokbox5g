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

#include <stdint.h>
#include "hostproto.h"
#include "jref.h"

/* A scripted scene through the ring and the reference executor: a drift
 * in miniature (two pictures uploaded into slots, a pan, fade steps, a
 * half step, presents), a slide, a scroll, a fill and a keyed blit.
 * Built for the Mac (jtest.c) and for the BCM (vc2sim, jobs_test.py):
 * both must give the same checksum, which is also golden in the test.
 * mem: JSCENE_BYTES of scratch. */

#define SW 96
#define SH 64
#define SIDE 72
#define SLOT_PITCH J_SLOT_PITCH(J_CLS_DRIFT, SIDE)  /* pixels: 80 */
#define SLOT_BYTES (SIDE * SLOT_PITCH * 2)

#define JSCENE_BYTES (3 * SW * SH * 2 + 2 * 4096 + 2 * SLOT_BYTES + 1024 + 64)

static uint32_t lcg(uint32_t *s)
{
    *s = *s * 1103515245u + 12345u;
    return *s >> 8;
}

/* The scene's checksum of a surface (FNV-1a over its pixels' bytes, row
 * by row): a test fingerprint, not COMMIT's sum */
static uint32_t fnv_surf(const struct jref_surf *s)
{
    uint32_t h = 0x811C9DC5u, x, y;

    for (y = 0; y < s->h; y++)
        for (x = 0; x < s->w; x++)
        {
            uint16_t v = s->px[y * s->pitch + x];

            h = (h ^ (v & 0xFF)) * 0x01000193u;
            h = (h ^ (v >> 8)) * 0x01000193u;
        }
    return h;
}

static uint32_t run(struct jref *j, struct jring *r, uint32_t *rec, uint32_t *rep,
                    uint32_t *bad)
{
    uint32_t n, nrep, last = 0;

    while ((n = jring_get(r, rec)) != 0)
    {
        if (jref_exec(j, rec, rep, &nrep) != HP_OK)
            (*bad)++;
        if (nrep)
            last = rep[nrep > 1 ? 1 : 0];
    }
    return last;
}

uint32_t jscene_run(uint8_t *mem)
{
    static struct jref j;
    uint32_t ring_w[1024], rec[HP_MAX_PAYLOAD + 1], rep[HP_MAX_PAYLOAD + 1];
    struct jring r;
    struct jref_surf front, back, panel;
    uint16_t *fb = (uint16_t *)mem;
    uint32_t seed = 2722, bad = 0, slot[2], k, t, p[8], h, slides = 0;
    uint8_t *pool = mem + 3 * SW * SH * 2;

    front.px = fb; front.w = SW; front.h = SH; front.pitch = SW;
    back.px = fb + SW * SH; back.w = SW; back.h = SH; back.pitch = SW;
    panel.px = fb + 2 * SW * SH; panel.w = SW; panel.h = SH; panel.pitch = SW;
    for (k = 0; k < 3 * SW * SH; k++)
        fb[k] = 0;
    jref_init(&j, front, back, panel);
    jref_pool(&j, J_CLS_DRIFT, pool, 2 * SLOT_BYTES + 1024);
    jref_pool(&j, J_CLS_STRIP, pool + 2 * SLOT_BYTES + 1024, 8192);
    r.w = ring_w;
    r.size = 1024;
    r.wr = r.rd = 0;

    /* two covers: ALLOC, the PP's write, COMMIT with the right sum */
    for (k = 0; k < 2; k++)
    {
        uint16_t *px;
        uint32_t i;

        p[0] = J_CLS_DRIFT; p[1] = J_XY(SIDE, SIDE); p[2] = J_FMT_RGB565;
        p[3] = 0x1000 + k; p[4] = 0xC0FE;
        jring_put(&r, J_OP_ALLOC, J_TAG_REPLY | k, p, 5);
        slot[k] = run(&j, &r, rec, rep, &bad);     /* the ring drains in order */
        px = jref_slot_px(&j, slot[k]);
        if (!px)
            return 0xBAD00000u | 0xFFFFF;
        for (i = 0; i < SIDE * SIDE; i++)
            px[i / SIDE * SLOT_PITCH + i % SIDE] = (uint16_t)((i % SIDE) * (k ? 7 : 3) + (i / SIDE) * (k ? 5 : 11) +
                               (lcg(&seed) & 3));
        {
            struct jref_surf c = { px, SIDE, SIDE, SLOT_PITCH };

            p[0] = slot[k];
            jref_f16w(&c, &p[1], &p[2]);
        }
        jring_put(&r, J_OP_COMMIT, 0, p, 3);
    }
    run(&j, &r, rec, rep, &bad);

    /* the drift: a pan of cover 0 into the front frame at three times */
    for (t = 0; t <= 1000; t += 500)
    {
        uint32_t a[9] = { HP_REC(J_OP_ANIM, 8, 1), J_ANIM_PAN, slot[0], J_XY(0, 0),
                          J_XY(SIDE - 40, SIDE - 30), J_XY(40, 30), J_XY(8, 4), 1000, 0 };

        jring_put(&r, J_OP_ANIM, 1, a + 1, 8);
        run(&j, &r, rec, rep, &bad);
        jref_anim_frame(&j, a, t);
        p[0] = J_SURF_FRONT; p[1] = J_XY(8, 4); p[2] = J_XY(40, 30); p[3] = 0;
        jring_put(&r, J_OP_PRESENT, 0, p, 4);
    }
    /* fade steps toward cover 1, then a half step, each presented */
    for (k = 1; k <= 8; k++)
    {
        p[0] = J_SURF_FRONT; p[1] = J_XY(8, 4); p[2] = J_XY(40, 30);
        p[3] = J_SURF_FRONT; p[4] = J_XY(8, 4); p[5] = slot[1]; p[6] = J_XY(3, 5);
        p[7] = (256 * k + 4) / 8;
        jring_put(&r, J_OP_FADE, 0, p, 8);
        p[0] = J_SURF_FRONT; p[1] = J_XY(8, 4); p[2] = J_XY(40, 30); p[3] = 0;
        jring_put(&r, J_OP_PRESENT, 0, p, 4);
    }
    p[0] = J_SURF_FRONT; p[1] = J_XY(8, 4); p[2] = J_XY(40, 30);
    p[3] = J_SURF_FRONT; p[4] = J_XY(8, 4); p[5] = slot[0]; p[6] = J_XY(1, 1);
    jring_put(&r, J_OP_AVG, 0, p, 7);
    /* the split shadow on the panel's left columns (8b-3's table) */
    p[0] = J_SURF_FRONT; p[1] = J_XY(8, 4); p[2] = J_XY(10, 30);
    p[3] = 92 | 75 << 8 | 59 << 16 | (uint32_t)45 << 24;
    p[4] = 33 | 23 << 8 | 15 << 16 | (uint32_t)8 << 24;
    p[5] = 4 | 1 << 8;
    jring_put(&r, J_OP_SHADE, 0, p, 6);
    /* a fill, a keyed blit, a scroll, into the back frame */
    p[0] = J_SURF_BACK; p[1] = J_XY(0, 0); p[2] = J_XY(SW, SH); p[3] = 0x2104;
    jring_put(&r, J_OP_FILL, 0, p, 4);
    p[0] = slot[1]; p[1] = J_XY(10, 10); p[2] = J_XY(50, 40); p[3] = J_SURF_BACK;
    p[4] = J_XY(20, 12); p[5] = J_BLIT_KEY | (uint32_t)0x0040 << 16;
    jring_put(&r, J_OP_BLIT, 0, p, 6);
    p[0] = J_SURF_BACK; p[1] = J_XY(16, 8); p[2] = J_XY(64, 48); p[3] = (uint16_t)-7;
    jring_put(&r, J_OP_SCROLL, 0, p, 4);
    p[0] = J_SURF_FRONT; p[1] = J_XY(0, 0); p[2] = J_XY(SW, SH); p[3] = 0;
    jring_put(&r, J_OP_PRESENT, 0, p, 4);
    p[0] = 7;
    jring_put(&r, J_OP_FENCE, 0, p, 1);
    if (run(&j, &r, rec, rep, &bad) != 7)
        bad++;
    /* a slide from the front frame itself (rev 0.4: moved on in place)
     * to the back, a frame every 33 ms, each presented */
    {
        uint32_t a[13] = { HP_REC(J_OP_ANIM, 12, 2), J_ANIM_SLIDE, J_SURF_FRONT, J_SURF_BACK,
                           J_XY(0, 0), J_XY(SW, SH), 1, 33, 7,
                           0 | 30 << 16, 55 | 73 << 16, 85 | 93 << 16, 96 };

        jring_put(&r, J_OP_ANIM, 2, a + 1, 12);
        run(&j, &r, rec, rep, &bad);
        for (t = 0; t <= 231; t += 33)
        {
            jref_anim_frame(&j, a, t);
            p[0] = J_SURF_FRONT; p[1] = 0; p[2] = J_XY(SW, SH); p[3] = 0;
            jring_put(&r, J_OP_PRESENT, 0, p, 4);
            run(&j, &r, rec, rep, &bad);
            slides = slides * 31 + fnv_surf(&panel);
        }
    }
    h = fnv_surf(&panel) ^ (fnv_surf(&front) * 3) ^ (fnv_surf(&back) * 7) ^ slides;
    return bad ? 0xBAD00000u | bad : h;
}

uint32_t jscene_bytes(void)
{
    return JSCENE_BYTES;
}
