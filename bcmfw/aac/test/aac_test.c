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
 * Host harness for bcmfw/aac: decodes an .au file (adts2au.py) with the
 * same libfaad sources and adapter the VideoCore image runs, and writes
 * the reference the vc2sim test compares against byte for byte.
 *
 *   aac_test IN.au OUT.s16 [OUT.raw]
 *     OUT.s16  interleaved 16-bit stereo PCM, every frame aac_decode
 *              returned, in order
 *     OUT.raw  per AU: u32 frames, then libfaad's 32-bit output
 *              (time_out[0], time_out[1]: 1024 words each), all AUs,
 *              priming AU included
 *   aac_test --reset IN.au   aac_reset() mid-stream must reproduce a
 *              fresh aac_open() fed from the same AU
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
#include <stdlib.h>
#include <string.h>
#include "aac.h"

struct au_file
{
    uint8_t *data;
    uint32_t size;
    const uint8_t *asc;
    uint32_t asc_len;
    uint32_t pos;           /* next AU record */
};

static uint32_t rd32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int load(const char *path, struct au_file *f)
{
    FILE *fp = fopen(path, "rb");
    long n;

    if (!fp)
        return -1;
    fseek(fp, 0, SEEK_END);
    n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    f->data = malloc(n > 0 ? n : 1);
    if (!f->data || fread(f->data, 1, n, fp) != (size_t)n || n < 8 ||
        memcmp(f->data, "AACU", 4))
    {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    f->size = n;
    f->asc_len = rd32(f->data + 4);
    f->asc = f->data + 8;
    f->pos = 8 + f->asc_len;
    return f->pos <= f->size ? 0 : -1;
}

/* the next AU, or NULL at the end */
static const uint8_t *next_au(struct au_file *f, uint32_t *len)
{
    const uint8_t *p;

    if (f->pos + 4 > f->size)
        return NULL;
    *len = rd32(f->data + f->pos);
    if (f->pos + 4 + *len > f->size)
        return NULL;
    p = f->data + f->pos + 4;
    f->pos += 4 + *len;
    return p;
}

static uint32_t fnv(uint32_t h, const void *p, size_t n)
{
    const uint8_t *b = p;

    while (n--)
        h = (h ^ *b++) * 16777619u;
    return h;
}

static int test_reset(struct au_file *f)
{
    /* decode to AU 40, reset, decode 40..79; then a fresh open at AU 40:
     * the two runs must agree from the reset on, to within 2 LSB: PNS's
     * noise generator (common.c random_int) is a static that neither an
     * open nor a reset restarts, so noise-filled bands differ slightly */
    static int16_t a[80][AAC_FRAME_LEN * 2], b[80][AAC_FRAME_LEN * 2];
    static int16_t pcm[AAC_FRAME_LEN * 2];
    uint32_t starts[80], lens[80], len;
    int na[80], nb[80];
    const uint8_t *au;
    int k, i, n = 0, bad = 0, worst = 0;

    while (n < 80 && (au = next_au(f, &len)) != NULL)
    {
        starts[n] = au - f->data;
        lens[n++] = len;
    }
    if (n < 80 || aac_open(f->asc, f->asc_len, NULL, NULL))
        return 1;
    for (k = 0; k < 80; k++)
    {
        if (k == 40)
            aac_reset();
        na[k] = aac_decode(f->data + starts[k], lens[k], pcm);
        memcpy(a[k], pcm, sizeof(pcm));
    }
    if (aac_open(f->asc, f->asc_len, NULL, NULL))
        return 1;
    for (k = 40; k < 80; k++)
    {
        nb[k] = aac_decode(f->data + starts[k], lens[k], pcm);
        memcpy(b[k], pcm, sizeof(pcm));
        if (na[k] != nb[k])
            bad++;
        for (i = 0; i < 2 * na[k]; i++)
        {
            int d = abs(a[k][i] - b[k][i]);
            worst = d > worst ? d : worst;
        }
    }
    bad += worst > 2;
    printf("reset: AUs 40-79 after aac_reset %s a fresh open (first AU %d "
           "frames, largest difference %d)\n", bad ? "DIFFER from" : "match",
           na[40], worst);
    return bad != 0;
}

int main(int argc, char **argv)
{
    static int16_t pcm[AAC_FRAME_LEN * 2];
    struct au_file f;
    FILE *out, *raw = NULL;
    uint32_t rate = 0, ch = 0, len, aus = 0, frames = 0, h = 2166136261u;
    uint32_t primed = 0;
    int errors = 0, r;
    const uint8_t *au;

    if (argc == 3 && !strcmp(argv[1], "--reset"))
    {
        if (load(argv[2], &f))
            return 2;
        return test_reset(&f);
    }
    if (argc < 3 || argc > 4)
    {
        fprintf(stderr, "usage: aac_test IN.au OUT.s16 [OUT.raw] | --reset IN.au\n");
        return 2;
    }
    if (load(argv[1], &f))
    {
        fprintf(stderr, "aac_test: cannot read %s\n", argv[1]);
        return 2;
    }
    r = aac_open(f.asc, f.asc_len, &rate, &ch);
    if (r)
    {
        fprintf(stderr, "aac_test: aac_open: %d\n", r);
        return 1;
    }
    out = fopen(argv[2], "wb");
    if (argc == 4)
        raw = fopen(argv[3], "wb");
    if (!out || (argc == 4 && !raw))
        return 2;
    while ((au = next_au(&f, &len)) != NULL)
    {
        r = aac_decode(au, len, pcm);
        aus++;
        if (r < 0)
        {
            errors++;
            fprintf(stderr, "aac_test: AU %u: error %d\n", aus - 1, r);
            r = 0;
        }
        if (r == 0)
            primed++;
        fwrite(pcm, 4, r, out);
        h = fnv(h, pcm, 4 * r);
        frames += r;
        if (raw)
        {
            uint32_t n = r;
            fwrite(&n, 4, 1, raw);
            fwrite(aac_raw_output(0), 4, AAC_FRAME_LEN, raw);
            fwrite(aac_raw_output(1), 4, AAC_FRAME_LEN, raw);
        }
    }
    fclose(out);
    if (raw)
        fclose(raw);
    printf("%s: %u Hz, %u ch, %u AUs, %u frames out (%u with none), %d errors, "
           "pcm fnv %08x\n", argv[1], rate, ch, aus, frames, primed, errors, h);
    return errors != 0;
}
