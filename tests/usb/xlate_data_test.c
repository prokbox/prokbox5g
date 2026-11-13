/*
 * #314: xlate_run_in_data(), which tells a host WRITE that may change a
 * folder or a file (it reaches a FAT32 partition's data area) from one
 * that cannot (the boot sectors, FSInfo, the FATs: the host's dirty flag
 * at mount and unmount; the firmware partition). The post-USB library
 * check is skipped only when no WRITE of the session can have changed a
 * file, so a wrong "no" here would leave new music out of the library:
 * every boundary is checked, plus the unknown cases that must say yes.
 *
 *   cc -I. tests/usb/xlate_data_test.c -o xlate_data_test && ./xlate_data_test
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
 */
#include <stdio.h>
#include <string.h>
#include "firmware/usbstack/usb_storage_xlate.c"

static int fails, checks;

static void check(const char *what, bool got, bool want)
{
    checks++;
    if (got != want)
    {
        fails++;
        printf("FAIL %s: got %d want %d\n", what, got, want);
    }
}

static void put32(unsigned char *p, uint32_t v)
{
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

/* An iPod-style disk in 2048-byte sectors (mult 4): the firmware
 * partition (type 0) at 1, the FAT32 one at 'fat_start' (disk sectors),
 * 32 reserved sectors, 'nfats' FATs of 'fatsz' sectors each */
static void make(struct xlate_state *s, uint32_t fat_start, uint32_t size,
                 uint32_t fatsz, unsigned nfats)
{
    unsigned char mbr[512], boot[2048];

    memset(mbr, 0, sizeof(mbr));
    mbr[510] = 0x55; mbr[511] = 0xaa;
    mbr[0x1be + 4] = 0x00;              /* the firmware partition */
    put32(mbr + 0x1be + 8, 1);
    put32(mbr + 0x1be + 12, fat_start - 1);
    mbr[0x1ce + 4] = 0x0b;              /* FAT32 */
    put32(mbr + 0x1ce + 8, fat_start);
    put32(mbr + 0x1ce + 12, size);
    xlate_learn_mbr(s, 4, mbr);

    memset(boot, 0, sizeof(boot));
    boot[510] = 0x55; boot[511] = 0xaa;
    boot[0x0b] = 0x00; boot[0x0c] = 0x08;       /* 2048 bytes */
    boot[0x0d] = 8;                             /* sectors per cluster */
    boot[0x0e] = 32;                            /* reserved */
    boot[0x10] = nfats;
    put32(boot + 0x20, size);
    put32(boot + 0x24, fatsz);
    boot[0x30] = 1;                             /* FSInfo */
    boot[0x32] = 6;                             /* backup boot */
    xlate_learn_boot(s, 1, boot);
}

int main(void)
{
    struct xlate_state s;
    /* host sectors: the FAT partition at 1000 * 4 = 4000; data from
     * 4000 + (32 + 2 * 500) * 4 = 8128; the end at 4000 + 100000 * 4 */
    const uint64_t start = 4000, data = 8128, end = 404000;

    make(&s, 1000, 100000, 500, 2);
    check("active", xlate_active(&s), true);
    check("data learned (8128)", s.part[1].data == data, true);
    check("boot sector", xlate_run_in_data(&s, start, 1), false);
    check("FSInfo", xlate_run_in_data(&s, start + 4, 4), false);
    check("FAT1 dirty flag", xlate_run_in_data(&s, start + 128, 1), false);
    check("FAT2 last sector", xlate_run_in_data(&s, data - 1, 1), false);
    check("run ending at data", xlate_run_in_data(&s, data - 8, 8), false);
    check("run into data", xlate_run_in_data(&s, data - 8, 9), true);
    check("first data sector", xlate_run_in_data(&s, data, 1), true);
    check("a file", xlate_run_in_data(&s, 200000, 64), true);
    check("last sector", xlate_run_in_data(&s, end - 1, 1), true);
    check("past the end", xlate_run_in_data(&s, end, 8), false);
    check("firmware partition", xlate_run_in_data(&s, 8, 64), false);
    check("MBR", xlate_run_in_data(&s, 0, 1), false);

    /* unknown: says yes */
    make(&s, 1000, 100000, 500, 0);     /* no FATs: implausible */
    check("no FATs: unknown", xlate_run_in_data(&s, start, 1), true);
    make(&s, 1000, 100, 500, 2);        /* FATs past the end */
    check("FATs past end: unknown", xlate_run_in_data(&s, start, 1), true);
    memset(&s, 0, sizeof(s));
    check("inactive: unknown", xlate_run_in_data(&s, 0, 1), true);

    printf("%s xlate_data_test: %d checks, %d failed\n",
           fails ? "FAIL" : "ok  ", checks, fails);
    return fails != 0;
}
