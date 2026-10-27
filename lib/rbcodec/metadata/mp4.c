/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2005 Magnus Holmgren
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
#include <stdlib.h>
#include <ctype.h>
#include <inttypes.h>
#include "platform.h"

#include "errno.h"
#include "metadata.h"
#include "metadata_common.h"
#include "metadata_parsers.h"
#include "logf.h"
#include "debug.h"
#include "replaygain.h"
#include "chapters.h"

#ifdef DEBUGF
#undef DEBUGF
#define DEBUGF(...)
#endif

#define MP4_3gp6 FOURCC('3', 'g', 'p', '6')
#define MP4_aART FOURCC('a', 'A', 'R', 'T')
#define MP4_alac FOURCC('a', 'l', 'a', 'c')
#define MP4_calb FOURCC(0xa9, 'a', 'l', 'b')
#define MP4_cART FOURCC(0xa9, 'A', 'R', 'T')
#define MP4_cgrp FOURCC(0xa9, 'g', 'r', 'p')
#define MP4_cgen FOURCC(0xa9, 'g', 'e', 'n')
#define MP4_chpl FOURCC('c', 'h', 'p', 'l')
#define MP4_cnam FOURCC(0xa9, 'n', 'a', 'm')
#define MP4_cwrt FOURCC(0xa9, 'w', 'r', 't')
#define MP4_ccmt FOURCC(0xa9, 'c', 'm', 't')
#define MP4_cday FOURCC(0xa9, 'd', 'a', 'y')
#define MP4_covr FOURCC('c', 'o', 'v', 'r')
#define MP4_disk FOURCC('d', 'i', 's', 'k')
#define MP4_esds FOURCC('e', 's', 'd', 's')
#define MP4_ftyp FOURCC('f', 't', 'y', 'p')
#define MP4_gnre FOURCC('g', 'n', 'r', 'e')
#define MP4_hdlr FOURCC('h', 'd', 'l', 'r')
#define MP4_ilst FOURCC('i', 'l', 's', 't')
#define MP4_mdat FOURCC('m', 'd', 'a', 't')
#define MP4_mdia FOURCC('m', 'd', 'i', 'a')
#define MP4_mdir FOURCC('m', 'd', 'i', 'r')
#define MP4_meta FOURCC('m', 'e', 't', 'a')
#define MP4_minf FOURCC('m', 'i', 'n', 'f')
#define MP4_moov FOURCC('m', 'o', 'o', 'v')
#define MP4_mp4a FOURCC('m', 'p', '4', 'a')
#define MP4_soun FOURCC('s', 'o', 'u', 'n')
#define MP4_stbl FOURCC('s', 't', 'b', 'l')
#define MP4_stsd FOURCC('s', 't', 's', 'd')
#define MP4_stts FOURCC('s', 't', 't', 's')
#define MP4_trak FOURCC('t', 'r', 'a', 'k')
#define MP4_trkn FOURCC('t', 'r', 'k', 'n')
#define MP4_udta FOURCC('u', 'd', 't', 'a')
#define MP4_extra FOURCC('-', '-', '-', '-')
#define MP4_chap FOURCC('c', 'h', 'a', 'p')
#define MP4_co64 FOURCC('c', 'o', '6', '4')
#define MP4_mdhd FOURCC('m', 'd', 'h', 'd')
#define MP4_stco FOURCC('s', 't', 'c', 'o')
#define MP4_stsc FOURCC('s', 't', 's', 'c')
#define MP4_stsz FOURCC('s', 't', 's', 'z')
#define MP4_text FOURCC('t', 'e', 'x', 't')
#define MP4_tkhd FOURCC('t', 'k', 'h', 'd')
#define MP4_tref FOURCC('t', 'r', 'e', 'f')

#ifdef HAVE_METADATA_EXTRAS
#include "lyrics.h"
#define MP4_clyr FOURCC(0xa9, 'l', 'y', 'r')
#define MP4_MAX_CHAP_REFS 4
#endif
#if defined(HAVE_METADATA_EXTRAS) || defined(HAVE_IPOD_UI) || defined(HAVE_BCMFW_VIDEO)
#define MP4_vide FOURCC('v', 'i', 'd', 'e')
#endif
#ifdef HAVE_BCMFW_VIDEO
/* iPod UI Videos (docs/phase14a-brief.md 3.2): iTunes video tags and the
 * sample entries of a picture the player decodes */
#define MP4_avc1 FOURCC('a', 'v', 'c', '1')
#define MP4_avc3 FOURCC('a', 'v', 'c', '3')
#define MP4_pcst FOURCC('p', 'c', 's', 't')
#define MP4_stik FOURCC('s', 't', 'i', 'k')
#define MP4_tves FOURCC('t', 'v', 'e', 's')
#define MP4_tvsh FOURCC('t', 'v', 's', 'h')
#define MP4_tvsn FOURCC('t', 'v', 's', 'n')
#ifndef MP4_vide
#define MP4_vide FOURCC('v', 'i', 'd', 'e')
#endif
#endif

/* Read the tag data from an MP4 file, storing up to buffer_size bytes in
 * buffer.
 */
static unsigned long read_mp4_tag(int fd, unsigned int size_left, char* buffer,
                                  unsigned int buffer_left)
{
    unsigned long bytes_read = 0;
    ssize_t rd_ret = 0;
    ssize_t bytes_req;
    #define MP4_TAG_HEADER_SIZE 16


    if (size_left >= MP4_TAG_HEADER_SIZE)
    {
        /* Skip the data tag header - maybe we should parse it properly? */
        lseek(fd, MP4_TAG_HEADER_SIZE, SEEK_CUR);
        size_left -= MP4_TAG_HEADER_SIZE;

        if (size_left > buffer_left)
            bytes_req = buffer_left;
        else
            bytes_req = size_left;

        rd_ret = read(fd, buffer, bytes_req);
        if (rd_ret == bytes_req)
            bytes_read = bytes_req;
        else
        {
            /* read less than expected or an error from read() */
            logf("Error %d, read_mp4_tag", rd_ret);
            if (rd_ret < 0)
                rd_ret = 0; /* Skip everything */
        }
    }
    if (size_left >  (unsigned int) rd_ret)
        lseek(fd, size_left - rd_ret, SEEK_CUR);


    return bytes_read;
}

/* Read a string tag from an MP4 file */
static unsigned int read_mp4_tag_string(int fd, int size_left, char** buffer,
                                        unsigned int* buffer_left, char** dest)
{
    unsigned int bytes_read = read_mp4_tag(fd, size_left, *buffer,
        *buffer_left > 0 ? *buffer_left - 1 : 0);
    unsigned int length = 0;

    if (bytes_read)
    {
        /* Do not overwrite already available metadata. Especially when reading
         * tags with e.g. multiple genres / artists. This way only the first
         * of multiple entries is used, all following are dropped. */
        if (*dest == NULL)
        {
            (*buffer)[bytes_read] = 0; /* zero-terminate for correct strlen().*/
            length = strlen(*buffer) + 1;
            length = MIN(length, ID3V2_MAX_ITEM_SIZE); /* Limit item size. */

            *dest = *buffer;
            (*buffer)[length-1] = 0; /* zero-terminate buffer. */
            *buffer_left -= length;
            *buffer += length;
        }
    }
    else
    {
        *dest = NULL;
    }

    return length;
}

static unsigned int read_mp4_atom(int fd, uint32_t* size,
                                  uint32_t* type, uint32_t size_left)
{
    read_uint32be(fd, size);
    read_uint32be(fd, type);

    if (*size == 1)
    {
        /* FAT32 doesn't support files this big, so something seems to
         * be wrong. (64-bit sizes should only be used when required.)
         */
        errno = EFBIG;
        *type = 0;
        return 0;
    }

    if (*size > 0)
    {
        if (*size > size_left)
        {
            size_left = 0;
        }
        else
        {
            size_left -= *size;
        }

        *size -= 8;
    }
    else
    {
        *size = size_left;
        size_left = 0;
    }

    return size_left;
}

static unsigned int read_mp4_length(int fd, uint32_t* size)
{
    unsigned int length = 0;
    int bytes = 0;
    unsigned char c = '\0';

    do
    {
        read(fd, &c, 1);
        bytes++;
        (*size)--;
        length = (length << 7) | (c & 0x7F);
    }
    while ((c & 0x80) && (bytes < 4) && (*size > 0));

    return length;
}

static bool read_mp4_esds(int fd, struct mp3entry* id3, uint32_t* size)
{
    unsigned char buf[8] = {0};
    bool sbr = false;
    bool sbr_signaled = false;

    lseek(fd, 4, SEEK_CUR);     /* Version and flags. */
    read(fd, buf, 1);           /* Verify ES_DescrTag. */
    *size -= 5;

    if (*buf == 3)
    {
        /* read length */
        if (read_mp4_length(fd, size) < 20)
        {
            return sbr;
        }

        lseek(fd, 3, SEEK_CUR);
        *size -= 3;
    }
    else
    {
        lseek(fd, 2, SEEK_CUR);
        *size -= 2;
    }

    read(fd, buf, 1);           /* Verify DecoderConfigDescrTab. */
    *size -= 1;

    if (*buf != 4)
    {
        return sbr;
    }

    if (read_mp4_length(fd, size) < 13)
    {
        return sbr;
    }

    lseek(fd, 13, SEEK_CUR);    /* Skip audio type, bit rates, etc. */
    read(fd, buf, 1);
    *size -= 14;

    if (*buf != 5)              /* Verify DecSpecificInfoTag. */
    {
        return sbr;
    }

    {
        static const int sample_rates[] =
        {
            96000, 88200, 64000, 48000, 44100, 32000,
            24000, 22050, 16000, 12000, 11025, 8000
        };
        unsigned long bits;
        unsigned int length;
        unsigned int index;
        unsigned int type;

        /* Read the (leading part of the) decoder config. */
        length = read_mp4_length(fd, size);
        length = MIN(length, *size);
        length = MIN(length, sizeof(buf));
        memset(buf, 0, sizeof(buf));
        read(fd, buf, length);
        *size -= length;

        /* Maybe time to write a simple read_bits function... */

        /* Decoder config format:
         * Object type           - 5 bits
         * Frequency index       - 4 bits
         * Channel configuration - 4 bits
         * Also see libfaad/mp4.c AudioSpecificConfig2 (consider using it instead of manual parsing)
         */
        bits = get_long_be(buf);
        type = bits >> 27;              /* Object type - 5 bits */
        index = (bits >> 23) & 0xf;     /* Frequency index - 4 bits */

        if (index < (sizeof(sample_rates) / sizeof(*sample_rates)))
        {
            id3->frequency = sample_rates[index];
        }

        if (type == 5)
        {
            unsigned int old_index = index;

            sbr = true;
            index = (bits >> 15) & 0xf; /* Frequency index - 4 bits */

            if (index == 15)
            {
                /* 17 bits read so far... */
                bits = get_long_be(&buf[2]);
                id3->frequency = (bits >> 7) & 0x00ffffff;
            }
            else if (index < (sizeof(sample_rates) / sizeof(*sample_rates)))
            {
                id3->frequency = sample_rates[index];
            }

            if (old_index == index)
            {
                /* Downsampled SBR */
                id3->frequency *= 2;
            }
        }
        /* Skip 13 bits from above, plus 3 bits, then read 11 bits */
        else if ((length >= 4) && (((bits >> 5) & 0x7ff) == 0x2b7))
        {
            /* We found an extensionAudioObjectType */
            type = bits & 0x1f;         /* Object type - 5 bits*/
            bits = get_long_be(&buf[4]);

            if (type == 5)
            {
                sbr = bits >> 31;
                sbr_signaled = true;

                if (sbr)
                {
                    unsigned int old_index = index;

                    /* 1 bit read so far */
                    index = (bits >> 27) & 0xf; /* Frequency index - 4 bits */

                    if (index == 15)
                    {
                        /* 5 bits read so far */
                        id3->frequency = (bits >> 3) & 0x00ffffff;
                    }
                    else if (index < (sizeof(sample_rates) / sizeof(*sample_rates)))
                    {
                        id3->frequency = sample_rates[index];
                    }

                    if (old_index == index)
                    {
                        /* Downsampled SBR */
                        id3->frequency *= 2;
                    }
                }
            }
        }
#ifndef CODEC_AAC_SBR_DEC
        //SBR_DEC is disabled so disable sbr implicit signalling
        sbr_signaled = true;
#endif
        if (!sbr && !sbr_signaled && id3->frequency <= 24000)
        {
            /* As stated in libfaad/mp4.c AudioSpecificConfig2:
             * no SBR signalled, this could mean either implicit signalling or no SBR in this file
             * MPEG specification states: assume SBR on files with samplerate <= 24000 Hz
             */
            id3->frequency *= 2;
            sbr = true;
        }
    }

    return sbr;
}

static void read_mp4_tag_i_from_n(int fd, int *i, char** i_from_n_string, uint32_t size, unsigned int *buffer_left, char **buffer)
{
    uint16_t x[3];
    *i = 0;
    *i_from_n_string = NULL;
    if (read_mp4_tag(fd, size, (char*) &x, sizeof(x))  == sizeof(x))
    {
        *i = betoh16(x[1]);
        int n = betoh16(x[2]);
        if (n > 0)
        {
            unsigned int string_length = snprintf(*buffer, *buffer_left, "%d/%d", *i, n) + 1;
            if (string_length <= *buffer_left)
            {
                *i_from_n_string = *buffer;
                *buffer += string_length;
                *buffer_left -= string_length;
            }
        }
    }
}

static bool read_mp4_tags(int fd, struct mp3entry* id3,
                          uint32_t size_left)
{
    uint32_t size;
    uint32_t type;
    unsigned int buffer_left = sizeof(id3->id3v2buf) + sizeof(id3->id3v1buf);
    char* buffer = id3->id3v2buf;
    bool cwrt = false;

    do
    {
        size_left = read_mp4_atom(fd, &size, &type, size_left);

        /* DEBUGF("Tag atom: '%c%c%c%c' (%d bytes left)\n", type >> 24 & 0xff,
            type >> 16 & 0xff, type >> 8 & 0xff, type & 0xff, size); */

        switch (type)
        {
        case MP4_cnam:
            read_mp4_tag_string(fd, size, &buffer, &buffer_left,
                &id3->title);
            break;

        case MP4_cART:
            read_mp4_tag_string(fd, size, &buffer, &buffer_left,
                &id3->artist);
            break;

        case MP4_aART:
            read_mp4_tag_string(fd, size, &buffer, &buffer_left,
                &id3->albumartist);
            break;

        case MP4_cgrp:
            read_mp4_tag_string(fd, size, &buffer, &buffer_left,
                &id3->grouping);
            break;

        case MP4_calb:
            read_mp4_tag_string(fd, size, &buffer, &buffer_left,
                &id3->album);
            break;

        case MP4_cwrt:
            read_mp4_tag_string(fd, size, &buffer, &buffer_left,
                &id3->composer);
            cwrt = false;
            break;

        case MP4_ccmt:
            read_mp4_tag_string(fd, size, &buffer, &buffer_left,
                &id3->comment);
            break;

        case MP4_cday:
            read_mp4_tag_string(fd, size, &buffer, &buffer_left,
                                &id3->year_string);

            /* Try to parse it as a year, for the benefit of the database.
             */
            if(id3->year_string)
            {
                id3->year = atoi(id3->year_string);
                if (id3->year < 1900)
                {
                    id3->year = 0;
                }
            }
            else
                id3->year = 0;

            break;

        case MP4_gnre:
            {
                unsigned short genre;
                const unsigned int g_size = sizeof(genre);
                id3->genre_string = NULL;
                if (read_mp4_tag(fd, size, (char*) &genre, g_size) == g_size)
                    id3->genre_string = id3_get_num_genre(betoh16(genre) - 1);
            }
            break;

        case MP4_cgen:
            read_mp4_tag_string(fd, size, &buffer, &buffer_left,
                &id3->genre_string);
            break;

        case MP4_disk:
            read_mp4_tag_i_from_n(fd, &id3->discnum, &id3->disc_string, size, &buffer_left, &buffer);
            break;

        case MP4_trkn: {
            read_mp4_tag_i_from_n(fd, &id3->tracknum, &id3->track_string, size, &buffer_left, &buffer);
            break;
        }
#ifdef HAVE_ALBUMART
        case MP4_covr:
            {
                int pos = lseek(fd, 0, SEEK_CUR) + 16;
                id3->albumart.type = AA_TYPE_UNKNOWN;

                if (read_mp4_tag(fd, size, buffer, 8) >= 4)
                {

                    if (memcmp(buffer, "\xff\xd8\xff\xe0", 4) == 0)
                    {
                        id3->albumart.type = AA_TYPE_JPG;
                    }
                    else if (memcmp(buffer, "\x89\x50\x4e\x47\x0d\x0a\x1a\x0a", 8) == 0)
                    {
                        id3->albumart.type = AA_TYPE_PNG;
                    }

                    if (id3->albumart.type != AA_TYPE_UNKNOWN)
                    {
                        id3->albumart.pos  = pos;
                        id3->albumart.size = size - 16;
                        id3->has_embedded_albumart = true;
                    }
                }
            }
            break;
#endif

#ifdef HAVE_BCMFW_VIDEO
        case MP4_tvsh:
            read_mp4_tag_string(fd, size, &buffer, &buffer_left,
                &id3->tv_show);
            break;

        case MP4_stik:      /* 1-byte ints ... */
        case MP4_pcst:
        case MP4_tvsn:      /* ... and 4-byte ones */
        case MP4_tves:
            {
                uint8_t b[4];
                unsigned int i, n = read_mp4_tag(fd, size, (char*)b, sizeof(b));
                uint32_t v = 0;

                for (i = 0; i < n; i++)
                    v = v << 8 | b[i];
                if (type == MP4_stik)
                    id3->stik = v <= 0xff ? v : 0;
                else if (type == MP4_pcst)
                    id3->is_podcast = v != 0;
                else if (type == MP4_tvsn)
                    id3->tv_season = MIN(v, 0xffu);
                else
                    id3->tv_episode = MIN(v, 0xffffu);
            }
            break;
#endif

#ifdef HAVE_METADATA_EXTRAS
        case MP4_clyr:
            {
                /* only note where the text is (lyrics.h) */
                uint8_t h[16];
                off_t pos = lseek(fd, 0, SEEK_CUR);
                if (size > 16 && id3->lyrics.size == 0
                    && read(fd, h, 16) == 16 && !memcmp(h + 4, "data", 4))
                {
                    uint32_t dsize = (uint32_t)h[0] << 24 | h[1] << 16
                                     | h[2] << 8 | h[3];
                    uint8_t dtype = h[11];  /* 1: UTF-8, 2: UTF-16BE */
                    if ((dtype == 1 || dtype == 2) && dsize > 16
                        && dsize <= size && dsize - 16 < (1u << 24))
                    {
                        id3->lyrics.pos = pos + 16;
                        id3->lyrics.size = dsize - 16;
                        id3->lyrics.format = dtype == 1 ? LYRICS_MP4_UTF8
                                                        : LYRICS_MP4_UTF16;
                    }
                }
                lseek(fd, pos + size, SEEK_SET);
            }
            break;
#endif

        case MP4_extra:
            {
                char tag_name[TAG_NAME_LENGTH];
                uint32_t sub_size;
                ssize_t rd_ret;
                /* "mean" atom */
                read_uint32be(fd, &sub_size);
                size -= sub_size;
                lseek(fd, sub_size - 4, SEEK_CUR);
                /* "name" atom */
                read_uint32be(fd, &sub_size);
                size -= sub_size;
                lseek(fd, 8, SEEK_CUR);
                sub_size -= 12;

                if (sub_size > sizeof(tag_name) - 1)
                {
                    rd_ret = read(fd, tag_name, sizeof(tag_name) - 1);
                    lseek(fd, sub_size - (sizeof(tag_name) - 1), SEEK_CUR);
                    sub_size = sizeof(tag_name) - 1;
                }
                else
                {
                    rd_ret = read(fd, tag_name, sub_size);
                }
                if (rd_ret != (ssize_t)sub_size)
                    rd_ret = 0;
                tag_name[rd_ret] = 0;

                static const char *tn_options[] = {"composer", "iTunSMPB",
                                   "musicbrainz track id", "album artist", NULL};

                int tn_op = string_option(tag_name, tn_options, true);


                if (tn_op == 0 && !cwrt) /*composer*/
                {
                    read_mp4_tag_string(fd, size, &buffer, &buffer_left,
                        &id3->composer);
                }
                else if (tn_op == 1) /*iTunSMPB*/
                {
                    char value[TAG_VALUE_LENGTH];
                    char* value_p = value;
                    char* any = NULL;
                    unsigned int length = sizeof(value);

                    read_mp4_tag_string(fd, size, &value_p, &length, &any);
                    id3->lead_trim = get_itunes_int32(value, 1);
                    id3->tail_trim = get_itunes_int32(value, 2);
                    DEBUGF("AAC: lead_trim %d, tail_trim %d\n",
                        id3->lead_trim, id3->tail_trim);
                }
                else if (tn_op == 2) /*musicbrainz track id*/
                {
                    read_mp4_tag_string(fd, size, &buffer, &buffer_left,
                        &id3->mb_track_id);
                }
                else if (tn_op == 3) /*album artist*/
                {
                    read_mp4_tag_string(fd, size, &buffer, &buffer_left,
                        &id3->albumartist);
                }
                else
                {
                    char* any = NULL;
                    unsigned int length = read_mp4_tag_string(fd, size,
                        &buffer, &buffer_left, &any);

                    if (length > 0)
                    {
                        /* Re-use the read buffer as the dest buffer... */
                        buffer -= length;
                        buffer_left += length;

                        parse_replaygain(tag_name, buffer, id3);
                    }
                }
            }
            break;

        default:
            lseek(fd, size, SEEK_CUR);
            break;
        }
    }
    while ((size_left > 0) && (errno == 0));

    return true;
}

#if defined(HAVE_IPOD_UI) || defined(HAVE_BCMFW_VIDEO)
static uint32_t vide_be32(const uint8_t *b)
{
    return (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3];
}

/* Skips an MPEG-4 descriptor length (1-4 bytes, top bit = more) at b[p],
 * never past n. */
static int esds_len_skip(const uint8_t *b, int p, int n)
{
    for (int k = 0; k < 4 && p < n && (b[p++] & 0x80); k++)
        ;
    return p;
}

/* An 'mp4v' sample entry at 'entry' (its first byte) whose decoder is a
 * still-picture one (DecoderConfigDescriptor objectTypeIndication 0x6C
 * JPEG, 0x6D PNG, 0x6E JPEG 2000): the way ffmpeg stores an m4b's chapter
 * images. One small read past the 86-byte visual sample entry; its child
 * boxes (pasp, btrt, fiel... may come first) are walked to the esds, which
 * is parsed with every index bounded by the read: ES_Descriptor (tag 3),
 * its ES_ID, flags and the optional fields they announce, then the
 * DecoderConfigDescriptor (tag 4). Anything else is not a still. 'at' gets
 * the file position. */
static bool mp4v_still(int fd, off_t entry, off_t *at)
{
    uint8_t b[96];
    ssize_t n = lseek(fd, entry + 86, SEEK_SET) < 0 ? 0 : read(fd, b, 96);
    int p = 0, f;
    uint32_t sz;

    *at = entry + 86 + (n > 0 ? n : 0);
    while (p + 8 <= n && memcmp(b + p + 4, "esds", 4))
    {
        sz = vide_be32(b + p);
        if (sz < 8 || sz > (uint32_t)(n - p))
            return false;
        p += sz;
    }
    if (p + 13 >= n || b[p + 12] != 3)
        return false;
    p = esds_len_skip(b, p + 13, n) + 2;    /* ES_ID */
    if (p >= n)
        return false;
    f = b[p++];
    if (f & 0x80)                       /* dependsOn_ES_ID */
        p += 2;
    if ((f & 0x40) && p < n)            /* URL: length byte + string */
        p += 1 + b[p];
    if (f & 0x20)                       /* OCR_ES_ID */
        p += 2;
    if (p >= n || b[p] != 4)
        return false;
    p = esds_len_skip(b, p + 1, n);
    return p < n && b[p] >= 0x6c && b[p] <= 0x6e;
}

/* A 'vide' trak's 'minf' (body of 'size' bytes at the file position): reads
 * only the first 'stsd' sample entry's fourcc, through one small window
 * (one read for the usual vmhd/dinf/stbl/stsd layout, at most four), and
 * sets is_video for moving-picture sample entries (video_avc for avc1/avc3,
 * what the player plays). Never enters the generic walk, so nothing of
 * the video trak reaches samples, length or bitrate (brief 3.2, H4). Stays
 * inside the atom and restores the file position. A chapter-image trak
 * ('jpeg', 'png ', or 'mp4v' with a still-picture decoder) is not a
 * video. */
static void mp4_vide_entry(int fd, struct mp3entry *id3, uint32_t size)
{
    uint8_t w[96];
    off_t start = lseek(fd, 0, SEEK_CUR);
    off_t at = start;                   /* the file position */
    uint32_t wpos = 0, wlen = 0, pos = 0, end = size;
    int reads = 0;
    bool in_stbl = false;

    while (start >= 0 && end - pos >= 8)
    {
        uint32_t need = MIN(24u, end - pos);
        uint32_t asz, type;
        const uint8_t *a;

        if (pos < wpos || pos + need > wpos + wlen)
        {
            ssize_t n;

            if (++reads > 4
                || (at != start + (off_t)pos
                    && lseek(fd, start + (off_t)pos, SEEK_SET) < 0))
                break;
            n = read(fd, w, MIN((uint32_t)sizeof(w), size - pos));
            if (n < (ssize_t)need)
                break;
            at = start + (off_t)pos + n;
            wpos = pos;
            wlen = n;
        }
        a = w + (pos - wpos);
        asz = vide_be32(a);
        type = vide_be32(a + 4);
        if (asz < 8 || asz > end - pos)
            break;
        if (!in_stbl && type == MP4_stbl)
        {
            in_stbl = true;
            end = pos + asz;
            pos += 8;
            continue;
        }
        if (in_stbl && type == MP4_stsd)
        {
            /* size, 'stsd', version/flags, count, entry size, fourcc */
            uint32_t fourcc = vide_be32(a + 20);
            if (asz >= 24 && need == 24 && vide_be32(a + 12) > 0
                && fourcc != FOURCC('j', 'p', 'e', 'g')
                && fourcc != FOURCC('p', 'n', 'g', ' ')
                && !(fourcc == FOURCC('m', 'p', '4', 'v')
                     && mp4v_still(fd, start + (off_t)pos + 16, &at)))
            {
                /* any moving picture leaves Music; only H.264 is listed
                 * in Videos (the player decodes nothing else) */
                id3->is_video = true;
#ifdef HAVE_BCMFW_VIDEO
                id3->video_avc = fourcc == MP4_avc1 || fourcc == MP4_avc3;
#endif
            }
            break;
        }
        pos += asz;
    }
    if (start >= 0 && at != start)
        lseek(fd, start, SEEK_SET);
}
#endif

static bool read_mp4_container(int fd, struct mp3entry* id3,
                               uint32_t size_left)
{
    uint32_t size    = 0;
    uint32_t type    = 0;
    uint32_t handler = 0;
    bool rc = true;
    bool done = false;

    do
    {
        size_left = read_mp4_atom(fd, &size, &type, size_left);

        /* DEBUGF("Atom: '%c%c%c%c' (0x%08lx, %lu bytes left)\n",
            (int) ((type >> 24) & 0xff), (int) ((type >> 16) & 0xff),
            (int) ((type >> 8) & 0xff), (int) (type & 0xff),
            type, size); */

        switch (type)
        {
        case MP4_ftyp:
            {
                // filetype (supported ignore case values: m4a, m4b, mp42, 3gp6, qt, isom)
                char filetype[4];
                read(fd, &filetype, 4);
                DEBUGF("MP4 file type:  '%.4s'\n", filetype);
                /* major brand "M4B " marks an iTunes audiobook */
                if ((filetype[0] == 'M' || filetype[0] == 'm')
                    && filetype[1] == '4'
                    && (filetype[2] == 'B' || filetype[2] == 'b'))
                {
                    id3->is_audiobook = true;
                }
                size -= 4;
            }
            break;

        case MP4_meta:
            lseek(fd, 4, SEEK_CUR);  /* Skip version */
            size -= 4;
            /* Fall through */

        case MP4_moov:
        case MP4_udta:
        case MP4_mdia:
        case MP4_stbl:
        case MP4_trak:
            rc = read_mp4_container(fd, id3, size);
            size = 0;
            break;

        case MP4_ilst:
            /* We need at least a size of 8 to read the next atom. */
            if (handler == MP4_mdir && size>8)
            {
                rc = read_mp4_tags(fd, id3, size);
                size = 0;
            }
            break;

        case MP4_minf:
            if (handler == MP4_soun)
            {
                rc = read_mp4_container(fd, id3, size);
                size = 0;
            }
#if defined(HAVE_IPOD_UI) || defined(HAVE_BCMFW_VIDEO)
            else if (handler == MP4_vide && !id3->is_video)
                mp4_vide_entry(fd, id3, size);
#endif
            break;

        case MP4_stsd:
            lseek(fd, 8, SEEK_CUR);
            size -= 8;
            rc = read_mp4_container(fd, id3, size);
            size = 0;
            break;

        case MP4_hdlr:
            lseek(fd, 8, SEEK_CUR);
            read_uint32be(fd, &handler);
            size -= 12;
            /* DEBUGF("    Handler '%c%c%c%c'\n", handler >> 24 & 0xff,
                handler >> 16 & 0xff, handler >> 8 & 0xff,handler & 0xff); */
            break;

        case MP4_stts:
            {
                uint32_t entries;
                unsigned int i;

                /* Reset to false. */
                id3->needs_upsampling_correction = false;

                lseek(fd, 4, SEEK_CUR);
                read_uint32be(fd, &entries);
                id3->samples = 0;

                for (i = 0; i < entries; i++)
                {
                    uint32_t n;
                    uint32_t l;

                    read_uint32be(fd, &n);
                    read_uint32be(fd, &l);

                    /* Some AAC file use HE profile. In this case the number
                     * of output samples is doubled to a maximum of 2048
                     * samples per frame. This means that files which already
                     * report a frame size of 2048 in their header will not
                     * need any further special handling. */
                    if (id3->codectype==AFMT_MP4_AAC_HE && l<=1024)
                    {
                        l *= 2;
                        id3->needs_upsampling_correction = true;
                    }

                    id3->samples += (uint64_t) n * l;
                }

                size = 0;
            }
            break;

        case MP4_mp4a:
            {
                uint32_t subsize;
                uint32_t subtype;

                /* Move to the next expected mp4 atom. */
                lseek(fd, 28, SEEK_CUR);
                read_mp4_atom(fd, &subsize, &subtype, size);
                size -= 36;

                if (subtype == MP4_esds)
                {
                    /* Read esds metadata and return if AAC-HE/SBR is used. */
                    if (read_mp4_esds(fd, id3, &size))
                        id3->codectype = AFMT_MP4_AAC_HE;
                    else
                        id3->codectype = AFMT_MP4_AAC;
                }
            }
            break;

        case MP4_alac:
            {
                uint32_t frequency;
                uint32_t subsize;
                uint32_t subtype;

                /* Move to the next expected mp4 atom. */
                lseek(fd, 28, SEEK_CUR);
                read_mp4_atom(fd, &subsize, &subtype, size);
                size -= 36;
#if 0
                /* We might need to parse for the alac metadata atom. */
                while (!((subsize==28) && (subtype==MP4_alac)) && (size>0))
                {
                    lseek(fd, -7, SEEK_CUR);
                    read_mp4_atom(fd, &subsize, &subtype, size);
                    size -= 1;
                    errno = 0; /* will most likely be set while parsing */
                }
#endif
                if (subtype == MP4_alac)
                {
                    lseek(fd, 24, SEEK_CUR);
                    read_uint32be(fd, &frequency);
                    size -= 28;
                    id3->frequency = frequency;
                    id3->codectype = AFMT_MP4_ALAC;
                }
            }
            break;

        case MP4_mdat:
            /* Some AAC files appear to contain additional empty mdat chunks.
               Ignore them. */
            if(size == 0)
                break;
            /* mdat chunks accumulate! */
            id3->filesize += size;
            if(id3->samples > 0) {
                /* We've already seen the moov chunk. */
                done = true;
            }
            break;

        case MP4_chpl:
            {
                /* ADDME: add support for real chapters. Right now it's only
                 * used for Nero's gapless hack */
                uint8_t chapters   = 0;
                uint64_t timestamp = 0;

                lseek(fd, 8, SEEK_CUR);
                read_uint8(fd, &chapters);
                size -= 9;

                /* the first chapter will be used as the lead_trim */
                if (chapters > 0) {
                    read_uint64be(fd, &timestamp);
                    id3->lead_trim = (timestamp * id3->frequency) / 10000000;
                    size -= 8;
                }
            }
            break;

        default:
            break;
        }

        /* Skip final seek. */
        if (!done)
        {
            lseek(fd, size, SEEK_CUR);
        }
    } while (rc && (size_left > 0) && (errno == 0) && !done);

    return rc;
}

bool get_mp4_metadata(int fd, struct mp3entry* id3)
{
    id3->codectype = AFMT_UNKNOWN;
    id3->filesize = 0;
    errno = 0;

    if (read_mp4_container(fd, id3, ffilesize(fd)) && (errno == 0)
        && (id3->samples > 0) && (id3->frequency > 0)
        && (id3->filesize > 0))
    {
        if (id3->codectype == AFMT_UNKNOWN)
        {
            logf("Not an ALAC or AAC file");
            return false;
        }

        id3->length = ((int64_t) id3->samples * 1000) / id3->frequency;

        id3->vbr = true; /* ALAC is native VBR, AAC very unlikely is CBR. */

        if (id3->length <= 0)
        {
            logf("mp4 length invalid!");
            return false;
        }

        id3->bitrate = ((int64_t) id3->filesize * 8) / id3->length;
        DEBUGF("MP4 bitrate %d, frequency %ld Hz, length %ld ms\n",
            id3->bitrate, id3->frequency, id3->length);

        /* Only count chapters here; the table itself is read on demand
         * via metadata_read_chapters() into a caller-owned buffer. A single
         * Nero chapter is the Nero gapless hack, not a real chapter list. */
        int count = mp4_read_chapters(fd, NULL);
        id3->chapter_count = count > 0 ? count : 0;
        if (id3->chapter_count > 1)
            id3->is_audiobook = true;
#if defined(HAVE_IPOD_UI) || defined(HAVE_BCMFW_VIDEO)
        /* a video is never an audiobook and its audio starts with the
         * picture: no 'chpl' lead_trim (brief 3.2, H3/M6). chapter_count
         * stays: playback loads the chapters on it. */
        if (id3->is_video)
        {
            id3->is_audiobook = false;
            id3->lead_trim = 0;
        }
#endif
    }
    else
    {
        logf("MP4 metadata error");
        DEBUGF("MP4 metadata error. errno %d, samples %ld, frequency %ld, "
            "filesize %ld\n", errno, id3->samples, id3->frequency,
            id3->filesize);
        return false;
    }

    return true;
}

/* ------------------------------------------------------------------------
 * Chapter reading (Nero 'chpl' and QuickTime 'chap' text track).
 *
 * This is a separate, lightweight walk over the atom tree. All of its state
 * lives in a small (~130 byte) on-stack context and chapter data is written
 * only into the caller-supplied table, so concurrent callers (tagcache
 * thread, audio thread) never share anything.
 * ------------------------------------------------------------------------ */

/* sample tables of one 'trak', as file offsets of their first entry */
struct mp4_chap_trak {
    uint32_t track_id;
    uint32_t handler;
    uint32_t timescale;
    uint32_t stts_pos, stts_n;
    uint32_t stsz_pos, stsz_n, stsz_uniform;
    uint32_t stsc_pos, stsc_n;
    uint32_t stco_pos, stco_n;
    bool co64;
};

struct mp4_chap_ctx {
    struct audiobook_chapters *out; /* NULL: count only */
    int nero_count;                 /* -1: no chpl seen */
#ifdef HAVE_METADATA_EXTRAS
    int nero_declared;              /* chpl count from its header */
#endif
    uint32_t chap_track_id;         /* from tref/chap, 0 if none */
    bool have_text;
    struct mp4_chap_trak cur;       /* trak currently being walked */
    struct mp4_chap_trak text;      /* candidate chapter text track */
#ifdef HAVE_METADATA_EXTRAS
    uint32_t chap_ids[MP4_MAX_CHAP_REFS]; /* all of tref/chap */
    bool have_vide;
    struct mp4_chap_trak vide;      /* candidate chapter image track */
    uint32_t chpl_body, chpl_size;  /* the 'chpl' atom, titles read last */
    struct mp4_win *win;            /* the caller's read window, or NULL */
    uint32_t rpos;                  /* the walk's read position */
#endif
};

#ifndef HAVE_METADATA_EXTRAS          /* the extras read through windows */
static bool chap_rd8(int fd, uint8_t *v)
{
    return read(fd, v, 1) == 1;
}

static bool chap_rd32(int fd, uint32_t *v)
{
    return read_uint32be(fd, v) == 4;
}

static bool chap_rd32_at(int fd, uint32_t pos, uint32_t *v)
{
    return lseek(fd, pos, SEEK_SET) == (off_t)pos && chap_rd32(fd, v);
}

/* copy a title of 'len' bytes at the current file position (truncated to
 * fit, NUL terminated) and leave the file positioned after it */
static void chap_read_title(int fd, char *dst, unsigned len)
{
    unsigned want = len < AB_MAX_CHAPTER_NAME - 1 ? len
                                                  : AB_MAX_CHAPTER_NAME - 1;
    ssize_t got = want ? read(fd, dst, want) : 0;
    if (got < 0)
        got = 0;
    if (len > (unsigned)got)
    {
        lseek(fd, len - got, SEEK_CUR);
        /* truncated: don't leave a partial UTF-8 sequence at the end */
        ssize_t i = got;
        while (i > 0 && ((uint8_t)dst[i - 1] & 0xc0) == 0x80)
            i--;
        if (i > 0 && ((uint8_t)dst[i - 1] & 0xc0) == 0xc0)
        {
            uint8_t lead = dst[i - 1];
            ssize_t need = lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3 : 2;
            if (got - (i - 1) < need)
                got = i - 1;
        }
    }
    dst[got] = '\0';
}
#else /* HAVE_METADATA_EXTRAS */
/* Read windows (P6 fix 2; owner rule: no streaming through many small
 * reads). The atom walk, the sample tables and the chapter titles are
 * read through windows carved from a buffer the CALLER passes in: each
 * fill is one large read of a contiguous range that is known to be needed
 * (up to the window's 'end': the container, the table, the run of
 * adjacent text samples), sized to the buffer; a fill that overlaps what
 * the window holds keeps those bytes and reads only the rest, so no byte
 * is read twice and nothing is re-sought per sample. No window
 * (w == NULL, or a read larger than it) reads the bytes directly, as
 * before. Nothing static: the tagcache, audio and UI threads each bring
 * their own buffer. */
struct mp4_win {
    uint8_t *buf;
    uint32_t cap;                   /* 0: no window */
    uint32_t pos, len;              /* file range held in buf */
    uint32_t end;                   /* fills stop here (0: exact reads) */
    bool peek;                      /* no I/O: fail what is not held */
};

/* one read: the window holds [pos, pos + fill) after it (less at the end
 * of the file); what it held of that range already is kept, not read */
static bool win_fill(int fd, struct mp4_win *w, uint32_t pos, uint32_t fill)
{
    uint32_t keep = 0, from = pos;
    ssize_t got;

    if (pos >= w->pos && pos - w->pos <= w->len)
    {
        keep = w->len - (pos - w->pos);
        if (keep > fill)
            keep = fill;
        memmove(w->buf, w->buf + (pos - w->pos), keep);
        from = pos + keep;
    }
    w->pos = pos;
    w->len = keep;
    if (keep == fill)
        return true;
    if (lseek(fd, from, SEEK_SET) != (off_t)from)
        return false;
    got = read(fd, w->buf + keep, fill - keep);
    if (got < 0)
        return false;
    w->len = keep + (uint32_t)got;
    return true;
}

/* copy up to n bytes at file offset pos into dst; returns the count
 * (short at the end of the file), or -1 */
static ssize_t win_read(int fd, struct mp4_win *w, uint32_t pos, void *dst,
                        uint32_t n)
{
    uint32_t at;

    if (w && w->peek)
    {
        if (pos < w->pos || pos - w->pos > w->len
            || n > w->len - (pos - w->pos))
            return -1;
    }
    else if (!w || n > w->cap)
    {
        if (lseek(fd, pos, SEEK_SET) != (off_t)pos)
            return -1;
        return n ? read(fd, dst, n) : 0;
    }
    else if (pos < w->pos || pos - w->pos > w->len
             || n > w->len - (pos - w->pos))
    {
        uint32_t fill = n;

        if (w->end > pos && w->end - pos > n)
            fill = w->end - pos < w->cap ? w->end - pos : w->cap;
        if (!win_fill(fd, w, pos, fill))
            return -1;
    }
    at = pos - w->pos;
    if (n > w->len - at)
        n = w->len - at;
    memcpy(dst, w->buf + at, n);
    return n;
}

/* make [pos, pos + n) held (n <= cap), reading only what is missing */
static bool win_hold(int fd, struct mp4_win *w, uint32_t pos, uint32_t n)
{
    if (!w || n > w->cap)
        return false;
    if (pos >= w->pos && pos - w->pos <= w->len
        && n <= w->len - (pos - w->pos))
        return true;
    return win_fill(fd, w, pos, n);
}

static uint32_t chap_be32(const uint8_t *b)
{
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16)
           | ((uint32_t)b[2] << 8) | b[3];
}

static bool win_rd32(int fd, struct mp4_win *w, uint32_t pos, uint32_t *v)
{
    uint8_t b[4];
    if (win_read(fd, w, pos, b, 4) != 4)
        return false;
    *v = chap_be32(b);
    return true;
}

/* Copy a title of 'len' bytes at file offset 'pos' as UTF-8 (truncated to
 * fit, NUL terminated). Like the stock chap_read_title(), but decodes
 * UTF-16 titles (with a BOM, as QuickTime text samples and some 'chpl'
 * writers store them) and repairs invalid UTF-8 (taken as Latin-1). */
static void chap_title_at(int fd, struct mp4_win *w, uint32_t pos,
                          unsigned len, char *dst)
{
    uint8_t raw[136];               /* >= 2 + 2 * (AB_MAX_CHAPTER_NAME - 1) */
    unsigned want = len < sizeof(raw) ? len : sizeof(raw);
    ssize_t got = win_read(fd, w, pos, raw, want);
    int enc = META_TEXT_UTF8;

    if (got < 0)
        got = 0;
    if (got >= 2 && ((raw[0] == 0xfe && raw[1] == 0xff)
                     || (raw[0] == 0xff && raw[1] == 0xfe)))
        enc = META_TEXT_UTF16;
    meta_text_to_utf8(enc, META_TEXT_STOP_NUL, raw, got, dst,
                      AB_MAX_CHAPTER_NAME);
}
#endif

/* The walk's reads (and the 'chpl' header's): stock reads at the file
 * position as it always did (the same tokens, so the same code); the
 * extras read at c->rpos through the caller's window, when there is one */
#ifdef HAVE_METADATA_EXTRAS
static bool cw_rd(int fd, struct mp4_chap_ctx *c, void *v, uint32_t n)
{
    bool ok = win_read(fd, c->win, c->rpos, v, n) == (ssize_t)n;
    c->rpos += n;
    return ok;
}

static bool cw_rd32(int fd, struct mp4_chap_ctx *c, uint32_t *v)
{
    uint8_t b[4];
    if (!cw_rd(fd, c, b, 4))
        return false;
    *v = chap_be32(b);
    return true;
}
#define CW_RD8(v)        cw_rd(fd, c, (v), 1)
#define CW_RD32(v)       cw_rd32(fd, c, (v))
#define CW_RD32_AT(p, v) (c->rpos = (p), cw_rd32(fd, c, (v)))
#define CW_SKIP(n)       (c->rpos += (n))
#else
#define CW_RD8(v)        chap_rd8(fd, (v))
#define CW_RD32(v)       chap_rd32(fd, (v))
#define CW_RD32_AT(p, v) chap_rd32_at(fd, (p), (v))
#define CW_SKIP(n)       lseek(fd, (n), SEEK_CUR)
#endif

static void chap_read_chpl(int fd, struct mp4_chap_ctx *c, uint32_t size)
{
#ifndef HAVE_METADATA_EXTRAS
    uint8_t version, count;
#else
    uint8_t version, count8;
    int count;
#endif
    uint32_t used = 4;

    if (size < 5 || !CW_RD8(&version))
        return;
    CW_SKIP(3);                 /* flags */
#ifndef HAVE_METADATA_EXTRAS
    if (version == 1)
    {
        CW_SKIP(4);             /* reserved */
        used += 4;
    }
    if (size < used + 1 || !CW_RD8(&count))
        return;
    used += 1;

    c->nero_count = count;                  /* <= 255 == AB_MAX_CHAPTERS */
#else
    /* Version 1 has 4 more bytes before the 8-bit count. ffmpeg (and
     * Nero) write them as 0 and stop at 255 chapters; mp4v2 writes one
     * reserved byte and a 32-bit count, whose low byte is in the same
     * place. Reading bytes 1-3 as the high bytes of the count handles
     * both. Version 0 has the 8-bit count only. */
    uint32_t hi = 0;
    if (version == 1)
    {
        if (size < used + 4 || !CW_RD32(&hi))
            return;
        used += 4;
    }
    if (size < used + 1 || !CW_RD8(&count8))
        return;
    used += 1;
    uint32_t n = ((hi & 0xffffff) << 8) | count8;
    if (n > (size - used) / 9)              /* 9 bytes per entry, at least */
        n = (size - used) / 9;
    count = n < AB_MAX_CHAPTERS ? (int)n : AB_MAX_CHAPTERS;
    c->nero_count = count;
    c->nero_declared = count;
#endif
    if (!c->out || count == 0)
        return;

    int i;
#ifdef HAVE_METADATA_EXTRAS
    if (count > ab_ch_cap(c->out))
        count = ab_ch_cap(c->out);
    ab_ch_expect(c->out, count);
#endif
#ifdef HAVE_METADATA_EXTRAS
    uint32_t pos = c->chpl_body + used;
    for (i = 0; i < count && size >= used + 9; i++)
    {
        struct audiobook_chapters *ab = c->out;
        uint8_t e[9];
        uint64_t ts;
        unsigned len;

        if (win_read(fd, c->win, pos, e, 9) != 9)
            break;
        ts = ((uint64_t)chap_be32(e) << 32) | chap_be32(e + 4);
        len = e[8];
        used += 9;
        pos += 9;
        if (len > size - used)
            len = size - used;

        ab_ch_set_start(ab, i, ts / 10000); /* 100ns units */
        chap_title_at(fd, c->win, pos, len, ab_ch_title_buf(ab, i));
        ab_ch_commit(ab, i);
        used += len;
        pos += len;
    }
#else
    for (i = 0; i < count && size >= used + 9; i++)
    {
        struct audiobook_chapters *ab = c->out;
        uint64_t ts;
        uint8_t len;

        if (read_uint64be(fd, &ts) != 8 || !CW_RD8(&len))
            break;
        used += 9;
        if (len > size - used)
            len = size - used;

        ab_ch_set_start(ab, i, ts / 10000); /* 100ns units */
        chap_read_title(fd, ab_ch_title_buf(ab, i), len);
        ab_ch_commit(ab, i);                /* stock: nothing */
        used += len;
    }
#endif
    c->nero_count = i;
}

#ifdef HAVE_METADATA_EXTRAS
static bool chap_is_ref(const struct mp4_chap_ctx *c, uint32_t id)
{
    unsigned k;
    for (k = 0; id && k < MP4_MAX_CHAP_REFS; k++)
        if (c->chap_ids[k] == id)
            return true;
    return false;
}
#endif

static void chap_walk(int fd, struct mp4_chap_ctx *c, uint32_t pos,
                      uint32_t end, int depth)
{
    while (pos < end && end - pos >= 8)
    {
        uint32_t size, type, v;
        uint8_t ver;

#ifdef HAVE_METADATA_EXTRAS
        /* the header and the few fields a leaf is read for, in one read
         * unless held already (a container that fit was read whole) */
        win_hold(fd, c->win, pos, end - pos < 32 ? end - pos : 32);
#endif
        if (!CW_RD32_AT(pos, &size) || !CW_RD32(&type))
            return;
        if (size == 0)
            size = end - pos;               /* extends to end */
        if (size < 8 || size > end - pos)
            return;                         /* 64-bit or corrupt size */

        uint32_t body = pos + 8, bsize = size - 8;

#ifdef HAVE_METADATA_EXTRAS
        /* a container that fits the window: all of it in one read (its
         * children's headers and fields; the big sample tables of a long
         * track do not fit, and are skipped header by header) */
        if (c->win && size <= c->win->cap
            && (type == MP4_moov || type == MP4_trak || type == MP4_udta
                || type == MP4_mdia || type == MP4_minf || type == MP4_stbl
                || type == MP4_tref))
            win_hold(fd, c->win, pos, size);
#endif
        switch (type)
        {
        case MP4_moov:
        case MP4_udta:
        case MP4_mdia:
        case MP4_minf:
        case MP4_stbl:
        case MP4_tref:
            if (depth < 8)
                chap_walk(fd, c, body, body + bsize, depth + 1);
            if (type == MP4_moov)
                return;                     /* nothing of interest after */
            break;

        case MP4_trak:
            memset(&c->cur, 0, sizeof(c->cur));
            if (depth < 8)
                chap_walk(fd, c, body, body + bsize, depth + 1);
            if (c->cur.handler == MP4_text && c->cur.timescale
                && c->cur.stts_n && c->cur.stsz_n && c->cur.stsc_n
                && c->cur.stco_n
                && (!c->have_text || c->cur.track_id == c->chap_track_id))
            {
                c->text = c->cur;
                c->have_text = true;
            }
#ifdef HAVE_METADATA_EXTRAS
            if (c->cur.handler == MP4_vide && c->cur.timescale
                && c->cur.stts_n && c->cur.stsz_n && c->cur.stsc_n
                && c->cur.stco_n
                && (!c->have_vide || chap_is_ref(c, c->cur.track_id)))
            {
                c->vide = c->cur;
                c->have_vide = true;
            }
#endif
            break;

        case MP4_tkhd:
            if (bsize >= 32 && CW_RD8(&ver))
            {
                CW_SKIP(ver == 1 ? 3 + 16 : 3 + 8);
                CW_RD32(&c->cur.track_id);
            }
            break;

        case MP4_chap:
            if (bsize >= 4 && CW_RD32(&v) && !c->chap_track_id)
                c->chap_track_id = v;
#ifdef HAVE_METADATA_EXTRAS
            if (bsize >= 4 && c->chap_track_id == v && !c->chap_ids[0])
            {
                unsigned k;
                c->chap_ids[0] = v;
                for (k = 1; k < MP4_MAX_CHAP_REFS && bsize >= 4 * (k + 1)
                            && CW_RD32(&v); k++)
                    c->chap_ids[k] = v;
            }
#endif
            break;

        case MP4_hdlr:
            if (bsize >= 12)
            {
                CW_SKIP(8);
                CW_RD32(&c->cur.handler);
            }
            break;

        case MP4_mdhd:
            if (bsize >= 24 && CW_RD8(&ver))
            {
                CW_SKIP(ver == 1 ? 3 + 16 : 3 + 8);
                CW_RD32(&c->cur.timescale);
            }
            break;

        case MP4_stts:
            if (bsize >= 8 && CW_RD32(&v) && CW_RD32(&v)
                && v <= (bsize - 8) / 8)
            {
                c->cur.stts_n = v;
                c->cur.stts_pos = body + 8;
            }
            break;

        case MP4_stsz:
            if (bsize >= 12 && CW_RD32(&v)
                && CW_RD32(&c->cur.stsz_uniform) && CW_RD32(&v)
                && (c->cur.stsz_uniform || v <= (bsize - 12) / 4))
            {
                c->cur.stsz_n = v;
                c->cur.stsz_pos = body + 12;
            }
            break;

        case MP4_stsc:
            if (bsize >= 8 && CW_RD32(&v) && CW_RD32(&v)
                && v <= (bsize - 8) / 12)
            {
                c->cur.stsc_n = v;
                c->cur.stsc_pos = body + 8;
            }
            break;

        case MP4_stco:
        case MP4_co64:
            if (bsize >= 8 && CW_RD32(&v) && CW_RD32(&v)
                && v <= (bsize - 8) / (type == MP4_co64 ? 8 : 4))
            {
                c->cur.stco_n = v;
                c->cur.stco_pos = body + 8;
                c->cur.co64 = (type == MP4_co64);
            }
            break;

        case MP4_chpl:
            if (c->nero_count < 0)
            {
#ifdef HAVE_METADATA_EXTRAS
                /* count only: its titles are read after the walk, and
                 * only if a longer text track does not replace it */
                struct audiobook_chapters *out = c->out;
                c->out = NULL;
                c->chpl_body = body;
                c->chpl_size = bsize;
                chap_read_chpl(fd, c, bsize);
                c->out = out;
#else
                chap_read_chpl(fd, c, bsize);
#endif
            }
            break;

        default:
            break;
        }

        pos += size;
    }
}

#ifdef HAVE_METADATA_EXTRAS
enum { MP4_W_STTS, MP4_W_STSZ, MP4_W_STSC, MP4_W_STCO, MP4_W_DATA,
       MP4_W_COUNT };

struct mp4_sample_it {
    const struct mp4_chap_trak *t;
    struct mp4_win *w[MP4_W_COUNT]; /* NULL: direct reads */
    uint32_t idx;
    uint64_t elapsed;               /* track timescale units */
    uint32_t stts_idx, run_left, delta;
    uint32_t stsc_idx, next_first, spc;
    uint32_t chunk, left_in_chunk, off;
};

/* Carve the windows for reading 'count' samples of 't' from the caller's
 * scratch (the window structs first). The four tables of a 'stbl' sit
 * next to each other: when the span from the first to the end of the
 * last fits in half the room, one window covers them all (one read).
 * Otherwise each table gets a share of that half in proportion to the
 * bytes it will be read for (never more than that), so they refill
 * about equally often. The sample data gets the rest. Every window's
 * fills stop at the end of its table. False when the scratch is too
 * small to help (direct reads). */
static bool mp4_windows(struct mp4_sample_it *it, uint32_t count,
                        void *scratch, size_t size,
                        const struct mp4_win *held)
{
    const struct mp4_chap_trak *t = it->t;
    struct mp4_win *w = scratch;
    uint8_t *p;
    uint32_t need[MP4_W_DATA], start[MP4_W_DATA], lo = 0xffffffffu, hi = 0;
    uint32_t total = 0, tabs, room, used = 0;
    int k;

    if (!scratch || size < sizeof(*w) * MP4_W_COUNT + AB_CH_SCRATCH_MIN
        || ((uintptr_t)scratch & 3))
        return false;
    p = (uint8_t *)(w + MP4_W_COUNT);
    room = (uint32_t)(size - sizeof(*w) * MP4_W_COUNT) & ~3u;
    start[MP4_W_STTS] = t->stts_pos;
    need[MP4_W_STTS] = (t->stts_n < count ? t->stts_n : count) * 8;
    start[MP4_W_STSZ] = t->stsz_pos;
    need[MP4_W_STSZ] = t->stsz_uniform ? 0 : count * 4;
    start[MP4_W_STSC] = t->stsc_pos;
    need[MP4_W_STSC] = t->stsc_n * 12;
    start[MP4_W_STCO] = t->stco_pos;
    need[MP4_W_STCO] = (t->stco_n < count ? t->stco_n : count)
                       * (t->co64 ? 8 : 4);
    for (k = 0; k < MP4_W_DATA; k++)
    {
        if (!need[k])
            continue;
        if (start[k] < lo)
            lo = start[k];
        if (start[k] + need[k] > hi)
            hi = start[k] + need[k];
        total += need[k];
    }
    tabs = room / 2;
    if (hi > lo && hi - lo <= tabs && hi - lo <= total + total / 4)
    {
        /* the walk read them already (a container that fit): moved, not
         * read again (before the window structs overwrite the start) */
        bool have = held && held->buf == scratch && lo >= held->pos
                    && lo - held->pos <= held->len
                    && hi - lo <= held->len - (lo - held->pos);
        if (have)
            memmove(p, held->buf + (lo - held->pos), hi - lo);
        memset(w, 0, sizeof(*w) * MP4_W_COUNT);
        if (have)
        {
            w[0].pos = lo;
            w[0].len = hi - lo;
        }
        /* one window over the whole run of tables (gaps: the tables'
         * atom headers, and at most a quarter of their bytes) */
        w[0].buf = p;
        w[0].cap = (hi - lo + 3) & ~3u;
        w[0].end = hi;
        for (k = 0; k < MP4_W_DATA; k++)
            it->w[k] = &w[0];
        used = w[0].cap;
    }
    else
    {
        memset(w, 0, sizeof(*w) * MP4_W_COUNT);
        for (k = 0; k < MP4_W_DATA; k++)
        {
            uint32_t cap = need[k];

            if (!cap)
                continue;
            if (total > tabs)
                cap = (uint32_t)((uint64_t)tabs * need[k] / total);
            cap &= ~3u;
            if (cap < 16)               /* one entry at least */
                cap = 16;
            w[k].buf = p + used;
            w[k].cap = cap;
            w[k].end = start[k] + need[k];
            it->w[k] = &w[k];
            used += cap;
        }
    }
    if (used + 160 > room)              /* a title (2 + 136 B) must fit */
    {
        memset(it->w, 0, sizeof(it->w));
        return false;
    }
    w[MP4_W_DATA].buf = p + used;
    w[MP4_W_DATA].cap = room - used;
    it->w[MP4_W_DATA] = &w[MP4_W_DATA];
    return true;
}

/* next sample's start (ms), file offset and size; false at the end or on
 * a read error / inconsistent tables (in peek mode: also when the tables'
 * windows do not hold what it needs) */
static bool mp4_sample_next(int fd, struct mp4_sample_it *it,
                            unsigned long *start_ms, uint32_t *off,
                            uint32_t *size)
{
    const struct mp4_chap_trak *t = it->t;
    uint8_t e[8];

    if (it->idx >= t->stsz_n)
        return false;
    if (it->idx == 0 && !win_rd32(fd, it->w[MP4_W_STSC], t->stsc_pos,
                                  &it->next_first))
        return false;

    while (it->run_left == 0 && it->stts_idx < t->stts_n)
    {
        if (win_read(fd, it->w[MP4_W_STTS],
                     t->stts_pos + it->stts_idx * 8, e, 8) != 8)
            return false;
        it->run_left = chap_be32(e);
        it->delta = chap_be32(e + 4);
        it->stts_idx++;
    }
    *start_ms = (unsigned long)(it->elapsed * 1000 / t->timescale);
    if (it->run_left)
    {
        it->run_left--;
        it->elapsed += it->delta;
    }

    *size = t->stsz_uniform;
    if (!*size && !win_rd32(fd, it->w[MP4_W_STSZ],
                            t->stsz_pos + it->idx * 4, size))
        return false;

    while (it->left_in_chunk == 0)
    {
        if (++it->chunk > t->stco_n)
            return false;
        while (it->stsc_idx < t->stsc_n && it->next_first <= it->chunk)
        {
            if (!win_rd32(fd, it->w[MP4_W_STSC],
                          t->stsc_pos + it->stsc_idx * 12 + 4, &it->spc))
                return false;
            if (++it->stsc_idx < t->stsc_n
                && !win_rd32(fd, it->w[MP4_W_STSC],
                             t->stsc_pos + it->stsc_idx * 12,
                             &it->next_first))
                return false;
        }
        it->left_in_chunk = it->spc;
        if (t->co64)
        {
            if (win_read(fd, it->w[MP4_W_STCO],
                         t->stco_pos + (it->chunk - 1) * 8, e, 8) != 8
                || chap_be32(e))
                return false;           /* beyond 4GB: impossible on FAT */
            it->off = chap_be32(e + 4);
        }
        else if (!win_rd32(fd, it->w[MP4_W_STCO],
                           t->stco_pos + (it->chunk - 1) * 4, &it->off))
            return false;
    }
    it->left_in_chunk--;
    *off = it->off;
    it->off += *size;
    it->idx++;
    return true;
}

static void it_peek(struct mp4_sample_it *it, bool on)
{
    int k;
    for (k = 0; k < MP4_W_DATA; k++)
        if (it->w[k])
            it->w[k]->peek = on;
}

/* Before the data window is filled at sample 'off' (size 'ssize'): its
 * fills stop at the end of the run of samples that follow it back to
 * back (a contiguous run of the text track, never the audio between its
 * chunks), as far as the tables' windows show without reading. */
static void data_run_end(struct mp4_sample_it *it, uint32_t off,
                         uint32_t ssize, int left)
{
    struct mp4_win *d = it->w[MP4_W_DATA];
    struct mp4_sample_it la = *it;      /* lookahead: no I/O */
    uint32_t end = off + ssize, noff, nsize;
    unsigned long ms;

    it_peek(it, true);
    while (left-- > 0 && end - off < d->cap
           && mp4_sample_next(-1, &la, &ms, &noff, &nsize) && noff == end)
        end += nsize;
    it_peek(it, false);
    d->end = end;
}

/* Resolve the QuickTime chapter text track: one sample per chapter, its
 * start time from 'stts', its location from 'stsc'/'stco'/'stsz'. Each
 * sample is a 16-bit big-endian length followed by the title (UTF-8, or
 * UTF-16 with a BOM). Reads through the caller's scratch when it has one
 * (mp4_windows()); tables inconsistent or unreadable part-way keep the
 * chapters before that point. */
static int chap_read_text_track(int fd, const struct mp4_chap_trak *t,
                                struct audiobook_chapters *out,
                                void *scratch, size_t scratch_size,
                                const struct mp4_win *held)
{
    int count = t->stsz_n < AB_MAX_CHAPTERS ? (int)t->stsz_n : AB_MAX_CHAPTERS;
    struct mp4_sample_it it;
    struct mp4_win *d;
    int i;

    if (!out)
        return count;
    if (count > ab_ch_cap(out))
        count = ab_ch_cap(out);
    ab_ch_expect(out, count);

    memset(&it, 0, sizeof(it));
    it.t = t;
    mp4_windows(&it, count, scratch, scratch_size, held);
    d = it.w[MP4_W_DATA];
    for (i = 0; i < count; i++)
    {
        char *title = ab_ch_title_buf(out, i);
        unsigned long start;
        uint32_t off, ssize, need;
        uint8_t lenbuf[2];

        if (!mp4_sample_next(fd, &it, &start, &off, &ssize))
            return i;
        ab_ch_set_start(out, i, start);
        title[0] = '\0';
        need = ssize < 2 + 136 ? ssize : 2 + 136;
        if (ssize > 2 && d
            && (off < d->pos || off - d->pos > d->len
                || need > d->len - (off - d->pos)))
            data_run_end(&it, off, ssize, count - 1 - i);
        if (ssize > 2 && win_read(fd, d, off, lenbuf, 2) == 2)
        {
            unsigned len = (lenbuf[0] << 8) | lenbuf[1];
            if (len > ssize - 2)
                len = ssize - 2;
            chap_title_at(fd, d, off + 2, len, title);
        }
        ab_ch_commit(out, i);
    }
    return count;
}
#else /* !HAVE_METADATA_EXTRAS */
/* Resolve the QuickTime chapter text track: one sample per chapter, its
 * start time from 'stts', its location from 'stsc'/'stco'/'stsz'. Each
 * sample is a 16-bit big-endian length followed by the (UTF-8) title. */
static int chap_read_text_track(int fd, const struct mp4_chap_trak *t,
                                struct audiobook_chapters *out)
{
    int count = t->stsz_n < AB_MAX_CHAPTERS ? (int)t->stsz_n : AB_MAX_CHAPTERS;
    if (!out)
        return count;
#ifdef HAVE_METADATA_EXTRAS
    if (count > ab_ch_cap(out))
        count = ab_ch_cap(out);
    ab_ch_expect(out, count);
#endif

    uint64_t elapsed = 0;                   /* in track timescale units */
    uint32_t stts_idx = 0, run_left = 0, delta = 0;
    uint32_t stsc_idx = 0, next_first = 0, spc = 0;
    uint32_t chunk = 0, left_in_chunk = 0, sample_off = 0;
    int i;

    if (!chap_rd32_at(fd, t->stsc_pos, &next_first))
        return -1;

    for (i = 0; i < count; i++)
    {
        char *title = ab_ch_title_buf(out, i);
        uint32_t ssize = t->stsz_uniform;

        /* start time: sum of the durations of all previous samples */
        while (run_left == 0 && stts_idx < t->stts_n)
        {
            if (!chap_rd32_at(fd, t->stts_pos + stts_idx * 8, &run_left)
                || !chap_rd32(fd, &delta))
                return -1;
            stts_idx++;
        }
        ab_ch_set_start(out, i,
                        (unsigned long)(elapsed * 1000 / t->timescale));
        title[0] = '\0';
        if (run_left)
        {
            run_left--;
            elapsed += delta;
        }

        /* sample size */
        if (!ssize && !chap_rd32_at(fd, t->stsz_pos + i * 4, &ssize))
            return -1;

        /* sample offset: advance to the chunk holding this sample */
        while (left_in_chunk == 0)
        {
            if (++chunk > t->stco_n)
                return i;                   /* tables inconsistent */
            while (stsc_idx < t->stsc_n && next_first <= chunk)
            {
                if (!chap_rd32_at(fd, t->stsc_pos + stsc_idx * 12 + 4, &spc))
                    return -1;
                if (++stsc_idx < t->stsc_n
                    && !chap_rd32_at(fd, t->stsc_pos + stsc_idx * 12,
                                     &next_first))
                    return -1;
            }
            left_in_chunk = spc;
            if (t->co64)
            {
                uint32_t hi;
                if (!chap_rd32_at(fd, t->stco_pos + (chunk - 1) * 8, &hi)
                    || !chap_rd32(fd, &sample_off))
                    return -1;
                if (hi)
                    return i;               /* beyond 4GB: impossible on FAT */
            }
            else if (!chap_rd32_at(fd, t->stco_pos + (chunk - 1) * 4,
                                   &sample_off))
                return -1;
        }
        left_in_chunk--;

        /* title */
        if (ssize > 2 && lseek(fd, sample_off, SEEK_SET) == (off_t)sample_off)
        {
            uint8_t lenbuf[2];
            if (read(fd, lenbuf, 2) == 2)
            {
                unsigned len = (lenbuf[0] << 8) | lenbuf[1];
                if (len > ssize - 2)
                    len = ssize - 2;
                chap_read_title(fd, title, len);
#ifndef HAVE_METADATA_EXTRAS
                /* UTF-16 titles (BOM) are not supported; show no title */
                if ((uint8_t)title[0] == 0xfe
                    && (uint8_t)title[1] == 0xff)
                    title[0] = '\0';
#endif
            }
        }
        ab_ch_commit(out, i);               /* stock: nothing */
        sample_off += ssize;
    }

    return count;
}
#endif /* !HAVE_METADATA_EXTRAS */

#ifdef HAVE_METADATA_EXTRAS
int mp4_read_chapters(int fd, struct audiobook_chapters *out)
{
    return mp4_read_chapters_buf(fd, out, NULL, 0);
}

int mp4_read_chapters_buf(int fd, struct audiobook_chapters *out,
                          void *scratch, size_t scratch_size)
#else
int mp4_read_chapters(int fd, struct audiobook_chapters *out)
#endif
{
    struct mp4_chap_ctx c;
    off_t fsize = ffilesize(fd);
    int count = 0;

    memset(&c, 0, sizeof(c));
    c.out = out;
    c.nero_count = -1;
    if (out)
    {
        out->chapter_count = 0;
        out->curr_chapter_idx = 0;
        ab_ch_reset(out);                   /* stock: nothing */
    }

    if (fsize <= 0)
        return -1;
    if ((uint64_t)fsize > 0xffffffffu)
        fsize = 0xffffffffu;

#ifdef HAVE_METADATA_EXTRAS
    /* one window over the caller's scratch for the walk and a 'chpl' */
    struct mp4_win win;
    memset(&win, 0, sizeof(win));
    if (scratch && scratch_size >= AB_CH_SCRATCH_MIN && out)
    {
        win.buf = scratch;
        win.cap = (uint32_t)scratch_size;
        c.win = &win;
    }
#endif
    chap_walk(fd, &c, 0, (uint32_t)fsize, 0);

#ifdef HAVE_METADATA_EXTRAS
    /* ffmpeg writes both a 'chpl' cut at 255 chapters and a QuickTime
     * chapter track with all of them: take the 'chpl' only if the text
     * track has no more chapters. Decided on the header counts, so that
     * the count-only pass and the full read agree. */
    if (c.nero_count >= 0 && c.have_text && c.chap_track_id
        && c.text.track_id == c.chap_track_id
        && c.text.stsz_n > (uint32_t)c.nero_declared)
        c.nero_count = 0;
    /* the walk only counted the 'chpl': read its titles now that it won
     * (so a replaced one costs no reads and no pool space, P6 Rev 1 15),
     * through the same window (what the walk left in it is not read
     * again), filled up to the atom's end */
    if (c.nero_count > 0 && out)
    {
        win.end = c.chpl_body + c.chpl_size;
        c.rpos = c.chpl_body;
        chap_read_chpl(fd, &c, c.chpl_size);
    }
    c.win = NULL;
#endif
    if (c.nero_count > 0)
        count = c.nero_count;
    else if (c.have_text && c.chap_track_id
             && c.text.track_id == c.chap_track_id)
#ifdef HAVE_METADATA_EXTRAS
        count = chap_read_text_track(fd, &c.text, out, scratch,
                                     scratch_size, win.cap ? &win : NULL);
#else
        count = chap_read_text_track(fd, &c.text, out);
#endif

    if (out)
        out->chapter_count = count > 0 ? count : 0;

    return count;
}

#ifdef HAVE_METADATA_EXTRAS
/* ------------------------------------------------------------------------
 * Chapter images (#39): the QuickTime chapter image track, a 'vide' track
 * listed in the audio track's tref/chap next to the text track, one JPEG
 * or PNG sample per picture. Only sample locations are recorded.
 * ------------------------------------------------------------------------ */

static int mp4_art_type(int fd, uint32_t off)
{
    uint8_t m[8];
    if (lseek(fd, off, SEEK_SET) != (off_t)off || read(fd, m, 8) != 8)
        return AB_ART_NONE;
    if (m[0] == 0xff && m[1] == 0xd8 && m[2] == 0xff)
        return AB_ART_JPEG;
    if (!memcmp(m, "\x89PNG\r\n\x1a\n", 8))
        return AB_ART_PNG;
    return AB_ART_NONE;
}

/* an image starting this little after a chapter start still belongs to it
 * (the two tracks use different timescales) */
#define MP4_ART_SLACK_MS 10

int mp4_read_chapter_art(int fd, const struct audiobook_chapters *ab,
                         struct ab_chapter_art *art)
{
    int count = ab->chapter_count;
    struct mp4_chap_ctx c;
    struct mp4_sample_it it;
    unsigned long cur_ms, nxt_ms = 0;
    uint32_t cur_off, cur_size, nxt_off = 0, nxt_size = 0;
    uint32_t typed_off = 0;
    int typed = AB_ART_NONE, found = 0, j;
    bool have_next;
    off_t fsize = ffilesize(fd);

    if (fsize <= 0)
        return -1;
    if ((uint64_t)fsize > 0xffffffffu)
        fsize = 0xffffffffu;

    memset(&c, 0, sizeof(c));
    c.nero_count = -1;
    chap_walk(fd, &c, 0, (uint32_t)fsize, 0);
    if (!c.have_vide || !chap_is_ref(&c, c.vide.track_id))
        return 0;

    memset(&it, 0, sizeof(it));
    it.t = &c.vide;
    if (!mp4_sample_next(fd, &it, &cur_ms, &cur_off, &cur_size))
        return 0;
    have_next = mp4_sample_next(fd, &it, &nxt_ms, &nxt_off, &nxt_size);

    for (j = 0; j < count; j++)
    {
        unsigned long at = ab_ch_start(ab, j) + MP4_ART_SLACK_MS;

        while (have_next && nxt_ms <= at)
        {
            cur_ms = nxt_ms;
            cur_off = nxt_off;
            cur_size = nxt_size;
            have_next = mp4_sample_next(fd, &it, &nxt_ms, &nxt_off,
                                        &nxt_size);
        }
        if (cur_ms > at || cur_size == 0 || cur_size >= (1u << 24))
            continue;
        if (typed_off != cur_off || typed == AB_ART_NONE)
        {
            typed = mp4_art_type(fd, cur_off);
            typed_off = cur_off;
        }
        if (typed == AB_ART_NONE)
            continue;
        art[j].pos = cur_off;
        art[j].size = cur_size;
        art[j].type = typed;
        found++;
    }
    return found;
}
#endif /* HAVE_METADATA_EXTRAS */
