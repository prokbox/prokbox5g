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
 * iPod UI shell: Settings > About > Save Diagnostics (release-plan 6.2).
 * The report a user attaches to a problem report, built and written only
 * when they ask for it:
 *  - the version, build and variant; the hardware revision and RAM; the
 *    volume's size and free space and the drive's kind; the battery; the
 *    uptime; the library as counts; the VideoCore image's state;
 *  - the last panic record (whole, logged or not: panic_record_peek());
 *  - the tail of this boot's log and of the boot before.
 * Privacy (release-plan 6.2): every text that may hold a name (the
 * panic's message and tail, every log line) goes through
 * diag_scrub_line() (diag_core.c, host-tested): paths become <path>,
 * names <title>, with the playing track's own names and path parts as a
 * deny list. No serial number, no device name, no file names.
 * Memory: one core_alloc block (DIAG_MEM, pinned while files are read and
 * written) freed before return; no statics. One bulk write.
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
#include <stdio.h>
#include <string.h>
#include "string-extra.h"
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "core_alloc.h"
#include "mv.h"
#include "audio.h"
#include "metadata.h"
#include "powermgmt.h"
#include "power.h"
#include "timefuncs.h"
#include "version.h"
#include "lang.h"
#include "settings.h"
#include "ipodui/diag.h"
#include "ipodui/diag_core.h"
#include "ipodui/uilog.h"
#include "ipodui/library.h"
#ifdef HAVE_BATTERY_LEARN
#include "ipodui/batcal.h"
#endif
#ifdef HAVE_PANIC_RECORD
#include "panic_record.h"
#endif
#if (CONFIG_STORAGE & STORAGE_ATA) && !defined(SIMULATOR)
#include "ata.h"
#endif
#ifndef SIMULATOR
#include "hwcompat.h"
#endif
#if defined(HAVE_BCMFW) && defined(HAVE_BCMFW_RESIDENT)
#include "bcm-devtest.h"
#include "video/bcmvideo_image.h"
#include "video/bcmvideo_clock.h"
#include "video/vclock_gate.h"
#ifdef HAVE_BCMFW_VIDEO
#include "video/vplayer.h"
#endif
#endif

#define DIAG_OUT        7168    /* the report: a few KB */
#define DIAG_WIN        3072    /* a log's last bytes, read at once */
#define DIAG_DENY_BUF   (MAX_PATH + 5 * 64)
#define DIAG_DENY_N     16
#define DIAG_MEM        (DIAG_OUT + DIAG_WIN + DIAG_DENY_BUF)
#define DIAG_LOG_LINES  40      /* this boot */
#define DIAG_PREV_LINES 20      /* the boot before */

struct diag_ctx {
    struct diag_buf b;
    char *win;
    const char *deny[DIAG_DENY_N];
    int ndeny;
    unsigned hits;
};

static const char *yesno(bool v)
{
    return v ? "yes" : "no";
}

/* The playing track's names and the parts of its path, copied (the track
 * may change while files are read): what the scrubber must never let
 * through even where no rule would see a name */
static void deny_list(struct diag_ctx *c, char *mem, size_t size)
{
    const struct mp3entry *id3 = audio_status() ? audio_current_track()
                                                : NULL;
    const char *names[5];
    size_t used = 0;
    int i;

    c->ndeny = 0;
    if (!id3)
        return;
    names[0] = id3->title;
    names[1] = id3->artist;
    names[2] = id3->album;
    names[3] = id3->albumartist;
    names[4] = id3->composer;
    for (i = 0; i < 5; i++)
    {
        size_t n = names[i] ? strlen(names[i]) : 0;

        if (n < DIAG_DENY_MIN || n > 63 || used + n + 1 > size
            || c->ndeny >= DIAG_DENY_N)
            continue;
        memcpy(mem + used, names[i], n + 1);
        c->deny[c->ndeny++] = mem + used;
        used += n + 1;
    }
    /* the path, split at each '/', its last part without the extension */
    if (used + strlen(id3->path) + 1 <= size)
    {
        char *p = mem + used, *dot;

        strcpy(p, id3->path);
        dot = strrchr(p, '.');
        if (dot && !strchr(dot, '/'))
            *dot = '\0';
        while (*p && c->ndeny < DIAG_DENY_N)
        {
            char *e = strchr(p, '/');

            if (e)
                *e = '\0';
            if (strlen(p) >= DIAG_DENY_MIN)
                c->deny[c->ndeny++] = p;
            if (!e)
                break;
            p = e + 1;
        }
    }
}

/* The last lines of a log file, scrubbed: one read of its last bytes */
static void log_tail(struct diag_ctx *c, const char *file, int lines)
{
    int fd = open(file, O_RDONLY);
    off_t size, from;
    ssize_t n;
    size_t s;

    if (fd < 0)
    {
        diag_printf(&c->b, "  (none)\n");
        return;
    }
    size = ffilesize(fd);
    from = size > DIAG_WIN ? size - DIAG_WIN : 0;
    n = (lseek(fd, from, SEEK_SET) == from) ? read(fd, c->win, DIAG_WIN)
                                            : -1;
    close(fd);
    if (n <= 0)
    {
        diag_printf(&c->b, "  (empty)\n");
        return;
    }
    s = diag_tail(c->win, (size_t)n, from == 0, lines);
    diag_put_lines(&c->b, "  ", c->win + s, (size_t)n - s, c->deny,
                   c->ndeny, &c->hits);
}

static void put_system(struct diag_ctx *c)
{
    struct diag_buf *b = &c->b;
    struct tm *tm = get_time();
    sector_t size = 0, freek = 0;
    unsigned long up = current_tick / HZ;
    struct lib_stats st;
    char cal[48];

    diag_printf(b, "saved: %04d-%02d-%02d %02d:%02d:%02d\n",
                tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                tm->tm_hour, tm->tm_min, tm->tm_sec);
    diag_printf(b, "version: %s\n", str(LANG_IPODUI_VERSION_STRING));
    diag_printf(b, "build: %s (%s)\n", rbversion,
#ifdef HAVE_IPOD_UI_DEVMODE
                "developer"
#else
                "release"
#endif
                );
    /* storage-ram-battery-connector-haptics, as configure named it */
    diag_printf(b, "variant: %s\n", IPOD_VARIANT);
#ifdef SIMULATOR
    diag_printf(b, "hardware: simulator ram=%d MB\n", MEMORYSIZE);
#else
    diag_printf(b, "hardware: rev %08lx ram=%u MB\n",
                (unsigned long)IPOD_HW_REVISION, (unsigned)probed_ramsize);
#endif
    if (global_settings.ipodui_model[0])
        diag_printf(b, "model: %.15s\n",
                    (const char *)global_settings.ipodui_model);

    volume_size(IF_MV(0,) &size, &freek);       /* KiB, FSInfo in RAM */
    diag_printf(b, "storage: %s size=%lu MB free=%lu MB\n",
#if (CONFIG_STORAGE & STORAGE_ATA) && !defined(SIMULATOR)
                ata_disk_isssd() ? "solid" : "rotating",
#else
                "simulated",
#endif
                (unsigned long)(size / 1024), (unsigned long)(freek / 1024));

    diag_printf(b, "battery: %d%% %d mV charger=%s charging=%s",
                battery_level(), battery_voltage(),
                yesno(charger_inserted()), yesno(charging_state()));
#ifdef IPOD_BATTERY_MAH
    if (IPOD_BATTERY_MAH > 0)
        diag_printf(b, " cell=%d mAh", (int)IPOD_BATTERY_MAH);
    else
        diag_printf(b, " cell=stock");
#endif
    diag_printf(b, "\n");
#ifdef HAVE_BATTERY_LEARN
    batcal_status(cal, sizeof(cal));
    diag_printf(b, "battery table: %s\n", cal);
    batcal_run_status(cal, sizeof(cal));
    diag_printf(b, "battery learning: %s\n", cal);
#else
    (void)cal;
#endif
    diag_printf(b, "uptime: %lu:%02lu:%02lu\n", up / 3600, up / 60 % 60,
                up % 60);
    if (lib_stats(&st))
        diag_printf(b, "library: songs=%ld podcasts=%ld audiobooks=%ld\n",
                    (long)st.songs, (long)st.episodes, (long)st.books);
    else
        diag_printf(b, "library: not counted yet\n");
#if defined(HAVE_BCMFW) && defined(HAVE_BCMFW_RESIDENT)
    diag_printf(b, "video chip: image=%s fnv=%08lx resident=%s verified=%s "
                "clock150=%s\n", BCMVIDEO_IMAGE_KIND,
                (unsigned long)BCMVIDEO_IMAGE_FNV,
                yesno(bcm_boot_resident_active()),
                yesno(bcm_boot_resident_verified()),
                yesno(vclock_gate(BCMVIDEO_IMAGE_FNV, bcmvideo_clock150_fnv,
                                  BCMVIDEO_N_CLOCK150, false, false)
                      == VCLOCK_TOKEN));
#ifdef HAVE_BCMFW_VIDEO
    {
        /* the first-use check's verdict and the unit's token (the video
           error screen asks for this report) */
        char line[160];

        vplayer_check_summary(line, sizeof(line));
        diag_printf(b, "%s\n", line);
    }
#endif
#else
    diag_printf(b, "video chip: not in this build\n");
#endif
}

#ifdef HAVE_PANIC_RECORD
static size_t bounded_len(const char *s, size_t max)
{
    size_t n = 0;

    while (n < max && s[n])
        n++;
    return n;
}

static void put_panic(struct diag_ctx *c)
{
    struct diag_buf *b = &c->b;
    const struct panic_record *p = panic_record_peek(panic_record_area());

    if (!p)
    {
        diag_printf(b, "\nlast panic: none since power-on\n");
        return;
    }
    diag_printf(b, "\nlast %s: kind=%u count=%u thread=%.*s at=%lus%s\n",
                p->kind == PANIC_REC_HANG ? "hang record" : "panic",
                (unsigned)p->kind, (unsigned)p->count, PANIC_REC_THREAD,
                p->thread, (unsigned long)(p->uptime / HZ),
                p->magic == PANIC_REC_MAGIC ? " (not yet logged)" : "");
    diag_printf(b, "  pc=%08lx lr=%08lx sp=%08lx\n", (unsigned long)p->pc,
                (unsigned long)p->lr, (unsigned long)p->sp);
    diag_printf(b, "  trace");
    for (int i = 0; i < PANIC_REC_TRACE; i++)
        diag_printf(b, " %08lx", (unsigned long)p->trace[i]);
    diag_printf(b, "\n");
    diag_put_lines(b, "  msg ", p->msg, bounded_len(p->msg, PANIC_REC_MSG),
                   c->deny, c->ndeny, &c->hits);
    diag_put_lines(b, "  info ", p->info, bounded_len(p->info, PANIC_REC_INFO),
                   c->deny, c->ndeny, &c->hits);
    diag_put_lines(b, "  | ", p->tail, bounded_len(p->tail, PANIC_REC_TAIL),
                   c->deny, c->ndeny, &c->hits);
}
#endif

bool diag_save_report(void)
{
    struct diag_ctx c;
    char *mem;
    int h, fd;
    bool ok = false;

    /* the ring's pending lines into the file first, so the tail is now */
    ui_log_flush();
    h = core_alloc(DIAG_MEM);
    /* paused, playback refuses the shrink and posts the give instead: wait
     * for it, as usb_buf_alloc does (10 x 100 ms, about 1 s in all) */
    for (int i = 1; h <= 0 && i < 10; i++)
    {
        sleep(HZ / 10);
        h = core_alloc(DIAG_MEM);
    }
    if (h <= 0)
    {
        ui_log("diag", "report FAILED nomem need=%d", DIAG_MEM);
        return false;
    }
    core_pin(h);                    /* file I/O yields */
    mem = core_get_data(h);
    diag_init(&c.b, mem, DIAG_OUT);
    c.win = mem + DIAG_OUT;
    c.hits = 0;
    deny_list(&c, mem + DIAG_OUT + DIAG_WIN, DIAG_DENY_BUF);

    diag_printf(&c.b, "ProkBox diagnostics report\n"
                "Song, album, artist and book names and file paths are "
                "left out: they read <title> and <path>.\n\n");
    put_system(&c);
#ifdef HAVE_PANIC_RECORD
    put_panic(&c);
#endif
    diag_printf(&c.b, "\nlog, this start (last %d lines):\n", DIAG_LOG_LINES);
    log_tail(&c, UI_LOG_FILE, DIAG_LOG_LINES);
    diag_printf(&c.b, "\nlog, the start before (last %d lines):\n",
                DIAG_PREV_LINES);
    log_tail(&c, UI_LOG_FILE ".1", DIAG_PREV_LINES);
    diag_printf(&c.b, "\nfields left out: %u%s\n", c.hits,
                c.b.full ? " (report cut short)" : "");

    fd = open(DIAG_REPORT_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0)
    {
        ok = write(fd, c.b.p, c.b.len) == (ssize_t)c.b.len;
        if (close(fd) < 0)
            ok = false;
    }
    ui_log("diag", "report %s bytes=%u redacted=%u",
           ok ? "saved" : "FAILED", (unsigned)c.b.len, c.hits);
    core_unpin(h);
    core_free(h);
    return ok;
}
