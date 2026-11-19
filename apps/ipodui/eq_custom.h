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
 * iPod UI shell: the custom EQ slots (#82)
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
#ifndef IPODUI_EQ_CUSTOM_H
#define IPODUI_EQ_CUSTOM_H

#include <stdbool.h>
#include "dac_ext.h"

/* #82 custom EQ, the contract between Settings > EQ (screens/eq.c, which
 * owns the apply path) and the editor. Settings > EQ lists Off, the 22
 * presets, then the 3 custom slots: ipodui_eq_preset values
 * EQ_CUSTOM_FIRST .. EQ_CUSTOM_FIRST + 2. A slot is stored as one line of
 * text (eqlab_core.h eqlab_format(), also the export format, so an export
 * is a straight dump) in ipodui_eq_custom1..3. Every curve, preset or
 * slot, applies through one path: eqlab_level() for the precut and the
 * make-up (#203), then dac_eq_set(). UI thread, all of them. */
#define EQ_CUSTOM_SLOTS     3
#define EQ_CUSTOM_FIRST     23

/* slot 0..2's curve, parsed (flat when empty; flat and logged when
 * unreadable) */
void ui_eq_custom_get(int slot, struct dac_eq_band *b);
/* stores slot's curve and saves the settings; when the slot is the EQ in
 * use, applies it */
void ui_eq_custom_set(int slot, const struct dac_eq_band *b);
/* the editor's live curve: applied through the same path, nothing
 * stored. NULL applies the stored EQ again (leaving, or compare) */
void ui_eq_live(const struct dac_eq_band *b);
/* makes slot the EQ in use (ipodui_eq_preset), applied and saved */
void ui_eq_custom_select(int slot);
/* "Custom 1" .. "Custom 3" (LANG_IPODUI_EQ_CUSTOM_1..3) */
const char *ui_eq_custom_name(int slot);

#endif /* IPODUI_EQ_CUSTOM_H */
