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
 * ProkBox (Fit-1 M0, docs/fit1-brief.md section A): the measurement kit.
 * Developer builds only (HAVE_DEVSTATS, set with HAVE_IPOD_UI_DEVMODE in
 * ipodvideo-variant.h); stock, release and bootloader builds compile none
 * of it, and every hook below is then an empty macro.
 *
 *  - CPU time: switch_thread() charges the microseconds since the last
 *    switch to the thread leaving the core, and the time spent in
 *    core_sleep() to that core's idle (target only: the simulator's SDL
 *    threads have no switch hook). Interrupts are charged to whatever ran
 *    under them.
 *  - Storage: every ATA command (or, in the simulator, every file read or
 *    write) is counted against the calling thread's slot: commands,
 *    sectors, PIO fallbacks. A command that finds the disk asleep is a
 *    wake, kept with its thread in a 16-entry ring for the shell to log.
 *    Failed standby attempts are counted (a disk that can't sleep shows as
 *    disk_pct 100).
 *  - Latency: a play, skip, seek or resume request stamps USEC_TIMER (the
 *    tick in the simulator); the next PCM start (pcm_play_data() or an
 *    unpause) closes it. The shell logs the pair once.
 *
 * Everything is RAM: counters only, no I/O, no lock beyond an IRQ mask
 * around a few words. apps/ipodui/m0log.c and powerlog.c read it and write
 * it out on the existing storage-idle flush.
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
#ifndef _DEVSTATS_H_
#define _DEVSTATS_H_

#include "config.h"

#ifdef HAVE_DEVSTATS
#include <stdbool.h>
#include <stdint.h>
#include "thread.h"

#define DEVSTATS_SLOTS      MAXTHREADS
#define DEVSTATS_NOSLOT     0xff
#define DEVSTATS_WAKES      16      /* the wake ring */

/* Per thread slot. A slot is reused when its thread exits; the readers
 * name it from the slot's current thread. */
struct devstats_thread {
    uint32_t cpu_us;        /* time on a core (target only) */
    uint32_t cmds;          /* storage commands (reads + writes) */
    uint32_t sectors;       /* 512 B sectors moved */
    uint16_t wakes;         /* commands that found the disk asleep */
    uint16_t pio;           /* commands that fell back to PIO */
    uint32_t thr_wakes;     /* P18 18-A (M1): times the thread was made
                             * runnable (a wait ended: event, timeout) */
    uint32_t slice_max_us;  /* npfix: the longest single run on a core
                             * since the reader last cleared it (target) */
};

struct devstats_wake {
    long tick;              /* current_tick at the command */
    uint32_t sector;        /* first sector (truncated to 32 bits) */
    uint16_t count;         /* sectors (saturated) */
    uint8_t slot;           /* the thread's slot */
    uint8_t write;          /* 1 write, 0 read */
};

/* Fit-3 (docs/fit3-brief.md, step 0): what the data-path map's
 * opportunities would save, measured before any of them is built.  Every
 * field only grows (32-bit words, one writer each); readers take deltas
 * and never clear.  pcm_wb_* are written only by the playback FIQ. */
struct devstats_fit3 {
    uint32_t lcd_bytes;     /* UI pixels pushed over the host port */
    uint32_t lcd_updates;   /* UI updates (lcd_update, _rect) */
    uint32_t lcd_us;        /* their total time (µs, target only) */
    uint32_t lcd_max_us;    /* the longest single UI update */
    uint32_t ovl_bytes;     /* video overlay pixels over the port */
    uint32_t ovl_updates;   /* overlay sends */
    uint32_t rd_tick;       /* port reads from lcd_tick (interrupt) */
    uint32_t rd_poll;       /* port reads from the video poll */
    uint32_t art_builds;    /* art images decoded */
    uint32_t art_bytes;     /* their files' bytes */
    uint32_t art_reads;     /* read calls made for them */
    uint32_t art_read_us;   /* time in those reads */
    uint32_t art_dec_us;    /* decode and scale time */
    uint32_t art_boosts;    /* boosts taken during them */
    uint32_t pcm_wb;        /* PCM buffers written back from the FIQ */
    uint32_t pcm_wb_us;     /* time in those write-backs */
    /* HAVE_LCD_E2: of the UI pushes above, what went by the PP's second
     * DMA engine and the time it ran while the CPU was free (yielded);
     * pieces stopped or refused (the CPU then copied) */
    uint32_t lcd_e2_bytes;
    uint32_t lcd_e2_us;
    uint32_t lcd_e2_fails;
};

struct devstats {
    struct devstats_thread t[DEVSTATS_SLOTS];
    uint32_t idle_us[NUM_CORES];    /* core_sleep() time (target only) */
    uint32_t wake_head;             /* wakes ever recorded */
    struct devstats_wake wake[DEVSTATS_WAKES];
    uint16_t sleeps;                /* standby entered */
    uint16_t sleep_fails;           /* flush or standby failed */
    uint16_t resets;                /* soft resets after a failed command */
    uint32_t irq_waits;             /* C4b: DMA ends that woke by interrupt */
    uint8_t irq_off;                /* C4b: 1 once polling took over */
    /* latency: request stamp, then the PCM start that closed it */
    uint32_t req_us, pcm_us;        /* USEC_TIMER (sim: tick * 10000) */
    uint16_t req_seq, pcm_seq;      /* pcm_seq == req_seq: closed */
    uint8_t req_why;                /* DEVSTATS_REQ_* */
    struct devstats_fit3 f3;        /* Fit-3 step 0 */
    /* P19 19-1a (19-1f): the four back-end pollers' waits, and how many
     * of them ended at their deadline (the rest ended on an event) */
    uint32_t poll_waits[4];
    uint32_t poll_tmo[4];
};

enum {
    DEVSTATS_POLL_AUDIO = 0,
    DEVSTATS_POLL_STORAGE,
    DEVSTATS_POLL_TAGCACHE,
    DEVSTATS_POLL_POWER,
};

enum {
    DEVSTATS_REQ_PLAY = 1,  /* audio_play() */
    DEVSTATS_REQ_SKIP,      /* audio_skip(), next/prev, dir skips */
    DEVSTATS_REQ_SEEK,      /* audio_ff_rewind() */
    DEVSTATS_REQ_RESUME,    /* audio_resume() */
};

extern struct devstats devstats;

/* storage: one command by the current thread */
void devstats_io(bool write, unsigned long sector, unsigned long count,
                 bool wake, bool pio);
void devstats_disk_sleep(bool ok);
void devstats_disk_reset(void);
void devstats_ata_irq(bool woke);
/* latency */
void devstats_request(int why);
void devstats_pcm_start(void);
/* A consistent copy (IRQs masked around the struct copy) */
void devstats_snapshot(struct devstats *out);
/* "name" of a slot's current thread into buf ("-" if free), no yield */
void devstats_slot_name(unsigned int slot, char *buf, int size);

#if !defined(SIMULATOR) && (CONFIG_PLATFORM & PLATFORM_NATIVE)
#include "system.h"
/* switch_thread() hooks: the core's last switch time */
extern uint32_t devstats_last_us[NUM_CORES];
static inline void devstats_switch_out(unsigned int core, unsigned int slot)
{
    uint32_t now = USEC_TIMER;
    uint32_t d = now - devstats_last_us[core];
    devstats.t[slot].cpu_us += d;
    if (d > devstats.t[slot].slice_max_us)
        devstats.t[slot].slice_max_us = d;  /* npfix: a cooperative hog */
    devstats_last_us[core] = now;
}
static inline void devstats_idle_end(unsigned int core)
{
    uint32_t now = USEC_TIMER;
    devstats.idle_us[core] += now - devstats_last_us[core];
    devstats_last_us[core] = now;
}
#define DEVSTATS_SWITCH_OUT(core, slot) devstats_switch_out(core, slot)
#define DEVSTATS_IDLE_END(core)         devstats_idle_end(core)
#endif
/* P18 18-A (M1): a plain add, called with IRQs masked (target: the
 * kernel's run-queue insert; simulator: the end of a wait) */
#define DEVSTATS_THREAD_WAKE(slot) \
    do { if ((slot) < DEVSTATS_SLOTS) devstats.t[slot].thr_wakes++; } while (0)

/* Fit-3 step 0: plain adds, no calls (DEVSTATS_PCM_WB runs in the FIQ) */
#define DEVSTATS_LCD(bytes, us) do { \
        devstats.f3.lcd_bytes += (bytes); devstats.f3.lcd_updates++; \
        devstats.f3.lcd_us += (us); \
        if ((uint32_t)(us) > devstats.f3.lcd_max_us) \
            devstats.f3.lcd_max_us = (us); } while (0)
#define DEVSTATS_LCD_E2(bytes, us) do { \
        devstats.f3.lcd_e2_bytes += (bytes); \
        devstats.f3.lcd_e2_us += (us); } while (0)
#define DEVSTATS_LCD_E2_FAIL()  (devstats.f3.lcd_e2_fails++)
#define DEVSTATS_OVL(bytes) do { \
        devstats.f3.ovl_bytes += (bytes); devstats.f3.ovl_updates++; } while (0)
#define DEVSTATS_RD_TICK()      (devstats.f3.rd_tick++)
#define DEVSTATS_RD_POLL()      (devstats.f3.rd_poll++)
#define DEVSTATS_ART(bytes, reads, read_us, dec_us, boosts) do { \
        devstats.f3.art_builds++; devstats.f3.art_bytes += (bytes); \
        devstats.f3.art_reads += (reads); \
        devstats.f3.art_read_us += (read_us); \
        devstats.f3.art_dec_us += (dec_us); \
        devstats.f3.art_boosts += (boosts); } while (0)
#define DEVSTATS_PCM_WB(us) do { \
        devstats.f3.pcm_wb++; devstats.f3.pcm_wb_us += (us); } while (0)
#define DEVSTATS_IO(w, s, c, wake, pio) devstats_io(w, s, c, wake, pio)
#define DEVSTATS_DISK_SLEEP(ok)         devstats_disk_sleep(ok)
#define DEVSTATS_DISK_RESET()           devstats_disk_reset()
#define DEVSTATS_ATA_IRQ(woke)          devstats_ata_irq(woke)
#define DEVSTATS_REQUEST(why)           devstats_request(why)
#define DEVSTATS_PCM_START()            devstats_pcm_start()
#define DEVSTATS_POLL(which, tmo) do { devstats.poll_waits[which]++; \
        if (tmo) devstats.poll_tmo[which]++; } while (0)
#endif /* HAVE_DEVSTATS */

#ifndef DEVSTATS_POLL
#define DEVSTATS_POLL(which, tmo)       do { } while (0)
#endif

#ifndef DEVSTATS_THREAD_WAKE
#define DEVSTATS_THREAD_WAKE(slot)      do { } while (0)
#endif
#ifndef DEVSTATS_SWITCH_OUT
#define DEVSTATS_SWITCH_OUT(core, slot) do { } while (0)
#define DEVSTATS_IDLE_END(core)         do { } while (0)
#endif
#ifndef DEVSTATS_IO
#define DEVSTATS_IO(w, s, c, wake, pio) do { } while (0)
#define DEVSTATS_DISK_SLEEP(ok)         do { } while (0)
#define DEVSTATS_DISK_RESET()           do { } while (0)
#define DEVSTATS_ATA_IRQ(woke)          do { } while (0)
#define DEVSTATS_REQUEST(why)           do { } while (0)
#define DEVSTATS_PCM_START()            do { } while (0)
#endif
#ifndef DEVSTATS_LCD
#define DEVSTATS_LCD(bytes, us)         do { } while (0)
#define DEVSTATS_LCD_E2(bytes, us)      do { } while (0)
#define DEVSTATS_LCD_E2_FAIL()          do { } while (0)
#define DEVSTATS_OVL(bytes)             do { } while (0)
#define DEVSTATS_RD_TICK()              do { } while (0)
#define DEVSTATS_RD_POLL()              do { } while (0)
#define DEVSTATS_ART(b, r, rus, dus, bo) do { } while (0)
#define DEVSTATS_PCM_WB(us)             do { } while (0)
#endif

#endif /* _DEVSTATS_H_ */
