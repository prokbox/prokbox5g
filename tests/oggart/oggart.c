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

/* Host harness for Ogg embedded art (#120): reads a file's metadata with
 * the real parser (lib/rbcodec/metadata, linked from build/warble's
 * librbcodec.a), prints the album art record, then streams the picture
 * back the way the firmware's art readers do (ogg_file_open_art() at
 * albumart.pos, ogg_file_read() through the page layer, base64_decode() per
 * chunk, up to albumart.size characters or the comment packet's end) and
 * writes the decoded bytes to OUT. albumart.size is the file span to the
 * value's end (page headers included), so OUT may run on past the image
 * into the next comments; the test compares the image's length.
 *
 * usage: oggart FILE OUT [CHUNK]   (CHUNK: base64 characters per read,
 *                                   a multiple of 4; default 4096)
 *        oggart -j FILE OUT [JPEG] decodes the art with the firmware's JPEG
 *                                   decoder (apps/recorder/jpeg_load.c,
 *                                   clip_jpeg_fd() at albumart.pos with the
 *                                   art's flags, as buffering.c and the art
 *                                   builder call it) to native-format pixels
 *                                   at full size; with JPEG, decodes that
 *                                   plain file instead (the reference). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "metadata.h"
#include "metadata_common.h"
#include "jpeg_load.h"

/* the rest of the firmware the metadata parser links against */
void debugf(const char *fmt, ...) { (void)fmt; }
off_t ffilesize(int fd)
{
    struct stat st;
    return fstat(fd, &st) ? -1 : st.st_size;
}

void yield(void) { }

/* Full-size decode of the JPEG at the file's current offset */
static int jpeg_pixels(int fd, int flags, unsigned long len, const char *out)
{
    const int maxsize = 32 << 20;
    struct bitmap bm;
    char *mem = malloc(maxsize);
    off_t at = lseek(fd, 0, SEEK_CUR);
    FILE *f;
    int rc;

    if (!mem)
        return 2;
    memset(&bm, 0, sizeof(bm));
    bm.data = (unsigned char *)mem;
    rc = clip_jpeg_fd(fd, flags, len, &bm, maxsize,
                      FORMAT_NATIVE | FORMAT_RETURN_SIZE, NULL);
    if (rc <= 0 || bm.width <= 0 || bm.height <= 0)
    {
        fprintf(stderr, "jpeg probe failed rc=%d\n", rc);
        return 1;
    }
    lseek(fd, at, SEEK_SET);
    bm.data = (unsigned char *)mem;
    rc = clip_jpeg_fd(fd, flags, len, &bm, maxsize, FORMAT_NATIVE, NULL);
    if (rc <= 0)
    {
        fprintf(stderr, "jpeg decode failed rc=%d\n", rc);
        return 1;
    }
    printf("jpeg: %dx%d\n", bm.width, bm.height);
    f = fopen(out, "wb");
    if (!f || fwrite(bm.data, sizeof(fb_data), (size_t)bm.width * bm.height, f)
              != (size_t)bm.width * bm.height)
        return 2;
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    static struct mp3entry id3;
    struct ogg_file ogg;
    unsigned char probe[92];
    unsigned char *buf;
    long left, total = 0;
    int fd, chunk;
    FILE *out;

    if (argc >= 4 && !strcmp(argv[1], "-j"))
    {
        if (argc > 4)
        {
            fd = open(argv[4], O_RDONLY);
            return fd < 0 ? 2 : jpeg_pixels(fd, 0, 0, argv[3]);
        }
        fd = open(argv[2], O_RDONLY);
        if (fd < 0 || !get_metadata(&id3, fd, argv[2])
            || !id3.has_embedded_albumart)
            return 1;
        lseek(fd, id3.albumart.pos, SEEK_SET);
        return jpeg_pixels(fd, id3.albumart.type & ~AA_CLEAR_FLAGS_MASK,
                           id3.albumart.size, argv[3]);
    }
    if (argc < 3)
    {
        fprintf(stderr, "usage: %s FILE OUT [CHUNK]\n", argv[0]);
        return 2;
    }
    chunk = argc > 3 ? atoi(argv[3]) : 4096;
    if (chunk <= 0 || chunk % 4)
        return 2;
    fd = open(argv[1], O_RDONLY);
    buf = malloc(chunk);
    if (fd < 0 || !buf)
        return 2;
    if (!get_metadata(&id3, fd, argv[1]))
    {
        fprintf(stderr, "no metadata\n");
        return 1;
    }
    printf("art: embedded=%d type=%d base64=%d pos=%ld size=%d\n",
           (int)id3.has_embedded_albumart,
           (int)(id3.albumart.type & AA_CLEAR_FLAGS_MASK),
           (id3.albumart.type & AA_FLAG_VORBIS_BASE64) != 0,
           (long)id3.albumart.pos, id3.albumart.size);
    if (!id3.has_embedded_albumart
        || !(id3.albumart.type & AA_FLAG_VORBIS_BASE64))
        return 1;

    if (!ogg_file_open_art(&ogg, fd, id3.albumart.pos, probe))
    {
        fprintf(stderr, "open failed\n");
        return 1;
    }
    out = fopen(argv[2], "wb");
    if (!out)
        return 2;
    for (left = id3.albumart.size; left > 0; )
    {
        ssize_t n = ogg_file_read(&ogg, buf, left < chunk ? left : chunk);
        size_t dec;

        if (n <= 0)
            break;  /* the comment packet's end (-1 past it, as jpeg_load) */
        left -= n;
        dec = base64_decode((char *)buf, n, buf);
        fwrite(buf, 1, dec, out);
        total += dec;
    }
    fclose(out);
    close(fd);
    printf("decoded: %ld bytes\n", total);
    return 0;
}
