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
 * iPod UI shell: the no-change boot stamp, pure half (Phase 16-2, #89)
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
#include <string.h>
#include "bootstamp.h"

/* the stamp is one 32 B record; its CRC covers the 28 bytes before it */
typedef char boot_stamp_size_check[sizeof(struct boot_stamp) == 32 ? 1 : -1];

/* CRC-32 (reflected, 0xEDB88320), a nibble table: 64 B of const, two
 * lookups a byte. The digest hashes ~30 B per directory entry, about
 * 110 KB for the device's 3,801 entries: a few ms on the PP5022. */
static const uint32_t crc_nib[16] = {
    0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac,
    0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
    0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
    0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
};

uint32_t boot_crc32(uint32_t crc, const void *p, size_t n)
{
    const unsigned char *b = p;

    crc = ~crc;
    while (n--)
    {
        crc ^= *b++;
        crc = (crc >> 4) ^ crc_nib[crc & 15];
        crc = (crc >> 4) ^ crc_nib[crc & 15];
    }
    return ~crc;
}

void boot_stamp_seal(struct boot_stamp *s)
{
    s->magic = BOOT_STAMP_MAGIC;
    s->version = BOOT_STAMP_VERSION;
    s->crc = boot_crc32(0, s, offsetof(struct boot_stamp, crc));
}

bool boot_stamp_check(const void *buf, size_t len, struct boot_stamp *out)
{
    struct boot_stamp s;

    if (!buf || len != sizeof(s))
        return false;
    memcpy(&s, buf, sizeof(s));
    if (s.magic != BOOT_STAMP_MAGIC || s.version != BOOT_STAMP_VERSION
        || s.crc != boot_crc32(0, &s, offsetof(struct boot_stamp, crc)))
        return false;
    if (out)
        *out = s;
    return true;
}

enum boot_why boot_stamp_precheck(const struct boot_stamp *s, bool have,
                                  uint32_t generation)
{
    if (!have || !s)
        return BOOT_WHY_NOSTAMP;
    if (!(s->flags & BOOT_STAMP_CLEAN))
        return BOOT_WHY_UNCLEAN;
    /* walk, then WALK_EVERY - 1 skips: this boot would be the next skip */
    if (s->boots_skipped + 1u >= (unsigned)BOOT_STAMP_WALK_EVERY)
        return BOOT_WHY_TENTH;
    if (s->db_generation != generation)
        return BOOT_WHY_GEN;
    return BOOT_WHY_TRUST;
}

enum boot_why boot_stamp_verdict(enum boot_digest_state st)
{
    switch (st)
    {
        case BOOT_DIGEST_MATCH: return BOOT_WHY_TRUST;
        case BOOT_DIGEST_SLOW:  return BOOT_WHY_SLOW;  /* walk */
        case BOOT_DIGEST_DIFF:  return BOOT_WHY_DIGEST;
        default:                return BOOT_WHY_DIGEST_FAIL;
    }
}

const char *boot_why_name(enum boot_why w)
{
    static const char *const names[BOOT_WHY__N] = {
        "trust", "slow", "nostamp", "torn", "unclean", "tenth",
        "gen", "digest", "digest_fail", "usb", "db", "days",
    };
    return (unsigned)w < BOOT_WHY__N ? names[w] : "?";
}

void boot_digest_begin(struct boot_digest *d)
{
    d->crc = 0;
    d->dirs = d->entries = 0;
}

static void put32(unsigned char *p, uint32_t v)
{
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

void boot_digest_entry(struct boot_digest *d, const char *name, bool dir,
                       uint32_t size, uint32_t mtime, uint32_t cluster)
{
    unsigned char rec[13];

    /* the name with its terminator, so "ab"+"c" never equals "a"+"bc" */
    d->crc = boot_crc32(d->crc, name, strlen(name) + 1);
    rec[0] = dir ? 'D' : 'F';
    put32(rec + 1, dir ? 0 : size);
    put32(rec + 5, mtime);
    put32(rec + 9, cluster);
    d->crc = boot_crc32(d->crc, rec, sizeof(rec));
    d->entries++;
}

void boot_digest_leave(struct boot_digest *d)
{
    static const unsigned char end[2] = { 0, 'E' };  /* never a name */

    d->crc = boot_crc32(d->crc, end, sizeof(end));
    d->dirs++;
}
