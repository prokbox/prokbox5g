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
 * Host harness for firmware/usbstack/usb_storage_xlate.c (#63).
 *
 *   xlate_tool view <disk.img> <view.img> <mult>
 *       the host's 512-byte view of a disk stored in <mult>*512-byte
 *       virtual sectors: every sector through xlate_read(), as USB reads
 *   xlate_tool back <disk.img> <view.img> <mult>
 *       a view written back to the disk: every sector through
 *       xlate_write(), as USB writes; exits 3 if a write is refused
 *   xlate_tool selftest <disk.img> <mult>
 *       round trip and refusal cases on the learned layout
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
#include "usb_storage_xlate.h"

#define S XLATE_SECTOR

static int learn(struct xlate_state *st, FILE *f, unsigned mult)
{
    unsigned char b[S];
    if (fseek(f, 0, SEEK_SET) || fread(b, 1, S, f) != S)
        return -1;
    xlate_learn_mbr(st, mult, b);
    for (int i = 0; i < XLATE_MAX_PARTS && xlate_active(st); i++)
    {
        if (!st->part[i].start)
            continue;
        if (fseeko(f, (off_t)st->part[i].start * S, SEEK_SET)
            || fread(b, 1, S, f) != S)
            return -1;
        xlate_learn_boot(st, i, b);
    }
    return xlate_active(st) ? 0 : -1;
}

static int copy(const struct xlate_state *st, FILE *in, FILE *out, int back)
{
    static unsigned char b[64 * S];
    uint64_t lba = 0;
    size_t n;

    fseek(in, 0, SEEK_SET);
    fseek(out, 0, SEEK_SET);
    while ((n = fread(b, 1, sizeof(b), in)) > 0)
    {
        unsigned cnt = n / S;
        if (back)
        {
            if (xlate_write_run(st, lba, cnt, b))
            {
                fprintf(stderr, "refused write near lba %llu\n",
                        (unsigned long long)lba);
                return 3;
            }
        }
        else
            xlate_read_run(st, lba, cnt, b);
        fwrite(b, 1, n, out);
        lba += cnt;
    }
    return 0;
}

static int fails = 0;
#define CHECK(c, msg) do { if (c) printf("ok   %s\n", msg); \
    else { printf("FAIL %s\n", msg); fails++; } } while (0)

static int selftest(FILE *f, unsigned mult)
{
    struct xlate_state st;
    unsigned char disk[S], view[S], back[S];

    CHECK(learn(&st, f, mult) == 0, "layout learned");
    uint64_t lbas[3] = { 0, st.part[0].start, st.part[0].backup };
    const char *names[3] = { "mbr", "boot", "backup boot" };
    for (int k = 0; k < 3; k++)
    {
        char msg[80];
        fseeko(f, (off_t)lbas[k] * S, SEEK_SET);
        fread(disk, 1, S, f);
        memcpy(view, disk, S);
        xlate_read(&st, lbas[k], view);
        snprintf(msg, sizeof(msg), "%s differs in the host view", names[k]);
        CHECK(memcmp(view, disk, S) != 0, msg);
        memcpy(back, view, S);
        snprintf(msg, sizeof(msg), "%s written back is accepted", names[k]);
        CHECK(xlate_write(&st, lbas[k], back) == 0, msg);
        snprintf(msg, sizeof(msg), "%s round trip is exact", names[k]);
        CHECK(memcmp(back, disk, S) == 0, msg);
        if (k > 0)
        {
            memcpy(back, view, S);
            back[0x0b] = 0x00; back[0x0c] = 0x10;   /* 4096 B/sector */
            snprintf(msg, sizeof(msg), "%s with a new sector size refused",
                     names[k]);
            CHECK(xlate_write(&st, lbas[k], back) != 0, msg);
            memcpy(back, view, S);
            back[0x0e] += 1;                        /* reserved + 1 */
            snprintf(msg, sizeof(msg), "%s with an odd reserved count refused",
                     names[k]);
            CHECK(xlate_write(&st, lbas[k], back) != 0, msg);
            memcpy(back, view, S);
            memcpy(back + 0x47, "NEWLABEL   ", 11);  /* a label change */
            snprintf(msg, sizeof(msg), "%s label change accepted", names[k]);
            CHECK(xlate_write(&st, lbas[k], back) == 0
                  && back[0x0b] == disk[0x0b] && back[0x0c] == disk[0x0c],
                  msg);
        }
        else
        {
            memcpy(back, view, S);
            back[0x1be + 8] += 1;                   /* start + 1 */
            CHECK(xlate_write(&st, 0, back) != 0,
                  "mbr with a start not divisible by mult refused");
            memcpy(back, view, S);
            back[0x1be + 8] += mult;                /* a new start */
            CHECK(xlate_write(&st, 0, back) != 0,
                  "mbr with a moved partition refused");
            memcpy(back, view, S);
            back[0x1b8] ^= 0xff;                    /* the disk signature */
            CHECK(xlate_write(&st, 0, back) == 0,
                  "mbr with a new disk signature accepted");
        }
    }
    unsigned char data[S], d2[S];
    fseeko(f, (off_t)(st.part[0].start + 4 * mult + 7) * S, SEEK_SET);
    fread(data, 1, S, f);
    memcpy(d2, data, S);
    xlate_read(&st, st.part[0].start + 4 * mult + 7, d2);
    CHECK(memcmp(d2, data, S) == 0, "other sectors pass through unchanged");
    printf("%s\n", fails ? "SELFTEST FAILED" : "selftest passed");
    return fails ? 1 : 0;
}

int main(int argc, char **argv)
{
    struct xlate_state st;

    if (argc == 4 && !strcmp(argv[1], "selftest"))
    {
        FILE *f = fopen(argv[2], "rb");
        return f ? selftest(f, atoi(argv[3])) : 2;
    }
    if (argc != 5 || (strcmp(argv[1], "view") && strcmp(argv[1], "back")))
    {
        fprintf(stderr, "usage: %s view|back disk.img view.img mult\n"
                "       %s selftest disk.img mult\n", argv[0], argv[0]);
        return 2;
    }
    int back = !strcmp(argv[1], "back");
    FILE *disk = fopen(argv[2], back ? "r+b" : "rb");
    FILE *view = fopen(argv[3], back ? "rb" : "wb");
    if (!disk || !view || learn(&st, disk, atoi(argv[4])))
    {
        fprintf(stderr, "cannot open or learn the layout\n");
        return 2;
    }
    return back ? copy(&st, view, disk, 1) : copy(&st, disk, view, 0);
}
