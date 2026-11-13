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

/* #298, the skip over an unplayable track: what the audio thread does
 * with a codec's end that arrives while an automatic track change is
 * still playing out, and when the PCM side's track-change notice must
 * wake the thread. Pure logic, no Rockbox calls (host test:
 * tests/autoskip/).
 *
 * Stock Rockbox defers any codec end that arrives during an automatic
 * change until the outgoing track's PCM has played out, then starts the
 * next codec. After a silent failure (the codec stopped before any PCM,
 * #281) nothing of the failed track sits in the PCM buffer, so that
 * deferral left the buffer to run dry: the skip cost the notice's poll
 * (0-100 ms) plus the next track's start (~37.5 ms on the device). Now
 * the next track's codec starts at the failure, behind the outgoing
 * track's PCM, as on a normal gapless change. */
#ifndef AUTOSKIP_CORE_H
#define AUTOSKIP_CORE_H

#include <stdbool.h>

enum as_verdict {
    AS_NOW = 0,     /* no change in progress: change track now (stock) */
    AS_DEFER,       /* remember it; act at the PCM's track change (stock) */
    AS_EARLY,       /* start the next track's codec now (#298) */
};

struct as_in {
    bool paused;            /* playback paused */
    bool skip_pending;      /* any track change is in progress */
    bool skip_auto;         /* ... and it is a plain automatic one */
    bool silent;            /* the codec ended before any PCM (#281) */
    bool repeat_one;        /* Repeat One: the "next" track is this one */
    int silent_fails;       /* silent failures in a row before this one */
    int fail_limit;         /* the run that ends the queue (#281) */
    bool next_ready;        /* the entry after the failed one is buffered
                             * with its audio */
    bool next_pauses;       /* single mode would pause before it */
};

enum as_verdict as_codec_end(const struct as_in *in);

/* The tick's check of the PCM side's track-change counter: true (and
 * *seen updated) when it moved since the last kick, so the audio thread
 * is woken once per notice. */
bool as_kick_due(unsigned int notices, unsigned int *seen);

#endif /* AUTOSKIP_CORE_H */
