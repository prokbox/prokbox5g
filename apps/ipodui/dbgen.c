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
 * iPod UI shell: recoverable database generations
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
#include <string.h>
#include <stdint.h>
#ifdef DBGEN_HOST
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#else
#include "config.h"
#include "file.h"
#include "dir.h"
#endif
#include "dbgen.h"
#ifndef DBGEN_HOST
#include "safefile.h"
#include "crc32.h"
#endif

#ifdef DBGEN_HOST
static uint64_t fault_mask[DBGEN_TEST_NOPS];
static int fault_error[DBGEN_TEST_NOPS];
void dbgen_test_fail(enum dbgen_test_op op, uint64_t mask, int error)
{
    fault_mask[op] = mask;
    fault_error[op] = error;
}
void dbgen_test_reset(void) { memset(fault_mask, 0, sizeof(fault_mask)); }
static bool fails(enum dbgen_test_op op)
{
    bool fail = (fault_mask[op] & 1) != 0;
    fault_mask[op] >>= 1;
    if (fail) errno = fault_error[op];
    return fail;
}
static int test_open(const char *p, int flags, ...)
{ return fails(DBGEN_TEST_OPEN) ? -1 : open(p, flags, 0666); }
static ssize_t test_read(int fd, void *p, size_t n)
{
    if (fails(DBGEN_TEST_READ)) return -1;
    if (fails(DBGEN_TEST_SHORT_READ) && n) n--;
    return read(fd, p, n);
}
static ssize_t test_write(int fd, const void *p, size_t n)
{
    if (fails(DBGEN_TEST_WRITE)) return -1;
    if (fails(DBGEN_TEST_SHORT_WRITE) && n) n--;
    return write(fd, p, n);
}
static int test_flush(int fd)
{ return fails(DBGEN_TEST_FLUSH) ? -1 : fsync(fd); }
static int test_rename(const char *a, const char *b)
{
    if (fails(DBGEN_TEST_RENAME)) return -1;
    int rc = rename(a, b);
    return fails(DBGEN_TEST_RENAME_POST) ? -1 : rc;
}
static int test_close(int fd)
{
    bool fail = fails(DBGEN_TEST_CLOSE);
    int rc = close(fd);
    return fail ? -1 : rc;
}
static int test_remove(const char *path)
{ return fails(DBGEN_TEST_REMOVE) ? -1 : remove(path); }
#define open test_open
#define read test_read
#define write test_write
#define fsync test_flush
#define rename test_rename
#define close test_close
#define remove test_remove
#endif

#define DBGEN_MAGIC 0x47445049u /* IPDG; native endian, this device only */
#define DBGEN_VERSION 1
#define DBGEN_RECOVERY 3        /* database_gen3: torn-manifest fresh builds */
typedef char dbgen_generations_check[DBGEN_RECOVERY + 1 == DBGEN_GENERATIONS ? 1 : -1];

struct file_stamp { uint32_t bytes, sum; };
struct manifest {
    uint32_t magic, version;
    uint64_t seq;
    uint32_t gen, count, schema;
    int32_t commitid, entries;
    uint32_t structural;
    struct file_stamp file[DBGEN_MAX_FILES];
    uint32_t sum, reserved; /* Explicit padding: same layout on host/ARM. */
};
typedef char manifest_size_check[sizeof(struct manifest) == 304 ? 1 : -1];

struct workspace {
    struct manifest manifest[2];
    enum dbgen_result status[2];
};

/* A 64-byte nibble table avoids a large permanent table and bit-per-byte
 * CRC overhead on ARM7. CRC-32/ISO-HDLC, start/end complement. */
static uint32_t crc_add(uint32_t crc, const void *data, size_t bytes)
{
#ifdef DBGEN_HOST
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

static uint32_t crc(const void *p, size_t bytes)
{ return ~crc_add(~0u, p, bytes); }

static bool join(char *out, const char *dir, const char *name)
{
    int n = snprintf(out, DBGEN_PATH_MAX, "%s/%s", dir, name);
    return n >= 0 && n < DBGEN_PATH_MAX;
}

static bool gen_path(char *out, const char *root, unsigned gen)
{
    char name[20];
    snprintf(name, sizeof(name), "database_gen%u", gen);
    return join(out, root, name);
}

static bool manifest_path(char *out, const char *root, unsigned slot, bool temp)
{
    char name[28];
    snprintf(name, sizeof(name), "database_manifest%u%s", slot,
             temp ? ".tmp" : ".dat");
    return join(out, root, name);
}

static bool cancelled(const struct dbgen_ops *ops)
{ return ops->cancel && ops->cancel(ops->opaque); }

static bool args_ok(const char *root, void *scratch, size_t bytes,
                    const struct dbgen_ops *ops)
{
    if (!root || !*root || !scratch || bytes < DBGEN_SCRATCH_MIN || !ops
        || !ops->files || !ops->count || ops->count > DBGEN_MAX_FILES
        || !ops->validate)
        return false;
#ifdef DBGEN_HOST
    /* The file table is a constant of the caller's (tagcache.c's
     * ui_generation_ops()): its names are checked by the host tests, not
     * at every call on the device. */
    for (unsigned i = 0; i < ops->count; i++)
    {
        const char *name = ops->files[i].name;
        if (!name || !*name || strchr(name, '/') || !strcmp(name, ".")
            || !strcmp(name, ".."))
            return false;
        for (unsigned j = 0; j < i; j++)
            if (!strcmp(name, ops->files[j].name)) return false;
    }
#endif
    return true;
}

/* Keep workspace and bulk I/O in caller memory, with natural alignment. */
static struct workspace *work_setup(void **scratch, size_t *bytes)
{
    uintptr_t p = (uintptr_t)*scratch;
    size_t pad = (8 - (p & 7)) & 7;
    struct workspace *w = (struct workspace *)(p + pad);
    *scratch = (unsigned char *)w + sizeof(*w);
    *bytes -= sizeof(*w) + pad;
    memset(w, 0, sizeof(*w));
    return w;
}

static uint32_t schema(const struct dbgen_ops *ops)
{
    uint32_t sum = ~0u;
    for (unsigned i = 0; i < ops->count; i++)
    {
        const struct dbgen_file *f = &ops->files[i];
        unsigned char flags = f->immutable ? 1 : 0;
        sum = crc_add(sum, f->name, strlen(f->name) + 1);
        sum = crc_add(sum, &flags, 1);
    }
    return ~sum;
}

static enum dbgen_result read_manifest(const char *root, unsigned slot,
    struct manifest *m, const struct dbgen_ops *ops)
{
    char path[DBGEN_PATH_MAX];
    if (!manifest_path(path, root, slot, false)) return DBGEN_INVALID;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return errno == ENOENT ? DBGEN_MISSING : DBGEN_IO;
    ssize_t n = read(fd, m, sizeof(*m));
    off_t end = lseek(fd, 0, SEEK_END);
    int rc = close(fd);
    if (n < 0 || end < 0 || rc < 0) return DBGEN_IO;
    if (end != sizeof(*m)) return DBGEN_CORRUPT;
    /* A full-length file whose read was short is an I/O failure, NOT
     * permission to recycle the generation referenced by unread bytes. */
    if (n != sizeof(*m)) return DBGEN_IO;
    uint32_t saved = m->sum;
    m->sum = 0;
    uint32_t sum = crc(m, sizeof(*m));
    m->sum = saved;
    if (sum != saved || m->magic != DBGEN_MAGIC)
        return DBGEN_CORRUPT;
    /* Intact but written by another version/schema: never selected, yet
     * its gen field is trustworthy enough to keep that directory safe. */
    if (m->version != DBGEN_VERSION || !m->seq || m->gen > DBGEN_RECOVERY
        || m->count != ops->count || m->schema != schema(ops)
        || m->entries < 0 || m->commitid < 0 || m->reserved)
        return DBGEN_FOREIGN;
    return DBGEN_OK;
}

/* An intact (ours or foreign) manifest names this directory. */
static bool reserved(const struct workspace *w, unsigned gen)
{
    for (unsigned i = 0; i < 2; i++)
        if ((w->status[i] == DBGEN_OK || w->status[i] == DBGEN_FOREIGN)
            && w->manifest[i].gen == gen) return true;
    return false;
}

static void read_manifests(struct workspace *w, const char *root,
                           const struct dbgen_ops *ops)
{
    for (unsigned i = 0; i < 2; i++)
        w->status[i] = read_manifest(root, i, &w->manifest[i], ops);
}

/* Hash complete immutable files with strict size/read checks. A short read
 * of a declared range is not accepted as EOF or retried as valid content. */
static enum dbgen_result stamp_file(const char *path, bool immutable,
    struct file_stamp *stamp, void *scratch, size_t bytes,
    const struct dbgen_ops *ops, bool flush)
{
    int fd = open(path, flush ? O_RDWR : O_RDONLY);
    if (fd < 0) return errno == ENOENT ? DBGEN_MISSING : DBGEN_IO;
    enum dbgen_result rc = DBGEN_OK;
    off_t end = lseek(fd, 0, SEEK_END);
    if (end < 0 || (uint64_t)end > UINT32_MAX || lseek(fd, 0, SEEK_SET) < 0)
        rc = DBGEN_IO;
    uint32_t sum = ~0u;
    off_t pos = 0;
    while (rc == DBGEN_OK && immutable && pos < end)
    {
        if (cancelled(ops)) { rc = DBGEN_CANCELLED; break; }
        size_t n = (uint64_t)(end - pos) < bytes ? (size_t)(end - pos) : bytes;
        if (read(fd, scratch, n) != (ssize_t)n) { rc = DBGEN_IO; break; }
        sum = crc_add(sum, scratch, n);
        pos += n;
    }
    if (rc == DBGEN_OK && flush && fsync(fd) < 0) rc = DBGEN_IO;
    if (close(fd) < 0) rc = DBGEN_IO;
    if (rc == DBGEN_OK)
    {
        stamp->bytes = (uint32_t)end;
        stamp->sum = immutable ? ~sum : 0;
    }
    return rc;
}

static enum dbgen_result validate_generation(const char *path,
    struct manifest *m, void *scratch, size_t bytes,
    const struct dbgen_ops *ops, bool publish)
{
    if (cancelled(ops)) return DBGEN_CANCELLED;
    uint32_t structural = 0;
    int32_t found;
    enum dbgen_result rc = ops->validate(path, m->commitid, m->entries,
                                        &structural, &found, ops->opaque);
    if (rc != DBGEN_OK) return rc;
    if (!publish && structural != m->structural) return DBGEN_CORRUPT;
    for (unsigned i = 0; i < ops->count; i++)
    {
        char file[DBGEN_PATH_MAX];
        struct file_stamp stamp;
        if (!join(file, path, ops->files[i].name)) return DBGEN_INVALID;
        rc = stamp_file(file, ops->files[i].immutable, &stamp,
                        scratch, bytes, ops, publish);
        if (rc != DBGEN_OK) return rc;
        if (!publish && (stamp.bytes != m->file[i].bytes
                         || stamp.sum != m->file[i].sum)) return DBGEN_CORRUPT;
        if (publish) m->file[i] = stamp;
    }
    if (publish) m->structural = structural;
    return DBGEN_OK;
}

enum dbgen_result dbgen_select(const char *root, char out[DBGEN_PATH_MAX],
    void *scratch, size_t bytes, const struct dbgen_ops *ops)
{
    if (out) out[0] = 0;
    if (!out || !args_ok(root, scratch, bytes, ops)) return DBGEN_INVALID;
    struct workspace *w = work_setup(&scratch, &bytes);
    read_manifests(w, root, ops);
    if (w->status[0] == DBGEN_MISSING && w->status[1] == DBGEN_MISSING)
    {
        if (strlen(root) >= DBGEN_PATH_MAX) return DBGEN_INVALID;
        strcpy(out, root);
        return DBGEN_LEGACY;
    }
    unsigned first = w->manifest[1].seq > w->manifest[0].seq ? 1 : 0;
    enum dbgen_result result = DBGEN_CORRUPT;
    for (unsigned j = 0; j < 2; j++)
    {
        unsigned i = first ^ j;
        enum dbgen_result rc = w->status[i];
        if (rc == DBGEN_OK)
        {
            if (!gen_path(out, root, w->manifest[i].gen)) return DBGEN_INVALID;
            rc = validate_generation(out, &w->manifest[i], scratch, bytes, ops, false);
            if (rc == DBGEN_OK) return DBGEN_OK;
            out[0] = 0;
        }
        if (rc == DBGEN_CANCELLED) return rc;
        if (rc == DBGEN_IO) result = rc;
    }
    if (result == DBGEN_IO || w->status[0] == DBGEN_OK
        || w->status[1] == DBGEN_OK)
        return result;
    /* Neither manifest is usable and none is ambiguous: no intact verdict
     * exists to contradict a structurally complete directory. Use the
     * newest one rather than an empty library; publish nothing here. */
    int32_t best = -1;
    for (unsigned gen = 0; gen <= DBGEN_RECOVERY + 1; gen++)
    {
        char path[DBGEN_PATH_MAX];
        uint32_t structural;
        int32_t found = -1;
        if (gen > DBGEN_RECOVERY)
        {
            if (strlen(root) >= DBGEN_PATH_MAX) return DBGEN_INVALID;
            strcpy(path, root);
        }
        else if (!gen_path(path, root, gen)) return DBGEN_INVALID;
        enum dbgen_result rc = ops->validate(path, -1, -1, &structural,
                                             &found, ops->opaque);
        if (rc == DBGEN_CANCELLED) { out[0] = 0; return rc; }
        if (rc == DBGEN_IO) result = rc;
        if (rc == DBGEN_OK && found > best)
        {
            best = found;
            strcpy(out, path);
        }
    }
    /* An unreadable directory may hold the newest library: never salvage
     * past it (and so let a prepare recycle it); defer instead (R-M1). */
    if (result == DBGEN_IO) { out[0] = 0; return DBGEN_IO; }
    return best >= 0 ? DBGEN_SALVAGED : result;
}

static enum dbgen_result copy_file(const char *source, const char *dest,
    void *scratch, size_t bytes, const struct dbgen_ops *ops)
{
    int in = open(source, O_RDONLY);
    if (in < 0) return errno == ENOENT ? DBGEN_MISSING : DBGEN_IO;
    int out = open(dest, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (out < 0) { close(in); return DBGEN_IO; }
    off_t end = lseek(in, 0, SEEK_END);
    enum dbgen_result rc = DBGEN_OK;
    if (end < 0 || lseek(in, 0, SEEK_SET) < 0) rc = DBGEN_IO;
    uint32_t source_sum = ~0u;
    for (off_t pos = 0; rc == DBGEN_OK && pos < end;)
    {
        if (cancelled(ops)) { rc = DBGEN_CANCELLED; break; }
        size_t n = (uint64_t)(end - pos) < bytes ? (size_t)(end - pos) : bytes;
        if (read(in, scratch, n) != (ssize_t)n
            || write(out, scratch, n) != (ssize_t)n) { rc = DBGEN_IO; break; }
        source_sum = crc_add(source_sum, scratch, n);
        pos += n;
    }
    if (rc == DBGEN_OK && fsync(out) < 0) rc = DBGEN_IO;
    if (rc == DBGEN_OK && (lseek(out, 0, SEEK_END) != end
                          || lseek(out, 0, SEEK_SET) != 0)) rc = DBGEN_IO;
    uint32_t dest_sum = ~0u;
    for (off_t pos = 0; rc == DBGEN_OK && pos < end;)
    {
        if (cancelled(ops)) { rc = DBGEN_CANCELLED; break; }
        size_t n = (uint64_t)(end - pos) < bytes ? (size_t)(end - pos) : bytes;
        if (read(out, scratch, n) != (ssize_t)n) { rc = DBGEN_IO; break; }
        dest_sum = crc_add(dest_sum, scratch, n);
        pos += n;
    }
    if (rc == DBGEN_OK && source_sum != dest_sum) rc = DBGEN_CORRUPT;
    if (close(out) < 0) rc = DBGEN_IO;
    if (close(in) < 0) rc = DBGEN_IO;
    return rc;
}

/* All declared files absent: nothing in this directory to protect. */
static enum dbgen_result declared_absent(const char *dir,
                                         const struct dbgen_ops *ops)
{
    for (unsigned i = 0; i < ops->count; i++)
    {
        char file[DBGEN_PATH_MAX];
        if (!join(file, dir, ops->files[i].name)) return DBGEN_INVALID;
        int fd = open(file, O_RDONLY);
        if (fd >= 0) { close(fd); return DBGEN_BUSY; }
        if (errno != ENOENT) return DBGEN_IO;
    }
    return DBGEN_OK;
}

/* torn: a torn manifest may name any directory. Only a directory with no
 * declared files may be used, database_gen3 included: once published it
 * is an ordinary generation, so no directory is free by name (R-M2). */
static enum dbgen_result prepare_directory(const char *root, const char *active,
    char out[DBGEN_PATH_MAX], const struct workspace *w, bool torn,
    const struct dbgen_ops *ops)
{
    /* Unreadable manifest may refer to any directory: do not reuse one. */
    if (w->status[0] == DBGEN_IO || w->status[1] == DBGEN_IO) return DBGEN_IO;
    /* gen3 is a last resort, e.g. both other directories still reserved
     * by foreign manifests while a salvaged one is active. Torn with no
     * empty directory: recycle the oldest (the lowest commit ID, a damaged
     * one first), never the active one, rather than never indexing again
     * (M1). */
    unsigned gen, pick = DBGEN_RECOVERY + 1, topgen = pick;
    int32_t best = INT32_MAX, top = -1;
    for (gen = 0; gen <= DBGEN_RECOVERY; gen++)
    {
        if (!gen_path(out, root, gen)) return DBGEN_INVALID;
        if (!strcmp(out, active) || reserved(w, gen)) continue;
        if (!torn) break;
        enum dbgen_result rc = declared_absent(out, ops);
        if (rc == DBGEN_BUSY)
        {
            uint32_t sum;
            int32_t found = -1;
            rc = ops->validate(out, -1, -1, &sum, &found, ops->opaque);
            if (rc != DBGEN_IO && rc != DBGEN_CANCELLED)
            {
                if (rc != DBGEN_OK) found = -1;
                if (found < best) { best = found; pick = gen; }
                if (found > top) { top = found; topgen = gen; }
                continue;
            }
        }
        if (rc == DBGEN_OK) break;
        out[0] = 0;
        return rc;
    }
    if (gen > DBGEN_RECOVERY)
    {
        /* never the newest valid library (with nothing active it is the
         * only fallback) */
        gen = pick == topgen ? DBGEN_RECOVERY + 1 : pick;
        if (gen > DBGEN_RECOVERY) { out[0] = 0; return DBGEN_BUSY; }
        gen_path(out, root, gen);
    }
#ifdef DBGEN_HOST
    int rc = mkdir(out, 0777);
#else
    int rc = mkdir(out);
#endif
    if (rc < 0 && errno != EEXIST) { out[0] = 0; return DBGEN_IO; }
    return DBGEN_OK;
}

int32_t dbgen_newest_commit(const char *root, void *scratch, size_t bytes,
                            const struct dbgen_ops *ops)
{
    int32_t best = -1;
    if (!args_ok(root, scratch, bytes, ops)) return best;
    struct workspace *w = work_setup(&scratch, &bytes);
    read_manifests(w, root, ops);
    for (unsigned i = 0; i < 2; i++)
        if (w->status[i] == DBGEN_OK && w->manifest[i].commitid > best)
            best = w->manifest[i].commitid;
    return best;
}

enum dbgen_result dbgen_prepare_empty(const char *root, const char *active,
    char out[DBGEN_PATH_MAX], void *scratch, size_t bytes,
    const struct dbgen_ops *ops)
{
    if (out) out[0] = 0;
    if (!out || !active || !args_ok(root, scratch, bytes, ops)) return DBGEN_INVALID;
    struct workspace *w = work_setup(&scratch, &bytes);
    read_manifests(w, root, ops);
    if (w->status[0] == DBGEN_IO || w->status[1] == DBGEN_IO) return DBGEN_IO;
    if (cancelled(ops)) return DBGEN_CANCELLED;
    /* A torn manifest may still name a retained directory. Its unknown
     * ownership is NOT permission to recycle a directory with content. */
    enum dbgen_result result = prepare_directory(root, active, out, w,
        w->status[0] == DBGEN_CORRUPT || w->status[1] == DBGEN_CORRUPT, ops);
    if (result != DBGEN_OK) return result;
    for (unsigned i = 0; i < ops->count; i++)
    {
        char dest[DBGEN_PATH_MAX];
        if (cancelled(ops)) { out[0] = 0; return DBGEN_CANCELLED; }
        if (!join(dest, out, ops->files[i].name))
        { out[0] = 0; return DBGEN_INVALID; }
        if (remove(dest) < 0 && errno != ENOENT)
        { out[0] = 0; return DBGEN_IO; }
    }
    return DBGEN_OK;
}

enum dbgen_result dbgen_prepare(const char *root, const char *active,
    char out[DBGEN_PATH_MAX], void *scratch, size_t bytes,
    const struct dbgen_ops *ops)
{
    if (out) out[0] = 0;
    if (!out || !active || !args_ok(root, scratch, bytes, ops)) return DBGEN_INVALID;
    struct workspace *w = work_setup(&scratch, &bytes);
    read_manifests(w, root, ops);
    /* A torn manifest and no intact one (a salvaged library running): the
     * torn one may reference any directory, so only an empty one is
     * scratch. An intact manifest's generation is the protected one; a
     * foreign (intact, other version) one reserves its own directory. */
    bool torn = w->status[0] != DBGEN_OK && w->status[1] != DBGEN_OK
        && (w->status[0] == DBGEN_CORRUPT || w->status[1] == DBGEN_CORRUPT);
    enum dbgen_result prepared = prepare_directory(root, active, out, w,
                                                   torn, ops);
    if (prepared != DBGEN_OK) return prepared;
    unsigned copied = 0, missing = 0;
    /* Check source completeness before changing even unreferenced scratch.
     * Never mistake a missing master plus orphaned strings for a fresh DB. */
    for (unsigned i = 0; i < ops->count; i++)
    {
        if (!ops->files[i].copy) continue;
        char source[DBGEN_PATH_MAX];
        if (!join(source, active, ops->files[i].name))
        { out[0] = 0; return DBGEN_INVALID; }
        copied++;
        int fd = open(source, O_RDONLY);
        if (fd < 0)
        {
            if (errno == ENOENT) missing++;
            else { out[0] = 0; return DBGEN_IO; }
        }
        else if (close(fd) < 0) { out[0] = 0; return DBGEN_IO; }
        if (cancelled(ops)) { out[0] = 0; return DBGEN_CANCELLED; }
    }
    if (missing && (missing != copied || strcmp(active, root)
        || w->status[0] != DBGEN_MISSING || w->status[1] != DBGEN_MISSING))
    { out[0] = 0; return DBGEN_MISSING; }
    for (unsigned i = 0; i < ops->count; i++)
    {
        char source[DBGEN_PATH_MAX], dest[DBGEN_PATH_MAX];
        if (cancelled(ops)) { out[0] = 0; return DBGEN_CANCELLED; }
        if (!join(dest, out, ops->files[i].name)
            || !join(source, active, ops->files[i].name))
        { out[0] = 0; return DBGEN_INVALID; }
        if (!ops->files[i].copy || missing == copied)
        {
            if (remove(dest) < 0 && errno != ENOENT)
            { out[0] = 0; return DBGEN_IO; }
            continue;
        }
        enum dbgen_result result = copy_file(source, dest, scratch, bytes, ops);
        if (result != DBGEN_OK)
        {
            /* Fresh databases are a caller decision; never turn missing one
             * structural component into a partially cloned generation. */
            out[0] = 0;
            return result;
        }
    }
    return DBGEN_OK;
}

enum dbgen_result dbgen_stage_publish(const char *root, const char *candidate,
    int32_t commitid, int32_t entries, void *scratch, size_t bytes,
    const struct dbgen_ops *ops, struct dbgen_publication *publication)
{
    if (!publication)
        return DBGEN_INVALID;
    publication->ready = false;
    if (!candidate || commitid < 0 || entries < 0
        || !args_ok(root, scratch, bytes, ops)) return DBGEN_INVALID;
    struct workspace *w = work_setup(&scratch, &bytes);
    read_manifests(w, root, ops);
    if (w->status[0] == DBGEN_IO || w->status[1] == DBGEN_IO) return DBGEN_IO;
    struct manifest m;
    memset(&m, 0, sizeof(m));
    char path[DBGEN_PATH_MAX];
    for (m.gen = 0; m.gen <= DBGEN_RECOVERY; m.gen++)
    {
        if (!gen_path(path, root, m.gen)) return DBGEN_INVALID;
        if (!strcmp(path, candidate)) break;
    }
    if (m.gen > DBGEN_RECOVERY) return DBGEN_INVALID;
    uint64_t seq = 0;
    for (unsigned i = 0; i < 2; i++)
    {
        if (w->status[i] != DBGEN_OK) continue;
        if (w->manifest[i].gen == m.gen) return DBGEN_BUSY;
        if (w->manifest[i].seq > seq) seq = w->manifest[i].seq;
    }
    if (seq == UINT64_MAX) return DBGEN_INVALID;
    /* A syntactically intact manifest can point to a corrupt generation.
     * Do not overwrite the only VALID fallback just because it is older.
     * I/O ambiguity is not corruption and blocks publication altogether. */
    for (unsigned i = 0; i < 2; i++)
    {
        if (w->status[i] != DBGEN_OK) continue;
        if (!gen_path(path, root, w->manifest[i].gen)) return DBGEN_INVALID;
        enum dbgen_result rc = validate_generation(path, &w->manifest[i],
                                                   scratch, bytes, ops, false);
        if (rc == DBGEN_IO || rc == DBGEN_CANCELLED) return rc;
        if (rc != DBGEN_OK) w->status[i] = DBGEN_CORRUPT;
    }
    m.magic = DBGEN_MAGIC;
    m.version = DBGEN_VERSION;
    m.seq = seq + 1;
    m.count = ops->count;
    m.schema = schema(ops);
    m.commitid = commitid;
    m.entries = entries;
    enum dbgen_result result = validate_generation(candidate, &m, scratch, bytes, ops, true);
    if (result != DBGEN_OK) return result;
    m.sum = crc(&m, sizeof(m));
    /* Replace a torn/missing/invalid slot first, a foreign one next, and
     * only then the older of two valid generations. Of two foreign ones,
     * the newer: salvage carried its (newest) directory forward, so the
     * older reference keeps protecting the other directory longer. */
    unsigned rank[2];
    for (unsigned i = 0; i < 2; i++)
        rank[i] = w->status[i] == DBGEN_OK ? 2
                : w->status[i] == DBGEN_FOREIGN ? 1 : 0;
    bool newer1 = w->manifest[1].seq > w->manifest[0].seq;
    unsigned slot = rank[0] != rank[1] ? rank[1] < rank[0]
                  : rank[0] == 2 ? !newer1 : rank[0] == 1 && newer1;
    char temp[DBGEN_PATH_MAX];
    if (!manifest_path(temp, root, slot, true)
        || !manifest_path(path, root, slot, false)) return DBGEN_INVALID;
    if (cancelled(ops)) return DBGEN_CANCELLED;
    int fd = open(temp, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return DBGEN_IO;
    bool ok = write(fd, &m, sizeof(m)) == sizeof(m) && fsync(fd) == 0
              && lseek(fd, 0, SEEK_SET) == 0
              && read(fd, scratch, sizeof(m)) == sizeof(m)
              && !memcmp(&m, scratch, sizeof(m));
    if (close(fd) < 0) ok = false;
    if (!ok) return DBGEN_IO;
    if (cancelled(ops)) return DBGEN_CANCELLED;
    strcpy(publication->temporary, temp);
    strcpy(publication->destination, path);
    publication->ready = true;
    return DBGEN_OK;
}

enum dbgen_result dbgen_finish_publish(struct dbgen_publication *publication)
{
    if (!publication || !publication->ready)
        return DBGEN_INVALID;
    publication->ready = false;
    /* The destination is the pair's stale slot (stage chose it: torn,
     * foreign, or the older of two valid). Remove it first, never a
     * rename onto its name (#91, M2: a failed victim removal inside a
     * rename-over leaves two entries). The OTHER manifest and its
     * complete generation stay untouched and are the recovery: a cut
     * between the two leaves only them, and select picks them. */
    if (remove(publication->destination) < 0 && errno != ENOENT)
        return DBGEN_IO;
    return rename(publication->temporary, publication->destination) == 0
           ? DBGEN_OK : DBGEN_IO;
}

void dbgen_recover(const char *root)
{
    char path[DBGEN_PATH_MAX], dest[DBGEN_PATH_MAX];
    for (unsigned slot = 0; slot < 2; slot++)
        if (root && manifest_path(path, root, slot, true)
            && manifest_path(dest, root, slot, false))
        {
            /* a staged manifest never outlives a boot. A cut inside the
             * publish rename can leave it on the manifest's own chain
             * (#91 H1): then only its name goes */
#ifdef DBGEN_HOST
            remove(path);
#else
            safefile_remove(path, dest);
#endif
        }
}

enum dbgen_result dbgen_publish(const char *root, const char *candidate,
    int32_t commitid, int32_t entries, void *scratch, size_t bytes,
    const struct dbgen_ops *ops)
{
    struct dbgen_publication publication;
    enum dbgen_result rc = dbgen_stage_publish(root, candidate, commitid,
                            entries, scratch, bytes, ops, &publication);
    return rc == DBGEN_OK ? dbgen_finish_publish(&publication) : rc;
}
