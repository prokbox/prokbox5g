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

/* tests/codecplace: the fields of struct codec_api that apps/codec_cop.c
 * names (lib/rbcodec/codecs/codecs.h has the rest) */
#ifndef STUB_CODECS_H
#define STUB_CODECS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "config.h"
struct mp3entry;
struct dsp_config;
enum { CODEC_ACTION_HALT = -1, CODEC_ACTION_NULL = 0 };
struct codec_api {
    off_t filesize;
    off_t curpos;
    struct mp3entry *id3;
    int audio_hid;
    struct dsp_config *dsp;
    void * (*codec_get_buffer)(size_t *size);
    void (*pcmbuf_insert)(const void *ch1, const void *ch2, int count);
    void (*set_elapsed)(unsigned long value);
    size_t (*read_filebuf)(void *ptr, size_t size);
    void * (*request_buffer)(size_t *realsize, size_t reqsize);
    void (*advance_buffer)(size_t amount);
    bool (*seek_buffer)(size_t newpos);
    void (*seek_complete)(void);
    void (*set_offset)(size_t value);
    void (*configure)(int setting, intptr_t value);
    long (*get_command)(intptr_t *param);
    bool (*loop_track)(void);
    void (*strip_filesize)(off_t value);
    void (*panicf)(const char *msg, ...);
    bool cop_assist;
};
int codec_run_proc(void);
#endif
