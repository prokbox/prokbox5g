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
 * Phase 8a system-screen sim commands; see sim_cmd_sys.h.
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
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "button.h"
#include "backlight.h"
#include "lcd.h"
#include "settings.h"
#include "ata_idle_notify.h"
#include "splash.h"
#include "yesno.h"
#include "sim_cmd_sys.h"
#include "sim_capture.h"
#ifdef HAVE_IPOD_UI_DEVMODE
#include "ipodui/powerlog.h"
#endif
#ifdef HAVE_DEVSTATS
#include "devstats.h"
#endif
#include "events.h"     /* P19: sys_events_peak() */
#ifdef HAVE_PANIC_RECORD
#include "panic_record.h"
#endif
#if defined(HAVE_IPOD_UI) && !defined(BOOTLOADER)
#include "quiet_wait_core.h"   /* P19 19-1a: qw_legacy */
#include "codec_place_core.h"  /* P19 19-1c: cpl_full_legacy */
#include "codec_cop.h"
#include "playback.h"          /* P19 19-1c: audio_set_crossfade() */
#endif

int sim_usb_glyph_pin = -1;

#if defined(HAVE_IPOD_UI) && defined(HAVE_TC_RAMCACHE)
#include "sim_db.h"
/* apps/tagcache.c (SIMULATOR) */
void sim_tagcache_commit_hold(int point);
int sim_tagcache_commit_held(void);
void sim_tagcache_commit_cancel(void);
static int db_count_mark = -1;
#endif

#ifdef HAS_BUTTON_HOLD
extern bool hold_button_state;          /* button-sdl.c */
#endif

/* "%20" stands for a space in script words */
static void unesc(char *dst, const char *src, size_t n)
{
    size_t o = 0;

    while (*src && o + 1 < n)
    {
        if (src[0] == '%' && src[1] == '2' && src[2] == '0')
        {
            dst[o++] = ' ';
            src += 3;
        }
        else
            dst[o++] = *src++;
    }
    dst[o] = '\0';
}

#ifdef HAVE_HANG_RECORD
volatile long sim_hang_ui_ms;   /* ui_main.c's pass reads it */
#endif

bool sim_cmd_sys(const char *cmd, char *argv[], int argc, int lineno)
{
#ifdef HAVE_LCD_SLEEP
    extern bool sim_lcd_sleeping(void);
    /* P18 18-A review: "expect_lcd_sleep <min_ms> <max_ms>": from the
     * backlight going off (after a backlight_expire) the LCD must go to
     * sleep no sooner than min_ms and by max_ms (the spec: about 60 s after the backlight goes
     * off, the setting's value) */
    if (!strcmp(cmd, "expect_lcd_sleep") && argc >= 3)
    {
        long min_ms = atol(argv[1]), max_ms = atol(argv[2]), ms, t0, w0;

        w0 = current_tick;
        while (is_backlight_on(true) && current_tick - w0 < 30 * HZ)
            sleep(HZ / 50);                     /* off first */
        t0 = current_tick;
        while (!sim_lcd_sleeping() && (current_tick - t0) * 10 < max_ms)
            sleep(HZ / 50);
        ms = (current_tick - t0) * (1000 / HZ);
        if (!sim_lcd_sleeping())
            fprintf(stderr, "sim_script:%d: expect_lcd_sleep failed: awake "
                    "%ld ms after the backlight went off\n", lineno, ms);
        else if (ms < min_ms)
            fprintf(stderr, "sim_script:%d: expect_lcd_sleep failed: asleep "
                    "after %ld ms, want >= %ld\n", lineno, ms, min_ms);
        else
            fprintf(stderr, "sim_script: expect_lcd_sleep ok: %ld ms\n", ms);
        return true;
    }
#endif
#ifdef HAVE_DEVSTATS
    /* P18 18-A: "wakes <label>": the per-thread wake counts since the
     * previous "wakes", one "sim_script: wakes <label> <thread> <n>" line
     * each (stderr), so a script can bracket a state and read its rates */
    static unsigned wakes_last[DEVSTATS_SLOTS]; /* the last window's */
    /* P19 19-1a: "expect_wakes_le <thread> <n>": the previous "wakes"
     * window counted at most n wakes for that thread */
    if (!strcmp(cmd, "expect_wakes_le") && argc >= 3)
    {
        char name[16];
        unsigned i, n = 0, max = (unsigned)atol(argv[2]);
        for (i = 0; i < DEVSTATS_SLOTS; i++)
        {
            devstats_slot_name(i, name, sizeof(name));
            if (!strcmp(name, argv[1]))
                n += wakes_last[i];
        }
        if (n > max)
            fprintf(stderr, "sim_script:%d: expect_wakes_le failed: %s "
                    "woke %u times, want <= %u\n", lineno, argv[1], n, max);
        else
            fprintf(stderr, "sim_script: expect_wakes_le ok: %s %u <= %u\n",
                    argv[1], n, max);
        return true;
    }
    if (!strcmp(cmd, "wakes"))
    {
        static unsigned base[DEVSTATS_SLOTS];
        static long at;
        char name[16];
        long now = current_tick;
        unsigned i;

        fprintf(stderr, "sim_script: wakes %s secs %ld\n",
                argc >= 2 ? argv[1] : "-", at ? (now - at) / HZ : 0L);
        for (i = 0; i < DEVSTATS_SLOTS; i++)
        {
            unsigned n = devstats.t[i].thr_wakes;
            wakes_last[i] = at ? n - base[i] : 0;
            if (at && n != base[i])
            {
                devstats_slot_name(i, name, sizeof(name));
                fprintf(stderr, "sim_script: wakes %s %s %u\n",
                        argc >= 2 ? argv[1] : "-", name, n - base[i]);
            }
            base[i] = n;
        }
        {
            /* P19 19-1a: the pollers' waits and how many timed out */
            static const char *const pn[4] = {
                "audio", "storage", "tagcache", "power" };
            static unsigned pw[4], pt[4];
            for (i = 0; i < 4; i++)
            {
                if (at && devstats.poll_waits[i] != pw[i])
                    fprintf(stderr, "sim_script: polls %s %s waits=%u "
                            "tmo=%u\n", argc >= 2 ? argv[1] : "-", pn[i],
                            (unsigned)(devstats.poll_waits[i] - pw[i]),
                            (unsigned)(devstats.poll_tmo[i] - pt[i]));
                pw[i] = devstats.poll_waits[i];
                pt[i] = devstats.poll_tmo[i];
            }
#if defined(HAVE_IPOD_UI) && !defined(BOOTLOADER)
            fprintf(stderr, "sim_script: events %s peak=%u max=%u\n",
                    argc >= 2 ? argv[1] : "-", sys_events_peak(),
                    sys_events_max());
#endif
        }
        at = now;
        return true;
    }
#endif
#if defined(HAVE_IPOD_UI) && !defined(BOOTLOADER)
    /* P19 19-1a: "quietpoll legacy|on": the pollers' old fixed waits for
     * an A/B (qw_legacy), or the deadline waits (the default) */
    if (!strcmp(cmd, "quietpoll") && argc >= 2)
    {
        qw_legacy = !strcmp(argv[1], "legacy");
        fprintf(stderr, "sim_script: quietpoll %s\n",
                qw_legacy ? "legacy" : "on");
        return true;
    }
    /* P19 19-1c: "fullwait legacy|on": the codec's old HZ/20 poll on a
     * full PCM buffer (cpl_full_legacy), or the low-watermark sleep */
    if (!strcmp(cmd, "fullwait") && argc >= 2)
    {
        cpl_full_legacy = !strcmp(argv[1], "legacy");
        fprintf(stderr, "sim_script: fullwait %s\n",
                cpl_full_legacy ? "legacy" : "on");
        return true;
    }
#ifdef HAVE_CROSSFADE
    /* P19 19-1c: "crossfade off|always": Rockbox's crossfade (hidden in
     * the shell), for the full-buffer sleep under crossfade's watermark */
    if (!strcmp(cmd, "crossfade") && argc >= 2)
    {
        int c = !strcmp(argv[1], "always") ? CROSSFADE_ENABLE_ALWAYS
                                           : CROSSFADE_ENABLE_OFF;
        global_settings.crossfade = c;
        audio_set_crossfade(c);
        fprintf(stderr, "sim_script: crossfade %d\n", c);
        return true;
    }
#endif
#ifdef HAVE_COP_CODEC
    /* P19 19-1c: "codecplace screen|cop|cpu": the decode's core rule
     * (the simulator has one core: the choice is logged, not taken) */
    if (!strcmp(cmd, "codecplace") && argc >= 2)
    {
        int m = !strcmp(argv[1], "cop") ? CPL_MODE_COP
              : !strcmp(argv[1], "cpu") ? CPL_MODE_CPU : CPL_MODE_SCREEN;
        codec_cop_set_place(m);
        fprintf(stderr, "sim_script: codecplace %d\n", m);
        return true;
    }
#endif
#endif
    if (!strcmp(cmd, "hold_switch") && argc >= 2)
    {
        bool on = !strcmp(argv[1], "on");

        if (!on && strcmp(argv[1], "off"))
        {
            fprintf(stderr, "sim_script:%d: hold_switch on|off\n", lineno);
            return true;
        }
#ifdef HAS_BUTTON_HOLD
        hold_button_state = on;
        sleep(3);       /* button_read_device() sees the change on a tick */
#endif
        fprintf(stderr, "sim_script: hold_switch %s\n", argv[1]);
        return true;
    }
    if (!strcmp(cmd, "backlight_expire"))
    {
        /* The sim default timeout is 0 ("Always On"), and with it
         * backlight.c posts no SYS_IPODUI_BACKLIGHT_OFF (brief Rev 1 M2),
         * so a real expiry needs a positive timeout first: 15 s, the stock
         * 7G default, for battery and (when on power) plugged alike. */
        if (global_settings.backlight_timeout <= 0)
        {
            global_settings.backlight_timeout = 15;
            backlight_set_timeout(15);
        }
#if CONFIG_CHARGING
        if (global_settings.backlight_timeout_plugged <= 0)
        {
            global_settings.backlight_timeout_plugged = 15;
            backlight_set_timeout_plugged(15);
        }
#endif
        backlight_off();
        fprintf(stderr, "sim_script: backlight_expire (timeout %d s)\n",
                backlight_get_current_timeout() / HZ);
        return true;
    }
    if (!strcmp(cmd, "usb_glyph") && argc >= 2)
    {
        if (!strcmp(argv[1], "usbc"))
            sim_usb_glyph_pin = 1;
        else if (!strcmp(argv[1], "dock30"))
            sim_usb_glyph_pin = 0;
        else if (!strcmp(argv[1], "auto"))
            sim_usb_glyph_pin = -1;
        else
        {
            fprintf(stderr, "sim_script:%d: usb_glyph usbc|dock30|auto\n",
                    lineno);
            return true;
        }
        fprintf(stderr, "sim_script: usb_glyph %s\n", argv[1]);
        return true;
    }
    if (!strcmp(cmd, "splash_probe") && argc >= 2)
    {
        char text[96];

        unesc(text, argv[1], sizeof(text));
        splashf(0, "%s", text);
        fprintf(stderr, "sim_script: splash_probe %s\n", argv[1]);
        return true;
    }
    if (!strcmp(cmd, "yesno_probe") && argc >= 2)
    {
#ifdef HAVE_IPOD_UI
        /* a stock yes/no prompt from the script thread: in the shell the
         * funnel logs "yesno-suppressed <text>" and answers No (D2) */
        char text[96];
        const char *lines[1] = { text };
        const struct text_message msg = { lines, 1 };
        enum yesno_res r;

        unesc(text, argv[1], sizeof(text));
        r = gui_syncyesno_run(&msg, NULL, NULL);
        fprintf(stderr, "sim_script: yesno_probe %s -> %s\n", argv[1],
                r == YESNO_YES ? "yes" : r == YESNO_NO ? "no" : "other");
#else
        fprintf(stderr, "sim_script:%d: yesno_probe needs a shell build\n",
                lineno);    /* a stock prompt would block the script */
#endif
        return true;
    }
    if (!strcmp(cmd, "capture") && argc >= 2)
    {
        /* what show_logo_boot() calls at the hand-off; no-op unless the
         * script has "# boot_shot" (IPODUI_SHOT_DIR) */
        sim_capture_screen(argv[1]);
        fprintf(stderr, "sim_script: capture %s\n", argv[1]);
        return true;
    }
    if (!strcmp(cmd, "storage_idle"))
    {
        call_storage_idle_notifys(true);
        fprintf(stderr, "sim_script: storage_idle\n");
        return true;
    }
#if defined(HAVE_IPOD_UI) && defined(HAVE_TC_RAMCACHE)
    if (!strcmp(cmd, "commit_hold") && argc >= 2)
    {
        sim_tagcache_commit_hold(atoi(argv[1]));
        fprintf(stderr, "sim_script: commit_hold %s\n", argv[1]);
        return true;
    }
    if (!strcmp(cmd, "commit_hold_wait"))
    {
        long ms = argc >= 2 ? atol(argv[1]) : 30000, t;

        for (t = 0; t < ms && sim_tagcache_commit_held() < 0; t += 20)
            sleep(HZ / 50);
        if (sim_tagcache_commit_held() >= 0)
            fprintf(stderr, "sim_script: commit_hold_wait ok at %d after "
                    "%ld ms\n", sim_tagcache_commit_held(), t);
        else
            fprintf(stderr, "sim_script:%d: expect_commit_hold failed\n",
                    lineno);
        return true;
    }
    if (!strcmp(cmd, "commit_release"))
    {
        sim_tagcache_commit_hold(-1);
        fprintf(stderr, "sim_script: commit_release\n");
        return true;
    }
    if (!strcmp(cmd, "commit_cancel"))
    {
        sim_tagcache_commit_cancel();
        fprintf(stderr, "sim_script: commit_cancel\n");
        return true;
    }
    if (!strcmp(cmd, "db_count_mark"))
    {
        db_count_mark = sim_db_entries();
        fprintf(stderr, "sim_script: db_count_mark %d\n", db_count_mark);
        return true;
    }
    if (!strcmp(cmd, "expect_db_count") && argc >= 2)
    {
        int want = db_count_mark + atoi(argv[1]), n = -1;
        long ms = argc >= 3 ? atol(argv[2]) : 60000, t;

        for (t = 0; t < ms; t += 50)
        {
            if (sim_db_state() == SIM_DB_READY
                && (n = sim_db_entries()) == want)
                break;
            sleep(HZ / 20);
        }
        if (t < ms)
            fprintf(stderr, "sim_script: expect_db_count ok %d after %ld "
                    "ms\n", n, t);
        else
            fprintf(stderr, "sim_script:%d: expect_db_count failed: %d, "
                    "want %d\n", lineno, n, want);
        return true;
    }
#endif
    if (!strcmp(cmd, "fs_chaincut") && argc >= 3)
    {   /* fs_chaincut <path> <bytes>: the file's cluster chain cut after
         * <bytes>, its size kept (filesystem-sim.c; a write cut by a
         * power loss, sitting 4c's panels.pak); fs_chaincut <path> check:
         * logs "fs_chaincut <path> whole" or "... cut" */
        extern int sim_fs_chaincut(const char *path, long bytes);
        bool check = !strcmp(argv[2], "check");
        int size = sim_fs_chaincut(argv[1], check ? -1 : atol(argv[2]));

        if (check)
            fprintf(stderr, "sim_script: fs_chaincut %s %s\n", argv[1],
                    size < 0 ? "missing" : size ? "cut" : "whole");
        else if (size < 0)
            fprintf(stderr, "sim_script:%d: fs_chaincut %s failed\n",
                    lineno, argv[1]);
        else
            fprintf(stderr, "sim_script: fs_chaincut %s at %ld of %d\n",
                    argv[1], atol(argv[2]), size);
        return true;
    }
    if (!strcmp(cmd, "hang_ui") && argc >= 2)
    {   /* the shell's thread stuck at work for MS at its next pass (any
         * input wakes it: a "wheel" line after this one); the hang
         * watcher records it (hang_watch.h) and, once it goes on, the
         * shell logs the record as recovered */
#ifdef HAVE_HANG_RECORD
        sim_hang_ui_ms = atol(argv[1]);
        fprintf(stderr, "sim_script: hang_ui %ld\n", sim_hang_ui_ms);
#else
        fprintf(stderr, "sim_script:%d: hang_ui needs HAVE_HANG_RECORD\n",
                lineno);
#endif
        return true;
    }
    if (!strcmp(cmd, "panic_fake"))
    {   /* what panicf() records, without the halt; a "reboot" line after
         * it shows the next boot logging it (panic_record.h) */
#ifdef HAVE_PANIC_RECORD
        char msg[PANIC_REC_MSG];
        unesc(msg, argc >= 2 ? argv[1] : "sim%20fake%20panic", sizeof(msg));
        panic_record_save(0, 0x00012340, 0, 0, msg);
        fprintf(stderr, "sim_script: panic_fake %s\n", msg);
#else
        fprintf(stderr, "sim_script:%d: panic_fake needs HAVE_PANIC_RECORD\n",
                lineno);
#endif
        return true;
    }
    if (!strcmp(cmd, "powerlog_max") && argc >= 2)
    {
#ifdef HAVE_IPOD_UI_DEVMODE
        powerlog_max = atol(argv[1]);
        fprintf(stderr, "sim_script: powerlog_max %ld\n", powerlog_max);
#endif
        return true;
    }
    if (!strcmp(cmd, "powerlog_mark") && argc >= 2)
    {
#ifdef HAVE_IPOD_UI_DEVMODE
        powerlog_mark(argv[1]);
        fprintf(stderr, "sim_script: powerlog_mark %s on=%d\n", argv[1],
                (int)powerlog_is_on());
#else
        fprintf(stderr, "sim_script:%d: powerlog_mark needs a developer "
                "build\n", lineno);
#endif
        return true;
    }
    return false;
}
