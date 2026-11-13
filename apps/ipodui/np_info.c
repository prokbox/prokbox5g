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
#include <string.h>
#include "config.h"
#include "audio.h"
#include "metadata.h"
#include "lang.h"
#include "ipodui/np_info.h"

/* Format names are names, not words: they are not translated */
static const char *format_name(unsigned int ct, bool *lossless)
{
    *lossless = false;
    switch (ct)
    {
    case AFMT_MPA_L1:      return "MP1";
    case AFMT_MPA_L2:      return "MP2";
    case AFMT_MPA_L3:      return "MP3";
    case AFMT_AIFF:        *lossless = true; return "AIFF";
    case AFMT_PCM_WAV:
    case AFMT_WAVE64:      *lossless = true; return "WAV";
    case AFMT_OGG_VORBIS:  return "Vorbis";
    case AFMT_FLAC:        *lossless = true; return "FLAC";
    case AFMT_MPC_SV7:
    case AFMT_MPC_SV8:     return "Musepack";
    case AFMT_A52:
    case AFMT_RM_AC3:      return "AC-3";
    case AFMT_WAVPACK:     *lossless = true; return "WavPack";
    case AFMT_MP4_ALAC:    *lossless = true; return "ALAC";
    case AFMT_MP4_AAC:
    case AFMT_MP4_AAC_HE:
    case AFMT_RM_AAC:
    case AFMT_AAC_BSF:     return "AAC";
    case AFMT_SHN:         *lossless = true; return "Shorten";
    case AFMT_SPEEX:       return "Speex";
    case AFMT_APE:         *lossless = true; return "APE";
    case AFMT_WMA:
    case AFMT_WMAPRO:
    case AFMT_WMAVOICE:    return "WMA";
    case AFMT_TTA:         *lossless = true; return "TTA";
    case AFMT_OPUS:        return "Opus";
    default:               return "";
    }
}

/* 44100 -> "44.1", 48000 -> "48", 22050 -> "22.05" */
static void khz_str(char *buf, size_t n, unsigned long hz)
{
    if (hz % 1000 == 0)
        snprintf(buf, n, "%lu", hz / 1000);
    else if (hz % 100 == 0)
        snprintf(buf, n, "%lu.%lu", hz / 1000, (hz % 1000) / 100);
    else
        snprintf(buf, n, "%lu.%02lu", hz / 1000, (hz % 1000 + 5) / 10);
}

void np_info_lines(char *l1, size_t n1, char *l2, size_t n2)
{
    struct mp3entry *id3 = audio_current_track();
    char khz[24];
    int orate = audio_output_rate(), obits = audio_output_bits();

    l1[0] = l2[0] = '\0';
    if (id3)
    {
        bool lossless;
        const char *fmt = format_name(id3->codectype, &lossless);

        if (*fmt && id3->frequency)
        {
            khz_str(khz, sizeof khz, id3->frequency);
            if (lossless && id3->bits_per_sample)
                snprintf(l1, n1, str(LANG_NP_INFO_LOSSLESS), fmt,
                         (int)id3->bits_per_sample, khz);
            else if (!lossless && id3->bitrate)
                snprintf(l1, n1, str(LANG_NP_INFO_LOSSY), fmt,
                         (int)id3->bitrate, khz);
            else
                snprintf(l1, n1, str(LANG_NP_INFO_RATE), fmt, khz);
        }
        else if (*fmt)
            snprintf(l1, n1, "%s", fmt);
    }
    if (orate > 0)
    {
        khz_str(khz, sizeof khz, (unsigned long)orate);
        snprintf(l2, n2, str(LANG_NP_INFO_OUTPUT), obits, khz);
    }
}
