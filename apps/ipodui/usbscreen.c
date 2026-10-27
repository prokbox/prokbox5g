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
 * iPod UI shell: the native USB screen (docs/phase8a-brief.md §3.2)
 *
 * What the 7G shows while a computer has the disk (owner photos 1165
 * and photo 1226): a dark header with the clock (or "iPod") centred, a
 * slate-to-navy body, a glossy orange disc carrying a connector glyph
 * while idle and two sync arrows turning while the host moves data, and
 * "Connected" / "Eject before disconnecting." under it.
 *
 * Compose once, then blit (lean rules 1 and 3). At entry, boosted, the
 * disc box (92 x 92 at (114, 53)) is rendered into the shell's strip:
 * the clean disc, the idle frame and six arrow frames. The arrows have
 * two-fold symmetry, so the frame for k * 30 degrees with k >= 6 is
 * bit-identical to k - 6: six stored frames give all 12 positions of
 * UI_USB_FRAMES (frozen default U2). Three of them are drawn from the
 * generator's half-planes (art/usb/usbscreen.py), the other three are
 * the same coverage maps turned 90 degrees by index remapping. The
 * header, body and text go straight into the framebuffer, then one full
 * push. While lit and the host moves data, every 1/10 s one frame's 92
 * rows of 184 B are copied in and one full-width 92-row band is pushed;
 * idle, nothing moves; dark, nothing is drawn.
 *
 * The pixel code above the USB_PURE_ONLY line depends on nothing but the
 * generated tables, so tests/usbscreen builds it on the host and checks
 * it against the generator's reference frames bit for bit.
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
#ifndef USB_PURE_ONLY
/* not ownable (file): a full-screen page or takeover; the drift panel was settled on the way in (ui_panel_hide(), or the takeover's own ui_fb_settle()), and a full lcd_update() releases it (bcmjob_owned_hook) */
#include "config.h"
#include "fat.h"                   /* #81: FSInfo */
#include <string.h>
#endif                                  /* else the includer has memcpy */
#include <stdbool.h>
#if defined(SIMULATOR) || defined(USB_PURE_ONLY)
#define USB_BOTH_GLYPHS                 /* "usb_glyph" pins either one */
#endif
#include "usbscreen_data.h"

typedef unsigned short usb_px;          /* RGB565, as fb_data on ipodvideo */

#define USB_N       USB_DISC_D
#define USB_BOX_X   (USB_DISC_CX - USB_DISC_D / 2)
#define USB_BOX_Y   (USB_DISC_CY - USB_DISC_D / 2)
#define USB_STORED  (USB_FRAMES / 2)    /* k and k + 6 are identical */

/* ------------------------------------------------------------ pixel code */

/* 8b-T (review M): the file is on the cold-Thumb list, but this pixel
 * code (the disc, the arrows, the 12-frame compose on every plug) stays
 * ARM, so the first USB frame costs what it did */
#ifdef __thumb__
#pragma GCC push_options
#pragma GCC target("arm")
#define USB_PIXEL_ARM
/* gcc inlines an ARM static into a Thumb caller: the entry points that
 * Thumb code calls stay out of line, so their loops stay ARM */
#define USB_ARM_ENTRY __attribute__((noinline))
#else
#define USB_ARM_ENTRY
#endif

static inline usb_px usb_blend(usb_px bg, usb_px fg, int a)
{
    unsigned r = (((fg >> 11) & 31) * a + ((bg >> 11) & 31) * (16 - a)) >> 4;
    unsigned g = (((fg >> 5) & 63) * a + ((bg >> 5) & 63) * (16 - a)) >> 4;
    unsigned b = ((fg & 31) * a + (bg & 31) * (16 - a)) >> 4;
    return (usb_px)(r << 11 | g << 5 | b);
}

static inline usb_px usb_darken(usb_px c, int a)   /* black over c, a/16 */
{
    unsigned k = 16 - a;
    return (usb_px)(((((c >> 11) & 31) * k) >> 4) << 11 |
                    ((((c >> 5) & 63) * k) >> 4) << 5 |
                    (((c & 31) * k) >> 4));
}

/* The body's two colours for row y (even and odd columns): the gradient
 * stops interpolated in 8.8 fixed point, then a 2 x 2 ordered dither to
 * RGB565 (usbscreen.py interp(), dither565()). */
static void usb_body_pair(int y, usb_px pair[2])
{
    static const unsigned char bayer[2][2] = { { 0, 2 }, { 3, 1 } };
    long v[3];
    int i, k, x;

    for (i = 0; i < USB_BODY_STOPS - 2; i++)
        if (y <= usb_body_stops[i + 1][0])
            break;
    for (k = 0; k < 3; k++)
    {
        long c0 = usb_body_stops[i][k + 1], c1 = usb_body_stops[i + 1][k + 1];
        long y0 = usb_body_stops[i][0], y1 = usb_body_stops[i + 1][0];
        v[k] = c0 * 256 + (c1 - c0) * 256 * (y - y0) / (y1 - y0);
    }
    for (x = 0; x < 2; x++)
    {
        long thr = (2 * bayer[y & 1][x] + 1) * 32;
        long r = (v[0] * 31 / 255 + thr) >> 8;
        long g = (v[1] * 63 / 255 + thr) >> 8;
        long b = (v[2] * 31 / 255 + thr) >> 8;
        pair[x] = (usb_px)((r > 31 ? 31 : r) << 11 | (g > 63 ? 63 : g) << 5 |
                           (b > 31 ? 31 : b));
    }
}

/* Row y of the body, w columns from an even x */
static void usb_body_row(usb_px *dst, int y, int w)
{
    usb_px pair[2];
    int x;

    usb_body_pair(y, pair);
    for (x = 0; x + 1 < w; x += 2)
    {
        dst[x] = pair[0];
        dst[x + 1] = pair[1];
    }
    if (x < w)
        dst[x] = pair[0];
}

/* The clean disc over the body into the 92 x 92 box (usbscreen.py
 * clean_disc()): doubled coordinates, integer edge coverage, colour by
 * y + (x - 46) >> 3 so the left side reads lighter. */
USB_ARM_ENTRY static void usb_draw_disc(usb_px *dst, int stride)
{
    int x, y;

    for (y = 0; y < USB_N; y++, dst += stride)
    {
        const int Y = 2 * y - (USB_N - 1);
        const usb_px *tab = usb_disc_tab + y - USB_DISC_IDX_MIN;

        usb_body_row(dst, USB_BOX_Y + y, USB_N);
        for (x = 0; x < USB_N; x++)
        {
            const int X = 2 * x - (USB_N - 1);
            const int d2 = X * X + Y * Y;
            usb_px c;

            if (d2 >= USB_R2_OUT)
                continue;
            c = tab[(x - USB_N / 2) >> 3];
            dst[x] = d2 <= USB_R2_IN ? c :
                usb_blend(dst[x], c, (USB_R2_OUT - d2) * 16
                                     / (USB_R2_OUT - USB_R2_IN));
        }
    }
}

static unsigned long usb_isqrt(unsigned long v)
{
    unsigned long r = 0, b = 1UL << 30;

    while (b > v)
        b >>= 2;
    while (b)
    {
        if (v >= r + b)
        {
            v -= r + b;
            r = (r >> 1) + b;
        }
        else
            r >>= 1;
        b >>= 2;
    }
    return r;
}

static inline int usb_clamp16(long v)
{
    return v < 0 ? 0 : (v > 16 ? 16 : (int)v);
}

static inline int usb_min(int a, int b)
{
    return a < b ? a : b;
}

/* radial coverage HW16 + 8 - |r16 - RM16| is 16 or 0 without a square
 * root when 64 * d2 lies in these ranges (exact, r16 = isqrt(64 * d2)) */
#define USB_FULL_LO ((USB_RM16 - USB_HW16 + 8L) * (USB_RM16 - USB_HW16 + 8L))
#define USB_FULL_HI ((USB_RM16 + USB_HW16 - 7L) * (USB_RM16 + USB_HW16 - 7L))
#define USB_ZERO_LO ((USB_RM16 - USB_HW16 - 7L) * (USB_RM16 - USB_HW16 - 7L))
#define USB_ZERO_HI ((USB_RM16 + USB_HW16 + 8L) * (USB_RM16 + USB_HW16 + 8L))

/* Coverage (0..16) of both arrows at base angle 'base' * 30 degrees into
 * a 92 x 92 byte map (usbscreen.py arrow_alpha()): the ring band by
 * distance, cut by the tail and head half-planes, plus the head
 * triangle; arrow 2 is arrow 1 through the centre. */
static void usb_arrow_alpha(int base, unsigned char *alpha)
{
    const long *p = usb_arrow_hp[base];
    int x, y, s;

    for (y = 0; y < USB_N; y++)
    {
        const long Y = 2 * y - (USB_N - 1);
        for (x = 0; x < USB_N; x++, alpha++)
        {
            const long X = 2 * x - (USB_N - 1);
            const long d2 = X * X + Y * Y;
            long r16, dev, s64;
            int radial, cov = 0;

            if (d2 < USB_RIN2 || d2 > USB_ROUT2)
            {
                *alpha = 0;
                continue;
            }
            s64 = d2 << 6;
            if (s64 >= USB_FULL_LO && s64 < USB_FULL_HI)
                radial = 16;            /* inside the stroke */
            else if (s64 < USB_ZERO_LO || s64 >= USB_ZERO_HI)
                radial = 0;             /* off the stroke */
            else                        /* the 1 px edges: by distance */
            {
                r16 = (long)usb_isqrt((unsigned long)s64);
                dev = r16 - USB_RM16;
                radial = usb_clamp16(USB_HW16 + 8 - (dev < 0 ? -dev : dev));
            }
            for (s = 1; s >= -1; s -= 2)
            {
                /* each term is skipped exactly where it is 0 */
                const long px = s * X, py = s * Y;
                const long dh = (p[2] * px + p[3] * py) >> USB_SH;

                if (radial && dh < 8)               /* the arc */
                {
                    const long dt = (p[0] * px + p[1] * py) >> USB_SH;
                    if (dt > -8)
                        cov += usb_min(radial, usb_min(usb_clamp16(dt + 8),
                                                       usb_clamp16(8 - dh)));
                }
                if (dh > -8)                        /* the head */
                {
                    const long e1 = (p[4] * px + p[5] * py + p[6]) >> USB_SH;
                    const long e2 = (p[7] * px + p[8] * py + p[9]) >> USB_SH;
                    long m = dh < e1 ? dh : e1;
                    cov += usb_clamp16((m < e2 ? m : e2) + 8);
                }
            }
            *alpha = (unsigned char)(cov > 16 ? 16 : cov);
        }
    }
}

/* One arrow frame: the clean disc with the coverage map turned 'rot'
 * quarter turns clockwise in black (usbscreen.py rotated(), ink_over()) */
static void usb_compose_frame(usb_px *dst, const usb_px *disc,
                              const unsigned char *alpha, int rot)
{
    const int n = USB_N - 1;
    int x, y;

    for (y = 0; y < USB_N; y++)
        for (x = 0; x < USB_N; x++, dst++, disc++)
        {
            int a;
            switch (rot)
            {
                case 0:  a = alpha[y * USB_N + x]; break;
                case 1:  a = alpha[(n - x) * USB_N + y]; break;
                case 2:  a = alpha[(n - y) * USB_N + n - x]; break;
                default: a = alpha[x * USB_N + n - y]; break;
            }
            *dst = a == 0 ? *disc : (a == 16 ? 0 : usb_darken(*disc, a));
        }
}

/* A run-length coded 4-bit glyph in black, centred in the box */
static void usb_glyph_over(usb_px *dst, int stride, const unsigned char *rle,
                           int w, int h)
{
    usb_px *row = dst + ((USB_N - h) / 2) * stride + (USB_N - w) / 2;
    int x = 0, y = 0;

    while (y < h)
    {
        int a = *rle >> 4, run = (*rle & 15) + 1;
        rle++;
        a += a >> 3;                    /* 0..15 -> 0..16 */
        while (run--)
        {
            if (a)
                row[x] = usb_darken(row[x], a);
            if (++x == w)
            {
                x = 0;
                y++;
                row += stride;
            }
        }
    }
}

USB_ARM_ENTRY static void usb_idle_glyph(usb_px *dst, int stride, int glyph)
{
#if defined(HAVE_IPOD_DOCK30) || defined(USB_BOTH_GLYPHS)
    if (glyph == 0)     /* UI_USB_GLYPH_DOCK30 */
    {
        usb_glyph_over(dst, stride, usb_glyph_dock30, USB_DOCK30_W,
                       USB_DOCK30_H);
        return;
    }
#endif
#if !defined(HAVE_IPOD_DOCK30) || defined(USB_BOTH_GLYPHS)
    usb_glyph_over(dst, stride, usb_glyph_usbc, USB_USBC_W, USB_USBC_H);
#endif
    (void)glyph;
}

/* Strip layout, in RGB565 pixels of one box each, then the maps */
#define USB_SLOT        (USB_N * USB_N)
#define USB_SLOT_DISC   0
#define USB_SLOT_IDLE   1
#define USB_SLOT_FRAME0 2
#define USB_SLOT_COUNT  (USB_SLOT_FRAME0 + USB_STORED)
#define USB_STRIP_BYTES (USB_SLOT_COUNT * USB_SLOT * 2 + USB_BASES * USB_SLOT)

/* Everything the strip holds, from scratch */
USB_ARM_ENTRY static void usb_compose_all(usb_px *strip, int glyph)
{
    unsigned char *maps = (unsigned char *)(strip + USB_SLOT_COUNT * USB_SLOT);
    usb_px *disc = strip + USB_SLOT_DISC * USB_SLOT;
    int k;

    usb_draw_disc(disc, USB_N);
    memcpy(strip + USB_SLOT_IDLE * USB_SLOT, disc, USB_SLOT * 2);
    usb_idle_glyph(strip + USB_SLOT_IDLE * USB_SLOT, USB_N, glyph);
    for (k = 0; k < USB_BASES; k++)
        usb_arrow_alpha(k, maps + k * USB_SLOT);
    for (k = 0; k < USB_STORED; k++)
        usb_compose_frame(strip + (USB_SLOT_FRAME0 + k) * USB_SLOT, disc,
                          maps + (k % USB_BASES) * USB_SLOT, k / USB_BASES);
}
#ifdef USB_PIXEL_ARM
#pragma GCC pop_options
#endif

#ifndef USB_PURE_ONLY
/* ------------------------------------------------------------ the screen */

#include <stdio.h>
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "font.h"
#include "button.h"
#include "bcmjob_pp.h"          /* P19: bcmjob_owner_event() */
#include "backlight.h"
#include "power.h"
#include "powermgmt.h"
#include "storage.h"
#include "settings.h"
#include "lang.h"
#include "ipodui/bootbatt.h"
#include "ipodui/anim.h"
#include "ipodui/theme.h"
#include "ipodui/header.h"
#include "ipodui/paint.h"
#include "tagcache.h"
#include "ipodui/uilog.h"
#include "ipodui/usbbusy.h"
#include "ipodui/usbscreen.h"
#include "ipodui/sysscreen.h"
#include "ipodui/alarm.h"
#include "ipodui/preview.h"     /* ui_fb_settle() (P19) */
#include "ipodui/sdk/sdk_int.h"     /* sdk_usb() */
#include "hang_watch.h"             /* hang_suspend(): the session */
#include "file.h"
#include "dir.h"
#if defined(HAVE_USB_STATS) && !defined(SIMULATOR)
#include "ata.h"
#include "usb.h"
#include "usb_core.h"
#include "usb_drv.h"
#include "usb_storage_stats.h"
#endif

#if USB_DISC_D != UI_USB_DISC_D || USB_DISC_CX != UI_USB_DISC_CX || \
    USB_DISC_CY != UI_USB_DISC_CY || USB_FRAMES != UI_USB_FRAMES
#error "usbscreen_data.h is out of step with usbscreen.h: rerun art/usb/usbscreen.py"
#endif
#if USB_HDR_H > 24 || USB_BOX_Y != UI_USB_BAND_Y || (USB_BOX_X & 1)
#error "usbscreen geometry"
#endif
#if USB_STRIP_BYTES > UI_STRIP_SIZE
#error "the USB frames do not fit the strip"
#endif

#define USB_TEXT1_BASE  187     /* "Connected", Bold 15 (photo: 186.5) */
#define USB_TEXT2_BASE  205     /* "Eject before disconnecting.", Reg 12 */
#define USB_TEXT_W      300

/* the text band: "Connected" (Bold 15) cap top to the second line's
 * descender, pushed alone when the computer ejects the disk (P11 E2) */
#define USB_TEXT_Y      (USB_TEXT1_BASE - 14)
#define USB_TEXT_H      (USB_TEXT2_BASE + 5 - USB_TEXT_Y)

static struct ui_usb_state st;          /* 4 B */
static bool usb_running;
static bool usb_ejected;                /* "OK to Disconnect" shows */
static unsigned usb_sessions;           /* ui_usb_sessions() */

unsigned ui_usb_sessions(void)
{
    return usb_sessions;
}

static void host_marker(const char *path)
{
    int fd;

    if (file_exists(path))
        return;
    fd = open(path, O_WRONLY | O_CREAT, 0666);
    if (fd >= 0)
        close(fd);
    ui_log("usb", "marker %s%s", path, fd >= 0 ? "" : " failed");
}

void ui_usb_host_markers(void)
{
    host_marker("/.metadata_never_index");
    if (!dir_exists("/.fseventsd"))
        mkdir("/.fseventsd");
    host_marker("/.fseventsd/no_log");
}

#ifdef SIMULATOR
/* uisimulator/common/sim_cmd_sys.c: the script command "usb_glyph"
 * pins the idle glyph: -1 auto (the build's UI_USB_IDLE_GLYPH), else
 * UI_USB_GLYPH_*. */
extern int sim_usb_glyph_pin;
extern long sim_disk_active_until;      /* sim_cmd_art.c "disk_active" */
/* The sim's variable models the disk staying awake (artcache.c keeps it
 * on for the 5 s spin-down after the art builder's reads), not the last
 * sector, so only what the host does during USB counts: a value set
 * after entry, by "disk_active <ms>" (the burst), plus the device's
 * 0.5 s hold after it. On the device nothing of ours touches the disk
 * during USB either (the art builder stops at SYS_USB_CONNECTED). */
static long sim_io_base;
#endif

static int usb_glyph_now(void)
{
#ifdef SIMULATOR
    int g = sim_usb_glyph_pin;
    if (g == UI_USB_GLYPH_DOCK30 || g == UI_USB_GLYPH_USBC)
        return g;
#endif
    return UI_USB_IDLE_GLYPH;
}

/* #58: the sustained-transfer detector (usbscreen.h), one per session */
static struct ui_usb_io usbio;          /* 16 B */

/* USB reliability: on the device the Connected screen is static. The
 * arrows' band pushes (and the header's) run on the main thread, which
 * takes the CPU whenever the USB and storage threads wait for the disk; a
 * push is tens of ms of LCD bridge work with no yield, and a DMA
 * completion that lands inside one is served late, which macOS answered
 * with a bus reset. Backlight and screen stay on; the only redraw is the
 * header (battery, clock), and never while the disk has been busy within
 * USB_QUIET_TICKS. The simulator keeps the animation (its scripts check it). */
#if defined(HAVE_USB_PREFETCH) && !defined(SIMULATOR)
#define USB_STATIC_SCREEN 1
#define USB_QUIET_TICKS (HZ * 2)
#endif

bool ui_usb_io_active(void)
{
    long now = current_tick, act;
    bool ok;
#ifdef SIMULATOR
    /* "disk_active <ms>" is a burst: the disk busy from the command until
     * then (the host reading or writing all the time) */
    ok = sim_disk_active_until && sim_disk_active_until != sim_io_base;
    act = ok && TIME_BEFORE(now, sim_disk_active_until)
          ? now : sim_disk_active_until;
#else
    act = storage_last_disk_activity();
    ok = act >= 0;
#endif
    return ui_usb_io_step(&usbio, now, act, ok);
}

bool ui_usb_screen_active(void)
{
    return usb_running;
}

static bool usb_powered(void)
{
#if CONFIG_CHARGING
    return charger_inserted();
#else
    return true;
#endif
}

static bool usb_charging(void)
{
#if CONFIG_CHARGING >= CHARGING_MONITOR
    return charging_state();
#else
    return false;
#endif
}

/* what the header shows; 'title' NULL when the shell's fonts are absent */
struct usb_hdr {
    const char *title;
    int pct;
    bool charging, powered;
};

static void usb_hdr_poll(struct usb_hdr *h, bool shell, int *wake)
{
    h->title = NULL;
    *wake = TIMEOUT_BLOCK;
    if (shell)
    {
        if (ui_header_time_in_title())
            h->title = ui_header_clock(wake);
        else
            h->title = (const char *)str(LANG_IPODUI_DEVICE_NAME);
    }
    h->pct = battery_level();
    h->charging = usb_charging();
    h->powered = usb_powered();
}

static bool usb_hdr_same(const struct usb_hdr *a, const struct usb_hdr *b,
                         unsigned long ta, unsigned long tb)
{
    return ta == tb && a->pct == b->pct && a->charging == b->charging &&
           a->powered == b->powered;
}

static unsigned long usb_text_hash(const char *s)
{
    unsigned long h = 5381;
    while (s && *s)
        h = h * 33 + (unsigned char)*s++;
    return h;
}

/* The text under the disc: "Connected" / "Eject before disconnecting.",
 * or, once the computer has ejected every LUN, "OK to Disconnect" alone
 * (P11 E2: USB mode stays until the cable is pulled). Over the body
 * gradient, which the caller has drawn (no push). */
static void usb_draw_text(void)
{
    struct viewport vp;

    memset(&vp, 0, sizeof(vp));
    vp.width = LCD_WIDTH;
    vp.height = LCD_HEIGHT;
    vp.flags = VP_DEFAULT_FLAGS;
    vp.drawmode = DRMODE_FG;
    vp.font = ui_theme->font_ui;
    lcd_set_viewport(&vp);
    lcd_set_foreground(LCD_WHITE);
    ui_paint_text_centre(LCD_WIDTH / 2, USB_TEXT1_BASE, USB_TEXT_W,
                         str(usb_ejected ? LANG_IPODUI_OK_TO_DISCONNECT
                                         : LANG_IPODUI_USB_CONNECTED));
    if (!usb_ejected)
    {
        vp.font = ui_theme->font_small;
        ui_paint_text_centre(LCD_WIDTH / 2, USB_TEXT2_BASE, USB_TEXT_W,
                             str(LANG_IPODUI_USB_EJECT_FIRST));
    }
    lcd_set_viewport(NULL);
}

/* Header, body and text straight into the framebuffer (no push) */
static void usb_draw_static(const struct usb_hdr *h, bool shell)
{
    int y;

    ui_header_draw_dark(usb_hdr_rows, USB_HDR_H, h->title, h->pct,
                        h->charging, h->powered);
    lcd_set_viewport(NULL);             /* FBADDR uses the current one */
    for (y = USB_HDR_H; y < LCD_HEIGHT; y++)
        usb_body_row(FBADDR(0, y), y, LCD_WIDTH);
    if (!shell)
        return;

    usb_draw_text();
}

/* Copies one disc box into the framebuffer at the disc position */
static void usb_blit_box(const usb_px *box)
{
    int y;

    lcd_set_viewport(NULL);
    for (y = 0; y < USB_N; y++)
        memcpy(FBADDR(USB_BOX_X, USB_BOX_Y + y), box + y * USB_N,
               USB_N * sizeof(usb_px));
}

/* The idle disc drawn directly (no strip, or the strip was taken) */
static void usb_draw_idle_direct(int glyph)
{
    lcd_set_viewport(NULL);
    usb_draw_disc(FBADDR(USB_BOX_X, USB_BOX_Y), LCD_WIDTH);
    usb_idle_glyph(FBADDR(USB_BOX_X, USB_BOX_Y), LCD_WIDTH, glyph);
}

/* Shows 'frame' (-1 idle) in the framebuffer box from the strip, or
 * the idle disc directly; false if the strip no longer holds our art */
static bool usb_show(int frame, int glyph)
{
    bool kept = false;
    usb_px *strip;

    if (!st.frames_valid)
    {
        usb_draw_idle_direct(glyph);
        return frame < 0;
    }
    strip = ui_strip_claim(UI_STRIP_OWNER_USB, &kept);
    if (!strip || !kept)
    {
        if (strip)
            ui_strip_put();
        st.frames_valid = false;
        usb_draw_idle_direct(glyph);
        return frame < 0;
    }
    usb_blit_box(strip + (frame < 0 ? USB_SLOT_IDLE
                                    : USB_SLOT_FRAME0 + frame) * USB_SLOT);
    ui_strip_put();
    return true;
}

static void usb_check_fonts(void)
{
    const int ids[3] = { ui_theme->font_ui, ui_theme->font_title,
                         ui_theme->font_small };
    int i;

    for (i = 0; i < 3; i++)
    {
        /* runs after font_disable_all() (usb_screen.c), which closes a
         * disk-cached font's fd and sets 'disabled': its glyphs now come
         * from the RAM cache alone, and a miss draws a placeholder */
        struct font *pf = font_get(ids[i]);
        if (pf && pf->disabled)
            ui_log("usb", "font %d cache-only (disk font)", ids[i]);
    }
}

#if defined(HAVE_USB_STATS) && !defined(SIMULATOR)
/* #63: what the disk negotiated, and where the session's time went. Log
 * lines hold 62 characters, so the counters take four short lines. */
static void usb_log_ata(void)
{
#if (CONFIG_STORAGE & STORAGE_ATA) && defined(HAVE_ATA_DMA)
    const unsigned short *id = ata_get_identify();
    struct ata_flush_stats fs;

    ata_flush_stats_get(&fs);
    ui_log("usb", "ata mode=0x%02x ssd=%d w88=%04x w93=%04x w76=%04x",
           ata_get_dma_mode(), ata_disk_isssd(), id[88], id[93], id[76]);
    /* #91 H2: write cache on (w85 bit 5), FLUSH CACHE (EXT) enabled
     * (w86 bits 12-13), the write-order barriers so far, failed (M2) and
     * not offered by the drive (nosup: no barrier held), and their cost */
    ui_log("usb", "ata w85=%04x w86=%04x flush n=%lu fail=%lu nosup=%lu",
           id[85], id[86], fs.n, fs.fail, fs.nosup);
    ui_log("usb", "ata flush max=%luus avg=%luus", fs.max_us,
           fs.n ? fs.total_us / fs.n : 0);
#endif
}

static unsigned long usb_ms(uint64_t us)
{
    return (unsigned long)(us / 1000);
}

/* USB resets: the controller's interrupts (usb_drv.h). irq: interrupts
 * by USBSTS bit; resets taken / only acknowledged (usb-arc-reset.h),
 * SET_ADDRESS applied (one per re-enumeration) and
 * USBMODE; the gaps between resets and the bulk dTDs retired with an
 * error (halted/buffer/transaction); then the first 8 resets, raw */
static void usb_log_irq(void)
{
    struct usb_drv_stats d;
    uint32_t i, n;

    usb_drv_get_stats(&d);
    ui_log("usb", "irq uri=%lu pci=%lu sli=%lu uei=%lu",
           (unsigned long)d.uri, (unsigned long)d.pci,
           (unsigned long)d.sli, (unsigned long)d.uei);
    ui_log("usb", "irq rstreal=%lu rstfake=%lu addr=%lu mode=%lx",
           (unsigned long)d.rst_real, (unsigned long)d.rst_fake,
           (unsigned long)d.addr_set, (unsigned long)d.usbmode);
    ui_log("usb", "irq gap<.1s=%lu <1s=%lu <10s=%lu more=%lu td=%lu/%lu/%lu",
           (unsigned long)d.gaps[0], (unsigned long)d.gaps[1],
           (unsigned long)d.gaps[2], (unsigned long)d.gaps[3],
           (unsigned long)d.td_halt, (unsigned long)d.td_buf,
           (unsigned long)d.td_txn);
    /* controller fixes: completions deferred
     * (dTD still active) / bit already set at a prime, primes timed out /
     * of a primed endpoint, flush retries/gave up, endpoints reset after
     * a dTD error, resets seen during a reset */
    ui_log("usb", "irq lost=%lu/%lu ptmo=%lu pmiss=%lu/%lu pstale=%lu",
           (unsigned long)d.lost_skip, (unsigned long)d.lost_stale,
           (unsigned long)d.prime_to, (unsigned long)d.prime_miss,
           (unsigned long)d.prime_retry, (unsigned long)d.prime_stale);
    ui_log("usb", "irq flush=%lu/%lu tdrst=%lu dblrst=%lu",
           (unsigned long)d.flush_retry, (unsigned long)d.flush_fail,
           (unsigned long)d.td_err_reset, (unsigned long)d.rst_dbl);
    n = d.ev_n < USB_DRV_RST_EVENTS ? d.ev_n : USB_DRV_RST_EVENTS;
    for (i = 0; i < n; i++) {
        const struct usb_drv_rst_event *e = &d.ev[i];
        if (i == 2 || i == 4 || i == 6)
            ui_log_flush();         /* the ring holds 12 lines */
        ui_log("usb", "rbits %lu s%04x p%02x a%02x k%d e%02x t%02x g%lu i%lu",
               (unsigned long)i, e->sts, e->portsc, e->addr, e->kind,
               e->prime, e->in_td, (unsigned long)e->gap_ms,
               (unsigned long)e->in_ms);
        /* pending prime, completion bits unserved, the primed IN
         * endpoint's overlay status and remaining 512s */
        ui_log("usb", "rbx %lu pr%02x cm%02x ov%02x ol%u b%03x nk%02x fr%u o%lu",
               (unsigned long)i, e->pend, e->cmpl, e->ov, e->ovlen,
               (unsigned)e->in_off, (unsigned)e->nak, (unsigned)e->fr_d,
               (unsigned long)e->out_ms);
    }
    ui_log_flush();
}

static void usb_log_stats(long secs)
{
    struct usb_storage_stats u;
    uint32_t mount_i, mount_first;

    usb_storage_get_stats(&u);
    /* Repeat this at exit. The entry line can be displaced by early boot
     * logging, but the policy is essential when comparing mount rounds. */
    {
        extern const char *usb_storage_switches(void);
        ui_log("usb", "sw %s", usb_storage_switches());
    }
    ui_log("usb", "exit s=%ld ejected=%d rd=%lu wr=%lu", secs, usb_ejected,
           (unsigned long)(u.rd_bytes >> 20), (unsigned long)(u.wr_bytes >> 20));
    ui_log("usb", "cmd rd=%lu wr=%lu calls=%lu ra=%lu/%lu hs=%d/%d c=%lu flip=%d",
           (unsigned long)u.rd_cmds, (unsigned long)u.wr_cmds,
           (unsigned long)u.stor_calls, (unsigned long)u.ra_hits,
           (unsigned long)u.ra_reads, u.high_speed_first, u.high_speed,
           (unsigned long)u.cfg_count, u.high_speed_changes);
    ui_log("usb", "ms busy=%lu stor=%lu usb=%lu host=%lu", usb_ms(u.busy_us),
           usb_ms(u.stor_us), usb_ms(u.usb_us), usb_ms(u.host_us));
    ui_log("usb", "ata dma=%lu/%lu pio=%lu/%lu rst=%lu",
           (unsigned long)u.dma_rd, (unsigned long)u.dma_wr,
           (unsigned long)u.pio_rd, (unsigned long)u.pio_wr,
           (unsigned long)u.ata_resets);
    /* #81 per-command latency: storage reads <= 16 KiB (count, mean us)
     * and larger ones (count, mean us, KB), and the CPU MHz seen */
    ui_log("usb", "stor small=%lu/%lu big=%lu/%lu/%lu mhz=%u-%u",
           (unsigned long)u.rd_small_n,
           (unsigned long)(u.rd_small_n ? u.rd_small_us / u.rd_small_n : 0),
           (unsigned long)u.rd_big_n,
           (unsigned long)(u.rd_big_n ? u.rd_big_us / u.rd_big_n : 0),
           (unsigned long)u.rd_big_kb,
           (unsigned)u.rd_mhz_min, (unsigned)u.rd_mhz_max);
    ui_log_flush();                 /* the ring holds 12 lines: keep exit */
    /* #81: the prefetch ring (ring=0: the read-ahead ran instead), and
     * the mount: SET_CONFIGURATION to the first READ, from it to the
     * first READ past the first FAT (0: never), KB sent in its first 30 s */
    ui_log("usb", "pf ring=%lu hit=%lu pend=%lu miss=%lu fb=%lu",
           (unsigned long)u.pf_ring_kb, (unsigned long)u.pf_hits,
           (unsigned long)u.pf_pend, (unsigned long)u.pf_miss,
           (unsigned long)u.pf_fallback);
    ui_log("usb", "pf fill=%lu mb=%lu stall=%lu/%lu wrstor=%lu",
           (unsigned long)u.pf_fills, (unsigned long)(u.pf_kb >> 10),
           (unsigned long)(u.pf_stall_max_us / 1000), usb_ms(u.pf_stall_us),
           usb_ms(u.wr_stor_us));
    ui_log("usb", "mnt cfg2tur=%lu cfg2inq=%lu inq2data=%lu",
           (unsigned long)u.cfg_to_tur_ms,
           (unsigned long)u.cfg_to_inq_ms,
           (unsigned long)u.inq_to_data_ms);
    ui_log("usb", "mnt cfg2rd=%lu rd2fat=%lu rd30=%lu",
           (unsigned long)u.cfg_to_rd_ms, (unsigned long)u.rd_to_fat_ms,
           (unsigned long)u.rd30_kb);
    ui_log_flush();
    if (u.rd18_ms)
        ui_log("usb", "mnt t%lu rd=%lu n=%lu at=%08lx/%lu",
               (unsigned long)u.rd18_ms, (unsigned long)u.rd18_kb,
               (unsigned long)u.mount_read18_n,
               (unsigned long)u.mount_read18_lba,
               (unsigned long)u.mount_read18_blocks);
    for (mount_i = 0; mount_i < USB_STATS_MOUNT_READS &&
         mount_i < u.mount_read_n; mount_i++)
        ui_log("usb", "mnt first %lu=%08lx/%lu", (unsigned long)mount_i,
               (unsigned long)u.mount_first_lba[mount_i],
               (unsigned long)u.mount_first_blocks[mount_i]);
    ui_log_flush();
    mount_first = u.mount_read_n > USB_STATS_MOUNT_READS ?
            u.mount_read_n - USB_STATS_MOUNT_READS : 0;
    for (mount_i = mount_first; mount_i < u.mount_read_n; mount_i++)
    {
        uint32_t slot = mount_i % USB_STATS_MOUNT_READS;
        ui_log("usb", "mnt last %lu=%08lx/%lu", (unsigned long)mount_i,
               (unsigned long)u.mount_last_lba[slot],
               (unsigned long)u.mount_last_blocks[slot]);
    }
    ui_log_flush();                 /* the ring holds 12 lines */
    /* USB reliability: the other CBWs by class; invalid CBWs, stale
     * completions ignored, failed WRITEs; the resets (bulk: Bulk-Only
     * Mass Storage Reset, down: configuration torn down, bus: bus reset
     * interrupts, defer: SETUPs held behind one) with the last 8 as
     * b<state>/d<state> (usb_storage.c's state numbers), and
     * bus_reset_pending and exclusive storage as this line finds them;
     * the longest single stalls of the USB thread, its storage calls and
     * the prefetch wait (ms) */
    struct usb_core_stats c;
    char rlog[USB_STATS_RST_LOG * 4 + 1];
    uint32_t i, first = u.rst_n > USB_STATS_RST_LOG ?
                        u.rst_n - USB_STATS_RST_LOG : 0;
    int n = 0;

    usb_core_get_stats(&c);
    rlog[0] = '\0';
    for (i = first; i < u.rst_n; i++) {
        uint8_t e = u.rst_log[i % USB_STATS_RST_LOG];
        n += snprintf(rlog + n, sizeof(rlog) - n, "%s%c%d", n ? "," : "",
                      (e >> 4) == USB_STATS_RST_BULK ? 'b' : 'd', e & 15);
    }
    ui_log("usb", "bot tur=%lu inq=%lu oth=%lu bad=%lu ign=%lu wrfail=%lu",
           (unsigned long)u.tur_cmds, (unsigned long)u.inq_cmds,
           (unsigned long)u.other_cmds, (unsigned long)u.bad_cbws,
           (unsigned long)u.ignored, (unsigned long)u.wr_fails);
    ui_log("usb", "rst bulk=%lu down=%lu bus=%lu defer=%lu pend=%d excl=%d",
           (unsigned long)u.bulk_resets,
           (unsigned long)u.cfg_downs, (unsigned long)c.bus_resets,
           (unsigned long)c.setups_deferred, c.pending_now ? 1 : 0,
           usb_exclusive_storage() ? 1 : 0);
    /* its own line: the ring's 61 characters would cut it off (review #2) */
    ui_log("usb", "rlog %s", n ? rlog : "-");
    ui_log("usb", "blk busy=%lu stor=%lu pf=%lu",
           (unsigned long)(u.busy_max_us / 1000),
           (unsigned long)(u.stor_max_us / 1000),
           (unsigned long)(u.pf_stall_max_us / 1000));
    ui_log_flush();
    usb_log_irq();
}
#endif

void ui_usb_screen_loop(bool early)
{
    const bool shell = ui_shell_started();
    struct usb_hdr hdr, cur;
    unsigned long hdr_hash;
    int glyph = usb_glyph_now();
    int boost = 0, frames = 0, wake_hdr;
    long next_frame = current_tick;
    bool lit_drawn = true;
    const long entered = current_tick;
    /* owner (#63): software power-offs are refused while connected
     * (powermgmt.c); log the first one of the session, and the total */
    const unsigned int blocked0 = powermgmt_poweroff_blocked();
    bool blocked_logged = false;

    (void)early;
    ui_fb_settle(0, 0, LCD_WIDTH, LCD_HEIGHT);  /* P19 screen truth: a takeover */
    hang_suspend(HANG_SUSP_USB, true);  /* the host owns the disk: no hang
                                         * watch until the session ends */
    usb_running = true;
    sdk_usb(true);          /* P12: a game suspends (after the ack; no
                             * write until the session ends) */
    usb_sessions++;
    usb_ejected = false;
#ifdef SIMULATOR
    sim_io_base = sim_disk_active_until;
#endif
    ui_usb_io_init(&usbio, current_tick);
    backlight_on();
    st.frame = -1;
#ifdef USB_STATIC_SCREEN
    st.io = false;
#else
    st.io = ui_usb_io_active();
#endif
    st.static_valid = false;
    st.frames_valid = false;
    UPDATE_TRACE("usb enter glyph=%s shell=%d",
                 glyph == UI_USB_GLYPH_DOCK30 ? "dock30" : "usbc", shell);
    UPDATE_TRACE("usb io=%d", st.io);
    ui_log("usb", "enter n=%u", usb_sessions);  /* #63: drops on the device */
#ifndef SIMULATOR
    {   /* #81: the FSInfo this session's host finds */
        unsigned long fc = 0, nf = 0, cl = 0;
        if (fat_get_fsinfo(IF_MV(0,) &fc, &nf, &cl))
            ui_log("fat", "fsinfo free=%lu next=0x%lx cl=%lu", fc, nf, cl);
        {   /* #81: the dirty flags (a dirty volume makes macOS run its
             * full repair check before mounting) */
            unsigned int bf = 0;
            unsigned long f1 = 0;
            if (fat_get_dirty(IF_MV(0,) &bf, &f1))
                ui_log("fat", "dirty bs41=%02x fat1=%08lx clean=%d herr=%d",
                       bf, f1, (int)((f1 >> 27) & 1), (int)(!((f1 >> 26) & 1)));
        }
    }
#endif
#if defined(HAVE_USB_STATS) && !defined(SIMULATOR)
    usb_storage_reset_stats();      /* one session, however many configs */
    usb_log_ata();
    {
        /* bus_reset_pending as the last usb_core_exit() found it: set
         * there means the last session ended with a reset outstanding */
        struct usb_core_stats c;
        usb_core_get_stats(&c);
        ui_log("usb", "prev pendexit=%d", c.pending_at_exit ? 1 : 0);
    }
    {   /* the developer USB switches in force (usb_storage.c) */
        extern const char *usb_storage_switches(void);
        ui_log("usb", "sw %s", usb_storage_switches());
    }
#ifndef SIMULATOR
    {   /* #81 bus experiment 1: the bus registers usb_drv_init() read
         * back (usb_drv.h), for the IDE/USB DMA overlap question */
        struct usb_drv_stats d;         /* as usb_log_irq() does */
        usb_drv_get_stats(&d);
        ui_log("usb", "regs sbus=%lx bst=%lx txf=%lx",
               (unsigned long)d.bus[0], (unsigned long)d.bus[1],
               (unsigned long)d.bus[2]);
        ui_log("usb", "regs cp=%lx dt=%lx xmb=%lx",
               (unsigned long)d.bus[3], (unsigned long)d.bus[4],
               (unsigned long)d.bus[5]);
    }
#endif
#endif
    if (shell)
        usb_check_fonts();

    /* the entry compose, boosted (Rev 1 L3: usb_storage boosts during
     * transfers anyway; this one makes the first frame quick even when
     * no transfer runs) */
    cpu_boost(true);
    boost++;
    if (shell)
    {
        bool kept;
        usb_px *strip = ui_strip_claim(UI_STRIP_OWNER_USB, &kept);
        if (strip)
        {
            usb_compose_all(strip, glyph);
            ui_strip_put();
            st.frames_valid = true;
        }
    }
    usb_hdr_poll(&hdr, shell, &wake_hdr);
    hdr_hash = usb_text_hash(hdr.title);
    usb_draw_static(&hdr, shell);
    if (st.io && st.frames_valid)
        st.frame = usb_show(0, glyph) ? 0 : -1;
    else
        usb_show(-1, glyph);
    st.static_valid = true;
    ui_lcd_push("usb:static", 0, 0, LCD_WIDTH, LCD_HEIGHT);
    cpu_boost(false);
    boost--;
    next_frame = current_tick + HZ / UI_USB_FPS;

    while (1)
    {
        const bool lit = is_backlight_on(true);
#ifdef USB_STATIC_SCREEN
        const bool io = false;      /* no arrows: nothing drawn per frame */
        const long act = storage_last_disk_activity();
        const bool quiet = act < 0 ||
                           !TIME_BEFORE(current_tick, act + USB_QUIET_TICKS);
#else
        const bool io = ui_usb_io_active();
#endif
        int tmo = TIMEOUT_BLOCK, g;
        long btn, now;

        if (io != st.io)
        {
            st.io = io;
            UPDATE_TRACE("usb io=%d", io);
            next_frame = current_tick;
        }
        /* P11 E2: the computer ejected the disk. Polled on the loop's
         * existing wakes (UI_USB_IO_HOLD while idle, lit or dark): one
         * text band. Never cleared: after an eject the host needs a
         * re-plug to mount again. */
        if (shell && !usb_ejected && usb_storage_all_ejected())
        {
            int y;

            usb_ejected = true;
            UPDATE_TRACE("usb ejected%s", "");
            lcd_set_viewport(NULL);         /* FBADDR uses the current one */
            for (y = USB_TEXT_Y; y < USB_TEXT_Y + USB_TEXT_H; y++)
                usb_body_row(FBADDR(0, y), y, LCD_WIDTH);
            usb_draw_text();
            /* full: the p8a-usb rule allows full or disc-band pushes;
             * once per session */
            ui_lcd_push("usb:static", 0, 0, LCD_WIDTH, LCD_HEIGHT);
        }
        if (lit)
        {
            if (!lit_drawn)
            {
                lit_drawn = true;
                next_frame = current_tick;
            }
            now = current_tick;
            g = usb_glyph_now();
            if (g != glyph)
            {
                glyph = g;
                if (st.frames_valid)
                {
                    bool kept;
                    usb_px *strip = ui_strip_claim(UI_STRIP_OWNER_USB, &kept);
                    if (strip && kept)
                    {
                        memcpy(strip + USB_SLOT_IDLE * USB_SLOT,
                               strip + USB_SLOT_DISC * USB_SLOT,
                               USB_SLOT * 2);
                        usb_idle_glyph(strip + USB_SLOT_IDLE * USB_SLOT,
                                       USB_N, glyph);
                    }
                    else
                        st.frames_valid = false;
                    if (strip)
                        ui_strip_put();
                }
                st.frame = -2;          /* force the idle redraw */
            }
            if (io && st.frames_valid)
            {
                if (!TIME_BEFORE(now, next_frame))
                {
                    int f = st.frame < 0 ? 0 : (st.frame + 1) % USB_STORED;
                    if (usb_show(f, glyph))
                    {
                        st.frame = f;
                        frames++;
                        ui_lcd_push("usb:frame", 0, UI_USB_BAND_Y,
                                    LCD_WIDTH, UI_USB_BAND_H);
                    }
                    else
                    {
                        st.frame = -1;          /* strip lost: idle art */
                        ui_lcd_push("usb:static", 0, UI_USB_BAND_Y, LCD_WIDTH,
                                    UI_USB_BAND_H);
                    }
                    /* drop frames rather than lag */
                    next_frame += HZ / UI_USB_FPS;
                    if (TIME_BEFORE(next_frame, now))
                        next_frame = now + HZ / UI_USB_FPS;
                }
                if (st.frames_valid)
                    tmo = MAX(next_frame - now, 1);
            }
            if (st.frame != -1 && !(io && st.frames_valid))
            {
                usb_show(-1, glyph);
                st.frame = -1;
                ui_lcd_push("usb:static", 0, UI_USB_BAND_Y, LCD_WIDTH,
                            UI_USB_BAND_H);
            }
            if (tmo == TIMEOUT_BLOCK)   /* notice the host starting, and
                                           sample a burst (#58) */
                tmo = ui_usb_io_wake(&usbio, current_tick);

#ifdef USB_STATIC_SCREEN
            if (!quiet)
            {
                /* the disk is busy: leave the screen alone, look again soon */
                tmo = tmo == TIMEOUT_BLOCK ? HZ : MIN(tmo, HZ);
                wake_hdr = TIMEOUT_BLOCK;
            }
            else
#endif
            {
                usb_hdr_poll(&cur, shell, &wake_hdr);
                if (!usb_hdr_same(&cur, &hdr, usb_text_hash(cur.title),
                                  hdr_hash))
                {
                    hdr = cur;
                    hdr_hash = usb_text_hash(hdr.title);
                    ui_header_draw_dark(usb_hdr_rows, USB_HDR_H, hdr.title,
                                        hdr.pct, hdr.charging, hdr.powered);
                    ui_lcd_push("usb:header", 0, 0, LCD_WIDTH, USB_HDR_H);
                }
            }
            if (wake_hdr != TIMEOUT_BLOCK)
                tmo = MIN(tmo, wake_hdr);
        }
        else
        {
            /* dark: draw nothing, but poll at the I/O hold rate, so a
             * backlight that comes on without a key (entry, charger,
             * the host) redraws the arrows and clock within 0.5 s */
            lit_drawn = false;
            tmo = ui_usb_io_wake(&usbio, current_tick);
        }

        {   /* P12 Q7: an alarm rings here as the piezo only, any button
             * stops it (alarm.c); its deadline joins the wait */
            long at = alarm_timeout();
            if (at != TIMEOUT_BLOCK && (tmo == TIMEOUT_BLOCK || at < tmo))
                tmo = (int)at;
        }
        btn = button_get_w_tmo(tmo);
        bcmjob_owner_event(btn);    /* P19: a decode's pass (port only) */
        alarm_poll_usb(btn);
        if (!blocked_logged && powermgmt_poweroff_blocked() != blocked0)
        {
            blocked_logged = true;
            ui_log("usb", "poweroff blocked");
        }
        if (btn == SYS_USB_DISCONNECTED)
            break;
        if (btn == SYS_CHARGER_DISCONNECTED)
            reset_runtime();
        /* any other button or event: re-evaluate (keys do nothing here;
         * HID is ignored, Rev 1 L4) */
    }

    hang_suspend(HANG_SUSP_USB, false); /* the exit's writes are watched */
    UPDATE_TRACE("usb exit frames=%d boost=%d", frames, boost);
    /* 8b brief 3.3: the release, to place the library's empty window
     * ("lib window", ui_main.c) against it; misc.c's system_restore_db()
     * posts the post-USB scan right after we return */
#ifdef HAVE_TAGCACHE
    ui_log("usb", "release tick=%ld scan_pending=%d", current_tick,
           tagcache_is_initialized() ? 1 : 0);
#else
    ui_log("usb", "release tick=%ld scan_pending=0", current_tick);
#endif
    ui_usbbusy_begin();                 /* #84: the glyph while the scan runs */
    if (powermgmt_poweroff_blocked() != blocked0)
        ui_log("usb", "poweroff blocked n=%u",
               powermgmt_poweroff_blocked() - blocked0);
#if defined(HAVE_USB_STATS) && !defined(SIMULATOR)
    usb_log_stats((current_tick - entered) / HZ);
#else
    ui_log("usb", "exit s=%ld ejected=%d", (current_tick - entered) / HZ,
           usb_ejected);
#endif
#if defined(HAVE_USB_STATS) && !defined(SIMULATOR)
    {   /* per SCSI opcode: FAIL CSWs / stalls / resets right after it */
        extern bool usb_storage_cmdstat(char *buf, int size);
        char cs[100];
        if (usb_storage_cmdstat(cs, sizeof(cs)))
            ui_log("usb", "cmdstat op:fail/stall/rst %s", cs);
    }
    ui_log_flush();                     /* the ring holds 12 lines */
    {   /* #81: the storage handover, in ms from this screen's entry
         * (negative: before it); the host's first command waits for it */
        extern void usb_storage_hold_info(unsigned int *n, long *max,
                                          long *now);
        struct usb_handover_info h;
        unsigned int held;
        long hmax, hnow;
        usb_get_handover_info(&h);
        usb_storage_hold_info(&held, &hmax, &hnow);
#define HAND_MS(t) ((t) ? ((t) - entered) * (1000 / HZ) : -1L)
        ui_log("usb", "hand n=%d acks=%d pend=%d late=%d req=%ld ack=%ld "
               "ok=%ld held=%u holdmax=%ld holdnow=%ld", h.n, h.acks,
               h.pending, h.late, HAND_MS(h.req), HAND_MS(h.last_ack),
               HAND_MS(h.ok), held, hmax * (1000 / HZ), hnow * (1000 / HZ));
        {   /* One compact record: the last ack is the only one that can
             * hold up the host, and preserves the reset records in the
             * small device log ring. */
            int i = MIN(h.ack_n, USB_HANDOVER_ACKERS) - 1;
            struct thread_debug_info ti;
            if (i < 0) {
                ui_log("usb", "hand last -");
            } else {
                if (thread_get_debug_info(h.acker[i], &ti) <= 0)
                    strcpy(ti.name, "?");
                ti.name[10] = 0;
                ui_log("usb", "hand last %s@%ld n=%d", ti.name,
                       HAND_MS(h.acker_at[i]), h.ack_n);
            }
        }
#undef HAND_MS
    }
#endif
    /* the session's lines go to the file now, while the disk is ours again:
     * the art and library passes that follow log enough to push them out
     * of the 12-line ring before the next idle flush (log dropped=N) */
    ui_log_flush();
#ifndef SIMULATOR
    /* #91 L: the rename record back if the host removed it */
    rbfs_rename_intent_usb_exit();
#endif
    bootbatt_usb_exit();    /* the drained latch: USB was power */
    (void)frames;                       /* trace only (no-op on target) */
    (void)boost;
    usb_running = false;
    sdk_usb(false);
}

/* -- P11: the dark chrome of the pushed dark screens (U2b) -------------- */

void ui_dark_body(int y, int h)
{
    int r, end = MIN(y + h, LCD_HEIGHT);

    lcd_set_viewport(NULL);             /* FBADDR uses the current one */
    /* the gradient's stops start below the header: rows above it (a
     * screen without a header) repeat its first row */
    for (r = MAX(y, 0); r < end; r++)
        usb_body_row(FBADDR(0, r), MAX(r, USB_HDR_H), LCD_WIDTH);
}

static struct {
    unsigned long title_hash;
    int pct;
    bool charging, powered, valid;
} dark_last;                            /* 12 B */

bool ui_dark_header(const char *title, bool force)
{
    unsigned long th = usb_text_hash(title);
    int pct = battery_level();
    bool charging = usb_charging(), powered = usb_powered();

    if (!force && dark_last.valid && dark_last.title_hash == th &&
        dark_last.pct == pct && dark_last.charging == charging &&
        dark_last.powered == powered)
        return false;
    ui_header_draw_dark(usb_hdr_rows, USB_HDR_H, title, pct, charging,
                        powered);
    dark_last.title_hash = th;
    dark_last.pct = pct;
    dark_last.charging = charging;
    dark_last.powered = powered;
    dark_last.valid = true;
    return true;
}
#endif /* !USB_PURE_ONLY */
