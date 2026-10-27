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
 * bcmfw video host protocol ("VP"): a header block at 0x100, a command
 * ring (host -> BCM) and an event ring (BCM -> host) in internal SRAM.
 * Our own design: docs/research/bcm2722/hostproto.md.  The PP-side
 * driver includes the same numbers (this header is shared on purpose:
 * no code, only constants, so it builds for ARM as well).
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
#ifndef BCMFW_HOSTPROTO_H
#define BCMFW_HOSTPROTO_H

/* 0: the image does not offer the video protocol (header stays zero).
 * 1: header, rings and the skeleton commands (version, open/close with
 * the null codec, feed, status). */
#ifndef BCMFW_HOSTPROTO
#define BCMFW_HOSTPROTO      0
#endif

/* ---- Version ---------------------------------------------------------- */
#define HP_MAGIC             0x5650      /* 'VP', bits 31:16 of HP_VERSION */
#define HP_MAJOR             1           /* incompatible changes */
#define HP_MINOR             4           /* additions only.  1: H.264
                                            (OPEN reply + queue depth,
                                            STATUS words 6-12, CLOSE 2-4);
                                            2: OPEN's SDRAM words;
                                            3: the memory test word;
                                            4: OVERLAY, FEED_HIDE */
#define HP_VERSION_WORD      (HP_MAGIC << 16 | HP_MAJOR << 8 | HP_MINOR)

/* ---- Header block (SRAM, as the host sees it) -------------------------- */
#define HP_BASE              0x100
#define HP_VERSION           0x100  /* BCM: written last at start-up */
#define HP_CAPS              0x104  /* BCM: HP_CAP_* */
#define HP_CMD_BASE          0x108  /* BCM: command ring address */
#define HP_CMD_SIZE          0x10C  /*      its size in words (2^n) */
#define HP_EVT_BASE          0x110  /* BCM: event ring address */
#define HP_EVT_SIZE          0x114  /*      its size in words (2^n) */
#define HP_FIFO_BASE         0x118  /* BCM: bitstream FIFO (host address) */
#define HP_FIFO_SIZE         0x11C  /*      its size in bytes (2^n) */
#define HP_CMD_WR            0x120  /* host: command ring write index */
#define HP_CMD_RD            0x124  /* BCM:  command ring read index */
#define HP_EVT_WR            0x128  /* BCM:  event ring write index */
#define HP_EVT_RD            0x12C  /* host: event ring read index */
#define HP_STATE             0x130  /* BCM:  heartbeat << 8 | state */
#define HP_FIFO_RD           0x134  /* BCM:  FIFO consumed, 64-byte units */
#define HP_END               0x140

/* Every shared index word carries a 16-bit value v as ~v << 16 | v
 * (the BCM_CMD() idea).  The host port writes 32 bits as two 16-bit
 * halves, so a reader can see half an update: such a word fails the
 * check and is read again later.  Indices count words (rings) or 64-byte
 * units (FIFO) and wrap at 2^16. */
#define HP_ENC(v)            ((~(uint32_t)(v) << 16) | ((v) & 0xFFFF))
#define HP_VALID(w)          ((((w) >> 16) ^ ((w) & 0xFFFF)) == 0xFFFF)

/* States (HP_STATE bits 7:0) */
#define HP_ST_IDLE           1      /* no session */
#define HP_ST_OPEN           2      /* session open, nothing queued */
#define HP_ST_PLAYING        3
#define HP_ST_PAUSED         4
#define HP_ST_SLEEPING       5      /* host may cut power */
#define HP_ST_ERROR          6

/* Capabilities (HP_CAPS) */
#define HP_CAP_NULL          0x0001 /* codec 0: accepts and drops packets */
#define HP_CAP_H264_BP       0x0002
#define HP_CAP_MPEG4_SP      0x0004
#define HP_CAP_SCALE         0x0010
#define HP_CAP_LCD_DMA       0x0020
#define HP_CAP_JOBS_2D       0x0100
#define HP_CAP_OVERLAY       0x0200 /* HP_OP_OVERLAY */
#define HP_CAP_AUDIO         0x0400 /* HP_OP_AUDIO (stage 8b) */
#define HP_CAP_AAC           0x0800 /* HP_OP_AAC (stage 9, audio-spike.md M4) */
#define HP_CAP_OVERLAY_SLIDE 0x1000 /* HP_OP_OVERLAY's slide word */
#define HP_CAP_PARK          0x2000 /* HP_OP_PARK: the warm swap (G9, B13) */
#define HP_CAP_VCLK          0x4000 /* HP_OP_VCLK: the level bands */

/* ---- Records ----------------------------------------------------------- */
/* Word 0: opcode | payload words << 8 | tag << 16, then the payload.
 * Replies carry the command's opcode | HP_REPLY and its tag, then a
 * status word (HP_E_*), then their own payload. */
#define HP_REC(op, n, tag)   ((op) | (n) << 8 | (uint32_t)(tag) << 16)
#define HP_REC_OP(w)         ((w) & 0xFF)
#define HP_REC_LEN(w)        ((w) >> 8 & 0xFF)
#define HP_REC_TAG(w)        ((w) >> 16)
#define HP_REPLY             0x80
#define HP_MAX_PAYLOAD       32     /* words, both directions */

/* Commands */
#define HP_OP_HELLO          0x01   /* host major<<8|minor -> version, caps */
#define HP_OP_OPEN           0x02   /* codec, w|h<<16, mode|flags, dst xy, dst wh */
#define HP_OP_FEED           0x03   /* session, offset, length, pts lo, pts hi, flags */
#define HP_OP_CLOCK          0x04   /* session, time lo, time hi, rate Q16 */
#define HP_OP_PAUSE          0x05   /* session, on */
#define HP_OP_SEEK           0x06   /* session, flags: flush everything queued */
#define HP_OP_CLOSE          0x07   /* session, flags */
#define HP_OP_STATUS         0x08   /* session */
#define HP_OP_STATS          0x09   /* session */
#define HP_OP_SLEEP          0x0A   /* flags */
#define HP_OP_PING           0x0B   /* anything: echoed */
#define HP_OP_PINS           0x0D   /* mask (GPIO 0-31), microseconds (at most
                                       1 s), flags (bit 0: the masked pins
                                       to inputs for the probe, then back;
                                       bits 3:2: a pull for the probe, 1
                                       down, 2 up, taken off after):
                                       samples, elapsed us, then the edges
                                       of the mask's first four pins, the
                                       levels' AND and OR (dev39: stage 8a
                                       listens for the codec's I2S clocks on
                                       the audio pins, never driving them) */
#define HP_OP_AUDIO          0x0E   /* session, AUDIO_CONFIG: input offset,
                                       output offset, ring size (SDRAM);
                                       AUDIO_PROCESS: offset in the ring,
                                       bytes, gain Q16 -> microseconds
                                       (dev39: stage 8b, PCM through the
                                       chip and back) */
#define HP_OP_TONE           0x10   /* HP_TONE_MAGIC, milliseconds (at most
                                       1500), divider, layout (0 s16, 1
                                       s16 byte-swapped, 2/3 32-bit slots
                                       high/low), phase step, and
                                       optionally the start bits (a subset
                                       of Apple's 0x6209): one tone at -40
                                       dBFS through the BCM's own audio
                                       block (0x10004800, DMA ch5) as a
                                       clock slave: only if GPIO 3 shows
                                       the codec's frame clock, GPIO 2/3 to
                                       function 2 (clock inputs), then
                                       GPIO 1 (data) once the block is
                                       ready (dev45) -> the control word
                                       after the rate, bit 23 seen, ms run,
                                       the control word before and while
                                       playing, GPIO_FSEL0 last set, edges
                                       on GPIO 2 and 3 in 3 ms, then (PB-5)
                                       the audio clock words 0x10001C34 and
                                       0x10001C38 before the rate, while
                                       playing and after the close (stage 8a-1;
                                       DRIVES GPIO 1: the host runs it only
                                       after 8a-0b showed the pin floats,
                                       8a-0 saw the clocks, and the owner
                                       said go) */
#define HP_TONE_MAGIC        0x544F4E45  /* "TONE" */
#define HP_OP_PARK           0x11   /* HP_PARK_MAGIC: the warm swap (jobs.md
                                       G9, bench row B13).  Replies OK |
                                       the warm flags, then the core parks
                                       in a stub outside every image
                                       (BCMFW_PARK_STUB) with the panel as
                                       it is; the host uploads the next
                                       image and writes BCMFW_WARM_GO.
                                       HP_E_BUSY with a session open,
                                       HP_E_STATE off the boot clock or in
                                       a standby */
#define HP_PARK_MAGIC        0x5041524B  /* "PARK" */
/* The warm word the host and the stub share (bcmfw/config.h has the
 * image's names, BCMFW_MB_WARM and BCMFW_WARM_*; hostproto.c checks they
 * agree).  Bits 31:8 a state, 7:0 the park's flags. */
#define HP_OP_LAB            0x12   /* video builds, bench rows B6/B7: HP_LAB_MAGIC,
                                       kind (HP_LAB_*), base, log2 bytes,
                                       count -> microseconds, a checksum,
                                       1 on the 150 MHz clock.  Blocks the
                                       loop for its run; the host may write
                                       the port meanwhile (contention) */
#define HP_LAB_MAGIC         0x4C414221  /* "LAB!" */
#define HP_LAB_EMPTY         0      /* the random-address arithmetic only */
#define HP_LAB_RAND16        1      /* + a halfword load there (a texel) */
#define HP_LAB_SEQ32         2      /* word loads in order */
#define HP_LAB_STORE32       3      /* word stores in order */
#define HP_LAB_COPY32        4      /* word copies, first half to second */
#define HP_LAB_KINDS         5
#define HP_LAB_SRAM          0x12B000    /* free SRAM in the video image, */
#define HP_LAB_SRAM_LOG2     15          /*   32 KB, below the AAC tables parked at 0x134000 until the first OPEN */
#define HP_LAB_SDRAM         0xFE100000  /* the DPB's SDRAM (no stream */
#define HP_LAB_SDRAM_LOG2    20          /*   decodes during a lab), 1 MB */
#define HP_LAB_COUNT_MAX     (4u << 20)
/* ---- The brief probes (brief-map-peripherals.md section 11, PB-*;
 * brief-map-power.md section 9, PW*).  The plain video image only
 * (BCMFW_VIDEO without BCMFW_JOBS: the merged image has no room); every
 * other image answers HP_E_UNKNOWN.  None drives
 * a pin or an output but HP_OP_TV (the TV output); HP_OP_PW changes the
 * core's clock and voltage; HP_OP_HIB parks the chip for the host's
 * hibernate pins.  Each needs a one-time code. */
#define HP_OP_PEEK           0x13   /* HP_PEEK_MAGIC, gap us (at most
                                       HP_PEEK_GAP_MAX), then 1 to
                                       HP_PEEK_MAX word addresses -> two
                                       reads of each, the second gap us
                                       after the first.  Reads only;
                                       HP_E_INVAL, nothing read, if any
                                       address is outside the compiled
                                       allowlist (hostproto.c peek_allow) */
#define HP_PEEK_MAGIC        0x5045454B  /* "PEEK" */
#define HP_PEEK_MAX          15
#define HP_PEEK_GAP_MAX      1000
#define HP_OP_TV             0x14   /* PB-7 to PB-9 (brief-map-peripherals.md
                                       sections 2.3 and 11): off by default.
                                       HP_TV_MAGIC, HP_TV_UNLOCK -> a one-time
                                       code.  Then HP_TV_MAGIC, step, standard
                                       (HP_TV_NTSC/PAL), the code, ms (1 to
                                       HP_TV_TIMING_MS / HP_TV_OUTPUT_MS);
                                       OUTPUT adds format (HP_TV_UYVY ...),
                                       width (HP_TV_W640/W704: PB-9's pair)
                                       and pattern (HP_TV_PAT_*).  Every step
                                       request uses the code up, right or
                                       wrong: none armed HP_E_UNSUPPORTED, a
                                       wrong one or a malformed request
                                       HP_E_INVAL, a session open HP_E_BUSY,
                                       the 1 MHz counter stopped HP_E_STATE;
                                       nothing written.  A run replies
                                       HP_TV_REPLY_WORDS (HP_TV_R_*), HP_OK
                                       or, if it ended early, HP_E_STATE;
                                       either way the off order ran and every
                                       register written was put back */
#define HP_TV_MAGIC          0x54564F4E  /* "TVON" */
#define HP_TV_TIMING         0      /* PB-7: the timing generator, no output */
#define HP_TV_OUTPUT         1      /* PB-8/9: composite out, one format */
#define HP_TV_UNLOCK         2      /* the code for the next step request */
#define HP_TV_STEPS          3
#define HP_TV_NTSC           0
#define HP_TV_PAL            1
#define HP_TV_UYVY           0      /* PB-8 a-d, the FIFO format arms */
#define HP_TV_YUYV           1
#define HP_TV_RGB565         2
#define HP_TV_Y32            3      /* a word a pixel, Y in bits 31:24 */
#define HP_TV_ARMS           4
#define HP_TV_W640           0      /* PB-9: input 640, output 704 */
#define HP_TV_W704           1      /*       input 704, output 704 */
#define HP_TV_PAT_BARS       0      /* 8 vertical bars (PB-8) */
#define HP_TV_PAT_LINES      1      /* a 1-pixel line every 32 (PB-9) */
#define HP_TV_TIMING_MS      1000
#define HP_TV_OUTPUT_MS      8000
#define HP_TV_WAIT_US        10000  /* each of the start waits (PB-8 step 5) */
#define HP_TV_BUF            0x12B000    /* the line buffer (SRAM, ours) */
#define HP_TV_BUF_MAX        2816   /* 704 pixels, 4 bytes each */
#define HP_TV_CB             0x12BC00    /* channel 6's control block */
#define HP_TV_PROGRESS       0x138  /* BCM: 'T' << 24 | phase << 16 | ms
                                       into the window (HP_TV_P_*); the
                                       host reads it after a hang */
#define HP_TV_PROG(ph, ms)   (0x54u << 24 | (uint32_t)(ph) << 16 | ((ms) & 0xFFFF))
#define HP_TV_P_SAVE         1      /* entry read, line buffer filled */
#define HP_TV_P_MV           2      /* the Macrovision off set */
#define HP_TV_P_CLOCK        3      /* pixel clock on */
#define HP_TV_P_GPIO         4      /* GPIO 19 output, high */
#define HP_TV_P_DAC          5      /* DAC on */
#define HP_TV_P_TIMING       6      /* 0x2C08-0x2C18 */
#define HP_TV_P_CTL          7      /* the generator on */
#define HP_TV_P_WAIT31       8      /* waiting for 0x2C00 bit 31 */
#define HP_TV_P_WAIT30       9      /* ... then bit 30 */
#define HP_TV_P_DMA          10     /* channel 6 started */
#define HP_TV_P_HOLD         11     /* the window */
#define HP_TV_P_OFF          12     /* the off order */
#define HP_TV_P_RESTORE      13     /* the saved values written back */
#define HP_TV_P_DONE         14
/* The reply */
#define HP_TV_R_EXIT         0      /* HP_TV_X_* */
#define HP_TV_R_PHASE        1      /* the last on phase reached */
#define HP_TV_R_DAC0         2      /* 0x10005404 at entry */
#define HP_TV_R_MODE0        3      /* 0x10002C18 at entry */
#define HP_TV_R_FIELDS       4      /* changes of 0x2C18 bit 31 in the window */
#define HP_TV_R_US           5      /* the window's length */
#define HP_TV_R_CLK          6      /* 0x10001C30 read after the generator */
#define HP_TV_R_FLAGS        7      /* HP_TV_F_*, denied writes << 8 */
#define HP_TV_R_MISMATCH     8      /* HP_TV_M_*: read back after restore */
#define HP_TV_R_SAMPLES      9      /* HP_TV_SAMPLES pairs: 0x2C00, ch6 status */
#define HP_TV_SAMPLES        10
#define HP_TV_REPLY_WORDS    29     /* HP_TV_R_SAMPLES + 2 * HP_TV_SAMPLES */
#define HP_TV_X_OK           0
#define HP_TV_X_TIMEOUT31    1      /* 0x2C00 bit 31 never came */
#define HP_TV_X_TIMEOUT30    2      /* ... bit 30 never came */
#define HP_TV_X_CHAIN        3      /* channel 6's chain stopped in the window */
#define HP_TV_F_MV           0x01   /* 0x5404 bit 0 was set: the off set written */
#define HP_TV_F_FSEL         0x02   /* GPIO 19's function changed to output */
#define HP_TV_M_MV           0x001  /* any Macrovision word */
#define HP_TV_M_CLOCK        0x002
#define HP_TV_M_FSEL         0x004
#define HP_TV_M_DAC          0x008
#define HP_TV_M_TIMING       0x010  /* 0x2C08 << 0 ... 0x2C18 << 4 */
#define HP_TV_M_CTL          0x200
#define HP_TV_M_DMA          0x400
#define HP_TV_M_GPIO19       0x800  /* the pin's level */
#define HP_OP_PULL           0x15   /* PB-4: HP_PULL_MAGIC, first pin, count
                                       (1-8) -> a word a pin: pin | flags
                                       (HP_PULL_*) | levels: bits 16-17 the
                                       two reads under the up pull, 18-19
                                       under the down pull.  Pins outside
                                       HP_PULL_ALLOW_LO/HI, or not inputs,
                                       are skipped (flagged), never pulled.
                                       Each pin ends pulled down (Apple's
                                       boot state) */
#define HP_PULL_MAGIC        0x50554C4C  /* "PULL" */
#define HP_PULL_MAX          8
#define HP_PULL_ALLOW_LO     0xFF77FFF1  /* 0, 4-18, 20-22, 24-31 */
#define HP_PULL_ALLOW_HI     0x0000FFFF  /* 32-47 */
#define HP_PULL_SKIP_PIN     0x100  /* not on the allowlist */
#define HP_PULL_SKIP_FSEL    0x200  /* its function is not 0 (input) */
#define HP_PULL_UP(w)        ((w) >> 16 & 3)
#define HP_PULL_DOWN(w)      ((w) >> 18 & 3)
/* PINS' four-word form (PB-2): mask 0-31, us, flags (must be 0: no fsel
 * change, no pull), mask 32-51 -> samples, elapsed us, AND 0-31, AND
 * 32-51, OR 0-31, OR 32-51, then HP_CENSUS_EDGE_WORDS words of edge
 * counts, a byte a pin (saturating at 255), pin p in word p / 4, bits
 * 8 * (p % 4) */
#define HP_CENSUS_PINS       52
#define HP_CENSUS_EDGE_WORDS 13
#define HP_CENSUS_WORDS      19
#define HP_OP_CACHE          0x16   /* PW2: HP_CACHE_MAGIC, sub-op, cache
                                       word (0, 0x80000000 or 0x80010000:
                                       Apple's values only), then:
                                       TIME: region (HP_CACHE_R_*) ->
                                         cold us, warm us, cold sum, warm
                                         sum, flush polls;
                                       COH: memory (0 SRAM, 1 SDRAM) ->
                                         the core's load after the host's
                                         write, steps timed out (bit n:
                                         the wait for host step 2n+2),
                                         flush polls.  The handshake word
                                         is HP_CACHE_STEP: the image posts
                                         1, 3, 5, the host answers 2, 4, 6.
                                       Needs SDRAM up and the cache word
                                       reading HP_CACHE_OURS (else
                                       HP_E_STATE with the word read); it
                                       is flushed and put back to that
                                       before the reply */
#define HP_CACHE_MAGIC       0x43414348  /* "CACH" */
#define HP_CACHE_TIME        0
#define HP_CACHE_COH         1
#define HP_CACHE_OURS        0x00010000
#define HP_CACHE_R_SRAM      0      /* 16 KB at 0x12B000 */
#define HP_CACHE_R_SDRAM     1      /* 16 KB at 0xFE3F6000 */
#define HP_CACHE_R_SDRAM1M   2      /* 1 MB at 0xFE100000, 32-byte stride */
#define HP_CACHE_LOADS       262144
#define HP_CACHE_SRAM        0x12B000
#define HP_CACHE_SDRAM       0xFE3F6000
#define HP_CACHE_SDRAM1M     0xFE100000
#define HP_CACHE_STEP        0x12B800    /* the handshake word (SRAM) */
#define HP_CACHE_COH_SRAM    0x12BFF0
#define HP_CACHE_COH_SDRAM   0xFE3FFFF0  /* host 0xC03FFFF0 */
#define HP_CACHE_V0          0x0C0FFEE0
#define HP_CACHE_V1          0x1C0FFEE1  /* the host's */
#define HP_CACHE_V2          0x2C0FFEE2
#define HP_CACHE_STEP_US     100000
#define HP_OP_DMACPY         0x17   /* PW8: HP_DMACPY_MAGIC, case
                                       (HP_DMACPY_*), seed -> DMA us, core
                                       us, wrong words after the DMA, after
                                       the core's copy, channel 7's status,
                                       1 if the DMA timed out.  Channel 7
                                       sub-channel 1 as Apple's dma_memcpy;
                                       HP_DMACPY_BYTES a copy, between the
                                       video image's free regions (the
                                       SDRAM cases need SDRAM up) */
#define HP_DMACPY_MAGIC      0x444D4143  /* "DMAC" */
#define HP_DMACPY_SS         0      /* SRAM 0x12B000 -> SRAM 0x12F000 */
#define HP_DMACPY_SD         1      /* SRAM 0x12B000 -> SDRAM 0xFE3F6000 */
#define HP_DMACPY_DS         2      /* SDRAM 0xFE3F6000 -> SRAM 0x12F000 */
#define HP_DMACPY_CASES      3
#define HP_DMACPY_SRC        0x12B000
#define HP_DMACPY_DST        0x12F000
#define HP_DMACPY_SDRAM      0xFE3F6000
#define HP_DMACPY_BYTES      16384  /* 64 KB (the spec) would overlap the
                                       parked AAC tables at 0x134000 */
#define HP_DMACPY_US         100000
#define HP_OP_PW             0x18   /* PW3, PW4 (brief-map-power.md section
                                       9): the core's voltage and clock at
                                       Apple's table values, off by default.
                                       HP_PW_MAGIC, HP_PW_UNLOCK -> a
                                       one-time code.  Then HP_PW_MAGIC,
                                       step, the code, what (LEVEL: a
                                       regulator level, HP_PW_LEVEL_MIN to
                                       _MAX, on the crystal; POINT: an
                                       index into Apple's ladder,
                                       HP_PW_P_*), hold ms (0 to
                                       HP_PW_HOLD_MS; the self-check runs
                                       over and over that long, at least
                                       once).  Refusals as HP_OP_TV's, plus
                                       HP_E_STATE when the core is not on
                                       the crystal or the regulator's level
                                       is outside 5-15: nothing written.
                                       A run replies HP_PW_REPLY_WORDS
                                       (HP_PW_R_*), HP_OK or, if it ended
                                       early, HP_E_STATE; either way the
                                       start's clock, level and refresh are
                                       written back and read back */
#define HP_PW_MAGIC          0x564F4C54  /* "VOLT" */
#define HP_PW_LEVEL          0      /* PW3: one regulator level, 27 MHz */
#define HP_PW_POINT          1      /* PW4: one clock point and its level */
#define HP_PW_UNLOCK         2
#define HP_PW_STEPS          3
#define HP_PW_LEVEL_MIN      5      /* Apple's table (@141ba) spans 5-15 */
#define HP_PW_LEVEL_MAX      15
#define HP_PW_P_46           0      /* Apple's ladder: MHz, level 5 */
#define HP_PW_P_74           1      /*   level 7 */
#define HP_PW_P_102          2      /*   level 9 */
#define HP_PW_P_150          3      /*   level 15, and a frame pushed */
#define HP_PW_POINTS         4
#define HP_PW_HOLD_MS        20000  /* the spec's meter hold */
#define HP_PW_WIN            0x12B000    /* the self-check's SRAM window */
#define HP_PW_WIN_BYTES      32768  /* 64 KB would reach the parked AAC tables */
#define HP_PW_SEED_A         0x2722C0DE  /* the two fill patterns, by turns */
#define HP_PW_SEED_B         0x5A5AF00D
#define HP_PW_SETTLE_US      10000  /* after a regulator change (@141f0) */
#define HP_PW_PLL_US         1000   /* PLL A before its enable (@4cb8) */
#define HP_PW_STALL_READS    1024   /* the counter read unchanged this
                                       often: stopped, the wait ends */
#define HP_PW_PROGRESS       0x138  /* BCM: 'P' << 24 | phase << 16 | ms
                                       (HP_PW_P*); HP_TV_PROGRESS's word,
                                       never both at once */
#define HP_PW_PROG(ph, ms)   (0x50u << 24 | (uint32_t)(ph) << 16 | ((ms) & 0xFFFF))
#define HP_PW_PH_SAVE        1      /* entry read */
#define HP_PW_PH_REF         2      /* the reference self-checks */
#define HP_PW_PH_VUP         3      /* the regulator up, and its settle */
#define HP_PW_PH_PLL         4      /* PLL A programmed, its wait */
#define HP_PW_PH_CORE        5      /* the core onto PLL A */
#define HP_PW_PH_VDOWN       6      /* the regulator down, and its settle */
#define HP_PW_PH_REFRESH     7      /* the SDRAM refresh for the clock */
#define HP_PW_PH_CHECK       8      /* the self-check at the point */
#define HP_PW_PH_FRAME       9      /* the 150 MHz frame push */
#define HP_PW_PH_BACK        10     /* the restore */
#define HP_PW_PH_DONE        11
#define HP_PW_R_EXIT         0      /* HP_PW_X_* */
#define HP_PW_R_PHASE        1      /* the last phase reached before BACK */
#define HP_PW_R_REG0         2      /* 0x10001400 at entry */
#define HP_PW_R_REG          3      /* ... at the point */
#define HP_PW_R_CORE         4      /* 0x10001C28: entry 15:0, point 31:16 */
#define HP_PW_R_PLLA         5      /* at the point: +0x00 << 16 | +0x14 */
#define HP_PW_R_REF0         6      /* SDRAM 0x10003424 at entry (0: down) */
#define HP_PW_R_REF          7      /* ... at the point */
#define HP_PW_R_SUM0         8      /* pattern A at entry: the CPU's sum */
#define HP_PW_R_SUM          9      /* the first wrong run's, else the last */
#define HP_PW_R_VSUM0        10     /* pattern A at entry: the vector sum */
#define HP_PW_R_VSUM         11
#define HP_PW_R_RUNS         12     /* runs at the point | wrong << 16 */
#define HP_PW_R_US0          13     /* one self-check at entry, us */
#define HP_PW_R_US           14     /* ... at the point (the first run) */
#define HP_PW_R_FRAME        15     /* the frame push at 150 MHz, us */
#define HP_PW_R_FLAGS        16     /* HP_PW_F_*, denied writes << 8 */
#define HP_PW_R_MISMATCH     17     /* HP_PW_M_*: read back after restore */
#define HP_PW_R_HOLD         18     /* us spent at the point */
#define HP_PW_REPLY_WORDS    19
#define HP_PW_X_OK           0
#define HP_PW_X_REFBAD       1      /* CPU and vector disagreed at entry:
                                       nothing written */
#define HP_PW_X_READBACK     2      /* a register read back wrong at the point */
#define HP_PW_X_CHECK        3      /* a self-check came out wrong */
#define HP_PW_X_STALL        4      /* the counter stopped in a wait */
#define HP_PW_F_SDRAM        0x01   /* SDRAM up: the refresh followed */
#define HP_PW_F_FRAME        0x02   /* the frame was pushed */
#define HP_PW_F_DARK         0x04   /* 150 MHz with the panel off: no frame */
#define HP_PW_M_REG          0x01
#define HP_PW_M_REFRESH      0x02
#define HP_PW_M_CORE         0x04
#define HP_PW_M_PLLN         0x08
#define HP_PW_M_PLLM         0x10
#define HP_PW_M_LCD          0x20   /* the panel's bus timing words */
#define HP_PW_M_STALL        0x40   /* a restore settle ended on a stall */
#define HP_OP_HIB            0x19   /* PW5 (brief-map-power.md sections 3
                                       and 9): Apple's hibernate park
                                       (videocore=1, @14288), off by
                                       default.  HP_HIB_MAGIC, HP_HIB_UNLOCK
                                       -> a one-time code.  Then
                                       HP_HIB_MAGIC, HP_HIB_PARK, the code,
                                       the wait bound in ms (1 to
                                       HP_HIB_WAIT_MS).  The image lays its
                                       patterns, shows the bars, parks the
                                       PLLs, the GPIO event words and SDRAM
                                       as Apple does, writes HP_HIB_PARKED
                                       to HP_HIB_WORD and polls it for
                                       HP_HIB_WAKE; the host drives GPO32
                                       bits 15 and 14 in between.  Then
                                       everything back, read back,
                                       HP_HIB_AWAKE (or HP_HIB_GAVEUP), the
                                       checks, the bars mirrored, and the
                                       reply: HP_HIB_REPLY_WORDS
                                       (HP_HIB_R_*), HP_OK or, if it ended
                                       early, HP_E_STATE.  Refusals as
                                       HP_OP_PW's (nothing written) */
#define HP_HIB_MAGIC         0x48494245  /* "HIBE" */
#define HP_HIB_PARK          0
#define HP_HIB_UNLOCK        1
#define HP_HIB_STEPS         2
#define HP_HIB_WAIT_MS       90000  /* the image's own bound on the park */
#define HP_HIB_WORD          0x1F0  /* Apple's handshake word; ours is
                                       BCMFW_MB_RECT_XY, idle meanwhile */
#define HP_HIB_PARKED        0xA5A50000  /* Apple's values (section 3) */
#define HP_HIB_WAKE          0xA5A50001  /*   host: wake up */
#define HP_HIB_AWAKE         0xA5A50002
#define HP_HIB_GAVEUP        0xA5A500FF  /* ours: no wake word in the bound */
#define HP_HIB_SRAM          0x12B000    /* HP_LAB_SRAM: 32 KB, below the */
#define HP_HIB_SRAM_BYTES    32768       /*   parked AAC tables */
#define HP_HIB_SDRAM         0xFE3F6000  /* after the dumps, 32 KB */
#define HP_HIB_SDRAM_BYTES   32768
#define HP_HIB_SEED_S        0x48494231  /* the patterns' seeds */
#define HP_HIB_SEED_D        0x48494232
#define HP_HIB_SDIDLE_POLLS  8000   /* Apple's bound on the SDRAM idle */
#define HP_HIB_PROGRESS      0x138  /* BCM: 'H' << 24 | phase << 16 | ms
                                       (HP_HIB_PH_*); HP_TV_PROGRESS's
                                       word, never both at once */
#define HP_HIB_PROG(ph, ms)  (0x48u << 24 | (uint32_t)(ph) << 16 | ((ms) & 0xFFFF))
#define HP_HIB_PH_SAVE       1      /* entry read */
#define HP_HIB_PH_PATTERN    2      /* the SRAM and SDRAM patterns */
#define HP_HIB_PH_LOOK1      3      /* the bars, pushed */
#define HP_HIB_PH_PLL        4      /* the PLLs parked */
#define HP_HIB_PH_GPIO       5      /* the event words saved and cleared */
#define HP_HIB_PH_SDRAM      6      /* SDRAM idled */
#define HP_HIB_PH_PARKED     7      /* polling the word; ms parked */
#define HP_HIB_PH_WAKE       8      /* the wake word seen (or given up) */
#define HP_HIB_PH_RESTORE    9      /* PLLs, SDRAM, GPIO back */
#define HP_HIB_PH_CHECK      10     /* the patterns checked */
#define HP_HIB_PH_LOOK2      11     /* the bars mirrored, pushed */
#define HP_HIB_PH_DONE       12
#define HP_HIB_R_EXIT        0      /* HP_HIB_X_* */
#define HP_HIB_R_PHASE       1      /* the last phase reached before WAKE */
#define HP_HIB_R_PARK_US     2      /* entry to the parked word */
#define HP_HIB_R_HELD_US     3      /* the counter, parked to wake word */
#define HP_HIB_R_POLLS       4      /* reads of the word while parked */
#define HP_HIB_R_RESTORE_US  5      /* wake word to HP_HIB_AWAKE */
#define HP_HIB_R_SRAM_BAD    6      /* words of the SRAM pattern wrong */
#define HP_HIB_R_SRAM_FIRST  7      /* the first one's index (~0: none) */
#define HP_HIB_R_SDRAM_BAD   8      /* ... SDRAM's (~0: not laid) */
#define HP_HIB_R_SDRAM_FIRST 9
#define HP_HIB_R_FRAME_PRE   10     /* the host frame's sum, parked */
#define HP_HIB_R_FRAME_POST  11     /* ... after the wake */
#define HP_HIB_R_SD          12     /* SDRAM's bring-up: 0, VIDEO_SD_*,
                                       ~0 not up */
#define HP_HIB_R_REG         13     /* 0x10001400: entry 15:0, wake 31:16 */
#define HP_HIB_R_CORE        14     /* 0x10001C28: entry 15:0, wake 31:16 */
#define HP_HIB_R_FLAGS       15     /* HP_HIB_F_*, denied writes << 8 */
#define HP_HIB_R_MISMATCH    16     /* HP_HIB_M_*: read back after restore */
#define HP_HIB_R_CLO_PARK    17     /* the counter at the park */
#define HP_HIB_R_CLO_WAKE    18     /* ... at the wake word */
#define HP_HIB_REPLY_WORDS   19
#define HP_HIB_X_OK          0
#define HP_HIB_X_GAVEUP      1      /* no wake word in the bound */
#define HP_HIB_X_STALL       2      /* the counter stopped while parked */
#define HP_HIB_X_SDIDLE      3      /* SDRAM's bit 15 never cleared: not
                                       parked */
#define HP_HIB_X_SDRAM       4      /* SDRAM's bring-up failed after */
#define HP_HIB_F_SDRAM       0x01   /* SDRAM was up: idled, brought up */
#define HP_HIB_F_PANEL       0x02   /* the panel on: the bars pushed */
#define HP_HIB_M_PLL         0x001  /* << word (0-7: B, C, D, A dividers
                                       and controls, the park's order) */
#define HP_HIB_M_GPIO        0x100  /* an event-enable word */
#define HP_HIB_M_CACHE       0x200  /* the cache word's enables */
#define HP_WARM_WORD         0x13C
#define HP_WARM_PARKED       0x50524B00  /* "PRK": parked, upload now */
#define HP_WARM_GO           0x57474F00  /* "WGO": host, after the upload */
#define HP_WARM_UP           0x57555000  /* "WUP": the new image started */
#define HP_WARM_PANEL_ON     0x01
#define HP_WARM_TIMING1      0x02
#define HP_WARM_STATE(w)     ((w) & ~0xFFu)
/* The park's steps (BCMFW_PARK_FLUSH images; bench row B13's
 * diagnostics): BCM, in HP_WARM_PROG, 'W' << 24 | step << 16 | detail */
#define HP_WARM_PROG         0x138
#define HP_WARM_PROG_MAGIC   0x57        /* 'W' */
#define HP_WARM_PROG_W(st, d) (HP_WARM_PROG_MAGIC << 24 | (uint32_t)(st) << 16 | \
                               ((d) & 0xFFFF))
#define HP_WARM_PH_ENTER     1           /* park() entered */
#define HP_WARM_PH_BLIT      2           /* the LCD DMA waited out */
#define HP_WARM_PH_IRQ       3           /* interrupts off, sources off */
#define HP_WARM_PH_COPIED    4           /* the stub copied; detail: words */
#define HP_WARM_PH_FLUSHED   5           /* the cache flushed; detail:
                                            retries << 12 | polls (4095 max) */
#define HP_WARM_PH_CALL      6           /* calling the stub; detail: the
                                            flush's, again */
#define HP_WARM_STUB_ADDR    0xDE900     /* BCMFW_PARK_STUB, for the PP's
                                            read-back */
#define HP_AUDIO_CONFIG      0
#define HP_AUDIO_PROCESS     1
#define HP_OP_VCLK           0x1A   /* P19 PW4, the core's level bands
                                       (clock.h): HP_VCLK_MAGIC, then
                                       HP_VCLK_STAT (0), or 1 + a band (0
                                       the crystal .. 11 150 MHz) and
                                       optionally a clock in it (whole
                                       MHz above the band below's top; 0
                                       or none: the band's top), run now
                                       at Apple's level, in Apple's order,
                                       its speed checked (any failure: the
                                       crystal).  The governor is the
                                       host's.  -> struct clock_gov's 7
                                       words (HP_VCLK_R_*).  A change
                                       lands between packets, never
                                       mid-frame.  HP_E_INVAL: anything
                                       else */
#define HP_VCLK_MAGIC        0x56434C4B  /* "VCLK" */
#define HP_VCLK_STAT         0
#define HP_VCLK_BANDS        12
#define HP_VCLK_R_BAND       0      /* (a failed switch: 0, the crystal) */
#define HP_VCLK_R_MHZ        1      /* the clock in the band */
#define HP_VCLK_R_BUSY       2      /* us busy, wraps */
#define HP_VCLK_R_NOW        3      /* the 1 MHz counter */
#define HP_VCLK_R_REGS       4      /* regulator writes */
#define HP_VCLK_R_PLLS       5      /* PLL A locks */
#define HP_VCLK_R_FAILS      6      /* fallbacks to the crystal */
#define HP_VCLK_WORDS        7
#define HP_OP_AAC            0x0F   /* AAC-LC on the chip (M4, stage 9):
                                       session, sub-op, then:
                                       OPEN: ASC offset, length (SDRAM)
                                         -> rate, channels, aac_open's code,
                                         then (dev42) the tables: the
                                         FNV-1a of the parked copy the
                                         first OPEN took, their load
                                         address in the image, bytes, and
                                         the FNV-1a of SDRAM's copy now;
                                       DECODE: AU offset, length, PCM
                                         offset (SDRAM; 4 KB out: 1024
                                         stereo s16 frames) -> frames (0 or
                                         1024), core us, MHz, aac_decode's
                                         code (< 0: HP_E_CORRUPT);
                                       RESET -> nothing;
                                       PROBE (no session needed): the
                                         32-bit bcs/bcc check -> taken bits
                                         (bcs 15:0, bcc 31:16), the bits
                                         vc2sim's model expects, cases.
                                       MEMPROBE (dev42, no session): word
                                         fills, then byte and halfword
                                         stores into SRAM and into SDRAM
                                         (VIDEO_AAC_MEMPROBE) -> per memory
                                         the 4 words ld reads back and
                                         ldb|ldb|ldh of three of them
                                         (SRAM 0-4, SDRAM 5-9; want
                                         HP_MEMPROBE_W0..W4)
                                       STATE (no session): first pair ->
                                         the pairs the last DECODE
                                         recorded, then up to 15 (tag,
                                         value) pairs from that one
                                         (bcmfw/aac/aac.h AAC_T_*: the
                                         parse's checkpoints, stage 9's
                                         dump-and-diff, session 36c).
                                       OPEN takes an optional fifth
                                         word, flags (aac.h AAC_FLAG_*;
                                         0 when absent).
                                       Offsets: inside video.h's
                                       VIDEO_AUDIO_* rings or VIDEO_AAC_IO;
                                       an H.264 session must be open (its
                                       OPEN put SDRAM up and the tables in) */
#define HP_AAC_OPEN          0
#define HP_AAC_DECODE        1
#define HP_AAC_RESET         2
#define HP_AAC_PROBE         3
#define HP_AAC_MEMPROBE      4
#define HP_AAC_STATE         5
#define HP_AAC_STATE_WORDS   31     /* the count, then 15 pairs: one reply */
#define HP_AAC_MEMSTRESS     6      /* (no session) -> 7 mismatch counts in
                                       SRAM, 7 in SDRAM (video.h
                                       VIDEO_AAC_STRESS: 0 if SDRAM is not
                                       up), SDRAM's first failure (pattern
                                       + 1, index, got, want), SDRAM up.
                                       Patterns: word, byte, halfword store
                                       then an immediate load of it; a
                                       byte, a halfword into a word then
                                       the word; aac_memcpy's halfword
                                       copy; aac_memset from a halfword */
#define HP_AAC_MEMSTRESS_PATTERNS 7
/* MEMPROBE's pattern: 16 bytes of 0xA5 by word stores, then stb 0x11 at
 * +1, stb 0x22 at +6, sth 0x3344 at +8, sth 0x5566 at +14; what correct
 * byte lanes read back (little-endian words), and ldb +1 | ldb +6 << 8 |
 * ldh +8 << 16 */
#define HP_MEMPROBE_W0       0xA5A511A5
#define HP_MEMPROBE_W1       0xA522A5A5
#define HP_MEMPROBE_W2       0xA5A53344
#define HP_MEMPROBE_W3       0x5566A5A5
#define HP_MEMPROBE_W4       0x33442211
#define HP_OP_OVERLAY        0x0C   /* session, x|y<<16, w|h<<16: the host's
                                       frame (BCMFW_FB) over the picture in
                                       that panel rectangle until w is 0
                                       (dev34: the shell's scrub bar);
                                       [slide]: HP_OVL_* | ms (below) */
/* HP_OP_OVERLAY's optional fourth word (HP_CAP_OVERLAY_SLIDE): the band
 * slides up from its rectangle's bottom edge (UP, from wherever a running
 * slide has it) or down and away (DOWN: the rectangle is the one up, and
 * the band goes as w 0 would at the end), over ms by the image's own
 * clock, eased out; the picture is put back under the rows it leaves.
 * 0 (or no fourth word): at once, as before; a running slide up goes on. */
#define HP_OVL_SLIDE_UP      0x00010000
#define HP_OVL_SLIDE_DOWN    0x00020000
#define HP_OVL_SLIDE_MS(s)   ((s) & 0xFFFF)

/* Unsolicited events (tag 0).  Bit 7 clear: replies always have it set,
 * so the two never mix up.  Commands are 0x01-0x5F, events 0x60-0x7F. */
#define HP_EV_ERROR          0x61   /* status, pts lo, pts hi */
#define HP_EV_EOS            0x62   /* session, last pts lo, hi */
#define HP_EV_LOST           0x63   /* session closed by the BCM (LCD_SLEEP) */

/* Status / error codes */
#define HP_OK                0
#define HP_E_VERSION         1
#define HP_E_INVAL           2
#define HP_E_BUSY            3
#define HP_E_NOSESSION       4
#define HP_E_UNSUPPORTED     5      /* codec, profile, size or mode */
#define HP_E_NOMEM           6
#define HP_E_FIFO            7      /* packet outside the FIFO or not in order */
#define HP_E_CORRUPT         8
#define HP_E_TOOBIG          9      /* record longer than HP_MAX_PAYLOAD */
#define HP_E_UNKNOWN         10     /* opcode this image does not implement */
#define HP_E_STATE           11

/* OPEN */
#define HP_CODEC_NULL        0
#define HP_CODEC_H264        1
#define HP_CODEC_MPEG4       2
#define HP_MODE_FIT          0      /* scale to fit, letterbox (keep aspect) */
#define HP_MODE_FILL         1      /* scale to fill, crop */
#define HP_MODE_NATIVE       2      /* 1:1, centred */
#define HP_OPEN_OWN_DISPLAY  0x100  /* LCD_UPDATEs are dropped while open */
#define HP_OPEN_SDRAM_DPB    0x400  /* H.264, diagnostic (dev11): frames
                                       in SDRAM even when they fit in
                                       SRAM, and the memory test first */
#define HP_OPEN_FAST_CLOCK   0x200  /* H.264: core at 150 MHz for the session
                                       (the top band; HP_OP_VCLK moves it
                                       after, in HP_CAP_VCLK images)
                                       (BCMFW_VIDEO_PLL images; the host
                                       sets it only with a stage 4b pass
                                       flag); ignored by other images */
#define HP_OPEN_C_SHIFT      12     /* H.264, diagnostic (dev14): bits */
#define HP_OPEN_C_MASK       0x1F000 /* 12-15 run these decoder families
                                       in scalar C (H264_OFF_* << 12: MC,
                                       IDCT, deblocking, intra), bit 16 the
                                       display conversion; 0 = vectors */
#define HP_OPEN_C_DISP       0x10000
#define HP_OPEN_DUMP         0x40000 /* H.264, diagnostic (dev18): the
                                       first picture's planes copied to
                                       SDRAM (video.h VIDEO_DUMP_PIC) */
#define HP_OPEN_SELFTEST     0x20000 /* H.264, diagnostic (dev14): the
                                       vector kernels against their C
                                       versions at OPEN (video.h VIDEO_ST_*,
                                       the guard block's words 16-20) */
#define HP_MAX_W             640
#define HP_MAX_H             480

/* FEED flags */
#define HP_FEED_KEY          0x1
#define HP_FEED_EOS          0x2
#define HP_FEED_DISCONT      0x4
#define HP_FEED_ACK          0x8    /* reply even on success */
#define HP_FEED_HIDE         0x10   /* decode, don't show (a seek's
                                       catch-up from its key) */

/* ---- Where the rings live (memory map: hostproto.md) ------------------- */
#define HP_CMD_RING          0xD0000
#define HP_CMD_WORDS         2048   /* 8 KB */
#define HP_EVT_RING          0xD2000
#define HP_EVT_WORDS         1024   /* 4 KB */
/* Bitstream FIFO.  Skeleton: an SRAM window (the null codec reads
 * nothing).  Video builds: SDRAM 0x000000, 1 MB (host address
 * 0xC0000000); the header says which. */
#define HP_SRAM_FIFO         0xD4000
#define HP_SRAM_FIFO_SIZE    0x8000

#if BCMFW_HOSTPROTO && !defined(__ASSEMBLER__) && defined(BCMFW_H)
void hp_init(void);
void hp_poll(void);
/* 1 while a session owns the display (LCD updates are dropped) */
int hp_owns_display(void);
/* LCD_SLEEP arrived: end any session (HP_EV_LOST) */
void hp_lcd_sleep(void);
/* an unsolicited event to the host */
void hp_event(uint32_t op, uint32_t status, const uint32_t *p, uint32_t n);
#if BCMFW_VIDEO
/* decoder work between command passes; 1 if there may be more */
int hp_work(void);
#endif
#if BCMFW_JOBS && (BCMFW_JOBS_DECODE || BCMFW_JOBS_OVL)
/* the heartbeat alone, from inside a running job (bcmfw_service) */
void hp_heartbeat(void);
#endif
/* Apple's whole-cache flush (vmcs.bin @25a), for code just written by the
 * core or the host, before the first call into it; interrupts off.
 * Returns re-requests << 12 | polls */
uint32_t hp_code_flush(void);
#endif

#endif /* BCMFW_HOSTPROTO_H */
