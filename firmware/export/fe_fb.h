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
 *  The pixel hand-off across the cores (P19 MF-4; docs/design/p19-ui-on-cop.md
 * section 6.3): before a CPU thread reads (or overwrites) framebuffer
 * pixels the UI may have drawn on the COP.
 *
 * Copyright (C) 2026 prokopto-dev
 * Copyright (C) 2026 prokopto-dev
 *
 * Portions derived from Rockbox (rockbox.org).
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
#ifndef FE_FB_H
#define FE_FB_H

/* fe_fb_publish(): the COP's dirty lines over the framebuffer reach memory.
 *  - the shell not on the COP (every build but a developer one switched to
 *    "cop"), or the caller on the COP itself: FE_FB_LOCAL at once (the
 *    COP caller's own cache is written back);
 *  - the ui thread blocked (in its input wait, or in a call to the back
 *    end): FE_FB_CLEAN at once, it wrote its cache back before blocking;
 *  - else a fence: the ui thread is woken, writes back, and answers within
 *    timeout_ms: FE_FB_PUBLISHED, or FE_FB_TIMEOUT (the COP hung or is
 *    busy in a long draw): the caller logs it loudly and goes on.
 * The CPU caller then reads and writes the pixels through
 * UNCACHED_ADDR(FBADDR(...)) so its own cache never holds them.
 * rect (x, y, w, h; NULL = the whole framebuffer) names what the caller
 * reads; the PP5022 has whole-cache maintenance only, so it is a record,
 * for the log.  CPU threads only, not interrupts; it may block. */
struct fe_fb_rect { int x, y, w, h; };
enum fe_fb_result {
    FE_FB_LOCAL = 0,
    FE_FB_CLEAN,
    FE_FB_PUBLISHED,
    FE_FB_TIMEOUT,
};
#define FE_FB_TIMEOUT_MS 50
int fe_fb_publish(const struct fe_fb_rect *rect, int timeout_ms);

#endif /* FE_FB_H */
