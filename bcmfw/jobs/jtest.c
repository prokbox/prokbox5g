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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define J_F16W_VECTORS
#include "hostproto.h"
#include "jref.h"
#include "jmix.h"

/* Host tests of the job interface's reference executor and ring codec
 * (bcmfw/tests/jobs_test.py builds and runs this).  Prints one line per
 * failed case, then "jtest: N cases, M failed" and the scene's checksum. */

uint32_t jscene_run(uint8_t *mem);
uint32_t jscene_bytes(void);

static int cases, failed;

#define CHECK(c, ...) do { cases++; if (!(c)) { failed++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* The shell's xfade_rect() as it is in apps/ipodui/preview.c, in place */
static void shell_xfade(uint16_t *dst, const uint16_t *src, int n, int weight)
{
    unsigned w32 = (weight + 4) >> 3, iw = 32 - w32;
    int x;

    if (weight >= 256)
    {
        memcpy(dst, src, n * 2);
        return;
    }
    if (w32 == 0)
        w32 = 1, iw = 31;
    for (x = 0; x < n; x++)
    {
        unsigned d = dst[x], s = src[x], r;
        if (d == s)
            continue;
        d = (d | (d << 16)) & 0x07e0f81f;
        s = (s | (s << 16)) & 0x07e0f81f;
        r = ((d * iw + s * w32 + 0x02008010) >> 5) & 0x07e0f81f;
        dst[x] = r | (r >> 16);
    }
}

/* The shell's split shadow (apps/ipodui/material.c ui_shadow_apply, and
 * material_data.h's table), verbatim */
#define UI_SHADOW_W 10
static const uint8_t ui_shadow_a[UI_SHADOW_W] = { 92, 75, 59, 45, 33, 23, 15, 8, 4, 1 };

static void shell_shadow(uint16_t *px, int stride, int h)
{
    int y, c;

    for (y = 0; y < h; y++, px += stride)
        for (c = 0; c < UI_SHADOW_W; c++)
        {
            unsigned p = px[c], k = 256 - ui_shadow_a[c];

            px[c] = (uint16_t)(((((p >> 11) * k) >> 8) << 11)
                               | (((((p >> 5) & 63) * k) >> 8) << 5)
                               | (((p & 31) * k) >> 8));
        }
}

/* The shell's AVG2 (preview.c), on two pixels packed in a word */
#define AVG2(a, b) (((a) & (b)) + ((((a) ^ (b)) & 0xf7def7deu) >> 1))

static uint16_t A[64 * 32], B[64 * 32], D[64 * 32], P[64 * 32], E[64 * 32];

static struct jref j;

static void setup(void)
{
    struct jref_surf f = { D, 64, 32, 64 }, b = { B, 64, 32, 64 }, p = { P, 64, 32, 64 };
    static uint8_t pool[3][64 * 1024];

    jref_init(&j, f, b, p);
    jref_pool(&j, J_CLS_DRIFT, pool[0], sizeof(pool[0]));
    jref_pool(&j, J_CLS_STRIP, pool[1], sizeof(pool[1]));
    jref_pool(&j, J_CLS_SCRATCH, pool[2], 4096);
}

static uint32_t exec(uint32_t op, uint32_t tag, const uint32_t *pl, uint32_t n,
                     uint32_t *rep, uint32_t *nrep)
{
    uint32_t rec[HP_MAX_PAYLOAD + 1];

    rec[0] = HP_REC(op, n, tag);
    memcpy(rec + 1, pl, n * 4);
    return jref_exec(&j, rec, rep, nrep);
}

/* The shell's F2 marquee (apps/ipodui/list.c ui_list_tick and mq_step;
 * nowplaying.c marquee_step is the same rule), in ms: still for the
 * delay, then mq_x += px every step; at a full lap (mq_x >= lap) back to
 * 0, still for the pause.  mq_x 0 shows the label at rest.  Returns mq_x
 * at 'now', the selection having settled at 0 */
static uint32_t f2_at(uint32_t now, uint32_t lap, uint32_t px, uint32_t step,
                      uint32_t delay, uint32_t pause)
{
    uint32_t x = 0, next = delay, t;

    for (t = 0; t <= now; t++)
        if (t >= next)
        {
            x += px;
            next = t + step;
            if (x >= lap)
            {
                x = 0;
                next = t + pause;
            }
        }
    return x;
}

static uint32_t slot_with(uint32_t cls, uint32_t w, uint32_t h, uint32_t key,
                          const uint16_t *px);

/* PAN's stepped form (jobs.h J_PAN_STEP, rev 0.3): the marquee.  Every
 * frame of three laps equals F2's window, for laps odd and even, sent at
 * F2's first step (start = the pause); one lap ends on the rest; a start
 * mid-lap and past a lap; the refusals */
static void marquee(void)
{
    enum { TW = 20, RH = 6, PX = 2, STEP = 50, PAUSE = 1000 };
    static uint16_t pic[(2 * TW + 64) * RH];
    uint32_t L, a[12], rep[40], n, s, sw, t, x, y, bad, k, nx;

    for (L = 37; L <= 38; L++)
    {
        /* [the rest cell][the label's lap][its head again], one row band */
        setup();
        sw = TW + L + TW;
        for (y = 0; y < RH; y++)
            for (x = 0; x < sw; x++)
                pic[y * sw + x] = (uint16_t)(x + 1000 * y + 1);
        s = slot_with(J_CLS_STRIP, sw, RH, 0x3A + L, pic);
        a[0] = HP_REC(J_OP_ANIM, J_PAN_ARGS, 5);
        a[1] = J_ANIM_PAN; a[2] = s; a[3] = J_XY(TW, 0); a[4] = J_XY(TW + L, 0);
        a[5] = J_XY(TW, RH); a[6] = J_XY(9, 7); a[7] = STEP; a[8] = J_PAN_STEP | 1;
        a[9] = PX | (uint32_t)PAUSE << 16; a[10] = J_XY(0, 0); a[11] = PAUSE;
        CHECK(exec(J_OP_ANIM, 0, a + 1, J_PAN_ARGS, rep, &n) == HP_OK, "stepped pan refused");
        bad = 0;
        for (t = PAUSE; t < PAUSE + 3 * (PAUSE + (L / PX + 1) * STEP); t += 10)
        {
            uint32_t m = f2_at(t, L, PX, STEP, PAUSE, PAUSE), sx = m ? TW + m : 0;

            nx = jref_anim_frame(&j, a, t - PAUSE);
            if (nx != STEP * 1000)
                bad++;
            for (y = 0; y < RH; y++)
                for (x = 0; x < TW; x++)
                    if (D[(7 + y) * 64 + 9 + x] != pic[y * sw + sx + x])
                        bad++;
        }
        CHECK(bad == 0, "lap %u: %u pixels or intervals differ from F2", L, bad);

        /* one lap (loop 0): the last frame is at rest and ends it */
        a[8] = J_PAN_STEP;
        k = PAUSE + (L - 1) / PX * STEP;    /* the lap's ms */
        a[11] = 0;
        nx = jref_anim_frame(&j, a, k - 1);
        CHECK(nx && D[7 * 64 + 9] == pic[TW + (L - 1) / PX * PX],
              "lap %u: the last step %04x", L, D[7 * 64 + 9]);
        nx = jref_anim_frame(&j, a, k);
        CHECK(!nx && D[7 * 64 + 9] == pic[0], "lap %u: one lap ends at rest: %u %04x", L, nx,
              D[7 * 64 + 9]);
        /* a start mid-lap (step 3), and past a lap (its step 1) */
        a[8] = J_PAN_STEP | 1;
        a[11] = PAUSE + 2 * STEP;
        jref_anim_frame(&j, a, 0);
        CHECK(D[7 * 64 + 9] == pic[TW + 3 * PX], "lap %u: start at step 3", L);
        a[11] = k + PAUSE;
        jref_anim_frame(&j, a, 0);
        CHECK(D[7 * 64 + 9] == pic[TW + PX], "lap %u: start past a lap", L);
        jref_anim_frame(&j, a, STEP - 1);
        CHECK(D[7 * 64 + 9] == pic[TW + PX], "lap %u: no step before its time", L);
        jref_anim_frame(&j, a, STEP);
        CHECK(D[7 * 64 + 9] == pic[TW + 2 * PX], "lap %u: the step on time", L);
    }

    /* refusals: too short, no step, no room for one, the rest outside */
    a[8] = J_PAN_STEP | 1;
    a[11] = 0;
    CHECK(exec(J_OP_ANIM, 0, a + 1, 8, rep, &n) == HP_E_INVAL, "stepped, 8 words");
    a[9] = PAUSE << 16;
    CHECK(exec(J_OP_ANIM, 0, a + 1, J_PAN_ARGS, rep, &n) == J_E_RANGE, "a step of 0 px");
    a[9] = 38 | (uint32_t)PAUSE << 16;
    CHECK(exec(J_OP_ANIM, 0, a + 1, J_PAN_ARGS, rep, &n) == J_E_RANGE, "no whole step");
    a[9] = PX | (uint32_t)PAUSE << 16;
    a[10] = J_XY(sw - TW + 1, 0);
    CHECK(exec(J_OP_ANIM, 0, a + 1, J_PAN_ARGS, rep, &n) == J_E_RANGE, "rest outside");
    a[10] = J_XY(0, 1);
    CHECK(exec(J_OP_ANIM, 0, a + 1, J_PAN_ARGS, rep, &n) == J_E_RANGE, "rest below");
    a[10] = 0;
    a[7] = 65536;
    CHECK(exec(J_OP_ANIM, 0, a + 1, J_PAN_ARGS, rep, &n) == J_E_RANGE, "a 65,536 ms step");
    a[7] = STEP;
    CHECK(exec(J_OP_ANIM, 0, a + 1, J_PAN_ARGS, rep, &n) == HP_OK, "stepped again");
    /* no pause: one lap still ends on the rest window */
    a[9] = PX;
    a[11] = 0;
    a[8] = J_PAN_STEP;
    CHECK(jref_anim_frame(&j, a, 0) && D[7 * 64 + 9] == pic[TW + PX] &&
          !jref_anim_frame(&j, a, (L - 1) / PX * STEP) && D[7 * 64 + 9] == pic[0],
          "no pause: step 1 first, the rest at the end");
    a[9] = PX | (uint32_t)PAUSE << 16;
    /* the linear form keeps 30 fps and its end */
    a[0] = HP_REC(J_OP_ANIM, 8, 5);
    a[8] = 0;
    a[7] = 100;
    CHECK(jref_anim_frame(&j, a, 50) == J_ANIM_FRAME_US &&
          jref_anim_frame(&j, a, 100) == 0, "linear pan's interval and end");
}

static uint32_t slot_with(uint32_t cls, uint32_t w, uint32_t h, uint32_t key,
                          const uint16_t *px)
{
    uint32_t p[5] = { cls, J_XY(w, h), J_FMT_RGB565, key, 0 }, rep[40], n, s, r;
    uint16_t *dst;

    exec(J_OP_ALLOC, J_TAG_REPLY, p, 5, rep, &n);
    s = rep[1];
    dst = jref_slot_px(&j, s);
    if (dst && px)                      /* row by row, at the slot's pitch */
        for (r = 0; r < h; r++)
            memcpy(dst + r * j.slot[s].surf.pitch, px + r * w, w * 2);
    p[0] = s;                           /* the real sums, black slots too */
    jref_f16w(&j.slot[s].surf, &p[1], &p[2]);
    CHECK(exec(J_OP_COMMIT, 0, p, 3, rep, &n) == HP_OK, "COMMIT of slot %u", s);
    return s;
}

/* COMMIT's F16w against jobs.h's vectors: j_f16w() on each, whole and a
 * pixel at a time; then the vector slots through ALLOC and COMMIT (the
 * right sums, a wrong A, a wrong B, 0 0, the 0.1 two-word form) */
static void f16w_vectors(void)
{
    static uint8_t pool[J_BUDGET_DRIFT];
    static struct jref jv;
    struct jref_surf f = { D, 64, 32, 64 };
    uint32_t i, k, a, b, a1, b1, rec[8], rep[8], n, slot, st, x, s;
    uint16_t *px;

    for (i = 0; i < sizeof(j_f16w_vec) / sizeof(j_f16w_vec[0]); i++)
    {
        const struct j_f16w_vec *v = &j_f16w_vec[i];

        a = b = a1 = b1 = 0;
        j_f16w(&a, &b, v->px, v->n);
        for (k = 0; k < v->n; k++)
            j_f16w(&a1, &b1, v->px + k, 1);
        CHECK(a == v->a && b == v->b && a1 == a && b1 == b,
              "F16w %s: %08x %08x, want %08x %08x", v->name, a, b, v->a, v->b);
    }
    jref_init(&jv, f, f, f);
    jref_pool(&jv, J_CLS_DRIFT, pool, sizeof(pool));
    for (k = 0; k < 2; k++)
    {
        uint32_t want_a = k ? J_F16W_WHITE_A : J_F16W_SLOT_A;
        uint32_t want_b = k ? J_F16W_WHITE_B : J_F16W_SLOT_B;

        rec[0] = HP_REC(J_OP_ALLOC, 5, J_TAG_REPLY);
        rec[1] = J_CLS_DRIFT; rec[2] = J_XY(300, 300); rec[3] = J_FMT_RGB565;
        rec[4] = 0xF16 + k; rec[5] = 0;
        st = jref_exec(&jv, rec, rep, &n);
        CHECK(st == HP_OK && rep[3] == 2 * J_DRIFT_PITCH, "vector slot ALLOC %u", st);
        if (st != HP_OK)
            return;
        slot = rep[1];
        px = jref_slot_px(&jv, slot);
        s = J_F16W_SEED;
        for (i = 0; i < 300; i++)
            for (x = 0; x < J_DRIFT_PITCH; x++)
            {
                s = x < 300 ? s * 1103515245u + 12345u : s;
                px[i * J_DRIFT_PITCH + x] = x >= 300 ? 0xDEAD : k ? 0xFFFF : s >> 16;
            }
        rec[0] = HP_REC(J_OP_COMMIT, 3, 0);
        rec[1] = slot; rec[2] = want_a ^ 1; rec[3] = want_b;
        CHECK(jref_exec(&jv, rec, rep, &n) == J_E_SUM, "vector slot %u: a wrong A", k);
        rec[2] = want_a; rec[3] = want_b ^ 0x80000000u;
        CHECK(jref_exec(&jv, rec, rep, &n) == J_E_SUM, "vector slot %u: a wrong B", k);
        rec[0] = HP_REC(J_OP_COMMIT, 2, 0);
        CHECK(jref_exec(&jv, rec, rep, &n) == HP_E_INVAL, "a rev 0.1 COMMIT (two words)");
        /* white with 0, 0: checked, so refused; then the explicit flag */
        rec[0] = HP_REC(J_OP_COMMIT, 3, J_TAG_REPLY);
        rec[2] = 0; rec[3] = 0;
        CHECK(jref_exec(&jv, rec, rep, &n) == J_E_SUM,
              "vector slot %u: 0, 0 is checked, not a pass", k);
        rec[0] = HP_REC(J_OP_COMMIT, 4, J_TAG_REPLY);
        rec[2] = k ? 0 : want_a; rec[3] = k ? 0 : want_b;
        rec[4] = k ? J_COMMIT_NOCHECK : 0;              /* white: unchecked */
        st = jref_exec(&jv, rec, rep, &n);
        CHECK(st == HP_OK && n == 3 && rep[1] == want_a && rep[2] == want_b,
              "vector slot %u: COMMIT %u, %08x %08x", k, st, rep[1], rep[2]);
    }
}

int main(void)
{
    uint32_t rep[40], n, p[8], i, w, s0, s1, st;
    uint32_t seed = 60;

    for (i = 0; i < 64 * 32; i++)
    {
        seed = seed * 1103515245u + 12345u;
        A[i] = seed >> 8;
        seed = seed * 1103515245u + 12345u;
        B[i] = (i & 7) ? seed >> 8 : A[i];         /* some equal pixels */
    }

    /* FADE equals the shell's xfade_rect for every weight */
    setup();
    s0 = slot_with(J_CLS_DRIFT, 64, 32, 1, A);
    s1 = slot_with(J_CLS_DRIFT, 64, 32, 2, B);
    for (w = 1; w <= 256; w++)
    {
        p[0] = J_SURF_FRONT; p[1] = 0; p[2] = J_XY(64, 32); p[3] = s0; p[4] = 0;
        p[5] = s1; p[6] = 0; p[7] = w;
        exec(J_OP_FADE, 0, p, 8, rep, &n);
        memcpy(E, A, sizeof(E));
        shell_xfade(E, B, 64 * 32, w);
        CHECK(!memcmp(D, E, sizeof(E)), "FADE weight %u differs from xfade_rect", w);
    }
    /* the shared header (jmix.h, gap G5) in place equals the shell's loop */
    for (w = 1; w <= 256; w += 17)
    {
        memcpy(D, A, sizeof(D));
        jmix_fade_rect(D, 64, D, 64, B, 64, 64, 32, w);
        memcpy(E, A, sizeof(E));
        shell_xfade(E, B, 64 * 32, w);
        CHECK(!memcmp(D, E, sizeof(E)), "jmix_fade_rect in place, weight %u", w);
    }
    /* SHADE equals ui_shadow_apply; FADE with the shade flag equals
     * ui_panel_fade(raw): the shadow columns of the raw source darkened,
     * then the fade; AVG then SHADE equals half_window (review H2) */
    {
        uint32_t q[16], k;

        memcpy(D, A, sizeof(D));
        q[0] = J_SURF_FRONT; q[1] = J_XY(0, 0); q[2] = J_XY(UI_SHADOW_W, 32);
        q[3] = q[4] = q[5] = 0;
        for (k = 0; k < UI_SHADOW_W; k++)
            q[3 + k / 4] |= (uint32_t)ui_shadow_a[k] << (8 * (k & 3));
        CHECK(exec(J_OP_SHADE, 0, q, 6, rep, &n) == HP_OK, "SHADE refused");
        memcpy(E, A, sizeof(E));
        shell_shadow(E, 64, 32);
        CHECK(!memcmp(D, E, sizeof(E)), "SHADE differs from ui_shadow_apply");
        memcpy(D, A, sizeof(D));
        jmix_shade_cols(D, 64, 32, ui_shadow_a, UI_SHADOW_W);
        CHECK(!memcmp(D, E, sizeof(E)), "jmix_shade_cols differs");
        for (w = 1; w <= 256; w += 15)
        {
            uint16_t t[UI_SHADOW_W];
            int y;

            q[0] = J_SURF_FRONT; q[1] = 0; q[2] = J_XY(64, 32); q[3] = s0; q[4] = 0;
            q[5] = s1; q[6] = 0; q[7] = w; q[8] = J_FADE_SHADE_B | UI_SHADOW_W;
            q[9] = q[10] = q[11] = 0;
            for (k = 0; k < UI_SHADOW_W; k++)
                q[9 + k / 4] |= (uint32_t)ui_shadow_a[k] << (8 * (k & 3));
            CHECK(exec(J_OP_FADE, 0, q, 12, rep, &n) == HP_OK, "raw FADE refused");
            /* the shell: the fade past the shadow, then per row the
             * shaded source columns faded in */
            memcpy(E, A, sizeof(E));
            for (y = 0; y < 32; y++)
            {
                shell_xfade(E + y * 64 + UI_SHADOW_W, B + y * 64 + UI_SHADOW_W,
                            64 - UI_SHADOW_W, w);
                memcpy(t, B + y * 64, sizeof(t));
                shell_shadow(t, UI_SHADOW_W, 1);
                shell_xfade(E + y * 64, t, UI_SHADOW_W, w);
            }
            CHECK(!memcmp(D, E, sizeof(E)), "raw FADE weight %u differs from ui_panel_fade", w);
        }
        q[0] = J_SURF_FRONT; q[1] = 0; q[2] = J_XY(64, 32); q[3] = s0; q[4] = 0;
        q[5] = s1; q[6] = 0;
        exec(J_OP_AVG, 0, q, 7, rep, &n);
        q[0] = J_SURF_FRONT; q[1] = 0; q[2] = J_XY(UI_SHADOW_W, 32); q[3] = q[4] = q[5] = 0;
        for (k = 0; k < UI_SHADOW_W; k++)
            q[3 + k / 4] |= (uint32_t)ui_shadow_a[k] << (8 * (k & 3));
        exec(J_OP_SHADE, 0, q, 6, rep, &n);
        for (k = 0; k < 64 * 32; k += 2)
        {
            uint32_t pa = A[k] | (uint32_t)A[k + 1] << 16, pb = B[k] | (uint32_t)B[k + 1] << 16;
            uint32_t v = AVG2(pa, pb);

            E[k] = v & 0xFFFF;
            E[k + 1] = v >> 16;
        }
        shell_shadow(E, 64, 32);
        CHECK(!memcmp(D, E, sizeof(E)), "AVG then SHADE differs from half_window");
        q[2] = J_XY(J_SHADE_MAX + 1, 32);
        CHECK(exec(J_OP_SHADE, 0, q, 12, rep, &n) == HP_E_INVAL, "a 33-column shade");
    }
    /* AVG equals AVG2 on packed pairs */
    p[0] = J_SURF_FRONT; p[1] = 0; p[2] = J_XY(64, 32); p[3] = s0; p[4] = 0;
    p[5] = s1; p[6] = 0;
    exec(J_OP_AVG, 0, p, 7, rep, &n);
    for (i = 0; i < 64 * 32; i += 2)
    {
        uint32_t a = A[i] | (uint32_t)A[i + 1] << 16, b = B[i] | (uint32_t)B[i + 1] << 16;
        uint32_t v = AVG2(a, b);

        CHECK(D[i] == (v & 0xFFFF) && D[i + 1] == v >> 16, "AVG pixel %u", i);
    }
    CHECK(exec(J_OP_FADE, 0, (uint32_t[]){ J_SURF_FRONT, 0, J_XY(64, 32), s0, 0, s1, 0, 0 },
               8, rep, &n) == HP_E_INVAL, "weight 0 accepted");

    /* slots: a hit, a bad sum, the epoch, a full pool, free and reuse */
    {
        uint32_t q[5] = { J_CLS_DRIFT, J_XY(64, 32), J_FMT_RGB565, 1, 0 };

        st = exec(J_OP_ALLOC, J_TAG_REPLY, q, 5, rep, &n);
        CHECK(st == HP_OK && rep[1] == s0 && rep[4] == J_ALLOC_HIT, "no hit for key 1");
        q[3] = 99;
        exec(J_OP_ALLOC, J_TAG_REPLY, q, 5, rep, &n);
        p[0] = rep[1]; p[1] = 12345; p[2] = 0;
        CHECK(exec(J_OP_COMMIT, 0, p, 3, rep, &n) == J_E_SUM && n == 1 && rep[0] == J_E_SUM,
              "a wrong sum committed");
        q[0] = J_CLS_SCRATCH; q[1] = J_XY(64, 64);
        CHECK(exec(J_OP_ALLOC, 0, q, 5, rep, &n) == J_E_NOSLOT, "8 KB in a 4 KB pool");
        q[1] = J_XY(32, 32);
        CHECK(exec(J_OP_ALLOC, J_TAG_REPLY, q, 5, rep, &n) == HP_OK, "2 KB in a 4 KB pool");
        i = rep[1];
        CHECK(exec(J_OP_ALLOC, J_TAG_REPLY, q, 5, rep, &n) == HP_OK, "the second 2 KB");
        CHECK(exec(J_OP_ALLOC, J_TAG_REPLY, q, 5, rep, &n) == J_E_NOSLOT, "pool overfilled");
        p[0] = i;
        exec(J_OP_FREE, 0, p, 1, rep, &n);
        CHECK(exec(J_OP_ALLOC, J_TAG_REPLY, q, 5, rep, &n) == HP_OK, "no reuse after FREE");
        q[0] = J_CLS_APP;
        CHECK(exec(J_OP_ALLOC, 0, q, 5, rep, &n) == J_E_NOSLOT, "APP without its pool");
        q[0] = J_CLS_GAME;
        CHECK(exec(J_OP_ALLOC, 0, q, 5, rep, &n) == J_E_MODE, "a game slot in UI mode");
        exec(J_OP_RESET, 0, p, 0, rep, &n);
        p[0] = J_SURF_FRONT; p[1] = 0; p[2] = J_XY(8, 8); p[3] = s0; p[4] = 0;
        p[5] = s1; p[6] = 0; p[7] = 128;
        CHECK(exec(J_OP_FADE, 0, p, 8, rep, &n) == J_E_SLOT, "a slot survived RESET");
    }

    /* replies: none unasked, always for errors and FENCE */
    setup();
    p[0] = J_SURF_FRONT; p[1] = 0; p[2] = J_XY(4, 4); p[3] = 0x1234;
    exec(J_OP_FILL, 0, p, 4, rep, &n);
    CHECK(n == 0, "FILL replied unasked");
    CHECK(D[0] == 0x1234 && D[3 * 64 + 3] == 0x1234 && D[4] != 0x1234,
          "FILL's 4x4 rect not filled, or spilled");
    exec(J_OP_FILL, J_TAG_REPLY, p, 4, rep, &n);
    CHECK(n == 1 && rep[0] == HP_OK, "FILL with a reply: %u words", n);
    p[2] = J_XY(65, 1);
    CHECK(exec(J_OP_FILL, 0, p, 4, rep, &n) == J_E_RANGE && n == 1, "a rect off the frame");
    p[0] = 41;
    CHECK(exec(J_OP_FENCE, 0, p, 1, rep, &n) == HP_OK && n == 2 && rep[1] == 41, "fence");
    CHECK(exec(J_OP_DECODE, 0, p, 1, rep, &n) == HP_E_UNKNOWN, "reserved op ran");
    CHECK(exec(J_OP_BLIT, 0, p, 2, rep, &n) == HP_E_INVAL, "a short BLIT ran");

    /* SCROLL: up and down; the rows uncovered keep their pixels */
    setup();
    for (i = 0; i < 64 * 32; i++)
        D[i] = i / 64;
    p[0] = J_SURF_FRONT; p[1] = J_XY(0, 4); p[2] = J_XY(64, 20); p[3] = (uint16_t)-3;
    exec(J_OP_SCROLL, 0, p, 4, rep, &n);
    CHECK(D[4 * 64] == 7 && D[20 * 64] == 23 && D[21 * 64] == 21 && D[23 * 64] == 23,
          "scroll up: rows %u %u %u", D[4 * 64], D[20 * 64], D[21 * 64]);
    for (i = 0; i < 64 * 32; i++)
        D[i] = i / 64;
    p[3] = 5;
    exec(J_OP_SCROLL, 0, p, 4, rep, &n);
    CHECK(D[9 * 64] == 4 && D[23 * 64] == 18 && D[4 * 64] == 4 && D[8 * 64] == 8,
          "scroll down: rows %u %u", D[9 * 64], D[23 * 64]);

    /* BLIT: overlapping on one surface behaves as memmove */
    for (i = 0; i < 64 * 32; i++)
        D[i] = i;
    p[0] = J_SURF_FRONT; p[1] = J_XY(0, 0); p[2] = J_XY(32, 8); p[3] = J_SURF_FRONT;
    p[4] = J_XY(4, 2); p[5] = 0;
    exec(J_OP_BLIT, 0, p, 6, rep, &n);
    CHECK(D[2 * 64 + 4] == 0 && D[9 * 64 + 35] == 7 * 64 + 31, "overlapping blit");

    /* PRESENT copies to the panel; a slide ends on its destination */
    setup();
    for (i = 0; i < 64 * 32; i++)
    {
        D[i] = 0x1111;
        B[i] = 0x2222;
    }
    p[0] = J_SURF_FRONT; p[1] = 0; p[2] = J_XY(64, 32); p[3] = 0;
    exec(J_OP_PRESENT, 0, p, 4, rep, &n);
    CHECK(P[0] == 0x1111 && P[64 * 32 - 1] == 0x1111, "present");
    {
        /* five frames 25 ms apart, the shell's offsets 0 16 32 48 64, into
         * the front frame, out of it (rev 0.4: moved on in place) */
        uint32_t a[12] = { HP_REC(J_OP_ANIM, 11, 3), J_ANIM_SLIDE, J_SURF_FRONT, J_SURF_BACK,
                           0, J_XY(64, 32), 0, 25, 5, 0 | 16 << 16, 32 | 48 << 16, 64 };
        uint32_t rep2[40], n2;
        int run;

        CHECK(exec(J_OP_ANIM, 0, a + 1, 11, rep2, &n2) == HP_OK, "slide refused");
        run = jref_anim_frame(&j, a, 0);
        CHECK(run && D[0] == 0x1111 && D[63] == 0x1111, "slide at 0");
        jref_anim_frame(&j, a, 50);
        CHECK(D[0] == 0x1111 && D[63] == 0x2222 && D[31] == 0x1111 && D[32] == 0x2222,
              "slide halfway: %04x %04x", D[31], D[32]);
        run = jref_anim_frame(&j, a, 100);
        CHECK(!run && D[0] == 0x2222 && D[63] == 0x2222, "slide at the end");
        a[10] = 8 | 48 << 16;
        CHECK(exec(J_OP_ANIM, 0, a + 1, 11, rep2, &n2) == J_E_RANGE, "offsets going back");
        a[10] = 32 | 48 << 16;
        a[11] = 65;
        CHECK(exec(J_OP_ANIM, 0, a + 1, 11, rep2, &n2) == J_E_RANGE, "offset past the width");
        a[8] = J_SLIDE_MAX + 1;
        CHECK(exec(J_OP_ANIM, 0, a + 1, 11, rep2, &n2) == HP_E_INVAL, "too many frames");
    }

    marquee();

    /* the ring: wrap-around, full, too big, byte for byte */
    {
        uint32_t w8[8], rec[40], k, got;
        struct jring r = { w8, 8, 0xFFFC, 0xFFFC };     /* indices about to wrap */
        uint32_t pl[3] = { 0xA, 0xB, 0xC };

        CHECK(jring_put(&r, J_OP_FILL, 5, pl, 3) == HP_OK, "put 1");
        CHECK(jring_put(&r, J_OP_FENCE, 6, pl, 3) == HP_OK, "put 2");
        CHECK(jring_put(&r, J_OP_FENCE, 7, pl, 0) == HP_E_BUSY, "an overfull ring took it");
        CHECK(w8[0xFFFC & 7] == HP_REC(J_OP_FILL, 3, 5) && w8[(0xFFFC + 3) & 7] == 0xC &&
              w8[(0xFFFC + 4) & 7] == HP_REC(J_OP_FENCE, 3, 6) && w8[(0xFFFC + 7) & 7] == 0xC,
              "ring words");
        got = jring_get(&r, rec);
        CHECK(got == 4 && rec[0] == HP_REC(J_OP_FILL, 3, 5) && rec[3] == 0xC, "get 1");
        got = jring_get(&r, rec);
        CHECK(got == 4 && HP_REC_TAG(rec[0]) == 6 && r.rd == 4 && r.wr == 4, "get 2, wrapped");
        CHECK(jring_get(&r, rec) == 0, "an empty ring gave a record");
        for (k = 0; k < 33; k++)
            pl[0] = k;
        CHECK(jring_put(&r, J_OP_FILL, 0, pl, 33) == HP_E_TOOBIG, "33 words accepted");
    }

    f16w_vectors();

    /* the scene, as vc2sim runs it */
    {
        uint8_t *mem = malloc(jscene_bytes());
        uint32_t h = jscene_run(mem);

        CHECK((h & 0xFFF00000u) != 0xBAD00000u, "scene: %08x", h);
        printf("jscene %08x\n", h);
        free(mem);
    }
    printf("jtest: %d cases, %d failed\n", cases, failed);
    return failed != 0;
}
