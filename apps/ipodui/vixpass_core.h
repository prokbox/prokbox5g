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
 * iPod UI shell: video index pass, the pure part (Phase 14a). No Rockbox
 * calls: the ledger's records, header and sum, the entry identity, the
 * keep / reuse / orphan decisions and the .vix names. apps/ipodui/
 * vixpass.c feeds it from the RAM database and does the file I/O;
 * tests/video/test_vixpass.c checks it on the host.
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
#ifndef _IPODUI_VIXPASS_CORE_H_
#define _IPODUI_VIXPASS_CORE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "vixpass.h"

#define VPC_MAGIC    0x58495650u    /* "PVIX" */
#define VPC_VERSION  2              /* 2: the id holds the filename seek */
#define VPC_NAME_LEN 16             /* == VIX_NAME_LEN (vix.h) */

struct vpc_hdr {                    /* 16 B, then 'count' records */
    uint32_t magic, version, count, sum;
};

#define VPC_USED(u, k)  ((u)[(k) >> 3] & (1u << ((k) & 7)))
#define VPC_MARK(u, k)  ((u)[(k) >> 3] |= (uint8_t)(1u << ((k) & 7)))

uint32_t vpc_fnv(uint32_t h, const void *p, size_t n);
#define VPC_FNV_INIT 2166136261u

/* An entry's identity: title, album and length (RAM database strings) and
 * the seek of its file name in the filename tag file. That seek is in RAM
 * (no disk read) and stays put across commits (the file is append-only),
 * but a renamed or moved file is a new entry with a new seek: so the old
 * record goes unused and its index is removed (M2b), and two copies of
 * one video are two identities. */
uint32_t vpc_id(const char *title, const char *album, uint32_t length,
                int32_t name_seek);

uint32_t vpc_sum(uint32_t h, const struct vixpass_ent *e, uint32_t n);
/* A header that may be followed by its records ('cap' fit); the sum is
 * checked by the caller once they are read */
bool vpc_hdr_ok(const struct vpc_hdr *h, uint32_t cap);

int vpc_cmp(const struct vixpass_ent *a, uint32_t id, uint32_t mtime);
/* An unused record of (id, mtime) in the sorted ledger 'old' (copies take
 * one each): marked used in 'used', its index; -1 if none */
int vpc_match(const struct vixpass_ent *old, int nold, uint8_t *used,
              uint32_t id, uint32_t mtime);
/* An unused record whose index is 'name' (a video whose identity changed
 * but whose file did not, e.g. a rebuilt database): marked used, its
 * index; -1 if none */
int vpc_claim_name(const struct vixpass_ent *old, int nold, uint8_t *used,
                   uint64_t name);
bool vpc_has_name(const struct vixpass_ent *e, int n, uint64_t name);
bool vpc_has_name64(const uint64_t *names, int n, uint64_t name);

/* Sorts e[] by (id, mtime), 'pend' alongside (may be NULL) */
void vpc_sort(struct vixpass_ent *e, int32_t *pend, int n);
/* Keeps the records whose pend[] is < 0 (resolved), in order: their count */
int vpc_keep_resolved(struct vixpass_ent *e, const int32_t *pend, int n);
/* Keeps the unused records that name an index, in order: their count */
int vpc_keep_unused(struct vixpass_ent *e, const uint8_t *used, int n);
/* The next records of the sorted merge of a[] and b[] into out[] (at most
 * 'cap'), advancing *ia and *ib: how many (0 at the end) */
int vpc_merge(const struct vixpass_ent *a, int na, int *ia,
              const struct vixpass_ent *b, int nb, int *ib,
              struct vixpass_ent *out, int cap);

/* VPC_NAME_LEN lower-case hex digits; false if not */
bool vpc_hex_name(const char *s, uint64_t *name);
/* A VIX_DIR entry "<16 hex>.vix"; false for anything else */
bool vpc_dir_name(const char *d_name, uint64_t *name);

#endif /* _IPODUI_VIXPASS_CORE_H_ */
