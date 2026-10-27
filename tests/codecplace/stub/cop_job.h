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

/* tests/codecplace: the cross-core channel, run inline. A job runs on the
 * caller's thread with "in worker" set, so codec_cop.c's callbacks take
 * their COP branch; a call runs its handler at once. The uncached alias
 * is a fixed offset, so the harness can tell the two views of id3 apart
 * (nothing dereferences it). */
#ifndef STUB_COP_JOB_H
#define STUB_COP_JOB_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define COP_SHARED_ATTR
#define COP_CALL_IO     4
#define COPJ_IN         0x01
#define COPJ_OUT        0x02
#define COPJ_CODE       0x04
#define COPJ_CALL_WROTE 0x08
typedef void (*cop_job_fn)(void *arg);
typedef intptr_t (*cop_call_fn)(intptr_t *io);
struct cop_worker {
    uint8_t started;
    uint32_t jobs, calls, call_wait_us;
};
bool cop_worker_start(struct cop_worker *w, void *stack, size_t stack_size,
                      const char *name, int priority);
void cop_worker_stop(struct cop_worker *w);
int  cop_job_post(struct cop_worker *w, cop_job_fn fn, void *arg,
                  unsigned flags);
void cop_job_wait(struct cop_worker *w, int ticket);
intptr_t cop_job_call(struct cop_worker *w, cop_call_fn fn, intptr_t *io,
                      unsigned flags);
bool cop_job_in_worker(const struct cop_worker *w);
bool cop_jobs_cross_core(void);
#define STUB_UNCACHED_OFS 0x10000000u
static inline void *cop_uncached(const void *p)
{
    return p ? (void *)((uintptr_t)p + STUB_UNCACHED_OFS) : NULL;
}
#endif
