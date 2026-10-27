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
 * The TV-out probe rows' pure logic (PB-7 to PB-9: docs/bcm2722-reference/
 * brief-map-peripherals.md section 11; bench-p19.md, cards PB-7 to PB-9):
 * the PP's timeouts and hang watchdog for HP_OP_TV, the reply's checks and
 * verdicts, the format arms, and which row is unlocked in this boot.  The
 * device half is in devbcm_probes.c; tests/tvprobe/ checks this on the host.
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
#ifndef _IPODUI_DEVBCM_TV_H_
#define _IPODUI_DEVBCM_TV_H_

#include <stdbool.h>
#include <stdint.h>
#include "../../bcmfw/hostproto.h"

/* ---- Windows and timeouts ----------------------------------------------
 * PB-7's window is half a second, not the spec's 1 s: enough to tell a
 * field rate from a frame rate and NTSC from PAL (see tvp_rate_class);
 * PB-8 and PB-9 hold a picture 4 s, long enough to look at. */
#define TVP_PB7_MS      500
#define TVP_OUT_MS      4000
#define TVP_HANG_US     2000000     /* neither word moved: a hang */
#define TVP_LOOK_US     10000       /* the PP reads the two words this often */
/* The reply's bound: the window, both start waits, then the 2 s of the
 * other probes' hang rule for the set-up, the off order and the reply */
#define TVP_MARGIN_US   (2 * HP_TV_WAIT_US + TVP_HANG_US)

static inline uint32_t tvp_deadline_us(uint32_t ms)
{
    return ms * 1000u + TVP_MARGIN_US;
}

/* The watchdog: the progress word (HP_TV_PROGRESS) moves with each phase
 * and each ms of the window, the heartbeat (HP_STATE) each ms of the
 * window and in the image's loop; a hang is neither moving for
 * TVP_HANG_US. */
struct tvp_dog
{
    uint32_t prog, beat, at_us;
    bool seen;
};

static inline void tvp_dog_start(struct tvp_dog *d, uint32_t now_us)
{
    d->seen = false;
    d->at_us = now_us;
}

/* One look at both words: true once neither has moved for TVP_HANG_US */
static inline bool tvp_dog_hung(struct tvp_dog *d, uint32_t now_us,
                                uint32_t prog, uint32_t beat)
{
    if (!d->seen || prog != d->prog || beat != d->beat)
    {
        d->seen = true;
        d->prog = prog;
        d->beat = beat;
        d->at_us = now_us;
        return false;
    }
    return now_us - d->at_us >= TVP_HANG_US;
}

/* The progress word's parts (0 phase when the tag isn't 'T') */
static inline uint32_t tvp_prog_phase(uint32_t w)
{
    return w >> 24 == 0x54 ? w >> 16 & 0xFF : 0;
}

static inline uint32_t tvp_prog_ms(uint32_t w)
{
    return w >> 24 == 0x54 ? w & 0xFFFF : 0;
}

/* ---- Requests ----------------------------------------------------------
 * The step request after HP_TV_MAGIC, its word count back (0: bad) */
static inline uint32_t tvp_request(uint32_t *a, bool out, uint32_t std,
                                   uint32_t code, uint32_t ms, uint32_t fmt,
                                   uint32_t w704, uint32_t pat)
{
    a[0] = HP_TV_MAGIC;
    a[1] = out ? HP_TV_OUTPUT : HP_TV_TIMING;
    a[2] = std;
    a[3] = code;
    a[4] = ms;
    if (std > HP_TV_PAL || ms == 0 ||
        ms > (out ? HP_TV_OUTPUT_MS : HP_TV_TIMING_MS))
        return 0;
    if (!out)
        return 5;
    if (fmt >= HP_TV_ARMS || w704 > HP_TV_W704 || pat > HP_TV_PAT_LINES)
        return 0;
    a[5] = fmt;
    a[6] = w704;
    a[7] = pat;
    return 8;
}

/* ---- Replies -----------------------------------------------------------
 * status: bvp's (HP_OK, HP_E_* >= 0, or < 0 for no reply); n words */
struct tvp_reply
{
    int status;
    uint32_t n;
    uint32_t w[HP_TV_REPLY_WORDS];
};

enum tvp_check
{
    TVP_OK = 0,         /* the window ran to its end, all put back */
    TVP_NOREPLY,        /* a hang, the port, or the deadline */
    TVP_REFUSED,        /* the image refused (HP_E_INVAL, _BUSY ...) */
    TVP_SHORT,          /* not HP_TV_REPLY_WORDS words */
    TVP_EARLY,          /* the run ended before the window's end */
    TVP_RESTORE,        /* a register read back wrong after the restore */
    TVP_DENIED,         /* the image denied a write (the allowlist hit) */
};

static inline uint32_t tvp_denied(const struct tvp_reply *r)
{
    return r->w[HP_TV_R_FLAGS] >> 8;
}

/* The checks common to every step, in that order */
static inline enum tvp_check tvp_check(const struct tvp_reply *r)
{
    if (r->status < 0)
        return TVP_NOREPLY;
    if (r->status != HP_OK && r->status != HP_E_STATE)
        return TVP_REFUSED;
    if (r->n != HP_TV_REPLY_WORDS)
        return TVP_SHORT;
    if (r->status != HP_OK || r->w[HP_TV_R_EXIT] != HP_TV_X_OK)
        return TVP_EARLY;
    if (r->w[HP_TV_R_MISMATCH])
        return TVP_RESTORE;
    if (tvp_denied(r))
        return TVP_DENIED;
    return TVP_OK;
}

static inline const char *tvp_check_name(enum tvp_check c)
{
    static const char *const s[] = {
        "ok", "no reply", "refused", "short reply", "ended early",
        "restore mismatch", "write denied",
    };
    return (unsigned)c < sizeof(s) / sizeof(s[0]) ? s[c] : "?";
}

/* Changes of 0x2C18 bit 31 a second, in thousandths (0: no window) */
static inline uint32_t tvp_rate_mhz(uint32_t fields, uint32_t us)
{
    return us ? (uint32_t)((uint64_t)fields * 1000000000u / us) : 0;
}

/* PB-7: the change rate against the standard's field rate (bit 31 a
 * field bit) and half of it (a frame bit).  The tolerance is 2 % plus
 * one change in the window (the count's own step). */
enum tvp_rate
{
    TVP_R_FIELD = 0,    /* 59.94 (NTSC) or 50 (PAL) a second */
    TVP_R_FRAME,        /* half that */
    TVP_R_NONE,         /* no change at all */
    TVP_R_ODD,          /* moved, at neither rate */
};

static inline enum tvp_rate tvp_rate_class(uint32_t std, uint32_t fields,
                                           uint32_t us)
{
    uint32_t field = std == HP_TV_PAL ? 50000 : 59940;
    uint32_t got = tvp_rate_mhz(fields, us), step, k;

    if (!fields || !us)
        return TVP_R_NONE;
    step = (uint32_t)(1000000000u / us);
    for (k = 0; k < 2; k++)
    {
        uint32_t want = field >> k, tol = want / 50 + step;

        if (got + tol >= want && got <= want + tol)
            return k ? TVP_R_FRAME : TVP_R_FIELD;
    }
    return TVP_R_ODD;
}

static inline const char *tvp_rate_name(enum tvp_rate r)
{
    static const char *const s[] = {
        "field rate", "frame rate", "no changes", "neither rate",
    };
    return (unsigned)r < sizeof(s) / sizeof(s[0]) ? s[r] : "?";
}

/* PB-7 passes on a clean run whose changes come at the standard's field
 * or frame rate */
static inline bool tvp_pb7_pass(uint32_t std, const struct tvp_reply *r)
{
    enum tvp_rate k;

    if (tvp_check(r) != TVP_OK)
        return false;
    k = tvp_rate_class(std, r->w[HP_TV_R_FIELDS], r->w[HP_TV_R_US]);
    return k == TVP_R_FIELD || k == TVP_R_FRAME;
}

/* ---- PB-8's arms: a standard and a FIFO format ------------------------ */
#define TVP_ARMS        (2 * HP_TV_ARMS)

static inline uint32_t tvp_arm_std(int arm)
{
    return (uint32_t)arm / HP_TV_ARMS;
}

static inline uint32_t tvp_arm_fmt(int arm)
{
    return (uint32_t)arm % HP_TV_ARMS;
}

static inline const char *tvp_std_name(uint32_t std)
{
    return std == HP_TV_PAL ? "pal" : "ntsc";
}

static inline const char *tvp_fmt_name(uint32_t fmt)
{
    static const char *const s[HP_TV_ARMS] = { "uyvy", "yuyv", "rgb565",
                                               "y32" };
    return fmt < HP_TV_ARMS ? s[fmt] : "?";
}

/* What the owner saw on the TV (PB-8) */
enum tvp_saw
{
    TVP_SAW_BARS = 0,
    TVP_SAW_GARBLED,
    TVP_SAW_NOTHING,
    TVP_SAWS,
};

static inline const char *tvp_saw_name(int saw)
{
    static const char *const s[TVP_SAWS] = { "bars", "garbled", "nothing" };
    return saw >= 0 && saw < TVP_SAWS ? s[saw] : "no answer";
}

/* PB-9: the lines at each input width, then the owner's pick */
enum tvp_edge
{
    TVP_EDGE_FULL = 0,  /* the lines reach both edges */
    TVP_EDGE_BAND,      /* a band or border down a side */
    TVP_EDGE_GARBLED,
    TVP_EDGE_NOTHING,
    TVP_EDGES,
};

static inline const char *tvp_edge_name(int e)
{
    static const char *const s[TVP_EDGES] = { "full", "band", "garbled",
                                              "nothing" };
    return e >= 0 && e < TVP_EDGES ? s[e] : "no answer";
}

enum tvp_pick
{
    TVP_PICK_640 = 0,
    TVP_PICK_704,
    TVP_PICK_SAME,
    TVP_PICK_NEITHER,
    TVP_PICKS,
};

static inline const char *tvp_pick_name(int p)
{
    static const char *const s[TVP_PICKS] = { "640", "704", "same",
                                              "neither" };
    return p >= 0 && p < TVP_PICKS ? s[p] : "no answer";
}

/* What PB-9 says about the 640 -> 704 step, from the two edge answers */
enum tvp_scale
{
    TVP_SCALER = 0,     /* 640 in fills the 704 line: the path scales */
    TVP_WINDOW,         /* 640 in leaves a band, 704 fills: no scaling */
    TVP_UNCLEAR,
};

static inline enum tvp_scale tvp_scale_verdict(int e640, int e704)
{
    if (e704 != TVP_EDGE_FULL)
        return TVP_UNCLEAR;
    if (e640 == TVP_EDGE_FULL)
        return TVP_SCALER;
    if (e640 == TVP_EDGE_BAND)
        return TVP_WINDOW;
    return TVP_UNCLEAR;
}

static inline const char *tvp_scale_name(enum tvp_scale s)
{
    return s == TVP_SCALER ? "scaler" : s == TVP_WINDOW ? "window"
                                                        : "unclear";
}

/* ---- The rows' locks, for this boot ------------------------------------
 * PB-8 opens once PB-7 passed for both standards; PB-9 once a PB-8 arm
 * whose window ran was marked "bars" (the last such arm is PB-9's). */
struct tvp_lock
{
    bool pb7;
    int bars_arm;       /* -1: none yet */
};

static inline void tvp_lock_init(struct tvp_lock *l)
{
    l->pb7 = false;
    l->bars_arm = -1;
}

static inline bool tvp_pb8_open(const struct tvp_lock *l)
{
    return l->pb7;
}

static inline bool tvp_pb9_open(const struct tvp_lock *l)
{
    return l->pb7 && l->bars_arm >= 0 && l->bars_arm < TVP_ARMS;
}

static inline void tvp_note_pb7(struct tvp_lock *l, bool ntsc, bool pal)
{
    if (ntsc && pal)
        l->pb7 = true;
}

static inline void tvp_note_pb8(struct tvp_lock *l, int arm,
                                const struct tvp_reply *r, int saw)
{
    if (saw == TVP_SAW_BARS && arm >= 0 && arm < TVP_ARMS &&
        r->status == HP_OK && r->n == HP_TV_REPLY_WORDS &&
        r->w[HP_TV_R_EXIT] == HP_TV_X_OK)
        l->bars_arm = arm;
}

#endif /* _IPODUI_DEVBCM_TV_H_ */
