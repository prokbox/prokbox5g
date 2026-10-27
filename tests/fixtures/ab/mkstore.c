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

/* Phase 6 fixture: writes a full books.dat (ab_store.h layout) with the
 * real record/header sealing from apps/ipodui/ab_store_core.c, for the
 * eviction script (docs/phase6-brief.md Rev 1 item 11).
 *
 *   mkstore <out> <count> <finished>
 *
 * count records with sorted, distinct keys 0x00a00000 + 3*i; the first
 * <finished> (the oldest stamps 1..finished) are FINISHED, the rest STARTED
 * (stamps follow). The header is marked seeded so the tagcache migration
 * does not run. Prints the key of record 0 (the oldest FINISHED, or with
 * <finished> 0 the oldest STARTED) on stdout: the key the first eviction
 * names. Built
 * by make_ab_fixtures.py. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ab_store.h"

int main(int argc, char **argv)
{
    int count, fin, i, sectors;
    unsigned char *buf;
    struct ab_store_hdr h;
    FILE *f;

    if (argc < 4)
        return 2;
    count = atoi(argv[2]);
    fin = atoi(argv[3]);
    if (count < 1 || count > AB_STORE_MAX)
        return 2;
    sectors = ab_store_sectors(count);
    buf = calloc(sectors + 2, AB_STORE_SECTOR);
    if (!buf)
        return 1;
    for (i = 0; i < count; i++)
    {
        struct ab_rec r;

        memset(&r, 0, sizeof(r));
        r.book_key = 0x00a00000u + 3u * (unsigned)i;
        r.file_crc = 0x1234u + (unsigned)i;
        r.file_idx = 0;
        r.stamp = (uint32_t)(i + 1);
        if (i < fin)
            r.flags = AB_REC_F_FINISHED;
        else
        {
            r.flags = AB_REC_F_STARTED;
            r.elapsed_ms = 5000u + (unsigned)i;
            r.book_ms = r.elapsed_ms;
        }
        ab_rec_seal(&r);
        memcpy(buf + (size_t)ab_store_rec_sector(i) * AB_STORE_SECTOR
               + (size_t)(i % AB_STORE_PER_SECT) * sizeof(r),
               &r, sizeof(r));
    }
    memset(&h, 0, sizeof(h));
    h.magic = AB_STORE_MAGIC;
    h.version = AB_STORE_VERSION;
    h.rec_size = sizeof(struct ab_rec);
    h.seq = 1;
    h.count = (uint32_t)count;
    h.sectors = (uint32_t)sectors;
    h.clock = (uint32_t)count + 1;
    h.seeded = 1;
    ab_store_hdr_seal(&h);
    memcpy(buf, &h, sizeof(h));
    memcpy(buf + (size_t)(sectors + 1) * AB_STORE_SECTOR, &h, sizeof(h));
    f = fopen(argv[1], "wb");
    if (!f || fwrite(buf, AB_STORE_SECTOR, (size_t)sectors + 2, f)
              != (size_t)sectors + 2)
        return 1;
    fclose(f);
    printf("a00000\n");       /* record 0: the oldest of its class */
    return 0;
}
