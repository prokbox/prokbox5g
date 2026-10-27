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

#ifndef BCMFW_HIBWAKE_H
#define BCMFW_HIBWAKE_H

#include <stdint.h>

/* The instant wake after dark: the hibernate park for the release line
 * (docs/research/bcm2722/power-contract.md section 6.6).  Constants and
 * the sum, shared by the stub (bcmfw/hibstub.c), the jobs images' PARK hook
 * (hostproto.c, BCMFW_HIB_PARK), the PP's sequence and decision rule
 * (firmware/target/arm/ipod/video/bcm-hibwake.h) and the tests.
 *
 * NOT FOR SILICON until PW5 passes on the unit (the PP's flag, the same
 * discipline as bcmfw/verified.txt): nothing here has run on a device.
 *
 * How it goes:
 *  1. The PP uploads the stub (bcmhib.bin, built from hibstub.c, linked
 *     at HW_STUB) once per BCM start, writes the parameter block at
 *     HW_PARAM (which regions to check), writes 0 to HW_WORD and sends
 *     HP_OP_PARK with HP_PARK_MAGIC, HP_PARK_HIB.
 *  2. The image refuses (HP_E_STATE, nothing done) without the stub's
 *     magic at HW_STUB, off the boot clock, with a session open, in a
 *     standby or, in the merged image, with SDRAM up.  Otherwise it
 *     replies OK and, after the ring pass, waits for the panel's
 *     transfer and calls the stub's entry.
 *  3. The stub sums the regions (HW_P_PRE_*), parks as PW5 does (Apple's
 *     videocore=1: the PLLs B, C, D, A to pw 0, the 14 GPIO event words
 *     saved and cleared), writes HP_HIB_PARKED and polls for HP_HIB_WAKE.
 *     The PP then sets GPO32 bit 15, waits 5 ms, clears bit 14 (Apple's
 *     retain order; no port access until bit 15 is low again).
 *  4. Wake: bit 14 high, 5 ms, bit 15 low; the PP reads HW_WORD (it must
 *     still say HP_HIB_PARKED), writes HP_HIB_WAKE.  The stub restores
 *     the PLLs (Apple's rule, @4cb8) and the event words, re-arms the
 *     idle tick, sums the regions again and answers:
 *       HP_HIB_AWAKE  the sums match: it returns into the image, which
 *                     runs on with J_EPOCH, FRONT, BACK and the slots
 *                     as they were;
 *       HW_SPOILED    a sum differs: it never returns (it spins), the PP
 *                     cuts and cold-boots (a new epoch).  A bad hold
 *                     costs a cold frame, never a wrong one.
 *     Anything else (no PARKED after the pins, no answer in time,
 *     HP_HIB_GAVEUP, HW_REFUSED) is a cold boot too.
 *
 * HW_WORD is PW5's (Apple's 0x1F0, BCMFW_MB_RECT_XY, idle in UI mode
 * between UPDATERECTs) with PW5's values; HW_SPOILED and HW_REFUSED are
 * ours. */

/* Where: the spare SRAM between the warm swap's park stub (0xDE900, 256
 * B) and the video guard (0xDF000, video mode only); memory-contract.md
 * section 2 */
#define HW_STUB              0x0DEA00    /* the stub's header, then code */
#define HW_STUB_MAX          0x600       /* to 0xDF000 */
#define HW_PARAM             0x0DF400    /* the parameter block */
#define HW_PARAM_MAX         0x100

#define HW_WORD              0x1F0       /* = HP_HIB_WORD */
#define HW_SPOILED           0xA5A500EE  /* a region's sum changed */
#define HW_REFUSED           0xA5A500FE  /* a bad block: nothing parked */

/* The stub's header, at HW_STUB (its .vectors) */
#define HW_STUB_MAGIC        0x48575342  /* "HWSB" */
#define HW_STUB_VERSION      1
#define HW_S_MAGIC           0x00
#define HW_S_VERSION         0x04
#define HW_S_BASE            0x08        /* HW_STUB */
#define HW_S_LOAD_END        0x0C        /* HW_STUB + the bytes uploaded */
#define HW_S_ENTRY           0x10        /* void entry(void) */

/* The parameter block, at HW_PARAM: words the PP writes, then words the
 * stub writes */
#define HW_PARAM_MAGIC       0x48575041  /* "HWPA" */
#define HW_REGIONS           8
#define HW_P_MAGIC           0x00        /* PP: HW_PARAM_MAGIC */
#define HW_P_WAIT_MS         0x04        /* PP: the stub's own bound on the
                                            park, 1 .. HW_WAIT_MS_MAX */
#define HW_P_NREG            0x08        /* PP: regions used, 1 .. 8 */
#define HW_P_REG             0x0C        /* PP: 8 x {base, bytes, stride}:
                                            word-aligned, bytes a multiple
                                            of 4 and > 0, stride in words
                                            (1: every word; 16: one word
                                            in 16), all inside SRAM */
#define HW_REG_WORDS         3
#define HW_P_EXIT            0x6C        /* BCM: HW_X_* */
#define HW_P_PRE_A           0x70        /* BCM: the sums at the park */
#define HW_P_PRE_B           0x74
#define HW_P_POST_A          0x78        /* BCM: ... after the wake */
#define HW_P_POST_B          0x7C
#define HW_P_PARK_US         0x80        /* BCM: entry to HP_HIB_PARKED */
#define HW_P_HELD_US         0x84        /* BCM: parked to the wake word */
#define HW_P_RESTORE_US      0x88        /* BCM: wake word to the answer */
#define HW_P_SUM_US          0x8C        /* BCM: the second sum's part */
#define HW_P_POLLS           0x90        /* BCM: reads of the word parked */
#define HW_P_SAVED           0x94        /* BCM: the 22 saved words (8 PLL
                                            words in the park's order, then
                                            the 14 event words) */
#define HW_SAVED_N           22
#define HW_P_END             (HW_P_SAVED + 4 * HW_SAVED_N)    /* 0xEC */

#define HW_WAIT_MS_MAX       3600000     /* an hour: the PP cuts long before */

#define HW_X_OK              0
#define HW_X_GAVEUP          1           /* no wake word within the bound */
#define HW_X_STALL           2           /* the counter stopped (not dark) */
#define HW_X_SPOILED         3           /* a sum differs after the wake */
#define HW_X_REFUSED         4           /* the block was bad, or the core
                                            off the crystal: nothing done */

/* The PARK request's second word (hostproto.h HP_OP_PARK) */
#define HP_PARK_HIB          0x48494252  /* "HIBR" */

/* The sum: Fletcher-like over 32-bit words, every stride-th word of each
 * region in order, from a = b = 0: a += w; b += a (mod 2^32).  Losing a
 * region (all zeros, all ones, noise) changes it with near certainty;
 * any single changed word that is summed changes a. */
static inline void hw_sum(uint32_t *a, uint32_t *b, const volatile uint32_t *w,
                          uint32_t words, uint32_t stride)
{
    uint32_t sa = *a, sb = *b, i = 0;

    if (stride == 1)        /* four a turn: the loop's own cost halved */
        for (; i + 4 <= words; i += 4)
        {
            sa += w[i];
            sb += sa;
            sa += w[i + 1];
            sb += sa;
            sa += w[i + 2];
            sb += sa;
            sa += w[i + 3];
            sb += sa;
        }
    for (; i < words; i += stride)
    {
        sa += w[i];
        sb += sa;
    }
    *a = sa;
    *b = sb;
}

#endif /* BCMFW_HIBWAKE_H */
