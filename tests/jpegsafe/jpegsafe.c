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

/*
 * B4 (RockWheel review, 2026-11-14): the device's album-art JPEG decoder
 * (apps/recorder/jpeg_load.c, the ARM build: no JPEG_JDEC) on crafted
 * files, run by tests/jpegsafe/run.sh under UBSan.
 *
 *   jpegsafe FILE.jpg        clip_jpeg_fd() on the file, as an art file
 *   jpegsafe -o FILE.ogg     the Ogg Vorbis embedded picture: the metadata
 *                            parser's art record, then clip_jpeg_fd() at
 *                            albumart.pos with its flags (base64), as the
 *                            art builder and buffering.c call it
 *
 * Each call is made twice, as the callers do: a size probe
 * (FORMAT_RETURN_SIZE) into a 4 MB buffer, then the decode into a buffer
 * of exactly the size the probe returned, followed by a 64 KiB guard of
 * 0xA5 bytes: a write past what the decoder may use changes the guard
 * ("GUARD <n> bytes changed", exit 3).
 *
 * Prints "probe rc=<n>" and, when the probe gives a size, "decode rc=<n>
 * <w>x<h>"; exits 0 whenever the decoder returns (a refusal is a clean
 * outcome), 2 on a harness failure.
 */
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

static int decode(int fd, int flags, unsigned long len)
{
    const int probe_size = 4 << 20;
    struct bitmap bm;
    char *mem = malloc(probe_size);
    off_t at = lseek(fd, 0, SEEK_CUR);
    int rc;

    if (!mem)
        return 2;
    memset(&bm, 0, sizeof(bm));
    bm.data = (unsigned char *)mem;
    rc = clip_jpeg_fd(fd, flags, len, &bm, probe_size,
                      FORMAT_NATIVE | FORMAT_RETURN_SIZE, NULL);
    printf("probe rc=%d %dx%d\n", rc, bm.width, bm.height);
    fflush(stdout);
    free(mem);
    if (rc <= 0)
        return 0;
    const int guard = 64 << 10;
    mem = malloc(rc + guard);
    if (!mem)
        return 2;
    memset(mem + rc, 0xA5, guard);
    lseek(fd, at, SEEK_SET);
    memset(&bm, 0, sizeof(bm));
    bm.data = (unsigned char *)mem;
    int need = rc;
    rc = clip_jpeg_fd(fd, flags, len, &bm, need, FORMAT_NATIVE, NULL);
    printf("decode rc=%d %dx%d\n", rc, bm.width, bm.height);
    int changed = 0;
    for (int i = 0; i < guard; i++)
        changed += (unsigned char)mem[need + i] != 0xA5;
    free(mem);
    if (changed)
    {
        printf("GUARD %d bytes changed past the %d-byte buffer\n", changed, need);
        return 3;
    }
    return 0;
}

int main(int argc, char **argv)
{
    static struct mp3entry id3;
    int fd;

    if (argc == 3 && !strcmp(argv[1], "-o"))
    {
        fd = open(argv[2], O_RDONLY);
        if (fd < 0 || !get_metadata(&id3, fd, argv[2])
            || !id3.has_embedded_albumart
            || !(id3.albumart.type & AA_FLAG_VORBIS_BASE64))
        {
            fprintf(stderr, "no base64 art record in %s\n", argv[2]);
            return 2;
        }
        lseek(fd, id3.albumart.pos, SEEK_SET);
        return decode(fd, id3.albumart.type & ~AA_CLEAR_FLAGS_MASK,
                      id3.albumart.size);
    }
    if (argc != 2)
    {
        fprintf(stderr, "usage: %s FILE.jpg | -o FILE.ogg\n", argv[0]);
        return 2;
    }
    fd = open(argv[1], O_RDONLY);
    if (fd < 0)
        return 2;
    return decode(fd, 0, 0);
}
