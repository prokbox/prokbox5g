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

/* iPod UI shell: the Developer menu's Status screen, the bench verdicts
 * on its Experiments rows, the speed governor's switch and the bench
 * session's own switches (devstatus.h; docs/design/devmode.md section 3).
 *
 * Nothing here runs at browse time: the results file is read when Status
 * or Experiments opens, and the switch list only inside a bench session
 * (and at boot, when a list is left over).  Every user-visible string is
 * a LANG_ phrase (feature ipodui_devmode); the hidden folder is never
 * named. */
#include "config.h"

#ifdef HAVE_IPOD_UI_DEVMODE
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "dir.h"
#include "misc.h"                   /* read_line */
#include "string-extra.h"
#include "version.h"
#include "lang.h"
#include "settings.h"               /* str() */
#include "gui/list.h"
#include "dac_ext.h"
#include "ipodui/devstatus.h"
#include "ipodui/devbench.h"        /* the results file's pure parser */
#include "ipodui/powerlog.h"
#include "ipodui/sleep.h"
#include "ipodui/uilog.h"
#include "ipodui/fe_be.h"
#ifdef HAVE_PLL_IDLE
#include "ipodui/clockpol.h"
#endif
#ifdef HAVE_ATA_UDMA_FORCE
#include "ipodui/udmatest.h"
#endif
#ifdef HAVE_STORAGE_POWEROFF
#include "stor_pwr.h"
#endif
#if defined(HAVE_USB_STATS) && !defined(SIMULATOR)
#include "usb_storage_stats.h"
#include "usb_drv.h"
#endif
#ifdef HAVE_BCMFW_RESIDENT
#include "bcm-devtest.h"
#endif
#ifdef HAVE_BCMFW_VIDEO
#include "video/bcmvideo_image.h"   /* BCMVIDEO_IMAGE_FNV */
#include "video/bcmvideo_clock.h"   /* the clock150 tokens */
#include "video/vclock_gate.h"
#endif
#if defined(HAVE_BCMFW_VIDEO) && !defined(SIMULATOR)
#include "ipodui/devbcm.h"          /* devbench_ask */
#endif

#define DS_BENCH_DIR    ROCKBOX_DIR "/bench"

/* ---- The newest results file of this build -------------------------- */

/* The Experiments rows' bench steps (devbench.c's ids) */
static const char *const ds_ids[] = { "vgov", "ucop2", "slp", "lvl5",
                                      "lit24ui" };
#define DS_NIDS ((int)ARRAYLEN(ds_ids))

static struct
{
    bool found, started, ended;
    char date[12];
    unsigned passed, failed, skipped;
    uint8_t res[ARRAYLEN(ds_ids)];  /* enum bench_res */
} ds;

/* As devbench.c's find_latest(): the newest "<date>-<build>.txt" */
static bool ds_find_latest(char *path, size_t n)
{
    char bp[40], tail[48], best[96] = "";   /* <date>-<build>.txt */
    DIR *d = opendir(DS_BENCH_DIR);
    struct dirent *e;
    size_t tl;

    if (!d)
        return false;
    bench_name_part(bp, sizeof(bp), rbversion);
    snprintf(tail, sizeof(tail), "-%s.txt", bp);
    tl = strlen(tail);
    while ((e = readdir(d)))
    {
        size_t l = strlen((char *)e->d_name);

        if (l > tl && l < sizeof(best) &&
            !strcmp((char *)e->d_name + l - tl, tail) &&
            strcmp((char *)e->d_name, best) > 0)
            strlcpy(best, (char *)e->d_name, sizeof(best));
    }
    closedir(d);
    if (!best[0])
        return false;
    snprintf(path, n, DS_BENCH_DIR "/%s", best);
    return true;
}

void devstatus_bench_load(void)
{
    char path[MAX_PATH], line[BENCH_LINE_MAX + 64], v[BENCH_TOKEN_MAX];
    struct bench_rec r;
    int fd, k;

    memset(&ds, 0, sizeof(ds));
    if (!ds_find_latest(path, sizeof(path)))
        return;
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return;
    ds.found = true;
    while (read_line(fd, line, sizeof(line)) > 0)
    {
        switch (bench_parse(line, &r))
        {
        case BENCH_K_SESSION:
            ds.started = true;
            if (!bench_field(line, "date", ds.date, sizeof(ds.date)))
                ds.date[0] = '\0';
            break;
        case BENCH_K_END:
            ds.ended = true;
            if (bench_field(line, "passed", v, sizeof(v)))
                ds.passed = strtoul_bench(v);
            if (bench_field(line, "failed", v, sizeof(v)))
                ds.failed = strtoul_bench(v);
            if (bench_field(line, "skipped", v, sizeof(v)))
                ds.skipped = strtoul_bench(v);
            break;
        case BENCH_K_RESULT:
            if (!ds.ended)
            {
                if (r.res == BENCH_PASS || r.res == BENCH_DONE)
                    ds.passed++;
                else if (r.res == BENCH_FAIL || r.res == BENCH_HANG)
                    ds.failed++;
                else if (r.res == BENCH_SKIP)
                    ds.skipped++;
            }
            for (k = 0; k < DS_NIDS; k++)
                if (!strcmp(r.id, ds_ids[k]))
                    ds.res[k] = (uint8_t)r.res;
            break;
        default:
            break;
        }
    }
    close(fd);
}

const char *devstatus_verdict(const char *step_id)
{
    int k;

    for (k = 0; k < DS_NIDS; k++)
    {
        if (strcmp(step_id, ds_ids[k]))
            continue;
        switch (ds.res[k])
        {
        case BENCH_PASS:
        case BENCH_DONE:
            return str(LANG_IPODUI_DEV_V_PASS);
        case BENCH_FAIL:
        case BENCH_HANG:
            return str(LANG_IPODUI_DEV_V_FAIL);
        case BENCH_SKIP:
            return str(LANG_IPODUI_DEV_V_SKIP);
        default:
            return NULL;
        }
    }
    return NULL;
}

char *devstatus_with_verdict(const char *text, const char *step_id,
                             char *buf, size_t len)
{
    const char *v = devstatus_verdict(step_id);

    if (!v)
        snprintf(buf, len, "%s", text);
    else
        snprintf(buf, len, "%s  [%s]", text, v);
    return buf;
}

/* ---- The speed governor's switch -------------------------------------- */

#if defined(HAVE_PP_GOV) && defined(HAVE_PLL_IDLE)
static int8_t gov_now = -1;         /* the markers, read once */

static void ds_marker(const char *path, bool want)
{
    if (!want)
        remove(path);
    else if (!file_exists(path))
    {
        int fd = creat(path, 0666);
        if (fd >= 0)
            close(fd);
    }
}

enum devgov devgov_get(void)
{
    if (gov_now < 0)
    {
        gov_now = DEVGOV_OFF;
        if (file_exists(CLOCKPOL_PGOV_ON))
        {
            gov_now = DEVGOV_STAGE1;
#ifdef HAVE_PP_GOV_DVS
            if (file_exists(CLOCKPOL_PGOV2_ON))
                gov_now = file_exists(CLOCKPOL_PGOV2_APPLY) ? DEVGOV_APPLY
                                                            : DEVGOV_LOG;
#endif
        }
    }
    return (enum devgov)gov_now;
}

void devgov_set(enum devgov g)
{
#ifndef HAVE_PP_GOV_DVS
    if (g > DEVGOV_STAGE1)
        g = DEVGOV_OFF;
#endif
    ds_marker(CLOCKPOL_PGOV_ON, g != DEVGOV_OFF);
#ifdef HAVE_PP_GOV_DVS
    ds_marker(CLOCKPOL_PGOV2_ON, g >= DEVGOV_LOG);
    ds_marker(CLOCKPOL_PGOV2_APPLY, g == DEVGOV_APPLY);
#endif
    gov_now = g;
    ui_log("gov", "dev next=%d", (int)g);
}

bool devgov_refused(void)
{
#ifdef HAVE_UI_THREAD_SPLIT
    return devgov_get() == DEVGOV_APPLY &&
           (fe_be_mode() == FE_COP || fe_be_mode_next() == FE_COP);
#else
    return false;
#endif
}

const char *devgov_name(enum devgov g)
{
    static const int ids[DEVGOV_N] = {
        LANG_IPODUI_DEV_GOV_OFF, LANG_IPODUI_DEV_GOV_STAGE1,
        LANG_IPODUI_DEV_GOV_LOG, LANG_IPODUI_DEV_GOV_APPLY };
    return str(ids[g < DEVGOV_N ? g : 0]);
}
#endif

/* ---- The bench session's switches ------------------------------------- */

#ifdef HAVE_BCMFW_DEVTEST
#define DSW_LIST    DS_BENCH_DIR "/switches.txt"
/* bcmjob-dev.c's BCMJOB_DEV_MARKER: the job gate for an image that
 * verified.txt doesn't list yet (read at boot) */
#define DSW_JOBS    ROCKBOX_DIR "/bcmjobs.on"
static const char *const dsw_paths[] = { DSW_JOBS };
static bool dsw_jobs_at_boot;       /* the marker was there at this boot */
static bool ds_jobs, ds_held;       /* Status: read once on entry */

/* Is path on the list?  (the list is a few short lines) */
static bool dsw_listed(const char *path)
{
    char line[MAX_PATH];
    int fd = open(DSW_LIST, O_RDONLY);
    bool hit = false;

    if (fd < 0)
        return false;
    while (!hit && read_line(fd, line, sizeof(line)) > 0)
        hit = !strcmp(line, path);
    close(fd);
    return hit;
}

/* Undo what the list says (only paths this file knows), then drop it */
static void dsw_undo(const char *why)
{
    char line[MAX_PATH];
    int fd = open(DSW_LIST, O_RDONLY);
    size_t k;

    if (fd >= 0)
    {
        while (read_line(fd, line, sizeof(line)) > 0)
            for (k = 0; k < ARRAYLEN(dsw_paths); k++)
                if (!strcmp(line, dsw_paths[k]))
                {
                    remove(dsw_paths[k]);
                    ui_log("bench", "switch %s off (%s)",
                           strrchr(dsw_paths[k], '/') + 1, why);
                }
        close(fd);
    }
    remove(DSW_LIST);
}

/* The newest results file of this build is open: a session line, no end
 * line (devbench.c resumes only this build's files) */
static bool dsw_session_open(void)
{
    char path[MAX_PATH], line[BENCH_LINE_MAX + 64];
    bool started = false, ended = false;
    int fd;

    if (!ds_find_latest(path, sizeof(path)))
        return false;
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    while (read_line(fd, line, sizeof(line)) > 0)
    {
        if (!strncmp(line, "session ", 8))
            started = true;
        else if (!strncmp(line, "end ", 4) || !strcmp(line, "end"))
            ended = true;
    }
    close(fd);
    return started && !ended;
}

void devswitch_boot(void)
{
    dsw_jobs_at_boot = file_exists(DSW_JOBS);
    if (file_exists(DSW_LIST) && !dsw_session_open())
        dsw_undo("no open session");
}

bool devswitch_held(void)
{
    return file_exists(DSW_LIST);
}

#if defined(HAVE_BCMFW_VIDEO) && !defined(SIMULATOR)
bool devswitch_bench_begin(int mode)
{
    size_t k;
    bool restart = false;

    mkdir(DS_BENCH_DIR);
    for (k = 0; k < ARRAYLEN(dsw_paths); k++)
    {
        const char *p = dsw_paths[k];

        if (!file_exists(p))
        {
            int fd = creat(p, 0666);

            if (fd < 0)
                continue;
            close(fd);
            ui_log("bench", "switch %s on", strrchr(p, '/') + 1);
        }
        else if (!dsw_listed(p))
            continue;               /* the owner's own: never undone */
        if (!dsw_listed(p))
        {
            int fd = open(DSW_LIST, O_WRONLY | O_CREAT | O_APPEND, 0666);

            if (fd >= 0)
            {
                fdprintf(fd, "%s\n", p);
                close(fd);
            }
        }
    }
#ifdef HAVE_BCMFW_RESIDENT
    /* the job gate is read at boot.  Only the re-verify run needs it
     * before a restart of its own: its jobs step runs first when the
     * stage flags are there (else s05 and the boot step restart anyway);
     * the full session's jobs2 runs after its boot step's restart */
    if (!dsw_jobs_at_boot && file_exists(DSW_JOBS) &&
        mode == BENCH_MODE_REVERIFY && bcm_boot_resident_active() &&
        !bcm_boot_resident_verified())
    {
        const char *why = "";

        restart = bcm_devtest_5i_allowed(&why) &&
                  bcm_devtest_4b_allowed(&why);
    }
#else
    (void)mode;
#endif
    if (restart)
    {
        const char *opts[1] = { str(LANG_IPODUI_DEV_OK) };

        ui_log("bench", "switch: restart first");
        devbench_ask(str(LANG_IPODUI_DEV_BENCH),
                     str(LANG_IPODUI_DEV_BENCH_RESTART), opts, 1);
    }
    return restart;
}

void devswitch_bench_end(void)
{
    if (file_exists(DSW_LIST))
        dsw_undo("session end");
}
#endif /* HAVE_BCMFW_VIDEO && !SIMULATOR */
#endif /* HAVE_BCMFW_DEVTEST */

/* ---- Status -----------------------------------------------------------
 * One read-only list; each row is made when the list draws it (no line
 * store), from this build's row kinds.  A row away from its default ends
 * in "(changed)"; the first row counts them. */

enum ds_row
{
    DS_SUMMARY = 0, DS_BUILD,
#ifdef HAVE_BCMFW_RESIDENT
    DS_VIDEO, DS_VERIFIED,
#ifdef HAVE_BCMFW_VIDEO
    DS_TOKEN,
#endif
    DS_VCSLEEP, DS_LVL5,
#endif
#ifdef HAVE_BCMFW_DEVTEST
    DS_JOBS,
#endif
#if defined(HAVE_PP_GOV) && defined(HAVE_PLL_IDLE)
    DS_GOV,
#endif
#ifdef HAVE_UI_THREAD_SPLIT
    DS_UI,
#endif
    DS_SOFT,
#ifdef HAVE_STORAGE_POWEROFF
    DS_DRIVE,
#endif
    DS_DAC, DS_PLOG,
#ifdef HAVE_PLL_IDLE
    DS_LIT, DS_IDLE24,
#endif
#ifdef HAVE_ATA_UDMA_FORCE
    DS_UDMA,
#endif
#if defined(HAVE_USB_STATS) && !defined(SIMULATOR)
    DS_USBPF, DS_USBINQ,
#if defined(IPOD_VIDEO)
    DS_USBATT,
#endif
#endif
    DS_BENCH, DS_BENCH_N,
#ifdef HAVE_BCMFW_DEVTEST
    DS_HELD,
#endif
    DS_NROWS
};

static int ds_changed;

static const char *onoff(bool on)
{
    return str(on ? LANG_IPODUI_DEV_ON : LANG_IPODUI_DEV_OFF);
}

#ifdef HAVE_BCMFW_RESIDENT
static const char *yesno(bool yes)
{
    return str(yes ? LANG_IPODUI_DEV_YES : LANG_IPODUI_DEV_NO);
}
#endif

/* "<format with val>", plus "(changed)" when off its default */
static const char *ds_fmt(char *buf, size_t len, bool changed, int fmt_id,
                          const char *val, bool *chg)
{
    size_t n;

    snprintf(buf, len, str(fmt_id), val);
    if (changed)
    {
        n = strlen(buf);
        snprintf(buf + n, len - n, " %s", str(LANG_IPODUI_DEV_ST_CHANGED));
    }
    *chg = changed;
    return buf;
}

/* Row k into buf; *chg: it is off its default */
static const char *ds_row_text(int k, char *buf, size_t len, bool *chg)
{
    char v[48];

    *chg = false;
    switch (k)
    {
    case DS_SUMMARY:
        snprintf(buf, len, str(LANG_IPODUI_DEV_ST_SUMMARY), ds_changed);
        return buf;
    case DS_BUILD:
        snprintf(buf, len, str(LANG_IPODUI_DEV_ST_BUILD), IPOD_VARIANT);
        return buf;
#ifdef HAVE_BCMFW_RESIDENT
    case DS_VIDEO:
    {
        bool idle, res = bcm_boot_resident_setting(&idle);

        return ds_fmt(buf, len, !res, LANG_IPODUI_DEV_ST_VIDEO,
                      str(!res ? LANG_IPODUI_DEV_RES_OFF
                          : bcm_boot_resident_active() ? LANG_IPODUI_DEV_RES_RUN
                          : LANG_IPODUI_DEV_RES_SET), chg);
    }
    case DS_VERIFIED:
        return ds_fmt(buf, len, false, LANG_IPODUI_DEV_ST_VERIFIED,
                      yesno(bcm_boot_resident_verified()), chg);
#ifdef HAVE_BCMFW_VIDEO
    case DS_TOKEN:
        return ds_fmt(buf, len, false, LANG_IPODUI_DEV_ST_TOKEN,
                      yesno(vclock_gate(BCMVIDEO_IMAGE_FNV,
                                        bcmvideo_clock150_fnv,
                                        BCMVIDEO_N_CLOCK150, false, false)
                            == VCLOCK_TOKEN), chg);
#endif
    case DS_VCSLEEP:
    {
        bool idle;

        bcm_boot_resident_setting(&idle);
        return ds_fmt(buf, len, idle, LANG_IPODUI_DEV_ST_VCSLEEP,
                      onoff(idle), chg);
    }
    case DS_LVL5:
        return ds_fmt(buf, len, bcm_boot_level5_setting(),
                      LANG_IPODUI_DEV_ST_LVL5,
                      onoff(bcm_boot_level5_setting()), chg);
#endif
#ifdef HAVE_BCMFW_DEVTEST
    case DS_JOBS:
        return ds_fmt(buf, len, ds_jobs, LANG_IPODUI_DEV_ST_JOBS,
                      onoff(ds_jobs), chg);
#endif
#if defined(HAVE_PP_GOV) && defined(HAVE_PLL_IDLE)
    case DS_GOV:
        snprintf(v, sizeof(v), "%s%s", devgov_name(devgov_get()),
                 devgov_refused() ? (const char *)str(LANG_IPODUI_DEV_GOV_REFUSED)
                                  : "");
        return ds_fmt(buf, len, devgov_get() != DEVGOV_OFF,
                      LANG_IPODUI_DEV_GOV, v, chg);
#endif
#ifdef HAVE_UI_THREAD_SPLIT
    case DS_UI:
    {
        static const int ids[3] = { LANG_IPODUI_DEV_UI_MAIN,
                                    LANG_IPODUI_DEV_UI_THREAD,
                                    LANG_IPODUI_DEV_UI_COP };
        enum fe_mode m = fe_be_mode(), n = fe_be_mode_next();

        if (m == n)
            snprintf(v, sizeof(v), "%s", str(ids[m % 3]));
        else
            snprintf(v, sizeof(v), str(LANG_IPODUI_DEV_NEXT_START),
                     str(ids[m % 3]), str(ids[n % 3]));
        return ds_fmt(buf, len, m != FE_INLINE || n != FE_INLINE,
                      LANG_IPODUI_DEV_ST_UI, v, chg);
    }
#endif
    case DS_SOFT:
        return ds_fmt(buf, len, ui_sleep_soft_get(), LANG_IPODUI_DEV_ST_SOFT,
                      onoff(ui_sleep_soft_get()), chg);
#ifdef HAVE_STORAGE_POWEROFF
    case DS_DRIVE:
        return ds_fmt(buf, len, !storage_poweroff_allowed(),
                      LANG_IPODUI_DEV_ST_DRIVE,
                      str(storage_poweroff_allowed()
                          ? LANG_IPODUI_DEV_DRIVE_OFF
                          : LANG_IPODUI_DEV_DRIVE_STANDBY), chg);
#endif
    case DS_DAC:
        return ds_fmt(buf, len, !dac_standby_allowed(), LANG_IPODUI_DEV_ST_DAC,
                      onoff(dac_standby_allowed()), chg);
    case DS_PLOG:
        if (powerlog_is_on())
            snprintf(v, sizeof(v), str(LANG_IPODUI_DEV_EVERY),
                     powerlog_interval());
        else
            snprintf(v, sizeof(v), "%s", onoff(false));
        return ds_fmt(buf, len, powerlog_is_on(), LANG_IPODUI_DEV_ST_PLOG, v,
                      chg);
#ifdef HAVE_PLL_IDLE
    case DS_LIT:
        return ds_fmt(buf, len, clockpol_lit24(), LANG_IPODUI_DEV_ST_LIT,
                      clockpol_lit24() ? "24" : "30", chg);
    case DS_IDLE24:
        return ds_fmt(buf, len, !clockpol_enabled(), LANG_IPODUI_DEV_ST_IDLE24,
                      onoff(clockpol_enabled()), chg);
#endif
#ifdef HAVE_ATA_UDMA_FORCE
    case DS_UDMA:
        return ds_fmt(buf, len, udma_is_on(), LANG_IPODUI_DEV_ST_UDMA,
                      onoff(udma_is_on()), chg);
#endif
#if defined(HAVE_USB_STATS) && !defined(SIMULATOR)
    case DS_USBPF:
    {
        static const int pf[USB_PF_MODES] = { LANG_IPODUI_DEV_USB_BATCH,
                                              LANG_IPODUI_DEV_OFF,
                                              LANG_IPODUI_DEV_USB_WORKER };
        int m = usb_storage_prefetch_mode();

        return ds_fmt(buf, len, m != 0, LANG_IPODUI_DEV_ST_USBPF,
                      str(pf[m < USB_PF_MODES ? m : 0]), chg);
    }
    case DS_USBINQ:
        return ds_fmt(buf, len, !usb_storage_inquiry_static(),
                      LANG_IPODUI_DEV_ST_USBINQ,
                      str(usb_storage_inquiry_static()
                          ? LANG_IPODUI_DEV_USB_STATIC
                          : LANG_IPODUI_DEV_USB_CORE), chg);
#if defined(IPOD_VIDEO)
    case DS_USBATT:
        return ds_fmt(buf, len, usb_drv_attach_reinit(),
                      LANG_IPODUI_DEV_ST_USBATT,
                      str(usb_drv_attach_reinit() ? LANG_IPODUI_DEV_USB_LEGACY
                                                  : LANG_IPODUI_DEV_USB_NORMAL),
                      chg);
#endif
#endif
    case DS_BENCH:
        if (!ds.found || !ds.started)
            return str(LANG_IPODUI_DEV_ST_BENCH_NONE);
        snprintf(buf, len, str(LANG_IPODUI_DEV_ST_BENCH),
                 ds.date[0] ? ds.date : "-",
                 str(ds.ended ? LANG_IPODUI_DEV_BENCH_ENDED
                              : LANG_IPODUI_DEV_BENCH_OPEN));
        return buf;
    case DS_BENCH_N:
        if (!ds.found || !ds.started)
            return "";
        snprintf(buf, len, str(LANG_IPODUI_DEV_ST_BENCH_N), ds.passed,
                 ds.failed, ds.skipped);
        return buf;
#ifdef HAVE_BCMFW_DEVTEST
    case DS_HELD:
        return ds_held ? (const char *)str(LANG_IPODUI_DEV_ST_HELD) : "";
#endif
    default:
        return "";
    }
}

static const char *ds_get_name(int item, void *data, char *buf, size_t len)
{
    bool chg;

    (void)data;
    return ds_row_text(item, buf, len, &chg);
}

int devstatus_show(void)
{
    struct simplelist_info info;
    char buf[64];
    bool chg;
    int k;

    devstatus_bench_load();
#ifdef HAVE_BCMFW_DEVTEST
    ds_jobs = file_exists(DSW_JOBS);    /* once, not at every redraw */
    ds_held = devswitch_held();
#endif
    ds_changed = 0;
    for (k = 1; k < DS_NROWS; k++)
    {
        ds_row_text(k, buf, sizeof(buf), &chg);
        ds_changed += chg;
    }
    simplelist_info_init(&info, str(LANG_IPODUI_DEV_STATUS), DS_NROWS, NULL);
    info.get_name = ds_get_name;
    return simplelist_show_list(&info) ? 1 : 0;
}
#endif /* HAVE_IPOD_UI_DEVMODE */
