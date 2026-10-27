/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 by Alan Korr
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
#include "kernel.h"
#include "thread.h"
#include "string.h"
#include "file.h"
#ifdef HAVE_PP_GOV
#include "pgov.h"         /* ProkBox: the band governor */
#endif

#ifndef CPU_FREQ
#define CPU_FREQ (-1)
#endif

long cpu_frequency SHAREDBSS_ATTR = CPU_FREQ;

#ifdef HAVE_ADJUSTABLE_CPU_FREQ
static int boost_counter SHAREDBSS_ATTR = 0;
static bool cpu_idle SHAREDBSS_ATTR = false;

int get_cpu_boost_counter(void)
{
    return boost_counter;
}

#ifdef HAVE_PLL_IDLE
/* Fit-1 C6: who needs the PLL while unboosted (CPU_PLL_HOLD_*); the
 * policy is off until the shell clears CPU_PLL_HOLD_DEV, and the sampled
 * holders count as holding until pll_idle_poll() first samples them */
static unsigned int pll_holds SHAREDDATA_ATTR =
    CPU_PLL_HOLD_DEV | CPU_PLL_HOLD_LIT | CPU_PLL_HOLD_LOAD;
static unsigned long pll_idle_entries SHAREDBSS_ATTR;
static unsigned long pll_idle_ticks SHAREDBSS_ATTR;
static unsigned long pll_boosts_from_idle SHAREDBSS_ATTR;
static long pll_idle_since SHAREDBSS_ATTR;

#ifdef HAVE_PP_GOV
#define gov_on() (pgov_active() && !cpu_idle)
#endif

/* The unboosted setting; called with the boost lock held. Upstream's
 * cpu_idle_mode() still forces 24 MHz, but never over ATA DMA or USB. */
static long unboosted_freq(void)
{
    if (pll_holds == 0
        || (cpu_idle && !(pll_holds & (CPU_PLL_HOLD_ATA|CPU_PLL_HOLD_USB))))
        return CPUFREQ_DEFAULT;
    return CPUFREQ_NORMAL;
}

/* set_cpu_frequency() with the time spent at 24 MHz counted */
static void set_freq_tracked(long f)
{
#ifdef HAVE_PP_GOV_DVS
    /* the band governor's stage 2: never a clock the core rail may not
     * carry (low or still settling), never 100 MHz under its caps; a
     * no-op unless its apply mode is on.  Every clock change of this
     * file passes here, with the boost lock held */
    f = pgov_clamp_hz(f);
#endif
    if (cpu_frequency == CPUFREQ_DEFAULT && f != CPUFREQ_DEFAULT)
    {
        pll_idle_ticks += current_tick - pll_idle_since;
        if (f == CPUFREQ_MAX)
            pll_boosts_from_idle++;
    }
    else if (cpu_frequency != CPUFREQ_DEFAULT && f == CPUFREQ_DEFAULT)
    {
        pll_idle_entries++;
        pll_idle_since = current_tick;
    }
    set_cpu_frequency(f);
}

void cpu_pll_hold(unsigned int reason, bool hold)
{
    unsigned int was;

    if (!cpu_boost_lock())
        return;
    was = pll_holds;
    pll_holds = hold ? (was | reason) : (was & ~reason);
    if (pll_holds != was && boost_counter <= 0)
    {
#ifdef HAVE_PP_GOV
        /* the governor: a holder is a floor, at once when it rises; a
         * fall waits for the governor's dwell */
        long f = gov_on() ? pgov_floor_hz(pll_holds) : unboosted_freq();
#else
        long f = unboosted_freq();
#endif
        if (f != cpu_frequency)
            set_freq_tracked(f);
    }
    cpu_boost_unlock();
}

#ifdef HAVE_PP_GOV
/* The power thread: the governor's decision (never below the boost) */
void cpu_pgov_apply(void)
{
    long f;

    if (!cpu_boost_lock())
        return;
    if (gov_on())
    {
        f = pgov_decision_hz();
        if (!(boost_counter > 0 && f < CPUFREQ_MAX) && f != cpu_frequency)
            set_freq_tracked(f);
    }
    cpu_boost_unlock();
}

#ifdef HAVE_PP_GOV_DVS
/* Stage 2's apply layer (power thread): one clock step, never below the
 * boost's 80 MHz while boosted (the rail's clamp may still hold it at
 * band 1's top until a raise settles) */
void cpu_pgov_set(long f)
{
    if (!cpu_boost_lock())
        return;
    if (gov_on() && !(boost_counter > 0 && f < CPUFREQ_MAX)
        && f != cpu_frequency)
        set_freq_tracked(f);
    cpu_boost_unlock();
}

/* Stage 2: whether the rail may go down now (the clock in band 0 or 1,
 * nothing boosting); recorded by the governor before its PMU write, so a
 * boost after this point is clamped to band 1's top */
bool cpu_pgov_lower(void)
{
    bool ok;

    if (!cpu_boost_lock())
        return false;
    ok = gov_on() && pgov_lower_locked(cpu_frequency, boost_counter);
    cpu_boost_unlock();
    return ok;
}

/* Stage 2 (review S3): a boost held at band 1's top while the rail rose
 * (the governor bypassed by cpu_idle_mode(), or the boost inside the
 * settle) gets its clock once the rail carries it */
void cpu_pgov_reboost(void)
{
    if (!cpu_boost_lock())
        return;
    if (boost_counter > 0 && cpu_frequency < CPUFREQ_MAX)
        set_freq_tracked(CPUFREQ_MAX);      /* clamped, as ever */
    cpu_boost_unlock();
}

/* Stage 2: a cap rose outside the governor (the BCM2722's port waking):
 * the clock re-clamped now */
void cpu_pgov_recheck(void)
{
    long f;

    if (!cpu_boost_lock())
        return;
    f = pgov_clamp_hz(cpu_frequency);
    if (f != cpu_frequency)
        set_freq_tracked(f);
    cpu_boost_unlock();
}
#endif /* HAVE_PP_GOV_DVS */

/* The governor switched off: today's unboosted clock */
void cpu_pgov_resync(void)
{
    if (!cpu_boost_lock())
        return;
    if (boost_counter <= 0)
    {
        long f = unboosted_freq();
        if (f != cpu_frequency)
            set_freq_tracked(f);
    }
    cpu_boost_unlock();
}
#endif /* HAVE_PP_GOV */

unsigned int cpu_pll_holds(void)
{
    return pll_holds;
}

void cpu_pll_get_stats(struct cpu_pll_stats *st)
{
    cpu_boost_lock();
    st->idle_entries = pll_idle_entries;
    st->idle_ticks = pll_idle_ticks;
    if (cpu_frequency == CPUFREQ_DEFAULT)
        st->idle_ticks += current_tick - pll_idle_since;
    st->boosts_from_idle = pll_boosts_from_idle;
    st->holds = pll_holds;
    cpu_boost_unlock();
    pll_lock_get_stats(&st->lock_count, &st->lock_us_last, &st->lock_us_max);
}
#ifdef HAVE_PP_GOV
/* The band governor (pgov.c), while it is on and upstream's
 * cpu_idle_mode() is not: a boost selects its point now (80 MHz); a
 * release leaves the clock to the governor's dwell */
#define set_unboosted() do {                            \
        if (!gov_on())                                  \
            set_freq_tracked(unboosted_freq());         \
    } while (0)
#define set_boosted() do {                              \
        if (!gov_on())                                  \
            set_freq_tracked(CPUFREQ_MAX);              \
        else {  /* still at 80 inside the dwell: no relock */ \
            long f_ = pgov_boost_hz();                  \
            if (f_ != cpu_frequency)                    \
                set_freq_tracked(f_);                   \
        }                                               \
    } while (0)
#else
#define set_unboosted() set_freq_tracked(unboosted_freq())
#define set_boosted()   set_freq_tracked(CPUFREQ_MAX)
#endif
#else /* upstream's code, so a stock build compiles to the same bytes */
#define set_unboosted() do {                    \
        if(cpu_idle)                            \
            set_cpu_frequency(CPUFREQ_DEFAULT); \
        else                                    \
            set_cpu_frequency(CPUFREQ_NORMAL);  \
    } while (0)
#define set_boosted()   set_cpu_frequency(CPUFREQ_MAX)
#endif /* HAVE_PLL_IDLE */

#ifdef CPU_BOOST_LOGGING
#define MAX_BOOST_LOG 64
static char cpu_boost_calls[MAX_BOOST_LOG][MAX_PATH];
static int cpu_boost_first = 0;
static int cpu_boost_calls_count = 0;
static int cpu_boost_track_message = 0;
int cpu_boost_log_getcount(void)
{
    return cpu_boost_calls_count;
}
char * cpu_boost_log_getlog_first(void)
{
    char *first;
    cpu_boost_lock();

    first = NULL;

    if (cpu_boost_calls_count)
    {
        cpu_boost_track_message = 1;
        first = cpu_boost_calls[cpu_boost_first];
    }

    cpu_boost_unlock();
    return first;
}

char * cpu_boost_log_getlog_next(void)
{
    int message;
    char *next;

    cpu_boost_lock();

    message = (cpu_boost_track_message+cpu_boost_first)%MAX_BOOST_LOG;
    next = NULL;

    if (cpu_boost_track_message < cpu_boost_calls_count)
    {
        cpu_boost_track_message++;
        next = cpu_boost_calls[message];
    }

    cpu_boost_unlock();
    return next;
}

void cpu_boost_(bool on_off, char* location, int line)
{
    int item = cpu_boost_calls_count;
    if (!cpu_boost_lock())
        return;

    if (cpu_boost_calls_count == MAX_BOOST_LOG)
    {
        cpu_boost_first = (cpu_boost_first+1)%MAX_BOOST_LOG;
        cpu_boost_calls_count--;
        if (cpu_boost_calls_count < 0)
            cpu_boost_calls_count = 0;
        item += cpu_boost_first;
    }
    if (cpu_boost_calls_count < MAX_BOOST_LOG)
    {
        int message = (cpu_boost_first+cpu_boost_calls_count)%MAX_BOOST_LOG;
        snprintf(cpu_boost_calls[message], MAX_PATH,"%d.) %c %d %s:%d",
                 item,on_off?'B':'U',boost_counter,location,line);
        cpu_boost_calls_count++;
    }
#else
void cpu_boost(bool on_off)
{
    if (!cpu_boost_lock())
        return;

#endif /* CPU_BOOST_LOGGING */
    if(on_off)
    {
        /* Boost the frequency if not already boosted */
        if(++boost_counter == 1)
            set_boosted();
    }
    else
    {
        /* Lower the frequency if the counter reaches 0 */
        if(--boost_counter <= 0)
        {
            set_unboosted();

            /* Safety measure */
            if (boost_counter < 0)
            {
                boost_counter = 0;
            }
        }
    }

    cpu_boost_unlock();
}

void cpu_idle_mode(bool on_off)
{
    if (!cpu_boost_lock())
        return;

    cpu_idle = on_off;

    /* We need to adjust the frequency immediately if the CPU
       isn't boosted */
    if(boost_counter == 0)
        set_unboosted();

    cpu_boost_unlock();
}
#endif /* HAVE_ADJUSTABLE_CPU_FREQ */
