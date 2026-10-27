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

/* The bench session in the simulator: the stubs devbench_sim_calls.h maps
 * devbench.c's device calls to, the model's state, and the bench_sim
 * script command (its fault switches and the results file's checks).
 * devbench_sim.h says what is modelled and what is real. */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "config.h"
#include "devbench_sim.h"

#ifdef DEVBENCH_SIM
#include "string-extra.h"
#include "action.h"
#include "gui/list.h"
#include "kernel.h"
#include "file.h"
#include "dir.h"
#include "core_alloc.h"
#include "audio.h"
#include "misc.h"
#include "ipodui/fe_be.h"
#include "bcmjob_pp.h"
#include "video/bcmres.h"
#include "video/bcmprobe_image.h"
#include "video/vplayer.h"
#include "devbench.h"
#include "bcm-devtest.h"          /* BCMDT_STAGE */
#include "../../bcmfw/hostproto.h"
#include "../../bcmfw/jobs.h"
#define DEVBENCH_SIM_IMPL        /* the stubs' prototypes, no mapping */
#include "devbench_sim_calls.h"

#define BENCH_DIR       ROCKBOX_DIR "/bench"    /* devbench.c's */

static void say(const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, "bench_sim: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* ---- The model's state (marker files: they survive a "reboot") ------- */

static bool has(const char *path)
{
    return file_exists(path);
}

static void touch(const char *path, const char *text)
{
    int fd;

    mkdir(DBS_DIR);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return;
    if (text)
        write(fd, text, strlen(text));
    close(fd);
}

static int read_small(const char *path, char *buf, int n)
{
    int fd = open(path, O_RDONLY), got;

    buf[0] = '\0';
    if (fd < 0)
        return -1;
    got = read(fd, buf, n - 1);
    close(fd);
    if (got < 0)
        got = 0;
    buf[got] = '\0';
    while (got > 0 && (buf[got - 1] == '\n' || buf[got - 1] == ' '))
        buf[--got] = '\0';
    return got;
}

/* A start's first look at the model: what the last start set for this one
   (the boot step's display driver, ucop's UI switch) takes effect */
static bool driver_now, cop_now;

static void boot_once(void)
{
    static bool booted;
    char next[16];

    if (booted)
        return;
    booted = true;
    if (has(DBS_DRIVER_NEXT))
    {
        remove(DBS_DRIVER_NEXT);
        touch(DBS_DRIVER, NULL);
    }
    if (read_small(DBS_COP_NEXT, next, sizeof(next)) >= 0)
    {
        remove(DBS_COP_NEXT);
        if (!strcmp(next, "cop"))
            touch(DBS_COP, NULL);
        else
            remove(DBS_COP);
    }
    driver_now = has(DBS_DRIVER);
    cop_now = has(DBS_COP);
    say("start: display driver %s, ui %s", driver_now ? "yes" : "no",
        cop_now ? "cop" : "cpu");
}

/* the fault switch DBS_FAIL<id>: that row or stage fails */
static bool fails(const char *id)
{
    char path[MAX_PATH];

    snprintf(path, sizeof(path), DBS_FAIL "%s", id);
    return has(path);
}

/* ---- devbcm.c's session helpers ---------------------------------------- */

static bool bench_batch;
static void (*bench_hook)(const char *line);

void dbs_bench_mode(bool batch, void (*hook)(const char *line))
{
    bench_batch = batch;
    bench_hook = hook;
}

/* a row's line: to the step's hook (its verdict words), as devbcm.c's
   rows send their log and summary lines */
static void line(const char *text)
{
    say("row %s", text);
    if (bench_hook)
        bench_hook(text);
}

/* the device's devbcm_log(): the step's log line into bcmtest.log (open,
   write, close: bcm-devtest-hal.c's dt_log, its ms stamp first), and to
   the hook while a row runs */
void dbs_log(const char *text)
{
    char buf[300];
    int fd, n;

    say("log %s", text);
    n = snprintf(buf, sizeof(buf), "%6ld %s\n", (long)(current_tick * 10),
                 text);
    fd = open(ROCKBOX_DIR "/bcmtest.log", O_WRONLY | O_CREAT | O_APPEND,
              0666);
    if (fd >= 0)
    {
        write(fd, buf, n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1);
        close(fd);
    }
    if (bench_hook)
        bench_hook(text);
}

bool dbs_show(struct simplelist_info *info)
{
    return simplelist_show_list(info);
}

/* devbcm.c's confirm_cb and choice_cb, for the same screens */
static int confirm_line, choice_first, choice_got;
static bool confirmed;

static int confirm_cb(int action, struct gui_synclist *lists)
{
    lists->wraparound = false;
    if (action == ACTION_STD_OK)
    {
        if (gui_synclist_get_sel_pos(lists) != confirm_line)
        {
            say("SELECT on row %d, not the go line %d",
                gui_synclist_get_sel_pos(lists), confirm_line);
            return ACTION_NONE;
        }
        confirmed = true;
        return ACTION_STD_CANCEL;
    }
    return action;
}

static int choice_cb(int action, struct gui_synclist *lists)
{
    lists->wraparound = false;
    if (action == ACTION_STD_OK)
    {
        int sel = gui_synclist_get_sel_pos(lists);

        if (sel < choice_first)
        {
            say("SELECT on row %d, not an answer (from %d)", sel,
                choice_first);
            return ACTION_NONE;
        }
        choice_got = sel - choice_first;
        return ACTION_STD_CANCEL;
    }
    return action;
}

bool dbs_lines_go(const char *title, const char *const *lines, int n,
                  const char *menu, const char *go)
{
    struct simplelist_info info;
    int i;

    boot_once();
    simplelist_info_init(&info, (char *)title, 0, NULL);
    simplelist_reset_lines();
    for (i = 0; i < n; i++)
        simplelist_addline("%s", lines[i]);
    if (menu)
        simplelist_addline("%s", menu);
    confirm_line = menu ? n + 1 : n;
    simplelist_addline("%s", go);
    confirmed = false;
    info.action_callback = confirm_cb;
    say("screen \"%s\" lines=%d", title, confirm_line + 1);
    simplelist_show_list(&info);
    say("screen \"%s\" -> %s", title, confirmed ? "go" : "menu");
    return confirmed;
}

int dbs_lines_choose(const char *title, const char *const *lines, int n,
                     const char *const *opts, int nopts)
{
    struct simplelist_info info;
    int j;

    boot_once();
    simplelist_info_init(&info, (char *)title, 0, NULL);
    simplelist_reset_lines();
    for (j = 0; j < n; j++)
        simplelist_addline("%s", lines[j]);
    choice_first = n;
    for (j = 0; j < nopts; j++)
        simplelist_addline("%s", opts[j]);
    choice_got = -1;
    info.action_callback = choice_cb;
    say("ask \"%s\" answers=%d", title, nopts);
    simplelist_show_list(&info);
    say("ask \"%s\" -> %d %s", title, choice_got,
        choice_got >= 0 ? opts[choice_got] : "(menu)");
    return choice_got;
}

/* ---- The rows and stages ------------------------------------------------ */

static bool row(const char *id, const char *what)
{
    char text[112];

    boot_once();
    snprintf(text, sizeof(text), "%s: %s: %s (sim model)", id, what,
             fails(id) ? "FAIL" : "PASS");
    line(text);
    return false;                   /* no USB */
}

bool dbs_row_c1(void)    { return row("c1", "engine 2 from the COP"); }
bool dbs_row_c2(void)    { return row("c2", "shared cache data array"); }
bool dbs_row_c3(void)    { return row("c3", "COP sleep under yield"); }
bool dbs_row_pb1(void)   { return row("pb1", "probe"); }
bool dbs_row_pb2(void)   { return row("pb2", "probe"); }
bool dbs_row_pb3(void)   { return row("pb3", "probe"); }
bool dbs_row_pb4(void)   { return row("pb4", "probe"); }
bool dbs_row_pb6(void)   { return row("pb6", "probe"); }
bool dbs_row_pb7(void)   { return row("pb7", "probe"); }
bool dbs_row_pb8(void)   { return row("pb8", "probe"); }
bool dbs_row_pb9(void)   { return row("pb9", "probe"); }
bool dbs_row_pw1(void)   { return row("pw1", "probe"); }
bool dbs_row_pw2(void)   { return row("pw2", "probe"); }
bool dbs_row_pw3(void)   { return row("pw3", "probe"); }
bool dbs_row_pw4(void)   { return row("pw4", "probe"); }
bool dbs_row_pw5(void)   { return row("pw5", "probe"); }
bool dbs_row_pw6(void)   { return row("pw6", "probe"); }
bool dbs_row_pw7(void)   { return row("pw7", "probe"); }
bool dbs_row_pw8(void)   { return row("pw8", "probe"); }
bool dbs_row_s6(void)    { return row("s6", "stage 6 test clips"); }
bool dbs_row_s7(void)    { return row("s7", "stage 7 video controls"); }
bool dbs_row_vfeed(void) { return row("vfeed", "video pace"); }
bool dbs_row_b13(void)   { return row("b13", "warm swap"); }
bool dbs_row_b5(void)    { return row("b5", "window push"); }
bool dbs_row_cr1(void)   { return row("cr1", "COMMIT readback"); }
bool dbs_row_rstat(void) { return row("rstat", "boot path status"); }

static int verdict_row(const char *id, const char *what)
{
    row(id, what);
    return fails(id) ? BENCH_FAIL : BENCH_PASS;
}

int dbs_vclk_verify(void) { return verdict_row("vbv", "clock bands"); }
int dbs_vclk_gov(void)    { return verdict_row("vgov", "clock governor"); }

/* The boot path's DMA rung for the next start: the display driver then */
bool dbs_boot_dma(void)
{
    boot_once();
    touch(DBS_DRIVER_NEXT, NULL);
    line("boot path: DMA rung set for the next start (sim model)");
    return false;
}

/* The device test's stages: each asked for passes unless its fault switch
   is on (fail_s05, fail_s4b, fail_s4c); a pass of stage 5 writes the
   flags */
unsigned dbs_bench_stages(unsigned stages)
{
    const char *id = stages & BCMDT_STAGE(BCMDT_STAGE_4C) ? "s4c"
                   : stages & BCMDT_STAGE(BCMDT_STAGE_4B) ? "s4b" : "s05";
    char text[96];

    boot_once();
    if (fails(id))
    {
        snprintf(text, sizeof(text), "%s: stages %x: FAIL (sim model)", id,
                 stages);
        line(text);
        return 0;
    }
    snprintf(text, sizeof(text), "%s: stages %x: PASS (sim model)", id,
             stages);
    line(text);
    if (!strcmp(id, "s05"))
        remove(DBS_NOFLAGS);
    return stages;
}

static bool flags(const char **why, const char *what)
{
    boot_once();
    if (has(DBS_NOFLAGS))
    {
        *why = what;
        return false;
    }
    return true;
}

bool dbs_4b_allowed(const char **why)
{
    return flags(why, "no stage 4 pass flag for this build");
}

bool dbs_4c_allowed(const char **why)
{
    return flags(why, "no stage 4b pass flag for this build");
}

bool dbs_5i_allowed(const char **why)
{
    return flags(why, "no stage 5 pass flag for this build");
}

void dbs_forget_4c(void)
{
    say("4c pass flag removed");
}

/* ---- The chip ----------------------------------------------------------- */

#define DBS_BLOCK       0x100000    /* the merged image's job block */

/* The probe image's port reads of the resident image: the merged image's
   job block, its start-up checks passed (J_VPU 3), when this start has the
   display driver; else no block */
int dbs_video_rd(uint32_t addr, uint32_t *val)
{
    boot_once();
    if (addr == HP_JOBS)
        *val = driver_now ? DBS_BLOCK : 0;
    else if (addr == DBS_BLOCK + J_VERSION)
        *val = J_VERSION_WORD;
    else if (addr == DBS_BLOCK + J_VPU)
        *val = fails("jvpu") ? J_VPU_CHECKED : J_VPU_CHECKED | J_VPU_SHIFT;
    else
        *val = 0;                   /* J_OPS: no DECODE; batch 4: none */
    return 0;
}

/* vplayer_probe(): the probe image read into a pinned block of its own
   (bcmres_read when the file is on the disk, else a block of its size:
   the same memory), then pre() and fn() on the model's port; -2 when the
   image can't be had, as load_image_from() fails on the device.  The
   fault switch DBS_LOWMEM refuses the block while music plays (sitting
   4c: the visualiser's album still playing, the probe image found no
   room) */
int dbs_vplayer_probe(int (*pre)(void *ctx),
                      int (*fn)(struct bvp *b, uint32_t fast, void *ctx),
                      void *ctx)
{
    static const struct vimage probe =
        { VPLAYER_PROBE_IMAGE, BCMPROBE_IMAGE_BYTES, BCMPROBE_IMAGE_FNV };
    uint8_t *img = NULL;
    uint32_t bytes = 0;
    int h, rc;
    bool playing = audio_status() & AUDIO_STATUS_PLAY;

    boot_once();
    if (has(VPLAYER_PROBE_IMAGE))
        h = bcmres_read(&probe, &img, &bytes);
    else
    {
        h = core_alloc(BCMPROBE_IMAGE_BYTES);
        if (h > 0)
            core_pin(h);
    }
    say("probe image: %s block of %lu bytes (music %s, free %lu)",
        h > 0 ? "a" : "no", (unsigned long)BCMPROBE_IMAGE_BYTES,
        playing ? "playing" : "stopped", (unsigned long)core_available());
    if (h > 0 && playing && has(DBS_LOWMEM))
    {
        say("probe image: refused (fault switch: low memory, music plays)");
        core_unpin(h);
        core_free(h);
        h = 0;
    }
    if (h <= 0)
        return -2;
    rc = pre ? pre(ctx) : 0;
    if (rc == 0)
        rc = fn(NULL, 0, ctx);
    core_unpin(h);
    core_free(h);
    return rc;
}

/* vplayer_peek(): the running image's port reads only, no image file and
   no memory (the device hands the chip straight back); -1 while a video
   holds the chip, as the device's */
int dbs_vplayer_peek(int (*pre)(void *ctx), void *ctx)
{
    bool video = vplayer_running();

    boot_once();
    say("peek: the running image's port reads (music %s, video %s)",
        audio_status() & AUDIO_STATUS_PLAY ? "playing" : "stopped",
        video ? "on" : "off");
    if (video)
        return -1;
    return pre(ctx);
}

bool dbs_cop_stuck(void)
{
    return false;
}

const char *dbs_off_why(bool *restart)
{
    boot_once();
    *restart = !driver_now;
    return driver_now ? NULL
         : "the display program isn't running in this start (sim model)";
}

/* the job model's own self-test, when this start has the display driver */
int dbs_selftest(void)
{
    boot_once();
    if (!driver_now)
        return 1;
    if (fails("jobs"))
        return -1;
    return bcmjob_selftest();
}

int dbs_inject_fail(uint32_t op, uint32_t n)
{
    say("inject op=0x%02lx n=%lu: no job driver in the sim",
        (unsigned long)op, (unsigned long)n);
    return -1;
}

/* ---- The UI on the COP -------------------------------------------------- */

enum fe_mode dbs_fe_mode(void)
{
    boot_once();
    return cop_now ? FE_COP : fe_be_mode();
}

void dbs_fe_mode_set_next(enum fe_mode m)
{
    boot_once();
    touch(DBS_COP_NEXT, m == FE_COP ? "cop" : "cpu");
    touch(DBS_UI_MARK, m == FE_COP ? "cop" : "cpu");
    say("ui switch for the next start: %s", m == FE_COP ? "cop" : "cpu");
}

/* the model's switch: DBS_UI_MARK; none: the build default (the sim's
   own fe_be.c) */
enum fe_mode dbs_fe_mode_next(void)
{
    char m[8];

    if (read_small(DBS_UI_MARK, m, sizeof(m)) >= 0)
        return !strcmp(m, "cop") ? FE_COP : FE_CPU_THREAD;
    return fe_be_mode_next();
}

void dbs_fe_mode_clear_next(void)
{
    boot_once();
    remove(DBS_UI_MARK);
    touch(DBS_COP_NEXT, "def");     /* the next start: not the COP */
    say("ui switch cleared: the next start runs the build default");
}

void dbs_fe_stats_get(struct fe_be_stats *st)
{
    static int calls;

    fe_be_stats_get(st);
    if (calls++ && fails("ucop2"))
    {
        st->stalls++;               /* the soak's end: one stall */
        say("fault switch: ucop2 sees a stall");
    }
}

/* ---- The session's switches, the watchdog, the results file ------------ */

bool dbs_switch_begin(int mode)
{
    (void)mode;
    return false;
}

void dbs_switch_end(void)
{
}

void dbs_panic_record(const char *msg)
{
    say("watchdog record: %s", msg);
}

void dbs_reboot(void)
{
    say("watchdog restart (not taken in the sim)");
}

/* The results file's appends (devbench.c put()): with DBS_PUTFAIL holding
   n, the n-th from now fails, once (sitting 4c's lost lines) */
int dbs_open(const char *path, int flags, ...)
{
    int mode = 0666;

    if (flags & O_CREAT)
    {
        va_list ap;

        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    if ((flags & O_APPEND) && !strncmp(path, BENCH_DIR "/", sizeof(BENCH_DIR)))
    {
        char buf[16];
        int n;

        /* putfails <k>: the next k opens fail in a row (put()'s tries
           too: k = 3 x the writes refused; the RAM queue holds them, or
           past PUT_QUEUE bcmtest.log's "bench unsaved:" lines) */
        if (read_small(DBS_PUTFAILS, buf, sizeof(buf)) >= 0)
        {
            n = atoi(buf);
            if (n <= 1)
                remove(DBS_PUTFAILS);
            else
            {
                snprintf(buf, sizeof(buf), "%d", n - 1);
                touch(DBS_PUTFAILS, buf);
            }
            say("results file: open refused in a row (fault switch)");
            return -1;
        }
        if (read_small(DBS_PUTFAIL, buf, sizeof(buf)) >= 0)
        {
            n = atoi(buf);
            if (n <= 1)
            {
                remove(DBS_PUTFAIL);
                say("results file: open refused (fault switch)");
                return -1;
            }
            snprintf(buf, sizeof(buf), "%d", n - 1);
            touch(DBS_PUTFAIL, buf);
        }
    }
    return open(path, flags, mode);
}

/* ---- The bench_sim command ---------------------------------------------- */

static void rm_dir_files(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    char path[MAX_PATH];

    if (!d)
        return;
    while ((e = readdir(d)))
    {
        if (e->d_name[0] == '.')
            continue;
        snprintf(path, sizeof(path), "%.60s/%.180s", dir, (char *)e->d_name);
        remove(path);
    }
    closedir(d);
}

/* The newest results file (devbench.c's find_latest: the names sort by
   date), into path; false if none */
static bool newest(char *path, size_t n)
{
    DIR *d = opendir(BENCH_DIR);
    struct dirent *e;
    char best[MAX_PATH] = "";

    if (!d)
        return false;
    while ((e = readdir(d)))
    {
        const char *nm = (char *)e->d_name;
        size_t l = strlen(nm);

        if (l > 4 && !strcmp(nm + l - 4, ".txt") && strcmp(nm, best) > 0)
            strlcpy(best, nm, sizeof(best));
    }
    closedir(d);
    if (!best[0])
        return false;
    snprintf(path, n, BENCH_DIR "/%.200s", best);
    return true;
}

#define CHECK_MAX_LINES 400

/* expect: the newest results file against the picked run's table.
 *   order        every pick once, with a result, in the picks' order, and
 *                no other step; a step again only after a "retry" line
 *   ended        an end line
 *   verified     a verified line; noverified: none
 *   token        a clock150 token line; notoken: none
 *   nextcop      the developer switch says cop (DBS_UI_MARK);
 *                nextdefault: no switch (the next start: the default)
 *   <id>=<res>   that step's result (pass, fail, skip, done, hang), or
 *                autoskip (a skip whose note says auto-skip: needs ...)
 * Logs "expect_bench ok", or "expect_bench failed: <why>" per failure
 * (scripts/shots.sh fails the run on it). */
static void expect(char *argv[], int argc, int lineno)
{
    static char lines[CHECK_MAX_LINES][BENCH_LINE_MAX + 64];
    const struct bench_pick *picks = bench_reverify_picks;
    int np = (int)(sizeof(bench_reverify_picks) / sizeof(bench_reverify_picks[0]));
    char path[MAX_PATH];
    int fd, n = 0, i, a, bad = 0, pos = 0;
    bool reverify = false, ended = false, verified = false, token = false;

    if (!newest(path, sizeof(path)))
    {
        fprintf(stderr, "sim_script:%d: expect_bench failed: no results "
                "file\n", lineno);
        return;
    }
    fd = open(path, O_RDONLY);
    while (fd >= 0 && n < CHECK_MAX_LINES &&
           read_line(fd, lines[n], sizeof(lines[n])) > 0)
        n++;
    if (fd >= 0)
        close(fd);
    for (i = 0; i < n; i++)
    {
        const char *l = lines[i];

        if (!strncmp(l, "session ", 8) && strstr(l, " mode=reverify"))
            reverify = true;
        else if (!strncmp(l, "end ", 4))
            ended = true;
        else if (!strncmp(l, "verified ", 9))
            verified = true;
        else if (!strncmp(l, "token clock150 ", 15))
            token = true;
    }
    for (a = 1; a < argc; a++)
    {
        const char *s = argv[a];
        const char *eq = strchr(s, '=');

        if (!strcmp(s, "order"))
        {
            bool retry_ok[64] = { false };
            int seen[64] = { 0 };

            if (!reverify)
            {
                fprintf(stderr, "sim_script:%d: expect_bench failed: not a "
                        "re-verify session\n", lineno);
                bad++;
            }
            pos = 0;
            for (i = 0; i < n; i++)
            {
                char id[24];
                const char *l = lines[i], *r;
                int p, len;

                if (!strncmp(l, "retry step=", 11))
                {
                    for (p = 0; p < np; p++)
                        if (!strcmp(l + 11, picks[p].id))
                            retry_ok[p] = true;
                    continue;
                }
                if (strncmp(l, "step=", 5) || !(r = strstr(l, " result=")))
                    continue;
                if (!strncmp(r, " result=start", 13))
                    continue;
                len = (int)(r - (l + 5));
                if (len <= 0 || len >= (int)sizeof(id))
                    continue;
                memcpy(id, l + 5, len);
                id[len] = '\0';
                for (p = 0; p < np && strcmp(id, picks[p].id); p++)
                    ;
                if (p == np)
                {
                    fprintf(stderr, "sim_script:%d: expect_bench failed: "
                            "%s is not in the re-verify run\n", lineno, id);
                    bad++;
                    continue;
                }
                if (seen[p]++ && !retry_ok[p])
                {
                    fprintf(stderr, "sim_script:%d: expect_bench failed: "
                            "%s has %d results (run again)\n", lineno, id,
                            seen[p]);
                    bad++;
                }
                retry_ok[p] = false;
                if (p < pos)
                {
                    fprintf(stderr, "sim_script:%d: expect_bench failed: "
                            "%s out of order (after %s)\n", lineno, id,
                            picks[pos].id);
                    bad++;
                }
                else
                    pos = p;
            }
            for (i = 0; i < np; i++)
                if (!seen[i] && !((picks[i].role & BENCH_R_HELD) &&
                                  BENCH_HOLD_LATER))
                {
                    fprintf(stderr, "sim_script:%d: expect_bench failed: "
                            "%s has no result\n", lineno, picks[i].id);
                    bad++;
                }
        }
        else if (!strcmp(s, "ended") && !ended)
        {
            fprintf(stderr, "sim_script:%d: expect_bench failed: no end "
                    "line\n", lineno);
            bad++;
        }
        else if (!strcmp(s, "nextcop") || !strcmp(s, "nextdefault"))
        {
            char m[8];
            bool mark = read_small(DBS_UI_MARK, m, sizeof(m)) >= 0;
            bool cop = mark && !strcmp(m, "cop");

            if (!strcmp(s, "nextcop") ? !cop : mark)
            {
                fprintf(stderr, "sim_script:%d: expect_bench failed: %s "
                        "(switch: %s)\n", lineno, s, mark ? m : "none");
                bad++;
            }
        }
        else if ((!strcmp(s, "verified") && !verified) ||
                 (!strcmp(s, "noverified") && verified) ||
                 (!strcmp(s, "token") && !token) ||
                 (!strcmp(s, "notoken") && token))
        {
            fprintf(stderr, "sim_script:%d: expect_bench failed: %s\n",
                    lineno, s);
            bad++;
        }
        else if (eq)
        {
            char want[96];
            const char *got = NULL, *note = NULL;
            int len = (int)(eq - s);

            snprintf(want, sizeof(want), "step=%.*s result=", len, s);
            for (i = n - 1; i >= 0 && !got; i--)
                if (!strncmp(lines[i], want, strlen(want)) &&
                    strncmp(lines[i] + strlen(want), "start", 5))
                {
                    got = lines[i] + strlen(want);
                    note = strstr(lines[i], " note=");
                }
            if (!got)
            {
                fprintf(stderr, "sim_script:%d: expect_bench failed: %.*s "
                        "has no result\n", lineno, len, s);
                bad++;
            }
            else if (!strcmp(eq + 1, "autoskip")
                     ? strncmp(got, "skip", 4) || !note ||
                       strncmp(note + 6, "auto-skip: needs ", 17)
                     : strncmp(got, eq + 1, strlen(eq + 1)) ||
                       got[strlen(eq + 1)] != ' ')
            {
                fprintf(stderr, "sim_script:%d: expect_bench failed: %.*s "
                        "is \"%.40s\", not %s\n", lineno, len, s, got,
                        eq + 1);
                bad++;
            }
        }
    }
    if (!bad)
        fprintf(stderr, "sim_script:%d: expect_bench ok (%d lines)\n",
                lineno, n);
}

/* bench_sim reset                  no results files, the model's defaults
 * bench_sim set <name> [text]      a switch: noflags, lowmem, putfail <n>,
 *                                  putfails <k>, fail_<id>
 * bench_sim clear <name>
 * bench_sim expect <checks...>     (expect(), above)
 * bench_sim dump                   the newest results file, to the log */
bool devbench_sim_cmd(char *argv[], int argc, int lineno)
{
    char path[MAX_PATH];

    if (argc < 2)
        return false;
    if (!strcmp(argv[1], "reset"))
    {
        rm_dir_files(BENCH_DIR);
        rm_dir_files(DBS_DIR);
        remove(ROCKBOX_DIR "/bcmtest.log");     /* the steps' log too */
        mkdir(DBS_DIR);
        fprintf(stderr, "sim_script:%d: bench_sim reset\n", lineno);
        return true;
    }
    if ((!strcmp(argv[1], "set") || !strcmp(argv[1], "clear")) && argc >= 3)
    {
        snprintf(path, sizeof(path), DBS_DIR "/%s", argv[2]);
        if (!strcmp(argv[1], "set"))
            touch(path, argc >= 4 ? argv[3] : NULL);
        else
            remove(path);
        fprintf(stderr, "sim_script:%d: bench_sim %s %s%s%s\n", lineno,
                argv[1], argv[2], argc >= 4 ? " " : "",
                argc >= 4 ? argv[3] : "");
        return true;
    }
    if (!strcmp(argv[1], "expect"))
    {
        expect(argv + 1, argc - 1, lineno);
        return true;
    }
    if (!strcmp(argv[1], "dump"))
    {
        char l[BENCH_LINE_MAX + 64];
        int fd;

        if (!newest(path, sizeof(path)) || (fd = open(path, O_RDONLY)) < 0)
        {
            fprintf(stderr, "sim_script:%d: bench_sim dump: no file\n",
                    lineno);
            return true;
        }
        while (read_line(fd, l, sizeof(l)) > 0)
            fprintf(stderr, "bench file: %s\n", l);
        close(fd);
        return true;
    }
    return false;
}
#endif /* DEVBENCH_SIM */
