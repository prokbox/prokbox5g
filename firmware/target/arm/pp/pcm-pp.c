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
static struct corelock pcm_lock_cl IBSS_ATTR;
static struct pcm_pp_stats pcmpp IBSS_ATTR;

/* The interrupt controller's per-core registers: the COP's sit 0x10
 * above the CPU's (EN_STAT, EN, DIS, PRIORITY; pp5020.h) */
#define CORE_INT_REG(cpu_reg, core) \
    (*(volatile unsigned long *)((uintptr_t)&(cpu_reg) + ((core) << 4)))

void fiq_by_core(void) ICODE_ATTR __attribute__((naked));
void fiq_by_core(void)
{
    /* FIQ mode banks r8-r14: r10 and r12 need no saving */
    asm volatile (
        "mov    r10, #0x60000000    \n" /* PROCESSOR_ID */
        "ldrb   r12, [r10]          \n"
        "ldr    r10, =fiq_core_target \n"
        "cmp    r12, #0x55          \n" /* the CPU */
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

    /* Wait for not busy + clear int */
    while (DMA0_STATUS & (DMA_STATUS_BUSY | DMA_STATUS_INTR));

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
#endif
    DMA0_STATUS; /* Clear any pending interrupt */

    size_t size = (DMA0_CMD & 0xffff) + 4; /* Get size of trasfer that caused
                                              this interrupt */
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

static void sink_set_freq(uint16_t freq) {
    /* ProkBox #191: whole 32-bit words (DMA0_INCR is 32-bit wide).  The
     * upstream rate >> 6 gave odd lengths (44.1 kHz: 689 bytes), and every
     * chunk after the first then started off a word boundary */
    max_dma_chunk_size = (hw_freq_sampr[freq] >> 6) & ~3;
    audiohw_set_frequency(freq);
}

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
    }
    corelock_unlock(&pcm_lock_cl);
    restore_interrupt(status);
}

static void sink_unlock(void)
{
    int status = disable_interrupt_save(IRQ_FIQ_STATUS);

    corelock_lock(&pcm_lock_cl);
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
    dma_tx_start(true);
}

static void sink_stop_pcm(void)
{
    dma_tx_stop();

    /* Wait for FIFO to empty */
    while (!IIS_TX_IS_EMPTY);

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

    dma_tx_setup();

    dma_play_data.addr = dma_tx_buf_prepare(addr);
    dma_play_data.size = size;
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
