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
 * iPod UI shell: the Apple fallback's sync job (#137; developer builds,
 * behind the developer switch). Part of the MOD_XINDEX module: it runs in
 * the indexer's take, on the storage worker, at the post-USB moment after
 * the library scan and in the boot window (xindex.c). One pass:
 *   plan (afb_plan: the state file, the link folder, the fingerprint);
 *   verify (a volume a host saw with live links: repair what may be);
 *   park (drop the link folder; no chain freed);
 *   unpark: consume Apple's Play Counts, make one link per Apple-playable
 *   song in iPod_Control/Music/PKB (songs already under iPod_Control/Music
 *   play in place), the link table, then the music database when the song
 *   set changed (streamed to iTunesDB.new, verified by re-parse, the old
 *   one kept as .bak, the rename the commit), and the state LIVE.
 * The park itself also runs on the USB ack path (afb_hook.c), before the
 * host is given the disk, so a host never sees a link.
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
#include "config.h"
#include <string.h>
#include <stdio.h>
#include "string-extra.h"
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "dir.h"
#include "timefuncs.h"
#include "tagcache.h"
#include "ipodui/uilog.h"
#include "ipodui/xindex.h"
#include "ipodui/afb.h"
#include "ipodui/afb_core.h"
#include "ipodui/afb_fat.h"
#include "ipodui/bootstamp.h"

#define ALOG(...) ui_log("afb", __VA_ARGS__)

#define OUT_BUF   (16 * 1024)
#define WIN_BUF   (4 * 1024)
#define TBL_BUF   (4 * 1024)
#define STR_MAX   TAGCACHE_BUFSZ

struct work {
    struct tagcache_search tcs;
    struct afb_rec rec;
    char path[MAX_PATH], loc[AFB_LOC_MAX], name[16];
    char str[4][STR_MAX];               /* title, artist, album, genre */
    uint8_t out[OUT_BUF];
    uint8_t win[WIN_BUF];
    uint8_t tbl[TBL_BUF];
};

/* -- buffered output with patches (the database) ------------------------ */
struct bout {
    int fd;
    uint8_t *buf;
    uint32_t n, base;
};

static int bout_flush(struct bout *b)
{
    if (b->n && write(b->fd, b->buf, b->n) != (ssize_t)b->n)
        return -1;
    b->base += b->n;
    b->n = 0;
    return 0;
}

static int bout_write(void *ctx, const void *p, uint32_t n)
{
    struct bout *b = ctx;

    if (b->n + n > OUT_BUF && bout_flush(b) < 0)
        return -1;
    if (n > OUT_BUF)
        return write(b->fd, p, n) == (ssize_t)n ? (b->base += n, 0) : -1;
    memcpy(b->buf + b->n, p, n);
    b->n += n;
    return 0;
}

static int bout_patch(void *ctx, uint32_t off, const void *p, uint32_t n)
{
    struct bout *b = ctx;

    if (off >= b->base)                 /* still in the buffer */
    {
        memcpy(b->buf + (off - b->base), p, n);
        return 0;
    }
    if (bout_flush(b) < 0 || lseek(b->fd, off, SEEK_SET) != (off_t)off
        || write(b->fd, p, n) != (ssize_t)n
        || lseek(b->fd, 0, SEEK_END) != (off_t)b->base)
        return -1;
    return 0;
}

/* -- windowed reader (the verify) --------------------------------------- */
struct win {
    int fd;
    uint8_t *buf;
    uint32_t at, n;
};

static int win_read(void *ctx, uint32_t off, void *p, uint32_t n)
{
    struct win *w = ctx;

    if (n > WIN_BUF)
        return -1;
    if (off < w->at || off + n > w->at + w->n)
    {
        ssize_t got;

        if (lseek(w->fd, off, SEEK_SET) != (off_t)off)
            return -1;
        got = read(w->fd, w->buf, WIN_BUF);
        if (got < (ssize_t)n)
            return -1;
        w->at = off;
        w->n = got;
    }
    memcpy(p, w->buf + (off - w->at), n);
    return 0;
}

static int verify_file(const char *path, struct work *wk)
{
    struct itdb_stats st;
    struct win w = { .buf = wk->win, .at = 0, .n = 0 };
    struct afb_in in = { .ctx = &w, .read = win_read };
    int rc;

    w.fd = open(path, O_RDONLY);
    if (w.fd < 0)
        return ITDB_E_IO;
    rc = itdb_verify(&in, ffilesize(w.fd), true, &st);
    close(w.fd);
    return rc;
}

/* -- the state file -------------------------------------------------------- */
static void state_read(struct afb_stfile *s)
{
    int fd = open(AFB_STATE, O_RDONLY);

    if (fd < 0 || read(fd, s, sizeof(*s)) != sizeof(*s)
        || s->magic != AFB_ST_MAGIC || s->fmt != AFB_FMT)
    {
        memset(s, 0, sizeof(*s));
        s->state = fd < 0 ? AFB_ST_NONE : AFB_ST_UNPARKING; /* bad: redo */
    }
    if (fd >= 0)
        close(fd);
}

static int state_write(struct afb_stfile *s)
{
    int fd;

    s->magic = AFB_ST_MAGIC;
    s->fmt = AFB_FMT;
    fd = open(AFB_STATE ".tmp", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return -1;
    if (write(fd, s, sizeof(*s)) != sizeof(*s))
    {
        close(fd);
        return -1;
    }
    close(fd);
    return rename(AFB_STATE ".tmp", AFB_STATE);
}

/* -- Play Counts: Apple's plays since the last write, by position ------- */
/* The deltas belong to the database they were counted against, so they
 * never meet a rewritten one: moved (not copied) into our folder for a
 * later merge, the one before kept too; that database is kept as .bak */
static void playcounts_take(void)
{
    if (!file_exists(AFB_PLAYCOUNTS))
        return;
    remove(AFB_DIR "/Play Counts.old");
    rename(AFB_DIR "/Play Counts", AFB_DIR "/Play Counts.old");
    ALOG("playcounts rc=%d", rename(AFB_PLAYCOUNTS, AFB_DIR "/Play Counts"));
}

/* -- the songs (the library, after the scan) ------------------------------ */
static bool song_next(struct work *wk)
{
    while (tagcache_get_next(&wk->tcs, wk->path, sizeof(wk->path)))
    {
        enum afb_kind k = afb_kind_of(wk->path);

        if (k == AFB_K_NONE || afb_in_link_dir(wk->path))
            continue;
        wk->rec.kind = k;
        return true;
    }
    return false;
}

static void song_tags(struct work *wk, struct itdb_track *t)
{
    static const uint8_t tags[4] = { tag_title, tag_artist, tag_album,
                                     tag_genre };
    struct tagcache_search *tcs = &wk->tcs;
    const char *base = strrchr(wk->path, '/');

    memset(t, 0, sizeof(*t));
    for (int i = 0; i < 4; i++)
    {
        char *dst = wk->str[i];

        if (!tagcache_retrieve(tcs, tcs->idx_id, tags[i], dst, STR_MAX)
            || !strcmp(dst, UNTAGGED))
            dst[0] = '\0';
    }
    if (!wk->str[0][0])
        strlcpy(wk->str[0], base ? base + 1 : wk->path, STR_MAX);
    t->title = wk->str[0];
    t->artist = wk->str[1];
    t->album = wk->str[2];
    t->genre = wk->str[3];
    t->ms = tagcache_get_numeric(tcs, tag_length);
    t->kbps = tagcache_get_numeric(tcs, tag_bitrate);
    t->track = tagcache_get_numeric(tcs, tag_tracknumber);
    t->year = tagcache_get_numeric(tcs, tag_year);
    t->sr = 44100;                      /* not in the library; display only */
    t->kind = wk->rec.kind;
}

/* -- verify (a host saw live links) ---------------------------------------- */
static void verify_links(struct work *wk)
{
    int fd = open(AFB_TABLE, O_RDONLY), tally[AFB_V__N];
    uint32_t have = 0, used;
    struct afb_tbl_hdr h;

    memset(tally, 0, sizeof(tally));
    if (fd < 0 || read(fd, &h, sizeof(h)) != sizeof(h)
        || h.magic != AFB_TBL_MAGIC || h.fmt != AFB_FMT)
    {
        ALOG("verify table bad");
        if (fd >= 0)
            close(fd);
        return;
    }
    while (1)
    {
        const char *p;
        uint32_t need;
        struct afb_rec *r = &wk->rec;
        struct afb_probe o, l;
        struct afb_facts f;
        enum afb_verdict v;
        ssize_t got = read(fd, wk->tbl + have, TBL_BUF - have);

        if (got > 0)
            have += got;
        used = afb_rec_get(wk->tbl, have, r, &p, &need);
        if (!used)
            break;                      /* the end, or a cut table */
        memcpy(wk->path, p, r->pathlen);
        wk->path[r->pathlen] = '\0';
        memmove(wk->tbl, wk->tbl + used, have - used);
        have -= used;
        afb_vol_probe(wk->path, &o, true);
        afb_link_name(wk->name, sizeof(wk->name), r->linkno, r->kind);
        snprintf(wk->loc, sizeof(wk->loc), AFB_LINK_DIR "/%s", wk->name);
        afb_vol_probe(wk->loc, &l, false);
        memset(&f, 0, sizeof(f));
        f.orig_exists = o.exists;
        f.orig_fst = o.fst;
        f.orig_size = o.size;
        f.orig_healthy = o.healthy == 1;
        f.runs_known = r->nruns != 0xff;
        f.orig_eq_runs = f.runs_known && o.nruns == r->nruns
                         && !memcmp(o.runs, r->runs, 8 * r->nruns);
        f.link_exists = l.exists;
        f.link_fst = l.fst;
        f.link_size = l.size;
        f.rec_fst = r->fst;
        f.rec_size = r->size;
        if (o.exists && o.fst == r->fst && o.size == r->size && !f.orig_healthy
            && f.runs_known)
            f.runs_free = afb_vol_runs_free(r->runs, r->nruns) == 1;
        v = afb_classify(&f);
        /* the repairs; one that fails counts as damaged (logged below) */
        if ((v == AFB_V_EMPTIED && afb_vol_set(wk->path, r->fst, r->size) < 0)
            || (v == AFB_V_FREED && afb_vol_rechain(r->runs, r->nruns) < 0))
            v = AFB_V_DAMAGED;
        tally[v]++;
    }
    close(fd);
    /* ok orphan emptied stale freed damaged gone: the next park and
     * unpark drop the orphans and remake the stale and gone links; the
     * damaged are lost songs (their clusters reused), never repaired */
    ALOG("verify %d/%d/%d/%d/%d/%d/%d", tally[0], tally[1], tally[2],
         tally[3], tally[4], tally[5], tally[6]);
}

/* -- unpark: the links, the table, the database -------------------------- */
static void ensure_dirs(void)
{
    static const char *const d[] = { AFB_DIR, "/iPod_Control",
        AFB_MUSIC_DIR, AFB_ITUNES_DIR, AFB_LINK_DIR };

    for (unsigned i = 0; i < ARRAYLEN(d); i++)
        if (!dir_exists(d[i]))
            mkdir(d[i]);
}

/* One walk over the library's Apple-playable songs, in the library's
 * order. Pass 0: the links and the table (and the song set's signature);
 * pass 1: the database. Both skip the same songs, so link numbers agree.
 * Songs under iPod_Control/Music (a sync app's copies) play in place. */
struct walk {
    int pass;
    int fd;                     /* pass 0: the table */
    uint32_t have, links, sig, songs, bad;
    struct itdb_w w;            /* pass 1 */
    uint64_t seed;
};

static int walk_one(struct work *wk, struct walk *k)
{
    struct afb_probe o;
    struct afb_rec *r = &wk->rec;
    bool link = !afb_in_music_dir(wk->path);

    k->songs++;
    r->pathlen = strlen(wk->path);
    k->sig = afb_hash(k->sig, wk->path, r->pathlen);
    if (afb_vol_probe(wk->path, &o, link) < 0 || !o.exists
        || (link && o.healthy != 1))
    {
        k->bad++;                       /* never a link to a broken chain */
        return 0;
    }
    k->sig = afb_hash(k->sig, &o.size, sizeof(o.size));
    if (link)
        afb_link_name(wk->name, sizeof(wk->name), ++k->links, r->kind);
    if (k->pass == 0)
    {
        if (!link)
            return 0;
        r->fst = o.fst;
        r->size = o.size;
        r->linkno = k->links;
        r->nruns = o.nruns < 0 ? 0xff : o.nruns;
        memcpy(r->runs, o.runs, sizeof(r->runs));
        if (afb_vol_link(wk->name, o.fst, o.size) < 0)
            return -1;
        if (k->have + afb_rec_bytes(r) > TBL_BUF)
        {
            if (write(k->fd, wk->tbl, k->have) != (ssize_t)k->have)
                return -1;
            k->have = 0;
        }
        afb_rec_put(wk->tbl + k->have, r, wk->path);
        k->have += afb_rec_bytes(r);
        return 0;
    }
    {
        struct itdb_track t;

        if (link)
            snprintf(wk->loc, sizeof(wk->loc), ":iPod_Control:Music:PKB:%s",
                     wk->name);
        else
            afb_location(wk->loc, sizeof(wk->loc), wk->path);
        song_tags(wk, &t);
        t.size = o.size;
        t.location = wk->loc;
        t.dbid = k->seed + (uint64_t)k->songs * 0x9e3779b97f4a7c15ull;
        return itdb_add(&k->w, &t);
    }
}

static int walk(struct work *wk, volatile bool *stop, struct walk *k)
{
    int rc = 0;

    k->have = k->links = k->songs = k->bad = 0;
    k->sig = AFB_HASH0;
    if (!tagcache_search(&wk->tcs, tag_filename))
        return -1;
    while (rc >= 0 && !*stop && song_next(wk))
        rc = walk_one(wk, k);
    tagcache_search_finish(&wk->tcs);
    return *stop ? -2 : rc;
}

/* pass 0 into links.tbl.new, renamed when whole */
static int make_links(struct work *wk, volatile bool *stop, struct walk *k)
{
    struct afb_tbl_hdr h = { AFB_TBL_MAGIC, AFB_FMT, 0, 0 };
    int rc;

    k->pass = 0;
    k->fd = open(AFB_TABLE ".new", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (k->fd < 0)
        return -1;
    rc = write(k->fd, &h, sizeof(h)) == sizeof(h) ? walk(wk, stop, k) : -1;
    h.count = k->links;
    if (rc >= 0 && (write(k->fd, wk->tbl, k->have) != (ssize_t)k->have
                    || lseek(k->fd, 0, SEEK_SET) != 0
                    || write(k->fd, &h, sizeof(h)) != sizeof(h)))
        rc = -1;
    close(k->fd);
    if (rc >= 0 && rename(AFB_TABLE ".new", AFB_TABLE) < 0)
        rc = -1;
    if (k->bad)
        ALOG("skipped %lu", (unsigned long)k->bad);
    return rc;
}

/* pass 1 into iTunesDB.new; its length */
static long write_db(struct work *wk, volatile bool *stop, struct walk *k)
{
    struct bout b = { .buf = wk->out };
    struct afb_out o = { .ctx = &b, .write = bout_write,
                         .patch = bout_patch };
    uint32_t ts = (uint32_t)mktime(get_time()) + AFB_MAC_EPOCH;
    long len;
    int rc;

    k->pass = 1;
    k->seed = ((uint64_t)ts << 32) | (uint32_t)current_tick;
    b.fd = open(AFB_ITDB ".new", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (b.fd < 0)
        return -1;
    itdb_begin(&k->w, &o, ts, k->seed * 2654435761u, k->seed ^ 0x5eedull,
               100);
    rc = walk(wk, stop, k);
    len = itdb_end(&k->w, "ProkBox");
    if (bout_flush(&b) < 0)
        len = -1;
    close(b.fd);
    return rc < 0 ? rc : len;
}

/* The commit: the verified .new becomes iTunesDB, the old one .bak (a
 * database we did not write is also kept once, in our folder). A cut
 * between the renames leaves no iTunesDB: the state says UNPARKING, so
 * the next boot writes it again. */
static int commit_db(struct work *wk, const struct afb_stfile *st, long len)
{
    int rc = verify_file(AFB_ITDB ".new", wk);

    if (rc != ITDB_OK)
    {
        ALOG("db bad rc=%d", rc);
        remove(AFB_ITDB ".new");
        return -1;
    }
    if (file_exists(AFB_ITDB))
    {
        int fd = open(AFB_ITDB, O_RDONLY);
        long have = fd >= 0 ? ffilesize(fd) : -1;

        if (fd >= 0)
            close(fd);
        /* one we did not write (a sync app's): kept once, in our folder */
        if ((uint32_t)have == st->db_len || file_exists(AFB_DIR "/iTunesDB.orig")
            || rename(AFB_ITDB, AFB_DIR "/iTunesDB.orig") < 0)
        {
            remove(AFB_ITDB ".bak");
            if (rename(AFB_ITDB, AFB_ITDB ".bak") < 0)
                return -1;
        }
        else
            ALOG("db kept foreign");
    }
    if (rename(AFB_ITDB ".new", AFB_ITDB) < 0)
        return -1;
    ALOG("db ok bytes=%ld", len);
    return 0;
}

static int unpark(struct work *wk, struct afb_stfile *st, volatile bool *stop,
                  int32_t *nlinks)
{
    struct walk k;
    long len;
    int rc;

    ensure_dirs();
    playcounts_take();                  /* before any positional rewrite */
    st->state = AFB_ST_UNPARKING;
    if (state_write(st) < 0)
        return -1;
    /* the scan never walks the link folder (tagcache.c check_ignore):
     * written before the first link */
    if (!file_exists(AFB_LINK_DIR "/database.ignore"))
        close(open(AFB_LINK_DIR "/database.ignore", O_WRONLY | O_CREAT, 0666));
    rc = make_links(wk, stop, &k);
    if (rc < 0)
        return rc;
    *nlinks = k.links;
    if (k.sig != st->sig || !file_exists(AFB_ITDB))
    {
        uint32_t sig = k.sig;

        len = write_db(wk, stop, &k);
        if (len < 0 || commit_db(wk, st, len) < 0)
            return len == -2 ? -2 : -1;
        st->db_len = len;
        st->sig = sig;
    }
    else
        ALOG("db same songs=%lu", (unsigned long)k.songs);
    afb_vol_fp(&st->fp_free, &st->fp_next);
    st->state = AFB_ST_LIVE;
    st->count = k.links;
    return state_write(st);
}

int afb_job(struct xidx_job *j, int when)
{
    struct work *wk = (struct work *)j->blk;
    struct afb_stfile st;
    struct afb_situation s;
    uint32_t fp_free, fp_next;
    unsigned plan;
    long t0 = current_tick;
    int rc = 0;

    if (j->blk_len < sizeof(*wk))
        return XIDX_JOB_FAIL;
    j->n = 0;
    state_read(&st);
    afb_vol_fp(&fp_free, &fp_next);
    s.on = afb_on();
    s.state = st.state;
    s.links_present = dir_exists(AFB_LINK_DIR);
    s.when = when;
    s.fp_same = fp_free == st.fp_free && fp_next == st.fp_next;
    if (when == AFB_AT_BOOT)
    {
        /* Our own writes after the links were made move FSInfo, so a
         * boot trusts the library's boot stamp instead: written at our
         * clean shutdown, its digest over the music tree matched (a host
         * that edited or a checker that cut an original changes it), and
         * no clusters were freed since (a host that deleted an unlocked
         * link frees the original's; ours only allocate after it) */
        struct boot_stamp bs;
        enum boot_why why;

        s.fp_same = boot_stamp_saved(&bs, &why) && why == BOOT_WHY_TRUST
                    && fp_free <= bs.fsinfo_free + 2;
    }
    plan = afb_plan(&s);
    ALOG("plan=%u state=%u links=%d when=%d fp=%d", plan, s.state,
         s.links_present, when, s.fp_same);
    if (!plan)
    {
        afb_live(st.state == AFB_ST_LIVE);
        return XIDX_JOB_FRESH;
    }
    if (plan & AFB_DO_VERIFY)
        verify_links(wk);
    if (plan & AFB_DO_PARK)
        ALOG("park rc=%d at=job", afb_vol_park());
    if (plan & AFB_DO_TEARDOWN)
    {
        remove(AFB_STATE);
        afb_live(false);
        ALOG("off");
        return XIDX_JOB_BUILT;
    }
    if (plan & AFB_DO_UNPARK)
    {
        int32_t n = 0;

        rc = unpark(wk, &st, j->stop, &n);
        j->n = n;
        ALOG("unpark rc=%d links=%ld ms=%ld", rc, (long)n,
             (current_tick - t0) * 1000 / HZ);
    }
    afb_live(rc >= 0);
    return rc == -2 ? XIDX_JOB_STOPPED : rc < 0 ? XIDX_JOB_FAIL
                                               : XIDX_JOB_BUILT;
}
