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

/* tests/schw: the few Rockbox declarations apps/ipodui/sc_hw.c uses, as
 * host stubs (each stub header includes this one) */
#ifndef SCHW_STUB_H
#define SCHW_STUB_H
#include <stdbool.h>
#include <stdint.h>

/* metadata.h */
struct mp3entry {
    long track_level, album_level;
    long track_gain, album_gain;
};

/* appevents.h */
#define PLAYBACK_EVENT_TRACK_CHANGE 1
struct track_event { unsigned int flags; struct mp3entry *id3; };
void add_event(unsigned short id,
               void (*handler)(unsigned short id, void *data));

/* audio.h */
#define AUDIO_STATUS_PLAY 1
int audio_status(void);
struct mp3entry *audio_current_track(void);

/* playlist.h */
int playlist_get_display_index(void);

/* settings.h */
struct user_settings { int volume_limit; };
extern struct user_settings global_settings;

/* dsp_misc.h */
enum { REPLAYGAIN_TRACK = 0, REPLAYGAIN_ALBUM, REPLAYGAIN_SHUFFLE,
       REPLAYGAIN_OFF };

/* ipodui/anim.h, ipodui/uilog.h */
void schw_trace(const char *fmt, ...);
#define UPDATE_TRACE(fmt, ...) schw_trace(fmt, __VA_ARGS__)
void ui_log(const char *tag, const char *fmt, ...);

#endif
