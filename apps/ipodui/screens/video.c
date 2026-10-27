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
 * iPod UI shell: video screen (Phase 14a)
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

/* The video screen (docs/phase14a-brief.md 3.7; the player's rules are
 * docs/research/bcm2722/shell-contract.md 1-2). Compiled only with
 * HAVE_BCMFW_VIDEO (apps/SOURCES).
 *
 * Start: vplayer_start(path) FIRST (its core_alloc would shrink the audio
 * buffer if taken after), then the audio as a one-item playlist at the
 * resume position, then this screen.
 *
 * Failure (owner, 2026-10-13, overriding CLAUDE.md rule 1's "logged, not
 * shown" for this case): a video that can't play fails entirely. The
 * audio stops (or never starts), the queue's resume point is dropped so
 * no Now Playing row points at it, and this screen turns into the light
 * prompt of New Software and Reset Settings (setwidgets.h) with one of
 * four messages and a single OK; SELECT or MENU goes back to the list.
 * The raw reason is logged ("failed why=%d", or the open probe's code).
 * A refusal at vplayer_start() is classified by vplayer_info()'s open_why
 * (dev32) where the player has one, and by a cold probe of the file's
 * boxes and avcC (verr_probe()) for the stream's own faults.
 *
 * While the picture plays this screen draws nothing, pushes nothing, takes
 * no boost and reads no disk: the BCM owns the panel, the LCD driver's
 * pushes are suspended and the player sets the backlight itself. The tick
 * (every HZ/10) only polls vplayer_info() and the audio status.
 *
 *   PLAY          audio_pause() / audio_resume(); the player follows.
 *   strip         slides up from the bottom when the first picture comes,
 *                 on PLAY, a chapter tap, a held NEXT/PREV and a finger
 *                 set on the wheel (the driver's touch count, polled by
 *                 the tick); a turn then takes it for the volume (owner,
 *                 2026-10-15). Its times follow once a second.
 *   wheel         volume (np_pb_volume_step), with the volume bar in the
 *                 strip, playing or paused (2 s after the last turn).
 *   SELECT        the bottom-strip overlay (progress, elapsed,
 *                 remaining, chapter ticks); a second SELECT puts the wheel
 *                 on the scrubber, a third gives it back to the volume.
 *                 Playing, it fades 4 s after the last key (the 7G's
 *                 overlay); paused, it stays.
 *   PREV / NEXT   the chapter before / after through playback's table
 *                 (ab_chapter_skip_target); no chapters: PREV restarts,
 *                 NEXT ends the video. Held: rw / ff on the shell's curve
 *                 (ui_seek_span_ms), the strip's diamond running; the
 *                 release lands it.
 *   MENU          vplayer_stop(), audio_stop() (the resume point is kept
 *                 by the track-finish handler), back to the list.
 *
 * The overlay: while the video owns the panel, lcd_update() reaches
 * nothing, so the strip is drawn into the frame and handed to the image
 * with vplayer_overlay(), which keeps it over every picture until it is
 * taken away (w 0); the picture underneath comes back by itself. State is the small static below;
 * nothing is allocated after the audio starts. */

/* not ownable (file): a full-screen page or takeover; the drift panel was settled on the way in (ui_panel_hide(), or the takeover's own ui_fb_settle()), and a full lcd_update() releases it (bcmjob_owned_hook) */
#include "config.h"
#include <stdio.h>
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "button.h"
#include "lcd.h"
#include "font.h"
#include "file.h"
#include "lang.h"
#include "settings.h"
#include "sound.h"
#include "audio.h"
#include "playback.h"
#include "playlist.h"
#include "metadata.h"
#include "tagcache.h"
#include "chapters.h"
#include "audiobook.h"
#include "video/vplayer.h"
#include "video/vpcore.h"
#include "../../../bcmfw/hostproto.h"
#include "ipodui/uilog.h"
#include "ipodui/anim.h"
#include "ipodui/theme.h"
#include "ipodui/paint.h"
#include "ipodui/header.h"
#include "ipodui/ipodui.h"
#include "ipodui/np_scrub.h"
#include "ipodui/preview.h"     /* ui_fb_settle */
#include "ipodui/np_draw.h"
#include "ipodui/np_playback.h"
#include "ipodui/screens/nowplaying.h"
#include "ipodui/screens/setwidgets.h"
#include "ipodui/screens/video.h"

/* the overlay strip, full width at the bottom */
#define STRIP_H     44
#define STRIP_Y     (LCD_HEIGHT - STRIP_H)
#define BAR_X       56
#define BAR_W       (LCD_WIDTH - 2 * BAR_X)
#define BAR_H       8
#define BAR_Y       (STRIP_Y + 18)

#define VOL_SHOW    (2 * HZ)    /* the volume bar's life after a click */
#define OV_SHOW     (4 * HZ)    /* playing: the overlay's life after a key */
#define START_GRACE (4 * HZ)    /* the audio may not report PLAY at once */
#define SCRUB_REST  (HZ / 6)    /* the wheel at rest: seek */
#define SLIDE_MS    220         /* the strip slides up / down this long */

static const struct ui_screen_ops video_ops;
static void video_end(void);

enum strip { ST_NONE, ST_VOLUME, ST_OVERLAY, ST_SCRUB };

static struct {
    bool owned;                 /* vplayer_start() succeeded, not yet stopped */
    bool seen_play;             /* the audio reported PLAY once */
    bool paused;                /* the paused flag the strip is for */
    unsigned char strip;        /* enum strip */
    bool seek_due;              /* scrubbed, seek when the wheel rests */
    long start;                 /* current_tick at start */
    long vol_until;             /* ST_VOLUME hides (the player redraws) */
    long ov_until;              /* playing: ST_OVERLAY/ST_SCRUB hide */
    long seek_at;               /* the last scrub click */
    unsigned long scrub_ms;     /* scrubber position */
    unsigned char err;          /* enum verr: the failure prompt shows */
    unsigned int touches;       /* button_wheel_touches() last seen */
    bool shown_start;           /* the strip came up with the first picture */
    bool slide_up;              /* the next strip up slides in */
    signed char hold_dir;       /* held NEXT / PREV: +1 / -1, 0 none */
    long hold_start;            /* tick of the hold */
    unsigned long hold_from;    /* elapsed ms at the hold */
    long shown_sec;             /* the second the strip's times show */
    int shown_px;               /* the bar's fill the strip shows */
#ifdef SIMULATOR
    short sim_fail;             /* tests: a fail_why to fake, 0 none */
#endif
} vs;

/* -- the failure prompt (cold) -------------------------------------------- */

enum verr { VE_NONE, VE_FORMAT, VE_LARGE, VE_DAMAGED, VE_OTHER, VE_CHECK,
            VE_COUNT };

static const short verr_lang[VE_COUNT][2] = {
    [VE_FORMAT]  = { LANG_IPODUI_VIDEO_ERR_FORMAT_HEAD,
                     LANG_IPODUI_VIDEO_ERR_FORMAT_BODY },
    [VE_LARGE]   = { LANG_IPODUI_VIDEO_ERR_LARGE_HEAD,
                     LANG_IPODUI_VIDEO_ERR_LARGE_BODY },
    [VE_DAMAGED] = { LANG_IPODUI_VIDEO_ERR_DAMAGED_HEAD,
                     LANG_IPODUI_VIDEO_ERR_DAMAGED_BODY },
    [VE_OTHER]   = { LANG_IPODUI_VIDEO_ERR_OTHER_HEAD,
                     LANG_IPODUI_VIDEO_ERR_OTHER_BODY },
    /* the unit's first-use check passed at no clock (vcheck.h): the
       report carries the reason (owner, 2026-10-26) */
    [VE_CHECK]   = { LANG_IPODUI_VIDEO_ERR_CHECK_HEAD,
                     LANG_IPODUI_VIDEO_ERR_CHECK_BODY },
};
static const char *const verr_name[VE_COUNT] = {
    "none", "format", "large", "damaged", "other", "check"
};
static const short verr_ok[1] = { LANG_IPODUI_OK };

/* The player's fail_why (a BVP_E_* from bcmvp.h, < 0, or the image's
 * HP_E_* status) as one of the four messages */
static int verr_of(int why)
{
    switch (why)
    {
        case HP_E_UNSUPPORTED:          /* codec, profile, size or mode */
            return VE_FORMAT;
        case HP_E_NOMEM:
        case HP_E_TOOBIG:
            return VE_LARGE;
        case HP_E_FIFO:
        case HP_E_CORRUPT:
            return VE_DAMAGED;
#ifdef BVP_E_STILL
        case BVP_E_STILL:               /* dev33: no picture after 3
                                         * restarts: not the format */
            return VE_OTHER;
#endif
#ifdef BVP_E_CHECK
        case BVP_E_CHECK:               /* the first-use check failed */
            return VE_CHECK;
#endif
        default:                        /* port, timeout, protocol, ... */
            return VE_OTHER;
    }
}

static long probe_read(void *ctx, uint64_t off, void *buf, unsigned long len)
{
    int fd = *(int *)ctx;

    if (lseek(fd, (off_t)off, SEEK_SET) != (off_t)off)
        return -1;
    return read(fd, buf, len);
}

/* vplayer_start() refused path and says only false: what it would have
 * met, from the same checks (mp4_open(), vpcore.c load_params()). A few
 * small box reads, once, on the failure path; nothing is kept. *rc gets
 * the demuxer's code (logged). */
static int verr_probe(const char *path, int *rc)
{
    struct mp4_io io;
    struct mp4_movie m;
    uint8_t head[5];            /* avcC: version, profile, constraints, .. */
    int fd = open(path, O_RDONLY), e;

    *rc = 0;
    if (fd < 0)
        return VE_OTHER;
    io.read_at = probe_read;
    io.ctx = &fd;
    io.size = (uint64_t)ffilesize(fd);
    *rc = mp4_open(&m, &io);
    if (*rc == MP4_E_FRAGMENTED || *rc == MP4_E_NO_AVC
        || *rc == MP4_E_EMPTY_EDIT)
        e = VE_FORMAT;
    else if (*rc == MP4_E_SPACE)
        e = VE_OTHER;
    else if (*rc < 0 || !m.avcc.end
             || probe_read(&fd, m.avcc.pos, head, 5) != 5)
        e = VE_DAMAGED;                 /* broken boxes, short file */
    else if ((head[4] & 3) != 3 || !vpc_baseline_ok(head[1], head[2]))
        e = VE_FORMAT;                  /* not H.264 Baseline */
    else
        e = VE_OTHER;                   /* no image, no memory, ... */
    close(fd);
    return e;
}

/* vplayer_start() returned false: its reason (vplayer_info()'s open_why,
 * dev32) where the player has one, the probe for the rest. An index or
 * table refusal is probed: a stream that isn't H.264 Baseline (High
 * profile, fragmented, no AVC track) says "format", anything else
 * "damaged". *why gets open_why (0: none), *rc the code logged. */
static int verr_open(const char *path, int *rc, int *why)
{
#ifdef VPLAYER_OPEN_FILE
    struct vplayer_info vi;

    vplayer_info(&vi);
    *why = vi.open_why;
    *rc = vi.open_err;
    switch (vi.open_why)
    {
        case VPLAYER_OPEN_FILE:
            return VE_DAMAGED;
        case VPLAYER_OPEN_MEMORY:
            return VE_LARGE;
        case VPLAYER_OPEN_IMAGE:
            return VE_OTHER;
        case VPLAYER_OPEN_INDEX:
        case VPLAYER_OPEN_DAMAGED:
        {
            int prc;

            if (verr_probe(path, &prc) == VE_FORMAT)
                return VE_FORMAT;
            if (!*rc)
                *rc = prc;
            return VE_DAMAGED;
        }
        default:
            break;
    }
#else
    *why = 0;
#endif
    return verr_probe(path, rc);
}

static void verr_page(struct ui_screen *s)
{
    s->title = str(LANG_IPODUI_VIDEO_ERR_TITLE);
    s->header_w = 0;                    /* the light header, full width */
    vs.strip = ST_NONE;
    UPDATE_TRACE("video error page=%s audio=%d", verr_name[vs.err],
                 audio_status());
}

/* From the tick: the playing video failed. Everything stops, the queue
 * is forgotten, and this screen becomes the prompt (a push slide). */
static void verr_show(struct ui_screen *s, int e)
{
    video_end();
    audio_stop();
    playlist_update_resume_info(NULL);  /* no Now Playing row for it */
    vs.err = e;
    verr_page(s);
    ui_restage();
}

/* -- the first-use check (cold) ------------------------------------------ */

/* The first play on a unit checks the video chip first (vplayer.h
 * vplayer_check_due(), about a second, once per unit and video image):
 * "Preparing Video..." on the video's black, in the list font, centred.
 * The player keeps this frame over its test pictures (vcheck.h: a
 * full-panel overlay) until the film's first picture replaces it. */
static void preparing_draw(void)
{
    const char *s = str(LANG_IPODUI_VIDEO_PREPARING);
    int w = 0, h = 0;

    ui_fb_settle(0, 0, LCD_WIDTH, LCD_HEIGHT);  /* P19 screen truth */
    lcd_set_viewport(NULL);
    lcd_set_background(LCD_BLACK);
    lcd_clear_display();
    lcd_setfont(ui_theme->font_ui);
    lcd_set_drawmode(DRMODE_FG);
    lcd_set_foreground(LCD_RGBPACK(235, 235, 240));
    font_getstringsize((const unsigned char *)s, &w, &h, ui_theme->font_ui);
    lcd_putsxy((LCD_WIDTH - w) / 2, (LCD_HEIGHT - h) / 2, s);
    lcd_set_drawmode(DRMODE_SOLID);
    lcd_update();
    UPDATE_TRACE("video preparing due=%d", 1);
}

/* -- the strip ------------------------------------------------ */

static void strip_dirty(void)
{
    ui_invalidate_band(STRIP_Y, STRIP_H);
}

static void put_text(int x, int y_base, int align, const char *s)
{
    int w = 0, h = 0;

    font_getstringsize((const unsigned char *)s, &w, &h, ui_theme->font_title);
    lcd_putsxy(align < 0 ? x - w : x, y_base - h, s);
}

static void draw_bar(int fill_px, const struct audiobook_chapters *ab,
                     unsigned long length)
{
    int i;

    lcd_set_foreground(LCD_RGBPACK(70, 70, 74));            /* track */
    lcd_fillrect(BAR_X, BAR_Y, BAR_W, BAR_H);
    if (fill_px > 0)
        lcd_gradient_fillrect(BAR_X, BAR_Y, fill_px, BAR_H,
                              LCD_RGBPACK(120, 170, 245),
                              LCD_RGBPACK(40, 90, 200));
    if (ab && length)
    {
        lcd_set_foreground(LCD_RGBPACK(200, 200, 205));
        for (i = 0; i < ab->chapter_count; i++)
        {
            unsigned long ms = ab_ch_start(ab, i);
            if (ms > 0 && ms < length)
                lcd_vline(BAR_X + (int)((uint64_t)ms * BAR_W / length),
                          BAR_Y, BAR_Y + BAR_H - 1);
        }
    }
}

/* an npglyphs glyph into the frame, centred on the bar's middle row */
static void put_glyph(enum np_glyph_id id, int x, int align, np_px fg,
                      np_px edge)
{
    struct np_canvas cv;
    int w, h;

    cv.px = FBADDR(0, 0);
    cv.stride = (int)(FBADDR(0, 1) - FBADDR(0, 0));
    cv.w = LCD_WIDTH;
    cv.h = LCD_HEIGHT;
    np_glyph_size(id, &w, &h);
    np_glyph_draw(&cv, id, align < 0 ? x - w : align == 0 ? x - w / 2 : x,
                  BAR_Y + (BAR_H - h) / 2, NULL, fg, edge);
}

static void draw_strip_pixels(void)
{
    struct mp3entry *id3 = audio_current_track();
    unsigned long length = id3 ? id3->length : 0;
    unsigned long pos = vs.strip == ST_SCRUB ? vs.scrub_ms
                                             : np_pb_elapsed_ms();
    const struct audiobook_chapters *ab = NULL;
    char buf[16];
    int px;

    lcd_set_viewport(NULL);
    lcd_gradient_fillrect(0, STRIP_Y, LCD_WIDTH, STRIP_H,
                          LCD_RGBPACK(58, 58, 62), LCD_RGBPACK(12, 12, 14));
    lcd_setfont(ui_theme->font_title);
    lcd_set_drawmode(DRMODE_FG);
    lcd_set_foreground(LCD_RGBPACK(235, 235, 240));
    if (vs.strip == ST_VOLUME)
    {
        /* the speakers either side say it is the volume (the 7G's) */
        np_px fg = LCD_RGBPACK(235, 235, 240);

        draw_bar(np_pb_volume_pct() * BAR_W / 100, NULL, 0);
        put_glyph(NPG_SPEAKER, BAR_X - 8, -1, fg, fg);
        put_glyph(NPG_SPEAKER_WAVES, BAR_X + BAR_W + 6, 1, fg, fg);
        return;
    }
    if (id3 && id3->audiobook && id3->audiobook->chapter_count > 0)
        ab = id3->audiobook;
    pos = MIN(pos, length ? length : pos);
    vs.shown_sec = (long)(pos / 1000);
    px = length ? (int)((uint64_t)pos * BAR_W / length) : 0;
    vs.shown_px = px;
    draw_bar(px, ab, length);
    if (vs.strip == ST_SCRUB)           /* the scrubber's diamond */
        put_glyph(NPG_DIAMOND, BAR_X + px, 0, LCD_RGBPACK(235, 235, 240),
                  LCD_RGBPACK(0x14, 0x44, 0x96));
    lcd_set_foreground(LCD_RGBPACK(235, 235, 240));
    np_fmt_time(buf, pos, false);
    put_text(BAR_X - 6, BAR_Y + BAR_H + 2, -1, buf);
    np_fmt_time(buf, length > pos ? length - pos : 0, true);
    put_text(BAR_X + BAR_W + 6, BAR_Y + BAR_H + 2, 1, buf);
}

/* The strip into the frame, then up over the video */
static void draw_strip(void)
{
    draw_strip_pixels();
    if (vs.owned
        && vplayer_overlay_slide(0, STRIP_Y, LCD_WIDTH, STRIP_H,
                                 vs.slide_up ? HP_OVL_SLIDE_UP | SLIDE_MS
                                             : 0) < 0)
        ui_log("video", "overlay up failed");
    vs.slide_up = false;
}

/* The strip slides up from the bottom when it comes and down when it goes
 * (owner, 2026-10-15), by the image's own clock (HP_OVL_SLIDE_*): the
 * band's pixels go over once, the image steps it smoothly and puts the
 * picture back under the rows it leaves, playing or paused, at no cost
 * here. An image without slides shows and hides it at once. */
static void strip_show(int st)
{
    if (vs.strip == ST_NONE)
        vs.slide_up = true;
    vs.strip = st;
    strip_dirty();
}

static bool strip_up(void)
{
    return vs.strip != ST_NONE;
}

/* -- control ------------------------------------------------------------ */

static void video_end(void)
{
    if (vs.owned)
    {
        vplayer_stop();
        vs.owned = false;
    }
    if (vs.strip != ST_NONE)
    {
        /* the strip lives in the shell's own frame too (the image only
         * showed it): wiped once the player is gone, so the way back to
         * the list never carries a stale bar (owner, 2026-10-15: "the
         * visuals disappear, but the overlay sticks around") */
        lcd_set_viewport(NULL);
        lcd_set_background(LCD_BLACK);
        lcd_set_drawmode(DRMODE_SOLID | DRMODE_INVERSEVID);
        lcd_fillrect(0, STRIP_Y, LCD_WIDTH, STRIP_H);
        lcd_set_drawmode(DRMODE_SOLID);
        vs.strip = ST_NONE;
    }
}

/* Chapter skip over playback's table. 1: done; 0: nothing to skip to (no
 * table, or NEXT from the last chapter) */
static int chapter_skip(int dir)
{
    struct mp3entry *id3 = audio_current_track();
    struct audiobook_chapters *ab = id3 ? id3->audiobook : NULL;
    uint32_t seq = np_pb_seq();
    unsigned long ms;
    int c;

    if (!ab || ab->chapter_count <= 0 || (seq & 1))
        return 0;
    c = ab_chapter_skip_target(ab, dir, id3->elapsed);
    if (c < 0)
        return 0;
    ms = ab_ch_start(ab, c);
    if (np_pb_seq() != seq)
        return 0;                       /* playback rewrote the table */
    audio_pre_ff_rewind();
    audio_ff_rewind(ms);
    vplayer_seek(ms);
    return 1;
}

static void skip_key(int dir)
{
    if (chapter_skip(dir))
        return;
    if (dir < 0)
    {
        audio_pre_ff_rewind();
        audio_ff_rewind(0);             /* no chapters: restart */
        vplayer_seek(0);
    }
    else
    {
        /* the end: audio_next() does nothing on the last track, so run
         * the audio out. The finish handler then marks it played (a
         * natural end resets the resume point) and the tick leaves. */
        struct mp3entry *id3 = audio_current_track();
        unsigned long end = id3 && id3->length > 300 ? id3->length - 300 : 0;

        audio_pre_ff_rewind();
        audio_ff_rewind(end);
        vplayer_seek(end);
    }
}

/* hides the strip: the image puts the picture back */
static void strip_hide(void)
{
    vs.seek_due = false;
    if (vs.owned && vs.strip != ST_NONE)
        vplayer_overlay_slide(0, STRIP_Y, LCD_WIDTH, STRIP_H,
                              HP_OVL_SLIDE_DOWN | SLIDE_MS);
    vs.strip = ST_NONE;
    vs.slide_up = false;
}

/* a held NEXT / PREV: the audio waits (audio_pre_ff_rewind(), as the
 * shell's hold does) while the strip's diamond runs along the bar on the
 * shell's curve; the release lands it (owner, 2026-10-15) */
static unsigned long hold_pos(void)
{
    struct mp3entry *id3 = audio_current_track();
    unsigned long len = id3 ? id3->length : 0;
    unsigned long d = ui_seek_span_ms(current_tick - vs.hold_start);

    if (vs.hold_dir > 0)
    {
        unsigned long end = len > 1000 ? len - 1000 : 0;
        return MIN(vs.hold_from + d, end);
    }
    return vs.hold_from > d ? vs.hold_from - d : 0;
}

static void hold_land(void)
{
    unsigned long pos = hold_pos();

    UPDATE_TRACE("video hold %s to %lu ms", vs.hold_dir > 0 ? "fwd" : "back",
                 pos);
    vs.hold_dir = 0;
    audio_ff_rewind(pos);
    vplayer_seek(pos);
    vs.ov_until = current_tick + OV_SHOW;
    strip_show(ST_OVERLAY);
}

static void scrub_apply(void)
{
    vs.seek_due = false;
    if (np_pb_seek(vs.scrub_ms, np_pb_seq()))
        vplayer_seek(vs.scrub_ms);
}

/* -- start --------------------------------------------------------------- */

/* tc_idx is the 0-based tagcache master index (lib_id()), -1 if unknown;
 * NOT the 1-based id3->tagcache_idx / hold-menu value. */
void ui_video_play(const char *path, int tc_idx)
{
    unsigned long elapsed = 0, offset = 0;
    bool ok;

    if (!path || !path[0])
        return;
    if (vs.owned)
        video_end();
    if (tc_idx >= 0 && global_settings.ipodui_video_start == 0)
    {
        long e = tagcache_ui_numeric(tc_idx, tag_lastelapsed);
        long o = tagcache_ui_numeric(tc_idx, tag_lastoffset);

        elapsed = e > 0 ? (unsigned long)e : 0;
        offset = (e > 0 && o > 0) ? (unsigned long)o : 0;
    }
    /* the player's block and its index build must not run under other
     * audio (a core_alloc while playing shrinks the buffer: a gap, a
     * restart and disk reads in the middle of a song); the stop keeps that
     * track's resume point (the track-finish handler) */
    if (audio_status())
        audio_stop();
    if (vplayer_check_due())
        preparing_draw();                   /* the check runs in the start */
    ok = vplayer_start(path);               /* first: contract 1 */
    memset(&vs, 0, sizeof(vs));
    if (!ok)
    {
        int rc, why;

        audio_buffer_regrow();              /* what the refused block took */
        vs.err = verr_open(path, &rc, &why);
        ui_log("video", "failed why=open%d rc=%d err=%s", why, rc,
               verr_name[vs.err]);
        if (ui_push(&video_ops, NULL) < 0)  /* the prompt; nothing plays */
            vs.err = VE_NONE;
        return;
    }
    playlist_create(NULL, NULL);
    playlist_insert_track(NULL, path, PLAYLIST_INSERT_LAST, false, true);
    playlist_start(0, elapsed, offset);
    vs.owned = true;
    vs.start = current_tick;
#ifdef SIMULATOR
    {
        /* tests (ipodui-p14a-error-*): /.rockbox/.sim_vfail<why> fakes
         * the player failing with that fail_why at its first poll */
        static const signed char why[] = { HP_E_UNSUPPORTED, HP_E_NOMEM,
                                           HP_E_CORRUPT, BVP_E_TIMEOUT };
        char f[48];
        unsigned i;

        for (i = 0; i < sizeof(why); i++)
        {
            snprintf(f, sizeof(f), ROCKBOX_DIR "/.sim_vfail%d", why[i]);
            if (file_exists(f))
                vs.sim_fail = why[i];
        }
    }
#endif
    if (elapsed)
        vplayer_seek(elapsed);
    if (ui_push(&video_ops, NULL) < 0)
    {
        video_end();                        /* fail entirely: no picture, */
        audio_stop();                       /* no sound */
        playlist_update_resume_info(NULL);
    }
}

/* -- the screen ---------------------------------------------------------- */

static int video_enter(struct ui_screen *s, const void *arg)
{
    (void)arg;
    if (vs.err)
    {
        verr_page(s);                   /* refused at start */
        return 0;
    }
    if (!vs.owned)
        return -1;
    s->title = "";
    s->header_w = UI_HEADER_DARK;       /* no clock cycle; drawn nowhere */
    vs.strip = ST_NONE;
    vs.slide_up = false;
    vs.hold_dir = 0;
    vs.shown_start = false;
    vs.touches = button_wheel_touches();
    return 0;
}

static void video_leave(struct ui_screen *s)
{
    (void)s;
    if (vs.hold_dir)
        hold_land();
    video_end();                        /* hold MENU, a failure, the end */
    vs.err = VE_NONE;
}

static void video_draw(struct ui_screen *s, unsigned dirty)
{
    (void)s;
    if (vs.err)
    {
        if (dirty & UI_DIRTY_BODY)
        {
            ui_prompt_draw(verr_lang[vs.err][0], verr_lang[vs.err][1]);
            ui_seg_draw(verr_ok, 1, 0, true);
        }
        return;
    }
    lcd_set_viewport(NULL);
    if ((dirty & UI_DIRTY_BODY) && vs.owned && !vs.paused)
    {
        /* the BCM owns the panel: only the strip, over the picture */
        if (vs.strip != ST_NONE)
            draw_strip();
        return;
    }
    if (dirty & UI_DIRTY_BODY)
    {
        lcd_set_background(LCD_BLACK);
        lcd_clear_display();            /* held until the player stops */
        if (vs.strip != ST_NONE && vs.paused)
            draw_strip();
    }
    else if ((dirty & UI_DIRTY_BAND) && vs.strip != ST_NONE)
        draw_strip();
}

static enum ui_result video_input(struct ui_screen *s,
                                  const struct ui_input *in)
{
    (void)s;
    if (vs.err)
    {
        /* SELECT (OK) or MENU: back to the list. Every other key is
         * swallowed: PLAY must not resume the stopped queue, NEXT/PREV
         * must not skip in it. Hold MENU stays the shell's. */
        if ((in->key == UI_KEY_SELECT || in->key == UI_KEY_MENU)
            && in->release && !in->long_press)
        {
            UPDATE_TRACE("video error dismiss key=%s",
                         in->key == UI_KEY_SELECT ? "select" : "menu");
            return UI_POP;
        }
        return UI_HANDLED;
    }
    if (vs.hold_dir && in->key != UI_KEY_NEXT && in->key != UI_KEY_PREV)
        hold_land();                    /* the release never came */
    switch (in->key)
    {
        case UI_KEY_MENU:
            if (!in->release || in->long_press)
                return UI_HANDLED;      /* hold MENU: the shell's */
            video_end();
            audio_stop();               /* the resume point is kept */
            return UI_POP;
        case UI_KEY_PLAY:
            if (!in->release || in->long_press)
                return UI_HANDLED;
            if (audio_status() & AUDIO_STATUS_PAUSE)
            {
                audio_resume();
                vs.paused = false;
                vs.seek_due = false;
            }
            else
            {
                audio_pause();
                vs.paused = true;
            }
            vs.ov_until = current_tick + OV_SHOW;
            if (!strip_up() || vs.strip == ST_VOLUME)
                strip_show(ST_OVERLAY);  /* stop and start show it */
            return UI_HANDLED;
        case UI_KEY_SELECT:
            if (!in->release || in->long_press)
                return UI_HANDLED;
            vs.ov_until = current_tick + OV_SHOW;
            if (!strip_up() || vs.strip == ST_VOLUME)
                strip_show(ST_OVERLAY);
            else if (vs.strip == ST_OVERLAY)
            {
                vs.scrub_ms = np_pb_elapsed_ms();
                strip_show(ST_SCRUB);
            }
            else
            {
                scrub_apply();
                strip_show(ST_OVERLAY);
            }
            return UI_HANDLED;
        case UI_KEY_WHEEL:
            if (vs.hold_dir)
                return UI_HANDLED;
            if (vs.strip == ST_SCRUB && strip_up())
            {
                struct mp3entry *id3 = audio_current_track();
                unsigned long len = id3 ? id3->length : 0;
                unsigned long step = np_scrub_step_ms(in->wheel_velocity);
                long d = (long)step * in->wheel_steps;
                long p = (long)vs.scrub_ms + d;

                vs.scrub_ms = p < 0 ? 0 : (len && (unsigned long)p > len
                                           ? len : (unsigned long)p);
                vs.seek_due = true;
                vs.seek_at = current_tick;
                vs.ov_until = current_tick + OV_SHOW;
                strip_dirty();
            }
            else
            {
                np_pb_volume_step(in->wheel_steps);
                vs.vol_until = current_tick + VOL_SHOW;
                strip_show(ST_VOLUME);  /* playing or paused (the 7G) */
            }
            return UI_HANDLED;
        case UI_KEY_NEXT:
        case UI_KEY_PREV:
        {
            int dir = in->key == UI_KEY_NEXT ? 1 : -1;

            if (in->release)
            {
                if (vs.hold_dir)
                    hold_land();
                else if (!in->long_press)
                {
                    skip_key(dir);      /* a tap: the chapter */
                    vs.ov_until = current_tick + OV_SHOW;
                    strip_show(ST_OVERLAY);
                }
                return UI_HANDLED;
            }
            if (in->long_press && !vs.hold_dir && audio_status())
            {
                /* the hold: the strip with its diamond running */
                vs.hold_dir = dir;
                vs.hold_start = current_tick;
                vs.hold_from = np_pb_elapsed_ms();
                vs.scrub_ms = vs.hold_from;
                vs.seek_due = false;
                audio_pre_ff_rewind();
                strip_show(ST_SCRUB);
                UPDATE_TRACE("video hold %s from %lu ms",
                             dir > 0 ? "fwd" : "back", vs.hold_from);
            }
            return UI_HANDLED;
        }
        default:
            return UI_IGNORED;
    }
}

/* The poll: no I/O, no drawing while the picture plays */
static int video_tick(struct ui_screen *s)
{
    struct vplayer_info vi;
    int st;

    memset(&vi, 0, sizeof(vi));
    if (vs.err)
        return TIMEOUT_BLOCK;           /* the prompt waits for a key */
    st = audio_status();
    ui_nowplaying_note_input();         /* no auto-return over the picture */
    if (!vs.owned)
    {
        ui_pop_request(s);              /* back on top after a failure */
        return TIMEOUT_BLOCK;
    }
    if (st & AUDIO_STATUS_PLAY)
        vs.seen_play = true;
    vs.paused = (st & AUDIO_STATUS_PAUSE) != 0;
    if (vplayer_info(&vi))
    {
#ifdef SIMULATOR
        if (vs.sim_fail)
        {
            vi.state = VPC_FAILED;
            vi.fail_why = vs.sim_fail;
        }
#endif
        if (vi.state == VPC_FAILED)
        {
            ui_log("video", "failed why=%d", vi.fail_why);
            verr_show(s, verr_of(vi.fail_why));
            return TIMEOUT_BLOCK;
        }
    }
    if (!(st & AUDIO_STATUS_PLAY)
        && (vs.seen_play || TIME_AFTER(current_tick, vs.start + START_GRACE)))
    {
        video_end();                    /* the track ended */
        ui_pop_request(s);
        return TIMEOUT_BLOCK;
    }
    if (!vs.shown_start && vi.posted > 0)
    {
        vs.shown_start = true;          /* the first picture: it starts */
        vs.ov_until = current_tick + OV_SHOW;
        if (!strip_up())
            strip_show(ST_OVERLAY);
    }
    if (button_wheel_touches() != vs.touches)
    {
        vs.touches = button_wheel_touches();
        if (!strip_up() || vs.strip != ST_VOLUME)
        {
            if (!strip_up())
            {
                UPDATE_TRACE("video touch: overlay up (%u)", vs.touches);
                strip_show(ST_OVERLAY);
            }
            vs.ov_until = current_tick + OV_SHOW;
        }
    }
    if (vs.hold_dir)
    {
        struct mp3entry *id3 = audio_current_track();
        unsigned long len = id3 ? id3->length : 0;

        vs.scrub_ms = hold_pos();
        vs.ov_until = current_tick + OV_SHOW;
        /* a new frame only when the diamond or the times move */
        if ((long)(vs.scrub_ms / 1000) != vs.shown_sec
            || (len && (int)((uint64_t)vs.scrub_ms * BAR_W / len)
                       != vs.shown_px))
            strip_dirty();
    }
    else if (strip_up() && vs.strip == ST_OVERLAY
             && (long)(np_pb_elapsed_ms() / 1000) != vs.shown_sec)
        strip_dirty();                  /* the times follow, once a second */
    if (vs.seek_due && TIME_AFTER(current_tick, vs.seek_at + SCRUB_REST))
        scrub_apply();
    if (strip_up() && vs.strip == ST_VOLUME
        && TIME_AFTER(current_tick, vs.vol_until))
        strip_hide();
    if (strip_up() && (vs.strip == ST_OVERLAY || vs.strip == ST_SCRUB)
        && !vs.paused && !vs.seek_due && !vs.hold_dir
        && TIME_AFTER(current_tick, vs.ov_until))
        strip_hide();                   /* playing: the 7G's overlay fades */
    return HZ / 10;
}

static int video_timeout(struct ui_screen *s)
{
    (void)s;
    return vs.err ? TIMEOUT_BLOCK : HZ / 10;
}

static const struct ui_screen_ops video_ops = {
    .name    = "video",
    .enter   = video_enter,
    .leave   = video_leave,
    .draw    = video_draw,
    .input   = video_input,
    .timeout = video_timeout,
    .tick    = video_tick,
};
