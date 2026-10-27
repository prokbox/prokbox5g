/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 by wavey@wavey.org
 * Modified for ProkBox5g (2026); see docs/changes-from-rockbox.md.
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

#include "config.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "panic.h"
#ifdef HAVE_PP_GOV_DVS
#include "pgov.h"
#endif
#include "kernel.h"
#include "lcd.h"
#include "font.h"
#include "debug.h"
#include "led.h"
#include "power.h"
#include "system.h"
#include "logf.h"
#include "rbversion.h"
#ifdef HAVE_PANIC_RECORD
#include "panic_record.h"
#ifdef HAVE_POWER_SLEEP
#include "power_sleep.h"
#endif
#include "../kernel/thread-internal.h"
#endif

#ifdef HAVE_RB_BACKTRACE
#include "gcc_extensions.h"
#include "backtrace.h"
#endif

#if (defined(CPU_MIPS) && (CONFIG_PLATFORM & PLATFORM_NATIVE))
/* TODO: see comment above exception_dump in system-mips.c */
char panic_buf[128];
#else
static char panic_buf[128];
#endif

#define LINECHARS (LCD_WIDTH/SYSFONT_WIDTH) - 2

#if defined(HAVE_PANIC_RECORD) && !(CONFIG_PLATFORM & PLATFORM_NATIVE)
/* ProkBox, sim: the record's stand-in for RAM a reset keeps is a file on
 * the run's own disk (--root), not in the shared build directory: the
 * gate's parallel workers share that, and one script's record (panic_fake,
 * hang_ui) showed in another's report (ipodui-diag-report, "last hang
 * record" for "last panic: none"; p19-gatefix4).  Hidden in the system
 * folder; shots.sh removes it before a script's first boot. */
int sim_get_os_path(char *buffer, const char *path, size_t bufsize);

static FILE *sim_record_open(const char *mode)
{
    char path[1024];                    /* a host path */

    if (sim_get_os_path(path, "/.rockbox/.sim_panic_record.bin",
                        sizeof(path)) < 0)
        return NULL;
    return fopen(path, mode);
}
#endif

#ifdef HAVE_PANIC_RECORD
/* ProkBox: PANIC_REC_RESERVE bytes under the codec buffer, below which
 * core_allocator_init() ends the audio buffer; through the uncached alias
 * only. Computed here rather than read from audiobufend, so a panic before
 * the allocator is up still lands in the same place. */
struct panic_record *panic_record_area(void)
{
#if (CONFIG_PLATFORM & PLATFORM_NATIVE)
    extern unsigned char audiobufend_lds[];
    uintptr_t end = (uintptr_t)audiobufend_lds;

    if (MEMORYSIZE == 64 && probed_ramsize != 64)
        end -= 32 << 20;            /* as core_allocator_init() */
    return (struct panic_record *)UNCACHED_ADDR(end - PANIC_REC_RESERVE);
#else
    static struct panic_record rec;
    static bool loaded;

    if (!loaded)
    {
        FILE *f = sim_record_open("rb");
        loaded = true;
        if (f)
        {
            if (fread(&rec, sizeof(rec), 1, f) != 1)
                rec.magic = 0;
            fclose(f);
        }
    }
    return &rec;
#endif
}

#ifdef SIMULATOR
void panic_record_sync(void)
{
    FILE *f = sim_record_open("wb");

    if (f)
    {
        fwrite(panic_record_area(), sizeof(struct panic_record), 1, f);
        fclose(f);
    }
}
#endif

void (*panic_record_tail_hook)(char *buf, unsigned int size);

#if (CONFIG_PLATFORM & PLATFORM_NATIVE) && defined(CPU_ARM)
/* a code address: in .text or the IRAM code (thumb bit ignored) */
static bool code_addr(uint32_t a)
{
    extern unsigned char loadaddress[], _text_end[];
    extern unsigned char _iramstart[], _iramend[];
    a &= ~1u;
    return (a >= (uintptr_t)loadaddress + 0x200 && a < (uintptr_t)_text_end)
        || (a >= (uintptr_t)_iramstart && a < (uintptr_t)_iramend);
}

/* return-address candidates above sp: a stack scan, not an unwind (an
 * old frame's leftovers can appear); enough to name the caller chain.
 * Also the hang watcher's (hang_watch.c) */
void panic_stack_scan(uint32_t sp, uint32_t *out, unsigned int n)
{
    const uint32_t *w = (const uint32_t *)(sp & ~3u);
    unsigned int found = 0;

    if (!sp)
        return;
    for (unsigned int i = 0; i < 256 && found < n; i++)
    {
        uint32_t v = w[i];
        if (code_addr(v))
            out[found++] = v;
    }
}

/* the thread whose stack holds sp, or NULL (UIE's jump-to-0 message) */
const char *panic_stack_owner(uint32_t sp)
{
    for (unsigned int i = 0; i < MAXTHREADS; i++)
    {
        const struct thread_entry *t = __thread_slot_entry(i);
        uintptr_t lo;

        if (!t || t->state == STATE_KILLED || !t->stack)
            continue;
        lo = (uintptr_t)t->stack;
        if (sp >= lo && sp <= lo + t->stack_size)
            return t->name ? t->name : "?";
    }
    return NULL;
}
#endif

void panic_record_save(uint32_t kind, uint32_t pc, uint32_t lr, uint32_t sp,
                       const char *msg)
{
    static bool saving;             /* a fault while saving: give up */
    static bool saved;              /* this boot's first panic is kept */
    struct thread_entry *t;
    struct panic_record *r;

    if (saving)
        return;
    if (saved)
    {
        /* a second panic in the same boot (the exit path's own, such as
         * a mutex taken in abort mode) only counts: the first one names
         * the fault */
        r = panic_record_area();
        r->count++;
        panic_record_seal(r);
        panic_record_sync();
        return;
    }
    saving = true;
    saved = true;
    t = __running_self_entry();
    r = panic_record_area();
    panic_record_write(r, kind, pc, lr, sp, t ? t->name : NULL, msg);
    /* ProkBox (2026-10-23 freeze): uptime, a stack scan and the log's
     * last lines, each added after the record is sealed once, so a fault
     * in any of them still leaves the basic record */
    r->magic = 0;
    r->uptime = current_tick;
#if (CONFIG_PLATFORM & PLATFORM_NATIVE) && defined(CPU_ARM)
    panic_stack_scan(sp, r->trace, PANIC_REC_TRACE);
#endif
    panic_record_seal(r);
    if (panic_record_tail_hook)
    {
        r->magic = 0;
        panic_record_tail_hook(r->tail, PANIC_REC_TAIL);
        r->tail[PANIC_REC_TAIL - 1] = '\0';
        panic_record_seal(r);
    }
    panic_record_sync();
    saving = false;
}

#ifdef HAVE_IPOD_UI
/* The reboot loop guard: two words at the end of the reserve (the sim:
 * a static), "PKGD" while armed. Set just before a panic's reboot; the
 * shell clears it once the next boot has run PANIC_GUARD_TICKS */
#define PANIC_GUARD_MAGIC 0x44474b50u
static uint32_t *guard_words(void)
{
#if (CONFIG_PLATFORM & PLATFORM_NATIVE)
    return (uint32_t *)((uintptr_t)panic_record_area()
                        + PANIC_REC_RESERVE - 8);
#else
    static uint32_t g[2];
    return g;
#endif
}

bool panic_guard_armed(void)
{
    uint32_t *g = guard_words();
    return g[0] == PANIC_GUARD_MAGIC && g[1] == ~PANIC_GUARD_MAGIC;
}

void panic_guard_clear(void)
{
    uint32_t *g = guard_words();
    g[0] = 0;
    g[1] = 0;
}

#if (CONFIG_PLATFORM & PLATFORM_NATIVE)
static void panic_guard_arm(void)
{
    uint32_t *g = guard_words();
    g[0] = PANIC_GUARD_MAGIC;
    g[1] = ~PANIC_GUARD_MAGIC;
}

/* ProkBox: no screen and no halt (a dead unit needing a hard reset is
 * worse than any message). Sound stops, then the unit restarts after
 * about a second; a second panic within PANIC_GUARD_TICKS of a panic's
 * restart switches off instead (or, with a cable in, which would wake it
 * straight back up, halts dark as before), so a panic at boot can't
 * loop. Interrupts are off: register writes and the polled timer only */
void panic_shell_exit(void)
{
#ifdef HAVE_PP_GOV_DVS
    /* ProkBox (review R1): the core rail to 0xEC first (raw: IRQs are
     * off), so neither a halt nor the restart leaves it low for Apple's
     * firmware */
    pgov2_rail_normal();
#endif
#ifdef IISCONFIG
    IISCONFIG &= ~IIS_TXFIFOEN;     /* no more samples: silence */
#endif
    udelay(1000000);
    if (panic_guard_armed() && current_tick < PANIC_GUARD_TICKS)
    {
        panic_guard_clear();
#if CONFIG_CHARGING && defined(IPOD_VIDEO)
        if (power_input_status() == POWER_INPUT_NONE)
        {
#ifdef HAVE_POWER_SLEEP
            /* register-level: no I2C mutex in an exception mode */
            pmu_raw_standby();
#else
            extern void pcf50605_standby_mode(void);
            pcf50605_standby_mode();
#endif
        }
#endif
        system_exception_wait();    /* halts */
    }
    panic_guard_arm();
    system_reboot();
    while (1);
}
#endif
#endif /* HAVE_IPOD_UI */
#endif /* HAVE_PANIC_RECORD */

#if defined(CPU_ARM) && defined(HAVE_RB_BACKTRACE)
void panicf_f( const char *fmt, ...) USED_ATTR;

/* we wrap panicf() here with naked function to catch SP value */
void __attribute__((naked)) panicf( const char *fmt, ...)
{
    (void)fmt;
    asm volatile ("mov r4, sp \n"
                  "b panicf_f \n"
                 );
    while (1);
}

/*
 * "Dude. This is pretty fucked-up, right here."
 */
void panicf_f( const char *fmt, ...)
{
    int sp;

    asm volatile ("mov %[SP],r4 \n"
                  : [SP] "=r" (sp)
                 );

    int pc = (int)__builtin_return_address(0);
#elif defined(BACKTRACE_MIPSUNWINDER)
void panicf( const char *fmt, ... )
{
    /* NOTE: these are obtained by the backtrace lib */
    const int pc = 0;
    const int sp = 0;
#else
void panicf( const char *fmt, ...)
{
#endif
    va_list ap;

#if (CONFIG_PLATFORM & PLATFORM_NATIVE)
    /* Disable interrupts */
#if defined(CPU_ARM_CLASSIC)
    disable_interrupt(IRQ_FIQ_STATUS);
#else
    set_irq_level(DISABLE_INTERRUPTS);
#endif
#endif /* SIMULATOR */

    va_start( ap, fmt );
    vsnprintf( panic_buf, sizeof(panic_buf), fmt, ap );
    va_end( ap );

#ifdef HAVE_PANIC_RECORD
#if defined(CPU_ARM) && defined(HAVE_RB_BACKTRACE)
    panic_record_save(0, pc, 0, sp, panic_buf);
#else
    panic_record_save(0, 0, 0, 0, panic_buf);
#endif
#endif

#ifndef HAVE_PANIC_RECORD   /* ProkBox: no screen, the record has it */
    lcd_set_viewport(NULL);

    int y = 1;

#if LCD_DEPTH > 1
    lcd_set_backdrop(NULL);
    lcd_set_drawinfo(DRMODE_SOLID, LCD_BLACK, LCD_WHITE);
#endif

    lcd_clear_display();
    lcd_setfont(FONT_SYSFIXED);
    lcd_puts(1, y++, (unsigned char *) "*PANIC* (" RBVERSION ")");
    {
        /* wrap panic line */
        int i, len = strlen(panic_buf);
        for (i=0; i<len; i+=LINECHARS) {
            unsigned char c = panic_buf[i+LINECHARS];
            panic_buf[i+LINECHARS] = 0;
            lcd_puts(1, y++, (unsigned char *)panic_buf+i);
            panic_buf[i+LINECHARS] = c;
        }
    }

#if defined(HAVE_RB_BACKTRACE)
    rb_backtrace(pc, sp, &y);
#endif
#ifdef ROCKBOX_HAS_LOGF
    logf_panic_dump(&y);
#endif

    lcd_update();
#endif /* !HAVE_PANIC_RECORD */
    DEBUGF("%s", panic_buf);

#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    if (cpu_boost_lock())
    {
        set_cpu_frequency(0);
        cpu_boost_unlock();
    }
#endif /* HAVE_ADJUSTABLE_CPU_FREQ */

#ifdef HAVE_ATA_POWER_OFF
    ide_power_enable(false);
#endif

#if defined(HAVE_IPOD_UI) && defined(HAVE_PANIC_RECORD) \
    && (CONFIG_PLATFORM & PLATFORM_NATIVE)
    panic_shell_exit();
#endif
    system_exception_wait(); /* if this returns, try to reboot */
    system_reboot();
    while (1);       /* halt */
}
