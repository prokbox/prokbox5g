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

/* The "Battery savings" bench rows (devbench_awake.h): the device side.
 * Called from devbench.c's step table on the UI thread (the CPU); each
 * returns an enum bench_res and logs through devbcm_log() (bcmtest.log
 * and the results file). Every row forces its awake row for its own run
 * only (pwraw_force) and puts the gate back on every exit path; a row
 * that writes a register puts back the value it found and reads it back.
 * Evidence: what each touches is [rockbox-derived] (pp5020.h, pcf5060x.h)
 * or [apple-RE] (apple-power-catalog.md); every effect is [hypothesis]
 * until these rows pass on silicon. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "devbench_sim.h"         /* DEVBENCH_SIM */

#if !defined(SIMULATOR) && defined(HAVE_PWR_AWAKE) && \
    defined(HAVE_IPOD_UI_DEVMODE) && \
    defined(HAVE_BCMFW_VIDEO)   /* devbcm_log, devbench_ask (devbcm.h) */
#include "system.h"
#include "kernel.h"
#include "backlight.h"
#include "lcd.h"
#include "audio.h"
#include "playlist.h"
#include "playback.h"             /* awlg: audio_get_filebuflen */
#include "file.h"
#include "dir.h"
#include "rbpaths.h"
#include "string-extra.h"
#include "pcm.h"
#include "pcm-internal.h"         /* pcm_is_playing() */
#include "usb.h"
#include "power.h"
#include "button.h"
#include "dac_ext.h"
#include "pcf50605.h"
#include "pcf5060x.h"
#include "stor_pwr.h"
#include "usbpark_core.h"
#include "pwr_awake.h"
#include "devbcm.h"
#include "devbench.h"
#include "devbench_awake.h"

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
    char line[160];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    devbcm_log(line);
}

/* A video's soundtrack in the audio playlist (#154): the rows that
   play want an album's songs (the session's "Play an album" step) */
static bool track_is_video(void)
{
    struct mp3entry *id3 = audio_current_track();
    const char *dot = id3 ? strrchr(id3->path, '.') : NULL;

    return dot && (!strcasecmp(dot, ".m4v") || !strcasecmp(dot, ".mov") ||
                   !strcasecmp(dot, ".mp4"));
}

static bool playing(void)
{
    int st = audio_status();

    return (st & AUDIO_STATUS_PLAY) && !(st & AUDIO_STATUS_PAUSE);
}

/* Sitting 4gh: the rows run inside one combined session, where the
   steps before them (the stage plays, the video) leave the music
   stopped, and the owner can't start it from inside a step.  So a row
   that needs music starts the current playlist itself, at its resume
   spot's track (as devbench_cop.c's duty row), or resumes a pause.
   true: it plays */
static bool start_music(void)
{
    long end;
    int index = 0;

    if (playing())
        return true;
    if (audio_status() & AUDIO_STATUS_PAUSE)
        audio_resume();
    else if (playlist_amount() > 0)
    {
        playlist_get_resume_info(&index);
        playlist_start(index < 0 ? 0 : index, 0, 0);
    }
    else
        return false;
    end = current_tick + 5 * HZ;
    while (TIME_BEFORE(current_tick, end))
    {
        if (playing() && pcm_is_playing())
            return true;
        sleep(HZ / 10);
    }
    return playing();
}

static void rec_line(const char *id, int row)
{
    const struct pwraw_rec *r = pwraw_rec(row);

    say("%s %s acts=%lu undos=%lu fails=%lu a=0x%08lx b=0x%08lx why=%d",
        id, pwraw_row_name(row), (unsigned long)r->acts,
        (unsigned long)r->undos, (unsigned long)r->fails,
        (unsigned long)r->a, (unsigned long)r->b,
        row < PWRAW_N_SWITCH ? (int)pwraw_why_now(row) : -1);
}

/* ---- awxm: AW-8 and AW-9, read-only ---------------------------------- */

int devbench_aw_xmb(void)
{
    rec_line("awxm", PWRAW_XMB);
    say("awxm xmb now=0x%08lx (before/after the last USB reset's OR: a, b; "
        "a change means the OR moved bits)", (unsigned long)XMB_RAM_CFG);
    rec_line("awxm", PWRAW_ADC);
    say("awxm adc: acts = done bit never seen, undos = reads that needed "
        "more than one poll");
    return BENCH_DONE;
}

/* ---- awd2 / awd2c: AW-3, T-D2REG ------------------------------------- */

#define D2_OFF      0x18        /* accessory_supply_set(false) */
#define D2_HOLD_S   5

int devbench_aw_d2reg(void)
{
    int v, w, back;

    if (!start_music())
    {
        say("awd2: Not run: no music to play");
        return BENCH_SKIP;
    }
    v = pcf50605_read(PCF5060X_D2REGC1);
    if (v < 0)
    {
        say("awd2: Not run: the PMU didn't answer");
        return BENCH_SKIP;
    }
    pcf50605_write(PCF5060X_D2REGC1, D2_OFF);
    w = pcf50605_read(PCF5060X_D2REGC1);
    say("awd2 d2regc1 found=0x%02x wrote=0x%02x read=0x%02x hp=%d usb=%d "
        "charger=%d, %d s", v & 0xff, D2_OFF, w & 0xff,
#ifdef HAVE_HEADPHONE_DETECTION
        (int)headphones_inserted(),
#else
        -1,
#endif
        (int)usb_inserted(), (int)charger_inserted(), D2_HOLD_S);
    sleep(D2_HOLD_S * HZ);
    pcf50605_write(PCF5060X_D2REGC1, (unsigned char)v);
    back = pcf50605_read(PCF5060X_D2REGC1);
    if (back != v)
    {
        pcf50605_write(PCF5060X_D2REGC1, (unsigned char)v);
        back = pcf50605_read(PCF5060X_D2REGC1);
    }
    say("awd2 restored=0x%02x playing=%d usb=%d charger=%d", back & 0xff,
        (int)playing(), (int)usb_inserted(), (int)charger_inserted());
    if (back != v)
    {
        say("awd2: FAIL: the supply's register is not back (0x%02x, "
            "found 0x%02x)", back & 0xff, v & 0xff);
        return BENCH_FAIL;
    }
    return playing() ? BENCH_PASS : BENCH_FAIL;
}

/* ---- awlg: AW-1, T-LINGER -------------------------------------------- */

#define LG_MAX_S    (20 * 60)
#define LG_WANT     2           /* linger cuts to see */
#define LG_FILLS    3           /* the buffer's worth of music queued */
#define LG_TRACKS   400         /* tracks counted or added, at most */

/* Sitting 4gh-b5: awlg saw no refill in 20 min (lingers=0): the album
   fitted the 64 MB buffer, so the one fill at the start was all.  The row
   counts the bytes queued from the playing spot on and, short of
   LG_FILLS buffers, adds the albums beside the playing one (its folder's
   parent, then that one's parent, never the root) to the playlist */
static bool aw_is_audio(const char *name)
{
    static const char *const ext[] =
        { ".mp3", ".flac", ".m4a", ".m4b", ".ogg", ".opus", ".wav",
          ".aif", ".aiff", ".ape", ".wv", ".mpc", ".wma", ".aac" };
    const char *dot = strrchr(name, '.');
    unsigned i;

    if (!dot || name[0] == '.')
        return false;
    for (i = 0; i < ARRAYLEN(ext); i++)
        if (!strcasecmp(dot, ext[i]))
            return true;
    return false;
}

static long aw_file_bytes(const char *path)
{
    int fd = open(path, O_RDONLY);
    long n;

    if (fd < 0)
        return 0;
    n = (long)lseek(fd, 0, SEEK_END);
    close(fd);
    return n < 0 ? 0 : n;
}

/* The bytes still to play: the rest of the playing track and every track
   after it in the playlist (LG_TRACKS at most) */
static uint64_t aw_queued(int *tracks)
{
    struct playlist_track_info ti;
    struct mp3entry *id3 = audio_current_track();
    int n = playlist_amount(), first = playlist_get_first_index(NULL);
    int cur = playlist_get_display_index() - 1, i;
    uint64_t q = 0;

    *tracks = 0;
    if (n <= 0 || cur < 0)
        return 0;
    for (i = cur; i < n && *tracks < LG_TRACKS; i++)
    {
        long b;

        if (playlist_get_track_info(NULL, (first + i) % n, &ti) < 0)
            continue;
        b = aw_file_bytes(ti.filename);
        if (i == cur && id3 && (long)id3->offset < b)
            b -= (long)id3->offset;
        q += (uint64_t)b;
        (*tracks)++;
    }
    return q;
}

/* Adds the audio files under path (depth levels down, skip: the playing
   album's folder) at the playlist's end until *q reaches want */
static int aw_add_dir(char *path, size_t len, size_t sz, int depth,
                      const char *skip, uint64_t *q, uint64_t want,
                      int *added)
{
    DIR *dir = opendir(path);
    struct dirent *e;

    if (!dir)
        return 0;
    while (*q < want && *added < LG_TRACKS && (e = readdir(dir)) != NULL)
    {
        struct dirinfo info = dir_get_info(dir, e);
        int n;

        if (e->d_name[0] == '.')
            continue;
        n = snprintf(path + len, sz - len, "%s%s",
                     len && path[len - 1] == '/' ? "" : "/", e->d_name);
        if (n < 0 || (size_t)n >= sz - len)
            continue;
        if (info.attribute & ATTR_DIRECTORY)
        {
            if (depth > 0 && strcasecmp(path, skip) &&
                strcasecmp(path, ROCKBOX_DIR))
                aw_add_dir(path, len + n, sz, depth - 1, skip, q, want,
                           added);
        }
        else if (aw_is_audio(e->d_name) &&
                 playlist_insert_track(NULL, path, PLAYLIST_INSERT_LAST,
                                       false, false) >= 0)
        {
            *q += info.size;
            (*added)++;
        }
        path[len] = '\0';
    }
    closedir(dir);
    return 0;
}

/* The source: LG_FILLS buffers' worth queued, or as near as the library
   gets; logged with what was checked */
static void aw_long_source(void)
{
    struct mp3entry *id3 = audio_current_track();
    uint64_t buf = audio_get_filebuflen(), want = buf * LG_FILLS, q;
    char path[MAX_PATH], album[MAX_PATH];
    int tracks, added = 0, up;
    unsigned kbps = id3 && id3->bitrate ? id3->bitrate : 0;

    q = aw_queued(&tracks);
    say("awlg source: queued=%lu KB in %d tracks, buffer=%lu KB, want "
        ">= %lu KB (%d buffers)", (unsigned long)(q >> 10), tracks,
        (unsigned long)(buf >> 10), (unsigned long)(want >> 10), LG_FILLS);
    if (q < want && id3 && id3->path[0] == '/')
    {
        strlcpy(album, id3->path, sizeof(album));
        *strrchr(album, '/') = '\0';            /* the album's folder */
        strlcpy(path, album, sizeof(path));
        for (up = 0; up < 2 && q < want; up++)
        {
            char *slash = strrchr(path, '/');

            if (!slash || slash == path)
                break;                          /* never the root */
            *slash = '\0';
            aw_add_dir(path, strlen(path), sizeof(path), 2, album, &q, want,
                       &added);
            strlcpy(album, path, sizeof(album));   /* done: skip it next */
        }
        if (added)
            playlist_sync(NULL);
        say("awlg source: added %d tracks from the albums beside it, "
            "queued=%lu KB", added, (unsigned long)(q >> 10));
    }
    if (kbps)
        say("awlg source: %u kbps: a buffer lasts about %lu s (%s within "
            "the row's %d s)", kbps,
            (unsigned long)(buf * 8 / 1000 / kbps),
            buf * 8 / 1000 / kbps * LG_WANT < LG_MAX_S ? "refills" :
            "too few refills", LG_MAX_S);
}

int devbench_aw_linger(void)
{
    const struct stor_pwr *s = storage_power_state();
    uint32_t l0 = s->lingers, r0 = s->rewakes, o0 = s->offs;
    long t0 = current_tick;
    bool ok;

    if (!start_music())
    {
        say("awlg: Not run: no music to play");
        return BENCH_SKIP;
    }
    if (track_is_video())
    {
        say("awlg: Not run: a video's sound is playing (play an album)");
        return BENCH_SKIP;
    }
    aw_long_source();
    t0 = current_tick;
    if (s->linger_off)
    {
        say("awlg: FAIL: the linger's fallback already tripped this "
            "session (rewakes=%lu)", (unsigned long)s->rewakes);
        return BENCH_FAIL;
    }
    pwraw_force(PWRAW_LINGER, 1);
    backlight_off();
    while (TIME_BEFORE(current_tick, t0 + LG_MAX_S * HZ) &&
           (s->lingers - l0 < LG_WANT ||
            TIME_BEFORE(current_tick, s->linger_cut_at + 30 * HZ)))
    {
        sleep(5 * HZ);
        if (!playing() || s->linger_off)
            break;
    }
    pwraw_force(PWRAW_LINGER, -1);
    backlight_on();
    rec_line("awlg", PWRAW_LINGER);
    say("awlg lingers=%lu rewakes=%lu offs=%lu backoff=%ld fallback=%d "
        "playing=%d s=%ld (a: ms from the fill's end to the cut)",
        (unsigned long)(s->lingers - l0), (unsigned long)(s->rewakes - r0),
        (unsigned long)(s->offs - o0), s->backoff, (int)s->linger_off,
        (int)playing(), (current_tick - t0) / HZ);
    ok = s->lingers - l0 >= LG_WANT && s->rewakes == r0 && !s->linger_off &&
         playing() && pwraw_rec(PWRAW_LINGER)->a <= 300;
    say("awlg: %s: the drive cut <= 0.3 s after each fill, no rewake "
        "within %d s", ok ? "PASS" : "FAIL", STOR_PWR_LINGER_REWAKE_S);
    return ok ? BENCH_PASS : BENCH_FAIL;
}

/* ---- awus: AW-2, T-USBPROBE ------------------------------------------ */

int devbench_aw_usb(void)
{
    const struct pwraw_rec *r = pwraw_rec(PWRAW_USBOFF);
    uint32_t a0;
    long end = current_tick + 60 * HZ;
    bool parked;

    while (!usb_inserted() && TIME_BEFORE(current_tick, end))
        sleep(HZ / 4);
    if (!usb_inserted())
    {
        say("awus: Not run: no charger plugged in within 60 s");
        return BENCH_SKIP;
    }
    a0 = r->acts;
    pwraw_force(PWRAW_USBOFF, 1);
    end = current_tick + (USBPARK_WINDOW_MS + 10000) * HZ / 1000;
    while (r->acts == a0 && usb_inserted() && TIME_BEFORE(current_tick, end))
        sleep(HZ / 4);
    parked = r->acts != a0;
    rec_line("awus", PWRAW_USBOFF);
    say("awus parked=%d charger=%d usb=%d (a: the charge current kept, mA) "
        "dev_on=%d", (int)parked, (int)charger_inserted(),
        (int)usb_inserted(), (int)!!(pwraw_dev_mask() &
                                     PWRAW_BIT(PWRAW_USBOFF)));
    /* back to the gate: the host plug after the session tests the daily
       path (a VBUS edge opens a fresh window either way) */
    pwraw_force(PWRAW_USBOFF, -1);
    if (!parked)
    {
        say("awus: FAIL: the controller was not parked within %d s on the "
            "charger (a host answered, or the storage was handed over)",
            USBPARK_WINDOW_MS / 1000 + 10);
        return BENCH_FAIL;
    }
    say("awus: parked on the charger; the host question follows");
    return BENCH_PASS;
}

/* ---- awld: AW-4, T-LCDLDO -------------------------------------------- */

#define LD_CYCLES   5
#define LD_DARK_S   12

int devbench_aw_lcdldo(void)
{
    const struct pwraw_rec *r = pwraw_rec(PWRAW_LCDLDO);
    uint32_t a0 = r->acts, u0 = r->undos, f0 = r->fails;
    unsigned long worst_us = 0;
    int k, lit = 0;
    bool ok;

    if (r->fails)
    {
        say("awld: FAIL: the row failed earlier this session");
        return BENCH_FAIL;
    }
    pwraw_force(PWRAW_LCDLDO, 1);
    for (k = 0; k < LD_CYCLES && !r->fails; k++)
    {
        unsigned long t;

        backlight_off();
        sleep(HZ / 2);
        if (lcd_active())
            lcd_sleep();
        sleep(LD_DARK_S * HZ);
        t = USEC_TIMER;
        lcd_awake();
        t = USEC_TIMER - t;
        backlight_on();
        if (t > worst_us)
            worst_us = t;
        if (lcd_active())
            lit++;
        say("awld cycle %d wake_us=%lu lit=%d d3regc1=0x%02x", k + 1, t,
            (int)lcd_active(), pcf50605_read(PCF5060X_D3REGC1) & 0xff);
        sleep(2 * HZ);              /* a look at the panel */
    }
    pwraw_force(PWRAW_LCDLDO, -1);
    rec_line("awld", PWRAW_LCDLDO);
    ok = r->fails == f0 && r->acts - a0 == (uint32_t)LD_CYCLES &&
         r->undos - u0 == (uint32_t)LD_CYCLES && lit == LD_CYCLES;
    say("awld: %s: lowered=%lu restored=%lu lit=%d/%d worst_wake_us=%lu",
        ok ? "PASS" : "FAIL", (unsigned long)(r->acts - a0),
        (unsigned long)(r->undos - u0), lit, LD_CYCLES, worst_us);
    return ok ? BENCH_PASS : BENCH_FAIL;
}

/* ---- awi2: AW-5, T-I2SGATE ------------------------------------------- */

#define I2_CYCLES   3           /* per variant */
#define I2_WAIT_S   12          /* DAC_IDLE_MS (5 s) plus the mixer's tail
                                   and a few seconds gated */

static bool dac_in_standby(void)
{
    struct dac_stats ds;

    dac_get_stats(&ds);
    return ds.standby;
}

int devbench_aw_i2s(void)
{
    static const unsigned long variant[2] =
        { DEV_EXTCLOCKS, DEV_EXTCLOCKS | DEV_I2S };
    const struct pwraw_rec *r = pwraw_rec(PWRAW_I2SGATE);
    unsigned long mask_was = wmcodec_gate_mask;
    uint32_t a0 = r->acts, f0 = r->fails;
    int v, k, gated = 0, resumed = 0, late = 0;
    bool ok;

    if (!start_music())
    {
        say("awi2: Not run: no music to play");
        return BENCH_SKIP;
    }
    if (!dac_standby_allowed())
    {
        say("awi2: Not run: the DAC standby switch is off");
        return BENCH_SKIP;
    }
    pwraw_force(PWRAW_I2SGATE, 1);
    for (v = 0; v < 2 && r->fails == f0; v++)
    {
        wmcodec_gate_mask = variant[v];
        for (k = 0; k < I2_CYCLES && r->fails == f0; k++)
        {
            long end;
            bool sb, g;

            audio_pause();
            sleep(I2_WAIT_S * HZ);
            sb = dac_in_standby();
            g = wmcodec_clocks_gated();
            gated += g;
            audio_resume();
            /* the watchdog: the PCM running within 50 ms of the DAC's
               wake (a few ticks of slack for the resume itself) */
            end = current_tick + HZ / 2;
            while (!pcm_is_playing() && TIME_BEFORE(current_tick, end))
                sleep(1);
            if (pcm_is_playing())
                resumed++;
            else
                late++;
            say("awi2 variant=%c cycle=%d standby=%d gated=%d pcm=%d "
                "iisconfig=0x%08lx dev_en=0x%08lx", v ? 'b' : 'a', k + 1,
                (int)sb, (int)g, (int)pcm_is_playing(),
                (unsigned long)IISCONFIG, (unsigned long)DEV_EN);
            sleep(3 * HZ);
        }
    }
    wmcodec_gate_mask = mask_was;
    pwraw_force(PWRAW_I2SGATE, -1);
    rec_line("awi2", PWRAW_I2SGATE);
    ok = r->fails == f0 && late == 0 && gated == 2 * I2_CYCLES &&
         r->acts - a0 == (uint32_t)(2 * I2_CYCLES) && playing();
    say("awi2: %s: gated=%d resumed=%d late=%d (pass: every pause gated, "
        "every resume running, the I2S set-up intact)", ok ? "PASS" : "FAIL",
        gated, resumed, late);
    return ok ? BENCH_PASS : BENCH_FAIL;
}

#elif defined(DEVBENCH_SIM)
/* the bench simulator: the rows are silicon questions, so the sim never
 * models a pass: "Not run (sim)".  Fault switches drive the runner's
 * paths: bench_sim set fail_<id> fails the row; bench_sim set hang_<id>
 * blocks it, as a frozen unit, until the script's "reboot" (the owner's
 * MENU+SELECT), so Resume's hang path is covered (sitting 4gh: awld
 * before jvpu) */
#include "file.h"
#include "kernel.h"
#include "devbench.h"
#include "devbench_awake.h"
#include "devbcm.h"
#include "devbench_sim_calls.h"

static int aw_sim_row(const char *id)
{
    char path[64], line[64];

    snprintf(path, sizeof(path), DBS_DIR "/hang_%s", id);
    if (file_exists(path))
    {
        snprintf(line, sizeof(line), "%s: hanging (fault switch, sim)", id);
        devbcm_log(line);
        for (;;)
            sleep(HZ);
    }
    snprintf(path, sizeof(path), DBS_FAIL "%s", id);
    if (file_exists(path))
    {
        snprintf(line, sizeof(line), "%s: FAIL (fault switch, sim)", id);
        devbcm_log(line);
        return BENCH_FAIL;
    }
    snprintf(line, sizeof(line), "%s: Not run (sim)", id);
    devbcm_log(line);
    return BENCH_SKIP;
}

int devbench_aw_xmb(void)    { return aw_sim_row("awxm"); }
int devbench_aw_d2reg(void)  { return aw_sim_row("awd2"); }
int devbench_aw_linger(void) { return aw_sim_row("awlg"); }
int devbench_aw_usb(void)    { return aw_sim_row("awus"); }
int devbench_aw_lcdldo(void) { return aw_sim_row("awld"); }
int devbench_aw_i2s(void)    { return aw_sim_row("awi2"); }
#else /* no awake rows: refused, never a pass */
#include "devbench.h"
#include "devbench_awake.h"

int devbench_aw_xmb(void)    { return BENCH_SKIP; }
int devbench_aw_d2reg(void)  { return BENCH_SKIP; }
int devbench_aw_linger(void) { return BENCH_SKIP; }
int devbench_aw_usb(void)    { return BENCH_SKIP; }
int devbench_aw_lcdldo(void) { return BENCH_SKIP; }
int devbench_aw_i2s(void)    { return BENCH_SKIP; }
#endif
