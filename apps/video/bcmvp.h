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

/* The PP side of our video host protocol ("VP", #64 piece 1;
   docs/research/bcm2722/hostproto.md): the header, both rings with their
   encoded indices, the bitstream FIFO's bookkeeping, replies and events,
   and the liveness check.  The numbers come from bcmfw/hostproto.h, which
   holds only constants and is shared with the image on purpose.

   Pure logic over a port (struct bvp_port): no Rockbox headers, so the
   same code runs on the PP (the device test's bounded host-port calls),
   in the simulator (a model BCM) and in the host tests, against the model
   (tests/video/test_vpcore.c) and against our real image in vc2sim
   (bcmfw/tests/vplayer_test.py).

   No call waits without a deadline.  The FIFO and the command ring
   answer "no room" at once (BVP_AGAIN) and the caller retries on its next
   tick; replies are collected by bvp_poll(), and only bvp_call() waits,
   for one reply, up to its timeout. */

#ifndef BCMVP_H
#define BCMVP_H

#include <stdbool.h>
#include <stdint.h>

/* Every port call returns in bounded time: < 0 when the host port gave
   up.  wr_block writes n halfwords (an even count: the port moves 32-bit
   words) from a word-aligned src to consecutive addresses from a word-
   aligned addr. */
struct bvp_port
{
    int (*rd)(uint32_t addr, uint32_t *val);
    int (*wr)(uint32_t addr, uint32_t val);
    int (*wr_block)(uint32_t addr, const uint16_t *src, uint32_t n);
    uint32_t (*now_us)(void);
    void (*yield)(void);
};

/* Return codes besides the protocol's HP_OK / HP_E_* (0-11) */
#define BVP_AGAIN       0x100   /* FIFO, queue or ring full: retry later */
#define BVP_E_PORT      (-1)    /* the host port gave up */
#define BVP_E_TIMEOUT   (-2)    /* no reply in time */
#define BVP_E_PROTO     (-3)    /* no protocol, another major, a bad header */
#define BVP_E_STATE     (-4)    /* no session, or packets out of order */
#define BVP_E_STILL     (-5)    /* vpcore: the picture never moved through
                                   VPC_STILL_MAX restarts (dev33) */
#define BVP_E_CHECK     (-6)    /* vplayer: the unit's first-use check
                                   (vcheck.h) passed at no band */

#define BVP_MAX_PKTS    64      /* packets in the FIFO (pushed, not consumed) */
#define BVP_REPLIES     8       /* replies kept until collected */
#define BVP_REPLY_WORDS 31      /* a reply's whole payload (HP_MAX_PAYLOAD
                                   less the status): TV's 29 words, the
                                   census's 19 (16 cut both short) */
#define BVP_DEAD_US     200000  /* heartbeat unchanged this long: dead */

struct bvp_reply
{
    uint16_t tag;               /* 0: free */
    uint8_t op;
    uint8_t n;                  /* payload words kept */
    int32_t status;
    uint32_t p[BVP_REPLY_WORDS];
};

struct bvp_pkt
{
    uint32_t off;               /* in the FIFO, bytes, 64-aligned */
    uint32_t len;               /* bytes */
    bool posted;                /* its FEED went out */
};

/* How packets go into the FIFO (stage 6, dev12; session 11 lost the
   port while the BCM decoded and the PP wrote): pace > 0 splits every
   block into pieces of that many halfwords, each its own address and
   handshake (Apple's osos never streams more than 256 bytes without
   one); verify reads each piece's first and last word back */
struct bvp_wire
{
    uint32_t pace;              /* halfwords a piece, 0: whole blocks */
    bool verify;
    uint32_t words, bad;        /* read back, wrong */
    uint32_t first[3];          /* the first wrong one: address, got, want */
};

struct bvp
{
    const struct bvp_port *port;
    struct bvp_wire *wire;      /* NULL: whole blocks, no readback; kept
                                   by bvp_attach() */
    uint32_t version, caps;
    uint32_t cmd_base, cmd_size, evt_base, evt_size;
    uint32_t fifo_host, fifo_size;      /* from the header */
    uint32_t cmd_wr, evt_rd;    /* the indices we own, 16-bit */
    uint16_t tag;

    /* the session (OPEN's reply) */
    uint32_t session, depth, sess_fifo, sess_fifo_size;
    /* OPEN's SDRAM words (minor 2; hostproto.md, "OPEN"): on HP_OK the
       set-up word and (minor 3) the memory test's in sd[4], on
       HP_E_NOMEM all five; sd_n = how many came */
    uint32_t sd[5], sd_n;

    /* FIFO: packets in order, [head, head + n) of pkt[], oldest first */
    struct bvp_pkt pkt[BVP_MAX_PKTS];
    uint32_t head, n;
    uint32_t wpos;              /* bytes: where the next packet starts */
    uint32_t fill;              /* bytes of the packet being written */
    uint32_t rd_seen;           /* FIFO_RD as last read, 64-byte units */
    uint32_t done_units;        /* consumed, not yet matched to packets */
    uint32_t in_flight;         /* posted and not consumed */

    struct bvp_reply replies[BVP_REPLIES];

    /* events (tag 0) */
    bool eos, lost;
    int32_t last_error;         /* EV_ERROR's status, 0 if none */
    bool evt_rd_stale;          /* HP_EVT_RD's last write failed */
    uint32_t idx_raw;           /* the last index word read, as read */
    uint32_t evt_torn, evt_raw; /* polls in a row that found HP_EVT_WR
                                   torn, and the word (dev14) */
    uint32_t errors, events;

    /* liveness */
    uint32_t beat, beat_at_us, beat_same;
    bool beat_valid;

    uint32_t xfers;             /* 16-bit host-port transfers, measured */
    uint32_t reads;             /* of them, single-word reads (slow) */
};

/* Reads the header (the image wrote HP_VERSION last); 0, or BVP_E_PROTO
   when the image offers no protocol or another major, or BVP_E_PORT. */
int bvp_attach(struct bvp *v, const struct bvp_port *port);

/* Queues one command: 0 and *tag set, BVP_AGAIN when the ring has no
   room, or BVP_E_PORT.  Never waits. */
int bvp_send(struct bvp *v, uint32_t op, const uint32_t *args, uint32_t n,
             int *tag);

/* Drains the event ring: replies are kept for bvp_take(), events set the
   flags above.  Returns the records read, or BVP_E_PORT. */
int bvp_poll(struct bvp *v);

/* The reply to tag, once bvp_poll() has seen it: its status (HP_OK or
   HP_E_*, >= 0) with up to cap payload words in out (*n set), and the
   slot freed.  BVP_AGAIN while it hasn't come. */
int bvp_take(struct bvp *v, int tag, uint32_t *out, uint32_t cap,
             uint32_t *n);

/* bvp_send, then bvp_poll and port->yield until the reply or timeout_us.
   Returns the reply's status or a BVP_E_* code. */
int bvp_call(struct bvp *v, uint32_t op, const uint32_t *args, uint32_t n,
             uint32_t *out, uint32_t cap, uint32_t *got, uint32_t timeout_us);

/* HELLO: our major/minor; HP_OK, HP_E_VERSION or BVP_E_*. */
int bvp_hello(struct bvp *v);

/* OPEN for H.264: the stream's size, a HP_MODE_* and HP_OPEN_* flags,
   on the full panel.  HP_OK and the session set up, or the error. */
int bvp_open(struct bvp *v, uint32_t w, uint32_t h, uint32_t mode_flags);

/* CLOSE: HP_OK, or the error; the session ends on our side either way. */
/* OVERLAY: the host frame the image holds at BCMFW_FB goes over the
   picture in that panel rectangle (w or h 0: off).  HP_OK, HP_E_UNKNOWN
   for an image without HP_CAP_OVERLAY, or another error. */
int bvp_overlay(struct bvp *v, uint32_t x, uint32_t y, uint32_t w,
                uint32_t h);
/* the same with HP_OP_OVERLAY's slide word (HP_OVL_SLIDE_* | ms); to an
   image without HP_CAP_OVERLAY_SLIDE a slide down is off at once and a
   slide up on at once */
int bvp_overlay_slide(struct bvp *v, uint32_t x, uint32_t y, uint32_t w,
                      uint32_t h, uint32_t slide);
int bvp_close(struct bvp *v, uint32_t flags);

/* Starts a packet of len bytes in the FIFO: 0, or BVP_AGAIN when the FIFO
   or the packet queue is full.  Then bvp_pkt_write() the bytes in order
   from word-aligned buffers (every piece but the last a multiple of 4
   bytes; the last is padded to one, inside the packet's 64-byte slot)
   and bvp_pkt_end(). */
int bvp_pkt_begin(struct bvp *v, uint32_t len);
int bvp_pkt_write(struct bvp *v, const void *buf, uint32_t len);
void bvp_pkt_end(struct bvp *v);
/* Drops the packet being written (begun, not ended): a host-port failure
   part way through it; the next one starts where it did. */
void bvp_pkt_abort(struct bvp *v);

/* FEED for the oldest packet not yet posted, with its PTS (us) and
   HP_FEED_* flags: 0, BVP_AGAIN (ring full), BVP_E_STATE (none pushed) or
   BVP_E_PORT. */
int bvp_feed(struct bvp *v, int64_t pts_us, uint32_t flags);

/* Packets pushed and not yet posted. */
uint32_t bvp_unposted(const struct bvp *v);

/* Reads FIFO_RD and retires the packets the BCM has finished with. */
int bvp_refresh(struct bvp *v);

/* false when the heartbeat hasn't moved for BVP_DEAD_US (over three
   looks or more) or the image reports a fault on the LCD command word.
   Look often: the beat is 8 bits of a 1.024 ms count, so it comes round
   every 262 ms, and looks that far apart can see the same value on a
   live image; on every tick (10-100 ms) two equal looks in a row can't
   be aliased. */
bool bvp_alive(struct bvp *v);

#endif /* BCMVP_H */
