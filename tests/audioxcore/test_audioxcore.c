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
 * The audio output path across the PP5022's two cores (P19 ucop-fix, owner
 * bench sitting 3, 2026-10-26: a tone while scrolling with the UI on the
 * COP and no music; artefacts going back to the menus with music).
 *
 * A model, not the firmware: the mixer's frame logic (firmware/pcm_mixer.c
 * mixer_buffer_callback(), mixer_channel_play_data(), mixer_start_pcm())
 * and the DAC standby flag (firmware/drivers/audio/wm8758.c dac_wake()),
 * with the placement as built in 7e8c140e40:
 *   - the channel table and the two downmix frames in IRAM (uncached,
 *     coherent);
 *   - the mixer's control words (downmix_index, next_size, idle_counter)
 *     in cached .bss, on one 16-byte line;
 *   - the DAC's standby state in cached .bss.
 * Each core has its own write-back copy of a cached line (the PP5022's
 * caches are not coherent: quirks 8 and 52), evicted at seeded random
 * points; the COP writes its cache back before each input wait and drops it
 * after each call to the back end (fe_be.c), as the shell does.
 *
 * Rule under test: the mixer and the PCM driver are CPU-only; DAC state both
 * cores use is uncached.
 *   AS BUILT  a click starts the beep channel on the COP:  the reset of
 *             idle_counter stays in the COP's cache, the CPU's FIQ skips
 *             the idle frames' memset and replays the click's frame until
 *             the idle tail ends: the tone (must be caught on most seeds).
 *   ROUTED    the click is posted to the CPU (BE_SOUND): exact output on
 *             every seed.
 *   DAC       a click's dac_wake() on the COP with a cached standby flag
 *             replays a standby wake (PLL and DAC power writes) over live
 *             music; uncached, never.
 *   EQ SET    (ucop-fix2) the DSP's EQ bands, one cache line each, read
 *             by the codec on the CPU at every frame.  Written on the COP
 *             (as built): the codec sees half-new sets and keeps a stale
 *             band after the COP wrote it back (its own valid line).  One
 *             remote call per band: the codec runs between the calls and
 *             sees half sets.  The whole set in one call on the CPU
 *             (ui_eq_dsp_set): never.
 *   PRESCALER (ucop-fix2) sound.c's prescaler: the volume written on the
 *             COP and the balance on the CPU share a line; a write-back
 *             of either core's copy loses the other's write.  Both on the
 *             CPU (the setters routed): never.
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
#include <stdbool.h>

#define F          64           /* samples per mix frame */
#define CLICK      40           /* the click's samples (< one frame) */
#define MAX_IDLE   30           /* idle frames before the PCM stops */
#define CPU        0
#define COP        1
#define SEEDS      300

static unsigned rng;
static unsigned rnd(unsigned n)
{
    rng = rng * 1103515245u + 12345u;
    return ((rng >> 16) & 0x7fff) % n;
}

/* ---- one cached line, two incoherent copies ------------------------- */
enum { W_IDX, W_NEXT, W_IDLE, W_STANDBY, NW };
static int mem[NW];
static struct { bool valid, dirty; int v[NW]; } line[2];
static bool uncached;           /* the line linked uncached (the fix) */

static void wb(int c)
{
    if (line[c].valid && line[c].dirty)
        memcpy(mem, line[c].v, sizeof(mem));
    line[c].dirty = false;
}
static void evict(int c) { wb(c); line[c].valid = false; }
static int ld(int c, int w)
{
    if (uncached)
        return mem[w];
    if (!line[c].valid)
    {
        memcpy(line[c].v, mem, sizeof(mem));
        line[c].valid = true;
        line[c].dirty = false;
    }
    return line[c].v[w];
}
static void st(int c, int w, int v)
{
    if (uncached)
    {
        mem[w] = v;
        return;
    }
    ld(c, w);
    line[c].v[w] = v;
    line[c].dirty = true;
}

/* ---- IRAM: coherent ------------------------------------------------- */
static int buf[2][F];
static struct { bool active; int pos, size, last; } chan;
static bool pcm_playing;        /* SHAREDBSS in pcm.c: coherent */
static int play_buf, play_size; /* the DMA's current frame */
static long out_nonzero, frames_out;

/* mixer_buffer_callback(), the model's subset: one channel */
static void buffer_callback(int c)
{
    int idx = ld(c, W_IDX) ^ 1, next = 0, mix = F, *p;

    st(c, W_IDX, idx);
    p = buf[idx];
    if (chan.active)
    {
        chan.pos += chan.last;
        chan.size -= chan.last;
        if (chan.size == 0)
            chan.active = false;    /* channel_stopped() */
    }
    if (chan.active)
    {
        int i, m = chan.size < mix ? chan.size : mix;
        for (i = 0; i < m; i++)
            p[i] = 1000 + chan.pos + i;     /* the click's samples */
        chan.last = m;
        next = m;
        p += m;
        if (next < F)
            goto idle;              /* "fill_frame" with no channel left */
    }
    else
    {
idle:;
        int idle = ld(c, W_IDLE);
        st(c, W_IDLE, idle + 1);
        if (idle < MAX_IDLE)
        {
            if (idle + 1 <= 3)
                memset(p, 0, (F - next) * sizeof(int));
            next = F;
        }
    }
    st(c, W_NEXT, next);
}

/* the FIQ at the end of a frame (CPU): output it, take the next */
static void fiq(void)
{
    int i;

    for (i = 0; i < play_size; i++)
        out_nonzero += buf[play_buf][i] != 0;
    frames_out++;
    play_buf = ld(CPU, W_IDX);              /* mixer_pcm_callback() */
    play_size = ld(CPU, W_NEXT);
    if (play_size == 0)
    {
        pcm_playing = false;                /* the PCM stops */
        return;
    }
    buffer_callback(CPU);                   /* the status callback */
}

/* ipodui_click_play() on core c: mixer_channel_stop(), then
 * mixer_channel_play_data() on the beep channel */
static void play_click(int c)
{
    chan.active = false;                    /* channel_stopped() */
    chan.size = 0;
    if (!chan.active)
        st(c, W_IDLE, 0);                   /* mixer_activate_channel() */
    chan.active = true;
    chan.pos = 0;
    chan.size = CLICK;
    chan.last = 0;
    if (!pcm_playing)                       /* mixer_start_pcm() */
    {
        buffer_callback(c);
        play_buf = ld(c, W_IDX);
        buffer_callback(c);
        play_size = F;
        pcm_playing = true;
    }
}

/* clicks at random spacing (a scroll), the idle tail; the click data out
 * must equal what went in, counted in samples */
static long run_clicks(unsigned seed, int click_core, bool unc)
{
    int k, f, clicks = 0;

    rng = seed;
    uncached = unc;
    memset(mem, 0, sizeof(mem));
    memset(line, 0, sizeof(line));
    memset(buf, 0, sizeof(buf));
    memset(&chan, 0, sizeof(chan));
    pcm_playing = false;
    out_nonzero = frames_out = 0;
    for (k = 0; k < 24; k++)
    {
        int gap = 1 + rnd(8);
        play_click(click_core);
        clicks++;
        if (click_core == COP)
            wb(COP);                        /* fe_wait_begin() */
        for (f = 0; f < gap; f++)
        {
            if (pcm_playing)
                fiq();
            if (rnd(8) == 0)
                evict(CPU);                 /* the codec, the art thread */
            if (rnd(2) == 0)
                evict(COP);                 /* a call's drop, or pressure */
        }
    }
    for (f = 0; f < 4 * MAX_IDLE && pcm_playing; f++)
        fiq();
    return out_nonzero - (long)clicks * CLICK;
}

/* ---- the DAC's standby flag -------------------------------------------
 * The COP's UI idle poll put the codec in standby; the CPU woke it for
 * music (mixer_channel_play_data() -> dac_wake()); then the user scrolls
 * and each click's dac_wake() on the COP reads the flag. A read of 1 with
 * music playing replays the standby wake's register writes over it. */
static int run_dac(unsigned seed, bool unc)
{
    int k, replays = 0;

    rng = seed;
    uncached = unc;
    memset(mem, 0, sizeof(mem));
    memset(line, 0, sizeof(line));
    st(COP, W_STANDBY, 1);                  /* dac_standby_enter(), COP */
    if (rnd(2))
        wb(COP);                            /* the COP's next input wait */
    if (ld(CPU, W_STANDBY))                 /* dac_wake(), CPU: music */
        st(CPU, W_STANDBY, 0);
    for (k = 0; k < 8; k++)                 /* clicks on the COP */
    {
        if (rnd(4) == 0)
            evict(CPU);
        if (rnd(4) == 0)
            evict(COP);
        if (ld(COP, W_STANDBY))
        {
            replays++;                      /* dac_standby_wake() again */
            st(COP, W_STANDBY, 0);
        }
        wb(COP);
    }
    return replays;
}

/* ---- the EQ set: NB lines, two incoherent copies each ---------------- */
#define NB  5
static int eq_mem[NB];
static struct { bool valid, dirty; int v; } eq_line[2][NB];

static void eq_evict(int c, int b)
{
    if (eq_line[c][b].valid && eq_line[c][b].dirty)
        eq_mem[b] = eq_line[c][b].v;
    eq_line[c][b].valid = eq_line[c][b].dirty = false;
}
static int eq_ld(int c, int b)
{
    if (!eq_line[c][b].valid)
    {
        eq_line[c][b].v = eq_mem[b];
        eq_line[c][b].valid = true;
        eq_line[c][b].dirty = false;
    }
    return eq_line[c][b].v;
}
static void eq_st(int c, int b, int v)
{
    eq_ld(c, b);
    eq_line[c][b].v = v;
    eq_line[c][b].dirty = true;
}

/* the codec's frame (CPU): reads every band; a set mixing generations is
 * torn.  Returns the set's generation when whole, -1 when torn */
static int eq_frame(void)
{
    int b, g = eq_ld(CPU, 0);

    for (b = 1; b < NB; b++)
        if (eq_ld(CPU, b) != g)
            return -1;
    return g;
}

enum { EQ_COP, EQ_PER_CALL, EQ_SET };

/* 6 applies (generations 1..6) with codec frames and evictions between;
 * returns torn frames, plus 1000 if the codec never settles on the last
 * set within 40 frames after the last apply */
static int run_eq(unsigned seed, int how)
{
    int k, b, f, torn = 0, settled = 0;

    rng = seed;
    memset(eq_mem, 0, sizeof(eq_mem));
    memset(eq_line, 0, sizeof(eq_line));
    for (b = 0; b < NB; b++)
        eq_ld(CPU, b);                      /* the codec's lines are hot */
    for (k = 1; k <= 6; k++)
    {
        if (how == EQ_SET)
        {
            /* be_svc: no yield inside the set */
            for (b = 0; b < NB; b++)
                eq_st(CPU, b, k);
        }
        else
        {
            for (b = 0; b < NB; b++)
            {
                eq_st(how == EQ_COP ? COP : CPU, b, k);
                if (rnd(3) == 0 && eq_frame() < 0)
                    torn++;                 /* the codec ran meanwhile */
                if (how == EQ_COP && rnd(4) == 0)
                    eq_evict(COP, rnd(NB));
            }
            if (how == EQ_COP)
                for (b = 0; b < NB; b++)
                    eq_evict(COP, b);       /* the COP's wait: written back */
        }
        for (f = 0; f < 1 + (int)rnd(4); f++)
        {
            if (eq_frame() < 0)
                torn++;
            if (rnd(6) == 0)
                eq_evict(CPU, rnd(NB));     /* the codec's own pressure */
        }
    }
    for (f = 0; f < 40 && !settled; f++)
    {
        int g = eq_frame();
        if (g < 0)
            torn++;
        settled = g == 6;
        if (rnd(6) == 0)
            eq_evict(CPU, rnd(NB));
    }
    return torn + (settled ? 0 : 1000);
}

/* ---- sound.c's prescaler: two fields on one line --------------------- */
static int run_prescaler(unsigned seed, bool routed)
{
    int k, lost = 0;

    rng = seed;
    uncached = false;
    memset(mem, 0, sizeof(mem));
    memset(line, 0, sizeof(line));
    for (k = 1; k <= 8; k++)
    {
        /* the volume (the UI's wheel) and the balance (a CPU caller) */
        st(routed ? CPU : COP, W_IDX, k);
        if (rnd(2))
            evict(COP);
        st(CPU, W_NEXT, k);
        if (rnd(2))
            evict(CPU);
        if (rnd(2))
            evict(COP);
    }
    evict(COP);
    evict(CPU);
    if (mem[W_IDX] != 8)
        lost++;
    if (mem[W_NEXT] != 8)
        lost++;
    return lost;
}

int main(void)
{
    unsigned s;
    int bad_as_built = 0, bad_routed = 0;
    int dac_cached = 0, dac_unc = 0;
    int eq_cop = 0, eq_call = 0, eq_set = 0, pre_cop = 0, pre_cpu = 0;
    long worst = 0;

    for (s = 1; s <= SEEDS; s++)
    {
        long d = run_clicks(s, COP, false);
        if (d)
            bad_as_built++;
        if (d > worst)
            worst = d;
        if (run_clicks(s, CPU, false))
            bad_routed++;
        if (run_dac(s, false))
            dac_cached++;
        if (run_dac(s, true))
            dac_unc++;
        if (run_eq(s, EQ_COP))
            eq_cop++;
        if (run_eq(s, EQ_PER_CALL))
            eq_call++;
        if (run_eq(s, EQ_SET))
            eq_set++;
        if (run_prescaler(s, false))
            pre_cop++;
        if (run_prescaler(s, true))
            pre_cpu++;
    }
    printf("audioxcore: as built (click on the COP): %d/%d seeds wrong, "
           "worst %ld extra samples (%ld frames' worth)\n",
           bad_as_built, SEEDS, worst, worst / F);
    printf("audioxcore: routed (click on the CPU): %d/%d seeds wrong\n",
           bad_routed, SEEDS);
    printf("audioxcore: DAC flag cached: %d/%d seeds replay a wake; "
           "uncached: %d/%d\n", dac_cached, SEEDS, dac_unc, SEEDS);
    printf("audioxcore: EQ set on the COP: %d/%d seeds torn or stale; "
           "one call per band: %d/%d torn; one call per set: %d/%d\n",
           eq_cop, SEEDS, eq_call, SEEDS, eq_set, SEEDS);
    printf("audioxcore: prescaler written on both cores: %d/%d seeds lose "
           "a write; on the CPU only: %d/%d\n", pre_cop, SEEDS, pre_cpu,
           SEEDS);
    if (bad_routed || dac_unc || eq_set || pre_cpu)
    {
        printf("FAIL: the fixed placement is not exact\n");
        return 1;
    }
    if (bad_as_built < SEEDS / 2 || dac_cached == 0 || eq_cop == 0 ||
        eq_call == 0 || pre_cop == 0)
    {
        printf("FAIL: the model no longer shows the as-built fault "
               "(negative control)\n");
        return 1;
    }
    return 0;
}
