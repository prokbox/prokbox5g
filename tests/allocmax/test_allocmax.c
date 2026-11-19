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

/* Host test (0.9.0 road-test crash 2026-11-17, "audio_reset_buffer(): OOM!"):
 * buflib_allocatable() must report only what one allocation can really get,
 * so that buflib_alloc_maximum() never fails while memory is free.
 *
 * hole_before_pinned: a pinned block cut from the audio block's tail (#170
 *   reserve take: the language buffer on a switch while playing) leaves,
 *   once the audio block is freed, a hole in front of it.  The hole must
 *   also hold the new block's header.
 * pinned_splits_holes: a pinned block between two holes is a wall, as an
 *   unmovable one is; the two holes are never one.
 *
 * Built twice by run.sh: with the developer build's 5-word header
 * (HAVE_BUFLIB_HEAPCHECK) and the release build's 4-word one; EXPECT_FIELDS
 * says which, so a wrong stub can't pass for the other. */
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "buflib.h"

#ifndef EXPECT_FIELDS
#error "EXPECT_FIELDS (the header's word count) must be given"
#endif

static int fails;
#define CHECK(c) do { if (!(c)) { \
    printf("  line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static jmp_buf panic_jmp;
void panicf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    longjmp(panic_jmp, 1);
}

static union buflib_data pool[65536 / sizeof(union buflib_data)];
static struct buflib_context ctx;
static size_t reserve_left;

/* playback.c's #170 path: cut the request off the block's end in place */
static int shrink_cb(int handle, unsigned hints, void *start, size_t old)
{
    size_t take = ((hints & BUFLIB_SHRINK_SIZE_MASK) + 3) & ~(size_t)3;
    if (take && take <= reserve_left && take < old) {
        buflib_shrink(&ctx, handle, start, old - take);
        reserve_left -= take;
        return BUFLIB_CB_OK;
    }
    return BUFLIB_CB_CANNOT_SHRINK;
}
static struct buflib_callbacks audio_ops = { .shrink_callback = shrink_cb };
/* no move and no shrink callback: unmovable (buflib_ops_locked) */
static struct buflib_callbacks locked_ops;

/* the block alloc_maximum hands out is really ours: fill it, and the
 * blocks named in keep[] are untouched */
static void check_max(const char *what, int *keep, int nkeep)
{
    size_t size = 0, allocatable = buflib_allocatable(&ctx);
    int h = buflib_alloc_maximum(&ctx, &size, &audio_ops);
    printf("  %s: alloc_maximum handle=%d size=%zu (allocatable was %zu)\n",
           what, h, size, allocatable);
    CHECK(h > 0);
    CHECK(size > 0);
    if (h > 0) {
        unsigned char *p = buflib_get_data(&ctx, h);
        for (int i = 0; i < nkeep; i++) {
            unsigned char *k = buflib_get_data(&ctx, keep[i]);
            CHECK(k != NULL);
            CHECK(k[0] == 0x5a);
            CHECK(p + size <= k || k + 1 <= p);
        }
        memset(p, 0xa5, size);
        for (int i = 0; i < nkeep; i++)
            CHECK(((unsigned char *)buflib_get_data(&ctx, keep[i]))[0] == 0x5a);
    }
}

static void mark(int h)
{
    if (h > 0)
        *(unsigned char *)buflib_get_data(&ctx, h) = 0x5a;
}

static void hole_before_pinned(void)
{
    size_t size = 0;
    int a, lang, other;

    buflib_init(&ctx, pool, sizeof(pool));
    other = buflib_alloc(&ctx, 1000);           /* the shell's own blocks */
    CHECK(other > 0);
    a = buflib_alloc_maximum(&ctx, &size, &audio_ops);   /* audio */
    CHECK(a > 0);
    reserve_left = 8192;
    lang = buflib_alloc(&ctx, 3000);            /* the language block */
    CHECK(lang > 0);
    buflib_pin(&ctx, lang);                     /* "pinned for good" */
    mark(other);
    mark(lang);
    CHECK((char *)buflib_get_data(&ctx, lang)
          > (char *)buflib_get_data(&ctx, a));  /* it sits past audio */
    printf("  hole_before_pinned: audio=%zu, lang cut off its end, "
           "reserve left=%zu\n", size, reserve_left);
    buflib_free(&ctx, a);                       /* audio_reset_buffer() */
    int keep[] = { other, lang };
    check_max("hole_before_pinned", keep, 2);
}

static void pinned_splits_holes(void)
{
    int x, p, y, fill;

    buflib_init(&ctx, pool, sizeof(pool));
    x = buflib_alloc(&ctx, 20000);
    p = buflib_alloc(&ctx, 1000);
    y = buflib_alloc(&ctx, 20000);
    CHECK(x > 0 && p > 0 && y > 0);
    buflib_pin(&ctx, p);
    /* an unmovable block takes the rest but the room kept for handles,
     * so neither hole can be reached through the end */
    size_t rest = buflib_allocatable(&ctx);
    fill = buflib_alloc_ex(&ctx, rest, &locked_ops);
    CHECK(fill > 0);
    mark(p);
    mark(fill);
    buflib_free(&ctx, x);
    buflib_free(&ctx, y);
    printf("  pinned_splits_holes: two ~20000-byte holes either side of a "
           "pinned block, an unmovable block of %zu after\n", rest);
    int keep[] = { p, fill };
    check_max("pinned_splits_holes", keep, 2);
}

int main(void)
{
    printf("  header: %d words (expected %d)\n", (int)BUFLIB_NUM_FIELDS,
           EXPECT_FIELDS);
    if (BUFLIB_NUM_FIELDS != EXPECT_FIELDS) {
        printf("FAIL: built with the wrong header\n");
        return 1;
    }
    int ran = 0;
    if (setjmp(panic_jmp) == 0) {
        hole_before_pinned();
        ran++;
    } else {
        printf("  hole_before_pinned: panic\n");
        fails++;
    }
    if (setjmp(panic_jmp) == 0) {
        pinned_splits_holes();
        ran++;
    } else {
        printf("  pinned_splits_holes: panic\n");
        fails++;
    }
    printf("  scenarios run: %d of 2\n", ran);
    if (ran != 2 && !fails)
        fails++;
    if (fails)
        printf("FAIL: %d checks\n", fails);
    else
        printf("ok\n");
    return fails ? 1 : 0;
}
