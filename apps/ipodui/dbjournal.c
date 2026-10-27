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
 * iPod UI shell: stable-identity absolute statistics journal
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
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#ifdef DBJOURNAL_HOST
#include <fcntl.h>
#include <unistd.h>
#else
#include "config.h"
#include "file.h"
#include "mv.h"         /* volume_size() */
#endif
#include "dbjournal.h"
#include "safefile.h"
#ifndef DBJOURNAL_HOST
#include "crc32.h"
#endif

#ifdef DBJOURNAL_HOST
static uint64_t masks[DBJOURNAL_TEST_NOPS];
static int errors[DBJOURNAL_TEST_NOPS];
static uint64_t write_calls, write_bytes;
void dbjournal_test_reset(void)
{ memset(masks, 0, sizeof(masks)); write_calls = write_bytes = 0; }
void dbjournal_test_write_stats(uint64_t *calls, uint64_t *bytes)
{ *calls = write_calls; *bytes = write_bytes; }
void dbjournal_test_fail(enum dbjournal_test_op op, uint64_t mask, int error)
{ masks[op] = mask; errors[op] = error; }
static bool fails(enum dbjournal_test_op op)
{
    bool failed = (masks[op] & 1) != 0;
    masks[op] >>= 1;
    if (failed) errno = errors[op];
    return failed;
}
static int test_open(const char *p, int flags, ...)
{ return fails(DBJOURNAL_TEST_OPEN) ? -1 : open(p, flags, 0666); }
static ssize_t test_read(int fd, void *p, size_t n)
{
    if (fails(DBJOURNAL_TEST_READ)) return -1;
    if (fails(DBJOURNAL_TEST_SHORT_READ) && n) n--;
    return read(fd, p, n);
}
static ssize_t test_write(int fd, const void *p, size_t n)
{
    write_calls++;
    write_bytes += n;
    if (fails(DBJOURNAL_TEST_WRITE)) return -1;
    if (fails(DBJOURNAL_TEST_SHORT_WRITE) && n) n--;
    return write(fd, p, n);
}
static int test_flush(int fd)
{ return fails(DBJOURNAL_TEST_FLUSH) ? -1 : fsync(fd); }
static int test_truncate(int fd, off_t n)
{ return fails(DBJOURNAL_TEST_TRUNCATE) ? -1 : ftruncate(fd, n); }
static int test_rename(const char *a, const char *b)
{ return fails(DBJOURNAL_TEST_RENAME) ? -1 : rename(a, b); }
/* safefile.c is built with -Drename=safefile_test_rename (run.sh): its
 * renames take the same faults */
int safefile_test_rename(const char *a, const char *b);
int safefile_test_rename(const char *a, const char *b)
{ return test_rename(a, b); }
static int test_close(int fd)
{
    bool fail = fails(DBJOURNAL_TEST_CLOSE);
    int rc = close(fd);
    return fail ? -1 : rc;
}
#define open test_open
#define read test_read
#define write test_write
#define fsync test_flush
#define ftruncate test_truncate
#define rename test_rename
#define close test_close
#endif

#define FILE_MAGIC 0x4a535049u  /* IPSJ */
#define FRAME_MAGIC 0x52535049u /* IPSR */
/* 2: frame tags may carry the caller's delta/relative flags (#85). A
 * version-1 file or frame (absolute values only) still reads and appends:
 * its tags never had those bits. */
#define JOURNAL_VERSION 2
#define JOURNAL_VERSION_OK(v) ((v) - 1u <= JOURNAL_VERSION - 1u)
#ifdef DBJOURNAL_HOST
static uint32_t write_version = JOURNAL_VERSION;   /* old files, in tests */
void dbjournal_test_version(uint32_t version)
{ write_version = version; }
#else
#define write_version JOURNAL_VERSION
#endif
#define NAME_PRIMARY "database_stats.jnl"
#define NAME_PREVIOUS "database_stats.prev"
#define NAME_TEMP "database_stats.tmp"
#define NAME_BAD "database_stats.bad"
/* safefile prevs (#91): a checkpoint set aside while its replacement is
 * renamed into place, one per name so recovery knows where it goes */
#define NAME_PRIMARY_OLD "database_stats.jnl.old"
#define NAME_PREVIOUS_OLD "database_stats.prev.old"
/* L4: a complete compaction stage under its destination's own name, while
 * the inactive checkpoint it replaces goes; rolled forward at open */
#define NAME_PRIMARY_NEW "database_stats.jnl.new"
#define NAME_PREVIOUS_NEW "database_stats.prev.new"

struct journal_header {
    uint32_t magic, version;
    uint64_t base;
    uint32_t sum, reserved;
};
struct frame_header {
    uint32_t magic, version, bytes, path_bytes, tag;
    int32_t value;
    uint64_t seq;
    uint32_t sum, header_sum;
};
typedef char header_size_check[sizeof(struct journal_header) == 24 ? 1 : -1];
typedef char frame_size_check[sizeof(struct frame_header) == 40 ? 1 : -1];
typedef char journal_limit_check[DBJOURNAL_MAX_BYTES <= INT32_MAX ? 1 : -1];

struct scan {
    uint64_t base, seq;
    uint32_t end;
    bool header_good;
};
struct reader {
    int fd;
    unsigned char *buf;
    size_t size, at, avail;
    uint32_t offset, end;
};

static uint32_t sum_add(uint32_t crc, const void *data, size_t bytes)
{
#ifdef DBJOURNAL_HOST
    static const uint32_t tab[16] = {
        0, 0x1db71064, 0x3b6e20c8, 0x26d930ac,
        0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
        0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
        0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c
    };
    const unsigned char *p = data;
    while (bytes--)
    {
        crc ^= *p++;
        crc = (crc >> 4) ^ tab[crc & 15];
        crc = (crc >> 4) ^ tab[crc & 15];
    }
    return crc;
#else
    /* the device's shared crc_32r(): the same nibble table */
    return crc_32r(data, bytes, crc);
#endif
}
static uint32_t checksum(const void *p, size_t n)
{ return ~sum_add(~0u, p, n); }

static bool path(char out[DBJOURNAL_ROOT_MAX], const struct dbjournal *j,
                  const char *name)
{
    int n = snprintf(out, DBJOURNAL_ROOT_MAX, "%s/%s", j->root, name);
    return n >= 0 && n < DBJOURNAL_ROOT_MAX;
}
static const char *active_name(const struct dbjournal *j)
{ return j->staged ? NAME_TEMP : j->previous ? NAME_PREVIOUS : NAME_PRIMARY; }
static const char *old_name(const char *name)
{ return strcmp(name, NAME_PREVIOUS) ? NAME_PRIMARY_OLD : NAME_PREVIOUS_OLD; }
static const char *new_name(const char *name)
{ return strcmp(name, NAME_PREVIOUS) ? NAME_PRIMARY_NEW : NAME_PREVIOUS_NEW; }

void dbjournal_recover(const char *root)
{
    struct dbjournal j;
    char dest[DBJOURNAL_ROOT_MAX], tmp[DBJOURNAL_ROOT_MAX],
         prev[DBJOURNAL_ROOT_MAX];
    if (!root || !*root) return;
    memset(&j, 0, sizeof(j));
    j.root = root;
    bool tmp_ok = path(tmp, &j, NAME_TEMP);
    /* a compaction's complete stage (L4): a cut inside its rename from the
     * temp can leave the temp on its chain, and one inside its rename onto
     * the checkpoint name the stage on the checkpoint's chain. Only those
     * second names go here; dbjournal_open() rolls a stage forward */
    for (int i = 0; i < 2; i++)
    {
        const char *name = i ? NAME_PREVIOUS : NAME_PRIMARY;
        if (!path(dest, &j, name) || !path(prev, &j, new_name(name)))
            continue;
        if (tmp_ok && safefile_drop_alias(tmp, prev) < 0)
            tmp_ok = false;
        safefile_drop_alias(prev, dest);
    }
    if (path(dest, &j, NAME_PRIMARY) && path(prev, &j, NAME_PRIMARY_OLD))
    {
        safefile_recover(dest, NULL, prev);
        /* the temp is renamed onto either name: a cut inside a rename onto
         * the primary can leave it on the primary's chain (#91 H1). Only
         * that second name goes; on an undecided check it stays */
        if (tmp_ok && safefile_drop_alias(tmp, dest) < 0)
            tmp_ok = false;
    }
    if (path(dest, &j, NAME_PREVIOUS) && path(prev, &j, NAME_PREVIOUS_OLD))
        safefile_recover(dest, tmp_ok ? tmp : NULL, prev);
}

/* Check encoding without changing canonical bytes or treating them as a
 * destination path. Reject overlong encodings, surrogates and >U+10FFFF. */
static bool utf8(const char *s, size_t n)
{
    const unsigned char *p = (const unsigned char *)s;
    for (size_t i = 0; i < n;)
    {
        unsigned c = p[i++], need, value, min;
        if (c < 0x80) { if (!c) return false; continue; }
        if (c >= 0xc2 && c <= 0xdf) { need = 1; value = c & 31; min = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { need = 2; value = c & 15; min = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { need = 3; value = c & 7; min = 0x10000; }
        else return false;
        if (need > n - i) return false;
        while (need--)
        {
            if ((p[i] & 0xc0) != 0x80) return false;
            value = (value << 6) | (p[i++] & 63);
        }
        if (value < min || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
            return false;
    }
    return true;
}

/* Bounded bulk reader. At most one compacting memmove when a record crosses
 * a caller-buffer boundary; never a read() per tiny header/filename. */
static bool ensure(struct reader *r, size_t n)
{
    if (r->avail - r->at >= n) return true;
    size_t left = r->avail - r->at;
    memmove(r->buf, r->buf + r->at, left);
    r->offset += r->at;
    r->at = 0;
    r->avail = left;
    size_t want = r->size - left;
    uint32_t remain = r->end - r->offset - left;
    if (remain < want) want = remain;
    if (want && read(r->fd, r->buf + left, want) != (ssize_t)want) return false;
    r->avail += want;
    return r->avail >= n;
}

static enum dbjournal_result scan_file(struct dbjournal *j, const char *name,
    struct scan *s, dbjournal_apply_fn apply, void *opaque,
    const struct dbjournal_cursor *cursor, uint32_t limit)
{
    memset(s, 0, sizeof(*s));
    if (j->cancel && j->cancel(j->cancel_opaque)) return DBJOURNAL_CANCELLED;
    char filename[DBJOURNAL_ROOT_MAX];
    if (!path(filename, j, name)) return DBJOURNAL_INVALID;
    int fd = open(filename, O_RDONLY);
    if (fd < 0) return errno == ENOENT ? DBJOURNAL_MISSING : DBJOURNAL_IO;
    off_t end = lseek(fd, 0, SEEK_END);
    enum dbjournal_result rc = DBJOURNAL_OK;
    if (end < 0 || lseek(fd, 0, SEEK_SET) != 0) rc = DBJOURNAL_IO;
    else if (end < (off_t)sizeof(struct journal_header)
             || (uint64_t)end > DBJOURNAL_MAX_BYTES) rc = DBJOURNAL_CORRUPT;
    if (rc == DBJOURNAL_OK && limit)
    {
        if (limit < sizeof(struct journal_header) || limit > (uint64_t)end)
            rc = DBJOURNAL_INVALID;
        else
            end = limit;            /* immutable prefix of a captured snapshot */
    }
    struct reader r = { fd, j->scratch, j->bytes, 0, 0, 0,
                       rc == DBJOURNAL_OK ? (uint32_t)end : 0 };
    struct journal_header h;
    if (rc == DBJOURNAL_OK && !ensure(&r, sizeof(h))) rc = DBJOURNAL_IO;
    if (rc == DBJOURNAL_OK)
    {
        memcpy(&h, r.buf, sizeof(h));
        uint32_t sum = h.sum;
        h.sum = 0;
        if (h.magic != FILE_MAGIC || !JOURNAL_VERSION_OK(h.version) || h.reserved
            || sum != checksum(&h, sizeof(h))) rc = DBJOURNAL_CORRUPT;
        else
        {
            s->header_good = true;
            s->base = s->seq = h.base;
            s->end = r.at = sizeof(h);
        }
    }
    if (rc == DBJOURNAL_OK && cursor)
    {
        if (cursor->base != s->base || cursor->seq < s->base
            || cursor->end < sizeof(h) || cursor->end > r.end)
            rc = DBJOURNAL_INVALID;
        else
        {
            s->seq = cursor->seq;
            s->end = cursor->end;
            if (cursor->end <= r.avail)
                r.at = cursor->end;
            else if (lseek(fd, cursor->end, SEEK_SET) != (off_t)cursor->end)
                rc = DBJOURNAL_IO;
            else
            {
                r.offset = cursor->end;
                r.at = r.avail = 0;
            }
        }
    }
    while (rc == DBJOURNAL_OK && r.offset + r.at < r.end)
    {
        if (j->cancel && j->cancel(j->cancel_opaque))
        { rc = DBJOURNAL_CANCELLED; break; }
        uint32_t left = r.end - r.offset - r.at;
        if (left < sizeof(struct frame_header)) break; /* Cut final header. */
        if (!ensure(&r, sizeof(struct frame_header))) { rc = DBJOURNAL_IO; break; }
        struct frame_header f;
        memcpy(&f, r.buf + r.at, sizeof(f));
        uint32_t header_sum = f.header_sum;
        f.header_sum = 0;
        if (header_sum != checksum(&f, sizeof(f))
            || f.magic != FRAME_MAGIC || !JOURNAL_VERSION_OK(f.version)
            || !f.path_bytes || f.path_bytes > DBJOURNAL_PATH_MAX
            || f.bytes != ((sizeof(f) + f.path_bytes + 3) & ~3u)
            || f.seq <= s->seq)
        { rc = DBJOURNAL_CORRUPT; break; }
        if (left < f.bytes) break; /* Verified header, truncated payload. */
        if (!ensure(&r, f.bytes)) { rc = DBJOURNAL_IO; break; }
        const char *p = (const char *)r.buf + r.at + sizeof(f);
        uint32_t sum = f.sum;
        f.sum = 0;
        uint32_t got = ~sum_add(sum_add(~0u, &f, sizeof(f)), p, f.bytes - sizeof(f));
        if (sum != got || f.path_bytes < 2 || p[f.path_bytes - 1]
            || !utf8(p, f.path_bytes - 1)) { rc = DBJOURNAL_CORRUPT; break; }
        if (apply)
        {
            rc = apply(opaque, p, f.tag, f.value, f.seq);
            if (rc != DBJOURNAL_OK) break;
        }
        s->seq = f.seq;
        r.at += f.bytes;
        s->end = r.offset + r.at;
    }
    if (close(fd) < 0) rc = DBJOURNAL_IO;
    return rc;
}

/* L4: a compaction cut after its stage was complete (renamed to name's
 * stage name) and before the stage took name. The stage is newer than
 * every frame on disk when its base (the checkpoint's sequence + 1) is
 * above both checkpoints' sequences: then the inactive checkpoint it
 * replaces goes and the stage takes its name. Otherwise (frames were
 * appended after a failed finish) it goes. Alias-aware removals (H1). */
static enum dbjournal_result settle_stage(struct dbjournal *j,
                                          const char *name)
{
    char stage[DBJOURNAL_ROOT_MAX], dest[DBJOURNAL_ROOT_MAX];
    struct scan n, s;
    uint64_t newest = 0;
    bool any = false;
    if (!path(stage, j, new_name(name)) || !path(dest, j, name))
        return DBJOURNAL_INVALID;
    enum dbjournal_result rc = scan_file(j, new_name(name), &n, NULL, NULL,
                                         NULL, 0);
    if (rc == DBJOURNAL_MISSING) return DBJOURNAL_OK;    /* the usual case */
    if (rc == DBJOURNAL_CANCELLED || rc == DBJOURNAL_IO) return rc;
    for (int i = 0; rc == DBJOURNAL_OK && i < 2; i++)
    {
        enum dbjournal_result r = scan_file(j, i ? NAME_PREVIOUS : NAME_PRIMARY,
                                            &s, NULL, NULL, NULL, 0);
        if (r == DBJOURNAL_CANCELLED || r == DBJOURNAL_IO) return r;
        if (r == DBJOURNAL_OK || s.header_good)
        {
            any = true;
            if (s.seq > newest) newest = s.seq;
        }
    }
    if (rc == DBJOURNAL_OK && any && n.base > newest)
    {
        if (!safefile_remove(dest, stage) || rename(stage, dest) < 0)
            return DBJOURNAL_IO;
        return DBJOURNAL_OK;
    }
    return safefile_remove(stage, dest) ? DBJOURNAL_OK : DBJOURNAL_IO;
}

enum dbjournal_result dbjournal_open(const char *root, struct dbjournal *j,
                                     void *scratch, size_t bytes)
{
    return dbjournal_open_cancel(root, j, scratch, bytes, NULL, NULL);
}

enum dbjournal_result dbjournal_open_cancel(const char *root,
    struct dbjournal *j, void *scratch, size_t bytes,
    bool (*cancel)(void *opaque), void *opaque)
{
    if (!j || !root || !*root || strlen(root) >= DBJOURNAL_ROOT_MAX
        || !scratch || bytes < DBJOURNAL_SCRATCH_MIN) return DBJOURNAL_INVALID;
    memset(j, 0, sizeof(*j));
    j->root = root;
    j->scratch = scratch;
    j->bytes = bytes;
    j->cancel = cancel;
    j->cancel_opaque = opaque;
    struct scan s[2];
    enum dbjournal_result rc[2];
    for (int i = 0; i < 2; i++)
    {
        rc[0] = settle_stage(j, i ? NAME_PREVIOUS : NAME_PRIMARY);
        if (rc[0] != DBJOURNAL_OK) return rc[0];
    }
    rc[0] = scan_file(j, NAME_PRIMARY, &s[0], NULL, NULL, NULL, 0);
    if (rc[0] == DBJOURNAL_CANCELLED) return rc[0];
    rc[1] = scan_file(j, NAME_PREVIOUS, &s[1], NULL, NULL, NULL, 0);
    if (rc[1] == DBJOURNAL_CANCELLED) return rc[1];
    if (rc[0] == DBJOURNAL_IO || rc[1] == DBJOURNAL_IO) return DBJOURNAL_IO;
    if (rc[0] == DBJOURNAL_MISSING && rc[1] == DBJOURNAL_MISSING)
    {
        j->ready = true;
        j->end = sizeof(struct journal_header);
        return DBJOURNAL_MISSING;
    }
    int pick = rc[0] == DBJOURNAL_OK ? 0 : rc[1] == DBJOURNAL_OK ? 1 : -1;
    if (pick < 0) return DBJOURNAL_CORRUPT;
    if (rc[1] == DBJOURNAL_OK && (rc[0] != DBJOURNAL_OK || s[1].seq > s[0].seq)) pick = 1;
    int other = pick ^ 1;
    /* Interior corruption in a newer/same checkpoint may conceal newer
     * events. Only an explicitly newer complete checkpoint supersedes it. */
    if (rc[other] == DBJOURNAL_CORRUPT && s[other].header_good
        && s[pick].base <= s[other].base) return DBJOURNAL_CORRUPT;
    j->previous = pick == 1;
    j->base = s[pick].base;
    j->seq = s[pick].seq;
    j->end = s[pick].end;
    j->ready = true;
    return DBJOURNAL_OK;
}

void dbjournal_close(struct dbjournal *j)
{ if (j) j->ready = false; }

static bool write_header(int fd, uint64_t base);

/* Append bytes [pos, end) of one journal file to out, in bulk; end 0
 * copies to the end of the file. */
static bool copy_range(struct dbjournal *j, int out, const char *name,
                       uint32_t pos, uint32_t end)
{
    char file[DBJOURNAL_ROOT_MAX];
    if (!path(file, j, name)) return false;
    int in = open(file, O_RDONLY);
    if (in < 0) return false;
    off_t size = lseek(in, 0, SEEK_END);
    if (!end && size > 0 && (uint64_t)size <= DBJOURNAL_MAX_BYTES)
        end = (uint32_t)size;
    bool ok = size >= (off_t)end && lseek(in, pos, SEEK_SET) == (off_t)pos;
    while (ok && pos < end)
    {
        if (j->cancel && j->cancel(j->cancel_opaque)) ok = false;
        size_t n = end - pos < j->bytes ? end - pos : j->bytes;
        ok = ok && read(in, j->scratch, n) == (ssize_t)n
             && write(out, j->scratch, n) == (ssize_t)n;
        pos += n;
    }
    if (close(in) < 0) ok = false;
    return ok;
}

/* Free bytes on the journal's volume (cached FAT count: no disk walk). */
#ifdef DBJOURNAL_HOST
static uint64_t test_free = UINT64_MAX;
void dbjournal_test_free_bytes(uint64_t bytes)
{ test_free = bytes; }
static uint64_t free_bytes(void)
{ return test_free; }
#else
static uint64_t free_bytes(void)
{
    sector_t free_kib = 0;
    volume_size(IF_MV(0,) NULL, &free_kib);
    return (uint64_t)free_kib * 1024;
}
#endif
#define BAD_COPY_MARGIN (16u * 1024 * 1024)

/* The diagnostic copy of 'name' already exists (an earlier, failed
 * salvage of the same corruption: a copy of the same size), or the disk
 * is too short for it: skip it (R-Low). Diagnostic only: another
 * corruption of exactly the same size keeps the earlier copy. */
static bool bad_copy_done(struct dbjournal *j, const char *name,
                          const char *bad)
{
    char file[DBJOURNAL_ROOT_MAX];
    bool done = false;
    if (!path(file, j, name)) return true;
    int in = open(file, O_RDONLY), old = open(bad, O_RDONLY);
    off_t size = in >= 0 ? lseek(in, 0, SEEK_END) : -1;
    if (size < 0 || free_bytes() < (uint64_t)size + BAD_COPY_MARGIN)
        done = true;
    else
        done = old >= 0 && lseek(old, 0, SEEK_END) == size;
    if (in >= 0) close(in);
    if (old >= 0) close(old);
    return done;
}

/* Every step leaves either the corrupt original in place (a later open
 * fails and salvage simply runs again) or a complete valid replacement. */
enum dbjournal_result dbjournal_salvage(const char *root, void *scratch,
    size_t bytes, bool (*cancel)(void *opaque), void *opaque)
{
    static const char *const names[2] = { NAME_PRIMARY, NAME_PREVIOUS };
    struct dbjournal j;
    struct scan s[2];
    enum dbjournal_result rc[2];
    char from[DBJOURNAL_ROOT_MAX], to[DBJOURNAL_ROOT_MAX];
    int order[2], n = 0, victim = -1;

    if (!root || !*root || strlen(root) >= DBJOURNAL_ROOT_MAX || !scratch
        || bytes < DBJOURNAL_SCRATCH_MIN) return DBJOURNAL_INVALID;
    memset(&j, 0, sizeof(j));
    j.root = root;
    j.scratch = scratch;
    j.bytes = bytes;
    j.cancel = cancel;
    j.cancel_opaque = opaque;
    for (int i = 0; i < 2; i++)
    {
        rc[i] = scan_file(&j, names[i], &s[i], NULL, NULL, NULL, 0);
        if (rc[i] != DBJOURNAL_OK && rc[i] != DBJOURNAL_MISSING
            && rc[i] != DBJOURNAL_CORRUPT) return rc[i];
        if (s[i].header_good) order[n++] = i;
        if (rc[i] == DBJOURNAL_CORRUPT) victim = i;
    }
    if (victim < 0) return DBJOURNAL_OK;        /* nothing to set aside */
    /* Oldest checkpoint first (equal base: the longer prefix is newer);
     * a newer one must begin after the older prefix to be merged. */
    if (n == 2 && (s[order[1]].base < s[order[0]].base
        || (s[order[1]].base == s[order[0]].base
            && s[order[1]].seq < s[order[0]].seq)))
    { int t = order[0]; order[0] = order[1]; order[1] = t; }
    if (n == 2 && (s[order[1]].base <= s[order[0]].seq
        || (uint64_t)s[order[0]].end + s[order[1]].end > DBJOURNAL_MAX_BYTES))
    { order[0] = order[1]; n = 1; }
    int newest = n ? order[n - 1] : -1;
    bool merge = newest >= 0 && rc[newest] == DBJOURNAL_CORRUPT;
    if (merge)
    {
        victim = newest;
        /* Its header is good, so it is the newest checkpoint: rows past
         * the bad frame are lost from it, and the older file's frames
         * would bring back values it superseded (the replay ignores
         * sequence). Keep only the newest valid prefix (R-M4); the caller
         * re-checkpoints from RAM after the replay. */
        order[0] = newest;
        n = 1;
    }
    /* Keep a copy of the (newest) corrupt file for diagnosis; the copy
     * replaces any older one. The original stays until replaced. Once per
     * corruption (a retry finds its copy), never into a short disk. */
    if (!path(to, &j, NAME_BAD)) return DBJOURNAL_INVALID;
    if (!bad_copy_done(&j, names[victim], to))
    {
        int fd = open(to, O_RDWR | O_CREAT | O_TRUNC, 0666);
        if (fd < 0) return DBJOURNAL_IO;
        copy_range(&j, fd, names[victim], 0, 0);
        close(fd);          /* diagnostic copy only: never blocks recovery */
    }
    int fd;
    if (merge)
    {
        if (!path(from, &j, NAME_TEMP)) return DBJOURNAL_INVALID;
        fd = open(from, O_RDWR | O_CREAT | O_TRUNC, 0666);
        if (fd < 0) return DBJOURNAL_IO;
        bool ok = write_header(fd, s[order[0]].base);
        for (int k = 0; ok && k < n; k++)
            ok = copy_range(&j, fd, names[order[k]],
                            sizeof(struct journal_header), s[order[k]].end);
        ok = ok && fsync(fd) == 0;
        if (close(fd) < 0) ok = false;
        if (!ok) return DBJOURNAL_IO;
        struct scan t;
        enum dbjournal_result v = scan_file(&j, NAME_TEMP, &t, NULL, NULL,
                                            NULL, 0);
        if (v == DBJOURNAL_CANCELLED) return v;
        if (v != DBJOURNAL_OK || t.seq != s[newest].seq) return DBJOURNAL_IO;
        char prev[DBJOURNAL_ROOT_MAX];
        if (!path(to, &j, names[newest])
            || !path(prev, &j, old_name(names[newest])))
            return DBJOURNAL_INVALID;
        /* never onto the corrupt file's name (#91): a cut puts it back */
        if (safefile_replace(from, to, prev) < 0) return DBJOURNAL_IO;
    }
    /* Remaining corrupt files: their prefixes are merged or unusable. */
    for (int i = 0; i < 2; i++)
    {
        if (rc[i] != DBJOURNAL_CORRUPT || (merge && i == newest)) continue;
        if (!path(from, &j, names[i])) return DBJOURNAL_INVALID;
        if (remove(from) < 0 && errno != ENOENT) return DBJOURNAL_IO;
    }
    return DBJOURNAL_OK;
}

static bool write_header(int fd, uint64_t base)
{
    struct journal_header h = { FILE_MAGIC, write_version, base, 0, 0 };
    h.sum = checksum(&h, sizeof(h));
    return write(fd, &h, sizeof(h)) == sizeof(h);
}

static enum dbjournal_result make_frame(void *output, const char *filename,
    uint32_t tag, int32_t value, uint64_t seq, size_t *bytes)
{
    size_t len = 0;
    while (len < DBJOURNAL_PATH_MAX && filename[len])
        len++;
    if (!len || len >= DBJOURNAL_PATH_MAX || !utf8(filename, len)) return DBJOURNAL_INVALID;
    struct frame_header f = { FRAME_MAGIC, write_version, 0, len + 1, tag,
                              value, seq, 0, 0 };
    f.bytes = (sizeof(f) + f.path_bytes + 3) & ~3u;
    memset(output, 0, f.bytes);
    memcpy((unsigned char *)output + sizeof(f), filename, len + 1);
    memcpy(output, &f, sizeof(f));
    f.sum = checksum(output, f.bytes);
    f.header_sum = checksum(&f, sizeof(f));
    memcpy(output, &f, sizeof(f));
    *bytes = f.bytes;
    return DBJOURNAL_OK;
}

/* First creation is transactional too: a failed initial header write must
 * not leave a corrupt published journal that prevents every future retry. */
static bool create_empty(struct dbjournal *j, const char *destination)
{
    char temporary[DBJOURNAL_ROOT_MAX];
    struct journal_header h;
    int fd;
    bool ok;

    if (!path(temporary, j, NAME_TEMP))
        return false;
    fd = open(temporary, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return false;
    ok = write_header(fd, 0) && fsync(fd) == 0
         && lseek(fd, 0, SEEK_SET) == 0
         && read(fd, &h, sizeof(h)) == (ssize_t)sizeof(h);
    if (close(fd) < 0)
        ok = false;
    if (!ok)
        return false;
    uint32_t sum = h.sum;
    h.sum = 0;
    if (h.magic != FILE_MAGIC || h.version != write_version || h.base
        || h.reserved || sum != checksum(&h, sizeof(h)))
        return false;
    if (j->staged)                      /* the active file is the temp */
        return true;
    char prev[DBJOURNAL_ROOT_MAX];
    return path(prev, j, old_name(active_name(j)))
           && safefile_replace(temporary, destination, prev) == 0;
}

enum dbjournal_result dbjournal_append(struct dbjournal *j,
    const char *filename, uint32_t tag, int32_t absolute)
{
    if (!j || !j->ready || !filename || j->seq == UINT64_MAX) return DBJOURNAL_INVALID;
    size_t bytes;
    enum dbjournal_result rc = make_frame(j->scratch, filename, tag, absolute, j->seq + 1, &bytes);
    if (rc != DBJOURNAL_OK) return rc;
    if (j->end + bytes > DBJOURNAL_MAX_BYTES) return DBJOURNAL_FULL;
    char file[DBJOURNAL_ROOT_MAX];
    if (!path(file, j, active_name(j))) return DBJOURNAL_INVALID;
    int fd = open(file, O_RDWR);
    if (fd < 0 && errno == ENOENT && j->seq == 0
        && j->end == sizeof(struct journal_header))
    {
        if (!create_empty(j, file))
        {
            j->ready = false;
            return DBJOURNAL_IO;
        }
        fd = open(file, O_RDWR);
    }
    if (fd < 0) return DBJOURNAL_IO;
    off_t size = lseek(fd, 0, SEEK_END);
    bool ok = size >= 0 && (uint64_t)size >= j->end;
    ok = ok && ftruncate(fd, j->end) == 0 && lseek(fd, j->end, SEEK_SET) == (off_t)j->end
         && write(fd, j->scratch, bytes) == (ssize_t)bytes && fsync(fd) == 0;
    /* Readback uses the spare caller-buffer tail: retain expected bytes. */
    unsigned char *readback = (unsigned char *)j->scratch + bytes;
    ok = ok && lseek(fd, j->end, SEEK_SET) == (off_t)j->end
         && read(fd, readback, bytes) == (ssize_t)bytes
         && !memcmp(j->scratch, readback, bytes);
    if (close(fd) < 0) ok = false;
    if (!ok) { j->ready = false; return DBJOURNAL_IO; }
    j->seq++;
    j->end += bytes;
    return DBJOURNAL_OK;
}

enum dbjournal_result dbjournal_replay(struct dbjournal *j,
    dbjournal_apply_fn apply, void *opaque)
{
    if (!j || !j->ready || !apply) return DBJOURNAL_INVALID;
    struct scan s;
    enum dbjournal_result rc = scan_file(j, active_name(j), &s, NULL, NULL,
                                        NULL, j->end);
    if (rc == DBJOURNAL_MISSING && !j->seq) return DBJOURNAL_OK;
    if (rc != DBJOURNAL_OK) return rc;
    if (s.seq != j->seq || s.end != j->end) return DBJOURNAL_INVALID;
    return scan_file(j, active_name(j), &s, apply, opaque, NULL, j->end);
}

enum dbjournal_result dbjournal_checkpoint(const struct dbjournal *j,
                                          struct dbjournal_cursor *cursor)
{
    if (!j || !j->ready || !cursor)
        return DBJOURNAL_INVALID;
    *cursor = (struct dbjournal_cursor){ j->base, j->seq, j->end, j->previous };
    return DBJOURNAL_OK;
}

enum dbjournal_result dbjournal_replay_since(struct dbjournal *j,
    struct dbjournal_cursor *cursor, dbjournal_apply_fn apply, void *opaque)
{
    struct scan s;
    if (!j || !j->ready || !cursor || !apply || cursor->base != j->base
        || cursor->previous != j->previous || cursor->seq > j->seq
        || cursor->end > j->end)
        return DBJOURNAL_INVALID;
    if (cursor->seq == j->seq && cursor->end == j->end)
        return DBJOURNAL_OK;
    enum dbjournal_result rc = scan_file(j, active_name(j), &s, apply, opaque,
                                        cursor, j->end);
    if (rc != DBJOURNAL_OK)
        return rc;
    if (s.seq != j->seq || s.end != j->end)
        return DBJOURNAL_INVALID;
    return dbjournal_checkpoint(j, cursor);
}

enum dbjournal_result dbjournal_compact(struct dbjournal *j,
    dbjournal_next_fn next, void *opaque)
{
    struct dbjournal staged;
    struct dbjournal_cursor cursor;
    enum dbjournal_result rc = dbjournal_checkpoint(j, &cursor);
    if (rc != DBJOURNAL_OK) return rc;
    rc = dbjournal_compact_stage(j, next, opaque, &staged);
    return rc == DBJOURNAL_OK
        ? dbjournal_compact_finish(j, &staged, &cursor) : rc;
}

enum dbjournal_result dbjournal_compact_stage(struct dbjournal *j,
    dbjournal_next_fn next, void *opaque, struct dbjournal *staged)
{
    if (!j || !j->ready || j->staged || !j->scratch
        || j->bytes < DBJOURNAL_SCRATCH_MIN || !next || !staged
        || j->seq == UINT64_MAX) return DBJOURNAL_INVALID;
    char temp[DBJOURNAL_ROOT_MAX];
    if (!path(temp, j, NAME_TEMP)) return DBJOURNAL_INVALID;
    int fd = open(temp, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return DBJOURNAL_IO;
    uint64_t seq = j->seq + 1;
    uint32_t end = sizeof(struct journal_header);
    enum dbjournal_result rc = write_header(fd, seq) ? DBJOURNAL_OK : DBJOURNAL_IO;
    /* Bulk output uses the first half of the existing caller buffer, with
     * enumeration's filename isolated in the second. Reserve room for a
     * maximum frame before encoding; no per-record writes or allocations. */
    size_t capacity = j->bytes / 2, used = 0;
    const size_t max_frame = (sizeof(struct frame_header)
                              + DBJOURNAL_PATH_MAX + 3) & ~(size_t)3;
    unsigned char *output = j->scratch;
    char *filename = (char *)j->scratch + capacity;
    int n = 0;
    while (rc == DBJOURNAL_OK)
    {
        if (j->cancel && j->cancel(j->cancel_opaque))
        { rc = DBJOURNAL_CANCELLED; break; }
        uint32_t tag;
        int32_t value;
        memset(filename, 0xff, DBJOURNAL_PATH_MAX);
        n = next(opaque, filename, DBJOURNAL_PATH_MAX, &tag, &value);
        if (n == 0) break;
        if (n < 0) { rc = DBJOURNAL_CANCELLED; break; }
        if (seq == UINT64_MAX) { rc = DBJOURNAL_FULL; break; }
        if (capacity - used < max_frame)
        {
            if (write(fd, output, used) != (ssize_t)used)
            { rc = DBJOURNAL_IO; break; }
            used = 0;
        }
        size_t bytes;
        rc = make_frame(output + used, filename, tag, value, ++seq, &bytes);
        if (rc != DBJOURNAL_OK) break;
        if (end + bytes > DBJOURNAL_MAX_BYTES) { rc = DBJOURNAL_FULL; break; }
        used += bytes;
        end += bytes;
    }
    if (rc == DBJOURNAL_OK && used
        && write(fd, output, used) != (ssize_t)used) rc = DBJOURNAL_IO;
    if (rc == DBJOURNAL_OK && fsync(fd) < 0) rc = DBJOURNAL_IO;
    if (close(fd) < 0) rc = DBJOURNAL_IO;
    if (rc != DBJOURNAL_OK) return rc;
    struct scan s;
    rc = scan_file(j, NAME_TEMP, &s, NULL, NULL, NULL, 0);
    if (rc != DBJOURNAL_OK) return rc;
    if (s.seq != seq || s.end != end) return DBJOURNAL_CORRUPT;
    *staged = *j;
    staged->staged = true;
    staged->base = s.base;
    staged->seq = seq;
    staged->end = end;
    return DBJOURNAL_OK;
}

enum dbjournal_result dbjournal_compact_finish(struct dbjournal *live,
    const struct dbjournal *staged, const struct dbjournal_cursor *cursor)
{
    if (!live || !staged || !cursor || !live->ready || !staged->ready
        || live->staged || !staged->staged || strcmp(live->root, staged->root)
        || cursor->base != live->base || cursor->seq != live->seq
        || cursor->end != live->end || cursor->previous != live->previous
        || staged->base <= live->base || staged->seq < live->seq)
        return DBJOURNAL_INVALID;
    char temp[DBJOURNAL_ROOT_MAX], dest[DBJOURNAL_ROOT_MAX],
         stage[DBJOURNAL_ROOT_MAX];
    const char *name = live->previous ? NAME_PRIMARY : NAME_PREVIOUS;
    if (!path(temp, live, NAME_TEMP) || !path(dest, live, name)
        || !path(stage, live, new_name(name)))
        return DBJOURNAL_INVALID;
    /* Caller serialized the bounded final tail, never the bulk stage.
     * Only replace inactive checkpoint, never a rename onto an existing
     * name (two entries for one name on the device, crash-fat 1019). The
     * complete stage takes its own name first, so a cut after the
     * inactive one went is rolled forward at open (L4); a cut before
     * leaves the active checkpoint, which open picks. A failed rename may
     * have landed: the stage's leftover name goes alias-aware. */
    if (!safefile_remove(stage, dest) || rename(temp, stage) < 0
        || !safefile_remove(dest, stage) || rename(stage, dest) < 0)
    {
        safefile_remove(stage, dest);
        live->ready = false;
        return DBJOURNAL_IO;
    }
    live->previous = !live->previous;
    live->base = staged->base;
    live->seq = staged->seq;
    live->end = staged->end;
    return DBJOURNAL_OK;
}
