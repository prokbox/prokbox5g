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
 * H.264 decoder internals: parameter sets, slice header, picture buffers,
 * per-macroblock state, the bit reader, and the functions shared between
 * the decoder's files.  Clause numbers refer to ITU-T H.264.
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
#ifndef H264_INT_H
#define H264_INT_H

#include "h264.h"
#include "h264_dsp.h"

/* The last refusal (H264_E_UNSUPPORTED): file << 16 | line, the file 1
 * h264.c, 2 ps.c, 3 slice.c (dev32: the image reports it in its guard
 * block, so a refused stream names the check) */
/* Any other error: the last one's file << 16 | line (dev37, the guard's
 * first decode error; file 4 is mb.c) */
#define H264_ERR(file, code) \
    (h264_error_at = (uint32_t)(file) << 16 | __LINE__, (code))
#define H264_REFUSE(file) \
    (h264_refused_at = (uint32_t)(file) << 16 | __LINE__, H264_E_UNSUPPORTED)

/* The only libc the decoder uses. */
void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);

#define H264_MAX_SPS       32
#define H264_MAX_PPS       32
#define H264_MAX_FRAMES    (H264_MAX_REF_FRAMES + 1)
#define H264_MAX_REFLIST   32
#define H264_MAX_MMCO      66

/* Keeps a function out of line, so profiles can charge it to its stage. */
#define H264_NOINLINE __attribute__((noinline))
/* Once a stream (the parameter sets): small rather than fast, for the
 * merged image's room below its arena (merged-image-batch.md).  Only GCC
 * knows the optimize attribute; the host tests' clang builds (-Werror)
 * keep the function out of line and nothing more. */
#if defined(__GNUC__) && !defined(__clang__)
#define H264_COLD __attribute__((noinline, optimize("Os")))
#else
#define H264_COLD __attribute__((noinline))
#endif

/* bcmfw (dev11): progress for the host to read when the image stops
 * answering (video.h VIDEO_MB_PROG, phases 0x31 macroblock, 0x32 loop
 * filter row); nothing elsewhere */
#ifdef H264_PROG_WORD
#define H264_PROG(ph, v) (*(volatile uint32_t *)(H264_PROG_WORD) = \
                          (uint32_t)(ph) << 24 | ((uint32_t)(v) & 0xFFFFFF))
#else
#define H264_PROG(ph, v) ((void)0)
#endif

#define CLIP3(lo, hi, v) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))
static inline int clip255(int v)
{
    return (unsigned)v > 255 ? (v < 0 ? 0 : 255) : v;
}
static inline int iabs(int v)
{
    return v < 0 ? -v : v;
}

/* ---- Bit reader ---------------------------------------------------------
 * cache holds the next n (>= 25 after bs_fill) bits, MSB first.  The bits
 * below those n are zero or the stream's next bits (a refill may load a
 * byte it doesn't count yet), so OR-ing the stream in again is harmless.
 * Reads past the end deliver zeros; bs_overrun() tells.  pos is a byte
 * index, so a runaway loop can't form a wild pointer.
 *
 * Hot parsers copy the reader into a local (struct h264_bits r = *b) and
 * back: the byte loads may alias any memory, so a reader reached through a
 * pointer would be stored back after every load. */
struct h264_bits {
    const uint8_t *buf;
    uint32_t pos, size;          /* next byte to load, RBSP size */
    uint32_t cache;
    int n;
    int err;                     /* an Exp-Golomb code was too long */
};

static inline void bs_init(struct h264_bits *b, const uint8_t *buf, int size)
{
    b->buf = buf;
    b->pos = 0;
    b->size = size;
    b->cache = 0;
    b->n = 0;
    b->err = 0;
}

/* the four bytes at pos as a big-endian word, zeros past size */
uint32_t bs_word_tail(const uint8_t *buf, uint32_t pos, uint32_t size);

/* n <= 24: load the next four bytes as one big-endian word and take the
 * whole bytes that fit (1 to 4), so n ends in 25..32.  Nothing here takes
 * the reader's address, so a local copy stays in registers. */
static inline void bs_fill(struct h264_bits *b)
{
    if (b->n <= 24)
    {
        uint32_t pos = b->pos, w;
        int n = b->n, k = (32 - n) >> 3;
        if (pos + 4 <= b->size)
        {
            const uint8_t *p = b->buf + pos;
            w = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
                (uint32_t)p[2] << 8 | p[3];
        }
        else
            w = bs_word_tail(b->buf, pos, b->size);
        b->cache |= w >> n;
        b->pos = pos + k;
        b->n = n + 8 * k;
    }
}

/* bits consumed so far */
static inline uint32_t bs_tell(const struct h264_bits *b)
{
    return b->pos * 8 - b->n;
}

static inline int bs_overrun(const struct h264_bits *b)
{
    return bs_tell(b) > b->size * 8;
}

/* k in 1..25 */
static inline uint32_t bs_peek(struct h264_bits *b, int k)
{
    bs_fill(b);
    return b->cache >> (32 - k);
}

static inline void bs_skip(struct h264_bits *b, int k)
{
    b->cache <<= k;              /* k <= 25 after a fill */
    b->n -= k;
}

/* k in 0..25 */
static inline uint32_t bs_get(struct h264_bits *b, int k)
{
    uint32_t v;
    if (k == 0)
        return 0;
    bs_fill(b);
    v = b->cache >> (32 - k);
    b->cache <<= k;
    b->n -= k;
    return v;
}

static inline uint32_t bs_get1(struct h264_bits *b)
{
    uint32_t v;
    if (b->n == 0)
        bs_fill(b);
    v = b->cache >> 31;
    b->cache <<= 1;
    b->n--;
    return v;
}

uint32_t bs_get_long(struct h264_bits *b, int k);   /* k in 0..32 */
uint32_t bs_ue_slow(struct h264_bits *b);

/* ue(v), 9.1 */
static inline uint32_t bs_ue(struct h264_bits *b)
{
    uint32_t c;
    int lz;
    bs_fill(b);
    c = b->cache;
    if (c >= 0x00080000u)        /* at most 12 leading zeros */
    {
        lz = __builtin_clz(c);
        lz = 2 * lz + 1;
        b->cache = c << lz;
        b->n -= lz;
        return (c >> (32 - lz)) - 1;
    }
    return bs_ue_slow(b);
}

/* se(v), 9.1.1 */
static inline int32_t bs_se(struct h264_bits *b)
{
    uint32_t k = bs_ue(b);
    int32_t v = (int32_t)((k + 1) >> 1), m = (int32_t)(k & 1) - 1;
    return (v ^ m) - m;          /* odd k: +v, even: -v, without a branch */
}

/* ---- CAVLC code tables (vlc_tables.c, generated) --------------------
 * row[lz] = base << 5 | shift: the entry is e[base + (w >> shift)], with
 * e = length << 8 | value; unused words are length 1, value 255 (every
 * caller range-checks the value).  See gen_tables.py. */
struct h264_vlc {
    uint8_t lzmax;
    uint16_t row[17];
    const uint16_t *e;
};

extern const struct h264_vlc h264_vlc_coeff_token0, h264_vlc_coeff_token1,
    h264_vlc_coeff_token2, h264_vlc_coeff_token_cdc;
/* total_zeros by TotalCoeff t, read directly: row = h264_tzd_row[t - 1]
 * (luma t = 1..15, then chroma DC t = 1..3), entry h264_tzd[(row >> 5) +
 * (w >> (row & 31))] = length << 8 | total_zeros, as h264_vlc's entries
 * (generated: gen_tables.py) */
extern const uint16_t h264_tzd[];
extern const uint32_t h264_tzd_row[18];
extern const struct h264_vlc h264_vlc_run_before7;
/* run_before for zerosLeft 1..6 by the next 3 bits: length << 4 | run */
extern const uint8_t h264_rb3[6][8];

/* Read one code (at most 16 bits); returns its value (255: unused word). */
static inline int bs_vlc(struct h264_bits *b, const struct h264_vlc *t)
{
    uint32_t w, e, r;
    int lz;
    bs_fill(b);
    w = b->cache;
    lz = __builtin_clz(w | 1);
    if (lz > t->lzmax)
        lz = t->lzmax;
    r = t->row[lz];
    e = t->e[(r >> 5) + (w >> (r & 31))];
    bs_skip(b, e >> 8);
    return e & 0xff;
}

/* ---- Parameter sets (7.3.2.1, 7.3.2.2), Baseline subset ---------------- */
struct h264_sps {
    uint8_t valid;
    uint8_t profile_idc, level_idc;
    uint8_t log2_max_frame_num;
    uint8_t poc_type, log2_max_poc_lsb;
    uint8_t delta_pic_order_always_zero;
    uint8_t num_ref_frames;
    uint8_t gaps_allowed;
    uint8_t mb_w, mb_h;
    uint16_t crop_l, crop_r, crop_t, crop_b;   /* in luma samples */
};

struct h264_pps {
    uint8_t valid;
    uint8_t sps_id;
    uint8_t bottom_field_pic_order_present;
    uint8_t num_ref_idx_default;      /* l0, minus1 + 1 */
    uint8_t constrained_intra_pred;
    uint8_t deblocking_filter_control_present;
    uint8_t redundant_pic_cnt_present;
    int8_t init_qp;
    int8_t chroma_qp_offset;
};

/* ---- Pictures ------------------------------------------------------------ */
#define REF_NONE   0
#define REF_SHORT  1

struct h264_frame {
    uint8_t *y, *u, *v;
    int frame_num;
    int frame_num_wrap;
    int pic_num;
    uint8_t ref;                 /* REF_* */
    uint8_t id;                  /* index in d->frames */
};

/* Per-macroblock state kept for the whole picture: neighbours read it
 * during decoding, and the loop filter after it. */
#define MBF_INTRA   0x01
#define MBF_I4x4    0x02
#define MBF_I16     0x04
#define MBF_PCM     0x08
#define MBF_SKIP    0x10
#define MBF_UNIFORM 0x20        /* one MV and reference for the whole MB
                                    (P_Skip, P_L0_16x16): bS fast path */
#define MBF_UNI8    0x40        /* one MV and reference per 8x8 (16x8,
                                    8x16, 8x8 without sub-partitions): the
                                    edges inside each 8x8 take bS from the
                                    coefficients only */

struct h264_mbinfo {
    /* the byte arrays first, on word boundaries, so the neighbour caches
     * copy their rows as words (load_neighbours, store_mb) */
    uint8_t nz[24];              /* TotalCoeff: luma raster 0-15, Cb 16-19,
                                    Cr 20-23 */
    int8_t ipm[16];              /* Intra4x4PredMode, raster; 2 (DC) for
                                    other macroblock types */
    int8_t ref[4];               /* refIdxL0 per 8x8 (raster), -1: intra */
    uint8_t refpic[4];           /* frame id per 8x8, for bS (0xff none) */
    uint16_t slice;              /* slice number in the picture; 0xffff:
                                    not decoded */
    uint16_t nzmask;             /* luma 4x4 with coefficients (raster;
                                    inter macroblocks only, the only ones
                                    the loop filter asks) */
    uint8_t flags;               /* MBF_* */
    int8_t qp;                   /* QP'Y for the loop filter (0: I_PCM) */
    int8_t qpc;                  /* QP'C for the loop filter */
    uint8_t dbk_idc;             /* disable_deblocking_filter_idc */
    int8_t alpha_off, beta_off;  /* FilterOffsetA/B */
} __attribute__((aligned(4)));

/* ---- Slice header (7.3.3) -------------------------------------------- */
struct h264_mmco {
    uint8_t op;
    uint32_t arg;
};

struct h264_slice_hdr {
    int nal_ref_idc;
    int idr;
    int first_mb;
    int type;                    /* 0 = P, 2 = I */
    int pps_id;
    int frame_num;
    int idr_pic_id;
    int poc_lsb;
    int num_ref_idx_active;
    int qp;
    int dbk_idc;
    int alpha_off, beta_off;     /* already * 2 */
    int adaptive_marking;
    int nmmco;
    int long_term_reference;
    struct h264_mmco mmco[H264_MAX_MMCO];
};

/* ---- Macroblock working state --------------------------------------
 * Neighbour caches use an 8-wide grid of 4x4 blocks with a border:
 * index C(x, y) = (y + 1) * 8 + x + 4 for x in -1..4, y in -1..3.
 * Row -1 is the macroblock above (x = 4: above-right, x = -1: above-left),
 * column -1 the macroblock to the left.  x = 0 sits on a word boundary
 * (the arrays are word aligned), so a row of four is one word copy; a
 * row's x = 4 shares its word with the next row's unused 0..2 and its
 * x = -1 (byte 3). */
#define C(x, y) (((y) + 1) * 8 + (x) + 4)
#define C_SIZE  44               /* C(4, 3) + 1, rounded up to a word */
/* a row of four cache bytes, or four bytes of struct h264_mbinfo */
typedef uint32_t __attribute__((may_alias)) h264_w4;
/* one MV (int16_t[2]) as a word: MV arrays are 4-byte aligned */
typedef uint32_t __attribute__((may_alias)) h264_mvw;
#define NZ_NA   (-1)             /* nz cache: block not available */
#define REF_NA  (-2)             /* ref cache: not available */

struct h264_mb {
    int x, y, addr;
    int flags;
    int mb_type;                 /* slice-type independent: see mb.c */
    int cbp;
    int qp, qpc;
    int chroma_mode;
    int avail;                   /* neighbour macroblocks: MBA_* */
    int iavail;                  /* the same, for intra prediction */
    int8_t nz[C_SIZE] __attribute__((aligned(4)));    /* luma TotalCoeff */
    int8_t nzc[2][C_SIZE] __attribute__((aligned(4)));
                                 /* chroma TotalCoeff (x, y in -1..1) */
    int8_t ipm[C_SIZE] __attribute__((aligned(4)));
    int8_t ref[C_SIZE] __attribute__((aligned(4)));
    int16_t mv[C_SIZE][2] __attribute__((aligned(4)));   /* word copies;
                                    the current macroblock's are stale
                                    until written (see load_neighbours) */
    int16_t coef[24][16] __attribute__((aligned(32)));
                                 /* dequantised, raster within the block;
                                    0-15 luma (raster), 16-19 Cb, 20-23 Cr */
    int16_t dc[16];
    uint32_t coded;              /* blocks with coefficients: bit k for
                                    coef[k] (luma raster, then chroma) */
    uint32_t dconly;             /* of those, only coef[k][0] is set */
    int32_t dq[52][16];          /* dequantisation by scan position for
                                    each QP, built at its first use
                                    (cavlc.c, dq_for): with adaptive
                                    quantisation the QP changes at almost
                                    every macroblock; 3.3 KB of arena */
    uint32_t dq_built[2];        /* the QPs whose table is built */
};

#define MBA_A 1                  /* left */
#define MBA_B 2                  /* above */
#define MBA_C 4                  /* above-right */
#define MBA_D 8                  /* above-left */

/* ---- The decoder ---------------------------------------------------------- */
/* Macroblock columns a frame's rows hold: mb_w rounded up to even, so the
 * luma stride (16 per column) is a multiple of 32 and the chroma's a
 * multiple of 16 */
#define H264_PAD_MBW(mb_w) (((mb_w) + 1) & ~1)

struct h264_dec {
    uint8_t *arena;
    size_t arena_size;
    uint8_t *fbuf;               /* frame buffers apart from the arena */
    size_t fbuf_size;            /* (h264_set_frames), or NULL / 0 */
    int arena_first;             /* frames in the arena when they fit
                                    there, fbuf only when not */
    struct h264_sps sps[H264_MAX_SPS];
    struct h264_pps pps[H264_MAX_PPS];
    const struct h264_sps *active_sps;
    int layout_mb_w, layout_mb_h, layout_refs;   /* buffers laid out for */

    struct h264_frame frames[H264_MAX_FRAMES];
    int nframes;
    struct h264_mbinfo *mbi;
    int16_t (*mvs)[16][2];
    int mb_w, mb_h, mbs;
    /* the planes' row strides: the width rounded up to 32 pixels (luma)
     * and 16 (chroma), so every vector access keeps one 16-byte line
     * phase from row to row, as Apple's image strides do [apple-RE]
     * (memory-access.md section 11; H264_PAD_MBW) */
    int ls, cs;

    /* the picture being decoded */
    struct h264_frame *cur;
    struct h264_slice_hdr sh;    /* the current slice's header */
    struct h264_slice_hdr first_sh;   /* the picture's first slice */
    const struct h264_pps *pps_cur;
    int in_picture;
    int mbs_done;
    int slice_num;
    int prev_ref_frame_num;
    int max_frame_num;
    int pending_mmco5;

    struct h264_frame *reflist[H264_MAX_REFLIST + 1];
    int nref;

    /* the NAL handed back with H264_FRAME_AGAIN (already unescaped) */
    const uint8_t *again_nal;
    int again_len, again_rbsp;
    uint32_t rbsp_bits;          /* position of the current NAL's stop bit */

    struct h264_mb mb;
    /* inter.c's edge-emulation window: 21 rows of 32 bytes and the vector
     * kernels' 32-byte overread (here, not on the stack: a 16-aligned
     * stack array costs h264_inter_pred a realigned frame on each call) */
    uint8_t edge[21 * 32 + 32] __attribute__((aligned(16)));
    /* the loop filter's thresholds for the last QPs it saw (deblock.c):
     * internal luma and chroma, macroblock edge luma and chroma */
    struct h264_dbk_par {
        int key;                 /* QP + 1 and the offsets; 0: none */
        int alpha, beta;         /* alpha 0 when either is (no filtering) */
        const int8_t *tc;        /* Table 8-17's row */
        uint32_t tcw[4];         /* tC0 for bS 0..3 in every byte (0, 4
                                    lanes per byte for the kernels) */
    } dbk[4];
    ptrdiff_t uv;                /* the picture's v plane - its u plane */
    int32_t blk_off[24];         /* each 4x4 block's offset in its plane
                                    (luma raster, then Cb, Cr: mb.c) */
    struct h264_picture out;
    struct h264_stats stats;
};

/* h264.c */
int h264_layout(struct h264_dec *d, const struct h264_sps *sps);

/* ps.c */
int h264_parse_sps(struct h264_dec *d, struct h264_bits *b);
int h264_parse_pps(struct h264_dec *d, struct h264_bits *b);

/* slice.c */
int h264_decode_slice(struct h264_dec *d, struct h264_bits *b, int nal_type,
                      int nal_ref_idc);
int h264_finish_picture(struct h264_dec *d);

/* mb.c */
int h264_decode_slice_data(struct h264_dec *d, struct h264_bits *b);

/* cavlc.c: the residual of macroblock m (7.3.5.3), CAVLC, dequantised
 * into m->coef with m->coded and m->dconly; m->nz and m->nzc get the
 * blocks' TotalCoeff.  i16: an Intra_16x16 macroblock.  Returns 0, or -1
 * on a bitstream error. */
int h264_residual(struct h264_bits *b, struct h264_mb *m, int i16);

/* mvpred.c */
#define MVP_MEDIAN 0
#define MVP_16x8   1             /* directional rules of 8.4.1.3 */
#define MVP_8x16   2
void h264_mv_pred(const struct h264_mb *m, int x, int y, int w, int ref,
                  int shape, int16_t *mvp);
void h264_mv_pred_skip(const struct h264_mb *m, int16_t *mvp);

/* inter.c: motion compensation of one partition (x, y, w, h in 4x4 units) */
void h264_inter_pred(struct h264_dec *d, const struct h264_frame *ref,
                     int bx, int by, int bw, int bh, const int16_t *mv);

/* deblock.c */
void h264_deblock_picture(struct h264_dec *d);

/* tables (tables.c) */
extern const uint8_t h264_zigzag4x4[16];
extern const uint8_t h264_blk_raster[16];      /* blkIdx -> raster */
extern const uint8_t h264_qp_div6[52 + 18];
extern const uint8_t h264_qp_mod6[52 + 18];
extern const uint8_t h264_dequant4[6][16];     /* v(m) per raster pos */
extern const uint8_t h264_chroma_qp[52];
extern const uint8_t h264_cbp_intra[48], h264_cbp_inter[48];

#endif /* H264_INT_H */
