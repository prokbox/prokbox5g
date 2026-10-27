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

/* The video player's core (#64 piece 6, the glue; player.md sections
   2-8): the engine (vplay.c) wired to the BCM through the VP client
   (bcmvp.c), with
     - the sample index read in windows from the .vix, and its key table;
     - each sample copied into the FIFO as Annex B, streamed in pieces,
       the parameter sets in front after every (re)start;
     - the STATUS round trip kept asynchronous, so the player never waits
       on a decode;
     - the stage A image's limits worked around: it decodes every FEED in
       FIFO order, so a dropped GOP (or a slideshow skip above 2x) restarts
       the session and re-pushes from the next sample posted;
     - the power states: the BCM is cut when the display goes dark and
       brought back, in sync with the audio, when it wakes;
     - liveness and recovery: a dead image is reloaded once; a second
       failure, a missing image or a protocol mismatch gives the BCM back
       to the display driver and video stops, while the audio plays on
       (the fallback: the normal CPU-pushed screen).

   Pure logic, like vplay.c: the caller (vplayer.c on the PP and in the
   simulator, tests/video/test_vpcore.c on the host) owns the thread, the
   clock and the side effects, through struct vpc_source and struct
   vpc_backend. */

#ifndef VPCORE_H
#define VPCORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "bcmvp.h"
#include "vplay.h"
#include "vtrace.h"
#include "vix.h"
#include "vclk.h"

/* The caller's memory (player.md section 5): the index window, the read
   and staging pieces and the parameter sets.  The key table comes
   separately, once its size is known (vpc_init's return). */
#define VPC_WINDOW_RECORDS  2048        /* 24 KB of index, ~68 s at 30 fps */
#define VPC_PIECE           2048        /* bytes per copy into the FIFO */
#define VPC_PARAMS_MAX      512
#define VPC_MEM_SIZE        (VPC_WINDOW_RECORDS * VIX_RECORD_SIZE + \
                             VPC_PIECE + 2 * (VPC_PIECE + 8) + \
                             VPC_PARAMS_MAX + 16)

#define VPC_LEAD_US         35000       /* record to screen, 320x240 */
#define VPC_LEAD_BIG_US     70000       /* 640x480 */
/* A seek while playing aims past the audio by the image's catch-up from
   the key (dev32), at this work per picture until STATUS says, at most
   VPC_CATCHUP_MAX_US */
/* The player thread's wait while playing (C5): until the next record is
   due, between a kernel tick and four */
#define VPC_WAIT_MIN_MS     10
#define VPC_WAIT_MAX_MS     40
#define VPC_WORK_GUESS_US   12000
#define VPC_CATCHUP_MAX_US  1000000
/* Playing, records out, and the picture still for this long: the session
   is stuck somewhere (session 28: 7.8 s of nothing at a file's start, cured
   by the next seek); logged once and restarted where the audio is */
#define VPC_STILL_US        2000000
/* a still restart that hasn't moved the picture this long after: the
   image is reloaded (session 30) */
#define VPC_STILL_RELOAD_US 1000000
/* "playing", but the audio's clock has not moved for this long: the
   pictures rightly wait for it, so the still watchdog stands down
   (session 36c: audio that never started read as a still and reloaded
   the image for nothing) */
#define VPC_AUDIO_STILL_US  1000000
#define VPC_STILL_MAX       3       /* restarts in a row, no picture
                                       between: give up (BVP_E_STILL) */
/* direct_read's waits for "not buffered" before it gives up (about 3 s
   at vplayer's 10 ms wait) */
#define VPC_DIRECT_TRIES    300
/* "sample source failed" (session 35: three plays resumed at 18:15 gave
   up at once): the next key instead, this many times in a row (a picture
   shown between resets the count), then the video gives up (dev42) */
#define VPC_SRC_TRIES       3

/* Why the index or the file failed a sample (dev42; logged at the
   failure, struct vpc_src_fail) */
enum vpc_src_why
{
    VPC_SRC_WINDOW = 1,         /* the .vix window's read came up short */
    VPC_SRC_EMPTY,              /* the record says 0 bytes */
    VPC_SRC_PROBE,              /* the sample's last byte: read failed */
    VPC_SRC_BEGIN,              /* bvp_pkt_begin refused (not the port) */
    VPC_SRC_PIECE,              /* a piece of the sample: read failed */
};
struct vpc_src_fail
{
    uint32_t why;               /* enum vpc_src_why, 0 none */
    uint32_t sample, size;      /* the sample and its record's size */
    long rc;                    /* what the read or begin returned */
    uint32_t want, at;          /* bytes asked for, and from (the piece's
                                   offset in the sample, or the window's
                                   first record) */
};

/* The video file and its index.  read: the video's bytes at off (n, or
   VPC_NOT_BUFFERED while they are not buffered yet, or another < 0);
   index_read: the .vix's bytes (n or < 0). */
#define VPC_NOT_BUFFERED    (-0x100)
struct vpc_source
{
    void *ctx;
    long (*read)(void *ctx, uint64_t off, void *buf, uint32_t n);
    long (*index_read)(void *ctx, uint64_t off, void *buf, uint32_t n);
    /* optional: let the buffering thread run (a read answered "not
       buffered yet" and must be retried) */
    void (*wait)(void);
};

/* The BCM.  take: from the display driver (0 or < 0); boot: power-cycle,
   start the video image and power the panel on, 0 once it reports ready;
   cut: power off (the display went dark); release: back to the display
   driver, which repaints the normal screen; log: one line. */
struct vpc_backend
{
    const struct bvp_port *port;
    int (*take)(void);
    int (*boot)(void);
    void (*cut)(void);
    void (*release)(void);
    void (*log)(const char *what, long a, long b);
    /* extra HP_OPEN_* bits for every OPEN (HP_OPEN_FAST_CLOCK when the
       4b and 4c pass flags allow it); NULL: none */
    uint32_t (*open_flags)(void);
    /* each STATUS whose picture moved (stage 6's trace, vtrace.h); NULL:
       none */
    void (*frame)(const struct vpc_frame *f);
    /* p14-resident: at a stop, after CLOSE, give the panel back with the
       image left running (it is the display driver's own image): 0 done,
       < 0 not possible (then cut and release as without it); NULL: never.
       take/boot see the same: boot returns at once when the image taken
       already runs. */
    int (*hand_back)(void);
    /* P19 PW4: the chip's level bands this unit verified (vclk.h; bit b
       for band b, bit 0 the crystal), the crystal's level and the in-band
       policy (enum vclk_policy); 0 or NULL:
       no governor, the session's clock as OPEN set it (today's) */
    uint32_t (*vclk)(unsigned *xtal_level, unsigned *policy);
    /* The first-use check (vcheck.h): run once, at the play's start,
       between HELLO and the session's OPEN when vpc.check_due is set;
       the image attached and past HELLO, no session open, and left so.
       0: play on (the backend's open_flags and vclk now say the band);
       < 0: the play fails with that fail_why (BVP_E_CHECK).  NULL:
       none. */
    int (*check)(struct bvp *b);
    /* Live invalidation (vcheck.h enum vcheck_bad): a decode error, a
       concealed block, a clock fallback or the image lost, once a play,
       outside any reply's handling (the backend may call vpc_vclk_cap);
       a is the count or the clock seen.  NULL: none. */
    void (*unit_bad)(int why, long a);
};

enum vpc_state
{
    VPC_IDLE = 0,
    VPC_PLAYING,                /* frames going out (or paused with them) */
    VPC_ASLEEP,                 /* the display is dark: BCM off */
    VPC_DONE,                   /* the last frame is up */
    VPC_FAILED,                 /* video gave up; the audio plays on */
};

struct vpc_stats
{
    uint32_t restarts;          /* session restarts for drops and skips */
    uint32_t wakes, recoveries;
    uint32_t pieces, bytes;     /* copied into the FIFO */
    uint32_t statuses;          /* STATUS replies */
    uint32_t feed_again;        /* a FEED found the ring full */
    uint32_t stills;            /* VPC_STILL_US restarts */
    uint32_t audio_stills;      /* stalls of the audio's clock (watchdog
                                   stood down) */
    uint32_t reloads;           /* image reloads after a still restart */
    uint32_t src_skips;         /* "sample source failed": to the next key */
    uint32_t data_waits;        /* pushes held: the sample not buffered yet
                                   (the rest of the engine's push waits:
                                   FIFO or packet queue full, lockstep) */
    uint32_t piece_waits;       /* a sample's piece "not buffered" after
                                   its last byte was (the audio's buffer
                                   reset under us): pushed again later */
    uint32_t aborts;            /* packets dropped part way (port or
                                   "not buffered"); params ride again */
};

/* What is read from a lost image (bcmfw/video/video.h, config.h) */
#define VPC_PM_WORDS        9   /* progress, packets, address, frames,
                                   fault pc, fault sp, info, state, status */

/* Stage 6's SDRAM readback (device-session-1.md, "6s"): a pattern
   through the host port into the FIFO, read back word by word */
#define VPC_SDC_BLOCKS      16          /* one 1 KB block every 64 KB */
#define VPC_SDC_WHOLE       64          /* the same over all 4 MB */
#define VPC_SDC_SDRAM       0x400000
#define VPC_SDC_READS       4           /* words read back per block */
struct vpc_sdcheck
{
    uint32_t done;              /* 1 once it ran */
    uint32_t words, bad;        /* read back, wrong */
    uint32_t first_off, first_got, first_want;  /* the first wrong one */
    int port_err;               /* a host-port failure, 0 if none */
    uint32_t span;              /* bytes covered from the FIFO's base */
    uint32_t last_ok;           /* offset of the last block written */
};

/* A host-port failure while the image plays (session 11: a 20 ms
   handshake timed out with the image alive and beating) is ridden out
   when HP_STATE reads back valid within VPC_STALL_US, up to
   VPC_STALL_MAX times a session; beyond that the image is lost */
#define VPC_STALL_US        250000
#define VPC_STALL_MAX       8

/* An event index that stays torn this many polls is a protocol failure
   (session 12: the VP header overwritten on the chip), not a stall */
#define VPC_TORN_MAX        25

/* The image's guard block (bcmfw/video/video.h VIDEO_GUARD, dev14) */
#define VPC_GUARD_AT        0xDF000
#define VPC_HOST_FB         0xE0000     /* bcmfw config.h BCMFW_FB */
#define VPC_PANEL_W         320
#define VPC_PANEL_H         240
#define VPC_GUARD_WORDS     60
#define VPC_RF_MAGIC        0x52460000  /* video.h VIDEO_RF_MAGIC */
#define VPC_ER_MAGIC        0x45520000  /* video.h VIDEO_ER_MAGIC */

struct vpc
{
    const struct vpc_source *src;
    const struct vpc_backend *be;
    enum vpc_state state;
    int fail_why;               /* why VPC_FAILED: a BVP_E_* or HP_E_* */

    struct vix_header hdr;
    uint8_t *win;               /* index window */
    uint32_t win_first, win_n;
    uint8_t *piece;             /* read buffer */
    uint8_t *stage;             /* staging (whole-word writes), one of */
    uint8_t *stage_buf[2];      /* two: a piece's DMA may still read the
                                   last while the next is filled (dev39) */
    uint32_t stage_n;
    uint8_t *params;
    uint32_t params_len;
    struct vp_key *keys;
    uint32_t nkeys;

    /* Annex B in pieces */
    uint32_t nal_left;          /* NAL bytes before the next length */
    uint32_t len_pos;           /* length bytes seen, 0-3 */
    uint32_t nal_acc;           /* the length being read */

    struct bvp bvp;
    struct vp eng;
    struct vp_ops ops;
    struct vp_status st;
    int status_tag;             /* the STATUS in flight, 0 if none */
    uint32_t shown_seen;        /* STATUS's pictures-shown count */
    bool fresh;                 /* the session was just opened */
    bool restart_pending;       /* unposted packets were skipped */
    bool resync;                /* reseek at the next tick */
    bool eos_posted;            /* the last sample's FEED went out */
    int failures;               /* image deaths this session */
    uint32_t open_drop;         /* HP_OPEN_* bits dropped after OPEN
                                   failed with them (FAST_CLOCK: SDRAM) */
    uint32_t sd_pll[5];         /* the failed 150 MHz OPEN's SDRAM words */
    bool sd_check;              /* stage 6: a readback after OPEN */
    bool sd_whole;              /*   over all 4 MB, not just the FIFO */
    /* the image's words when it was last lost (dev11): VPC_PM_* order,
       pm_n of them read before the port failed */
    uint32_t pm[VPC_PM_WORDS], pm_n, pm_losses;
    bool pm_fast;               /* lost while on PLL A */
    uint32_t dpb;               /* frame 0's luma plane, at the first
                                   picture (BCM address), 0 before */
    struct vpc_sdcheck sdc;
    /* stage 6's write modes (dev12, bcmvp.h struct bvp_wire), and
       lockstep: no packet is written while one posted is undecoded */
    struct bvp_wire wire;
    bool lockstep;
    uint32_t lock_waits;        /* pushes held back by lockstep */
    /* host-port stalls ridden out: the image still beat (VPC_STALL_*) */
    uint32_t stalls, stall_max_us;
    bool pkt_open;              /* a packet begun and not ended */
    uint32_t boot_us;           /* the last bring_up's boot, us */
    uint32_t work_avg_us;       /* the image's work per picture (STATUS
                                   word 13, averaged): a seek's catch-up */
    uint32_t shown_wall_us;     /* when the picture last moved (port
                                   clock), or the session (re)started */
    uint32_t still_mark;        /* the engine's posts then */
    uint32_t still_run;         /* watchdog restarts since a picture */
    uint32_t still_reload_at;   /* port clock: reload if still then, 0 no */
    uint32_t audio_moved_us;    /* port clock: the audio's clock last moved
                                   (or playing began) */
    bool audio_still_said;      /* "audio stands still" logged this stall */
    uint32_t st_words[16];      /* the last STATUS reply */
    bool first_set;             /* the session's first packet (params */
    uint32_t first_off;         /* first): its FIFO offset and first */
    uint32_t first_bytes[4];    /* 16 bytes as the port's words */
    uint32_t seek_wall_us;      /* a seek's start (port clock), 0 none */
    uint32_t seek_waits;        /* the engine's push waits then */
    uint32_t seek_data_waits;   /* and stats.data_waits (dev42) */
    uint32_t ovl_x, ovl_y, ovl_w, ovl_h;    /* the shell's overlay, w 0 */
    const uint16_t *ovl_src;    /* none; its pixels (the shell's frame) */
    uint32_t ovl_pitch;
    /* the guard block as last read (at a loss or the stop), guard_n
       words of it */
    uint32_t guard[VPC_GUARD_WORDS], guard_n;
    uint32_t guard_said[2];     /* the guard's last summary line */
    uint32_t refused_at;        /* the image's refusal: file << 16 | line */
    uint32_t refused_nal;       /* and the NAL type it refused */
    uint32_t error_said;        /* the first decode error logged */
    uint32_t mhz_said;          /* the chip's clock as last logged */
    uint32_t start_us;          /* port clock at vplayer_start; 0 once
                                   the first picture is logged */
    bool read_direct;           /* the source reads the file only (the
                                   retry after a refusal, dev32) */
    bool push_port_err;         /* a push met a port failure: vpc_tick
                                   rides it out or reloads */
    struct vpc_src_fail src_fail;   /* the last source failure (dev42) */
    uint32_t src_run;           /* source failures since the last picture */
    int64_t audio_us;           /* the clock at the current tick */
    struct vpc_stats stats;
    /* P19 PW4: the chip's clock governor (vclk.h), once the image answers
       HP_OP_VCLK and the backend names verified bands */
    bool vclk_on, vclk_inited;
    struct vclk_gov gov;
    struct vclk_video vfl;      /* the session's floor (a client) */
    int vclk_tag;               /* the HP_OP_VCLK in flight, 0 none */
    int vclk_asked;             /* the band it asked for, -1 a reading */
    int vclk_pending;           /* the governor's want waits for the
                                   command in flight */
    uint32_t vclk_busy, vclk_now;   /* the meter's last reading */
    uint32_t vclk_poll_at;      /* port clock: the last reading asked */
    uint32_t vclk_miss_mark;    /* the engine's skipped and dropped */
    uint32_t vclk_misses;
    int64_t vclk_pts;           /* the frame time's last picture */
    uint32_t vclk_shown;
    uint32_t vclk_force;        /* the bench: 1 + a band held all play
                                   (the floor fixed), 0 the governor */
    /* the first-use check (vcheck.h) */
    bool check_due;             /* run the backend's check at the start */
    uint32_t sess_mhz;          /* the session's clock without the
                                   governor (150 or a capped band's), 0
                                   unknown: a lower STATUS clock is a
                                   fallback */
    uint8_t ub_why;             /* a live invalidation waiting, 0 none */
    bool ub_said;               /* reported this play */
    long ub_a;
};

/* The first-use check's live step-down: no band above b for the rest of
   the play (the governor's mask, or HP_OP_VCLK now when it isn't
   running); 0, or < 0 when the session can't be asked */
int vpc_vclk_cap(struct vpc *v, int b);

/* P19 PW4: the governor's means over the play so far (vclk.h): false if
   it never ran */
bool vpc_vclk_means(const struct vpc *v, uint32_t *mhz, uint32_t *lvl10,
                    uint32_t *idle_pm, uint32_t *ms);

/* Reads the index's header and counts its key samples, with mem
   (VPC_MEM_SIZE bytes, any alignment) for the window and pieces.
   Returns the key count (> 0; the key table vpc_keys() needs), or < 0. */
long vpc_init(struct vpc *v, const struct vpc_source *src,
              const struct vpc_backend *be, void *mem, size_t len);

/* Fills keys (nkeys entries, from vpc_init) from the index; 0 or < 0. */
int vpc_keys(struct vpc *v, struct vp_key *keys, uint32_t nkeys);

/* Takes and boots the BCM, opens the session and starts at audio_us
   (the audio already plays the file).  The state after: VPC_PLAYING, or
   VPC_FAILED with the BCM given back. */
enum vpc_state vpc_start(struct vpc *v, int64_t audio_us);

/* Once per kernel tick: the audio clock, whether it runs, and whether the
   display is lit.  Returns the state; *wait_ms is how long the caller may
   sleep before the next tick. */
enum vpc_state vpc_tick(struct vpc *v, int64_t audio_us, bool playing,
                        bool display_on, uint32_t *wait_ms);

/* The audio was seeked (by the user): the video follows. */
void vpc_seek(struct vpc *v, int64_t audio_us);

void vpc_set_speed(struct vpc *v, int percent);

/* The shell's overlay (the scrub bar, dev34): w x h pixels of src (rows
   pitch pixels apart, RGB565) into the image's host frame at panel x, y,
   then shown over the picture until vpc_overlay() with w 0.  x and w go
   to even pixels (the port moves words).  Kept: every later OPEN (a
   restart, a wake) puts it up again from src, which must stay valid
   while it shows.  0, or < 0 / an HP_E_*. */
int vpc_overlay(struct vpc *v, uint32_t x, uint32_t y, uint32_t w,
                uint32_t h, const uint16_t *src, uint32_t pitch);
/* The same, the band sliding (HP_OVL_SLIDE_UP | ms: up from its bottom
   edge; HP_OVL_SLIDE_DOWN | ms: the band up goes down and away, the
   rectangle ignored) by the image's own clock */
int vpc_overlay_slide(struct vpc *v, uint32_t x, uint32_t y, uint32_t w,
                      uint32_t h, const uint16_t *src, uint32_t pitch,
                      uint32_t slide);

/* Stage 8b (dev39): PCM through the chip and back.  config names the
   rings (SDRAM offsets, bcmfw video.h VIDEO_AUDIO_*); block writes bytes
   of pcm into in + off, has the chip copy them to out + off at gain
   (Q16), and reads them back into pcm, with each part's time. */
struct vpc_audio_times
{
    uint32_t write_us, call_us, chip_us, read_us;
};
int vpc_audio_config(struct vpc *v, uint32_t in, uint32_t out,
                     uint32_t size);
int vpc_audio_block(struct vpc *v, uint32_t in, uint32_t out, uint32_t off,
                    uint16_t *pcm, uint32_t bytes, uint32_t gain,
                    struct vpc_audio_times *t);

/* Ends the session and gives the BCM back. */
void vpc_stop(struct vpc *v);

/* Whether an avcC's profile byte and constraint flags (avcC bytes 1 and 2)
   describe a stream the image decodes: Baseline (66), or any profile that
   declares Baseline conformance (constraint_set0_flag, 0x80), as some
   encoders label Constrained Baseline as Main.  Refused streams play as
   audio alone. */
static inline bool vpc_baseline_ok(uint8_t profile, uint8_t constraints)
{
    return profile == 66 || (constraints & 0x80);
}

#endif /* VPCORE_H */
