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
 *  fe_be_core: the front end / back end boundary's pure logic (P19 S1)
 *
 * docs/design/p19-ui-on-cop.md sections 2.3 and 7.1 (S1).  No Rockbox
 * calls: tests/febe/ builds this file on the host, alone and against a
 * two-core write-back cache model (tests/febe/cachemodel.c).
 *
 * What lives here:
 *  - the seqlock that guards the "now" record: one writer (the CPU), any
 *    number of readers (the front end, on the CPU or the COP);
 *  - the be_call slot: the front end's synchronous request record, its
 *    states and their single writers, and which cache operation each side
 *    does at each point (copj_cache_ops(), shared with cop_job: one table);
 *  - the generation hand-off for bulk tables (publish, see, acknowledge,
 *    reclaim), RCU-style;
 *  - a small checksum for the coherency canaries.
 *
 * Memory model.  Every shared word is a 32-bit word at a 4-byte aligned
 * address (the PP5022 aborts on a misaligned access) in memory both cores
 * see uncached: XCORE_BSS on the device.  Uncached stores from one core
 * reach memory in program order, and the PP5022's ARM7TDMI cores have no
 * write buffer reordering between uncached stores [hypothesis: design
 * section 8.3, checked by the canaries], so the only barrier needed is the
 * compiler's (FEBE_BARRIER).  Every access goes through FEBE_LD/FEBE_ST so
 * the host cache model can watch them.
 *
 * Copyright (C) 2026 prokopto-dev
 * Copyright (C) 2026 prokopto-dev
 *
 * Portions derived from Rockbox (rockbox.org).
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
#ifndef FE_BE_CORE_H
#define FE_BE_CORE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* ---- shared-word access --------------------------------------------- */
#ifdef FEBE_MEM_MODEL
/* tests/febe/cachemodel.c: every shared access is seen by the model */
uint32_t febe_mm_load(const volatile uint32_t *p);
void     febe_mm_store(volatile uint32_t *p, uint32_t v);
void     febe_mm_yield(void);           /* a scheduling point */
#define FEBE_LD(w)      febe_mm_load(&(w))
#define FEBE_ST(w, v)   febe_mm_store(&(w), (uint32_t)(v))
#define FEBE_BARRIER()  febe_mm_yield()
#else
#define FEBE_LD(w)      (*(const volatile uint32_t *)&(w))
#define FEBE_ST(w, v)   (*(volatile uint32_t *)&(w) = (uint32_t)(v))
#ifdef FEBE_HOST_SMP
/* tests/febe/ on a weakly ordered host (arm64): a real fence */
#define FEBE_BARRIER()  __sync_synchronize()
#else
#define FEBE_BARRIER()  __asm__ __volatile__("" ::: "memory")
#endif
#endif

/* ---- the seqlock ------------------------------------------------------
 * Writer (one core, never two at once: on the device every writer is a
 * CPU thread inside a section that does not yield, and Rockbox's threads
 * on one core switch only when they yield or block):
 *     febe_seq_write_begin(&seq); ...stores...; febe_seq_write_end(&seq);
 * Reader: febe_seq_copy() copies a record of whole words, retrying while a
 * write is in progress or happened during the copy. */
void febe_seq_write_begin(volatile uint32_t *seq);
void febe_seq_write_end(volatile uint32_t *seq);
/* Copies nwords from src to dst under seq.  Returns the number of tries
 * (>= 1) on a consistent copy, or -1 after max_tries (dst then holds the
 * last, possibly torn, copy: the caller logs it and uses the old value). */
int  febe_seq_copy(const volatile uint32_t *seq, const volatile uint32_t *src,
                   uint32_t *dst, unsigned nwords, unsigned max_tries);

/* ---- the canary checksum -------------------------------------------- */
/* FNV-1a over 32-bit words; never 0 (0 means "no canary") */
uint32_t febe_sum(const uint32_t *w, unsigned nwords);

/* ---- the be_call slot ---------------------------------------------------
 * One slot per front-end caller thread (design Q-U7: four, one used).
 * The slot record lives uncached.  Each state has one writer:
 *   FREE    -> FILLED  : the front end (it wrote the arguments)
 *   FILLED  -> TAKEN   : the back end (it read them; it now owns the slot)
 *   TAKEN   -> DONE    : the back end (the results are written)
 *   DONE    -> FREE    : the front end (it copied the results out)
 * The front end never withdraws a call: it waits for DONE however long it
 * takes (logging a stall), so the back end can never write into a record
 * or buffer the front end has given up on (the S10 withdraw race cannot
 * arise here). */
enum febe_slot_state {
    FEBE_SLOT_FREE = 0,
    FEBE_SLOT_FILLED,
    FEBE_SLOT_TAKEN,
    FEBE_SLOT_DONE,
};

#define FEBE_ARG_WORDS  16          /* small values in and out (a remote
                                     * call: fn, 12 words, a 64-bit result) */
#define FEBE_SLOT_BYTES 256         /* a path or a small struct, in and out */

struct febe_slot {
    uint32_t state;                 /* enum febe_slot_state */
    uint32_t seq;                   /* the call's number (canary, identity) */
    uint32_t kind;                  /* enum be_kind (fe_be.h) */
    uint32_t flags;                 /* FEBE_F_* */
    uint32_t ret;                   /* the handler's return value */
    uint32_t sum;                   /* canary: febe_sum over arg[] at fill */
    uint32_t arg[FEBE_ARG_WORDS];   /* in and out */
    uint32_t bytes[FEBE_SLOT_BYTES / 4];    /* in and out, word aligned */
};

/* What a call touches (the be_call flags) */
#define FEBE_F_IN_BULK  0x01        /* the BE reads an FE buffer (not the
                                     * slot): the FE writes back first */
#define FEBE_F_WROTE    0x02        /* the BE wrote an FE buffer through the
                                     * uncached alias or by DMA: the FE
                                     * writes back and drops after DONE */

/* the front end */
bool febe_slot_fill(struct febe_slot *s, uint32_t seq, uint32_t kind,
                    uint32_t flags, const uint32_t *arg, unsigned nargs);
bool febe_slot_done(const struct febe_slot *s);
/* copies the results out and frees the slot; false if not DONE */
bool febe_slot_collect(struct febe_slot *s, uint32_t *arg, unsigned nargs,
                       uint32_t *ret);
/* the back end: FILLED -> TAKEN (false: nothing to take, or a canary
 * mismatch, which *bad then says: the call is still answered, with
 * ret = FEBE_RET_BADSUM, so the front end never waits for ever) */
bool febe_slot_take(struct febe_slot *s, bool *bad);
void febe_slot_finish(struct febe_slot *s, uint32_t ret);
#define FEBE_RET_BADSUM 0xFEBEBAD0u

/* Which cache operation each side does (copj_cache_ops() codes,
 * COPJ_C_*): the FE before it posts, the FE after DONE.  The BE never does
 * a cache operation for the FE: it reads the slot uncached and writes FE
 * buffers through the uncached alias (design section 2.3), so the codec's
 * hot lines on the CPU are never dropped. */
unsigned febe_ops_post(uint32_t flags, bool cross_core);
unsigned febe_ops_reply(uint32_t flags, bool cross_core);

/* ---- generations: bulk tables by hand-off --------------------------------
 * The BE builds a table, writes back (xcore_publish()), then publishes.
 * The FE sees a new generation, drops its cache once (xcore_acquire()),
 * reads, and acknowledges.  The BE reuses or frees the old table only once
 * the FE acknowledged a newer one.  pub has one writer (the BE), ack one
 * (the FE).  Wrap-safe (signed difference). */
struct febe_gen {
    uint32_t pub;
    uint32_t ack;
};
void febe_gen_publish(struct febe_gen *g);          /* BE */
/* FE: true if a generation newer than *seen is out (then *seen = it, and
 * the FE must acquire before it reads the table) */
bool febe_gen_check(const struct febe_gen *g, uint32_t *seen);
void febe_gen_ack(struct febe_gen *g, uint32_t seen);  /* FE */
/* BE: the FE has acknowledged generation gen or a newer one */
bool febe_gen_reclaimable(const struct febe_gen *g, uint32_t gen);

#endif /* FE_BE_CORE_H */
