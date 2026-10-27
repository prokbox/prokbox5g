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
 * USB mass storage: 2048-byte virtual sectors shown as 512-byte sectors
 * (see usb_storage_xlate.h). Pure logic on sector buffers, so the host
 * tests (tests/usb/xlate_tool.c, xlate_mount_test.sh) run exactly this file.
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
#include "usb_storage_xlate.h"

/* MBR */
#define MBR_PART0       0x1be
#define MBR_ENTRY       16
#define MBR_TYPE        4
#define MBR_START       8
#define MBR_SIZE        12

/* FAT32 boot sector */
#define BPB_BYTES       0x0b    /* u16 */
#define BPB_SPC         0x0d    /* u8 */
#define BPB_RESERVED    0x0e    /* u16 */
#define BPB_ROOTENT     0x11    /* u16, 0 on FAT32 */
#define BPB_TOTSEC16    0x13    /* u16 */
#define BPB_FATSZ16     0x16    /* u16, 0 on FAT32 */
#define BPB_HIDDEN      0x1c    /* u32 */
#define BPB_TOTSEC32    0x20    /* u32 */
#define BPB_FATSZ32     0x24    /* u32 */
#define BPB_FSINFO      0x30    /* u16 */
#define BPB_BKBOOT      0x32    /* u16 */

static uint32_t rd16(const unsigned char *p)
{
    return p[0] | (uint32_t)p[1] << 8;
}

static uint32_t rd32(const unsigned char *p)
{
    return rd16(p) | rd16(p + 2) << 16;
}

static void wr16(unsigned char *p, uint32_t v)
{
    p[0] = v;
    p[1] = v >> 8;
}

static void wr32(unsigned char *p, uint32_t v)
{
    wr16(p, v);
    wr16(p + 2, v >> 16);
}

static bool has_sig(const unsigned char *b)
{
    return b[510] == 0x55 && b[511] == 0xaa;
}

/* The layout fields of one sector as stored, and as the host sees them.
 * A field is scaled by mult, or (bytes per sector) set to 512. */
struct field {
    unsigned char off, wide;    /* wide: 4 bytes, else 2 */
};

static const struct field bpb_scaled[] = {
    { BPB_RESERVED, 0 }, { BPB_TOTSEC16, 0 }, { BPB_HIDDEN, 1 },
    { BPB_TOTSEC32, 1 }, { BPB_FATSZ32, 1 }, { BPB_FSINFO, 0 },
    { BPB_BKBOOT, 0 },
};
#define N_SCALED (sizeof(bpb_scaled) / sizeof(bpb_scaled[0]))

static uint32_t get(const unsigned char *b, const struct field *f)
{
    return f->wide ? rd32(b + f->off) : rd16(b + f->off);
}

static void put(unsigned char *b, const struct field *f, uint32_t v)
{
    if (f->wide)
        wr32(b + f->off, v);
    else
        wr16(b + f->off, v);
}

static bool fits(const struct field *f, uint64_t v)
{
    return v <= (f->wide ? 0xffffffffu : 0xffffu);
}

void xlate_learn_mbr(struct xlate_state *s, unsigned int mult,
                     const unsigned char *mbr)
{
    memset(s, 0, sizeof(*s));
    if (mult < 2 || (mult & (mult - 1)) || !has_sig(mbr))
        return;

    for (int i = 0; i < XLATE_MAX_PARTS; i++)
    {
        const unsigned char *e = mbr + MBR_PART0 + i * MBR_ENTRY;
        uint64_t start = (uint64_t)rd32(e + MBR_START) * mult;
        uint64_t end = start + (uint64_t)rd32(e + MBR_SIZE) * mult;

        if (end > 0xffffffffu)
            return;                 /* past 2 TiB in 512s: not shown */
        if (e[MBR_TYPE] == 0x0b || e[MBR_TYPE] == 0x0c)
            s->part[i].start = start;
    }
    memcpy(s->mbr_parts, mbr + MBR_PART0, sizeof(s->mbr_parts));
    s->mult = mult;
}

/* A FAT32 boot sector in the disk's own sector size, whose scaled fields
 * all fit the 512-byte view */
static bool boot_ok(const struct xlate_state *s, const unsigned char *b)
{
    unsigned int spc = b[BPB_SPC] * s->mult;

    if (!has_sig(b) || rd16(b + BPB_BYTES) != XLATE_SECTOR * s->mult
        || rd16(b + BPB_ROOTENT) != 0 || rd16(b + BPB_FATSZ16) != 0
        || b[BPB_SPC] == 0 || (b[BPB_SPC] & (b[BPB_SPC] - 1)) || spc > 128)
        return false;
    for (unsigned int i = 0; i < N_SCALED; i++)
    {
        uint32_t v = get(b, &bpb_scaled[i]);
        /* 0xffff in a sector-number field means "none": kept as is */
        if (!fits(&bpb_scaled[i], (uint64_t)v * s->mult)
            && !(!bpb_scaled[i].wide && v == 0xffff))
            return false;
    }
    return true;
}

void xlate_learn_boot(struct xlate_state *s, int part,
                      const unsigned char *boot)
{
    struct xlate_part *p = &s->part[part];
    uint32_t bk;

    if (!xlate_active(s) || !p->start)
        return;
    if (!boot_ok(s, boot))
    {
        s->mult = 0;                /* export the disk as stored */
        return;
    }
    bk = rd16(boot + BPB_BKBOOT);
    p->backup = (bk && bk != 0xffff) ? p->start + (uint64_t)bk * s->mult : 0;
}

static uint32_t scale_field(const struct xlate_state *s,
                            const struct field *f, uint32_t v)
{
    return (!f->wide && v == 0xffff) ? v : v * s->mult;
}

static bool is_boot(const struct xlate_state *s, uint64_t lba)
{
    for (int i = 0; i < XLATE_MAX_PARTS; i++)
        if (s->part[i].start
            && (lba == s->part[i].start || lba == s->part[i].backup))
            return true;
    return false;
}

void xlate_read(const struct xlate_state *s, uint64_t lba, unsigned char *buf)
{
    if (!xlate_active(s))
        return;
    if (lba == 0 && has_sig(buf))
    {
        for (int i = 0; i < XLATE_MAX_PARTS; i++)
        {
            unsigned char *e = buf + MBR_PART0 + i * MBR_ENTRY;
            wr32(e + MBR_START, rd32(e + MBR_START) * s->mult);
            wr32(e + MBR_SIZE, rd32(e + MBR_SIZE) * s->mult);
        }
    }
    else if (is_boot(s, lba) && boot_ok(s, buf))
    {
        for (unsigned int i = 0; i < N_SCALED; i++)
            put(buf, &bpb_scaled[i],
                scale_field(s, &bpb_scaled[i], get(buf, &bpb_scaled[i])));
        buf[BPB_SPC] *= s->mult;
        wr16(buf + BPB_BYTES, XLATE_SECTOR);
    }
}

int xlate_write(const struct xlate_state *s, uint64_t lba, unsigned char *buf)
{
    if (!xlate_active(s))
        return 0;
    if (lba == 0)
    {
        if (!has_sig(buf))
            return -1;
        for (int i = 0; i < XLATE_MAX_PARTS; i++)
        {
            unsigned char *e = buf + MBR_PART0 + i * MBR_ENTRY;
            uint32_t start = rd32(e + MBR_START), size = rd32(e + MBR_SIZE);

            if (start % s->mult || size % s->mult)
                return -1;          /* not a layout the disk can store */
            wr32(e + MBR_START, start / s->mult);
            wr32(e + MBR_SIZE, size / s->mult);
        }
        /* the boot code and disk signature may change; the partitions
         * may not (a repartition or erase would leave the layout this
         * state knows, and the iPod's, behind) */
        if (memcmp(buf + MBR_PART0, s->mbr_parts, sizeof(s->mbr_parts)))
            return -1;
        return 0;
    }
    if (!is_boot(s, lba))
        return 0;
    /* a boot sector as the host sees it: 512 bytes per sector, scaled
     * fields that divide back exactly; anything else changes the layout */
    if (!has_sig(buf) || rd16(buf + BPB_BYTES) != XLATE_SECTOR
        || buf[BPB_SPC] % s->mult)
        return -1;
    for (unsigned int i = 0; i < N_SCALED; i++)
    {
        uint32_t v = get(buf, &bpb_scaled[i]);
        if (!bpb_scaled[i].wide && v == 0xffff)
            continue;
        if (v % s->mult)
            return -1;
        put(buf, &bpb_scaled[i], v / s->mult);
    }
    buf[BPB_SPC] /= s->mult;
    wr16(buf + BPB_BYTES, XLATE_SECTOR * s->mult);
    return boot_ok(s, buf) ? 0 : -1;
}

void xlate_read_run(const struct xlate_state *s, uint64_t lba,
                    unsigned int count, unsigned char *buf)
{
    if (!xlate_active(s))
        return;
    for (unsigned int i = 0; i < count; i++)
        xlate_read(s, lba + i, buf + i * XLATE_SECTOR);
}

int xlate_write_run(const struct xlate_state *s, uint64_t lba,
                    unsigned int count, unsigned char *buf)
{
    if (!xlate_active(s))
        return 0;
    for (unsigned int i = 0; i < count; i++)
        if (xlate_write(s, lba + i, buf + i * XLATE_SECTOR))
            return -1;
    return 0;
}
