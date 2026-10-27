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

/* sdk_badgame: the misbehaving test game (docs/design/sdk.md 15; the
 * simulator and developer builds only). Its mode is the first byte of its
 * state record (the simulator's `sdk_bad <mode>` writes it); it never
 * commits, so the mode stays until the next `sdk_bad`.
 *   0 well behaved: idle, a label
 *   1 slow: every step takes 30 ms (> the 20 ms slice): a violation,
 *     then a kill on the repeat
 *   2 loop: spins in its own code with no SDK call (the unwind from the
 *     watchdog, the simulator's model of the IRQ path)
 *   3 SDK loop: spins calling sdk_now_ms() (the flag path)
 *   4 slow exit: exit() takes 3 s under a 1 s graceful policy (the
 *     deadline)
 *   5 one huge slice: a 300 ms step (over 10 x: killed at once)
 *   6 bad argument: draws from outside its memory (a violation, no
 *     kill on its own) */
#include "ipodui/sdk/sdk.h"
#include "lang.h"

static const uint16_t bg_lang[] = { LANG_IPODUI_X_SDK_TESTS };
static const struct sdk_strings bg_strings = { 1, bg_lang };

static uint8_t mode;
static volatile uint32_t spin;

static void busy_ms(struct sdk_ctx *c, uint32_t ms)
{
    uint32_t t0 = sdk_now_ms(c);

    /* game time: the loop itself, between two clock reads */
    while (sdk_now_ms(c) - t0 < ms)
        for (spin = 0; spin < 2000; spin++)
            ;
}

static int bg_init(struct sdk_ctx *c)
{
    uint16_t len;
    const uint8_t *rec = sdk_state(c, &len);

    mode = len >= 1 && rec ? rec[0] : 0;
    sdk_log(c, "mode=%u", mode);
    return SDK_OK;
}

static void bg_draw(struct sdk_ctx *c)
{
    int x, y, w, h;

    sdk_area(c, &x, &y, &w, &h);
    sdk_material(c, SDK_MAT_METAL, x, y, w, h);
    sdk_text(c, x + 20, y + 20, 0, SDK_FONT_LIST, SDK_RGB(0, 0, 0));
    sdk_text_num(c, x + 20, y + 44, mode, SDK_FONT_LIST, SDK_RGB(0, 0, 0));
    sdk_dirty(c, y, h);
}

static int bg_step(struct sdk_ctx *c, const struct sdk_input *in,
                   uint32_t now_ms)
{
    (void)in; (void)now_ms;
    switch (mode)
    {
        case 1:
            busy_ms(c, 30);
            break;
        case 2:
            for (;;)
                spin++;
        case 3:
            for (;;)
                (void)sdk_now_ms(c);
        case 5:
            busy_ms(c, 300);
            break;
        case 6:
            sdk_bitmap(c, (const sdk_px *)(uintptr_t)0x10, 0, 0, 4, 0, 40,
                       4, 4);
            return SDK_IDLE;
        default:
            return SDK_IDLE;
    }
    return SDK_FRAME_MIN_MS;
}

static bool bg_back(struct sdk_ctx *c) { (void)c; return false; }
static void bg_suspend(struct sdk_ctx *c, enum sdk_why why)
{ (void)c; (void)why; }
static void bg_resume(struct sdk_ctx *c) { bg_draw(c); }

static void bg_exit(struct sdk_ctx *c, enum sdk_why why)
{
    (void)why;
    if (mode == 4)
        busy_ms(c, 3000);
}

SDK_DESC_A(badgame, 4096, 64, 64, MOD_EXIT_GRACEFUL, 1000);
SDK_GAME(badgame, bg_strings) = {
    .init = bg_init, .step = bg_step, .back = bg_back,
    .suspend = bg_suspend, .resume = bg_resume, .exit = bg_exit,
};
