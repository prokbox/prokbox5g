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
 * Host test for the USB prefetch cache bookkeeping (#81,
 * firmware/usbstack/usb_storage_pf.h). It drives the real upf_* functions
 * the way usb_storage.c does, with the worker's steps (plan, the read in
 * flight, done) interleaved at random between the host's commands and the
 * USB's pieces, against a model disk, and checks every byte the host gets.
 *
 * What would break, and how it shows:
 * - a read in flight lands in the ring as garbage first (its DMA is not
 *   instant), so a sector sent from a range being read shows up;
 * - a piece is copied out of the ring only when its "USB transfer"
 *   completes, after more worker steps, so an overwrite of a pinned range
 *   shows up;
 * - WRITEs change the disk at random, so stale data shows up;
 * - the sector filter is an XOR applied once, at fill time (the firmware
 *   rewrites the layout sectors when the read lands), and once on the
 *   ordinary path: twice or never shows up.
 *
 *   cc -I. tests/usb/pf_test.c -o pf_test && ./pf_test
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
#include "firmware/usbstack/usb_storage_pf.h"

#define BS      16                      /* small sectors keep it quick */
#define NSECT   50000
#define CAP     1024                    /* ring, sectors */
#define CHUNK   128                     /* largest prefetch read */
#define PIECE   64                      /* one USB transfer, sectors */

static unsigned char disk[NSECT * BS];
static unsigned char ring[CAP * BS];
static struct upf pf;
static unsigned long hits, pendings, misses, fills, waits, fails, drops;
static unsigned long errors;

/* the worker */
static int w_state;                     /* 0 idle, 1 read in flight */
static uint64_t w_start;
static uint32_t w_n, w_pos, w_gen;
static int fail_next;                   /* make the next read fail */
/* demand batching (usb_storage.c pf_sync): no worker in the background;
 * the USB thread reads the next chunk itself, only between pieces */
static int sync_mode;
static unsigned long sync_cmds;         /* storage reads issued, sync mode */

static unsigned char xf(uint64_t s, unsigned i)
{
    return (unsigned char)(0x5a ^ s ^ i);
}

static void worker_step(void)
{
    if (w_state == 0) {
        w_n = upf_plan(&pf, &w_start, &w_pos, &w_gen);
        if (!w_n)
            return;
        if (w_pos + w_n > CAP) {
            printf("FAIL: read wraps the ring\n");
            errors++;
        }
        memset(ring + w_pos * BS, 0xee, w_n * BS);  /* DMA under way */
        w_state = 1;
        return;
    }
    bool ok = !fail_next;
    fail_next = 0;
    if (ok) {
        memcpy(ring + w_pos * BS, disk + w_start * BS, w_n * BS);
        for (uint32_t k = 0; k < w_n; k++)
            for (unsigned i = 0; i < BS; i++)
                ring[(w_pos + k) * BS + i] ^= xf(w_start + k, i);
    } else
        fails++;
    if (upf_done(&pf, w_gen, w_start, w_n, ok))
        fills++;
    w_state = 0;
}

static void worker_some(void)
{
    if (sync_mode)
        return;                 /* nothing runs behind the host's back */
    int k = rand() % 4;
    while (k--)
        worker_step();
}

static void check(uint64_t s, const unsigned char *got, const char *how)
{
    for (unsigned i = 0; i < BS; i++) {
        unsigned char want = disk[s * BS + i] ^ xf(s, i);
        if (got[i] != want) {
            if (errors++ < 10)
                printf("FAIL: %s sector %llu byte %u: %02x want %02x\n",
                       how, (unsigned long long)s, i, got[i], want);
            return;
        }
    }
}

/* usb_storage.c's READ, condensed: from the ring piece by piece, waiting
 * for the worker when a piece is pending; else the ordinary path */
static void host_read(uint64_t s, uint32_t n)
{
    int r = upf_lookup(&pf, 0, s, n);

    if (r == UPF_PENDING && !upf_is_seq(&pf, 0, s))
        r = UPF_MISS;   /* usb_storage.c: never wait for a scattered READ */
    upf_note_read(&pf, 0, s, n, r != UPF_MISS, NSECT);
    if (r == UPF_MISS) {
        misses++;
        unsigned char tmp[BS];
        for (uint32_t k = 0; k < n; k++) {
            memcpy(tmp, disk + (s + k) * BS, BS);
            for (unsigned i = 0; i < BS; i++)
                tmp[i] ^= xf(s + k, i);
            check(s + k, tmp, "miss");
        }
        worker_some();
        return;
    }
    if (r == UPF_HIT)
        hits++;
    else
        pendings++;
    while (n) {
        uint32_t piece = n < PIECE ? n : PIECE;
        uint32_t p = upf_pos(&pf, s);
        if (piece > CAP - p)
            piece = CAP - p;
        int tries = 0;
        for (;;) {
            int q = upf_lookup(&pf, 0, s, piece);
            if (q == UPF_HIT)
                break;
            if (q == UPF_MISS) {
fallback:
                /* the fallback: read the piece the ordinary way */
                unsigned char tmp[BS];
                for (uint32_t k = 0; k < piece; k++) {
                    memcpy(tmp, disk + (s + k) * BS, BS);
                    for (unsigned i = 0; i < BS; i++)
                        tmp[i] ^= xf(s + k, i);
                    check(s + k, tmp, "fallback");
                }
                goto next;
            }
            upf_pin(&pf, s);
            if (sync_mode) {
                /* usb_storage.c pf_wait(), pf_sync: plan, read, land, in
                 * line; no piece is on the wire while it runs */
                worker_step();
                if (w_state == 0)
                    goto fallback;      /* nothing planned: the plain read */
                sync_cmds++;
                worker_step();
            } else
                worker_step();
            waits++;
            if (++tries > 100000) {
                printf("FAIL: pending never arrives at %llu\n",
                       (unsigned long long)s);
                errors++;
                return;
            }
        }
        upf_pin(&pf, s);
        worker_some();          /* the transfer is on the wire */
        for (uint32_t k = 0; k < piece; k++)
            check(s + k, ring + ((p + k) & (CAP - 1)) * BS, "ring");
next:
        s += piece;
        n -= piece;
    }
    worker_some();
}

static void host_write(uint64_t s, uint32_t n)
{
    upf_drop(&pf);              /* the command hook, before any data */
    drops++;
    worker_some();              /* a read in flight may land meanwhile */
    for (uint32_t k = 0; k < n * BS; k++)
        disk[s * BS + k] = (unsigned char)rand();
    worker_some();
}

static int suite(void)
{
    srand(81);
    hits = pendings = misses = fills = waits = fails = drops = 0;
    w_state = 0;
    fail_next = 0;
    for (unsigned i = 0; i < sizeof(disk); i++)
        disk[i] = (unsigned char)rand();
    upf_init(&pf, CAP, CHUNK);

    /* 1: a FAT scan, ~12 KB READs (24 sectors) straight through */
    uint64_t s = 1000;
    while (s + 24 < 40000) {
        host_read(s, 24);
        s += 24;
    }
    printf("scan: hits=%lu pending=%lu misses=%lu fills=%lu waits=%lu\n",
           hits, pendings, misses, fills, waits);
    if (sync_mode) {
        /* the point of it: one storage read serves many 12 KB READs */
        unsigned long reads = (40000 - 1000) / 24;
        printf("sync scan: %lu READs, %lu storage reads (%.1f per read)\n",
               reads, sync_cmds + misses, (double)reads / (sync_cmds + misses));
        if (sync_cmds + misses > reads / 4) {
            printf("FAIL: demand batching should batch the scan\n");
            errors++;
        }
    }
    if (misses > 2) {
        printf("FAIL: a sequential scan should miss only at its start\n");
        errors++;
    }

    /* 1b: random mode. Scattered READs stop the read-ahead (no plan) and
     * never wait; a sequential run brings it back. */
    {
        upf_drop(&pf);
        unsigned long w0 = waits, f0 = fills;
        uint64_t at[] = { 9000, 100, 20000, 4000, 31000, 700, 15000, 2500 };
        for (unsigned i = 0; i < sizeof(at) / sizeof(at[0]); i++) {
            host_read(at[i], 8);
            for (int k = 0; k < 8 && !sync_mode; k++)
                worker_step();
            if (i >= UPF_RANDOM_N && !upf_random(&pf)) {
                printf("FAIL: not random after %u scattered READs\n", i + 1);
                errors++;
            }
        }
        {
            uint64_t st;
            uint32_t ps, g;
            if (upf_random(&pf) && w_state == 0 &&
                upf_plan(&pf, &st, &ps, &g)) {
                printf("FAIL: read-ahead planned in random mode\n");
                errors++;
            }
        }
        if (waits != w0 || fills != f0) {
            printf("FAIL: random mode waited (%lu) or filled (%lu)\n",
                   waits - w0, fills - f0);
            errors++;
        }
        /* the run resumes: sequential READs restart the stream */
        host_read(5000, 24);
        host_read(5024, 24);
        host_read(5048, 24);
        if (upf_random(&pf)) {
            printf("FAIL: still random after a sequential run\n");
            errors++;
        }
        for (int k = 0; k < 8 && !sync_mode; k++)
            worker_step();
        if (sync_mode)
            host_read(5072, 24);        /* the demand read fills it */
        if (fills == f0) {
            printf("FAIL: no read-ahead after the run resumed\n");
            errors++;
        }
        printf("random: ok (waits %lu, fills %lu)\n", waits - w0,
               fills - f0);
    }

    /* 2: random mix: sequential runs, jumps, re-reads, writes, failures */
    for (int it = 0; it < 200000; it++) {
        int op = rand() % 100;
        uint32_t n = 1 + rand() % (rand() % 8 ? 32 : CAP);
        if (op < 70) {
            if (s + n >= NSECT)
                s = rand() % 1000;
            host_read(s, n);
            s += n;
        } else if (op < 80) {
            s = rand() % (NSECT - CAP);
            host_read(s, n);
            s += n;
        } else if (op < 85) {
            uint64_t back = rand() % 2000;
            s = s > back ? s - back : 0;
            host_read(s, n);
            s += n;
        } else if (op < 95) {
            uint64_t w = rand() % 3 ? s + rand() % 300 : rand() % NSECT;
            if (n > 64)
                n = 64;
            if (w + n > NSECT)
                w = NSECT - n;
            host_write(w, n);
        } else {
            fail_next = 1;
            worker_some();
        }
        if (sync_mode && w_state != 0) {
            printf("FAIL: a read left in flight in sync mode\n");
            errors++;
        }
    }
    /* 3: reads up to the disk's end */
    for (s = NSECT - 2000; s + 7 <= NSECT; s += 7)
        host_read(s, 7);

    printf("all: hits=%lu pending=%lu misses=%lu fills=%lu waits=%lu "
           "drops=%lu fails=%lu\n", hits, pendings, misses, fills, waits,
           drops, fails);
    return errors ? 1 : 0;
}

int main(void)
{
    int rc = suite();
    printf("-- demand batching (no worker) --\n");
    sync_mode = 1;
    memset(&pf, 0, sizeof(pf));
    rc |= suite();
    if (errors) {
        printf("pf_test: %lu errors\n", errors);
        return 1;
    }
    printf("pf_test: ok (worker and demand batching)\n");
    return 0;
}
