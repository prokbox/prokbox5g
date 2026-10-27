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

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include "system.h"
#include "audio.h"
#include "kernel.h"
#include "logf.h"
#include "screens.h"
#include "list.h"
#include "action.h"
#include "lang.h"
#include "settings.h"
#include "playback.h"
#include "metadata.h"
#include "audiobook.h"

bool ab_chapter_skip(struct audiobook_chapters *ab, int direction,
                     unsigned long curr_pos)
{
    int chapter = ab_chapter_skip_target(ab, direction, curr_pos);

    if (chapter < 0 || !(audio_status() & AUDIO_STATUS_PLAY))
        return false;

    audio_pre_ff_rewind();
    audio_ff_rewind(ab_ch_start(ab, chapter));
    return true;
}

static const char *ab_list_get_name_cb(int selected_item,
                                       void *data,
                                       char *buffer,
                                       size_t buffer_len)
{
    struct audiobook_chapters *ab = (struct audiobook_chapters *)data;
    int ch_idx = selected_item;

    if (ch_idx < 0 || ch_idx >= ab->chapter_count)
        return "";

    unsigned long ms = ab_ch_start(ab, ch_idx);
    unsigned long secs = ms / 1000;
    unsigned int hrs = secs / 3600;
    unsigned int mins = (secs % 3600) / 60;
    unsigned int s = secs % 60;

    if (ab_ch_title_ptr(ab, ch_idx)[0])
    {
        snprintf(buffer, buffer_len, "%02u:%02u:%02u  %s",
                 hrs, mins, s, ab_ch_title_ptr(ab, ch_idx));
    }
    else
    {
        snprintf(buffer, buffer_len, "%02u:%02u:%02u  Chapter %d",
                 hrs, mins, s, ch_idx + 1);
    }

    return buffer;
}

void ab_browse_chapters(struct audiobook_chapters *ab)
{
    struct gui_synclist lists;
    int action;
    bool done = false;

    struct mp3entry *id3 = audio_current_track();

    gui_synclist_init(&lists, ab_list_get_name_cb, ab, false, 1, NULL);
    gui_synclist_set_nb_items(&lists, ab->chapter_count);
    gui_synclist_set_title(&lists, "Chapters", 0);

    if (id3)
    {
        gui_synclist_select_item(&lists,
                                 ab_find_current_chapter(ab, id3->elapsed));
    }

    while (!done)
    {
        gui_synclist_draw(&lists);
        action = get_action(CONTEXT_LIST, TIMEOUT_BLOCK);
        if (gui_synclist_do_button(&lists, &action))
            continue;
        switch (action)
        {
            case ACTION_STD_OK:
            {
                int sel = gui_synclist_get_sel_pos(&lists);
                if (sel >= 0 && sel < ab->chapter_count)
                {
                    if (audio_status() & AUDIO_STATUS_PLAY)
                    {
                        audio_pre_ff_rewind();
                        audio_ff_rewind(ab_ch_start(ab, sel));
                    }
                }
                done = true;
                break;
            }
            case ACTION_STD_CANCEL:
                done = true;
                break;
            default:
                break;
        }
    }
}

void ab_draw_chapter_markers(struct screen *screen,
                             struct audiobook_chapters *ab,
                             unsigned long tracklen,
                             int x, int y, int w, int h)
{
    int i, xi;
    unsigned long tracklen_seconds = tracklen / 1000;

    if (tracklen_seconds == 0)
        return;

    for (i = 1; i < ab->chapter_count; i++)
    {
        xi = x + (w * (ab_ch_start(ab, i) / 1000)) / tracklen_seconds;
        screen->set_drawmode(DRMODE_COMPLEMENT);
        screen->vline(xi, y, y + h - 1);
    }
}
