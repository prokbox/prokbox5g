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
 * firmware/common/disk_cache.c's defensive checks (int4-dcache, #355).
 *
 * The cache is #included so the test can do what a stale cache-line
 * write-back does on the device (quirks.md #88): put back a map bit the
 * cache had cleared.  Cases:
 *   stale-buf    a map bit restored for an entry dc_get_buffer() handed
 *                out: the probe must miss, not return that buffer as the
 *                sector's (and the peek must not return it)
 *   stale-free   the same for an entry dc_discard_buf() freed
 *   dirty-below  dc_dirty_buf() a pointer just below or past the buffers:
 *                no entry turns dirty, counted (developer builds: panic)
 *   dirty-free   dc_dirty_buf() a freed entry's buffer: not dirtied,
 *                counted (developer builds: panic)
 *   dirty-inner  dc_dirty_buf() a pointer inside an in-use entry (fat.c
 *                does this for directory entries): dirty, no complaint
 * With -DHAVE_IPOD_UI_DEVMODE the bad dirty marks must panic instead.
 * -DDCTEST_CONTROL builds against a cache without the counter (the
 * negative control in run.sh: it must fail).
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
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include DCTEST_SOURCE   /* firmware/common/disk_cache.c (or the control) */

static jmp_buf panic_jmp;
static int panic_armed;
static int panics;

void panicf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "  panicf: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    panics++;
    if (panic_armed)
        longjmp(panic_jmp, 1);
    exit(3);
}

void dc_writeback_callback(IF_MV(int volume,) sector_t sector, void *buf)
{
    (void)sector; (void)buf;
}

bool dc_writeback_first(IF_MV(int volume,) sector_t sector)
{
    (void)sector;
    return false;
}

static int failures;

#define CHECK(c, what) do { if (!(c)) { \
    printf("FAIL %s: %s\n", name, (what)); failures++; return; } } while (0)

#ifdef DCTEST_CONTROL
#define BAD_REFS() 0u
#else
#define BAD_REFS() dc_bad_refs
#endif

static void reset(void)
{
    memset(cache_entry, 0, sizeof cache_entry);
    memset(cache_map_entry, 0, sizeof cache_map_entry);
    memset(cache_vol_map, 0, sizeof cache_vol_map);
#ifndef DCTEST_CONTROL
    dc_bad_refs = 0;
#endif
    panics = 0;
    dc_init();
}

static unsigned int idx_of(void *buf)
{
    return ((uint8_t *)buf - &cache_buffer[0][0]) / DC_CACHE_BUFSIZE;
}

/* what a stale line write-back of the map word does: the bit is back */
static void restore_bit(sector_t sector, unsigned int index)
{
    cache_map_set_bit(&CACHE_MAP_ENTRY(0, map_sector(sector)), index);
}

/* a dirty mark the cache must refuse: 1 if it panicked */
static int bad_dirty(void *buf)
{
    panic_armed = 1;
    if (setjmp(panic_jmp)) {
        panic_armed = 0;
        return 1;
    }
    dc_dirty_buf(buf);
    panic_armed = 0;
    return 0;
}

static int any_dirty(void)
{
    for (unsigned int i = 0; i < DC_NUM_ENTRIES; i++)
        if (cache_entry[i].flags & DCE_DIRTY)
            return 1;
    return 0;
}

static void case_stale_buf(void)
{
    const char *name = "stale-buf";
    unsigned int flags;
    const sector_t s = 1234;

    reset();
    void *b = dc_cache_probe(IF_MV(0,) s, &flags);
    CHECK(flags == 0, "first probe hit");
    CHECK(dc_cache_probe(IF_MV(0,) s, &flags) == b && flags == DCE_INUSE,
          "second probe missed");
    /* another sector becomes the MRU (the cache keeps that one back);
     * take buffers until the one holding s is handed out */
    dc_cache_probe(IF_MV(0,) s + 1, &flags);
    void *g = NULL;
    for (int n = 0; n < DC_NUM_ENTRIES && g != b; n++)
        g = dc_get_buffer();
    CHECK(g == b, "dc_get_buffer never handed out the entry");
    CHECK(dc_peek(IF_MV(0,) s) == NULL, "peek found a handed-out buffer");
    restore_bit(s, idx_of(b));
    void *p = dc_cache_probe(IF_MV(0,) s, &flags);
    CHECK(p != b, "probe returned a buffer handed out by dc_get_buffer()");
    CHECK(flags == 0, "probe claimed a hit on a handed-out buffer");
    CHECK(dc_peek(IF_MV(0,) s) == p, "peek missed the refilled entry");
    CHECK(BAD_REFS() >= 1, "not counted");
    printf("ok   %s (bad refs %u)\n", name, BAD_REFS());
}

static void case_stale_free(void)
{
    const char *name = "stale-free";
    unsigned int flags;
    const sector_t s = 77;

    reset();
    void *b = dc_cache_probe(IF_MV(0,) s, &flags);
    dc_discard_buf(b);
    restore_bit(s, idx_of(b));
    CHECK(dc_peek(IF_MV(0,) s) == NULL, "peek found a freed entry");
    dc_cache_probe(IF_MV(0,) s, &flags);
    CHECK(flags == 0, "probe claimed a hit on a freed entry");
    CHECK(BAD_REFS() >= 1, "not counted");
    printf("ok   %s (bad refs %u)\n", name, BAD_REFS());
}

static void case_dirty_below(void)
{
    const char *name = "dirty-below";
    unsigned int flags;

    reset();
    /* the first probe fills the LRU, entry 0 */
    void *b = dc_cache_probe(IF_MV(0,) 5, &flags);
    CHECK(idx_of(b) == 0, "first entry is not entry 0");
    int p = bad_dirty((uint8_t *)&cache_buffer[0][0] - 1);
    CHECK(!any_dirty(), "a pointer below the cache dirtied an entry");
#ifdef HAVE_IPOD_UI_DEVMODE
    CHECK(p, "developer build: no panic");
#else
    CHECK(!p, "release build panicked");
#endif
    p = bad_dirty((uint8_t *)&cache_buffer[0][0] + sizeof cache_buffer);
    CHECK(!any_dirty(), "a pointer past the cache dirtied an entry");
    CHECK(BAD_REFS() == 2, "not counted twice");
    printf("ok   %s (bad refs %u)\n", name, BAD_REFS());
}

static void case_dirty_free(void)
{
    const char *name = "dirty-free";
    unsigned int flags;

    reset();
    void *b = dc_cache_probe(IF_MV(0,) 9, &flags);
    dc_discard_buf(b);
    int p = bad_dirty(b);
    CHECK(!any_dirty(), "a freed entry turned dirty");
#ifdef HAVE_IPOD_UI_DEVMODE
    CHECK(p, "developer build: no panic");
#else
    CHECK(!p, "release build panicked");
#endif
    CHECK(BAD_REFS() == 1, "not counted");
    printf("ok   %s (bad refs %u)\n", name, BAD_REFS());
}

static void case_dirty_inner(void)
{
    const char *name = "dirty-inner";
    unsigned int flags;

    reset();
    dc_cache_probe(IF_MV(0,) 3, &flags);
    void *b = dc_cache_probe(IF_MV(0,) 4, &flags);
    int p = bad_dirty((uint8_t *)b + DC_CACHE_BUFSIZE - 32);
    CHECK(!p, "panicked on a pointer inside an in-use entry");
    CHECK(cache_entry[idx_of(b)].flags & DCE_DIRTY, "entry not dirty");
    CHECK(BAD_REFS() == 0, "counted a good mark");
    CHECK(panics == 0, "panicked");
    printf("ok   %s\n", name);
}

int main(void)
{
    case_stale_buf();
    case_stale_free();
    case_dirty_below();
    case_dirty_free();
    case_dirty_inner();
    printf("%s: %d failure(s), 5 cases, %d entries of %d B\n",
           failures ? "FAILED" : "PASSED", failures, DC_NUM_ENTRIES,
           DC_CACHE_BUFSIZE);
    return failures ? 1 : 0;
}
