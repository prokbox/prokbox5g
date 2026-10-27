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

/* tests/mpainline: the codec API slice mpa.c and libmad use */
#ifndef STUB_CODECLIB_H
#define STUB_CODECLIB_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <pthread.h>
#include "config.h"

#define CODEC_HEADER
#define betoh32(x)      __builtin_bswap32(x)   /* a little-endian host */
#define TIMEOUT_BLOCK   -1
#define IF_PRIO(...)    __VA_ARGS__
#define IF_COP(...)     __VA_ARGS__
#define PRIORITY_PLAYBACK 16
#define COP             1

enum codec_status { CODEC_OK = 0, CODEC_ERROR = -1 };
enum codec_entry_call_reason { CODEC_LOAD = 0, CODEC_UNLOAD };
enum { CODEC_ACTION_HALT = -1, CODEC_ACTION_NULL = 0,
       CODEC_ACTION_SEEK_TIME = 1 };
enum { DSP_SET_FREQUENCY = 1, DSP_SET_SAMPLE_DEPTH, DSP_SET_STEREO_MODE };
enum { STEREO_INTERLEAVED = 0, STEREO_NONINTERLEAVED, STEREO_MONO };

struct mp3entry {
    unsigned long frequency, length, elapsed, offset, bitrate;
    unsigned long first_frame_offset, filesize;
    unsigned long id3v1len;
    int layer, lead_trim, tail_trim, id;
    bool vbr, has_toc, is_asf_stream;
    unsigned char toc[100];
};

struct semaphore {
    pthread_mutex_t m;
    pthread_cond_t c;
    int count, max;
};

struct codec_api {
    off_t filesize;
    off_t curpos;
    struct mp3entry *id3;
    void (*pcmbuf_insert)(const void *ch1, const void *ch2, int count);
    void (*set_elapsed)(unsigned long value);
    void * (*request_buffer)(size_t *realsize, size_t reqsize);
    void (*advance_buffer)(size_t amount);
    bool (*seek_buffer)(size_t newpos);
    void (*seek_complete)(void);
    void (*set_offset)(size_t value);
    void (*configure)(int setting, intptr_t value);
    long (*get_command)(intptr_t *param);
    void (*strip_filesize)(off_t value);
    void *(*memset)(void *s, int c, size_t n);
    unsigned int (*create_thread)(void (*fn)(void), void *stack,
                                  size_t size, unsigned flags,
                                  const void *name, int prio, int core);
    void (*thread_wait)(unsigned int id);
    void (*semaphore_init)(struct semaphore *s, int max, int start);
    int  (*semaphore_wait)(struct semaphore *s, int timeout);
    void (*semaphore_release)(struct semaphore *s);
    void (*commit_discard_dcache)(void);
    bool cop_assist;
};
extern struct codec_api *ci;

int codec_init(void);
void codec_set_replaygain(const struct mp3entry *id3);
enum codec_status codec_main(enum codec_entry_call_reason reason);
enum codec_status codec_run(void);
#endif
