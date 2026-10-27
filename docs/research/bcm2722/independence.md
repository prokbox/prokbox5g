# Display without Apple code (#62)

*Plan, 2026-10-11; level 1 implemented 2026-10-12 behind `--bcmfw-l1` (§3a), not yet run on the device. No device write comes from this file. Owner direction: eventually no Apple code at any stage (#62). Confidence tags as in `hardware.md`: [H] seen on the device or in our code, [M] strong inference, [L] guess.*

This plan reaches **level 1**: in normal use ProkBox never uploads Apple's `vmcs` to the BCM. Our image drives the panel from early boot through every sleep and wake, and Apple's `vmcs` is only the fallback until our image is proven. The later levels are sketched at the end.

## 1. What runs today

### The boot chain [H/M]

| Step | Code | Who owns the BCM |
|---|---|---|
| 1. Power-on | Apple's flash ROM bootloader (NOR at `0x20000000`) | **Apple's vmcs, started by the ROM [M]** |
| 2. Rockbox bootloader (`bootloader/ipod.c`) | ours (`osos` via ipodpatcher) | Apple's vmcs, still running |
| 3. Rockbox main: `lcd_init_device()` | ours | Apple's vmcs ("BCM is powered. Assume it is initialized.") |
| 4. Shell start: `bcm_boot_switch()` (`apps/ipodui/ui_main.c:742`, dev7) | ours, test builds only | **ours**, if the toggle and pass flag allow it |
| 5. Every backlight sleep and wake | `lcd_sleep()`/`lcd_awake()` | whatever `flash_vmcs_*` points at: ours once step 4 swapped |

**Why step 1 is the ROM's vmcs [M]:**
- The Rockbox bootloader draws its boot screen with `lcd_update()` without uploading anything, because its `lcd_init_device()` has no upload in `BOOTLOADER` builds.
- The main firmware's `lcd_init_device()` uploads only when the BCM is unpowered, which its own comment says "can only happen when loading Rockbox via ROLO".

So something before our code has already powered the BCM, uploaded a `vmcs` and started it. The only code before ours is Apple's flash ROM. Session 8's cold-boot observation (§5, step 1) turns this into a device fact.

### What `bcm_init()` does, and which parts are Apple's

Read from `firmware/target/arm/ipod/video/lcd-video.c`.

| Part | What it is | Apple code? |
|---|---|---|
| `flash_get_section('vmcs')` | Walks the NOR directory at `0x200FFE00` (`flsh` entries, 40 bytes each), finds `vmcs` and sums its bytes against the directory's checksum | No: Rockbox's reader (FS#6721). It *locates* Apple's image |
| The NOR `vmcs` image | Broadcom/Alphamosaic RTOS and display driver | **Yes**: the only Apple/Broadcom code this path runs |
| Power: `GPO32_VAL \|= 0x4000`, 50 ms | The PP's GPO bit that powers the BCM | No: hardware |
| Bootstrap stages 1–2: `STRAP_OPT_A`, `0x70000040 = 0x1313`, the 8-byte `bcm_bootstrapdata` on `BCM_CONTROL`/`BCM_ALT_CONTROL`, the ready-bit waits | The host-port handshake into the BCM's boot ROM | No: Rockbox's sequence; the BCM's own mask ROM answers it |
| Stage 3: upload to SRAM 0, `0x10000C00 = 0xC0000000`, wait for bit 0, `0x10000400 = 0xA5A50002` | Load and start | No: the same path loads our image (session 4 onwards) |
| Wait for `BCMA_COMMAND` ≠ 0 | Ready | No. Bounded to 1 s in `HAVE_BCMFW_DEVTEST` builds, **unbounded in stock** |

**What this tells us:** the whole host-side path is already free of Apple code. What remains Apple's is the image itself, plus whatever the ROM loaded at step 1.

## 2. What our image already does, and what it must take over

**A wake is already a cold start for the BCM [H].** `lcd_sleep()` sends `LCD_SLEEP` and `0xC`, then cuts the BCM's power (GPO `0x4000`). So every wake re-bootstraps the chip from scratch, and our image then powers the panel on from its first `LCD_UPDATE`. This is device-verified:
- **Sessions 5–7:** every DMA image powered panel 3 on from cold.
- **Session 7, stage 5:** 3 wakes, with the image ready in 1.9–2.1 ms and the first frame 436–458 ms after the upload began.
- **Session 7:** a boot swap whose first frame was confirmed in 283 ms.

| Duty | Apple's vmcs | Ours (dev7 `hello_dma`) | Needed for level 1 |
|---|---|---|---|
| Panel power-on script (panel ID from the info word; 6 variants) | yes | yes: `panels.c`, identical event for event to Apple's in `dma_test.py` | nothing new |
| Panel sleep script (`LCD_SLEEP`) | yes | yes | nothing new |
| Full and partial updates (`LCD_UPDATE`, `UPDATERECT`) | yes | yes: DMA, with a CPU fallback | nothing new |
| 1 MHz counter (clock block `+0x4C/+0x54`) | yes | yes (session 4) | nothing new |
| PLL to 150 MHz | yes (video) | yes, as stage 4b (session 7) | **UI image stays at 27 MHz** (see `pipeline.md`); video only |
| SDRAM init | yes (video, `RenderServer`) | not in the UI image; the video image's set-up is unverified on the device | **not needed for level 1**: the UI image runs from SRAM only (the framebuffer at `0xE0000`) |
| TV out, selftest, `TV BMP` | yes | no | Not needed: ProkBox never sends them (`BCMCMD_SELFTEST` only in a debug screen [M]; check at the release step) |
| **Cold power-on state left by the ROM** | the ROM's job | n/a | **The one real difference** (see "The cold-boot difference" below) |

### The cold-boot difference

At a wake, the BCM and the panel start from power-off. At a cold boot, our swap starts from **whatever the ROM's vmcs left**: a running core, the panel already on, and possibly clocks or pads the ROM set. Today's swap handles this the stock way:
1. `LCD_SLEEP`;
2. cut the power;
3. wait 50 ms;
4. bootstrap our image;
5. power the panel on again.

Session 7 shows this works [H]: `power-down: stock order`, image ready 1.9 ms after the start, first frame confirmed at 283 ms. So level 1 needs no new hardware knowledge. It needs the swap moved earlier and made a release feature.

## 3. Level 1: what changes

1. **Move the boot code out of the device test** (`pipeline.md`, "Not done yet").
   - A small `firmware/target/arm/ipod/video/bcmfw-boot.c` behind its own `HAVE_BCMFW_BOOT` flag gets these from the devtest code:
     - the swap (`bcmdt_boot_swap` minus the test logging);
     - the attempt marker;
     - the strikes;
     - `verified.txt` checks;
     - the forced-Apple switch;
     - the embedded `hello_dma` image.
   - The device-test build keeps using it, and a release build includes it without `HAVE_BCMFW_DEVTEST`.
2. **Bound every host-port handshake** before the swap is on by default.
   - Today `bcm_read32()`'s two waits, the bootstrap's `BCM_ALT_CONTROL` and `RD_ADDR` waits, and `bcm_write_addr()`'s wait are unbounded stock loops. (They are bounded in the swap's own HAL, but not in `bcm_init()`'s bootstrap stages 1–3.)
   - A wedged port at boot would freeze the boot screen (§4). Each wait gets a deadline, for example 100 ms; on expiry the step fails the same way "never ready" does.
   - This is the only stock-code change with risk, so it stays `#ifdef HAVE_BCMFW` and must keep `lcd-video.c`'s stock text unchanged without the flag.
3. **Swap earlier: right after the disk is mounted, before the shell starts**, instead of from `ui_main.c`.
   - The swap needs the filesystem for `bcmfw.try` and `bcmfw.strikes`, so it can't run in `lcd_init_device()`.
   - The earliest point that has both the disk and the display is `init()` in `apps/main.c`, right after `disk_mount_all()` (line 729). Settings, the tagcache and playback set-up all come later in `init()`.
   - **The Next-held Developer escape must still win.** Today it runs after `init()` (`apps/main.c`, `ipodui_devmode_boot_requested()`, just before `ipodui_main()`) and before the swap, so the escape's Developer list always draws on Apple's vmcs. The early hook therefore asks `ipodui_devmode_boot_requested()` first and skips the swap if the escape is held. That keeps "hold Next to get a working screen" true.
   - Moving it shortens Apple's window from "until the menu" to "until the disk mounts". It also means the swap's dark interval (§3, "Boot-time cost") falls inside the boot screen rather than after the menu appeared.
   - Behind `HAVE_BCMFW_BOOT`, as one hook call.
4. **ROLO and power-on without the ROM.**
   - When `lcd_init_device()` finds the BCM unpowered (ROLO), it calls `lcd_awake()`, which today uploads **Apple's** vmcs because no swap has run yet.
   - Level 1 points `flash_vmcs_*` at our embedded image before that first `lcd_awake()`, if the same gates allow it (marker absent, strikes < 3, flag or `verified.txt`). If they don't, it keeps Apple's.
   - D8 already avoids ROLO in the shell, so this is a rare path, but level 1 shouldn't leave a hole there.
5. **Default on.** `--bcmfw-default` (dev7) becomes the release build's setting once the image's hash is in `bcmfw/verified.txt` (stage 5 pass plus a clean step 4, session 7 or later), so no per-unit test run is needed.
6. **Fallback stays Apple's NOR vmcs**, unchanged from dev7 (`pipeline.md`, "Fallback").
   - Level 1 adds one rung below our DMA image: our **CPU-push image** (stage 2's `hello`, 5.5 KB, the image without DMA).
   - Only if that also fails is Apple's vmcs loaded. Because the PP can't drive the panel without *some* BCM image, the fallback ladder is what eventually lets level 2 drop Apple's image.

### 3a. What is implemented (branch p14-bcm-l1, 2026-10-12)

Everything below is behind `HAVE_BCMFW_L1` (`tools/configure --bcmfw-l1`, which needs `--bcmfw`), **off by default**. Without it, the stock, `--ipodui` and `--bcmfw` builds link byte for byte as before (checked against dev9 `c7c29d8e7e`, version string masked). No BCM image changed: `bcm-devtest-images.c` and `bcmfw/` are untouched, so every pass flag and `verified.txt` entry still applies.

| Item (§3) | Done | Where |
|---|---|---|
| 1. Boot code out of the device test | **Moved verbatim** into `firmware/target/arm/ipod/video/bcmfw-boot.c` (+ `bcmfw-boot.h` for the level 1 calls); `bcm-devtest-hal.c` `#include`s it where the code was (lcd-video.c's preprocessed text identical). It still needs the device-test HAL's helpers and images, so a release build without `HAVE_BCMFW_DEVTEST` is **not** yet possible: that split (the HAL helpers, the embedded images and `bcmdt_boot_swap` into a non-test unit) is the release step | `bcmfw-boot.c` |
| 2. Bounded handshakes | Every host-port wait in `lcd-video.c` (`bcm_write_addr`, `bcm_read32`, bootstrap stages 1–3, the start bit) is bounded: 20 ms per handshake, 100 ms for the start bit. A port that never answers reads as busy (`0xFFFF`) until the next power-up, so nothing is kicked into it; with our image that also marks it stopped (one rung down at the next update). If **Apple's** vmcs never answers either, the display stays off until the next wake tries again (never a hang; MENU+SELECT resets) | `lcd-video.c`: `bcm_port_wait()`, `bcm_bootstrap_bounded()` |
| 3. Early swap | `bcm_boot_early()` right after the disk mounts (`apps/main.c`, before `pcm_init()`); the shell's later `bcm_boot_switch()` is then a no-op. With the Developer escape (Next) held it is skipped and the shell's call swaps after the list, as in dev7 | `apps/main.c`, `bcmfw-boot.c` |
| 4. ROLO | `lcd_init_device()` notes whether the BCM was powered. An unpowered BCM (ROLO) still wakes onto **Apple's** vmcs there: the gates (marker, strikes, pass flags) are files, and the filesystem isn't mounted yet. The early swap then takes over from it exactly as from the ROM's vmcs | `lcd-video.c`, `bcmfw-boot.c` |
| 5. Default on | Not changed: `--bcmfw-default` plus `verified.txt` as before | — |
| 6. The ladder | At boot: our DMA image, then our CPU-push image (when a stage 2 pass flag names it or `verified.txt` lists it), then Apple's vmcs. The first rung takes over from Apple's like the dev7 swap; a later rung starts cold from the power cut the failed one left (`bcmdt_boot_ladder()`). A boot that needed the CPU rung is a strike. At run time a fallback goes **one rung down**: from our DMA image to our CPU-push image, then to Apple's (`bcmfw_drop()`). The device test still goes straight to Apple's | `bcm-devtest-core.c`, `bcmfw-boot.c` |

**Kept unchanged:** the toggle, the attempt marker (written before the first rung, removed only by a confirmed frame on any rung), the strikes (`bcmdt_strike_next`), and deliberate fallbacks counting no strike.

**Changed meaning under level 1:** "test fallback" now goes one rung down, so on our DMA image with a CPU rung available it lands on our CPU-push image (status "Running: hello"); a second "test fallback" lands on Apple's vmcs.

**Tests (host, `bcmfw/tests/devtest_test.py`, "ladder:" rows, against vc2sim):** eight cases, each bounded (the slowest 3.6 s): DMA good; DMA never ready, faults, hangs in its first frame, or its bootstrap handshake never answers → CPU rung; DMA faults with no CPU rung, DMA faults and CPU hangs, both handshakes never answer → Apple's (BCM off, then the stock restore). Not covered on the host: `lcd-video.c`'s bounded waits themselves (target code; the "never answers" case is modelled by the HAL's bootstrap failing, which is what the bounded wait produces) and the early hook's timing in `init()`. Session 9 covers those.

**Debug entries (Developer list, level 1 builds):**
- **BCM boot path: fail next boot's DMA rung** — writes `/.rockbox/bcmfw.inject` (`swap`); at the next boot the DMA rung's bootstrap reports a handshake failure after the real power cut, and the ladder continues. One-shot; no strike.
- **BCM boot path: fail next boot's every rung** — the same for every rung (`swap-all`): Apple's vmcs, marker kept.
- **BCM boot path: next wake never answers** — in RAM: the next backlight wake's bounded bootstrap reports a timeout on our image, so the wake goes one rung down. No strike.

### Where our image lives

| Option | Pros | Cons | Verdict |
|---|---|---|---|
| **In `rockbox.ipod` (embedded array)**, as dev7 does | Every wake reloads it from RAM, so **no disk access at a wake** (`docs/hardware.md`: never spin up for the UI); updates ship with the firmware; the hash is fixed at build time; no file can go missing | ~8 KB of RAM for the DMA image, plus 5.5 KB for the CPU-push rung | **Level 1, the UI image** |
| On the disk (`/.rockbox/bcm/*.bin`) | Updatable without a firmware build; no RAM until used | A wake would need the disk, or a RAM copy anyway; a missing or corrupt file must be caught (FNV check) | **The video image** (72 KB), read in bulk at video open and hash-checked; not the UI image |
| In the Rockbox bootloader | Could draw the boot screen with our image | The bootloader has no filesystem guards when it draws; bootloader updates are rare and riskier; the main firmware would have to re-find the image | **Not for level 1** (see "Bootloader swap" below) |

The embedded image is the right home for the UI image *because* of the strikes and the marker. Those live on the disk and are read once at boot. Everything a wake needs is already in RAM.

### Boot-time cost [H, session 7]

From the session 7 boot-path block (`bcmtest.log`, the `=== boot path: hello_dma ===` block; times in ms from the block's start):

| Phase | Time |
|---|---|
| `LCD_SLEEP`, power cut, 50 ms off | 0 → 63 → ~247 |
| Bootstrap and upload (8,016 bytes), image ready | 247 → 316 (ready 1.9 ms after start) |
| Panel 3 power-on script and first DMA frame (17.1 ms) | 316 → 674 |
| Confirmed by the host | 717 |

**Net cost:** the swap adds about **0.7 s to boot, with about 0.6 s dark** (power cut at 63 ms to the first frame at 674 ms). Almost all of it is the panel's own power-on delays (268 ms for panel 3, the same in Apple's script) and the stock 50 ms power-off rule. Our image's own start (2 ms) and upload are negligible. The upload will shrink further with dev8's faster PP upload.

### Bootloader swap (level 1b, optional, later)

**The idea:** do the swap in the Rockbox bootloader, before the boot glyph is drawn.
- **Benefit:** it removes the second dark interval, because the glyph and everything after it are drawn by our image.
- **Costs:**
  - The image and the gates move into the bootloader.
  - The bootloader has no disk guards before it draws.
  - A bad bootloader image needs a re-patch, not a file copy.

**A "warm swap" [L]** could save most of the 0.6 s without moving anything: restart the BCM core without cutting GPO `0x4000`, and skip the panel power-on when the info word shows it already on. Nobody has tried this. It needs a stage that tests it, and it's the one place the ROM's leftover state matters. Deferred until level 1 is in daily use.

## 4. Failure at cold boot, and what the user sees

The screen is what fails here, so this section assumes the user sees nothing useful and walks each failure through.

| Failure | What happens | What the user sees | Recovery |
|---|---|---|---|
| Our image never gets ready, or faults during the swap | The swap's HAL times out → power cut → Apple's vmcs reloaded; `bcmfw.try` stays | The boot glyph, ~1 s dark, then the menu on Apple's vmcs | Automatic. No swap on later boots until a mode is chosen again |
| First frame never completes | Same as above (the swap waits for a confirmed frame) | Same | Automatic |
| Image ready, then garbles the screen but "confirms" | Swap counted as a success | A garbled screen | Hold **Next** while booting: the Developer list opens before the swap runs, on Apple's vmcs; choose **Apple vmcs (forced)**. Or reset (MENU+SELECT) and hold **MENU** to boot Apple's firmware |
| **Host port wedges during the swap** (the unbounded waits, §3 item 2) | The PP spins for ever | The boot glyph, frozen, or dark | **MENU+SELECT** (≈6 s) resets the iPod. The marker was written before the swap, so the next boot skips it and runs Apple's vmcs. After level 1's bounding this row becomes "never ready" |
| Marker can't be written (disk full or failing) | The swap is skipped (dev7 aborts before touching the BCM) | Normal boot on Apple's vmcs | Nothing to do |
| Stops later in use | `lcd_tick` sees the fault word or 2 s busy → next update wakes onto Apple's vmcs; one strike | A short dark blink | Automatic; 3 strikes stop the swap |

**Why the user is never stranded:**
- Every failure that leaves the screen frozen or unreadable is reachable by MENU+SELECT. It's a hardware reset, independent of our code, and it's how Rockbox hangs have always been recovered [H].
- After the reset there are three routes back:
  - the marker sends the next boot back to Apple's vmcs;
  - **hold MENU at boot** starts Apple's own firmware, which loads its own vmcs and doesn't involve our image at all (`bootloader/ipod.c:400`);
  - **Disk Mode** (reset, then SELECT+PLAY) lets a Mac delete `/.rockbox/bcmfw.cfg` or `bcmfw.try`. Disk Mode is believed to be in the ROM; that's ⚠️ to verify on a spare unit, per `install-and-restore.md`.

**The one bad case is a garbled-but-confirmed screen.** The owner has to know the Next-at-boot escape. `device-session-1.md` already documents it; the release build's user guide must too.

## 5. What sessions 8 and 9 must prove

These come after dev8, which re-earns the pass flags for the 27 MHz hashes. Each step names its photo and its log line. Steps 1 and 2 need no code and could ride in session 8; steps 3 to 9 need the level 1 build (`bcmfw-boot.c`, bounded handshakes, early swap), so they belong to session 9.

| # | Step | Photo / report | Log line that proves it |
|---|---|---|---|
| 1 | **Cold-boot observation.** Reset (MENU+SELECT), then watch from power-on to the menu | A short phone video, or a description: what shows before our boot glyph (Apple logo, white, black?) and for how long | `bcmtest.log`: `boot: BCM powered (the ROM's vmcs) at entry` (level 1 build); until then, the existing `boot path:` lines |
| 2 | **Hold MENU at boot** (Apple's firmware) with the boot path on | A photo of Apple's menu | none (Apple's firmware); then reset back |
| 3 | **Early swap**, toggle DMA (or a default build), normal boot | Report whether the dark interval falls inside the boot screen | `boot: BCM … at entry`, then `boot path: active: hello_dma (…), first frame N us`; the swap's dark interval ends before the menu first draws |
| 4 | **Boot time**, swap on vs forced Apple | Stopwatch from reset to menu, 3 times each | the ticks of the `boot path:` and shell start lines |
| 5 | **Forced boot failure**: "BCM boot path: fail next boot's every rung", then restart | A photo of the status screen after boot | `boot path: debug: the next 2 bootstrap(s) fail`, `… fell back to Apple's vmcs (bootstrap/upload: …); marker kept`; on the next boot `skipped: the last attempt did not finish (bcmfw.try)`. Re-arm by choosing a mode |
| 6 | **Bounded handshake at a wake**: "BCM boot path: next wake never answers", then let the backlight time out and wake it | Report: no freeze, a dark blink at most; status "Running: hello" (or Apple vmcs without a CPU rung) | at the idle flush: `boot path: fell back: level 1: next rung, our CPU-push image`, `… wake: host-port handshake never answered`, `… test fallback: no strike` |
| 7 | **CPU-push rung at boot**: needs a stage 2 pass flag (session 8 re-earns it); "BCM boot path: fail next boot's DMA rung", then restart | A photo of the status: "Running: hello" | `boot path: hello_dma failed (bootstrap/upload: …); next rung`, `boot path: active: hello (fallback rung); hello_dma failed`; status line 9 "Level 1: next rung Apple vmcs" |
| 8 | **ROLO path**, only if a build path still uses ROLO | Status photo | `boot: BCM unpowered (ROLO: Apple's vmcs loaded) at entry`, then the early swap's `active:` line |
| 9 | **A day of use** on the level 1 build (the power log on) | Owner report | no `fell back` lines; `strikes cleared after a clean run` |

**Recovery drills (session 9, owner's call):**
- **(a)** With the forced-failure entry armed, press MENU+SELECT during the swap's dark interval. The next boot must show `skipped: attempt marker present`.
- **(b)** Hold Next at boot and switch to Apple vmcs (forced).
- **(c)** Disk Mode reached and exited without changes. This is the read-only half of the ⚠️ in `install-and-restore.md`.

## 6. Beyond level 1

**Level 2: no Apple code loaded by ProkBox at all.**
- Drop the NOR vmcs from the fallback ladder once our DMA and CPU images have run for N unit-months without a fallback (strike logs from several units). The ladder becomes: DMA image → CPU-push image → black screen plus a reset hint. That's a real loss of safety, so it's the owner's call with data.
- In the same level, the install can ship without Apple's firmware on the disk: Method B's `osos` holds only the ProkBox bootloader (`install-and-restore.md`).
- Recovery then relies on the ROM's Disk Mode and a Mac-side restore. That has to be verified on a spare unit first.
- Apple's OF stays as the recovery path on the owner's unit (CLAUDE.md rule 5) until then.

**Level 3: no Apple code executes at all, which means the flash ROM.**
- As long as Apple's ROM bootloader runs at power-on, Apple code executes, and step 1 (§1) says it starts Apple's vmcs on the BCM before any of ours [M].
- Removing that means rewriting the NOR flash: the ROM bootloader, its `vmcs` copy, the diagnostics and Disk Mode.
- **This is the only step that can brick a unit beyond software recovery.** Disk Mode, the thing that recovers every other mistake, lives there.

**Preconditions, none met today:**
1. a spare 5G or 5.5G;
2. a hardware recovery path for the NOR (JTAG or the flash chip's test points, read and written from outside, proven by a read-back of the stock ROM first);
3. our own ROM bootloader with its own Disk Mode equivalent and a hold-key escape;
4. a complete, verified dump of the stock NOR kept off the device. It's Apple's code, so it's never committed.

Until then, level 3 is research only. #62's "no Apple loaders" is reachable for everything on the disk (levels 1–2), and for the ROM only with the hardware above.

**Order:** level 1 (sessions 8–9) → months of strike data → level 2 on a spare unit → level 3 on a spare unit with JTAG.

## Sources
- `firmware/target/arm/ipod/video/lcd-video.c`: `flash_get_section`, `bcm_init`, `lcd_init_device`, `lcd_sleep`/`lcd_awake`, `bcm_powerdown`.
- `firmware/target/arm/ipod/video/bcm-devtest-hal.c`: `bcm_boot_switch`, the marker and strikes (dev7 7242b4fbae).
- `bootloader/ipod.c`: the boot screen without an upload; hold MENU.
- Session logs:
  - `session log, session 7b bcmtest.log`: the boot-path block, stage 5's wake lines, the fallback test;
  - sessions 4–6 for the cold panel power-on.
- `pipeline.md` ("The everyday path"), `device-session-1.md` (recovery, Next at boot, known gap 11), `install-and-restore.md`, `artefacts.md`.
