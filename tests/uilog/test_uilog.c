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

/* Host test of apps/ipodui/uilog.c (Phase 8a U2; docs/phase8a-brief.md
 * §3.3, Revision 1 H3, M3): the ring, the persistent idle-flush handler,
 * overflow, rotation, a line logged during a flush, repeat collapse and a
 * write error. GPL v2+ */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include "stubs.h"
#include "ipodui/uilog.h"

#undef open
#undef close
#undef write
#undef rename
#undef remove

long current_tick = 1000;
static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; \
        printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
        printf("\n"); } } while (0)

/* -- fakes -------------------------------------------------------------- */

static int opens, adds, writes, renames, fail_write_at = -1;
static bool disk_active;
static int audio_state;         /* audio_status() */

const char rbversion[] = "test-build";
#define BOOT "boot build=test-build rtc=2026-10-15 09:01:14\n"

int audio_status(void)
{
    return audio_state;
}

struct tm *get_time(void)
{
    static struct tm tm;

    tm.tm_year = 2026 - 1900;
    tm.tm_mon = 9;
    tm.tm_mday = 15;
    tm.tm_hour = 9;
    tm.tm_min = 1;
    tm.tm_sec = 14;
    return &tm;
}

bool storage_disk_is_active(void)
{
    return disk_active;
}

/* the core allocator: a few blocks (the boot ring and the bench ring) */
int t_core_live, t_core_fail;
static void *core_block[4];

int core_alloc(size_t size)
{
    int h;

    if (t_core_fail)
    {
        t_core_fail = 0;
        return -1;
    }
    for (h = 0; h < 4 && core_block[h]; h++)
        ;
    if (h == 4 || !(core_block[h] = malloc(size)))
        return -1;
    memset(core_block[h], 0xA5, size);  /* garbage, as the allocator gives */
    t_core_live++;
    return h + 1;
}

void *core_get_data(int h)
{
    return h >= 1 && h <= 4 ? core_block[h - 1] : NULL;
}

void core_pin(int h) { (void)h; }
void core_unpin(int h) { (void)h; }

int core_free(int h)
{
    if (h >= 1 && h <= 4 && core_block[h - 1])
    {
        free(core_block[h - 1]);
        core_block[h - 1] = NULL;
        t_core_live--;
    }
    return 0;
}
static bool add_oneshot = true;
static void (*idle_handler)(unsigned short, void *, void *);
static void (*write_hook)(void);
static char trace[8192];

int t_open(const char *path, int flags, int mode)
{
    opens++;
    return open(path, flags, mode);
}

int t_close(int fd)
{
    return close(fd);
}

long t_write(int fd, const void *buf, unsigned long n)
{
    if (write_hook)
    {
        void (*h)(void) = write_hook;
        write_hook = NULL;
        h();                    /* another thread runs at this yield */
    }
    if (writes++ == fail_write_at)
        return -1;
    return write(fd, buf, n);
}

long t_filesize(int fd)
{
    struct stat st;
    return fstat(fd, &st) ? -1 : (long)st.st_size;
}

int t_rename(const char *a, const char *b)
{
    renames++;
    return rename(a, b);
}

int t_remove(const char *p)
{
    return unlink(p);
}

bool add_event_ex(unsigned short id, bool oneshot,
                  void (*handler)(unsigned short, void *, void *), void *ud)
{
    (void)ud;
    CHECK(id == DISK_EVENT_SPINUP, "event id %x", id);
    adds++;
    add_oneshot = oneshot;
    idle_handler = handler;
    return true;
}

void t_debugf(const char *fmt, ...)
{
    va_list ap;
    size_t n = strlen(trace);
    va_start(ap, fmt);
    if (n < sizeof(trace) - 1)
        vsnprintf(trace + n, sizeof(trace) - n, fmt, ap);
    va_end(ap);
}

/* -- helpers ------------------------------------------------------------ */

static char *slurp(const char *path)
{
    static char buf[65536];
    FILE *f = fopen(path, "rb");
    size_t n = 0;

    buf[0] = 0;
    if (f)
    {
        n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
    }
    buf[n] = 0;
    return buf;
}

static void idle(void)          /* the storage thread's spin-down notify */
{
    if (idle_handler)
        idle_handler(DISK_EVENT_SPINUP, NULL, NULL);
}

static void late_line(void)
{
    ui_log("late", "during flush");
}

#ifdef T_BOOT_RING
/* The boot ring (UI_LOG_BOOT_LINES): 40 held lines all kept; a bench
 * session and back returns to it, not to the 12-line ring */
static int boot_ring_test(const char *path)
{
    char *s;
    int i;

    ui_log_init();
    CHECK(t_core_live == 1, "boot ring allocated");
    ui_log_hold(true);
    for (i = 0; i < 40; i++)
        ui_log("boot", (i & 1) ? "a=%d" : "b=%d", i);
    ui_log_hold(false);
    ui_log_flush();
    s = slurp(path);
    CHECK(strstr(s, "log boot ring lines=64\n") && strstr(s, "boot b=0\n") &&
          strstr(s, "boot a=39\n") && !strstr(s, "log dropped"),
          "40 lines kept: [%.300s]", s);
    ui_log_bench(true);
    CHECK(t_core_live == 2, "bench ring beside it");
    ui_log_bench(false);
    CHECK(t_core_live == 1, "bench ring freed, boot ring kept");
    ui_log_hold(true);
    for (i = 0; i < 40; i++)
        ui_log("after", (i & 1) ? "a=%d" : "b=%d", i);
    ui_log_hold(false);
    ui_log_flush();
    s = slurp(path);
    CHECK(strstr(s, "after b=0\n") && strstr(s, "after a=39\n") &&
          !strstr(s, "log dropped"), "back to 64 after the session");
    return fails ? 1 : 0;
}
#endif

int main(void)
{
    const char *path = UI_LOG_FILE;
    char *s;
    int i, o;

    for (i = 0; i <= UI_LOG_KEEP; i++)
    {
        char g[sizeof(UI_LOG_FILE) + 12];
        snprintf(g, sizeof(g), i ? "%s.%d" : "%s", UI_LOG_FILE, i);
        unlink(g);
    }

    /* S36: each boot moves the generations on (.1 .. .UI_LOG_KEEP, the
     * oldest goes) and starts the file with a boot line */
    for (i = 0; i <= UI_LOG_KEEP; i++)
    {
        char g[sizeof(UI_LOG_FILE) + 12];
        FILE *f;
        snprintf(g, sizeof(g), i ? "%s.%d" : "%s", UI_LOG_FILE, i);
        f = fopen(g, "wb");
        fprintf(f, "gen %d\n", i);
        fclose(f);
    }

#ifdef T_BOOT_RING
    return boot_ring_test(path);
#endif
    /* init: one persistent registration (H3), idempotent; the boot ring's
       allocation fails here, so these checks run on the 12-line ring */
    t_core_fail = 1;
    ui_log_init();
    ui_log_init();
    CHECK(adds == 1 && !add_oneshot && idle_handler, "one persistent add");
    CHECK(renames == UI_LOG_KEEP, "one roll at boot (%d renames)", renames);
    s = slurp(path);
    CHECK(!strcmp(s, BOOT), "boot line: [%s]", s);
    for (i = 1; i <= UI_LOG_KEEP; i++)
    {
        char g[sizeof(UI_LOG_FILE) + 12], want[24];
        snprintf(g, sizeof(g), "%s.%d", UI_LOG_FILE, i);
        snprintf(want, sizeof(want), "gen %d\n", i - 1);
        s = slurp(g);
        CHECK(!strcmp(s, want), "generation %d holds [%s]", i, s);
    }

    /* nothing pending: the notify returns without touching the disk */
    o = opens;
    idle();
    CHECK(opens == o, "idle with nothing pending opens nothing");

    /* lines go to the trace at once and to the file at the next idle */
    ui_log("splash-suppressed", "%s", "probe");
    ui_log("err", "code=%d", 7);
    CHECK(strstr(trace, "ipodui: splash-suppressed probe\n") != NULL,
          "trace line: %s", trace);
    CHECK(strstr(trace, "ipodui: err code=7\n") != NULL, "trace 2");
    idle();
    s = slurp(path);
    CHECK(!strcmp(s, BOOT "1 splash-suppressed probe\n2 err code=7\n"),
          "file after flush: [%s]", s);
    CHECK(strstr(trace, "ipodui: log flush lines=2 seq=1-2\n") != NULL,
          "flush trace");
    o = opens;
    idle();
    CHECK(opens == o, "second idle: nothing pending");

    /* a long line is cut to the slot, never overflows it */
    ui_log("t", "%s", "0123456789012345678901234567890123456789"
           "0123456789012345678901234567890123456789");
    idle();
    s = slurp(path);
    s = strstr(s, "3 t 0123");
    CHECK(s && strlen(s) == 2 + 61 + 1, "long line cut (%zu)",
          s ? strlen(s) : 0);

    /* overflow: 15 lines into 12 slots -> the 3 oldest are dropped */
    for (i = 0; i < 15; i++)
        ui_log("ov", (i & 1) ? "a=%d" : "b=%d", i); /* no repeats */
    idle();
    s = slurp(path);
    CHECK(strstr(s, "log dropped=3\n7 ov a=3\n") != NULL,
          "overflow drops oldest: [%s]", s);
    CHECK(strstr(s, "18 ov b=14\n") != NULL && !strstr(s, "ov b=2\n"),
          "overflow keeps newest");

    /* a line logged during a flush goes out on the next flush */
    ui_log("a", "one");
    write_hook = late_line;
    idle();
    s = slurp(path);
    CHECK(strstr(s, "19 a one\n") && !strstr(s, "late during flush"),
          "late line not in this flush");
    idle();
    s = slurp(path);
    CHECK(strstr(s, "20 late during flush\n") != NULL, "late line next");

    /* M3: the same fmt pointer within 1 s is counted, not logged */
    for (i = 0; i < 5; i++)
    {
        ui_log("splash-suppressed", "%s", "search");
        current_tick += HZ / 10;
    }
    ui_log("other", "x");
    idle();
    s = slurp(path);
    CHECK(strstr(s, "21 splash-suppressed search\n"
                    "22 splash-suppressed repeat=4\n23 other x\n") != NULL,
          "repeat collapse: [%s]", s);
    CHECK(strstr(trace, "ipodui: splash-suppressed repeat=4\n") != NULL,
          "repeat traced");
    /* a pending repeat is written by the flush; after 1 s it logs again */
    ui_log("r", "%s", "x");
    ui_log("r", "%s", "x");
    idle();
    current_tick += HZ;
    ui_log("r", "%s", "x");
    idle();
    s = slurp(path);
    CHECK(strstr(s, "24 r x\n25 r repeat=1\n26 r x\n") != NULL,
          "repeat at flush, then again after 1 s: [%s]", s);

    /* write error: the line is dropped and counted on the next flush */
    ui_log("w", "lost");
    fail_write_at = writes;         /* that line's write */
    idle();
    fail_write_at = -1;
    ui_log("w", "kept");
    idle();
    s = slurp(path);
    CHECK(strstr(s, "log dropped=1\n28 w kept\n") != NULL,
          "write error counted: [%s]", s);

    /* rotation: past UI_LOG_MAX the file moves to .1 once, but never on
     * the storage thread (fix round, finding 4): its flush appends and
     * marks the rotation due; ui_log_service() (UI thread) rotates only
     * while the disk spins */
    {
        FILE *f = fopen(path, "ab");
        for (i = 0; i < UI_LOG_MAX / 64 + 1; i++)
            fprintf(f, "%063d\n", i);
        fclose(f);
    }
    ui_log("rot", "late");
    o = renames;
    idle();
    s = slurp(path);
    CHECK(renames == o && strlen(s) > UI_LOG_MAX
          && strstr(s, "29 rot late\n"), "storage thread never rotates");
    disk_active = false;
    ui_log_service();
    CHECK(renames == o, "no rotation while the disk sleeps");
    disk_active = true;
    ui_log_service();
    CHECK(renames == o + UI_LOG_KEEP, "UI thread rotates while the disk spins");
    ui_log_service();
    CHECK(renames == o + UI_LOG_KEEP, "rotation runs once");
    disk_active = false;
    s = slurp(UI_LOG_FILE ".1");
    CHECK(strlen(s) > UI_LOG_MAX && strstr(s, "28 w kept\n")
          && strstr(s, "29 rot late\n"),
          "old file in .1");
    ui_log("rot", "fresh");
    idle();
    s = slurp(path);
    CHECK(!strcmp(s, "30 rot fresh\n"), "fresh file: [%s]", s);

    /* power-off path: the same flush, callable directly */
    ui_log("off", "bye");
    ui_log_flush();
    s = slurp(path);
    CHECK(strstr(s, "31 off bye\n") != NULL, "flush at power-off");

    /* the power-off flush (UI thread) rotates at once when due */
    {
        FILE *f = fopen(path, "ab");
        for (i = 0; i < UI_LOG_MAX / 64 + 1; i++)
            fprintf(f, "%063d\n", i);
        fclose(f);
    }
    ui_log("off", "again");
    ui_log_flush();
    s = slurp(path);
    CHECK(!strcmp(s, "32 off again\n"), "power-off rotates: [%s]", s);

    /* fix round, finding 3: one "%s" with different texts is not a repeat */
    ui_log("splash-suppressed", "%s", "alpha");
    ui_log("splash-suppressed", "%s", "beta");
    ui_log("splash-suppressed", "%s", "beta");
    ui_log("splash-suppressed", "%s", "alpha");
    idle();
    s = slurp(path);
    CHECK(strstr(s, "33 splash-suppressed alpha\n34 splash-suppressed beta\n"
                    "35 splash-suppressed repeat=1\n"
                    "36 splash-suppressed alpha\n") != NULL,
          "different texts under one fmt: [%s]", s);

    /* S36: half the ring pending, the disk awake, audio stopped: the UI
     * thread writes it (a disk that never sleeps never runs the idle
     * flush); not while audio plays, not while the disk sleeps, not
     * below half */
    for (i = 0; i < 5; i++)
        ui_log("act", (i & 1) ? "a=%d" : "b=%d", i);
    disk_active = true;
    o = opens;
    ui_log_service();
    CHECK(opens == o, "below half the ring: kept");
    ui_log("act", "c=%d", 5);
    audio_state = 1;
    ui_log_service();
    CHECK(opens == o, "playing: kept");
    audio_state = 0;
    disk_active = false;
    ui_log_service();
    CHECK(opens == o, "disk asleep: kept");
    disk_active = true;
    ui_log_service();
    s = slurp(path);
    CHECK(opens == o + 1 && strstr(s, "37 act b=0\n") && strstr(s, "42 act c=5\n"),
          "active-disk flush: [%s]", s);
    disk_active = false;

    /* the bench session (sitting 3, 2026-10-26: a held video step logged
     * 150 lines and the 12-line ring dropped 138): its ring keeps a held
     * step's lines, its file rotates later, and the end puts the static
     * ring back, keeping the newest pending lines */
    ui_log("pre", "pending");
    ui_log_bench(true);
    CHECK(t_core_live == 1, "bench ring allocated");
    s = slurp(path);
    CHECK(strstr(s, "43 pre pending\n") != NULL, "flushed at the switch: [%s]", s);
    ui_log_hold(true);
    for (i = 0; i < 150; i++)
        ui_log("held", (i & 1) ? "a=%d" : "b=%d", i);
    idle();
    CHECK(!strstr(slurp(path), "held b=0"), "held: nothing written");
    ui_log_hold(false);
    idle();
    s = slurp(path);
    CHECK(strstr(s, "held b=0\n") && strstr(s, "held a=149\n") &&
          !strstr(s, "log dropped"), "150 held lines kept: [%.200s]", s);
    {
        FILE *f = fopen(path, "ab");
        for (i = 0; i < UI_LOG_MAX / 64 + 1; i++)
            fprintf(f, "%063d\n", i);
        fclose(f);
    }
    o = renames;
    ui_log("big", "file");
    ui_log_flush();
    CHECK(renames == o, "no rotation past UI_LOG_MAX in the session");
    ui_log_hold(true);
    for (i = 0; i < 20; i++)
        ui_log("end", (i & 1) ? "a=%d" : "b=%d", i);
    ui_log_hold(false);
    ui_log_bench(false);            /* held lines flushed first */
    CHECK(t_core_live == 0, "bench ring freed");
    s = slurp(path);
    CHECK(strstr(s, "end b=0\n") && strstr(s, "end a=19\n"),
          "the end flushes the session's lines");
    ui_log_hold(true);              /* back to 12: 15 lines drop 3 */
    for (i = 0; i < 15; i++)
        ui_log("small", (i & 1) ? "a=%d" : "b=%d", i);
    ui_log_hold(false);
    o = renames;
    ui_log_flush();                 /* the normal size again: rotates */
    CHECK(renames == o + UI_LOG_KEEP, "rotation past UI_LOG_MAX after it");
    s = slurp(path);
    CHECK(strstr(s, "log dropped=3\n") && strstr(s, "small a=3\n") &&
          !strstr(s, "small b=2\n"), "the static ring again: [%s]", s);
    /* a switch with more pending than the small ring keeps the newest */
    ui_log_bench(true);
    ui_log_hold(true);
    for (i = 0; i < 20; i++)
        ui_log("sw", (i & 1) ? "a=%d" : "b=%d", i);
    ui_log_bench(false);            /* held: no flush, 21 (the switch's
                                       own line too) into 12 */
    ui_log_hold(false);
    ui_log_flush();
    s = slurp(path);
    CHECK(strstr(s, "log dropped=9\n") && strstr(s, "sw b=8\n") &&
          strstr(s, "sw a=19\n") && !strstr(s, "sw a=7\n"),
          "the switch back keeps the newest 12: [%s]", s);
    t_core_fail = 1;
    ui_log_bench(true);
    CHECK(t_core_live == 0, "no memory: the small ring stays");
    ui_log_flush();
    CHECK(strstr(slurp(path), "log bench ring: no memory") != NULL,
          "no memory is logged");

    for (i = 0; i <= UI_LOG_KEEP; i++)
    {
        char g[sizeof(UI_LOG_FILE) + 12];
        snprintf(g, sizeof(g), i ? "%s.%d" : "%s", UI_LOG_FILE, i);
        unlink(g);
    }
    if (fails)
    {
        printf("%d failure(s)\n", fails);
        return 1;
    }
    return 0;
}
