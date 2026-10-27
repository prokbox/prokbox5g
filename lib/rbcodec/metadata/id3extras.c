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

/* ID3v2 extras for the iPod UI shell: chapters (CHAP frames with TIT2
 * titles in any text encoding), per-chapter art (APIC inside CHAP) and the
 * USLT lyrics reader.
 *
 * Like mp4_read_chapters(), this is a separate, on-demand walk over the tag
 * with all state on the stack (well under 512 bytes) and output only into
 * caller-supplied memory, so the tagcache thread and the audio thread can
 * use it at the same time. setid3v2title() only counts CHAP frames and
 * notes where USLT is (id3_note_extra_frame()); nothing here runs while a
 * list is browsed. */

#include <string.h>
#include "platform.h"
#include "metadata.h"
#include "metadata_parsers.h"
#include "chapters.h"
#include "lyrics.h"

#ifdef HAVE_METADATA_EXTRAS

/* ---- byte source with optional de-unsynchronisation ---- */

struct id3_src {
    int fd;
    bool unsync;            /* drop the 0x00 that follows each 0xff */
    bool prev_ff;
    uint32_t pos;           /* file offset of the next raw byte */
    uint32_t dec;           /* bytes delivered so far (after unsync) */
};

/* a frame or sub-frame body: its end, counted in raw file bytes or in
 * delivered (de-unsynchronised) bytes, whichever its size field counts */
struct id3_lim {
    bool raw;
    uint32_t end;
};

static void src_init(struct id3_src *s, int fd, uint32_t pos, bool unsync)
{
    s->fd = fd;
    s->unsync = unsync;
    s->prev_ff = false;
    s->pos = pos;
    s->dec = 0;
}

static uint32_t src_read(struct id3_src *s, void *dst, uint32_t len)
{
    uint8_t *buf = dst;
    uint32_t got = 0;

    while (got < len)
    {
        /* each raw byte yields at most one byte, so this never reads
         * past what the caller asked for */
        ssize_t r = read(s->fd, buf + got, len - got);
        if (r <= 0)
            break;
        s->pos += r;
        if (!s->unsync)
        {
            got += r;
            continue;
        }
        uint32_t o = got, k;
        for (k = got; k < got + (uint32_t)r; k++)
        {
            uint8_t c = buf[k];
            if (s->prev_ff && c == 0)
            {
                s->prev_ff = false;
                continue;
            }
            s->prev_ff = (c == 0xff);
            buf[o++] = c;
        }
        got = o;
    }
    s->dec += got;
    return got;
}

static bool src_skip(struct id3_src *s, uint32_t len)
{
    if (!s->unsync)
    {
        s->pos += len;
        s->dec += len;
        return lseek(s->fd, s->pos, SEEK_SET) == (off_t)s->pos;
    }
    uint8_t tmp[32];
    while (len)
    {
        uint32_t n = len < sizeof(tmp) ? len : sizeof(tmp);
        if (src_read(s, tmp, n) != n)
            return false;
        len -= n;
    }
    return true;
}

static void lim_set(const struct id3_src *s, struct id3_lim *l, bool raw,
                    uint32_t size)
{
    l->raw = raw;
    l->end = (raw ? s->pos : s->dec) + size;
}

static uint32_t lim_left(const struct id3_src *s, const struct id3_lim *l)
{
    uint32_t cur = l->raw ? s->pos : s->dec;
    return cur < l->end ? l->end - cur : 0;
}

/* move to the end of a body */
static bool lim_finish(struct id3_src *s, const struct id3_lim *l)
{
    if (l->raw)
    {
        s->pos = l->end;
        s->prev_ff = false;
        return lseek(s->fd, s->pos, SEEK_SET) == (off_t)s->pos;
    }
    return src_skip(s, lim_left(s, l));
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

static uint32_t synchsafe32(const uint8_t *p)
{
    return (p[0] & 0x7f) << 21 | (p[1] & 0x7f) << 14 | (p[2] & 0x7f) << 7
           | (p[3] & 0x7f);
}

/* ---- #50: ID3v2.4 sizes written unsynchsafed (ffmpeg's CTOC) ---- */

/* true if a frame boundary at 'pos' is plausible: the tag end, padding
 * (a 0 byte) or a frame ID [A-Z0-9]{4} with room for its header */
static bool frame_at(int fd, uint32_t pos, uint32_t end)
{
    uint8_t id[4];
    int k;

    if (pos == end)
        return true;
    if (pos > end || lseek(fd, pos, SEEK_SET) != (off_t)pos
        || read(fd, id, 1) != 1)
        return false;
    if (id[0] == 0)
        return true;                    /* padding */
    if (end - pos < 10 || read(fd, id + 1, 3) != 3)
        return false;
    for (k = 0; k < 4; k++)
        if (!((id[k] >= 'A' && id[k] <= 'Z') || (id[k] >= '0' && id[k] <= '9')))
            return false;
    return true;
}

uint32_t id3_v24_frame_size(int fd, uint32_t body, const uint8_t sz[4],
                            uint32_t left)
{
    uint32_t ss = synchsafe32(sz), be = be32(sz);

    /* equal unless a byte has bit 7 set or the value is >= 0x80: the
     * usual small frame costs nothing */
    if (ss == be)
        return ss;
    if (!(ss <= left && frame_at(fd, body + ss, body + left))
        && be <= left && frame_at(fd, body + be, body + left))
        ss = be;
    lseek(fd, body, SEEK_SET);
    return ss;
}

/* ---- tag and frame walking ---- */

struct id3_walk {
    struct id3_src s;
    struct id3_lim tag;
    uint8_t version;        /* 3 or 4 */
    bool global_unsync;
};

/* parse the tag header at the start of the file; false if there is no
 * ID3v2.3/2.4 tag (v2.2 has no CHAP frames) */
static bool walk_open(int fd, struct id3_walk *w)
{
    uint8_t h[10];

    src_init(&w->s, fd, 0, false);
    if (lseek(fd, 0, SEEK_SET) != 0 || src_read(&w->s, h, 10) != 10
        || memcmp(h, "ID3", 3) || (h[3] != 3 && h[3] != 4))
        return false;

    w->version = h[3];
    w->global_unsync = (h[5] & 0x80) != 0;
    w->tag.raw = true;
    w->tag.end = 10 + synchsafe32(h + 6);

    if (h[5] & 0x40)                    /* extended header */
    {
        uint8_t e[4];
        if (src_read(&w->s, e, 4) != 4)
            return false;
        uint32_t skip = w->version == 3 ? be32(e) : synchsafe32(e) - 4;
        if (!src_skip(&w->s, skip))
            return false;
    }
    /* ID3v2.3 unsynchronises the whole tag after the header, and frame
     * sizes count the de-unsynchronised bytes; ID3v2.4 does it per frame
     * and frame sizes count the stored bytes */
    w->s.unsync = w->global_unsync && w->version == 3;
    return true;
}

/* read a (sub-)frame header; *body gets the body's extent. false at the
 * end of the tag (padding, short read) */
static bool walk_frame(struct id3_walk *w, const struct id3_lim *outer,
                       char id[4], uint16_t *flags, struct id3_lim *body)
{
    uint8_t h[10];

    if (lim_left(&w->s, outer) < 10 || src_read(&w->s, h, 10) != 10
        || h[0] == 0)
        return false;
    memcpy(id, h, 4);
    *flags = h[8] << 8 | h[9];
    uint32_t size = w->version == 4 ? synchsafe32(h + 4) : be32(h + 4);
    /* #50: a v2.4 size may be plain big-endian (ffmpeg's CTOC); only
     * where raw file offsets apply (not inside an unsynchronised body) */
    if (w->version == 4 && !w->s.unsync && outer->raw
        && outer->end >= w->s.pos)
        size = id3_v24_frame_size(w->s.fd, w->s.pos, h + 4,
                                  outer->end - w->s.pos);
    /* inside an unsynchronised body sizes count delivered bytes */
    lim_set(&w->s, body, !w->s.unsync, size);
    if (body->raw && outer->raw && body->end > outer->end)
        body->end = outer->end;
    return true;
}

/* apply a frame's format flags at the start of its body. Returns false if
 * the body can't be read (compressed or encrypted); sets *unsync */
static bool walk_body(struct id3_walk *w, uint16_t flags, bool *unsync)
{
    uint8_t skip = 0;

    *unsync = w->s.unsync;
    if (w->version == 4)
    {
        if (flags & 0x000c)             /* compression, encryption */
            return false;
        if (flags & 0x0040)             /* grouping identity */
            skip += 1;
        if (flags & 0x0001)             /* data length indicator */
            skip += 4;
        if ((flags & 0x0002) || w->global_unsync)
            *unsync = true;
    }
    else
    {
        if (flags & 0x00c0)             /* compression, encryption */
            return false;
        if (flags & 0x0020)             /* grouping identity */
            skip += 1;
    }
    return src_skip(&w->s, skip);
}

/* ---- CHAP ---- */

#define TITLE_RAW_MAX  136  /* enough raw text for a 63-byte UTF-8 title */
#define APIC_HEAD_MAX   72  /* mime, picture type, description, magic */

static int art_type_from_magic(const uint8_t *p, uint32_t n)
{
    if (n >= 3 && p[0] == 0xff && p[1] == 0xd8 && p[2] == 0xff)
        return AB_ART_JPEG;
    if (n >= 8 && !memcmp(p, "\x89PNG\r\n\x1a\n", 8))
        return AB_ART_PNG;
    return AB_ART_NONE;
}

/* APIC sub-frame body: encoding, MIME (Latin-1, NUL), picture type,
 * description (encoding-terminated), image data */
static void chap_apic(struct id3_walk *w, uint32_t body_size,
                      struct ab_chapter_art *art)
{
    uint8_t b[APIC_HEAD_MAX];
    uint32_t start = w->s.pos;
    uint32_t n = body_size < sizeof(b) ? body_size : sizeof(b);
    uint32_t i;

    if (src_read(&w->s, b, n) != n || n < 4)
        return;
    uint8_t enc = b[0];
    for (i = 1; i < n && b[i]; i++)     /* MIME */
        ;
    i += 2;                             /* NUL, picture type */
    if (enc == META_TEXT_UTF16 || enc == META_TEXT_UTF16BE)
    {
        while (i + 1 < n && (b[i] || b[i + 1]))
            i += 2;
        i += 2;
    }
    else
    {
        while (i < n && b[i])
            i++;
        i += 1;
    }
    if (i >= n || i >= body_size)
        return;                         /* description too long: no art */

    int type = art_type_from_magic(b + i, n - i);
    uint32_t size = body_size - i;
    if (type == AB_ART_NONE || size >= (1u << 24))
        return;
    art->pos = start + i;
    art->size = size;
    art->type = type;
}

/* Walk every CHAP frame. With neither 'out' nor 'art', only count them
 * (ab_ch_expect() clamps the count to the table). Without 'art', fill
 * 'out' in file order;
 * with 'art', match each CHAP that has an image to the one of the 'nchaps'
 * chapters with the same start time. Returns the number of CHAP frames
 * used. */
static int chap_walk(int fd, struct audiobook_chapters *out,
                     const struct audiobook_chapters *chaps,
                     struct ab_chapter_art *art)
{
    struct id3_walk w;
    struct id3_lim frame;
    char id[4];
    uint16_t flags;
    int count = 0;

    if (!walk_open(fd, &w))
        return 0;

    while (walk_frame(&w, &w.tag, id, &flags, &frame))
    {
        bool unsync_outer = w.s.unsync, unsync;

        if (memcmp(id, "CHAP", 4) || !walk_body(&w, flags, &unsync))
            goto next;
        if (!out && !art)
        {
            count++;                    /* count only */
            goto next;
        }
        w.s.unsync = unsync;

        /* element ID (NUL terminated), start, end, start/end offsets */
        uint8_t b[TITLE_RAW_MAX];
        uint32_t i = 0;
        do {
            if (lim_left(&w.s, &frame) == 0 || src_read(&w.s, b, 1) != 1)
                goto next;
        } while (b[0] && ++i < 256);
        if (b[0] || lim_left(&w.s, &frame) < 16
            || src_read(&w.s, b, 16) != 16)
            goto next;

        unsigned long start_ms = be32(b);
        char title[AB_MAX_CHAPTER_NAME];
        struct ab_chapter_art found = { 0, 0, AB_ART_NONE };
        title[0] = '\0';

        /* sub-frames */
        struct id3_lim sub;
        char sid[4];
        uint16_t sflags;
        while (walk_frame(&w, &frame, sid, &sflags, &sub))
        {
            bool sunsync, saved = w.s.unsync;
            if (walk_body(&w, sflags, &sunsync))
            {
                w.s.unsync = sunsync;
                uint32_t left = lim_left(&w.s, &sub);
                if (!art && !memcmp(sid, "TIT2", 4) && left > 1)
                {
                    uint32_t n = left < sizeof(b) ? left : sizeof(b);
                    n = src_read(&w.s, b, n);
                    if (n > 1)
                        meta_text_to_utf8(b[0], META_TEXT_STOP_NUL, b + 1,
                                          n - 1, title, sizeof(title));
                }
                else if (art && !memcmp(sid, "APIC", 4) && !sunsync
                         && found.size == 0)
                    chap_apic(&w, left, &found);
                w.s.unsync = saved;
            }
            if (!lim_finish(&w.s, &sub))
                break;
        }

        if (!art)
        {
            if (count < ab_ch_cap(out))
            {
                ab_ch_put(out, count, start_ms, title, strlen(title));
                count++;
            }
        }
        else
        {
            int j;
            count++;
            for (j = 0; found.size && j < chaps->chapter_count; j++)
            {
                if (ab_ch_start(chaps, j) == start_ms
                    && art[j].size == 0)
                {
                    art[j] = found;
                    break;
                }
            }
        }
next:
        w.s.unsync = unsync_outer;
        if (!lim_finish(&w.s, &frame))
            break;
    }
    return count;
}

int id3_read_chapters_n(int fd, struct audiobook_chapters *out, int expect)
{
    int n;

    ab_ch_reset(out);
    /* the CHAP count setid3v2title() made (P6 D1): each title gets a fair
     * share of the pool, cut on a UTF-8 boundary (Rev 1 item 15) */
    ab_ch_expect(out, expect);
    n = chap_walk(fd, out, NULL, NULL);

    /* CHAP frames may be stored in any order: stable insertion sort by
     * start time, once per track. O(n) for the usual sorted tags; at
     * worst (reversed, 1,024 entries of 8 B) ~4.2 MB of memmove. */
    out->chapter_count = n;
    ab_ch_sort(out);
    return n;
}

/* without a count (the file is walked once more to count the CHAPs) */
int id3_read_chapters(int fd, struct audiobook_chapters *out)
{
    return id3_read_chapters_n(fd, out, chap_walk(fd, NULL, NULL, NULL));
}

int id3_read_chapter_art(int fd, const struct audiobook_chapters *ab,
                         struct ab_chapter_art *art)
{
    int i, found = 0;
    chap_walk(fd, NULL, ab, art);
    for (i = 0; i < ab->chapter_count; i++)
        found += art[i].size != 0;
    return found;
}

/* ---- called from setid3v2title() for frames it doesn't keep ---- */

bool id3_note_extra_frame(struct mp3entry *entry, const char *header,
                          int version, uint32_t pos, uint32_t size,
                          bool unsync, bool global_unsync)
{
    if (version < ID3_VER_2_3)
        return false;
    if (!memcmp(header, "CHAP", 4))
    {
        if (entry->chapter_count < AB_MAX_CHAPTERS)
            entry->chapter_count++;
        return true;
    }
    if (!memcmp(header, "USLT", 4) && entry->lyrics.size == 0
        && size > 4 && size < (1u << 24))
    {
        entry->lyrics.pos = pos;
        entry->lyrics.size = size;
        entry->lyrics.format =
            (global_unsync && version == ID3_VER_2_3) ? LYRICS_ID3_UNSYNC23
            : (unsync || global_unsync) ? LYRICS_ID3_UNSYNC
            : LYRICS_ID3;
        return true;
    }
    return false;
}

/* ---- USLT: encoding, language, description, text ---- */

int id3_read_lyrics(int fd, const struct embedded_lyrics *l,
                    struct meta_text *t)
{
    struct id3_src s;
    struct id3_lim body;
    uint8_t b[64];
    uint32_t carry = 0;

    src_init(&s, fd, l->pos, l->format != LYRICS_ID3);
    if (lseek(fd, l->pos, SEEK_SET) != (off_t)l->pos)
        return -1;
    lim_set(&s, &body, l->format != LYRICS_ID3_UNSYNC23, l->size);

    if (src_read(&s, b, 4) != 4)
        return -1;
    uint8_t enc = b[0];
    bool wide = (enc == META_TEXT_UTF16 || enc == META_TEXT_UTF16BE);

    /* content descriptor */
    for (;;)
    {
        if (lim_left(&s, &body) < (wide ? 2u : 1u)
            || src_read(&s, b, wide ? 2 : 1) != (wide ? 2u : 1u))
            return 0;
        if (b[0] == 0 && (!wide || b[1] == 0))
            break;
    }

    meta_text_init(t, enc, META_TEXT_NEWLINES | META_TEXT_STOP_NUL,
                   t->dst, t->size);
    while (!t->done)
    {
        uint32_t left = lim_left(&s, &body);
        uint32_t want = sizeof(b) - carry;
        if (want > left)
            want = left;
        uint32_t got = want ? src_read(&s, b + carry, want) : 0;
        uint32_t have = carry + got;
        if (have == 0)
            break;
        uint32_t used = meta_text_feed(t, b, have);
        carry = have - used;
        if (carry)
        {
            if (got == 0)
                break;                  /* trailing odd byte */
            memmove(b, b + used, carry);
        }
    }
    return t->len;
}

#endif /* HAVE_METADATA_EXTRAS */
