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
 * iPod UI shell: the library's version stamp
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
#ifndef IPODUI_LIBSTAMP_H
#define IPODUI_LIBSTAMP_H

/* The library's version stamp (p090-upgpage; owner, 2026-11-13: "make
 * sure libraries are versioned so future libraries can be quickly checked
 * for versioning compatibility").
 *
 * Every published generation's manifest (dbgen.c, struct manifest, 32
 * bytes of it) carries the stamp of the firmware that built it. The boot
 * reads the manifests anyway, so the check costs no extra read. Before
 * any of the generation's content is read, the newest intact manifest's
 * stamp is compared with this firmware's (libstamp_check()); on ANY
 * difference, older or newer (a downgrade too), or no stamp at all (a
 * library from before 0.9.0), the library is never used: it is owed a
 * full rebuild behind the library page, its statistics exported first
 * when this firmware can still read its master index (tagcache.c,
 * ui_generation_select_io(), log "db upgrade owed from=.. to=.. why=..").
 *
 * WHEN TO BUMP (future authors, read this):
 *  - LIBSTAMP_SCHEMA: whenever anything the scan or the commit DERIVES
 *    and stores changes meaning or content, so an existing library would
 *    be wrong without every file read again: the sort keys or the sort
 *    values stored in the tag entries (#235, CJK keys), the unplayable
 *    reason bits (#286), the multi-value artist join (#167), the genre
 *    table (#163), the class flags (bookclass.h), the library file's
 *    sections, a new stored tag. One bump per release line is enough:
 *    until a release ships, later changes in the same line share it.
 *  - SORTKEY_VERSION (sortkey.h, its own rules and test): when
 *    ui_sortkey()'s order changes. It moves LIBSTAMP_DB_MAGIC with it.
 *  - LIBSTAMP_DB_MAGIC: when the on-disk format of the master index or the
 *    tag files changes (it is TAGCACHE_MAGIC, tagcache.c).
 *  - NOT for UI_GROUPING_VERSION (sortkey.h): the grouping rules only
 *    re-sort the tag files once, with no rebuild (ui_resort_owed()).
 *  - NOT for the firmware version: it is in the stamp for the logs only.
 * tests/libstamp/run.sh pins the check and the current numbers: update its
 * expected values in the same change as a bump.
 *
 * Pure and header-only: dbgen.c, the firmware and the host tests share it.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "sortkey.h"            /* SORTKEY_VERSION */

/* The database format (TAGCACHE_MAGIC, tagcache.c): "TCH" and the key
 * order's version */
#define LIBSTAMP_DB_MAGIC   (0x544348a0 + SORTKEY_VERSION)
#define LIBSTAMP_DB_FAMILY  0x54434800u     /* the top 24 bits of any */
/* The ProkBox library schema: see WHEN TO BUMP above. 1: 0.9.0 (the
 * unplayable reason, ID3 genres 148-191, joined artists, the sort values
 * in the tag entries; docs/changes-from-rockbox.md 3.1). */
#define LIBSTAMP_SCHEMA     1

#define LIBSTAMP_FW_LEN 24
struct libstamp {
    uint32_t db_magic;          /* LIBSTAMP_DB_MAGIC; 0: no stamp */
    uint16_t sortkey;           /* SORTKEY_VERSION */
    uint16_t schema;            /* LIBSTAMP_SCHEMA */
    char fw[LIBSTAMP_FW_LEN];   /* the building firmware, NUL-terminated,
                                 * truncated; logs only, never compared */
};
typedef char libstamp_size_check[sizeof(struct libstamp) == 32 ? 1 : -1];

enum libstamp_why {
    LIBSTAMP_OK = 0,            /* this firmware's library */
    LIBSTAMP_MISSING,           /* none: a library from before 0.9.0 */
    LIBSTAMP_CORRUPT,           /* not a stamp any firmware writes */
    LIBSTAMP_DB_FORMAT,         /* another database format */
    LIBSTAMP_SORTKEY,           /* another key order */
    LIBSTAMP_SCHEMA_DIFF,       /* another library schema */
};

/* This firmware's stamp; fw: its version string (prokversion), or NULL */
static inline void libstamp_ours(struct libstamp *s, const char *fw)
{
    memset(s, 0, sizeof(*s));
    s->db_magic = LIBSTAMP_DB_MAGIC;
    s->sortkey = SORTKEY_VERSION;
    s->schema = LIBSTAMP_SCHEMA;
    if (fw)
    {
        size_t n = strlen(fw);
        if (n > LIBSTAMP_FW_LEN - 1)
            n = LIBSTAMP_FW_LEN - 1;
        memcpy(s->fw, fw, n);
    }
}

/* found: a manifest's stamp as read (any bytes: never trusted). ours: this
 * firmware's. LIBSTAMP_OK only when every compared field is equal; the
 * first difference otherwise, in the order above. */
static inline enum libstamp_why libstamp_check(const struct libstamp *found,
                                               const struct libstamp *ours)
{
    static const struct libstamp zero;
    if (!found || !memcmp(found, &zero, sizeof(zero)))
        return LIBSTAMP_MISSING;
    if ((found->db_magic & 0xffffff00u) != LIBSTAMP_DB_FAMILY
        || found->sortkey == 0 || found->schema == 0
        || !memchr(found->fw, '\0', sizeof(found->fw)))
        return LIBSTAMP_CORRUPT;
    if (found->db_magic != ours->db_magic)
        return LIBSTAMP_DB_FORMAT;
    if (found->sortkey != ours->sortkey)
        return LIBSTAMP_SORTKEY;
    if (found->schema != ours->schema)
        return LIBSTAMP_SCHEMA_DIFF;
    return LIBSTAMP_OK;
}

static inline const char *libstamp_why_name(enum libstamp_why why)
{
    switch (why)
    {
        case LIBSTAMP_OK:          return "none";
        case LIBSTAMP_MISSING:     return "missing";
        case LIBSTAMP_CORRUPT:     return "corrupt";
        case LIBSTAMP_DB_FORMAT:   return "format";
        case LIBSTAMP_SORTKEY:     return "sortkey";
        case LIBSTAMP_SCHEMA_DIFF: return "schema";
    }
    return "?";
}

/* The compared fields for a log line (64 bytes a line): "a3/k3/s1" (the
 * format magic's last byte: its family is fixed), "xdeadbeef" (the
 * whole magic) for a
 * format outside the family, "none" for no stamp. Safe on any bytes. */
static inline const char *libstamp_format(const struct libstamp *s,
                                          char *buf, size_t n)
{
    static const struct libstamp zero;

    if (!s || !memcmp(s, &zero, sizeof(zero)))
        snprintf(buf, n, "none");
    else if ((s->db_magic & 0xffffff00u) == LIBSTAMP_DB_FAMILY)
        snprintf(buf, n, "%02lx/k%u/s%u",
                 (unsigned long)(s->db_magic & 0xff),
                 (unsigned)s->sortkey, (unsigned)s->schema);
    else
        snprintf(buf, n, "x%08lx", (unsigned long)s->db_magic);
    return buf;
}

/* The building firmware's version, printable ("-": none). Safe on any
 * bytes. */
static inline const char *libstamp_fw(const struct libstamp *s,
                                      char *buf, size_t n)
{
    size_t i;

    if (!n)
        return buf;
    for (i = 0; s && i < LIBSTAMP_FW_LEN && i + 1 < n && s->fw[i]; i++)
        buf[i] = (s->fw[i] > ' ' && s->fw[i] < 127) ? s->fw[i] : '?';
    buf[i] = '\0';
    if (!i && n > 1)
        snprintf(buf, n, "-");
    return buf;
}

#endif
