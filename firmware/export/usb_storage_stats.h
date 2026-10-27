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
 * USB mass storage counters (ProkBox #63): where a USB session's time went,
 * for one line in the device log. Reset when the host configures the
 * device; kept after the cable is pulled until the next session.
 *
 * Times are microseconds of the USB thread's view:
 *   busy   handling transfer completions (includes storage)
 *   stor   inside storage_read_sectors / storage_write_sectors
 *   usb    waiting for a data phase (block send/receive) to complete
 *   host   waiting for the host: CSW out, next command in
 * busy + usb + host is roughly the session's active time; idle gaps
 * between bursts count as host.
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
#ifndef USB_STORAGE_STATS_H
#define USB_STORAGE_STATS_H

#include <stdbool.h>
#include <stdint.h>

#define USB_STATS_MOUNT_READS 4

struct usb_storage_stats {
    uint64_t rd_bytes;      /* data sent to the host */
    uint64_t wr_bytes;      /* data received from the host */
    uint64_t busy_us;
    uint64_t stor_us;
    uint64_t usb_us;
    uint64_t host_us;
    uint32_t rd_cmds;       /* SCSI READ(10/16) */
    uint32_t wr_cmds;       /* SCSI WRITE(10/16) */
    uint32_t stor_calls;    /* storage_read/write_sectors calls */
    uint32_t ra_hits;       /* READs served (at least in part) from read-ahead */
    uint32_t ra_reads;      /* read-ahead fills */
    /* #81 prefetch cache (HAVE_USB_PREFETCH), else 0. stor_us also counts
     * the worker's reads, which overlap the USB thread's busy time. */
    uint32_t pf_ring_kb;    /* ring size; 0: none (the read-ahead runs) */
    uint32_t pf_hits;       /* READs whose data was all in the ring */
    uint32_t pf_pend;       /* READs the stream was still reading */
    uint32_t pf_miss;       /* READs on the ordinary path */
    uint32_t pf_fallback;   /* pieces read the ordinary way after a wait */
    uint32_t pf_fills;      /* the worker's reads that landed */
    uint32_t pf_kb;         /* KB they brought */
    uint32_t pf_stall_max_us;   /* longest wait for one piece */
    uint64_t pf_stall_us;       /* all such waits */
    uint64_t wr_stor_us;    /* the storage time of writes (part of stor) */
    /* native mount timing: from SET_CONFIGURATION to the first TUR,
     * INQUIRY and READ; the first INQUIRY data completion after it; from
     * the first READ to the first READ past the first FAT (0: not seen),
     * and the KB sent in the first 30 s after the first READ */
    uint32_t cfg_to_tur_ms;
    uint32_t cfg_to_inq_ms;
    uint32_t inq_to_data_ms;
    uint32_t cfg_to_rd_ms;
    uint32_t rd_to_fat_ms;
    uint32_t rd30_kb;
    /* One snapshot near FSKit's activation deadline. It is captured on the
     * first READ command at or after 18 seconds from the first READ. */
    uint32_t rd18_ms;
    uint32_t rd18_kb;
    uint32_t mount_read18_n;
    uint32_t mount_read18_lba;
    uint32_t mount_read18_blocks;
    /* First and final READs seen during native volume activation. These are
     * diagnostic snapshots only; LBA/count are host-facing 512-byte units
     * on ProkBox. */
    uint32_t mount_read_n;
    uint32_t mount_first_lba[USB_STATS_MOUNT_READS];
    uint32_t mount_first_blocks[USB_STATS_MOUNT_READS];
    uint32_t mount_last_lba[USB_STATS_MOUNT_READS];
    uint32_t mount_last_blocks[USB_STATS_MOUNT_READS];
    /* ATA commands during the session (HAVE_ATA_XFER_STATS), else 0 */
    uint32_t dma_rd, dma_wr, pio_rd, pio_wr, ata_resets;
    int high_speed;         /* 1: USB 2.0 high speed at last configuration */
    uint32_t cfg_count;     /* storage configurations in this cable session */
    int high_speed_first;   /* link speed at the first configuration */
    int high_speed_changes; /* later configurations which changed speed */
    /* USB reliability: every other CBW by class (READ/WRITE are above),
     * the resets with the storage state at each, and the longest stalls */
    uint32_t tur_cmds;      /* TEST UNIT READY */
    uint32_t inq_cmds;      /* INQUIRY */
    uint32_t other_cmds;    /* anything else */
    uint32_t bulk_resets;   /* Bulk-Only Mass Storage Resets */
    uint32_t cfg_downs;     /* configuration torn down: bus reset,
                               SET_CONFIGURATION, or the end of USB */
    uint32_t bad_cbws;      /* invalid CBWs (both pipes halted) */
    uint32_t ignored;       /* stale completions ignored between commands */
    uint32_t wr_fails;      /* WRITEs whose storage write failed */
    uint32_t busy_max_us;   /* longest single pass of the USB thread
                               (pf_wait and storage reads included) */
    uint32_t stor_max_us;   /* longest single storage call, any thread */
    /* #81 per-command latency: storage reads of <= 16 KiB (the FAT scan's
     * size) and larger ones, counted and timed apart, so that the fixed
     * cost of one ATA command shows; and the CPU clock seen during reads */
    uint32_t rd_small_n, rd_big_n, rd_big_kb;
    uint64_t rd_small_us, rd_big_us;
    uint16_t rd_mhz_min, rd_mhz_max;
    /* the last USB_STATS_RST_LOG resets, oldest first from rst_n:
     * USB_STATS_RST_* << 4 | the storage state (usb_storage.c's enum) */
    uint32_t rst_n;         /* resets logged in all */
    uint8_t rst_log[8];
};
#define USB_STATS_RST_LOG   8
#define USB_STATS_RST_BULK  1   /* Bulk-Only Mass Storage Reset */
#define USB_STATS_RST_DOWN  2   /* configuration torn down */

/* Copies the counters of the current or last session. */
void usb_storage_get_stats(struct usb_storage_stats *out);
/* zero the counters: once per USB session, by whoever shows it */
void usb_storage_reset_stats(void);

/* The iPod Video defaults to the compatible, page-aligned INQUIRY backing
 * object. This Developer control remains a volatile core/static A/B switch. */
void usb_storage_set_inquiry_static(bool enabled);
bool usb_storage_inquiry_static(void);

/* Volatile, Developer-menu A/B control, read when the next USB session
 * starts. The iPod Video default is demand batching; never a worker. */
enum usb_pf_mode {
    USB_PF_BATCH,       /* the ring, filled in line between pieces */
    USB_PF_OFF,         /* no ring: one storage read per host READ */
    USB_PF_WORKER,      /* the overlapped worker: a test only (fault A) */
    USB_PF_MODES
};
void usb_storage_set_prefetch_mode(enum usb_pf_mode mode);
enum usb_pf_mode usb_storage_prefetch_mode(void);

#endif /* USB_STORAGE_STATS_H */
