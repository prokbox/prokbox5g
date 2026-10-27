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
 * iPod UI shell: video index pass (Phase 14a)
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

/* Contract: vixpass.h (docs/phase14a-brief.md 3.4). Tagcache thread, at
 * the end of lib_precompute() (or of its retry while only this pass is
 * owed), audio stopped. The decisions are in vixpass_core.c (host-tested,
 * tests/video/test_vixpass.c); this file feeds them and does the I/O.
 *
 * The ledger VIX_DIR "/vix.dat": a 16 B header { magic, version, count,
 * sum } and 'count' struct vixpass_ent records sorted by (id, mtime):
 *   id     vpc_id(): title, album, length and the filename seek, all in
 *          the RAM database. Not the path: with dircache compiled out
 *          (Phase 9) a file name is a disk read (tagcache_retrieve()),
 *          and the brief's point is no I/O for an indexed video. The
 *          filename seek changes when a file is renamed or moved (a new
 *          database entry), so such a video's old record goes unused.
 *   mtime  tag_mtime (== dir_get_info().mtime, what vplayer.c hashes)
 *   name   the .vix name's 64-bit hash (vix_name()'s 16 hex digits):
 *          what an orphan is unlinked by (vix_name() needs the path and
 *          the size, which the pass no longer has for a deleted file).
 *
 * A video whose (id, mtime) is in the ledger costs no I/O. A new or
 * changed one costs its file name (one tagcache read), one open for the
 * size, and then: the index of an unused record of the same name is taken
 * over (the file is the same, e.g. a rebuilt database); an index already
 * on disk is kept (one open; the player built it at open); otherwise
 * vix_build_file(). One that the demuxer refuses is recorded too (name of
 * the index it would have), so it is not retried every pass.
 *
 * Orphans (M2b): after the new and changed videos, every unused record
 * that names an index loses it (unless a live record names the same one)
 * and is dropped. A pass that has to wait first (playback started) writes
 * those records back with the resolved ones, so the next pass still
 * removes their indexes; nothing is lost to the disk. A missing or torn
 * ledger: one readdir of VIX_DIR, its .vix names kept for the pass (an
 * index already there is not rebuilt) and the unreferenced ones deleted.
 *
 * Scratch (the caller's block, pinned): the tagcache search and a path,
 * the old ledger, the new records, the pending entries, the directory's
 * names (torn only), then >= 4 KB for vix_build_file() (also the ledger
 * write's chunk buffer). Stack: ~100 B here plus vix_build_file()'s
 * ~700 B. */
#include "config.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "dir.h"
#include "audio.h"
#include "tagcache.h"
#include "debug.h"
#include "ipodui/anim.h"
#include "ipodui/library.h"
#include "ipodui/vixpass.h"
#include "ipodui/vixpass_core.h"
#include "video/vix_file.h"

#define LED_PATH    VIX_DIR "/vix.dat"
#define LED_TMP     VIX_DIR "/vix.tmp"
#define LED_YIELD   4096            /* RAM entries between yields */
#define LED_BUILD_MIN 4096          /* vix_build_file() scratch */

typedef char led_name_check[VPC_NAME_LEN == VIX_NAME_LEN ? 1 : -1];

/* What the scratch holds (offsets from its aligned start) */
struct vp {
    struct tagcache_search tcs;
    char path[MAX_PATH];
    char vix[MAX_PATH];
    struct vixpass_ent *old;        /* L records, sorted */
    uint8_t *used;                  /* L bits */
    struct vixpass_ent *cur;        /* the new ledger, n records */
    int32_t *pend;                  /* master index per cur record, or -1 */
    uint64_t *dir;                  /* D .vix names (torn ledger only) */
    char *work;
    size_t work_len;
    int nold, ncur, ndir, gen;
};

/* The entry's identity from the RAM database (never blocks) */
static uint32_t ent_id(const int32_t *s)
{
    return vpc_id(tagcache_ui_str(tag_title, s[tag_title]),
                  tagcache_ui_str(tag_album, s[tag_album]),
                  (uint32_t)s[tag_length], s[tag_filename]);
}

static void name_path(char *out, size_t cap, uint64_t name)
{
    snprintf(out, cap, "%s/%08lx%08lx.vix", VIX_DIR,
             (unsigned long)(uint32_t)(name >> 32), (unsigned long)(uint32_t)name);
}

/* The old ledger into v->old (room: 'cap' records); false = missing,
 * torn, or too big for the scratch (then rebuilt like a missing one) */
static bool led_read(struct vp *v, int cap)
{
    struct vpc_hdr h;
    int fd = open(LED_PATH, O_RDONLY);
    bool ok;

    v->nold = 0;
    if (fd < 0)
        return false;
    ok = read(fd, &h, sizeof(h)) == (ssize_t)sizeof(h)
        && vpc_hdr_ok(&h, (uint32_t)cap)
        && read(fd, v->old, h.count * sizeof(struct vixpass_ent))
           == (ssize_t)(h.count * sizeof(struct vixpass_ent))
        && vpc_sum(VPC_FNV_INIT, v->old, h.count) == h.sum;
    close(fd);
    if (ok)
        v->nold = h.count;
    return ok;
}

/* Torn or missing ledger: the .vix names in VIX_DIR (one readdir) */
static void dir_read(struct vp *v, int cap)
{
    struct dirent *e;
    DIR *d = opendir(VIX_DIR);

    v->ndir = 0;
    if (!d)
        return;
    while ((e = readdir(d)) && v->ndir < cap)
    {
        uint64_t n;

        if (vpc_dir_name(e->d_name, &n))
            v->dir[v->ndir++] = n;
    }
    closedir(d);
}

/* Writes the resolved records and 'nkeep' unused old ones still naming an
 * index (a pass that waited), merged in order, in chunks through the work
 * buffer (.tmp, then renamed); false on an error */
static bool led_write(struct vp *v, int nkeep)
{
    struct vpc_hdr h;
    struct vixpass_ent *buf = (struct vixpass_ent *)v->work;
    int n, fd, ia = 0, ib = 0, chunk;
    int bcap = (int)(v->work_len / sizeof(struct vixpass_ent));
    bool ok;

    vpc_sort(v->cur, v->pend, v->ncur);
    n = vpc_keep_resolved(v->cur, v->pend, v->ncur);
    mkdir(ROCKBOX_DIR "/ipodui");
    mkdir(VIX_DIR);
    fd = open(LED_TMP, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return false;
    h.magic = VPC_MAGIC;
    h.version = VPC_VERSION;
    h.count = n + nkeep;
    h.sum = VPC_FNV_INIT;
    /* the header again once the sum is known */
    ok = write(fd, &h, sizeof(h)) == (ssize_t)sizeof(h);
    while (ok && (chunk = vpc_merge(v->cur, n, &ia, v->old, nkeep, &ib, buf,
                                    bcap)) > 0)
    {
        h.sum = vpc_sum(h.sum, buf, chunk);
        ok = write(fd, buf, chunk * sizeof(*buf))
             == (ssize_t)(chunk * sizeof(*buf));
    }
    ok = ok && lseek(fd, 0, SEEK_SET) == 0
        && write(fd, &h, sizeof(h)) == (ssize_t)sizeof(h);
    ok = close(fd) == 0 && ok;
    /* no rename onto an existing name (crash-fat 1019): a cut after the
     * remove leaves no ledger, which led_read() rebuilds from VIX_DIR */
    ok = ok && (remove(LED_PATH) == 0 || errno == ENOENT)
         && rename(LED_TMP, LED_PATH) == 0;
    if (!ok)
        remove(LED_TMP);
    UPDATE_TRACE("vixpass: ledger write n=%d kept-unused=%d ok=%d", n, nkeep,
                 (int)ok);
    return ok;
}

/* Resolves pending record k (master index v->pend[k]): its path, size,
 * name, and its index unless one is there already. False if the
 * database changed meanwhile (the reads may yield). */
static bool resolve(struct vp *v, int k, int *built)
{
    struct vixpass_ent *e = &v->cur[k];
    char hex[VIX_NAME_LEN + 1];
    uint64_t size, name = 0;
    long rc UNUSED_ATTR = 0;
    const char *how UNUSED_ATTR;
    int32_t idx = v->pend[k];
    int fd;

    v->pend[k] = -1;
    e->name = 0;                        /* gone: recorded, never built */
    if (!tagcache_retrieve(&v->tcs, idx, tag_filename, v->path,
                           sizeof(v->path)))
        return tagcache_ui_generation() == v->gen;
    fd = open(v->path, O_RDONLY);
    if (fd < 0)
    {
        UPDATE_TRACE("vixpass: open failed %s", v->path);
        return tagcache_ui_generation() == v->gen;
    }
    size = (uint64_t)ffilesize(fd);
    close(fd);
    vix_name(v->path, size, e->mtime, hex);
    vpc_hex_name(hex, &name);
    if (vpc_claim_name(v->old, v->nold, v->used, name) >= 0)
        how = "reused";                 /* same file, new identity */
    else if (v->ndir ? vpc_has_name64(v->dir, v->ndir, name)
             : (name_path(v->vix, sizeof(v->vix), name),
                file_exists(v->vix)))
        how = "kept";
    else
    {
        rc = vix_build_file(v->path, VIX_DIR, size, e->mtime, v->work,
                            v->work_len);
        ++*built;
        how = "built";
    }
    UPDATE_TRACE("vixpass: %s %s rc=%ld", how, v->path, rc);
    e->name = name;
    return tagcache_ui_generation() == v->gen;
}

int vixpass_run(void *scratch, size_t size, bool (*stop)(void))
{
    uintptr_t base = ALIGN_UP((uintptr_t)scratch, 8);
    size_t room = size - (base - (uintptr_t)scratch), off;
    struct vp *v = (struct vp *)base;
    int total = tagcache_ui_count(), nvid = 0, i, cap, steps = 0, nkeep = 0;
    int built = 0, removed UNUSED_ATTR = 0, kept UNUSED_ATTR = 0;
    bool torn, dirty = false, waited = false, tcs_open = false;
    unsigned long t0 UNUSED_ATTR = current_tick;

    if (total < 0 || room < sizeof(*v) + LED_BUILD_MIN + 64)
        return LIB_PRE_DONE;
    memset(v, 0, sizeof(*v));
    v->gen = tagcache_ui_generation();

    /* the videos in RAM (no yield: flags only, ~1 ms at 50k in the sim) */
    for (i = 0; i < total; i++)
    {
        int32_t flag;

        if (tagcache_ui_seeks(i, &flag) && (flag & TAGCACHE_UI_VIDEO))
            nvid++;
    }

    /* layout: old ledger (as much as fits beside the rest), its bits,
     * cur + pend (nvid each), dir names (torn), work */
    off = ALIGN_UP(sizeof(*v), 8);
    {
        size_t fixed = off + (size_t)nvid * (sizeof(struct vixpass_ent) + 4)
                     + LED_BUILD_MIN + 64;

        if (fixed > room)
        {
            UPDATE_TRACE("vixpass: no room videos=%d bytes=%lu", nvid,
                         (unsigned long)room);
            return LIB_PRE_DONE;        /* the player builds at open */
        }
        /* each old record costs 16 B + 1 bit; keep half the slack for
         * the build's scratch */
        cap = (int)((room - fixed) / 2 / (sizeof(struct vixpass_ent) + 1));
    }
    v->old = (struct vixpass_ent *)(base + off);
    off += (size_t)cap * sizeof(struct vixpass_ent);
    v->used = (uint8_t *)(base + off);
    off = ALIGN_UP(off + (size_t)(cap + 7) / 8, 8);
    v->cur = (struct vixpass_ent *)(base + off);
    off += (size_t)nvid * sizeof(struct vixpass_ent);
    v->pend = (int32_t *)(base + off);
    off = ALIGN_UP(off + (size_t)nvid * 4, 8);

    torn = !led_read(v, cap);
    memset(v->used, 0, (size_t)(cap + 7) / 8);
    if (torn)
    {
        /* the old ledger's room holds the directory's names instead */
        v->dir = (uint64_t *)v->old;
        dir_read(v, cap * 2);
        dirty = true;
    }
    v->work = (char *)(base + off);
    v->work_len = room - off;
    UPDATE_TRACE("vixpass: start videos=%d ledger=%d torn=%d dir=%d",
                 nvid, v->nold, (int)torn, v->ndir);

    /* each video: in the ledger (no I/O) or pending */
    for (i = 0; i < total && v->ncur < nvid; i++)
    {
        const int32_t *s;
        int32_t flag;
        uint32_t id, mt;

        if (++steps == LED_YIELD)
        {
            steps = 0;
            yield();
            if (tagcache_ui_generation() != v->gen || tagcache_ui_count() < 0)
                return LIB_PRE_WAIT;    /* a new load precomputes again */
        }
        s = tagcache_ui_seeks(i, &flag);
        if (!s || !(flag & TAGCACHE_UI_VIDEO))
            continue;
        id = ent_id(s);
        mt = (uint32_t)s[tag_mtime];
        v->cur[v->ncur].id = id;
        v->cur[v->ncur].mtime = mt;
        /* copies (same tags, path and mtime) take a record each */
        {
            int k = vpc_match(v->old, v->nold, v->used, id, mt);

            v->cur[v->ncur].name = k >= 0 ? v->old[k].name : 0;
            v->pend[v->ncur] = k >= 0 ? -1 : i;
            if (k >= 0)
                kept++;
        }
        v->ncur++;
    }

    /* the new and changed ones (they may take over an unused record's
     * index): only with audio stopped, checked right before each file */
    for (i = 0; !waited && i < v->ncur; i++)
    {
        if (v->pend[i] < 0)
            continue;
        if ((stop && stop()) || audio_status() != 0)
        {
            waited = true;
            break;
        }
        if (!tcs_open && !(tcs_open = tagcache_search(&v->tcs,
                                                      tag_filename)))
        {
            waited = true;
            break;
        }
        dirty = true;
        if (!resolve(v, i, &built))
        {
            tagcache_search_finish(&v->tcs);
            return LIB_PRE_WAIT;        /* the database changed */
        }
        yield();
    }
    if (tcs_open)
        tagcache_search_finish(&v->tcs);

    /* the ledger's videos that are gone, changed, renamed or moved: their
     * index too, re-checking the audio first (the loops above yielded) */
    if (!waited && ((stop && stop()) || audio_status() != 0))
        waited = true;
    for (i = 0; !waited && i < v->nold; i++)
        if (!VPC_USED(v->used, i) && v->old[i].name)
        {
            if (!vpc_has_name(v->cur, v->ncur, v->old[i].name))
            {
                name_path(v->vix, sizeof(v->vix), v->old[i].name);
                remove(v->vix);
                removed++;
            }
            dirty = true;
        }
    if (waited && !torn && dirty)
        /* still owed: kept in the ledger for the next pass */
        nkeep = vpc_keep_unused(v->old, v->used, v->nold);

    /* a torn ledger: the directory's indexes nobody references go */
    if (torn && !waited)
    {
        int j;

        for (j = 0; j < v->ndir; j++)
            if (!vpc_has_name(v->cur, v->ncur, v->dir[j]))
            {
                name_path(v->vix, sizeof(v->vix), v->dir[j]);
                remove(v->vix);
                removed++;
            }
    }
    /* a torn ledger stays torn until its directory check ran */
    if (dirty && !(torn && waited))
        led_write(v, nkeep);
    UPDATE_TRACE("vixpass: %s videos=%d kept=%d built=%d removed=%d "
                 "ticks=%ld", waited ? "wait" : "done", nvid, kept, built,
                 removed, (long)(current_tick - t0));
    return waited ? LIB_PRE_WAIT : LIB_PRE_DONE;
}
