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

/* #318: which FLAC streams the codec plays, shared by the codec (flac.c, at
   open) and the library (the unplayable reason at index time, #286), so
   the two cannot disagree. */
#ifndef _FLAC_LIMITS_H
#define _FLAC_LIMITS_H

/* The widest sample the decoder hands on at its own width; wider streams
   (30..32 bits) are reduced to it with dither, and cost more to decode. */
#define FLAC_OUTPUT_DEPTH 29

/* The most samples a second (rate x channels) a stream wider than
   FLAC_OUTPUT_DEPTH may carry: 48 kHz stereo.  tests/flacarm's ARM7TDMI
   estimate (zero-wait memory) for 32-bit stereo is 466 cycles a sample at
   -8 and 726 with 32nd-order predictors, against about 250 for 24-bit: at
   48 kHz that is 22-35 MHz of the COP, at 88.2/96 kHz 41-70 MHz, too close
   to its 80 MHz once the DSP and cache misses are added. */
#define FLAC_WIDE_MAX_RATE (48000 * 2)

/* True when the codec refuses a stream of bps bits at rate Hz with
   channels channels for its cost (logged at open as "too slow") */
#define FLAC_TOO_WIDE_FOR_RATE(bps, rate, channels) \
    ((bps) > FLAC_OUTPUT_DEPTH && \
     (long)(rate) * (long)(channels) > FLAC_WIDE_MAX_RATE)

#endif
