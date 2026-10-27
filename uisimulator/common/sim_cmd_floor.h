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
 * Sim script commands of the audio-buffer floor gate; see below.
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
#ifndef SIM_CMD_FLOOR_H
#define SIM_CMD_FLOOR_H

#include <stdbool.h>

/* The audio-buffer floor gate (Phase 16-A, #105). The floor is 7,500,000 B
 * on a 32 MB build and 39,000,000 B on a 64 MB build (MEMORYSIZE), the
 * audio file buffer being audio_buffer_size() (playback.c: the buffer
 * minus AUDIO_BUFFER_RESERVE). Handles these sim_script.c commands:
 *
 *   floor <label>           log the buffer now ("floor <label> ram_mb=
 *                           audio= filebuflen="), fail ("expect_floor
 *                           failed: ...") when it is under the floor or
 *                           there is no audio session
 *   floor_watch begin [timeline]
 *                           a kernel thread samples the buffer every tick (10 ms)
 *                           (and the core heap's free bytes); "timeline"
 *                           also logs "floor_tl ms= free= audio=" at each
 *                           256 KB move of the free heap (the commit
 *                           peak per phase, read against the tagcache
 *                           lines in sim.log)
 *   floor_watch end <label> [free]
 *                           stop it; log the minimum seen (and the
 *                           minimum free heap) and fail under the floor ("free": log the free-heap
 *                           minimum only, no audio session needed)
 *                           (sees the transient dips: commit
 *                           peak, panel block, chapter table)
 *   floor_reserve <slot> <bytes>
 *                           core_alloc <bytes> (slot 0-4), as a shell
 *                           reservation: panel transient 785844, module
 *                           arena 65536, chap_bytes 77832; slot 4 the
 *                           resident BCM image (floor_gate.sh)
 *   floor_release <slot>    free it again
 *
 * The USB prefetch ring is not modelled: it is taken only while audio is
 * idle (docs/hardware.md). Returns false for any other command. */
bool sim_cmd_floor(const char *cmd, char *argv[], int argc, int lineno);

#endif
