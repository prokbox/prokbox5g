/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2005 by Miika Pekkarinen
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

/*
 *                    TagCache API
 *
 *       ----------x---------x------------------x-----
 *                 |         |                  |              External
 * +---------------x-------+ |       TagCache   |              Libraries
 * | Modification routines | |         Core     |
 * +-x---------x-----------+ |                  |
 *   | (R/W)   |             |                  |           |
 *   |  +------x-------------x-+  +-------------x-----+     |
 *   |  |                      x==x Filters & clauses |     |
 *   |  | Search routines      |  +-------------------+     |
 *   |  |                      x============================x DirCache
 *   |  +-x--------------------+                            | (optional)
 *   |    | (R)                                             |
 *   |    | +-------------------------------+  +---------+  |
 *   |    | | DB Commit (sort,unique,index) |  |         |  |
 *   |    | +-x--------------------------x--+  | Control |  |
 *   |    |   | (R/W)                    | (R) | Thread  |  |
 *   |    |   | +----------------------+ |     |         |  |
 *   |    |   | | TagCache DB Builder  | |     +---------+  |
 *   |    |   | +-x-------------x------+ |                  |
 *   |    |   |   | (R)         | (W)    |                  |
 *   |    |   |   |          +--x--------x---------+        |
 *   |    |   |   |          | Temporary Commit DB |        |
 *   |    |   |   |          +---------------------+        |
 * +-x----x-------x--+                                      |
 * | TagCache RAM DB x==\(W) +-----------------+            |
 * +-----------------+   \===x                 |            |
 *   |    |   |   |      (R) |  Ram DB Loader  x============x DirCache
 * +-x----x---x---x---+   /==x                 |            | (optional)
 * | Tagcache Disk DB x==/   +-----------------+            |
 * +------------------+                                     |
 *
 */

#if !defined(PLUGIN)

/*#define LOGF_ENABLE*/
/*#define LOGF_CLAUSES define to enable logf clause matching (LOGF_ENABLE req'd) */

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#ifdef APPLICATION
#include <unistd.h> /* readlink() */
#include <limits.h> /* PATH_MAX */
#endif
#include "config.h"
#include "ata_idle_notify.h"
#if defined(HAVE_IPOD_UI) && defined(HAVE_TC_RAMCACHE) && !defined(__PCTOOL__)
#include "storage.h"      /* storage_disk_is_active() */
#ifdef HAVE_STORAGE_POWEROFF
#include "stor_pwr.h"     /* #114: storage_power_warm() */
#endif
#endif
#include "thread.h"
#include "kernel.h"
#include "system.h"
#include "logf.h"
#include "string-extra.h"
#include "usb.h"
#include "metadata.h"
#include "chapters.h"
#include "tagcache.h"
#include "yesno.h"
#include "core_alloc.h"
#include "crc32.h"
#include "misc.h"
#include "settings.h"
#include "dir.h"
#include "pathfuncs.h"
#include "debug.h"
#include "dircache.h"
#include "errno.h"
#if defined(HAVE_IPOD_UI) && !defined(PLUGIN) && !defined(__PCTOOL__)
#include "ipodui/sortkey.h"
#include "audio.h"
#include "power.h"
#ifdef HAVE_TC_RAMCACHE
#include "ipodui/libfile.h"
#include "ipodui/library.h"
#include "ipodui/dbgen.h"
#include "ipodui/dbjournal.h"
#include "ipodui/uilog.h"
#include "ipodui/maint.h"
#include "ipodui/artcache.h"    /* P18: artcache_user_idle() */
#include "ipodui/bootstamp.h"
#include "ipodui/sleep.h"       /* P18: the RTC for the Q11 walk */
#include "ipodui/sleep_core.h"
#include "ipodui/dbqueue.h"     /* #109: queues by database index */
#include "quiet_wait_core.h"    /* P19 19-1a: qw_tc_wait() */
#include "backlight.h"
#include "devstats.h"           /* P19 19-1a: DEVSTATS_POLL */
#include "playback.h"
#ifdef HAVE_BCMFW_VIDEO
#include "ipodui/vclass.h"
#endif
#endif
#define UI_COLLATE      /* tag files are sorted in the shell's order */
#define UI_SORTKEY_LEN  96  /* compare() key length (sets the order) */
#ifdef HAVE_TC_RAMCACHE
#define UI_COMMIT       /* bounded commit buffer, never evicts playback */
/* P19 19-1a: the thread waits for owed work or a deadline, not 1 s */
#define TC_QUIET_WAIT
#ifdef SIMULATOR
#define UI_TIMING       /* commit/sort times on the sim console */
#endif
#endif
#endif

#ifndef __PCTOOL__
#include "lang.h"
#include "eeprom_settings.h"
#endif
#ifdef UI_COMMIT
/* set by tagcache_prepare_shutdown(): a power-off cancels a running
 * commit instead of being refused (see there) */
static volatile bool ui_commit_cancel;
/* The commit's checkpoints all lie in its in-place phase: once the master
 * header is marked dirty, build_index() truncates and rewrites the tag
 * files and the master index where they are. A power-off cuts that phase
 * (not its last stage, ui_commit_finishing): that database is then
 * rebuilt, never merged into again. USB is honoured before it
 * (ui_commit_usb_queued(), commit()). With generations the commit writes
 * a candidate, unreferenced until its manifest rename: USB cuts it too,
 * at once, and the candidate is dropped at the connect and built again
 * afterwards (no in-place wait while the host waits). Every shell commit
 * writes a candidate; the ui_build test only keeps the stock rule for a
 * commit that would not. */
#ifdef HAVE_IPOD_UI
/* P18 (#86): the sleep-entry window closed with a commit running: its
 * candidate stops at the next checkpoint, as at USB (the active library
 * stays published and clean), and the update is owed to a later window
 * (ui_sleep_rescan). Set and cleared by sleep.c, UI thread. */
static volatile bool ui_sleep_cut;
/* P18: a tagcache-thread job the cut cannot stop midway (a database
 * queue's rebase or materialization, a RAM cache reload) runs: sleep
 * entry waits for it before the statistics drain (tagcache_ui_sleep_busy());
 * set before the cut is read, so the UI thread never misses one */
static volatile bool ui_sleep_job;
#define UI_SLEEP_CUT ui_sleep_cut
/* P18 review M2: sleep entry's drain goes out now, past the journal's
 * retry back-off (ui_drain_wanted()); cleared by the drain */
static volatile bool ui_drain_sleep;
/* P18 review M3: a wake since the stamp sleep entry wrote: it is removed
 * at the first natural disk wake, before this thread writes anything */
static volatile bool ui_stamp_spoil;
static long ui_spoil_from;      /* the wake's tick */
/* P18 review L2: woke, and no natural moment since: a cut update waits
 * for a window or the user's idle, never runs right at the wake */
static volatile bool ui_sleep_woke;
#else
#define UI_SLEEP_CUT false
#endif
#define USR_CANCEL (ui_commit_cancel \
                    || (ui_build != ui_active \
                        && (ui_commit_usb_queued() || UI_SLEEP_CUT)))
/* the commit is in its last stage (build_numeric_indices()): a power-off
 * waits for it instead of cancelling it */
static volatile bool ui_commit_finishing;
#else
#define USR_CANCEL false
#endif

/* The USB event is queued to this thread, but a 1 TB scan can be between
 * queue polls while the host is already waiting for mass storage.  The UI
 * sets this before its USB acknowledgement, making the existing scan and
 * commit checkpoints stop immediately regardless of queue ordering. */
static volatile bool tagcache_usb_quiescing;
#else/*!defined(PLUGIN)*/
#define USR_CANCEL (tc_stat.commit_delayed == true)
#endif /*!defined(PLUGIN)*/
/*
 * Define this to support non-native endian tagcache files.
 * Databases are always written in native endian so this is
 * basically only necessary to support databases generated
 * by the PC database tool.
 *
 * Adds around 0.5-1.0k of code.
 */
#define TAGCACHE_SUPPORT_FOREIGN_ENDIAN

/* Allow a little drift to the filename ordering (should not be too high/low). */
#define POS_HISTORY_COUNT 4

/* How much to pre-load entries while committing to prevent seeking. */
#define IDX_BUF_DEPTH 64

/* Tag Cache Header version 'TCHxx'. Increment when changing internal structures. */
#ifdef UI_COLLATE
/* the shell stores class flags (TAGCACHE_UI_*) and a title for untagged
 * files at scan time, and its tag files in ui_groupkey() order (#47:
 * ui_sortkey() of the case-folded name, ties by ui_casefold_cmp()): a
 * database without them, or sorted by an older ui_sortkey(), is rebuilt.
 * The last byte follows SORTKEY_VERSION (ipodui/sortkey.h), which
 * tests/ipodui/test_sortkey_version.c forces up whenever the key order
 * changes; a change of the grouping rules alone (UI_GROUPING_VERSION, same
 * test) only re-sorts the tag files once (ui_resort_owed()). Not for the db_commit plugin (PLUGIN): it sorts the stock way,
 * so it must not write a database the shell takes as its own. */
#define TAGCACHE_MAGIC  (0x544348a0 + SORTKEY_VERSION)
#else
#define TAGCACHE_MAGIC  0x54434810
#endif

/* Dump store/restore header version 'TCSxx'. */
#define TAGCACHE_STATEFILE_MAGIC 0x54435301

/* How much to allocate extra space for ramcache. */
#define TAGCACHE_RESERVE 32768

/*
 * Define how long one entry must be at least (longer -> less memory at commit).
 * Must be at least 4 bytes in length for correct alignment.
 */
#define TAGFILE_ENTRY_CHUNK_LENGTH   8

/* Used to guess the necessary buffer size at commit. */
#define TAGFILE_ENTRY_AVG_LENGTH   16

/* Max events in the internal tagcache command queue. */
#define TAGCACHE_COMMAND_QUEUE_LENGTH 32

/* Idle time before committing events in the command queue. */
#define TAGCACHE_COMMAND_QUEUE_COMMIT_DELAY  HZ*2

/* Dont commit database_tmp data. */
#define TAGCACHE_FILE_NOCOMMIT  "database_commit.ignore"

/* Temporary database containing new tags to be committed to the main db. */
#define TAGCACHE_FILE_TEMP       "database_tmp.tcd"

/* The main database master index and numeric data. */
#define TAGCACHE_FILE_MASTER     "database_idx.tcd"

/* The main database string data. */
#define TAGCACHE_FILE_INDEX      "database_%d.tcd"

/* ASCII dumpfile of the DB contents. */
#define TAGCACHE_FILE_CHANGELOG  "database_changelog.txt"
#define UI_EXPORT_END   "## end\n"  /* last line of a complete export */
#ifdef UI_COMMIT
/* Marks a changelog import still to run (empty file) */
#define TAGCACHE_FILE_IMPORT     "database_import.pending"
#endif

/* Serialized DB. */
#define TAGCACHE_STATEFILE       "database_state.tcd"

/* Flags */
#define FLAG_DELETED     0x0001  /* Entry has been removed from db */
#define FLAG_DIRCACHE    0x0002  /* Filename is a dircache pointer */
#define FLAG_DIRTYNUM    0x0004  /* Numeric data has been modified */
#define FLAG_TRKNUMGEN   0x0008  /* Track number has been generated  */
#define FLAG_RESURRECTED 0x0010  /* Statistics data has been resurrected */
#ifdef UI_COMMIT
#define FLAG_UI_STATS_PENDING 0x0800 /* RAM-only checkpoint recovery tail */
/* RAM-only, during the boot replay: a delta or relative frame changed this
 * row; a failed replay restores its statistics from the master on disk */
#define FLAG_UI_STATS_REPLAYED   0x400000
/* On disk, set by a candidate's statistics replay where a journal value
 * changed the row: the generation it was built from (the fallback, if
 * this one fails validation) holds the older value, so a checkpoint
 * writes this row even when it equals the master (MED-A). Kept by
 * update builds (a clone): extra frames only, never a missing one. */
#define FLAG_UI_STATS_FOLDED     0x2000
/* Statistics frames and queue entries: the tag in the low 6 bits, with
 * TAGCACHE_UI_DELTA (add the value) or TAGCACHE_UI_REL (a lastplayed
 * taken before the boot replay, whose serial can be below the journal's:
 * the replay gives it the next serial after every one so far; its value
 * is 0, MED-2). Neither needs a base,
 * so both are journaled before, during and after the replay; the replay
 * applies frames in order onto the master's values. Masters never fold
 * them (a candidate's statistics take absolutes only): the active
 * snapshot's replay adds them, after every publication too. */
#define TAGCACHE_UI_REL          0x40
#define UI_STAT_TAG(t)           ((t) & 0x3f)
typedef char ui_stat_tag_check[TAG_COUNT <= 0x40 ? 1 : -1];
#endif
#if defined(HAVE_IPOD_UI) && defined(HAVE_TC_RAMCACHE) && !defined(__PCTOOL__)
/* The shell's class bits (TAGCACHE_UI_*, tagcache.h) share the index flag
 * word with the FLAG_* bits above: they must never overlap. */
typedef char tagcache_ui_flag_check[
    ((TAGCACHE_UI_AUDIOBOOK | TAGCACHE_UI_PODCAST | TAGCACHE_UI_COMPILATION)
     & (FLAG_DELETED | FLAG_DIRCACHE | FLAG_DIRTYNUM
        | FLAG_TRKNUMGEN | FLAG_RESURRECTED)) == 0 ? 1 : -1];
#ifdef UI_COMMIT
/* every shell bit, the RAM-only statistics marks included, apart */
typedef char tagcache_ui_flag_check_all[
    ((TAGCACHE_UI_AUDIOBOOK | TAGCACHE_UI_PODCAST | TAGCACHE_UI_COMPILATION
      | TAGCACHE_UI_VIDEO | TAGCACHE_UI_TV | TAGCACHE_UI_VIDEO_CHECKED
      | TAGCACHE_UI_VIDEO_OTHER)
     & (FLAG_DELETED | FLAG_DIRCACHE | FLAG_DIRTYNUM | FLAG_TRKNUMGEN
        | FLAG_RESURRECTED | FLAG_UI_STATS_PENDING | FLAG_UI_STATS_REPLAYED
        | FLAG_UI_STATS_FOLDED))
    == 0
    && (TAGCACHE_UI_VIDEO_CHECKED & TAGCACHE_UI_VIDEO_OTHER) == 0 ? 1 : -1];
#endif
#ifdef HAVE_BCMFW_VIDEO
typedef char tagcache_ui_video_flag_check[
    ((TAGCACHE_UI_VIDEO | TAGCACHE_UI_TV)
     & (FLAG_DELETED | FLAG_DIRCACHE | FLAG_DIRTYNUM | FLAG_TRKNUMGEN
        | FLAG_RESURRECTED | TAGCACHE_UI_AUDIOBOOK | TAGCACHE_UI_PODCAST
        | TAGCACHE_UI_COMPILATION | 0xffff0000)) == 0
    && VCLASS_VIDEO == TAGCACHE_UI_VIDEO && VCLASS_TV == TAGCACHE_UI_TV
    && VCLASS_PODCAST == TAGCACHE_UI_PODCAST ? 1 : -1];
#endif
#endif

#ifdef __PCTOOL__
#define yield() do { } while(0)
#define sim_sleep(timeout) do { } while(0)
#define do_timed_yield() do { } while(0)
static void db_log(const char *prefix, const char *msg);
#endif

#ifdef __PCTOOL__
#define DB_LOG(__prefix, __file) db_log(__prefix, __file)
#else
#define DB_LOG(...)
#endif

#ifndef __PCTOOL__
/* Tag Cache thread. */
static struct event_queue tagcache_queue SHAREDBSS_ATTR;
static long tagcache_stack[(DEFAULT_STACK_SIZE + 0x4000)/sizeof(long)];
static const char tagcache_thread_name[] = "tagcache";
#endif

/* Previous path when scanning directory tree recursively. */
static char curpath[TAGCACHE_BUFSZ];
/* Shared buffer for several build_index fns to reduce stack usage */
static char build_idx_buf[TAGCACHE_BUFSZ];
static const long build_idx_bufsz = sizeof(build_idx_buf);
/* Used when removing duplicates. */
static char *tempbuf;     /* Allocated when needed. */
static long tempbufidx;   /* Current location in buffer. */
static size_t tempbuf_size; /* Buffer size (TEMPBUF_SIZE). */
static long tempbuf_left; /* Buffer space left. */
static long tempbuf_pos;
#ifndef __PCTOOL__
static int tempbuf_handle;
#endif

#define SORTED_TAGS_COUNT 9
#define TAGCACHE_IS_UNIQUE(tag) (BIT_N(tag) & TAGCACHE_UNIQUE_TAGS)
#define TAGCACHE_IS_SORTED(tag) (BIT_N(tag) & TAGCACHE_SORTED_TAGS)
#define TAGCACHE_IS_NUMERIC_OR_NONUNIQUE(tag) \
    (BIT_N(tag) & (TAGCACHE_NUMERIC_TAGS | ~TAGCACHE_UNIQUE_TAGS))
/* Tags we want to get sorted (loaded to the tempbuf). */
#define TAGCACHE_SORTED_TAGS ((1LU << tag_artist) | (1LU << tag_album) | \
    (1LU << tag_genre) | (1LU << tag_composer) | (1LU << tag_comment) | \
    (1LU << tag_albumartist) | (1LU << tag_grouping) | (1LU << tag_title) | \
    (1LU << tag_virt_canonicalartist))

/* Uniqued tags (we can use these tags with filters and conditional clauses). */
#define TAGCACHE_UNIQUE_TAGS ((1LU << tag_artist) | (1LU << tag_album) | \
    (1LU << tag_genre) | (1LU << tag_composer) | (1LU << tag_comment) | \
    (1LU << tag_albumartist) | (1LU << tag_grouping) | \
    (1LU << tag_virt_canonicalartist))

/* String presentation of the tags defined in tagcache.h. Must be in correct order! */
static const char * const tags_str[] = { "artist", "album", "genre", "title",
    "filename", "composer", "comment", "albumartist", "grouping", "year",
    "discnumber", "tracknumber", "canonicalartist", "bitrate", "length",
    "playcount", "rating", "playtime", "lastplayed", "commitid", "mtime",
    "lastelapsed", "lastoffset"
#if !defined(LOGF_ENABLE) || !defined(LOGF_CLAUSES)
};
#define logf_clauses(...) do { } while(0)
#else /* strings for logf debugging */
    "tag_virt_basename", "tag_virt_length_min", "tag_virt_length_sec",
    "tag_virt_playtime_min", "tag_virt_playtime_sec",
    "tag_virt_entryage", "tag_virt_autoscore"
};
/* more debug strings */
static const char * const tag_type_str[] = {
    [clause_none] = "clause_none", [clause_is] = "clause_is",
    [clause_is_not] = "clause_is_not", [clause_gt] = "clause_gt",
    [clause_gteq] = "clause_gteq", [clause_lt] = "clause_lt",
    [clause_lteq] = "clause_lteq", [clause_contains] = "clause_contains",
    [clause_not_contains] = "clause_not_contains",
    [clause_begins_with] = "clause_begins_with",
    [clause_not_begins_with] = "clause_not_begins_with",
    [clause_ends_with] = "clause_ends_with",
    [clause_not_ends_with] = "clause_not_ends_with",
    [clause_oneof] = "clause_oneof",
    [clause_contains_oneof] = "clause_contains_oneof",
    [clause_begins_oneof] = "clause_begins_oneof",
    [clause_ends_oneof] = "clause_ends_oneof",
    [clause_not_oneof] = "clause_not_oneof",
    [clause_not_contains_oneof] = "clause_not_contains_oneof",
    [clause_not_begins_oneof] = "clause_not_begins_oneof",
    [clause_not_ends_oneof] = "clause_not_ends_oneof",
    [clause_logical_or] = "clause_logical_or"
 };
#define logf_clauses logf
#endif /* !defined(LOGF_ENABLE) || !defined(LOGF_CLAUSES) */

#if defined(PLUGIN)
char *itoa_buf(char *buf, size_t bufsz, long int i)
{
    snprintf(buf, bufsz, "%ld", i);
    return buf;
}
#endif

/* Status information of the tagcache. */
#ifndef UI_COMMIT
static struct tagcache_stat tc_stat;
#endif
#ifdef HAVE_IPOD_UI
static bool ui_maintenance_rebuild;
static volatile bool ui_scan_active;
/* #84 (Q6): "._" sidecars seen by the scan in progress / by the last
 * scan that ran to its end. Counted where the walk already skips them. */
static int ui_sidecars_run, ui_sidecars_done = -1;
#endif
#ifdef UI_TIMING
/* scan speed (1019), sim console: lookups, tag-file entries read by
 * them, master opens */
static long ui_scan_lookups, ui_scan_reads, ui_master_opens;
/* tests/perf/scan_misses.sh: SIM_TC_SCAN_LEGACY=1 measures the scan as
 * it was (no table, an open per track, no probe, the clone up front) */
#include <stdlib.h>
#define UI_SCAN_LEGACY() (getenv("SIM_TC_SCAN_LEGACY") != NULL)
#else
#define UI_SCAN_LEGACY() 0
#endif

/* Queue commands. */
enum tagcache_queue {
    Q_STOP_SCAN = 0,
    Q_START_SCAN,
    Q_IMPORT_CHANGELOG,
    Q_UPDATE,
    Q_REBUILD,
#ifdef UI_COMMIT
    Q_FLUSH_STATS,
    Q_BOOT_CHECK,       /* #87: the boot window starts the check now */
#endif
#ifdef TC_QUIET_WAIT
    Q_QUIET_KICK,       /* P19 19-1a: work owed; handled as a timeout */
#endif

    /* Internal tagcache command queue. */
    CMD_UPDATE_MASTER_HEADER,
    CMD_UPDATE_NUMERIC,
};

#ifdef TC_QUIET_WAIT
/* P19 19-1a: set while the thread waits with nothing owed (a deadline or
 * the cap); a producer then posts one Q_QUIET_KICK. Never while the
 * thread runs, so a scan's or a precompute's queue peek never sees one. */
static volatile bool tc_quiet_blocked;
#endif

struct tagcache_command_entry {
#ifdef UI_COMMIT
    struct tagcache_context *context;
    int32_t idx_id;
    int32_t data;
    uint8_t command;            /* 16 B an entry, not 20 (32 of them) */
    uint8_t tag;
#else
    int32_t command;
    int32_t idx_id;
    int32_t tag;
    int32_t data;
#endif
};

#ifndef __PCTOOL__
static struct tagcache_command_entry command_queue[TAGCACHE_COMMAND_QUEUE_LENGTH];
static volatile int command_queue_widx = 0;
static volatile int command_queue_ridx = 0;
static struct mutex command_queue_mutex SHAREDBSS_ATTR;
#endif

/* Tag database structures. */

/* Variable-length tag entry in tag files. */
struct tagfile_entry {
    int32_t tag_length;  /* Length of the data in bytes including '\0' */
    int32_t idx_id;      /* Corresponding entry location in index file of not unique tags */
    char tag_data[0];  /* Begin of the tag data */
};

/* Fixed-size tag entry in master db index. */
struct index_entry {
    int32_t tag_seek[TAG_COUNT]; /* Location of tag data or numeric tag data */
    int32_t flag;                /* Status flags */
};

/* Header is the same in every file. */
struct tagcache_header {
    int32_t magic;       /* Header version number */
    int32_t datasize;    /* Data size in bytes */
    int32_t entry_count; /* Number of entries in this file */
};

struct master_header {
    struct tagcache_header tch;
    int32_t serial; /* Increasing counting number */
    int32_t commitid; /* Number of commits so far */
    int32_t dirty;
};

#ifndef UI_COMMIT
static struct master_header current_tcmh;
#endif

#ifdef HAVE_TC_RAMCACHE

#define TC_ALIGN_PTR(p, type, gap_out_p) \
    ({ typeof (p) __p = (p);                                  \
       typeof (p) __palgn = ALIGN_UP(__p, __alignof__(type)); \
       *(gap_out_p) = (char *)__palgn - (char *)__p;          \
       __palgn; })

#define IF_TCRCDC(...) IF_DIRCACHE(__VA_ARGS__)

#ifdef HAVE_DIRCACHE
#define tcrc_dcfrefs \
    ((struct dircache_fileref *)(tcramcache.hdr->tags[tag_filename] + \
                                  sizeof (struct tagcache_header)))
#endif /* HAVE_DIRCACHE */

/* Header is created when loading database to ram. */
struct ramcache_header {
    char *tags[TAG_COUNT];       /* Tag file content (dcfrefs if tag_filename) */
    int entry_count[TAG_COUNT];  /* Number of entries in the indices. */
    struct index_entry indices[0]; /* Master index file content */
};

#ifdef HAVE_EEPROM_SETTINGS
struct statefile_header {
    int32_t magic;                /* Statefile version number */
    struct master_header mh;      /* Header from the master index */
    struct ramcache_header *hdr;  /* Old load address of hdr for relocation */
    struct tagcache_stat tc_stat;
};
#endif /* HAVE_EEPROM_SETTINGS */

/* In-RAM ramcache structure (not persisted) */
#ifdef UI_COMMIT
struct tcramcache
#else
static struct tcramcache
#endif
{
    struct ramcache_header *hdr;      /* allocated ramcache_header */
    int handle;                       /* buffer handle */
}
#ifndef UI_COMMIT
tcramcache
#endif
;

#ifdef UI_COMMIT
/* The builder owns one context. Readers retain the other context across
 * yielding file operations; changing ui_build never changes their snapshot.
 * Contexts are small descriptors; the two RAM payloads use core_alloc. */
struct tagcache_context {
    struct tagcache_stat stat;
    struct master_header master;
    struct tcramcache ram;
    unsigned readers;
    unsigned operations;
    bool reserved;
    volatile int reader_count;
    volatile int writer_count;
    int generation;
    int32_t loaded_commitid;
    long filename_pos;
    long filename_ram_pos;
    int filename_generation;
};
static struct tagcache_context ui_context_a;
static struct tagcache_context ui_context_b;
static struct tagcache_context *ui_build = &ui_context_a;
static struct tagcache_context *ui_active = &ui_context_a;
/* the candidate's files were written (new, deleted or reclassified rows,
 * a rebuild, an import), not only its statistics: an update that found
 * nothing is dropped, never published as an identical library */
static bool ui_candidate_changed;
/* the active snapshot's precompute ran since it became active or loaded */
static bool ui_active_pre_done;
static volatile bool ui_publishing;
static char ui_db_root[MAX_PATHNAME + 1];
#ifdef UI_COLLATE
/* #47: the tag files' order follows the shell's grouping rules
 * (sortkey.h UI_GROUPING_VERSION). A library sorted by older rules, whose
 * files never change, would keep that order: a "re-sort owed" marker
 * (none, or an older version in TAGCACHE_FILE_ORDER) makes the next scan
 * commit once even with nothing new, which re-sorts every tag file
 * (build_index() sorts each whole); the publication writes the marker.
 * No TAGCACHE_MAGIC change, no rebuild. -1 not read yet, 1 owed, 0 not. */
#define TAGCACHE_FILE_ORDER "/database_order.dat"
static int8_t ui_resort = -1;
static bool ui_resort_ran;      /* a commit ran for the candidate */

static bool ui_resort_owed(void)
{
    if (ui_resort < 0)
    {
        char path[MAX_PATH];
        uint32_t v = 0;
        int fd;

        snprintf(path, sizeof(path), "%s" TAGCACHE_FILE_ORDER, ui_db_root);
        fd = open(path, O_RDONLY);
        if (fd >= 0)
        {
            if (read(fd, &v, sizeof(v)) != (ssize_t)sizeof(v))
                v = 0;
            close(fd);
        }
        ui_resort = v != UI_GROUPING_VERSION;
        if (ui_resort)
            ui_log("db", "resort owed v=%lu", (unsigned long)v);
    }
    return ui_resort > 0;
}

/* a publication of a committed candidate: its tag files are in the
 * current order */
static void ui_resort_published(void)
{
    char path[MAX_PATH];
    uint32_t v = UI_GROUPING_VERSION;
    int fd;

    if (!ui_resort_ran)
        return;
    ui_resort_ran = false;
    if (!ui_resort_owed())
        return;
    snprintf(path, sizeof(path), "%s" TAGCACHE_FILE_ORDER, ui_db_root);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0)
    {
        if (write(fd, &v, sizeof(v)) == (ssize_t)sizeof(v))
            ui_resort = 0;
        close(fd);
    }
    ui_log("db", "resort done ok=%d", ui_resort == 0);
}
#endif
static bool ui_candidate_pending;
static bool ui_maintenance_for_snapshot;
static bool ui_candidate_maybe_published;
/* the import's input: 1 the rebuild's own export (MED-A), 2 the owed
 * fresh rebuild's export of the active master (R7b-1b), 3 none (that
 * readable master has no statistics: never an older file), 0 the best */
static int ui_candidate_exported;
static int ui_rebuild_again;    /* Q_REBUILD data to post again, or 0 */
static int ui_rebuild_maint;    /* the last rebuild's maintenance bit */
static int32_t ui_export_bad = -1; /* commit whose export found damage */
static bool ui_scan_pending, ui_deleted_owed;
static bool ui_generation_unresolved;
static bool ui_generation_fresh; /* definitively unusable selected data, keep old files */
static bool ui_boot_stats_replayed;
static bool ui_stats_restart;
static bool ui_stats_recovering; /* producer mutex: RAM-coalesced until checkpoint */
/* The boot replay targets the active snapshot even beside a candidate; the
 * import helpers use this context (NULL: ui_build) */
static struct tagcache_context *ui_import_ctx;
#define UI_IMPORT_CTX (ui_import_ctx ? ui_import_ctx : ui_build)
static unsigned ui_stats_dropped, ui_stats_dropped_logged; /* never slept */
/* the seq a failed append may have written; a checkpoint forgets it (its
 * new sequence can land on the same number: LOW-1) */
static uint64_t ui_append_maybe;
static bool ui_journal_full;    /* an append found it full: compaction owed */
/* Journal work (filename reads, appends, salvage of up to 128 MiB) runs on
 * the tagcache thread only: its stack is sized for it, the storage thread's
 * 2 KiB is not. Other threads (the storage-idle notifier, system_flush())
 * only mark it due; the worker drains within a second (R-H2). */
static unsigned int ui_worker_thread;
static volatile bool ui_drain_due, ui_drain_force;
/* #118: the worker has acked a USB connect and waits for the unplug: it
 * cannot drain until then, so system_flush() must not wait for it */
static volatile bool ui_worker_usb_parked;

static bool ui_on_worker(void)
{
    return thread_self() == ui_worker_thread;
}

/* Clean shutdown posts its final flush only after audio_stop has queued the
 * last track's values. USB system_flush must never wait for this checkpoint. */
static volatile bool ui_shutdown_intent;
static volatile bool ui_shutdown_flush_pending;
static volatile bool ui_shutdown_flush_done;
static bool ui_shutdown_flushing; /* tagcache worker only */
static volatile long ui_shutdown_flush_deadline;
#define UI_STATS_SHUTDOWN_TICKS (3 * HZ)  /* appends only (B-M5) */
static struct dbjournal ui_stats_journal;
static bool ui_candidate_begin(void);
static long ui_changelog_scan(char *name);
static bool ui_candidate_publish(void);
static bool ui_candidate_stats_prepare(void);
static bool ui_candidate_stats_finish(void *buffer, size_t bytes);
static void ui_candidate_stats_release(void);
#ifdef SIMULATOR
static void sim_commit_hold(int point);
#endif
#define tc_stat       (ui_build->stat)
#define current_tcmh  (ui_build->master)
#define tcramcache    (ui_build->ram)
#define TC_CTX_PARAM  struct tagcache_context *ctx,
#define TC_CTX_ARG(c) (c),
#define TC_CTX_ONLY struct tagcache_context *ctx
#define TC_CTX_PASS(c) (c)
#define TC_STAT(c)    ((c)->stat)
#define TC_MASTER(c)  ((c)->master)
#define TC_RAM(c)     ((c)->ram)
#define TC_READ_LOCK(c) ((c)->reader_count)
#define TC_WRITE_LOCK(c) ((c)->writer_count)
static bool ui_context_ref(struct tagcache_context *ctx, bool operation)
{
    int level = disable_irq_save();
    bool ok = !ctx->reserved;
    if (ok)
    {
        ctx->readers++;
        if (operation)
            ctx->operations++;
    }
    restore_irq(level);
    return ok;
}

static void ui_context_put(struct tagcache_context *ctx, bool operation)
{
    int level = disable_irq_save();
    if (ctx->readers)
        ctx->readers--;
    if (operation && ctx->operations)
        ctx->operations--;
    restore_irq(level);
}

static void ui_publish_end(void)
{
    int level = disable_irq_save();
    ui_publishing = false;
    restore_irq(level);
}
#else
#define TC_CTX_PARAM
#define TC_CTX_ARG(c)
#define TC_CTX_ONLY void
#define TC_CTX_PASS(c)
#define TC_STAT(c)    tc_stat
#define TC_MASTER(c)  current_tcmh
#define TC_RAM(c)     tcramcache
#define TC_READ_LOCK(c) read_lock
#define TC_WRITE_LOCK(c) write_lock
#endif

#ifdef HAVE_IPOD_UI
/* see tagcache_ui_generation() / tagcache_ui_busy() */
#ifdef UI_COMMIT
#define ui_generation (ui_build->generation)
#else
static int ui_generation;
#endif
static bool ui_busy = true;     /* until the boot-time check is done */
/* The header's busy glyph (apps/ipodui/usbbusy.c, 3.20): library work
 * running on this thread besides the scan and the commit (a reload after
 * the first load, the lists' precompute, the changelog import). Nesting;
 * this thread writes, the UI thread reads (tagcache_ui_updating()). */
static volatile unsigned char ui_upd_jobs;
static bool ui_upd_loaded;      /* a RAM cache was loaded once: a reload */
#define UI_UPD_RUN(stmt) do { ui_upd_jobs++; stmt; ui_upd_jobs--; } while (0)
#endif
#ifdef UI_COMMIT
/* The library's empty window (8b brief 3.3), stamped where it happens:
 * the post-USB scan's start, the RAM cache dropped (the first drop wins
 * until it is ready again), the RAM cache loaded. The shell logs them on
 * its next pass (tagcache_ui_window()), so a late pass still logs exact
 * times. Tagcache thread writes, UI thread reads; a torn read only skews
 * one log line. */
static long ui_win_scan, ui_win_empty, ui_win_ready;
static void ui_win_drop(void)
{
    /* 8b-T (T6): with generations the lists only go empty when the
     * context being dropped is the published one, or nothing is published
     * yet (the first build); a candidate dropped behind a live library is
     * not an empty window */
    if (ui_build != ui_active && tagcache_ui_count_ctx(ui_active) >= 0)
    {
        if (ui_win_ready)
            ui_win_empty = 0;   /* the last window is closed: none now */
        return;
    }
    if (!ui_win_empty || ui_win_ready)
    {
        ui_win_empty = current_tick;
        ui_win_ready = 0;
    }
}
#endif

static inline void tcrc_buffer_lock(TC_CTX_ONLY)
{
    core_pin(TC_RAM(ctx).handle);
}

static inline void tcrc_buffer_unlock(TC_CTX_ONLY)
{
    core_unpin(TC_RAM(ctx).handle);
}

#else /* ndef HAVE_TC_RAMCACHE */

#define IF_TCRCDC(...)

#endif /* HAVE_TC_RAMCACHE */

#ifndef TC_CTX_PARAM
#define TC_CTX_PARAM
#define TC_CTX_ARG(c)
#define TC_CTX_ONLY void
#define TC_CTX_PASS(c)
#define TC_STAT(c) tc_stat
#define TC_MASTER(c) current_tcmh
#define TC_RAM(c) tcramcache
#define TC_READ_LOCK(c) read_lock
#define TC_WRITE_LOCK(c) write_lock
#endif
#ifdef UI_COMMIT
#define TC_DCFREFS(c) \
    ((struct dircache_fileref *)((c)->ram.hdr->tags[tag_filename] + \
                                  sizeof(struct tagcache_header)))
#else
#define TC_DCFREFS(c) tcrc_dcfrefs
#endif

/**
 * Full tag entries stored in a temporary file waiting
 * for commit to the cache. */
struct temp_file_entry {
    int32_t tag_offset[TAG_COUNT];
    int16_t tag_length[TAG_COUNT];
    int32_t flag;
    int32_t data_length;
};

struct tempbuf_id_list {
    long id;
    struct tempbuf_id_list *next;
};

struct tempbuf_searchidx {
    long idx_id;
    char *str;
    int seek;
    struct tempbuf_id_list idlist;
#ifdef UI_COLLATE
    char *key;      /* ui_sortkey(str), precomputed before the sort */
#endif
};

/* Lookup buffer for fixing messed up index while after sorting. */
static long commit_entry_count;
static long lookup_buffer_depth;
static struct tempbuf_searchidx **lookup;

/* Used when building the temporary file. */
static int cachefd = -1, filenametag_fd;
static int total_entry_count = 0;
static int data_size = 0;
static int processed_dir_count;

/* Thread safe locking */
#ifdef UI_COMMIT
#define write_lock (ui_build->writer_count)
#define read_lock  (ui_build->reader_count)
#else
static volatile int write_lock;
static volatile int read_lock;
#endif

#ifdef UI_COMMIT
/* A commit delayed because playback holds the memory it needs; the
 * tagcache thread retries it once audio stops (SYS_TIMEOUT). */
static bool ui_commit_pending;
/* A RAM cache reload put off because it would take memory from playback
 * (load_ramcache()); retried like a delayed commit. */
static bool ui_reload_pending;
/* Numeric tags changed since the last export (tagcache_update_numeric):
 * + 1 per change; an export saves the value it started with, so a change
 * made while it runs is still owed. ui_stats_tick: the last change. */
static volatile long ui_stats_gen, ui_stats_exported;
static long ui_stats_tick;
/* The database files were removed (remove_files(): a rebuild, the first
 * build): the commit that recreates them imports the changelog. */
static bool ui_db_fresh;
/* The changelog import is queued or was cut off (TAGCACHE_FILE_IMPORT
 * exists): until it has run, an export would replace the old statistics
 * with the new database's few. */
static bool ui_import_pending;
/* Entries in the RAM cache at its last load (library view pool growth) */
static long ui_loaded_entries;
/* #88: the commit's peak beyond the free heap at its last wait (0: none
 * waits), so the once-a-second retry checks the paused window and the
 * floor with no disk access */
static size_t ui_maint_need;
/* P16: the candidate that gave its RAM cache back while audio ran
 * (ui_candidate_ram_yield()); it loads again at the stop or in a paused
 * window (ui_yield_reload_due()). ui_yield_bytes: that reload's size,
 * from its header in RAM at the yield (no disk access to decide whether
 * it fits); a commit into the candidate ends the yield (the commit's
 * own reload follows). ui_yields: yields since its last publication;
 * at UI_YIELD_MAX the reload stops retrying until a new request, USB
 * or the next boot (the update stays owed). */
static struct tagcache_context *ui_ram_yielded;
static size_t ui_yield_bytes;
static unsigned ui_yields;
#define UI_YIELD_MAX 4
/* ui_candidate_publish() refused only because a reader holds the active
 * library (retried within a second: no yield for that) */
static bool ui_publish_busy;
/* beside the commit's own peak, a window's take covers two generation
 * buffers (UI_GENERATION_BYTES) and a margin for small allocations */
#define UI_MAINT_SLACK (2 * 16 * 1024 + 64 * 1024)

/* the delayed commit may run although audio is on (paused) */
static bool ui_maint_due(void)
{
    /* P16: memory a take already freed counts once (maint_take_fits()),
     * and a take refused this pause is not retried (no commit attempt,
     * no disk access, until the pause ends) */
    return ui_maint_need && maint_window_open()
           && !maint_declined(MAINT_COMMIT) && maint_take_fits(ui_maint_need);
}

/* The commit job's peak: its last known one, else a guess from the RAM
 * cache (the candidate's reload is about its size, plus the view pool
 * and the statistics lookup, ~29 B an entry; P16: a yielded candidate's
 * reload size stands in for its freed cache) */
static size_t ui_peak_estimate(void)
{
    long ram = tc_stat.ramcache_allocated;

    if (ui_maint_need)
        return ui_maint_need;
    if (ram <= 0 && ui_ram_yielded == ui_build)
        ram = (long)ui_yield_bytes;
    return (size_t)MAX(ram, 0) * 9 / 8
           + (size_t)current_tcmh.tch.entry_count * 16 + 64 * 1024
           + UI_MAINT_SLACK;
}

/* Audio on: may 'bytes' be allocated without shrinking the audio buffer
 * now? In a paused window (#88) the first step that lacks room takes the
 * whole job's peak (the commit's last known one, else these bytes and a
 * margin) by one partial shrink, so the steps after it find free memory
 * (one silent restart per window, not one per step). Tagcache thread. */
static bool ui_audio_room(size_t bytes)
{
    if (audio_status() == 0 || core_allocatable() >= bytes + 64)
        return true;
    /* the job's first take covers its peak (ui_peak_estimate()); a
     * later take, this step only */
    size_t peak = ui_peak_estimate();

    return maint_take(MAINT_COMMIT, maint_holds(MAINT_COMMIT)
                      ? bytes + UI_MAINT_SLACK
                      : MAX(bytes + UI_MAINT_SLACK, peak));
}

#ifdef HAVE_IPOD_UI
/* P18: ui_audio_room()'s answer without taking anything (any thread; a
 * take refused this pause stays refused) */
static bool ui_audio_room_fits(size_t bytes)
{
    if (audio_status() == 0 || core_allocatable() >= bytes + 64)
        return true;
    return !maint_declined(MAINT_COMMIT)
           && maint_take_fits(maint_holds(MAINT_COMMIT)
                              ? bytes + UI_MAINT_SLACK
                              : MAX(bytes + UI_MAINT_SLACK,
                                    ui_peak_estimate()));
}
#endif
/* What a pending reload needs (ui_reload_fits()) */
static size_t ui_reload_bytes;
/* The shell's library precompute (library.c lib_precompute(), file
 * LIBFILE_NAME): due after every RAM cache load and every deleted-file
 * pass that deleted something ('pending'); asked for by the shell when it
 * found the file damaged ('request': rewritten even if its stamp
 * matches). Run on this thread, in chunks, by ui_precompute_run(). */
static bool ui_pre_pending;
static volatile bool ui_pre_request;
/* LIBFILE_NAME was removed by this pass's first deletion */
static bool ui_libfile_dropped;
/* The commit id of the database in RAM, as loaded (the changelog import
 * raises current_tcmh.commitid afterwards; the precompute's stamp must
 * not change with it) */
#define ui_loaded_commitid (ui_build->loaded_commitid)
static void ui_precompute_run(void);
static void ui_export_recover(char *name);
#endif
#if defined(HAVE_IPOD_UI) && defined(HAVE_TC_RAMCACHE)
/* delete_entry() flagged entries in the loaded RAM cache: open views are
 * stale. Told once, at the end of a pass, not once per entry (each new
 * generation makes the shell reopen its open lists). */
static bool ui_deleted;

static void ui_flush_deleted(void)
{
    if (ui_deleted)
    {
        ui_deleted = false;
        ui_generation++;
#ifdef UI_COMMIT
        ui_pre_pending = true;  /* the lists lost entries */
#endif
    }
}
#endif

static bool delete_entry(long idx_id);
#ifdef UI_COMMIT
static bool check_event_queue(void);
static bool ui_commit_usb_queued(void);
#endif
/* a power-off, USB or a stop request: the queue's head event, or a
 * power-off already under way (USR_CANCEL, set by
 * tagcache_prepare_shutdown(); another event may be ahead of its
 * Q_STOP_SCAN in the queue) */
#ifdef UI_COMMIT
#define STOP_REQUESTED() (ui_commit_cancel || check_event_queue())
#else
#define STOP_REQUESTED() check_event_queue()
#endif

static inline void str_setlen(char *buf, size_t len)
{
    buf[len] = '\0';
}

const char* tagcache_tag_to_str(int tag)
{
    return tags_str[tag];
}

#ifdef TAGCACHE_SUPPORT_FOREIGN_ENDIAN
static void swap_tagfile_entry(TC_CTX_PARAM struct tagfile_entry *buf)
{
    if (TC_STAT(ctx).econ)
    {
        buf->tag_length = swap32(buf->tag_length);
        buf->idx_id = swap32(buf->idx_id);
    }
}

static void swap_index_entry(TC_CTX_PARAM struct index_entry *buf)
{
    if (TC_STAT(ctx).econ)
    {
        for (int i = 0; i < TAG_COUNT; ++i)
            buf->tag_seek[i] = swap32(buf->tag_seek[i]);
        buf->flag = swap32(buf->flag);
    }
}

static void swap_tagcache_header(TC_CTX_PARAM struct tagcache_header *buf)
{
    if (TC_STAT(ctx).econ)
    {
        buf->magic = swap32(buf->magic);
        buf->datasize = swap32(buf->datasize);
        buf->entry_count = swap32(buf->entry_count);
    }
}

static void swap_master_header(TC_CTX_PARAM struct master_header *buf)
{
    if (TC_STAT(ctx).econ)
    {
        swap_tagcache_header(TC_CTX_ARG(ctx) &buf->tch);
        buf->serial = swap32(buf->serial);
        buf->commitid = swap32(buf->commitid);
        buf->dirty = swap32(buf->dirty);
    }
}
#else
static void swap_tagfile_entry(TC_CTX_PARAM struct tagfile_entry *buf) { (void)buf; }
static void swap_index_entry(TC_CTX_PARAM struct index_entry *buf) { (void)buf; }
static void swap_tagcache_header(TC_CTX_PARAM struct tagcache_header *buf) { (void)buf; }
static void swap_master_header(TC_CTX_PARAM struct master_header *buf) { (void)buf; }
#endif

static ssize_t read_tagfile_entry(TC_CTX_PARAM int fd, struct tagfile_entry *buf)
{
    ssize_t ret = read(fd, buf, sizeof(*buf));
    if (ret == sizeof(*buf) && TC_STAT(ctx).econ)
        swap_tagfile_entry(TC_CTX_ARG(ctx) buf);

    return ret;
}

static ssize_t write_tagfile_entry(TC_CTX_PARAM int fd, struct tagfile_entry *buf)
{
    struct tagfile_entry e = *buf;

    swap_tagfile_entry(TC_CTX_ARG(ctx) &e);

    return write(fd, &e, sizeof(e));
}

enum e_read_errors {
    e_SUCCESS = 0,
    e_SUCCESS_LEN_ZERO = 1,
    e_ENTRY_SIZEMISMATCH,
    e_TAG_TOOLONG,
    e_TAG_SIZEMISMATCH
};

static enum e_read_errors
read_tagfile_entry_and_tag(TC_CTX_PARAM int fd, struct tagfile_entry *tfe,
                           char* buf, int bufsz)
{
    if (read_tagfile_entry(TC_CTX_ARG(ctx) fd, tfe) != sizeof(struct tagfile_entry))
        return e_ENTRY_SIZEMISMATCH;

    long tag_length = tfe->tag_length;
    if (tag_length >= bufsz)
        return e_TAG_TOOLONG;

    if (tag_length > 0 && read(fd, buf, tag_length) != tag_length)
        return e_TAG_SIZEMISMATCH;

    str_setlen(buf, tag_length);
    return (tag_length > 0 && *buf) ? e_SUCCESS : e_SUCCESS_LEN_ZERO;
}

static ssize_t read_index_entries(TC_CTX_PARAM int fd, struct index_entry *buf, size_t count)
{
    ssize_t ret = read(fd, buf, sizeof(*buf) * count);
    for (ssize_t i = 0; i < ret; i += sizeof(*buf))
        swap_index_entry(TC_CTX_ARG(ctx) buf++);

    return ret;
}

static ssize_t write_index_entries(TC_CTX_PARAM int fd, struct index_entry *buf, size_t count)
{
#ifdef UI_COMMIT
    if (ctx != ui_active)
        ui_candidate_changed = true;
#endif
#ifdef TAGCACHE_SUPPORT_FOREIGN_ENDIAN
    ssize_t ret = 0;
    for (; count > 0; count--)
    {
        struct index_entry e = *buf++;
        swap_index_entry(TC_CTX_ARG(ctx) &e);

        ssize_t rc = write(fd, &e, sizeof(e));
        if (rc < 0)
            return rc;
        ret += rc;
    }

    return ret;
#else
    return write(fd, buf, sizeof(*buf) * count);
#endif
}

static ssize_t read_tagcache_header(TC_CTX_PARAM int fd, struct tagcache_header *buf)
{
    ssize_t ret = read(fd, buf, sizeof(*buf));
    if (ret == sizeof(*buf))
        swap_tagcache_header(TC_CTX_ARG(ctx) buf);

    return ret;
}

static ssize_t write_tagcache_header(TC_CTX_PARAM int fd, struct tagcache_header *buf)
{
    struct tagcache_header e = *buf;
    swap_tagcache_header(TC_CTX_ARG(ctx) &e);
    return write(fd, &e, sizeof(e));
}

static ssize_t read_master_header(TC_CTX_PARAM int fd, struct master_header *buf)
{
    ssize_t ret = read(fd, buf, sizeof(*buf));
    if (ret == sizeof(*buf))
        swap_master_header(TC_CTX_ARG(ctx) buf);

    return ret;
}

static ssize_t write_master_header(TC_CTX_PARAM int fd, struct master_header *buf)
{
    struct master_header e = *buf;
    swap_master_header(TC_CTX_ARG(ctx) &e);
    return write(fd, &e, sizeof(e));
}

/*
 * open_db_fd and remove_db_file are noinline to minimize stack usage
 */
static int NO_INLINE open_db_fd(TC_CTX_PARAM const char* filename, int mode)
{
    char buf[MAX_PATH];

    if(mode & O_CREAT)
    {
        if (mkdir(TC_STAT(ctx).db_path) < 0 && errno != EEXIST)
            return -1;
    }

    return open_pathfmt(buf, sizeof(buf), mode, "%s/%s",
                        TC_STAT(ctx).db_path, filename);
}

static int NO_INLINE remove_db_file(TC_CTX_PARAM const char* filename)
{
    char buf[MAX_PATH];

    snprintf(buf, sizeof(buf), "%s/%s",
             TC_STAT(ctx).db_path, filename);

    return remove(buf);
}

static int open_tag_fd(TC_CTX_PARAM struct tagcache_header *hdr, int tag, bool write)
{
    int fd;
    char fname[MAX_PATH];

    if (TAGCACHE_IS_NUMERIC(tag) || tag < 0 || tag >= TAG_COUNT)
        return -1;

    fd = open_pathfmt(fname, sizeof(fname),
                      write ? O_RDWR : O_RDONLY, "%s/" TAGCACHE_FILE_INDEX,
                      TC_STAT(ctx).db_path, tag);
    if (fd < 0)
    {
        logf("%s failed: tag=%d write=%d file= " TAGCACHE_FILE_INDEX,
             __func__, tag, write, tag);
#ifndef UI_COMMIT
        TC_STAT(ctx).ready = false;
#endif
        return fd;
    }

    /* Check the header. */
    if (read_tagcache_header(TC_CTX_ARG(ctx) fd, hdr) != sizeof(struct tagcache_header) ||
        hdr->magic != TAGCACHE_MAGIC
#ifdef UI_COMMIT
        || hdr->datasize < 0 || hdr->entry_count < 0
        || ffilesize(fd) < (off_t)sizeof(*hdr) + hdr->datasize
#endif
       )
    {
        logf("header error");
#ifndef UI_COMMIT
        TC_STAT(ctx).ready = false;
#endif
        close(fd);
        return -2;
    }

    return fd;
}

static int open_master_fd(TC_CTX_PARAM struct master_header *hdr, bool write)
{
    int fd;
    int rc;

#ifdef UI_TIMING
    ui_master_opens++;
#endif
    fd = open_db_fd(TC_CTX_ARG(ctx) TAGCACHE_FILE_MASTER, write ? O_RDWR : O_RDONLY);
    if (fd < 0)
    {
        logf("master file open failed for R/W");
#ifndef UI_COMMIT
        TC_STAT(ctx).ready = false;
#endif
        return fd;
    }

    rc = read(fd, hdr, sizeof(struct master_header));
    if (rc != sizeof(struct master_header))
    {
        logf("master file read failed");
        close(fd);
#ifdef UI_COMMIT
        if (rc >= 0)
            return -2;          /* short: a damaged file, not I/O (errno) */
#endif
        return -1;
    }

    /* Tagcache files can have either endianness. A device will always
     * create files in its native endianness, but we accept non-native
     * endian files for compatibility reasons. */
    if (hdr->tch.magic == TAGCACHE_MAGIC)
        TC_STAT(ctx).econ = false;
#ifdef TAGCACHE_SUPPORT_FOREIGN_ENDIAN
    else if (hdr->tch.magic == swap32(TAGCACHE_MAGIC))
    {
        TC_STAT(ctx).econ = true;
        swap_master_header(TC_CTX_ARG(ctx) hdr);
    }
#endif
    else
    {
        logf("master file bad magic: %08lx\n", (unsigned long)hdr->tch.magic);
        close(fd);
        return -2;
    }

    return fd;
}

static void remove_files(void)
{
    int i;
    char buf[MAX_PATH];
    const int bufsz = sizeof(buf);
    logf("%s", __func__);

    tc_stat.ready = false;
    tc_stat.ramcache = false;
    tc_stat.econ = false;
#ifdef UI_COMMIT
    ui_db_fresh = true;
    ui_candidate_changed = true;    /* a rebuild always publishes */
    /* The new database imports the changelog. Marked now, not after its
     * commit: a power cut between the commit's clean header and the
     * marker would leave a clean database without the statistics, and
     * its next export would overwrite the changelog with its few. */
    if (!ui_import_pending)
    {
        int fd = open_db_fd(TC_CTX_ARG(ui_build) TAGCACHE_FILE_IMPORT,
                            O_WRONLY | O_CREAT | O_TRUNC);
        if (fd >= 0)
            close(fd);
        ui_import_pending = true;
    }
#endif
    remove_db_file(TC_CTX_ARG(ui_build) TAGCACHE_FILE_MASTER);
#ifdef UI_COMMIT
    /* the shell's precomputed lists belong to the old database */
    remove_db_file(TC_CTX_ARG(ui_build) LIBFILE_NAME);
    remove_db_file(TC_CTX_ARG(ui_build) LIBFILE_TEMP);
#endif
    for (i = 0; i < TAG_COUNT; i++)
    {
        if (TAGCACHE_IS_NUMERIC(i))
            continue;

        snprintf(buf, bufsz, "%s/" TAGCACHE_FILE_INDEX,
                 tc_stat.db_path, i);
        remove(buf);
    }
}

static bool check_all_headers(TC_CTX_ONLY)
{
    struct master_header myhdr;
    struct tagcache_header tch;
    int tag;
    int fd;

    if ( (fd = open_master_fd(TC_CTX_ARG(ctx) &myhdr, false)) < 0)
        return false;

    close(fd);
    if (myhdr.dirty)
    {
        logf("tagcache is dirty!");
        return false;
    }

    memcpy(&TC_MASTER(ctx), &myhdr, sizeof(struct master_header));

    for (tag = 0; tag < TAG_COUNT; tag++)
    {
        if (TAGCACHE_IS_NUMERIC(tag))
            continue;

        if ( (fd = open_tag_fd(TC_CTX_ARG(ctx) &tch, tag, false)) < 0)
            return false;

        close(fd);
    }

    return true;
}

static bool update_master_header(TC_CTX_ONLY)
{
    struct master_header myhdr;
    int fd;

    if (!TC_STAT(ctx).ready)
        return false;

    if ( (fd = open_master_fd(TC_CTX_ARG(ctx) &myhdr, true)) < 0)
        return false;

    myhdr.serial = TC_MASTER(ctx).serial;
    myhdr.commitid = TC_MASTER(ctx).commitid;
    myhdr.dirty = TC_MASTER(ctx).dirty;

    /* Write it back */
    lseek(fd, 0, SEEK_SET);
    write_master_header(TC_CTX_ARG(ctx) fd, &myhdr);
    close(fd);

    return true;
}

#if !defined(PLUGIN)
#ifndef __PCTOOL__
static bool do_timed_yield(void)
{
    /* Sorting can lock up for quite a while, so yield occasionally */
    static long wakeup_tick = 0;
    if (TIME_AFTER(current_tick, wakeup_tick))
    {
        yield();
        wakeup_tick = current_tick + (HZ/25);
        return true;
    }
    return false;
}
#endif /* __PCTOOL__ */

static void allocate_tempbuf(void)
{
    /* Yeah, malloc would be really nice now :) */
    size_t size;
    tempbuf_size = 0;

#ifdef __PCTOOL__
    size = 32*1024*1024;
    tempbuf = malloc(size);
    if (tempbuf)
        tempbuf_size = size;
#else /* !__PCTOOL__ */
    /* Need to pass dummy ops to prevent the buffer being moved
     * out from under us, since we yield during the tagcache commit. */
    tempbuf_handle = core_alloc_maximum(&size, &buflib_ops_locked);
    if (tempbuf_handle > 0)
    {
        tempbuf = core_get_data(tempbuf_handle);
        tempbuf_size = size;
    }
#endif /* __PCTOOL__ */

}

static void free_tempbuf(void)
{
    if (tempbuf_size == 0)
        return ;

#ifdef __PCTOOL__
    free(tempbuf);
#else
    tempbuf_handle = core_free(tempbuf_handle);
#endif
    tempbuf = NULL;
    tempbuf_size = 0;
}

#if defined(HAVE_TC_RAMCACHE) && defined(HAVE_DIRCACHE)
/* find the ramcache entry corresponding to the file indicated by
 * filename and dc (it's corresponding dircache id). */
static long find_entry_ram(TC_CTX_PARAM const char *filename)
{
#ifdef UI_COMMIT
#define last_pos (ctx->filename_ram_pos)
#else
    static long last_pos = 0;
#endif
    struct dircache_fileref dcfref;

    /* Check if tagcache is loaded into ram. */
    if (!TC_STAT(ctx).ramcache
        || global_settings.tagcache_ram != TAGCACHE_RAM_ON)
        return -1;

    if (dircache_search(DCS_CACHED_PATH | DCS_UPDATE_FILEREF, &dcfref,
                        filename) <= 0)
    {
        logf("tagcache: file not found.");
        return -1;
    }

    /* Search references */
    int end_pos = TC_MASTER(ctx).tch.entry_count;
    while (1)
    {
        for (int i = last_pos; i < end_pos; i++)
        {
            do_timed_yield();

            if (!(TC_RAM(ctx).hdr->indices[i].flag & FLAG_DIRCACHE))
                continue;

            int cmp = dircache_fileref_cmp(&TC_DCFREFS(ctx)[i], &dcfref);
            if (cmp < 3)
                continue;

            last_pos = MAX(0, i - 3);
            return i;
        }

        if (last_pos == 0)
        {
            last_pos = MAX(0, end_pos - 3);
            break;
        }

        end_pos = last_pos;
        last_pos = 0;
    }

    return -1;
#ifdef UI_COMMIT
#undef last_pos
#endif
}
#endif /* defined (HAVE_TC_RAMCACHE) && defined (HAVE_DIRCACHE) */

#ifdef UI_TIMING
static long ui_scan_misses;     /* scan lookups that read the whole file */
#endif

static long find_entry_disk(TC_CTX_PARAM const char *filename_raw, bool localfd)
{
    struct tagfile_entry tfe;
    struct tagcache_header tch;
#ifdef UI_COMMIT
#define last_pos (ctx->filename_pos)
#define last_gen (ctx->filename_generation)
#else
    static long last_pos = -1;
#endif
    long pos_history[POS_HISTORY_COUNT];
    unsigned int pos_history_idx = 0;
    unsigned int i;

    char buf[TAGCACHE_BUFSZ];
    const long bufsz = sizeof(buf);

    int fd;
    int pos = -1;
    long idx = -1;

    bool found = false;

    const char *filename = filename_raw;

#ifdef APPLICATION
    char pathbuf[PATH_MAX]; /* Note: Don't use MAX_PATH here, it's too small */
    if (realpath(filename, pathbuf) == pathbuf)
        filename = pathbuf;
#endif /* APPLICATION */

    if (!TC_STAT(ctx).ready)
        return -2;

    fd = filenametag_fd;
    if (fd < 0 || localfd)
    {
#ifdef UI_COMMIT
        /* S36 (C1): the position is kept across lookups in one database
         * generation (a commit rewrites the file): an album's tracks are
         * neighbours there, so the next track is a sector or two on, not
         * a read from the start. A stale one only costs the restart
         * below. */
        if (last_gen != ctx->generation)
#endif
        last_pos = -1;
        if ( (fd = open_tag_fd(TC_CTX_ARG(ctx) &tch, tag_filename, false)) < 0)
            return -1;
    }
#ifdef UI_COMMIT
    last_gen = ctx->generation;
#endif
#ifdef HAVE_IPOD_UI
    /* a name that is not there reads the whole file; the next lookup
     * starts where this one did, not from the start (scan speed, 1019) */
    const long start_pos = last_pos;
#endif

    check_again:

    if (last_pos > 0) /* pos gets cached to prevent reading from beginning */
        pos = lseek(fd, last_pos, SEEK_SET);
    else /* start back at beginning */
        pos = lseek(fd, sizeof(struct tagcache_header), SEEK_SET);

    long tag_length = strlen(filename) + 1; /* include NULL */

    if (tag_length < bufsz)
    {
        while (true)
        {
            for (i = pos_history_idx-1; i < pos_history_idx; i--)
                pos_history[i+1] = pos_history[i];
            pos_history[0] = pos;
#ifdef UI_TIMING
            if (!localfd)
                ui_scan_reads++;
#endif

            if (read_tagfile_entry(TC_CTX_ARG(ctx) fd, &tfe) != sizeof(struct tagfile_entry))
            {
                logf("size mismatch find entry");
                break;
            }
            else
            {
                pos += sizeof(struct tagfile_entry) + tfe.tag_length;
                /* don't read the entry unless the length matches */
                if (tfe.tag_length == tag_length)
                {
                    if(read(fd, buf, tfe.tag_length) != tag_length)
                    {
                        logf("read error #2");
                        close(fd);
                        if (!localfd)
                            filenametag_fd = -1;
                        last_pos = -1;
                        return -3;
                    }
                    if (!strncmp(filename, buf, tag_length))
                    {
                        last_pos = pos_history[pos_history_idx];
                        found = true;
                        idx = tfe.idx_id;
                        break ;
                    }
                }
                else
                    lseek(fd, pos, SEEK_SET);

            }
        }
        if (pos_history_idx < POS_HISTORY_COUNT - 1)
            pos_history_idx++;
    }

    /* Not found? */
    if (!found)
    {
        if (last_pos > 0) /* start back at the beginning */
        {
            last_pos = -1;
            logf("seek again");
            goto check_again;
        }

        idx = -4;
#ifdef HAVE_IPOD_UI
        last_pos = start_pos;
#endif
#ifdef UI_TIMING
        if (!localfd)
            ui_scan_misses++;
#endif
    }

    if (fd != filenametag_fd || localfd)
        close(fd);

    return idx;
#ifdef UI_COMMIT
#undef last_pos
#undef last_gen
#endif
}

#ifdef UI_COMMIT
/* Two 32-bit hashes of a filename and its index: the import's lookup
 * table (below) and the playback memo (next) */
struct ui_name_key {
    uint32_t h1, h2;
    int32_t idx_id;
};
/* A sorted table of them (ui_import_table()), movable */
struct ui_import {
    long masterfd;
    int handle;                 /* the table (movable); 0: none */
    long count;
};
/* The table, fetched again after anything that may yield (it moves) */
#define UI_KEYS(imp) ((struct ui_name_key *)core_get_data((imp)->handle))
/* The scan's (scan speed, 1019): its filename table (h2 holds the tag
 * file offset) and one master fd (the candidate has no RAM cache) */
static struct ui_import ui_scan = { .masterfd = -1 };
/* The read-only pass (lazy clone, 1019): add_tagcache() and
 * check_file_refs() look at the active library, write nothing and stop at
 * the first change (ui_probe_found) */
static bool ui_probe, ui_probe_found;
/* a change found that the candidate has not yet scanned: no probe again */
static bool ui_probe_owed;
/* the last probe hit every live entry: no file can be missing */
static bool ui_probe_clean;
static long ui_probe_hits;
/* #89 (docs/phase16-brief.md 3.2, 4.2): the no-change boot. The walk
 * digests the tree it reads (boot_digest, the hidden folder left out);
 * a walk that read all of it vouches for the library until the tree
 * changes. ui_walk_ok: the digest of the last completed walk (or of the
 * stamp a skipped boot trusted) describes the library. USB only sets a
 * RAM flag (no disk write at USB); a walk that starts after it and
 * completes clears it. */
static struct boot_digest ui_walk_dg;   /* the walk in progress */
static int ui_walk_mute;                /* inside the hidden folder */
static bool ui_walk_ok;
/* the last walk read the whole tree (no folder failed to open, M2); it
 * vouches (ui_walk_ok) only once no file can be missing either: its probe
 * hit every live entry, or the deleted-file pass finished after it (H2) */
static bool ui_walk_read, ui_walk_broken;
static long ui_walk_metafail;           /* files it could not parse (L1) */
static bool ui_boot_deleted_due;        /* H2: a boot walk's deleted pass */
static bool ui_stamp_written;           /* M1: a shutdown wrote the stamp */
/* npfix: a completed walk's vouch (or a trusted boot) the stamp does not
 * carry yet: written once the library settles (ui_boot_stamp_settle()) */
static bool ui_stamp_due;
/* npfix review L4: the stamp's writers take turns: the UI thread's write
 * at a power-off or a sleep entry (ui_boot_stamp_clean()) and this
 * thread's settle write and removals */
static struct mutex ui_stamp_mtx SHAREDBSS_ATTR;
static uint32_t ui_walk_digest;
static unsigned ui_walk_skipped;        /* boots skipped since that walk */
/* P18 (Q11): the RTC of the walk ui_walk_ok rests on (0: none), and a
 * check owed to the next sleep-entry window, cleared by the vouch */
static uint32_t ui_walk_rtc;
static bool ui_sleep_walk;
static volatile unsigned ui_usb_seq;    /* USB sessions this boot */
static unsigned ui_walk_usb_seq;
static volatile bool ui_usb_since_walk;
static bool ui_boot_decided;            /* the boot check, once */
static long ui_boot_entries = -1;       /* the digest's count: progress */
static volatile signed char ui_boot_state; /* 0 pending, 1 walk, 2 done */
#ifdef HAVE_IPOD_UI_DEVMODE
#define UI_BS_LOG(...) ui_log("boot", __VA_ARGS__)
#else
#define UI_BS_LOG(fmt, ...) DEBUGF("ipodui: boot " fmt "\n", __VA_ARGS__)
#endif
static void ui_boot_walk_forced(void);
/* the last walk may vouch for the library now (H2) */
static void ui_walk_vouch(void)
{
    if (!ui_walk_read)
        return;
    ui_walk_read = false;
    ui_walk_ok = true;
    ui_stamp_due = true;                /* npfix: stamped once settled */
    ui_walk_skipped = 0;
    ui_walk_rtc = ui_sleep_rtc_now();
    if (!ui_walk_rtc)
        ui_walk_rtc = 1;                /* an unset clock: still a walk */
    ui_sleep_walk = false;
    if (ui_usb_seq == ui_walk_usb_seq)
        ui_usb_since_walk = false;
}
static long ui_scan_find(const char *path, struct index_entry *idx,
                         char *buf);

static void ui_name_hash(const char *name, struct ui_name_key *k)
{
    size_t len = strlen(name);
    uint32_t h = 2166136261u;   /* FNV-1a */

    k->h1 = crc_32(name, len, 0xffffffff);
    while (len--)
        h = (h ^ (unsigned char)*name++) * 16777619u;
    k->h2 = h;
}

/* S36 (C1): the filename -> index lookups of the tracks playback loaded.
 * The shell has no dircache, so find_index(TC_CTX_ARG(ui_build) ) reads the filename tag file
 * from its start in sector-sized steps (~150 single-sector reads at 718
 * tracks), and the runtime statistics did that on the audio thread at
 * every track change (tagtree_buffer_event()), long after the track was
 * buffered: a disk wake a track, twice with the next load. playback.c now
 * asks at load time (tagcache_ui_prefetch_index()), while the disk spins
 * for that track's file anyway, and the event finds the answer here. One
 * slot per buffered track ahead is enough; 4 x 16 B. Valid for one
 * database generation (a commit renumbers). */
#define UI_FIDX_SLOTS 4
static struct {
    struct ui_name_key k;
    struct tagcache_context *context;
    int gen1;                   /* ui_generation + 1; 0: empty */
} ui_fidx[UI_FIDX_SLOTS];
static unsigned int ui_fidx_next;

static long ui_fidx_get(struct tagcache_context *ctx, const struct ui_name_key *k)
{
    for (int i = 0; i < UI_FIDX_SLOTS; i++)
        if (ui_fidx[i].context == ctx && ui_fidx[i].gen1 == ctx->generation + 1
            && ui_fidx[i].k.h1 == k->h1 && ui_fidx[i].k.h2 == k->h2)
            return ui_fidx[i].k.idx_id;
    return -1;
}

static void ui_fidx_put(struct tagcache_context *ctx,
                        const struct ui_name_key *k, long idx_id, int gen)
{
    unsigned int i = ui_fidx_next++ % UI_FIDX_SLOTS;

    ui_fidx[i].k = *k;
    ui_fidx[i].context = ctx;
    ui_fidx[i].k.idx_id = idx_id;
    ui_fidx[i].gen1 = gen + 1;
}
#endif /* UI_COMMIT */

static int find_index(TC_CTX_PARAM const char *filename)
{
    long idx_id = -1;
#ifdef UI_COMMIT
    struct ui_name_key key;
    int gen = ctx->generation;

    ui_name_hash(filename, &key);
    idx_id = ui_fidx_get(ctx, &key);
    if (idx_id >= 0)
        return idx_id;
#endif

#if defined(HAVE_TC_RAMCACHE) && defined(HAVE_DIRCACHE)
    idx_id = find_entry_ram(TC_CTX_ARG(ctx) filename);
#endif

    if (idx_id < 0)
        idx_id = find_entry_disk(TC_CTX_ARG(ctx) filename, true);
#ifdef UI_COMMIT
    /* the scan yields: keep it only if the database did not change */
    if (idx_id >= 0 && gen == ctx->generation)
        ui_fidx_put(ctx, &key, idx_id, gen);
#endif

    return idx_id;
}

#ifdef UI_COMMIT
/* playback.c, audio thread, when a track's metadata has just been read
 * (the disk is awake): the lookup its statistics event will need */
static struct tagcache_context *ui_context_try_acquire(void);
void tagcache_ui_prefetch_index(const char *filename)
{
    /* audio thread: skip (the buffer event looks up) rather than wait */
    struct tagcache_context *ctx = ui_context_try_acquire();
    if (!ctx)
        return;
    if (TC_STAT(ctx).ready && TC_STAT(ctx).commit_step == 0 && !TC_READ_LOCK(ctx)
        && filename && *filename)
        find_index(TC_CTX_ARG(ctx) filename);
    tagcache_context_release(ctx);
}
#endif

#ifdef UI_COMMIT
static bool search_context(TC_CTX_PARAM struct tagcache_search *tcs, int tag);
#endif

bool tagcache_find_index(struct tagcache_search *tcs, const char *filename)
{
#ifdef UI_COMMIT
    struct tagcache_context *ctx = ui_active;
#endif
    /* NOTE: on ret==true you need to call tagcache_search_finish(tcs) yourself */
    int idx_id;

    if (!TC_STAT(ctx).ready)
        return false;

#ifdef UI_COMMIT
    if (!search_context(TC_CTX_ARG(ctx) tcs, tag_filename))
        return false;

    idx_id = find_index(TC_CTX_ARG(ctx) filename);
    if (idx_id < 0)
    {
        tagcache_search_finish(tcs);
        return false;
    }
#else
    idx_id = find_index(filename);
    if (idx_id < 0)
        return false;

    if (!tagcache_search(tcs, tag_filename))
        return false;
#endif

    tcs->entry_count = 0;
    tcs->idx_id = idx_id;

    return true;
}

static bool get_index(TC_CTX_PARAM int masterfd, int idxid,
                      struct index_entry *idx, bool use_ram)
{
    bool localfd = false;

    if (idxid < 0
#ifdef UI_COMMIT
        || (use_ram && TC_STAT(ctx).ramcache
            && idxid >= TC_MASTER(ctx).tch.entry_count)
#endif
       )
    {
        logf("Incorrect idxid: %d", idxid);
        return false;
    }

#ifdef HAVE_TC_RAMCACHE
    if (TC_STAT(ctx).ramcache && use_ram)
    {
        if (TC_RAM(ctx).hdr->indices[idxid].flag & FLAG_DELETED)
            return false;

        *idx = TC_RAM(ctx).hdr->indices[idxid];
        return true;
    }
#endif /* HAVE_TC_RAMCACHE */

    if (masterfd < 0)
    {
        struct master_header tcmh;

        localfd = true;
        masterfd = open_master_fd(TC_CTX_ARG(ctx) &tcmh, false);
        if (masterfd < 0)
            return false;
    }

    lseek(masterfd, idxid * sizeof(struct index_entry)
          + sizeof(struct master_header), SEEK_SET);
    if (read_index_entries(TC_CTX_ARG(ctx) masterfd, idx, 1) != sizeof(struct index_entry))
    {
        logf("read error #3");
        if (localfd)
            close(masterfd);

        return false;
    }

    if (localfd)
        close(masterfd);

    if (idx->flag & FLAG_DELETED)
        return false;

    return true;

    (void)use_ram;
}

#ifndef __PCTOOL__

static bool write_index(TC_CTX_PARAM int masterfd, int idxid, struct index_entry *idx)
{
    /* We need to exclude all memory only flags & tags when writing to disk. */
    if (idx->flag & FLAG_DIRCACHE)
    {
        logf("memory only flags!");
        return false;
    }

#ifdef HAVE_TC_RAMCACHE
    /* Only update numeric data. Writing the whole index to RAM by memcpy
     * destroys dircache pointers!
     */
    if (TC_STAT(ctx).ramcache)
    {
        struct index_entry *idx_ram = &TC_RAM(ctx).hdr->indices[idxid];

        for (int tag = 0; tag < TAG_COUNT; tag++)
        {
            if (TAGCACHE_IS_NUMERIC(tag))
            {
                idx_ram->tag_seek[tag] = idx->tag_seek[tag];
            }
        }

        /* Don't touch the dircache flag or attributes. */
        idx_ram->flag = (idx->flag & 0x0000ffff)
            | (idx_ram->flag & (0xffff0000 | FLAG_DIRCACHE));
    }
#endif /* HAVE_TC_RAMCACHE */

    lseek(masterfd, idxid * sizeof(struct index_entry)
          + sizeof(struct master_header), SEEK_SET);
    if (write_index_entries(TC_CTX_ARG(ctx) masterfd, idx, 1) != sizeof(struct index_entry))
    {
        logf("write error #3");
        logf("idxid: %d", idxid);
        return false;
    }

    return true;
}

#endif /* !__PCTOOL__ */

static bool open_files(TC_CTX_PARAM struct tagcache_search *tcs, int tag)
{
    if (tcs->idxfd[tag] < 0)
    {
        char fname[MAX_PATH];
        tcs->idxfd[tag] = open_pathfmt(fname, sizeof(fname),
                                       O_RDONLY, "%s/" TAGCACHE_FILE_INDEX,
                                       TC_STAT(ctx).db_path, tag);
        if (tcs->idxfd[tag] < 0)
        {
            logf("File not open!");
            return false;
        }
    }

    return true;
}

static bool retrieve(TC_CTX_PARAM struct tagcache_search *tcs, IF_DIRCACHE(int idx_id,)
                     struct index_entry *idx, int tag, char *buf, long bufsz)
{
    bool success = false;
    bool is_basename = false;
    struct tagfile_entry tfe;
    long seek;

    if (tag == tag_virt_basename)
    {
        tag = tag_filename;
        is_basename = true;
    }

    if (TAGCACHE_IS_NUMERIC(tag))
        goto failure;

    seek = idx->tag_seek[tag];
    if (seek < 0)
    {
        logf("Retrieve failed");
        goto failure;
    }

#ifdef HAVE_TC_RAMCACHE
    if (tcs->ramsearch)
    {
#ifdef HAVE_DIRCACHE
        if (tag == tag_filename && (idx->flag & FLAG_DIRCACHE))
        {
            if (dircache_get_fileref_path(&TC_DCFREFS(ctx)[idx_id], buf, bufsz) >= 0)
                success = true;
        }
        else
#endif /* HAVE_DIRCACHE */
        if (tag != tag_filename)
        {
            struct tagfile_entry *ep =
                (struct tagfile_entry *)&TC_RAM(ctx).hdr->tags[tag][seek];
            strmemccpy(buf, ep->tag_data, bufsz);
            success = true;
        }
    }
#endif /* HAVE_TC_RAMCACHE */

    if (!success && open_files(TC_CTX_ARG(ctx) tcs, tag))
    {
        lseek(tcs->idxfd[tag], seek, SEEK_SET);
        switch (read_tagfile_entry_and_tag(TC_CTX_ARG(ctx) tcs->idxfd[tag], &tfe, buf, bufsz))
        {
            case e_ENTRY_SIZEMISMATCH:
                logf("read error #5");
                break;
            case e_TAG_TOOLONG:
                logf("too long tag #5");
                break;
            case e_TAG_SIZEMISMATCH:
                logf("read error #6");
                break;
            default:
                logf("unknown_error");
                break;
            case e_SUCCESS_LEN_ZERO:
            case e_SUCCESS:
                success = true;
                break;
        }
    }

    if (success)
    {
        if (is_basename)
        {
            char* basename = strrchr(buf, '/');
            if (basename != NULL)
                memmove(buf, basename + 1, strlen(basename)); /* includes NULL */
        }
        return true;
    }

failure:
    str_setlen(buf, 0);
    return false;
}

#define COMMAND_QUEUE_IS_EMPTY (command_queue_ridx == command_queue_widx)

static long tc_find_tag(TC_CTX_PARAM int tag, int idx_id, const struct index_entry *idx)
{
#ifdef UI_COMMIT
    /* Enqueue updates the owning RAM snapshot immediately. Numeric readers
     * never wait behind journal flushes or generation validation. */
    (void)ctx;
    (void)idx_id;
    return idx->tag_seek[tag];
#else
#ifndef __PCTOOL__
    if (! COMMAND_QUEUE_IS_EMPTY && TAGCACHE_IS_NUMERIC(tag))
    {
        /* Attempt to find tag data through store-to-load forwarding in
           command queue */
        long result = -1;

        mutex_lock(&command_queue_mutex);

        int ridx = command_queue_widx;

        while (ridx != command_queue_ridx)
        {
            if (--ridx < 0)
                ridx = TAGCACHE_COMMAND_QUEUE_LENGTH - 1;

            if (command_queue[ridx].command == CMD_UPDATE_NUMERIC
#ifdef UI_COMMIT
                && command_queue[ridx].context == ctx
#endif
                && command_queue[ridx].idx_id == idx_id
                && command_queue[ridx].tag == tag)
            {
                result = command_queue[ridx].data;
                break;
            }
        }

        mutex_unlock(&command_queue_mutex);

        if (result >= 0)
        {
            logf("tc_find_tag: Recovered tag %d value %lX from write queue",
                 tag, (unsigned long) result);
            return result;
        }
    }
#else
    (void)idx_id;
#endif

    return idx->tag_seek[tag];
#endif
}

static inline long sec_in_ms(long ms)
{
    return (ms/1000) % 60;
}

static inline long min_in_ms(long ms)
{
    return (ms/1000) / 60;
}

static long check_virtual_tags(TC_CTX_PARAM int tag, int idx_id,
                               const struct index_entry *idx)
{
    long data = 0;

    switch (tag)
    {
        case tag_virt_length_sec:
            data = sec_in_ms(tc_find_tag(TC_CTX_ARG(ctx) tag_length, idx_id, idx));
            break;

        case tag_virt_length_min:
            data = min_in_ms(tc_find_tag(TC_CTX_ARG(ctx) tag_length, idx_id, idx));
            break;

        case tag_virt_playtime_sec:
            data = sec_in_ms(tc_find_tag(TC_CTX_ARG(ctx) tag_playtime, idx_id, idx));
            break;

        case tag_virt_playtime_min:
            data = min_in_ms(tc_find_tag(TC_CTX_ARG(ctx) tag_playtime, idx_id, idx));
            break;

        case tag_virt_autoscore:
            if (tc_find_tag(TC_CTX_ARG(ctx) tag_length, idx_id, idx) == 0
                || tc_find_tag(TC_CTX_ARG(ctx) tag_playcount, idx_id, idx) == 0)
            {
                data = 0;
            }
            else
            {
                /* A straight calculus gives:
                     autoscore = 100 * playtime / length / playcout (1)
                   Now, consider the euclidian division of playtime by length:
                     playtime = alpha * length + beta
                   With:
                     0 <= beta < length
                   Now, (1) becomes:
                     autoscore = 100 * (alpha / playcout + beta / length / playcount)
                   Both terms should be small enough to avoid any overflow
                */
                long playtime = tc_find_tag(TC_CTX_ARG(ctx) tag_playtime, idx_id, idx);
                long length = tc_find_tag(TC_CTX_ARG(ctx) tag_length, idx_id, idx);
                long playcount = tc_find_tag(TC_CTX_ARG(ctx) tag_playcount, idx_id, idx);
                data = 100 * (playtime / length) + (100 * (playtime % length)) / length;
                data /= playcount;
            }
            break;

        /* How many commits before the file has been added to the DB. */
        case tag_virt_entryage:
            data = TC_MASTER(ctx).commitid
                   - tc_find_tag(TC_CTX_ARG(ctx) tag_commitid, idx_id, idx) - 1;
            break;

        case tag_virt_basename:
            tag = tag_filename; /* return filename; caller handles basename */
            /* FALLTHRU */

        default:
            data = tc_find_tag(TC_CTX_ARG(ctx) tag, idx_id, idx);
    }

    return data;
}

long tagcache_get_numeric(const struct tagcache_search *tcs, int tag)
{
#ifdef UI_COMMIT
    struct tagcache_context *ctx = tcs->context;
#endif
    struct index_entry idx;

    if (!TC_STAT(ctx).ready)
        return false;

    if (!TAGCACHE_IS_NUMERIC(tag))
        return -1;

    if (!get_index(TC_CTX_ARG(ctx) tcs->masterfd, tcs->idx_id, &idx, true))
        return -2;

    return check_virtual_tags(TC_CTX_ARG(ctx) tag, tcs->idx_id, &idx);
}

inline static bool str_ends_with(const char *str1, const char *str2)
{
    logf_clauses("%s %s %s", str1, __func__, str2);
    int str_len = strlen(str1);
    int clause_len = strlen(str2);

    if (clause_len > str_len)
        return false;

    return !strcasecmp(&str1[str_len - clause_len], str2);
}

inline static bool str_oneof(const char *str, const char *list)
{
    logf_clauses("%s %s %s", str, __func__, list);
    const char *sep;
    int l, len = strlen(str);

    while (*list)
    {
        sep = strchr(list, '|');
        l = sep ? (intptr_t)sep - (intptr_t)list : (int)strlen(list);
        if ((l==len) && !strncasecmp(str, list, len))
            return true;
        list += sep ? l + 1 : l;
    }

    return false;
}

inline static bool str_begins_ends_oneof(const char *str, const char *list, bool begins)
{
    logf_clauses("%s %s (%s) %s", str, __func__, begins ? "begins" : "ends", list);
    const char *sep;
    int l, p, len = strlen(str);

    while (*list)
    {
        sep = strchr(list, '|');
        l = sep ? (intptr_t)sep - (intptr_t)list : (int)strlen(list);
        p = begins ? 0 : len - l;
        if (l <= len && !strncasecmp(&str[p], list, l))
            return true;
        list += sep ? l + 1 : l;
    }

    return false;
}

inline static bool str_contains_oneof(const char *str, char *list)
{
    logf_clauses("%s %s %s", str, __func__, list);
	const char *sep;
	int l, len = strlen(str);
	
	while (*list)
	{
		sep = strchr(list, '|');
		l = sep ? (intptr_t)sep - (intptr_t)list : (int)strlen(list);
		list[l] = '\0';
		if (l <= len && strcasestr(str, list) != NULL) {
			if (sep) list[l] = '|';
			return true;
		}
		if (sep) list[l] = '|';
		list += sep ? l + 1 : l;
	}
	
	return false;
}

static bool check_against_clause(long numeric, const char *str,
                                 const struct tagcache_search_clause *clause)
{
    if (clause->numeric)
    {
        switch (clause->type)
        {
            case clause_is:
                return numeric == clause->numeric_data;
            case clause_is_not:
                return numeric != clause->numeric_data;
            case clause_gt:
                return numeric > clause->numeric_data;
            case clause_gteq:
                return numeric >= clause->numeric_data;
            case clause_lt:
                return numeric < clause->numeric_data;
            case clause_lteq:
                return numeric <= clause->numeric_data;
            default:
                logf("Incorrect numeric tag: %d", clause->type);
        }
    }
    else
    {
        switch (clause->type)
        {
            case clause_is:
                return !strcasecmp(clause->str, str);
            case clause_is_not:
                return strcasecmp(clause->str, str);
            case clause_gt:
                return 0>strcasecmp(clause->str, str);
            case clause_gteq:
                return 0>=strcasecmp(clause->str, str);
            case clause_lt:
                return 0<strcasecmp(clause->str, str);
            case clause_lteq:
                return 0<=strcasecmp(clause->str, str);
            case clause_contains:
                return (strcasestr(str, clause->str) != NULL);
            case clause_not_contains:
                return (strcasestr(str, clause->str) == NULL);
            case clause_begins_with:
                return (strcasestr(str, clause->str) == str);
            case clause_not_begins_with:
                return (strcasestr(str, clause->str) != str);
            case clause_ends_with:
                return str_ends_with(str, clause->str);
            case clause_not_ends_with:
                return !str_ends_with(str, clause->str);
            case clause_oneof:
                return str_oneof(str, clause->str);
            case clause_not_oneof:
                return !str_oneof(str, clause->str);
            case clause_ends_oneof:
                /* Fall-Through */
            case clause_begins_oneof:
                return str_begins_ends_oneof(str, clause->str,
                                             clause->type == clause_begins_oneof);
            case clause_not_ends_oneof:
                /* Fall-Through */
            case clause_not_begins_oneof:
                return !str_begins_ends_oneof(str, clause->str,
                                            clause->type == clause_not_begins_oneof);
			case clause_contains_oneof:
				return str_contains_oneof(str, clause->str);
			case clause_not_contains_oneof:
				return !str_contains_oneof(str, clause->str);
            default:
                logf("Incorrect tag: %d", clause->type);
        }
    }

    return false;
}

static bool check_clauses(TC_CTX_PARAM struct tagcache_search *tcs,
                          struct index_entry *idx,
                          struct tagcache_search_clause **clauses, int count)
{
    int i;

    /* Go through all conditional clauses. */
    for (i = 0; i < count; i++)
    {
        int seek;
        char buf[256];
        const int bufsz = sizeof(buf);
        char *str = buf;
        struct tagcache_search_clause *clause = clauses[i];

        logf_clauses("%s clause %d %s %s [%ld] %s",
            "Checking",  i, tag_type_str[clause->type],
            tags_str[clause->tag],  clause->numeric_data,
            (clause->numeric || clause->str == NULL) ? "[NUMERIC?]" : clause->str);

        if (clause->type == clause_logical_or)
        {
            logf_clauses("Bailing");
            break; /* all conditions before logical-or satisfied --
                      stop processing clauses */
        }
        seek = check_virtual_tags(TC_CTX_ARG(ctx) clause->tag, tcs->idx_id, idx);

#ifdef HAVE_TC_RAMCACHE
        if (tcs->ramsearch)
        {
            struct tagfile_entry *tfe;

            if (!TAGCACHE_IS_NUMERIC(clause->tag))
            {
                if (clause->tag == tag_filename
                    || clause->tag == tag_virt_basename)
                {
                    retrieve(TC_CTX_ARG(ctx) tcs, IF_DIRCACHE(tcs->idx_id,) idx, clause->tag,
                             buf, bufsz);
                }
                else
                {
                    tfe = (struct tagfile_entry *)
                                        &TC_RAM(ctx).hdr->tags[clause->tag][seek];
                    /* str points to movable data, but no locking required here,
                     * as no yield() is following */
                    str = tfe->tag_data;
                }
            }
        }
        else
#endif /* HAVE_TC_RAMCACHE */
        {
            struct tagfile_entry tfe;

            if (!TAGCACHE_IS_NUMERIC(clause->tag))
            {
                int tag = clause->tag;
                if (tag == tag_virt_basename)
                    tag = tag_filename;

                int fd = tcs->idxfd[tag];
                lseek(fd, seek, SEEK_SET);

                switch (read_tagfile_entry_and_tag(TC_CTX_ARG(ctx) fd, &tfe, str, bufsz))
                {
                    case e_SUCCESS_LEN_ZERO: /* Check if entry has been deleted. */
                        return false;
                    case e_SUCCESS:
                        if (clause->tag == tag_virt_basename)
                        {
                            char *basename = strrchr(str, '/');
                            if (basename)
                                str = basename + 1;
                        }
                        break;
                    case e_ENTRY_SIZEMISMATCH:
                        logf("read error #15");
                        return false;
                    case e_TAG_TOOLONG:
                        logf("too long tag #6");
                        return false;
                    case e_TAG_SIZEMISMATCH:
                        logf("read error #16");
                        return false;
                    default:
                        logf("unknown_error");
                        break;;
                }
            }
        }

        if (!check_against_clause(seek, str, clause))
        {
            /* Clause failed -- try finding a logical-or clause */
            while (++i < count)
            {
                if (clauses[i]->type == clause_logical_or)
                    break;
            }

            if (i < count)        /* Found logical-or? */
                continue;         /* Check clauses after logical-or */

            return false;
        }

        logf_clauses("%s clause %d %s %s [%ld] %s",
            "Found",  i, tag_type_str[clause->type],
            tags_str[clause->tag],  clause->numeric_data,
            (clause->numeric || clause->str == NULL) ? "[NUMERIC?]" : clause->str);
    }

    return true;
}

bool tagcache_check_clauses(struct tagcache_search *tcs,
                            struct tagcache_search_clause **clause, int count)
{
#ifdef UI_COMMIT
    struct tagcache_context *ctx = tcs->context;
#endif
    struct index_entry idx;

    if (count == 0)
        return true;

    if (!get_index(TC_CTX_ARG(ctx) tcs->masterfd, tcs->idx_id, &idx, true))
        return false;

    return check_clauses(TC_CTX_ARG(ctx) tcs, &idx, clause, count);
}

static bool add_uniqbuf(struct tagcache_search *tcs, uint32_t id)
{
    int i;

    /* If uniq buffer is not defined we must return true for search to work. */
    if (tcs->unique_list == NULL || (!TAGCACHE_IS_UNIQUE(tcs->type)
                                     && !TAGCACHE_IS_NUMERIC(tcs->type)))
    {
        return true;
    }

    for (i = 0; i < tcs->unique_list_count; i++)
    {
        /* Return false if entry is found. */
        if (tcs->unique_list[i] == id)
        {
            //logf("%d Exists @ %d", id, i);
            return false;
        }
    }

    if (tcs->unique_list_count < tcs->unique_list_capacity)
    {
        tcs->unique_list[i] = id;
        tcs->unique_list_count++;
    }

    return true;
}

static bool build_lookup_list(TC_CTX_PARAM struct tagcache_search *tcs)
{
    struct index_entry entry;
    int i, j;

    tcs->seek_list_count = 0;

#ifdef HAVE_TC_RAMCACHE
    if (tcs->ramsearch)
    {
#ifdef UI_COMMIT
        /* a search begun before a commit or reload dropped the cache
         * (another thread may continue it meanwhile; the handle is 0
         * while a reload sizes the new cache, or after a failed load):
         * its data is gone, never pin handle 0 */
        if (!TC_STAT(ctx).ramcache || TC_RAM(ctx).handle <= 0)
        {
            tcs->valid = false;
            return false;
        }
#endif
        tcrc_buffer_lock(TC_CTX_PASS(ctx)); /* lock because below makes a pointer to movable data */

        for (i = tcs->seek_pos; i < TC_MASTER(ctx).tch.entry_count; i++)
        {
            struct tagcache_seeklist_entry *seeklist;
            /* idx points to movable data, don't yield or reload */
            struct index_entry *idx = &TC_RAM(ctx).hdr->indices[i];
            if (tcs->seek_list_count == SEEK_LIST_SIZE)
                break ;

            /* Skip deleted files. */
            if (idx->flag & FLAG_DELETED)
                continue;

            /* Go through all filters.. */
            for (j = 0; j < tcs->filter_count; j++)
            {
                if (idx->tag_seek[tcs->filter_tag[j]] != tcs->filter_seek[j])
                {
                    break ;
                }
            }

            if (j < tcs->filter_count)
                continue ;

            /* Check for conditions. */
            if (!check_clauses(TC_CTX_ARG(ctx) tcs, idx, tcs->clause, tcs->clause_count))
                continue;
            /* Add to the seek list if not already in uniq buffer (doesn't yield)*/
            if (!add_uniqbuf(tcs, idx->tag_seek[tcs->type]))
                continue;

            /* Lets add it. */
            seeklist = &tcs->seeklist[tcs->seek_list_count];
            seeklist->seek = idx->tag_seek[tcs->type];
            seeklist->flag = idx->flag;
            seeklist->idx_id = i;
            tcs->seek_list_count++;
        }

        tcrc_buffer_unlock(TC_CTX_PASS(ctx));

        tcs->seek_pos = i;

        return tcs->seek_list_count > 0;
    }
#endif /* HAVE_TC_RAMCACHE */

    if (tcs->masterfd < 0)
    {
        struct master_header tcmh;
        tcs->masterfd = open_master_fd(TC_CTX_ARG(ctx) &tcmh, false);
    }

    lseek(tcs->masterfd, tcs->seek_pos * sizeof(struct index_entry) +
            sizeof(struct master_header), SEEK_SET);

    while (read_index_entries(TC_CTX_ARG(ctx) tcs->masterfd, &entry, 1) == sizeof(struct index_entry))
    {
        struct tagcache_seeklist_entry *seeklist;

        if (tcs->seek_list_count == SEEK_LIST_SIZE)
            break ;

        i = tcs->seek_pos;
        tcs->seek_pos++;

        /* Check if entry has been deleted. */
        if (entry.flag & FLAG_DELETED)
            continue;

        /* Go through all filters.. */
        for (j = 0; j < tcs->filter_count; j++)
        {
            if (entry.tag_seek[tcs->filter_tag[j]] != tcs->filter_seek[j])
                break ;
        }

        if (j < tcs->filter_count)
            continue ;

        /* Check for conditions. */
        if (!check_clauses(TC_CTX_ARG(ctx) tcs, &entry, tcs->clause, tcs->clause_count))
            continue;

        /* Add to the seek list if not already in uniq buffer. */
        if (!add_uniqbuf(tcs, entry.tag_seek[tcs->type]))
            continue;

        /* Lets add it. */
        seeklist = &tcs->seeklist[tcs->seek_list_count];
        seeklist->seek = entry.tag_seek[tcs->type];
        seeklist->flag = entry.flag;
        seeklist->idx_id = i;
        tcs->seek_list_count++;

        yield();
    }

    return tcs->seek_list_count > 0;
}


#ifdef UI_COMMIT
static bool search_context(TC_CTX_PARAM struct tagcache_search *tcs, int tag)
#else
bool tagcache_search(struct tagcache_search *tcs, int tag)
#endif
{
    /* NOTE: call tagcache_search_finish(&tcs) when finished or BAD things may happen (TM) */
    struct tagcache_header tag_hdr;
    struct master_header   master_hdr;
    int i;

#ifdef UI_COMMIT
    /* Capture before the first open/sleep: filesystems can yield before a
     * search has opened all of its descriptors. */
    if (!ui_context_ref(ctx, false))
        return false;
#endif

    while (TC_READ_LOCK(ctx))
        sleep(1);

    memset(tcs, 0, sizeof(struct tagcache_search));
#ifdef UI_COMMIT
    tcs->context = ctx;
#endif
#ifdef UI_COMMIT
    if (TC_STAT(ctx).commit_step > 0 || !TC_STAT(ctx).ready
        || tag < 0 || tag >= TAG_COUNT)
    {
        ui_context_put(ctx, false);
        return false;
    }
#else
    if (tc_stat.commit_step > 0 || !tc_stat.ready)
        return false;
#endif

    tcs->position = sizeof(struct tagcache_header);
    tcs->type = tag;
    tcs->seek_pos = 0;
    tcs->list_position = 0;
    tcs->seek_list_count = 0;
    tcs->filter_count = 0;
    tcs->masterfd = -1;

    for (i = 0; i < TAG_COUNT; i++)
        tcs->idxfd[i] = -1;

#ifndef HAVE_TC_RAMCACHE
    tcs->ramsearch = false;
#else
    tcs->ramsearch = TC_STAT(ctx).ramcache;
    if (tcs->ramsearch)
    {
        tcs->entry_count = TC_RAM(ctx).hdr->entry_count[tcs->type];
    }
    else
#endif
    {
        /* Always open as R/W so we can pass tcs to functions that modify data also
         * without failing. */
        tcs->masterfd = open_master_fd(TC_CTX_ARG(ctx) &master_hdr, true);
        if (tcs->masterfd < 0)
#ifdef UI_COMMIT
        {
            ui_context_put(ctx, false);
            return false;
        }
#else
            return false;
#endif

        if (!TAGCACHE_IS_NUMERIC(tcs->type))
        {
            tcs->idxfd[tcs->type] = open_tag_fd(TC_CTX_ARG(ctx) &tag_hdr, tcs->type, false);
            if (tcs->idxfd[tcs->type] < 0)
#ifdef UI_COMMIT
            {
                close(tcs->masterfd);
                ui_context_put(ctx, false);
                return false;
            }
#else
                return false;
#endif

            tcs->entry_count = tag_hdr.entry_count;
        }
        else
        {
            tcs->entry_count = master_hdr.tch.entry_count;
        }
    }

    tcs->valid = true;
    tcs->initialized = true;
    TC_WRITE_LOCK(ctx)++;

    return true;
}

#ifdef UI_COMMIT
bool tagcache_search(struct tagcache_search *tcs, int tag)
{
    struct tagcache_context *ctx = tagcache_context_acquire();
    bool ok = search_context(ctx, tcs, tag);
    tagcache_context_release(ctx);
    return ok;
}

bool tagcache_search_ctx(struct tagcache_context *ctx,
                         struct tagcache_search *tcs, int tag)
{
    return search_context(ctx, tcs, tag);
}
#endif

void tagcache_search_set_uniqbuf(struct tagcache_search *tcs,
                                 void *buffer, long length)
{
    tcs->unique_list = (uint32_t *)buffer;
    tcs->unique_list_capacity = length / sizeof(*tcs->unique_list);
    tcs->unique_list_count = 0;
    memset(tcs->unique_list, 0, tcs->unique_list_capacity);
}

bool tagcache_search_add_filter(struct tagcache_search *tcs,
                                int tag, int seek)
{
    if (tcs->filter_count == TAGCACHE_MAX_FILTERS)
        return false;

    if (TAGCACHE_IS_NUMERIC_OR_NONUNIQUE(tag))
        return false;

    tcs->filter_tag[tcs->filter_count] = tag;
    tcs->filter_seek[tcs->filter_count] = seek;
    tcs->filter_count++;

    return true;
}

bool tagcache_search_add_clause(struct tagcache_search *tcs,
                                struct tagcache_search_clause *clause)
{
    int i;
    int clause_count = tcs->clause_count;

    if (clause_count >= TAGCACHE_MAX_CLAUSES)
    {
        logf("Too many clauses");
        return false;
    }

    if (clause->type != clause_logical_or)
    {
        /* BUGFIX OR'd clauses seem to be mishandled once made into a filter */
        if (clause_count <= 1 || tcs->clause[clause_count - 1]->type != clause_logical_or)
        {
            /* Check if there is already a similar filter in present (filters are
             * much faster than clauses).
             */
            for (i = 0; i < tcs->filter_count; i++)
            {
                if (tcs->filter_tag[i] == clause->tag)
                {
                    return true;
                }
            }
        }

        if (!TAGCACHE_IS_NUMERIC(clause->tag) && tcs->idxfd[clause->tag] < 0)
        {
            char fname[MAX_PATH];
            tcs->idxfd[clause->tag] = open_pathfmt(fname, sizeof(fname), O_RDONLY,
                                                   "%s/" TAGCACHE_FILE_INDEX,
                                                   tc_stat.db_path, clause->tag);
        }
    }

    tcs->clause[tcs->clause_count] = clause;
    tcs->clause_count++;

    return true;
}

static bool get_next(TC_CTX_PARAM struct tagcache_search *tcs, bool is_numeric, char *buf, long bufsz)
{
    struct tagfile_entry entry;
#if defined(HAVE_TC_RAMCACHE) && defined(HAVE_DIRCACHE)
    long flag = 0;
#endif

    if (tcs->idxfd[tcs->type] < 0 && !is_numeric
#ifdef HAVE_TC_RAMCACHE
        && !tcs->ramsearch
#endif
        )
        return false;

    /* Relative fetch. */
    if (tcs->filter_count > 0 || tcs->clause_count > 0 || is_numeric
#if defined(HAVE_TC_RAMCACHE) && defined(HAVE_DIRCACHE)
        /* We need to retrieve flag status for dircache. */
        || (tcs->ramsearch && tcs->type == tag_filename)
#endif
        )
    {
        struct tagcache_seeklist_entry *seeklist;

        /* Check for end of list. */
        if (tcs->list_position == tcs->seek_list_count)
        {
            tcs->list_position = 0;

            /* Try to fetch more. */
            if (!build_lookup_list(TC_CTX_ARG(ctx) tcs))
            {
                tcs->valid = false;
                return false;
            }
        }

        seeklist = &tcs->seeklist[tcs->list_position];
#if defined(HAVE_TC_RAMCACHE) && defined(HAVE_DIRCACHE)
        flag = seeklist->flag;
#endif
        tcs->position = seeklist->seek;
        tcs->idx_id = seeklist->idx_id;
        tcs->list_position++;
    }
    else
    {
        if (tcs->entry_count == 0)
        {
            tcs->valid = false;
            return false;
        }

        tcs->entry_count--;
    }

    tcs->result_seek = tcs->position;

    if (is_numeric)
    {
        itoa_buf(buf, bufsz, tcs->position);
        tcs->result = buf;
        tcs->result_len = strlen(buf) + 1;
        return true;
    }

    /* Direct fetch. */
#ifdef HAVE_TC_RAMCACHE
    if (tcs->ramsearch)
    {
#ifdef HAVE_DIRCACHE
        if (tcs->type == tag_filename && (flag & FLAG_DIRCACHE))
        {
            ssize_t len = dircache_get_fileref_path(&TC_DCFREFS(ctx)[tcs->idx_id],
                                                    buf, bufsz);
            if (len >= 0)
            {
                tcs->result_len = len + 1;
                tcs->result     = buf;
                tcs->ramresult  = false;
                return true;
            }
            /* else do it the hard way */
        }
#endif /* HAVE_DIRCACHE */

        if (tcs->type != tag_filename)
        {
            struct tagfile_entry *ep;

            ep = (struct tagfile_entry *)&TC_RAM(ctx).hdr->tags[tcs->type][tcs->position];
            /* don't return ep->tag_data directly as it may move */
            tcs->result_len = strlcpy(buf, ep->tag_data, bufsz) + 1;
            tcs->result = buf;
            tcs->idx_id = ep->idx_id;
            tcs->ramresult = false; /* was true before we copied to buf too */

            /* Increase position for the next run. This may get overwritten. */
            tcs->position += sizeof(struct tagfile_entry) + ep->tag_length;

            return true;
        }
    }
#endif /* HAVE_TC_RAMCACHE */

    if (!open_files(TC_CTX_ARG(ctx) tcs, tcs->type))
    {
        tcs->valid = false;
        return false;
    }

    /* Seek stream to the correct position and continue to direct fetch. */
    lseek(tcs->idxfd[tcs->type], tcs->position, SEEK_SET);

    switch (read_tagfile_entry_and_tag(TC_CTX_ARG(ctx) tcs->idxfd[tcs->type], &entry, buf, bufsz))
    {
        case e_SUCCESS_LEN_ZERO:
        case e_SUCCESS:
             break;
        case e_ENTRY_SIZEMISMATCH:
            logf("read error #5");
            tcs->valid = false;
            return false;
        case e_TAG_TOOLONG:
            tcs->valid = false;
            logf("too long tag #2");
            logf("P:%lX/%" PRIX32, (unsigned long) tcs->position, entry.tag_length);
            return false;
        case e_TAG_SIZEMISMATCH:
            tcs->valid = false;
            logf("read error #4");
            return false;
    }

    /**
     Update the position for the next read (this may be overridden
     if filters or clauses are being used).
     */
    tcs->position += sizeof(struct tagfile_entry) + entry.tag_length;
    str_setlen(buf, entry.tag_length);

    tcs->result = buf;
    tcs->result_len = entry.tag_length + 1;
    tcs->idx_id = entry.idx_id;
    tcs->ramresult = false;

    return true;
}

bool tagcache_get_next(struct tagcache_search *tcs, char *buf, long size)
{
#ifdef UI_COMMIT
    struct tagcache_context *ctx = tcs->context;
#endif
    if (tcs->valid && tagcache_is_usable())
    {
        bool is_numeric = TAGCACHE_IS_NUMERIC(tcs->type);
        while (get_next(TC_CTX_ARG(ctx) tcs, is_numeric, buf, size))
        {
            if (tcs->result_len > 1)
                return true;
        }
    }
#ifdef LOGF_ENABLE
    if (tcs->unique_list_count > 0)
        logf(" uniqbuf: %d used / %d avail", tcs->unique_list_count, tcs->unique_list_capacity);
#endif

    return false;
}

bool tagcache_retrieve(struct tagcache_search *tcs, int idxid,
                       int tag, char *buf, long size)
{
#ifdef UI_COMMIT
    struct tagcache_context *ctx = tcs->context;
#endif
    struct index_entry idx;

    *buf = '\0';
    if (!get_index(TC_CTX_ARG(ctx) tcs->masterfd, idxid, &idx, true))
        return false;

    return retrieve(TC_CTX_ARG(ctx) tcs, IF_DIRCACHE(idxid,) &idx, tag, buf, size);
}

void tagcache_search_finish(struct tagcache_search *tcs)
{
#ifdef UI_COMMIT
    struct tagcache_context *ctx = tcs->context;
#endif
    int i;

    if (!tcs->initialized)
        return;

    if (tcs->masterfd >= 0)
    {
        close(tcs->masterfd);
        tcs->masterfd = -1;
    }

    for (i = 0; i < TAG_COUNT; i++)
    {
        if (tcs->idxfd[i] >= 0)
        {
            close(tcs->idxfd[i]);
            tcs->idxfd[i] = -1;
        }
    }

    tcs->ramsearch = false;
    tcs->valid = false;
    tcs->initialized = 0;
    if (TC_WRITE_LOCK(ctx) > 0)
        TC_WRITE_LOCK(ctx)--;
#ifdef UI_COMMIT
    ui_context_put(ctx, false);
    tcs->context = NULL;
#endif
}

#if defined(HAVE_TC_RAMCACHE) && defined(HAVE_DIRCACHE)
static struct tagfile_entry *get_tag(TC_CTX_PARAM const struct index_entry *entry, int tag)
{
    return (struct tagfile_entry *)&TC_RAM(ctx).hdr->tags[tag][entry->tag_seek[tag]];
}

static long get_tag_numeric(TC_CTX_PARAM const struct index_entry *entry, int tag, int idx_id)
{
    return check_virtual_tags(TC_CTX_ARG(ctx) tag, idx_id, entry);
}

static char* get_tag_string(TC_CTX_PARAM const struct index_entry *entry, int tag)
{
    char* s = get_tag(TC_CTX_ARG(ctx) entry, tag)->tag_data;
    return strcmp(s, UNTAGGED) ? s : NULL;
}

#ifdef UI_COMMIT
static bool tagcache_fill_tags_held(TC_CTX_PARAM struct mp3entry *id3, const char *filename)
#else
bool tagcache_fill_tags(struct mp3entry *id3, const char *filename)
#endif
{
    struct index_entry *entry;
    int idx_id;

    if (!TC_STAT(ctx).ready || !TC_STAT(ctx).ramcache)
        return false;

    /* Find the corresponding entry in tagcache. */

    if (filename != NULL)
        memset(id3, 0, sizeof(struct mp3entry));
    else /* Note: caller clears id3 prior to call */
        filename = id3->path;

    idx_id = find_entry_ram(TC_CTX_ARG(ctx) filename);
    if (idx_id < 0)
        return false;

    entry = &TC_RAM(ctx).hdr->indices[idx_id];

    char* buf = id3->id3v2buf;
    ssize_t remaining = sizeof(id3->id3v2buf);

    /* this macro sets id3 strings by copying to the id3v2buf */
#define SET(x, y) do                                                           \
    {                                                                          \
        if (remaining > 0)                                                     \
        {                                                                      \
            x          = NULL; /* initialize with null if tag doesn't exist */ \
            char* src = get_tag_string(TC_CTX_ARG(ctx) entry, y);                              \
            if (src)                                                           \
            {                                                                  \
                x = buf;                                                       \
                size_t len = strlcpy(buf, src, remaining) +1;                  \
                buf += len; remaining -= len;                                  \
            }                                                                  \
        }                                                                      \
    } while(0)


    SET(id3->title,         tag_title);
    SET(id3->artist,        tag_artist);
    SET(id3->album,         tag_album);
    SET(id3->genre_string,  tag_genre);
    SET(id3->composer,      tag_composer);
    SET(id3->comment,       tag_comment);
    SET(id3->albumartist,   tag_albumartist);
    SET(id3->grouping,      tag_grouping);

    id3->length     = get_tag_numeric(TC_CTX_ARG(ctx) entry, tag_length, idx_id);
    id3->playcount  = get_tag_numeric(TC_CTX_ARG(ctx) entry, tag_playcount, idx_id);
    id3->rating     = get_tag_numeric(TC_CTX_ARG(ctx) entry, tag_rating, idx_id);
    id3->lastplayed = get_tag_numeric(TC_CTX_ARG(ctx) entry, tag_lastplayed, idx_id);
    id3->score      = get_tag_numeric(TC_CTX_ARG(ctx) entry, tag_virt_autoscore, idx_id) / 10;
    id3->year       = get_tag_numeric(TC_CTX_ARG(ctx) entry, tag_year, idx_id);

    id3->discnum = get_tag_numeric(TC_CTX_ARG(ctx) entry, tag_discnumber, idx_id);
    id3->tracknum = get_tag_numeric(TC_CTX_ARG(ctx) entry, tag_tracknumber, idx_id);
    id3->bitrate = get_tag_numeric(TC_CTX_ARG(ctx) entry, tag_bitrate, idx_id);
    if (id3->bitrate == 0)
        id3->bitrate = 1;

    if (global_settings.autoresume_enable
        || metadata_is_audiobook_ext(id3->path))
    {
        id3->elapsed = get_tag_numeric(TC_CTX_ARG(ctx) entry, tag_lastelapsed, idx_id);
        logf("tagcache_fill_tags: Set elapsed for %s to %lX\n",
             id3->title, id3->elapsed);

        id3->offset = get_tag_numeric(TC_CTX_ARG(ctx) entry, tag_lastoffset, idx_id);
        logf("tagcache_fill_tags: Set offset for %s to %lX\n",
             id3->title, id3->offset);
    }

    return true;
}

#ifdef UI_COMMIT
bool tagcache_fill_tags(struct mp3entry *id3, const char *filename)
{
    struct tagcache_context *ctx = tagcache_context_acquire();
    bool ok = tagcache_fill_tags_held(TC_CTX_ARG(ctx) id3, filename);
    tagcache_context_release(ctx);
    return ok;
}
#endif
#endif /* defined(HAVE_TC_RAMCACHE) && defined(HAVE_DIRCACHE) */

static inline void write_item(const char *item)
{
    int len = strlen(item) + 1;

    data_size += len;
    write(cachefd, item, len);
}

static int check_if_empty(char **tag)
{
    int length;

    if (*tag == NULL || **tag == '\0')
    {
        *tag = UNTAGGED;
        return sizeof(UNTAGGED); /* Tag length */
    }

    length = strlen(*tag);
    if (length > TAG_MAXLEN)
    {
        logf("over length tag: %s", *tag);
        length = TAG_MAXLEN;
        str_setlen((*tag), length);
    }

    return length + 1;
}

#ifdef __PCTOOL__
static void db_log(const char *prefix, const char *msg)
{
    /* Crude logging for the sim - to aid in debugging */
    int logfd = open(ROCKBOX_DIR "/database.log",
                     O_WRONLY | O_APPEND | O_CREAT, 0666);
    if (logfd >= 0)
    {
        write(logfd, prefix, strlen(prefix));
        write(logfd, ": ", 2);
        write(logfd, msg, strlen(msg));
        write(logfd, "\n", 1);
        close(logfd);
    }
}
#endif

#ifdef HAVE_IPOD_UI
static bool ui_is_one_of(const char *s, const char *a, const char *b)
{
    return s && (!strcasecmp(s, a) || !strcasecmp(s, b));
}

/* The shell keeps audiobooks and podcasts apart from music and lists
 * compilations separately. Decided once here, while the file is open;
 * a podcast wins over an audiobook (chaptered podcast episodes). */
static int32_t ui_classify(const struct mp3entry *id3, const char *path)
{
    int32_t flag = TAGCACHE_UI_VIDEO_CHECKED;

    if (id3->is_video)
        return flag | TAGCACHE_UI_VIDEO;

    if (ui_is_one_of(id3->genre_string, "Podcast", "Podcasts")
        || strcasestr(path, "/Podcasts/"))
        flag |= TAGCACHE_UI_PODCAST;
    else if (id3->is_audiobook
             || ui_is_one_of(id3->genre_string, "Audiobook", "Audiobooks"))
        flag |= TAGCACHE_UI_AUDIOBOOK;

    if (ui_is_one_of(id3->albumartist, "Various Artists", "Various"))
        flag |= TAGCACHE_UI_COMPILATION;
    return flag;
}

#ifdef HAVE_BCMFW_VIDEO
/* Videos (docs/phase14a-brief.md 3.2): the class bits and the grouping the
 * shell's Movies, TV Shows and Podcasts lists read. vclass_classify() writes
 * cleaned names into 'buf' (add_tagcache()'s ui_title, no static); the
 * scanner then stores album <- show (TV; "" stays untagged and the screen
 * shows Unknown Show), discnumber <- season, tracknumber <- episode,
 * title <- title. Scanner (tagcache thread) only. */
static int32_t ui_classify_video(struct mp3entry *id3, const char *path,
                                 char *buf, size_t len)
{
    struct vclass_in in = {
        .path = path, .album = id3->album, .title = id3->title,
        .genre = id3->genre_string, .tvsh = id3->tv_show,
        .is_video = id3->is_video, .pcst = id3->is_podcast,
        .stik = id3->stik, .tvsn = id3->tv_season, .tves = id3->tv_episode,
    };
    struct vclass_out out;
    int32_t flag = vclass_classify(&in, &out, buf, len);

    if (flag & TAGCACHE_UI_TV)
    {
        id3->album = *out.show ? (char *)out.show : NULL;
        id3->discnum = out.season;
        id3->tracknum = out.episode;
    }
    id3->title = (char *)out.title;
    DEBUGF("vclass: %s %s show='%s' s=%d e=%d title='%s'\n",
           (flag & TAGCACHE_UI_PODCAST) ? "podcast"
           : (flag & TAGCACHE_UI_TV) ? "tv" : "movie",
           path, out.show, out.season, out.episode, out.title);
    return flag | TAGCACHE_UI_VIDEO_CHECKED;
}
#endif

/* Podcasts and audiobooks remember their playback position (the 7G
 * default); tagtree.c asks when a track is buffered and when it ends.
 * Videos always do: tested from the parsed field, no classifier on the
 * audio thread (brief 3.2a, M10). */
bool tagcache_ui_resumes(const struct mp3entry *id3)
{
#ifdef HAVE_BCMFW_VIDEO
    if (id3->is_video)
        return true;
#endif
    return ui_classify(id3, id3->path)
           & (TAGCACHE_UI_PODCAST | TAGCACHE_UI_AUDIOBOOK);
}
#endif /* HAVE_IPOD_UI */

/* GCC 3.4.6 for Coldfire can choose to inline this function. Not a good
 * idea, as it uses lots of stack and is called from a recursive function
 * (check_dir).
 */
static void NO_INLINE add_tagcache(char *path, unsigned long mtime)
{
    #define ADD_TAG(entry, tag, data) \
        /* Adding tag */                              \
        entry.tag_length[tag] = check_if_empty(data); \
        entry.tag_offset[tag] = offset;               \
        offset += entry.tag_length[tag]

    struct mp3entry id3;
    struct temp_file_entry entry;
    bool ret;
    int idx_id = -1;
    char tracknumfix[3];
    int offset = 0;
    int path_length = strlen(path);
    bool has_artist;
    bool has_grouping;
#ifdef HAVE_IPOD_UI
    char ui_title[MAX_PATH];
#endif

    DB_LOG("file", path);

    if (cachefd < 0
#ifdef UI_COMMIT
        && !ui_probe
#endif
       )
        return ;

    /* Check for overlength file path. */
    if (path_length > MAX_PATH || path_length > TAG_MAXLEN)
    {
        /* Path can't be shortened. */
        logf("Too long path: %s", path);
        DB_LOG("error", "path too long");
        return ;
    }

    /* Check if the file is supported. */
    int afmt = probe_file_format(path);
    if (afmt == AFMT_UNKNOWN)
        return;

    /* Check if the file is already cached. */
#if defined(HAVE_TC_RAMCACHE) && defined(HAVE_DIRCACHE)
    idx_id = find_entry_ram(TC_CTX_ARG(ui_build) path);
#endif

    /* Be sure the entry doesn't exist. */
#ifdef UI_COMMIT
    struct index_entry idx;
#ifdef UI_TIMING
    ui_scan_lookups++;
#endif
    /* the scan's table: a miss reads nothing (scan speed, 1019) */
    bool have_idx = filenametag_fd >= 0 && idx_id < 0 && ui_scan.handle > 0
                    && path_length < MAX_PATH;  /* the name fits ui_title */
    if (have_idx)
    {
        idx_id = ui_scan_find(path, &idx, ui_title);
        if (idx_id == -2)
        {
            DB_LOG("error", "failed to retrieve index entry");
            return;
        }
        have_idx = idx_id >= 0;
#ifdef UI_TIMING
        if (!have_idx)
            ui_scan_misses++;
#endif
    }
    else
#endif
    if (filenametag_fd >= 0 && idx_id < 0)
        idx_id = find_entry_disk(TC_CTX_ARG(ui_build) path, false);

    /* Check if file has been modified. */
    if (idx_id >= 0)
    {
#ifndef UI_COMMIT
        struct index_entry idx;

#endif
        /* TODO: Mark that the index exists (for fast reverse scan) */
        //found_idx[idx_id/8] |= idx_id%8;

#ifdef UI_COMMIT
        if (!have_idx
            && !get_index(TC_CTX_ARG(ui_build) ui_scan.masterfd, idx_id, &idx, true))
#else
        if (!get_index(TC_CTX_ARG(ui_build) -1, idx_id, &idx, true))
#endif
        {
            logf("failed to retrieve index entry");
            DB_LOG("error", "failed to retrieve index entry");
            return ;
        }

        if ((unsigned long)idx.tag_seek[tag_mtime] == mtime)
        {
#ifdef UI_COMMIT
            /* Older audio-only shell builds did not record video streams.
             * Migrate classifications locally, without deleting/re-adding
             * tracks or invalidating artwork identities. Suffixes select
             * containers to inspect; only parsed stream metadata classifies. */
            if (!(idx.flag & TAGCACHE_UI_VIDEO_CHECKED))
            {
                const char *ext = strrchr(path, '.');
                bool inspect = ext && (!strcasecmp(ext, ".mp4")
                    || !strcasecmp(ext, ".m4v") || !strcasecmp(ext, ".mov")
                    || !strcasecmp(ext, ".m4a") || !strcasecmp(ext, ".m4b"));
                bool checked = !inspect
                    || get_metadata_afmt(&id3, -1, path, afmt,
                                         METADATA_EXCLUDE_ID3_PATH);
                if (checked && ui_probe)
                {
                    ui_probe_found = true;  /* a reclassification */
                    return;
                }
                if (checked)
                {
                    struct master_header mh;
                    int fd = open_master_fd(ui_build, &mh, true);
                    idx.flag |= TAGCACHE_UI_VIDEO_CHECKED;
                    if (inspect && id3.is_video)
#ifdef HAVE_BCMFW_VIDEO
                        idx.flag |= id3.video_avc ? TAGCACHE_UI_VIDEO
                                                  : TAGCACHE_UI_VIDEO_OTHER;
#else
                        idx.flag |= TAGCACHE_UI_VIDEO;
#endif
                    if (fd >= 0)
                    {
                        write_index(ui_build, fd, idx_id, &idx);
                        close(fd);
                    }
                }
            }
#endif
            /* No changes to file. */
#ifdef UI_COMMIT
            ui_probe_hits++;
#endif
            return ;
        }

        /* Metadata might have been changed. Delete the entry. */
        logf("Re-adding: %s", path);
        DB_LOG("info", "re-adding");
#ifndef UI_COMMIT
        if (!delete_entry(idx_id))
        {
            logf("delete_entry failed: %d", idx_id);
            DB_LOG("error", "delete entry failed");
            return ;
        }
#endif
    }

    /*memset(&id3, 0, sizeof(struct mp3entry)); -- get_metadata does this for us */
    memset(&entry, 0, sizeof(struct temp_file_entry));
    memset(&tracknumfix, 0, sizeof(tracknumfix));
    ret = get_metadata_afmt(&id3, -1, path, afmt, METADATA_EXCLUDE_ID3_PATH);

    if (!ret)
    {
        logf("get_metadata failed: %s", path);
        DB_LOG("error", "get_metadata failed");
#ifdef UI_COMMIT
        if (!ui_walk_metafail++)
            UI_BS_LOG("walk metafail %s", path);    /* the first one */
#endif
        return ;
    }

#ifdef UI_COMMIT
    if (ui_probe)
    {
        ui_probe_found = true;  /* a new or changed file */
        return;
    }
    /* Parse first. A temporarily unreadable modified file must not turn a
     * successfully cached track into a deletion in the next generation. */
    if (idx_id >= 0 && !delete_entry(idx_id))
        return;
#endif

    logf("-> %s", path);

    if (id3.tracknum < 0)              /* Track number missing? */
    {
        id3.tracknum = -1;
    }

#ifdef HAVE_IPOD_UI
#ifdef HAVE_BCMFW_VIDEO
    if (id3.is_video && id3.video_avc)
        entry.flag = ui_classify_video(&id3, path, ui_title, sizeof(ui_title));
    else
#endif
    {
    /* lists never open files: an untagged title is the file name
     * without its extension, as on the 7G */
    if (id3.title == NULL || id3.title[0] == '\0')
    {
        const char *base = strrchr(path, '/');
        char *dot;

        strmemccpy(ui_title, base ? base + 1 : path, sizeof(ui_title));
        dot = strrchr(ui_title, '.');
        if (dot && dot != ui_title)
            *dot = '\0';
        id3.title = ui_title;
    }
    entry.flag = ui_classify(&id3, path);
#ifdef HAVE_BCMFW_VIDEO
    if (id3.is_video)           /* not avc1/avc3: hidden, never music */
        entry.flag = (entry.flag & ~TAGCACHE_UI_VIDEO)
                     | TAGCACHE_UI_VIDEO_OTHER;
#endif
    }
#endif

    /* Numeric tags */
    entry.tag_offset[tag_year] = id3.year;
    entry.tag_offset[tag_discnumber] = id3.discnum;
    entry.tag_offset[tag_tracknumber] = id3.tracknum;
    entry.tag_offset[tag_length] = id3.length;
    entry.tag_offset[tag_bitrate] = id3.bitrate;
    entry.tag_offset[tag_mtime] = mtime;

    /* String tags. */
    has_artist = id3.artist != NULL
        && strlen(id3.artist) > 0;
    has_grouping = id3.grouping != NULL
        && strlen(id3.grouping) > 0;

    ADD_TAG(entry, tag_filename, &path);
    ADD_TAG(entry, tag_title, &id3.title);
    ADD_TAG(entry, tag_artist, &id3.artist);
    ADD_TAG(entry, tag_album, &id3.album);
    ADD_TAG(entry, tag_genre, &id3.genre_string);
    ADD_TAG(entry, tag_composer, &id3.composer);
    ADD_TAG(entry, tag_comment, &id3.comment);
    ADD_TAG(entry, tag_albumartist, &id3.albumartist);
    if (has_artist)
    {
        ADD_TAG(entry, tag_virt_canonicalartist, &id3.artist);
    }
    else
    {
        ADD_TAG(entry, tag_virt_canonicalartist, &id3.albumartist);
    }
    if (has_grouping)
    {
        ADD_TAG(entry, tag_grouping, &id3.grouping);
    }
    else
    {
        ADD_TAG(entry, tag_grouping, &id3.title);
    }
    entry.data_length = offset;

    /* Write the header */
    write(cachefd, &entry, sizeof(struct temp_file_entry));

    /* And tags also... Correct order is critical */
    write_item(path);
    write_item(id3.title);
    write_item(id3.artist);
    write_item(id3.album);
    write_item(id3.genre_string);
    write_item(id3.composer);
    write_item(id3.comment);
    write_item(id3.albumartist);
    if (has_artist)
    {
        write_item(id3.artist);
    }
    else
    {
        write_item(id3.albumartist);
    }
    if (has_grouping)
    {
        write_item(id3.grouping);
    }
    else
    {
        write_item(id3.title);
    }

    total_entry_count++;

    #undef ADD_TAG
}
#endif /*!defined(PLUGIN)*/


static bool tempbuf_insert(char *str, int id, int idx_id, bool unique)
{
    struct tempbuf_searchidx *index = (struct tempbuf_searchidx *)tempbuf;
    int len = strlen(str)+1;
    int i;
    unsigned *crcbuf = (unsigned *)&tempbuf[tempbuf_size-4];
    unsigned crc32 = 0xffffffff;
#ifdef UI_COLLATE
    /* #47: one string per name of one shell group (sortkey.h case
     * folding, Latin, Greek and Cyrillic), not ASCII case only */
    crc32 = ui_casefold_hash(str);
#else
    char chr_lower;
    for (i = 0; str[i] != '\0' && i < len -1; i++)
    {
        chr_lower = tolower(str[i]);
        crc32 = crc_32(&chr_lower, 1, crc32);
    }
#endif

    if (unique)
    {
        /* Check if the crc does not exist -> entry does not exist for sure. */
        for (i = 0; i < tempbufidx; i++)
        {
            if (crcbuf[-i] != crc32)
                continue;

#ifdef UI_COLLATE
            if (!ui_casefold_cmp(str, index[i].str))
#else
            if (!strcasecmp(str, index[i].str))
#endif
            {
                if (id < 0 || id >= lookup_buffer_depth)
                {
                    logf("lookup buf overf.: %d", id);
                    return false;
                }

                lookup[id] = &index[i];
                return true;
            }
        }
    }

    /* Insert to CRC buffer. */
    crcbuf[-tempbufidx] = crc32;
    tempbuf_left -= 4;

    /* Insert it to the buffer. */
    tempbuf_left -= len;
    if (tempbuf_left - 4 < 0 || tempbufidx >= commit_entry_count)
    {
        logf("temp buf error rem: %ld idx: %ld / %ld",
             tempbuf_left, tempbufidx, commit_entry_count-1);
        return false;
    }
    if (id >= lookup_buffer_depth)
    {
        logf("lookup buf overf. #2: %d", id);
        return false;
    }

    if (id >= 0)
    {
        lookup[id] = &index[tempbufidx];
        index[tempbufidx].idlist.id = id;
    }
    else
        index[tempbufidx].idlist.id = -1;

    index[tempbufidx].idlist.next = NULL;
    index[tempbufidx].idx_id = idx_id;
    index[tempbufidx].seek = -1;
    index[tempbufidx].str = &tempbuf[tempbuf_pos];
#ifdef UI_COLLATE
    index[tempbufidx].key = NULL;
#endif
    memcpy(index[tempbufidx].str, str, len);
    tempbuf_pos += len;
    tempbufidx++;

    return true;
}

static int compare(const void *p1, const void *p2)
{
    do_timed_yield();

    struct tempbuf_searchidx *e1 = (struct tempbuf_searchidx *)p1;
    struct tempbuf_searchidx *e2 = (struct tempbuf_searchidx *)p2;

    if (strcmp(e1->str, UNTAGGED) == 0)
    {
        if (strcmp(e2->str, UNTAGGED) == 0)
            return 0;
        return -1;
    }
    else if (strcmp(e2->str, UNTAGGED) == 0)
        return 1;

#ifdef UI_COLLATE
    {
        /* Sort the tag files in the 7G list order (ui_groupkey, #47), so the
         * shell's lists come out of the RAM cache already sorted: the
         * sort is paid here, in the background, not when a list opens.
         * Keys are precomputed (ui_precompute_keys); one that did not fit
         * in the commit buffer is made here. */
        char k1[UI_SORTKEY_LEN], k2[UI_SORTKEY_LEN];
        const char *a = e1->key, *b = e2->key;
        int r;

        if (!a)
        {
            ui_groupkey(e1->str, k1, sizeof(k1));
            a = k1;
        }
        if (!b)
        {
            ui_groupkey(e2->str, k2, sizeof(k2));
            b = k2;
        }
        r = strcmp(a, b);
        if (r)
            return r;
        /* #47: the shell's group order (library.c cmp_groups()): the
         * group key, then the case-folded name */
        return ui_casefold_cmp(e1->str, e2->str);
    }
#else
    return strncasecmp(e1->str, e2->str, TAG_MAXLEN);
#endif
}

#ifdef UI_TIMING
static long ui_sort_ticks;  /* time in tempbuf_sort() (DEBUGF timing) */
#endif
#ifdef UI_COLLATE
/* Computes every entry's sort key once, into the free part of the commit
 * buffer (sized for it by ui_commit_need()), instead of twice per
 * comparison. Entries whose key does not fit keep key == NULL. */
static void ui_precompute_keys(struct tempbuf_searchidx *index, int count)
{
    char key[UI_SORTKEY_LEN];
    int i, n = 0;

    for (i = 0; i < count; i++)
    {
        int len = ui_groupkey(index[i].str, key, sizeof(key)) + 1;

        if (tempbuf_left - 4 - len < 0)
            break;
        index[i].key = &tempbuf[tempbuf_pos];
        memcpy(index[i].key, key, len);
        tempbuf_pos += len;
        tempbuf_left -= len;
        n++;
        if ((i & 255) == 0)
            do_timed_yield();
    }
    if (n < count)
        logf("tagcache: %d of %d sort keys precomputed", n, count);
}
#endif

static int tempbuf_sort(int fd)
{
    struct tempbuf_searchidx *index = (struct tempbuf_searchidx *)tempbuf;
    struct tagfile_entry fe;
    int i;
    int length;

    /* Generate reverse lookup entries. */
    for (i = 0; i < lookup_buffer_depth; i++)
    {
        struct tempbuf_id_list *idlist;

        if (!lookup[i])
            continue;

        if (lookup[i]->idlist.id == i)
            continue;

        idlist = &lookup[i]->idlist;
        while (idlist->next != NULL)
            idlist = idlist->next;

        ALIGN_BUFFER(tempbuf_pos, tempbuf_left, alignof(struct tempbuf_id_list));
        tempbuf_left -= sizeof(struct tempbuf_id_list);
        if (tempbuf_left < 0)
            return -1;

        idlist->next = (struct tempbuf_id_list *)&tempbuf[tempbuf_pos];
        tempbuf_pos += sizeof(struct tempbuf_id_list);

        idlist = idlist->next;
        idlist->id = i;
        idlist->next = NULL;

        do_timed_yield();
    }

#ifdef UI_TIMING
    long sort_start = current_tick;
#endif
#ifdef UI_COLLATE
    ui_precompute_keys(index, tempbufidx);
#endif
    qsort(index, tempbufidx, sizeof(struct tempbuf_searchidx), compare);
#ifdef UI_TIMING
    ui_sort_ticks += current_tick - sort_start;
#endif
    memset(lookup, 0, lookup_buffer_depth * sizeof(struct tempbuf_searchidx **));

    for (i = 0; i < tempbufidx; i++)
    {
        struct tempbuf_id_list *idlist = &index[i].idlist;

        /* Fix the lookup list. */
        while (idlist != NULL)
        {
            if (idlist->id >= 0)
                lookup[idlist->id] = &index[i];
            idlist = idlist->next;
        }

        index[i].seek = lseek(fd, 0, SEEK_CUR);
        length = strlen(index[i].str) + 1;
        fe.tag_length = length;
        fe.idx_id = index[i].idx_id;

        /* Check the chunk alignment. */
        if ((fe.tag_length + sizeof(struct tagfile_entry))
            % TAGFILE_ENTRY_CHUNK_LENGTH)
        {
            fe.tag_length += TAGFILE_ENTRY_CHUNK_LENGTH -
                ((fe.tag_length + sizeof(struct tagfile_entry))
                 % TAGFILE_ENTRY_CHUNK_LENGTH);
        }

        if (write_tagfile_entry(TC_CTX_ARG(ui_build) fd, &fe) != sizeof(struct tagfile_entry))
        {
            logf("tempbuf_sort: write error #1");
            return -1;
        }

        if (write(fd, index[i].str, length) != length)
        {
            logf("tempbuf_sort: write error #2");
            return -2;
        }

        /* Write some padding. */
        if (fe.tag_length - length > 0)
            write(fd, "XXXXXXXX", fe.tag_length - length);
    }

    return i;
}

inline static struct tempbuf_searchidx* tempbuf_locate(int id)
{
    if (id < 0 || id >= lookup_buffer_depth)
        return NULL;

    return lookup[id];
}


inline static int tempbuf_find_location(int id)
{
    struct tempbuf_searchidx *entry;

    entry = tempbuf_locate(id);
    if (entry == NULL)
        return -1;

    return entry->seek;
}

static bool build_numeric_indices(struct tagcache_header *h, int tmpfd)
{
    struct master_header tcmh;
    struct index_entry idx;
    int masterfd;
    int masterfd_pos;
    struct temp_file_entry *entrybuf = (struct temp_file_entry *)tempbuf;
    int max_entries;
    int entries_processed = 0;
    int i, j;

    max_entries = tempbuf_size / sizeof(struct temp_file_entry) - 1;

    logf("Building numeric indices...");
    lseek(tmpfd, sizeof(struct tagcache_header), SEEK_SET);

    if ( (masterfd = open_master_fd(TC_CTX_ARG(ui_build) &tcmh, true)) < 0)
        return false;

    masterfd_pos = lseek(masterfd, tcmh.tch.entry_count * sizeof(struct index_entry),
                         SEEK_CUR);
    if (masterfd_pos < 0)
    {
        logf("we can't append!");
        close(masterfd);
        return false;
    }

    while (entries_processed < h->entry_count && !USR_CANCEL)
    {
        int count = MIN(h->entry_count - entries_processed, max_entries);

        /* Read in as many entries as possible. */
        for (i = 0; i < count; i++)
        {
            struct temp_file_entry *tfe = &entrybuf[i];
            int datastart;

            /* Read in numeric data. */
            if (read(tmpfd, tfe, sizeof(struct temp_file_entry)) !=
                sizeof(struct temp_file_entry))
            {
                logf("read fail #1");
                close(masterfd);
                return false;
            }

            datastart = lseek(tmpfd, 0, SEEK_CUR);

            /**
             * Read string data from the following tags:
             * - tag_filename
             * - tag_artist
             * - tag_album
             * - tag_title
             *
             * A crc32 hash is calculated from the read data
             * and stored back to the data offset field kept in memory.
             */
#define tmpdb_read_string_tag(tag) \
    lseek(tmpfd, tfe->tag_offset[tag], SEEK_CUR); \
    if ((unsigned long)tfe->tag_length[tag] > (unsigned long)build_idx_bufsz) \
    { \
        logf("read fail: buffer overflow"); \
        close(masterfd); \
        return false; \
    } \
    \
    if (read(tmpfd, build_idx_buf, tfe->tag_length[tag]) != \
        tfe->tag_length[tag]) \
    { \
        logf("read fail #2"); \
        close(masterfd); \
        return false; \
    } \
    str_setlen(build_idx_buf, tfe->tag_length[tag]); \
    \
    tfe->tag_offset[tag] = crc_32(build_idx_buf, strlen(build_idx_buf), 0xffffffff); \
    lseek(tmpfd, datastart, SEEK_SET)

            tmpdb_read_string_tag(tag_filename);
            tmpdb_read_string_tag(tag_artist);
            tmpdb_read_string_tag(tag_album);
            tmpdb_read_string_tag(tag_title);

            /* Seek to the end of the string data. */
            lseek(tmpfd, tfe->data_length, SEEK_CUR);
        }

        /* Backup the master index position. */
        masterfd_pos = lseek(masterfd, 0, SEEK_CUR);
        lseek(masterfd, sizeof(struct master_header), SEEK_SET);

        /* Check if we can resurrect some deleted runtime statistics data. */
        for (i = 0; i < tcmh.tch.entry_count && !USR_CANCEL; i++)
        {
            /* Read the index entry. */
            if (read_index_entries(TC_CTX_ARG(ui_build) masterfd, &idx, 1) != sizeof(struct index_entry))
            {
                logf("read fail #3");
                close(masterfd);
                return false;
            }

            /**
             * Skip unless the entry is marked as being deleted
             * or the data has already been resurrected.
             */
            if (!(idx.flag & FLAG_DELETED) || (idx.flag & FLAG_RESURRECTED))
                continue;

            /* Now try to match the entry. */
            /**
             * To succesfully match a song, the following conditions
             * must apply:
             *
             * For numeric fields: tag_length
             * - Full identical match is required
             *
             * If tag_filename matches, no further checking necessary.
             *
             * For string hashes: tag_artist, tag_album, tag_title
             * - All three of these must match
             */
            for (j = 0; j < count; j++)
            {
                struct temp_file_entry *tfe = &entrybuf[j];

                /* Try to match numeric fields first. */
                if (tfe->tag_offset[tag_length] != idx.tag_seek[tag_length])
                    continue;

                /* Now it's time to do the hash matching. */
                if (tfe->tag_offset[tag_filename] != idx.tag_seek[tag_filename])
                {
                    int match_count = 0;

                    /* No filename match, check if we can match two other tags. */
#define tmpdb_match(tag) \
    if (tfe->tag_offset[tag] == idx.tag_seek[tag]) \
        match_count++

                    tmpdb_match(tag_artist);
                    tmpdb_match(tag_album);
                    tmpdb_match(tag_title);

                    if (match_count < 3)
                    {
                        /* Still no match found, give up. */
                        continue;
                    }
                }

                /* A match found, now copy & resurrect the statistical data. */
#define tmpdb_copy_tag(tag) \
    tfe->tag_offset[tag] = idx.tag_seek[tag]

                tmpdb_copy_tag(tag_playcount);
                tmpdb_copy_tag(tag_rating);
                tmpdb_copy_tag(tag_playtime);
                tmpdb_copy_tag(tag_lastplayed);
                tmpdb_copy_tag(tag_commitid);
                tmpdb_copy_tag(tag_lastelapsed);
                tmpdb_copy_tag(tag_lastoffset);

                /* Avoid processing this entry again. */
                idx.flag |= FLAG_RESURRECTED;

                lseek(masterfd, -(off_t)sizeof(struct index_entry), SEEK_CUR);
                if (write_index_entries(TC_CTX_ARG(ui_build) masterfd, &idx, 1) != sizeof(struct index_entry))
                {
                    logf("masterfd writeback fail #1");
                    close(masterfd);
                    return false;
                }

                logf("Entry resurrected");
            }
        }


        /* Restore the master index position. */
        lseek(masterfd, masterfd_pos, SEEK_SET);

        /* Commit the data to the index. */
        for (i = 0; i < count && !USR_CANCEL; i++)
        {
            int loc = lseek(masterfd, 0, SEEK_CUR);

            if (read_index_entries(TC_CTX_ARG(ui_build) masterfd, &idx, 1) != sizeof(struct index_entry))
            {
                logf("read fail #3");
                close(masterfd);
                return false;
            }

            for (j = 0; j < TAG_COUNT; j++)
            {
                if (!TAGCACHE_IS_NUMERIC(j))
                    continue;

                idx.tag_seek[j] = entrybuf[i].tag_offset[j];
            }
            idx.flag = entrybuf[i].flag;

            if (idx.tag_seek[tag_commitid])
            {
                /* Data has been resurrected. */
                idx.flag |= FLAG_DIRTYNUM;
            }
            else if (tc_stat.ready && current_tcmh.commitid > 0)
            {
                idx.tag_seek[tag_commitid] = current_tcmh.commitid;
                idx.flag |= FLAG_DIRTYNUM;
            }

            /* Write back the updated index. */
            lseek(masterfd, loc, SEEK_SET);
            if (write_index_entries(TC_CTX_ARG(ui_build) masterfd, &idx, 1) != sizeof(struct index_entry))
            {
                logf("write fail");
                close(masterfd);
                return false;
            }
        }

        entries_processed += count;
        logf("%d/%" PRId32 " entries processed", entries_processed, h->entry_count);
    }

    close(masterfd);

    return true;
}

/**
 * Return values:
 *     > 0   success
 *    == 0   temporary failure
 *     < 0   fatal error
 */
static int build_index(int index_type, struct tagcache_header *h, int tmpfd)
{
    int i;
    struct tagcache_header tch;
    struct master_header   tcmh;
    struct index_entry idxbuf[IDX_BUF_DEPTH];
    int idxbuf_pos;
    int fd = -1, masterfd;
    bool error = false;
    int init;
    int masterfd_pos;

    logf("Building index: %d", index_type);

    /* Check the number of entries we need to allocate ram for. */
    commit_entry_count = h->entry_count + 1;

    masterfd = open_master_fd(TC_CTX_ARG(ui_build) &tcmh, false);
    if (masterfd >= 0)
    {
        commit_entry_count += tcmh.tch.entry_count;
        close(masterfd);
    }
    else
        remove_files(); /* Just to be sure we are clean. */

    /* Open the index file, which contains the tag names. */
    fd = open_tag_fd(TC_CTX_ARG(ui_build) &tch, index_type, true);
    if (fd >= 0)
    {
        logf("tch.datasize=%" PRId32, tch.datasize);
        lookup_buffer_depth = 1 +
        /* First part */ commit_entry_count +
        /* Second part */ (tch.datasize / TAGFILE_ENTRY_CHUNK_LENGTH);
    }
    else
    {
        lookup_buffer_depth = 1 +
        /* First part */ commit_entry_count +
        /* Second part */ 0;
    }

    logf("lookup_buffer_depth=%ld", lookup_buffer_depth);
    logf("commit_entry_count=%ld", commit_entry_count);

    /* Allocate buffer for all index entries from both old and new
     * tag files. */
    tempbufidx = 0;
    tempbuf_pos = commit_entry_count * sizeof(struct tempbuf_searchidx);

    /* Allocate lookup buffer. The first portion of commit_entry_count
     * contains the new tags in the temporary file and the second
     * part for locating entries already in the db.
     *
     *  New tags  Old tags
     * +---------+---------------------------+
     * |  index  | position/ENTRY_CHUNK_SIZE |  lookup buffer
     * +---------+---------------------------+
     *
     * Old tags are inserted to a temporary buffer with position:
     *     tempbuf_insert(position/ENTRY_CHUNK_SIZE, ...);
     * And new tags with index:
     *     tempbuf_insert(idx, ...);
     *
     * The buffer is sorted and written into tag file:
     *     tempbuf_sort(...);
     * leaving master index locations messed up.
     *
     * That is fixed using the lookup buffer for old tags:
     *     new_seek = tempbuf_find_location(old_seek, ...);
     * and for new tags:
     *     new_seek = tempbuf_find_location(idx);
     */
    lookup = (struct tempbuf_searchidx **)&tempbuf[tempbuf_pos];
    tempbuf_pos += lookup_buffer_depth * sizeof(void **);
    memset(lookup, 0, lookup_buffer_depth * sizeof(void **));

    /* And calculate the remaining data space used mainly for storing
     * tag data (strings). */
    tempbuf_left = tempbuf_size - tempbuf_pos - 8;
    if (tempbuf_left - TAGFILE_ENTRY_AVG_LENGTH * commit_entry_count < 0)
    {
        logf("Buffer way too small!");
        close(fd);
        return 0;
    }

    if (fd >= 0)
    {
        /**
         * If tag file contains unique tags (sorted index), we will load
         * it entirely into memory so we can resort it later for use with
         * chunked browsing.
         */
        if (TAGCACHE_IS_SORTED(index_type))
        {
            logf("loading tags...");
            for (i = 0; i < tch.entry_count && !USR_CANCEL; i++)
            {
                struct tagfile_entry entry;
                int loc = lseek(fd, 0, SEEK_CUR);
                bool ret;
                switch (read_tagfile_entry_and_tag(TC_CTX_ARG(ui_build) fd, &entry, build_idx_buf, build_idx_bufsz))
                {
                    case e_SUCCESS_LEN_ZERO: /* Skip deleted entries. */
                        continue;
                    case e_SUCCESS:
                         break;
                    case e_ENTRY_SIZEMISMATCH:
                        logf("read error #7");
                        close(fd);
                        return -2;
                    case e_TAG_TOOLONG:
                        logf("too long tag #3");
                        close(fd);
                        return -2;
                    case e_TAG_SIZEMISMATCH:
                        logf("read error #8");
                        close(fd);
                        return -2;
                }

                /**
                 * Save the tag and tag id in the memory buffer. Tag id
                 * is saved so we can later reindex the master lookup
                 * table when the index gets resorted.
                 */
                ret = tempbuf_insert(build_idx_buf, loc/TAGFILE_ENTRY_CHUNK_LENGTH
                                     + commit_entry_count, entry.idx_id,
                                     TAGCACHE_IS_UNIQUE(index_type));
                if (!ret)
                {
                    close(fd);
                    return -3;
                }
                do_timed_yield();
            }
            logf("done");
        }
        else
            tempbufidx = tch.entry_count;
    }
    else
    {
        logf("Create New Index: %d", index_type);
        /**
         * Creating new index file to store the tags. No need to preload
         * anything whether the index type is sorted or not.
         *
         * Note: although we are creating a file under the db path, it must
         * already exist by this point so no mkdir is required.
         */
        fd = open_pathfmt(build_idx_buf, build_idx_bufsz,
                          O_WRONLY | O_CREAT | O_TRUNC,
                          "%s/" TAGCACHE_FILE_INDEX,
                          tc_stat.db_path, index_type);
        if (fd < 0)
        {
            logf(TAGCACHE_FILE_INDEX " open fail", index_type);
            return -2;
        }

        tch.magic = TAGCACHE_MAGIC;
        tch.entry_count = 0;
        tch.datasize = 0;

        if (write_tagcache_header(TC_CTX_ARG(ui_build) fd, &tch) != sizeof(struct tagcache_header))
        {
            logf("header write failed");
            close(fd);
            return -2;
        }
    }

    /* Loading the tag lookup file as "master file". */
    logf("Loading index file");
    masterfd = open_db_fd(TC_CTX_ARG(ui_build) TAGCACHE_FILE_MASTER, O_RDWR);

    if (masterfd < 0)
    {
        logf("Creating new DB");
        masterfd = open_db_fd(TC_CTX_ARG(ui_build) TAGCACHE_FILE_MASTER, O_WRONLY | O_CREAT | O_TRUNC);

        if (masterfd < 0)
        {
            logf("Failure to create index file (%s)", TAGCACHE_FILE_MASTER);
            close(fd);
            return -2;
        }

        /* Write the header (write real values later). */
        memset(&tcmh, 0, sizeof(struct master_header));
#ifdef UI_COMMIT
        tcmh.commitid = current_tcmh.commitid;  /* monotonic (M-2) */
#endif
        tcmh.tch = *h;
        tcmh.tch.entry_count = 0;
        tcmh.tch.datasize = 0;
        tcmh.dirty = true;
        write_master_header(TC_CTX_ARG(ui_build) masterfd, &tcmh);
        init = true;
        masterfd_pos = lseek(masterfd, 0, SEEK_CUR);
    }
    else
    {
        /**
         * Master file already exists so we need to process the current
         * file first.
         */
        init = false;

        if (read_master_header(TC_CTX_ARG(ui_build) masterfd, &tcmh) != sizeof(struct master_header) ||
            tcmh.tch.magic != TAGCACHE_MAGIC)
        {
            logf("header error");
            close(fd);
            close(masterfd);
            return -2;
        }

        /**
         * If we reach end of the master file, we need to expand it to
         * hold new tags. If the current index is not sorted, we can
         * simply append new data to end of the file.
         * However, if the index is sorted, we need to update all tag
         * pointers in the master file for the current index.
         */
        masterfd_pos = lseek(masterfd, tcmh.tch.entry_count * sizeof(struct index_entry),
            SEEK_CUR);
        if (masterfd_pos == ffilesize(masterfd))
        {
            logf("appending...");
            init = true;
        }
    }

    /**
     * Load new unique tags in memory to be sorted later and added
     * to the master lookup file.
     */
    if (TAGCACHE_IS_SORTED(index_type))
    {
        lseek(tmpfd, sizeof(struct tagcache_header), SEEK_SET);
        /* h is the header of the temporary file containing new tags. */
        logf("inserting new tags...");
        for (i = 0; i < h->entry_count && !USR_CANCEL; i++)
        {
            struct temp_file_entry entry;

            if (read(tmpfd, &entry, sizeof(struct temp_file_entry)) !=
                sizeof(struct temp_file_entry))
            {
                logf("read fail #3");
                error = true;
                goto error_exit;
            }

            /* Read data. */
            if (entry.tag_length[index_type] >= build_idx_bufsz)
            {
                logf("too long entry!");
                error = true;
                goto error_exit;
            }

            lseek(tmpfd, entry.tag_offset[index_type], SEEK_CUR);
            if (read(tmpfd, build_idx_buf, entry.tag_length[index_type]) !=
                entry.tag_length[index_type])
            {
                logf("read fail #4");
                error = true;
                goto error_exit;
            }
            str_setlen(build_idx_buf, entry.tag_length[index_type]);

#if defined(PLUGIN)
            if (user_check_tag(index_type, build_idx_buf))
#endif /*defined(PLUGIN)*/
            {
                if (TAGCACHE_IS_UNIQUE(index_type))
                    error = !tempbuf_insert(build_idx_buf, i, -1, true);
                else
                    error = !tempbuf_insert(build_idx_buf, i,
                                            tcmh.tch.entry_count + i, false);

                if (error)
                {
                    logf("insert error");
                    goto error_exit;
                }
            }
            /* Skip to next. */
            lseek(tmpfd, entry.data_length - entry.tag_offset[index_type] -
                    entry.tag_length[index_type], SEEK_CUR);
            do_timed_yield();
        }
        logf("done");

        /* Sort the buffer data and write it to the index file. */
        lseek(fd, sizeof(struct tagcache_header), SEEK_SET);
        /**
         * We need to truncate the index file now. There can be junk left
         * at the end of file (however, we _should_ always follow the
         * entry_count and don't crash with that).
         */
        ftruncate(fd, lseek(fd, 0, SEEK_CUR));

        i = tempbuf_sort(fd);
        if (i < 0)
            goto error_exit;
        logf("sorted %d tags", i);

        /**
         * Now update all indexes in the master lookup file.
         */
        logf("updating indices...");
        lseek(masterfd, sizeof(struct master_header), SEEK_SET);
        for (i = 0; i < tcmh.tch.entry_count && !USR_CANCEL; i += idxbuf_pos)
        {
            int j;
            int loc = lseek(masterfd, 0, SEEK_CUR);

            idxbuf_pos = MIN(tcmh.tch.entry_count - i, IDX_BUF_DEPTH);

            if (read_index_entries(TC_CTX_ARG(ui_build) masterfd, idxbuf, idxbuf_pos) !=
                (ssize_t)sizeof(struct index_entry) * idxbuf_pos)
            {
                logf("read fail #5");
                error = true;
                goto error_exit ;
            }
            lseek(masterfd, loc, SEEK_SET);

            for (j = 0; j < idxbuf_pos; j++)
            {
                if (idxbuf[j].flag & FLAG_DELETED)
                {
                    /* We can just ignore deleted entries. */
                    // idxbuf[j].tag_seek[index_type] = 0;
                    continue;
                }

                idxbuf[j].tag_seek[index_type] = tempbuf_find_location(
                    idxbuf[j].tag_seek[index_type]/TAGFILE_ENTRY_CHUNK_LENGTH
                    + commit_entry_count);

                if (idxbuf[j].tag_seek[index_type] < 0)
                {
                    logf("update error: %" PRId32 "/%d/%" PRId32,
                         idxbuf[j].flag, i+j, tcmh.tch.entry_count);
                    error = true;
                    goto error_exit;
                }

                do_timed_yield();
            }

            /* Write back the updated index. */
            if (write_index_entries(TC_CTX_ARG(ui_build) masterfd, idxbuf, idxbuf_pos) !=
                (ssize_t)sizeof(struct index_entry) * idxbuf_pos)
            {
                logf("write fail");
                error = true;
                goto error_exit;
            }
        }
        logf("done");
    }

    /**
     * Walk through the temporary file containing the new tags.
     */
    // build_normal_index(h, tmpfd, masterfd, idx);
    logf("updating new indices...");
    lseek(masterfd, masterfd_pos, SEEK_SET);
    lseek(tmpfd, sizeof(struct tagcache_header), SEEK_SET);
    lseek(fd, 0, SEEK_END);
    for (i = 0; i < h->entry_count && !USR_CANCEL; i += idxbuf_pos)
    {
        int j;

        idxbuf_pos = MIN(h->entry_count - i, IDX_BUF_DEPTH);
        if (init)
        {
            memset(idxbuf, 0, sizeof(struct index_entry)*IDX_BUF_DEPTH);
        }
        else
        {
            int loc = lseek(masterfd, 0, SEEK_CUR);

            if (read_index_entries(TC_CTX_ARG(ui_build) masterfd, idxbuf, idxbuf_pos) !=
                (ssize_t)sizeof(struct index_entry) * idxbuf_pos)
            {
                logf("read fail #6");
                error = true;
                break ;
            }
            lseek(masterfd, loc, SEEK_SET);
        }

        /* Read entry headers. */
        for (j = 0; j < idxbuf_pos; j++)
        {
            if (!TAGCACHE_IS_SORTED(index_type))
            {
                struct temp_file_entry entry;
                struct tagfile_entry fe;

                if (read(tmpfd, &entry, sizeof(struct temp_file_entry)) !=
                    sizeof(struct temp_file_entry))
                {
                    logf("read fail #7");
                    error = true;
                    break ;
                }

                /* Read data. */
                if (entry.tag_length[index_type] >= build_idx_bufsz)
                {
                    logf("too long entry!");
                    logf("length=%d", entry.tag_length[index_type]);
                    logf("pos=0x%02lx", (unsigned long) lseek(tmpfd, 0, SEEK_CUR));
                    error = true;
                    break ;
                }

                lseek(tmpfd, entry.tag_offset[index_type], SEEK_CUR);
                if (read(tmpfd, build_idx_buf, entry.tag_length[index_type]) !=
                    entry.tag_length[index_type])
                {
                    logf("read fail #8");
                    logf("offset=0x%02" PRIx32, entry.tag_offset[index_type]);
                    logf("length=0x%02x", entry.tag_length[index_type]);
                    error = true;
                    break ;
                }

                /* Write to index file. */
                idxbuf[j].tag_seek[index_type] = lseek(fd, 0, SEEK_CUR);
                fe.tag_length = entry.tag_length[index_type];
                fe.idx_id = tcmh.tch.entry_count + i + j;
                write_tagfile_entry(TC_CTX_ARG(ui_build) fd, &fe);
                write(fd, build_idx_buf, fe.tag_length);
                tempbufidx++;

                /* Skip to next. */
                lseek(tmpfd, entry.data_length - entry.tag_offset[index_type] -
                      entry.tag_length[index_type], SEEK_CUR);
            }
            else
            {
                /* Locate the correct entry from the sorted array. */
                idxbuf[j].tag_seek[index_type] = tempbuf_find_location(i + j);
                if (idxbuf[j].tag_seek[index_type] < 0)
                {
                    logf("entry not found (%d)", j);
                    error = true;
                    break ;
                }
            }
        }

        /* Write index. */
        if (write_index_entries(TC_CTX_ARG(ui_build) masterfd, idxbuf, idxbuf_pos) !=
            (ssize_t)sizeof(struct index_entry) * idxbuf_pos)
        {
            logf("tagcache: write fail #4");
            error = true;
            break ;
        }

        do_timed_yield();
    }
    logf("done");

    /* Finally write the header. */
    tch.magic = TAGCACHE_MAGIC;
    tch.entry_count = tempbufidx;
    tch.datasize = lseek(fd, 0, SEEK_END) - sizeof(struct tagcache_header);
    lseek(fd, 0, SEEK_SET);
    write_tagcache_header(TC_CTX_ARG(ui_build) fd, &tch);

    if (index_type != tag_filename)
        h->datasize += tch.datasize;
    logf("s:%d/%" PRId32 "/%" PRId32, index_type, tch.datasize, h->datasize);
    error_exit:

    close(fd);
    close(masterfd);

    if (error)
        return -2;

    return 1;
}

#ifdef UI_COMMIT
/* Adds up the new string data of each tag in the temp file (entry headers
 * only; the strings are skipped). False if the file can't be read. */
static bool ui_temp_tag_sizes(long count, size_t *sizes)
{
    struct tagcache_header h;
    struct temp_file_entry e;
    bool ok = true;
    long i;
    int fd = open_db_fd(TC_CTX_ARG(ui_build) TAGCACHE_FILE_TEMP, O_RDONLY);

    if (fd < 0)
        return false;
    memset(sizes, 0, TAG_COUNT * sizeof(*sizes));
    if (read(fd, &h, sizeof(h)) != sizeof(h))
        ok = false;
    for (i = 0; ok && i < count; i++)
    {
        if (read(fd, &e, sizeof(e)) != sizeof(e)
            || lseek(fd, e.data_length, SEEK_CUR) < 0)
        {
            ok = false;
            break;
        }
        for (int tag = 0; tag < TAG_COUNT; tag++)
            if (!TAGCACHE_IS_NUMERIC(tag) && e.tag_length[tag] > 0)
                sizes[tag] += e.tag_length[tag];
        if ((i & 255) == 0)
            do_timed_yield();
    }
    close(fd);
    return ok;
}

/* Upper bound of the commit buffer build_index() needs for the largest tag
 * (index + lookup table + strings, their CRCs and id lists, and the sort
 * keys), with 'tmph' the temp file header, and in *reload the RAM cache
 * the database needs once committed (allocate_tagcache()). Reads only the
 * headers. */
static size_t ui_commit_need(const struct tagcache_header *tmph,
                             size_t *reload)
{
    struct master_header mh;
    struct tagcache_header th;
    size_t n = tmph->entry_count + 1;
    size_t old_size = sizeof(struct master_header);
    size_t need = 0, newstr = 0;
    size_t sizes[TAG_COUNT];
    bool have_sizes = ui_temp_tag_sizes(tmph->entry_count, sizes);
    int fd, tag;

    fd = open_master_fd(TC_CTX_ARG(ui_build) &mh, false);
    if (fd >= 0)
    {
        n += mh.tch.entry_count;
        old_size = mh.tch.datasize;
        close(fd);
    }

    for (tag = 0; tag < TAG_COUNT; tag++)
    {
        size_t ds = 0, t;

        if (TAGCACHE_IS_NUMERIC(tag))
            continue;
        fd = open_tag_fd(TC_CTX_ARG(ui_build) &th, tag, false);
        if (fd >= 0)
        {
            ds = th.datasize;
            close(fd);
        }
        t = n * sizeof(struct tempbuf_searchidx)
            + (1 + n + ds / TAGFILE_ENTRY_CHUNK_LENGTH) * sizeof(void *) + 8;
        if (TAGCACHE_IS_SORTED(tag))
        {
            /* old strings plus the new ones, then as much again for the
             * keys (a key is at most its string + 1) */
            size_t str = ds + (have_sizes ? sizes[tag]
                                           : (size_t)tmph->datasize);
            t += MAX(2 * str + n * (4 + 2 + sizeof(struct tempbuf_id_list)),
                     n * TAGFILE_ENTRY_AVG_LENGTH);
        }
        else
            t += n * TAGFILE_ENTRY_AVG_LENGTH;
        need = MAX(need, t);
    }

    /* the RAM cache holds every tag's strings but the filenames' */
    for (tag = 0; tag < TAG_COUNT; tag++)
        if (!TAGCACHE_IS_NUMERIC(tag) && tag != tag_filename)
            newstr += have_sizes ? sizes[tag] + (size_t)tmph->entry_count
                           * (sizeof(struct tagfile_entry) + 3) : 0;
    if (!have_sizes)
        newstr = tmph->datasize;
    *reload = old_size + tmph->entry_count * sizeof(struct index_entry)
        + newstr + 256 + TAGCACHE_RESERVE
        + sizeof(struct ramcache_header) + TAG_COUNT * sizeof(void *);
#ifdef HAVE_DIRCACHE
    *reload += n * sizeof(struct dircache_fileref);
#endif
    /* and the shell's view pool grows with the new entries (pool_sync()) */
    *reload += tmph->entry_count * TAGCACHE_UI_VIEW_BYTES;
    return ALIGN_UP(need + 1024, 16);
}

/* Sets up the commit buffer: the RAM cache (dropped by the commit anyway)
 * if it is big enough, else a bounded allocation. Never shrinks the audio
 * buffer while audio is playing or paused (playback.c gives it up for
 * good when it is asked to): then the commit waits, and false is
 * returned. *allocated says whether free_tempbuf() must release it. */
static bool ui_commit_buffer(const struct tagcache_header *tmph,
                             bool *allocated)
{
    size_t reload, steal = 0;
    size_t need = ui_commit_need(tmph, &reload);
    size_t avail = core_allocatable();
    bool audio = audio_status() != 0;

    *allocated = false;
    if (ui_build != ui_active && tcramcache.handle > 0
        && tc_stat.ramcache_allocated
        > (int)(sizeof(struct ramcache_header) + 128))
    {
        steal = (tc_stat.ramcache_allocated - sizeof(struct ramcache_header)
                 - 128) & ~3;
    }
    DEBUGF("tagcache: commit of %ld entries needs %lu B (RAM cache %lu B, "
           "free %lu B, reload %lu B)%s\n", (long)tmph->entry_count,
           (unsigned long)need, (unsigned long)steal, (unsigned long)avail,
           (unsigned long)reload, audio ? ", audio on" : "");

    /* #88: paused, inside the window, one partial shrink makes room for
     * the commit's peak (its buffer, or the reload after it, and two
     * generation buffers), if the audio buffer stays above the floor */
    if (audio && (avail < reload || (steal < need && avail < need + 64)))
    {
        /* after the commit: the reload, the candidate's view pool
         * (lib_generation_pool_prepare(), ~12 B an entry + 64 KB) */
        size_t pool = (size_t)(current_tcmh.tch.entry_count + tmph->entry_count)
                      * 16 + 64 * 1024;
        size_t peak = MAX(steal < need ? need + 64 : 0, reload) + pool
                      + UI_MAINT_SLACK;

        ui_maint_need = peak;
        if (maint_take(MAINT_COMMIT, peak))
            avail = core_allocatable();
    }
    /* while audio runs, the reload after the commit must fit too */
    if (audio && (avail < reload
                  || (steal < need && avail < need + 64)))
    {
        logf("tagcache: commit waits for playback to stop");
        DEBUGF("tagcache: commit delayed until audio stops\n");
        ui_commit_pending = true;
        tc_stat.commit_delayed = true;
        return false;
    }

    if (steal >= need)
        return true;            /* commit() steals the RAM cache */

    if (audio_status() != 0 &&
        (core_allocatable() < reload || core_allocatable() < need + 64))
    {
        ui_commit_pending = true;
        tc_stat.commit_delayed = true;
        return false;
    }
    tempbuf_handle = core_alloc_ex(need, &buflib_ops_locked);
    if (tempbuf_handle > 0)
    {
        tempbuf = core_get_data(tempbuf_handle);
        tempbuf_size = need;
    }
    else if (audio_status() == 0)
    {
        allocate_tempbuf();     /* stopped: as upstream, take what's free */
    }
    else
    {
        /* playback started meanwhile: core_alloc_maximum() would take
         * its buffer for good */
        tempbuf_handle = 0;
        ui_commit_pending = true;
        tc_stat.commit_delayed = true;
        return false;
    }
    *allocated = tempbuf_size > 0;
    return true;
}

/* After a commit. A new database (remove_files() ran: a rebuild, the
 * first build) imports the changelog -- also before its RAM cache is
 * allocated, which upstream waits for, so a rebuild at boot lost the play
 * counts and resume points. The marker file keeps an import that a
 * power-off cuts short for the next boot. Statistics are exported before
 * a rebuild and at USB connect and shutdown, not after every commit. */
static void ui_after_commit(void)
{
    ui_ram_yielded = NULL;      /* P16: a new commit: its reload follows */
    if (!ui_db_fresh)
        return;
    ui_db_fresh = false;
    if (!ui_import_pending)     /* remove_files() marked it already */
    {
        int fd = open_db_fd(TC_CTX_ARG(ui_build) TAGCACHE_FILE_IMPORT,
                            O_WRONLY | O_CREAT | O_TRUNC);
        if (fd >= 0)
            close(fd);
        ui_import_pending = true;
    }
    queue_post(&tagcache_queue, Q_IMPORT_CHANGELOG, 0);
}

/* Immutable generations exclude only numeric runtime statistics. A database
 * manifest is not invalidated when a play count or resume point is saved. */
static const struct dbgen_file ui_generation_files[] = {
    { TAGCACHE_FILE_MASTER, false, true },
    { "database_0.tcd", true, true },
    { "database_1.tcd", true, true },
    { "database_2.tcd", true, true },
    { "database_3.tcd", true, true },
    { "database_4.tcd", true, true },
    { "database_5.tcd", true, true },
    { "database_6.tcd", true, true },
    { "database_7.tcd", true, true },
    { "database_8.tcd", true, true },
    { "database_12.tcd", true, true },
    { LIBFILE_NAME, true, false }
};

#define UI_GENERATION_BYTES (16 * 1024)
struct ui_generation_io { void *buffer; size_t size; };

/* Failing background I/O work is retried with exponential backoff (2 s
 * doubling to 5 min), never every second with full validation passes.
 * Success resets one; USB and new update/rebuild requests reset all. */
enum { UI_RETRY_SELECT, UI_RETRY_BEGIN, UI_RETRY_PUBLISH, UI_RETRY_LOAD,
       UI_RETRY_JOURNAL, UI_RETRY_REPLAY, UI_RETRY_COMPACT, UI_RETRY_YIELD,
       UI_RETRY_COUNT };
static struct { long next, delay; } ui_retry[UI_RETRY_COUNT];
static unsigned ui_retries;     /* total this boot: logs, simulator */
#ifdef SIMULATOR
static unsigned ui_prepares;    /* candidate prepare attempts: simulator */
#endif

static bool ui_retry_due(int which)
{
    return !ui_retry[which].delay
        || !TIME_BEFORE(current_tick, ui_retry[which].next);
}

static void ui_retry_later(int which, const char *what, int rc)
{
    long delay = ui_retry[which].delay;
    delay = delay ? MIN(delay * 2, 300 * HZ) : 2 * HZ;
    ui_retry[which].delay = delay;
    ui_retry[which].next = current_tick + delay;
    ui_retries++;
    ui_log("db", "%s rc=%d n=%u %lds", what, rc, ui_retries,
           delay / HZ);
}

static bool ui_generation_cancel(void *opaque)
{
    (void)opaque;
    do_timed_yield();
    if (ui_shutdown_flushing)
    {
        /* The shutdown's stop/poweroff events are not cancellation of its
         * own final checkpoint. USB ownership and the deadline still win. */
        return tagcache_usb_quiescing
            || !TIME_BEFORE(current_tick, ui_shutdown_flush_deadline)
            || queue_peek_ex(&tagcache_queue, NULL, 0,
                             QPEEK_FILTER1(SYS_USB_CONNECTED));
    }
    return check_event_queue() || ui_commit_cancel;
}

/* 0..5 for the six runtime statistics, -1 for any other tag */
static int ui_stat_slot(int tag)
{
    switch (tag)
    {
        case tag_playcount:   return 0;
        case tag_rating:      return 1;
        case tag_playtime:    return 2;
        case tag_lastplayed:  return 3;
        case tag_lastelapsed: return 4;
        case tag_lastoffset:  return 5;
        default:              return -1;
    }
}

static bool ui_runtime_tag(int tag)
{
    return ui_stat_slot(tag) >= 0;
}

static enum dbgen_result ui_generation_validate(const char *path,
    int32_t commitid, int32_t entries, uint32_t *structural_sum,
    int32_t *found_commitid, void *opaque)
{
    struct ui_generation_io *io = opaque;
    /* dbgen owns the first half, including its live manifest workspace.
     * Its validator must never overwrite that workspace through opaque. */
    void *buffer = (char *)io->buffer + io->size / 2;
    size_t buffer_size = io->size / 2;
    struct tagcache_context ctx;
    struct master_header mh;
    struct libfile_hdr lh;
    uint32_t sizes[TAG_COUNT];
    uint32_t sum = 0xffffffff;
    char name[MAX_PATH];
    int fd, tag;
    bool ok = true;

    memset(&ctx, 0, sizeof(ctx));
    strmemccpy(ctx.stat.db_path, path, sizeof(ctx.stat.db_path));
    fd = open_master_fd(&ctx, &mh, false);
    if (fd < 0)
        return fd == -2 ? DBGEN_CORRUPT
             : errno == ENOENT ? DBGEN_MISSING : DBGEN_IO;
    if (mh.dirty || mh.tch.entry_count < 0 || mh.tch.datasize < 0
        || (commitid >= 0 && mh.commitid != commitid)
        || (entries >= 0 && mh.tch.entry_count != entries)
        || ffilesize(fd) != (off_t)sizeof(mh)
              + (off_t)mh.tch.entry_count * (off_t)sizeof(struct index_entry))
        ok = false;
    sum = crc_32(&mh.tch, sizeof(mh.tch), sum);
    sum = crc_32(&mh.commitid, sizeof(mh.commitid), sum);
    for (tag = 0; ok && tag < TAG_COUNT; tag++)
    {
        struct tagcache_header th;
        int tf;
        if (TAGCACHE_IS_NUMERIC(tag))
            continue;
        tf = open_tag_fd(&ctx, &th, tag, false);
        if (tf < 0)
        {
            int err = errno;
            close(fd);
            return tf == -2 ? DBGEN_CORRUPT
                 : err == ENOENT ? DBGEN_MISSING : DBGEN_IO;
        }
        ok = th.datasize >= 0 && th.entry_count >= 0
            && ffilesize(tf) == (off_t)sizeof(th) + th.datasize;
        sizes[tag] = sizeof(th) + th.datasize;
        close(tf);
    }
    for (int32_t i = 0; ok && i < mh.tch.entry_count; )
    {
        struct index_entry *idx = buffer;
        size_t n = MIN(buffer_size / sizeof(*idx),
                       (size_t)(mh.tch.entry_count - i));
        if (read_index_entries(&ctx, fd, idx, n)
            != (ssize_t)(n * sizeof(*idx)))
        {
            close(fd);
            return DBGEN_IO;
        }
        for (size_t j = 0; ok && j < n; j++)
        {
            /* FOLDED: set by statistics after the stamp, like their values */
            int32_t flag = idx[j].flag
                & ~(FLAG_DIRCACHE | FLAG_DIRTYNUM | FLAG_UI_STATS_FOLDED);
            sum = crc_32(&flag, sizeof(flag), sum);
            for (tag = 0; tag < TAG_COUNT; tag++)
            {
                int32_t seek = idx[j].tag_seek[tag];
                if (!ui_runtime_tag(tag))
                    sum = crc_32(&seek, sizeof(seek), sum);
                if (!(flag & FLAG_DELETED) && !TAGCACHE_IS_NUMERIC(tag)
                    && (seek < (int32_t)sizeof(struct tagcache_header)
                        || (uint32_t)seek >= sizes[tag]))
                    ok = false;
            }
        }
        i += n;
        if (ui_generation_cancel(opaque))
        {
            close(fd);
            return DBGEN_CANCELLED;
        }
    }
    close(fd);
    if (!ok)
        return DBGEN_CORRUPT;
    snprintf(name, sizeof(name), "%s/%s", path, LIBFILE_NAME);
    fd = open(name, O_RDONLY);
    if (fd < 0)
        return errno == ENOENT ? DBGEN_MISSING : DBGEN_IO;
    off_t length = ffilesize(fd);
    ok = read(fd, &lh, sizeof(lh)) == sizeof(lh);
    if (!ok)
    {
        close(fd);
        return DBGEN_IO;
    }
    uint32_t saved = lh.sum;
    struct libfile_sum_state ls;
    lh.sum = 0;
    libfile_sum_init(&ls);
    libfile_sum_add(&ls, &lh, sizeof(lh));
    ok = lh.magic == LIBFILE_MAGIC && lh.version == LIBFILE_VERSION
        && lh.sortkey_version == SORTKEY_VERSION
        && lh.commitid == mh.commitid && lh.entries == mh.tch.entry_count
        && lh.nsec <= LIBFILE_MAX_SEC && libfile_sum_end(&ls) == saved;
    for (unsigned i = 0; ok && i < lh.nsec; i++)
    {
        const struct libfile_sec *sec = &lh.sec[i];
        uint64_t bytes = (uint64_t)sec->count * sec->elem;
        if (sec->offset < sizeof(lh) || sec->offset > (uint64_t)length
            || bytes > (uint64_t)length - sec->offset
            || lseek(fd, sec->offset, SEEK_SET) != (off_t)sec->offset)
        {
            ok = false;
            break;
        }
        libfile_sum_init(&ls);
        while (bytes)
        {
            size_t n = MIN(bytes, buffer_size);
            if (read(fd, buffer, n) != (ssize_t)n)
            {
                close(fd);
                return DBGEN_IO;
            }
            libfile_sum_add(&ls, buffer, n);
            bytes -= n;
            if (ui_generation_cancel(opaque))
            {
                close(fd);
                return DBGEN_CANCELLED;
            }
        }
        ok = libfile_sum_end(&ls) == sec->sum;
    }
    close(fd);
    *structural_sum = sum;
    *found_commitid = mh.commitid;
    return ok ? DBGEN_OK : DBGEN_CORRUPT;
}

static int ui_generation_buffer(struct ui_generation_io *io)
{
    /* Check immediately before allocation: no I/O/yield between the check
     * and allocation can allow playback to start and lose its buffer. */
    if (!ui_audio_room(UI_GENERATION_BYTES))
        return 0;
    int h = core_alloc_ex(UI_GENERATION_BYTES, &buflib_ops_locked);
    if (h > 0)
    {
        io->buffer = core_get_data(h);
        io->size = UI_GENERATION_BYTES;
    }
    return h;
}

static struct dbgen_ops ui_generation_ops(struct ui_generation_io *io)
{
    struct dbgen_ops ops = {
        ui_generation_files, ARRAYLEN(ui_generation_files),
        ui_generation_validate, ui_generation_cancel, io
    };
    return ops;
}

/* the last ui_candidate_begin() failed only for audio's memory */
static bool ui_begin_mem_wait;

static bool ui_candidate_begin(void)
{
    struct tagcache_context *next;
    struct ui_generation_io io;
    char path[DBGEN_PATH_MAX];
    int h;
    enum dbgen_result rc;

    ui_begin_mem_wait = false;
    if (ui_generation_unresolved)
        return false;
    if (ui_build != ui_active)
        return true;
    if (!ui_retry_due(UI_RETRY_BEGIN))
        return false;
    /* #109 (4.2.3-4): a database queue's rebase or materialization still
     * needs the retained generation (or dbq.hdr isn't loaded yet): the
     * next one waits */
    if (dbq_prepare_wait())
    {
        ui_retry_later(UI_RETRY_BEGIN, "dbq owed", -1);
        return false;
    }
    next = ui_active == &ui_context_a ? &ui_context_b : &ui_context_a;
    int level = disable_irq_save();
    bool unavailable = next->readers || next->operations || next->reserved
        || next->writer_count || next->reader_count;
    if (!unavailable)
        next->reserved = true;
    restore_irq(level);
    if (unavailable)
        return false;
    lib_generation_pool_abandon(next);
    h = ui_generation_buffer(&io);
    if (h <= 0)
    {
        ui_begin_mem_wait = true;
        next->reserved = false;
        return false;
    }
    struct dbgen_ops ops = ui_generation_ops(&io);
    bool fresh = ui_generation_fresh;
#ifdef SIMULATOR
    ui_prepares++;
#endif
    if (fresh)
        rc = dbgen_prepare_empty(ui_db_root, ui_active->stat.db_path, path,
                                 io.buffer, io.size / 2, &ops);
    else
        rc = dbgen_prepare(ui_db_root, ui_active->stat.db_path, path,
                           io.buffer, io.size / 2, &ops);
    if (rc == DBGEN_MISSING && !ui_active->stat.ramcache
        && !ui_active->stat.ready)
    {
        /* Partial legacy files are not a clonable database. Preserve them
         * and construct an empty, unreferenced candidate, never the root. */
        rc = dbgen_prepare_empty(ui_db_root, ui_active->stat.db_path, path,
                                 io.buffer, io.size / 2, &ops);
        fresh = rc == DBGEN_OK;
    }
    core_free(h);
    audio_buffer_regrow();
    if (rc != DBGEN_OK)
    {
        ui_retry_later(UI_RETRY_BEGIN, "prepare", rc);
        next->reserved = false;
        return false;
    }
    ui_retry[UI_RETRY_BEGIN].delay = 0;
    ui_candidate_exported = 0;
    /* a fresh build or a validated copy of a salvaged directory always
     * publishes; an update clone only once something was written */
    ui_candidate_changed = fresh || ui_pre_request;
    if (next->ram.handle > 0)
        core_free(next->ram.handle);
    memset(next, 0, sizeof(*next));
    next->reserved = true;
    next->stat = ui_active->stat;
    next->master = ui_active->master;
    if (fresh)
    {
        /* commit IDs stay monotonic (M-2): a fresh build commits above
         * the library selected at boot, the newest valid one (manifest,
         * or the salvaged highest commit), so "lowest" stays "oldest" for
         * dbgen's recycling */
        int32_t seed = ui_active->master.commitid;
        /* ... and above every other line's (R7b-1a): a format change or
         * damage leaves this one at 0 while old manifests and exports
         * carry higher IDs that would outrank the new line's exports */
        h = ui_generation_buffer(&io);
        if (h > 0)
        {
            ops = ui_generation_ops(&io);
            seed = MAX(seed, dbgen_newest_commit(ui_db_root, io.buffer,
                                                 io.size / 2, &ops));
            seed = MAX(seed, ui_changelog_scan(io.buffer));
            core_free(h);
        }
        memset(&next->master, 0, sizeof(next->master));
        next->master.commitid = MAX(seed, 0);
        next->stat.ready = false;
        ui_generation_fresh = true;
        ui_db_fresh = true;
    }
    next->stat.ramcache = false;
    next->stat.ramcache_allocated = 0;
    next->stat.ramcache_used = 0;
    next->generation = ui_active->generation + 1;
    strmemccpy(next->stat.db_path, path, sizeof(next->stat.db_path));
    if ((remove_db_file(next, TAGCACHE_FILE_TEMP) < 0 && errno != ENOENT)
        || (remove_db_file(next, TAGCACHE_FILE_IMPORT) < 0 && errno != ENOENT))
    {
        next->reserved = false;
        return false;
    }
    ui_build = next;
    next->reserved = false;
    ui_candidate_pending = true;
    return true;
}

/* The automatic first build: "Updating Library..." while it runs; a
 * deferred one (backoff, memory) must not leave that status stuck. */
static bool ui_initial_build_begin(void)
{
    /* "Updating Library..." also while the build waits for audio to
     * give back the memory its candidate needs (there is no library yet);
     * any other failure (a prepare that keeps failing, an unresolved
     * generation) never leaves it up (Finding 1) */
    bool ok = ui_candidate_begin();
    ui_busy = ok || (ui_begin_mem_wait && audio_status() != 0);
    return ok;
}

/* A candidate that definitively fails validation is dropped, never retried
 * forever: the active library stays; the next update/request starts anew
 * in an unreferenced directory (nothing on disk is deleted here). */
static void ui_candidate_abandon(int rc)
{
    struct tagcache_context *old = ui_build;
    ui_log("db", "candidate drop rc=%d", rc);
    ui_ram_yielded = NULL;
    ui_yields = 0;
    ui_retry[UI_RETRY_YIELD].delay = 0;
    ui_candidate_stats_release();
    ui_build = ui_active;
    lib_generation_pool_abandon(old);
    if (old->ram.handle > 0)
    {
        old->ram.hdr = NULL;
        old->ram.handle = core_free(old->ram.handle);
    }
    old->stat.ramcache = false;
    ui_candidate_pending = false;
    ui_maintenance_for_snapshot = false;
    ui_import_pending = false;  /* owed to the dropped database only */
    ui_db_fresh = false;
    ui_pre_pending = ui_pre_request = false;
    ui_busy = false;
}

static bool ui_candidate_publish(void)
{
    struct ui_generation_io io;
    int h;
    enum dbgen_result rc;

    ui_publish_busy = false;
    if (ui_build == ui_active)
        return true;
    if (ui_stats_recovering)
        return false;              /* recovery owns the active numeric namespace */
    if (!tc_stat.ready || !tc_stat.ramcache)
        return false;
    /* Cheap checks first: no validation pass while it cannot finish. */
    ui_publish_busy = ui_active->operations != 0;
    if (!ui_retry_due(UI_RETRY_PUBLISH) || ui_publish_busy)
        return false;
#ifdef SIMULATOR
    sim_commit_hold(100);       /* commit_hold 100: PLAY during publication */
#endif
    if (!lib_generation_pool_prepare(ui_build))
        return false;
    h = ui_generation_buffer(&io);
    if (h <= 0)
        return false;
    struct dbgen_ops ops = ui_generation_ops(&io);
    struct dbgen_publication publication;
    bool already_published = false;
    rc = dbgen_stage_publish(ui_db_root, tc_stat.db_path,
        current_tcmh.commitid, current_tcmh.tch.entry_count,
        io.buffer, io.size / 2, &ops, &publication);
    if (rc == DBGEN_BUSY)
    {
        /* A failed rename can have reached disk. Reconcile by validating
         * the selected manifest, never by rewriting a referenced directory. */
        char selected[DBGEN_PATH_MAX];
        rc = dbgen_select(ui_db_root, selected, io.buffer, io.size / 2, &ops);
        already_published = rc == DBGEN_OK && !strcmp(selected, tc_stat.db_path);
        if (!already_published)
            rc = DBGEN_BUSY;    /* never abandon over the retained state */
    }
    bool ok = false;
    if (rc != DBGEN_OK)
    {
        if (rc == DBGEN_CORRUPT || rc == DBGEN_INVALID)
            ui_candidate_abandon(rc);
        else if (rc != DBGEN_CANCELLED)
            ui_retry_later(UI_RETRY_PUBLISH, "publish", rc);
        goto out;
    }
    mutex_lock(&command_queue_mutex);
    int level = disable_irq_save();
    bool busy = ui_active->operations != 0;
    if (!busy)
        ui_publishing = true;
    restore_irq(level);
    ui_publish_busy = busy;
    if (busy)
        goto unlock;
    if (!ui_candidate_stats_finish(io.buffer, io.size))
    {
        ui_publish_end();
        ui_retry_later(UI_RETRY_PUBLISH, "cand stats", -1);
        if (ui_stats_restart)
        {
            ui_candidate_stats_release();
            ui_stats_restart = false;
        }
        goto unlock;
    }
    rc = already_published ? DBGEN_OK : dbgen_finish_publish(&publication);
    if (rc != DBGEN_OK)
    {
        ui_candidate_maybe_published = true;
        ui_publish_end();
        ui_retry_later(UI_RETRY_PUBLISH, "rename", rc);
        goto unlock;
    }
    /* No yield/allocation between manifest success and RAM publication.
     * Searches already holding the previous descriptor keep it alive. */
    struct tagcache_context *dbq_old = ui_active;
    ui_active = ui_build;
    /* #109 (M4): a database queue keeps the old snapshot by a passive
     * reference for its rebase; a flag, no lock, no I/O here */
    if (dbq_idx_live() && ui_context_ref(dbq_old, false))
        dbq_published(dbq_old);
    /* its master holds absolutes only: replay the journal into its RAM */
    ui_boot_stats_replayed = false;
    ui_generation_fresh = false;
    ui_retry[UI_RETRY_PUBLISH].delay = 0;
    ui_yields = 0;
    ui_retry[UI_RETRY_YIELD].delay = 0;
    ui_publish_end();
    ui_candidate_maybe_published = false;
    ok = true;
unlock:
    mutex_unlock(&command_queue_mutex);
    if (ok)
    {
        ui_candidate_pending = false;
        ui_maintenance_for_snapshot = false;
        ui_reload_pending = false;
        ui_busy = false;
        ui_pre_pending = true;
        ui_active_pre_done = false;
        ui_log("db", "gen %ld n=%ld",
               (long)current_tcmh.commitid, (long)current_tcmh.tch.entry_count);
#ifdef UI_COLLATE
        ui_resort_published();
#endif
        ui_candidate_stats_release();
        /* #88: published, and no commit still owed (a reload-only
         * generation is not the job's end): the window's memory goes
         * back to the audio buffer (maint_poll()'s regrow, or the next
         * cold start) */
        if (!ui_commit_pending && !ui_scan_pending)
        {
            ui_maint_need = 0;
            maint_done(MAINT_COMMIT, true);
        }
    }
out:
    core_free(h);
    audio_buffer_regrow();
    return ok;
}

#ifdef SIMULATOR
static char sim_selected[DBGEN_PATH_MAX]; /* boot choice, for sim_cmd_dbrec */
#endif

static bool ui_generation_select(void)
{
    struct ui_generation_io io;
    char path[DBGEN_PATH_MAX];
    int h = ui_generation_buffer(&io);
    if (h <= 0)
        return false;
    struct dbgen_ops ops = ui_generation_ops(&io);
    enum dbgen_result rc = dbgen_select(ui_db_root, path, io.buffer,
                                        io.size / 2, &ops);
    if (rc == DBGEN_OK || rc == DBGEN_SALVAGED)
    {
        strmemccpy(tc_stat.db_path, path, sizeof(tc_stat.db_path));
        tc_stat.ready = check_all_headers(ui_build);
        /* ascertained: the boot skips tagcache_commit_finalize() for a
         * ready library, and playback's buffer event waits for this */
        tc_stat.readyvalid = true;
        /* No manifest vouches for a salvaged directory: give it one by
         * publishing a validated copy (the next precompute pass). */
        if (rc == DBGEN_SALVAGED)
            ui_pre_request = true;
    }
    else if (rc == DBGEN_CORRUPT)
    {
        /* Definitive format/CRC failure is not ambiguous I/O. A fresh
         * scratch generation may be built if both manifest references are
         * still trustworthy; prepare_empty refuses torn references. */
        ui_generation_fresh = true;
        tc_stat.ready = false;
        tc_stat.readyvalid = true;
    }
    core_free(h);
    audio_buffer_regrow();
    ui_log("db", "gen select rc=%d", rc);
#ifdef SIMULATOR
    strmemccpy(sim_selected, rc == DBGEN_LEGACY ? ui_db_root : path,
               sizeof(sim_selected));
#endif
    return rc == DBGEN_OK || rc == DBGEN_LEGACY || rc == DBGEN_CORRUPT
        || rc == DBGEN_SALVAGED;
}

/* A changelog's age: the commit ID its export read (the "## commit"
 * line, round 7b), -1 for a complete one without it (older exports),
 * -3 for one without the end line (a stock "Export Modifications"
 * file), -4 for none. 'buf': 64 bytes. */
static long ui_changelog_stamp(const char *path, char *buf)
{
    char tail[sizeof(UI_EXPORT_END) - 1];
    long stamp = -4;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return stamp;
    stamp = -3;
    if (lseek(fd, -(off_t)sizeof(tail), SEEK_END) >= 0
        && read(fd, tail, sizeof(tail)) == (ssize_t)sizeof(tail)
        && !memcmp(tail, UI_EXPORT_END, sizeof(tail))
        && lseek(fd, 0, SEEK_SET) == 0)
    {
        ssize_t n = read(fd, buf, 63);
        buf[n > 0 ? n : 0] = '\0';
        char *line = strstr(buf, "\n## commit ");
        stamp = line ? atoi(line + 11) : -1;
    }
    close(fd);
    return stamp;
}

/* Every directory's changelog (the active one first, database_gen0..3,
 * the root; never the candidate's own scratch directory), and a complete
 * ".new" an export left unrenamed (a power cut; LOW-7c): the best input
 * into 'name' ("" for none). Best: the highest stamp; an export above
 * the loaded active library's commit is an unpublished candidate's and
 * ranks below every export at or below that commit (R7b-1c), above the
 * unstamped ones; ties: the order above.
 * Returns the highest stamp seen. 'name': TAGCACHE_BUFSZ. */
static long ui_changelog_scan(char *name)
{
    char *path = name + MAX_PATH, *buf = name + 2 * MAX_PATH;
    long best = -8, top = -1;
    name[0] = '\0';
    for (int dir = -1; dir <= DBGEN_GENERATIONS; dir++)
    {
        if (dir < 0)
            snprintf(path, MAX_PATH, "%s", ui_active->stat.db_path);
        else if (dir < DBGEN_GENERATIONS)
            snprintf(path, MAX_PATH, "%s/database_gen%d", ui_db_root, dir);
        else
            snprintf(path, MAX_PATH, "%s", ui_db_root);
        if (!strcmp(path, tc_stat.db_path))
            continue;
        strlcat(path, "/" TAGCACHE_FILE_CHANGELOG ".new", MAX_PATH);
        for (int pass = 0; pass < 2; pass++)
        {
            if (pass)
                path[strlen(path) - 4] = '\0';     /* without ".new" */
            long stamp = ui_changelog_stamp(path, buf);
            if (!pass && stamp < -2)
                continue;           /* a .new counts only when complete */
            top = MAX(top, stamp);
            long key = 2 * stamp;
            if (ui_active->stat.ready && stamp > ui_active->master.commitid)
                key = -1;   /* below every in-line stamp, above unstamped */
            if (key > best)
            {
                best = key;
                strcpy(name, path);
            }
        }
    }
    return top;
}

/* Compatibility imports are structural worker jobs too: a candidate reads
 * its input in place, never rewriting the active master through an old
 * pending marker; the journal is replayed afterwards and wins conflicts.
 * The input: the rebuild's own export of the master on disk (MED-A), or
 * the owed fresh rebuild's export of the active one (R7b-1b), else the
 * best changelog there is (the journal is sparse: rows unchanged since an
 * export live in it and the masters only; HIGH-R7). */
static int ui_changelog_open(char *name)
{
    if (ui_build == ui_active || ui_candidate_exported == 1)
        return open_db_fd(ui_build, TAGCACHE_FILE_CHANGELOG, O_RDONLY);
    if (ui_candidate_exported >= 2)     /* 3: the master has none */
        return ui_candidate_exported == 2
            ? open_db_fd(ui_active, TAGCACHE_FILE_CHANGELOG, O_RDONLY) : -1;
    ui_changelog_scan(name);
    return name[0] ? open(name, O_RDONLY) : -1;
}
#endif /* UI_COMMIT */

#if defined(SIMULATOR) && defined(UI_COMMIT)
/* commits that ran (past the dirty mark) and ended, ok or not, this boot
 * (sim_db.c sim_db_commits(): wait_commit latches on it, so a commit
 * that starts and ends between two polls, or before the command, is
 * never missed) */
static volatile long sim_commits_ended;

long sim_tagcache_commits_ended(void)
{
    return sim_commits_ended;
}

/* Script hooks (uisimulator/common/sim_cmd_sys.c: commit_hold, ...): the
 * commit waits at one point so USB or a power-off's cancel can be raised
 * exactly there. 0: before the USB check (nothing changed yet), 1: just
 * after the dirty mark, k + 1: after build_index() of commit step k;
 * 100: a candidate's publication, before its view pool (P16). */
static volatile int sim_hold = -1, sim_held = -1;

void sim_tagcache_commit_hold(int point)
{
    sim_hold = point;
}

int sim_tagcache_commit_held(void)
{
    return sim_held;
}

void sim_tagcache_commit_cancel(void)
{
    ui_commit_cancel = true;    /* a power-off's cancel, then aborted */
}

static void sim_commit_hold(int point)
{
    if (sim_hold != point)
        return;
    sim_held = point;
    DEBUGF("tagcache: commit held at %d\n", point);
    while (sim_hold == point && !UI_SLEEP_CUT)  /* P18: a safe point */
        sleep(HZ / 50);
    sim_held = -1;
}
#endif

static bool commit(void)
{
    struct tagcache_header tch;
    struct master_header   tcmh;
    int i, len, rc;
    int tmpfd;
    int masterfd;
#if defined(HAVE_DIRCACHE) && !defined(UI_COMMIT)
    bool dircache_buffer_stolen = false;
#endif
#ifdef HAVE_TC_RAMCACHE
    bool ramcache_buffer_stolen = false;
#endif
#ifdef UI_COMMIT
    bool ui_tempbuf = false;
    bool ui_in_place = false;   /* the master header says dirty */
#endif
    logf("committing tagcache");

    while (write_lock)
        sleep(1);

#if !defined(PLUGIN)
    int fd = open_db_fd(TC_CTX_ARG(ui_build) TAGCACHE_FILE_NOCOMMIT, O_RDONLY);
    if (fd >= 0)
    {
        logf("canceling commit");
        tc_stat.commit_delayed = true;
        close(fd);
        tmpfd = -1;
    }
    else
#endif /*!defined(PLUGIN)*/
    {
        tmpfd = open_db_fd(TC_CTX_ARG(ui_build) TAGCACHE_FILE_TEMP, O_RDONLY);
    }
    if (tmpfd < 0)
    {
        logf("nothing to commit");
#ifdef UI_COMMIT
        /* S36 (C1): a delayed commit whose temp file is gone (or held off
         * by the no-commit file) must not stay due: the thread retried
         * it every second, two failed opens each time, i.e. a scan of
         * the system folder's directory sectors (~28 reads a second,
         * the flash never in standby while stopped) */
        ui_commit_pending = false;
#endif
        return true;
    }


    /* Load the header. */
    len = sizeof(struct tagcache_header);
    rc = read(tmpfd, &tch, len);

    if (tch.magic != TAGCACHE_MAGIC || rc != len)
    {
        logf("incorrect tmpheader");
        close(tmpfd);
        remove_db_file(TC_CTX_ARG(ui_build) TAGCACHE_FILE_TEMP);
#ifdef UI_COMMIT
        ui_commit_pending = false;  /* a torn scan's file: nothing left */
#endif
        return false;
    }

    if (tch.entry_count == 0)
        logf("nothing to commit");

    /* Fully initialize existing headers (if any) before going further. */
    tc_stat.ready = check_all_headers(TC_CTX_PASS(ui_build));

#if defined(HAVE_IPOD_UI) && defined(HAVE_TC_RAMCACHE)
    /* An update that found nothing new: committing would only drop the
     * RAM cache, and it is reloaded only after new entries. */
    if (tch.entry_count == 0 && tc_stat.ready
#ifdef UI_COLLATE
        && !ui_resort_owed()    /* #47: the one re-sort runs */
#endif
        )
    {
        close(tmpfd);
        remove_db_file(TC_CTX_ARG(ui_build) TAGCACHE_FILE_TEMP);
#ifdef UI_COMMIT
        ui_commit_pending = false;
#endif
        return true;
    }
#endif
#ifdef UI_COMMIT
    /* USB is waiting: the commit runs after it, from the same temp file
     * (the post-USB Q_START_SCAN wakes the retry). Nothing is changed or
     * dropped yet, so the host is not held up by the in-place phase. */
#ifdef SIMULATOR
    sim_commit_hold(0);
#endif
    if (ui_commit_usb_queued())
    {
        DEBUGF("tagcache: commit deferred for USB\n");
        close(tmpfd);
        ui_commit_pending = true;
        return false;
    }
    /* before anything is dropped: a delayed commit keeps the library */
    if (tempbuf_size == 0 && !ui_commit_buffer(&tch, &ui_tempbuf))
    {
        close(tmpfd);
        return false;
    }
    ui_commit_pending = false;
#endif

#ifdef HAVE_EEPROM_SETTINGS
    remove_db_file(TC_CTX_ARG(ui_build) TAGCACHE_STATEFILE);
#endif

    /* At first be sure to unload the ramcache! */
#ifdef HAVE_TC_RAMCACHE
    tc_stat.ramcache = false;
#ifdef UI_COMMIT
    ui_win_drop();              /* lib window: the lists go empty */
#endif
#ifdef HAVE_IPOD_UI
    ui_generation++;
    ui_busy = true;             /* reloaded after the commit */
#endif
#endif

    /* Beyond here, jump to commit_error to undo locks and restore dircache */
    rc = false;
    read_lock++;

    /* Try to steal every buffer we can :) */
#if defined(HAVE_DIRCACHE) && !defined(UI_COMMIT)
    if (tempbuf_size == 0)
    {
        /* Suspend dircache to free its allocation. */
        dircache_free_buffer();
        dircache_buffer_stolen = true;

        allocate_tempbuf();
    }
#endif /* HAVE_DIRCACHE */

#ifdef HAVE_TC_RAMCACHE
    if (tempbuf_size == 0 && tc_stat.ramcache_allocated > 0
#ifdef UI_COMMIT
        && tcramcache.handle > 0    /* never pin handle 0 */
#endif
       )
    {
        tcrc_buffer_lock(TC_CTX_PASS(ui_build));
        tempbuf = (char *)(tcramcache.hdr + 1);
        tempbuf_size = tc_stat.ramcache_allocated - sizeof(struct ramcache_header) - 128;
        tempbuf_size &= ~0x03;
        ramcache_buffer_stolen = true;
    }
#endif /* HAVE_TC_RAMCACHE */

#if defined(PLUGIN)
    if (tempbuf_size == 0)
    {
        tempbuf = rb->plugin_get_audio_buffer(&tempbuf_size);
        tempbuf_size &= ~0x03;
    }
#endif /*defined(PLUGIN)*/

    /* And finally fail if there are no buffers available. */
    if (tempbuf_size == 0)
    {
        logf("delaying commit until next boot");
        tc_stat.commit_delayed = true;
        close(tmpfd);
        goto commit_error;
    }

    logf("commit %" PRId32 " entries...", tch.entry_count);
#ifdef UI_TIMING
    long ui_commit_start = current_tick;
    ui_sort_ticks = 0;
#endif

    /* Mark DB dirty so it will stay disabled if commit fails. */
    current_tcmh.dirty = true;
    update_master_header(TC_CTX_PASS(ui_build));
#ifdef UI_COMMIT
    ui_in_place = true;
#ifdef SIMULATOR
    sim_commit_hold(1);
#endif
#endif

    /* Now create the index files. */
    tc_stat.commit_step = 0;
    tch.datasize = 0;
    tc_stat.commit_delayed = false;

    for (i = 0; i < TAG_COUNT && !USR_CANCEL; i++)
    {
        int ret;

        if (TAGCACHE_IS_NUMERIC(i))
            continue;

        tc_stat.commit_step++;
        ret = build_index(i, &tch, tmpfd);
#if defined(SIMULATOR) && defined(UI_COMMIT)
        sim_commit_hold(1 + tc_stat.commit_step);
#endif
        if (ret <= 0)
        {
            close(tmpfd);
            logf("tagcache failed init");
            if (ret == 0)
                tc_stat.commit_delayed = true;

            tc_stat.commit_step = 0;
            goto commit_error;
        }
        do_timed_yield();
    }

#ifdef UI_COMMIT
#ifdef HAVE_IPOD_UI
    /* P18: cut for a sleep before the last stage: nothing is published,
     * so the numeric indices would be built for nothing */
    if (UI_SLEEP_CUT && ui_build != ui_active)
    {
        close(tmpfd);
        tc_stat.commit_step = 0;
        goto commit_error;
    }
#endif
    /* the last stage: a power-off lets it finish (prepare_shutdown) */
    ui_commit_finishing = true;
#ifdef UI_TIMING
    long ui_numeric_start = current_tick;
#endif
#endif
    if (!build_numeric_indices(&tch, tmpfd))
    {
        logf("Failure to commit numeric indices");
        close(tmpfd);
        tc_stat.commit_step = 0;
#ifdef UI_COMMIT
        ui_commit_finishing = false;
#endif
        goto commit_error;
    }
#ifdef UI_COMMIT
    ui_commit_finishing = false;
#ifdef UI_TIMING
    DEBUGF("tagcache: numeric indices in %ld ms\n",
           (current_tick - ui_numeric_start) * 1000 / HZ);
#endif
#endif

    close(tmpfd);

    tc_stat.commit_step = 0;

    if (!USR_CANCEL)
    {
        /* Update the master index headers. */
        if ( (masterfd = open_master_fd(TC_CTX_ARG(ui_build) &tcmh, true)) < 0)
            goto commit_error;

        remove_db_file(TC_CTX_ARG(ui_build) TAGCACHE_FILE_TEMP);

        tcmh.tch.entry_count += tch.entry_count;
        tcmh.tch.datasize = sizeof(struct master_header)
            + sizeof(struct index_entry) * tcmh.tch.entry_count
            + tch.datasize;
        tcmh.dirty = false;
        tcmh.commitid++;

        lseek(masterfd, 0, SEEK_SET);
        write_master_header(TC_CTX_ARG(ui_build) masterfd, &tcmh);
        close(masterfd);

        logf("tagcache committed");
#ifdef UI_TIMING
        DEBUGF("tagcache: committed in %ld ms (sorting %ld ms)\n",
               (current_tick - ui_commit_start) * 1000 / HZ,
               ui_sort_ticks * 1000 / HZ);
#endif
        tagcache_commit_finalize();
#ifdef UI_COMMIT
        ui_after_commit();
#endif
#ifdef UI_COLLATE
        ui_resort_ran = true;
#endif

#if defined(HAVE_TC_RAMCACHE)
        if (ramcache_buffer_stolen)
        {
            tempbuf = NULL;
            tempbuf_size = 0;
            ramcache_buffer_stolen = false;
            tcrc_buffer_unlock(TC_CTX_PASS(ui_build));
        }

#ifndef UI_COMMIT   /* the shell's callers reload it themselves: a scan
                     * here would walk the disk a second time */
        /* Reload tagcache only if we committed new entries */
        if (tc_stat.ramcache_allocated > 0 && tch.entry_count > 0)
            tagcache_start_scan();
#endif
#endif /* HAVE_TC_RAMCACHE */

        rc = true;
    } /*!USR_CANCEL*/

commit_error:
#ifdef HAVE_TC_RAMCACHE
#ifdef HAVE_IPOD_UI
    if (!rc)
        ui_busy = false;        /* no reload follows: the library is off */
#endif
    if (ramcache_buffer_stolen)
    {
        tempbuf = NULL;
        tempbuf_size = 0;
        tcrc_buffer_unlock(TC_CTX_PASS(ui_build));
    }
#endif /* HAVE_TC_RAMCACHE */

    read_lock--;

#ifdef UI_COMMIT
    /* Cut (a power-off, perhaps aborted later) or failed in its in-place
     * phase: the tag files and the master index are partly rewritten and
     * the header says dirty. Never retried over that: the database is
     * unusable, and it is rebuilt from the files (with the statistics
     * kept) after USB or at the next boot, as one found dirty at boot. */
    if (!rc && ui_in_place)
    {
        tc_stat.ready = false;
        ui_commit_pending = false;
        DEBUGF("tagcache: commit cut in place: rebuild\n");
    }
    if (ui_tempbuf)
        free_tempbuf();
#ifdef SIMULATOR
    if (ui_in_place)
        sim_commits_ended++;    /* wait_commit's latch: this one ran */
#endif
#endif

#if defined(HAVE_DIRCACHE) && !defined(UI_COMMIT)
    /* Resume the dircache, if we stole the buffer. */
    if (dircache_buffer_stolen)
    {
        free_tempbuf();
        dircache_resume();
    }
#endif /* HAVE_DIRCACHE */

    return rc;
}

#if defined(HAVE_IPOD_UI_DEVMODE) && !defined(PLUGIN) && !defined(__PCTOOL__)
/* ProkBox Fit-1 M0: the device timings that calibrate the simulator's
 * x200-400 factor (docs/hardware.md, "Tagcache commit buffer"). Every
 * later commit() and tagcache_build() call goes through these. */
#include "ipodui/uilog.h"
static bool commit_timed(void)
{
    long t0 = current_tick;
    bool ok = commit();

    ui_log("db", "commit ms=%ld ok=%d n=%ld b=%d",
           (current_tick - t0) * (1000 / HZ), (int)ok,
           (long)current_tcmh.tch.entry_count,
#if defined(HAVE_ADJUSTABLE_CPU_FREQ) && !defined(SIMULATOR)
           get_cpu_boost_counter()
#else
           0
#endif
           );
    return ok;
}
#define commit() commit_timed()
#endif

#if defined(UI_COMMIT) && defined(HAVE_IPOD_UI)
/* P18 (#86): whether a commit runs (sleep entry waits for it to stop),
 * and whether a sleep cut one (the worker drops its candidate) */
static volatile bool ui_in_commit;
static bool ui_sleep_cut_seen;  /* tagcache thread */
static bool ui_sleep_rescan;    /* the cut update, owed to a window */

static bool ui_commit_run(void)
{
    ui_in_commit = true;
    bool ok = commit();
    if (!ok && ui_sleep_cut && ui_build != ui_active)
    {
        /* owed now, whoever drops the candidate (below, or the worker's
         * unchanged-clone drop) */
        ui_sleep_cut_seen = true;
        if (!ui_db_fresh)
            ui_sleep_rescan = true;
        maint_done(MAINT_COMMIT, false);    /* its memory back */
    }
    ui_in_commit = false;
    return ok;
}
#undef commit
#define commit() ui_commit_run()
#endif

void tagcache_commit_finalize(void)
{
#ifdef UI_COMMIT
    if (ui_build == ui_active && (ui_generation_unresolved || ui_generation_fresh))
    {
        tc_stat.ready = false;
        tc_stat.readyvalid = true;
        return;
    }
#endif
    tc_stat.ready = check_all_headers(TC_CTX_PASS(ui_build));
    tc_stat.readyvalid = true;
}

#if !defined(PLUGIN)
#ifndef __PCTOOL__

#ifndef UI_COMMIT   /* the shell's statistics replay writes its own */
static bool modify_numeric_entry(TC_CTX_PARAM int masterfd, int idx_id, int tag, long data)
{
    struct index_entry idx;

    if (!TC_STAT(ctx).ready)
        return false;

    if (!TAGCACHE_IS_NUMERIC(tag))
        return false;

    if (!get_index(TC_CTX_ARG(ctx) masterfd, idx_id, &idx, false))
        return false;

    idx.tag_seek[tag] = data;
    idx.flag |= FLAG_DIRTYNUM;

    return write_index(TC_CTX_ARG(ctx) masterfd, idx_id, &idx);
}
#endif

#if 0
bool tagcache_modify_numeric_entry(struct tagcache_search *tcs,
                                   int tag, long data)
{
#ifdef UI_COMMIT
    struct tagcache_context *ctx = tcs->context;
#endif
    struct master_header myhdr;

    if (tcs->masterfd < 0)
    {
        if ( (tcs->masterfd = open_master_fd(TC_CTX_ARG(ctx) &myhdr, true)) < 0)
            return false;
    }

    return modify_numeric_entry(TC_CTX_ARG(ctx) tcs->masterfd, tcs->idx_id, tag, data);
}
#endif

static bool command_queue_is_full(void)
{
    int next;

    next = command_queue_widx + 1;
    if (next >= TAGCACHE_COMMAND_QUEUE_LENGTH)
        next = 0;

    return (next == command_queue_ridx);
}

#ifdef UI_COMMIT
static bool ui_entry_filename(struct tagcache_context *ctx, int idx_id,
                              char *filename, size_t capacity)
{
    struct index_entry idx;
    struct tagcache_header th;
    struct tagfile_entry fe;
    int fd;

    if (!get_index(ctx, -1, idx_id, &idx, false))
        return false;
    fd = open_tag_fd(ctx, &th, tag_filename, false);
    if (fd < 0)
        return false;
    bool ok = lseek(fd, idx.tag_seek[tag_filename], SEEK_SET)
                    == idx.tag_seek[tag_filename]
        && read_tagfile_entry(ctx, fd, &fe) == sizeof(fe)
        && fe.idx_id == idx_id && fe.tag_length > 0
        && (size_t)fe.tag_length <= capacity
        && read(fd, filename, fe.tag_length) == fe.tag_length
        && filename[fe.tag_length - 1] == '\0';
    close(fd);
    return ok;
}

static bool ui_journal_ready(void *buffer, size_t size)
{
    if (!ui_stats_journal.ready)
    {
        if (!ui_on_worker())
        {
            ui_drain_due = true;    /* open/salvage: the worker's stack */
            return false;
        }
        if (!ui_shutdown_flushing && !ui_retry_due(UI_RETRY_JOURNAL))
            return false;
        enum dbjournal_result rc = dbjournal_open_cancel(ui_db_root,
            &ui_stats_journal, buffer, size, ui_generation_cancel, NULL);
        if (rc == DBJOURNAL_CORRUPT)
        {
            /* One bad frame must not stop statistics for good: set the
             * file aside, keep its valid prefix, and once RAM holds the
             * replayed values checkpoint them (frames after it are lost
             * from disk, not from RAM). */
            rc = dbjournal_salvage(ui_db_root, buffer, size,
                                   ui_generation_cancel, NULL);
            ui_log("db", "jnl bad rc=%d", rc);
            if (rc == DBJOURNAL_OK)
                rc = dbjournal_open_cancel(ui_db_root, &ui_stats_journal,
                    buffer, size, ui_generation_cancel, NULL);
            if ((rc == DBJOURNAL_OK || rc == DBJOURNAL_MISSING)
                && ui_boot_stats_replayed)
                ui_stats_recovering = ui_active->stat.ramcache;
        }
        if (rc != DBJOURNAL_OK && rc != DBJOURNAL_MISSING)
        {
            if (rc != DBJOURNAL_CANCELLED)
                ui_retry_later(UI_RETRY_JOURNAL, "journal open", rc);
            return false;
        }
        ui_retry[UI_RETRY_JOURNAL].delay = 0;
    }
    ui_stats_journal.scratch = buffer;
    ui_stats_journal.bytes = size;
    return true;
}

struct ui_stats_enum {
    struct tagcache_context *ctx;
    const int32_t *values;
    int row, tag, named;
    bool every;                 /* each row whole (MED-B, a full journal) */
    int masterfd;               /* the master on disk */
    struct index_entry *chunk;  /* its rows [first, first + n), bulk read */
    int first, n, cap;
    char filename[MAX_PATH];
};

static int ui_stats_next(void *opaque, char *filename, size_t capacity,
                          uint32_t *tag, int32_t *absolute)
{
    static const uint8_t tags[] = { tag_playcount, tag_rating, tag_playtime,
        tag_lastplayed, tag_lastelapsed, tag_lastoffset };
    struct ui_stats_enum *e = opaque;
    int count = e->ctx->master.tch.entry_count;
    while (e->row < count)
    {
        struct index_entry *idx = &e->ctx->ram.hdr->indices[e->row];
        const int32_t *v = &e->values[e->row * ARRAYLEN(tags)];
        if (idx->flag & FLAG_DELETED)
        {
            e->row++;
            e->tag = 0;
            continue;
        }
        /* MED-5: only values that differ from the master on disk (the
         * replay's base): a checkpoint of a sparse played set stays small */
        {
            if (e->row < e->first || e->row >= e->first + e->n)
            {
                int want = MIN(e->cap, count - e->row);
                if (lseek(e->masterfd, sizeof(struct master_header)
                          + (off_t)e->row * sizeof(*e->chunk), SEEK_SET) < 0
                    || read_index_entries(e->ctx, e->masterfd, e->chunk, want)
                       != (ssize_t)(want * sizeof(*e->chunk)))
                    return -1;
                e->first = e->row;
                e->n = want;
            }
            const struct index_entry *disk = &e->chunk[e->row - e->first];
            /* a folded row: the fallback generation lacks it (MED-A) */
            while (e->tag < (int)ARRAYLEN(tags) && !e->every
                   && !(disk->flag & FLAG_UI_STATS_FOLDED)
                   && v[e->tag] == disk->tag_seek[tags[e->tag]])
                e->tag++;
            if (e->tag == (int)ARRAYLEN(tags))
            {
                e->tag = 0;
                e->row++;
                continue;
            }
        }
        if (e->named != e->row + 1)
        {
            if (!ui_entry_filename(e->ctx, e->row, e->filename,
                                   sizeof(e->filename)))
                return -1;
            e->named = e->row + 1;
        }
        if (strlen(e->filename) >= capacity)
            return -1;
        strcpy(filename, e->filename);
        *tag = tags[e->tag];
        *absolute = v[e->tag];
        if (++e->tag == (int)ARRAYLEN(tags))
        {
            e->tag = 0;
            e->row++;
        }
        return 1;
    }
    return 0;
}

static bool ui_ram_numeric(struct tagcache_context *ctx, long idx_id, int tag,
                           long data, int32_t flags);
/* Caller holds command_queue_mutex. A queue item owns its source snapshot,
 * so a publication cannot reinterpret its row number in another database. */
static bool ui_command_drain_locked(void *buffer, size_t size, bool compact)
{
    char filename[TAGCACHE_BUFSZ];

    if (ui_stats_recovering)
        return false;
    if (ui_stats_dropped != ui_stats_dropped_logged)
    {
        ui_stats_dropped_logged = ui_stats_dropped;
        ui_log("db", "dropped=%u", ui_stats_dropped);
    }
    if (!ui_journal_ready(buffer, size))
    {
        /* Before the boot replay RAM lacks the journal's values: coalescing
         * there and checkpointing it would lose them. Queue and retry. Off
         * the worker nothing failed: the worker opens the journal. */
        if (ui_on_worker())
            ui_stats_recovering = ui_active->stat.ramcache
                                  && ui_boot_stats_replayed;
        return false;
    }
    while (!COMMAND_QUEUE_IS_EMPTY)
    {
        if (tagcache_usb_quiescing
            || (ui_shutdown_flushing && ui_generation_cancel(NULL)))
            return false;
        struct tagcache_command_entry *ce = &command_queue[command_queue_ridx];
        struct tagcache_context *ctx = ce->context;
        /* a delta or relative frame needs no base: valid before, during
         * and after the boot replay (the flags travel in the tag) */
        int tag = ce->tag;
        int32_t value = ce->data;
        uint64_t seq = ui_stats_journal.seq;

        /* An append that failed after its write (readback, close) may be
         * on disk: the reopened journal tells (this thread alone appends).
         * A delta must never go out twice (MED-1). */
        bool written = ui_append_maybe && seq == ui_append_maybe;
        ui_append_maybe = 0;
        if (!written)
        {
            if (!ui_entry_filename(ctx, ce->idx_id, filename, sizeof(filename)))
                return false;
            enum dbjournal_result rc = dbjournal_append(&ui_stats_journal,
                filename, tag, value);
            if (rc == DBJOURNAL_IO)
                ui_append_maybe = seq + 1;
            if (rc != DBJOURNAL_OK)
            {
                ui_stats_recovering = ui_active->stat.ramcache
                                      && ui_boot_stats_replayed;
                if (rc == DBJOURNAL_FULL && !compact)
                    ui_stats_restart = true;
                ui_journal_full = rc == DBJOURNAL_FULL;
                ui_log("db", "append rc=%d", rc);
                return false;
            }
        }
        ui_context_put(ctx, false);
        if (ctx->stat.queue_length > 0)
            ctx->stat.queue_length--;
        if (++command_queue_ridx >= TAGCACHE_COMMAND_QUEUE_LENGTH)
            command_queue_ridx = 0;
    }
    return true;
}


/* Copy current absolutes and clear only our RAM-only pending marker, with
 * no yield (producers push lock-free, IRQs off, between yields only).
 * Filename I/O and checkpoint writes happen later. A producer arriving
 * afterwards sets the marker again for the next tail. */
static bool ui_stats_capture(struct tagcache_context *ctx, int32_t *values,
                             unsigned char *dirty, unsigned char *taken,
                             bool all)
{
    static const uint8_t tags[] = { tag_playcount, tag_rating, tag_playtime,
        tag_lastplayed, tag_lastelapsed, tag_lastoffset };
    size_t count = ctx->master.tch.entry_count;
    bool any = false;
    memset(dirty, 0, (count + 7) / 8);
    for (size_t row = 0; row < count; row++)
    {
        struct index_entry *idx = &ctx->ram.hdr->indices[row];
        if (idx->flag & FLAG_UI_STATS_PENDING)
            taken[row / 8] |= 1u << (row & 7);  /* restored if unfinished */
        else if (!all)
            continue;
        idx->flag &= ~FLAG_UI_STATS_PENDING;
        if (idx->flag & FLAG_DELETED)
            continue;
        dirty[row / 8] |= 1u << (row & 7);
        for (size_t tag = 0; tag < ARRAYLEN(tags); tag++)
            values[row * ARRAYLEN(tags) + tag] = idx->tag_seek[tags[tag]];
        any = true;
    }
    return any;
}

static bool ui_stats_pending(struct tagcache_context *ctx)
{
    for (int row = 0; row < ctx->master.tch.entry_count; row++)
        if (ctx->ram.hdr->indices[row].flag & FLAG_UI_STATS_PENDING)
            return true;
    return false;
}

/* Appends six absolutes per selected row: rows set in 'dirty', values
 * from 'values'; or, with values NULL, the RAM rows marked
 * FLAG_UI_STATS_PENDING, from RAM, each unmarked once appended. */
static enum dbjournal_result ui_stats_append_dirty(struct tagcache_context *ctx,
    const int32_t *values, const unsigned char *dirty, struct dbjournal *staged)
{
    static const uint8_t tags[] = { tag_playcount, tag_rating, tag_playtime,
        tag_lastplayed, tag_lastelapsed, tag_lastoffset };
    char filename[MAX_PATH];
    for (int row = 0; row < ctx->master.tch.entry_count; row++)
    {
        struct index_entry *idx = &ctx->ram.hdr->indices[row];
        if (values ? !(dirty[row / 8] & (1u << (row & 7)))
                   : (idx->flag & (FLAG_UI_STATS_PENDING | FLAG_DELETED))
                     != FLAG_UI_STATS_PENDING)
            continue;
        if (ui_generation_cancel(NULL))
            return DBJOURNAL_CANCELLED;
        if (!ui_entry_filename(ctx, row, filename, sizeof(filename)))
            return DBJOURNAL_IO;
        for (size_t tag = 0; tag < ARRAYLEN(tags); tag++)
        {
            enum dbjournal_result rc = dbjournal_append(staged, filename,
                tags[tag], values ? values[row * ARRAYLEN(tags) + tag]
                                  : idx->tag_seek[tags[tag]]);
            if (rc != DBJOURNAL_OK)
                return rc;
        }
        if (!values)
            idx->flag &= ~FLAG_UI_STATS_PENDING;
    }
    return DBJOURNAL_OK;
}

static void ui_stats_discard_captured_queue(void)
{
    while (!COMMAND_QUEUE_IS_EMPTY)
    {
        struct tagcache_context *ctx = command_queue[command_queue_ridx].context;
        ui_context_put(ctx, false);
        if (ctx->stat.queue_length > 0)
            ctx->stat.queue_length--;
        if (++command_queue_ridx >= TAGCACHE_COMMAND_QUEUE_LENGTH)
            command_queue_ridx = 0;
    }
}

/* Soft headroom leaves room for ordinary serialized appends during bulk
 * compaction. Never enumerate the library while blocking its producers.
 * A hard-full/error journal switches producers to a coalesced RAM tail:
 * no queue backpressure and no older prefix replayed over newer absolutes.
 * Until checkpoint publication, these unflushed values survive in RAM only;
 * a power cut retains the prior durable journal, just as with a failed flush. */
static void ui_journal_compact_worker(void)
{
    static uint64_t compact_after = DBJOURNAL_MAX_BYTES / 2;
    static const uint8_t tags[] = { tag_playcount, tag_rating, tag_playtime,
        tag_lastplayed, tag_lastelapsed, tag_lastoffset };
    struct tagcache_context *ctx = ui_active;
    struct ui_generation_io io;
    struct dbjournal source, staged;
    struct dbjournal_cursor cursor;
    int values_handle = 0, io_handle = 0;
    unsigned char *taken = NULL;
    bool finished = false, tried = false;

    if ((!ui_stats_recovering && (!ui_stats_journal.ready
         || ui_stats_journal.end < compact_after))
        || !ctx->stat.ramcache || !ui_boot_stats_replayed
        || (ui_commit_cancel && !ui_shutdown_flushing) || tagcache_usb_quiescing
        || !ui_retry_due(UI_RETRY_COMPACT))
        return;
    /* MED-B: not while a candidate is unpublished: its prepared replay
     * folded values a sparse checkpoint may omit (equal to the active
     * master), and a re-prepare does not re-clone it, so a stale fold
     * would be published. Drains still append the owed rows. A full
     * journal (no drain can) gets a checkpoint of every row instead:
     * correct over any master, the candidate's (published or not) too,
     * with no candidate dropped and no rescan (R7a, R7b). */
    bool every = ui_build != ui_active;
    if (every && !ui_journal_full)
        return;
    ctx = tagcache_context_acquire_passive();
    if (!ctx)
        return;
    size_t count = ctx->master.tch.entry_count;
    if (count > SIZE_MAX / (sizeof(int32_t) * ARRAYLEN(tags)))
        goto done;
    size_t values_bytes = count * sizeof(int32_t) * ARRAYLEN(tags);
    size_t bytes = values_bytes + 2 * ((count + 7) / 8);
    /* No I/O or yield between the last playback check and allocation. */
    if (audio_status() != 0 && core_allocatable() < bytes + 64)
        goto done;
    values_handle = core_alloc_ex(MAX(bytes, sizeof(int32_t)), &buflib_ops_locked);
    if (values_handle <= 0)
        goto done;
    int32_t *values = core_get_data(values_handle);
    unsigned char *dirty = (unsigned char *)values + values_bytes;
    taken = dirty + (count + 7) / 8;
    memset(taken, 0, (count + 7) / 8);
    io_handle = ui_generation_buffer(&io);
    if (io_handle <= 0)
        goto done;

    /* Producers coalesce into RAM (PENDING) from the capture until the
     * checkpoint is published: the queued prefix is in RAM already (after
     * the replay every update changes RAM at once) and goes with it. A
     * pass that does not finish leaves recovering set: drains append the
     * prefix and the PENDING rows. Reopen a failed journal first. A pass
     * that fails from here on backs off (LOW-4: the disk sleeps). */
    tried = true;
    if (!ui_journal_ready(io.buffer, io.size / 2))
        goto done;
    mutex_lock(&command_queue_mutex);
    ui_stats_recovering = true;
    ui_stats_capture(ctx, values, dirty, taken, true);
    source = ui_stats_journal;
    bool ok = dbjournal_checkpoint(&source, &cursor) == DBJOURNAL_OK;
    ui_stats_journal.scratch = NULL;
    mutex_unlock(&command_queue_mutex);
    if (!ok)
        goto done;

    source.scratch = io.buffer;
    source.bytes = io.size / 2;
    struct master_header mh;
    struct ui_stats_enum enumeration = { .ctx = ctx, .values = values,
        .every = every,
        .masterfd = open_master_fd(ctx, &mh, false),
        .chunk = (void *)((char *)io.buffer + io.size / 2),
        .cap = io.size / 2 / sizeof(struct index_entry) };
    ok = enumeration.masterfd >= 0
        && dbjournal_compact_stage(&source, ui_stats_next, &enumeration,
                                   &staged) == DBJOURNAL_OK;
    if (enumeration.masterfd >= 0)
        close(enumeration.masterfd);
    if (!ok)
        goto done;
    /* Catch up outside the producer lock. Finish only at an exact empty
     * boundary; sustained activity defers instead of extending the lock. */
    for (unsigned attempt = 0; attempt < 4; attempt++)
    {
        mutex_lock(&command_queue_mutex);
        ui_stats_capture(ctx, values, dirty, taken, false);
        mutex_unlock(&command_queue_mutex);
        if (ui_stats_append_dirty(ctx, values, dirty, &staged) != DBJOURNAL_OK)
            break;
        mutex_lock(&command_queue_mutex);
        ok = !ui_stats_pending(ctx)
            && ui_stats_journal.seq == cursor.seq
            && ui_stats_journal.end == cursor.end;
        if (ok)
            ok = dbjournal_compact_finish(&ui_stats_journal, &staged, &cursor)
                 == DBJOURNAL_OK;
        if (ok)
        {
            ui_append_maybe = 0;
            ui_journal_full = false;
            ui_stats_discard_captured_queue();
            /* producers coalesce without a lock: any row marked while the
             * checkpoint was published stays owed */
            ui_stats_recovering = ui_stats_pending(ctx);
            ui_stats_restart = true;
        }
        mutex_unlock(&command_queue_mutex);
        if (ok)
        {
            finished = true;
            /* Long canonical paths can make a complete checkpoint exceed
             * the initial soft threshold. Leave headroom/hysteresis instead
             * of rebuilding that same checkpoint at every worker timeout. */
            uint64_t headroom = MIN((uint64_t)4 * 1024 * 1024,
                                    (uint64_t)DBJOURNAL_MAX_BYTES / 8);
            uint64_t growth = MIN((uint64_t)16 * 1024 * 1024,
                                  (uint64_t)DBJOURNAL_MAX_BYTES / 4);
            compact_after = MAX(ui_stats_journal.end + 1,
                MIN((uint64_t)DBJOURNAL_MAX_BYTES - headroom,
                    ui_stats_journal.end + growth));
            ui_log("db", "ckpt n=%lu b=%lu",
                   (unsigned long)count, (unsigned long)ui_stats_journal.end);
            break;
        }
    }
done:
    /* H2: rows whose PENDING mark this pass took are owed again unless
     * their checkpoint was published */
    for (size_t row = 0; taken && !finished && row < count; row++)
        if (taken[row / 8] & (1u << (row & 7)))
            ctx->ram.hdr->indices[row].flag |= FLAG_UI_STATS_PENDING;
    if (finished)
        ui_retry[UI_RETRY_COMPACT].delay = 0;
    else if (tried && !ui_commit_cancel && !tagcache_usb_quiescing)
        ui_retry_later(UI_RETRY_COMPACT, "compact", -1);   /* not a stop */
    if (io_handle > 0)
        core_free(io_handle);
    if (values_handle > 0)
        core_free(values_handle);
    tagcache_context_release_passive(ctx);
    audio_buffer_regrow();
}

/* The worker's drain, through a 4 KiB stack buffer: no allocation, so it
 * works beside a full audio buffer. With RAM-coalesced values owed
 * (recovering), the queued (older) prefix and then every PENDING row's
 * absolutes go out as ordinary frames; replay applies them in order. Never
 * a checkpoint: clean power-off uses this too and waits at most
 * UI_STATS_SHUTDOWN_TICKS (B-M5, H2). The tagcache stack holds it. */
static void ui_stats_drain(void)
{
    char scratch[DBJOURNAL_SCRATCH_MIN];
    struct tagcache_context *ctx = ui_active;
    bool owed = ui_stats_recovering && ctx->stat.ramcache && ctx->ram.hdr;

    ui_drain_due = false;
    ui_drain_force = false;
#ifdef HAVE_IPOD_UI
    ui_drain_sleep = false;
#endif
    mutex_lock(&command_queue_mutex);
    if (owed)
        ui_stats_recovering = false;    /* the drain appends the prefix */
    enum dbjournal_result rc = DBJOURNAL_OK;
    bool ok = ui_command_drain_locked(scratch, sizeof(scratch), false)
        && (!owed || (rc = ui_stats_append_dirty(ctx, NULL, NULL,
                                                 &ui_stats_journal))
                     == DBJOURNAL_OK);
    if (ok || rc == DBJOURNAL_FULL)
        ui_journal_full = !ok;
    ui_stats_journal.scratch = NULL;
    mutex_unlock(&command_queue_mutex);
    if (owed)
    {
        /* a full or failing journal is retried with backoff (the
         * stopped-only compaction makes room), not every second */
        ui_stats_recovering = !ok;
        if (ok)
            ui_retry[UI_RETRY_JOURNAL].delay = 0;
        else
            ui_retry_later(UI_RETRY_JOURNAL, "stats append", 0);
    }
}

static void ui_drain_kick(void)
{
    ui_drain_due = true;
#ifdef TC_QUIET_WAIT
    tagcache_quiet_kick();  /* P19 19-1a: the disk is awake now */
#endif
}

/* Queued statistics are written by the worker while storage is awake
 * anyway (an audio refill, any access), never with a spin-up of their
 * own per track change: the storage-idle notice comes right before the
 * disk sleeps, so on the target the worker also needs the disk still (or
 * again) active, or a queue half full. The simulator has no disk state
 * (hostfs): the notice is enough there. */
static bool ui_drain_wanted(void)
{
    int queued = (command_queue_widx - command_queue_ridx
                  + TAGCACHE_COMMAND_QUEUE_LENGTH)
                 % TAGCACHE_COMMAND_QUEUE_LENGTH;
    bool now = false;   /* P18 review M2: sleep entry's drain */
#ifdef HAVE_IPOD_UI
    now = ui_drain_sleep;
#endif
    if (!queued && !(ui_stats_recovering
                     && (now || ui_retry_due(UI_RETRY_JOURNAL))))
        return false;
#ifdef HAVE_STORAGE_POWEROFF
    /* #114: any time an access is free (powered, inside the idle hold),
     * not only in the second after a transfer: a drain that missed that
     * second waited for the queue to fill half and powered the drive up
     * for itself, every few tracks between refills */
    if (storage_power_warm())
        return true;
#endif
#if defined(SIMULATOR) && defined(HAVE_STORAGE_POWEROFF)
    /* the sim's drive model: the device's rule (its notice comes while
     * the drive is warm, which the test above covers) */
    return ui_drain_force || queued >= TAGCACHE_COMMAND_QUEUE_LENGTH / 2;
#elif defined(SIMULATOR)
    return ui_drain_force || ui_drain_due || ui_stats_recovering;
#else
    return ui_drain_force || storage_disk_is_active()
        || queued >= TAGCACHE_COMMAND_QUEUE_LENGTH / 2;
#endif
}
#endif

static void command_queue_sync_callback(void)
{
#ifdef UI_COMMIT
    if (!ui_on_worker())
    {
        ui_drain_due = true;
        return;
    }
    ui_stats_drain();
#else
    struct master_header myhdr;
    int masterfd;

    mutex_lock(&command_queue_mutex);

    if ( (masterfd = open_master_fd(TC_CTX_ARG(ui_build) &myhdr, true)) < 0)
    {
#ifdef HAVE_IPOD_UI     /* don't leave the queue locked for good */
        mutex_unlock(&command_queue_mutex);
#endif
        return;
    }

    while (command_queue_ridx != command_queue_widx)
    {
        struct tagcache_command_entry *ce = &command_queue[command_queue_ridx];

        switch (ce->command)
        {
            case CMD_UPDATE_MASTER_HEADER:
            {
                close(masterfd);
                update_master_header(TC_CTX_PASS(ui_build));

                /* Re-open the masterfd. */
                if ( (masterfd = open_master_fd(TC_CTX_ARG(ui_build) &myhdr, true)) < 0)
                {
#ifdef HAVE_IPOD_UI
                    mutex_unlock(&command_queue_mutex);
#endif
                    return;
                }

                break;
            }
            case CMD_UPDATE_NUMERIC:
            {
                modify_numeric_entry(TC_CTX_ARG(ui_build) masterfd, ce->idx_id, ce->tag, ce->data);
                break;
            }
        }

        if (++command_queue_ridx >= TAGCACHE_COMMAND_QUEUE_LENGTH)
            command_queue_ridx = 0;
    }

    close(masterfd);

    tc_stat.queue_length = 0;
    mutex_unlock(&command_queue_mutex);
#endif
}

#if defined(UI_COMMIT) && defined(HAVE_IPOD_UI)
/* P18 (review M3): any storage access since tick t (the disk woke for
 * someone): the driver's last activity, which a 1 s poll cannot miss as
 * it can the short "active" state; the sim's fill model in the sim */
static bool ui_disk_used_since(long t)
{
#ifdef SIMULATOR
    extern long sim_disk_active_until;  /* artcache.c's model, HZ/5 */
    return sim_disk_active_until
           && TIME_AFTER(sim_disk_active_until, t + HZ/5);
#else
    return TIME_AFTER(storage_last_disk_activity(), t);
#endif
}
#endif

static void run_command_queue(bool force)
{
    if (COMMAND_QUEUE_IS_EMPTY)
        return;

    if (force || command_queue_is_full())
        command_queue_sync_callback();
    else
#ifdef UI_COMMIT
        register_storage_idle_func(ui_drain_kick);
#else
        register_storage_idle_func(command_queue_sync_callback);
#endif
}

#ifdef UI_COMMIT
/* A numeric update's RAM write: false if 'ctx' has no such RAM row or the
 * tag is not numeric. A TAGCACHE_UI_DELTA tag adds to the row's value. */
static bool ui_ram_numeric(struct tagcache_context *ctx, long idx_id, int tag,
                           long data, int32_t flags)
{
    int t = UI_STAT_TAG(tag);
    if (!ctx->stat.ramcache || idx_id < 0
        || idx_id >= ctx->master.tch.entry_count || !TAGCACHE_IS_NUMERIC(t))
        return false;
    int32_t *v = &ctx->ram.hdr->indices[idx_id].tag_seek[t];
    *v = (tag & TAGCACHE_UI_DELTA) ? *v + data : data;
    ctx->ram.hdr->indices[idx_id].flag |= flags;
    return true;
}

/* Producers (audio, UI, tagcache threads) never block or wait: IRQs off,
 * a push into the ring the tagcache thread alone consumes (H1). This
 * exclusion assumes every producer runs on the CPU core: a producer on
 * the COP needs a real lock here. The RAM row changes at once, except
 * that before the boot replay a delta or relative value leaves it alone
 * (the replay result overwrites it). Recovering (a full queue after the
 * replay, or a failing journal): RAM only, marked PENDING, appended by the
 * next drain. A full queue before the replay (the worker cannot even
 * drain: the journal fails) drops and counts. */
static void queue_command(int cmd, long idx_id, int tag, long data)
{
    int level = disable_irq_save();
    struct tagcache_context *ctx = ui_active;
    bool pre = !ui_boot_stats_replayed;
    int slot = ui_stat_slot(UI_STAT_TAG(tag));
    if (UI_STAT_TAG(tag) == tag_lastplayed && (pre || (tag & TAGCACHE_UI_REL)))
    {
        /* before the replay: the next serial after the journal's; a
         * relative one arriving afterwards (the path hand-off) takes the
         * next serial now */
        tag = pre ? tag_lastplayed | TAGCACHE_UI_REL : tag_lastplayed;
        data = pre ? 0 : ctx->master.serial++;
    }
    bool flex = pre && (tag & (TAGCACHE_UI_DELTA | TAGCACHE_UI_REL));
    int next = (command_queue_widx + 1) % TAGCACHE_COMMAND_QUEUE_LENGTH;
    int32_t flags = FLAG_DIRTYNUM | FLAG_UI_STATS_PENDING;
    if (slot < 0)
        flags = 0;              /* only runtime statistics are journaled */
    else if (ui_stats_recovering)
        ;
    else if (next != command_queue_ridx && ui_context_ref(ctx, false))
    {
        struct tagcache_command_entry *ce = &command_queue[command_queue_widx];
        ce->command = cmd;
        ce->context = ctx;
        ce->idx_id = idx_id;
        ce->tag = tag;
        ce->data = data;
        if (!flex)
            ui_ram_numeric(ctx, idx_id, tag, data, FLAG_DIRTYNUM);
        command_queue_widx = next;
        ctx->stat.queue_length++;
        flags = FLAG_DIRTYNUM;  /* queued */
    }
    else if (ctx->stat.ramcache && !pre)
        ui_stats_recovering = true; /* full: RAM until a drain appends it */
    else
        flags = 0;
    if (flags & FLAG_UI_STATS_PENDING)
    {
        if (!ui_ram_numeric(ctx, idx_id, tag, data, flags))
            ui_stats_dropped++;
    }
    else if (!flags)
        ui_stats_dropped++;     /* logged by the worker */
    restore_irq(level);
}
#else
static void queue_command(int cmd, long idx_id, int tag, long data)
{
    while (1)
    {
        int next;

        mutex_lock(&command_queue_mutex);
        next = command_queue_widx + 1;
        if (next >= TAGCACHE_COMMAND_QUEUE_LENGTH)
            next = 0;

        /* Make sure queue is not full. */
        if (next != command_queue_ridx)
        {
            struct tagcache_command_entry *ce = &command_queue[command_queue_widx];

            ce->command = cmd;
            ce->idx_id = idx_id;
            ce->tag = tag;
            ce->data = data;

            command_queue_widx = next;

            tc_stat.queue_length++;

            mutex_unlock(&command_queue_mutex);
            break;
        }

        /* Queue is full, try again later... */
        mutex_unlock(&command_queue_mutex);
        sleep(1);
    }
}
#endif

long tagcache_increase_serial(void)
{
#ifdef UI_COMMIT
    /* audio thread (track finish): no wait for a publication; the serial
     * is reconstructed from the journal's lastplayed values anyway */
    int level = disable_irq_save();
    struct tagcache_context *ctx = ui_active;
    long old = ctx->stat.ready ? ctx->master.serial++ : -2;
    restore_irq(level);
    if (old < 0)
        return -2;
    /* no CMD_UPDATE_MASTER_HEADER: the serial is rebuilt from the journal's
     * lastplayed values (ui_stats_apply) and carried at publication */
#else
    long old;

    if (!tc_stat.ready)
        return -2;

    while (read_lock)
        sleep(1);

    old = current_tcmh.serial++;
    queue_command(CMD_UPDATE_MASTER_HEADER, 0, 0, 0);
#endif

    return old;
}

/* Publication-operation pins are short UI calls; passive pins may span a
 * background pass and retain only the old namespace, not block publication. */
#ifdef UI_COMMIT
struct tagcache_context *tagcache_context_acquire_passive(void)
{
    int level = disable_irq_save();
    struct tagcache_context *ctx = ui_active;
    bool ok = ui_context_ref(ctx, false);
    restore_irq(level);
    return ok ? ctx : NULL;
}

void tagcache_context_release_passive(struct tagcache_context *ctx)
{
    ui_context_put(ctx, false);
}
#endif

void tagcache_update_numeric(int idx_id, int tag, long data)
{
    queue_command(CMD_UPDATE_NUMERIC, idx_id, tag, data);
#ifdef UI_COMMIT
    ui_stats_gen++;
    ui_stats_tick = current_tick;
#endif
#ifdef TC_QUIET_WAIT
    /* P19 19-1a: a drain the poll would have started now (the disk
     * awake, or the queue half full); else the next storage-idle notice */
    if (tc_quiet_blocked && ui_drain_wanted())
        tagcache_quiet_kick();
#endif
}
#ifdef UI_COMMIT
/* An operation reference without waiting: NULL while a publication runs
 * (audio thread: it must never spin on ui_publishing). */
static struct tagcache_context *ui_context_try_acquire(void)
{
    int level = disable_irq_save();
    struct tagcache_context *ctx = ui_active;
    bool ok = !ui_publishing && ui_context_ref(ctx, true);
    restore_irq(level);
    return ok ? ctx : NULL;
}

/* Which snapshot a row number belongs to: the context and its generation
 * (a context descriptor is reused later, with a higher generation). */
static int32_t ui_bind_token(const struct tagcache_context *ctx)
{
    return (int32_t)(0x80000000u | ((uint32_t)ctx->generation << 1)
                     | (ctx == &ui_context_b));
}

int32_t tagcache_ui_bind_token(const struct tagcache_search *tcs)
{
    return tcs->context ? ui_bind_token(tcs->context) : 0;
}

/* Statistics the producer could not bind from RAM: the tagcache thread
 * binds their full path (find_index(): memo, then the filename tag file)
 * at its next pass (at most a second; no event, which would hide a stop
 * or a reboot from a scan's head-only peek, M3). One 288 B slot, from the
 * heap at the thread's start (rare: a stale snapshot or a memo miss);
 * producers claim it with IRQs off, never wait; busy or missing: dropped
 * and counted. */
#define UI_PATHQ_TAGS  5
struct ui_pathq {
    volatile uint8_t state;     /* 0 free, 1 filling, 2 ready */
    uint8_t count;
    uint8_t tags[UI_PATHQ_TAGS];
    long values[UI_PATHQ_TAGS];
    char path[MAX_PATH];
};
static struct ui_pathq *ui_pathq;

/* Queues the statistics of row 'idx' of 'ctx' (held): false if it is no
 * live row there */
static bool ui_stats_enqueue(struct tagcache_context *ctx, long idx, int count,
                             const unsigned char *tags, const long *values)
{
    if (idx < 0 || idx >= ctx->master.tch.entry_count
        || (ctx->ram.hdr && (ctx->ram.hdr->indices[idx].flag & FLAG_DELETED)))
        return false;
    for (int i = 0; i < count; i++)
        tagcache_update_numeric(idx, tags[i], values[i]);
    return true;
}

static bool ui_pathq_put(const char *path, int count,
                         const unsigned char *tags, const long *values)
{
    struct ui_pathq *q = ui_pathq;
    int level = disable_irq_save();
    if (q && !q->state)
        q->state = 1;
    else
    {
        q = NULL;
        ui_stats_dropped++;     /* logged by the worker */
    }
    restore_irq(level);
    if (!q)
        return false;
    strmemccpy(q->path, path, sizeof(q->path));
    q->count = count;
    memcpy(q->tags, tags, count);
    for (int i = 0; i < count; i++)     /* its serial predates the replay */
        if (tags[i] == tag_lastplayed && !ui_boot_stats_replayed)
            q->tags[i] |= TAGCACHE_UI_REL;
    memcpy(q->values, values, count * sizeof(*values));
    q->state = 2;
#ifdef TC_QUIET_WAIT
    tagcache_quiet_kick();  /* P19 19-1a: bound at the next pass */
#endif
    return true;
}

/* Tagcache thread only (the sole publisher: ui_active cannot change
 * under it). A library not ready yet keeps the request for later. */
static void ui_pathq_run(void)
{
    struct ui_pathq *q = ui_pathq;
    if (!q || q->state != 2)
        return;
    if (ui_shutdown_flushing && ui_generation_cancel(NULL))
        return;                 /* the power-off's deadline came */
    struct tagcache_context *ctx = tagcache_context_acquire();
    if (ctx->stat.ready && !ctx->stat.commit_step)
    {
        if (!ui_stats_enqueue(ctx, find_index(ctx, q->path), q->count,
                              q->tags, q->values))
            ui_log("db", "path lost");
        q->state = 0;
    }
    tagcache_context_release(ctx);
}

bool tagcache_ui_update_stats(const char *path, long idx_id, int32_t token,
                              int count, const unsigned char *tags,
                              const long *values)
{
    if (!path || count <= 0 || count > UI_PATHQ_TAGS)
        return false;
    struct tagcache_context *ctx = ui_context_try_acquire();
    bool ok = false;
    if (ctx)
    {
        long idx = -1;
        if (token && token == ui_bind_token(ctx))
            idx = idx_id;   /* found by full path in this very snapshot */
        else if (path[0])
        {
            struct ui_name_key key;
            ui_name_hash(path, &key);
            idx = ui_fidx_get(ctx, &key);   /* the memo: full-path matches */
        }
        /* without RAM the deleted flag is unknown: the worker decides */
        ok = ctx->ram.hdr && ui_stats_enqueue(ctx, idx, count, tags, values);
        tagcache_context_release(ctx);
    }
    return ok || (path[0] && ui_pathq_put(path, count, tags, values));
}
#endif
#endif /* !__PCTOOL__ */

static bool write_tag(int fd, const char *tagstr, const char *datastr)
{
    char buf[512];
    const int bufsz = sizeof(buf);
    int i;

    snprintf(buf, bufsz, "%s=\"", tagstr);

    for (i = strlen(buf); i < (long)sizeof(buf)-4; i++)
    {
        if (*datastr == '\0')
            break;

        if (*datastr == '"' || *datastr == '\\')
            buf[i++] = '\\';

        else if (*datastr == '\n')
        {
            buf[i++] = '\\';
            buf[i] = 'n';
            continue;
        }

        buf[i] = *(datastr++);
    }

    str_setlen(buf, bufsz - 1);
    strmemccpy(&buf[i], "\" ", (bufsz - i - 1));

    write(fd, buf, i + 2);

    return true;
}

#ifndef __PCTOOL__

static bool read_tag(char *dest, long size,
                     const char *src, const char *tagstr)
{
    int pos;
    char current_tag[32];

    while (*src != '\0')
    {
        /* Skip all whitespace */
        while (*src == ' ')
            src++;

        if (*src == '\0')
            break;

        pos = 0;
        /* Read in tag name */
        while (*src != '=' && *src != ' ')
        {
            current_tag[pos] = *src;
            src++;
            pos++;

            if (*src == '\0' || pos >= (int) sizeof(current_tag))
                return false;
        }

        str_setlen(current_tag, pos);

        /* Read in tag data */

        /* Find the start. */
        while (*src != '"' && *src != '\0')
            src++;

        if (*src == '\0' || *(++src) == '\0')
            return false;

        /* Read the data. */
        for (pos = 0; pos < size; pos++)
        {
            if (*src == '\0')
                break;

            if (*src == '\\')
            {
                src++;
                if (*src == 'n')
                    dest[pos] = '\n';
                else
                    dest[pos] = *src;

                src++;
                continue;
            }

            if (*src == '\0')
                break;

            if (*src == '"')
            {
                src++;
                break;
            }

            dest[pos] = *(src++);
        }

        str_setlen(dest, pos);

        if (!strcasecmp(tagstr, current_tag))
            return true;
    }

    return false;
}

#ifdef UI_COMMIT
/* The import's filename lookup (round-3 review M3). A changelog from an
 * older database is mostly in the new one's index order, which the
 * sequential find_entry_disk(TC_CTX_ARG(ui_build) ) follows, but every line that is out of
 * order (files re-added later) or names a deleted file makes it read the
 * whole filename tag file: 50k tracks with 5% moved and 2% deleted took
 * 53 s in the sim (hours on the device). The table is built once per
 * import from one pass over the filename tag file: two 32-bit hashes of
 * each name (a false match needs both to collide: ~1e-10 at 50k) and
 * the entry's index, sorted, so a line costs a binary search and no
 * disk read until its index entry. 12 B per entry (600 KB at 50k),
 * allocated only when it fits beside playback, at most
 * UI_IMPORT_TABLE_MAX; else the sequential lookup is used. */
#define UI_IMPORT_TABLE_MAX (2 * 1024 * 1024)
static bool ui_import_lookup_io_failed;

static int ui_name_key_cmp(const void *a, const void *b)
{
    const struct ui_name_key *x = a, *y = b;

    if (x->h1 != y->h1)
        return x->h1 < y->h1 ? -1 : 1;
    if (x->h2 != y->h2)
        return x->h2 < y->h2 ? -1 : 1;
    return x->idx_id < y->idx_id ? -1 : x->idx_id > y->idx_id;
}

/* The first row of the table with k's hashes (h1 only for the scan's) */
static long NO_INLINE ui_keys_first(const struct ui_import *imp,
                                    struct ui_name_key *k,
                          bool h1_only)
{
    long lo = 0, hi = imp->count;

    if (h1_only)
        k->h2 = 0;
    k->idx_id = INT32_MIN;
    while (lo < hi)
    {
        long mid = lo + (hi - lo) / 2;
        if (ui_name_key_cmp(&UI_KEYS(imp)[mid], k) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

/* The scan's lookup (scan speed, 1019): the live entry named 'path', its
 * index entry in *idx; -1 if none. A miss reads nothing; a hit reads its
 * filename entry (into 'buf') and its index entry (RAM or ui_scan's fd). */
static long ui_scan_find(const char *path, struct index_entry *idx,
                         char *buf)
{
    struct ui_name_key k;
    struct tagfile_entry fe;
    long len = strlen(path) + 1;

    ui_name_hash(path, &k);
    for (long i = ui_keys_first(&ui_scan, &k, true); i < ui_scan.count; i++)
    {
        const struct ui_name_key e = UI_KEYS(&ui_scan)[i];
        if (e.h1 != k.h1)
            break;
#ifdef UI_TIMING
        ui_scan_reads++;
#endif
        if (lseek(filenametag_fd, e.h2, SEEK_SET) == (off_t)e.h2
            && read_tagfile_entry(ui_build, filenametag_fd, &fe) == sizeof(fe)
            && fe.idx_id == e.idx_id && fe.tag_length == len
            && read(filenametag_fd, buf, len) == len && !memcmp(buf, path, len))
            /* found: an entry that can't be read (deleted, an I/O error)
             * is not added again, as find_entry_disk() + get_index() */
            return get_index(ui_build, ui_scan.masterfd, e.idx_id, idx, true)
                   ? e.idx_id : -2;
    }
    return -1;
}

/* Fills the table from the filename tag file (filenametag_fd); returns
 * the buflib handle (also in imp->handle), 0 when the sequential lookup is
 * to be used, -1 when a power-off or USB stopped it. 'name' is a scratch
 * buffer.
 * The block is movable (#40): with audio stopped it may come out of the
 * idle audio buffer (which holds nearly all free memory, so requiring
 * free room then too would leave the table out, and a realistic 50k
 * import at hours on the device), and a playback started meanwhile can
 * still compact around it; with audio on (or paused) it is taken only
 * from free memory. */
static int ui_import_table(struct ui_import *imp, const struct tagcache_header *tch,
                           char *name, int namesz, bool offsets)
{
    struct tagfile_entry fe;
    size_t need;
    long i, n = 0;
    uint32_t pos = sizeof(struct tagcache_header);
    int handle, rc = e_SUCCESS;

    imp->handle = 0;
    if (filenametag_fd < 0 || tch->entry_count <= 0)
        return 0;
    need = (size_t)tch->entry_count * sizeof(struct ui_name_key);
    /* never from playback: with audio on (or paused), only free memory */
    if (need > UI_IMPORT_TABLE_MAX || !ui_audio_room(need + 64 * 1024))
    {
        DEBUGF("tagcache: import without lookup table (%lu B)\n",
               (unsigned long)need);
        return 0;
    }
    handle = core_alloc(need);
    if (handle <= 0)
        return 0;

    lseek(filenametag_fd, sizeof(struct tagcache_header), SEEK_SET);
    for (i = 0; i < tch->entry_count; i++)
    {
        rc = read_tagfile_entry_and_tag(UI_IMPORT_CTX, filenametag_fd, &fe, name, namesz);
        if (rc == e_SUCCESS)
        {
            struct ui_name_key *k =
                (struct ui_name_key *)core_get_data(handle) + n;
            ui_name_hash(name, k);
            if (offsets)
                k->h2 = pos;    /* the scan's: verified by a read there */
            k->idx_id = fe.idx_id;
            n++;
        }
        else if (rc != e_SUCCESS_LEN_ZERO)
            break;
        pos += sizeof(fe) + fe.tag_length;
        if (do_timed_yield() && STOP_REQUESTED())
            rc = -1;
        if (rc == -1)
            break;
    }
    if (rc != e_SUCCESS && rc != e_SUCCESS_LEN_ZERO)
    {
        /* stopped (-1), or a read error: the sequential lookup copes */
        core_free(handle);
        return rc == -1 ? -1 : 0;
    }
    qsort(core_get_data(handle), n, sizeof(struct ui_name_key),
          ui_name_key_cmp);
    imp->count = n;
    imp->handle = handle;
    return handle;
}

/* The entry of 'name' (the live one if the path is also in the database
 * as a deleted entry), its index entry in *idx; -1 if none */
static long ui_import_lookup(const struct ui_import *imp, const char *name,
                             struct index_entry *idx)
{
    struct ui_name_key k;
    struct index_entry first;
    struct tagcache_context *ctx = UI_IMPORT_CTX;
    long lo, found = -1;

    ui_name_hash(name, &k);
    lo = ui_keys_first(imp, &k, false);
    /* get_index(TC_CTX_ARG(ui_build) ) reads the disk (may yield): UI_KEYS() each time */
    for (; lo < imp->count && UI_KEYS(imp)[lo].h1 == k.h1
           && UI_KEYS(imp)[lo].h2 == k.h2; lo++)
    {
        int32_t id = UI_KEYS(imp)[lo].idx_id;

        off_t offset = sizeof(struct master_header)
            + (off_t)id * sizeof(*idx);
        if (id < 0 || id >= ctx->master.tch.entry_count
            || lseek(imp->masterfd, offset, SEEK_SET) != offset
            || read_index_entries(ctx, imp->masterfd, idx, 1)
                    != sizeof(*idx))
        {
            ui_import_lookup_io_failed = true;
            continue;
        }
        if (idx->flag & FLAG_DELETED)
            continue;
        char actual[TAGCACHE_BUFSZ];
        if (!ui_entry_filename(ctx, id, actual, sizeof(actual)))
        {
            ui_import_lookup_io_failed = true;
            continue;
        }
        if (strcmp(actual, name))
            continue;
        if (!(idx->flag & FLAG_DELETED))
            return id;
        if (found < 0)
        {
            found = id;
            first = *idx;
        }
    }
    if (found >= 0)
        *idx = first;           /* only deleted entries have that name */
    return found;
}

static struct ui_import ui_stats_lookup = { .masterfd = -1 };
static bool ui_stats_prepared;
static struct dbjournal_cursor ui_stats_cursor;

static enum dbjournal_result ui_stats_apply(void *opaque, const char *filename,
    uint32_t tag, int32_t absolute, uint64_t sequence)
{
    struct ui_import *imp = opaque;
    struct tagcache_context *ctx = UI_IMPORT_CTX;
    struct index_entry idx;
    int t = UI_STAT_TAG(tag);
    (void)sequence;
    if ((tag & ~(uint32_t)(TAGCACHE_UI_DELTA | TAGCACHE_UI_REL | 0x3f))
        || !ui_runtime_tag(t))
        return DBJOURNAL_CORRUPT;
    /* a candidate's master takes absolutes only: the active snapshot's
     * replay adds deltas and relative values onto it (no lookup I/O) */
    if (ctx != ui_active && tag != (uint32_t)t)
        return DBJOURNAL_OK;
    ui_import_lookup_io_failed = false;
    long id = ui_import_lookup(imp, filename, &idx);
    if (ui_import_lookup_io_failed)
        return DBJOURNAL_IO;
    if (id < 0)
        return DBJOURNAL_OK;       /* removed tracks retain their journal data */
    if (ctx != ui_active)
    {
        if (t == tag_lastplayed && absolute >= 0 && absolute < INT32_MAX)
            ctx->master.serial = MAX(ctx->master.serial, absolute + 1);
        /* idx: just read from this master by the lookup */
        if (idx.tag_seek[t] == absolute)
            return DBJOURNAL_OK;
        idx.tag_seek[t] = absolute;
        idx.flag |= FLAG_DIRTYNUM | FLAG_UI_STATS_FOLDED;
        bool changed = ui_candidate_changed;    /* statistics only */
        bool ok = write_index(ctx, imp->masterfd, id, &idx);
        ui_candidate_changed = changed;
        return ok ? DBJOURNAL_OK : DBJOURNAL_IO;
    }
    if (!ctx->stat.ramcache || !ctx->ram.hdr)
        return DBJOURNAL_INVALID;
    struct index_entry *row = &ctx->ram.hdr->indices[id];
    if (tag & TAGCACHE_UI_REL)          /* after every lastplayed so far */
        absolute = ctx->master.serial;
    if (t == tag_lastplayed && absolute >= 0 && absolute < INT32_MAX)
        ctx->master.serial = MAX(ctx->master.serial, absolute + 1);
    if (tag != (uint32_t)t)
        row->flag |= FLAG_UI_STATS_REPLAYED;
    row->tag_seek[t] = (tag & TAGCACHE_UI_DELTA)
                       ? row->tag_seek[t] + absolute : absolute;
    row->flag |= FLAG_DIRTYNUM;
    return DBJOURNAL_OK;
}

static void ui_candidate_stats_release(void)
{
    if (ui_stats_lookup.masterfd >= 0)
        close(ui_stats_lookup.masterfd);
    if (ui_stats_lookup.handle > 0)
        core_free(ui_stats_lookup.handle);
    memset(&ui_stats_lookup, 0, sizeof(ui_stats_lookup));
    ui_stats_lookup.masterfd = -1;
    ui_stats_prepared = false;
}

/* The boot replay's end, with no yield since its last frame. Done: the
 * updates queued during the replay reach RAM (a relative lastplayed after
 * every one replayed). Failed: the rows deltas or relative values
 * changed get their statistics back from the master on disk, so a retry
 * applies every frame once (the replay is retried with backoff). */
static void ui_stats_replay_end(bool done, int masterfd)
{
    struct tagcache_context *ctx = ui_active;
    bool lost = false;
    for (int row = 0; row < ctx->master.tch.entry_count; row++)
    {
        struct index_entry *idx = &ctx->ram.hdr->indices[row];
        if (!done && (idx->flag & FLAG_UI_STATS_REPLAYED))
        {
            struct index_entry disk;
            if (lseek(masterfd, sizeof(struct master_header)
                      + row * sizeof(disk), SEEK_SET) >= 0
                && read_index_entries(ctx, masterfd, &disk, 1) == sizeof(disk))
            {
                for (int t = 0; t < TAG_COUNT; t++)
                    if (ui_runtime_tag(t))
                        idx->tag_seek[t] = disk.tag_seek[t];
            }
            else
                lost = true;
        }
        idx->flag &= ~FLAG_UI_STATS_REPLAYED;
    }
    /* the updates queued during the replay reach RAM (a relative
     * lastplayed takes the next serial); after a failure, this session's
     * queued absolutes (resume, rating) go back over the restored rows */
    for (int r = command_queue_ridx; r != command_queue_widx;
         r = (r + 1) % TAGCACHE_COMMAND_QUEUE_LENGTH)
    {
        struct tagcache_command_entry *ce = &command_queue[r];
        long v = ce->data;
        if (ce->tag & (TAGCACHE_UI_DELTA | TAGCACHE_UI_REL) && !done)
            continue;
        if (ce->tag & TAGCACHE_UI_REL)
            v = ctx->master.serial++;
        ui_ram_numeric(ce->context, ce->idx_id, ce->tag & ~TAGCACHE_UI_REL,
                       v, FLAG_DIRTYNUM);
    }
    if (done)
    {
        ui_boot_stats_replayed = true;
        ui_retry[UI_RETRY_REPLAY].delay = 0;
    }
    else if (lost)
    {
        /* rows hold part of the replay: never retry onto them (MED-4) */
        ctx->stat.ramcache = false;
        ctx->generation++;          /* open views reread */
        ui_reload_pending = ui_busy = true;
        ui_log("db", "restore: reload");
    }
}

/* The boot replay over the serial the master was loaded with, not with
 * this boot's pre-replay increase_serial() bumps: a relative lastplayed
 * from an earlier boot must sort below that boot's later plays (LOW-2).
 * The RAM serial is the larger of both after it. */
static bool ui_stats_replay_boot(struct dbjournal *snapshot, bool ok,
                                 long loaded)
{
    struct tagcache_context *ctx = ui_active;
    long bumped = ctx->master.serial;
    ctx->master.serial = loaded;
    ok = ok && dbjournal_replay(snapshot, ui_stats_apply, &ui_stats_lookup)
                == DBJOURNAL_OK;
    ctx->master.serial = MAX(ctx->master.serial, bumped);
    ui_stats_replay_end(ok, ui_stats_lookup.masterfd); /* no yield since */
    return ok;
}

static bool ui_candidate_stats_prepare(void)
{

    struct ui_generation_io io;
    struct master_header mh;
    struct tagcache_header th;
    char filename[TAGCACHE_BUFSZ];
    struct dbjournal snapshot;
    int h;
    struct tagcache_context *ctx = UI_IMPORT_CTX;
    bool boot = ctx == ui_active;
    if (ui_stats_prepared)
        return true;
    ui_stats_lookup.masterfd = open_master_fd(ctx, &mh, !boot);
    if (ui_stats_lookup.masterfd < 0)
        return false;
    filenametag_fd = open_tag_fd(ctx, &th, tag_filename, false);
    int table = filenametag_fd < 0 ? -1
        : ui_import_table(&ui_stats_lookup, &th, filename, sizeof(filename),
                          false);
    if (filenametag_fd >= 0)
        close(filenametag_fd);
    filenametag_fd = -1;
    if (table < 0 || (th.entry_count > 0 && table == 0))
    {
        ui_candidate_stats_release();
        return false;
    }
    h = ui_generation_buffer(&io);
    if (h <= 0)
    {
        ui_candidate_stats_release();
        return false;
    }
    mutex_lock(&command_queue_mutex);
    bool ok = ui_command_drain_locked(io.buffer, io.size, true);
    snapshot = ui_stats_journal;
    ui_stats_journal.scratch = NULL;
    mutex_unlock(&command_queue_mutex);
    snapshot.scratch = io.buffer;
    snapshot.bytes = io.size;
    if (boot)
        ok = ui_stats_replay_boot(&snapshot, ok, mh.serial);
    else if (ok)
        ok = dbjournal_replay(&snapshot, ui_stats_apply, &ui_stats_lookup)
                == DBJOURNAL_OK;
    if (ok)
    {
        dbjournal_checkpoint(&snapshot, &ui_stats_cursor);
        ok = boot || fsync(ui_stats_lookup.masterfd) == 0;
    }
    core_free(h);
    audio_buffer_regrow();
    if (!ok)
        ui_candidate_stats_release();
    ui_stats_prepared = ok;
    if (ok)
        ui_stats_restart = false;
    return ok;
}


static bool ui_candidate_stats_finish(void *buffer, size_t bytes)
{
    if (!ui_stats_prepared || !ui_command_drain_locked(buffer, bytes, false))
        return false;
    enum dbjournal_result rc = dbjournal_replay_since(&ui_stats_journal,
        &ui_stats_cursor, ui_stats_apply, &ui_stats_lookup);
    bool ok = rc == DBJOURNAL_OK;
    if (rc == DBJOURNAL_INVALID)
        ui_stats_restart = true;
    if (ok)
    {
        current_tcmh.serial = MAX(current_tcmh.serial, ui_active->master.serial);
        ok = update_master_header(ui_build)
            && fsync(ui_stats_lookup.masterfd) == 0;
    }
    ui_stats_journal.scratch = NULL;
    return ok;
}
#endif /* UI_COMMIT */

static int parse_changelog_line(int line_n, char *buf, void *parameters)
{
    struct index_entry idx;
    char tag_data[TAGCACHE_BUFSZ];
    int idx_id;
#ifdef UI_COMMIT
    const struct ui_import *imp = parameters;
    long masterfd = imp->masterfd;
    bool have_idx = false;
#else
    long masterfd = (long)(intptr_t)parameters;
#endif
    const int import_tags[] = { tag_playcount, tag_rating, tag_playtime,
                                tag_lastplayed, tag_commitid, tag_lastelapsed,
                                tag_lastoffset };
    int i;
    (void)line_n;

    if (*buf == '#')
        return 0;

    /* logf("%d/%s", line_n, buf); */
    if (!read_tag(tag_data, sizeof tag_data, buf, "filename"))
    {
        logf("%d/filename missing", line_n);
        logf("-> %s", buf);
        return 0;
    }

#ifdef UI_COMMIT
    /* a power-off or USB stops the import (the marker file keeps it for
     * the next boot); it never holds write_lock for long */
    if (STOP_REQUESTED())
        return -6;
    do_timed_yield();
    /* find_index(TC_CTX_ARG(ui_build) ) reopens the filename tag file and reads it from the
     * start for every line: quadratic. The import's open file and the
     * position of the last match make it near-linear, as exports are in
     * index order, like the filename tag file; the lookup table
     * (ui_import_table()) makes lines out of order, and deleted files,
     * as cheap as the rest. */
    idx_id = -1;
    if (imp->handle > 0)
    {
        idx_id = ui_import_lookup(imp, tag_data, &idx);
        have_idx = true;
    }
    else
    {
#if defined(HAVE_TC_RAMCACHE) && defined(HAVE_DIRCACHE)
        idx_id = find_entry_ram(TC_CTX_ARG(ui_build) tag_data);
#endif
        if (idx_id < 0)
            idx_id = find_entry_disk(TC_CTX_ARG(ui_build) tag_data, false);
    }
#else
    idx_id = find_index(TC_CTX_ARG(ui_build) tag_data);
#endif
    if (idx_id < 0)
    {
        logf("%d/entry not found", line_n);
        return 0;
    }

    if (
#ifdef UI_COMMIT
        !have_idx &&
#endif
        !get_index(TC_CTX_ARG(ui_build) masterfd, idx_id, &idx, false))
    {
        logf("%d/failed to retrieve index entry", line_n);
        return 0;
    }

    /* Stop if tag has already been modified. (In the shell: an entry
     * whose statistics changed in the new database before the import got
     * to it -- played, or resumed, between the commit and the import --
     * keeps the new values and loses the changelog's, e.g. an older play
     * count. It also makes a second import of the same changelog a no-op;
     * docs/hardware.md, changelog row.) */
    if (idx.flag & FLAG_DIRTYNUM)
        return 0;

    logf("%d/import: %s", line_n, tag_data);

    idx.flag |= FLAG_DIRTYNUM;
    for (i = 0; i < (long)(sizeof(import_tags)/sizeof(import_tags[0])); i++)
    {
        int data;

        if (!read_tag(tag_data, sizeof tag_data, buf,
                      tagcache_tag_to_str(import_tags[i])))
        {
            continue;
        }

        data = atoi(tag_data);
        if (data < 0)
            continue;

        idx.tag_seek[import_tags[i]] = data;

        if (import_tags[i] == tag_lastplayed && data >= current_tcmh.serial)
            current_tcmh.serial = data + 1;
#ifndef UI_COMMIT
        /* the shell's commit IDs are generation stamps, seeded above every
         * export at a fresh build: an import that raised it after the load
         * left the precomputed lists' stamp behind (loaded_commitid), so
         * the candidate failed validation and was dropped, for ever */
        else if (import_tags[i] == tag_commitid && data >= current_tcmh.commitid)
            current_tcmh.commitid = data + 1;
#endif
    }

    return write_index(TC_CTX_ARG(ui_build) masterfd, idx_id, &idx) ? 0 : -5;
}

bool tagcache_import_changelog(void)
{
    struct master_header myhdr;
    struct tagcache_header tch;
    int clfd;
    long masterfd;
    char buf[2048];
    const int bufsz = sizeof(buf);

    if (!tc_stat.ready)
        return false;

    while (read_lock)
        sleep(1);

#ifdef UI_COMMIT
    ui_export_recover(buf);
    clfd = ui_changelog_open(buf);
#else
    clfd = open_db_fd(TC_CTX_ARG(ui_build) TAGCACHE_FILE_CHANGELOG, O_RDONLY);
#endif
    if (clfd < 0)
    {
        logf("failure to open changelog");
#ifdef UI_COMMIT
        remove_db_file(TC_CTX_ARG(ui_build) TAGCACHE_FILE_IMPORT);   /* nothing to import */
        ui_import_pending = false;
#endif
        return false;
    }

    if ( (masterfd = open_master_fd(TC_CTX_ARG(ui_build) &myhdr, true)) < 0)
    {
        close(clfd);
        return false;
    }

    write_lock++;

    filenametag_fd = open_tag_fd(TC_CTX_ARG(ui_build) &tch, tag_filename, false);

#ifdef UI_COMMIT
#ifdef UI_TIMING
    long ui_start = current_tick;
#endif
    struct ui_import imp = { .masterfd = masterfd, .handle = 0, .count = 0 };
    int ui_handle = ui_import_table(&imp, &tch, buf, bufsz, false);
#ifdef UI_TIMING
    long ui_table_ms = (current_tick - ui_start) * 1000 / HZ;
#endif
    int ui_rc = ui_handle < 0 ? -6
        : fast_readline(clfd, buf, bufsz, &imp, parse_changelog_line);
    if (ui_handle > 0)
        core_free(ui_handle);
#ifdef UI_TIMING
    DEBUGF("tagcache: changelog import %s in %ld ms (lookup table %s, "
           "%ld ms)\n", ui_rc == -6 ? "stopped" : "done",
           (current_tick - ui_start) * 1000 / HZ,
           ui_handle > 0 ? "yes" : "no", ui_table_ms);
#endif
    if (ui_rc != -6)            /* else the next boot resumes it */
    {
        remove_db_file(TC_CTX_ARG(ui_build) TAGCACHE_FILE_IMPORT);
        ui_import_pending = false;
    }
#else
    fast_readline(clfd, buf, bufsz, (void *)(intptr_t)masterfd,
                  parse_changelog_line);
#endif

    close(clfd);
    close(masterfd);

    if (filenametag_fd >= 0)
    {
        close(filenametag_fd);
        filenametag_fd = -1;
    }

    write_lock--;

    update_master_header(TC_CTX_PASS(ui_build));

    return true;
}

#endif /* !__PCTOOL__ */

bool tagcache_create_changelog(struct tagcache_search *tcs)
{
    struct master_header myhdr;
    struct index_entry idx;
    char buf[TAGCACHE_BUFSZ];
    const int bufsz = sizeof(buf);
    char temp[32];
    int clfd;
    int i, j;

    if (!tc_stat.ready)
        return false;

    if (!tagcache_search(tcs, tag_filename))
        return false;

    /* Initialize the changelog */
    clfd = open_db_fd(TC_CTX_ARG(ui_build) TAGCACHE_FILE_CHANGELOG, O_WRONLY | O_CREAT | O_TRUNC);
    if (clfd < 0)
    {
        logf("failure to open changelog");
        tagcache_search_finish(tcs);
        return false;
    }

    if (tcs->masterfd < 0)
    {
        if ( (tcs->masterfd = open_master_fd(TC_CTX_ARG(ui_build) &myhdr, false)) < 0)
        {
            close(clfd);
            tagcache_search_finish(tcs);
            return false;
        }
    }
    else
    {
        lseek(tcs->masterfd, 0, SEEK_SET);
        read_master_header(TC_CTX_ARG(ui_build) tcs->masterfd, &myhdr);
    }

    write(clfd, "## Changelog version 1\n", 23);

    for (i = 0; i < myhdr.tch.entry_count; i++)
    {
        if (read_index_entries(TC_CTX_ARG(ui_build) tcs->masterfd, &idx, 1) != sizeof(struct index_entry))
        {
            logf("read error #9");
            tagcache_search_finish(tcs);
            close(clfd);
            return false;
        }

        /* Skip until the entry found has been modified. */
        if (! (idx.flag & FLAG_DIRTYNUM) )
            continue;

        /* Skip deleted entries too. */
        if (idx.flag & FLAG_DELETED)
            continue;

        /* Now retrieve all tags. */
        for (j = 0; j < TAG_COUNT; j++)
        {
            if (TAGCACHE_IS_NUMERIC(j))
            {
                itoa_buf(temp, sizeof temp, (int)idx.tag_seek[j]);
                write_tag(clfd, tagcache_tag_to_str(j), temp);
                continue;
            }

            tcs->type = j;
            tagcache_retrieve(tcs, i, tcs->type, buf, bufsz);
            write_tag(clfd, tagcache_tag_to_str(j), buf);
        }

        write(clfd, "\n", 1);
        do_timed_yield();
    }

    close(clfd);

    tagcache_search_finish(tcs);

    return true;
}

#ifdef UI_COMMIT
/* The export's output chunk (tagcache thread stack): lines are built in
 * it and written out when the next worst-case line might not fit, so a
 * 50k export makes ~10k writes, not ~900k (one per tag). */
#define UI_EXPORT_CHUNK 1536
/* What the import reads (parse_changelog_line()): the export writes
 * only these, and only when non-zero (a missing tag keeps the new
 * database's value, which is 0 for an entry the import may change) */
static const unsigned char ui_export_tags[] = {
    tag_playcount, tag_rating, tag_playtime, tag_lastplayed, tag_commitid,
    tag_lastelapsed, tag_lastoffset
};

/* Appends tag="value" (escaped like write_tag()) at out + n. */
static int ui_export_put(char *out, int n, int tag, const char *val)
{
    const char *t = tagcache_tag_to_str(tag);

    while (*t)
        out[n++] = *t++;
    out[n++] = '=';
    out[n++] = '"';
    for (; *val; val++)
    {
        if (*val == '"' || *val == '\\')
            out[n++] = '\\';
        if (*val == '\n')
        {
            out[n++] = '\\';
            out[n++] = 'n';
            continue;
        }
        out[n++] = *val;
    }
    out[n++] = '"';
    out[n++] = ' ';
    return n;
}

/* An export cut between its remove() and rename() (a power cut) left
 * only the new file: it is complete when it ends with UI_EXPORT_END.
 * Before an import or an export. 'name' is a TAGCACHE_BUFSZ buffer. */
static void ui_export_recover(char *name)
{
    char *from = name, *to = name + MAX_PATH;
    char tail[sizeof(UI_EXPORT_END) - 1];
    int fd;
    bool whole = false;

    snprintf(from, MAX_PATH, "%s/" TAGCACHE_FILE_CHANGELOG ".new",
             tc_stat.db_path);
    snprintf(to, MAX_PATH, "%s/" TAGCACHE_FILE_CHANGELOG, tc_stat.db_path);
    if (file_exists(to) || (fd = open(from, O_RDONLY)) < 0)
        return;
    if (lseek(fd, -(off_t)sizeof(tail), SEEK_END) >= 0
        && read(fd, tail, sizeof(tail)) == (ssize_t)sizeof(tail))
        whole = !memcmp(tail, UI_EXPORT_END, sizeof(tail));
    close(fd);
    if (whole)
    {
        logf("tagcache: changelog recovered from its new file");
        rename(from, to);
    }
}

/* Saves the runtime statistics (play counts, ratings, resume positions) of
 * every entry that has any to the changelog that a rebuild imports (the
 * stock "Export Modifications" file), so a rebuild -- a new database
 * format, a new SORTKEY_VERSION, an interrupted commit -- keeps them.
 * Unlike tagcache_create_changelog() it needs no search, also reads a
 * dirty database, writes only what the import reads (the filename and the
 * non-zero statistics), never replaces the changelog with an empty one,
 * and writes a temp file first (renamed when complete, with an end line:
 * ui_export_recover()). Reads the master on disk only. Tagcache thread only: before a rebuild, at USB connect,
 * and in the background while stopped on external power
 * (ui_export_idle()); not at shutdown (Phase 5: with runtimedb on,
 * statistics change whenever anything plays, and a 50k export takes
 * minutes on the device). Never while a commit, an import or a search
 * runs; it holds write_lock (as a reader) so that no commit starts
 * meanwhile. 'stoppable': gives up at a power-off, USB or playback,
 * keeping the old changelog. 'name' is a TAGCACHE_BUFSZ buffer. */
static int ui_export_to(char *name, bool wait, bool stoppable)
{
    static bool busy;
    struct master_header mh;
    struct tagcache_header th;
    struct tagfile_entry fe;
    struct index_entry idx;
    char out[UI_EXPORT_CHUNK];
    int masterfd, namefd, clfd = -1, n = 0;
    long i, written UNUSED_ATTR = 0, skipped = 0, t0 UNUSED_ATTR;
    long gen = ui_stats_gen;
    bool ok = true, stopped UNUSED_ATTR = false;

    /* the tagcache thread lets a running search finish (a few s at most) */
    for (i = 0; wait && i < 5 * HZ && (read_lock || write_lock); i++)
        sleep(1);
    if (busy || ui_import_pending || read_lock || write_lock
        || tc_stat.commit_step > 0)
    {
        DEBUGF("tagcache: changelog export skipped (busy)\n");
        return -1;
    }
    ui_export_recover(name);
    masterfd = open_master_fd(TC_CTX_ARG(ui_build) &mh, false);
    /* 0: unreadable for good (damaged, an older format, none): keep
     * the old changelog; -1: I/O that a retry may get past (LOW-a) */
    if (masterfd < 0)
        return masterfd == -2 || errno == ENOENT ? 0 : -1;
    namefd = open_tag_fd(TC_CTX_ARG(ui_build) &th, tag_filename, false);
    if (namefd < 0)
    {
        close(masterfd);
        return namefd == -2 || errno == ENOENT ? 0 : -1;
    }
    busy = true;
    write_lock++;
    t0 = current_tick;

    for (i = 0; i < mh.tch.entry_count && ok; i++)
    {
        unsigned t;

        if (stoppable && (STOP_REQUESTED() || audio_status()))
        {
            stopped = true;
            ok = false;
            break;
        }
        /* the master on disk only: RAM holds the journal's deltas, which
         * a rebuild's replay adds again after importing this file (the
         * changelog lags until a publication folds the journal's
         * absolutes) */
        if (lseek(masterfd, sizeof(mh) + i * sizeof(idx), SEEK_SET) < 0
                 || read_index_entries(TC_CTX_ARG(ui_build) masterfd, &idx, 1) != sizeof(idx))
        {
            ok = false;
            break;
        }

        if (!(idx.flag & FLAG_DIRTYNUM) || (idx.flag & FLAG_DELETED))
            continue;
        /* A row with statistics whose name is damaged (an offset outside
         * the name file, R8a, or a bad entry): this export is not
         * complete (U1), never "nothing to export". Stop at once: no end
         * line, so a power cut can never leave a complete-looking partial
         * file (R8b). A seek that fails inside the file is I/O: retried. */
        int32_t at = idx.tag_seek[tag_filename];
        if (at < (int32_t)sizeof(th)
            || at >= (int32_t)sizeof(th) + th.datasize)
        {
            skipped = 1;
            break;
        }
        if (lseek(namefd, at, SEEK_SET) < 0)
        {
            if (errno == EINVAL)
                skipped = 1;        /* past the real end: damage too */
            else
                ok = false;
            break;
        }
        if (read_tagfile_entry_and_tag(TC_CTX_ARG(ui_build) namefd, &fe, name,
                                       TAGCACHE_BUFSZ) != e_SUCCESS)
        {
            skipped = 1;
            break;
        }

        if (clfd < 0)
        {
            clfd = open_db_fd(TC_CTX_ARG(ui_build) TAGCACHE_FILE_CHANGELOG ".new",
                              O_WRONLY | O_CREAT | O_TRUNC);
            if (clfd < 0)
            {
                ok = false;
                break;
            }
            /* the commit read: the newest export wins an import (7b) */
            n = snprintf(out, UI_EXPORT_CHUNK,
                         "## Changelog version 1\n## commit %ld\n",
                         (long)mh.commitid);
        }
        /* room for the worst case: the name escaped (2x) + 7 tags */
        if (n + 2 * (int)strlen(name) + 16 + 7 * 28 + 2 > UI_EXPORT_CHUNK)
        {
            if (write(clfd, out, n) != n)
            {
                ok = false;
                break;
            }
            n = 0;
        }
        n = ui_export_put(out, n, tag_filename, name);
        for (t = 0; t < ARRAYLEN(ui_export_tags); t++)
        {
            char temp[12];
            int v = (int)idx.tag_seek[ui_export_tags[t]];

            if (v == 0)
                continue;
            itoa_buf(temp, sizeof temp, v);
            n = ui_export_put(out, n, ui_export_tags[t], temp);
        }
        out[n++] = '\n';
        written++;
        do_timed_yield();
    }

    close(namefd);
    close(masterfd);
    if (clfd >= 0)
    {
        /* 'name' holds both paths (TAGCACHE_BUFSZ > 2 * MAX_PATH) */
        char *from = name, *to = name + MAX_PATH;

        if (ok && !skipped && n + (int)sizeof(UI_EXPORT_END) > UI_EXPORT_CHUNK)
        {
            ok = write(clfd, out, n) == n;
            n = 0;
        }
        if (ok && !skipped)
        {
            memcpy(out + n, UI_EXPORT_END, sizeof(UI_EXPORT_END) - 1);
            n += sizeof(UI_EXPORT_END) - 1;
            ok = write(clfd, out, n) == n;
        }
        ok = close(clfd) == 0 && ok;
        snprintf(from, MAX_PATH, "%s/" TAGCACHE_FILE_CHANGELOG ".new",
                 tc_stat.db_path);
        snprintf(to, MAX_PATH, "%s/" TAGCACHE_FILE_CHANGELOG,
                 tc_stat.db_path);
        if (ok && !skipped)
        {
            remove(to);
            ok = rename(from, to) >= 0;
        }
        else
            remove(from);           /* a partial one never replaces */
    }
    logf("tagcache: changelog export %ld entries: %s", written,
         ok ? "ok" : "failed");
    DEBUGF("tagcache: changelog export: %ld entries in %ld ms%s\n", written,
           (current_tick - t0) * 1000 / HZ,
           stopped ? " (stopped)" : ok ? "" : " (failed)");
    if (ok && !skipped)
        ui_stats_exported = gen;    /* a change made meanwhile is owed */
    /* damaged names stay damaged until a publication or rebuild: the
     * idle and post-USB exports leave this library alone (R8b) */
    ui_export_bad = ok && skipped ? mh.commitid : -1;
    write_lock--;
    busy = false;
    /* 1: complete, 2: complete with no row to export (no file written),
     * 0: rows with statistics unreadable (damaged names: the import takes
     * the best older changelog instead), -1: failed or stopped */
    return !ok ? -1 : skipped ? 0 : 1 + !written;
}

/* On the tagcache thread (a large stack): the buffers are local. Updates
 * still queued (tagcache_update_numeric()) are written out first: the
 * export reads the master index, and before a rebuild they would land on
 * the new database's entries of the same number. */
static int NO_INLINE ui_export_changelog_ex(bool stoppable)
{
    char name[TAGCACHE_BUFSZ];

    run_command_queue(true);
    return ui_export_to(name, !stoppable, stoppable);
}

static int ui_export_changelog(void)
{
    return ui_export_changelog_ex(false);
}

/* When statistics changed: after USB releases the filesystem (tagcache
 * thread). It must never run before the USB acknowledgement: an export can
 * take seconds on the device, while the host is waiting for storage now. */
static void ui_export_if_changed(void)
{
    if (ui_stats_gen != ui_stats_exported && tc_stat.ready
        && ui_export_bad != current_tcmh.commitid)
        ui_export_changelog();
}

/* The background export (Phase 5, #4). On the tagcache thread's 1 s
 * timeout, once statistics changed: only while audio is stopped, on
 * external power (no battery cost), with nothing else pending,
 * UI_EXPORT_QUIET after the last change. Stops at a power-off, USB or
 * playback (the old changelog stays) and is tried again later. */
#define UI_EXPORT_QUIET (60 * HZ)
static void ui_export_idle(void)
{
    if (ui_stats_gen == ui_stats_exported || !tc_stat.ready
        || ui_export_bad == current_tcmh.commitid || ui_import_pending
        || ui_commit_pending || ui_reload_pending
        || read_lock || write_lock || audio_status()
        || TIME_BEFORE(current_tick, ui_stats_tick + UI_EXPORT_QUIET))
        return;
#if CONFIG_CHARGING
    if (!charger_inserted())
        return;
    ui_export_changelog_ex(true);
    ui_stats_tick = current_tick;   /* a stopped one waits again */
#endif
}
#endif /* UI_COMMIT */

static bool delete_entry(long idx_id)
{
    int fd = -1;
    int masterfd = -1;
    int tag, i;
    struct index_entry idx, myidx;
    struct master_header myhdr;
    int in_use[TAG_COUNT];

    logf("delete_entry(): %ld", idx_id);

#ifdef UI_COMMIT
    /* The shell's precomputed lists still have this entry, and their
     * stamp (commit id, entry count) does not change with a deletion:
     * removed first, once per pass; the pass's end precomputes them
     * again (ui_flush_deleted(), check_file_refs()). */
    if (!ui_libfile_dropped)
    {
        remove_db_file(TC_CTX_ARG(ui_build) LIBFILE_NAME);
        ui_libfile_dropped = true;
    }
#endif
#ifdef HAVE_TC_RAMCACHE
    /* At first mark the entry removed from ram cache. */
    if (tc_stat.ramcache)
    {
        tcramcache.hdr->indices[idx_id].flag |= FLAG_DELETED;
#ifdef HAVE_IPOD_UI
        /* changed in place (no reload follows): open views are stale,
         * told once the pass is over (ui_flush_deleted()) */
        ui_deleted = true;
#endif
    }
#endif

    if ( (masterfd = open_master_fd(TC_CTX_ARG(ui_build) &myhdr, true) ) < 0)
        return false;

    lseek(masterfd, idx_id * sizeof(struct index_entry), SEEK_CUR);
    if (read_index_entries(TC_CTX_ARG(ui_build) masterfd, &myidx, 1) != sizeof(struct index_entry))
    {
        logf("delete_entry(): read error");
        goto cleanup;
    }

    if (myidx.flag & FLAG_DELETED)
    {
        logf("delete_entry(): already deleted!");
        goto cleanup;
    }

    myidx.flag |= FLAG_DELETED;
    lseek(masterfd, -(off_t)sizeof(struct index_entry), SEEK_CUR);
    if (write_index_entries(TC_CTX_ARG(ui_build) masterfd, &myidx, 1) != sizeof(struct index_entry))
    {
        logf("delete_entry(): write_error #1");
        goto cleanup;
    }

    /* Now check which tags are no longer in use (if any) */
    for (tag = 0; tag < TAG_COUNT; tag++)
        in_use[tag] = 0;

    lseek(masterfd, sizeof(struct master_header), SEEK_SET);
    for (i = 0; i < myhdr.tch.entry_count; i++)
    {
        struct index_entry *idxp;

#ifdef HAVE_TC_RAMCACHE
        /* Use RAM DB if available for greater speed */
        if (tc_stat.ramcache)
            idxp = &tcramcache.hdr->indices[i];
        else
#endif
        {
            if (read_index_entries(TC_CTX_ARG(ui_build) masterfd, &idx, 1) != sizeof(struct index_entry))
            {
                logf("delete_entry(): read error #2");
                goto cleanup;
            }
            idxp = &idx;
        }

        if (idxp->flag & FLAG_DELETED)
            continue;

        for (tag = 0; tag < TAG_COUNT; tag++)
        {
            if (TAGCACHE_IS_NUMERIC(tag))
                continue;

            if (idxp->tag_seek[tag] == myidx.tag_seek[tag])
                in_use[tag]++;
        }
    }

    /* Now delete all tags no longer in use. */
    for (tag = 0; tag < TAG_COUNT; tag++)
    {
        struct tagcache_header tch;
        int oldseek = myidx.tag_seek[tag];

        if (TAGCACHE_IS_NUMERIC(tag))
            continue;

        /**
         * Replace tag seek with a hash value of the field string data.
         * That way runtime statistics of moved or altered files can be
         * resurrected.
         */
#ifdef HAVE_TC_RAMCACHE
        if (tc_stat.ramcache && tag != tag_filename)
        {
            struct tagfile_entry *tfe;
            int32_t *seek = &tcramcache.hdr->indices[idx_id].tag_seek[tag];

            /* crc_32 is assumed not to yield (why would it...?) */
            tfe = (struct tagfile_entry *)&tcramcache.hdr->tags[tag][*seek];
            *seek = crc_32(tfe->tag_data, strlen(tfe->tag_data), 0xffffffff);
            myidx.tag_seek[tag] = *seek;
        }
        else
#endif /* HAVE_TC_RAMCACHE */
        {
            struct tagfile_entry tfe;

            /* Open the index file, which contains the tag names. */
            if ((fd = open_tag_fd(TC_CTX_ARG(ui_build) &tch, tag, true)) < 0)
                goto cleanup;

            /* Skip the header block */
            lseek(fd, myidx.tag_seek[tag], SEEK_SET);

            switch (read_tagfile_entry_and_tag(TC_CTX_ARG(ui_build) fd, &tfe,
                                               build_idx_buf, build_idx_bufsz))
            {
                case e_SUCCESS_LEN_ZERO:
                    logf("deleted_entry(): SUCCESS");
                    /* FALL THROUGH */
                case e_SUCCESS:
                     break;
                case e_ENTRY_SIZEMISMATCH:
                    logf("delete_entry(): read error #3");
                    goto cleanup;
                case e_TAG_TOOLONG:
                    logf("too long tag #4");
                    goto cleanup;
                case e_TAG_SIZEMISMATCH:
                    logf("delete_entry(): read error #3");
                    goto cleanup;
            }

            myidx.tag_seek[tag] = crc_32(build_idx_buf,
                                         strlen(build_idx_buf), 0xffffffff);
        }

        if (in_use[tag])
        {
            logf("in use: %d/%d", tag, in_use[tag]);
            if (fd >= 0)
            {
                close(fd);
                fd = -1;
            }
            continue;
        }

#ifdef HAVE_TC_RAMCACHE
        /* Delete from ram. */
        if (tc_stat.ramcache && tag != tag_filename)
        {
            struct tagfile_entry *tagentry =
                    (struct tagfile_entry *)&tcramcache.hdr->tags[tag][oldseek];
            str_setlen(tagentry->tag_data, 0);
        }
#endif /* HAVE_TC_RAMCACHE */

        /* Open the index file, which contains the tag names. */
        if (fd < 0)
        {
            if ((fd = open_tag_fd(TC_CTX_ARG(ui_build) &tch, tag, true)) < 0)
                goto cleanup;
        }

        /* Skip the header block */
        lseek(fd, oldseek + sizeof(struct tagfile_entry), SEEK_SET);

        /* Debug, print 10 first characters of the tag
        read(fd, buf, 10);
        buf[10]='\0';
        logf("TAG:%s", buf);
        lseek(fd, -10, SEEK_CUR);
        */

        /* Write first data byte in tag as \0 */
        write(fd, "", 1);

        /* Now tag data has been removed */
        close(fd);
        fd = -1;
    }

    /* Write index entry back into master index. */
    lseek(masterfd, sizeof(struct master_header) +
          (idx_id * sizeof(struct index_entry)), SEEK_SET);
    if (write_index_entries(TC_CTX_ARG(ui_build) masterfd, &myidx, 1) != sizeof(struct index_entry))
    {
        logf("delete_entry(): write_error #2");
        goto cleanup;
    }

    close(masterfd);

    return true;

    cleanup:
    if (fd >= 0)
        close(fd);
    if (masterfd >= 0)
        close(masterfd);

    return false;
}

/**
 * Returns true if there is an event waiting in the queue
 * that requires the current operation to be aborted.
 */
static bool check_event_queue(void)
{
#ifndef __PCTOOL__
    struct queue_event ev;

    if (tagcache_usb_quiescing)
        return true;
#if defined(UI_COMMIT) && defined(HAVE_IPOD_UI)
    if (ui_sleep_cut)
        return true;    /* P18: the entry window closed: a walk stops at
                         * its next folder (the sleep waits for it) */
#endif
    if(!queue_peek(&tagcache_queue, &ev))
        return false;

    switch (ev.id)
    {
        case Q_STOP_SCAN:
        case SYS_POWEROFF:
        case SYS_REBOOT:
        case SYS_USB_CONNECTED:
            return true;
    }
#endif /* __PCTOOL__ */

    return false;
}

#ifdef UI_COMMIT
/* USB only (not a power-off, whose grace is ui_commit_cancel's and
 * ui_commit_finishing's): the GUI armed the handover, or the connect is
 * the queue's head event */
static bool ui_commit_usb_queued(void)
{
    struct queue_event ev;

    if (tagcache_usb_quiescing)
        return true;
    return queue_peek(&tagcache_queue, &ev) && ev.id == SYS_USB_CONNECTED;
}
#endif

#ifdef HAVE_TC_RAMCACHE

static void fix_ramcache(TC_CTX_PARAM void* old_addr, void* new_addr)
{
    ptrdiff_t offpos = new_addr - old_addr;
    for (int i = 0; i < TAG_COUNT; i++)
#ifdef HAVE_IPOD_UI
        /* tags not in RAM (tag_filename) stay NULL: no offset on NULL
         * (UB; UBSan in the simulator, H1) */
        if (TC_RAM(ctx).hdr->tags[i])
#endif
        TC_RAM(ctx).hdr->tags[i] += offpos;
}

static int move_cb(int handle, void* current, void* new)
{
#ifdef UI_COMMIT
    /* Compaction can run on any allocating thread. Resolve the allocation
     * by handle, never through the mutable builder pointer. */
    struct tagcache_context *ctx = NULL;
    if (ui_context_a.ram.handle == handle)
        ctx = &ui_context_a;
    else if (ui_context_b.ram.handle == handle)
        ctx = &ui_context_b;
    if (!ctx)
        return BUFLIB_CB_CANNOT_MOVE;
    fix_ramcache(TC_CTX_ARG(ctx) current, new);
    ctx->ram.hdr = new;
#else
    (void)handle;
    fix_ramcache(current, new);
    tcramcache.hdr = new;
#endif
    return BUFLIB_CB_OK;
}

static struct buflib_callbacks ops = {
    .move_callback = move_cb,
    .shrink_callback = NULL,
};

static bool allocate_tagcache(void)
{
    tc_stat.ramcache_allocated = 0;
    tcramcache.handle = 0;
    tcramcache.hdr = NULL;

    /* Load the header. */
    struct master_header tcmh;
    int fd = open_master_fd(TC_CTX_ARG(ui_build) &tcmh, false);
    if (fd < 0)
        return false;

    close(fd);

    /**
     * Now calculate the required cache size plus
     * some extra space for alignment fixes.
     */
    size_t alloc_size = tcmh.tch.datasize + 256 + TAGCACHE_RESERVE +
        sizeof(struct ramcache_header) + TAG_COUNT*sizeof(void *);
#ifdef HAVE_DIRCACHE
    alloc_size += tcmh.tch.entry_count*sizeof(struct dircache_fileref);
#endif

#ifdef UI_COMMIT
    /* Playback can start during the header I/O above. Never ask buflib to
     * shrink its audio buffer based on a check made before yielding. */
    if (!ui_audio_room(alloc_size))
        return false;
#endif
    int handle = core_alloc_ex(alloc_size, &ops);
    if (handle <= 0)
        return false;

    tcramcache.handle = handle;
    tcramcache.hdr = core_get_data(handle);
    tc_stat.ramcache_allocated = alloc_size;

    memset(tcramcache.hdr, 0, sizeof(struct ramcache_header));
    memcpy(&current_tcmh, &tcmh, sizeof current_tcmh);
    logf("tagcache: %d bytes allocated.", tc_stat.ramcache_allocated);

    return true;
}

#ifdef HAVE_EEPROM_SETTINGS
static bool tagcache_dumpload(void)
{
    struct statefile_header shdr;
    int fd, rc, handle;

    tcramcache.handle = 0;
    tcramcache.hdr = NULL;

    fd = open_db_fd(TC_CTX_ARG(ui_build) TAGCACHE_STATEFILE, O_RDONLY);
    if (fd < 0)
    {
        logf("no tagcache statedump");
        return false;
    }

    /* Check the statefile memory placement */
    rc = read(fd, &shdr, sizeof(struct statefile_header));
    if (rc != sizeof(struct statefile_header)
        || shdr.magic != TAGCACHE_STATEFILE_MAGIC
        || shdr.mh.tch.magic != TAGCACHE_MAGIC)
    {
        logf("incorrect statefile");
        close(fd);
        return false;
    }

    /* Lets allocate real memory and load it */
    handle = core_alloc_ex(shdr.tc_stat.ramcache_allocated, &ops);
    if (handle <= 0)
    {
        logf("alloc failure");
        return false;
    }

    tcramcache.handle = handle;
    tcrc_buffer_lock(TC_CTX_PASS(ui_build));
    tcramcache.hdr = core_get_data(handle);
    rc = read(fd, tcramcache.hdr, shdr.tc_stat.ramcache_allocated);
    tcrc_buffer_unlock(TC_CTX_PASS(ui_build));

    close(fd);

    if (rc != shdr.tc_stat.ramcache_allocated)
    {
        logf("read failure!");
        core_free(handle);
        return false;
    }

    tc_stat = shdr.tc_stat;

    /* Now fix the pointers */
    fix_ramcache(TC_CTX_ARG(ui_build) shdr.hdr, tcramcache.hdr);

    /* Load the tagcache master header (should match the actual DB file header). */
    memcpy(&current_tcmh, &shdr.mh, sizeof current_tcmh);

    return true;
}

static bool tagcache_dumpsave(void)
{
    struct statefile_header shdr;
    int fd;

    if (!tc_stat.ramcache)
        return false;

    fd = open_db_fd(TC_CTX_ARG(ui_build) TAGCACHE_STATEFILE, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
    {
        logf("failed to create a statedump");
        return false;
    }

    /* Create the header */
    shdr.magic = TAGCACHE_STATEFILE_MAGIC;
    shdr.hdr = tcramcache.hdr;
    memcpy(&shdr.mh, &current_tcmh, sizeof current_tcmh);
    memcpy(&shdr.tc_stat, &tc_stat, sizeof tc_stat);
    write(fd, &shdr, sizeof shdr);

    /* And dump the data too */
    tcrc_buffer_lock(TC_CTX_PASS(ui_build));
    write(fd, tcramcache.hdr, tc_stat.ramcache_allocated);
    tcrc_buffer_unlock(TC_CTX_PASS(ui_build));
    close(fd);

    return true;
}
#endif /* HAVE_EEPROM_SETTINGS */

#ifdef UI_COMMIT
/* Persisted content proves the files bad; I/O, memory and USB do not. */
static bool ui_load_content;
static unsigned ui_load_failures;
#define LOAD_CONTENT_FAIL() (ui_load_content = true)

static bool ui_rebuild_owed;    /* sticky: a fresh rebuild not begun yet */
static bool ui_rebuild_tried;   /* one per boot (M2) */
static int ui_rebuild_exported; /* its export: 1 unreadable, 2 written,
                                   3 nothing to export */

/* The owed fresh rebuild, retried from the worker's loop under the begin
 * backoff (never a lost request): the active library is declared unusable
 * only once its replacement has begun. */
static void ui_rebuild_run(void)
{
    if (!ui_rebuild_owed)
        return;
    if (ui_build == ui_active)
    {
        /* MED-C: the fresh build imports the active library's changelog;
         * export its master on disk there first when it can be read (a
         * failed RAM load often leaves it readable), once per request */
        if (!ui_rebuild_exported && tagcache_context_published(ui_active))
        {
            if (!ui_retry_due(UI_RETRY_BEGIN))
                return;
            int exported = ui_export_changelog();
            if (exported < 0)
            {
                ui_retry_later(UI_RETRY_BEGIN, "export", -1);
                return;
            }
            ui_rebuild_exported = 1 + exported; /* 2: written, 3: none */
        }
        bool fresh = ui_generation_fresh;
        ui_generation_fresh = true;
        if (!ui_candidate_begin())
        {
            ui_generation_fresh = fresh;
            return;
        }
        ui_candidate_exported = ui_rebuild_exported >= 2
                                ? ui_rebuild_exported : 0;
        ui_active->stat.ready = false;
    }
    ui_rebuild_owed = false;
    ui_rebuild_exported = 0;
    queue_post(&tagcache_queue, Q_REBUILD, 2);  /* internal: keep backoff */
}

/* A failed RAM load retries with backoff. Only content failures count
 * towards a rebuild: after three, rebuild into an unreferenced directory
 * (prepare_empty); the bad files themselves are kept. At most once per
 * boot: a library that fails the same way again is not rescanned again
 * and again (a whole disk each time; R-M3, M2). */
static void ui_load_failed(bool content)
{
    ui_retry_later(UI_RETRY_LOAD, "ram load", content);
    if (!content || ++ui_load_failures < 3)
        return;
    ui_load_failures = 0;
    if (ui_rebuild_tried)
    {
        ui_log("db", "ram bad: rebuilt");
        return;
    }
    ui_log("db", "ram bad: rebuild");
    ui_rebuild_tried = true;
    ui_reload_pending = false;
    ui_rebuild_owed = true;
    ui_rebuild_run();
}
#else
#define LOAD_CONTENT_FAIL() ((void)0)
#endif

static bool load_tagcache(void)
{
    /* DEBUG: After tagcache commit and dircache rebuild, hdr-sturcture
     * may become corrupt. */

    bool ok = false;
    ssize_t bytesleft = tc_stat.ramcache_allocated - sizeof(struct ramcache_header);
    int fd;

#ifdef HAVE_DIRCACHE
    /* Wait for any in-progress dircache build to complete */
    dircache_wait();
#endif /* HAVE_DIRCACHE */

    logf("loading tagcache to ram...");
#ifdef UI_COMMIT
    ui_load_content = false;
#endif

    tcrc_buffer_lock(TC_CTX_PASS(ui_build)); /* lock for the rest of the scan, simpler to handle */

    fd = open_db_fd(TC_CTX_ARG(ui_build) TAGCACHE_FILE_MASTER, O_RDONLY);
    if (fd < 0)
    {
        logf("tagcache open failed");
        goto failure;
    }

    struct master_header tcmh;
#ifdef UI_COMMIT
    ssize_t header_bytes = read_master_header(TC_CTX_ARG(ui_build) fd, &tcmh);
    if (header_bytes != sizeof(struct master_header) ||
        tcmh.tch.magic != TAGCACHE_MAGIC)
    {
        logf("incorrect header");
        if (header_bytes == sizeof(struct master_header))
            LOAD_CONTENT_FAIL();
        goto failure;
    }
#else
    if (read_master_header(TC_CTX_ARG(ui_build) fd, &tcmh) != sizeof(struct master_header) ||
        tcmh.tch.magic != TAGCACHE_MAGIC)
    {
        logf("incorrect header");
        goto failure;
    }
#endif

    /* Master header copy should already match, this can be redundant to do. */
    current_tcmh = tcmh;

    /* Load the master index table. */
    for (int i = 0; i < tcmh.tch.entry_count; i++)
    {
        bytesleft -= sizeof(struct index_entry);
        if (bytesleft < 0)
        {
            logf("too big tagcache.");
            goto failure;
        }

        int rc = read_index_entries(TC_CTX_ARG(ui_build) fd, &tcramcache.hdr->indices[i], 1);
        if (rc != sizeof (struct index_entry))
        {
            logf("read error #10");
            goto failure;
        }
    }

    close(fd);
    fd = -1;

    /* Load the tags right after the index entries */
    char *p = (char *)&tcramcache.hdr->indices[tcmh.tch.entry_count];

    for (int tag = 0; tag < TAG_COUNT; tag++)
    {
        ssize_t rc;

        if (TAGCACHE_IS_NUMERIC(tag))
            continue;

        p = TC_ALIGN_PTR(p, struct tagcache_header, &rc);
        bytesleft -= rc;
        if (bytesleft < (ssize_t)sizeof(struct tagcache_header))
        {
            logf("Too big tagcache #10.5");
            goto failure;
        }

        tcramcache.hdr->tags[tag] = p;

        /* Load the header */
        struct tagcache_header *tch = (struct tagcache_header *)p;
        p += sizeof(struct tagcache_header);
        bytesleft -= sizeof (struct tagcache_header);

        fd = open_tag_fd(TC_CTX_ARG(ui_build) tch, tag, false);
#ifdef UI_COMMIT
        if (fd < 0)
        {
            if (fd == -2)
                LOAD_CONTENT_FAIL();
            goto failure;
        }
#elif defined(HAVE_IPOD_UI)
        if (fd < 0)
            goto failure;
#else
        if (rc < 0)
            goto failure;
#endif

        /* Load the entries for this tag */
        for (tcramcache.hdr->entry_count[tag] = 0;
             tcramcache.hdr->entry_count[tag] < tch->entry_count;
             tcramcache.hdr->entry_count[tag]++)
        {
            /* Abort if we got a critical event in queue */
            if (do_timed_yield() && check_event_queue())
                goto failure;

            p = TC_ALIGN_PTR(p, struct tagfile_entry, &rc);
            bytesleft -= rc;
            if (bytesleft < (ssize_t)sizeof(struct tagfile_entry))
            {
                logf("Too big tagcache #10.75");
                goto failure;
            }

            struct tagfile_entry *fe = (struct tagfile_entry *)p;
            off_t pos = lseek(fd, 0, SEEK_CUR);

            /* Load the header for the tag itself */
            if (read_tagfile_entry(TC_CTX_ARG(ui_build) fd, fe) != sizeof(struct tagfile_entry))
            {
                /* End of lookup table. */
                logf("read error #11");
                goto failure;
            }

            int idx_id = fe->idx_id; /* dircache reference clobbers *fe */
#ifndef UI_COMMIT
            struct index_entry *idx = &tcramcache.hdr->indices[idx_id];
#endif

            if (idx_id != -1 || tag == tag_filename) /* filename NOT optional */
            {
                if (idx_id < 0 || idx_id >= tcmh.tch.entry_count)
                {
                    logf("corrupt tagfile entry:tag=%d:idxid=%d", tag, idx_id);
                    LOAD_CONTENT_FAIL();
                    goto failure;
                }
#ifdef UI_COMMIT
                /* only a checked idx_id indexes the table */
                struct index_entry *idx = &tcramcache.hdr->indices[idx_id];
#endif

#ifdef HAVE_IPOD_UI
                /* delete_entry() replaces a deleted entry's seeks with
                 * hashes of its strings (for resurrection) but leaves its
                 * tag file entries, so the check would fail on every
                 * deleted file until the next commit drops them, and the
                 * shell would have no library */
                if (idx->flag & FLAG_DELETED)
                    ;
                else
#endif
                if (idx->tag_seek[tag] != pos)
                {
                    logf("corrupt data structures!:");
                    logf("  tag_seek[%d]=%" PRId32 ":pos=%ld", tag,
                         idx->tag_seek[tag], (long) pos);
                    LOAD_CONTENT_FAIL();
                    goto failure;
                }
            }

            /* We have a special handling for the filename tags; neither the
               paths nor the entry headers are stored; only the tagcache header
               and dircache references are. */
            if (tag == tag_filename)
            {
            #ifdef HAVE_DIRCACHE
#ifdef UI_COMMIT
                struct index_entry *idx = &tcramcache.hdr->indices[idx_id];
#endif
                if (idx->flag & FLAG_DIRCACHE)
                {
                    /* This flag must not be used yet. */
                    logf("internal error!");
                    goto failure;
                }

                p += sizeof (struct dircache_fileref);
                bytesleft -= sizeof (struct dircache_fileref);
            #endif /* HAVE_DIRCACHE */

                char filename[TAGCACHE_BUFSZ];
                if (fe->tag_length >= (long)sizeof(filename)-1)
                {
                    read(fd, filename, 10);
                    str_setlen(filename, 10);
                    logf("TAG:%s", filename);
                    logf("too long filename");
                    LOAD_CONTENT_FAIL();
                    goto failure;
                }

#ifdef UI_COMMIT
                if ((tcramcache.hdr->indices[idx_id].flag & FLAG_DELETED)
#else
                if ((idx->flag & FLAG_DELETED)
#endif
                    IFN_DIRCACHE( || !global_settings.tagcache_autoupdate ))
                {
                    /* seek over tag data instead of reading */
                    if (lseek(fd, fe->tag_length, SEEK_CUR) < 0)
                    {
                        logf("read error #11.5");
                        goto failure;
                    }

                    continue;
                }

                if (read(fd, filename, fe->tag_length) != fe->tag_length)
                {
                    logf("read error #12");
                    goto failure;
                }
                continue;
            }

            bytesleft -= sizeof(struct tagfile_entry) + fe->tag_length;
            if (bytesleft < 0
#ifdef UI_COMMIT
                || (fe->tag_length < 0 && LOAD_CONTENT_FAIL())
#endif
               )
            {
                logf("too big tagcache #2");
                logf("tl: %" PRId32, fe->tag_length);
                logf("bl: %ld", (long) bytesleft);
                goto failure;
            }

            p = fe->tag_data;
            rc = read(fd, p, fe->tag_length);
            p += rc;

            if (rc != fe->tag_length)
            {
                logf("read error #13");
                logf("rc=0x%04x", (unsigned int)rc); // 0x431
                logf("len=0x%04" PRIx32, fe->tag_length); // 0x4000
                logf("pos=0x%04lx", (unsigned long) lseek(fd, 0, SEEK_CUR)); // 0x433
                logf("tag=0x%02x", tag); // 0x00
                goto failure;
            }
        }

    #ifdef HAVE_DIRCACHE
        if (tag == tag_filename)
            p = (char *)&tcrc_dcfrefs[tcmh.tch.entry_count];
    #endif /* HAVE_DIRCACHE */

        close(fd);
#ifdef UI_COMMIT
        fd = -1;
#endif
    }

    tc_stat.ramcache_used = tc_stat.ramcache_allocated - bytesleft;
    logf("tagcache loaded into ram!");
    logf("utilization: %d%%", 100*tc_stat.ramcache_used / tc_stat.ramcache_allocated);

    ok = true;

failure:
    if (fd >= 0)
        close(fd);

    tcrc_buffer_unlock(TC_CTX_PASS(ui_build));
    return ok;
}
#endif /* HAVE_TC_RAMCACHE */

static bool check_file_refs(bool auto_update)
{
#ifdef UI_COMMIT
    /* Reverse scans edit flags and precomputes. Defer active-snapshot
     * deletion checks until the normal scan has prepared a candidate
     * (the read-only probe only looks for a missing file). */
    if (ui_build == ui_active && !ui_probe)
        return false;
#endif
    int fd;
    bool ret = true;
    char buf[TAGCACHE_BUFSZ];
    const int bufsz = sizeof(buf);
    struct tagfile_entry tfe;
    struct tagcache_header hdr;

    logf("reverse scan...");

#ifdef HAVE_DIRCACHE
    if (tcramcache.handle > 0)
        tcrc_buffer_lock(TC_CTX_PASS(ui_build));
    else
        return false;
    /* Wait for any in-progress dircache build to complete */
    dircache_wait();
#else
    if (!auto_update)
        return false;
#endif

    fd = open_tag_fd(TC_CTX_ARG(ui_build) &hdr, tag_filename, false); /* open read only*/

    if (fd < 0)
    {
        logf(TAGCACHE_FILE_INDEX " open fail", tag_filename);
        return false;
    }

    processed_dir_count = 0;
#ifdef UI_TIMING
    long ui_start = current_tick;
#endif

    while (!STOP_REQUESTED())
    {
#ifdef UI_COMMIT
        off_t entry_pos = lseek(fd, 0, SEEK_CUR);
#endif
        int res = read_tagfile_entry_and_tag(TC_CTX_ARG(ui_build) fd, &tfe, buf, bufsz);
        processed_dir_count++;

        switch (res)
        {
            case e_ENTRY_SIZEMISMATCH:
                logf("size mismatch entry EOF?"); /* likely EOF */
#ifdef UI_COMMIT
                /* #89 H2: the end of the file is the pass completing
                 * (upstream reports every pass as failed here, so "done"
                 * never vouched); a read error short of it is not, and
                 * neither is a file cut part-way through its last entry
                 * header: done only if this entry's read began exactly at
                 * the end, i.e. read nothing */
                ret = entry_pos >= 0 && entry_pos == ffilesize(fd);
#else
                ret = false;
#endif
                goto wend_finished;
            case e_TAG_TOOLONG:
                logf("too long tag");
                ret = false;
                goto wend_finished;
            case e_TAG_SIZEMISMATCH:
                logf("size mismatch tag - read error #14");
                ret = false;
                goto wend_finished;
            case e_SUCCESS:
                break;
            case e_SUCCESS_LEN_ZERO:
                continue;
        }

        int idx_id = tfe.idx_id; /* dircache reference clobbers *tfe */
#ifdef HAVE_DIRCACHE
        struct index_entry *idx = &tcramcache.hdr->indices[idx_id];
        unsigned int searchflag;
        if (!auto_update)
        {
            if(idx->flag & FLAG_DIRCACHE) /* already found */
            {
                continue;
            }
            searchflag = DCS_CACHED_PATH; /* attempt to load cache references */
        }
        else /* If auto updating, check storage too */
        {
            searchflag = DCS_STORAGE_PATH;
        }

        int rc_cache = dircache_search(searchflag | DCS_UPDATE_FILEREF,
                                   &tcrc_dcfrefs[idx_id], buf);

        if (rc_cache > 0)           /* in cache and we have fileref */
        {
            idx->flag |= FLAG_DIRCACHE;
        }
        else if (rc_cache == 0)     /* not in cache but okay */
        {;}
#ifdef HAVE_IPOD_UI
        /* dircache_search() fails with a negative code and sets errno
         * (dircache.c): it never returns ENOENT itself, so upstream's test
         * never deletes anything */
        else if (auto_update && rc_cache < 0 && errno == ENOENT)
#else
        else if (auto_update && rc_cache == ENOENT)
#endif
#else
#ifdef UI_COMMIT
        errno = 0;
        if (!file_exists(buf) && errno == ENOENT)
#else
        if (!file_exists(buf))
#endif
#endif /* HAVE_DIRCACHE */
        {
            logf("Entry no longer valid.");
            logf("-> %s / %" PRId32, buf, tfe.tag_length);
#ifdef UI_COMMIT
            if (ui_probe)
            {
                ui_probe_found = true;
                break;
            }
#endif
            delete_entry(idx_id);
        }

        do_timed_yield();
    }

wend_finished:

#ifdef HAVE_DIRCACHE
    if (tcramcache.handle > 0)
        tcrc_buffer_unlock(TC_CTX_PASS(ui_build));
#endif
#if defined(HAVE_IPOD_UI) && defined(HAVE_TC_RAMCACHE)
    ui_flush_deleted();
#endif
#ifdef UI_TIMING
    DEBUGF("tagcache: reverse scan of %d entries in %ld ms\n",
           processed_dir_count, (current_tick - ui_start) * 1000 / HZ);
#endif
    close(fd);
    logf("done");
#ifdef UI_COMMIT
    if (ui_probe)
        return ret;
    ui_libfile_dropped = false;
    ui_precompute_run();        /* if the pass deleted anything */
#endif

    return ret;
}

#ifdef UI_COMMIT
static void ui_load_missing(void);
/* The live entries of a context with RAM cache; -1 without */
static long ui_live_count(struct tagcache_context *ctx)
{
    long n = 0;

    if (!ctx->stat.ramcache)
        return -1;
    for (long i = 0; i < ctx->master.tch.entry_count; i++)
        n += !(ctx->ram.hdr->indices[i].flag & FLAG_DELETED);
    return n;
}
#endif

static bool check_deleted_files(void)
{
#ifdef UI_COMMIT
    bool clean = ui_probe_clean;

    ui_probe_clean = false;
    if (ui_build == ui_active && !ui_probe_owed && !UI_SCAN_LEGACY())
    {
        /* Lazy clone (1019): the update's probe hit every live entry, or
         * a read-only pass finds every file: nothing to delete, no
         * candidate. Else the candidate's pass below deletes. */
        if (!clean)
        {
            ui_probe = true;
            ui_probe_found = false;
            bool done = check_file_refs(true) && !STOP_REQUESTED();
            ui_probe = false;
            clean = !ui_probe_found;
            ui_probe_found = false;
            if (!done && clean)
                return false;   /* cut short (USB, power-off, a read
                                 * error): the pass stays owed */
        }
        if (clean)
        {
            ui_deleted_owed = false;
            ui_walk_vouch();    /* H2: nothing missing */
            return true;
        }
        ui_probe_owed = true;
    }
    if (ui_build == ui_active && !ui_candidate_begin())
    {
        /* waits with the deferred scan (audio holds the memory), never
         * dropped: the retry runs it after the scan */
        ui_deleted_owed = ui_scan_pending = true;
        return false;
    }
    ui_deleted_owed = false;
    ui_probe_owed = false;
    bool ok = check_file_refs(true);
    /* a candidate begun here (the lazy clone) has no RAM cache yet: the
     * publication needs it, as after a scan */
    ui_load_missing();
    if (ok && !STOP_REQUESTED())
        ui_walk_vouch();        /* H2: the missing ones are marked */
    return ok;
#else
    return check_file_refs(true);
#endif
}

/* Note that this function must not be inlined, otherwise the whole point
 * of having the code in a separate function is lost.
 */
static void NO_INLINE check_ignore(const char *dirname,
    int *ignore, int *unignore)
{
    char newpath[MAX_PATH];
    const int bufsz = sizeof(newpath);

    /* check for a database.ignore file */
    snprintf(newpath, bufsz, "%s/database.ignore", dirname);
    *ignore = file_exists(newpath);
    /* check for a database.unignore file */
    snprintf(newpath, bufsz, "%s/database.unignore", dirname);
    *unignore = file_exists(newpath);
}

/* max roots on native. on application more can be added via malloc() */
#define MAX_STATIC_ROOTS 12

static struct search_roots_ll {
    const char *path;
    struct search_roots_ll * next;
} roots_ll[MAX_STATIC_ROOTS];

/* check if the path is already included in the search roots, by the
 * means that the path itself or one of its parents folders is in the list */
static bool search_root_exists(const char *path)
{
    struct search_roots_ll *this;
    for(this = &roots_ll[0]; this; this = this->next)
    {
        size_t root_len = strlen(this->path);
        /* check if the link target is inside of an existing search root
         * don't add if target is inside, we'll scan it later */
        if (!strncmp(this->path, path, root_len))
            return true;
    }
    return false;
}

#ifdef APPLICATION
/*
 * This adds a path to the search roots, possibly during traveling through
 * the filesystem. It only adds if the path is not inside an already existing
 * search root.
 *
 * Returns true if it added the path to the search roots
 *
 * Windows 2000 and greater supports symlinks, but they don't provide
 * realpath() or readlink(), and symlinks are rarely used on them so
 * ignore this for windows for now
 **/
static bool add_search_root(const char *name)
{
    (void)name;
#ifndef WIN32
    struct search_roots_ll *this, *prev = NULL;
    char target[MAX_PATH];
    const int target_bufsz = sizeof(target);
    /* Okay, realpath() is almost completely broken on android
     *
     * It doesn't accept NULL for resolved_name to dynamically allocate
     * the resulting path; and it assumes resolved_name to be PATH_MAX
     * (actually MAXPATHLEN, but it's the same [as of 2.3]) long
     * and blindly writes to the end if it
     *
     * therefore use sufficiently large static storage here
     * Note that PATH_MAX != MAX_PATH
     **/
    static char abs_target[PATH_MAX];
    ssize_t len;

    len = readlink(name, target, target_bufsz-1);
    if (len < 0)
        return false;

    str_setlen(target, len);
    if (realpath(target, abs_target) == NULL)
        return false;

    if (search_root_exists(abs_target))
        return false;

    /* get the end of the list */
    for(this = &roots_ll[0]; this; prev = this, this = this->next);

    if (prev)
    {
        size_t len = strlen(abs_target) + 1; /* count \0 */
        this = malloc(sizeof(struct search_roots_ll) + len );
        if (!this || len > MIN(PATH_MAX, MAX_PATH))
        {
            logf("Error at adding a search root: %s", this ? "path too long":"OOM");
            free(this);
            prev->next = NULL;
            return false;
        }
        this->path = ((char*)this) + sizeof(struct search_roots_ll);
        strcpy((char*)this->path, abs_target); /* ok to cast const away here */
        this->next = NULL;
        prev->next = this;
        logf("Added %s to the search roots\n", abs_target);
        return true;
    }
#endif
    return false;
}

static int free_search_root_single(struct search_roots_ll * start)
{
    if (start < &roots_ll[0] && start >= &roots_ll[MAX_STATIC_ROOTS])
    {
        free(start->next);
        return sizeof(struct search_roots_ll);
    }
    return 0;
}

static int free_search_roots(struct search_roots_ll * start)
{
    int ret = 0;
    if (start->next)
    {
        ret += free_search_root_single(start->next);
    }
    return ret;
}
#else /* native, simulator */
#define add_search_root(a) do {} while(0)
#define free_search_roots(a) do {} while(0)
#endif

#if defined(HAVE_IPOD_UI_DEVMODE) && defined(UI_COMMIT) && !defined(PLUGIN) \
    && !defined(__PCTOOL__)
/* 16-0 scan trace (devmode only, log lines, no UI): where a scan's time
 * and storage commands go, per stage of the walk. A stage is the span
 * from the previous mark to this one. Commands are the tagcache thread's
 * own count (devstats, as power.csv's disk_cmds); the sim has none (0). */
#define SCAN_TRACE
#ifdef SIMULATOR
extern unsigned long sim_scan_us(void);
#define ST_US()   sim_scan_us()
#define ST_CMDS() 0UL
#else
#include "devstats.h"
#define ST_US()   ((unsigned long)USEC_TIMER)
/* the slot is the low byte of the thread id (thread-internal.h) */
#define ST_CMDS() ((unsigned long)devstats.t[(unsigned)thread_self() \
                                              % DEVSTATS_SLOTS].cmds)
#endif
enum { ST_TABLE, ST_OPEN, ST_IGNORE, ST_READDIR, ST_INFO, ST_ADD, ST_N };
static struct { unsigned long us, cmds; } st_b[ST_N];
static unsigned long st_t0, st_c0, st_tstart;
static long st_dirs, st_entries, st_files, st_ign_miss;
static void st_mark(int b)
{
    unsigned long t = ST_US(), c = ST_CMDS();
    st_b[b].us += t - st_t0;
    st_b[b].cmds += c - st_c0;
    st_t0 = t;
    st_c0 = c;
}
static void st_begin(void)
{
    memset(st_b, 0, sizeof(st_b));
    st_dirs = st_entries = st_files = st_ign_miss = 0;
    st_t0 = st_tstart = ST_US();
    st_c0 = ST_CMDS();
}
static void st_log(long lookups, long reads)
{
    unsigned long total = ST_US() - st_tstart;
    ui_log("db", "scantrace ms=%lu dirs=%ld ent=%ld files=%ld",
           total / 1000, st_dirs, st_entries, st_files);
    ui_log("db", "scantrace ms tbl=%lu open=%lu ign=%lu rdir=%lu",
           st_b[ST_TABLE].us / 1000, st_b[ST_OPEN].us / 1000,
           st_b[ST_IGNORE].us / 1000, st_b[ST_READDIR].us / 1000);
    ui_log("db", "scantrace ms info=%lu add=%lu ignmiss=%ld",
           st_b[ST_INFO].us / 1000, st_b[ST_ADD].us / 1000, st_ign_miss);
    ui_log("db", "scantrace cmds tbl=%lu open=%lu ign=%lu",
           st_b[ST_TABLE].cmds, st_b[ST_OPEN].cmds, st_b[ST_IGNORE].cmds);
    ui_log("db", "scantrace cmds rdir=%lu info=%lu add=%lu",
           st_b[ST_READDIR].cmds, st_b[ST_INFO].cmds, st_b[ST_ADD].cmds);
    ui_log("db", "scantrace lookups=%ld reads=%ld", lookups, reads);
}
#define ST_MARK(b) st_mark(b)
#else
#define ST_MARK(b) do {} while (0)
#endif

static bool check_dir(const char *dirname, int add_files)
{
    int success = false;

    DIR *dir = opendir(dirname);
    ST_MARK(ST_OPEN);
    if (!dir)
    {
        logf("tagcache: opendir(%s) failed", dirname);
#ifdef UI_COMMIT
        ui_walk_broken = true;  /* M2: the parent goes on; no stamp */
#endif
        return false;
    }

    /* check for a database.ignore and database.unignore */
    int ignore, unignore;
    check_ignore(dirname, &ignore, &unignore);
    ST_MARK(ST_IGNORE);
#ifdef SCAN_TRACE
    st_dirs++;
    st_ign_miss += !ignore + !unignore;
#endif

    /* don't do anything if both ignore and unignore are there */
    if (ignore != unignore)
        add_files = unignore;

    /* Recursively scan the dir. */
    while (!check_event_queue()
#ifdef UI_COMMIT
           && !ui_probe_found
#endif
          )
    {
        struct dirent *entry = readdir(dir);
        ST_MARK(ST_READDIR);
        if (entry == NULL)
        {
            success = true;
            break;
        }

        if (is_dotdir_name(entry->d_name))
            continue;
#ifdef SCAN_TRACE
        st_entries++;
#endif
#ifdef HAVE_IPOD_UI
        /* "._name": a macOS AppleDouble sidecar (Finder writes one per
         * copied file with extended attributes on FAT), never audio. With
         * an audio suffix it missed its lookup (a read of the whole
         * filename tag file) and failed its tag read at every scan. */
        if (entry->d_name[0] == '.' && entry->d_name[1] == '_')
        {
            ui_sidecars_run++;          /* #84: counted, never touched */
            continue;
        }
#endif
#ifdef UI_COMMIT
        /* a long scan still writes statistics out when storage idles
         * (this thread owns journal work, R-H2) */
        if (ui_drain_wanted())
            command_queue_sync_callback();
#endif

        struct dirinfo info = dir_get_info(dir, entry);
        ST_MARK(ST_INFO);
        size_t len = strlen(curpath);
        path_append(&curpath[len-1], PA_SEP_HARD, entry->d_name,
                    sizeof (curpath) - len);
#ifdef UI_COMMIT
        /* #89: the tree as read, for the boot stamp (bootstamp.c walks it
         * the same way); the hidden folder changes at every run */
        bool ui_hidden = (info.attribute & ATTR_DIRECTORY)
                         && !strcasecmp(curpath, ROCKBOX_DIR);
        if (ui_hidden)
            ui_walk_mute++;
        else if (!ui_walk_mute)
            boot_digest_entry(&ui_walk_dg, entry->d_name,
                              info.attribute & ATTR_DIRECTORY,
                              (uint32_t)info.size, (uint32_t)info.mtime,
                              BOOT_DIRENT_CLUSTER(entry));
#endif

        processed_dir_count++;
#if defined(SIMULATOR) && defined(HAVE_IPOD_UI)
        {
            extern int sim_scan_slow_ticks; /* tests: "scan_slow" */
            if (sim_scan_slow_ticks > 0 && (info.attribute & ATTR_DIRECTORY))
                sleep(sim_scan_slow_ticks);
        }
#endif
        if (info.attribute & ATTR_DIRECTORY)
        {
#ifndef SIMULATOR
            /* don't follow symlinks to dirs, but try to add it as a search root
             * this makes able to avoid looping in recursive symlinks */
            if (info.attribute & ATTR_LINK)
                add_search_root(curpath);
            else
#endif /* SIMULATOR */
                check_dir(curpath, add_files);
#ifdef UI_COMMIT
            if (ui_hidden)
                ui_walk_mute--;
#endif
        }
        else if (add_files)
        {
            tc_stat.curentry = curpath;

            /* Add a new entry to the temporary db file. */
#ifdef SCAN_TRACE
            st_files++;
#endif
            add_tagcache(curpath, info.mtime);
            ST_MARK(ST_ADD);

            /* Wait until current path for debug screen is read and unset. */
            while (tc_stat.syncscreen && tc_stat.curentry != NULL)
                yield();

            tc_stat.curentry = NULL;
        }

        str_setlen(curpath, len);
    }

    closedir(dir);
#ifdef UI_COMMIT
    if (success && !ui_walk_mute)
        boot_digest_leave(&ui_walk_dg);
#endif

    return success;
}

void tagcache_screensync_event(void)
{
    tc_stat.curentry = NULL;
}

void tagcache_screensync_enable(bool state)
{
    tc_stat.syncscreen = state;
}

#ifndef __PCTOOL__
/* this is called by the database tool to not pull in global_settings */
static
#endif
void do_tagcache_build(const char *path[])
{
    struct tagcache_header header;
    bool ret;
#if defined(HAVE_IPOD_UI) && !defined(__PCTOOL__)
    /* Updates run unboosted in the background; the first build and an
     * explicit Music maintenance rebuild get the full scan budget. */
    const bool boost = !tc_stat.ready || ui_maintenance_rebuild;
#else
    const bool boost = true;
#endif

    str_setlen(curpath, 0);
    data_size = 0;
    total_entry_count = 0;
    processed_dir_count = 0;
#ifdef SCAN_TRACE
    st_begin();
#endif
#ifdef UI_TIMING
    ui_scan_misses = ui_scan_lookups = ui_scan_reads = 0;
    long ui_opens0 = ui_master_opens;
#endif

#ifdef HAVE_DIRCACHE
    dircache_wait();
#endif

    logf("updating tagcache");

#ifdef UI_COMMIT
    if (!ui_probe)  /* the probe writes nothing */
#endif
    {
    cachefd = open_db_fd(TC_CTX_ARG(ui_build) TAGCACHE_FILE_TEMP, O_RDONLY);
    if (cachefd >= 0)
    {
        logf("skipping, cache already waiting for commit");
        close(cachefd);
        return ;
    }

    cachefd = open_db_fd(TC_CTX_ARG(ui_build) TAGCACHE_FILE_TEMP, O_RDWR | O_CREAT | O_TRUNC);
    if (cachefd < 0)
    {
        logf("master file open failed: %s", TAGCACHE_FILE_TEMP);
        return ;
    }
    }

    filenametag_fd = open_tag_fd(TC_CTX_ARG(ui_build) &header, tag_filename, false);
#ifdef UI_COMMIT
    /* scan speed (1019): lookups by a table of the names, built in one
     * pass (a miss walked the whole file), and one master fd for the
     * index entries of a context without RAM cache (an open per track).
     * No table (memory): the sequential lookup. */
    if (filenametag_fd >= 0 && !UI_SCAN_LEGACY())
        ui_import_table(&ui_scan, &header, curpath, sizeof(curpath), true);
    if (!tc_stat.ramcache && !UI_SCAN_LEGACY())
    {
        struct master_header mh;
        ui_scan.masterfd = open_master_fd(ui_build, &mh, false);
    }
    ST_MARK(ST_TABLE);
#endif

    if (boost)
    {
        cpu_boost(true);
    }

    logf("Scanning files...");
    /* Scan for new files. */
    memset(&header, 0, sizeof(struct tagcache_header));
#ifdef UI_COMMIT
    if (!ui_probe)
#endif
    write(cachefd, &header, sizeof(struct tagcache_header));

    ret = true;

    roots_ll[0].path = path[0];
    roots_ll[0].next = NULL;

#if defined(HAVE_MULTIVOLUME) && !defined(SIMULATOR) && !defined(__PCTOOL__) && !defined(APPLICATION)
    extern bool ns_volume_is_visible(int volume); /*rb_namespace.c*/
    /* i is for the path vector, j for the roots_ll array */
    int i = 1, j = 1;
    bool added = false;
    char volnamebuf[NUM_VOLUMES][VOL_MAX_LEN + 1];
    /* we can just parse the root directory ('/') and get to any mounted
    * volume but we can also enumerate a volume in the root directory
    * when this occurs it leads to multiple entries since the files can
    * be reached through multiple paths ex, /Foo could also be /SD1/Foo
    * we used to hide the volume that was mapped but then when you switch
    * from the sd to the internal the paths don't map to the right volume
    * instead we will attempt to rewrite the root with any non-hidden volumes
    * failing that just leave the paths alone */
    if (!strcmp(PATH_ROOTSTR, path[0]))
    {
        i = 0;
        j = 0;
    }
    /* path can be skipped , but root_ll entries can't */
    for(; path[i] && j < MAX_STATIC_ROOTS; i++)
    {
        /* check if the link target is inside of an existing search root
         * don't add if target is inside, we'll scan it later */
        if (!added && !strcmp(PATH_ROOTSTR, path[i]))
        {
            for (int v = 0; v < NUM_VOLUMES; v++)
            {
                if (ns_volume_is_visible(v))
                {
                    make_volume_root(v, volnamebuf[v]);
                    roots_ll[j].path = volnamebuf[v];
                    if (j > 0)
                        roots_ll[j-1].next = &roots_ll[j];
                    j++;
                    added = true;
                }
            }
            if(!added)
                j = 1;
            added = true;
            continue;
        }
#else
    /* i is for the path vector, j for the roots_ll array
     * path can be skipped , but root_ll entries can't */
    for(int i = 1, j = 1; path[i] && j < MAX_STATIC_ROOTS; i++)
    {
#endif /*def HAVE_MULTIVOLUME*/
        if (search_root_exists(path[i])) /* skip this path */
            continue;

        roots_ll[j].path = path[i];
        roots_ll[j-1].next = &roots_ll[j];
        j++;
    }

#ifdef UI_COMMIT
    /* #89: this walk's digest; until it completes no stamp vouches */
    boot_digest_begin(&ui_walk_dg);
    ui_walk_mute = 0;
    ui_walk_ok = ui_walk_read = ui_walk_broken = false;
    ui_walk_metafail = 0;
    ui_walk_usb_seq = ui_usb_seq;
    boot_stamp_walk_started();
    /* npfix: a stamp written since the last walk settled never outlives
     * the start of library work (a cut walk or commit must walk again);
     * this walk's own vouch writes the next one */
    ui_stamp_due = false;
    mutex_lock(&ui_stamp_mtx);
    if (ui_stamp_written && !tagcache_usb_quiescing)
    {
        ui_stamp_written = false;
        boot_stamp_drop("walk");
    }
    mutex_unlock(&ui_stamp_mtx);
#endif
    struct search_roots_ll * this;
    /* check_dir might add new roots */
    for(this = &roots_ll[0]; this; this = this->next)
    {
        logf("Search root %s", this->path);
        strmemccpy(curpath, this->path, sizeof(curpath));

        if (ret)
        {
            if (dir_exists(this->path))
                ret = check_dir(this->path, true);
            else
                logf("Dir not found %s", this->path);
        }
    }
    free_search_roots(&roots_ll[0]);
#ifdef UI_COMMIT
    /* read to the end, every folder opened: the library is about to
     * match this tree (a probe found no change, or the scan's commit
     * follows; the stamp waits for it, ui_boot_stamp_clean()), once no
     * file can be missing either (ui_walk_vouch()) */
    if (ret && !ui_walk_broken && !ui_probe_found && !ui_commit_cancel
        && !tagcache_usb_quiescing)
    {
        ui_walk_digest = ui_walk_dg.crc;
        ui_walk_read = true;
    }
    if (ui_walk_broken)
        UI_BS_LOG("walk %s", "opendir failed: no stamp");
    /* L1: a file the walk could not parse stays out of the library until
     * the tree changes or the 10th boot walks again */
    if (ui_walk_metafail)
        UI_BS_LOG("walk metafail=%ld (missing until a change or the "
                  "10th boot)", ui_walk_metafail);
#endif
#ifdef SCAN_TRACE
#ifdef UI_TIMING
    st_log(ui_scan_lookups, ui_scan_reads);
#else
    st_log(-1, -1);             /* counters are sim-only */
#endif
#endif
#if defined(HAVE_IPOD_UI) && defined(HAVE_TC_RAMCACHE) && !defined(__PCTOOL__)
    ui_flush_deleted();         /* changed files were deleted in place */
#endif

#ifdef UI_COMMIT
    if (!ui_probe)
#endif
    {
    /* Write the header. */
    header.magic = TAGCACHE_MAGIC;
    header.datasize = data_size;
    header.entry_count = total_entry_count;
    lseek(cachefd, 0, SEEK_SET);
    write(cachefd, &header, sizeof(struct tagcache_header));
    close(cachefd);
    }
#ifdef UI_TIMING
    DEBUGF("tagcache: %s %s, %d new, %ld missed, %ld lookups, %ld reads, "
           "%ld master opens%s\n", ui_probe ? "probe" : "scan",
           ret ? "done" : "stopped", total_entry_count, ui_scan_misses,
           ui_scan_lookups, ui_scan_reads, ui_master_opens - ui_opens0,
           ui_scan.handle > 0 ? "" : " (no table)");
#endif

    if (filenametag_fd >= 0)
    {
        close(filenametag_fd);
        filenametag_fd = -1;
    }
#ifdef UI_COMMIT
    if (ui_scan.masterfd >= 0)
        close(ui_scan.masterfd);
    ui_scan.masterfd = -1;
    if (ui_scan.handle > 0)
    {
        ui_scan.handle = core_free(ui_scan.handle);
        audio_buffer_regrow();
    }
    if (ui_probe)
    {
        /* found a change, or completed with none; else cut */
        if (!ui_probe_found && !ret)
            ui_probe_hits = -1;
        if (boost)
        {
            cpu_boost(false);
        }
        return;
    }
#endif

    if (!ret)
    {
        logf("Aborted.");
        /* This scan created the temp database itself. A partial result must
         * not look like a completed database waiting for commit, or a USB
         * interruption would prevent the post-disconnect retry. */
        remove_db_file(TC_CTX_ARG(ui_build) TAGCACHE_FILE_TEMP);
        if (boost)
        {
            cpu_boost(false);
        }
        return ;
    }

    /* Commit changes to the database. */
#ifdef __PCTOOL__
    allocate_tempbuf();
#endif
    if (commit())
    {
        logf("tagcache built!");
    }
#ifdef __PCTOOL__
    free_tempbuf();
#endif

#if defined(HAVE_TC_RAMCACHE) && !defined(UI_COMMIT)
    if (tcramcache.hdr)
    {
        /* Import runtime statistics if we just initialized the db. */
        if (current_tcmh.serial == 0)
            queue_post(&tagcache_queue, Q_IMPORT_CHANGELOG, 0);
    }
#endif

    if (boost)
    {
        cpu_boost(false);
    }
}

#ifndef __PCTOOL__
void tagcache_build(void)
{
    char *vect[MAX_STATIC_ROOTS + 1]; /* +1 to ensure NULL sentinel */
    char str[sizeof(global_settings.tagcache_scan_paths)];
    strmemccpy(str, global_settings.tagcache_scan_paths, sizeof(str));

    int res = split_string(str, ':', vect, MAX_STATIC_ROOTS);
    vect[res] = NULL;

#ifdef UI_COMMIT
    ui_probe_clean = false;
    /* L2: a walk the boot window waits for (an initial or dirty build
     * too): it hands over instead of holding the full cap */
    if (!ui_boot_state)
        ui_boot_state = 1;
    /* a walk before this boot's check (a first, dirty or legacy build):
     * the stamp has no say in it */
    ui_boot_walk_forced();
    /* Lazy clone (1019): an update of a ready library first walks it
     * read-only (nothing written, no candidate); the clone and the real
     * scan follow only when that finds a new, changed or reclassified
     * file. A rebuild, a fresh build, a precompute request and a change
     * found before (a deferred begin) go straight to the candidate. One
     * call of the walk below, for both passes (inlined). */
    bool probe;
again:
    ui_scan_pending = true;
#ifdef UI_COLLATE
    if (ui_build == ui_active && tc_stat.ready && ui_resort_owed())
        ui_probe_owed = true;   /* #47: the clone and its commit, once */
#endif
    probe = ui_build == ui_active && tc_stat.ready && !ui_generation_fresh
        && !ui_generation_unresolved && !ui_pre_request && !ui_probe_owed
        && !UI_SCAN_LEGACY();
    if (probe)
        ui_probe_hits = 0;
    /* retried each second while audio holds the memory: not logged (the
     * log is written to the disk) */
    else if (!ui_candidate_begin())
        return;
    else
    {
        ui_probe_owed = false;
        ui_candidate_stats_release();
    }
    ui_probe = probe;
#endif

#ifdef HAVE_IPOD_UI
    ui_sidecars_run = 0;
    ui_scan_active = true;
#endif
    do_tagcache_build((const char**)vect);
#ifdef UI_COMMIT
    ui_probe = false;
    ui_scan_pending = ui_commit_cancel || tagcache_usb_quiescing;
    if (probe && ui_probe_found)
    {
        ui_probe_found = false;     /* (it stops a walk) */
        ui_probe_owed = true;
        ui_scan_active = false;
        goto again;
    }
    /* unchanged (a cut one is owed again as before) */
    if (probe && ui_probe_hits >= 0 && !ui_scan_pending)
        ui_probe_clean = ui_probe_hits == ui_live_count(ui_active);
    if (ui_probe_clean)
        ui_walk_vouch();        /* every live entry seen: none missing */
#ifdef HAVE_IPOD_UI
    if (ui_sleep_cut)
    {
        /* P18: a walk (or its scan) the sleep entry stopped: the update
         * is owed to a later window, never run at the wake; a candidate
         * it began goes as a cut commit's does */
        ui_sleep_rescan = true;
        if (ui_build != ui_active)
            ui_sleep_cut_seen = true;
    }
#endif
#endif
#ifdef HAVE_IPOD_UI
    if (!ui_scan_pending)
        ui_sidecars_done = ui_sidecars_run;
    ui_scan_active = false;
#endif
}
#endif /* __PCTOOL__ */

#if defined(HAVE_IPOD_UI_DEVMODE) && !defined(PLUGIN) && !defined(__PCTOOL__)
/* M0: the scan, with the commit it ends in (logged on its own above) */
static void tagcache_build_timed(void)
{
    long t0 = current_tick;

    tagcache_build();
#ifdef UI_COMMIT
    if (ui_scan_pending && !ui_commit_cancel && !tagcache_usb_quiescing)
        return;                 /* deferred (retried each second): no log */
    /* a scan cut by USB is logged too: its time is the handover evidence */
    ui_log("db", "scan ms=%ld n=%ld%s", (current_tick - t0) * (1000 / HZ),
           (long)current_tcmh.tch.entry_count, ui_scan_pending ? " cut" : "");
#else
    ui_log("db", "scan ms=%ld n=%ld", (current_tick - t0) * (1000 / HZ),
           (long)current_tcmh.tch.entry_count);
#endif
}
#define tagcache_build() tagcache_build_timed()
#endif

#ifdef HAVE_TC_RAMCACHE
#ifdef UI_COMMIT
/* Whether the RAM cache for the database on disk, and the library view
 * pool growth that follows it (pool_sync()), fit in free memory. While
 * audio plays or is paused, a load that does not fit would shrink the
 * audio buffer (a gap, or a stop: playback.c shrink_callback); it waits
 * then (ui_reload_pending), as a commit does. */
/* What loading the RAM cache of a database with header 'mh' takes */
static size_t ui_reload_need(const struct master_header *mh)
{
    size_t need = mh->tch.datasize + 256 + TAGCACHE_RESERVE
        + sizeof(struct ramcache_header) + TAG_COUNT * sizeof(void *);
#ifdef HAVE_DIRCACHE
    need += mh->tch.entry_count * sizeof(struct dircache_fileref);
#endif
    if (mh->tch.entry_count > ui_loaded_entries)
        need += (mh->tch.entry_count - ui_loaded_entries)
                * TAGCACHE_UI_VIEW_BYTES;
    return need;
}

static bool ui_reload_fits(void)
{
    struct master_header mh;
    size_t need;
    int fd;

    if (audio_status() == 0)
        return true;
    if (ui_reload_pending)      /* sized already: no disk access */
        return ui_audio_room(ui_reload_bytes);
    fd = open_master_fd(TC_CTX_ARG(ui_build) &mh, false);
    if (fd < 0)
        return true;            /* the load fails without allocating */
    close(fd);
    need = ui_reload_need(&mh);
    ui_reload_bytes = need;
    if (ui_audio_room(need))
        return true;
    DEBUGF("tagcache: RAM cache reload (%lu B) waits for playback to "
           "stop\n", (unsigned long)need);
    return false;
}
#endif /* UI_COMMIT */

static void load_ramcache(void)
{
#ifdef UI_COMMIT
    /* Until manifest selection is trustworthy, even a readable legacy
     * root is not the selected namespace. A later retry must not pair its
     * RAM contents with a newly selected generation's filenames. */
    if (ui_generation_unresolved || (ui_generation_fresh && ui_build == ui_active))
        return;
    bool disk_ready = tc_stat.ready;
#endif
#ifdef HAVE_IPOD_UI
    /* Size the cache for the database as it is now: upstream allocates
     * once at boot, so after the first build there is no cache until a
     * reboot, and a reload after a commit that grew the database by more
     * than the 32 KB reserve fails and disables the database. */
#ifdef UI_COMMIT
    /* (called with the cache already dropped: nothing changes meanwhile) */
    ui_reload_pending = false;
    ui_reload_pending = !ui_reload_fits();
    /* never free RAM a pinned reader may still use across a yield (a
     * search, a background pass, a queued update): as with a retired
     * snapshot, wait until the pins are gone (LOW-5, the MED-4 reload) */
    if (!ui_reload_pending && tcramcache.handle > 0 && ui_build->readers)
        ui_reload_pending = ui_drain_force = true; /* queued pins go now */
    if (ui_reload_pending)
    {
        ui_busy = true;         /* "Updating Library..." until it loads */
        return;
    }
#endif
    tc_stat.ramcache = false;
#ifdef UI_COMMIT
    ui_win_drop();              /* lib window: the lists go empty */
#endif
    ui_generation++;
    if (tcramcache.handle > 0)
    {
        tcramcache.hdr = NULL;
        tcramcache.handle = core_free(tcramcache.handle);
    }
    if (!allocate_tagcache())
    {
        logf("tagcache: no RAM cache");
#ifdef UI_COMMIT
        tc_stat.ready = disk_ready;
        ui_reload_pending = true;
#endif
        ui_busy = false;
        return;
    }
#else
    if (!tcramcache.hdr)
        return ;
#endif

    cpu_boost(true);

    /* At first we should load the cache (if exists). */
    tc_stat.ramcache = load_tagcache();
#ifdef UI_COMMIT
    if (tc_stat.ramcache)
        ui_win_ready = current_tick;    /* lib window: lists readable */
#endif
#ifdef HAVE_IPOD_UI
    if (tc_stat.ramcache)
    {
        /* Give back what the load did not use (the filename tag file,
         * which is never stored, and the reserve): audit section 5. */
        size_t used = ALIGN_UP(tc_stat.ramcache_used, 16);
        int before = tc_stat.ramcache_allocated;
        if (used < (size_t)tc_stat.ramcache_allocated
            && core_shrink(tcramcache.handle, tcramcache.hdr, used))
        {
            tc_stat.ramcache_allocated = used;
        }
        DEBUGF("tagcache: RAM cache %d entries, %d bytes (allocated %d)\n",
               (int)current_tcmh.tch.entry_count, tc_stat.ramcache_allocated,
               before);
        (void)before;
#ifdef UI_COMMIT
        ui_load_failures = 0;
        ui_retry[UI_RETRY_LOAD].delay = 0;
        if (ui_ram_yielded == ui_build)
            ui_ram_yielded = NULL;
        ui_loaded_entries = current_tcmh.tch.entry_count;
        ui_loaded_commitid = current_tcmh.commitid;
        ui_libfile_dropped = false;
        ui_pre_pending = true;  /* run below, unboosted */
        if (ui_build == ui_active)
            ui_active_pre_done = false;
#endif
    }
    ui_generation++;
    ui_busy = false;
#endif

    if (!tc_stat.ramcache)
    {
        /* If loading failed, it must indicate some problem with the db
         * so disable it entirely to prevent further issues. */
#ifdef UI_COMMIT
        /* Allocation, USB cancellation and short reads do not prove that
         * persisted data is corrupt. Keep it and retry loading, rather than
         * letting the automatic first-build path erase it. */
        tc_stat.ready = disk_ready;
        ui_reload_pending = true;
        ui_load_failed(ui_load_content);
#else
        tc_stat.ready = false;
#endif
        tcramcache.hdr = NULL;
        int handle = tcramcache.handle;
        tcramcache.handle = 0;
        core_free(handle);
#ifdef UI_COMMIT
        /* no cache, no size: commit() and ui_commit_buffer() would
         * steal the freed buffer (pinning handle 0, upstream too) */
        tc_stat.ramcache_allocated = 0;
#endif
    }

    cpu_boost(false);
#ifdef UI_COMMIT
    /* here, for every caller (the boot load, after a commit, the reload
     * that waited for playback): before the long disk walk that often
     * follows, so that the lists use the file as soon as possible */
    if (tc_stat.ramcache && ui_build == ui_active)
        ui_precompute_run();
#endif
}

#ifdef HAVE_IPOD_UI
/* 3.20: a reload counts as library work for the header's glyph, the
 * boot's first load does not (nothing is being updated) */
static void ui_load_ramcache_counted(void)
{
    if (ui_upd_loaded)
        UI_UPD_RUN(load_ramcache());
    else
        load_ramcache();
    if (tc_stat.ramcache)
        ui_upd_loaded = true;
}
#define load_ramcache() ui_load_ramcache_counted()
#endif

#ifdef UI_COMMIT
/* The precompute stops at any queued event (USB, a power-off, an update,
 * the changelog import): the event is handled first, the precompute
 * redone afterwards. */
static bool ui_precompute_event(void)
{
    struct queue_event ev;

    return tagcache_usb_quiescing || ui_commit_cancel || ui_sleep_cut
           || queue_peek(&tagcache_queue, &ev);
}

/* P16 (gate fix): a candidate not yet published while audio is on --
 * its statistics prepare refused (the import table, ~600 KB at 50k, from
 * free memory only), or its publication refused (the view pool from
 * free memory, lib_generation_pool_prepare(); the precompute's transient
 * block; a failed step's backoff) -- would hold its RAM cache (6.9 MB
 * at 50k), and a prepared lookup, through the whole playback: loaded
 * while stopped (after the #47 re-sort at the first boot, after a
 * post-USB commit), with PLAY pressed before it published. It gives
 * both back, and loads again at the stop or in a paused window
 * (ui_yield_reload_due()), as a deferred commit's candidate does
 * (b5f5b413dd). Unpublished: no reader holds it. The audio buffer,
 * sized without it, grows back at the next cold start (the window's
 * regrow, if paused). Tagcache thread. (Statics: ui_ram_yielded.) */

/* one more yield: the reload waits a doubling backoff (2 s .. 5 min, a
 * play/stop loop reloads 6.9 MB at most that often), and after
 * UI_YIELD_MAX yields it stops retrying (logged once) */
static void ui_yield_note(void)
{
    if (!ui_retry[UI_RETRY_YIELD].delay)
        ui_yields = 0;          /* reset since (USB, a request): anew */
    ui_yields++;
    ui_retry_later(UI_RETRY_YIELD, "cand ram reload wait", (int)ui_yields);
    if (ui_yields == UI_YIELD_MAX)
        ui_log("db", "cand ram reload gives up n=%u (owed: a request, "
               "USB or the next boot)", ui_yields);
}

/* May a yielded candidate load its RAM cache again now? At the stop,
 * after its backoff; with audio on only in a paused window whose take
 * would keep the floor (sized at the yield: no disk access), at most
 * one refused take a pause (maint_declined()). Not after giving up. */
/* ... and P18: the same, the window aside (the sleep-entry window owes
 * the job before it opens) */
static bool ui_yield_reload_can(void)
{
    if (ui_yields >= UI_YIELD_MAX && ui_retry[UI_RETRY_YIELD].delay)
        return false;           /* given up */
    if (!ui_retry_due(UI_RETRY_YIELD))
        return false;           /* in its backoff */
    if (audio_status() == 0)
        return true;
    /* what ui_audio_room() would take: the job's whole peak */
    return !maint_declined(MAINT_COMMIT)
           && maint_take_fits(MAX(ui_yield_bytes + UI_MAINT_SLACK,
                                  ui_peak_estimate()));
}

static bool ui_yield_reload_due(void)
{
    if (ui_ram_yielded != ui_build || ui_build == ui_active)
        return false;
    return ui_yield_reload_can()
           && (audio_status() == 0 || maint_window_open());
}

static void ui_candidate_ram_yield(void)
{
    if (ui_build == ui_active || audio_status() == 0
        || maint_window_open() || tcramcache.handle <= 0
        || ui_candidate_maybe_published
        || ui_build->readers || ui_build->operations || read_lock)
        return;
    bool stats = ui_stats_prepared;

    ui_candidate_stats_release();       /* the prepared lookup too */
    ui_yield_bytes = ui_reload_need(&current_tcmh);
    lib_generation_pool_abandon(ui_build);
    tc_stat.ramcache = false;
    ui_win_drop();
    ui_generation++;
    tcramcache.hdr = NULL;
    tcramcache.handle = core_free(tcramcache.handle);
    ui_log("db", "cand ram yield bytes=%ld stats=%d",
           (long)tc_stat.ramcache_allocated, (int)stats);
    tc_stat.ramcache_allocated = 0;
    ui_ram_yielded = ui_build;
    ui_yield_note();
    /* a window's take for this candidate is free memory now: its
     * tenancy ends (no commit owed), so a later pause may regrow */
    if (!ui_commit_pending && !ui_scan_pending)
    {
        ui_maint_need = 0;
        maint_done(MAINT_COMMIT, false);
    }
    audio_buffer_regrow();
}

/* Runs a due library precompute (tagcache thread only). It runs at any
 * time in the block library.c reserved at shell start (H1, #41.2); a
 * database that has outgrown that block (or none was reserved: no
 * database at boot) needs a transient block, so it waits (stays due)
 * while audio plays or is paused, and stops when playback starts
 * (library.c checks that itself). It also stops for an event (handled
 * first, the precompute redone afterwards). The thread's loop runs this
 * again every second (SYS_TIMEOUT) and after every event, in every
 * database state (it is set by every load). */
static void ui_precompute_run(void)
{
    bool force;

    if (ui_shutdown_intent)
        return;
    /* The boot replay goes into the active snapshot first, a candidate
     * or not (M5), retried with backoff (the disk sleeps meanwhile) */
    if (ui_active->stat.ramcache && !ui_boot_stats_replayed)
    {
        /* nothing else (no candidate's statistics: MED-3) until it is
         * done; any failure, an allocation too, waits its backoff */
        bool ok = false;
        if (!ui_stats_prepared && ui_retry_due(UI_RETRY_REPLAY))
        {
            ui_import_ctx = ui_active;
            ok = ui_candidate_stats_prepare();
            ui_candidate_stats_release();
            ui_import_ctx = NULL;
            if (!ok)
                ui_retry_later(UI_RETRY_REPLAY, "stats replay", 0);
        }
        if (!ok)
        {
            ui_pre_pending = true;
            return;
        }
    }
    if (ui_candidate_maybe_published)
    {
        /* No candidate structural writes until an ambiguous manifest
         * publication has been reconciled with its retained on-disk copy. */
        ui_candidate_publish();
        return;
    }
    if (ui_import_pending)
        return;
    /* an update that changed nothing: drop the clone (no publication, no
     * new generation, nothing for the art or lists to redo) */
    if (ui_build != ui_active && !ui_candidate_changed && ui_pre_pending
        && !ui_commit_pending && !ui_scan_pending)
    {
        ui_candidate_abandon(0);
        /* the active library's lists, only if still owed (each pass
         * asks the art builder for one too) */
        ui_pre_pending = !ui_active_pre_done;
        return;
    }
    if (ui_pre_request && ui_build == ui_active
        && tagcache_context_published(ui_active))
    {
        if (!ui_candidate_begin())
            return;
        load_ramcache();
    }
    if ((!ui_pre_pending && !ui_pre_request) || !tc_stat.ramcache
        || tagcache_ui_count_ctx(ui_build) < 0 || ui_precompute_event())
        return;
    force = ui_pre_request;
    ui_pre_request = false;
    ui_pre_pending = false;
    if (ui_build != ui_active && !ui_stats_prepared)
    {
        /* a failed prepare (memory) waits its backoff, not a second */
        bool due = ui_retry_due(UI_RETRY_PUBLISH);
        if (!due || !ui_candidate_stats_prepare())
        {
            if (due)
            {
                ui_retry_later(UI_RETRY_PUBLISH, "cand stats", -1);
                ui_candidate_ram_yield();   /* publishes at the stop */
            }
            ui_pre_pending = true;
            return;
        }
    }
    int rc;
    UI_UPD_RUN(rc = lib_precompute_ctx(ui_build, force,
                                       ui_precompute_event));
    /* its transient block is freed by now: its tenancy ends (P16) */
    maint_done(MAINT_LISTS, rc == LIB_PRE_DONE);
    ui_publish_busy = false;
    if (rc == LIB_PRE_DONE && ui_build == ui_active)
    {
        ui_active_pre_done = true;
        /* #88 (L): a take for the lists alone ends here, so the window
         * can give its memory back */
        if (!ui_commit_pending && !ui_scan_pending)
            maint_done(MAINT_COMMIT, true);
    }
    if (rc != LIB_PRE_DONE
        || (ui_build != ui_active && !ui_candidate_publish()))
    {
        ui_pre_pending = true;
        if (force)
            ui_pre_request = true;
        /* P16: a candidate refused publication while audio is on (or
         * its lists, for their transient block) gives its RAM back; not
         * for an event (handled first) or a reader of the active
         * library (published within a second) */
        if (ui_build != ui_active && !ui_publish_busy
            && !(rc != LIB_PRE_DONE && ui_precompute_event()))
            ui_candidate_ram_yield();
    }
}
#endif

void tagcache_unload_ramcache(void)
{
#ifdef UI_COMMIT
    /* USB/media updates prepare a new snapshot. Existing RAM remains a
     * valid last-known library until the replacement has been published. */
    return;
#else
    tc_stat.ramcache = false;
#ifdef HAVE_IPOD_UI
    ui_generation++;
#endif
    /* Just to make sure there is no statefile present. */
    // remove_db_file(TC_CTX_ARG(ui_build) TAGCACHE_STATEFILE);
#endif
}

#if defined(HAVE_IPOD_UI) && !defined(__PCTOOL__)
int tagcache_ui_generation_ctx(struct tagcache_context *ctx)
{
    return ctx->generation;
}

struct tagcache_context *tagcache_context_active(void)
{
    return ui_active;
}

struct tagcache_context *tagcache_context_acquire(void)
{
    while (true)
    {
        int level = disable_irq_save();
        struct tagcache_context *ctx = ui_active;
        if (!ui_publishing && !ctx->reserved)
        {
            /* Nested IRQ-safe primitive; restore preserves outer mask. */
            ui_context_ref(ctx, true);
            restore_irq(level);
            return ctx;
        }
        restore_irq(level);
        /* a publication's window can span a disk spin-up (fsync, the
         * manifest rename): sleep, never spin the UI thread (F1) */
        sleep(1);
    }
}

void tagcache_context_release(struct tagcache_context *ctx)
{
    ui_context_put(ctx, true);
}

bool tagcache_context_published(struct tagcache_context *ctx)
{
    return ctx == ui_active && strcmp(ctx->stat.db_path, ui_db_root);
}

#ifdef UI_COMMIT
void tagcache_ui_window(long *scan, long *empty, long *ready)
{
    *scan = ui_win_scan;
    *empty = ui_win_empty;
    *ready = ui_win_ready;
}
#endif

bool tagcache_ui_busy(void)
{
#ifdef UI_COMMIT
    /* "Updating Library..." also while a RAM cache reload, or the commit
     * of a new database, waits for playback to stop (the thread's
     * SYS_TIMEOUT clears ui_busy meanwhile) */
    if (ui_reload_pending || (ui_commit_pending && ui_db_fresh))
        return true;
#endif
    return ui_busy;
}

#ifdef UI_COMMIT
int32_t tagcache_ui_commitid_ctx(struct tagcache_context *ctx)
{
    return ctx->loaded_commitid;
}

const char *tagcache_ui_db_path_ctx(struct tagcache_context *ctx)
{
    return ctx->stat.db_path;
}

void tagcache_ui_precompute_request(void)
{
    ui_pre_request = true;      /* the thread's next SYS_TIMEOUT runs it */
#ifdef TC_QUIET_WAIT
    tagcache_quiet_kick();      /* P19 19-1a: which is now */
#endif
}

long tagcache_ui_disk_entries(void)
{
    struct master_header mh;
    struct tagcache_header th;
    long n = -1;
    int fd = open_db_fd(TC_CTX_ARG(ui_build) TAGCACHE_FILE_MASTER, O_RDONLY);

    if (fd >= 0)
    {
        if (read(fd, &mh, sizeof(mh)) == (ssize_t)sizeof(mh)
            && mh.tch.magic == TAGCACHE_MAGIC && mh.tch.entry_count >= 0)
            n = mh.tch.entry_count;
        close(fd);
    }
    if (n < 0)
        return -1;              /* no database (yet), or a foreign one */
    /* a scan waiting for its commit adds its entries */
    fd = open_db_fd(TC_CTX_ARG(ui_build) TAGCACHE_FILE_TEMP, O_RDONLY);
    if (fd >= 0)
    {
        if (read(fd, &th, sizeof(th)) == (ssize_t)sizeof(th)
            && th.entry_count > 0)
            n += th.entry_count;
        close(fd);
    }
    return n;
}
#endif

int tagcache_ui_count_ctx(struct tagcache_context *ctx)
{
    if (!ctx->stat.ready || !ctx->stat.ramcache || ctx->stat.commit_step > 0
        || !ctx->ram.hdr)
        return -1;
    return ctx->master.tch.entry_count;
}

const int32_t *tagcache_ui_seeks_ctx(struct tagcache_context *ctx, int idx_id, int32_t *flag)
{
    const struct index_entry *idx;

    if (idx_id < 0 || idx_id >= tagcache_ui_count_ctx(ctx))
        return NULL;
    idx = &ctx->ram.hdr->indices[idx_id];
    if (idx->flag & FLAG_DELETED)
        return NULL;
    if (flag)
        *flag = idx->flag;
    return idx->tag_seek;
}

long tagcache_ui_numeric_ctx(struct tagcache_context *ctx, int idx_id, int tag)
{
    const int32_t *seeks = tagcache_ui_seeks_ctx(ctx, idx_id, NULL);

    if (!seeks || !TAGCACHE_IS_NUMERIC(tag) || tag >= TAG_COUNT)
        return -1;
    return tc_find_tag(TC_CTX_ARG(ctx) tag, idx_id, (const struct index_entry *)seeks);
}

bool tagcache_ui_walk_ctx(struct tagcache_context *ctx, int tag, int32_t *pos, int32_t *idx_id)
{
    const struct tagcache_header *h;
    const struct tagfile_entry *ep;

    if (tag < 0 || tag >= TAG_COUNT || tag == tag_filename
        || TAGCACHE_IS_NUMERIC(tag) || tagcache_ui_count_ctx(ctx) < 0)
        return false;
    h = (const struct tagcache_header *)ctx->ram.hdr->tags[tag];
    if (*pos < (int32_t)sizeof(*h))
        *pos = sizeof(*h);
    if (*pos >= (int32_t)sizeof(*h) + h->datasize)
        return false;
    ep = (const struct tagfile_entry *)&ctx->ram.hdr->tags[tag][*pos];
    *idx_id = ep->idx_id;
    *pos += sizeof(*ep) + ep->tag_length;
    return true;
}

const char *tagcache_ui_str_ctx(struct tagcache_context *ctx, int tag, int32_t seek)
{
    const struct tagfile_entry *ep;

    if (tag < 0 || tag >= TAG_COUNT || tag == tag_filename
        || TAGCACHE_IS_NUMERIC(tag) || seek < 0 || tagcache_ui_count_ctx(ctx) < 0)
        return NULL;
    ep = (const struct tagfile_entry *)&ctx->ram.hdr->tags[tag][seek];
    if (ep->tag_length <= 1 || !strcmp(ep->tag_data, UNTAGGED))
        return NULL;
    return ep->tag_data;
}
int tagcache_ui_generation(void)
{
    return tagcache_ui_generation_ctx(ui_active);
}

int32_t tagcache_ui_commitid(void)
{
    return tagcache_ui_commitid_ctx(ui_active);
}

const char *tagcache_ui_db_path(void)
{
    return tagcache_ui_db_path_ctx(ui_active);
}

int tagcache_ui_count(void)
{
    return tagcache_ui_count_ctx(ui_active);
}

const int32_t *tagcache_ui_seeks(int idx_id, int32_t *flag)
{
    return tagcache_ui_seeks_ctx(ui_active, idx_id, flag);
}

long tagcache_ui_numeric(int idx_id, int tag)
{
    return tagcache_ui_numeric_ctx(ui_active, idx_id, tag);
}

bool tagcache_ui_walk(int tag, int32_t *pos, int32_t *idx_id)
{
    return tagcache_ui_walk_ctx(ui_active, tag, pos, idx_id);
}

const char *tagcache_ui_str(int tag, int32_t seek)
{
    return tagcache_ui_str_ctx(ui_active, tag, seek);
}

#endif /* HAVE_IPOD_UI */
#endif /* HAVE_TC_RAMCACHE */

#ifndef __PCTOOL__

/*
 * db_file_exists is noinline to minimize stack usage
 */
static bool NO_INLINE db_file_exists(const char* filename)
{
    char buf[MAX_PATH];

    snprintf(buf, sizeof(buf), "%s/%s", tc_stat.db_path, filename);

    return file_exists(buf);
}

#ifdef UI_COMMIT
/* a library on disk whose RAM copy is missing (dropped by a commit, a
 * failed load) is loaded again */
static void ui_load_missing(void)
{
    /* #109 (32 MB floor): a candidate whose commit waits for the stop
     * gets its RAM cache after that commit ("delayed commit" below),
     * into the memory the commit frees. Loaded now it only holds 6.9 MB
     * (50k) of free heap through the deferral -- the commit drops it and
     * reloads anyway, and steals it only when it is bigger than the
     * commit's buffer -- so the stopped commit had to take its whole
     * buffer from the stopped audio buffer */
    if (ui_build != ui_active && ui_commit_pending && audio_status() != 0)
    {
        DEBUGF("tagcache: candidate RAM cache waits for the commit\n");
        return;
    }
    /* P16: one that gave its RAM back while audio ran
     * (ui_candidate_ram_yield()) loads it at the stop, or in a paused
     * window that takes the reload above the floor (#88), after its
     * backoff; the take is tried at most once a pause (refused: Q11) */
    if (ui_ram_yielded == ui_build && ui_build != ui_active)
    {
        if (!ui_yield_reload_due()
            || (audio_status() != 0 && !ui_audio_room(ui_yield_bytes)))
            return;
        ui_ram_yielded = NULL;
    }
    if (!tc_stat.ramcache && global_settings.tagcache_ram)
        load_ramcache();
}
#endif

#ifdef UI_COMMIT
static bool ui_boot_stop(void)
{
    return STOP_REQUESTED();
}

/* #89 (docs/phase16-brief.md 4.2): whether this boot's walk can be
 * skipped. Once per boot, on this thread, right before the first walk
 * would start (the RAM cache is loaded: its commit id is the active
 * generation). The safe-replace and rename-intent recoveries (#91) ran
 * before: main.c's init and the top of tagcache_thread(). */
static bool ui_boot_skip_walk(bool rescan)
{
    struct boot_stamp st;
    enum boot_why why;
    bool have, trusted;

    if (ui_boot_decided)
        return false;
    ui_boot_decided = true;
    have = boot_stamp_saved(&st, &why);
    if (rescan || ui_usb_seq)
        why = BOOT_WHY_USB;
    else if (!tc_stat.ready || ui_build != ui_active || ui_generation_fresh
             || ui_generation_unresolved || ui_pre_request || ui_probe_owed
             || ui_commit_pending || ui_scan_pending || ui_deleted_owed
             || ui_maintenance_rebuild || UI_SCAN_LEGACY()
#ifdef UI_COLLATE
             || ui_resort_owed()    /* #47: the one re-sort walks */
#endif
             )
        why = BOOT_WHY_DB;
    else if (have)
    {
        why = boot_stamp_precheck(&st, true,
                                  (uint32_t)ui_active->loaded_commitid);
        /* P18 Q11: a check owed by the calendar runs at this boot */
        if (why == BOOT_WHY_TRUST
            && sleep_walk_owed(ui_sleep_rtc_now(), st.last_walk_rtc))
            why = BOOT_WHY_DAYS;
    }
    if (why == BOOT_WHY_TRUST)
    {
        struct boot_digest d;
        char *vect[MAX_STATIC_ROOTS + 1];
        char str[sizeof(global_settings.tagcache_scan_paths)];
        enum boot_digest_state ds;
        const long t0 = current_tick;

        strmemccpy(str, global_settings.tagcache_scan_paths, sizeof(str));
        vect[split_string(str, ':', vect, MAX_STATIC_ROOTS)] = NULL;
        cpu_boost(true);
        ds = boot_stamp_digest(&d, (const char *const *)vect, curpath,
                               sizeof(curpath),
                               t0 + BOOT_DIGEST_CAP_MS * HZ / 1000,
                               ui_boot_stop);
        cpu_boost(false);
        str_setlen(curpath, 0);
        if (ds == BOOT_DIGEST_MATCH && d.crc != st.dir_digest)
            ds = BOOT_DIGEST_DIFF;
        if (ds == BOOT_DIGEST_MATCH || ds == BOOT_DIGEST_DIFF)
            ui_boot_entries = d.entries;
        why = boot_stamp_verdict(ds);
        UI_BS_LOG("digest ms=%ld dirs=%ld ent=%ld st=%d",
                  (current_tick - t0) * (1000 / HZ), d.dirs, d.entries,
                  (int)ds);
    }
    trusted = boot_why_trusted(why);
    boot_stamp_decided(why);
    /* H2: the walk sees new and changed files only; one that is gone
     * stays a ghost unless the deleted-file pass follows it (nearly free
     * when the probe hit every live entry, check_deleted_files()) */
    ui_boot_deleted_due = boot_why_deleted_pass(why);
    if (trusted)
    {
        /* the library still matches the tree that walk read */
        ui_walk_digest = st.dir_digest;
        ui_walk_ok = true;
        ui_walk_skipped = st.boots_skipped + 1u;
        ui_walk_rtc = st.last_walk_rtc;
        ui_walk_usb_seq = ui_usb_seq;
        ui_stamp_due = true;    /* npfix: back on disk once settled */
    }
    ui_boot_state = trusted ? 2 : 1;
    UI_BS_LOG("stamp walk=%d why=%s skipped=%u", trusted ? 0 : 1,
              boot_why_name(why), have ? (unsigned)st.boots_skipped : 0u);
    return trusted;
}

/* a boot whose library is rebuilt (first build, a dirty or legacy one):
 * it walks, the stamp has no say (expect_boot_walk sees "db") */
static void ui_boot_walk_forced(void)
{
    if (ui_boot_decided)
        return;
    ui_boot_decided = true;
    boot_stamp_decided(BOOT_WHY_DB);
    UI_BS_LOG("stamp walk=1 why=%s skipped=0", boot_why_name(BOOT_WHY_DB));
}

/* H2: after this boot's walk, the deleted-file pass it is due (after a
 * deferred scan: owed, run by the retry once the scan is done) */
static void ui_boot_deleted_pass(void)
{
    if (!ui_boot_deleted_due)
        return;
    ui_boot_deleted_due = false;
    if (ui_scan_pending)
        ui_deleted_owed = true;
    else
        check_deleted_files();
}

/* why the stamp may not be written now (NULL: it may) */
static const char *ui_stamp_block(void)
{
    if (!ui_walk_ok)
        return "nowalk";
    if (ui_usb_since_walk)
        return "usb";
    if (!tc_stat.ready || ui_build != ui_active || ui_scan_pending
        || ui_scan_active || ui_commit_pending || ui_deleted_owed
        || ui_probe_owed || ui_generation_unresolved
        || ui_generation_fresh || ui_rebuild_again
        || ui_maintenance_rebuild || ui_candidate_maybe_published)
        return "busy";
    return NULL;
}

/* tagcache_shutdown(), after the statistics flush, and a sleep entry
 * (UI thread): the stamp, only for a settled library that a completed
 * walk (or a trusted boot) vouches for, with no USB session since */
static void ui_boot_stamp_clean(void)
{
    const char *no;

    mutex_lock(&ui_stamp_mtx);      /* review L4: one writer at a time */
    no = ui_stamp_block();
    if (no)
    {
        UI_BS_LOG("stamp write=0 why=%s", no);
        mutex_unlock(&ui_stamp_mtx);
        return;
    }
    /* M1: a USB session can still supersede the power-off: its exit
     * removes this stamp again (ui_boot_stamp_usb_exit()) */
    if (boot_stamp_write(ui_walk_digest, (uint32_t)ui_active->loaded_commitid,
                         ui_walk_skipped, ui_walk_rtc, false))
    {
        ui_stamp_written = true;
        ui_stamp_due = false;
    }
    mutex_unlock(&ui_stamp_mtx);
}

/* npfix (owner log run-1137: "why=nostamp", a 19.8 s walk): the stamp
 * was written only at a clean shutdown or a sleep entry, so a session
 * that ended any other way after its walk walked again at the next boot,
 * and a short one carried that debt on. Now the walk's vouch (or a
 * trusted boot's decision) is written as soon as the library settles,
 * on this thread, only with the disk already up (never a spin-up for
 * it), once per vouch (ui_stamp_due). The rule that remains: a crash
 * keeps the last stamp written, so the next boot skips its walk exactly
 * as after a clean shutdown when no walk and no USB session began since
 * that write. The stamp is removed at the start of any walk (a cut walk,
 * and the scan and commit it starts, walk again), at a USB connect
 * before the ack (H1) and at its exit (M1), and at a soft sleep's wake
 * (ui_stamp_spoil); it is never written while the boot's deleted-file
 * pass, a scan, a commit, a rebuild or a sleep-owed check is pending
 * (ui_stamp_block()), nor after a USB session until a walk completes
 * after it. The next boot still checks the generation it names and the
 * tree's directory digest. */
#ifdef HAVE_IPOD_UI
static void ui_boot_stamp_settle(void)
{
    const char *no;

    if (!ui_stamp_due || ui_stamp_written || ui_stamp_spoil
        || !ui_boot_decided || ui_boot_deleted_due || ui_shutdown_intent
        || tagcache_usb_quiescing || ui_sleep_rescan || ui_sleep_walk
        || !ui_disk_used_since(current_tick - HZ))
        return;
    no = ui_stamp_block();
    if (no)
    {
        if (strcmp(no, "busy"))
            ui_stamp_due = false;   /* only a new vouch writes one now */
        return;
    }
    /* review L4: the UI thread may be writing it (a power-off or a sleep
     * entry): after it, this one is not needed */
    mutex_lock(&ui_stamp_mtx);
    if (!ui_stamp_written && !ui_shutdown_intent && !tagcache_usb_quiescing)
    {
        ui_stamp_due = false;
        if (boot_stamp_write(ui_walk_digest,
                             (uint32_t)ui_active->loaded_commitid,
                             ui_walk_skipped, ui_walk_rtc, true))
            ui_stamp_written = true;
    }
    mutex_unlock(&ui_stamp_mtx);
}
#endif /* HAVE_IPOD_UI */

/* M1: the USB exit (never its entry: no disk write there), before the
 * post-USB scan: a stamp a superseded power-off wrote no longer holds */
static void ui_boot_stamp_usb_exit(void)
{
    mutex_lock(&ui_stamp_mtx);      /* review L4 */
    if (ui_stamp_written)
    {
        ui_stamp_written = false;
        boot_stamp_remove();
    }
    mutex_unlock(&ui_stamp_mtx);
}

void tagcache_boot_window(void)
{
    queue_post(&tagcache_queue, Q_BOOT_CHECK, 0);
}

int tagcache_boot_check(int *permille)
{
    int st = ui_boot_state;

    if (permille)
    {
        long n = ui_boot_entries;
        *permille = st == 1 && n > 0
                    ? (int)MIN(999L, processed_dir_count * 1000L / n) : -1;
    }
    return st;
}
#endif /* UI_COMMIT */

#ifdef HAVE_IPOD_UI
void tagcache_quiet_kick(void)
{
#ifdef TC_QUIET_WAIT
    /* test and clear with IRQs off: two producers post one kick */
    int level = disable_irq_save();
    bool post = tc_quiet_blocked;
    tc_quiet_blocked = false;
    restore_irq(level);
    if (post)
        queue_post(&tagcache_queue, Q_QUIET_KICK, 0);
#endif
}
#endif

#ifdef TC_QUIET_WAIT
/* P19 19-1a: work the 1 s pass would find or start. Each of these keeps
 * the old poll while it is set; becoming set from outside this thread
 * kicks it (tagcache_quiet_kick(): the producers above, the sleep and
 * shutdown entries, the storage-idle notice, a stop or pause through
 * maint.c, dbq_resume_block()). Conservative: a pending commit or scan
 * keeps the poll even while playback holds it off. */
static bool tc_quiet_owed(bool check_done)
{
    return !check_done || ui_busy || ui_commit_cancel
        || ui_commit_pending || ui_scan_pending || ui_import_pending
        || ui_reload_pending || ui_rebuild_again || ui_rebuild_owed
        || ui_generation_unresolved
        || ui_sleep_rescan || ui_sleep_cut_seen || ui_sleep_woke
        || ui_sleep_walk || ui_stamp_spoil || ui_stamp_due
        || ui_pre_pending || ui_pre_request
        || ui_probe_owed || ui_deleted_owed || ui_boot_deleted_due
        || (ui_ram_yielded == ui_build && ui_build != ui_active)
        || ui_drain_force || ui_drain_sleep || ui_stats_recovering
        || (ui_pathq && ui_pathq->state == 2)
        || dbq_prepare_wait() || ui_drain_wanted()
        || maint_active() || maint_window_open()
        || (maint_wanted() && audio_status() != AUDIO_STATUS_PLAY);
}

/* the earliest time-based change the pass reacts to: a retry back-off
 * ending, the paused window's age, the play length that queues a Now
 * Playing cover (np-sharp), the idle export's quiet period. Only
 * future ones (qw_tc_earliest(), review H1): this runs after a pass, so a
 * past one's work is gated by something else (Backlight Off or a key or
 * a video holding the window shut, a worker that returned at its gate
 * without rescheduling) and polling it again changes nothing */
static long tc_quiet_deadline(void)
{
    long now = current_tick;
    long d = QW_BLOCK;
    long at;

    for (int i = 0; i < UI_RETRY_COUNT; i++)
        if (ui_retry[i].delay)
            d = qw_tc_earliest(now, d, ui_retry[i].next);
    if (maint_window_due(&at))
        d = qw_tc_earliest(now, d, at);
    if (artcache_poll_due(&at))     /* np-sharp: an album's play length */
        d = qw_tc_earliest(now, d, at);
    if (ui_stats_gen != ui_stats_exported && audio_status() == 0)
        d = qw_tc_earliest(now, d, ui_stats_tick + UI_EXPORT_QUIET);
    return d;
}

/* the wait, and whether it is the quiet kind (a producer must kick) */
static long tc_quiet_wait(bool check_done)
{
    bool poll = is_backlight_on(true) || tc_quiet_owed(check_done);
    long d = poll ? QW_BLOCK : tc_quiet_deadline();

    return qw_tc_wait(poll, current_tick, d, HZ);
}
#endif /* TC_QUIET_WAIT */

static void tagcache_thread(void)
{
    struct queue_event ev;
    bool check_done = false;
#ifdef HAVE_IPOD_UI
    bool ui_built = false;
    bool ui_rescan = false;     /* check for deleted files (after USB) */
#endif
    cpu_boost(true);
#ifdef UI_COMMIT
    /* Safe-replace recovery (#91) before anything opens the journal or
     * selects a generation: a cut replace is put back or its temp goes */
    dbjournal_recover(ui_db_root);
    dbgen_recover(ui_db_root);
    /* Ambiguous manifest I/O never authorizes rebuilding the legacy root.
     * Retry without mutating any generation until selection is trustworthy. */
    ui_generation_unresolved = !ui_generation_select();
#endif
    /* If the previous cache build/update was interrupted, commit
     * the changes first in foreground. */
    if (db_file_exists(TAGCACHE_FILE_TEMP))
#ifdef UI_COMMIT
    {
        /* never in the foreground: main.c waits for it behind the boot
         * logo (a 50k commit takes minutes on the device). A clean
         * database commits from the loop below, in the background (when
         * playback allows); a dirty one (a commit cut off) is rebuilt
         * there instead. */
        ui_commit_pending = true;
    }
#else
    {
#if !(defined(__APPLE__) && (CONFIG_PLATFORM & PLATFORM_SDL)) \
    && !defined(HAVE_IPOD_UI)   /* the shell never prompts: just commit */
        static const char *lines[] = {ID2P(LANG_TAGCACHE_BUSY),
                                      ID2P(LANG_TAGCACHE_UPDATE)};
        static const struct text_message message = {lines, 2};

        if (gui_syncyesno_run_w_tmo(HZ * 5, YESNO_YES, str(LANG_TAGCACHE),
                                    &message, NULL, NULL) == YESNO_YES)
#endif
        {
            allocate_tempbuf();
            commit();
            free_tempbuf();
        }
    }
#endif /* UI_COMMIT */

#ifdef HAVE_TC_RAMCACHE
#ifdef HAVE_EEPROM_SETTINGS
    if (firmware_settings.initialized && firmware_settings.disk_clean
        && global_settings.tagcache_ram)
    {
        check_done = tagcache_dumpload();
    }

    remove_db_file(TC_CTX_ARG(ui_build) TAGCACHE_STATEFILE);
#endif /* HAVE_EEPROM_SETTINGS */

#ifndef HAVE_IPOD_UI   /* load_ramcache() allocates, at the right size */
    /* Allocate space for the tagcache if found on disk. */
    if (global_settings.tagcache_ram && !tc_stat.ramcache)
        allocate_tagcache();
#endif
#endif /* HAVE_TC_RAMCACHE */

    cpu_boost(false);
    tc_stat.initialized = true;

    /* Don't delay bootup with the header check but do it on background. */
    if (!tc_stat.ready)
    {
        sleep(HZ);
        tagcache_commit_finalize();
    }
#ifdef UI_COMMIT
    /* an import cut off by a power-off (or never run) resumes */
    if (!ui_generation_fresh && db_file_exists(TAGCACHE_FILE_IMPORT))
    {
        ui_import_pending = true;
        queue_post(&tagcache_queue, Q_IMPORT_CHANGELOG, 0);
    }
#endif

#if defined(TC_QUIET_WAIT) && defined(HAVE_STORAGE_POWEROFF)
    storage_set_awake_hook(tagcache_quiet_kick);    /* P19 19-1a, M1 */
#endif
#ifdef UI_COMMIT
    /* the path hand-off slot: from the heap once, at boot, not BSS */
    {
        int ph = core_alloc_ex(sizeof(*ui_pathq), &buflib_ops_locked);
        if (ph > 0)
            ui_pathq = memset(core_get_data(ph), 0, sizeof(*ui_pathq));
    }
#endif
    while (1)
    {
#ifdef UI_COMMIT
        /* Back here nothing runs. A power-off whose wait timed out left
         * the cancel set (tagcache_prepare_shutdown()); if that power-off
         * was then aborted, every later commit and import would stop at
         * once until a reboot (#40). */
        if (ui_commit_cancel && !read_lock && !write_lock)
        {
            logf("tagcache: cancel cleared");
            ui_commit_cancel = false;
        }
#ifdef HAVE_IPOD_UI
        /* P18: a commit a sleep cut (as USB does at its connect): the
         * candidate goes, RAM only; the update is owed to the next
         * window (sleep entry, paused, stopped), never run at a wake */
        if (ui_sleep_cut_seen && !read_lock && !write_lock)
        {
            ui_sleep_cut_seen = false;
            if (ui_build != ui_active && !tc_stat.ready
                && !ui_candidate_maybe_published)
            {
                bool rebuild = ui_db_fresh;
                ui_candidate_abandon(-3);
                if (rebuild)
                    ui_rebuild_again = 2 | ui_rebuild_maint;
                ui_log("db", "commit suspended rebuild=%d", (int)rebuild);
            }
        }
        /* (review L2) stopped right after a wake is not a natural
         * moment: the user went idle (dark, no key) or a window opens */
        if (ui_sleep_woke && (maint_window_open() || artcache_user_idle()))
            ui_sleep_woke = false;
        if (ui_sleep_rescan && !ui_sleep_cut && !ui_sleep_woke
            && (audio_status() == 0 || maint_window_open()))
        {
            ui_sleep_rescan = false;
            ui_scan_pending = true;     /* the update again, below */
        }
#endif
#endif
        run_command_queue(false);
#if defined(UI_COMMIT) && defined(HAVE_IPOD_UI)
        ui_boot_stamp_settle();     /* npfix: a settled walk's stamp */
#endif

#ifdef TC_QUIET_WAIT
        {
            /* P19 19-1a: a drive coming up for an access kicks a dark
             * wait (storage_set_awake_hook(), set below at the start),
             * so the drain and the panel rotation ride that power-on;
             * ui_drain_kick() stays the one-shot idle notice that
             * run_command_queue() registers while statistics are queued
             * (review M2: no event slot of its own) */
            tc_quiet_blocked = true;
            long tmo = tc_quiet_wait(check_done);
            if (tmo == HZ)
                tc_quiet_blocked = false;   /* the old poll: no kick */
            queue_wait_w_tmo(&tagcache_queue, &ev, tmo);
            tc_quiet_blocked = false;
            DEVSTATS_POLL(DEVSTATS_POLL_TAGCACHE, ev.id == SYS_TIMEOUT);
            /* review L1: no kick may stay queued (one that raced the
             * flag): at the queue's head it would hide a Q_STOP_SCAN
             * from check_event_queue() and stop a precompute */
            {
                static const long kick_only[][2] = {
                    { Q_QUIET_KICK, Q_QUIET_KICK } };
                while (queue_peek_ex(&tagcache_queue, NULL,
                                     0 | QPEEK_REMOVE_EVENTS, kick_only))
                    ;
            }
            if (ev.id == Q_QUIET_KICK)
                ev.id = SYS_TIMEOUT;        /* a pass, as the poll's */
        }
#else
        queue_wait_w_tmo(&tagcache_queue, &ev, HZ);
#endif

#if defined(UI_COMMIT) && defined(HAVE_IPOD_UI)
        /* P18 (brief §4.3, review M3): the stamp sleep entry wrote holds
         * only while nothing is written after the wake. Removed here, on
         * this thread, at the first natural disk wake (the disk up for
         * someone else) and before any work of this thread's own (an
         * event, a drain, a scan); never with a spin-up just for it. A
         * crash before that point finds the stamp, and nothing was
         * written since it. */
        if (ui_stamp_spoil
            && (ui_disk_used_since(ui_spoil_from)
                || ev.id != SYS_TIMEOUT
                || ui_drain_wanted() || ui_scan_pending || ui_sleep_rescan
                || ui_commit_pending))
        {
            ui_stamp_spoil = false;
            mutex_lock(&ui_stamp_mtx);      /* review L4 */
            if (ui_stamp_written && !tagcache_usb_quiescing)
            {
                ui_stamp_written = false;
                boot_stamp_remove();
                UI_BS_LOG("stamp spoiled why=%s",
                          ev.id == SYS_TIMEOUT ? "disk" : "work");
            }
            mutex_unlock(&ui_stamp_mtx);
        }
#endif
#ifdef UI_COMMIT
        /* Once shutdown starts, finish its final numeric checkpoint but do
         * not start another scan/import/precompute behind the caller. */
        if (ui_shutdown_intent && ev.id != Q_FLUSH_STATS
            && ev.id != SYS_USB_CONNECTED)
            continue;
#endif
#ifdef UI_COMMIT
        /* statistics the producers could not bind, then the queued ones a
         * storage-idle notifier (or another thread) marked due; never at
         * USB (the host owns the disk) nor ahead of the shutdown flush */
        if (ev.id != SYS_USB_CONNECTED && ev.id != Q_FLUSH_STATS
            && !tagcache_usb_quiescing)
        {
            /* #109: a database queue's rebase and materialization, here,
             * outside every publication window, unboosted; when the gate
             * clears, the begin retry's back-off goes (review M-A) */
            bool dbq_waited = dbq_prepare_wait();
            ui_sleep_job = true;
            if (!ui_sleep_cut)      /* P18: none starts at sleep entry */
                dbq_tc_poll();
            ui_sleep_job = false;
            if (dbq_waited && !dbq_prepare_wait())
            {
                ui_retry[UI_RETRY_BEGIN].delay = 0;
                ui_retry[UI_RETRY_BEGIN].next = current_tick;
            }
            ui_pathq_run();
            if (ui_drain_wanted())
                command_queue_sync_callback();
        }
#endif
        switch (ev.id)
        {
#ifdef UI_COMMIT
            case Q_FLUSH_STATS:
                ui_shutdown_flushing = true;
                if (ui_shutdown_intent && !ui_generation_cancel(NULL))
                {
                    /* Appends only, never a compaction: a short, capped
                     * wait. Frames not written are still in the journal's
                     * previous state or replay from RAM at the next boot
                     * only if they were appended (B-M5). */
                    ui_pathq_run();
                    ui_stats_drain();
                }
                ui_shutdown_flushing = false;
                ui_shutdown_flush_done = true; /* no worker descriptor open */
                ui_shutdown_flush_pending = false;
                break;
#endif
            case Q_IMPORT_CHANGELOG:
#ifdef UI_COMMIT
                /* posted more than once (boot, commit, after USB): one
                 * pass, which clears ui_import_pending, is enough */
                if (!ui_import_pending)
                    break;
                if (ui_generation_unresolved)
                    break; /* pending marker remains for selection retry */
                if (ui_generation_fresh && !tc_stat.ready)
                {
                    ui_import_pending = false; /* fresh commit requeues import */
                    break;
                }
                if (ui_build == ui_active && !tc_stat.ready
                    && !db_file_exists(TAGCACHE_FILE_MASTER))
                {
                    /* no library to import into: the initial build owed
                     * below marks the import again (remove_files()) for
                     * its commit; waiting here would hold that build off
                     * for ever (an old build's marker, no database). A
                     * master that is there (dirty, still readable) keeps
                     * the import: else the initial build's export would
                     * overwrite the changelog with its statistics. */
                    ui_import_pending = false;
                    break;
                }
                if (ui_build == ui_active)
                {
                    if (!tc_stat.ramcache && tc_stat.ready
                        && global_settings.tagcache_ram)
                        load_ramcache();
                    if (!ui_candidate_begin())
                        break;
                    if (ui_generation_fresh && !tc_stat.ready)
                    {
                        ui_import_pending = false;
                        break;
                    }
                }
                if (ui_build == ui_active || ui_candidate_maybe_published)
                    break;
#endif
#ifdef HAVE_IPOD_UI
                UI_UPD_RUN(tagcache_import_changelog());
#else
                tagcache_import_changelog();
#endif
#ifdef UI_COMMIT
                ui_load_missing();
#endif
                break;

            case Q_REBUILD:
#ifdef UI_COMMIT
                if (ev.data < 2)    /* a new request (2, 3: internal) */
                    memset(ui_retry, 0, sizeof(ui_retry));
                /* the request stays until a rebuild begins (LOW-c) */
                ui_rebuild_maint = ev.data & 1;
                ui_rebuild_again = 2 | ui_rebuild_maint;
                if (!ui_candidate_begin())
                {
                    if (ui_retry_due(UI_RETRY_BEGIN))
                        ui_retry_later(UI_RETRY_BEGIN, "rebuild", -1);
                    break;
                }
#endif
#ifdef HAVE_IPOD_UI
                ui_maintenance_rebuild = ev.data & 1;
#endif
#ifdef UI_COMMIT
                /* The statistics survive: the import reads this export of
                 * the master on disk, never an older changelog that lacks
                 * what the last publication folded (MED-A). One that
                 * cannot run now keeps the active library and retries. A
                 * fresh rebuild (no readable master) imports the last
                 * changelog there is. */
                if (!ui_generation_fresh)
                {
                    remove_db_file(ui_build, TAGCACHE_FILE_CHANGELOG);
                    remove_db_file(ui_build, TAGCACHE_FILE_CHANGELOG ".new");
                    int exported = ui_export_changelog();
                    if (exported < 0)
                    {
                        ui_candidate_abandon(exported);
                        ui_retry_later(UI_RETRY_BEGIN, "export", -1);
                        break;
                    }
                    ui_candidate_exported = exported > 0;   /* 1 or 0 */
                }
                ui_rebuild_again = 0;
                ui_commit_pending = false;  /* the temp file goes */
#endif
                remove_files();
                remove_db_file(TC_CTX_ARG(ui_build) TAGCACHE_FILE_TEMP);
                tagcache_build();
#ifdef HAVE_IPOD_UI
                ui_maintenance_rebuild = false;
#endif
#ifdef UI_COMMIT
                ui_load_missing();
#endif
                break;

            case Q_UPDATE:
#ifdef UI_COMMIT
                memset(ui_retry, 0, sizeof(ui_retry)); /* a new request */
#endif
#ifdef HAVE_IPOD_UI
                ui_maintenance_rebuild = ev.data != 0;
#endif
                tagcache_build();
#ifdef UI_COMMIT
                /* reallocating a still valid cache would take memory
                 * from playback; after a commit it is gone and reloads.
                 * #109: a candidate whose commit waits for the stop loads
                 * it after that commit (ui_load_missing()) */
                ui_load_missing();
#elif defined(HAVE_TC_RAMCACHE)
                load_ramcache();
#endif
                check_deleted_files();
#ifdef HAVE_IPOD_UI
                ui_maintenance_rebuild = false;
#endif
                break ;

            case Q_START_SCAN:
                check_done = false;
#ifdef UI_COMMIT
                ui_win_scan = current_tick;     /* lib window: scan */
#endif
#ifdef HAVE_IPOD_UI
                /* posted by tree_restore(): after USB, files may be gone */
                ui_rescan = true;
#endif
#ifdef UI_COMMIT
                /* an import that USB stopped (the marker file is kept):
                 * resume it now, not at the next boot, so that exports
                 * are not held off for the rest of the session */
                if (ui_import_pending)
                    queue_post(&tagcache_queue, Q_IMPORT_CHANGELOG, 0);
#endif
                /* fallthrough */
#ifdef UI_COMMIT
            case Q_BOOT_CHECK:          /* the boot window: no wait */
#endif
            case SYS_TIMEOUT:
#ifdef UI_COMMIT
                maint_poll();
                ui_rebuild_run();
                if (ui_rebuild_again && ui_retry_due(UI_RETRY_BEGIN))
                    queue_post(&tagcache_queue, Q_REBUILD, ui_rebuild_again);
                ui_journal_compact_worker();
                if (ui_generation_unresolved)
                {
                    if (!ui_retry_due(UI_RETRY_SELECT))
                        break;
                    ui_generation_unresolved = !ui_generation_select();
                    if (ui_generation_unresolved)
                    {
                        ui_retry_later(UI_RETRY_SELECT, "gen select", -1);
                        break;
                    }
                }
                if (ui_scan_pending && !ui_commit_cancel
                    && !tagcache_usb_quiescing)
                {
                    if (!ui_retry_due(UI_RETRY_BEGIN))
                        break;      /* a deferred scan waits its backoff */
                    tagcache_build();
                    if (ui_deleted_owed && !ui_scan_pending)
                        check_deleted_files();  /* owed by a deferred update */
                    ui_load_missing();
                    break;
                }
                /* a fresh database's commit waiting below goes first:
                 * the import needs it (its commit marks the import) */
                if (ui_import_pending && !ui_candidate_maybe_published
                    && !(ui_commit_pending && ui_db_fresh))
                {
                    queue_post(&tagcache_queue, Q_IMPORT_CHANGELOG, 0);
                    break;
                }
#endif
#ifdef HAVE_IPOD_UI
            {
                /* No usable database (first boot, new format, a failed
                 * commit): build one silently, once per boot. The shell
                 * shows "Updating Library..." meanwhile. */
                bool built = false;
#ifdef UI_COMMIT
                /* a commit that waited for playback to stop: an update
                 * of a ready database, or a new one (a rebuild, the first
                 * build: remove_files() ran this boot, so ready is false
                 * and the initial build below has run already). A dirty
                 * database found at boot is neither (ui_db_fresh is set
                 * only by remove_files()): it is rebuilt below. */
                if (ui_commit_pending && (tc_stat.ready || ui_db_fresh)
                    && (audio_status() == 0 || ui_maint_due())
                    && read_lock == 0 && write_lock == 0)
                {
                    if (ui_build == ui_active)
                    {
                        /* A retained legacy temp file is only a scan hint.
                         * Never replay its in-place commit into active data. */
                        ui_load_missing();
                        if (!ui_candidate_begin())
                            break;
                        ui_commit_pending = false;
                        tagcache_build();
                        ui_load_missing();
                        break;
                    }
                    logf("tagcache: delayed commit");
                    /* #109: whatever commit() returns -- a torn temp
                     * header clears the pending commit and returns false,
                     * and the candidate still needs its RAM to publish */
                    commit();
                    ui_load_missing();
                }
                /* a reload that waited for room (load_ramcache()); #109:
                 * not a candidate's while its commit waits for the stop */
                else if (ui_reload_pending && ui_retry_due(UI_RETRY_LOAD)
                         && !ui_sleep_cut && ui_reload_fits())
                {
                    ui_sleep_job = true;
                    if (!ui_sleep_cut)  /* P18: never into a sleep */
                        ui_load_missing();
                    ui_sleep_job = false;
                }
                /* P16: a yielded candidate's RAM, at the stop or in a
                 * paused window (decided with no disk access) */
                else if (!ui_sleep_cut && ui_yield_reload_due())
                {
                    ui_sleep_job = true;
                    if (!ui_sleep_cut)
                        ui_load_missing();
                    ui_sleep_job = false;
                }
                /* P16: the commit took the window (a candidate's view
                 * pool, library.c lib_generation_pool_prepare(), a
                 * reload) but no longer fits above the floor, its own
                 * free memory counted once: it waits for the stop (Q11),
                 * so its tenancy ends now (maint_poll()'s regrow, once the
                 * other tenants are done too), not at the MAINT_JOB_TICKS
                 * lapse */
                if (ui_commit_pending && ui_maint_need && audio_status() != 0
                    && maint_holds(MAINT_COMMIT)
                    && !maint_take_fits(ui_maint_need))
                {
                    ui_log("db", "maint commit unfit need=%lu",
                           (unsigned long)ui_maint_need);
                    maint_done(MAINT_COMMIT, false);
                }
#endif
                if (!check_done && !tc_stat.ready && !ui_built)
                {
#ifdef UI_COMMIT
                    if (!ui_initial_build_begin())
                        break;
#endif
                    logf("tagcache: initial build");
                    ui_built = built = true;
#ifdef UI_COMMIT
                    /* a dirty database (interrupted commit) still has its
                     * statistics; an older format can't be read, and its
                     * last export is kept */
                    ui_export_changelog();
                    ui_commit_pending = false;  /* the temp file goes */
#endif
                    remove_files();
                    remove_db_file(TC_CTX_ARG(ui_build) TAGCACHE_FILE_TEMP);
                    tagcache_build();
                }
                if (check_done || !tc_stat.ready)
                {
                    ui_busy = false;
#ifdef UI_COMMIT
                    ui_boot_state = 2;  /* nothing to hold the boot for */
                    if (check_done)
                        ui_export_idle();
#endif
                    break ;
                }
#else
                if (check_done || !tc_stat.ready)
                    break ;
#endif

#ifdef HAVE_TC_RAMCACHE
                if (!tc_stat.ramcache && global_settings.tagcache_ram)
                {
#ifdef UI_COMMIT
                    /* #109: post-USB / boot pass: not a candidate's RAM
                     * while its commit waits for the stop */
                    ui_load_missing();
#else
                    load_ramcache();
#endif
#ifdef HAVE_IPOD_UI
                    if (built)  /* just scanned everything */
                    {
                        check_done = true;
#ifdef UI_COMMIT
                        ui_boot_state = 2;
#endif
                        break;
                    }
#endif
                    if (global_settings.tagcache_ram == TAGCACHE_RAM_ON
#ifdef HAVE_IPOD_UI
                        /* a disk lookup per entry with dircache off:
                         * after USB only, not at every boot */
                        && ui_rescan
#endif
                       )
                        check_file_refs(global_settings.tagcache_autoupdate);
                    if (tc_stat.ramcache && global_settings.tagcache_autoupdate
#ifdef UI_COMMIT
                        && !ui_boot_skip_walk(ui_rescan)    /* #89 */
#endif
                       )
                        tagcache_build();
#ifdef UI_COMMIT
                    if (!tc_stat.ramcache)  /* a commit dropped it */
                        ui_load_missing();
                    ui_boot_deleted_pass(); /* #89 H2 */
#endif
                }
                else
#endif /* HAVE_RC_RAMCACHE */
                if (global_settings.tagcache_autoupdate
#ifdef UI_COMMIT
                    && !ui_boot_skip_walk(ui_rescan)        /* #89 */
#endif
                   )
                {
                    tagcache_build();
#ifdef UI_COMMIT
                    ui_load_missing();
#endif

                    /* This will be very slow unless dircache is enabled
                       or target is flash based, but do it anyway for
                       consistency. */
#ifdef HAVE_IPOD_UI
                    if (ui_rescan)
#endif
                    check_deleted_files();
#ifdef UI_COMMIT
                    else
                        ui_boot_deleted_pass();     /* #89 H2 */
#endif
                }
#ifdef HAVE_IPOD_UI
                ui_rescan = false;
#endif

                logf("tagcache check done");

                check_done = true;
#ifdef UI_COMMIT
                ui_boot_state = 2;
#endif
#ifdef HAVE_IPOD_UI
                ui_busy = false;
            }
#endif
                break ;

            case Q_STOP_SCAN:
                break ;

            case SYS_POWEROFF:
            case SYS_REBOOT:
                break ;

            case SYS_USB_CONNECTED:
#ifdef UI_COMMIT
                /* A USB arrival can supersede a requested poweroff. Once
                 * this worker owns no descriptors, permit normal restore
                 * jobs again after disconnect. A stale flush event is inert. */
                ui_shutdown_intent = false;
                memset(ui_retry, 0, sizeof(ui_retry));
                /* A candidate still being built (a cut commit or scan:
                 * not ready) is dropped, RAM only, so the host waits for
                 * nothing; after USB the post-USB scan (or the sticky
                 * rebuild) builds it again from the active library. */
                if (ui_build != ui_active && !tc_stat.ready
                    && !ui_candidate_maybe_published)
                {
                    bool rebuild = ui_db_fresh;
                    ui_candidate_abandon(-2);
                    if (rebuild)
                        ui_rebuild_again = 2 | ui_rebuild_maint;
                }
                /* #89: RAM only, no disk write at USB */
                ui_usb_seq++;
                ui_usb_since_walk = true;
#ifdef HAVE_IPOD_UI
                /* npfix H1 (owner decision 2026-10-23, option a): a stamp written
                 * this session (the settle's, or a power-off's that USB
                 * superseded) is removed here, before the ack, so a host
                 * never gets the disk while one vouches for the library:
                 * a crash during or after the session walks at the next
                 * boot. This thread already writes before its ack (the
                 * changelog export); the UI thread still writes nothing
                 * at USB screen entry. */
                mutex_lock(&ui_stamp_mtx);
                ui_stamp_due = false;
                if (ui_stamp_written)
                {
                    ui_stamp_written = false;
                    boot_stamp_drop("usb");
                }
                mutex_unlock(&ui_stamp_mtx);
#endif
#endif
                logf("USB: TagCache");
#ifdef UI_COMMIT
                ui_worker_usb_parked = true;
#endif
                usb_acknowledge(SYS_USB_CONNECTED_ACK, ev.data);
                usb_wait_for_disconnect(&tagcache_queue);
#ifdef UI_COMMIT
                ui_worker_usb_parked = false;
#endif
                tagcache_usb_quiescing = false;
#ifdef UI_COMMIT
                ui_boot_stamp_usb_exit();   /* #89 M1: the exit, not entry */
#endif
#ifdef HAVE_IPOD_UI
                /* A first build interrupted by USB has no usable database.
                 * Its partial temp file was removed above, so let the normal
                 * timeout path restart it now that the filesystem is ours. */
                if (!tc_stat.ready)
                    ui_built = false;
#endif
#ifdef UI_COMMIT
                /* The device owns the filesystem again. Deferring this
                 * preserves the statistics without making the host wait
                 * through a full changelog export at connect. */
                ui_export_if_changed();
#endif
                break ;
        }
#ifdef UI_COMMIT
        /* a precompute that waited for playback to stop, or was stopped
         * by the event just handled */
        ui_precompute_run();
#endif
    }
}

bool tagcache_prepare_shutdown(void)
{
#ifdef UI_COMMIT
    ui_shutdown_intent = true;
    /* A power-off is never refused (silently, in the shell): a running
     * commit is cancelled (USR_CANCEL) and the database left dirty, so the
     * next boot rebuilds it in the background, keeping the statistics
     * (export, import). A commit in its last stage (the numeric indices:
     * one pass over the temp file and the master index) is left to finish
     * instead, which saves that rebuild; after UI_SHUTDOWN_FINISH_TICKS it
     * is cancelled too. The sort can't be cut short: the wait is boosted
     * (a 50k first build: ~12 s on the device instead of ~32 s unboosted)
     * and bounded (UI_SHUTDOWN_WAIT_TICKS, then logged). */
#define UI_SHUTDOWN_FINISH_TICKS (10 * HZ)
#define UI_SHUTDOWN_WAIT_TICKS   (60 * HZ)
    long waited;
    bool busy = tagcache_get_commit_step() > 0 || read_lock;

    if (busy && !ui_commit_finishing)
    {
        logf("tagcache: power-off cancels the commit");
        ui_commit_cancel = true;
    }
#else
    if (tagcache_get_commit_step() > 0)
        return false;
#endif

    tagcache_stop_scan();
#ifdef UI_COMMIT
    if (busy)
    {
        cpu_boost(true);
    }
    for (waited = 0; waited < UI_SHUTDOWN_WAIT_TICKS
         && (read_lock || write_lock || tagcache_get_commit_step() > 0);
         waited++)
    {
        if (waited == UI_SHUTDOWN_FINISH_TICKS && !ui_commit_cancel)
        {
            logf("tagcache: power-off cancels the last commit stage");
            ui_commit_cancel = true;
        }
        sleep(1);
    }
    if (busy)
    {
        cpu_boost(false);
    }
    if (waited < UI_SHUTDOWN_WAIT_TICKS)
        ui_commit_cancel = false;   /* nothing runs: a later commit may */
    else
    {
        /* left set: what still runs stops at its next check */
        logf("tagcache: power-off wait timed out");
    }
    if (waited > 0)
        DEBUGF("tagcache: power-off waited %ld ms%s\n", waited * 1000 / HZ,
               waited < UI_SHUTDOWN_WAIT_TICKS ? "" : " (timed out)");
#else
    while (read_lock || write_lock)
        sleep(1);
#endif

    return true;
}

void tagcache_shutdown(void)
{
#ifdef UI_COMMIT
    if (ui_shutdown_intent && !tagcache_usb_quiescing)
    {
        /* Only the worker may compact. The playback stop happened before
         * system_flush(), so its last absolute values are included now. */
        if (!ui_shutdown_flush_pending)
        {
            ui_shutdown_flush_done = false;
            ui_shutdown_flush_deadline = current_tick + UI_STATS_SHUTDOWN_TICKS;
            ui_shutdown_flush_pending = true;
            queue_post(&tagcache_queue, Q_FLUSH_STATS, 0);
        }
        long deadline = ui_shutdown_flush_deadline + HZ;
        while (!ui_shutdown_flush_done && TIME_BEFORE(current_tick, deadline)
               && !tagcache_usb_quiescing)
            sleep(1);
        ui_log("db", "off done=%d", (int)ui_shutdown_flush_done);
        /* #89: the stamp, after the flush, never at the USB handover */
        if (ui_shutdown_flush_done && !tagcache_usb_quiescing)
            ui_boot_stamp_clean();
        return;
    }
    if (tagcache_usb_quiescing)
        return; /* No checkpoint or additional filesystem work at handover. */
    /* Other system_flush() callers: the worker drains (it owns journal
     * work), waited for at most a second. #118: not once the worker has
     * acked a USB connect: it drains from RAM after the unplug (as the
     * forced flag stays set), so the wait only held the host back. */
    ui_drain_force = !COMMAND_QUEUE_IS_EMPTY || ui_stats_recovering;
#ifdef TC_QUIET_WAIT
    if (ui_drain_force)
        tagcache_quiet_kick();  /* P19 19-1a: the wait below needs it */
#endif
    for (long end = current_tick + HZ; ui_drain_force
         && (!COMMAND_QUEUE_IS_EMPTY || ui_stats_recovering)
         && !ui_worker_usb_parked
         && TIME_BEFORE(current_tick, end); )
        sleep(1);
    if (ui_worker_usb_parked && ui_drain_force)
        ui_log("db", "usb drain after unplug");
    return;
#endif
    /* Flush the command queue. */
    run_command_queue(true);
#ifdef UI_COMMIT
    /* Normal flushes append to the absolute journal. Clean shutdown above
     * additionally resolves any RAM-coalesced recovery on its owner thread. */
#endif

#if defined(HAVE_EEPROM_SETTINGS) && defined(HAVE_TC_RAMCACHE)
    if (tc_stat.ramcache)
        tagcache_dumpsave();
#endif
}

#ifdef HAVE_IPOD_UI
/* P18 (#86): a library job the sleep-entry window may run: a commit or
 * a RAM cache reload waiting for the stop. A reload or a yielded
 * candidate counts only when it can progress now (retry due, not given
 * up, its take fits): a stuck one would hold every window its whole cap,
 * boosted. Not on maint_window_open(): asked before the window opens. */
bool tagcache_ui_sleep_owed(void)
{
#ifdef UI_COMMIT
    return ui_commit_pending || ui_sleep_rescan
           || (ui_reload_pending && ui_retry_due(UI_RETRY_LOAD)
               && ui_audio_room_fits(ui_reload_bytes))
           || ui_sleep_cut_seen || ui_scan_pending || ui_scan_active
           || ui_in_commit
           || (ui_build != ui_active    /* a candidate to publish */
               && (ui_ram_yielded != ui_build || ui_yield_reload_can()));
#else
    return false;
#endif
}

/* P18 (#86): the window closed (true: running commits stop at their next
 * checkpoint) or the unit woke (false) */
void tagcache_ui_sleep_cut(bool cut)
{
#ifdef UI_COMMIT
    ui_sleep_cut = cut;
#ifdef TC_QUIET_WAIT
    tagcache_quiet_kick();      /* P19 19-1a: owed work may go on now */
#endif
#else
    (void)cut;
#endif
}

/* P18: a commit or a walk still runs (sleep entry waits for its stop) */
bool tagcache_ui_sleep_busy(void)
{
#ifdef UI_COMMIT
    return ui_in_commit || ui_scan_active || ui_sleep_job;
#else
    return false;
#endif
}

/* P18 (#86) sleep entry, UI thread, §4.3 step 3: the statistics drained
 * by the worker as any non-shutdown flush (appended to the journal, never
 * compacted; tagcache_shutdown() without the intent, which no sleep
 * sets), then the boot stamp by P16's rules with the Q11 wakes folded
 * into the skipped-boot count. Nothing at the USB handover. */
void tagcache_ui_sleep_prepare(const struct mp3entry *id3)
{
#ifdef UI_COMMIT
    if (tagcache_usb_quiescing || ui_shutdown_intent)
        return;
    /* the paused track's resume point, as tagtree's track-finish event
     * writes it at a stop (its 3 s rule), but no play counted: the track
     * goes on after the wake. Drained with the rest just below. */
    if (id3 && id3->tagcache_idx && id3->elapsed >= 3 * 1000
        && (global_settings.autoresume_enable || id3->is_audiobook
            || tagcache_ui_resumes(id3)))
    {
        static const unsigned char tags[2] = {
            tag_lastelapsed, tag_lastoffset };
        long vals[2] = { (long)id3->elapsed, (long)id3->offset };

        tagcache_ui_update_stats(id3->path, id3->tagcache_idx - 1,
                                 id3->tagcache_bind, 2, tags, vals);
    }
    /* review M2: RAM-coalesced statistics (ui_stats_recovering) go out
     * in this drain too, past the journal's back-off; sleep.c waits for
     * tagcache_ui_sleep_drained() */
    ui_drain_sleep = true;
    ui_stamp_spoil = false;     /* this entry's stamp supersedes the last */
    tagcache_shutdown();
    /* no stamp while an update is owed or running: a commit cut at this
     * entry drops its candidate (RAM), so after a power cut only the
     * boot walk would find those files again */
    if (!tagcache_usb_quiescing && !ui_in_commit && !ui_commit_pending
        && !ui_scan_pending && !ui_sleep_rescan && !ui_sleep_cut_seen)
        ui_boot_stamp_clean();
#else
    (void)id3;
#endif
}

/* P18 (review M2): sleep entry's drain is done: nothing queued, nothing
 * RAM-coalesced; what a clean power-off keeps is in the journal */
bool tagcache_ui_sleep_drained(void)
{
#ifdef UI_COMMIT
    return COMMAND_QUEUE_IS_EMPTY && !ui_stats_recovering && !ui_drain_force;
#else
    return true;
#endif
}

/* P18 (review M3, L2): the unit woke from a soft sleep (UI thread, before
 * the first frame; no I/O): the entry's stamp is spoiled at the next
 * natural disk wake, and a cut update waits for a natural moment */
void tagcache_ui_sleep_woke(void)
{
#ifdef UI_COMMIT
    ui_spoil_from = current_tick;
    ui_stamp_spoil = true;
    ui_sleep_woke = true;
#ifdef TC_QUIET_WAIT
    tagcache_quiet_kick();      /* P19 19-1a: polled from here on */
#endif
#endif
}

/* P18 (Q11, owner 2026-10-22), sleep entry, UI thread, before the window:
 * folds the soft-sleep wakes that count as boots into the skipped-boot
 * count, then owes the library check when that count would make the
 * next boot P16's tenth, or SLEEP_WALK_DAYS have passed since the last
 * completed walk (none recorded: owed). An owed check is a probe walk
 * plus the deleted-file pass, as a boot walk's (H2), run by the window
 * that is opening (the rescan below; ui_walk_vouch() ends it). True: the
 * check is owed now. */
bool tagcache_ui_sleep_walk(unsigned boots)
{
#ifdef UI_COMMIT
    ui_walk_skipped += boots;
    ui_log("db", "walk check now=%lu last=%lu ready=%d",
           (unsigned long)ui_sleep_rtc_now(), (unsigned long)ui_walk_rtc,
           (int)tc_stat.ready);
    if (tagcache_usb_quiescing || ui_shutdown_intent || !tc_stat.ready)
        return false;   /* (a build or rebuild walks anyway) */
    if (!ui_sleep_walk
        && (ui_walk_skipped + 1u >= (unsigned)BOOT_STAMP_WALK_EVERY
            || sleep_walk_owed(ui_sleep_rtc_now(), ui_walk_rtc)))
    {
        ui_log("db", "walk owed skipped=%u rtc=%lu", ui_walk_skipped,
               (unsigned long)ui_walk_rtc);
        ui_sleep_walk = true;
    }
    if (ui_sleep_walk)
    {
        ui_sleep_rescan = true;
        ui_deleted_owed = true;
#ifdef TC_QUIET_WAIT
        tagcache_quiet_kick();  /* P19 19-1a */
#endif
    }
    return ui_sleep_walk;
#else
    (void)boots;
    return false;
#endif
}

/* P18 (§4.3 boot-job table): the owed library job is a rebuild (a dirty
 * or new database: its files were removed, the selected generation is
 * unusable (a fresh one is built), or a rebuild waits to be
 * posted again), so the window logs it as "rebuild", not "commit" */
bool tagcache_ui_sleep_rebuild(void)
{
#ifdef UI_COMMIT
    return ui_db_fresh || ui_generation_fresh || ui_rebuild_again
           || ui_maintenance_rebuild;
#else
    return false;
#endif
}

/* P18: the owed check has not completed yet */
bool tagcache_ui_sleep_walking(void)
{
#ifdef UI_COMMIT
    return ui_sleep_walk;
#else
    return false;
#endif
}
#endif /* HAVE_IPOD_UI */

void tagcache_remove_statefile(void)
{
    remove_db_file(TC_CTX_ARG(ui_build) TAGCACHE_STATEFILE);
}

static int get_progress(void)
{
    int total_count = -1;

#ifdef HAVE_DIRCACHE
    struct dircache_info dcinfo;
    dircache_get_info(&dcinfo);
    if (dcinfo.status != DIRCACHE_IDLE &&
        ((!tc_stat.ramcache) || current_tcmh.tch.entry_count == 0))
    {
        total_count = dcinfo.entry_count;
    }
    else
#endif /* HAVE_DIRCACHE */
#ifdef HAVE_TC_RAMCACHE
    {
        if (tcramcache.hdr && tc_stat.ramcache)
            total_count = current_tcmh.tch.entry_count;
    }
#endif /* HAVE_TC_RAMCACHE */

    if (total_count < 0)
        return -1;
    else if (total_count == 0)
        return 0;
    else if (processed_dir_count > total_count)
        return 100;
    else
        return processed_dir_count * 100 / total_count;
}

struct tagcache_stat* tagcache_get_stat(void)
{
    tc_stat.total_entries = current_tcmh.tch.entry_count;
    tc_stat.progress = get_progress();
    tc_stat.processed_entries = processed_dir_count;

    return &tc_stat;
}

void tagcache_start_scan(void)
{
    queue_post(&tagcache_queue, Q_START_SCAN, 0);
}

bool tagcache_update(void)
{
    if (!tc_stat.ready)
        return false;

    queue_post(&tagcache_queue, Q_UPDATE, 0);
    return false;
}

bool tagcache_rebuild(void)
{
    queue_post(&tagcache_queue, Q_REBUILD, 0);
    return false;
}

#ifdef HAVE_IPOD_UI
bool tagcache_rebuild_maintenance(void)
{
    /* Keep the working database until the scan has a replacement.  Q_REBUILD
     * removes it up front, so a successful commit followed by a transient
     * RAM reload failure strands Music empty. Q_UPDATE still walks every
     * path, finds new/deleted metadata and commits it atomically. Set this
     * before posting so artwork cannot begin a stale-cache pass. */
    ui_maintenance_rebuild = true;
#ifdef UI_COMMIT
    ui_maintenance_for_snapshot = true;
#endif
    queue_post(&tagcache_queue, Q_UPDATE, 1);
    return false;
}

bool tagcache_maintenance_pending(void)
{
#ifdef UI_COMMIT
    return ui_maintenance_rebuild || ui_maintenance_for_snapshot;
#else
    return ui_maintenance_rebuild;
#endif
}

bool tagcache_scan_active(void)
{
    return ui_scan_active;
}

bool tagcache_ui_updating(void)
{
#ifdef UI_COMMIT
    /* a read-only probe walk changes nothing: only the scan it hands
     * over to, once it found a change, is an update */
    if ((ui_scan_active && !ui_probe) || ui_in_commit)
        return true;
#else
    if (ui_scan_active)
        return true;
#endif
    return ui_upd_jobs != 0;
}

int tagcache_ui_sidecars(void)
{
    return ui_sidecars_done;
}

void tagcache_ui_sidecars_reset(void)
{
    ui_sidecars_done = -1;
}

bool tagcache_ui_scan_owed(void)
{
#ifdef UI_COMMIT
    return ui_scan_pending;
#else
    return false;
#endif
}
#endif

void tagcache_stop_scan(void)
{
    queue_post(&tagcache_queue, Q_STOP_SCAN, 0);
}

void tagcache_usb_prepare(void)
{
#ifdef UI_COMMIT
    ui_usb_seq++;               /* #89: RAM only (no disk write at USB) */
    ui_usb_since_walk = true;
#endif
    tagcache_usb_quiescing = true;
}

void tagcache_usb_finish(void)
{
    tagcache_usb_quiescing = false;
}

#endif /* !__PCTOOL__ */


void tagcache_init(void)
{
    memset(&tc_stat, 0, sizeof(struct tagcache_stat));
    memset(&current_tcmh, 0, sizeof(struct master_header));
    filenametag_fd = -1;
    write_lock = read_lock = 0;

#ifndef __PCTOOL__
    strmemccpy(tc_stat.db_path, global_settings.tagcache_db_path,
               sizeof(tc_stat.db_path));
#ifdef UI_COMMIT
    strmemccpy(ui_db_root, tc_stat.db_path, sizeof(ui_db_root));
#endif
    mutex_init(&command_queue_mutex);
#ifdef UI_COMMIT
    mutex_init(&ui_stamp_mtx);      /* npfix review L4 */
#endif
    queue_init(&tagcache_queue, true);
#ifdef UI_COMMIT
    /* #89: the stamp is read and removed before the thread starts */
    boot_stamp_spoil();
    ui_worker_thread =
#endif
    create_thread(tagcache_thread, tagcache_stack,
                  sizeof(tagcache_stack), 0, tagcache_thread_name
                  IF_PRIO(, PRIORITY_BACKGROUND)
                  IF_COP(, CPU));
#else
    /* use default DB path */
    strcpy(tc_stat.db_path, ROCKBOX_DIR);
    tc_stat.initialized = true;
    allocate_tempbuf();
    commit();
    free_tempbuf();
    tc_stat.ready = check_all_headers(TC_CTX_PASS(ui_build));
#endif
}

#ifdef __PCTOOL__
void tagcache_reverse_scan(void)
{
    logf("Checking for deleted files");
    check_deleted_files();
}
#endif

bool tagcache_is_initialized(void)
{
    return tc_stat.initialized;
}
bool tagcache_is_fully_initialized(void)
{
    return tc_stat.readyvalid;
}
bool tagcache_is_usable(void)
{
#ifdef UI_COMMIT
    return ui_active->stat.initialized && ui_active->stat.ready;
#else
    return tc_stat.initialized && tc_stat.ready;
#endif
}
#ifdef HAVE_TC_RAMCACHE
bool tagcache_is_in_ram(void)
{
#ifdef UI_COMMIT
    return ui_active->stat.ramcache;
#else
    return tc_stat.ramcache;
#endif
}
#endif
int tagcache_get_commit_step(void)
{
    return tc_stat.commit_step;
}
int tagcache_get_max_commit_step(void)
{
    return (int)(SORTED_TAGS_COUNT)+1;
}
#if defined(SIMULATOR) && defined(UI_COMMIT)
/* Test-only observability: descriptors stay owned by the database thread. */
const char *sim_tagcache_selected(void)
{
    return sim_selected;
}

void sim_tagcache_recovery_state(long state[5])
{
    state[0] = ui_retries;
    state[1] = ui_prepares;
    state[2] = ui_active->master.tch.entry_count;
    state[3] = ui_stats_recovering;
    state[4] = ui_generation_fresh;
}

/* master index opens this boot (P16: none to decide a reload's fit) */
long sim_tagcache_master_opens(void)
{
    return ui_master_opens;
}

void sim_tagcache_snapshot_state(long state[6])
{
    int level = disable_irq_save();
    state[0] = ui_active->stat.ramcache_allocated;
    state[1] = ui_build != ui_active ? ui_build->stat.ramcache_allocated : 0;
    state[2] = ui_active->master.tch.entry_count;
    state[3] = ui_build != ui_active ? ui_build->master.tch.entry_count : 0;
    state[4] = ui_build != ui_active && ui_build->stat.ready
        && ui_build->stat.ramcache && ui_active->operations != 0;
    state[5] = ui_active->operations;
    restore_irq(level);
}
#endif
#endif /*!defined(PLUGIN)*/
