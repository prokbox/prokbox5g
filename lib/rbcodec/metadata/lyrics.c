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
#include <string.h>
#include "platform.h"
#include "metadata.h"
#include "chapters.h"
#include "lyrics.h"

#ifdef HAVE_METADATA_EXTRAS

bool metadata_has_lyrics(const struct mp3entry *id3)
{
    return id3 && id3->lyrics.size != 0;
}

/* MP4: the text is the payload of the (c)lyr 'data' atom */
static int mp4_read_lyrics(int fd, const struct embedded_lyrics *l,
                           struct meta_text *t)
{
    uint8_t b[64];
    uint32_t left = l->size, carry = 0;

    if (lseek(fd, l->pos, SEEK_SET) != (off_t)l->pos)
        return -1;
    while (!t->done)
    {
        uint32_t want = sizeof(b) - carry;
        if (want > left)
            want = left;
        ssize_t got = want ? read(fd, b + carry, want) : 0;
        if (got < 0)
            return -1;
        left -= got;
        uint32_t have = carry + got;
        if (have == 0)
            break;
        uint32_t used = meta_text_feed(t, b, have);
        carry = have - used;
        if (carry)
        {
            if (got == 0)
                break;                  /* trailing partial character */
            memmove(b, b + used, carry);
        }
    }
    return t->len;
}

int metadata_read_lyrics(int fd, const struct mp3entry *id3,
                         char *buf, size_t size, bool *truncated)
{
    struct meta_text t;
    int rc = -1;

    if (truncated)
        *truncated = false;
    if (!buf || size == 0)
        return -1;
    buf[0] = '\0';
    if (fd < 0 || !metadata_has_lyrics(id3))
        return -1;

    meta_text_init(&t, META_TEXT_UTF8, META_TEXT_NEWLINES | META_TEXT_STOP_NUL,
                   buf, size > 0xffffffffu ? 0xffffffffu : size);

    switch (id3->lyrics.format)
    {
    case LYRICS_ID3:
    case LYRICS_ID3_UNSYNC:
    case LYRICS_ID3_UNSYNC23:
        rc = id3_read_lyrics(fd, &id3->lyrics, &t);
        break;

    case LYRICS_MP4_UTF16:
        t.enc = META_TEXT_UTF16BE;
        /* fall through */
    case LYRICS_MP4_UTF8:
        rc = mp4_read_lyrics(fd, &id3->lyrics, &t);
        break;
    }

    if (rc < 0)
    {
        buf[0] = '\0';
        return -1;
    }
    if (truncated)
        *truncated = t.truncated;
    return t.len;
}

#endif /* HAVE_METADATA_EXTRAS */
