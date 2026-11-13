/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2006 by Michael Sevakis
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
#include <stdlib.h>
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "logf.h"
#include "audio.h"
#include "sound.h"
#include "pcm.h"
#include "pcm_sampr.h"
#include "pcm-internal.h"
#include "pcm_sink.h"
#include "devstats.h"   /* Fit-3 step 0: empty unless HAVE_DEVSTATS */

/** DMA **/

struct dma_data
{
/* NOTE: The order of size and p is important if you use assembler
   optimised fiq handler, so don't change it. */
    union
    {
        unsigned long addr;
        const void *p_r;
        void *p_w;
        uint32_t *p16;  /* For packed 16-16 stereo pairs */
        uint16_t *p32;  /* For individual samples converted to 32-bit */
    };
    size_t size;
#if NUM_CORES > 1
    unsigned core;
#endif
    int locked;
    int state;
};

extern void *fiq_function;

#ifdef HAVE_CORE_LAYOUT_A
/* ProkBox core layout (A) steps 0b and 2 (firmware/export/corelayout.h,
 * docs/changes-from-rockbox.md): either core may take the PCM DMA's FIQ.
 * The vector's one shared target (fiq_function, both cores jump through
 * it) points at fiq_by_core, which reads the core's identity and jumps
 * through that core's own target.  Apple's FIQ stub does the same with
 * PROCESSOR_ID in two banked registers (osos 1.3 0x1000_015C).  The
 * targets live in IRAM (uncached, both cores read them); each is written
 * only while that core's source 26 is masked. */
#if !defined(CPU_PP502x)
#error "HAVE_CORE_LAYOUT_A: the PP502x FIQ path only"
#endif
#include "corelayout.h"
void *fiq_core_target[NUM_CORES] IBSS_ATTR;
static unsigned int pcm_core IBSS_ATTR;         /* takes source 26: CPU */
static volatile int pcm_fiq_busy IBSS_ATTR;     /* pcm_core's FIQ runs */
/* 0.8.3: FIQs since the last start (the first two are not judged) and the
 * last one's time, for the FIFO and gap stats (IRAM: uncached) */
static uint8_t pcm_fiq_warm IBSS_ATTR;
static uint32_t pcm_fiq_last_us IBSS_ATTR;
/* 0.8.3 fifo96: the one-channel lateness model.  A piece armed at
 * pcm_arm_us with pcm_arm_free FIFO words free takes the FIFO's free
 * words at once and the rest at the I2S rate, so the hardware ends it
 * (words - free) word times later with the FIFO full; the FIQ is late by
 * what passed beyond that.  pcm_tw_q8: one FIFO word's time, us x 256
 * (CPU-written while stopped, at each rate or depth change) */
static uint32_t pcm_arm_us IBSS_ATTR;
static uint16_t pcm_arm_words IBSS_ATTR;
static uint8_t pcm_arm_free IBSS_ATTR;
static uint32_t pcm_tw_q8 IBSS_ATTR;
static uint32_t pcm_lock_us IBSS_ATTR;  /* the PCM lock's first take */
static uint8_t pcm_lock_playing IBSS_ATTR; /* playing when it was taken */
static struct corelock pcm_lock_cl IBSS_ATTR;
static struct pcm_pp_stats pcmpp IBSS_ATTR;

/* The interrupt controller's per-core registers: the COP's sit 0x10
 * above the CPU's (EN_STAT, EN, DIS, PRIORITY; pp5020.h) */
#define CORE_INT_REG(cpu_reg, core) \
    (*(volatile unsigned long *)((uintptr_t)&(cpu_reg) + ((core) << 4)))

/* 0.8.3 fifo96: where each core was when its last FIQ came: the
 * interrupted pc + 4 (lr_fiq) and its cpsr (spsr_fiq), per core.  The
 * FIQ keeps the pair of its latest late run (pcm_pp_stats late_pc): a pc
 * just after an interrupt unmask names the section that held it off */
uint32_t fiq_entry_ctx[NUM_CORES][2] IBSS_ATTR;

void fiq_by_core(void) ICODE_ATTR __attribute__((naked));
void fiq_by_core(void)
{
    /* FIQ mode banks r8-r14: r10 and r12 need no saving.  The flags of
     * the compare live through the stores (none sets them) */
    asm volatile (
        "mov    r10, #0x60000000    \n" /* PROCESSOR_ID */
        "ldrb   r12, [r10]          \n"
        "cmp    r12, #0x55          \n" /* the CPU */
        "ldr    r10, =fiq_entry_ctx \n"
        "addne  r10, r10, #8        \n" /* the COP's pair */
        "str    lr, [r10]           \n"
        "mrs    r12, spsr           \n"
        "str    r12, [r10, #4]      \n"
        "ldr    r10, =fiq_core_target \n"
        "ldreq  pc, [r10]           \n"
        "ldr    pc, [r10, #4]       \n" /* the COP */
        ".ltorg                     \n"
    );
}

/* A FIQ on a core with no target yet: its source 26 off, counted */
static void fiq_spurious(void) ICODE_ATTR __attribute__((interrupt("FIQ")));
static void fiq_spurious(void)
{
    if (CURRENT_CORE == CPU)
    {
        CPU_INT_DIS = DMA_MASK;
        pcmpp.spurious[CPU]++;
    }
    else
    {
        COP_INT_DIS = DMA_MASK;
        pcmpp.spurious[COP]++;
    }
}
#define FIQ_TARGET_SET(core, fn) (fiq_core_target[(core)] = (void *)(fn))
#define IF_LAYOUT_A_CORE pcm_core
#else
#define IF_LAYOUT_A_CORE CPU
#define FIQ_TARGET_SET(core, fn) (fiq_function = (void *)(fn))
#endif /* HAVE_CORE_LAYOUT_A */

/* Dispatch to the proper handler and leave the main vector table alone */
void fiq_handler(void) ICODE_ATTR __attribute__((naked));
void fiq_handler(void)
{
    asm volatile (
        "ldr pc, [pc, #-4]  \n"
    "fiq_function:          \n"
#ifdef HAVE_CORE_LAYOUT_A
        ".word fiq_by_core  \n"
#else
        ".word 0            \n"
#endif
    );
}

#ifdef HAVE_PCM_DMA_ADDRESS
void * pcm_dma_addr(void *addr)
{
    if (addr != NULL && (unsigned long)addr < UNCACHED_BASE_ADDR)
        addr = UNCACHED_ADDR(addr);
    return addr;
}
#endif

/* TODO: Get simultaneous recording and playback to work. Just needs some tweaking */

/****************************************************************************
 ** Playback DMA transfer
 **/
static struct dma_data dma_play_data IBSS_ATTR =
{
    /* Initialize to a locked, stopped state */
    { .addr = 0 },
    .size = 0,
#if NUM_CORES > 1
    .core = 0x00,
#endif
    .locked = 0,
    .state = 0
};

/* DMA status cannot be viewed from outside code in control because that can
 * clear the interrupt from outside the handler and prevent the handler from
 * from being called. Split up transfers to a reasonable size that is good as
 * a timer and peaking yet still keeps the FIQ count low.
 */
/* set by the CPU (sink_set_freq), read by the FIQ: in layout A that is the
   COP's, so the word is uncached (IRAM) */
#ifdef HAVE_CORE_LAYOUT_A
static uint16_t max_dma_chunk_size IBSS_ATTR;
#else
static uint16_t max_dma_chunk_size;
#endif

#ifdef HAVE_PCM_24BIT_OUT
/* ProkBox #268: bytes a frame (4: 16-bit, 8: 24-bit in 32-bit words) and
   the rate index, so a depth change re-sizes the DMA pieces; CPU-written
   while the PCM is stopped, read by the FIQ: IRAM (uncached) */
static uint8_t pcm_pp_bpf IDATA_ATTR = 4;
static uint16_t pcm_pp_freq IDATA_ATTR = HW_FREQ_DEFAULT;
/* the stall watch's progress word (pcm_builtin_progress()) */
static unsigned long pcm_pp_pieces IBSS_ATTR;
#define PCM_PP_PIECE_DONE() (pcm_pp_pieces++)
#else
#define PCM_PP_PIECE_DONE() do { } while (0)
#endif

#ifdef HAVE_CORE_LAYOUT_A
/* 0.8.3 fifo96: the FIQ's timing (IRAM: the FIQ is the COP's) */
static uint32_t pcm_fiq_in_us IBSS_ATTR;    /* this FIQ's entry */

/* us for n FIFO words (pcm_tw_q8: one word's time x 256) */
static inline uint32_t pcm_words_us(uint32_t n)
{
    return (n * pcm_tw_q8) >> 8;
}

/* a FIQ came late us after the hardware ended its piece */
static void ICODE_ATTR pcm_note_late(int32_t us)
{
    const uint32_t *ctx = fiq_entry_ctx[CURRENT_CORE];

    if (us <= 0)
        return;
    if ((uint32_t)us > pcmpp.late_max_us)
    {
        pcmpp.late_max_us = us;
        pcmpp.late_pc = ctx[0] - 4;
        pcmpp.late_psr = ctx[1];
    }
    if ((uint32_t)us > pcm_words_us(16))
    {
        pcmpp.late_over++;
        pcmpp.over_pc = ctx[0] - 4;
        pcmpp.over_psr = ctx[1];
    }
}

/* the next piece is armed: the lateness model's start, and in the FIQ the
 * time from its entry */
#define PCM_PP_ARMED(size) do {                                     \
        pcm_arm_free = IIS_TX_FREE_COUNT;                           \
        pcm_arm_us = USEC_TIMER;                                    \
        pcm_arm_words = (size) / 4;                                 \
        if (pcm_fiq_busy &&                                         \
            pcm_arm_us - pcm_fiq_in_us > pcmpp.restart_max_us)      \
            pcmpp.restart_max_us = pcm_arm_us - pcm_fiq_in_us;      \
    } while (0)
#define PQ_STAT_PIECE()     (pcmpp.q_pieces++)
#define PQ_STAT_DRY()       (pcmpp.q_dry++)
#define PQ_STAT_ODD()       (pcmpp.q_odd++)
#define PQ_STAT_FELL()      (pcmpp.q_fell++)
/* the queued piece had moved b bytes when its FIQ looked: its start was
 * the last one's end, with the FIFO full */
#define PQ_STAT_LATE(b)     pcm_note_late((int32_t)pcm_words_us((b) / 4))
#define PQ_STAT_RESTART() do {                                      \
        uint32_t t_ = USEC_TIMER - pcm_fiq_in_us;                   \
        if (t_ > pcmpp.restart_max_us)                              \
            pcmpp.restart_max_us = t_;                              \
    } while (0)
/* st: the playing piece's status as the FIQ ends: its bytes left (the
 * field counts as the command's, less 4) and the full FIFO behind it */
#define PQ_STAT_SLACK(st) do {                                      \
        uint32_t s_ = pcm_words_us(((st) & 0xfffc) / 4 + 1 + 16);   \
        if (s_ < pcmpp.q_slack_min_us)                              \
            pcmpp.q_slack_min_us = s_;                              \
    } while (0)
#else
#define PCM_PP_ARMED(size) do { } while (0)
#endif

/* pcm-wait: begin (tests/pcmwait runs this block) */
/* ProkBox #312: every wait on the playback path is bounded.  The I2S FIFO
 * drains only while the codec clocks the I2S, and a playback channel told
 * to stop goes idle only once the FIFO takes its last word.  In DAC
 * standby (the codec's PLL off, wm8758.c) or with the I2S gated (AW-5,
 * wmcodec-pp.c) neither happens, and an unbounded wait spins for ever
 * with the PCM lock held.  The slowest drain: the FIFO's 16 words at
 * 8 kHz, one word a 16-bit frame: 2 ms (a 24-bit frame is two words, half
 * that).  The bound is four times it.  These waits run under the PCM lock
 * or in the FIQ, where the codec cannot be woken (dac_wake() sleeps on
 * I2C), so the start path wakes it first (pcm.c pcm_play_data()).  Here a
 * wait that reaches the bound clears the FIFO (a stopped channel's last
 * request is then served), waits once more and gives up: the stop
 * completes either way, and the fault is kept for one log line from
 * thread context (pcm_builtin_wait_fault(), pcm.c) */
#define PCM_PP_FIFO_WORDS   16
#define PCM_PP_SLOWEST_HZ   8000
#define PCM_PP_WAIT_US \
    (4ul * PCM_PP_FIFO_WORDS * 1000000ul / PCM_PP_SLOWEST_HZ)
#ifndef PCM_DMA_STATUS
#define PCM_DMA_STATUS(ch) \
    (*(volatile unsigned long *)(DMA0_BASE_ADDR + 0x20 * (ch) + 0x04))
#endif

static struct pcm_wait_fault pcm_wf IBSS_ATTR;
static unsigned long pcm_wf_dma IBSS_ATTR;  /* the channels' last status */

/* The wait is over: the channels in what idle (busy and completion clear;
 * reading a status acknowledges its completion), or the FIFO empty */
static bool pcm_wait_done(unsigned int what)
{
#if defined(CPU_PP502x)
    if (what != PCM_WAIT_FIFO)
    {
        unsigned long st = 0;

        if (what & PCM_WAIT_CH0)
            st |= PCM_DMA_STATUS(0);
        if (what & PCM_WAIT_CH1)
            st |= PCM_DMA_STATUS(1);
        pcm_wf_dma = st;
        return !(st & (DMA_STATUS_BUSY | DMA_STATUS_INTR));
    }
#endif
    return IIS_TX_IS_EMPTY;
}

/* true when the wait ended within us (one last look past the bound: a
 * long interrupt in between is not a fault) */
static bool pcm_wait_within(unsigned int what, unsigned long us)
{
    const unsigned long t0 = USEC_TIMER;

    while (!pcm_wait_done(what))
    {
        if (USEC_TIMER - t0 > us)
            return pcm_wait_done(what);
    }
    return true;
}

static void pcm_wait_bounded(unsigned int what)
{
    const unsigned long t0 = USEC_TIMER;
    unsigned long fifo;
    bool ok;

    if (pcm_wait_within(what, PCM_PP_WAIT_US))
        return;
    fifo = IISFIFO_CFG;
    pcm_wf.dma = what == PCM_WAIT_FIFO ? 0 : pcm_wf_dma;
    IISFIFO_CFG |= IIS_TXCLR;
    ok = pcm_wait_within(what, PCM_PP_WAIT_US);
    pcm_wf.fifo = fifo;
    pcm_wf.us = USEC_TIMER - t0;
    pcm_wf.what = what;
    pcm_wf.recovered = ok;
    pcm_wf.count++;     /* last: the reader's signal */
}

void pcm_builtin_wait_fault(struct pcm_wait_fault *out)
{
    *out = pcm_wf;
}
/* pcm-wait: end */

#if defined(CPU_PP502x)
/* 16-bit, L-R packed into 32 bits with left in the least significant halfword */
#define SAMPLE_SIZE   16
/* DMA Requests from IIS, Memory to peripheral, single transfer,
   wait for DMA request, interrupt on complete */
#define DMA_PLAY_CONFIG ((DMA_REQ_IIS << DMA_CMD_REQ_ID_POS) | \
                          DMA_CMD_RAM_TO_PER | DMA_CMD_SINGLE | \
                          DMA_CMD_WAIT_REQ | DMA_CMD_INTR)

static inline void dma_tx_init(void)
{
    /* Enable DMA controller */
    DMA_MASTER_CONTROL |= DMA_MASTER_CONTROL_EN;
    /* FIQ priority for DMA */
#ifdef HAVE_CORE_LAYOUT_A
    CORE_INT_REG(CPU_INT_PRIORITY, pcm_core) |= DMA_MASK;
#else
    CPU_INT_PRIORITY |= DMA_MASK;
#endif
    /* Enable request?? Not setting or clearing everything doesn't seem to
     * prevent it operating. Perhaps important for reliability (how requests
     * are handled). */
    DMA_REQ_STATUS |= 1ul << DMA_REQ_IIS;
    DMA0_STATUS;
}

static inline void dma_tx_setup(void)
{
    /* Setup DMA controller */
    DMA0_PER_ADDR = (unsigned long)&IISFIFO_WR;
    DMA0_FLAGS = DMA_FLAGS_UNK26;
    DMA0_INCR = DMA_INCR_RANGE_FIXED | DMA_INCR_WIDTH_32BIT;
}

static inline unsigned long dma_tx_buf_prepare(const void *addr)
{
    unsigned long a = (unsigned long)addr;

    if (a < UNCACHED_BASE_ADDR) {
        /* VA in DRAM - writeback all data and get PA */
        a = UNCACHED_ADDR(a);
        commit_dcache();
    }

    return a;
}

static inline void dma_tx_start(bool begin)
{
    size_t size = max_dma_chunk_size;

    /* Not at least max_dma_chunk_size left or there would be less
     * than a FIFO's worth of data after this transfer? */
    if (size + 16*4 > dma_play_data.size)
        size = dma_play_data.size;

    PCM_PP_ARMED(size);     /* 0.8.3 fifo96: the lateness model's start */
    /* Set the new DMA values and activate channel */
    DMA0_RAM_ADDR = dma_play_data.addr;
    DMA0_CMD = DMA_PLAY_CONFIG | (size - 4) | DMA_CMD_START;

    (void)begin;
}

static void dma_tx_stop(void)
{
    unsigned long status = DMA0_STATUS; /* Snapshot- resume from this point */
    unsigned long cmd = DMA0_CMD;
    size_t size = 0;

    /* Stop transfer */
    DMA0_CMD = cmd & ~(DMA_CMD_START | DMA_CMD_INTR);

    /* Wait for not busy + clear int (ProkBox #312: bounded) */
    pcm_wait_bounded(PCM_WAIT_CH0);

    if (status & DMA_STATUS_BUSY) {
        /* Transfer was interrupted - leave what's left */
        size = (cmd & 0xfffc) - (status & 0xfffc);
    }
    else if (status & DMA_STATUS_INTR) {
        /* Transfer was finished - DMA0_STATUS will have been reloaded
         * automatically with size in DMA0_CMD. Setup to restart on next
         * segment. */
        size = (cmd & 0xfffc) + 4;
    }
    /* else not an active state - size = 0 */

    dma_play_data.addr += size;
    dma_play_data.size -= size;

    if (dma_play_data.size == 0)
        dma_play_data.addr = 0; /* Entire buffer has completed. */
}

static inline void dma_tx_lock(void)
{
#ifdef HAVE_CORE_LAYOUT_A
    /* pcm_core's source off, from either core: DIS is a write-one-to-clear
     * register, so the write is atomic [rockbox-derived; that a CPU write
     * reaches the COP's mask is what bench row pcop checks] */
    CORE_INT_REG(CPU_INT_DIS, pcm_core) = DMA_MASK;
#else
    CPU_INT_DIS = DMA_MASK;
#endif
}

static inline void dma_tx_unlock(void)
{
#ifdef HAVE_CORE_LAYOUT_A
    CORE_INT_REG(CPU_INT_EN, pcm_core) = DMA_MASK;
#else
    CPU_INT_EN = DMA_MASK;
#endif
}

#if defined(HAVE_CORE_LAYOUT_A) && !defined(PCM_PP_QUEUE)
#define PCM_PP_QUEUE
#endif
#ifdef PCM_PP_QUEUE
/* 0.8.3 fifo96: the two-channel queue (docs/pp5022-reference/dma.md 2.1).
 * One channel can only be re-armed from the FIQ once its piece is done,
 * and the I2S FIFO then lasts 16 words: 83 us at 96 kHz 24-bit, so any
 * FIQ later than that is a click.  Here two general-engine channels on
 * the same I2S request take turns: while one plays, the other holds the
 * next piece, armed and waiting, and the hardware moves on to it with no
 * software in between.  A FIQ then has a whole piece (1.3 ms at 96 kHz
 * 24-bit, 5.8 ms at 44.1 kHz 16-bit, less the mix) to arm the one after.
 * That the engine serves two channels on one request one after the other,
 * in either order, is proved silently at the first start of each boot
 * (pq_probe(): [silicon] only once the owner's log says "proved"); a
 * refusal keeps the one-channel path.
 *
 * pq_n pieces are armed and not yet seen done; pq_act holds the older.
 * dma_play_data is what is not armed yet.  At a completion: the queued
 * piece plays already; if the done piece ended its buffer the mixer is
 * told (PCM_DMAST_STARTED: it mixes into the buffer just freed), then the
 * free channel is armed with what comes next, asking the PCM layer for
 * the next buffer when this one is all armed.  So the mixer's two buffers
 * are enough: one in the hardware, one mixed and armed or being mixed. */
/* pcm-queue: begin (tests/pcmdma/test_pcmq.c runs this block) */
#ifndef PQ_REG
#define PQ_REG(ch, off) \
    (*(volatile unsigned long *)(DMA0_BASE_ADDR + 0x20 * (ch) + (off)))
#endif
#define PQ_CMD(ch)    PQ_REG(ch, 0x00)
#define PQ_STATUS(ch) PQ_REG(ch, 0x04)
#define PQ_RAM(ch)    PQ_REG(ch, 0x10)

static uint8_t pq_on IBSS_ATTR;         /* the queue is in use */
static uint8_t pq_n IBSS_ATTR;          /* pieces armed, not yet seen done */
static uint8_t pq_act IBSS_ATTR;        /* the channel of the older one */
static uint8_t pq_last[2] IBSS_ATTR;    /* its piece ends its buffer */
static uint16_t pq_sz4[2] IBSS_ATTR;    /* its piece's size field */
/* fifo96 review: the queue failing in use (a hand-over found the queued
 * piece done too a second time since the start, or an odd order): no more
 * pieces are queued; once the armed ones end the play goes on with one
 * channel, for the rest of the boot (pq_on clear), counted (fell) */
static uint8_t pq_fail IBSS_ATTR;
static uint8_t pq_dry_run IBSS_ATTR;    /* dry hand-overs since the start */
static uint8_t pq_ready IBSS_ATTR;      /* the mixer holds a mixed buffer
                                           not handed out yet */

/* The next piece of dma_play_data onto the free channel, cut as
 * dma_tx_start() cuts (whole words; no FIFO's worth left over), at most
 * cap bytes.  With one armed already it waits for that one's end */
static void ICODE_ATTR pq_arm(size_t cap)
{
    const unsigned int ch = pq_n ? pq_act ^ 1 : pq_act;
    size_t size = cap;

    if (size + 16*4 > dma_play_data.size)
        size = dma_play_data.size;
    PQ_RAM(ch) = dma_play_data.addr;
    PQ_CMD(ch) = DMA_PLAY_CONFIG | (size - 4) | DMA_CMD_START;
    pq_sz4[ch] = size - 4;
    dma_play_data.addr += size;
    dma_play_data.size -= size;
    pq_last[ch] = dma_play_data.size == 0;
    pq_n++;
}

/* Both channels armed: the rest of this buffer, else the next one.
 * false: no more data (the PCM layer has stopped the PCM) */
static bool ICODE_ATTR pq_fill(void)
{
    while (pq_n < 2)
    {
        if (pq_fail && pq_n != 0)
            return true;            /* the armed one ends first */
        if (dma_play_data.size == 0)
        {
            if (!pcm_play_dma_complete_callback(PCM_DMAST_OK,
                                                &dma_play_data.p_r,
                                                &dma_play_data.size))
                return false;
            pq_ready = 0;
            dma_play_data.addr = dma_tx_buf_prepare(dma_play_data.p_r);
        }
        if (pq_fail)
        {
            /* nothing armed: channel 0 alone from here, as the one-channel
             * FIQ expects it (dma_play_data from the piece it plays, the
             * mixer one buffer ahead) */
            pq_on = 0;
            PQ_STAT_FELL();
            dma_tx_start(false);
            if (!pq_ready)
                pcm_play_dma_status_callback(PCM_DMAST_STARTED);
            return true;
        }
        pq_arm(max_dma_chunk_size);
    }
    return true;
}

/* At the start: the first buffer in two pieces at least, so the second
 * waits armed from the first word (a 1 KB mixer frame is one piece at
 * 64 kHz and up) */
static void pq_start(void)
{
    size_t half = (dma_play_data.size / 2) & ~3;

    pq_n = 0;
    pq_act = 0;
    pq_dry_run = 0;
    pq_ready = 1;                   /* the mixer's start mixed two */
    pq_arm(half >= 64 && half < max_dma_chunk_size ? half
                                                   : max_dma_chunk_size);
    if (dma_play_data.size != 0)
        pq_arm(max_dma_chunk_size);
}

/* The FIQ.  Reads only the older piece's status (reading acknowledges);
 * the queued one cannot end before it.  Late enough that both ended, the
 * loop takes both, the gap counted (dry) */
static void ICODE_ATTR pq_fiq(void)
{
    unsigned long st;
    bool first = true;

    if (pq_n == 0)
        return;
    st = PQ_STATUS(pq_act);
    if (!(st & DMA_STATUS_INTR) && pq_n == 2)
    {
        /* the queued piece ended and the older did not: an engine the
         * probe should have refused.  Taken, so its request cannot hold
         * the FIQ up for ever; counted */
        const unsigned long o = PQ_STATUS(pq_act ^ 1);

        if (o & DMA_STATUS_INTR)
        {
            /* the queued piece played before the older one: their words
             * came out of order.  Never on an engine the probe proved, so
             * the queue is given up at once (fifo96 review) */
            PQ_STAT_ODD();
            pq_fail = 1;
            pq_act ^= 1;
            st = o;
        }
    }
    while (st & DMA_STATUS_INTR)
    {
        const bool last = pq_last[pq_act];

        PCM_PP_PIECE_DONE();
        PQ_STAT_PIECE();
        if (--pq_n != 0)
        {
            pq_act ^= 1;                /* the queued piece plays */
            st = PQ_STATUS(pq_act);
            if (st & DMA_STATUS_INTR)
            {
                /* it ended too: the FIFO ran out.  A second time since
                 * the start (a FIQ that never comes for one channel, a
                 * late one each piece) gives the queue up */
                PQ_STAT_DRY();
                if (++pq_dry_run >= 2)
                    pq_fail = 1;
            }
            else if (first && (st & 0xfffc) <= (pq_sz4[pq_act] & 0xfffc))
                PQ_STAT_LATE((pq_sz4[pq_act] & 0xfffc) - (st & 0xfffc));
        }
        else
        {
            if (!pq_fail)
                PQ_STAT_DRY();          /* nothing was queued: a gap */
            st = 0;
        }
        first = false;
        /* a buffer ended: the mixer refills it, unless it still holds one
         * not handed out (only while giving the queue up: two ends with
         * no fill between; the fallback's start mixes the freed one) */
        if (last && !pq_ready)
        {
            pcm_play_dma_status_callback(PCM_DMAST_STARTED);
            pq_ready = 1;
        }
        if (!pq_fill() || !pq_on)
            return;
        PQ_STAT_RESTART();
        if (!(st & DMA_STATUS_INTR))
        {
            st = PQ_STATUS(pq_act);     /* ended meanwhile? */
            if (!(st & DMA_STATUS_INTR))
                PQ_STAT_SLACK(st);
        }
    }
}
/* Both channels stopped: the waiting one first, so it cannot start */
static void pq_stop(void)
{
    const unsigned int q = pq_act ^ 1;

    PQ_CMD(q) &= ~(DMA_CMD_START | DMA_CMD_INTR);
    PQ_CMD(pq_act) &= ~(DMA_CMD_START | DMA_CMD_INTR);
    pcm_wait_bounded(PCM_WAIT_CH0 | PCM_WAIT_CH1);     /* #312 */
    pq_n = 0;
}
/* pcm-queue: end */


static void pq_setup(void)
{
    DMA1_PER_ADDR = (unsigned long)&IISFIFO_WR;
    DMA1_FLAGS = DMA_FLAGS_UNK26;
    DMA1_INCR = DMA_INCR_RANGE_FIXED | DMA_INCR_WIDTH_32BIT;
}

/* The silent proof, at the PCM's start (the PCM lock held: the source is
 * masked, no FIQ runs): two pieces of zeros, one on each channel, armed
 * back to back, both orders.  Proved when the waiting channel did not
 * move while the first was busy (at most one sample, the hand-over's read
 * race), the FIFO never ran dry between them, and both raised their
 * completion bit.  ~3 ms at 44.1 kHz, once a boot (no progress in 20 ms:
 * again at the next start, three runs at most) */
#define PQ_PROBE_BYTES 128
static uint32_t pq_zero[2 * PQ_PROBE_BYTES / 4] NOCACHEBSS_ATTR
    __attribute__((aligned(16)));
static struct pcm_pp_qprobe pq_res NOCACHEBSS_ATTR;

static int pq_probe_dir(unsigned int first, struct pcm_pp_qdir *d)
{
    const unsigned int second = first ^ 1;
    const unsigned long full = (PQ_PROBE_BYTES - 4) & 0xfffc;
    unsigned long base = (unsigned long)pq_zero;
    unsigned long a, b, t0, t = 0;
    bool first_done = false;
    int rc = -1;

    if (base < UNCACHED_BASE_ADDR)
        base = UNCACHED_ADDR(base);
    memset(d, 0, sizeof(*d));
    (void)PQ_STATUS(0);
    (void)PQ_STATUS(1);
    PQ_RAM(first) = base;
    PQ_CMD(first) = DMA_PLAY_CONFIG | (PQ_PROBE_BYTES - 4) | DMA_CMD_START;
    PQ_RAM(second) = base + PQ_PROBE_BYTES;
    PQ_CMD(second) = DMA_PLAY_CONFIG | (PQ_PROBE_BYTES - 4) | DMA_CMD_START;
    t0 = USEC_TIMER;
    while (1)
    {
        unsigned long fifo_free;

        a = PQ_STATUS(first);
        b = PQ_STATUS(second);
        fifo_free = IIS_TX_FREE_COUNT;
        t = USEC_TIMER - t0;
        if (d->samples == 0)
            d->second_st0 = b;
        if (d->samples < 0xffff)
            d->samples++;
        if (a & DMA_STATUS_INTR)
            d->intr_first = 1;
        if (b & DMA_STATUS_INTR)
            d->intr_second = 1;
        if ((a & DMA_STATUS_BUSY) && (b & 0xfffc) < full &&
            d->overlap < 0xffff)
            d->overlap++;
        if (!(a & DMA_STATUS_BUSY))
            first_done = true;
        if (first_done && (b & DMA_STATUS_BUSY) && fifo_free >= 16 &&
            d->gap < 0xffff)
            d->gap++;
        /* done when both have raised their completion (what the queue's
         * FIQ relies on), whatever a waiting channel's busy bit says */
        if (d->intr_first && d->intr_second && !((a | b) & DMA_STATUS_BUSY))
        {
            rc = 0;
            break;
        }
        if (t > 20000)
            break;
    }
    d->us = t;
    PQ_CMD(second) &= ~(DMA_CMD_START | DMA_CMD_INTR);
    PQ_CMD(first) &= ~(DMA_CMD_START | DMA_CMD_INTR);
    pcm_wait_bounded(PCM_WAIT_CH0 | PCM_WAIT_CH1);     /* #312 */
    return rc;
}

static void pq_probe(void)
{
    int r0, r1;

    dma_tx_setup();
    pq_setup();
    r0 = pq_probe_dir(0, &pq_res.dir[0]);
    r1 = pq_probe_dir(1, &pq_res.dir[1]);
    pq_res.runs++;
    if (r0 != 0 || r1 != 0)
    {
        /* no progress (the codec's clocks?): again at the next start */
        pq_res.verdict = pq_res.runs < 3 ? 3 : 2;
    }
    else
    {
        bool ok = true;

        for (int i = 0; i < 2; i++)
        {
            const struct pcm_pp_qdir *d = &pq_res.dir[i];

            /* one overlapping sample can be the read race at the
             * hand-over (the first's status read just before it ended); a
             * shared request overlaps for the whole piece */
            if (d->overlap > 1 || d->gap || !d->intr_first ||
                !d->intr_second)
                ok = false;
        }
        pq_res.verdict = ok ? 1 : 2;
    }
    pq_on = pq_res.verdict == 1;
}

void pcm_pp_queue_probe_result(struct pcm_pp_qprobe *out)
{
    *out = pq_res;
}

bool pcm_pp_queue_on(void)
{
    return pq_on;
}

void pcm_pp_queue_refuse(void)
{
    if (dma_play_data.state != 0)
        return;
    pq_on = 0;
    pq_res.verdict = 2;
}
#endif /* PCM_PP_QUEUE */

/* NOTE: direct stack use forbidden by GCC stack handling bug for FIQ */
void fiq_playback(void) ICODE_ATTR __attribute__((interrupt("FIQ")));
void fiq_playback(void)
{
#ifdef HAVE_CORE_LAYOUT_A
    /* A lock on the other core masks pcm_core's source and then waits
     * while this flag is up.  Flag first, then the mask's state: a FIQ
     * raised just before the mask landed leaves its request pending
     * (DMA0_STATUS unread) and runs again at the unlock */
    pcm_fiq_busy = 1;
    if (UNLIKELY(!(CORE_INT_REG(CPU_INT_EN_STAT, CURRENT_CORE) & DMA_MASK)))
    {
        pcmpp.held_off++;
        pcm_fiq_busy = 0;
        return;
    }
    pcmpp.fiq[CURRENT_CORE]++;
    {
        /* 0.8.3: the FIFO as this piece ended, and the gap since the
         * last piece's FIQ: a FIFO found empty is a click */
        uint32_t now = USEC_TIMER;
        uint32_t free = IIS_TX_FREE_COUNT;

        pcm_fiq_in_us = now;
        if (pcm_fiq_warm >= 2)
        {
            uint32_t gap = now - pcm_fiq_last_us;

            if (free >= 16)
                pcmpp.fifo_empty++;
            if (free > pcmpp.fifo_free_max)
                pcmpp.fifo_free_max = free;
            if (gap > pcmpp.gap_max_us)
                pcmpp.gap_max_us = gap;
        }
        else
            pcm_fiq_warm++;
        pcm_fiq_last_us = now;
    }
#ifdef PCM_PP_QUEUE
    if (pq_on)
    {
        pq_fiq();
        goto fiq_out;
    }
#endif
    /* 0.8.3 fifo96: one channel: how late past the piece's end (the
     * model at PCM_PP_ARMED) */
    pcm_note_late((int32_t)(pcm_fiq_in_us - pcm_arm_us) -
                  (int32_t)pcm_words_us(pcm_arm_words > pcm_arm_free ?
                                        pcm_arm_words - pcm_arm_free : 0));
#endif
    DMA0_STATUS; /* Clear any pending interrupt */

    size_t size = (DMA0_CMD & 0xffff) + 4; /* Get size of trasfer that caused
                                              this interrupt */
    PCM_PP_PIECE_DONE();
    dma_play_data.addr += size;
    dma_play_data.size -= size;

    if (LIKELY(dma_play_data.size != 0)) {
        /* Begin next segment */
        dma_tx_start(false);
    }
    else if (pcm_play_dma_complete_callback(PCM_DMAST_OK, &dma_play_data.p_r,
                                            &dma_play_data.size)) {
#ifdef HAVE_DEVSTATS
        uint32_t ds_t0 = USEC_TIMER;    /* the write-back: adds only */
#endif
        dma_play_data.addr = dma_tx_buf_prepare(dma_play_data.p_r);
#ifdef HAVE_DEVSTATS
#ifdef HAVE_CORE_LAYOUT_A
        /* devstats is the CPU's cached memory: the COP never writes it */
        if (CURRENT_CORE == CPU)
#endif
        DEVSTATS_PCM_WB(USEC_TIMER - ds_t0);
#endif
        dma_tx_start(false);
        pcm_play_dma_status_callback(PCM_DMAST_STARTED);
    }
#ifdef HAVE_CORE_LAYOUT_A
#ifdef PCM_PP_QUEUE
fiq_out:
#endif
    {
        uint32_t t = USEC_TIMER - pcm_fiq_in_us;

        if (t > pcmpp.handler_max_us)
            pcmpp.handler_max_us = t;
    }
    pcm_fiq_busy = 0;
#endif
}

#else /* !defined (CPU_PP502x) */

/* 32-bit, one left 32-bit sample followed by one right 32-bit sample */
#define SAMPLE_SIZE   32

static void dma_tx_init(void)
{
    /* Set up banked registers for FIQ mode */

    /* Use non-banked registers for scratch. */
    register volatile void *iiscfg asm("r0") = &IISCONFIG;
    register volatile void *dmapd asm("r1") = &dma_play_data;

    asm volatile (
        "mrs    r2, cpsr            \n" /* Save mode and interrupt status */
        "msr    cpsr_c, #0xd1       \n" /* Switch to FIQ mode */
        "mov    r8, #0              \n"
        "mov    r9, #0              \n"
        "mov    r10, %[iiscfg]      \n"
        "mov    r11, %[dmapd]       \n"
        "msr    cpsr_c, r2          \n"
        :
        : [iiscfg]"r"(iiscfg), [dmapd]"r"(dmapd)
        : "r2");

    /* FIQ priority for I2S */
    CPU_INT_PRIORITY |= IIS_MASK;
    CPU_INT_EN = IIS_MASK;
}

static inline void dma_tx_setup(void)
{
    /* Nothing to do */
}

static inline unsigned long dma_tx_buf_prepare(const void *addr)
{
    return (unsigned long)addr;
}

static inline void dma_tx_start(bool begin)
{
    if (begin) {
        IISCONFIG &= ~IIS_TXFIFOEN; /* Stop transmitting */
    }

    /* Fill the FIFO or start when data is used up */
    while (IIS_TX_FREE_COUNT >= 2 && dma_play_data.size != 0) {
        IISFIFO_WRH = *dma_play_data.p32++;
        IISFIFO_WRH = *dma_play_data.p32++;
        dma_play_data.size -= 4;
    }

    if (begin) {
        IISCONFIG |= IIS_TXFIFOEN; /* Start transmitting */
    }
}

static inline void dma_tx_stop(void)
{
    /* Disable TX interrupt */
    IIS_IRQTX_REG &= ~IIS_IRQTX;
}

static inline void dma_tx_lock(void)
{
    IIS_IRQTX_REG &= ~IIS_IRQTX;
}

static inline void dma_tx_unlock(void)
{
    IIS_IRQTX_REG |= IIS_IRQTX;
}

/* ASM optimised FIQ handler. Checks for the minimum allowed loop cycles by
 * evalutation of free IISFIFO-slots against available source buffer words.
 * Through this it is possible to move the check for IIS_TX_FREE_COUNT outside
 * the loop and do some further optimization. Right after the loops (source
 * buffer -> IISFIFO) are done we need to check whether we have to exit FIQ
 * handler (this must be done, if all free FIFO slots were filled) or we will
 * have to get some new source data. Important information kept from former
 * ASM implementation (not used anymore): GCC fails to make use of the fact
 * that FIQ mode has registers r8-r14 banked, and so does not need to be saved.
 * This routine uses only these registers, and so will never touch the stack
 * unless it actually needs to do so when calling pcm_play_dma_complete_callback.
 * C version is still included below for reference and testing.
 */
#if 1
void fiq_playback(void) ICODE_ATTR __attribute__((naked));
void fiq_playback(void)
{
    /* r8 and r9 contains local copies of p and size respectively.
     * r10 contains IISCONFIG address (set during PCM init to minimize code in
     * FIQ handler.Most other addresses we need are generated by using offsets
     * from this.
     * r10 + 0x40 is IISFIFO_WR, and r10 + 0x1c is IISFIFO_CFG.
     * r11 contains address of dma_play_data
     * r12 and r14 are working registers.
     *
     * Divided into two blocks: one where no external calls are needed and
     * one where external callbacks are made
     */
    asm volatile (
    /* No external calls */
        BEGIN_ARM_ASM_SYNTAX_UNIFIED
        "sub     lr, lr, #4           \n" /* Prepare return address */
        "stmfd   sp!, { lr }          \n" /* stack lr so we can use it */
        "ldr     r12, =0xcf001040     \n" /* Some magic from iPodLinux ... */
        "ldr     r12, [r12]           \n" /* ... actually a DMA INT ack? */
        "ldmia   r11, { r8-r9 }       \n" /* r8 = p, r9 = size */
        "cmp     r9, #0               \n" /* is size 0? */
        "beq     1f                   \n" /* if so, ask PCM for more data */

        "ldr     r14, [r10, #0x1c]    \n" /* read IISFIFO_CFG to check FIFO status */
        "and     r14, r14, #(0xe<<23) \n" /* r14 = (IIS_TX_FREE_COUNT & ~1) << 23 */
        "cmp     r9, r14, lsr #22     \n" /* number of words from source */
        "movlo   r14, r9, lsl #22     \n" /* r14 = amount of allowed loops */
        "sub     r9, r9, r14, lsr #22 \n" /* r14 words will be written in loop */
    "0:                               \n"
        "ldr     r12, [r8], #4        \n" /* load left-right pair */
        "subs    r14, r14, #(0x2<<23) \n" /* one more loop? ... */
        "strh    r12, [r10, #0x40]    \n" /* left sample to IISFIFO_WR */
        "mov     r12, r12, lsr #16    \n" /* put right sample in bottom 16 bits */
        "strh    r12, [r10, #0x40]    \n" /* right sample to IISFIFO_WR */
        "bhi     0b                   \n" /* ... yes, continue */

        "cmp     r9, #0               \n" /* either FIFO full or size empty? */
        "stmiane r11, { r8-r9 }       \n" /* save p and size, if not empty */
        "ldmfdne sp!, { pc }^         \n" /* RFE if not empty */

    /* Making external calls */
    "1:                               \n"
        "stmfd   sp!, { r0-r3 }       \n" /* Must save volatiles */
    "2:                               \n"
        "mov     r0, %0               \n" /* r0 = status */
        "mov     r1, r11              \n" /* r1 = &dma_play_data.p_r */
        "add     r2, r11, #4          \n" /* r2 = &dma_play_data.size */
        "ldr     r3, =pcm_play_dma_complete_callback \n"
        "mov     lr, pc               \n" /* long call (not in same section) */
        "bx      r3                   \n"
        "cmp     r0, #0               \n" /* more data? */
        "ldmfdeq sp!, { r0-r3, pc }^  \n" /* no? -> exit */

        "ldr     r14, [r10, #0x1c]    \n" /* read IISFIFO_CFG to check FIFO status */
        "ands    r14, r14, #(0xe<<23) \n" /* r14 = (IIS_TX_FREE_COUNT & ~1) << 23 */
        "bne     4f                   \n"
    "3:                               \n" /* inform of started status if registered */
        "ldr     r1, =pcm_play_status_callback \n"
        "ldr     r1, [r1]             \n"
        "cmp     r1, #0               \n"
        "movne   r0, %1               \n"
        "movne   lr, pc               \n"
        "bxne    r1                   \n"
        "ldmfd   sp!, { r0-r3, pc }^  \n" /* exit */
    "4:                               \n"
        "ldmia   r11, { r8-r9 }       \n" /* load new p and size */
        "cmp     r9, r14, lsr #22     \n" /* number of words from source */
        "movlo   r14, r9, lsl #22     \n" /* r14 = amount of allowed loops */
        "sub     r9, r9, r14, lsr #22 \n" /* r14 words will be written in loop */
    "0:                               \n"
        "ldr     r12, [r8], #4        \n" /* load left-right pair */
        "subs    r14, r14, #(0x2<<23) \n" /* one more loop? ... */
        "strh    r12, [r10, #0x40]    \n" /* left sample to IISFIFO_WR */
        "mov     r12, r12, lsr #16    \n" /* put right sample in bottom 16 bits */
        "strh    r12, [r10, #0x40]    \n" /* right sample to IISFIFO_WR */
        "bhi     0b                   \n" /* ... yes, continue */
        "stmia   r11, { r8-r9 }       \n" /* save p and size */

        "cmp     r9, #0               \n" /* used up data in FIFO fill? */
        "bne     3b                   \n" /* no? -> go return */
        "b       2b                   \n" /* yes -> get even more */
        ".ltorg                       \n"
        END_ARM_ASM_SYNTAX_UNIFIED
        : /* These must only be integers! No regs */
        : "i"(PCM_DMAST_OK), "i"(PCM_DMAST_STARTED));
}

#else /* C version for reference */

/* NOTE: direct stack use forbidden by GCC stack handling bug for FIQ */
void fiq_playback(void) ICODE_ATTR __attribute__((interrupt ("FIQ")));
void fiq_playback(void)
{
    inl(0xcf001040);

    if (LIKELY(dma_play_data.size != 0)) {
        dma_tx_start(false);

        if (dma_play_data.size != 0) {
            /* Still more data */
            return;
        }
    }

    while (pcm_play_dma_complete_callback(PCM_DMAST_OK, &dma_play_data.p_r,
                                          &dma_play_data.size)) {
        dma_tx_start(false);
        pcm_play_dma_status_callback(PCM_DMAST_STARTED);

        if (dma_play_data.size != 0) {
            return;
        }
    }
}
#endif /* ASM / C selection */
#endif /* CPU_PP502x */

#ifdef HAVE_CORE_LAYOUT_A
/* 0.8.3 fifo96: one FIFO word's time in us x 256: 16-bit frames are one
 * word, 24-bit frames two (bpf: bytes a frame) */
static void pcm_tw_update(unsigned long rate, unsigned int bpf)
{
    const uint32_t u = 4000000u / bpf;  /* 1 s in us over words a frame */

    if (rate == 0)
        return;
    pcm_tw_q8 = ((u / rate) << 8) + ((u % rate) << 8) / rate;
}
#endif

static void sink_set_freq(uint16_t freq) {
    /* ProkBox #191: whole 32-bit words (DMA0_INCR is 32-bit wide).  The
     * upstream rate >> 6 gave odd lengths (44.1 kHz: 689 bytes), and every
     * chunk after the first then started off a word boundary */
#ifdef HAVE_PCM_24BIT_OUT
    /* #268: the same time a piece (1/64 s) in either frame size */
    pcm_pp_freq = freq;
    max_dma_chunk_size = ((hw_freq_sampr[freq] >> 6) & ~3) * (pcm_pp_bpf / 4);
#else
    max_dma_chunk_size = (hw_freq_sampr[freq] >> 6) & ~3;
#endif
#ifdef HAVE_CORE_LAYOUT_A
#ifdef HAVE_PCM_24BIT_OUT
    pcm_tw_update(hw_freq_sampr[freq], pcm_pp_bpf);
#else
    pcm_tw_update(hw_freq_sampr[freq], 4);
#endif
#endif
    audiohw_set_frequency(freq);
}

#ifdef HAVE_PCM_24BIT_OUT
/* #268: FIFO format 3 (one 32-bit word a sample, the 24 bits at the top)
   with the codec's 24-bit word, or format 7 (one word a 16-bit stereo
   frame) with its 16-bit word: b11 i2s24 [silicon], the I2S size code
   kept (16-bit; all four codes played clean).  The PCM is stopped (the
   mixer's start), so no FIFO holds the old format's words */
void pcm_builtin_set_depth(int bits)
{
    uint8_t bpf = bits > 16 ? 8 : 4;

    if (bpf == pcm_pp_bpf)
        return;
    pcm_pp_bpf = bpf;
    IISCONFIG = (IISCONFIG & ~IIS_FIFO_FORMAT_MASK) |
                (bpf == 8 ? IIS_FIFO_FORMAT_LE32 : IIS_FIFO_FORMAT_LE16_2);
    IISFIFO_CFG |= IIS_TXCLR;
    audiohw_set_depth(bpf == 8 ? 24 : 16);
    max_dma_chunk_size = ((hw_freq_sampr[pcm_pp_freq] >> 6) & ~3) *
                         (bpf / 4);
#ifdef HAVE_CORE_LAYOUT_A
    pcm_tw_update(hw_freq_sampr[pcm_pp_freq], bpf);
#endif
}

unsigned long pcm_builtin_progress(void)
{
    return pcm_pp_pieces;
}
#endif

#ifdef HAVE_CORE_LAYOUT_A
/* The locker is not pcm_core: its mask has stopped new FIQs there, and a
 * FIQ already running there is waited out (bounded; a stuck one is
 * counted, never hung on) */
static void fiq_wait_out(void)
{
    uint32_t n = 0;

    (void)CORE_INT_REG(CPU_INT_EN_STAT, pcm_core);  /* the mask has landed */
    while (pcm_fiq_busy)
    {
        if (++n > 100000)
        {
            pcmpp.lock_stuck++;
            break;
        }
    }
    if (n)
    {
        pcmpp.lock_waits++;
        if (n > pcmpp.lock_spin_max)
            pcmpp.lock_spin_max = n;
    }
}

/* Either core may lock.  The depth counter and the mask decisions sit
 * under a corelock (uncached, IRAM); the mask on pcm_core's source is what
 * keeps the FIQ out once the lock returns, on either core */
static void sink_lock(void)
{
    const unsigned int core = CURRENT_CORE;
    int status = disable_interrupt_save(IRQ_FIQ_STATUS);

    corelock_lock(&pcm_lock_cl);
    if (core != pcm_core)
        pcmpp.lock_xcore++;
    if (++dma_play_data.locked == 1) {
        dma_tx_lock();
        if (core != pcm_core)
            fiq_wait_out();
        pcm_lock_us = USEC_TIMER;   /* fifo96: the source masked from */
        pcm_lock_playing = dma_play_data.state != 0;
    }
    corelock_unlock(&pcm_lock_cl);
    restore_interrupt(status);
}

static void sink_unlock(void)
{
    int status = disable_interrupt_save(IRQ_FIQ_STATUS);

    corelock_lock(&pcm_lock_cl);
    if (dma_play_data.locked == 1) {
        /* fifo96: how long the source was masked while playing (a FIQ
         * waits); not a start (the probe) nor a stop (the FIFO drain) */
        uint32_t t = USEC_TIMER - pcm_lock_us;

        if (pcm_lock_playing && dma_play_data.state != 0 &&
            t > pcmpp.lock_hold_max_us)
            pcmpp.lock_hold_max_us = t;
    }
    if (--dma_play_data.locked == 0 && dma_play_data.state != 0) {
        dma_tx_unlock();
    }
    corelock_unlock(&pcm_lock_cl);
    restore_interrupt(status);
}

int pcm_pp_set_core(unsigned int core)
{
    int status, rc = -1;

    if (core >= NUM_CORES || CURRENT_CORE != CPU)
        return -1;
    status = disable_interrupt_save(IRQ_FIQ_STATUS);
    corelock_lock(&pcm_lock_cl);
    if (dma_play_data.state == 0 && dma_play_data.locked == 0) {
        unsigned int old = pcm_core;

        if (core != old) {
            /* both off; the priority moves; the old core's target goes
               back to the catch-all.  The next start enables the new one */
            CORE_INT_REG(CPU_INT_DIS, old) = DMA_MASK;
            CORE_INT_REG(CPU_INT_DIS, core) = DMA_MASK;
            CORE_INT_REG(CPU_INT_PRIORITY, old) &= ~DMA_MASK;
            CORE_INT_REG(CPU_INT_PRIORITY, core) |= DMA_MASK;
            FIQ_TARGET_SET(old, fiq_spurious);
            pcm_core = core;
        }
        rc = 0;
    }
    corelock_unlock(&pcm_lock_cl);
    restore_interrupt(status);
    return rc;
}

unsigned int pcm_pp_core(void)
{
    return pcm_core;
}

void pcm_pp_get_stats(struct pcm_pp_stats *out)
{
    *out = pcmpp;
}

/* 0.8.3: the maxima start over (a run's own); a race with the FIQ only
 * loses one sample of a statistic */
void pcm_pp_reset_max(void)
{
    pcmpp.fifo_free_max = 0;
    pcmpp.gap_max_us = 0;
    pcmpp.late_max_us = 0;
    pcmpp.late_pc = pcmpp.late_psr = 0;
    pcmpp.over_pc = pcmpp.over_psr = 0;
    pcmpp.restart_max_us = 0;
    pcmpp.handler_max_us = 0;
    pcmpp.lock_hold_max_us = 0;
    pcmpp.q_slack_min_us = ~0u;
}
#else /* !HAVE_CORE_LAYOUT_A */
/* For the locks, FIQ must be disabled because the handler manipulates
   IISCONFIG and the operation is not atomic - dual core support
   will require other measures */
static void sink_lock(void)
{
    int status = disable_fiq_save();

    if (++dma_play_data.locked == 1) {
        dma_tx_lock();
    }

    restore_fiq(status);
}

static void sink_unlock(void)
{
    int status = disable_fiq_save();

    if (--dma_play_data.locked == 0 && dma_play_data.state != 0) {
        dma_tx_unlock();
    }

    restore_fiq(status);
}
#endif /* HAVE_CORE_LAYOUT_A */

static void sink_start_pcm(void)
{
    FIQ_TARGET_SET(IF_LAYOUT_A_CORE, fiq_playback);
    dma_play_data.state = 1;
#ifdef PCM_PP_QUEUE
    if (pq_on)
    {
        pq_start();
        return;
    }
#endif
    dma_tx_start(true);
}

static void sink_stop_pcm(void)
{
#ifdef PCM_PP_QUEUE
    if (pq_on)
        pq_stop();
    else
#endif
    dma_tx_stop();

    /* Wait for FIFO to empty.  ProkBox #270, #312: bounded, at every
       rate (pcm_wait_bounded(): a codec in standby or that lost its PLL
       never drains it) */
    pcm_wait_bounded(PCM_WAIT_FIFO);

    dma_play_data.state = 0;
}

static void sink_dma_stop(void);

static void sink_dma_start(const void *addr, size_t size)
{
    sink_dma_stop();

#if NUM_CORES > 1
    /* This will become more important later - and different ! */
    dma_play_data.core = processor_id(); /* save initiating core */
#endif

#ifdef PCM_PP_QUEUE
    /* fifo96: the queue's silent proof, at the first start of a boot (the
     * PCM lock is held: the FIQ's source is masked).  The stop above left
     * both channels idle; pq_on changes only here, while stopped */
    if (pq_res.verdict == 0 || pq_res.verdict == 3)
        pq_probe();
    if (pq_on)
        pq_setup();
#endif
    dma_tx_setup();

    dma_play_data.addr = dma_tx_buf_prepare(addr);
    dma_play_data.size = size;
#ifdef HAVE_CORE_LAYOUT_A
    pcm_fiq_warm = 0;   /* 0.8.3: the FIFO starts empty */
#endif
    sink_start_pcm();
}

/* Stops the DMA transfer and interrupt */
static void sink_dma_stop(void)
{
    sink_stop_pcm();
    dma_play_data.addr = 0;
    dma_play_data.size = 0;
#if NUM_CORES > 1
    dma_play_data.core = 0; /* no core in control */
#endif
}

static void sink_dma_init(void)
{
    /* Initialize default register values. */
    audiohw_init();

#ifdef HAVE_CORE_LAYOUT_A
    /* the CPU takes the PCM's FIQ until pcm_pp_set_core() moves it */
    FIQ_TARGET_SET(CPU, fiq_spurious);
    FIQ_TARGET_SET(COP, fiq_spurious);
    corelock_init(&pcm_lock_cl);
    pcm_core = CPU;
    pcmpp.q_slack_min_us = ~0u;     /* fifo96: a minimum */
#endif
    dma_tx_init();

    IISCONFIG |= IIS_TXFIFOEN;
}

struct pcm_sink builtin_pcm_sink = {
    .caps = {
        .samprs       = hw_freq_sampr,
        .num_samprs   = HW_NUM_FREQ,
        .default_freq = HW_FREQ_DEFAULT,
        .volume_type  = PCM_NATIVE_VOLUME_TYPE,
    },
    .ops = {
        .init     = sink_dma_init,
        .postinit = audiohw_postinit,
        .set_freq = sink_set_freq,
        .lock     = sink_lock,
        .unlock   = sink_unlock,
        .play     = sink_dma_start,
        .stop     = sink_dma_stop,
    },
};

/****************************************************************************
 ** Recording DMA transfer
 **/
#ifdef HAVE_RECORDING
/* PCM recording interrupt routine lockout */
static struct dma_data dma_rec_data IBSS_ATTR =
{
    /* Initialize to a locked, stopped state */
    { .addr = 0 },
    .size = 0,
#if NUM_CORES > 1
    .core = 0x00,
#endif
    .locked = 0,
    .state  = 0
};

/* For the locks, FIQ must be disabled because the handler manipulates
   IISCONFIG and the operation is not atomic - dual core support
   will require other measures */
void pcm_rec_lock(void)
{
    int status = disable_fiq_save();

    if (++dma_rec_data.locked == 1)
        IIS_IRQRX_REG &= ~IIS_IRQRX;

    restore_fiq(status);
}

void pcm_rec_unlock(void)
{
    int status = disable_fiq_save();

    if (--dma_rec_data.locked == 0 && dma_rec_data.state != 0)
        IIS_IRQRX_REG |= IIS_IRQRX;

    restore_fiq(status);
}

/* NOTE: direct stack use forbidden by GCC stack handling bug for FIQ */
void fiq_record(void) ICODE_ATTR __attribute__((interrupt ("FIQ")));

#if defined(SANSA_C200) || defined(SANSA_E200)
void fiq_record(void)
{
    register int32_t value;

    if (audio_channels == 2) {
        /* RX is stereo */
        while (dma_rec_data.size > 0) {
            if (IIS_RX_FULL_COUNT < 2) {
                return;
            }

            /* Discard every other sample since ADC clock is 1/2 LRCK */
            value = IISFIFO_RD;
            IISFIFO_RD;

            *dma_rec_data.p16++ = value;
            dma_rec_data.size -= 4;

            /* TODO: Figure out how to do IIS loopback */
            if (audio_output_source != AUDIO_SRC_PLAYBACK) {
                if (IIS_TX_FREE_COUNT >= 16) {
                    /* Resync the output FIFO - it ran dry */
                    IISFIFO_WR = 0;
                    IISFIFO_WR = 0;
                }
                IISFIFO_WR = value;
                IISFIFO_WR = value;
            }
        }
    }
    else {
        /* RX is left channel mono */
        while (dma_rec_data.size > 0) {
            if (IIS_RX_FULL_COUNT < 2) {
                return;
            }

            /* Discard every other sample since ADC clock is 1/2 LRCK */
            value = IISFIFO_RD;
            IISFIFO_RD;

            value = (uint16_t)value | (value << 16);

            *dma_rec_data.p16++ = value;
            dma_rec_data.size -= 4;

            if (audio_output_source != AUDIO_SRC_PLAYBACK) {
                if (IIS_TX_FREE_COUNT >= 16) {
                    /* Resync the output FIFO - it ran dry */
                    IISFIFO_WR = 0;
                    IISFIFO_WR = 0;
                }

                value = *((int32_t *)dma_rec_data.p16 - 1);
                IISFIFO_WR = value;
                IISFIFO_WR = value;
            }
        }
    }

    if (pcm_rec_dma_complete_callback(PCM_DMAST_OK, &dma_rec_data.p_w,
                                      &dma_rec_data.size))
    {
        pcm_rec_dma_status_callback(PCM_DMAST_STARTED);
    }
}

#else /* !(SANSA_C200 || SANSA_E200) */

void fiq_record(void)
{
    while (dma_rec_data.size > 0) {
        if (IIS_RX_FULL_COUNT < 2) {
            return;
        }

#if SAMPLE_SIZE == 16
        *dma_rec_data.p16++ = IISFIFO_RD;
#elif SAMPLE_SIZE == 32
        *dma_rec_data.p32++ = IISFIFO_RDH;
        *dma_rec_data.p32++ = IISFIFO_RDH;
#endif
        dma_rec_data.size -= 4;
    }

    if (pcm_rec_dma_complete_callback(PCM_DMAST_OK, &dma_rec_data.p_w,
                                      &dma_rec_data.size))
    {
        pcm_rec_dma_status_callback(PCM_DMAST_STARTED);
    }
}

#endif /* SANSA_C200 || SANSA_E200 */

void pcm_rec_dma_stop(void)
{
    /* disable interrupt */
    IIS_IRQRX_REG &= ~IIS_IRQRX;

    dma_rec_data.state = 0;
    dma_rec_data.size = 0;
#if NUM_CORES > 1
    dma_rec_data.core = 0x00;
#endif

    /* disable fifo */
    IISCONFIG &= ~IIS_RXFIFOEN;
    IISFIFO_CFG |= IIS_RXCLR;
}

void pcm_rec_dma_start(void *addr, size_t size)
{
    pcm_rec_dma_stop();

    dma_rec_data.addr = (unsigned long)addr;
    dma_rec_data.size = size;
#if NUM_CORES > 1
    /* This will become more important later - and different ! */
    dma_rec_data.core = processor_id(); /* save initiating core */
#endif
    /* setup FIQ handler */
    FIQ_TARGET_SET(CPU, fiq_record);

    /* interrupt on full fifo, enable record fifo interrupt */
    dma_rec_data.state = 1;

    /* enable RX FIFO */
    IISCONFIG |= IIS_RXFIFOEN;

    /* enable IIS interrupt as FIQ */
    CPU_INT_PRIORITY |= IIS_MASK;
    CPU_INT_EN = IIS_MASK;
}

void pcm_rec_dma_close(void)
{
    pcm_rec_dma_stop();
} /* pcm_close_recording */

void pcm_rec_dma_init(void)
{
    pcm_rec_dma_stop();
} /* pcm_init */

const void * pcm_rec_dma_get_peak_buffer(void)
{
    return (void *)((unsigned long)dma_rec_data.addr & ~3);
} /* pcm_rec_dma_get_peak_buffer */

#endif /* HAVE_RECORDING */
