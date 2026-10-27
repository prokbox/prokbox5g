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

#ifndef VIX_FILE_H
#define VIX_FILE_H

#include "vix.h"

/* Where the index of a video lives (hidden, like every shell file). */
#define VIX_DIR ROCKBOX_DIR "/ipodui/video"

/* <dir>/<vix_name()>.vix into out; 0 or MP4_E_SPACE. */
int vix_path(const char *dir, const char *video, uint64_t size,
             uint32_t mtime, char *out, size_t cap);

/* Builds the index of video (size and mtime as the library has them) in
   dir.  scratch: 4 KB or more from the caller (core_alloc on the PP).
   Returns the index's size in bytes or < 0 (an mp4_err); files the demuxer
   refuses get no index. */
long vix_build_file(const char *video, const char *dir, uint64_t size,
                    uint32_t mtime, void *scratch, size_t scratch_len);

#endif /* VIX_FILE_H */
