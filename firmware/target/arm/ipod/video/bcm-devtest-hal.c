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
 * BCM2722 host-port layer: the device test's hardware layer
 * (HAVE_BCMFW_DEVTEST) and the boot path's (HAVE_BCMFW, also in a release
 * build without the device test: then only the port, the take and
 * restore, the log, the boot path and the video player's hold; the
 * stages' helpers, labs and probes are #ifdef HAVE_BCMFW_DEVTEST).
 * #included at the end of lcd-video.c, because it needs that file's
 * host-port macros, lcd_state and lcd_tick; nothing here is compiled
 * into any other build.  The stages are in bcm-devtest-core.c.
 *
 * Ownership: bcm_devtest_owned (lcd-video.c) makes lcd_awake() and
 * lcd_sleep() return at once, so the backlight thread cannot touch the
 * BCM while a test runs; display_on = false makes every lcd_update*()
 * a no-op.  Giving it back either re-arms the tick (Apple's vmcs still
 * running) or runs the stock lcd_awake(), which reloads Apple's vmcs from
 * NOR exactly as every backlight wake does.
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
#include <stdio.h>
#include "file.h"
#include "button.h"
#include "backlight.h"
#include "rbpaths.h"
#include "string-extra.h"
#include "bcm-devtest.h"
#include "core_alloc.h"
#if defined(HAVE_LCD_E2) || defined(HAVE_BCMFW_VIDEO)
#include "e2-port.h"            /* stage 5s: the boot upload by engine 2 */
#endif
#ifdef HAVE_BCMFW
#include "ata_idle_notify.h"
#endif

#define DT_LOG    ROCKBOX_DIR "/bcmtest.log"
#define DT_PASS2  ROCKBOX_DIR "/bcmpass2.txt"   /* stage 2 pass flag */
#define DT_PASS3  ROCKBOX_DIR "/bcmpass3.txt"   /* stage 3 pass flag */
#define DT_PASS4  ROCKBOX_DIR "/bcmpass4.txt"   /* stage 4 (timing bench) */
#define DT_PASS5  ROCKBOX_DIR "/bcmpass5.txt"   /* stage 5 (everyday path) */
#define DT_PASS4B ROCKBOX_DIR "/bcmpass4b.txt"  /* stage 4b (150 MHz, dev9) */
#define DT_PASS4C ROCKBOX_DIR "/bcmpass4c.txt"  /* stage 4c (150 MHz panel) */
#define DT_PASS5I ROCKBOX_DIR "/bcmpass5i.txt"  /* stage 5i (idle sleep) */
#define DT_PORT_US 20000    /* a port handshake takes microseconds */

/* Wait for (reg & mask) to become set (or clear); false after 20 ms */
static bool dt_until(volatile unsigned short *reg, unsigned mask, bool set)
{
    unsigned long t0 = USEC_TIMER;

    while (((*reg & mask) != 0) != set)
        if (USEC_TIMER - t0 > DT_PORT_US)
            return false;
    return true;
}

/* The handshakes' record (dev12, bcm_video_port_stats): the longest
 * wait of each kind (read address, read data, write), timeouts, and at
 * the last timeout its kind (1-3), address and the two registers */
static uint32_t dt_port[8];    /* the player's thread writes, the UI's
                                   reads unlocked: diagnostics only */

static bool dt_wait(volatile unsigned short *reg, unsigned mask,
                    unsigned kind, uint32_t addr)
{
    unsigned long t0 = USEC_TIMER, us;
    bool ok = dt_until(reg, mask, true);

    us = USEC_TIMER - t0;
    if (us > dt_port[kind])
        dt_port[kind] = us;
    if (!ok)
    {
        dt_port[3]++;
        dt_port[4] = kind + 1;
        dt_port[5] = addr;
        dt_port[6] = BCM_CONTROL;
        dt_port[7] = BCM_RD_ADDR;
    }
    return ok;
}

static int dt_rd(uint32_t addr, uint32_t *val)
{
    if (!dt_wait(&BCM_RD_ADDR, 1, 0, addr))
        return -1;
    BCM_RD_ADDR32 = addr;
    if (!dt_wait(&BCM_CONTROL, 0x10, 1, addr))
        return -1;
    *val = BCM_DATA32;
    return 0;
}

/* The host port's write address ignores bits 1:0 [silicon, CR1
 * 2026-10-25: a 301-wide slot's odd rows, written from 2 mod 4, landed one
 * pixel early]: a write asked at 2 mod 4 would land 2 bytes low and
 * corrupt the halfword before it, so every writer refuses one (counted in
 * dt_half_writes) instead of writing it.  Callers merge an odd first
 * pixel into its word (bcmjob-core.c write_row). */
static uint32_t dt_half_writes;

static int dt_wr(uint32_t addr, uint32_t val)
{
    if (addr & 3)
    {
        dt_half_writes++;
        return -1;
    }
    BCM_WR_ADDR32 = addr;
    if (!dt_wait(&BCM_CONTROL, 0x2, 2, addr))
        return -1;
    BCM_DATA32 = val;
    return 0;
}

/* bcm_port_block()'s primitives (bcm-port-block.h), on the main bank */
static inline int bpb_addr(uint32_t a)
{
    BCM_WR_ADDR32 = a;
    return dt_wait(&BCM_CONTROL, 0x2, 2, a) ? 0 : -1;
}

static inline void bpb_words(const uint16_t *s, uint32_t n)
{
    lcd_write_data((const fb_data *)s, n);      /* n even, s aligned */
}

static inline void bpb_word(uint32_t v)
{
    BCM_DATA32 = v;
}

static inline int bpb_rd(uint32_t a, uint32_t *v)
{
    return dt_rd(a, v);
}

static inline int bpb_wr(uint32_t a, uint32_t v)
{
    return dt_wr(a, v);
}

#include "bcm-port-block.h"

/* n halfwords to a word address: any count, any halfword-aligned source
 * (an odd last halfword by a read-modify-write of its word: the port
 * loses a lone trailing halfword [silicon, ALGNP PLH1/3/5]) */
static int dt_wr_block(uint32_t addr, const uint16_t *src, uint32_t n)
{
    if (addr & 3)
    {
        dt_half_writes++;
        return -1;
    }
    return bcm_port_block(addr, src, n);
}

/* dev8, stage 5's upload lab: the framebuffer to the BCM by one method.
 * The PP DMA path: channel 3 (the audio uses 0; 1-3 are free on this
 * target), memory to the fixed data port, 32-bit, no request line (free
 * running), in 32 KB pieces (the count field is 16 bits), each polled to
 * its end.  Returns 1, not a port failure, if a piece never ends: the lab
 * logs that method as failed and goes on.  (The pieces' sizes are the
 * video player's DMA blocks' too.) */
#define DT_DMA_PIECE 0x8000
#define DT_DMA_US    100000
#define VDMA_MIN     256         /* smaller blocks: the CPU (as Apple's osos) */

#ifdef HAVE_BCMFW_DEVTEST
extern void lcd_write_data_burst8(const fb_data *addr, int pixelcount);

static int dt_ppdma(const void *src, uint32_t bytes)
{
    unsigned long a = (unsigned long)src;

    if (a < UNCACHED_BASE_ADDR)
    {
        a = UNCACHED_ADDR(a);
        commit_dcache();
    }
    DMA_MASTER_CONTROL |= DMA_MASTER_CONTROL_EN;
    DMA3_PER_ADDR = (unsigned long)&BCM_DATA32;
    DMA3_FLAGS = DMA_FLAGS_UNK26;
    DMA3_INCR = DMA_INCR_RANGE_FIXED | DMA_INCR_WIDTH_32BIT;
    while (bytes)
    {
        uint32_t n = bytes > DT_DMA_PIECE ? DT_DMA_PIECE : bytes;
        unsigned long t0 = USEC_TIMER;

        DMA3_RAM_ADDR = a;
        DMA3_CMD = DMA_CMD_RAM_TO_PER | DMA_CMD_SINGLE | (n - 4) |
                   DMA_CMD_START;
        while (DMA3_STATUS & DMA_STATUS_BUSY)
            if (USEC_TIMER - t0 > DT_DMA_US)
            {
                DMA3_CMD = 0;
                (void)DMA3_STATUS;
                return 1;
            }
        a += n;
        bytes -= n;
    }
    (void)DMA3_STATUS;
    return 0;
}

static int dt_fb_upload(uint32_t addr, uint32_t off, uint32_t bytes, int how)
{
    const fb_data *src = (const fb_data *)((const char *)FBADDR(0, 0) + off);

    BCM_WR_ADDR32 = addr;
    if (!dt_until(&BCM_CONTROL, 0x2, true))
        return -1;
    switch (how)
    {
    case BCMDT_UP_BURST8:
        lcd_write_data_burst8(src, bytes / 2);
        return 0;
    case BCMDT_UP_PPDMA:
        return dt_ppdma(src, bytes);
    default:
        lcd_write_data(src, bytes / 2);
        return 0;
    }
}

static const uint16_t *dt_fb(void)
{
    return (const uint16_t *)FBADDR(0, 0);
}

static bool dt_boosted;

static void dt_boost(bool on)
{
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    if (on != dt_boosted)
        cpu_boost(on);
#endif
    dt_boosted = on;
}
#endif /* HAVE_BCMFW_DEVTEST */

/* The boot path's first frame: Rockbox's framebuffer, as lcd_update() */
static int dt_upload_fb(void)
{
    BCM_WR_ADDR32 = BCMA_CMDPARAM;
    if (!dt_until(&BCM_CONTROL, 0x2, true))
        return -1;
    lcd_write_data(FBADDR(0, 0), LCD_WIDTH * LCD_HEIGHT);
    return 0;
}

static void dt_doorbell(void)
{
    BCM_CONTROL = 0x31;
}

/* bcm_init()'s bootstrap with every wait bounded: power on, the strap,
 * the handshake; the BCM is then held for its upload */
static int dt_bootstrap(void)
{
    int i;

    GPO32_VAL |= 0x4000;
    sleep(HZ/20);
    STRAP_OPT_A &= ~0xF00;
    outl(0x1313, 0x70000040);

    if (!dt_until(&BCM_ALT_CONTROL, 0x80, false) ||
        !dt_until(&BCM_ALT_CONTROL, 0x40, true))
        return -1;
    for (i = 0; i < 8; i++)
        BCM_CONTROL = bcm_bootstrapdata[i];
    for (i = 3; i < 8; i++)
        BCM_ALT_CONTROL = bcm_bootstrapdata[i];
    if (!dt_until(&BCM_RD_ADDR, 1, true) ||
        !dt_until(&BCM_ALT_RD_ADDR, 1, true))
        return -1;
    (void)BCM_WR_ADDR;
    (void)BCM_ALT_WR_ADDR;
    if (!dt_until(&BCM_ALT_CONTROL, 0x80, false) ||
        !dt_until(&BCM_ALT_CONTROL, 0x40, true))
        return -1;
    return 0;
}

/* After the upload: 0x1F8 = 0, the cache flush, the start write */
static int dt_start(void)
{
    uint32_t v;
    unsigned long t0;

    if (dt_wr(BCMA_COMMAND, 0) || dt_wr(0x10000C00, 0xC0000000))
        return -1;
    t0 = USEC_TIMER;
    for (;;)
    {
        if (dt_rd(0x10000C00, &v) || USEC_TIMER - t0 > 100000)
            return -1;
        if (v & 1)
            break;
        yield();
    }
    if (dt_wr(0x10000C00, 0) || dt_wr(0x10000400, 0xA5A50002))
        return -1;
    return 0;
}

/* bcm_init() with every wait bounded, and the ready wait left to the
 * caller.  Same register writes in the same order. */
static int dt_boot(const uint8_t *img, uint32_t bytes)
{
    if (dt_bootstrap())
        return -1;
    /* the image is padded to a multiple of 4 bytes */
    if (dt_wr_block(0, (const uint16_t *)img, ((bytes + 3) >> 1) & ~1))
        return -1;
    return dt_start();
}

#ifdef HAVE_BCMFW_DEVTEST
/* Stage 5s: bytes (a multiple of 4) to BCM address 0 by engine 2, piece
 * by piece (each sends its address again); the CPU waits for each */
static int dt_e2_upload(const uint8_t *src, uint32_t bytes, uint32_t *pieces)
{
#if defined(HAVE_LCD_E2) || defined(HAVE_BCMFW_VIDEO)
    uint32_t done, n;
    unsigned long t0;

    if (!e2port_claim(E2PORT_VIDEO))
        return -1;
    for (done = 0; done < bytes; done += n)
    {
        n = MIN(bytes - done, E2PORT_PIECE_MAX);
        if (!e2port_fits(src + done, n) ||
            e2port_start(done, src + done, n) < 0)
            goto fail;
        t0 = USEC_TIMER;
        while (e2port_busy())
        {
            if (USEC_TIMER - t0 > E2PORT_TIMEOUT_US)
            {
                e2port_stop();
                goto fail;
            }
            yield();
        }
        if (!e2port_port_ready())
            goto fail;
        (*pieces)++;
    }
    e2port_release(E2PORT_VIDEO);
    return 0;
fail:
    e2port_release(E2PORT_VIDEO);
    return -1;
#else
    (void)src; (void)bytes; (void)pieces;
    return -1;
#endif
}

/* Stage 5s: the bootstrap, the upload by how (timed), every word read
 * back, then the start (struct bcmdt_hal, boot_how).  A failed upload
 * leaves the BCM held; the stage's power-down cuts it. */
static int dt_boot_how(const uint8_t *img, uint32_t bytes, int how,
                       uint32_t out[4])
{
    uint32_t i, v, w;
    unsigned long t0;
    int rc;

    out[0] = out[1] = out[3] = 0;
    out[2] = ~0u;
    bytes = (bytes + 3) & ~3u;
    if (dt_bootstrap())
        return -1;
    t0 = USEC_TIMER;
    rc = how == BCMDT_BOOT_E2 ? dt_e2_upload(img, bytes, &out[3])
                              : dt_wr_block(0, (const uint16_t *)img, bytes >> 1);
    out[0] = USEC_TIMER - t0;
    if (rc)
        return -1;
    for (i = 0; i < bytes; i += 4)
    {
        if (dt_rd(i, &v))
            return -1;
        memcpy(&w, img + i, 4);
        if (v != w && out[1]++ == 0)
            out[2] = i;
    }
    return dt_start();
}
#endif /* HAVE_BCMFW_DEVTEST */

static void dt_power_cut(void)
{
    GPO32_VAL &= ~0x4000;
}

static void dt_light(bool on);
static void dt_log(const char *line);

static void dt_restore(bool running)
{
#ifdef HAVE_BCM_JOBS
    /* P19 M4: HELLO again, a new epoch; outside the port lock, as in
     * dt_take */
    if (!jd_suspend(false))
        dt_log("dt_restore: the job ring's owner did not acknowledge");
#endif
    if (running)
    {
        /* Apple's vmcs never stopped: hand the tick back */
        lcd_state.state = LCD_IDLE;
        lcd_state.display_on = true;
        bcm_devtest_owned = false;
        tick_add_task(&lcd_tick);
    }
    else
    {
        /* BCM is off: the stock wake path (bcm_init from NOR vmcs, first
         * update, tick) - the same code every backlight wake runs.  Still
         * owned while it runs: lcd_awake() from the backlight thread must
         * not start a second bcm_init() meanwhile. */
        lcd_state.update_timeout = current_tick - HZ;
        lcd_awake_body();
        bcm_devtest_owned = false;
    }
    dt_light(true);             /* the stages leave it off around power steps */
}

static void dt_light(bool on)
{
    _backlight_hw_enable(on);   /* lcd_awake() inside is a no-op while owned */
    if (on)
        backlight_on();         /* restart the backlight timeout */
}

static uint32_t dt_now_us(void)
{
    return USEC_TIMER;
}

static void dt_yield(void)
{
    yield();
}

static void dt_sleep_ms(uint32_t ms)
{
    sleep((ms * HZ + 999) / 1000);
}

#ifdef HAVE_BCMFW_DEVTEST
static void dt_hold(uint32_t ms)
{
    long end = current_tick + (long)(ms * HZ / 1000);

    while (TIME_BEFORE(current_tick, end))
    {
        long btn = button_get_w_tmo(HZ/10);

        if (btn & SYS_EVENT)
        {
            button_queue_post(btn, 0);  /* e.g. USB: leave it for the UI */
            break;
        }
        if ((btn & BUTTON_SELECT) && !(btn & (BUTTON_REL | BUTTON_REPEAT)))
            break;
        backlight_on();
    }
}
#endif

/* Once a boot (bcm_boot_early, main thread, the disk just mounted): the
 * last boot's log becomes bcmtest.1.log, so a pull never mixes runs
 * (owner, 2026-10-13); the older ones move up to bcmtest.5.log, the
 * oldest dropped (session 36: six reboots in a session left only the
 * last two boots).  The stages' summaries go to bcmtest-results.log,
 * never rolled here (apps/ipodui/devbcm.c).  Not from the storage
 * thread: its stack is small (apps/ipodui/uilog.c). */
#ifdef HAVE_BCMFW_L1    /* its only caller (bcmfw-boot.c) */
#define DT_LOG_GENS 5
static void dt_log_roll(void)
{
    char from[MAX_PATH], to[MAX_PATH];
    int k;

    if (!file_exists(DT_LOG))
        return;
    snprintf(to, sizeof(to), ROCKBOX_DIR "/bcmtest.%d.log", DT_LOG_GENS);
    remove(to);
    for (k = DT_LOG_GENS - 1; k >= 1; k--)
    {
        snprintf(from, sizeof(from), ROCKBOX_DIR "/bcmtest.%d.log", k);
        if (file_exists(from))
            rename(from, to);
        strlcpy(to, from, sizeof(to));
    }
    rename(DT_LOG, to);     /* to = bcmtest.1.log */
}
#endif

/* What became of the log's lines this boot (session 37: a boot's stage
 * lines never reached the file, and nothing said why): lines asked for,
 * opens that failed (each tried twice), writes or closes that failed,
 * and the last failure's return code */
static uint32_t dt_log_n, dt_log_open_bad, dt_log_write_bad, dt_log_bytes;
static int dt_log_last_rc;

/* out[4] the bytes written this boot and out[5] the file's size now: the
 * two differ if writes were lost without an error (a forced UDMA4 write,
 * session 37's suspect); the roll leaves the file empty at boot */
void bcm_devtest_log_stats(uint32_t out[6])
{
    int fd = open(DT_LOG, O_RDONLY);

    out[0] = dt_log_n;
    out[1] = dt_log_open_bad;
    out[2] = dt_log_write_bad;
    out[3] = (uint32_t)dt_log_last_rc;
    out[4] = dt_log_bytes;
    out[5] = fd < 0 ? 0 : (uint32_t)lseek(fd, 0, SEEK_END);
    if (fd >= 0)
        close(fd);
}

static void dt_log(const char *line)
{
    /* open, write, close: every line is on disk before the next step */
    size_t n = strlen(line);
    int fd = open(DT_LOG, O_WRONLY | O_CREAT | O_APPEND, 0666);

    dt_log_n++;
    if (fd < 0)
    {
        yield();
        fd = open(DT_LOG, O_WRONLY | O_CREAT | O_APPEND, 0666);
    }
    if (fd < 0)
    {
        dt_log_open_bad++;
        dt_log_last_rc = fd;
        return;
    }
    dt_log_bytes += n + 1;
    if (write(fd, line, n) != (ssize_t)n || write(fd, "\n", 1) != 1)
    {
        dt_log_write_bad++;
        dt_log_last_rc = -1000;
    }
    if (close(fd) < 0)
    {
        dt_log_write_bad++;
        dt_log_last_rc = -2000;
    }
}

static int dt_take(void)
{
    long t0;
    bool locked;

#ifdef HAVE_BCM_JOBS
    /* P19 M4 / S0: the ring's owner stops first, BEFORE the port lock
     * (it may be waiting on that lock); no acknowledgement in
     * BJC_ACK_MS: the port is never touched (vplayer: its error screen) */
    if (!jd_suspend(true))
    {
        dt_log("dt_take: refused: the job ring's owner did not ack");
        return -1;
    }
#endif
    /* a UI push by the second DMA engine (HAVE_LCD_E2) finishes first:
       the push holds this lock while its pieces move */
    locked = lcd_port_lock();

    /* Only from the normal state: display on, Apple's vmcs found in NOR */
    if (!lcd_state.display_on || flash_vmcs_length == 0 || bcm_devtest_owned)
    {
        lcd_port_unlock(locked);
        /* which one (bench 2026-10-25: stage 6's take refused, unnamed) */
        dt_log(bcm_devtest_owned ? "dt_take: refused: the device test owns it"
               : !lcd_state.display_on ? "dt_take: refused: display off"
               : "dt_take: refused: no vmcs in NOR");
#ifdef HAVE_BCM_JOBS
        if (!jd_suspend(false))     /* refused: the jobs resume */
            dt_log("dt_take: jobs did not resume after a refusal");
#endif
        return -1;
    }

    /* Owned first, so lcd_awake()/lcd_sleep() step aside; then stop new
     * updates and let the one in flight finish */
#ifdef HAVE_PP_GOV_DVS
    pgov_port_cap(true);        /* off 100 MHz: this owner drives the port
                                 * without lcd_awake() (review S4) */
#endif
    bcm_devtest_owned = true;
    lcd_state.display_on = false;
    t0 = current_tick;
    while (lcd_state.state != LCD_INITIAL && lcd_state.state != LCD_IDLE &&
           TIME_BEFORE(current_tick, t0 + HZ))
        yield();
    tick_remove_task(&lcd_tick);
    lcd_port_unlock(locked);
    return 0;
}

#ifdef HAVE_BCMFW_DEVTEST
/* dev9: the developer power log's phase mark, set by the shell (the
 * firmware layer can't reach apps/); NULL: no marks */
static void (*dt_mark_fn)(const char *tag);

void bcm_devtest_set_mark(void (*fn)(const char *tag))
{
    dt_mark_fn = fn;
}

static void dt_mark(const char *tag)
{
    if (dt_mark_fn)
        dt_mark_fn(tag);
}

/* Stage 5b's covers: buflib memory for the stage only (the playback
 * buffer gives it up; audio is stopped).  scratch(0) frees it. */
static int dt_scratch_h;

static void *dt_scratch(uint32_t bytes)
{
    if (!bytes)
    {
        if (dt_scratch_h > 0)
            dt_scratch_h = core_free(dt_scratch_h);
        return NULL;
    }
    if (dt_scratch_h <= 0)
        dt_scratch_h = core_alloc(bytes);
    if (dt_scratch_h <= 0)
        return NULL;
    core_pin(dt_scratch_h);
    return core_get_data(dt_scratch_h);
}
#endif /* HAVE_BCMFW_DEVTEST */

/* The stages' port calls, each one piece under the port lock (jobs.md
 * section 6): after dt_take() the stages own the BCM, and the lock keeps
 * any other writer from landing between an address and its data.  The
 * lock is recursive; under an interrupt or before the driver's init it is
 * skipped (lcd_port_lock() says false), as for the UI's own pushes. */
static int dt_rd_l(uint32_t addr, uint32_t *val)
{
    bool locked = lcd_port_lock();
    int rc = dt_rd(addr, val);

    lcd_port_unlock(locked);
    return rc;
}

static int dt_wr_l(uint32_t addr, uint32_t val)
{
    bool locked = lcd_port_lock();
    int rc = dt_wr(addr, val);

    lcd_port_unlock(locked);
    return rc;
}

static int dt_wr_block_l(uint32_t addr, const uint16_t *src, uint32_t n)
{
    bool locked = lcd_port_lock();
    int rc = dt_wr_block(addr, src, n);

    lcd_port_unlock(locked);
    return rc;
}

#ifdef HAVE_BCMFW_DEVTEST
static int dt_fb_upload_l(uint32_t addr, uint32_t off, uint32_t bytes,
                          int how)
{
    bool locked = lcd_port_lock();
    int rc = dt_fb_upload(addr, off, bytes, how);

    lcd_port_unlock(locked);
    return rc;
}
#endif

#ifdef HAVE_BCMFW_DEVTEST
/* Bench row ALGN's port checks (bcm-devtest.h BCMDT_PA_*): one write in
 * the asked form.  Bounded like every other port call; the lock held for
 * the whole write so nothing else lands between address and data. */
static int dt_pa_addr(uint32_t addr, unsigned how)
{
    if (how & BCMDT_PA_ADDR16)
    {
        BCM_WR_ADDR = addr & 0xFFFF;
        BCM_WR_ADDR = addr >> 16;
    }
    else
        BCM_WR_ADDR32 = addr;
    return (how & BCMDT_PA_NOPOLL) || dt_wait(&BCM_CONTROL, 0x2, 2, addr) ? 0 : -1;
}

static int dt_pa_cpu(uint32_t addr, const uint16_t *src, uint32_t n,
                     unsigned how)
{
    uint32_t i = 0;

    if (n == 0)
        return 0;
    if (dt_pa_addr(addr, how))
        return -1;
    if (how & BCMDT_PA_DATA16)
        for (; i < n; i++)
            BCM_DATA = src[i];
    else
    {
        for (; i + 1 < n; i += 2)
            BCM_DATA32 = src[i] | (uint32_t)src[i + 1] << 16;
        if (i < n)
            BCM_DATA32 = src[i];        /* ours: a word, high half 0 */
    }
    return 0;
}

static int dt_port_algn(uint32_t addr, const uint16_t *src, uint32_t n,
                        unsigned how)
{
    bool locked = lcd_port_lock();
    int rc = 0;

    if (!(how & BCMDT_PA_E2))
        rc = dt_pa_cpu(addr, src, n, how);
    else
    {
#if defined(HAVE_LCD_E2) || defined(HAVE_BCMFW_VIDEO)
        uint32_t bytes = (2 * n) & ~3u;
        unsigned long t0;

        if (!e2port_claim(E2PORT_VIDEO))
            rc = -1;
        else
        {
            if (!e2port_fits(src, bytes) || e2port_start(addr, src, bytes) < 0)
                rc = -1;
            t0 = USEC_TIMER;
            while (rc == 0 && e2port_busy())
                if (USEC_TIMER - t0 > E2PORT_TIMEOUT_US)
                {
                    e2port_stop();
                    rc = -1;
                }
            if (rc == 0 && !(how & BCMDT_PA_NODRAIN) && !e2port_port_ready())
                rc = -1;
            e2port_release(E2PORT_VIDEO);
            if (rc == 0)
                rc = dt_pa_cpu(addr + bytes, src + bytes / 2, n - bytes / 2,
                               how);
        }
#else
        rc = -1;
#endif
    }
    lcd_port_unlock(locked);
    return rc;
}
#endif

static int dt_upload_fb_l(void)
{
    bool locked = lcd_port_lock();
    int rc = dt_upload_fb();

    lcd_port_unlock(locked);
    return rc;
}

static const struct bcmdt_hal dt_hal =
{
    .rd = dt_rd_l, .wr = dt_wr_l, .wr_block = dt_wr_block_l,
    .doorbell = dt_doorbell, .boot = dt_boot, .power_cut = dt_power_cut,
    .take = dt_take, .restore = dt_restore, .light = dt_light,
    .now_us = dt_now_us, .yield = dt_yield, .sleep_ms = dt_sleep_ms,
    .log = dt_log, .upload_fb = dt_upload_fb_l,
    .hello_dma = &bcmdt_hello_dma_image,
#ifdef HAVE_BCMFW_DEVTEST
    /* the stages' images and labs; the boot path needs none of them */
    .hold = dt_hold,
    .probe = &bcmdt_probe_image, .hello = &bcmdt_hello_image,
    .hello_rb = &bcmdt_hello_rb_image,
    .hello_dma_rb = &bcmdt_hello_dma_rb_image,
    .bench = &bcmdt_bench_image, .bench_pll = &bcmdt_bench_pll_image,
    .fb_upload = dt_fb_upload_l, .fb = dt_fb, .boost = dt_boost,
    .hello_dma_clk = &bcmdt_hello_dma_clk_image, .mark = dt_mark,
    .drift = &bcmdt_drift_image, .scratch = dt_scratch,
    .hello_dma_idle = &bcmdt_hello_dma_idle_image,
    .hello_dma_sby = &bcmdt_hello_dma_sby_image, .boot_how = dt_boot_how,
    .hello_3d = &bcmdt_hello_3d_image,
    .port_algn = dt_port_algn,
#endif
};

/* Read a small text file (a flag or the toggle) into buf; "" if absent */
static void dt_read(const char *path, char *buf, int len)
{
    int fd = open(path, O_RDONLY), n = 0;

    if (fd >= 0)
    {
        n = read(fd, buf, len - 1);
        close(fd);
    }
    buf[n > 0 ? n : 0] = '\0';
}

static bool dt_write(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    bool ok;

    if (fd < 0)
        return false;
    ok = write(fd, text, strlen(text)) == (ssize_t)strlen(text);
    return close(fd) == 0 && ok;
}

#ifdef HAVE_BCMFW_DEVTEST
/* A stage's pass flag: written when it passed, removed when this run
 * tried it (or stage 2 failed under stage 3) and it did not pass */
static void dt_flag(int stage, unsigned stages, const struct bcmdt_result *res)
{
    const char *path = stage == 2 ? DT_PASS2 : stage == 3 ? DT_PASS3 :
                       stage == 4 ? DT_PASS4 : stage == 5 ? DT_PASS5 :
                       stage == BCMDT_FLAG_4B ? DT_PASS4B :
                       stage == BCMDT_FLAG_5I ? DT_PASS5I : DT_PASS4C;
    const struct bcmdt_image *img = stage == 2 ? res->img2 :
                                    stage == 3 ? res->img3 :
                                    stage == 4 ? res->img4 :
                                    stage == 5 ? res->img5 :
                                    stage == BCMDT_FLAG_4B ? res->img4b :
                                    stage == BCMDT_FLAG_5I ? res->img5i :
                                    res->img4c;
    /* the bit in stages/passed_mask: 4b sits at 5, 5 at 6, 4c at 7, 5i at 9 */
    unsigned bit = BCMDT_STAGE(stage == 5 ? BCMDT_STAGE_5 :
                               stage == BCMDT_FLAG_4B ? BCMDT_STAGE_4B :
                               stage == BCMDT_FLAG_4C ? BCMDT_STAGE_4C :
                               stage == BCMDT_FLAG_5I ? BCMDT_STAGE_5I : stage);
    char line[80];

    if ((res->passed_mask & bit) && img)
    {
        int n = snprintf(line, sizeof(line), "pass flag: ");

        bcmdt_flag_format(line + n, sizeof(line) - n, stage, img);
        dt_write(path, line + n);
        line[strlen(line) - 1] = '\0';
        dt_log(line);
    }
    else if (((stages & bit) ||
              (stage == 3 && (stages & BCMDT_STAGE(2)) &&
               !(res->passed_mask & BCMDT_STAGE(2)))) && remove(path) == 0)
    {
        snprintf(line, sizeof(line), "pass flag: stage %d%s cleared",
                 stage >= 10 ? stage / 10 : stage,
                 stage == BCMDT_FLAG_4B ? "b" : stage == BCMDT_FLAG_4C ? "c" :
                 stage == BCMDT_FLAG_5I ? "i" : "");
        dt_log(line);
    }
}
#endif /* HAVE_BCMFW_DEVTEST */

#ifdef HAVE_BCMFW
static void bcmfw_fallback(const char *why);
static bool bcmfw_deliberate;   /* the next drop was asked for (dev8) */
#ifdef BCMDT_LADDER
/* The ladder (bcmfw-boot.c): the rung below the running image (our DMA
 * image below the resident video image, our CPU-push image below our DMA
 * image with level 1) and the one below that; NULL: Apple's vmcs next */
static const struct bcmdt_image *bcmfw_rung_next, *bcmfw_rung_after;
#endif
#ifdef HAVE_BCMFW_RESIDENT
/* p14-resident (bcmfw-boot.c): the image bcmres.c offered, and
 * whether it may sleep its core while it serves the UI */
static struct bcmdt_image bcmfw_res_img;
static bool bcmfw_res_idle;
#endif
#endif

#ifdef HAVE_BCMFW_DEVTEST
/* dev9: stage 4c's precondition, a 4b flag naming the embedded 150 MHz
 * bench; and the video session's (HP_OPEN_FAST_CLOCK): 4b and 4c flags
 * naming the embedded images (pipeline.md, "The 150 MHz clock") */
bool bcm_devtest_4c_allowed(const char **why)
{
    static const struct bcmdt_image *const one[1] = { &bcmdt_bench_pll_image };
    char text[48];

    dt_read(DT_PASS4B, text, sizeof(text));
    if (!text[0])
    {
        *why = "no stage 4b pass flag";
        return false;
    }
    return bcmdt_flag_match(text, BCMDT_FLAG_4B, one, 1, why) != NULL;
}

bool bcm_devtest_fast_clock_allowed(const char **why)
{
    static const struct bcmdt_image *const one[1] = { &bcmdt_hello_dma_clk_image };
    char text[48];

    if (!bcm_devtest_4c_allowed(why))
        return false;
    dt_read(DT_PASS4C, text, sizeof(text));
    if (!text[0])
    {
        *why = "no stage 4c pass flag";
        return false;
    }
    return bcmdt_flag_match(text, BCMDT_FLAG_4C, one, 1, why) != NULL;
}

/* The bench session's stage 4c: the owner judged its pictures wrong, so
 * the flag its automatic checks wrote goes (videos stay at 27 MHz) */
void bcm_devtest_forget_4c(void)
{
    remove(DT_PASS4C);
}

bool bcm_devtest_5i_allowed(const char **why)
{
    static const struct bcmdt_image *const one[1] = { &bcmdt_hello_dma_image };
    char text[48];

    dt_read(DT_PASS5, text, sizeof(text));
    if (!text[0])
    {
        *why = "no stage 5 pass flag";
        return false;
    }
    return bcmdt_flag_match(text, 5, one, 1, why) != NULL;
}

bool bcm_devtest_5s_allowed(const char **why, bool *sleep)
{
    static const struct bcmdt_image *const one[1] = { &bcmdt_hello_dma_idle_image };
    const char *why5i;
    char text[48];

    *sleep = false;
    if (!bcm_devtest_5i_allowed(why))
        return false;
    dt_read(DT_PASS5I, text, sizeof(text));
    *sleep = text[0] && bcmdt_flag_match(text, BCMDT_FLAG_5I, one, 1, &why5i);
    return true;
}

bool bcm_devtest_3d_allowed(const char **why, bool *fast)
{
    const char *why_fast;

    *fast = false;
    if (!bcm_devtest_5i_allowed(why))     /* the same stage 5 flag */
        return false;
    *fast = bcm_devtest_fast_clock_allowed(&why_fast);
    return true;
}

bool bcm_devtest_4b_allowed(const char **why)
{
    static const struct bcmdt_image *const one[1] = { &bcmdt_bench_image };
    char text[48];

    dt_read(DT_PASS4, text, sizeof(text));
    if (!text[0])
    {
        *why = "no stage 4 pass flag";
        return false;
    }
    return bcmdt_flag_match(text, 4, one, 1, why) != NULL;
}

/* The gated stages' preconditions: the pass flag files they read, one
 * line each (bcmdt_run_stages picks its line by tag) */
static void dt_flags(unsigned stages, char *buf, int len)
{
    static const struct { unsigned bit; const char *path; } gates[] =
    {
        { BCMDT_STAGE(BCMDT_STAGE_4B), DT_PASS4 },
        { BCMDT_STAGE(BCMDT_STAGE_4C), DT_PASS4B },
        { BCMDT_STAGE(BCMDT_STAGE_5B), DT_PASS5 },
        { BCMDT_STAGE(BCMDT_STAGE_5I), DT_PASS5 },
        { BCMDT_STAGE(BCMDT_STAGE_5S), DT_PASS5 },
        { BCMDT_STAGE(BCMDT_STAGE_5S), DT_PASS5I },
        { BCMDT_STAGE(BCMDT_STAGE_3D), DT_PASS5 },
        { BCMDT_STAGE(BCMDT_STAGE_3D), DT_PASS4B },
        { BCMDT_STAGE(BCMDT_STAGE_3D), DT_PASS4C },
    };
    unsigned k;
    int n = 0;

    buf[0] = '\0';
    for (k = 0; k < ARRAYLEN(gates); k++)
    {
        char line[48];

        if (!(stages & gates[k].bit) || n >= len - 2)
            continue;
        dt_read(gates[k].path, line, sizeof(line));
        if (!line[0])
            continue;
        n += snprintf(buf + n, len - n, "%s%s", line,
                      line[strlen(line) - 1] == '\n' ? "" : "\n");
    }
}

int bcm_devtest_run_stages(unsigned stages, struct bcmdt_result *res)
{
    char flags[384];
    int rc;

#ifdef HAVE_BCMFW
    /* the stages start from Apple's vmcs */
    if (bcmfw_active)
    {
#ifdef BCMDT_LADDER
        bcmfw_rung_next = bcmfw_rung_after = NULL;  /* straight to Apple's */
#endif
        bcmfw_deliberate = true;
        bcmfw_fallback("the device test runs on Apple's vmcs");
    }
#endif
    dt_flags(stages, flags, sizeof(flags));
    rc = bcmdt_run_stages(&dt_hal, stages, flags, res);
    if (rc >= -1)
    {
        dt_flag(2, stages, res);
        dt_flag(3, stages, res);
        dt_flag(4, stages, res);
        dt_flag(5, stages, res);
        dt_flag(BCMDT_FLAG_4B, stages, res);
        dt_flag(BCMDT_FLAG_4C, stages, res);
        dt_flag(BCMDT_FLAG_5I, stages, res);
    }
    return rc;
}

int bcm_devtest_run(int last, struct bcmdt_result *res)
{
    return bcm_devtest_run_stages(BCMDT_STAGE(last + 1) - 1, res);
}

/* ---- PB-6: Apple's own TV pattern (brief-map-peripherals.md, PB-6) ----
 * One command to the running NOR vmcs and its completion: the word at
 * 0x1F8 no longer the command nor 0xFFFF (bcm_command()'s rule), within
 * limit_us.  *port false when a port access failed. */
static void tvpat_cmd(uint32_t cmd, uint32_t limit_us, uint32_t *st,
                      uint32_t *us, bool *port)
{
    uint32_t t0 = USEC_TIMER;

    *port = dt_wr(BCMA_COMMAND, cmd) == 0;
    *st = cmd;
    if (*port)
    {
        dt_doorbell();
        for (;;)
        {
            if (dt_rd(BCMA_COMMAND, st) < 0)
            {
                *port = false;
                break;
            }
            if (*st != cmd && *st != 0xFFFF)
                break;
            if (USEC_TIMER - t0 > limit_us)
                break;
            yield();
        }
    }
    *us = USEC_TIMER - t0;
}

int bcm_devtest_tv_pattern(const uint16_t *bmp, uint32_t bytes, bool pal,
                           uint32_t limit_us, uint32_t hold_ms,
                           struct bcm_tvpat_result *r)
{
    long t0;

    memset(r, 0, sizeof(*r));
    if (bytes == 0 || (bytes & 3) || ((uintptr_t)bmp & 3))
        return -2;
#ifdef HAVE_BCMFW
    /* Apple's vmcs only: our image driving the display steps down to it
     * through the stock wake path, for the rest of this boot, as the
     * device test does */
    if (bcmfw_active)
    {
#ifdef BCMDT_LADDER
        bcmfw_rung_next = bcmfw_rung_after = NULL;  /* straight to Apple's */
#endif
        bcmfw_deliberate = true;
        bcmfw_fallback("PB-6 runs on Apple's vmcs");
        r->forced = true;
    }
    if (bcmfw_active)
        return -3;              /* never with an image of ours resident */
#endif
    if (dt_take() < 0)
        return -1;              /* nothing touched */

    /* the BMP into the chip's SDRAM, then its first two words back */
    r->upload = dt_wr_block(BCMA_TV_BMPDATA, bmp, bytes / 2);
    if (r->upload == 0 && (dt_rd(BCMA_TV_BMPDATA, &r->head[0]) < 0 ||
                           dt_rd(BCMA_TV_BMPDATA + 4, &r->head[1]) < 0))
        r->upload = -1;
    if (r->upload == 0)
    {
        r->ran = true;
        tvpat_cmd(pal ? BCMCMD_TV_PALBMP : BCMCMD_TV_NTSCBMP, limit_us,
                  &r->cmd_st, &r->cmd_us, &r->cmd_port);
        if (r->cmd_port)
            tvpat_cmd(BCMCMD_TV_MVOFF, limit_us, &r->mv_st, &r->mv_us,
                      &r->mv_port);
        /* the owner watches the TV; SELECT ends it early */
        t0 = current_tick;
        dt_hold(hold_ms);
        r->held_ms = (uint32_t)(current_tick - t0) * (1000 / HZ);
    }

    /* every path: power cut (>= 50 ms off), then the stock wake, which
     * loads Apple's vmcs from NOR and repaints the screen */
    dt_power_cut();
    dt_sleep_ms(60);
    dt_restore(false);
    return 0;
}
#endif /* HAVE_BCMFW_DEVTEST */

#ifdef HAVE_BCMFW
#include "bcmfw-boot.c"      /* the boot path; needs the helpers above */
#endif

#ifdef HAVE_BCMFW_VIDEO
/* ---- The video player's hold on the BCM (apps/video/vplayer.c) -------- */

static bool video_running;      /* an image answers (stock-order power-off) */
/* p14-resident: the image running is the boot path's resident video image
 * (taken from the display driver, or booted again by the player from the
 * same bytes): a start skips the boot, a stop hands the panel back */
static bool video_res;
static uint32_t video_idle[5];  /* its idle counters at the take */
static bool video_took_ours;    /* the take found our image, not Apple's */
static int vdma_wait(void);      /* below: a PP DMA block's end */

int bcm_video_take(void)
{
    bool res = bcmfw_active && bcmfw_active == &bcmfw_res_img &&
               !bcmfw_hung && lcd_state.display_on;

    if (dt_take() < 0)
        return -1;
    video_running = true;       /* Apple's vmcs or the boot path's image */
    video_res = res;
    video_took_ours = bcmfw_active != NULL;
    video_idle[4] = 0;
    if (res)
    {
        /* the counters for the log; the session polls, as proven */
        video_idle[4] = bcmfw_idle_read(dt_rd, video_idle);
        dt_wr(BCMA_IDLE_EN, 0);
    }
    return 0;
}

int bcm_video_boot(const uint8_t *img, uint32_t bytes)
{
    uint32_t st;
    int rc;

    vdma_wait();
    if (video_res && img == bcmfw_res_img.data && bytes == bcmfw_res_img.size)
    {
        /* resident: it runs and answers; HELLO and OPEN come next */
        if (dt_rd(BCMA_COMMAND, &st) == 0 && st == BCMFW_ST_DONE)
            return 0;
        dt_log("video: the resident image does not answer: booted again");
    }
    rc = bcmdt_video_boot(&dt_hal, img, bytes, video_running);
    video_running = rc == 0;
    /* the same bytes as the resident image: it can stay at the stop */
    video_res = rc == 0 && img == bcmfw_res_img.data &&
                bytes == bcmfw_res_img.size;
    return rc;
}

void bcm_video_off(void)
{
    vdma_wait();                /* a block still going out */
    bcmdt_video_off(&dt_hal, video_running);
    video_running = video_res = false;
}

void bcm_video_cut(void)
{
    vdma_wait();
    dt_power_cut();
    video_running = video_res = false;
    dt_sleep_ms(60);            /* >= 50 ms off before the next power-up */
}

void bcm_video_release(void)
{
    video_running = video_res = false;
    dt_restore(false);
}

int bcm_video_hand_back(void)
{
    uint32_t st;

    if (!video_running || !video_res || bcmfw_active != &bcmfw_res_img ||
        vdma_wait() || dt_rd(BCMA_COMMAND, &st) || st != BCMFW_ST_DONE)
        return -1;
    if (bcmfw_res_idle)
        dt_wr(BCMA_IDLE_EN, BCMA_IDLE_EN_MAGIC);
    video_running = video_res = false;
    /* the driver as after a wake: nothing pending, the whole frame next
     * (the panel still shows the last picture) */
    lcd_state.x0 = lcd_state.x1 = 0;
    bcmfw_hung = false;
    bcmfw_kick_tick = current_tick;
    dt_restore(true);
    lcd_update();
    return 0;
}

void bcm_video_idle_counters(uint32_t c[5])
{
    memcpy(c, video_idle, sizeof(video_idle));
}

#ifdef HAVE_BCMFW_DEVTEST
/* ---- The power map's probes (brief-map-power.md section 9) ----------- */

bool bcm_video_took_ours(void)
{
    return video_took_ours;
}

/* PW1 step 6: the PP's side of the BCM's pins, read only */
void bcm_video_pp_regs(uint32_t out[5])
{
    out[0] = GPO32_VAL;
    out[1] = GPO32_ENABLE;
    out[2] = *(volatile unsigned long *)0x70000088;    /* GPO32 read-back */
    out[3] = STRAP_OPT_A;
    out[4] = *(volatile unsigned long *)0x70000040;
}

/* PW5 (bcmdt_hib_cycle's gpo): GPO32 bits 14 and 15 only, cleared then
 * set, read-modify-write with interrupts off; GPO32_ENABLE is never
 * written.  (0, 0) only reads. */
uint32_t bcm_video_gpo32(uint32_t clear, uint32_t set)
{
    int oldlevel = disable_irq_save();
    uint32_t v;

    clear &= 0xC000;
    set &= 0xC000;
    if (clear | set)
        GPO32_VAL = (GPO32_VAL & ~clear) | set;
    v = GPO32_VAL;
    restore_irq(oldlevel);
    return v;
}

uint32_t bcm_video_gpo32_enable(void)
{
    return GPO32_ENABLE;
}

/* PW6: the port's four registers read as they are, no address written
 * first (no WR_ADDR, RD_ADDR or CONTROL write): DATA four times,
 * CONTROL, ALT_DATA four times, ALT_CONTROL */
void bcm_video_port_raw(uint16_t out[10])
{
    int i;

    vdma_wait();
    for (i = 0; i < 4; i++)
        out[i] = BCM_DATA;
    out[4] = BCM_CONTROL;
    for (i = 0; i < 4; i++)
        out[5 + i] = BCM_ALT_DATA;
    out[9] = BCM_ALT_CONTROL;
}

/* PW7: the ALT bank.  Every handshake bounded at 1 ms; nothing written
 * outside 0x12C000-0x12CFFF (free SRAM in the video image) and the host
 * frame at 0xE0000; no CONTROL write. */
#define ALT_LAB_AREA    0x12C000
#define ALT_LAB_HW      256         /* halfwords an area */
#define ALT_LAB_FRAME   0xE0000
#define ALT_LAB_FRAME_B (LCD_WIDTH * LCD_HEIGHT * 2)

static bool alt_until(volatile unsigned short *reg, unsigned mask)
{
    unsigned long t0 = USEC_TIMER;

    while (!(*reg & mask))
        if (USEC_TIMER - t0 > 1000)
            return false;
    return true;
}

/* The 2-register stm loop (lcd_write_data's) into either bank's DATA */
static void alt_stream(unsigned long port, const uint16_t *src, uint32_t bytes)
{
    const uint32_t *w = (const uint32_t *)src;
    uint32_t n = bytes / 8;

    asm volatile(
        "1:  ldmia %0!, {r2, r3}   \n"
        "    stmia %2, {r2, r3}    \n"
        "    subs  %1, %1, #1      \n"
        "    bgt   1b              \n"
        : "+r"(w), "+r"(n) : "r"(port) : "r2", "r3", "cc", "memory");
}

static uint32_t alt_count(uint32_t v, uint32_t want_lo, uint32_t want_hi)
{
    return ((v & 0xFFFF) == want_lo) + ((v >> 16) == want_hi);
}

int bcm_video_alt_lab(struct bcm_alt_result *r)
{
    static const uint16_t tags[4] = { 0xA000, 0xB000, 0xC000, 0xD000 };
    uint32_t i, k, v, a, t0;
    int oldlevel;

    memset(r, 0, sizeof(*r));
    if (vdma_wait())
        return -1;
    /* 1: main bank */
    BCM_WR_ADDR32 = ALT_LAB_AREA;
    if (!alt_until(&BCM_CONTROL, 0x2))
        return r->fail_step = 1, -1;
    for (i = 0; i < ALT_LAB_HW; i++)
        BCM_DATA = 0xA000 | i;
    /* 2: ALT bank, the address by halves (low first), Apple's way: no
       write-ready wait needed, only watched */
    BCM_ALT_WR_ADDR = (ALT_LAB_AREA + 0x400) & 0xFFFF;
    BCM_ALT_WR_ADDR = (ALT_LAB_AREA + 0x400) >> 16;
    r->alt_ready = alt_until(&BCM_ALT_CONTROL, 0x2);
    for (i = 0; i < ALT_LAB_HW; i++)
        BCM_ALT_DATA = 0xB000 | i;
    /* 3: interleaved, each bank its own latch */
    BCM_WR_ADDR32 = ALT_LAB_AREA + 0x800;
    if (!alt_until(&BCM_CONTROL, 0x2))
        return r->fail_step = 3, -1;
    BCM_ALT_WR_ADDR32 = ALT_LAB_AREA + 0xC00;
    oldlevel = disable_irq_save();      /* the two streams as one run */
    for (i = 0; i < ALT_LAB_HW; i++)
    {
        BCM_DATA = 0xC000 | i;
        BCM_ALT_DATA = 0xD000 | i;
    }
    restore_irq(oldlevel);
    /* 4: all four areas read back by each bank */
    for (k = 0; k < 4; k++)
        for (i = 0; i < ALT_LAB_HW; i += 2)
        {
            a = ALT_LAB_AREA + 0x400 * k + 2 * i;
            if (dt_rd(a, &v))
                return r->fail_step = 4, -1;
            if (v == 0xFFFFFFFF)
                return r->fail_step = 4, r->dead = 1, -1;
            r->main_ok[k] += alt_count(v, tags[k] | i, tags[k] | (i + 1));
            BCM_ALT_RD_ADDR32 = a;
            if (!alt_until(&BCM_ALT_CONTROL, 0x10))
                return r->fail_step = 5, -1;
            v = BCM_ALT_DATA32;
            if (v == 0xFFFFFFFF)
                return r->fail_step = 5, r->dead = 1, -1;
            r->alt_ok[k] += alt_count(v, tags[k] | i, tags[k] | (i + 1));
        }
    /* 5: a frame into the host frame's own area, each bank */
    BCM_ALT_WR_ADDR32 = ALT_LAB_FRAME;
    alt_until(&BCM_ALT_CONTROL, 0x2);
    t0 = USEC_TIMER;
    alt_stream(0x30040000, dt_fb(), ALT_LAB_FRAME_B);
    r->frame_us[1] = USEC_TIMER - t0;
    BCM_WR_ADDR32 = ALT_LAB_FRAME;
    if (!alt_until(&BCM_CONTROL, 0x2))
        return r->fail_step = 6, -1;
    t0 = USEC_TIMER;
    alt_stream(0x30000000, dt_fb(), ALT_LAB_FRAME_B);
    r->frame_us[0] = USEC_TIMER - t0;
    return 0;
}
#endif /* HAVE_BCMFW_DEVTEST */

/* dev39: the player's blocks by PP DMA (channel 3), meant to free the CPU
 * while a block moves.  A dead end on silicon (docs/data-paths.md D1):
 * sessions 34 and 35 found nothing landed, into the port's SDRAM FIFO or
 * into SRAM, at every size; session 8's "clean" result was the CPU's own
 * write read back.  Off unless a lab or an A/B turns it on.  A block's
 * DMA is started and left running; the next access to the port waits for
 * it first.  Small blocks, and all of them after a DMA that never ended,
 * go by CPU. */
static bool vdma_on = false, vdma_busy, vdma_broken;
static uint32_t vdma_addr, vdma_w0;     /* the block's word 0, rewritten */

/* Session 37: the PP's second DMA engine delivers into the port, every
 * word, 256 B to 64 KB (E2-3, E2-4, E2-7(1)).  The player's blocks go
 * through it (e2-port.c, shared with the UI's screen pushes) while it
 * holds the engine (E2PORT_VIDEO); the end is polled at the next port
 * access. */
#include "e2-port.h"
static bool vdma_e2;            /* the second engine, not channel 3 */
static uint32_t vdma_t0;

static bool vdma_fixup = true;          /* rewrite word 0 (the lab: off) */

/* Stage 8a-0b (dev40): the PP's I2S transmitter stopped and started as
 * Apple's driver does (osos 0x1018C55C: IISCONFIG bits 29 and 1), the
 * codec's clocks running on; whatever was set comes back */
static unsigned long iis_tx_saved;

void bcm_devtest_iis_tx(bool on)
{
    if (!on)
    {
        iis_tx_saved = IISCONFIG & (IIS_TXFIFOEN | IIS_IRQTX);
        IISCONFIG &= ~(IIS_TXFIFOEN | IIS_IRQTX);
    }
    else
        IISCONFIG |= iis_tx_saved;
}

void bcm_video_set_dma_fixup(bool on)
{
    vdma_fixup = on;
}
static struct bcm_video_port_load vload;

void bcm_video_set_dma(bool on)
{
    vdma_on = on;
    vdma_e2 = false;
    vdma_broken = false;
}

/* The second engine for the port's blocks (session 37's E2 lab); off
 * gives the CPU's copy, as bcm_video_set_dma(false) */
void bcm_video_set_dma_e2(bool on)
{
    vdma_on = on;
    vdma_e2 = on;
    vdma_broken = false;
}

void bcm_video_port_load(struct bcm_video_port_load *out, bool reset)
{
    *out = vload;
    if (reset)
        memset(&vload, 0, sizeof(vload));
}

/* ---- The port lock, a piece at a time (jobs.md section 6, gap G3) -----
 *
 * Every port call of the player's (bcm_video_rd/wr/wr_block/flush) takes
 * lcd-video.c's port lock for its piece and gives it back at the end,
 * so another writer (the UI's push, the job ring once 19-9 moves it to the
 * COP) can never land between an address and its data.  A DMA block
 * outlives its call: the lock taken for it stays held (vdma_held) until
 * vdma_wait() sees its end, on the same thread (the feed flushes before it
 * blocks and before it exits; the stop paths flush on the UI's thread).
 * The lock is recursive, so the nested takes cost a count.  Ownership
 * (bcm_devtest_owned, display_on off) still keeps lcd_update*() away;
 * debug builds check it on every piece. */
static bool vdma_held;              /* the lock kept for a block in flight */
#ifdef DEBUG
#include "panic.h"
static unsigned int vdma_holder;
#define VPORT_CHECK() do { \
        if (!bcm_devtest_owned || lcd_state.display_on) \
            panicf("video port: not owned"); } while (0)
#else
#define VPORT_CHECK() do { } while (0)
#endif

static void vdma_unhold(void)
{
    if (!vdma_held)
        return;
#ifdef DEBUG
    if (vdma_holder != thread_self())
        panicf("video port: block ended on another thread");
#endif
    vdma_held = false;
    lcd_port_unlock(true);
}

static int vdma_wait_end(void);

/* A block's end, and the lock held for it given back */
static int vdma_wait(void)
{
    int rc = vdma_wait_end();

    if (!vdma_busy)
        vdma_unhold();
    return rc;
}

static int vdma_wait_end(void)
{
    if (!vdma_busy)
        return 0;
    if (vdma_e2)
    {
        while (e2port_busy())
        {
            if (USEC_TIMER - vdma_t0 > E2PORT_TIMEOUT_US)
            {
                e2port_stop();
                e2port_release(E2PORT_VIDEO);
                vdma_busy = false;
                vdma_broken = true;     /* the CPU from now on */
                vload.dma_fail++;
                return -1;
            }
            yield();
        }
        vdma_busy = false;
        e2port_release(E2PORT_VIDEO);
        vload.dma_us += USEC_TIMER - vdma_t0;
        /* the port's write side ready again before a new address */
        if (!e2port_port_ready())
        {
            vload.dma_fail++;
            vdma_broken = true;
            return -1;
        }
        return 0;
    }
    while (DMA3_STATUS & DMA_STATUS_BUSY)
    {
        if (USEC_TIMER - vdma_t0 > DT_DMA_US)
        {
            DMA3_CMD = 0;
            (void)DMA3_STATUS;
            vdma_busy = false;
            vdma_broken = true;         /* the CPU from now on */
            vload.dma_fail++;
            return -1;
        }
        yield();
    }
    (void)DMA3_STATUS;
    vdma_busy = false;
    vload.dma_us += USEC_TIMER - vdma_t0;
    /* the engine has read its last word, not necessarily handed it to
       the BCM: wait for the port's write side to be ready again before
       anything sets a new address (session 32: blocks lost their first
       word and later words came shifted, 77% read back wrong) */
    if (!dt_wait(&BCM_CONTROL, 0x2, 2, 0))
    {
        vload.dma_fail++;
        vdma_broken = true;
        return -1;
    }
    /* word 0 again, by CPU (see bcm_video_wr_block) */
    if (!vdma_fixup)
        return 0;
    BCM_WR_ADDR32 = vdma_addr;
    if (!dt_wait(&BCM_CONTROL, 0x2, 2, vdma_addr))
        return -1;
    BCM_DATA32 = vdma_w0;
    return 0;
}

int bcm_video_rd(uint32_t addr, uint32_t *val)
{
    bool locked = lcd_port_lock();
    int rc;

    VPORT_CHECK();
    rc = vdma_wait() ? -1 : dt_rd(addr, val);
    lcd_port_unlock(locked);
    return rc;
}

int bcm_video_wr(uint32_t addr, uint32_t val)
{
    bool locked = lcd_port_lock();
    int rc;

    VPORT_CHECK();
    rc = vdma_wait() ? -1 : dt_wr(addr, val);
    lcd_port_unlock(locked);
    return rc;
}

static int vwr_block(uint32_t addr, const uint16_t *src, uint32_t n);

/* One block: by the CPU, the lock back at the end; by DMA, the lock kept
 * until vdma_wait() sees the block through */
int bcm_video_wr_block(uint32_t addr, const uint16_t *src, uint32_t n)
{
    bool locked = lcd_port_lock();
    int rc;

    VPORT_CHECK();
    rc = vwr_block(addr, src, n);
    if (vdma_busy && locked && !vdma_held)
    {
        vdma_held = true;
#ifdef DEBUG
        vdma_holder = thread_self();
#endif
    }
    else
        lcd_port_unlock(locked);
    return rc;
}

static int vwr_block(uint32_t addr, const uint16_t *src, uint32_t n)
{
    unsigned long a = (unsigned long)src;
    uint32_t bytes = 2 * n, t0;
    int rc;

    if (vdma_wait())
        return -1;
    if (addr & 3)               /* lands 2 bytes low: refused (dt_wr) */
    {
        dt_half_writes++;
        return -1;
    }
    vload.bytes += bytes;
    if (!vdma_on || vdma_broken || bytes < VDMA_MIN ||
        (vdma_e2 ? !e2port_fits(src, bytes) || !e2port_claim(E2PORT_VIDEO)
                 : bytes > DT_DMA_PIECE || (a & 3) || (bytes & 3)))
    {
        t0 = USEC_TIMER;
        rc = dt_wr_block(addr, src, n);
        vload.cpu_us += USEC_TIMER - t0;
        vload.cpu_blocks++;
        return rc;
    }
    if (vdma_e2)
    {
        if (e2port_start(addr, src, bytes) < 0)
        {
            e2port_release(E2PORT_VIDEO);
            return -1;
        }
        vdma_t0 = USEC_TIMER;
        vdma_busy = true;
        vload.dma_blocks++;
        return 0;
    }
    if (a < UNCACHED_BASE_ADDR)
    {
        a = UNCACHED_ADDR(a);
        commit_dcache();
    }
    BCM_WR_ADDR32 = addr;
    if (!dt_wait(&BCM_CONTROL, 0x2, 2, addr))
        return -1;
    /* sessions 32-33: each DMA block's word 0 never landed (the FIFO still
       held the fill pattern there) while the rest did; vdma_wait writes
       it again by CPU once the block is through */
    vdma_addr = addr;
    vdma_w0 = *(const uint32_t *)src;
    DMA_MASTER_CONTROL |= DMA_MASTER_CONTROL_EN;
    DMA3_PER_ADDR = (unsigned long)&BCM_DATA32;
    DMA3_FLAGS = DMA_FLAGS_UNK26;
    DMA3_INCR = DMA_INCR_RANGE_FIXED | DMA_INCR_WIDTH_32BIT;
    DMA3_RAM_ADDR = a;
    vdma_t0 = USEC_TIMER;
    DMA3_CMD = DMA_CMD_RAM_TO_PER | DMA_CMD_SINGLE | (bytes - 4) |
               DMA_CMD_START;
    vdma_busy = true;
    vload.dma_blocks++;
    return 0;
}

int bcm_video_flush(void)
{
    bool locked;
    int rc;

    if (!vdma_busy && !vdma_held)
        return 0;               /* the CPU's copy: nothing in flight */
    locked = lcd_port_lock();
    rc = vdma_wait();
    lcd_port_unlock(locked);
    return rc;
}

/* Bench row CR1 only (apps/video/vplayer.c): n pixels (even) at addr by
 * the CPU, in one of Apple's write forms instead of ours (BCM_WRV_*:
 * the address as two halfword stores to WR_ADDR, low then high; the data
 * as halfword stores; no CONTROL poll after the address).  Never used by
 * the shared write paths. */
int bcm_video_wr_variant(uint32_t addr, const uint16_t *src, uint32_t n,
                         unsigned how)
{
    bool locked = lcd_port_lock();
    uint32_t i;
    int rc = 0;

    VPORT_CHECK();
    /* whole words only: in either data form an odd last halfword would be
       lost (the port loses a lone trailing halfword [silicon, ALGNP
       PLH1/3/5]; the 32-bit loop never sends it) */
    if (vdma_wait() || (n & 1))
        rc = -1;
    else
    {
        if (how & BCM_WRV_ADDR16)
        {
            BCM_WR_ADDR = addr & 0xFFFF;
            BCM_WR_ADDR = addr >> 16;
        }
        else
            BCM_WR_ADDR32 = addr;
        if (!(how & BCM_WRV_NOPOLL) && !dt_wait(&BCM_CONTROL, 0x2, 2, addr))
            rc = -1;
        else if (how & BCM_WRV_DATA16)
            for (i = 0; i < n; i++)
                BCM_DATA = src[i];
        else
            for (i = 0; i + 1 < n; i += 2)
                BCM_DATA32 = src[i] | (uint32_t)src[i + 1] << 16;
    }
    lcd_port_unlock(locked);
    return rc;
}

uint32_t bcm_video_now_us(void)
{
    return USEC_TIMER;
}

void bcm_video_port_stats(uint32_t out[8], bool reset)
{
    memcpy(out, dt_port, sizeof(dt_port));
    if (reset)
        memset(dt_port, 0, sizeof(dt_port));
}

/* Stage 6 (apps/ipodui/devbcm.c): a clip through the real player, on a
 * stage 5 pass flag naming the embedded DMA image; its lines go to the
 * device test's log */
#ifdef HAVE_BCMFW_DEVTEST
bool bcm_devtest_6_allowed(const char **why)
{
    return bcmfw_flag(DT_PASS5, 5, bcmfw_dma_images, why) != NULL;
}
#endif

void bcm_devtest_log(const char *line)
{
    dt_log(line);
}
#endif /* HAVE_BCMFW_VIDEO */

#if defined(HAVE_BCMFW_VIDEO) && defined(HAVE_BCMFW_DEVTEST)
/* ---- The second DMA engine lab (E2-0 ... E2-7, apps/ipodui/devbcm.c) ---
 *
 * Apple's register sequence for the engine at 0x6000_8000 (pp5022-
 * reference/dma.md section 6.3; nothing here has run on silicon before
 * this lab), the general engine's channel 3 as the control, the BCM held
 * between bootstrap and start, and a counting loop in IRAM for the CPU
 * the engine frees.  Register names are dma.md's.  Every wait is bounded;
 * a piece that never ends is stopped in Apple's order with IRQ and FIQ
 * masked.  Interrupt sources 26-28 stay masked while a piece runs (26
 * only when the audio is stopped: its FIQ handler would take an engine-2
 * event for its own), so no handler is needed. */
#include "bcm-e2lab.h"

#define E2_MASTER         (*(volatile uint32_t *)0x60008000)
#define E2_REG(n, off)    (*(volatile uint32_t *)(0x60009000 + 0x20 * (n) + (off)))
#define E2_CMD(n)         E2_REG(n, 0x00)
#define E2_STATUS(n)      E2_REG(n, 0x04)
#define E2_SRC(n)         E2_REG(n, 0x10)
#define E2_SRC_CFG(n)     E2_REG(n, 0x14)
#define E2_DST(n)         E2_REG(n, 0x18)
#define E2_DST_CFG(n)     E2_REG(n, 0x1C)
#define E2_INT_BITS       (7u << 26)    /* raw sources 26-28 */
#define E2_INT_AUDIO      (1u << 26)    /* the general engine: audio's FIQ */
#define E2_INT_STAT       ((volatile uint32_t *)&INT_STAT)
#define E2_DMA3_STATUS    ((volatile uint32_t *)&DMA3_STATUS)
#define E2_DMA3_CMD       ((volatile uint32_t *)&DMA3_CMD)
/* The write address as two halfword stores, low half first, as Apple's
 * port driver sends it (dma.md section 6.3 step 2) */
#define E2_WR_ADDR_LO     (*(volatile unsigned short *)0x30010000)
#define E2_WR_ADDR_HI     (*(volatile unsigned short *)0x30010002)

static uint32_t e2_saved[13];
static uint32_t e2_masked;      /* sources masked for a piece and left so */

static uint32_t e2_phys(const void *p)
{
    uintptr_t a = (uintptr_t)p;

    return a < UNCACHED_BASE_ADDR ? (uint32_t)(a | UNCACHED_BASE_ADDR)
                                  : (uint32_t)a;
}

void bcm_e2_dump(uint32_t *out)
{
    int i, k = 0;

    for (i = 0; i < 4; i++)
        out[k++] = inl(0x60008000 + 4 * i);
    for (i = 0; i < 64; i++)
        out[k++] = inl(0x60009000 + 4 * i);
    for (i = 0; i < 4; i++)
        out[k++] = inl(0x6000A000 + 4 * i);
    for (i = 0; i < 32; i++)
        out[k++] = inl(0x6000B000 + 4 * i);
}

void bcm_e2_save(void)
{
    vdma_wait();
    e2_saved[0] = E2_MASTER;
    e2_saved[1] = E2_CMD(0);
    e2_saved[2] = E2_SRC(0);
    e2_saved[3] = E2_SRC_CFG(0);
    e2_saved[4] = E2_DST(0);
    e2_saved[5] = E2_DST_CFG(0);
    e2_saved[6] = DMA_MASTER_CONTROL;
    e2_saved[7] = DMA3_FLAGS;
    e2_saved[8] = DMA3_INCR;
    e2_saved[9] = DMA3_PER_ADDR;
    e2_saved[10] = DMA3_RAM_ADDR;
    e2_saved[11] = E2_CMD(1);
    e2_saved[12] = INT_STAT & E2_INT_BITS;
    e2_masked = 0;
}

/* Apple's stop: IRQ and FIQ masked, bit 30 clear, then bit 31 */
static void e2_stop(volatile uint32_t *cmd)
{
    int level = disable_interrupt_save(IRQ_FIQ_STATUS);

    *cmd &= ~E2_CMD_INTR;
    *cmd &= ~E2_CMD_START;
    restore_interrupt(level);
}

void bcm_e2_restore(uint32_t final[6])
{
    uint32_t raised;
    int n;

    for (n = 0; n < 2; n++)
        if (E2_CMD(n) & (E2_CMD_INTR | E2_CMD_START))
            e2_stop(&E2_CMD(n));
    E2_SRC(0) = e2_saved[2];
    E2_SRC_CFG(0) = e2_saved[3];
    E2_DST(0) = e2_saved[4];
    E2_DST_CFG(0) = e2_saved[5];
    E2_CMD(0) = e2_saved[1] & ~(E2_CMD_INTR | E2_CMD_START);
    (void)E2_STATUS(0);
    (void)E2_STATUS(0);
    E2_MASTER = e2_saved[0];
    if (DMA3_STATUS & DMA_STATUS_BUSY)
        DMA3_CMD = 0;
    (void)DMA3_STATUS;
    DMA3_FLAGS = e2_saved[7];
    DMA3_INCR = e2_saved[8];
    DMA3_PER_ADDR = e2_saved[9];
    DMA3_RAM_ADDR = e2_saved[10];
    DMA_MASTER_CONTROL = (DMA_MASTER_CONTROL & ~DMA_MASTER_CONTROL_EN) |
                         (e2_saved[6] & DMA_MASTER_CONTROL_EN);
    (void)E2_STATUS(1);
    raised = INT_STAT & E2_INT_BITS & ~e2_saved[12];
    if (e2_masked & ~raised)
        CPU_INT_EN = e2_masked & ~raised;
    final[0] = E2_MASTER;
    final[1] = E2_CMD(0);
    final[2] = E2_CMD(1);
    final[3] = DMA_MASTER_CONTROL;
    final[4] = e2_saved[12];
    final[5] = INT_STAT & E2_INT_BITS;
}

/* The counting loop, in IRAM: touches no SDRAM unless mem is set (an
 * uncached word: one store and one load an iteration).  Ends when
 * (*reg & mask) == want (mask 0: never; *reg is read anyway, so the
 * baseline's loop costs the same) or after limit_us.  out[0] us,
 * out[1] the I2S TX FIFO's most free entries seen, out[2] 1 if ended by
 * the register. */
static uint32_t ICODE_ATTR __attribute__((noinline))
e2_spin(volatile uint32_t *reg, uint32_t mask, uint32_t want,
        uint32_t limit_us, volatile uint32_t *mem, uint32_t out[3])
{
    uint32_t n = 0, t0 = USEC_TIMER, f, fmax = 0, hit = 0, v;

    for (;;)
    {
        n++;
        if (mem)
        {
            *mem = n;
            n += *mem - n;
        }
        f = IISFIFO_CFG & IIS_TX_FREE_MASK;
        if (f > fmax)
            fmax = f;
        v = *reg;               /* read even for the baseline: same cost */
        if (mask && (v & mask) == want)
        {
            hit = 1;
            break;
        }
        if (USEC_TIMER - t0 >= limit_us)
            break;
    }
    out[0] = USEC_TIMER - t0;
    out[1] = fmax >> 16;
    out[2] = hit;
    return n;
}

uint32_t bcm_e2_spin(volatile uint32_t *word, uint32_t limit_us,
                     uint32_t out[2])
{
    /* engine 2's channel 1, which the lab never starts */
    uint32_t o[3], n = e2_spin(&E2_STATUS(1), 0, 0, limit_us, word, o);

    out[0] = o[0];
    out[1] = o[1];
    return n;
}

int bcm_e2_hold(void)
{
    int i;

    GPO32_VAL |= 0x4000;
    sleep(HZ/20);
    STRAP_OPT_A &= ~0xF00;
    outl(0x1313, 0x70000040);
    if (!dt_until(&BCM_ALT_CONTROL, 0x80, false) ||
        !dt_until(&BCM_ALT_CONTROL, 0x40, true))
        goto fail;
    for (i = 0; i < 8; i++)
        BCM_CONTROL = bcm_bootstrapdata[i];
    for (i = 3; i < 8; i++)
        BCM_ALT_CONTROL = bcm_bootstrapdata[i];
    if (!dt_until(&BCM_RD_ADDR, 1, true) ||
        !dt_until(&BCM_ALT_RD_ADDR, 1, true))
        goto fail;
    (void)BCM_WR_ADDR;
    (void)BCM_ALT_WR_ADDR;
    if (!dt_until(&BCM_ALT_CONTROL, 0x80, false) ||
        !dt_until(&BCM_ALT_CONTROL, 0x40, true))
        goto fail;
    return 0;               /* held: no image, no start (Apple's upload
                               point) */
fail:
    dt_power_cut();
    return -1;
}

void bcm_e2_cut(void)
{
    dt_power_cut();
}

int bcm_e2_cpu_copy(uint32_t bcm_addr, const void *src, uint32_t bytes,
                    uint32_t *us)
{
    uint32_t t0;
    int rc;

    if (vdma_wait() || ((uintptr_t)src & 3) || (bytes & 3))
        return -1;
    t0 = USEC_TIMER;
    rc = dt_wr_block(bcm_addr, (const uint16_t *)src, bytes / 2);
    if (rc == 0 && !dt_until(&BCM_CONTROL, 0x2, true))
        rc = -1;
    *us = USEC_TIMER - t0;
    return rc;
}

/* Poll a running piece for every completion signal, up to the bound */
static int e2_poll(struct e2_piece *p, volatile uint32_t *st,
                   volatile uint32_t *cmd, uint32_t t0)
{
    bool busy_seen = false, cmd_seen = false;
    uint32_t s, c, i, t, pre = p->st_pre[p->st_pre[1] ? 1 : 0], last = pre;

    for (;;)
    {
        s = *st;
        c = *cmd;
        i = INT_STAT & E2_INT_BITS;
        t = USEC_TIMER - t0;
        if (s != last && p->nsv < 4)
        {
            p->sv[p->nsv] = s;
            p->sv_t[p->nsv++] = t;
        }
        last = s;
        if ((s & E2_CMD_INTR) && !(pre & E2_CMD_INTR) &&
            !(p->how & E2LAB_HOW_ST30))
        {
            p->how |= E2LAB_HOW_ST30;
            p->t_how[0] = t;
        }
        if (s & DMA_STATUS_BUSY)
            busy_seen = true;
        else if ((busy_seen || p->engine == E2LAB_CH3) &&
                 !(p->how & E2LAB_HOW_ST31))
        {
            /* channel 3's busy bit is known: clear means done */
            p->how |= E2LAB_HOW_ST31;
            p->t_how[1] = t;
        }
        if (c & E2_CMD_START)
            cmd_seen = true;
        else if (cmd_seen && !(p->how & E2LAB_HOW_CMD31))
        {
            p->how |= E2LAB_HOW_CMD31;
            p->t_how[2] = t;
        }
        if (i != p->int_pre && !(p->how & E2LAB_HOW_INT))
        {
            p->how |= E2LAB_HOW_INT;
            p->t_how[3] = t;
        }
        /* any signal ends it, but never while a busy bit still shows */
        if (p->how && !(s & DMA_STATUS_BUSY))
        {
            p->us = t;
            return 0;
        }
        if (t > E2LAB_TIMEOUT_US)
        {
            p->us = t;
            return 1;
        }
    }
}

/* The register and value that mean "done" for the counting loop: the
 * first signal an earlier piece of the lab saw */
static void e2_done_sig(const struct e2_piece *p, volatile uint32_t **reg,
                        uint32_t *mask, uint32_t *want)
{
    volatile uint32_t *st = p->engine == E2LAB_ENG2 ? &E2_STATUS(0)
                                                    : E2_DMA3_STATUS;
    volatile uint32_t *cmd = p->engine == E2LAB_ENG2 ? &E2_CMD(0)
                                                     : E2_DMA3_CMD;

    *reg = st;
    if (p->done_how & E2LAB_HOW_ST30)
        *mask = *want = E2_CMD_INTR;
    else if (p->done_how & E2LAB_HOW_ST31)
    {
        *mask = DMA_STATUS_BUSY;
        *want = 0;
    }
    else if (p->done_how & E2LAB_HOW_INT)
    {
        /* the source bits that rose in the piece that learnt it */
        *reg = E2_INT_STAT;
        *mask = *want = p->done_int & E2_INT_BITS;
    }
    else
    {
        *reg = cmd;
        *mask = E2_CMD_START;
        *want = 0;
    }
}

int bcm_e2_piece(struct e2_piece *p)
{
    volatile uint32_t *st, *cmd;
    uint32_t src, dst, t0, en, v, o[3], raised;
    int rc;

    p->rc = -1;
    p->us = p->how = p->nsv = p->count = p->iis_max_free = 0;
    p->ready_us = p->cmd_written = p->cmd_post = p->en_masked = 0;
    p->st_pre[0] = p->st_pre[1] = p->st_post[0] = p->st_post[1] = 0;
    memset(p->t_how, 0, sizeof(p->t_how));
    p->freq = cpu_frequency;
    if (((uintptr_t)p->src & 3) || (uintptr_t)p->src >= 0x40000000)
        return -1;
    if (p->port ? !e2_target_ok(p->bcm_addr, p->bytes, p->lo, p->hi)
                : !e2_target_ok((uint32_t)(uintptr_t)p->dst, p->bytes,
                                p->lo, p->hi))
        return -1;
    if (p->work == E2LAB_WORK_SDRAM && !p->work_word)
        return -1;
    src = e2_phys(p->src);
    dst = p->port ? 0x30000000u : e2_phys(p->dst);
    p->rc = -2;
    if (vdma_wait())
        return -2;
    if (p->port)
    {
        E2_WR_ADDR_LO = p->bcm_addr & 0xFFFF;
        E2_WR_ADDR_HI = p->bcm_addr >> 16;
        if (p->wait_ready && !dt_until(&BCM_CONTROL, 0x2, true))
            return -2;
    }
    commit_dcache();            /* Apple: the whole cache, every start */
    en = CPU_INT_EN_STAT & E2_INT_BITS & ~(p->audio ? E2_INT_AUDIO : 0);
    if (en)
        CPU_INT_DIS = en;
    p->en_masked = en;
    p->int_pre = INT_STAT & E2_INT_BITS;
    if (p->engine == E2LAB_ENG2)
    {
        st = &E2_STATUS(0);
        cmd = &E2_CMD(0);
        E2_MASTER |= 1u << 31;
        *cmd = e2_cmd_init(*cmd);
        E2_SRC_CFG(0) = e2_cfg_code(E2_SRC_CFG(0), p->src_code);
        E2_DST_CFG(0) = e2_cfg_code(E2_DST_CFG(0), p->dst_code);
        E2_SRC(0) = src;
        E2_DST(0) = dst;
        v = e2_cmd_piece(*cmd, p->bytes, p->b27);
        *cmd = v;
        p->st_pre[0] = *st;
        if (p->st_pre[0])
            p->st_pre[1] = *st;     /* Apple: again, a stale event */
        if (p->intr)
            v |= E2_CMD_INTR;
        *cmd = v;
        v |= E2_CMD_START;
        p->cmd_written = v;
        t0 = USEC_TIMER;
        *cmd = v;
    }
    else
    {
        st = E2_DMA3_STATUS;
        cmd = E2_DMA3_CMD;
        DMA_MASTER_CONTROL |= DMA_MASTER_CONTROL_EN;
        DMA3_PER_ADDR = dst;
        DMA3_RAM_ADDR = src;
        DMA3_FLAGS = p->flags;
        DMA3_INCR = p->incr;
        p->st_pre[0] = *st;
        v = DMA_CMD_RAM_TO_PER | DMA_CMD_SINGLE | (p->bytes - 4) |
            DMA_CMD_START;
        p->cmd_written = v;
        t0 = USEC_TIMER;
        *cmd = v;
    }
    if (p->work == E2LAB_WORK_POLL)
        rc = e2_poll(p, st, cmd, t0);
    else
    {
        volatile uint32_t *reg;
        uint32_t mask, want;

        e2_done_sig(p, &reg, &mask, &want);
        p->count = e2_spin(reg, mask, want, E2LAB_TIMEOUT_US,
                           p->work == E2LAB_WORK_SDRAM ? p->work_word : NULL,
                           o);
        p->us = o[0];
        p->iis_max_free = o[1];
        p->how = o[2] ? p->done_how : 0;
        rc = o[2] ? 0 : 1;
    }
    if (rc)
    {
        /* never ended: stopped, then the status twice */
        e2_stop(cmd);
        p->st_post[0] = *st;
        p->st_post[1] = *st;
        p->cmd_post = *cmd;
    }
    else
    {
        p->st_post[0] = *st;
        p->st_post[1] = *st;
        p->cmd_post = *cmd;
        if (p->cmd_post & (E2_CMD_INTR | E2_CMD_START))
            e2_stop(cmd);
    }
    /* an event the piece raised must be gone before a source comes back
     * on (the audio FIQ, or an IRQ with no handler, would take it):
     * Apple's acknowledge is a STATUS read, done twice above; once more */
    p->int_post = INT_STAT & E2_INT_BITS;
    if (p->int_post & ~p->int_pre)
    {
        (void)*st;
        p->int_post = INT_STAT & E2_INT_BITS;
    }
    raised = p->int_post & ~p->int_pre &
             ~(p->audio ? E2_INT_AUDIO : 0);   /* the audio's own DMA */
    if (en & ~raised)
        CPU_INT_EN = en & ~raised;
    e2_masked |= en & raised;   /* back at the restore, if it ever drops */
    if (raised && !rc)
        rc = 2;                 /* the caller stops the lab */
    if (p->port && rc == 0)
    {
        t0 = USEC_TIMER;
        if (!dt_until(&BCM_CONTROL, 0x2, true))
            rc = -2;
        p->ready_us = USEC_TIMER - t0;
    }
    p->rc = rc;
    return rc;
}
#endif /* HAVE_BCMFW_VIDEO && HAVE_BCMFW_DEVTEST */

#if defined(HAVE_BCMFW_VIDEO) && defined(HAVE_BCMFW_DEVTEST)
/* ---- P19 19-9's COP bench rows (bcm-coplab.h, cards C1-C3) ------------
 *
 * A worker of our own on the COP (firmware/export/cop_job.h), its jobs
 * posted with no cache flags, so the channel itself cleans or drops
 * nothing: every cache step is the row's own.  Every wait has a bound;
 * a job that never ends leaves the worker alone and says so (reboot).
 * The rows run with the audio stopped (the confirm screens stop it). */
#include "bcm-coplab.h"

#ifdef HAVE_COP_JOBS
#include "cop_job.h"

#define COP_STACK   2048

static struct cop_worker dt_cop SHAREDBSS_ATTR;
static int dt_cop_h;                /* its stack (buflib, pinned) */
static bool dt_cop_up, dt_cop_hung;

int bcm_cop_open(void)
{
    if (dt_cop_hung)
        return -4;
    if (dt_cop_up)
        return 0;
    dt_cop_h = core_alloc(COP_STACK);
    if (dt_cop_h <= 0)
    {
        dt_cop_h = 0;
        return -1;
    }
    core_pin(dt_cop_h);
    if (!cop_worker_start(&dt_cop, core_get_data(dt_cop_h), COP_STACK,
                          "coplab", PRIORITY_USER_INTERFACE) ||
        !cop_jobs_cross_core())
    {
        /* a thread may stay queued on a COP that never answered: its
           stack is kept (2 KB until the reboot), never freed under it */
        cop_worker_stop(&dt_cop);   /* no-op unless it started */
        dt_cop_hung = true;
        return -1;
    }
    dt_cop_up = true;
    return 0;
}

void bcm_cop_close(void)
{
    if (!dt_cop_up || dt_cop_hung)
        return;                 /* a hung job keeps its stack */
    cop_worker_stop(&dt_cop);
    core_unpin(dt_cop_h);
    core_free(dt_cop_h);
    dt_cop_h = 0;
    dt_cop_up = false;
}

bool bcm_cop_stuck(void)
{
    return dt_cop_hung;
}

/* fn(arg) on the COP, waited for up to COPLAB_JOB_MS (no cop_job_wait():
 * it has no bound).  0, -3 no worker, -4 never finished */
static int dt_cop_run(cop_job_fn fn, void *arg)
{
    long end;
    int t;

    if (!dt_cop_up || dt_cop_hung)
        return -3;
    t = cop_job_post(&dt_cop, fn, arg, 0);
    if (t < 0)
        return -3;
    end = current_tick + COPLAB_JOB_MS * HZ / 1000 + 1;
    while (!copj_is_done(&dt_cop.ring, t))
    {
        if (TIME_AFTER(current_tick, end))
        {
            dt_cop_hung = true;
            return -4;
        }
        semaphore_wait(&dt_cop.poster, HZ / 50);
    }
    copj_release(&dt_cop.ring, t);
    return 0;
}

/* ---- C1: one engine-2 piece, started and polled by whichever core runs
 * the job.  The job only programs the engine's registers (e2port_start,
 * _busy, _stop, _port_ready); the engine's owner flag (shared, under
 * its corelock since S0) is claimed by the caller's thread, and the
 * piece holds the port lock and keeps lcd_tick out for its whole run
 * (the port lock's rule 2, lcd-video.c) */
static bool dt_block_tick(void)
{
    int oldlevel = disable_irq_save();
    bool was;

#if NUM_CORES > 1
    corelock_lock(&lcd_state.cl);
#endif
    was = lcd_state.blocked;
    lcd_state.blocked = true;
#if NUM_CORES > 1
    corelock_unlock(&lcd_state.cl);
#endif
    restore_irq(oldlevel);
    return was;
}

static void dt_unblock_tick(bool was)
{
    int oldlevel = disable_irq_save();

#if NUM_CORES > 1
    corelock_lock(&lcd_state.cl);
#endif
    lcd_state.blocked = was;
#if NUM_CORES > 1
    corelock_unlock(&lcd_state.cl);
#endif
    restore_irq(oldlevel);
}
struct dt_e2_job
{
    uint32_t addr, bytes;
    const void *src;
    struct coplab_e2 r;
};
static struct dt_e2_job dt_e2j SHAREDBSS_ATTR;

static void dt_e2_job(void *arg)
{
    struct dt_e2_job *j = arg;
    uint32_t t0, polls = 0;

    j->r.core = CURRENT_CORE == CPU ? 0 : 1;
    t0 = USEC_TIMER;
    if (e2port_start(j->addr, j->src, j->bytes) < 0)
    {
        j->r.rc = -1;
        return;
    }
    j->r.start_us = USEC_TIMER - t0;
    t0 = USEC_TIMER;
    while (e2port_busy())
    {
        polls++;
        if (USEC_TIMER - t0 > COPLAB_E2_US)
        {
            e2port_stop();
            j->r.run_us = USEC_TIMER - t0;
            j->r.polls = polls;
            j->r.rc = 1;
            return;
        }
    }
    j->r.run_us = USEC_TIMER - t0;
    j->r.polls = polls;
    j->r.rc = e2port_port_ready() ? 0 : 2;
}

int bcm_cop_e2_piece(uint32_t bcm_addr, const void *src, uint32_t bytes,
                     bool on_cop, struct coplab_e2 *out)
{
    bool locked, blocked;
    uint32_t t0;
    int rc;

    memset(out, 0, sizeof(*out));
    if (!coplab_in_window(bcm_addr, bytes) || !e2port_fits(src, bytes))
        return out->rc = -1;
    if (on_cop && !dt_cop_up)
        return out->rc = -3;
    locked = lcd_port_lock();       /* the piece is one writer's */
    blocked = dt_block_tick();      /* and lcd_tick stays out (rule 2) */
    if (vdma_wait() || !e2port_claim(E2PORT_VIDEO))
    {
        dt_unblock_tick(blocked);
        lcd_port_unlock(locked);
        return out->rc = -1;
    }
    /* the engine reads memory: this core's dirty lines of src go first
       (e2port_start's own write-back is the running core's only) */
    commit_dcache();
    memset(&dt_e2j, 0, sizeof(dt_e2j));
    dt_e2j.addr = bcm_addr;
    dt_e2j.src = src;
    dt_e2j.bytes = bytes;
    t0 = USEC_TIMER;
    if (on_cop)
        rc = dt_cop_run(dt_e2_job, &dt_e2j);
    else
    {
        dt_e2_job(&dt_e2j);
        rc = 0;
    }
    dt_e2j.r.wall_us = USEC_TIMER - t0;
    if (rc == 0)
        *out = dt_e2j.r;
    else
        out->rc = rc;               /* stuck: the engine stays claimed */
    if (rc != -4)
        e2port_release(E2PORT_VIDEO);
    dt_unblock_tick(blocked);
    lcd_port_unlock(locked);
    return out->rc;
}

/* ---- C2: the cache trials ---------------------------------------------- */
enum { CJ_DROP, CJ_WRITER, CJ_READ };

struct dt_cache_job
{
    uint32_t *line;
    int op;
    uint32_t v1, v9;
    uint32_t out0, out1;
};
static struct dt_cache_job dt_cj SHAREDBSS_ATTR;

/* The steps, on whichever core runs them; c the cached word, u its
 * uncached alias */
static void dt_cache_op(struct dt_cache_job *j)
{
    volatile uint32_t *c = j->line;
    volatile uint32_t *u = (volatile uint32_t *)UNCACHED_ADDR(j->line);

    switch (j->op)
    {
    case CJ_DROP:
        commit_discard_dcache();
        break;
    case CJ_WRITER:
        j->out0 = *c;           /* held, so the write below hits */
        *c = j->v1;
        j->out1 = *u;           /* memory: v0 while the write is cached */
        *u = j->v9;             /* memory changed under both caches */
        break;
    case CJ_READ:
        j->out0 = *c;
        break;
    }
}

static void dt_cache_job(void *arg)
{
    dt_cache_op(arg);
}

static int dt_cache_step(bool on_cop, int op)
{
    dt_cj.op = op;
    if (on_cop)
        return dt_cop_run(dt_cache_job, &dt_cj);
    dt_cache_op(&dt_cj);
    return 0;
}

int bcm_cop_cache_trial(uint32_t *line, bool cpu_writes, uint32_t seq,
                        struct coplab_cache_trial *t)
{
    volatile uint32_t *u = (volatile uint32_t *)UNCACHED_ADDR(line);
    bool w_cop = !cpu_writes, r_cop = cpu_writes;
    int rc;

    if (((uintptr_t)line & 15) || (uintptr_t)line >= UNCACHED_BASE_ADDR)
        return -1;
    if (!dt_cop_up)
        return -3;
    coplab_cache_values(t, seq);
    memset(&dt_cj, 0, sizeof(dt_cj));
    dt_cj.line = line;
    dt_cj.v1 = t->v1;
    dt_cj.v9 = t->v9;
    /* 1: both caches clean and empty of it (before v0 goes to memory: a
       dirty line left from the buffer's earlier use would land over it),
       then the reader holds the line */
    if ((rc = dt_cache_step(w_cop, CJ_DROP)) ||
        (rc = dt_cache_step(r_cop, CJ_DROP)))
        return rc;
    *u = t->v0;
    if ((rc = dt_cache_step(r_cop, CJ_READ)))
        return rc;
    t->pre = dt_cj.out0;
    /* 2: the writer's cached write, memory checked, then changed */
    if ((rc = dt_cache_step(w_cop, CJ_WRITER)))
        return rc;
    t->u1 = dt_cj.out1;
    /* 3: the reader's cached view */
    if ((rc = dt_cache_step(r_cop, CJ_READ)))
        return rc;
    t->r = dt_cj.out0;
    /* 4: memory still v9 (the writer's line not written back) */
    t->p = *u;
    /* the cache state put back: both clean and empty of the line */
    if ((rc = dt_cache_step(true, CJ_DROP)))
        return rc;
    commit_discard_dcache();
    return 0;
}

/* ---- C3: the COP's sleep while a thread there waits -------------------- */
struct dt_sleep_job
{
    int how;
    volatile uint32_t stop;
    uint32_t limit_us, loops;
    struct semaphore sem;
};
static struct dt_sleep_job dt_sj SHAREDBSS_ATTR;

static void dt_sleep_job(void *arg)
{
    struct dt_sleep_job *j = arg;
    uint32_t t0 = USEC_TIMER;

    j->loops = 0;
    switch (j->how)
    {
    case COPLAB_W_YIELD:
        while (!j->stop && USEC_TIMER - t0 < j->limit_us)
        {
            yield();
            j->loops++;
        }
        break;
    case COPLAB_W_SLEEP1:
        while (!j->stop && USEC_TIMER - t0 < j->limit_us)
        {
            sleep(1);
            j->loops++;
        }
        break;
    default:
        semaphore_wait(&j->sem, (int)(j->limit_us / (1000000 / HZ)) + 1);
        j->loops = 1;
        break;
    }
}

#define COP_SAMPLE_US 20

int bcm_cop_sleep_window(int how, uint32_t window_ms, struct coplab_sleep *s)
{
    uint32_t t0, next, now, win = window_ms * 1000;
#ifdef HAVE_DEVSTATS
    uint32_t idle0;
#endif
    int t, rc;
    long end;

    coplab_sleep_init(s);
    if (!dt_cop_up || dt_cop_hung)
        return -3;
    if (how < 0 || how >= COPLAB_W_N || window_ms == 0 || window_ms > 5000)
        return -1;
    memset(&dt_sj, 0, sizeof(dt_sj));
    dt_sj.how = how;
    dt_sj.limit_us = win + 500000;  /* the job's own bound */
    semaphore_init(&dt_sj.sem, 1, 0);
    t = cop_job_post(&dt_cop, dt_sleep_job, &dt_sj, 0);
    if (t < 0)
        return -3;
    sleep(HZ / 20);                 /* the job under way, settled */
#ifdef HAVE_DEVSTATS
    idle0 = devstats.idle_us[COP];
#endif
    /* this thread samples the whole window and yields to nobody: the
       COP's state is all that changes (the tick still wakes it) */
    t0 = next = USEC_TIMER;
    while ((now = USEC_TIMER) - t0 < win)
    {
        if (now - next < 0x80000000u)
        {
            coplab_sleep_add(s, (COP_CTL & PROC_SLEEP) != 0);
            next += COP_SAMPLE_US;
        }
    }
    s->window_us = USEC_TIMER - t0;
#ifdef HAVE_DEVSTATS
    s->idle_us = devstats.idle_us[COP] - idle0;
#endif
    dt_sj.stop = 1;
    if (how == COPLAB_W_BLOCK)
        semaphore_release(&dt_sj.sem);
    end = current_tick + COPLAB_JOB_MS * HZ / 1000 + 1;
    rc = 0;
    while (!copj_is_done(&dt_cop.ring, t))
    {
        if (TIME_AFTER(current_tick, end))
        {
            dt_cop_hung = true;
            return -4;
        }
        semaphore_wait(&dt_cop.poster, HZ / 50);
    }
    copj_release(&dt_cop.ring, t);
    s->loops = dt_sj.loops;
    return rc;
}

#else /* !HAVE_COP_JOBS: no second core's worker in this build */
int bcm_cop_open(void) { return -1; }
void bcm_cop_close(void) { }
bool bcm_cop_stuck(void) { return false; }
int bcm_cop_e2_piece(uint32_t bcm_addr, const void *src, uint32_t bytes,
                     bool on_cop, struct coplab_e2 *out)
{
    (void)bcm_addr; (void)src; (void)bytes; (void)on_cop;
    memset(out, 0, sizeof(*out));
    return out->rc = -3;
}
int bcm_cop_cache_trial(uint32_t *line, bool cpu_writes, uint32_t seq,
                        struct coplab_cache_trial *t)
{
    (void)line; (void)cpu_writes; (void)seq; (void)t;
    return -3;
}
int bcm_cop_sleep_window(int how, uint32_t window_ms, struct coplab_sleep *s)
{
    (void)how; (void)window_ms;
    coplab_sleep_init(s);
    return -3;
}
#endif /* HAVE_COP_JOBS */
#endif /* HAVE_BCMFW_VIDEO && HAVE_BCMFW_DEVTEST */
