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

/* Phase 6 §3.5 / D14: audiobook speed. global_settings.ipodui_ab_speed
 * (0.75x / 1x / 1.25x) applies to audiobooks only, never podcasts or
 * music, from M's guarded hook where the codec starts a track (audio
 * thread) and at once when the setting changes (UI thread).
 *
 * time-stretch is forced on at shell init (ui_main.c), so tdspeed's
 * 57,344 B are allocated once at boot; at 100 % the stage is inactive
 * (tdspeed_update() deactivates it), so music and Normal books cost no
 * CPU. Under HAVE_IPOD_UI dsp_set_timestretch() does not write
 * global_status.resume_speed (tdspeed.c), so no status save follows
 * (Rev 1 item 6). Classification is RAM only:
 * id3->is_audiobook or the database's flags. Statics: none. */

#include <stdbool.h>
#include "config.h"
#include "system.h"
#include "settings.h"
#include "audio.h"
#include "metadata.h"
#include "tagcache.h"
#include "dsp_misc.h"
#include "tdspeed.h"
#include "logf.h"
#include "ipodui/anim.h"        /* UPDATE_TRACE */
#include "ipodui/ab_book.h"

static void apply(long factor)
{
    if (!dsp_timestretch_available() || dsp_get_timestretch() == factor)
        return;
    dsp_set_timestretch(factor);        /* leaves global_status alone */
    UPDATE_TRACE(AB_TR_SPEED, (int)(factor / 100));
}

static long factor_for(const struct mp3entry *id3)
{
    int32_t flag = 0;
    bool book, pod;

    if (!id3)
        return ab_speed_factor(AB_SPEED_NORMAL);
    if (id3->tagcache_idx > 0
        && !tagcache_ui_seeks((int)id3->tagcache_idx - 1, &flag))
        flag = 0;
    pod = (flag & TAGCACHE_UI_PODCAST) != 0;
    book = id3->is_audiobook || (flag & TAGCACHE_UI_AUDIOBOOK);
#ifdef HAVE_BCMFW_VIDEO
    /* a movie is never slowed or sped: the picture follows the audio's
     * clock, and a chaptered one must not read as a book (3.2a) */
    if (id3->is_video || (flag & TAGCACHE_UI_VIDEO))
        return ab_speed_factor(AB_SPEED_NORMAL);
#endif
    return ab_speed_for_track(global_settings.ipodui_ab_speed, book, pod);
}

void ab_speed_track_start(const struct mp3entry *id3)
{
    apply(factor_for(id3));
}

void ab_speed_changed(void)
{
    if (audio_status() & AUDIO_STATUS_PLAY)
        apply(factor_for(audio_current_track()));
}
