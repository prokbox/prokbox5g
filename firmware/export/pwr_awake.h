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

/* Awake-time battery savings taken from Apple 1.3 (P19 pwr-awake;
 * docs/pp5022-reference/apple-power-catalog.md section 3, rows AW-n).
 *
 * Each row that changes behaviour is an automatic A/B with a fallback:
 *   - on in every build whose bench row passed on a real unit: its bit in
 *     PWRAW_TOKENS (pwr_awake_tokens.h, the "token pwraw-<row>" line the
 *     bench session writes, copied in by hand like bcmfw/verified.txt's
 *     clock150 lines);
 *   - in developer builds, on also when the developer mask says so (the
 *     marker file read at boot; PWRAW_DEV_DEFAULT without one), so the
 *     row gets regular-use testing before its bench;
 *   - off for the rest of the session after any failure signal
 *     (pwraw_fail()), whatever the token or mask said.
 * Every act, undo and failure is counted in a small per-row record the
 * UI thread drains into ipodui.log (powerlog.c: "pwraw <row> ..."), so
 * any core or IRQ may note without logging itself.
 *
 * Rows that are not behaviour switches keep a record too: AW-8 (the
 * XMB_RAM_CFG values around Rockbox's USB-reset write, read-only) and
 * AW-9 (the PMU ADC's done-bit polls). AW-3 (the accessory supply) is a
 * per-unit flag (ACCESSORY_SUPPLY_DEFAULT, config/ipodvideo.h) plus its
 * bench row, not a row here: the USB-C mod board may draw from it. */
#ifndef PWR_AWAKE_H
#define PWR_AWAKE_H

#include <stdbool.h>
#include <stdint.h>

enum pwraw_row
{
    PWRAW_LINGER = 0,   /* AW-1: the drive cut 250 ms after a fill (flash) */
    PWRAW_USBOFF,       /* AW-2: USB off on a charger with no host */
    PWRAW_LCDLDO,       /* AW-4: the LCD LDO in mode 010 while dark */
    PWRAW_I2SGATE,      /* AW-5: codec clock and I2S gated in DAC standby */
    PWRAW_N_SWITCH,     /* the rows above are switches */
    PWRAW_XMB = PWRAW_N_SWITCH, /* AW-8: read-only log */
    PWRAW_ADC,          /* AW-9: done-bit polls */
    PWRAW_N
};

#define PWRAW_BIT(r)        (1u << (r))
/* Each switch row's code is compiled in by its config flag
 * (config/ipodvideo.h: HAVE_PWR_LINGER, HAVE_USB_CHARGER_PARK,
 * HAVE_PWR_LCDLDO, HAVE_PWR_I2SGATE): always in developer builds, in a
 * release once its token is set. */
/* developer builds without a mask file: the rows with no hang risk on
 * (regular-use testing); AW-4 and AW-5 wait for their bench rows */
#define PWRAW_DEV_DEFAULT   (PWRAW_BIT(PWRAW_LINGER) | PWRAW_BIT(PWRAW_USBOFF))

/* Why a row is on or off now (the log's why=) */
enum pwraw_why
{
    PWRAW_W_TOKEN,      /* its bench row passed: the build's default */
    PWRAW_W_DEV,        /* developer mask */
    PWRAW_W_FAILED,     /* a failure signal this session */
    PWRAW_W_NONE,       /* no token, no developer mask */
};

/* Pure: the gate. tokens: PWRAW_TOKENS; dev_mask: the developer mask
 * (0 in a release build); failed: the session's failed rows */
static inline enum pwraw_why pwraw_gate(int row, uint32_t tokens,
                                        uint32_t dev_mask, uint32_t failed)
{
    uint32_t b = PWRAW_BIT(row);

    if (failed & b)
        return PWRAW_W_FAILED;
    if (tokens & b)
        return PWRAW_W_TOKEN;
    if (dev_mask & b)
        return PWRAW_W_DEV;
    return PWRAW_W_NONE;
}

static inline bool pwraw_why_on(enum pwraw_why w)
{
    return w == PWRAW_W_TOKEN || w == PWRAW_W_DEV;
}

/* One row's record: counters only grow; a and b are the last act's
 * values (a register found and written, a window, a count) */
struct pwraw_rec
{
    uint32_t acts;      /* times it acted (cut, parked, lowered, gated) */
    uint32_t undos;     /* times it put the state back */
    uint32_t fails;     /* failure signals (the first one turns it off) */
    uint32_t a, b;
};

#ifndef PWRAW_HOST_TEST
/* firmware/common/pwr_awake.c */
bool pwraw_active(int row);             /* any context: two loads */
enum pwraw_why pwraw_why_now(int row);
void pwraw_set_dev_mask(uint32_t mask); /* apps at boot (developer) */
uint32_t pwraw_dev_mask(void);
/* the bench rows' A/B: forces a switch row on (1), off (0) or back to the
 * gate (-1); held until the next call */
void pwraw_force(int row, int on);
void pwraw_note(int row, bool act, uint32_t a, uint32_t b);
void pwraw_fail(int row, uint32_t a, uint32_t b);
const struct pwraw_rec *pwraw_rec(int row);
const char *pwraw_row_name(int row);

/* AW-5, firmware/target/arm/pp/wmcodec-pp.c (HAVE_DAC_STANDBY): gate or
 * ungate in DAC standby; the mask picks T-I2SGATE's variant (a: the
 * codec clock only, b: with I2S; b by default) */
void wmcodec_clocks_gate(bool gate);
bool wmcodec_clocks_gated(void);
extern unsigned long wmcodec_gate_mask;

/* AW-4, firmware/target/arm/ipod/powermgmt-ipod-pcf.c: the LCD LDO
 * (PCF5060X_D3REGC1) to mode 010 while the LCD sleeps (dark true, from
 * lcd_sleep() after the BCM is down) and back to its found value, 10 ms
 * settled, before lcd_awake() touches the BCM (dark false) */
void pwraw_lcd_ldo(bool dark);
#endif

#endif /* PWR_AWAKE_H */
