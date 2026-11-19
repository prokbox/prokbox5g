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

/* pcmbuf.c's data_level(): the bytes in quarter_secs/4 s at the frame's
 * real size. The function is pasted in verbatim by srcpatch.py
 * (spec_level.py); the expected values are literals, not the code's own
 * arithmetic. Built twice: the shell's (HAVE_PCM_24BIT_OUT, 4- or 8-byte
 * frames) and stock (4-byte frames only). */
#include <stdio.h>

static unsigned long test_freq;
static unsigned int pcmbuf_bpf = 4;
#define PCMBUF_SAMPLE_SIZE pcmbuf_bpf
static unsigned long mixer_get_frequency(void) { return test_freq; }

#include "level_slice.c"

static int fails, checks;

static void want(unsigned long freq, unsigned int bpf, int q,
                 unsigned int expect)
{
    test_freq = freq;
    pcmbuf_bpf = bpf;
    unsigned int got = data_level(q);
    checks++;
    if (got != expect)
    {
        printf("FAIL data_level(%d) at %lu Hz, %u-byte frames: %u, want %u\n",
               q, freq, bpf, got, expect);
        fails++;
    }
}

int main(void)
{
    /* 16-bit (4-byte frames): upstream's numbers, in every build */
    want(44100, 4, 1, 44100);       /* 0.25 s */
    want(44100, 4, 4, 176400);      /* 1 s */
    want(48000, 4, 2, 96000);       /* 0.5 s */
#ifdef HAVE_PCM_24BIT_OUT
    /* 24-bit (8-byte frames): twice the bytes for the same time */
    want(44100, 8, 1, 88200);
    want(44100, 8, 4, 352800);      /* 1 s: the prefill */
    want(48000, 8, 2, 192000);      /* 0.5 s: the crossfade minimum */
    want(96000, 8, 4, 768000);      /* 1 s at Audiophile's top rate */
#endif
    printf("VERDICT: %s (%d checks)\n", fails ? "FAIL" : "PASS", checks);
    return fails != 0;
}
