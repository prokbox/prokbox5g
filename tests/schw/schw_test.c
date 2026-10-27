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
 * Host test of hardware Sound Check (docs/phase15-brief.md section 3.5,
 * wave 2b): firmware/export/dac_gain.h (the trim fitted to the volume max
 * and Volume Limit, split over amp and DAC, rounding <= 0.25 dB),
 * apps/ipodui/sc_hw.h (which ReplayGain value a track gets) and
 * apps/ipodui/sc_hw.c against stubs (tests/schw/stub): the gain switches
 * only at PLAYBACK_EVENT_TRACK_CHANGE, from that event's track, never from
 * whatever the decoder is on; boot before audio_init() never touches the
 * playback state; a Volume Limit change refits a boost; one trace per
 * applied change; a boost that doesn't fit is logged.
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
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "schw_stub.h"
#include "dac_ext.h"
#include "dac_gain.h"
#include "ipodui/sc_hw.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; \
        printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
        printf("\n"); } } while (0)

/* ---- stubs ------------------------------------------------------------ */
struct user_settings global_settings = { 6 };
static void (*track_cb)(unsigned short, void *);
static int status;                      /* audio_status() */
static struct mp3entry *current;        /* audio_current_track() */
static int current_calls, display_index;
static int dac_gain = 0, dac_ceil = DAC_GAIN_VOL_MAX, dac_vol = -250;
static int dac_calls;
static char last_trace[128], last_log[128];
static int traces, logs;

void add_event(unsigned short id, void (*h)(unsigned short, void *))
{
    CHECK(id == PLAYBACK_EVENT_TRACK_CHANGE, "event %u", id);
    CHECK(!track_cb, "hooked twice");
    track_cb = h;
}
int audio_status(void) { return status; }
struct mp3entry *audio_current_track(void)
{
    current_calls++;
    return current;
}
int playlist_get_display_index(void) { return display_index; }
void schw_trace(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_trace, sizeof(last_trace), fmt, ap);
    va_end(ap);
    traces++;
}
void ui_log(const char *tag, const char *fmt, ...)
{
    va_list ap;
    int n = snprintf(last_log, sizeof(last_log), "%s ", tag);
    va_start(ap, fmt);
    vsnprintf(last_log + n, sizeof(last_log) - n, fmt, ap);
    va_end(ap);
    logs++;
}
void dac_set_gain_db10(int db10, int ceil_db10)
{
    dac_gain = db10;
    dac_ceil = ceil_db10;
    dac_calls++;
}
void dac_get_stats(struct dac_stats *s)
{
    int d, a;
    memset(s, 0, sizeof(*s));
    s->gain_db10 = dac_gain;
    s->gain_applied_db10 = dac_gain_split(dac_vol, dac_gain, dac_ceil,
                                          &d, &a);
}

#define LVL(db10) ((long)(db10) * SC_LEVEL_ONE / 10)

static struct mp3entry track(long tg10, long ag10)
{
    struct mp3entry e;
    e.track_level = LVL(tg10);
    e.track_gain = tg10 ? 1 : 0;        /* linear factor; only != 0 used */
    e.album_level = LVL(ag10);
    e.album_gain = ag10 ? 1 : 0;
    return e;
}

static void play(struct mp3entry *id3, int index)
{
    struct track_event te = { 0, id3 };
    display_index = index;
    track_cb(PLAYBACK_EVENT_TRACK_CHANGE, &te);
}

/* ---- dac_gain.h ------------------------------------------------------- */
static double level_db(int dac, int amp)
{
    return (amp - 57) + (dac - 255) * 0.5;
}

static void test_split(void)
{
    int vol, g, dac, amp;

    /* no trim: whole dB reproduces the upstream split exactly */
    for (vol = -890; vol <= 60; vol += 10)
    {
        int db = vol / 10 + 90, ud, ua;
        if (db > 32) { ud = 255; ua = db - 90 + 57; }
        else { ud = (db - 33) * 2 + 255; ua = 0; }
        dac_gain_split(vol, 0, 60, &dac, &amp);
        CHECK(dac == ud && amp == ua, "vol %d: dac %d amp %d, upstream %d %d",
              vol, dac, amp, ud, ua);
    }
    dac_gain_split(-900, 50, 60, &dac, &amp);
    CHECK(dac == 0 && amp == 0x40, "mute stays mute (%d %d)", dac, amp);
    dac_gain_split(-900, -50, 60, &dac, &amp);
    CHECK(dac == 0 && amp == 0x40, "mute stays mute with a cut");

    /* every volume x trim: rounding <= 0.25 dB, registers in range, the
     * fitted total never above the ceiling */
    for (vol = -890; vol <= 60; vol += 10)
        for (g = -480; g <= 170; g++)
        {
            int ceil = (g & 1) ? 60 : -100, fit;
            double want, got;
            fit = dac_gain_split(vol, g, ceil, &dac, &amp);
            want = (vol + fit) / 10.0;
            got = level_db(dac, amp);
            CHECK(dac >= 1 && dac <= 255 && amp >= 0 && amp <= 63,
                  "vol %d g %d: dac %d amp %d", vol, g, dac, amp);
            if (want >= -184.0)         /* above the DAC's floor */
                CHECK(got - want <= 0.25 && want - got <= 0.25,
                      "vol %d g %d: %.2f dB for %.2f", vol, g, got, want);
            if (g > 0)
            {
                CHECK(fit >= 0 && fit <= g, "vol %d g %d: fit %d", vol, g,
                      fit);
                CHECK(vol + fit <= (ceil < 60 ? ceil : 60) || fit == 0,
                      "vol %d g %d ceil %d: total %d", vol, g, ceil,
                      vol + fit);
            }
            else
                CHECK(fit == g, "a cut always applies (%d %d)", g, fit);
            if (amp > 0 && amp < 63)
                CHECK(dac >= 254, "amp region keeps the DAC within 0.5 dB "
                      "(vol %d g %d dac %d)", vol, g, dac);
        }

    /* the examples the brief names */
    CHECK(dac_gain_fit(-250, 32, 60) == 32, "+3.2 dB fits at -25 dB");
    CHECK(dac_gain_fit(40, 32, 60) == 20, "+3.2 dB at +4 dB: 2 dB room");
    CHECK(dac_gain_fit(-250, 32, -240) == 10, "Volume Limit -24 dB: 1 dB");
    CHECK(dac_gain_fit(-200, 32, -240) == 0, "over the limit: no boost");
    CHECK(dac_gain_fit(60, -65, 60) == -65, "a cut at the max");
    dac_gain_split(-250, -65, 60, &dac, &amp);
    CHECK(amp == 57 - 31 && dac == 254, "-31.5 dB: amp -31, DAC -0.5 "
          "(%d %d)", amp, dac);
}

/* ---- sc_hw.h ---------------------------------------------------------- */
static void test_choice(void)
{
    struct mp3entry t = track(-65, -40), a = track(0, -40), n = track(0, 0),
                    o = track(32, 0);

#define G(m, e, p) sc_gain_db10(m, p, (e).track_gain, (e).track_level, \
                                (e).album_gain, (e).album_level)
    CHECK(G(SC_TRACK, t, 0) == -65, "track mode: track gain");
    CHECK(G(SC_TRACK, a, 0) == -40, "track mode, no track gain: album");
    CHECK(G(SC_ALBUM, t, 0) == -40, "album mode: album gain");
    CHECK(G(SC_ALBUM, o, 0) == 32, "album mode, no album gain: track");
    CHECK(G(SC_TRACK, n, 0) == 0, "no ReplayGain: 0");
    CHECK(G(SC_TRACK, n, 30) == 0, "no ReplayGain: no preamp either");
    CHECK(G(SC_TRACK, t, 30) == -35, "preamp adds");
    CHECK(G(SC_OFF, t, 30) == 0, "off: 0");
    t = track(-600, 0);
    CHECK(G(SC_TRACK, t, 0) == -480, "clamped at -48 dB like the DSP");
    t.track_level = -(3 * SC_LEVEL_ONE + SC_LEVEL_ONE / 4); /* -3.25 dB */
    CHECK(G(SC_TRACK, t, 0) == -33, "nearest tenth (%d)", G(SC_TRACK, t, 0));
#undef G
}

/* ---- sc_hw.c: the switch timing --------------------------------------- */
static void test_timing(void)
{
    struct mp3entry a = track(-65, 0), b = track(32, 0), c = track(0, -40);
    int t0;

    /* boot: settings_apply() before audio_init(): nothing is playing, the
     * playback state isn't touched, the trim is 0 */
    status = 0;
    ui_sc_update(REPLAYGAIN_OFF, 0);
    CHECK(track_cb != NULL, "TRACK_CHANGE hooked");
    CHECK(current_calls == 0, "audio_current_track() before audio_init()");
    CHECK(dac_gain == 0 && dac_ceil == 60, "boot trim %d ceil %d", dac_gain,
          dac_ceil);
    CHECK(traces == 1 && !strcmp(last_trace,
          "sc engine=hw gain_db10=0 track=0"), "boot trace '%s'", last_trace);

    /* Sound Check on, still idle: no change, no trace */
    ui_sc_update(REPLAYGAIN_TRACK, 0);
    CHECK(traces == 1, "no trace without a change");

    /* track 1 starts playing: its gain */
    status = AUDIO_STATUS_PLAY;
    current = &a;
    play(&a, 1);
    CHECK(dac_gain == -65, "track 1: %d", dac_gain);
    CHECK(!strcmp(last_trace, "sc engine=hw gain_db10=-65 track=1"),
          "'%s'", last_trace);

    /* the decoder reaches track 2 seconds early (its metadata becomes the
     * id3 the playback code reads ahead): the codec hands its gains to the
     * DSP, which ignores them; sc_hw has no hook there, the trim stays */
    t0 = dac_calls;
    current = &b;               /* worst case: "current" already moved */
    CHECK(dac_gain == -65 && dac_calls == t0, "no change before the event");

    /* the PCM reaches track 2: the event carries track 2 */
    current = &a;               /* and a stale "current" can't leak in */
    play(&b, 2);
    CHECK(dac_gain == 32, "track 2 from the event: %d", dac_gain);
    CHECK(!strcmp(last_trace, "sc engine=hw gain_db10=32 track=2"),
          "'%s'", last_trace);

    /* Volume Limit -24 dB at volume -25 dB: the +3.2 dB boost refits to
     * 1 dB and is logged */
    current = &b;
    dac_vol = -250;
    global_settings.volume_limit = -24;
    logs = 0;
    ui_sc_refresh();
    CHECK(dac_ceil == -240, "ceil follows the limit (%d)", dac_ceil);
    CHECK(logs == 0, "same gain and track: no new trace, no log");
    play(&c, 3);
    play(&b, 2);                /* a skip back: traced, capped, logged */
    CHECK(logs == 1 && !strcmp(last_log, "sc capped want=32 applied=10"),
          "cap log '%s' (%d)", last_log, logs);
    global_settings.volume_limit = 6;
    ui_sc_refresh();

    /* track 3 (album gain only), then Sound Check off and on mid-track */
    current = &c;
    play(&c, 3);
    CHECK(dac_gain == -40, "track 3 fallback: %d", dac_gain);
    ui_sc_update(REPLAYGAIN_OFF, 0);
    CHECK(dac_gain == 0 && !strcmp(last_trace,
          "sc engine=hw gain_db10=0 track=3"), "off: '%s'", last_trace);
    ui_sc_update(REPLAYGAIN_TRACK, 0);
    CHECK(dac_gain == -40, "on again: %d", dac_gain);

    /* a track change with no id3 (stop): 0 */
    play(NULL, 0);
    CHECK(dac_gain == 0, "no track: %d", dac_gain);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    test_split();
    test_choice();
    test_timing();
    printf("schw: %s (%d failure%s)\n", fails ? "FAIL" : "ok", fails,
           fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
