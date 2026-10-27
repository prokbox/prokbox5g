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
 * iPod UI shell
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
#ifndef IPODUI_ROLLER_H
#define IPODUI_ROLLER_H

#include <stdbool.h>
#include <stdint.h>
#include "ipodui/input.h"

/* Phase 11 roller widget (docs/phase11-brief.md sections 3.5, 4.6):
 * N numeric fields with ranges, the current one in a blue rounded box
 * (photo extras-screenlock-new-combination.jpg). Used by Volume Limit's
 * Enter Combination, Date & Time's Date and Time screens, and P12 Screen
 * Lock. Contract, committed before fan-out; roller.c (U2a) implements it.
 * The state lives in the calling screen's arena state (no allocation). */

#define UI_ROLLER_MAX 4
struct ui_roller_field {
    short min, max, value;
    uint8_t digits;                      /* cell width, in digits */
    bool text;                           /* shows txt[value - min] instead */
};
struct ui_roller {
    struct ui_roller_field f[UI_ROLLER_MAX];
    uint8_t n, cur;                      /* fields, the boxed one */
    bool dark;                           /* dark screens (combination, date) */
    const char *txt[2];                  /* strings of the text fields (AM, PM) */
};
enum ui_roller_res { UI_ROLLER_BUSY, UI_ROLLER_DONE, UI_ROLLER_CANCEL };
/* Pure: the wheel changes the boxed field, SELECT (and NEXT/PREV) move
 * between fields; DONE after SELECT on the last field, CANCEL on MENU.
 * *band_y / *band_h: the band to push for this step (one per step). */
enum ui_roller_res ui_roller_input(struct ui_roller *r, const struct ui_input *in,
                                   int *band_y, int *band_h);
/* Draws the fields with their box at LCD row y; label_lang: a LANG_*
 * id drawn above them, or -1 for none. Draw only (no lcd_update). */
void ui_roller_draw(const struct ui_roller *r, int y, int label_lang);

#endif /* IPODUI_ROLLER_H */
