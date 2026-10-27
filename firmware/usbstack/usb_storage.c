/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2007 by Björn Stenberg
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
#include "string.h"
#include "system.h"
#include "usb_core.h"
#include "usb_drv.h"
#include "usb_class_driver.h"
/*#define LOGF_ENABLE*/
#include "logf.h"
#include "storage.h"
#include "disk.h"
#include "fs_defines.h"
/* Needed to get at the audio buffer */
#include "audio.h"
#include "usb_storage.h"
#if CONFIG_RTC
#include "timefuncs.h"
#endif
#include "core_alloc.h"
#include "panic.h"
#include "usb_storage_bot.h"
#if defined(HAVE_IPOD_UI) && !defined(BOOTLOADER)
#include "usb_buf_alloc.h"
#endif
#include <stdio.h>
#if defined(HAVE_USB_STATS) && !defined(BOOTLOADER)
#include "file.h"
#include "rbpaths.h"
#endif

#ifdef USB_USE_RAMDISK
#define RAMDISK_SIZE 2048
#endif

#ifdef HAVE_USB_SECTOR_XLATE
/* ProkBox (#63): a 2048-byte virtual-sector disk shown as a 512-byte one;
 * the layout sectors are rewritten on the way through */
#include "usb_storage_xlate.h"
static struct xlate_state xlate[NUM_DRIVES];
static bool xlate_learned[NUM_DRIVES];
static void xlate_learn(int lun);
#define USBSTOR_READ_SECTORS_FILTER() ({ \
    xlate_read_run(&xlate[cur_cmd.lun], cur_cmd.sector, \
        MIN(READ_BUFFER_SIZE/cur_cmd.block_size, cur_cmd.count), \
        cur_cmd.data[cur_cmd.data_select]); 0; })
#define USBSTOR_WRITE_SECTORS_FILTER() \
    xlate_write_run(&xlate[cur_cmd.lun], cur_cmd.sector, \
        MIN(WRITE_BUFFER_SIZE/cur_cmd.block_size, cur_cmd.count), \
        cur_cmd.data[cur_cmd.data_select])
/* #81: the prefetch ring holds the host's view, rewritten when it lands */
#define USBSTOR_PF_FILTER(lun, start, n, buf) \
    xlate_read_run(&xlate[lun], start, n, buf)
#endif
#ifndef USBSTOR_PF_FILTER
#define USBSTOR_PF_FILTER(lun, start, n, buf) ((void)0)
#endif

/* These defaults allow the operation */
#ifndef USBSTOR_READ_SECTORS_FILTER
#define USBSTOR_READ_SECTORS_FILTER() ({ 0; })
#endif

#ifndef USBSTOR_WRITE_SECTORS_FILTER
#define USBSTOR_WRITE_SECTORS_FILTER() ({ 0; })
#endif

#ifdef HAVE_USB_STATS
/* ProkBox (#63): where a session's time goes, for the device log
 * (usb_storage_stats.h). Every storage call below is timed. */
#include "usb_storage_stats.h"
#endif
#if (CONFIG_STORAGE & STORAGE_ATA)
#include "ata.h"
#endif
#ifdef HAVE_USB_STATS
#if defined(HAVE_ATA_XFER_STATS)
static struct ata_xfer_stats ata_base;
#endif
static struct usb_storage_stats ustats;
static unsigned long ustats_wait_since;  /* 0: not waiting */
/* #81: CBWs held for the storage handover (WAITING_FOR_STORAGE) */
static unsigned int hold_n;
static long hold_since, hold_max;         /* ticks; hold_since 0: none */
static bool ustats_wait_usb;             /* waiting for a data phase */

#ifdef USEC_TIMER
#define USTATS_NOW() ((unsigned long)USEC_TIMER | 1)
#else
#define USTATS_NOW() ((unsigned long)current_tick * (1000000 / HZ) | 1)
#endif

static int ustats_read(IF_MD(int drive,) sector_t start, int count, void *buf)
{
    static unsigned int bs;         /* the storage's sector size */
    unsigned long t0 = USTATS_NOW();
    int rc = storage_read_sectors(IF_MD(drive,) start, count, buf);
    unsigned long dt = USTATS_NOW() - t0;
    unsigned int mhz = cpu_frequency / 1000000;
    ustats.stor_us += dt;
    ustats.stor_calls++;
    if (dt > ustats.stor_max_us)
        ustats.stor_max_us = dt;
    if (!bs) {
        struct storage_info info;
#ifdef HAVE_MULTIDRIVE
        storage_get_info(drive, &info);
#else
        storage_get_info(0, &info);
#endif
        bs = info.sector_size ? info.sector_size : 512;
    }
    if ((unsigned long)count * bs <= 16384) {
        ustats.rd_small_n++;
        ustats.rd_small_us += dt;
    } else {
        ustats.rd_big_n++;
        ustats.rd_big_us += dt;
        ustats.rd_big_kb += (unsigned long)count * bs / 1024;
    }
    if (!ustats.rd_mhz_min || mhz < ustats.rd_mhz_min)
        ustats.rd_mhz_min = mhz;
    if (mhz > ustats.rd_mhz_max)
        ustats.rd_mhz_max = mhz;
    return rc;
}

static int ustats_write(IF_MD(int drive,) sector_t start, int count,
                        const void *buf)
{
    unsigned long t0 = USTATS_NOW();
    int rc = storage_write_sectors(IF_MD(drive,) start, count, buf);
    unsigned long dt = USTATS_NOW() - t0;
    ustats.stor_us += dt;
    ustats.wr_stor_us += dt;
    ustats.stor_calls++;
    if (dt > ustats.stor_max_us)
        ustats.stor_max_us = dt;
    return rc;
}

/* #81: native-mount timing (usb_storage_stats.h). The first standard SCSI
 * probes identify a controller/BOT failure before the host reaches FAT I/O;
 * an INQUIRY whose reply was primed but never retired leaves inq_to_data_ms
 * zero. */
static unsigned long ustats_cfg_at;     /* SET_CONFIGURATION; 0: unknown */
static unsigned long ustats_inq_at;     /* first INQUIRY CBW; 0: none yet */
static unsigned long ustats_rd_at;      /* the first READ; 0: none yet */
static sector_t ustats_fat_end;         /* first FAT's end; 0: unknown */
static uint64_t ustats_rd30;            /* bytes sent in the first 30 s */

static void ustats_read_cmd(sector_t sector, unsigned int count)
{
    unsigned long now = USTATS_NOW();
    unsigned int n = ustats.mount_read_n++;

    if (!ustats_rd_at) {
        ustats_rd_at = now;
        if (ustats_cfg_at && !ustats.cfg_to_rd_ms)
            ustats.cfg_to_rd_ms = (now - ustats_cfg_at) / 1000 | 1;
    }
    if (ustats_fat_end && !ustats.rd_to_fat_ms && sector >= ustats_fat_end)
        ustats.rd_to_fat_ms = (now - ustats_rd_at) / 1000 | 1;

    /* Keep a pre-timeout snapshot distinct from later manual-mount traffic.
     * Disk Arbitration gives FSKit roughly 20 s to activate the volume. */
    if (!ustats.rd18_ms && now - ustats_rd_at >= 18000000UL) {
        ustats.rd18_ms = (now - ustats_rd_at) / 1000 | 1;
        ustats.rd18_kb = ustats.rd_bytes >> 10;
        ustats.mount_read18_n = n;
        ustats.mount_read18_lba = (uint32_t)sector;
        ustats.mount_read18_blocks = count;
    }

    /* FSKit has a 20 s native-mount activation deadline. Retain its first
     * four reads and the rolling final four in RAM: that distinguishes a
     * sequential FAT walk from a repeated/stalled request without putting
     * log traffic in the USB transfer path. */
    if (n < USB_STATS_MOUNT_READS)
    {
        ustats.mount_first_lba[n] = (uint32_t)sector;
        ustats.mount_first_blocks[n] = count;
    }
    ustats.mount_last_lba[n % USB_STATS_MOUNT_READS] = (uint32_t)sector;
    ustats.mount_last_blocks[n % USB_STATS_MOUNT_READS] = count;
}
#define storage_read_sectors ustats_read
#define storage_write_sectors ustats_write
#endif /* HAVE_USB_STATS */

#ifdef HAVE_USB_READ_AHEAD
/* ProkBox (#63): sequential READs are answered from the half of the double
 * buffer the USB is not sending from; see usb_storage_ra.h. The CSW and the
 * small replies then need their own space (USBSTOR_SMALL_SIZE), because
 * they were built at the start of the first half. */
#include "usb_storage_ra.h"
#define USBSTOR_SMALL_SIZE 1024
static unsigned char *blk_buffer;
static struct ura ra;
static sector_t ra_limit;
#define USBSTOR_BLK blk_buffer
#ifdef USB_STATIC_ALLOC
#error "HAVE_USB_READ_AHEAD expects the core_alloc'd buffer"
#endif
#else
#define USBSTOR_SMALL_SIZE 0
#define USBSTOR_BLK tb.transfer_buffer
#endif

#ifdef HAVE_USB_PREFETCH
/* ProkBox (#81): while USB owns the disk (playback stopped), a ring of RAM
 * and a worker thread that fills it ahead of the host's sequential READs
 * in large reads; READs are sent from it by DMA. See usb_storage_pf.h.
 * Without the ring (no memory, no thread slot) the read-ahead above runs
 * as before. */
#include "usb_storage_pf.h"
#ifndef HAVE_USB_READ_AHEAD
#error "HAVE_USB_PREFETCH falls back to HAVE_USB_READ_AHEAD"
#endif
/* One worker read: the size the FAT layer already reads in one command
 * (Fit-1 C12a), 4x the USB buffer, so the flash sees few large commands */
#define PF_CHUNK_BYTES  (256*1024)
#define PF_STACK_SIZE   0x1000
/* longest wait for one piece before it is read the ordinary way: short,
 * because the USB thread answers nothing else meanwhile (a bus reset's
 * teardown, a Bulk-Only reset) and the ordinary read then queues behind
 * the worker's read on the same disk anyway */
#define PF_WAIT_TICKS   (HZ/5)
static int pf_handle;               /* ring + stack; 0: none */
static unsigned char *pf_mem;       /* its 32-byte aligned start */
static unsigned int pf_alloc_kb;    /* its ring's size */
static bool pf_stack;               /* a worker stack in front of it */
static unsigned char *pf_ring;      /* uncached alias, 32-byte aligned */
static unsigned int pf_thread;      /* 0: not running */
static unsigned int pf_bs;          /* the ring's sector size */
static struct upf pf;
static struct semaphore pf_wake;    /* the worker: plan again */
static struct semaphore pf_landed;  /* the USB thread: a read landed */
static volatile bool pf_quit;
static bool pf_waiting;             /* the USB thread waits on pf_landed */
static bool pf_sending;             /* this READ goes out of the ring */
static unsigned int pf_ring_kb;     /* this session's ring, for the log */
#ifdef USB_PREFETCH_DEFAULT_OFF
/* #81 demand batching: the ring without the worker. On a sequential READ
 * that runs past the ring, the USB thread itself reads the next 64-256 KB
 * into it before anything is sent, while no Bulk-IN is in flight (the
 * serial rule, data-paths.md D12/D13): one ATA command now serves ~20 of
 * the host's 12 KiB FAT-scan READs instead of one. */
static bool pf_sync;
#define PF_SYNC() pf_sync
#else
#define PF_SYNC() false
#endif
static void usbstor_pf_cmd(unsigned char op);
#define PF_ON() (pf_thread != 0 || PF_SYNC())
#if !defined(BOOTLOADER)
extern unsigned int ata_usb_bg_thread;     /* ata-pp5020.c */
#endif
#else
#define PF_ON() false
#endif

/* The drive's own write cache out to the medium (SYNCHRONIZE CACHE, eject).
 * HAVE_STORAGE_FLUSH covers drivers that hold data in RAM; an ATA drive
 * keeps its volatile write cache on (ata.c set_features), so the host's
 * "done" must mean FLUSH CACHE ran. 0, or < 0 on failure. */
static int usbstor_flush_all(void)
{
    int rc = 0;
#ifdef HAVE_STORAGE_FLUSH
    storage_flush();
#endif
#if (CONFIG_STORAGE & STORAGE_ATA)
    rc = ata_flush();
#endif
    return rc;
}

/* The PP5022 Video's first Bulk-IN response is most reliable from this
 * page-aligned, uncached backing object. The SCSI payload is identical to
 * the core-buffer path; only the DMA placement differs. Keep the Developer
 * switch as a regression control, but make compatible placement normal on
 * this target. */
#if defined(IPOD_VIDEO) && defined(UNCACHED_ADDR) && defined(HAVE_IPOD_UI)
#define USB_INQUIRY_STATIC  /* ProkBox only: stock keeps tb.inquiry */
static bool sw_inqstatic = true;
#endif

#ifdef HAVE_USB_STATS
/* A volatile A/B escape hatch for the Developer menu, read at the next
 * session: demand batching (the default), off (plain serial reads, no
 * ring: the usbfix baseline) or the worker test. This must not turn the
 * worker on by accident: the Video default keeps ATA and bulk-IN DMA
 * apart (USB_PREFETCH_DEFAULT_OFF). The withdrawn marker experiments
 * (usb_pfoff, usb_pf64, usb_boost) are gone: docs/hw-experiments.md. */
static unsigned char sw_pf_mode;    /* enum usb_pf_mode */
#define sw_pf_test (sw_pf_mode == USB_PF_WORKER)
static char sw_text[48];

const char *usb_storage_switches(void)
{
#ifdef USB_INQUIRY_STATIC
#define SW_INQ  (sw_inqstatic ? "static" : "core")
#else
#define SW_INQ  "core"
#endif
    static const char *const names[] = { "batch", "off", "worker" };
    snprintf(sw_text, sizeof(sw_text), "pf=%s overlap=%d inq=%s",
             names[sw_pf_mode],
#ifdef USB_STORAGE_NO_READ_OVERLAP
             0, SW_INQ);
#else
             1, SW_INQ);
#endif
    return sw_text;
}
#endif /* HAVE_USB_STATS */

/* Which command types end in a FAIL CSW, a stall, or are the last one
 * before a Bulk-Only or bus reset (the device log's "usb cmdstat" line):
 * a few small rows keyed by the SCSI opcode. */
#define CS_ROWS 12
static struct { unsigned char op; unsigned short fail, stall, rst; }
    cs_tab[CS_ROWS];
static unsigned char cs_last_op = 0xff;

static unsigned short *cs_row(unsigned char op, int col)
{
    int i;
    for (i = 0; i < CS_ROWS; i++) {
        if (cs_tab[i].op == op && (cs_tab[i].fail | cs_tab[i].stall |
                                   cs_tab[i].rst))
            break;
        if (!(cs_tab[i].fail | cs_tab[i].stall | cs_tab[i].rst)) {
            cs_tab[i].op = op;
            break;
        }
    }
    if (i == CS_ROWS)
        i = CS_ROWS - 1;                /* overflow: lumped in the last row */
    return col == 0 ? &cs_tab[i].fail : col == 1 ? &cs_tab[i].stall
                                                  : &cs_tab[i].rst;
}

static void cs_count(int col)
{
    unsigned short *c = cs_row(cs_last_op, col);
    if (*c < 0xffff)
        (*c)++;
}

/* "op:fail/stall/rst ..." for the rows used; false when none */
bool usb_storage_cmdstat(char *buf, int size)
{
    int i, n = 0;
    buf[0] = 0;
    for (i = 0; i < CS_ROWS; i++) {
        if (!(cs_tab[i].fail | cs_tab[i].stall | cs_tab[i].rst))
            continue;
        n += snprintf(buf + n, size - n, "%s%02x:%u/%u/%u", n ? " " : "",
                      cs_tab[i].op, cs_tab[i].fail, cs_tab[i].stall,
                      cs_tab[i].rst);
        if (n >= size - 1)
            break;
    }
    return n > 0;
}

/* a new session: the per-opcode command table starts empty */
#ifdef HAVE_USB_STATS
static void usb_storage_reset_cmd_table(void)
{
    memset(cs_tab, 0, sizeof(cs_tab));
    cs_last_op = 0xff;
}
#endif /* HAVE_USB_STATS */

/* the ARC driver currently supports up to 64k USB transfers. This is
 * enough for efficient mass storage support, as commonly host OSes
 * don't do larger SCSI transfers anyway, so larger USB transfers
 * wouldn't buy us anything.
 * Due to being the double-buffering system used, using a smaller write buffer
 * ends up being more efficient. Measurements have shown that 24k to 28k is
 * optimal, except for sd devices that apparently don't gain anything from
 * double-buffering
 */
#ifdef USB_READ_BUFFER_SIZE
#define READ_BUFFER_SIZE USB_READ_BUFFER_SIZE
#else
#if CONFIG_USBOTG == USBOTG_AS3525
/* We'd need to implement multidescriptor dma for sizes >65535 */
#define READ_BUFFER_SIZE (1024*63)
#else
#define READ_BUFFER_SIZE (1024*64)
#endif /* CONFIG_CPU == AS3525 */
#endif /* USB_READ_BUFFER_SIZE */

/* We don't use sizeof() here, because we *need* a multiple of 32 */
#define MAX_CBW_SIZE 512

#ifdef USB_WRITE_BUFFER_SIZE
#define WRITE_BUFFER_SIZE USB_WRITE_BUFFER_SIZE
#else
#if (CONFIG_STORAGE & STORAGE_SD)
#if CONFIG_USBOTG == USBOTG_AS3525
/* We'd need to implement multidescriptor dma for sizes >65535 */
#define WRITE_BUFFER_SIZE (1024*63)
#else
#define WRITE_BUFFER_SIZE (1024*64)
#endif /* CONFIG_CPU == AS3525 */
#else
#define WRITE_BUFFER_SIZE (1024*24)
#endif /* (CONFIG_STORAGE & STORAGE_SD) */
#endif /* USB_WRITE_BUFFER_SIZE */

#define ALLOCATE_BUFFER_SIZE (2*MAX(READ_BUFFER_SIZE,WRITE_BUFFER_SIZE) \
                              + USBSTOR_SMALL_SIZE)

/* bulk-only class specific requests */
#define USB_BULK_RESET_REQUEST   0xff
#define USB_BULK_GET_MAX_LUN     0xfe

#define DIRECT_ACCESS_DEVICE     0x00    /* disks */
#define DEVICE_REMOVABLE         0x80

#define CBW_SIGNATURE            0x43425355
#define CSW_SIGNATURE            0x53425355

#define SCSI_TEST_UNIT_READY      0x00
#define SCSI_INQUIRY              0x12
#define SCSI_MODE_SENSE_6         0x1a
#define SCSI_MODE_SENSE_10        0x5a
#define SCSI_REQUEST_SENSE        0x03
#define SCSI_ALLOW_MEDIUM_REMOVAL 0x1e
#define SCSI_READ_CAPACITY_10     0x25
#define SCSI_READ_CAPACITY_16     0x9e
#define USB_UFI_READ_FORMAT_CAPACITY 0x23
#define SCSI_READ_10              0x28
#define SCSI_WRITE_10             0x2a
#define SCSI_START_STOP_UNIT      0x1b
#define SCSI_REPORT_LUNS          0xa0
#define SCSI_WRITE_BUFFER         0x3b
#define SCSI_SYNCHRONIZE_CACHE_10 0x35
#define SCSI_VERIFY_10            0x2f

#define SAT_ATA_PASSTHROUGH_12    0xa1
#define SAT_ATA_PASSTHROUGH_16    0x85

#define SCSI_READ_16              0x88
#define SCSI_WRITE_16             0x8a

#define UMS_STATUS_GOOD            0x00
#define UMS_STATUS_FAIL            0x01

#define SENSE_NOT_READY             0x02
#define SENSE_MEDIUM_ERROR          0x03
#define SENSE_ILLEGAL_REQUEST       0x05
#define SENSE_UNIT_ATTENTION        0x06
#define SENSE_DATA_PROTECT          0x07

#define ASC_MEDIUM_NOT_PRESENT      0x3a
#define ASC_INVALID_FIELD_IN_CBD    0x24
#define ASC_LBA_OUT_OF_RANGE        0x21
#define ASC_WRITE_ERROR             0x0C
#define ASC_READ_ERROR              0x11
#define ASC_NOT_READY               0x04
#define ASC_INVALID_COMMAND         0x20
#define ASC_WRITE_PROTECTED         0x27

/* A drive whose driver can only read it is reported write protected */
#ifdef HAVE_STORAGE_READONLY
#define LUN_READONLY(lun)   storage_readonly(lun)
#else
#define LUN_READONLY(lun)   false
#endif

#define ASCQ_BECOMING_READY         0x01

#define SCSI_FORMAT_CAPACITY_FORMATTED_MEDIA 0x02000000

/* storage interface */

#define USB_SC_SCSI      0x06            /* Transparent */
#define USB_PROT_BULK    0x50            /* bulk only */

static struct usb_interface_descriptor __attribute__((aligned(2)))
                                       interface_descriptor =
{
    .bLength            = sizeof(struct usb_interface_descriptor),
    .bDescriptorType    = USB_DT_INTERFACE,
    .bInterfaceNumber   = 0,
    .bAlternateSetting  = 0,
    .bNumEndpoints      = 2,
    .bInterfaceClass    = USB_CLASS_MASS_STORAGE,
    .bInterfaceSubClass = USB_SC_SCSI,
    .bInterfaceProtocol = USB_PROT_BULK,
    .iInterface         = 0
};

static struct usb_endpoint_descriptor __attribute__((aligned(2)))
                                      endpoint_descriptor =
{
    .bLength          = sizeof(struct usb_endpoint_descriptor),
    .bDescriptorType  = USB_DT_ENDPOINT,
    .bEndpointAddress = 0,
    .bmAttributes     = USB_ENDPOINT_XFER_BULK,
    .wMaxPacketSize   = 0,
    .bInterval        = 0
};

struct inquiry_data {
    unsigned char DeviceType;
    unsigned char DeviceTypeModifier;
    unsigned char Versions;
    unsigned char Format;
    unsigned char AdditionalLength;
    unsigned char Reserved[2];
    unsigned char Capability;
    unsigned char VendorId[8];
    unsigned char ProductId[16];
    unsigned char ProductRevisionLevel[4];
} __attribute__ ((packed));

struct report_lun_data {
    unsigned int lun_list_length;
    unsigned int reserved1;
    unsigned char luns[NUM_DRIVES][8];
} __attribute__ ((packed));

struct sense_data {
    unsigned char ResponseCode;
    unsigned char Obsolete;
    unsigned char fei_sensekey;
    unsigned int Information;
    unsigned char AdditionalSenseLength;
    unsigned int  CommandSpecificInformation;
    unsigned char AdditionalSenseCode;
    unsigned char AdditionalSenseCodeQualifier;
    unsigned char FieldReplaceableUnitCode;
    unsigned char SKSV;
    unsigned short SenseKeySpecific;
} __attribute__ ((packed));

struct mode_sense_bdesc_longlba {
    unsigned char num_blocks[8];
    unsigned char reserved[4];
    unsigned char block_size[4];
} __attribute__ ((packed));

struct mode_sense_bdesc_shortlba {
    unsigned char density_code;
    unsigned char num_blocks[3];
    unsigned char reserved;
    unsigned char block_size[3];
} __attribute__ ((packed));

struct mode_sense_data_10 {
    unsigned short mode_data_length;
    unsigned char medium_type;
    unsigned char device_specific;
    unsigned char longlba;
    unsigned char reserved;
    unsigned short block_descriptor_length;
    struct mode_sense_bdesc_longlba block_descriptor;
} __attribute__ ((packed));

struct mode_sense_data_6 {
    unsigned char mode_data_length;
    unsigned char medium_type;
    unsigned char device_specific;
    unsigned char block_descriptor_length;
    struct mode_sense_bdesc_shortlba block_descriptor;
} __attribute__ ((packed));

struct command_block_wrapper {
    unsigned int signature;
    unsigned int tag;
    unsigned int data_transfer_length;
    unsigned char flags;
    unsigned char lun;
    unsigned char command_length;
    unsigned char command_block[16];
} __attribute__ ((packed));

struct command_status_wrapper {
    unsigned int signature;
    unsigned int tag;
    unsigned int data_residue;
    unsigned char status;
} __attribute__ ((packed));

struct capacity_10 {
    unsigned char block_count[4];
    unsigned char block_size[4];
} __attribute__ ((packed));

struct capacity_16 {
    unsigned char block_count[8];
    unsigned char block_size[4];
    unsigned char flags[2];
    unsigned char lowest_aligned_lba[2];
} __attribute__ ((packed));

struct format_capacity {
    unsigned int following_length;
    unsigned int block_count;
    unsigned int block_size;
} __attribute__ ((packed));


static union {
    unsigned char* transfer_buffer;
    struct inquiry_data* inquiry;
    struct capacity_10* capacity_data_10;
    struct capacity_16* capacity_data_16;
    struct format_capacity* format_capacity_data;
    struct sense_data *sense_data;
    struct mode_sense_data_6 *ms_data_6;
    struct mode_sense_data_10 *ms_data_10;
    struct report_lun_data *lun_data;
    struct command_status_wrapper* csw;
    char *max_lun;
} tb;

/*
 * The usual small replies share tb, which comes from core_alloc_ex() on the
 * Video. macOS currently times out before its first INQUIRY data packet, so
 * usb_inqstatic selects this otherwise independent, page-aligned SDRAM reply
 * object. Use the same uncached alias as tb: this tests placement/alignment
 * without introducing a cache-coherency difference into the comparison.
 */
#ifdef USB_INQUIRY_STATIC
static struct inquiry_data inquiry_static __attribute__((aligned(4096)));
#endif

static struct inquiry_data *inquiry_reply_buffer(void)
{
#ifdef USB_INQUIRY_STATIC
    if (sw_inqstatic)
        return (void *)UNCACHED_ADDR((uintptr_t)&inquiry_static);
#endif
    return tb.inquiry;
}

#ifdef USB_INQUIRY_STATIC
void usb_storage_set_inquiry_static(bool enabled)
{
    sw_inqstatic = enabled;
}

bool usb_storage_inquiry_static(void)
{
    return sw_inqstatic;
}
#endif

static char *cbw_buffer;

static struct {
    sector_t sector;
    unsigned int count;
    unsigned int cur_cmd;
    unsigned int block_size;
    unsigned int tag;
    unsigned int lun;
    unsigned char *data[2];
    unsigned char data_select;
    /* The serial PP5022 path only reads this half after the previous
     * bulk-IN completion; never send it before that read has happened. */
    bool read_pending;
    unsigned int last_result;
    unsigned int data_residue;
} cur_cmd;

static struct {
    unsigned char sense_key;
    unsigned char information;
    unsigned char asc;
    unsigned char ascq;
} cur_sense_data;

static void handle_scsi(struct command_block_wrapper* cbw);
static void send_csw(int status);
static void reject_command(const struct command_block_wrapper *cbw);
static void csw_no_data(const struct command_block_wrapper *cbw, int status);
static void fail_cmd(const struct command_block_wrapper *cbw, int key,
                     int asc, int ascq);
static void account_data_transfer(int dir, int status, int length);
static void send_command_result(void *data,int size);
static void send_command_failed_result(void);
static void send_block_data(void *data,int size);
static void receive_block_data(void *data,int size);
#if CONFIG_RTC
static void receive_time(void);
#endif
static void fill_inquiry(struct inquiry_data *inquiry IF_MD(, int lun));
static void send_and_read_next(void);
static bool ejected[NUM_DRIVES];
static bool locked[NUM_DRIVES];

#ifdef HAVE_IPOD_UI
/* The iPod UI shell's USB screen shows "OK to Disconnect" once the host
 * has ejected every LUN (apps/ipodui/sysscreen.h). Read-only. */
bool usb_storage_all_ejected(void)
{
    int i, n = storage_num_drives();

    for (i = 0; i < n; i++)
        if (!ejected[i])
            return false;
    return n > 0;
}
#endif

static int usb_interface;

static struct usb_class_driver_ep_allocation ep_allocs[2] = {
    {.type = USB_ENDPOINT_XFER_BULK, .dir = DIR_IN, .optional = false, .mps = -1},
    {.type = USB_ENDPOINT_XFER_BULK, .dir = DIR_OUT, .optional = false, .mps = -1},
};

#define EP_IN (ep_allocs[0].ep)
#define EP_OUT (ep_allocs[1].ep)

#if defined(HAVE_MULTIDRIVE)
static bool skip_first = 0;
#endif

#ifdef USB_USE_RAMDISK
static unsigned char* ramdisk_buffer;
#endif

static enum {
    WAITING_FOR_COMMAND,
    WAITING_FOR_STORAGE,
    SENDING_BLOCKS,
    SENDING_RESULT,
    SENDING_FAILED_RESULT,
    RECEIVING_BLOCKS,
#if CONFIG_RTC
    RECEIVING_TIME,
#endif
    WAITING_FOR_CSW_COMPLETION_OR_COMMAND,
    WAITING_FOR_CSW_COMPLETION
} state = WAITING_FOR_COMMAND;

/* USB reliability (usb_storage_bot.h): the held CBW's validity, and a
 * WRITE's storage failure until its CSW */
static bool held_cbw_ok;
static struct ubot_wr wr_fail;
#if CONFIG_USBOTG == USBOTG_ARC
/* ARC reports each transfer's real status and length: a CBW must be 31
 * bytes (BOT 6.2.1) */
#define BOT_STATUS(s) (s)
#define BOT_LENGTH(l) (l)
#else
/* other drivers: as before, any OUT completion may be a CBW (the
 * signature check stays) */
#define BOT_STATUS(s) 0
#define BOT_LENGTH(l) UBOT_CBW_LEN
#endif

#ifdef HAVE_USB_STATS
static void usbstor_stats_rst(int kind)
{
    ustats.rst_log[ustats.rst_n % USB_STATS_RST_LOG] = kind << 4 | state;
    ustats.rst_n++;
}
#endif

#ifdef HAVE_USB_STATS
/* end of a transfer-complete pass: what the thread now waits for */
static void usbstor_stats_wait(unsigned long t0)
{
    unsigned long now = USTATS_NOW();

    ustats.busy_us += now - t0;
    if (now - t0 > ustats.busy_max_us)
        ustats.busy_max_us = now - t0;
    ustats_wait_since = now;
    ustats_wait_usb = state == SENDING_BLOCKS || state == RECEIVING_BLOCKS ||
                      state == SENDING_RESULT ||
                      state == SENDING_FAILED_RESULT;
}

/* CBWs held for the handover this session, the longest hold, and the one
 * still held now (ticks) */
void usb_storage_hold_info(unsigned int *n, long *max, long *now)
{
    *n = hold_n;
    *max = hold_max;
    *now = hold_since ? current_tick - hold_since : 0;
}

void usb_storage_reset_stats(void)
{
    usb_storage_reset_cmd_table();
    memset(&ustats, 0, sizeof(ustats));
    ustats_wait_since = 0;
    hold_n = 0;
    hold_since = hold_max = 0;
    ustats_inq_at = 0;
    ustats_rd_at = 0;           /* the mount is timed from the next READ */
    ustats_rd30 = 0;
#ifdef HAVE_USB_PREFETCH
    /* Never report a ring allocated by an earlier configuration as active
     * in this one; it obscures whether the policy gate actually held. */
    if (!PF_ON())
        pf_ring_kb = 0;
#endif
#if (CONFIG_STORAGE & STORAGE_ATA) && defined(HAVE_ATA_XFER_STATS)
    ata_get_xfer_stats(&ata_base);
#endif
}

void usb_storage_get_stats(struct usb_storage_stats *out)
{
    *out = ustats;
    out->rd30_kb = ustats_rd30 >> 10;
#ifdef HAVE_USB_PREFETCH
    out->pf_ring_kb = pf_ring_kb;
#endif
#if (CONFIG_STORAGE & STORAGE_ATA) && defined(HAVE_ATA_XFER_STATS)
    struct ata_xfer_stats now;
    ata_get_xfer_stats(&now);
    out->dma_rd = now.dma_rd - ata_base.dma_rd;
    out->dma_wr = now.dma_wr - ata_base.dma_wr;
    out->pio_rd = now.pio_rd - ata_base.pio_rd;
    out->pio_wr = now.pio_wr - ata_base.pio_wr;
    out->ata_resets = now.resets - ata_base.resets;
#endif
}
#endif /* HAVE_USB_STATS */

#if defined(HAVE_USB_STATS) || defined(HAVE_USB_READ_AHEAD) \
    || defined(HAVE_USB_PREFETCH)
static void usbstor_cmd_hook(unsigned char op)
{
    bool rd = op == SCSI_READ_10, wr = op == SCSI_WRITE_10;
#ifdef STORAGE_64BIT_SECTOR
    rd |= op == SCSI_READ_16;
    wr |= op == SCSI_WRITE_16;
#endif
#ifdef HAVE_USB_STATS
    unsigned long now = USTATS_NOW();

    if (op == SCSI_TEST_UNIT_READY && !ustats.cfg_to_tur_ms && ustats_cfg_at)
        ustats.cfg_to_tur_ms = (now - ustats_cfg_at) / 1000 | 1;
    if (op == SCSI_INQUIRY && !ustats.cfg_to_inq_ms && ustats_cfg_at) {
        ustats.cfg_to_inq_ms = (now - ustats_cfg_at) / 1000 | 1;
        ustats_inq_at = now;
    }
    ustats.rd_cmds += rd;
    ustats.wr_cmds += wr;
    if (op == SCSI_TEST_UNIT_READY)
        ustats.tur_cmds++;
    else if (op == SCSI_INQUIRY)
        ustats.inq_cmds++;
    else if (!rd && !wr)
        ustats.other_cmds++;
#endif
#ifdef HAVE_USB_READ_AHEAD
    if (wr)
        ura_write(&ra);         /* before any of its data reaches the disk */
#endif
#ifdef HAVE_USB_PREFETCH
    usbstor_pf_cmd(op);
#endif
    (void)rd;
    (void)wr;
}
#endif

#ifdef HAVE_USB_READ_AHEAD
/* a READ starts: its first chunk from the prefetch, if it is there */
static bool usbstor_ra_begin(sector_t limit)
{
    int half = 0;
    uint32_t off = 0;
    uint32_t n;

    if (PF_ON())
        return false;           /* the prefetch ring serves sequential READs */
    n = ura_read(&ra, cur_cmd.lun, cur_cmd.sector, cur_cmd.count,
                 READ_BUFFER_SIZE / cur_cmd.block_size, &half, &off);
    ra_limit = limit;
    if (!n)
        return false;
    cur_cmd.data[0] = blk_buffer + half * READ_BUFFER_SIZE
                      + off * cur_cmd.block_size;
    cur_cmd.data[1] = blk_buffer + !half * READ_BUFFER_SIZE;
    cur_cmd.data_select = 0;
    cur_cmd.last_result = 0;
#ifdef HAVE_USB_STATS
    ustats.ra_hits++;
#endif
    return true;
}

/* a READ's last chunk is on the wire from `sent`: prefetch into the other
 * half when the host is reading sequentially */
#ifndef USB_STORAGE_NO_READ_OVERLAP
static void usbstor_ra_fill(const unsigned char *sent)
{
    int other = sent - blk_buffer >= READ_BUFFER_SIZE ? 0 : 1;
    uint64_t start;
    uint32_t n;

    if (PF_ON())
        return;
    n = ura_plan(&ra, ra_limit, READ_BUFFER_SIZE / cur_cmd.block_size, &start);
    if (!n)
        return;
    if (storage_read_sectors(IF_MD(cur_cmd.lun,) start, n,
                             blk_buffer + other * READ_BUFFER_SIZE) == 0) {
        ura_filled(&ra, start, n, other);
#ifdef HAVE_USB_STATS
        ustats.ra_reads++;
#endif
    }
    else
        ra.count = 0;
}
#endif /* USB_STORAGE_NO_READ_OVERLAP */
#endif /* HAVE_USB_READ_AHEAD */

#ifdef HAVE_USB_PREFETCH
/* The worker: one storage read at a time into the ring, ahead of the host.
 * Same core as the USB thread, and the kernel is cooperative, so the
 * bookkeeping needs no lock: it only changes between blocking calls, and
 * upf_done() throws away a read that was dropped while in flight. */
static void pf_worker(void)
{
    while (!pf_quit) {
        uint64_t start;
        uint32_t pos, gen;
        uint32_t n = upf_plan(&pf, &start, &pos, &gen);

        if (!n) {
            semaphore_wait(&pf_wake, TIMEOUT_BLOCK);
            continue;
        }
        int lun = pf.lun;
        unsigned char *buf = pf_ring + pos * pf_bs;
        int rc = storage_read_sectors(IF_MD(lun,) start, n, buf);

        if (rc == 0 && gen == pf.gen)
            USBSTOR_PF_FILTER(lun, start, n, buf);
        (void)lun;
        if (upf_done(&pf, gen, start, n, rc == 0)) {
#ifdef HAVE_USB_STATS
            ustats.pf_fills++;
            ustats.pf_kb += n * pf_bs / 1024;
#endif
        }
        if (pf_waiting)
            semaphore_release(&pf_landed);
    }
    thread_exit();
}

/* Wait for a stopping worker to finish (its read in flight first) */
static void pf_join(void)
{
    if (pf_thread) {
        pf_quit = true;
        semaphore_release(&pf_wake);
        thread_wait(pf_thread);
        pf_thread = 0;
    }
}

/* At SET_CONFIGURATION: the ring and the worker, if there is room. The
 * memory comes out of the idle audio buffer (its shrink callback gives it
 * up while playback is stopped, and USB mode stops playback); the next
 * playback start takes it back (audio_reset_buffer()). A ring kept from
 * an earlier configuration of the same session is reused. */
static void pf_start(void)
{
    static const unsigned short ring_kb[] = { 8192, 4096, 2048 };
    const unsigned short *ring_kb_list = ring_kb;
    unsigned int ring_kb_n = ARRAYLEN(ring_kb);
    struct storage_info info;
    int h = -1;
    unsigned int i, kb = 0;

    pf_join();                  /* stopped at the last disconnect */
    pf_sending = false;
#ifdef USB_PREFETCH_DEFAULT_OFF
    /* The production arm stays serial: no worker, so no concurrent
     * IDE/USB DMA. Only the explicit volatile Developer A/B starts the
     * worker. Without it the ring is filled on demand (pf_sync). */
    pf_sync = false;
    if (sw_pf_mode == USB_PF_OFF) {
        if (pf_handle > 0)      /* plain serial reads: no ring at all */
            pf_handle = core_free(pf_handle);
        pf_mem = NULL;
        pf_ring = NULL;
        return;
    }
    if (!sw_pf_test) {
        static const unsigned short sync_kb[] = { 1024, 512 };
        ring_kb_list = sync_kb;
        ring_kb_n = ARRAYLEN(sync_kb);
    }
#endif
    /* the A/B's big ring: give it back; a batching ring has no worker
     * stack, so the worker never reuses one */
    if (pf_handle > 0 && (pf_alloc_kb > ring_kb_list[0]
                          || (sw_pf_test && !pf_stack))) {
        pf_handle = core_free(pf_handle);
        pf_mem = NULL;
        pf_ring = NULL;
    }
    if (pf_handle <= 0) {
        storage_get_info(0, &info);
        pf_bs = info.sector_size;
        if (pf_bs < 512 || pf_bs > 4096 || (pf_bs & (pf_bs - 1)))
            return;
        size_t stack = sw_pf_test ? PF_STACK_SIZE : 0;
        for (i = 0; i < ring_kb_n && h <= 0; i++) {
            kb = ring_kb_list[i];
            h = core_alloc_ex(kb * 1024 + stack + 31, &buflib_ops_locked);
        }
        if (h <= 0)
            return;
        pf_handle = h;
        pf_alloc_kb = kb;
        pf_stack = stack != 0;
        pf_mem = (void *)(((uintptr_t)core_get_data(h) + 31) & ~31);
#ifdef UNCACHED_ADDR
        pf_ring = (void *)UNCACHED_ADDR((uintptr_t)(pf_mem + stack));
#else
        pf_ring = pf_mem + stack;
#endif
        commit_discard_dcache(); /* no dirty line may land on the ring later */
    }
    /* chunk <= cap / 4 (usb_storage_pf.h): 256 KB, or 128 KB in a 512 KB ring */
    upf_init(&pf, pf_alloc_kb * 1024 / pf_bs,
             MIN(PF_CHUNK_BYTES, pf_alloc_kb * 1024 / 4) / pf_bs);
    semaphore_init(&pf_wake, 1, 0);
    semaphore_init(&pf_landed, 1, 0);
    pf_quit = false;
    pf_waiting = false;
#ifdef USB_PREFETCH_DEFAULT_OFF
    if (!sw_pf_test) {
        pf_sync = true;
        pf_ring_kb = pf_alloc_kb;
        return;
    }
#endif
    pf_thread = create_thread(pf_worker, pf_mem, PF_STACK_SIZE, 0,
                              "usb prefetch"
                              IF_PRIO(, PRIORITY_REALTIME) IF_COP(, CPU));
    if (pf_thread) {
        pf_ring_kb = pf_alloc_kb;
#if !defined(BOOTLOADER)
        ata_usb_bg_thread = pf_thread;  /* its disk waits may yield */
#endif
    }
    /* no thread slot: the read-ahead runs; the ring is freed at the end */
}

#ifdef HAVE_USB_STATS
void usb_storage_set_prefetch_mode(enum usb_pf_mode mode)
{
    sw_pf_mode = mode < USB_PF_MODES ? mode : USB_PF_BATCH;
}

enum usb_pf_mode usb_storage_prefetch_mode(void)
{
    return sw_pf_mode;
}
#endif

/* At disconnect (bus reset, SET_CONFIGURATION, the end of USB): tell the
 * worker to stop, without waiting. A bus reset's teardown runs here, and
 * the host's next SETUPs wait on this thread; the join (up to one 256 KB
 * read, longer on a spinning-up disk) moves to the next pf_start() or to
 * usb_storage_reap(). Nothing reads the ring until pf_start() has joined
 * the worker and started it over. */
static void pf_stop(void)
{
    if (pf_thread) {
        pf_quit = true;
        semaphore_release(&pf_wake);
    }
#ifdef USB_PREFETCH_DEFAULT_OFF
    pf_sync = false;            /* the ring is unused until pf_start() */
#endif
    pf_sending = false;
}

/* usb_core_exit(), after the last disconnect: before the disk goes back
 * to the system */
void usb_storage_reap(void)
{
    pf_join();
#if defined(HAVE_USB_PREFETCH) && !defined(BOOTLOADER)
    ata_usb_bg_thread = 0;
#endif
#ifdef USB_PREFETCH_DEFAULT_OFF
    pf_sync = false;
#endif
    if (pf_handle > 0)
        pf_handle = core_free(pf_handle);
    pf_mem = NULL;
    pf_ring = NULL;
}

/* Wait until [s, s + n) is in the ring: true, or false when it will not
 * come (the stream stopped or moved, or PF_WAIT_TICKS passed) */
static bool pf_wait(sector_t s, uint32_t n)
{
    int q = upf_lookup(&pf, cur_cmd.lun, s, n);
    long until;
#ifdef HAVE_USB_STATS
    unsigned long t0, dt;
#endif

    if (q != UPF_PENDING)
        return q == UPF_HIT;
#ifdef HAVE_USB_STATS
    t0 = USTATS_NOW();
#endif
#ifdef USB_PREFETCH_DEFAULT_OFF
    if (pf_sync) {
        /* Called before a piece is sent, never while one is on the wire
         * (a READ's start, or the previous piece's IN completion): the
         * disk read and the USB DMA never overlap, and nothing is sent
         * before it has been read (D13). */
        while (q == UPF_PENDING) {
            uint64_t start;
            uint32_t pos, gen;
            uint32_t got = upf_plan(&pf, &start, &pos, &gen);
            int rc;

            if (!got)
                break;
            rc = storage_read_sectors(IF_MD(pf.lun,) start, got,
                                      pf_ring + pos * pf_bs);
            if (rc == 0 && gen == pf.gen)
                USBSTOR_PF_FILTER(pf.lun, start, got, pf_ring + pos * pf_bs);
#ifdef HAVE_USB_STATS
            if (upf_done(&pf, gen, start, got, rc == 0)) {
                ustats.pf_fills++;
                ustats.pf_kb += got * pf_bs / 1024;
            }
#else
            upf_done(&pf, gen, start, got, rc == 0);
#endif
            q = upf_lookup(&pf, cur_cmd.lun, s, n);
        }
#ifdef HAVE_USB_STATS
        dt = USTATS_NOW() - t0;
        ustats.pf_stall_us += dt;
        if (dt > ustats.pf_stall_max_us)
            ustats.pf_stall_max_us = dt;
#endif
        return q == UPF_HIT;
    }
#endif
    until = current_tick + PF_WAIT_TICKS;
    pf_waiting = true;
    while (q == UPF_PENDING && TIME_BEFORE(current_tick, until)) {
        semaphore_wait(&pf_landed, HZ/10);
        q = upf_lookup(&pf, cur_cmd.lun, s, n);
    }
    pf_waiting = false;
#ifdef HAVE_USB_STATS
    dt = USTATS_NOW() - t0;
    ustats.pf_stall_us += dt;
    if (dt > ustats.pf_stall_max_us)
        ustats.pf_stall_max_us = dt;
#endif
    return q == UPF_HIT;
}

/* The next piece of a READ sent from the ring: at most one USB buffer's
 * worth, never across the ring's end. A piece that will not come is read
 * the ordinary way into the first USB buffer (free: the previous piece's
 * transfer has completed). */
static void pf_send_next(void)
{
    uint32_t piece = MIN(cur_cmd.count, READ_BUFFER_SIZE / pf_bs);
    uint32_t p = upf_pos(&pf, cur_cmd.sector);

    if (piece > pf.cap - p)
        piece = pf.cap - p;
    upf_pin(&pf, cur_cmd.sector);
    semaphore_release(&pf_wake);    /* room ahead may have opened */
    if (pf_wait(cur_cmd.sector, piece)) {
        send_block_data(pf_ring + p * pf_bs, piece * pf_bs);
    }
    else {
        unsigned char *b = USBSTOR_BLK;
        int rc = storage_read_sectors(IF_MD(cur_cmd.lun,) cur_cmd.sector,
                                      piece, b);
        if (rc == 0)
            USBSTOR_PF_FILTER(cur_cmd.lun, cur_cmd.sector, piece, b);
        else if (cur_cmd.last_result == 0)
            cur_cmd.last_result = rc;
#ifdef HAVE_USB_STATS
        ustats.pf_fallback++;
#endif
        send_block_data(b, piece * pf_bs);
    }
    cur_cmd.sector += piece;
    cur_cmd.count -= piece;
}

/* A READ starts: true when it goes out of the ring (it has started) */
static bool usbstor_pf_begin(sector_t limit)
{
    int q;

    pf_sending = false;
    if (!PF_ON() || cur_cmd.block_size != pf_bs)
        return false;
    q = upf_lookup(&pf, cur_cmd.lun, cur_cmd.sector, cur_cmd.count);
    if (q == UPF_PENDING &&
        !upf_is_seq(&pf, cur_cmd.lun, cur_cmd.sector))
        q = UPF_MISS;           /* never wait for a scattered READ */
    if (upf_note_read(&pf, cur_cmd.lun, cur_cmd.sector, cur_cmd.count,
                      q != UPF_MISS, limit))
        semaphore_release(&pf_wake);
#ifdef HAVE_USB_STATS
    if (q == UPF_HIT)
        ustats.pf_hits++;
    else if (q == UPF_PENDING)
        ustats.pf_pend++;
    else
        ustats.pf_miss++;
#endif
    if (q == UPF_MISS)
        return false;
    pf_sending = true;
    cur_cmd.last_result = 0;
    pf_send_next();
    return true;
}

/* Any command that may change the disk drops the ring first; only the
 * ones known to leave it alone keep it */
static void usbstor_pf_cmd(unsigned char op)
{
    switch (op) {
        case SCSI_READ_10:
#ifdef STORAGE_64BIT_SECTOR
        case SCSI_READ_16:
#endif
        case SCSI_TEST_UNIT_READY:
        case SCSI_INQUIRY:
        case SCSI_MODE_SENSE_6:
        case SCSI_MODE_SENSE_10:
        case SCSI_REQUEST_SENSE:
        case SCSI_ALLOW_MEDIUM_REMOVAL:
        case SCSI_READ_CAPACITY_10:
        case SCSI_READ_CAPACITY_16:
        case USB_UFI_READ_FORMAT_CAPACITY:
        case SCSI_REPORT_LUNS:
            return;
        case SCSI_SYNCHRONIZE_CACHE_10:
        case SCSI_VERIFY_10:
            /* dropped on purpose: the host asks because it changed or
             * wants to trust the medium */
        default:
            if (PF_ON())
                upf_drop(&pf);
            pf_sending = false;
            return;
    }
}
#endif /* HAVE_USB_PREFETCH */

#if CONFIG_RTC
static void yearday_to_daymonth(int yd, int y, int *d, int *m)
{
    static char t[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

    bool leap = (y%4 == 0 && y%100 != 0) || y%400 == 0;
    t[1] = leap ? 29 : 28;

    int i;
    for (i = 0; i < 12 && yd >= t[i]; i++)
        yd -= t[i];

    *d = yd+1;
    *m = i;
}
#endif /* CONFIG_RTC */

static bool check_disk_present(IF_MD_NONVOID(int volume))
{
#ifdef USB_USE_RAMDISK
    return true;
#else
    return disk_present(IF_MD(volume));
#endif
}

#ifdef HAVE_HOTSWAP
static void usb_storage_notify_hotswap(int volume,bool inserted)
{
    logf("notify %d",inserted);
    if(inserted && check_disk_present(IF_MD(volume))) {
        ejected[volume] = false;
    }
    else {
        ejected[volume] = true;
        /* If this happens while the device is locked, weird things may happen.
           At least try to keep our state consistent */
        locked[volume]=false;
    }
}
#endif

#ifdef HAVE_MULTIDRIVE
void usb_set_skip_first_drive(bool skip)
{
    skip_first = skip;
}
#endif

/* called by usb_core_init() */
static void usb_storage_init(void)
{
    logf("usb_storage_init done");
}

static int usb_storage_set_first_interface(int interface)
{
    usb_interface = interface;
    return interface + 1;
}

static int usb_storage_get_config_descriptor(unsigned char *dest,int max_packet_size)
{
    unsigned char *orig_dest = dest;

    interface_descriptor.bInterfaceNumber = usb_interface;
    PACK_DATA(&dest, interface_descriptor);

    endpoint_descriptor.wMaxPacketSize = max_packet_size;

    endpoint_descriptor.bEndpointAddress = EP_IN;
    PACK_DATA(&dest, endpoint_descriptor);

    endpoint_descriptor.bEndpointAddress = EP_OUT;
    PACK_DATA(&dest, endpoint_descriptor);

    return (dest - orig_dest);
}
#if (CONFIG_CPU == IMX31L || defined(CPU_TCC780X) || \
     CONFIG_CPU == S5L8702 || CONFIG_CPU == S5L8701 || CONFIG_CPU == AS3525v2 || \
     defined(BOOTLOADER) || CONFIG_CPU == DM320) && !defined(CPU_PP502x)
#define USB_STATIC_ALLOC
#else
static int usb_handle = 0;
#endif

static int usb_storage_init_connection(void)
{
    logf("ums: set config");
    /* prime rx endpoint. We only need room for commands */
    state = WAITING_FOR_COMMAND;
    held_cbw_ok = false;
    ubot_wr_begin(&wr_fail);

#ifdef USB_STATIC_ALLOC
    static unsigned char _cbw_buffer[MAX_CBW_SIZE]
        USB_DEVBSS_ATTR __attribute__((aligned(32)));
    cbw_buffer = (void *)_cbw_buffer;

    static unsigned char _transfer_buffer[ALLOCATE_BUFFER_SIZE]
        USB_DEVBSS_ATTR __attribute__((aligned(32)));
    tb.transfer_buffer = (void *)_transfer_buffer;
#ifdef USB_USE_RAMDISK
    static unsigned char _ramdisk_buffer[RAMDISK_SIZE*SECTOR_SIZE];
    ramdisk_buffer = _ramdisk_buffer;
#endif
#else
    unsigned char * buffer;

    // Add 31 to handle worst-case misalignment
#if defined(HAVE_IPOD_UI) && !defined(BOOTLOADER)
    /* ProkBox: playback stops first (a paused audio buffer refuses this
     * thread's shrink), then a bounded retry; an OOM fails this driver's
     * configuration (logged), never a panic: usb_buf_alloc.h */
    usb_handle = usb_buf_alloc(ALLOCATE_BUFFER_SIZE + MAX_CBW_SIZE + 31,
                               &buflib_ops_locked, "storage");
    if (usb_handle <= 0)
    {
        usb_handle = 0;
        return -1;
    }
#else
    usb_handle = core_alloc_ex(ALLOCATE_BUFFER_SIZE + MAX_CBW_SIZE + 31,
                               &buflib_ops_locked);
    if (usb_handle < 0)
        panicf("%s(): OOM", __func__);
#endif

    buffer = core_get_data(usb_handle);
#if defined(UNCACHED_ADDR) && CONFIG_CPU != AS3525
    cbw_buffer = (void *)UNCACHED_ADDR((unsigned int)(buffer+31) & 0xffffffe0);
#else
    cbw_buffer = (void *)((unsigned int)(buffer+31) & 0xffffffe0);
#endif
    tb.transfer_buffer = cbw_buffer + MAX_CBW_SIZE;
#ifdef HAVE_USB_READ_AHEAD
    blk_buffer = tb.transfer_buffer + USBSTOR_SMALL_SIZE;
#endif
#ifdef HAVE_USB_PREFETCH
    pf_start();
#endif
    commit_discard_dcache();
#ifdef USB_USE_RAMDISK
    ramdisk_buffer = tb.transfer_buffer + ALLOCATE_BUFFER_SIZE;
#endif
#endif
    usb_drv_recv_nonblocking(EP_OUT, cbw_buffer, MAX_CBW_SIZE);

    int i;
    for(i=0;i<storage_num_drives();i++) {
#ifdef HAVE_USB_SECTOR_XLATE
        xlate_learned[i] = false;   /* read the layout again */
#endif
        locked[i] = false;
        ejected[i] = !check_disk_present(IF_MD(i));
        queue_broadcast(SYS_USB_LUN_LOCKED, (i<<16)+0);
    }
#ifdef HAVE_USB_READ_AHEAD
    ura_reset(&ra);
#endif
#ifdef HAVE_USB_STATS
    ustats_cfg_at = USTATS_NOW();
    /* the counters span the whole USB session (usb_storage_reset_stats()
     * at the shell's Connected screen): the host may configure the
     * device again within one session (after an eject or a reset) */
    ustats_wait_since = 0;
    {
        int speed = usb_drv_port_speed();
        if (ustats.cfg_count && ustats.high_speed != speed)
            ustats.high_speed_changes++;
        if (!ustats.cfg_count)
            ustats.high_speed_first = speed;
        ustats.high_speed = speed;
        ustats.cfg_count++;
    }
#endif
    return 0;
}

void usb_storage_disconnect(void)
{
    cs_count(2);
#ifdef HAVE_USB_STATS
    ustats.cfg_downs++;
    usbstor_stats_rst(USB_STATS_RST_DOWN);
#endif
#ifdef HAVE_USB_PREFETCH
    pf_stop();
#endif
#ifndef USB_STATIC_ALLOC
    usb_handle = core_free(usb_handle);
#endif
}

/* called by usb_core_transfer_complete() */
static void usb_storage_transfer_complete(int ep,int dir,int status,int length)
{
    (void)ep;
    struct command_block_wrapper* cbw = (void*)cbw_buffer;
#if CONFIG_RTC
    struct tm tm;
#endif


    logf("transfer result for ep %d/%d %X %d", ep,dir,status, length);

#ifdef HAVE_USB_STATS
    unsigned long ustats_t0 = USTATS_NOW();
    if (ustats_wait_since) {
        if (ustats_wait_usb)
            ustats.usb_us += ustats_t0 - ustats_wait_since;
        else
            ustats.host_us += ustats_t0 - ustats_wait_since;
        ustats_wait_since = 0;
    }
    if (status == 0 && length > 0) {
        if (dir == USB_DIR_IN && state == SENDING_BLOCKS)
        {
            ustats.rd_bytes += length;
            if (ustats_rd_at && ustats_t0 - ustats_rd_at < 30000000UL)
                ustats_rd30 += length;
        }
        else if (dir == USB_DIR_OUT && state == RECEIVING_BLOCKS)
            ustats.wr_bytes += length;
    }
#endif

    account_data_transfer(dir, status, length);

#ifdef HAVE_USB_STATS
    /* Record the class-driver completion, rather than the prime: the
     * active-dTD failure reached prime but never reaches this point. */
    if (status == 0 && dir == USB_DIR_IN && state == SENDING_RESULT &&
        cur_cmd.cur_cmd == SCSI_INQUIRY && ustats_inq_at &&
        !ustats.inq_to_data_ms)
        ustats.inq_to_data_ms = (ustats_t0 - ustats_inq_at) / 1000 | 1;
#endif

    switch(state) {
        case WAITING_FOR_STORAGE:
            /* The CBW is retained until usb_storage_notify_event(). */
            break;
        case RECEIVING_BLOCKS:
            if(dir==USB_DIR_IN) {
                /* stale: an IN completion is never write data */
                logf("IN received in RECEIVING");
#ifdef HAVE_USB_STATS
                ustats.ignored++;
#endif
                break;
            }
            logf("scsi write %llu %d", cur_cmd.sector, cur_cmd.count);
            if(status==0) {
                unsigned int chunk = MIN(WRITE_BUFFER_SIZE,
                                         cur_cmd.count*cur_cmd.block_size);
                if((unsigned int)length != chunk) {
                    /* The host ended the data phase early (a short
                     * packet); nothing else is primed, so the CSW may go.
                     * This used to leave the command without a CSW. */
                    logf("unexpected length :%d",length);
                    ubot_wr_fail(&wr_fail, cur_cmd.data_residue,
                                 (unsigned int)length);
                    cur_cmd.data_residue = ubot_wr_residue(&wr_fail,
                                                   cur_cmd.data_residue);
                    send_csw(UMS_STATUS_FAIL);
                    cur_sense_data.sense_key=SENSE_ILLEGAL_REQUEST;
                    cur_sense_data.asc=ASC_INVALID_COMMAND;
                    cur_sense_data.ascq=0;
                    break;
                }

                sector_t next_sector = cur_cmd.sector +
                             (WRITE_BUFFER_SIZE/cur_cmd.block_size);
                unsigned int next_count = cur_cmd.count -
                             MIN(cur_cmd.count,WRITE_BUFFER_SIZE/cur_cmd.block_size);
                int next_select = !cur_cmd.data_select;

                if(next_count!=0) {
                    /* Ask the host to send more, to the other buffer */
                    receive_block_data(cur_cmd.data[next_select],
                                       MIN(WRITE_BUFFER_SIZE,next_count*cur_cmd.block_size));
                }

                /* Now write the data that just came in, while the host is
                   sending the next bit */
#ifdef USB_USE_RAMDISK
                memcpy(ramdisk_buffer + cur_cmd.sector*cur_cmd.block_size,
                        cur_cmd.data[cur_cmd.data_select],
                        MIN(WRITE_BUFFER_SIZE/cur_cmd.block_size, cur_cmd.count)*cur_cmd.block_size);
#else
                if(ubot_wr_should_write(&wr_fail)) {
                    int result = USBSTOR_WRITE_SECTORS_FILTER();

                    if (result == 0) {
                        result = storage_write_sectors(IF_MD(cur_cmd.lun,)
                            cur_cmd.sector,
                            MIN(WRITE_BUFFER_SIZE/cur_cmd.block_size, cur_cmd.count),
                            cur_cmd.data[cur_cmd.data_select]);
                    }

                    if(result != 0) {
                        /* The next chunk's receive is already primed:
                         * the CSW (which primes the CBW receive) must
                         * wait for the end of the data phase, else the
                         * host's next data lands in the CBW buffer. The
                         * rest is received and dropped (usb_storage_bot.h). */
                        ubot_wr_fail(&wr_fail, cur_cmd.data_residue, chunk);
                        cur_sense_data.sense_key=SENSE_MEDIUM_ERROR;
                        cur_sense_data.asc=ASC_WRITE_ERROR;
                        cur_sense_data.ascq=0;
#ifdef HAVE_USB_STATS
                        ustats.wr_fails++;
#endif
                    }
                }
#endif
                if(next_count==0) {
                    cur_cmd.data_residue = ubot_wr_residue(&wr_fail,
                                                   cur_cmd.data_residue);
                    send_csw(wr_fail.failed ? UMS_STATUS_FAIL
                                            : UMS_STATUS_GOOD);
                }

                /* Switch buffers for the next one */
                cur_cmd.data_select=!cur_cmd.data_select;

                cur_cmd.sector = next_sector;
                cur_cmd.count = next_count;
            }
            else {
                logf("Transfer failed %X",status);
                send_csw(UMS_STATUS_FAIL);
                /* TODO fill in cur_sense_data */
                cur_sense_data.sense_key=0;
                cur_sense_data.information=0;
                cur_sense_data.asc=0;
                cur_sense_data.ascq=0;
            }
            break;
        case WAITING_FOR_CSW_COMPLETION_OR_COMMAND:
        case WAITING_FOR_COMMAND:
        case WAITING_FOR_CSW_COMPLETION: {
            /* usb_storage_bot.h: only an OUT completion is a command,
             * and only a 31-byte one is a valid CBW */
            enum ubot_wait w =
                state == WAITING_FOR_COMMAND ? UBOT_WAIT_CMD :
                state == WAITING_FOR_CSW_COMPLETION ? UBOT_WAIT_CSW :
                UBOT_WAIT_CSW_OR_CMD;
            switch(ubot_between(w, dir == USB_DIR_IN, BOT_STATUS(status),
                                BOT_LENGTH(length), held_cbw_ok)) {
                case UBOT_CSW_DONE:     /* This was the CSW */
                    state = WAITING_FOR_COMMAND;
                    break;
                case UBOT_HOLD_CBW:
                    /* This was the command. We now have the CBW, but we
                     * won't execute it yet to avoid issues with the
                     * still-pending CSW */
                    held_cbw_ok = ubot_cbw_valid(BOT_STATUS(status),
                                                 BOT_LENGTH(length));
                    state = WAITING_FOR_CSW_COMPLETION;
                    break;
                case UBOT_RUN_CBW:
                    state = WAITING_FOR_COMMAND;
                    handle_scsi(cbw);
                    break;
                case UBOT_BAD_CBW:
                    /* BOT 6.6.1: halt both pipes until the host's reset
                     * recovery (which primes the CBW receive again) */
                    logf("ums: invalid cbw len=%d status=%d", length, status);
                    state = WAITING_FOR_COMMAND;
                    cbw->signature = 0;
                    usb_drv_stall(EP_IN, true, true);
                    usb_drv_stall(EP_OUT, true, false);
                    cs_last_op = 0xfe;      /* no command: a bad CBW */
                    cs_count(1);
#ifdef HAVE_USB_STATS
                    ustats.bad_cbws++;
#endif
                    break;
                case UBOT_IGNORE:
                    logf("ums: stale completion dir=%d state=%d", dir, state);
#ifdef HAVE_USB_STATS
                    ustats.ignored++;
#endif
                    break;
            }
            break;
        }
        case SENDING_RESULT:
            if(dir==USB_DIR_OUT) {
                logf("OUT received in SENDING");
            }
            if(status==0) {
                //logf("data sent, now send csw");
                send_csw(UMS_STATUS_GOOD);
            }
            else {
                logf("Transfer failed %X",status);
                send_csw(UMS_STATUS_FAIL);
                /* TODO fill in cur_sense_data */
                cur_sense_data.sense_key=0;
                cur_sense_data.information=0;
                cur_sense_data.asc=0;
                cur_sense_data.ascq=0;
            }
            break;
        case SENDING_FAILED_RESULT:
            if(dir==USB_DIR_OUT) {
                logf("OUT received in SENDING");
            }
            send_csw(UMS_STATUS_FAIL);
            break;
        case SENDING_BLOCKS:
            if(dir==USB_DIR_OUT) {
                logf("OUT received in SENDING");
            }
            if(status==0) {
                if(cur_cmd.count==0) {
                    //logf("data sent, now send csw");
                    if(cur_cmd.last_result!=0) {
                        /* The last read failed. */
                        send_csw(UMS_STATUS_FAIL);
                        cur_sense_data.sense_key=SENSE_MEDIUM_ERROR;
                        cur_sense_data.asc=ASC_READ_ERROR;
                        cur_sense_data.ascq=0;
                        return;
                    }
                    else
                        send_csw(UMS_STATUS_GOOD);
                }
                else {
#ifdef HAVE_USB_PREFETCH
                    if (pf_sending)
                        pf_send_next();
                    else
#endif
                    send_and_read_next();
                }
            }
            else {
                logf("Transfer failed %X",status);
                send_csw(UMS_STATUS_FAIL);
                /* TODO fill in cur_sense_data */
                cur_sense_data.sense_key=0;
                cur_sense_data.information=0;
                cur_sense_data.asc=0;
                cur_sense_data.ascq=0;
            }
            break;
#if CONFIG_RTC
        case RECEIVING_TIME:
            tm.tm_year=(tb.transfer_buffer[0]<<8)+tb.transfer_buffer[1] - 1900;
            tm.tm_yday=(tb.transfer_buffer[2]<<8)+tb.transfer_buffer[3];
            tm.tm_hour=tb.transfer_buffer[5];
            tm.tm_min=tb.transfer_buffer[6];
            tm.tm_sec=tb.transfer_buffer[7];
            yearday_to_daymonth(tm.tm_yday,tm.tm_year + 1900,&tm.tm_mday,&tm.tm_mon);
            set_day_of_week(&tm);
            set_time(&tm);
            send_csw(UMS_STATUS_GOOD);
            break;
#endif /* CONFIG_RTC */
    }
#ifdef HAVE_USB_STATS
    usbstor_stats_wait(ustats_t0);
#endif
}

#if (CONFIG_STORAGE & STORAGE_ATA)
static void usb_storage_send_ata_identify(void)
{
    unsigned short* identify_info = ata_get_identify();

    for (int i = 0 ; i < ATA_IDENTIFY_WORDS ; i++) {
        tb.transfer_buffer[i*2] = identify_info[i] & 0xff;
        tb.transfer_buffer[i*2+1] = identify_info[i] >> 8;
    }
    cur_cmd.count = 0;
    cur_cmd.last_result = 0;
    /* IDENTIFY can be the first data command after SET_CONFIGURATION.
     * cur_cmd.data[] belongs to previous READ/WRITE commands and may be
     * unset, or refer to a buffer freed by a USB reset. */
    send_block_data(tb.transfer_buffer, ATA_IDENTIFY_WORDS * 2);
}
#ifdef HAVE_ATA_SMART
static void usb_storage_send_smart(uint8_t cmd)
{
    ata_read_smart((struct ata_smart_values*) tb.transfer_buffer, cmd);
    cur_cmd.count = 0;
    cur_cmd.last_result = 0;
    cur_cmd.data[0] = tb.transfer_buffer;
    send_block_data(cur_cmd.data[0], 512);
}
#endif /* HAVE_ATA_SMART */
#endif /* STORAGE_ATA */

/* Bulk-Only Mass Storage Reset (BOT 3.1): the host abandons whatever
 * command was running, in any state, and sends a new CBW after clearing
 * both pipes' halts. UMS BOT 3.1 says the device shall preserve the value
 * of its bulk data toggle bits and endpoint STALL conditions despite the
 * reset. */
static void usbstor_bulk_reset(void)
{
    cs_count(2);
#ifdef HAVE_USB_STATS
    ustats.bulk_resets++;
    usbstor_stats_rst(USB_STATS_RST_BULK);
#endif
#if CONFIG_USBOTG == USBOTG_ARC
    /* USB reliability: the reset used to rearm the CBW receive only in
     * WAITING_FOR_STORAGE, so a reset mid-command (a READ stuck behind the
     * prefetch, a WRITE, an invalid CBW's halt) left nothing primed and
     * the device deaf until unplugged. Drop whatever is still primed on
     * both pipes (a data or CSW send would answer the host's next read
     * with stale bytes; a data receive would take its next CBW as write
     * data), then wait for the command. ARC's endpoint reset is a flush
     * only: toggles and halts stay, as BOT asks. */
    usb_drv_reset_endpoint(EP_IN, true);
    usb_drv_reset_endpoint(EP_OUT, false);
#ifdef HAVE_USB_PREFETCH
    if (PF_ON())
        upf_drop(&pf);
    pf_sending = false;
#endif
#ifdef HAVE_USB_READ_AHEAD
    ura_reset(&ra);
#endif
    ubot_wr_begin(&wr_fail);
    held_cbw_ok = false;
    state = WAITING_FOR_COMMAND;
    /* a CBW held for the storage handover is abandoned too */
    ((struct command_block_wrapper *)cbw_buffer)->signature = 0;
    usb_drv_recv_nonblocking(EP_OUT, cbw_buffer, MAX_CBW_SIZE);
#else
    if(state == WAITING_FOR_STORAGE) {
        /* Discard the retained CBW on host reset and accept a new
         * one; do not replay the cancelled command when ready. */
        usb_drv_recv_nonblocking(EP_OUT, cbw_buffer, MAX_CBW_SIZE);
    }
    state = WAITING_FOR_COMMAND;
#endif
}

/* called by usb_core_control_request() */
static bool usb_storage_control_request(struct usb_ctrlrequest* req, uint8_t* reqdata, size_t reqdata_size)
{
    (void)reqdata;
    (void)reqdata_size;

    bool handled = false;

    switch (req->bRequest) {
        case USB_BULK_GET_MAX_LUN: {
            *tb.max_lun = storage_num_drives() - 1;
#if defined(HAVE_MULTIDRIVE)
            if(skip_first) (*tb.max_lun) --;
#endif
            logf("ums: getmaxlun");
            usb_core_control_response(USB_CONTROL_ACK, tb.max_lun, 1);
            handled = true;
            break;
        }

        case USB_BULK_RESET_REQUEST:
            logf("ums: bulk reset");
            usbstor_bulk_reset();
            usb_core_control_response(USB_CONTROL_ACK, NULL, 0);
            handled = true;
            break;
    }

    return handled;
}

static void send_and_read_next(void)
{
#if defined(HAVE_USB_READ_AHEAD) && !defined(USB_STORAGE_NO_READ_OVERLAP)
    unsigned char *sent = cur_cmd.data[cur_cmd.data_select];
#endif
    int result;

#ifdef USB_STORAGE_NO_READ_OVERLAP
    if (cur_cmd.read_pending) {
        /* The old no-overlap control deferred this ATA DMA, but failed to
         * perform it before sending the other half. That exposed stale
         * 64 KiB buffers as alternating file corruption. */
#ifdef USB_USE_RAMDISK
        memcpy(cur_cmd.data[cur_cmd.data_select],
               ramdisk_buffer + cur_cmd.sector * cur_cmd.block_size,
               MIN(READ_BUFFER_SIZE / cur_cmd.block_size, cur_cmd.count) *
               cur_cmd.block_size);
        result = 0;
#else
        result = storage_read_sectors(IF_MD(cur_cmd.lun,) cur_cmd.sector,
                MIN(READ_BUFFER_SIZE / cur_cmd.block_size, cur_cmd.count),
                cur_cmd.data[cur_cmd.data_select]);
#endif
        cur_cmd.read_pending = false;
        if (result != 0 && cur_cmd.last_result == 0)
            cur_cmd.last_result = result;
    }
#endif

    result = USBSTOR_READ_SECTORS_FILTER();

    if(result != 0 && cur_cmd.last_result == 0)
        cur_cmd.last_result = result;

    send_block_data(cur_cmd.data[cur_cmd.data_select],
                    MIN(READ_BUFFER_SIZE,cur_cmd.count*cur_cmd.block_size));

    /* Switch buffers for the next one */
    cur_cmd.data_select=!cur_cmd.data_select;

    cur_cmd.sector+=(READ_BUFFER_SIZE/cur_cmd.block_size);
    cur_cmd.count-=MIN(cur_cmd.count,READ_BUFFER_SIZE/cur_cmd.block_size);

    if(cur_cmd.count!=0) {
#ifdef USB_STORAGE_NO_READ_OVERLAP
        /* Fill it only when this bulk-IN completes. */
        cur_cmd.read_pending = true;
#else
        /* already read the next bit, so we can send it out immediately when the
         * current transfer completes.  */
#ifdef USB_USE_RAMDISK
        memcpy(cur_cmd.data[cur_cmd.data_select],
                ramdisk_buffer + cur_cmd.sector*cur_cmd.block_size,
                MIN(READ_BUFFER_SIZE/cur_cmd.block_size, cur_cmd.count)*cur_cmd.block_size);
#else
        result = storage_read_sectors(IF_MD(cur_cmd.lun,)
                cur_cmd.sector,
                MIN(READ_BUFFER_SIZE/cur_cmd.block_size, cur_cmd.count),
                cur_cmd.data[cur_cmd.data_select]);
        if(cur_cmd.last_result == 0)
            cur_cmd.last_result = result;

#endif
#endif /* USB_STORAGE_NO_READ_OVERLAP */
    }
#ifdef HAVE_USB_READ_AHEAD
#ifndef USB_STORAGE_NO_READ_OVERLAP
    else if(cur_cmd.last_result == 0)
        usbstor_ra_fill(sent);  /* while this last chunk is on the wire */
#endif
#endif
}
/****************************************************************************/

#ifdef HAVE_USB_SECTOR_XLATE
/* The disk's MBR and its FAT32 boot sectors, as stored. Any read error or
 * unusual layout leaves the translation off: the disk is shown as before,
 * in 2048-byte blocks. */
static void xlate_learn(int lun)
{
    static unsigned char buf[XLATE_SECTOR] STORAGE_ALIGN_ATTR;
    struct xlate_state *s = &xlate[lun];

    struct storage_info info;

    xlate_learned[lun] = true;
    memset(s, 0, sizeof(*s));
    storage_get_info(lun, &info);
    if (info.sector_size != XLATE_SECTOR    /* the filters step by 512 */
        || storage_read_sectors(IF_MD(lun,) 0, 1, buf))
        return;
    xlate_learn_mbr(s, disk_get_sector_multiplier(IF_MD(lun)), buf);
    for (int i = 0; i < XLATE_MAX_PARTS && xlate_active(s); i++)
    {
        if (!s->part[i].start)
            continue;
        if (storage_read_sectors(IF_MD(lun,) s->part[i].start, 1, buf))
            s->mult = 0;
        else {
            xlate_learn_boot(s, i, buf);
#ifdef HAVE_USB_STATS
            /* #81: where the first FAT ends, in host sectors, to time the
             * host's mount-time FAT scan */
            unsigned int bps = buf[11] | buf[12] << 8;
            unsigned int rsvd = buf[14] | buf[15] << 8;
            uint32_t fatsz = buf[36] | buf[37] << 8 | buf[38] << 16 |
                             (uint32_t)buf[39] << 24;
            if (!ustats_fat_end && xlate_active(s) && bps >= XLATE_SECTOR)
                ustats_fat_end = s->part[i].start +
                    (sector_t)(rsvd + fatsz) * (bps / XLATE_SECTOR);
#endif
        }
    }
    logf("ums: xlate lun %d mult %u", lun, s->mult);
}
#endif

static void fill_sense(void)
{
    tb.sense_data->ResponseCode=0x70;/*current error*/
    tb.sense_data->Obsolete=0;
    tb.sense_data->fei_sensekey=cur_sense_data.sense_key&0x0f;
    tb.sense_data->Information=cur_sense_data.information;
    tb.sense_data->AdditionalSenseLength=10;
    tb.sense_data->CommandSpecificInformation=0;
    tb.sense_data->AdditionalSenseCode=cur_sense_data.asc;
    tb.sense_data->AdditionalSenseCodeQualifier=cur_sense_data.ascq;
    tb.sense_data->FieldReplaceableUnitCode=0;
    tb.sense_data->SKSV=0;
    tb.sense_data->SenseKeySpecific=0;
}

static void handle_scsi_ready(struct command_block_wrapper* cbw)
{
    struct storage_info info;
    unsigned int length = cbw->data_transfer_length;
    sector_t block_count;
    unsigned int block_size;
    bool lun_present=true;
    unsigned char lun = cbw->lun;

    if(letoh32(cbw->signature) != CBW_SIGNATURE) {
        logf("ums: bad cbw signature (%x)", cbw->signature);
        usb_drv_stall(EP_IN, true,true);
        usb_drv_stall(EP_OUT, true,false);
        return;
    }
    /* Clear the signature to prevent possible bugs elsewhere
     * to trigger a second execution of the same command with
     * bogus data */
    cbw->signature=0;

#if defined(HAVE_MULTIDRIVE)
    if(skip_first) lun++;
#endif

    storage_get_info(lun,&info);
#ifdef USB_USE_RAMDISK
    block_size = SECTOR_SIZE;
    block_count = RAMDISK_SIZE;
#else
    block_size=info.sector_size;
    block_count=info.num_sectors;
#endif

    if(storage_removable(lun) && !storage_present(lun))
        ejected[lun] = true;

    if(ejected[lun])
        lun_present = false;

    unsigned int block_size_mult = 1; /* Number of LOGICAL storage device blocks in each USB block */
#ifdef MAX_VIRT_SECTOR_SIZE
    block_size_mult = disk_get_sector_multiplier(IF_MD(lun));
#endif
#ifdef HAVE_USB_SECTOR_XLATE
    if (lun_present && !xlate_learned[lun])
        xlate_learn(lun);
    if (xlate_active(&xlate[lun]))
        block_size_mult = 1;    /* 512-byte blocks; see xlate_learn() */
#endif

    uint32_t bsize = block_size*block_size_mult;
    sector_t bcount = block_count/block_size_mult;

    cur_cmd.data_residue = letoh32(cbw->data_transfer_length);
    cur_cmd.tag = cbw->tag;
    cur_cmd.lun = lun;
    cur_cmd.cur_cmd = cbw->command_block[0];
    cs_last_op = cbw->command_block[0];
    ubot_wr_begin(&wr_fail);
#if defined(HAVE_USB_STATS) || defined(HAVE_USB_READ_AHEAD) \
    || defined(HAVE_USB_PREFETCH)
    usbstor_cmd_hook(cbw->command_block[0]);
#endif

    switch (cbw->command_block[0]) {
        case SCSI_TEST_UNIT_READY:
            logf("scsi test_unit_ready %d",lun);
            if(lun_present) {
                send_csw(UMS_STATUS_GOOD);
            }
            else {
                send_csw(UMS_STATUS_FAIL);
                cur_sense_data.sense_key=SENSE_NOT_READY;
                cur_sense_data.asc=ASC_MEDIUM_NOT_PRESENT;
                cur_sense_data.ascq=0;
            }
            break;

        case SCSI_REPORT_LUNS: {
            logf("scsi report luns %d",lun);
            unsigned int allocation_length=0;
            int i;
            unsigned int response_length = 8+8*storage_num_drives();
            allocation_length|=(cbw->command_block[6]<<24);
            allocation_length|=(cbw->command_block[7]<<16);
            allocation_length|=(cbw->command_block[8]<<8);
            allocation_length|=(cbw->command_block[9]);
            memset(tb.lun_data,0,sizeof(struct report_lun_data));
            tb.lun_data->lun_list_length=htobe32(8*storage_num_drives());
            for(i=0;i<storage_num_drives();i++)
            {
                if(storage_removable(i))
                    tb.lun_data->luns[i][1]=1;
                else
                    tb.lun_data->luns[i][1]=0;
            }
            length = MIN(length, allocation_length);
            send_command_result(tb.lun_data,
                                MIN(response_length, length));
            break;
        }

        case SCSI_INQUIRY: {
            unsigned int alloc = cbw->command_block[3] << 8 |
                                 cbw->command_block[4];
            logf("scsi inquiry %d",lun);
            if (cbw->command_block[1] & 1) {        /* EVPD */
                uint8_t vpd[8];
                unsigned int n = ubot_vpd_page(cbw->command_block[2], vpd);
                if (!n) {
                    fail_cmd(cbw, SENSE_ILLEGAL_REQUEST,
                             ASC_INVALID_FIELD_IN_CBD, 0);
                    break;
                }
                memcpy(tb.transfer_buffer, vpd, n);
                send_command_result(tb.transfer_buffer,
                                    ubot_clamp(length, alloc, n));
                break;
            }
            struct inquiry_data *inquiry = inquiry_reply_buffer();
            fill_inquiry(inquiry IF_MD(, lun));
            send_command_result(inquiry,
                    ubot_clamp(length, alloc, sizeof(struct inquiry_data)));
            break;
        }

        case SCSI_REQUEST_SENSE: {
            fill_sense();
            logf("scsi request_sense %d",lun);
            send_command_result(tb.sense_data,
                    ubot_clamp(length, cbw->command_block[4],
                               sizeof(struct sense_data)));
            break;
        }

        case SCSI_MODE_SENSE_10: {
            if(!lun_present) {
                send_command_failed_result();
                cur_sense_data.sense_key=SENSE_NOT_READY;
                cur_sense_data.asc=ASC_MEDIUM_NOT_PRESENT;
                cur_sense_data.ascq=0;
                break;
            }
            /*unsigned char pc = (cbw->command_block[2] & 0xc0) >>6;*/
            unsigned char page_code = cbw->command_block[2] & 0x3f;
            uint8_t pg[24];
            unsigned int pl = 0;
            logf("scsi mode_sense_10 %d %X",lun,page_code);
            if (cbw->command_block[3] && page_code != 0x3f)
                page_code = 0x7f;       /* no subpages: unsupported */
            if (page_code != 0x3f)
                pl = ubot_mode_page(page_code, pg);
            switch(page_code) {
                case 0x3f:
                case 0x08:
                case 0x1c:
                    tb.ms_data_10->mode_data_length =
                        htobe16(sizeof(struct mode_sense_data_10)-2+pl);
                    tb.ms_data_10->medium_type = 0;
                    tb.ms_data_10->device_specific =
                        LUN_READONLY(lun) ? 0x80 : 0;
                    tb.ms_data_10->reserved = 0;
                    tb.ms_data_10->longlba = 1;
                    tb.ms_data_10->block_descriptor_length =
                        htobe16(sizeof(struct mode_sense_bdesc_longlba));

                    memset(tb.ms_data_10->block_descriptor.reserved,0,4);

#ifdef STORAGE_64BIT_SECTOR
                    tb.ms_data_10->block_descriptor.num_blocks[2] =
                        (bcount & 0xff00000000ULL)>>40;
                    tb.ms_data_10->block_descriptor.num_blocks[3] =
                        (bcount & 0x00ff000000ULL)>>32;
#else
                    tb.ms_data_10->block_descriptor.num_blocks[2] = 0;
                    tb.ms_data_10->block_descriptor.num_blocks[3] = 0;
#endif
                    tb.ms_data_10->block_descriptor.num_blocks[4] =
                        (bcount & 0xff000000)>>24;
                    tb.ms_data_10->block_descriptor.num_blocks[5] =
                        (bcount & 0x00ff0000)>>16;
                    tb.ms_data_10->block_descriptor.num_blocks[6] =
                        (bcount & 0x0000ff00)>>8;
                    tb.ms_data_10->block_descriptor.num_blocks[7] =
                        (bcount & 0x000000ff);

                    tb.ms_data_10->block_descriptor.block_size[0] =
                        (bsize & 0xff000000)>>24;
                    tb.ms_data_10->block_descriptor.block_size[1] =
                        (bsize & 0x00ff0000)>>16;
                    tb.ms_data_10->block_descriptor.block_size[2] =
                        (bsize & 0x0000ff00)>>8;
                    tb.ms_data_10->block_descriptor.block_size[3] =
                        (bsize & 0x000000ff);
                    if (pl)
                        memcpy((uint8_t *)tb.ms_data_10 +
                               sizeof(struct mode_sense_data_10), pg, pl);
                    send_command_result(tb.ms_data_10,
                            ubot_clamp(length, cbw->command_block[7] << 8 |
                                       cbw->command_block[8],
                                       sizeof(struct mode_sense_data_10) +
                                       pl));
                    break;
                default:
                    fail_cmd(cbw, SENSE_ILLEGAL_REQUEST,
                             ASC_INVALID_FIELD_IN_CBD, 0);
                    break;
            }
            break;
        }

        case SCSI_MODE_SENSE_6: {
            if(!lun_present) {
                send_command_failed_result();
                cur_sense_data.sense_key=SENSE_NOT_READY;
                cur_sense_data.asc=ASC_MEDIUM_NOT_PRESENT;
                cur_sense_data.ascq=0;
                break;
            }
            /*unsigned char pc = (cbw->command_block[2] & 0xc0) >>6;*/
            unsigned char page_code = cbw->command_block[2] & 0x3f;
            uint8_t pg[24];
            unsigned int pl = 0;
            logf("scsi mode_sense_6 %d %X",lun,page_code);
            if (cbw->command_block[3] && page_code != 0x3f)
                page_code = 0x7f;       /* no subpages: unsupported */
            if (page_code != 0x3f)
                pl = ubot_mode_page(page_code, pg);
            switch(page_code) {
                case 0x3f:
                case 0x08:
                case 0x1c:
                    /* All supported pages. */
                    tb.ms_data_6->mode_data_length =
                        sizeof(struct mode_sense_data_6)-1+pl;
                    tb.ms_data_6->medium_type = 0;
                    tb.ms_data_6->device_specific =
                        LUN_READONLY(lun) ? 0x80 : 0;
                    tb.ms_data_6->block_descriptor_length =
                        sizeof(struct mode_sense_bdesc_shortlba);
                    tb.ms_data_6->block_descriptor.density_code = 0;
                    tb.ms_data_6->block_descriptor.reserved = 0;
                    if(bcount > 0xffffff) {
                        tb.ms_data_6->block_descriptor.num_blocks[0] = 0xff;
                        tb.ms_data_6->block_descriptor.num_blocks[1] = 0xff;
                        tb.ms_data_6->block_descriptor.num_blocks[2] = 0xff;
                    } else {
                        tb.ms_data_6->block_descriptor.num_blocks[0] =
                            (bcount & 0xff0000)>>16;
                        tb.ms_data_6->block_descriptor.num_blocks[1] =
                            (bcount & 0x00ff00)>>8;
                        tb.ms_data_6->block_descriptor.num_blocks[2] =
                            (bcount & 0x0000ff);
                    }
                    tb.ms_data_6->block_descriptor.block_size[0] =
                        (bsize & 0xff0000)>>16;
                    tb.ms_data_6->block_descriptor.block_size[1] =
                        (bsize & 0x00ff00)>>8;
                    tb.ms_data_6->block_descriptor.block_size[2] =
                        (bsize & 0x0000ff);
                    if (pl)
                        memcpy((uint8_t *)tb.ms_data_6 +
                               sizeof(struct mode_sense_data_6), pg, pl);
                    send_command_result(tb.ms_data_6,
                        ubot_clamp(length, cbw->command_block[4],
                                   sizeof(struct mode_sense_data_6) + pl));
                    break;
                default:
                    fail_cmd(cbw, SENSE_ILLEGAL_REQUEST,
                             ASC_INVALID_FIELD_IN_CBD, 0);
                    break;
            }
            break;
        }

        case SCSI_START_STOP_UNIT:
            logf("scsi start_stop unit %d",lun);
            if((cbw->command_block[4] & 0xf0) == 0) /*load/eject bit is valid*/
            { /* Process start and eject bits */
                logf("scsi load/eject");
                if((cbw->command_block[4] & 0x01) == 0) /* Don't start */
                {
                    /* The host stops or ejects the unit when it is done with
                     * it - "safely remove" - and may cut power next. Commit
                     * what the storage driver still holds in RAM, and the
                     * drive's own write cache. */
                    usbstor_flush_all();
                    if((cbw->command_block[4] & 0x02) != 0) /* eject */
                    {
                        logf("scsi eject");
                        ejected[lun]=true;
                    }
                }
            }
            send_csw(UMS_STATUS_GOOD);
            break;

        case SCSI_ALLOW_MEDIUM_REMOVAL:
            logf("scsi allow_medium_removal %d",lun);
            if((cbw->command_block[4] & 0x03) == 0) {
                locked[lun]=false;
                queue_broadcast(SYS_USB_LUN_LOCKED, (lun<<16)+0);
            }
            else {
                locked[lun]=true;
                queue_broadcast(SYS_USB_LUN_LOCKED, (lun<<16)+1);
            }
            send_csw(UMS_STATUS_GOOD);
            break;

        case SCSI_SYNCHRONIZE_CACHE_10:
            /* macOS sends it on sync and unmount; nothing is held back
             * here but the storage driver's own RAM (usbstor_pf_cmd()
             * drops the prefetch ring for it) */
            logf("scsi sync_cache %d",lun);
            if(!lun_present) {
                fail_cmd(cbw, SENSE_NOT_READY, ASC_MEDIUM_NOT_PRESENT, 0);
                break;
            }
            if (usbstor_flush_all() < 0) {
                fail_cmd(cbw, SENSE_MEDIUM_ERROR, ASC_WRITE_ERROR, 0);
                break;
            }
            send_csw(UMS_STATUS_GOOD);
            break;

        case SCSI_VERIFY_10:
            /* BYTCHK=0: the medium is trusted; BYTCHK=1 brings a data-out
             * phase that is not served */
            logf("scsi verify %d",lun);
            if(!lun_present)
                fail_cmd(cbw, SENSE_NOT_READY, ASC_MEDIUM_NOT_PRESENT, 0);
            else if(cbw->command_block[1] & 0x02)
                reject_command(cbw);
            else
                send_csw(UMS_STATUS_GOOD);
            break;

        case USB_UFI_READ_FORMAT_CAPACITY: {
            logf("usb ufi read_format_capacity %d",lun);
            if(lun_present) {
                tb.format_capacity_data->following_length=htobe32(8);
                /* "block count" actually means "number of last block" */
                if (bcount > 0xffffffff)
                    tb.format_capacity_data->block_count = 0xffffffff;
                else
                    tb.format_capacity_data->block_count = htobe32(bcount - 1);
                tb.format_capacity_data->block_size = htobe32(bsize);
                tb.format_capacity_data->block_size |=
                    htobe32(SCSI_FORMAT_CAPACITY_FORMATTED_MEDIA);

                send_command_result(tb.format_capacity_data,
                    MIN(sizeof(struct format_capacity), length));
           }
           else {
               send_command_failed_result();
               cur_sense_data.sense_key=SENSE_NOT_READY;
               cur_sense_data.asc=ASC_MEDIUM_NOT_PRESENT;
               cur_sense_data.ascq=0;
           }
           break;
        }

        case SCSI_READ_CAPACITY_10: {
            logf("scsi read_capacity10 %d",lun);

            if(lun_present) {
                /* "block count" actually means "number of last block" */
                if (bcount >= 0xffffffff) {
                    tb.capacity_data_10->block_count[0] = 0xff;
                    tb.capacity_data_10->block_count[1] = 0xff;
                    tb.capacity_data_10->block_count[2] = 0xff;
                    tb.capacity_data_10->block_count[3] = 0xff;
                } else {
                    tb.capacity_data_10->block_count[0] = ((bcount-1) & 0xff000000)>>24;
                    tb.capacity_data_10->block_count[1] = ((bcount-1) & 0x00ff0000)>>16;
                    tb.capacity_data_10->block_count[2] = ((bcount-1) & 0x0000ff00)>>8;
                    tb.capacity_data_10->block_count[3] = ((bcount-1) & 0x000000ff);
                }
                tb.capacity_data_10->block_size[0] = (bsize & 0xff000000)>>24;
                tb.capacity_data_10->block_size[1] = (bsize & 0x00ff0000)>>16;
                tb.capacity_data_10->block_size[2] = (bsize & 0x0000ff00)>>8;
                tb.capacity_data_10->block_size[3] = (bsize & 0x000000ff);

                send_command_result(tb.capacity_data_10,
                    MIN(sizeof(struct capacity_10), length));
            } else {
                send_command_failed_result();
                cur_sense_data.sense_key=SENSE_NOT_READY;
                cur_sense_data.asc=ASC_MEDIUM_NOT_PRESENT;
                cur_sense_data.ascq=0;
            }
            break;
        }
        case SCSI_READ_CAPACITY_16: {
            logf("scsi read_capacity16 %d",lun);

            if(lun_present) {
                unsigned int lbppbe = 0;
                /* 32 bytes, zeroed: the unused ones were stale buffer
                 * contents. "block count" is the number of the last
                 * block. */
                memset(tb.transfer_buffer, 0, 32);
#ifdef STORAGE_64BIT_SECTOR
                tb.capacity_data_16->block_count[2] =
                    ((bcount-1) & 0xff00000000ULL)>>40;
                tb.capacity_data_16->block_count[3] =
                    ((bcount-1) & 0x00ff000000ULL)>>32;
#endif
                tb.capacity_data_16->block_count[4] =
                    ((bcount-1) & 0xff000000)>>24;
                tb.capacity_data_16->block_count[5] =
                    ((bcount-1) & 0x00ff0000)>>16;
                tb.capacity_data_16->block_count[6] =
                    ((bcount-1) & 0x0000ff00)>>8;
                tb.capacity_data_16->block_count[7] =
                    ((bcount-1) & 0x000000ff);

                tb.capacity_data_16->block_size[0] = (bsize & 0xff000000)>>24;
                tb.capacity_data_16->block_size[1] = (bsize & 0x00ff0000)>>16;
                tb.capacity_data_16->block_size[2] = (bsize & 0x0000ff00)>>8;
                tb.capacity_data_16->block_size[3] = (bsize & 0x000000ff);

                /* byte 12: no protection; byte 13: logical blocks per
                 * physical block, as a 0-based exponent (the host block
                 * already includes block_size_mult, so only the 512-byte
                 * view of a 2048-byte layout has more than one); bytes
                 * 14-15: the lowest aligned LBA, 0 */
#ifdef HAVE_USB_SECTOR_XLATE
                if (xlate_active(&xlate[lun]))
                    lbppbe = find_first_set_bit(xlate[lun].mult);
#endif
                tb.capacity_data_16->flags[1] = lbppbe & 0x0f;

                send_command_result(tb.capacity_data_16,
                    ubot_clamp(length, cbw->command_block[10] << 24 |
                               cbw->command_block[11] << 16 |
                               cbw->command_block[12] << 8 |
                               cbw->command_block[13], 32));
            } else {
                send_command_failed_result();
                cur_sense_data.sense_key=SENSE_NOT_READY;
                cur_sense_data.asc=ASC_MEDIUM_NOT_PRESENT;
                cur_sense_data.ascq=0;
            }
            break;
        }
        case SCSI_READ_10:
            logf("scsi read10 %d",lun);
            if(!lun_present) {
                fail_cmd(cbw, SENSE_NOT_READY, ASC_MEDIUM_NOT_PRESENT, 0);
                break;
            }
            cur_cmd.data[0] = USBSTOR_BLK;
            cur_cmd.data[1] = &USBSTOR_BLK[READ_BUFFER_SIZE];
            cur_cmd.data_select=0;
            cur_cmd.read_pending=false;
            cur_cmd.sector = block_size_mult *
                (cbw->command_block[2] << 24 |
                 cbw->command_block[3] << 16 |
                 cbw->command_block[4] << 8  |
                 cbw->command_block[5] );
            cur_cmd.count = block_size_mult *
                (cbw->command_block[7] << 8 |
                 cbw->command_block[8]);
            cur_cmd.block_size = block_size;

            logf("scsi read %llu %d", cur_cmd.sector, cur_cmd.count);

            if(ubot_zero_len(cur_cmd.count)) {
                /* no data phase at all */
                csw_no_data(cbw, UMS_STATUS_GOOD);
                break;
            }
            if((cur_cmd.sector + cur_cmd.count) > block_count) {
                fail_cmd(cbw, SENSE_ILLEGAL_REQUEST, ASC_LBA_OUT_OF_RANGE, 0);
            }
            else {
#ifdef USB_USE_RAMDISK
                memcpy(cur_cmd.data[cur_cmd.data_select],
                        ramdisk_buffer + cur_cmd.sector*cur_cmd.block_size,
                        MIN(READ_BUFFER_SIZE/cur_cmd.block_size,cur_cmd.count)*cur_cmd.block_size);
#else
#ifdef HAVE_USB_STATS
                ustats_read_cmd(cur_cmd.sector, cur_cmd.count);
#endif
#ifdef HAVE_USB_PREFETCH
                if (usbstor_pf_begin(block_count))
                    break;
#endif
#ifdef HAVE_USB_READ_AHEAD
                if (!usbstor_ra_begin(block_count))
#endif
                cur_cmd.last_result = storage_read_sectors(IF_MD(cur_cmd.lun,)
                        cur_cmd.sector,
                        MIN(READ_BUFFER_SIZE/cur_cmd.block_size, cur_cmd.count),
                        cur_cmd.data[cur_cmd.data_select]);
#endif
                send_and_read_next();
            }
            break;
#ifdef STORAGE_64BIT_SECTOR
        case SCSI_READ_16:
            logf("scsi read16 %d",lun);
            if(!lun_present) {
                fail_cmd(cbw, SENSE_NOT_READY, ASC_MEDIUM_NOT_PRESENT, 0);
                break;
            }
            cur_cmd.data[0] = USBSTOR_BLK;
            cur_cmd.data[1] = &USBSTOR_BLK[READ_BUFFER_SIZE];
            cur_cmd.data_select=0;
            cur_cmd.read_pending=false;
            cur_cmd.sector = block_size_mult *
                 ((uint64_t)cbw->command_block[2] << 56 |
                 (uint64_t)cbw->command_block[3] << 48 |
                 (uint64_t)cbw->command_block[4] << 40 |
                 (uint64_t)cbw->command_block[5] << 32 |
                 cbw->command_block[6] << 24 |
                 cbw->command_block[7] << 16 |
                 cbw->command_block[8] << 8  |
                 cbw->command_block[9]);
            cur_cmd.count = block_size_mult *
                (cbw->command_block[10] << 24 |
                 cbw->command_block[11] << 16 |
                 cbw->command_block[12] << 8 |
                 cbw->command_block[13]);
            cur_cmd.block_size = block_size;

            logf("scsi read %llu %d", cur_cmd.sector, cur_cmd.count);

            if(ubot_zero_len(cur_cmd.count)) {
                /* no data phase at all */
                csw_no_data(cbw, UMS_STATUS_GOOD);
                break;
            }
            if((cur_cmd.sector + cur_cmd.count) > block_count) {
                fail_cmd(cbw, SENSE_ILLEGAL_REQUEST, ASC_LBA_OUT_OF_RANGE, 0);
            }
            else {
#ifdef USB_USE_RAMDISK
                memcpy(cur_cmd.data[cur_cmd.data_select],
                        ramdisk_buffer + cur_cmd.sector*cur_cmd.block_size,
                        MIN(READ_BUFFER_SIZE/cur_cmd.block_size,cur_cmd.count)*cur_cmd.block_size);
#else
#ifdef HAVE_USB_STATS
                ustats_read_cmd(cur_cmd.sector, cur_cmd.count);
#endif
#ifdef HAVE_USB_PREFETCH
                if (usbstor_pf_begin(block_count))
                    break;
#endif
#ifdef HAVE_USB_READ_AHEAD
                if (!usbstor_ra_begin(block_count))
#endif
                cur_cmd.last_result = storage_read_sectors(IF_MD(cur_cmd.lun,)
                        cur_cmd.sector,
                        MIN(READ_BUFFER_SIZE/cur_cmd.block_size, cur_cmd.count),
                        cur_cmd.data[cur_cmd.data_select]);
#endif
                send_and_read_next();
            }
            break;
#endif
        case SCSI_WRITE_10:
            logf("scsi write10 %d",lun);
            if(!lun_present) {
                fail_cmd(cbw, SENSE_NOT_READY, ASC_MEDIUM_NOT_PRESENT, 0);
                break;
            }
            if(LUN_READONLY(lun)) {
                fail_cmd(cbw, SENSE_DATA_PROTECT, ASC_WRITE_PROTECTED, 0);
                break;
            }
            cur_cmd.data[0] = USBSTOR_BLK;
            cur_cmd.data[1] = &USBSTOR_BLK[WRITE_BUFFER_SIZE];
            cur_cmd.data_select=0;
            cur_cmd.sector = block_size_mult *
                (cbw->command_block[2] << 24 |
                 cbw->command_block[3] << 16 |
                 cbw->command_block[4] << 8  |
                 cbw->command_block[5] );
            cur_cmd.count = block_size_mult *
                (cbw->command_block[7] << 8 |
                 cbw->command_block[8]);
            cur_cmd.block_size = block_size;

            if(ubot_zero_len(cur_cmd.count)) {
                /* no data phase at all */
                csw_no_data(cbw, UMS_STATUS_GOOD);
                break;
            }
            /* expect data */
            if((cur_cmd.sector + cur_cmd.count) > block_count) {
                fail_cmd(cbw, SENSE_ILLEGAL_REQUEST, ASC_LBA_OUT_OF_RANGE, 0);
            }
            else {
                receive_block_data(cur_cmd.data[0],
                        MIN(WRITE_BUFFER_SIZE, cur_cmd.count*cur_cmd.block_size));
            }
            break;
#ifdef STORAGE_64BIT_SECTOR
        case SCSI_WRITE_16:
            logf("scsi write16 %d",lun);
            if(!lun_present) {
                fail_cmd(cbw, SENSE_NOT_READY, ASC_MEDIUM_NOT_PRESENT, 0);
                break;
            }
            if(LUN_READONLY(lun)) {
                fail_cmd(cbw, SENSE_DATA_PROTECT, ASC_WRITE_PROTECTED, 0);
                break;
            }
            cur_cmd.data[0] = USBSTOR_BLK;
            cur_cmd.data[1] = &USBSTOR_BLK[WRITE_BUFFER_SIZE];
            cur_cmd.data_select=0;
            cur_cmd.sector = block_size_mult *
                ((uint64_t)cbw->command_block[2] << 56 |
                 (uint64_t)cbw->command_block[3] << 48 |
                 (uint64_t)cbw->command_block[4] << 40 |
                 (uint64_t)cbw->command_block[5] << 32 |
                 cbw->command_block[6] << 24 |
                 cbw->command_block[7] << 16 |
                 cbw->command_block[8] << 8  |
                 cbw->command_block[9]);
            cur_cmd.count = block_size_mult *
                (cbw->command_block[10] << 24 |
                 cbw->command_block[11] << 16 |
                 cbw->command_block[12] << 8 |
                 cbw->command_block[13]);
            cur_cmd.block_size = block_size;

            if(ubot_zero_len(cur_cmd.count)) {
                /* no data phase at all */
                csw_no_data(cbw, UMS_STATUS_GOOD);
                break;
            }
            /* expect data */
            if((cur_cmd.sector + cur_cmd.count) > block_count) {
                fail_cmd(cbw, SENSE_ILLEGAL_REQUEST, ASC_LBA_OUT_OF_RANGE, 0);
            }
            else {
                receive_block_data(cur_cmd.data[0],
                        MIN(WRITE_BUFFER_SIZE, cur_cmd.count*cur_cmd.block_size));
            }
            break;
#endif

#if CONFIG_RTC
        case SCSI_WRITE_BUFFER:
            if(cbw->command_block[1]==1 /* mode = vendor specific */
            && cbw->command_block[2]==0 /* buffer id = 0 */

            && cbw->command_block[3]==0x0c /* offset (3 bytes) */
            && cbw->command_block[4]==0
            && cbw->command_block[5]==0

            /* Some versions of itunes set the parameter list length to 0.
             * Technically it should be 0x0c, which is what libgpod sends */
            && cbw->command_block[6]==0 /* parameter list (3 bytes) */
            && cbw->command_block[7]==0
            && (cbw->command_block[8]==0 || cbw->command_block[8]==0x0c)

            && cbw->command_block[9]==0)
                receive_time();
            else
                reject_command(cbw);
            break;
#endif /* CONFIG_RTC */

#if (CONFIG_STORAGE & STORAGE_ATA)
    case SAT_ATA_PASSTHROUGH_12:
            if (cbw->command_block[1] == 0x08 &&
                cbw->command_block[2] == 0x0e &&
                cbw->command_block[4] == 0x01) {
                if (cbw->command_block[9] == 0xa1 ||
                    cbw->command_block[9] == 0xec) {
                    usb_storage_send_ata_identify();
                    break;
#ifdef HAVE_ATA_SMART
                } else if (cbw->command_block[9] == 0xb0) {
                    usb_storage_send_smart(cbw->command_block[3]);
                    break;
#endif
                }
            }
            send_csw(UMS_STATUS_FAIL);
            cur_sense_data.sense_key=SENSE_ILLEGAL_REQUEST;
            cur_sense_data.asc=ASC_INVALID_COMMAND;
            cur_sense_data.ascq=0;
            break;
    case SAT_ATA_PASSTHROUGH_16:
            if (cbw->command_block[1] == 0x08 &&
                cbw->command_block[2] == 0x0e &&
                cbw->command_block[6] == 0x01) {
                if (cbw->command_block[14] == 0xa1 ||
                    cbw->command_block[14] == 0xec) {
                    usb_storage_send_ata_identify();
                    break;
#ifdef HAVE_ATA_SMART
                } else if (cbw->command_block[14] == 0xb0) {
                    usb_storage_send_smart(cbw->command_block[4]);
                    break;
#endif
                }
            }
            send_csw(UMS_STATUS_FAIL);
            cur_sense_data.sense_key=SENSE_ILLEGAL_REQUEST;
            cur_sense_data.asc=ASC_INVALID_COMMAND;
            cur_sense_data.ascq=0;
            break;
#endif /* STORAGE_ATA */
        default:
            logf("scsi unknown cmd %x",cbw->command_block[0x0]);
            reject_command(cbw);
            break;
    }
}

/* SET_CONFIGURATION now precedes the storage handover. Keep the first
 * CBW pending until all Rockbox clients have acknowledged/unmounted. This
 * avoids falsely reporting an absent disk to a fast host and, importantly,
 * prevents READ/WRITE commands from racing Rockbox filesystem users. The
 * OUT endpoint is not rearmed until this CBW has received its CSW. */
static void handle_scsi(struct command_block_wrapper* cbw)
{
    if(!usb_exclusive_storage()) {
        enum ubot_early e = ubot_early_cmd(cbw->command_block[0]);

        if(e != UBOT_EARLY_HOLD && letoh32(cbw->signature) == CBW_SIGNATURE) {
            /* Not the storage's yet: answer what touches none of it, so a
             * host that polls TEST UNIT READY is told "becoming ready"
             * instead of waiting out its command timeout. Anything else
             * is held until the handover. */
            unsigned int dtl = letoh32(cbw->data_transfer_length);

            cbw->signature = 0;
            cur_cmd.data_residue = dtl;
            cur_cmd.tag = cbw->tag;
            cur_cmd.cur_cmd = cbw->command_block[0];
            cs_last_op = cbw->command_block[0];
            if(e == UBOT_EARLY_TUR) {
                fail_cmd(cbw, SENSE_NOT_READY, ASC_NOT_READY,
                         ASCQ_BECOMING_READY);
            } else {
                fill_sense();
                send_command_result(tb.sense_data,
                        ubot_clamp(dtl, cbw->command_block[4],
                                   sizeof(struct sense_data)));
            }
            return;
        }
#ifdef HAVE_USB_STATS
        hold_n++;               /* each CBW held, across resets */
        if (!hold_since)        /* the wait runs from the first one */
            hold_since = current_tick | 1;
#endif
        state = WAITING_FOR_STORAGE;
        return;
    }
#ifdef HAVE_USB_STATS
    if (hold_since) {
        if (current_tick - hold_since > hold_max)
            hold_max = current_tick - hold_since;
        hold_since = 0;
    }
#endif
    handle_scsi_ready(cbw);
}

static void usb_storage_notify_event(intptr_t data)
{
    (void)data;
    if(state == WAITING_FOR_STORAGE && usb_exclusive_storage())
        handle_scsi((struct command_block_wrapper*)cbw_buffer);
}

/* Count data only, never the CBW or CSW. A short result must report the
 * untransferred bytes instead of claiming that the whole request succeeded. */
static void account_data_transfer(int dir, int status, int length)
{
    bool data_in = state == SENDING_RESULT || state == SENDING_FAILED_RESULT ||
                   state == SENDING_BLOCKS;
    bool data_out = state == RECEIVING_BLOCKS;
#if CONFIG_RTC
    data_out |= state == RECEIVING_TIME;
#endif
    if(status == 0 && length > 0 &&
       ((data_in && dir == USB_DIR_IN) || (data_out && dir == USB_DIR_OUT)))
        cur_cmd.data_residue -= MIN(cur_cmd.data_residue, (unsigned int)length);
}

/* BOT cases Hi/Dn and Ho/Dn still have a data phase. Sending the CSW as
 * IN data makes the host wait for a second status that will never arrive. */
static void reject_command(const struct command_block_wrapper *cbw)
{
    fail_cmd(cbw, SENSE_ILLEGAL_REQUEST, ASC_INVALID_COMMAND, 0);
}

/* The CSW of a command that has no data phase to run (it failed first, or
 * moves 0 blocks). When the CBW announced data, the data pipe is halted
 * first; the host clears it before reading the CSW (BOT 6.7.2 cases 4 and
 * 10, 6.7.3 case 9), and the CSW queues behind the halt. The CBW's fields
 * are read before send_csw() primes the next CBW into the same buffer. */
static void csw_no_data(const struct command_block_wrapper *cbw, int status)
{
    bool in = (cbw->flags & USB_DIR_IN) != 0;

    switch(ubot_err_stall(letoh32(cbw->data_transfer_length), in)) {
        case UBOT_STALL_IN:
            usb_drv_stall(EP_IN, true, true);
            cs_count(1);
            break;
        case UBOT_STALL_OUT:
            usb_drv_stall(EP_OUT, true, false);
            cs_count(1);
            break;
        default:
            break;
    }
    send_csw(status);
}

static void fail_cmd(const struct command_block_wrapper *cbw, int key,
                     int asc, int ascq)
{
    cur_sense_data.sense_key = key;
    cur_sense_data.information = 0;
    cur_sense_data.asc = asc;
    cur_sense_data.ascq = ascq;
    csw_no_data(cbw, UMS_STATUS_FAIL);
}

static void send_block_data(void *data,int size)
{
    /* usb_drv_send_nonblocking() enables IRQs before it returns.  At high
     * speed a short BOT response can therefore complete immediately; publish
     * the state first so that completion always queues the next phase. */
    state = SENDING_BLOCKS;
    usb_drv_send_nonblocking(EP_IN, data,size);
}

static void send_command_result(void *data,int size)
{
    if(cur_cmd.data_residue == 0) {
        /* the host expects no data (dCBWDataTransferLength 0): a zero-length
         * packet would be read as the CSW */
        send_csw(UMS_STATUS_GOOD);
        return;
    }
    state = SENDING_RESULT;
    usb_drv_send_nonblocking(EP_IN, data,size);
}

static void send_command_failed_result(void)
{
    if(cur_cmd.data_residue == 0) {
        send_csw(UMS_STATUS_FAIL);
        return;
    }
    state = SENDING_FAILED_RESULT;
    usb_drv_send_nonblocking(EP_IN, NULL, 0);
}

#if CONFIG_RTC
static void receive_time(void)
{
    usb_drv_recv_nonblocking(EP_OUT, tb.transfer_buffer, 12);
    state = RECEIVING_TIME;
}
#endif /* CONFIG_RTC */

static void receive_block_data(void *data,int size)
{
    usb_drv_recv_nonblocking(EP_OUT, data, size);
    state = RECEIVING_BLOCKS;
}

static void send_csw(int status)
{
    tb.csw->signature = htole32(CSW_SIGNATURE);
    tb.csw->tag = cur_cmd.tag;
    tb.csw->data_residue = htole32(cur_cmd.data_residue);
    tb.csw->status = status;
    if (status != UMS_STATUS_GOOD) {
        unsigned char keep = cs_last_op;
        cs_last_op = cur_cmd.cur_cmd;
        cs_count(0);
        cs_last_op = keep;
    }

    state = WAITING_FOR_CSW_COMPLETION_OR_COMMAND;
    usb_drv_send_nonblocking(EP_IN, tb.csw,
            sizeof(struct command_status_wrapper));
    //logf("CSW: %X",status);
    /* Already start waiting for the next command */
    usb_drv_recv_nonblocking(EP_OUT, cbw_buffer, MAX_CBW_SIZE);
    /* The next completed transfer will be either the CSW one
     * or the new command */

    if(status == UMS_STATUS_GOOD) {
        cur_sense_data.sense_key=0;
        cur_sense_data.information=0;
        cur_sense_data.asc=0;
        cur_sense_data.ascq=0;
    }
}

static void copy_padded(char *dest, char *src, int len)
{
    for (int i = 0; i < len; i++) {
        if (src[i] == '\0') {
            memset(&dest[i], ' ', len - i);
            return;
        }
        dest[i] = src[i];
    }
}

/* build SCSI INQUIRY */
static void fill_inquiry(struct inquiry_data *inquiry IF_MD(, int lun))
{
    struct storage_info info;
    memset(inquiry, 0, sizeof(*inquiry));
    storage_get_info(lun,&info);
    copy_padded(inquiry->VendorId,info.vendor,sizeof(inquiry->VendorId));
    copy_padded(inquiry->ProductId,info.product,sizeof(inquiry->ProductId));
    copy_padded(inquiry->ProductRevisionLevel,info.revision,sizeof(inquiry->ProductRevisionLevel));

    inquiry->DeviceType = DIRECT_ACCESS_DEVICE;
    inquiry->AdditionalLength = 0x1f;
//    memset(tb.inquiry->Reserved, 0, sizeof(tb.inquiry->Reserved)); // Redundant
    inquiry->Versions = 4; /* SPC-2 */
    inquiry->Format = 2; /* SPC-2/3 inquiry format */

    inquiry->DeviceTypeModifier = DEVICE_REMOVABLE;
}

struct usb_class_driver usb_cdrv_storage = {
    .needs_exclusive_storage = true,
    .needs_cpu_boost = true,
    .config = 1,
    .ep_allocs_size = ARRAYLEN(ep_allocs),
    .ep_allocs = ep_allocs,
    .set_first_interface = usb_storage_set_first_interface,
    .get_config_descriptor = usb_storage_get_config_descriptor,
    .init_connection = usb_storage_init_connection,
    .init = usb_storage_init,
    .disconnect = usb_storage_disconnect,
    .transfer_complete = usb_storage_transfer_complete,
    .control_request = usb_storage_control_request,
    .notify_event = usb_storage_notify_event,
#ifdef HAVE_HOTSWAP
    .notify_hotswap = usb_storage_notify_hotswap,
#endif
};
