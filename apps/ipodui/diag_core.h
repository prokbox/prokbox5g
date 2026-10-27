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
 * iPod UI shell: the diagnostics report's pure logic (release-plan 6.2):
 * the privacy scrubber, the log tail and the bounded text builder
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
#ifndef _IPODUI_DIAG_CORE_H_
#define _IPODUI_DIAG_CORE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>

/* What a scrubbed field becomes */
#define DIAG_PATH   "<path>"        /* a file or folder name, a path */
#define DIAG_TITLE  "<title>"       /* a song, album, artist or book name */
#define DIAG_SYS    "<sys>"         /* the system folder's name */

#define DIAG_DENY_MIN 3             /* shorter deny strings are ignored */

/* Scrubs one log line (in[0..len), no '\n') into out (size >= 16), NUL
 * ended; returns the length written (truncated to fit). Rules, in order
 * of precedence at each position (tests/diag):
 *  - a deny string (the playing track's names and path parts, matched
 *    without case) becomes <title>;
 *  - "..." becomes <title> (unclosed: to the end of the line);
 *  - key=value for a name key (title artist album albumartist composer
 *    genre book song name comment grouping: <title>; file path dir folder
 *    url: <path>) loses its value to the end of the line;
 *  - a path ('/' at a token start, followed by a non-space) becomes
 *    <path>, through its file extension when it has a known one, else to
 *    the end of the line;
 *  - a known media/playlist/text extension (".flac", ".m4b", ".jpg", ...)
 *    becomes <path> back to the last safe boundary (the message's start
 *    after "<seq> <tag> ", an '=' or an earlier marker): names hold
 *    spaces and brackets, so nothing between is kept;
 *  - the system folder's name ("rockbox", any case) becomes <sys>;
 *  - a line still holding a non-ASCII or control byte after all that
 *    keeps its prefix only, the message replaced by <title>.
 * *hits (may be NULL) counts the fields replaced. */
size_t diag_scrub_line(const char *in, size_t len, char *out, size_t size,
                       const char *const *deny, int ndeny, unsigned *hits);

/* buf[0..len): a file's last bytes; whole: buf starts at the file's
 * start. Returns the offset of the first of the last n lines (a partial
 * first line is skipped unless whole). */
size_t diag_tail(const char *buf, size_t len, bool whole, int n);

/* The bounded text the report is built in: appends never overrun, the
 * text stays NUL ended and 'full' records a cut */
struct diag_buf {
    char *p;
    size_t len, size;
    bool full;
};
void diag_init(struct diag_buf *b, char *p, size_t size);
void diag_printf(struct diag_buf *b, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
/* scrubs each line of text[0..len) and appends it with 'indent' first */
void diag_put_lines(struct diag_buf *b, const char *indent, const char *text,
                    size_t len, const char *const *deny, int ndeny,
                    unsigned *hits);

#endif /* _IPODUI_DIAG_CORE_H_ */
