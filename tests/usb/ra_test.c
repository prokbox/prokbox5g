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
 * Host test for the USB read-ahead bookkeeping (#63,
 * firmware/usbstack/usb_storage_ra.h). It drives the real ura_* functions
 * the way usb_storage.c does (READ: first chunk, then send_and_read_next's
 * ping-pong over two 64 KB halves, then the fill while the last chunk is on
 * the wire; WRITE: ura_write() first) against a model disk, and checks
 * every byte the host gets.
 *
 * The sector filter (usb_storage_xlate.c rewrites sectors in place before
 * they are sent) is modelled by an in-place XOR: a sector sent twice from
 * the prefetch, or a stale one, shows up as a mismatch.
 *
 *   cc -I. tests/usb/ra_test.c -o ra_test && ./ra_test
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
#include "firmware/usbstack/usb_storage_ra.h"

#define BS      512
#define CHUNK   128                     /* READ_BUFFER_SIZE / BS */
#define NSECT   20000

static unsigned char disk[NSECT * BS];
static unsigned char blk[2 * CHUNK * BS];
static unsigned char out[256 * 1024 / BS * BS * 8];
static struct ura ra;
static unsigned long hits, fills, disk_reads, fails;

static void disk_read(unsigned long s, unsigned n, unsigned char *buf)
{
    memcpy(buf, disk + s * BS, n * BS);
    disk_reads++;
}

static void filter(unsigned char *buf, unsigned n)
{
    for (unsigned i = 0; i < n * BS; i++)
        buf[i] ^= 0x5a;
}

/* usb_storage.c's READ path, condensed */
static void host_read(unsigned long s, unsigned c)
{
    unsigned char *data[2] = { blk, blk + CHUNK * BS };
    unsigned long s0 = s;
    int sel = 0, half = 0;
    uint32_t off = 0;
    unsigned got = 0;

    if (ura_read(&ra, 0, s, c, CHUNK, &half, &off)) {
        data[0] = blk + half * CHUNK * BS + off * BS;
        data[1] = blk + !half * CHUNK * BS;
        hits++;
    } else
        disk_read(s, c < CHUNK ? c : CHUNK, data[0]);

    for (;;) {
        unsigned char *sent = data[sel];
        unsigned m = c < CHUNK ? c : CHUNK;

        filter(sent, m);
        memcpy(out + got * BS, sent, m * BS);
        got += m;
        sel = !sel;
        s += CHUNK;
        c -= m;
        if (c) {
            disk_read(s, c < CHUNK ? c : CHUNK, data[sel]);
            continue;
        }
        uint64_t start;
        uint32_t n = ura_plan(&ra, NSECT, CHUNK, &start);
        if (n) {
            int other = sent - blk >= CHUNK * BS ? 0 : 1;
            disk_read(start, n, blk + other * CHUNK * BS);
            ura_filled(&ra, start, n, other);
            fills++;
        }
        break;
    }

    for (unsigned i = 0; i < got * BS; i++)
        if (out[i] != (disk[s0 * BS + i] ^ 0x5a)) {
            printf("FAIL read %lu+%u: byte %u wrong\n", s0, got, i);
            fails++;
            return;
        }
}

static void host_write(unsigned long s, unsigned c)
{
    ura_write(&ra);
    for (unsigned i = 0; i < c * BS; i++)
        disk[s * BS + i] = rand();
}

static unsigned rnd(unsigned n) { return (unsigned)rand() % n; }

int main(void)
{
    unsigned long s;
    int i;

    srand(63);
    for (i = 0; i < NSECT * BS; i++)
        disk[i] = rand();
    ura_reset(&ra);

    /* 1: macOS's FAT scan: sequential 24-sector (12 KB) READs */
    for (s = 100; s + 24 < 6000; s += 24)
        host_read(s, 24);
    printf("seq 12 KB: hits %lu fills %lu disk reads %lu\n", hits, fills,
           disk_reads);
    if (hits < 200) { printf("FAIL too few hits\n"); fails++; }

    /* 2: sequential 1 MB READs (2048 sectors): the prefetch is a full chunk */
    hits = fills = disk_reads = 0;
    for (s = 6000; s + 2048 < 16000; s += 2048)
        host_read(s, 2048);
    printf("seq 1 MB: hits %lu fills %lu disk reads %lu\n", hits, fills,
           disk_reads);
    if (hits < 2) { printf("FAIL long READs never used the prefetch\n"); fails++; }

    /* 3: writes into the prefetched range between sequential READs */
    hits = 0;
    for (s = 200; s + 16 < 4000; s += 16) {
        host_read(s, 16);
        if (rnd(4) == 0)
            host_write(s + 16 + rnd(64), 1 + rnd(8));   /* ahead, overlapping */
        if (rnd(8) == 0)
            host_write(rnd(NSECT - 300), 1 + rnd(256)); /* anywhere */
    }
    printf("seq with writes: hits %lu\n", hits);

    /* 4: random mix: sizes 1..300, sequential runs, rereads, writes */
    for (i = 0; i < 20000; i++) {
        unsigned c = 1 + rnd(300);
        int k = rnd(10);
        if (k < 5)
            s = ra.next < NSECT - 300 ? ra.next : 0;    /* continue */
        else if (k < 7)
            s = ra.next > c && ra.next < NSECT - 300 ?
                ra.next - c : 0;                        /* reread */
        else
            s = rnd(NSECT - 300);
        if (k == 9)
            host_write(rnd(NSECT - 300), 1 + rnd(300));
        else
            host_read(s, c);
    }

    /* 5: the end of the disk */
    for (s = NSECT - 100; s + 10 <= NSECT; s += 10)
        host_read(s, 10);

    printf("%s\n", fails ? "RA TEST FAILED" : "RA TEST PASSED");
    return fails ? 1 : 0;
}
