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

/* Phase 6: the book block, the books.pak reader and the book map
 * (ab_book.h runtime; the page API is in library.c, speed in ab_speed.c).
 *
 * The block is one core_alloc of 11,344 B at shell start (before any
 * playback), pinned for good: the audio and storage threads read the map
 * (checkpoints), so it must never move. Statics here: ~16 B.
 *
 * Reading a book's blob for the map (lib_play() without the page's
 * blob, the boot resume): books.idx header + a binary search of 24 B
 * records (<= 12 small reads, sector-cached by the FAT layer), then the
 * blob streamed through a 512 B stack buffer: its map lands in the block,
 * its entry starts become the tick bitmap, the rest only feeds the sum.
 * No allocation. */

#include <stdbool.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "audio.h"
#include "metadata.h"
#include "playlist.h"
#include "core_alloc.h"
#include "logf.h"
#include "ipodui/anim.h"        /* UPDATE_TRACE */
#include "ipodui/artcache.h"    /* ARTCACHE_DIR, artcache_key_for_id3() */
#include "ipodui/ab_book.h"
#include "ipodui/ab_store.h"
#include "ipodui/library.h"

#define AB_IDX_PATH  ARTCACHE_DIR "/books.idx"
#define AB_IDX_PREV  ARTCACHE_DIR "/books.idx.prev"
#define AB_PAK_PATH  ARTCACHE_DIR "/books.pak"

static struct ab_book_block *blk;       /* pinned, 8-aligned */

/* ---- the block ------------------------------------------------------------ */

void ab_book_init(void)
{
    int h;

    if (blk)
        return;
    h = core_alloc(AB_BOOK_BLOCK_SIZE + 8);
    if (h <= 0)
    {
        UPDATE_TRACE("ab alloc failed bytes=%d", AB_BOOK_BLOCK_SIZE + 8);
        return;
    }
    core_pin(h);                        /* read by the audio thread */
    blk = (struct ab_book_block *)ALIGN_UP((uintptr_t)core_get_data(h), 8);
    memset(blk, 0, sizeof(*blk));
    ab_store_load();
}

struct ab_book_block *ab_book_block(void)
{
    return blk;
}

/* ---- the queue -------------------------------------------------------------- */

bool ab_shell_owns_resume(void)
{
    return lib_queue_is_book(playlist_get_current());
}

static bool map_ok(void)
{
    return blk && blk->map.src != AB_MAP_NONE && blk->map.book_key
           && blk->map.file_count > 0;
}

bool ab_book_playing(void)
{
    return map_ok() && (audio_status() & AUDIO_STATUS_PLAY)
           && lib_queue_is_book(playlist_get_current());
}

/* queue position of the playing entry (insert order: book queues are
 * never shuffled, first_index stays 0) */
static int queue_pos(void)
{
    return playlist_get_display_index() - 1;
}

bool ab_book_checkpoint_id3(struct ab_rec *r, const struct mp3entry *id3,
                            int q, bool stopping, bool ended)
{
    const struct ab_book_map *m;
    uint32_t crc, book_ms;

    if (!map_ok() || !id3 || !lib_queue_is_book(playlist_get_current()))
        return false;
    m = &blk->map;
    if (q < 0 || q >= m->file_count)
        return false;
    crc = m->file_crc[q];
    book_ms = ended ? m->len_ms : ab_book_pos(m, q, id3->elapsed);
    ab_rec_set(r, m->book_key, crc, q, ended ? 0 : id3->elapsed,
               ended ? 0 : id3->offset, book_ms, m->len_ms,
               stopping || ended, blk->store.clock++);
    return true;
}

bool ab_book_checkpoint(struct ab_rec *r, bool stopping)
{
    if (!(audio_status() & AUDIO_STATUS_PLAY))
        return false;
    return ab_book_checkpoint_id3(r, audio_current_track(), queue_pos(),
                                  stopping, false);
}

/* ---- books.idx / books.pak ------------------------------------------------------ */

static bool idx_hdr(int fd, struct ab_idx_hdr *h)
{
    return lseek(fd, 0, SEEK_SET) == 0
           && read(fd, h, sizeof(*h)) == (ssize_t)sizeof(*h)
           && h->magic == AB_IDX_MAGIC && h->version == AB_PAK_VERSION
           && h->rec_size == sizeof(struct ab_idx_rec)
           && h->count <= 0xffff;
}

/* Bounded binary search, not a whole-index validation in a browse callback.
 * The builder validates the full sum before publishing either retained file;
 * the selected record is range-checked and its blob validated at read time. */
static bool pak_lookup(const char *path, ab_key_t key, struct ab_idx_rec *out,
                        uint32_t *gen)
{
    struct ab_idx_hdr h;
    int fd, lo, hi;
    bool found = false;

    if (!key)
        return false;
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    if (idx_hdr(fd, &h))
    {
        lo = 0;
        hi = (int)h.count - 1;
        while (lo <= hi && !found)
        {
            int mid = lo + (hi - lo) / 2;
            off_t off = sizeof(h) + (off_t)mid * sizeof(*out);

            if (lseek(fd, off, SEEK_SET) != off
                || read(fd, out, sizeof(*out)) != (ssize_t)sizeof(*out))
                break;
            if (out->book_key == key)
                found = true;
            else if (out->book_key < key)
                lo = mid + 1;
            else
                hi = mid - 1;
        }
        *gen = h.generation;
    }
    close(fd);
    return found && out->size >= sizeof(struct ab_blob_hdr)
           && out->size <= AB_BLOB_MAX_SIZE && !(out->size & 7)
           && !(out->offset & 7)
           && (uint64_t)out->offset + out->size <= h.pak_size;
}

bool ab_pak_lookup(ab_key_t key, struct ab_idx_rec *out, uint32_t *gen)
{
    return pak_lookup(AB_IDX_PATH, key, out, gen)
           || pak_lookup(AB_IDX_PREV, key, out, gen);
}

static bool idx_path_gen_is(const char *path, uint32_t gen)
{
    struct ab_idx_hdr h;
    int fd = open(path, O_RDONLY);
    bool ok = fd >= 0 && idx_hdr(fd, &h) && h.generation == gen;

    if (fd >= 0)
        close(fd);
    return ok;
}

static bool idx_gen_is(uint32_t gen)
{
    /* Appended packs retain all offsets used by the previous generation. A
     * publication between lookup and read therefore need not blank the book. */
    return idx_path_gen_is(AB_IDX_PATH, gen)
           || idx_path_gen_is(AB_IDX_PREV, gen);
}

/* The whole blob into buf (8-aligned): its size, or 0. One read. */
size_t ab_pak_read(const struct ab_idx_rec *rec, uint32_t gen, void *buf,
                   size_t size, struct ab_blob_view *v)
{
    int fd;
    bool ok;

    if (rec->size > size || ((uintptr_t)buf & 7))
        return 0;
    fd = open(AB_PAK_PATH, O_RDONLY);
    if (fd < 0)
        return 0;
    ok = lseek(fd, rec->offset, SEEK_SET) == (off_t)rec->offset
         && read(fd, buf, rec->size) == (ssize_t)rec->size;
    close(fd);
    if (!ok || !idx_gen_is(gen) || !ab_blob_parse(buf, rec->size, v)
        || v->hdr->book_key != rec->book_key
        || v->hdr->max_mtime != rec->max_mtime
        || v->hdr->file_count != rec->file_count)
        return 0;
    return rec->size;
}

/* ab_word_sum() continued over a chunk starting at word 'w0' */
static uint32_t sum_more(uint32_t s, const void *p, size_t n, size_t w0)
{
    const uint32_t *w = p;
    size_t i;

    for (i = 0; i < n / 4; i++)
        s += w[i] ^ (uint32_t)(w0 + i);
    return s;
}

static void tick_set(uint8_t *bits, uint32_t s, uint32_t len)
{
    uint32_t x;

    if (!len || s == 0 || s >= len)
        return;
    x = (uint32_t)((uint64_t)s * AB_TICK_RES / len);
    if (x > 0 && x < AB_TICK_RES)
        bits[x >> 3] |= 1 << (x & 7);
}

/* Reads n bytes (a multiple of 4) of the pak into the sum; 'cmp' (if not
 * NULL) must equal them, 'dst' (if not NULL) receives them. */
static bool pak_chunk(int fd, void *dst, const void *cmp, size_t n,
                      uint32_t *s, size_t *word, uint8_t *buf)
{
    while (n)
    {
        size_t c = n < 512 ? n : 512;
        uint8_t *p = dst ? (uint8_t *)dst : buf;

        if (read(fd, p, c) != (ssize_t)c)
            return false;
        if (cmp && memcmp(p, cmp, c))
            return false;
        *s = sum_more(*s, p, c, *word);
        *word += c / 4;
        if (dst)
            dst = (uint8_t *)dst + c;
        if (cmp)
            cmp = (const uint8_t *)cmp + c;
        n -= c;
    }
    return true;
}

/* Streams key's blob into the map (starts, first entries, entries,
 * length, ticks). 'verify': the map's file_crc[] (the queue's paths)
 * must equal the blob's, else they are read from the blob. false on a
 * miss, a stale or bad blob, or a CRC mismatch. */
static bool map_from_pak(ab_key_t key, int files, bool verify)
{
    struct ab_book_map *m = &blk->map;
    union { uint8_t b[512]; uint32_t w[128]; } buf;
    struct ab_idx_rec rec;
    struct ab_blob_hdr hdr;
    struct audiobook_chapters th;
    uint32_t gen, s = 0x41425355u;      /* ab_word_sum()'s seed */
    size_t word = 0, left = 0, mapb, firstb;
    int fd, i, n = 0;
    bool ok;

    if (!ab_pak_lookup(key, &rec, &gen) || rec.file_count != files)
        return false;
    fd = open(AB_PAK_PATH, O_RDONLY);
    if (fd < 0)
        return false;
    mapb = AB_BLOB_MAP_SIZE(files);
    firstb = mapb - (size_t)files * 8;  /* file_first + pad, <= 2,048 */
    ok = lseek(fd, rec.offset, SEEK_SET) == (off_t)rec.offset
         && read(fd, &hdr, sizeof(hdr)) == (ssize_t)sizeof(hdr)
         && hdr.book_key == key && hdr.max_mtime == rec.max_mtime
         && hdr.file_count == files
         && rec.size >= sizeof(hdr) + mapb + AB_CH_HDR_SIZE
         && pak_chunk(fd, m->file_start_ms, NULL, files * 4, &s, &word,
                      buf.b)
         && pak_chunk(fd, verify ? NULL : m->file_crc,
                      verify ? m->file_crc : NULL, files * 4, &s, &word,
                      buf.b)
         && pak_chunk(fd, m->file_first, NULL, firstb, &s, &word, buf.b)
         && pak_chunk(fd, &th, NULL, AB_CH_HDR_SIZE, &s, &word, buf.b)
         && th.chapter_count >= 1 && th.chapter_count <= th.cap
         && rec.size == sizeof(hdr) + mapb
                        + AB_CH_SIZE(th.cap, th.pool_size);
    memset(m->ticks, 0, sizeof(m->ticks));
    /* entries (8 B each, start first), then the pool: ticks and the sum */
    if (ok)
        left = rec.size - sizeof(hdr) - mapb - AB_CH_HDR_SIZE;
    while (ok && left)
    {
        size_t c = left < sizeof(buf) ? left : sizeof(buf);

        ok = read(fd, buf.b, c) == (ssize_t)c;
        for (i = 0; ok && i < (int)(c / 8) && n < th.chapter_count; i++, n++)
            if (n > 0)
                tick_set(m->ticks, buf.w[i * 2], hdr.len_ms);
        s = sum_more(s, buf.b, c, word);
        word += c / 4;
        left -= c;
    }
    close(fd);
    if (!ok || s != hdr.sum || !idx_gen_is(gen))
        return false;
    m->entries = th.chapter_count;
    m->len_ms = hdr.len_ms;
    return true;
}

/* ---- the map ---------------------------------------------------------------- */

struct ab_book_map *ab_map_begin(ab_key_t key, int files)
{
    struct ab_book_map *m;

    if (!blk)
        return NULL;
    m = &blk->map;
    m->src = AB_MAP_NONE;
    m->book_key = key;
    m->old_key = 0;
    m->file_count = 0;
    m->entries = 0;
    m->len_ms = 0;
    m->gen++;
    if (!key || files < 1 || files > AB_MAP_FILES_MAX)
    {
        UPDATE_TRACE(AB_TR_MAP, "none", (unsigned long)(uint32_t)key,
                     files, 0);
        return NULL;
    }
    m->file_count = files;
    return m;
}

/* lib_play(), after it filled file_crc[] from the queue's paths: the
 * map from the opened page's blob when it is this book's (no read), else
 * from books.pak (verified against the queue's CRCs). false: the caller
 * fills file_start_ms[] and len_ms from the RAM database and calls
 * ab_map_fallback(). */
bool ab_map_pak(const struct ab_blob_view *page)
{
    struct ab_book_map *m;
    int files, i;
    bool pak = false;

    if (!blk || !blk->map.file_count)
        return false;
    m = &blk->map;
    files = m->file_count;
    if (page && page->hdr && page->hdr->file_count == files
        && (page->hdr->book_key == m->book_key
            || (m->old_key && page->hdr->book_key == m->old_key)))
    {
        for (i = 0; i < files && page->file_crc[i] == m->file_crc[i]; i++)
            ;
        if (i == files)
        {
            memcpy(m->file_start_ms, page->file_start_ms, files * 4);
            memcpy(m->file_first, page->file_first, files * 2);
            m->entries = page->tab->chapter_count;
            m->len_ms = page->hdr->len_ms;
            ab_book_ticks(&ab_ch_start(page->tab, 0),
                          sizeof(struct ab_ch_entry), m->entries,
                          m->len_ms, m->ticks);
            pak = true;
        }
    }
    else
        pak = map_from_pak(m->book_key, files, true)
              || (m->old_key && map_from_pak(m->old_key, files, true));
    UPDATE_TRACE(AB_TR_PAK, pak ? "hit" : "miss",
                 (unsigned long)(uint32_t)m->book_key);
    if (!pak)
        return false;
    m->src = AB_MAP_PAK;
    m->gen++;
    UPDATE_TRACE(AB_TR_MAP, "pak", (unsigned long)(uint32_t)m->book_key,
                 files, m->entries);
    return true;
}

/* Files as entries (§3.3 fallback, 0 file reads) */
void ab_map_fallback(void)
{
    struct ab_book_map *m;
    int i;

    if (!blk || !blk->map.file_count)
        return;
    m = &blk->map;
    for (i = 0; i < m->file_count; i++)
        m->file_first[i] = i;
    m->entries = m->file_count;
    ab_book_ticks(m->file_start_ms, sizeof(uint32_t), m->file_count,
                  m->len_ms, m->ticks);
    m->src = AB_MAP_FALLBACK;
    m->gen++;
    UPDATE_TRACE(AB_TR_MAP, "fallback", (unsigned long)(uint32_t)m->book_key,
                 m->file_count, m->entries);
}

/* The boot resume (lib_queue_resumed()): the playing track identifies
 * the book; only the pak can map it (no file list in RAM). */
void ab_map_resumed(const struct mp3entry *id3, int q, int files)
{
    struct ab_book_map *m;
    ab_key_t key = artcache_key_for_id3(id3);
    ab_key_t old = id3 ? artcache_key_ascii(id3->albumartist, id3->artist,
                                            id3->album, id3->path) : 0;

    m = ab_map_begin(key, files);
    if (!m)
        return;
    m->old_key = old != key ? old : 0;
    /* #47: a pak entry still under the pre-#47 key (ASCII-folded art
     * keys) maps it until the art builder files it under the new one */
    if (q >= 0 && q < files
        && (map_from_pak(key, files, false)
            || (old != key && map_from_pak(old, files, false)))
        && m->file_crc[q] == ab_path_crc(id3->path))
    {
        m->src = AB_MAP_RESUMED;
        m->gen++;
        UPDATE_TRACE(AB_TR_MAP, "resumed", (unsigned long)(uint32_t)key,
                     files, m->entries);
        return;
    }
    m->file_count = 0;
    UPDATE_TRACE(AB_TR_MAP, "none", (unsigned long)(uint32_t)key, files, 0);
}
