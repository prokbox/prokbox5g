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
 * Shell font slot map, shared by firmware/font.c (HAVE_IPOD_UI) and the
 * font converter apps/ipodui/fonts/convttf-shell.c: one table, so the
 * glyphs the converter puts in the slots and the code points the
 * firmware sends there can't drift apart.
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
#ifndef FONT_SHELL_MAP_H
#define FONT_SHELL_MAP_H

/* The shell fonts (apps/ipodui/fonts) are one contiguous range,
 * U+0020..U+0237 (Heros has nothing in U+0238..U+024F). Characters above
 * it that real text uses (curly quotes, dashes, music notes, the euro
 * sign, ...) are drawn in the 32 C1 control slots U+0080..U+009F, which
 * text never displays as themselves: reaching U+266A directly would add
 * ~9,800 empty offset and width entries per font (~29 KB each).
 *
 * Where a slot's Windows-1252 character is one of these, it sits at its
 * Windows-1252 position, so text decoded as ISO-8859-1 that was really
 * Windows-1252 (old ID3v1 tags) still shows the right glyph. The slots
 * whose Windows-1252 character already has a glyph in range (Š Œ Ž š œ ž
 * Ÿ ƒ) or that are unassigned in Windows-1252 (0x81 0x8D 0x8F 0x90 0x9D)
 * carry the other characters. 0 = empty. */
static const unsigned short font_shell_slot[32] = {
    0x20AC, 0x2669, 0x201A, 0x266A, 0x201E, 0x2026, 0x2020, 0x2021, /* 80 */
    0x02C6, 0x2030, 0x266B, 0x2039, 0x266C, 0x2032, 0x2033, 0x2116, /* 88 */
    0x2009, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, /* 90 */
    0x02DC, 0x2122, 0xFB01, 0x203A, 0xFB02, 0x2192, 0x2190, 0x2212, /* 98 */
};

/* A C1 code point in text (U+0080..U+009F) is read as its Windows-1252
 * character; 0 = unassigned there (drawn as the default character). */
static const unsigned short font_shell_cp1252[32] = {
    0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, /* 80 */
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0,      0x017D, 0,      /* 88 */
    0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, /* 90 */
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178, /* 98 */
};

/* Characters drawn with another character's glyph (no glyph of their
 * own): spaces, hyphens and dashes that look the same at these sizes.
 * Sorted by code point (binary search). */
static const unsigned short font_shell_alias[][2] = {
    { 0x02BC, 0x2019 },     /* modifier apostrophe -> right quote */
    { 0x2002, 0x0020 },     /* en space */
    { 0x2003, 0x0020 },     /* em space */
    { 0x2004, 0x0020 },     /* three-per-em space */
    { 0x2005, 0x0020 },     /* four-per-em space */
    { 0x2006, 0x2009 },     /* six-per-em space -> thin */
    { 0x2007, 0x0020 },     /* figure space */
    { 0x2008, 0x2009 },     /* punctuation space -> thin */
    { 0x200A, 0x2009 },     /* hair space -> thin */
    { 0x2010, 0x002D },     /* hyphen */
    { 0x2011, 0x002D },     /* non-breaking hyphen */
    { 0x2012, 0x2013 },     /* figure dash -> en dash */
    { 0x2015, 0x2014 },     /* horizontal bar -> em dash */
    { 0x2024, 0x002E },     /* one-dot leader */
    { 0x2027, 0x00B7 },     /* hyphenation point */
    { 0x202F, 0x2009 },     /* narrow no-break space -> thin */
    { 0x205F, 0x2009 },     /* medium mathematical space -> thin */
};

/* The converter's last code point (gen.sh -l 591); a font ends at or
 * before it (Heros' last glyph in range is U+0237). */
#define FONT_SHELL_LAST 0x024F

#endif /* FONT_SHELL_MAP_H */
