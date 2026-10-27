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
 * iPod UI shell
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
#include "config.h"
#include <string.h>
#include <limits.h>
#include "system.h"
#include "logf.h"
#include "debug.h"
#include "ipodui/screen_stack.h"

/* anim.h's UPDATE_TRACE, without anim.h's kernel/lcd includes (this file
 * also builds on the host: tests/ipodui, tests/nav, tests/arena) */
#define ARENA_TRACE(fmt, ...) do { \
        logf("ipodui: " fmt, __VA_ARGS__); \
        DEBUGF("ipodui: " fmt "\n", __VA_ARGS__); \
    } while (0)

static struct ui_screen stack[UI_STACK_MAX];
static int depth;
static struct { short y0, y1; } bands[UI_BANDS_MAX];   /* [y0, y1) */
static int nbands;
static int transition;          /* +1 push, -1 pop, 0 none */
static enum ui_transition transition_kind = UI_T_PUSH;
static ui_leave_hook_fn leave_hook;
static ui_cover_hook_fn cover_hook;
static struct ui_screen *pop_req; /* ui_pop_request(), one pending */

/* -- the screen arena (P11, screen_stack.h) -------------------------------- */

static unsigned char *arena;    /* UI_ARENA_ALIGN-aligned, pinned */
static uint16_t arena_size, arena_top;

void ui_arena_init(void *base, size_t size)
{
    uintptr_t a = ((uintptr_t)base + UI_ARENA_ALIGN - 1)
                  & ~(uintptr_t)(UI_ARENA_ALIGN - 1);
    size_t skip = a - (uintptr_t)base;

    arena = NULL;
    arena_size = arena_top = 0;
    if (!base || size <= skip)
        return;
    size -= skip;
    if (size > UI_ARENA_SIZE)
        size = UI_ARENA_SIZE;           /* the offsets are 16 bit */
    arena = (unsigned char *)a;
    arena_size = (uint16_t)(size & ~(size_t)(UI_ARENA_ALIGN - 1));
}

static size_t arena_aligned_top(void)
{
    return ((size_t)arena_top + UI_ARENA_ALIGN - 1)
           & ~(size_t)(UI_ARENA_ALIGN - 1);
}

void *ui_arena_alloc(size_t bytes)
{
    size_t off = arena_aligned_top();
    void *p;

    if (!arena || off > arena_size || bytes > arena_size - off)
    {
        ARENA_TRACE("arena full need=%u free=%u", (unsigned)bytes,
                    (unsigned)ui_arena_free());
        return NULL;
    }
    p = arena + off;
    memset(p, 0, bytes);
    arena_top = (uint16_t)(off + bytes);
    return p;
}

size_t ui_arena_free(void)
{
    size_t off = arena_aligned_top();

    return off < arena_size ? arena_size - off : 0;
}

void ui_arena_pop(void *p, size_t bytes)
{
    unsigned char *b = p;

    /* the newest block: it ends at the top, or at most the alignment's
     * padding below it (a block given back before it left a pad) */
    if (!arena || !b || b < arena || b + bytes > arena + arena_top
        || arena + arena_top - (b + bytes) >= UI_ARENA_ALIGN)
    {
        ARENA_TRACE("arena bad pop top=%u", (unsigned)arena_top);
        return;
    }
    arena_top = (uint16_t)(b - arena);
}

uint16_t ui_arena_mark(void)
{
    return arena_top;
}

void ui_arena_reset(uint16_t mark)
{
    if (mark > arena_top)
    {
        ARENA_TRACE("arena bad reset mark=%u top=%u", (unsigned)mark,
                    (unsigned)arena_top);
        return;
    }
    arena_top = mark;
}

void ui_set_cover_hook(ui_cover_hook_fn fn)
{
    cover_hook = fn;
}

void ui_set_leave_hook(ui_leave_hook_fn fn)
{
    leave_hook = fn;
}

bool ui_screen_live(const struct ui_screen *s)
{
    return s >= &stack[0] && s < &stack[depth];
}

/* The top screen leaves: the hook first (it may still look at 's'), then
 * leave(), then the slot is free. */
static void leave_top(void)
{
    struct ui_screen *s = &stack[depth - 1];

    if (pop_req == s)
        pop_req = NULL;
    if (leave_hook)
        leave_hook(s);
    if (s->ops->leave)
        s->ops->leave(s);
    ui_arena_reset(s->arena_mark);  /* whatever enter() took goes too */
    mod_screen_drop(s->ops);        /* #106: a module screen's hold */
    depth--;
    logf("ipodui: pop %s (%d)", s->ops->name, depth);
}

int ui_push(const struct ui_screen_ops *ops, const void *arg)
{
    return ui_push_t(ops, arg, UI_T_PUSH);
}

int ui_push_t(const struct ui_screen_ops *ops, const void *arg,
              enum ui_transition t)
{
    struct ui_screen *s;

    if (depth >= UI_STACK_MAX)
    {
        logf("ipodui: stack full, %s not pushed", ops->name);
        return -1;
    }

    s = &stack[depth++];
    memset(s, 0, sizeof(*s));
    s->ops = ops;
    s->dirty = UI_DIRTY_ALL;
    s->transition = t;
    s->arena_mark = ui_arena_mark();
    mod_screen_hold(ops);           /* #106: held while on the stack (a
                                     * stand-in's enter holds its module) */

    if (ops->enter && ops->enter(s, arg) < 0)
    {
        logf("ipodui: enter %s failed", ops->name);
        ui_arena_reset(s->arena_mark);  /* a failed enter() leaks nothing */
        mod_screen_drop(s->ops);
        depth--;
        return -1;
    }
    ARENA_TRACE("arena push s=%s n=%u free=%u", ops->name,
                (unsigned)(arena_top - s->arena_mark),
                (unsigned)ui_arena_free());

    ui_clear_dirty();
    s->dirty = UI_DIRTY_ALL;
    if (depth > 1)
    {
        transition = 1;
        transition_kind = t;
        if (cover_hook)
            cover_hook(&stack[depth - 2], ops);
    }
    logf("ipodui: push %s (%d)", ops->name, depth);
    return 0;
}

void ui_pop(void)
{
    struct ui_screen *s;

    if (depth <= 1)
        return;                     /* never pop the root */

    s = &stack[depth - 1];
    transition_kind = (enum ui_transition)s->transition;
    leave_top();

    ui_clear_dirty();
    stack[depth - 1].dirty = UI_DIRTY_ALL;
    transition = -1;
}

void ui_restage(void)
{
    if (depth <= 1)
        return;
    ui_clear_dirty();
    stack[depth - 1].dirty = UI_DIRTY_ALL;
    transition = 1;
    transition_kind = UI_T_PUSH;
}

void ui_pop_to_root(void)
{
    enum ui_transition k;

    if (depth <= 1)
        return;
    /* a system screen on top (Charging) keeps its plain slide back */
    k = stack[depth - 1].transition == UI_T_SYSTEM ? UI_T_SYSTEM : UI_T_PUSH;
    while (depth > 1)
        leave_top();
    transition_kind = k;
    ui_clear_dirty();
    stack[0].dirty = UI_DIRTY_ALL;
    transition = -1;
}

void ui_pop_request(struct ui_screen *s)
{
    pop_req = s;
}

bool ui_take_pop_request(void)
{
    bool on_top = pop_req && depth > 1 && pop_req == &stack[depth - 1];

    pop_req = NULL;
    return on_top;
}

int ui_depth(void)
{
    return depth;
}

int ui_take_transition(void)
{
    int t = transition;
    transition = 0;
    return t;
}

enum ui_transition ui_transition_kind(void)
{
    return transition_kind;
}

struct ui_screen *ui_top(void)
{
    return depth ? &stack[depth - 1] : NULL;
}

void ui_invalidate(unsigned dirty)
{
    if (depth)
        stack[depth - 1].dirty |= dirty;
}

static void merge_into(int i, int y0, int y1)
{
    bands[i].y0 = MIN(bands[i].y0, y0);
    bands[i].y1 = MAX(bands[i].y1, y1);
}

void ui_invalidate_band(int y, int h)
{
    int y1 = y + h;
    int i, best = 0, best_gap = INT_MAX;

    if (h <= 0 || !depth)
        return;
    stack[depth - 1].dirty |= UI_DIRTY_BAND;

    for (i = 0; i < nbands; i++)
    {
        int gap = MAX(bands[i].y0 - y1, y - bands[i].y1);
        if (gap <= 0)
        {
            merge_into(i, y, y1);
            goto coalesce;
        }
        if (gap < best_gap)
        {
            best_gap = gap;
            best = i;
        }
    }
    if (nbands < UI_BANDS_MAX)
    {
        bands[nbands].y0 = y;
        bands[nbands].y1 = y1;
        nbands++;
        goto coalesce;
    }
    merge_into(best, y, y1);

coalesce:
    /* a grown band may now touch the other one */
    if (nbands == 2 && MAX(bands[0].y0 - bands[1].y1,
                           bands[1].y0 - bands[0].y1) <= 0)
    {
        merge_into(0, bands[1].y0, bands[1].y1);
        nbands = 1;
    }
}

int ui_dirty_bands(int *y, int *h)
{
    int i, first = 0;

    if (nbands == 2 && bands[1].y0 < bands[0].y0)
        first = 1;
    for (i = 0; i < nbands; i++)
    {
        int b = (first + i) % nbands;
        y[i] = bands[b].y0;
        h[i] = bands[b].y1 - bands[b].y0;
    }
    return nbands;
}

void ui_clear_dirty(void)
{
    nbands = 0;
    if (depth)
        stack[depth - 1].dirty = 0;
}
