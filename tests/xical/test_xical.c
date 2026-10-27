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

/* Host test of the Calendars index (P12 W3-C): apps/ipodui/xical_core.c,
 * the iCalendar parser, the recurrence expander, the cap and the layout,
 * against the fixtures in tests/xical/fixtures */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "ipodui/tzpick.h"
#include "ipodui/xical_core.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
    printf("\n"); } } while (0)

/* The firmware's crc_32 (firmware/common/crc32.c), bitwise */
uint32_t crc_32(const void *src, uint32_t len, uint32_t crc);
uint32_t crc_32(const void *src, uint32_t len, uint32_t crc)
{
    const unsigned char *p = src;
    while (len--)
    {
        int b;
        crc ^= (uint32_t)*p++ << 24;
        for (b = 0; b < 8; b++)
            crc = crc & 0x80000000u ? (crc << 1) ^ 0x04c11db7u : crc << 1;
    }
    return crc;
}

static uint32_t outbuf[(XCAL_FILE_BYTES + 3) / 4];
static uint16_t uids[XCAL_MAX_INSTANCES];
static struct xcal_rec todos[XCAL_TODO_MAX];
static struct xcal_build B;
static int32_t bytes;

static uint32_t at(int y, int m, int d, int h, int mi)
{
    return (uint32_t)(xcal_days(y, m, d) * 86400 + h * 3600 + mi * 60);
}

static void begin(int home_std, int home_rule)
{
    memset(&B, 0, sizeof(B));
    B.now = at(2026, 3, 15, 12, 0);
    B.home_std = (int16_t)home_std;
    B.home_rule = (uint8_t)home_rule;
    B.out = (uint8_t *)outbuf;
    B.uid = uids;
    B.todo = todos;
    xcal_begin(&B);
}

/* chunk 0: the whole file in one feed */
static void feed_file(const char *name, int chunk)
{
    char path[256], buf[64 * 1024];
    FILE *f;
    size_t n, i;

    snprintf(path, sizeof(path), "tests/xical/fixtures/%s", name);
    f = fopen(path, "rb");
    if (!f)
    {
        CHECK(0, "fixture %s", path);
        return;
    }
    n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    xcal_file_begin(&B, name);
    if (!chunk)
        xcal_feed(&B, buf, (uint32_t)n);
    else
        for (i = 0; i < n; i += chunk)
            xcal_feed(&B, buf + i, (uint32_t)(n - i < (size_t)chunk
                                              ? n - i : (size_t)chunk));
    xcal_file_end(&B);
}

static const struct xidx_hdr *H(void)
{
    return (const struct xidx_hdr *)outbuf;
}
static const struct xcal_rec *R(int i)
{
    return (const struct xcal_rec *)((const uint8_t *)outbuf + H()->rec_off)
           + i;
}
static const struct xcal_meta *M(void)
{
    return (const struct xcal_meta *)((const uint8_t *)outbuf
                                      + xcal_tab_off(H()));
}
static const char *S(uint32_t off)
{
    return (const char *)outbuf + H()->str_off + off;
}
static const uint16_t *FIRST(void)
{
    return (const uint16_t *)((const uint8_t *)M() + xcal_first_off());
}
static const uint16_t *OVL(void)
{
    return (const uint16_t *)((const uint8_t *)M() + xcal_ovl_off(M()));
}
static int bit(int row, int d)
{
    const uint8_t *b = (const uint8_t *)M() + xcal_bits_off(M());

    return (b[row * M()->bm_bytes + d / 8] >> (d % 8)) & 1;
}

/* the index's own invariants: header, CRC, meta, order, day tables */
static void sound(void)
{
    const struct xcal_meta *m;
    int i, d;

    CHECK(bytes > 0 && bytes <= XIDX_FILE_MAX, "bytes %d", (int)bytes);
    CHECK(xidx_check_body(outbuf, (uint32_t)bytes) == XIDX_OK, "crc");
    m = M();
    CHECK(xcal_meta_check(H(), m, (uint32_t)bytes) == XIDX_OK, "meta");
    CHECK(xcal_tab_check(m, H()->rec_off - 32) == XIDX_OK, "tables crc");
    ((uint8_t *)outbuf)[32 + sizeof(*m) + 1] ^= 1;
    CHECK(xcal_tab_check(m, H()->rec_off - 32) == XIDX_ECRC, "tables crc bad");
    ((uint8_t *)outbuf)[32 + sizeof(*m) + 1] ^= 1;
    CHECK(H()->count <= XCAL_MAX_INSTANCES, "cap");
    for (i = 1; i < m->nevents; i++)
        CHECK(R(i - 1)->start_rtc <= R(i)->start_rtc, "order at %d", i);
    for (d = 0; d < m->ndays; d++)
    {
        uint32_t d0 = m->day0 + d * 86400u;
        int any = 0;

        CHECK(FIRST()[d] <= FIRST()[d + 1], "first monotone %d", d);
        for (i = 0; i < m->nevents; i++)
        {
            const struct xcal_rec *r = R(i);
            uint32_t e = r->end_rtc > r->start_rtc ? r->end_rtc
                                                   : r->start_rtc + 1;
            int ov = r->start_rtc < d0 + 86400u && e > d0;

            if (ov)
            {
                any = 1;
                CHECK(i >= OVL()[d] && i < FIRST()[d + 1],
                      "day %d: event %d outside [%u, %u)", d, i, OVL()[d],
                      FIRST()[d + 1]);
                CHECK(bit(r->cal + 1, d), "cal bit day %d", d);
            }
            if (r->start_rtc >= d0 && r->start_rtc < d0 + 86400u)
                CHECK(i >= FIRST()[d] && i < FIRST()[d + 1], "start day %d", d);
        }
        CHECK(bit(0, d) == any, "dot day %d: bit %d any %d", d, bit(0, d), any);
    }
    for (i = 0; i < (int)H()->count; i++)
    {
        CHECK(R(i)->title_off < H()->str_bytes, "title off %d", i);
        CHECK(R(i)->loc_off < H()->str_bytes, "loc off %d", i);
    }
}

static int find(const char *title, uint32_t start)
{
    int i;

    for (i = 0; i < (int)H()->count; i++)
        if (!strcmp(S(R(i)->title_off), title)
            && (!start || R(i)->start_rtc == start))
            return i;
    return -1;
}

static int count_title(const char *title)
{
    int i, n = 0;

    for (i = 0; i < M()->nevents; i++)
        n += !strcmp(S(R(i)->title_off), title);
    return n;
}

static void window(void)
{
    begin(0, TZ_DST_EU);
    CHECK(B.win0 == (int64_t)at(2026, 2, 1, 0, 0), "win0");
    CHECK(B.win1 == (int64_t)at(2027, 4, 1, 0, 0), "win1");
    CHECK(B.ndays == 424, "ndays %d", B.ndays);
    bytes = xcal_emit(&B, 0x1234);
    sound();
    CHECK(H()->count == 0 && M()->ndays == 424 && H()->sig == 0x1234,
          "empty index");
}

/* a weekly 09:00 New York across both DST switches, seen from London */
static void dst_weekly(void)
{
    static const int d[12][3] = {
        {2, 2, 14}, {2, 9, 14}, {2, 16, 14}, {2, 23, 14}, {3, 2, 14},
        {3, 9, 13}, {3, 16, 13}, {3, 23, 13},       /* US on DST, EU not */
        {3, 30, 14}, {4, 6, 14}, {4, 13, 14}, {4, 20, 14} };
    int i, r;

    begin(0, TZ_DST_EU);                        /* London */
    feed_file("dst-weekly.ics", 0);
    bytes = xcal_emit(&B, 1);
    sound();
    CHECK(H()->count == 12, "dst weekly count %u", H()->count);
    CHECK(!strcmp(M()->name[0], "Work"), "calname %s", M()->name[0]);
    for (i = 0; i < 12; i++)
    {
        r = find("Stand-up", at(2026, d[i][0], d[i][1], d[i][2], 0));
        CHECK(r >= 0, "stand-up %d/%d at %d:00", d[i][0], d[i][1], d[i][2]);
        if (r >= 0)
        {
            CHECK(R(r)->end_rtc - R(r)->start_rtc == 1800, "30 min");
            CHECK(R(r)->flags == XCAL_ALARM, "alarm flag %x", R(r)->flags);
            CHECK(!strcmp(S(R(r)->loc_off), "Room 4"), "loc");
        }
    }
    begin(-300, TZ_DST_US);                     /* New York: as written */
    feed_file("dst-weekly.ics", 0);
    bytes = xcal_emit(&B, 1);
    sound();
    for (i = 0; i < 12; i++)
        CHECK(find("Stand-up", at(2026, d[i][0], d[i][1], 9, 0)) >= 0,
              "NY home %d/%d 9:00", d[i][0], d[i][1]);
    begin(600, TZ_DST_SOUTH);                   /* Sydney (AEDT +11) */
    feed_file("dst-weekly.ics", 0);
    bytes = xcal_emit(&B, 1);
    CHECK(find("Stand-up", at(2026, 2, 3, 1, 0)) >= 0, "Sydney next day 01:00");
    CHECK(find("Stand-up", at(2026, 3, 10, 0, 0)) >= 0, "Sydney, US DST 00:00");
}

static void allday(void)
{
    const struct xcal_meta *m;
    int r, d;

    begin(0, TZ_DST_EU);
    feed_file("allday.ics", 0);
    bytes = xcal_emit(&B, 1);
    sound();
    m = M();
    CHECK(m->nevents == 3, "allday events %u", m->nevents);
    r = find("Trip to the coast", at(2026, 3, 10, 0, 0));
    CHECK(r >= 0 && R(r)->end_rtc == at(2026, 3, 13, 0, 0)
          && (R(r)->flags & XCAL_ALL_DAY), "trip");
    d = (int)((at(2026, 3, 10, 0, 0) - m->day0) / 86400);
    CHECK(bit(0, d) && bit(0, d + 1) && bit(0, d + 2) && !bit(0, d + 3),
          "trip dots");
    CHECK(OVL()[d + 2] <= (unsigned)r, "trip in day 3's range");
    CHECK(count_title("Sam's birthday") == 1
          && find("Sam's birthday", at(2026, 7, 4, 0, 0)) >= 0, "birthday");
    r = find("Holiday", at(2026, 12, 31, 0, 0));
    CHECK(r >= 0 && R(r)->end_rtc == at(2027, 1, 1, 0, 0), "one-day default");
    /* To Do's: open ones, by due date, undated last */
    CHECK(H()->count == 6, "todos %u", H()->count - m->nevents);
    CHECK(!strcmp(S(R(3)->title_off), "Call the bank")
          && R(3)->start_rtc == at(2026, 3, 18, 17, 0)
          && (R(3)->flags & XCAL_TODO), "todo 1");
    CHECK(!strcmp(S(R(4)->title_off), "Return library books")
          && (R(4)->flags & XCAL_ALL_DAY), "todo 2");
    CHECK(!strcmp(S(R(5)->title_off), "Water the plants")
          && R(5)->start_rtc == 0, "todo undated");
}

static void tzid_utc(void)
{
    int r;

    begin(0, TZ_DST_EU);
    feed_file("tzid-utc.ics", 0);
    bytes = xcal_emit(&B, 1);
    sound();
    CHECK(find("Paris call", at(2026, 3, 20, 9, 0)) >= 0, "Paris 10:00 -> 9:00");
    CHECK(find("UTC call", at(2026, 3, 20, 9, 0)) >= 0, "UTC 9:00");
    r = find("Floating tea", at(2026, 3, 20, 15, 0));
    CHECK(r >= 0 && R(r)->end_rtc - R(r)->start_rtc == 45 * 60, "floating");
    CHECK(find("Summer UTC", at(2026, 7, 1, 13, 0)) >= 0, "BST 13:00");
    CHECK(find("Unknown zone", at(2026, 3, 21, 8, 0)) >= 0, "unknown: floating");
    CHECK(find("Prefixed zone", at(2026, 7, 1, 17, 0)) >= 0, "LA 9:00 -> 17:00");
    CHECK(B.tz_unknown == 1, "tz_unknown %u", B.tz_unknown);
}

static void folded(void)
{
    static const char title[] =
        "Team lunch, with everyone at the caf\xc3\xa9 on the corner";
    static const char desc[] = "First line\nSecond line which is long enough "
                               "that it gets folded with a tab";
    int chunk, r;

    for (chunk = 0; chunk <= 7; chunk += 1)
    {
        begin(0, TZ_DST_EU);
        feed_file("folded.ics", chunk);
        bytes = xcal_emit(&B, 1);
        sound();
        r = find(title, at(2026, 3, 5, 12, 30));
        CHECK(r >= 0, "folded title (chunk %d): %s", chunk,
              H()->count ? S(R(0)->title_off) : "-");
        if (r < 0)
            continue;
        CHECK(!strcmp(S(R(r)->loc_off), "Main St; upstairs"), "loc %s",
              S(R(r)->loc_off));
        CHECK(!strcmp(S(R(r)->loc_off) + strlen(S(R(r)->loc_off)) + 1, desc),
              "desc follows loc");
        CHECK(!strcmp(M()->name[0], "folded"), "file name %s", M()->name[0]);
    }
}

static void count_until(void)
{
    int i;
    static const int tt[9] = { 3, 5, 10, 12, 17, 19, 24, 26, 31 };

    begin(0, TZ_DST_EU);
    feed_file("count-until.ics", 0);
    bytes = xcal_emit(&B, 1);
    sound();
    CHECK(count_title("Daily five") == 4, "COUNT=5 less an EXDATE: %d",
          count_title("Daily five"));
    CHECK(find("Daily five", at(2026, 3, 4, 8, 0)) < 0, "EXDATE");
    CHECK(find("Daily five", at(2026, 3, 6, 8, 0)) >= 0, "5th");
    CHECK(count_title("Tue Thu") == 9, "UNTIL: %d", count_title("Tue Thu"));
    for (i = 0; i < 9; i++)
        CHECK(find("Tue Thu", at(2026, 3, tt[i], 18, 0)) >= 0, "tue/thu %d",
              tt[i]);
    CHECK(count_title("Last Friday") == 2
          && find("Last Friday", at(2026, 2, 27, 17, 0)) >= 0
          && find("Last Friday", at(2026, 3, 27, 17, 0)) >= 0,
          "-1FR COUNT counts January: %d", count_title("Last Friday"));
    CHECK(count_title("Rent") == 6
          && find("Rent", at(2026, 4, 30, 10, 0)) >= 0
          && find("Rent", at(2026, 5, 31, 10, 0)) >= 0, "BYMONTHDAY=15,-1: %d",
          count_title("Rent"));
    CHECK(count_title("Thanksgiving") == 1
          && find("Thanksgiving", at(2026, 11, 26, 12, 0)) >= 0, "4TH");
    CHECK(count_title("Biweekly") == 2
          && find("Biweekly", at(2026, 3, 6, 7, 0)) >= 0
          && find("Biweekly", at(2026, 4, 17, 7, 0)) >= 0
          && find("Biweekly", at(2026, 3, 20, 7, 0)) < 0
          && find("Biweekly", at(2026, 4, 3, 7, 0)) < 0, "overrides: %d",
          count_title("Biweekly"));
    CHECK(find("Biweekly (moved)", at(2026, 3, 20, 9, 30)) >= 0, "moved");
    CHECK(count_title("Cancelled") == 0, "cancelled");
    CHECK(count_title("Hourly") == 1 && B.rr_unsup == 1, "unsupported FREQ");
    CHECK(H()->count == 26, "count-until total %u", H()->count);
}

/* the cap: the earliest instances are kept, the file stays in its 64 KB */
static char big[512 * 1024];

static void cap(void)
{
    int i, n = 0, o;
    uint32_t last;

    begin(0, TZ_DST_EU);
    xcal_file_begin(&B, "big.ics");
    for (i = 0; i < 12; i++)
        n += snprintf(big + n, sizeof(big) - n,
                      "BEGIN:VEVENT\nUID:d%d\nDTSTART:20250101T%02d0000\n"
                      "DURATION:PT30M\nRRULE:FREQ=DAILY\nSUMMARY:Daily %d\n"
                      "END:VEVENT\n", i, 8 + i, i);
    xcal_feed(&B, big, (uint32_t)n);
    xcal_file_end(&B);
    bytes = xcal_emit(&B, 7);
    sound();
    CHECK(H()->count == XCAL_MAX_INSTANCES, "capped count %u", H()->count);
    CHECK(M()->cut == 12 * 424 - XCAL_MAX_INSTANCES, "cut %u", M()->cut);
    /* 2048 = 170 whole days of 12, then 8 of the 171st */
    last = R(XCAL_MAX_INSTANCES - 1)->start_rtc;
    CHECK(last == at(2026, 2, 1, 0, 0) + 170 * 86400 + 15 * 3600,
          "the earliest kept (last %u)", last);
    for (i = 0; i < 170; i++)
        CHECK(FIRST()[i + 1] - FIRST()[i] == 12, "day %d full", i);

    /* a file of many long events: the strings' pool, not the count */
    begin(0, TZ_DST_EU);
    xcal_file_begin(&B, "long.ics");
    for (i = 0, n = 0; i < 1500; i++)
        n += snprintf(big + n, sizeof(big) - n,
                      "BEGIN:VEVENT\nDTSTART:2026%02d%02dT100000\n"
                      "SUMMARY:Event number %d with a long descriptive title\n"
                      "LOCATION:Building %d\nDESCRIPTION:%0100d\nEND:VEVENT\n",
                      3 + i % 9, 1 + i % 28, i, i, i);
    xcal_feed(&B, big, (uint32_t)n);
    xcal_file_end(&B);
    bytes = xcal_emit(&B, 8);
    sound();
    CHECK(H()->count == 1500, "long count %u", H()->count);
    CHECK(B.pool_cut > 0 && H()->str_bytes <= XCAL_POOL, "pool cut %u",
          B.pool_cut);
    o = find("Event number 1499 with a long descriptive title", 0);
    CHECK(o < 0 || R(o)->title_off, "late titles: kept or empty");
    CHECK(xidx_check(H(), (uint32_t)bytes) == XIDX_OK, "header");

    /* more calendars than the table: the rest are left out, counted */
    begin(0, TZ_DST_EU);
    for (i = 0; i < XCAL_MAX_CALS + 2; i++)
        if (xcal_file_begin(&B, "x.ics") == 0)
            xcal_file_end(&B);
    CHECK(B.meta.ncal == XCAL_MAX_CALS && B.cal_cut == 2, "cal cap");
}

int main(void)
{
    window();
    dst_weekly();
    allday();
    tzid_utc();
    folded();
    count_until();
    cap();
    if (fails)
        return 1;
    return 0;
}
