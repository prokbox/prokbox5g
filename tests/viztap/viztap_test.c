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

/* Host tests for the visualizer's PCM tap (apps/ipodui/viz_tap_core.c and
 * viz_tap.c over stubs; docs/research/bcm2722/visualizer.md section 11):
 * the ring's wraparound, a writer faster than the reader, a reader with
 * no writer, torn reads, the split order, the downmix and decimation,
 * the normalising shift against jviz_l2, the audiobook refusal, the
 * memory rule, and the ear-to-window lag of the mixer tap against a tap
 * at the DSP's output. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <stdbool.h>

/* the writer in the middle of a read: an FIQ on the device */
struct viz_ring;
static void midread(const struct viz_ring *r);
#define VT_MIDREAD(r) midread(r)
#include "../../apps/ipodui/viz_tap_core.c"
#include "viz_tap.h"
#include "metadata.h"
#include "pcm_mixer.h"
#include "np_playback.h"
#include "../../bcmfw/jobs/jviz.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
    printf("\n"); } } while (0)

/* ---- the stream: frame i has L = a(i), R = b(i) ------------------------ */
static int16_t sl(uint32_t i) { return (int16_t)((i * 37u) % 20000u) - 10000; }
static int16_t sr(uint32_t i) { return (int16_t)((i * 91u + 5u) % 16000u) - 8000; }
static uint32_t pack(int16_t l, int16_t r)
{
    return (uint16_t)l | ((uint32_t)(uint16_t)r << 16);
}

static uint32_t gen_pos;        /* the stream's next frame */
static void gen(uint32_t *f, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++, gen_pos++)
        f[i] = pack(sl(gen_pos), sr(gen_pos));
}

/* the downmix of frames [a, a + dec), as the reader defines it */
static int16_t want(int64_t a, uint32_t dec)
{
    int32_t s = 0;
    uint32_t sh = dec == 4 ? 3 : dec == 2 ? 2 : 1;
    for (uint32_t j = 0; j < dec; j++)
        if (a + j >= 0)
            s += sl((uint32_t)(a + j)) + sr((uint32_t)(a + j));
    return (int16_t)(s >> sh);
}

/* mid-read writes */
static int mid_writes, mid_tries;
static uint32_t mid_frame = 256;
static void midread(const struct viz_ring *r)
{
    static uint32_t buf[4096];
    if (mid_tries-- <= 0)
        return;
    for (int k = 0; k < mid_writes; k++)
    {
        gen(buf, mid_frame);
        viz_ring_write((struct viz_ring *)r, buf, mid_frame);
    }
}

static uint32_t ring_mem[8192];

static void t_sizes(void)
{
    CHECK(viz_ring_frames(512, 1, 256) == 1024, "512/1/256 -> %u",
          viz_ring_frames(512, 1, 256));
    CHECK(viz_ring_frames(256, 1, 256) == 1024, "256");
    CHECK(viz_ring_frames(512, 2, 512) == 2048, "512/2/512");
    CHECK(viz_tap_dec(44100) == 1 && viz_tap_dec(48000) == 1
          && viz_tap_dec(8000) == 1, "dec 1 up to 48 kHz");
    CHECK(viz_tap_dec(88200) == 2 && viz_tap_dec(96000) == 2, "dec 2");
    CHECK(viz_tap_dec(176400) == 4 && viz_tap_dec(192000) == 4, "dec 4");
}

/* fill frames of uneven sizes, read the last n at every step */
static void t_wrap_and_split(uint32_t n, uint32_t dec)
{
    struct viz_ring r;
    uint32_t fmax = 256 * dec, frames = viz_ring_frames(n, dec, fmax);
    uint32_t buf[1024], last = 0, sizes[] = { 256, 256, 100, 256, 3, 256, 255 };
    int16_t out[512];
    struct viz_tap_frame f;
    int bad = 0, reads = 0;

    viz_ring_init(&r, ring_mem, frames, fmax);
    gen_pos = 0;
    for (int step = 0; step < 400; step++)
    {
        uint32_t m = sizes[step % 7] * dec;
        gen(buf, m);
        viz_ring_write(&r, buf, m);
        unsigned fl = viz_ring_read(&r, out, n, dec, last, 0, &f);
        last = f.head;
        reads++;
        /* head: everything before the frame just written */
        if (f.head != gen_pos - m)
            bad++;
        bool under = f.head < n * dec;
        if (!!(fl & VIZ_TAP_UNDERRUN) != under || (fl & VIZ_TAP_TORN))
            bad++;
        int64_t base = (int64_t)f.head - (int64_t)n * dec;
        for (uint32_t i = 0; i < n; i++)
        {
            int16_t w = want(base + (int64_t)i * dec, dec);
            uint32_t at = (i & 1) ? n / 2 + i / 2 : i / 2;
            if (out[at] != w)
            {
                if (bad < 5)
                    printf("  n=%u dec=%u step %d i=%u got %d want %d\n",
                           n, dec, step, i, out[at], w);
                bad++;
                break;
            }
        }
    }
    CHECK(bad == 0, "wrap+split n=%u dec=%u: %d bad of %d reads "
          "(ring %u frames, wrapped %u times)", n, dec, bad, reads, frames,
          gen_pos / frames);
}

static void t_no_writer(void)
{
    struct viz_ring r;
    int16_t out[512];
    struct viz_tap_frame f;
    unsigned fl;

    viz_ring_init(&r, ring_mem, 1024, 256);
    memset(out, 0x55, sizeof(out));
    fl = viz_ring_read(&r, out, 512, 1, 0, 0, &f);
    CHECK(fl == (VIZ_TAP_UNDERRUN | VIZ_TAP_STALE | VIZ_TAP_SILENT),
          "no writer: flags %#x", fl);
    int nz = 0;
    for (int i = 0; i < 512; i++)
        nz += out[i] != 0;
    CHECK(nz == 0 && f.peak == 0, "no writer: zeros (%d not)", nz);
    fl = viz_ring_read(&r, out, 512, 1, f.head, 0, &f);
    CHECK(fl & VIZ_TAP_STALE, "no writer again: stale %#x", fl);
}

/* the writer runs 40 frames between two reads: the reader sees only the
 * newest window, never a mix, and nothing is torn */
static void t_writer_faster(void)
{
    struct viz_ring r;
    uint32_t buf[256], last = 0;
    int16_t out[512];
    struct viz_tap_frame f;
    unsigned fl;

    viz_ring_init(&r, ring_mem, 1024, 256);
    gen_pos = 0;
    for (int k = 0; k < 40; k++)
    {
        gen(buf, 256);
        viz_ring_write(&r, buf, 256);
    }
    fl = viz_ring_read(&r, out, 512, 1, last, 0, &f);
    CHECK(fl == 0, "faster writer: flags %#x", fl);
    CHECK(f.head == 39 * 256, "head %u", f.head);
    CHECK(out[0] == want(39 * 256 - 512, 1) && out[511] == want(39 * 256 - 1, 1),
          "faster writer: the newest window");
    last = f.head;
    fl = viz_ring_read(&r, out, 512, 1, last, 0, &f);
    CHECK(fl == VIZ_TAP_STALE, "same head: stale only (%#x)", fl);
}

/* writes in the middle of a read (the FIQ on the device) */
static void t_torn(void)
{
    struct viz_ring r;
    uint32_t buf[256];
    int16_t out[512];
    struct viz_tap_frame f;
    unsigned fl;

    viz_ring_init(&r, ring_mem, 1024, 256);
    gen_pos = 0;
    for (int k = 0; k < 4; k++)
    {
        gen(buf, 256);
        viz_ring_write(&r, buf, 256);
    }
    /* one frame arrives during the read: sized for it, not torn */
    mid_writes = 1; mid_tries = 1;
    fl = viz_ring_read(&r, out, 512, 1, 0, 0, &f);
    CHECK(!(fl & VIZ_TAP_TORN) && out[511] == want(3 * 256 - 1, 1),
          "one mid-read frame: intact (%#x)", fl);
    /* three frames on the first try only: retried, the newer window */
    mid_writes = 3; mid_tries = 1;
    fl = viz_ring_read(&r, out, 512, 1, 0, 0, &f);
    CHECK(!(fl & VIZ_TAP_TORN) && f.head == gen_pos - 256
          && out[511] == want(gen_pos - 256 - 1, 1),
          "torn once: the retry's window (%#x head %u)", fl, f.head);
    /* torn twice: zeros and the flag */
    mid_writes = 3; mid_tries = 2;
    fl = viz_ring_read(&r, out, 512, 1, 0, 0, &f);
    int nz = 0;
    for (int i = 0; i < 512; i++)
        nz += out[i] != 0;
    CHECK((fl & VIZ_TAP_TORN) && (fl & VIZ_TAP_SILENT) && nz == 0,
          "torn twice: zeros (%#x, %d not)", fl, nz);
    mid_writes = 0; mid_tries = 0;
}

static void t_silence_and_norm(void)
{
    struct viz_ring r;
    uint32_t buf[1024];
    int16_t out[512], ref[512];
    struct viz_tap_frame f;
    unsigned fl;

    /* 1-LSB dither is silence */
    viz_ring_init(&r, ring_mem, 1024, 256);
    for (int i = 0; i < 1024; i++)
        buf[i] = pack((int16_t)((i & 1) ? 1 : -2), (int16_t)((i & 2) ? 2 : -1));
    for (int k = 0; k < 4; k++)
        viz_ring_write(&r, buf + 256 * k, 256);
    fl = viz_ring_read(&r, out, 512, 1, 0, VIZ_RD_NORM, &f);
    CHECK(fl == VIZ_TAP_SILENT && f.shift == 0, "dither: silent, unshifted "
          "(%#x shift %u)", fl, f.shift);

    /* a -30 dBFS sine at 3 kHz: shifted 5 bits, the levels undo exactly */
    for (int i = 0; i < 1024; i++)
    {
        int16_t v = (int16_t)lrint(1000.0 * sin(2 * M_PI * 3000.0 * i / 44100.0));
        buf[i] = pack(v, v);
    }
    viz_ring_init(&r, ring_mem, 1024, 256);
    for (int k = 0; k < 4; k++)
        viz_ring_write(&r, buf + 256 * k, 256);
    viz_ring_read(&r, ref, 512, 1, 0, 0, &f);
    fl = viz_ring_read(&r, out, 512, 1, 0, VIZ_RD_NORM, &f);
    CHECK(f.peak == 1000 && f.shift == 5 && !(fl & ~VIZ_TAP_STALE),
          "norm: peak %d shift %u flags %#x", f.peak, f.shift, fl);
    int bad = 0;
    for (int i = 0; i < 512; i++)
        bad += out[i] != ref[i] * 32;
    CHECK(bad == 0, "norm: every sample x32 (%d not)", bad);

    /* through the CPU path (jviz): the peak band is the sine's, and the
     * shift comes off the levels at 32 l2 units a bit */
    static uint32_t p0[256], p1[256];
    uint16_t e[3] = { 1, 60, 256 }, l0[2], l1[2];
    jviz_power_ref(p0, ref, 512);
    jviz_power_ref(p1, out, 512);
    jviz_bands(l0, p0, 256, e, 2);
    jviz_bands(l1, p1, 256, e, 2);
    uint32_t pk = 0;
    for (int k = 1; k < 256; k++)
        if (p1[k] > p1[pk])
            pk = k;
    CHECK(pk == 35 || pk == 34, "peak bin %u (3 kHz / 86.1 Hz = 34.8)", pk);
    int d = (int)l1[0] - 32 * f.shift - (int)l0[0];
    CHECK(d >= -3 && d <= 3, "band level: shifted %u - 160 vs %u (%d)",
          l1[0], l0[0], d);
    printf("  norm: -30 dBFS sine, band l2 %u unshifted, %u shifted "
           "(-%u = %d)\n", l0[0], l1[0], 32 * f.shift, l1[0] - 32 * f.shift);
}

/* ---- the lag: what the reader gets against what the DAC plays -------- */
/* The mixer double-buffers (pcm_mixer.c): it mixes two frames before the
 * DMA starts, and at the start of each frame's DMA (the FIQ, status
 * STARTED) mixes the one after it, which is when the tap sees it.  The
 * DAC plays frame k over [kF, (k+1)F).  The reader runs at about 30 fps
 * at random phases. */
static void t_lag(void)
{
    const double rate = 44100.0;
    const uint32_t F = 256, n = 512;
    struct viz_ring r;
    uint32_t buf[256], last = 0;
    int16_t out[512];
    struct viz_tap_frame f;
    double worst = 0, best = 1e9, sum = 0;
    int reads = 0, bad = 0;

    viz_ring_init(&r, ring_mem, viz_ring_frames(n, 1, F), F);
    gen_pos = 0;
    srand(7);
    /* two frames before the DMA starts */
    gen(buf, F); viz_ring_write(&r, buf, F);
    gen(buf, F); viz_ring_write(&r, buf, F);
    double t_next_read = 0.040;     /* s */
    for (uint32_t k = 1; k < 3000; k++)     /* frame k starts playing */
    {
        double t0 = k * F / rate, t1 = (k + 1) * F / rate;
        gen(buf, F); viz_ring_write(&r, buf, F);
        while (t_next_read >= t0 && t_next_read < t1)
        {
            viz_ring_read(&r, out, n, 1, last, 0, &f);
            last = f.head;
            /* the window's newest sample, by its value: sl(i) + sr(i) */
            double dac = t_next_read * rate;    /* the frame at the DAC */
            double lead = (f.head - dac) / rate * 1000.0;
            if (out[n - 1] != want(f.head - 1, 1))
                bad++;
            if (lead > worst) worst = lead;
            if (lead < best) best = lead;
            sum += lead;
            reads++;
            t_next_read += 1.0 / 30 + (rand() % 1000) / 1e6;
        }
    }
    printf("  lag, mixer tap: %d reads; the window's end leads the DAC by "
           "%.2f..%.2f ms (mean %.2f), one mixer frame = %.2f ms\n",
           reads, best, worst, sum / reads, F / rate * 1000);
    CHECK(bad == 0, "lag: the window ends at head (%d not)", bad);
    CHECK(best >= 0 && worst <= F / rate * 1000 + 1e-6,
          "lag: within one mixer frame (%.2f..%.2f ms)", best, worst);
    CHECK(worst < 33.3, "lag: within one 30 fps frame");

    /* the same reads from a tap at the DSP's output (pcmbuf's input):
     * the codec keeps pcmbuf between its watermark (2 s) and full (3 s,
     * MIN_BUFFER_SIZE), so that tap runs ahead of the ear by its fill */
    double fill_lo = 2.0, fill_hi = 3.0, w2 = 0, b2 = 1e9, fill = fill_hi;
    for (int i = 0; i < reads; i++)
    {
        /* the codec tops up to full when under the watermark */
        fill -= 1.0 / 30;
        if (fill < fill_lo)
            fill = fill_hi;
        if (fill * 1000 > w2) w2 = fill * 1000;
        if (fill * 1000 < b2) b2 = fill * 1000;
    }
    printf("  lag, a tap after the DSP (model): leads by %.0f..%.0f ms "
           "(pcmbuf's fill), %.0fx one 30 fps frame\n", b2, w2, w2 / 33.3);
    CHECK(b2 > 33.3, "the post-DSP tap is out of sync (model)");
}

/* ---- viz_tap.c over stubs ------------------------------------------- */
static mixer_tap_fn s_tap;
static unsigned s_rate = 44100;
static int s_status = 1;
static struct mp3entry s_track = { "/Music/a.flac", false, false };
static uint32_t s_pbtrack = 1;
static size_t s_avail = 0;
static int s_allocs;
static unsigned char s_heap[32768];

void mixer_set_tap(mixer_tap_fn fn) { s_tap = fn; }
unsigned int mixer_get_frequency(void) { return s_rate; }
int audio_status(void) { return s_status; }
struct mp3entry *audio_current_track(void) { return &s_track; }
uint32_t np_pb_track(void) { return s_pbtrack; }
bool np_pb_is_book(const struct mp3entry *id3, bool *pod)
{
    *pod = id3->podcast;
    return id3->is_audiobook || id3->podcast;
}
int core_alloc(size_t size) { s_allocs++; return size <= sizeof(s_heap) ? 1 : -1; }
void core_pin(int h) { (void)h; }
int core_free(int h) { (void)h; s_allocs--; return 0; }
void *core_get_data(int h) { (void)h; return s_heap; }
size_t core_available(void) { return s_avail; }

static void mixer_frames(uint32_t frames_n)
{
    static uint32_t buf[256];
    for (uint32_t k = 0; k < frames_n; k++)
    {
        gen(buf, 256);
        if (s_tap)
            s_tap(buf, sizeof(buf));
    }
}

static void t_glue(void)
{
    static unsigned char mem[8192];
    int16_t out[512];
    struct viz_tap_frame f;
    unsigned fl;
    size_t need = viz_tap_bytes(512);

    printf("  viz_tap_bytes(512) = %zu (the PP's 256-frame mixer)\n", need);
    CHECK(need <= 4096 + 64 + 15, "bytes %zu", need);
    CHECK(viz_tap_open(300, mem, sizeof(mem)) == VIZ_TAP_E_SIZE, "size");
    CHECK(viz_tap_open(512, mem, need - 1) == VIZ_TAP_E_NOMEM, "short mem");
    CHECK(!s_tap, "closed: no tap set");
    fl = viz_tap_read(out, 0, &f);
    CHECK(fl == VIZ_TAP_CLOSED, "closed read %#x", fl);

    /* playing, no free memory: never a core_alloc (it would shrink the
     * audio buffer: a stop and restart) */
    s_status = 1; s_avail = 0;
    CHECK(viz_tap_open(512, NULL, 0) == VIZ_TAP_E_NOMEM && s_allocs == 0,
          "playing, NULL mem: refused without an allocation");
    s_avail = 1 << 20;
    CHECK(viz_tap_open(512, NULL, 0) == VIZ_TAP_OK && s_allocs == 1,
          "playing, free memory: allocated");
    viz_tap_close();
    CHECK(s_allocs == 0 && !s_tap, "close frees and unhooks");

    /* an audiobook playing: refused */
    s_track.is_audiobook = true;
    CHECK(viz_tap_open(512, mem, sizeof(mem)) == VIZ_TAP_E_BOOK && !s_tap,
          "book: open refused");
    s_track.is_audiobook = false;
    s_track.podcast = true;
    CHECK(viz_tap_open(512, mem, sizeof(mem)) == VIZ_TAP_E_BOOK,
          "podcast (the shell's is_audiobook): refused");
    s_track.podcast = false;

    /* music: samples */
    CHECK(viz_tap_open(512, mem, sizeof(mem)) == VIZ_TAP_OK && s_tap,
          "music: open");
    CHECK(viz_tap_open(512, mem, sizeof(mem)) == VIZ_TAP_E_BUSY, "busy");
    gen_pos = 0;
    mixer_frames(6);
    fl = viz_tap_read(out, 0, &f);
    CHECK(fl == 0 && f.rate == 44100 && f.head == 5 * 256
          && out[511] == want(5 * 256 - 1, 1), "music: fresh window %#x", fl);
    fl = viz_tap_read(out, 0, &f);
    CHECK(fl == VIZ_TAP_STALE, "no new frame: stale %#x", fl);

    /* the queue moves on to a book (np_pb_track changes): zeros */
    s_track.is_audiobook = true; s_pbtrack++;
    mixer_frames(2);
    fl = viz_tap_read(out, 0, &f);
    int nz = 0;
    for (int i = 0; i < 512; i++)
        nz += out[i] != 0;
    CHECK((fl & VIZ_TAP_BOOK) && nz == 0, "book mid-open: zeros %#x", fl);
    /* and back to music */
    s_track.is_audiobook = false; s_pbtrack++;
    mixer_frames(1);
    fl = viz_tap_read(out, 0, &f);
    CHECK(!(fl & VIZ_TAP_BOOK) && out[511] == want(gen_pos - 256 - 1, 1),
          "music again %#x", fl);
    viz_tap_close();
    CHECK(!s_tap && !viz_tap_is_open(), "closed");
    mixer_frames(1);    /* a frame after close touches nothing */
}

int main(void)
{
    t_sizes();
    t_wrap_and_split(512, 1);
    t_wrap_and_split(256, 1);
    t_wrap_and_split(512, 2);
    t_wrap_and_split(256, 4);
    t_no_writer();
    t_writer_faster();
    t_torn();
    t_silence_and_norm();
    t_lag();
    t_glue();
    printf("viztap: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
