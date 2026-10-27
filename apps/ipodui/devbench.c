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

/* The guided bench session: Developer > "Bench session".  The
 * pure part (step table shape, results file lines, resume, verdicts) is
 * devbench.h; this is the device side: the step table, the runs, the
 * owner's screens, the results file and the watchdog.
 *
 * Part A runs every step that needs no owner, one after another, after
 * one confirm screen that lists them; each row's own confirm and summary
 * screens are skipped by devbcm_bench_mode()'s batch flag (never by
 * changing the rows' defaults), and its log and summary lines feed the
 * step's verdict.  Part B shows one instruction a step, runs it, and asks
 * its question.  A manual step (no function yet) is done by the owner
 * outside the session; the next press of the row asks its question.
 *
 * The results file (ROCKBOX_DIR "/bench/<date>-<build>.txt", never named
 * on screen) is appended a line at a time, each write closed at once, so
 * a restart keeps everything up to the step that was running.  Part A's
 * watchdog restarts the iPod if a step overruns its limit; the file then
 * shows the step started and never ended, and the next press offers to
 * resume after it.  Writes happen only on this thread, between steps. */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "string-extra.h"
#include "action.h"
#include "gui/list.h"
#include "kernel.h"
#include "system.h"
#include "file.h"
#include "dir.h"
#include "misc.h"
#include "timefuncs.h"
#include "version.h"
#include "bcm-devtest.h"
#include "devbcm.h"
#include "devbench.h"
#include "devbench_power.h"      /* p19-battery: the battery rows */
#ifdef HAVE_PLL_IDLE
#include "ipodui/clockpol.h"     /* lit24ui */
#endif
#include "ipodui/litmiss.h"      /* lit24ui's frame counters */
#include "ipodui/fe_be.h"        /* P19 19-9: the UI-on-COP soak */
#include "devbench_vclk.h"       /* P19 PW4: the clock bands, the governor */
#include "uilog.h"
#include "hang_watch.h"          /* the hang record's bench step */
#include "ipodui/devstatus.h"    /* the session's own switches */
#include "storage.h"
#include "backlight.h"
#include "lcd.h"
#include "core_alloc.h"
#include "audio.h"
#include "pcmbuf.h"              /* ucop2: the PCM underruns */

#include "devbench_sim.h"         /* DEVBENCH_SIM: the simulator's runner */

#if (defined(HAVE_BCMFW_VIDEO) && !defined(SIMULATOR)) || defined(DEVBENCH_SIM)
#ifndef DEVBENCH_SIM
#include "bcm-coplab.h"
#include "panic_record.h"
#endif
#include "video/vplayer.h"
#include "video/bcmvideo_image.h" /* BCMVIDEO_IMAGE_*: the image the build carries */
#include "../../bcmfw/hostproto.h"
#include "../../bcmfw/jobs.h"
#if defined(HAVE_BCM_JOBS) || defined(DEVBENCH_SIM)
#include "bcmjob_pp.h"
#endif
#ifdef DEVBENCH_SIM
#include "devbench_sim_calls.h"  /* the device's calls as the sim's stubs */
#endif

#define BENCH_DIR       ROCKBOX_DIR "/bench"
#define BENCH_KEEP      6           /* a step's log lines kept for the file */
#define BENCH_KEEP_LEN  112
#define BENCH_SAID      3           /* a bench step's own lines */

/* ---- The current step's lines (the hook) ----------------------------- */

static struct
{
    char path[MAX_PATH];
    struct bench_tally_lines lines;
    char keep[BENCH_KEEP][BENCH_KEEP_LEN];
    int nkeep;
    char note[BENCH_KEEP_LEN];
    char said[BENCH_SAID][BENCH_KEEP_LEN];  /* say(): for bcmtest.log */
    int nsaid;
    char pinned[BENCH_KEEP_LEN];    /* a J_VPU reading: always recorded */
    bool restart;                   /* the step couldn't run, and a restart
                                       may fix it (offer_retry) */
} B;

static void hook(const char *line)
{
    enum bench_res r = bench_classify(line);

    bench_lines_add(&B.lines, r);
    if (r != BENCH_NONE)
        strlcpy(B.note, line, sizeof(B.note));
    if (strstr(line, "J_VPU"))
        strlcpy(B.pinned, line, sizeof(B.pinned));
    if (!*line)
        return;
    if (B.nkeep == BENCH_KEEP)
    {
        memmove(B.keep[0], B.keep[1], (BENCH_KEEP - 1) * BENCH_KEEP_LEN);
        B.nkeep--;
    }
    strlcpy(B.keep[B.nkeep++], line, BENCH_KEEP_LEN);
}

/* ---- The file --------------------------------------------------------- */

/* One line, appended and closed (the close commits it).  Never lost
 * (sitting 4c: three lines, vbv's start and result and vgov's start,
 * silently dropped while bcmtest.log, which retries, kept writing; vbv
 * then looked undone and ran again): the open is tried PUT_TRIES times,
 * a line that still can't go is kept in RAM (PUT_QUEUE lines) and goes
 * first at the next write that works; the first failure of a session is
 * logged once with its errno and the files open then (a handle leak or
 * the disk); a line the queue can't hold goes to bcmtest.log instead,
 * where the resume's cross-check (bench_logscan) finds results too. */
#define PUT_TRIES   3
#define PUT_QUEUE   8
#define PUT_LEN     256
static char put_q[PUT_QUEUE][PUT_LEN];
static int put_n;
static bool put_said;

/* the descriptors open now: a handle leak shows here */
static int open_fds(void)
{
    int fd, n = 0;

    for (fd = 0; fd < MAX_OPEN_FILES; fd++)
        if (ffilesize(fd) >= 0)
            n++;
    return n;
}

static bool put_write(int fd, const char *text)
{
    size_t n = strlen(text);

    return write(fd, text, n) == (ssize_t)n;
}

static void put_fault(const char *what, int err, int rc)
{
    char line[120];

    if (put_said)
        return;
    put_said = true;
    snprintf(line, sizeof(line), "FAULT bench put errno=%d rc=%d %s, %d "
             "files open of %d", err, rc, what, open_fds(), MAX_OPEN_FILES);
    ui_log("bench", "%s", line);
    devbcm_log(line);
}

static void put_keep(const char *text)
{
    char line[PUT_LEN + 16];

    if (put_n < PUT_QUEUE)
    {
        strlcpy(put_q[put_n++], text, PUT_LEN);
        return;
    }
    /* the queue full: bcmtest.log has it (one line, no newline) */
    snprintf(line, sizeof(line), "bench unsaved: %s", text);
    line[strcspn(line, "\n")] = '\0';
    devbcm_log(line);
}

static void put(const char *text)
{
    int fd = -1, t, err = 0, k, done = 0;

    if (!B.path[0])
        return;
    for (t = 0; t < PUT_TRIES && fd < 0; t++)
    {
        if (t)
            sleep(HZ / 10);
        fd = open(B.path, O_WRONLY | O_CREAT | O_APPEND, 0666);
        if (fd < 0)
            err = errno;
    }
    if (fd < 0)
    {
        put_fault("open", err, fd);
        if (text)
            put_keep(text);
        return;
    }
    /* the kept lines first, in their order */
    for (k = 0; k < put_n && put_write(fd, put_q[k]); k++)
        done++;
    if (done < put_n || (text && !put_write(fd, text)))
    {
        put_fault("write", errno, -1);
        memmove(put_q[0], put_q[done], (put_n - done) * PUT_LEN);
        put_n -= done;
        close(fd);
        if (text)
            put_keep(text);
        return;
    }
    put_n = 0;
    if (close(fd) < 0)
        put_fault("close", errno, -1);
}

/* the kept lines alone into the file, when it opens again (put(NULL)) */
static void put_flush(void)
{
    if (put_n)
        put(NULL);
}

/* the session's end (or a new session's file) with lines still kept: to
   bcmtest.log.  Not at a leave for an outside step: the kept lines stay
   in RAM (this boot) and go in first at the next write (sitting 4d's sim
   run: draining at the leave lost uck2's start line, so the resume
   offered "Resume after" instead of "Go on"); bcmtest.log's "auto:"
   lines still cover a restart meanwhile (bench_logscan) */
static void put_drain(void)
{
    int k;

    for (k = 0; k < put_n; k++)
    {
        char line[PUT_LEN + 16];

        snprintf(line, sizeof(line), "bench unsaved: %s", put_q[k]);
        line[strcspn(line, "\n")] = '\0';
        devbcm_log(line);
    }
    put_n = 0;
    put_said = false;
}

/* This session's marker in bcmtest.log (bench_logscan): the results
   file's name, at its start and at each resume */
static void log_mark(void)
{
    char line[MAX_PATH + 16];
    const char *name = strrchr(B.path, '/');

    snprintf(line, sizeof(line), BENCH_LOG_MARK "%s", name ? name + 1
                                                       : B.path);
    devbcm_log(line);
}

/* ---- Part A's watchdog ------------------------------------------------
 * Never a restart mid-write (devbench.h, bench_dog_decide): past a step's
 * limit the tick task restarts the iPod only while the disk is quiet; busy,
 * it looks again each tick for up to DOG_BOUND; still busy, it gives up,
 * leaves a record in RAM (the next boot's ipodui.log names it) and the
 * watchdog thread asks the owner for MENU + SELECT.  The bench writes
 * nothing while a step runs: the results file, its auto: lines and
 * ui_log's flush (held, ui_log_hold) all wait for the step's end, when
 * the watchdog is disarmed.  The rows' own bcmtest.log lines are the only
 * writes during a step; the disk check covers them. */
#define DOG_QUIET       (HZ / 2)    /* no transfer stamped this long: quiet */
#define DOG_BOUND       (10 * HZ)   /* the longest it defers */
#define DOG_STACK       2048

static struct bench_dog dog;
static volatile bool dog_gave_up;
static char dog_msg_restart[48], dog_msg_busy[48];
static int dog_stack_h;
static unsigned int dog_thread_id;
static volatile bool dog_quit;

static void dog_tick(void)
{
    switch (bench_dog_decide(&dog, current_tick, storage_disk_is_active(),
                             storage_last_disk_activity(), DOG_QUIET,
                             DOG_BOUND))
    {
    case BENCH_DOG_RESTART:
        /* the results file shows the step started and never ended */
        panic_record_save(0, 0, 0, 0, dog_msg_restart);
        system_reboot();
        break;
    case BENCH_DOG_GIVE_UP:
        panic_record_save(0, 0, 0, 0, dog_msg_busy);
        dog_gave_up = true;
        break;
    default:
        break;
    }
}

/* Only draws the give-up screen (the hung step may hold the UI thread);
 * best effort: if the step never yields, the screen stays as it was and
 * the run sheet's rule stands (a frozen screen past the limit: MENU +
 * SELECT) */
static void dog_thread(void)
{
    bool shown = false;

    while (!dog_quit)
    {
        sleep(HZ / 5);
        if (dog_gave_up && !shown)
        {
            shown = true;
            backlight_on();
            lcd_clear_display();
            lcd_puts(0, 0, "A step hung while the");
            lcd_puts(0, 1, "disk was busy.");
            lcd_puts(0, 3, "Hold MENU and SELECT");
            lcd_puts(0, 4, "to restart, then open");
            lcd_puts(0, 5, "the Bench session row.");
            lcd_update();
        }
    }
}

static void dog_start_thread(void)
{
    dog_quit = false;
    dog_gave_up = false;
    dog_stack_h = core_alloc(DOG_STACK);
    if (dog_stack_h <= 0)
    {
        dog_stack_h = 0;
        return;                     /* the restart still works; no screen */
    }
    core_pin(dog_stack_h);
    dog_thread_id = create_thread(dog_thread, core_get_data(dog_stack_h),
                                  DOG_STACK, 0, "bench dog"
                                  IF_PRIO(, PRIORITY_SYSTEM) IF_COP(, CPU));
    if (!dog_thread_id)
    {
        core_unpin(dog_stack_h);
        core_free(dog_stack_h);
        dog_stack_h = 0;
    }
}

static void dog_stop_thread(void)
{
    if (dog_thread_id)
    {
        dog_quit = true;
        thread_wait(dog_thread_id);
        dog_thread_id = 0;
    }
    if (dog_stack_h)
    {
        core_unpin(dog_stack_h);
        core_free(dog_stack_h);
        dog_stack_h = 0;
    }
}

static void dog_set(const struct bench_step *s)
{
    if (!(s->flags & BENCH_F_AUTO) || !s->limit_s)
        return;
    snprintf(dog_msg_restart, sizeof(dog_msg_restart), "bench watchdog: %s",
             s->id);
    snprintf(dog_msg_busy, sizeof(dog_msg_busy),
             "bench hang, disk busy: %s", s->id);
    bench_dog_arm(&dog, current_tick, (long)s->limit_s * HZ);
    tick_add_task(dog_tick);
}

static void dog_off(void)
{
    dog.armed = false;
    tick_remove_task(dog_tick);
}

/* ---- The steps' functions ---------------------------------------------
 * A row's wrapper returns BENCH_NONE: its verdict is then its lines'
 * (bench_lines_verdict).  The others return their own. */
#define ROW(name, fn) static int name(void) { fn(); return BENCH_NONE; }
ROW(r_c1, devbcm_c1)
ROW(r_c2, devbcm_c2)
ROW(r_c3, devbcm_c3)
ROW(r_pb1, devbcm_pb1)
ROW(r_pb2, devbcm_pb2)
ROW(r_pb3, devbcm_pb3)
ROW(r_pb4, devbcm_pb4)
ROW(r_pb6, devbcm_pb6)
ROW(r_pb7, devbcm_pb7)
ROW(r_pb8, devbcm_pb8)
ROW(r_pb9, devbcm_pb9)
ROW(r_pw1, devbcm_pw1)
ROW(r_pw2, devbcm_pw2)
ROW(r_pw3, devbcm_pw3)
ROW(r_pw4, devbcm_pw4)
ROW(r_pw5, devbcm_pw5)
ROW(r_pw6, devbcm_pw6)
ROW(r_pw7, devbcm_pw7)
ROW(r_pw8, devbcm_pw8)
ROW(r_s6, devbcm_cpu_test6)
ROW(r_s7, devbcm_cpu_test7)
ROW(r_vfeed, devbcm_vfeed)
ROW(r_b13, devbcm_warmswap)
ROW(r_b5, devbcm_window)
ROW(r_cr1, devbcm_commit)
ROW(r_rstat, devbcm_boot_status)

/* A bench step's own line: kept in RAM with the step's lines (the hook)
   and written by record(), after the step: nothing is written mid-step */
static void say(const char *line)
{
    hook(line);
    if (B.nsaid < BENCH_SAID)
        strlcpy(B.said[B.nsaid++], line, BENCH_KEEP_LEN);
}

/* Why this start can't run the merged image's checks (no display driver,
 * no jobs), or NULL; *restart: a restart may fix it */
static const char *env_why(bool *restart)
{
    *restart = false;
#ifdef HAVE_BCMFW_RESIDENT
    *restart = true;
    if (!bcm_boot_resident_active())
        return "the display program isn't running in this start";
#endif
#if defined(HAVE_BCM_JOBS) || defined(DEVBENCH_SIM)
    return bcmjob_off_why(restart);
#else
    return NULL;
#endif
}

/* The RTC's seconds now, and when this start began by it */
static unsigned long wall_now(void)
{
    return (unsigned long)mktime(get_time());
}

static unsigned long wall_start(void)
{
    return wall_now() - (unsigned long)(current_tick / HZ);
}

static int s_stages05(void)
{
    unsigned want = (BCMDT_STAGE(5) - 1) | BCMDT_STAGE(BCMDT_STAGE_5);
    unsigned got = devbcm_bench_stages(want);

    return (got & want) == want ? BENCH_PASS : BENCH_FAIL;
}

static int s_stage4b(void)
{
    const char *why = "";
    unsigned got;

    if (!bcm_devtest_4b_allowed(&why))
    {
        char line[96];

        snprintf(line, sizeof(line), "Not available: %s", why);
        say(line);
        return BENCH_SKIP;
    }
    got = devbcm_bench_stages(BCMDT_STAGE(0) | BCMDT_STAGE(BCMDT_STAGE_4B));
    return got & BCMDT_STAGE(BCMDT_STAGE_4B) ? BENCH_PASS : BENCH_FAIL;
}

/* Stage 4c, the 150 MHz clock with the screen on (devbcm_test4c's run,
   without its confirm): on a 4b flag naming the embedded 150 MHz bench;
   a pass writes the 4c flag, which with 4b's lets the video player run
   the chip at 150 MHz (bcm_devtest_fast_clock_allowed).  The owner's
   answer judges the pictures: anything but "Yes" removes the flag
   (undo()) */
static int s_stage4c(void)
{
    const char *why = "";
    unsigned got;

    if (!bcm_devtest_4c_allowed(&why))
    {
        char line[96];

        snprintf(line, sizeof(line), "Not available: %s", why);
        say(line);
        return BENCH_SKIP;
    }
    got = devbcm_bench_stages(BCMDT_STAGE(0) | BCMDT_STAGE(1) |
                              BCMDT_STAGE(BCMDT_STAGE_4C));
    return got & BCMDT_STAGE(BCMDT_STAGE_4C) ? BENCH_PASS : BENCH_FAIL;
}

/* The re-verify run's pre-flight: the stage 5 pass flag the BCM rows
   need (bcm_devtest_5i_allowed, as devbcm_lab_open and B13 check it),
   and this start's display driver: our merged image, jobs allowed (what
   jobs and jvpu need).  Flags missing: a skip, and the run offers the
   watched stages 0-5 (s05), then the restart (boot).  Flags there but no
   display driver: done, and the restart runs (bench 2026-10-26: pre
   passed on the flags alone in a start an earlier session's s05 had left
   on Apple's program, and jobs could not run) */
static int s_pre5(void)
{
    const char *why = "";
    bool restart;
    char line[BENCH_KEEP_LEN];

    /* both flags must name this build's images: s05 writes them (stage 4
       is 4b's precondition, so a stale stage 4 flag would leave s4b, s4c
       and stage 6's 150 MHz clock unearned) */
    if (!bcm_devtest_5i_allowed(&why) || !bcm_devtest_4b_allowed(&why))
    {
        snprintf(line, sizeof(line), "pre: Not available: %s", why);
        say(line);
        return BENCH_SKIP;
    }
    why = env_why(&restart);
    if (!why)
    {
        say("pre: stage 4 and 5 pass flags and the display driver: PASS");
        return BENCH_PASS;
    }
    snprintf(line, sizeof(line), "pre: stage 4 and 5 pass flags there; a "
             "restart comes first: %s", why);
    say(line);
    strlcpy(B.note, line, sizeof(B.note));
    return BENCH_DONE;
}

static int s_jobs(void)
{
#if defined(HAVE_BCM_JOBS) || defined(DEVBENCH_SIM)
    int r = bcmjob_selftest();
    char line[BENCH_KEEP_LEN];

    if (r == 1)
    {
        /* unavailable: the reason on screen and in the note, and the
           restart offered (offer_retry) when one may fix it */
        const char *why = env_why(&B.restart);

        snprintf(line, sizeof(line), "jobs: Not available (self-test rc 1): "
                 "%s", why ? why : "the job driver says no");
        say(line);
        return BENCH_SKIP;
    }
    snprintf(line, sizeof(line), "jobs: self-test rc %d", r);
    say(line);
    return r == 0 ? BENCH_PASS : BENCH_FAIL;
#else
    say("jobs: no job driver in this build");
    return BENCH_SKIP;
#endif
}

/* J_VPU, read from the running image (vplayer_peek's pre(): port reads
   of whatever runs; the image keeps running).  Sitting 4c read "no probe
   image" here: the probe image this used to boot after the reads could
   not get its 213 KB while the music sat paused, and nothing else of it
   was used */
struct vpu_ctx
{
    uint32_t block, version, vpu, ops;
    /* batch 4 (jobs.h J_VPU2, J_LAT_*): information, never the verdict */
    uint32_t vpu2, fail, got, exp, lat_sd, lat_sr, lat_mhz;
    int rc;
};

static int vpu_pre(void *ctx)
{
    struct vpu_ctx *c = ctx;

    c->rc = -1;
    if (bcm_video_rd(HP_JOBS, &c->block) < 0)
        return 0;
    c->rc = 1;                      /* no job block */
    if (!c->block || (c->block & 3) || c->block >= 0x140000)
        return 0;
    if (bcm_video_rd(c->block + J_VERSION, &c->version) < 0 ||
        bcm_video_rd(c->block + J_VPU, &c->vpu) < 0 ||
        bcm_video_rd(c->block + J_OPS, &c->ops) < 0)
    {
        c->rc = -1;
        return 0;
    }
    /* batch 4's words (0 in older images; a failed read leaves them 0) */
    if (bcm_video_rd(c->block + J_VPU2, &c->vpu2) < 0 ||
        bcm_video_rd(c->block + J_VPU2_FAIL, &c->fail) < 0 ||
        bcm_video_rd(c->block + J_VPU2_GOT, &c->got) < 0 ||
        bcm_video_rd(c->block + J_VPU2_EXP, &c->exp) < 0 ||
        bcm_video_rd(c->block + J_LAT_SDRAM, &c->lat_sd) < 0 ||
        bcm_video_rd(c->block + J_LAT_SRAM, &c->lat_sr) < 0 ||
        bcm_video_rd(c->block + J_LAT_MHZ, &c->lat_mhz) < 0)
        c->vpu2 = c->lat_mhz = 0;
    c->rc = c->version == J_VERSION_WORD ? 0 : 2;
    return 0;
}

/* Batch 4's lines (bench_ingest.py "vector forms and load timings"):
   the merged image's form checks and its first OPEN's load timings.
   Information only, like the x15 rows: s_jvpu's verdict ignores them, so
   the words avoid the verdict words (bench_classify) and "J_VPU" (the
   pinned reading) */
static const char *const vpu2_forms[J_VPU2_FORMS + 1] = {
    "", "BSL1", "BSF1", "BSM1", "BSR1", "BSX1", "BSS1", "BSZ1"
};

static void jvpu_batch4(const struct vpu_ctx *c)
{
    char line[BENCH_KEEP_LEN];
    int f, n;

    if (!(c->vpu2 & J_VPU2_CHECKED))
        say("jvpu: forms not checked by this image (vpu2 0)");
    else
    {
        n = snprintf(line, sizeof(line), "jvpu: forms");
        for (f = 1; f <= J_VPU2_FORMS && n < (int)sizeof(line); f++)
            n += snprintf(line + n, sizeof(line) - n, " %s %s", vpu2_forms[f],
                          c->vpu2 & J_VPU2_BIT(f) ? "ok" : "differs");
        if (n < (int)sizeof(line))
            snprintf(line + n, sizeof(line) - n, " (vpu2 %lx)",
                     (unsigned long)c->vpu2);
        say(line);
        f = c->fail & 0xFF;
        if (f >= 1 && f <= J_VPU2_FORMS)
        {
            snprintf(line, sizeof(line), "jvpu: forms: first differing %s lane %lu "
                     "got %04lx exp %04lx", vpu2_forms[f],
                     (unsigned long)(c->fail >> 8 & 0xFF), (unsigned long)c->got,
                     (unsigned long)c->exp);
            say(line);
        }
    }
    if (!(c->lat_mhz & J_LAT_DONE))
        say("jvpu: load timings not measured yet (no OPEN since the image started)");
    else
    {
        /* ns x 10 a load, bytes low and words high */
        snprintf(line, sizeof(line), "jvpu: sdram byte %lu.%lu ns word %lu.%lu ns; "
                 "sram byte %lu.%lu ns word %lu.%lu ns at %lu MHz",
                 (unsigned long)(c->lat_sd & 0xFFFF) / 10,
                 (unsigned long)(c->lat_sd & 0xFFFF) % 10,
                 (unsigned long)(c->lat_sd >> 16) / 10,
                 (unsigned long)(c->lat_sd >> 16) % 10,
                 (unsigned long)(c->lat_sr & 0xFFFF) / 10,
                 (unsigned long)(c->lat_sr & 0xFFFF) % 10,
                 (unsigned long)(c->lat_sr >> 16) / 10,
                 (unsigned long)(c->lat_sr >> 16) % 10,
                 (unsigned long)(c->lat_mhz & 0xFFFF));
        say(line);
    }
}

static int s_jvpu(void)
{
    struct vpu_ctx c = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1 };
    uint32_t want;
    char line[120];
    int rc = vplayer_peek(vpu_pre, &c);

    if (rc < 0)
    {
        snprintf(line, sizeof(line), "jvpu: Not available: the video chip "
                 "is %s", rc == -1 ? "playing" : "not free");
        say(line);
        return BENCH_SKIP;
    }
    if (c.rc)
    {
        /* no job block: whatever ran was not our image (Apple's program
           after a device test): a restart brings the display driver */
        B.restart = c.rc == 1;
        snprintf(line, sizeof(line), "jvpu: %s (block %08lx)",
                 c.rc < 0 ? "port read failed" : c.rc == 1
                 ? "Not available: no job block: the resident image isn't "
                   "running"
                 : "no job block version", (unsigned long)c.block);
        say(line);
        return c.rc < 0 ? BENCH_FAIL : BENCH_SKIP;
    }
    /* each image's own value (devbench.h): the merged image 3 (checked,
       shift), an image that decodes (the display-only one) 7; B5 logs the
       display-only image's own reading (lab5: J_VPU) */
    want = bench_vpu_expect(c.ops);
    snprintf(line, sizeof(line), "jvpu: J_VPU %lx, expected %lx (%s image; "
             "checked %d, shift %d, MAC %d) at block %08lx",
             (unsigned long)c.vpu, (unsigned long)want,
             want & J_VPU_MAC ? "decoding" : "merged",
             !!(c.vpu & J_VPU_CHECKED), !!(c.vpu & J_VPU_SHIFT),
             !!(c.vpu & J_VPU_MAC), (unsigned long)c.block);
    say(line);
    jvpu_batch4(&c);            /* information: not in the verdict */
    return bench_vpu_verdict(c.vpu, c.ops);
}

static int s_port(void)
{
    say("port: measured by stage 6's play (part B): it needs eyes on the clip");
    return BENCH_SKIP;
}

static int s_boot(void)
{
    devbcm_boot_dma();
    return BENCH_DONE;
}

/* ---- The table ---------------------------------------------------------
 * Order is the run order.  BENCH_F_AUTO steps are part A (or run hands
 * off in part B, after the restart).  Other sessions add entries here,
 * under their own feature flags; an entry without a function is manual. */
#define BA  BENCH_F_AUTO
#define BC  BENCH_F_COP
#define OPT (BENCH_F_OPTIONAL | BENCH_F_OWN)

#if defined(HAVE_BCM_JOBS) || defined(DEVBENCH_SIM)
/* Drill 2 (bench-p19.md, part B): the next two FADE replies are read as
 * failed through the driver's real reply path (bcmjob_dev_inject_fail),
 * so the second counted failure marks FADE for the boot and the drift
 * must fall back (the still cover or the CPU's drift); the owner then
 * says what showed */
static int s_drill2(void)
{
    char line[80];
    int rc = bcmjob_dev_inject_fail(J_OP_FADE, 2);

    snprintf(line, sizeof(line), "drill2: inject op=0x%02x n=2 rc=%d",
             J_OP_FADE, rc);
    say(line);
    if (rc < 0)
    {
        say("drill2: Not available: no job driver on this thread");
        return BENCH_SKIP;
    }
    return BENCH_PASS;
}
#endif

/* The owner's script lives in this table: each step's start screen (say,
 * say2) and question (ask), word-wrapped on the device (bench_next_line)
 * and copied into bench-p19.md's "Owner's script" by
 * scripts/bench_script.py, which tests/bench_script checks against this
 * file.  Every start screen ends with a SELECT line (added by
 * start_screen()); every question with ASK_HINT.  A step that may flash,
 * blank, show test pictures or restart opens with a one-line heads-up
 * saying that's normal.  Say what the buttons do during the step, how
 * long it takes, what is normal, and what each answer means (2 to 4, the
 * first the normal one).  Plain words: no step codes on screen. */
#if defined(HAVE_BCMFW_RESIDENT) && defined(HAVE_PLL_IDLE)
/* lit24ui: the lit clock at 24 MHz for the owner's look at the menus;
   the answer puts it back (after()); a restart does too (not saved) */
static int s_lit24ui(void)
{
#ifdef HAVE_IPOD_UI_DEVMODE
    /* phase A at 30 MHz; the first frame after LITMISS_A_S sets 24 (B) */
    litmiss_begin();
    say("lit24ui: counting missed frames, 30 MHz then 24");
#else
    clockpol_lit24_set(true);
    say("lit24ui: lit clock 24 MHz for the menus");
#endif
    return BENCH_PASS;
}

#ifdef HAVE_IPOD_UI_DEVMODE
/* On the owner's return, before the question: stop counting, log both
 * phases and judge them (devbench_power.h bpw_miss_*).  The verdict
 * joins the owner's answer. */
static int lit24ui_end(void)
{
    const struct litmiss *m = &litmiss;
    char line[100];
    int j;

    litmiss_end();
    for (j = 0; j < 2; j++)
    {
        snprintf(line, sizeof(line), "lit24ui %s drift_miss=%lu "
                 "scroll_miss=%lu frames=%lu/%lu", j ? "B24" : "A30",
                 (unsigned long)m->drift_miss[j],
                 (unsigned long)m->scroll_miss[j],
                 (unsigned long)m->drift_frames[j],
                 (unsigned long)m->scroll_frames[j]);
        say(line);
    }
    if (!bpw_miss_enough(m->drift_frames[0], m->scroll_frames[0]) ||
        !bpw_miss_enough(m->drift_frames[1], m->scroll_frames[1]))
    {
        say("lit24ui: FAIL: too few frames to judge (let the drift run in "
            "the main menu and scroll Songs in each minute)");
        return BENCH_FAIL;
    }
    if (!bpw_miss_kind_ok(m->drift_miss[0], m->drift_frames[0],
                          m->drift_miss[1], m->drift_frames[1]) ||
        !bpw_miss_kind_ok(m->scroll_miss[0], m->scroll_frames[0],
                          m->scroll_miss[1], m->scroll_frames[1]))
    {
        say("lit24ui: FAIL: more frames missed at 24 MHz than at 30");
        return BENCH_FAIL;
    }
    say("lit24ui: PASS (no more frames missed at 24 MHz than at 30)");
    return BENCH_PASS;
}
#endif
#endif

#ifdef HAVE_UI_THREAD_SPLIT
/* P19 19-9: the UI on the COP (docs/design/p19-ui-on-cop.md "As built").
 * ucop sets the switch for the next start; the owner restarts; ucop2
 * starts the soak and leaves; on the owner's return ucop2_end() judges
 * the boundary's counters (fe_be.h) over the soak. */
static struct fe_be_stats ucop_base;
static long ucop_t0;
static unsigned long ucop_under0;   /* the PCM underruns at the start */

/* the PCM's underruns so far (pcmbuf.c counts them in the PCM callback, on
 * the CPU: a remote call from the COP, febe_rpc.list) */
static unsigned long ucop_underruns(void)
{
#ifdef HAVE_BCMFW_VIDEO
    return pcmbuf_underruns();
#else
    return 0;
#endif
}

static int s_ucop(void)
{
    fe_be_mode_set_next(FE_COP);
    say("ucop: the UI thread switch is cop for the next start");
    return BENCH_PASS;
}

static int s_ucop2(void)
{
    char line[64];

    if (fe_be_mode() != FE_COP)
    {
        snprintf(line, sizeof(line), "ucop2: FAIL: the UI runs %s, not on "
                 "the COP (refused or fell back: fe lines)",
                 fe_be_mode_name(fe_be_mode()));
        say(line);
        return BENCH_FAIL;
    }
    fe_be_stats_get(&ucop_base);
    ucop_under0 = ucop_underruns();
    ucop_t0 = current_tick;
    say("ucop2: the soak starts, the UI on the COP");
    return BENCH_PASS;
}

#ifndef UCOP_MIN_S              /* devbench_sim_calls.h: the sim's soak */
#define UCOP_MIN_S      100         /* the soak ran long enough to judge */
#endif
#define UCOP_CALL_MS    250         /* a call to the back end this slow
                                       fails, unless storage powered up on
                                       the way (sitting 3: 0.55-0.61 s on
                                       the flash unit's power-on, as slow
                                       inline; reported apart) */

static int ucop2_end(void)
{
    struct fe_be_stats st;
    char line[100];
    long secs;
    unsigned long under;

    if (fe_be_mode() != FE_COP || !ucop_t0)
        return BENCH_FAIL;
    fe_be_stats_get(&st);
    be_post(BE_RCALL_REPORT, 0);    /* the profile, to the log (be_svc) */
    secs = (current_tick - ucop_t0) / HZ;
    snprintf(line, sizeof(line), "ucop2 s=%ld calls=%lu rcalls=%lu "
             "max_us=%lu stalls=%lu coh=%lu fbto=%lu", secs,
             (unsigned long)(st.calls - ucop_base.calls),
             (unsigned long)(st.rcalls - ucop_base.rcalls),
             (unsigned long)st.call_max_us,
             (unsigned long)(st.stalls - ucop_base.stalls),
             (unsigned long)(st.coh_fails - ucop_base.coh_fails),
             (unsigned long)(st.fb_timeouts - ucop_base.fb_timeouts));
    say(line);
    under = ucop_underruns() - ucop_under0;
    snprintf(line, sizeof(line), "ucop2 nostor_max_us=%lu stor_calls=%lu "
             "offcore=%lu post_drops=%lu underruns=%lu",
             (unsigned long)st.call_max_nostor_us,
             (unsigned long)(st.stor_calls - ucop_base.stor_calls),
             (unsigned long)(st.offcore - ucop_base.offcore),
             (unsigned long)(st.post_drops - ucop_base.post_drops), under);
    say(line);
    if (secs < UCOP_MIN_S)
    {
        say("ucop2: FAIL: under 100 s of use to judge");
        return BENCH_FAIL;
    }
    if (st.stalls != ucop_base.stalls || st.coh_fails != ucop_base.coh_fails
        || st.fb_timeouts != ucop_base.fb_timeouts)
    {
        say("ucop2: FAIL: a stall, a coherence failure or a pixel fence "
            "timeout");
        return BENCH_FAIL;
    }
    if (st.offcore != ucop_base.offcore)
    {
        say("ucop2: FAIL: CPU-only code ran on the COP (offcore)");
        return BENCH_FAIL;
    }
    if (under)
    {
        /* the codec ran dry: a back-end call held the CPU too long */
        say("ucop2: FAIL: the PCM underran during the soak");
        return BENCH_FAIL;
    }
    if (st.call_max_nostor_us > UCOP_CALL_MS * 1000ul)
    {
        say("ucop2: FAIL: a call to the back end took over 250 ms "
            "(no storage power-up in it)");
        return BENCH_FAIL;
    }
    say("ucop2: PASS (no stall, no coherence failure, no off-core call, "
        "no PCM underrun, calls under 250 ms without a storage power-up; "
        "uilat lines for the latency)");
    return BENCH_PASS;
}
#endif

/* vplay (sitting 4d): this unit already has a check result (sitting 4c's
   play wrote vcheck.tok), so the step drops it before the owner's play:
   the play outside then runs the ~0.9 s first-use check for real, and
   its vck line (verdict and time) is in the log; finish() puts the
   verdict on the step's result line */
static int s_vplay(void)
{
    int rc = vplayer_check_reset();

    say(rc > 0 ? "vplay: this unit's check result dropped: the next play "
        "checks first" : rc ? "vplay: the check result could not be "
        "dropped: the play may not check" : "vplay: no check result "
        "stored: the next play checks first");
    return rc < 0 ? BENCH_FAIL : BENCH_DONE;
}

static const struct bench_step steps[] =
{
    /* ---- the re-verify run's pre-flight (never in the full run) ---- */
    { "pre", "Start-up check", NULL, NULL, s_pre5, NULL, { NULL },
      BENCH_F_PICKED, 0, 5 },
    /* ---- part A ---- */
    /* stages 2 and 3 pass on counts and mailboxes, not on the picture
       (bcm-devtest-core.c's PHOTO lines): the owner judges it */
    { "s05", "Test pictures",
      "The screen will flash and show test pictures for about 5 min: "
      "that's normal.\n\n"
      "Colour bars, red, green and blue ramps, a letter F at the bottom "
      "left, a magenta box. One picture scrolls: its top bar stays still, "
      "except the clock box. The screen may go dark between pictures.",
      "Buttons do nothing until the question. Normal: steady, clean "
      "colours; nothing torn, shifted or speckled.", s_stages05,
      "Did the test pictures look right?\n"
      "Yes: clean and steady.\n"
      "Wrong colours: a colour is off.\n"
      "Garbled: torn, shifted or speckled.\n"
      "Blank: nothing showed.",
      { "Yes", "Wrong colours", "Garbled", "Blank" }, 0, 0, 300 },
    { "s4b", "Fast clock check", NULL, NULL, s_stage4b, NULL, { NULL }, BA,
      180, 0 },
    /* the 150 MHz clock with the screen on: with 4b's, its flag lets the
       video player run the chip at 150 MHz (bcm_devtest_fast_clock_allowed) */
    { "s4c", "Fast clock pictures",
      "The screen will flash and show test pictures for about 1 min: "
      "that's normal.\n\n"
      "The video chip runs at its fast clock with the screen on. Three "
      "pictures, 8 s each:\n"
      "1. colour bars, ramps, an F;\n"
      "2. the same with a magenta box at the top left;\n"
      "3. back at the normal clock, no box.\n"
      "Then the normal screen returns.",
      "Buttons do nothing until the question. Normal: all three as clean "
      "as the earlier test pictures.", s_stage4c,
      "Did the 3 pictures look right?\n"
      "Yes: clean. Videos may then use the fast clock.\n"
      "Wrong colours: a colour is off.\n"
      "Garbled: speckles, streaks or shifted lines.\n"
      "Blank: nothing showed.\n"
      "Any answer but Yes keeps videos at the normal clock.",
      { "Yes", "Wrong colours", "Garbled", "Blank" }, 0, 0, 60 },
    { "c1", "Second core check", NULL, NULL, r_c1, NULL, { NULL }, BA | BC,
      90, 0 },
    { "c2", "COP row C2", NULL, NULL, r_c2, NULL, { NULL }, BA | BC, 60, 0 },
    { "c3", "COP row C3", NULL, NULL, r_c3, NULL, { NULL }, BA | BC, 60, 0 },
    { "pb1", "Probe PB-1", NULL, NULL, r_pb1, NULL, { NULL }, BA, 90, 0 },
    { "pb4", "Probe PB-4", NULL, NULL, r_pb4, NULL, { NULL }, BA, 90, 0 },
    { "pw1", "Probe PW1", NULL, NULL, r_pw1, NULL, { NULL }, BA, 90, 0 },
    { "pw2", "Probe PW2", NULL, NULL, r_pw2, NULL, { NULL }, BA, 90, 0 },
    { "pw7", "Probe PW7", NULL, NULL, r_pw7, NULL, { NULL }, BA, 90, 0 },
    { "pw8", "Probe PW8", NULL, NULL, r_pw8, NULL, { NULL }, BA, 90, 0 },
    { "jobs", "Chip self-test", NULL, NULL, s_jobs, NULL, { NULL }, BA, 30,
      0 },
    { "jvpu", "Chip maths check", NULL, NULL, s_jvpu, NULL, { NULL }, BA, 30,
      0 },
    { "port", "Port speed", NULL, NULL, s_port, NULL, { NULL }, BA, 10, 0 },
    /* ---- part B ---- */
    { "pb2", "Probe PB-2",
      "About 5 min. Reads the video chip's pins in 7 states. Have the "
      "USB-C cable, the AV cable and headphones at hand.",
      "7 prompts follow, one per state (cables, music, Hold). Do what each "
      "says, then scroll to its last line and press SELECT. MENU on a "
      "prompt ends this probe early; the session goes on. No question at "
      "the end.", r_pb2, NULL, { NULL }, 0, 0, 300 },
    { "pb3", "Probe PB-3",
      "About 1 min. Reads the video chip's USB status bit with the USB-C "
      "cable in, then out (a charger or a computer; the iPod only "
      "charges).",
      "2 prompts follow: plug in, then unplug. Do it, then scroll to the "
      "prompt's last line and press SELECT. MENU on a prompt ends this "
      "probe early; the session goes on. No question.", r_pb3, NULL,
      { NULL }, 0, 0, 60 },
    { "s6", "Test clips",
      "The screen will go dark, then test clips play for about 1 min: "
      "that's normal. Music stops.\n\n"
      "Clip 1 plays twice: colour bars, a counter 0 to 7 and a steady tone "
      "(8 s). Then clip 2: 13 s of real film with fast pans and cuts.",
      "No buttons: it plays to the end by itself. Normal: smooth motion "
      "with no skips, sound in step with the picture.", r_s6,
      "How did the clips play?\n"
      "Smooth, in sync: normal.\n"
      "Stutter: jerky or frozen moments.\n"
      "Sound off picture: out of step.\n"
      "No picture: nothing showed.",
      { "Smooth, in sync", "Stutter", "Sound off picture", "No picture" },
      0, 0, 60 },
    { "s7", "Video controls",
      "About 3 min, hands off. The iPod plays the test video and works "
      "its own controls: pause, jump ahead, back to the start, the screen "
      "dark for 3 s and back, then it closes the video and opens it "
      "again where it was. That's the test.",
      "Then the video restarts six times, 8 s each, to measure settings; "
      "two of those run on purpose at a slow clock and may stutter. "
      "Normal: after each step the picture and sound come back within a "
      "moment.", r_s7,
      "The test closes and reopens the video a few times. Did it come "
      "back and keep playing each time?\n"
      "Yes: every time.\n"
      "Froze or error: it stalled or showed an error.\n"
      "Didn't come back: it stayed closed.",
      { "Yes", "Froze or error", "Didn't come back", NULL }, 0, 0, 180 },
    /* P19 vplayer health: the decoder's pace at the verified clock on a
       demanding clip and a clip in the recommended profile
       (video-profile.md); automatic rule p95 < 75 % of the frame, no
       skips, A/V in bounds, 150 MHz (vtrace_pace_ok) */
    { "vfeed", "Video pace",
      "The screen will go dark, then two videos play for about 1 min: "
      "that's normal. Music stops.\n\n"
      "The 13 s film clip from the test clips, then a 40 s converted clip, "
      "each to the end by itself.",
      "No buttons. Normal: smooth motion, no jumps or frozen moments, "
      "sound in step with the picture in both.", r_vfeed,
      "How did the two videos play?\n"
      "Smooth: steady motion, in sync.\n"
      "Stutter: jerky, jumps or freezes.\n"
      "Sound off picture: out of step.\n"
      "No picture: nothing showed.",
      { "Smooth", "Stutter", "Sound off picture", "No picture" },
      0, 0, 75 },
#ifdef HAVE_BCMFW_VIDEO
    /* P19 PW4 (bench-p19.md "The clock rows"): the chip's level bands,
       then its governor against fixed 150 MHz; performance and viability
       only, skipped by an image without the bands */
    { "vbv", "Clock bands",
      "The screen will go dark, then a clip plays 11 times over about "
      "2 min: that's normal. Music stops.\n\n"
      "Clip 1 plays 8 s at each of the video chip's clock bands, slowest "
      "first. At the slow ones the picture may stutter or freeze: "
      "expected.",
      "No buttons. Normal: every play shows the bars and the counter "
      "moving, no garbled picture.", devbench_vclk_verify,
      "Did any play show a garbled picture?\n"
      "No: normal.\n"
      "Yes, garbled: broken or wrong picture.\n"
      "No picture at all: a play stayed blank.",
      { "No", "Yes, garbled", "No picture at all", NULL }, 0, 0, 120 },
    { "vgov", "Clock governor",
      "The screen will go dark, then clips play for about 3 min: that's "
      "normal. Music stops.\n\n"
      "Each clip plays three times for 20 s: at full clock, then with the "
      "clock following the load two ways. Every play jumps back once at "
      "5 s and pauses for 1 s at 10 s.",
      "No buttons. Normal: all plays of a clip look and sound the same: "
      "smooth, in step, a short jump and a short pause.", devbench_vclk_gov,
      "Did the clips keep playing smoothly, after each jump and pause "
      "too?\n"
      "Yes: smooth throughout.\n"
      "Stutter: jerky or skipping at times.\n"
      "Sound off picture: out of step.\n"
      "No picture: a play stayed blank.",
      { "Yes", "Stutter", "Sound off picture", "No picture" }, 0, 0, 200 },
#endif
    /* sitting 4c: one normal play from Videos, outside the bench (the
       session has left, so vplayer_bench_play() is off): the first-use
       check's "Preparing Video..." screen, then the play.  Re-verify only
       (devbench.h's picks: after the video steps, needing s6 and s4c) */
    { "vplay", "Play a video",
      "Done outside the session, under 1 min. From the main menu open "
      "Videos and play any video. \"Preparing Video...\" shows for about "
      "a second first: that's normal (a one-off check, run again for "
      "this test).",
      "SELECT here leaves the session. Then open Bench session again and "
      "pick Go on: Play a video; the question comes first.", s_vplay,
      "Did \"Preparing Video...\" show briefly, then the video play?\n"
      "Yes: briefly, then it played.\n"
      "No message: it just played.\n"
      "Stuck: it stayed on that screen.\n"
      "No video: it didn't play.",
      { "Yes", "No message", "Stuck", "No video" },
      BENCH_F_PICKED | BENCH_F_LEAVES, 0, 60 },
    { "b13", "Program swap",
      "The screen will go dark once at the end, under 1 min from now: "
      "that's normal.\n\n"
      "The video chip swaps between its video and display programs up to "
      "20 times (10 each way) with the screen on, and stops at the first "
      "swap that fails. Each swap back adds one small square, green then "
      "red in turn, a step to the right near the bottom. No video plays.",
      "No buttons. Normal: no blink, blank or flicker before that last "
      "dark moment.", r_b13,
      "Did it blink, blank or flicker before the last dark moment?\n"
      "No: normal.\n"
      "Yes, briefly: once, short.\n"
      "Yes, badly: often or long.",
      { "No", "Yes, briefly", "Yes, badly", NULL }, 0, 0, 60 },
    { "b5", "Window test",
      "The screen will flash for under 1 min: that's normal.\n\n"
      "Windows flash by in the same spot (one picture sent 31 times from "
      "three differently laid-out copies), then the last stays 5 s: a "
      "white frame and a white diagonal over red and blue stripes.",
      "No buttons. Normal: the flashes, then that last picture, the frame "
      "straight and whole, the diagonal unbroken.", r_b5,
      "Was the white frame straight and whole?\n"
      "Yes: normal.\n"
      "Sheared: slanted or shifted rows.\n"
      "Broken: gaps or garbage.",
      { "Yes", "Sheared", "Broken", NULL }, 0, 0, 45 },
    /* open question #67: the port's writes read back, then COMMIT, on the
       display-only image (as b5 boots it); read-only, hands off */
    { "cr1", "COMMIT readback", NULL, NULL, r_cr1, NULL, { NULL }, BA, 120,
      0 },
    { "boot", "Display program",
      "The iPod will restart once: that's normal.\n\n"
      "A few seconds: our video program drives the screen from the next "
      "start on (it stays, on purpose). Then you restart the iPod.",
      "After it: hold MENU and SELECT together 6 to 8 s. After the "
      "restart: Developer > Bench session, then pick Go on: "
      "Display program.", s_boot, NULL, { NULL }, BENCH_F_REBOOTS, 0, 60 },
    { "rstat", "Driver status", NULL, NULL, r_rstat, NULL, { NULL }, BA, 30,
      0 },
    { "jobs2", "Chip self-test", NULL, NULL, s_jobs, NULL, { NULL }, BA, 30,
      0 },
    { "jvpu2", "Chip maths check", NULL, NULL, s_jvpu, NULL, { NULL }, BA,
      30, 0 },
#ifdef HAVE_BCMFW_RESIDENT
    /* p19-battery: the battery levers as viability checks (bench-p19.md
       "The battery rows"): does the lever work, does it keep things
       smooth; no meter, no power claims (owner, 2026-10-25).
       devbench_power.c draws the phases; a row that could not run asks
       nothing */
    { "slp", "Chip idle sleep",
      "About 80 s, on battery (unplug the iPod). 4 phases of 20 s, A B A "
      "B. A: the video chip polls (today). B: it sleeps between ticks. "
      "The screen stays still and shows the phase. Leave it alone until "
      "the question appears.",
      "It checks itself: in B the chip wakes on every tick, sleeps, and "
      "still answers. Through B the iPod also rings the chip's doorbell at "
      "random moments and counts them; a lost one fails the row.\n"
      "MENU in a phase: ends the row early (skipped, no question).",
      devbench_pwr_idle,
      "Did the screen stay right?\n"
      "Yes: still and clean.\n"
      "Garbled: noise or wrong pixels.\n"
      "Froze: stuck after the row.",
      { "Yes", "Garbled", "Froze", NULL }, 0, 0, 100 },
    { "lvl5", "Chip core level 5",
      "About 80 s after a go question, on battery. 4 phases of 20 s, A B "
      "A B. A: the video chip's core at level 11 (today). B: level 5. "
      "The screen stays still. Leave it alone until the question "
      "appears.",
      "It checks itself: level 5 holds and the job self-test passes at "
      "it.\n"
      "MENU in a phase: ends the row early (skipped, no question).",
      devbench_pwr_level,
      "Did the screen stay right?\n"
      "Yes: still and clean.\n"
      "Garbled: noise or wrong pixels.\n"
      "Froze: stuck after the row.",
      { "Yes", "Garbled", "Froze", NULL }, 0, 0, 110 },
    { "lit24", "Lit clock 24 MHz",
      "Logs only, about 80 s, on battery (USB keeps the clock at 30 MHz). "
      "4 phases of 20 s, A B A B: the iPod's clock at 30 MHz (today), "
      "then 24 MHz. The screen stays still; leave it alone. No question.",
      "MENU in a phase: ends the row early (skipped).",
      devbench_pwr_lit, NULL, { NULL }, 0, 0, 100 },
#ifdef HAVE_PLL_IDLE
    /* lit24's smoothness: the menus at 24 MHz, judged by eye (the row
       above only proves the clock gets there) */
    { "lit24ui", "Menus at 24 MHz",
      "About 3 min. SELECT leaves the session and starts counting missed "
      "frames: 1 min at 30 MHz, then 1 min at 24 (the clock changes by "
      "itself; nothing shows). In each minute watch the covers drift in "
      "the main menu for 30 s, then open Songs and turn the wheel fast to "
      "the bottom and back.",
      "Then open Bench session again and pick Go on: Menus at 24 MHz; "
      "the question comes first and puts the clock back to 30. It fails "
      "if 24 MHz misses more frames than 30. Normal: as smooth as ever.",
      s_lit24ui,
      "Did the menus stay smooth at 24 MHz?\n"
      "Yes: as smooth as ever.\n"
      "Drift stuttered: jerky covers.\n"
      "Scroll lagged: the list fell behind the wheel.\n"
      "Hung: stuck; you restarted it.",
      { "Yes", "Drift stuttered", "Scroll lagged", "Hung" },
      BENCH_F_LEAVES, 0, 150 },
#endif
#endif
#ifdef HAVE_UI_THREAD_SPLIT
    /* P19 19-9 (bench-p19.md "The UI on the COP"): after c1 (engine 2
       started from the COP) passed; one restart, then a 2 min soak */
    { "ucop", "Screens on second core",
      "The iPod will restart once: that's normal.\n\n"
      "About 4 min in all. The iPod's screens and menus will run on its "
      "second core from the next start. SELECT sets that; then you "
      "restart.",
      "After it: hold MENU and SELECT together 6 to 8 s. After the "
      "restart: Developer > Bench session, then pick Go on: Clicks, "
      "music stopped.", s_ucop, NULL, { NULL }, BENCH_F_REBOOTS, 0, 30 },
    /* p19-ucop-fix's rows (docs/design/p19-ui-on-cop.md, "Bench rows for
       sitting 4"): the UI on the COP since ucop's restart, Clicker on */
    { "uck1", "Clicks, music stopped",
      "Done outside the session, under 1 min. With Settings > Clicker on "
      "and no music playing: in Music > Songs scroll up and down for 20 s, "
      "then stop and listen for 5 s.",
      "SELECT here leaves the session. Then open Bench session again and "
      "pick Go on: Clicks, music stopped; the question comes first. "
      "Normal: only the clicks, then silence.", NULL,
      "Besides the clicks, did you hear a tone or hum?\n"
      "No: only clicks, then quiet.\n"
      "Faint: a quiet tone or hum.\n"
      "Clear: an easy-to-hear tone or hum.",
      { "No", "Faint", "Clear", NULL }, 0, 0, 60 },
    { "uck2", "Clicks with music",
      "Done outside the session, about 1 min. Play a song from Music > "
      "Songs, press MENU back to the main menu, and scroll up and down "
      "for 20 s while it plays.",
      "SELECT here leaves the session. Then open Bench session again and "
      "pick Go on: Clicks with music; the question comes first. Normal: "
      "the music plays on cleanly under the clicks.", NULL,
      "Did the music glitch, pop or crackle while you scrolled?\n"
      "Never: clean the whole time.\n"
      "Once or twice: a rare glitch.\n"
      "Often: glitches again and again.",
      { "Never", "Once or twice", "Often", NULL }, 0, 0, 60 },
    { "ufeel", "Scrolling feel",
      "Done outside the session, under 1 min. Scroll Music > Songs fast, "
      "up and down, then open the Developer menu and scroll it.",
      "SELECT here leaves the session. Then open Bench session again and "
      "pick Go on: Scrolling feel; the question comes first. Compare it "
      "with how the iPod normally feels.", NULL,
      "Compared with the normal UI, scrolling felt:\n"
      "Same or faster: as quick, or quicker.\n"
      "A little slower: a slight lag.\n"
      "Much slower: clearly laggy.",
      { "Same or faster", "A little slower", "Much slower", NULL },
      0, 0, 60 },
    { "upanel", "Panel keeping up",
      "Done outside the session, under 1 min. With music playing, go to "
      "the main menu and spin the wheel fast up and down for 10 s.",
      "SELECT here leaves the session. Then open Bench session again and "
      "pick Go on: Panel keeping up; the question comes first. Normal: "
      "the right-hand panel follows the highlighted row.", NULL,
      "Did the right-hand panel keep up with the highlighted row?\n"
      "Kept up: always the right picture.\n"
      "A little behind: caught up within a moment.\n"
      "Far behind: lagged well behind or stuck.",
      { "Kept up", "A little behind", "Far behind", NULL }, 0, 0, 45 },
    { "ucop2", "Second core, everyday use",
      "About 3 min. SELECT leaves the session. For 2 min use the iPod: "
      "browse Music, play an MP3, open Now Playing, turn the wheel fast "
      "through Songs and back.",
      "Then open Bench session again and pick Go on: Second core, everyday "
      "use; the question comes first. It fails on a stall, a coherence error "
      "or a slow call. Normal: as quick as ever. A hang: hold MENU and "
      "SELECT; the next start runs the screens on the main core.",
      s_ucop2,
      "How did the iPod feel on the second core?\n"
      "Normal: quick, nothing odd.\n"
      "Slow: lag or stutter.\n"
      "Glitches: wrong pixels or text.\n"
      "Hung: stuck; you restarted it.",
      { "Normal", "Slow", "Glitches", "Hung" }, BENCH_F_LEAVES, 0, 180 },
#endif
#if defined(HAVE_BCM_JOBS) || defined(DEVBENCH_SIM)
    /* P19 19-2 (bench-p19.md "Session plan", part B): manual until 19-2
       wires its rows; drill 2 arms its failures, then leaves */
    { "b3", "Drift on the chip",
      "Done outside the session, about 3 min. In Developer > Advanced > "
      "Video chip, press BCM jobs: drift on video chip until its screen "
      "says on. Then go to "
      "the main menu and watch the covers drift for 2 min.",
      "SELECT here leaves the session. Then open Bench session again and "
      "pick Go on: Drift on the chip; the question comes first. Normal: a "
      "slow, smooth drift and cross-fades that never stop (a still panel "
      "was the fault this build fixes).", NULL,
      "Did the covers drift smoothly?\n"
      "Yes: smooth.\n"
      "Stutter: jerky.\n"
      "Frozen: they stopped.\n"
      "Blank: no covers.",
      { "Yes", "Stutter", "Frozen", "Blank" }, 0, 0, 180 },
    /* RESPANEL (bench-p19.md): the album's panel decoded on the chip
       (the merged image's DECODE overlay) and kept there; ipodui.log has
       the times (resident show us=, read_ms=) and must have no FAULT */
    /* 0.8.0: the visualizer drawn on the chip in the release path (the
       merged image's viz overlay, viz-ovl-m.bin; visualizer.md 8.5): a
       minute on screen, then a track change (the cover's DECODE takes
       the shared overlay region and the visualizer reloads).  The log
       carries the CPU (uilat cpu) and the swap (bcmjob overlay load) */
    { "uviz", "Visualiser on the chip",
      "Done outside the session, about 2 min. Play a song (not an "
      "audiobook), open Now Playing and press SELECT until the "
      "visualiser fills the screen. Watch it 1 min, then press Next "
      "once and watch 20 s more.",
      "SELECT here leaves the session. Then open Bench session again and "
      "pick Go on: Visualiser on the chip; the question comes first. "
      "Normal: smooth motion, a short pause at the new song, then moving "
      "again.", NULL,
      "How did the visualiser look?\n"
      "Smooth: moving all along, back after the new song.\n"
      "Stuttered: jerky or frozen at times.\n"
      "Stuck after Next: did not move again.\n"
      "Never showed: no visualiser.",
      { "Smooth", "Stuttered", "Stuck after Next", "Never showed" },
      0, 0, 120 },
    { "respanel", "Cover on the chip",
      "Done outside the session, about 2 min. Play an album you have not "
      "played on this build. Rest on the main menu's Now Playing row 20 s, "
      "go to Music 10 s, come back 10 s. Then let the screen go dark and "
      "wake it with MENU.",
      "SELECT here leaves the session. Then open Bench session again and "
      "pick Go on: Cover on the chip; the question comes first. Normal: "
      "the cover appears sharp, stays put, and comes back after the "
      "wake.", NULL,
      "How did the album cover look?\n"
      "Sharp, steady: normal.\n"
      "Blurry or late: soft, or long to come.\n"
      "Wrong picture: garbled or another cover.\n"
      "Missing: no cover.",
      { "Sharp, steady", "Blurry or late", "Wrong picture", "Missing" },
      0, 0, 120 },
    { "b3ab", "Drift A/B",
      "Done outside the session, about 12 min. Developer > Power log: on. "
      "Drift on video chip: on (as before); watch the main menu 5 min. "
      "Then press it until it says off; watch 5 min more.",
      "SELECT here leaves the session. Then open Bench session again and "
      "pick Go on: Drift A/B; the question comes first. Normal: the menu "
      "drifts the whole time.", NULL,
      "Did both halves run to the end?\n"
      "Yes: both.\n"
      "On half failed: it stopped while on.\n"
      "Off half failed: it stopped while off.",
      { "Yes", "On half failed", "Off half failed", NULL }, 0, 0, 720 },
    { "drill1", "Drill: restart",
      "Done outside the session, about 2 min. In Developer > Advanced > "
      "Video chip > Fault injection, press BCM boot path: next wake never "
      "answers. Set the drift on, go to "
      "the main menu, let the screen go dark, then press MENU once to "
      "wake it.",
      "SELECT here leaves the session. Then open Bench session again and "
      "pick Go on: Drill: restart. Normal: no freeze, a dark blink at "
      "most.", NULL,
      "Did the screen recover?\n"
      "Yes: the menu came back.\n"
      "Still cover: a still picture, no drift.\n"
      "Hang: stuck; you restarted it.\n"
      "Reboot: it restarted by itself.",
      { "Yes", "Still cover", "Hang", "Reboot" }, 0, 0, 120 },
    { "drill2", "Drill: job failure",
      "About 2 min. SELECT arms it: the next two cross-fades fail on "
      "purpose. The session then leaves. With the drift on, watch the "
      "main menu 1 min.",
      "Then open Bench session again and pick Go on: Drill: job failure; "
      "the question comes first. Normal: no visible change, or a still "
      "cover.", s_drill2,
      "What showed after it?\n"
      "Drift or still cover: normal.\n"
      "Frozen: the menu stuck.\n"
      "Blank: nothing showed.\n"
      "It restarted: by itself.",
      { "Drift or still cover", "Frozen", "Blank", "It restarted" },
      BENCH_F_LEAVES, 0, 120 },
    { "drill3", "Drill: backlight",
      "Done outside the session, about 2 min. With the drift on, in the "
      "main menu, let the screen go dark during a cross-fade (as the "
      "cover changes), then press MENU once to wake it.",
      "SELECT here leaves the session. Then open Bench session again and "
      "pick Go on: Drill: backlight. Normal: the drift carries on.", NULL,
      "Did the drift resume cleanly?\n"
      "Yes: it carried on.\n"
      "Stuck frame: a frozen picture.\n"
      "Hang: stuck; you restarted it.",
      { "Yes", "Stuck frame", "Hang", NULL }, 0, 0, 120 },
    { "scroll", "Scroll over drift",
      "Done outside the session, 1 min. With the drift on, open a long "
      "list (Songs) and turn the wheel fast to the bottom and back.",
      "SELECT here leaves the session. Then open Bench session again and "
      "pick Go on: Scroll over drift. Normal: smooth, nothing left "
      "behind.", NULL,
      "Was the scroll smooth?\n"
      "Yes: smooth.\n"
      "Tearing: split pictures.\n"
      "Stale pixels: bits left behind.\n"
      "Stutter: jerky.",
      { "Yes", "Tearing", "Stale pixels", "Stutter" }, 0, 0, 60 },
#endif
    /* ---- wave 2: only on its own go; the rows keep their own confirms,
       locks and questions ---- */
    { "wave2", "Wave 2", NULL, NULL, NULL,
      "Run wave 2: TV out, voltage, hibernate? Some need the AV cable and "
      "a TV; each has its own screens.\n"
      "Not now: skips all of it.\n"
      "Yes, run it: runs them, one by one.",
      { "Not now", "Yes, run it", NULL }, BENCH_F_GO, 0, 10 },
    { "pb6", "Probe PB-6",
      "Under 1 min. Needs the AV cable and a TV. Shows Apple's own TV "
      "test pattern; you pick NTSC or PAL first.",
      "Its own screens follow: MENU there cancels just this check. At the "
      "end it asks what the TV showed.", r_pb6, NULL, { NULL }, OPT, 0, 60 },
    { "pb7", "Probe PB-7",
      "Under 1 min. TV timing with nothing plugged in; nothing to watch.",
      "Its own screens follow: MENU there cancels just this check.", r_pb7,
      NULL, { NULL }, OPT, 0, 30 },
    { "pb8", "Probe PB-8",
      "About 2 min. Needs the AV cable and a TV: the chip's TV output in "
      "a few formats.",
      "Its own screens follow: MENU there cancels just this check. It "
      "asks what the TV showed. Locked until PB-7 passed.", r_pb8, NULL,
      { NULL }, OPT, 0, 120 },
    { "pb9", "Probe PB-9",
      "About 2 min. Needs the AV cable and a TV: the picture at two "
      "widths.",
      "Its own screens follow: MENU there cancels just this check. Locked "
      "until PB-8 passed.", r_pb9,
      NULL, { NULL }, OPT, 0, 120 },
    { "pw3", "Probe PW3",
      "About 2 min, hands off: the chip's core voltage steps through "
      "Apple's levels and back.",
      "Its own screens follow: MENU there cancels just this check.", r_pw3,
      NULL, { NULL }, OPT, 0, 120 },
    { "pw4", "Probe PW4",
      "About 3 min, hands off: the chip's clock and voltage step up and "
      "back. Locked until PW3 passed.",
      "Its own screens follow: MENU there cancels just this check.", r_pw4,
      NULL, { NULL }, OPT, 0, 180 },
    { "pw5", "Probe PW5",
      "About 3 min, hands off: the chip hibernates and wakes; the screen "
      "may go dark. Locked until PW1 passed.",
      "Its own screens follow: MENU there cancels just this check.", r_pw5,
      NULL, { NULL }, OPT, 0, 180 },
    { "pw6", "Probe PW6",
      "Under 1 min, hands off: reads the screen's state with the chip "
      "off.",
      "Its own screens follow: MENU there cancels just this check.", r_pw6,
      NULL, { NULL }, OPT, 0, 60 },
};
#undef BA
#undef BC
#undef OPT
#define NSTEPS ((int)ARRAYLEN(steps))
typedef char bench_table_fits[NSTEPS <= BENCH_MAX_STEPS ? 1 : -1];

/* ---- The owner's screens ------------------------------------------------
 * Text word-wrapped to WRAP_WIDTH (bench_next_line), then the buttons.
 * The fixed texts are macros so scripts/bench_script.py can read them:
 * the doc's "Owner's script" is generated from this file.  Plain words,
 * UK spelling, no step codes (owner, after sitting 4b): the session
 * decides what to run by itself, so a step's screen shows only SELECT;
 * MENU there still leaves quietly (resume later), but no screen offers
 * it.  MENU on a question skips the answer. */
#define WRAP_WIDTH      30          /* < SIMPLELIST_MAX_LINELENGTH */
#define SCR_MAX         26          /* text lines; + 2 buttons <= 32 */

#define MENU_CANCEL "MENU: cancel"
#define MENU_CLOSE  "MENU: close"
#define GO_START    "Scroll here, SELECT: start"
#define GO_LEAVE    "Scroll here, SELECT: leave"
#define GO_ON       "Scroll here, SELECT: go on"
#define GO_CLOSE    "Scroll here, SELECT: close"
#define ASK_HINT    "Pick an answer with SELECT. MENU: no answer."

#define HANDS_FMT   "Automatic checks next, about %u min. Put the iPod " \
    "down: no buttons are needed.\n\nThe screen will flash, show test " \
    "pictures or lists, and may go dark: that's normal.\n\nIf the iPod " \
    "restarts by itself: open Bench session again and pick Resume.\n\n" \
    "Screen frozen for over %u min: hold MENU and SELECT 6 to 8 s, then " \
    "the same."
#define DONE_FMT    "Automatic checks done: %u passed, %u failed, %u " \
    "skipped so far.\n\nNext: a step that needs you."
#define WAVE_FMT    "About %u min, %d checks."
#define RESTART_FMT "Set for the next start.\n\nNow restart: hold MENU " \
    "and SELECT together 6 to 8 s, until the screen goes dark.\n\nAfter " \
    "the restart: Developer > Bench session, then pick Go on: %s."
#define NORESTART_FMT "No restart seen: this is still the start the " \
    "step asked you to leave.\n\nHold MENU and SELECT together 6 to 8 s " \
    "until the screen goes dark.\n\nAfter the restart: Developer > Bench " \
    "session, then pick Go on: %s."
#define RETRY_FMT   "%s couldn't run in this start. A restart usually " \
    "fixes that, and the step then runs again.\n\nRestart and retry: sets " \
    "it up, then you restart.\nCarry on: the session goes on without it; " \
    "the steps that need it are skipped."
#define LEFT_FMT    "Set. Do the rest outside the session, as the first " \
    "screen said.\n\nThen: Developer > Bench session, and pick Go on: " \
    "%s. The question comes first."
#define RESUME_TEXT "A session is open: it stopped, or the iPod " \
    "restarted.\n\nResume or Go on: carries on where it stopped (a step " \
    "you left asks its question first).\nStart a new session: begins " \
    "again; the old results stay saved.\nCancel (or MENU): leave."
#define MODES_FMT   "Which session?\n\nFull session: every check, about " \
    "%u min. The first time on a build.\nRe-verify the video image: about " \
    "%u min, for a new video image after a full session.\nPort write " \
    "check: about %u min.\n\nPick with SELECT. MENU: cancel."
#define COUNT_FMT   "%d steps, about %u min."
#define WAVE2_FMT   "Wave 2 at the end only if you say yes there: about " \
    "%u min more."
#define IFPRE_FMT   "Plus about %u min if this iPod needs its test " \
    "pictures and a restart first."
#define RESTARTS_TEXT "If the iPod restarts by itself: open Bench session " \
    "again and pick Resume."
#define FULL_TEXT   "Have ready: battery charged, a USB-C cable, the " \
    "AV cable, headphones. The battery rows run unplugged.\n\nYou watch " \
    "two sets of test " \
    "pictures first; hands-off checks run between and after. All " \
    "read-only or put back, but the display program step."
#define REVERIFY_TEXT "Re-verifies a new video image on this iPod (a full " \
    "session ran before). Have ready: battery charged; nothing to plug " \
    "in.\n\nAll read-only or put back. The session picks what to run by " \
    "itself: if a step fails, the steps that need it are skipped, with " \
    "one line saying why."
#define PORT_TEXT   "Checks the video chip's port writes: patterns " \
    "written, read back and checked. Hands off " \
    "after the start; nothing to plug in.\n\nRead-only for your music."
#define STEPS_TEXT  "Each step's first screen says what to do, how long " \
    "it takes and what is normal."
#define SKIP_TITLE  "Skipping ahead"
#define FINISH_TEXT "That's all for today. Plug the iPod into the Mac."
#define STOPPED_TEXT "Results so far are saved. To carry on: open Bench " \
    "session again and pick Go on or Resume."
#define NOTPASS_TEXT "Didn't pass:"

static struct
{
    int n;
    char text[SCR_MAX][WRAP_WIDTH + 1];
    const char *line[SCR_MAX];
} scr;

static void scr_reset(void)
{
    scr.n = 0;
}

static void scr_add(const char *text)
{
    const char *p = text, *l;
    int len;

    while (scr.n < SCR_MAX && (p = bench_next_line(p, WRAP_WIDTH, &l, &len)))
    {
        memcpy(scr.text[scr.n], l, (size_t)len);
        scr.text[scr.n][len] = '\0';
        scr.line[scr.n] = scr.text[scr.n];
        scr.n++;
    }
}

static void scr_blank(void)
{
    if (scr.n < SCR_MAX)
    {
        scr.text[scr.n][0] = '\0';
        scr.line[scr.n] = scr.text[scr.n];
        scr.n++;
    }
}

static bool scr_go(const char *title, const char *menu, const char *go)
{
    return devbcm_lines_go(title, scr.line, scr.n, menu, go);
}

/* One text, then MENU and SELECT: true for SELECT */
static bool screen(const char *title, const char *text, const char *menu,
                   const char *go)
{
    scr_reset();
    scr_add(text);
    return scr_go(title, menu, go);
}

int devbench_ask(const char *title, const char *text,
                 const char *const *opts, int n)
{
    scr_reset();
    scr_add(text);
    return devbcm_lines_choose(title, scr.line, scr.n, opts, n);
}

/* The wave 2 steps (the go question's time): the table's optional ones */
static unsigned long optional_secs(int *count)
{
    unsigned long secs = 0;
    int k, n = 0;

    for (k = 0; k < NSTEPS; k++)
        if (steps[k].flags & BENCH_F_OPTIONAL)
        {
            secs += bench_step_secs(&steps[k]);
            n++;
        }
    *count = n;
    return secs;
}

/* A step's start screen: say, an empty line, say2, then SELECT (start
   it, or leave the session to do a manual one).  No MENU line: MENU still
   stops the session quietly (this step comes back at the next press) */
static bool start_screen(const struct bench_step *s)
{
    scr_reset();
    scr_add(s->say);
    if (s->say2)
    {
        scr_blank();
        scr_add(s->say2);
    }
    return scr_go(s->title, NULL, s->fn ? GO_START : GO_LEAVE);
}

/* ---- Running a step ---------------------------------------------------- */

static void record(int k, enum bench_res v, int a, unsigned long ms,
                   struct bench_state *st)
{
    const struct bench_step *s = &steps[k];
    char line[BENCH_LINE_MAX + 64];
    const char *ans = a < 0 ? NULL : a == BENCH_ANSWERS ? "Skip"
                    : s->answers[a];
    int i;

    for (i = 0; i < B.nsaid; i++)
        devbcm_log(B.said[i]);
    if (B.pinned[0])
    {
        bench_fmt_log(line, sizeof(line), s->id, B.pinned);
        put(line);
    }
    for (i = 0; i < B.nkeep; i++)
    {
        bench_fmt_log(line, sizeof(line), s->id, B.keep[i]);
        put(line);
    }
    bench_fmt_result(line, sizeof(line), s->id, v, ans, ms, B.note);
    put(line);
    snprintf(line, sizeof(line), "auto: %s %s answer=%s ms=%lu %s", s->id,
             bench_res_name[v], ans ? ans : "-", ms, B.note);
    devbcm_log(line);
    bench_set(st, k, v);
    if ((s->flags & BENCH_F_GO) && v != BENCH_SKIP)
        st->go_yes = a == 1;
    st->next = k + 1;
}

static void clear_lines(void)
{
    memset(&B.lines, 0, sizeof(B.lines));
    B.nkeep = 0;
    B.nsaid = 0;
    B.pinned[0] = '\0';
    B.note[0] = '\0';
    B.restart = false;
}

/* A planned restart asked for: the RTC's seconds into the file (the next
   press checks a restart came, bench_restart_seen), then the screen */
static void ask_restart(int k)
{
    char line[BENCH_LINE_MAX];

    bench_fmt_restart_asked(line, sizeof(line), steps[k].id, wall_now());
    put(line);
    snprintf(line, sizeof(line), RESTART_FMT, steps[k].title);
    screen(steps[k].title, line, MENU_CLOSE, GO_CLOSE);
}

/* The step's question: its text (the answers' meanings with it), the
   hint and its answers; MENU skips it (recorded as "Skip").  The go
   question adds what it would run */
static int ask(const struct bench_step *s)
{
    const char *opts[BENCH_ANSWERS];
    int n = bench_n_answers(s), k, a;

    if (!s->ask)
        return -1;
    for (k = 0; k < n; k++)
        opts[k] = s->answers[k];
    scr_reset();
    scr_add(s->ask);
    if (s->flags & BENCH_F_GO)
    {
        char line[48];
        int count;
        unsigned long secs = optional_secs(&count);

        snprintf(line, sizeof(line), WAVE_FMT, bench_minutes(secs), count);
        scr_add(line);
    }
    scr_add(ASK_HINT);
    a = devbcm_lines_choose(s->title, scr.line, scr.n, opts, n);
    return a < 0 || a >= n ? BENCH_ANSWERS : a;
}

/* After a step's answer: stage 4c's flag (videos at 150 MHz) stands only
   on the owner's "Yes" to its pictures; lit24ui's clock goes back to 30 */
static void after(const struct bench_step *s, int a)
{
    if (a > 0 && !strcmp(s->id, "s4c"))
    {
        bcm_devtest_forget_4c();
        say("s4c: 4c pass flag removed (the pictures were not judged right)");
    }
#if defined(HAVE_BCMFW_RESIDENT) && defined(HAVE_PLL_IDLE)
    if (!strcmp(s->id, "lit24ui"))
        clockpol_lit24_set(false);
#endif
}

/* Finish a step left dangling on purpose (a manual step, a restart).
   false: a planned restart that never came (sitting 3, 2026-10-26: the
   boot step was recorded done in the start it asked to leave, and jobs
   then found no display driver); nothing is recorded, the step stays
   open and the caller says so */
static bool finish(int k, struct bench_state *st)
{
    bool restart = steps[k].flags & BENCH_F_REBOOTS;
    enum bench_res autor = BENCH_NONE;
    int a;

    if (restart && !bench_restart_seen(st->restart_asked, wall_start()))
    {
        char line[BENCH_LINE_MAX + 64];

        bench_fmt_log(line, sizeof(line), steps[k].id,
                      "No restart seen: still the start that asked for it");
        put(line);
        snprintf(line, sizeof(line), "auto: %s no restart seen (asked at "
                 "%lu, this start began at %lu)", steps[k].id,
                 st->restart_asked, wall_start());
        devbcm_log(line);
        return false;
    }

#if defined(HAVE_BCMFW_RESIDENT) && defined(HAVE_PLL_IDLE) && \
    defined(HAVE_IPOD_UI_DEVMODE)
    bool lit = !strcmp(steps[k].id, "lit24ui");

    if (lit)
        litmiss_end();              /* the question's frames don't count */
#endif
#ifdef HAVE_UI_THREAD_SPLIT
    bool ucop = !strcmp(steps[k].id, "ucop2");
#endif
    a = restart ? -1 : ask(&steps[k]);
    clear_lines();
#if defined(HAVE_BCMFW_RESIDENT) && defined(HAVE_PLL_IDLE) && \
    defined(HAVE_IPOD_UI_DEVMODE)
    if (lit)
        autor = lit24ui_end();      /* its lines join the step's record */
#endif
#ifdef HAVE_UI_THREAD_SPLIT
    if (ucop)
    {
        autor = ucop2_end();        /* the counters over the soak */
        /* a failed soak, or an answer but Normal: ucop's switch goes,
           so the next start runs the build's default (ucop_settle()) */
        if (autor != BENCH_PASS || a != 0)
            fe_be_mode_clear_next();
    }
#endif
    if (!strcmp(steps[k].id, "vplay"))
    {
        /* the first-use check's verdict and time (the vck line has the
           detail), on the step's own result line */
        char vs[112];

        vplayer_check_summary(vs, sizeof(vs));
        say(vs);
        strlcpy(B.note, vs, sizeof(B.note));
        if (strstr(vs, "FAIL"))
            autor = BENCH_FAIL;
    }
    if (!restart)
        after(&steps[k], a);
    else
    {
        /* what the restart brought: the next steps need the display
           driver (pre's check) */
        bool again;
        const char *why = env_why(&again);
        char line[BENCH_KEEP_LEN];

        if (st->restart_asked)
            snprintf(line, sizeof(line), "restart seen; %s%s",
                     why ? "but " : "the display driver runs",
                     why ? why : "");
        else
            snprintf(line, sizeof(line), "restart not checked (no record); "
                     "%s%s", why ? "" : "the display driver runs",
                     why ? why : "");
        say(line);
        strlcpy(B.note, line, sizeof(B.note));
        autor = BENCH_DONE;
#ifdef HAVE_UI_THREAD_SPLIT
        /* ucop's restart must bring the UI onto the COP: if the switch
           was refused or fell back, the second-core steps after it would
           test nothing, so it fails and they skip (devbench.h's gates) */
        if (!strcmp(steps[k].id, "ucop") && fe_be_mode() != FE_COP)
        {
            snprintf(line, sizeof(line), "ucop: the UI runs %s after the "
                     "restart, not on the COP (refused or fell back: fe "
                     "lines)", fe_be_mode_name(fe_be_mode()));
            say(line);
            strlcpy(B.note, line, sizeof(B.note));
            autor = BENCH_FAIL;
        }
#endif
    }
    record(k, restart ? autor : bench_verdict(&steps[k], autor, a),
           a, 0, st);
    st->dangling = -1;
    return true;
}

/* false: the owner stopped the session here (MENU), or left it to do a
   manual step; the next press resumes */
static bool run(int k, struct bench_state *st)
{
    const struct bench_step *s = &steps[k];
    const char *why = bench_skip_why(st, s);
    char line[256];
    long t0;
    int r, a;

    clear_lines();
    if (!why && (s->flags & BENCH_F_COP) && bcm_cop_stuck())
    {
        st->cop_stuck = true;
        why = bench_skip_why(st, s);
    }
    if (why)
    {
        strlcpy(B.note, why, sizeof(B.note));
        record(k, BENCH_SKIP, -1, 0, st);
        return true;
    }
    if (s->say && !start_screen(s))
        return false;
    bench_fmt_start(line, sizeof(line), s->id);
    put(line);
#ifdef HAVE_HANG_RECORD
    hang_bench_step = s->id;        /* the hang record's bench= */
#endif
    vplayer_bench_play(true);       /* no first-use check in a step's plays */
    if (!s->fn && !(s->flags & BENCH_F_GO))
        return false;               /* manual: asked at the next press */
    t0 = current_tick;
    r = BENCH_NONE;
    if (s->fn)
    {
        devbcm_bench_mode(!(s->flags & BENCH_F_OWN), hook);
        /* the freeze audit (sitting 4's freeze 1 fell between two steps,
           its lines lost): what came since the last step, its start
           screen and this start line, in the file before the step runs;
           the disk spins for the results line above anyway, and the
           watchdog is not armed yet */
        ui_log_flush();
        ui_log_hold(true);          /* the log stays in RAM mid-step */
        dog_set(s);
        /* the step's own long work on this thread (minutes, its own
         * watchdog above): the hang watcher leaves this thread alone */
        hang_pause(HANG_UI, true);
        r = s->fn();
        hang_pause(HANG_UI, false);
        dog_off();
        ui_log_hold(false);
        devbcm_bench_mode(false, NULL);
        ui_log_flush();             /* between steps, the watchdog off */
        if (r == BENCH_NONE)
            r = bench_lines_verdict(&B.lines);
    }
    if (s->flags & BENCH_F_REBOOTS)
    {
        /* recorded at the next press, after the restart (finish) */
        ask_restart(k);
        return false;
    }
    /* a step that could not run asks nothing: its question would be
       about nothing the owner saw */
    if (s->fn && r == BENCH_SKIP)
    {
        record(k, BENCH_SKIP, -1,
               (unsigned long)(current_tick - t0) * (1000 / HZ), st);
        return true;
    }
    if (s->flags & BENCH_F_LEAVES)
    {
        /* the owner watches outside; the question at the next press */
        snprintf(line, sizeof(line), LEFT_FMT, s->title);
        screen(s->title, line, MENU_CLOSE, GO_CLOSE);
        return false;
    }
    a = ask(s);
    after(s, a);
    record(k, bench_verdict(s, (enum bench_res)r, a), a,
           (unsigned long)(current_tick - t0) * (1000 / HZ), st);
    return true;
}

/* ---- The row ------------------------------------------------------------ */

/* The mode's confirm screen: the counts, then the mode's text; true for
   SELECT on its last line */
static bool confirm(int mode, const struct bench_run *sel)
{
    char line[96];
    int n, opt;
    unsigned long secs = bench_run_sum(sel, steps, 0, BENCH_SUM_MAIN, &n);
    unsigned long extra;

    scr_reset();
    snprintf(line, sizeof(line), COUNT_FMT, n, bench_minutes(secs));
    scr_add(line);
    extra = bench_run_sum(sel, steps, 0, BENCH_SUM_OPTIONAL, &opt);
    if (opt)
    {
        snprintf(line, sizeof(line), WAVE2_FMT, bench_minutes(extra));
        scr_add(line);
    }
    extra = bench_run_sum(sel, steps, 0, BENCH_SUM_IFPRE, &opt);
    if (opt)
    {
        snprintf(line, sizeof(line), IFPRE_FMT, bench_minutes(extra));
        scr_add(line);
    }
    scr_blank();
    scr_add(mode == BENCH_MODE_PORTCHECK ? PORT_TEXT
            : mode == BENCH_MODE_REVERIFY ? REVERIFY_TEXT : FULL_TEXT);
    scr_blank();
    if (mode != BENCH_MODE_PORTCHECK)
    {
        scr_add(STEPS_TEXT);
        scr_blank();
    }
    scr_add(RESTARTS_TEXT);
    return scr_go("Bench session", MENU_CANCEL, GO_START);
}

/* The hands-off screen: the block's time, and when a frozen screen means
   a restart by hand (its longest limit plus the watchdog's 10 s) */
static bool hands_off_screen(const struct bench_run *sel, int p)
{
    char line[400];
    unsigned long secs = bench_run_sum(sel, steps, p, BENCH_SUM_BLOCK, NULL);
    unsigned lim = 0;
    int q;

    for (q = p; q < sel->n && (steps[sel->k[q]].flags & BENCH_F_AUTO); q++)
        if (steps[sel->k[q]].limit_s > lim)
            lim = steps[sel->k[q]].limit_s;
    snprintf(line, sizeof(line), HANDS_FMT, bench_minutes(secs),
             bench_minutes(lim + 10));
    return screen("Hands off now", line, NULL, GO_START);
}

/* A text into the summary list, wrapped as the other screens are */
static void sum_add(const char *text)
{
    const char *p = text, *l;
    char piece[WRAP_WIDTH + 1];
    int len;

    while ((p = bench_next_line(p, WRAP_WIDTH, &l, &len)))
    {
        memcpy(piece, l, (size_t)len);
        piece[len] = '\0';
        simplelist_addline("%s", piece);
    }
}

#ifdef HAVE_UI_THREAD_SPLIT
/* The unit after the session (sitting 4d: it stays the owner's daily
   driver).  ucop's switch ("cop", persistent) stays only when ucop2
   passed; a session that ran ucop but whose ucop2 failed, hung, skipped
   or never ran takes it away, so the next start runs the build's default.
   A session that never ran ucop leaves the switch as the owner set it.
   The log line and the summary's sentence say where the next start puts
   the screens. */
static const char *ucop_next_text;

static int step_index(const char *id)
{
    int k;

    for (k = 0; k < NSTEPS && strcmp(steps[k].id, id); k++)
        ;
    return k < NSTEPS ? k : -1;
}

static void ucop_settle(const struct bench_state *st)
{
    int k1 = step_index("ucop"), k2 = step_index("ucop2");
    bool cop;
    char line[96];

    if (k1 < 0 || k2 < 0 || st->res[k1] == BENCH_NONE)
        return;                     /* no switch of this session's */
    if (st->res[k2] != BENCH_PASS)
        fe_be_mode_clear_next();    /* also a "cop" a boot guard held back */
    cop = fe_be_mode_next() == FE_COP;
    snprintf(line, sizeof(line), "next start: ui %s (ucop2 %s)",
             fe_be_mode_name(fe_be_mode_next()),
             bench_res_name[st->res[k2] < BENCH_N_RES ? st->res[k2] : 0]);
    devbcm_log(line);
    ucop_next_text = cop
        ? "Next start: the screen runs on the second core."
        : "Next start: the screen runs on the main processor.";
}
#else
#define ucop_next_text ((const char *)NULL)
static inline void ucop_settle(const struct bench_state *st) { (void)st; }
#endif

/* The end screen.  done: the session is over (every step ran or skipped
   by itself): the counts, what didn't pass (by title, no codes), the
   verified line when earned, then "That's all for today".  Else the
   session stopped or left for the owner: where to pick it up.
   verified: the verified-list line a passed run earns, else NULL;
   reverify: the run was the re-verify one (the full session says why
   there is no line only for the merged image, which could earn one); the
   port write check earns none and says nothing of it */
#define SUM_FAILED_MAX  8       /* titles listed; the rest "and n more" */

static bool summary(const struct bench_state *st, const struct bench_run *sel,
                    bool done, bool reverify, const char *verified)
{
    bool verifies = st->mode != BENCH_MODE_PORTCHECK;
    struct simplelist_info info;
    int p, nf = 0;

    simplelist_info_init(&info, "Bench session", 0, NULL);
    simplelist_reset_lines();
    simplelist_addline("%u passed, %u failed, %u skipped", st->passed,
                       st->failed, st->skipped);
    if (!done)
    {
        sum_add(STOPPED_TEXT);
        simplelist_addline("MENU: leave this screen");
        return simplelist_show_list(&info);
    }
    for (p = 0; p < sel->n; p++)
    {
        enum bench_res r = (enum bench_res)st->res[sel->k[p]];

        if (r != BENCH_FAIL && r != BENCH_HANG)
            continue;
        if (!nf)
            sum_add(NOTPASS_TEXT);
        if (++nf <= SUM_FAILED_MAX)
            simplelist_addline("- %s", steps[sel->k[p]].title);
    }
    if (nf > SUM_FAILED_MAX)
        simplelist_addline("- and %d more", nf - SUM_FAILED_MAX);
    if (verified)
    {
        const char *q = verified;
        char piece[WRAP_WIDTH + 1];

        simplelist_addline(reverify ? "Every step passed. Add this"
                                    : "Video checks passed. Add this");
        simplelist_addline("line to the verified list:");
        while (*q)
        {
            int l = bench_wrap(q, WRAP_WIDTH);

            strlcpy(piece, q, (size_t)l + 1);
            simplelist_addline("%s", piece);
            q += l;
        }
    }
    else if (reverify && !st->merged && verifies)
    {
        simplelist_addline("No verified line: the video");
        simplelist_addline("image isn't the merged one.");
    }
    else if (st->merged && verifies)
    {
        simplelist_addline(reverify ? "Not every step passed:"
                                    : "Not every video check passed:");
        simplelist_addline("no verified line this time.");
    }
    if (ucop_next_text)
        sum_add(ucop_next_text);
    sum_add(FINISH_TEXT);
    simplelist_addline("MENU: leave this screen");
    return simplelist_show_list(&info);
}

/* A must-pass step k that couldn't run for a reason a restart may fix
   (B.restart: no display driver in this start): offer the restart.  On
   "Restart and retry" the file says to run k again (retry lines, which
   the replay honours) and the boot step runs as itself: the display
   driver set, the restart asked for; after it, Go on finishes boot
   (checking the restart came) and runs k.  1: left for the restart; 0:
   the owner carries on without it (or MENU): the steps that need it skip
   by themselves; -1: not offered (B.restart clear, or no boot step) */
static int offer_retry(int k)
{
    static const char *const opts[2] = { "Restart and retry", "Carry on" };
    int kb = bench_find(steps, NSTEPS, "boot");
    char text[400], line[BENCH_LINE_MAX];

    if (!B.restart || kb < 0 || !steps[kb].fn)
        return -1;
    snprintf(text, sizeof(text), RETRY_FMT, steps[k].title);
    if (devbench_ask(steps[k].title, text, opts, 2) != 0)
        return 0;
    bench_fmt_retry(line, sizeof(line), steps[k].id);
    put(line);
    bench_fmt_retry(line, sizeof(line), steps[kb].id);
    put(line);
    /* the log's copy forgets their results too (bench_logscan) */
    snprintf(line, sizeof(line), "auto: %s retry", steps[k].id);
    devbcm_log(line);
    snprintf(line, sizeof(line), "auto: %s retry", steps[kb].id);
    devbcm_log(line);
    bench_fmt_start(line, sizeof(line), steps[kb].id);
    put(line);
    clear_lines();
    devbcm_bench_mode(true, hook);
    steps[kb].fn();
    devbcm_bench_mode(false, NULL);
    ask_restart(kb);
    return 1;
}

/* The mode question, each mode's time with it */
static int pick_mode(void)
{
    char text[400];
    unsigned m[BENCH_N_MODES];
    struct bench_run run;
    int k;

    for (k = 0; k < BENCH_N_MODES; k++)
    {
        bench_select(k, steps, NSTEPS, &run);
        m[k] = bench_minutes(bench_run_sum(&run, steps, 0, BENCH_SUM_MAIN,
                                           NULL));
    }
    snprintf(text, sizeof(text), MODES_FMT, m[BENCH_MODE_FULL],
             m[BENCH_MODE_REVERIFY], m[BENCH_MODE_PORTCHECK]);
    return bench_mode_choice(devbench_ask("Bench session", text,
                                          bench_mode_answers,
                                          BENCH_N_MODES));
}

/* After the file's replay: results the file lost, from the logs
   (bench_logscan), oldest generation first, into the file and the
   state */
#define LOG_GENS    5               /* bcm-devtest-hal.c DT_LOG_GENS */
static struct bench_logscan logscan;

static void replay_log(struct bench_state *st)
{
    char path[MAX_PATH], line[BENCH_LINE_MAX + 64];
    const char *name = strrchr(B.path, '/');
    int g, fd, k = 0;

    bench_logscan_init(&logscan, name ? name + 1 : B.path);
    for (g = LOG_GENS; g >= 0; g--)
    {
        if (g)
            snprintf(path, sizeof(path), ROCKBOX_DIR "/bcmtest.%d.log", g);
        else
            snprintf(path, sizeof(path), ROCKBOX_DIR "/bcmtest.log");
        fd = open(path, O_RDONLY);
        if (fd < 0)
            continue;
        while (read_line(fd, line, sizeof(line)) > 0)
            bench_logscan_line(&logscan, steps, NSTEPS, line);
        close(fd);
    }
    while ((k = bench_logscan_missing(&logscan, st, steps, NSTEPS, k, line,
                                      sizeof(line))) >= 0)
    {
        put(line);
        bench_replay(st, steps, NSTEPS, line);
        snprintf(path, sizeof(path), "auto: %s recovered from the log",
                 steps[k].id);
        ui_log("bench", "%s", path);
        k++;
    }
}

static void build_part(char *out, size_t n)
{
    bench_name_part(out, n, rbversion);
}

/* The newest file of this build in BENCH_DIR into B.path; false if none */
static bool find_latest(void)
{
    char bp[40], tail[48], best[MAX_PATH] = "";
    DIR *d = opendir(BENCH_DIR);
    struct dirent *e;
    size_t tl;

    if (!d)
        return false;
    build_part(bp, sizeof(bp));
    snprintf(tail, sizeof(tail), "-%s.txt", bp);
    tl = strlen(tail);
    while ((e = readdir(d)))
    {
        size_t l = strlen((char *)e->d_name);

        if (l > tl && !strcmp((char *)e->d_name + l - tl, tail) &&
            strcmp((char *)e->d_name, best) > 0)
            strlcpy(best, (char *)e->d_name, sizeof(best));
    }
    closedir(d);
    if (!best[0])
        return false;
    snprintf(B.path, sizeof(B.path), BENCH_DIR "/%s", best);
    return true;
}

static void replay_file(struct bench_state *st, const struct bench_step *t,
                        int n)
{
    char line[BENCH_LINE_MAX + 64];
    int fd = open(B.path, O_RDONLY);

    bench_state_init(st);
    if (fd < 0)
        return;
    while (read_line(fd, line, sizeof(line)) > 0)
        bench_replay(st, t, n, line);
    close(fd);
}

static bool new_file(int nsteps, int mode)
{
    char bp[40], dt[20], line[200];
    struct tm *tm = get_time();

    build_part(bp, sizeof(bp));
    put_drain();                    /* the old session's lines, not here */
    mkdir(BENCH_DIR);
    snprintf(B.path, sizeof(B.path),
             BENCH_DIR "/%04d%02d%02d-%02d%02d%02d-%s.txt",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour,
             tm->tm_min, tm->tm_sec, bp);
    snprintf(dt, sizeof(dt), "%04d-%02d-%02d %02d:%02d", tm->tm_year + 1900,
             tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min);
    bench_fmt_session(line, sizeof(line), bp, dt, nsteps, mode,
                      (unsigned long)BCMVIDEO_IMAGE_BYTES,
                      (uint32_t)BCMVIDEO_IMAGE_FNV, BCMVIDEO_IMAGE_KIND);
    put(line);
    log_mark();
    return file_exists(B.path);
}

static bool session(void);

/* The session with ipodui.log's bench ring (ui_log_bench): a step's held
   lines all reach the file; a step left for outside use (BENCH_F_LEAVES)
   runs with the normal ring, the session over by then.  Results lines
   kept in RAM (put_keep) go in first, before the file is read again */
bool devbench_session(void)
{
    bool r;

    ui_log_bench(true);
    put_flush();                    /* lines kept since the leave, first */
    r = session();
    ui_log_bench(false);
    return r;
}

static bool session(void)
{
    struct simplelist_info info;
    struct bench_state st;
    struct bench_run sel, picks;
    char line[BENCH_LINE_MAX + 64];
    const char *verified = NULL;
    char vline[BENCH_LINE_MAX];
    bool reverify, retry_offered = false;
    int p, gate_shown = -1;

    bench_state_init(&st);
    if (find_latest())
    {
        replay_file(&st, steps, NSTEPS);
        if (st.started && !st.ended)
            replay_log(&st);
    }
    if (st.started && !st.ended)
    {
        static const char *opts[3] = { NULL, "Start a new session",
                                       "Cancel" };
        static char resume[48];
        int at = st.dangling >= 0 ? st.dangling : st.last_done, c;

        snprintf(resume, sizeof(resume), at >= 0 ? "Resume after %s"
                 : "Resume", at >= 0 ? steps[at].title : "");
        if (st.dangling >= 0 &&
            (!steps[at].fn ||
             (steps[at].flags & (BENCH_F_REBOOTS | BENCH_F_LEAVES))))
            snprintf(resume, sizeof(resume), "Go on: %s", steps[at].title);
        opts[0] = resume;
        c = devbench_ask("Bench session", RESUME_TEXT, opts, 3);
        if (c < 0 || c == 2)
            return false;
        if (c == 1)
        {
            ucop_settle(&st);       /* the old session's switch, settled */
            st.started = false;
        }
    }
    else
        st.started = false;

    if (!st.started)
    {
        int mode = pick_mode();

        if (mode < 0)
            return false;
        bench_select(mode, steps, NSTEPS, &sel);
        if (!confirm(mode, &sel))
            return false;
        audio_stop();               /* the rows' confirms did, before */
        bench_state_init(&st);
        st.mode = mode;
        st.merged = !strcmp(BCMVIDEO_IMAGE_KIND, "merged");
        st.image_bytes = BCMVIDEO_IMAGE_BYTES;
        st.image_fnv = BCMVIDEO_IMAGE_FNV;
        if (!new_file(sel.n, mode))
        {
            simplelist_info_init(&info, "Bench session", 0, NULL);
            simplelist_reset_lines();
            simplelist_addline("Not run: the results can't be saved");
            return devbcm_show(&info);
        }
        /* the switches the session needs (bcmjobs.on), set by it and
           undone at its end; true: one restart first, then Resume */
        if (devswitch_bench_begin(mode))
            return false;
        {
            struct tm *tm = get_time();

            snprintf(st.date, sizeof(st.date), "%04d-%02d-%02d",
                     tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
        }
        p = 0;
    }
    else
    {
        int d = st.dangling;
        enum bench_res r = bench_settle(&st, steps);

        bench_select(st.mode, steps, NSTEPS, &sel);
        if (r == BENCH_HANG)
        {
            clear_lines();
            strlcpy(B.note, "restarted during the step", sizeof(B.note));
            bench_fmt_result(line, sizeof(line), steps[d].id, BENCH_HANG,
                             NULL, 0, B.note);
            put(line);
            snprintf(line, sizeof(line), "auto: %s hang (a restart)",
                     steps[d].id);
            devbcm_log(line);
        }
        else if (r == BENCH_START && !finish(d, &st))
        {
            /* the planned restart never came: say so, offer it again */
            snprintf(line, sizeof(line), NORESTART_FMT, steps[d].title);
            screen("No restart seen", line, MENU_CLOSE, GO_CLOSE);
            return false;
        }
        log_mark();
        p = bench_run_next(&sel, &st);
        snprintf(line, sizeof(line), "resume after=%s\n",
                 steps[sel.k[p > 0 ? p - 1 : 0]].id);
        put(line);
    }
    reverify = st.mode == BENCH_MODE_REVERIFY;

    dog_start_thread();
    for (; p < sel.n; p++)
    {
        int k = sel.k[p], dep;
        bool hands_off = steps[k].flags & BENCH_F_AUTO;
        bool was = p > 0 && (steps[sel.k[p - 1]].flags & BENCH_F_AUTO);
        const char *why = bench_role_skip(&sel, p, &st);

        if (why)
        {
            clear_lines();
            strlcpy(B.note, why, sizeof(B.note));
            record(k, BENCH_SKIP, -1, 0, &st);
            continue;
        }
        /* a step whose needs failed skips by itself (devbench.h, the
           picks' need[] and bench_gates[]): recorded with the reason, and
           one plain sentence on screen, once a gate */
        dep = bench_dep_skip(&sel, steps, &st, p);
        if (dep >= 0)
        {
            int root = bench_dep_root(&sel, steps, &st, dep);

            clear_lines();
            bench_fmt_autoskip(B.note, sizeof(B.note), steps, &st, dep);
            record(k, BENCH_SKIP, -1, 0, &st);
            if (root != gate_shown)
            {
                gate_shown = root;
                screen(SKIP_TITLE, bench_dep_why(steps, root), NULL, GO_ON);
            }
            continue;
        }
        /* the screens where the owner's part changes: hands off, then
           back */
        if (hands_off && !was && p > 0 && !hands_off_screen(&sel, p))
            break;
        if (!hands_off && was)
        {
            snprintf(line, sizeof(line), DONE_FMT, st.passed, st.failed,
                     st.skipped);
            if (!screen("Automatic checks done", line, NULL, GO_ON))
                break;
        }
        if (!run(k, &st))
            break;
        if (reverify && sel.role[p] == BENCH_R_NEED &&
            st.res[k] == BENCH_SKIP && !retry_offered)
        {
            /* a must-pass step couldn't run: the line can't be earned as
               it stands.  When a restart may fix it (no display driver in
               this start), offer it once, and the step runs again after
               it.  Else, or if the owner carries on, the session goes on:
               what needs the step skips by itself, the rest still runs */
            retry_offered = true;
            if (offer_retry(k) > 0)
                break;              /* left for the restart: Go on */
        }
    }
    dog_stop_thread();
#ifdef HAVE_HANG_RECORD
    hang_bench_step = NULL;
#endif
    vplayer_bench_play(false);
    if (p < sel.n)
        return summary(&st, &sel, false, reverify, NULL);
    /* the full session runs every check the re-verify run makes (and
       more): either earns the merged image's verified line */
    if (st.mode == BENCH_MODE_PORTCHECK ? false
        : reverify ? bench_reverify_ok(&sel, &st)
                   : bench_full_ok(steps, NSTEPS, &st, &picks))
    {
        char bp[40];

        build_part(bp, sizeof(bp));
        bench_fmt_verified_in(vline, sizeof(vline), st.image_fnv,
                              st.image_bytes, bp, st.date,
                              bench_opt_passed(reverify ? &sel : &picks,
                                               &st), st.mode);
        snprintf(line, sizeof(line), "verified %s\n", vline);
        put(line);
        verified = vline;
    }
    if (st.mode != BENCH_MODE_PORTCHECK && st.merged &&
        bench_clock150_ok(steps, NSTEPS, &st))
    {
        char bp[40], cl[200];

        build_part(bp, sizeof(bp));
        bench_fmt_clock150(cl, sizeof(cl), st.image_fnv, st.image_bytes, bp,
                           st.date);
        snprintf(line, sizeof(line), "token %s\n", cl);
        put(line);
    }
    ucop_settle(&st);               /* before the end line: the next start */
    snprintf(line, sizeof(line), "end passed=%u failed=%u skipped=%u\n",
             st.passed, st.failed, st.skipped);
    put(line);
    put_drain();                    /* the end: what still can't go in */
    devswitch_bench_end();          /* the switches begin() set, undone */
    return summary(&st, &sel, true, reverify, verified);
}
#endif /* (HAVE_BCMFW_VIDEO && !SIMULATOR) || DEVBENCH_SIM */
