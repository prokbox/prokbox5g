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

/* Host test of #324: no load or store reaches the BCM2722's host port while
 * GPO32 bit 14 (its power) is clear, and the port lock's waits are bounded.
 *
 * Runs the REAL accessors, extracted by run.sh (no copy kept here) from
 * lcd-video.c, bcm-devtest-hal.c and e2-port.c, against a model:
 *  - the port (0x30000000-0x3007FFFF) is a host page: readable and
 *    writable while "powered" (all bits set: every handshake ready), and
 *    PROT_NONE while unpowered, so ANY load or store the code makes there
 *    faults and is reported as a touch (the silicon's bus stall);
 *  - the two DMA engines that can be aimed at the port (channel 3 and
 *    engine 2): a start with the port as destination while unpowered is a
 *    touch too;
 *  - USEC_TIMER one model microsecond per read; current_tick derived from
 *    it (HZ 100); sleep() and yield() advance it.  Past POLL_LIMIT in one
 *    case the run is cut and reported as HANG;
 *  - the port lock: a model mutex another thread may hold; the old
 *    lcd_port_lock() (mutex_lock) on a held lock is a HANG.
 *
 * Every accessor, unpowered, must: touch nothing, return its own failure
 * value, take about no time (<= 50 model us), count a refusal, and name
 * itself as the refusal's source (the log line's "from <fn>": the entry
 * point, not a helper below it).  Powered, each must reach the port (a
 * register it writes changes), so the extraction runs the real paths.
 *
 * Output: one line per case and a final "VERDICT: PASS" or
 * "VERDICT: FAIL <case>: <why>" (run.sh requires the verdict line). */
#include <setjmp.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

/* ---- build shape (run.sh: -DHAVE_BCMFW_L1 for the release's) ---- */
#define HAVE_IPOD_UI
#define HAVE_LCD_SLEEP
#define BCM_WAIT_BOUNDS
#define BCM_PORT_GATE
#define NUM_CORES 1
#define HZ 100
#define LCD_WIDTH  320
#define LCD_HEIGHT 240
#define XCORE_BSS
#define XCORE_DATA
#define IBSS_ATTR
#define ICODE_ATTR
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define DEVSTATS_RD_TICK() do { } while (0)
#define VPORT_CHECK() do { } while (0)
typedef uint16_t fb_data;

/* ---- the model: time ---- */
#define POLL_LIMIT 50000000ul   /* 500 s of model time in one case: a hang */
static unsigned long sim_us;
static sigjmp_buf case_jb;
static unsigned long model_us(void)
{
    if (++sim_us > POLL_LIMIT)
        siglongjmp(case_jb, 2);
    return sim_us;
}
#define USEC_TIMER (model_us())
#define current_tick ((long)(sim_us / 10000))
#define TIME_AFTER(a, b)  ((long)(b) - (long)(a) < 0)
#define TIME_BEFORE(a, b) TIME_AFTER(b, a)
#define sleep(t) model_sleep(t)
static void model_sleep(int ticks) { sim_us += (unsigned long)ticks * 10000; model_us(); }
static void yield(void) { sim_us += 100; model_us(); }
static int disable_irq_save(void) { return 0; }
static void restore_irq(int l) { (void)l; }
#define IRQ_FIQ_STATUS 0
static int disable_interrupt_save(int m) { (void)m; return 0; }
static void restore_interrupt(int l) { (void)l; }
static bool irq_enabled(void) { return true; }
static void commit_dcache(void) { }

/* ---- the model: power and the port ---- */
static volatile uint32_t gpo32;
#define GPO32_VAL gpo32
#include "bcm-port-gate.h"      /* the real gate */

#define PORT_SIZE 0x80000
static unsigned char *port;
static int powered;
static volatile unsigned long touches;
static volatile void *touch_addr;
/* the 32-bit address registers sit 8 bytes into their 64 KB window, so
 * an address written never hides the 16-bit status word the code polls
 * there (the silicon keeps them apart; this page cannot) */
#define PORT_HW(o) (*(volatile uint16_t *)(port + (o)))
#define PORT_W(o)  (*(volatile uint32_t *)(port + (o)))
#define BCM_DATA          PORT_HW(0x00000)
#define BCM_DATA32        PORT_W(0x00000)
#define BCM_WR_ADDR       PORT_HW(0x10000)
#define BCM_WR_ADDR32     PORT_W(0x10008)
#define BCM_RD_ADDR       PORT_HW(0x20000)
#define BCM_RD_ADDR32     PORT_W(0x20008)
#define BCM_CONTROL       PORT_HW(0x30000)
#define BCM_ALT_DATA      PORT_HW(0x40000)
#define BCM_ALT_DATA32    PORT_W(0x40000)
#define BCM_ALT_WR_ADDR   PORT_HW(0x50000)
#define BCM_ALT_WR_ADDR32 PORT_W(0x50008)
#define BCM_ALT_RD_ADDR   PORT_HW(0x60000)
#define BCM_ALT_RD_ADDR32 PORT_W(0x60008)
#define BCM_ALT_CONTROL   PORT_HW(0x70000)
/* e2-port.c's names for the same registers */
#define PORT_DATA     0x30000000u
#define PORT_ADDR_LO  PORT_HW(0x10000)
#define PORT_ADDR_HI  PORT_HW(0x10002)
#define PORT_CONTROL  PORT_HW(0x30000)
#define PORT_WR_READY 0x2
#define PORT_US       20000

/* The raw-read guard (#324, b12: vcdark froze in a DATA read with no read
 * address pending, the chip powered).  While on, the DATA and ALT_DATA
 * windows fault, and each bank's 32-bit read-address cell holds a
 * sentinel.  A DATA access with its bank's cell still the sentinel (no
 * read address written in this call) is a raw read; otherwise the window
 * opens and the access completes.  Only for accessors that never write
 * DATA (a push is a write, not a raw read). */
#define RD_SENTINEL 0xDEADBEEFu
static int guard_on;
static unsigned long raw_reads;
#define WIN 0x10000
static int in_win(const unsigned char *a, unsigned long off)
{
    return a >= port + off && a < port + off + WIN;
}
static void guard_set(int on)
{
    int prot = on ? PROT_NONE : PROT_READ | PROT_WRITE;

    if (on)
    {
        PORT_W(0x20008) = RD_SENTINEL;      /* BCM_RD_ADDR32 */
        PORT_W(0x60008) = RD_SENTINEL;      /* BCM_ALT_RD_ADDR32 */
    }
    guard_on = on;
    mprotect(port + 0x00000, WIN, prot);    /* DATA */
    mprotect(port + 0x40000, WIN, prot);    /* ALT_DATA */
    mprotect(port + 0x70000, WIN, prot);    /* ALT_CONTROL: unproven raw */
}

static void on_fault(int sig, siginfo_t *si, void *uc)
{
    const unsigned char *a = si->si_addr;

    (void)uc;
    if (guard_on && powered &&
        ((in_win(a, 0x00000) && PORT_W(0x20008) != RD_SENTINEL) ||
         (in_win(a, 0x40000) && PORT_W(0x60008) != RD_SENTINEL)))
    {
        guard_set(0);           /* an address was written: the read goes */
        return;
    }
    if (guard_on && powered && (in_win(a, 0x00000) || in_win(a, 0x40000) ||
                                in_win(a, 0x70000)))
    {
        raw_reads++;
        guard_set(0);
        siglongjmp(case_jb, 3);
    }
    if ((unsigned char *)si->si_addr >= port &&
        (unsigned char *)si->si_addr < port + PORT_SIZE)
    {
        touches++;
        touch_addr = si->si_addr;
        siglongjmp(case_jb, 1);
    }
    signal(sig, SIG_DFL);       /* a real crash: let it happen */
}

static void set_power(int on)
{
    powered = on;
    if (on)
    {
        gpo32 |= BCM_POWER_BIT;
        mprotect(port, PORT_SIZE, PROT_READ | PROT_WRITE);
        memset(port, 0xFF, PORT_SIZE);      /* every handshake ready */
    }
    else
    {
        gpo32 &= ~BCM_POWER_BIT;
        mprotect(port, PORT_SIZE, PROT_NONE);
    }
}

/* the streaming writers (lcd-as-video.S, the alt lab's inline asm): each
 * stores into the port as the real loops do */
static unsigned long fb_writes;
static void lcd_write_data(const fb_data *p, int count)
{
    fb_writes++;
    while (count > 0) { BCM_DATA32 = p[0] | (uint32_t)p[1] << 16; p += 2; count -= 2; }
}
static void lcd_write_data_burst8(const fb_data *p, int count) { lcd_write_data(p, count); }
#define lcd_write_fb lcd_write_data
static void lcd_write_yuv420_lines(unsigned char const * const src[3],
                                   unsigned bcmaddr, int width, int stride)
{
    (void)src; (void)stride;
    BCM_WR_ADDR32 = bcmaddr;
    for (; width > 0; width -= 2)
        BCM_DATA32 = 0;
}
static void alt_stream(unsigned long a, const uint16_t *src, uint32_t bytes)
{
    (void)src; (void)bytes;
    PORT_W(a - 0x30000000u) = 0;
}

/* ---- the model: DMA channel 3 and engine 2 ---- */
static unsigned long DMA_MASTER_CONTROL, DMA3_PER_ADDR, DMA3_FLAGS, DMA3_INCR,
                     DMA3_RAM_ADDR, DMA3_CMD, DMA3_STATUS;
#define DMA_MASTER_CONTROL_EN 0x80000000ul
#define DMA_CMD_START         0x80000000ul
#define DMA_CMD_RAM_TO_PER    0x08000000ul
#define DMA_CMD_SINGLE        0x04000000ul
#define DMA_STATUS_BUSY       0x80000000ul
#define DMA_FLAGS_UNK26       0x04000000ul
#define DMA_INCR_RANGE_FIXED  0x00008000ul
#define DMA_INCR_WIDTH_32BIT  0x00020000ul
static uint32_t e2_master, e2_cmd, e2_status, e2_src, e2_src_cfg, e2_dst,
                e2_dst_cfg;
#define E2_MASTER   e2_master
#define E2_CMD      e2_cmd
#define E2_STATUS   e2_status
#define E2_SRC      e2_src
#define E2_SRC_CFG  e2_src_cfg
#define E2_DST      e2_dst
#define E2_DST_CFG  e2_dst_cfg
#define E2_START    (1u << 31)
#define E2_INTR     (1u << 30)
#define E2_BUSY     (1u << 31)
#define E2_CFG_MASK (7u << 24)
#define UNCACHED_BASE_ADDR 0x10000000u
#define UNCACHED_ADDR(a)   ((a) | UNCACHED_BASE_ADDR)
enum e2port_owner { E2PORT_FREE = 0, E2PORT_LCD, E2PORT_VIDEO };
#define E2PORT_PIECE_MAX  0x10000
#define E2PORT_TIMEOUT_US 100000
static volatile enum e2port_owner owner;
#define OWNER_LOCK()   do { } while (0)
#define OWNER_UNLOCK() do { } while (0)

/* an engine started at the port while unpowered moves words into it */
static int engine_touch(void)
{
    int t = 0;

    if ((DMA3_CMD & DMA_CMD_START) && DMA3_PER_ADDR == (unsigned long)&BCM_DATA32)
        t = 1;
    if ((e2_cmd & E2_START) && e2_dst == PORT_DATA)
        t = 1;
    DMA3_CMD = 0;
    e2_cmd = 0;
    return t;
}

/* ---- the model: the port lock ---- */
struct mutex { int owner, rec; };
#define SELF  1
#define OTHER 2
static struct mutex lcd_port_mtx;
static bool lcd_port_ready = true;
static bool mutex_trylock(struct mutex *m)
{
    if (m->owner == SELF) { m->rec++; return true; }
    if (m->owner) return false;
    m->owner = SELF;
    return true;
}
static void mutex_lock(struct mutex *m)
{
    if (m->owner == OTHER)
        siglongjmp(case_jb, 2);     /* blocks for ever */
    mutex_trylock(m);
}
static void mutex_unlock(struct mutex *m)
{
    if (m->rec) m->rec--; else m->owner = 0;
}
static bool lcd_port_lock(void)     /* lcd-video.c's, the unbounded one */
{
    if (!lcd_port_ready || !irq_enabled())
        return false;
    mutex_lock(&lcd_port_mtx);
    return true;
}
static void lcd_port_unlock(bool locked)
{
    if (locked)
        mutex_unlock(&lcd_port_mtx);
}

/* ---- lcd-video.c's surroundings ---- */
#define BCMA_COMMAND   0x1F8
#define BCMA_CMDPARAM  0xE0000
#define BCM_CMD(x) ((uint32_t)((~((uint32_t)(x)) << 16) | ((uint32_t)(x))))
#define BCMCMD_LCD_UPDATE BCM_CMD(0)
#define BCM_UPDATE_TIMEOUT (HZ/20)
#define BCM_PORT_US 20000
#define LCD_PORT_LOCK_T (2 * HZ)    /* run.sh pins the real line */
enum lcd_status { LCD_IDLE, LCD_INITIAL, LCD_NEED_UPDATE, LCD_UPDATING };
static struct
{
    long update_timeout;
    enum lcd_status state;
    bool blocked;
    bool display_on;
    bool waking;
    int initwakeup;
} lcd_state;
static void semaphore_release(int *s) { (void)s; }
enum
{
    BW_AWAKE = 0, BW_SLEEP, BW_POWERON, BW_COMMAND, BW_TICK, BW_TICK_JOBS,
    BW_TICK_VMCS, BW_LOCK_UPDATE, BW_LOCK_AWAKE, BW_LOCK_VIDEO, BW_N
};
static unsigned bw_faults;
static void bcm_wait_fault(int k, long ticks) { (void)ticks; bw_faults |= 1u << k; }
static unsigned long posts;
#define HAVE_BCM_JOBS
#define SYS_BCMJOB_REQ 1
static void button_queue_post(long id, intptr_t data) { (void)id; (void)data; posts++; }
static volatile bool bcm_port_dead;
static const void *bcmfw_active;
static volatile bool bcmfw_hung;
static fb_data fb[LCD_WIDTH * LCD_HEIGHT];
static bool bcmjob_owned_get(int *x, int *y, int *w, int *h)
{
    (void)x; (void)y; (void)w; (void)h;
    return false;
}
#define FBADDR(x, y) (&fb[(y) * LCD_WIDTH + (x)])

/* ---- bcm-devtest-hal.c's surroundings ---- */
#define DT_PORT_US   20000
#define DT_DMA_PIECE 0x8000
#define DT_DMA_US    100000
#define VDMA_MIN     256
#define ALT_LAB_AREA    0x12C000
#define ALT_LAB_HW      256
#define ALT_LAB_FRAME   0xE0000
#define ALT_LAB_FRAME_B (LCD_WIDTH * LCD_HEIGHT * 2)
enum { BCMDT_UP_STOCK, BCMDT_UP_BURST8, BCMDT_UP_PPDMA, BCMDT_UP_N };
#define BCMDT_PA_ADDR16  0x01
#define BCMDT_PA_DATA16  0x02
#define BCMDT_PA_NOPOLL  0x04
#define BCM_WRV_ADDR16   0x1
#define BCM_WRV_DATA16   0x2
#define BCM_WRV_NOPOLL   0x4
struct bcm_alt_result
{
    uint32_t main_ok[4], alt_ok[4];
    uint32_t frame_us[2];
    bool alt_ready;
    bool dead;
    int fail_step;
};
struct bcm_video_port_load
{
    uint32_t bytes, cpu_us, cpu_blocks, dma_us, dma_blocks, dma_fail;
};
static struct bcm_video_port_load vload;
static uint32_t dt_port[8];
static uint32_t dt_half_writes;
static bool vdma_on, vdma_busy, vdma_broken, vdma_e2, vdma_held;
static uint32_t vdma_addr, vdma_w0, vdma_t0;
static int vdma_wait(void) { return 0; }
static const uint16_t *dt_fb(void) { return fb; }

#ifdef BCMGATE_OLD
/* the tree before the gate has no record: the checks below go red */
static const char *volatile bpg_fn;
static volatile uint32_t bpg_n;
static void bcm_port_powered_up(void) { }
void bcm_port_refuse(const char *fn) { (void)fn; }
uint32_t bcm_port_refused_count(void) { return 0; }
static int lcd_port_lock_bounded(int k) { (void)k; return lcd_port_lock() ? 1 : 0; }
#endif

/* forward declarations: the parts come in file order */
static bool bcm_port_wait(volatile unsigned short *reg, unsigned mask, bool set);
static bool dt_until(volatile unsigned short *reg, unsigned mask, bool set);
static bool dt_wait(volatile unsigned short *reg, unsigned mask,
                    unsigned kind, uint32_t addr);
static int dt_rd(uint32_t addr, uint32_t *val);
static int dt_wr(uint32_t addr, uint32_t val);
static int dt_wr_block(uint32_t addr, const uint16_t *src, uint32_t n);
static int vwr_block(uint32_t addr, const uint16_t *src, uint32_t n);
static void continue_lcd_awake(void);
static void lcd_unblock_and_update(void);
static int lcd_port_lock_tmo(long t);
bool e2port_claim(enum e2port_owner who);
void e2port_release(enum e2port_owner who);
bool e2port_fits(const void *src, uint32_t bytes);
bool e2port_port_ready(void);
int e2port_start(uint32_t bcm_addr, const void *src, uint32_t bytes);
bool e2port_busy(void);
void e2port_stop(void);
void lcd_update_rect(int x, int y, int width, int height);
void lcd_blit_yuv(unsigned char * const src[3], int src_x, int src_y,
                  int stride, int x, int y, int width, int height);
void bcm_video_port_raw(uint16_t out[10]);
int bcm_video_alt_lab(struct bcm_alt_result *r);
int bcm_video_rd(uint32_t addr, uint32_t *val);
int bcm_video_wr(uint32_t addr, uint32_t val);
int bcm_video_wr_block(uint32_t addr, const uint16_t *src, uint32_t n);
int bcm_video_wr_variant(uint32_t addr, const uint16_t *src, uint32_t n,
                         unsigned how);

#include "bcmgate_parts.c"

/* ---- the cases ---- */
static uint16_t src16[256] __attribute__((aligned(4)));
static unsigned char yuv_plane[3][64];
static int rc;          /* the accessor's answer */
static uint16_t raw[10];
static int rc_want;     /* its failure value */

/* each body runs one accessor; rc gets its answer.  ok_off: the failure
 * value; on_reg: the port register the powered call must have written
 * (offset; -1: check rc only) and its value */
struct gcase
{
    const char *name;           /* the accessor, as the gate names it */
    void (*body)(void);
    int off_rc;                 /* the failure value */
    long on_reg;                /* powered: this register written ... */
    uint32_t on_val;            /* ... with this */
};

static void b_bcm_port_wait(void) { rc = bcm_port_wait(&BCM_CONTROL, 0x2, true); }
static void b_bcm_write_addr(void) { bcm_write_addr(0x1234); rc = -1; }
static void b_bcm_write32(void) { bcm_write32(0x1F8, 0xA5A5); rc = -1; }
static void b_bcm_read32(void) { rc = (int)bcm_read32(0x1F8); }
static void b_lcd_tick(void)
{
    lcd_state.blocked = false;
    lcd_state.state = LCD_NEED_UPDATE;
    lcd_state.update_timeout = current_tick - 1;    /* stalled: re-kick */
    lcd_tick();
    rc = -1;
}
static void b_lcd_unblock(void)
{
    lcd_state.state = LCD_INITIAL;     /* kicks whatever it reads */
    lcd_unblock_and_update();
    rc = -1;
}
static void b_lcd_update_rect(void)
{
    lcd_state.display_on = true;
    lcd_update_rect(0, 0, 64, 2);
    rc = -1;
}
static void b_lcd_blit_yuv(void)
{
    unsigned char *src[3] = { yuv_plane[0], yuv_plane[1], yuv_plane[2] };

    lcd_state.display_on = true;
    lcd_blit_yuv(src, 0, 0, 8, 0, 0, 8, 2);
    rc = -1;
}
static void b_bcm_command(void) { bcm_command(BCM_CMD(8)); rc = -1; }
static void b_dt_until(void) { rc = dt_until(&BCM_CONTROL, 0x2, true); }
static void b_dt_wait(void) { rc = dt_wait(&BCM_CONTROL, 0x2, 2, 0x100); }
static void b_dt_rd(void) { uint32_t v; rc = dt_rd(0x100, &v); }
static void b_dt_wr(void) { rc = dt_wr(0x100, 0x11223344); }
static void b_bpb_addr(void) { rc = bpb_addr(0x200); }
static void b_bpb_words(void) { bpb_words(src16, 8); rc = -1; }
static void b_bpb_word(void) { bpb_word(0x55667788); rc = -1; }
static void b_bpb_rd(void) { uint32_t v; rc = bpb_rd(0x100, &v); }
static void b_bpb_wr(void) { rc = bpb_wr(0x100, 0x99AABBCC); }
static void b_dt_wr_block(void) { rc = dt_wr_block(0x300, src16, 9); }
static void b_dt_ppdma(void) { rc = dt_ppdma(src16, 64); }
static void b_dt_fb_upload(void) { rc = dt_fb_upload(0x400, 0, 64, BCMDT_UP_STOCK); }
static void b_dt_upload_fb(void) { rc = dt_upload_fb(); }
static void b_dt_doorbell(void) { dt_doorbell(); rc = -1; }
static void b_dt_pa_addr(void) { rc = dt_pa_addr(0x500, BCMDT_PA_NOPOLL); }
static void b_dt_pa_cpu(void) { rc = dt_pa_cpu(0x500, src16, 8, BCMDT_PA_NOPOLL); }
static void b_port_raw(void)
{
    int i;

    memset(raw, 0, sizeof(raw));
    if (powered)
        PORT_HW(0x30000) = 0x1234;  /* a powered read sees CONTROL */
    bcm_video_port_raw(raw);
    rc = 0;
    for (i = 0; i < 10; i++)
        if (raw[i] != 0xFFFF)
            rc = 1;             /* unpowered: every word 0xFFFF */
    if (rc == 0)
        rc = -1;
}
static void b_alt_lab(void) { struct bcm_alt_result r; rc = bcm_video_alt_lab(&r); }
static void b_video_rd(void) { uint32_t v; rc = bcm_video_rd(0x100, &v); }
static void b_video_wr(void) { rc = bcm_video_wr(0x100, 0x13579BDF); }
static void b_video_wr_block(void) { rc = bcm_video_wr_block(0x600, src16, 8); }
static void b_vwr_block_dma(void)
{
    vdma_on = true;             /* channel 3: the DMA start itself */
    vdma_e2 = false;
    vdma_broken = false;
    rc = vwr_block(0x700, src16, 256);     /* 512 bytes: the DMA's */
    vdma_on = false;
    vdma_busy = false;
}
static void b_wr_variant(void)
{
    rc = bcm_video_wr_variant(0x800, src16, 8, BCM_WRV_NOPOLL);
}
static void b_port_ready(void) { rc = e2port_port_ready(); }
static void b_e2port_start(void) { rc = e2port_start(0x900, src16, 64); }

static const struct gcase cases[] =
{
    { "bcm_port_wait",       b_bcm_port_wait,   0,      -1,      0 },
    { "bcm_write_addr",      b_bcm_write_addr,  -1,     0x10008, 0x1234 },
    { "bcm_write32",         b_bcm_write32,     -1,     0x00000, 0xA5A5 },
    { "bcm_read32",          b_bcm_read32,      0xFFFF, 0x20008, 0x1F8 },
    { "bcm_read32",          b_lcd_tick,        -1,     0x10008, 0x1F8 },
    { "bcm_read32",          b_lcd_unblock,     -1,     0x10008, 0x1F8 },
    { "lcd_update_rect",     b_lcd_update_rect, -1,     0x10008, 0x1F8 },
    { "lcd_blit_yuv",        b_lcd_blit_yuv,    -1,     0x10008, 0x1F8 },
    { "bcm_command",         b_bcm_command,     -1,     0x00000, 0xFFF70008 },
    { "dt_until",            b_dt_until,        0,      -1,      0 },
    { "dt_wait",             b_dt_wait,         0,      -1,      0 },
    { "dt_rd",               b_dt_rd,           -1,     0x20008, 0x100 },
    { "dt_wr",               b_dt_wr,           -1,     0x00000, 0x11223344 },
    { "bpb_addr",            b_bpb_addr,        -1,     0x10008, 0x200 },
    { "bpb_words",           b_bpb_words,       -1,     0x00000, 0x00070006 },
    { "bpb_word",            b_bpb_word,        -1,     0x00000, 0x55667788 },
    { "dt_rd",               b_bpb_rd,          -1,     0x20008, 0x100 },
    { "dt_wr",               b_bpb_wr,          -1,     0x00000, 0x99AABBCC },
    { "dt_wr_block",         b_dt_wr_block,     -1,     -1,      0 },
    { "dt_ppdma",            b_dt_ppdma,        -1,     -1,      0 },
    { "dt_fb_upload",        b_dt_fb_upload,    -1,     0x10008, 0x400 },
    { "dt_upload_fb",        b_dt_upload_fb,    -1,     0x10008, BCMA_CMDPARAM },
    { "dt_doorbell",         b_dt_doorbell,     -1,     0x30000, 0xFFFF0031 },
    { "dt_pa_addr",          b_dt_pa_addr,      -1,     0x10008, 0x500 },
    { "dt_pa_addr",          b_dt_pa_cpu,       -1,     0x10008, 0x500 },
    { "bcm_video_port_raw",  b_port_raw,        -1,     -1,      0 },
    { "bcm_video_alt_lab",   b_alt_lab,         -1,     0x10008, 0xE0000 },
    { "dt_rd",               b_video_rd,        -1,     0x20008, 0x100 },
    { "dt_wr",               b_video_wr,        -1,     0x00000, 0x13579BDF },
    { "vwr_block",           b_video_wr_block,  -1,     0x10008, 0x600 },
    { "vwr_block",           b_vwr_block_dma,   -1,     0x10008, 0x700 },
    { "bcm_video_wr_variant", b_wr_variant,     -1,     0x10008, 0x800 },
    { "e2port_port_ready",   b_port_ready,      0,      -1,      0 },
    { "e2port_start",        b_e2port_start,    -1,     -1,      0 },
};
#define NCASES (sizeof(cases) / sizeof(cases[0]))

static const char *fail_case, *fail_why;
static char case_buf[96];
static char why_buf[160];

static void failed(const char *c, const char *why)
{
    printf("FAIL %s: %s\n", c, why);
    if (!fail_case)
    {
        snprintf(case_buf, sizeof(case_buf), "%s", c);
        fail_case = case_buf;
        snprintf(why_buf, sizeof(why_buf), "%s", why);
        fail_why = why_buf;
    }
}

/* one accessor, unpowered then powered */
static void run_gate(const struct gcase *c, int idx)
{
    char name[96], why[160];
    unsigned long us0, t0 = touches;
    uint32_t n0;
    volatile int jr;

    snprintf(name, sizeof(name), "%s#%d", c->name, idx);
    /* unpowered */
    lcd_port_mtx.owner = 0;
    lcd_port_mtx.rec = 0;
    bcm_port_dead = false;
    set_power(0);
    bcm_port_powered_up();      /* re-armed: this refusal names its source */
    bpg_fn = NULL;
    n0 = bcm_port_refused_count();
    rc = 12345;
    us0 = sim_us;
    jr = sigsetjmp(case_jb, 1);
    if (jr == 0)
        c->body();
    if (engine_touch())
        touches++;
    set_power(1);               /* readable again for the checks */
    if (jr == 2)
        return failed(name, "HANG unpowered");
    if (touches != t0)
        return failed(name, "touched the port with the chip unpowered");
    if (rc != c->off_rc)
    {
        snprintf(why, sizeof(why), "unpowered: returned %d, not %d", rc,
                 c->off_rc);
        return failed(name, why);
    }
    if (sim_us - us0 > 50)
    {
        snprintf(why, sizeof(why), "unpowered: took %lu model us",
                 sim_us - us0);
        return failed(name, why);
    }
    if (bcm_port_refused_count() == n0)
        return failed(name, "no refusal counted");
    if (!bpg_fn || strcmp(bpg_fn, c->name))
    {
        snprintf(why, sizeof(why), "refusal named %s, not %s",
                 bpg_fn ? bpg_fn : "(none)", c->name);
        return failed(name, why);
    }
    /* powered: the real path reaches the port */
    lcd_port_mtx.owner = 0;
    bcm_port_dead = false;
    lcd_state.update_timeout = current_tick + 1000;
    rc = 12345;
    jr = sigsetjmp(case_jb, 1);
    if (jr == 0)
        c->body();
    engine_touch();
    vdma_busy = vdma_held = false;
    if (jr == 2)
        return failed(name, "HANG powered");
    if (c->on_reg >= 0 && PORT_W(c->on_reg) != c->on_val)
    {
        snprintf(why, sizeof(why), "powered: register +0x%lx is 0x%x, not 0x%x",
                 c->on_reg, (unsigned)PORT_W(c->on_reg), (unsigned)c->on_val);
        return failed(name, why);
    }
    if (c->on_reg < 0 && rc == c->off_rc)
        return failed(name, "powered: still the failure value");
    printf("ok   %-24s unpowered: rc %d, nothing touched, refusal named; powered: reaches the port\n",
           name, c->off_rc);
}

/* powered, the readers under the raw-read guard: none loads DATA before
 * a read address, none reads ALT_CONTROL; bcm_video_port_raw() reads
 * CONTROL only */
struct rcase { const char *name; void (*body)(void); };
static const struct rcase rcases[] =
{
    { "raw:bcm_video_port_raw", b_port_raw },
    { "raw:bcm_read32",         b_bcm_read32 },
    { "raw:dt_rd",              b_dt_rd },
    { "raw:bcm_video_rd",       b_video_rd },
};

static void run_raw(const struct rcase *c)
{
    volatile int jr;
    unsigned long r0 = raw_reads;

    set_power(1);
    lcd_port_mtx.owner = 0;
    bcm_port_dead = false;
    PORT_HW(0x30000) = 0x1234;
    PORT_HW(0x70000) = 0x5678;
    memset(raw, 0, sizeof(raw));
    jr = sigsetjmp(case_jb, 1);
    if (jr == 0)
    {
        guard_set(1);
        c->body();
    }
    guard_set(0);
    if (jr == 2)
        return failed(c->name, "HANG");
    if (jr == 3 || raw_reads != r0)
        return failed(c->name, "read DATA with no read address pending, or ALT_CONTROL raw");
    if (c->body == b_port_raw &&
        (raw[4] != 0x1234 || raw[9] != 0xFFFF || raw[0] != 0xFFFF ||
         raw[5] != 0xFFFF))
        return failed(c->name, "CONTROL not read, or another slot not 0xFFFF");
    printf("ok   %-26s no DATA load without a read address\n", c->name);
}

/* the refusal's log line: the first after each power-up only */
static void run_rate(void)
{
    unsigned long p0, t0 = touches;
    uint32_t v;

    set_power(0);
    bcm_port_powered_up();
    bpg_fn = NULL;
    p0 = posts;
    if (sigsetjmp(case_jb, 1) != 0 || touches != t0)
        return set_power(1), failed("rate", "touched the port with the chip unpowered");
    (void)dt_rd(0, &v);
    (void)dt_rd(0, &v);
    (void)dt_wr(0, 0);
    if (posts - p0 != 1 || !bpg_fn || strcmp(bpg_fn, "dt_rd"))
        return set_power(1), failed("rate", "not one log request for three refusals");
    bcm_port_powered_up();
    (void)dt_wr(0, 0);
    if (posts - p0 != 2 || strcmp(bpg_fn, "dt_wr"))
        return set_power(1), failed("rate", "a power-up did not re-arm the log line");
    set_power(1);
    printf("ok   rate                   one log request per power-up (3 refusals: 1; after a power-up: 1 more)\n");
}

/* the port lock held by another thread: each bounded holder gives up after
 * LCD_PORT_LOCK_T (2 s), records its cause, takes nothing */
struct lcase { const char *name; void (*body)(void); int bw; };
static void l_video_rd(void) { uint32_t v; rc = bcm_video_rd(0x100, &v); }
static void l_video_wr(void) { rc = bcm_video_wr(0x100, 1); }
static void l_video_wr_block(void) { rc = bcm_video_wr_block(0x100, src16, 8); }
static void l_update_rect(void)
{
    lcd_state.display_on = true;
    lcd_update_rect(0, 0, 64, 2);
    rc = -1;
}
static const struct lcase lcases[] =
{
    { "lock:bcm_video_rd",       l_video_rd,       BW_LOCK_VIDEO },
    { "lock:bcm_video_wr",       l_video_wr,       BW_LOCK_VIDEO },
    { "lock:bcm_video_wr_block", l_video_wr_block, BW_LOCK_VIDEO },
    { "lock:lcd_update_rect",    l_update_rect,    BW_LOCK_UPDATE },
};

static void run_lock(const struct lcase *c)
{
    char why[160];
    long t0;
    unsigned long w0;
    volatile int jr;

    set_power(1);
    lcd_port_mtx.owner = OTHER;
    lcd_port_mtx.rec = 0;
    bw_faults = 0;
    w0 = fb_writes;
    PORT_W(0x10000) = 0xFFFFFFFF;
    t0 = current_tick;
    rc = 12345;
    jr = sigsetjmp(case_jb, 1);
    if (jr == 0)
        c->body();
    lcd_port_mtx.owner = 0;
    if (jr == 2)
        return failed(c->name, "HANG: waited for the port lock without a bound");
    if (rc != -1)
        return failed(c->name, "did not fail with the lock never given");
    if (current_tick - t0 < 2 * HZ || current_tick - t0 > 2 * HZ + 2)
    {
        snprintf(why, sizeof(why), "gave up after %ld ticks, not about %d",
                 current_tick - t0, 2 * HZ);
        return failed(c->name, why);
    }
    if (!(bw_faults & (1u << c->bw)))
        return failed(c->name, "the timeout's cause was not recorded");
    if (fb_writes != w0 || PORT_W(0x10000) != 0xFFFFFFFF)
        return failed(c->name, "touched the port without the lock");
    printf("ok   %-26s gave up after %ld ticks, cause recorded, port untouched\n",
           c->name, current_tick - t0);
}

static void on_alarm(int sig)
{
    static const char m[] = "VERDICT: FAIL hang: a loop the model cannot end (real time)\n";

    (void)sig;
    if (write(1, m, sizeof(m) - 1) < 0)
        _exit(3);
    _exit(3);
}

int main(void)
{
    struct sigaction sa;
    unsigned i;

    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGALRM, on_alarm);
    alarm(60);

    port = mmap(NULL, PORT_SIZE, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANON, -1, 0);
    if (port == MAP_FAILED)
    {
        printf("VERDICT: FAIL setup: mmap\n");
        return 1;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    for (i = 0; i < sizeof(src16) / 2; i++)
        src16[i] = (uint16_t)i;
    (void)rc_want;

    /* the model can see a touch at all: a raw read with the chip off */
    set_power(0);
    if (sigsetjmp(case_jb, 1) == 0)
        (void)BCM_CONTROL;
    set_power(1);
    if (touches != 1)
    {
        printf("VERDICT: FAIL model: an unpowered read was not seen\n");
        return 1;
    }
    touches = 0;

    for (i = 0; i < NCASES; i++)
        run_gate(&cases[i], (int)i);
    run_rate();
    for (i = 0; i < sizeof(rcases) / sizeof(rcases[0]); i++)
        run_raw(&rcases[i]);
    for (i = 0; i < sizeof(lcases) / sizeof(lcases[0]); i++)
        run_lock(&lcases[i]);
    printf("cases: %u accessors, 1 rate, %u lock\n", (unsigned)NCASES,
           (unsigned)(sizeof(lcases) / sizeof(lcases[0])));
    if (fail_case)
    {
        printf("VERDICT: FAIL %s: %s\n", fail_case, fail_why);
        return 1;
    }
    printf("VERDICT: PASS\n");
    return 0;
}
