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
 * Phase 11 sim commands (docs/phase11-brief.md section 8); see
 * sim_cmd_p11.h. Links in both the stock and the --ipodui sim: shell
 * symbols are weak here.
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
#include <ctype.h>
#include <SDL.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "button.h"
#include "backlight.h"
#include "powermgmt.h"
#include "playlist.h"
#include "button-sdl.h"
#include "sim_db.h"
#include "ipodui/search.h"
#ifdef HAVE_IPOD_UI
#include "settings.h"
#include "ipodui/header.h"
#endif
#define OTG_FORMAT_ONLY
#include "ipodui/otg.h"
#include "sim_cmd_p11.h"

#define P11_DB_POLL_MS      100
#define P11_WHEEL_MS        60      /* slow: no wheel acceleration */
#define P11_KEY_MS          100
#define P11_SETTLE_MS       150

/* screens/search.c (U1); weak for the stock sim */
int ui_search_strip_pos(void) __attribute__((weak));
int ui_search_strip_pos(void)
{
    return -1;
}

/* -- usb_eject ---------------------------------------------------------- */

static volatile bool sim_ejected;

/* sysscreen.h: the target's is in firmware/usbstack/usb_storage.c */
bool usb_storage_all_ejected(void)
{
    return sim_ejected;
}

/* -- helpers ------------------------------------------------------------ */

static void p11_sleep_ms(long ms)
{
    if (ms > 0)
        sleep((ms * HZ + 999) / 1000);
}

static char *p11_unesc(char *p)
{
    char *r = p, *w = p;

    while (*r)
    {
        if (r[0] == '%' && r[1] == '2' && r[2] == '0')
        {
            *w++ = ' ';
            r += 3;
        }
        else
            *w++ = *r++;
    }
    *w = '\0';
    return p;
}

static void p11_key(int sdl_key)
{
    sim_inject_key(sdl_key, true);
    p11_sleep_ms(P11_KEY_MS);
    sim_inject_key(sdl_key, false);
    p11_sleep_ms(P11_SETTLE_MS);
}

/* n wheel clicks, posted as the clickwheel driver posts them (bit 31:
 * speed data, 15 degrees per click), at a fixed slow speed */
static void p11_wheel(int n)
{
    int btn = n < 0 ? BUTTON_SCROLL_BACK : BUTTON_SCROLL_FWD;
    unsigned est = 1000 / P11_WHEEL_MS;      /* clicks/s */

    for (n = abs(n); n > 0; n--)
    {
        backlight_on();
        reset_poweroff_timer();
        if (!button_queue_full())
            button_queue_post(btn, (1u << 31) | (1 << 24) | (est * 15));
        p11_sleep_ms(P11_WHEEL_MS);
    }
    p11_sleep_ms(P11_SETTLE_MS);
}

/* -- type --------------------------------------------------------------- */

static void type_chars(char *s, int lineno)
{
    p11_unesc(s);
    for (; *s; s++)
    {
        int c = tolower((unsigned char)*s), pos, want;
        const char *at;

        if (c == ' ')
        {
            p11_key(SDLK_RIGHT);            /* NEXT: a space */
            continue;
        }
        if (c == '<')
        {
            p11_key(SDLK_LEFT);             /* PREV: delete */
            continue;
        }
        at = strchr(SRCH_ALPHABET, c);
        pos = ui_search_strip_pos();
        if (!at || pos < 0)
        {
            fprintf(stderr, "sim_script:%d: expect_type failed at '%c' "
                    "(%s)\n", lineno, *s, at ? "no letter strip"
                                             : "not in the alphabet");
            return;
        }
        want = (int)(at - SRCH_ALPHABET);
        if (want != pos)
            p11_wheel(want - pos);
        if (ui_search_strip_pos() != want)
        {
            fprintf(stderr, "sim_script:%d: expect_type failed at '%c' "
                    "(box on %d, want %d)\n", lineno, *s,
                    ui_search_strip_pos(), want);
            return;
        }
        p11_key(SDLK_RETURN);               /* SELECT: enter it */
    }
    fprintf(stderr, "sim_script: type ok\n");
}

/* -- expect_queue ------------------------------------------------------- */

static void expect_queue(int n, char *first, int lineno)
{
    struct playlist_track_info info;
    int amount = playlist_amount();
    bool path_ok = true;
    const char *got = "-";

    p11_unesc(first);
    if (strcmp(first, "-"))
    {
        size_t lf = strlen(first), lg;

        path_ok = false;
        if (amount > 0
            && playlist_get_track_info(NULL,
                    playlist_get_first_index(NULL) % amount, &info) == 0)
        {
            got = info.filename;
            lg = strlen(got);
            path_ok = lg >= lf && !strcmp(got + lg - lf, first);
        }
    }
    if (amount == n && path_ok)
        fprintf(stderr, "sim_script: expect_queue ok n=%d first=%s\n",
                amount, got);
    else
        fprintf(stderr, "sim_script:%d: expect_queue failed n=%d (want %d) "
                "first=%s (want ...%s)\n", lineno, amount, n, got, first);
}

/* -- the On-The-Go files ------------------------------------------------ */

#define OTG_LIST_PATH OTG_DIR "/" OTG_LIST

/* size and a checksum of a file; -1 if it is missing */
static long file_sum(const char *path, unsigned long *sum, int *lines)
{
    char buf[1024];
    long size = 0;
    ssize_t r;
    int fd = open(path, O_RDONLY);

    *sum = 5381;
    if (lines)
        *lines = 0;
    if (fd < 0)
        return -1;
    while ((r = read(fd, buf, sizeof(buf))) > 0)
    {
        for (ssize_t i = 0; i < r; i++)
        {
            *sum = *sum * 33 + (unsigned char)buf[i];
            if (lines && buf[i] == '\n')
                (*lines)++;
        }
        size += r;
    }
    close(fd);
    return size;
}

static void otg_dump(void)
{
    char buf[1024], line[MAX_PATH];
    int fd, n = 0, len = 0;
    ssize_t r;
    struct otg_idx_hdr h;
    struct otg_rec rec;

    fd = open(OTG_LIST_PATH, O_RDONLY);
    if (fd < 0)
        fprintf(stderr, "sim_script: otg_dump list missing\n");
    else
    {
        while ((r = read(fd, buf, sizeof(buf))) > 0)
        {
            for (ssize_t i = 0; i < r; i++)
            {
                if (buf[i] == '\n')
                {
                    line[len] = '\0';
                    fprintf(stderr, "sim_script: otg_dump line %d %s\n",
                            n++, line);
                    len = 0;
                }
                else if (len < (int)sizeof(line) - 1)
                    line[len++] = buf[i];
            }
        }
        if (len > 0)
        {
            line[len] = '\0';
            fprintf(stderr, "sim_script: otg_dump torn %s\n", line);
        }
        close(fd);
        fprintf(stderr, "sim_script: otg_dump lines=%d\n", n);
    }
    fd = open(OTG_IDX, O_RDONLY);
    if (fd < 0)
    {
        fprintf(stderr, "sim_script: otg_dump idx missing\n");
        return;
    }
    if (read(fd, &h, sizeof(h)) == (ssize_t)sizeof(h))
    {
        fprintf(stderr, "sim_script: otg_dump idx magic=%08lx version=%lu "
                "count=%lu sum=%08lx commitid=%ld\n",
                (unsigned long)h.magic, (unsigned long)h.version,
                (unsigned long)h.count, (unsigned long)h.sum,
                (long)h.commitid);
        for (n = 0; read(fd, &rec, sizeof(rec)) == (ssize_t)sizeof(rec); n++)
            fprintf(stderr, "sim_script: otg_dump rec %d idx=%ld check=%08lx\n",
                    n, (long)rec.idx, (unsigned long)rec.check);
    }
    else
        fprintf(stderr, "sim_script: otg_dump idx short\n");
    close(fd);
}

static void db_rebuild_keep_otg(long timeout_ms, int lineno)
{
    unsigned long ls0, is0, ls1, is1;
    int lines0, lines1;
    long l0 = file_sum(OTG_LIST_PATH, &ls0, &lines0);
    long i0 = file_sum(OTG_IDX, &is0, NULL);
    int gen = sim_db_generation();
    long waited = 0;
    long l1, i1;

    sim_db_rebuild();
    while (waited < timeout_ms
           && (sim_db_state() != SIM_DB_READY || sim_db_generation() < 0
               || sim_db_generation() == gen))
    {
        p11_sleep_ms(P11_DB_POLL_MS);
        waited += P11_DB_POLL_MS;
    }
    if (waited >= timeout_ms)
    {
        fprintf(stderr, "sim_script:%d: rebuild_db timed out after %ld ms\n",
                lineno, waited);
        return;
    }
    l1 = file_sum(OTG_LIST_PATH, &ls1, &lines1);
    i1 = file_sum(OTG_IDX, &is1, NULL);
    if (l0 == l1 && ls0 == ls1 && i0 == i1 && is0 == is1)
        fprintf(stderr, "sim_script: db_rebuild_keep_otg ok lines=%d "
                "after %ld ms\n", lines1, waited);
    else
        fprintf(stderr, "sim_script:%d: expect_otg_kept failed list %ld->%ld "
                "B idx %ld->%ld B\n", lineno, l0, l1, i0, i1);
}

bool sim_cmd_p11(const char *cmd, char *argv[], int argc, int lineno)
{
    if (!strcmp(cmd, "usb_eject"))
    {
        sim_ejected = !(argc >= 2 && !strcmp(argv[1], "off"));
        fprintf(stderr, "sim_script: usb_eject %s\n",
                sim_ejected ? "on" : "off");
    }
    else if (!strcmp(cmd, "type") && argc >= 2)
        type_chars(argv[1], lineno);
    else if (!strcmp(cmd, "expect_queue") && argc >= 3)
        expect_queue(atoi(argv[1]), argv[2], lineno);
    else if (!strcmp(cmd, "time_in_title") && argc >= 2)
    {
        bool on = !strcmp(argv[1], "on");

        if (!on && strcmp(argv[1], "off"))
            fprintf(stderr, "sim_script:%d: time_in_title on|off\n", lineno);
        else
        {
#ifdef HAVE_IPOD_UI
            global_settings.ipodui_time_in_title = on;
            ui_header_set_time_in_title(on);
            fprintf(stderr, "sim_script: time_in_title %s\n",
                    on ? "on" : "off");
#else
            fprintf(stderr, "sim_script: time_in_title %s (no shell)\n",
                    on ? "on" : "off");
#endif
        }
    }
    else if (!strcmp(cmd, "otg_dump"))
        otg_dump();
    else if (!strcmp(cmd, "db_rebuild_keep_otg"))
        db_rebuild_keep_otg(argc >= 2 ? atol(argv[1]) : 30000, lineno);
    else
        return false;
    return true;
}
