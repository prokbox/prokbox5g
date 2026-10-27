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
 * The pixel hand-off (BCM MF-4) in a two-core cache-incoherence model:
 * firmware/target/arm/ipod/video/lcd-fb-xcore.c run as built, the COP
 * drawing, the CPU pushing, at seeded random interleavings.
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

/* The model (as tests/febe/cachemodel.c): two "cores" (threads that run
 * one at a time and switch at seeded random points on every shared
 * access), each with its own direct-mapped write-back data cache over one
 * shared SDRAM, plus an uncached window onto the same SDRAM.  Dirty lines
 * are evicted at random.  The frame is 16 x 8 pixels of one word each
 * (64-byte rows: whole lines, as the device's 640-byte rows).
 *
 * Three hand-offs, as the device runs them:
 *  A. a CPU push of the COP's frame (lcd_awake's wake frame, the devtest's
 *     hand-back, any lcd_update_rect from a CPU thread): lcd_fb_fence()
 *     then reads through lcd_fb_read(), random rects, four rounds;
 *  B. vplayer's overlay: the COP draws a rect, lcd_fb_publish(), posts;
 *     the CPU reads it through lcd_fb_read();
 *  C. lcd_fb_take() in power_off's shape (that clear is compiled out on
 *     the iPod Video by HAVE_LCD_SHUTDOWN; the take is the API for any CPU
 *     code that draws): the COP waits in a be_call (written back before
 *     the post); the CPU lcd_fb_take()s, draws its top row, pushes the frame, and
 *     the COP's leftover dirty lines land after (the worst case).
 * Each runs correct (no stale pixel over any seed) and with one operation
 * left out (the negative controls: each must be caught on some seed). */

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FW      16                  /* the frame, pixels (one word each) */
#define FH      8
#define MEMW    (FW * FH)
#define LINES   32                  /* per core: 32 lines of 16 B, the
                                       frame's size: stale lines live long */
#define LW      4                   /* words per line */
#define XCORE_LINE 16

static uint32_t mem[MEMW] __attribute__((aligned(16)));   /* uncached */
static uint32_t cview[MEMW] __attribute__((aligned(16))); /* cached (address) */

struct line { int valid, dirty; unsigned tag; uint32_t d[LW]; };
static struct line cache[2][LINES];

static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int turn, alive[2];
static __thread int me;
static unsigned rng;

/* the operations a negative control leaves out */
enum {
    O_FENCE_WB  = 1 << 0,   /* the producer's fence skips its write-back */
    O_NO_FENCE  = 1 << 1,   /* the consumer pushes without a fence */
    O_CACHED    = 1 << 2,   /* the consumer reads through its cache */
    O_PUB_COP   = 1 << 3,   /* lcd_fb_publish() on the COP does nothing */
    O_BECALL_WB = 1 << 4,   /* the be_call does not write back first */
    O_ACQ       = 1 << 5,   /* lcd_fb_take()'s drop left out */
};
static unsigned omit;

static unsigned rnd(void)
{
    rng = rng * 1103515245u + 12345u;
    return (rng >> 16) & 0x7fff;
}

static void switch_to_other(void)
{
    if (!alive[!me])
        return;
    turn = !me;
    pthread_cond_broadcast(&cv);
    while (turn != me)
        pthread_cond_wait(&cv, &mx);
}

static void writeback(struct line *l)
{
    if (l->valid && l->dirty)
        memcpy(&mem[l->tag * LW], l->d, sizeof(l->d));
    l->dirty = 0;
}

static void step(void)
{
    if (rnd() % 3 == 0)
        switch_to_other();
    if (rnd() % 5 == 0)
    {
        struct line *l = &cache[me][rnd() % LINES];
        writeback(l);
        l->valid = 0;
    }
}

static struct line *fill(unsigned i)
{
    unsigned tag = i / LW;
    struct line *l = &cache[me][tag % LINES];

    if (!l->valid || l->tag != tag)
    {
        writeback(l);
        l->valid = 1;
        l->dirty = 0;
        l->tag = tag;
        memcpy(l->d, &mem[tag * LW], sizeof(l->d));
    }
    return l;
}

static uint32_t ld(const uint32_t *p)
{
    step();
    if (p >= mem && p < mem + MEMW)
        return mem[p - mem];
    if (p >= cview && p < cview + MEMW)
    {
        unsigned i = (unsigned)(p - cview);
        return fill(i)->d[i % LW];
    }
    abort();
}

static void st(uint32_t *p, uint32_t v)
{
    step();
    if (p >= mem && p < mem + MEMW)
        mem[p - mem] = v;
    else if (p >= cview && p < cview + MEMW)
    {
        unsigned i = (unsigned)(p - cview);
        struct line *l = fill(i);
        l->d[i % LW] = v;
        l->dirty = 1;
    }
    else
        abort();
}

#define C_COMMIT    1
#define C_DISCARD   2
static void cache_ops(unsigned ops)
{
    int i;
    for (i = 0; i < LINES; i++)
    {
        struct line *l = &cache[me][i];
        writeback(l);
        if (ops & C_DISCARD)
            l->valid = 0;
    }
}

static void *to_mem(void *p)
{
    uint32_t *q = p;
    if (q >= cview && q < cview + MEMW)
        return mem + (q - cview);
    return p;
}

/* ---- the code under test, as built ------------------------------------ */
#include "../../firmware/export/fe_fb.h"
static int model_fence(const struct fe_fb_rect *r);
typedef uint32_t lfx_px;
#define LFX_HOST
#define LFX_W           FW
#define LFX_H           FH
#define LFX_FB          ((char *)cview)
#define LFX_CORE()      ((unsigned)me)
#define LFX_CPU         0u
#define LFX_UNCACHED(p) ((omit & O_CACHED) ? (p) : to_mem(p))
#define LFX_PUBLISH()   do { if (!((omit & O_PUB_COP) && me == 1)) \
                                 cache_ops(C_COMMIT); } while (0)
#define LFX_ACQUIRE()   do { if (!(omit & O_ACQ)) \
                                 cache_ops(C_COMMIT | C_DISCARD); } while (0)
#define LFX_FENCE(r)    model_fence(r)
#define LFX_STATE
#include "../../firmware/target/arm/ipod/video/lcd-fb-xcore.c"

/* ---- the producer's fence (the shell's fe_fb_publish, modelled) -------
 * The consumer asks and waits, bounded; the COP answers between draws (the
 * ui thread at its queue: FE_FB_PUBLISHED), or at once when it is blocked
 * in a be_call or its input wait, which wrote back first (FE_FB_CLEAN).
 * In a modal loop that does not serve the fence it never answers: after
 * the bound, FE_FB_TIMEOUT. */
#define FENCE_BOUND 40              /* switches to the other core */
static volatile int f_req, f_done, cop_in_call, timeouts;

static int model_fence(const struct fe_fb_rect *r)
{
    int n;

    (void)r;
    if (omit & O_NO_FENCE)
        return FE_FB_PUBLISHED;
    if (cop_in_call)
        return FE_FB_CLEAN;
    f_req = 1;
    for (n = 0; !f_done; n++)
    {
        if (n >= FENCE_BOUND)
        {
            timeouts++;
            return FE_FB_TIMEOUT;
        }
        switch_to_other();
    }
    f_done = 0;
    return FE_FB_PUBLISHED;
}

static void cop_serve(void)
{
    if (f_req)
    {
        if (!(omit & O_FENCE_WB))
            cache_ops(C_COMMIT);
        f_req = 0;
        f_done = 1;
    }
}

static uint32_t px_val(int gen, int i) { return (uint32_t)gen << 16 | i; }

/* ---- shared scenario state (uncached in the model: plain words) ------- */
static volatile int claimed, drawn, pushed, bad;
static int ra[8][4];                /* each round's rect */

/* ---- A: the CPU pushes the COP's frame -------------------------------- */
#define ROUNDS 4
static void a_cop(void)
{
    int g, i;

    lcd_fb_claim();
    claimed = 1;
    for (g = 1; g <= ROUNDS; g++)
    {
        for (i = 0; i < MEMW; i++)
        {
            st(&cview[i], px_val(g, i));
            if (rnd() % 7 == 0)
                cop_serve();        /* a fence between draws: allowed */
        }
        drawn = g;
        while (pushed != g)
        {
            cop_serve();
            switch_to_other();
        }
    }
}

static void a_cpu(void)
{
    int g, x, y;

    while (!claimed)
        switch_to_other();
    for (g = 1; g <= ROUNDS; g++)
    {
        const int *r = ra[g];
        const uint32_t *p;

        while (drawn != g)
            switch_to_other();
        if (!lcd_fb_cross())
            bad++;
        lcd_fb_fence(r[0], r[1], r[2], r[3]);
        p = lcd_fb_read(r[0], r[1]);
        for (y = 0; y < r[3]; y++)
            for (x = 0; x < r[2]; x++)
                if (ld(p + y * FW + x) != px_val(g, (r[1] + y) * FW + r[0] + x))
                    bad++;
        pushed = g;
    }
}

/* ---- B: vplayer's overlay --------------------------------------------- */
static void b_cop(void)
{
    int g, x, y;

    lcd_fb_claim();
    claimed = 1;
    for (g = 1; g <= ROUNDS; g++)
    {
        const int *r = ra[g];
        for (y = r[1]; y < r[1] + r[3]; y++)
            for (x = r[0]; x < r[0] + r[2]; x++)
                st(&cview[y * FW + x], px_val(g, y * FW + x));
        lcd_fb_publish(r[0], r[1], r[2], r[3]);
        drawn = g;                  /* queue_post(VQ_OVERLAY) */
        while (pushed != g)         /* sleep(1) until ovl_done */
            switch_to_other();
    }
}

static void b_cpu(void)
{
    int g, x, y;

    while (!claimed)
        switch_to_other();
    for (g = 1; g <= ROUNDS; g++)
    {
        const int *r = ra[g];
        const uint32_t *p;

        while (drawn != g)
            switch_to_other();
        p = lcd_fb_cross() ? lcd_fb_read(r[0], r[1])
                           : (const uint32_t *)cview + r[1] * FW + r[0];
        for (y = 0; y < r[3]; y++)
            for (x = 0; x < r[2]; x++)
                if (ld(p + y * FW + x) != px_val(g, (r[1] + y) * FW + r[0] + x))
                    bad++;
        pushed = g;
    }
}

/* ---- C: power_off ------------------------------------------------------ */
#define CLEAR 0xC1EAu
#define CLR_N FW        /* one row: the boot frame's lower rows stay cached */
static void c_cop(void)
{
    int i;

    while (!drawn)                  /* the boot frame is the CPU's */
        switch_to_other();
    lcd_fb_claim();
    claimed = 1;
    for (i = 0; i < MEMW; i++)
        st(&cview[i], px_val(2, i));
    /* be_call(sys_poweroff): write back, post, block */
    if (!(omit & O_BECALL_WB))
        cache_ops(C_COMMIT);
    cop_in_call = 1;
    while (!pushed)
        switch_to_other();
    /* the worst case: whatever the COP still holds dirty lands now */
    cache_ops(C_COMMIT);
}

static void c_cpu(void)
{
    int i;
    const uint32_t *p;

    /* the boot frame, drawn on the CPU before the ui thread; the CPU's
     * write-back at create_thread() leaves its lines clean but valid */
    for (i = 0; i < MEMW; i++)
        st(&cview[i], px_val(1, i));
    cache_ops(C_COMMIT);
    drawn = 1;
    while (!cop_in_call)
        switch_to_other();
    lcd_fb_take();
    if (lcd_fb_cross())
        bad++;
    for (i = 0; i < CLR_N; i++)     /* the clear: the top row here */
        st(&cview[i], CLEAR);
    p = lcd_fb_read(0, 0);          /* the push */
    for (i = 0; i < MEMW; i++)
        if (ld(p + i) != (i < CLR_N ? CLEAR : px_val(2, i)))
            bad++;
    cache_ops(C_COMMIT);            /* e2port_start's write-back */
    pushed = 1;
    while (alive[1])
        switch_to_other();
    for (i = 0; i < MEMW; i++)      /* what the panel keeps */
        if (mem[i] != (i < CLR_N ? CLEAR : px_val(2, i)))
            bad++;
}

/* ---- D: the fence times out (a modal loop on the ui thread) ------------
 * The COP draws gen 1 and enters a modal loop (the USB screen, a prompt)
 * that keeps drawing gen 2 but never serves the fence.  The CPU's push
 * times out, is counted, and reads uncached: every pixel is one the COP
 * wrote (gen 1 or 2) or not yet written back (0), never anything else,
 * and nothing hangs.  The modal loop ends and the COP serves fences
 * again; the next push is exact. */
static void d_cop(void)
{
    int i;

    lcd_fb_claim();
    claimed = 1;
    for (i = 0; i < MEMW; i++)
        st(&cview[i], px_val(1, i));
    drawn = 1;
    for (i = 0; i < MEMW; i++)      /* the modal loop: no fence served */
        st(&cview[i], px_val(2, i));
    while (pushed != 1)
        switch_to_other();
    f_req = f_done = 0;             /* the stale request, dropped */
    drawn = 2;                      /* back at its queue */
    while (pushed != 2)
    {
        cop_serve();
        switch_to_other();
    }
}

static void d_cpu(void)
{
    int i, r;
    const uint32_t *p;

    while (drawn != 1)
        switch_to_other();
    r = lcd_fb_fence(0, 0, FW, FH);
    if (r != FE_FB_TIMEOUT && r != FE_FB_PUBLISHED)
        bad++;
    if (r == FE_FB_TIMEOUT && lcd_fb_fence_fails != 1)
        bad++;
    p = lcd_fb_read(0, 0);
    for (i = 0; i < MEMW; i++)
    {
        uint32_t v = ld(p + i);
        if (v != 0 && v != px_val(1, i) && v != px_val(2, i))
            bad++;                  /* corrupted: never allowed */
    }
    pushed = 1;
    while (drawn != 2)
        switch_to_other();
    if (lcd_fb_fence(0, 0, FW, FH) != FE_FB_PUBLISHED)
        bad++;
    p = lcd_fb_read(0, 0);
    for (i = 0; i < MEMW; i++)
        if (ld(p + i) != px_val(2, i))
            bad++;
    pushed = 2;
}

/* ---- the runner -------------------------------------------------------- */
typedef void (*side_fn)(void);
struct run { side_fn fn; int core; };

static void *thread_main(void *arg)
{
    struct run *r = arg;
    pthread_mutex_lock(&mx);
    me = r->core;
    while (turn != me)
        pthread_cond_wait(&cv, &mx);
    r->fn();
    alive[me] = 0;
    turn = !me;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mx);
    return NULL;
}

static int run2(unsigned seed, side_fn cpu, side_fn cop, unsigned o)
{
    pthread_t t0, t1;
    struct run r0 = { cpu, 0 }, r1 = { cop, 1 };
    int g;

    memset(mem, 0, sizeof(mem));
    memset(cview, 0, sizeof(cview));
    memset(cache, 0, sizeof(cache));
    rng = seed * 2654435761u + 1;
    for (g = 0; g < 8; g++)         /* random rects, some full width */
    {
        int *r = ra[g];
        if (rnd() % 3 == 0)
        {
            r[0] = 0; r[2] = FW;
        }
        else
        {
            r[0] = rnd() % FW;
            r[2] = 1 + rnd() % (FW - r[0]);
        }
        r[1] = rnd() % FH;
        r[3] = 1 + rnd() % (FH - r[1]);
    }
    omit = o;
    lfx_core = 0;
    claimed = drawn = pushed = bad = 0;
    f_req = f_done = cop_in_call = timeouts = 0;
    lcd_fb_fence_fails = 0;
    turn = (int)(seed & 1);
    alive[0] = alive[1] = 1;
    pthread_create(&t0, NULL, thread_main, &r0);
    pthread_create(&t1, NULL, thread_main, &r1);
    pthread_join(t0, NULL);
    pthread_join(t1, NULL);
    return bad;
}

#define SEEDS 300
static int failures;

/* correct: no stale pixel on any seed; a negative control: caught */
static void check(const char *name, side_fn cpu, side_fn cop, unsigned o)
{
    unsigned s;
    int caught = 0;

    for (s = 0; s < SEEDS; s++)
        if (run2(s, cpu, cop, o))
            caught++;
    if (!o)
    {
        if (caught)
        {
            printf("FAIL %-40s stale pixels on %d/%d seeds\n", name, caught,
                   SEEDS);
            failures++;
        }
        else
            printf("ok   %-40s clean over %d seeds\n", name, SEEDS);
    }
    else if (!caught)
    {
        printf("FAIL %-40s omission never caught\n", name);
        failures++;
    }
    else
        printf("ok   %-40s caught %d/%d\n", name, caught, SEEDS);
}

/* lcd_fb_lines: every rect's lines whole, covering it, inside the frame */
static void check_lines(void)
{
    int x, y, w, h, bad_lines = 0;
    const uintptr_t fb = (uintptr_t)cview, end = fb + sizeof(cview);

    for (y = 0; y < FH; y++)
        for (h = 1; y + h <= FH; h++)
            for (x = 0; x < FW; x++)
                for (w = 1; x + w <= FW; w++)
                {
                    uintptr_t a, first, last;
                    uint32_t n;

                    lcd_fb_lines(x, y, w, h, &a, &n);
                    first = fb + (uintptr_t)(y * FW + x) * 4;
                    last = fb + (uintptr_t)((y + h - 1) * FW + x + w) * 4;
                    if ((a | n) % XCORE_LINE || a > first || a + n < last ||
                        a < fb || a + n > end)
                        bad_lines++;
                    if (x == 0 && w == FW && (a != first || a + n != last))
                        bad_lines++;    /* full rows: exact */
                }
    {   /* a rect past every edge: clipped to the whole frame */
        uintptr_t a;
        uint32_t n;
        lcd_fb_lines(-3, -2, FW + 9, FH + 9, &a, &n);
        if (a != fb || a + n != end)
            bad_lines++;
        lcd_fb_lines(FW, 0, 4, 4, &a, &n);      /* nothing left */
        if (n)
            bad_lines++;
    }
    /* the device's rows: 320 RGB565 pixels on whole 16-byte lines */
    if ((320 * 2) % XCORE_LINE)
        bad_lines++;
    if (bad_lines)
    {
        printf("FAIL lcd_fb_lines: %d bad rects\n", bad_lines);
        failures++;
    }
    else
        printf("ok   lcd_fb_lines (every rect: whole lines, covered, "
               "inside; full rows exact)\n");
}

int main(void)
{
    me = 0;
    check_lines();

    check("A cpu push (wake frame)", a_cpu, a_cop, 0);
    check("A  - no write-back in the fence", a_cpu, a_cop, O_FENCE_WB);
    check("A  - no fence", a_cpu, a_cop, O_NO_FENCE);
    check("A  - cached read", a_cpu, a_cop, O_CACHED);

    check("B overlay (publish, then post)", b_cpu, b_cop, 0);
    check("B  - no publish", b_cpu, b_cop, O_PUB_COP);
    check("B  - cached read", b_cpu, b_cop, O_CACHED);

    check("C take (power_off's shape)", c_cpu, c_cop, 0);
    check("C  - be_call without write-back", c_cpu, c_cop, O_BECALL_WB);
    check("C  - take without the drop", c_cpu, c_cop, O_ACQ);

    check("D fence timeout (modal loop)", d_cpu, d_cop, 0);
    {   /* the timeout path was taken, and counted, on most seeds */
        unsigned sd;
        int to = 0;
        for (sd = 0; sd < SEEDS; sd++)
        {
            run2(sd, d_cpu, d_cop, 0);
            to += timeouts > 0;
        }
        if (to < SEEDS / 2)
        {
            printf("FAIL D timeout path taken on only %d/%d seeds\n", to,
                   SEEDS);
            failures++;
        }
        else
            printf("ok   %-40s timeout on %d/%d seeds\n",
                   "D  (the timeout path)", to, SEEDS);
    }
    check("D  - cached read", d_cpu, d_cop, O_CACHED);

    return failures ? 1 : 0;
}
