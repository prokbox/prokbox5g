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

/* A model of our video image's host side, for the simulator and the host
   tests only (never in a target build): the same memory layout, header,
   rings, encoded indices and stage A semantics as bcmfw/hostproto.c with
   BCMFW_VIDEO=1 (FEEDs checked against the FIFO order and decoded in
   order; FIFO_RD moves when a packet is done; STATUS words 6-12; EV_EOS),
   with a decode time per packet instead of a decoder.  Each picture is
   handed to a present callback with its packet's bytes: the simulator
   decodes them with bcmfw/h264 and shows them, the tests check them.

   The real image in vc2sim is the other end the client is tested
   against (bcmfw/tests/vplayer_test.py); this model is the fast one, with
   faults on demand. */

#ifndef VBCM_MODEL_H
#define VBCM_MODEL_H

#include <stdbool.h>
#include <stdint.h>

#define VBM_SRAM_SIZE   0x140000
#define VBM_SDRAM_HOST  0xC0000000u
#define VBM_SDRAM_SIZE  0x400000
#define VBM_QUEUE       16      /* VIDEO_QUEUE_DEPTH */

struct vbm_pkt
{
    uint32_t off, len, flags;
    int64_t pts_us;
    uint32_t done_at;           /* its decode ends */
    bool stale;                 /* its bytes not yet behind the host's
                                   writes when the image took it */
};

/* The host's SDRAM writes reach the image's reads only behind a host
   read of SDRAM (the port's readback, bvp_wire's verify) or this long
   after the last write.  Whether, and when, the port's SDRAM writes are
   seen by the core before the next SRAM command is not proven on
   silicon; sitting 4b's check, which sent each FEED the moment its
   packet was written, got no picture at every band (the image answered,
   took the packet, decoded nothing).  A FEED of bytes still in flight is
   taken as silicon took it: consumed, no picture, no error.  The player
   pushes ahead of its posts (vplay.c VP_AHEAD_US) by far more than
   this, in time and in bytes: a write lands this long after it, or
   once this many bytes more went to SDRAM behind it (a posted write
   queue's depth), or at once behind a host read of SDRAM. */
#ifndef VBM_LAND_US
#define VBM_LAND_US     2000
#endif
#define VBM_LAND_BYTES  4096

/* One 32-bit read through the host port (an address set-up and two
   16-bit reads), in ns, as silicon charged it.  Calibration [silicon]
   sitting 4c (2026-10-26, merged image 23693ff4): the first-use check
   took 7.99 s at 150 MHz, about 920,000 word reads (the planes'
   readback) in about 7.7 s of it, less 0.2 s of decode and 66 ms of
   OPEN, overlay and readback: 8.9 us a word (data-paths.md).  The old [estimate]
   was 0.5-1 us (data-paths.md R2), and the simulator charged nothing:
   it measured the same check at 887 ms.  The model itself keeps no
   clock (now_us is the caller's), so the caller charges it: the
   simulator's bcm_video_rd() (SIM_PORT_RD_NS overrides; 0 is more
   permissive than silicon, for A/B only) and test_vcheck's port.  A
   read under a decode's load can wait far longer (35-85 us, R2); this
   is the idle chip's cost, the floor. */
#ifndef VBM_RD_NS
#define VBM_RD_NS       8900
#endif
#define VBM_PENDS       32

struct vbm_pend
{
    uint32_t lo, hi;            /* SDRAM offsets */
    uint32_t at, mark;          /* the run's last write: when, and the
                                   SDRAM bytes written by then */
};

struct vbm
{
    uint8_t *sram, *sdram;      /* the caller's memory */
    uint32_t (*now_us)(void);
    /* a picture went up; pkt is the packet's bytes, unwrapped */
    void (*present)(void *ctx, const uint8_t *pkt, uint32_t len,
                    int64_t pts_us, uint32_t flags);
    void *ctx;
    uint8_t *scratch;           /* VBM_SCRATCH bytes, for present */

    uint32_t decode_us;         /* per packet */
    uint32_t panel_us;          /* decode done to the picture up */

    /* faults */
    bool hang;                  /* stops answering and beating */
    bool no_protocol;           /* the header stays zero */
    uint32_t major;             /* 0: ours */
    uint32_t die_after;         /* hang after this many packets, 0 never */
    bool hang_fast;             /* knob: hang at the first FEED of a
                                   150 MHz session (session 10) */
    bool hang_port;             /* knob: a hang takes the host port with
                                   it (a bus lock-up): reads fail */
    bool diag;                  /* the session's HP_OPEN_SDRAM_DPB */
    uint32_t stall_after;       /* knob: at this FEED the host port fails
                                   the next stall_n accesses, reads and
                                   writes, the image running on (session
                                   11), 0 never */
    uint32_t stall_n, stall_left;
    bool stall_writes;          /* knob: the stall fails writes only */
    uint32_t tear_after;        /* knob: from this FEED on, HP_EVT_WR
                                   reads as junk until the next boot (the
                                   header overwritten, session 12) */
    bool torn;
    bool refuse;                /* knob: the stream is unsupported: the
                                   first FEED raises EV_ERROR and the
                                   session stops decoding (HP_ST_ERROR) */
    bool lag_one;               /* knob: as the image (video.c), a
                                   picture is up when the next packet's
                                   first slice ends it, or at EOS */
    uint32_t stuck_boots;       /* knob: sessions in the first this many
                                   boots decode nothing, restarts or not
                                   (session 30's stall) */
    uint32_t stuck_opens;       /* knob: the first this many sessions
                                   take FEEDs and decode nothing (session
                                   28's still start, whatever its cause) */
    /* OVERLAY: the rectangle shown (wh 0: none), how many came, and a
       callback with the host frame (SRAM 0xE0000, 320 pixels a row) */
    uint32_t ovl_xy, ovl_wh, overlays, slides;
    uint32_t au_in, au_out, au_size, audio_blocks;  /* HP_OP_AUDIO */
    void (*overlay)(void *ctx, const uint8_t *frame, uint32_t xy,
                    uint32_t wh);
    bool held;                  /* lag_one: a picture waits */
    int64_t held_pts;

    /* state, as hostproto.c keeps it */
    bool powered, running;
    uint32_t cmd_rd, evt_wr, state, session, own_display;
    uint32_t fifo_next, fifo_rd;
    uint32_t packets, bytes, errors, events_lost;
    uint32_t decoded, shown, hidden;
    int64_t shown_pts;
    struct vbm_pkt q[VBM_QUEUE];
    uint32_t q_rd, q_wr;
    uint32_t last_done;         /* the last queued decode ends */

    /* what the tests look at */
    uint32_t boots, cuts, opens, closes, feeds, fifo_errors, statuses;
    uint32_t fast_opens;        /* OPENs with HP_OPEN_FAST_CLOCK */
    bool nomem_fast;            /* knob: OPEN with HP_OPEN_FAST_CLOCK
                                   answers HP_E_NOMEM (SDRAM on PLL A) */
    bool fast;                  /* the session's clock (STATUS word 15) */
    uint32_t port_misuse;       /* blocks the PP's port can't write */
    uint32_t total_packets;
    /* P19 PW4: HP_OP_VCLK (knob vclk: the image has it).  A packet's
       decode takes decode_us x 150 / the band's MHz; busy sums them */
    bool vclk;
    uint32_t band, mhz, vclk_seq, vclk_sets, vclk_busy, vclk_bad;
    /* knob (the first-use check, vcheck.h): a switch to band b (bit b;
       OPEN's HP_OPEN_FAST_CLOCK is band 11) fails its speed check: the
       crystal, counted in the clock record's fails word, as clock.c */
    uint32_t vclk_fail_mask, vclk_fails;
    uint32_t pic_bad;           /* knob: from this picture of a session on
                                   (1-based; 0 never), each picture
                                   vbm_picture() lays out has the first
                                   luma byte of every row flipped (a unit
                                   that decodes wrong: a bad column, in
                                   whichever rows the check samples) */
    uint32_t st_errors, st_concealed;   /* knob: STATUS words 8 and 9 (the
                                   image's decode errors and concealed
                                   blocks) */
    uint32_t pic_addr_skew;     /* knob: VIDEO_MB_PROG_A this far off the
                                   picture (the image's words not where
                                   the check reads them) */
    /* VBM_LAND_US: the host's SDRAM writes not yet seen by the image,
       oldest first (runs of consecutive writes); the knobs land_us and
       land_bytes (vbm_init: VBM_LAND_US, VBM_LAND_BYTES; land_us 0: at
       once, ~0 with land_bytes ~0: only behind a host read) */
    uint32_t land_us, land_bytes, sd_bytes;
    struct vbm_pend pend[VBM_PENDS];
    uint32_t pend_first, pend_n;
    uint32_t stale_feeds;       /* packets taken before their bytes were */
    bool no_pic;                /* knob: every packet taken that way,
                                   whatever the host did (an image or a
                                   path that decodes nothing) */
};

#define VBM_SCRATCH     0x100000    /* one packet, unwrapped */

void vbm_init(struct vbm *m, uint8_t *sram, uint8_t *sdram, uint8_t *scratch,
              uint32_t (*now_us)(void));

/* Power on and start the image: the header is written, version last. */
void vbm_boot(struct vbm *m);
void vbm_cut(struct vbm *m);

/* The host port (bvp_port's calls); each one lets the model run up to
   now first.  0, or -1 for an address outside the model. */
int vbm_rd(struct vbm *m, uint32_t addr, uint32_t *val);
int vbm_wr(struct vbm *m, uint32_t addr, uint32_t val);
int vbm_wr_block(struct vbm *m, uint32_t addr, const uint16_t *src,
                 uint32_t n);

/* Lets the model run up to now (decodes finish, pictures go up). */
void vbm_run(struct vbm *m);

/* The decoded picture as the image leaves it, for a present callback that
   decodes (the simulator, the host tests): its planes into SRAM where
   video.c's frames are (VBM_FRAMES, three in turn: Y, then U at
   Y + stride_y x h, V at Y + stride_y x h x 5/4, as h264.c lays them out),
   VIDEO_MB_PROG_A at its luma and VIDEO_MB_PROG idle, and the guard's
   first-picture words (Y, U, V, the stride) at a session's first.
   Pictures too large for SRAM are left out (the image puts those
   frames in SDRAM).  The first-use check (vcheck.h) reads them back. */
#define VBM_FRAMES      0x40000
void vbm_picture(struct vbm *m, const uint8_t *y, const uint8_t *u,
                 const uint8_t *v, uint32_t stride_y, uint32_t stride_c,
                 uint32_t w, uint32_t h);

#endif /* VBCM_MODEL_H */
