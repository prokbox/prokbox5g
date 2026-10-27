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

/* The first-use video check (docs/research/bcm2722/first-use-check.md):
 * the per-unit safety net under the image's clock150 token.
 *
 * The image's token (vclock_gate.h) says the image held 150 MHz on a real
 * unit; it says nothing about this unit.  So the first video play on a
 * unit (no unit token, or one naming another image) runs, inside its own
 * start-up and on the merged image's release path only:
 *   1. the session at a band of the ladder (vclock_ladder.h): OPEN with
 *      HP_OPEN_FAST_CLOCK for the top band, else HP_OP_VCLK to the band;
 *      the chip's own record (HP_VCLK_STAT) must show the band reached
 *      and no fallback (clock_band checks the core's speed against the
 *      crystal on every switch);
 *   2. the SDRAM readback stage 6 uses (a pattern through the host port
 *      into the FIFO, read back word by word): 0 bad words;
 *   3. the built-in test stream (vcheck_stream.h, about 30 pictures at
 *      320x240), one picture a FEED in lockstep: each decoded picture's
 *      planes read back over the host port (where the image's own words
 *      say they are: the guard's first-picture planes, VIDEO_MB_PROG_A at
 *      each conversion) and summed on the PP, a sample of rows that
 *      covers every macroblock (vcheck_sample_row), every sum exactly as
 *      the table; no decode error, no concealed block, every picture at the
 *      band's clock and within the frame budget.
 * No image change: everything read is a word the merged image already
 * writes.  The picture is converted but never reaches the panel: a
 * full-panel overlay of the shell's own frame ("Preparing video...")
 * covers it (video.c picture_blit leaves an overlay's rows alone).
 *
 * The first band that passes, highest first, goes into the unit's token
 * (a small text file in the hidden folder: the image's FNV-1a, the band,
 * the build and the date).  None: the play fails with the styled error
 * screen.  A check that can't run as designed (the stream file wrong,
 * the image's words not where they should be) is inconclusive: logged
 * loudly, no token, the play goes on at the image's clock, and the next
 * play checks again.
 *
 * Pure logic over the VP client (bcmvp.h): no Rockbox calls, so
 * tests/video/test_vcheck.c runs it against the BCM model with our own
 * decoder behind it. */
#ifndef VCHECK_H
#define VCHECK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "bcmvp.h"

/* ---- the unit's token ---------------------------------------------------
 * Text, one "key value" a line after the first:
 *   vcheck 1
 *   image 2c6dc03a
 *   band 11
 *   build <version>
 *   date 2026-10-26
 * Anything else (a cut write, another version, a band out of range) is
 * corrupt and reads as no token: the next play checks again. */
#define VCHECK_TOKEN_MAX    192         /* the file's bytes, at most */

struct vcheck_token
{
    uint32_t image_fnv;
    int band;                           /* 1 .. 11 (vclk.h) */
    char build[48];
    char date[16];
};

/* 0, or -1 when text isn't a token */
int vcheck_token_parse(const char *text, size_t n, struct vcheck_token *t);
/* the file's text into buf: its length, or -1 when it doesn't fit */
int vcheck_token_format(const struct vcheck_token *t, char *buf, size_t n);
/* The band this unit may run with this image: the token's, or -1 when
 * the check is due (no token, or one for another image).  t may be
 * NULL (no file, or a corrupt one). */
int vcheck_unit_band(const struct vcheck_token *t, uint32_t image_fnv);

/* ---- the ladder ---------------------------------------------------------
 * The next band to try after band failed: the ladder's next row below
 * it, or -1 when none is left.  band < 0: the ladder's first row. */
int vcheck_ladder_next(const uint8_t *ladder, int n, int band);

/* ---- live invalidation --------------------------------------------------
 * During any later play these drop the unit's token at once (and step
 * the play down a band where it can): the next play checks again. */
enum vcheck_bad
{
    VCHECK_BAD_NONE = 0,
    VCHECK_BAD_ERROR,                   /* the image counted a decode error */
    VCHECK_BAD_CONCEAL,                 /* ... or concealed a block */
    VCHECK_BAD_CLOCK,                   /* the chip fell back to the crystal */
    VCHECK_BAD_FAULT,                   /* the image was lost (FAULT, dead) */
    VCHECK_BAD_JOBS,                    /* the job driver saw the chip fail */
    VCHECK_BADS
};
extern const char *const vcheck_bad_name[VCHECK_BADS];

/* ---- the test stream ----------------------------------------------------
 * bcmfw/video/mkvcheck.py's file: "VCK1", width, height (16 bits each),
 * pictures, frame us, a length a picture, then the packets, each padded
 * to 4 bytes (so each starts word-aligned when the file does). */
#define VCHECK_MAGIC        0x314B4356u /* "VCK1" */
#define VCHECK_MAX_PICS     64

struct vcheck_stream
{
    const uint8_t *base;
    uint32_t bytes;
    uint32_t w, h, pics, frame_us;
};

/* 0, or -1 when buf isn't a whole, sane stream file */
int vcheck_stream_parse(struct vcheck_stream *s, const uint8_t *buf,
                        uint32_t bytes);
/* picture k's packet and its length */
const uint8_t *vcheck_stream_pkt(const struct vcheck_stream *s, uint32_t k,
                                 uint32_t *len);
/* FNV-1a over 32-bit words as the port reads them (the planes'
 * little-endian words), chained from h (0x811C9DC5 to start) */
static inline uint32_t vcheck_sum_word(uint32_t h, uint32_t w)
{
    return (h ^ w) * 0x01000193u;
}
#define VCHECK_SUM_START    0x811C9DC5u
uint32_t vcheck_fnv_bytes(const uint8_t *p, uint32_t n);

/* The rows a picture's sum covers (bcmfw/video/mkvcheck.py's
 * sample_rows() makes the table from the same rows).  Sitting 4c: the
 * whole planes, about 864k single-word port reads, took 7993 ms of which
 * the decode was about 0.2 s (8.9 us a read).  So each picture's sum
 * takes, in every macroblock row, one whole luma row and one whole row of
 * each chroma plane: every macroblock of every picture is in its sum
 * (its intra prediction, its motion compensation, the loop filter across
 * its vertical edges), and the row inside the macroblock moves with the
 * macroblock row and the picture (U and V half a block apart), so over
 * the stream every row offset is read, those next to the horizontal
 * edges the filter touches among them.  2400 words a picture at 320x240,
 * 72k for the stream: about 0.65 s of reads.  plane 0 is Y, 1 U, 2 V;
 * the sum runs Y's rows, then U's, then V's, top to bottom, each row's
 * words left to right. */
#define VCHECK_MB_ROWS(h)   ((h) / 16)
static inline uint32_t vcheck_sample_row(uint32_t plane, uint32_t mbrow,
                                         uint32_t pic)
{
    return plane ? mbrow * 8 + ((3 * mbrow + 5 * pic + 4 * (plane - 1)) & 7)
                 : mbrow * 16 + ((7 * mbrow + 5 * pic) & 15);
}

/* ---- the check ---------------------------------------------------------- */

/* the image's words it reads (bcmfw/video/video.h) */
#define VCHECK_MB_PROG      0x1C4       /* VIDEO_MB_PROG: phase << 24 | n */
#define VCHECK_MB_PROG_A    0x1CC       /* VIDEO_MB_PROG_A: at CONVERT, Y */
#define VCHECK_PH_IDLE      0x0F        /* VIDEO_PH_IDLE: a packet done */
#define VCHECK_GUARD        0xDF000     /* VIDEO_GUARD: [8..10] the first
                                           picture's Y, U, V; [11] stride */
#define VCHECK_HOST_FB      0xE0000     /* BCMFW_FB: the overlay's frame */
#define VCHECK_PANEL_W      320
#define VCHECK_PANEL_H      240

/* the budget: any one picture within two frames (a stall), the P
   pictures' mean within 75 % of a frame at 30 fps (vtrace.h's pace rule,
   VTRACE_PACE_PCT); the key picture's parse alone can take longer */
#define VCHECK_PIC_MAX_US   66666
#define VCHECK_MEAN_MAX_US  25000
#define VCHECK_PIC_WAIT_US  400000      /* a picture's decode, polled */
#define VCHECK_CALL_US      200000      /* a command's reply */
/* How the check waits for a picture, as the player does (vpcore.c
 * status()): one STATUS in flight, its reply collected, FIFO_RD read,
 * at most once every VCHECK_POLL_US; never back-to-back calls while the
 * chip decodes (sitting 4b) */
#define VCHECK_POLL_US      1000
/* the SDRAM readback: one 1 KB block every 1/16 of the FIFO, 4 words of
   each read back (vpcore.h VPC_SDC_*, stage 6's "6s") */
#define VCHECK_SDC_BLOCKS   16
#define VCHECK_SDC_READS    4
#define VCHECK_MAX_TRIES    12
/* A check never holds the film's start longer than this, nor reboots the
 * image more than once: past either it stops, inconclusive */
#define VCHECK_BUDGET_US    4000000

enum vcheck_why
{
    VCHECK_OK = 0,
    VCHECK_W_NOVCLK,        /* the image can't move the clock (no VCLK) */
    VCHECK_W_OPEN,          /* OPEN refused (at 150 MHz: SDRAM didn't
                               come up on PLL A) */
    VCHECK_W_SWITCH,        /* the band wasn't reached */
    VCHECK_W_FALLBACK,      /* the chip fell back (its speed check) */
    VCHECK_W_SDRAM,         /* the readback found bad words */
    VCHECK_W_DECODE,        /* a decode error or a concealed block */
    VCHECK_W_SUM,           /* a picture's sum differed */
    VCHECK_W_CLOCK,         /* a picture decoded at another clock */
    VCHECK_W_SLOW,          /* over the frame budget */
    VCHECK_W_CHIP,          /* no answer: port, timeout, a stuck decode */
    VCHECK_W_ADDR,          /* inconclusive: the planes not where the
                               image's words say (not the unit's fault) */
    VCHECK_W_STREAM,        /* inconclusive: the stream refused (FIFO) */
    VCHECK_W_NOPIC,         /* inconclusive: the image took the packet
                               (FIFO_RD past it) and answered, and no
                               picture came of it, with no error: the
                               image's or the path's, not the unit's
                               (sitting 4b's signature) */
    VCHECK_W_FEED,          /* inconclusive: the image answered for the
                               whole wait and never took the FEED */
    VCHECK_WHYS
};
extern const char *const vcheck_why_name[VCHECK_WHYS];

struct vcheck_band
{
    uint8_t band;
    uint8_t why;            /* enum vcheck_why, VCHECK_OK: passed */
    uint8_t pic;            /* the picture at the failure */
    int rc;                 /* the call's code at the failure */
    uint32_t mhz;           /* the clock the chip reported */
    uint32_t pics;          /* pictures whose sum matched */
    uint32_t errors, concealed, fails;
    uint32_t sd_words, sd_bad;
    uint32_t work_mean_us, work_max_us;
    uint32_t got, want;     /* the sum (or word) at the failure */
    uint32_t us;            /* this band's time */
    /* the image's own words when a picture didn't come (part 2): STATUS
       decoded, queued, packets, VIDEO_MB_PROG, the guard's [59] (packets
       decoded | the first packet's NAL types << 16), [56] (SPS/PPS seen
       and parsed), [48] (a refusal or first error), FIFO_RD, the
       STATUS replies since the FEED; diag_n 0: none */
    uint32_t diag[9], diag_n;
};

struct vcheck_io
{
    const struct bvp_port *port;
    struct bvp *bvp;        /* attached and past HELLO, no session open */
    /* the image again after a band left it dead: power cut, boot, attach,
       HELLO; 0 or < 0 */
    int (*reboot)(void *ctx);
    void (*log)(void *ctx, const char *line);
    void *ctx;
    /* the frame shown while the check runs (the shell's "Preparing
       video..." screen: VCHECK_PANEL_W x VCHECK_PANEL_H RGB565, rows
       pitch pixels apart), put over the picture; NULL: none */
    const uint16_t *screen;
    uint32_t pitch;
    uint16_t *scratch;      /* 512 halfwords, 4-aligned: the readback */
    const uint32_t *sums;   /* the stream's table (vcheck_stream.h) */
};

#define VCHECK_PASS         0
#define VCHECK_FAIL         1
#define VCHECK_INCONCLUSIVE 2

struct vcheck_result
{
    int verdict;            /* VCHECK_PASS, _FAIL or _INCONCLUSIVE */
    int band;               /* the band that passed, -1 none */
    uint32_t us;
    int n;                  /* bands tried */
    struct vcheck_band tried[VCHECK_MAX_TRIES];
};

/* Down the ladder from its first row, one band at a time, until one
   passes.  Every session it opens is closed again; the image is left
   running, past HELLO, no session open (rebooted if a band killed it),
   or, on a reboot that failed, dead (the verdict is then FAIL with
   VCHECK_W_CHIP).  One log line a band and one for the verdict. */
int vcheck_run(const struct vcheck_io *io, const struct vcheck_stream *s,
               const uint8_t *ladder, int n, struct vcheck_result *r);
/* A band's two log lines, each short enough for the shell's log (56
   characters): part 0 "vck 150 PASS ok p29 rc0 30/150 MHz 1212ms" (the
   band, the verdict and why, the picture, the call's code, pictures
   matched, the clock, the time), part 1 "vck e0 c0 f0 sd64/0 w11319/11319
   00000000/00000000" (errors, concealed, fallbacks, readback words and
   bad ones, work mean and max us, the value got and wanted); part 2,
   only when b->diag_n, "vck x d0 q0 p1 ph0f000001 g59 00010001" (STATUS
   decoded, queued, packets; VIDEO_MB_PROG; the guard's packets and NAL
   types), part 3 "vck y g56 00000101 g48 00000000 rd0 a2" (the guard's
   parameter sets and first error, FIFO_RD, STATUS replies since the
   FEED).  Returns the length, or 0 for a part not to be logged. */
int vcheck_band_line(const struct vcheck_band *b, int part, char *buf,
                     size_t n);

#endif /* VCHECK_H */
