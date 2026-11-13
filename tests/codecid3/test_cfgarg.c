
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

/* Host test: a codec's configure() setting that is a pointer
 * (REPLAYGAIN_SET_GAINS: a struct on the COP worker's stack) read by the
 * CPU's h_configure() (apps/codec_cop.c). The laptop's two-cache model
 * found the CPU reading the stack's DEADBEEF fill (2026-11-08).
 *
 * The model, line-granular (16 B), never evicting unless told:
 *  - the CPU fills the worker's stack with DEADBEEF through its cache and
 *    writes it back without dropping it (create_thread(), thread.c);
 *  - per track the COP writes the gains on its stack, then does what
 *    copj_cache_ops(0, COPJ_CALL) says (the real table);
 *  - the CPU's handler reads them: with the fix through cop_cfg_arg()
 *    from the uncached alias (memory); with -DCFG_OLD through its cache
 *    (0.8.0-rc.6's h_configure()), which must fail, so the model is shown
 *    able to see the hazard on every run.
 * Random evictions between steps vary what stays resident. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cop_job_core.h"
#include "codec_cop_cfg.h"

#define LINE    16
#define SZ      4096
#define NL      (SZ / LINE)
#define STACK0  1024                /* the worker's stack, [STACK0, STACKE) */
#define STACKE  3072
#define SET_GAINS 0x55              /* stands in for REPLAYGAIN_SET_GAINS */
#define DEADBEEF 0xDEADBEEFu

struct gains { int32_t track_gain, album_gain, track_peak, album_peak; };

static uint8_t mem[SZ];
struct cache { int valid[NL], dirty[NL]; uint8_t d[NL][LINE]; };
static struct cache cpu, cop;
static unsigned rng = 1;

static unsigned rnd(void)
{
    rng = rng * 1103515245u + 12345u;
    return rng >> 8;
}

static void c_load(struct cache *c, unsigned l)
{
    if (!c->valid[l])
    {
        memcpy(c->d[l], &mem[l * LINE], LINE);
        c->valid[l] = 1;
        c->dirty[l] = 0;
    }
}

static void c_write(struct cache *c, unsigned a, const void *p, unsigned n)
{
    const uint8_t *s = p;

    for (unsigned i = 0; i < n; i++, a++)
    {
        unsigned l = a / LINE;

        c_load(c, l);               /* write-allocate */
        c->d[l][a % LINE] = s[i];
        c->dirty[l] = 1;
    }
}

static __attribute__((unused)) void c_read(struct cache *c, unsigned a, void *p,
                                           unsigned n)
{
    uint8_t *d = p;

    for (unsigned i = 0; i < n; i++, a++)
    {
        unsigned l = a / LINE;

        c_load(c, l);
        d[i] = c->d[l][a % LINE];
    }
}

static void c_commit(struct cache *c, int discard)
{
    for (unsigned l = 0; l < NL; l++)
    {
        if (c->valid[l] && c->dirty[l])
            memcpy(&mem[l * LINE], c->d[l], LINE);
        c->dirty[l] = 0;
        if (discard)
            c->valid[l] = 0;
    }
}

static void c_apply(struct cache *c, unsigned ops)
{
    if (ops & (COPJ_C_COMMIT_DISCARD | COPJ_C_COMMIT_DISCARD_I))
        c_commit(c, 1);
    else if (ops & COPJ_C_COMMIT)
        c_commit(c, 0);
}

/* drop (after writing back) each valid line with probability pct % */
static void c_evict(struct cache *c, unsigned pct)
{
    for (unsigned l = 0; l < NL; l++)
        if (c->valid[l] && rnd() % 100 < pct)
        {
            if (c->dirty[l])
                memcpy(&mem[l * LINE], c->d[l], LINE);
            c->valid[l] = c->dirty[l] = 0;
        }
}

static const struct cop_cfg_ptr tab[] = { { SET_GAINS, sizeof(struct gains) } };

/* The CPU's handler: the gains the DSP gets */
static struct gains handler(unsigned addr)
{
    struct gains got;
#ifdef CFG_OLD
    c_read(&cpu, addr, &got, sizeof(got));      /* the raw pointer */
#else
    long copy[COP_CFG_COPY_MAX / sizeof(long)];
    size_t size = cop_cfg_ptr_size(tab, 1, SET_GAINS);
    intptr_t v = cop_cfg_arg(size, (intptr_t)addr, copy, &mem[addr]);

    if (v != (intptr_t)copy)
    {
        printf("FAIL: the pointer setting was not copied\n");
        exit(1);
    }
    memcpy(&got, copy, sizeof(got));            /* the CPU's own stack */
#endif
    return got;
}

int main(void)
{
    unsigned stale = 0, deadbeef = 0, runs = 2000, tracks = 0;

    /* the helper's value paths */
    {
        long copy[4];
        if (cop_cfg_ptr_size(tab, 1, SET_GAINS + 1) != 0 ||
            cop_cfg_arg(0, 1234, copy, NULL) != 1234 ||
            cop_cfg_arg(sizeof(struct gains), 0, copy, NULL) != 0)
        {
            printf("FAIL: a value setting or NULL was changed\n");
            return 1;
        }
    }

    for (unsigned r = 0; r < runs; r++)
    {
        /* a gains frame near the stack's top, sometimes across two lines */
        unsigned addr = STACKE - 64 - (rnd() % 8) * 8;

        memset(&cpu, 0, sizeof(cpu));
        memset(&cop, 0, sizeof(cop));
        memset(mem, 0, sizeof(mem));
        /* create_thread(): the fill on the CPU, written back, kept */
        for (unsigned a = STACK0; a < STACKE; a += 4)
        {
            uint32_t w = DEADBEEF;
            c_write(&cpu, a, &w, 4);
        }
        c_commit(&cpu, 0);
        c_evict(&cpu, rnd() % 100);

        for (unsigned t = 0; t < 5; t++, tracks++)
        {
            struct gains g = {
                (int32_t)rnd(), (int32_t)rnd(), (int32_t)rnd(),
                (int32_t)rnd()
            }, got;

            c_write(&cop, addr, &g, sizeof(g));
            c_apply(&cop, copj_cache_ops(0, COPJ_CALL, 1));
            got = handler(addr);
            c_apply(&cop, copj_cache_ops(0, COPJ_RETURN, 1));
            if (memcmp(&got, &g, sizeof(g)))
            {
                stale++;
                if ((uint32_t)got.track_gain == DEADBEEF ||
                    (uint32_t)got.album_peak == DEADBEEF)
                    deadbeef++;
            }
            c_evict(&cpu, rnd() % 100);      /* the CPU's work between */
            c_evict(&cop, rnd() % 50);
        }
    }
    if (stale)
    {
        printf("FAIL: the CPU read stale gains %u of %u tracks "
               "(stack fill DEADBEEF %u)\n", stale, tracks, deadbeef);
        return 1;
    }
    printf("gains across cores: %u tracks in %u runs, 0 stale\n", tracks,
           runs);
    return 0;
}
