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

/* The BCM2722 job driver on the device (P19 W0-6, HAVE_BCM_JOBS): the
 * port, power and restart for bcmjob-core.c, the one port lock, and the
 * shell's set-up.  #included at the end of lcd-video.c after the device
 * test's HAL (it needs lcd_state, lcd_tick, the bounded port calls dt_*,
 * lcd_e2_push and the boot path's statics); nothing else compiles it.
 *
 * When jobs may run: our resident image is the display driver
 * (bcm_boot_resident_active()), it is up and nobody owns it (no developer
 * stage, no video session: they suspend the client), it has not stopped,
 * and at boot its FNV-1a was in bcmfw/verified.txt or the developer marker
 * file below exists.  Otherwise every call answers OFF without touching
 * the port (C0 included). */

#include "bcmjob-core.h"
#include "power.h"
#include "button.h"         /* button_queue_post: the owner's kick */

#ifdef HAVE_BCMFW_DEVTEST
#define BCMJOB_DEV_MARKER ROCKBOX_DIR "/bcmjobs.on"   /* developer: unverified ok */
#endif
#define J_SURF_BACK_ADDR  0x105800  /* the second frame (memory-contract.md) */

/* written once by bcmjob_init(), read from either core (the player's
 * take, the art thread, the owner): uncached (P19 S0; 19-9 moves the
 * owner) */
static bool jd_ready XCORE_BSS, jd_allowed XCORE_BSS;
static struct bjc_slot *jd_slots XCORE_BSS; /* the job client's allocation */
static void (*jd_trace)(const char *line) XCORE_BSS;
/* the lock's depth and whether the mutex was taken: written by the lock's
 * holder only, on either core (P19 S0: uncached) */
static int bcm_port_depth XCORE_BSS;
static bool bcm_port_mtx XCORE_BSS;
/* P19 S0 (MF-2): the request record, its acknowledgement and the owner */
static struct bjc_req jd_req XCORE_BSS;
static struct semaphore jd_ack XCORE_BSS;
static struct semaphore jd_art XCORE_BSS;   /* the decode caller's wake */
static unsigned int jd_owner XCORE_BSS;     /* the owner's thread id */
static unsigned int jd_owner_core XCORE_BSS; /* ... and its core (S10) */

/* ---- the one port lock (gap G3): lcd-video.c's mutex and the tick's
 * corelock, recursive per thread, never a second lock ------------------- */
void bcm_port_lock(void)
{
    bool m = lcd_port_lock();   /* recursive for its owner; other threads wait */

    if (bcm_port_depth++ == 0)
    {
        bcm_port_mtx = m;
        lcd_block_tick();       /* the tick's reads of the command word */
    }
}

void bcm_port_unlock(void)
{
    bool m = bcm_port_mtx;

    if (--bcm_port_depth == 0)
    {
        int oldlevel = disable_irq_save();

        corelock_lock(&lcd_state.cl);
        lcd_state.blocked = false;  /* a pending update goes at the next tick */
        corelock_unlock(&lcd_state.cl);
        restore_irq(oldlevel);
    }
    lcd_port_unlock(m);
}

/* ---- the port ------------------------------------------------------------ */
/* dt_wr_block takes any count from any halfword-aligned source (an
 * unaligned row word by word under one address, an odd last halfword by
 * a read-modify-write: bcm-port-block.h) */
static int jd_wr_block(uint32_t addr, const uint16_t *src, uint32_t n)
{
    return dt_wr_block(addr, src, n);
}

static uint32_t jd_upload(uint32_t addr, const uint16_t *src, uint32_t bytes)
{
    return lcd_e2_push(addr, (const fb_data *)src, bytes);  /* lock held */
}

/* a DECODE ring piece (8 KB at most, whole 64-byte units): the engine
 * at any size the core hands it; the core keeps small pieces on the CPU */
static uint32_t jd_upload_any(uint32_t addr, const void *src, uint32_t bytes)
{
    return lcd_e2_push_min(addr, src, bytes, 4);    /* lock held */
}

/* A pinned allocation's data rounded up to a cache line, bytes long:
 * both ends on a line, so no line is shared with buflib's header or a
 * neighbour (xcore.h rule 3; P19 S10) */
static void *jd_lines(int h)
{
    uintptr_t p = (uintptr_t)core_get_data(h);

    return (void *)((p + XCORE_LINE - 1) & ~(uintptr_t)(XCORE_LINE - 1));
}

/* one decode's piece buffer: from the core allocator, pinned while it
 * streams (yield() may let others allocate), freed after: nothing kept
 * between decodes.  On cache lines at both ends (BCMJOB_DEC_PIECE is a
 * multiple of 16): with the owner on the other core, the art thread
 * publishes each piece and engine 2 or the owner's alias reads it
 * (bcmjob-core.c, S10).  A call whose give-up was never acknowledged
 * keeps it pinned (bcmjob-core.c returns it later); never two out */
static int jd_dec_h;
static void *jd_dec_buf(bool get)
{
    if (!get)
    {
        if (jd_dec_h > 0)
        {
            core_unpin(jd_dec_h);
            core_free(jd_dec_h);
        }
        jd_dec_h = 0;
        return NULL;
    }
    jd_dec_h = core_alloc(BCMJOB_DEC_PIECE + XCORE_LINE);
    if (jd_dec_h <= 0)
    {
        jd_dec_h = 0;
        return NULL;
    }
    core_pin(jd_dec_h);
    return jd_lines(jd_dec_h);
}

static uint32_t jd_now_us(void) { return USEC_TIMER; }
static uint32_t jd_now_ms(void) { return current_tick * (1000 / HZ); }
static void jd_yield(void) { yield(); }

static uint32_t jd_gen(void)
{
    if (!jd_allowed || bcm_devtest_owned || bcmfw_hung || !lcd_state.display_on ||
        !(GPO32_VAL & 0x4000) || !bcm_boot_resident_active())
        return 0;
    return bcmjob_gen ? bcmjob_gen : 1;
}

static bool jd_lit(void) { return lcd_state.display_on; }

/* bcmjob_pp.h: the screen-truth stamp (owned rects die with a power-up) */
uint32_t bcmjob_power_gen(void) { return jd_gen(); }

static int jd_push(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    lcd_update_rect(x, y, w, h);    /* the LCD contract: today's push */
    return 0;
}

/* G2: a dead BCM is cut and our image started again at once; its first
 * frame is the PP framebuffer (lcd_awake's first update).  The panel is
 * dark for the restart, with no text.  Not a strike, no rung down: the
 * image did not fail to boot, it stopped; the ladder still takes over if
 * the restart itself fails (bcm_init). */
static void jd_restart(void)
{
    if (!lcd_state.display_on || bcm_devtest_owned)
        return;
#ifdef HAVE_PP_GOV_DVS
    pgov_port_cap(true);        /* off 100 MHz: this owner drives the port
                                 * without lcd_awake() (review S4) */
#endif
    bcm_devtest_owned = true;   /* lcd_awake()/lcd_sleep() step aside */
    lcd_state.display_on = false;
    tick_remove_task(&lcd_tick);
    _backlight_hw_enable(false);
    GPO32_VAL &= ~0x4000;
    sleep(HZ/10);
    lcd_state.update_timeout = current_tick - HZ;
    lcd_awake_body();
    bcm_devtest_owned = false;
    dt_light(true);
    bcmfw_note("jobs: the BCM stopped; restarted");
}

/* A dark want (G11): the BCM and panel up with the backlight off, or cut
 * again unless the screen came on meanwhile */
static int jd_power(bool up)
{
    if (up)
    {
        lcd_awake();
        return lcd_state.display_on ? 0 : -1;
    }
    if (!is_backlight_on(true))
        lcd_sleep();
    return 0;
}

static bool jd_charging(void) { return charger_inserted(); }

static const struct bjc_port jd_port =
{
    .rd = dt_rd, .wr = dt_wr, .wr_block = jd_wr_block, .upload = jd_upload,
    .doorbell = dt_doorbell, .lock = bcm_port_lock, .unlock = bcm_port_unlock,
    .now_us = jd_now_us, .now_ms = jd_now_ms, .yield = jd_yield,
    .gen = jd_gen, .lit = jd_lit, .push = jd_push, .restart = jd_restart,
    .power = jd_power, .charging = jd_charging,
    .upload_any = jd_upload_any, .dec_buf = jd_dec_buf,
};

/* lcd_sleep / lcd_awake: accounting only (backlight thread); the dark
 * record is that thread's, the owner reads it (no wake) */
static void jd_dark(bool dark)
{
    if (jd_ready)
        bjc_dark(dark);
}

/* ---- the owner (P19 S0, MF-2) ---------------------------------------------
 * The ring owner is the thread that ran bcmjob_init() (the shell's UI
 * thread).  A request from another thread posts SYS_BCMJOB_REQ to
 * button_queue, which the owner's input pump answers with bcmjob_apply(). */
static bool jd_is_owner(void) { return thread_self() == jd_owner; }
static void jd_kick(void) { button_queue_post(SYS_BCMJOB_REQ, 0); }
static int jd_wait(uint32_t ms)
{
    return semaphore_wait(&jd_ack, (ms * HZ + 999) / 1000 + 1) ==
           OBJ_WAIT_TIMEDOUT ? -1 : 0;
}
static void jd_acked(void) { semaphore_release(&jd_ack); }
static void jd_mode(bool jobs) { bcmjob_on = jobs ? bcmjob_gen + 1 : 0; }
/* bcmjob_decode()/bcmjob_read() on the art thread: it sleeps until the
 * owner asks for a piece or ends the call, or a tick passes (then it
 * kicks the owner again, so the owner's passes poll the BCM) */
static void jd_art_wait(uint32_t ms)
{
    semaphore_wait(&jd_art, (ms * HZ + 999) / 1000);
}
static void jd_art_wake(void) { semaphore_release(&jd_art); }

/* P19 S10, the decode's hand-off across cores (bcmjob-core.h): true on a
 * caller running on the other core than the owner.  Today's build has the
 * owner and the art thread both on the CPU: false, and none of the
 * hand-off runs.  The PP5022 has no ranged maintenance, so publish and
 * acquire are the whole-cache helpers of xcore.h */
static bool jd_cross(void)
{
#if NUM_CORES > 1
    return CURRENT_CORE != jd_owner_core;
#else
    return false;
#endif
}
static void jd_publish(const void *p, uint32_t n)
{
    (void)p;
    (void)n;
    xcore_publish();
}
static void jd_acquire(void *p, uint32_t n)
{
    (void)p;
    (void)n;
    xcore_acquire();
}
/* the uncached alias of cached SDRAM; anything else (IRAM) as is */
static void *jd_uncached(void *p)
{
#if NUM_CORES > 1 && defined(UNCACHED_BASE_ADDR)
    if ((uintptr_t)p < UNCACHED_BASE_ADDR)
        return UNCACHED_ADDR(p);
#endif
    return p;
}

static const struct bjc_owner jd_own =
{
    .req = &jd_req, .is_owner = jd_is_owner, .kick = jd_kick,
    .wait = jd_wait, .ack = jd_acked, .mode = jd_mode,
    .art_wait = jd_art_wait, .art_wake = jd_art_wake,
    .cross = jd_cross, .publish = jd_publish, .acquire = jd_acquire,
    .uncached = jd_uncached,
};

void bcmjob_decode_stop(void)
{
    if (jd_ready)
        bjc_decode_stop();
}

void bcmjob_frame_due_hook(bool (*fn)(void))
{
    bjc_frame_due(fn);          /* kept until the next bjc_init(): none */
}

void bcmjob_apply(void)
{
#ifdef BCM_WAIT_BOUNDS
    bcm_wait_faults_log();      /* lcd-video.c's waits that ran out */
#endif
    if (jd_ready)
        bjc_apply();
}

#ifdef HAVE_BCMFW_DEVTEST
/* bcmjob_pp.h: the bench's forced job failure (bjc_inject_fail), from the
 * owner's thread only */
int bcmjob_dev_inject_fail(uint32_t op, uint32_t n)
{
    if (!jd_ready || !jd_is_owner())
        return -1;
    bjc_inject_fail(op, n);
    if (!n)
        bjc_inject_forgive(op);     /* "off": the op is forgiven too */
    return 0;
}
#endif

/* ---- the overlays (#126; jobs.md section 5, "Overlays") --------------------
 * An overlay file from the system folder, paired with the image it was
 * linked against (bcmjob_pp.h, the kinds): the visualizer's viz-ovl-m.bin
 * and DECODE's dec-ovl.bin with the merged video image (mkvideo.py writes
 * all three and the records, the caller's: bcmvideo_image.h), and in a
 * developer build the visualizer's viz-ovl.bin with the display-only
 * image this firmware embeds (bcm-devtest-images.c, one bcmfw build:
 * mkdevtest.py writes both).  make zip refuses a mismatched pair
 * (bcmfw/ovlpair.py).  The load is split by thread (the freeze audit,
 * 2026-10-26: no file read on the UI thread): the art thread reads a
 * piece into its own buffer (bcmjob_ovl_read), the job owner puts it to
 * the BCM (bcmjob_ovl_put: the port, no file), FNV-1a and F16w summed
 * piece by piece.  Only the record that names the running image's region
 * is ever due (bjc_ovl_due), so the other image's file is never opened.
 * The merged image's two kinds share its one region: a load evicts the
 * other (bcmjob-core.c bjc_ovl_begin), whose load is due again at once. */
static void jd_line(const char *why)
{
    char line[64];

    if (jd_trace)
    {
        snprintf(line, sizeof(line), "bcmjob overlay refused why=%s", why);
        jd_trace(line);
    }
}

static void jd_rec(struct bjc_ovl_rec *rec, const struct bcmjob_ovl_file *f)
{
    rec->bytes = f->bytes;
    rec->fnv = f->fnv;
    rec->abi = f->abi;
    rec->abi_at = f->abi_at;
    rec->base = f->base;
}

/* the developer build's display-only image: its embedded record of
 * viz-ovl.bin (no record in a release: never) */
static bool jd_viz_dt(const struct bcmjob_ovl_file *f, struct bcmjob_ovl_file *out)
{
#ifdef HAVE_BCMFW_DEVTEST
    if (out)
    {
        out->bytes = bcmdt_viz_ovl.bytes;
        out->fnv = bcmdt_viz_ovl.fnv;
        out->abi = bcmdt_viz_ovl.abi;
        out->abi_at = bcmdt_viz_ovl.abi_at;
        out->base = bcmdt_viz_ovl.base;
    }
    return !f || (f->base == bcmdt_viz_ovl.base && f->fnv == bcmdt_viz_ovl.fnv);
#else
    (void)f;
    (void)out;
    return false;
#endif
}

_Static_assert((int)BCMJOB_OVL_VIZ == (int)BJC_OVL_VIZ &&
               (int)BCMJOB_OVL_DEC == (int)BJC_OVL_DEC,
               "bcmjob_pp.h's overlay kinds are the core's instances");
_Static_assert(BCMJOB_OVL_AGAIN == BJC_OVL_AGAIN && BCMJOB_OVL_AGAIN > 0,
               "bcmjob_pp.h's AGAIN is the core's, apart from every BCMJOB_*");

static bool jd_kind_ok(int kind)
{
    return kind == BCMJOB_OVL_VIZ || kind == BCMJOB_OVL_DEC;
}

/* the ops a kind adds, all offered: loaded */
static bool jd_ovl_have(int kind)
{
    return kind == BCMJOB_OVL_DEC ? bcmjob_have(J_OP_DECODE)
                                  : bcmjob_have(J_OP_SPECTRUM) &&
                                    bcmjob_have(J_OP_BARS);
}

bool bcmjob_ovl_due(int kind, struct bcmjob_ovl_file *f)
{
    struct bcmjob_ovl_file dt;
    struct bjc_ovl_rec rec;
    uint32_t region;

    if (!jd_ready || !jd_kind_ok(kind) || !f)
        return false;
    region = bjc_ovl_region();
    if (!region)
        return false;
    /* the visualizer's: the merged image's file, or the display-only
       image's when that one runs (the records' regions differ); the
       record named only for the image that runs, so a refusal's state
       stands (bjc_ovl_set keeps it for the same record) */
    if (kind == BCMJOB_OVL_VIZ && f->base != region && jd_viz_dt(NULL, &dt) &&
        dt.base == region)
        *f = dt;
    if (!f->bytes || f->base != region)
        return false;
    jd_rec(&rec, f);
    bjc_ovl_set(kind, &rec);
    return bjc_ovl_due(kind);
}

/* bcmjob_pp.h: the file's side (the art thread): one piece of the file
 * into the caller's buffer.  Open, seek, read, close: the owner's piece
 * upload comes between two reads */
int bcmjob_ovl_read(int kind, void *buf, uint32_t off, uint32_t n, uint32_t *size,
                    const struct bcmjob_ovl_file *f)
{
    static bool missing[3];             /* each file logged once a boot */
    static const char *const path[3] = {
        ROCKBOX_DIR "/dec-ovl.bin", ROCKBOX_DIR "/viz-ovl-m.bin",
        ROCKBOX_DIR "/viz-ovl.bin"
    };
    int fd, got, w;

    if (!jd_ready || !jd_kind_ok(kind) || !f || !buf || ((uintptr_t)buf & 3) ||
        (n & 3))
        return -1;
    w = kind == BCMJOB_OVL_DEC ? 0 : jd_viz_dt(f, NULL) ? 2 : 1;
    fd = open(path[w], O_RDONLY);
    if (fd < 0)
    {
        if (!missing[w])
            jd_line("nofile");
        missing[w] = true;
        return -1;
    }
    *size = ffilesize(fd);
    if (off >= *size)
    {
        close(fd);
        jd_line("read");
        return -1;
    }
    n = MIN(n, *size - off);
    got = lseek(fd, off, SEEK_SET) == (off_t)off ? read(fd, buf, n) : -1;
    close(fd);
    if (got != (int)n)
    {
        jd_line("read");
        return -1;
    }
    return got;
}

/* ... the owner's side: the pieces in order, the sums as they come (the
 * owner's alone, as b); one load at a time, of either kind.  pend: the
 * core's posted wait (bcmjob-core.h BJC_OVL_AGAIN) the caller asks again
 * for: JD_PEND_BEGIN, the first piece's FENCE (the same piece is put
 * again); JD_PEND_END, the last piece's OVERLAY reply (asked again with
 * the same piece, or by bcmjob_ovl_poll(), which needs no buffer) */
enum { JD_PEND_NONE = 0, JD_PEND_BEGIN, JD_PEND_END };
static struct
{
    uint32_t next, fnv, a, s;
    int kind;
    bool on;                    /* a load begun and not ended */
    uint8_t pend;
} jd_ol;

static int jd_end(int kind)
{
    int r = bjc_ovl_end(kind, jd_ol.fnv, jd_ol.a, jd_ol.s);

    if (r == BJC_OVL_AGAIN)
    {
        jd_ol.pend = JD_PEND_END;
        jd_ol.kind = kind;
        return BCMJOB_OVL_AGAIN;
    }
    jd_ol.pend = JD_PEND_NONE;
    if (r != BCMJOB_OK)
        return r;
    return jd_ovl_have(kind) ? BCMJOB_OK : BCMJOB_NOJOB;
}

int bcmjob_ovl_poll(void)
{
    if (!jd_ready || !jd_is_owner() || jd_ol.pend == JD_PEND_NONE)
        return BCMJOB_NOJOB;
    if (jd_ol.pend == JD_PEND_END)
        return jd_end(jd_ol.kind);
    /* the first piece's FENCE: without the piece the load can't go on;
       the next load begins afresh (the core ends the posted wait) */
    jd_ol.pend = JD_PEND_NONE;
    jd_ol.on = false;
    return BCMJOB_OFF;
}

int bcmjob_ovl_put(int kind, const void *buf, uint32_t off, uint32_t n,
                   uint32_t size, const struct bcmjob_ovl_file *f)
{
    const uint8_t *p = buf;
    struct bjc_ovl_rec rec;
    uint32_t i;
    int r;

    if (!jd_ready || !jd_is_owner() || !jd_kind_ok(kind) || !f || !f->bytes ||
        !buf || ((uintptr_t)buf & 3) || (n & 3) || !n)
        return BCMJOB_OFF;
    if (jd_ol.pend == JD_PEND_END && jd_ol.kind == kind && off + n == size &&
        jd_ol.next == size)
        return jd_end(kind);            /* the last piece's answer */
    if (jd_ol.pend == JD_PEND_END)
        jd_ol.pend = JD_PEND_NONE;      /* another load: the core ends it */
    if (jd_ovl_have(kind))
    {
        jd_ol.on = false;
        jd_ol.pend = JD_PEND_NONE;
        return BCMJOB_OK;               /* loaded already */
    }
    if (off == 0)
    {
        if (jd_ol.pend != JD_PEND_BEGIN || jd_ol.kind != kind)
        {
            jd_rec(&rec, f);
            jd_ol.on = false;
            bjc_ovl_set(kind, &rec);
            if (!bjc_ovl_due(kind))
                return BCMJOB_NOJOB;
            if (n < 4 * BJC_OVL_HDR_WORDS)
            {
                jd_line("read");
                return BCMJOB_FAILED;
            }
        }
        r = bjc_ovl_begin(kind, buf, size);
        jd_ol.pend = r == BJC_OVL_AGAIN ? JD_PEND_BEGIN : JD_PEND_NONE;
        jd_ol.kind = kind;
        if (r == BJC_OVL_AGAIN)
            return BCMJOB_OVL_AGAIN;    /* the same piece, a later pass */
        if (r != BCMJOB_OK)
            return r;
        jd_ol.on = true;
        jd_ol.kind = kind;
        jd_ol.next = 0;
        jd_ol.fnv = 2166136261u;
        jd_ol.a = jd_ol.s = 0;
    }
    if (!jd_ol.on || jd_ol.kind != kind || off != jd_ol.next || off + n > size)
    {
        jd_ol.on = false;
        jd_ol.pend = JD_PEND_NONE;
        jd_line("read");                /* out of order: the load ends */
        return BCMJOB_FAILED;
    }
    for (i = 0; i < n; i++)
        jd_ol.fnv = (jd_ol.fnv ^ p[i]) * 16777619u;
    j_f16w(&jd_ol.a, &jd_ol.s, buf, n / 2);
    r = bjc_ovl_piece(kind, off, buf, n);
    if (r != BCMJOB_OK)
    {
        jd_ol.on = false;
        return r;
    }
    jd_ol.next = off + n;
    if (jd_ol.next < size)
        return BCMJOB_OK;
    jd_ol.on = false;
    return jd_end(kind);
}

bool bcmjob_dec_lends(void)
{
    return jd_ready && bjc_dec_lends();
}

/* the device test's take and restore (review M4): false when the owner
 * never acknowledged a suspend (the take must refuse; jobs.md section 6) */
static bool jd_suspend(bool on)
{
    if (!jd_ready)
        return true;
    return bjc_suspend_request(on, BJC_ACK_MS) == BCMJOB_OK;
}

/* ---- bcmjob_pp.h ----------------------------------------------------------- */
void bcmjob_init(void (*trace)(const char *line))
{
    char line[64];
    int h;
    bool verified;

#ifdef BCM_WAIT_BOUNDS
    bw_trace = trace;           /* lcd-video.c's FAULT lines, jobs or not */
#endif
    if (jd_ready || !bcmjob_cpu_init())
        return;
    /* the owner's (S10: on its own cache lines, as the core's b and j) */
    h = core_alloc(BJC_SLOTS * sizeof(struct bjc_slot) + XCORE_LINE);
    if (h <= 0)
        return;
    core_pin(h);
    jd_slots = jd_lines(h);
    memset(jd_slots, 0, BJC_SLOTS * sizeof(struct bjc_slot));
    verified = bcm_boot_resident_active() && bcmfw_res_verified();
#ifdef HAVE_BCMFW_DEVTEST
    jd_allowed = bcm_boot_resident_active() &&
                 (verified || file_exists(BCMJOB_DEV_MARKER));
#else
    jd_allowed = verified;      /* a release ignores the developer marker */
#endif
    bjc_init(&jd_port, jd_slots, trace);
    semaphore_init(&jd_ack, 1, 0);
    semaphore_init(&jd_art, 1, 0);
    jd_owner = thread_self();
    jd_owner_core = CURRENT_CORE;
    bjc_set_owner(&jd_own);
    jd_trace = trace;
    jd_ready = true;
    if (!lcd_state.display_on)
        bjc_dark(true);
    /* S10: the driver's init-time state (b, r, own, the slots) written
       back, for an owner or a caller on the other core */
    xcore_publish();
    if (trace)
    {
        snprintf(line, sizeof(line), "bcmjob init allowed=%d verified=%d",
                 jd_allowed, verified);
        trace(line);
    }
}

#ifdef HAVE_BCMFW_VIDEO
/* bcmjob_pp.h: the chip failures the job driver saw this boot (the video
   player's first-use check) */
uint32_t bcmjob_chip_failures(void) { return bjc_chip_failures(); }
#endif

/* bcmjob_pp.h: why jobs can't run now, the gates jd_gen() and the self-test
 * check, in their order; NULL: they can */
const char *bcmjob_off_why(bool *restart)
{
    *restart = true;            /* a new start brings the driver up afresh */
    if (!jd_ready)
        return "the job driver did not start";
    if (!bcm_boot_resident_active())
        return "the display program isn't running in this start";
    if (bcmfw_hung)
        return "the video chip stopped";
    *restart = false;
    if (!jd_allowed)
    {
#ifdef HAVE_BCMFW_DEVTEST
        /* the gate is read at boot: a marker made since (the bench
           session's switches) counts from the next start */
        if (file_exists(BCMJOB_DEV_MARKER))
        {
            *restart = true;
            return "jobs not allowed in this start: the developer marker "
                   "came after it";
        }
#endif
        return "jobs not allowed: the image is not verified and no "
               "developer marker";
    }
    if (bcm_devtest_owned)
        return "a developer test holds the video chip";
    if (!lcd_state.display_on || !(GPO32_VAL & 0x4000))
        return "the screen is off";
    *restart = true;
    if (!bcmjob_have(J_OP_FILL))
        return "the video chip offers no jobs (no fill)";
    return NULL;
}

int bcmjob_selftest(void)
{
    uint32_t fill[4] = { J_SURF_BACK, 0, J_XY(32, 16), 0x07E0 }, seq, v = 0;
    int r;

    if (!jd_ready || !bcmjob_have(J_OP_FILL))
    {
        if (jd_trace)
            jd_trace("bcmjob selftest unavailable");
        return 1;
    }
    r = bcmjob_submit(J_OP_FILL, 1, fill, 4);
    if (!r)
        r = bcmjob_fence(&seq);
    if (!r)
        r = bcmjob_wait(seq, J_T_FENCE_BASE + J_T_DRAW);
    if (!r)
    {
        bcm_port_lock();
        if (dt_rd(J_SURF_BACK_ADDR, &v))
            r = BCMJOB_OFF;
        bcm_port_unlock();
        if (!r && v != (0x07E0u | 0x07E0u << 16))
            r = BCMJOB_FAILED;
    }
    if (jd_trace)
    {
        char line[48];

        snprintf(line, sizeof(line), r ? "bcmjob selftest failed rc=%d"
                                       : "bcmjob selftest ok", r);
        jd_trace(line);
    }
    return r;
}

/* bcmjob_pp.h: the ring's owner is the calling thread from now on (19-9:
 * the UI thread that runs ipodui_main(), wherever it was started).  Call
 * it from the new owner, before its first bcmjob call and before any
 * other thread posts a request; bcmjob_init() names its caller, so a
 * build that never moves the owner needs no call. */
void bcmjob_claim_owner(void)
{
    /* S10: an owner on the other core drops any stale line of the
       driver's state, which bcmjob_init() wrote (and published) there */
    xcore_acquire();
    jd_owner_core = CURRENT_CORE;
    jd_owner = thread_self();
}
