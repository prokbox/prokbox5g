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

/* Embedded lyrics (ID3 USLT, MP4 (c)lyr) for the iPod UI shell.
 *
 * get_metadata() records only where the lyrics are (mp3entry.lyrics: 8
 * bytes); the text, often several KB, is never loaded into the mp3entry or
 * the buffer. The UI reads it on demand, when the Lyrics screen opens, with
 * metadata_read_lyrics() into its own buffer. */

#ifndef _METADATA_LYRICS_H
#define _METADATA_LYRICS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* mp3entry.lyrics.format */
enum lyrics_format {
    LYRICS_NONE = 0,
    LYRICS_ID3,             /* USLT frame body; size = bytes in the file */
    LYRICS_ID3_UNSYNC,      /* ditto, unsynchronised (ID3v2.4) */
    LYRICS_ID3_UNSYNC23,    /* ID3v2.3 tag-wide unsynchronisation: size
                               counts de-unsynchronised bytes */
    LYRICS_MP4_UTF8,        /* (c)lyr data, UTF-8 */
    LYRICS_MP4_UTF16,       /* (c)lyr data, UTF-16BE */
};

struct mp3entry;
struct embedded_lyrics;
struct meta_text;
struct audiobook_chapters;

/* true if the track has embedded lyrics */
bool metadata_has_lyrics(const struct mp3entry *id3);

/* Read the embedded lyrics of 'id3' from fd into buf as UTF-8: NUL
 * terminated, line breaks normalised to '\n' (iTunes uses CR), the ID3
 * language and description dropped. Text that doesn't fit is cut at a
 * character boundary and *truncated (may be NULL) is set. Uses ~320 bytes
 * of stack and no static state (any thread). The file position of fd is
 * not preserved.
 * Returns the length written (excluding the NUL), or -1 if the track has
 * no lyrics or on a read error (buf then holds ""). */
int metadata_read_lyrics(int fd, const struct mp3entry *id3,
                         char *buf, size_t size, bool *truncated);

/* internal: the ID3 backends (id3extras.c) */
int id3_read_lyrics(int fd, const struct embedded_lyrics *l,
                    struct meta_text *t);
bool id3_note_extra_frame(struct mp3entry *entry, const char *header,
                          int version, uint32_t pos, uint32_t size,
                          bool unsync, bool global_unsync);
/* #50: the size of an ID3v2.4 frame whose 4 size bytes are 'sz' and whose
 * body starts at file offset 'body', with 'left' tag bytes from there.
 * The synchsafe reading, unless it lands on something that is not a frame
 * ID ([A-Z0-9]{4}), padding or the tag end while the plain big-endian
 * reading does (ffmpeg writes CTOC sizes unsynchsafed). Peeks at most two
 * 4-byte headers; the file position is restored to 'body'. */
uint32_t id3_v24_frame_size(int fd, uint32_t body, const uint8_t sz[4],
                            uint32_t left);
/* id3_read_chapters() with the CHAP count known (setid3v2title() counted
 * it into mp3entry.chapter_count): the pool's fair share (P6 Rev 1 item
 * 15) is computed over 'expect' entries. expect <= 0: first-come. */
int id3_read_chapters_n(int fd, struct audiobook_chapters *out, int expect);

#endif /* _METADATA_LYRICS_H */
