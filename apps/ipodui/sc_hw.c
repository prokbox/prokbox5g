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
 * iPod UI shell: hardware Sound Check (docs/phase15-brief.md section 3.5,
 * wave 2b; how and why in sc_hw.h)
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
#include "config.h"
#include <stdbool.h>
#include <stddef.h>
#include "appevents.h"
#include "audio.h"
#include "metadata.h"
#include "playlist.h"
#include "settings.h"
#include "dsp_misc.h"               /* REPLAYGAIN_* */
#include "dac_ext.h"
#include "ipodui/anim.h"            /* UPDATE_TRACE */
#include "ipodui/uilog.h"
#include "ipodui/sc_hw.h"

static enum sc_mode sc_mode;
static int sc_preamp;
static bool sc_hooked;
/* the last traced change; written by the audio and UI threads, which the
 * cooperative scheduler never interleaves inside sc_apply() except at the
 * codec mutex (both values are re-read after it) */
static int sc_last_gain = 0x7fffffff;
static int sc_last_track = -1;

/* Volume Limit in tenth dB (SOUND_VOLUME is whole dB on the WM8758) */
static int sc_ceil(void)
{
    return global_settings.volume_limit * 10;
}

static void sc_apply(const struct mp3entry *id3)
{
    struct dac_stats st;
    int track = id3 ? playlist_get_display_index() : 0;
    int g = id3 ? sc_gain_db10(sc_mode, sc_preamp,
                               id3->track_gain, id3->track_level,
                               id3->album_gain, id3->album_level) : 0;

    dac_set_gain_db10(g, sc_ceil());
    if (g == sc_last_gain && track == sc_last_track)
        return;
    sc_last_gain = g;
    sc_last_track = track;
    UPDATE_TRACE("sc engine=hw gain_db10=%d track=%d", g, track);
    dac_get_stats(&st);
    if (st.gain_applied_db10 != g)
        ui_log("sc", "capped want=%d applied=%d", g, st.gain_applied_db10);
}

/* Audio thread, once the listener hears the new track (sc_hw.h) */
static void sc_track_changed(unsigned short id, void *data)
{
    struct track_event *te = data;

    (void)id;
    sc_apply(te ? te->id3 : NULL);
}

/* The playing track, or NULL: settings_apply() runs before audio_init(),
 * when audio_current_track()'s id3 mutex isn't set up yet */
static struct mp3entry *sc_current(void)
{
    return (audio_status() & AUDIO_STATUS_PLAY) ? audio_current_track()
                                                : NULL;
}

void ui_sc_update(int rg_type, int preamp_db10)
{
    sc_mode = rg_type == REPLAYGAIN_TRACK ? SC_TRACK
            : rg_type == REPLAYGAIN_ALBUM ? SC_ALBUM : SC_OFF;
    sc_preamp = preamp_db10;
    if (!sc_hooked)
    {
        sc_hooked = true;
        add_event(PLAYBACK_EVENT_TRACK_CHANGE, sc_track_changed);
    }
    sc_apply(sc_current());
}

void ui_sc_refresh(void)
{
    sc_apply(sc_current());
}
