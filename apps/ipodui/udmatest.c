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
 * iPod UI shell: the UDMA4 experiment on the flash adapter (Fit-1 C7,
 * docs/fit1-brief.md section A). See udmatest.h.
 *
 * The adapter lists UDMA0-5 in IDENTIFY word 88 but leaves words 76 and
 * 93 at 0, so the cable check caps it at UDMA2 (storage.md section 6).
 * Forcing UDMA4 may be faster or may corrupt transfers, and this is the
 * disk with the owner's library, so every safety is on the driver side
 * (firmware/drivers/ata.c, ata-pp5020.c) and here:
 *  - off by default; on only while /.rockbox/udma4.cfg exists, which only
 *    the developer row writes;
 *  - while forced, writes go by PIO: a bad UDMA4 transfer can only spoil
 *    a read, never what lands on the flash;
 *  - the first command that needs a retry drops the force (the retry runs
 *    at UDMA2); udma_service() then removes udma4.cfg, so the next boot
 *    stays at UDMA2, and logs it;
 *  - the test writes only its own scratch file in the system folder
 *    (UDMA_TEST_FILE), with playback stopped, and deletes it; a mismatch
 *    turns the switch off as a retry would.
 * Log lines (ipodui.log): "udma boot force=4", "udma set on|off",
 * "udma fell back resets=R", "udma test ...".
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
#include "config.h"

#ifdef HAVE_ATA_UDMA_FORCE
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "string-extra.h"
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "ata.h"
#include "audio.h"
#include "core_alloc.h"
#include "devstats.h"
#include "ipodui/uilog.h"
#include "ipodui/udmatest.h"

#define UDMA_CFG            ROCKBOX_DIR "/udma4.cfg"
#define UDMA_TEST_FILE      ROCKBOX_DIR "/udmatest.bin"
#define UDMA_MODE           4
#define UDMA_CHUNK          (256 * 1024)        /* one storage command */
#define UDMA_TEST_MB        64                  /* past any adapter cache */
#define UDMA_TEST_PASSES    8
#define UDMA_CHUNKS         (UDMA_TEST_MB * 1024 * 1024 / UDMA_CHUNK)

static char result[48] = "UDMA4 test: not run";

static void cfg_write(bool on)
{
    if (on)
    {
        int fd = open(UDMA_CFG, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd >= 0)
        {
            write(fd, "4\n", 2);
            close(fd);
        }
    }
    else
        remove(UDMA_CFG);
}

void udma_boot(void)
{
    if (file_exists(UDMA_CFG))
    {
        ata_set_udma_force(UDMA_MODE);
        ui_log("udma", "boot force=%d", UDMA_MODE);
    }
}

void udma_service(void)
{
    if (!ata_udma_fell_back())
        return;
    ata_udma_clear_fell_back();
    cfg_write(false);               /* the next boot stays at UDMA2 */
    ui_log("udma", "fell back resets=%u", (unsigned)devstats.resets);
}

bool udma_is_on(void)
{
    return ata_udma_force() != 0;
}

void udma_set(bool on)
{
    cfg_write(on);
    ata_set_udma_force(on ? UDMA_MODE : 0);
    ui_log("udma", "set %s now=%d", on ? "on" : "off",
           (int)ata_udma_forced_now());
}

/* The pattern: word w of the file holds w * K + C (K odd), unique for
 * every offset in the file, so a shifted, repeated or stale block never
 * matches; one add per word keeps the check cheap */
#define PAT_K   0x9e3779b1u
#define PAT_C   0x7f4a7c15u

static void fill(uint32_t *b, uint32_t first)
{
    uint32_t i, v = first * PAT_K + PAT_C;
    for (i = 0; i < UDMA_CHUNK / 4; i++, v += PAT_K)
        b[i] = v;
}

static uint32_t check(const uint32_t *b, uint32_t first)
{
    uint32_t i, bad = 0, v = first * PAT_K + PAT_C;
    for (i = 0; i < UDMA_CHUNK / 4; i++, v += PAT_K)
        bad += b[i] != v;
    return bad;
}

/* One read pass over the file: bad words, or -1 on an I/O error. *ticks
 * gets the time spent in read() only. */
static long read_pass(uint32_t *b, long *ticks)
{
    long bad = 0, t = 0;
    int c, fd = open(UDMA_TEST_FILE, O_RDONLY);

    if (fd < 0)
        return -1;
    for (c = 0; c < UDMA_CHUNKS; c++)
    {
        long t0 = current_tick;
        ssize_t n = read(fd, b, UDMA_CHUNK);
        t += current_tick - t0;
        if (n != UDMA_CHUNK)
        {
            bad = -1;
            break;
        }
        bad += check(b, (uint32_t)c * (UDMA_CHUNK / 4));
    }
    close(fd);
    *ticks = t;
    return bad;
}

static int mbps(long ticks, int passes)
{
    return ticks > 0 ? (int)((long)UDMA_TEST_MB * passes * HZ / ticks) : 0;
}

int udma_test(void)
{
    int h, fd, c, p, rc = 0, was = ata_udma_force();
    long bad = 0, t2 = 0, t4 = 0, t;
    unsigned resets0 = devstats.resets;
    uint32_t *b;

    if (audio_status())
    {
        strlcpy(result, "UDMA4 test: stop playback first", sizeof(result));
        return -1;
    }
    h = core_alloc(UDMA_CHUNK + 16);
    if (h <= 0)
    {
        strlcpy(result, "UDMA4 test: no memory", sizeof(result));
        return -1;
    }
    core_pin(h);
    b = (uint32_t *)CACHEALIGN_UP((uintptr_t)core_get_data(h));
    cpu_boost(true);                /* the compare; read() time is timed */
    ui_log("udma", "test start mb=%d passes=%d", UDMA_TEST_MB,
           UDMA_TEST_PASSES);

    /* 1. write at the negotiated mode (DMA writes, as normal) */
    ata_set_udma_force(0);
    fd = open(UDMA_TEST_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    for (c = 0; fd >= 0 && c < UDMA_CHUNKS; c++)
    {
        fill(b, (uint32_t)c * (UDMA_CHUNK / 4));
        if (write(fd, b, UDMA_CHUNK) != UDMA_CHUNK)
            break;
    }
    if (fd >= 0)
        close(fd);
    if (fd < 0 || c < UDMA_CHUNKS)
    {
        rc = -2;
        snprintf(result, sizeof(result), "UDMA4 test: write failed");
        goto out;
    }
    /* 2. one pass at UDMA2: the write is good, the speed before */
    bad = read_pass(b, &t2);
    if (bad)
    {
        rc = -3;
        snprintf(result, sizeof(result), "UDMA4 test: UDMA2 read bad=%ld",
                 bad);
        goto out;
    }
    /* 3. the passes at UDMA4 */
    ata_set_udma_force(UDMA_MODE);
    if (!ata_udma_forced_now())
    {
        /* the disk may sleep between the steps: a read wakes it with
         * the force applied */
        read_pass(b, &t);
    }
    for (p = 0; p < UDMA_TEST_PASSES; p++)
    {
        bad = read_pass(b, &t);
        t4 += t;
        if (bad || ata_udma_fell_back() || !ata_udma_forced_now())
            break;
    }
    if (bad || p < UDMA_TEST_PASSES)
    {
        rc = -4;
        ata_set_udma_force(0);
        was = 0;                    /* a failure turns the switch off */
        cfg_write(false);
        snprintf(result, sizeof(result), "UDMA4 test: FAIL pass %d bad=%ld",
                 p + 1, bad);
        goto out;
    }
    snprintf(result, sizeof(result), "UDMA4 test: pass, %d -> %d MB/s",
             mbps(t2, 1), mbps(t4, UDMA_TEST_PASSES));
out:
    cpu_boost(false);
    remove(UDMA_TEST_FILE);
    core_unpin(h);
    core_free(h);
    ata_set_udma_force(was);
    ui_log("udma", "test rc=%d bad=%ld mbs2=%d mbs4=%d resets=%u", rc, bad,
           mbps(t2, 1), mbps(t4, UDMA_TEST_PASSES),
           (unsigned)(devstats.resets - resets0));
    ui_log_flush();
    return rc;
}

const char *udma_test_result(void)
{
    return result;
}
#endif /* HAVE_ATA_UDMA_FORCE */
