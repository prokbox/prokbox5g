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
/* Host harness for the art builder's source reads: links the core JPEG
 * decoder (apps/recorder/jpeg_load.c) and scaler (resize.c, bmp.c), built
 * with the shell simulator's flags, against a file layer that counts
 * read() calls and bytes and models the storage commands Rockbox's
 * file.c would issue for them [model]:
 *  - a read from mid-sector: the head comes from the file's one-sector
 *    cache, one command if that sector is not the cached one;
 *  - whole sectors go straight to the caller, one command per run of at
 *    most 256 KB (fat.c's limit; a contiguous file assumed);
 *  - a partial tail: one command into the cache (unless cached).
 * Steps per cover, as decode_pinned() does for a thumbnail: the size probe
 * (FORMAT_RETURN_SIZE), then the fill-size decode. Writes the decoded
 * pixels to 'out' for a byte comparison between builds.
 *
 * usage: artread FILE OFFSET LENGTH BOX OUT [FLAGS] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

/* the decoder's prototypes, without the firmware headers */
struct bitmap {
    int width, height;
    int format;
    unsigned char *maskdata;
    int alpha_offset;
    unsigned char *data;
};
struct custom_format;
int clip_jpeg_fd(int fd, int flags, unsigned long len, struct bitmap *bm,
                 int maxsize, int format, const struct custom_format *cf);
extern const size_t JPEG_DECODE_OVERHEAD;
/* firmware/export/lcd.h, bmp.h */
#define FORMAT_NATIVE       1
#define FORMAT_DITHER       0x20000000
#define FORMAT_RESIZE       0x08000000
#define FORMAT_RETURN_SIZE  0x02000000

#define SECTOR 512
#define RUN_MAX (256 * 1024 / SECTOR)

static long n_reads, n_bytes, n_cmds, n_seeks;
static long cached = -1;            /* the file's cached sector */

int sim_open(const char *name, int o, ...) { return open(name, o); }
int sim_close(int fd) { return close(fd); }
off_t sim_lseek(int fd, off_t off, int whence)
{
    if (!(whence == SEEK_CUR && off == 0))
        n_seeks++;
    return lseek(fd, off, whence);
}
off_t sim_ffilesize(int fd)
{
    struct stat st;
    return fstat(fd, &st) ? -1 : st.st_size;
}

ssize_t sim_read(int fd, void *buf, size_t n)
{
    off_t pos = lseek(fd, 0, SEEK_CUR);
    ssize_t got = read(fd, buf, n);
    long s, e, head;

    if (got <= 0)
        return got;
    n_reads++;
    n_bytes += got;
    s = pos / SECTOR;
    e = (pos + got) / SECTOR;       /* first sector not wholly read */
    head = pos % SECTOR;
    if (head)
    {
        if (cached != s)
            n_cmds++, cached = s;
        if (head + got <= SECTOR)
            return got;
        s++;
    }
    if (e > s)
        n_cmds += (e - s + RUN_MAX - 1) / RUN_MAX;
    if ((pos + got) % SECTOR && e >= s)
    {
        if (cached != e)
            n_cmds++, cached = e;
    }
    return got;
}

/* the rest of the firmware the decoder links against */
volatile long current_tick;
unsigned int thread_self(void) { return 1; }
void yield(void) { current_tick++; }
void debugf(const char *fmt, ...) { (void)fmt; }
int id3_unsynchronize(char *tag, int len, bool *ff_found)
{
    int i, o = 0;
    for (i = 0; i < len; i++)
    {
        if (*ff_found && tag[i] == 0)
            *ff_found = false;
        else
        {
            *ff_found = (unsigned char)tag[i] == 0xff;
            tag[o++] = tag[i];
        }
    }
    return o;
}
int base64_decode(const char *in, int len, unsigned char *out)
{ (void)in; (void)len; (void)out; return 0; }
int get_ogg_format_and_move_to_comments(int fd, unsigned char *buf)
{ (void)fd; (void)buf; return -1; }
void ogg_file_init(void *ogg, int fd, int type, int remaining)
{ (void)ogg; (void)fd; (void)type; (void)remaining; }
ssize_t ogg_file_read(void *ogg, void *buf, size_t count)
{ (void)ogg; (void)buf; (void)count; return -1; }
bool ogg_file_open_art(void *ogg, int fd, off_t pos, unsigned char *buf)
{ (void)ogg; (void)fd; (void)pos; (void)buf; return false; }

int main(int argc, char **argv)
{
    long off, len;
    int box, fd, rc, sw, sh, dw, dh, flags;
    size_t maxsize;
    unsigned char *mem;
    struct bitmap bm;
    FILE *out;

    if (argc < 6)
    {
        fprintf(stderr, "usage: %s FILE OFFSET LENGTH BOX OUT [FLAGS]\n",
                argv[0]);
        return 2;
    }
    off = atol(argv[2]);
    len = atol(argv[3]);
    box = atoi(argv[4]);
    flags = argc > 6 ? atoi(argv[6]) : 0;
    /* artcache_reserve()'s decode block for a box this size */
    maxsize = JPEG_DECODE_OVERHEAD + (size_t)4 * box * box * 2
            + 3 * 12 * 4 * box + 4096;
    mem = malloc(maxsize + 16);
    fd = open(argv[1], O_RDONLY);
    if (fd < 0 || !mem)
        return 2;

    memset(&bm, 0, sizeof(bm));
    bm.data = mem;
    lseek(fd, off, SEEK_SET);
    rc = clip_jpeg_fd(fd, flags, len, &bm, (int)maxsize,
                      FORMAT_NATIVE | FORMAT_RETURN_SIZE, NULL);
    sw = bm.width;
    sh = bm.height;
    if (rc <= 0 || sw <= 0 || sh <= 0)
    {
        fprintf(stderr, "probe failed rc=%d\n", rc);
        return 1;
    }
    printf("probe  reads=%ld bytes=%ld cmds=%ld\n", n_reads, n_bytes, n_cmds);
    if ((long)sw * box >= (long)sh * box)
        dh = box, dw = (int)(((long)sw * box + sh - 1) / sh);
    else
        dw = box, dh = (int)(((long)sh * box + sw - 1) / sw);
    if (dw > 4 * box) dw = 4 * box;
    if (dh > 4 * box) dh = 4 * box;
    memset(&bm, 0, sizeof(bm));
    bm.width = dw;
    bm.height = dh;
    bm.data = mem;
    lseek(fd, off, SEEK_SET);
    rc = clip_jpeg_fd(fd, flags, len, &bm, (int)maxsize,
                      FORMAT_NATIVE | FORMAT_DITHER | FORMAT_RESIZE, NULL);
    if (rc <= 0 || bm.width != dw || bm.height != dh)
    {
        fprintf(stderr, "decode failed rc=%d\n", rc);
        return 1;
    }
    printf("total  reads=%ld bytes=%ld cmds=%ld seeks=%ld src=%dx%d "
           "out=%dx%d block=%zu\n", n_reads, n_bytes, n_cmds, n_seeks,
           sw, sh, dw, dh, maxsize);
    out = fopen(argv[5], "wb");
    if (!out || fwrite(bm.data, 2, (size_t)dw * dh, out) != (size_t)dw * dh)
        return 2;
    fclose(out);
    close(fd);
    return 0;
}
