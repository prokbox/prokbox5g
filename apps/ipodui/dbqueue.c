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

/* Queue by database index (#109): the runtime. See dbqueue.h and
 * docs/design/queue-by-index.md (sections 3-5). */
#include "config.h"
#include <stdio.h>
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "xcore.h"                  /* P19 S0: XCORE_BSS */
#include "file.h"
#include "dir.h"
#include "core_alloc.h"
#include "logf.h"
#include "debug.h"
#include "audio.h"
#include "tagcache.h"
#include "playlist.h"
#include "rbpaths.h"
#include "ipodui/anim.h"
#include "ipodui/safefile.h"
#include "ipodui/maint.h"
#include "ipodui/dbqueue.h"

#define DBQ_DIR         ROCKBOX_DIR "/ipodui"
#define DBQ_HDR         DBQ_DIR "/dbq.hdr"
#define DBQ_HDR_TMP     DBQ_HDR "_temp"
#define DBQ_HDR_PREV    DBQ_HDR "_prev"
#define DBQ_PATHS       DBQ_DIR "/dbqpath.dat"
#define DBQ_PATHS_TMP   DBQ_PATHS "_temp"
#define DBQ_PATHS_PREV  DBQ_PATHS "_prev"

/* upstream tagcache's files and on-disk layout (tagcache.c:208-231,
 * 450-474): a master header of 6 words, then one index entry per track
 * (TAG_COUNT seeks and a flag word); a tag file entry is {length, idx}
 * then the name */
#define TC_MASTER       "database_idx.tcd"
#define TC_NAMES        "database_4.tcd"        /* tag_filename */
#define TC_MASTER_HDR   (6 * sizeof(int32_t))
#define TC_ENTRY        ((TAG_COUNT + 1) * sizeof(int32_t))
#define TC_DELETED      0x0001

#define DBQ_CHUNK       (32 * 1024)     /* one sequential pass's reads */
#define DBQ_OUTBUF      (4 * 1024)      /* dbqpath.dat's writes */

/* The current queue's slot table (movable core_alloc): q[cap], then the
 * album-group bits and a scratch bitmap of cap/8+1 bytes each */
struct dbq_live {
    uint32_t seq;           /* 0 until dbq_commit() */
    uint32_t count;
    uint32_t cap;
    uint32_t split;         /* IDX -> SEEK in place: slots below are SEEK */
    uint8_t  mode;
    uint8_t  file;
    uint8_t  pad[2];
    struct dbq_stamp st;
    uint32_t q[];
};

/* Locks (review M-B): persist_mtx -> dbq_mtx; playlist -> persist_mtx
 * (resume) and playlist -> dbq_mtx (the resolver). dbq_mtx guards the
 * table and is never held across I/O; persist_mtx serializes every dbq
 * file write and the resume's reads, and holds nothing the audio thread
 * waits on. A table being written (busy_h) is pinned; a reset meanwhile
 * leaves it to the writer to free (orphan). */
/* P19 S0: in uncached shared memory, so a front end on the COP may take
 * them (design section 3.2; xcore.h) */
static struct mutex dbq_mtx XCORE_BSS, persist_mtx XCORE_BSS;
static int live_h;                  /* 0: no table */
static int busy_h;                  /* the table a write reads from */
static bool orphan;                 /* ... reset meanwhile: free it after */
static uint32_t hdr_next_seq = 1;   /* persist_mtx */
static uint32_t hdr_total;
static bool dbq_booted;
static bool boot_block;             /* dbq.hdr's e[0] is still this boot's
                                       queue (no new queue since) */
static volatile bool idx_live, rebase_owed, mat_owed, check_owed;
static struct tagcache_context *old_ctx;    /* passive, publish hook */
static long miss_tick;
static uint32_t miss_seq;

static inline struct dbq_live *live(void)
{
    return live_h > 0 ? core_get_data(live_h) : NULL;
}

static inline size_t bits_len(uint32_t cap)
{
    return cap / 8 + 1;
}

size_t dbq_need(int m)
{
    return m <= 0 ? 0 : sizeof(struct dbq_live) + (size_t)m * 4
                        + 2 * bits_len(m);
}

static bool hdr_read(struct dbq_hdr *h)
{
    int fd = open(DBQ_HDR, O_RDONLY);
    bool ok = false;

    if (fd >= 0)
    {
        ok = read(fd, h, sizeof(*h)) == (ssize_t)sizeof(*h)
             && dbq_hdr_check(h);
        close(fd);
    }
    if (!ok)
        memset(h, 0, sizeof(*h));
    return ok;
}

/* dbq.hdr by a safe replace (16-3), never in place. dbq mutex held. */
static bool hdr_write(struct dbq_hdr *h)
{
    int fd;
    bool ok;

    dbq_hdr_seal(h);
    fd = open(DBQ_HDR_TMP, O_CREAT|O_TRUNC|O_WRONLY, 0666);
    if (fd < 0)
        return false;
    ok = write(fd, h, sizeof(*h)) == (ssize_t)sizeof(*h) && fsync(fd) >= 0;
    close(fd);
    if (ok && safefile_replace(DBQ_HDR_TMP, DBQ_HDR, DBQ_HDR_PREV) == 0)
    {
        hdr_next_seq = h->next_seq;
        return true;
    }
    safefile_remove(DBQ_HDR_TMP, DBQ_HDR);
    return false;
}

/* One block into dbq<file>.dat from offset 0 (O_TRUNC). vals pinned. */
static bool payload_write(unsigned file, uint32_t seq, uint32_t mode,
                          const uint32_t *vals, uint32_t n)
{
    char name[sizeof(DBQ_DIR "/dbq0.dat")];
    struct dbq_block b;
    ssize_t bytes = n * sizeof(uint32_t);
    int fd;
    bool ok;

    snprintf(name, sizeof(name), DBQ_DIR "/dbq%u.dat", file & 1);
    dbq_block_seal(&b, seq, n, mode, vals);
    fd = open(name, O_CREAT|O_TRUNC|O_WRONLY, 0666);
    if (fd < 0 && mkdir(DBQ_DIR) == 0)     /* the first queue ever */
        fd = open(name, O_CREAT|O_TRUNC|O_WRONLY, 0666);
    if (fd < 0)
        return false;
    ok = write(fd, &b, sizeof(b)) == (ssize_t)sizeof(b)
         && write(fd, vals, bytes) == bytes && fsync(fd) >= 0;
    close(fd);
    return ok;
}

static bool payload_read(const struct dbq_entry *e, uint32_t *vals)
{
    char name[sizeof(DBQ_DIR "/dbq0.dat")];
    struct dbq_block b;
    ssize_t bytes = e->count * sizeof(uint32_t);
    int fd;
    bool ok;

    snprintf(name, sizeof(name), DBQ_DIR "/dbq%u.dat", e->file & 1);
    fd = open(name, O_RDONLY);
    if (fd < 0)
        return false;
    ok = lseek(fd, e->file_off, SEEK_SET) == (off_t)e->file_off
         && read(fd, &b, sizeof(b)) == (ssize_t)sizeof(b)
         && read(fd, vals, bytes) == bytes
         && dbq_block_check(&b, vals, e->seq, e->count) && b.mode == e->mode;
    close(fd);
    return ok;
}

/* The stamp's directory still holds that generation: its master header
 * names the commit id and the entry count (one small read). */
static bool master_holds(const struct dbq_stamp *st, char *scratch)
{
    int32_t mh[6];
    int fd;
    bool ok = false;

    snprintf(scratch, MAX_PATH + 1, "%s/" TC_MASTER, st->db_path);
    fd = open(scratch, O_RDONLY);
    if (fd < 0)
        return false;
    if (read(fd, mh, sizeof(mh)) == (ssize_t)sizeof(mh))
        ok = mh[4] == st->commitid && mh[2] == st->entries;
    close(fd);
    return ok;
}

static bool stamp_of(struct tagcache_context *ctx, struct dbq_stamp *st)
{
    const char *p = tagcache_ui_db_path_ctx(ctx);

    memset(st, 0, sizeof(*st));
    st->commitid = tagcache_ui_commitid_ctx(ctx);
    st->entries = tagcache_ui_count_ctx(ctx);
    if (!p || strlen(p) >= DBQ_PATH_LEN)
        return false;
    strcpy(st->db_path, p);
    return true;
}

/* -- boot, reset ------------------------------------------------------ */

void dbq_init(void)
{
    struct dbq_hdr h;
    uint32_t i;

    mutex_init(&dbq_mtx);
    mutex_init(&persist_mtx);
    safefile_recover(DBQ_HDR, DBQ_HDR_TMP, DBQ_HDR_PREV);
    safefile_recover(DBQ_PATHS, DBQ_PATHS_TMP, DBQ_PATHS_PREV);
    if (hdr_read(&h))
    {
        hdr_next_seq = h.next_seq ? h.next_seq : 1;
        for (i = 0; i < h.nblocks; i++)
            if (h.e[i].count > hdr_total)
                hdr_total = h.e[i].count;
        if (h.nblocks > 0)
        {
            boot_block = true;
            /* e[0] is the queue a resume replays: its generation is
             * checked, or its materialization finished, on the tagcache
             * thread before any new one is prepared (4.2.4, M3) */
            if (h.e[0].mode == DBQ_MODE_IDX)
                check_owed = idx_live = true;
            else if (h.e[0].mode == DBQ_MODE_SEEK)
                mat_owed = true;
        }
    }
    dbq_booted = true;
    UPDATE_TRACE("dbq boot blocks=%lu total=%lu owed=%d", (unsigned long)
                 h.nblocks, (unsigned long)hdr_total,
                 (int)check_owed | (int)mat_owed << 1);
}

uint32_t dbq_total(void)
{
    return hdr_total;
}

/* dbq_mtx held */
static void drop_table(void)
{
    if (live_h > 0)
    {
        if (live_h == busy_h)
            orphan = true;          /* its writer frees it */
        else
            core_free(live_h);
    }
    live_h = 0;
}

void dbq_reset(void)
{
    mutex_lock(&dbq_mtx);
    drop_table();
    /* dbq.hdr's rows belong to a queue nobody plays now: no conversion
     * is owed for them (a new queue writes its own) */
    boot_block = false;
    idx_live = false;
    check_owed = false;
    mat_owed = false;
    mutex_unlock(&dbq_mtx);
}

bool dbq_queue_live(void)
{
    return live_h > 0;
}

/* The table with that seq, pinned for a write outside dbq_mtx; NULL if it
 * is gone. Release with table_put(). persist_mtx held. */
static struct dbq_live *table_get(uint32_t seq)
{
    struct dbq_live *l;

    mutex_lock(&dbq_mtx);
    l = live();
    if (l && l->seq == seq && busy_h == 0)
    {
        busy_h = live_h;
        orphan = false;
        core_pin(busy_h);
        l = core_get_data(busy_h);
    }
    else
        l = NULL;
    mutex_unlock(&dbq_mtx);
    return l;
}

static void table_put(void)
{
    mutex_lock(&dbq_mtx);
    if (busy_h > 0)
    {
        core_unpin(busy_h);
        if (orphan)
            core_free(busy_h);
    }
    busy_h = 0;
    orphan = false;
    mutex_unlock(&dbq_mtx);
}

/* -- building ---------------------------------------------------------- */

bool dbq_begin(int m)
{
    struct dbq_stamp st;
    struct dbq_live *l;
    int h;

    if (m <= 0 || (uint32_t)m > DBQ_SLOT)
        return false;
    if (!stamp_of(tagcache_context_active(), &st) || st.entries < 0)
        return false;
    h = core_alloc(dbq_need(m));
    if (h <= 0)
        return false;
    l = core_get_data(h);
    memset(l, 0, sizeof(*l));
    l->cap = m;
    l->mode = DBQ_MODE_IDX;
    l->st = st;
    memset((uint8_t *)&l->q[m], 0, 2 * bits_len(m));
    mutex_lock(&dbq_mtx);
    drop_table();
    live_h = h;
    idx_live = true;    /* a publication from here on keeps the old one */
    mutex_unlock(&dbq_mtx);
    return true;
}

int dbq_add(int32_t idx)
{
    struct dbq_live *l = live();

    if (!l || l->seq || l->count >= l->cap || idx < 0)
        return -1;
    l->q[l->count] = idx;
    return l->count++;
}

uint8_t *dbq_groups(int *n, uint8_t **placed)
{
    struct dbq_live *l = live();
    uint8_t *g;

    if (!l)
        return NULL;
    g = (uint8_t *)&l->q[l->cap];
    if (n)
        *n = l->count;
    if (placed)
        *placed = g + bits_len(l->cap);
    return g;
}

/* Persist the table with that seq (payload into the file dbq.hdr's e[0]
 * doesn't use, then dbq.hdr): as a new queue (e[0] = it, the previous IDX
 * row kept, M5), or as a mode change of e[0]. persist_mtx held. */
static bool persist_table(uint32_t seq, bool fresh)
{
    struct dbq_hdr h, old;
    struct dbq_live *l;
    struct dbq_entry e;
    unsigned file;
    bool had, ok;

    had = hdr_read(&old);
    if (!fresh && (!had || old.nblocks == 0 || old.e[0].seq != seq))
        return true;                /* not the persisted queue: nothing */
    l = table_get(seq);
    if (!l)
        return true;                /* replaced meanwhile */
    file = had && old.nblocks > 0 ? (old.e[0].file & 1) ^ 1 : 0;
    memset(&e, 0, sizeof(e));
    e.file = file;
    e.mode = l->mode;
    e.seq = seq;
    e.count = l->count;
    e.st = l->st;
    ok = payload_write(file, seq, l->mode, l->q, l->count);
    table_put();
    if (!ok)
        return false;
    memset(&h, 0, sizeof(h));
    h.nblocks = 1;
    h.next_seq = fresh ? dbq_seq_next(seq) : hdr_next_seq;
    h.cur_file = file;
    h.e[0] = e;
    if (fresh && had && old.nblocks > 0 && old.e[0].mode == DBQ_MODE_IDX
        && old.e[0].file != file)
    {
        h.e[1] = old.e[0];
        h.nblocks = 2;
    }
    return hdr_write(&h);
}

int dbq_commit(uint32_t *tag)
{
    struct dbq_live *l;
    uint32_t seq, n = 0;
    bool ok;

    *tag = 0;
    mutex_lock(&persist_mtx);
    seq = hdr_next_seq ? hdr_next_seq : 1;
    hdr_next_seq = dbq_seq_next(seq);       /* never reused */
    mutex_lock(&dbq_mtx);
    l = live();
    if (l && l->count > 0 && !l->seq)
    {
        l->seq = seq;
        n = l->count;
    }
    mutex_unlock(&dbq_mtx);
    if (!n)
    {
        mutex_unlock(&persist_mtx);
        return -1;
    }
    ok = persist_table(seq, true);
    if (ok)
    {
        struct dbq_hdr h;
        hdr_total = n;
        idx_live = true;
        if (hdr_read(&h) && h.e[0].seq == seq)
            *tag = dbq_tag(h.e[0].file, seq);
    }
    mutex_unlock(&persist_mtx);
    if (!ok)
        UPDATE_TRACE("dbq commit fail n=%lu", (unsigned long)n);
    return n;
}

/* -- resolve (P1) ------------------------------------------------------ */

/* The name at 'seek' in <db_path>/database_4.tcd */
static int read_name(const char *db_path, int32_t seek, uint32_t expect,
                     char *buf, size_t len, char *scratch)
{
    int32_t eh[2];
    int fd, got = -1;

    if (seek < 0)
        return DBQ_WHY_IO;
    snprintf(scratch, MAX_PATH + 1, "%s/" TC_NAMES, db_path);
    fd = open(scratch, O_RDONLY);
    if (fd < 0)
        return DBQ_WHY_IO;
    if (lseek(fd, seek, SEEK_SET) == seek
        && read(fd, eh, sizeof(eh)) == (ssize_t)sizeof(eh)
        && eh[0] >= 2 && (size_t)eh[0] <= len)
        got = read(fd, buf, eh[0]);
    close(fd);
    return dbq_check_name(eh, expect, buf, got, len);
}

/* IDX, on disk: the entry's filename seek from the stamp's master index;
 * the directory must still hold that generation */
static int read_master(const struct dbq_stamp *st, uint32_t idx,
                       int32_t *seek, char *scratch)
{
    int32_t mh[6], w[2];
    off_t off = TC_MASTER_HDR + (off_t)idx * TC_ENTRY;
    int fd, why = DBQ_WHY_IO;

    snprintf(scratch, MAX_PATH + 1, "%s/" TC_MASTER, st->db_path);
    fd = open(scratch, O_RDONLY);
    if (fd < 0)
        return DBQ_WHY_LOST;
    if (read(fd, mh, sizeof(mh)) != (ssize_t)sizeof(mh))
        why = DBQ_WHY_IO;
    else if (mh[4] != st->commitid || mh[2] != st->entries
             || (int32_t)idx >= mh[2])
        why = DBQ_WHY_LOST;
    else if (lseek(fd, off + tag_filename * 4, SEEK_SET) >= 0
             && read(fd, &w[0], 4) == 4
             && lseek(fd, off + TAG_COUNT * 4, SEEK_SET) >= 0
             && read(fd, &w[1], 4) == 4)
    {
        why = (w[1] & TC_DELETED) ? DBQ_WHY_DELETED : DBQ_OK;
        *seek = w[0];
    }
    close(fd);
    return why;
}

static int read_path(uint32_t off, char *buf, size_t len)
{
    int fd = open(DBQ_PATHS, O_RDONLY);
    int got = -1;

    if (fd < 0)
        return DBQ_WHY_IO;
    if (lseek(fd, off, SEEK_SET) == (off_t)off)
        got = read(fd, buf, len);
    close(fd);
    if (got <= 1 || !memchr(buf, '\0', got) || buf[0] != '/')
        return DBQ_WHY_IO;
    return DBQ_OK;
}

static int resolve(int mode, uint32_t v, const struct dbq_stamp *st,
                   char *buf, size_t len, char *scratch)
{
    struct tagcache_context *ctx;
    struct dbq_stamp act;
    const int32_t *seeks;
    int32_t seek = -1;
    int why;

    if (mode == DBQ_MODE_IDX && v != DBQ_LOST)
    {
        /* try only: a passive reference never waits on a publication */
        ctx = tagcache_context_acquire_passive();
        if (ctx)
        {
            bool have = stamp_of(ctx, &act);
            if (have && dbq_res_mode(mode, v, st, &act, false)
                        == DBQ_RES_LIVE)
            {
                seeks = tagcache_ui_seeks_ctx(ctx, v, NULL);
                if (seeks)
                    seek = seeks[tag_filename];
                tagcache_context_release_passive(ctx);
                if (!seeks)
                    return DBQ_WHY_DELETED;
                return read_name(st->db_path, seek, v, buf, len, scratch);
            }
            tagcache_context_release_passive(ctx);
        }
    }
    /* SEEK's directory is kept by the prepare gate (4.2.3); IDX on disk
     * checks it in read_master() */
    switch (dbq_res_mode(mode, v, st, NULL, true))
    {
    case DBQ_RES_DISK:
        why = read_master(st, v, &seek, scratch);
        if (why != DBQ_OK)
            return why;
        return read_name(st->db_path, seek, v, buf, len, scratch);
    case DBQ_RES_SEEK:
        return read_name(st->db_path, v, DBQ_LOST, buf, len, scratch);
    case DBQ_RES_PATH:
        return read_path(v, buf, len);
    default:
        return DBQ_WHY_LOST;
    }
}

int dbq_filename(uint32_t k, char *buf, size_t len, char *scratch)
{
    struct dbq_stamp st = { 0, 0, { 0 } };
    struct dbq_live *l;
    uint32_t v = DBQ_LOST, seq = 0;
    int mode = DBQ_MODE_IDX, why;

    if (len > 0)
        buf[0] = '\0';
    /* locals only: the table is movable and I/O yields (L4); dbq_mtx is
     * never held across a write (M-B) */
    mutex_lock(&dbq_mtx);
    l = live();
    if (l && k < l->count)
    {
        v = l->q[k];
        mode = dbq_slot_mode(l->mode, l->split, k);
        st = l->st;
        seq = l->seq;
    }
    mutex_unlock(&dbq_mtx);
    why = v == DBQ_LOST ? DBQ_WHY_LOST
                        : resolve(mode, v, &st, buf, len, scratch);
    if (why != DBQ_OK
        && (seq != miss_seq || TIME_AFTER(current_tick, miss_tick + HZ)))
    {
        miss_seq = seq;
        miss_tick = current_tick;
        UPDATE_TRACE("dbq miss k=%lu why=%s", (unsigned long)k,
                     dbq_why_name(why));
    }
    return dbq_resolve_finish(why, k, buf, len);
}

/* -- resume (P3) ------------------------------------------------------- */

int dbq_resume_block(uint32_t tag, uint32_t first_k, uint32_t count)
{
    struct dbq_hdr h;
    const struct dbq_entry *e;
    struct dbq_live *l;
    char scratch[MAX_PATH + 1];
    const char *why UNUSED_ATTR = "hdr";
    int hd = 0;
    bool ok;

    /* playlist -> persist_mtx: no writer can tear what is read here */
    mutex_lock(&persist_mtx);
    if (!hdr_read(&h) || !(e = dbq_hdr_find(&h, tag, first_k, count)))
        goto fail;
    /* Q-R4-5: a block whose generation is gone is not resumed */
    why = "lost";
    if (e->mode != DBQ_MODE_PATH && !master_holds(&e->st, scratch))
        goto fail;
    why = "mem";
    hd = core_alloc(dbq_need(count));
    if (hd <= 0)
        goto fail;
    l = core_get_data(hd);
    memset(l, 0, sizeof(*l));
    memset((uint8_t *)&l->q[count], 0, 2 * bits_len(count));
    core_pin(hd);
    ok = payload_read(e, l->q);
    core_unpin(hd);
    why = "payload";
    if (!ok)
        goto fail;
    l = core_get_data(hd);
    l->seq = e->seq;
    l->count = l->cap = count;
    l->mode = e->mode;
    l->file = e->file;
    l->st = e->st;
    mutex_lock(&dbq_mtx);
    drop_table();
    live_h = hd;
    if (e != &h.e[0])
    {
        /* the previous queue's row (M5): its conversions are owed too */
        boot_block = false;
        check_owed = e->mode == DBQ_MODE_IDX;
        mat_owed = e->mode == DBQ_MODE_SEEK;
    }
    idx_live = e->mode == DBQ_MODE_IDX;
    mutex_unlock(&dbq_mtx);
    mutex_unlock(&persist_mtx);
    tagcache_quiet_kick();  /* P19 19-1a: a conversion may be owed now */
    UPDATE_TRACE("dbq resume n=%lu mode=%d", (unsigned long)count, e->mode);
    return 0;
fail:
    if (hd > 0)
        core_free(hd);
    mutex_unlock(&persist_mtx);
    UPDATE_TRACE("dbq resume fail why=%s n=%lu", why, (unsigned long)count);
    return -1;
}

/* -- the tagcache thread (4.2) ----------------------------------------- */

bool dbq_idx_live(void)
{
    return idx_live;
}

void dbq_published(struct tagcache_context *old)
{
    /* inside the publication's no-yield window: no lock, no I/O (M4) */
    if (!old)
        return;
    if (old_ctx)
        tagcache_context_release_passive(old);
    else
        old_ctx = old;
    rebase_owed = true;
}

bool dbq_prepare_wait(void)
{
    return !dbq_booted || rebase_owed || mat_owed || check_owed;
}

static void release_old(void)
{
    if (old_ctx)
        tagcache_context_release_passive(old_ctx);
    old_ctx = NULL;
}

/* fingerprints in the old (0) and the new (1) snapshot, RAM only */
struct fp_ctx { struct tagcache_context *s[2]; };

static uint32_t str_crc(struct tagcache_context *ctx, const int32_t *sk,
                        int tag)
{
    const char *s = tagcache_ui_str_ctx(ctx, tag, sk[tag]);
    return s ? dbq_crc(s, strlen(s)) : 0;
}

static int fp_cb(void *c, int which, uint32_t idx, uint32_t *fp,
                 int32_t *seek)
{
    struct tagcache_context *ctx = ((struct fp_ctx *)c)->s[which];
    const int32_t *sk;
    uint32_t a;

    if ((int32_t)idx >= tagcache_ui_count_ctx(ctx))
        return DBQ_FP_RANGE;
    sk = tagcache_ui_seeks_ctx(ctx, idx, NULL);
    if (!sk)
        return DBQ_FP_DELETED;
    a = str_crc(ctx, sk, tag_album);
    *fp = str_crc(ctx, sk, tag_title) ^ (a << 1 | a >> 31)
          ^ (uint32_t)sk[tag_length] ^ ((uint32_t)sk[tag_mtime] * 2654435761u);
    *seek = sk[tag_filename];
    return DBQ_FP_OK;
}

/* The resident block's identity, read under dbq_mtx. false: none (or
 * one still being built: seq 0, the next poll looks again). */
static bool resident(uint32_t *seq, uint32_t *n, int *mode,
                     struct dbq_stamp *st, bool *building)
{
    struct dbq_live *l;
    bool ok = false;

    *building = false;
    mutex_lock(&dbq_mtx);
    l = live();
    if (l && !l->seq)
        *building = true;
    else if (l)
    {
        *seq = l->seq;
        *n = l->count;
        *mode = l->mode;
        *st = l->st;
        ok = true;
    }
    mutex_unlock(&dbq_mtx);
    return ok;
}

#define DBQ_STEP 256

/* The RAM rebase in place (H2, review M-A): no transient, a chunk of
 * DBQ_STEP slots at a time under dbq_mtx, yielding between chunks.
 * 1: re-stamped; 0: converted to SEEK; -1: the table was replaced. */
static int rebase_in_place(uint32_t seq, uint32_t n, struct fp_ctx *fc,
                           const struct dbq_stamp *now, bool compare)
{
    struct dbq_fp_ops ops = { fp_cb, NULL, fc };
    struct dbq_live *l;
    uint32_t k, m;
    bool mism = !compare;

    for (k = 0; k < n && !mism; k += DBQ_STEP)
    {
        mutex_lock(&dbq_mtx);
        l = live();
        if (!l || l->seq != seq)
        {
            mutex_unlock(&dbq_mtx);
            return -1;
        }
        m = MIN(DBQ_STEP, n - k);
        mism = dbq_rebase_check(&l->q[k], m, &ops);
        mutex_unlock(&dbq_mtx);
        yield();
    }
    if (!mism)
    {
        mutex_lock(&dbq_mtx);
        l = live();
        if (l && l->seq == seq)
            l->st = *now;
        mutex_unlock(&dbq_mtx);
        return l ? 1 : -1;
    }
    for (k = 0; k < n; k += DBQ_STEP)
    {
        mutex_lock(&dbq_mtx);
        l = live();
        if (!l || l->seq != seq)
        {
            mutex_unlock(&dbq_mtx);
            return -1;
        }
        m = MIN(DBQ_STEP, n - k);
        dbq_rebase_seek(&l->q[k], m, &ops);
        l->split = k + m;           /* these resolve as SEEK already */
        if (l->split >= n)
        {
            l->mode = DBQ_MODE_SEEK;    /* the old stamp: its directory */
            l->split = 0;
        }
        mutex_unlock(&dbq_mtx);
        yield();
    }
    return 0;
}

/* A transient for the disk passes (vals, perm, a read chunk, a write
 * buffer): free heap only while audio holds its buffer; else, as the art
 * thread's chapter pass does, #88's paused window (the job is part of the
 * commit: the next one waits for it), with the floor kept; else it waits
 * for a stop (Q11). Checked right before the allocation, no yield. */
struct job {
    int h;
    uint32_t seq, n;
    int mode;
    struct dbq_stamp st;
    uint32_t *vals, *perm;
    uint8_t *chunk, *out;
    bool window;
};

static bool job_alloc(struct job *j, uint32_t n)
{
    size_t need = (size_t)n * 8 + DBQ_CHUNK + DBQ_OUTBUF;

    if (audio_status() && core_allocatable() < need + 1024)
    {
        /* its own tenant (P16): a commit ending its window never ends
         * this one, and the regrow waits for job_free() */
        maint_want(MAINT_DBQ);
        if (!maint_take(MAINT_DBQ, need + 1024))
            return false;
        j->window = true;
    }
    j->h = core_alloc(need);
    if (j->h <= 0)
    {
        if (j->window)
            maint_done(MAINT_DBQ, false);
        j->window = false;
        return false;
    }
    core_pin(j->h);             /* transient, for the job: I/O yields */
    j->vals = core_get_data(j->h);
    j->perm = j->vals + n;
    j->chunk = (uint8_t *)(j->vals + 2 * n);
    j->out = j->chunk + DBQ_CHUNK;
    return true;
}

static void job_free(struct job *j)
{
    if (j->h > 0)
    {
        core_unpin(j->h);
        core_free(j->h);
    }
    j->h = 0;
    if (j->window)
        maint_done(MAINT_DBQ, true);        /* the memory goes back */
    j->window = false;
}

/* The block's values into the transient: the resident table (if it is
 * still seq), else this boot's persisted e[0]. 1 ok, 0 nothing, -1 wait. */
static int job_load(struct job *j, bool res)
{
    struct dbq_hdr h;
    struct dbq_live *l;

    if (!job_alloc(j, j->n))
        return -1;
    if (res)
    {
        mutex_lock(&dbq_mtx);
        l = live();
        if (l && l->seq == j->seq && l->count == j->n)
            memcpy(j->vals, l->q, j->n * 4);
        else
            j->seq = 0;
        mutex_unlock(&dbq_mtx);
    }
    else if (!hdr_read(&h) || h.nblocks == 0 || h.e[0].seq != j->seq
             || !payload_read(&h.e[0], j->vals))
        j->seq = 0;
    if (!j->seq)
    {
        job_free(j);
        return 0;
    }
    return 1;
}

/* The job's values become the block's (same seq only), then persisted;
 * mode -1: lost (Q-R4-5): the row goes. persist_mtx held. */
static void job_apply(struct job *j, int mode, const struct dbq_stamp *st)
{
    struct dbq_hdr h;
    struct dbq_live *l;
    bool res = false;

    mutex_lock(&dbq_mtx);
    l = live();
    if (l && l->seq == j->seq && l->count == j->n)
    {
        memcpy(l->q, j->vals, j->n * 4);
        l->mode = mode < 0 ? DBQ_MODE_PATH : mode;
        l->split = 0;
        l->st = *st;
        res = true;
    }
    mutex_unlock(&dbq_mtx);
    if (!hdr_read(&h) || h.nblocks == 0 || h.e[0].seq != j->seq)
        return;
    if (mode < 0)
    {
        h.nblocks = 0;
        hdr_write(&h);
    }
    else if (res)
        persist_table(j->seq, false);
    else
    {
        unsigned file = (h.e[0].file & 1) ^ 1;  /* not resident: the copy */
        if (payload_write(file, j->seq, mode, j->vals, j->n))
        {
            h.e[0].file = file;
            h.e[0].file_off = 0;
            h.e[0].mode = mode;
            h.e[0].st = *st;
            h.nblocks = 1;
            h.cur_file = file;
            hdr_write(&h);
        }
    }
}

/* An IDX block whose snapshot isn't in RAM any more: its filename seeks
 * from the stamp's master index, one sequential pass (4.2.4). false:
 * that generation is gone (every slot LOST). */
static bool disk_seek_pass(struct job *j, char *scratch)
{
    int32_t w[2];
    uint32_t i, k, idx;
    off_t base = -1, off;
    ssize_t got = 0;
    int fd;

    if (!master_holds(&j->st, scratch))
        return false;
    snprintf(scratch, MAX_PATH + 1, "%s/" TC_MASTER, j->st.db_path);
    fd = open(scratch, O_RDONLY);
    if (fd < 0)
        return false;
    dbq_sort_perm(j->perm, j->vals, j->n);
    for (i = 0; i < j->n; i++)
    {
        k = j->perm[i];
        idx = j->vals[k];
        if (idx == DBQ_LOST)
            continue;
        off = TC_MASTER_HDR + (off_t)idx * TC_ENTRY;
        if (base < 0 || off < base || off + (off_t)TC_ENTRY > base + got)
        {
            base = off;
            got = lseek(fd, off, SEEK_SET) == off
                  ? read(fd, j->chunk, DBQ_CHUNK) : -1;
            if (got < (ssize_t)TC_ENTRY)
            {
                j->vals[k] = DBQ_LOST;
                base = -1;
                continue;
            }
        }
        memcpy(&w[0], j->chunk + (off - base) + tag_filename * 4, 4);
        memcpy(&w[1], j->chunk + (off - base) + TAG_COUNT * 4, 4);
        j->vals[k] = (w[1] & TC_DELETED) || w[0] < 0 ? DBQ_LOST
                                                     : (uint32_t)w[0];
    }
    close(fd);
    return true;
}

/* SEEK -> PATH (4.2.3): every name from the stamp's filename tag file in
 * one sequential pass, into dbqpath.dat; the values become offsets. */
static bool materialize(struct job *j, char *scratch, uint32_t *bytes)
{
    int32_t eh[2];
    uint32_t i, k, total = 0, used = 0;
    off_t base = -1, off;
    ssize_t got = 0;
    int fd, ofd;
    bool ok = true;

    snprintf(scratch, MAX_PATH + 1, "%s/" TC_NAMES, j->st.db_path);
    fd = open(scratch, O_RDONLY);
    if (fd < 0)
        return false;
    ofd = open(DBQ_PATHS_TMP, O_CREAT|O_TRUNC|O_WRONLY, 0666);
    if (ofd < 0)
    {
        close(fd);
        return false;
    }
    dbq_sort_perm(j->perm, j->vals, j->n);
    for (i = 0; i < j->n && ok; i++)
    {
        k = j->perm[i];
        off = j->vals[k];
        if (j->vals[k] == DBQ_LOST)
            continue;
        if (base < 0 || off < base || off + 8 > base + got)
        {
            base = off;
            got = lseek(fd, off, SEEK_SET) == off
                  ? read(fd, j->chunk, DBQ_CHUNK) : -1;
        }
        if (got < 8 || off + 8 > base + got)
        {
            j->vals[k] = DBQ_LOST;
            base = -1;
            continue;
        }
        memcpy(eh, j->chunk + (off - base), 8);
        if (eh[0] >= 2 && eh[0] <= MAX_PATH
            && off + 8 + eh[0] > base + got)
        {
            base = off;         /* the name runs past the chunk */
            got = lseek(fd, off, SEEK_SET) == off
                  ? read(fd, j->chunk, DBQ_CHUNK) : -1;
        }
        if (eh[0] < 2 || eh[0] > MAX_PATH || got < 8 + eh[0]
            || dbq_check_name(eh, DBQ_LOST,
                              (char *)j->chunk + (off - base) + 8,
                              eh[0], MAX_PATH + 1) != DBQ_OK)
        {
            j->vals[k] = DBQ_LOST;
            continue;
        }
        if (used + eh[0] > DBQ_OUTBUF)
        {
            ok = write(ofd, j->out, used) == (ssize_t)used;
            used = 0;
        }
        memcpy(j->out + used, j->chunk + (off - base) + 8, eh[0]);
        used += eh[0];
        j->vals[k] = total;
        total += eh[0];
    }
    if (ok && used)
        ok = write(ofd, j->out, used) == (ssize_t)used;
    ok = ok && fsync(ofd) >= 0;
    close(ofd);
    close(fd);
    if (ok)
        ok = safefile_replace(DBQ_PATHS_TMP, DBQ_PATHS, DBQ_PATHS_PREV) == 0;
    if (!ok)
        safefile_remove(DBQ_PATHS_TMP, DBQ_PATHS);
    *bytes = total;
    return ok;
}

/* rebase_owed / check_owed. false: wait (memory for a disk pass). */
static bool job_rebase(char *scratch)
{
    struct tagcache_context *act = tagcache_context_active();
    struct dbq_stamp now, old, st;
    struct dbq_hdr h;
    struct fp_ctx fc;
    struct job j;
    long t0 UNUSED_ATTR = current_tick;
    const char *res UNUSED_ATTR = "none";
    uint32_t seq = 0, n = 0;
    int mode = DBQ_MODE_IDX, rc;
    bool res_ok, building;

    res_ok = resident(&seq, &n, &mode, &st, &building);
    if (building)
        return false;               /* its stamp is set at the commit */
    if (!res_ok)
    {
        if (!boot_block || !hdr_read(&h) || h.nblocks == 0)
            goto done;
        seq = h.e[0].seq;
        n = h.e[0].count;
        mode = h.e[0].mode;
        st = h.e[0].st;
    }
    if (mode != DBQ_MODE_IDX || n == 0)
        goto done;
    stamp_of(act, &now);
    /* "same" from the stamps and one master header read: no memory */
    if (dbq_stamp_eq(&st, &now)
        || (now.entries < 0 && !strcmp(st.db_path, now.db_path)
            && master_holds(&st, scratch)))
    {
        res = "same";
        goto done;
    }
    if (res_ok && old_ctx && stamp_of(old_ctx, &old)
        && dbq_stamp_eq(&st, &old) && tagcache_ui_count_ctx(old_ctx) >= 0)
    {
        /* in place, while the old snapshot is still referenced */
        fc.s[0] = old_ctx;
        fc.s[1] = act;
        mutex_lock(&persist_mtx);
        rc = rebase_in_place(seq, n, &fc, &now,
                             tagcache_ui_count_ctx(act) >= 0);
        release_old();
        if (rc == 1)
        {
            res = "stamp";
            persist_table(seq, false);
        }
        else if (rc == 0)
        {
            res = "seek";
            persist_table(seq, false);
            mat_owed = true;
        }
        mutex_unlock(&persist_mtx);
        goto done;
    }
    /* no RAM snapshot of the block's generation: the disk pass */
    release_old();
    memset(&j, 0, sizeof(j));
    j.seq = seq;
    j.n = n;
    j.st = st;
    rc = job_load(&j, res_ok);
    if (rc < 0)
        return false;               /* the old snapshot isn't held */
    if (rc > 0)
    {
        mutex_lock(&persist_mtx);
        if (disk_seek_pass(&j, scratch))
        {
            res = "seek";
            job_apply(&j, DBQ_MODE_SEEK, &j.st);
            mat_owed = true;
        }
        else
        {
            memset(j.vals, 0xff, j.n * 4);
            res = "lost";
            job_apply(&j, -1, &j.st);
        }
        mutex_unlock(&persist_mtx);
        job_free(&j);
    }
done:
    UPDATE_TRACE("dbq rebase block=%lu result=%s n=%lu us=%ld",
                 (unsigned long)seq, res, (unsigned long)n,
                 (long)(current_tick - t0) * (1000000 / HZ));
    release_old();
    rebase_owed = check_owed = false;
    return true;
}

static bool job_materialize(char *scratch)
{
    struct dbq_hdr h;
    struct job j;
    long t0 UNUSED_ATTR = current_tick;
    uint32_t bytes = 0, seq = 0, n = 0;
    int mode = DBQ_MODE_IDX, rc;
    bool res_ok, building;
    struct dbq_stamp st;

    res_ok = resident(&seq, &n, &mode, &st, &building);
    if (building)
        return false;
    if (!res_ok)
    {
        if (!boot_block || !hdr_read(&h) || h.nblocks == 0)
            goto done;
        seq = h.e[0].seq;
        n = h.e[0].count;
        mode = h.e[0].mode;
        st = h.e[0].st;
    }
    if (mode != DBQ_MODE_SEEK || n == 0)
        goto done;
    memset(&j, 0, sizeof(j));
    j.seq = seq;
    j.n = n;
    j.st = st;
    rc = job_load(&j, res_ok);
    if (rc < 0)
        return false;
    if (rc > 0)
    {
        mutex_lock(&persist_mtx);
        if (materialize(&j, scratch, &bytes))
        {
            job_apply(&j, DBQ_MODE_PATH, &j.st);
            UPDATE_TRACE("dbq materialize n=%lu bytes=%lu ms=%ld",
                         (unsigned long)j.n, (unsigned long)bytes,
                         (long)(current_tick - t0) * (1000 / HZ));
        }
        else
        {
            UPDATE_TRACE("dbq materialize fail n=%lu", (unsigned long)j.n);
            rc = -1;            /* again at the next poll; gate stays */
        }
        mutex_unlock(&persist_mtx);
        job_free(&j);
        if (rc < 0)
            return false;
    }
done:
    mat_owed = false;
    return true;
}

static volatile bool working;   /* 3.20: dbq_working() */

bool dbq_working(void)
{
    return working;
}

void dbq_tc_poll(void)
{
    static bool waiting;    /* one "waits" line per wait */
    char scratch[MAX_PATH + 1];
    bool done = true;

    if (!dbq_booted)
        return;
    if (!rebase_owed && !check_owed && !mat_owed)
    {
        waiting = false;
        return;
    }
    working = true;
    if (rebase_owed || check_owed)
        done = job_rebase(scratch);
    if (done && mat_owed)
        done = job_materialize(scratch);
    working = false;
    if (!done && !waiting)
        UPDATE_TRACE("dbq %s waits", mat_owed ? "materialize" : "rebase");
    waiting = !done;
}
