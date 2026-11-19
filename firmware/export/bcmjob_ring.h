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

#ifndef BCMJOB_RING_H
#define BCMJOB_RING_H

#include <stdint.h>
#include <stdio.h>

/* Sitting b10 (2026-11-09): the job self-test timed out on the merged
 * image b9bcbe57 ("bcmjob fault op=0x25 why=timeout", rc -4) with the
 * heartbeat moving.  This is what a bench build (HAVE_BCMFW_DEVTEST)
 * reads back through the port when a job wait runs out, before anything
 * else touches the chip (bcmjob-core.c timed_out(): no doorbell, the port
 * lock held), so a reading separates "the kick was never seen, the image
 * asleep" from "the image took the job and stalled":
 *
 *   the image's command ring indices (HP_CMD_WR as the image sees it,
 *   HP_CMD_RD, how far it has read) beside the driver's own write index;
 *   the ring word at the image's read index (the header it would take
 *   next: a length past what was written stalls its loop for good);
 *   J_FENCE against the last FENCE put, J_DONE against the jobs sent,
 *   J_CUR_OP (the op running, 0 between jobs);
 *   the heartbeat (HP_STATE bits 15:8, stepped only by the image's main
 *   loop or a DECODE's service, never by an interrupt), the idle tick and
 *   sleep counts and the doorbell count (bcmfw config.h BCMFW_MB_IDLE_*,
 *   at 0x1D4-0x1DC in a video image), the info word (0x1E8: its
 *   BCMFW_INFO_TICK_SLOW bit says the 10 ms tick runs), the idle gate
 *   (0x16C, "SLP1" = sleep allowed) and the LCD contract's command word;
 *   then the same words again after BJR_WINDOW_US with no doorbell: does
 *   the heartbeat step on the tick alone, does the image take the record
 *   late.
 *
 * Pure: the device's driver fills it, the bench rows and the host tests
 * read it. */

#define BJR_WINDOW_US       25000u  /* 2.5 of the slow 10 ms idle ticks */
#define BJR_LOOKS_MAX       4u      /* looks a boot (each costs the window) */

/* bcmfw config.h, a video image's mailbox (the merged image is one) */
#define BJR_MB_IDLE_EN      0x16C   /* BCMFW_MB_IDLE_EN: "SLP1" sleep on */
#define BJR_IDLE_EN_MAGIC   0x534C5031u
#define BJR_MB_IDLE_TICKS   0x1D4   /* timer compare 1 handler runs */
#define BJR_MB_IDLE_BELLS   0x1D8   /* doorbell handler runs */
#define BJR_MB_IDLE_SLEEPS  0x1DC   /* sleeps the idle loop returned from */
#define BJR_MB_INFO         0x1E8   /* BCMFW_MB_INFO */
#define BJR_INFO_TICK_SLOW  0x0008  /* BCMFW_INFO_TICK_SLOW: the 10 ms tick */
#define BJR_MB_LCD_CMD      0x1F8   /* the LCD contract's command word */

struct bjc_ring_look
{
    uint32_t seq;           /* looks taken this boot; 0: none yet */
    uint32_t bad;           /* port reads that gave up (0: all read) */
    uint32_t host_wr;       /* the driver's own write index (16 bits) */
    uint32_t cmd_wr;        /* HP_CMD_WR as the image sees it (raw) */
    uint32_t cmd_rd;        /* HP_CMD_RD, the image's read index (raw) */
    uint32_t next_hdr;      /* the ring word at the image's read index */
    uint32_t fence;         /* J_FENCE (raw) */
    uint32_t fence_want;    /* the last FENCE put (16 bits) */
    uint32_t done;          /* J_DONE less the power-up's start */
    uint32_t sent;          /* jobs sent this power-up */
    uint32_t cur_op;        /* J_CUR_OP (raw) */
    uint32_t beat0, beat1;  /* HP_STATE (raw) at the look, after the window */
    uint32_t ticks0, ticks1;    /* the idle tick count then and after */
    uint32_t bells, sleeps; /* after the window */
    uint32_t info;          /* BCMFW_MB_INFO */
    uint32_t idle_en;       /* BCMFW_MB_IDLE_EN */
    uint32_t lcd_cmd;       /* the LCD contract's command word */
    uint32_t cmd_rd1;       /* HP_CMD_RD after the window (raw) */
    uint32_t done1;         /* J_DONE less the start, after the window */
    uint32_t window_us;     /* the window as measured */
    /* the port lock's record (bjc_port lock_stats; 0s without one): the
     * mutex refused (count, last caller), the lock entered with the tick
     * block already set (count, last caller) */
    uint32_t lk_off, lk_off_at, lk_nest, lk_nest_at;
};

/* hostproto.h HP_VALID: an index word written whole */
static inline int bjr_valid(uint32_t w)
{
    return (((w >> 16) ^ (w & 0xFFFF)) == 0xFFFF);
}

/* 1: the image's read index reached the driver's write index (it took
 * every record put, the timed-out one too); 0: not; -1: unreadable */
static inline int bjr_took(const struct bjc_ring_look *l)
{
    if (!bjr_valid(l->cmd_rd))
        return -1;
    return (l->cmd_rd & 0xFFFF) == (l->host_wr & 0xFFFF);
}

/* 1: J_FENCE has reached the last FENCE put; 0: not; -1: unreadable */
static inline int bjr_fence_done(const struct bjc_ring_look *l)
{
    if (!bjr_valid(l->fence))
        return -1;
    return !(((l->fence & 0xFFFF) - l->fence_want) & 0x8000);
}

/* 1: the heartbeat (HP_STATE bits 15:8, as bcmjob-core.c alive() reads
 * it) stepped over the window with no doorbell; 0: not */
static inline int bjr_beat_moved(const struct bjc_ring_look *l)
{
    return ((l->beat0 ^ l->beat1) >> 8 & 0xFF) != 0;
}

static inline const char *bjr_word(int v, const char *yes, const char *no)
{
    return v < 0 ? "unread" : v ? yes : no;
}

/* The one line: "image took the record: yes/no; fence: done/not;
 * heartbeat moved: yes/no" */
static inline int bjr_fmt_verdict(char *buf, size_t n,
                                  const struct bjc_ring_look *l)
{
    return snprintf(buf, n, "image took the record: %s; fence: %s; "
                    "heartbeat moved: %s", bjr_word(bjr_took(l), "yes", "no"),
                    bjr_word(bjr_fence_done(l), "done", "not"),
                    bjr_word(bjr_beat_moved(l), "yes", "no"));
}

/* The ring at the look: the image's indices, the driver's, the next
 * header, the fence, the jobs done of those sent, the op running */
static inline int bjr_fmt_ring(char *buf, size_t n,
                               const struct bjc_ring_look *l)
{
    return snprintf(buf, n, "wr %04lx host %04lx rd %04lx next %08lx fence "
                    "%04lx/%04lx done %lu/%lu cur %02lx lcd %08lx bad %lu",
                    (unsigned long)(l->cmd_wr & 0xFFFF),
                    (unsigned long)(l->host_wr & 0xFFFF),
                    (unsigned long)(l->cmd_rd & 0xFFFF),
                    (unsigned long)l->next_hdr,
                    (unsigned long)(l->fence & 0xFFFF),
                    (unsigned long)(l->fence_want & 0xFFFF),
                    (unsigned long)l->done, (unsigned long)l->sent,
                    (unsigned long)(l->cur_op & 0xFF),
                    (unsigned long)l->lcd_cmd, (unsigned long)l->bad);
}

/* The window with no doorbell: the heartbeat, the idle ticks, the read
 * index and J_DONE after it; the doorbell and sleep counts, the info
 * word, the idle gate */
static inline int bjr_fmt_window(char *buf, size_t n,
                                 const struct bjc_ring_look *l)
{
    return snprintf(buf, n, "%lu us no bell: beat %02lx>%02lx ticks +%lu "
                    "rd %04lx done %lu bells %lu sleeps %lu info %08lx "
                    "sleep %s", (unsigned long)l->window_us,
                    (unsigned long)(l->beat0 >> 8 & 0xFF),
                    (unsigned long)(l->beat1 >> 8 & 0xFF),
                    (unsigned long)(l->ticks1 - l->ticks0),
                    (unsigned long)(l->cmd_rd1 & 0xFFFF),
                    (unsigned long)l->done1, (unsigned long)l->bells,
                    (unsigned long)l->sleeps, (unsigned long)l->info,
                    l->idle_en == BJR_IDLE_EN_MAGIC ? "on" : "off");
}

/* The same, as short lines for ipodui.log (uilog.h UI_LOG_LINE: 63
 * characters a line): line k of BJR_LOG_LINES, prefixed "bcmjob ring " */
#define BJR_LOG_LINES       6
static inline int bjr_fmt_log(char *buf, size_t n,
                              const struct bjc_ring_look *l, int k)
{
    switch (k)
    {
    case 0:
        return snprintf(buf, n, "bcmjob ring took=%s fence=%s beat=%s bad=%lu",
                        bjr_word(bjr_took(l), "yes", "no"),
                        bjr_word(bjr_fence_done(l), "done", "not"),
                        bjr_word(bjr_beat_moved(l), "yes", "no"),
                        (unsigned long)l->bad);
    case 1:
        return snprintf(buf, n, "bcmjob ring wr=%04lx host=%04lx rd=%04lx "
                        "next=%08lx", (unsigned long)(l->cmd_wr & 0xFFFF),
                        (unsigned long)(l->host_wr & 0xFFFF),
                        (unsigned long)(l->cmd_rd & 0xFFFF),
                        (unsigned long)l->next_hdr);
    case 2:
        return snprintf(buf, n, "bcmjob ring fence=%04lx/%04lx done=%lu/%lu "
                        "cur=%02lx lcd=%08lx", (unsigned long)(l->fence & 0xFFFF),
                        (unsigned long)(l->fence_want & 0xFFFF),
                        (unsigned long)l->done, (unsigned long)l->sent,
                        (unsigned long)(l->cur_op & 0xFF),
                        (unsigned long)l->lcd_cmd);
    case 3:
        return snprintf(buf, n, "bcmjob ring us=%lu beat=%02lx>%02lx "
                        "ticks=+%lu rd=%04lx done=%lu",
                        (unsigned long)l->window_us,
                        (unsigned long)(l->beat0 >> 8 & 0xFF),
                        (unsigned long)(l->beat1 >> 8 & 0xFF),
                        (unsigned long)(l->ticks1 - l->ticks0),
                        (unsigned long)(l->cmd_rd1 & 0xFFFF),
                        (unsigned long)l->done1);
    case 4:
        return snprintf(buf, n, "bcmjob ring bells=%lu sleeps=%lu info=%08lx "
                        "sleep=%s", (unsigned long)l->bells,
                        (unsigned long)l->sleeps, (unsigned long)l->info,
                        l->idle_en == BJR_IDLE_EN_MAGIC ? "on" : "off");
    case 5:
        return snprintf(buf, n, "bcmjob ring lockoff=%lu@%08lx nest=%lu@%08lx",
                        (unsigned long)l->lk_off, (unsigned long)l->lk_off_at,
                        (unsigned long)l->lk_nest,
                        (unsigned long)l->lk_nest_at);
    }
    return 0;
}

/* ---- #257 (sitting b11): the overlay swap -------------------------------
 * The merged image hung inside OVERLAY ("cur=35", heartbeat frozen) both
 * ways when one overlay was loaded over the other in its one region.  The
 * driver now flushes the chip's caches from the host (bcmjob-core.c
 * host_code_flush) before such a region is rewritten and again before
 * OVERLAY; each flush's record is below.  The word is 0x1000_0C00:
 * before, after (as read back), re-requests (bit 0 read 1 at once), polls
 * until bit 0, microseconds, and rc (0, or the BCMJOB_* failure). */
struct bjc_code_flush
{
    uint32_t seq;           /* flushes this boot (0: none yet) */
    int32_t rc;
    uint32_t before, after;
    uint32_t rq, polls, us;
};

/* When an OVERLAY's reply never comes, a bench build reads back, before
 * the restart, what the host can see of the load (no image change: the
 * merged image b9bcbe57 has no step word of its own):
 *   J_CUR_OP (0x35: inside OVERLAY), J_OPS (the evicted overlay's bits
 *   gone: the image ran its ovl_drop; the new overlay's bits set: its
 *   init returned), J_CAPS, the cache word;
 *   the region's FNV-1a over the file's bytes, read through the port
 *   (fnv_read = fnv_want: the region holds the file, so nothing wrote
 *   back over it, #257 H(b); different: something did);
 *   the load's two code flushes.
 * step is the host's reading (BJO_STEP_*), not a word the image wrote.
 * The same look can be taken on demand (bcmjob_dev_ovl_look_now) after a
 * load that answered, so a bench row can check the region too. */
enum
{
    BJO_STEP_UNREAD = 0,    /* a read gave up */
    BJO_STEP_OTHER,         /* J_CUR_OP is not OVERLAY */
    BJO_STEP_QUEUED,        /* OVERLAY, the evicted overlay's ops still set */
    BJO_STEP_DROPPED,       /* OVERLAY, ops dropped, the new init not back */
    BJO_STEP_INIT_DONE      /* the new overlay's ops are set */
};
struct bjc_ovl_look
{
    uint32_t seq;           /* looks this boot (0: none yet) */
    uint32_t k;             /* the load's overlay (BJC_OVL_*) */
    uint32_t step;          /* BJO_STEP_* */
    uint32_t bad;           /* port reads that gave up */
    uint32_t base, bytes;   /* the region read back */
    uint32_t fnv_want, fnv_read;
    uint32_t cur_op, ops, caps, cache;
    uint32_t evicted_ops;   /* the ops the overlay it replaced had added */
    uint32_t us;            /* the look's time */
    struct bjc_code_flush drain, code;  /* the load's flushes (seq 0: none) */
};

static inline const char *bjo_step_name(uint32_t step)
{
    static const char *const n[] = { "unread", "not-overlay", "queued",
                                     "dropped", "init-done" };
    return step < sizeof(n) / sizeof(n[0]) ? n[step] : "?";
}

/* The look as short lines for ipodui.log (63 characters a line) */
#define BJO_LOG_LINES       3
static inline int bjo_fmt_log(char *buf, size_t n,
                              const struct bjc_ovl_look *l, int k)
{
    switch (k)
    {
    case 0:
        return snprintf(buf, n, "bcmjob ovlook k=%lu step=%s cur=%02lx bad=%lu",
                        (unsigned long)l->k, bjo_step_name(l->step),
                        (unsigned long)(l->cur_op & 0xFF), (unsigned long)l->bad);
    case 1:
        return snprintf(buf, n, "bcmjob ovlook fnv=%08lx want=%08lx %s",
                        (unsigned long)l->fnv_read, (unsigned long)l->fnv_want,
                        l->fnv_read == l->fnv_want ? "same" : "DIFFERS");
    case 2:
        return snprintf(buf, n, "bcmjob ovlook ops=%08lx ev=%08lx cache=%08lx",
                        (unsigned long)l->ops, (unsigned long)l->evicted_ops,
                        (unsigned long)l->cache);
    }
    return 0;
}

#endif /* BCMJOB_RING_H */
