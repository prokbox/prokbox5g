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
/* Host test of firmware/common/panic_record.c */
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "panic_record.h"

static int fails;

#define CHECK(c) do { if (!(c)) { \
    printf("  line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(void)
{
    struct panic_record r;
    const struct panic_record *p;
    char longmsg[300];
    unsigned int i;

    /* fits the reserve the firmware takes off the audio buffer */
    CHECK(sizeof(r) <= PANIC_REC_RESERVE);
    CHECK(sizeof(r) % 4 == 0);

    /* power-on garbage is never taken */
    memset(&r, 0xa5, sizeof(r));
    CHECK(panic_record_take(&r) == NULL);
    memset(&r, 0, sizeof(r));
    CHECK(panic_record_take(&r) == NULL);
    /* the right magic over garbage is not enough */
    memset(&r, 0x5a, sizeof(r));
    r.magic = PANIC_REC_MAGIC;
    CHECK(panic_record_take(&r) == NULL);

    /* a garbage area starts the count at 1 */
    memset(&r, 0xa5, sizeof(r));
    panic_record_write(&r, 3, 0x1234, 0x5678, 0x9abc, "audio",
                       "Data abort");
    CHECK(r.magic == PANIC_REC_MAGIC);
    CHECK(r.count == 1);

    /* a second panic before a boot logs it counts on, newest wins */
    panic_record_write(&r, 0, 0x2000, 0, 0x3000, "tagcache", "Stkov x");
    CHECK(r.count == 2);
    p = panic_record_take(&r);
    CHECK(p == &r);
    if (p)
    {
        CHECK(p->kind == 0 && p->pc == 0x2000 && p->sp == 0x3000);
        CHECK(!strcmp(p->thread, "tagcache"));
        CHECK(!strcmp(p->msg, "Stkov x"));
    }
    /* consumed: taken once only; the fields stay readable */
    CHECK(r.magic == PANIC_REC_USED);
    CHECK(panic_record_take(&r) == NULL);
    CHECK(!strcmp(r.msg, "Stkov x"));

    /* after a logged one the count starts again */
    panic_record_write(&r, 1, 1, 2, 3, NULL, NULL);
    CHECK(r.count == 1);
    CHECK(r.thread[0] == '\0' && r.msg[0] == '\0');

    /* truncation: NUL-terminated, the tail padded */
    for (i = 0; i < sizeof(longmsg) - 1; i++)
        longmsg[i] = 'a' + i % 26;
    longmsg[sizeof(longmsg) - 1] = '\0';
    panic_record_write(&r, 0, 0, 0, 0, "a-very-long-thread-name", longmsg);
    CHECK(strlen(r.msg) == PANIC_REC_MSG - 1);
    CHECK(!memcmp(r.msg, longmsg, PANIC_REC_MSG - 1));
    CHECK(strlen(r.thread) == PANIC_REC_THREAD - 1);

    /* any flipped bit before the sum is caught */
    {
        unsigned char *b = (unsigned char *)&r;
        unsigned int n = offsetof(struct panic_record, sum), bad = 0;
        struct panic_record good = r;
        for (i = 4; i < n; i++)     /* the magic is checked by itself */
        {
            int bit;
            for (bit = 0; bit < 8; bit++)
            {
                r = good;
                b[i] ^= 1u << bit;
                if (panic_record_take(&r))
                    bad++;
            }
        }
        CHECK(bad == 0);
        r = good;
        r.sum ^= 0x80;
        CHECK(panic_record_take(&r) == NULL);
        r = good;
        CHECK(panic_record_take(&r) != NULL);
    }

    /* a reset between the magic store and the sum store: invalid */
    panic_record_write(&r, 0, 0, 0, 0, "x", "y");
    r.sum = 0;
    CHECK(panic_record_take(&r) == NULL);

    /* the extras (uptime, stack scan, log tail): cleared by write,
     * covered by the sum once sealed again; a write without the reseal
     * (a fault while filling them) leaves the record invalid */
    memset(&r, 0x77, sizeof(r));
    panic_record_write(&r, 0, 1, 0, 2, "main", "m");
    CHECK(r.uptime == 0 && r.trace[0] == 0 && r.tail[0] == '\0'
          && r.tail[PANIC_REC_TAIL - 1] == '\0');
    r.magic = 0;
    r.uptime = 1234;
    r.trace[0] = 0x907e8;
    strcpy(r.tail, " boot window ms=3000\n*lib play n=1\n");
    CHECK(panic_record_take(&r) == NULL);
    panic_record_seal(&r);
    {
        struct panic_record good = r;
        r.tail[3] ^= 1;
        CHECK(panic_record_take(&r) == NULL);
        r = good;
        CHECK(panic_record_peek(&r) == &r);     /* not yet logged */
        CHECK(panic_record_take(&r) != NULL && r.uptime == 1234
              && !strcmp(r.tail + 1, "boot window ms=3000\n*lib play n=1\n"));
        /* logged: still readable whole for the report, never taken again */
        CHECK(panic_record_peek(&r) == &r);
        CHECK(panic_record_take(&r) == NULL);
        r.msg[0] ^= 1;
        CHECK(panic_record_peek(&r) == NULL);   /* torn since */
        r.msg[0] ^= 1;
        r.magic = 0;
        CHECK(panic_record_peek(&r) == NULL);   /* never sealed */
    }

    /* the hang record (hang_watch.c): its own kind, clear of panicf's 0
     * and UIE's 1..6; info cleared by write and covered by the sum */
    CHECK(PANIC_REC_HANG != 0 && PANIC_REC_HANG > 6);
    memset(&r, 0x33, sizeof(r));
    panic_record_write(&r, PANIC_REC_HANG, 0, 0x1111, 0x2222, "storage",
                       "hang: sto no progress 10 s (blocked) bench=-");
    CHECK(r.info[0] == '\0' && r.info[PANIC_REC_INFO - 1] == '\0');
    CHECK(panic_record_pending(&r));
    r.magic = 0;
    strcpy(r.info, "stk ui 2104/8192 sto 3072/3072!OVF mx ata=storage i2c=-");
    panic_record_seal(&r);
    {
        struct panic_record good = r;
        r.info[5] ^= 1;
        CHECK(!panic_record_pending(&r));       /* torn info: invalid */
        r = good;
        p = panic_record_take(&r);
        CHECK(p && p->kind == PANIC_REC_HANG && p->lr == 0x1111
              && strstr(p->info, "ata=storage i2c=-") != NULL);
        CHECK(!panic_record_pending(&r));       /* logged: may be replaced */
        CHECK(panic_record_peek(&r) == &r);
    }

    /* the watcher's test, per thread (now/limit in ticks, HZ = 100) */
    {
        struct hang_mon m = { 0, 0 };
        /* first look: progress (the count moved from 0) */
        CHECK(!hang_overdue(&m, 1, HANG_ST_WORK, 100, 600, 0, 200));
        CHECK(m.since == 100);
        /* at work, no progress: late at exactly the limit */
        CHECK(!hang_overdue(&m, 1, HANG_ST_WORK, 699, 600, 0, 200));
        CHECK(hang_overdue(&m, 1, HANG_ST_WORK, 700, 600, 0, 200));
        /* progress restarts the clock */
        CHECK(!hang_overdue(&m, 2, HANG_ST_WORK, 800, 600, 0, 200));
        CHECK(!hang_overdue(&m, 2, HANG_ST_WORK, 1399, 600, 0, 200));
        /* idle (a queue wait) is never late, however long */
        CHECK(!hang_overdue(&m, 2, HANG_ST_IDLE, 99999, 600, 0, 200));
        /* and the clock counts from the end of the idle */
        CHECK(!hang_overdue(&m, 2, HANG_ST_WORK, 100500, 600, 0, 200));
        CHECK(hang_overdue(&m, 2, HANG_ST_WORK, 100599, 600, 0, 200));
        /* waiting on a watched peer: the peer's grace more */
        m.seen = 3;
        m.since = 0;
        CHECK(!hang_overdue(&m, 3, HANG_ST_PEER, 799, 600, 0, 200));
        CHECK(hang_overdue(&m, 3, HANG_ST_PEER, 800, 600, 0, 200));
        /* a spin-up's grace */
        CHECK(!hang_overdue(&m, 3, HANG_ST_WORK, 1599, 600, 1000, 200));
        CHECK(hang_overdue(&m, 3, HANG_ST_WORK, 1600, 600, 1000, 200));
    }

    if (fails)
        printf("%d check(s) failed\n", fails);
    return fails != 0;
}
