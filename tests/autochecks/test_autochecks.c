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

#include <stdio.h>
#include <string.h>
/* sitting 4d holds some picks (devbench.h BENCH_R_HELD); the tests that
   cover the roles those picks normally have run with the hold off */
static int hold_later = 1;
#define BENCH_HOLD_LATER hold_later
#include "devbench.h"
#include "devbench_power.h"
#include "devbench_vclk.h"

static int fails;

#define CHECK(c) do { if (!(c)) { fails++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static int ran(void) { return BENCH_PASS; }

/* A table shaped like devbench.c's: part A, a COP pair, a guided step,
 * a planned restart, a go question and its optional steps */
static const struct bench_step T[] =
{
    { "s05", "Stages", NULL, NULL, ran, NULL, { NULL }, BENCH_F_AUTO, 120, 0 },
    { "c1", "C1", NULL, NULL, ran, NULL, { NULL },
      BENCH_F_AUTO | BENCH_F_COP, 60, 0 },
    { "c2", "C2", NULL, NULL, ran, NULL, { NULL },
      BENCH_F_AUTO | BENCH_F_COP, 30, 0 },
    { "pb1", "PB-1", NULL, NULL, ran, NULL, { NULL }, BENCH_F_AUTO, 60, 0 },
    { "b13", "Warm swap", "Watch the screen", NULL, ran,
      "Did the screen blink or flicker?", { "No", "Yes, briefly",
      "Yes, badly", NULL }, 0, 0, 0 },
    { "drill1", "Drill", "Hold MENU through a restart", NULL, NULL,
      "Did the screen recover?", { "Yes", "Still cover", "Hang", "Reboot" },
      0, 0, 0 },
    { "boot", "Boot path", "Restart now", NULL, ran, NULL, { NULL },
      BENCH_F_REBOOTS, 0, 0 },
    { "wave2", "Wave 2", NULL, NULL, NULL, "Run wave 2?",
      { "Not now", "Yes, run it", NULL }, BENCH_F_GO, 0, 0 },
    { "pb6", "PB-6", NULL, NULL, ran, NULL, { NULL },
      BENCH_F_OPTIONAL | BENCH_F_OWN, 0, 0 },
};
#define N ((int)(sizeof(T) / sizeof(T[0])))

static void t_classify(void)
{
    CHECK(bench_classify("pb1: PASS (12 blocks)") == BENCH_PASS);
    CHECK(bench_classify("c1: FAIL (rc -3): CPU PASS, COP FAIL") == BENCH_FAIL);
    CHECK(bench_classify("pb1 canary boot: counter +0 us: FAIL (run void)")
          == BENCH_FAIL);
    CHECK(bench_classify("pw2: hang at step 3") == BENCH_FAIL);
    CHECK(bench_classify("Not available: no probe image") == BENCH_SKIP);
    CHECK(bench_classify("Locked.") == BENCH_SKIP);
    CHECK(bench_classify("All stages passed") == BENCH_PASS);
    CHECK(bench_classify("Stage 4: PASS") == BENCH_PASS);
    /* words, not substrings */
    CHECK(bench_classify("PASSAGE FAILSAFE changed") == BENCH_NONE);
    CHECK(bench_classify("lab5 pitch 300: 160 px from row 31") == BENCH_NONE);
    CHECK(bench_classify("") == BENCH_NONE);
}

static void t_lines_verdict(void)
{
    struct bench_tally_lines t = { 0, 0, 0 };

    CHECK(bench_lines_verdict(&t) == BENCH_DONE);
    bench_lines_add(&t, BENCH_PASS);
    CHECK(bench_lines_verdict(&t) == BENCH_PASS);
    bench_lines_add(&t, BENCH_SKIP);
    CHECK(bench_lines_verdict(&t) == BENCH_PASS);
    bench_lines_add(&t, BENCH_FAIL);
    CHECK(bench_lines_verdict(&t) == BENCH_FAIL);
    memset(&t, 0, sizeof(t));
    bench_lines_add(&t, BENCH_SKIP);
    CHECK(bench_lines_verdict(&t) == BENCH_SKIP);
}

static void t_verdict(void)
{
    const struct bench_step *b13 = &T[4], *drill = &T[5], *go = &T[7];

    CHECK(bench_n_answers(b13) == 3);
    CHECK(bench_n_answers(drill) == 4);
    /* the automatic result with the owner's answer */
    CHECK(bench_verdict(b13, BENCH_PASS, 0) == BENCH_PASS);
    CHECK(bench_verdict(b13, BENCH_PASS, 1) == BENCH_FAIL);
    CHECK(bench_verdict(b13, BENCH_FAIL, 0) == BENCH_FAIL);
    CHECK(bench_verdict(b13, BENCH_PASS, BENCH_ANSWERS) == BENCH_PASS);
    CHECK(bench_verdict(b13, BENCH_DONE, 0) == BENCH_PASS);
    CHECK(bench_verdict(b13, BENCH_SKIP, BENCH_ANSWERS) == BENCH_SKIP);
    CHECK(bench_verdict(b13, BENCH_DONE, -1) == BENCH_DONE);
    /* a manual step: the answer is the verdict */
    CHECK(bench_verdict(drill, BENCH_NONE, 0) == BENCH_PASS);
    CHECK(bench_verdict(drill, BENCH_NONE, 2) == BENCH_FAIL);
    CHECK(bench_verdict(drill, BENCH_NONE, BENCH_ANSWERS) == BENCH_SKIP);
    /* a go question is done either way, skipped only by "Skip" */
    CHECK(bench_verdict(go, BENCH_NONE, 0) == BENCH_DONE);
    CHECK(bench_verdict(go, BENCH_NONE, 1) == BENCH_DONE);
    CHECK(bench_verdict(go, BENCH_NONE, BENCH_ANSWERS) == BENCH_SKIP);
    /* a hang stays a hang */
    CHECK(bench_verdict(&T[0], BENCH_HANG, -1) == BENCH_HANG);
}

static void t_format(void)
{
    char buf[BENCH_LINE_MAX + 64], tok[BENCH_TOKEN_MAX];
    struct bench_rec r;

    bench_token(tok, sizeof(tok), "Yes, briefly");
    CHECK(!strcmp(tok, "Yes,_briefly"));
    bench_token(tok, sizeof(tok), "a=b c\n");
    CHECK(!strcmp(tok, "ab_c"));
    bench_token(tok, sizeof(tok), NULL);
    CHECK(!strcmp(tok, "-"));

    bench_name_part(tok, sizeof(tok), "c08ccbe-M260924 (dirty)/x");
    CHECK(!strcmp(tok, "c08ccbe-M260924dirtyx"));
    bench_name_part(tok, sizeof(tok), "///");
    CHECK(!strcmp(tok, "x"));

    bench_fmt_start(buf, sizeof(buf), "pb1");
    CHECK(!strcmp(buf, "step=pb1 result=start\n"));
    CHECK(bench_parse(buf, &r) == BENCH_K_START && !strcmp(r.id, "pb1"));

    bench_fmt_result(buf, sizeof(buf), "b13", BENCH_FAIL, "Yes, badly",
                     61234, "warm: FAIL (3 of 8 swaps)\nsecond line");
    CHECK(!strcmp(buf, "step=b13 result=fail answer=Yes,_badly ms=61234 "
                       "note=warm: FAIL (3 of 8 swaps) second line\n"));
    CHECK(bench_parse(buf, &r) == BENCH_K_RESULT);
    CHECK(!strcmp(r.id, "b13") && r.res == BENCH_FAIL && r.ms == 61234);
    CHECK(!strcmp(r.answer, "Yes,_badly"));
    CHECK(bench_answer_index(&T[4], r.answer) == 2);
    CHECK(bench_answer_index(&T[4], "Skip") == BENCH_ANSWERS);
    CHECK(bench_answer_index(&T[4], "-") == -1);

    /* no answer: "-"; a note holding "answer=" and "ms=" doesn't confuse */
    bench_fmt_result(buf, sizeof(buf), "pb1", BENCH_PASS, NULL, 7,
                     "x answer=Yes ms=99 result=fail");
    CHECK(bench_parse(buf, &r) == BENCH_K_RESULT);
    CHECK(!strcmp(r.answer, "-") && r.ms == 7 && r.res == BENCH_PASS);

    /* a log line whose text looks like a result is still a log line */
    bench_fmt_log(buf, sizeof(buf), "pb1", "result=fail ms=3\r");
    CHECK(!strcmp(buf, "step=pb1 log=result=fail ms=3 \n"));
    CHECK(bench_parse(buf, &r) == BENCH_K_LOG && !strcmp(r.id, "pb1"));

    CHECK(bench_parse("session build=abc date=2026-10-24 12:00 steps=9",
                      &r) == BENCH_K_SESSION);
    CHECK(bench_parse("end passed=3 failed=1 skipped=2", &r) == BENCH_K_END);
    CHECK(bench_parse("resume after=c1", &r) == BENCH_K_RESUME &&
          !strcmp(r.id, "c1"));
    CHECK(bench_parse("garbage", &r) == BENCH_K_OTHER);
    CHECK(bench_parse("step=x result=bogus", &r) == BENCH_K_OTHER);

    /* an over-long note is cut, never overruns, and still parses */
    {
        char longnote[600];

        memset(longnote, 'n', sizeof(longnote) - 1);
        longnote[sizeof(longnote) - 1] = '\0';
        bench_fmt_result(buf, sizeof(buf), "pw1", BENCH_DONE, NULL, 1,
                         longnote);
        CHECK(strlen(buf) < sizeof(buf));
        CHECK(bench_parse(buf, &r) == BENCH_K_RESULT && r.res == BENCH_DONE);
    }
}

static void replay(struct bench_state *st, const char *const *lines)
{
    bench_state_init(st);
    for (; *lines; lines++)
        bench_replay(st, T, N, *lines);
}

static void t_resume(void)
{
    struct bench_state st;

    /* an empty file: from the start */
    {
        static const char *const f[] = { NULL };

        replay(&st, f);
        CHECK(st.next == 0 && st.dangling == -1 && !st.started);
        CHECK(bench_settle(&st, T) == BENCH_NONE);
    }
    /* a hang in PB-1: recorded as hung, resume after it */
    {
        static const char *const f[] = {
            "session build=b date=d steps=9",
            "step=s05 result=start",
            "step=s05 log=stage 5 PASS",
            "step=s05 result=pass answer=- ms=9 note=",
            "step=c1 result=start",
            "step=c1 result=pass answer=- ms=9 note=",
            "step=c2 result=start",
            "step=c2 result=done answer=- ms=9 note=",
            "step=pb1 result=start",
            NULL };

        replay(&st, f);
        CHECK(st.started && !st.ended);
        CHECK(st.dangling == 3 && st.next == 3 && st.last_done == 2);
        CHECK(st.passed == 3 && st.failed == 0);
        CHECK(bench_settle(&st, T) == BENCH_HANG);
        CHECK(st.next == 4 && st.failed == 1 && st.last_done == 3);
        CHECK(!st.cop_stuck);
    }
    /* a hang in a COP step: the later COP steps are skipped */
    {
        static const char *const f[] = {
            "step=s05 result=pass answer=- ms=9 note=",
            "step=c1 result=start",
            NULL };

        replay(&st, f);
        CHECK(bench_settle(&st, T) == BENCH_HANG);
        CHECK(st.cop_stuck && st.next == 2);
        CHECK(bench_skip_why(&st, &T[2]) != NULL);     /* c2 */
        CHECK(bench_skip_why(&st, &T[3]) == NULL);     /* pb1 */
    }
    /* a hang already recorded in the file (an earlier resume) */
    {
        static const char *const f[] = {
            "step=c1 result=start",
            "step=c1 result=hang answer=- ms=0 note=resumed",
            "resume after=c1",
            NULL };

        replay(&st, f);
        CHECK(st.cop_stuck && st.next == 2 && st.dangling == -1);
    }
    /* a planned restart: done, not hung */
    {
        static const char *const f[] = {
            "step=b13 result=pass answer=No ms=1 note=",
            "step=drill1 result=skip answer=Skip ms=1 note=",
            "step=boot result=start",
            NULL };

        replay(&st, f);
        CHECK(bench_settle(&st, T) == BENCH_START);
        CHECK(st.next == 6 && st.dangling == 6 && st.failed == 0 &&
              st.skipped == 1);
    }
    /* a manual step the owner left the session for: finish it on resume */
    {
        static const char *const f[] = {
            "step=b13 result=pass answer=No ms=1 note=",
            "step=drill1 result=start",
            NULL };

        replay(&st, f);
        CHECK(bench_settle(&st, T) == BENCH_START);
        CHECK(st.next == 5 && st.dangling == 5 && st.failed == 0);
    }
    /* the go question opens the optional steps; "Not now" keeps them shut */
    {
        static const char *const yes[] = {
            "step=wave2 result=done answer=Yes,_run_it ms=1 note=", NULL };
        static const char *const no[] = {
            "step=wave2 result=done answer=Not_now ms=1 note=", NULL };

        replay(&st, yes);
        CHECK(st.go_yes && bench_skip_why(&st, &T[8]) == NULL);
        replay(&st, no);
        CHECK(!st.go_yes && bench_skip_why(&st, &T[8]) != NULL);
    }
    /* steps from another build's table are counted, not placed */
    {
        static const char *const f[] = {
            "step=gone result=fail answer=- ms=1 note=",
            "step=s05 result=pass answer=- ms=1 note=",
            "end passed=1 failed=1 skipped=0",
            NULL };

        replay(&st, f);
        CHECK(st.ended && st.next == 1 && st.failed == 1 && st.passed == 1);
    }
}

/* A table with the re-verify run's steps, out of its run order, with a
 * full-run-only step between and the pre-flight marked picked */
static const struct bench_step R[] =
{
    { "pre", "Stage 5 flag", NULL, NULL, ran, NULL, { NULL },
      BENCH_F_PICKED, 0, 0 },
    { "s05", "Stages", "Watch", NULL, ran, "Did they look right?",
      { "Yes", "Garbled", NULL }, 0, 0, 0 },
    { "pb1", "PB-1", NULL, NULL, ran, NULL, { NULL }, BENCH_F_AUTO, 60, 0 },
    { "jobs", "Jobs", NULL, NULL, ran, NULL, { NULL }, BENCH_F_AUTO, 30, 0 },
    { "jvpu", "J_VPU", NULL, NULL, ran, NULL, { NULL }, BENCH_F_AUTO, 30, 0 },
    { "s6", "Clips", "Watch", NULL, ran, "How?", { "Smooth", "Stutter",
      NULL }, 0, 0, 0 },
    { "b13", "Warm swap", "Watch", NULL, ran, "Blink?", { "No", "Yes",
      NULL }, 0, 0, 0 },
    { "b5", "Window", "Watch", NULL, ran, "Whole?", { "Yes", "Broken",
      NULL }, 0, 0, 0 },
    { "b3", "Drift", "Do it", NULL, NULL, "Smooth?", { "Yes", "Stutter",
      NULL }, 0, 0, 0 },
    { "drill2", "Drill 2", "Watch", NULL, ran, "What showed?",
      { "Drift or still cover", "Frozen", NULL }, 0, 0, 0 },
};
#define NR ((int)(sizeof(R) / sizeof(R[0])))

/* the run's step ids, comma-joined */
static void run_ids(const struct bench_run *run, const struct bench_step *t,
                    char *out, size_t n)
{
    int p;

    out[0] = '\0';
    for (p = 0; p < run->n; p++)
    {
        if (p)
            strncat(out, ",", n - strlen(out) - 1);
        strncat(out, t[run->k[p]].id, n - strlen(out) - 1);
    }
}

static void replay_r(struct bench_state *st, const char *const *lines)
{
    bench_state_init(st);
    for (; *lines; lines++)
        bench_replay(st, R, NR, *lines);
}

#define RV_HEAD "session build=b date=2026-10-24 09:30 steps=9 mode=reverify " \
                "image=218756 fnv=9af095b8 kind=merged"

static void t_modes(void)
{
    struct bench_run run;
    struct bench_state st;
    char ids[160], buf[400];

    hold_later = 0;             /* the roles as they were (4c) */
    /* the row's question */
    CHECK(bench_mode_choice(0) == BENCH_MODE_FULL);
    CHECK(bench_mode_choice(1) == BENCH_MODE_REVERIFY);
    CHECK(bench_mode_choice(-1) == -1);         /* MENU */
    CHECK(bench_mode_choice(2) == BENCH_MODE_PORTCHECK);
    CHECK(bench_mode_choice(3) == -1);
    CHECK(!strcmp(bench_mode_answers[0], "Full session"));
    CHECK(!strcmp(bench_mode_answers[1], "Re-verify the video image"));
    CHECK(!strcmp(bench_mode_answers[2], "Port write check"));

    /* the full run: the table in order, the pre-flight left out */
    CHECK(bench_select(BENCH_MODE_FULL, R, NR, &run) == NR - 1);
    run_ids(&run, R, ids, sizeof(ids));
    CHECK(!strcmp(ids, "s05,pb1,jobs,jvpu,s6,b13,b5,b3,drill2"));
    CHECK(run.role[0] == BENCH_R_RUN);
    /* the devbench.c-shaped table T: unchanged, every step */
    CHECK(bench_select(BENCH_MODE_FULL, T, N, &run) == N);

    /* the re-verify run: its own order, pb1 left out */
    /* drill2 is no pick since sitting 3's trim: the full run only */
    CHECK(bench_select(BENCH_MODE_REVERIFY, R, NR, &run) == 8);
    run_ids(&run, R, ids, sizeof(ids));
    CHECK(!strcmp(ids, "pre,s05,jobs,b3,jvpu,b13,b5,s6"));
    CHECK(run.role[0] == BENCH_R_PRE && run.role[1] == BENCH_R_IFPRE &&
          run.role[2] == BENCH_R_NEED && run.role[3] == BENCH_R_OPT &&
          run.role[7] == BENCH_R_NEED);
    /* a build without the drift steps (no HAVE_BCM_JOBS): left out */
    CHECK(bench_select(BENCH_MODE_REVERIFY, R, NR - 2, &run) == 7);
    run_ids(&run, R, ids, sizeof(ids));
    CHECK(!strcmp(ids, "pre,s05,jobs,jvpu,b13,b5,s6"));

    /* the session line, written and read back */
    bench_fmt_session(buf, sizeof(buf), "b", "2026-10-24 09:30", 9,
                      BENCH_MODE_REVERIFY, 218756, 0x9af095b8u, "merged");
    CHECK(!strcmp(buf, RV_HEAD "\n"));
    bench_fmt_session(buf, sizeof(buf), "b", "2026-10-24 09:30", 30,
                      BENCH_MODE_FULL, 1, 2, "plain");
    /* the full session's: no mode=, the image fields too */
    CHECK(!strcmp(buf, "session build=b date=2026-10-24 09:30 steps=30 "
                  "image=1 fnv=00000002 kind=plain\n"));
    bench_state_init(&st);
    bench_replay(&st, R, NR, buf);
    CHECK(st.started && st.mode == BENCH_MODE_FULL && !st.merged);
    CHECK(st.image_bytes == 1 && st.image_fnv == 2);
    /* an older full session's line, without them */
    bench_state_init(&st);
    bench_replay(&st, R, NR, "session build=b date=2026-10-24 09:30 steps=30");
    CHECK(st.started && st.mode == BENCH_MODE_FULL && !st.merged);
    bench_state_init(&st);
    bench_replay(&st, R, NR, RV_HEAD);
    CHECK(st.mode == BENCH_MODE_REVERIFY && st.merged);
    CHECK(st.image_bytes == 218756 && st.image_fnv == 0x9af095b8u);
    CHECK(!strcmp(st.date, "2026-10-24"));
    bench_replay(&st, R, NR, "session build=b date=d steps=1 mode=reverify "
                 "image=1 fnv=00000001 kind=plain");
    CHECK(st.mode == BENCH_MODE_REVERIFY && !st.merged);
    /* a verified line in the file is no step line */
    CHECK(bench_parse("verified bcmvideo_jobs 9af095b8  stage 6 PASS", &(struct bench_rec){0})
          == BENCH_K_OTHER);
}

/* The port write check: pre, s05 only if the flag is missing, cr1; its
 * session line says mode=portcheck and a resume reads it back */
static const struct bench_step P[] =
{
    { "pre", "Stage 5 flag", NULL, NULL, ran, NULL, { NULL },
      BENCH_F_PICKED, 0, 0 },
    { "s05", "Stages", "Watch", NULL, ran, "Did they look right?",
      { "Yes", "Garbled", NULL }, 0, 0, 0 },
    { "b5", "Window", "Watch", NULL, ran, "Whole?", { "Yes", "Broken",
      NULL }, 0, 0, 0 },
    { "cr1", "COMMIT readback", NULL, NULL, ran, NULL, { NULL },
      BENCH_F_AUTO, 120, 0 },
    { "boot", "Driver", "Restart", NULL, ran, NULL, { NULL },
      BENCH_F_REBOOTS, 0, 0 },
};
#define NP ((int)(sizeof(P) / sizeof(P[0])))

static void t_portcheck(void)
{
    struct bench_run run;
    struct bench_state st;
    char ids[80], buf[300];

    /* the full run keeps cr1 in table order */
    CHECK(bench_select(BENCH_MODE_FULL, P, NP, &run) == NP - 1);
    run_ids(&run, P, ids, sizeof(ids));
    CHECK(!strcmp(ids, "s05,b5,cr1,boot"));
    /* the port write check: its three, no question but s05's offer */
    CHECK(bench_select(BENCH_MODE_PORTCHECK, P, NP, &run) == 3);
    run_ids(&run, P, ids, sizeof(ids));
    CHECK(!strcmp(ids, "pre,s05,cr1"));
    CHECK(run.role[0] == BENCH_R_PRE && run.role[1] == BENCH_R_IFPRE &&
          run.role[2] == BENCH_R_NEED);
    CHECK(P[run.k[2]].ask == NULL && (P[run.k[2]].flags & BENCH_F_AUTO));
    /* s05 runs only when the pre-flight skipped */
    bench_state_init(&st);
    st.mode = BENCH_MODE_PORTCHECK;
    st.res[0] = BENCH_PASS;
    CHECK(bench_role_skip(&run, 1, &st) != NULL);
    st.res[0] = BENCH_SKIP;
    CHECK(bench_role_skip(&run, 1, &st) == NULL);
    /* it never earns a verified line */
    st.res[0] = BENCH_PASS;
    st.res[2] = BENCH_PASS;
    st.merged = true;
    CHECK(!bench_reverify_ok(&run, &st));
    /* the session line, written and read back */
    bench_fmt_session(buf, sizeof(buf), "b", "2026-10-25 18:00", 3,
                      BENCH_MODE_PORTCHECK, 218660, 0xcd8e5d96u, "merged");
    CHECK(!strcmp(buf, "session build=b date=2026-10-25 18:00 steps=3 "
                  "mode=portcheck image=218660 fnv=cd8e5d96 kind=merged\n"));
    bench_state_init(&st);
    bench_replay(&st, P, NP, buf);
    CHECK(st.started && st.mode == BENCH_MODE_PORTCHECK && st.merged);
    /* a resume after pre carries on at cr1's turn (s05 skipped there) */
    bench_replay(&st, P, NP, "step=pre result=pass answer=- ms=1 note=");
    bench_select(st.mode, P, NP, &run);
    CHECK(bench_run_next(&run, &st) == 1);
    bench_replay(&st, P, NP, "step=s05 result=skip answer=- ms=0 note=");
    CHECK(bench_run_next(&run, &st) == 2);
    /* CR1's verdict lines classify as the step's */
    CHECK(bench_classify("cr1: FAIL: write path (control 64 wrong, pools 7 "
                         "of 7 wrong; slots 0 pass, 8 write, 0 sum, 0 other)")
          == BENCH_FAIL);
    CHECK(bench_classify("cr1: PASS (control 0 wrong, pools 0 of 7 wrong; "
                         "slots 8 pass, 0 write, 0 sum, 0 other)")
          == BENCH_PASS);
    CHECK(bench_classify("cr1 region 0x046000 wrong=64/64 (drift) first got "
                         "00000000 want 12345678") == BENCH_NONE);
    CHECK(bench_classify("cr1 variant addr16 at 0x046000 wrong=0/64")
          == BENCH_NONE);
}

/* The re-verify run with its restart step (sitting 3, 2026-10-26): the
 * pre-flight that finds the flags but no display driver (DONE) skips s05
 * and runs boot; boot is recorded only after a restart; a must-pass step
 * that couldn't run comes back after "retry" lines */
static const struct bench_step RB[] =
{
    { "pre", "Stage 5 flag", NULL, NULL, ran, NULL, { NULL },
      BENCH_F_PICKED, 0, 5 },
    { "s05", "Stages", "Watch", NULL, ran, "Did they look right?",
      { "Yes", "Garbled", NULL }, 0, 0, 300 },
    { "jobs", "Jobs", NULL, NULL, ran, NULL, { NULL }, BENCH_F_AUTO, 30, 0 },
    { "jvpu", "J_VPU", NULL, NULL, ran, NULL, { NULL }, BENCH_F_AUTO, 30, 0 },
    { "s6", "Clips", "Watch", NULL, ran, "How?", { "Smooth", "Stutter",
      NULL }, 0, 0, 0 },
    { "b13", "Warm swap", "Watch", NULL, ran, "Blink?", { "No", "Yes",
      NULL }, 0, 0, 0 },
    { "b5", "Window", "Watch", NULL, ran, "Whole?", { "Yes", "Broken",
      NULL }, 0, 0, 0 },
    { "boot", "Driver", "Restart", NULL, ran, NULL, { NULL },
      BENCH_F_REBOOTS, 0, 60 },
};
#define NRB ((int)(sizeof(RB) / sizeof(RB[0])))

static void t_restart(void)
{
    struct bench_run run;
    struct bench_state st;
    struct bench_rec rec;
    char ids[120], buf[200];
    int p, n;

    CHECK(bench_select(BENCH_MODE_REVERIFY, RB, NRB, &run) == 8);
    run_ids(&run, RB, ids, sizeof(ids));
    CHECK(!strcmp(ids, "pre,s05,boot,jobs,jvpu,b13,b5,s6"));
    CHECK(run.role[1] == BENCH_R_IFPRE && run.role[2] == BENCH_R_IFBOOT);
    /* the confirm screen's "if missing" time: s05 and boot */
    CHECK(bench_run_sum(&run, RB, 0, BENCH_SUM_IFPRE, &n) == 360 && n == 2);

    /* pre passed (flags and the display driver): neither runs */
    bench_state_init(&st);
    st.mode = BENCH_MODE_REVERIFY;
    st.merged = true;
    st.res[0] = BENCH_PASS;
    CHECK(bench_role_skip(&run, 1, &st) != NULL);
    CHECK(bench_role_skip(&run, 2, &st) != NULL);
    /* pre done (the flags, no display driver): the restart only */
    st.res[0] = BENCH_DONE;
    CHECK(bench_role_skip(&run, 1, &st) != NULL);
    CHECK(bench_role_skip(&run, 2, &st) == NULL);
    /* pre skipped (no flags): both */
    st.res[0] = BENCH_SKIP;
    CHECK(bench_role_skip(&run, 1, &st) == NULL);
    CHECK(bench_role_skip(&run, 2, &st) == NULL);

    /* the line: pre done, s05 skipped, boot done, the rest passed */
    for (p = 3; p < run.n; p++)
        st.res[run.k[p]] = BENCH_PASS;
    st.res[0] = BENCH_DONE;
    st.res[1] = BENCH_SKIP;
    st.res[7] = BENCH_DONE;
    CHECK(bench_reverify_ok(&run, &st));
    st.res[7] = BENCH_SKIP;             /* pre done: boot had to run */
    CHECK(!bench_reverify_ok(&run, &st));
    st.res[0] = BENCH_PASS;             /* pre passed: boot may skip */
    CHECK(bench_reverify_ok(&run, &st));

    /* the restart's record: read back while boot is open */
    bench_fmt_restart_asked(buf, sizeof(buf), "boot", 1000);
    CHECK(!strcmp(buf, "step=boot log=restart asked: wall=1000\n"));
    bench_state_init(&st);
    bench_replay(&st, RB, NRB, RV_HEAD);
    bench_replay(&st, RB, NRB, "step=pre result=done answer=- ms=1 note=pre: "
                 "stage 4 and 5 pass flags there; a restart comes first: x");
    bench_replay(&st, RB, NRB, "step=s05 result=skip answer=- ms=0 note=n");
    bench_replay(&st, RB, NRB, "step=boot result=start");
    bench_replay(&st, RB, NRB, "step=boot log=restart asked: wall=1000");
    CHECK(st.restart_asked == 1000 && st.dangling == 7);
    CHECK(bench_settle(&st, RB) == BENCH_START);
    /* no restart: this start began before the ask; a restart: after */
    CHECK(!bench_restart_seen(1000, 990));
    CHECK(!bench_restart_seen(1000, 997));
    CHECK(bench_restart_seen(1000, 998));   /* the RTC's slack */
    CHECK(bench_restart_seen(1000, 1030));
    CHECK(bench_restart_seen(0, 5));        /* no record: not checked */
    /* another step's log line, or the step started again, clears it */
    bench_replay(&st, RB, NRB, "step=boot result=start");
    CHECK(st.restart_asked == 0);
    bench_replay(&st, RB, NRB, "step=jobs log=wall=7");
    CHECK(st.restart_asked == 0);

    /* jobs couldn't run; the owner restarts and retries */
    {
        static const char *const f[] = {
            RV_HEAD,
            "step=pre result=pass answer=- ms=1 note=pre: PASS",
            "step=s05 result=skip answer=- ms=0 note=not needed",
            "step=boot result=skip answer=- ms=0 note=not needed",
            "step=jobs result=start",
            "step=jobs log=jobs: Not available (self-test rc 1): x",
            "step=jobs result=skip answer=- ms=0 note=jobs: Not available "
            "(self-test rc 1): x",
            "retry step=jobs",
            "retry step=boot",
            "step=boot result=start",
            "step=boot log=restart asked: wall=5000",
            NULL };

        bench_state_init(&st);
        for (p = 0; f[p]; p++)
            bench_replay(&st, RB, NRB, f[p]);
        CHECK(st.res[2] == BENCH_NONE && st.res[7] == BENCH_NONE);
        CHECK(st.skipped == 1 && st.passed == 1);  /* s05's only */
        CHECK(st.dangling == 7 && st.restart_asked == 5000);
        CHECK(bench_settle(&st, RB) == BENCH_START);
        bench_set(&st, 7, BENCH_DONE);          /* finish(), restart seen */
        bench_select(st.mode, RB, NRB, &run);
        CHECK(bench_run_next(&run, &st) == 3);  /* jobs again */
    }
    CHECK(bench_parse("retry step=jobs", &rec) == BENCH_K_RETRY &&
          !strcmp(rec.id, "jobs"));
    CHECK(bench_parse("retry", &rec) == BENCH_K_OTHER);
    bench_fmt_retry(buf, sizeof(buf), "jvpu");
    CHECK(!strcmp(buf, "retry step=jvpu\n"));
}

static void t_reverify(void)
{
    struct bench_run run;
    struct bench_state st;
    char buf[400];
    int p;

    hold_later = 0;             /* the roles as they were (4c) */
    bench_select(BENCH_MODE_REVERIFY, R, NR, &run);

    /* a fresh run: from the pre-flight */
    bench_state_init(&st);
    st.mode = BENCH_MODE_REVERIFY;
    CHECK(bench_run_next(&run, &st) == 0);

    /* the flag is there: s05 isn't needed, all pass, drift skipped */
    {
        static const char *const f[] = {
            RV_HEAD,
            "step=pre result=start",
            "step=pre result=pass answer=- ms=1 note=pre: stage 5 pass flag: PASS",
            "step=s05 result=skip answer=- ms=0 note=not needed",
            "step=jobs result=pass answer=- ms=1 note=",
            "step=jvpu result=pass answer=- ms=1 note=",
            "step=b3 result=skip answer=Skip ms=0 note=skipped (optional)",
            "step=drill2 result=skip answer=Skip ms=0 note=skipped (optional)",
            "step=s6 result=pass answer=Smooth ms=1 note=",
            "step=b13 result=pass answer=No ms=1 note=",
            "step=b5 result=pass answer=Yes ms=1 note=",
            NULL };

        replay_r(&st, f);
        CHECK(bench_run_next(&run, &st) == run.n);
        CHECK(bench_pre_res(&run, &st) == BENCH_PASS);
        CHECK(bench_role_skip(&run, 1, &st) != NULL);  /* s05 */
        CHECK(bench_role_skip(&run, 2, &st) == NULL);
        CHECK(bench_reverify_ok(&run, &st));
        CHECK(!bench_opt_passed(&run, &st));
        /* a plain image never earns the line */
        st.merged = false;
        CHECK(!bench_reverify_ok(&run, &st));
        st.merged = true;
        /* nor does the full run */
        st.mode = BENCH_MODE_FULL;
        CHECK(!bench_reverify_ok(&run, &st));
        st.mode = BENCH_MODE_REVERIFY;
        /* each needed step must pass: a skip or a fail costs the line */
        for (p = 0; p < run.n; p++)
        {
            uint8_t was = st.res[run.k[p]];

            if (run.role[p] != BENCH_R_NEED)
                continue;
            st.res[run.k[p]] = BENCH_SKIP;
            CHECK(!bench_reverify_ok(&run, &st));
            st.res[run.k[p]] = BENCH_DONE;
            CHECK(!bench_reverify_ok(&run, &st));
            st.res[run.k[p]] = was;
        }
        /* an optional step may skip, never fail */
        st.res[run.k[3]] = BENCH_FAIL;
        CHECK(!bench_reverify_ok(&run, &st));
        st.res[run.k[3]] = BENCH_HANG;
        CHECK(!bench_reverify_ok(&run, &st));
        st.res[run.k[3]] = BENCH_PASS;
        CHECK(bench_reverify_ok(&run, &st));
        CHECK(bench_opt_passed(&run, &st));
    }

    /* no flag: the pre-flight skips and s05 is offered; it must pass */
    {
        static const char *const f[] = {
            RV_HEAD,
            "step=pre result=skip answer=- ms=1 note=pre: Not available: no stage 5 pass flag",
            NULL };

        replay_r(&st, f);
        CHECK(bench_run_next(&run, &st) == 1);
        CHECK(bench_role_skip(&run, 1, &st) == NULL);  /* s05 runs */
        CHECK(!bench_reverify_ok(&run, &st));          /* not finished */
        bench_replay(&st, R, NR, "step=s05 result=fail answer=Garbled ms=1 note=");
        CHECK(bench_run_next(&run, &st) == 2);
        for (p = 2; p < run.n; p++)
            st.res[run.k[p]] = BENCH_PASS;
        CHECK(!bench_reverify_ok(&run, &st));
        st.res[run.k[1]] = BENCH_SKIP;
        CHECK(!bench_reverify_ok(&run, &st));
        st.res[run.k[1]] = BENCH_PASS;
        CHECK(bench_reverify_ok(&run, &st));
    }

    /* resume within the run: a hang in jvpu, then a manual drift step
       the owner left the session for */
    {
        static const char *const hang[] = {
            RV_HEAD,
            "step=pre result=pass answer=- ms=1 note=",
            "step=s05 result=skip answer=- ms=0 note=not needed",
            "step=jobs result=pass answer=- ms=1 note=",
            "step=b3 result=pass answer=Yes ms=0 note=",
            "step=jvpu result=start",
            NULL };
        static const char *const manual[] = {
            RV_HEAD,
            "step=pre result=pass answer=- ms=1 note=",
            "step=s05 result=skip answer=- ms=0 note=not needed",
            "step=jobs result=pass answer=- ms=1 note=",
            "step=b3 result=start",
            NULL };
        static const char *const later[] = {
            RV_HEAD,
            "step=pre result=pass answer=- ms=1 note=",
            "step=s05 result=skip answer=- ms=0 note=not needed",
            "step=jobs result=pass answer=- ms=1 note=",
            "step=b3 result=pass answer=Yes ms=0 note=",
            "resume after=b3",
            "step=jvpu result=pass answer=- ms=1 note=",
            "step=s6 result=pass answer=Smooth ms=1 note=",
            NULL };

        replay_r(&st, hang);
        CHECK(st.mode == BENCH_MODE_REVERIFY);
        CHECK(bench_run_next(&run, &st) == 4);         /* jvpu dangling */
        CHECK(bench_settle(&st, R) == BENCH_HANG);
        CHECK(bench_run_next(&run, &st) == 5);         /* on to b13 */
        CHECK(st.res[4] == BENCH_HANG && st.failed == 1);
        for (p = 5; p < run.n; p++)
            st.res[run.k[p]] = BENCH_PASS;
        CHECK(!bench_reverify_ok(&run, &st));          /* jvpu hung */

        replay_r(&st, manual);
        CHECK(bench_settle(&st, R) == BENCH_START);    /* ask its question */
        CHECK(st.dangling == 8 && bench_run_next(&run, &st) == 3);
        bench_set(&st, 8, BENCH_PASS);                 /* finish() */
        CHECK(bench_run_next(&run, &st) == 4);         /* jvpu */

        replay_r(&st, later);
        CHECK(bench_run_next(&run, &st) == 5);         /* b13: s6 is last */
        CHECK(st.passed == 5 && st.skipped == 1);
    }

    /* the verified line: the run sheet's format, no path */
    {
        int n = bench_fmt_verified(buf, sizeof(buf), 0x9af095b8u, 218756,
                                   "c08ccbe-M260924", "2026-10-24", false);

        CHECK(!strcmp(buf, "bcmvideo_jobs 9af095b8  stage 6 PASS (merged, "
                      "218756 bytes), B5 PASS, jobs self-test PASS, "
                      "J_VPU 3 PASS on the owner's 5.5G: re-verify session "
                      "2026-10-24, build c08ccbe-M260924"));
        CHECK(n > 0 && n < BENCH_LINE_MAX);
        CHECK(!strchr(buf, '/') && !strchr(buf, '\n'));
        n = bench_fmt_verified(buf, sizeof(buf), 0x00c0ffeeu, 1, "b", "", 2);
        CHECK(!strncmp(buf, "bcmvideo_jobs 00c0ffee  ", 24));
        CHECK(strstr(buf, ", drift B3 and drill 2 PASS on") != NULL);
        /* only the drift steps the run made: b3 alone since the trim */
        bench_fmt_verified(buf, sizeof(buf), 0x00c0ffeeu, 1, "b", "", 1);
        CHECK(strstr(buf, ", drift B3 PASS on") != NULL &&
              !strstr(buf, "drill"));
        CHECK(strstr(buf, "session -, build b") != NULL);
        CHECK(n < BENCH_LINE_MAX);
        /* verified.txt's reader (mkdevtest.py) splits name, fnv, note */
        CHECK(bench_hex("9af095b8") == 0x9af095b8u);
        CHECK(bench_hex("00C0FFEE x") == 0x00c0ffeeu);
        /* the clock token: verified.txt's clock150 line, its fnv second */
        n = bench_fmt_clock150(buf, sizeof(buf), 0x80bc8611u, 218692,
                               "799ff0f8d7-261026", "2026-10-25");
        CHECK(!strncmp(buf, "clock150 80bc8611  bcmvideo_jobs (218692 bytes): "
                       "stage 4c PASS and stage 6 PASS at 150 MHz", 90));
        CHECK(n > 0 && n < BENCH_LINE_MAX && !strchr(buf, '/'));
        CHECK(bench_hex(buf + 9) == 0x80bc8611u);
    }

    /* the clock token's steps: s4c and s6 both passed */
    {
        static const struct bench_step t[3] = {
            { .id = "s4b" }, { .id = "s4c" }, { .id = "s6" } };
        struct bench_state st;

        bench_state_init(&st);
        CHECK(!bench_clock150_ok(t, 3, &st));
        st.res[1] = BENCH_PASS;
        CHECK(!bench_clock150_ok(t, 3, &st));
        st.res[2] = BENCH_FAIL;
        CHECK(!bench_clock150_ok(t, 3, &st));
        st.res[2] = BENCH_PASS;
        CHECK(bench_clock150_ok(t, 3, &st));
        st.res[1] = BENCH_SKIP;
        CHECK(!bench_clock150_ok(t, 3, &st));
        CHECK(!bench_clock150_ok(t, 1, &st));       /* no s4c, s6 in the table */
    }

    /* the line on screen, in pieces */
    {
        const char *s = "bcmvideo_jobs 9af095b8  stage 6 PASS (merged, 1 bytes)";
        int l, total = 0, pieces = 0;

        CHECK(bench_wrap("short", 30) == 5);
        CHECK(bench_wrap("abcdefghij", 4) == 4);    /* no space: cut */
        CHECK(bench_wrap("ab cd ef", 5) == 3);      /* after the space */
        while (*s)
        {
            l = bench_wrap(s, 30);
            CHECK(l > 0 && l <= 30);
            total += l;
            pieces++;
            s += l;
        }
        CHECK(total == (int)strlen("bcmvideo_jobs 9af095b8  stage 6 PASS (merged, 1 bytes)") && pieces == 2);
    }
}

/* The full session's table, devbench.c-shaped: the pre-flight picked,
 * the re-verify checks among full-run-only steps, the boot step and the
 * reruns of the job checks on the display driver, then the drift */
static const struct bench_step F[] =
{
    { "pre", "Stage 5 flag", NULL, NULL, ran, NULL, { NULL },
      BENCH_F_PICKED, 0, 0 },
    { "s05", "Stages", "Watch", NULL, ran, "Did they look right?",
      { "Yes", "Garbled", NULL }, 0, 0, 0 },
    { "c1", "C1", NULL, NULL, ran, NULL, { NULL },
      BENCH_F_AUTO | BENCH_F_COP, 60, 0 },
    { "jobs", "Jobs", NULL, NULL, ran, NULL, { NULL }, BENCH_F_AUTO, 30, 0 },
    { "jvpu", "J_VPU", NULL, NULL, ran, NULL, { NULL }, BENCH_F_AUTO, 30, 0 },
    { "s6", "Clips", "Watch", NULL, ran, "How?", { "Smooth", "Stutter",
      NULL }, 0, 0, 0 },
    { "s7", "Video", "Watch", NULL, ran, "Controls?", { "Yes", "No",
      NULL }, 0, 0, 0 },
    { "b13", "Warm swap", "Watch", NULL, ran, "Blink?", { "No", "Yes",
      NULL }, 0, 0, 0 },
    { "b5", "Window", "Watch", NULL, ran, "Whole?", { "Yes", "Broken",
      NULL }, 0, 0, 0 },
    { "boot", "Boot path", "Restart now", NULL, ran, NULL, { NULL },
      BENCH_F_REBOOTS, 0, 0 },
    { "jobs2", "Jobs", NULL, NULL, ran, NULL, { NULL }, BENCH_F_AUTO, 30, 0 },
    { "jvpu2", "J_VPU", NULL, NULL, ran, NULL, { NULL }, BENCH_F_AUTO, 30, 0 },
    { "b3", "Drift", "Do it", NULL, NULL, "Smooth?", { "Yes", "Stutter",
      NULL }, 0, 0, 0 },
    { "drill2", "Drill 2", "Watch", NULL, ran, "What showed?",
      { "Drift or still cover", "Frozen", NULL }, 0, 0, 0 },
};
#define NF ((int)(sizeof(F) / sizeof(F[0])))

#define FULL_HEAD "session build=b date=2026-10-25 10:00 steps=13 " \
                  "image=218660 fnv=cd8e5d96 kind=merged"

/* the verified line a finished full session writes: "" for none */
static void full_line(const struct bench_state *st, char *buf, size_t n)
{
    struct bench_run picks;

    buf[0] = '\0';
    if (bench_full_ok(F, NF, st, &picks))
        bench_fmt_verified_in(buf, n, st->image_fnv, st->image_bytes,
                              "b", st->date, bench_opt_passed(&picks, st),
                              st->mode);
}

static void t_full(void)
{
    static const char *const f[] = {
        FULL_HEAD,
        "step=s05 result=pass answer=Yes ms=1 note=",
        "step=c1 result=pass answer=- ms=1 note=",
        "step=jobs result=pass answer=- ms=1 note=",
        "step=jvpu result=pass answer=- ms=1 note=",
        "step=s6 result=pass answer=Smooth ms=1 note=",
        "step=s7 result=fail answer=No ms=1 note=",
        "step=b13 result=pass answer=No ms=1 note=",
        "step=b5 result=pass answer=Yes ms=1 note=",
        "step=boot result=done answer=- ms=1 note=",
        "step=jobs2 result=pass answer=- ms=1 note=",
        "step=jvpu2 result=pass answer=- ms=1 note=",
        "step=b3 result=pass answer=Yes ms=1 note=",
        "step=drill2 result=pass answer=Drift_or_still_cover ms=1 note=",
        NULL };
    struct bench_state st;
    struct bench_run picks, run;
    char buf[400];
    int p, k;

    hold_later = 0;             /* the roles as they were (4c) */
    bench_state_init(&st);
    for (p = 0; f[p]; p++)
        bench_replay(&st, F, NF, f[p]);
    CHECK(st.mode == BENCH_MODE_FULL && st.merged);
    /* the full run never offers the pre-flight */
    bench_select(BENCH_MODE_FULL, F, NF, &run);
    CHECK(bench_run_next(&run, &st) == run.n && run.n == NF - 1);
    CHECK(st.res[0] == BENCH_NONE);

    /* every required step passed (a full-only step's fail doesn't
       matter): the exact line, drift and all */
    full_line(&st, buf, sizeof(buf));
    CHECK(!strcmp(buf, "bcmvideo_jobs cd8e5d96  stage 6 PASS (merged, "
                  "218660 bytes), B5 PASS, jobs self-test PASS, "
                  "J_VPU 3 PASS, drift B3 PASS on the owner's "
                  "5.5G: full session 2026-10-25, build b"));
    CHECK((int)strlen(buf) < BENCH_LINE_MAX);
    /* the picks are the re-verify run's */
    CHECK(bench_full_ok(F, NF, &st, &picks) && picks.n == 11);
    /* re-verify's own verdict never takes a full run */
    CHECK(!bench_reverify_ok(&picks, &st));

    /* one failed required step: no line, each in turn */
    for (p = 0; p < picks.n; p++)
    {
        uint8_t was;

        if (picks.role[p] == BENCH_R_PRE || picks.role[p] == BENCH_R_OPT)
            continue;
        k = picks.k[p];
        if (picks.role[p] == BENCH_R_ALSO)
        {
            /* s4b, s4c, b13 (the warm swap, not the merged image's path:
               owner, 2026-10-25): a failure never costs the line */
            was = st.res[k];
            st.res[k] = BENCH_FAIL;
            full_line(&st, buf, sizeof(buf));
            CHECK(buf[0] != '\0');
            st.res[k] = was;
            continue;
        }
        was = st.res[k];
        st.res[k] = BENCH_FAIL;
        if (!strcmp(F[k].id, "jobs") || !strcmp(F[k].id, "jvpu"))
        {
            /* the rerun on the display driver stands in */
            full_line(&st, buf, sizeof(buf));
            CHECK(buf[0] != '\0');
            st.res[k + 7] = BENCH_FAIL;     /* jobs2, jvpu2 */
            full_line(&st, buf, sizeof(buf));
            CHECK(buf[0] == '\0');
            st.res[k + 7] = BENCH_SKIP;
            full_line(&st, buf, sizeof(buf));
            CHECK(buf[0] == '\0');
            st.res[k + 7] = BENCH_PASS;
        }
        else
        {
            full_line(&st, buf, sizeof(buf));
            CHECK(buf[0] == '\0');
            st.res[k] = BENCH_SKIP;         /* s05 skipped: no pre-flight */
            full_line(&st, buf, sizeof(buf));
            CHECK(buf[0] == '\0');
        }
        st.res[k] = was;
    }
    CHECK(!strcmp(F[3 + 7].id, "jobs2") && !strcmp(F[4 + 7].id, "jvpu2"));
    /* part A's job checks skipped, the reruns passed: the line */
    st.res[3] = st.res[4] = BENCH_SKIP;
    full_line(&st, buf, sizeof(buf));
    CHECK(buf[0] != '\0');
    st.res[3] = st.res[4] = BENCH_PASS;

    /* an optional drift step skipped: still the line, without the drift */
    st.res[12] = BENCH_SKIP;
    full_line(&st, buf, sizeof(buf));
    CHECK(buf[0] && !strstr(buf, "drift"));
    CHECK(strstr(buf, "J_VPU 3 PASS on the owner's 5.5G: full session") != NULL);
    /* b3 failed: none */
    st.res[12] = BENCH_FAIL;
    full_line(&st, buf, sizeof(buf));
    CHECK(buf[0] == '\0');
    /* drill2 is no pick: its failure never costs the line, which never
       names it */
    st.res[12] = BENCH_PASS;
    st.res[13] = BENCH_FAIL;
    full_line(&st, buf, sizeof(buf));
    CHECK(buf[0] && !strstr(buf, "drill"));
    st.res[13] = BENCH_PASS;
    st.res[12] = BENCH_PASS;

    /* a plain image, or a session line without the image fields: none */
    st.merged = false;
    full_line(&st, buf, sizeof(buf));
    CHECK(buf[0] == '\0');
    st.merged = true;
    /* a build without the drift steps */
    CHECK(bench_full_ok(F, NF - 2, &st, &picks) && picks.n == 10);
    st.mode = BENCH_MODE_REVERIFY;
    CHECK(!bench_full_ok(F, NF, &st, &picks));
}

static void t_dog(void)
{
    struct bench_dog d;
    const long Q = 50, BOUND = 1000;

    memset(&d, 0, sizeof(d));
    CHECK(bench_dog_decide(&d, 100, false, -1, Q, BOUND) == BENCH_DOG_IDLE);
    bench_dog_arm(&d, 100, 9000);
    CHECK(bench_dog_decide(&d, 9100, true, 9099, Q, BOUND) == BENCH_DOG_WAIT);
    /* past the limit, the disk off: restart */
    CHECK(bench_dog_decide(&d, 9101, false, 9099, Q, BOUND) == BENCH_DOG_RESTART);
    CHECK(!d.armed);
    CHECK(bench_dog_decide(&d, 9102, false, 0, Q, BOUND) == BENCH_DOG_IDLE);
    /* on but quiet for Q ticks: restart */
    bench_dog_arm(&d, 0, 100);
    CHECK(bench_dog_decide(&d, 200, true, 150, Q, BOUND) == BENCH_DOG_RESTART);
    /* never touched (-1): quiet */
    bench_dog_arm(&d, 0, 100);
    CHECK(bench_dog_decide(&d, 200, true, -1, Q, BOUND) == BENCH_DOG_RESTART);
    /* busy: defer, then restart once it goes quiet */
    bench_dog_arm(&d, 0, 100);
    CHECK(bench_dog_decide(&d, 101, true, 100, Q, BOUND) == BENCH_DOG_DEFER);
    CHECK(bench_dog_decide(&d, 140, true, 139, Q, BOUND) == BENCH_DOG_DEFER);
    CHECK(d.armed && d.defer_from == 101);
    CHECK(bench_dog_decide(&d, 190, true, 139, Q, BOUND) == BENCH_DOG_RESTART);
    /* busy past the bound: give up, never restart */
    bench_dog_arm(&d, 0, 100);
    CHECK(bench_dog_decide(&d, 101, true, 101, Q, BOUND) == BENCH_DOG_DEFER);
    CHECK(bench_dog_decide(&d, 1100, true, 1099, Q, BOUND) == BENCH_DOG_DEFER);
    CHECK(bench_dog_decide(&d, 1101, true, 1100, Q, BOUND) == BENCH_DOG_GIVE_UP);
    CHECK(!d.armed);
    CHECK(bench_dog_decide(&d, 1102, false, 0, Q, BOUND) == BENCH_DOG_IDLE);
    /* near the top of the tick range */
    bench_dog_arm(&d, 0x7ffffff0L, 0x20);
    CHECK(bench_dog_decide(&d, (long)0x7ffffff8L, false, -1, Q, BOUND)
          == BENCH_DOG_WAIT);
}

static void t_vpu(void)
{
    uint32_t dec = 1u << (J_OP_DECODE - 0x20), other = 0x1F;

    CHECK(bench_vpu_expect(other) == 3);            /* merged */
    CHECK(bench_vpu_expect(other | dec) == 0x27);   /* display-only: + YUV */
    CHECK(bench_vpu_verdict(3, other) == BENCH_PASS);
    CHECK(bench_vpu_verdict(0x27, other | dec) == BENCH_PASS);
    CHECK(bench_vpu_verdict(7, other | dec) == BENCH_FAIL);   /* colour check off */
    CHECK(bench_vpu_verdict(0x27, other) == BENCH_FAIL);
    CHECK(bench_vpu_verdict(3, other | dec) == BENCH_FAIL);
    CHECK(bench_vpu_verdict(1, other) == BENCH_FAIL);   /* shift disagreed */
    CHECK(bench_vpu_verdict(0, other) == BENCH_FAIL);   /* never checked */
}

/* p19-battery: the battery rows' phase plan and verdicts */
static void t_power(void)
{
    CHECK(!bpw_phase_b(0) && bpw_phase_b(1) && !bpw_phase_b(2) &&
          bpw_phase_b(3));
    CHECK(bpw_idle_ok(60000, 60100, 0, 60000));
    CHECK(bpw_idle_ok(54000, 1, 0, 60000));        /* 90 % */
    CHECK(!bpw_idle_ok(53999, 1, 0, 60000));
    CHECK(bpw_idle_ok(66000, 1, 0, 60000));        /* 110 % */
    CHECK(!bpw_idle_ok(66001, 1, 0, 60000));
    CHECK(!bpw_idle_ok(0, 0, 0, 60000));           /* no tick: #3 no */
    CHECK(!bpw_idle_ok(60000, 0, 0, 60000));       /* never slept */
    CHECK(!bpw_idle_ok(60000, 60000, 1, 60000));   /* r0-r5 disturbed */
    CHECK(!bpw_idle_ok(0, 0, 0, 0));
    CHECK(bpw_lit_ok(true, 24) && !bpw_lit_ok(true, 30));
    CHECK(bpw_lit_ok(false, 30) && bpw_lit_ok(false, 80) &&
          !bpw_lit_ok(false, 24));
    /* slp: lost doorbells */
    CHECK(bpw_bells_lost(40, 40) == 0 && bpw_bells_lost(40, 55) == 0);
    CHECK(bpw_bells_lost(40, 38) == 2);
    CHECK(bpw_bells_ok(40, 40, 0) && bpw_bells_ok(40, 52, 0));
    CHECK(!bpw_bells_ok(40, 39, 0) && !bpw_bells_ok(40, 40, 1) &&
          !bpw_bells_ok(0, 5, 0));
    /* slp: doorbell wakes by FENCE latency */
    {
        uint16_t v[BPW_LAT_MAX];
        uint32_t i, n, under;

        for (i = 0; i < 9; i++)
            v[i] = (uint16_t)(90 - 10 * i);        /* 90 .. 10 */
        CHECK(bpw_quantile(v, 9, 50) == 50 && v[0] == 10 && v[8] == 90);
        CHECK(bpw_quantile(v, 9, 90) == 80 && bpw_quantile(v, 9, 100) == 90);
        CHECK(bpw_quantile(v, 0, 50) == 0);
        /* woken by the bell: all near the polling baseline */
        CHECK(bpw_lat_ok(40, 40, 60, 40));
        CHECK(bpw_lat_ok(40, 32, 230, 40));         /* 80 %, +190 us */
        CHECK(!bpw_lat_ok(40, 31, 60, 40));         /* under 80 % */
        CHECK(!bpw_lat_ok(40, 40, 241, 40));        /* median too slow */
        CHECK(!bpw_lat_ok(BPW_LAT_MIN - 1, BPW_LAT_MIN - 1, 40, 40));
        /* bell-blind: uniform over the 1 ms tick fails */
        for (n = 0, under = 0; n < 100; n++)
        {
            v[n] = (uint16_t)(n * 10 + 5);           /* 5 .. 995 us */
            under += v[n] < BPW_LAT_UNDER_US;
        }
        CHECK(!bpw_lat_ok(100, under, bpw_quantile(v, 100, 50), 40));
    }
    /* lit24ui: missed frames, per 1000, B against A */
    CHECK(bpw_miss_enough(30, 10) && !bpw_miss_enough(29, 10) &&
          !bpw_miss_enough(30, 9));
    CHECK(bpw_miss_kind_ok(0, 800, 0, 800) && bpw_miss_kind_ok(5, 500, 5, 500));
    CHECK(bpw_miss_kind_ok(10, 1000, 30, 1000));       /* A 10, B 30: +20 */
    CHECK(!bpw_miss_kind_ok(10, 1000, 31, 1000));
    CHECK(bpw_miss_kind_ok(0, 100, 1, 100));           /* 10 per 1000 */
    CHECK(!bpw_miss_kind_ok(0, 100, 3, 100));          /* 30 per 1000 */
    CHECK(bpw_miss_kind_ok(4, 100, 0, 0));             /* no B frames: no miss */
}

/* The owner's script: the wrap rule (scripts/bench_script.py's wrap() is
   the same), the confirm screens' sums, s4c's role, a leaving step */
static int wrap_all(const char *text, int width, char out[][40], int max)
{
    const char *p = text, *l;
    int len, n = 0;

    while (n < max && (p = bench_next_line(p, width, &l, &len)))
    {
        memcpy(out[n], l, (size_t)len);
        out[n][len] = '\0';
        n++;
    }
    return n;
}

static void t_script(void)
{
    char o[8][40];
    static const struct bench_step S[] =
    {
        { "pre", "Pre", NULL, NULL, ran, NULL, { NULL }, BENCH_F_PICKED, 0,
          5 },
        { "s05", "S", "x", NULL, ran, "q", { "Yes", NULL }, 0, 0, 300 },
        { "a1", "A1", NULL, NULL, ran, NULL, { NULL }, BENCH_F_AUTO, 90, 0 },
        { "a2", "A2", NULL, NULL, ran, NULL, { NULL }, BENCH_F_AUTO, 30, 0 },
        { "jobs", "J", NULL, NULL, ran, NULL, { NULL }, BENCH_F_AUTO, 30, 0 },
        { "jvpu", "V", NULL, NULL, ran, NULL, { NULL }, BENCH_F_AUTO, 30, 0 },
        { "s4c", "C", "x", NULL, ran, "q", { "Yes", "Bad", NULL }, 0, 0, 60 },
        { "s6", "6", "x", NULL, ran, "q", { "Yes", NULL }, 0, 0, 60 },
        { "b13", "13", "x", NULL, ran, "q", { "No", NULL }, 0, 0, 60 },
        { "b5", "5", "x", NULL, ran, "q", { "Yes", NULL }, 0, 0, 45 },
        { "drill2", "D", "x", NULL, ran, "q", { "Ok", "Bad", NULL },
          BENCH_F_LEAVES, 0, 120 },
        { "w2", "W", NULL, NULL, NULL, "q", { "No", "Yes", NULL },
          BENCH_F_GO, 0, 10 },
        { "o1", "O", "x", NULL, ran, NULL, { NULL },
          BENCH_F_OPTIONAL | BENCH_F_OWN, 0, 120 },
    };
    int ns = (int)(sizeof(S) / sizeof(S[0])), cnt, p;
    struct bench_run run;
    struct bench_state st;

    hold_later = 0;             /* the roles as they were (4c) */
    /* wrapping: at the last space in reach, '\n' breaks, "\n\n" an empty
       line, a word past the width cut, spaces at a break dropped */
    CHECK(wrap_all("short", 30, o, 8) == 1 && !strcmp(o[0], "short"));
    CHECK(wrap_all("aaa bbb ccc", 7, o, 8) == 2 && !strcmp(o[0], "aaa bbb")
          && !strcmp(o[1], "ccc"));
    CHECK(wrap_all("aaa bbbb", 7, o, 8) == 2 && !strcmp(o[0], "aaa") &&
          !strcmp(o[1], "bbbb"));
    CHECK(wrap_all("a\n\nb", 30, o, 8) == 3 && !o[1][0] &&
          !strcmp(o[2], "b"));
    CHECK(wrap_all("abcdefghij", 4, o, 8) == 3 && !strcmp(o[0], "abcd") &&
          !strcmp(o[2], "ij"));
    CHECK(wrap_all("ab   cd", 3, o, 8) == 2 && !strcmp(o[0], "ab") &&
          !strcmp(o[1], "cd"));
    CHECK(wrap_all("", 30, o, 8) == 0);
    CHECK(wrap_all("end\n", 30, o, 8) == 1);

    /* sums: the full run's main steps, wave 2, a hands-off block */
    CHECK(bench_step_secs(&S[2]) == 90 && bench_step_secs(&S[1]) == 300);
    bench_select(BENCH_MODE_FULL, S, ns, &run);
    CHECK(bench_run_sum(&run, S, 0, BENCH_SUM_MAIN, &cnt) ==
          300 + 90 + 30 + 30 + 30 + 60 + 60 + 60 + 45 + 120 + 10 && cnt == 11);
    CHECK(bench_run_sum(&run, S, 0, BENCH_SUM_OPTIONAL, &cnt) == 120 &&
          cnt == 1);
    CHECK(bench_run_sum(&run, S, 1, BENCH_SUM_BLOCK, &cnt) == 180 &&
          cnt == 4);
    CHECK(bench_run_sum(&run, S, 0, BENCH_SUM_BLOCK, &cnt) == 0 && cnt == 0);
    CHECK(bench_minutes(0) == 0 && bench_minutes(1) == 1 &&
          bench_minutes(60) == 1 && bench_minutes(61) == 2);
    /* re-verify: s05 counts only as the IFPRE extra; s4c is ALSO */
    bench_select(BENCH_MODE_REVERIFY, S, ns, &run);
    CHECK(bench_run_sum(&run, S, 0, BENCH_SUM_IFPRE, &cnt) == 300 &&
          cnt == 1);
    for (p = 0; p < run.n; p++)
        if (!strcmp(S[run.k[p]].id, "s4c"))
            break;
    /* the clock flags come straight after the hands-off checks, before
       any outside step (sitting 2's first try) */
    CHECK(p < run.n && run.role[p] == BENCH_R_ALSO &&
          !strcmp(S[run.k[p - 1]].id, "jvpu") &&
          !strcmp(S[run.k[p + 1]].id, "b13"));
    /* s4c's result never counts for the verified line */
    bench_state_init(&st);
    st.mode = BENCH_MODE_REVERIFY;
    st.merged = true;
    bench_set(&st, 0, BENCH_PASS);
    bench_set(&st, 1, BENCH_SKIP);
    bench_set(&st, 4, BENCH_PASS);
    bench_set(&st, 5, BENCH_PASS);
    bench_set(&st, 7, BENCH_PASS);
    bench_set(&st, 8, BENCH_PASS);
    bench_set(&st, 9, BENCH_PASS);
    bench_set(&st, 10, BENCH_SKIP);
    bench_set(&st, 6, BENCH_FAIL);
    CHECK(bench_reverify_ok(&run, &st));
    bench_set(&st, 6, BENCH_SKIP);
    CHECK(bench_reverify_ok(&run, &st));
    bench_set(&st, 9, BENCH_FAIL);
    CHECK(!bench_reverify_ok(&run, &st));

    /* a leaving step (drill 2) dangles on purpose: its question at the
       next press, never a hang */
    bench_state_init(&st);
    st.dangling = 10;
    CHECK(bench_settle(&st, S) == BENCH_START && st.next == 10);
    st.dangling = 2;
    CHECK(bench_settle(&st, S) == BENCH_HANG);
}

/* P19 PW4: the clock rows' verdicts and the [estimate] */
static void t_vclk(void)
{
    static const uint8_t mhz[12] =
        { 27, 46, 59, 74, 87, 102, 116, 123, 130, 140, 147, 150 };
    static const uint8_t lvl[12] =
        { 11, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
    uint32_t ms[12] = { 0 };

    CHECK(bvc_verify_ok(4, 4, 0, 200, 0, 0, 0));
    CHECK(!bvc_verify_ok(0, 4, 1, 200, 0, 0, 0));      /* fell back */
    CHECK(!bvc_verify_ok(4, 4, 0, 10, 0, 0, 0));       /* too few shown */
    CHECK(!bvc_verify_ok(4, 4, 0, 200, 1, 0, 0));      /* a decode error */
    CHECK(!bvc_verify_ok(4, 4, 0, 200, 0, 3, 0));
    CHECK(!bvc_verify_ok(4, 4, 0, 200, 0, 0, 1));      /* the image lost */
    CHECK(bvc_gov_ok(0, 0, 30000, 20000, 2, 1, 6, 20000, true));
    CHECK(!bvc_gov_ok(1, 0, 30000, 20000, 2, 1, 6, 20000, true)); /* a miss */
    CHECK(!bvc_gov_ok(0, 0, 60000, 20000, 2, 1, 6, 20000, true)); /* sync */
    CHECK(!bvc_gov_ok(0, 0, 30000, 20000, 3, 1, 6, 20000, true)); /* pacing */
    CHECK(bvc_gov_ok(0, 0, 30000, 20000, 1, 1, 6, 18000, true));  /* 20/min */
    CHECK(!bvc_gov_ok(0, 0, 30000, 20000, 1, 1, 7, 18000, true)); /* flaps */
    CHECK(!bvc_gov_ok(0, 0, 30000, 20000, 1, 1, 6, 20000, false)); /* race */
    CHECK(!bvc_gov_ok(0, 0, 30000, 20000, 1, 1, 0, 0, true));
    /* against its own 150 baseline (sitting 4c): clip2 skipped 33 and
       drifted 235 ms at fixed 150; the governor's 32 and 233 ms are no
       worse; more misses, or A/V past 150's plus the slack, still fail */
    CHECK(bvc_gov_ok(32, 33, 233378, 235000, 40, 49, 6, 24000, true));
    CHECK(!bvc_gov_ok(34, 33, 233378, 235000, 40, 49, 6, 24000, true));
    CHECK(!bvc_gov_ok(32, 33, 245001, 235000, 40, 49, 6, 24000, true));
    CHECK(bvc_gov_ok(0, 0, 67657, 71000, 29, 30, 6, 24000, true));
    /* the change rate is its own rule, never relative: sitting 4c's 25
       changes in 24 s (clip1) and 28 (clip2) still fail */
    CHECK(!bvc_gov_ok(0, 0, 67657, 71000, 29, 30, 25, 24000, true));
    /* the estimate: polling at 150 MHz, level 15 is the model's 27.1 mA
       (scripts/vclk_gov_sim.py --poll, fixed150); at 87 MHz, level 8 */
    CHECK(bvc_est_ma100(ms, mhz, lvl, 12) == 0);
    ms[11] = 1000;
    CHECK(bvc_est_ma100(ms, mhz, lvl, 12) >= 2700 &&
          bvc_est_ma100(ms, mhz, lvl, 12) <= 2715);
    ms[11] = 0;
    ms[4] = 1000;
    CHECK(bvc_est_ma100(ms, mhz, lvl, 12) >= 930 &&
          bvc_est_ma100(ms, mhz, lvl, 12) <= 960);
    ms[11] = 1000;          /* half and half: the mean */
    CHECK(bvc_est_ma100(ms, mhz, lvl, 12) >= 1810 &&
          bvc_est_ma100(ms, mhz, lvl, 12) <= 1840);
}

/* Sitting 4c's runner (devbench.h): the re-verify order, the needs and
 * the auto-skip, the clean finish.  A table with every pick, in the
 * devbench.c table's own order (the picks reorder it) */
#define M(id) { id, id, NULL, NULL, ran, NULL, { NULL }, 0, 0, 10 }
static const struct bench_step SR[] =
{
    { "pre", "Start-up check", NULL, NULL, ran, NULL, { NULL },
      BENCH_F_PICKED, 0, 5 },
    M("s05"), M("s4b"), M("s4c"), M("c1"), M("jobs"), M("jvpu"), M("s6"),
    M("s7"), M("vfeed"), M("vbv"), M("vgov"),
    { "vplay", "Play a video", NULL, NULL, NULL, "q", { "Yes", "No", NULL },
      BENCH_F_PICKED, 0, 60 },
    M("b13"), M("b5"), M("boot"), M("ucop"), M("uck1"), M("uck2"),
    M("ufeel"), M("upanel"), M("ucop2"), M("uviz"), M("respanel"), M("b3"),
};
#undef M
#define NSR ((int)(sizeof(SR) / sizeof(SR[0])))

static int pos_of(const struct bench_run *run, const char *id)
{
    int p;

    for (p = 0; p < run->n; p++)
        if (!strcmp(SR[run->k[p]].id, id))
            return p;
    return -1;
}

/* every pick passes (or is not needed), as a clean sitting would */
static void steer_all_pass(const struct bench_run *run, struct bench_state *st)
{
    int p;

    bench_state_init(st);
    st->mode = BENCH_MODE_REVERIFY;
    st->merged = true;
    for (p = 0; p < run->n; p++)
        bench_set(st, run->k[p], run->role[p] == BENCH_R_IFPRE ||
                  run->role[p] == BENCH_R_IFBOOT ? BENCH_SKIP : BENCH_PASS);
}

/* the session loop's auto-skip, as devbench.c runs it: each position in
   order, skipped (and recorded) when its needs failed; returns how many */
static int steer_walk(const struct bench_run *run, struct bench_state *st,
                      int from, char *ids, size_t n)
{
    int p, c = 0, dep;

    ids[0] = '\0';
    for (p = from; p < run->n; p++)
    {
        dep = bench_dep_skip(run, SR, st, p);
        if (dep < 0)
            continue;
        if (c++)
            strncat(ids, ",", n - strlen(ids) - 1);
        strncat(ids, SR[run->k[p]].id, n - strlen(ids) - 1);
        st->res[run->k[p]] = BENCH_SKIP;
    }
    return c;
}

static void t_steer(void)
{
    struct bench_run run;
    struct bench_state st;
    char ids[400], buf[200];
    int p, d;

    hold_later = 1;             /* sitting 4d's run */
    /* the order: basic to advanced, independent groups first; sitting
       4d's held picks (BENCH_R_LATER: b3, s4b, s4c, vfeed, s7, vbv, vgov)
       stay in the table but are never selected */
    CHECK(bench_select(BENCH_MODE_REVERIFY, SR, NSR, &run) == NSR - 7);
    run_ids(&run, SR, ids, sizeof(ids));
    CHECK(!strcmp(ids, "pre,s05,boot,jobs,respanel,uviz,jvpu,c1,"
                  "b13,b5,s6,vplay,ucop,uck1,uck2,ufeel,"
                  "upanel,ucop2"));
    {
        static const char *const held[] = { "b3", "s4b", "s4c", "vfeed",
                                            "s7", "vbv", "vgov" };
        int h, q, np = (int)(sizeof(bench_reverify_picks) /
                             sizeof(bench_reverify_picks[0]));

        for (h = 0; h < (int)(sizeof(held) / sizeof(held[0])); h++)
        {
            for (q = 0; q < np && strcmp(bench_reverify_picks[q].id,
                                         held[h]); q++)
                ;
            CHECK(q < np && (bench_reverify_picks[q].role & BENCH_R_HELD));
            CHECK(pos_of(&run, held[h]) < 0);
        }
    }
    /* every need comes before the step that needs it */
    for (p = 0; p < run.n; p++)
        for (d = 0; d < BENCH_NEEDS; d++)
            if (run.need[p][d] >= 0)
            {
                int q;

                for (q = 0; q < p && run.k[q] != run.need[p][d]; q++)
                    ;
                CHECK(q < p);
            }
    /* the visualizer, the panel and the drift on the chip straight after
       the job self-test, before anything that swaps the chip's program
       (jvpu's and c1's probe image, the 4b/4c stages, the labs, the stage
       plays: Apple's vmcs after each); the panel before the visualizer */
    CHECK(pos_of(&run, "respanel") == pos_of(&run, "jobs") + 1);
    CHECK(pos_of(&run, "uviz") < pos_of(&run, "jvpu"));
    CHECK(pos_of(&run, "respanel") < pos_of(&run, "uviz"));
    CHECK(pos_of(&run, "uviz") < pos_of(&run, "c1"));
    /* video before the second core; the Videos play after the bench's
       own plays, before ucop's restart */
    CHECK(pos_of(&run, "s6") < pos_of(&run, "vplay"));
    CHECK(pos_of(&run, "vplay") < pos_of(&run, "ucop"));
    CHECK(pos_of(&run, "ucop2") == run.n - 1);
    /* the needs, as table indices: vplay needs s6 alone (the image's
       clock150 token opens the fast clock, not s4c's flag) */
    p = pos_of(&run, "vplay");
    CHECK(run.need[p][0] == bench_find(SR, NSR, "s6") &&
          run.need[p][1] == -1);
    p = pos_of(&run, "b13");
    CHECK(run.need[p][0] == -1 && run.need[p][1] == -1);
    /* the full run has no needs */
    bench_state_init(&st);
    bench_select(BENCH_MODE_FULL, SR, NSR, &run);
    for (p = 0; p < run.n; p++)
        CHECK(run.need[p][0] == -1 && bench_dep_skip(&run, SR, &st, p) < 0);
    bench_select(BENCH_MODE_REVERIFY, SR, NSR, &run);

    /* a clean sitting: nothing skips by itself, the line is earned */
    steer_all_pass(&run, &st);
    for (p = 0; p < run.n; p++)
        CHECK(bench_dep_skip(&run, SR, &st, p) < 0);
    /* the line, from jobs, jvpu, b5 and s6; no clock token without s4c
       (sitting 4c's token is already in verified.txt) */
    CHECK(bench_reverify_ok(&run, &st) && !bench_clock150_ok(SR, NSR, &st));
    CHECK(!bench_opt_passed(&run, &st));    /* no drift step: none named */

    /* s6 shows no picture: the rest of video and the Videos play skip,
       one sentence; the second-core group still runs */
    steer_all_pass(&run, &st);
    for (p = pos_of(&run, "s6"); p < run.n; p++)
        st.res[run.k[p]] = BENCH_NONE;
    st.res[bench_find(SR, NSR, "s6")] = BENCH_FAIL;
    CHECK(steer_walk(&run, &st, pos_of(&run, "s6") + 1, ids, sizeof(ids)) == 1);
    CHECK(!strcmp(ids, "vplay"));
    d = bench_dep_skip(&run, SR, &st, pos_of(&run, "vplay"));
    CHECK(d == bench_find(SR, NSR, "s6"));
    CHECK(strstr(bench_dep_why(SR, d), "Skipping the other video steps") ==
          bench_dep_why(SR, d));
    CHECK(!strchr(bench_dep_why(SR, d), '\n'));
    /* the note the file gets, and ingest's reading of it */
    bench_fmt_autoskip(buf, sizeof(buf), SR, &st, d);
    CHECK(!strcmp(buf, "auto-skip: needs s6 (fail)"));
    CHECK(bench_is_autoskip(buf) && !bench_is_autoskip("not needed") &&
          !bench_is_autoskip(NULL));
    /* no line (s6 is a must-pass), no clock token */
    CHECK(!bench_reverify_ok(&run, &st) && !bench_clock150_ok(SR, NSR, &st));
    /* a hang blocks as a failure does */
    st.res[bench_find(SR, NSR, "s6")] = BENCH_HANG;
    CHECK(bench_dep_skip(&run, SR, &st, pos_of(&run, "vplay")) ==
          bench_find(SR, NSR, "s6"));

    /* the job self-test can't run (and the owner carried on): the jobs
       group skips; the clock flags, the labs, video and the second core
       still run */
    steer_all_pass(&run, &st);
    for (p = pos_of(&run, "jobs"); p < run.n; p++)
        st.res[run.k[p]] = BENCH_NONE;
    st.res[bench_find(SR, NSR, "jobs")] = BENCH_SKIP;
    st.res[bench_find(SR, NSR, "jvpu")] = BENCH_PASS;
    for (p = pos_of(&run, "c1"); p < run.n; p++)
        st.res[run.k[p]] = BENCH_PASS;
    CHECK(steer_walk(&run, &st, pos_of(&run, "respanel"), ids,
                     sizeof(ids)) == 2);
    CHECK(!strcmp(ids, "respanel,uviz"));
    CHECK(strstr(bench_dep_why(SR, bench_find(SR, NSR, "jobs")), "jobs"));
    /* jvpu runs after group 2: its failure skips nothing (no line) */
    steer_all_pass(&run, &st);
    st.res[bench_find(SR, NSR, "jvpu")] = BENCH_FAIL;
    for (p = pos_of(&run, "c1"); p < run.n; p++)
        st.res[run.k[p]] = BENCH_NONE;
    CHECK(steer_walk(&run, &st, pos_of(&run, "c1"), ids, sizeof(ids)) == 0);
    CHECK(!bench_reverify_ok(&run, &st));

    /* the pre-flight's own "not needed" skips block nothing; s05 failing
       ends the session: every later step skips, its sentence says so */
    steer_all_pass(&run, &st);
    CHECK(st.res[bench_find(SR, NSR, "s05")] == BENCH_SKIP);
    for (p = 0; p < run.n; p++)
        CHECK(bench_dep_skip(&run, SR, &st, p) < 0);
    for (p = pos_of(&run, "s05"); p < run.n; p++)
        st.res[run.k[p]] = BENCH_NONE;
    st.res[bench_find(SR, NSR, "pre")] = BENCH_SKIP;
    st.res[bench_find(SR, NSR, "s05")] = BENCH_FAIL;
    CHECK(steer_walk(&run, &st, pos_of(&run, "s05") + 1, ids, sizeof(ids)) ==
          run.n - 2);
    d = bench_dep_skip(&run, SR, &st, pos_of(&run, "ucop2"));
    CHECK(d == bench_find(SR, NSR, "s05"));
    CHECK(!strncmp(bench_dep_why(SR, d), "That's all for today", 20));
    CHECK(bench_run_next(&run, &st) == run.n);     /* the session is over */

    /* c1 failed: ucop skips; then the second-core rows skip after ucop,
       whose root is c1 (one sentence, c1's) */
    steer_all_pass(&run, &st);
    for (p = pos_of(&run, "ucop"); p < run.n; p++)
        st.res[run.k[p]] = BENCH_NONE;
    st.res[bench_find(SR, NSR, "c1")] = BENCH_FAIL;
    CHECK(steer_walk(&run, &st, pos_of(&run, "ucop"), ids, sizeof(ids)) == 6);
    d = bench_dep_skip(&run, SR, &st, pos_of(&run, "uck1"));
    CHECK(d == bench_find(SR, NSR, "ucop"));
    CHECK(bench_dep_root(&run, SR, &st, d) == bench_find(SR, NSR, "c1"));
    CHECK(bench_dep_root(&run, SR, &st, bench_find(SR, NSR, "c1")) ==
          bench_find(SR, NSR, "c1"));
    bench_fmt_autoskip(buf, sizeof(buf), SR, &st, d);
    CHECK(!strcmp(buf, "auto-skip: needs ucop (skip)"));
    /* a gate this table has no sentence for: the general one */
    CHECK(bench_dep_why(SR, bench_find(SR, NSR, "b13")) ==
          bench_dep_why(SR, -1));
    /* ucop's restart left the UI on the CPU (recorded failed): the
       second-core rows skip, the line still stands (all ALSO) */
    steer_all_pass(&run, &st);
    for (p = pos_of(&run, "uck1"); p < run.n; p++)
        st.res[run.k[p]] = BENCH_NONE;
    st.res[bench_find(SR, NSR, "ucop")] = BENCH_FAIL;
    CHECK(steer_walk(&run, &st, pos_of(&run, "uck1"), ids, sizeof(ids)) == 5);
    CHECK(!strcmp(ids, "uck1,uck2,ufeel,upanel,ucop2"));
    CHECK(bench_reverify_ok(&run, &st));

    /* the clean finish: a must-pass failing never ends the session by
       itself (b5 has no dependants: everything after it still runs) */
    steer_all_pass(&run, &st);
    for (p = pos_of(&run, "s6"); p < run.n; p++)
        st.res[run.k[p]] = BENCH_NONE;
    st.res[bench_find(SR, NSR, "b5")] = BENCH_FAIL;
    CHECK(steer_walk(&run, &st, pos_of(&run, "s6"), ids, sizeof(ids)) == 0);
    CHECK(!bench_reverify_ok(&run, &st));
    /* the held drift (b3) skipped or not run: the line still stands */
    steer_all_pass(&run, &st);
    st.res[bench_find(SR, NSR, "b3")] = BENCH_SKIP;
    CHECK(bench_reverify_ok(&run, &st) && !bench_opt_passed(&run, &st));

    /* a resume carries the auto-skips: they are results like any other */
    bench_state_init(&st);
    bench_replay(&st, SR, NSR, "session build=b date=2026-10-27 10:00 "
                 "steps=25 mode=reverify image=1 fnv=00000001 kind=merged");
    bench_replay(&st, SR, NSR, "step=s6 result=fail answer=No ms=1 note=");
    bench_replay(&st, SR, NSR, "step=vfeed result=skip answer=- ms=0 "
                 "note=auto-skip: needs s6 (fail)");
    CHECK(st.res[bench_find(SR, NSR, "vfeed")] == BENCH_SKIP &&
          st.skipped == 1 && st.failed == 1);
}

/* The resume's cross-check with bcmtest.log (bench_logscan): sitting
   4c's file lost b13's start and result (here standing in for vbv); the
   log's "auto:" line, inside this session's marker, gives it back, with
   its answer and time, so the step is never run again.  Another
   session's lines, a step's retry, and lines before the marker don't
   count */
static void t_logscan(void)
{
    static struct bench_logscan ls;
    struct bench_state st;
    char buf[BENCH_LINE_MAX + 64];
    struct bench_rec r;
    int k, kb = bench_find(T, N, "b13"), kp = bench_find(T, N, "pb1");
    static const char *const file[] =
    {
        "session build=b date=2026-10-26 20:27 steps=9",
        "step=s05 result=pass answer=- ms=10 note=",
        "step=c1 result=pass answer=- ms=10 note=",
        "step=c2 result=pass answer=- ms=10 note=",
        "step=pb1 result=pass answer=- ms=10 note=",
        /* b13's start and result: lost */
        NULL
    };
    static const char *const log[] =
    {
        "   12 auto: b13 pass answer=No ms=99 an earlier session",
        "bench file 20261026-100000-x.txt",
        "   13 auto: b13 fail answer=Yes, badly ms=98 another session",
        "bench file 20261026-202750-b-261026.txt",
        "118396 auto: s05 pass answer=- ms=10 ",
        "  1488 auto: pb1 skip answer=- ms=0 retried below",
        "  1489 auto: pb1 retry",
        "326384 auto: b13 fail answer=Yes, briefly ms=16340 Warm swap: FAIL",
        "  2000 auto: drill1 no restart seen (asked at 5)",
        NULL
    };
    const char *const *l;

    bench_state_init(&st);
    for (l = file; *l; l++)
        bench_replay(&st, T, N, *l);
    CHECK(st.res[kb] == BENCH_NONE);
    bench_logscan_init(&ls, "20261026-202750-b-261026.txt");
    for (l = log; *l; l++)
        bench_logscan_line(&ls, T, N, *l);
    CHECK(ls.got[kb].res == BENCH_FAIL && ls.got[kb].ans == 1 &&
          ls.got[kb].ms == 16340);
    CHECK(ls.got[kp].res == BENCH_NONE);        /* the retry forgot it */
    k = bench_logscan_missing(&ls, &st, T, N, 0, buf, sizeof(buf));
    CHECK(k == kb);
    CHECK(bench_parse(buf, &r) == BENCH_K_RESULT && r.res == BENCH_FAIL &&
          !strcmp(r.answer, "Yes,_briefly") && r.ms == 16340 &&
          strstr(buf, BENCH_LOG_NOTE));
    bench_replay(&st, T, N, buf);
    CHECK(st.res[kb] == BENCH_FAIL && st.next == kb + 1);
    CHECK(bench_logscan_missing(&ls, &st, T, N, k + 1, buf,
                                sizeof(buf)) < 0);
    /* s05's result was in the file: the log's never written over it */
    CHECK(bench_logscan_missing(&ls, &st, T, N, 0, buf, sizeof(buf)) < 0);
    /* without the marker nothing counts */
    bench_logscan_init(&ls, "20261026-999999-b-261026.txt");
    for (l = log; *l; l++)
        bench_logscan_line(&ls, T, N, *l);
    CHECK(ls.got[kb].res == BENCH_NONE);
}

int main(void)
{
    t_script();
    t_power();
    t_vclk();
    t_classify();
    t_lines_verdict();
    t_verdict();
    t_format();
    t_resume();
    t_logscan();
    t_dog();
    t_vpu();
    t_modes();
    t_reverify();
    t_restart();
    t_portcheck();
    t_full();
    t_steer();
    if (fails)
        printf("%d failed\n", fails);
    return fails != 0;
}
