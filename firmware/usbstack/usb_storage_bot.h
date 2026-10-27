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
 * USB mass storage, Bulk-Only Transport decisions (ProkBox, USB
 * reliability): the parts of usb_storage.c's state machine that decide
 * what a bulk completion means between commands, and how a WRITE whose
 * storage write failed finishes. Pure, so tests/usb/bot_test.c runs them
 * on the host against a model host and endpoint layer.
 *
 * Rules (usb_storage.c keeps them):
 * - Only an OUT completion can be a command. A late IN completion (a CSW
 *   or data that finished after a reset) is never parsed as a CBW.
 * - A CBW is exactly 31 bytes (BOT 6.2.1); anything else is invalid and
 *   handled like a bad signature (both pipes halted, BOT 6.6.1).
 * - A command that fails before its data phase (READ or WRITE out of range
 *   or without a disk, a refused field) halts the data pipe the CBW names,
 *   when dCBWDataTransferLength is not 0, before the failed CSW (BOT 6.7.2
 *   cases 4 and 10); a command with no data phase to refuse (a READ or WRITE
 *   of 0 blocks) gets its CSW at once.
 * - A WRITE whose storage write fails keeps receiving its data phase
 *   (the next chunk is already primed) and writes none of it; the CSW
 *   (FAIL) follows the last chunk. Priming the CBW while a data receive
 *   is still primed let the host's next data land in the CBW buffer.
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
#ifndef USB_STORAGE_BOT_H
#define USB_STORAGE_BOT_H

#include <stdint.h>
#include <stdbool.h>

#define UBOT_CBW_LEN 31

/* the three states between commands */
enum ubot_wait {
    UBOT_WAIT_CMD,          /* CSW done (or none): only a CBW may come */
    UBOT_WAIT_CSW_OR_CMD,   /* CSW and the next CBW both primed */
    UBOT_WAIT_CSW,          /* CBW here, its CSW predecessor still out */
};

enum ubot_act {
    UBOT_IGNORE,            /* stale or unexpected: nothing changes */
    UBOT_RUN_CBW,           /* execute the CBW in the buffer */
    UBOT_BAD_CBW,           /* invalid CBW: halt both pipes */
    UBOT_HOLD_CBW,          /* CBW came first: run it when the CSW is done */
    UBOT_CSW_DONE,          /* the CSW went out: wait for the command */
};

/* the held CBW's own completion: was it a valid CBW? */
static inline bool ubot_cbw_valid(int status, int length)
{
    return status == 0 && length == UBOT_CBW_LEN;
}

/* a bulk completion between commands; held_ok: ubot_cbw_valid() of the
 * CBW held in UBOT_WAIT_CSW */
static inline enum ubot_act ubot_between(enum ubot_wait w, bool in,
                                         int status, int length,
                                         bool held_ok)
{
    switch (w) {
        case UBOT_WAIT_CMD:
            if (in)
                return UBOT_IGNORE;
            return ubot_cbw_valid(status, length) ? UBOT_RUN_CBW
                                                  : UBOT_BAD_CBW;
        case UBOT_WAIT_CSW_OR_CMD:
            /* an invalid CBW also waits for the CSW, then is rejected */
            return in ? UBOT_CSW_DONE : UBOT_HOLD_CBW;
        case UBOT_WAIT_CSW:
            if (!in)
                return UBOT_IGNORE;
            return held_ok ? UBOT_RUN_CBW : UBOT_BAD_CBW;
    }
    return UBOT_IGNORE;
}

/* a WRITE's failure, kept until its CSW */
struct ubot_wr {
    bool failed;
    uint32_t residue;       /* bytes not written, from the first failure */
};

static inline void ubot_wr_begin(struct ubot_wr *w)
{
    w->failed = false;
    w->residue = 0;
}

/* a chunk's storage write failed; `left` is the command's residue after
 * this chunk was received, `chunk` its bytes */
static inline void ubot_wr_fail(struct ubot_wr *w, uint32_t left,
                                uint32_t chunk)
{
    if (!w->failed) {
        w->failed = true;
        w->residue = left + chunk;
    }
}

static inline bool ubot_wr_should_write(const struct ubot_wr *w)
{
    return !w->failed;
}

/* the residue the CSW reports */
static inline uint32_t ubot_wr_residue(const struct ubot_wr *w,
                                       uint32_t residue)
{
    return w->failed ? w->residue : residue;
}

/* --- failing before the data phase (BOT 6.7.2 cases 4 and 10) ------------ */

enum ubot_stall {
    UBOT_STALL_NONE,        /* no data phase was announced */
    UBOT_STALL_IN,
    UBOT_STALL_OUT,
};

/* which pipe to halt before a failed CSW, for a CBW with this length and
 * direction */
static inline enum ubot_stall ubot_err_stall(uint32_t dtl, bool dir_in)
{
    if (!dtl)
        return UBOT_STALL_NONE;
    return dir_in ? UBOT_STALL_IN : UBOT_STALL_OUT;
}

/* a READ or WRITE of this many blocks has no data phase at all */
static inline bool ubot_zero_len(uint32_t blocks)
{
    return blocks == 0;
}

/* a reply never exceeds the host's buffer (dCBWDataTransferLength) or
 * the command's own allocation length */
static inline uint32_t ubot_clamp(uint32_t dtl, uint32_t alloc, uint32_t have)
{
    uint32_t n = have;

    if (n > dtl)
        n = dtl;
    if (n > alloc)
        n = alloc;
    return n;
}

/* --- while the storage handover is pending ------------------------------- */

enum ubot_early {
    UBOT_EARLY_HOLD,        /* keep the CBW until the handover is done */
    UBOT_EARLY_TUR,         /* TEST UNIT READY: NOT READY, becoming ready */
    UBOT_EARLY_SENSE,       /* REQUEST SENSE: answered from the saved sense */
};

/* holding every command makes a host that retries TUR hit its command
 * timeout; these two touch no storage, so they are answered at once */
static inline enum ubot_early ubot_early_cmd(uint8_t op)
{
    if (op == 0x00)
        return UBOT_EARLY_TUR;
    if (op == 0x03)
        return UBOT_EARLY_SENSE;
    return UBOT_EARLY_HOLD;
}

/* --- pages --------------------------------------------------------------- */

/* the mode page `code` (without header and block descriptor) into buf:
 * its length, or 0 when the page is not supported. Caching (0x08): write
 * cache and read-ahead off, so a host never waits for a flush that does not
 * come; informational exceptions (0x1C): reporting disabled. */
static inline unsigned ubot_mode_page(unsigned code, uint8_t *buf)
{
    unsigned n, i;

    if (code == 0x08) {
        n = 20;
        for (i = 0; i < n; i++)
            buf[i] = 0;
        buf[0] = 0x08;
        buf[1] = 0x12;
        buf[2] = 0x01;      /* RCD: no read cache, WCE (bit 2) 0 */
        return n;
    }
    if (code == 0x1c) {
        n = 12;
        for (i = 0; i < n; i++)
            buf[i] = 0;
        buf[0] = 0x1c;
        buf[1] = 0x0a;
        buf[2] = 0x08;      /* DEXCPT: no informational exceptions */
        return n;
    }
    return 0;
}

/* the INQUIRY vital product data page `code` into buf (up to 8 bytes):
 * its length, or 0 when it is not supported: only the list of pages */
static inline unsigned ubot_vpd_page(unsigned code, uint8_t *buf)
{
    if (code != 0x00)
        return 0;
    buf[0] = 0x00;          /* direct access, connected */
    buf[1] = 0x00;          /* this page: supported pages */
    buf[2] = 0x00;
    buf[3] = 0x01;          /* one page listed */
    buf[4] = 0x00;
    return 5;
}

#endif /* USB_STORAGE_BOT_H */
