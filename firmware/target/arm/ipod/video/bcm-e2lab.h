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
 * The second DMA engine lab (E2-0 ... E2-7; docs/data-paths.md section
 * 7.1 on hw-experiments, pp5022-reference/dma.md section 6): device test
 * builds only (HAVE_BCMFW_DEVTEST with HAVE_BCMFW_VIDEO).
 *
 * The first half is pure logic (patterns, the readback's classes, the
 * command words, piece splitting, the allowed targets), with no Rockbox
 * header, so tests/e2lab tests it on the host.  The second half is the
 * hardware layer at the end of bcm-devtest-hal.c, which the stage in
 * apps/ipodui/devbcm.c drives.
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
#ifndef BCM_E2LAB_H
#define BCM_E2LAB_H

#include <stdint.h>
#include <stdbool.h>

/* ---- Sizes and windows ------------------------------------------------- */

#define E2LAB_PIECE_MAX   0x10000   /* one piece: the port's 64 KB window */
#define E2LAB_BULK_ALIGN  256       /* Apple: the DMA part is a multiple */
#define E2LAB_GUARD_WORDS 16        /* pattern G before and after a target */
#define E2LAB_TIMEOUT_US  50000     /* a piece's bound (section 7.1) */
#define E2LAB_MAX_WORDS   (E2LAB_PIECE_MAX / 4)

/* The BCM's scratch windows (host-port addresses, guards outside them):
 * HELD: held between bootstrap and start, no image (S35's area);
 * IDLE: our video image running and idle: above VIDEO_FB (0x12A800), the
 *       tone op's buffer (used only while that op runs), below the end
 *       of SRAM (0x140000);
 * SDRAM: inside the open session's bitstream FIFO, which nothing reads
 *       before a FEED (bcmfw/video/video.h) */
#define E2LAB_WIN_HELD    0x0E0000u
#define E2LAB_WIN_IDLE    0x12F000u
#define E2LAB_WIN_SDRAM   0xC0040000u
#define E2LAB_WIN_SIZE    E2LAB_PIECE_MAX

/* ---- Patterns ----------------------------------------------------------
 * Word i of pattern P under a run's seed: the high half is P's tag and
 * the seed, the low half the index XOR P's mask, so no word of S equals
 * the T or G word at the same index (either half), and S words name their
 * index (a shifted word tells by how much).  The seed changes every run
 * (12 bits, never 0), so no earlier run's data matches. */
enum { E2_PAT_T = 0xA, E2_PAT_S = 0x5, E2_PAT_G = 0xC, E2_PAT_C = 0x3 };

static inline uint32_t e2_pat(unsigned pat, uint32_t seed, uint32_t i)
{
    uint32_t hi = (uint32_t)(pat & 0xF) << 12 | (seed & 0xFFF);
    uint32_t lo = (i ^ (0x1111u * (pat & 0xF))) & 0xFFFF;

    return hi << 16 | lo;
}

static inline uint32_t e2_seed(uint32_t entropy)
{
    uint32_t s = (entropy ^ entropy >> 12 ^ entropy >> 24) & 0xFFF;

    return s ? s : 0x5EE;
}

/* Fill n words of pattern pat from index first */
static inline void e2_fill(uint32_t *w, uint32_t n, unsigned pat,
                           uint32_t seed, uint32_t first)
{
    uint32_t i;

    for (i = 0; i < n; i++)
        w[i] = e2_pat(pat, seed, first + i);
}

/* ---- Classifying a readback --------------------------------------------- */
enum { E2_OK, E2_UNTOUCHED, E2_SHIFTED, E2_GUARDLIKE, E2_OTHER };

struct e2_tally
{
    uint32_t n;                 /* words checked */
    uint32_t ok, untouched, shifted, guardlike, other;
    int32_t first_bad;          /* index of the first wrong word, -1 none */
    int32_t first_d;            /* the first shifted word: i - j, 0 none */
    uint32_t first_other;       /* the first "other" word as read */
    int32_t first_other_i;      /* and its index, -1 none */
    int32_t last_ok;            /* index of the last correct word, -1 none */
    int32_t w0_j;               /* the S word word 0 holds, -1 none */
};

/* Word i of n read back as got after S was moved over T: its class, and
 * for E2_SHIFTED the index j of the S word it holds */
static inline int e2_class(uint32_t got, uint32_t seed, uint32_t i,
                           uint32_t n, uint32_t *j)
{
    uint32_t k;

    if (got == e2_pat(E2_PAT_S, seed, i))
        return E2_OK;
    if (got == e2_pat(E2_PAT_T, seed, i))
        return E2_UNTOUCHED;
    if ((got >> 16) == (e2_pat(E2_PAT_S, seed, 0) >> 16))
    {
        k = (got ^ 0x5555u) & 0xFFFF;
        if (k < n)
        {
            *j = k;
            return E2_SHIFTED;
        }
    }
    if ((got >> 16) == (e2_pat(E2_PAT_G, seed, 0) >> 16))
        return E2_GUARDLIKE;
    return E2_OTHER;
}

static inline void e2_tally_init(struct e2_tally *t)
{
    t->n = t->ok = t->untouched = t->shifted = t->guardlike = t->other = 0;
    t->first_bad = t->first_other_i = t->last_ok = -1;
    t->first_d = 0;
    t->first_other = 0;
    t->w0_j = -1;
}

static inline void e2_tally_add(struct e2_tally *t, uint32_t got,
                                uint32_t seed, uint32_t i, uint32_t n)
{
    uint32_t j = 0;
    int c = e2_class(got, seed, i, n, &j);

    t->n++;
    if (i == 0 && (c == E2_OK || c == E2_SHIFTED))
        t->w0_j = c == E2_OK ? 0 : (int32_t)j;
    if (c == E2_OK)
    {
        t->ok++;
        t->last_ok = (int32_t)i;
        return;
    }
    if (t->first_bad < 0)
        t->first_bad = (int32_t)i;
    switch (c)
    {
    case E2_UNTOUCHED:
        t->untouched++;
        break;
    case E2_SHIFTED:
        if (!t->shifted++)
            t->first_d = (int32_t)i - (int32_t)j;
        break;
    case E2_GUARDLIKE:
        t->guardlike++;
        break;
    default:
        if (!t->other++)
        {
            t->first_other = got;
            t->first_other_i = (int32_t)i;
        }
        break;
    }
}

static inline bool e2_tally_pass(const struct e2_tally *t)
{
    return t->n > 0 && t->ok == t->n;
}

/* Only word 0 holds an S word (the last one written, if every write
 * went there), the rest untouched: the destination never moved (fine for
 * the port, one register for 64 KB) */
static inline bool e2_tally_fixed_dst(const struct e2_tally *t)
{
    return t->n > 1 && t->w0_j >= 0 && t->untouched == t->n - 1;
}

/* Partial delivery (S32/S33): some S words landed but not all in place */
static inline bool e2_tally_partial(const struct e2_tally *t)
{
    return t->shifted > 0 || (t->ok > 0 && t->ok < t->n);
}

/* Guards: 2 x E2LAB_GUARD_WORDS words, G indices 0..31; count changed */
static inline uint32_t e2_guard_hits(const uint32_t *before,
                                     const uint32_t *after, uint32_t seed)
{
    uint32_t k, bad = 0;

    for (k = 0; k < E2LAB_GUARD_WORDS; k++)
    {
        bad += before[k] != e2_pat(E2_PAT_G, seed, k);
        bad += after[k] != e2_pat(E2_PAT_G, seed, E2LAB_GUARD_WORDS + k);
    }
    return bad;
}

/* ---- Pieces ------------------------------------------------------------- */

/* Apple's split: the bulk (a multiple of 256) by DMA, the tail by CPU */
static inline uint32_t e2_bulk(uint32_t bytes)
{
    return bytes & ~(uint32_t)(E2LAB_BULK_ALIGN - 1);
}

/* The next piece of a bulk transfer, done bytes in */
static inline uint32_t e2_piece_len(uint32_t bulk, uint32_t done)
{
    uint32_t left = bulk > done ? bulk - done : 0;

    return left > E2LAB_PIECE_MAX ? E2LAB_PIECE_MAX : left;
}

/* ---- The second engine's command words (dma.md section 6.3) ------------ */
#define E2_CMD_START    (1u << 31)
#define E2_CMD_INTR     (1u << 30)
#define E2_CMD_B28      (1u << 28)
#define E2_CMD_B27      (1u << 27)
#define E2_CMD_B26      (1u << 26)
#define E2_CMD_B25      (1u << 25)
#define E2_CMD_WAITREQ  (1u << 24)
#define E2_CMD_OPT      (0xFu << 20)
#define E2_CMD_REQ      (0xFu << 16)
#define E2_CMD_COUNT    0xFFFFu
#define E2_CFG_CODE(c)  (((uint32_t)(c) & 7) << 24)
#define E2_CFG_CODE_MASK (7u << 24)

/* Channel init by read-modify-write: clear 31, 30, 28, 25, 24, [23:16];
 * set 26 */
static inline uint32_t e2_cmd_init(uint32_t v)
{
    v &= ~(E2_CMD_START | E2_CMD_INTR | E2_CMD_B28 | E2_CMD_B25 |
           E2_CMD_WAITREQ | E2_CMD_OPT | E2_CMD_REQ);
    return v | E2_CMD_B26;
}

/* One piece: the count field bytes - 4, bit 27 (Apple: always; E2-5's
 * last arm: clear) */
static inline uint32_t e2_cmd_piece(uint32_t v, uint32_t bytes, bool b27)
{
    v = (v & ~E2_CMD_COUNT) | ((bytes - 4) & E2_CMD_COUNT);
    return b27 ? v | E2_CMD_B27 : v & ~E2_CMD_B27;
}

static inline uint32_t e2_cfg_code(uint32_t v, unsigned code)
{
    return (v & ~E2_CFG_CODE_MASK) | E2_CFG_CODE(code);
}

/* A piece is programmed only for: 4..64 KB, a multiple of 4; a word-
 * aligned source; a destination inside [lo, hi) for its whole length;
 * never a PP peripheral (0x7xxx_xxxx) */
static inline bool e2_target_ok(uint32_t dst, uint32_t bytes, uint32_t lo,
                                uint32_t hi)
{
    if (bytes < 4 || bytes > E2LAB_PIECE_MAX || (bytes & 3) || (dst & 3))
        return false;
    if ((dst >> 28) == 0x7 || ((dst + bytes - 1) >> 28) == 0x7)
        return false;
    return dst >= lo && dst < hi && bytes <= hi - dst;
}

/* ---- The hardware layer (bcm-devtest-hal.c) ----------------------------- */

#define E2LAB_DUMP_WORDS  104   /* 0x60008000-0C, 0x60009000-FF,
                                   0x6000A000-0C, 0x6000B000-7F */

enum { E2LAB_ENG2, E2LAB_CH3 };
enum { E2LAB_WORK_POLL, E2LAB_WORK_IRAM, E2LAB_WORK_SDRAM };

/* What ended a piece's wait (e2_piece.how): STATUS bit 30 set, STATUS
 * bit 31 seen set then clear, CMD bit 31 seen set then clear, INT_STAT
 * bits 26-28 changed */
#define E2LAB_HOW_ST30  1u
#define E2LAB_HOW_ST31  2u
#define E2LAB_HOW_CMD31 4u
#define E2LAB_HOW_INT   8u

struct e2_piece
{
    /* in */
    int engine;                 /* E2LAB_ENG2 or E2LAB_CH3 */
    const void *src;            /* CPU address, word aligned, SDRAM */
    void *dst;                  /* CPU address (SDRAM), when !port */
    bool port;                  /* to the host port's DATA register */
    uint32_t bcm_addr;          /* port: where it lands in the BCM */
    uint32_t lo, hi;            /* allowed: dst (physical) or bcm_addr */
    uint32_t bytes;
    unsigned src_code, dst_code;    /* engine 2: [26:24] (Apple: 2, 2) */
    bool b27;                   /* engine 2: bit 27 (Apple: set) */
    bool intr;                  /* engine 2: bit 30 (Apple: set; never
                                   with audio playing) */
    bool audio;                 /* leave source 26 (the audio FIQ) alone */
    uint32_t flags, incr;       /* channel 3's FLAGS and INCR */
    bool wait_ready;            /* port: write-ready wait before start */
    int work;                   /* E2LAB_WORK_*: count while it runs */
    volatile uint32_t *work_word;   /* WORK_SDRAM: an uncached word */
    uint32_t done_how;          /* WORK_*: the signal to wait for */
    uint32_t done_int;          /* ... for E2LAB_HOW_INT: the bits */
    /* out */
    int rc;                     /* 0 done, 1 timeout, 2 done but its event
                                   stays raised (sources left masked),
                                   -1 refused, -2 port */
    uint32_t us;                /* start to the end seen */
    uint32_t how, t_how[4];     /* E2LAB_HOW_* seen, and when (us) */
    uint32_t st_pre[2], st_post[2];     /* STATUS before start, after */
    uint32_t cmd_written, cmd_post;
    uint32_t sv[4], sv_t[4], nsv;   /* STATUS values seen, first 4 */
    uint32_t int_pre, int_post; /* INT_STAT & bits 26-28 */
    uint32_t en_masked;         /* CPU_INT_EN bits 26-28 masked for it */
    uint32_t count;             /* WORK_*: loop iterations */
    uint32_t iis_max_free;      /* WORK_*: the I2S TX FIFO's emptiest */
    uint32_t ready_us;          /* port: write-ready after the end */
    long freq;                  /* the PP's clock at the start */
};

/* E2-0: the read-only dump, E2LAB_DUMP_WORDS words in the order above */
void bcm_e2_dump(uint32_t *out);
/* Save the registers the lab may change (both engines' masters, engine
 * 2's channel 0, general channel 3); restore them, with both engine-2
 * channels' bits 30/31 clear, and any interrupt source a piece left
 * masked back on unless it is still raised.  final[]: engine-2 master,
 * channel 0 CMD, channel 1 CMD, general master, after the restore;
 * INT_STAT bits 26-28 at the save and now (a source raised now that was
 * not then: reboot before playing audio). */
void bcm_e2_save(void);
void bcm_e2_restore(uint32_t final[6]);
/* One piece, every wait bounded (E2LAB_TIMEOUT_US); on a timeout the
 * channel is stopped in Apple's order with IRQ and FIQ masked */
int bcm_e2_piece(struct e2_piece *p);
/* The counting loop alone (in IRAM), for limit_us: the idle baseline.
 * out[0] us, out[1] the I2S TX FIFO's emptiest */
uint32_t bcm_e2_spin(volatile uint32_t *word, uint32_t limit_us,
                     uint32_t out[2]);
/* The BCM held as Apple holds it: after bcm_video_take() and
 * bcm_video_off(), power on and the bootstrap, no image, no start.
 * 0, or < 0 (the BCM is then cut).  bcm_e2_cut(): power off. */
int bcm_e2_hold(void);
void bcm_e2_cut(void);
/* The CPU's copy of the same bytes into the port (R1), timed */
int bcm_e2_cpu_copy(uint32_t bcm_addr, const void *src, uint32_t bytes,
                    uint32_t *us);

#endif /* BCM_E2LAB_H */
