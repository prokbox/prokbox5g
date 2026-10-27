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

/* Host driver for apps/video/mp4demux.c, with the same options as
   tests/video/mp4demux.py so tests/video/mp4demux_c_test.py can compare
   their outputs byte for byte:

     test_mp4demux FILE [--table OUT.csv] [--annexb OUT.264]
                        [--seek-us N] [--scratch BYTES]
                        [--index OUT.vix] [--build DIR]
     test_mp4demux --check-index FILE.vix FILE
     test_mp4demux --selftest

   --index writes the sample index with vix_write(); --build writes it
   the way the library update will, with vix_build_file() (a .tmp file,
   renamed), and prints its path; --check-index reads an index back with
   vix_read_header() and vix_read_record() and compares it with the
   demuxer's own samples.

   Reads go through pread(), a window at a time, as on the PP. */

#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "video/mp4demux.h"
#include "video/vix.h"
#include "video/vix_file.h"

static unsigned long reads;

static long read_at(void *ctx, uint64_t off, void *buf, unsigned long len)
{
    reads++;
    return (long)pread(*(int *)ctx, buf, len, (off_t)off);
}

/* mp4_annexb() with the NAL length sizes no corpus file uses, and its
   refusals (mp4demux.py packet() raises on the same inputs). */
static int selftest(void)
{
    static const uint8_t two[] = { 0, 2, 0x65, 0xaa, 0, 1, 0x41 };
    static const uint8_t one[] = { 2, 0x65, 0xaa, 1, 0x41 };
    static const uint8_t three[] = { 0, 0, 2, 0x65, 0xaa, 0, 0, 1, 0x41 };
    static const uint8_t want[] = { 0, 0, 0, 1, 0x65, 0xaa, 0, 0, 0, 1, 0x41 };
    static const uint8_t past[] = { 0, 3, 0x65, 0xaa };
    static const uint8_t trail[] = { 0, 1, 0x65, 0 };
    uint8_t out[32];
    int bad = 0;

    bad |= mp4_annexb(two, sizeof(two), 2, out, sizeof(out)) != sizeof(want)
           || memcmp(out, want, sizeof(want));
    bad |= mp4_annexb(one, sizeof(one), 1, out, sizeof(out)) != sizeof(want)
           || memcmp(out, want, sizeof(want));
    bad |= mp4_annexb(three, sizeof(three), 3, out, sizeof(out))
           != sizeof(want) || memcmp(out, want, sizeof(want));
    bad |= mp4_annexb(past, sizeof(past), 2, out, sizeof(out)) >= 0;
    bad |= mp4_annexb(trail, sizeof(trail), 2, out, sizeof(out)) >= 0;
    bad |= mp4_annexb(two, sizeof(two), 2, out, 6) != MP4_E_SPACE;
    printf("%s mp4_annexb with 1-, 2- and 3-byte lengths, and its refusals\n",
           bad ? "FAIL" : "ok  ");

    /* index names: 16 hex digits, different for a new size or mtime */
    {
        char a[VIX_NAME_LEN + 1], b[VIX_NAME_LEN + 1], c[VIX_NAME_LEN + 1];
        int nb = 0;
        vix_name("/Videos/Film.m4v", 1000, 5, a);
        vix_name("/Videos/Film.m4v", 1000, 6, b);
        vix_name("/Videos/Film.m4v", 1001, 5, c);
        nb |= strlen(a) != VIX_NAME_LEN || strspn(a, "0123456789abcdef")
              != VIX_NAME_LEN || !strcmp(a, b) || !strcmp(a, c);
        vix_name("/Videos/Film.m4v", 1000, 5, b);
        nb |= strcmp(a, b) != 0;
        printf("%s vix_name: %s, stable, changes with size and mtime\n",
               nb ? "FAIL" : "ok  ", a);
        bad |= nb;
    }
    return bad;
}

static int fail(const char *what, int rc)
{
    fprintf(stderr, "mp4demux: %s: error %d\n", what, rc);
    return 2;
}

static int file_write(void *ctx, const void *buf, unsigned long len)
{
    return fwrite(buf, 1, len, ctx) == len ? 0 : MP4_E_IO;
}

/* Every record of an index against the demuxer's samples. */
static int check_index(const char *vix, struct mp4_movie *m, void *scratch,
                       size_t scratch_len)
{
    struct vix_header h;
    struct mp4_iter it;
    struct mp4_sample s, r;
    uint8_t buf[VIX_HEADER_SIZE];
    FILE *f = fopen(vix, "rb");
    uint32_t i, bad = 0;
    int rc;

    if (!f || fread(buf, 1, sizeof(buf), f) != sizeof(buf) ||
        vix_read_header(buf, &h) < 0)
        return fail("index header", MP4_E_FORMAT);
    if (h.count != m->nsamples || h.width != m->width ||
        h.height != m->height || h.avcc_off != m->avcc.pos ||
        h.avcc_len != m->avcc.end - m->avcc.pos)
        return fail("index header fields", MP4_E_FORMAT);
    if ((rc = mp4_iter_init(&it, m, scratch, scratch_len)) < 0)
        return fail("iter", rc);
    for (i = 0; i < h.count; i++)
    {
        uint8_t rec[VIX_RECORD_SIZE];
        if (fread(rec, 1, sizeof(rec), f) != sizeof(rec) ||
            mp4_iter_next(&it, &s) != 1)
            return fail("index records", MP4_E_FORMAT);
        vix_read_record(rec, &r);
        bad += r.offset != s.offset || r.size != s.size || r.key != s.key ||
               r.pts_us != s.pts_us / 1000 * 1000;
    }
    fclose(f);
    printf("index %s: %u records, %u differ\n", vix, (unsigned)h.count,
           (unsigned)bad);
    return bad ? 3 : 0;
}

int main(int argc, char **argv)
{
    const char *table = NULL, *annexb = NULL, *index = NULL, *build = NULL;
    const char *check = NULL;
    long long seek_us = -1;
    size_t scratch_len = 6 * 1024;
    struct mp4_movie m;
    struct mp4_iter it;
    struct mp4_sample s;
    struct mp4_io io;
    struct stat st;
    uint8_t *scratch;
    uint32_t first = 0, nkey = 0, i;
    int past = 0;
    int fd, rc, a;

    if (argc == 2 && !strcmp(argv[1], "--selftest"))
        return selftest();
    if (argc == 4 && !strcmp(argv[1], "--check-index"))
    {
        check = argv[2];
        argv += 2;
        argc -= 2;
    }
    if (argc < 2)
    {
        fprintf(stderr, "usage: %s FILE [options]\n", argv[0]);
        return 1;
    }
    for (a = 2; a + 1 < argc; a += 2)
    {
        if (!strcmp(argv[a], "--table"))
            table = argv[a + 1];
        else if (!strcmp(argv[a], "--annexb"))
            annexb = argv[a + 1];
        else if (!strcmp(argv[a], "--seek-us"))
            seek_us = atoll(argv[a + 1]);
        else if (!strcmp(argv[a], "--index"))
            index = argv[a + 1];
        else if (!strcmp(argv[a], "--build"))
            build = argv[a + 1];
        else if (!strcmp(argv[a], "--scratch"))
            scratch_len = (size_t)atol(argv[a + 1]);
        else
        {
            fprintf(stderr, "unknown option %s\n", argv[a]);
            return 1;
        }
    }

    fd = open(argv[1], O_RDONLY);
    if (fd < 0 || fstat(fd, &st) < 0)
    {
        perror(argv[1]);
        return 1;
    }
    io.read_at = read_at;
    io.ctx = &fd;
    io.size = (uint64_t)st.st_size;
    if ((rc = mp4_open(&m, &io)) < 0)
        return fail("open", rc);
    scratch = malloc(scratch_len);

    /* pass 1: the table, the key count, the seek target */
    {
        FILE *f = table ? fopen(table, "w") : NULL;
        if ((rc = mp4_iter_init(&it, &m, scratch, scratch_len)) < 0)
            return fail("iter", rc);
        while ((rc = mp4_iter_next(&it, &s)) > 0)
        {
            if (f)
                fprintf(f, "%" PRIu64 ",%u,%" PRId64 ",%d\n", s.offset,
                        (unsigned)s.size, s.pts_us, s.key ? 1 : 0);
            nkey += s.key;
            /* the last key sample at or before seek_us */
            if (seek_us >= 0 && s.pts_us > seek_us)
                past = 1;
            if (seek_us >= 0 && !past && s.key)
                first = it.index - 1;
        }
        if (f)
            fclose(f);
        if (rc < 0)
            return fail("samples", rc);
    }
    printf("video %ux%u, timescale %u, %u samples, %u key, NAL length %u, "
           "shift %" PRId64 "; audio %s; %lu reads\n",
           (unsigned)m.width, (unsigned)m.height, (unsigned)m.timescale,
           (unsigned)m.nsamples, (unsigned)nkey, (unsigned)m.nal_length,
           m.shift, m.has_audio ? "yes" : "no", reads);

    if (check)
        return check_index(check, &m, scratch, scratch_len);
    if (index)
    {
        FILE *f = fopen(index, "wb");
        long n = vix_write(&m, scratch, scratch_len, file_write, f);
        fclose(f);
        if (n < 0)
            return fail("index", (int)n);
    }
    if (build)
    {
        char path[1024];
        long n = vix_build_file(argv[1], build, io.size,
                                (uint32_t)st.st_mtime, scratch, scratch_len);
        if (n < 0)
            return fail("build", (int)n);
        vix_path(build, argv[1], io.size, (uint32_t)st.st_mtime, path,
                 sizeof(path));
        printf("built %s, %ld bytes\n", path, n);
    }

    if (annexb)
    {
        FILE *f = fopen(annexb, "wb");
        uint8_t *buf = NULL;
        size_t cap = 0;
        if ((rc = mp4_iter_init(&it, &m, scratch, scratch_len)) < 0)
            return fail("iter", rc);
        for (i = 0; (rc = mp4_iter_next(&it, &s)) > 0; i++)
        {
            long n;
            if (i < first)
                continue;
            if (s.size + 1024 > cap)
            {
                cap = s.size * 2 + 1024;
                buf = realloc(buf, cap);
            }
            if (i == first)
            {
                n = mp4_params(&m, buf, cap);
                if (n < 0)
                    return fail("params", (int)n);
                fwrite(buf, 1, (size_t)n, f);
            }
            if (read_at(&fd, s.offset, buf, s.size) != (long)s.size)
                return fail("read", MP4_E_IO);
            /* in place for 4-byte lengths, as the PP will do it while
               copying to the FIFO */
            {
                uint8_t *out = m.nal_length == 4 ? buf : buf + s.size;
                n = mp4_annexb(buf, s.size, m.nal_length, out,
                               cap - (size_t)(out - buf));
                if (n < 0)
                    return fail("annexb", (int)n);
                fwrite(out, 1, (size_t)n, f);
            }
        }
        fclose(f);
        free(buf);
        if (rc < 0)
            return fail("samples", rc);
    }
    free(scratch);
    close(fd);
    return 0;
}
