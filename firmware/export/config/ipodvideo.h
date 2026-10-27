/* Modified for ProkBox5g (2026); see docs/changes-from-rockbox.md. */
/*
 * This config file is for the Apple iPod Video
 */

/* Phase 9: per-unit variant facts and the shell's trims (rule 7) */
#include "ipodvideo-variant.h"

#define IPOD_ARCH 1

#define MODEL_NAME   "Apple iPod Video"

/* For Rolo and boot loader */
#define MODEL_NUMBER 5

/* define this if you use an ATA controller */
#define CONFIG_STORAGE STORAGE_ATA

/* define this if the ATA controller and method of USB access support LBA48 */
#define HAVE_LBA48

#ifndef IPODUI_TRIM_RECORDING
/* define this if you have recording possibility */
#define HAVE_RECORDING
#endif

/* Define bitmask of input sources - recordable bitmask can be defined
   explicitly if different */
#if !defined(IPODUI_TRIM_RECORDING) && !defined(IPODUI_TRIM_RADIO)
#define INPUT_SRC_CAPS (SRC_CAP_LINEIN | SRC_CAP_FMRADIO)
#elif !defined(IPODUI_TRIM_RECORDING)
#define INPUT_SRC_CAPS SRC_CAP_LINEIN
#elif !defined(IPODUI_TRIM_RADIO)
#define INPUT_SRC_CAPS SRC_CAP_FMRADIO
#endif /* else config_caps.h: playback only */

/* define the bitmask of hardware sample rates */
#define HW_SAMPR_CAPS   (SAMPR_CAP_48 | SAMPR_CAP_44 | SAMPR_CAP_32 | \
                         SAMPR_CAP_24 | SAMPR_CAP_22 | SAMPR_CAP_16 | \
                         SAMPR_CAP_12 | SAMPR_CAP_11 | SAMPR_CAP_8)

#ifndef IPODUI_TRIM_RECORDING
/* define the bitmask of recording sample rates */
#define REC_SAMPR_CAPS  HW_SAMPR_CAPS
#endif




/* define this if you have a colour LCD */
#define HAVE_LCD_COLOR

/* define this if you want album art for this target */
#define HAVE_ALBUMART

/* define this to enable bitmap scaling */
#define HAVE_BMP_SCALING

/* define this to enable JPEG decoding */
#define HAVE_JPEG

#ifndef IPODUI_TRIM_QUICKSCREEN
/* define this if you have access to the quickscreen */
#define HAVE_QUICKSCREEN
#endif

/* define this if you would like tagcache to build on this target */
#define HAVE_TAGCACHE

/* LCD dimensions */
#define LCD_WIDTH  320
#define LCD_HEIGHT 240
/* sqrt(320^2 + 240^2) / 2.5 = 160.0 */
#define LCD_DPI 160
#define LCD_DEPTH  16   /* 65536 colours */
#define LCD_PIXELFORMAT RGB565 /* rgb565 */

/* LCD stays visible without backlight - simulator hint */
#define HAVE_TRANSFLECTIVE_LCD

#define CONFIG_KEYPAD IPOD_4G_PAD

/* Define this to have CPU boosted while scrolling in the UI */
#define HAVE_GUI_BOOST

#if !defined(IPODUI_TRIM_MISC) && !defined(IPODUI_TRIM_KEYBOARD)
/* Define this to enable morse code input */
#define HAVE_MORSE_INPUT
#endif




/* define this if you have a real-time clock */
#define CONFIG_RTC RTC_PCF50605

/* Define if the device can wake from an RTC alarm */
#define HAVE_RTC_ALARM

/* Define this if you can switch on/off the accessory power supply */
#define HAVE_ACCESSORY_SUPPLY

/* Define this, if you can switch on/off the lineout */
#define HAVE_LINEOUT_POWEROFF

/* Define this if you have a software controlled poweroff */
#define HAVE_SW_POWEROFF

/* The number of bytes reserved for loadable codecs */
#define CODEC_SIZE 0x100000

/* The number of bytes reserved for loadable plugins */
#ifdef IPODUI_TRIM_PLUGINS
/* Phase 9 scratch slot (M8): sized by the live plugin_get_buffer() callers,
 * 0 when none is compiled in (ipodvideo-variant.h, apps/scratchbuf.c) */
#define PLUGIN_BUFFER_SIZE IPODUI_SCRATCH_SIZE
#else
#define PLUGIN_BUFFER_SIZE 0x80000
#endif

/* Define this if you have the WM8758 audio codec */
#define HAVE_WM8758

#ifndef IPODUI_TRIM_MISC
#define AB_REPEAT_ENABLE
#define ACTION_WPSAB_SINGLE ACTION_WPS_BROWSE
#endif

#ifdef IPOD_STORAGE_HDD
/* define this if you have a disk storage, i.e. something
   that needs spinups and can cause skips when shaked */
#define HAVE_DISK_STORAGE
#else
/* Flash-modded iPod Video: use flash storage semantics.
   No spinup delays, no anti-skip buffer, no disk spindown timer. */
#define HAVE_FLASH_STORAGE
#endif

/* Define this for LCD backlight available */
#define HAVE_BACKLIGHT
#define HAVE_BACKLIGHT_BRIGHTNESS

#ifndef BOOTLOADER
/* Support for LCD sleep/BCM shutdown */
#define HAVE_LCD_SLEEP
#define HAVE_LCD_SLEEP_SETTING
/* The same code may also be used when shutting down the iPod */
#define HAVE_LCD_SHUTDOWN
#endif

/* We can fade the backlight by using PWM */
#define CONFIG_BACKLIGHT_FADING BACKLIGHT_FADING_PWM

/* Main LCD backlight brightness range and defaults */
#define MIN_BRIGHTNESS_SETTING      1
#define MAX_BRIGHTNESS_SETTING      32
#define DEFAULT_BRIGHTNESS_SETTING  16


/* define this if the unit uses a scrollwheel for navigation */
#define HAVE_SCROLLWHEEL
/* define to activate advanced wheel acceleration code */
#define HAVE_WHEEL_ACCELERATION
/* define from which rotation speed [degree/sec] on the acceleration starts */
#define WHEEL_ACCEL_START 270
/* define type of acceleration (1 = ^2, 2 = ^3, 3 = ^4) */
#define WHEEL_ACCELERATION 3

/* Define this if you can detect headphones */
#define HAVE_HEADPHONE_DETECTION

/* Type of mobile power (Phase 9: IPOD_BATTERY_MAH, configure --battery;
 * 0 = the stock cell, picked by model at runtime) */
#if IPOD_BATTERY_MAH > 0
#define BATTERY_CAPACITY_DEFAULT IPOD_BATTERY_MAH /* fitted cell; only for
                                                     variable initialisation */
#define BATTERY_CAPACITY_DEFAULT_THIN IPOD_BATTERY_MAH /* both models */
#define BATTERY_CAPACITY_DEFAULT_THICK IPOD_BATTERY_MAH
#else
#define BATTERY_CAPACITY_DEFAULT  400  /* only for variable initialisation */
#define BATTERY_CAPACITY_DEFAULT_THIN  400 /* default battery capacity for the
                                              30GB model */
#define BATTERY_CAPACITY_DEFAULT_THICK 600 /* default battery capacity for the
                                              60/80GB model */
#endif
#define BATTERY_CAPACITY_MIN      300 /* min. capacity selectable */
#if IPOD_BATTERY_MAH > 3000
#define BATTERY_CAPACITY_MAX IPOD_BATTERY_MAH /* max. capacity selectable */
#else
#define BATTERY_CAPACITY_MAX     3000 /* max. capacity selectable --
                                         3rd party batteries go this high */
#endif
#define BATTERY_CAPACITY_INC       50 /* capacity increment */


#define CONFIG_BATTERY_MEASURE VOLTAGE_MEASURE

/* Hardware controlled charging with monitoring */
#define CONFIG_CHARGING CHARGING_MONITOR

/* define this if the unit can be powered or charged via USB */
#define HAVE_USB_POWER

/* define this if the unit can have USB charging disabled by user -
 * if USB/MAIN power is discernable and hardware doesn't compel charging */
#define HAVE_USB_CHARGING_ENABLE
#define TARGET_USB_CHARGING_DEFAULT USB_CHARGING_FORCE

/* define current usage levels */
#define CURRENT_NORMAL     24  /* 30MHz clock, LCD off, accessory supply on */
#define CURRENT_BACKLIGHT  20  /* FIXME: this needs adjusting */
#if defined(HAVE_RECORDING)
#define CURRENT_RECORD     35  /* FIXME: this needs adjusting */
#endif

#ifndef IPODUI_TRIM_RADIO
/* Define Apple remote tuner */
#define CONFIG_TUNER IPOD_REMOTE_TUNER
#define HAVE_RDS_CAP
#define CONFIG_RDS RDS_CFG_PUSH
#endif

/* Define this if you have a PortalPlayer PP5022 */
#define CONFIG_CPU PP5022

/* Define this if you want to use the PP5020 i2c interface */
#define CONFIG_I2C I2C_PP5020

#ifdef IPOD_STORAGE_HDD
/* We're able to shut off power to the HDD */
#define HAVE_ATA_POWER_OFF

/* Turn on SMART support */
#define HAVE_ATA_SMART
#else
/* Flash-modded: no HDD power cycling needed */
/* #define HAVE_ATA_POWER_OFF */

/* SMART not applicable to flash adapters */
/* #define HAVE_ATA_SMART */
#endif

/* define this if the hardware can be powered off while charging */
//#define HAVE_POWEROFF_WHILE_CHARGING

/* The start address index for ROM builds */
#define ROM_START 0x00000000

/* The size of the flash ROM */
#define FLASH_SIZE 0x100000

/* Define this to the CPU frequency */
#define CPU_FREQ      11289600

#define CONFIG_LCD LCD_IPODVIDEO

/* USB On-the-go */
#define CONFIG_USBOTG USBOTG_ARC

/* enable these for the experimental usb stack */
#define HAVE_USBSTACK
#define USB_VENDOR_ID 0x05ac
#define USB_PRODUCT_ID 0x1209
#ifndef IPODUI_TRIM_USB_CLASSES
#define HAVE_USB_HID_MOUSE
#endif

/* Define this if you have adjustable CPU frequency */
#define HAVE_ADJUSTABLE_CPU_FREQ

/* Define this if you can read an absolute wheel position */
#define HAVE_WHEEL_POSITION

#define HAVE_HARDWARE_CLICK

/* define this if the device has larger sectors when accessed via USB */
#define MAX_VIRT_SECTOR_SIZE 2048

/* If we have no valid paritions, advertise this as our sector size */
#define DEFAULT_VIRT_SECTOR_SIZE 2048

#ifdef HAVE_IPOD_UI
#if !defined(BOOTLOADER) && defined(IPODUI_WITH_MODULES)
/* ProkBox (#106): cold shell screens run as modules from modules.pak in
 * one arena reserved at shell start (apps/ipodui/modload.h). Off by
 * default until P12's tenants make it pay; EXTRA_DEFINES=
 * -DIPODUI_WITH_MODULES turns it on.
 * The storage thread runs the module reads (storage_post_job()). */
#define IPODUI_MODULES
#define HAVE_STORAGE_JOBS
/* the jobs open and read files on the storage thread: the open path's
 * frames (-fstack-usage: about 1.3 KB deepest, path walk + readdir) plus
 * a read with ATA retries come too near the default 2 KB; the load log
 * prints the thread's measured use (stk=%) for the device check */
#define ATA_THREAD_STACK_SIZE (DEFAULT_STACK_SIZE * 3)
#endif
/* ProkBox (#63): USB shows the 2048-byte layout as an ordinary 512-byte
 * disk, which every host mounts (macOS's automount refuses the 2048 one).
 * Nothing on the disk changes: firmware/usbstack/usb_storage_xlate.c */
#define HAVE_USB_SECTOR_XLATE
#ifndef BOOTLOADER
/* ProkBox: what the last panic or exception reported, kept in RAM a reset
 * does not clear and logged by the next boot (panic_record.h); the shell
 * build also draws no panic screen */
#define HAVE_PANIC_RECORD
/* ProkBox (p19-hangrec): the hang watcher, a tick task that writes a
 * PANIC_REC_HANG record when a watched thread (the shell's, be_svc, the
 * art builder, the storage thread) makes no progress for its bound while
 * at work (hang_watch.h) */
#define HAVE_HANG_RECORD
/* ProkBox P19 S0 (19-9 step 0): the kernel objects and flags both cores
 * may touch (the LCD port lock, engine 2's claim, the BCM flags, the
 * piezo queue, the shell's cross-thread locks) live in uncached shared
 * memory, engine 2's claim takes a corelock, and buflib and tick-list
 * edits are marked CPU-only (firmware/export/xcore.h).  No behaviour
 * change on one core; the stock build keeps upstream's placement. */
#define HAVE_COP_COHERENCE
/* ProkBox P19 (#126): the Now Playing visualizer's PCM tap, the mixer's
 * output frame by frame into a ring while the view is open
 * (apps/ipodui/viz_tap.c; a NULL hook otherwise) */
#define HAVE_VIZ_TAP
/* ProkBox P19 MF-4: the pixel hand-off across cores (xcore.h,
 * lcd-fb-xcore.c): a CPU push of the frame the shell drew on the COP asks
 * for the COP's write-back (fe_fb_publish()) and reads uncached.  Every
 * shell firmware, release included, since the UI may run on the COP
 * there too (HAVE_UI_THREAD_SPLIT); the shell's fe_cop_refusal() keys on
 * it */
#ifndef SIMULATOR
#define HAVE_FE_FB_HANDOFF
#endif
#endif
/* ProkBox (#63): USB session counters for the device log
 * (usb_storage_stats.h, ATA DMA/PIO counts), and read-ahead for sequential
 * USB reads from the existing double buffer (usb_storage_ra.h) */
#define HAVE_USB_STATS
#define HAVE_ATA_XFER_STATS
#define HAVE_USB_READ_AHEAD
/* ProkBox (#81): in USB mode, a prefetch ring in the idle audio buffer
 * ahead of sequential READs, so macOS's mount-time FAT sweep runs faster
 * (usb_storage_pf.h). Without memory the read-ahead above runs instead. */
#define HAVE_USB_PREFETCH
/* PP5022's USB and IDE bus masters can starve one another while the USB
 * prefetch worker overlaps large ATA DMA reads with bulk-IN DMA. By
 * default no worker runs: the USB thread fills the ring in line, between
 * pieces, with no ATA DMA during a bulk-IN (demand batching). The worker
 * stays available for development tests. Every command that may change
 * the disk drops the ring first (usbstor_pf_cmd()). */
#define USB_PREFETCH_DEFAULT_OFF
/* Diagnostic control for #81: unlike the synchronous read-ahead above,
 * ordinary multi-buffer SCSI READs still used to start the next ATA DMA
 * while the current bulk-IN transfer was active.  Keep every ATA DMA on
 * the far side of that completion while we establish whether the two PP5022
 * bus masters are the cause of the active-dTD stalls.  This is deliberately
 * a conservative diagnostic default, not a throughput claim. */
#define USB_STORAGE_NO_READ_OVERLAP
/* PP5022 ARC Bulk-IN can strand the tail of a 16 KiB dTD while macOS is
 * probing the volume. Keep the 64 KiB BOT transfer, but describe it in
 * 4 KiB pieces for the USB-C reliability trial. */
#define USB_ARC_SMALL_DTD
/* ProkBox (USB resets): a USB reset interrupt that finds the port out of
 * reset and our address intact is acknowledged without tearing the
 * session down (firmware/target/arm/usb-arc-reset.h) */
#define HAVE_USB_RESET_GATE
/* ProkBox (Phase 15): the WM8758's PLL, DACs, mixers and headphone amps
 * power down once PCM has been stopped for DAC_IDLE_MS (dac_ext.h) */
#define HAVE_DAC_STANDBY
/* ProkBox (Fit-1 C2): fixed playback priority bands (codec, video feed,
 * buffering, UI; no pcmbuf ladder) and time-bounded aging in the
 * scheduler (firmware/kernel/include/sched_age.h, docs/fit1-brief.md B) */
#define HAVE_SCHED_BANDS
#ifndef BOOTLOADER
/* ProkBox #114 (P18 A3, 18-2): the drive's power is cut when idle (also
 * between buffer refills while playing) and at sleep entry, with one
 * power-down/power-up path (firmware/export/stor_pwr.h); a rotating drive
 * keeps upstream's policy. The simulator models it (filesystem-sim.c). */
#define HAVE_STORAGE_POWEROFF
#endif
#if !defined(SIMULATOR) && !defined(BOOTLOADER)
/* ProkBox (#75/#76, session 37): the UI's screen pushes (lcd_update and
 * whole-width lcd_update_rect bands) go to the BCM through the PP's second
 * DMA engine while the CPU yields, instead of a ~27 ms CPU copy that holds
 * the single core (in USB mode long enough for the host to reset the
 * device).  The CPU's copy stays for small, narrow, IRQ-off or failed
 * pushes (lcd-video.c, e2-port.c; the withdrawn "lcd_e2off" A/B marker is
 * gone: docs/hw-experiments.md) */
#define HAVE_LCD_E2
/* ProkBox Fit-1 C6: unboosted and with nothing needing the PLL (dark,
 * no USB, no ATA DMA, a track that fits), run from the 24 MHz crystal
 * with the PLL off instead of 30 MHz from the PLL (firmware/pll_idle.c,
 * docs/fit1-brief.md section C). Every PP5022/PP5021C iPod Video. */
#define HAVE_PLL_IDLE
/* ProkBox band governor, G1 (docs/design/pp-governor.md): the clock band
 * from measured per-core load, holders as floors, cpu_boost() wrapped
 * (firmware/pgov{,_core}.c).  Built in, off until the developer switch
 * (a marker file) turns it on; off is today's code path.  Developer
 * builds only, like its stage 2 below: only a developer marker can turn
 * it on, so a release build (--ipodui-release) compiles none of it (and
 * core_sleep() takes no idle stamp) */
#ifdef HAVE_IPOD_UI_DEVMODE
#define HAVE_PP_GOV
#endif
/* Its stage 2 (docs/design/pp-governor.md section 16): band 1 and band 0
 * one core-voltage step down (DCDC1 0xEB), band 3 at 100 MHz where the
 * speed grade allows.  Built in, off at runtime: a second marker runs it
 * as a logged shadow, a third applies it, with a watchdog that reverts to
 * stage 1.  Needs the raw PMU path (HAVE_POWER_SLEEP's pmu_raw).
 * Developer builds only until a bench sitting has run it: a release
 * build (--ipodui-release) compiles none of it */
#ifdef HAVE_IPOD_UI_DEVMODE
#define HAVE_PP_GOV_DVS
#endif
/* ProkBox Phase 18-1: soft sleep with RAM kept, the kernel frozen and
 * both cores in PROC_SLEEP (firmware/export/power_sleep.h,
 * power_sleep-pp502x.c, docs/phase18-brief.md 4.2); the wake path resets
 * the battery filter from the sleep's raw reading */
#define HAVE_POWER_SLEEP
#endif
#ifndef BOOTLOADER
/* the wake path (above) and the simulator's battery pin
 * (powermgmt-sim.c) restart the battery filter from a reading */
#define HAVE_RESET_BATTERY_FILTER
#endif
#if (defined(HAVE_BCMFW_VIDEO) || defined(HAVE_BCMFW_MERGED_VIDEO)) && \
    !defined(BOOTLOADER)
/* ProkBox P19 W0-5/W0-6: the BCM2722 display job interface (bcmfw/jobs/
 * bcmjob.h) where our merged image can run: the video builds, and
 * --bcmfw-merged without --bcmfw-video (the release line: the jobs and the
 * resident image, no player; p19-release-noplayer).  The
 * device: the host driver (bcmjob-dev.c, bcmjob-core.c) and the reference
 * executor as every job's CPU path (bcmjob-ref.c, bcmjob-cpu.c).  The
 * simulator: the host model (uisimulator/common/sim_bcm_job.c over
 * bcmfw/jobs/jmodel.c), which also reserves BCM_RESIDENT_BYTES at shell
 * start so the floor gate counts the resident image (EXTRA_DEFINES=
 * -DHAVE_BCM_JOBS_MODEL turns it on in the plain shell simulator).
 * Nothing in the shell sends jobs yet. */
#ifdef SIMULATOR
#define HAVE_BCM_JOBS_MODEL
#else
#define HAVE_BCM_JOBS
/* the boot path's resident rung: bcmvideo.bin read once at boot
 * (apps/video/bcmres.c) and offered as the display driver; the player
 * plays from the same block when it is built */
#define HAVE_BCMFW_RESIDENT
#endif
#endif
#if defined(HAVE_BCM_JOBS) || defined(HAVE_BCM_JOBS_MODEL)
/* the resident image's PP copy: the merged image's cap (the arena starts
 * at 0x35400; docs/research/bcm2722/memory-contract.md section 5) */
#define BCM_RESIDENT_BYTES 0x35400
#endif
#endif

/* define this if the hard drive uses large physical sectors (ATA-7 feature) */
/* and doesn't handle them in the drive firmware */
#define MAX_PHYS_SECTOR_SIZE 1024

/* define this if we want to support 512n and 4Kn drives */
//#define MAX_VARIABLE_LOG_SECTOR 4096

#define BOOTFILE_EXT "ipod"
#define BOOTFILE "rockbox." BOOTFILE_EXT
#define BOOTDIR "/.rockbox"

#define ICODE_ATTR_TREMOR_NOT_MDCT

#ifndef IPODUI_TRIM_IAP
#define IPOD_ACCESSORY_PROTOCOL
#define HAVE_SERIAL
#if defined(HAVE_IPOD_UI) && !defined(BOOTLOADER)
/* ProkBox: the iap thread is made at an accessory's first sync pair, on
 * the USB thread, and the 100 ms tick stops at a USB unplug. The buffers
 * stay static, sized for this player: RX 4 KB holds eight of the
 * largest inbound packets the lingoes expect (SetDisplayImage: 500 B of
 * payload), room for certificate sections and bursts while a handler
 * waits on the disk; a larger one is dropped by the framer, as upstream
 * drops one that overflows. The stack keeps upstream's 8 KB: the measured worst
 * path is ~7 KB (-fcallgraph-info: lingo 4's 3,360 B frame, a track's
 * metadata parse up to 3,144 B). apps/iap/iap-core.c; docs/hardware.md */
#define IAP_LAZY_THREAD
#define IAP_RX_BUFLEN 4096
#define IAP_STACK_SIZE (DEFAULT_STACK_SIZE*8)
#endif
#endif

/* DMA is used only for reading on PP502x because although reads are ~8x faster
 * writes appear to be ~25% slower.
 */
#ifndef BOOTLOADER
#define HAVE_ATA_DMA
#ifndef IPOD_STORAGE_HDD
/* Flash-modded: PP5022 supports UDMA4 (66 MB/s). Stock disk was limited to
   UDMA2 because the ~10% gain wasn't worth the CPU boost requirement.
   Flash adapters benefit significantly from the higher transfer rate. */
#define ATA_MAX_UDMA 4
#endif
#endif

#ifndef IPODUI_TRIM_QUICKSCREEN
/* Define this if a programmable hotkey is mapped */
#define HAVE_HOTKEY
#endif
