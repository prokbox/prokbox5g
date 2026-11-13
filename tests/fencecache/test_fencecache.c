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

/* Host test of the PP5022 cache drop (#290; 0ebecb0db6): the REAL cache
 * section of firmware/target/arm/pp/system-pp502x.c (run.sh slices it out
 * verbatim as cache_fence.c) with its registers and the CPSR's mask as
 * recording hooks (stub/fc_env.h).
 *
 * Model: the calling core has dirty lines before the call, and the code
 * that runs before each mask may dirty more; a flush (CACHE_OPERATION |=
 * FLUSH, seen at the CACHE_CTL poll) writes them all back; rewriting a
 * status word while any line is dirty loses it. The status rewrite runs
 * between hooks (each window's restore, each USEC_TIMER read), under the
 * mask the last hook left.
 *
 * Checked, for the COP and the CPU and each entry mask (IRQ/FIQ open or
 * masked):
 *  - no status word is rewritten while a line is dirty (the write-back
 *    precedes the drop);
 *  - the IRQ stays masked from the first mask until the rewrite is done;
 *  - no status word is rewritten with the FIQ open; the FIQ opens only
 *    at a block boundary (a multiple of 64 lines), on the COP only, and
 *    only if it was open at the call (7 windows then, none otherwise);
 *  - on the COP the bulk write-back runs before the mask (the entry's
 *    state) and a second one with both masked before the rewrite; on the
 *    CPU one write-back with both masked;
 *  - all 512 lines are rewritten, and the entry's mask is restored.
 * Prints the counts it checked and one verdict line. */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "fc_env.h"

#include "cache_fence.c"

#define LINES       (0x2000 / CACHEALIGN_SIZE)          /* 512 */
#define STRIDE      (CACHEALIGN_SIZE / sizeof(unsigned long))
#define REWRITTEN   (CACHE_LINE_VALID | CACHE_ADDRESS_MASK)
#define SENTINEL    (CACHE_LINE_VALID | CACHE_LINE_DIRTY | 0x1234)

static int fails, checks;
static char first_fail[200];

static void fail(const char *fmt, ...)
{
    va_list ap;
    char b[200];

    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    if (fails++ < 8)
        fprintf(stderr, "  FAIL: %s\n", b);
    if (!first_fail[0])
        snprintf(first_fail, sizeof(first_fail), "%s", b);
}

/* ---- the hooks ------------------------------------------------------- */
volatile unsigned long fc_cache_op;
volatile unsigned long fc_status[2][0x2000 / sizeof(unsigned long)];
unsigned int fc_core;

static int cpsr;                /* the mask bits now */
static int masked_once;         /* the call masked the IRQ at least once */
static int dirty;               /* lines dirty in the calling core */
static unsigned last_n;         /* rewritten at the last hook */
static int last_cpsr;           /* the mask the rewrite since ran under */
static const char *ctx;

#define MAX_EV 64
static struct { int cpsr; unsigned n; } flush_ev[MAX_EV];
static int n_flush;
static unsigned windows[MAX_EV];
static int n_windows;

static unsigned rewritten(void)
{
    unsigned n = 0;

    for (unsigned i = 0; i < LINES; i++)
        if (fc_status[fc_core][i * STRIDE] == REWRITTEN)
            n++;
    return n;
}

/* every hook: what ran since the last one ran under last_cpsr */
static void observe(const char *where)
{
    unsigned n = rewritten();

    checks++;
    if (n != last_n)
    {
        if (dirty)
            fail("%s: %s: %u status words rewritten with %d lines still "
                 "dirty (lost)", ctx, where, n - last_n, dirty);
        if (!(last_cpsr & FIQ_STATUS))
            fail("%s: %s: status words %u..%u rewritten with the FIQ open",
                 ctx, where, last_n, n);
        if (!(last_cpsr & IRQ_STATUS))
            fail("%s: %s: status words %u..%u rewritten with the IRQ open",
                 ctx, where, last_n, n);
    }
    last_n = n;
}

volatile unsigned long *fc_ctl(void)
{
    static volatile unsigned long ctl;

    observe("CACHE_CTL");
    if (fc_cache_op & CACHE_OP_FLUSH)
    {
        fc_cache_op &= ~(unsigned long)CACHE_OP_FLUSH;
        if (n_flush < MAX_EV)
        {
            flush_ev[n_flush].cpsr = cpsr;
            flush_ev[n_flush].n = last_n;
        }
        n_flush++;
        dirty = 0;
    }
    ctl = CACHE_CTL_ENABLE;         /* never busy */
    return &ctl;
}

uint32_t fc_usec(void)
{
    static uint32_t t;

    observe("USEC_TIMER");
    return t += 3;
}

int disable_interrupt_save(int mask)
{
    int old = cpsr;

    observe("disable_interrupt_save");
    dirty += 3;                 /* the code before the mask wrote some */
    cpsr |= mask & IRQ_FIQ_STATUS;
    last_cpsr = cpsr;
    if (cpsr & IRQ_STATUS)
        masked_once = 1;
    return old;
}

void restore_interrupt(int x)
{
    observe("restore_interrupt");
    cpsr = x & IRQ_FIQ_STATUS;
    last_cpsr = cpsr;
    if (masked_once && last_n < LINES && !(cpsr & IRQ_STATUS))
        fail("%s: the IRQ opened with %u of %u status words rewritten",
             ctx, last_n, LINES);
    if (last_n > 0 && last_n < LINES && !(cpsr & FIQ_STATUS))
    {
        /* a window: a pending FIQ runs here (it writes no cached memory) */
        if (n_windows < MAX_EV)
            windows[n_windows] = last_n;
        n_windows++;
    }
}

/* ---- one call -------------------------------------------------------- */
static void run_case(unsigned core, int entry)
{
    static char name[64];
    int want_windows = (core == COP && !(entry & FIQ_STATUS)) ? 7 : 0;

    snprintf(name, sizeof(name), "%s entry %s%s", core == COP ? "COP" : "CPU",
             entry & IRQ_STATUS ? "irq-masked " : "irq-open ",
             entry & FIQ_STATUS ? "fiq-masked" : "fiq-open");
    ctx = name;
    fc_core = core;
    for (unsigned c = 0; c < 2; c++)
        for (unsigned i = 0; i < LINES; i++)
            fc_status[c][i * STRIDE] = SENTINEL;
    fc_cache_op = 0;
    cpsr = last_cpsr = entry;
    masked_once = 0;
    dirty = 10;
    last_n = 0;
    n_flush = n_windows = 0;

    commit_discard_dcache();
    observe("return");

    checks += 6;
    if (last_n != LINES)
        fail("%s: %u of %u status words rewritten", name, last_n, LINES);
    if (rewritten() != LINES || fc_status[core ^ 1][0] != SENTINEL)
        fail("%s: the other core's status words touched", name);
    if (dirty)
        fail("%s: returns with %d lines dirty", name, dirty);
    if (cpsr != entry)
        fail("%s: returns with mask 0x%x, entered with 0x%x", name, cpsr,
             entry);
    if (n_windows != want_windows)
        fail("%s: the FIQ opened %d times mid-rewrite, want %d", name,
             n_windows, want_windows);
    for (int i = 0; i < n_windows && i < MAX_EV; i++)
    {
        checks++;
        if (windows[i] % 64)
            fail("%s: the FIQ opened after %u lines (not a 64-line block)",
                 name, windows[i]);
    }
    if (core == COP)
    {
        checks += 2;
        if (n_flush < 2 || flush_ev[0].cpsr != entry || flush_ev[0].n)
            fail("%s: no bulk write-back before the mask (%d flushes; the "
                 "first under 0x%x)", name, n_flush,
                 n_flush ? flush_ev[0].cpsr : -1);
        if (n_flush < 2 || flush_ev[1].cpsr != (entry | IRQ_FIQ_STATUS)
            || flush_ev[1].n)
            fail("%s: no write-back with both masked before the rewrite",
                 name);
    }
    else
    {
        checks++;
        if (n_flush != 1 || flush_ev[0].cpsr != (entry | IRQ_FIQ_STATUS)
            || flush_ev[0].n)
            fail("%s: the write-back is not one, both masked, before the "
                 "rewrite (%d flushes)", name, n_flush);
    }
}

int main(void)
{
    static const int entries[] = { 0, FIQ_STATUS, IRQ_STATUS, IRQ_FIQ_STATUS };
    int cases = 0;

    for (unsigned core = 0; core < 2; core++)
        for (unsigned e = 0; e < 4; e++, cases++)
            run_case(core, entries[e]);
    /* the plain commit, either core: one flush, nothing rewritten */
    fc_core = COP;
    n_flush = 0;
    dirty = 4;
    cpsr = last_cpsr = 0;
    last_n = rewritten();
    commit_dcache();
    checks++;
    if (n_flush != 1 || dirty)
        fail("commit_dcache: %d flushes, %d dirty after", n_flush, dirty);
    printf("     %d calls, %d checks\n", cases + 1, checks);
    if (checks < 50)
        fail("vacuous: %d checks", checks);
    if (fails)
        printf("VERDICT: FAIL %d (%s)\n", fails, first_fail);
    else
        printf("VERDICT: PASS\n");
    return fails ? 1 : 0;
}
