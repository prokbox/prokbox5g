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
 * The PP5022 band governor (docs/design/pp-governor.md): the contract.
 * The pure policy (pgov_core.c, host-tested and run inside the
 * simulator in tests/ppgov/) and the firmware glue (pgov.c).
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
#ifndef _PGOV_H_
#define _PGOV_H_

#include <stdint.h>
#include <stdbool.h>

/* ---- The bands (brief section 3.1) ------------------------------------ */

/* Operating points, MHz.  Band 0 is the 24 MHz crystal with the PLL off;
 * band 1 the PLL at 30 MHz at the low core voltage (live only with DVS);
 * band 2 the PLL at normal voltage, 30-80 MHz; band 3 100 MHz (live only
 * where the speed grade allows and its check passed) */
#define PGOV_MHZ_XTAL   24
#define PGOV_MHZ_LOW    30
#define PGOV_MHZ_TOP    80
#define PGOV_MHZ_B3     100

enum { PGOV_B0 = 0, PGOV_B1, PGOV_B2, PGOV_B3, PGOV_NBANDS };

#define PGOV_LIVE_B1    0x02    /* the voltage step is live on this unit */
#define PGOV_LIVE_B3    0x08    /* 100 MHz is live on this unit */

/* DCDC1 codes: the only two the governor may ever write (section 3.2) */
#define PGOV_V_NORM     0xEC
#define PGOV_V_LOW      0xEB

enum pgov_inband {
    PGOV_INBAND_TOP = 0,        /* T: a live band runs at its top */
    PGOV_INBAND_PROP,           /* P: the lowest point of band 2 that fits */
};

/* Why the decision moved (the "gov band=" line's why=) */
enum pgov_why {
    PGOV_WHY_NONE = 0,
    PGOV_WHY_LOAD,              /* the measured need, up */
    PGOV_WHY_SAT,               /* saturation, up */
    PGOV_WHY_FLOOR,             /* a floor rose */
    PGOV_WHY_BOOST,             /* cpu_boost() */
    PGOV_WHY_DWELL,             /* down, after the dwell */
    PGOV_WHY_CAP,               /* a cap fell */
    PGOV_WHY_VOLT,              /* held at band 1's top while the voltage rises */
    PGOV_WHY_N
};

/* ---- The policy -------------------------------------------------------- */

struct pgov_params {
    uint16_t dwell_ms[PGOV_NBANDS]; /* below the band this long to step
                                       down out of it ([0] unused) */
    uint16_t vdwell_ms;         /* in bands 0/1 this long before 0xEB */
    uint16_t sat_pm;            /* a sample this busy (permille) is saturated */
    uint8_t headroom_pct;       /* H: a band fits while need <= H% of its top */
    uint8_t down_pct;           /* down to a point only while need <= this %
                                   of it (>= H: the measurement at the higher
                                   clock overstates a memory-bound load) */
    uint8_t ewma_shift;         /* the need's EWMA weight is 1 / 2^shift */
    uint8_t sat_samples;        /* saturated samples in a row to go up */
    uint8_t inband;             /* enum pgov_inband */
    uint8_t b2_mask;            /* band 2's points this build runs: bit 0
                                   30, 1 48, 2 60, 3 80 (80 always) */
};

/* One sample's inputs (the CPU's tick, every 10 ms) */
struct pgov_in {
    uint16_t busy_pm[2];        /* per core: busy share of the sample, permille */
    uint8_t cur_mhz;            /* the clock that ran (as applied) */
    uint8_t floor_mhz;          /* every floor, max: 0, 30, 80 or 100 */
    uint8_t cap_mhz;            /* every cap, min (0: none) */
    uint8_t boost;              /* cpu_boost() count > 0 */
    uint8_t vwrite;             /* a storage write in flight: normal voltage */
    uint8_t dt_ms;              /* the sample's length */
};

/* Voltage states */
enum { PGOV_VS_NORM = 0, PGOV_VS_LOW, PGOV_VS_RAISING };

struct pgov_core {
    struct pgov_params p;
    int32_t need_khz;           /* EWMA of max(busy x clock), kHz */
    uint16_t below_ms;          /* how long the decision has wanted less */
    uint16_t vok_ms;            /* how long low voltage has been allowed */
    uint16_t settle_ms;         /* a raise's settle still to run */
    uint8_t live;               /* PGOV_LIVE_* */
    uint8_t sat_run;            /* saturated samples in a row */
    uint8_t mhz;                /* the decision: the operating point */
    uint8_t vcode;              /* the decision: DCDC1 */
    uint8_t vstate;             /* PGOV_VS_* */
    uint8_t dvs_ok;             /* armed this boot, no fallback since */
    uint8_t why;                /* enum pgov_why of the last change */
    uint8_t from;               /* the operating point before it */
};

/* The brief's starting point (section 4.2), before the simulator */
void pgov_params_default(struct pgov_params *p);
void pgov_core_init(struct pgov_core *c, const struct pgov_params *p,
                    unsigned int live, unsigned int start_mhz);
/* One sample.  Returns true when the decision (mhz or vcode) changed;
 * the caller applies it (voltage up before the clock, down after) */
bool pgov_core_step(struct pgov_core *c, const struct pgov_in *in);
/* cpu_boost() 0 -> 1: the point to select now (80 MHz, or band 1's top
 * while the voltage is low: the raise is then pending, vcode = NORM) */
unsigned int pgov_core_boost(struct pgov_core *c);
/* A floor rose outside the tick (ATA, USB): the point to select now,
 * at least floor_mhz, never below the decision */
unsigned int pgov_core_floor(struct pgov_core *c, unsigned int floor_mhz);
/* DVS: whether the voltage may go low this boot (the glue's section 9.2
 * conditions and the record) */
void pgov_core_arm(struct pgov_core *c, bool armed);
/* A fallback trigger (section 9.3): normal voltage at once, DVS off for
 * the boot.  Returns true if the voltage must be raised */
bool pgov_core_fallback(struct pgov_core *c);
/* Invariants 1-2 (section 9.1) for an applied point and code */
bool pgov_core_valid(const struct pgov_core *c, unsigned int mhz,
                     unsigned int vcode);
unsigned int pgov_band_of(unsigned int live, unsigned int mhz);
unsigned int pgov_need_mhz10(const struct pgov_core *c);   /* MHz x 10 */
/* A core's busy share, permille, from its idle time over a sample (a
 * parked core, asleep throughout, reads 0) */
unsigned int pgov_busy_pm(uint32_t idle_us, uint32_t elapsed_us);

/* ---- Tables (section 3.1, mac-and-dvs.md section 3) -------------------- */

/* PLL_CONTROL for a PLL point (0: not a PLL point) */
uint32_t pgov_pll_word(unsigned int mhz);
/* DEV_TIMING1: Apple's 90 ns rule, floored at Rockbox's value */
uint32_t pgov_dev_timing1(unsigned int mhz);
/* Apple's ceiling for STRAP_OPT_A[6:5], MHz */
unsigned int pgov_grade_mhz(uint32_t strap_opt_a);
/* The core voltage's ceiling clock: 30 MHz at 0xEB */
unsigned int pgov_vceil_mhz(unsigned int live, unsigned int vcode);

/* ---- The persistent record (section 9.4) ------------------------------- */

enum { PGOV_REC_UNTRIED = 0, PGOV_REC_TRIAL, PGOV_REC_OK, PGOV_REC_OFF };

/* Fallback triggers (section 9.3), the record's code and the "fb" line */
enum {
    PGOV_FB_NONE = 0, PGOV_FB_READBACK, PGOV_FB_CRC, PGOV_FB_CODEC,
    PGOV_FB_UNDERRUN, PGOV_FB_DOG, PGOV_FB_ABORT, PGOV_FB_UNCLEAN,
    PGOV_FB_B3CHECK,
    PGOV_FB_LOCK            /* stage 2: a 48/60 MHz PLL word did not lock */
};

struct pgov_record {
    uint8_t dvs;                /* PGOV_REC_* */
    uint8_t b3;                 /* PGOV_REC_* */
    uint8_t code;               /* the trigger that turned it off */
    uint8_t hist;               /* last five armed boots, bit set = unclean */
    uint8_t armed;              /* this boot armed (written at the settle) */
    uint8_t skip;               /* one strike: skip the next boot */
    uint8_t ok_boots;           /* clean armed boots with >= 1 h low */
    uint8_t pad;
};

/* At boot, before anything else: the last boot's end.  Returns whether
 * DVS may arm in this boot */
bool pgov_rec_boot(struct pgov_record *r, bool prev_clean);
void pgov_rec_armed(struct pgov_record *r);
void pgov_rec_fail(struct pgov_record *r, unsigned int code);
void pgov_rec_clean_end(struct pgov_record *r, unsigned long low_s);
void pgov_rec_reset(struct pgov_record *r);

/* ---- Stage 2: the apply layer (brief section 16) ------------------------ */

/* A cap from outside the tick (an ATA transfer, USB, the BCM port waking):
 * the point to select now, at most cap_mhz (a point this unit has) */
unsigned int pgov_core_cap(struct pgov_core *c, unsigned int cap_mhz);

/* What the hardware is at, as the apply layer knows it.  vcode is the
 * LOWEST code the rail may be at: a lowering is recorded before its PMU
 * write, a raise only after its read-back, so a reader that trusts it
 * never runs a clock the rail may not carry (section 16.3) */
struct pgov_hw {
    uint32_t v_at_us;           /* USEC_TIMER at the last confirmed raise */
    uint8_t mhz;                /* the clock applied */
    uint8_t vcode;              /* PGOV_V_* */
    uint8_t live;               /* PGOV_LIVE_* */
    uint8_t pad;
};

/* A raise settles this long before any clock above band 1 [ours; Apple's
 * BCM2722 driver waits 10 ms per regulator change, brief-map-power.md] */
#define PGOV_SETTLE_US  10000

/* The highest clock the rail carries now (band 1's top while low or still
 * settling) */
unsigned int pgov_hw_ceil(const struct pgov_hw *hw, uint32_t now_us);

/* One step of the ordered move from hw toward (mhz, vcode): voltage up
 * before the clock goes up, then the settle, then the clock; the clock
 * down before the voltage goes down.  The caller does op, updates hw and
 * asks again until PGOV_OP_NONE */
enum { PGOV_OP_NONE = 0, PGOV_OP_VUP, PGOV_OP_WAIT, PGOV_OP_CLOCK,
       PGOV_OP_VDOWN };
struct pgov_op {
    uint8_t op;
    uint8_t arg;                /* VUP/VDOWN: the code; CLOCK: MHz */
    uint16_t wait_us;           /* WAIT: how long */
};
void pgov_hw_next(const struct pgov_hw *hw, unsigned int mhz,
                  unsigned int vcode, uint32_t now_us, struct pgov_op *op);

/* The revert-on-timeout watchdog (section 16.5, review B2).  Called by
 * the CPU's tick with whether a raise is owed (the decision wants the
 * normal voltage and the rail is not confirmed at it).  A raise not
 * confirmed within PGOV_DOG_MS returns FIRE once: the tick then only
 * reverts the policy and books the fault; it never writes the PMU (the
 * clamp already holds the clock at band 1's top while the rail may be
 * low, and the power thread does the raise under the I2C lock) */
#define PGOV_DOG_MS     250
enum { PGOV_DOG_OK = 0, PGOV_DOG_FIRE };
struct pgov_dog {
    uint32_t t0_ms;             /* when the owed raise began */
    uint8_t owed;               /* a raise is owed */
    uint8_t fired;              /* FIRE returned for this owed raise */
    uint8_t pad[2];
};
unsigned int pgov_dog_step(struct pgov_dog *d, bool owed, uint32_t now_ms);

/* VDOWN's re-check, made with the I2C lock held just before the PMU
 * write (review B2a): still applying, the rail recorded low, the
 * decision still low, no storage command in flight, the clock in band 0
 * or 1 */
bool pgov_vdown_recheck(unsigned int mode, const struct pgov_hw *hw,
                        unsigned int want_vcode, unsigned int stor_busy,
                        unsigned int cur_mhz);

/* A boost on the COP (review B1): it never writes the policy's state; it
 * selects at most band 2's top within what the rail carries, read from
 * the uncached state, and books the raise for the CPU */
unsigned int pgov_cop_boost_mhz(const struct pgov_hw *hw, uint32_t now_us);

/* Every reboot path (review S1): what it must do to leave DCDC1 at 0xEC.
 * NONE: no stage-2 boot, or the rail is recorded normal; LOCKED: the
 * driver's write under the I2C lock (a thread, IRQs on); RAW: the raw
 * write (a panic or exception path, nothing else runs) */
enum { PGOV_RB_NONE = 0, PGOV_RB_LOCKED, PGOV_RB_RAW };
unsigned int pgov_reboot_action(bool rec_rw, unsigned int hw_vcode,
                                bool can_lock);

/* Band 3's built-in check (section 3.3) at its first entry in a boot: the
 * 10,000-iteration loop timed at 100 MHz, and a CRC of the same code
 * taken at 80 MHz just before */
#define PGOV_B3_LOOP_MIN_US 380
#define PGOV_B3_LOOP_MAX_US 420
bool pgov_b3_check(uint32_t loop_us, uint32_t crc80, uint32_t crc100);

/* The record's file (the system folder's pgov.dat): magic, version, the
 * record and a checksum; anything else reads as a fresh record */
#define PGOV_REC_FILE_SIZE  16
void pgov_rec_pack(const struct pgov_record *r,
                   uint8_t buf[PGOV_REC_FILE_SIZE]);
bool pgov_rec_unpack(const uint8_t *buf, unsigned int len,
                     struct pgov_record *r);
/* At boot (review R3): the record from pgov.dat and pgov.tmp.  The write
 * goes temp, sync, remove, rename, and a broken write never checks, so a
 * sound pgov.tmp is the newest; else pgov.dat; else a fresh record.
 * Returns 0 fresh, 1 from the record, 2 from the temporary */
unsigned int pgov_rec_pick(const uint8_t *main, unsigned int main_len,
                           const uint8_t *tmp, unsigned int tmp_len,
                           struct pgov_record *r);
/* The settle still to run after a confirmed raise, us (0: settled, or
 * the rail not recorded normal); the apply pass sleeps on it (R4) */
uint32_t pgov_hw_settle_left(const struct pgov_hw *hw, uint32_t now_us);

/* Stage 2's modes (pgov2_enable(), section 16) */
enum { PGOV2_OFF = 0, PGOV2_SHADOW, PGOV2_APPLY };

/* ---- The firmware glue (pgov.c, HAVE_PP_GOV) --------------------------- */

#if defined(HAVE_PP_GOV) && !defined(PGOV_HOST)
/* Per core, single writer: the time in core_sleep() (thread-pp.c) */
extern uint32_t pgov_idle_us[2];
extern uint32_t pgov_sleep_t0[2];

struct pgov_stats {
    uint32_t band_ms[PGOV_NBANDS];
    uint32_t up, dn, pll_starts, boosts, sats;
};

/* The developer switch (a marker file read by the shell at boot); off by
 * default, and off is today's behaviour exactly */
void pgov_enable(bool on);
bool pgov_active(void);
/* system.c, with the boost lock held: cpu_boost() 0 -> 1 (the point to
 * select now), a holder change (the floor, applied at once when it
 * rises), the decision in force */
long pgov_boost_hz(void);
long pgov_floor_hz(unsigned int holds);
long pgov_decision_hz(void);
/* system.c: apply the decision under the boost lock (never below 80 MHz
 * while boosted); back to today's unboosted clock when switched off */
void cpu_pgov_apply(void);
void cpu_pgov_resync(void);
/* The power thread (2 Hz step and kicks): apply the decision */
void pgov_apply(void);
/* power_sleep(): band 0 now, if nothing holds or boosts */
void pgov_settle(void);
void pgov_get_stats(struct pgov_stats *st);
/* The shell's log (ui_log on a CPU thread); NULL to stop */
void pgov_set_log(void (*log)(const char *fmt, ...));
void pgov_log_summary(const char *at);
#endif

#if defined(HAVE_PP_GOV_DVS) && !defined(PGOV_HOST)
/* system.c, boost lock held: every clock change clamped (stage 2's apply
 * mode); whether the rail may go down now (recorded before the write) */
long pgov_clamp_hz(long f);
bool pgov_lower_locked(long cur_hz, int boosts);
/* system.c, for pgov.c's power-thread apply layer */
void cpu_pgov_set(long f);
bool cpu_pgov_lower(void);
void cpu_pgov_recheck(void);
void cpu_pgov_reboost(void);
/* system-pp502x.c: the governor's PLL words that did not lock in 2 ms */
unsigned long pgov_pll_lock_fails(void);

/* Stage 2 (section 16), off unless the shell's markers turn it on: the
 * stage-2 policy (band 1 at the low voltage, band 3 at 100 MHz) runs
 * beside stage 1 on the same samples.  Shadow: its decisions are logged,
 * never applied.  Apply (a second marker): its decisions are applied,
 * with the stage-1 decision as the revert target */
void pgov2_enable(unsigned int mode);
unsigned int pgov2_mode(void);
/* sys_poweroff()/sys_reboot(): the planned end; the rail goes back to
 * normal now (synchronously) and the record's write rides system_flush();
 * cancel_shutdown(): a cancelled power-off lets stage 2 arm again */
void pgov2_shutdown(void);
void pgov2_shutdown_cancelled(void);
/* system_reboot() and rolo_load(): the rail to 0xEC before the restart
 * (review S1); safe from any context */
void pgov2_rail_normal(void);
/* ata.c, around every command under its mutex (review S2): the normal
 * voltage and band 3 capped for any storage command, PIO or DMA */
void pgov2_storage(bool busy);
/* A fault seen outside the governor (PGOV_FB_*): revert to stage 1 */
void pgov2_fault(unsigned int code);
/* lcd-video.c: the BCM2722's host port is about to be used (true, before
 * the first access of a wake) or is quiet (false, the display asleep).
 * Band 3 runs only while it is quiet */
void pgov_port_cap(bool busy);
#endif

#endif /* _PGOV_H_ */
