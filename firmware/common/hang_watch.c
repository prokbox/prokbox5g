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

/* The hang watcher (hang_watch.h).  The tick task runs on the CPU in its
 * interrupt, so everything here is lock-free: no mutex, no file, no log
 * call, no printf; plain loops into the (uncached) record, as panic.c.
 *
 * How the stuck thread's place is found:
 *  - blocked, sleeping or switched out (on its core's run queue): the
 *    context its last switch saved (sp, lr), and a stack scan from sp;
 *  - the thread the tick interrupted (it runs on the CPU and never
 *    yields): the interrupted pc from the IRQ stack's top word (the IRQ
 *    handler's entry pushes lr, already less 4, highest; IRQs never nest
 *    here) and the System-mode sp and lr, read by a brief switch to System
 *    mode; the scan from that sp;
 *  - running on the COP (the shell's thread, UI split): no frame to read
 *    from the CPU; the scan starts at the lowest word its stack ever used
 *    (the deepest frame, the newest-looking), through the uncached alias. */
#include <stddef.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "xcore.h"
#include "storage.h"
#include "panic_record.h"
#include "hang_watch.h"
#include "../kernel/thread-internal.h"

#define HANG_CHECK_TICKS   (HZ / 10)
#define HANG_SPINUP_GRACE  (10 * HZ)    /* a disk spin-up, retries and all */
#define HANG_MAX_RECORDS   4            /* per boot: a recovered one re-arms */

/* each slot's bound, seconds: the shell's loop and be_svc are never meant
 * to sit at work this long; the art builder and the storage thread's
 * idle callbacks write files and get more */
static const unsigned char hang_limit_s[HANG_SLOTS] = { 6, 6, 10, 10 };
static const char hang_slot_name[HANG_SLOTS][4] = { "ui", "be", "art", "sto" };
static const char hang_mx_name[HANG_MX_N][4] = { "ata", "i2c" };

struct hang_slot_rec hang_slots[HANG_SLOTS] XCORE_BSS;
const char *volatile hang_bench_step XCORE_BSS;
static volatile uint8_t hang_susp[HANG_SUSP_N] XCORE_BSS;
static struct mutex *hang_mx[HANG_MX_N];

/* the tick's own (CPU only) */
static struct hang_mon mon[HANG_SLOTS];
static int fired = -1;                  /* the slot recorded, -1: none */
static uint32_t fired_beats;
static long fired_stuck;                /* ticks without progress then */
static long fired_at;
static int records;
static bool task_on;
static volatile long recovered = -1;    /* stuck seconds; -1: none, -2: taken */
static volatile bool rearm_req XCORE_BSS;

/* ---- the threads' side ------------------------------------------------ */

static int self_slot(void)
{
    struct thread_entry *self = __running_self_entry();

    for (int s = 0; s < HANG_SLOTS; s++)
        if (hang_slots[s].t == self)
            return s;
    return -1;
}

static void hang_tick(void);

void hang_watch_register(enum hang_slot s)
{
    struct hang_slot_rec *h = &hang_slots[s];

    h->beats++;
    h->state = HANG_ST_WORK;
    h->paused = 0;
    h->t = __running_self_entry();
    /* the tick list is CPU-only (xcore.h); the storage thread, a CPU
     * thread, registers at boot, so the task is always on by then */
    if (!task_on && CURRENT_CORE == CPU)
    {
        task_on = true;
        tick_add_task(hang_tick);
    }
}

static int set_state(unsigned int st)
{
    int s = self_slot();
    int prev;

    if (s < 0)
        return -1;
    prev = hang_slots[s].state;
    hang_slots[s].state = st;
    return (s << 8) | prev;
}

int hang_idle_begin(void)
{
    return set_state(HANG_ST_IDLE);
}

int hang_peer_begin(void)
{
    return set_state(HANG_ST_PEER);
}

void hang_idle_end(int prev)
{
    struct hang_slot_rec *h;

    if (prev < 0)
        return;
    h = &hang_slots[prev >> 8];
    h->beats++;                     /* back at work: the clock starts now */
    h->state = prev & 0xff;
}

void hang_pause(enum hang_slot s, bool on)
{
    hang_slots[s].beats++;
    hang_slots[s].paused = on;
}

void hang_suspend(enum hang_susp why, bool on)
{
    hang_susp[why] = on;
}

void hang_watch_mutex(enum hang_mx i, struct mutex *m)
{
    hang_mx[i] = m;
}

bool hang_take_recovered(long *secs, const char **name)
{
    long r = recovered;

    if (r < 0)
        return false;
    recovered = -2;
    *secs = r;
    *name = fired >= 0 ? hang_slot_name[fired] : "?";
    return true;
}

void hang_rearm(void)
{
    rearm_req = true;               /* the tick clears it all */
}

/* ---- the record --------------------------------------------------------- */

struct hang_buf
{
    char *p;
    unsigned int len, size;
};

static void put_s(struct hang_buf *b, const char *s, unsigned int max)
{
    for (unsigned int i = 0; s && s[i] && i < max && b->len + 1 < b->size;
         i++)
        b->p[b->len++] = s[i];
    b->p[b->len] = '\0';
}

static void put_u(struct hang_buf *b, unsigned long v, int hex)
{
    char d[12];
    int n = 0;

    do
    {
        unsigned int k = hex ? (v & 15) : (v % 10);
        d[n++] = k < 10 ? '0' + k : 'a' + k - 10;
        v = hex ? v >> 4 : v / 10;
    } while (v && n < (int)sizeof(d));
    if (hex)
        while (n < 8)
            d[n++] = '0';
    while (n && b->len + 1 < b->size)
        b->p[b->len++] = d[--n];
    b->p[b->len] = '\0';
}

static const char *state_name(const struct thread_entry *t)
{
    switch (t->state)
    {
    case STATE_RUNNING:         return "ready";     /* switched out */
    case STATE_BLOCKED:         return "blocked";
    case STATE_SLEEPING:        return "sleeping";
    case STATE_BLOCKED_W_TMO:   return "blocked-tmo";
    case STATE_FROZEN:          return "frozen";
    default:                    return "killed";
    }
}

#if !defined(HAVE_SDL_THREADS)
/* the thread's stack as this core may read it: the COP's through the
 * uncached alias (SDRAM only; IRAM is not cached) */
static const uintptr_t *stack_view(const struct thread_entry *t,
                                   const uintptr_t *p)
{
#if NUM_CORES > 1
    if (t->core != CPU && (uintptr_t)p < UNCACHED_BASE_ADDR)
        return UNCACHED_ADDR(p);
#else
    (void)t;
#endif
    return p;
}

/* bytes ever used (the 0xdeadbeef fill's high-water); *ovf: the guard
 * word at the bottom is gone (the kernel's own check, thread.c) */
static unsigned int stack_used(const struct thread_entry *t, bool *ovf,
                               const uintptr_t **low)
{
    const uintptr_t *w = stack_view(t, t->stack);
    unsigned int n = t->stack_size / sizeof(uintptr_t);

    *ovf = false;
    *low = NULL;
    if (!t->stack || !n)
        return 0;
    *ovf = w[0] != DEADBEEF;
    for (unsigned int i = 0; i < n; i++)
        if (w[i] != DEADBEEF)
        {
            *low = &w[i];
            return (n - i) * sizeof(uintptr_t);
        }
    return 0;
}
#endif

#if (CONFIG_PLATFORM & PLATFORM_NATIVE) && defined(CPU_ARM)
/* System mode's sp and lr (the interrupted thread's), from IRQ mode */
static void sys_regs(uint32_t *sp, uint32_t *lr)
{
    uint32_t s, l, c;

    asm volatile (
        "mrs    %2, cpsr        \n"
        "msr    cpsr_c, #0xdf   \n"     /* System, IRQ and FIQ off */
        "mov    %0, sp          \n"
        "mov    %1, lr          \n"
        "msr    cpsr_c, %2      \n"     /* back to IRQ mode */
        : "=&r" (s), "=&r" (l), "=&r" (c));
    *sp = s;
    *lr = l;
}
#endif

static void hang_record(int slot, long stuck)
{
    struct thread_entry *t = hang_slots[slot].t;
    struct panic_record *r = panic_record_area();
    uint32_t pc = 0, lr = 0, sp = 0, scan = 0;
    const char *how = state_name(t);
    struct thread_entry *own = NULL;
    const char *bench = hang_bench_step;
    char msg[PANIC_REC_MSG];
    struct hang_buf b = { msg, 0, sizeof(msg) };
    bool any_ovf = false;

#if (CONFIG_PLATFORM & PLATFORM_NATIVE) && defined(CPU_ARM)
    {
        extern unsigned char irq_stack[];   /* crt0-pp.S: its top */
        bool ovf;
        const uintptr_t *low;

        if (t->core == CPU && __core_id_entry(CPU)->running == t)
        {
            sys_regs(&sp, &lr);
            /* the IRQ entry's first push: lr less 4, highest */
            pc = *(const volatile uint32_t *)((uintptr_t)irq_stack - 4);
            how = "running";
            scan = sp;
        }
#if NUM_CORES > 1
        else if (t->core != CPU && __core_id_entry(t->core)->running == t)
        {
            how = "running-cop";
            (void)stack_used(t, &ovf, &low);
            scan = (uint32_t)(uintptr_t)low;
        }
#endif
        else
        {
            sp = t->context.sp;
            lr = t->context.lr;
            scan = (uint32_t)(uintptr_t)stack_view(t, (uintptr_t *)sp);
        }
    }
#endif
#ifdef HAVE_PRIORITY_SCHEDULING
    if (t->blocker)
        own = t->blocker->thread;
#endif

    put_s(&b, "hang: ", 8);
    put_s(&b, hang_slot_name[slot], 4);
    put_s(&b, " no progress ", 16);
    put_u(&b, (unsigned long)(stuck / HZ), 0);
    put_s(&b, " s (", 4);
    put_s(&b, how, 12);
    put_s(&b, ") bench=", 8);
    put_s(&b, bench ? bench : "-", 12);
    if (t->wqp)
    {
        put_s(&b, " wq=", 4);
        put_u(&b, (uintptr_t)t->wqp, 1);
    }
    if (own)
    {
        put_s(&b, " own=", 5);
        put_s(&b, own->name, 10);
#if (CONFIG_PLATFORM & PLATFORM_NATIVE) && defined(CPU_ARM)
        put_s(&b, " olr=", 5);
        put_u(&b, own->context.lr, 1);
#endif
    }

    panic_record_write(r, PANIC_REC_HANG, pc, lr, sp, t->name, msg);
    r->magic = 0;
    r->uptime = current_tick;
#if (CONFIG_PLATFORM & PLATFORM_NATIVE) && defined(CPU_ARM)
    panic_stack_scan(scan, r->trace, PANIC_REC_TRACE);
#else
    (void)scan;
#endif
    /* info: "stk ui 2104/8192 sto 3072/3072! mx ata=storage i2c=-" */
    b = (struct hang_buf){ r->info, 0, PANIC_REC_INFO };
    put_s(&b, "stk", 4);
    for (int s = 0; s < HANG_SLOTS; s++)
    {
        struct thread_entry *w = hang_slots[s].t;
        if (!w)
            continue;
        put_s(&b, " ", 1);
        put_s(&b, hang_slot_name[s], 4);
#if !defined(HAVE_SDL_THREADS)
        {
            bool ovf;
            const uintptr_t *low;
            put_s(&b, " ", 1);
            put_u(&b, stack_used(w, &ovf, &low), 0);
            put_s(&b, "/", 1);
            put_u(&b, w->stack_size, 0);
            if (ovf)
            {
                put_s(&b, "!OVF", 4);
                any_ovf = true;
            }
        }
#endif
    }
    put_s(&b, " mx", 3);
    for (int i = 0; i < HANG_MX_N; i++)
    {
        struct thread_entry *o = hang_mx[i] ? hang_mx[i]->blocker.thread
                                            : NULL;
        put_s(&b, " ", 1);
        put_s(&b, hang_mx_name[i], 4);
        put_s(&b, "=", 1);
        put_s(&b, o ? o->name : "-", 10);
    }
    panic_record_seal(r);
    if (any_ovf)
    {
        /* a guard word gone is the likeliest cause: said in the message,
         * where every reader looks first */
        r->magic = 0;
        b = (struct hang_buf){ r->msg, 0, PANIC_REC_MSG };
        while (b.len < PANIC_REC_MSG - 1 && r->msg[b.len])
            b.len++;
        if (b.len > PANIC_REC_MSG - 16)
            b.len = PANIC_REC_MSG - 16;
        put_s(&b, " STACK OVERFLOW", 15);
        panic_record_seal(r);
    }
    if (panic_record_tail_hook)
    {
        r->magic = 0;
        panic_record_tail_hook(r->tail, PANIC_REC_TAIL);
        r->tail[PANIC_REC_TAIL - 1] = '\0';
        panic_record_seal(r);
    }
    panic_record_sync();
}

/* ---- the watcher -------------------------------------------------------- */

static void hang_tick(void)
{
    static unsigned int div;
    long now = current_tick;
    long grace = 0;
    bool susp = false;

    if (++div < HANG_CHECK_TICKS)
        return;
    div = 0;
    for (int i = 0; i < HANG_SUSP_N; i++)
        susp |= hang_susp[i];
    if (rearm_req || susp)
    {
        /* every clock starts afresh (a USB session, a shutdown, the
         * record logged) */
        for (int s = 0; s < HANG_SLOTS; s++)
        {
            mon[s].seen = hang_slots[s].beats;
            mon[s].since = now;
        }
        if (rearm_req)
        {
            rearm_req = false;
            fired = -1;
            recovered = -1;
        }
        return;
    }
    if (fired >= 0)
    {
        /* recorded: has it got going again? (the shell logs the record
         * then, and re-arms) */
        if (recovered == -1 && hang_slots[fired].beats != fired_beats)
            recovered = (fired_stuck + (now - fired_at)) / HZ;
        return;
    }
    if (records >= HANG_MAX_RECORDS)
        return;
#if (CONFIG_STORAGE & STORAGE_ATA) && !defined(SIMULATOR)
    if (ata_spinning_up())
        grace = HANG_SPINUP_GRACE;
#endif
    for (int s = 0; s < HANG_SLOTS; s++)
    {
        struct hang_slot_rec *h = &hang_slots[s];
        uint32_t beats;
        unsigned int st;

        if (!h->t)
            continue;
        beats = h->beats;
        st = h->paused ? HANG_ST_IDLE : h->state;
        if (!hang_overdue(&mon[s], beats, st, now, hang_limit_s[s] * HZ,
                          grace, HANG_PEER_GRACE_MS * HZ / 1000))
            continue;
        /* never over a real panic's record not yet logged: look again
         * next time (it is logged early in the boot) */
        if (panic_record_pending(panic_record_area()))
            return;
        fired = s;
        fired_beats = beats;
        fired_at = now;
        fired_stuck = now - mon[s].since;
        records++;
        hang_record(s, fired_stuck);
        return;
    }
}
