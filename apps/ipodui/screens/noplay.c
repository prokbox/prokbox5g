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

/* #286: why a greyed row can't play (owner, 2026-11-13). SELECT on a song,
 * audiobook or video the library draws greyed (list.c, the can't-play
 * glyph) shows a short page instead of doing nothing:
 *   - a video: the video screen's own "Can't Play Video" page with the
 *     reason (ui_video_show_why(), screens/video.c), the page a video that
 *     fails at its start shows; nothing tries to play;
 *   - a song, a podcast episode or a book: this page, in the same light
 *     prompt style (the header title, a heading, one sentence, OK).
 * ui_noplay_row() is the one entry the lists use (browse.c, search.c,
 * otglist.c): the row's reason and kind from the library's RAM flag
 * word, the page if it can't play.
 * Nothing plays, nothing stops, the queue is untouched: what was playing
 * keeps playing. SELECT (OK) or MENU pops back to the list, which kept its
 * selected row and scroll (the list screen stays on the stack). Every
 * other key is swallowed, as on the video page. No state of its own: the
 * reason and kind ride in s->priv. */

#include "config.h"
#include <stdbool.h>
#include <stdint.h>
#include "system.h"
#include "kernel.h"
#include "lang.h"
#include "audio.h"
#include "metadata.h"
#include "tagcache.h"
#include "ipodui/library.h"
#include "ipodui/anim.h"
#include "ipodui/uilog.h"
#include "ipodui/ipodui.h"
#include "ipodui/screen_stack.h"
#include "ipodui/screens/setwidgets.h"
#include "ipodui/screens/screens.h"
#ifdef HAVE_BCMFW_VIDEO
#include "ipodui/screens/video.h"
#endif

static const short noplay_ok[1] = { LANG_IPODUI_OK };

/* the header title per kind (UI_NOPLAY_VIDEO never gets this page) */
static const short noplay_title[UI_NOPLAY_VIDEO] = {
    [UI_NOPLAY_SONG]    = LANG_IPODUI_NOPLAY_SONG_TITLE,
    [UI_NOPLAY_BOOK]    = LANG_IPODUI_NOPLAY_BOOK_TITLE,
    [UI_NOPLAY_EPISODE] = LANG_IPODUI_NOPLAY_EPISODE_TITLE,
};
static const char *const noplay_kind_name[UI_NOPLAY_VIDEO] = {
    "song", "book", "episode"
};

/* s->priv: the reason (enum unplayable_reason) in bits 0-7, the kind
 * (enum ui_noplay_kind) above */
#define PRIV_WHY(s)  ((int)((intptr_t)(s)->priv & 0xff))
#define PRIV_KIND(s) ((int)((intptr_t)(s)->priv >> 8))

static bool protected_why(int why)
{
    return why == UNPLAYABLE_DRM || why == UNPLAYABLE_PROTECTED;
}

static int noplay_enter(struct ui_screen *s, const void *arg)
{
    intptr_t v = (intptr_t)arg;

    s->priv = (void *)v;
    s->title = str(noplay_title[PRIV_KIND(s)]);
    /* hdr: the header's width, 0 = the full 246 px title (P20: the
     * Japanese titles need it; a split header would cut them) */
    UPDATE_TRACE("noplay page why=%d kind=%s audio=%d hdr=%d", PRIV_WHY(s),
                 noplay_kind_name[PRIV_KIND(s)], audio_status(), s->header_w);
    (void)noplay_kind_name;     /* the trace compiles out on target */
    return 0;
}

static void noplay_draw(struct ui_screen *s, unsigned dirty)
{
    if (dirty & UI_DIRTY_BODY)
    {
        if (protected_why(PRIV_WHY(s)))
            ui_prompt_draw(LANG_IPODUI_NOPLAY_PROTECTED,
                           LANG_IPODUI_NOPLAY_PROTECTED_BODY);
        else
            ui_prompt_draw(LANG_IPODUI_VIDEO_ERR_FORMAT_HEAD,
                           LANG_IPODUI_NOPLAY_FORMAT);
        ui_seg_draw(noplay_ok, 1, 0, true);
    }
}

static enum ui_result noplay_input(struct ui_screen *s,
                                   const struct ui_input *in)
{
    (void)s;
    /* SELECT (OK) or MENU: back to the list. The rest is swallowed, as on
     * the video page: PLAY, NEXT and PREV must not act on the queue from
     * a page about a file that isn't in it. Hold MENU stays the shell's. */
    if ((in->key == UI_KEY_SELECT || in->key == UI_KEY_MENU)
        && in->release && !in->long_press)
    {
        UPDATE_TRACE("noplay page dismiss key=%s audio=%d",
                     in->key == UI_KEY_SELECT ? "select" : "menu",
                     audio_status());
        return UI_POP;
    }
    return UI_HANDLED;
}

static const struct ui_screen_ops noplay_screen = {
    .name    = "noplay",
    .enter   = noplay_enter,
    .leave   = NULL,
    .draw    = noplay_draw,
    .input   = noplay_input,
    .timeout = NULL,
    .tick    = NULL,
};

bool ui_noplay_show(int why, enum ui_noplay_kind kind)
{
    if (why <= UNPLAYABLE_NONE || why >= UNPLAYABLE_COUNT)
        return false;
#ifdef HAVE_BCMFW_VIDEO
    if (kind == UI_NOPLAY_VIDEO)
        return ui_video_show_why(why);  /* logs its own line */
#endif
    if ((unsigned)kind >= UI_NOPLAY_VIDEO)
        kind = UI_NOPLAY_SONG;      /* no player in this build: the plain
                                       page, never a video one */
    ui_log("list", "can't play why=%d kind=%d", why, (int)kind);
    return ui_push(&noplay_screen,
                   (const void *)(intptr_t)(why | ((int)kind << 8))) >= 0;
}

int ui_noplay_row(int view, int idx)
{
    int why = lib_row_unplayable(view, idx);
    int cls;
    enum ui_noplay_kind kind = UI_NOPLAY_SONG;

    if (!why)
        return 0;
    /* the entry's class, as the index stored it: a video before all (a
     * video podcast episode is a video), then an episode, then a book */
    cls = lib_row_class(view, idx);
    if (cls & TAGCACHE_UI_VIDEO)
        kind = UI_NOPLAY_VIDEO;
    else if (cls & TAGCACHE_UI_PODCAST)
        kind = UI_NOPLAY_EPISODE;
    else if (cls & TAGCACHE_UI_AUDIOBOOK)
        kind = UI_NOPLAY_BOOK;
    ui_noplay_show(why, kind);
    return why;
}
