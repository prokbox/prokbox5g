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
 * Host test: the MP4 parser's video fields (Phase 14a V2,
 * docs/phase14a-brief.md 3.2, 4.1, A3).
 *
 * Built by tests/video/run.sh with warble's configuration plus
 * -DHAVE_BCMFW_VIDEO, against the real lib/rbcodec/metadata/{metadata,mp4,
 * metadata_common,...}.c. Usage:
 *
 *   test_mp4meta FILE CHECK...      parse FILE with get_metadata(), check
 *   test_mp4meta --copy FILE        copy_mp3entry() relocates tv_show
 *
 * CHECK is name=value with name one of video, podcast, stik, tvsh, tvsn,
 * tves, audiobook, chapters, lead_trim, samples, or length=MS~TOL (|length -
 * MS| <= TOL). tvsh=- means no tvsh. Exits non-zero on any mismatch.
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
#include <stdarg.h>
#include <sys/stat.h>
#include "platform.h"
#include "metadata.h"
#if defined(HAVE_IPOD_UI) && !defined(HAVE_BCMFW_VIDEO)
#include "nfc.h"
/* warble's firmware objects omit shell normalization; link the real compact
 * composer for this plain-shell metadata variant, not a no-op substitute. */
void utf8_normalize(char *s) { nfc_normalize(s); }
#endif

#if !defined(HAVE_BCMFW_VIDEO) && !defined(HAVE_IPOD_UI)
#error "test_mp4meta needs a shell classification build"
#endif

/* what warble.c provides to the metadata code */
void debugf(const char *fmt, ...) { (void)fmt; }
void panicf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(2);
}
off_t ffilesize(int fd)
{
    struct stat st;
    return fstat(fd, &st) ? -1 : st.st_size;
}

static int fails;

static void check_int(const char *file, const char *name, long got,
                      long want)
{
    if (got == want) {
        printf("ok   %s %s=%ld\n", file, name, want);
    } else {
        printf("FAIL %s %s: got %ld, want %ld\n", file, name, got, want);
        fails++;
    }
}

static int copy_test(const char *file)
{
#ifdef HAVE_BCMFW_VIDEO
    struct mp3entry *a = calloc(1, sizeof(*a));
    struct mp3entry *b = calloc(1, sizeof(*b));
    const char *lo, *hi;

    if (!a || !b || !get_metadata(a, -1, file) || !a->tv_show) {
        printf("FAIL %s copy: no tv_show to copy\n", file);
        return 1;
    }
    copy_mp3entry(b, a);
    lo = (const char *)b;
    hi = lo + sizeof(*b);
    if (b->tv_show < lo || b->tv_show >= hi || strcmp(b->tv_show, a->tv_show)
        || (b->title && (b->title < lo || b->title >= hi))) {
        printf("FAIL %s copy: tv_show not relocated into the copy\n", file);
        return 1;
    }
    memset(a, 0x55, sizeof(*a));    /* the copy must not lean on the original */
    if (strcmp(b->tv_show, "Show A")) {
        printf("FAIL %s copy: tv_show '%s' after the original was wiped\n",
               file, b->tv_show);
        return 1;
    }
    printf("ok   %s copy_mp3entry tv_show=\"%s\"\n", file, b->tv_show);
    free(a);
    free(b);
    return 0;
#else
    (void)file;
    return 1;
#endif
}

int main(int argc, char **argv)
{
    static struct mp3entry id3;
    const char *file;
    int i;

    if (argc == 3 && !strcmp(argv[1], "--copy"))
        return copy_test(argv[2]);
    if (argc < 3) {
        fprintf(stderr, "usage: %s FILE CHECK... | --copy FILE\n", argv[0]);
        return 2;
    }
    file = argv[1];
    if (!get_metadata(&id3, -1, file)) {
        printf("FAIL %s: get_metadata failed\n", file);
        return 1;
    }
    for (i = 2; i < argc; i++) {
        char name[32];
        const char *eq = strchr(argv[i], '=');
        const char *v;
        size_t n;

        if (!eq || (n = eq - argv[i]) >= sizeof(name)) {
            fprintf(stderr, "bad check '%s'\n", argv[i]);
            return 2;
        }
        memcpy(name, argv[i], n);
        name[n] = '\0';
        v = eq + 1;
        if (!strcmp(name, "video"))
            check_int(file, name, id3.is_video, atol(v));
#ifdef HAVE_BCMFW_VIDEO
        else if (!strcmp(name, "playable"))
            check_int(file, name, id3.video_avc, atol(v));
        else if (!strcmp(name, "podcast"))
            check_int(file, name, id3.is_podcast, atol(v));
        else if (!strcmp(name, "stik"))
            check_int(file, name, id3.stik, atol(v));
        else if (!strcmp(name, "tvsn"))
            check_int(file, name, id3.tv_season, atol(v));
        else if (!strcmp(name, "tves"))
            check_int(file, name, id3.tv_episode, atol(v));
#endif
        else if (!strcmp(name, "audiobook"))
            check_int(file, name, id3.is_audiobook, atol(v));
        else if (!strcmp(name, "chapters"))
            check_int(file, name, id3.chapter_count, atol(v));
        else if (!strcmp(name, "lead_trim"))
            check_int(file, name, id3.lead_trim, atol(v));
        else if (!strcmp(name, "samples"))
            check_int(file, name, (long)id3.samples, atol(v));
#ifdef HAVE_BCMFW_VIDEO
        else if (!strcmp(name, "tvsh")) {
            const char *got = id3.tv_show ? id3.tv_show : "-";
            if (strcmp(got, v)) {
                printf("FAIL %s tvsh: got '%s', want '%s'\n", file, got, v);
                fails++;
            } else {
                printf("ok   %s tvsh=%s\n", file, v);
            }
        }
#endif
        else if (!strcmp(name, "length")) {
            long want = atol(v);
            const char *t = strchr(v, '~');
            long tol = t ? atol(t + 1) : 0;
            long d = (long)id3.length - want;
            if (d < -tol || d > tol) {
                printf("FAIL %s length: got %lu ms, want %ld +- %ld\n", file,
                       id3.length, want, tol);
                fails++;
            } else {
                printf("ok   %s length=%lu ms (ffprobe %ld +- %ld)\n", file,
                       id3.length, want, tol);
            }
        } else {
            fprintf(stderr, "unknown check '%s'\n", name);
            return 2;
        }
    }
    return fails ? 1 : 0;
}
