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
 * Host test of buflib's heap walk (firmware/buflib_mempool.c, developer
 * builds' HAVE_BUFLIB_HEAPCHECK): good heaps pass, every corruption fails.
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
/* Sitting 4gh-b5's panic: a compaction's move_block() met a header whose
 * handle word read 0x023e6613.  Proves (1) the walk passes on heaps the
 * allocator built (allocations, frees, compactions, shrinks, a whole-buffer
 * shift), (2) it fails, with the right reason and the block before, on each
 * corruption (the panic's own, an out-of-table handle, zero and runaway
 * lengths, a moved back-pointer, an ops word only the CRC sees, an overrun
 * from the block before, a stray handle, the bound), and (3) buflib's panic
 * names the block before the bad one. */
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "buflib.h"

static int fails;
#define CHECK(c) do { if (!(c)) { \
    printf("  line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static jmp_buf panic_jmp;
static char panic_msg[256];
static int panic_armed;

void panicf(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(panic_msg, sizeof(panic_msg), fmt, ap);
    va_end(ap);
    if (panic_armed)
        longjmp(panic_jmp, 1);
    printf("  unexpected panic: %s\n", panic_msg);
    fails++;
    longjmp(panic_jmp, 2);
}

static union buflib_data pool[16384 / sizeof(union buflib_data)];
static struct buflib_context ctx;

static int move_cb(int handle, void *cur, void *new)
{
    (void)handle; (void)cur; (void)new;
    return BUFLIB_CB_OK;
}
static struct buflib_callbacks ops_mv = { .move_callback = move_cb };

static union buflib_data *hdr(int h)
{
    return _buflib_get_block_header(buflib_get_data(&ctx, h));
}

static int walk(struct buflib_walk *w)
{
    return buflib_walk(&ctx, w, 4096);
}

/* five blocks, the third with ops: A B C D E */
static int h[5];
static void setup(void)
{
    static const size_t sz[5] = { 100, 333, 64, 1000, 12 };
    int i;

    memset(pool, 0xEE, sizeof(pool));
    buflib_init(&ctx, pool, sizeof(pool));
    for (i = 0; i < 5; i++)
    {
        h[i] = buflib_alloc_ex(&ctx, sz[i], i == 2 ? &ops_mv : NULL);
        memset(buflib_get_data(&ctx, h[i]), 0x11 * (i + 1), sz[i]);
    }
}

static void t_good(void)
{
    struct buflib_walk w;
    size_t size = 0;
    int k, x;

    setup();
    CHECK(walk(&w) == 0 && w.blocks == 5 && w.used == 5 && w.handles == 5);
    /* a hole, then an allocation that fits it, then a compaction */
    buflib_free(&ctx, h[1]);
    CHECK(walk(&w) == 0 && w.blocks == 5 && w.used == 4 && w.handles == 4);
    x = buflib_alloc(&ctx, 40);
    CHECK(x > 0 && walk(&w) == 0 && w.used == 5);
    buflib_free(&ctx, h[3]);
    buflib_buffer_out(&ctx, &size);     /* compacts, then shifts it all */
    CHECK(walk(&w) == 0 && w.used == 4 && w.handles == 4);
    buflib_buffer_in(&ctx, (int)size);
    CHECK(walk(&w) == 0 && w.used == 4);
    /* shrinks from the front and the back: the CRC is sealed again */
    CHECK(buflib_shrink(&ctx, h[4], (char *)buflib_get_data(&ctx, h[4]) + 4,
                        8));
    CHECK(walk(&w) == 0);
    CHECK(buflib_shrink(&ctx, h[0], NULL, 20));
    CHECK(walk(&w) == 0);
    /* pins don't touch the CRC */
    buflib_pin(&ctx, h[2]);
    CHECK(walk(&w) == 0);
    buflib_unpin(&ctx, h[2]);
    /* many allocations and frees, the walk after each */
    for (k = 0; k < 200; k++)
    {
        int y = buflib_alloc(&ctx, (size_t)(k * 37 % 700) + 1);

        CHECK(walk(&w) == 0);
        if (y > 0 && k % 3)
            buflib_free(&ctx, y);
        CHECK(walk(&w) == 0);
    }
}

/* the block before C (B) is named for each corruption of C */
static void expect_prev_b(const struct buflib_walk *w,
                          const union buflib_data *c)
{
    CHECK(w->at == c);
    CHECK(w->prev == hdr(h[1]));
    CHECK(w->prev_handle == h[1]);
    CHECK(w->prev_len * (intptr_t)sizeof(union buflib_data) >= 333);
    CHECK(w->prev_ops == NULL && w->prev_move == NULL);
}

static void t_bad(void)
{
    struct buflib_walk w;
    union buflib_data *c, save;

    /* the panic's own: an unaligned handle word */
    setup();
    c = hdr(h[2]);
    c[BUFLIB_IDX_HANDLE].handle = (union buflib_data *)(uintptr_t)0x023e6613;
    CHECK(walk(&w) == BUFLIB_WALK_HANDLE_ALIGN);
    expect_prev_b(&w, c);
    CHECK(!strcmp(buflib_walk_why(w.bad), "hdlalign"));
    /* aligned but outside the table */
    setup();
    c = hdr(h[2]);
    c[BUFLIB_IDX_HANDLE].handle = c;
    CHECK(walk(&w) == BUFLIB_WALK_HANDLE_RANGE);
    expect_prev_b(&w, c);
    /* zero and runaway lengths */
    setup();
    c = hdr(h[2]);
    c[BUFLIB_IDX_LEN].val = 0;
    CHECK(walk(&w) == BUFLIB_WALK_LEN_ZERO);
    expect_prev_b(&w, c);
    setup();
    c = hdr(h[2]);
    c[BUFLIB_IDX_LEN].val = 1 << 20;
    CHECK(walk(&w) == BUFLIB_WALK_LEN_RANGE);
    setup();
    c = hdr(h[2]);
    c[BUFLIB_IDX_LEN].val = 2;
    CHECK(walk(&w) == BUFLIB_WALK_LEN_SHORT);
    /* the handle's data moved outside its block */
    setup();
    c = hdr(h[2]);
    ctx.handle_table[-h[2]].alloc += 4096;
    CHECK(walk(&w) == BUFLIB_WALK_BACKPTR);
    expect_prev_b(&w, c);
    /* the ops word: every other check passes, the CRC doesn't */
    setup();
    c = hdr(h[3]);
    c[BUFLIB_IDX_OPS].ops = &ops_mv;
    CHECK(walk(&w) == BUFLIB_WALK_CRC);
    CHECK(w.at == c && w.crc != w.want && w.prev == hdr(h[2]));
    CHECK(w.prev_handle == h[2] && w.prev_ops == &ops_mv &&
          w.prev_move == (const void *)move_cb);
    /* a length off by one unit, and still in range: the CRC */
    setup();
    c = hdr(h[3]);
    c[BUFLIB_IDX_LEN].val += 1;
    CHECK(walk(&w) != 0);
    /* an overrun: B's payload written 8 bytes past its end */
    setup();
    c = hdr(h[2]);
    save = c[0];
    memset((char *)c - 4, 0x23, 12);
    CHECK(walk(&w) != 0 && w.at == c && w.prev == hdr(h[1]));
    c[0] = save;
    /* a stray live handle with no block */
    setup();
    ctx.last_handle--;
    ctx.last_handle->alloc = (char *)pool;
    CHECK(walk(&w) == BUFLIB_WALK_HANDLES && w.used == 5 && w.handles == 6);
    /* the bound */
    setup();
    CHECK(buflib_walk(&ctx, &w, 2) == BUFLIB_WALK_LIMIT && w.blocks == 2);
    /* the first block: no block before it */
    setup();
    c = hdr(h[0]);
    c[BUFLIB_IDX_HANDLE].handle = (union buflib_data *)(uintptr_t)0x3;
    CHECK(walk(&w) == BUFLIB_WALK_HANDLE_ALIGN && w.prev == NULL);
}

/* buflib's own panic in a compaction names the block before */
static void t_panic(void)
{
    union buflib_data *c;
    size_t size = 0;
    char want[64];

    setup();
    buflib_free(&ctx, h[1]);            /* a hole before C: C moves */
    c = hdr(h[2]);
    c[BUFLIB_IDX_HANDLE].handle = (union buflib_data *)(uintptr_t)0x023e6613;
    panic_armed = 1;
    panic_msg[0] = '\0';
    if (!setjmp(panic_jmp))
    {
        buflib_buffer_out(&ctx, &size);     /* compacts: move_block(C) */
        CHECK(!"no panic");
    }
    panic_armed = 0;
    printf("  panic: %s\n", panic_msg);
    CHECK(strstr(panic_msg, "handle unaligned") != NULL);
    snprintf(want, sizeof(want), "prev %p", (void *)hdr(h[0]));
    CHECK(strstr(panic_msg, "prev ") != NULL);
    /* C's block before is A, then the freed B merged behind it: B's
       space is one free block after A, so prev is that free block */
    CHECK(strstr(panic_msg, "h=0") != NULL || strstr(panic_msg, want));
    CHECK(strlen(panic_msg) < 120);     /* the panic record's msg */
}

int main(void)
{
    if (setjmp(panic_jmp))
    {
        printf("FAIL: heap walk (a panic)\n");
        return 1;
    }
    t_good();
    t_bad();
    t_panic();
    if (fails)
    {
        printf("FAIL: heap walk, %d checks\n", fails);
        return 1;
    }
    return 0;
}
