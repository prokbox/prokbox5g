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
 * bcmfw timing bench (BCMFW_BENCH builds): the generated tables
 * (bench_tab.c, from gen_bench.py), the scalar checks (bench_scalar.S)
 * and the results block the host reads (bringup.md, "Device session 2").
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
#ifndef BCMFW_BENCH_H
#define BCMFW_BENCH_H

#include "config.h"

/* One timing kernel: tag (4 ASCII characters, big-endian), instructions
 * per loop iteration (body + per-iteration setup + the loop branch), the
 * measured instructions and their vector rows (REP summed) per iteration */
struct bench_kdesc
{
    uint32_t tag;
    uint32_t per_iter;
    uint32_t ops;
    uint32_t rows;
};

/* One vector check: input block (32-byte rows) and expected output */
struct bench_vdesc
{
    uint32_t tag;
    const uint32_t *in;
    const uint32_t *want;
    uint32_t words;
    uint32_t info;      /* informational: reported, not in BR_VEC_ALL */
};

extern const uint32_t bench_nkernels;
extern const uint32_t bench_nvcases;
extern const struct bench_kdesc bench_kdesc[];
extern const struct bench_vdesc bench_vdesc[];

/* Kernel k, n iterations: returns its own time on the 1 MHz counter */
uint32_t bench_time(uint32_t k, uint32_t n);
/* Vector check k: in and out 32-byte aligned */
void bench_vcase(uint32_t k, const uint32_t *in, uint32_t *out);
/* bench_scalar.S: bit n set = scalar check n passed */
#define BENCH_SCALAR_ALL 0x1FF
uint32_t bench_scalar_check(void);

void bench_run(int timer_ok);

/* ---- The results block at BCMFW_BENCH_RESULTS (words) ----------------- */
#define BR_MAGIC        0   /* BCMFW_BENCH_MAGIC */
#define BR_STATE        1   /* 1 running, 2 done */
#define BR_NKERNELS     2
#define BR_ITERS        3   /* loop iterations per timed run */
#define BR_RUNS         4   /* timed runs per kernel */
#define BR_FLAGS        5   /* BRF_* */
#define BR_VEC_PASS     6   /* vector checks: bit k = required check k passed */
#define BR_VEC_ALL      7   /* the required checks (informational ones left out) */
#define BR_SCALAR_PASS  8   /* scalar checks (bench_scalar.S) */
#define BR_SCALAR_ALL   9
#define BR_PROBE_PASS   10  /* the probe's ten checks (probe.S), again */
#define BR_PROBE_ALL    11
#define BR_FIRST_FAIL   12  /* check << 16 | word, or 0xFFFFFFFF */
#define BR_FAIL_GOT     13
#define BR_FAIL_WANT    14
#define BR_REG_BEFORE   15  /* PLL build: 0x10001400 before the switch */
#define BR_REG_AFTER    16  /*   and after */
#define BR_PLLA_M       17  /*   PLL A divider, control and the core */
#define BR_PLLA_CTL     18  /*   clock select, read back after it */
#define BR_CLK_SEL      19
#define BR_TABLE1       20  /* word offset of the timing table */
#define BR_TABLE2       21  /* PLL build: the table at 150 MHz, else 0 */
#define BR_ENTRY_WORDS  22  /* 8 */
#define BR_VEC_COUNT    23
#define BR_VREC         24  /* word offset of the vector check records, 0 if none */
#define BR_VREC_WORDS   25  /* words per record (BV_WORDS) */
#define BR_VEC_LATE     26  /* bit k = check k passed on the delayed re-read */
#define BR_VEC_PASS_HI  27  /* checks 32-63: BR_VEC_PASS, BR_VEC_ALL and */
#define BR_VEC_ALL_HI   28  /*   BR_VEC_LATE for them (bit k - 32) */
#define BR_VEC_LATE_HI  29
#define BR_SLOC         30  /* word offset of the store-locating records, 0 none */
#define BR_SLOC_COUNT   31
#define BR_HEADER_WORDS 32

/* Vector check records (BR_VREC, one per check, BV_WORDS each), after
 * the timing tables: what each check stored, read at once and again
 * after BENCH_LATE_SPIN spin iterations (a posted vector store shows as
 * a late pass).  bringup.md, "Vector diagnostics". */
#define BR_VREC_AT      0x800   /* word offset: past both timing tables */
#define BENCH_LATE_SPIN 20000   /* about 2 ms at 27 MHz */
#define BV_TAG          0
#define BV_FLAGS        1   /* BVF_*, words checked << 8, first bad word << 16 */
#define BV_GOT          2   /* out[0..3], read right after the check returns */
#define BV_LATE         6   /* out[0..3], read again after the delay */
#define BV_BAD_GOT      10  /* the first mismatching word, immediate read */
#define BV_BAD_WANT     11  /*   and what the model expects there */
#define BV_WORDS        12
#define BVF_PASS        0x1 /* every word matched at once */
#define BVF_LATE_PASS   0x2 /* every word matched after the delay */
#define BVF_NONE        0xFFFF  /* first bad word field: no mismatch */

/* Store-locating records (BR_SLOC, BS_WORDS each, dev4, sources dev5;
 * isa.md "Vector stores on silicon"): the sources in gen_bench.py's
 * SLOC_SRCS hold tagged halfwords (((x = 32 ? 0x40 : 0x80) | y) << 8 |
 * lane), one vst16 from one source to bench_vbuf + bench_sloc_target
 * (silicon layout; the last record uses the old load-style d08); then,
 * after BENCH_LATE_SPIN, every word of
 * bench_vbuf (bench_vbuf_bytes) that is not the fill.  Offsets are bytes from
 * the target, signed. */
#define BR_SLOC_AT      0xB00   /* word offset: past 64 vector records */
#define BS_ROW          0   /* 0x534C0000 ('SL') | source: y | x << 8,
                               | 0x8000 for the old load-style encoding */
#define BS_CHANGED      1   /* words in the buffer that changed */
#define BS_FIRST        2   /* offset of the first changed word */
#define BS_LAST         3   /* offset of the last changed word */
#define BS_W_FIRST      4   /* the first changed word */
#define BS_W_TARGET     5   /* the word at the target itself */
#define BS_W_LAST       6   /* the last changed word */
#define BS_SPAN         7   /* the buffer: bytes before the target << 16 | after */
#define BS_WORDS        8
#define BS_MAGIC        0x534C0000

extern const uint32_t bench_nsloc;
extern const uint32_t bench_sloc_target;
extern const uint32_t bench_vbuf_bytes;
extern const uint16_t bench_sloc_src[];
extern uint32_t bench_vbuf[];
/* Store-locating check k: one vst16 to target (bench_gen.S) */
void bench_sloc(uint32_t k, uint32_t *target);

#define BRF_TIMER_OK    0x1     /* the 1 MHz counter runs */
#define BRF_PLL_BUILD   0x2     /* built with BCMFW_BENCH_PLL */
#define BRF_PLL_DONE    0x4     /* the clock switch ran to its end */
#define BRF_PLL_STALL   0x8     /* a wait in it ended on its spin limit:
                                   the counter stopped */

/* Timing entry (8 words) */
#define BE_TAG          0
#define BE_INSNS        1   /* instructions in the timed loop (iters x per_iter) */
#define BE_OPS          2   /* measured instructions */
#define BE_ROWS         3   /* their vector rows */
#define BE_MIN          4   /* fastest run, 1 MHz counter ticks */
#define BE_MAX          5   /* slowest run */
#define BE_PER_ITER     6
#define BE_WORDS        8

#endif /* BCMFW_BENCH_H */
