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
 * iPod UI shell: the error/splash log (Phase 8a U2; docs/phase8a-brief.md
 * §3.3, §4.5, Revision 1 H3, M3). Errors and suppressed splashes are
 * logged, never shown (CLAUDE.md rule 1).
 *
 * A 768 B static ring of 12 lines, each also traced as DEBUGF
 * ("ipodui: <tag> <text>"; a no-op on the device). The ring is appended
 * to /.rockbox/ipodui.log from a persistent DISK_EVENT_SPINUP handler,
 * i.e. when the disk is about to sleep anyway: 0 extra wake-ups. Also at
 * SYS_POWEROFF (input.c).
 *
 * Threads: ui_log() may run on any CPU-core thread; the kernel is
 * cooperative per core and the writer never yields, so it needs no lock.
 * COP callers (P19 S0) write a small ring of their own in uncached memory
 * (single producer: the COP, whose writer never yields either; single
 * consumer: the CPU), which the CPU drains into this ring, each line
 * marked "c=1" after its tag, at its next ui_log() and before every flush;
 * a full COP ring drops and counts. The flusher (storage thread, or
 * the UI thread at power-off; a flag keeps them apart) fixes the last
 * seq at its start, copies one line at a time into a 64 B local and
 * re-checks the slot's seq before writing it: a line logged meanwhile
 * gets a later seq and goes out on the next flush, never torn.
 *
 * Each boot rolls the logs one generation on (.1 to .5, UI_LOG_KEEP) and
 * starts the new file with a boot line (build id, RTC time), written
 * directly so the ring's early overflow can't lose it (S36: six boots
 * kept one previous log and a day's evidence was gone). A ring with
 * half its lines pending is also written by the UI thread when the disk
 * is awake anyway and audio is stopped (a disk that never sleeps never
 * runs the idle flush); never during playback.
 *
 * Rotation (fix round, finding 4): remove()/rename() never run on the
 * storage thread's 2 KB stack. A flush there that finds the file past
 * UI_LOG_MAX only marks the rotation due and appends; the UI thread
 * rotates in ui_log_service() while the disk spins anyway (no extra
 * wake-up), or at power-off in ui_log_flush().
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
#include "config.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "storage.h"
#include "ata_idle_notify.h"
#include "debug.h"
#include "audio.h"
#include "core_alloc.h"             /* the bench session's ring */
#include "timefuncs.h"
#include "version.h"
#include "ipodui/uilog.h"
#include "ipodui/safefile.h"
#include "ipodui/fe_be.h"           /* P19 S2/S7: routing, the drain kick */
#if NUM_CORES > 1
#include "xcore.h"                  /* P19 S0: the COP ring */
#endif
#if (CONFIG_STORAGE & STORAGE_ATA) && !defined(SIMULATOR)
#include "ata.h"
#include "fat.h"                    /* fat_slot_fault */
#endif
#ifdef HAVE_PANIC_RECORD
#include "panic_record.h"
#include "hang_watch.h"
#endif

#define RING_LINES  (UI_LOG_RING / sizeof(struct ui_log_line))
#define LINES       ring_lines
#define REPEAT_TICKS HZ             /* M3: same fmt within 1 s is counted */

typedef char ring_fits[sizeof(struct ui_log_line) == UI_LOG_LINE
                       && RING_LINES == 12 ? 1 : -1] __attribute__((unused));

/* The ring: the static 12 lines, or the bench session's larger one
 * (ui_log_bench).  Indexed by seq % ring_lines; a slot's seq says whether
 * it still holds that line, so a switch between rings (UI thread, no
 * yield while it moves the pending lines) never tears a line, and a
 * flush that yields mid-way re-reads both globals at each line. */
static struct ui_log_line ring_static[RING_LINES];
static struct ui_log_line *ring = ring_static;
static unsigned int ring_lines = RING_LINES;
static long log_max = UI_LOG_MAX;   /* the rotation size: UI_LOG_BENCH_MAX
                                       while the bench session runs */
static int bench_h;                 /* the bench ring's allocation; 0 none */
/* the ring between bench sessions: the boot ring once allocated, else the
 * static one */
static struct ui_log_line *base_ring = ring_static;
static unsigned int base_lines = RING_LINES;
static unsigned short next_seq = 1; /* seq of the next line */
static unsigned short done_seq;     /* last seq appended to the file */
static unsigned short dropped;      /* lost: overwritten unflushed, COP */
static unsigned short rep_count;    /* repeats of the last line not logged */
static const char *rep_fmt, *rep_tag;
static unsigned long rep_hash;      /* of the last line's text: "%s" alone
                                       must not collapse different texts */
static long rep_tick;
static bool registered, flushing, rotate_due;
static bool held;                   /* ui_log_hold(): lines stay in RAM */
/* the line being logged; the writer never yields, so one is enough */
static char scratch[sizeof(ring[0].text)];

#if NUM_CORES > 1
/* P19 S0: the COP's lines (see the top): head is the COP's, tail and
 * cop_seen the CPU's, cop_lost the COP's; all uncached but cop_seen */
#define COP_LINES 16                 /* P19 S7: 1,024 B (the 1 KB static
                                     * rule's limit; a power of two, as the
                                     * 16-bit indices wrap); the shell on
                                     * the COP logs in bursts, drained by
                                     * be_svc at once */
static struct ui_log_line cop_ring[COP_LINES] XCORE_BSS;
static volatile unsigned short cop_head XCORE_BSS, cop_tail XCORE_BSS;
static volatile unsigned short cop_lost XCORE_BSS;
static unsigned short cop_seen;
#endif

static unsigned long text_hash(const char *t)
{
    unsigned long h = 2166136261u;  /* FNV-1a */
    while (*t)
        h = (h ^ (unsigned char)*t++) * 16777619u;
    return h;
}

/* Formats "<tag> <text>" into scratch (never yields). */
static void format(const char *tag, const char *fmt, va_list ap)
{
    int n = snprintf(scratch, sizeof(scratch), "%s ", tag);

    if (n < 0 || n >= (int)sizeof(scratch))
        n = sizeof(scratch) - 1;
    vsnprintf(scratch + n, sizeof(scratch) - n, fmt, ap);
}

/* The next ring slot; its text is written, then commit() (never yields) */
static struct ui_log_line *slot(void)
{
    struct ui_log_line *l = &ring[next_seq % LINES];

    if ((short)(l->seq - done_seq) > 0)
        dropped++;                  /* the oldest line was never flushed */
    return l;
}

/* A "<tag> FAULT ..." line (a bounded wait that ran out, a broken
 * invariant): written out at the next ui_log_service(), not at the next
 * idle, so a hang that follows it keeps it (the freeze audit) */
static bool fault_due;

static void commit(struct ui_log_line *l)
{
    const char *t = strchr(l->text, ' ');

    l->seq = next_seq++;
    if (t && !strncmp(t + 1, "FAULT ", 6))
        fault_due = true;
    DEBUGF("ipodui: %s\n", l->text);
}

#if NUM_CORES > 1
static void put_repeats(void);

/* The COP: "<tag> c=1 <text>" straight into its ring's next line */
static void cop_put(const char *tag, const char *fmt, va_list ap)
{
    unsigned short h = cop_head;
    bool was_empty = h == cop_tail;
    struct ui_log_line *l;
    int n;

    if ((unsigned short)(h - cop_tail) >= COP_LINES)
    {
        cop_lost++;
        return;
    }
    l = &cop_ring[h % COP_LINES];
    n = snprintf(l->text, sizeof(l->text), "%s c=1 ", tag);
    if (n < 0 || n >= (int)sizeof(l->text))
        n = sizeof(l->text) - 1;
    vsnprintf(l->text + n, sizeof(l->text) - n, fmt, ap);
    cop_head = h + 1;               /* published after its text */
    if (was_empty)
        fe_be_log_kick();           /* be_svc drains it (P19 S7) */
}


/* The CPU: the COP's lines into the ring (never yields) */
static void cop_drain(void)
{
    unsigned short lost = cop_lost;

    if (cop_tail != cop_head)
    {
        /* the CPU's pending repeat count goes first, and a COP line in
         * between ends the run (S0 review L3) */
        put_repeats();
        rep_fmt = NULL;
    }
    while (cop_tail != cop_head)
    {
        struct ui_log_line *l = slot();

        memcpy(l->text, cop_ring[cop_tail % COP_LINES].text,
               sizeof(l->text));
        l->text[sizeof(l->text) - 1] = '\0';
        commit(l);
        cop_tail = cop_tail + 1;
    }
    if (lost != cop_seen)
    {
        dropped += (unsigned short)(lost - cop_seen);
        cop_seen = lost;
    }
}

/* P19 S7: be_svc, on the COP's first line since the last drain */
void ui_log_drain(void)
{
    if (CURRENT_CORE == CPU)
        cop_drain();
}
#else
#define cop_drain() do { } while (0)
#endif

static void put_repeats(void)
{
    if (rep_count)
    {
        struct ui_log_line *l = slot();
        snprintf(l->text, sizeof(l->text), "%s repeat=%u", rep_tag,
                 rep_count);
        commit(l);
        rep_count = 0;
    }
}

void ui_log_va(const char *tag, const char *fmt, va_list ap)
{
    long now = current_tick;
    struct ui_log_line *l;
    unsigned long h;

#if NUM_CORES > 1
    if (CURRENT_CORE != CPU)
    {
        cop_put(tag, fmt, ap);      /* P19 S0: the COP's own ring */
        return;
    }
#endif
    cop_drain();
    format(tag, fmt, ap);
    h = text_hash(scratch);
    if (fmt == rep_fmt && tag == rep_tag && h == rep_hash
        && now - rep_tick < REPEAT_TICKS)
    {
        rep_count++;
        rep_tick = now;
        return;
    }
    put_repeats();
    rep_fmt = fmt;
    rep_tag = tag;
    rep_hash = h;
    rep_tick = now;
    l = slot();
    memcpy(l->text, scratch, sizeof(l->text));
    commit(l);
}

void ui_log(const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    ui_log_va(tag, fmt, ap);
    va_end(ap);
}

/* generation g's name: 0 the live file, 1..UI_LOG_KEEP the older boots */
static void gen_name(char *out, size_t size, int g)
{
    if (g)
        snprintf(out, size, "%s.%d", UI_LOG_FILE, g);
    else
        snprintf(out, size, "%s", UI_LOG_FILE);
}

/* The generations move on by one: .4 to .5 (the old .5 goes), ..., the
 * file to .1 (UI thread only: see the top). Crash-safe (tests/fatcrash
 * site uilog-rotate): the only removal is the oldest generation, and only
 * once .4 exists to replace it; every rename lands on a free name (the
 * overwrite rename can leave two entries for a name), highest first, so a
 * cut leaves every file whole and at most one gap, which the next boot's
 * rotation steps over. A cut inside a rename can leave two names on one
 * chain: those second names go first, by name only (safefile_remove()
 * never frees a chain its partner still holds). A name held by a distinct
 * file is never replaced. */
static void rotate(void)
{
    char from[sizeof(UI_LOG_FILE) + 12], to[sizeof(UI_LOG_FILE) + 12];
    int g;

    for (g = 0; g < UI_LOG_KEEP; g++)
    {
        gen_name(from, sizeof(from), g);
        gen_name(to, sizeof(to), g + 1);
        if (file_exists(from) && file_exists(to))
            safefile_drop_alias(to, from);
    }
    for (g = UI_LOG_KEEP - 1; g >= 0; g--)
    {
        gen_name(from, sizeof(from), g);
        gen_name(to, sizeof(to), g + 1);
        if (!file_exists(from))
            continue;
        if (g + 1 == UI_LOG_KEEP && file_exists(to)
            && !safefile_remove(to, from))
            continue;               /* the oldest goes only to be replaced */
        if (!file_exists(to))
            rename(from, to);
    }
    rotate_due = false;
}

/* The new file's first line: which build, and when by the RTC (the tick
 * restarts at every boot). One write while the disk spins for the boot. */
static void boot_line(void)
{
    char buf[UI_LOG_LINE + 8];
    struct tm *tm = get_time();
    int fd = open(UI_LOG_FILE, O_WRONLY | O_CREAT | O_APPEND, 0666);
    int n;

    if (fd < 0)
        return;
    n = snprintf(buf, sizeof(buf),
                 "boot build=%.24s rtc=%04d-%02d-%02d %02d:%02d:%02d\n",
                 rbversion, tm->tm_year + 1900, tm->tm_mon + 1,
                 tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);
    if (n > (int)sizeof(buf) - 1)
        n = sizeof(buf) - 1;
    write(fd, buf, n);
    close(fd);
}

/* Opens the log for appending. Past UI_LOG_MAX it rotates first on the
 * UI thread; the storage thread only marks the rotation due. */
static int open_log(bool ui_thread)
{
    int fd = open(UI_LOG_FILE, O_WRONLY | O_CREAT | O_APPEND, 0666);

    if (fd >= 0 && ffilesize(fd) > log_max)
    {
        if (!ui_thread)
        {
            rotate_due = true;
            return fd;
        }
        close(fd);
        rotate();                   /* a live file that stayed is appended to,
                                       never truncated */
        fd = open(UI_LOG_FILE, O_WRONLY | O_CREAT | O_APPEND, 0666);
    }
    return fd;
}

/* Stack: buf (72: "<seq> " + text + newline) + a few words, < 128 B (H3).
 * One write per line, so an error never leaves half a line. */
static void flush(bool ui_thread)
{
    char buf[UI_LOG_LINE + 8];
    unsigned short last, first;
    int fd, lines = 0;

    if (flushing || held)
        return;
    cop_drain();
    put_repeats();
    last = next_seq - 1;
    if (last == done_seq && !dropped)
    {
        fault_due = false;
        return;                     /* nothing pending: the common case */
    }
    flushing = true;
    fd = open_log(ui_thread);
    if (fd < 0)
    {
        flushing = false;           /* kept: retried at the next idle */
        return;
    }
    fault_due = false;              /* this flush writes it */
    if (dropped)
    {
        int n = snprintf(buf, sizeof(buf), "log dropped=%u\n", dropped);
        dropped = 0;
        write(fd, buf, n);
    }
    first = done_seq + 1;
    while (done_seq != last)
    {
        unsigned short s = done_seq + 1;
        const struct ui_log_line *l;
        int n;

        if ((short)(last - s) >= (short)LINES)
            s = last - LINES + 1;   /* overwritten (already counted) */
        l = &ring[s % LINES];
        if (l->seq != s)
        {
            done_seq = s;           /* overwritten meanwhile: skip */
            continue;
        }
        n = snprintf(buf, sizeof(buf), "%u %.*s\n", s,
                     (int)sizeof(l->text) - 1, l->text);
        if (write(fd, buf, n) != n)
        {
            done_seq = s;           /* this line is lost, counted */
            dropped++;
            break;
        }
        done_seq = s;
        lines++;
    }
    close(fd);
    flushing = false;
    DEBUGF("ipodui: log flush lines=%d seq=%u-%u\n", lines, first, last);
    (void)lines;                    /* DEBUGF is a no-op on the device */
    (void)first;
}

#ifdef FAT_SOFT_EMPTY_SLOT
/* 2026-10-23 freeze: a close found its directory entry empty (fat.c no
 * longer panics). One case's detail, flushed at once; the count also at
 * power-off (ui_log_flush()) */
static unsigned long slot_seq_logged;

static void log_dirent(const char *what, const uint8_t *e)
{
    static const char hex[] = "0123456789abcdef";
    char h[33];
    for (int half = 0; half < 2; half++)
    {
        for (int i = 0; i < 16; i++)
        {
            h[2 * i] = hex[e[16 * half + i] >> 4];
            h[2 * i + 1] = hex[e[16 * half + i] & 15];
        }
        h[32] = '\0';
        ui_log("fat", "slot %s %c %s", what, half ? 'b' : 'a', h);
    }
}

static void slot_fault_service(void)
{
    struct fat_slot_fault f;                    /* one copy: UI thread */

    if (fat_slot_fault_get(NULL) == slot_seq_logged)
        return;
    fat_slot_fault_get(&f);
    slot_seq_logged = f.seq;
    ui_log("fat", "empty slot n=%lu thread=%.15s from_disk=%d",
           f.count, f.thread, (int)f.from_disk);
    ui_log("fat", "empty slot dircl=%lx entry=%lu firstcl=%lx size=%lu",
           (unsigned long)f.dircluster, f.entry,
           (unsigned long)f.firstcluster, f.size);
    flush(true);
    for (int i = 0; i < 3; i++)
        if (f.have[i])
            log_dirent(i == 0 ? "prev" : i == 1 ? "this" : "next", f.raw[i]);
    flush(true);
}
#endif

void ui_log_hold(bool on)
{
    held = on;
}

/* The pending lines (written to the file or not, still in their slots)
 * into the ring `to` of n lines; the oldest that don't fit are counted
 * dropped.  Never yields. */
static void ring_move(struct ui_log_line *to, unsigned int n)
{
    unsigned short s = done_seq + 1, last = next_seq - 1;

    memset(to, 0, n * sizeof(*to));
    if ((short)(last - done_seq) > (short)n)
    {
        dropped += (unsigned short)(last - done_seq - n);
        s = last - n + 1;
    }
    for (; (short)(last - s) >= 0; s++)
    {
        const struct ui_log_line *l = &ring[s % ring_lines];

        if (l->seq == s)
            to[s % n] = *l;
    }
    ring = to;
    ring_lines = n;
}

void ui_log_bench(bool on)
{
    int h;

#if NUM_CORES > 1
    if (CURRENT_CORE != CPU)
        return;                     /* the ring is the CPU's (the UI on
                                       the COP keeps the small one) */
#endif
    if (on == (bench_h > 0))
        return;
    cop_drain();
    put_repeats();
    if (!flushing && !held)
        flush(true);                /* the disk spins for the bench anyway */
    if (on)
    {
        h = core_alloc(UI_LOG_BENCH_LINES * sizeof(struct ui_log_line));
        if (h <= 0)
        {
            ui_log("log", "bench ring: no memory (%u lines)", ring_lines);
            return;
        }
        core_pin(h);
        ring_move(core_get_data(h), UI_LOG_BENCH_LINES);
        bench_h = h;
        log_max = UI_LOG_BENCH_MAX;
        ui_log("log", "bench ring lines=%u max=%ld", ring_lines, log_max);
        return;
    }
    ring_move(base_ring, base_lines);
    h = bench_h;
    bench_h = 0;
    log_max = UI_LOG_MAX;
    core_unpin(h);
    core_free(h);
}

void ui_log_flush(void)              /* UI thread (power-off) */
{
    if (fe_be_route())
    {
        (void)be_call(BE_LOG_FLUSH, 0, 0, 0);  /* P19 S2: on be_svc */
        return;
    }
#ifdef FAT_SOFT_EMPTY_SLOT
    {
        struct fat_slot_fault f;
        fat_slot_fault_get(&f);
        if (f.count)
            ui_log("fat", "empty slot total=%lu", f.count);
    }
#endif
    flush(true);
}

#ifdef HAVE_PANIC_RECORD
/* panic_record_tail_hook: the ring's lines, oldest first, into the
 * record at a panic. Interrupts are off: no locks, no calls but these
 * loops; a line being written may come out torn, never out of bounds.
 * Lines not yet in the file are marked '*' */
static void panic_tail(char *buf, unsigned int size)
{
    unsigned int pos = 0;

    for (unsigned int k = LINES; k > 0 && pos + 2 < size; k--)
    {
        unsigned short seq = (unsigned short)(next_seq - k);
        const struct ui_log_line *l = &ring[seq % LINES];
        if (l->seq != seq || !seq)
            continue;
        buf[pos++] = (short)(seq - done_seq) > 0 ? '*' : ' ';
        for (unsigned int i = 0; i < sizeof(l->text) && l->text[i]
                                 && pos + 2 < size; i++)
            buf[pos++] = l->text[i];
        buf[pos++] = '\n';
    }
    buf[pos] = '\0';
}
#endif

#ifdef HAVE_PANIC_RECORD
/* The record's lines go straight to the file, ahead of the ring: a boot
 * whose early flushes fail (the 12-line ring overflowing before the disk
 * takes a write) must never drop them (P12 device bench, 2026-10-24).
 * "p " instead of a sequence number; through the ring only when the file
 * can't be opened. */
static int panic_fd = -1;

static void ATTRIBUTE_PRINTF(1, 2) panic_line(const char *fmt, ...)
{
    char buf[UI_LOG_LINE + 16];
    va_list ap;
    int n;

    va_start(ap, fmt);
    if (panic_fd >= 0)
    {
        n = snprintf(buf, sizeof(buf), "p panic ");
        n += vsnprintf(buf + n, sizeof(buf) - n - 1, fmt, ap);
        if (n > (int)sizeof(buf) - 2)
            n = sizeof(buf) - 2;
        buf[n] = '\0';
        DEBUGF("ipodui: %s\n", buf + 2);   /* the sim's trace, as commit() */
        buf[n++] = '\n';
        write(panic_fd, buf, n);
    }
    else
        ui_log_va("panic", fmt, ap);
    va_end(ap);
}

void ui_log_panic_record(void)
{
    const struct panic_record *p = panic_record_take(panic_record_area());
    size_t i, len;

    if (!p)
        return;
    panic_record_sync();
    flush(true);                    /* what came before, in order */
    panic_fd = open_log(true);
    if (p->kind == PANIC_REC_HANG)
        /* the hang watcher's (hang_watch.h): no fault, a thread stuck */
        panic_line("hang record thread=%.*s count=%u", PANIC_REC_THREAD,
                   p->thread, (unsigned)p->count);
    else
        panic_line("kind=%u count=%u thread=%s", (unsigned)p->kind,
                   (unsigned)p->count, p->thread);
    panic_line("pc=%08x lr=%08x sp=%08x", (unsigned)p->pc,
               (unsigned)p->lr, (unsigned)p->sp);
    len = strlen(p->msg);           /* the sum held: write() ended it */
    for (i = 0; i < len; i += 48)
        panic_line("msg %.48s", p->msg + i);
    panic_line("uptime=%lu trace %08x %08x %08x %08x",
               (unsigned long)p->uptime, (unsigned)p->trace[0],
               (unsigned)p->trace[1], (unsigned)p->trace[2],
               (unsigned)p->trace[3]);
    panic_line("trace %08x %08x %08x %08x", (unsigned)p->trace[4],
               (unsigned)p->trace[5], (unsigned)p->trace[6],
               (unsigned)p->trace[7]);
    /* a hang's stack high-waters and lock owners */
    for (len = 0; len < PANIC_REC_INFO && p->info[len]; len++)
        ;
    for (i = 0; i < len; i += 48)
        panic_line("info %.48s", p->info + i);
    /* the lines the ring held at the panic (the file may have lost them) */
    {
        const char *t = p->tail;
        int n = 0;
        while (*t)
        {
            const char *e = strchr(t, '\n');
            int l = e ? (int)(e - t) : (int)strlen(t);
            panic_line("tail %.*s", l > 54 ? 54 : l, t);
            t += l + (e ? 1 : 0);
            if (panic_fd < 0 && ++n % 6 == 0)
                flush(true);        /* the ring path: never drop one */
        }
    }
    if (panic_fd >= 0)
        close(panic_fd);
    panic_fd = -1;
    flush(true);
}
#endif

#ifdef HAVE_PANIC_RECORD
static void panic_guard_service(void)
{
#ifdef HAVE_IPOD_UI
    static bool cleared;
    /* this boot has lived past the guard window: the next panic restarts
     * again rather than switching off */
    if (!cleared && TIME_AFTER(current_tick, PANIC_GUARD_TICKS))
    {
        cleared = true;
        if (panic_guard_armed())
        {
            panic_guard_clear();
            ui_log("panic", "restart guard cleared");
        }
    }
#endif
}
#endif

void ui_log_service(void)
{
#ifdef HAVE_PANIC_RECORD
    panic_guard_service();
#endif
#ifdef HAVE_HANG_RECORD
    {
        /* the hang watcher recorded a thread that has since gone on: a
         * long stall, not a freeze; its record now, then watch again */
        long secs;
        const char *who;
        if (hang_take_recovered(&secs, &who))
        {
            ui_log("hang", "recovered %s after %lds; its record:", who,
                   secs);
            ui_log_panic_record();
            hang_rearm();
        }
    }
#endif
#ifdef FAT_SOFT_EMPTY_SLOT
    slot_fault_service();
#endif
#if (CONFIG_STORAGE & STORAGE_ATA) && !defined(SIMULATOR)
    /* #91 M2: a write-order barrier that failed, or a drive with no
     * FLUSH CACHE (no barrier holds): once per boot */
    static bool flush_noted;
    if (!flush_noted)
    {
        struct ata_flush_stats fs;
        ata_flush_stats_get(&fs);
        if (fs.fail || fs.nosup)
        {
            flush_noted = true;
            ui_log("ata", "barrier flush fail=%lu nosup=%lu of n=%lu",
                   fs.fail, fs.nosup, fs.n);
        }
    }
#endif
    if (rotate_due && !flushing && !held && storage_disk_is_active())
    {
        flushing = true;            /* keeps the storage thread's flush out */
        rotate();
        flushing = false;
    }
    /* a FAULT line: now, whatever the disk and audio (rare: each cause
     * logs once a boot), so a hang after it still leaves it in the file */
    if (fault_due && !flushing && !held)
        flush(true);
    /* half the ring pending, the disk awake anyway, audio stopped: write
     * now rather than wait for an idle flush that may never come */
    if (!flushing && (unsigned short)(next_seq - 1 - done_seq) >= LINES / 2
        && storage_disk_is_active() && audio_status() == 0)
        flush(true);
}

static void idle_cb(unsigned short id, void *data, void *user)
{
    (void)id;
    (void)data;
    (void)user;
    flush(false);                   /* storage thread: never rotates */
}

/* The boot ring (UI_LOG_BOOT_LINES), once, on the CPU: kept for the
 * boot, pinned so the ring never moves under a writer */
static void boot_ring(void)
{
    static bool tried;
    int h;

#if NUM_CORES > 1
    if (CURRENT_CORE != CPU)
        return;
#endif
    if (tried || bench_h > 0)
        return;
    tried = true;
    h = core_alloc(UI_LOG_BOOT_LINES * sizeof(struct ui_log_line));
    if (h <= 0)
        return;                     /* the static ring stays (silently: it
                                       is the normal small-memory case) */
    core_pin(h);
    ring_move(core_get_data(h), UI_LOG_BOOT_LINES);
    base_ring = ring;
    base_lines = ring_lines;
    ui_log("log", "boot ring lines=%u", ring_lines);
}

/* The first call (UI thread, early in boot, while the disk spins for the
 * boot anyway) rolls the logs one generation on, so each boot starts a
 * fresh file and a pulled log holds this boot's lines only (owner,
 * 2026-10-13), with the last UI_LOG_KEEP boots beside it (S36). */
void ui_log_init(void)
{
    static bool rolled;

    if (!rolled)
    {
        rolled = true;
        rotate();
        boot_line();
    }
    if (!registered)
        registered = add_event_ex(DISK_EVENT_SPINUP, false, idle_cb, NULL);
    boot_ring();
#ifdef HAVE_PANIC_RECORD
    panic_record_tail_hook = panic_tail;
#endif
}
