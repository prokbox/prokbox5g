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
 * P19 19-9's three COP bench rows (Developer > Debug; apps/ipodui/
 * devbcm_probes.c; docs/research/bcm2722/bench-p19.md, cards C1-C3):
 *   C1  engine 2 started and polled from the COP into our BCM scratch;
 *   C2  does a cached write on one core show through the other core's
 *       cached view (one shared data array, or one each)?
 *   C3  does the COP sleep while a thread there polls with yield()?
 * The pure logic (the classes, tallies and verdicts) is here, as static
 * inlines, so tests/coplab/ checks it on the host; the device half is in
 * bcm-devtest-hal.c (declared at the end).
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
#ifndef BCM_COPLAB_H
#define BCM_COPLAB_H

#include <stdint.h>
#include <stdbool.h>

/* ---- C1: engine 2 from the COP ------------------------------------------
 * The target is our video image's free SRAM (bcmfw/hostproto.h
 * HP_LAB_SRAM, 32 KB below the AAC tables, nothing there until an OPEN):
 * guard words at both ends inside the window, the piece between them. */
#define COPLAB_WIN          0x12B000u
#define COPLAB_WIN_SIZE     0x8000u
#define COPLAB_GUARD_WORDS  16
#define COPLAB_TARGET       (COPLAB_WIN + 4 * COPLAB_GUARD_WORDS)
#define COPLAB_BYTES        (COPLAB_WIN_SIZE - 8 * COPLAB_GUARD_WORDS)
#define COPLAB_WORDS        (COPLAB_BYTES / 4)
#define COPLAB_E2_US        100000  /* a piece's bound (E2PORT_TIMEOUT_US) */
#define COPLAB_JOB_MS       1000    /* a COP job's bound, any row */

/* A piece wholly inside the window (nothing else of the BCM's is touched) */
static inline bool coplab_in_window(uint32_t addr, uint32_t bytes)
{
    return bytes > 0 && !(bytes & 3) && !(addr & 3) &&
           addr >= COPLAB_WIN && bytes <= COPLAB_WIN_SIZE &&
           addr - COPLAB_WIN <= COPLAB_WIN_SIZE - bytes;
}

/* ---- C2: the shared cache data array ------------------------------------
 * One trial, one 16-byte line of our own (cached SDRAM), a writer core W
 * and a reader core R:
 *   1. both cores clean and drop their caches; the line's word set to
 *      v0 through the uncached alias; R reads the word cached (pre: v0,
 *      so R now holds the line);
 *   2. W reads the word cached (W holds it too), writes v1 cached, reads
 *      the uncached alias (u1: v0 while the write sits in W's cache),
 *      then writes v9 to memory through the uncached alias;
 *   3. R reads the word cached (r);
 *   4. the uncached alias again (p: still v9 unless W's dirty line went
 *      to memory meanwhile).
 * r == v0: R answered from its own copy, blind to W's write: one data
 * array per core (no coherency, as cores-and-threads.md section 4 says).
 * r == v1: R's lookup found W's data: one data array shared by both,
 * each core with its own status (tags).  r == v9: R missed (its line was
 * gone).  The voids say the trial proves nothing. */
enum coplab_cache_class
{
    COPLAB_C_SEPARATE = 0,  /* r == v0: each core its own data */
    COPLAB_C_SHARED,        /* r == v1: one data array */
    COPLAB_C_MISS,          /* r == v9: R's line was evicted */
    COPLAB_C_SETUP,         /* pre != v0: the set-up read went wrong */
    COPLAB_C_THROUGH,       /* u1 == v1: W's write reached memory at once
                               (write-through or no allocate) */
    COPLAB_C_EVICT,         /* p != v9: W's dirty line was written back
                               before R read */
    COPLAB_C_ODD,           /* r is none of them */
    COPLAB_C_N
};

struct coplab_cache_trial
{
    uint32_t v0, v1, v9;    /* the trial's three values */
    uint32_t pre, u1, r, p; /* the readings */
};

/* Three values that differ in both halves and from each other; seq
 * changes them every trial so no earlier trial's word matches */
static inline void coplab_cache_values(struct coplab_cache_trial *t,
                                       uint32_t seq)
{
    uint32_t s = seq & 0xFFFF;

    t->v0 = 0xC0DE0000u | s;
    t->v1 = 0xA11C0000u | (s ^ 0x5A5A);
    t->v9 = 0x99990000u | (s ^ 0xA5A5);
    t->pre = t->u1 = t->r = t->p = 0;
}

static inline int coplab_cache_class(const struct coplab_cache_trial *t)
{
    if (t->pre != t->v0)
        return COPLAB_C_SETUP;
    if (t->u1 == t->v1)
        return COPLAB_C_THROUGH;
    if (t->p != t->v9)
        return COPLAB_C_EVICT;
    if (t->r == t->v0)
        return COPLAB_C_SEPARATE;
    if (t->r == t->v1)
        return COPLAB_C_SHARED;
    if (t->r == t->v9)
        return COPLAB_C_MISS;
    return COPLAB_C_ODD;
}

static inline const char *coplab_cache_name(int c)
{
    static const char *const n[COPLAB_C_N] = {
        "separate", "shared", "miss", "setup", "through", "evict", "odd" };

    return c >= 0 && c < COPLAB_C_N ? n[c] : "?";
}

/* The verdict over a direction's (or both directions') counts */
enum coplab_cache_verdict
{
    COPLAB_V_SEPARATE = 0,  /* only "separate" among the valid trials */
    COPLAB_V_SHARED,        /* only "shared" */
    COPLAB_V_MIXED,         /* both: unexplained, rerun and look */
    COPLAB_V_NONE           /* fewer than COPLAB_MIN_VALID valid trials */
};
#define COPLAB_MIN_VALID 4

static inline int coplab_cache_verdict(const uint32_t n[COPLAB_C_N])
{
    uint32_t sep = n[COPLAB_C_SEPARATE], sh = n[COPLAB_C_SHARED];

    if (n[COPLAB_C_ODD] || (sep && sh))
        return COPLAB_V_MIXED;
    if (sep + sh < COPLAB_MIN_VALID)
        return COPLAB_V_NONE;
    return sep ? COPLAB_V_SEPARATE : COPLAB_V_SHARED;
}

static inline const char *coplab_verdict_name(int v)
{
    static const char *const n[4] = {
        "SEPARATE (no coherency)", "SHARED data array", "MIXED: rerun",
        "INCONCLUSIVE" };

    return v >= 0 && v < 4 ? n[v] : "?";
}

/* ---- C3: the COP's sleep under yield() polling --------------------------
 * The CPU samples the COP's control register (COP_CTL bit 31, PROC_SLEEP:
 * set while that core sleeps) over a fixed window while a COP thread
 * waits in one of three ways; asleep samples give the sleep share, and
 * awake-to-asleep edges a lower bound on the sleep entries.  The kernel's
 * own idle time (devstats idle_us[COP], when the build has it) is the
 * cross-check. */
enum coplab_wait
{
    COPLAB_W_YIELD = 0,     /* while (!flag) yield(); */
    COPLAB_W_SLEEP1,        /* while (!flag) sleep(1); (lcd_e2_push's way) */
    COPLAB_W_BLOCK,         /* semaphore_wait() until the CPU releases it */
    COPLAB_W_N
};

struct coplab_sleep
{
    uint32_t samples, asleep, entries;
    bool last;              /* the previous sample was asleep */
    uint32_t window_us;     /* the window as run */
    uint32_t loops;         /* the COP thread's wait turns */
    uint32_t idle_us;       /* the kernel's idle time for the COP, or
                               ~0u when the build does not count it */
};

static inline void coplab_sleep_init(struct coplab_sleep *s)
{
    s->samples = s->asleep = s->entries = 0;
    s->last = false;
    s->window_us = s->loops = 0;
    s->idle_us = ~0u;
}

static inline void coplab_sleep_add(struct coplab_sleep *s, bool asleep)
{
    s->samples++;
    if (asleep)
    {
        s->asleep++;
        if (!s->last)
            s->entries++;
    }
    s->last = asleep;
}

/* The asleep share in tenths of a percent (0-1000) */
static inline uint32_t coplab_sleep_permille(const struct coplab_sleep *s)
{
    return s->samples ? (uint32_t)((uint64_t)s->asleep * 1000 / s->samples)
                      : 0;
}

/* The kernel's idle share in tenths of a percent, or ~0u */
static inline uint32_t coplab_idle_permille(const struct coplab_sleep *s)
{
    if (s->idle_us == ~0u || !s->window_us)
        return ~0u;
    if (s->idle_us >= s->window_us)
        return 1000;
    return (uint32_t)((uint64_t)s->idle_us * 1000 / s->window_us);
}

/* Yield polling against the blocking wait: the hypothesis (yield() never
 * lets the core idle while the poller is its only runnable thread) holds
 * when the yield window sleeps under 5 % and the blocking one over 50 % */
enum
{
    COPLAB_S_AWAKE = 0,     /* yield keeps the COP awake (as expected) */
    COPLAB_S_SLEEPS,        /* the COP sleeps under yield too */
    COPLAB_S_UNCLEAR,       /* the blocking control did not sleep either,
                               or the windows had no samples */
};

static inline int coplab_sleep_verdict(const struct coplab_sleep *yield,
                                       const struct coplab_sleep *block)
{
    uint32_t y = coplab_sleep_permille(yield),
             b = coplab_sleep_permille(block);

    if (!yield->samples || !block->samples || b < 500)
        return COPLAB_S_UNCLEAR;
    if (y < 50)
        return COPLAB_S_AWAKE;
    if (y >= 500)
        return COPLAB_S_SLEEPS;
    return COPLAB_S_UNCLEAR;
}

/* ---- The device half (bcm-devtest-hal.c; HAVE_COP_JOBS) ---------------- */
#if !defined(COPLAB_HOST_TEST)
/* A worker on the COP for the rows (its stack from buflib, pinned).
 * 0, or -1 no second core running / no memory.  bcm_cop_close() stops it;
 * after a job that never ended it leaves the worker and its stack alone
 * (bcm_cop_stuck(): reboot before anything else). */
int bcm_cop_open(void);
void bcm_cop_close(void);
bool bcm_cop_stuck(void);

/* C1: one engine-2 piece of bytes from src (word aligned, SDRAM, already
 * written back by the caller's core) to bcm_addr inside the C1 window,
 * started and polled on the COP (on_cop) or on the caller's thread.  The
 * engine is claimed and the port locked by the caller's thread around it.
 * rc: 0 done; 1 never ended (stopped); 2 the port not ready after; -1
 * refused (window, engine busy, port); -3 no worker; -4 the COP job
 * never finished (stuck) */
struct coplab_e2
{
    int32_t rc;
    uint32_t core;          /* the core that ran it: 0 CPU, 1 COP */
    uint32_t start_us;      /* e2port_start() (address, ready, cache) */
    uint32_t run_us;        /* start to the end seen */
    uint32_t polls;         /* busy polls */
    uint32_t wall_us;       /* the caller's whole wait */
};
int bcm_cop_e2_piece(uint32_t bcm_addr, const void *src, uint32_t bytes,
                     bool on_cop, struct coplab_e2 *out);

/* C2: one trial on the line (16 bytes, 16-byte aligned, cached SDRAM of
 * the caller's own); cpu_writes: the CPU is W and the COP R, else the
 * reverse.  Both cores' caches are cleaned and dropped before and after.
 * 0, or < 0 as bcm_cop_e2_piece */
int bcm_cop_cache_trial(uint32_t *line, bool cpu_writes, uint32_t seq,
                        struct coplab_cache_trial *t);

/* C3: one window of window_ms with the COP thread waiting as how
 * (enum coplab_wait); the CPU samples the COP's sleep bit throughout
 * (it runs no other thread meanwhile).  0, or < 0 as above */
int bcm_cop_sleep_window(int how, uint32_t window_ms,
                         struct coplab_sleep *s);
#endif

#endif /* BCM_COPLAB_H */
