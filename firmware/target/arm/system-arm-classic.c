/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2006 by Thom Johansen
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
#include "system.h"
#include <stdio.h>
#include "lcd.h"
#include "font.h"
#include "gcc_extensions.h"

#include <get_sp.h>
#include <backtrace.h>
#ifdef HAVE_PANIC_RECORD
#include "panic_record.h"
#endif

static const char* const uiename[] = {
    "Undefined instruction",
    "Prefetch abort",
    "Data abort",
    "Divide by zero",
    "SWI",
#if defined(HAVE_PANIC_RECORD) && !defined(BOOTLOADER)
    "Jump to 0"         /* ProkBox: crt0-pp.S reset_reentry_handler (the
                           same guard, so the stock bootloader's bytes
                           stay upstream's) */
#endif
};

#ifdef HAVE_PANIC_RECORD
/* ProkBox: the interrupted r0-r12 at a data abort (ARM7TDMI has no fault
 * address register, so the base register is the next best thing), saved
 * before the handler uses any; UIE() puts r0-r7 in the record's message */
uint32_t uie_abort_regs[13];
bool uie_abort_regs_ok;
/* ProkBox: the CPSR a branch to address 0 arrived with (crt0-pp.S) */
uint32_t uie_reentry_cpsr;
#endif

void __attribute__((weak,naked)) data_abort_handler(void)
{
    asm volatile(
#ifdef HAVE_PANIC_RECORD
        "stmfd  sp!, {r0}           \n"
        "ldr    r0, =uie_abort_regs \n"
        "stmia  r0, {r0-r12}        \n"    /* r0's slot fixed below */
        "ldmfd  sp!, {r1}           \n"
        "str    r1, [r0]            \n"
        "ldr    r0, =uie_abort_regs_ok \n"
        "mov    r1, #1              \n"
        "strb   r1, [r0]            \n"
#endif
        "sub    r0, lr, #8  \n"
        "mov    r1, #2      \n"
        "b      UIE         \n"
#ifdef HAVE_PANIC_RECORD
        ".ltorg             \n"
#endif
        );
}

void __attribute__((weak,naked)) software_int_handler(void)
{
    asm volatile(
        "sub    r0, lr, #4  \n"
        "mov    r1, #4      \n"
        "b      UIE         \n"
        );
}

void __attribute__((weak,naked)) reserved_handler(void)
{
    asm volatile(
        "sub    r0, lr, #4  \n"
        "mov    r1, #4      \n"
        "b      UIE         \n"
        );
}

void __attribute__((weak,naked)) prefetch_abort_handler(void)
{
    asm volatile(
        "sub    r0, lr, #4  \n"
        "mov    r1, #1      \n"
        "b      UIE         \n"
        );
}

void __attribute__((weak,naked)) undef_instr_handler(void)
{
    asm volatile(
        "sub    r0, lr, #4    \n"
#ifdef USE_THUMB
        "mrs    r1, spsr      \n"
        "tst    r1, #(1 << 5) \n" // T bit set ?
        "subne  r0, lr, #2    \n" // if yes, offset to THUMB instruction
#endif
        "mov    r1, #0        \n"
        "b      UIE           \n"
        );
}

/* Unexpected Interrupt or Exception handler. Currently only deals with
   exceptions, but will deal with interrupts later.
 */
void NORETURN_ATTR UIE(unsigned int pc, unsigned int num)
{
    /* safe guard variable - we call backtrace() only on first
     * UIE call. This prevent endless loop if backtrace() touches
     * memory regions which cause abort
     */
    static bool triggered = false;

#ifdef HAVE_PANIC_RECORD
    {   /* ProkBox: the interrupted thread's lr and sp are System mode's
         * banked pair (an exception taken in IRQ mode shows the thread the
         * IRQ interrupted). ARM7TDMI has no fault address register. */
        uint32_t lr, sp;
        asm volatile("mrs   r2, cpsr        \n"
                     "msr   cpsr_c, #0xdf   \n"
                     "mov   %0, lr          \n"
                     "mov   %1, sp          \n"
                     "msr   cpsr_c, r2      \n"
                     : "=&r"(lr), "=&r"(sp) : : "r2", "lr");
        if (num == 2 && uie_abort_regs_ok)
        {
            /* the base register of the faulting load or store is among
             * r0-r7 for Thumb code (fits the 120-byte message) */
            static char msg[120];
            const uint32_t *g = uie_abort_regs;
            snprintf(msg, sizeof(msg), "%s %lx %lx %lx %lx %lx %lx %lx %lx",
                     uiename[num], (unsigned long)g[0], (unsigned long)g[1],
                     (unsigned long)g[2], (unsigned long)g[3],
                     (unsigned long)g[4], (unsigned long)g[5],
                     (unsigned long)g[6], (unsigned long)g[7]);
            panic_record_save(num + 1, pc, lr, sp, msg);
        }
        else if (num == 5)
        {
            /* a branch to 0: pc is the branching mode's lr, the caller's
             * return address; the mode it ran in, and whose stack System
             * mode's sp is in (an overflow shows as another's) */
            static char msg[120];
            const char *own = panic_stack_owner(sp);
            snprintf(msg, sizeof(msg), "%s from lr=%08lx mode=%02lx "
                     "irq=%s sp=%08lx stack=%s", uiename[num],
                     (unsigned long)pc,
                     (unsigned long)(uie_reentry_cpsr & 0x1f),
                     (uie_reentry_cpsr & 0x80) ? "off" : "on",
                     (unsigned long)sp, own ? own : "-");
            panic_record_save(num + 1, pc, lr, sp, msg);
        }
        else
            panic_record_save(num + 1, pc, lr, sp, uiename[num]);
    }
#endif

#ifdef HAVE_PANIC_RECORD    /* ProkBox: no screen, the record has it */
    (void)triggered;
#else
#if LCD_DEPTH > 1
    lcd_set_backdrop(NULL);
    lcd_set_drawinfo(DRMODE_SOLID, LCD_BLACK, LCD_WHITE);
#endif
    unsigned line = 0;

    lcd_setfont(FONT_SYSFIXED);
    lcd_set_viewport(NULL);
    lcd_clear_display();
    lcd_putsf(0, line++, "%s at %08x" IF_COP(" (%d)"), uiename[num], pc IF_COP(, CURRENT_CORE));

#if !defined(CPU_ARM7TDMI) && (CONFIG_CPU != RK27XX) /* arm7tdmi has no MPU/MMU */
    if(num == 1 || num == 2) /* prefetch / data abort */
    {
        register unsigned status;

#if ARM_ARCH >= 6
        /* ARMv6 has 2 different registers for prefetch & data aborts */
        if(num == 1)    /* instruction prefetch abort */
            asm volatile( "mrc p15, 0, %0, c5, c0, 1\n" : "=r"(status));
        else
#endif
            asm volatile( "mrc p15, 0, %0, c5, c0, 0\n" : "=r"(status));

        lcd_putsf(0, line++, "FSR 0x%x", status);

        unsigned int domain = (status >> 4) & 0xf;
        unsigned int fault = status & 0xf;
#if ARM_ARCH >= 6
        fault |= (status & (1<<10)) >> 6; /* fault is 5 bits on armv6 */
#endif
        lcd_putsf(0, line++, "(domain %d, fault %d)", domain, fault);

        if(num == 2) /* data abort */
        {
            register unsigned address;
            /* read FAR (fault address register) */
            asm volatile( "mrc p15, 0, %0, c6, c0\n" : "=r"(address));
            lcd_putsf(0, line++, "address 0x%8x", address);
#if ARM_ARCH >= 6
            lcd_putsf(0, line++, (status & (1<<11)) ? "(write)" : "(read)");
#endif
        }
    }   /* num == 1 || num == 2 // prefetch/data abort */
#endif /* !defined(CPU_ARM7TDMI */

#ifdef HAVE_RB_BACKTRACE
    if (!triggered)
    {
        triggered = true;
        rb_backtrace(pc, __get_sp(), &line);
    }
#else
    (void)triggered;
#endif

    lcd_update();
#endif /* !HAVE_PANIC_RECORD */

    disable_interrupt(IRQ_FIQ_STATUS);

#if defined(HAVE_IPOD_UI) && defined(HAVE_PANIC_RECORD)
    panic_shell_exit();             /* ProkBox: restart, never a halt */
#endif
    system_exception_wait(); /* If this returns, try to reboot */
    system_reboot();
    while (1);       /* halt */
}

/* Needs to be here or gcc won't find it */
void __attribute__((naked)) __div0(void)
{
    asm volatile (
        "ldr    r0, [sp]    \r\n"
        "sub    r0, r0, #4  \r\n"
        "mov    r1, #3      \r\n"
        "b      UIE         \r\n"
    );
}
