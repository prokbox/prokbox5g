/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2014 by Michael Sevakis
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
#include "config.h"
#include <errno.h>
#include "system.h"
#include "debug.h"
#include "panic.h"
#include "pathfuncs.h"
#include "disk_cache.h"
#include "fileobj_mgr.h"
#include "rb_namespace.h"
#include "string-extra.h"
#include "rbunicode.h"
#ifdef FILE_WALK_CACHE
#include "core_alloc.h"
#endif

/* Define LOGF_ENABLE to enable logf output in this file */
//#define LOGF_ENABLE
#ifdef LOGF_ENABLE
#include "logf.h"
#undef DEBUGF
#define DEBUGF logf
#endif

/** Internal common filesystem service functions  **/

/* for internal functions' scanning use to save quite a bit of stack space -
   access must be serialized by the writer lock */
#if defined(IAUDIO_M5) || CONFIG_CPU == IMX233
/* otherwise, out of IRAM */
struct fat_direntry dir_fatent;
#else
struct fat_direntry dir_fatent IBSS_ATTR;
#endif

struct mrsw_lock file_internal_mrsw SHAREDBSS_ATTR;

#ifdef XCORE_CHECKS
/* ProkBox p19-copfileio (xcore.h): the file layer entered on the COP.  The
 * message names the entry's caller (lr); the panic record keeps it */
void xcore_fs_offcore(const char *what, void *lr)
{
    panicf("fs %s off the CPU lr=%08lx", what, (unsigned long)lr);
}
#endif


/** File stream sector caching **/

/* initialize a new cache structure */
void file_cache_init(struct filestr_cache *cachep)
{
    cachep->buffer = NULL;
    cachep->sector = INVALID_SECNUM;
    cachep->flags  = 0;
}

/* discard and mark the cache buffer as unused */
void file_cache_reset(struct filestr_cache *cachep)
{
    cachep->sector = INVALID_SECNUM;
    cachep->flags  = 0;
}

/* allocate resources attached to the cache */
void file_cache_alloc(struct filestr_cache *cachep)
{
    /* if this fails, it is a bug; check for leaks and that the cache has
       enough buffers for the worst case */
    if (!cachep->buffer && !(cachep->buffer = dc_get_buffer()))
        panicf("file_cache_alloc - OOM");
}

/* free resources attached to the cache */
void file_cache_free(struct filestr_cache *cachep)
{
    if (cachep)
    {
        if(cachep->buffer)
        {
            dc_release_buffer(cachep->buffer);
            cachep->buffer = NULL;
        }
        file_cache_reset(cachep);
    }
}


/** Stream base APIs **/

static inline void filestr_clear(struct filestr_base *stream, unsigned int flags)
{
    stream->flags = flags;
    stream->bindp = NULL;
#if 0
    stream->mtx   = NULL;
#endif
}

/* actually late-allocate the assigned cache */
void filestr_alloc_cache(struct filestr_base *stream)
{
    file_cache_alloc(stream->cachep);
}

/* free the stream's cache buffer if it's its own */
void filestr_free_cache(struct filestr_base *stream)
{
    if (stream->cachep == &stream->cache)
        file_cache_free(stream->cachep);
}

/* assign a cache to the stream */
void filestr_assign_cache(struct filestr_base *stream,
                          struct filestr_cache *cachep)
{
    if (cachep)
    {
        filestr_free_cache(stream);
        stream->cachep = cachep;
    }
    else /* assign own cache */
    {
        file_cache_reset(&stream->cache);
        stream->cachep = &stream->cache;
    }
}

/* duplicate a cache into a stream's local cache */
void filestr_copy_cache(struct filestr_base *stream,
                        struct filestr_cache *cachep)
{
    stream->cachep = &stream->cache;
    stream->cache.sector = cachep->sector;
    stream->cache.flags  = cachep->flags;

    if (cachep->buffer)
    {
        file_cache_alloc(&stream->cache);
        memcpy(stream->cache.buffer, cachep->buffer, DC_CACHE_BUFSIZE);
    }
    else
    {
        file_cache_free(&stream->cache);
    }
}

/* discard cache contents and invalidate it */
void filestr_discard_cache(struct filestr_base *stream)
{
    file_cache_reset(stream->cachep);
}

/* Initialize the base descriptor */
void filestr_base_init(struct filestr_base *stream)
{
    filestr_clear(stream, FD_VALID);
    file_cache_init(&stream->cache);
    stream->cachep = &stream->cache;
}

/* free base descriptor resources */
void filestr_base_destroy(struct filestr_base *stream)
{
    filestr_clear(stream, 0);
    filestr_free_cache(stream);
}


/** Internal directory service functions **/

/* read the next directory entry and return its FS info */
int uncached_readdir_internal(struct filestr_base *stream,
                              struct file_base_info *infop,
                              struct fat_direntry *fatent)
{
    return fat_readdir(&stream->fatstr, &infop->fatfile.e,
                       filestr_get_cache(stream), fatent);
}

/* rewind the FS directory to the beginning */
void uncached_rewinddir_internal(struct file_base_info *infop)
{
    fat_rewinddir(&infop->fatfile.e);
}

/* check if the directory is empty (ie. only "." and/or ".." entries
   exist at most) */
int test_dir_empty_internal(struct filestr_base *stream)
{
    int rc;

    struct file_base_info info;
    fat_rewind(&stream->fatstr);
    rewinddir_internal(&info);

    while ((rc = readdir_internal(stream, &info, &dir_fatent)) > 0)
    {
        /* no OEM decoding is recessary for this simple check */
        if (!is_dotdir_name(dir_fatent.name))
        {
            DEBUGF("Directory not empty\n");
            FILE_ERROR_RETURN(ENOTEMPTY, -1);
        }
    }

    if (rc < 0)
    {
        DEBUGF("I/O error checking directory: %d\n", rc);
        FILE_ERROR_RETURN(EIO, rc * 10 - 2);
    }

    return 0;
}

/* iso decode the name to UTF-8 */
void iso_decode_d_name(char *d_name)
{
#ifdef HAVE_FILESYSTEM_CODEPAGE
    if (is_dotdir_name(d_name))
        return;

    char shortname[13];
    /* this only gets called in the case of DOS (8.3) filenames */
    size_t len = strlcpy(shortname, d_name, sizeof (shortname));
    /* This MUST be the default codepage thus not something that could be
       loaded on call */
    iso_decode(shortname, d_name, -1, len + 1);
#else
    (void)d_name;
#endif
}

#ifdef HAVE_DIRCACHE
/* nullify all the fields of the struct dirent */
void empty_dirent(struct DIRENT *entry)
{
    entry->d_name[0] = '\0';
    entry->info.attr    = 0;
    entry->info.size    = 0;
    entry->info.wrtdate = 0;
    entry->info.wrttime = 0;
}

/* fill the native dirinfo from the static dir_fatent */
void fill_dirinfo_native(struct dirinfo_native *dinp)
{
    struct fat_direntry *fatent = get_dir_fatent();
    dinp->attr    = fatent->attr;
    dinp->size    = fatent->filesize;
    dinp->wrtdate = fatent->wrtdate;
    dinp->wrttime = fatent->wrttime;
}
#endif /* HAVE_DIRCACHE */

#ifdef FILE_WALK_CACHE
/* ProkBox (#243): the walk lookup cache. Without dircache every open(),
 * opendir() and file_exists() scans each folder on its path again, name
 * by name (a library walk resolved /Music, the artist and the album for
 * every track: ~7x stock's lookup time at 10k tracks). While a walk has
 * it on, folders are kept here, keyed by the parent's first cluster and
 * the name, and a lookup of one skips the scan of its parent. A folder is
 * kept when a lookup's scan finds it, and when a listing (readdir) reads
 * it: the walk's next opendir() is of that folder (a scan of the parent
 * from its start for every folder grew with the square of its size).
 *  - Folders only: a folder's entry never changes in place (no size, no
 *    cluster change, never written through), so the only way one goes
 *    stale is an entry added or freed, or a remount, and fat.c moves
 *    fat_dirent_gen for each (any thread); a slot set under another
 *    generation is never read. Misses are not kept.
 *  - Exactly what the scan finds: the first entry whose name matches
 *    case-insensitively, as strcasecmp() (one byte for one byte, so equal
 *    names have equal lengths); the scan state (the entry's index and
 *    count) is the one the scan leaves. A name already kept stays (the
 *    first of a name). A name longer than a slot holds, a listed name in
 *    the OEM code page, and FF_NOISO lookups scan as before.
 *  - Shared by every thread: lookups and mutations hold the file writer
 *    lock, listings the reader lock; nothing in here yields. No resident
 *    RAM: a movable core_alloc() made by a file_walk_cache(true) that
 *    finds none (the first, or a later one after a first had no room),
 *    freed by the last (false); the users are counted (the library walk
 *    and the art builder, #332), taken only from free memory (never by
 *    shrinking the audio buffer; no room: no cache). The pointer is
 *    fetched again at every use. */
#ifndef WALK_CACHE_SLOTS
#define WALK_CACHE_SLOTS 16         /* x 128 B: tests/walkcache */
#endif
struct walk_cache_slot
{
    struct fat_file fatfile;        /* the folder; .dircluster: the key */
    uint32_t        filesize;       /* as its entry says */
    uint32_t        age;            /* last use; 0: empty */
    uint8_t         attr;           /* its entry's attributes */
    uint8_t         length;         /* name bytes */
    char            name[128 - 26]; /* not terminated */
};
static int walk_cache_handle;       /* > 0: on */
static int walk_cache_users;
static unsigned int walk_cache_gen; /* fat_dirent_gen the slots are of */
static uint32_t walk_cache_tick;
#ifdef WALK_CACHE_STATS
unsigned long walk_cache_hits, walk_cache_misses;
#endif

/* the slots, or NULL: off, or emptied when a directory entry changed */
static struct walk_cache_slot * walk_cache_slots(void)
{
    if (walk_cache_handle <= 0)
        return NULL;

    struct walk_cache_slot *s = core_get_data(walk_cache_handle);
    if (walk_cache_gen != fat_dirent_gen)
    {
        memset(s, 0, WALK_CACHE_SLOTS * sizeof (*s));
        walk_cache_gen = fat_dirent_gen;
    }

    return s;
}

/* the slot of 'name' in the folder whose first cluster is 'dircluster' */
static struct walk_cache_slot * walk_cache_find(struct walk_cache_slot *s,
                                                const struct fat_file *parent,
                                                const char *name,
                                                size_t length)
{
    for (int i = 0; i < WALK_CACHE_SLOTS; i++, s++)
    {
#if defined(WALKCACHE_BREAK) && (WALKCACHE_BREAK & 32)
        if (s->age && s->fatfile.dircluster == parent->firstcluster)
            return s;               /* negative control: the name ignored */
#endif
        if (s->age && s->length == length
            && s->fatfile.dircluster == parent->firstcluster
            IF_MV( && s->fatfile.volume == parent->volume )
            && !strncasecmp(s->name, name, length))
            return s;
    }

    return NULL;
}

/* keep 'file', found or listed in its parent under 'name', if a folder */
static void walk_cache_put(const struct fat_file *parent,
                           const struct fat_file *file,
                           const struct fat_direntry *fatent,
                           const char *name, size_t length)
{
    struct walk_cache_slot *s = walk_cache_slots();
    if (!s || !(fatent->attr & ATTR_DIRECTORY) || length > sizeof (s->name)
        || walk_cache_find(s, parent, name, length))
        return;

    /* an empty slot (age 0), else the least recently used */
    struct walk_cache_slot *victim = s;
#if !defined(WALKCACHE_BREAK) || !(WALKCACHE_BREAK & 16)
    for (int i = 1; i < WALK_CACHE_SLOTS; i++)
        if (s[i].age < victim->age)
            victim = &s[i];
#endif                              /* else: always slot 0 */

    victim->fatfile  = *file;
    victim->filesize = fatent->filesize;
    victim->attr     = fatent->attr;
    victim->length   = length;
    memcpy(victim->name, name, length);
    victim->age      = ++walk_cache_tick;
}

void file_walk_cache(bool on)
{
    const size_t size = WALK_CACHE_SLOTS * sizeof (struct walk_cache_slot);
    int handle = 0;

    /* outside the lock: an allocation may call other owners back. Also
     * when others hold it with no block (#332: the art builder's hold,
     * made with no room, must not leave a library walk that starts later
     * with free memory uncached); a second block is freed below */
    if (on && walk_cache_handle <= 0 && core_allocatable() >= size + 64)
        handle = core_alloc(size);

    file_internal_lock_WRITER();
    if (on)
    {
        walk_cache_users++;
        if (handle > 0 && walk_cache_handle <= 0)
        {
            memset(core_get_data(handle), 0, size);
            walk_cache_handle = handle;
            walk_cache_gen = fat_dirent_gen;
            walk_cache_tick = 0;
            handle = 0;
        }
    }
    else if (walk_cache_users > 0 && --walk_cache_users == 0)
    {
        handle = walk_cache_handle;
        walk_cache_handle = 0;
    }
    file_internal_unlock_WRITER();

    if (handle > 0)
        core_free(handle);          /* not installed, or the last off */
}
#endif /* FILE_WALK_CACHE */

int uncached_readdir_dirent(struct filestr_base *stream,
                            struct dirscan_info *scanp,
                            struct DIRENT *entry)
{
    struct fat_direntry fatent;
    int rc = fat_readdir(&stream->fatstr, &scanp->fatscan,
                         filestr_get_cache(stream), &fatent);

    /* FAT driver clears the struct fat_dirent if nothing is returned */
    strcpy(entry->d_name, fatent.name);
    entry->info.attr    = fatent.attr;
    entry->info.size    = fatent.filesize;
    entry->info.wrtdate = fatent.wrtdate;
    entry->info.wrttime = fatent.wrttime;
#if defined(HAVE_IPOD_UI) && !defined(BOOTLOADER)
    entry->info.fstclus = fatent.firstcluster;  /* #89: the boot digest */
#endif
#ifdef FILE_WALK_CACHE
    if (rc == 1)    /* a long name (2: OEM, decoded by the caller) */
    {
        /* as a lookup's scan of this folder leaves the entry it finds */
        const struct fat_file *parent = &stream->infop->fatfile;
        struct fat_file file;
        IF_MV( file.volume = parent->volume; )
        file.firstcluster = fatent.firstcluster;
        file.dircluster   = parent->firstcluster;
        file.e            = scanp->fatscan;
#if defined(WALKCACHE_BREAK) && (WALKCACHE_BREAK & 64)
        file.e.entry++;             /* negative control: a wrong position */
#endif
        walk_cache_put(parent, &file, &fatent, fatent.name,
                       strlen(fatent.name));
    }
#endif

    return rc;
}

/* rewind the FS directory pointer */
void uncached_rewinddir_dirent(struct dirscan_info *scanp)
{
    fat_rewinddir(&scanp->fatscan);
}


/** open_stream_internal() helpers and types **/

struct pathwalk
{
    const char                 *path;     /* current location in input path */
    unsigned int               callflags; /* callflags parameter */
    struct path_component_info *compinfo; /* compinfo parameter */
    file_size_t                filesize;  /* size of the file */
};

struct pathwalk_component
{
    struct file_base_info     info;       /* basic file information */
    const char                *name;      /* component name location in path */
    uint16_t                  length;     /* length of name of component */
    uint16_t                  attr;       /* attributes of this component */
    struct pathwalk_component *nextp;     /* parent if in use else next free */
};

#define WALK_RC_NOT_FOUND    0   /* successfully not found (aid for file creation) */
#define WALK_RC_FOUND        1   /* found and opened */
#define WALK_RC_FOUND_ROOT   2   /* found and opened sys root */
#define WALK_RC_CONT_AT_ROOT 3   /* continue at root level */

/* return another struct pathwalk_component from the pool, or NULL if the
   pool is completely used */
static void * pathwalk_comp_alloc_(struct pathwalk_component *parentp)
{
    /* static pool that goes to a depth of STATIC_COMP_NUM before allocating
       elements from the stack */
    static struct pathwalk_component aux_pathwalk[STATIC_PATHCOMP_NUM];
    struct pathwalk_component *compp = NULL;

    if (!parentp)
        compp = &aux_pathwalk[0]; /* root */
    else if (PTR_IN_ARRAY(aux_pathwalk, parentp, STATIC_PATHCOMP_NUM-1))
        compp = parentp + 1;

    return compp;
}

/* allocates components from the pool or stack depending upon the depth */
#define pathwalk_comp_alloc(parentp) \
    ({                                                        \
        void *__c = pathwalk_comp_alloc_(parentp);            \
        if (!__builtin_constant_p(parentp) && !__c)           \
            __c = alloca(sizeof (struct pathwalk_component)); \
        (struct pathwalk_component *)__c;                     \
    })

/* fill in the details of the struct path_component_info for caller */
static int fill_path_compinfo(struct pathwalk *walkp,
                              struct pathwalk_component *compp,
                              int rc)
{
    if (rc == -ENOENT)
    {
        /* this component wasn't found; see if more of them exist or path
           has trailing separators; if it does, this component should be
           interpreted as a directory even if it doesn't exist and it's the
           final one; also, this has to be the last part or it's an error*/
        const char *p = GOBBLE_PATH_SEPCH(walkp->path);
        if (!*p)
        {
            if (p > walkp->path)
                compp->attr |= ATTR_DIRECTORY;

            rc = WALK_RC_NOT_FOUND; /* successfully not found */
        }
    }

    if (rc >= 0)
    {
        struct path_component_info *compinfo = walkp->compinfo;
        compinfo->name       = compp->name;
        compinfo->length     = compp->length;
        compinfo->attr       = compp->attr;
        compinfo->filesize   = walkp->filesize;
        if (walkp->callflags & FF_INFO)
            compinfo->info = compp->info;
        if (walkp->callflags & FF_PARENTINFO)
            compinfo->parentinfo = (compp->nextp ?: compp)->info;
    }

    return rc;
}

/* open the final stream itself, if found */
static int walk_open_info(struct pathwalk *walkp,
                          struct pathwalk_component *compp,
                          int rc,
                          struct filestr_base *stream)
{
    /* this may make adjustments to things; do it first */
    if (walkp->compinfo)
        rc = fill_path_compinfo(walkp, compp, rc);

    if (rc < 0 || rc == WALK_RC_NOT_FOUND)
        return rc;

    unsigned int callflags = walkp->callflags;
    bool isdir = compp->attr & ATTR_DIRECTORY;

    /* type must match what is called for */
    switch (callflags & FF_TYPEMASK)
    {
    case FF_FILE:
        if (!isdir) break;
        DEBUGF("File is a directory\n");
        return -EISDIR;
    case FF_DIR:
        if (isdir) break;
        DEBUGF("File is not a directory\n");
        return -ENOTDIR;
    /* FF_ANYTYPE: basically, ignore FF_FILE/FF_DIR */
    }

    /* FO_DIRECTORY must match type */
    if (isdir)
        callflags |= FO_DIRECTORY;
    else
        callflags &= ~FO_DIRECTORY;

    /* make open official if not simply probing for presence - must do it here
       or compp->info on stack will get destroyed before it was copied */
    if (!(callflags & (FF_PROBE|FF_NOFS)))
        fileop_onopen_internal(stream, &compp->info, callflags);
    return compp->attr == ATTR_SYSTEM_ROOT ? WALK_RC_FOUND_ROOT : WALK_RC_FOUND;
}

/* check the component against the prefix test info */
static void walk_check_prefix(struct pathwalk *walkp,
                              struct pathwalk_component *compp)
{
    if (compp->attr & ATTR_PREFIX)
        return;

    if (!fat_file_is_same(&compp->info.fatfile,
                          &walkp->compinfo->prefixp->fatfile))
        return;

    compp->attr |= ATTR_PREFIX;
}

#ifdef FILE_WALK_CACHE
/* a hit: compp and walkp as the scan would leave them */
static bool walk_cache_get(struct pathwalk *walkp,
                           struct pathwalk_component *compp)
{
    struct walk_cache_slot *s = walk_cache_slots();
    if (!s || (walkp->callflags & FF_NOISO))
        return false;

    s = walk_cache_find(s, &compp->nextp->info.fatfile, compp->name,
                        compp->length);
#if defined(WALKCACHE_BREAK) && (WALKCACHE_BREAK & 8)
    s = NULL;                       /* negative control: never a hit */
#endif
#ifdef WALK_CACHE_STATS
    if (s) walk_cache_hits++; else walk_cache_misses++;
#endif
    if (!s)
        return false;

    s->age = ++walk_cache_tick;
    compp->info.fatfile = s->fatfile;
    walkp->filesize = s->filesize;
    compp->attr |= s->attr;
    return true;
}
#endif /* FILE_WALK_CACHE */

/* opens the component named by 'comp' in the directory 'parent' */
static NO_INLINE int open_path_component(struct pathwalk *walkp,
                                         struct pathwalk_component *compp,
                                         struct filestr_base *stream)
{
    int rc;

    /* create a null-terminated copy of the component name */
    char *compname = strmemdupa(compp->name, compp->length);

    unsigned int callflags = walkp->callflags;
    struct pathwalk_component *parentp = compp->nextp;

    /* children inherit the prefix coloring from the parent */
    compp->attr = parentp->attr & ATTR_PREFIX;

    /* most of the next would be abstracted elsewhere if doing other
       filesystems */

    /* scan parent for name; stream is converted to this parent */
    file_cache_reset(stream->cachep);
    stream->infop = &parentp->info;
    fat_filestr_init(&stream->fatstr, &parentp->info.fatfile);
    rewinddir_internal(&compp->info);

#ifdef FILE_WALK_CACHE
    if (walk_cache_get(walkp, compp))
        goto found;
#endif

    while ((rc = readdir_internal(stream, &compp->info, &dir_fatent)) > 0)
    {
        if (rc > 1 && !(callflags & FF_NOISO))
            iso_decode_d_name(dir_fatent.name);

        if (!strcasecmp(compname, dir_fatent.name))
            break;
    }

    if (rc == 0)
    {
        DEBUGF("File/directory not found\n");
        return -ENOENT;
    }
    else if (rc < 0)
    {
        DEBUGF("I/O error reading directory %d\n", rc);
        return -EIO;
    }

    rc = fat_open(stream->fatstr.fatfilep, dir_fatent.firstcluster,
                  &compp->info.fatfile);
    if (rc < 0)
    {
        DEBUGF("I/O error opening file/directory %s (%d)\n",
               compname, rc);
        return -EIO;
    }

    walkp->filesize = dir_fatent.filesize;
    compp->attr    |= dir_fatent.attr;
#ifdef FILE_WALK_CACHE
    if (!(callflags & FF_NOISO))
        walk_cache_put(&parentp->info.fatfile, &compp->info.fatfile,
                       &dir_fatent, compp->name, compp->length);
found:
#endif

    if (callflags & FF_CHECKPREFIX)
        walk_check_prefix(walkp, compp);

    return WALK_RC_FOUND;
}

/* parse a path component, open it and process the next */
static NO_INLINE int
walk_path(struct pathwalk *walkp, struct pathwalk_component *compp,
          struct filestr_base *stream)
{
    int rc = WALK_RC_FOUND;

    if (walkp->callflags & FF_CHECKPREFIX)
        walk_check_prefix(walkp, compp);

    /* alloca is used in a loop, but we reuse any blocks previously allocated
       if we went up then back down; if the path takes us back to the root, then
       everything is cleaned automatically */
    struct pathwalk_component *freep = NULL;

    const char *name;
    ssize_t len;

    while ((len = parse_path_component(&walkp->path, &name)))
    {
        /* whatever is to be a parent must be a directory */
        if (!(compp->attr & ATTR_DIRECTORY))
            return -ENOTDIR;

        if (len > MAX_COMPNAME)
            return -ENAMETOOLONG;

        /* no filesystem is mounted here */
        if (walkp->callflags & FF_NOFS)
            return -ENOENT;

        /* check for "." and ".." */
        if (name[0] == '.')
        {
            if (len == 1)
                continue; /* is "." */

            if (len == 2 && name[1] == '.')
            {
                /* is ".." */
                struct pathwalk_component *parentp = compp->nextp;

                if (!parentp IF_MV( || parentp->attr == ATTR_SYSTEM_ROOT ))
                    return WALK_RC_CONT_AT_ROOT;

                compp->nextp = freep;
                freep = compp;
                compp = parentp;
                continue;
            }
        }

        struct pathwalk_component *newp = freep;
        if (!newp)
            newp = pathwalk_comp_alloc(compp);
        else
            freep = freep->nextp;

        newp->nextp = compp;
        compp = newp;

        compp->name = name;
        compp->length = len;

        rc = open_path_component(walkp, compp, stream);
        if (rc < 0)
            break; /* return info below */
    }

    return walk_open_info(walkp, compp, rc, stream);
}

/* open a stream given a path to the resource */
int open_stream_internal(const char *path, unsigned int callflags,
                         struct filestr_base *stream,
                         struct path_component_info *compinfo)
{
    DEBUGF("%s(path=\"%s\",flg=%X,str=%p,compinfo=%p)\n", __func__,
           path, callflags, stream, compinfo);
    int rc;

    filestr_base_init(stream);

    if (!path_is_absolute(path))
    {
        /* while this supports relative components, there is currently no
           current working directory concept at this level by which to
           fully qualify the path (though that would not be excessively
           difficult to add) */
        DEBUGF("\"%s\" is not an absolute path\n"
               "Only absolute paths currently supported.\n", path);
        FILE_ERROR(path ? ENOENT : EFAULT, -1);
    }

    /* if !compinfo then these cannot be returned anyway */
    if (!compinfo)
        callflags &= ~(FF_INFO | FF_PARENTINFO | FF_CHECKPREFIX);

    /* This lets it be passed quietly to directory scanning */
    stream->flags |= callflags & FF_MASK;

    struct pathwalk walk;
    walk.path      = path;
    walk.callflags = callflags;
    walk.compinfo  = compinfo;
    walk.filesize  = 0;

    struct pathwalk_component *rootp = pathwalk_comp_alloc(NULL);
    rootp->nextp = NULL;

    while (1)
    {
        rc = ns_parse_root(walk.path, &rootp->name, &rootp->length);
        if (rc < 0)
            break;

        rc = ns_open_root(IF_MV(rc,) &walk.callflags, &rootp->info, &rootp->attr);
        if (rc < 0)
            break;

        walk.path = rootp->name + rootp->length;

        rc = walk_path(&walk, rootp, stream);
        if (rc != WALK_RC_CONT_AT_ROOT)
            break;
    }

    if (rc >= 0)
    {
        /* FF_PROBE leaves nothing for caller to clean up */
        if (walk.callflags & FF_PROBE)
            filestr_base_destroy(stream);
    }
    else
    {
        /* utter, abject failure :`( */
        DEBUGF("Open failed: rc=%d, errno=%d\n", rc, errno);
        filestr_base_destroy(stream);
        FILE_ERROR(-rc, -1);
    }

file_error:
    return rc;
}

/* close the stream referenced by 'stream' */
int close_stream_internal(struct filestr_base *stream)
{
    int rc;
    unsigned int foflags = fileobj_get_flags(stream);

    if ((foflags & (FO_SINGLE|FO_REMOVED)) == (FO_SINGLE|FO_REMOVED))
    {
        /* nothing is referencing it so now remove the file's data */
        rc = fat_remove(&stream->infop->fatfile, FAT_RM_DATA);
        if (rc < 0)
        {
            DEBUGF("I/O error removing file data: %d\n", rc);
            FILE_ERROR(EIO, rc * 10 - 1);
        }
    }

    rc = 0;
file_error:
    /* close no matter what */
    fileop_onclose_internal(stream);
    return rc;
}

/* create a new stream in the parent directory */
int create_stream_internal(struct file_base_info *parentinfop,
                           const char *basename, size_t length,
                           unsigned int attr, unsigned int callflags,
                           struct filestr_base *stream)
{
    /* assumes an attempt was made beforehand to open *stream with
       open_stream_internal() which returned zero (successfully not found),
       so does not initialize it here */
    const char * const name = strmemdupa(basename, length);
    DEBUGF("Creating \"%s\"\n", name);

    struct file_base_info info;
    int rc = fat_create_file(&parentinfop->fatfile, name, attr,
                             &info.fatfile, get_dir_fatent_dircache());
    if (rc < 0)
    {
        DEBUGF("Create failed: %d\n", rc);
        FILE_ERROR(rc == FAT_RC_ENOSPC ? ENOSPC : EIO, rc * 10 - 1);
    }

    /* dir_fatent is implicit arg */
    fileop_oncreate_internal(stream, &info, callflags, parentinfop, name);
    rc = 0;
file_error:
    return rc;
}

/* removes files and directories - back-end to remove() and rmdir() */
int remove_stream_internal(const char *path, struct filestr_base *stream,
                           unsigned int callflags)
{
    /* Only FF_* flags should be in callflags */
    int rc;

    struct filestr_base opened_stream;
    if (!stream)
        stream = &opened_stream;

#ifdef HAVE_IPOD_UI
    /* ProkBox (#137): a link (FAT_ATTR_LINK, fat.h, inside the link
     * folder FAT_LINK_DIR) shares the chain of an original: removing it
     * drops its entry only. Anything else removes as upstream does. */
    bool link = false;
#endif
    if (stream == &opened_stream)
    {
        /* no stream provided so open local one */
#ifdef HAVE_IPOD_UI
        if (!(callflags & FF_DIR)
            && !strncasecmp(path, FAT_LINK_DIR "/",
                            sizeof(FAT_LINK_DIR "/") - 1))
        {
            /* a probe for the attributes first: the open below stays the
             * plain one (its info must not be the probe's) */
            struct path_component_info ci;
            link = open_stream_internal(path, callflags | FF_PROBE | FF_INFO,
                                        stream, &ci) >= 0
                   && (ci.attr & FAT_ATTR_LINK) == FAT_ATTR_LINK;
        }
#endif
        rc = open_stream_internal(path, callflags, stream, NULL);
        if (rc < 0)
        {
            DEBUGF("Failed opening path: %d\n", rc);
            FILE_ERROR(ERRNO, rc * 10 - 1);
        }
    }
    /* else ignore the 'path' argument */

    if (callflags & FF_DIR)
    {
        /* directory to be removed must be empty */
        rc = test_dir_empty_internal(stream);
        if (rc < 0)
            FILE_ERROR(ERRNO, rc * 10 - 2);
    }

    /* save old info since fat_remove() will destroy the dir info */
    struct file_base_info oldinfo = *stream->infop;
    rc = fat_remove(&stream->infop->fatfile, FAT_RM_DIRENTRIES);
    if (rc < 0)
    {
        DEBUGF("I/O error removing dir entries: %d\n", rc);
        FILE_ERROR(EIO, rc * 10 - 3);
    }

    fileop_onremove_internal(stream, &oldinfo);
#ifdef HAVE_IPOD_UI
    if (link)
        stream->infop->fatfile.firstcluster = 0;  /* the close frees none */
#endif

    rc = 0;
file_error:
    if (stream == &opened_stream)
    {
        /* will do removal of data below if this is the only reference */
        int rc2 = close_stream_internal(stream);
        if (rc2 < 0 && rc >= 0)
        {
            rc = rc2 * 10 - 4;
            DEBUGF("Success but failed closing stream: %d\n", rc);
        }
    }

    return rc;
}

#if defined(HAVE_IPOD_UI_DEVMODE) && !defined(BOOTLOADER)
/* ProkBox (#137, the park): drop a directory with whatever entries it
 * holds; frees the directory's own clusters, never a chain one of its
 * entries names (the links' chains are the originals'). Takes the
 * writer lock. 0 done, -1 absent or failed. */
int rmdir_shallow(const char *path)
{
    struct filestr_base stream;
    int rc;

    file_internal_lock_WRITER();
    rc = open_stream_internal(path, FF_DIR, &stream, NULL);
    if (rc >= 0)
    {
        struct file_base_info oldinfo = *stream.infop;
        int rc2;

        rc = fat_remove(&stream.infop->fatfile, FAT_RM_DIRENTRIES);
        if (rc >= 0)
            fileop_onremove_internal(&stream, &oldinfo);
        rc2 = close_stream_internal(&stream);   /* frees the dir's chain */
        if (rc >= 0)
            rc = rc2;
    }
    file_internal_unlock_WRITER();
    return rc < 0 ? -1 : 0;
}
#endif

/* test file/directory existence with constraints */
int test_stream_exists_internal(const char *path, unsigned int callflags)
{
    /* only FF_* flags should be in callflags */
    struct filestr_base stream;
    return open_stream_internal(path, callflags | FF_PROBE, &stream, NULL);
}

/* one-time init at startup */
void filesystem_init(void)
{
    mrsw_init(&file_internal_mrsw);
    dc_init();
    fileobj_mgr_init();
}
