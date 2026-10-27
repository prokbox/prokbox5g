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
 * iPod UI shell: automatic battery calibration, the pure part (tracker
 * #70; docs/batcal-brief.md). No Rockbox calls: sample packing and
 * decimation, the recorder's state machine as a function of one input per
 * second, the boot decision, the fit, its sanity limits, the blend of the
 * stored fits and both file formats. apps/ipodui/batcal.c feeds it from a
 * timeout and does the file I/O; tests/batcal/ checks it on the host.
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
#ifndef _IPODUI_BATCAL_CORE_H_
#define _IPODUI_BATCAL_CORE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BC_POINTS       11      /* 0 %, 10 %, ... 100 % */
#define BC_RING         512     /* samples: 1,024 B, the static limit */
#define BC_PERIOD0      60      /* s per sample at the start of a run */
#define BC_FULL_MV      4100    /* "full" latch: at least this ... */
#define BC_FULL_SECS    120     /* ... with charging done this long */
#define BC_END_MV       50      /* last sample this near disk-safe: ended */
#define BC_SKIP_RUN     600     /* s: surface charge at a run's start */
#define BC_SKIP_BOOT    300     /* s: rest recovery after each boot */
#define BC_BUSY_W16     24      /* busy charge weight, 1.5 in 16ths: an
                                 * estimate until measured (hardware.md) */
#define BC_RESERVE_PM   30      /* 0 % sits 3 % of the run before its end */
#define BC_MIN_SECS     (2 * 3600)  /* a complete run: on-time ... */
#define BC_MIN_FIT      120         /* ... fit samples ... */
#define BC_MAX_BUSY_PC  30          /* ... and busy share at most */
#define BC_FITS         3       /* fits kept in the learned-table file */

/* A sample: mV - 3000 in the top 12 bits, busy sixteenths in the low 4.
 * BC_SKIP in the mV field: recorded, but not a fit sample (its charge
 * weight still counts). */
#define BC_SKIP         0xfff
#define BC_PACK(mv12, busy)  ((uint16_t)(((mv12) << 4) | (busy)))
#define BC_MV12(s)      ((s) >> 4)
#define BC_BUSY(s)      ((s) & 15)

/* Run header flags */
#define BC_F_RUN        0x01    /* a run is open */
#define BC_F_ENDING     0x02    /* ended (shutoff, or plugged in at its end) */
#define BC_F_CLEAN      0x04    /* written by a clean shutdown */

#define BC_MAGIC        0x4c414342u     /* "BCAL" */
#define BC_VERSION      1

/* The run file: this header, n samples, then a CRC-32 (crc_32(), seed
 * 0xffffffff) over both. Native byte order: the device's own file. */
struct bc_hdr {
    uint32_t magic;
    uint8_t  version;
    uint8_t  flags;
    uint16_t boots;         /* boots into this run */
    uint32_t seq;           /* run sequence number */
    uint16_t period;        /* s per sample (60 << decimations) */
    uint16_t n;             /* samples */
    uint16_t last_mv;       /* the last sample's mean voltage, even a skip */
    uint16_t pad;
};

/* Recorder state: the header plus the accumulator (RAM only) */
struct bc_state {
    struct bc_hdr h;
    uint32_t acc_mv;        /* sum of the period's per-second voltages */
    uint16_t acc_n;         /* seconds accumulated */
    uint16_t acc_busy;      /* busy seconds */
    uint8_t  acc_disk;      /* the disk was active in the period */
    uint8_t  acc_skip;      /* a skip second fell in the period */
    uint8_t  was_power;     /* power was present at the last second */
    uint8_t  dirty;         /* changed since the last write */
    uint16_t full_secs;     /* continuous seconds of the "full" latch */
    uint16_t disksafe;      /* mV, battery_level_disksafe */
    uint32_t run_secs;      /* on-time since the run opened */
    uint32_t boot_secs;     /* since this boot */
    uint32_t next_seq;
};

/* One second's view of the hardware */
struct bc_in {
    uint16_t mv;            /* battery_voltage() */
    bool busy;              /* backlight lit, boosted or disk active */
    bool disk;              /* storage_disk_is_active() */
    bool power;             /* any power input present */
    bool charge_ok;         /* charger allowed >= 500 mA */
    bool charging;          /* charging_state() */
};

enum bc_event {
    BC_EV_NONE = 0,
    BC_EV_SAMPLE,           /* one sample stored */
    BC_EV_OPEN,             /* a run opened at unplug */
    BC_EV_PARTIAL,          /* power came: the run is discarded */
    BC_EV_ENDED,            /* power came at the run's end: kept, ending */
};

enum bc_boot {
    BC_BOOT_NONE = 0,       /* no run */
    BC_BOOT_KEEP,           /* a cleanly stopped run carries on */
    BC_BOOT_FIT,            /* a complete run: fit it */
    BC_BOOT_DROP,           /* crash, short, too busy: discard */
};

/* What a write chance (the disk spinning, UI thread) should do */
enum bc_write {
    BC_W_NONE = 0,          /* nothing changed, or below disk-safe */
    BC_W_RUN,               /* write the run file (temp + rename) */
    BC_W_REMOVE,            /* no run: remove the file */
};

/* One learned table and its provenance */
struct bc_fit {
    uint32_t seq;
    char date[11];          /* "YYYY-MM-DD" or "-" (clock not set) */
    uint16_t hours;         /* on-time of the run */
    uint16_t mv[BC_POINTS];
};

/* Recorder */
void bc_init(struct bc_state *st, uint16_t disksafe);
enum bc_event bc_tick(struct bc_state *st, uint16_t *ring,
                      const struct bc_in *in);
uint16_t bc_merge(uint16_t a, uint16_t b);
/* Never anything while !level_safe (battery_level_safe() false) */
enum bc_write bc_write_plan(const struct bc_state *st, bool level_safe,
                            bool file_exists);
/* Whether a write chance (disk up) is taken now: a run event, the
 * 5-min spacing, or the run within BC_END_MV of disk-safe (every later
 * chance may be the last one above it) */
bool bc_write_due(const struct bc_state *st, bool urgent, bool spaced);
enum bc_boot bc_boot_decide(const struct bc_hdr *h, const uint16_t *ring,
                            uint16_t disksafe);

/* Fit: the table from a complete run; true when it passes the sanity
 * limits (out holds the attempt either way) */
bool bc_fit_run(const struct bc_hdr *h, const uint16_t *ring,
                uint16_t disksafe, uint16_t out[BC_POINTS]);
bool bc_sane(const uint16_t t[BC_POINTS]);
/* Adds a fit (dropping the lowest sequence beyond BC_FITS, kept ordered
 * by sequence); returns the new count */
int bc_fits_add(struct bc_fit *fits, int n, const struct bc_fit *f);
/* The per-point median of the stored fits */
void bc_blend(const struct bc_fit *fits, int n, uint16_t out[BC_POINTS]);

/* Files. The run file is checked whole: size, magic, version, bounds and
 * CRC; crc is the stored trailer. */
bool bc_run_valid(const struct bc_hdr *h, const uint16_t *ring,
                  uint32_t crc, long size);
uint32_t bc_run_crc(const struct bc_hdr *h, const uint16_t *ring);
/* The learned-table file: "batcal 1", one line per fit, then
 * "end <crc32 hex>" over the bytes before it. format returns the length
 * (0 if it doesn't fit), parse the fit count (0 if any check fails). */
size_t bc_dat_format(char *buf, size_t size, const struct bc_fit *fits,
                     int n);
int bc_dat_parse(const char *buf, size_t len, struct bc_fit *fits);

#endif /* _IPODUI_BATCAL_CORE_H_ */
