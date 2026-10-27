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
 * Bench row ALGN (bench builds): the alignment checks, one table of small
 * routines run on the host's CMD_ALGN, each fault caught and recorded.
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
#ifndef BCMFW_ALGN_H
#define BCMFW_ALGN_H

#include "config.h"

/* docs/research/bcm2722/bench-p19.md "ALGN"; memory-access.md "Alignment".
 *
 * The host sends CMD_ALGN (after stage 4's bench is done and logged); the
 * image runs every check of algn_tab (algn.S) in order and answers.  A
 * check is a leaf routine that keeps its results in r2-r5 and touches only
 * r0-r5; a fault inside it (or within the few instructions an imprecise
 * exception may still run) is caught by crt0.S's hook, which records the
 * vector, the saved pc and r0-r5 and resumes in bcmfw_algn_call with the
 * check's sp.  So no check can stop the image; a hang (a bus that never
 * answers) leaves the block's step word on its check, and the host's
 * command timeout says "stopped at check k".
 *
 * Adding a check: a routine and a table line in algn.S (or a C check in
 * algn.c), and its readings in bcmfw/bench/readlog.py ALGN_READINGS.  The
 * block is self-describing (count and record size in the header), so the
 * host side never changes. */

#define CMD_ALGN             0x45

#define ALGN_RESULTS         (BCMFW_BENCH_RESULTS + 0x3000)  /* bench.h: past
                                                    the store-locating records */
#define ALGN_MAGIC           0x414C4731  /* 'ALG1' */
#define ALGN_VERSION         1
#define ALGN_HDR_WORDS       8
#define ALGN_REC_WORDS       16
#define ALGN_MAX             60          /* records that fit before 0xC4000 */
#define ALGN_WIN_WORDS       8           /* buffer words a record keeps */

/* Header words */
#define AH_MAGIC             0
#define AH_FORMAT            1   /* n | REC_WORDS << 8 | VERSION << 16 */
#define AH_STATE             2   /* 1 running, 2 done */
#define AH_STEP              3   /* the check running (its index) */
#define AH_FAULTS            4   /* faults caught over the run */
#define AH_SRC               5   /* the source pattern's address */
#define AH_BUF               6   /* the target buffer's address */

/* Record words */
#define AR_TAG               0   /* 4 ASCII characters, big-endian */
#define AR_STATUS            1   /* state | vector << 8 | faults << 16 */
#define AR_PC                2   /* saved pc of the (first) fault, or 0 */
#define AR_AT                3   /* the access instruction's address
                                    (DMA checks: the control block's) */
#define AR_REGS              4   /* r2-r5 at return or at the fault */
#define AR_BUF               8   /* buffer words win .. win + 7 */

#define ALGN_ST_RAN          1   /* returned with no fault */
#define ALGN_ST_FAULT        3   /* faulted; resumed by the hook */
#define ALGN_ST_TIMEOUT      4   /* a DMA check's wait ran out */

/* Table entries (algn.S): tag, routine, access address, flags | window */
#define ALGN_E_WORDS         4
#define ALGN_F_C             0x100   /* a C routine (algn.c), no fault hook */
#define ALGN_WIN(e)          ((e) & 0xFF)

#define ALGN_SRC_BYTES       128     /* the source pattern: byte k = 0x10 + k */
#define ALGN_BUF_WORDS       32      /* the target, filled per check */
#define ALGN_FILL(i)         (0xA5A5A5A0u + (i))

#ifndef __ASSEMBLER__

void algn_run(void);
/* algn.S */
extern const uint32_t algn_tab[];
extern uint8_t algn_src[];
extern uint32_t algn_buf[];
extern uint32_t bcmfw_algn_regs[4];
extern uint32_t bcmfw_algn_fault[9];    /* vector, pc, r0-r5, count */
uint32_t bcmfw_algn_call(uint32_t fn);  /* 0 returned, 1 faulted */
#endif

#endif /* BCMFW_ALGN_H */
