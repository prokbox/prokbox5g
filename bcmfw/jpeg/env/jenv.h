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

/* bcmfw/jpeg/env: what jpeg_load.c, resize.c and bmp.c take from the rest
 * of Rockbox, for the JPEG spike (jpeg-spike.md S2): the colour LCD's
 * pixel format, struct bitmap, the FORMAT_* flags, and no-op kernel and
 * file calls (the decode runs from memory: JPEG_FROM_MEM). */
#ifndef JENV_H
#define JENV_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>               /* SEEK_*: the file mode's skips */
#include <string.h>
#include "config.h"

/* the simulator's values (build/sim-ui, ipodvideo, --ipodui): the
   reference (bcmfw/jpeg/host) is built from those objects */
#define HAVE_IPOD_UI 1          /* the shell's decoder: 64 KB bulk reads */
/* the decoder as jdec builds it (bcmfw/jpeg/jhook.h): bad Huffman codes
   fail, and its rows feed several outputs.  The shell's build doesn't
   define it; its twin of jdec will */
#define JPEG_JDEC 1
/* and built for jdec alone (outside Rockbox): the file loaders it never
   calls (bmp.c's read_bmp_fd, jpeg_load.c's clip_jpeg_file) are left out,
   which the display-only image's room needs.  The shell's twin of jdec
   builds with Rockbox's own headers, so keeps them */
#define JDEC_ALONE 1
#define HAVE_BMP_SCALING
#define HAVE_UPSCALER 1
#define HAVE_JPEG
#define RGB565 565
#define LCD_PIXELFORMAT RGB565
#define HORIZONTAL_STRIDE 2
#define VERTICAL_STRIDE 1
#define LCD_STRIDEFORMAT HORIZONTAL_STRIDE
#define LCD_FBWIDTH LCD_WIDTH
#define LCD_FBHEIGHT LCD_HEIGHT
#define LCD_PIXEL_ASPECT_WIDTH 1
#define LCD_PIXEL_ASPECT_HEIGHT 1
#define LCD_BM_WIDTH(width) (width)
#define LCD_BM_HEIGHT(height) (height)
#define FB_DATA_SZ 2
typedef unsigned short fb_data;
#define _RGBPACK_LCD(r,g,b) ( ((r) << 11) | ((g) << 5) | (b) )
#define LCD_RGBPACK_LCD(r,g,b) _RGBPACK_LCD((r), (g), (b))
#define _RGBPACK(r,g,b) _RGBPACK_LCD((r) >> 3, (g) >> 2, (b) >> 3)
#define LCD_RGBPACK(r,g,b) _RGBPACK((r), (g), (b))
#define FB_RGBPACK_LCD(r_,g_,b_) LCD_RGBPACK_LCD(r_, g_, b_)
#define IF_PIX_FMT(...) __VA_ARGS__

#define FORMAT_MONO        0x00000000
#define FORMAT_NATIVE      0x00000001
#define FORMAT_ANY         0x00000002
#define FORMAT_RETURN_SIZE 0x02000000
#define FORMAT_KEEP_ASPECT 0x04000000
#define FORMAT_RESIZE      0x08000000
#define FORMAT_REMOTE      0x10000000
#define FORMAT_DITHER      0x20000000
#define FORMAT_TRANSPARENT 0x40000000

struct bitmap {
    int width;
    int height;
    int format;
    unsigned char *maskdata;
    int alpha_offset;
    unsigned char *data;
};

#define ICODE_ATTR
#define ICONST_ATTR
#define IDATA_ATTR
#define IBSS_ATTR
#define INIT_ATTR
#define CACHEALIGN_ATTR
#define CACHEALIGN_AT_LEAST_ATTR(x) __attribute__((aligned(x)))
#define MEM_ALIGN_ATTR __attribute__((aligned(sizeof(intptr_t))))
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#define ALIGN_DOWN(n,a) ((typeof(n))((uintptr_t)(n)/(a)*(a)))
#define ALIGN_UP(n,a) ALIGN_DOWN((n)+((a)-1),a)
#define ALIGN_BUFFER(ptr,size,align) ({ size_t __sz = (size); size_t __ali = (align); \
    uintptr_t __a1 = (uintptr_t)(ptr); uintptr_t __a2 = __a1 + __sz; \
    __a1 = ALIGN_UP(__a1, __ali); __a2 = ALIGN_DOWN(__a2, __ali); \
    (ptr) = (typeof (ptr))__a1; (size) = __a2 > __a1 ? __a2 - __a1 : 0; })
#define SKIPBYTES(p,count) ((typeof (p))((char *)(p) + (count)))
#define PTR_ADD(ptr,x) ((typeof(ptr))((char*)(ptr) + (x)))
#define ARRAYLEN(a) (sizeof(a)/sizeof((a)[0]))
#define MAX(a,b) (((a)>(b))?(a):(b))
#define MIN(a,b) (((a)<(b))?(a):(b))
#define BIT_N(n) (1U << (n))
#define STRIDE_MAIN(w, h) (w)
#define letoh16(x) (x)
#define letoh32(x) (x)
#define DEBUGF(...)
#define LOGF(...)

/* the kernel: one decode at a time, alone (resize.c's boost cap asks
   which thread it is and the time; neither matters to the pixels).
   yield(): jdec's hook (jdec.h jdec_yield), which the BCM image sets to
   service its main loop meanwhile (S3c); unset, nothing */
extern void (*jdec_yield)(void);
static inline void yield(void) { if (jdec_yield) jdec_yield(); }
static inline void *thread_self(void) { return (void *)1; }
#define current_tick 0L

/* the files: the decoder's reads and skips go to jdec's source
   (bcmfw/jobs/jdec.c); bmp.c's file loader is compiled, never called */
#define O_RDONLY 0
int jdec_io_open(const char *path, int flags, ...);
int jdec_io_close(int fd);
long jdec_io_read(int fd, void *buf, unsigned long n);
long jdec_io_lseek(int fd, long off, int whence);
long jdec_io_size(int fd);
#define open jdec_io_open
#define close jdec_io_close
#define read jdec_io_read
#define lseek jdec_io_lseek
#define ffilesize jdec_io_size
#endif
