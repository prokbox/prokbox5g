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

/* Host test of the quiet pollers (P19 19-1a): the waits each back-end
 * thread computes (firmware/common/quiet_wait_core.c), and a small model
 * of each thread's loop: no wake while idle, an event or a kick ends the
 * wait at once (the thread recomputes within one tick), and every
 * deadline the old poll served is still met. */

#include <stdio.h>
#include "quiet_wait_core.h"

static int fails;

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
    fails++; } } while (0)

#define HZ 100

/* ---- helpers ---------------------------------------------------------- */

static void test_min_until(void)
{
    CHECK(qw_min(QW_BLOCK, QW_BLOCK) == QW_BLOCK);
    CHECK(qw_min(QW_BLOCK, 5) == 5);
    CHECK(qw_min(7, QW_BLOCK) == 7);
    CHECK(qw_min(7, 5) == 5);
    CHECK(qw_until(100, 150) == 51);    /* strict: the tick after */
    CHECK(qw_until(151, 150) == 0);
    CHECK(qw_until(500, 150) == 0);
}

/* ---- audio ------------------------------------------------------------ */

static void test_audio(void)
{
    struct qw_audio_in in = { 0 };

    /* playing between tracks: no poll at all */
    CHECK(qw_audio_wait(&in, HZ) == QW_BLOCK);
    in.dac_pending = true;              /* the DAC is up while playing */
    CHECK(qw_audio_wait(&in, HZ) == QW_BLOCK);
    /* an automatic skip in progress: the FIQ's notice is polled */
    in.skip_pending = true;
    CHECK(qw_audio_wait(&in, HZ) == HZ / 10);
    /* paused with the codec's standby still to come: the old HZ/2 */
    in.skip_pending = false;
    in.paused = true;
    CHECK(qw_audio_wait(&in, HZ) == HZ / 2);
    /* paused, the codec in standby: nothing */
    in.dac_pending = false;
    CHECK(qw_audio_wait(&in, HZ) == QW_BLOCK);

    /* legacy: the old poll in every state */
    qw_legacy = true;
    in.paused = false;
    CHECK(qw_audio_wait(&in, HZ) == HZ / 2);
    in.skip_pending = true;
    CHECK(qw_audio_wait(&in, HZ) == HZ / 10);
    qw_legacy = false;
}

/* ---- storage ---------------------------------------------------------- */

/* a model of the storage thread and its drive (flash): accesses at the
 * given ticks; standby sleep_timeout after the last, the power cut a hold
 * later. Counts the thread's wakes and records when standby and the cut
 * happen. */
struct drive_model {
    int state;
    long last;
    long standby_at, off_at;
    int wakes, kicks;
};

static void drive_run(struct drive_model *m, const long *access, int n,
                      long until, long sleep_timeout, long hold, bool lit)
{
    long now = 0;
    int ai = 0;
    bool ticked = false;
    bool blocked = false;

    m->state = QW_ST_OFF;
    m->last = -100000;
    m->standby_at = m->off_at = -1;
    m->wakes = m->kicks = 0;
    while (now < until)
    {
        struct qw_stor_in in = {
            .now = now, .state = m->state, .last_activity = m->last,
            .sleep_timeout = sleep_timeout,
            .off_armed = m->state == QW_ST_SLEEPING,
            .off_deadline = m->last + hold, .lit = lit, .ticked = ticked,
        };
        long w = qw_stor_wait(&in, HZ);
        long next_access = ai < n ? access[ai] : until;
        blocked = w == QW_BLOCK;

        CHECK(w != 0);                  /* never a busy loop */
        if (!blocked && now + w <= next_access)
        {
            now += w;                   /* the deadline: the tick runs */
            m->wakes++;
            ticked = true;
            if (m->state == QW_ST_ON && now > m->last + sleep_timeout)
            {
                m->state = QW_ST_SLEEPING;
                m->standby_at = now;
            }
            else if (m->state == QW_ST_SLEEPING && now > m->last + hold)
            {
                m->state = QW_ST_OFF;
                m->off_at = now;
            }
            continue;
        }
        /* an access first: in the caller's thread; it kicks a blocked
         * storage thread (storage_quiet_kick()), which then recomputes */
        if (next_access >= until)
            break;
        now = next_access;
        ai++;
        m->state = QW_ST_ON;
        m->last = now;
        ticked = false;
        if (blocked)
        {
            m->kicks++;
            m->wakes++;
        }
    }
}

static void test_storage(void)
{
    struct qw_stor_in in = { .now = 1000, .last_activity = 900,
                             .sleep_timeout = HZ };
    struct drive_model m;

    /* off: nothing to do until an access */
    in.state = QW_ST_OFF;
    CHECK(qw_stor_wait(&in, HZ) == QW_BLOCK);
    /* on: standby at the tick after last + timeout */
    in.state = QW_ST_ON;
    CHECK(qw_stor_wait(&in, HZ) == 1);          /* 900 + 100 + 1 - 1000 */
    in.now = 950;
    CHECK(qw_stor_wait(&in, HZ) == 51);
    /* due, after a tick that failed to standby: the old retry cadence */
    in.now = 1200;
    in.ticked = true;
    CHECK(qw_stor_wait(&in, HZ) == QW_STOR_RETRY(HZ));
    in.ticked = false;
    CHECK(qw_stor_wait(&in, HZ) == 1);
    /* sleep_timeout 0: never sleeps on its own */
    in.sleep_timeout = 0;
    CHECK(qw_stor_wait(&in, HZ) == QW_BLOCK);
    /* standby with no cut owed (cache unsafe, USB, the switch off) */
    in.state = QW_ST_SLEEPING;
    in.off_armed = false;
    CHECK(qw_stor_wait(&in, HZ) == QW_BLOCK);
    /* standby, the cut owed: its deadline; capped while lit */
    in.now = 1000;
    in.off_armed = true;
    in.off_deadline = 900 + 3 * HZ;
    CHECK(qw_stor_wait(&in, HZ) == 201);
    in.off_deadline = 900 + 60 * HZ;
    in.lit = true;
    CHECK(qw_stor_wait(&in, HZ) == QW_STOR_LIT_CAP(HZ));

    /* an hour with the drive off: no wake at all */
    drive_run(&m, NULL, 0, 3600L * HZ, HZ, 3 * HZ, false);
    CHECK(m.wakes == 0);

    /* a refill (reads over half a second) in the dark: one kick, then
     * standby and the cut at their deadlines, then nothing for the hour */
    {
        static const long reads[] = { 1000, 1010, 1020, 1030, 1040, 1050 };
        drive_run(&m, reads, 6, 3600L * HZ, HZ, 3 * HZ, false);
        CHECK(m.kicks == 1);
        CHECK(m.standby_at == 1050 + HZ + 1);
        CHECK(m.off_at == 1050 + 3 * HZ + 1);
        CHECK(m.wakes <= 6);
        printf("     storage: refill in the dark: %d wakes, standby +%ld, "
               "cut +%ld ticks\n", m.wakes, m.standby_at - 1050,
               m.off_at - 1050);
    }
    /* the old poll for the same hour: 7200 wakes */
    qw_legacy = true;
    {
        static const long reads[] = { 1000 };
        drive_run(&m, reads, 1, 3600L * HZ, HZ, 3 * HZ, false);
        CHECK(m.wakes >= 7190);
    }
    qw_legacy = false;
}

/* ---- power ------------------------------------------------------------ */

static void test_power(void)
{
    struct qw_power_in in = { .now = 1000, .last_step = 1000 };

    in.quiet = false;
    CHECK(qw_power_wait(&in, HZ) == HZ / 2);   /* stock step */
    in.quiet = true;
    CHECK(qw_power_wait(&in, HZ) == 2 * HZ);   /* 18-A, lit or paused */
    in.dark_playing = true;
    CHECK(qw_power_wait(&in, HZ) == 10 * HZ);  /* 19-1a */
    /* a sleep timer ending sooner is a deadline of its own */
    in.sleeptimer = true;
    in.sleep_end = 1000 + 3 * HZ;
    CHECK(qw_power_wait(&in, HZ) == 3 * HZ + 1);
    in.sleep_end = 1000 + 60 * HZ;
    CHECK(qw_power_wait(&in, HZ) == 10 * HZ);
    /* a step overdue: now */
    in.now = 1000 + 20 * HZ;
    CHECK(qw_power_wait(&in, HZ) == 0);
    /* a kick (the backlight lighting): the caller recomputes with the
     * new inputs, so a lit panel takes 18-A's step from the last one */
    in.now = 1000 + 4 * HZ;
    in.dark_playing = false;
    in.sleeptimer = false;
    CHECK(qw_power_wait(&in, HZ) == 0);        /* 2 s step already due */
    in.now = 1000 + HZ;
    CHECK(qw_power_wait(&in, HZ) == HZ);
    /* not quiet (a shutdown, a charger, USB, at or under 15 %): HZ/2 */
    in.quiet = false;
    in.dark_playing = true;
    CHECK(qw_power_wait(&in, HZ) == 0);

    /* legacy: 18-A's 2 s, the sleep timer as a step check */
    qw_legacy = true;
    in.now = 1000;
    in.quiet = true;
    in.sleeptimer = true;
    in.sleep_end = 1000 + HZ;
    CHECK(qw_power_wait(&in, HZ) == 2 * HZ);
    qw_legacy = false;

    /* an hour of dark play: 360 steps instead of 1800 (18-A) */
    {
        long now = 0, last = 0;
        int steps = 0;
        struct qw_power_in p = { .quiet = true, .dark_playing = true };
        while (now < 3600L * HZ)
        {
            p.now = now;
            p.last_step = last;
            long w = qw_power_wait(&p, HZ);
            now += w > 0 ? w : 0;
            last = now;
            steps++;
            CHECK(w > 0 || steps == 1);
        }
        CHECK(steps >= 360 && steps <= 361);
    }
}

/* ---- tagcache --------------------------------------------------------- */

static void test_tagcache(void)
{
    /* work owed, or lit: the old 1 s poll */
    CHECK(qw_tc_wait(true, 1000, QW_BLOCK, HZ) == HZ);
    /* nothing owed, no deadline: the safety cap */
    CHECK(qw_tc_wait(false, 1000, QW_BLOCK, HZ) == QW_TC_CAP(HZ));
    /* a back-off ending sooner */
    CHECK(qw_tc_wait(false, 1000, 1500, HZ) == 501);
    /* review H1: a deadline already past had its pass; its work is
     * blocked (window held shut, retry gate): the cap, never a spin */
    CHECK(qw_tc_wait(false, 1000, 900, HZ) == QW_TC_CAP(HZ));
    CHECK(qw_tc_wait(false, 1000, 1000, HZ) == QW_TC_CAP(HZ));
    /* a deadline under a second away: floored at the old poll */
    CHECK(qw_tc_wait(false, 1000, 1010, HZ) == HZ);
    /* a deadline past the cap: the cap */
    CHECK(qw_tc_wait(false, 1000, 1000 + 120 * HZ, HZ) == QW_TC_CAP(HZ));
    qw_legacy = true;
    CHECK(qw_tc_wait(false, 1000, QW_BLOCK, HZ) == HZ);
    qw_legacy = false;

    /* qw_tc_earliest(): only future candidates count */
    CHECK(qw_tc_earliest(1000, QW_BLOCK, 900) == QW_BLOCK);
    CHECK(qw_tc_earliest(1000, QW_BLOCK, 1000) == QW_BLOCK);
    CHECK(qw_tc_earliest(1000, QW_BLOCK, 1200) == 1200);
    CHECK(qw_tc_earliest(1000, 1500, 1200) == 1200);
    CHECK(qw_tc_earliest(1000, 1200, 1500) == 1200);
    CHECK(qw_tc_earliest(1000, 1200, 900) == 1200);  /* past: ignored */

    /* blocked-work scenarios over an hour, each pass changing nothing
     * (the deadline's work is gated), so the deadline stays put:
     *  A. paused >= 3 min with Backlight Off (maint's MAINT_NO_LIT keeps
     *     the window shut) / a key held / a paused video: the window's
     *     age deadline paused_since + 180 s is past
     *  B. a retry back-off whose worker returns at a gate without
     *     rescheduling (COMPACT short of memory, JOURNAL): its next is
     *     past for the rest of the session
     *  C. both, plus one future deadline that is then met once */
    {
        static const struct { long paused_since, retry_next, future; } sc[] = {
            { 0, QW_BLOCK, QW_BLOCK },                /* A */
            { QW_BLOCK, 50 * HZ, QW_BLOCK },          /* B */
            { 0, 50 * HZ, 400 * HZ },                 /* C */
        };
        for (unsigned k = 0; k < sizeof(sc) / sizeof(sc[0]); k++)
        {
            long now = 200 * HZ;    /* after the window's age and retry */
            int wakes = 0;
            while (now < 200 * HZ + 3600L * HZ)
            {
                long d = QW_BLOCK;
                if (sc[k].paused_since != QW_BLOCK)
                    d = qw_tc_earliest(now, d, sc[k].paused_since + 180 * HZ);
                if (sc[k].retry_next != QW_BLOCK)
                    d = qw_tc_earliest(now, d, sc[k].retry_next);
                if (sc[k].future != QW_BLOCK)
                    d = qw_tc_earliest(now, d, sc[k].future);
                long w = qw_tc_wait(false, now, d, HZ);
                CHECK(w >= HZ);
                now += w;
                wakes++;
            }
            /* the cap: 120 an hour (+1 for C's deadline) */
            CHECK(wakes <= 3600 / 30 + 2);
            printf("     tagcache: blocked scenario %c: %d wakes/h\n",
                   'A' + k, wakes);
        }
    }
}

int main(void)
{
    test_min_until();
    test_audio();
    test_storage();
    test_power();
    test_tagcache();
    if (fails)
        fprintf(stderr, "%d check(s) failed\n", fails);
    return fails ? 1 : 0;
}
