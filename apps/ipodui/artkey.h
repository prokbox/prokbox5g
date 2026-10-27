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
 * iPod UI shell: album art cache keys
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
#ifndef _IPODUI_ARTKEY_H_
#define _IPODUI_ARTKEY_H_

#include <stdint.h>
#include <stddef.h>

/* Pure, host-testable key function of the album art cache
 * (docs/design/artcache.md §5). No Rockbox headers, no I/O, no allocation:
 * library.c (the per-album precompute) and the host tests include this;
 * artcache.c never includes library.h.
 *
 * Key string, hashed as it is produced (nothing is buffered):
 *
 *   artist = albumartist, unless it is NULL, empty after norm() or
 *            "<Untagged>" (any case); then artist (same rules; may end up
 *            empty)
 *   tagged album:    norm(artist) "\x1F" norm(album)
 *   untagged album:  "dir" "\x1F" norm(directory of first_track_path),
 *                    the directory without its trailing '/'
 *                    ("/Music/Rips/x.mp3" -> "dir\x1F/music/rips");
 *                    no path -> no key (0)
 *   key = FNV-1a 64 (offset 0xcbf29ce484222325, prime 0x100000001b3) of the
 *         UTF-8 bytes; a hash of 0 becomes 1, so 0 always means "none".
 *
 * norm(): trim leading and trailing whitespace (space, \t, \n, \v, \f, \r),
 * collapse every inner run of it to one space, fold ASCII A-Z to a-z, and
 * (#47) every other well-formed 2- or 3-byte UTF-8 character through
 * ui_casefold_char() (sortkey.h: the library's grouping, so "BJÖRVIK" and
 * "Björvik" share a key; accents kept, sharp s kept). Malformed bytes pass
 * through. The host tool must match it byte for byte: gen_keys.py reads
 * the folding tables from sortkey.c. tests/artcache/keys.txt holds the golden
 * keys; tests/artcache/gen_keys.py is the reference implementation the
 * Mac tool (Phase 7b) must match.
 *
 * Callers pass exactly the strings the album lists group by (the
 * representative track's tags). Podcast shows, grouped by album only, pass
 * NULL for both artists. */

typedef uint64_t artcache_key_t;

#define ARTKEY_SEP      '\x1F'
#define ARTKEY_UNTAGGED "<Untagged>"

artcache_key_t artcache_key(const char *albumartist, const char *artist,
                            const char *album, const char *first_track_path);

/* The key before #47 (norm() folding ASCII only): the book records
 * (ab_store) written under it are moved to the new key when found
 * (library.c book_rec_get()). */
artcache_key_t artcache_key_ascii(const char *albumartist, const char *artist,
                                  const char *album,
                                  const char *first_track_path);

/* Per-chapter art (#39, docs/design/chapter-art.md): the key of chapter
 * 'chapter' (0-based, in metadata_read_chapters() order, i.e. sorted by
 * start) of the book whose cover key is 'book'. FNV-1a 64 (as above) over
 * the 8 bytes of 'book' little-endian, then "\x1F" "ch", then 'chapter'
 * as 4 bytes little-endian; a hash of 0 becomes 1. 'book' 0 or 'chapter'
 * < 0: 0 (none). tests/artcache/chapter_keys.txt holds the goldens,
 * gen_keys.py --chapters the reference. */
artcache_key_t artcache_key_chapter(artcache_key_t book, int chapter);

/* norm() into 'out' (always terminated, truncated to outsize - 1 bytes);
 * returns the full normalized length. For tests and diagnostics. */
size_t artkey_norm(const char *s, char *out, size_t outsize);

/* FNV-1a 32 of a string (source path fingerprints, artcache.md §6). */
uint32_t artkey_fnv32(const char *s);

#endif /* _IPODUI_ARTKEY_H_ */
