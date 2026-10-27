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
 * Host test for firmware/drivers/ata-common.c's partial-physical-sector
 * cache (#63): the real file is #included over a RAM disk, with 1024-byte
 * physical sectors (phys_sector_mult 2, as on the 5.5G once the 2048-byte
 * virtual sectors are mounted) and the host's 512-byte writes of the USB
 * 512-byte view. Without the fix, a full-sector write that bypasses the
 * cache leaves it stale and the next partial write restores old data.
 *
 *   cc -I. tests/usb/ata_cache_test.c -o ata_cache_test [-DHAVE_USB_SECTOR_XLATE]
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
#include <stdint.h>
#include <stdbool.h>

/* the few firmware definitions ata-common.c uses */
typedef uint64_t sector_t;
struct mutex { int x; };
#define SHAREDBSS_ATTR
#define STORAGE_ALIGN_ATTR
#define ATA_IDENTIFY_WORDS 256
#define MAX_PHYS_SECTOR_SIZE 1024
#define SECTOR_SIZE 512
#define IF_MD(x...)
#define IF_MD_NONVOID(x...) void
#define BIT_N(n) (1u << (n))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define DEBUGF(...)
#define panicf(...) (printf(__VA_ARGS__), exit(2))
static void mutex_lock(struct mutex *m) { (void)m; }
static void mutex_unlock(struct mutex *m) { (void)m; }

#define NSECT 64
static unsigned char disk[NSECT * 512];
static int ata_transfer_sectors(uint64_t start, int count, void *buf, int write)
{
    if (start + count > NSECT)
        return -1;
    if (write)
        memcpy(disk + start * 512, buf, count * 512);
    else
        memcpy(buf, disk + start * 512, count * 512);
    return 0;
}

#include "firmware/drivers/ata-common.c"

static void fill(unsigned char *b, int n, unsigned char v)
{
    memset(b, v, n * 512);
}

int main(void)
{
    unsigned char w[4 * 512], r[512];
    int fails = 0;

    log_sector_size = 512;
    phys_sector_mult = 1;
    ata_set_phys_sector_mult(4);        /* disk_mount with 2048 virtual */
    printf("phys_sector_mult %u\n", phys_sector_mult);

    /* the review's scenario at sectors 10/11 (one 1024-byte physical) */
    fill(w, 1, 0xA1); ata_write_sectors(11, 1, w);     /* FAT sector 11 */
    fill(w, 4, 0xB2); ata_write_sectors(10, 4, w);     /* a run over 10..13 */
    fill(w, 1, 0xC3); ata_write_sectors(11, 1, w);     /* 11 alone again */
    ata_read_sectors(10, 1, r);
    if (r[0] != 0xB2) { printf("FAIL sector 10 = %02x, want b2 (stale flush)\n", r[0]); fails++; }
    else printf("ok   sector 10 keeps the run's data\n");
    if (disk[11 * 512] != 0xC3) { printf("FAIL sector 11 not written\n"); fails++; }
    else printf("ok   sector 11 has the latest write\n");

    /* a stale partial read after a bypassing write */
    fill(w, 1, 0x11); ata_write_sectors(21, 1, w);     /* caches 20/21 */
    fill(w, 2, 0x22); ata_write_sectors(20, 2, w);     /* bypasses */
    ata_read_sectors(21, 1, r);
    if (r[0] != 0x22) { printf("FAIL partial read of 21 = %02x, want 22 (stale cache)\n", r[0]); fails++; }
    else printf("ok   partial read after a bypassing write is current\n");

    printf("%s\n", fails ? "ATA CACHE TEST FAILED" : "ATA CACHE TEST PASSED");
    return fails ? 1 : 0;
}
