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

/* #196: the codec's id3 (playback.c CODEC_ID3) is written by both cores
 * while the decode runs on the COP. The codec writes fields of it
 * (shorten: frequency, length, bitrate; vgm: length, tail_trim; asap:
 * length; mpa: elapsed for an ASF stream; atrac3_rm: id3v2buf), and the
 * CPU writes elapsed and offset (set_elapsed, set_offset, a seek) and the
 * whole entry at each track start. The PP5022's caches do not snoop and
 * write back whole 16-byte lines (quirks.md 8, 88), so a CPU line holding
 * a stale copy of a field the COP wrote puts the stale value back when it
 * is written back.
 *
 * So the entry has cache lines of its own and both cores reach it only
 * through the uncached alias: every store goes to memory as it is, a
 * field at a time, whichever core makes it. codec_id3_place() takes the
 * slot in the audio scratch area, aligns the entry inside it, drops any
 * line the CPU still holds of the slot's memory from its earlier use,
 * and returns the uncached pointer. */
#ifndef CODEC_ID3_H
#define CODEC_ID3_H

#include <stddef.h>

#define CODEC_ID3_LINE  16      /* the PP5022's cache line, in bytes */

/* Bytes for a slot that holds 'size' bytes on lines of its own, wherever
 * the slot starts */
#define CODEC_ID3_SLOT_SIZE(size) \
    ((((size) + CODEC_ID3_LINE - 1) & ~(size_t)(CODEC_ID3_LINE - 1)) \
     + CODEC_ID3_LINE)

/* The entry's place in a slot of CODEC_ID3_SLOT_SIZE(size) bytes: the
 * first line boundary in it, through the uncached alias where the build
 * has one, after the CPU's cache was written back and dropped. Call it on
 * the CPU, with the codec stopped, before anything touches the entry. */
void *codec_id3_place(void *slot);

#endif /* CODEC_ID3_H */
