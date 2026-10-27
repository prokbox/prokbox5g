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

/* Phase 6: the per-book resume store's runtime (ab_store.h; books.dat).
 *
 * Memory: the header and sector index live in the book block
 * (ab_book.h, struct ab_store_index); here a 512 B sector buffer, the
 * 32 B pending record and a few words (~600 B of statics). Inserts
 * stream the file through that buffer plus one 512 B buffer on the UI
 * thread's stack: no allocation ever, so a first save while playing
 * costs 0 B (Rev 1 item 2, rule 13).
 *
 * Threads: the audio thread only copies into the pending record
 * (ab_store_pending(), ab_store_pending_now() for pause/stop: the
 * write-now lives here, H1). One persistent DISK_EVENT_SPINUP handler
 * (never consumed, so it is armed at every flush) runs on:
 *  - the storage thread after storage ran: ONE existing record in place
 *    (Rev 1 item 7), a fresh checkpoint if a book plays and none is
 *    pending; at most once per 30 s (call_storage_idle_notifys());
 *  - the UI thread from misc.c system_flush() (USB connect, shutdown:
 *    clean_shutdown() stops audio first, so the stop record is pending):
 *    everything now, plus a stop record if a book still plays.
 * Everything else (inserts, the seed, forced checkpoints, write-now)
 * runs in the UI pass (np_pb_idle()). A record older than the last one
 * written for its book is never written. All file access holds 'busy'
 * (threads on one core are cooperative: test-and-set with no yield
 * between is atomic); the storage thread never waits for it, it leaves
 * the work to the UI.
 *
 * Storage per action (also in the report):
 *   load      1 open + (S + 2) sector reads, once per boot
 *   find/get  1 sector read (0 when it is the cached sector)
 *   update    1 sector read (0 cached) + 1 x 32 B write + 32 B read back
 *   insert    stream: (S + 1) sector reads + (S' + 2) sector writes into
 *             books.tmp, remove, rename (+1 pass of S reads at the cap)
 *
 * Host-tested (tests/abbook/test_abstore.c, AB_HOST_TEST): power cuts at
 * every write, torn sectors, books.tmp recovery, eviction at the cap. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef AB_HOST_TEST
#include "ab_host.h"            /* tests/abbook: POSIX shims */
#else
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "thread.h"
#include "file.h"
#include "ata_idle_notify.h"
#include "audio.h"              /* audio_status(): paused */
#include "logf.h"
#include "ipodui/anim.h"        /* UPDATE_TRACE */
#include "ipodui/artcache.h"    /* ARTCACHE_DIR */
#include "ipodui/library.h"     /* P6 glue: checkpoint, wake, seed walk */
#define AB_STORE_DAT  ARTCACHE_DIR "/books.dat"
#define AB_STORE_TMP  ARTCACHE_DIR "/books.tmp"
#define AB_STORE_OLD  ARTCACHE_DIR "/books.old"   /* safefile prev (#91) */
#endif
#include "ab_book.h"
#include "ab_store.h"
#include "ipodui/fe_be.h"             /* P19 S2 */
#include "safefile.h"

#define FORCE_TICKS   (30 * 60 * HZ)    /* ⚠️12: one forced wake / 30 min */
#define SEED_MAX      256               /* records per seed (one stream) */
/* AB_TR_STORE_W + whether the write forced its own wake (the 30 min
 * checkpoint) or rode one (T's check_trace caps forced wakes) */
#define TR_STORE_W    AB_TR_STORE_W " forced=%d"

static union {
    struct ab_rec rec[AB_STORE_PER_SECT];
    uint8_t b[AB_STORE_SECTOR];
} sbuf;                                 /* the cached record sector */
static int cache_s = -1;                /* its record-sector index */
static uint32_t cache_gen;              /* index gen it was read under */

static struct ab_rec pending;           /* the latest checkpoint */
static volatile bool pend_valid, pend_now, need_insert;
static volatile bool busy;
static ab_key_t last_key;               /* the last record written */
static uint32_t last_stamp;
static bool last_valid;
static uint32_t hdr_seq;                /* the newer header's seq */
static bool seeded = true;              /* until a load says otherwise */
static bool seed_failed;                /* a write failed: next boot */
static bool loaded;
static long last_write;                 /* tick of the last record write */
static unsigned int ui_thread;
/* #67 rule 3: the newest unseeded written record (key + stamp, published
 * together under disable_irq: the storage thread writes them too), and
 * what ab_store_pin_key() answered last (UI thread only). 24 B. */
static ab_key_t pin_key;
static uint32_t pin_stamp;
static bool pin_valid;
static ab_key_t pin_last;
static uint32_t pin_gen;
static uint8_t pin_src;
static void idle_register(void);
static bool stamp_before(uint32_t a, uint32_t b);
static void pin_note(const struct ab_rec *r);

static struct ab_store_index *ix_get(void)
{
    struct ab_book_block *b = ab_book_block();

    return b ? &b->store : NULL;
}

static bool sec_read(int fd, int s, void *buf)
{
    return lseek(fd, (off_t)s * AB_STORE_SECTOR, SEEK_SET)
               == (off_t)s * AB_STORE_SECTOR
           && read(fd, buf, AB_STORE_SECTOR) == AB_STORE_SECTOR;
}

static bool sec_write(int fd, int s, const void *buf)
{
    return lseek(fd, (off_t)s * AB_STORE_SECTOR, SEEK_SET)
               == (off_t)s * AB_STORE_SECTOR
           && write(fd, buf, AB_STORE_SECTOR) == AB_STORE_SECTOR;
}

/* the header copy at the end of a file whose sector 0 is bad */
static bool tail_hdr(int fd, struct ab_store_hdr *h)
{
    off_t end = lseek(fd, 0, SEEK_END);

    if (end < 2 * AB_STORE_SECTOR)
        return false;
    if (!sec_read(fd, (int)(end / AB_STORE_SECTOR) - 1, sbuf.b))
        return false;
    memcpy(h, sbuf.b, sizeof(*h));
    return true;
}

/* ---- load ------------------------------------------------------------- */

static int open_recover(void)
{
    int fd = open(AB_STORE_DAT, O_RDONLY);

    if (fd < 0)
    {
        /* safe-replace recovery (#91): an insert cut between books.dat ->
         * books.old and books.tmp -> books.dat puts books.old back */
        safefile_recover(AB_STORE_DAT, NULL, AB_STORE_OLD);
        fd = open(AB_STORE_DAT, O_RDONLY);
    }
    if (fd >= 0)
    {
        /* an interrupted insert: books.dat holds the previous or the new
         * state, both complete. A cut inside a rename can leave books.tmp
         * or books.old on books.dat's own chain (#91 H1): then only the
         * second name goes, never the data */
        safefile_remove(AB_STORE_TMP, AB_STORE_DAT);
        safefile_remove(AB_STORE_OLD, AB_STORE_DAT);
        return fd;
    }
    /* builds before #91 removed books.dat, then renamed: books.tmp is
     * complete (closed before books.dat goes; the load checks its seal) */
    fd = open(AB_STORE_TMP, O_RDONLY);
    if (fd < 0)
        return -1;
    close(fd);
    if (rename(AB_STORE_TMP, AB_STORE_DAT) < 0)
        return -1;
    UPDATE_TRACE("ab store recovered %s", "books.tmp");
    return open(AB_STORE_DAT, O_RDONLY);
}

bool ab_store_load(void)
{
    struct ab_store_index *ix = ix_get();
    struct ab_store_hdr h0, h1;
    const struct ab_store_hdr *h;
    int fd, s;
    uint32_t gen, clock;

    if (!loaded)
    {
        ui_thread = thread_self();
        loaded = true;
        last_write = current_tick;
        idle_register();                /* persistent: every storage wake
                                           and every flush (H1) */
    }
    if (!ix)
        return false;
    gen = ix->gen;
    clock = ix->clock;                  /* stamps never go backwards */
    memset(ix, 0, sizeof(*ix));
    ix->gen = gen + 1;
    ix->clock = clock;
    cache_s = -1;
    hdr_seq = 0;
    seeded = false;
    {
        int level = disable_irq_save();

        pin_valid = false;              /* found again from the records */
        restore_irq(level);
    }
    fd = open_recover();
    if (fd < 0)
        return false;                   /* no store yet: seed it */
    memset(&h0, 0, sizeof(h0));
    memset(&h1, 0, sizeof(h1));
    if (sec_read(fd, 0, sbuf.b))
        memcpy(&h0, sbuf.b, sizeof(h0));
    if (ab_store_hdr_ok(&h0))
    {
        if (sec_read(fd, (int)h0.sectors + 1, sbuf.b))
            memcpy(&h1, sbuf.b, sizeof(h1));
    }
    else
        tail_hdr(fd, &h1);
    h = ab_store_hdr_pick(&h0, &h1);
    if (!h)
    {
        close(fd);
        UPDATE_TRACE("ab store bad %s", "header");
        return false;
    }
    hdr_seq = h->seq;
    seeded = h->seeded != 0;
    for (s = 0; s < (int)h->sectors; s++)
    {
        int i;

        if (!sec_read(fd, s + 1, sbuf.b))
            memset(sbuf.b, 0, sizeof(sbuf.b));  /* reads as unplayed */
        ab_store_index_add(ix, s, sbuf.rec, AB_STORE_PER_SECT);
        for (i = 0; i < AB_STORE_PER_SECT; i++)
            pin_note(&sbuf.rec[i]);     /* #67 rule 3: no extra read */
    }
    close(fd);
    ix->sectors = h->sectors;
    ix->count = h->count;
    if (h->clock > ix->clock)
        ix->clock = h->clock;
    if (last_valid && stamp_before(ix->clock, last_stamp + 1))
        last_valid = false;             /* a clock behind the last write:
                                           a new store (host test reboot) */
    return true;
}

/* ---- lookup ----------------------------------------------------------- */

/* Reads key's sector into sbuf; returns the record-sector index and the
 * slot (< 0: not there), -1 with no sector, -2 on a read error. Holds
 * 'busy'. */
static int locate(ab_key_t key, int *slot)
{
    struct ab_store_index *ix = ix_get();
    int s, n;

    *slot = -1;
    if (!ix || !key)
        return -1;
    s = ab_store_sector_for(ix, key);
    if (s < 0)
        return -1;
    if (cache_s != s || cache_gen != ix->gen)
    {
        int fd = open(AB_STORE_DAT, O_RDONLY);
        bool ok = fd >= 0 && sec_read(fd, s + 1, sbuf.b);

        if (fd >= 0)
            close(fd);
        if (!ok)
        {
            cache_s = -1;
            return -2;                  /* I/O error */
        }
        cache_s = s;
        cache_gen = ix->gen;
    }
    n = (int)ix->count - s * AB_STORE_PER_SECT;
    if (n > AB_STORE_PER_SECT)
        n = AB_STORE_PER_SECT;
    if (n > 0)
        *slot = ab_store_rec_find(sbuf.rec, n, key);
    return s;
}

static bool take(void)
{
    if (busy)
        return false;
    busy = true;
    return true;
}

enum ab_rec_state ab_store_find(ab_key_t key)
{
    enum ab_rec_state st = AB_REC_NONE;
    int slot;

    if (!take())
        return AB_REC_NONE;
    if (locate(key, &slot) >= 0 && slot >= 0)
        st = ab_rec_state(&sbuf.rec[slot]);
    busy = false;
    return st;
}

bool ab_store_get(ab_key_t key, struct ab_rec *out)
{
    bool ok = false;
    int slot;

    if (!take())
        return false;
    if (locate(key, &slot) >= 0 && slot >= 0 && ab_rec_ok(&sbuf.rec[slot]))
    {
        *out = sbuf.rec[slot];
        ok = true;
    }
    busy = false;
    return ok;
}

/* ---- writes ------------------------------------------------------------ */

/* One record in place: 1 if written and read back equal, 0 if the key
 * has no record (an insert is needed), -1 on an I/O error. Holds busy. */
static int write_in_place(const struct ab_rec *r)
{
    struct ab_rec back;
    off_t off;
    int s, slot, fd, ok;

    s = locate(r->book_key, &slot);
    if (s == -2)
        return -1;
    if (s < 0 || slot < 0)
        return 0;
    off = (off_t)(s + 1) * AB_STORE_SECTOR + slot * (off_t)sizeof(*r);
    fd = open(AB_STORE_DAT, O_RDWR);
    if (fd < 0)
        return -1;
    ok = lseek(fd, off, SEEK_SET) == off
         && write(fd, r, sizeof(*r)) == (ssize_t)sizeof(*r)
         && lseek(fd, off, SEEK_SET) == off
         && read(fd, &back, sizeof(back)) == (ssize_t)sizeof(back)
         && !memcmp(&back, r, sizeof(back));
    close(fd);
    sbuf.rec[slot] = *r;
    if (!ok)
    {
        cache_s = -1;
        UPDATE_TRACE("ab store write failed book=%08lx",
                     (unsigned long)(uint32_t)r->book_key);
        return -1;
    }
    return 1;
}

static int cls_of(const struct ab_rec *r)
{
    enum ab_rec_state st = ab_rec_state(r);

    return st == AB_REC_FINISHED ? 1 : st == AB_REC_STARTED ? 2 : 0;
}

struct stream {
    int fd;
    int on;                             /* records in out */
    int s;                              /* record sectors written */
    uint32_t count;
    struct ab_store_index *ix;
    struct ab_rec *out;                 /* AB_STORE_PER_SECT */
    bool ok;
};

static void emit_flush(struct stream *w)
{
    if (!w->on)
        return;
    if (w->on < AB_STORE_PER_SECT)
        memset(&w->out[w->on], 0,
               (AB_STORE_PER_SECT - w->on) * sizeof(struct ab_rec));
    if (w->s >= AB_STORE_SECTORS_MAX || !sec_write(w->fd, w->s + 1, w->out))
        w->ok = false;
    else
        ab_store_index_add(w->ix, w->s, w->out, w->on);
    w->s++;
    w->on = 0;
}

static void emit(struct stream *w, const struct ab_rec *r)
{
    if (w->count >= AB_STORE_MAX)
    {
        w->ok = false;
        return;
    }
    w->out[w->on++] = *r;
    w->count++;
    if (w->on == AB_STORE_PER_SECT)
        emit_flush(w);
}

/* The global eviction pick (D10) over the file: its record index, or
 * -1. Reads every record sector once (only at the cap). */
static int evict_scan(int fd, uint32_t count)
{
    int s, best = -1, best_cls = 3;
    uint32_t best_stamp = 0;

    for (s = 0; s * AB_STORE_PER_SECT < (int)count; s++)
    {
        int n = (int)count - s * AB_STORE_PER_SECT, i;

        if (n > AB_STORE_PER_SECT)
            n = AB_STORE_PER_SECT;
        if (!sec_read(fd, s + 1, sbuf.b))
            return s * AB_STORE_PER_SECT;   /* unreadable: lost anyway */
        i = ab_store_evict_pick(sbuf.rec, n);
        if (i >= 0)
        {
            int c = cls_of(&sbuf.rec[i]);
            if (c < best_cls || (c == best_cls && sbuf.rec[i].stamp < best_stamp))
            {
                best = s * AB_STORE_PER_SECT + i;
                best_cls = c;
                best_stamp = sbuf.rec[i].stamp;
            }
        }
    }
    return best;
}

/* Rewrites books.dat merging add[0..n) (sorted, unique keys): an added
 * key replaces the file's record if 'replace', else the file's is kept.
 * books.tmp + remove + rename. UI thread, holds busy. */
static bool rewrite(const struct ab_rec *add, int n, bool replace,
                    bool seed_done)
{
    struct ab_store_index *ix = ix_get();
    struct ab_rec out[AB_STORE_PER_SECT];   /* 512 B, UI stack */
    struct ab_store_hdr h;
    struct stream w;
    uint32_t count, i, gen, clock;
    int fdi, ai = 0, evict = -1, s_in = -1;

    if (!ix)
        return false;
    count = ix->count;
    gen = ix->gen;
    fdi = count ? open(AB_STORE_DAT, O_RDONLY) : -1;
    if (count && fdi < 0)
        count = 0;                      /* lost: start again (logged) */
    if (count + n > AB_STORE_MAX && fdi >= 0 && n == 1)
    {
        evict = evict_scan(fdi, count);
        if (evict >= 0)
        {
            if (sec_read(fdi, ab_store_rec_sector(evict), sbuf.b))
                UPDATE_TRACE(AB_TR_STORE_EVICT, (unsigned long)(uint32_t)
                             sbuf.rec[evict % AB_STORE_PER_SECT].book_key);
        }
    }
    memset(&w, 0, sizeof(w));
    w.fd = open(AB_STORE_TMP, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (w.fd < 0)
    {
        if (fdi >= 0)
            close(fdi);
        return false;
    }
    w.ix = ix;
    w.out = out;
    w.ok = true;
    clock = ix->clock;                  /* pending stamps stay newer (L5) */
    memset(ix, 0, sizeof(*ix));         /* rebuilt as sectors go out */
    ix->gen = gen + 1;
    ix->clock = clock;
    cache_s = -1;
    memset(sbuf.b, 0, sizeof(sbuf.b));
    w.ok = sec_write(w.fd, 0, sbuf.b);  /* the header comes last */

    for (i = 0; w.ok && i < count; i++)
    {
        const struct ab_rec *r;
        int s = (int)(i / AB_STORE_PER_SECT);

        if (s != s_in)
        {
            if (!sec_read(fdi, s + 1, sbuf.b))
                memset(sbuf.b, 0, sizeof(sbuf.b));
            s_in = s;
        }
        r = &sbuf.rec[i % AB_STORE_PER_SECT];
        if ((int)i == evict || r->book_key == 0)
            continue;
        while (ai < n && add[ai].book_key < r->book_key)
            emit(&w, &add[ai++]);
        if (ai < n && add[ai].book_key == r->book_key)
        {
            emit(&w, replace ? &add[ai] : r);
            ai++;
            continue;
        }
        emit(&w, r);
    }
    while (w.ok && ai < n)
        emit(&w, &add[ai++]);
    emit_flush(&w);
    if (fdi >= 0)
        close(fdi);

    memset(&h, 0, sizeof(h));
    h.seq = hdr_seq + 1;
    h.count = w.count;
    h.sectors = ab_store_sectors(w.count);
    h.clock = ix->clock;
    h.seeded = (seeded || seed_done) ? 1 : 0;
    ab_store_hdr_seal(&h);
    memset(sbuf.b, 0, sizeof(sbuf.b));
    memcpy(sbuf.b, &h, sizeof(h));
    if (w.ok)
        w.ok = sec_write(w.fd, 0, sbuf.b)
               && sec_write(w.fd, (int)h.sectors + 1, sbuf.b);
    if (close(w.fd) < 0)
        w.ok = false;
    if (!w.ok)
    {
        remove(AB_STORE_TMP);
        UPDATE_TRACE("ab store write failed book=%08lx", 0ul);
        ab_store_load();                /* the old file is intact */
        return false;
    }
    /* never a rename onto books.dat (#91): a failed victim removal
     * would leave two entries; a cut leaves books.dat or books.old */
    if (safefile_replace(AB_STORE_TMP, AB_STORE_DAT, AB_STORE_OLD) < 0)
    {
        safefile_remove(AB_STORE_TMP, AB_STORE_DAT);
        ab_store_load();                /* the old file is intact */
        return false;
    }
    hdr_seq = h.seq;
    seeded = h.seeded != 0;
    ix->sectors = h.sectors;
    ix->count = h.count;
    if (h.clock > ix->clock)
        ix->clock = h.clock;
    cache_s = -1;
    return true;
}

/* wrap-safe: stamp a is older than b */
static bool stamp_before(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) < 0;
}

/* A record older than the last one written for its key is never written
 * (H1: a stale track-change checkpoint must not overwrite a stop) */
static bool stale(const struct ab_rec *r)
{
    return last_valid && r->book_key == last_key
           && stamp_before(r->stamp, last_stamp);
}

/* #67 rule 3, any thread: r (just loaded or written) is the newest
 * unseeded record if its stamp is not older than the current one's. Key
 * and stamp change together under the irq lock, so the UI thread never
 * sees a torn 64-bit key (two word stores on ARM7). */
static void pin_note(const struct ab_rec *r)
{
    int level;

    if (!r->book_key || (r->flags & AB_REC_F_SEEDED) || !ab_rec_ok(r))
        return;
    level = disable_irq_save();
    if (!pin_valid || !stamp_before(r->stamp, pin_stamp))
    {
        pin_key = r->book_key;
        pin_stamp = r->stamp;
        pin_valid = true;
    }
    restore_irq(level);
}

/* every record write ends here (put_locked(), storage_pass()) */
static void wrote(const struct ab_rec *r)
{
    last_write = current_tick;
    last_key = r->book_key;
    last_stamp = r->stamp;
    last_valid = true;
    pin_note(r);
}

ab_key_t ab_store_pin_key(uint32_t *gen)
{
    struct ab_book_block *b = ab_book_block();
    ab_key_t k = 0;
    uint8_t src = AB_PIN_NONE;
    int level;

    /* 1: the live book (map_ok() inside ab_book_playing() needs a
     * nonzero book_key; a key not in the books view pins nothing) */
    if (b && ab_book_playing() && b->map.book_key)
    {
        k = b->map.book_key;
        src = AB_PIN_PLAY;
    }
    level = disable_irq_save();
    if (!k && pend_valid && pending.book_key)
    {
        k = pending.book_key;           /* 2: written or not */
        src = AB_PIN_PENDING;
    }
    if (!k && pin_valid)
    {
        k = pin_key;                    /* 3: newest unseeded stamp */
        src = AB_PIN_STAMP;
    }
    restore_irq(level);
    if (k != pin_last)
    {
        pin_last = k;
        pin_gen++;
    }
    pin_src = src;
    if (gen)
        *gen = pin_gen;
    return k;
}

enum ab_pin_src ab_store_pin_src(void)
{
    return (enum ab_pin_src)pin_src;
}

static bool put_locked(const struct ab_rec *r, const char *how, int forced)
{
    int rc;

    (void)how; (void)forced;            /* trace only */

    if (stale(r))
    {
        UPDATE_TRACE("ab store stale book=%08lx ms=%lu",
                     (unsigned long)(uint32_t)r->book_key,
                     (unsigned long)r->book_ms);
        return true;                    /* a newer one is on disk */
    }
    rc = write_in_place(r);
    /* no record: insert; a record sector that cannot be read (a file
     * cut short by a power cut): rewrite, which keeps every readable
     * sector and drops the lost ones */
    if (rc <= 0)
        rc = rewrite(r, 1, true, false) ? 1 : -1;
    if (rc > 0)
    {
        wrote(r);
        UPDATE_TRACE(TR_STORE_W, how,
                     (unsigned long)(uint32_t)r->book_key,
                     (unsigned long)r->book_ms, forced);
    }
    return rc > 0;
}

/* pending is r's key and no newer than r: r supersedes it */
static void pending_clear_upto(const struct ab_rec *r)
{
    int level = disable_irq_save();

    if (pend_valid && pending.book_key == r->book_key
        && !stamp_before(r->stamp, pending.stamp))
        pend_valid = pend_now = need_insert = false;
    restore_irq(level);
}

bool ab_store_put(const struct ab_rec *r)
{
    bool ok;

    if (!r->book_key || !ix_get())
        return false;
    if (!take())
    {
        ab_store_pending_now(r);        /* the next pass writes it */
        return false;
    }
    ok = put_locked(r, "now", 0);
    busy = false;
    if (ok)
        pending_clear_upto(r);
    return ok;
}

/* the pending record: never replaced by an older one of the same book */
static void pend_set(const struct ab_rec *r, bool now)
{
    int level = disable_irq_save();

    if (!pend_valid || pending.book_key != r->book_key
        || !stamp_before(r->stamp, pending.stamp))
    {
        pending = *r;
        pend_valid = true;
    }
    if (now)
        pend_now = true;
    restore_irq(level);
}

void ab_store_pending(const struct ab_rec *r)
{
    pend_set(r, false);
}

/* Pause / stop (audio thread, H1): the record lives in the store, so any
 * flush (USB connect, shutdown: misc.c system_flush() on the UI thread)
 * writes it even if no UI pass ran in between */
void ab_store_pending_now(const struct ab_rec *r)
{
    pend_set(r, true);
}

/* ---- idle / checkpoints ------------------------------------------------- */

/* the pending record, copied out under the irq lock */
static bool pending_copy(struct ab_rec *r)
{
    int level = disable_irq_save();
    bool v = pend_valid;

    if (v)
        *r = pending;
    restore_irq(level);
    return v;
}

static void pending_done(const struct ab_rec *r)
{
    int level = disable_irq_save();

    if (pend_valid && pending.book_key == r->book_key
        && pending.stamp == r->stamp)
        pend_valid = pend_now = need_insert = false;
    restore_irq(level);
}

/* UI thread: the pending record, now (one wake if storage sleeps) */
static void pending_write_now(void)
{
    struct ab_rec r;

    if (pending_copy(&r) && take())
    {
        if (put_locked(&r, "now", 0))
            pending_done(&r);
        busy = false;
    }
}

static int seed_maybe(void);

/* storage thread, storage just ran: in-place only (small stack) */
static void storage_pass(void)
{
    struct ab_rec r;

    /* paused: the pause's own record (write-now) stands; a fresh
     * checkpoint here would turn a FINISHED pause back to STARTED.
     * call_storage_idle_notifys() runs this at most once per 30 s */
    if (!pend_valid)
    {
        if ((audio_status() & AUDIO_STATUS_PAUSE)
            || !ab_book_checkpoint(&r, false))
            return;
        ab_store_pending(&r);
    }
    if (!pending_copy(&r) || !take())
        return;
    if (stale(&r))
        pending_done(&r);
    else switch (write_in_place(&r))
    {
    case 1:
        wrote(&r);
        UPDATE_TRACE(TR_STORE_W, "idle",
                     (unsigned long)(uint32_t)r.book_key,
                     (unsigned long)r.book_ms, 0);
        pending_done(&r);
        break;
    case 0:
    case -1:                            /* unreadable sector: rewrite */
        need_insert = true;             /* the UI thread inserts it */
        np_pb_ab_wake();
        break;
    default:
        break;
    }
    busy = false;
}

/* UI thread from a flush (USB connect, shutdown): everything now. A book
 * still playing (USB reaches the UI before the audio thread stops) gets
 * a stop record of its own; the audio thread's later stop is newer and
 * is written by the pass its wake posts. */
static void flush_pass(void)
{
    struct ab_rec r;

    if (ab_book_playing() && ab_book_checkpoint(&r, true))
        ab_store_pending_now(&r);
    if (pend_valid)
        pending_write_now();
}

static void idle_cb(void)
{
    if (fe_be_is_service_thread())     /* P19 S2: be_svc or the inline UI */
        flush_pass();
    else
        storage_pass();
}

#ifndef AB_HOST_TEST
static void idle_ev(unsigned short id, void *ev_data, void *user_data)
{
    (void)id; (void)ev_data; (void)user_data;
    idle_cb();
}
static void idle_register(void)
{
    /* not register_storage_idle_func(): that one is one-shot, and a
     * consumed callback was never re-armed before a stop or a flush */
    add_event_ex(DISK_EVENT_SPINUP, false, idle_ev, NULL);
}
#else
static void idle_register(void)
{
    register_storage_idle_func(idle_cb);
}
#endif

int ab_store_idle(bool ui_thread_call)
{
    struct ab_rec r;
    long left;

    if (!ui_thread_call)
    {
        storage_pass();
        return -1;
    }
    /* write-now (pause, stop), an insert the storage thread left, or a
     * stopped book's last checkpoint */
    if (pend_valid && (pend_now || need_insert || !ab_book_playing()))
        pending_write_now();
    if (!ab_book_playing())
        return seed_maybe();
    left = last_write + FORCE_TICKS - current_tick;
    if (left <= 0)
    {
        /* no fill for 30 min (a 60 MB fill lasts ~2 h at 64 kbps) */
        if (ab_book_checkpoint(&r, false) && take())
        {
            if (put_locked(&r, "idle", 1))  /* forced (⚠️12) */
                pending_clear_upto(&r);
            busy = false;
        }
        last_write = current_tick;
        left = FORCE_TICKS;
    }
    return (int)left;
}

/* ---- the tagcache migration (§3.6, Rev 1 item 12) ------------------------ */

/* Runs when the library says a load finished (lib_committed() ->
 * lib_book_seed_begin() is owed, M2) and audio is stopped; a failed write
 * is not retried until the next boot */
static int seed_maybe(void)
{
    if (!seeded && !seed_failed && ix_get())
        ab_store_seed();
    return -1;
}

static void sort_recs(struct ab_rec *r, int n)
{
    int i, j;

    for (i = 1; i < n; i++)             /* n <= SEED_MAX, once per store */
    {
        struct ab_rec t = r[i];
        for (j = i; j > 0 && r[j - 1].book_key > t.book_key; j--)
            r[j] = r[j - 1];
        r[j] = t;
    }
}

void ab_store_seed(void)
{
    struct ab_rec *recs;
    int h, n, i, k;

    if (seeded || seed_failed || !ix_get() || !take())
        return;
    /* the walk allocates its views: never while audio plays (rule 13);
     * < 0 also when no library load is owed since the last try */
    h = lib_book_seed_begin(SEED_MAX * sizeof(struct ab_rec), (void **)&recs);
    if (h < 0)
    {
        busy = false;
        return;
    }
    n = lib_book_seed(recs, SEED_MAX);
    if (n >= 0)                         /* < 0: changed; the load re-owes */
    {
        struct ab_store_index *ix = ix_get();

        for (i = 0; i < n; i++)
        {
            recs[i].stamp = ix->clock++;
            recs[i].flags = AB_REC_F_STARTED | AB_REC_F_SEEDED;
            ab_rec_seal(&recs[i]);
        }
        sort_recs(recs, n);
        for (i = k = 0; i < n; i++)     /* one record per book */
            if (!k || recs[k - 1].book_key != recs[i].book_key)
                recs[k++] = recs[i];
        if (rewrite(recs, k, false, true))
            UPDATE_TRACE(AB_TR_SEED, k);
        else
        {
            seed_failed = true;         /* next boot */
            UPDATE_TRACE("ab seed failed n=%d", k);
        }
    }
    busy = false;
    lib_book_seed_end(h);
}
