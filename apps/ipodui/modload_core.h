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
 * iPod UI shell: module pack format, relocation, integrity (pure, host-tested)
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
#ifndef IPODUI_MODLOAD_CORE_H
#define IPODUI_MODLOAD_CORE_H

/* modules.pak, written by tools/modpak.py (little-endian):
 *   [0, MODPAK_INDEX)  struct modpak_hdr, zero padded
 *   per module, at a MODPAK_ALIGN boundary: the image (text + rodata as
 *   linked at link_base), then nreloc uint16 word indices (the image
 *   words that hold an address inside the module), padded to 4 bytes.
 * In the arena a module takes: image, then .bss at bss_off (zeroed at
 * every load and move), then its relocation list (kept for moves). */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define MODPAK_MAGIC    0x444f4d50u     /* "PMOD" */
#define MODPAK_VERSION  2               /* P12: v1 packs are refused
                                           (MODPAK_EVERSION) */
#define MODPAK_INDEX    2048
#define MODPAK_ALIGN    2048
#define MODPAK_MAX      16              /* the P12 tenants + 3 games + spare;
                                           index = 32 B hdr + 16 x 44 B */
#define MOD_ALIGN       16              /* DMA-safe placement in the arena */

/* -- P12 module classes (docs/phase12-brief.md section 2, 3.1) ---------- */
/* Every module carries one class: what it may use, what happens to
 * playback, how fast it must enter and leave. P12 builds Class A only;
 * B and C are defined here so the ABI does not change later. */
/* the most page loads a second a Class A module may declare (modpak.py
 * check's 1..30; brief 2.1) */
#define MOD_PAGES_MAX 30

enum mod_class { MOD_CLASS_A = 0,       /* lightweight, alongside music */
                 MOD_CLASS_B = 1,       /* a borrow from the audio buffer */
                 MOD_CLASS_C = 2,       /* exclusive, like video */
                 MOD_CLASS__N };
enum mod_ram   { MOD_RAM_32 = 0,        /* runs on 32 MB and 64 MB units */
                 MOD_RAM_64 = 1,        /* needs a 64 MB unit (B/C only) */
                 MOD_RAM__N };
enum mod_exit  { MOD_EXIT_GRACEFUL = 0, /* exit request, bounded time */
                 MOD_EXIT_IMMEDIATE = 1,
                 MOD_EXIT__N };
enum mod_kind  { MOD_KIND_SHELL = 0,    /* our cold screens: --just-symbols
                                           against the core, build ID */
                 MOD_KIND_SDK = 1,      /* games: linked only against the
                                           SDK table (section 2A),
                                           ABI-versioned */
                 MOD_KIND__N };
#define SDK_ABI_MAJOR   0               /* rev 0; a major change refuses */
#define SDK_ABI_MINOR   1               /* a module's minor <= the core's */
#define SDK_EXIT_MS_MAX 1000            /* Q19: graceful exit ceiling */
/* The classes this firmware runs (P12: A only), a mask of 1 << mod_class */
#define MOD_CLASSES_BUILT (1u << MOD_CLASS_A)

struct modpak_ent {                     /* 44 B on disk (v1: 32) */
    uint32_t id;            /* enum mod_id */
    uint32_t offset;        /* file offset of the image (MODPAK_ALIGN) */
    uint32_t image;         /* image bytes (multiple of 4) */
    uint32_t bss_off;       /* .bss offset from the image start */
    uint32_t bss;           /* .bss bytes */
    uint32_t nreloc;        /* uint16 word indices after the image */
    uint32_t nexport;       /* words of the export table at image offset 0 */
    uint32_t fletcher;      /* over image + relocation bytes (padded) */
    uint8_t  mclass;        /* enum mod_class */
    uint8_t  min_ram;       /* enum mod_ram */
    uint8_t  abi_major;     /* SDK modules; 0/0 for shell ones */
    uint8_t  abi_minor;
    uint16_t work_kb;       /* Class A working block, KB */
    uint8_t  kind;          /* enum mod_kind */
    uint8_t  exit_policy;   /* enum mod_exit */
    uint32_t borrow_kb;     /* Class B borrow, KB; 0 for A/C */
};

struct modpak_hdr {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    uint32_t build_id;      /* the core's (mod_core_build_id) */
    uint32_t link_base;     /* every module is linked here */
    uint32_t total;         /* file bytes */
    uint32_t reserved[3];
    struct modpak_ent ent[MODPAK_MAX];
};

_Static_assert(sizeof(struct modpak_ent) == 44, "modpak v2: entry size");
_Static_assert(sizeof(struct modpak_hdr) == 32 + MODPAK_MAX * 44,
               "modpak v2: index size");
_Static_assert(sizeof(struct modpak_hdr) <= MODPAK_INDEX,
               "modpak v2: the index fits its sector");

/* C1 (rev 2): the index is never a stack object. W1-F's job_open() reads
 * the 2 KB index sector in one read into the arena's top 2 KB (reserved
 * inside MOD_ARENA_SIZE: no module may use it; modpak.py checks
 * image + bss + work <= MOD_ARENA_SIZE - MOD_IDX_STAGE), checks it there
 * and compacts it into mod_rt[]. */
#define MOD_IDX_STAGE   2048
_Static_assert(MOD_IDX_STAGE >= MODPAK_INDEX, "the stage holds the index");

/* M1 (rev 2): one compact runtime table (W1-F, modload.c) replaces the
 * static struct modpak_hdr and the per-module slot statistics: the
 * loader's own state, read on every load (lean rule 9: can't share
 * because nothing else lives as long). Load counters and timings stay in
 * developer builds only, in a separate table. Natural alignment puts
 * this at 40 B (the brief's 36 B estimate packed it); W1-F may narrow
 * refs/state/flags. */
struct mod_rt {
    uint32_t id, offset, image, fletcher;
    uint16_t bss, nreloc, nexport;      /* bss <= 56 KB by the size rule */
    uint8_t  mclass, min_ram, kind, exit_policy;
    uint8_t  abi_major, abi_minor;
    uint16_t work_kb;
    uint32_t arena_off;                 /* in the arena, or ~0 */
    uint8_t  refs;                      /* W1-F: narrowed from uint16 (the
                                           stack holds <= UI_STACK_MAX + 2
                                           panel keys + pins) */
    uint8_t  lru;                       /* W1-F: last-use age for eviction */
    uint8_t  state, flags;              /* enum mod_state; MOD_RT_* */
};
_Static_assert(sizeof(struct mod_rt) == 40, "mod_rt: 40 B");
#define MOD_RT_HIDDEN   0x01            /* not runnable here: no row */
#define MOD_RT_PINNED   0x02            /* mod_pin(): no move, no eviction */
#define MOD_RT_INPAK    0x04            /* the pack has this module */
#define MOD_RT_VERIFIED 0x08            /* Fletcher-32 passed this mount
                                           generation */
#define MOD_RT_RELEASED 0x10            /* its last hold went this pass: its
                                           code may still be on the stack */
#define MOD_RT_DISCARD  0x20            /* H1: done with (a builder after its
                                           build): evicted at the next pass
                                           that finds it idle */

/* Export slot 0 of every module is its struct mod_desc; screens and other
 * exports start at slot 1 (W1-F, P12) */
#define MOD_SLOT_DESC   0
#define MOD_SLOT_FIRST  1

/* W1-F: the pack fields of entry e into r (the runtime fields refs, lru,
 * state, arena_off and the flags other than INPAK/HIDDEN are kept). The
 * entry must already have passed modpak_check(); returns MODPAK_EBOUNDS
 * for one mod_rt cannot hold (bss over 64 KB, .bss not right after the
 * image, more than 65535 relocations or exports), which then reads as
 * absent. */
int mod_rt_fill(struct mod_rt *r, const struct modpak_ent *e);
/* The arena footprint of a compacted entry (as modpak_footprint()) */
uint32_t mod_rt_footprint(const struct mod_rt *r);
/* Back to a modpak_ent's labels, for mod_why_hidden()/mod_desc_check() */
void mod_rt_ent(const struct mod_rt *r, struct modpak_ent *e);

/* The module header: export slot 0 of every module (W1-F moves today's
 * exports up by one). The loader compares it with the pack entry before
 * running anything; a mismatch is MODPAK_EHDR (logged, the module treated
 * as absent). In the simulator (modules link statically) it is still
 * slot 0, so the accountant knows each screen's class. */
#define MOD_HDR_MAGIC   0x484d4b50u     /* "PKMH" */
struct mod_desc {
    uint32_t magic;                     /* MOD_HDR_MAGIC */
    uint8_t  abi_major, abi_minor;      /* SDK_ABI_* it was built against */
    uint8_t  mclass;                    /* enum mod_class */
    uint8_t  min_ram;                   /* enum mod_ram (Class A: MOD_RAM_32
                                           only; modpak.py refuses 64) */
    uint8_t  kind;                      /* enum mod_kind */
    uint8_t  exit_policy;               /* enum mod_exit */
    uint16_t exit_ms;                   /* graceful: asked-for time, capped
                                           at SDK_EXIT_MS_MAX (Q19) */
    uint16_t work_kb;                   /* Class A working block */
    uint16_t quota_kb;                  /* private storage quota (2A.5) */
    uint32_t borrow_kb;                 /* Class B */
    uint8_t  boost;                     /* 0 bursts only, 1 session (B/C) */
    uint8_t  pages;                     /* Class A paged lists (W3-K): page
                                           loads a second it may make
                                           while browsing; 0 none, at most
                                           MOD_PAGES_MAX (mod_desc_check) */
    uint16_t state;                     /* SDK modules: bytes of the game's
                                           state record (sdk.md 4.1); 0 for
                                           shell modules */
    uint16_t name_lang;                 /* shell: LANG_* id of its title */
    uint16_t names_off;                 /* SDK: offset of its own per-
                                           language name table (UTF-8),
                                           built from .lang sources */
};
_Static_assert(sizeof(struct mod_desc) == 28, "mod_desc: 28 B");

enum modpak_err {
    MODPAK_OK = 0,
    MODPAK_EMAGIC = -1,
    MODPAK_EVERSION = -2,
    MODPAK_EBUILD = -3,     /* a module from another build: refused */
    MODPAK_EBOUNDS = -4,
    MODPAK_ERELOC = -5,
    MODPAK_ESUM = -6,
    MODPAK_EHDR = -7,       /* mod_desc and the pack entry disagree */
};

/* Header and every entry checked against the file size and the core's
 * build ID */
int modpak_check(const struct modpak_hdr *h, uint32_t file_bytes,
                 uint32_t build_id);
const struct modpak_ent *modpak_find(const struct modpak_hdr *h, uint32_t id);
/* Bytes read from the file: image + relocations (padded) */
uint32_t modpak_read_bytes(const struct modpak_ent *e);
/* Arena bytes: image/.bss, then relocations, rounded to MOD_ALIGN */
uint32_t modpak_footprint(const struct modpak_ent *e);
/* Offset of the kept relocation list inside the footprint */
uint32_t modpak_reloc_at(const struct modpak_ent *e);

/* Why a module is hidden on this unit (logged "mod hide <name> why="):
 * never a row, a page or a splash (native illusion) */
enum mod_why { MOD_WHY_OK = 0, MOD_WHY_ABI, MOD_WHY_RAM, MOD_WHY_CLASS,
               MOD_WHY_HDR,
               MOD_WHY_ARENA };     /* L3: a game in a build with no module
                                       arena (modload.c static_init()) */
/* Runnable here: SDK modules need the ABI major equal and the minor <=
 * the core's (shell modules carry 0/0 and the build ID instead); min_ram
 * <= the unit's (unit_ram, enum mod_ram from the probed RAM size); the
 * class built into this firmware (classes, a MOD_CLASSES_BUILT mask).
 * Returns enum mod_why; modload.c's mod_runnable() wraps it. */
int mod_why_hidden(const struct modpak_ent *e, int unit_ram,
                   unsigned classes);
/* export slot 0 against the pack entry: MODPAK_OK or MODPAK_EHDR */
int mod_desc_check(const struct mod_desc *d, const struct modpak_ent *e);

/* Fletcher-32 over 16-bit little-endian words (n rounded up, zero pad) */
uint32_t mod_fletcher32(const void *p, size_t n);

/* Adds delta to every listed image word; MODPAK_ERELOC if an index is
 * outside the image (nothing is changed then) */
int mod_relocate(void *image, uint32_t image_bytes, const uint16_t *rel,
                 uint32_t n, int32_t delta);

/* After the read of modpak_read_bytes() at base: move the relocation
 * list to its kept place, zero .bss, apply the relocations for a module
 * now at run_addr. Returns MODPAK_OK or an error (the slot is unusable). */
int mod_settle(void *base, const struct modpak_ent *e, uint32_t link_base,
               uint32_t run_addr);

/* First-fit placement among the used spans [off, off+len), sorted by off
 * or not; returns the offset or -1 */
struct mod_span { uint32_t off, len; };
long mod_fit(const struct mod_span *used, int n, uint32_t arena,
             uint32_t need);

/* -- making room (pure: the policy modload.c runs, host-tested) --------- */
/* The arena's tenants as the policy sees them. age(i) is a module's last-
 * use age (larger = older) when it may be evicted: placed, idle (no hold,
 * no pin, not loading, not live, not released this pass); MOD_AGE_HELD
 * when it may not; MOD_AGE_YIELD when it may not, but is pinned by a
 * background job (the sync indexer) that can be asked to stop. span(i)
 * says where module i sits (false: not in the arena). extra() lists the
 * other used spans (the Class A working blocks). compact() slides idle
 * modules down (NULL: none). */
#define MOD_AGE_HELD    (-1)
#define MOD_AGE_YIELD   (-2)
#define MOD_ROOM_SPANS  (MODPAK_MAX + 4)    /* modules + working blocks */
struct mod_room {
    int  n;                             /* module ids 0..n-1 */
    void *ctx;
    bool (*span)(void *ctx, int i, struct mod_span *s);
    int  (*age)(void *ctx, int i);
    int  (*extra)(void *ctx, struct mod_span *sp, int max);
    void (*evict)(void *ctx, int i);
    void (*compact)(void *ctx);
};
/* An offset for need bytes in [0, arena): free space, else after a
 * compaction, else after evicting idle modules, least recently used first
 * (never keep, never a held or yielding one). MOD_ROOM_NONE: no room even
 * with every idle module gone; MOD_ROOM_YIELD: room once a yielding job
 * lets go (nothing was evicted for it: the caller asks the job to stop
 * and tries again). */
#define MOD_ROOM_NONE   (-1L)
#define MOD_ROOM_YIELD  (-2L)
long mod_room_find(const struct mod_room *r, uint32_t arena, uint32_t need,
                   int keep);
/* Could mod_room_find() succeed, evicting idle modules if it must? No
 * eviction, no move: 1 yes, 0 no, MOD_ROOM_YIELD only once a yielding job
 * lets go. */
int mod_room_possible(const struct mod_room *r, uint32_t arena,
                      uint32_t need, int keep);

/* Writes n bytes at off into a file of *size bytes through io, growing a
 * shorter file with zeros first: Rockbox's lseek refuses to go past the
 * end (file.c), so a new file must be filled in order. 0, or -1. */
struct mod_fileio {
    long (*seek)(void *h, uint32_t off);        /* SEEK_SET; <0 error */
    long (*write)(void *h, const void *p, uint32_t n);
};
int mod_file_put(const struct mod_fileio *io, void *h, uint32_t *size,
                 uint32_t off, const void *p, uint32_t n);

#endif /* IPODUI_MODLOAD_CORE_H */
