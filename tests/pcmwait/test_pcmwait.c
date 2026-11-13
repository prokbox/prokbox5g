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

/* Host test of #312: the PCM driver's waits are bounded with the codec
 * held in standby.
 *
 * Runs the REAL code, extracted by run.sh (no copy kept here):
 *  - firmware/target/arm/pp/pcm-pp.c: the pcm-wait block (the bounded
 *    wait, its recovery and its fault record), dma_tx_stop(), pq_stop()
 *    and sink_stop_pcm();
 *  - firmware/pcm.c: pcm_play_data() (the codec woken before a start).
 * against a model of the registers they touch:
 *  - an I2S FIFO of 16 words that drains one word every word_us only while
 *    the codec clocks the I2S (clocks_on); IIS_TXCLR empties it;
 *  - a gated I2S block (AW-5's DEV_I2S off): its FIFO register reads 0
 *    (no free entry: never empty) and ignores writes;
 *  - two playback channels: one told to stop (START cleared) while busy
 *    stays busy until the FIFO takes its last word; reading a status
 *    acknowledges its completion bit;
 *  - USEC_TIMER: one model microsecond per register access, so a spin
 *    advances time and the bound is reached in a bounded number of looks.
 *
 * Every loop in the code under test is bounded here: past POLL_LIMIT
 * register accesses in one call the run is cut (longjmp) and reported as
 * HANG, the verdict the old unbounded waits give with the clocks off.
 *
 * Output: one line per case and a final "VERDICT: PASS" or
 * "VERDICT: FAIL <case>: <why>" (run.sh requires the verdict line). */
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pp_defs.h"            /* the real IIS_*, DMA_CMD_*, DMA_STATUS_* */
#include "pcm_wait_fault.h"     /* the real struct and PCM_WAIT_* */

#define CPU_PP502x
#define PCM_PP_QUEUE
#define IBSS_ATTR
#define ICODE_ATTR
#define logf(...) do { } while (0)

/* ---- the model ---- */
#define POLL_LIMIT 5000000ul    /* 5 s of model time in one call: a hang */
static unsigned long sim_us, polls;
static jmp_buf hang_jb;
static int clocks_on, i2s_gated;
static unsigned long word_us, next_drain;
static int fifo_n;
static unsigned long fifo_reg, junk;
static struct { unsigned long cmd, status, ram; int busy, intr; } ch[2];

static void model_step(void)
{
    sim_us++;
    if (++polls > POLL_LIMIT)
        longjmp(hang_jb, 1);
    /* a write of IIS_TXCLR into the FIFO register (seen at the next
     * access; the block ignores it while gated) */
    if (fifo_reg & IIS_TXCLR)
    {
        if (!i2s_gated)
            fifo_n = 0;
        fifo_reg &= ~(unsigned long)IIS_TXCLR;
    }
    if (clocks_on && !i2s_gated && fifo_n > 0 && sim_us >= next_drain)
    {
        fifo_n--;
        next_drain = sim_us + word_us;
    }
    for (int c = 0; c < 2; c++)
    {
        /* stopped while busy: its word in flight lands once there is room */
        if (ch[c].busy && !(ch[c].cmd & DMA_CMD_START) && !i2s_gated &&
            fifo_n < 16)
        {
            fifo_n++;
            ch[c].busy = 0;
        }
    }
}

static unsigned long model_usec(void)
{
    model_step();
    return sim_us;
}
#define USEC_TIMER model_usec()

static volatile unsigned long *model_fifo(void)
{
    model_step();
    fifo_reg = (fifo_reg & IIS_TXCLR) |
               (i2s_gated ? 0 : (unsigned long)(16 - fifo_n) << 16);
    return &fifo_reg;
}
#define IISFIFO_CFG (*model_fifo())

static volatile unsigned long *model_dma(int c, int off)
{
    model_step();
    switch (off)
    {
    case 0x00:
        return &ch[c].cmd;
    case 0x04:
        ch[c].status = ((unsigned long)ch[c].busy << 31) |
                       ((unsigned long)ch[c].intr << 30) |
                       (ch[c].cmd & 0xfffc);
        ch[c].intr = 0;
        return &ch[c].status;
    case 0x10:
        return &ch[c].ram;
    }
    return &junk;
}
#define PCM_DMA_STATUS(c) (*model_dma((c), 0x04))
#define PQ_CMD(c)         (*model_dma((c), 0x00))
#define PQ_STATUS(c)      (*model_dma((c), 0x04))
#define DMA0_CMD          (*model_dma(0, 0x00))
#define DMA0_STATUS       (*model_dma(0, 0x04))

/* ---- what the extracted code expects ---- */
struct dma_data
{
    union { unsigned long addr; const void *p_r; };
    size_t size;
    int locked;
    int state;
};
static struct dma_data dma_play_data;
static uint8_t pq_on, pq_act, pq_n;

/* pcm.c's side */
typedef bool (*pcm_play_callback_type)(const void **addr, size_t *size);
typedef int (*pcm_status_callback_type)(int status);
static pcm_play_callback_type pcm_callback_for_more;
static pcm_status_callback_type pcm_play_status_callback;
static bool pcm_playing;
#define ALIGN_AUDIOBUF(start, size) do { } while (0)
#define HAVE_IPOD_UI
#define HAVE_DAC_STANDBY

static char events[256];
static int lock_depth, wake_under_lock, dac_standby_flag, dac_wakes;
static void ev(char c)
{
    size_t n = strlen(events);
    if (n + 1 < sizeof(events))
    {
        events[n] = c;
        events[n + 1] = '\0';
    }
}
static void pcm_play_lock(void) { lock_depth++; ev('L'); }
static void pcm_play_unlock(void) { lock_depth--; ev('U'); }
static void pcm_apply_settings(void) { ev('S'); }
static void pcm_play_dma_start_int(const void *a, size_t s)
{
    (void)a; (void)s;
    ev(dac_standby_flag ? 'p' : 'P');   /* p: started with the codec asleep */
}
static void pcm_play_stop_int(void) { ev('X'); }
static bool pcm_get_more_int(const void **a, size_t *s)
{
    (void)a; (void)s;
    return false;
}
static void pcm_report_waits(void) { ev('R'); }
static void dac_busy_now(void) { ev('b'); }
static bool dac_in_standby(void) { return dac_standby_flag; }
static void dac_wake(void)
{
    ev('W');
    dac_wakes++;
    if (lock_depth)
        wake_under_lock++;
    dac_standby_flag = 0;
}

#include "pcmwait_parts.c"      /* the real code (run.sh) */

/* ---- the cases ---- */
static int failed;
static char fail_line[256];

static void fail(const char *name, const char *fmt, ...)
{
    va_list ap;
    char why[200];

    va_start(ap, fmt);
    vsnprintf(why, sizeof(why), fmt, ap);
    va_end(ap);
    printf("  FAIL %s: %s\n", name, why);
    if (!failed)
        snprintf(fail_line, sizeof(fail_line), "%s: %s", name, why);
    failed = 1;
}

static unsigned long faults(int *recovered, int *what)
{
#ifdef PCM_PP_WAIT_US
    struct pcm_wait_fault f;

    pcm_builtin_wait_fault(&f);
    *recovered = f.recovered;
    *what = f.what;
    return f.count;
#else
    *recovered = 0;
    *what = -1;
    return 0;           /* the old code keeps no record */
#endif
}

/* playing: the FIFO full, channel 0 (and 1 with the queue) busy */
static void setup(int clocks, int gated, unsigned long wus, int queue)
{
    memset(ch, 0, sizeof(ch));
    clocks_on = clocks;
    i2s_gated = gated;
    word_us = wus;
    fifo_n = 16;
    fifo_reg = 0;
    next_drain = sim_us + wus;
    pq_on = queue;
    pq_act = 0;
    pq_n = queue ? 2 : 1;
    for (int c = 0; c < (queue ? 2 : 1); c++)
    {
        ch[c].cmd = DMA_CMD_START | DMA_CMD_INTR | 0x3fc;
        ch[c].busy = 1;
    }
    dma_play_data.addr = 0x1000;
    dma_play_data.size = 0x2000;
    dma_play_data.state = 1;
}

/* sink_stop_pcm() in this setup: returns the model microseconds it took,
 * or -1 on a hang */
static long run_stop(void)
{
    unsigned long t0 = sim_us;

    polls = 0;
    if (setjmp(hang_jb))
        return -1;
    sink_stop_pcm();
    return (long)(sim_us - t0);
}

/* expect_faults: how many bounds this stop must reach; max_us: the
 * literal ceiling on its time (the bound is 8000 us: 16 words at 8 kHz,
 * times four; two tries per wait) */
static void stop_case(const char *name, int clocks, int gated,
                      unsigned long wus, int queue,
                      unsigned long expect_faults, int expect_recovered,
                      long max_us)
{
    int rec = 0, what = 0;
    unsigned long f0 = faults(&rec, &what), f1;
    long us;

    setup(clocks, gated, wus, queue);
    us = run_stop();
    f1 = faults(&rec, &what);
    printf("%-44s %s, %ld us, %lu bound(s) reached%s\n", name,
           us < 0 ? "HANG" : "returned", us, f1 - f0,
           f1 != f0 ? (rec ? ", recovered" : ", gave up") : "");
    if (us < 0)
    {
        fail(name, "HANG (no return in %lu register accesses)", POLL_LIMIT);
        return;
    }
    if (us > max_us)
        fail(name, "took %ld us, ceiling %ld", us, max_us);
    if (f1 - f0 != expect_faults)
        fail(name, "%lu bound(s) recorded, expected %lu", f1 - f0,
             expect_faults);
    if (expect_faults && f1 != f0 && rec != expect_recovered)
        fail(name, "recovered=%d, expected %d", rec, expect_recovered);
    if (dma_play_data.state != 0)
        fail(name, "the PCM's state not stopped (%d)", dma_play_data.state);
    if (queue && pq_n != 0)
        fail(name, "the queue's armed count not cleared (%u)", pq_n);
}

static int dummy_more(const void **a, size_t *s)
{
    (void)a; (void)s;
    return 0;
}

static void start_case(const char *name, int asleep, int locked,
                       const char *want)
{
    static const char buf[64];

    events[0] = '\0';
    dac_standby_flag = asleep;
    dac_wakes = wake_under_lock = 0;
    lock_depth = locked;
    pcm_play_data((pcm_play_callback_type)dummy_more, NULL, buf,
                  sizeof(buf));
    printf("%-44s events %s\n", name, events);
    if (strchr(events, 'p'))
        fail(name, "the PCM started with the codec in standby (%s)", events);
    if (wake_under_lock)
        fail(name, "dac_wake() ran under the PCM lock (%s)", events);
    if (want && !strstr(events, want))
        fail(name, "events %s, expected %s in them", events, want);
}

int main(void)
{
    /* clocks on: no bound may fire, at the slowest rate too */
    stop_case("one channel, 44.1 kHz, clocks on", 1, 0, 23, 0, 0, 0, 2000);
    stop_case("one channel, 8 kHz (slowest), clocks on", 1, 0, 125, 0, 0,
              0, 4000);
    stop_case("queue, 8 kHz, clocks on", 1, 0, 125, 1, 0, 0, 4000);
    /* the codec in standby, the I2S block on: the channel's wait and the
     * FIFO's each reach the bound, the FIFO clear recovers both */
    stop_case("one channel, codec in standby", 0, 0, 125, 0, 2, 1, 17000);
    stop_case("queue, codec in standby", 0, 0, 125, 1, 2, 1, 17000);
    /* AW-5's gate closed too: nothing moves, both give up, bounded */
    stop_case("one channel, standby + I2S gated", 0, 1, 125, 0, 2, 0,
              33000);
    stop_case("queue, standby + I2S gated", 0, 1, 125, 1, 2, 0, 33000);
    /* and the next stop after a gated one still returns */
    stop_case("again, standby + I2S gated", 0, 1, 125, 0, 2, 0, 33000);
    /* the start: the codec woken before the lock when asleep (a bench
     * row's direct start); no wake, no mutex, under the mixer's lock */
    start_case("start, codec asleep, no lock held", 1, 0, "bWL");
    start_case("start, codec awake, mixer holds the lock", 0, 1, NULL);
    if (dac_wakes)
        fail("start, codec awake, mixer holds the lock",
             "dac_wake() called with the codec awake");

    if (failed)
        printf("VERDICT: FAIL %s\n", fail_line);
    else
        printf("VERDICT: PASS\n");
    return failed;
}
