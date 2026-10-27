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
 * Host test for the USB mass storage Bulk-Only Transport decisions (USB
 * reliability, firmware/usbstack/usb_storage_bot.h). A model host, a model
 * endpoint layer (ARC-like: one prime per pipe, a second prime replaces
 * the first) and a model of usb_storage.c's command states, driven by the
 * real ubot_* functions, with faults injected at random: storage write
 * failures mid-WRITE, Bulk-Only resets at any point, stale IN completions
 * after a reset, short data phases.
 *
 * What would break, and how it shows:
 * - a CSW sent while the next data chunk's receive is still primed (the
 *   old failed-WRITE path) primes the CBW receive twice: "double prime";
 *   the host's next data then lands in the CBW buffer, and a data chunk
 *   that looks like a CBW runs as one: "misparsed CBW";
 * - a reset that leaves nothing primed: the host's next CBW is never
 *   taken: "deaf after reset";
 * - a chunk written after a failure, or to the wrong sector: "bad write";
 * - an IN completion taken as a command: "IN ran a CBW";
 * - a CSW whose status or residue is wrong: "bad CSW".
 *
 * The model of the states mirrors usb_storage.c by hand (the decisions are
 * the shared header's). The same run with the old rules (-l) must fail,
 * which shows the checks can see the bugs.
 *
 *   cc -I. tests/usb/bot_test.c -o bot_test && ./bot_test
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "firmware/usbstack/usb_storage_bot.h"

#define CHUNK     4         /* sectors per data transfer (WRITE_BUFFER) */
#define BS        512
#define MAX_CBW   512       /* the CBW receive's length */
#define NSECT     4096
#define NEVENTS   64

enum { W_CMD, W_CSW_OR_CMD, W_CSW, RECV, SEND };
enum { P_NONE, P_CBW, P_DATA, P_CSW };

/* what a buffer holds: a CBW (valid or not) or a data chunk's tag */
struct cbw {
    int sig;                /* 1: valid signature */
    int op;                 /* 'W', 'R', 'T' */
    int sector, count;
    unsigned tag;
};

static int legacy;          /* the old rules */
static unsigned long violations, errs_double, errs_misparse, errs_deaf,
       errs_write, errs_in, errs_csw;
static unsigned long n_cmds, n_resets, n_wfail, n_stale, n_short, n_done;

/* --- the device ------------------------------------------------------ */
static int state = W_CMD;
static struct cbw cbw_buf;
static bool held_ok;
static struct ubot_wr wr;
static int cur_sector, cur_count;
static unsigned cur_tag, cur_residue;
static int out_primed, in_primed;
static int fail_at = -1;            /* the storage write that fails */
static int writes_done;
static unsigned disk[NSECT];        /* the tag of the last write */

static struct { bool in; int status, length; } q[NEVENTS];
static int qn;

static void violation(unsigned long *which, const char *what)
{
    (*which)++;
    violations++;
    if (!legacy && violations <= 5)
        fprintf(stderr, "bot_test: %s\n", what);
}

static void post(bool in, int status, int length)
{
    if (qn < NEVENTS) {
        q[qn].in = in;
        q[qn].status = status;
        q[qn].length = length;
        qn++;
    }
}

static void prime_out(int what)
{
    if (out_primed != P_NONE)
        violation(&errs_double, "double prime");
    out_primed = what;              /* the newer prime wins */
}

static void prime_in(int what)
{
    in_primed = what;
}

/* the CSW's contents, for the host */
static int csw_status;
static unsigned csw_tag, csw_residue;

static void send_csw(int status)
{
    csw_status = status;
    csw_tag = cur_tag;
    csw_residue = cur_residue;
    prime_in(P_CSW);
    state = W_CSW_OR_CMD;
    prime_out(P_CBW);
}

static void handle_cbw(bool from_in)
{
    if (from_in)
        violation(&errs_in, "IN ran a CBW");
    if (!cbw_buf.sig) {
        out_primed = P_NONE;        /* stalled; reset recovery follows */
        return;
    }
    cbw_buf.sig = 0;
    ubot_wr_begin(&wr);
    cur_tag = cbw_buf.tag;
    cur_sector = cbw_buf.sector;
    cur_count = cbw_buf.count;
    cur_residue = cbw_buf.count * BS;
    if (cbw_buf.op == 'T') {
        cur_residue = 0;
        send_csw(0);
    }
    else if (cbw_buf.op == 'W') {
        state = RECV;
        prime_out(P_DATA);
    }
    else {
        state = SEND;
        prime_in(P_DATA);
    }
}

/* one storage write of the chunk that arrived for sectors [s, s+n) */
static int storage_write(int s, int n, unsigned tag)
{
    if (writes_done++ == fail_at)
        return -1;
    for (int i = 0; i < n; i++) {
        if (s + i >= NSECT) {
            violation(&errs_write, "bad write");
            return -1;
        }
        disk[s + i] = tag;
    }
    return 0;
}

static void complete(bool in, int status, int length)
{
    int chunk;

    switch (state) {
        case W_CMD:
        case W_CSW_OR_CMD:
        case W_CSW: {
            if (legacy) {
                if (state == W_CSW_OR_CMD)
                    state = in ? W_CMD : W_CSW;
                else {
                    state = W_CMD;
                    handle_cbw(in && state != W_CSW);
                }
                break;
            }
            enum ubot_wait w = state == W_CMD ? UBOT_WAIT_CMD :
                               state == W_CSW ? UBOT_WAIT_CSW :
                               UBOT_WAIT_CSW_OR_CMD;
            switch (ubot_between(w, in, status, length, held_ok)) {
                case UBOT_CSW_DONE:
                    state = W_CMD;
                    break;
                case UBOT_HOLD_CBW:
                    held_ok = ubot_cbw_valid(status, length);
                    state = W_CSW;
                    break;
                case UBOT_RUN_CBW:
                    state = W_CMD;
                    handle_cbw(w == UBOT_WAIT_CMD && in);
                    break;
                case UBOT_BAD_CBW:
                    state = W_CMD;
                    cbw_buf.sig = 0;
                    break;
                case UBOT_IGNORE:
                    break;
            }
            break;
        }
        case RECV:
            if (in && !legacy)
                break;
            chunk = (cur_count < CHUNK ? cur_count : CHUNK) * BS;
            if (status == 0 && length != chunk) {
                if (legacy)
                    break;          /* the old code: no CSW at all */
                ubot_wr_fail(&wr, cur_residue - length, length);
                cur_residue = ubot_wr_residue(&wr, cur_residue - length);
                send_csw(1);
                break;
            }
            cur_residue -= length;
            {
                int n = chunk / BS;
                int next_count = cur_count - n;
                if (next_count)
                    prime_out(P_DATA);
                if (legacy || ubot_wr_should_write(&wr)) {
                    if (storage_write(cur_sector, n, cur_tag)) {
                        n_wfail++;
                        if (legacy) {
                            send_csw(1);
                            break;
                        }
                        ubot_wr_fail(&wr, cur_residue, chunk);
                    }
                }
                else if (fail_at >= 0 && writes_done > fail_at) {
                    /* drained: nothing written */
                }
                if (!next_count) {
                    cur_residue = ubot_wr_residue(&wr, cur_residue);
                    send_csw(wr.failed ? 1 : 0);
                }
                cur_sector += n;
                cur_count = next_count;
            }
            break;
        case SEND:
            if (!in)
                break;
            chunk = cur_count < CHUNK ? cur_count : CHUNK;
            cur_count -= chunk;
            cur_residue -= chunk * BS;
            if (cur_count)
                prime_in(P_DATA);
            else
                send_csw(0);
            break;
    }
}

static void run_device(void)
{
    for (int i = 0; i < qn; i++)
        complete(q[i].in, q[i].status, q[i].length);
    qn = 0;
}

static void bulk_reset(void)
{
    n_resets++;
    if (legacy) {
        state = W_CMD;              /* WAITING_FOR_STORAGE not modelled */
        return;
    }
    out_primed = P_NONE;            /* the flush */
    in_primed = P_NONE;
    ubot_wr_begin(&wr);
    held_ok = false;
    state = W_CMD;
    cbw_buf.sig = 0;
    prime_out(P_CBW);
}

/* --- the host -------------------------------------------------------- */
static unsigned tag_seq = 1;

/* the host sends `c`; false: the device was not listening */
static bool host_send_cbw(const struct cbw *c)
{
    if (out_primed != P_CBW) {
        if (out_primed == P_DATA)
            return false;           /* lands in a data buffer: NAK model */
        return false;
    }
    out_primed = P_NONE;
    cbw_buf = *c;
    post(false, 0, UBOT_CBW_LEN);
    run_device();
    return true;
}

/* a data chunk; it carries a CBW-looking pattern for a WRITE to sector 0,
 * the worst case if it ever lands in the CBW buffer */
static bool host_send_chunk(int len)
{
    if (out_primed == P_DATA) {
        out_primed = P_NONE;
        post(false, 0, len);
        run_device();
        return true;
    }
    if (out_primed == P_CBW) {
        out_primed = P_NONE;
        cbw_buf.sig = 1;
        cbw_buf.op = 'W';
        cbw_buf.sector = 0;
        cbw_buf.count = 1;
        cbw_buf.tag = 0xbad;
        post(false, 0, len < MAX_CBW ? len : MAX_CBW);
        run_device();
        if (disk[0] == 0xbad || state == RECV)
            violation(&errs_misparse, "misparsed CBW");
        return true;
    }
    return false;
}

static bool host_read_csw(int *status, unsigned *tag, unsigned *residue)
{
    if (in_primed != P_CSW)
        return false;
    in_primed = P_NONE;
    *status = csw_status;
    *tag = csw_tag;
    *residue = csw_residue;
    post(true, 0, 13);
    run_device();
    return true;
}

static bool host_read_data(void)
{
    if (in_primed != P_DATA)
        return false;
    in_primed = P_NONE;
    post(true, 0, CHUNK * BS);
    run_device();
    return true;
}

static void host_recover(void)
{
    bulk_reset();
    if (!legacy && rand() % 3 == 0) {
        /* a late IN completion (a CSW or data that finished just before
         * the flush) reaches the thread after the reset */
        n_stale++;
        post(true, 0, rand() % 2 ? 13 : CHUNK * BS);
        run_device();
    }
}

static void one_command(void)
{
    struct cbw c;
    int r = rand() % 10;
    int reset_at = rand() % 6 == 0 ? rand() % 8 : -1;
    int step = 0, chunk_i = 0, short_at = -1;
    int status;
    unsigned tag, residue;

    memset(&c, 0, sizeof(c));
    c.sig = 1;
    c.tag = tag_seq++;
    c.op = r < 5 ? 'W' : r < 8 ? 'R' : 'T';
    c.count = c.op == 'T' ? 0 : 1 + rand() % (3 * CHUNK);
    c.sector = rand() % (NSECT - c.count);
    fail_at = -1;
    writes_done = 0;
    if (c.op == 'W' && rand() % 4 == 0) {
        fail_at = rand() % ((c.count + CHUNK - 1) / CHUNK);
    }
    n_cmds++;

    if (!host_send_cbw(&c)) {
        violation(&errs_deaf, "deaf after reset");
        host_recover();
        return;
    }
    if (c.op == 'W') {
        int left = c.count;
        while (left > 0) {
            int n = left < CHUNK ? left : CHUNK;
            int len = n * BS;
            if (step++ == reset_at) {
                host_recover();
                return;
            }
            if (rand() % 50 == 0) {
                len -= BS / 2;      /* the host ends the phase short */
                n_short++;
                short_at = chunk_i;
                n = left;
            }
            chunk_i++;
            if (!host_send_chunk(len)) {
                host_recover();     /* host timeout */
                return;
            }
            left -= n;
        }
    }
    else if (c.op == 'R') {
        int left = c.count;
        while (left > 0) {
            if (step++ == reset_at) {
                host_recover();
                return;
            }
            if (!host_read_data()) {
                host_recover();
                return;
            }
            left -= left < CHUNK ? left : CHUNK;
        }
    }
    if (step == reset_at) {
        host_recover();
        return;
    }
    if (!host_read_csw(&status, &tag, &residue)) {
        host_recover();
        return;
    }
    if (tag != c.tag) {
        violation(&errs_csw, "bad CSW (tag)");
        return;
    }
    if (c.op == 'W') {
        /* the sectors written are exactly those before the failure */
        int ok_chunks = fail_at < 0 ? 1 << 30 : fail_at;
        if (short_at >= 0 && short_at < ok_chunks)
            ok_chunks = short_at;
        for (int i = 0; i < c.count; i++) {
            bool want = i / CHUNK < ok_chunks;
            if ((disk[c.sector + i] == c.tag) != want) {
                violation(&errs_write, "bad write");
                break;
            }
        }
        if (ok_chunks < (1 << 30) && (status != 1 ||
            residue != (unsigned)(c.count - ok_chunks * CHUNK) * BS))
            violation(&errs_csw, "bad CSW (failed WRITE)");
        if (ok_chunks == (1 << 30) && (status != 0 || residue != 0))
            violation(&errs_csw, "bad CSW (WRITE)");
    }
    n_done++;
}

static unsigned long run(int is_legacy, unsigned seed, int cmds)
{
    legacy = is_legacy;
    srand(seed);
    violations = 0;
    state = W_CMD;
    memset(disk, 0, sizeof(disk));
    out_primed = P_NONE;
    in_primed = P_NONE;
    prime_out(P_CBW);               /* SET_CONFIGURATION */
    for (int i = 0; i < cmds; i++)
        one_command();
    return violations;
}

/* the shared decisions for the failing and no-data paths (usb_storage.c's
 * csw_no_data(), the zero-length READ/WRITE, the handover wait, the mode
 * and VPD pages): stall-then-CSW on error, none without a data phase */
#define EXPECT(c, what) \
    do { if (!(c)) { printf("FAIL: %s\n", what); bad++; } } while (0)

static int check_decisions(void)
{
    int bad = 0;
    uint8_t b[32];

    EXPECT(ubot_err_stall(0, true) == UBOT_STALL_NONE, "no data: no stall in");
    EXPECT(ubot_err_stall(0, false) == UBOT_STALL_NONE, "no data: no stall out");
    EXPECT(ubot_err_stall(512, true) == UBOT_STALL_IN, "failed READ stalls IN");
    EXPECT(ubot_err_stall(65536, false) == UBOT_STALL_OUT,
           "failed WRITE stalls OUT");
    EXPECT(ubot_zero_len(0) && !ubot_zero_len(1), "zero-length READ/WRITE");
    EXPECT(ubot_clamp(36, 255, 36) == 36, "clamp: whole reply");
    EXPECT(ubot_clamp(36, 5, 36) == 5, "clamp: allocation length");
    EXPECT(ubot_clamp(4, 255, 36) == 4, "clamp: dCBWDataTransferLength");
    EXPECT(ubot_clamp(0, 255, 36) == 0, "clamp: no data");
    EXPECT(ubot_early_cmd(0x00) == UBOT_EARLY_TUR, "early TUR");
    EXPECT(ubot_early_cmd(0x03) == UBOT_EARLY_SENSE, "early REQUEST SENSE");
    EXPECT(ubot_early_cmd(0x28) == UBOT_EARLY_HOLD, "READ held");
    EXPECT(ubot_early_cmd(0x12) == UBOT_EARLY_HOLD, "INQUIRY held");
    memset(b, 0xaa, sizeof(b));
    EXPECT(ubot_mode_page(0x08, b) == 20 && b[0] == 0x08 && b[1] == 0x12 &&
           !(b[2] & 0x04), "caching page, WCE 0");
    EXPECT(ubot_mode_page(0x1c, b) == 12 && b[0] == 0x1c && b[1] == 0x0a,
           "informational exceptions page");
    EXPECT(ubot_mode_page(0x3f, b) == 0 && ubot_mode_page(0x05, b) == 0,
           "other pages unsupported");
    EXPECT(ubot_vpd_page(0x00, b) == 5 && b[3] == 1 && b[4] == 0,
           "VPD page 0 lists itself");
    EXPECT(ubot_vpd_page(0x80, b) == 0 && ubot_vpd_page(0x83, b) == 0,
           "other VPD pages fail");
    return bad;
}

int main(int argc, char **argv)
{
    unsigned long v, lv;

    if (check_decisions()) {
        printf("bot_test: FAIL (decisions)\n");
        return 1;
    }

    (void)argv;
    v = run(0, 1234, 200000);
    printf("bot_test: %lu commands, %lu completed, %lu resets, %lu failed "
           "writes, %lu stale INs, %lu short phases: %lu violations\n",
           n_cmds, n_done, n_resets, n_wfail, n_stale, n_short, v);
    if (v)
        printf("  double=%lu misparse=%lu deaf=%lu write=%lu in=%lu "
               "csw=%lu\n", errs_double, errs_misparse, errs_deaf,
               errs_write, errs_in, errs_csw);
    errs_double = errs_misparse = errs_deaf = errs_write = errs_in =
        errs_csw = 0;
    lv = run(1, 1234, 20000);
    printf("bot_test: the old rules: %lu violations (double=%lu "
           "misparse=%lu deaf=%lu csw=%lu)\n", lv, errs_double,
           errs_misparse, errs_deaf, errs_csw);
    if (argc > 1)
        return 0;
    if (v) {
        printf("bot_test: FAIL\n");
        return 1;
    }
    if (!lv || !errs_double || !errs_deaf) {
        printf("bot_test: FAIL (the old rules passed: the checks see "
               "nothing)\n");
        return 1;
    }
    printf("bot_test: ok\n");
    return 0;
}
