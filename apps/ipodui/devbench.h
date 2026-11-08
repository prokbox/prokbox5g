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

#ifndef _IPODUI_DEVBENCH_H_
#define _IPODUI_DEVBENCH_H_

/* The guided bench session (Developer > "Bench session"): one row
 * that runs the developer and hardware checks needing no owner first
 * (part A, unattended), then walks the owner through the rest (part B),
 * one short instruction per step and a question with wheel answers at
 * the end of each step that needs a human judgement.  Everything goes
 * into one results file a session, written as it goes, which is also
 * the resume record after a hang or a planned restart.
 * docs/research/bcm2722/bench-p19.md, "Session plan", lists the steps.
 *
 * This header is the pure part: the step table's shape, the results
 * file's line format (written and parsed), the replay that turns a file
 * back into where to resume, and the verdicts.  No Rockbox calls, so
 * tests/autochecks runs it on the host.  devbench.c is the device side.
 *
 * The results file, one item a line, space-separated key=value fields;
 * note= and log= take the rest of the line:
 *
 *   session build=<id> date=<yyyy-mm-dd hh:mm> steps=<n>
 *   step=<id> result=start
 *   step=<id> log=<a log line the step wrote>
 *   step=<id> result=<pass|fail|skip|done|hang> answer=<token|-> ms=<n> note=<text>
 *   resume after=<id>
 *   retry step=<id>          (the owner restarts to run a step again)
 *   verified <a bcmfw/verified.txt line>     (a passed run, either mode)
 *   end passed=<n> failed=<n> skipped=<n>
 *
 * The session line adds image=, fnv= and kind=, and a picked run's
 * (re-verify, port write check) mode= too ("Sessions" below).
 *
 * A step that skipped by itself, because a step it needs failed (the
 * picks' need[], below), is a skip with the note "auto-skip: needs <id>
 * (<that step's result>)" (bench_fmt_autoskip).
 *
 * An answer token is the answer's text with spaces as '_'.  "done": the
 * step ran and has no verdict of its own (informational); it counts with
 * the passed.  "hang": found on resume, a step started and never ended.
 *
 * Adding steps (the hook for other sessions' code): an entry in
 * devbench.c's table, compiled in under the feature's flag (as the 19-2
 * drift entries are, under HAVE_BCM_JOBS).  An entry with fn == NULL is
 * manual: the instruction, then the owner does it, then the question.
 * Wiring a function in later changes nothing else. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../bcmfw/jobs.h"         /* J_VPU_*, J_OP_DECODE */

/* Step results (the file's result=) */
enum bench_res
{
    BENCH_NONE = 0,
    BENCH_START,
    BENCH_PASS,
    BENCH_FAIL,
    BENCH_SKIP,
    BENCH_DONE,
    BENCH_HANG,
    BENCH_N_RES
};

static const char *const bench_res_name[BENCH_N_RES] =
{
    "none", "start", "pass", "fail", "skip", "done", "hang"
};

#define BENCH_ANSWERS       4       /* the step's own answers; "Skip" is added */

/* Step flags */
#define BENCH_F_AUTO        0x01    /* part A: no owner, watchdog on */
#define BENCH_F_COP         0x02    /* uses the COP worker: skipped after a
                                       COP step hung (the worker is stuck) */
#define BENCH_F_REBOOTS     0x04    /* ends in a restart the owner makes:
                                       resume records it done, not hung */
#define BENCH_F_GO          0x08    /* a go question: answers[1] (yes) runs
                                       the BENCH_F_OPTIONAL steps after it */
#define BENCH_F_OPTIONAL    0x10    /* only after a go question's yes */
#define BENCH_F_OWN         0x20    /* keeps the row's own confirm screens
                                       (no batch bypass): wave 2 */
#define BENCH_F_PICKED      0x40    /* only in a picked session (the
                                       re-verify run), never the full one */
#define BENCH_F_LEAVES      0x80    /* its function arms something the
                                       owner then watches outside the
                                       session: the session leaves after
                                       it, and the next press asks the
                                       question (as for a manual step) */

#define BENCH_MAX_STEPS     96      /* the table's bound (devbench.c checks; 4g: the devtest table passed 64;
                                       4gh: 96 for the four row groups to come; at most 127, need[] is int8_t) */

/* The owner's script (bench-p19.md "Owner's script", generated from this
 * table by scripts/bench_script.py): say and say2 are the step's start
 * screen, word-wrapped to the screen's width ('\n' breaks a line, "\n\n"
 * leaves an empty one); devbench.c adds the MENU and SELECT lines.  say:
 * what happens, for how long, what is normal; say2: the buttons during it
 * and what comes after.  ask is the question's whole text, the answers'
 * meanings included. */
struct bench_step
{
    const char *id;                 /* short token, unique: "pb1", "b13" */
    const char *title;              /* the screen's title */
    const char *say;                /* the start screen (NULL: none) */
    const char *say2;               /* its second part (NULL: none) */
    int (*fn)(void);                /* enum bench_res; NULL: manual */
    const char *ask;                /* the question (NULL: none) */
    const char *answers[BENCH_ANSWERS]; /* [0] the good one; NULL ends */
    uint8_t flags;
    uint16_t limit_s;               /* BENCH_F_AUTO's watchdog, seconds */
    uint16_t budget_s;              /* the expected time, seconds, for the
                                       confirm screens' totals (0: limit_s) */
    uint16_t hands_s;               /* of that, seconds of hands-off wait
                                       inside a step that is not AUTO (the
                                       owner's time is the rest) */
};

/* A step's expected seconds */
static inline unsigned bench_step_secs(const struct bench_step *s)
{
    return s->budget_s ? s->budget_s : s->limit_s;
}

/* The next screen line of text for a screen `width` characters wide: its
 * start in *line and length in *len; the return is where the following
 * one starts, or NULL when the text is done.  A line breaks after the
 * last space in reach (or at the width, in a word that long); '\n' breaks
 * a line, so "\n\n" leaves an empty one.  Spaces at a break are dropped.
 * scripts/bench_script.py's wrap() is the same rule. */
static inline const char *bench_next_line(const char *p, int width,
                                          const char **line, int *len)
{
    int n = 0, k;

    if (!p || !*p)
        return NULL;
    while (p[n] && p[n] != '\n')
        n++;
    *line = p;
    if (n <= width)
    {
        *len = n;
        return p[n] ? p + n + 1 : p + n;
    }
    for (k = width; k > 0 && p[k] != ' '; k--)
        ;
    if (k == 0)
        k = width;
    *len = k;
    while (*len > 0 && p[*len - 1] == ' ')
        (*len)--;
    p += k;
    while (*p == ' ')
        p++;
    return p;
}

/* ---- Verdicts ------------------------------------------------------- */

/* A line a step wrote (its log line or its summary line), classified:
 * BENCH_FAIL if it reports a failure, BENCH_SKIP if the row wasn't
 * available, BENCH_PASS if it reports a pass, else BENCH_NONE */
static inline bool bench_word(const char *s, const char *w)
{
    size_t n = strlen(w);
    const char *p = s;

    while ((p = strstr(p, w)))
    {
        bool before = p == s || !((p[-1] >= 'A' && p[-1] <= 'Z') ||
                                  (p[-1] >= 'a' && p[-1] <= 'z'));
        bool after = !((p[n] >= 'A' && p[n] <= 'Z') ||
                       (p[n] >= 'a' && p[n] <= 'z'));
        if (before && after)
            return true;
        p += n;
    }
    return false;
}

static inline enum bench_res bench_classify(const char *line)
{
    if (bench_word(line, "FAIL") || bench_word(line, "FAILED") ||
        strstr(line, "(run void)") || bench_word(line, "hang"))
        return BENCH_FAIL;
    if (strstr(line, "Not available") || strstr(line, "Not run") ||
        bench_word(line, "Locked"))
        return BENCH_SKIP;
    if (bench_word(line, "PASS") || strstr(line, "All stages passed"))
        return BENCH_PASS;
    return BENCH_NONE;
}

/* The lines' tally: any failure fails; else a skip with no pass skips;
 * else any pass passes; else done (it ran, no verdict) */
struct bench_tally_lines
{
    unsigned pass, fail, skip;
};

static inline void bench_lines_add(struct bench_tally_lines *t,
                                   enum bench_res r)
{
    if (r == BENCH_PASS) t->pass++;
    else if (r == BENCH_FAIL) t->fail++;
    else if (r == BENCH_SKIP) t->skip++;
}

static inline enum bench_res bench_lines_verdict(const struct bench_tally_lines *t)
{
    if (t->fail)
        return BENCH_FAIL;
    if (t->skip && !t->pass)
        return BENCH_SKIP;
    if (t->pass)
        return BENCH_PASS;
    return BENCH_DONE;
}

/* The step's result with the owner's answer: a = -1 none asked, 0 the
 * good answer, 1..BENCH_ANSWERS-1 the others, BENCH_ANSWERS "Skip" (or
 * MENU).  A bad answer fails the step; "Skip" on a manual step skips it;
 * a go question is done whatever the answer. */
static inline enum bench_res bench_verdict(const struct bench_step *s,
                                           enum bench_res automatic, int a)
{
    if (s->flags & BENCH_F_GO)
        return a == BENCH_ANSWERS ? BENCH_SKIP : BENCH_DONE;
    if (automatic == BENCH_FAIL || automatic == BENCH_HANG)
        return automatic;
    if (a > 0 && a < BENCH_ANSWERS)
        return BENCH_FAIL;
    if (a == BENCH_ANSWERS && (!s->fn || automatic == BENCH_SKIP ||
                               automatic == BENCH_NONE))
        return BENCH_SKIP;
    if (a == 0)
        return automatic == BENCH_SKIP || automatic == BENCH_NONE ||
               automatic == BENCH_DONE ? BENCH_PASS : automatic;
    return automatic == BENCH_NONE ? BENCH_DONE : automatic;
}

static inline int bench_n_answers(const struct bench_step *s)
{
    int n = 0;

    while (n < BENCH_ANSWERS && s->answers[n])
        n++;
    return n;
}

/* ---- The file's lines ------------------------------------------------ */

#define BENCH_LINE_MAX      240
#define BENCH_TOKEN_MAX     32

/* text into a token: spaces as '_', '=' and controls dropped */
static inline void bench_token(char *out, size_t n, const char *in)
{
    size_t k = 0;

    if (!in || !*in)
        in = "-";
    for (; *in && k + 1 < n; in++)
    {
        unsigned char c = (unsigned char)*in;

        if (c == ' ')
            out[k++] = '_';
        else if (c > ' ' && c != '=' && c < 0x7f)
            out[k++] = (char)c;
    }
    out[k] = '\0';
}

/* note text: one line, no newline or carriage return */
static inline void bench_flat(char *out, size_t n, const char *in)
{
    size_t k = 0;

    for (; in && *in && k + 1 < n; in++)
        out[k++] = (*in == '\n' || *in == '\r' || *in == '\t') ? ' ' : *in;
    out[k] = '\0';
}

static inline int bench_fmt_start(char *buf, size_t n, const char *id)
{
    return snprintf(buf, n, "step=%s result=start\n", id);
}

static inline int bench_fmt_log(char *buf, size_t n, const char *id,
                                const char *text)
{
    char flat[BENCH_LINE_MAX];

    bench_flat(flat, sizeof(flat), text);
    return snprintf(buf, n, "step=%s log=%s\n", id, flat);
}

static inline int bench_fmt_result(char *buf, size_t n, const char *id,
                                   enum bench_res r, const char *answer,
                                   unsigned long ms, const char *note)
{
    char tok[BENCH_TOKEN_MAX], flat[BENCH_LINE_MAX / 2];

    bench_token(tok, sizeof(tok), answer);
    bench_flat(flat, sizeof(flat), note ? note : "");
    return snprintf(buf, n, "step=%s result=%s answer=%s ms=%lu note=%s\n",
                    id, bench_res_name[r < BENCH_N_RES ? r : 0], tok, ms,
                    flat);
}

/* A step to run again (the NEED retry after a restart): its earlier
 * result no longer counts (bench_replay) */
static inline int bench_fmt_retry(char *buf, size_t n, const char *id)
{
    return snprintf(buf, n, "retry step=%s\n", id);
}

/* A planned restart (BENCH_F_REBOOTS): the RTC's seconds when the step
 * asked for it, so the next press can tell whether one happened
 * (bench_restart_seen) */
static inline int bench_fmt_restart_asked(char *buf, size_t n, const char *id,
                                          unsigned long wall)
{
    return snprintf(buf, n, "step=%s log=restart asked: wall=%lu\n", id,
                    wall);
}

#define BENCH_RESTART_SLACK_S   2   /* the RTC's second and the tick's */

/* Did a restart come after the ask?  asked: the RTC's seconds at the ask
 * (0: not recorded, so not known: true); start: this start's own, the
 * RTC's seconds now less the time since the start.  Without a restart
 * this start began before the ask by at least the time it took to reach
 * the step (seconds: the boot, the menus); after one it began after it. */
static inline bool bench_restart_seen(unsigned long asked, unsigned long start)
{
    return !asked || start + BENCH_RESTART_SLACK_S >= asked;
}

/* build ids into a file name: [A-Za-z0-9-] only, at most n-1 */
static inline void bench_name_part(char *out, size_t n, const char *in)
{
    size_t k = 0;

    for (; in && *in && k + 1 < n; in++)
    {
        char c = *in;

        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-')
            out[k++] = c;
    }
    if (!k && n > 1)
        out[k++] = 'x';
    out[k] = '\0';
}

/* One parsed line */
enum bench_kind
{
    BENCH_K_OTHER = 0,
    BENCH_K_SESSION,
    BENCH_K_START,
    BENCH_K_LOG,
    BENCH_K_RESULT,
    BENCH_K_RESUME,
    BENCH_K_END,
    BENCH_K_RETRY
};

struct bench_rec
{
    enum bench_kind kind;
    char id[BENCH_TOKEN_MAX];
    enum bench_res res;
    char answer[BENCH_TOKEN_MAX];
    unsigned long ms;
};

/* field k=v's value (up to the next space) into out; false if absent */
static inline bool bench_field(const char *line, const char *key, char *out,
                               size_t n)
{
    size_t kl = strlen(key), k = 0;
    const char *p = line;

    while ((p = strstr(p, key)))
    {
        if ((p == line || p[-1] == ' ') && p[kl] == '=')
            break;
        p += kl;
    }
    if (!p)
        return false;
    p += kl + 1;
    while (*p && *p != ' ' && *p != '\n' && *p != '\r' && k + 1 < n)
        out[k++] = *p++;
    out[k] = '\0';
    return true;
}

static inline enum bench_res bench_res_parse(const char *s)
{
    int k;

    for (k = 1; k < BENCH_N_RES; k++)
        if (!strcmp(s, bench_res_name[k]))
            return (enum bench_res)k;
    return BENCH_NONE;
}

static inline unsigned long strtoul_bench(const char *s)
{
    unsigned long v = 0;

    while (*s >= '0' && *s <= '9')
        v = v * 10 + (unsigned long)(*s++ - '0');
    return v;
}

/* An answer token back to its index: 0..n-1, BENCH_ANSWERS for "Skip",
 * -1 for none or unknown */
static inline int bench_answer_index(const struct bench_step *s,
                                     const char *tok)
{
    char t[BENCH_TOKEN_MAX];
    int k;

    if (!tok || !strcmp(tok, "-"))
        return -1;
    if (!strcmp(tok, "Skip"))
        return BENCH_ANSWERS;
    for (k = 0; k < bench_n_answers(s); k++)
    {
        bench_token(t, sizeof(t), s->answers[k]);
        if (!strcmp(t, tok))
            return k;
    }
    return -1;
}

static inline enum bench_kind bench_parse(const char *line,
                                          struct bench_rec *r)
{
    char v[BENCH_TOKEN_MAX];
    const char *rest;

    memset(r, 0, sizeof(*r));
    if (!strncmp(line, "session ", 8))
        return r->kind = BENCH_K_SESSION;
    if (!strncmp(line, "end ", 4) || !strcmp(line, "end"))
        return r->kind = BENCH_K_END;
    if (!strncmp(line, "resume ", 7))
    {
        bench_field(line, "after", r->id, sizeof(r->id));
        return r->kind = BENCH_K_RESUME;
    }
    if (!strncmp(line, "retry ", 6))
    {
        if (!bench_field(line, "step", r->id, sizeof(r->id)))
            return r->kind = BENCH_K_OTHER;
        return r->kind = BENCH_K_RETRY;
    }
    if (strncmp(line, "step=", 5) || !bench_field(line, "step", r->id,
                                                  sizeof(r->id)))
        return r->kind = BENCH_K_OTHER;
    /* the second field says which: a log line's text or a note may hold
       anything after it */
    rest = strchr(line, ' ');
    if (rest && !strncmp(rest + 1, "log=", 4))
        return r->kind = BENCH_K_LOG;
    if (!rest || strncmp(rest + 1, "result=", 7) ||
        !bench_field(rest + 1, "result", v, sizeof(v)))
        return r->kind = BENCH_K_OTHER;
    r->res = bench_res_parse(v);
    if (r->res == BENCH_START)
        return r->kind = BENCH_K_START;
    if (r->res == BENCH_NONE)
        return r->kind = BENCH_K_OTHER;
    {
        /* answer= and ms= sit before note=, whose text is free */
        char head[BENCH_LINE_MAX];
        const char *note = strstr(rest, " note=");
        size_t hl = note ? (size_t)(note - rest) : strlen(rest);

        if (hl >= sizeof(head))
            hl = sizeof(head) - 1;
        memcpy(head, rest, hl);
        head[hl] = '\0';
        if (!bench_field(head, "answer", r->answer, sizeof(r->answer)))
            strcpy(r->answer, "-");
        if (bench_field(head, "ms", v, sizeof(v)))
            r->ms = strtoul_bench(v);
    }
    return r->kind = BENCH_K_RESULT;
}

/* ---- Replay and resume ----------------------------------------------- */

/* Where a session stands, from its file's lines in order */
struct bench_state
{
    int next;                   /* the first step not yet ended */
    int dangling;               /* a step started, never ended; -1 none */
    int last_done;              /* the last step ended; -1 none */
    bool go_yes;                /* the last go question said yes */
    bool cop_stuck;             /* a COP step hung */
    bool ended;                 /* the file has its end line */
    bool started;               /* the file has its session line */
    unsigned passed, failed, skipped;
    /* the session line's mode= and image fields (the re-verify run) */
    int mode;                   /* enum bench_mode */
    bool merged;                /* kind=merged */
    unsigned long image_bytes;
    uint32_t image_fnv;
    char date[12];              /* yyyy-mm-dd */
    uint8_t res[BENCH_MAX_STEPS];   /* each step's last result, by index */
    /* a planned restart's record (bench_fmt_restart_asked): the RTC's
       seconds when the step asked for it; 0 none */
    unsigned long restart_asked;
};

static inline int bench_find(const struct bench_step *t, int n,
                             const char *id)
{
    int k;

    for (k = 0; k < n; k++)
        if (!strcmp(t[k].id, id))
            return k;
    return -1;
}

static inline void bench_state_init(struct bench_state *st)
{
    memset(st, 0, sizeof(*st));
    st->dangling = -1;
    st->last_done = -1;
}

static inline void bench_uncount(struct bench_state *st, enum bench_res r)
{
    if ((r == BENCH_PASS || r == BENCH_DONE) && st->passed) st->passed--;
    else if ((r == BENCH_FAIL || r == BENCH_HANG) && st->failed) st->failed--;
    else if (r == BENCH_SKIP && st->skipped) st->skipped--;
}

static inline void bench_count(struct bench_state *st, enum bench_res r)
{
    if (r == BENCH_PASS || r == BENCH_DONE) st->passed++;
    else if (r == BENCH_FAIL || r == BENCH_HANG) st->failed++;
    else if (r == BENCH_SKIP) st->skipped++;
}

/* Step k ended with r: counted, and kept as the step's result */
static inline void bench_set(struct bench_state *st, int k, enum bench_res r)
{
    bench_count(st, r);
    if (k >= 0 && k < BENCH_MAX_STEPS)
        st->res[k] = (uint8_t)r;
}

/* ---- Sessions: the full run, the re-verify run, the port write check ---
 * The row asks which (after the resume offer, when a session is open):
 * the full session, the short run that re-verifies a new merged video
 * image (about 10 min), or the port write check (CR1 alone, about 2 min).
 * All run steps from the same table; the session line records the mode,
 * so a resume carries on in it:
 *
 *   session build=<id> date=<yyyy-mm-dd hh:mm> steps=<n> mode=reverify
 *           image=<bytes> fnv=<hex> kind=<merged|plain>
 *
 * (mode=portcheck for the port write check, which earns no verified
 * line)
 * (one line; the full session's line has no mode=, as before, but has the
 * image fields).  When every re-verify check passed, in either session
 * (bench_reverify_ok, bench_full_ok), the file also gets
 *
 *   verified <the bcmfw/verified.txt line to add>
 *   token <a bcmfw/verified.txt token line: clock150, s4c and s6 passed>
 *
 * before its end line, and the end screen shows the same line. */
enum bench_mode
{
    BENCH_MODE_FULL = 0,
    BENCH_MODE_REVERIFY,
    BENCH_MODE_PORTCHECK,
    BENCH_MODE_PROBE,           /* sitting 4h: the core layout probe */
    BENCH_MODE_BATCH,           /* a chip batch's own checks (batch 5) */
    BENCH_N_MODES
};

static const char *const bench_mode_name[BENCH_N_MODES] =
{
    "full", "reverify", "portcheck", "probe", "batch"
};

/* The picked runs that earn no verified line (the port write check, the
 * core layout probe) */
static inline bool bench_mode_verifies(int mode)
{
    return mode == BENCH_MODE_FULL || mode == BENCH_MODE_REVERIFY ||
           mode == BENCH_MODE_BATCH;
}

/* The runs that may earn the clock150 token line (s4c and s6 passed in
 * the session): the verifying ones and the chip batch's checks, which
 * run s4c and s6 for a new image (bench_batch_picks) */
static inline bool bench_mode_tokens(int mode)
{
    return bench_mode_verifies(mode) || mode == BENCH_MODE_BATCH;
}

/* The row's question, in this order: its answer (0..BENCH_N_MODES-1;
 * anything else is MENU) to a mode, -1 for cancel */
static const char *const bench_mode_answers[BENCH_N_MODES] =
{
    "Full session", "Re-verify the video image", "Port write check",
    "Core layout probe", "Chip update checks"
};

static inline int bench_mode_choice(int a)
{
    return a >= 0 && a < BENCH_N_MODES ? a : -1;
}

/* A step's part in a picked run */
enum bench_role
{
    BENCH_R_RUN = 0,            /* the full run: the table's own rules */
    BENCH_R_PRE,                /* pre-flight: pass, or skip (no flag) */
    BENCH_R_IFPRE,              /* runs only if the pre-flight skipped */
    BENCH_R_NEED,               /* must pass */
    BENCH_R_OPT,                /* may be skipped; must not fail */
    BENCH_R_ALSO,               /* runs with the others; its result never
                                   counts for the verified line (s4c: the
                                   150 MHz clock flag, not the image) */
    BENCH_R_IFBOOT              /* the restart onto the display driver:
                                   runs unless the pre-flight passed, so
                                   also when the flags hold but this start
                                   has no display driver (pre DONE) */
};

/* A pick held for a later sitting: its role | BENCH_R_HELD.  It stays in
 * the table, role and needs intact, but bench_select leaves it out while
 * BENCH_HOLD_LATER (tests/autochecks turns that off to cover the roles).
 * Sitting 4d (owner, 2026-10-26: a short sitting, under 15 min) holds b3,
 * s4b, s4c, vfeed, s7, vbv and vgov; back in a run: drop the "| HELD".
 * Sitting 4e keeps them held and adds vizsoak and vlong (under 20 min of
 * the owner's time; vlong's 10-minute play is hands off).  Sitting 4f
 * (2026-11-05) re-runs what 4e failed (uviz, vizsoak, jvpu) and what it
 * never reached (b13 on): it also holds respanel and c1, which passed in
 * 4e (ucop's need on c1 then blocks nothing: a step with no result in
 * the run blocks nothing, bench_dep_failed).  Sitting 4g re-proves the
 * video after 4f's fixes (the restart's clock, the index window) and
 * the visualiser's CPU split: uviz, jvpu, b5, s6, vplay and vlong after
 * the start-up steps (jvpu and b5 earn the verified line), and b13 as a
 * host-only A/B of the host's cache flush (BCM quirk #41, program-swap.md;
 * no image change: round 0 in the cold start's form, round 1 in
 * @25a's); it holds vizsoak.  19-9, the UI on the COP, is retired
 * (owner, 2026-11-05: Apple's core layout is adopted), so the
 * second-core group (ucop, uck1, uck2, ufeel, upanel, ucusb, ucmod,
 * ucop2, ucoff) and the core-layout study's arms (clc, clf) are held,
 * their code kept.  With ucop held, the session's end clears any "cop"
 * switch an earlier session left (ucop_settle, bench_pick_held), so the
 * unit starts on the main processor as the owner's daily driver. */
#define BENCH_R_HELD        0x80
#ifndef BENCH_HOLD_LATER
#define BENCH_HOLD_LATER    1
#endif

/* The order and the dependencies (owner, after sitting 4b: "I'd rather
 * wait an hour for a clean session than have another one stopped by the
 * UI").  A picked run goes basic to advanced, independent groups first,
 * so a failure can't strand a later group; no step asks "run it or
 * skip?".  A step names up to BENCH_NEEDS steps it needs (need[]): when
 * one of them failed, hung, or could not run (skipped, but for a role's
 * own "not needed": IFPRE, IFBOOT, PRE), the step is skipped by itself,
 * recorded as such (bench_fmt_autoskip: the reason in the note), and the
 * screen says why in one plain sentence (bench_gates[], once a gate).
 * A gate marked BENCH_GATE_END ends the session: every later step skips.
 *
 * The clean finish: a must-pass (NEED) step that fails never stops the
 * session by itself.  What needs it skips; the groups that don't need it
 * still run, because each still teaches us something (the table below
 * says which, group by group); then the summary names what didn't pass
 * and says "That's all for today". */
#define BENCH_NEEDS         2

struct bench_pick
{
    const char *id;
    uint8_t role;
    const char *need[BENCH_NEEDS];  /* steps this one needs; NULL: none */
};

/* The one sentence a skip for a gate shows (the auto-skip screen), and
 * whether the gate ends the session.  A gate not listed: BENCH_GATE_WHY */
#define BENCH_GATE_END      0x01

struct bench_gate
{
    const char *id;
    uint8_t flags;
    const char *why;
};

#define BENCH_GATE_WHY  "Skipping a step: something it needs didn't " \
    "pass. The log has what we need."

static const struct bench_gate bench_gates[] =
{
    { "s05", BENCH_GATE_END, "That's all for today: the test pictures "
      "didn't pass, so the other checks can't run. The log has what we "
      "need." },
    { "jobs", 0, "Skipping the steps that use the video chip's jobs: "
      "its self-test didn't pass. The log has what we need." },
    { "s4b", 0, "Skipping the fast clock pictures: the fast clock check "
      "didn't pass." },
    { "s4c", 0, "Skipping the video start-up question: without the fast "
      "clock pictures passing, that check doesn't run." },
    { "s6", 0, "Skipping the other video steps: the test clips didn't "
      "play properly. The log has what we need." },
    { "c1", 0, "Skipping the second-core steps: the second core check "
      "didn't pass. The log has what we need." },
    { "ucop", 0, "Skipping the second-core steps: the screens didn't "
      "move to the second core. The log has what we need." },
};

/* The re-verify run, in run order (sitting 4c), group by group.  An id
 * this build's table lacks (b3 without HAVE_BCM_JOBS) is left out.
 *
 * 1. Start-up: the job self-test first, straight after the boot, while
 *    the resident image is the merged one.  s05 failing ends the session
 *    (nothing after it can run without the stage flags).  jobs failing
 *    skips group 2; the rest still runs.
 * 2. The chip's jobs: the drift, the panel and the visualizer, all on the
 *    merged image's job path, straight after jobs.  Everything after them
 *    that swaps the chip's program hands it back to Apple's vmcs through
 *    the stock wake path (dt_restore(false)), the jobs off until the next
 *    start: c1's probe image (vplayer_probe; jvpu only reads the
 *    running image's words, vplayer_peek), the 4b/4c
 *    stages, the labs (b13, b5) and the stage plays (sitting 4b: the
 *    panel decoded on the PP with no overlay load; "a little laggy" was
 *    the visualizer's CPU fallback, why=nojobs).  None of the three swaps
 *    the program itself: b3 sets the drift's backend (dj_dev_row), the
 *    panel's DECODE and the visualizer's overlay load into the merged
 *    image's shared overlay region.  respanel before uviz: it wants an
 *    album not yet played on this build, and uviz plays one.  vizsoak
 *    (sitting 4e) after uviz: the same view, a few minutes.  Needs jobs.
 * 1b. The rest of the self-tests: jvpu (J_VPU from the still resident
 *    image, 3 for the merged one; B5 boots the display-only image, which
 *    would answer 0x27), c1, then the clock flags.
 * 3. The chip's programs, no jobs needed: the warm swap, the window
 *    push.  Independent: they run whatever failed before.
 * 4. Video, s6 first.  s6 failing skips the rest of the group and 5.
 * 5. One normal play from Videos, outside the bench (vplay: the session
 *    has left, so vplayer_bench_play() is off and the first-use check
 *    runs).  Needs s6; the fast clock comes from the image's clock150
 *    token (sitting 4c's s4c and s6), so s4c no longer gates it.  The
 *    step drops this unit's check result first (vplayer_check_reset), so
 *    the check runs again on the unit that already has one.  Then vlong
 *    (sitting 4e): the test episode, 10 minutes hands off; needs s6.
 * 6. The UI on the second core, last.  ucop's restart moves the UI to
 *    the COP for the rest of the session, and the video group can't run
 *    there: the bench runs its steps on the UI thread, and the devtest's
 *    stage plays and labs are CPU-only code (no remote calls: they edit
 *    the tick list in dt_restore, BCM must-fix 3, and drive engine 2 and
 *    the devtest's flags directly; p19-ui-on-cop.md).  The restart also
 *    brings the merged image back after the stage plays.  Needs c1
 *    (engine 2 from the COP); ucop failing (the UI didn't move) skips the
 *    rest.
 * 4b. The rows that may freeze (sitting 4gh): p2, p1 (the core layout
 *    probe), after s6, before the long plays.
 * 7. duty, the long dark row, at the very end. */
static const struct bench_pick bench_reverify_picks[] =
{
    /* 1. start-up and self-tests */
    { "pre",    BENCH_R_PRE, { NULL } },
    /* SLOT RO (the final sitting, owner 2026-11-05): the read-only rows,
       first: pgrd, preg, the PP id line (devbench_pwr.c).  One line a
       row, BENCH_R_ALSO, no needs.  awxm (p19-pwr-awake, AW-8 and AW-9:
       the memory setting and the ADC's done bit, logged), with the
       battery probes' read-only pair (p19-pwr-probes, devbench_pwr.h
       bench_pwr_picks: pgrd the speed grade, preg the power registers,
       logged before any probe writes them), then prtc (hands off, about
       15 s, three clocks compared; harmless).  One hands-off block */
    { "pgrd",   BENCH_R_ALSO, { NULL } },
    { "preg",   BENCH_R_ALSO, { NULL } },
    { "awxm",   BENCH_R_ALSO, { NULL } },
    { "prtc",   BENCH_R_ALSO, { NULL } },
    { "s05",    BENCH_R_IFPRE, { NULL } },
    /* s05 rewrote the stage flags: the boot path reads them only at a
       start, so the merged image becomes the display driver only after a
       restart (bench 2026-10-25, sitting 2's first try: jobs and jvpu
       found no resident image).  Also after a pre-flight that found the
       flags but no display driver in this start (sitting 3, 2026-10-26:
       an earlier session's s05 left Apple's program running) */
    { "boot",   BENCH_R_IFBOOT, { NULL } },
    { "jobs",   BENCH_R_NEED, { NULL } },
    /* 2. the chip's jobs, straight after the job self-test: everything
       after it that swaps the chip's program (c1's probe image,
       the 4b/4c stages, b13, b5, the stage plays) hands the chip back to
       Apple's program through the stock wake path, with the jobs off
       until the next start (sitting 4b: the panel and the visualizer
       went to the PP) */
    { "b3",     BENCH_R_OPT | BENCH_R_HELD, { "jobs" } },
    /* batch 3: the panel decoded on the chip by the merged image's DECODE
       overlay and kept resident (RESPANEL); a reading, not the line */
    { "respanel", BENCH_R_ALSO | BENCH_R_HELD, { "jobs" } },
    /* 0.8.0: the visualizer on the chip (release path), its CPU and the
       DECODE/VIZ swap at a track change */
    { "uviz",   BENCH_R_ALSO, { "jobs" } },
    /* sitting 4e: the visualiser a few minutes on the chip (day 4c's
       "viz soak" line): every frame on the BCM, no CPU fallback */
    /* sitting 4g: VF0, the BCM's SPECTRUM (its C path, today's) against
       the Mac's goldens, hands off (viz-fft-vc.md section 10) */
    { "vf0",    BENCH_R_ALSO, { "jobs" } },
    { "vizsoak", BENCH_R_ALSO | BENCH_R_HELD, { "jobs" } },
    /* SLOT AWAKE, the VC's part (p19-vcpwr, devbench_vcpwr.h
       bench_vcpwr_picks): the chip's awake-time A/Bs want the merged
       image as the display driver with its jobs up, so they run here,
       before jvpu (everything after jvpu may hand the chip to Apple's
       program).  s1 idle sleep, l5 core level 5, ck block clocks.
       Then the LCD supply pair, on our resident image: pldo
       (p19-pwr-probes S-4: the LCD's supply in Apple's sleep mode once,
       5 s, with a question; then the codec's, the album playing) and
       awld (p19-pwr-awake, AW-4: the daily code's mode 010 across five
       dark/wake cycles), pldo first, as the less likely to leave the
       screen dark.  pwraw-lcdldo is for the daily path, so both run with
       our resident image driving the screen; the token line is awld's
       (bench_awake_tokens), pldo's answers sit beside it in the file.
       Either may leave the screen dark: the owner restarts by hand, and
       Resume records the step as hung and carries on (pre, s05 and boot
       already have their results; the stage flags bring the resident
       image back at that start: ipodui-p19-bench-awldhang) */
    { "s1",     BENCH_R_ALSO, { "jobs" } },
    { "l5",     BENCH_R_ALSO, { "jobs" } },
    { "ck",     BENCH_R_ALSO, { "jobs" } },
    { "pldo",   BENCH_R_ALSO, { NULL } },
    { "awld",   BENCH_R_ALSO, { NULL } },
    { "jvpu",   BENCH_R_NEED, { NULL } },
    /* engine 2 from the COP (19-9's B10) */
    { "c1",     BENCH_R_ALSO | BENCH_R_HELD, { NULL } },
    /* 4c's precondition: its flag names this build's 150 MHz bench */
    { "s4b",    BENCH_R_ALSO | BENCH_R_HELD, { NULL } },
    /* the clock flag s6 plays at */
    { "s4c",    BENCH_R_ALSO | BENCH_R_HELD, { "s4b" } },
    /* 3. the chip's programs.  The warm swap tests the display-only
       image's PARK into the video image: not this merged image's path (it
       is both), so it never gates the merged image's line (owner,
       2026-10-25) */
    { "b13",    BENCH_R_ALSO, { NULL } },
    { "b5",     BENCH_R_NEED, { NULL } },
    /* 4. video.  s7, vbv, vgov back for sitting 4 (owner, 2026-10-26: the
       sitting carries what the work needs): the controls on the converted
       clip (V-1), the image's level bands (V-7) and the governor;
       readings, not the line.  drill2 (passed sitting 2) stays in the
       full session */
    { "s6",     BENCH_R_NEED, { NULL } },
    { "vfeed",  BENCH_R_ALSO | BENCH_R_HELD, { "s6" } },
    { "s7",     BENCH_R_ALSO | BENCH_R_HELD, { "s6" } },
    { "vbv",    BENCH_R_ALSO | BENCH_R_HELD, { "s6" } },
    { "vgov",   BENCH_R_ALSO | BENCH_R_HELD, { "s6" } },
    /* The final sitting's order (owner, 2026-11-05): read-only rows
       (SLOT RO, above), the awake A/Bs, VS, the rows that may freeze,
       then the long hands-off rows last (vlong, k1, duty).  Each group
       to come adds one line a row at its slot (BENCH_R_ALSO: readings,
       never the verified line), its rows in devbench.c's table, and
       its screens in scripts/bench_sim_script.py.  The slots come after
       everything the verified line needs (s6), so a row that disturbs
       the chip or freezes the unit can't cost the line.
       SLOT AWAKE: the awake A/Bs.  The VC's (s1, l5, ck) sit before
       jvpu (above: they need the resident image); "Battery savings"
       (devbench_awake.c, bench_awake_picks), PP-side, here, none
       needing the video chip: the cable steps together (the headphone
       socket, a USB-C sound adapter, a charger then the computer), so
       the owner handles the cables once.  Each starts the album uviz
       played when it needs music.  awd2c needs awd2 (the same toggle
       proven on the headphones first); awus leaves the session for the
       computer and asks at Go on.  The rest of the group sits
       elsewhere: awxm at SLOT RO, awld after ck (the resident image),
       awi2 at SLOT FREEZE, awlg at SLOT LONG; pldo (devbench_pwr.c) goes
       just before awld.
       SLOT VS: vs (devbench_vcpwr.c; about 4 min; needs s6, plays as
       vgov does), below, after the cable steps.
       SLOT FREEZE: p2 and p1, awi2 (the codec clock gate), then the
       battery probes that write (p19-pwr-probes), least risky first:
       pstl (timed self-stalls, seconds), pden (unused DEV_EN blocks off
       one at a time, a key press each), psrf (SDRAM self-refresh from
       IRAM).  pden and psrf need preg (the starting words logged).  Each
       puts back exactly what it found (devbench_pwr.h), so vplay, vlong,
       awlg and duty find the unit as before; all may freeze.  sd, the other row that
       may freeze, ends in a cut that drops the resident image for the
       rest of the boot, so it runs last of all (below duty).
       4b. sitting 4gh: the core layout probe's rows that may freeze
       (bench_probe_picks has the reasons): p2 (the COP's own Timer 2
       interrupt) and p1 (the PCM DMA's FIQ on the COP, the tone and its
       question).  p1 failing skips nothing.  Neither touches the video
       chip or needs a restart */
    { "awd2",   BENCH_R_ALSO, { NULL } },
    { "awd2c",  BENCH_R_ALSO, { "awd2" } },
    { "awus",   BENCH_R_ALSO, { NULL } },
    { "vs",     BENCH_R_ALSO, { "s6" } },
    { "p2",     BENCH_R_ALSO, { NULL } },
    { "p1",     BENCH_R_ALSO, { NULL } },
    /* core layout A (devbench_core.h): pcm-pp.c's own driver with the
       FIQ on the COP, after the raw form (p1) proved the route */
    { "pcop",   BENCH_R_ALSO, { "p1" } },
    { "awi2",   BENCH_R_ALSO, { NULL } },
    { "pstl",   BENCH_R_ALSO, { NULL } },
    { "pden",   BENCH_R_ALSO, { "preg" } },
    { "psrf",   BENCH_R_ALSO, { "preg" } },
    /* 5. a normal play from Videos (the first-use check).  s6 alone
       gates it: the image's clock150 token (bcmfw/verified.txt) opens
       the fast clock without s4c's flag (sitting 4d) */
    { "vplay",  BENCH_R_ALSO, { "s6" } },
    /* sitting 4e: the test episode from Videos, 10 minutes hands off
       (day 4c's stall came about 10 minutes in): the pictures in step
       with the audio to the end, any still recovered by itself */
    { "vlong",  BENCH_R_ALSO, { "s6" } },
    /* sitting 4g: the core-layout study's comparison rows
       (docs/design/core-layout-decision.md section 5), existing switches
       only: L-1 (lit Now Playing) and a shortened R-1 (audio under
       stress) in one outside step per arm, C (the decode on the CPU,
       input boost) then F (Fit-2, on the COP); each arm sets the
       decode's core, tags the log and puts the core back at its
       question.  Arm B is ucop2's soak with the UI on the COP (tagged
       there).  Before ucop's restart: the UI must be on the CPU */
    { "clc",    BENCH_R_ALSO | BENCH_R_HELD, { NULL } },
    { "clf",    BENCH_R_ALSO | BENCH_R_HELD, { NULL } },
    /* 6. 19-9's UI on the COP: a restart, p19-ucop-fix's four owner rows
       on the second core, then the soak outside */
    { "ucop",   BENCH_R_ALSO | BENCH_R_HELD, { "c1" } },
    { "uck1",   BENCH_R_ALSO | BENCH_R_HELD, { "ucop" } },
    { "uck2",   BENCH_R_ALSO | BENCH_R_HELD, { "ucop" } },
    { "ufeel",  BENCH_R_ALSO | BENCH_R_HELD, { "ucop" } },
    { "upanel", BENCH_R_ALSO | BENCH_R_HELD, { "ucop" } },
    /* sitting 4g: the cheap second-core checks after p19-copfileio (the
       file layer, the USB path, a module); the power-off last, after
       the soak: it ends the start, its question comes at the next
       start's Go on */
    { "ucusb",  BENCH_R_ALSO | BENCH_R_HELD, { "ucop" } },
    { "ucmod",  BENCH_R_ALSO | BENCH_R_HELD, { "ucop" } },
    { "ucop2",  BENCH_R_ALSO | BENCH_R_HELD, { "ucop" } },
    { "ucoff",  BENCH_R_ALSO | BENCH_R_HELD, { "ucop" } },
    /* SLOT LONG: the long hands-off rows, after vlong, before duty.  awlg (p19-pwr-awake, AW-1: the drive's
       release linger forced on, about 20 min dark) gets its own soak:
       it changes when the drive sleeps, so it would move duty's wake
       counts, and duty's decode arms (C F C F) would move its refills */
    { "awlg",   BENCH_R_ALSO, { NULL } },
    /* core layout A's K-1 (devbench_core.h): the parked COP against
       today's pulse, its own dark soak (Hold on); duty moves the decode's
       core, so they can't share one */
    { "k1",     BENCH_R_ALSO, { NULL } },
    /* 7. sitting 4gh: the long dark row at the very end (duty, about
       15 min hands off; it plays the album uviz started; a reading) */
    { "duty",   BENCH_R_ALSO, { NULL } },
    /* 8. last of all: sd (p19-vcpwr, may freeze).  Its restore is always
       a cut, which drops the resident image for the rest of the boot;
       every step before it (vplay, vlong, duty's dark play on the daily
       display driver) wants that image, so nothing runs after it.  The
       owner is back at duty's end anyway */
    { "sd",     BENCH_R_ALSO, { "jobs" } },
};

/* The port write check (bench 2026-10-25: COMMIT failed, open question
 * #67): CR1 on the stage 5 flag as the BCM rows need it (s05 is
 * offered only if the flag is missing).  No wheel question otherwise. */
static const struct bench_pick bench_portcheck_picks[] =
{
    { "pre",    BENCH_R_PRE, { NULL } },
    { "s05",    BENCH_R_IFPRE, { NULL } },
    { "cr1",    BENCH_R_NEED, { NULL } },
};

/* The core layout probe (sitting 4h, owner 2026-11-05: Apple's layout (A),
 * the audio engine on the COP, if the CPU is essentially asleep with the
 * screen off while music plays; docs/design/core-layout-decision.md
 * section 5).  Basic to advanced, the owner's time first:
 *
 * 1. p2, hands off, silent: can the COP take an interrupt of its own at
 *    all (Timer 2, 100 Hz, 10 s)?  The COP's deadline source in (A).
 * 2. p1, a tone in the headphones: the PCM DMA's FIQ served by the COP,
 *    then the question.  The gate for (A).  It runs whatever p2 found
 *    (an IRQ and a FIQ are separate paths, and every way out of p1 puts
 *    the route back from the CPU's side, so a COP that serves nothing
 *    leaves nothing behind): the sitting carries both answers.
 * 3. duty, hands off, dark, about 14 min: today's CPU duty with the screen
 *    off, decode on the CPU and on the COP.  Needs nothing: it reads the
 *    existing layout, whatever the probes found.
 *
 * Nothing here touches the video chip, and no step earns a verified
 * line.  None needs a restart. */
static const struct bench_pick bench_probe_picks[] =
{
    { "p2",     BENCH_R_NEED, { NULL } },
    { "p1",     BENCH_R_NEED, { NULL } },
    { "duty",   BENCH_R_ALSO, { NULL } },
};

/* The chip batch's own checks (batch 5, merged-image-batch.md section
 * 8; sitting 4gh-b5): what the re-verify run leaves out for a new merged
 * image, and (since sitting 4gh ran its 34 steps on 23693ff4 first) the
 * 4gh rows whose answer batch 5 can change.  Owner, after 4gh: a row
 * that passed in 4gh is cut unless batch 5 changes the code or image it
 * exercises, so l5 and ck (regulator and clock words: unchanged in
 * b9bcbe57) and vlong (the decoder: unchanged; s6 and vs play on the new
 * image) are out; s1 (the adaptive tick), vs and b13 (their 4gh failures
 * were the rule and the host flush: quirks #56, #57), sd (never ran: its
 * guard, #58) and sdc (new) stay.  Then the PP reruns
 * (docs/bench-reruns.md), by the same rule: preg (read-only; psrf's
 * starting words), pldo (its codec part; the screen part is its gate)
 * on the resident image before jvpu, then after every video row (each
 * puts a clip in the audio playlist, #154): p1 then pcop (the COP's own
 * FIQ unmask), psrf (music stopped), alb (the owner plays an album:
 * awlg and duty need it), awlg and duty (arm F's codec load on the
 * COP), the long dark rows; k1 is settled.  sd stays last (its cut).  The image (b9bcbe57) has no clock150 token,
 * and batch 5 regenerated the stage 4 bench (VF3, the out[] guard) and
 * so the 150 MHz bench the 4b flag names: until s05, s4b and s4c pass on
 * this build, every video play runs the chip at 27 MHz
 * (bcm_devtest_fast_clock_allowed), the re-verify run's too.  Basic to
 * advanced:
 *
 * 1. pre, then s05 always (not IFPRE): stages 0 to 5 with the stage 4
 *    bench (VF3's six probes, GTHR in the guarded out[]); its new stage 4
 *    flag opens s4b.  s05 failing ends the session (its gate).
 * 2. s4b (the 150 MHz bench, hands off) and s4c (its pictures, a
 *    question): their flags let the player run 150 MHz on this unit.
 * 3. boot: the restart that brings the merged image back as the display
 *    driver (s05 and the stages left Apple's program running).
 * 4. On the resident image: jobs (the heartbeat on the adaptive tick),
 *    b3 (the drift: the jobs animation's 1 ms tick), vf0 (loads the
 *    visualiser overlay: its FFT start-up check and SPECTRUM against the
 *    Mac's SEP goldens), then s1 (idle sleep on the adaptive 10 ms
 *    tick), then jvpu (the vfft word).
 * 5. s6 at 150 MHz: with s4c, the clock150 token line
 *    (bench_mode_tokens).  vs (idle sleep inside a session, clips).
 * 6. sdc: SDRAM idle at the first-use check's CLOSE, then the play; may
 *    freeze the video chip.  Needs s6.
 * 7. sd last of all, as in the re-verify run: its restore is a cut, which
 *    drops the resident image for the rest of the boot.
 *
 * b13 and b5 (the program swap and the window test) after jvpu, as in
 * the re-verify run: they hand the chip to Apple's program until the next
 * start, so every step on the resident image comes before them; the
 * video steps after them load the merged image themselves.  With jobs,
 * jvpu, b5 and s6 (NEED) passing the run earns b9bcbe57's verified line
 * (bench_batch_ok), the drift (b3, OPT) counted as in the re-verify. */
static const struct bench_pick bench_batch_picks[] =
{
    { "pre",    BENCH_R_PRE, { NULL } },
    { "preg",   BENCH_R_ALSO, { NULL } },
    { "s05",    BENCH_R_NEED, { NULL } },
    { "s4b",    BENCH_R_ALSO, { "s05" } },
    { "s4c",    BENCH_R_ALSO, { "s4b" } },
    { "boot",   BENCH_R_ALSO, { NULL } },
    { "jobs",   BENCH_R_NEED, { NULL } },
    { "b3",     BENCH_R_OPT, { "jobs" } },
    { "vf0",    BENCH_R_ALSO, { "jobs" } },
    { "s1",     BENCH_R_ALSO, { "jobs" } },
    { "pldo",   BENCH_R_ALSO, { NULL } },
    { "jvpu",   BENCH_R_NEED, { NULL } },
    { "b13",    BENCH_R_ALSO, { NULL } },
    { "b5",     BENCH_R_NEED, { NULL } },
    { "s6",     BENCH_R_NEED, { NULL } },
    { "vs",     BENCH_R_ALSO, { "s6" } },
    { "sdc",    BENCH_R_ALSO, { "s6" } },
    { "p1",     BENCH_R_ALSO, { NULL } },
    { "pcop",   BENCH_R_ALSO, { "p1" } },
    { "psrf",   BENCH_R_ALSO, { "preg" } },
    { "alb",    BENCH_R_ALSO, { NULL } },
    { "awlg",   BENCH_R_ALSO, { "alb" } },
    { "duty",   BENCH_R_ALSO, { "alb" } },
    /* sitting 4gh-b5: the heap A/B (an album playing), before sd's cut */
    { "heap",   BENCH_R_ALSO, { "alb" } },
    { "sd",     BENCH_R_ALSO, { "jobs" } },
};

/* A session's steps, in run order: table indices and their roles */
struct bench_run
{
    int n;
    uint8_t k[BENCH_MAX_STEPS];
    uint8_t role[BENCH_MAX_STEPS];
    int8_t need[BENCH_MAX_STEPS][BENCH_NEEDS];  /* table indices; -1 none
                                                   (or not in this build) */
};

/* hold: leave out the held picks (BENCH_R_HELD) */
static inline int bench_select_hold(int mode, const struct bench_step *t,
                                    int n, struct bench_run *run, bool hold)
{
    int k, p;

    run->n = 0;
    if (mode == BENCH_MODE_REVERIFY || mode == BENCH_MODE_PORTCHECK ||
        mode == BENCH_MODE_PROBE || mode == BENCH_MODE_BATCH)
    {
        const struct bench_pick *picks = mode == BENCH_MODE_REVERIFY
            ? bench_reverify_picks : mode == BENCH_MODE_PROBE
            ? bench_probe_picks : mode == BENCH_MODE_BATCH
            ? bench_batch_picks : bench_portcheck_picks;
        int np = mode == BENCH_MODE_REVERIFY
            ? (int)(sizeof(bench_reverify_picks) / sizeof(bench_reverify_picks[0]))
            : mode == BENCH_MODE_PROBE
            ? (int)(sizeof(bench_probe_picks) / sizeof(bench_probe_picks[0]))
            : mode == BENCH_MODE_BATCH
            ? (int)(sizeof(bench_batch_picks) / sizeof(bench_batch_picks[0]))
            : (int)(sizeof(bench_portcheck_picks) / sizeof(bench_portcheck_picks[0]));

        for (p = 0; p < np; p++)
        {
            int d;

            if ((picks[p].role & BENCH_R_HELD) && hold)
                continue;           /* held for a later sitting */
            k = bench_find(t, n, picks[p].id);
            if (k < 0 || k >= BENCH_MAX_STEPS || run->n >= BENCH_MAX_STEPS)
                continue;
            run->k[run->n] = (uint8_t)k;
            for (d = 0; d < BENCH_NEEDS; d++)
            {
                int kd = picks[p].need[d]
                         ? bench_find(t, n, picks[p].need[d]) : -1;
                int q;

                /* a need held out of this run (sitting 4f: ucop's c1,
                   passed in 4e) gates nothing */
                for (q = 0; hold && kd >= 0 && q < np; q++)
                    if (!strcmp(picks[q].id, picks[p].need[d]))
                    {
                        if (picks[q].role & BENCH_R_HELD)
                            kd = -1;
                        break;
                    }
                run->need[run->n][d] = (int8_t)(kd < BENCH_MAX_STEPS ? kd
                                                                      : -1);
            }
            run->role[run->n++] = picks[p].role & ~BENCH_R_HELD;
        }
        return run->n;
    }
    for (k = 0; k < n && k < BENCH_MAX_STEPS; k++)
    {
        if (t[k].flags & BENCH_F_PICKED)
            continue;
        run->k[run->n] = (uint8_t)k;
        memset(run->need[run->n], -1, sizeof(run->need[run->n]));
        run->role[run->n++] = BENCH_R_RUN;
    }
    return run->n;
}

static inline int bench_select(int mode, const struct bench_step *t, int n,
                               struct bench_run *run)
{
    return bench_select_hold(mode, t, n, run, BENCH_HOLD_LATER);
}

/* Where the run goes on (after the replay and bench_settle): the full run
 * from the table's next step; a picked run from its first step with no
 * result yet.  run->n: all done */
static inline int bench_run_next(const struct bench_run *run,
                                 const struct bench_state *st)
{
    int p;

    for (p = 0; p < run->n; p++)
    {
        if (run->role[p] == BENCH_R_RUN ? run->k[p] >= st->next
                                        : st->res[run->k[p]] == BENCH_NONE)
            break;
    }
    return p;
}

/* The confirm screens' totals over a run: the steps (count) and their
 * expected seconds, from position `from`.  which: BENCH_SUM_MAIN the
 * steps every run takes, BENCH_SUM_OPTIONAL those behind a go question
 * (wave 2), BENCH_SUM_IFPRE those run only without the stage 5 flag,
 * BENCH_SUM_BLOCK the hands-off steps in a row from `from` (the "Hands
 * off now" screen's time) */
enum bench_sum
{
    BENCH_SUM_MAIN = 0,
    BENCH_SUM_OPTIONAL,
    BENCH_SUM_IFPRE,
    BENCH_SUM_BLOCK
};

static inline unsigned long bench_run_sum(const struct bench_run *run,
                                          const struct bench_step *t,
                                          int from, int which, int *count)
{
    unsigned long secs = 0;
    int p, n = 0;

    for (p = from; p < run->n; p++)
    {
        const struct bench_step *s = &t[run->k[p]];
        bool opt = s->flags & BENCH_F_OPTIONAL;
        bool ifpre = run->role[p] == BENCH_R_IFPRE ||
                     run->role[p] == BENCH_R_IFBOOT;
        bool in;

        if (which == BENCH_SUM_BLOCK)
        {
            if (!(s->flags & BENCH_F_AUTO))
                break;
            in = true;
        }
        else if (which == BENCH_SUM_OPTIONAL)
            in = opt;
        else if (which == BENCH_SUM_IFPRE)
            in = ifpre;
        else
            in = !opt && !ifpre;
        if (!in)
            continue;
        secs += bench_step_secs(s);
        n++;
    }
    if (count)
        *count = n;
    return secs;
}

/* The hands-off part of the main steps' time (the confirm screen's
 * "plus about N min hands-off"): BENCH_F_AUTO steps whole, others their
 * hands_s.  Same selection as bench_run_sum(BENCH_SUM_MAIN); the owner's
 * time is that sum minus this. */
static inline unsigned long bench_run_hands(const struct bench_run *run,
                                            const struct bench_step *t)
{
    unsigned long h = 0;
    int p;

    for (p = 0; p < run->n; p++)
    {
        const struct bench_step *s = &t[run->k[p]];

        if ((s->flags & BENCH_F_OPTIONAL) || run->role[p] == BENCH_R_IFPRE ||
            run->role[p] == BENCH_R_IFBOOT)
            continue;
        h += (s->flags & BENCH_F_AUTO) ? bench_step_secs(s) : s->hands_s;
    }
    return h;
}

/* Seconds as whole minutes, rounded to the nearest (a nonzero time is at
 * least 1); the confirm screen's split uses this */
static inline unsigned bench_minutes_near(unsigned long secs)
{
    unsigned m = (unsigned)((secs + 30) / 60);

    return secs && !m ? 1 : m;
}

/* Seconds as whole minutes, rounded up (at least 1) */
static inline unsigned bench_minutes(unsigned long secs)
{
    return secs ? (unsigned)((secs + 59) / 60) : 0;
}

/* The pre-flight's result in this run (BENCH_NONE: no pre-flight) */
static inline enum bench_res bench_pre_res(const struct bench_run *run,
                                           const struct bench_state *st)
{
    int p;

    for (p = 0; p < run->n; p++)
        if (run->role[p] == BENCH_R_PRE)
            return (enum bench_res)st->res[run->k[p]];
    return BENCH_NONE;
}

/* A role's own reason not to run position p; NULL: run it (the table's
 * rules, bench_skip_why, apply as well).  The pre-flight passes when the
 * flags hold and this start's display driver is our image with jobs
 * allowed; it is DONE when only the flags hold (a restart is due) */
static inline const char *bench_role_skip(const struct bench_run *run, int p,
                                          const struct bench_state *st)
{
    enum bench_res pre = bench_pre_res(run, st);

    if (run->role[p] == BENCH_R_IFPRE &&
        (pre == BENCH_PASS || pre == BENCH_DONE))
        return "not needed: the stage 5 flag is there";
    if (run->role[p] == BENCH_R_IFBOOT && pre == BENCH_PASS)
        return "not needed: the display driver runs";
    return NULL;
}

/* ---- Dependencies: the auto-skip ----------------------------------------- */

/* Did table step k leave the steps that need it without a footing?  It
 * failed or hung, or it could not run (a skip), but not a role's own
 * "not needed" skip (the pre-flight and the steps behind it).  A step
 * with no result yet (not reached, or not in this run) blocks nothing */
static inline bool bench_dep_failed(const struct bench_run *run,
                                    const struct bench_state *st, int k)
{
    enum bench_res r;
    int p;

    if (k < 0 || k >= BENCH_MAX_STEPS)
        return false;
    r = (enum bench_res)st->res[k];
    if (r == BENCH_FAIL || r == BENCH_HANG)
        return true;
    if (r != BENCH_SKIP)
        return false;
    for (p = 0; p < run->n; p++)
        if (run->k[p] == k)
            return run->role[p] != BENCH_R_PRE &&
                   run->role[p] != BENCH_R_IFPRE &&
                   run->role[p] != BENCH_R_IFBOOT;
    return true;
}

static inline const struct bench_gate *bench_gate_of(const char *id)
{
    int g;

    for (g = 0; g < (int)(sizeof(bench_gates) / sizeof(bench_gates[0])); g++)
        if (!strcmp(bench_gates[g].id, id))
            return &bench_gates[g];
    return NULL;
}

/* Should position p skip by itself?  The table index of the step that
 * stops it (an earlier BENCH_GATE_END gate that failed, else the first of
 * its needs that failed), or -1: run it */
static inline int bench_dep_skip(const struct bench_run *run,
                                 const struct bench_step *t,
                                 const struct bench_state *st, int p)
{
    const struct bench_gate *g;
    int q, d;

    for (q = 0; q < p && q < run->n; q++)
    {
        g = bench_gate_of(t[run->k[q]].id);
        if (g && (g->flags & BENCH_GATE_END) &&
            bench_dep_failed(run, st, run->k[q]))
            return run->k[q];
    }
    for (d = 0; d < BENCH_NEEDS; d++)
        if (run->need[p][d] >= 0 && bench_dep_failed(run, st, run->need[p][d]))
            return run->need[p][d];
    return -1;
}

/* The step a skip comes back to: k itself, or, when k skipped by itself,
 * the failed step it needed, and so on (c1 failed: ucop skips, then
 * uck1 skips after ucop, whose root is c1), so one failure shows one
 * sentence */
static inline int bench_dep_root(const struct bench_run *run,
                                 const struct bench_step *t,
                                 const struct bench_state *st, int k)
{
    int guard, p, d;

    for (guard = 0; guard < BENCH_MAX_STEPS && k >= 0 &&
                    st->res[k] == BENCH_SKIP; guard++)
    {
        for (p = 0; p < run->n && run->k[p] != k; p++)
            ;
        if (p == run->n)
            break;
        d = bench_dep_skip(run, t, st, p);
        if (d < 0 || d == k)
            break;
        k = d;
    }
    return k;
}

/* The auto-skip screen's sentence for a skip caused by table step k */
static inline const char *bench_dep_why(const struct bench_step *t, int k)
{
    const struct bench_gate *g = k >= 0 ? bench_gate_of(t[k].id) : NULL;

    return g ? g->why : BENCH_GATE_WHY;
}

/* The auto-skip's note in the results file: which step it needed and how
 * that ended (scripts/bench_ingest.py reads it back:
 * "auto-skip: needs <id> (<result>)") */
static inline int bench_fmt_autoskip(char *buf, size_t n,
                                     const struct bench_step *t,
                                     const struct bench_state *st, int k)
{
    return snprintf(buf, n, "auto-skip: needs %s (%s)", t[k].id,
                    bench_res_name[st->res[k] < BENCH_N_RES ? st->res[k]
                                                             : 0]);
}

static inline bool bench_is_autoskip(const char *note)
{
    return note && !strncmp(note, "auto-skip: ", 11);
}

/* The verified line's checks, in either session.  run is the re-verify
 * selection (bench_reverify_picks); st->res[] holds the run's results by
 * step.  NEED steps pass; optional steps pass, are done or skip; the image
 * is the merged one.  The pre-flight differs by session:
 * - re-verify: pre passes, or skipped and then s05 (IFPRE) passed, or
 *   done (the flags, no display driver) and then boot (IFBOOT) done;
 * - full: pre never runs (BENCH_F_PICKED), so s05 itself must pass.
 * alt (full session only; NULL: none) gives, by pick position, the table
 * index of a later rerun of the same check that may stand in for it, or
 * -1 (bench_full_ok: jobs2 and jvpu2, after the restart onto the display
 * driver, for jobs and jvpu). */
static inline bool bench_picks_ok(const struct bench_run *run,
                                  const struct bench_state *st, int mode,
                                  const int8_t *alt)
{
    enum bench_res pre = bench_pre_res(run, st);
    int p;

    if (!st->merged || !run->n)
        return false;
    for (p = 0; p < run->n; p++)
    {
        enum bench_res r = (enum bench_res)st->res[run->k[p]];

        if (alt && alt[p] >= 0 && r != BENCH_PASS &&
            st->res[alt[p]] == BENCH_PASS)
            r = BENCH_PASS;
        switch (run->role[p])
        {
        case BENCH_R_PRE:
            if (mode == BENCH_MODE_FULL)
                break;
            if (r != BENCH_PASS && r != BENCH_SKIP && r != BENCH_DONE)
                return false;
            break;
        case BENCH_R_IFPRE:
            if (r != BENCH_PASS && r != BENCH_DONE &&
                !(mode != BENCH_MODE_FULL && r == BENCH_SKIP &&
                  (pre == BENCH_PASS || pre == BENCH_DONE)))
                return false;
            break;
        case BENCH_R_IFBOOT:
            /* DONE: the restart step (boot) has no verdict of its own */
            if (r != BENCH_PASS && r != BENCH_DONE &&
                !(mode != BENCH_MODE_FULL && r == BENCH_SKIP &&
                  pre == BENCH_PASS))
                return false;
            break;
        case BENCH_R_OPT:
            if (r != BENCH_PASS && r != BENCH_DONE && r != BENCH_SKIP)
                return false;
            break;
        case BENCH_R_ALSO:
            break;
        default:
            if (r != BENCH_PASS)
                return false;
            break;
        }
    }
    return true;
}

/* Is the re-verify pick id held out of this sitting's run? */
static inline bool bench_pick_held(const char *id)
{
    int p, np = (int)(sizeof(bench_reverify_picks) /
                      sizeof(bench_reverify_picks[0]));

    for (p = 0; BENCH_HOLD_LATER && p < np; p++)
        if (!strcmp(bench_reverify_picks[p].id, id))
            return (bench_reverify_picks[p].role & BENCH_R_HELD) != 0;
    return false;
}

/* Did every must-pass step of the chip update checks pass?  The same
 * rules as the re-verify run's, on its own picks (bench_batch_picks) */
static inline bool bench_batch_ok(const struct bench_run *run,
                                  const struct bench_state *st)
{
    return st->mode == BENCH_MODE_BATCH &&
           bench_picks_ok(run, st, BENCH_MODE_BATCH, NULL);
}

/* Did every step of a re-verify run pass?  (bench_picks_ok) */
static inline bool bench_reverify_ok(const struct bench_run *run,
                                     const struct bench_state *st)
{
    return st->mode == BENCH_MODE_REVERIFY &&
           bench_picks_ok(run, st, BENCH_MODE_REVERIFY, NULL);
}

/* A full session's later reruns of a picked check: after the boot step's
 * restart the merged image is the resident display driver, so jobs2 and
 * jvpu2 read it even when part A's jobs and jvpu skipped (s05 and the
 * probes leave the resident image gone) */
static const struct
{
    const char *id, *again;
} bench_full_again[] =
{
    { "jobs", "jobs2" },
    { "jvpu", "jvpu2" },
};

/* Did a full session pass every check the re-verify run makes?  picks
 * gets the re-verify selection (for bench_opt_passed: the drift) */
static inline bool bench_full_ok(const struct bench_step *t, int n,
                                 const struct bench_state *st,
                                 struct bench_run *picks)
{
    int8_t alt[BENCH_MAX_STEPS];
    int p, a, k;

    /* the full session runs every step, the held ones too: they count as
       their own roles (the drift, b3) */
    bench_select_hold(BENCH_MODE_REVERIFY, t, n, picks, false);
    if (st->mode != BENCH_MODE_FULL)
        return false;
    for (p = 0; p < picks->n; p++)
    {
        alt[p] = -1;
        for (a = 0; a < (int)(sizeof(bench_full_again) /
                              sizeof(bench_full_again[0])); a++)
        {
            if (strcmp(t[picks->k[p]].id, bench_full_again[a].id))
                continue;
            k = bench_find(t, n, bench_full_again[a].again);
            if (k >= 0 && k < BENCH_MAX_STEPS)
                alt[p] = (int8_t)k;
        }
    }
    return bench_picks_ok(picks, st, BENCH_MODE_FULL, alt);
}

/* Did the run's optional steps all pass (the drift, on a job build)?
 * How many it made when they did, else 0: the verified line names only
 * the drift steps this run had (b3 always; drill2 once more when it is
 * picked again, after sitting 3's trim) */
static inline int bench_opt_passed(const struct bench_run *run,
                                   const struct bench_state *st)
{
    int p, n = 0;

    for (p = 0; p < run->n; p++)
    {
        if (run->role[p] != BENCH_R_OPT)
            continue;
        if (st->res[run->k[p]] != BENCH_PASS)
            return 0;
        n++;
    }
    return n;
}

/* The session line.  The full session's has no mode=, as before, and
 * the image fields too (its verified line needs them on a resume) */
static inline int bench_fmt_session(char *buf, size_t n, const char *build,
                                    const char *datetime, int steps,
                                    int mode, unsigned long bytes,
                                    uint32_t fnv, const char *kind)
{
    if (mode <= BENCH_MODE_FULL || mode >= BENCH_N_MODES)
        return snprintf(buf, n, "session build=%s date=%s steps=%d "
                        "image=%lu fnv=%08lx kind=%s\n", build, datetime,
                        steps, bytes, (unsigned long)fnv, kind);
    return snprintf(buf, n, "session build=%s date=%s steps=%d mode=%s "
                    "image=%lu fnv=%08lx kind=%s\n", build, datetime, steps,
                    bench_mode_name[mode], bytes, (unsigned long)fnv, kind);
}

/* Sitting 4g: the PP's id words (pp5020.h PP_VER1, PP_VER2 at
 * 0x70000000/4) after the session line, to settle PP5021C against PP5022C
 * for this unit (pp5022-reference open question 22).  The id is their 8
 * ASCII bytes in debug-pp.c's order ("PP5022C"-style: PP_VER2 then
 * PP_VER1, each from its top byte), a byte outside ' '..'~' as '.' */
static inline int bench_fmt_pp(char *buf, size_t n, uint32_t v1, uint32_t v2)
{
    char id[9];
    int i;

    for (i = 0; i < 8; i++)
    {
        uint32_t w = i < 4 ? v2 : v1;
        unsigned c = (w >> (8 * (3 - (i & 3)))) & 0xFF;

        id[i] = c >= ' ' && c <= '~' ? (char)c : '.';
    }
    id[8] = '\0';
    return snprintf(buf, n, "pp ver1=0x%08lx ver2=0x%08lx id=\"%s\"\n",
                    (unsigned long)v1, (unsigned long)v2, id);
}

static inline uint32_t bench_hex(const char *s)
{
    uint32_t v = 0;

    for (;; s++)
    {
        if (*s >= '0' && *s <= '9') v = v * 16 + (uint32_t)(*s - '0');
        else if (*s >= 'a' && *s <= 'f') v = v * 16 + (uint32_t)(*s - 'a' + 10);
        else if (*s >= 'A' && *s <= 'F') v = v * 16 + (uint32_t)(*s - 'A' + 10);
        else return v;
    }
}

static inline void bench_session_fields(struct bench_state *st,
                                        const char *line)
{
    char v[BENCH_TOKEN_MAX];
    int m;

    st->mode = BENCH_MODE_FULL;
    if (bench_field(line, "mode", v, sizeof(v)))
        for (m = BENCH_MODE_FULL + 1; m < BENCH_N_MODES; m++)
            if (!strcmp(v, bench_mode_name[m]))
                st->mode = m;
    if (bench_field(line, "date", st->date, sizeof(st->date)) == false)
        st->date[0] = '\0';
    if (bench_field(line, "image", v, sizeof(v)))
        st->image_bytes = strtoul_bench(v);
    if (bench_field(line, "fnv", v, sizeof(v)))
        st->image_fnv = bench_hex(v);
    st->merged = bench_field(line, "kind", v, sizeof(v)) &&
                 !strcmp(v, "merged");
}

/* The bcmfw/verified.txt line a passed run earns, in the run sheet's
 * format (bench-p19.md): name, FNV-1a, what verified it, and which
 * session (mode).  No path: the end screen shows it too. */
static inline int bench_fmt_verified_in(char *buf, size_t n, uint32_t fnv,
                                        unsigned long bytes,
                                        const char *build, const char *date,
                                        int drift, int mode)
{
    return snprintf(buf, n, "bcmvideo_jobs %08lx  stage 6 PASS (merged, "
                    "%lu bytes), B5 PASS, jobs self-test PASS, "
                    "J_VPU 3 PASS%s on the owner's 5.5G: %s session "
                    "%s, build %s", (unsigned long)fnv, bytes,
                    drift >= 2 ? ", drift B3 and drill 2 PASS" :
                    drift == 1 ? ", drift B3 PASS" : "",
                    mode == BENCH_MODE_FULL ? "full" :
                    mode == BENCH_MODE_BATCH ? "chip update" : "re-verify",
                    date && *date ? date : "-", build);
}

/* The clock token (bcmfw/verified.txt clock150, vclock_gate.h): s4c (the
 * 150 MHz switch with the panel on) and s6 (the player's clips, which run
 * at 150 MHz once 4b and 4c have their flags) both passed this session.
 * Independent of the full verified line: a release plays this image at
 * 150 MHz on the token alone. */
static inline bool bench_clock150_ok(const struct bench_step *t, int n,
                                     const struct bench_state *st)
{
    int a = bench_find(t, n, "s4c"), b = bench_find(t, n, "s6");

    return a >= 0 && b >= 0 && st->res[a] == BENCH_PASS &&
           st->res[b] == BENCH_PASS;
}

static inline int bench_fmt_clock150(char *buf, size_t n, uint32_t fnv,
                                     unsigned long bytes, const char *build,
                                     const char *date)
{
    return snprintf(buf, n, "clock150 %08lx  bcmvideo_jobs (%lu bytes): "
                    "stage 4c PASS and stage 6 PASS at 150 MHz on the "
                    "owner's 5.5G: session %s, build %s", (unsigned long)fnv,
                    bytes, date && *date ? date : "-", build);
}

/* The re-verify run's line */
static inline int bench_fmt_verified(char *buf, size_t n, uint32_t fnv,
                                     unsigned long bytes, const char *build,
                                     const char *date, int drift)
{
    return bench_fmt_verified_in(buf, n, fnv, bytes, build, date, drift,
                                 BENCH_MODE_REVERIFY);
}

/* The next piece of a long line for a screen `width` characters wide:
 * its length, broken after a space when one is in reach */
static inline int bench_wrap(const char *s, int width)
{
    int len = (int)strlen(s), k;

    if (len <= width)
        return len;
    for (k = width; k > 0; k--)
        if (s[k - 1] == ' ')
            return k;
    return width;
}

/* One line of the file, in order.  A result for a step not in this
 * build's table (another build's file) is counted and otherwise ignored */
static inline void bench_replay(struct bench_state *st,
                                const struct bench_step *t, int n,
                                const char *line)
{
    struct bench_rec r;
    int k;

    switch (bench_parse(line, &r))
    {
    case BENCH_K_SESSION:
        st->started = true;
        bench_session_fields(st, line);
        break;
    case BENCH_K_END:
        st->ended = true;
        break;
    case BENCH_K_START:
        k = bench_find(t, n, r.id);
        if (k >= 0)
        {
            st->dangling = k;
            if (t[k].flags & BENCH_F_REBOOTS)
                st->restart_asked = 0;
        }
        break;
    case BENCH_K_LOG:
        k = bench_find(t, n, r.id);
        if (k >= 0 && (t[k].flags & BENCH_F_REBOOTS) && st->dangling == k)
        {
            char v[BENCH_TOKEN_MAX];

            if (bench_field(line, "wall", v, sizeof(v)))
                st->restart_asked = strtoul_bench(v);
        }
        break;
    case BENCH_K_RETRY:
        /* the step runs again: its earlier result no longer counts */
        k = bench_find(t, n, r.id);
        if (k >= 0 && k < BENCH_MAX_STEPS && st->res[k] != BENCH_NONE)
        {
            bench_uncount(st, (enum bench_res)st->res[k]);
            st->res[k] = BENCH_NONE;
        }
        break;
    case BENCH_K_RESULT:
        k = bench_find(t, n, r.id);
        bench_set(st, k, r.res);
        if (k < 0)
            break;
        if (st->dangling == k)
            st->dangling = -1;
        st->last_done = k;
        if (k + 1 > st->next)
            st->next = k + 1;
        if (t[k].flags & BENCH_F_GO)
            st->go_yes = r.answer[0] && strcmp(r.answer, "-") &&
                         bench_answer_index(&t[k], r.answer) == 1;
        if (r.res == BENCH_HANG && (t[k].flags & BENCH_F_COP))
            st->cop_stuck = true;
        break;
    default:
        break;
    }
}

/* ---- The log's copy of each result (bcmtest.log) ---------------------
 * record() also writes each result to bcmtest.log as
 *
 *   auto: <id> <result> answer=<the answer's text> ms=<n> <note>
 *
 * and the log keeps its own retries.  A results-file line that never
 * landed (sitting 4c: vbv's start and result and vgov's start lost, so
 * the resume after the outside step ran vbv again) is taken from there:
 * after the file's replay, a step with no result in the file but one in
 * the log gets the log's, written into the file with a note saying so,
 * and is never run again.  Only this session's lines count:
 * "bench file <the results file's name>" (written at the session's start
 * and at each resume, as the log rolls at each start) opens them and
 * another name closes them; "auto: <id> retry" forgets the step's
 * earlier result, as the file's retry line does.  The logs are read
 * oldest first. */
#define BENCH_LOG_MARK      "bench file "
#define BENCH_LOG_NOTE      "from bcmtest.log (the results line was lost)"

struct bench_logged
{
    uint8_t res;                /* enum bench_res; BENCH_NONE: none */
    int8_t ans;                 /* bench_answer_index's; -1 none */
    uint32_t ms;
};

struct bench_logscan
{
    bool in;                    /* inside this session's lines */
    char name[64];              /* the results file's name */
    struct bench_logged got[BENCH_MAX_STEPS];
};

static inline void bench_logscan_init(struct bench_logscan *ls,
                                      const char *name)
{
    int k;

    memset(ls, 0, sizeof(*ls));
    snprintf(ls->name, sizeof(ls->name), "%s", name ? name : "");
    for (k = 0; k < BENCH_MAX_STEPS; k++)
        ls->got[k].ans = -1;
}

/* the next space-ended token of *p into out; *p past it and its space */
static inline void bench_log_tok(const char **p, char *out, size_t n)
{
    size_t k = 0;

    while (**p && **p != ' ' && **p != '\n' && **p != '\r')
    {
        if (k + 1 < n)
            out[k++] = **p;
        (*p)++;
    }
    out[k] = '\0';
    if (**p == ' ')
        (*p)++;
}

static inline void bench_logscan_line(struct bench_logscan *ls,
                                      const struct bench_step *t, int n,
                                      const char *line)
{
    const char *p = strstr(line, BENCH_LOG_MARK);
    char id[BENCH_TOKEN_MAX], v[BENCH_TOKEN_MAX], ans[BENCH_TOKEN_MAX];
    char name[sizeof(ls->name)];
    enum bench_res r;
    int k;

    if (p)
    {
        p += sizeof(BENCH_LOG_MARK) - 1;
        bench_log_tok(&p, name, sizeof(name));
        ls->in = ls->name[0] && !strcmp(name, ls->name);
        return;
    }
    if (!ls->in || !(p = strstr(line, "auto: ")))
        return;
    p += 6;
    bench_log_tok(&p, id, sizeof(id));
    bench_log_tok(&p, v, sizeof(v));
    k = bench_find(t, n, id);
    if (k < 0 || k >= BENCH_MAX_STEPS)
        return;
    if (!strcmp(v, "retry"))
    {
        ls->got[k].res = BENCH_NONE;
        return;
    }
    r = bench_res_parse(v);
    if (r == BENCH_NONE || r == BENCH_START)
        return;
    ls->got[k].res = (uint8_t)r;
    ls->got[k].ans = -1;
    ls->got[k].ms = 0;
    if (!strncmp(p, "answer=", 7))
    {
        /* the answer's text may hold spaces: up to " ms=" */
        const char *e = strstr(p, " ms=");
        char text[BENCH_TOKEN_MAX];
        size_t l;

        p += 7;
        l = e ? (size_t)(e - p) : strlen(p);
        if (l >= sizeof(text))
            l = sizeof(text) - 1;
        memcpy(text, p, l);
        text[l] = '\0';
        bench_token(ans, sizeof(ans), text);
        ls->got[k].ans = (int8_t)bench_answer_index(&t[k], ans);
        if (e)
            ls->got[k].ms = strtoul_bench(e + 4);
    }
}

/* The first step from `from` on with a result in the log and none in the
   file: its results line into buf (BENCH_LOG_NOTE as the note), its
   index; -1 when there is none */
static inline int bench_logscan_missing(const struct bench_logscan *ls,
                                        const struct bench_state *st,
                                        const struct bench_step *t, int n,
                                        int from, char *buf, size_t bn)
{
    int k;

    for (k = from < 0 ? 0 : from; k < n && k < BENCH_MAX_STEPS; k++)
    {
        const struct bench_logged *g = &ls->got[k];
        const char *a;

        if (st->res[k] != BENCH_NONE || g->res == BENCH_NONE)
            continue;
        a = g->ans < 0 ? NULL : g->ans == BENCH_ANSWERS ? "Skip"
          : t[k].answers[g->ans];
        bench_fmt_result(buf, bn, t[k].id, (enum bench_res)g->res, a, g->ms,
                         BENCH_LOG_NOTE);
        return k;
    }
    return -1;
}

/* After the replay: what a dangling step means.
 * - A manual step (fn == NULL: the owner left the session to do it), a
 *   step that left on purpose (BENCH_F_LEAVES) or a planned restart
 *   (BENCH_F_REBOOTS): BENCH_START, the session finishes
 *   it now (asks its question, records it); next stays on it.
 * - Anything else hung: BENCH_HANG, counted, next moves past it (and a
 *   COP step's hang marks the worker stuck); the caller records it.
 * BENCH_NONE: nothing dangling. */
static inline enum bench_res bench_settle(struct bench_state *st,
                                          const struct bench_step *t)
{
    int k = st->dangling;

    if (k < 0)
        return BENCH_NONE;
    if (!t[k].fn || (t[k].flags & (BENCH_F_REBOOTS | BENCH_F_LEAVES)))
    {
        st->next = k;
        return BENCH_START;
    }
    if (t[k].flags & BENCH_F_COP)
        st->cop_stuck = true;
    bench_set(st, k, BENCH_HANG);
    st->dangling = -1;
    st->last_done = k;
    if (k + 1 > st->next)
        st->next = k + 1;
    return BENCH_HANG;
}

/* Should step k run, given the state?  NULL: run it; else why not */
static inline const char *bench_skip_why(const struct bench_state *st,
                                         const struct bench_step *s)
{
    if ((s->flags & BENCH_F_OPTIONAL) && !st->go_yes)
        return "not chosen";
    if ((s->flags & BENCH_F_COP) && st->cop_stuck)
        return "the COP worker is stuck (a COP step hung)";
    return NULL;
}

/* ---- Part A's watchdog: never a restart mid-write ----------------------
 * A step past its limit restarts the iPod only while the disk is quiet:
 * off, or no transfer stamped for `quiet` ticks (ata.c stamps the time at
 * every transfer's start, chunk and end).  Busy: it looks again each tick,
 * up to `bound` ticks; still busy then, it gives up: no restart, a record
 * in RAM, and a screen asking for MENU + SELECT.  The bench's own writes
 * (the results file, bcmtest.log's auto: lines, ui_log's flush) happen
 * only between steps, with the watchdog disarmed. */
enum bench_dog_act
{
    BENCH_DOG_IDLE = 0,         /* disarmed */
    BENCH_DOG_WAIT,             /* within the limit */
    BENCH_DOG_DEFER,            /* past it, the disk busy: look again */
    BENCH_DOG_RESTART,          /* past it, the disk quiet: restart now */
    BENCH_DOG_GIVE_UP           /* busy past the bound: ask the owner */
};

struct bench_dog
{
    bool armed;
    long deadline;              /* tick */
    long defer_from;            /* tick the deferral began; 0 none */
    bool deferring;
};

static inline void bench_dog_arm(struct bench_dog *d, long now, long limit)
{
    d->armed = true;
    d->deadline = now + limit;
    d->deferring = false;
    d->defer_from = 0;
}

static inline enum bench_dog_act bench_dog_decide(struct bench_dog *d,
                                                  long now, bool disk_active,
                                                  long last_activity,
                                                  long quiet, long bound)
{
    bool busy;

    if (!d->armed)
        return BENCH_DOG_IDLE;
    if ((long)(now - d->deadline) <= 0)
        return BENCH_DOG_WAIT;
    busy = disk_active && last_activity != -1 &&
           (long)(now - last_activity) < quiet;
    if (!busy)
    {
        d->armed = false;
        return BENCH_DOG_RESTART;
    }
    if (!d->deferring)
    {
        d->deferring = true;
        d->defer_from = now;
    }
    if ((long)(now - d->defer_from) >= bound)
    {
        d->armed = false;
        return BENCH_DOG_GIVE_UP;
    }
    return BENCH_DOG_DEFER;
}

/* ---- J_VPU: each image's own value --------------------------------------
 * The merged image (no DECODE) sets CHECKED | SHIFT: 3.  An image that
 * decodes (the display-only one) also runs the IDCT check (MAC) and the
 * colour stage's (YUV, since 2026-10-25), so 0x27.
 * A shift check that disagreed leaves SHIFT clear (1, or 5): a finding to
 * compare with the bench rows ASHF/ASHG, not a broken image. */
static inline uint32_t bench_vpu_expect(uint32_t ops)
{
    bool decodes = ops & (1u << (J_OP_DECODE - 0x20));

    return J_VPU_CHECKED | J_VPU_SHIFT | (decodes ? J_VPU_MAC | J_VPU_YUV : 0);
}

/* The bits the image's own start-up checks write (jobs_init).  A loaded
 * visualiser overlay adds its own (FFT, BARS, FLIP: sitting 4e read 0x53,
 * the merged image's 3 plus BARS and FLIP, after uviz loaded
 * viz-ovl-m.bin: ipodui.log "bcmjob overlay ok ... vpu=0x53"); those
 * are the overlay's, not this verdict's */
#define BENCH_VPU_IMAGE (J_VPU_CHECKED | J_VPU_SHIFT | J_VPU_MAC | J_VPU_YUV)

static inline enum bench_res bench_vpu_verdict(uint32_t vpu, uint32_t ops)
{
    if (!(vpu & J_VPU_CHECKED))
        return BENCH_FAIL;      /* the start-up checks never ran */
    return (vpu & BENCH_VPU_IMAGE) == bench_vpu_expect(ops) ? BENCH_PASS
                                                            : BENCH_FAIL;
}

/* ---- Sitting 4e's two soaks ----------------------------------------------
 * vlong: the owner plays the test episode from Videos and leaves it at
 * least 10 minutes (day 4c's stall came about 10 minutes in).  The
 * player's long-play record (vplayer.h struct vplayer_prog, the figures
 * of its "video prog" lines) is copied here as plain numbers.  PASS: at
 * least min_s of audio played, the pictures in step with it over the
 * whole play (pics against the audio's time at the file's own rate:
 * about 30 a second for a 30 fps file, 24 for Sintel) and over its last
 * one to two minutes, so a still, if any, ended by itself.  A file of
 * unknown rate is judged against 24 (low) and 30 (high). */
struct bench_vlong
{
    bool valid;                 /* a play since the step's start */
    uint32_t pics, played_ms, fps_x100, tail_pics, tail_ms;
};

#define BENCH_VLONG_HI_PCT  115 /* restarts re-show a few pictures */
/* Sitting 4f: the owner watched about 5 min in vplay, then vlong's step
 * cleared that record and failed with "no play from Videos since the
 * step".  A record this long (a play still running, or the last one) is
 * kept at vlong's step and judged at the question, its minimum this
 * instead of the step's 10 min; a new play from Videos replaces it. */
#define BENCH_VLONG_CARRY_S 180

static inline bool bench_vlong_keeps(const struct bench_vlong *p,
                                     unsigned carry_s)
{
    return p->valid && p->played_ms >= carry_s * 1000u;
}
#define BENCH_VLONG_TAIL_MS 20000   /* a shorter tail is not judged */

/* pictures against the audio's time, per cent of the rate (fps_x100) */
static inline uint32_t bench_pace_pct(uint32_t pics, uint32_t ms,
                                      uint32_t fps_x100)
{
    uint64_t want = (uint64_t)ms * fps_x100 / 100000;

    return want ? (uint32_t)((uint64_t)pics * 100 / want) : 0;
}

static inline enum bench_res bench_vlong_verdict(const struct bench_vlong *p,
                                                 unsigned min_s,
                                                 unsigned min_pct,
                                                 const char **why)
{
    uint32_t lo = p->fps_x100 ? p->fps_x100 : 2400;
    uint32_t hi = p->fps_x100 ? p->fps_x100 : 3000;

    if (!p->valid)
        return *why = "no play from Videos since the step", BENCH_FAIL;
    if (p->played_ms < min_s * 1000u)
        return *why = "played too short to judge", BENCH_FAIL;
    if (bench_pace_pct(p->pics, p->played_ms, lo) < min_pct)
        return *why = "pictures fell behind the audio", BENCH_FAIL;
    if (bench_pace_pct(p->pics, p->played_ms, hi) > BENCH_VLONG_HI_PCT)
        return *why = "more pictures than the audio's time", BENCH_FAIL;
    if (p->tail_ms >= BENCH_VLONG_TAIL_MS &&
        bench_pace_pct(p->tail_pics, p->tail_ms, lo) < min_pct)
        return *why = "pictures stopped near the end", BENCH_FAIL;
    return *why = "pictures in step with the audio", BENCH_PASS;
}

/* vizsoak: the visualiser on Now Playing for a few minutes; its views'
 * sums (viz.h struct viz_soak).  PASS: at least min_s on screen, every
 * frame drawn on the BCM, no CPU fallback. */
struct bench_vizsoak
{
    uint32_t views, secs, bcm, cpu;
    bool fb;
    uint32_t dark;              /* views the screen going dark closed */
};

static inline enum bench_res bench_vizsoak_verdict(
    const struct bench_vizsoak *s, unsigned min_s, const char **why)
{
    if (!s->views)
        return *why = "the visualiser never opened since the step began",
               BENCH_FAIL;
    /* the view holds the light (viz.c hold_light()): a view the dark
       closed is the firmware's fault, never the owner's */
    if (s->dark)
        return *why = "the screen went dark under the visualiser",
               BENCH_FAIL;
    if (s->fb || s->cpu)
        return *why = "CPU fallback: frames drawn on the PP", BENCH_FAIL;
    if (!s->bcm)
        return *why = "no frame drawn on the chip", BENCH_FAIL;
    if (s->secs < min_s)
        return *why = "on screen too short to judge", BENCH_FAIL;
    return *why = "every frame on the chip", BENCH_PASS;
}

#endif /* _IPODUI_DEVBENCH_H_ */
