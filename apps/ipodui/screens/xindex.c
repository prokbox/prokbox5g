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

/* iPod UI shell: the MOD_XINDEX module, the sync-time Notes builder (P12
 * W1-N, docs/phase12-brief.md 3.5, 8 row 13). Runs only off-screen, in a
 * sync window, on the storage worker (or inline where there is none),
 * inside the driver's maint_take() block (xindex.c); its manifest says
 * context = sync, the one shell module allowed a directory walk.
 *
 * One pass:
 *  1. the last index, read whole into the block and checked (header and
 *     CRC): its signature;
 *  2. the walk: /Notes's entries (notes "*.txt" and folders), then each
 *     folder's notes, in directory order, gathered into the block and
 *     summed into the signature (name, size, mtime; a folder adds its
 *     name only);
 *  3. equal signature and a sound index: fresh, nothing written;
 *     otherwise the index is laid out (xnote_emit) and replaces the old
 *     one through safefile (tmp, fsync, rename).
 * The block (XIDX_BUILD_BLK): gathered entries | sort scratch | name
 * pool | the index being written. No allocation, no boost of its own (the
 * driver holds one around the job), no screen. */
#include "config.h"
#include <stdio.h>
#include <string.h>
#include "system.h"
#include "file.h"
#include "dir.h"
#include "pathfuncs.h"
#include "usb.h"
#include "kernel.h"
#include "ipodui/sortkey.h"
#include "ipodui/safefile.h"
#include "ipodui/xindex.h"
#include "ipodui/xcontact_core.h"

#define B_SRC   0
#define B_SCR   (B_SRC + XNOTE_MAX * sizeof(struct xnote_src))
#define B_POOL  ((B_SCR + XNOTE_SCRATCH(XNOTE_MAX) * 2 + 15) & ~15)
#define B_OUT   ((B_POOL + XNOTE_POOL + 15) & ~15)
#define OUT_MAX XIDX_FILE_MAX           /* Notes: its first 96 KB */
_Static_assert(B_OUT + OUT_MAX <= XIDX_BUILD_BLK, "Notes fits the block");
_Static_assert(OUT_MAX >= 40 + (8 + 4 + 12 + 3) * (XNOTE_MAX + 1)
                         + XNOTE_POOL, "the largest Notes index fits");

struct walk {
    struct xidx_job *j;
    struct xnote_src *src;
    char *pool;
    uint32_t pool_used, sig;
    int n;
};

/* Collation for the layout: ui_sortkey() of the display names (the
 * list's order), then the bytes, so equal keys stay in a fixed order */
#define KEY_NAME 64
static int name_cmp(const char *a, int alen, const char *b, int blen)
{
    char na[KEY_NAME], nb[KEY_NAME], ka[KEY_NAME], kb[KEY_NAME];
    int r;

    alen = MIN(alen, KEY_NAME - 1);
    blen = MIN(blen, KEY_NAME - 1);
    memcpy(na, a, alen);
    memcpy(nb, b, blen);
    na[alen] = nb[blen] = '\0';
    ui_sortkey(na, ka, sizeof(ka));
    ui_sortkey(nb, kb, sizeof(kb));
    r = strcmp(ka, kb);
    if (!r)
        r = strcmp(na, nb);
    return r;
}

static bool stopped(const struct walk *w)
{
    return *w->j->stop || usb_inserted();
}

/* One folder's entries: folders only in the root (one level, stock) */
static int walk_dir(struct walk *w, const char *path, uint16_t parent)
{
    struct dirent *e;
    DIR *d = opendir(path);

    if (!d)
        return XIDX_JOB_FAIL;
    while ((e = readdir(d)) != NULL)
    {
        struct dirinfo info;
        bool dir;
        uint32_t len;

        if (stopped(w))
        {
            closedir(d);
            return XIDX_JOB_STOPPED;
        }
        if (e->d_name[0] == '.')        /* dot dirs, "._" sidecars */
            continue;
        info = dir_get_info(d, e);
        dir = (info.attribute & ATTR_DIRECTORY) != 0;
        if (dir ? parent != XNOTE_PARENT_ROOT : !xnote_is_note(e->d_name))
            continue;
        /* the signature sees every entry, also the ones a cap leaves out */
        w->sig = xidx_sig_add(w->sig, e->d_name,
                              dir ? 0 : (uint32_t)info.size,
                              dir ? 0 : (uint32_t)info.mtime);
        len = strlen(e->d_name) + 1;
        if (w->n >= XNOTE_MAX || w->pool_used + len > XNOTE_POOL)
        {
            w->j->cut = 1;
            continue;
        }
        w->src[w->n].name = w->pool_used;
        w->src[w->n].size = dir ? 0 : (uint32_t)info.size;
        w->src[w->n].parent = parent;
        w->src[w->n].dir = dir;
        memcpy(w->pool + w->pool_used, e->d_name, len);
        w->pool_used += len;
        w->n++;
    }
    closedir(d);
    return XIDX_JOB_BUILT;
}

/* The last index's signature, if it is whole and sound */
static bool last_sig(uint8_t *buf, uint32_t buf_len, const char *idx,
                     int kind, uint32_t *sig)
{
    const struct xidx_hdr *h = (const struct xidx_hdr *)buf;
    int fd = open(idx, O_RDONLY);
    bool ok = false;
    off_t size;

    if (fd < 0)
        return false;
    size = ffilesize(fd);
    if (size >= (off_t)sizeof(*h) && size <= (off_t)xidx_file_max(kind)
        && size <= (off_t)buf_len && read(fd, buf, size) == size
        && xidx_check_body(buf, (uint32_t)size) == XIDX_OK
        && h->kind == kind)
    {
        *sig = h->sig;
        ok = true;
    }
    close(fd);
    return ok;
}

static bool write_index(const uint8_t *out, int32_t bytes, const char *idx,
                        const char *tmp, const char *prev)
{
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    bool ok;

    if (fd < 0)
        return false;
    ok = write(fd, out, bytes) == bytes;
    ok = fsync(fd) == 0 && ok;
    ok = close(fd) == 0 && ok;
    if (ok && safefile_replace(tmp, idx, prev) == 0)
        return true;
    remove(tmp);
    return false;
}

static int build_notes(struct xidx_job *j)
{
    struct walk w;
    char path[MAX_PATH];
    uint32_t old = 0;
    bool have_old;
    int rc, i, nroot;
    int32_t bytes;

    if (j->blk_len < XIDX_BUILD_BLK)
        return XIDX_JOB_FAIL;
    j->n = j->bytes = 0;
    j->cut = j->made_dir = 0;
    /* a replace cut by a power loss is finished first */
    safefile_recover(XNOTES_IDX, XNOTES_TMP, XNOTES_PREV);
    have_old = last_sig(j->blk, j->blk_len, XNOTES_IDX, XIDX_NOTES, &old);

    w.j = j;
    w.src = (struct xnote_src *)(j->blk + B_SRC);
    w.pool = (char *)j->blk + B_POOL;
    w.pool_used = 0;
    w.sig = XIDX_SIG_INIT;
    w.n = 0;
    if (!dir_exists(XNOTE_ROOT_DIR))
    {
        /* Q12: the folder a user drops notes into is always there */
        if (mkdir(XNOTE_ROOT_DIR) == 0)
            j->made_dir = 1;
        rc = XIDX_JOB_BUILT;            /* empty either way */
    }
    else
        rc = walk_dir(&w, XNOTE_ROOT_DIR, XNOTE_PARENT_ROOT);
    nroot = w.n;
    for (i = 0; rc == XIDX_JOB_BUILT && i < nroot; i++)
    {
        if (!w.src[i].dir)
            continue;
        if (snprintf(path, sizeof(path), XNOTE_ROOT_DIR "/%s",
                     w.pool + w.src[i].name) >= (int)sizeof(path))
            continue;                   /* a path past MAX_PATH: unread */
        rc = walk_dir(&w, path, i);
        if (rc == XIDX_JOB_FAIL)
            rc = XIDX_JOB_BUILT;        /* gone since: its notes are not */
    }
    if (rc != XIDX_JOB_BUILT)
        return rc;
    j->n = w.n;
    if (have_old && old == w.sig)
        return XIDX_JOB_FRESH;
    bytes = xnote_emit(j->blk + B_OUT, OUT_MAX, w.src, w.n, w.pool, w.sig,
                       name_cmp, (uint16_t *)(j->blk + B_SCR));
    if (bytes < 0)
        return XIDX_JOB_FAIL;
    j->bytes = bytes;
    if (stopped(&w))
        return XIDX_JOB_STOPPED;
    return write_index(j->blk + B_OUT, bytes, XNOTES_IDX, XNOTES_TMP,
                       XNOTES_PREV) ? XIDX_JOB_BUILT
                                               : XIDX_JOB_FAIL;
}

/* -- Contacts (W3-K) ---------------------------------------------------------
 * The same job for /Contacts's ".vcf" files (flat): the walk's signature
 * (name, size, mtime of each) against contacts.idx; on a difference every
 * file is parsed in directory order (xcontact_core.c) and the index laid
 * out and replaced through safefile. The block (XIDX_BUILD_BLK): gathered
 * cards | string pool | sort scratch | parser state | the index being
 * written (XIDX_CONTACTS_FILE_MAX); the read chunk is the index's own
 * space (nothing is laid out until every file is read). 2,500 cards: 50 KB
 * of cards, 192 KB of pool, 20 KB of sort scratch, 256 KB of index. */
#define C_SRC   0
#define C_POOL  ((C_SRC + XCONTACT_MAX * sizeof(struct xc_src) + 15) & ~15)
#define C_SCR   ((C_POOL + XC_POOL + 15) & ~15)
#define C_XCB   ((C_SCR + XCONTACT_SCRATCH(XCONTACT_MAX) * 2 + 15) & ~15)
#define C_OUT   ((C_XCB + sizeof(struct xcbuild) + 15) & ~15)
#define C_OUT_MAX XIDX_CONTACTS_FILE_MAX
#define C_CHUNK 8192
#define C_PARSE_MAX (4u * 1024 * 1024)  /* bytes read per build */
#define C_CUT_MORE (64u * 1024)         /* read past the first cut */
_Static_assert(C_OUT + C_OUT_MAX <= XIDX_BUILD_BLK, "contacts: the block");

static void yield_tick(void)
{
    yield();
}

/* The two key texts compare as the lists do: ui_sortkey, then the bytes */
static int key_cmp(const char *a, const char *b)
{
    char ka[2 * KEY_NAME], kb[2 * KEY_NAME];
    int r;

    ui_sortkey(a, ka, sizeof(ka));
    ui_sortkey(b, kb, sizeof(kb));
    r = strcmp(ka, kb);
    return r ? r : strcmp(a, b);
}

/* One pass over the folder: the signature, or (parse) every file's cards */
static int contacts_walk(struct xidx_job *j, struct xcbuild *b, bool parse,
                         uint32_t *sig)
{
    struct dirent *e;
    DIR *d = opendir(XCONTACT_ROOT_DIR);
    uint8_t *chunk = j->blk + C_OUT;
    char path[MAX_PATH];
    uint32_t total = 0, after_cut = 0;

    if (!d)
        return XIDX_JOB_FAIL;
    while ((e = readdir(d)) != NULL)
    {
        struct dirinfo info;
        int fd;
        ssize_t got;

        if (*j->stop || usb_inserted())
        {
            closedir(d);
            return XIDX_JOB_STOPPED;
        }
        info = dir_get_info(d, e);
        if ((info.attribute & ATTR_DIRECTORY) || !xcontact_is_file(e->d_name))
            continue;
        if (!parse)
        {
            *sig = xidx_sig_add(*sig, e->d_name, (uint32_t)info.size,
                                (uint32_t)info.mtime);
            continue;
        }
        if (total >= C_PARSE_MAX || after_cut >= C_CUT_MORE
            || snprintf(path, sizeof(path), XCONTACT_ROOT_DIR "/%s",
                        e->d_name) >= (int)sizeof(path))
            continue;
        fd = open(path, O_RDONLY);
        if (fd < 0)
            continue;
        while (total < C_PARSE_MAX && after_cut < C_CUT_MORE
               && (got = read(fd, chunk, C_CHUNK)) > 0)
        {
            xcb_feed(b, chunk, got);
            total += got;
            if (b->cut)
                after_cut += got;
            if (*j->stop || usb_inserted())
            {
                close(fd);
                closedir(d);
                return XIDX_JOB_STOPPED;
            }
        }
        close(fd);
        xcb_end_file(b);
    }
    closedir(d);
    return XIDX_JOB_BUILT;
}

static int build_contacts(struct xidx_job *j)
{
    struct xcbuild *b = (struct xcbuild *)(j->blk + C_XCB);
    uint32_t old = 0, sig = XIDX_SIG_INIT;
    bool have_old, exists = true;
    int rc;
    int32_t bytes;

    if (j->blk_len < XIDX_BUILD_BLK)
        return XIDX_JOB_FAIL;
    j->n = j->bytes = 0;
    j->cut = j->made_dir = 0;
    safefile_recover(XCONTACTS_IDX, XCONTACTS_TMP, XCONTACTS_PREV);
    have_old = last_sig(j->blk, j->blk_len, XCONTACTS_IDX, XIDX_CONTACTS,
                        &old);
    if (!dir_exists(XCONTACT_ROOT_DIR))
    {
        /* Q12: the folder a user drops contacts into is always there */
        if (mkdir(XCONTACT_ROOT_DIR) == 0)
            j->made_dir = 1;
        exists = false;                 /* empty either way */
    }
    else
    {
        rc = contacts_walk(j, b, false, &sig);
        if (rc != XIDX_JOB_BUILT)
            return rc == XIDX_JOB_FAIL ? XIDX_JOB_BUILT : rc;
    }
    if (have_old && old == sig)
        return XIDX_JOB_FRESH;
    xcb_init(b, (struct xc_src *)(j->blk + C_SRC),
             XCONTACT_MAX, (char *)j->blk + C_POOL, XC_POOL,
             C_OUT_MAX - 64, yield_tick);
    if (exists && contacts_walk(j, b, true, &sig) == XIDX_JOB_STOPPED)
        return XIDX_JOB_STOPPED;
    bytes = xcb_emit(b, j->blk + C_OUT, C_OUT_MAX, sig, key_cmp,
                     (uint16_t *)(j->blk + C_SCR));
    j->n = b->n;
    j->cut = b->cut;
    if (bytes < 0)
        return XIDX_JOB_FAIL;
    j->bytes = bytes;
    if (*j->stop || usb_inserted())
        return XIDX_JOB_STOPPED;
    return write_index(j->blk + C_OUT, bytes, XCONTACTS_IDX, XCONTACTS_TMP,
                       XCONTACTS_PREV) ? XIDX_JOB_BUILT : XIDX_JOB_FAIL;
}

int xcal_build_job(struct xidx_job *j);    /* screens/xical_build.c */

const struct xidx_ops xidx_ops = {
    .notes = build_notes,
    .contacts = build_contacts,
    .cal   = xcal_build_job,
};
