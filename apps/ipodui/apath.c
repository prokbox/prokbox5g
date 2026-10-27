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
 * iPod UI shell: the developer "Audio path test" (docs/phase15-brief.md
 * section 3.5, wave 2b). Proves on the device that a 16-bit source reaches
 * the PCM sink unchanged: a known 16-bit WAV plays while a mixer tap
 * (pcm_mixer.h, armed only during the test) hashes the frames sent to the
 * sink, and the hash is compared with the file's own samples, read back
 * from the disk. Covers the decoder, the DSP chain (ARM assembly
 * included), pcmbuf and the mixer.
 *
 * The test file, /.rockbox/apath.wav, is generated on the device by the
 * first run (2 s of seeded noise, 44.1 kHz 16-bit stereo, 352,844 bytes)
 * and kept for the next; not in the music library, not shipped in the zip.
 * The noise is at about -18 dBFS and the volume is held at -30 dB or
 * lower while it plays.
 *
 * Alignment: the hash starts at the first frame matching an 8-frame
 * signature taken 0.5 s into the file (past any fade-in, 250 ms) and runs
 * over 1 s of frames. Result in ipodui.log:
 *   apath frames=<n> hash=<x> want=<y> match=<0|1>
 * frames=0 means the signature never went by (a stage changed the
 * samples, or the playback didn't start).
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
#include <stdint.h>
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "button.h"
#include "bcmjob_pp.h"          /* P19: bcmjob_owner_event() */
#include "file.h"
#include "audio.h"
#include "playlist.h"
#include "settings.h"
#include "sound.h"
#include "misc.h"                   /* default_event_handler */
#include "pcm_mixer.h"
#include "eq.h"                     /* dsp_eq_enable */
#include "dsp_misc.h"
#include "tdspeed.h"
#include "ipodui/uilog.h"
#include "ipodui/apath.h"
#include "xcore.h"                  /* XCORE_BSS */
#include "ipodui/screens/screens.h" /* ui_eq_apply */

#define AP_FILE     ROCKBOX_DIR "/apath.wav"
#define AP_RATE     44100
#define AP_FRAMES   (2 * AP_RATE)           /* 2 s */
#define AP_HDR      44
#define AP_BYTES    (AP_HDR + 4 * AP_FRAMES)
#define AP_SKIP     (AP_RATE / 2)           /* 0.5 s: past a fade-in */
#define AP_SIG      8
#define AP_HASH_N   AP_RATE                 /* 1 s hashed */
#define AP_SEED     0x2545f491u
#define AP_CHUNK    128                     /* frames per disk I/O */
#define AP_VOL_MAX  (-30)                   /* dB while the noise plays */
#define AP_TIMEOUT  (8 * HZ)

#define FNV_BASIS   0x811c9dc5u
#define FNV_PRIME   0x01000193u

enum { AP_SEEK, AP_HASHING, AP_DONE };

/* tap state: written by the PCM callback (FIQ, CPU) and the UI thread
 * (which may run on the COP: P19 ucop-fix2), so uncached (xcore.h) */
static volatile int ap_state XCORE_BSS;
static uint32_t ap_sig[AP_SIG] XCORE_BSS;
static int ap_m XCORE_BSS;
static uint32_t ap_hash XCORE_BSS;
static volatile unsigned int ap_count XCORE_BSS;

static uint32_t ap_rand(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
}

/* One frame of the test signal: two 13-bit samples (about -18 dBFS) */
static uint32_t ap_frame(uint32_t *s)
{
    uint32_t r = ap_rand(s);
    uint16_t l = (uint16_t)((int16_t)r >> 3);
    uint16_t rr = (uint16_t)((int16_t)(r >> 16) >> 3);
    return l | ((uint32_t)rr << 16);
}

static uint32_t ap_fnv(uint32_t h, uint32_t frame)
{
    return (h ^ frame) * FNV_PRIME;
}

static void put_le(uint8_t *p, uint32_t v, int n)
{
    while (n--)
    {
        *p++ = v & 0xff;
        v >>= 8;
    }
}

/* The mixer tap (FIQ): find the signature, then hash AP_HASH_N frames */
static void ap_tap(const void *frames, size_t size)
{
    const uint32_t *f = frames;
    size_t i, n = size / 4;

    for (i = 0; i < n && ap_state != AP_DONE; i++)
    {
        uint32_t v = f[i];

        if (ap_state == AP_SEEK)
        {
            if (v == ap_sig[ap_m])
            {
                if (++ap_m == AP_SIG)
                {
                    int k;
                    ap_hash = FNV_BASIS;
                    for (k = 0; k < AP_SIG; k++)
                        ap_hash = ap_fnv(ap_hash, ap_sig[k]);
                    ap_count = AP_SIG;
                    ap_state = AP_HASHING;
                }
            }
            else
                ap_m = v == ap_sig[0] ? 1 : 0;
        }
        else
        {
            ap_hash = ap_fnv(ap_hash, v);
            if (++ap_count == AP_HASH_N)
                ap_state = AP_DONE;
        }
    }
}

/* The test file: kept when it has the right size, else written anew */
static bool ap_make_file(void)
{
    uint32_t buf[AP_CHUNK];
    uint32_t seed = AP_SEED;
    uint8_t *h = (uint8_t *)buf;
    int fd, i, left;

    fd = open(AP_FILE, O_RDONLY);
    if (fd >= 0)
    {
        bool ok = ffilesize(fd) == AP_BYTES;
        close(fd);
        if (ok)
            return true;
    }
    fd = open(AP_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return false;
    memcpy(h, "RIFF", 4);
    put_le(h + 4, AP_BYTES - 8, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    put_le(h + 16, 16, 4);              /* fmt chunk size */
    put_le(h + 20, 1, 2);               /* PCM */
    put_le(h + 22, 2, 2);               /* stereo */
    put_le(h + 24, AP_RATE, 4);
    put_le(h + 28, AP_RATE * 4, 4);     /* bytes per second */
    put_le(h + 32, 4, 2);               /* block align */
    put_le(h + 34, 16, 2);              /* bits per sample */
    memcpy(h + 36, "data", 4);
    put_le(h + 40, 4 * AP_FRAMES, 4);
    if (write(fd, h, AP_HDR) != AP_HDR)
        goto fail;
    for (left = AP_FRAMES; left > 0; left -= AP_CHUNK)
    {
        int n = MIN(left, AP_CHUNK);
        for (i = 0; i < n; i++)
            put_le((uint8_t *)&buf[i], ap_frame(&seed), 4);
        if (write(fd, buf, n * 4) != n * 4)
            goto fail;
    }
    close(fd);
    ui_log("apath", "gen bytes=%d", AP_BYTES);
    return true;
fail:
    close(fd);
    remove(AP_FILE);
    return false;
}

/* The file's own samples, read back: the signature and the hash */
static bool ap_want(uint32_t *want)
{
    uint8_t buf[AP_CHUNK * 4];
    uint32_t h = FNV_BASIS;
    int fd, got = 0;

    fd = open(AP_FILE, O_RDONLY);
    if (fd < 0)
        return false;
    if (lseek(fd, AP_HDR + 4 * AP_SKIP, SEEK_SET) != AP_HDR + 4 * AP_SKIP)
        goto fail;
    while (got < AP_HASH_N)
    {
        int n = MIN(AP_HASH_N - got, AP_CHUNK), i;
        if (read(fd, buf, n * 4) != n * 4)
            goto fail;
        for (i = 0; i < n; i++, got++)
        {
            const uint8_t *p = buf + 4 * i;
            uint32_t v = p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16)
                       | ((uint32_t)p[3] << 24);
            if (got < AP_SIG)
                ap_sig[got] = v;
            h = ap_fnv(h, v);
        }
    }
    close(fd);
    *want = h;
    return true;
fail:
    close(fd);
    return false;
}

int ui_apath_test(void)
{
    uint32_t want;
    long end;
    bool sw_eq = global_settings.eq_enabled;
    int speed = 100;

    if (!ap_make_file() || !ap_want(&want))
    {
        ui_log("apath", "no test file%s", "");
        ui_log_flush();
        return 0;
    }
#if defined(HAVE_PITCHCONTROL) && defined(HAVE_TIMESTRETCH)
    speed = dsp_get_timestretch() * 100 / PITCH_SPEED_100;
#endif
    ui_log("apath", "start sw_eq=%d rg=%d speed=%d", (int)sw_eq,
           global_settings.replaygain_settings.type, speed);

    audio_stop();
    dsp_eq_enable(false);                   /* EQ off: no software EQ */
    sound_set_volume(MIN(global_status.volume, AP_VOL_MAX));
    ap_m = 0;
    ap_hash = 0;
    ap_count = 0;
    ap_state = AP_SEEK;
    mixer_set_tap(ap_tap);

    playlist_create(NULL, NULL);
    if (playlist_insert_track(NULL, AP_FILE, PLAYLIST_INSERT_LAST,
                              false, true) >= 0)
        playlist_start(0, 0, 0);

    end = current_tick + AP_TIMEOUT;
    while (ap_state != AP_DONE && TIME_BEFORE(current_tick, end))
    {
        long btn = button_get_w_tmo(HZ / 10);
        if (bcmjob_owner_event(btn))
            continue;                       /* P19: the job owner's pass */
        if (btn != BUTTON_NONE && !(btn & BUTTON_REL))
        {
            if (IS_SYSEVENT(btn))
                default_event_handler(btn);
            break;                          /* any key stops it */
        }
    }
    mixer_set_tap(NULL);
    sleep(1);                               /* a callback in the tap ends */
    audio_stop();
    sound_set_volume(global_status.volume);
    ui_eq_apply();                          /* the stored preset again */

    ui_log("apath", "frames=%u hash=%08lx want=%08lx match=%d",
           ap_state == AP_SEEK ? 0 : ap_count, (unsigned long)ap_hash,
           (unsigned long)want,
           (int)(ap_state == AP_DONE && ap_hash == want));
    ui_log_flush();
    return 0;
}
