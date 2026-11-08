/***************************************************************************
*             __________               __   ___.
*   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
*   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
*   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
*   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
*                     \/            \/     \/    \/            \/
*
* This is a memory allocator designed to provide reasonable management of free
* space and fast access to allocated data. More than one allocator can be used
* at a time by initializing multiple contexts.
*
* Copyright (C) 2009 Andrew Mahone
* Copyright (C) 2011 Thomas Martitz
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
#ifndef _BUFLIB_MEMPOOL_H_
#define _BUFLIB_MEMPOOL_H_

#ifndef _BUFLIB_H_
# error "include buflib.h instead"
#endif

#include "system.h"

/* Indices used to access block fields as block[BUFLIB_IDX_XXX] */
enum {
    BUFLIB_IDX_LEN,     /* length of the block, must come first */
    BUFLIB_IDX_HANDLE,  /* pointer to entry in the handle table */
    BUFLIB_IDX_OPS,     /* pointer to an ops struct */
    BUFLIB_IDX_PIN,     /* pin count */
#ifdef HAVE_BUFLIB_HEAPCHECK
    /* ProkBox, developer builds (sitting 4gh-b5): a CRC of LEN, the handle's
     * table index and OPS, sealed by buflib_alloc_ex() and buflib_shrink();
     * a move copies it with the block (none of the three changes).  Last,
     * so a payload underrun hits it first; an overrun from the block
     * before hits LEN */
    BUFLIB_IDX_CRC,
#endif
    BUFLIB_NUM_FIELDS,
};

union buflib_data
{
    intptr_t val;                 /* length of the block in n*sizeof(union buflib_data).
                                     Includes buflib metadata overhead. A negative value
                                     indicates block is unallocated */
    volatile unsigned pincount;   /* number of pins */
    struct buflib_callbacks* ops; /* callback functions for move and shrink. Can be NULL */
    char* alloc;                  /* start of allocated memory area */
    union buflib_data *handle;    /* pointer to entry in the handle table.
                                     Used during compaction for fast lookup */
};

struct buflib_context
{
    union buflib_data *handle_table;
    union buflib_data *first_free_handle;
    union buflib_data *last_handle;
    union buflib_data *buf_start;
    union buflib_data *alloc_end;
    bool compact;
};

#define BUFLIB_ALLOC_OVERHEAD (BUFLIB_NUM_FIELDS * sizeof(union buflib_data))

#ifndef BUFLIB_DEBUG_GET_DATA
static inline void *buflib_get_data(struct buflib_context *ctx, int handle)
{
    return (void *)ctx->handle_table[-handle].alloc;
}
#endif

static inline union buflib_data *_buflib_get_block_header(void *data)
{
    union buflib_data *bd = ALIGN_DOWN(data, sizeof(*bd));
    return bd - BUFLIB_NUM_FIELDS;
}

static inline void *buflib_get_data_pinned(struct buflib_context *ctx, int handle)
{
    void *data = buflib_get_data(ctx, handle);
    union buflib_data *bd = _buflib_get_block_header(data);

    bd[BUFLIB_IDX_PIN].pincount++;
    return data;
}

static inline void buflib_put_data_pinned(struct buflib_context *ctx, void *data)
{
    (void)ctx;
    union buflib_data *bd = _buflib_get_block_header(data);
    bd[BUFLIB_IDX_PIN].pincount--;
}

#ifdef HAVE_BUFLIB_HEAPCHECK
/* ProkBox, developer builds: the heap walk (buflib_walk).  Read-only and
 * bounded: every block from buf_start to alloc_end, then the handle table.
 * Never panics; the first bad block stops the walk and is described, with
 * the block just before it (the likeliest source of an overrun). */
enum buflib_walk_bad
{
    BUFLIB_WALK_OK = 0,
    BUFLIB_WALK_LEN_ZERO,       /* a zero length: the chain can't go on */
    BUFLIB_WALK_LEN_RANGE,      /* the length runs past alloc_end */
    BUFLIB_WALK_LEN_SHORT,      /* allocated, shorter than its header */
    BUFLIB_WALK_HANDLE_ALIGN,   /* the handle pointer is unaligned */
    BUFLIB_WALK_HANDLE_RANGE,   /* the handle pointer is outside the table */
    BUFLIB_WALK_BACKPTR,        /* the handle's data is outside the block */
    BUFLIB_WALK_CRC,            /* the header's CRC doesn't match */
    BUFLIB_WALK_HANDLES,        /* live handles != allocated blocks */
    BUFLIB_WALK_LIMIT,          /* more blocks than the caller's bound */
    BUFLIB_WALK_REASONS
};

struct buflib_walk
{
    int blocks;                 /* blocks walked (free and allocated) */
    int used;                   /* of them allocated */
    int handles;                /* live handles in the table */
    int bad;                    /* enum buflib_walk_bad */
    /* the first bad block (bad != 0) */
    const void *at;
    intptr_t len;               /* its length word, in units */
    const void *handle, *ops;   /* its HANDLE and OPS words, as found */
    uint32_t crc, want;         /* its CRC word, and what it should be */
    /* the block just before it (prev NULL: the bad one is the first) */
    const void *prev;
    intptr_t prev_len;          /* in units; < 0 free */
    int prev_handle;            /* its handle number (0 free or not valid) */
    const void *prev_ops;       /* its ops (the owner: a static struct) */
    const void *prev_move;      /* and its move callback (NULL: none) */
};

/* The short name of a reason ("crc", "lenrange", ...) */
const char *buflib_walk_why(int bad);
/* Walks ctx: the reason (0 ok) and *w filled.  max_blocks bounds it */
int buflib_walk(struct buflib_context *ctx, struct buflib_walk *w,
                int max_blocks);
#endif

#endif /* _BUFLIB_MEMPOOL_H_ */
