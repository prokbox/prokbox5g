/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2006-2007 Robert Keevil
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

#include "thread.h"
#include "system.h"
#include "kernel.h"
#include "usb.h"
#include "logf.h"
#include "piezo.h"
#include "xcore.h"

static long piezo_stack[DEFAULT_STACK_SIZE/sizeof(long)];
static const char piezo_thread_name[] = "piezo";
/* posted by the UI (clicks) on either core (P19 S0: uncached under
 * HAVE_COP_COHERENCE, xcore.h) */
static struct event_queue piezo_queue XCORE_BSS;
/* the click's length: written by the poster (the UI, on either core),
 * read by the piezo thread on the CPU: uncached with the queue
 * (ucop-fix, 2026-10-26), in builds that can put the UI on the COP */
#ifdef HAVE_UI_THREAD_SPLIT
static unsigned int duration XCORE_BSS;
#else
static unsigned int duration;
#endif
static bool beeping;

enum {
    Q_PIEZO_BEEP = 1,
    Q_PIEZO_BEEP_FOR_TICK,
    Q_PIEZO_BEEP_FOR_USEC,
    Q_PIEZO_STOP
};

static inline void piezo_hw_init(void)
{
#ifndef SIMULATOR
    /*logf("PIEZO: hw_init");*/
    DEV_INIT1 &= ~0xc;
    DEV_EN |= DEV_PWM;
#endif
}

static void piezo_hw_tick(unsigned int form_and_period)
{
#ifndef SIMULATOR
    PWM0_CTRL = 0x80000000 | form_and_period;
#endif
}

static inline void piezo_hw_stop(void)
{
#ifndef SIMULATOR
    PWM0_CTRL = 0;
#endif
}

static void piezo_thread(void)
{
    struct queue_event ev;
    long piezo_usec_off;

    while(1)
    {
        queue_wait(&piezo_queue, &ev);
        switch(ev.id)
        {
            case Q_PIEZO_BEEP:
                piezo_hw_tick((unsigned int)ev.data);
                beeping = true;
                break;
            case Q_PIEZO_BEEP_FOR_TICK:
                piezo_hw_tick((unsigned int)ev.data);
                beeping = true;
                sleep(duration);
                if (beeping)
                    piezo_hw_stop();
                beeping = false;
                /* remove anything that appeared while sleeping */
                queue_clear(&piezo_queue);
                break;
            case Q_PIEZO_BEEP_FOR_USEC:
                piezo_usec_off = USEC_TIMER + duration;
                piezo_hw_tick((unsigned int)ev.data);
                beeping = true;
                while (TIME_BEFORE(USEC_TIMER, piezo_usec_off))
                    if (duration >= 5000) yield();
                if (beeping)
                    piezo_hw_stop();
                beeping = false;
                /* remove anything that appeared while sleeping */
                queue_clear(&piezo_queue);
                break;
            case Q_PIEZO_STOP:
                if (beeping)
                    piezo_hw_stop();
                beeping = false;
                break;
#ifndef SIMULATOR
            case SYS_USB_CONNECTED:
                /*logf("USB: Piezo core");*/
                piezo_hw_stop();
                queue_clear(&piezo_queue);
                usb_acknowledge(SYS_USB_CONNECTED_ACK, ev.data);
                usb_wait_for_disconnect(&piezo_queue);
                break ;
#endif
            case SYS_TIMEOUT:
                break;
        }
        yield();
    }
}


void piezo_play(unsigned short inv_freq, unsigned char form)
{
    queue_post(&piezo_queue, Q_PIEZO_BEEP,
        (intptr_t)((unsigned int)form << 16 | inv_freq));
}

void piezo_play_for_tick(unsigned short inv_freq,
                    unsigned char form, unsigned int dur)
{
    duration = dur;
    queue_post(&piezo_queue, Q_PIEZO_BEEP_FOR_TICK,
        (intptr_t)((unsigned int)form << 16 | inv_freq));
}

void piezo_play_for_usec(unsigned short inv_freq,
                    unsigned char form, unsigned int dur)
{
    duration = dur;
    queue_post(&piezo_queue, Q_PIEZO_BEEP_FOR_USEC,
        (intptr_t)((unsigned int)form << 16 | inv_freq));
}

void piezo_stop(void)
{
    queue_post(&piezo_queue, Q_PIEZO_STOP, 0);
}

void piezo_clear(void)
{
    queue_clear(&piezo_queue);
    piezo_stop();
}

bool piezo_busy(void)
{
    return !queue_empty(&piezo_queue);
}

/*  conversion factor based on the following data

        period          Hz
        10              8547
        20              4465
        30              3024
        40              2286
        50              1846
        60              1537
        70              1320
        80              1165
        90              1030
        100             928

    someone with better recording/analysing equipment should be able
    to get more accurate figures
*/
unsigned int piezo_hz(unsigned int hz)
{
    if (hz > 0)
        return 91225/hz;
    else
        return 0;
}

void piezo_init(void)
{
    /*logf("PIEZO: init");*/
    piezo_hw_init();
    queue_init(&piezo_queue, true);
    create_thread(piezo_thread, piezo_stack, sizeof(piezo_stack), 0,
            piezo_thread_name IF_PRIO(, PRIORITY_REALTIME)
            IF_COP(, CPU));
}

/* Click pulse (Phase 9, #23): the haptic clicker mod gets its own
 * constants, equal to the piezo's until a device check sets them
 * (P10 H8, V4). */
#ifdef HAVE_IPOD_HAPTICS
#define HAPTIC_TICK_INV_FREQ  91
#define HAPTIC_TICK_FORM      0x80
#define HAPTIC_TICK_USEC      4000
#else
#define HAPTIC_TICK_INV_FREQ  91
#define HAPTIC_TICK_FORM      0x80
#define HAPTIC_TICK_USEC      4000
#endif

void piezo_button_beep(bool beep, bool force)
{
    /* old on clickwheel action - piezo_play_for_usec(50, 0x80, 400);
       old on button action - piezo_play_for_usec(50, 0x80, 3000); */

    if (force)
        piezo_clear();

    if (queue_empty(&piezo_queue))
    {
        if (beep)
            piezo_play_for_tick(40, 0x80, HZ/5);
        else
            piezo_play_for_usec(HAPTIC_TICK_INV_FREQ, HAPTIC_TICK_FORM,
                                HAPTIC_TICK_USEC);
    }
}
