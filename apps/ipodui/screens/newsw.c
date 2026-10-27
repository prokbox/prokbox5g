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

/* New software (tracker #68): the firmware file on the disk changed while
 * the computer had it (a new build copied over USB). apps/misc.c's
 * check_bootfile() used to ask the stock yes/no prompt, which the shell
 * draws behind its screens or not at all. Now it calls
 * ui_bootfile_changed(), which pushes this prompt right after the USB
 * screen leaves: the light prompt shared with Reset Settings, a segmented
 * Restart | Later, Restart preselected. Restart loads the new image
 * (rolo_load() refuses a bad one and returns: logged, back to the shell);
 * Later does nothing, the new software starts at the next restart.
 * MENU is Later. */

#include "config.h"
#include <stdbool.h>
#include "system.h"
#include "kernel.h"
#include "lang.h"
#include "audio.h"
#include "rolo.h"
#include "ipodui/anim.h"
#include "ipodui/uilog.h"
#include "ipodui/ipodui.h"
#include "ipodui/screen_stack.h"
#include "ipodui/header.h"
#include "ipodui/screens/setwidgets.h"
#include "ipodui/screens/screens.h"

enum { SEG_RESTART, SEG_LATER, SEG_COUNT };
static const short seg_lang[SEG_COUNT] = { LANG_IPODUI_NEWSW_RESTART,
                                           LANG_IPODUI_NEWSW_LATER };

static int seg(const struct ui_screen *s)
{
    return (int)(intptr_t)s->priv;
}

static void newsw_restart(void)
{
    UPDATE_TRACE("bootfile changed choice=%s", "restart");
#ifndef SIMULATOR                   /* the simulator has no second image */
    audio_hard_stop();
    if (rolo_load(BOOTDIR "/" BOOTFILE) < 0)
    {
        const char *why = rolo_error_text();

        ui_log("boot", "restart refused: %s", why ? why : "bad image");
    }
#endif
}

static int newsw_enter(struct ui_screen *s, const void *arg)
{
    (void)arg;
    s->title = str(LANG_IPODUI_NEWSW_TITLE);
    s->priv = (void *)(intptr_t)SEG_RESTART;
    return 0;
}

static void newsw_draw(struct ui_screen *s, unsigned dirty)
{
    if (dirty & UI_DIRTY_BODY)
    {
        ui_prompt_draw(LANG_IPODUI_NEWSW_HEADING, LANG_IPODUI_NEWSW_BODY);
        ui_seg_draw(seg_lang, SEG_COUNT, seg(s), true);
    }
    else if (dirty & UI_DIRTY_BAND)
        ui_seg_draw(seg_lang, SEG_COUNT, seg(s), false);
}

static enum ui_result newsw_input(struct ui_screen *s,
                                  const struct ui_input *in)
{
    int v = seg(s), d = 0;

    switch (in->key)
    {
        case UI_KEY_WHEEL:
            d = in->wheel_steps;
            break;
        case UI_KEY_NEXT:
        case UI_KEY_PREV:
            if (!in->release || in->long_press)
                return UI_HANDLED;
            d = in->key == UI_KEY_NEXT ? 1 : -1;
            break;
        case UI_KEY_SELECT:
            if (!in->release || in->long_press)
                return UI_HANDLED;
            if (v == SEG_RESTART)
                newsw_restart();
            else
                UPDATE_TRACE("bootfile changed choice=%s", "later");
            return UI_POP;          /* back here only if the restart failed */
        case UI_KEY_MENU:
            if (!in->release || in->long_press)
                return UI_HANDLED;
            UPDATE_TRACE("bootfile changed choice=%s", "later");
            return UI_POP;
        default:
            return UI_IGNORED;
    }
    v = MIN(MAX(v + d, 0), SEG_COUNT - 1);
    if (v != seg(s))
    {
        s->priv = (void *)(intptr_t)v;
        ui_invalidate_band(UI_SEG_Y, UI_SEG_H);
    }
    return UI_HANDLED;
}

const struct ui_screen_ops ui_newsw_screen = {
    .name    = "newsw",
    .enter   = newsw_enter,
    .leave   = NULL,
    .draw    = newsw_draw,
    .input   = newsw_input,
    .timeout = NULL,
    .tick    = NULL,
};

/* misc.c check_bootfile(), after the USB screen. The shell's input
 * loop then redraws everything; the prompt comes up without a slide
 * (the screen behind it is the USB picture). */
void ui_bootfile_changed(void)
{
    if (ui_push(&ui_newsw_screen, NULL) == 0)
        (void)ui_take_transition();
}
