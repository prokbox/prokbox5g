/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2025 by Sho Tanimoto
 * Modified for ProkBox5g (2026); see docs/changes-from-rockbox.md.
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
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "config.h"

#define PCM_SINK_HWVOL 0
#ifdef WANT_SWVOL
#define PCM_SINK_SWVOL 1
#endif

struct pcm_sink_caps {
    const unsigned long* samprs;
    uint8_t              num_samprs;
    uint8_t              default_freq;
    uint8_t              volume_type;
};

struct pcm_sink_ops {
    void (*init)(void);
    void (*postinit)(void);
    void (*set_freq)(uint16_t freq);
    void (*lock)(void);
    void (*unlock)(void);
    void (*play)(const void* addr, size_t size);
    void (*stop)(void);
};

struct pcm_sink {
    /* characteristics */
    struct pcm_sink_caps caps;

    /* operations */
    const struct pcm_sink_ops ops;

    /* runtime states */
    unsigned long pending_freq;
    unsigned long configured_freq;
};

enum pcm_sink_ids {
    PCM_SINK_BUILTIN = 0,
#ifdef USB_ENABLE_IAP
    PCM_SINK_IAP,
#endif
    PCM_SINK_NUM
};

/* defined in each platform pcm source */
extern struct pcm_sink builtin_pcm_sink;

#ifdef HAVE_PCM_24BIT_OUT
/* ProkBox #268: the built-in sink's frame format.  16: 4-byte frames (two
 * int16, left first); 24: 8-byte frames (two int32, left first, the 24
 * bits at the top, the low byte 0).  Only while the PCM is stopped: the
 * mixer sets it at its start (pcm_mixer.c).  CPU */
void pcm_builtin_set_depth(int bits);
/* Playback DMA pieces moved since boot (any core's FIQ; one writer per
 * piece, uncached): the high-rate stall watch's progress word */
unsigned long pcm_builtin_progress(void);
#endif

#if defined(CPU_PP) && (CONFIG_PLATFORM & PLATFORM_NATIVE)
/* ProkBox #312: the built-in sink's bounded waits (pcm-pp.c).  A wait for
 * the I2S FIFO to drain, or for a stopped playback channel to go idle,
 * that reaches its bound is counted here; the stop completes anyway.
 * what: PCM_WAIT_FIFO, or the channels waited for (PCM_WAIT_CH0/1) */
#define PCM_WAIT_FIFO   0
#define PCM_WAIT_CH0    1
#define PCM_WAIT_CH1    2
struct pcm_wait_fault
{
    unsigned long count;        /* bounds reached since boot */
    unsigned long fifo;         /* IISFIFO_CFG at the last one */
    unsigned long dma;          /* its channels' status (0: the FIFO's) */
    unsigned long us;           /* the time it took, both tries */
    unsigned char what;
    unsigned char recovered;    /* the FIFO cleared, the wait then ended */
};
/* Any context; a copy (one writer at a time: the PCM lock or the FIQ) */
void pcm_builtin_wait_fault(struct pcm_wait_fault *out);
#if defined(HAVE_IPOD_UI) && !defined(BOOTLOADER)
#define HAVE_PCM_WAIT_REPORT
/* pcm.c: one line through log for each bound reached, from the next PCM
 * start or stop (thread context); NULL to stop */
void pcm_set_fault_log(void (*log)(const char *fmt, ...));
#endif
#endif
