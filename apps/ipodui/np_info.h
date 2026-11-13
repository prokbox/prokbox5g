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

#ifndef _IPODUI_NP_INFO_H_
#define _IPODUI_NP_INFO_H_

#include <stddef.h>

/* The Now Playing "info" page (#277): what is playing and how it reaches
 * the output. Read from the playing track's parsed tags (no disk reads).
 *   line 1: "FLAC  24-bit  192 kHz" (lossless) / "MP3  256 kbps  44.1 kHz"
 *   line 2: "Output 24-bit 96 kHz"
 * Either may come back empty (nothing parsed): the page then draws what
 * it has. */
void np_info_lines(char *l1, size_t n1, char *l2, size_t n2);

/* What the output stage runs at: the PCM's rate and the frame format the
 * mix runs in (24 while a deeper-than-16-bit source plays, #268). Back
 * end, apps/audiophile.c; the shell's calls cross to the CPU (febe). */
int audio_output_rate(void);   /* Hz */
int audio_output_bits(void);   /* 16 or 24 */

#endif
