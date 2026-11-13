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

/* Stub for the host test: codec_cop.c names the one pointer setting the
 * codecs pass (REPLAYGAIN_SET_GAINS) and its pointee's size
 * (lib/rbcodec/dsp/dsp_misc.h); the real header pulls in the whole DSP.
 * The value is any distinct setting id: the test never configures it. */
#ifndef STUB_DSP_PROC_ENTRY_H
#define STUB_DSP_PROC_ENTRY_H

#define REPLAYGAIN_SET_GAINS    0x7001

struct dsp_replay_gains
{
    long track_gain;
    long album_gain;
    long track_peak;
    long album_peak;
};

#ifndef ARRAYLEN
#define ARRAYLEN(a) (sizeof(a) / sizeof((a)[0]))
#endif

#endif /* STUB_DSP_PROC_ENTRY_H */
