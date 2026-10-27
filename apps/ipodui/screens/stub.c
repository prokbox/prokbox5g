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
/* Placeholder for every screen that is not built yet: the header with the
 * item's title over an empty list. MENU goes back. */
#include "config.h"
#include "lcd.h"
#include "ipodui/theme.h"
#include "ipodui/menu.h"
#include "ipodui/screens/screens.h"

static int stub_enter(struct ui_screen *s, const void *arg)
{
    s->title = arg;
    return 0;
}

static void stub_draw(struct ui_screen *s, unsigned dirty)
{
    const struct ui_theme *t = ui_theme;
    (void)s;

    if (!(dirty & UI_DIRTY_BODY))
        return;
    lcd_set_viewport(NULL);
    lcd_set_foreground(t->list_bg);
    lcd_fillrect(0, t->header_h, LCD_WIDTH, LCD_HEIGHT - t->header_h);
}

const struct ui_screen_ops ui_stub_screen = {
    .name    = "stub",
    .enter   = stub_enter,
    .leave   = NULL,
    .draw    = stub_draw,
    .input   = NULL,
    .timeout = NULL,
};

/* Settings > About is screens/about.c (P11), under the symbol and .name
 * "about" the developer chord arms on (Phase 8a Revision 1 M6). */
