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

/* Boot battery guard (#70 follow-up, 2026-10-22): the pure decision.
 * A flat cell recovers about 210 mV at rest after it runs out under load
 * (owner rundown: cut at 3,362 mV, rests of 3,576 and 3,569 after), and
 * the boot load then pulls it under and it browns out with no clean
 * shutdown. Two rules keep it from booting (owner decision 2026-10-22):
 *   - a resting floor, BB_FLOOR_MV, read before boosting or writing
 *     anything (stage 1, before storage);
 *   - a drained latch: when the display reaches 0 % on battery the
 *     firmware records "drained" on disk (the clean low-battery shutdown
 *     is only a backup: this unit can hard-cut before it), and on
 *     battery a drained cell is refused whatever it rests at (stage 2,
 *     read right after the mount). Power seen (charger or USB) clears
 *     it, and so does a rest that shows a real charge (BB_RECHARGE_MV).
 * With external power present it boots as always. apps/ipodui/bootbatt.c
 * samples and acts; tests/bootbatt/ checks this file on the host. No
 * Rockbox calls. */
#ifndef _IPODUI_BOOTBATT_CORE_H_
#define _IPODUI_BOOTBATT_CORE_H_

#include <stdbool.h>
#include <stdint.h>

/* The resting floor (both storage variants; rule 7): low enough that a
 * cell with hours of playback left always boots (3,600 refused cells
 * that still had them), above the runtime shutoff (3,300) so a cell that
 * just reached it under load must still recover at rest. The latch, not
 * the floor, refuses an empty cell resting above it. */
#define BB_FLOOR_MV     3400
#define BB_SAMPLES      5       /* ADC conversions, median taken */
#define BB_MV_MIN       2500    /* below: not a reading (ADC or I2C fault) */
#define BB_MV_MAX       5000    /* above: not a reading */

enum bb_verdict {
    BB_BOOT = 0,        /* at or above the floor, not drained */
    BB_BOOT_POWER,      /* external power (charger, USB): boot as always */
    BB_BOOT_NOREAD,     /* no valid reading, not drained: never strand */
    BB_REFUSE,          /* below the floor on battery: screen, off */
    BB_REFUSE_DRAINED,  /* the drained latch on battery: screen, off */
    BB_BOOT_CHARGED,    /* drained, but the rest shows a charge the
                         * firmware never saw: boot, clear */
};

/* A charge taken while the firmware was off (Apple's firmware, a charger
 * that never booted ours) clears the latch when the stage-1 rest median
 * is at least this far over the 0 % point (the larger of the one stored
 * with the flag and the built-in table's): about 3,720 mV on flash. An
 * empty cell rested at 3,569-3,593 mV [silicon, owner rundown
 * 2026-10-22], 120 mV or more under it, so only a real charge passes. */
#define BB_RECHARGE_MV  300

/* stage 1, before storage. mv[]: n raw readings (sorted in place);
 * *median_out may be NULL. Never BB_REFUSE_DRAINED. */
enum bb_verdict bb_decide(int *mv, int n, bool power, int *median_out);
/* stage 2, after the mount: the latch applied to stage 1's verdict.
 * Refuses a drained cell on battery (a reading or none); power and an
 * already refused verdict pass unchanged. */
enum bb_verdict bb_latch(enum bb_verdict v, bool drained);
/* stage 2 with the charge rule: as bb_latch(), but a drained cell whose
 * rest median reaches max(stored_zero, builtin_zero) + BB_RECHARGE_MV
 * boots as BB_BOOT_CHARGED (a valid reading on battery only) */
enum bb_verdict bb_latch_rest(enum bb_verdict v, bool drained, int rest_mv,
                              int stored_zero, int builtin_zero);
int bb_recharge_mv(int stored_zero, int builtin_zero);
/* true when the verdict clears the latch (external power seen) */
bool bb_clears(enum bb_verdict v);
bool bb_refused(enum bb_verdict v);
/* median of n (1..BB_SAMPLES) readings; sorts mv[] in place */
int bb_median(int *mv, int n);
const char *bb_verdict_name(enum bb_verdict v);

/* When the latch is written (owner decision 2026-10-22, second round):
 * the displayed level reaches 0 %, i.e. the filtered voltage at or below
 * the 0 % point of the discharge table in use, on battery, while writes
 * are still allowed (over disk-safe), once per discharge (not latched
 * yet). A normal safe write, before the cell can hard-cut. */
bool bb_zero_due(int filtered_mv, int zero_mv, bool power, bool safe,
                 bool latched);
/* The backup at the low-battery shutdown: only while writes are allowed
 * and not already latched (at the shutoff it is normally neither). */
bool bb_shutdown_due(bool safe, bool latched);

/* The latch record: the first bytes of a one-sector file written in
 * place (bootbatt.c). A torn or foreign record reads as not drained (the
 * floor still guards). */
#define BB_LATCH_MAGIC  0x4c44424bu     /* "KBDL" */
struct bb_latch_rec {
    uint32_t magic;
    uint32_t drained;                   /* 0 or 1 */
    uint32_t seq;                       /* bumped by every write */
    uint32_t zero_mv;                   /* the 0 % point when set, or 0 */
    uint32_t sum;                       /* of the four above */
};
void bb_latch_fill(struct bb_latch_rec *r, bool drained, uint32_t seq,
                   int zero_mv);
/* 1 drained, 0 clear, -1 not a valid record */
int bb_latch_parse(const struct bb_latch_rec *r);

#endif
