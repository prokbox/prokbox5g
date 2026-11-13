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

/* #298: see autoskip_core.h */
#include "autoskip_core.h"

enum as_verdict as_codec_end(const struct as_in *in)
{
    if (!in->paused && !in->skip_pending)
        return AS_NOW;
    /* Paused: nothing sounds; the resume begins the change (stock).
     * A short track that produced PCM: its PCM is queued behind the
     * outgoing track's; only one change marker exists, so wait (stock). */
    if (in->paused || !in->skip_auto || !in->silent || in->repeat_one)
        return AS_DEFER;
    /* This failure ends the queue (#281): the deferred path counts it
     * and stops once the outgoing PCM has played out, as before */
    if (in->silent_fails + 1 >= in->fail_limit)
        return AS_DEFER;
    /* The next entry must be ready to start now; otherwise the stock
     * path loads it at the change */
    if (!in->next_ready || in->next_pauses)
        return AS_DEFER;
    return AS_EARLY;
}

bool as_kick_due(unsigned int notices, unsigned int *seen)
{
    if (notices == *seen)
        return false;
    *seen = notices;
    return true;
}
