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

/* Phase 18-1, the platform's soft sleep (docs/phase18-brief.md §3.1,
 * §4.2). Evidence tags as in docs/pp5022-reference/: [silicon] measured
 * on our unit, [rockbox-derived] known from upstream's working code,
 * [hypothesis] unproven (the device session's D2 rows prove or drop
 * them). Nothing here has run on silicon yet.
 *
 * Sequence, on the CPU, UI thread, IRQ and FIQ off throughout:
 *  1. refuse (WAKE_DEEP, out->refused) unless: no boost, no PLL hold, at
 *     24 MHz with the PLL off (Fit-1 C6's CPUFREQ_DEFAULT, so no clock
 *     change is made here), PCM stopped, no button but PLAY held, Timer
 *     2 free;
 *  2. the normal I2C driver idle (its mutex taken) and the COP parked:
 *     the COP's scheduler only sleeps with an empty run queue and no
 *     corelock (thread.c's idle), so "COP_CTL shows PROC_SLEEP" is its
 *     acknowledgement; with Timer 1 stopped and the COP's own enables
 *     masked, nothing wakes it until the CPU's core_wake() at exit;
 *  3. Timer 1 (the tick) stopped, every source masked, then the wake set
 *     enabled: the wheel (high 40), Hold as a GPIOA edge (high 32),
 *     VBUS (high 34, only if the USB driver had it on) and the poll
 *     timer (Timer 2, one-shot);
 *  4. PROC_SLEEP in a loop; each wake is read (packets by hand, the
 *     driver's handler never runs: IRQs are off at the core) and
 *     classified by sleep_classify(); poll wakes read the battery and the
 *     RTC by the raw PMU path;
 *  5. restore in reverse: timers, the GPIO edge, the opto as the switch
 *     says, the CPU's masks, then the COP's masks and a wake for the COP
 *     (CPU first, then COP: the boot rule), the driver's wheel and Hold
 *     state, IRQs, the I2C mutex.
 * Stack: this frame is 200 B (under the brief's 256 B); with its deepest
 * callees (the raw RTC read and its BCD conversion, or
 * clickwheel_sleep_exit()'s queue post) about 300 B of the UI thread's
 * 8 KB, over 256 B once callees count. Not in IRAM: the loop runs from
 * SDRAM, which stays refreshed while the cores sleep [hypothesis, D1].
 *
 * What can always wake it (review M2): the Timer 2 poll is armed for
 * every sleep, whatever Hold says, and every poll reads Hold and VBUS as
 * GPIO levels. So with Hold on at entry (opto off, source 40 masked) a
 * Hold-off is seen at the next poll at the latest even if the GPIOA
 * edge never wakes PROC_SLEEP: the opto is brought up and unmasked there
 * and the next press wakes. VBUS likewise (its source 34 is shared with
 * ports I-K and is masked here after its first wake). Residual risk: if
 * Timer 2 itself does not wake PROC_SLEEP at 24 MHz with the PLL off,
 * nothing but a button (opto powered) or MENU+SELECT ends the sleep.
 * That is exactly D2's first row, run before any other. */

#include <string.h>
#include "config.h"
#include "system.h"
#include "cpu.h"
#include "button.h"
#include "i2c-pp.h"
#include "thread.h"                 /* core_wake() */
#include "pcm-internal.h"           /* pcm_is_playing() */
#include "power_sleep.h"
#ifdef HAVE_PP_GOV
#include "pgov.h"                   /* ProkBox: pgov_settle() */
#endif

#define WHEEL_CTL    (*(volatile unsigned long *)0x7000C100)
#define WHEEL_STAT   (*(volatile unsigned long *)0x7000C104)
#define WHEEL_DATA   (*(volatile unsigned long *)0x7000C140)
#define WHEEL_READY  0x04000000     /* WHEEL_STAT bit 26 [rockbox-derived] */
#define HOLD_BIT     0x20           /* GPIOA bit 5, low = Hold on */
#define VBUS_BIT     0x10           /* GPIOL bit 4, high = VBUS */
#define TIMER_EN     0x80000000     /* TIMERn_CFG enable; no repeat bit:
                                     * one-shot [rockbox-derived] */
#define TIMER2_MAX_S 500            /* period field is 29 bits of us */
#define COP_PARK_US  100000         /* bounded wait for the COP's idle */

static bool hold_on(void)
{
    return !(GPIOA_INPUT_VAL & HOLD_BIT);
}

/* A packet waiting; the status means nothing with the opto unpowered */
static bool wheel_ready(void)
{
    return (DEV_EN & DEV_OPTO) && (WHEEL_STAT & WHEEL_READY);
}

static bool vbus_on(void)
{
    return (GPIOL_INPUT_VAL & VBUS_BIT) != 0;
}

/* The COP in its scheduler's idle sleep, seen twice 50 us apart */
static bool cop_parked(void)
{
    uint32_t t0 = USEC_TIMER;

    for (;;)
    {
        if (PROC_CTL(COP) & PROC_SLEEP)
        {
            udelay(50);
            if (PROC_CTL(COP) & PROC_SLEEP)
                return true;
        }
        if (USEC_TIMER - t0 > COP_PARK_US)
            return false;
    }
}

/* Hold as a level interrupt on the opposite of the switch now: the edge
 * that comes next [hypothesis: GPIOA interrupts from PROC_SLEEP, D2 row
 * iii]; the polarity flip is the VBUS handler's [rockbox-derived] */
static void hold_arm(void)
{
    GPIO_WRITE_BITWISE(GPIOA_INT_LEV, (GPIOA_INPUT_VAL & HOLD_BIT) ^ HOLD_BIT,
                       HOLD_BIT);
    GPIOA_INT_CLR = HOLD_BIT;
}

/* The poll: Timer 2 one-shot (the default), or the lab's PROC_WAIT_CNT
 * seconds countdown [hypothesis: PP5022 only, never used by Rockbox] */
static void poll_arm(uint32_t s, bool waitcnt)
{
    if (s == 0)
        s = 1;
    if (s > TIMER2_MAX_S)
        s = TIMER2_MAX_S;
    if (!waitcnt)
    {
        TIMER2_CFG = 0;
        (void)TIMER2_VAL;
        TIMER2_CFG = TIMER_EN | (s * 1000000u - 1);
    }
}

static void wheel_resync(void)
{
    /* the driver's resync [rockbox-derived] (input.md §3) */
    udelay(2000);
    WHEEL_CTL &= ~0x60000000;
    WHEEL_STAT |= WHEEL_READY;
    WHEEL_CTL |= 0x60000000;
}

void power_sleep(const struct sleep_req *rq, struct sleep_result *out)
{
    struct sleep_core_req crq;
    struct sleep_core_state st;
    struct sleep_core_ev ev;
    struct sleep_lab *lab = rq->lab;
    const bool waitcnt = lab && lab->timer == SLEEP_LAB_WAITCNT;
    const bool opto_kept = lab && lab->opto_kept;
    uint32_t int_lo, int_hi, cop_lo, cop_hi, t1cfg, a_lev;
    uint32_t gpio_en[4];            /* ports A-D: GPIO0_IRQ's sources */
    uint32_t rtc, arm_rtc, arm_us, poll_s, lo, hi;
    uint16_t mv = 0;
    enum sleep_wake why = WAKE_NONE;
    bool again = false;
    bool muted = false;             /* SACT_WHEEL_MUTE until the next poll */
    int irq;

    memset(out, 0, sizeof(*out));
    out->why = WAKE_DEEP;
#ifdef HAVE_PP_GOV
    pgov_settle();      /* the governor: band 0 now, not after its dwell */
#endif
    out->sdram_crc_ok = SLEEP_CRC_UNCHECKED;

    if (CURRENT_CORE != CPU)
        out->refused = SLEEP_REFUSE_CORE;
    else if (get_cpu_boost_counter() != 0 || cpu_pll_holds() != 0)
        out->refused = SLEEP_REFUSE_BOOST;
    else if (cpu_frequency != CPUFREQ_DEFAULT)
        out->refused = SLEEP_REFUSE_CLOCK;
    else if (pcm_is_playing())
        out->refused = SLEEP_REFUSE_PCM;
    else if (rq->buttons_at_entry & ~WBTN_PLAY)
        out->refused = SLEEP_REFUSE_KEYS;
    else if (!waitcnt && (TIMER2_CFG & TIMER_EN))
        out->refused = SLEEP_REFUSE_TIMER2;
    if (out->refused)
        return;

    i2c_lock();                     /* no kernel-path transaction in flight */
    irq = disable_interrupt_save(IRQ_FIQ_STATUS);

    if (!cop_parked())
        out->refused = SLEEP_REFUSE_COP;
    else if (!pmu_raw_rtc_seconds(&rtc))
        out->refused = SLEEP_REFUSE_RTC;
    if (out->refused)
    {
        restore_interrupt(irq);
        i2c_unlock();
        return;
    }

    crq.deep_after_s = rq->deep_after_s;
    if (crq.deep_after_s == 0 || crq.deep_after_s > POWER_SLEEP_MAX_S)
        crq.deep_after_s = POWER_SLEEP_MAX_S;
    crq.alarm_rtc = rq->alarm_rtc;
    crq.bcm_cut_after_s = rq->bcm_cut_after_s;
    crq.hold_wake_s = rq->hold_wake_s;
    crq.low_mv = rq->low_mv;
    crq.poll_s = rq->poll_s;
    crq.buttons_at_entry = rq->buttons_at_entry;
    crq.wake_on_hold = rq->wake_on_hold;
    sleep_core_begin(&st, &crq, rtc, hold_on(), vbus_on());

    /* 3. save, stop the tick, mask everything, enable the wake set */
    cop_lo = COP_INT_EN_STAT;
    cop_hi = COP_HI_INT_EN_STAT;
    COP_INT_DIS = 0xFFFFFFFF;
    COP_HI_INT_DIS = 0xFFFFFFFF;
    int_lo = CPU_INT_EN_STAT;
    int_hi = CPU_HI_INT_EN_STAT;
    t1cfg = TIMER1_CFG;
    TIMER1_CFG = 0;
    (void)TIMER1_VAL;
    CPU_INT_DIS = 0xFFFFFFFF;
    CPU_HI_INT_DIS = 0xFFFFFFFF;
    /* high source 32 is ports A-D together: only Hold may raise it */
    gpio_en[0] = GPIOA_INT_EN;
    gpio_en[1] = GPIOB_INT_EN;
    gpio_en[2] = GPIOC_INT_EN;
    gpio_en[3] = GPIOD_INT_EN;
    GPIOB_INT_EN = 0;
    GPIOC_INT_EN = 0;
    GPIOD_INT_EN = 0;
    a_lev = GPIOA_INT_LEV;

    /* Hold on at entry: the driver already powered the opto off; H2's
     * fallback brings it up for the whole sleep (Rev 4, N5) */
    if ((opto_kept || !hold_on()) && !(DEV_EN & DEV_OPTO))
        clickwheel_sleep_opto_init();   /* or Hold went off < a tick ago */
    hold_arm();
    GPIOA_INT_EN = HOLD_BIT;
    /* the wheel's source only while the opto is powered: with it off a
     * raw read's I2C completion is the only thing source 40 can carry */
    CPU_HI_INT_EN = ((DEV_EN & DEV_OPTO) ? I2C_MASK : 0) | GPIO0_MASK
                  | (int_hi & GPIO2_MASK);
    CPU_INT_EN = HI_MASK | (waitcnt ? 0 : TIMER2_MASK);

    poll_s = sleep_poll_deadline(&crq, 0, rtc);
    poll_arm(poll_s, waitcnt);
    arm_rtc = rtc;
    arm_us = USEC_TIMER;

    /* 4. the loop */
    while (why == WAKE_NONE)
    {
        bool rtc_read = false;

        if (!again)
        {
            if (waitcnt)    /* sleep for the countdown, seconds unit */
                PROC_CTL(CPU) = PROC_WAIT_CNT | PROC_CNT_SEC
                              | (poll_s > 255 ? 255 : poll_s);
            else
                sleep_core(CPU);
            wake_core(CPU);         /* clear the control word, as
                                     * core_sleep() does after a wake */
        }
        again = false;

        memset(&ev, 0, sizeof(ev));
        lo = CPU_INT_STAT;
        hi = CPU_HI_INT_STAT;
        if (lo & TIMER2_MASK)
        {
            (void)TIMER2_VAL;       /* acknowledges [rockbox-derived] */
            TIMER2_CFG = 0;
            ev.src |= SRC_TIMER;
        }
        else if (waitcnt && !(hi & (I2C_MASK | GPIO0_MASK | GPIO2_MASK)))
            ev.src |= SRC_TIMER;    /* nothing pending: the countdown */
        else if (USEC_TIMER - arm_us > poll_s * 1000000u + 1000000u)
            ev.src |= SRC_TIMER;    /* backstop: overdue by a second */
        if ((hi & GPIO0_MASK) && (GPIOA_INT_STAT & HOLD_BIT))
        {
            hold_arm();             /* the next edge; clears this one */
            ev.src |= SRC_GPIO;
        }
        if (hi & GPIO2_MASK)
        {
            /* VBUS: its handler acknowledges it once IRQs are back; until
             * then the poll covers it (an unplug must not storm) */
            CPU_HI_INT_DIS = GPIO2_MASK;
            ev.src |= SRC_GPIO;
        }
        if (ev.src & SRC_TIMER)
        {
            ev.poll_s = (uint16_t)poll_s;
            muted = false;          /* read the wheel again at each poll */
        }
        ev.hold_on = hold_on();     /* levels, read at every wake */
        ev.vbus = vbus_on();

        /* the raw reads, with the wheel's source masked (it is the I2C
         * block's interrupt, quirk 19): at a poll, and at a Hold edge
         * (Q12 needs the time since entry) */
        if ((ev.src & SRC_TIMER) || ev.hold_on != st.hold_on)
        {
            CPU_HI_INT_DIS = I2C_MASK;
            ev.rtc_ok = pmu_raw_rtc_seconds(&ev.rtc);
            rtc_read = ev.rtc_ok;
            if (ev.src & SRC_TIMER)
            {
                ev.batt_ok = pmu_raw_battery_mv(&ev.mv);
                if (ev.batt_ok)
                    mv = ev.mv;
            }
        }

        /* a packet: on its interrupt, or one that arrived during the raw
         * reads (Rev 4, N7). The handler's own steps [rockbox-derived] */
        if (!muted && ((hi & I2C_MASK) || wheel_ready()))
        {
            CPU_HI_INT_DIS = I2C_MASK;
            udelay(50);
            ev.src |= SRC_WHEEL;
            ev.pkt_ready = (WHEEL_STAT & WHEEL_READY) != 0;
            if (ev.pkt_ready)
                ev.pkt = WHEEL_DATA;
            WHEEL_STAT |= 0x0C000000;
            WHEEL_CTL = 0x400A1F00;
        }
        if (ev.src == 0)
            ev.src = SRC_GPIO;      /* an unknown wake: counted as bad */

        why = sleep_classify(&st, &ev);

        if (st.act & SACT_RESYNC)
            wheel_resync();
        if ((st.act & SACT_OPTO_OFF) && !opto_kept)
            DEV_EN &= ~DEV_OPTO;
        if ((st.act & SACT_OPTO_INIT) && !(DEV_EN & DEV_OPTO))
            clickwheel_sleep_opto_init();   /* Hold off: buttons live now */
        if (st.act & SACT_WHEEL_MUTE)
            muted = true;           /* a resting finger: until the poll */

        if (ev.src & SRC_TIMER)
        {
            if (lab && lab->n < SLEEP_LAB_POLLS)
            {
                struct sleep_lab_poll *p = &lab->poll[lab->n++];
                p->asked_s = (uint16_t)poll_s;
                p->rtc_s = (uint16_t)(ev.rtc_ok ? ev.rtc - arm_rtc : 0xFFFF);
                p->us = USEC_TIMER - arm_us;
            }
            if (rtc_read)
                arm_rtc = ev.rtc;
            poll_s = sleep_poll_deadline(&crq, st.slept_s, arm_rtc);
            poll_arm(poll_s, waitcnt);
            arm_us = USEC_TIMER;
        }
        else if (waitcnt)
        {
            /* the countdown restarts at every sleep: program what is
             * left, so a storm of touches cannot postpone the poll */
            uint32_t el = (USEC_TIMER - arm_us) / 1000000u;
            poll_s = poll_s > el ? poll_s - el : 1;
            arm_us = USEC_TIMER;
        }

        if ((DEV_EN & DEV_OPTO) && !muted)
            CPU_HI_INT_EN = I2C_MASK;
        if (!muted && wheel_ready())
            again = true;           /* N7: never sleep on a ready packet */
    }

    /* the exit's own RTC read: slept_s for every wake, not the last poll */
    CPU_HI_INT_DIS = I2C_MASK;
    if (pmu_raw_rtc_seconds(&rtc) && rtc >= st.rtc_entry
        && rtc - st.rtc_entry > st.slept_s)
        st.slept_s = rtc - st.rtc_entry;    /* never below the polls' sum */

    /* 5. restore, in reverse */
    TIMER2_CFG = 0;
    (void)TIMER2_VAL;
    GPIOA_INT_EN = 0;
    GPIO_WRITE_BITWISE(GPIOA_INT_LEV, a_lev & HOLD_BIT, HOLD_BIT);
    GPIOA_INT_CLR = HOLD_BIT;
    GPIOA_INT_EN = gpio_en[0];
    GPIOB_INT_EN = gpio_en[1];
    GPIOC_INT_EN = gpio_en[2];
    GPIOD_INT_EN = gpio_en[3];
    if (hold_on())
        DEV_EN &= ~DEV_OPTO;        /* as the driver keeps it under Hold */
    else if (!(DEV_EN & DEV_OPTO))
        clickwheel_sleep_opto_init();
    CPU_HI_INT_DIS = 0xFFFFFFFF;
    CPU_HI_INT_EN = int_hi;
    TIMER1_CFG = t1cfg;             /* the tick resumes */
    CPU_INT_DIS = 0xFFFFFFFF;
    CPU_INT_EN = int_lo;
    COP_HI_INT_EN = cop_hi;
    COP_INT_EN = cop_lo;
    core_wake(COP);                 /* CPU first, then the COP */
    clickwheel_sleep_exit(st.hold_on ? 0 : st.prev_buttons,
                          rq->buttons_at_entry,
                          why != WAKE_DEEP && why != WAKE_LOWBATT
                          && why != WAKE_BCM_CUT);
    restore_interrupt(irq);
    i2c_unlock();

    out->why = why;
    out->slept_s = st.slept_s;
    out->timer_wakes = st.timer_wakes;
    out->spurious = st.spurious;
    out->resyncs = st.resyncs;
    out->wake_buttons = st.wake_buttons;
    out->hold_edges = st.hold_edges;
    out->last_mv = mv;
}
