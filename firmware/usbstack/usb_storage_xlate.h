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
 * USB mass storage: present a disk laid out in 2048-byte virtual sectors
 * (the iPod 5.5G's MBR and FAT32 boot sector) to the host as an ordinary
 * 512-byte-sector disk, without changing anything on the disk.
 *
 * Only the sectors that state the layout differ between the two views: the
 * MBR's partition entries and each FAT32 partition's boot sector and its
 * backup. Every other byte sits at the same offset in both. On the way out
 * those sectors are rewritten for 512-byte sectors; on the way in they are
 * rewritten back, and a write that would change the layout is refused.
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
#ifndef USB_STORAGE_XLATE_H
#define USB_STORAGE_XLATE_H

#include <stdint.h>
#include <stdbool.h>

#define XLATE_SECTOR    512
#define XLATE_MAX_PARTS 4

/* One FAT32 partition's layout sectors, in 512-byte host sectors */
struct xlate_part {
    uint64_t start;         /* 0: not a FAT32 partition we translate */
    uint64_t backup;        /* the backup boot sector; 0: none */
};

struct xlate_state {
    unsigned int mult;      /* virtual sector / 512; < 2: inactive */
    struct xlate_part part[XLATE_MAX_PARTS];
    unsigned char mbr_parts[64];    /* the partition entries, as stored */
};

/* Learning the layout (the disk's own sectors, as stored). xlate_learn_mbr()
 * sets mult (the caller knows it from the FAT driver) and the partitions'
 * starts; then xlate_learn_boot() for each partition's boot sector. Either
 * may leave the state inactive: an unusual layout is exported as stored. */
void xlate_learn_mbr(struct xlate_state *s, unsigned int mult,
                     const unsigned char *mbr);
void xlate_learn_boot(struct xlate_state *s, int part,
                      const unsigned char *boot);

static inline bool xlate_active(const struct xlate_state *s)
{
    return s->mult > 1;
}

/* Host sector 'lba' as read from the disk, in place: the host's view */
void xlate_read(const struct xlate_state *s, uint64_t lba, unsigned char *buf);

/* Host sector 'lba' as sent by the host, in place: the disk's view.
 * Returns -1 (write nothing) if the host changed the layout. */
int xlate_write(const struct xlate_state *s, uint64_t lba, unsigned char *buf);

/* A run of 'count' host sectors starting at 'lba' */
void xlate_read_run(const struct xlate_state *s, uint64_t lba,
                    unsigned int count, unsigned char *buf);
int xlate_write_run(const struct xlate_state *s, uint64_t lba,
                    unsigned int count, unsigned char *buf);

#endif /* USB_STORAGE_XLATE_H */
