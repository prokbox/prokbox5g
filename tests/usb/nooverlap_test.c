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
 * Host test for send_and_read_next() in firmware/usbstack/usb_storage.c,
 * the READ data phase over two 64 KB halves (budget review M5, #81).
 *
 * run.sh cuts the real function out of usb_storage.c (build/xlate/
 * sarn.inc) and this file compiles it against a model disk, so the test
 * follows the source without changing the device's object code. Built
 * twice: with USB_STORAGE_NO_READ_OVERLAP (the iPod Video's serial path)
 * and without it (upstream's overlapped path).
 *
 * What would break, and how it shows:
 * - the deferred half sent before it is read (the first serial build's
 *   bug: stale 64 KB halves, alternating file corruption): a sent byte
 *   differs from the disk;
 * - a read into the half that is still on the wire: "read into the half
 *   on the wire";
 * - with USB_STORAGE_NO_READ_OVERLAP, any read started while a bulk-IN is
 *   in flight (the ATA and USB DMA overlap the flag exists to prevent):
 *   "read during bulk-IN";
 * - a wrong last chunk: the byte count sent differs from the request.
 *
 *   cc -Wall -I. -Ibuild/xlate [-DUSB_STORAGE_NO_READ_OVERLAP] \
 *       tests/usb/nooverlap_test.c -o nooverlap_test
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
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define BS                  512
#define READ_BUFFER_SIZE    (64 * 1024)
#define NSECT               4096
#define MIN(a, b)           ((a) < (b) ? (a) : (b))
#define IF_MD(...)
#define USBSTOR_READ_SECTORS_FILTER() ({ 0; })

typedef unsigned long sector_t;

/* the fields send_and_read_next() uses, as in usb_storage.c */
static struct {
    sector_t sector;
    unsigned int count;
    unsigned int block_size;
    unsigned int lun;
    unsigned char *data[2];
    unsigned char data_select;
    bool read_pending;
    unsigned int last_result;
} cur_cmd;

static unsigned char halves[2 * READ_BUFFER_SIZE];
static unsigned char *on_wire;          /* the half a bulk-IN is sending */
static sector_t next_sent;              /* the next sector the host expects */
static unsigned long sent_bytes, reads, fails;

static unsigned char pattern(sector_t s, unsigned int i)
{
    return (unsigned char)(s * 7 + i * 13 + (s >> 3));
}

static int storage_read_sectors(sector_t start, int n, void *buf)
{
    unsigned char *p = buf;

    reads++;
    if (on_wire && p == on_wire)
    {
        printf("FAIL read into the half on the wire (sector %lu)\n", start);
        fails++;
    }
#ifdef USB_STORAGE_NO_READ_OVERLAP
    if (on_wire)
    {
        printf("FAIL read during bulk-IN (sector %lu)\n", start);
        fails++;
    }
#endif
    for (int k = 0; k < n; k++)
        for (unsigned int i = 0; i < BS; i++)
            p[k * BS + i] = pattern(start + k, i);
    return 0;
}

static void send_block_data(void *data, unsigned int size)
{
    unsigned char *p = data;

    for (unsigned int b = 0; b < size; b++)
    {
        sector_t s = next_sent + b / BS;
        if (p[b] != pattern(s, b % BS))
        {
            printf("FAIL sector %lu byte %u: sent %02x, disk %02x\n", s,
                   b % BS, p[b], pattern(s, b % BS));
            fails++;
            break;
        }
    }
    next_sent += size / BS;
    sent_bytes += size;
    on_wire = p;
}

static void send_and_read_next(void);
#include "sarn.inc"

/* One SCSI READ the way handle_scsi_ready() and the transfer completion
 * drive it: the first half read, then send_and_read_next() at the start
 * and after every bulk-IN completion until the count is spent. */
static void scsi_read(sector_t start, unsigned int count)
{
    unsigned long r0 = reads;

    memset(halves, 0xa5, sizeof(halves));   /* stale: never on the wire */
    cur_cmd.data[0] = halves;
    cur_cmd.data[1] = halves + READ_BUFFER_SIZE;
    cur_cmd.data_select = 0;
    cur_cmd.read_pending = false;
    cur_cmd.sector = start;
    cur_cmd.count = count;
    cur_cmd.block_size = BS;
    cur_cmd.last_result = 0;
    next_sent = start;
    sent_bytes = 0;
    on_wire = NULL;

    cur_cmd.last_result = storage_read_sectors(cur_cmd.sector,
        MIN(READ_BUFFER_SIZE / cur_cmd.block_size, cur_cmd.count),
        cur_cmd.data[cur_cmd.data_select]);
    send_and_read_next();
    for (;;)
    {
        on_wire = NULL;                 /* the bulk-IN completed */
        if (cur_cmd.count == 0)
            break;
        send_and_read_next();
    }
    if (sent_bytes != (unsigned long)count * BS)
    {
        printf("FAIL READ %lu+%u: sent %lu bytes, want %u\n", start, count,
               sent_bytes, count * BS);
        fails++;
    }
    if (cur_cmd.last_result)
    {
        printf("FAIL READ %lu+%u: last_result %u\n", start, count,
               cur_cmd.last_result);
        fails++;
    }
    if (reads - r0 != (count + 127) / 128)
    {
        printf("FAIL READ %lu+%u: %lu reads, want %u\n", start, count,
               reads - r0, (count + 127) / 128);
        fails++;
    }
}

int main(void)
{
    static const unsigned int counts[] = { 1, 8, 127, 128, 129, 255, 256,
                                           257, 384, 500, 1024 };

    for (unsigned int i = 0; i < sizeof(counts) / sizeof(counts[0]); i++)
    {
        scsi_read(0, counts[i]);
        scsi_read(NSECT - counts[i], counts[i]);
        scsi_read(333, counts[i]);
    }
    printf("nooverlap_test (%s): %s\n",
#ifdef USB_STORAGE_NO_READ_OVERLAP
           "no overlap",
#else
           "overlap",
#endif
           fails ? "FAILED" : "ok");
    return fails != 0;
}
