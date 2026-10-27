# Brief: Phase 9, Hardware-specific trim and tuning (reversible flags; build variants)

*Orchestrator brief, drafted 2026-10-09, read-only, against `main` `cc44351e19` and branch `p11` (`ec1e371a51`, worktree `agent-aea6de579fe54aa27`). Phase 11 merges to `main` before Phase 9 fans out. **Every number marked (m) was measured on the p11 build** `build/fw-ui` (built 2026-10-09 18:24, `--target=22 --type=n --ipodui`, devmode on), with `arm-none-eabi-size -A`, `nm --size-sort`, and a reachability pass over the linked ELF described in §3.1. Process: `docs/process.md` (brief template, checklist 1–13, lean rules 1–11, one gate per machine). Anything still open is frozen as ⚠️ with a default (§10). The brief is not revised mid-phase for owner answers.*

*Revision 1 (2026-10-09, brief review): applies findings H1–H4, M1–M8 and L1–L9 inline; the closing "Revision 1" table maps each finding to its resolution and sections. Headline numbers are re-baselined to the latest p11 build: `rockbox.ipod` **1,385,064 B**, `_end` **1,957,056 B** (orchestrator figure; this reviser's reading of the 19:38 p11 `build/fw-ui` is 1,385,376 / 1,957,360, within 312 B). Per-section and per-object figures below are still from the 18:24 build; they move by less than the rounding of every projection. `main` does not yet contain p11, so the orchestrator's base measurement on merged `main` (§5) stays the reference.*

**Goal.** Make the shipped firmware contain only what the shell calls, built for one hardware combination at a time:
- `rockbox.ipod` comes down from **1,385,064 B (m)** to **≤ 760,000 B**. The budget ceiling is 914,813 B.
- Roughly **2.2 MB** goes back to the audio buffer (§3.2).
- The five per-unit facts (storage, RAM, battery, dock, haptics) become `tools/configure` flags with the owner's unit as the default.
- Playback codecs are untouched (mission). Every cut is a reversible flag. Videos stays possible.

**Two parts, split by risk and by when device data exists:**
- **Part 1, Trim (wave 1).** Binary and RAM cuts, variant flags, and runtime hard-wiring that changes nothing the user sees. It needs no device data. It merges to `main` on its own (one integration gate). The owner can then install it firmware-only by the same route as tonight's P11 install, so every Part 2 measurement runs on the trimmed build.
- **Part 2, Tune (wave 2).** Buffering and fill policy, boost audit, power instrumentation, battery curve hook, LCD sleep. It lands the mechanisms with ⚠️ frozen parameters, and P10 measurements set the values (§9, handoffs H1–H8). It is a second, smaller integration with its own gate.

---

## 0. What already exists (measured; reuse it, do not rebuild)

| Fact | Where / number | P9 does |
|---|---|---|
| **Size today** | `fw-ui` `rockbox.ipod` **1,385,064 B** (m, latest p11; 1,381,860 at 18:24, whose sections were `.text` 845,792, `.rodata` 490,804, `.data` 14,452, `.iram` 9,748, `.init` 20,984). Stock `fw` 1,154,480 B. Budget **914,813 B** (`docs/hardware.md`) | Cuts to ≤ 760,000 B (§7.2) |
| **Resident core** | `_end` (`.audiobuf`) **1,957,056 B** (m, latest p11; 1,953,824 at 18:24) in the shell build vs 1,665,072 B stock. `.bss` 598,488 + `.ncbss` 4,280 (18:24) | Target ≤ 1,100,000 B (§7.3) |
| **The shell itself** | `apps/ipodui/**` reachable: **274,144 B** image, 4,380 B bss (m). Largest: `library.o` 34,608, `panels.o` 26,113, `artcache.o` 24,032, `np_draw.o` 20,202, `nowplaying.o` 19,208 | Not trimmed in P9, except one duplicate (below) |
| **`--gc-sections` is already on** (ROADMAP says "currently off") | `lib/fixedpoint/fixedpoint.make` sets `CORE_GCSECTIONS := yes`, and `tools/root.make` then adds `-Wl,--gc-sections` to the core link. Without `-ffunction-sections` it can only drop whole unreferenced input sections. Today it drops **84,541 B** (m): `rockboxlogo` 62,744, `usblogo` 16,920, `folder_select` 2,617, `usb_keymaps` 788, … | Adds per-function sections for the core (§3.3.1) |
| **What keeps dead UI alive** (the keystones) | Reference paths traced in the ELF (m):<br>1. `rockbox_api` (`apps/plugin.c`, the plugin API table, 1,676 B) points at hundreds of stock functions: root menu items, `rockbox_browse`, `kbd_input`, `option_screen`, `yesno_pop`, `simplelist`, `browse_id3`, USB HID, …<br>2. `main.c` init calls that still run in the shell: `tree_mem_init`, `filetype_init`, `shortcuts_init`, `tagtree_init`, `sb_skin_init`, `gui_sync_skin_init`, `settings_apply_skins`, `iap_setup`, `talk_announce_voice_invalid` (`main.c` :443, :514–521, :825–861).<br>3. devmode `do_menu` → `list_stop_handler` (`misc.c`) → bookmarks.<br>4. `settings_list.c` callback pointers.<br>5. `SERIAL_ISR` → `iap_getc` | Each is cut under a trim macro (§3.3) |
| **Shell → stock UI calls** | `apps/ipodui` calls exactly one stock UI entry point, `do_menu()` in `devmode.c`. No `plugin_load`, splash, keyboard, `rockbox_browse` or `gui_wps_show`. `root_menu()` is unreachable in the shell build (m) | Confirms the stock UI layer can go |
| **Boot allocations nobody uses** | `tree_mem_init()` allocates `max_files_in_dir` × (40 + 20) = 5,000 × 60 = **300,000 B** at every boot (`tree.c`). Skin buffers and backdrops come from `settings_apply_skins()` (153,600 B per backdrop when a theme has one). With `HAVE_DIRCACHE` compiled in, tagcache refs cost 12 B/track: **~480 KB at 40k tracks** even with dircache off (`docs/hardware.md`). Playlist refs: 2,000 × 12 = 24,000 B | Removed (§3.4) |
| **Plugin slot** | `PLUGIN_BUFFER_SIZE` 0x80000 (**524,288 B**) is reserved at the top of DRAM (`app-pp.lds` `DRAMSIZE`). Nothing in the shell loads plugins. Upstream borrows it as scratch through `plugin_get_buffer()`: `cuesheet.c` (`struct cuesheet` ≈ **73.2 KB**), `bookmark.c` (list UI), `fileop.c`, `skin_parser.c`, `playlist_viewer.c`, `tagtree.c`, `gui/folder_select.c`. Source check (Rev 1, M8): every one of these callers sits in a trimmed file or a trimmed path (`display_cuesheet_content` is called only from `filetree.c`; `fileop.c` only from `onplay.c`; `tagtree.c` :2501 is outside the kept stats handlers) | Sized by `reach.py`'s live-caller list (§3.3.2): **0 B (+524,288 B) expected**, else the largest live caller (80 KB for a cuesheet, +442,368 B). Full 512 KB with `--ipodui-plugins` |
| **iAP wakes the CPU 10×/s on a dock-less unit** | `iap-core.c` `iap_setup()`: `timeout_register(&iap_task_tmo, iap_task, MS_TO_TICKS(100))` posts `IAP_EV_TICK` to `iap_thread` forever. Statics: `serbuf` 65,672 B + `thread_stack` 6,144 B. The callback runs inside the 100 Hz tick the CPU takes anyway, so the measurable saving is RAM and code, not battery (Rev 1, L5) | Gone under `!HAVE_IPOD_DOCK30` |
| **Dock-only power rails left on** | `accessory_supply` and `lineout_active` both default **true** (`settings_list.c` :1489, :1493). `lineout_set()` → `audiohw_enable_lineout()` powers WM8758 OUT2 (the dock line-out; the headphone jack is OUT1). `accessory_supply_set()` drives the PCF50605 3.3 V accessory regulator (`D2REGC1`) | OUT2 off under `!DOCK30`. Accessory rail stays on (O5 answered: default), and stays **independent of `_IAP`** (§3.4) |
| **Unit facts hard-coded for every build** (against rule 7) | `firmware/export/config/ipodvideo.h` vs `upstream/master`: `HAVE_FLASH_STORAGE` replaces `HAVE_DISK_STORAGE`, `HAVE_ATA_POWER_OFF` and `HAVE_ATA_SMART` are commented out, `ATA_MAX_UDMA 4` is added, and `BATTERY_CAPACITY_DEFAULT{,_THIN,_THICK}` are all 3000 (upstream 400/400/600) | Moved behind variant flags. Defaults reproduce today's file byte for byte |
| **Existing flags** | `tools/configure` has `--ipodui`, `--ipodui-release` (no devmode) and `--dock30` (`HAVE_IPOD_DOCK30`, picks only the USB glyph in 8a). The gate builds `fw-ui fw sim-ui sim boot boot-ui fw-rel` (`scripts/validate.sh` :207–282) | Add the variant flags. `--dock30` gains its compile-out (#54 part 2) |
| **RAM probe** | `crt0-pp.S` writes 64/32 to two banks; `system-pp502x.c` reads `probed_ramsize`. Branches: `core_alloc.c` :66 (subtracts 32 MiB from `audiobufend`), `battery-video.c` :28 (thin/thick capacity), `debug_menu.c` :1064 (display). `MEMORYSIZE` also drives `MMAP_MASK` in `crt0-pp.S` and `DRAMSIZE` in `app-pp.lds` | `--ram=32` builds a real 32 MB image (no probe). `--ram=64` keeps one boot-time branch as a mis-install guard |
| **LCD** | `lcd-video.c` has **no panel-type probe** (unlike `lcd-color_nano.c`'s `lcd_type`). Only the BCM state machine and the `vmcs` flash lookup for LCD sleep | Nothing to hard-wire. Recorded so nobody looks again |
| **Codecs** | 43 `.codec` files (m): **39 decoders + 4 encoders** (`aiff_enc`, `wav_enc`, `wavpack_enc`, `mp3_enc`). `codec_api` has `HAVE_RECORDING` members, so codec binaries change when recording goes; they ship with the core in `.rockbox` | Decoder set unchanged (39). Only the 4 encoders go |
| **Plugins** | 168 `.rock` files built (m), including `apps/plugins/mpegplayer/mpegplayer.rock`, which is **the Videos path** | Not built by default. `--ipodui-plugins` brings them back |
| **COP** | Every core thread is created `IF_COP(, CPU)`: audio, buffering, codec, playlist, artcache (`artcache.c` :3765), iAP, piezo. Only the MP3 codec (`mpa.c` :336) and SPC put work on COP. `cop_main` otherwise sleeps | No COP move in P9 (⚠️ P5) |
| **Buffering** | Flash path watermark is `seconds = 1` plus bitrate (`playback.c` `audio_update_filebuf_watermark`). Reads are `BUFFERING_DEFAULT_FILECHUNK` **32 KB** (`buffering.c` :70), so a 58 MB refill is ≈ 1,800 `read()` calls. **Every handle fill is boosted** (`buffering.c` :651 `trigger_cpu_boost()`, released at idle :1592/:1597). While any handle still wants data the thread waits with a 1-tick timeout (`queue_wait_w_tmo(..., filling ? 1 : HZ/2)`, :1592), so a handle that is never "full" spins boosted. Separately, ATA at UDMA > 2 boosts **per DMA transfer** (`firmware/target/arm/pp/ata-pp5020.c` :185–187 on, :248–250 off) PCM buffer watermark 2 s, minimum 3 s (`pcmbuf.c` :81–82) | Part 2 (§3.6) |
| **Battery** | `powermgmt-ipod-pcf.c`: `IPOD_VIDEO` discharge table "30GB Li-Ion 400mAh" 3600…4180 mV, shutoff 3300 mV, disk-safe 3500 mV | Part 2 hook. Values come from P10 |
| **LCD sleep** | `LCD_SLEEP_DEFAULT` 90 s in the shell (`settings_list.c` :1081, 8a #51) | Part 2 measures it, P10 decides |
| **Core IRAM** | `.ibss` 27,300 + `.iram` 9,748 + idle 256 + stack 8,192 = **45,496 / 49,152 B** (m), 3,656 B free. Voice holds ≈ 4,464 B of it (`static-ram-audit.md` §3) | Voice cut frees ≈ 4.4 KB |
| **Duplicate rodata** | `panelart.h` defines `static const` arrays that are included by both `about.c` and `panels.c`: `panelart_wheel_rgb` 4,568 B ×2 and `panelart_wheel_a` 505 B ×2 (m) | Moved to one TU (−5,073 B) |
| **Thumb** | `docs/code-audit-1.md` §2.3: whole-firmware `--thumb` **−162,496 B `.text` (−23.9 %)**, shell −29.5 %. ARM7TDMI Thumb is 10–30 % slower on compute. `tools/thumb-cc.py` today is all-or-nothing | Adds a cold-list mode, off by default (⚠️ T6) |

---

## 1. Owner pre-flight

**Already decided (not reopened):**
- Every playback codec stays. Only recording and its encoders may go.
- Videos may return later: hide, never make impossible.
- No upstream files deleted.
- Developer mode is minimal: debug screens, plus USB and recovery (ROADMAP "Architecture decisions").
- The language system stays (COMP-6, rule 8).
- The dock connector is gone on this unit (owner 2026-10-06).
- Battery goal: match or beat Apple firmware 1.3.
- Build variants are picked by the installer (`docs/install-and-restore.md`).
- No skin engine in the shell (2026-10-06).

### 1.1 Question list (one round; each default is frozen in §10)
- **O1 (TRIM-1).** Once the stock settings UI is compiled out, how do Rockbox-only tunables change (crossfeed, ReplayGain mode, watermarks)? *Default: by editing `.rockbox/config.cfg` over USB, read at boot. No new UI.*
- **O2.** Do you rely on any Rockbox plugin before P12 lands (games, clock, text viewer, `battery_bench`)? *Default: none. Plugins are compiled out. Videos and plugins come back with `--ipodui-plugins`. P10 rundowns use the new devmode power log (§3.6.5), not `battery_bench`.*
- **O3.** Which build runs on your 5.5G day to day during P9 and P10? *Default: the devmode build (debug screens via the About chord), until the bootloader install.*
- **O4 (captures from tonight's install, not decisions).**
  - Read devmode → Debug → the buffer/memory screen: confirm 64 MB, and note the buffer size and free RAM.
  - Note the time from the boot screen to the main menu.
  - Run DEV-1 (FAT32 cluster size: `diskutil list external` and `minfo`).
  - *Default until read: 64 MB, 32 KB clusters.*
- **O5.** Does the USB-C board or the haptic mod draw power from the 30-pin **accessory 3.3 V** pin? *Default: the accessory supply stays on (stock behaviour) until you confirm it doesn't. The dock line-out stage (OUT2) is switched off either way, since its pins are gone.*
- **O6.** Rundown method for the battery goal. A full rundown of a 3000 mAh cell at the 5.5G's ~30 mA playback draw is roughly **4 days per firmware** (estimate). *Default: tuning uses partial 24 h A/B runs (same playlist, fixed volume, screen off, compare voltage drop and log), and there is one full rundown each for Apple 1.3 and the final P9 build.*
- **O7.** CI: is a Forgejo Actions runner available now, or does the matrix wait for GitHub? *Default: wait for GitHub. A local `scripts/variants.sh` runs the pairwise set at integration, and the workflow file is committed but not enabled.*

---

### 1.2 Owner answers (2026-10-09)
**All defaults accepted** (O1–O7). The owner confirms the unit has **64 MB**. The other O4 captures (buffer size, boot time, cluster size) come from the device after tonight's firmware-only install. Provisional choices stay compile-time constants with no runtime cost (as in the P11 rule).

---

## 2. Scope

**In, Part 1 (Trim):**
1. Per-function sections for core objects under `--ipodui` (codecs and plugins untouched), with `--gc-sections` (already on).
2. Trim macros, one per reversible cut (§4.1): recording plus encoders, radio/RDS, iAP and serial (`!DOCK30`), USB HID/UAC/USB-iAP, voice, skin engine and WPS, stock menus, file browser family, tagtree, keyboard/morse, quickscreen/pitch/hotkey, AB repeat and the car-adapter tick, dircache, utf8proc (replaced by a compact NFC composer behind the new `HAVE_UTF8_NORMALIZE` guard), plugins (slot sized by `reach.py`'s live callers: 0 B expected). Kept out of the cuts: tagtree's runtime-stats and resume handlers (H1) and the tagcache halves of `tree_flush`/`tree_restore` (H2).
3. Statics: lazy `codepage_table` (64 KB) and a `language_buffer` sized to the loaded `.lng`, both unmovable boot-time allocations made before audio starts (§3.3.2); the `panelart` duplicate.
4. Variant flags in `tools/configure` (§3.5), moving today's hard-coded unit facts behind them.
5. Runtime hard-wiring (§3.4): the RAM probe, battery capacity, dock rails, iAP timer.
6. The Thumb cold-list mechanism (off by default).
7. Tooling: reachability and size reports, the trim-revert check, the variant runner, boot-allocation dump, `run_rock`.

**In, Part 2 (Tune):**
1. Chapter-aligned fill for spoken tracks (mechanism plus host-tested core).
2. Per-variant fill constants (watermark, chunk), wired with today's values.
3. `cpu_boost` audit: a report plus fixes in shell files.
4. Devmode power log (CSV) for rundowns and the curve.
5. Battery table hook per variant.
6. LCD sleep and refill-boost measurement hooks.

**Out (named so nobody builds it):**
- Removing or shrinking any decoder codec, or `CODEC_SIZE` (stays 1 MiB: it holds the codec image **plus** the codec's working buffer, and `kss` alone needs 443,896 B).
- Deleting upstream files. LTO.
- Moving threads to COP (⚠️ P5).
- Voice-file support. Any user-visible change: Part 1 must change **zero product-screen** goldens. The only expected recaptures are the devmode goldens that show stock lists, which draw unthemed under `_SKIN` (§7.1).
- The installer (#43). The bootloader install.
- The shell's own lean backlog (`code-audit-1.md` F1–F13: separate quick wins).
- `AB_MAX_CHAPTERS` and the packed chapter table (#48).
- Cover Flow texture budget (P7). Final re-trim after P12/P13 (9b).

---

## 3. Design

### 3.1 How the savings were measured (and how agents re-measure)
- **Image size by subsystem:** a parser over `build/fw-ui/rockbox.map`'s "Linker script and memory map" section attributes every input section to its object.
- **Reachability:** a graph over the linked ELF.
  - Nodes are symbols.
  - Edges: ARM `b`/`bl` targets; `add/sub rd, pc, #imm` (ADR); any aligned 32-bit word equal to a function start; for data, any word pointing into an object's input section. Section anchors at `-Os` hide which static is used, so the whole section counts as reached.
  - Linker veneers are included (`.init` → `.text` calls go through them).
  - Roots: `crt0-pp.o`, `main`, `irq_handler`, `fiq_handler`.
  - **A cut** removes a subsystem's objects and counts what else becomes unreachable. This over-approximates reachability, so the savings are conservative.
- T turns both into `scripts/sizemap.py` and `scripts/risk/reach.py` (§5), so 9b re-runs them after P12/P13 add callers.

### 3.2 Ranked candidate savings (m = measured on the p11 ELF; e = estimate)
Image Δ is the `rockbox.ipod` reduction; all of it is resident DRAM too. bss Δ is resident only. Rows are not additive. The combined line is the measured joint cut.

| # | Cut (macro) | Image Δ | bss Δ | Other | Evidence (largest pieces) |
|---:|---|---:|---:|---|---|
| 1 | utf8proc → compact NFC (`IPODUI_TRIM_UTF8PROC`) | **−340,676** (m), + ≤ 8,192 tables (pairs + combining classes, e) | −8,192 `normbuf` (inside the joint Σ below; not subtracted again) | faster tag normalisation (§6.3) | `utf8proc_properties` 201,240, `stage2table` 92,672, `sequences` 25,922, `stage1table` 8,704, `combinations_*` 2×3,844 |
| 2 | Skin engine + WPS + skinned bar (`_SKIN`) | −55,018 (m) | −1,481 | boot skin buffers, backdrops 153,600 each | `skin_parser.o` 11,456, `skin_tokens.o` 9,780, `skin_render.o` 5,552, `libskin_parser` 4,912, `skin_display.o` 4,732, `wps.o` 4,000 |
| 3 | Stock menus + root menu (`_STOCK_MENUS`; `menu.c` stays for devmode) | −51,064 (m) incl. `menu.o` 3,884 → **≈ −47,000** | −732 | | `apps/menus/*`, `root_menu.o` 2,896, `eq_menu.o` 4,612, `settings_menu.o` 3,156 |
| 4 | Voice/talk + speex (`_VOICE`) | −37,156 (m) | −8,124 | **IRAM ≈ −4,464** (audit) | `talk.o` 8,616, `nb_celp.o` 5,048, `sb_celp.o` 3,360 |
| 5 | iAP + serial (`_IAP`, `!DOCK30`) | −34,876 (m) | **−74,230** | 10 Hz wake gone; 1 thread | `iap.o` 11,584, `iap-core.o` 6,140, lingo4 4,040, lingo3 3,580; `serbuf` 65,672 |
| 6 | onplay, playlist viewer/catalog, shortcuts (`_BROWSER`) | −29,762 (m) | −4,147 | | `onplay.o` 9,536, `playlist_viewer.o` 6,428, `shortcuts.o` 4,438 |
| 7 | USB HID/UAC/USB-iAP (`_USB_CLASSES`) | −28,896 (m) | −895 | IRAM ≈ −800 (audit) | `usb_audio.o` 4,117, `usb_hid.o` 3,397 |
| 8 | Recording + encoders (`_RECORDING`) | −20,703 (m) | −625 | IRAM −116; 4 `*_enc.codec` | `pcm_record.o` 6,342, `recording.o` 4,752, `peakmeter.o` 4,417 |
| 9 | tagtree (`_TAGTREE`) | −14,779 (m), less the kept stats/resume handlers and their registration (≈ +1,000, e; H1) | **−66,658** | | `tagtree.o` 13,511; `uniqbuf` 65,536 |
| 10 | File browser (`_BROWSER`) | −14,015 (m), less the tagcache halves of `tree_flush`/`tree_restore` moved to `misc.c` (≈ +300, e; H2) | −5,624 | **−300,000 boot alloc** | `tree.o` 6,171, `filetree.o` 4,196, `filetypes.o` 2,880 |
| 11 | Radio/RDS (`_RADIO`) | −12,157 (m) | −534 | | `presets.o` 4,040, `radio*.o` |
| 12 | dircache (`_DIRCACHE`) | −10,773 (m) | −2,404 | **12 B/track tagcache refs (~480 KB @ 40k)**, playlist refs −24,000, 1 thread | `dircache.o` 9,257 |
| 13 | Plugin API (`_PLUGINS`) | −9,276 (m) alone (the keystone: rows 2, 3, 6, 10, 14 depend on it) | −884 | **slot 524,288 → 0: +524,288** expected (no live `plugin_get_buffer` caller, M8); 81,920 (+442,368) if `reach.py` finds a live cuesheet caller | `plugin.o` 3,424, `open_plugin` |
| 14 | Keyboard/morse (`_KEYBOARD`) | −7,615 (m) | −2,209 | | `keyboard.o` 7,194 |
| 15 | Quickscreen/pitch/hotkey (`_QUICKSCREEN`) | −2,632 (m) | | | |
| 16 | Per-function sections (no source change) | −14,618 (m) unreferenced, **minus anchor loss** (e: `-fdata-sections` disables section anchors; measure ±) | | | `root_menu` 1,472, `debug.o` string tables 1,588, `np_placeholder_render_glyph` 812, `__aeabi_dsub` 788, `pitch_xcorr` 340, … |
| 17 | Secondary `codepage_table` static removed (upstream fallback; §3.3.2, corrected in Rev 1) | 0 | **−65,538** | | `unicode.o` |
| 18 | `language_buffer` sized to the loaded `.lng` (R4) | 0 | −18,800 net (e): the 38,214 B static goes, an unmovable boot allocation of the `.lng`'s size (≈ 19.4 KB English) comes | | 38,214 B (m) today |
| 19 | `panelart` duplicate | −5,073 (m) | | | §0 |
| 20 | Thumb for cold code (#45), opt-in | −100,000…−160,000 (e) | | 10–30 % slower where applied | audit §2.3 |
| **Σ** | **Devmode build, rows 2–15 + 1 + 12, joint cut (m)** | **−650,098** | **−168,564** | `.iram` image −1,150, `.init` −1,208 | |
| **Σ** | **Release build** (also `menu.o`, `debug_menu.o`, `devmode.o`) | **−678,695** (m) | −168,980 | | |

**Projected result** (Rev 1: re-based on the latest p11 build; `normbuf` counted once, inside the joint Σ; the NFC tables, the `panelart` cut and the kept H1/H2 code included):
- Devmode `rockbox.ipod` ≈ 1,385,064 − 650,098 − 1,150 − 1,208 + 8,192 (NFC) − 5,073 (`panelart`) + 1,300 (kept stats handlers and flush/restore, e) ≈ **737,000 B**, before section-GC gains and with no Thumb.
- Release ≈ 1,385,064 − 678,695 − 1,150 − 1,208 + 8,192 − 5,073 + 1,300 ≈ **708,000 B**.
- Resident `_end` ≈ 1,957,056 − 650,098 − 168,564 − 65,538 (`codepage_table`) − 18,800 (language, net) + 8,192 − 5,073 + 1,300 ≈ **1,059,000 B** (≈ 1.05 MB; the 1,043,000 in the draft subtracted `normbuf` twice and left out the NFC tables and `panelart`).
- Audio pool gain ≈ 0.90 MB static + **0.52 MB slot** (0.44 MB if a cuesheet caller stays live) + 0.30 MB browser cache + ~0.48 MB dircache refs at 40k + 0.02 MB playlist refs + skin buffers ≈ **2.2 MB** at the owner's library size.

### 3.3 Part 1 (a): binary trim

#### 3.3.1 Function and data sections
- **Flags.** Under `--ipodui` only, configure adds `-ffunction-sections` for **core objects**: everything linked into `rockbox.elf` (`apps/`, `firmware/`, core `lib/*`). The flag goes through a core-only variable, the way `fixedpoint.make` adds its own. Codec and plugin objects keep today's flags, because the codec IRAM window is full (FLAC 81,008 / APE 81,104 of 81,920 B) and padding changes there are not worth the risk.
- **`-fdata-sections` is measured separately.** It disables GCC's section anchors, so code accessing statics may grow. Keep it only if the A/B shows a net gain (⚠️ T1).
- **`app-pp.lds` audit (the IRAM watch):**
  - The patterns `*(.text*)`, `*(.rodata*)`, `*(.data*)`, `*(.bss*)` already absorb `.text.<fn>` and the like.
  - Explicit-section attributes (`ICODE_ATTR` → `.icode`, `IBSS_ATTR` → `.ibss`, `INIT_ATTR` → `.init`, `.ncdata`/`.ncbss`, `.qharray`) keep their exact names under `-ffunction-sections`. IRAM placement doesn't change, and GC granularity there stays one section per TU.
  - `*(.init.text)` is matched by the `.text` output section before `*(.init*)`, and `start` (`ENTRY`) roots it. `.vectors` is already `KEEP`.
  - Anything referenced **only** from assembly or the linker script must be checked: `cop_main`, `irq_handler`, `fiq_handler`, `_initstart`/`_initend`, `audiobuffer`, the `"Rockbox"` osos signature in `crt0-pp.S`.
- **Review artifact (mandatory, Rev 1 L8):** `-Wl,--print-gc-sections` output, cross-checked by B against `reach.py` (built by T on opus). A section the linker drops that `reach.py` calls reachable **fails the hand-back**, not just the review. The cross-check is a script in B's branch (`scripts/risk/gc_check.sh`) whose output goes in the PR.
- **Boot safety:** the simulator doesn't use this linker script, so the **device is the first real boot test**. Recovery stays as today (hold MENU → Apple firmware; replace `rockbox.ipod` over Disk Mode).

#### 3.3.2 Trim macros (contract §4.1)
- **Where the macros act.** Upstream feature macros are wrapped in `ipodvideo.h` (`#ifndef IPODUI_TRIM_RECORDING` around `HAVE_RECORDING` and `INPUT_SRC_CAPS`, and so on). `config.h` gates `HAVE_DIRCACHE` on `!IPODUI_TRIM_DIRCACHE`. `apps/SOURCES` (preprocessed with `config.h`) wraps the stock files. `main.c` guards each init call.
- **The trims depend on each other (Rev 1, H3).** Bringing one stock layer back drags in the layers it calls, so "delete one line and it still builds" is false for four of them (`PLUGINS`, `SKIN`, `BROWSER`, `TAGTREE`). The graph is published in §4.1 (as `#error` checks in the contract header, plus `scripts/trim-deps.txt` for the tools). Removing a trim means removing its **dependency closure**.
- **The two makefile-level switches are derived, not duplicated (H3).** `tools/configure` today sets `no_utf8proc=1` and `plugins=""` by hand. B makes configure derive both from the same macros: after writing `autoconf.h`, configure runs the preprocessor over `ipodvideo-variant.h` with the build's defines and reads `IPODUI_TRIM_UTF8PROC` and `IPODUI_TRIM_PLUGINS` back (one `$CC -dM -E` call). Removing the header line therefore also restores `libutf8proc` and the plugin build, with no second edit. If B can't make the derivation work, `trim-revert.sh` edits both places and says so in its log.
- **Voice.** `talk.h` gets inline no-op stubs under `IPODUI_TRIM_VOICE`. `voice_thread.c` and `talk.c` drop out of `SOURCES`, and `VOICESPEEXLIB` isn't linked. The language system is untouched (rule 8).
- **Skin.** Under `IPODUI_TRIM_SKIN`, `viewport.c`'s theme hooks become no-ops, so devmode lists draw unthemed (architecture decision; the devmode goldens are recaptured, §7.1). `sb_skin_*`, `gui_sync_skin_init` and `settings_apply_skins` are compiled out. Kept files that call into the skin engine are guarded by A: `apps/menu.c` (`skinlist_set_cfg` :807) and `apps/debug_menu.c` (`dbg_skin_engine` :2961).
- **Tagtree: the stats and resume handlers stay (Rev 1, H1).** `tagtree_init()` (`main.c` :443) registers two playback events that the shell depends on:
  - `tagtree_buffer_event` (`tagtree.c` :934) loads `playcount`, `rating`, `lastplayed`, `playtime`, `lastelapsed`, `lastoffset` into the `mp3entry` and sets `id3->tagcache_idx`;
  - `tagtree_track_finish_event` (:1002–1090) writes `playcount`, `playtime`, `lastplayed` (runtimedb) and `lastelapsed`, `lastoffset` (autoresume, which the shell also turns on per track via `tagcache_ui_resumes()`, as in the buffer event), and returns early without `tagcache_idx`;
  - registration at :1473–1474, removal at :1398–1399.
  - The shell reads these values in `library.c` :2056, :2218, :3690, and `lib_book_seed()` (:5262–5299) is built on them. Without them, play counts, Recently Played, podcast "unplayed" dots and book resume all stop updating.
  - **How:** a trimmed `tagtree.c`, not a new file. Under `IPODUI_TRIM_TAGTREE` everything in `tagtree.c` is compiled out **except** the two handlers, their helpers, and a minimal `tagtree_init()` that only does the two `add_event` calls (no `tagnavi.config` parse, no `uniqbuf`, no menu). The stock build sees no change in the file, so stock byte identity holds and no code is duplicated. `tagtree.c` stays in `SOURCES`.
  - Listed in checklist 12. Script `ipodui-p9-stats-playcount`: play a short track to its end, then assert `playcount` +1 and a fresh `lastplayed` for that track in the database (`sim_db` dump).
- **Browser.** `tree_mem_init()` is gone. `playlist.c`'s directory paths that call `tree_get_context()` (`get_next_dir`, `create_and_play_dir`, `playlist_directory_tracksearch`) are compiled out under `_BROWSER`, and `next_folder` is forced off (it defaults to `FOLDER_ADVANCE_OFF`; the setting is hidden in the shell). A must show no shell path reaches them.
- **Browser: `tree_flush` and `tree_restore` are not browser code (Rev 1, H2).** `tree.c` also holds the USB and shutdown hooks that `misc.c`'s `system_flush()`/`system_restore()` call (`misc.c` :352, :359; reached from :465, :691, :703, :745):
  - `tree_flush()` (`tree.c` :1344): `tagcache_shutdown()` (flushes queued statistics and ratings to disk), `tagcache_unload_ramcache()`, and the dircache suspend;
  - `tree_restore()` (`tree.c` :1393): `tagcache_remove_statefile()`, the dircache resume, and `tagcache_start_scan()`, which is the post-USB `Q_START_SCAN` that runs the deleted-file and new-file scans.
  - **How:** under `IPODUI_TRIM_BROWSER`, `misc.c` gets two static functions, `system_flush_db()` and `system_restore_db()`, holding exactly those calls (including the `HAVE_DIRCACHE` parts, so reverting `_DIRCACHE` alone still builds; the "Scanning disk..." splash becomes a `DEBUGF`, rule 1). `system_flush()`/`system_restore()` call them instead of `tree_flush()`/`tree_restore()`. Stock keeps calling `tree.c`. A owns both files.
  - Scripts: `ipodui-p9-usb-rescan` (USB connect, add an album to the sim disk, disconnect: the album appears in Albums) and `ipodui-p9-rating-persist` (rate a track, power off, reboot: the rating is still set).
- **Plugins.**
  - `plugin.c` and `open_plugin.c` go. `plugin_get_buffer()` keeps its name and signature in a 20-line `apps/scratchbuf.c`.
  - **Slot size from the live callers (Rev 1, M8).** `reach.py` lists every `plugin_get_buffer` caller still reachable in the default trim set. The source check (§0) expects **none**: then `PLUGIN_BUFFER_SIZE` is 0, `plugin_get_buffer()` returns `NULL` with size 0, and `DRAMSIZE` gains the whole **524,288 B**. If a caller is live, the slot is sized to the largest one (a cuesheet: 0x14000, 81,920 B, with the static assert). The size is a compile-time rule in `ipodvideo.h`: 0 while `_BROWSER`, `_SKIN` and `_TAGTREE` (the files holding every caller) are all trimmed, else 0x14000. So a trim revert that brings a caller back also brings its slot back. A checks that each caller already handles `NULL` (as `display_cuesheet_content` does).
  - **`--ipodui-plugins` (the Videos path)** drops `IPODUI_TRIM_PLUGINS` **and its dependency closure** (§4.1: `_BROWSER`, `_STOCK_MENUS`, `_KEYBOARD`, `_QUICKSCREEN`, `_SKIN`, `_VOICE`). It restores the 512 KB slot and builds the plugins, including `mpegplayer.rock`. The hardware-dead trims (recording, radio, iAP, USB classes, dircache, utf8proc, tagtree, misc) stay. It is a bigger build that reports its size and is not held to the ceiling (⚠️ T4).
- **Dircache.** The device's deleted-file path becomes the simulator's `file_exists()` branch, which **retires** the P10 check "Deleted files, dircache branch" for shell builds. The devmode option to turn dircache on goes away (⚠️ T3). The hdd variant also runs the deleted-file pass without dircache (one `file_exists()` per track, on a spinning disk); the orchestrator records that in `docs/hardware.md`'s "Build variants" section (Rev 1, L9). Kept file `apps/debug_menu.c` has dircache screens (`dircache_callback` :1904); A guards them under `HAVE_DIRCACHE`.
- **utf8proc → NFC (Rev 1, H4).**
  - **The guard.** Today `utf8_normalize()` and `unicode_init()` are declared and defined only under `UTF8PROC_EXPORTS` (`rbunicode.h` :78–83, `unicode.c` :658–686), and the only caller, `metadata.c` :489–501 (plus its `#include` at :30), uses the same guard. That macro comes from `lib/utf8proc/utf8proc.make` :20, so with `no_utf8proc` normalisation would **silently stop**. New guard **`HAVE_UTF8_NORMALIZE`**, set in `config.h` when `UTF8PROC_EXPORTS` or `IPODUI_TRIM_UTF8PROC` is defined (B owns that line). `metadata.c`, `rbunicode.h` and `unicode.c` test `HAVE_UTF8_NORMALIZE` instead. The stock build defines it exactly when it defines `UTF8PROC_EXPORTS` today, so its preprocessed output is unchanged. **U owns `metadata.c`** (that one guard and include) and `rbunicode.h`. A build-time `#error` in `unicode.c` fires if `IPODUI_TRIM_UTF8PROC` is set without `HAVE_UTF8_NORMALIZE`.
  - **What has to match.** The current call is `utf8proc_decompose(string, 0, normbuf, 2047, UTF8PROC_NULLTERM)` followed by `utf8proc_reencode(..., NULLTERM|COMPOSE|STABLE)` (`unicode.c` :671–673). Without `UTF8PROC_DECOMPOSE` the first call neither decomposes nor reorders (`utf8proc.c` :596 guards the reordering), so today's behaviour is **compose-only, no reordering**. That's what T2 already said, and it is now known to be exact. The compose pass (`utf8proc.c` :661–700) is what we must reproduce:
    - a mark composes with the last starter only if no character between them has a combining class **≥ its own** (the blocking rule, tracked as `max_combining_class`);
    - Hangul L+V and LV+T compose algorithmically;
    - a starter is any character with combining class 0, including ccc-0 second characters such as U+0B3E and U+0DCF, which themselves compose (U+0B47+U+0B3E → U+0B4B);
    - composition exclusions and singletons never appear as results (they are absent from utf8proc's pair data);
    - non-BMP pairs exist (U+11099 + U+110BA → U+1109A);
    - invalid UTF-8 makes `utf8proc_decompose` fail, and the string is left unchanged;
    - the buffer limit: `utf8proc_decompose` is given 2,047 slots. Up to 2,047 code points it composes normally. At 2,048 or more it returns the required length without writing past the buffer, but `unicode.c` then passes that length to `utf8proc_reencode()`, which reads and writes past `normbuf` (an upstream overrun, not "left unchanged" as the review assumed). It is unreachable with real tags: every normalised field lives in the 1,800-byte `id3v2buf` (`ID3V2_BUF_SIZE`, `metadata.h` :191), so no field exceeds 1,800 code points. `nfc.c` defines the case safely (string unchanged), and the golden compares up to 2,047 code points only;
    - the result is copied back only if it is no longer than the input (always true for composition).
    A pair table alone can't do this. `nfc.c` therefore carries a **canonical-pair table, a combining-class table** (ranges of non-zero ccc: start, length, class) and algorithmic Hangul, all generated by `tools/gen_nfc.c` from the in-tree `lib/utf8proc` data, nothing downloaded. Both tables together are ≤ 8,192 B, static-asserted; the generator prints the split.
  - **Algorithm.** ASCII fast path (no buffer, one scan). Otherwise a single in-place pass that mirrors utf8proc's compose loop: decode, look up ccc, try Hangul, then the pair table if not blocked, re-encode behind the read pointer (never longer). Validate the whole string first; any invalid sequence or ≥ 2,048 code points → unchanged.
  - **Golden: generated and exhaustive, byte identity required.** `tools/gen_nfc.c` also writes `tests/unicode/nfc_cases.txt`, and a host test runs both the in-tree utf8proc path and `nfc.c` on every case:
    - every composition pair, bare, and with one intervening mark of **every** combining class present in the data, both blocked (class ≥ the second's) and unblocked;
    - every Hangul L+V, LV+T, and L+V+T (all 11,172 syllables), plus non-composing Hangul neighbours;
    - the ccc-0 second characters (U+0B3E, U+0DCF and the rest the data lists), after their starters and after an intervening mark;
    - composition exclusions in decomposed form (e.g. U+0915 U+093C must not become U+0958), and singletons themselves (U+212B, U+2126, U+0340: compose-only never decomposes them, so they stay as they are);
    - non-BMP pairs (U+11099 + U+110BA and the rest);
    - invalid UTF-8 (truncated, overlong, surrogates, > U+10FFFF, stray continuation bytes);
    - 2,046 and 2,047 code points (byte identity), and 2,048 code points (`nfc.c` only: must be unchanged; the reference overruns, so it isn't run there);
    - the earlier mixed-script corpus (Latin, Greek, Cyrillic in NFD and NFC, emoji) as a smoke set.
    Any byte difference fails the test. **There is no `TAGCACHE_MAGIC` fallback**: a mismatch is a bug in `nfc.c`, fixed before hand-back.
- **Codepages (Rev 1, M5; mechanism corrected against the code).** The draft treated `codepage_table` as the default-codepage table. It isn't: the **default** table is already a `core_alloc` made by `set_codepage()` at boot (`unicode.c` :589–630, `default_cp_handle`). The 65,538 B static (:131–134) is the **secondary** table, loaded on demand inside `iso_decode_ex()` (:333–335) when a caller passes an explicit codepage whose table differs from the default's. Upstream's comment says why it is static: "cannot be bufalloced! playback itself may be making the load request". The only core caller that passes an explicit table codepage is `lib/rbcodec/metadata/smaf.c` :121 (`spc.c`/`sid.c` pass 0, ISO-8859-1, which needs no table; every other caller passes −1, the default). So a boot-time allocation keyed on `default_codepage` would cover nothing, and an on-demand allocation would happen while playing (checklist 2).
  - **Design:** under `IPODUI_TRIM_MISC` the static goes, and a request for a secondary table takes upstream's own load-failure path (`cp = INIT_CODEPAGE`, :336), logged once as `DEBUGF("p9 cp2 fallback cp=%d")`. The only visible effect is on SMAF (`.mmf`) files whose tags declare a non-default codepage: their tag text decodes as ISO-8859-1. The file still plays. This is the one user-visible edge in Part 1, and it needs a SMAF file with a declared CJK or ISO-8859-x codepage; it is named here so nobody calls it a regression. No allocation is ever made while playing, so the "pinned, before audio" requirement holds trivially.
  - **Default table:** unchanged upstream behaviour (allocated in `set_codepage()` at boot, before audio). It is movable today (`core_alloc_ex(size, NULL)`; buflib moves NULL-ops blocks freely). U confirms that `iso_decode_ex()` doesn't yield between `cp_table_get_data()` and its last table read (the decode loop has no yield), and otherwise pins it with `cp_table_pin()` for the decode. A devmode codepage change still applies at the next boot.
  - T7 keeps its intent (no 64 KB static for a case the owner's library never hits); its wording changes in §10.
- **Language buffer (Rev 1, M5).** `language_buffer` (38,214 B static, `language.c` :46, sized to the largest language by `max_language_size.h`) becomes an allocation of exactly the loaded language's `lang_size` (from the `.lng` subheader, `language.c` :93), made in `lang_core_load()` at boot, before audio starts. `str()` returns pointers into it that callers keep, so it is **unmovable** (pinned with `core_pin()` for its lifetime, or allocated with a move callback that refuses). With the built-in English and no `.lng` loaded, nothing is allocated. Every language still builds and still loads; none is dropped (rule 8). Loading a language later (devmode) is applied at the next boot, never while playing.

#### 3.3.3 Thumb for cold code (#45, opt-in)
- `--thumb-cold` adds a list mode to `tools/thumb-cc.py`: a source compiles `-mthumb` only if its path matches `tools/thumb-cold.txt`.
- **Initial list:** `apps/settings*.c`, `apps/misc.c`, `apps/screens.c`, `apps/debug_menu.c`, `apps/gui/list.c`, `apps/menu.c`, `lang/`, the shell's screen files (`apps/ipodui/screens/*` except `nowplaying.c` and `browse.c`), `about`, `legal`, `reset`, `datetime`, `sysscreen`, `usbscreen`.
- **Never Thumb:** `firmware/`, `lib/rbcodec/`, `buffering.c`, `playback.c`, `pcmbuf.c`, `tagcache.c`, `apps/ipodui/{library,sortkey,artfmt,artblur,np_draw,list,paint,search_core}.c`, `firmware/usbstack/usb_storage.c` (MSC throughput).
- Default **off**. P9 reports the size only. Turning it on is a P10/9b decision after the speed checks in §9 (⚠️ T6).

### 3.4 Part 1 (b): runtime hard-wiring (behind variant flags, with cheap fallbacks)
| Fact | Today | After |
|---|---|---|
| RAM 64 MB | probe at boot and a runtime subtract (`core_alloc.c` :66) | `--ram=64`: linked for 64; the one boot branch stays as a **mis-install guard** (a 64 MB image on a 32 MB unit still runs, via aliasing, as upstream does). This is an **accepted exception** to §1.2's "provisional choices are compile-time constants" (Rev 1, L4): it is not a provisional tuning choice but a safety net against the installer picking the wrong image, it costs one compare at boot and nothing afterwards, and it is listed in §10 (V2) so the owner round can drop it. `--ram=32`: `MEMORYSIZE` 32 (`MMAP_MASK`, `DRAMSIZE` correct); the branch compiles out |
| Battery capacity | `battery_default_capacity()` picks thin/thick by `probed_ramsize` | `IPOD_BATTERY_MAH` constant; `0` (`--battery=stock`) keeps upstream's 400/600 by model |
| Flash storage | hard-coded | `--storage=flash` (default) is today's values; `--storage=hdd` is upstream's (`HAVE_DISK_STORAGE`, `HAVE_ATA_POWER_OFF`, `HAVE_ATA_SMART`, default UDMA). The ATA identify parse stays (once at boot; it gives LBA48 and the DMA mode) |
| No dock | iAP thread, 10 Hz timer, serial ISR, `serbuf`; line-out and accessory rails on | `!HAVE_IPOD_DOCK30`: iAP and serial compiled out, `lineout_set(false)` at boot with the setting hidden. **Accessory rail stays on and stays independent of `_IAP`** (Rev 1, M4): `accessory_supply_set(global_settings.accessory_supply)` (`main.c` :850) is guarded only by `HAVE_ACCESSORY_SUPPLY`, and so is its setting and callback (`settings_list.c` :1489). A must not move either under `_IAP` or `_MISC`. A boot trace `p9 accessory=1` (`DEBUGF`, after the call) appears in every `!DOCK30` variant, and A shows (grep plus the `ipodui-p9-smoke-usb` and charging-sleep scripts' traces) that neither the USB path nor 8a's `charging_sleep_prepare()` ever calls `accessory_supply_set()`. `car_adapter_tick` (100 Hz tick task) out under `_MISC`; like the iAP post, it rides the tick the CPU takes anyway, so it is **not** credited towards the battery goal (L5) |
| Haptics | piezo pulse `piezo_play_for_usec(91, 0x80, 4000)` | `HAVE_IPOD_HAPTICS` selects `HAPTIC_TICK_*` constants in `piezo.c`, identical to the piezo values until P10 (#23, ⚠️ V4) |
| LCD | – | nothing (no probe exists; §0) |

### 3.5 Variant flags (configure) and the CI matrix
| Flag | Values (**default = owner's unit**) | Defines / effect | Applies to |
|---|---|---|---|
| `--storage=` | **`flash`** \| `hdd` | `hdd` → `-DIPOD_STORAGE_HDD` | all target-22 builds (the defaults keep the stock build identical) |
| `--ram=` | **`64`** \| `32` | configure `memory=` → `MEMORYSIZE` | all; the sim too (`core_alloc.c` sizes its pool from `MEMORYSIZE`, so `sim-ui --ram=32` exercises a 31 MB pool) |
| `--battery=` | **`3000`** \| `stock` \| `<mAh>` | `-DIPOD_BATTERY_MAH=<n>` (`stock` = 0); `BATTERY_CAPACITY_MAX` = max(3000, n); > 600 picks the large-cell table (§3.6.4) | all |
| `--dock30` | **off** | `HAVE_IPOD_DOCK30` (exists); off → `IPODUI_TRIM_IAP` | `--ipodui` |
| `--haptics` / `--no-haptics` | **on** | `HAVE_IPOD_HAPTICS` | `--ipodui` |
| `--ipodui-plugins` | **off** | `HAVE_IPOD_UI_PLUGINS` (§3.3.2) | `--ipodui` |
| `--thumb-cold` | **off** | §3.3.3 | `--ipodui` |
| (derived) | | `-DIPOD_VARIANT="\"flash-64-3000-usbc-haptic\""`, logged at boot (`DEBUGF`) and shown in devmode | |

**Matrix:** 2 storage × 2 RAM × 2 battery classes (large, stock) × 2 dock × 2 haptics = **32 variants**.
- **CI (GitHub later, ⚠️ O7):**
  - all 32 compile, with a size report each; any default-trim variant over 914,813 B fails;
  - `sim-ui` gate subset (boot, browse, play, USB, devmode) for the 4 storage × RAM combinations;
  - the full gate for the owner's variant only;
  - also the stock `fw` identity, `fw-rel`, `--ipodui-plugins` (size and `mpegplayer.rock` built) and `--thumb-cold` (size only).
- **Local (8 GB Mac, one gate per machine):**
  - the full gate gains one compile-only build, `fw-alt` (row 2 below), started after the parallel builds finish, so the memory peak doesn't grow;
  - `scripts/variants.sh` runs the **pairwise covering set** below at integration. Every pair of flag values appears in at least one build.
  - **Outside the gate lock, with its own budget (Rev 1, M7).** Five extra builds plus up to 13 trim-revert builds (§7.6) would blow the gate watchdog (`GATE_MAX_MINUTES`, 45). So `variants.sh` and `trim-revert.sh` don't take the gate lock and aren't part of `validate.sh`. They take their own lock (`~/.cache/rockbox-p9-batch.lock`), refuse to start while the gate lock is held, run one build dir at a time at `-j4` on the shared `ccache`, and stop themselves after `P9_BATCH_MAX_MINUTES` (default 120), logging which rows ran. The orchestrator runs them after the Part 1 integration gate passes, never at the same time as a gate. `trim-revert.sh` batches by dependency closure (§4.1), so trims with the same closure share one build.

| Row | storage | ram | battery | dock | haptics |
|---:|---|---|---|---|---|
| 1 (= `fw-ui`) | flash | 64 | 3000 | usbc | on |
| 2 (= `fw-alt`) | hdd | 32 | 3000 | usbc | off |
| 3 | hdd | 64 | stock | usbc | off |
| 4 | hdd | 64 | 3000 | dock30 | on |
| 5 | flash | 32 | stock | dock30 | off |
| 6 | flash | 32 | stock | dock30 | on |

### 3.6 Part 2 (c): tuning (✋ = needs device data; values frozen ⚠️ until P10)
1. **Dircache:** decided in Part 1 (compiled out). Nothing left to tune.
2. **Buffer watermarks and chunking.** Per-variant constants in one place (`playback.c` / `buffering.c`, guarded):
   - flash keeps today's 1 s plus bitrate, and the 32 KB chunk (⚠️ P2);
   - hdd keeps upstream's spin-up formula.
   - ✋ P10 measures the refill time and CPU at a 32 vs 128 vs 256 KB chunk (lean rule 11: fewer, larger reads; FAT merges contiguous 32 KB clusters into one ATA command).
3. **Chapter-aligned audiobook fill** (`docs/hardware.md` power rule 3):
   - **Predicate (checklist 13):** the playing track's audio handle, the track is **spoken** (the shell's audiobook or podcast class), and `struct audiobook_chapters` has ≥ 2 entries. **Songs are never affected.**
   - **Who computes, who applies (Rev 1, M3c).** The P6 chapter slots are **audio-thread only** (`np_playback.h` :216–229: "Audio thread only, except `np_pb_ab_seq()`"). So the buffering thread never reads them. The **audio thread** computes the stop offset with `fp_fill_stop()` (§4.5) whenever the playing track's chapters land, after a seek, and on each buffer-low watermark, and passes it in with a new per-handle call, `buf_set_fill_limit(handle_id, limit)` (`buffering.c`, queued to the buffering thread like the other `buf_*` requests). The buffering thread only compares the handle's write offset with `h->fill_limit` (default: no limit).
   - **Mechanism:** the fill stops at the limit, the estimated byte offset of the last chapter start that fits. The estimate is average bytes per ms, rounded down to 64 KB, so the codec's own seek always lands inside buffered data.
   - **Floor (M3a):** a fill never reclaims bytes at or after the **current chapter's start**. `fp_fill_stop()` takes the current chapter's estimated start offset (`floor_off`); if the room the fill needs could only come from freeing bytes at or after `floor_off`, the fill is clamped further (to what fits without them) or skipped. A seek back to the start of the current chapter never reloads.
   - **EOF (M3b):** whenever the rest of the file (`filesize − fill_from`) fits in the room, `fp_fill_stop()` returns `filesize`: no clamp. The handle completes, and the next file of a multi-file book (or the next track) starts buffering on time. Gapless multi-file transitions are tested (`ipodui-p9-fill-multifile`: a 3-file book, the transition at each boundary has no gap in the `pcmbuf` trace).
   - **No spin (M3d):** today a handle that still wants data keeps the buffering thread in a 1-tick wait, boosted (`buffering.c` :1592). A handle stopped by its limit is marked **satisfied** (`h->clamped`), so it counts as full: no `buffer_handle()` calls for it and no boost, until the next buffer-low watermark, when the audio thread recomputes and resets the limit. Acceptance: between a `fill stop=chapter` trace and the next watermark there are **zero** `buffer_handle` calls for that handle and no `cpu_boost` from buffering (traced).
   - **Guard:** never stop with less than 10 min buffered ahead (⚠️ P1). A seek inside a buffered chapter never reloads; a far seek accepts the ~1 s reload.
   - **Traces:** `fill stop=chapter idx=%d ahead_ms=%ld`, `fill stop=room`, `fill stop=eof`, `fill limit=%llu` (audio thread, when set).
   - Because this touches playback core, P gets a **brief-review delta before code** (process: split by risk).
   - ✋ Reads per hour of book playback, and stall-free listening.
4. **Battery curve.**
   - `percent_to_volt_discharge/charge` get a large-cell table under `IPOD_VIDEO && IPOD_BATTERY_MAH > 600`, equal to the upstream table until calibrated (⚠️ V3).
   - The disk-safe floor (3,500 mV) becomes a per-storage constant. Flash may not need it, since the last ~0.2 V is usable capacity (⚠️ V6).
   - ✋ A full discharge log from §3.6.5 sets both.
5. **Power log (devmode only).**
   - `powerlog.c`: a `timeout_register` sample every 60 s into a 4 KB RAM ring. No I/O in the callback.
   - Columns: tick, mV, level %, charging, backlight, LCD on, boost %, playing, codec.
   - Flushed by a storage-idle callback, so it **never wakes the disk** (lean rules 5, 11). Flushed at shutdown; at USB connect the flush happens before the acknowledgement.
   - File: `/.rockbox/ipodui/power.csv`. Off by default, toggled in devmode.
6. **`cpu_boost` audit.** A report covering every site:
   - shell: `library.c` 9, `anim.c` 4, `otg.c` 2, `search.c` 2, `usbscreen.c` 2, `nowplaying.c` 1, `artcache.c` 1;
   - core: `tagcache.c` 9, `buffering.c` 3, `playback.c` 3, `pcmbuf.c`, `codec_thread.c`.
   - The report checks lean rule 6 (never boosted on I/O) and fixes shell sites.
   - The upstream fill boost (`buffering.c` :651) gets a variant constant, `BUFFERING_BOOST_FILL`, default on (⚠️ P3). ✋ Refill time and current with it off at UDMA4.
   - **The ATA driver boosts too (Rev 1, L6).** At `ATA_MAX_UDMA > 2` (the flash variant's UDMA4), `ata-pp5020.c` boosts at the start of every DMA transfer and unboosts at its end (:185–187, :248–250). The audit lists it, and H4 measures refill time and current with both boosts, with only the ATA one, and with neither. No change to it in P9 (firmware driver, stock path).
7. **LCD sleep.** No code change. A trace `lcd sleep after=%d` plus the power log. ✋ Current lit vs backlight-off-LCD-on vs LCD asleep sets the default (⚠️ P4).
8. **COP offload.** Not in P9. Both cores share one clock, so COP work only saves energy if it shortens boosted time. Rockbox keeps cross-core data in uncached sections (`SHAREDBSS_ATTR`, `.ncbss`) because the caches aren't coherent, and buflib moves the art blocks. ✋ P10 measures art-builder busy time per fill window. The decision goes to P7/9b (⚠️ P5).

---

## 4. Contracts (real code; the orchestrator commits these on `p9` before fan-out)

### 4.1 `firmware/export/config/ipodvideo-variant.h` (new; included first by `ipodvideo.h`)
```c
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
#ifndef IPOD_VARIANT
#define IPOD_VARIANT "flash-64-3000-usbc-haptic"
#endif

#ifdef HAVE_IPOD_UI
/* One line per reversible cut. Deleting a line brings the feature back;
 * the build must still pass once the line's dependency closure (below,
 * and scripts/trim-deps.txt) is deleted with it (scripts/trim-revert.sh).
 * Never set without --ipodui. tools/configure derives no_utf8proc and
 * plugins="" from IPODUI_TRIM_UTF8PROC / IPODUI_TRIM_PLUGINS (cpp -dM). */
#define IPODUI_TRIM_RECORDING      /* recording, *_enc.codec, peak meter   */
#define IPODUI_TRIM_RADIO          /* remote tuner, RDS, presets           */
#ifndef HAVE_IPOD_DOCK30
#define IPODUI_TRIM_IAP            /* iAP, serial; dock line-out off       */
#endif
#define IPODUI_TRIM_USB_CLASSES    /* HID, HID mouse, UAC, USB-iAP; MSC stays */
#define IPODUI_TRIM_DIRCACHE       /* device path == sim path              */
#define IPODUI_TRIM_UTF8PROC       /* firmware/common/nfc.c instead        */
#define IPODUI_TRIM_TAGTREE        /* library.c replaced it (P4)           */
#define IPODUI_TRIM_MISC           /* AB repeat, morse, car-adapter tick,
                                      secondary codepage_table static     */
#ifndef HAVE_IPOD_UI_PLUGINS       /* the plugin API needs these back      */
#define IPODUI_TRIM_PLUGINS        /* slot 512 KB -> IPODUI_SCRATCH_SIZE   */
#define IPODUI_TRIM_VOICE          /* talk.h stubs                         */
#define IPODUI_TRIM_SKIN           /* skin engine, WPS, skinned bar        */
#define IPODUI_TRIM_STOCK_MENUS    /* root menu, apps/menus (menu.c stays) */
#define IPODUI_TRIM_BROWSER        /* tree, filetree, filetypes, onplay,
                                      playlist viewer/catalog, shortcuts  */
#define IPODUI_TRIM_KEYBOARD
#define IPODUI_TRIM_QUICKSCREEN    /* quickscreen, pitch screen, hotkey    */
#endif

/* Dependency graph (Rev 1, H3). "A needs B" = bringing A back (deleting
 * its line) also needs B back, because A's code calls into B:
 *   PLUGINS  needs BROWSER, STOCK_MENUS, KEYBOARD, SKIN, QUICKSCREEN, VOICE
 *                  (plugin.c's API table)
 *   SKIN     needs BROWSER, QUICKSCREEN   (the WPS: onplay, quickscreen)
 *   BROWSER  needs STOCK_MENUS, SKIN      (root_menu, skin; onplay is BROWSER)
 *   TAGTREE  needs KEYBOARD, BROWSER      (keyboard, onplay)
 * SKIN <-> BROWSER is a cycle: they come back together. The checks make an
 * incoherent set fail at compile time with a clear message instead of a
 * link error. T confirms the edges on the first trim-revert run and adds
 * any it finds (STOCK_MENUS' own edges are measured, not assumed). */
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
    defined(IPODUI_TRIM_SKIN))
#error "browser needs STOCK_MENUS and SKIN"
#endif
#if !defined(IPODUI_TRIM_TAGTREE) && (defined(IPODUI_TRIM_KEYBOARD) || \
    defined(IPODUI_TRIM_BROWSER))
#error "tagtree needs KEYBOARD and BROWSER"
#endif

/* Scratch slot (M8): every plugin_get_buffer() caller lives in BROWSER,
 * SKIN or TAGTREE code. reach.py confirms none is live; then 0. */
#if defined(IPODUI_TRIM_PLUGINS) && defined(IPODUI_TRIM_BROWSER) && \
    defined(IPODUI_TRIM_SKIN) && defined(IPODUI_TRIM_TAGTREE)
#define IPODUI_SCRATCH_SIZE 0
#elif defined(IPODUI_TRIM_PLUGINS)
#define IPODUI_SCRATCH_SIZE 0x14000   /* holds struct cuesheet (73.2 KB) */
#endif
#endif /* HAVE_IPOD_UI */
#endif /* IPODVIDEO_VARIANT_H */
```

`config.h` (B) adds, next to the `HAVE_DIRCACHE` gate (Rev 1, H4):
```c
#if defined(UTF8PROC_EXPORTS) || defined(IPODUI_TRIM_UTF8PROC)
#define HAVE_UTF8_NORMALIZE   /* utf8_normalize(): utf8proc, or nfc.c */
#endif
```

**Dependency closures** (what `trim-revert.sh` deletes for each line; it builds each distinct closure once):

| Trim line removed | Also removed (closure) | Build |
|---|---|---|
| `RECORDING`, `RADIO`, `IAP`, `USB_CLASSES`, `DIRCACHE`, `UTF8PROC`, `MISC`, `VOICE`, `KEYBOARD`, `QUICKSCREEN`, `STOCK_MENUS` | none known (leaves; T records any edge found) | 1 each (11) |
| `SKIN`, `BROWSER` | each other, `STOCK_MENUS`, `QUICKSCREEN` (+ `STOCK_MENUS`' edges) | 1 |
| `TAGTREE` | `KEYBOARD`, `BROWSER` and its closure | 1 |
| `PLUGINS` | `BROWSER`, `STOCK_MENUS`, `KEYBOARD`, `SKIN`, `QUICKSCREEN`, `VOICE` (= the `--ipodui-plugins` build) | 1 (shared with criterion 5) |

That is 13 distinct builds (the plugins closure doubles as the Videos build). Removing a leaf whose removal the graph says is safe must build; a failure means a missing edge, which T adds to the graph and to `trim-deps.txt`, and the closure is re-run.

### 4.2 Unicode (`firmware/include/nfc.h`, new; `rbunicode.h` switches its guard to `HAVE_UTF8_NORMALIZE`, Rev 1 H4)
```c
/* Compact canonical composition (Phase 9, replaces utf8proc in the shell).
 * Reproduces utf8proc's compose-only pass as called by unicode.c today
 * (decompose with UTF8PROC_NULLTERM only: no decomposition, no reordering;
 * reencode with COMPOSE|STABLE): blocking rule on combining classes,
 * algorithmic Hangul LV/LVT, pair table incl. non-BMP and ccc-0 seconds.
 * utf8_normalize() keeps its contract: in place, never longer; invalid
 * UTF-8 or >= 2048 code points (unreachable: fields are <= 1800 bytes)
 * leave the string unchanged. */
unsigned long nfc_compose(unsigned long starter, unsigned long second); /* 0 = none; incl. Hangul */
unsigned char nfc_ccc(unsigned long cp);                  /* canonical combining class */
/* nfc_table.h: generated by tools/gen_nfc.c from lib/utf8proc; checked in.
 * Pairs: sorted { u16 starter, u16 second, u16 result } for the BMP + a short
 * u32 list for the rest. Classes: sorted { u32 first:24, len:8 ; u8 ccc }
 * ranges of non-zero ccc. sizeof(pairs) + sizeof(classes) <= 8192,
 * static-asserted. tools/gen_nfc.c also writes tests/unicode/nfc_cases.txt
 * (§3.3.2) for the byte-identity host test against lib/utf8proc. */
```

### 4.3 Scratch slot (`apps/scratchbuf.{c,h}`, new; built under `IPODUI_TRIM_PLUGINS`; Rev 1 M8)
```c
void *plugin_get_buffer(size_t *buffer_size);  /* same name: callers unchanged */
/* scratchbuf.c: PLUGIN_BUFFER_SIZE = IPODUI_SCRATCH_SIZE (§4.1).
 *   0      -> returns NULL, *buffer_size = 0; no pluginbuf reserved.
 *   0x14000 -> _Static_assert(sizeof(struct cuesheet) <= PLUGIN_BUFFER_SIZE,
 *              "scratch slot must hold a cuesheet"); returns pluginbuf. */
```

### 4.4 Sim and tooling hooks (`uisimulator/common/sim_cmd_p9.c`, new; one registration line in `sim_script.c`)
- `alloc_dump`: every live buflib allocation (name, size) to `sim.log` as `p9 alloc name=%s size=%ld`.
- `open_count`: the file opens since boot, as `p9 opens=%d`.
- `run_rock <path>`: only under `HAVE_IPOD_UI_PLUGINS`. It calls `plugin_load()` and logs `p9 rock rc=%d`.
- Trace macro: `DEBUGF` (`logf` is a no-op in `apps/ipodui`). A trace rule that finds none of its lines fails.

### 4.5 Part 2 contracts (committed at Part 2 start)
```c
/* apps/ipodui/fillpolicy_core.h: pure, host-tested (tests/fillpolicy) */
struct fp_track {
    uint64_t filesize;          /* bytes */
    uint32_t length_ms;
    const uint32_t *chap_ms;    /* ascending chapter starts, chap_ms[0] == 0 */
    int nchap;
};
#define FP_ROUND        (64*1024)
#define FP_MIN_AHEAD_MS (10UL*60*1000)          /* ⚠️ P1 */
#define FP_NO_LIMIT     UINT64_MAX
/* Called on the AUDIO thread only (it owns the P6 chapter slots).
 * Where a fill starting at fill_from may stop:
 *  - filesize - fill_from <= room            -> filesize (EOF: never clamp,
 *    so the next file of a multi-file book buffers on time);
 *  - else the estimated offset of the last chapter start <= fill_from + room
 *    (rounded down to FP_ROUND), if that keeps >= min_ahead_ms past play_ms
 *    buffered; else fill_from + room;
 *  - floor: room must never come from bytes at or after floor_off (the
 *    current chapter's estimated start); the caller passes the room that
 *    is free without them, so the result never forces their reclaim. */
uint64_t fp_fill_stop(const struct fp_track *t, uint64_t fill_from,
                      uint64_t room, uint64_t floor_off,
                      uint32_t play_ms, uint32_t min_ahead_ms);

/* apps/buffering.h (P): per-handle fill limit, set by the audio thread.
 * The buffering thread only compares the write offset with it; a handle
 * that reaches its limit is marked satisfied (no buffer_handle() calls,
 * no boost) until the next buffer-low watermark resets the limit. */
bool buf_set_fill_limit(int handle_id, uint64_t limit); /* FP_NO_LIMIT clears */

/* apps/ipodui/powerlog.h (HAVE_IPOD_UI_DEVMODE) */
#define POWERLOG_PERIOD      (60*HZ)
#define POWERLOG_RING_BYTES  4096   /* flushed at storage idle; never wakes the disk */
void powerlog_set(bool on);         /* devmode toggle; off at boot */
```

---

## 5. Ownership (worktrees, one owner per file)

**Concurrency:** at most 5 agents; one gate per machine. **Part 1 wave:** B, A, U, T (4 agents). **Part 2 wave** (after Part 1 merges to `main`): P, W, T2 (3 agents). **Memory (Rev 1, M7):** each agent builds in **one build dir at a time** (`build/fw-ui` *or* `build/sim-ui`, never both at once) at `make -j2`, and builds `fw` (the stock identity check, §6.11) **only at hand-back**, so four agents stay at about 8 compilers and the 8 GB Mac stays under about 10. Agents run at most 2 `scripts/gate-run --quick --changed` and never start `variants.sh` or `trim-revert.sh` (orchestrator only, §3.5).

| Agent (tier) | Owns | Must not edit |
|---|---|---|
| **B** build and variants (opus) | `tools/configure` (variant flags, core-only section flags, `no_utf8proc`/`plugins` **derived from the trim macros** (§3.3.2), `--thumb-cold`), `tools/root.make` and the core-flag plumbing in `apps/apps.make` / `firmware/firmware.make` / `lib/*/*.make` (core libs only), `tools/thumb-cc.py` + new `tools/thumb-cold.txt`, `firmware/export/config/ipodvideo.h` (variant wiring + trim wrapping; the contract header is the orchestrator's), `firmware/export/config.h` (`HAVE_DIRCACHE` gate, `HAVE_UTF8_NORMALIZE` define, §4.1), `ipodvideo.h`'s `PLUGIN_BUFFER_SIZE` = `IPODUI_SCRATCH_SIZE` rule, `firmware/target/arm/pp/app-pp.lds` (only if a `KEEP` proves needed), `firmware/core_alloc.c`, `firmware/target/arm/ipod/video/battery-video.c`, `firmware/target/arm/ipod/powermgmt-ipod-pcf.c` (`IPOD_VIDEO` hooks only), `firmware/target/arm/ipod/piezo.c` (haptic constants), USB class config, `scripts/variants.sh`, new `scripts/risk/gc_check.sh` (the mandatory GC cross-check, §3.3.1), `ci/variants.yml` (draft, not enabled) | `apps/` sources (except the `.make` plumbing), unicode, tests |
| **A** apps UI-layer cut (opus) | `apps/SOURCES` (wrapping stock entries in trims; this phase only, overriding the "additions only" rule), `apps/main.c` (incl. the `p9 accessory=1` trace; `accessory_supply_set` stays outside `_IAP`), `apps/misc.c` (incl. `lineout_set(false)` under `_IAP`; `system_flush_db()`/`system_restore_db()` under `_BROWSER`, H2), `apps/settings.c`, `apps/settings_list.c` (storage kept; UI-only callbacks under trims; `accessory_supply` untouched), `apps/plugin.{c,h}`, new `apps/scratchbuf.{c,h}`, `apps/talk.h`, `apps/tree.c`, **`apps/tagtree.c`** (trimmed to the stats/resume handlers under `_TAGTREE`, H1), `apps/playlist.c` (tree-context paths only), `apps/gui/viewport.c`, `apps/gui/statusbar*.c`, `apps/features.txt`, `apps/ipodui/devmode.c` (only if `do_menu`'s dependencies need it). **Kept stock files that call trimmed code (Rev 1, M1):** `apps/menu.c` (`quickscreen_show` :475, `root_menu_` :512, `shortcuts_add` :589, `skinlist_set_cfg` :807), `apps/debug_menu.c` (`talk_get_debug_data` :2514, `dbg_skin_engine` :2961, the dircache screens from :1904), `apps/screens.c` (`plugin_load` :840, `talk_*`) | `firmware/`, `tools/`, `unicode.c`, `metadata.c`, `english.lang` |
| **U** unicode and statics (opus) | `firmware/common/unicode.c` (`utf8_normalize` via NFC under `HAVE_UTF8_NORMALIZE`; the secondary `codepage_table` fallback, §3.3.2), `firmware/include/rbunicode.h` (the guard), **`lib/rbcodec/metadata/metadata.c`** (the `HAVE_UTF8_NORMALIZE` guard at :30 and :489 only, Rev 1 H4), new `firmware/common/nfc.c`, `firmware/include/nfc.h` body, generated `firmware/common/nfc_table.h`, new `tools/gen_nfc.c`, `tests/unicode/*` (host test + generated cases + `scripts/test.sh` registration line), `apps/language.c` (R4, pinned allocation sized to the loaded `.lng`), new `apps/ipodui/panelart.c` and the matching `#include` lines in `about.c` / `panels.c` | build system, `apps/SOURCES` (asks the orchestrator for the `panelart.c` line) |
| **T** tooling (**opus**, Rev 1 L8: `reach.py` decides the scratch slot and backs the GC cross-check) | new `scripts/sizemap.py`, `scripts/risk/reach.py` (incl. the live `plugin_get_buffer` caller list, M8), `scripts/trim-revert.sh` + `scripts/trim-deps.txt` (closures, §4.1), the batch lock and `P9_BATCH_MAX_MINUTES` budget shared with `variants.sh`; `scripts/validate.sh` (size step: ceiling, `_end`, decoder-set check, `fw-alt` compile-only after the parallel builds), `scripts/gate_map.txt`, `uisimulator/common/sim_cmd_p9.c` + its registration line, `tests/ui/ipodui-p9-*.script` (incl. `-stats-playcount`, `-usb-rescan`, `-rating-persist`, and `-devmode-debug`, which opens **every** devmode debug screen in turn and asserts the sim neither crashes nor shows a splash, M1), `tests/ui/README.md` | `apps/`, `firmware/`, `lib/`, `tools/` |
| **P** playback (opus, Part 2) | `apps/buffering.{c,h}` (`buf_set_fill_limit`, the satisfied flag), `apps/playback.c` (audio-thread limit computation, variant constants), new `apps/ipodui/fillpolicy_core.c`, `tests/fillpolicy/*`, `tests/perf/bookfill.sh`, `ipodui-p9-fill-*.script` (incl. `-fill-multifile`) | shell screens, `tagcache.c` |
| **W** power (sonnet, Part 2) | new `apps/ipodui/powerlog.c`, the devmode toggle item (`devmode.c`), the boost-audit report and fixes in `apps/ipodui/*.c` **except** files P owns, `ipodui-p9-powerlog.script` | `buffering.c`, `playback.c` |
| **T2** tooling (sonnet, Part 2) | trace rules in `scripts/check_trace.py` (`fill stop=`, `powerlog`), the reads-per-hour report, `gate_map.txt` lines | `apps/`, `firmware/` |

**Orchestrator:**
- the contract commit (§4.1–§4.4 headers; empty `nfc.c`/`scratchbuf.c` skeletons that compile; `apps/SOURCES` lines for new files);
- the base measurement (`sizemap.py`/`reach.py` output on merged `main`, recorded in the PR);
- the Part 1 integration gate, then the `variants.sh` and `trim-revert.sh` runs outside the gate lock (§3.5, M7);
- the stock identity check at integration (§6.11, M6);
- the two owner images for H1 (§9, L7);
- `docs/hardware.md` (a new "Build variants" section from B's text, including that the hdd variant runs the deleted-file pass without dircache, L9; budget lines), `docs/ROADMAP.md` (fix "currently off", P10 list), at close;
- the Part 2 contract commit and brief-review delta.

**Read lists** (≤ 3 each; contract headers don't count):
- **B:** this brief §3.3.1, §3.4, §3.5; `firmware/target/arm/pp/app-pp.lds`; `docs/install-and-restore.md` "Build variants".
- **A:** this brief §3.2, §3.3.2, §4.1 (dependency graph); `apps/main.c` init sequence (`init()`, target branch); `apps/tagtree.c` :934–1090 and `apps/tree.c` :1343–1415 (what stays).
- **U:** this brief §3.3.2 (utf8proc, codepages, language buffer); `firmware/common/unicode.c` (`utf8_normalize`, `iso_decode_ex`, `set_codepage`); `lib/utf8proc/utf8proc.c` :544–720 (decompose options and the compose pass).
- **T:** this brief §3.1, §7, §8; `scripts/validate.sh` size step; `tests/ui/README.md`.
- **P:** this brief §3.6.2–3; `docs/hardware.md` power rule 3; `docs/phase6-brief.md` chapter-table section.
- **W:** this brief §3.6.5–6; `docs/process.md` lean rules 5, 6, 11; `docs/code-audit-1.md` F13.

---

## 6. Device-reality checklist (numbers; agents re-measure on their branch)

1. **Compiled features.**
   - Everything new is under `HAVE_IPOD_UI` or the variant defines. `sim-ui` includes `ipodvideo.h`, so it gets the same trims. `HAVE_DIRCACHE` was never in the simulator; after P9 it is off on the device too, so the deleted-file path is one tested path.
   - `HAVE_DISK_STORAGE` exists only in the hdd variants. It is compiled by `fw-alt` (row 2) and run by the `sim-ui --storage=hdd` CI smoke. The hdd variant runs the deleted-file pass without dircache, one `file_exists()` per track on a spinning disk (recorded in `docs/hardware.md`, L9).
   - `HAVE_UTF8_NORMALIZE` (new, H4) is defined in every build that normalises today (stock via `UTF8PROC_EXPORTS`, the shell via `IPODUI_TRIM_UTF8PROC`), so normalisation can't silently switch off; `unicode.c` `#error`s if the trim is set without it, and the NFC host test runs the shell path.
   - The tagtree stats/resume handlers (H1) and the tagcache flush/restore calls (H2) are compiled in every shell variant; `ipodui-p9-stats-playcount`, `-usb-rescan` and `-rating-persist` run them in `sim-ui`.
   - Bootloaders never see `HAVE_IPOD_UI`. Their storage defaults are unchanged, so `boot` and `boot-ui` identity hold.
2. **Memory.**
   - **No new permanent allocation.** Removed at boot: the browser cache **300,000 B**, skin buffers and backdrops (`alloc_dump` before/after), playlist dircache refs **24,000 B**, tagcache dircache refs **12 B × N** (480 KB at 40k, 600 KB at 50k).
   - Statics: **−168,564 B** joint cut (already includes `normbuf` −8,192; Rev 1 L2) + `codepage_table` −65,538 + `language_buffer` ≈ −18,800 net. Plugin slot **+524,288 B** to `DRAMSIZE` expected (+442,368 B if a cuesheet caller is live, M8).
   - **How it returns to the audio buffer:** `_end` moves down, `DRAMSIZE` grows, and the first `audio_reset_buffer()` → `core_alloc_maximum()` takes all of it. Nothing to regrow.
   - Codepages (M5, corrected): the secondary table is never allocated in the shell (fallback path, §3.3.2); the default table stays upstream's boot-time allocation. The language buffer is a pinned allocation of the loaded `.lng`'s size, made at boot before audio. Neither allocates while playing; a devmode codepage or language change applies at the next boot.
   - The power log uses a static 4 KB ring (a budget line: it can't share, because it is filled by a timer callback while anything else may own the strip). Devmode only.
3. **CPU.**
   - **NFC:** ASCII tags take one scan with no buffer; utf8proc decomposes every tag into a 2,048-word buffer and re-encodes it. U reports ELF-harness instructions per tag (ASCII, Latin NFC, Latin NFD) for old and new, plus the 50k-track scan estimate.
   - **iAP:** the 10 Hz `iap_task` post and thread switch are removed.
   - **`car_adapter_tick`:** 100 Hz tick task removed.
   - Neither is credited towards the battery goal (L5): both ride the 100 Hz tick the CPU takes anyway, so the gain is RAM and code. H2 measures battery on the whole build, not per item.
   - Thumb-cold is the only change that slows anything, and it is off.
   - Part 2 fill policy: `fp_fill_stop()` is O(log chapters), on the audio thread, once per chapter load, seek or watermark. A clamped handle is satisfied: no `buffer_handle()` calls and no boost until the next watermark (M3d).
4. **Storage.**
   - Fewer opens at boot: no skin `.wps`/`.sbs`/backdrop, no `viewers.config` (filetypes), no shortcuts file, no voice probe, no dircache scan. T reports `p9 opens=` at boot, before and after.
   - No new read anywhere in Part 1.
   - Part 2: chapter-aligned fills change *where* a fill stops, not its size. `bookfill.sh` reports `read()` calls and bytes per hour of book playback (lean rule 11).
5. **LCD.** No Part 1 change. Part 2 adds only a trace line. Zero golden changes in Part 1 (criterion 1).
6. **Threads and locks.**
   - Removed: the iAP thread (6,144 B stack, 10 Hz), the voice thread (1,984 B IRAM stack), the dircache thread (2,048 B).
   - The power log's timeout callback only writes a RAM ring. The flush runs on the storage thread's idle callback, never takes a UI mutex, and holds no pointer across a yield.
   - The fill limit is computed on the **audio thread**, which owns the P6 chapter slots (`np_playback.h` :216–229), and passed to the buffering thread through `buf_set_fill_limit()` (the buffering queue). The buffering thread never reads a chapter slot (Rev 1, M3c).
7. **Upstream splashes.**
   - Trimming removes most splash sources. Remaining callers of `plugin_load`, `talk_*` and the stock browser are compiled out or stubbed silently: no "Plugin not found" path. That includes the kept files A guards (M1): `screens.c` :840 (`plugin_load` of the text viewer), `menu.c`'s quickscreen and shortcut calls, `debug_menu.c`'s skin and talk screens. `ipodui-p9-devmode-debug` opens every debug screen.
   - The "Scanning disk..." splash in `tree_restore()` becomes a `DEBUGF` in `misc.c`'s copy (H2).
   - The 8a audit grep must stay empty.
   - `main.c`'s storage-error and no-partition screens are unchanged (P10 recovery, already accepted in 8a).
8. **Persistence.**
   - `config.cfg`: settings of trimmed features are ignored on load and dropped on the next save. Reverting a trim gives defaults, not old values (acceptable, documented).
   - Database: U's NFC golden must be byte-identical to utf8proc; there is no `TAGCACHE_MAGIC` fallback (H4). Stored tag strings therefore don't change and no rebuild happens.
   - Runtime statistics and resume data (H1, H2). Triggers and paths: track finish → `tagtree_track_finish_event` on the audio thread queues `tagcache_update_numeric` (playcount, playtime, lastplayed, lastelapsed, lastoffset); power-off → `clean_shutdown` → `system_flush()` → `system_flush_db()` → `tagcache_shutdown()` on the UI thread writes them, and ratings, to disk; USB connect → `system_flush()` (same call); USB disconnect → `system_restore_db()` → `tagcache_start_scan()`. Scripts, in the playing state where it applies: `ipodui-p9-stats-playcount`, `ipodui-p9-rating-persist`, `ipodui-p9-usb-rescan`.
   - Dircache refs are RAM-only. A confirms that no on-disk file (tagcache statefile or anything else) embeds the `dcfrefs` layout; if one does, it gets a version check.
   - `books.dat`, OTG, artcache, panels and the changelog carry no variant-dependent field. T greps their structs for `HAVE_DIRCACHE`, `MEMORYSIZE` and `HAVE_RECORDING`.
   - Power log: flushed at storage idle, at clean shutdown (`clean_shutdown` path, UI thread) and at USB connect before the acknowledgement. One script per trigger in the playing state (`ipodui-p9-powerlog-{idle,poweroff,usb}`).
9. **Scale.** Dircache-ref savings scale with N (12 B/track). The NFC cost scales with tags × length (U's 50k estimate). Nothing else in Part 1 is O(N).
10. **Deferred or retried work.** The power-log flush waits for storage idle: with the disk asleep it waits, the 4 KB ring holds ≈ 60 rows, and the oldest rows are overwritten with a `dropped=` count. Tested with `disk_active` pinned off. Retries in database states: n/a (no database work added).
11. **Stock firmware unchanged.**
    - Section flags, trims and `--thumb-cold` apply only with `--ipodui`. The variant defaults reproduce today's `ipodvideo.h`.
    - **Checks (Rev 1, M6).** A `.size-baseline` match compares sizes, not bytes, so it isn't enough for this phase (B moves facts between headers, U renames a guard, A edits shared files). The orchestrator builds stock `fw` and `boot` twice, at the merge-base with `main` and at the branch HEAD, in two clean build dirs with the **same `VERSION=`** (so the version string can't differ), and compares per-section SHA-256 (`arm-none-eabi-objcopy -O binary -j <section>` for `.text`, `.rodata`, `.data`, `.init`, `.iram`, `.vectors`, plus the whole `objcopy -O binary` image). Every hash must match. B and A run the same script (`scripts/risk/stock_identity.sh`, B) at hand-back. `english.lang` untouched (the `features.txt` changes drop trimmed phrases from shell builds only).
    - The decoder set is unchanged: the same 39 decoder `.codec` names, each built, sizes reported. Byte identity is **not** expected, because the `codec_api` layout loses its `HAVE_RECORDING` members, and core and codecs ship together in `.rockbox`.
12. **Existing upstream paths doing the same job.**
    - utf8proc vs `nfc.c`: `nfc.c` wins in the shell; stock keeps utf8proc.
    - Secondary codepage table vs upstream's load-failure fallback: the fallback wins in the shell (§3.3.2).
    - Plugin buffer vs scratch slot: same symbol, slot sized by the live callers (0 expected, M8).
    - tagtree vs `library.c`: library wins for browsing; tagtree's menus are compiled out. **Kept (H1):** `tagtree_buffer_event` and `tagtree_track_finish_event` and their registration, the only writers of playcount, playtime, lastplayed, lastelapsed and lastoffset (`library.c` and `lib_book_seed()` read them).
    - `tree.c`'s `tree_flush()`/`tree_restore()` vs `misc.c`'s new `system_flush_db()`/`system_restore_db()`: the `misc.c` copies win in the shell (they hold the same tagcache shutdown, ramcache unload, statefile removal and post-USB `tagcache_start_scan()`); stock keeps calling `tree.c` (H2).
    - Normalisation: `utf8proc` vs `nfc.c` share one guard, `HAVE_UTF8_NORMALIZE`, so exactly one of them runs (H4).
    - dircache vs tagcache's `file_exists()`: `file_exists()` wins.
    - Skin WPS vs native Now Playing: native wins; the WPS is unreachable (`root_menu` was its only caller).
    - Upstream battery tables vs the variant table: the variant table, equal for now.
    - Upstream `charging_splash()` and 8a's `charging_sleep_prepare()`: untouched.
    - Part 2: playback's own watermark logic stays the engine; the fill stop is one per-handle limit (`buf_set_fill_limit`) set by the audio thread and honoured by `buffer_handle()`, not a second filler.
    - Engines keeping references into rewritten files: none (no file format changes).
13. **Builder iteration predicate.** No new background builder in Part 1. Part 2's fill predicate is in §3.6.3, and `ipodui-p9-fill-*` covers a multi-file book (with a gapless transition, `-fill-multifile`), a coverless book, a podcast with chapters, a 1-chapter book (never clamped) and an album (never clamped).

---

## 7. Acceptance criteria

1. **Gate and hygiene.**
   - `gate-run --quick` passes on each branch; the integration full gate passes once per part.
   - No new warnings; no static > 1 KB except the power-log ring (budget line); `cpu_boost` balanced; no "Rockbox" newly visible.
   - **Part 1 changes zero product-screen goldens** (Rev 1, M2). Expected recaptures, and only these: the devmode goldens that show stock lists, which draw unthemed under `_SKIN`: `ipodui-p8a-devmode/developer.png`, `ipodui-p8a-devmode-boot/developer.png`, and any debug-screen golden. They are recaptured with `golden_capture.sh` (two pixel-identical runs) and shown to the owner side by side. Every other golden, including `about_*`, `settings_*` and `main.png` in the same scripts, must match unchanged.
2. **Size** (owner variant, devmode `fw-ui`): `rockbox.ipod` **≤ 760,000 B** target. **Hard ceiling 914,813 B**: above it the gate fails. `fw-rel` ≤ 735,000 B. Every pairwise row (§3.5) ≤ 914,813 B. Above the target but under the ceiling, the miss is reported per row of §3.2.
3. **RAM back to the audio buffer.**
   - `_end` ≤ 1,100,000 B (from 1,957,056; projected ≈ 1,059,000).
   - `DRAMSIZE` +524,288 B (slot 0) if `reach.py` lists no live `plugin_get_buffer` caller, else +442,368 B; `reach.py`'s caller list is in the PR (M8).
   - The sim `alloc_dump` at boot shows no browser cache, skin, backdrop, iAP or dircache allocation.
   - The computed device pool (`DRAMSIZE − _end − boot allocations`) is ≥ base + 1.6 MB, excluding the library-dependent dircache refs. P10 confirms on the device (H1).
4. **Codecs.** The same 39 decoders build. Only `aiff_enc`, `wav_enc`, `wavpack_enc` and `mp3_enc` are gone. The FLAC and APE IRAM images still fit. The sim plays every format in `tests/fixtures`.
5. **Videos stays possible.** `--ipodui --ipodui-plugins` builds, links `plugin.o`, restores `PLUGIN_BUFFER_SIZE` 0x80000, and builds `mpegplayer.rock`. In the plugins sim, `run_rock` loads a small rock, returns `rc=0`, and the shell redraws. Its size is reported, not gated.
6. **Reversibility (Rev 1, H3).** `scripts/trim-revert.sh` removes each trim line **together with its dependency closure** (§4.1, `scripts/trim-deps.txt`) and builds `fw-ui`, one build per distinct closure (13 builds; the `PLUGINS` closure is the criterion-5 build). With the derived `no_utf8proc`/`plugins` (§3.3.2), removing `UTF8PROC` or `PLUGINS` also brings back `libutf8proc` or the plugin build with no second edit. All pass. It runs outside the gate lock, after the integration gate (§3.5, M7). Each `#error` in §4.1 is also exercised once (an incoherent set must fail to compile with that message).
7. **Stock still compiles and is identical.** The `fw`, `boot` and `sim` builds pass, and stock `fw` and `boot` built at the merge-base and at HEAD with the same `VERSION=` have identical per-section hashes and identical `objcopy -O binary` images (checklist 11, M6).
8. **Variants.**
   - All 6 pairwise rows compile.
   - `fw-alt` is in the full gate.
   - `sim-ui --ram=32` passes the smoke set: `ipodui-p9-smoke-boot`, `-browse`, `-play`, `-usb`, `-devmode`.
   - `IPOD_VARIANT` appears in the boot log.
9. **Sections GC.** `--print-gc-sections` is cross-checked with `reach.py` by B's `scripts/risk/gc_check.sh`, **mandatory at hand-back** (L8); no reachable symbol is dropped. `cop_main`, `irq_handler`, `fiq_handler`, `start` and the osos signature are present at the same section offsets. The `-fdata-sections` A/B is reported, and kept only if it wins.
10. **NFC (Rev 1, H4).**
    - Every generated case in `tests/unicode/nfc_cases.txt` (§3.3.2: all pairs with blocked and unblocked intervening marks of every class, all Hangul, ccc-0 seconds, exclusions and singletons, non-BMP, invalid UTF-8, 2,046/2,047 code points, the mixed-script smoke set) is **byte-identical** to the in-tree utf8proc, and the 2,048-code-point case is unchanged by `nfc.c`. No `TAGCACHE_MAGIC` bump.
    - `HAVE_UTF8_NORMALIZE` is defined in `fw-ui` and `sim-ui` (checked by the size step: `utf8_normalize` is in the ELF and `metadata.c` calls it).
    - Pair and class tables together ≤ 8,192 B.
    - Instructions per tag reported (checklist 3).
    - An NFD-tagged album and its NFC twin show as one album in the 50k perf library (`tests/perf/lib50k.sh` with an NFD slice).
11. **Dock rails.** Under `!DOCK30`, `PWRMGMT3` is written without `LOUT2EN`/`ROUT2EN` at boot (a sim trace `p9 lineout=0`). The accessory rail stays on (O5): `p9 accessory=1` at boot in every `!DOCK30` variant, and no `accessory_supply_set` call on the USB or charging-sleep paths (grep plus traces in `ipodui-p9-smoke-usb` and the 8a charging-sleep script) (M4).

11a. **Statistics, resume and post-USB scan survive the trims (H1, H2).** `ipodui-p9-stats-playcount` (playcount +1 and a fresh lastplayed after a track plays to its end), `ipodui-p9-rating-persist` (a rating survives power-off and reboot) and `ipodui-p9-usb-rescan` (an album added over USB appears after disconnect) pass. The existing P6 book-resume scripts pass unchanged.

11b. **Kept stock files (M1).** `ipodui-p9-devmode-debug` opens every devmode debug screen; no crash, no splash.
12. **Part 2.**
    - The `tests/fillpolicy` table tests pass: chapter fits, no fit, the min-ahead guard, 1 chapter, songs, **the floor** (no stop that needs bytes at or after the current chapter's start) and **EOF** (the rest of the file fits → `filesize`).
    - `ipodui-p9-fill-multifile`: a 3-file book plays across both boundaries with no gap (`pcmbuf` trace), and the next file's handle opens before the current one ends.
    - Between a `fill stop=chapter` trace and the next buffer-low watermark: zero `buffer_handle` calls for that handle and no buffering `cpu_boost` (traced; M3d).
    - The buffering thread never calls into the P6 chapter slots (grep: no `np_pb_ab_*` or chapter-table access in `buffering.c`; M3c).
    - `ipodui-p9-fill-book` shows `fill stop=chapter` on a 199-chapter, 51.7 h synthetic book and `fill stop=room` on an album.
    - A within-chapter seek after a fill causes no rebuffer (`buffering` trace).
    - Reads per hour reported.
    - `ipodui-p9-powerlog-*` produce a CSV with pinned battery values and never trigger a disk wake (`disk_active` trace).
    - The boost-audit table is complete, with every shell site either boosting only while composing or fixed.

## 8. Review artifacts
| Artifact | Producer | Content |
|---|---|---|
| Size waterfall | T `sizemap.py` | base → after each trim, by subsystem (the §3.2 rows re-measured on the branch), `rockbox.ipod`, `_end`, IRAM |
| Keystone report | T `reach.py` | the reference paths of §0 before and after (empty after), the unreachable list, and the live `plugin_get_buffer` callers (expected none, M8) |
| GC cross-check | B `gc_check.sh` (mandatory) | `--print-gc-sections` summary vs `reach.py` |
| Stock identity | orchestrator, `stock_identity.sh` | per-section SHA-256 of stock `fw` and `boot`, merge-base vs HEAD, same `VERSION=` (M6) |
| Boot allocations and opens | T | `alloc_dump` + `p9 opens=` before and after, sim |
| Variant log | B `variants.sh` | 6 rows: configure line, `rockbox.ipod` size, `_end` |
| Trim-revert log | T (run by the orchestrator) | 13 closure builds, pass/fail, the closure used for each, any new edge found; the `#error` checks exercised |
| NFC report | U | case count by category and diff vs utf8proc (must be empty), pair/class table sizes, instructions per tag |
| Videos proof | T | plugins-variant `run_rock` sim log and screenshot; `mpegplayer.rock` size |
| Thumb-cold size | B | text and image deltas (not shipped) |
| Part 2 | P, W | fill traces on the long-book and multi-file scripts, the clamp-to-watermark trace (no calls, no boost), `bookfill.sh` reads per hour, power-log CSV sample, boost-audit table (incl. the ATA per-transfer boost) |

## 9. P10 device checks and handoffs (go to ROADMAP → Device checklist at close)
- **H1 (after Part 1 merges; owner's go-ahead; firmware-only install, same route as tonight).**
  - **Two images (Rev 1, L7)**, so a boot failure is attributed to the right change: **image A**, the trims only (`-ffunction-sections` off), installed first; **image B**, trims plus function sections, installed only after A has booted and played. Both sizes go in the H1 note.
  - Devmode buffer screen: audio buffer vs tonight's P11 build (expect +~2 MB at the owner's library size).
  - Boot time to the main menu (fewer inits: expect shorter).
  - `IPOD_VARIANT` in the log.
  - Device boot of image B (function sections): if it doesn't boot, hold MENU → Apple firmware, Disk Mode, restore image A's `rockbox.ipod`.
- **H2: partial A/B rundowns (⚠️ O6).** Apple 1.3 vs P11 vs P9 Part 1: same playlist, fixed volume, screen off, 24 h each, voltage drop and power log. The full rundown comes later, for the final number.
- **H3: battery curve.** A full discharge with the power log on the P9 build, from 100 % to shutoff. It sets the large-cell table and whether flash needs the 3,500 mV disk-safe floor (⚠️ V3, V6).
- **H4: refill.** Boosted vs `BUFFERING_BOOST_FILL` off at UDMA4, each with and without the ATA driver's per-transfer boost (L6): refill wall time and current. Chunk 32/128/256 KB: refill time and `read()` count. It sets ⚠️ P2 and P3.
- **H5: LCD.** Current lit, backlight off with the LCD on, LCD asleep. It sets the LCD sleep default (⚠️ P4).
- **H6: audiobook fill.** On the Lovecraft m4b (199 chapters, 51.7 h, copied on the Mac, never from the 5.5G):
  - a seek within the current chapter never pauses;
  - a far seek pauses ≈ 1 s;
  - no stall over 2 h of listening;
  - reads per hour from the log.
- **H7: COP.** Art-builder busy time per fill window and the boost ratio, to feed the COP decision (⚠️ P5).
- **H8: haptics.** Motor pulse length and frequency feel (#23). It sets the `HAPTIC_TICK_*` values (⚠️ V4).
- **Thumb-cold speed check (only if 9b wants it):** a `lib50k`-equivalent open, the art builder per album, and a Search keystroke, ARM vs `--thumb-cold`.
- **Retired:** "Deleted files, dircache branch" (dircache is compiled out in shell builds).

## 10. ⚠️ Frozen defaults (the owner round confirms; not revised mid-phase)
| # | Default | Alternative |
|---|---|---|
| O1 | Tunables via `config.cfg` over USB | devmode settings editor |
| O2 | No plugins until P12; Videos via `--ipodui-plugins` | keep the plugin API in the daily build |
| O3 | Devmode build on the owner's unit | release build |
| O5 | Accessory 3.3 V rail stays on, independent of `_IAP` (`p9 accessory=1` trace); dock line-out (OUT2) off under `!DOCK30` | rail off too |
| O6 | Partial 24 h A/B runs for tuning; one full rundown each for the final number | full rundowns only |
| O7 | Local pairwise runner; workflow not enabled until GitHub | Forgejo runner now |
| V1 | Owner variant: flash, 64, 3000, usbc, haptics | – |
| V2 | `--ram=64` keeps the runtime 32 MB guard (one boot branch). **Accepted exception** to §1.2's compile-time rule: a mis-install safety net, one compare at boot (L4) | hard 64 |
| V3 | Large-cell table = upstream table until H3 | estimated table now |
| V4 | Haptic pulse = piezo pulse until H8 | guessed motor pulse |
| V5 | hdd variant = upstream values (disk storage, ATA power-off, SMART, default UDMA) | UDMA4 on hdd |
| V6 | Disk-safe floor 3,500 mV on both storages until H3 | lower on flash |
| T1 | `-ffunction-sections` on core objects; `-fdata-sections` only if the A/B wins | both always |
| T2 | NFC reproduces utf8proc's current compose-only call exactly (blocking rule, Hangul, no reordering, which is what today's call does); byte identity on an exhaustive generated set; no magic bump (H4) | full NFC |
| T3 | Dircache compiled out in every shell variant | keep for hdd |
| T4 | `--ipodui-plugins` lifts the UI-library trims (the API references them); size reported, not gated | a slim, mpegplayer-only API (later) |
| T5 | Voice out (stubs) | keep |
| T6 | Thumb-cold mechanism lands, off | on for the listed files |
| T7 | The 64 KB secondary `codepage_table` static goes; a secondary-codepage request takes upstream's ISO-8859-1 fallback (only SMAF tags with a declared codepage are affected). The default table stays upstream's boot allocation (M5, corrected against the code) | keep the static |
| T8 | Scratch slot sized by `reach.py`'s live callers: 0 B expected (+524,288 B), 80 KB if a cuesheet caller is live (M8) | 80 KB always |
| P1 | Chapter-aligned fill for spoken tracks with ≥ 2 chapters; ≥ 10 min ahead; never past the current chapter's start; never clamped when the rest of the file fits | 5 / 20 min |
| P2 | Flash watermark 1 s plus bitrate; 32 KB chunk | from H4 |
| P3 | Fills stay boosted | unboosted |
| P4 | LCD sleep 90 s | from H5 |
| P5 | No COP moves | art builder on COP |

## 11. Size and effort estimate
| Track | Lines (≈) | Effect |
|---|---|---|
| B | 450 (configure 200, `ipodvideo.h` 80, make plumbing 60, battery/piezo/core_alloc 50, `variants.sh` 60) | flags, sections, −14.6 KB GC (net of anchors, to measure) |
| A | 800, mostly `#if` wrapping (SOURCES 80, `main.c` 60, `talk.h` stubs 120, `settings_list.c` 150, `plugin.c`/`scratchbuf` 60, `playlist.c`/`tree.c` 60, `tagtree.c` trim 40, `misc.c` flush/restore 30, `menu.c`/`debug_menu.c`/`screens.c` guards 40, viewport 60, misc 50) | −307 KB image, −100 KB bss, −300 KB boot alloc, +512 KB slot (expected) |
| U | 750 (`nfc.c` 180, generator incl. case writer 250, host test 150, guard/metadata/unicode/language/panelart 120, codepage fallback 50) | −341 KB image (+≤ 8 KB), −92 KB bss |
| T | 700 (`sizemap` 150, `reach` 280, `trim-revert` + closures + batch lock 100, sim cmds 100, scripts) | – |
| **Part 1** | **≈ 2,700** | **`rockbox.ipod` ≈ 737 KB (devmode), ≈ 708 KB (release); `_end` ≈ 1.06 MB; ≈ 2.2 MB more audio buffer at 40k** |
| P | 600 (core 140, `buf_set_fill_limit` + satisfied flag 100, audio-thread hook 60, tests 200, perf/scripts 100) | fills end on chapter starts |
| W | 400 (powerlog 200, audit fixes 100, scripts 100) | measurement for H2–H5 |
| **Part 2** | **≈ 1,000** | |

**Effort:** Part 1 ≈ 0.8× Phase 8a (mechanical but wide; risk sits in the link and boot). Part 2 ≈ 0.4×. Four, then three, agents at ≤ 250k tokens each.

## 12. Risks the brief review should test
1. **Boot after section GC.** The simulator can't test the link. Is the `--print-gc-sections` / `reach.py` cross-check plus the symbol presence list enough before a device install? Is the recovery route stated?
2. **The anchor trade-off.** `-fdata-sections` may cost more than it saves. The A/B decides; check the brief doesn't assume the gain.
3. **Voice stubs.** Every kept caller of `talk_*` (`settings_list.c`, `misc.c`, playback) compiles to nothing, and no stub changes behaviour (e.g. `talk_id` return values used as conditions).
4. **Browser removal vs `playlist.c`.** No shell path reaches `tree_get_context()`. `next_folder` is off and hidden. Resume of a directory-based playlist from an old `config.cfg` is ignored, not crashed.
5. **NFC equivalence.** A mismatch would change stored tag strings, so there is no fallback: the generated exhaustive set must be byte-identical (H4). Is the case set really exhaustive for the compose pass (every pair × every blocking class, Hangul, ccc-0 seconds, non-BMP, invalid input, the buffer limit)?
6. **Scratch slot users.** `reach.py` must list no live `plugin_get_buffer` caller for the slot to go to 0; every caller handles `NULL`; the `IPODUI_SCRATCH_SIZE` rule brings the slot back with any trim revert that brings a caller back (M8).
6a. **Hidden dependencies of trimmed files.** H1 and H2 came from files that looked like pure UI (`tagtree.c`, `tree.c`) but held engine hooks. A lists, for every file it removes from `SOURCES`, each `add_event`, `register_storage_idle_func`, `timeout_register` and each function called from a kept file, and shows each one is either kept or dead.
7. **Plugins variant scope.** Lifting the UI trims for `--ipodui-plugins` must still link. Report its size.
8. **Mis-installed variant.** 64 on a 32 MB unit runs through the guard. 32 on a 64 MB unit wastes 32 MB but runs. hdd on flash and flash on hdd both run (timing differences only).
9. **The Part 2 fill clamp.** It must never shrink a fill below what a gapless song queue needs (the predicate excludes songs, and EOF is never clamped), must never reclaim the current chapter, and must never fight the buffer-low watermark into a refill loop (the satisfied flag). Host-test all three.

### Critical Files for Implementation
- tools/configure
- firmware/export/config/ipodvideo.h
- apps/SOURCES
- apps/main.c
- firmware/common/unicode.c

---

**Notes for the coordinator (not part of the brief):**
- **Where the numbers come from.**
  - Every size comes from `.claude/worktrees/agent-aea6de579fe54aa27/build/fw-ui/rockbox.{elf,map}` (p11, built 18:24, ≈ `ec1e371a51`). Main's own `build/fw-ui` is stale (Oct 7, 1,236,472 B), so re-measure on merged `main` before fan-out (§5, orchestrator).
  - The reachability numbers came from a throwaway inline script (nothing written to disk); T rebuilds it as `scripts/risk/reach.py`.
  - The cut estimates are conservative: data references are widened to whole input sections because of section anchors.
- **ROADMAP correction.** `--gc-sections` is already on for the core link via `lib/fixedpoint/fixedpoint.make` (`CORE_GCSECTIONS := yes`). It currently discards 84,541 B (`rockboxlogo`, `usblogo`, …). What's missing is `-ffunction-sections`.
- **Rule 7 drift found.** Flash, no ATA power-off or SMART, UDMA4 and 3000 mAh are hard-coded in `ipodvideo.h` for **every** build, stock included. P9 moves them behind flags whose defaults keep the stock build byte-identical.
- **Battery finds worth flagging to the owner.**
  - iAP posts to its thread every 100 ms forever (`iap-core.c` `timeout_register(..., MS_TO_TICKS(100))`) on a unit with no dock.
  - The dock line-out stage (WM8758 OUT2) and the accessory 3.3 V rail both default on (`settings_list.c` :1489–1494). Line-out is safe to switch off; the accessory rail is owner question O5, in case the mods draw power from it.
- **Plugin slot.** It is 512 KB of DRAM reserved for plugins the shell never loads. Upstream borrows it as scratch (`cuesheet.c` needs ≈ 73.2 KB), but every borrower is in a trimmed file, so Rev 1 sizes it from `reach.py`'s live-caller list: 0 expected, 80 KB only if a cuesheet caller survives.
- **Codec identity.** Decoder `.codec` files will *not* be byte-identical after P9, because `codec_api` has `HAVE_RECORDING` members. The criterion is "the same 39 decoders built", which still proves the mission holds.
- **Rundown duration.** A 3000 mAh rundown is days, not hours. That is why O6 proposes partial A/B runs for tuning.

---

## Revision 1 (2026-10-09, brief review)

Each finding was checked against `main` and the p11 worktree. All are accepted. Three are accepted with a correction where the code showed a detail of the finding was off (H4, M5, M8; noted in the table).

| Id | Resolution | Sections touched |
|---|---|---|
| H1 | `tagtree_buffer_event` and `tagtree_track_finish_event`, with their `add_event` registration, stay in the shell build via a trimmed `tagtree.c` (everything else compiled out under `_TAGTREE`; stock file unchanged, no duplicate code). A owns `tagtree.c`. Script `ipodui-p9-stats-playcount` (playcount +1, fresh lastplayed). Listed in checklist 12. | §2, §3.2 row 9, §3.3.2, §5, §6.1, §6.8, §6.12, §7.11a, §11, §12.6a |
| H2 | The tagcache halves of `tree_flush()`/`tree_restore()` (shutdown flush, ramcache unload, statefile removal, post-USB `tagcache_start_scan()`, plus the dircache parts) move to `misc.c` as `system_flush_db()`/`system_restore_db()` under `_BROWSER`; the splash becomes a `DEBUGF`. Scripts `ipodui-p9-usb-rescan`, `ipodui-p9-rating-persist`. | §2, §3.2 row 10, §3.3.2, §5, §6.7, §6.8, §6.12, §7.11a |
| H3 | Dependency graph published in §4.1 as `#error` checks plus `scripts/trim-deps.txt`; `trim-revert.sh` removes each trim with its closure (13 distinct builds; the plugins closure is the Videos build). `tools/configure` derives `no_utf8proc` and `plugins=""` from the trim macros (fallback: trim-revert edits both and logs it). | §3.3.2, §4.1, §5, §7.6, §8 |
| H4 | New guard `HAVE_UTF8_NORMALIZE` (defined in `config.h` from `UTF8PROC_EXPORTS` or `IPODUI_TRIM_UTF8PROC`) used by `metadata.c`, `rbunicode.h`, `unicode.c`; U owns the `metadata.c` guard. `nfc.c` mirrors utf8proc's compose pass: combining-class table + pair table (≤ 8 KB together) + algorithmic Hangul. Generated exhaustive golden with byte identity; magic-bump fallback dropped. **Correction:** today's call doesn't decompose or reorder at all (no `UTF8PROC_DECOMPOSE`), so compose-only is exact; and strings of 2,048+ code points are not "left unchanged" upstream but overrun `normbuf` in `unicode.c`, unreachable because fields are ≤ 1,800 bytes. The golden compares up to 2,047 code points, and `nfc.c` leaves longer strings unchanged. | §2, §3.2 row 1, §3.3.2, §4.1, §4.2, §5, §6.1, §6.8, §6.12, §7.10, §8, §10 T2, §11, §12.5 |
| M1 | A owns the kept stock files that call trimmed code: `apps/menu.c`, `apps/debug_menu.c`, `apps/screens.c`. U owns `metadata.c`. T adds `ipodui-p9-devmode-debug`, which opens every debug screen. | §3.3.2, §5, §6.7, §7.11b |
| M2 | Devmode goldens that show stock lists (`ipodui-p8a-devmode/developer.png`, `ipodui-p8a-devmode-boot/developer.png`, any debug-screen golden) are expected recaptures under `_SKIN`; product-screen goldens still allow zero changes. | §2 Out, §3.3.2, §7.1 |
| M3 | (a) A floor: a fill never reclaims bytes at or after the current chapter's start. (b) `fp_fill_stop()` returns EOF whenever the rest of the file fits; `ipodui-p9-fill-multifile` tests a gapless multi-file transition. (c) The audio thread computes the limit and passes it through `buf_set_fill_limit()`; the buffering thread never reads the P6 chapter slots. (d) A clamped handle is marked satisfied until the next watermark: no `buffer_handle` calls and no boost in between (traced). | §3.6.3, §4.5, §5, §6.3, §6.6, §6.12, §6.13, §7.12, §8, §10 P1, §11, §12.9 |
| M4 | The accessory rail stays independent of `_IAP` (`main.c` :850 and `settings_list.c` :1489 untouched); boot trace `p9 accessory=1` in every `!DOCK30` variant; the USB and charging-sleep paths are shown never to call `accessory_supply_set()`. | §0, §3.4, §5, §7.11, §10 O5 |
| M5 | Language buffer: a pinned allocation of the loaded `.lng`'s `lang_size`, made at boot before audio, nothing allocated for built-in English, no language dropped. **Correction for the codepage half:** `codepage_table` is the *secondary* (explicit-codepage) table, loaded on demand from any thread inside `iso_decode_ex()`; the default table is already a boot-time `core_alloc` in `set_codepage()`. A boot-time allocation keyed on the default codepage would cover nothing. So the static is removed and secondary requests take upstream's own ISO-8859-1 fallback (only SMAF tags with a declared codepage are affected). Nothing is allocated while playing, which meets the finding's intent. | §2, §3.2 rows 17–18, §3.3.2, §4.1 (`_MISC` comment), §5, §6.2, §6.12, §10 T7 |
| M6 | Stock identity: stock `fw` and `boot` built at the merge-base and at HEAD with the same `VERSION=`, per-section SHA-256 and `objcopy -O binary` compared (`scripts/risk/stock_identity.sh`). | §5, §6.11, §7.7, §8 |
| M7 | `variants.sh` and `trim-revert.sh` run outside the gate lock under their own lock and `P9_BATCH_MAX_MINUTES` (120), never during a gate, batched by closure. Agents use one build dir at a time at `-j2` and build `fw` only at hand-back. | §3.5, §5, §7.6 |
| M8 | `reach.py` lists the live `plugin_get_buffer` callers. Source check: every caller is in trimmed code, so the slot is expected to be **0** (+524,288 B). `IPODUI_SCRATCH_SIZE` is 0 while `_BROWSER`, `_SKIN` and `_TAGTREE` are all trimmed, else 80 KB (so a revert that brings a caller back brings the slot back). | §0, §2, §3.2 row 13 and projection, §3.3.2, §4.1, §4.3, §5, §6.2, §6.12, §7.3, §8, §10 T8, §12.6 |
| L1 | Re-baselined on the latest p11 build: `rockbox.ipod` 1,385,064 B, `_end` 1,957,056 B (orchestrator figure; this revision read 1,385,376 / 1,957,360 from the 19:38 build). `main` doesn't contain p11 yet, so the orchestrator's base measurement on merged `main` stays the reference. | header, Goal, §0, §3.2, §7.3 |
| L2 | `normbuf` is counted once (inside the joint Σ); the NFC tables, the `panelart` cut and the kept H1/H2 code are included. Realistic `_end` ≈ 1,059,000 B (≈ 1.05 MB); devmode image ≈ 737,000 B, release ≈ 708,000 B; audio gain ≈ 2.2 MB. | Goal, §3.2, §6.2, §7.3, §11 |
| L3 | `CODEC_SIZE` reworded as "codec image + codec working buffer" (`kss` 443,896 B). | §2 Out |
| L4 | The V2 runtime 32 MB guard is marked as an accepted exception to the compile-time rule (a mis-install safety net, one compare at boot); the owner round can still drop it. | §3.4, §10 V2 |
| L5 | `car_adapter_tick` and the iAP posts are no longer credited towards the battery goal: both ride the 100 Hz tick, so the gain is RAM and code. | §0, §3.4, §6.3 |
| L6 | The ATA driver's per-transfer boost at UDMA4 (`ata-pp5020.c` :185–187, :248–250) is added to the boost audit and to H4's measurements. | §0, §3.6.6, §9 H4 |
| L7 | The owner gets two images at H1: A, trims only (installed first); B, trims plus function sections. | §5, §9 H1 |
| L8 | Both: T (which writes `reach.py`) moves to opus, and B's GC cross-check (`gc_check.sh`) is mandatory at hand-back. | §3.3.1, §5, §7.9, §8 |
| L9 | `docs/hardware.md`'s "Build variants" section records that the hdd variant runs the deleted-file pass without dircache (orchestrator, at close). | §3.3.2, §5, §6.1 |
