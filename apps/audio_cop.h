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

/* Audio output on the COP (apps/audio_cop.c) */
#ifndef AUDIO_COP_H
#define AUDIO_COP_H

#include "config.h"

enum audio_cop_state
{
    AUDIO_COP_CPU = 0,      /* not tried (no layout A, or before boot) */
    AUDIO_COP_ON,           /* the COP takes the PCM's FIQ */
    AUDIO_COP_FAILED,       /* tried; the CPU takes it this boot */
};

#ifdef HAVE_CORE_LAYOUT_A
void audio_cop_init(void);
enum audio_cop_state audio_cop_state(void);
#else
static inline void audio_cop_init(void) {}
static inline enum audio_cop_state audio_cop_state(void) { return AUDIO_COP_CPU; }
#endif

#endif /* AUDIO_COP_H */
