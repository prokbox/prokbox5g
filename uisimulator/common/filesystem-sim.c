/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 Daniel Stenberg
 * Copyright (C) 2014 Michael Sevakis
 * Modified for ProkBox5g (2026); see docs/changes-from-rockbox.md.
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
#define RB_FILESYSTEM_OS
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>
#include <limits.h>
#include "config.h"
#include "system.h"
#include "file.h"
#include "dir.h"
#ifdef HAVE_IPOD_UI
#include "thread.h"
#endif
#include "file_internal.h"
#include "pathfuncs.h"
#include "string-extra.h"
#include "debug.h"
#include "mv.h"

#ifndef os_fstatat
#define USE_OSDIRNAME /* we need to remember the open directory path */
#endif

extern const char *sim_root_dir;

/* Windows (and potentially other OSes) distinguish binary and text files.
 * Define a dummy for the others. */
#ifndef O_BINARY
#define O_BINARY 0
#endif

struct filestr_desc
{
    int  osfd;              /* The host OS file descriptor */
    bool mounted;           /* Is host volume still mounted? */
#ifdef HAVE_MULTIVOLUME
    int  volume;            /* The virtual volume number */
#endif
    int  cut;               /* fs_chaincut entry + 1, 0 none */
} openfiles[MAX_OPEN_FILES] =
{
    [0 ... MAX_OPEN_FILES-1] = { .osfd = -1 }
};

/* fs_chaincut <path> <bytes> (sim_cmd_sys.c): a file whose cluster chain
 * ends after 'bytes' while its directory entry keeps the whole size, the
 * signature of a write cut by a power loss (sitting 4c's panels.pak: 2
 * clusters, 32 KB, of a 5,407,744 B file; the rest of the chain free;
 * read-only fsck on the device). As the device's FAT code (fat.c
 * fat_seek, fat_readwrite): the size and lseek are unchanged; a read or
 * write that starts at or past the chain's end fails (EIO); a read that
 * crosses it stops there; a write that crosses it extends the chain from
 * its end, as fat_readwrite allocates. Keyed by the host file (device,
 * inode), so a rename keeps the damage and a new file (O_TRUNC, a
 * temporary renamed over it) does not have it. */
#include <sys/stat.h>
#define SIM_CUTS 4
static struct { dev_t dev; ino_t ino; off_t end; bool on; } sim_cuts[SIM_CUTS];

static int cut_of(int osfd)
{
    struct stat st;
    int i;

    if (fstat(osfd, &st) < 0)
        return 0;
    for (i = 0; i < SIM_CUTS; i++)
        if (sim_cuts[i].on && sim_cuts[i].dev == st.st_dev
            && sim_cuts[i].ino == st.st_ino)
            return i + 1;
    return 0;
}

/* bytes >= 0: cut the file's chain there (its size, or -1); bytes < 0:
 * 1 if the file's chain is still cut short, 0 if whole, -1 no file */
int sim_fs_chaincut(const char *path, long bytes)
{
    char ospath[SIM_TMPBUF_MAX_PATH];
    struct stat st;
    int i;

    if (sim_get_os_path(ospath, path, sizeof(ospath)) < 0
        || stat(ospath, &st) < 0)
        return -1;
    if (bytes < 0)
    {
        /* the query: 1 if this file's chain is still cut short */
        for (i = 0; i < SIM_CUTS; i++)
            if (sim_cuts[i].on && sim_cuts[i].dev == st.st_dev
                && sim_cuts[i].ino == st.st_ino)
                return sim_cuts[i].end < st.st_size;
        return 0;
    }
    for (i = 0; i < SIM_CUTS && sim_cuts[i].on; i++)
        if (sim_cuts[i].dev == st.st_dev && sim_cuts[i].ino == st.st_ino)
            break;
    if (i == SIM_CUTS)
        return -1;
    sim_cuts[i].dev = st.st_dev;
    sim_cuts[i].ino = st.st_ino;
    sim_cuts[i].end = bytes;
    sim_cuts[i].on = true;
    for (int f = 0; f < MAX_OPEN_FILES; f++)
        if (openfiles[f].osfd >= 0 && !openfiles[f].cut)
            openfiles[f].cut = cut_of(openfiles[f].osfd);
    return (int)st.st_size;
}

/* the bytes of [pos, pos + n) a cut chain lets through, -1 (EIO) none */
static ssize_t cut_span(struct filestr_desc *filestr, size_t n, bool write)
{
    off_t pos, end;

    if (!filestr->cut || !sim_cuts[filestr->cut - 1].on)
        return (ssize_t)n;
    end = sim_cuts[filestr->cut - 1].end;
    pos = os_lseek(filestr->osfd, 0, SEEK_CUR);
    if (pos < 0)
        return -1;
    if (n && pos >= end)
    {
        errno = EIO;
        return -1;
    }
    if (pos + (off_t)n > end)
    {
        if (!write)
            return (ssize_t)(end - pos);
        sim_cuts[filestr->cut - 1].end = pos + (off_t)n;
    }
    return (ssize_t)n;
}

static struct filestr_desc * alloc_filestr(int *fildesp)
{
    for (unsigned int i = 0; i < MAX_OPEN_FILES; i++)
    {
        struct filestr_desc *filestr = &openfiles[i];
        if (filestr->osfd < 0)
        {
            *fildesp = i;
            return filestr;
        }
    }

    return NULL;
}

static struct filestr_desc * get_filestr(int fildes)
{
    struct filestr_desc *filestr = &openfiles[fildes];

    if ((unsigned int)fildes >= MAX_OPEN_FILES || filestr->osfd < 0)
        filestr = NULL;
    else if (filestr->mounted)
        return filestr;

    errno = filestr ? ENXIO : EBADF;
    DEBUGF("fildes %d: %s\n", fildes, strerror(errno));
    return NULL;
}

struct dirstr_desc
{
    int               osfd;          /* Host OS directory file descriptor */
    bool              osfd_opened;   /* Host fd is another open file */
    OS_DIR_T          *osdirp;       /* Host OS directory stream */
#ifdef USE_OSDIRNAME
    char              *osdirname;    /* Host OS directory path */
#endif
    struct sim_dirent entry;         /* Rockbox directory entry */
#ifdef HAVE_MULTIVOLUME
    int               volume;        /* Virtual volume number */
    int               volumecounter; /* Counter for root volume entries */
#endif
    bool              mounted;       /* Is the virtual volume still mounted? */
} opendirs[MAX_OPEN_DIRS];

static struct dirstr_desc * alloc_dirstr(void)
{
    for (unsigned int i = 0; i < MAX_OPEN_DIRS; i++)
    {
        struct dirstr_desc *dirstr = &opendirs[i];
        if (dirstr->osdirp == NULL)
            return dirstr;
    }

    return NULL;
}

static struct dirstr_desc * get_dirstr(DIR *dirp)
{
    struct dirstr_desc *dirstr = (struct dirstr_desc *)dirp;

    if (!PTR_IN_ARRAY(opendirs, dirstr, MAX_OPEN_DIRS) || !dirstr->osdirp)
        dirstr = NULL;
    else if (dirstr->mounted)
        return dirstr;

    errno = dirstr ? ENXIO : EBADF;
    DEBUGF("dir #%d: %s\n", (int)(dirstr - opendirs), strerror(errno));
    return NULL;
}

static int close_dirstr(struct dirstr_desc *dirstr)
{
    OS_DIR_T *osdirp = dirstr->osdirp;

    dirstr->mounted = false;

#ifdef USE_OSDIRNAME
    free(dirstr->osdirname);
#endif
    if (dirstr->osfd_opened)
    {
        os_close(dirstr->osfd);
        dirstr->osfd_opened = false;
    }

    int rc = os_closedir(osdirp);
    dirstr->osdirp = NULL;

    return rc;
}

#ifdef HAVE_MULTIVOLUME
static int readdir_volume_inner(struct dirstr_desc *dirstr,
                                struct sim_dirent *entry)
{
    /* Volumes (secondary file systems) get inserted into the system root
     * directory. If the path specified volume 0, enumeration will not
     * include other volumes, but just its own files and directories.
     *
     * Fake special directories, which don't really exist, that will get
     * redirected upon opendir()
     */
    while (++dirstr->volumecounter < NUM_VOLUMES)
    {
        /* on the system root */
        if (!volume_present(dirstr->volumecounter))
            continue;

        get_volume_name(dirstr->volumecounter, entry->d_name);
        return 1;
    }

    /* do normal directory entry fetching */
    return 0;
}
#endif /* HAVE_MULTIVOLUME */

static inline int readdir_volume(struct dirstr_desc *dirstr,
                                 struct sim_dirent *entry)
{
#ifdef HAVE_MULTIVOLUME
    if (dirstr->volumecounter < NUM_VOLUMES)
        return readdir_volume_inner(dirstr, entry);
#endif /* HAVE_MULTIVOLUME */

    /* do normal directory entry fetching */
    return 0;
    (void)dirstr; (void)entry;
}


/** Internal functions **/

#ifdef HAVE_HOTSWAP
/**
 * Handle drive extraction by pretending the files' volumes no longer exist
 * and invalidating their I/O for the remainder of their lifetimes as would
 * happen on a native target
 */
void sim_ext_extracted(int drive)
{
    for (unsigned int i = 0; i < MAX_OPEN_FILES; i++)
    {
        struct filestr_desc *filestr = &openfiles[i];
        if (filestr->osfd >= 0 IF_MV(&& volume_drive(filestr->volume) == drive))
            filestr->mounted = false;
    }

    for (unsigned int i = 0; i < MAX_OPEN_DIRS; i++)
    {
        struct dirstr_desc *dirstr = &opendirs[i];
        if (dirstr->osdirp IF_MV(&& volume_drive(dirstr->volume) == drive))
            dirstr->mounted = false;
    }

    (void)drive;
}
#endif /* HAVE_HOTSWAP */

/**
 * Provides target-like path parsing behavior with single and multiple volumes
 * while performing minimal transforming of the input.
 *
 * Paths are sandboxed to the simulated namespace:
 *   e.g. "{simdir}/foo/../../../../bar" becomes "{simdir}/foo/../bar"
 */
int sim_get_os_path(char *buffer, const char *path, size_t bufsize)
{
    #define ADVBUF(amt) \
        ({ buffer += (amt); bufsize -= (amt); })

    #define PPP_SHOWPATHS 0

    if (!path_is_absolute(path))
    {
        DEBUGF("ERROR: path is not absolute: \"%s\"\n", path);
        errno = ENOENT;
        return -1;
    }

#if PPP_SHOWPATHS
    const char *const buffer0 = buffer;
    DEBUGF("PPP (pre): \"%s\"\n", path);
#endif

    /* Prepend sim root */
    size_t size = strlcpy(buffer, sim_root_dir, bufsize);
    if (size >= bufsize)
    {
        errno = ENAMETOOLONG;
        return -1;
    }
    ADVBUF(size);

#ifdef HAVE_MULTIVOLUME
    /* Track the last valid volume spec encountered */
    int volume = -1;
    bool sysroot = true;

    /* Basename of sim dir to switch back to simdisk from ext */
    #define DIRBASE_FMT  ".." PATH_SEPSTR "%s"
    ssize_t dirbase_len = 0;
    char dirbase[size + 3 + 1];

    /* Basename of ext directory to switch to alternate volume */
    #define SIMEXT_FMT   ".." PATH_SEPSTR "simext%d"
    char extbuf[sizeof (SIMEXT_FMT) + 20 + 1];
#endif /* HAVE_MULTIVOLUME */

    int level = 0;
    bool done = false;
    while (!done)
    {
        const char *p;
        ssize_t len = parse_path_component(&path, &p);


        switch (len)
        {
        case 0:
            done = true;
            if (path[-1] != PATH_SEPCH)
                continue;

            /* Path ends with a separator; preserve that */
            p = &path[-1];
            len = 1;
            break;

        case 1:
        case 2:
            if (p[0] == '.')
            {
                if (len == 1)
                    break;

                if (p[1] == '.')
                    goto is_dot_dot;
            }

        default:
            level++;

        #ifdef HAVE_MULTIVOLUME
            if (level != 1)
                break; /* Volume spec only valid @ root level */
            if (p[-1] != PATH_SEPCH)
                break;

            const char *next;
            volume = path_strip_volume(p-1, &next, true);

            if (volume == ROOT_VOLUME)
                volume = 0; /* FIXME: root no longer implies volume 0 */

            if (next > p)
            {
                /* Feign failure if the volume isn't "mounted" */
                if (!volume_present(volume))
                {
                    errno = ENXIO;
                    return -1;
                }

                sysroot = false;

                if (volume == 0)
                    continue;

                p = extbuf;
                len = snprintf(extbuf, sizeof (extbuf), SIMEXT_FMT, volume);
            }
        #endif /* HAVE_MULTIVOLUME */
            break;

        is_dot_dot:
            if (level <= 0)
                continue; /* Can't go above root; erase */

            level--;

        #ifdef HAVE_MULTIVOLUME
            if (level == 0)
            {
                int v = volume;
                bool sr = sysroot;
                volume = -1;
                sysroot = true;

                if (v <= 0)
                {
                    if (sr)
                        break;

                    continue;
                }

                /* Going up from a volume root and back down to the sys root */
                if (dirbase_len == 0)
                {
                    /* Get the main simdisk directory so it can be reentered */
                    char tmpbuf[sizeof (dirbase)];
                #ifdef WIN32
                    path_correct_separators(tmpbuf, sim_root_dir);
                    path_strip_drive(tmpbuf, &p, false);
                #else
                    p = tmpbuf;
                    strcpy(tmpbuf, sim_root_dir);
                #endif
                    size = path_basename(p, &p);
                    ((char *)p)[size] = '\0';

                    if (size == 0 || is_dotdir_name(p))
                    {
                        /* This is nonsense and won't work */
                        DEBUGF("ERROR: sim root dir basname is dotdir or"
                               " empty: \"%s\"\n", sim_root_dir);
                        errno = ENOENT;
                        return -1;
                    }

                    dirbase_len = snprintf(dirbase, sizeof (dirbase),
                                           DIRBASE_FMT, p);
                }

                p = dirbase;
                len = dirbase_len;
                break;
            }
        #endif /* HAVE_MULTIVOLUME */
            break;
        } /* end switch */

        char compname[len + 1];
        strmemcpy(compname, p, len);

        size = path_append(buffer, PA_SEP_HARD, compname, bufsize);
        if (size >= bufsize)
        {
            errno = ENAMETOOLONG;
            return -1;
        }
        ADVBUF(size);
    }

#if PPP_SHOWPATHS
    DEBUGF("PPP (post): \"%s\"" IF_MV(" vol:%d") "\n",
           buffer0 IF_MV(, volume));
#endif

    return IF_MV(volume) +1;
}


#ifdef HAVE_DEVSTATS
/* ProkBox Fit-1 M0: the simulator's disk model for devstats. Every data
 * read or write, and every open (a failed one too), is a storage command
 * of the calling thread; one that comes after the modelled ATA hold has run out is a
 * wake (firmware/drivers/ata.c: 1 s on a flash shell build, else 5 s).
 * No sector cache is modelled, so a sim count is an upper bound. */
#include "kernel.h"
#include "devstats.h"
#if defined(HAVE_IPOD_UI) && defined(HAVE_FLASH_STORAGE)
#define SIM_ATA_HOLD    (1*HZ)
#else
#define SIM_ATA_HOLD    (5*HZ)
#endif
static long sim_io_until;
static void sim_io(bool write, size_t bytes)
{
    bool wake = !sim_io_until || TIME_AFTER(current_tick, sim_io_until);
    long t = current_tick + SIM_ATA_HOLD;

    sim_io_until = t ? t : 1;
    DEVSTATS_IO(write, 0, (bytes + 511) / 512, wake, false);
}
#define SIM_IO0(w, n)   sim_io(w, n)
#else
#define SIM_IO0(w, n)   do { } while (0)
#endif

#ifdef HAVE_STORAGE_POWEROFF
/* ProkBox #114: the simulator's model of the adapter's power
 * (firmware/drivers/ata.c on the device, firmware/export/stor_pwr.h).
 * Every file call is an access; one that finds the power off waits
 * SIM_PWRUP (>= 250 ms, the device's wait plus its re-init) in the
 * calling thread, as the device's transfer does, then counts a power-on.
 * The cut comes from a tick task once the policy's hold has run out
 * (stor_pwr_core.c, the device's own code). USB powers it up before the
 * host is told (sim_tasks.c) and keeps it on. A KEEP_LOCK power-down
 * holds the model's lock, so an access in the dark blocks as on the
 * device. Scripts read it with the stor_* commands (sim_cmd_storpwr.c). */
#include "kernel.h"
#include "backlight.h"
#include "stor_pwr.h"
#define SIM_PWRUP   (HZ * 3 / 10)
static struct stor_pwr sim_spw;
static struct stor_pwr_tune sim_spw_tune = {
    .hold_dark = STOR_PWR_HOLD_DARK_S * HZ,
    .hold_lit  = STOR_PWR_HOLD_LIT_S * HZ,
    .min_off   = STOR_PWR_MIN_OFF_S * HZ,
    .hold_max  = STOR_PWR_HOLD_MAX_S * HZ,
    .relax     = STOR_PWR_RELAX_S * HZ,
};
static struct mutex sim_spw_mtx;
static bool sim_spw_ready, sim_spw_allow = true, sim_spw_usb;
static long sim_spw_pwrup = SIM_PWRUP;  /* stor_pwrup <ms> (npfix) */
static unsigned int sim_spw_ui = ~0u;   /* the UI thread (ui_main.c) */
static bool sim_spw_watch;              /* stor_watch_ui on|off */
static volatile long sim_spw_last;      /* tick of the last access */

static void sim_spw_tick(void)
{
    if (sim_spw.on && sim_spw_allow && !sim_spw_usb
        && stor_pwr_off_due(&sim_spw, &sim_spw_tune, current_tick,
                            sim_spw_last, is_backlight_on(true)))
        stor_pwr_note_off(&sim_spw, current_tick, STOR_PWR_R_IDLE, 1000/HZ);
}

static void sim_spw_init(void)
{
    if (sim_spw_ready)
        return;
    sim_spw_ready = true;
    mutex_init(&sim_spw_mtx);
    sim_spw_last = current_tick;
    stor_pwr_init(&sim_spw, current_tick, true);
    tick_add_task(sim_spw_tick);
}

static void (*sim_awake_hook)(void);    /* P19 19-1a (review M1) */

void storage_set_awake_hook(void (*fn)(void))
{
    sim_awake_hook = fn;
}

static void sim_spw_on(int reason)      /* the lock held */
{
    if (!sim_spw.on)
    {
        long t0 = current_tick;
        sleep(sim_spw_pwrup);
        /* before the state turns on: the tick task (another host
         * thread) must not see it on with the old access time */
        sim_spw_last = current_tick;
        stor_pwr_note_on(&sim_spw, &sim_spw_tune, current_tick,
                         current_tick - t0, reason, 1000/HZ);
        /* who powered it up: scripts count refills against other wakes */
#ifdef HAVE_DEVSTATS
        char by[16];
        devstats_slot_name(thread_self() & 0xff, by, sizeof(by)); /* the slot */
        fprintf(stderr, "sim stor on by=%s t=%ld\n", by,
                current_tick * 1000 / HZ);
#endif
        if (sim_awake_hook)
            sim_awake_hook();   /* P19 19-1a: as the device's ata.c */
    }
    sim_spw_last = current_tick;
}

void sim_stor_pwrup(long ms)
{
    sim_spw_pwrup = ms * HZ / 1000;
}

void sim_stor_ui_thread(unsigned int id)
{
    sim_spw_ui = id;
}

void sim_stor_watch_ui(bool on)
{
    sim_spw_watch = on;
}

void sim_stor_access(void)
{
    sim_spw_init();
    /* npfix: a UI-thread file call while the script watches (the
     * "npstall" rule: none while Now Playing is lit) */
    if (sim_spw_watch && thread_self() == sim_spw_ui)
        fprintf(stderr, "sim io ui pwr=%d\n", sim_spw.on ? 1 : 0);
    mutex_lock(&sim_spw_mtx);
    sim_spw_on(STOR_PWR_R_ACCESS);
    mutex_unlock(&sim_spw_mtx);
}

void sim_stor_tune(long hold_dark_ms, long min_off_ms)
{
    sim_spw_init();
    sim_spw_tune.hold_dark = hold_dark_ms * HZ / 1000;
    sim_spw_tune.hold_lit = hold_dark_ms * HZ / 1000;
    sim_spw_tune.min_off = min_off_ms * HZ / 1000;
    sim_spw_tune.relax = 2 * sim_spw_tune.min_off;
}

void sim_stor_usb(bool connected)
{
    sim_spw_init();
    mutex_lock(&sim_spw_mtx);
    sim_spw_usb = connected;
    if (connected)
        sim_spw_on(STOR_PWR_R_USB);
    mutex_unlock(&sim_spw_mtx);
}

int storage_power_down(int reason, unsigned flags)
{
    int rc = 0;

    sim_spw_init();
    mutex_lock(&sim_spw_mtx);
    if (sim_spw_usb)
        rc = -3;
    else if (!sim_spw_allow)
        rc = 1;
    else
        stor_pwr_note_off(&sim_spw, current_tick, reason, 1000/HZ);
    if (!(flags & STOR_PWR_KEEP_LOCK))
        mutex_unlock(&sim_spw_mtx);
    return rc;
}

void storage_power_release(void)
{
    mutex_unlock(&sim_spw_mtx);
}

int storage_power_up(int reason)
{
    sim_spw_init();
    mutex_lock(&sim_spw_mtx);
    sim_spw_on(reason);
    mutex_unlock(&sim_spw_mtx);
    return 0;
}

bool storage_powered(void)
{
    return !sim_spw_ready || sim_spw.on;
}

bool storage_power_warm(void)          /* the model: no standby */
{
    return storage_powered();
}

void storage_set_poweroff(bool allow)
{
    sim_spw_allow = allow;
}

bool storage_poweroff_allowed(void)
{
    return sim_spw_allow;
}

const struct stor_pwr *storage_power_state(void)
{
    sim_spw_init();
    return &sim_spw;
}
#define SIM_SPW()   sim_stor_access()
#else
#define SIM_SPW()   do { } while (0)
#endif
#ifdef HAVE_IPOD_UI
/* P12 (brief 3.6): the class accountant counts every sim file open/read/
 * write of a Class A module's screen (apps/ipodui/modclass.h) */
#include "ipodui/modclass.h"
#define SIM_MODCLASS(w, n)  modclass_io(!!(w), (n))
#else
#define SIM_MODCLASS(w, n)  do { } while (0)
#endif
#define SIM_IO(w, n)    do { SIM_SPW(); SIM_IO0(w, n); SIM_MODCLASS(w, n); } while (0)

/** File functions **/

#ifdef HAVE_IPOD_UI
static int open_total;  /* successful sim_open() calls; sim_open_count() */
/* writes of any kind since boot (opens for writing, writes, removals,
 * renames, mkdir, rmdir): the boot battery guard's "wrote nothing" */
static int write_total;
int sim_write_count(void)
{
    return write_total;
}
/* #118: the host owns the disk from the last USB ack (sim_tasks.c) to
 * the unplug. Any write in that window would race the host on a device;
 * each one is logged so a UI script can fail on it. */
static bool usb_host_window;
static int usb_host_writes;
void sim_usb_host_window(bool open)
{
    if (open)
        usb_host_writes = 0;
    else
        DEBUGF("ipodui: usb host window close writes=%d\n", usb_host_writes);
    usb_host_window = open;
}
extern unsigned int sim_script_thread_id;    /* sim_script.c */
static void sim_wrote(const char *op, const char *path)
{
    write_total++;
    /* the script thread stands for the host (its copies, removals and
     * screenshots): only the firmware's own threads count */
    if (usb_host_window && thread_self() != sim_script_thread_id)
    {
        usb_host_writes++;
        DEBUGF("ipodui: usb host write op=%s path=%s tid=%u\n", op,
               path ? path : "-", thread_self());
    }
}
#define SIM_WROTE()     sim_wrote(__func__, NULL)
#define SIM_WROTE_P(p)  sim_wrote(__func__, (p))
/* #85 B-H1: database files opened (or tried) on the audio thread */
static int open_audio_db;
extern unsigned int audio_thread_id;
int sim_open_audio_db_count(void)
{
    return open_audio_db;
}
#else
#define SIM_WROTE()     do { } while (0)
#define SIM_WROTE_P(p)  do { } while (0)
#endif

#ifdef HAVE_IPOD_UI
/* fix-closewrite: IPODUI_FSTRACE=<file> logs every file operation (op,
 * fd, path, thread) so tests/fatcrash/fsreplay can replay a session on
 * the real FAT code (the simulator's host files can't show FAT faults) */
#include <unistd.h>
static FILE *fstrace_f;
static int fstrace_on = -1;
static void fstrace(const char *fmt, ...)
{
    if (fstrace_on < 0)
    {
        const char *e = getenv("IPODUI_FSTRACE");
        fstrace_f = e ? fopen(e, "w") : NULL;
        fstrace_on = fstrace_f != NULL;
    }
    if (!fstrace_on)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(fstrace_f, fmt, ap);
    va_end(ap);
    fprintf(fstrace_f, "\t%x\n", (unsigned)thread_self());
    fflush(fstrace_f);
}
#define FSTRACE(...) fstrace(__VA_ARGS__)
#else
#define FSTRACE(...) do { } while (0)
#endif

int sim_open(const char *path, int oflag, ...)
{
    int fildes;
    struct filestr_desc *filestr = alloc_filestr(&fildes);
    if (!filestr)
    {
        errno = EMFILE;
        return -1;
    }

    char ospath[SIM_TMPBUF_MAX_PATH];
    int pprc = sim_get_os_path(ospath, path, sizeof (ospath));
    if (pprc < 0)
        return -2;
#ifdef HAVE_IPOD_UI
    if (thread_self() == audio_thread_id && strstr(path, "/database"))
        open_audio_db++;
#endif

    filestr->osfd = os_open(ospath, oflag | O_BINARY __OPEN_MODE_ARG);
    if (filestr->osfd < 0)
    {
        /* a failed open walks the directory too (on the device a scan of
         * every sector of it): a command, so a retry loop shows (S36) */
        SIM_IO(false, 0);
        FSTRACE("openfail\t%d\t%s\t%x", -1, path, oflag);
        return -3;
    }
    FSTRACE("open\t%d\t%s\t%x", fildes, path, oflag);
#ifdef HAVE_IPOD_UI
    open_total++;
#endif
    SIM_IO(oflag & (O_WRONLY | O_RDWR), 0);
    if (oflag & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC))
        SIM_WROTE_P(path);

#ifdef HAVE_MULTIVOLUME
    filestr->volume  = MAX(pprc - 1, 0);
#endif
    filestr->mounted = true;
    filestr->cut = cut_of(filestr->osfd);
    if (filestr->cut && (oflag & O_TRUNC))
    {
        sim_cuts[filestr->cut - 1].on = false;  /* a new, empty chain */
        filestr->cut = 0;
    }
    return fildes;
}

int sim_creat(const char *path, mode_t mode)
{
    return sim_open(path, O_WRONLY|O_CREAT|O_TRUNC, mode);
}

int sim_close(int fildes)
{
    struct filestr_desc *filestr = &openfiles[fildes];
    if ((unsigned int)fildes >= MAX_OPEN_FILES || filestr->osfd < 0)
    {
        errno = EBADF;
        return -1;
    }

    int osfd = filestr->osfd;
    filestr->osfd = -1;
    FSTRACE("close\t%d", fildes);
    return os_close(osfd);
}

int sim_ftruncate(int fildes, off_t length)
{
    struct filestr_desc *filestr = get_filestr(fildes);
    if (!filestr)
        return -1;

    off_t size = os_ffilesize(filestr->osfd);
    if (size < 0)
        return -1;

    if (length >= size)
        return 0;

    int rc = os_ftruncate(filestr->osfd, length);
    FSTRACE("ftruncate\t%d\t%ld", fildes, (long)length);

#ifdef HAVE_DIRCACHE
    if (rc >= 0)
        dircache_ftruncate(xxxxxx);
#endif

    return rc;
}

int sim_fsync(int fildes)
{
    struct filestr_desc *filestr = get_filestr(fildes);
    if (!filestr)
        return -1;

    int rc = os_fsync(filestr->osfd);
    FSTRACE("fsync\t%d", fildes);

#ifdef HAVE_DIRCACHE
    if (rc >= 0)
        dircache_fsync(xxxxxx);
#endif

    return rc;
}

off_t sim_lseek(int fildes, off_t offset, int whence)
{
    struct filestr_desc *filestr = get_filestr(fildes);
    if (!filestr)
        return -1;
    FSTRACE("lseek\t%d\t%ld\t%d", fildes, (long)offset, whence);

#ifdef HAVE_IPOD_UI
    /* as the device's file layer (firmware/common/file.c lseek_internal()):
     * no position past the end of the file (EINVAL), so code that would
     * write past EOF fails here too (#100 review M2) */
    {
        off_t cur = os_lseek(filestr->osfd, 0, SEEK_CUR);
        off_t size = os_lseek(filestr->osfd, 0, SEEK_END);
        off_t pos = whence == SEEK_SET ? offset
                  : whence == SEEK_CUR ? cur + offset : size + offset;

        if (cur < 0 || size < 0
            || os_lseek(filestr->osfd, cur, SEEK_SET) != cur)
            return -1;
        if (pos < 0 || pos > size)
        {
            errno = EINVAL;
            return -1;
        }
        return os_lseek(filestr->osfd, pos, SEEK_SET);
    }
#else
    return os_lseek(filestr->osfd, offset, whence);
#endif
}

#ifdef HAVE_IPOD_UI
/* read() calls of one thread, for the shell's traces ("ab open reads=") */
static unsigned int read_count_thread = ~0u;
static int read_count;

void sim_read_count_begin(void)
{
    read_count_thread = thread_self();
    read_count = 0;
}

int sim_read_count_end(void)
{
    read_count_thread = ~0u;
    return read_count;
}

/* a second counted thread (#90: the art builder's pass), so a pass never
 * steals the UI thread's count above: reads and bytes */
static unsigned int read_count2_thread = ~0u;
static int read_count2;
static unsigned long read_bytes2;

void sim_read_count2_begin(void)
{
    read_count2_thread = thread_self();
    read_count2 = 0;
    read_bytes2 = 0;
}

int sim_read_count2_end(unsigned long *bytes)
{
    read_count2_thread = ~0u;
    if (bytes)
        *bytes = read_bytes2;
    return read_count2;
}

/* file opens since boot, any thread (Phase 9 "open_count", sim_cmd_p9.c) */
int sim_open_count(void)
{
    return open_total;
}
#endif

ssize_t sim_read(int fildes, void *buf, size_t nbyte)
{
    struct filestr_desc *filestr = get_filestr(fildes);
    if (!filestr)
        return -1;
#ifdef HAVE_IPOD_UI
    if (thread_self() == read_count_thread)
        read_count++;
    if (thread_self() == read_count2_thread)
    {
        read_count2++;
        read_bytes2 += nbyte;
    }
#endif
    SIM_IO(false, nbyte);
    {
        ssize_t n = cut_span(filestr, nbyte, false);

        if (n < 0)
            return -1;
        nbyte = (size_t)n;
    }

    return os_read(filestr->osfd, buf, nbyte);
}

ssize_t sim_write(int fildes, const void *buf, size_t nbyte)
{
    struct filestr_desc *filestr = get_filestr(fildes);
    if (!filestr)
        return -1;
    SIM_IO(true, nbyte);
    SIM_WROTE();
    FSTRACE("write\t%d\t%ld", fildes, (long)nbyte);
    if (cut_span(filestr, nbyte, true) < 0)
        return -1;

    return os_write(filestr->osfd, buf, nbyte);
}

int sim_remove(const char *path)
{
    SIM_WROTE_P(path);
    char ospath[SIM_TMPBUF_MAX_PATH];
    if (sim_get_os_path(ospath, path, sizeof (ospath)) < 0)
        return -1;

    int rc = os_remove(ospath);
    FSTRACE("remove\t%d\t%s", rc, path);

#ifdef HAVE_DIRCACHE
    if (rc >= 0)
        dircache_remove(xxxxxx);
#endif

    return rc;
}

int sim_rename(const char *old, const char *new)
{
    SIM_WROTE_P(old);
    char osold[SIM_TMPBUF_MAX_PATH];
    int pprc1 = sim_get_os_path(osold, old, sizeof (osold));
    if (pprc1 < 0)
        return -1;

    char osnew[SIM_TMPBUF_MAX_PATH];
    int pprc2 = sim_get_os_path(osnew, new, sizeof (osnew));
    if (pprc2 < 0)
        return -1;

    if (MAX(pprc1 - 1, 0) != MAX(pprc2 - 1, 0))
    {
        /* Pretend they're different devices */
        errno = EXDEV;
        return -1;
    }

    int rc = os_rename(osold, osnew);
    FSTRACE("rename\t%d\t%s\t%s", rc, old, new);

#ifdef HAVE_DIRCACHE
    if (rc >= 0)
        dircache_rename(xxxxxx);
#endif

    return rc;
}

int sim_modtime(const char *path, time_t modtime)
{
    char ospath[SIM_TMPBUF_MAX_PATH];

    if (sim_get_os_path(ospath, path, sizeof (ospath)) < 0)
        return false;

    return os_modtime(ospath, modtime);
}

off_t sim_ffilesize(int fildes)
{
    struct filestr_desc *filestr = get_filestr(fildes);
    if (!filestr)
        return -1;

    return os_ffilesize(filestr->osfd);
}

int sim_fsamefile(int fildes1, int fildes2)
{
    struct filestr_desc *filestr1 = get_filestr(fildes1);
    if (!filestr1)
        return -1;

    struct filestr_desc *filestr2 = get_filestr(fildes2);
    if (!filestr2)
        return -1;

    if (filestr1 == filestr2)
        return 1;

    return os_fsamefile(filestr1->osfd, filestr2->osfd);
}

int sim_relate(const char *path1, const char *path2)
{
    char ospath1[SIM_TMPBUF_MAX_PATH];
    if (sim_get_os_path(ospath1, path1, sizeof (ospath1)) < 0)
        return -1;

    char ospath2[SIM_TMPBUF_MAX_PATH];
    if (sim_get_os_path(ospath2, path2, sizeof (ospath2)) < 0)
        return -1;

    return os_relate(ospath1, ospath2);
}

bool sim_file_exists(const char *path)
{
    char ospath[SIM_TMPBUF_MAX_PATH];
    if (sim_get_os_path(ospath, path, sizeof (ospath)) < 0)
        return false;

    SIM_IO(false, 0);
    return os_file_exists(ospath);
}


/** Directory functions **/
DIR * sim_opendir(const char *dirname)
{
    struct dirstr_desc *dirstr = alloc_dirstr();
    if (!dirstr)
    {
        errno = EMFILE;
        return NULL;
    }

    char osdirname[SIM_TMPBUF_MAX_PATH];
    int pprc = sim_get_os_path(osdirname, dirname, sizeof (osdirname));
    if (pprc < 0)
        return NULL;

    int rc = os_opendir_and_fd(osdirname, &dirstr->osdirp, &dirstr->osfd);
    if (rc < 0)
        return NULL;

    dirstr->osfd_opened = rc > 0;
    SIM_IO(false, 0);

#ifdef USE_OSDIRNAME
    dirstr->osdirname = strdup(osdirname);
    if (!dirstr->osdirname)
    {
        close_dirstr(dirstr);
        return NULL;
    }
#endif

    dirstr->entry.d_name[0] = 0; /* Mark as invalid */
#ifdef HAVE_MULTIVOLUME
    dirstr->volume = MAX(pprc - 1, 0);
    dirstr->volumecounter = pprc == 0 ? 0 : INT_MAX;
#endif
    dirstr->mounted = true;
    return (DIR *)dirstr; /* A-Okay */
}

int sim_closedir(DIR *dirp)
{
    struct dirstr_desc *dirstr = (struct dirstr_desc *)dirp;
    if (!PTR_IN_ARRAY(opendirs, dirstr, MAX_OPEN_DIRS) || !dirstr->osdirp)
    {
        errno = EBADF;
        return -1;
    }

    return close_dirstr(dirstr);
}

struct sim_dirent * sim_readdir(DIR *dirp)
{
    struct dirstr_desc *dirstr = get_dirstr(dirp);
    if (!dirstr)
        return NULL;

    struct sim_dirent *entry = &dirstr->entry;
    entry->info.osdirent = NULL;

    if (readdir_volume(dirstr, entry))
        return entry;

    OS_DIRENT_T *osdirent = os_readdir(dirstr->osdirp);
    if (!osdirent)
        return NULL;

    size_t size = sizeof (entry->d_name);
    if (strlcpy_from_os(entry->d_name, osdirent->d_name, size) >= size)
        FILE_ERROR_RETURN(ENAMETOOLONG, NULL);

    entry->info.osdirent = osdirent;
    return entry;
}

int sim_mkdir(const char *path)
{
    SIM_WROTE_P(path);
    char ospath[SIM_TMPBUF_MAX_PATH];
    if (sim_get_os_path(ospath, path, sizeof (ospath)) < 0)
        return -1;

    int rc = os_mkdir(ospath __MKDIR_MODE_ARG);

#ifdef HAVE_DIRCACHE
    if (rc >= 0)
        dircache_mkdir(xxxxxx);
#endif

    return rc;
}

int sim_rmdir(const char *path)
{
    SIM_WROTE_P(path);
    char ospath[SIM_TMPBUF_MAX_PATH];
    if (sim_get_os_path(ospath, path, sizeof (ospath)) < 0)
        return -1;

    int rc = os_rmdir(ospath);

#ifdef HAVE_DIRCACHE
    if (rc >= 0)
        dircache_rmdir(xxxxxx);
#endif

    return rc;
}

int sim_samedir(DIR *dirp1, DIR *dirp2)
{
    struct dirstr_desc *dirstr1 = get_dirstr(dirp1);
    if (!dirstr1)
        return -1;

    struct dirstr_desc *dirstr2 = get_dirstr(dirp2);
    if (!dirstr2)
        return -1;

    return os_fsamefile(dirstr1->osfd, dirstr2->osfd);
}

bool sim_dir_exists(const char *dirname)
{
    char osdirname[SIM_TMPBUF_MAX_PATH];
    if (sim_get_os_path(osdirname, dirname, sizeof (osdirname)) < 0)
        return false;

    OS_DIR_T *dirp = os_opendir(osdirname);
    if (!dirp)
        return false;

    os_closedir(dirp);
    return true;
}

struct dirinfo sim_dir_get_info(DIR *dirp, struct sim_dirent *entry)
{
    int rc;
    struct dirstr_desc *dirstr = get_dirstr(dirp);
    if (!dirstr)
        FILE_ERROR(ERRNO, RC);

    if (entry->d_name[0] == 0)
        FILE_ERROR(ENOENT, RC);

    OS_DIRENT_T *osdirent = entry->info.osdirent;
    if (osdirent == NULL)
        return (struct dirinfo){ .attribute = ATTR_MOUNT_POINT };

    struct dirinfo info;
    info.attribute = 0;

    OS_STAT_T s;

#ifdef os_fstatat
    if (os_fstatat(dirstr->osfd, entry->d_name, &s, 0))
#else /* no fstatat; build file name for stat() */
    char statpath[SIM_TMPBUF_MAX_PATH];
    if (path_append(statpath, dirstr->osdirname, entry->d_name,
                    sizeof (statpath)) >= sizeof (statpath))
    {
        FILE_ERROR(ENAMETOOLONG, RC);
    }

    if (os_stat(statpath, &s)) /* get info */
#endif /* os_fstatat */
    {
        /* File size larger than 2 GB? */
    #ifdef EOVERFLOW
        if (errno == EOVERFLOW)
            DEBUGF("stat overflow for \"%s\"\n", entry->d_name);
    #endif
        FILE_ERROR(ERRNO, RC);
    }

    if (S_ISDIR(s.st_mode))
        info.attribute |= ATTR_DIRECTORY;

    info.size = s.st_size;

    struct tm tm;
    if (localtime_r(&s.st_mtime, &tm) == NULL)
        FILE_ERROR(ERRNO, RC);

    info.mtime = mktime(&tm);

    return info;

file_error:
    return (struct dirinfo){ .size = 0 };
}

int os_volume_path(IF_MV(int volume, ) char *buffer, size_t bufsize)
{
    if (!buffer || !bufsize IF_MV( || !volume_present(volume) ))
        return -1;

    char tmpbuf[SIM_TMPBUF_MAX_PATH];
    tmpbuf[0] = '\0';

#ifdef HAVE_MULTIVOLUME
    char volname[VOL_MAX_LEN + 1];
    get_volume_name(volume, volname);

    if (path_append(tmpbuf, PA_SEP_HARD, volname, sizeof (tmpbuf))
            >= sizeof (volname))
        return -1;
#endif /* HAVE_MULTIVOLUME */

    if (path_append(tmpbuf, PA_SEP_HARD, ".", sizeof (tmpbuf))
        >= sizeof (tmpbuf))
        return -1;

    return sim_get_os_path(buffer, tmpbuf, bufsize);
}

const char* sim_root_realpath(void)
{
    return PATH_ROOTSTR;
}
