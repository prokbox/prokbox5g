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

/* iPod UI shell: the Calendars index (P12 W3-C, docs/phase12-brief.md 3.5):
 * the iCalendar parser, the recurrence expander and the index layout.
 * Pure, host-tested in tests/xical */
#ifndef IPODUI_XICAL_CORE_H
#define IPODUI_XICAL_CORE_H

/* What it reads: the .ics (and plain .vcs) files of /Calendars and its
 * folders, one level deep (Q12, stock); each file is one calendar, named
 * by its X-WR-CALNAME or else its file name. VEVENT (DTSTART, DTEND or
 * DURATION, all-day dates, TZID / UTC / floating times, SUMMARY,
 * LOCATION, DESCRIPTION, RRULE, EXDATE, RECURRENCE-ID, STATUS, VALARM),
 * VTODO (SUMMARY, DUE, STATUS/COMPLETED) and VTIMEZONE (its STANDARD and
 * DAYLIGHT offsets, the rule matched to tzpick's US / EU / south rules).
 *
 * Times. Every instance is stored in the home zone's local time (the RTC
 * holds local time: tzpick.h): an event's wall time in its own zone goes
 * to UTC (its standard offset and DST rule), then to the home city's
 * standard offset plus the home DST at that instant (tz_dst_minutes(),
 * the W0 formula: never the home offset alone). A zone equal to the
 * home's is taken as is (exact in the switch hour too). Floating times
 * and all-day dates are home local as written. A TZID with no VTIMEZONE
 * is looked up in a short table of common zone names; an unknown one is
 * read as floating (counted, "tzunk").
 *
 * Recurrences (the rule chosen for the cap): FREQ DAILY / WEEKLY /
 * MONTHLY / YEARLY with INTERVAL, COUNT, UNTIL, BYDAY (with ordinals in
 * MONTHLY / YEARLY), BYMONTHDAY, BYMONTH and WKST are expanded in the
 * event's own wall time (so a weekly 09:00 in New York stays 09:00 New
 * York across both DST switches), then converted. Only the instances
 * that overlap the window [first day of last month, first day of the
 * month 13 months on) are kept: about -1 to +12 months, whole months for
 * the month grid; the window rolls forward at each build (the month is
 * part of the folder signature). Other FREQs, BYSETPOS, BYWEEKNO and
 * BYYEARDAY keep the first instance only (counted, "rrunsup"). EXDATE
 * removes an instance; a RECURRENCE-ID event replaces its series'
 * instance (STATUS:CANCELLED removes it). At most XCAL_ITER_MAX periods
 * are visited per event.
 *
 * The cap: XCAL_MAX_INSTANCES records in all (events + To Do's). When
 * more instances fall in the window, the EARLIEST are kept (a max-heap
 * on the start drops the latest), so what is cut is the far future, never
 * today; the count cut is logged ("xcal cut n="). Strings go in a 16 KB
 * pool in priority order: titles always, a location while a tenth of the
 * pool is free, a description while a quarter is (counted, "poolcut").
 *
 * File (kind XIDX_CAL, xindex_core.h): struct xidx_hdr; then, at 32, the
 * tables: struct xcal_meta, uint16 day_first[ndays + 1] (the first event
 * starting on or after day d), uint16 day_ovl[ndays] (the first event
 * overlapping day d: a day's events are among [day_ovl[d],
 * day_first[d + 1]), those whose end passes the day's start), then
 * (ncal + 1) rows of bm_bytes of per-day bits (row 0 any calendar, row
 * c + 1 calendar c: the month grid's dots); then count records (struct
 * xcal_rec) at rec_off: the events by start [0, nevents), then the To
 * Do's (open ones only, by due date, undated last); then the strings at
 * str_off. An event's strings are its title, then at loc_off its location
 * immediately followed by its description (each NUL-terminated; "" at
 * offset 0; the title and the location are adjacent when both are kept).
 * The tables come first so that one read from 0 gives a screen the
 * header and every table (Class A: one open and one read at entry); the
 * header's CRC starts at rec_off, so the tables carry their own
 * (xcal_meta.tab_crc, xcal_tab_check()). */

#include <stdint.h>
#include "ipodui/xindex_core.h"

#define XCAL_ROOT_DIR   "/Calendars"    /* Q12: the visible folder */
#define XCAL_MAX_CALS   16              /* calendar files indexed */
#define XCAL_NAME_MAX   32              /* a calendar's name, with its NUL */
#define XCAL_DAYS_MAX   428             /* 14 whole months, at most */
#define XCAL_TITLE_MAX  64              /* bytes kept, with the NUL */
#define XCAL_LOC_MAX    64
#define XCAL_DESC_MAX   256
#define XCAL_TODO_MAX   128
#define XCAL_POOL       (16*1024)
#define XCAL_EXDATE_MAX 32              /* per event */
#define XCAL_BYDAY_MAX  16
#define XCAL_BYMD_MAX   8
#define XCAL_OVR_MAX    128             /* RECURRENCE-ID overrides, per file */
#define XCAL_TZ_MAX     12              /* VTIMEZONEs, per file */
#define XCAL_TZID_MAX   48
#define XCAL_LINE_MAX   1024            /* an unfolded line; more is cut */
#define XCAL_ITER_MAX   20000           /* periods visited per event */
#define XCAL_SPAN_DAYS  62              /* days a long event marks */

struct xcal_meta {
    uint32_t day0;                      /* local RTC s: 00:00, first day */
    uint32_t built;                     /* local RTC s of the build */
    uint16_t ndays;                     /* days in the window */
    uint16_t nevents;                   /* records [0, nevents) are events */
    uint16_t ncal;                      /* calendars (files) */
    uint16_t bm_bytes;                  /* (ndays + 7) / 8 */
    uint16_t cut;                       /* instances the cap left out */
    int16_t  home_std;                  /* the home zone at the build */
    uint8_t  home_rule, flags;
    uint16_t reserved;
    uint32_t tab_crc;                   /* crc_32(~0) over the tables, this
                                           field read as 0 */
    char     name[XCAL_MAX_CALS][XCAL_NAME_MAX];
};
_Static_assert(sizeof(struct xcal_meta) == 28 + XCAL_MAX_CALS * XCAL_NAME_MAX,
               "xcal_meta layout");

/* the tables' bytes (at 32, before the records) */
#define XCAL_TAB_BYTES(ndays, ncal) \
    (sizeof(struct xcal_meta) + 2u * (2u * (ndays) + 1u) \
     + (uint32_t)((ncal) + 1) * (((ndays) + 7u) / 8u))
#define XCAL_TAB_MAX    ((XCAL_TAB_BYTES(XCAL_DAYS_MAX, XCAL_MAX_CALS) + 3) & ~3u)
/* the largest index */
#define XCAL_FILE_BYTES (sizeof(struct xidx_hdr) \
                         + XCAL_MAX_INSTANCES * sizeof(struct xcal_rec) \
                         + XCAL_TAB_MAX + XCAL_POOL)
_Static_assert(XCAL_FILE_BYTES <= XIDX_FILE_MAX, "the Calendars index cap");

/* -- the reader's side (the screens) ------------------------------------- */
/* Checks a meta block read from a sound header (count records, rec_off):
 * its own ranges against the file; 0 or XIDX_EBOUNDS */
int xcal_meta_check(const struct xidx_hdr *h, const struct xcal_meta *m,
                    uint32_t file_bytes);
/* The tables' CRC (tab = the meta and its tables in memory, len >=
 * XCAL_TAB_BYTES(ndays, ncal)): 0 or XIDX_ECRC */
int xcal_tab_check(const void *tab, uint32_t len);
/* The tables' offset in the file, and the offsets of its parts */
static inline uint32_t xcal_tab_off(const struct xidx_hdr *h)
{
    (void)h;
    return sizeof(struct xidx_hdr);
}
static inline uint32_t xcal_first_off(void)
{
    return sizeof(struct xcal_meta);
}
static inline uint32_t xcal_ovl_off(const struct xcal_meta *m)
{
    return sizeof(struct xcal_meta) + 2u * (m->ndays + 1u);
}
static inline uint32_t xcal_bits_off(const struct xcal_meta *m)
{
    return xcal_ovl_off(m) + 2u * m->ndays;
}

/* -- civil time (seconds since 2000-01-01 00:00, signed) ----------------- */
int64_t xcal_days(int y, int m, int d);         /* days since 2000-01-01 */
void    xcal_civil(int64_t days, int *y, int *m, int *d);
int     xcal_wday(int64_t days);                /* 0 = Sunday */
int     xcal_mdays(int y, int m);               /* days in month m (1..12) */

/* -- the builder --------------------------------------------------------- */
enum xcal_zkind { XCAL_Z_FLOAT = 0, XCAL_Z_UTC, XCAL_Z_TZ };
struct xcal_zone {
    int16_t std;                        /* minutes east of UTC */
    uint8_t rule;                       /* enum tz_dst (tzpick.h) */
    uint8_t kind;                       /* enum xcal_zkind */
};

struct xcal_dt {                        /* a DATE or DATE-TIME as written */
    int64_t wall;                       /* s since 2000, its own clock */
    uint8_t set, date_only, utc;
    uint8_t tz;                         /* the file's zone index, or 0xff */
};

struct xcal_tzdef {                     /* one VTIMEZONE */
    char id[XCAL_TZID_MAX];
    int16_t std, dst;                   /* TZOFFSETTO of each part */
    int8_t  dmon, dord, smon, sord;     /* their RRULE BYMONTH, BYDAY n */
    int64_t dstart, sstart;             /* the parts' DTSTART (latest kept) */
    uint8_t has_dst;
};

struct xcal_ev {                        /* the VEVENT or VTODO being read */
    char title[XCAL_TITLE_MAX];
    char loc[XCAL_LOC_MAX];
    char desc[XCAL_DESC_MAX];
    char tzid_s[XCAL_TZID_MAX], tzid_e[XCAL_TZID_MAX], tzid_r[XCAL_TZID_MAX];
    struct xcal_dt start, end, rid, until;
    int64_t dur;                        /* DURATION, s; -1 none */
    int64_t ex[XCAL_EXDATE_MAX];        /* EXDATE walls (date_only: days) */
    uint8_t ex_date[XCAL_EXDATE_MAX];
    char    ex_tz[XCAL_EXDATE_MAX];     /* 'z' UTC, 'f' floating, 's' start's */
    uint8_t nex;
    /* RRULE */
    uint8_t freq;                       /* 0 none, 1 daily .. 4 yearly, 5 other */
    uint8_t unsup;                      /* BYSETPOS, BYWEEKNO, BYYEARDAY */
    uint16_t interval;
    uint32_t count;
    int8_t  bd_ord[XCAL_BYDAY_MAX];
    uint8_t bd_wd[XCAL_BYDAY_MAX], nbd;
    int8_t  bymd[XCAL_BYMD_MAX];
    uint8_t nbymd;
    uint16_t bymon;                     /* bit m (1..12) */
    uint8_t wkst;                       /* 0 = Sunday; default Monday */
    uint8_t cancelled, done, alarm, has_dur;
    uint16_t uid;                       /* 15-bit hash of UID */
};

struct xcal_ovr {                       /* a series instance replaced */
    int64_t at;                         /* home local */
    uint16_t uid, cal;
};

#define XCAL_UID_OVR    0x8000u         /* uid[] mark: an override's own */

struct xcal_build {
    /* set by the caller before xcal_begin() */
    uint32_t now;                       /* local RTC s of the build */
    int16_t  home_std;                  /* the home city's standard offset */
    uint8_t  home_rule;                 /* and its DST rule */
    uint8_t  pad0;
    uint8_t *out;                       /* XCAL_FILE_BYTES, the index */
    uint16_t *uid;                      /* XCAL_MAX_INSTANCES */
    struct xcal_rec *todo;              /* XCAL_TODO_MAX */
    /* the window (xcal_begin) */
    int64_t win0, win1;
    uint16_t ndays;
    /* the collection */
    struct xcal_rec *rec;               /* out + 32: a max-heap by start */
    uint32_t n, ntodo;
    char *pool;                         /* in out, at its largest offset */
    uint32_t pool_used;
    struct xcal_meta meta;              /* names, ncal */
    /* counters (the trace) */
    uint32_t cut, todo_cut, pool_cut, iter_cut, tz_unknown, rr_unsup;
    uint32_t events, instances, files, cal_cut, bad;
    /* the file being read (xcal_file_begin .. xcal_file_end) */
    uint16_t cal;
    uint8_t  comp, sub, depth, in_alarm, trunc, pending, ntz, cal_named;
    uint16_t novr;
    uint32_t len;                       /* the logical line's bytes */
    /* a VTIMEZONE's STANDARD or DAYLIGHT part being read */
    int64_t  p_start;
    int16_t  p_off;
    int8_t   p_mon, p_ord;
    struct xcal_ev ev;
    struct xcal_tzdef tz[XCAL_TZ_MAX];
    struct xcal_ovr ovr[XCAL_OVR_MAX];
    char line[XCAL_LINE_MAX];
};

/* The window from b->now, the empty collection. */
void xcal_begin(struct xcal_build *b);
/* One calendar file: its display name (the file's, without extension;
 * an X-WR-CALNAME replaces it). Returns 0, or -1: the calendar cap (the
 * file is left out, counted in cal_cut). */
int  xcal_file_begin(struct xcal_build *b, const char *name);
/* Bytes of the file in order, any chunking */
void xcal_feed(struct xcal_build *b, const char *buf, uint32_t len);
void xcal_file_end(struct xcal_build *b);
/* Sorts, lays out the tables and the strings and seals the index with
 * sig. Returns the file's bytes. */
int32_t xcal_emit(struct xcal_build *b, uint32_t sig);

#endif /* IPODUI_XICAL_CORE_H */
