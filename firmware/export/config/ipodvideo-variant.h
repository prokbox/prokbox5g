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

/* Phase 9: per-unit build variant (CLAUDE.md rule 7) and the shell's trims.
 * tools/configure passes IPOD_* / HAVE_IPOD_*; unset ones take the owner's
 * unit's value below. These defaults reproduce the pre-P9 ipodvideo.h
 * exactly, so the stock build stays byte-identical (checklist 11). */
#ifndef IPODVIDEO_VARIANT_H
#define IPODVIDEO_VARIANT_H

/* --storage=flash|hdd   -> IPOD_STORAGE_HDD when hdd                   */
/* --ram=64|32           -> configure memory= (MEMORYSIZE)              */
/* --battery=<mAh>|stock -> IPOD_BATTERY_MAH (0 = stock cell, 400/600)  */
#ifndef IPOD_BATTERY_MAH
#define IPOD_BATTERY_MAH 3000
#endif
/* --dock30 -> HAVE_IPOD_DOCK30 (8a). --haptics -> HAVE_IPOD_HAPTICS.   */
/* IPOD_VARIANT: written to autoconf.h by tools/configure for --ipodui
 * firmware builds (storage-ram-battery-dock-haptics), else this default. */
#ifndef IPOD_VARIANT
#define IPOD_VARIANT "flash-64-3000-usbc-haptic"
#endif

#ifdef HAVE_IPOD_UI
/* One line per reversible cut. Deleting a line brings the feature back;
 * the build must still pass once the line's dependency closure (below,
 * and scripts/trim-deps.txt) is deleted with it (scripts/trim-revert.sh).
 * Never set without --ipodui. tools/configure derives no_utf8proc and
 * plugins="" from IPODUI_TRIM_UTF8PROC / IPODUI_TRIM_PLUGINS (cpp -dM).
 * The set below is the shipped default for every --ipodui build (Part 1
 * integration removed the contract's IPODUI_TRIM_DEFAULTS guard). */
#define IPODUI_TRIM_RECORDING      /* recording, *_enc.codec, peak meter   */
#define IPODUI_TRIM_RADIO          /* remote tuner, RDS, presets           */
#ifndef HAVE_IPOD_DOCK30
#define IPODUI_TRIM_IAP            /* iAP, serial; dock line-out off       */
#endif
#define IPODUI_TRIM_USB_CLASSES    /* HID, HID mouse, UAC, USB-iAP; MSC stays */
#define IPODUI_TRIM_DIRCACHE       /* device path == sim path              */
#define IPODUI_TRIM_UTF8PROC       /* firmware/common/nfc.c instead        */
#define IPODUI_TRIM_MISC           /* AB repeat, morse, car-adapter tick,
                                      secondary codepage_table static,
                                      language_buffer static (38,232 B)
                                      -> a pinned core_alloc sized to the
                                      loaded .lng at boot (none for the
                                      built-in English)                   */
#ifndef HAVE_IPOD_UI_PLUGINS       /* the plugin API needs these back      */
#define IPODUI_TRIM_PLUGINS        /* slot 512 KB -> IPODUI_SCRATCH_SIZE   */
#define IPODUI_TRIM_VOICE          /* talk.h stubs                         */
#define IPODUI_TRIM_SKIN           /* skin engine, WPS, skinned bar        */
#define IPODUI_TRIM_STOCK_MENUS    /* root menu, apps/menus (menu.c stays) */
#define IPODUI_TRIM_BROWSER        /* tree, filetree, filetypes, onplay,
                                      playlist viewer/catalog, shortcuts  */
#define IPODUI_TRIM_KEYBOARD
#define IPODUI_TRIM_QUICKSCREEN    /* quickscreen, pitch screen, hotkey    */
#define IPODUI_TRIM_TAGTREE        /* library.c replaced it (P4); the
                                      stats/resume handlers stay (H1).
                                      In this block: tree.c and onplay.c
                                      call tagtree_* (BROWSER -> TAGTREE) */
#endif

/* Variant coherence: a trim that the variant itself contradicts. */
#if defined(IPODUI_TRIM_IAP) && defined(HAVE_IPOD_DOCK30)
#error "IPODUI_TRIM_IAP contradicts --dock30 (the unit has a dock connector)"
#endif
#if defined(IPODUI_TRIM_PLUGINS) && defined(HAVE_IPOD_UI_PLUGINS)
#error "IPODUI_TRIM_PLUGINS contradicts --ipodui-plugins"
#endif

/* Dependency graph (Rev 1, H3; edges measured by A's single-trim revert
 * builds in Part 1). "A needs B" = bringing A back (deleting its line)
 * also needs B back, because A's code calls into B:
 *   PLUGINS     needs BROWSER, STOCK_MENUS, KEYBOARD, SKIN, QUICKSCREEN,
 *                     VOICE                      (plugin.c's API table)
 *   SKIN        needs BROWSER, QUICKSCREEN       (the WPS: onplay, quickscreen)
 *   BROWSER     needs STOCK_MENUS, SKIN, TAGTREE (root_menu, skin; tree.c and
 *                     onplay.c call tagtree_load/enter/...)
 *   TAGTREE     needs KEYBOARD, BROWSER          (keyboard, onplay)
 *   STOCK_MENUS needs BROWSER, SKIN, PLUGINS, TAGTREE (bookmark/catalog/
 *                     viewer, gui_wps_show, plugin_load, tagtree_export)
 *   QUICKSCREEN needs BROWSER, SKIN              (do_shortcut_menu,
 *                     shortcuts_add, sb_skin_force_next_update)
 *   IAP         leaf (iap-lingo4.c starts a playlist without
 *                     filetree.c under BROWSER; the --dock30 build)
 *   RADIO       needs IAP, KEYBOARD, BROWSER, SKIN, STOCK_MENUS
 *                     (ipod_remote_tuner.c iap_send_pkt; presets.c
 *                     kbd_input/rockbox_browse; radio_skin.c; radio menu)
 *   Measured leaves: KEYBOARD, VOICE, MISC, USB_CLASSES, IAP.
 * Cycles (SKIN, BROWSER, TAGTREE, STOCK_MENUS, PLUGINS) come back
 * together. The checks make an incoherent set fail at compile time with a
 * clear message naming every need, instead of a link error. Keep
 * scripts/trim-deps.txt in step with these checks (T). */
#if !defined(IPODUI_TRIM_PLUGINS) && (defined(IPODUI_TRIM_BROWSER) || \
    defined(IPODUI_TRIM_STOCK_MENUS) || defined(IPODUI_TRIM_KEYBOARD) || \
    defined(IPODUI_TRIM_SKIN) || defined(IPODUI_TRIM_QUICKSCREEN) || \
    defined(IPODUI_TRIM_VOICE))
#error "plugin API needs BROWSER, STOCK_MENUS, KEYBOARD, SKIN, QUICKSCREEN, VOICE"
#endif
#if !defined(IPODUI_TRIM_SKIN) && (defined(IPODUI_TRIM_BROWSER) || \
    defined(IPODUI_TRIM_QUICKSCREEN))
#error "skin/WPS needs BROWSER and QUICKSCREEN"
#endif
#if !defined(IPODUI_TRIM_BROWSER) && (defined(IPODUI_TRIM_STOCK_MENUS) || \
    defined(IPODUI_TRIM_SKIN) || defined(IPODUI_TRIM_TAGTREE))
#error "browser needs STOCK_MENUS, SKIN and TAGTREE"
#endif
#if !defined(IPODUI_TRIM_TAGTREE) && (defined(IPODUI_TRIM_KEYBOARD) || \
    defined(IPODUI_TRIM_BROWSER))
#error "tagtree needs KEYBOARD and BROWSER"
#endif
#if !defined(IPODUI_TRIM_STOCK_MENUS) && (defined(IPODUI_TRIM_BROWSER) || \
    defined(IPODUI_TRIM_SKIN) || defined(IPODUI_TRIM_PLUGINS) || \
    defined(IPODUI_TRIM_TAGTREE))
#error "stock menus need BROWSER, SKIN, PLUGINS and TAGTREE"
#endif
#if !defined(IPODUI_TRIM_QUICKSCREEN) && (defined(IPODUI_TRIM_BROWSER) || \
    defined(IPODUI_TRIM_SKIN))
#error "quickscreen needs BROWSER and SKIN"
#endif
#if !defined(IPODUI_TRIM_RADIO) && (defined(IPODUI_TRIM_IAP) || \
    defined(IPODUI_TRIM_KEYBOARD) || defined(IPODUI_TRIM_BROWSER) || \
    defined(IPODUI_TRIM_SKIN) || defined(IPODUI_TRIM_STOCK_MENUS))
#error "radio needs IAP, KEYBOARD, BROWSER, SKIN and STOCK_MENUS"
#endif

/* Fit-1 C12a: one storage command moves up to 256 KB (buffering's fill
 * read); upstream caps it at 256 FAT sectors (128 KB at 512 B) */
#define FAT_MAX_TRANSFER_BYTES (256 * 1024)

/* Scratch slot (M8): every plugin_get_buffer() caller lives in BROWSER,
 * SKIN or TAGTREE code. reach.py confirms none is live; then 0. */
#if defined(IPODUI_TRIM_PLUGINS) && defined(IPODUI_TRIM_BROWSER) && \
    defined(IPODUI_TRIM_SKIN) && defined(IPODUI_TRIM_TAGTREE)
#define IPODUI_SCRATCH_SIZE 0
#elif defined(IPODUI_TRIM_PLUGINS)
#define IPODUI_SCRATCH_SIZE 0x14000   /* holds struct cuesheet (73.2 KB) */
#endif
#endif /* HAVE_IPOD_UI */

/* Fit-1 M0: the measurement kit (firmware/export/devstats.h): per-thread
 * CPU and disk attribution, tap-to-PCM. Developer builds only. */
#if defined(HAVE_IPOD_UI_DEVMODE) && !defined(BOOTLOADER)
#define HAVE_DEVSTATS
#endif

/* Fit-1 C4b: ATA DMA completion wakes the waiting thread from the IDE
 * interrupt (source 23) instead of a yield() poll; polling comes back by
 * itself if the interrupt is not seen (ata-pp5020.c). */
#if defined(HAVE_IPOD_UI) && !defined(BOOTLOADER) && !defined(SIMULATOR)
#define HAVE_ATA_DMA_IRQ
#endif

/* Fit-1 C7: the developer experiment that forces UDMA4 on the flash
 * adapter (IDENTIFY words 76/93 are 0, so it negotiates UDMA2). Off by
 * default, developer builds and flash units only; DMA writes are off
 * while it is forced, and the first failed command falls back to UDMA2
 * (ata.c, apps/ipodui/udmatest.c). */
#if defined(HAVE_IPOD_UI_DEVMODE) && !defined(BOOTLOADER) \
    && !defined(IPOD_STORAGE_HDD) && !defined(SIMULATOR)
#define HAVE_ATA_UDMA_FORCE
#endif
/* Fit-2: work for the second core (firmware/export/cop_job.h,
 * docs/fit2-brief.md). First client: the codec's decode on the COP, on by
 * default in every shell build (owner, 2026-10-18, on session 36's
 * numbers); the developer row turns it off until the next boot, for A/B
 * and a later battery comparison (apps/codec_cop.c). A video's soundtrack
 * still decodes on the CPU (fit2-brief section 12.1). The simulator has
 * one core: the same code runs every job on the caller's thread and the
 * switch changes nothing. */
#if defined(HAVE_IPOD_UI) && !defined(BOOTLOADER)
#define HAVE_COP_CODEC
#define HAVE_COP_JOBS
#endif
/* P19 S1/S3 (docs/design/p19-ui-on-cop.md section 7): the shell's loop
 * may run on its own "ui" thread, on the CPU or the COP (19-9), with the
 * main thread as the back end's service thread (apps/ipodui/fe_be.h).
 * Every shell build, release included (0.8.0 gate, owner 2026-10-26).
 * Which one a boot runs: the boot guard's fallback, then (developer
 * builds) the ui_split marker, then the hidden config.cfg key "ipodui ui
 * core", then IPODUI_UI_CORE_DEFAULT below (fe_be.c mode_read()). */
#if defined(HAVE_IPOD_UI) && !defined(BOOTLOADER)
#define HAVE_UI_THREAD_SPLIT
/* The build default, enum fe_mode: 0 the shell on the main thread, 1 its
 * own CPU thread, 2 the COP.  The 0.8.0 flip once bench 4's 19-9 rows
 * (ucop, uck1, uck2, ufeel, upanel, ucop2: underruns=0, offcore=0) pass
 * is this one line, 0 -> 2 (docs/release-plan.md 1.3). */
#ifndef IPODUI_UI_CORE_DEFAULT
#define IPODUI_UI_CORE_DEFAULT 0
#endif
#endif
#endif /* IPODVIDEO_VARIANT_H */
