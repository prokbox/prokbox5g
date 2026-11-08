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

/* The battery probes (devbench_pwr.c), as a row group for the bench
 * session: PP5022 silicon questions S-1, S-3, S-4, S-6 and S-8 of
 * docs/pp5022-reference/apple-power.md section 8 (open-questions.md #39,
 * #41, #42, #44, #46), plus two read-only snapshots (the speed grade, #35,
 * and the power registers against Apple's values).  Developer builds only
 * (HAVE_IPOD_UI_DEVMODE, and devbench.c itself only in the device test
 * builds): the release has none of it.
 *
 * Two uses of this header:
 *  - included normally: the pure part (verdicts, decodes, the restore
 *    bookkeeping, the picks), host-tested in tests/pwrprobes;
 *  - included inside devbench.c's steps[] with DEVBENCH_PWR_ROWS defined:
 *    the rows themselves, as table initializers (the one hook):
 *        #define DEVBENCH_PWR_ROWS
 *        #include "devbench_pwr.h"
 *
 * The rows, basic to advanced (bench_pwr_picks below):
 *   pgrd  read-only: STRAP_OPT_A[6:5], Apple's speed grade (66-133 MHz)
 *   preg  read-only: the power and clock registers, raw, for apple-power.md
 *   prtc  hands off ~12 s: does 0x6000_5014 count, at what rate against
 *         USEC_TIMER and the PMU's RTC (S-6)
 *   pstl  hands off: CPU_CTL count-mode stalls (clock, us, ms units),
 *         timed against USEC_TIMER, short forms only (S-8)
 *   pldo  two owner questions: the LCD LDO (0x26) then the codec LDO
 *         (0x24) in Apple's sleep mode 010 for 5 s each, restored exactly;
 *         the codec part only after the screen part passed (S-4)
 *   pden  one key press per bit: DEV_EN bits for blocks nothing on the
 *         Video uses, gated one at a time and restored (S-3, awake part)
 *   psrf  hands off ~3 s: SDRAM self-refresh from IRAM (XMB_RAM_CFG bit 22
 *         and its acknowledge, bit 30), 1 s held, a CRC either side (S-1)
 *
 * Every row puts back exactly what it found and logs before and after.
 * The simulator refuses every row ("Not run (sim)", a skip), never a
 * modelled pass. */

#ifdef DEVBENCH_PWR_ROWS
#undef DEVBENCH_PWR_ROWS
#ifdef HAVE_IPOD_UI_DEVMODE
    /* ---- Battery probes (devbench_pwr.h; picked runs only:
       bench_pwr_picks) ---- */
    { "pgrd", "Speed grade", NULL, NULL, devbench_pwrp_grade, NULL,
      { NULL }, BENCH_F_AUTO | BENCH_F_PICKED, 10, 0, 0 },
    { "preg", "Power registers", NULL, NULL, devbench_pwrp_regs, NULL,
      { NULL }, BENCH_F_AUTO | BENCH_F_PICKED, 10, 1, 0 },
    { "prtc", "Seconds counter",
      "Hands off, about 15 s. The iPod compares three of its clocks for "
      "about 12 s.",
      "Nothing to watch: the next step follows by itself.",
      devbench_pwrp_rtc, NULL, { NULL }, BENCH_F_AUTO | BENCH_F_PICKED, 30,
      15, 0 },
    { "pstl", "Timed pauses",
      "Hands off, a few seconds. The iPod's main core pauses itself for "
      "short timed moments, the longest 10 ms; the music pauses for them. "
      "The screen may freeze; if "
      "so, hold MENU+SELECT to restart.",
      "Nothing to watch: the next step follows by itself.",
      devbench_pwrp_stall, NULL, { NULL }, BENCH_F_AUTO | BENCH_F_PICKED,
      20, 5, 0 },
    { "pldo", "Screen and sound supply",
      "About 1 min. Headphones on; the album you played last plays. The "
      "screen goes dark for about 6 s and comes back; then a question. "
      "Then the music pauses for about 12 s and plays again; then a "
      "second question.",
      "Leave the iPod alone until each question. If the sound stays "
      "silent at the end, a restart brings it back.",
      devbench_pwrp_ldo, NULL, { NULL }, BENCH_F_PICKED, 0, 20, 12 },
    { "pden", "Unused blocks off",
      "Under 1 min. The iPod switches off parts of its chip that nothing "
      "uses, one at a time. For each it asks you to press SELECT. The "
      "screen may freeze; if so, hold MENU+SELECT to restart.",
      "Press SELECT whenever asked. If nothing is switched on to test, "
      "the step ends by itself.",
      devbench_pwrp_den, NULL, { NULL }, BENCH_F_PICKED, 0, 45, 10 },
    { "psrf", "Memory rest",
      "Hands off, about 5 s. The music stops and the iPod's memory rests "
      "for 1 s. The screen may freeze; if so, hold MENU+SELECT to restart.",
      "Nothing to watch: the next step follows by itself, and the music "
      "plays again.",
      devbench_pwrp_srf, NULL, { NULL }, BENCH_F_PICKED, 0, 6, 5 },
#endif
#else /* the header proper */

#ifndef _IPODUI_DEVBENCH_PWR_H_
#define _IPODUI_DEVBENCH_PWR_H_

#include <stdbool.h>
#include <stdint.h>
#include "devbench.h"

/* the rows (devbench_pwr.c; each an enum bench_res) */
int devbench_pwrp_grade(void);
int devbench_pwrp_regs(void);
int devbench_pwrp_rtc(void);
int devbench_pwrp_stall(void);
int devbench_pwrp_ldo(void);
int devbench_pwrp_den(void);
int devbench_pwrp_srf(void);

/* The group's run order and needs, for the mode that runs it (the BCM
 * session wires it into the combined sitting).  Basic to advanced: the
 * read-only rows, the hands-off timing rows, then the writes, the most
 * likely to freeze last.  Needs: pden and psrf need preg (the registers
 * they change are logged before any write, so a freeze still leaves the
 * starting state in the file).  No row needs pgrd, prtc or pstl: each is
 * its own question.  pldo's codec part needs its screen part, inside the
 * row.  None earns a verified line (ALSO); none restarts. */
static const struct bench_pick bench_pwr_picks[] =
{
    { "pgrd",   BENCH_R_ALSO, { NULL } },
    { "preg",   BENCH_R_ALSO, { NULL } },
    { "prtc",   BENCH_R_ALSO, { NULL } },
    { "pstl",   BENCH_R_ALSO, { NULL } },
    { "pldo",   BENCH_R_ALSO, { NULL } },
    { "pden",   BENCH_R_ALSO, { "preg" } },
    { "psrf",   BENCH_R_ALSO, { "preg" } },
};
#define BENCH_PWR_NPICKS \
    ((int)(sizeof(bench_pwr_picks) / sizeof(bench_pwr_picks[0])))

/* The group as a picked run (bench_select_hold's rule for its picks):
 * table indices in order, roles, needs; an id the table lacks is left
 * out.  The run's count */
static inline int bench_pwr_select(const struct bench_step *t, int n,
                                   struct bench_run *run)
{
    int p, d;

    run->n = 0;
    for (p = 0; p < BENCH_PWR_NPICKS && run->n < BENCH_MAX_STEPS; p++)
    {
        int k = bench_find(t, n, bench_pwr_picks[p].id);

        if (k < 0 || k >= BENCH_MAX_STEPS)
            continue;
        run->k[run->n] = (uint8_t)k;
        for (d = 0; d < BENCH_NEEDS; d++)
        {
            int kd = bench_pwr_picks[p].need[d]
                     ? bench_find(t, n, bench_pwr_picks[p].need[d]) : -1;

            run->need[run->n][d] = (int8_t)(kd < BENCH_MAX_STEPS ? kd : -1);
        }
        run->role[run->n++] = bench_pwr_picks[p].role;
    }
    return run->n;
}

/* ---- pgrd: the speed grade (mac-and-dvs.md 3.2, [apple]) ------------- */

static inline unsigned pwrp_grade(uint32_t strap)
{
    return (strap >> 5) & 3;
}

/* Apple's table: 0 133, 1 100, 2 80, 3 66 MHz */
static inline unsigned pwrp_grade_mhz(uint32_t strap)
{
    static const uint8_t mhz[4] = { 133, 100, 80, 66 };

    return mhz[pwrp_grade(strap)];
}

/* ---- prtc: the seconds counter at 0x6000_5014 (S-6) ------------------ */

/* Apple's decode: days [29:24], hours [21:16], minutes [13:8], seconds
 * [5:0] (apple-power.md 4.1, [apple-RE]) */
static inline uint32_t pwrp_5014_secs(uint32_t v)
{
    return ((v >> 24) & 0x3F) * 86400u + ((v >> 16) & 0x3F) * 3600u +
           ((v >> 8) & 0x3F) * 60u + (v & 0x3F);
}

#define PWRP_RTC_SECS       10      /* the window, about */
#define PWRP_RTC_PPM_MAX    20000   /* a 1 Hz counter within 2 % */

/* The verdict: it counts (d5014 > 0) at 1 Hz against USEC_TIMER (edge to
 * edge, us) within PWRP_RTC_PPM_MAX, and agrees with the PMU's RTC to a
 * second.  ppm: the rate's error, parts per million (out) */
static inline enum bench_res pwrp_rtc_verdict(uint32_t d5014, uint32_t dus,
                                              int32_t dpmu, int32_t *ppm)
{
    int64_t want = (int64_t)d5014 * 1000000;
    int64_t err;

    *ppm = 0;
    if (d5014 == 0 || dus == 0)
        return BENCH_FAIL;          /* it doesn't count */
    err = (int64_t)dus - want;
    *ppm = (int32_t)(err * 1000000 / (int64_t)dus);
    if (*ppm > PWRP_RTC_PPM_MAX || *ppm < -PWRP_RTC_PPM_MAX)
        return BENCH_FAIL;
    if (dpmu - (int32_t)d5014 > 1 || (int32_t)d5014 - dpmu > 1)
        return BENCH_FAIL;
    return BENCH_PASS;
}

/* ---- pstl: CPU_CTL count-mode stalls (S-8) --------------------------- */

/* Apple's forms (apple-power.md 2.3): PROC_WAIT_CNT | unit | count.  The
 * order is the row's: clock cycles, then microseconds, then milliseconds;
 * a unit runs only if every form before it passed */
struct pwrp_stall
{
    uint32_t word;
    uint32_t want_us;               /* the stated time */
};

#define PWRP_UNIT_CLKS      0x08000000u
#define PWRP_UNIT_USEC      0x02000000u
#define PWRP_UNIT_MSEC      0x01000000u
#define PWRP_WAIT_CNT       0x40000000u

static const struct pwrp_stall pwrp_stalls[] =
{
    { PWRP_WAIT_CNT | PWRP_UNIT_CLKS | 15,   0 },    /* Apple's 0x4800_000F */
    { PWRP_WAIT_CNT | PWRP_UNIT_USEC | 10,   10 },
    { PWRP_WAIT_CNT | PWRP_UNIT_USEC | 100,  100 },
    { PWRP_WAIT_CNT | PWRP_UNIT_USEC | 250,  250 },  /* 0x4200_00FA */
    { PWRP_WAIT_CNT | PWRP_UNIT_MSEC | 1,    1000 },
    { PWRP_WAIT_CNT | PWRP_UNIT_MSEC | 10,   10000 },/* 0x4100_000A */
};
#define PWRP_NSTALLS ((int)(sizeof(pwrp_stalls) / sizeof(pwrp_stalls[0])))
#define PWRP_STALL_REPS     3

/* A stall held as stated: at least 90 % of it, at most 110 % plus 8 us
 * (the timer reads and the write around it); a clock-cycle form, under
 * 8 us.  The bound past which the row stops: 10 times the time plus 1 ms */
static inline bool pwrp_stall_ok(uint32_t want_us, uint32_t got_us)
{
    return got_us + want_us / 10 >= want_us &&
           got_us <= want_us + want_us / 10 + 8;
}

/* ---- pldo: the LDO sleep mode (S-4) --------------------------------- */

/* The PCF50605's DnREGC1: mode [7:5] (111 on, 010 Apple's light sleep,
 * 000 off), voltage code [4:0] (0.9 V + 0.1 V a step) [apple-RE,
 * rockbox-derived] */
#define PWRP_PMU_D1REGC1    0x24    /* the codec's supply */
#define PWRP_PMU_D3REGC1    0x26    /* the LCD's supply */
#define PWRP_LDO_ON         7
#define PWRP_LDO_SLEEP      2

static inline unsigned pwrp_ldo_mode(uint8_t v)
{
    return v >> 5;
}

/* Apple's sleep form: the same voltage code, mode 010 */
static inline uint8_t pwrp_ldo_sleep_val(uint8_t v)
{
    return (uint8_t)((v & 0x1F) | (PWRP_LDO_SLEEP << 5));
}

/* The voltage, in tens of mV */
static inline unsigned pwrp_ldo_cv(uint8_t v)
{
    return 90 + 10 * (v & 0x1F);
}

/* The row's two parts: the codec part runs only after the screen part
 * passed; the row passes only if both did.  An answer of 0 is "Yes".
 * Sitting 4gh-b5: lcd BENCH_DONE is the screen's supply found already in
 * mode 010 (the lcdldo token's daily path, dark): the codec part runs
 * anyway and decides the row */
enum pwrp_ldo_part { PWRP_LDO_LCD = 0, PWRP_LDO_CODEC, PWRP_LDO_PARTS };

static inline enum bench_res pwrp_ldo_verdict(enum bench_res lcd,
                                              enum bench_res codec)
{
    if (lcd != BENCH_PASS && lcd != BENCH_DONE)
        return lcd == BENCH_SKIP ? BENCH_SKIP : BENCH_FAIL;
    if (codec == BENCH_PASS)
        return BENCH_PASS;
    return codec == BENCH_SKIP ? BENCH_DONE : BENCH_FAIL;
}

/* ---- pden: DEV_EN bits for unused blocks (S-3) ---------------------- */

/* The candidates, named by Rockbox's pp5020.h, that nothing on the iPod
 * Video uses while it runs (each checked against the code's users,
 * docs/pp5022-reference/clocks-power.md section 11):
 *   FireWire (23): no driver, no PHY wired for data;
 *   LCD (26): the Video's screen is the BCM's, through the host port;
 *   ATA (14): the SD/flash controller of the Sansas (ata-sd-pp.c);
 *   UART0 and UART1 (6, 7): the dock's serial port, unused without an
 *     accessory (uart-pp.c turns them on when one is set up);
 *   USB1 (22): only with the USB controller off (usb-fw-pp502x.c turns
 *     both USB bits on and off together).
 * The row tests only the candidates that are on now: a bit already off
 * says nothing.  Kept, never tested: SYSTEM, EXTCLOCKS, USB0, I2S, I2C,
 * OPTO, PWM (the clicker), IDE0, and the unnamed bits 5, 8, 30, 31 */
struct pwrp_den
{
    uint32_t bit;
    const char *name;
};

static const struct pwrp_den pwrp_dens[] =
{
    { 1u << 23, "FireWire" },
    { 1u << 26, "LCD controller" },
    { 1u << 14, "SD/ATA controller" },
    { 1u << 6,  "UART0" },
    { 1u << 7,  "UART1" },
    { 1u << 22, "USB1" },
};
#define PWRP_NDENS ((int)(sizeof(pwrp_dens) / sizeof(pwrp_dens[0])))
#define PWRP_DEN_USB0       (1u << 3)
#define PWRP_DEN_USB1       (1u << 22)
#define PWRP_DEN_UARTS      ((1u << 6) | (1u << 7))

/* The candidates to test in this DEV_EN: on now; USB1 only with USB0
 * off and no cable; the UARTs only with no accessory set up */
static inline uint32_t pwrp_den_candidates(uint32_t dev_en, bool usb,
                                           bool serial)
{
    uint32_t m = 0;
    int i;

    for (i = 0; i < PWRP_NDENS; i++)
        m |= pwrp_dens[i].bit;
    if (usb || (dev_en & PWRP_DEN_USB0))
        m &= ~PWRP_DEN_USB1;
    if (serial)
        m &= ~PWRP_DEN_UARTS;
    return dev_en & m;
}

/* A gated bit kept the system healthy: the tick ran (within 10 % of
 * USEC_TIMER over the window), the wheel answered, and the audio played
 * on if it was playing */
static inline bool pwrp_den_healthy(uint32_t ticks, uint32_t us, bool wheel,
                                    bool playing, uint32_t played_ms)
{
    uint32_t want = us / 10000;     /* HZ 100 */

    if (ticks * 10 < want * 9 || ticks * 10 > want * 11 + 10)
        return false;
    if (!wheel)
        return false;
    return !playing || played_ms * 10 >= us / 1000 * 8;
}

/* ---- psrf: SDRAM self-refresh (S-1) --------------------------------- */

#define PWRP_SRF_REQ        (1u << 22)  /* XMB_RAM_CFG: the request */
#define PWRP_SRF_ACK        (1u << 30)  /* and its acknowledge */
#define PWRP_SRF_HOLD_US    1000000u
#define PWRP_SRF_BOUND_US   100000u     /* each poll's bound */
#define PWRP_SRF_TIMEOUT    0xFFFFFFFFu
#define PWRP_SRF_CRC_BYTES  0x100000u   /* 1 MB of the firmware's code */

/* The IRAM routine's block (in IRAM; offsets fixed for its assembly) */
struct pwrp_srf_io
{
    uint32_t hold_us;       /* 0: in: the hold */
    uint32_t bound_us;      /* 4: in: each poll's bound */
    uint32_t ack_us;        /* 8: out: request to acknowledge, or TIMEOUT */
    uint32_t held_us;       /* 12: out: the hold as timed */
    uint32_t rel_us;        /* 16: out: release to the acknowledge clear */
    uint32_t cfg_before;    /* 20: out: XMB_RAM_CFG before */
    uint32_t cfg_in;        /* 24: out: with the acknowledge seen */
    uint32_t cfg_after;     /* 28: out: after the release */
};

/* S-1's answer: the acknowledge set and cleared within the bounds, the
 * request bit back as found, the code's CRC unchanged */
static inline enum bench_res pwrp_srf_verdict(const struct pwrp_srf_io *io,
                                              uint32_t crc0, uint32_t crc1)
{
    if (crc0 != crc1)
        return BENCH_FAIL;          /* memory lost: loudly */
    if (io->ack_us == PWRP_SRF_TIMEOUT || io->rel_us == PWRP_SRF_TIMEOUT)
        return BENCH_FAIL;
    if ((io->cfg_after ^ io->cfg_before) & (PWRP_SRF_REQ | PWRP_SRF_ACK))
        return BENCH_FAIL;
    return BENCH_PASS;
}

/* ---- the restore bookkeeping (every row) ---------------------------- */

/* What a row changed, as found: a PP register by its address, a PMU
 * register as PWRP_PMU(reg) (odd: no PP register is) */
#define PWRP_SAVE_MAX       8
#define PWRP_PMU(reg)       (((uint32_t)(reg) << 8) | 1u)

struct pwrp_saved
{
    int n;
    uint32_t addr[PWRP_SAVE_MAX];
    uint32_t was[PWRP_SAVE_MAX];
    uint32_t mask[PWRP_SAVE_MAX];   /* the bits the row owns */
};

static inline void pwrp_saved_init(struct pwrp_saved *s)
{
    s->n = 0;
}

/* false: full (the row must not change it) */
static inline bool pwrp_save(struct pwrp_saved *s, uint32_t addr,
                             uint32_t was, uint32_t mask)
{
    if (s->n >= PWRP_SAVE_MAX)
        return false;
    s->addr[s->n] = addr;
    s->was[s->n] = was;
    s->mask[s->n++] = mask;
    return true;
}

/* The first saved word whose owned bits don't read back as found, or -1:
 * all restored */
static inline int pwrp_unrestored(const struct pwrp_saved *s,
                                  uint32_t (*rd)(uint32_t addr))
{
    int i;

    for (i = 0; i < s->n; i++)
        if ((rd(s->addr[i]) ^ s->was[i]) & s->mask[i])
            return i;
    return -1;
}

#endif /* _IPODUI_DEVBENCH_PWR_H_ */
#endif /* DEVBENCH_PWR_ROWS */
