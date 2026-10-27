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
 * bcmfw DMA: the LCD frame push by the DMA engine, paced by the LCD
 * interface's request line, instead of CPU writes.  Register layout and
 * field meanings: docs/research/bcm2722/hardware.md ("DMA engine", "How
 * Apple sends a frame"); confidence is noted per field.  The code is ours.
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
#ifndef BCMFW_DMA_H
#define BCMFW_DMA_H

#include "config.h"

/* ---- Switches (until a device run confirms the DMA path) ------------- */

/* 0: pixels go out by CPU writes (push.S).  1: the DMA engine feeds the
 * LCD FIFO; on a timeout the frame is re-sent by the CPU and DMA stays
 * off until the next power-up. */
#ifndef BCMFW_LCD_DMA
#define BCMFW_LCD_DMA        0
#endif
/* Give up on a transfer after this long (a full frame at 10 Mtransfers/s
 * takes 7.7 ms; lcd-video.c re-kicks after 50 ms anyway). */
#ifndef BCMFW_DMA_TIMEOUT_US
#define BCMFW_DMA_TIMEOUT_US 100000
#endif
/* Poll the completion bit (DMA_INT_LCD) this often while a transfer runs */
#ifndef BCMFW_DMA_POLL_US
#define BCMFW_DMA_POLL_US    64
#endif
/* Channel and sub-channel for the LCD.  Channel 1 is the one Apple's
 * firmware gives the LCD [H]; the sub-channel is picked at run time
 * there, so 0 is our choice [L]. */
#define DMA_LCD_CHAN         1
#define DMA_LCD_SUB          0

/* 1: read the LCD interface's control and run-length registers back
 * after arming and at the end of each push (mailbox 0x198/0x19C), to
 * see whether the run length counts down as pixels go out [L] */
#ifndef BCMFW_DMA_READBACK
#define BCMFW_DMA_READBACK   1
#endif
/* After a timed-out push the CPU re-sends the rectangle, then paints its
 * first BCMFW_DMA_MARK rows black, so a photo tells a CPU re-send from a
 * DMA frame (0: no mark) */
#ifndef BCMFW_DMA_MARK
#define BCMFW_DMA_MARK       8
#endif

/* Info word bits (mailbox 0x1E8; README.md lists the others) */
#define BCMFW_INFO_DMA       0x0800  /* built with BCMFW_LCD_DMA */
#define BCMFW_INFO_DMA_OFF   0x1000  /* a DMA timeout switched to the CPU */

/* ---- Registers --------------------------------------------------------- */

#define DMA_BASE             0x10000800
#define DMA_CHAN(n)          (DMA_BASE + 0x80 * (n))
#define DMA_CS(n)            (DMA_CHAN(n) + 0x00)  /* config and state */
#define DMA_INT(n)           (DMA_CHAN(n) + 0x04)  /* status, write 1 to clear [M] */
#define DMA_CBADDR(n, k)     (DMA_CHAN(n) + 0x38 + 0x20 * (k))  /* start [H] */

/* Channel config: bit 0 enable [M]; bits 20:16 the request line [M].
 * The other bits are the value Apple writes for the LCD channel, kept
 * as is because their meaning is unknown [L]. */
#define DMA_CS_ENABLE        0x00000001
#define DMA_CS_DREQ(d)       ((d) << 16)
#define DMA_CS_LCD_OTHER     0x09002100
#define DMA_DREQ_LCD         3
/* Bits 31:29 read back a state: 2 once our frame had gone (device
 * session 4), the value Apple's TV poller waits for [M] */
#define DMA_CS_STATE(v)      ((v) >> 29)

/* Status word (+0x04).  Apple's handler (@7b7c) acknowledges the whole
 * word and calls each registered user whose mask meets it: the LCD push
 * registers 0x100 (@145e2), the generic transfers 0x800 | 1 << (13 + k)
 * for sub-channel k (@804c) [H masks].  Session 4 read 0x2300 after our
 * frame: bits 8, 9 and 13 [H]. */
#define DMA_INT_LCD          0x00000100  /* the LCD run finished: Apple's
                                            completion for a frame [H] */
#define DMA_INT_SUB(k)       (1 << (13 + (k)))  /* sub k's chain ended [M] */
#define DMA_INT_ALL          0xFFFFFFFF

/* Transfer info (control block +16) */
#define TI_ON                0x00000001  /* always set [L] */
#define TI_SRC_WIDTH(l)      ((l) << 4)  /* log2 bytes per read [H] */
#define TI_DST_WIDTH(l)      ((l) << 12) /* log2 bytes per write [H] */
#define TI_DST_DREQ          0x00008000  /* destination paced [M] */
#define TI_2D                0x00010000  /* [H] */
#define TI_DST_INC           0x00020000  /* destination increments [M] */
#define TI_INTEN             0x01000000  /* cleared on all but the last
                                            block of Apple's chains [M] */
#define TI_UNCACHED          0x08000000  /* [M] */
#define TI_PERIPH            0x10000000  /* set on peripheral targets [L] */
/* LCD FIFO target: fixed 32-bit destination, paced */
#define TI_LCD               (TI_ON | TI_DST_WIDTH(2) | TI_DST_DREQ | TI_INTEN \
                              | TI_UNCACHED | TI_PERIPH)

/* LCD interface run registers (hardware.md "LCD", "Controller") */
#define LCD_RUNLEN           0x10004004  /* bus transfers in the run [M] */
#define LCD_RUNFMT           0x10004008  /* run format [L] */
#define LCD_FIFO             0x1000400C  /* DMA destination [H] */
#define LCD_RUN_FMT_VALUE    0x1F
#define LCD_RUN_PREPARE      0x231       /* | burst << 8 */
#define LCD_RUN_GO           0x229       /* | burst << 8 */

#ifndef __ASSEMBLER__
/* Control block: 32 bytes, 32-byte aligned [H] */
struct dma_cb
{
    uint32_t next;      /* next block, or 0 */
    uint32_t src;
    uint32_t dst;
    uint32_t len;       /* units; 2-D: rows << 16 | units per row */
    uint32_t ti;
    uint32_t stride;    /* 2-D skips in units: dst << 16 | src [H] */
    uint32_t pad[2];
};

/* Send w x h pixels starting at src (rows pitch_px apart) to the LCD,
 * whose window and RAM-write command are already set.  Returns 1 when
 * the DMA engine sent them; 0 when the caller must send them by CPU
 * (DMA off, an odd width or address); -1 after a timeout (nothing or
 * part of the rectangle was sent: the caller resets the window, sends
 * it by CPU and marks it, BCMFW_DMA_MARK). */
int dma_lcd_push(const uint16_t *src, uint32_t w, uint32_t h, uint32_t pitch_px);
/* The same in two halves (the video path overlaps the transfer with
 * decoding): start returns 1 if the transfer is running (0: send by
 * CPU, nothing was sent); wait returns 1 when it has finished, -1 after
 * a timeout (then part of it was sent and DMA is off from now on). */
int dma_lcd_start(const uint16_t *src, uint32_t w, uint32_t h, uint32_t pitch_px);
int dma_lcd_wait(void);
/* Info word bits for the DMA path */
uint32_t dma_info(void);
/* Both exist only in a BCMFW_LCD_DMA build: the CPU-push image is
 * unchanged byte for byte. */
#endif

#endif /* BCMFW_DMA_H */
