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

#ifndef IPODUI_EQTABLE_CORE_H
#define IPODUI_EQTABLE_CORE_H

#include <stdbool.h>
#include "dac_ext.h"

/* #352: the EQ presets as one table per output-rate family (owner,
 * 2026-11-13). The WM8758's EQ coefficients exist for 48 kHz and below
 * only, so at 88.2/96 kHz every band acts about an octave high
 * (docs/wm8758-reference/playback-path.md section 4.4) and the presets
 * need a second fit. The firmware picks the table for the family of the
 * current output rate; a family without a table plays the preset flat
 * (bypassed) and the custom slots apply at every rate.
 *
 *  - base (<= 48 kHz): compiled in (eq_hw_presets[], tools/eqfit), so the
 *    presets always work;
 *  - hi (88.2/96 kHz; 176.4/192 kHz sources are converted down to them):
 *    an optional text file in the hidden system folder, parsed once at
 *    boot into a small core_alloc'd block. 0.9.0 ships none; 0.9.1 adds
 *    the file, no code change.
 *
 * The file (EQTABLE_VERSION 1):
 *    eqtable 1 family=hi n=22
 *    # comments and blank lines are skipped
 *    eq1 +5@80 +4@230 ...          <- preset 1 (Acoustic), the catalogue
 *    ...                              order of screens/eq.c presets[]
 *    eq1 -2@80 ...                 <- preset 22 (Vocal Booster)
 * Every preset line is a custom slot's line (eqlab_core.h eqlab_format(),
 * parsed by eqlab_parse(): one format and one parser for slots, tables
 * and later shared or per-album EQs, #338). Its Hz are the nominal
 * datasheet values, i.e. the register codes, not the corners heard at
 * the family's rate. Exactly n preset lines, n == EQTABLE_PRESETS; a
 * short, long, malformed or other-version file is refused whole. */
#define EQTABLE_VERSION     1
#define EQTABLE_PRESETS     22
#define EQTABLE_FILE_MAX    4096    /* bytes; ~1.1 KB for 22 full lines */
#define EQTABLE_BYTES       (EQTABLE_PRESETS * DAC_EQ_BANDS * \
                             (int)sizeof(struct dac_eq_band))   /* 330 */
/* the base family's top rate: above it, the hi family */
#define EQTABLE_BASE_MAX_HZ 48000

enum eqtable_family {
    EQTABLE_BASE = 0,
    EQTABLE_HI,
    EQTABLE_FAMILIES
};

enum eqtable_err {
    EQT_OK = 0,
    EQT_SIZE,       /* empty or over EQTABLE_FILE_MAX */
    EQT_HEADER,     /* not "eqtable <v> family=<name> n=<count>" */
    EQT_VERSION,    /* <v> is not EQTABLE_VERSION */
    EQT_FAMILY,     /* <name> is not the family asked for */
    EQT_COUNT,      /* <count> is not EQTABLE_PRESETS */
    EQT_LINE,       /* a preset line eqlab_parse() refuses (or too long) */
    EQT_SHORT,      /* fewer preset lines than <count> */
    EQT_LONG,       /* more preset lines than <count> */
};

/* the family of an output rate (Hz; 0, unknown, is base) */
int eqtable_family_of(unsigned long rate_hz);
/* "base", "hi" (the file's family= and the log's) */
const char *eqtable_family_name(int family);
/* "short", "bad line", ... for the log */
const char *eqtable_err_name(int err);

/* Parses a table file image (len bytes, not NUL-terminated) for family
 * into out[EQTABLE_PRESETS][DAC_EQ_BANDS]. EQT_OK or the first error;
 * *line (if not NULL) is the 1-based line it was found on (0: none).
 * out is scratch on failure: the caller keeps nothing then. */
int eqtable_parse(const char *buf, int len, int family,
                  struct dac_eq_band (*out)[DAC_EQ_BANDS], int *line);

#endif /* IPODUI_EQTABLE_CORE_H */
