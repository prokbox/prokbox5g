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
 * The PP5022 band governor's firmware glue (docs/design/pp-governor.md,
 * increment G1): the CPU's tick samples both cores' idle time every
 * 10 ms and runs the policy (pgov_core.c); a changed decision kicks the
 * power thread, which applies it under the boost lock (system.c).  Off
 * by default: the developer switch (the shell's marker file) turns it on,
 * and off is today's code path exactly.
 * Stage 2 (section 16, HAVE_PP_GOV_DVS): a second policy instance with
 * band 1 (30 MHz at DCDC1 0xEB), band 0 at 0xEB and band 3 (100 MHz)
 * runs on the same samples.  Shadow (marker pgov2_on): its decisions and
 * time per band are logged, nothing is applied.  Apply (marker
 * pgov2_apply as well): the power thread applies them in order (voltage
 * up, settle, clock up; clock down, voltage down) through pgov_hw_next(),
 * every clock change is clamped to what the rail carries (system.c), and
 * a raise not confirmed in 250 ms is done by the tick over the raw PMU
 * path, after which stage 1 is applied again for the rest of the boot.
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
#include "config.h"
#include "system.h"
#include "cpu.h"
#include "kernel.h"
#include "powermgmt.h"
#include "pgov.h"
#include "xcore.h"           /* XCORE_BSS: state both cores read */
#ifdef HAVE_PP_GOV_DVS
#include <string.h>
#include "pcf5060x.h"
#include "pcf50605.h"
#include "i2c-pp.h"         /* i2c_lock() */
#include "power_sleep.h"    /* pmu_raw_core_voltage() */
#include "usb.h"
#include "file.h"
#include "crc32.h"
#include "rbpaths.h"
#include "ata_idle_notify.h"
#endif

/* Per core, one writer each (core_sleep(), thread-pp.c); uncached */
uint32_t pgov_idle_us[2] SHAREDBSS_ATTR;
uint32_t pgov_sleep_t0[2] SHAREDBSS_ATTR;   /* nonzero while asleep */

/* Review B1: the decisions are read by a boost on either core (the COP
 * under FE_COP) and written only on the CPU (its tick, its threads): so
 * they live uncached, and a COP boost books its change for the CPU's tick
 * (cop_boost) instead of writing them */
static struct pgov_core core XCORE_BSS;
static volatile uint8_t cop_boost XCORE_BSS;
static struct pgov_stats st;
static uint32_t last_us, last_idle[2], band_rem;
static bool on;
static uint8_t logged_mhz;
static void (*log_fn)(const char *fmt, ...);

static const char * const why_name[PGOV_WHY_N] = {
    "none", "load", "sat", "floor", "boost", "dwell", "cap", "volt"
};

#ifdef HAVE_PP_GOV_DVS
/* ---- Stage 2 state (section 16) ---------------------------------------- */
#define PGOV2_ARM_AFTER     (60*HZ)     /* section 9.2: boot at normal V */
#define PGOV2_ARM_MV        3500        /* section 9.2: the disk-safe level */
#define PGOV2_REC_PATH      ROCKBOX_DIR "/pgov.dat"
#define PGOV2_REC_TMP       ROCKBOX_DIR "/pgov.tmp"
#define PGOV2_CAP_HOLDS     (CPU_PLL_HOLD_ATA | CPU_PLL_HOLD_USB)

static struct pgov_core core2 XCORE_BSS;    /* the stage-2 policy */
static struct pgov_hw hw XCORE_BSS;         /* what is applied */
static struct pgov_dog dog;
static struct pgov_record rec;          /* in RAM; rec_disk: as written */
static struct pgov_record rec_disk;
static volatile uint8_t mode2 XCORE_BSS;    /* PGOV2_* */
static bool rec_ok;                     /* the record lets DVS arm */
static bool armed2;                     /* pgov_core_arm(&core2) state */
static bool rec_queued;                 /* a write waits for the disk */
static volatile bool port_busy XCORE_DATA = true; /* the BCM2722's port
                                                     may be in use */
static volatile uint8_t stor_busy XCORE_BSS;  /* storage commands in
                                                 flight (ata.c) */
static bool shutting;
static uint8_t b3_state;                /* 0 untested, 1 passed, 2 failed */
static uint8_t s2_mhz, s2_v;            /* the last logged decision */
static uint8_t last_fb;
static volatile uint8_t fault_pending XCORE_BSS; /* pgov2_fault() */
static volatile bool rec_rw XCORE_BSS;  /* this boot started in apply mode
                                           (writes the record; the clamp) */
static struct {
    uint32_t band_ms[PGOV_NBANDS], vlow_ms;
    uint32_t vch, vup_boost, fb, dog;
} st2;

/* band 3's caps: the BCM2722's port awake or powered (a device test or
 * the job driver may drive it without lcd_awake(): review S4), any
 * storage command, ATA DMA, USB */
static bool b3_capped(void)
{
    return port_busy || (GPO32_VAL & 0x4000) || stor_busy
        || (cpu_pll_holds() & PGOV2_CAP_HOLDS);
}

static struct pgov_core *applied(void)
{
    return mode2 == PGOV2_APPLY ? &core2 : &core;
}
#else
#define applied() (&core)
#define mode2 PGOV2_OFF
#endif

/* A core's idle time so far, the sleep in progress included (a parked
 * COP is asleep throughout and reads idle) */
static uint32_t idle_now(unsigned int c, uint32_t now)
{
    uint32_t t0, idle;

    do
    {
        t0 = pgov_sleep_t0[c];
        idle = pgov_idle_us[c];
    } while (t0 != pgov_sleep_t0[c]);   /* it woke between the reads */
    return t0 ? idle + (now - t0) : idle;
}

static unsigned int floor_of(unsigned int holds)
{
    /* every holder is a 30 MHz floor; the lit screen holds only when the
     * shell's lit switch says 30 (D1: band 0 lit or dark by default) */
    return holds ? PGOV_MHZ_LOW : 0;
}

#ifdef HAVE_PP_GOV_DVS
static void revert2(unsigned int code);

/* The tick: the stage-2 policy on the same sample, its time per band,
 * and the watchdog over an owed raise (apply mode) */
static void pgov2_tick(const struct pgov_in *in1, uint32_t ms)
{
    struct pgov_in in = *in1;
    unsigned int holds = cpu_pll_holds(), vnow, mnow;
    bool changed;

    /* band 3's caps; every ATA transfer at the normal voltage (reads
     * too: the floor is band 1 then anyway) */
    (void)holds;
    in.cap_mhz = b3_capped() ? PGOV_MHZ_TOP : 0;
    /* any storage command at the normal voltage (review S2) */
    in.vwrite = stor_busy || (holds & CPU_PLL_HOLD_ATA);
    changed = pgov_core_step(&core2, &in);

    if (mode2 == PGOV2_APPLY)
    {
        mnow = in.cur_mhz;
        vnow = hw.vcode;
    }
    else
    {
        mnow = core2.mhz;
        vnow = core2.vcode;
    }
    st2.band_ms[pgov_band_of(core2.live, mnow)] += ms;
    if (vnow == PGOV_V_LOW)
        st2.vlow_ms += ms;

    if (mode2 != PGOV2_APPLY)
    {
        if (changed)
            power_thread_kick();    /* the shadow's log line */
        return;
    }
    if (changed)
        power_thread_kick();
    /* review B2: the tick never writes the PMU.  A raise unconfirmed for
     * 250 ms reverts the policy (normal voltage decided, stage 1 for the
     * boot) and books the fault for the power thread; meanwhile the clamp
     * holds the clock at band 1's top while the rail may be low */
    if (pgov_dog_step(&dog, core2.vcode == PGOV_V_NORM
                            && hw.vcode != PGOV_V_NORM,
                      current_tick * (1000 / HZ)) == PGOV_DOG_FIRE)
    {
        st2.dog++;
        pgov_core_fallback(&core2);
        if (!fault_pending)
            fault_pending = PGOV_FB_DOG;
        power_thread_kick();
    }
}
#endif /* HAVE_PP_GOV_DVS */

/* The tick (CPU, IRQ context): sample, decide, kick on a change */
static void pgov_tick(void)
{
    uint32_t now = USEC_TIMER, el = now - last_us;
    struct pgov_in in;
    unsigned int cur = cpu_frequency / 1000000;

    for (unsigned int c = 0; c < NUM_CORES; c++)
    {
        uint32_t idle = idle_now(c, now);
        uint32_t d = idle - last_idle[c];

        if ((int32_t)d < 0)
            d = 0;                  /* never backwards */
        else
            last_idle[c] = idle;
        in.busy_pm[c] = pgov_busy_pm(d, el);
    }
#if NUM_CORES < 2
    in.busy_pm[1] = 0;
#endif
    last_us = now;
    el += band_rem;                 /* the time per band, in ms */
    st.band_ms[pgov_band_of(core.live, cur)] += el / 1000;
    band_rem = el % 1000;
    in.cur_mhz = cur;
    in.floor_mhz = floor_of(cpu_pll_holds());
    in.cap_mhz = 0;                 /* G1: no band 3, nothing to cap */
    in.boost = get_cpu_boost_counter() > 0;
    in.vwrite = 0;
    in.dt_ms = el / 1000 > 255 ? 255 : el / 1000;
    if (cop_boost)
    {
        /* a COP boost, booked (review B1): the policy's change, here */
        cop_boost = 0;
        pgov_core_boost(&core);
#ifdef HAVE_PP_GOV_DVS
        if (mode2)
            pgov_core_boost(&core2);
#endif
        power_thread_kick();
    }
    if (core.sat_run == core.p.sat_samples)
        st.sats++;
    if (pgov_core_step(&core, &in) && mode2 != PGOV2_APPLY)
        power_thread_kick();
#ifdef HAVE_PP_GOV_DVS
    if (mode2)
        pgov2_tick(&in, el / 1000);
#endif
}

bool pgov_active(void)
{
    return on;
}

void pgov_enable(bool en)
{
    struct pgov_params p;
    int irq;

    if (en == on)
        return;
    if (en)
    {
        pgov_params_default(&p);
        irq = disable_irq_save();
        /* G1: band 0 and band 2 only (no voltage step, no 100 MHz) */
        pgov_core_init(&core, &p, 0, cpu_frequency / 1000000);
        logged_mhz = core.mhz;
        last_us = USEC_TIMER;
        for (unsigned int c = 0; c < NUM_CORES; c++)
            last_idle[c] = idle_now(c, last_us);
        restore_irq(irq);
        on = true;
        tick_add_task(pgov_tick);
    }
    else
    {
#ifdef HAVE_PP_GOV_DVS
        pgov2_enable(PGOV2_OFF);    /* the rail normal first */
#endif
        tick_remove_task(pgov_tick);
        on = false;
        cpu_pgov_resync();          /* today's unboosted clock */
    }
    if (log_fn)
        log_fn("on=%d", (int)en);
}

/* system.c, boost lock held */
long pgov_boost_hz(void)
{
    int irq;

#if NUM_CORES > 1
    if (CURRENT_CORE == COP)
    {
        /* review B1: read only; the tick applies the policy's boost */
        unsigned int m = PGOV_MHZ_TOP;
#ifdef HAVE_PP_GOV_DVS
        if (rec_rw)
            m = pgov_cop_boost_mhz(&hw, USEC_TIMER);
#endif
        cop_boost = 1;
        power_thread_kick();
        return m * 1000000L;
    }
#endif
    irq = disable_irq_save();
    struct pgov_core *c = applied();
#ifdef HAVE_PP_GOV_DVS
    bool low = c == &core2 && core2.vstate != PGOV_VS_NORM;
#endif
    unsigned int m = pgov_core_boost(c);
    restore_irq(irq);
    st.boosts++;
#ifdef HAVE_PP_GOV_DVS
    if (low)
    {
        st2.vup_boost++;
        power_thread_kick();        /* the raise, then 80 MHz */
    }
#endif
    return m * 1000000L;
}

long pgov_floor_hz(unsigned int holds)
{
    int irq;
    struct pgov_core *c = applied();
    unsigned int m;

#if NUM_CORES > 1
    if (CURRENT_CORE == COP)
    {
        /* read only (review B1); the clamp bounds it, the tick follows */
        m = c->mhz > floor_of(holds) ? c->mhz : floor_of(holds);
        power_thread_kick();
        return m * 1000000L;
    }
#endif
    irq = disable_irq_save();
    m = pgov_core_floor(c, floor_of(holds));
#ifdef HAVE_PP_GOV_DVS
    if (c == &core2 && (holds & PGOV2_CAP_HOLDS))
        m = pgov_core_cap(c, PGOV_MHZ_TOP);
#endif
    restore_irq(irq);
    return m * 1000000L;
}

long pgov_decision_hz(void)
{
    return applied()->mhz * 1000000L;
}

#ifdef HAVE_PP_GOV_DVS
/* system.c, boost lock held: every clock change, clamped to what the
 * rail carries now and to band 3's caps (a boot that started in apply
 * mode only) */
long pgov_clamp_hz(long f)
{
    unsigned int ceil;

    /* from the first apply-mode boot on, also after a revert or with
     * stage 2 switched off: a raise the tick made still settles */
    if (!rec_rw)
        return f;
    ceil = pgov_hw_ceil(&hw, USEC_TIMER);
    if (ceil > PGOV_MHZ_TOP && (b3_capped() || b3_state == 2))
        ceil = PGOV_MHZ_TOP;
    if (f > (long)ceil * 1000000L)
    {
        if (ceil <= PGOV_MHZ_LOW && core2.vstate == PGOV_VS_LOW)
        {
            /* asked for more than the low rail carries (a boost while the
             * governor is bypassed, cpu_idle_mode()): the raise is owed;
             * booked for the tick when on the COP (review B1) */
#if NUM_CORES > 1
            if (CURRENT_CORE == COP)
                cop_boost = 1;
            else
#endif
            {
                int irq = disable_irq_save();
                pgov_core_boost(&core2);
                restore_irq(irq);
            }
        }
        f = (long)ceil * 1000000L;
        power_thread_kick();    /* review S3: the clock again once it may */
    }
    return f;
}

/* system.c, boost lock held: the voltage may go down now (the clock is in
 * band 0 or 1 and nothing boosts); recorded before the PMU write */
bool pgov_lower_locked(long cur_hz, int boosts)
{
    if (mode2 != PGOV2_APPLY || cur_hz > PGOV_MHZ_LOW * 1000000L
        || boosts > 0)
        return false;
    hw.vcode = PGOV_V_LOW;
    return true;
}
#endif

#ifdef HAVE_PP_GOV_DVS
/* ---- Stage 2: the record, arming, the apply layer ----------------------- */

/* The storage thread as the disk idles (or system_flush() at a clean
 * shutdown): the record, when it changed.  The disk spins anyway */
static void rec_write_cb(void)
{
    struct pgov_record r = rec;
    uint8_t buf[PGOV_REC_FILE_SIZE];
    bool ok;
    int fd;

    rec_queued = false;
    pgov_rec_pack(&r, buf);
    /* atomic: a temporary name, synced, then renamed over the record
     * (review nit); a cut leaves the old record or the new, never half */
    fd = open(PGOV2_REC_TMP, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return;
    ok = write(fd, buf, sizeof(buf)) == (ssize_t)sizeof(buf);
    ok = fsync(fd) >= 0 && ok;
    ok = close(fd) >= 0 && ok;
    if (ok)
    {
        remove(PGOV2_REC_PATH);
        ok = rename(PGOV2_REC_TMP, PGOV2_REC_PATH) >= 0;
    }
    if (ok)
        rec_disk = r;
    power_thread_kick();            /* armed on disk: DVS may go low */
}

static void rec_changed(void)
{
    if (rec_rw && memcmp(&rec, &rec_disk, sizeof(rec)) && !rec_queued)
    {
        rec_queued = true;
        register_storage_idle_func(rec_write_cb);
    }
}

/* the shell's log, tagged as the stage-2 policy's */
#define s2log(fmt, ...) \
    do { if (log_fn) log_fn("s2 " fmt, ##__VA_ARGS__); } while (0)

/* Section 9.2's conditions, re-checked at each power-thread pass; the
 * apply mode also waits until "armed" is on disk (arming before trying:
 * a crash at the low voltage always finds armed=1 at the next boot) */
static void arm_check(void)
{
    bool ok = rec_ok && !shutting && (core2.live & PGOV_LIVE_B1)
              && TIME_AFTER(current_tick, PGOV2_ARM_AFTER)
              && !usb_inserted() && battery_voltage() >= PGOV2_ARM_MV;

    if (ok && mode2 == PGOV2_APPLY && !rec_disk.armed)
    {
        if (!rec.armed)
        {
            pgov_rec_armed(&rec);
            rec_changed();
        }
        ok = false;                 /* not until it is written */
    }
    if (ok != armed2)
    {
        int irq = disable_irq_save();
        pgov_core_arm(&core2, ok);
        restore_irq(irq);
        armed2 = ok;
        s2log("arm=%d mv=%d", (int)ok, battery_voltage());
    }
}

/* One DCDC1 write with section 3.2's hard limits, read back */
static bool set_core_voltage(unsigned int code)
{
    int was = pcf50605_read(PCF5060X_DCDC1);

    if (code != PGOV_V_NORM && code != PGOV_V_LOW)
        return false;
    if (was < 0 || (was & 0xe0) != (code & 0xe0))
        return false;
    if (pcf50605_write(PCF5060X_DCDC1, code) < 0)
        return false;
    return pcf50605_read(PCF5060X_DCDC1) == (int)code;
}

/* Back to stage 1 for the rest of the boot (the stage-2 policy stays as
 * a shadow with DVS off), the record told.  The rail is already normal
 * or raised here */
static void revert2(unsigned int code)
{
    int irq = disable_irq_save();

    pgov_core_fallback(&core2);
    if (code == PGOV_FB_B3CHECK)
    {
        core2.live &= ~PGOV_LIVE_B3;
        hw.live &= ~PGOV_LIVE_B3;
        rec.b3 = PGOV_REC_OFF;
        b3_state = 2;
    }
    else if (code != PGOV_FB_LOCK)     /* a lock miss: this boot only */
        pgov_rec_fail(&rec, code);
    mode2 = PGOV2_SHADOW;
    armed2 = false;
    last_fb = code;
    st2.fb++;
    restore_irq(irq);
    power_thread_kick();            /* stage 1's decision, the record */
}

/* A thread (never the COP): the rail to normal, by the driver, else by
 * the raw path under the driver's lock (a bus the driver gave up on) */
static bool raise_now(void)
{
    bool ok;

    i2c_lock();                     /* recursive: the driver's calls too */
    ok = set_core_voltage(PGOV_V_NORM) || pmu_raw_core_voltage(PGOV_V_NORM);
    if (ok)
    {
        int irq = disable_irq_save();
        hw.vcode = PGOV_V_NORM;
        hw.v_at_us = USEC_TIMER;
        restore_irq(irq);
    }
    i2c_unlock();
    return ok;
}

/* The rail to 0xEC from any context (review S1): a thread with IRQs on
 * takes the I2C lock; a panic or exception path (IRQs off, nothing else
 * runs) writes raw */
void pgov2_rail_normal(void)
{
    switch (pgov_reboot_action(rec_rw, hw.vcode, irq_enabled()))
    {
    case PGOV_RB_LOCKED:
        if (raise_now())
            break;
        /* fall through: the bus is lost; the raw write is all there is */
    case PGOV_RB_RAW:
#if NUM_CORES > 1
        /* review nit: on the COP with the bus held, the holder is a CPU
         * thread mid-transaction; a raw write would cut into it */
        if (CURRENT_CORE == COP && i2c_held())
            break;
#endif
        if (pmu_raw_core_voltage(PGOV_V_NORM))
        {
            hw.vcode = PGOV_V_NORM;
            hw.v_at_us = USEC_TIMER;
        }
        break;
    }
}

/* ata.c, its mutex held: every storage command at the normal voltage,
 * band 3 capped (review S2) */
void pgov2_storage(bool busy)
{
    if (!busy)
    {
        if (stor_busy)
            stor_busy--;
        return;
    }
    stor_busy++;
    if (!rec_rw)
        return;
    if (hw.vcode != PGOV_V_NORM && !raise_now())
        pgov2_fault(PGOV_FB_READBACK);
    if (cpu_frequency > CPUFREQ_MAX)
        cpu_pgov_recheck();
}

/* Any thread or core: the decision goes to the normal voltage at once
 * (pure state; the tick's watchdog covers the raise from here), and the
 * power thread raises and reverts (the COP never drives the PMU's bus) */
void pgov2_fault(unsigned int code)
{
    int irq;

    if (mode2 != PGOV2_APPLY)
        return;
    irq = disable_irq_save();
    pgov_core_fallback(&core2);
    if (!fault_pending)
        fault_pending = code;
    restore_irq(irq);
    power_thread_kick();
}

static void fault_service(void)
{
    unsigned int code = fault_pending;

    fault_pending = 0;
    if (!code || mode2 != PGOV2_APPLY)
        return;
    if (hw.vcode != PGOV_V_NORM && !raise_now())
    {
        /* the PMU does not answer: the clamp keeps the clock at band 1's
         * top; stay reverted and say so (no blind reset, review B2c) */
        s2log("fb why=%u rail unconfirmed", code);
    }
    else
        s2log("fb why=%u", code);
    revert2(code);
}

/* Band 3's check at its first entry (section 3.3): the 10,000-iteration
 * loop (4 cycles each, cached: 501 us at 80 MHz [external]) and a CRC of
 * 32 KB of this image's code, taken before and after the switch */
static uint32_t b3_loop_us(void)
{
    uint32_t n = 10000, t;
    int irq = disable_irq_save();
    uint32_t t0 = USEC_TIMER;

    asm volatile ("1: subs %0, %0, #1\n\tbne 1b" : "+r"(n) : : "cc");
    t = USEC_TIMER - t0;
    restore_irq(irq);
    return t;
}

static uint32_t b3_crc(void)
{
    /* the 32 KB-aligned block holding the policy's code: .text, or the
     * .rodata after it (both read-only; .data starts 170 KB later) */
    const void *p = (const void *)((uintptr_t)pgov_core_step & ~0x7fffu);
    return crc_32(p, 32768, 0xffffffff);
}

/* One clock step of the apply layer (the boost lock is system.c's) */
static void clock_step(unsigned int mhz)
{
    unsigned long fails = pgov_pll_lock_fails();
    uint32_t crc80 = 0;
    bool check = mhz == PGOV_MHZ_B3 && b3_state == 0;

    if (check)
        crc80 = b3_crc();
    cpu_pgov_set(mhz * 1000000L);
    if (pgov_pll_lock_fails() != fails)
    {
        s2log("fb why=lock mhz=%u", mhz);
        revert2(mhz == PGOV_MHZ_B3 ? PGOV_FB_B3CHECK : PGOV_FB_LOCK);
        return;
    }
    if (check && cpu_frequency == PGOV_MHZ_B3 * 1000000L)
    {
        uint32_t us = b3_loop_us(), crc = b3_crc();
        bool pass = pgov_b3_check(us, crc80, crc);

        s2log("b3 loop_us=%lu crc=%s", (unsigned long)us,
             crc == crc80 ? "ok" : "bad");
        if (pass)
        {
            b3_state = 1;
            if (rec.b3 == PGOV_REC_UNTRIED)
            {
                rec.b3 = PGOV_REC_TRIAL;
                rec_changed();
            }
        }
        else
        {
            cpu_pgov_set(PGOV_MHZ_TOP * 1000000L);
            revert2(PGOV_FB_B3CHECK);
        }
    }
}

/* The power thread: move the hardware to the stage-2 decision, one
 * ordered step at a time (pgov_hw_next()), a few steps per pass */
static void apply2(void)
{
    for (int i = 0; i < 8 && mode2 == PGOV2_APPLY; i++)
    {
        struct pgov_op op;
        unsigned int mhz, v;
        int irq = disable_irq_save();

        mhz = core2.mhz;
        v = core2.vcode;
        hw.mhz = cpu_frequency / 1000000;
        pgov_hw_next(&hw, mhz, v, USEC_TIMER, &op);
        restore_irq(irq);
        switch (op.op)
        {
        case PGOV_OP_NONE:
            if (pgov_hw_settle_left(&hw, USEC_TIMER) && i < 7)
            {
                sleep(1);           /* review R4: not a spin to the end */
                break;
            }
            cpu_pgov_reboost();     /* review S3: a boost held at 30 MHz */
            return;
        case PGOV_OP_VUP:
            st2.vch++;
            if (!raise_now())
            {
                /* the clamp holds band 1's top on an unconfirmed rail */
                pgov2_fault(PGOV_FB_READBACK);
                fault_service();
                return;
            }
            break;
        case PGOV_OP_WAIT:
            sleep(1);               /* the settle: at most two ticks */
            break;
        case PGOV_OP_CLOCK:
            clock_step(op.arg);
            break;
        case PGOV_OP_VDOWN:
        {
            bool ok;

            /* review B2a: the lock across the re-check, the write and the
             * read-back; nothing else writes DCDC1 meanwhile */
            i2c_lock();
            if (!cpu_pgov_lower()
                || !pgov_vdown_recheck(mode2, &hw, core2.vcode, stor_busy,
                                       cpu_frequency / 1000000))
            {
                /* a boost, a fault, a storage command came in */
                if (hw.vcode == PGOV_V_LOW)
                {
                    int irq2 = disable_irq_save();
                    hw.vcode = PGOV_V_NORM;     /* nothing was written */
                    restore_irq(irq2);
                }
                i2c_unlock();
                return;
            }
            st2.vch++;
            ok = set_core_voltage(PGOV_V_LOW);
            i2c_unlock();
            if (!ok)
            {
                s2log("fb why=readback down");
                pgov2_fault(PGOV_FB_READBACK);
                fault_service();
                return;
            }
            break;
        }
        }
    }
}

static void s2log_decision(void)
{
    if (core2.mhz == s2_mhz && core2.vcode == s2_v)
        return;
    s2log("band=%u/%u v=%x from=%u why=%s util=%u",
         pgov_band_of(core2.live, core2.mhz), core2.mhz, core2.vcode,
         s2_mhz, why_name[core2.why < PGOV_WHY_N ? core2.why : 0],
         pgov_need_mhz10(&core2) * 10 / (core2.mhz ? core2.mhz : 1));
    s2_mhz = core2.mhz;
    s2_v = core2.vcode;
}
#endif /* HAVE_PP_GOV_DVS */

/* The power thread: apply, then log the change */
void pgov_apply(void)
{
    unsigned int m;

    if (!on)
        return;
#ifdef HAVE_PP_GOV_DVS
    if (mode2)
    {
        fault_service();
        arm_check();
        rec_changed();              /* a revert's, from the tick */
        if (mode2 == PGOV2_APPLY)
            apply2();
        if (mode2 == PGOV2_APPLY && hw.vcode != PGOV_V_NORM
            && core2.vcode == PGOV_V_NORM)
            power_thread_kick();    /* a raise still owed: come back */
        s2log_decision();
    }
    if (mode2 != PGOV2_APPLY)
#endif
    cpu_pgov_apply();
    m = applied()->mhz;
    if (m != logged_mhz)
    {
        if (m > logged_mhz)
            st.up++;
        else
            st.dn++;
        if (log_fn)
            log_fn("band=%u/%u from=%u why=%s util=%u",
                   pgov_band_of(core.live, m), m, logged_mhz,
                   why_name[applied()->why < PGOV_WHY_N
                            ? applied()->why : 0],
                   pgov_need_mhz10(applied()) * 10 / m);
        logged_mhz = m;
    }
}

void pgov_settle(void)
{
    /* power_sleep(): band 0 now when nothing boosts or holds, instead of
     * waiting out the dwell (the sleep refuses any other clock) */
    if (on && get_cpu_boost_counter() == 0 && cpu_pll_holds() == 0)
    {
        int irq = disable_irq_save();
        core.mhz = PGOV_MHZ_XTAL;
        core.below_ms = 0;
#ifdef HAVE_PP_GOV_DVS
        /* Q-G5's default: the soft sleep at the normal voltage; the
         * power thread re-arms after the wake */
        core2.mhz = PGOV_MHZ_XTAL;
        core2.below_ms = 0;
        if (armed2)
        {
            pgov_core_arm(&core2, false);
            armed2 = false;
        }
#endif
        restore_irq(irq);
#ifdef HAVE_PP_GOV_DVS
        if (mode2 == PGOV2_APPLY)
        {
            if (hw.vcode != PGOV_V_NORM && !raise_now())
                pgov2_fault(PGOV_FB_READBACK);
            cpu_pgov_set(PGOV_MHZ_XTAL * 1000000L);
            return;
        }
#endif
        cpu_pgov_apply();
    }
}

#ifdef HAVE_PP_GOV_DVS
unsigned int pgov2_mode(void)
{
    return mode2;
}

/* The shell, at boot (UI thread): the markers' mode.  Reads the record
 * and the gate inputs; stage 1 must be on */
void pgov2_enable(unsigned int mode)
{
    uint32_t strap = STRAP_OPT_A;
    unsigned int live = 0;
    int dcdc1, irq;
    bool prev_armed;

    if (mode == mode2 || !on)
        return;
    if (mode == PGOV2_OFF)
    {
        if (mode2 == PGOV2_APPLY && hw.vcode != PGOV_V_NORM)
            raise_now();            /* the clamp stays on (rec_rw) */
        irq = disable_irq_save();
        mode2 = PGOV2_OFF;
        restore_irq(irq);
        if (rec.armed)
        {
            pgov_rec_clean_end(&rec, st2.vlow_ms / 1000);
            rec_changed();
        }
        cpu_pgov_apply();
        s2log("off");
        return;
    }
    if (mode2 != PGOV2_OFF)
        return;                     /* shadow <-> apply: at boot only */

    /* the record: an "armed" left from the last boot is an unclean end
     * of a low-voltage window (section 9.4) */
    {
        /* review R3: a cut between the write's remove and rename leaves
         * the newest record as pgov.tmp; a sound one is the newest */
        uint8_t buf[PGOV_REC_FILE_SIZE], tbuf[PGOV_REC_FILE_SIZE];
        int fd = open(PGOV2_REC_PATH, O_RDONLY);
        int n = fd >= 0 ? read(fd, buf, sizeof(buf)) : -1;
        int tn;

        if (fd >= 0)
            close(fd);
        fd = open(PGOV2_REC_TMP, O_RDONLY);
        tn = fd >= 0 ? read(fd, tbuf, sizeof(tbuf)) : -1;
        if (fd >= 0)
            close(fd);
        pgov_rec_pick(buf, n < 0 ? 0 : (unsigned int)n,
                      tbuf, tn < 0 ? 0 : (unsigned int)tn, &rec);
        rec_disk = rec;
    }
    prev_armed = rec.armed;
    rec_ok = pgov_rec_boot(&rec, false);

    /* the gates: DCDC1 as Rockbox set it; Apple's grade table [apple];
     * a strap read of all ones (no such register) never allows band 3 */
    dcdc1 = pcf50605_read(PCF5060X_DCDC1);
    if (dcdc1 == PGOV_V_NORM)
        live |= PGOV_LIVE_B1;
    else
        rec_ok = false;
    if (strap != 0xffffffffu && pgov_grade_mhz(strap) >= PGOV_MHZ_B3
        && rec.b3 != PGOV_REC_OFF)
        live |= PGOV_LIVE_B3;
    if (mode == PGOV2_SHADOW)
        rec_ok = rec.dvs != PGOV_REC_OFF && dcdc1 == PGOV_V_NORM;

    {
        struct pgov_params p;
        pgov_params_default(&p);
        irq = disable_irq_save();
        pgov_core_init(&core2, &p, live, core.mhz);
        hw.mhz = cpu_frequency / 1000000;
        hw.vcode = PGOV_V_NORM;
        hw.v_at_us = USEC_TIMER - PGOV_SETTLE_US;
        hw.live = live;
        memset(&dog, 0, sizeof(dog));
        memset(&st2, 0, sizeof(st2));
        b3_state = 0;
        armed2 = false;
        s2_mhz = core2.mhz;
        s2_v = core2.vcode;
        mode2 = mode;
        restore_irq(irq);
    }
    s2log("boot mode=%s grade=%u b1=%d b3=%d dcdc1=%x",
         mode == PGOV2_APPLY ? "apply" : "shadow",
         (unsigned int)((strap >> 5) & 3), (int)!!(live & PGOV_LIVE_B1),
         (int)!!(live & PGOV_LIVE_B3), (unsigned int)dcdc1);
    s2log("rec dvs=%u b3=%u code=%u hist=%x strike=%d ok=%d",
         rec.dvs, rec.b3, rec.code, rec.hist, (int)prev_armed,
         (int)rec_ok);
    rec_rw = mode == PGOV2_APPLY;   /* a shadow never writes it */
    rec_changed();                  /* the boot's own update, if any */
    power_thread_kick();
}

void pgov2_shutdown(void)
{
    if (mode2 == PGOV2_OFF)
        return;
    shutting = true;
    if (armed2)
    {
        int irq = disable_irq_save();
        pgov_core_arm(&core2, false);
        restore_irq(irq);
        armed2 = false;
    }
    if (rec.armed)
    {
        pgov_rec_clean_end(&rec, st2.vlow_ms / 1000);
        rec_changed();              /* system_flush() writes it */
    }
    pgov2_rail_normal();            /* now, not on the next pass (S1) */
    power_thread_kick();
}

void pgov2_shutdown_cancelled(void)
{
    if (shutting)
    {
        shutting = false;           /* stage 2 may arm again */
        power_thread_kick();
    }
}

void pgov_port_cap(bool busy)
{
    port_busy = busy;
    if (busy && rec_rw && cpu_frequency > CPUFREQ_MAX)
        cpu_pgov_recheck();         /* off 100 MHz before the port */
}
#endif /* HAVE_PP_GOV_DVS */

void pgov_get_stats(struct pgov_stats *s)
{
    int irq = disable_irq_save();
    *s = st;
    restore_irq(irq);
}

void pgov_set_log(void (*log)(const char *fmt, ...))
{
    log_fn = log;
    if (log)
    {
        /* read only: the speed grade gates band 3 (G3), PP_VER1's chip
         * byte the PP5022 path [apple, mac-and-dvs.md 3.2] */
        uint32_t strap = STRAP_OPT_A;
        log("boot grade=%u max=%u strap=0x%08lx chip=%c on=%d",
            (unsigned int)((strap >> 5) & 3), pgov_grade_mhz(strap),
            (unsigned long)strap, (int)((PP_VER1 >> 16) & 0xff), (int)on);
    }
}

void pgov_log_summary(const char *at)
{
    struct pgov_stats s;
    uint32_t t, pm[PGOV_NBANDS];

    if (!log_fn || !on)
        return;
    pgov_get_stats(&s);
    t = (s.band_ms[0] + s.band_ms[1] + s.band_ms[2] + s.band_ms[3]) / 1000;
    for (unsigned int b = 0; b < PGOV_NBANDS; b++)
        pm[b] = s.band_ms[b] / (t + 1);     /* permille */
    log_fn("sum at=%s t=%lu b0=%lu b1=%lu b2=%lu b3=%lu vlow=0", at,
           (unsigned long)t, (unsigned long)pm[0], (unsigned long)pm[1],
           (unsigned long)pm[2], (unsigned long)pm[3]);
    log_fn("tr up=%lu dn=%lu boost=%lu sat=%lu", (unsigned long)s.up,
           (unsigned long)s.dn, (unsigned long)s.boosts,
           (unsigned long)s.sats);
#ifdef HAVE_PP_GOV_DVS
    if (mode2 || st2.fb)
    {
        uint32_t ms = 0;
        for (unsigned int b = 0; b < PGOV_NBANDS; b++)
            ms += st2.band_ms[b];
        ms = ms / 1000 + 1;
        s2log("sum b0=%lu b1=%lu b2=%lu b3=%lu vlow=%lu",
             (unsigned long)(st2.band_ms[0] / ms),
             (unsigned long)(st2.band_ms[1] / ms),
             (unsigned long)(st2.band_ms[2] / ms),
             (unsigned long)(st2.band_ms[3] / ms),
             (unsigned long)(st2.vlow_ms / ms));
        s2log("tr vch=%lu rse=%lu fb=%lu/%u dog=%lu lockf=%lu",
             (unsigned long)st2.vch, (unsigned long)st2.vup_boost,
             (unsigned long)st2.fb, last_fb, (unsigned long)st2.dog,
             pgov_pll_lock_fails());
    }
#endif
}
