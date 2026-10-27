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
#ifdef HAVE_METADATA_EXTRAS
#include "lyrics.h"                 /* id3_read_chapters_n() */
#endif

bool metadata_is_audiobook_ext(const char *path)
{
    const char *ext = path ? strrchr(path, '.') : NULL;
    return ext && strcasecmp(ext + 1, "m4b") == 0;
}

#ifdef HAVE_METADATA_EXTRAS
int metadata_read_chapters(int fd, const struct mp3entry *id3,
                           struct audiobook_chapters *out)
{
    return metadata_read_chapters_buf(fd, id3, out, NULL, 0);
}

int metadata_read_chapters_buf(int fd, const struct mp3entry *id3,
                               struct audiobook_chapters *out,
                               void *scratch, size_t scratch_size)
#else
int metadata_read_chapters(int fd, const struct mp3entry *id3,
                           struct audiobook_chapters *out)
#endif
{
    out->chapter_count = 0;
    out->curr_chapter_idx = 0;
    ab_ch_reset(out);                       /* stock: nothing */

    if (fd < 0 || !id3)
        return -1;

    switch (id3->codectype)
    {
    case AFMT_MP4_AAC:
    case AFMT_MP4_AAC_HE:
    case AFMT_MP4_ALAC:
#ifdef HAVE_METADATA_EXTRAS
        return mp4_read_chapters_buf(fd, out, scratch, scratch_size);
#else
        return mp4_read_chapters(fd, out);
#endif

#ifdef HAVE_METADATA_EXTRAS
    case AFMT_MPA_L1:
    case AFMT_MPA_L2:
    case AFMT_MPA_L3:
        /* the CHAP count is known (setid3v2title()): no counting walk */
        return id3_read_chapters_n(fd, out, id3->chapter_count);
#endif

    default:
        return 0;
    }
}

#ifdef HAVE_METADATA_EXTRAS
#include "rbunicode.h"

int metadata_read_chapter_art(int fd, const struct mp3entry *id3,
                              const struct audiobook_chapters *ab,
                              struct ab_chapter_art *art)
{
    int count = ab ? ab->chapter_count : 0;

    if (fd < 0 || !id3 || (count > 0 && !art))
        return -1;
    if (count <= 0)
        return 0;
    memset(art, 0, count * sizeof(*art));

    switch (id3->codectype)
    {
    case AFMT_MP4_AAC:
    case AFMT_MP4_AAC_HE:
    case AFMT_MP4_ALAC:
        return mp4_read_chapter_art(fd, ab, art);

    case AFMT_MPA_L1:
    case AFMT_MPA_L2:
    case AFMT_MPA_L3:
        return id3_read_chapter_art(fd, ab, art);

    default:
        return 0;
    }
}

/* ---- text decoding ---- */

void meta_text_init(struct meta_text *t, int enc, int flags,
                    char *dst, uint32_t size)
{
    memset(t, 0, sizeof(*t));
    t->dst = dst;
    t->size = size;
    t->enc = enc;
    t->flags = flags;
    t->le = false;
    if (size)
        dst[0] = '\0';
    else
        t->done = true;
}

/* append one code point, or stop when it doesn't fit */
static void meta_text_put(struct meta_text *t, unsigned long cp)
{
    unsigned char buf[6];
    uint32_t n;

    if (t->flags & META_TEXT_NEWLINES)
    {
        bool was_cr = t->prev_cr;
        t->prev_cr = (cp == '\r');
        if (cp == '\r')
            cp = '\n';
        else if (cp == '\n' && was_cr)
            return;                         /* the LF of a CR LF */
    }

    n = utf8encode(cp, buf) - buf;
    if (t->len + n >= t->size)
    {
        t->done = t->truncated = true;
        return;
    }
    memcpy(t->dst + t->len, buf, n);
    t->len += n;
    t->dst[t->len] = '\0';
}

uint32_t meta_text_feed(struct meta_text *t, const uint8_t *src, uint32_t n)
{
    uint32_t i = 0;

    while (i < n && !t->done)
    {
        unsigned long cp;

        if (t->enc == META_TEXT_UTF16 || t->enc == META_TEXT_UTF16BE)
        {
            if (n - i < 2)
                break;                      /* odd byte: wait for more */
            if (!t->started)
            {
                t->started = true;
                if (src[i] == 0xfe && src[i + 1] == 0xff)
                {
                    t->le = false;
                    i += 2;
                    continue;
                }
                if (src[i] == 0xff && src[i + 1] == 0xfe)
                {
                    t->le = true;
                    i += 2;
                    continue;
                }
                /* no BOM: UTF-16BE is big endian by definition; for
                 * "UTF-16" guess from where the zero byte of an ASCII
                 * character is */
                t->le = t->enc == META_TEXT_UTF16
                        && src[i] != 0 && src[i + 1] == 0;
            }
            cp = t->le ? (src[i] | src[i + 1] << 8)
                       : (src[i] << 8 | src[i + 1]);
            i += 2;

            if (cp >= 0xdc00 && cp <= 0xdfff && t->hi)
            {
                cp = 0x10000 + ((t->hi - 0xd800) << 10) + (cp - 0xdc00);
                t->hi = 0;
            }
            else
            {
                if (t->hi)                  /* lone high surrogate */
                {
                    t->hi = 0;
                    meta_text_put(t, 0xfffd);
                    if (t->done)
                        break;
                }
                if (cp >= 0xd800 && cp <= 0xdbff)
                {
                    t->hi = cp;
                    continue;
                }
                if (cp >= 0xdc00 && cp <= 0xdfff)
                    cp = 0xfffd;            /* lone low surrogate */
            }
        }
        else if (t->enc == META_TEXT_UTF8)
        {
            uint8_t c = src[i];
            uint32_t need = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;

            if (!t->started)
            {
                t->started = true;
                if (n - i >= 3 && c == 0xef && src[i + 1] == 0xbb
                    && src[i + 2] == 0xbf)
                {
                    i += 3;                 /* UTF-8 BOM */
                    continue;
                }
            }
            if (need > 1 && n - i < need)
                break;                      /* partial sequence */
            cp = c;
            if (need > 1)
            {
                uint32_t k;
                cp = c & (0x3f >> (need - 1));
                for (k = 1; k < need; k++)
                {
                    if ((src[i + k] & 0xc0) != 0x80)
                        break;
                    cp = cp << 6 | (src[i + k] & 0x3f);
                }
                if (k < need || c >= 0xf8)
                {
                    cp = c;                 /* not UTF-8: take it as Latin-1 */
                    need = 1;
                }
            }
            i += need;
        }
        else                                /* Latin-1 */
            cp = src[i++];

        if (cp == 0 && (t->flags & META_TEXT_STOP_NUL))
        {
            t->done = true;
            break;
        }
        meta_text_put(t, cp);
    }

    return t->done ? n : i;
}

uint32_t meta_text_to_utf8(int enc, int flags, const uint8_t *src,
                           uint32_t n, char *dst, uint32_t size)
{
    struct meta_text t;
    meta_text_init(&t, enc, flags, dst, size);
    meta_text_feed(&t, src, n);
    return t.len;
}
#endif /* HAVE_METADATA_EXTRAS */
