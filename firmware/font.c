/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (c) 2002 by Greg Haerr <greg@censoft.com>
 * Modified for ProkBox5g (2026); see docs/changes-from-rockbox.md.
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
 * Rockbox startup font initialization
 * This file specifies which fonts get compiled-in and
 * loaded at startup, as well as their mapping into
 * the FONT_SYSFIXED, FONT_UI and FONT_MP3 ids.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>

#include "config.h"
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "string-extra.h"
#include "font.h"
#include "file.h"
#include "core_alloc.h"
#include "debug.h"
#include "panic.h"
#include "rbunicode.h"
#include "diacritic.h"
#include "rbpaths.h"

/* Define LOGF_ENABLE to enable logf output in this file */
//#define LOGF_ENABLE
#include "logf.h"

#define MAX_FONTSIZE_FOR_16_BIT_OFFSETS 0xFFDB

#define FONT_EXT "fnt"
#define GLYPH_CACHE_EXT "gc"

#ifdef UNICODE32
#define FC_HEADER_VAL 0x01000020
#else
#define FC_HEADER_VAL 0x01000010
#endif

/* max static loadable font buffer size */
#ifndef MAX_FONT_SIZE
#if LCD_HEIGHT > 64
#if MEMORYSIZE > 2
#define MAX_FONT_SIZE   60000
#else
#define MAX_FONT_SIZE   10000
#endif
#else
#define MAX_FONT_SIZE   4000
#endif
#endif
#define GLYPHS_TO_CACHE 256

#if MEMORYSIZE < 4
#define FONT_HARD_LIMIT
#endif

#ifndef FONT_HEADER_SIZE
#define FONT_HEADER_SIZE 36
#endif

#ifndef BOOTLOADER
/* Font cache includes */
#include "font_cache.h"
#include "lru.h"
#endif

#ifndef O_BINARY
#define O_BINARY 0
#endif

#ifdef HAVE_IPOD_UI
#include "devstats.h"           /* DEVSTATS_FONT_*: empty without it */
#endif
#if defined(HAVE_IPOD_UI) && !defined(BOOTLOADER)
#include "font_shell_map.h"
#include "font_sparse.h"
/* The shell fonts (apps/ipodui/fonts) cover U+0020..U+024F, plus the
 * characters of font_shell_slot[] (quotes, dashes, music notes, euro, ...)
 * drawn in the C1 control slots U+0080..U+009F; see font_shell_map.h.
 * Map a code point onto the slot or alias that draws it. Text below
 * U+0080 and the Latin range pass with two compares. A code point no
 * table knows is returned as is and drawn as the default character.
 * Only a font that holds the slots and ends by FONT_SHELL_LAST is
 * remapped (Heros' last glyph is U+0237, so the shell fonts end there);
 * a font that reaches further has its own glyphs and is left alone. */
static ucschar_t font_remap(const struct font *pf, ucschar_t c)
{
    int lo, hi, i;

    if (c < 0x80 || pf->firstchar > 0x80
        || pf->firstchar + pf->size > FONT_SHELL_LAST + 1)
        return c;
    if (c <= 0x9F) {
        /* a raw C1 code point: Windows-1252 text decoded as Latin-1 */
        c = font_shell_cp1252[c - 0x80];
        if (!c)
            return 0xFFFF;      /* out of range: the default character */
    }
    if (c <= FONT_SHELL_LAST)
        return c;
    lo = 0;
    hi = (int)(sizeof(font_shell_alias) / sizeof(font_shell_alias[0])) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (font_shell_alias[mid][0] == c) {
            c = font_shell_alias[mid][1];
            if (c <= FONT_SHELL_LAST && (c < 0x80 || c > 0x9F))
                return c;
            break;
        }
        if (font_shell_alias[mid][0] < c)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    for (i = 0; i < 32; i++)
        if (font_shell_slot[i] == c)
            return 0x80 + i;
    return c;
}
#else
#define font_remap(pf, c) (c)
#endif

/* Define this to try loading /.rockbox/.glyphcache    *
 * when a font specific file fails. This requires the  *
 * user to copy and rename a font glyph cache file     */
//#define TRY_DEFAULT_GLYPHCACHE

/* compiled-in font */
extern struct font sysfont;

#if !defined(BOOTLOADER) || defined(SONY_NWZ_LINUX) || defined(HIBY_LINUX) || defined(FIIO_M3K_LINUX)

struct buflib_alloc_data {
    struct font font;    /* must be the first member! */
    char *path; /* font path and filename (allocd at end of buffer) */
    size_t path_bufsz; /* size of path buffer */
    int refcount;        /* how many times has this font been loaded? */
#ifdef HAVE_IPOD_UI
    /* ProkBox (#235): the sparse form (font_sparse.h): the shared index
     * set it draws from (-1: a stock font), its record stride and id.
     * Here, not in struct font: sysfont's generated initializer is
     * positional. Reached from a loaded font's pf (font is first). */
    signed char sparse_set;
    unsigned short sparse_stride;
    uint32_t sparse_id;
    /* jaglyph: the pinned records (font_sparse_pin()): a movable block,
     * pin_n code points (uint16, ascending, padded to 4 bytes) then their
     * records; -1 / 0 when none */
    int pin_handle;
    unsigned short pin_n;
#endif
    unsigned char buffer[];
};
#ifdef HAVE_IPOD_UI
#define SPD(pf) ((struct buflib_alloc_data *)(pf))
#endif
static int buflib_allocations[MAXFONTS];
static int ui_font = FONT_LASTUSERFONT;

static int cache_fd;
static struct font* cache_pf;

static void glyph_cache_save(int font_id);

static int buflibmove_callback(int handle, void* current, void* new)
{
    (void)handle;
    struct buflib_alloc_data *alloc = (struct buflib_alloc_data*)current;
    ptrdiff_t diff = new - current;

    logf("%s %s", __func__, alloc->path);
#define UPDATE(x) if (x) { x = PTR_ADD(x, diff); }

    UPDATE(alloc->font.bits);
    UPDATE(alloc->font.offset);
    UPDATE(alloc->font.width);

    UPDATE(alloc->font.buffer_start);
    UPDATE(alloc->font.buffer_end);
    UPDATE(alloc->font.buffer_position);
    UPDATE(alloc->path);

    UPDATE(alloc->font.cache._index);
    UPDATE(alloc->font.cache._lru._base);
    return BUFLIB_CB_OK;
}
static void lock_font_handle(int handle, bool lock)
{
    if ( handle < 0 )
        return;

    if (lock)
        core_pin(handle);
    else
        core_unpin(handle);
}

void font_lock(int font_id, bool lock)
{
    if( font_id < 0 || font_id >= MAXFONTS )
        return;
    if( buflib_allocations[font_id] > 0 )
        lock_font_handle(buflib_allocations[font_id], lock);
}

static struct buflib_callbacks buflibops = {buflibmove_callback, NULL, NULL };

static inline unsigned char *buffer_from_handle(int handle)
{
    struct buflib_alloc_data *alloc = core_get_data(handle);
    unsigned char* buffer = alloc->buffer;
    return buffer;
}

/* Font cache structures */
static void cache_create(struct font* pf);
static NO_INLINE void glyph_cache_load(const char *font_path, struct font *pf);
static int glyph_bytes( struct font *pf, int width );

#ifdef HAVE_IPOD_UI
/* ProkBox (#235): the sparse form's resident index (font_sparse.h: the
 * presence bitmap and the counts, FONT_SPARSE_INDEX bytes), one movable
 * block per distinct glyph set, shared by every font of that set (the
 * three ShellCJK sizes hold one set: one block, not three). Nothing keeps
 * a pointer into it across a yield. */
#define SPARSE_SETS 2
static int sparse_handle[SPARSE_SETS];
static int sparse_refs[SPARSE_SETS];
static int fill_font = -1;              /* font_set_fill() */
static font_fill_fn fill_fn;

static void sparse_init(void)
{
    for (int i = 0; i < SPARSE_SETS; i++)
    {
        sparse_handle[i] = -1;
        sparse_refs[i] = 0;
    }
}

static const uint8_t *sparse_map(const struct font *pf)
{
    return core_get_data(sparse_handle[(int)SPD(pf)->sparse_set]);
}

/* whether set s holds the index at the file's FONT_SPARSE_MAPOFF:
 * compared in pieces, no second copy */
static bool sparse_same(int fd, int s)
{
    unsigned char buf[256];
    size_t off;

    if (lseek(fd, FONT_SPARSE_MAPOFF, SEEK_SET) != FONT_SPARSE_MAPOFF)
        return false;
    for (off = 0; off < FONT_SPARSE_INDEX; off += sizeof(buf))
    {
        size_t n = MIN(sizeof(buf), FONT_SPARSE_INDEX - off);
        if (read(fd, buf, n) != (ssize_t)n
            || memcmp(buf, (const char *)core_get_data(sparse_handle[s])
                           + off, n))
            return false;
    }
    return true;
}

/* the sparse font's sub-header and its index set; false: refuse it */
static bool sparse_attach(struct font *pf, int fd)
{
    struct font_sparse_sub sub;
    int s, free_s = -1;

    SPD(pf)->sparse_set = -1;
    if (lseek(fd, FONT_SPARSE_SUBOFF, SEEK_SET) != FONT_SPARSE_SUBOFF
        || read(fd, &sub, sizeof(sub)) != sizeof(sub)
        || sub.magic != FONT_SPARSE_MAGIC
        || sub.stride != 1u + glyph_bytes(pf, pf->maxwidth)
        || pf->firstchar != FONT_SPARSE_FIRST
        || pf->size != FONT_SPARSE_SIZE
        || (uint32_t)pf->bits_size != sub.count * sub.stride)
    {
        logf("font: sparse header refused");
        return false;
    }
    for (s = 0; s < SPARSE_SETS; s++)
    {
        if (sparse_handle[s] <= 0)
        {
            if (free_s < 0)
                free_s = s;
        }
        else if (sparse_same(fd, s))
            break;
    }
    if (s == SPARSE_SETS)
    {
        unsigned char *map;
        bool ok;

        if (free_s < 0)
            return false;
        s = free_s;
        sparse_handle[s] = core_alloc_ex(FONT_SPARSE_INDEX, NULL);
        if (sparse_handle[s] <= 0)
        {
            sparse_handle[s] = -1;
            return false;
        }
        /* pinned for its life, like the shell fonts (theme.c): the COP
         * may draw from it while a compaction on the CPU runs */
        map = core_get_data_pinned(sparse_handle[s]);
        ok = lseek(fd, FONT_SPARSE_MAPOFF, SEEK_SET) == FONT_SPARSE_MAPOFF
             && read(fd, map, FONT_SPARSE_INDEX) == FONT_SPARSE_INDEX;
        if (!ok)
        {
            core_free(sparse_handle[s]);
            sparse_handle[s] = -1;
            return false;
        }
    }
    sparse_refs[s]++;
    SPD(pf)->sparse_set = s;
    SPD(pf)->sparse_stride = sub.stride;
    SPD(pf)->sparse_id = sub.font_id;
    return true;
}

static void sparse_detach(struct font *pf)
{
    int s = SPD(pf)->sparse_set;

    if (s < 0)
        return;
    SPD(pf)->sparse_set = -1;
    if (--sparse_refs[s] <= 0 && sparse_handle[s] > 0)
    {
        core_free(sparse_handle[s]);
        sparse_handle[s] = -1;
        sparse_refs[s] = 0;
    }
}

/* jaglyph: the pinned records (font_sparse_pin()) */
#define PIN_CPS_BYTES(n) ALIGN_UP((size_t)(n) * sizeof(uint16_t), 4)

static void sparse_pin_free(struct buflib_alloc_data *pd)
{
    if (pd->pin_handle > 0)
        core_free(pd->pin_handle);  /* pinned for its life, freed so */
    pd->pin_handle = -1;
    pd->pin_n = 0;
}

/* a miss of code point c: its pinned record into dst (the cache entry's
 * width byte and cell), true when pf holds it pinned. No yield. */
static bool sparse_pin_get(struct font *pf, ucschar_t c, unsigned char *dst)
{
    const struct buflib_alloc_data *pd = SPD(pf);
    const uint16_t *cps;
    int lo = 0, hi = pd->pin_n - 1;

    if (pd->pin_n == 0 || c > 0xffff)
        return false;
    cps = core_get_data(pd->pin_handle);
    while (lo <= hi)
    {
        int mid = (lo + hi) >> 1;
        if (cps[mid] == c)
        {
            memcpy(dst, (const unsigned char *)cps + PIN_CPS_BYTES(pd->pin_n)
                        + (size_t)mid * pd->sparse_stride, pd->sparse_stride);
            return true;
        }
        if (cps[mid] < c)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return false;
}
#endif /* HAVE_IPOD_UI */
/* End Font cache structures */

void font_init(void)
{
    int i = 0;
    cache_fd = -1;
    while (i<MAXFONTS)
        buflib_allocations[i++] = -1;
#ifdef HAVE_IPOD_UI
    sparse_init();
#endif
}

/* Check if we have x bytes left in the file buffer */
#define HAVEBYTES(x) (pf->buffer_position + (x) <= pf->buffer_end)

/* Helper functions to read big-endian unaligned short or long from
   the file buffer.  Bounds-checking must be done in the calling
   function.
 */

static short readshort(struct font *pf)
{
    uint16_t s;

    s = *pf->buffer_position++ & 0xff;
    s |= (*pf->buffer_position++ << 8);
    return s;
}

static int32_t readlong(struct font *pf)
{
    uint32_t l;

    l = *pf->buffer_position++ & 0xff;
    l |= *pf->buffer_position++ << 8;
    l |= ((uint32_t)(*pf->buffer_position++)) << 16;
    l |= ((uint32_t)(*pf->buffer_position++)) << 24;
    return l;
}

static int glyph_bytes( struct font *pf, int width )
{
    int ret;
    if (pf->depth)
        ret = ( pf->height * width + 1 ) / 2;
    else
        ret = width * ((pf->height + 7) / 8);
    return (ret + 1) & ~1;
}

/* Load memory font */
static struct font* font_load_in_memory(struct font* pf,
                                        int32_t nwidth,
                                        int32_t noffset )
{
    int i;
    /* variable font data*/
    pf->buffer_position = pf->buffer_start + 36;
    pf->bits = (unsigned char *)pf->buffer_position;
    pf->buffer_position += pf->bits_size*sizeof(unsigned char);

    if (pf->bits_size < MAX_FONTSIZE_FOR_16_BIT_OFFSETS)
    {
        /* pad to 16-bit boundary */
        pf->buffer_position = (unsigned char *)(((intptr_t)pf->buffer_position + 1) & ~1);
    }
    else
    {
        /* pad to 32-bit boundary*/
        pf->buffer_position = (unsigned char *)(((intptr_t)pf->buffer_position + 3) & ~3);
    }

    if (noffset)
    {
        if (pf->bits_size < MAX_FONTSIZE_FOR_16_BIT_OFFSETS)
        {
            pf->long_offset = 0;
            pf->offset = (uint16_t*)pf->buffer_position;

            /* Check we have sufficient buffer */
            if (!HAVEBYTES(noffset * sizeof(uint16_t)))
                return NULL;

            for (i=0; i<noffset; ++i)
            {
                ((uint16_t*)(pf->offset))[i] = (uint16_t)readshort(pf);
            }
        }
        else
        {
            pf->long_offset = 1;
            pf->offset = (uint16_t*)pf->buffer_position;

            /* Check we have sufficient buffer */
            if (!HAVEBYTES(noffset * sizeof(int32_t)))
                return NULL;

            for (i=0; i<noffset; ++i)
            {
                ((uint32_t*)(pf->offset))[i] = (uint32_t)readlong(pf);
            }
        }
    }
    else
        pf->offset = NULL;

    if (nwidth) {
        pf->width = (unsigned char *)pf->buffer_position;
        pf->buffer_position += nwidth*sizeof(unsigned char);
    }
    else
        pf->width = NULL;

    if (pf->buffer_position > pf->buffer_end)
        return NULL;

    return pf;    /* success!*/
}

/* Load cached font */
static struct font* font_load_cached(struct font* pf,
                                     int32_t nwidth,
                                     int32_t noffset)
{
    /* We are now at the bitmap data, this is fixed at 36.. */
    pf->width = NULL;
    pf->bits = NULL;

    /* Calculate offset to offset data */
    pf->buffer_position += pf->bits_size * sizeof(unsigned char);

    if (pf->bits_size < MAX_FONTSIZE_FOR_16_BIT_OFFSETS)
    {
        pf->long_offset = 0;
        /* pad to 16-bit boundary */
        pf->buffer_position = (unsigned char *)(((intptr_t)pf->buffer_position + 1) & ~1);
    }
    else
    {
        pf->long_offset = 1;
        /* pad to 32-bit boundary*/
        pf->buffer_position = (unsigned char *)(((intptr_t)pf->buffer_position + 3) & ~3);
    }

    if (noffset)
        pf->file_offset_offset = (uint32_t)(pf->buffer_position - pf->buffer_start);
    else
        pf->file_offset_offset = 0;

    /* Calculate offset to widths data */
    if (pf->bits_size < MAX_FONTSIZE_FOR_16_BIT_OFFSETS)
        pf->buffer_position += noffset * sizeof(uint16_t);
    else
        pf->buffer_position += noffset * sizeof(uint32_t);

    if (nwidth)
        pf->file_width_offset = (uint32_t)(pf->buffer_position - pf->buffer_start);
    else
        pf->file_width_offset = 0;

    /* Create the cache */
    cache_create(pf);

    return pf;
}

bool font_filename_matches_loaded_id(int font_id, const char *filename)
{
    if ( font_id >= 0 && font_id < MAXFONTS )
    {
        int handle = buflib_allocations[font_id];
        if (handle > 0)
        {
            struct buflib_alloc_data *data = core_get_data(handle);
            logf("%s id: [%d], %s", __func__, font_id, data->path);
            return strcmp(data->path, filename) == 0;
        }
    }
    return false;
}

static int find_font_index(const char* path)
{
    for(int index = 0; index < MAXFONTS; index++)
    {
        if(font_filename_matches_loaded_id(index, path))
        {
            logf("%s Found id: [%d], %s", __func__, index, path);
            return index;
        }
    }
    logf("%s %s Not found using id: [%d], FONT_SYSFIXED",
         __func__, path, FONT_SYSFIXED);
    return FONT_SYSFIXED;
}

static size_t font_glyphs_to_bufsize(struct font *pf, int glyphs)
{
    size_t bufsize;

    /* LRU bytes per glyph */
    bufsize = LRU_SLOT_OVERHEAD + sizeof(struct font_cache_entry) +
        sizeof(unsigned short);
    /* Image bytes per glyph */
    bufsize += glyph_bytes(pf, pf->maxwidth);
    bufsize *= glyphs;

    return bufsize;
}

static struct font* font_load_header(int fd, struct font *pheader,
                                     struct font *pf,
                                     uint32_t *nwidth, uint32_t *noffset)
{
    /* Load the header. Readshort() and readlong()              *
     * update buffer_position address as they read              */
    pheader->buffer_start = pheader->buffer_position = (char *)pheader;
    pheader->buffer_size = FONT_HEADER_SIZE;
    pheader->buffer_end = pheader->buffer_start + pheader->buffer_size;

    if (read(fd, pheader, FONT_HEADER_SIZE) != FONT_HEADER_SIZE)
        return NULL;

    /* read magic and version #*/
    if (memcmp(pheader->buffer_position, VERSION, 4) != 0)
        return NULL;

    pheader->buffer_position += 4;

    /* font info*/
    pf->maxwidth      = readshort(pheader);
    pf->height        = readshort(pheader);
    pf->ascent        = readshort(pheader);
    pf->depth         = readshort(pheader);
    pf->firstchar     = readlong(pheader);
    pf->defaultchar   = readlong(pheader);
    pf->size          = readlong(pheader);

    /* get variable font data sizes*/
    /* # words of bitmap_t*/
    pf->bits_size     = readlong(pheader);
    *noffset          = readlong(pheader);
    *nwidth           = readlong(pheader);

    return pf;
}

/* load a font with room for glyphs, limited to bufsize if not zero */
int font_load_ex( const char *path, size_t buf_size, int glyphs )
{
    /* needed to handle the font properly after it's loaded */
    size_t path_len = strlen(path);
    if ( path_len >= MAX_PATH )
        return -1;

    //printf("\nfont_load_ex(%s, %d, %d)\n", path, buf_size, glyphs);
    int fd = open(path, O_RDONLY|O_BINARY);
    if ( fd < 0 )
        return -1;

#ifdef UNICODE32
    if (glyphs && glyphs < 3)
        glyphs = 3; /* Guarantee we'll always have at least 2 after alignment */
#else
    if (glyphs && glyphs < 2)
        glyphs = 2; /* Guarantee we'll always have at least 1 after alignment */
#endif

    /* load font struct f with file header */
    int file_size = ffilesize( fd );
    struct font header;
    struct font f;

    uint32_t nwidth, noffset;
    if ( !font_load_header( fd, &header, &f, &nwidth, &noffset )
#if LCD_DEPTH < 16
        || f.depth
#endif
    )
    {
        close(fd);
        return -1;
    }

    /* examine f and calc buffer size */
    bool cached = false;
    size_t bufsize = buf_size;
#ifdef HAVE_IPOD_UI
    /* ProkBox (#235): the sparse form is always cached, with a glyph LRU */
    bool sparse = (f.depth & FONT_SPARSE_DEPTH) != 0;
    f.depth &= ~FONT_SPARSE_DEPTH;
    if (sparse && !glyphs)
    {
        close(fd);
        return -1;
    }
#endif
    size_t glyph_buf_size = font_glyphs_to_bufsize( &f, glyphs );

    if ( bufsize && glyphs && bufsize > glyph_buf_size)
         bufsize = glyph_buf_size;
    else
    {
        if ( glyphs )
            bufsize = glyph_buf_size;
        else
            bufsize = MAX_FONT_SIZE;
    }
#ifdef FONT_HARD_LIMIT
    if ( bufsize > MAX_FONT_SIZE )
        bufsize = MAX_FONT_SIZE;
#endif
    if ( bufsize < (size_t) file_size )
        cached = true;
    else
        bufsize = file_size;
#ifdef HAVE_IPOD_UI
    if (sparse && !cached)
    {
        cached = true;                  /* a tiny set: still record reads */
        bufsize = glyph_buf_size;
    }
#endif

    /* check already loaded */
    int font_id = find_font_index(path);

    if (font_id > FONT_SYSFIXED)
    {
        /* already loaded, no need to reload */
        struct buflib_alloc_data *pd = core_get_data(buflib_allocations[font_id]);
        if (pd->font.buffer_size < bufsize || pd->path_bufsz < path_len)
        {
            int old_refcount, old_id;
            size_t old_bufsize = pd->font.buffer_size;
            bool failed = false;
            /* reload the font:
             * 1) save of refcont and id
             * 2) force unload (set refcount to 1 to make sure it get unloaded)
             * 3) reload with the larger buffer
             * 4) restore the id and refcount
             */
            old_id = font_id;
            old_refcount = pd->refcount;
            pd->refcount = 1;
            font_unload(font_id);
            font_id = font_load_ex(path, bufsize, glyphs);
            if (font_id < 0)
            {
                failed = true;
                font_id = font_load_ex(path, old_bufsize, 0);
                /* we couldn't even get the old size, this shouldn't happen */
                if ( font_id < 0 )
                    return -1;
            }
            if (old_id != font_id)
            {
                buflib_allocations[old_id] = buflib_allocations[font_id];
                buflib_allocations[font_id] = -1;
                font_id = old_id;
            }
            pd = core_get_data(buflib_allocations[font_id]);
            pd->refcount = old_refcount;
            if(failed)
                /* return error because we didn't satisfy the new buffer size */
                return -1;
        }
        pd->refcount++;
        //printf("reusing handle %d for %s (count: %d)\n", font_id, path, pd->refcount);
        close(fd);
        return font_id;
    }

    int open_slot = -1;

    for (font_id = FONT_FIRSTUSERFONT; font_id < MAXFONTS; font_id++)
    {
        if (buflib_allocations[ font_id ] < 0)
        {
            open_slot = font_id;
            break;
        }
    }
    if ( open_slot == -1 )
        return -1;
    font_id = open_slot;
    size_t path_bufsz = MAX(path_len + 1, 64); /* enough size for common case */
    /* allocate mem */
    int handle = core_alloc_ex(
                     bufsize + path_bufsz + sizeof( struct buflib_alloc_data ),
                     &buflibops );
    if ( handle <= 0 )
    {
        return -1;
    }
    struct buflib_alloc_data *pdata;

    pdata = core_get_data_pinned(handle);
    pdata->refcount     = 1;
#ifdef HAVE_IPOD_UI
    pdata->sparse_set   = -1;
    pdata->pin_handle   = -1;
    pdata->pin_n        = 0;
#endif
    pdata->path = pdata->buffer + bufsize;
    /* save load path so we can recognize this font later */
    memcpy(pdata->path, path, path_len+1);

    /* load and init */
    struct font *pf = &pdata->font;
    memcpy(pf, &f, sizeof( struct font) );

    pf->fd = fd;
    pf->fd_width = pf->fd_offset = -1;
    pf->handle = handle;
    pf->disabled = false;

    pf->buffer_start = buffer_from_handle( pf->handle );
    pf->buffer_position = pf->buffer_start + FONT_HEADER_SIZE;
    pf->buffer_size = bufsize;
    pf->buffer_end = pf->buffer_start + bufsize;

    if ( cached )
    {
#ifdef HAVE_IPOD_UI
        if (sparse)
        {
            if (!sparse_attach(pf, fd))
            {
                close(fd);
                core_free(handle);
                return -1;
            }
            pf->width = NULL;
            pf->bits = NULL;
            pf->file_width_offset = pf->file_offset_offset = 0;
            cache_create(pf);
        }
        else
#endif
        if ( ! font_load_cached( pf, nwidth, noffset ) )
        {
            core_free( handle );
            return -1;
        }

        /* trick to get a small cache for each file section   *
         * during glyph_cache_load()                          */
        pf->fd_width  = open( path, O_RDONLY|O_BINARY );
        pf->fd_offset = open( path, O_RDONLY|O_BINARY );

        glyph_cache_load( path, pf );

        /* cached font: pf->fd stays open until the font is unloaded */
        close( pf->fd_width );
        pf->fd_width = -1;
        close( pf->fd_offset );
        pf->fd_offset = -1;
    }
    else
    {
        lseek( fd, 0, SEEK_SET);
        read(fd, pf->buffer_start, pf->buffer_size);

        close( fd );
        pf->fd = -1;

        if ( ! font_load_in_memory( pf, nwidth, noffset ) )
        {
            core_free( handle );
            return -1;
        }
    }
    buflib_allocations[font_id] = handle;
    //printf("%s -> [%d] -> %d\n", path, font_id, *handle);
    core_put_data_pinned(pdata);
    logf("%s id: [%d], %s", __func__, font_id, path);
    return font_id; /* success!*/
}

int font_load(const char *path)
{
    return font_load_ex(path, MAX_FONT_SIZE, GLYPHS_TO_CACHE);
}

void font_unload(int font_id)
{
    if ( font_id < 0 || font_id >= MAXFONTS )
        return;
    int handle = buflib_allocations[font_id];
    if ( handle < 0 )
        return;
    struct buflib_alloc_data *pdata = core_get_data(handle);
    struct font* pf = &pdata->font;
    pdata->refcount--;
    if (pdata->refcount < 1)
    {
        logf("%s %s", __func__, pdata->path);
        if (pf && pf->fd >= 0)
        {
            glyph_cache_save(font_id);
            close(pf->fd);
        }
#ifdef HAVE_IPOD_UI
        sparse_pin_free(pdata);
        sparse_detach(pf);
        if (fill_font == font_id)
            fill_font = -1;
#endif
        core_free(handle);
        buflib_allocations[font_id] = -1;

    }
}

void font_unload_all(void)
{
    int i;
    for (i=0; i<MAXFONTS; i++)
    {
        if (buflib_allocations[i] > 0)
        {
            struct buflib_alloc_data *alloc = core_get_data(buflib_allocations[i]);
            alloc->refcount = 1; /* force unload */
            font_unload(i);
        }
    }
}

static void font_disable(int font_id)
{
    if ( font_id < 0 || font_id >= MAXFONTS )
        return;
    int handle = buflib_allocations[font_id];
    if ( handle < 0 )
        return;
    struct buflib_alloc_data *pdata = core_get_data(handle);
    struct font *pf = &pdata->font;

    if (pf->fd >= 0)
    {
        /* save the cache, but it keep it in-RAM so that cache lookups
         * can still succeed on the same font */
        glyph_cache_save(font_id);
        close(pf->fd);
        pf->fd = -1;
        pf->disabled = true;
    }
}

void font_disable_all(void)
{
    for(int i = 0; i < MAXFONTS; i++)
        font_disable(i);
}

static void font_enable(int font_id)
{
    if ( font_id < 0 || font_id >= MAXFONTS )
        return;
    int handle = buflib_allocations[font_id];
    if ( handle < 0 )
        return;
    struct buflib_alloc_data *pdata = core_get_data_pinned(handle);
    struct font *pf = &pdata->font;

    if (pf->disabled && pf->fd < 0)
    {
        pf->fd = open(pdata->path, O_RDONLY);
        pf->disabled = false;
    }
    core_put_data_pinned(pdata);
}

void font_enable_all(void)
{
    for(int i = 0; i < MAXFONTS; i++)
        font_enable(i);
}


/*
 * Return a pointer to an incore font structure.
 * If the requested font isn't loaded/compiled-in,
 * decrement the font number and try again.
 */
struct font* font_get(int font)
{
    bool is_ui_font = (font == FONT_UI);
    struct font* pf;
    if (is_ui_font)
        font = ui_font;
    if (font <= FONT_SYSFIXED || font >= MAXFONTS)
        return &sysfont;

    while (1) {
        if (buflib_allocations[font] > 0)
        {
            struct buflib_alloc_data *alloc = core_get_data(buflib_allocations[font]);
            pf = &alloc->font;
            if (pf && pf->height)
                return pf;
        }
        if (--font < 0)
        {
            if (is_ui_font && ui_font != FONT_LASTUSERFONT)
            {
                /* The users preferred UI font couldn't be loaded */
                ui_font = FONT_LASTUSERFONT;
                font = ui_font; /* try from the top to find something loaded */
            }
            else
                return &sysfont; /* Nothing else is available */
        }
    }
}

/* When FONT_UI is supplied to font_get this font will be tried first if it is
 * not loaded then fall back to default of any loaded font wins */
void set_ui_font(int font)
{
    /* Note: we don't allow setting FONT_SYSFIXED here */
    if (font > FONT_SYSFIXED && font < MAXFONTS)
        ui_font = font;
    else /* default (same as supplying FONT_UI) */
        ui_font = FONT_LASTUSERFONT;
}

/*
 * Reads an entry into cache entry
 */
static void
load_cache_entry(struct font_cache_entry* p, void* callback_data)
{
    struct font* pf = callback_data;

    ucschar_t char_code = p->_char_code;
    int fd;

    lock_font_handle(pf->handle, true);
#ifdef HAVE_IPOD_UI
    if (SPD(pf)->sparse_set >= 0)
    {
        /* ProkBox (#235): one read of one record at its rank */
        const uint8_t *map = sparse_map(pf);
        int bit = font_sparse_bit(char_code + pf->firstchar);
        int n = font_sparse_has(map, bit)
            ? font_sparse_rank(map, (const uint16_t *)
                               (map + FONT_SPARSE_BITMAP), bit) : -1;
        off_t at = FONT_SPARSE_DATA + (off_t)n * SPD(pf)->sparse_stride;

        /* jaglyph: a pinned record costs no read */
        if (n >= 0 && sparse_pin_get(pf, char_code + pf->firstchar,
                                     &p->width))
        {
            lock_font_handle(pf->handle, false);
            return;
        }
        DEVSTATS_FONT_BEGIN();      /* the shell's font work, not the
                                       screen's (modclass.c) */
        if (n < 0 || lseek(pf->fd, at, SEEK_SET) != at
            || read(pf->fd, &p->width, SPD(pf)->sparse_stride)
               != (ssize_t)SPD(pf)->sparse_stride)
        {
            /* a hole (font_companion() never sends one) or a short read:
             * a blank half-em, paper not ink */
            p->width = pf->maxwidth / 2;
            memset(p->bitmap, 0xff, glyph_bytes(pf, pf->maxwidth));
        }
        DEVSTATS_FONT_END();
        lock_font_handle(pf->handle, false);
        return;
    }
#endif
    if (pf->file_width_offset)
    {
        int width_offset = pf->file_width_offset + char_code;
        /* load via different fd to get this file section cached */
        if(pf->fd_width >=0 )
            fd = pf->fd_width;
        else
            fd = pf->fd;
        lseek(fd, width_offset, SEEK_SET);
        read(fd, &(p->width), 1);
    }
    else
    {
        p->width = pf->maxwidth;
    }

    int32_t bitmap_offset = 0;

    if (pf->file_offset_offset)
    {
        int32_t offset = pf->file_offset_offset + char_code * (pf->long_offset ? sizeof(int32_t) : sizeof(int16_t));
        /* load via different fd to get this file section cached */
        if(pf->fd_offset >=0 )
            fd = pf->fd_offset;
        else
            fd = pf->fd;
        lseek(fd, offset, SEEK_SET);
        unsigned char tmp[2];
        if (read (fd, tmp, 2) == 2)
        {
            bitmap_offset = tmp[0] | (tmp[1] << 8);
            if (pf->long_offset) {
                if (read (fd, tmp, 2) == 2)
                    bitmap_offset |= (tmp[0] << 16) | (tmp[1] << 24);
            }
        }
    }
    else
    {
        bitmap_offset = char_code * glyph_bytes(pf, p->width);
    }

    int32_t file_offset = FONT_HEADER_SIZE + bitmap_offset;
    lseek(pf->fd, file_offset, SEEK_SET);
    int src_bytes = glyph_bytes(pf, p->width);
    read(pf->fd, p->bitmap, src_bytes);

    lock_font_handle(pf->handle, false);
}

/*
 * Converts cbuf into a font cache
 */
static void cache_create(struct font* pf)
{
    /* maximum size of rotated bitmap */
    int bitmap_size = glyph_bytes(pf, pf->maxwidth);
    /* reserve one blank glyph that is guaranteed to be available, even
     * when the font file is closed during USB */
    unsigned char *cache_buf = pf->buffer_start + bitmap_size;
    size_t cache_size = pf->buffer_size - bitmap_size;
    ALIGN_BUFFER(cache_buf, cache_size, sizeof(ucschar_t));
#ifdef HAVE_IPOD_UI
    /* ProkBox (#266): a 4-bit glyph's 0 is full ink, 15 the paper; the
     * blank placeholder must be paper, or a cache miss while the font is
     * disabled (USB) draws a solid box */
    memset(pf->buffer_start, pf->depth ? 0xff : 0, bitmap_size);
#else
    memset(pf->buffer_start, 0, bitmap_size);
#endif
    /* Initialise cache */
    font_cache_create(&pf->cache, cache_buf, cache_size, bitmap_size);
}

#if defined(HAVE_IPOD_UI)
/* ProkBox (#235 first slice, #266): the ShellCJK companion. A code point
 * from U+3000 up that pf lacks is drawn from pf's companion font when one
 * is set, loaded, of the same height (so the glyph sits on pf's line) and
 * its window holds the code point; a hole of the companion draws its
 * default glyph (blank). Latin text pays one compare. */
/* 1 + the companion's id for each font id; 0 = none */
static signed char font_comp[MAXFONTS];

static struct font *font_companion(const struct font *pf, ucschar_t c)
{
    int i;

    if (c < 0x3000)
        return NULL;
    for (i = 0; i < MAXFONTS; i++)
    {
        int id = font_comp[i] - 1;
        struct font *cf;

        if (id < 0 || buflib_allocations[i] <= 0
            || buflib_allocations[id] <= 0
            || pf != &((struct buflib_alloc_data *)
                       core_get_data(buflib_allocations[i]))->font)
            continue;
        cf = &((struct buflib_alloc_data *)
               core_get_data(buflib_allocations[id]))->font;
        if (cf->height != pf->height || c < cf->firstchar
            || c >= cf->firstchar + cf->size)
            return NULL;
        /* #235: a code point the sparse set lacks stays with pf (its
         * aliases, or its default glyph) */
        if (SPD(cf)->sparse_set >= 0
            && !font_sparse_has(sparse_map(cf), font_sparse_bit(c)))
            return NULL;
        return cf;
    }
    return NULL;
}

void font_set_companion(int font_id, int comp_id)
{
    if (font_id < 0 || font_id >= MAXFONTS)
        return;
    font_comp[font_id] = (comp_id >= 0 && comp_id < MAXFONTS)
                         ? comp_id + 1 : 0;
}

static struct font *font_of_id(int font_id)
{
    if (font_id < 0 || font_id >= MAXFONTS
        || buflib_allocations[font_id] <= 0)
        return NULL;
    return &((struct buflib_alloc_data *)
             core_get_data(buflib_allocations[font_id]))->font;
}

uint32_t font_sparse_id(int font_id)
{
    struct font *pf = font_of_id(font_id);
    return pf && SPD(pf)->sparse_set >= 0 ? SPD(pf)->sparse_id : 0;
}

int font_sparse_stride(int font_id)
{
    struct font *pf = font_of_id(font_id);
    return pf && SPD(pf)->sparse_set >= 0 ? SPD(pf)->sparse_stride : 0;
}

/* jaglyph: one forward pass over the records, a 4 KB read covers every
 * wanted record inside it (the kana sit together; the kanji spread) */
#define PIN_CHUNK 4096

long font_sparse_pin(int font_id, const uint16_t *cps, int n, int *reads)
{
    struct font *pf = font_of_id(font_id);
    int i, m = 0, got = 0, handle, chunk, fhandle;
    unsigned char *blk, *buf;
    size_t stride, size;
    off_t chunk_at = -1;
    ssize_t chunk_len = 0;

    if (reads)
        *reads = 0;
    if (!pf || SPD(pf)->sparse_set < 0)
        return -1;
    sparse_pin_free(SPD(pf));
    if (!cps || n <= 0)
        return 0;
    if (pf->fd < 0 || pf->disabled)
        return -1;
    if (n > 0xffff)
        n = 0xffff;
    for (i = 0; i < n; i++)
        if (font_sparse_has(sparse_map(pf), font_sparse_bit(cps[i])))
            m++;
    if (m == 0)
        return 0;
    stride = SPD(pf)->sparse_stride;
    size = PIN_CPS_BYTES(m) + (size_t)m * stride;
    fhandle = pf->handle;
    lock_font_handle(fhandle, true);    /* pf stays put across the reads */
    handle = core_alloc_ex(size, NULL);
    chunk = handle > 0 ? core_alloc_ex(PIN_CHUNK, NULL) : -1;
    if (handle <= 0 || chunk <= 0)
    {
        if (handle > 0)
            core_free(handle);
        lock_font_handle(fhandle, false);
        return -1;
    }
    /* pinned for its life, like the index (sparse_attach()): the COP may
     * draw a miss from it while a compaction on the CPU runs */
    blk = core_get_data_pinned(handle);
    buf = core_get_data_pinned(chunk);
    for (i = 0; i < n; i++)
    {
        const uint8_t *map = sparse_map(pf);
        int bit = font_sparse_bit(cps[i]);
        off_t at;

        if (!font_sparse_has(map, bit))
            continue;
        at = FONT_SPARSE_DATA + (off_t)font_sparse_rank(map,
                (const uint16_t *)(map + FONT_SPARSE_BITMAP), bit) * stride;
        if (chunk_at < 0 || at < chunk_at
            || at + (off_t)stride > chunk_at + chunk_len)
        {
            chunk_at = at;
            chunk_len = -1;
            if (lseek(pf->fd, at, SEEK_SET) == at)
                chunk_len = read(pf->fd, buf, PIN_CHUNK);
            if (reads)
                (*reads)++;
            if (chunk_len < (ssize_t)stride)
                break;                  /* a short read: none pinned */
        }
        ((uint16_t *)blk)[got] = cps[i];
        memcpy(blk + PIN_CPS_BYTES(m) + (size_t)got * stride,
               buf + (at - chunk_at), stride);
        got++;
    }
    core_free(chunk);
    pf = font_of_id(font_id);
    if (got != m)
    {
        core_free(handle);
        lock_font_handle(fhandle, false);
        return -1;
    }
    SPD(pf)->pin_handle = handle;
    SPD(pf)->pin_n = (unsigned short)m;
    lock_font_handle(fhandle, false);
    return (long)size;
}

int font_cache_slots(int font_id)
{
    struct font *pf = font_of_id(font_id);
    return pf && pf->fd >= 0 ? (int)pf->cache._capacity : 0;
}

void font_set_fill(int font_id, font_fill_fn fn)
{
    fill_font = fn ? font_id : -1;
    fill_fn = fn;
}

static void fill_entry(struct font_cache_entry *p, void *arg)
{
    struct font *pf = ((void **)arg)[0];
    memcpy(&p->width, ((void **)arg)[1], SPD(pf)->sparse_stride);
}

void font_fill_put(struct font *pf, ucschar_t c, const unsigned char *rec)
{
    void *arg[2] = { pf, (void *)rec };

    if (SPD(pf)->sparse_set < 0 || c < pf->firstchar
        || c >= pf->firstchar + pf->size)
        return;
    font_cache_get(&pf->cache, c - pf->firstchar, false, fill_entry, arg);
}

/* before a cached sparse font's lookup of 'code' (from firstchar): a miss
 * goes to the fill source first, when pf has one */
static void font_fill_try(struct font *pf, ucschar_t code)
{
    if (SPD(pf)->sparse_set < 0 || fill_font < 0
        || buflib_allocations[fill_font] != pf->handle
        || font_cache_get(&pf->cache, code, true, NULL, NULL))
        return;
    lock_font_handle(pf->handle, true);
    fill_fn(pf, code + pf->firstchar);
    lock_font_handle(pf->handle, false);
}
#define FONT_COMPANION(pf, c) do { \
        struct font *cf_ = font_companion((pf), (c)); \
        if (cf_) \
            (pf) = cf_; \
    } while (0)
#else
#define FONT_COMPANION(pf, c) do { } while (0)
#endif

/*
 * Returns width of character
 */
int font_get_width(struct font* pf, ucschar_t char_code)
{
    int width;
    struct font_cache_entry *e;

    FONT_COMPANION(pf, char_code);
    /* check input range*/
    char_code = font_remap(pf, char_code);
    if (char_code < pf->firstchar || char_code >= pf->firstchar+pf->size)
        char_code = pf->defaultchar;
    char_code -= pf->firstchar;

    if (pf->fd >= 0 && pf != &sysfont)
    {
#ifdef HAVE_IPOD_UI
        font_fill_try(pf, char_code);
#endif
        width = font_cache_get(&pf->cache,char_code,false,load_cache_entry,pf)->width;
    }
    else if (pf->disabled &&
        (e = font_cache_get(&pf->cache,char_code,true,load_cache_entry,pf)))
        width = e->width; /* falls back to pf->maxwidth if !e */
    else if (pf->width)
        width = pf->width[char_code];
    else
        width = pf->maxwidth;

    return width;
}

const unsigned char* font_get_bits(struct font* pf, ucschar_t char_code)
{
    const unsigned char* bits;

    FONT_COMPANION(pf, char_code);
    /* check input range*/
    char_code = font_remap(pf, char_code);
    if (char_code < pf->firstchar || char_code >= pf->firstchar+pf->size)
        char_code = pf->defaultchar;
    char_code -= pf->firstchar;

    if (pf->fd >= 0 && pf != &sysfont)
    {
#ifdef HAVE_IPOD_UI
        font_fill_try(pf, char_code);
#endif
        bits =
            (unsigned char*)font_cache_get(&pf->cache, char_code,
                                false, load_cache_entry, pf)->bitmap;
    }
    else if (pf->disabled)
    {
        /* the font handle is closed, but the cache is intact. Attempt
         * a lookup, which is very likely to succeed. Return a placeholder
         * glyph on miss (again, this is very unlikely */
        struct font_cache_entry *e = font_cache_get(&pf->cache, char_code,
                                                    true, NULL, NULL);
        if (LIKELY(e))
            bits = (unsigned char *) e->bitmap;
        else
        {
            /* Could attempt to find a suitable fallback glyph from the same
             * font. For now just return blank space which is
             * reserved by cache_create() at buffer_start */
            bits = pf->buffer_start;
        }
    }
    else
    {
        /* This font is entirely in RAM */
        bits = pf->bits;
        if (pf->offset)
        {
            if (pf->bits_size < MAX_FONTSIZE_FOR_16_BIT_OFFSETS)
                bits += ((uint16_t*)(pf->offset))[char_code];
            else
                bits += ((uint32_t*)(pf->offset))[char_code];
        }
        else
            bits += char_code * glyph_bytes(pf, pf->maxwidth);
    }

    return bits;
}

#if defined(HAVE_IPOD_UI)
/* Whether pf draws c with a glyph of its own (or an alias's) rather than
 * the default character: in range after font_remap() and not a hole the
 * converter filled with the default glyph. For text readers that mark a
 * character the type lacks (apps/ipodui/screens/notes.c, contacts.c). */
bool font_has_glyph(struct font *pf, ucschar_t c)
{
    ucschar_t r;

    FONT_COMPANION(pf, c);
    r = font_remap(pf, c);

    if (r < pf->firstchar || r >= pf->firstchar + pf->size)
        return false;
    if (r == (ucschar_t)pf->defaultchar)
        return true;
    return font_get_bits(pf, c) != font_get_bits(pf, pf->defaultchar);
}
#endif

extern int open_pathfmt(char *buf, size_t size, int oflag, const char *pathfmt, ...); /*misc.c*/
static NO_INLINE int font_path_open_glyphcache(const char *font_path, int oflags)
{
    char glyph_path[MAX_PATH];
    int dotidx = strlen(font_path) - sizeof(FONT_EXT);
    logf("%s %.*s.%s", __func__, dotidx, font_path, GLYPH_CACHE_EXT);
    return open_pathfmt(glyph_path, sizeof(glyph_path), oflags, 
                        "%.*s.%s", dotidx, font_path, GLYPH_CACHE_EXT);
}

/* call with NULL to flush */
static void glyph_file_write(void* data)
{
    struct font_cache_entry* p = data;
    struct font* pf = cache_pf;
    ucschar_t ch;
    static int buffer_pos = 0;
#define WRITE_BUFFER 256
    static unsigned char buffer[WRITE_BUFFER];

    /* flush buffer & reset */
    if ( data == NULL || buffer_pos >= WRITE_BUFFER)
    {
        write(cache_fd, buffer, buffer_pos);
        buffer_pos = 0;
        if ( data == NULL )
            return;
    }
    if ( p->_char_code == 0xffff )
        return;

    ch = p->_char_code + pf->firstchar;
#ifdef UNICODE32
    buffer[buffer_pos] = (ch >> 24) & 0xff;
    buffer[buffer_pos+1] = (ch >> 16) & 0xff;
    buffer[buffer_pos+2] = (ch >> 8) & 0xff;
    buffer[buffer_pos+3] = ch & 0xff;
    buffer_pos += 4;
#else
    buffer[buffer_pos] = (ch >> 8) & 0xff;
    buffer[buffer_pos+1] = ch & 0xff;
    buffer_pos += 2;
#endif
    return;
}

/* save the char codes of the loaded glyphs to a file */
static void glyph_cache_save(int font_id)
{
    int fd;

    if( font_id < 0 )
        return;
    int handle = buflib_allocations[font_id];
    if ( handle < 0 )
        return;

    struct buflib_alloc_data *pdata = core_get_data_pinned(handle);
    struct font *pf = &pdata->font;

    if(pf && pf->fd >= 0)
    {
        fd = font_path_open_glyphcache(pdata->path, O_WRONLY|O_CREAT|O_TRUNC);
        if (fd >= 0)
        {
            uint32_t header = FC_HEADER_VAL;
            write(fd, &header, sizeof(header));
            cache_pf = pf;
            cache_fd = fd;
            lru_traverse(&cache_pf->cache._lru, glyph_file_write);
            glyph_file_write(NULL);
            if (cache_fd >= 0)
            {
                close(cache_fd);
                cache_fd = -1;
            }
        }
    }
    core_put_data_pinned(pdata);
    return;
}


static int ucscharcmp(const void *a, const void *b)
{
    return ((int)(*(ucschar_t*)a - *(ucschar_t*)b));
}

static NO_INLINE void glyph_cache_load(const char *font_path, struct font *pf)
{
#define MAX_SORT 256
    if (pf->fd >= 0) {
        int i, size, fd;
        unsigned char tmp[sizeof(ucschar_t)];
        ucschar_t ch;
        ucschar_t glyphs[MAX_SORT];
        ucschar_t glyphs_lru_order[MAX_SORT];
        unsigned int glyph_file_skip=0, glyph_file_size=0;

        int sort_size = pf->cache._capacity;
        if ( sort_size > MAX_SORT )
             sort_size = MAX_SORT;

        fd = font_path_open_glyphcache(font_path, O_RDONLY|O_BINARY);
#ifdef TRY_DEFAULT_GLYPHCACHE
        /* if font specific file fails, try default */
        if (fd < 0)
            fd = open(GLYPH_CACHE_FILE, O_RDONLY|O_BINARY);
#endif
        if (fd >= 0) {
            /* Header */
            uint32_t hdr = 0;
            read(fd, &hdr, sizeof(hdr));
            if (hdr != FC_HEADER_VAL)
                goto latin;
            /* only read what fits */
            glyph_file_size = ffilesize( fd );
            if (glyph_file_size < sizeof(uint32_t))
                goto latin;
            glyph_file_size -= sizeof(uint32_t);
            if ( glyph_file_size > (int)sizeof(ucschar_t)*pf->cache._capacity ) {
                glyph_file_skip = glyph_file_size - sizeof(ucschar_t)*pf->cache._capacity;
                lseek( fd, glyph_file_skip + sizeof(uint32_t), SEEK_SET );
            }
            while(1) {
                for ( size = 0;
                      read( fd, tmp, sizeof(tmp) ) == sizeof(tmp) && size < sort_size;
                      size++ )
                {
#ifdef UNICODE32
                    glyphs[size] = (tmp[0] << 24) | (tmp[1] << 16) | (tmp[2] << 8) | tmp[3];
#else
                    glyphs[size] = (tmp[0] << 8) | tmp[1];
#endif
                    glyphs_lru_order[size] = glyphs[size];
                }

                /* sort glyphs array to make sector cache happy */
                qsort((void *)glyphs, size, sizeof(ucschar_t),
                      ucscharcmp );

                /* load font bitmaps */
                for( i = 0; i < size ; i++ )
                    font_get_bits(pf, glyphs[i]);

                /* redo to fix lru order */
                for ( i = 0; i < size ; i++)
                    font_get_bits(pf, glyphs_lru_order[i]);

                if ( size < sort_size )
                    break;
            }

            close(fd);
        } else {
        latin:
#ifdef HAVE_IPOD_UI
            if (SPD(pf)->sparse_set >= 0)
                return;         /* #235: no Latin in a sparse CJK font */
#endif
            /* load latin1 chars into cache */
            for ( ch = 32 ; ch < 256  && ch < pf->cache._capacity + 32; ch++ )
                font_get_bits(pf, ch);
        }
    }
    return;
}
#else /* BOOTLOADER */

void font_init(void)
{
}

void font_lock(int font_id, bool lock)
{
    (void)font_id;
    (void)lock;
}

/*
 * Bootloader only supports the built-in sysfont.
 */
struct font* font_get(int font)
{
    (void)font;
    return &sysfont;
}

/*
 * Returns width of character
 */
int font_get_width(struct font* pf, ucschar_t char_code)
{
    /* check input range*/
    if (char_code < pf->firstchar || char_code >= pf->firstchar+pf->size)
        char_code = pf->defaultchar;
    char_code -= pf->firstchar;

    return pf->width? pf->width[char_code]: pf->maxwidth;
}

const unsigned char* font_get_bits(struct font* pf, ucschar_t char_code)
{
    const unsigned char* bits;

    /* check input range*/
    if (char_code < pf->firstchar || char_code >= pf->firstchar+pf->size)
        char_code = pf->defaultchar;
    char_code -= pf->firstchar;

    /* assume small font with uint16_t offsets*/
    bits = pf->bits + (pf->offset?
            ((uint16_t*)(pf->offset))[char_code]:
            (((pf->height + 7) / 8) * pf->maxwidth * char_code));

    return bits;
}

#endif /* BOOTLOADER */

/*
 * Returns the length (in bytes) of a given NULL terminated string
 *  stops after maxwidth, maxbytes or NULL (\0) whichever occurs first.
 * maxbytes = -1 ignores maxbytes and relies on NULL terminator (\0)
 *  to terminate the string
 * maxwidth = -1 ignores maxwidth
 * stringsize pixel width and height will be returned in *w and *h
 */
int font_measurestring(const unsigned char *str, size_t maxbytes, size_t maxwidth, int *w, int *h, int fontnum)
{
    const unsigned char *start = str;
    size_t bytes;
    struct font* pf = font_get(fontnum);
    font_lock( fontnum, true );
    ucschar_t ch;
    size_t width = 0;

    while (true)
    {
        bytes = str - start;
        str = utf8decode(str, &ch);
        if (ch == 0 || bytes >= maxbytes)
        {
            break;
        }
        if (IS_DIACRITIC(ch))
            continue;
        int fw = font_get_width(pf,ch);
        width += fw;
        if (width > maxwidth)
        {
            width -= fw;
            break;
        }
    }

    if ( h )
        *h = pf->height;
    if ( w )
        *w = width;

    font_lock( fontnum, false );

    return bytes;
}

/*
 * Returns the stringsize of a given NULL terminated string
 *  stops after maxbytes or NULL (\0) whichever occurs first.
 * maxbytes = -1 ignores maxbytes and relies on NULL terminator (\0)
 *  to terminate the string
 */
int font_getstringnsize(const unsigned char *str, size_t maxbytes, int *w, int *h, int fontnum)
{
    int width = 0;
    if (!w)
        w = &width;
    font_measurestring(str, maxbytes, -1, w, h, fontnum);
    return *w;
}

/*
 * Returns the stringsize of a given NULL terminated string.
 */
int font_getstringsize(const unsigned char *str, int *w, int *h, int fontnumber)
{
    return font_getstringnsize(str, -1, w, h, fontnumber);
}

/* -----------------------------------------------------------------
 * vim: et sw=4 ts=8 sts=4 tw=78
 */
