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
#ifndef IPODUI_SC_HW_H
#define IPODUI_SC_HW_H

/* Hardware Sound Check (docs/phase15-brief.md section 3.5, wave 2b).
 *
 * The ReplayGain gain is applied by the WM8758 (dac_set_gain_db10(): amp
 * and DAC volume, with the EQ precut) instead of the DSP's PGA, so the
 * digital path stays bit-perfect and costs no CPU. One engine at a time:
 * replaygain_update() (apps/misc.c) hands the mode here and gives the DSP
 * "off" with no clip guard, so the PGA stays at unity.
 *
 * Timing: the gain follows PLAYBACK_EVENT_TRACK_CHANGE, which the audio
 * thread sends once the PCM buffer has played the previous track's last
 * chunk (pcmbuf_pcm_callback() -> Q_AUDIO_TRACK_CHANGED), i.e. when the
 * listener starts hearing the new track, not when the codec reaches it
 * (codec_set_replaygain(), seconds ahead through the buffer). At a cold
 * start or a manual skip the event comes before any of the track's PCM.
 *
 * Trace (UPDATE_TRACE) at each applied change:
 *   sc engine=hw gain_db10=<n> track=<playlist display index>
 * and, when a boost is cut short by the volume max or Volume Limit, an
 * ipodui.log line "sc capped want=<n> applied=<m>". */

enum sc_mode { SC_OFF = 0, SC_TRACK, SC_ALBUM };

#define SC_LEVEL_ONE    4096    /* mp3entry track_level: dB << 12 */
#define SC_LEVEL_MIN    (-48 * SC_LEVEL_ONE)    /* ReplayGain's own clamp */
#define SC_LEVEL_MAX    ( 17 * SC_LEVEL_ONE)

/* The gain a track gets, tenth dB, the way the DSP chose it
 * (dsp_replaygain_update()): track mode takes the track gain when the
 * file has one, else the album gain; album mode the reverse; no gain
 * information is 0 (no preamp either). Pure: tests/schw compiles it. */
static inline int sc_gain_db10(enum sc_mode mode, int preamp_db10,
                               long track_gain, long track_level,
                               long album_gain, long album_level)
{
    long level;

    if (mode == SC_OFF)
        return 0;
    if ((mode == SC_TRACK && track_gain != 0) || album_gain == 0)
    {
        if (track_gain == 0)
            return 0;
        level = track_level;
    }
    else
        level = album_level;
    if (level < SC_LEVEL_MIN)
        level = SC_LEVEL_MIN;
    if (level > SC_LEVEL_MAX)
        level = SC_LEVEL_MAX;
    level *= 10;
    level = level >= 0 ? (level + SC_LEVEL_ONE / 2) / SC_LEVEL_ONE
                       : -((SC_LEVEL_ONE / 2 - level) / SC_LEVEL_ONE);
    return (int)level + preamp_db10;
}

/* replaygain_update(): the mode (upstream's replaygain type after
 * replaygain_setting_mode(), REPLAYGAIN_TRACK/ALBUM/OFF) and the preamp
 * (tenth dB). Applies at once to the playing track. Thread context. */
void ui_sc_update(int rg_type, int preamp_db10);
/* The Volume Limit changed (ui_vlimit_apply()): refit a boost to it */
void ui_sc_refresh(void);

#endif /* IPODUI_SC_HW_H */
