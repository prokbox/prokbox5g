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
 * iPod UI shell: the developer power log's ring and CSV formatting, pure
 * logic with no Rockbox calls (tracker #10; docs/phase9-brief.md section
 * 3.6.5). apps/ipodui/powerlog.c fills it from a timeout callback and
 * writes it out at storage idle; tests/powerlog/ checks it on the host.
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
#ifndef _IPODUI_POWERLOG_CORE_H_
#define _IPODUI_POWERLOG_CORE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One sample, 32 bytes. The *_pct fields are the share of the 4 Hz
 * sub-samples in the row's interval that saw the state; the rest is the
 * state at the row's tick. Counters are cumulative and wrap at 65536.
 * The Fit-1 M0 fields (after mark) cover the row's interval: CPU time
 * from switch_thread(), storage commands from the ATA driver
 * (firmware/export/devstats.h); PL_UNKNOWN where the build has no
 * measurement (the simulator's CPU time). */
struct pl_row {
    uint32_t secs;          /* current_tick / HZ */
    uint16_t mv;            /* battery_voltage() */
    uint16_t dac_sleeps;
    uint16_t dac_wakes;
    uint16_t dropped;       /* rows lost before this one (saturates) */
    uint8_t pct;            /* battery_level(), 0..100 (255: unknown) */
    uint8_t flags;          /* PL_F_* */
    uint8_t boost_pct;      /* CPU boosted */
    uint8_t bl_pct;         /* backlight lit */
    uint8_t lcd_pct;        /* LCD awake */
    uint8_t disk_pct;       /* storage_disk_is_active() */
    int8_t vol;             /* volume, dB */
    uint8_t mark;           /* 0, or the mark's id (1..255) */
    /* Fit-1 M0 */
    uint8_t idle_pct;       /* CPU core asleep in core_sleep() */
    uint8_t cop_idle_pct;   /* the COP, the same */
    uint8_t cpu1_slot;      /* thread slot with the most CPU time ... */
    uint8_t cpu1_pct;       /* ... and its share of the interval */
    uint8_t cpu2_slot;      /* the runner-up */
    uint8_t cpu2_pct;
    uint8_t disk_by;        /* slot that issued the most disk commands */
    uint8_t disk_wakes;     /* commands that found the disk asleep */
    uint16_t disk_cmds;     /* disk commands (saturates) */
    uint16_t disk_kb;       /* KB moved (saturates at 65535) */
    /* #114: the storage adapter's power (firmware/export/stor_pwr.h) */
    uint8_t stor_on_pct;    /* powered (on or standby) */
    uint8_t stor_ons;       /* power-ons in the interval (saturates) */
    uint16_t stor_ons_total;    /* power-ons since boot (saturates) */
};

#define PL_UNKNOWN      0xff    /* a pct or slot field with no measurement */

#define PL_F_CHARGER    0x01    /* charger present */
#define PL_F_CHARGING   0x02
#define PL_F_PLAYING    0x04
#define PL_F_PAUSED     0x08
#define PL_F_DAC_SB     0x10    /* the codec is in standby */
#define PL_F_DAC_ALLOW  0x20    /* the developer standby switch is on */

#define PL_ROWS         192     /* 6,912 B: 96 min at 30 s, 16 min at 5 s */
#define PL_MARKS        8
#define PL_TAG_LEN      22      /* with the NUL */

struct pl_mark {
    uint16_t no;            /* serial; the row's id is no % 255 + 1 */
    char tag[PL_TAG_LEN];
};

/* The whole log state: 7,116 B (the budget line), a core_alloc while on */
struct pl_ring {
    struct pl_row rows[PL_ROWS];
    struct pl_mark marks[PL_MARKS];
    uint32_t head;          /* rows ever pushed */
    uint32_t tail;          /* rows ever popped (written out or lost) */
    uint16_t dropped;       /* rows overwritten before their flush */
    uint16_t mark_no;       /* marks ever made */
};

/* The CSV's first line, with its newline */
extern const char pl_csv_header[];

void pl_ring_reset(struct pl_ring *r);
static inline uint32_t pl_ring_pending(const struct pl_ring *r)
{
    return r->head - r->tail;
}
/* Appends a row; a full ring loses its oldest (counted in dropped, which
 * the next pushed row carries). Never yields: the caller masks IRQs. */
void pl_ring_push(struct pl_ring *r, const struct pl_row *row);
/* Stores tag (commas, quotes and control characters become '_', cut to
 * PL_TAG_LEN - 1) and returns the id to put in the row's mark field */
uint8_t pl_ring_mark(struct pl_ring *r, const char *tag);
/* The tag for a row's mark id, "" for none, "?" if its slot was reused */
const char *pl_ring_tag(const struct pl_ring *r, uint8_t id);
/* One CSV line with its newline into buf; returns its length (< size).
 * Lines are at most PL_LINE_MAX bytes. names: the thread names of the
 * row's cpu1_slot, cpu2_slot and disk_by, in that order (NULL entries,
 * or a NULL array, print "-"); commas in them become '_'. */
#define PL_LINE_MAX     192
#define PL_NAME_LEN     16      /* with the NUL */
int pl_format_row(char *buf, size_t size, const struct pl_row *row,
                  const char *tag, const char *const names[3]);

/* The 4 Hz sub-sample accumulator for the row being built */
struct pl_acc {
    uint16_t n, boost, bl, lcd, disk, stor;
};
static inline uint8_t pl_acc_pct(uint16_t k, uint16_t n)
{
    return n ? (uint8_t)((k * 100u + n / 2) / n) : 0;
}

#endif /* _IPODUI_POWERLOG_CORE_H_ */
