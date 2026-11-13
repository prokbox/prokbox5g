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

/* tests/artsrc (#332 follow-up): the art builder's source choice, the real
 * functions of apps/ipodui/artcache.c (source_decode(), source_sig(),
 * source_unchanged() and the lookups they call, extracted by run.sh into
 * extracted.c) over the real search_albumart_files() (apps/recorder/
 * albumart.c, built against tests/walkcache/artshim), with the storage
 * and the decoders stubbed: a table of files whose lookups are counted.
 *
 * For every track kind (embedded art that decodes or not, an image file
 * that decodes or not, neither, a PNG only, synced tracks with and
 * without a cover database image, videos with a poster) under every
 * album_art setting (off, prefer embedded, prefer file), and for
 * source_decode() with and without the panel set's embedded preference:
 *   - the sources tried, in order, and the result are those of a second,
 *     independent statement of the stock order (the oracle below);
 *   - the lookups made are 0 where embedded art comes first and the track
 *     has some that decodes (source_decode()), has some (source_sig()),
 *     or the old record is embedded (source_unchanged()); else exactly the
 *     lookups of one find_file() (plus png_folder()'s where it runs).
 * Every result line is printed (run.sh compares builds); exit 1 on any
 * FAIL. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <stdbool.h>
#include "metadata.h"               /* artshim: struct mp3entry, file.h */
#include "strmemccpy.h"

#ifndef O_RDONLY
#define O_RDONLY 0
#endif
#define ARRAYLEN(a) (sizeof(a) / sizeof((a)[0]))

bool search_albumart_files(const struct mp3entry *id3,
                           const char *size_string, char *buf, int buflen);

/* ---- what artcache.c's extracted functions see ---- */
enum { AA_OFF = 0, AA_PREFER_EMBEDDED, AA_PREFER_IMAGE_FILE };
static struct { int album_art; } global_settings;

enum artsrc_kind { ARTSRC_NONE = 0, ARTSRC_FILE = 1, ARTSRC_EMBEDDED = 2,
                   ARTSRC_SYNCDB = 3 };
enum { ARTREC_OK = 1, ARTREC_NOART = 2, ARTREC_FAILED = 3 };
#define ARTREC_T_SIG2       0x80
#define ARTREC_T_PANEL_EMB  0x20

struct artcache_rec
{
    uint8_t state, src_kind, src_type;
    uint32_t src_size, src_hash, src_crc;
};

struct syn_ent { uint32_t offset, size; };

struct bwork
{
    struct mp3entry id3;
    char src[MAX_PATH];
    char path[MAX_PATH];
    char album[96], artist[96], aartist[96];
    uint32_t syn_key;
    struct syn_ent syn;
    bool vid, panel_emb, source_retry, source_terminal;
    char tried[8];                  /* the sources tried, in order */
};

#define ART_PACE() ((void)0)

/* ---- the volume: every file that exists, and how it decodes ---- */
struct vfile { const char *path; uint32_t size; bool decodes; };
static const struct vfile files[] = {
    { "/Music/E2/cover.jpg",          5000, true  },
    { "/Music/E4/cover.jpg",          6000, true  },
    { "/Music/E5/cover.jpg",          7000, false },
    { "/Music/N2/cover.jpg",          8000, true  },
    { "/Music/N3/cover.png",          9000, true  },
    { "/Music/F1/folder.jpg",         9100, true  },
    { "/Movies/V1/poster.jpg",        9200, true  },
    { "/Movies/V2/poster.jpg",        9300, true  },
    { "/Movies/V3/poster.jpg",        9400, true  },
};

struct track
{
    const char *path, *album;
    bool emb, emb_ok;               /* embedded art; it decodes */
    bool file;                      /* the stock search (or the poster
                                       lookup) finds an image file */
    bool png;                       /* cover.png only (png_folder()) */
    bool synced, syn, syn_ok;       /* iPod_Control; map entry; decodes */
    bool vid;
};
static const struct track tracks[] = {
    { "/Music/E1/01.mp3", "E1", true,  true,  false, false, 0, 0, 0, 0 },
    { "/Music/E2/01.mp3", "E2", true,  true,  true,  false, 0, 0, 0, 0 },
    { "/Music/E3/01.mp3", "E3", true,  false, false, false, 0, 0, 0, 0 },
    { "/Music/E4/01.mp3", "E4", true,  false, true,  false, 0, 0, 0, 0 },
    { "/Music/E5/01.mp3", "E5", true,  true,  true,  false, 0, 0, 0, 0 },
    { "/Music/N1/01.mp3", "N1", false, false, false, false, 0, 0, 0, 0 },
    { "/Music/N2/01.mp3", "N2", false, false, true,  false, 0, 0, 0, 0 },
    { "/Music/N3/01.mp3", "N3", false, false, false, true,  0, 0, 0, 0 },
    { "/Music/F1/01.mp3", "F1", true,  true,  true,  false, 0, 0, 0, 0 },
    { "/iPod_Control/Music/F00/AAAA.m4a", "S1", true, true, false, false,
      1, 1, 1, 0 },
    { "/iPod_Control/Music/F01/BBBB.m4a", "S2", true, true, false, false,
      1, 0, 0, 0 },
    { "/iPod_Control/Music/F02/CCCC.m4a", "S3", true, false, false, false,
      1, 1, 0, 0 },
    { "/iPod_Control/Music/F03/DDDD.m4a", "S4", false, false, false, false,
      1, 1, 1, 0 },
#ifdef HAVE_BCMFW_VIDEO
    { "/Movies/V1/v.m4v", "V1", true,  true,  true,  false, 0, 0, 0, 1 },
    { "/Movies/V2/v.m4v", "V2", true,  false, true,  false, 0, 0, 0, 1 },
    { "/Movies/V3/v.m4v", "V3", false, false, true,  false, 0, 0, 0, 1 },
#endif
};
static const struct track *cur;     /* the track being looked at */
static unsigned long probes;        /* file_exists() calls */
static int failures;

static const struct vfile *vfind(const char *path)
{
    for (unsigned i = 0; i < ARRAYLEN(files); i++)
        if (!strcasecmp(files[i].path, path))
            return &files[i];
    return NULL;
}

bool file_exists(const char *path)
{
    probes++;
    return vfind(path) != NULL;
}

#define FD_TRACK 3
#define FD_SYNDB 4
#define FD_FILE0 10
int open(const char *path, int oflag, ...)
{
    const struct vfile *f;

    (void)oflag;
    if (cur && !strcmp(path, cur->path))
        return FD_TRACK;
    if (!strcmp(path, "/iPod_Control/Artwork/F1_1.ithmb"))
        return FD_SYNDB;
    f = vfind(path);
    return f ? FD_FILE0 + (int)(f - files) : -1;
}

int close(int fd)
{
    (void)fd;
    return 0;
}

off_t ffilesize(int fd)
{
    if (fd >= FD_FILE0 && fd < FD_FILE0 + (int)ARRAYLEN(files))
        return files[fd - FD_FILE0].size;
    return -1;
}

uint32_t artkey_fnv32(const char *s)
{
    uint32_t h = 2166136261u;

    while (*s)
        h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

static bool get_metadata(struct mp3entry *id3, int fd, const char *path)
{
    (void)fd;
    memset(id3, 0, sizeof(*id3));
    strcpy(id3->path, path);
    id3->album = (char *)cur->album;
    id3->artist = (char *)"Artist";
    id3->has_embedded_albumart = cur->emb;
    if (cur->emb)
    {
        id3->albumart.pos = 1000;
        id3->albumart.size = 4000;
        id3->albumart.type = 1;
    }
    return true;
}

static uint32_t src_crc(struct bwork *w, int fd, off_t off, uint32_t size,
                        bool sig2)
{
    (void)w;
    return (uint32_t)fd * 7919u + (uint32_t)off * 31u + size + sig2;
}

static void walk_hold(void) { }

static bool syncart_synced(const char *path)
{
    return !strncasecmp(path, "/iPod_Control/Music/", 20);
}

static bool syncart_lookup(uint32_t key, struct syn_ent *syn, char *buf,
                           size_t len)
{
    (void)key;
    if (!cur->syn)
        return false;
    syn->offset = 0x1000;
    syn->size = 80000;
    snprintf(buf, len, "/iPod_Control/Artwork/F1_1.ithmb");
    return true;
}

static uint32_t syn_hash(struct bwork *w)
{
    return artkey_fnv32(w->src) ^ (w->syn.offset * 2654435761u);
}

static bool syn_same(struct bwork *w, const struct artcache_rec *old)
{
    return syn_hash(w) == old->src_hash && w->syn.size == old->src_size;
}

static void tried(struct bwork *w, char c)
{
    size_t n = strlen(w->tried);

    if (n + 1 < sizeof(w->tried))
        w->tried[n] = c, w->tried[n + 1] = 0;
}

static bool try_file(struct bwork *w, struct artcache_rec *rec)
{
    const struct vfile *f = vfind(w->src);

    tried(w, 'F');
    if (!f)
    {
        w->source_retry = true;
        return false;
    }
    rec->src_kind = ARTSRC_FILE;
    rec->src_type = 1 | ARTREC_T_SIG2;
    rec->src_size = f->size;
    rec->src_hash = artkey_fnv32(w->src);
    rec->src_crc = f->size * 3u;
    if (!f->decodes)
        w->source_terminal = true;  /* a format it can't decode */
    return f->decodes;
}

static bool try_embedded(struct bwork *w, int fd, struct artcache_rec *rec)
{
    (void)fd;
    tried(w, 'E');
    rec->src_kind = ARTSRC_EMBEDDED;
    rec->src_type = 1 | ARTREC_T_SIG2;
    rec->src_size = w->id3.albumart.size;
    rec->src_hash = artkey_fnv32(w->path);
    rec->src_crc = 4242;
    if (!cur->emb_ok)
        w->source_terminal = true;
    return cur->emb_ok;
}

static bool try_syncdb(struct bwork *w, struct artcache_rec *rec, bool emb)
{
    tried(w, 'S');
    rec->src_kind = ARTSRC_SYNCDB;
    rec->src_type = 4 | ARTREC_T_SIG2 | (emb ? ARTREC_T_PANEL_EMB : 0);
    rec->src_size = w->syn.size;
    rec->src_hash = syn_hash(w);
    rec->src_crc = 99;
    if (!cur->syn_ok)
        w->source_terminal = true;
    return cur->syn_ok;
}

/* the functions under test, as they are in artcache.c */
#include "extracted.c"

/* ---- the oracle: the stock order, stated again ---- */
static bool file_first_now(void)
{
    return global_settings.album_art != AA_PREFER_EMBEDDED && !cur->vid;
}

static bool file_decodes(void)
{
    char dir[MAX_PATH];
    const char *slash = strrchr(cur->path, '/');
    const struct vfile *f;

    if (cur->vid)
    {
        snprintf(dir, sizeof(dir), "%.*sposter.jpg",
                 (int)(slash - cur->path) + 1, cur->path);
        f = vfind(dir);
    }
    else
    {
        snprintf(dir, sizeof(dir), "%.*scover.jpg",
                 (int)(slash - cur->path) + 1, cur->path);
        f = vfind(dir);
        if (!f)
        {
            snprintf(dir, sizeof(dir), "%.*sfolder.jpg",
                     (int)(slash - cur->path) + 1, cur->path);
            f = vfind(dir);
        }
    }
    return f && f->decodes;
}

static void oracle_decode(bool panel_emb, char *seq, int *state)
{
    bool ff = file_first_now(), syn_first = cur->syn
         && !(panel_emb && cur->emb), ok = false;
    char order[8];
    int n = 0;

    if (syn_first)
        order[n++] = 'S';
    if (cur->file && ff)
        order[n++] = 'F';
    if (cur->emb)
        order[n++] = 'E';
    if (cur->file && !ff)
        order[n++] = 'F';
    if (cur->syn && !syn_first)
        order[n++] = 'S';
    seq[0] = 0;
    for (int i = 0; i < n && !ok; i++)
    {
        seq[i] = order[i];
        seq[i + 1] = 0;
        ok = order[i] == 'S' ? cur->syn_ok
           : order[i] == 'E' ? cur->emb_ok : file_decodes();
    }
    if (ok)
        *state = ARTREC_OK;
    else if (!cur->file && !cur->emb && !cur->syn)
        *state = cur->png ? ARTREC_FAILED : ARTREC_NOART;
    else
        *state = ARTREC_FAILED;
}

/* the lookups one find_file() makes for the current track */
static unsigned long one_search(void)
{
    struct bwork w;
    unsigned long before = probes;

    memset(&w, 0, sizeof(w));
    strcpy(w.path, cur->path);
    w.vid = cur->vid;
    get_metadata(&w.id3, FD_TRACK, w.path);
    find_file(&w, false);
    return probes - before;
}

/* png_folder()'s lookups when source_decode() reaches it */
static unsigned long png_lookups(void)
{
    if (cur->file || cur->emb || cur->syn || cur->synced)
        return 0;
    return cur->png ? 1 : 2;
}

static void check(bool ok, const char *what)
{
    if (!ok)
    {
        printf("  FAIL %s\n", what);
        failures++;
    }
}

static void setup(struct bwork *w)
{
    memset(w, 0, sizeof(*w));
    strcpy(w->path, cur->path);
    strcpy(w->album, cur->album);
    strcpy(w->artist, "Artist");
    w->vid = cur->vid;
    w->syn_key = 1;
}

static const char *const aa_name[] = { "off", "emb", "file" };

static void run_decode(int aa, bool panel_emb)
{
    struct bwork w;
    struct artcache_rec rec;
    unsigned long before = probes, n, want;
    char seq[8];
    int st, ost;
    bool skip = !file_first_now() && cur->emb && cur->emb_ok;

    setup(&w);
    w.panel_emb = panel_emb;
    memset(&rec, 0, sizeof(rec));
    st = source_decode(&w, &rec);
    n = probes - before;
    oracle_decode(panel_emb, seq, &ost);
    /* skip: embedded art comes first and decodes (a synced track's
     * search makes no lookups at all, S1) */
    want = skip ? 0 : one_search() + png_lookups();
    printf("decode %s aa=%s panel=%d st=%d tried=%s kind=%d type=%02x "
           "size=%u hash=%08x crc=%u term=%d retry=%d probes=%lu\n",
           cur->album, aa_name[aa], panel_emb, st, w.tried, rec.src_kind,
           rec.src_type, rec.src_size, rec.src_hash, rec.src_crc,
           w.source_terminal, w.source_retry, n);
    check(st == ost, "state differs from the stock order");
    check(!strcmp(seq, w.tried), "sources tried differ from the stock order");
    check(n == want, "lookups");
    if (n != want)
        printf("  (lookups %lu, want %lu)\n", n, want);
}

static void run_sig(int aa, int want_kind)
{
    struct bwork w;
    struct artcache_rec now;
    unsigned long before = probes, n, want;
    bool ret, ff = file_first_now();
    int kind;

    setup(&w);
    memset(&now, 0, sizeof(now));
    ret = source_sig(&w, want_kind, true, &now);
    n = probes - before;
    kind = cur->syn ? ARTSRC_SYNCDB
         : cur->file && (ff || !cur->emb) ? ARTSRC_FILE
         : cur->emb ? ARTSRC_EMBEDDED : ARTSRC_NONE;
    want = cur->syn || (!ff && cur->emb) ? 0 : one_search();
    printf("sig %s aa=%s want=%d ret=%d kind=%d size=%u hash=%08x crc=%u "
           "type=%02x probes=%lu\n", cur->album, aa_name[aa], want_kind,
           ret, now.src_kind, now.src_size, now.src_hash, now.src_crc,
           now.src_type, n);
    check(now.src_kind == kind, "first source differs from the stock order");
    check(ret == (kind == want_kind && !cur->syn), "signature verdict");
    check(n == want, "lookups");
    if (n != want)
        printf("  (lookups %lu, want %lu)\n", n, want);
}

static void run_unchanged(int aa, const char *name,
                          const struct artcache_rec *old, bool expect)
{
    struct bwork w;
    unsigned long before = probes, n, want;
    bool ret, prefer_emb = !file_first_now();

    setup(&w);
    ret = source_unchanged(&w, old);
    n = probes - before;
    want = cur->synced || (old->src_kind == ARTSRC_EMBEDDED && prefer_emb)
           ? 0 : one_search();
    printf("unchanged %s aa=%s old=%s ret=%d probes=%lu\n", cur->album,
           aa_name[aa], name, ret, n);
    check(ret == expect, "verdict");
    check(n == want, "lookups");
    if (n != want)
        printf("  (lookups %lu, want %lu)\n", n, want);
}

static void unchanged_all(int aa)
{
    struct artcache_rec old;
    char cover[MAX_PATH];
    const struct vfile *f;
    const char *slash = strrchr(cur->path, '/');
    bool prefer_emb = !file_first_now(), found = cur->file;

    snprintf(cover, sizeof(cover), "%.*s%s", (int)(slash - cur->path) + 1,
             cur->path, cur->vid ? "poster.jpg" : "cover.jpg");
    f = vfind(cover);
    if (!f)
    {
        snprintf(cover, sizeof(cover), "%.*sfolder.jpg",
                 (int)(slash - cur->path) + 1, cur->path);
        f = vfind(cover);
    }

    memset(&old, 0, sizeof(old));
    old.src_kind = ARTSRC_EMBEDDED;
    run_unchanged(aa, "emb", &old, cur->synced ? !cur->syn
                  : prefer_emb || !found);
    old.src_kind = ARTSRC_NONE;
    run_unchanged(aa, "none", &old, cur->synced ? !cur->syn : !found);
    old.src_kind = ARTSRC_FILE;
    old.src_hash = artkey_fnv32(cover);
    old.src_size = f ? f->size : 0;
    run_unchanged(aa, "file", &old, !cur->synced && found);
    old.src_size += 1;
    run_unchanged(aa, "file-resized", &old, false);
    if (cur->synced)
    {
        struct bwork w;

        setup(&w);
        syncart_lookup(0, &w.syn, w.src, sizeof(w.src));
        old.src_kind = ARTSRC_SYNCDB;
        old.src_hash = syn_hash(&w);
        old.src_size = w.syn.size;
        run_unchanged(aa, "syncdb", &old, cur->syn);
    }
}

int main(void)
{
    unsigned long checked = 0;

    for (int aa = AA_OFF; aa <= AA_PREFER_IMAGE_FILE; aa++)
    {
        global_settings.album_art = aa;
        for (unsigned t = 0; t < ARRAYLEN(tracks); t++)
        {
            cur = &tracks[t];
            /* the table's 'file' is what the real search finds */
            {
                struct bwork w;

                setup(&w);
                get_metadata(&w.id3, FD_TRACK, w.path);
                if (find_file(&w, false) != cur->file)
                {
                    printf("FAIL table: %s file=%d\n", cur->album, cur->file);
                    failures++;
                }
            }
            run_decode(aa, false);
            run_decode(aa, true);
            run_sig(aa, ARTSRC_FILE);
            run_sig(aa, ARTSRC_EMBEDDED);
            unchanged_all(aa);
            checked++;
        }
    }
    printf("artsrc: %lu track x setting cases, %d failures\n", checked,
           failures);
    return failures != 0;
}
