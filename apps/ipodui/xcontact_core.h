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
 * iPod UI shell: the vCard (.vcf) parser and the Contacts index (P12 W3-K,
 * docs/phase12-brief.md 3.5; the XIDX_CONTACTS kind of xindex_core.h).
 * Pure (stdint, string), host-tested in tests/xindex/test_xcontact.c.
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
#ifndef IPODUI_XCONTACT_CORE_H
#define IPODUI_XCONTACT_CORE_H

/* Contacts: the .vcf files of /Contacts (Q12, flat) -> contacts.idx.
 *
 * The parser reads vCard 2.1, 3.0 and 4.0 (one or many cards per file,
 * folded lines, quoted-printable, UTF-8 or a Latin-1 CHARSET, the
 * "item1." groups some exporters write) in any chunking: xcb_feed() takes
 * the bytes as they are read. It keeps FN/N, ORG, TEL (with its type),
 * EMAIL, ADR, BDAY and NOTE, each cut to a fixed size; PHOTO and the
 * rest are skipped. The builder gathers the cards into a pool, then
 * xcb_emit() sorts them twice (First, Last: Settings > Sort By) and lays
 * out the index file.
 *
 * File (little-endian, paged: xindex_core.h "Paged lists"):
 *   xidx_hdr (kind XIDX_CONTACTS, count = cards, rec_off 32, str_off/
 *   str_bytes = the card block)
 *   struct xcontact_tab                 at 32
 *   the First directory (npage[0] x struct xidx_pdir) at 48, then the
 *   Last directory (npage[1]); hdr_end = their end
 *   First page 0 at hdr_end, Last page 0 right after it (entry reads the
 *   header, both directories and both first pages in one read), then
 *   the First pages 1.., the Last pages 1..
 *   the card block at str_off: "" (offset 0), then each card in First
 *   order: its organisation (NUL-terminated, may be empty) directly
 *   followed by its detail items and a 0 byte, so a card is one bounded
 *   range read (XC_CARD_MAX) from its row's card offset.
 * A page row (struct xcontact_rec, xindex_core.h): the card's offset in
 * the card block, its label "Given Family" (page-relative) and flags.
 * The list shows "Given Family" in either order (the iPod's list does
 * not swap the names); only the order differs.
 *
 * Sizes: a typical card (a name of ~14 bytes, two phones, an e-mail,
 * one in five with an organisation or an address) costs ~100 bytes of
 * index: two rows (8 B + its label each) and ~60 B of card. 2,000 of
 * them make ~200 KB (measured: 200,046 B, tests/xindex);
 * XIDX_CONTACTS_FILE_MAX (256 KB) leaves ~25 % for heavier cards (about
 * 1,450 cards that each carry an organisation and a note fit),
 * XCONTACT_MAX (2,500) bounds the count. Only pages are
 * ever in a reader's RAM (1 KB each), so the file's size is not RAM-
 * bound; the builder's block is (XC_BUILD_NEED, sync time only).
 *
 * Detail items: kind byte (low nibble XCD_*, high
 * nibble XCS_* subtype), then the UTF-8 text (a '\n' breaks a line),
 * then NUL. The capped sizes below keep a card under ~1 KB. */

#include <stdint.h>
#include "ipodui/xindex_core.h"

#define XCONTACT_ROOT_DIR "/Contacts"   /* Q12: the visible folder */
#define XCONTACT_EXT      ".vcf"
#define XCONTACT_MAX      2500          /* cards indexed; more are cut
                                           (logged "xidx cut") */
#define XC_POOL           (192 * 1024)  /* the builder's string pool: the
                                           file cap binds first */
#define XC_PAGE_BYTES     1024          /* a page's target (~44 typical
                                           rows) */
#define XC_PAGE_LIMIT     XIDX_PAGE_LIMIT(XC_PAGE_BYTES, 8)  /* a page's
                                           most bytes (1024: 12 rows of
                                           the longest label fit under the
                                           target) */
#define XCONTACT_PDIR_MAX 192           /* pages per order: the directories
                                           stay <= 3 KB, inside the
                                           smallest reader's cache */
#define XC_LINE_MAX       640           /* a logical line; the rest of a
                                           longer one (a PHOTO) is dropped */
#define XC_NAME_MAX       64            /* given, family, organisation */
#define XC_VAL_MAX        96            /* a phone, an e-mail, an address
                                           part */
#define XC_NOTE_MAX       200
#define XC_DETAIL_MAX     480           /* bytes of items per card */
#define XC_ITEMS_MAX      14
#define XC_LABEL_MAX      (2 * XC_NAME_MAX + 1)
#define XC_CARD_MAX       (XC_NAME_MAX + 1 + XC_DETAIL_MAX + 1)  /* a card's bytes, at most */

enum xcd_kind { XCD_PHONE = 1, XCD_EMAIL = 2, XCD_ADDR = 3, XCD_BDAY = 4,
                XCD_NOTE = 5 };
enum xcd_sub  { XCS_NONE = 0, XCS_MOBILE = 1, XCS_HOME = 2, XCS_WORK = 3,
                XCS_OTHER = 4 };
#define XCD_KIND(b) ((b) & 0x0f)
#define XCD_SUB(b)  (((b) >> 4) & 0x0f)

/* xcontact_rec.flags (XCONTACT_ORG_ONLY is in xindex_core.h) */
#define XCONTACT_NO_NAME  0x0002        /* neither a name nor an
                                           organisation: the label is its
                                           first phone or e-mail */

/* at file offset 32 */
struct xcontact_tab {
    uint16_t npage[2];                  /* pages of the First, Last order */
    uint32_t hdr_end;                   /* the directories' end: First
                                           page 0 */
    uint32_t pad[2];
};
_Static_assert(sizeof(struct xcontact_tab) == 16, "xcontact_tab layout");
#define XCONTACT_TAB_OFF  sizeof(struct xidx_hdr)
#define XCONTACT_DIR_OFF  (XCONTACT_TAB_OFF + sizeof(struct xcontact_tab))

/* One gathered card (pool offsets; 0 is the empty string) */
struct xc_src {
    uint32_t given, family, org, detail;
    uint16_t dlen;
    uint16_t flags;
};

/* Key order: strcmp-like over two key texts ("Given Family" for First,
 * "Family Given" for Last) */
typedef int (*xcontact_cmp_fn)(const char *a, const char *b);

/* The parser + builder state; the caller provides the memory (the
 * indexer's block) and calls xcb_init() */
struct xc_item_buf {
    uint16_t len;
    uint8_t  n;
    char     b[XC_DETAIL_MAX];
};
struct xcbuild {
    /* output side */
    struct xc_src *src;
    char *pool;
    uint32_t pool_used, pool_cap, est, out_cap;
    uint32_t row_bytes;                 /* one order's rows (8 B + label
                                           each): bounds its pages */
    int n, max;
    uint8_t cut;                        /* a cap left cards out */
    uint16_t cards;                     /* BEGIN:VCARD seen (incl. cut) */
    void (*tick)(void);                 /* every 64 cards, or NULL */
    /* line assembler */
    uint16_t pl;
    uint8_t bol, softcont, in_card;
    char line[XC_LINE_MAX];
    /* the card in progress */
    char given[XC_NAME_MAX], family[XC_NAME_MAX], fn[2 * XC_NAME_MAX];
    char org[XC_NAME_MAX], note[XC_NOTE_MAX];
    struct xc_item_buf items;
    char tmp[2][XC_VAL_MAX * 2];        /* decoding scratch */
};

/* out_cap: the most bytes the finished index may take */
void xcb_init(struct xcbuild *b, struct xc_src *src, int max, char *pool,
              uint32_t pool_cap, uint32_t out_cap, void (*tick)(void));
/* Any number of bytes of a file, in order */
void xcb_feed(struct xcbuild *b, const void *buf, uint32_t n);
/* The end of one file: the last line is taken, an unfinished card dropped */
void xcb_end_file(struct xcbuild *b);
/* Lays out and seals the index. scratch: XCONTACT_SCRATCH(n) uint16_t,
 * 4-byte aligned.
 * Returns the file's bytes, XIDX_ECAP when it does not fit out_max. */
#define XCONTACT_SCRATCH(n) (4 * (n) + 4)
int32_t xcb_emit(struct xcbuild *b, void *out, uint32_t out_max,
                 uint32_t sig, xcontact_cmp_fn cmp, uint16_t *scratch);

/* A file the Contacts index takes: "*.vcf" (any case), not hidden */
int xcontact_is_file(const char *name);

#endif /* IPODUI_XCONTACT_CORE_H */
