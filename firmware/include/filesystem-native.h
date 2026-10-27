/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 by Björn Stenberg
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
#ifndef _FILESYSTEM_NATIVE_H_
#define _FILESYSTEM_NATIVE_H_

#if defined(PLUGIN) || defined(CODEC)
#define FILEFUNCTIONS_DECLARED
#define FILEFUNCTIONS_DEFINED
#define DIRFUNCTIONS_DECLARED
#define DIRFUNCTIONS_DEFINED
#endif /* PLUGIN || CODEC */

#define FS_PREFIX(_x_) rbfs_ ## _x_
#endif /* _FILESYSTEM_NATIVE_H_ */

#ifdef _FILE_H_
#ifndef _FILESYSTEM_NATIVE__FILE_H_
#define _FILESYSTEM_NATIVE__FILE_H_

#ifdef RB_FILESYSTEM_OS
#define FILEFUNCTIONS_DEFINED
#endif

#ifndef FILEFUNCTIONS_DECLARED
#define __OPEN_MODE_ARG
#define __CREAT_MODE_ARG

#include <time.h>

int     rbfs_open(const char *name, int oflag);
int     rbfs_creat(const char *name);
int     rbfs_close(int fildes);
int     rbfs_ftruncate(int fildes, off_t length);
int     rbfs_fsync(int fildes);
off_t   rbfs_lseek(int fildes, off_t offset, int whence);
ssize_t rbfs_read(int fildes, void *buf, size_t nbyte);
ssize_t rbfs_write(int fildes, const void *buf, size_t nbyte);
int     rbfs_remove(const char *path);
int     rbfs_rename(const char *old, const char *new);
#ifdef HAVE_IPOD_UI
/* The last failed rbfs_rename(), for the shell's log: rc as returned, and
 * undo: 1 when an overwrite's victim could not be removed and the source
 * got its old name back (one entry per name), < 0 (fat_rename()'s rc) when
 * that failed too and two entries share the new name; 0 otherwise. */
struct rbfs_rename_fault { int rc, undo; };
/* a copy of it, and rc cleared (a function: the shell's remote calls wrap
 * functions only) */
void rbfs_rename_fault_take(struct rbfs_rename_fault *out);
/* #91: drop path if it is a second name on keep's cluster chain (a cut
 * inside a rename); 1 dropped, 0 different files or one missing, -1 error */
int     rbfs_remove_alias(const char *path, const char *keep);
/* #91: resolve a rename a power cut interrupted (one name left on the
 * chain); at boot, after the mount, before any app opens a file */
void    rbfs_rename_intent_boot(void);
/* #91 L: after a USB session: the record made again if the host removed
 * it; a pending one retired (the host's tree wins) */
void    rbfs_rename_intent_usb_exit(void);
/* A one-sector record file (file.c), written in place: one storage write
 * of its first data sector, no FAT or directory write, then a write
 * barrier. read/write: n bytes from its start (n <= its sector size);
 * returns n, or < 0 when missing, too small or failing. create: makes it
 * (one zeroed sector) if missing; 0 when it exists. Not BOOTLOADER. */
int     rbfs_record_read(const char *path, void *data, size_t n);
int     rbfs_record_write(const char *path, const void *data, size_t n);
int     rbfs_record_create(const char *path);
#endif
int     rbfs_modtime(const char *path, time_t modtime);
off_t   rbfs_ffilesize(int fildes);
int     rbfs_fsamefile(int fildes1, int fildes2);
int     rbfs_relate(const char *path1, const char *path2);
bool    rbfs_file_exists(const char *path);
#endif /* !FILEFUNCTIONS_DECLARED */

#if !defined(RB_FILESYSTEM_OS) && !defined (FILEFUNCTIONS_DEFINED)
#define rbfs_open(path, oflag, ...) rbfs_open(path, oflag)
#define rbfs_creat(path, mode)      rbfs_creat(path)
#endif /* FILEFUNCTIONS_DEFINED */

#endif /* _FILESYSTEM_NATIVE__FILE_H_ */
#endif /* _FILE_H_ */

#ifdef _DIR_H_
#ifndef _FILESYSTEM_NATIVE__DIR_H_
#define _FILESYSTEM_NATIVE__DIR_H_

#define DIRENT dirent
struct dirent;

struct dirinfo_native
{
    unsigned int attr;
    off_t        size;
    uint16_t     wrtdate;
    uint16_t     wrttime;
#if defined(HAVE_IPOD_UI) && !defined(BOOTLOADER)
    uint32_t     fstclus;   /* ProkBox #89: first cluster (boot digest) */
#endif
};

typedef struct {} DIR;

#ifndef DIRFUNCTIONS_DECLARED
#define __MKDIR_MODE_ARG

#ifdef RB_FILESYSTEM_OS
#define DIRFUNCTIONS_DEFINED
#endif

DIR *  rbfs_opendir(const char *dirname);
struct dirent * rbfs_readdir(DIR *dirp);
int    rbfs_readdir_r(DIR *dirp, struct dirent *entry,
                      struct dirent **result);
void   rbfs_rewinddir(DIR *dirp);
int    rbfs_closedir(DIR *dirp);
int    rbfs_mkdir(const char *path);
int    rbfs_rmdir(const char *path);
int    rbfs_samedir(DIR *dirp1, DIR *dirp2);
bool   rbfs_dir_exists(const char *dirname);

struct dirinfo rbfs_dir_get_info(DIR *dirp, struct DIRENT *entry);
const char *   rbfs_root_realpath(void);
#endif /* !DIRFUNCTIONS_DECLARED */

#endif /* _FILESYSTEM_NATIVE__DIR_H_ */
#endif /* _DIR_H_ */
