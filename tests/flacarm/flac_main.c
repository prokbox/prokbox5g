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

/* Bare-metal driver for the FLAC decoder (libffmpegFLAC) under
 * qemu-system-arm with semihosting, run by tests/flacarm/run.sh.
 *
 * It decodes ./in.flac (qemu's working directory) with the decoder objects
 * of a real firmware build (decoder.o and arm.o, linked in by run.sh), and
 * writes the samples to ./out.raw as signed little-endian PCM of the
 * stream's width, interleaved: the format flac -d --force-raw-format
 * writes, so the two files must be byte-identical.  Every function of the
 * driver is named drv_* (or main), so the cycle plugin's per-symbol profile
 * separates the decoder's cycles from the driver's.  Prints
 * "FRAMES <n> SAMPLES <n> BPS <n> CH <n>" or "ERROR <code> AT <frame>". */
#include <stdint.h>
#include <string.h>
#include "decoder.h"

#define IN_BASE  ((uint8_t *)0x02000000)    /* the stream, up to 32 MB */
#define OUT_BASE ((uint8_t *)0x04000000)    /* the PCM, up to 64 MB */
#define MAXBS    65535

uint8_t bs_log2_tab[256];                   /* codeclib's, for av_log2 */
static int32_t dec[2][MAXBS];
static FLACContext fc;

static int drv_sh(int op, void *arg)
{
    register int r0 __asm__("r0") = op;
    register void *r1 __asm__("r1") = arg;
    __asm__ volatile("svc 0x123456" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}

static void drv_puts(const char *s)
{
    drv_sh(4, (void *)s);
}

static char *drv_dec(char *p, uint32_t v)
{
    char t[12];
    int n = 0;
    do { t[n++] = '0' + v % 10; v /= 10; } while (v);
    while (n)
        *p++ = t[--n];
    return p;
}

static void drv_yield(void)
{
}

static uint8_t *drv_out(uint8_t *o, int n, int ch, int bps)
{
    int sh = FLAC_OUTPUT_DEPTH - bps, w = (bps + 7) / 8;
    for (int i = 0; i < n; i++)
        for (int c = 0; c < ch; c++) {
            int32_t v = fc.decoded[c][i] >> sh;
            for (int b = 0; b < w; b++, v >>= 8)
                *o++ = (uint8_t)v;
        }
    return o;
}

unsigned flac_range_or_arm(const int32_t *p, int n, int bias);

/* arm.S's range check against the C loop it replaces, on samples in and out
   of range: a valid stream never sends lpc_analyze_remodulate() down the
   path where the answer matters, so flac -d cannot judge it */
static int drv_range_check(void)
{
    static int32_t v[64];
    uint32_t x = 1;
    for (int t = 0; t < 400; t++) {
        int n = t % 41, bias = 1 << (t % 24);
        unsigned c = 0;
        for (int i = 0; i < n; i++) {
            x = x * 1664525u + 1013904223u;
            v[i] = (int32_t)x >> (t % 31);
            c |= v[i] + bias;
        }
        if (flac_range_or_arm(v, n, bias) != c)
            return 0;
    }
    return 1;
}

int main(void)
{
    uint32_t a[3];
    static char msg[96];
    char *p = msg;
    int h, len, pos, frames = 0, res = 0;
    uint32_t samples = 0;
    uint8_t *o = OUT_BASE;

    a[0] = (uint32_t)"selftest"; a[1] = 1; a[2] = 8;
    h = drv_sh(0x01, a);
    if (h >= 0) {         /* run.sh's self-test run: no stream, no cycles */
        a[0] = h;
        drv_sh(0x02, a);
        drv_puts(drv_range_check() ? "RANGE ok\n" : "RANGE FAIL\n");
        return 0;
    }
    for (int i = 1; i < 256; i++)
        bs_log2_tab[i] = bs_log2_tab[i >> 1] + (i > 1);

    a[0] = (uint32_t)"in.flac"; a[1] = 1; a[2] = 7;          /* "rb" */
    h = drv_sh(0x01, a);
    if (h < 0) { drv_puts("ERROR open\n"); return 1; }
    a[0] = h;
    len = drv_sh(0x0c, a);
    a[0] = h; a[1] = (uint32_t)IN_BASE; a[2] = len;
    drv_sh(0x06, a);
    a[0] = h;
    drv_sh(0x02, a);

    const uint8_t *b = IN_BASE;
    if (len < 42 || b[0] != 'f' || b[1] != 'L' || b[2] != 'a' || b[3] != 'C') { drv_puts("ERROR magic\n"); return 1; }
    pos = 4;
    for (;;) {
        int last = b[pos] & 0x80, type = b[pos] & 0x7f;
        int bl = b[pos + 1] << 16 | b[pos + 2] << 8 | b[pos + 3];
        const uint8_t *s = b + pos + 4;
        if (type == 0) {
            fc.min_blocksize = s[0] << 8 | s[1];
            fc.max_blocksize = s[2] << 8 | s[3];
            fc.min_framesize = s[4] << 16 | s[5] << 8 | s[6];
            fc.max_framesize = s[7] << 16 | s[8] << 8 | s[9];
            fc.samplerate = s[10] << 12 | s[11] << 4 | s[12] >> 4;
            fc.channels = ((s[12] & 0x0e) >> 1) + 1;
            fc.bps = ((s[12] & 1) << 4 | s[13] >> 4) + 1;
        }
        pos += 4 + bl;
        if (last)
            break;
    }
    if (fc.channels > 2) { drv_puts("ERROR channels\n"); return 1; }
    fc.decoded[0] = dec[0];
    fc.decoded[1] = fc.channels == 2 ? dec[1] : dec[0];

    while (len - pos >= MIN_FRAME_SIZE) {
        int left = len - pos;
        /* all that is left: a 65535-sample frame can be far larger than
           MAX_FRAMESIZE (flac.c copies such frames whole) */
        res = flac_decode_frame(&fc, (uint8_t *)b + pos, left, drv_yield);
        if (res)
            break;
        o = drv_out(o, fc.blocksize, fc.channels, fc.bps);
        samples += fc.blocksize;
        pos += fc.framesize;
        frames++;
    }

    a[0] = (uint32_t)"out.raw"; a[1] = 5; a[2] = 7;          /* "wb" */
    h = drv_sh(0x01, a);
    a[0] = h; a[1] = (uint32_t)OUT_BASE; a[2] = o - OUT_BASE;
    drv_sh(0x05, a);
    a[0] = h;
    drv_sh(0x02, a);

    if (res) {
        p = memcpy(p, "ERROR ", 6) + 6;
        *p++ = '-';
        p = drv_dec(p, -res);
        p = memcpy(p, " AT ", 4) + 4;
        p = drv_dec(p, frames);
    } else {
        p = memcpy(p, "FRAMES ", 7) + 7;
        p = drv_dec(p, frames);
        p = memcpy(p, " SAMPLES ", 9) + 9;
        p = drv_dec(p, samples);
        p = memcpy(p, " BPS ", 5) + 5;
        p = drv_dec(p, fc.bps);
        p = memcpy(p, " CH ", 4) + 4;
        p = drv_dec(p, fc.channels);
    }
    *p++ = '\n';
    *p = 0;
    drv_puts(msg);
    return res ? 1 : 0;
}
