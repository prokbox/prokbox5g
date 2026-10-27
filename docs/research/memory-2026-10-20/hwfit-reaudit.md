> Research record, 2026-10-20 (read-only audit; inputs to docs/memory-audit-2026-10-20.md, docs/design/module-loader.md and docs/hardware-fit-audit.md). Paths to `scratchpad/` refer to the session's scratch area and are not kept. Where it conflicts with the synthesis docs (notably the floor framing: the owner chose option 1, the audio file buffer itself), the synthesis wins.

# Hardware-fit re-audit (read-only, 2026-10-20)

Refreshes `docs/hardware-fit-audit.md` (2026-10-13). Code read at `.claude/worktrees/headers` `eb2f96dea5`. Sizes come from that worktree's existing `build/fw-ui` (dev, owner variant, `_end` 1,092,208) and `build/fw-rel` (release, `_end` 1,050,656) ELFs and maps, read with `scripts/sizemap.py` and `scripts/risk/reach.py --cut`. Nothing was built and nothing in the repo was edited. Scratch outputs: `hw_obj_ui.txt`, `hw_obj_rel.txt` in this folder.

Also read: `docs/data-paths.md` (R7, R8, D1–D14), `docs/pp5022-reference/` (quirks, open questions, clocks-power §4/§7/§8, cores-and-threads §6–7), `docs/bcm2722-reference/quirks.md`, `docs/hardware.md`, `docs/storage-choices.md`, `docs/changes-from-rockbox.md` (Fit-1/2/3, #81, #85, 8b-T, crash-fat rows), and memory audits A, B and C.

Tags: **[measured]** from an ELF, map, log or doc measurement; **[code]** read from source; **[est]** my estimate; **[hyp]** hypothesis. "Device" means the number can only come from the unit.

---

## 0. What changed since 2026-10-13

| Old ID | Item | Status now | Evidence |
|---|---|---|---|
| M0 | Measurement kit | **Done** (devstats, per-thread CPU, `idle_pct`, `cop_idle_pct`, `m0 tap2pcm`, disk wakes). Still missing: the inline USB-C meter session, HX-B6 (per-command latency), per-thread *wake* counts | ledger 2.8 |
| C1 | Storage idle hold | **Done**: 1 s on flash. The stopped-state wake storm was a commit retry's failed opens (D11, fixed) | ledger 2.8, D11 |
| C2 | Codec priority | **Done**: the ladder is compiled out; fixed bands (codec 13, video feed 14, buffering 15, UI 16) and time-bounded aging | `thread.c`, `pcmbuf.c` rows |
| C3 | Prefill | **Done**: 0.25 s on flash, 1 s on HDD | `pcmbuf.c:86-99` |
| C4a | No fill boost | **Done** on flash | `buffering.c:660` |
| C4b | ATA completion by IRQ | **Built** (`HAVE_ATA_DMA_IRQ`); whether source 23 fires is **unknown** (DP-4, PP #29) | R8 step 6 |
| C5 | Event-driven video feed | Partly: `vplayer.c:1029` waits with a computed timeout, the port waits still yield-poll. P14 | [code] |
| C6 | 24 MHz, PLL off | **Done**: unboosted runs 24 MHz unless a holder (lit screen, ATA DMA, USB, a track that does not fit) keeps the PLL. Lock time and the saving are unmeasured | clocks-power §8 |
| C7 | UDMA4 | **Closed, no gain**: 21 MB/s either way, plus a boost per command (D3) | S36c |
| C8 | BCM wake without a power cut | Open: needs stage 5i and our image as the default display driver | BCM #3 |
| C9 | BCM decodes AAC | Open (stage 9). BCM session | |
| C10 | BCM compositor | Partly: full-width pushes (≥ 16 rows) now go by the PP's **second DMA engine** (E2, #75/#76), so the CPU sleeps through them. The compositor itself is open | ledger `lcd-video.c` (#75/#76) |
| C11 | Codec on the COP | **Done** (Fit-2): CPU idle ~88–92 % against ~45 % on FLAC; COP idle 41–46 % | S36, fit2 §12 |
| C12 | Buffering fitted to flash | (a) **done**: 256 KB fills. (b) chapter-aligned fills still planned. (c) not done | ledger 2.8 |
| C13 | Own library store | Re-scoped as R6 below: the RAM cost is now set by #85's two snapshots | `hardware.md` #85 |

Done since then as well: 8b-T (cold Thumb, splash buffer on the stack, a 32-bucket disk-cache map), #85 (library generations, statistics journal), #81 (USB batching and serial ATA/USB, D12/D13), FAT ordered write-back, DAC standby (P15), the panic record, automatic battery learning (#70).

## 1. New findings that change the ranking

1. **Directory scans cost one ATA command per 512-byte sector.** `fat.c:2852-2862` (`fat_readwrite(dirstr, 1, ...)`) reads directories one sector at a time, and dircache is compiled out. Each command costs an estimated **L ≈ 1–3 ms** (`usb-speed.md` §4a; unmeasured, HX-B6). So every path walk without dircache (each track open, each `check_dir` in a scan, each entry of the post-USB deleted-file pass, each art-builder lookup) pays roughly 16 directory entries per command. On a 1 TB library a top-level folder of 3–4k artists with long names is ~1,000 sectors, about **1–3 s for a cold lookup near its end** [est]. This is the biggest generic-code cost left on the storage side, and nothing else touches it.
2. **The audio buffer is still a buflib block whose shrink stops playback** (`playback.c:1054`, upstream's own `TODO: Do it without stopping playback`). The shell is built around avoiding it: every reservation at shell start, a second drift slot taken only at start, the single view-pool block, deferrals while paused, and the rule "an allocation while paused restarts the queue". This is now the largest **simplicity** lever, and it also decides how Cover Flow textures, modules and Extras can allocate.
3. **The binding RAM case is #85's double library, not `_end`.** Measured in the sim at 50k tracks with both snapshots: peak 13,895,312 B, minimum free core heap **8,269,688 B on 32 MB**, which is **~0.77 MB above the 7.5 MB floor**. Audit A's steady-state margin (+13–15 MB on 32 MB) does not include the commit peak; audit C's "~0.8 MB" does. The tagcache RAM layout (~140 B/track, 96 of which is the index) is therefore the RAM lever that matters, ahead of any static.
4. **Library playback still uses Rockbox's path-based playlist.** `lib_play` and Shuffle Songs read up to 2,000 file names from the tagcache's file tag and write a text control file. The 2,000 cap exists because of it; upstream notes ~10 s for 2,000 entries on a PP5020 (`library.c:6407-6410`).
5. **Rockbox's stock UI is still resident in the release build** (§4): 12,910 B of image and 5,998 B of bss [measured, `reach.py --cut`], kept alive by three keystones in `apps/main.c` (`list_init`, `gui_syncstatusbar_init` through the `screens[]` vtable that pins `put_line`/`print_line`/icons/scroll engine, and `viewportmanager_init`), plus `gui_usb_screen_run` → `playlist_resume` → `action_userabort`.
6. **Settings persistence is Rockbox's text `.cfg` with temp-file plus rename** (`settings.c:605-720`). Each save rewrites the whole changed set line by line through `fdprintf` and does create / remove / rename / rename in `.rockbox`. The file starts with `# .cfg file created by rockbox` (a Rockbox string on disk; it is never shown, but it is there). `ab_store.c` already has the fitted pattern: one sector updated in place, no allocation, written while the disk is awake.
7. **The "a lit screen holds the PLL" rule predates E2.** The hold exists because a CPU push is 20 % slower at 24 MHz. Full-width pushes now go by DMA, so the rule can be re-measured (R10).
8. **Wake-ups nobody measured.** The buffering thread wakes every HZ/2 whenever handles exist, even when the buffer is full (`buffering.c:1639`). The power thread wakes at 2 Hz (`POWER_THREAD_STEP_TICKS`), and the battery ADC reads over I2C every 0.4 s (`adc-ipod-pcf.c`). M0 counts CPU time but not wakes per thread.

---

## 2. Subsystem by subsystem

Each entry gives what Rockbox does generically, what our hardware knowledge allows, the gain, risk, measurement, effort (S < 2 d, M ≤ 1 wk, L 1–3 wk, XL > 3 wk), phase, and whether it needs the BCM session.

### 2.1 Kernel: threads, scheduler, tick, idle, COP
- **Generic:** HZ 100 tick with tick tasks (`button_tick`, `adc` via powermgmt, `usb_tick`, `lcd_tick`, `timeout_tick`); the COP is woken by the CPU every tick; per-subsystem threads (UI, tagcache, buffering, audio, codec, usb, storage, backlight, power, piezo, scroll, plus the art builder and the COP worker).
- **Done:** fixed priority bands and time-bounded aging (C2); codec decode on the COP (C11).
- **Still generic:**
  - **K1 Wake discipline.** Event or deadline waits for buffering (sleep until the predicted time to the watermark instead of HZ/2), the power thread (2 Hz → event plus 5–10 s while steady on battery; battery learning only needs 1 s averages, which its own timeout already takes), and the battery ADC (0.4 s → follow the power thread). Gain: from ~4–5 wakes/s to ~0.5/s in dark playback [est]; battery < 0.5 % [est, unmeasured]; mostly hygiene. Risk low. Measure: add a per-thread wake counter to devstats, then a 30-min A/B. Effort S. **P18.**
  - **K2 Fewer threads.** The scroll thread (3 KB stack plus 1.7 KB of lines) is unused by the shell; it goes with RUI-1. Backlight (1 KB) and power (0.5 KB) could be one service thread. Gain ~5.5 KB and fewer contexts. Risk low–med. Effort S–M. **P9b** (with RUI).
  - **K3 Tickless / no per-tick COP wake:** still **not recommended** (under 1 %).
  - **K4 COP work beyond the codec:** see R13 (art builder) and the video soundtrack (DP-3, open). The COP is ~55 % busy on FLAC, so there is room.

### 2.2 Memory allocators (buflib, `core_alloc`, shell arenas)
- **Generic:** a single buflib pool; playback takes `core_alloc_maximum()`; any later allocation compacts and calls the audio buffer's shrink callback, which **stops and restarts** playback (a 340 ms gap measured; "paused restarts the queue").
- **R5 Late pool or a shrink without a stop.**
  - (a) Carve a fixed **runtime pool** (e.g. 1 MB on 64 MB, 384 KB on 32 MB [est sizes]) before `audio_reset_buffer()`, as its own buflib context or a sub-allocator. Transient and lazy shell allocations (panel transient, module slot, a second drift slot, commit helpers, USB stats) live there and never touch the audio buffer.
  - (b) Implement upstream's TODO: shrink the ring at its tail by dropping not-yet-started handles (keep the current track and the PCM ring), with no stop.
  - **Gain:** removes the stop/restart class of bugs and the "allocate only at shell start" rule. Shell-start reservations that are idle most of the time (second drift slot 180,000 B, panel transient 785,844 B while stopped) can become lazy. Simplicity: high.
  - **Cost:** (a) the pool's bytes come off the audio buffer for good, so they count against the floor. (b) costs nothing but is harder.
  - **Risk:** (a) low–med; (b) med–high (gapless, art handles).
  - **Measure:** `audio shrink want=` lines go to zero in a day's log; the floor trace at 50k on 32 and 64 MB.
  - **Effort:** (a) M; (b) L. **P16** (it unblocks P12 modules and P7 textures).
- **Compaction cost:** the shell's design avoids it; no measured problem. Keep.
- **Shell arenas** (8 KB screen arena, strip 180 KB): fitted already.

### 2.3 Buffering, playback, pcmbuf
- **Generic remaining:** a handle-based ring (`buffering.c`); watermark = 1 s of bitrate on flash, HDD formula otherwise; PCM ring 3 s, watermark 2 s; pcmbuf boosts when the PCM is low (`pcmbuf.c:549,573`, gated by clockpol).
- **B1 Chapter-aligned fills for spoken tracks** (old C12b, `hardware.md` policy). Gain: seeks within a chapter never reload; fewer wakes on large books. Risk low. Effort M. **P18** (or P9b).
- **B2 Watermark from runtime storage data** (`storage-choices.md` §3, one build): today a compile-time branch. Gain: one build. Risk low. Effort S. **P17.**
- **B3 Event-driven buffering thread:** in K1.
- **B4 Queue by database index (R4).** Replace the path-based playlist for library playback with a binary queue of database indices (4 B per entry), resolving paths lazily for only the next few tracks; the control file becomes one binary write.
  - **Gain:** play-all and Shuffle Songs start in milliseconds instead of seconds (2,000 file-name reads and control-file lines removed); the 2,000 cap goes; resume becomes an index plus an offset (good for P18's instant resume).
  - **Risk:** med–high (`playlist.c` is large: queue edits, bookmarks, resume, folder playback still path-based).
  - **Measure:** `m0 tap2pcm why=play` for a 2,000-row Shuffle Songs on the device.
  - **Effort:** L. **P16.**
- **Replacing buffering wholesale (old C12c):** still not justified.

### 2.4 Codecs and DSP
- **Done:** COP decode (Fit-2); hardware EQ, gain trim and DAC standby on the WM8758 (P15); dither only below the 16-bit LSB.
- **D1 Compile out DSP stages the shell never offers** (`HAVE_IPOD_UI` guards in `lib/rbcodec/dsp`): compressor 2,949 + 352 bss, surround 2,459 + 85, PBE 1,782 + 352, crossfeed 991 + 168, AFR 784 + 228 [measured image + bss] ≈ **10.1 KB**, plus `comp_curve` 264 B of core IRAM.
  - Keep the software EQ: P12's custom EQ (#82) may use a software engine.
  - Keep tdspeed (audiobook speed), resample (sources that are not 44.1 or 48 kHz) and replaygain unless P15's hardware gain trim covers every path (check first).
  - CPU: none while off, because Rockbox enables only active stages.
  - Risk low. Effort S. **P9b.**
- **D2 Codec slot right-size** (audit C §5): only code + bss is known; measure each codec's malloc high-water. Gain potentially 100–300 KB of floor or a free module slot [hyp]. Effort S (measure) + S (act). **P16** measure, **P9b** act.
- **IRAM:** the codec window is full (FLAC 81,008 / 81,920); core IRAM has 3.3–6.1 KB free (the budget table and audit C differ; reconcile). Use it only for one hot loop (R14).

### 2.5 Filesystem (FAT, disk cache, handles, path lookup) and ATA
- **Generic:** FAT32 with a 64 × 512 B sector cache; directories read one sector per command; path lookup walks each component linearly; no dircache (compiled out, ~3.9 MB at 50k); 31 file and 32 directory slots (static); temp-file-plus-rename idioms throughout `apps/`.
- **R1 Directory reads in bursts.** Read directory sectors in runs (one cluster = 16 KiB = 32 sectors, or at least 4 KiB) into a per-scan buffer or the disk cache, with one ATA command.
  - **Gain:** up to 32× fewer commands per directory scan [est]: scans, track opens, the deleted-file pass, art lookups. At L ≈ 1–3 ms a 1,000-sector folder goes from 1–3 s to ~35–100 ms. Fewer commands also means fewer adapter wakes. Identical on HDD (fewer seeks).
  - **RAM:** a 16 KiB buffer must not be a static. Use a transient from the tagcache thread's existing I/O block or the disk cache (fill 8 slots with one command, 4 KiB): 8× fewer commands for 0 B.
  - **Risk:** low–med (`fat.c` core; coherence with the cached directory writes in `cache_sector`, which a disk-cache-based design keeps by construction).
  - **Measure:** devstats `disk_cmds` for one scan and `db scan ms=` on the device, before and after; the sim's disk model for the command count.
  - **Effort:** M. **P17** (benefits P16).
- **R3 Last-parent cache.** Remember the last N (e.g. 4) parent directories (path hash → directory cluster) so the next track in the same album skips the walk; drop it on any directory write and at USB. Gain: a track open goes from (depth) directory scans to one. RAM < 0.5 KB. Risk low. Effort S. **P17.**
- **R2 Open by stored location.** At scan time store each track's directory cluster, entry index and first cluster in the database (pre-compute at sync time); open by location, verify the short name and the first cluster, and fall back to the path walk.
  - **Gain:** a track open costs one sector read. The post-USB deleted-file pass, estimated at 2–8 min of disk activity at 50k (`hardware.md`), becomes 50k location checks sorted by sector (sequential, batchable) [est: tens of seconds].
  - **Risk:** med (stale after host edits; mitigated because every USB session already ends in the deleted-file pass, which this design speeds up).
  - **Effort:** L. **P16.**
- **F4 Handle tables:** `MAX_OPEN_DIRS` 32 → 16 after a high-water log (7,168 B, audit A). Low value under the floors; **P9b**.
- **F5 Cluster size 16 → 64 KiB** (`fat32-migration-plan.md`): FAT 250 → 62.5 MB; macOS's mount sweep ~11–12 s → ~3 s; fewer FAT sectors per file. Needs a verified backup and a reformat. Risk high (data). Effort M plus a session. **P17.**
- **ATA:**
  - **A1 Measure the per-command latency L (HX-B6, PP #28/DP-5).** Every estimate in R1/R2/F5 scales with it. S plus a session. **P17.**
  - **A2 Confirm the IDE IRQ (DP-4)**: read `m0 disk ... irq=` from one session. If it never fires, the CPU still polls through every DMA. S. **P17.**
  - **A3 Power off the adapter after a long idle** (`HAVE_ATA_POWER_OFF` chosen at runtime when the drive is solid; `storage-choices.md` says it is a harmless pin action on flash). Gain: the adapter's standby current goes to 0. Unknown: an iFlash in STANDBY may draw ~1–10 mA [hyp], which against a playback draw of tens of mA could be several % of runtime. Cost: re-init latency on the next access (unknown). Risk med (wake path). Measure: meter A/B, wake time. Effort S plus a meter session. **P18.**
  - UDMA4: closed (D3).

### 2.6 USB stack
- **Done:** 4 KiB dTDs, prime retry, endpoint reset, serial ATA/USB (D12), load-before-send (D13), demand batching, static INQUIRY, the prefetch ring (worker off by default).
- **Remaining:** F5 (cluster size) is the biggest mount-time lever; A1 decides whether batching can do more. Runtime storage choice of the sector multiplier and the prefetch wait (`storage-choices.md` §2/§7). **P17.** Nothing generic is worth replacing here now.

### 2.7 LCD path (BCM bridge, framebuffer, pushes)
- **Done:** E2 DMA for full-width bands (the CPU sleeps through a push); partial updates in the shell; the strip and row cache.
- **L1 Re-measure the 24 MHz rule with a lit screen (R10).** With E2 moving the pixels, a lit screen may not need the PLL. Gain: PLL power plus 20 % fewer cycles during all screen-on use (browsing is where the screen is lit) [hyp, size unknown]. Risk low–med (animation frame budget; boosts on input still go to 80 MHz). Measure: E2's rate at 24 MHz, the frame time of a list glide, then a power A/B. Effort S plus a session. **P18.**
- **L2 Our BCM image as the default display driver.** This is the gate for C8 (wake ~500 ms → ~30 ms, no image re-upload per wake), the BCM-side second frame (#76, 0 PP RAM), the strip framebuffer (the PP's 153,600 B framebuffer → ~20 KB, about **−130 KB of floor** [hyp]), Cover Flow textures in BCM SDRAM (**−512 KB of floor**), and the compositor (C10). **BCM session.** Effort XL in total. **P19.**
- **L3 Narrow updates** (under 16 rows) still go by CPU copy: small, leave as they are.

### 2.8 Tagcache and library
- **Generic:** tagcache keeps one file per tag, a master index (~96 B/track in RAM) and strings (~44 B/track) in RAM: 6,993,248 B at 50,033 tracks [measured, sim]. #85 builds the candidate while the active one stays loaded: two snapshots at the commit peak.
- **Done:** library precompute (`database_lib.dat`), view pool, generations, journal; the commit never steals from playback.
- **R6 Fit the RAM library to the shell.**
  - (a) Load into RAM only the fields and tags the shell reads (check `comment`, `grouping` and unused numeric fields; strings of tags it never shows stay on disk).
  - (b) A packed index (16-bit fields where the range allows) for the shell build.
  - (c) During a commit, build the candidate as a delta or stream it instead of a second full RAM snapshot.
  - **Gain** [est]: (b) 96 → ~48 B/track = −2.4 MB per snapshot at 50k, about **−4.8 MB at the commit peak**, which is the 32 MB floor case (margin today ~0.77 MB). (c) alone removes up to ~7 MB of peak.
  - **Risk:** high (tagcache core, statistics, resume, the gate's library scripts).
  - **Measure:** a per-tag RAM breakdown at 50k (sim, then the devmode buffer screen); the floor trace on 32 MB.
  - **Effort:** (a) M; (b) and (c) L–XL. **P16.**
- **Scan cost:** dominated by R1 (directory reads) and metadata parse. Keep the tagcache scanner otherwise.
- **Deleted-file pass:** R2.

### 2.9 Fonts and text rendering
- **Generic:** Rockbox `.fnt` files held whole (101,354 B); 4-bit anti-aliasing blended by the generic `lcd-16bit` alpha path; `font_cache.o` (808 B) for the stock font only.
- **R14 RGB565 × 4-bit alpha fast path:** a specialised ARM loop (in the 3–6 KB of free core IRAM, if the budget allows) for the shell's glyph blits and shadows. Gain: an estimated 2–3× faster text draw [est], so fewer boosted frames during glides and drift. Risk low–med (pixel parity: `scripts/pngdiff`). Measure: devstats CPU per glide frame on the device; sim pixel diff = 0. Effort M. **P19** (or P9b).
- Glyph atlases on the BCM: later, with L2 (saves port and CPU, not PP RAM).

### 2.10 Settings and config I/O
- **Generic:** `settings_list.c` (12,203 B; the whole Rockbox settings table, mostly hidden) and `settings.c` (4,250 B + 2,168 bss); a text `.cfg` rewritten through temp plus rename; `.resume.cfg` every 5 min while the disk is awake.
- **R8 Binary settings and resume record:** a preallocated file with two sector slots (sequence + CRC), updated in place like `ab_store`; a one-time import of the old `config.cfg`; a table holding only the settings ProkBox reads.
  - **Gain:** each save becomes one sector write with no directory operations (today a create, a write, a remove and two renames in `.rockbox`, which is also a crash-consistency exposure). Code −12–16 KB with RUI-4 [est]. The Rockbox header string goes from disk.
  - **Risk:** med (migration; developer access to hidden Rockbox settings).
  - **Measure:** directory writes per save in the sim's disk model; a cut-power test (`tests/ui/*commit-cut*` style).
  - **Effort:** M. **P16 / P18** (instant resume).

### 2.11 Logging
- `uilog` (768 B ring, flushed while the disk is awake), the panic record, the developer power log as a `core_alloc`. Fitted already. Only follow-up: `sizemap.py` misattributes the merged 21.6 KB string pool to `debug_menu.o` (audit B); fix the tool before the next size audit. S. Tooling milestone.

### 2.12 Power management
- **Done:** 24 MHz with the PLL off (C6), DAC standby, battery learning, disk-safe floors.
- **Open:** K1 (power thread and ADC cadence), R10 (24 MHz while lit), A3 (adapter power-off), C8 (BCM sleep). DVFS stays out (PMU part and voltage formula unknown, PP #20). The inline-meter session (M1) is what turns every one of these into a number.

### 2.13 Code loader
- Studied separately (audit C): shell modules on Rockbox's `lc_open`, slot options (a) a static slot or (b) carved from the codec slot (needs D2), developer UI as the first tenant. Referenced only; RUI-3 below is its natural first module.

---

## 3. Rockbox UI layer: what it still costs, and a plan to retire it (owner, 2026-10-20)

### 3.1 Measured resident cost

`reach.py --cut` frees the cut objects plus everything only they reach ("also"). Numbers are image + bss bytes [measured]:

| Cut set | fw-ui (developer) | fw-rel (release) |
|---|---|---|
| `apps/gui/*` + `menu.o` + `debug_menu.o` + `screens.o` + scroll engine | **52,628 + 7,597 = 60,225** | 12,910 + 5,998 = **18,908** (gui + scroll engine; `menu`/`debug_menu`/`screens` absent) |
| same + `devmode.o` + `udmatest.o` (dev screens to be rewritten) | 61,400 + 7,801 = 69,201 | n/a |
| same + `settings_list.o` + `action.o` + `keymaps/*` + `core_keymap.o` + `screen_access.o` (glue; needs replacements) | 72,713 + 7,605 = **80,318** | 32,995 + 6,006 = **39,001** |

Per object [measured, sizemap image/bss, fw-ui]: `debug_menu` 26,812 (of which ~21.6 KB is the merged string pool that belongs to every object); `settings_list` 12,203; `settings` 4,250/2,168; `menu` 2,303/264; `gui/list` 2,192/1,217; `gui/line` 2,033/144; `action` 2,127; `option_select` 1,704; `bitmap/list` 1,544/80; `keymap-ipod` 1,439; `splash` 1,147; `scrollbar` 876; `icon` 836/64; `screen_access` 680; `viewport` 616/132; `core_keymap` 284; `usb_screen` 184; `statusbar` 52/84; `scroll_engine` 1,077/5,481; `backdrop` 108. Language: ~327 entries used only by stock apps files (~4.6 KB) and ~238 only by `settings_list` (~3.8 KB) [audit B, upper bound].

**What keeps it alive in release** (`reach.py --paths`, fw-rel):
- `main > gui_syncstatusbar_init > screens > screen_helper_put_line > vput_line > print_line > {screen_put_iconxy, put_text > scroller_main}`: the `screens[]` vtable pins `line.o`, `icon.o` and the scroll engine.
- `main > viewportmanager_init > viewportmanager_theme_enable > viewport_set_defaults`.
- `main > gui_usb_screen_run > playlist_resume > action_userabort > get_custom_action > get_action_worker / do_backlight`.
- `main > splashf` (the funnel stub, 56 B) and `main > list_init`.

**Shell features that still depend on it:**
- Developer mode: `devmode.c`, `udmatest.c`, `devbcm.c` and the stock `debug_menu` screens (OS stacks, buffer, battery, disk info, BCM device test, USB stats) use `simplelist`, `option_screen`, `do_menu` and splashes.
- The splash funnel: upstream code (playlist, tagcache, main) still calls `splash*`; under the shell it returns early (logs), and only the developer UI gets past it.
- Settings storage: `global_settings` (fine to keep as a struct), but `settings_list` supplies defaults, `.cfg` names, `find_setting()` for `reset.c`, and `settings_save()` for every shell setting screen (14 call sites).
- The action layer: the shell reads the wheel through `button_get_w_tmo` directly (`input.c:145`). Only stock paths use `action.o` (`playlist_resume`'s abort check, `do_backlight`).
- The USB screen wrapper: `gui_usb_screen_run` (stock) calls the shell's `ui_usb_screen_loop`.

### 3.2 Staged plan (roadmap item "RUI: retire the Rockbox UI", proposed as a **P9b step**, with RUI-3 as the module spike's tenant)

| Stage | What | Resident saving | Risk | Effort |
|---|---|---|---|---|
| RUI-0 | Fix `sizemap.py`'s string-pool attribution; add `reach.py --cut` for these sets to the size report | 0 (accurate numbers) | none | S |
| RUI-1 | **Break the release keystones**: no `gui_syncstatusbar_init`, `viewportmanager_init` or `list_init` under `HAVE_IPOD_UI && !DEVMODE`; slim the `screens[]` vtable (no `put_line`); the shell's USB entry calls `ui_usb_screen_loop` directly; `playlist_resume`'s abort through a shell hook | **~18.9 KB release** (12,910 image + 5,998 bss), the scroll thread gone | low–med (boot order, the USB entry rule "no disk writes at entry") | S–M |
| RUI-2 | **ProkBox developer UI kit**: three primitives drawn by the shell's own list renderer (`devui_list`, `devui_value`, `devui_confirm`, ~3–5 KB [est]), log-style info pages; port `devmode.c` rows first | −3 to −5 KB (cost) | low | M |
| RUI-3 | Rewrite the stock debug screens the owner uses (stacks, buffer, battery, disk/IDENTIFY, USB stats, BCM device test, udmatest) on the kit; delete `debug_menu`, `menu`, `option_select`, `gui/list`, `bitmap/list`, `scrollbar`, the stock splash body. Optionally as the first shell module (audit C) | **~55–60 KB developer build** (60,225 − kit) | med (feature parity of the developer tools) | M–L |
| RUI-4 | **Own settings table** (R8): only the settings ProkBox reads, binary A/B storage, a one-time import; drop `settings_list` and the `.cfg` writer; the developer UI shows raw values | **~12–16 KB** both builds [est] + ~3.8 KB lang | med (migration) | M |
| RUI-5 | Drop `action.o`, the keymaps and `core_keymap` (backlight-on-input moves to the shell's input path); prune lang entries only stock code used (keep the language backend, rule 8) | **~3.8 KB** + up to ~4.6 KB lang | low–med | S–M |

**Totals [measured upper bound for the cut sets, est for replacements]:** release **~35–39 KB** resident (≈ 39,001 cut minus the small replacements); developer build **~75–80 KB**. On a 32 MB unit that is ~5 % of the commit-peak floor margin. The bigger gain is simplicity: one UI toolkit, one input path, no Rockbox strings left to police, and the developer UI becomes movable into a module.

Device-reality notes: RUI-1 changes boot order (the boot logo stays up; check the boot and USB entry on the device); RUI-3's screens need a device pass of each developer tool; RUI-4 needs a power-cut test and an upgrade-in-place test from an existing `config.cfg`.

---

## 4. Ranking by value to the owner

Value weights battery, responsiveness, RAM-floor margin and simplicity, discounted by risk and by unproven gains.

| Rank | ID | What | Owner value | Gain (tag) | Risk | Effort | Phase | BCM? |
|---|---|---|---|---|---|---|---|---|
| 1 | M1 | Measurement session: inline USB-C meter (per-state mA), HX-B6 latency L, IDE IRQ (DP-4), per-thread wake counter, codec malloc high-water | enables every number below | n/a | low | S + 1 session | P17/P18 | no |
| 2 | R1 | Directory reads in bursts (`fat.c` dir scan) | responsiveness (opens, scans, deleted pass), fewer wakes | ÷8–32 commands per directory [est] | low–med | M | P17 (P16 benefits) | no |
| 3 | R5 | Late pool, or a shrink without a stop, for the audio buffer | simplicity, responsiveness (no 340 ms gaps), lazy reservations | removes a bug class; up to ~1 MB of lazy reservations [est] | med | M (a) / L (b) | P16 | no |
| 4 | R6 | Fit the RAM library (fields, packed index, no second full snapshot) | **32 MB floor** (margin ~0.77 MB at the commit peak) | −2.4 to −7 MB peak at 50k [est] | high | M→XL | P16 | no |
| 5 | R4 | Queue by database index instead of path playlists | responsiveness (play/shuffle start), no 2,000 cap, instant resume | seconds → ms [est] | med–high | L | P16 | no |
| 6 | RUI-1..3 | Retire the Rockbox UI behind ProkBox's developer UI | simplicity, native illusion | release 18.9 KB, dev ~60 KB [measured cut] | low→med | S→L | P9b step | no |
| 7 | R3 | Last-parent directory cache | responsiveness (next-track open) | depth→1 scans per open [est] | low | S | P17 | no |
| 8 | R10 | 24 MHz with a lit screen now that E2 pushes | battery during screen-on use | unknown until M1 | low–med | S + session | P18 | no |
| 9 | A3 | Adapter power-off after long idle (runtime, solid drives) | battery (stopped and dark playback) | unknown until M1 [hyp: several %] | med | S + session | P18 | no |
| 10 | R2 | Open by stored FAT location; deleted-file pass by location | responsiveness (open = 1 sector), post-USB pass minutes → tens of s [est] | | med | L | P16 | no |
| 11 | R8 / RUI-4 | Binary A/B settings and resume record, own settings table | crash safety, fewer writes, −12–16 KB | | med | M | P16/P18 | no |
| 12 | L2 | Our BCM image as the default display driver → C8 wake, #76 second frame, strip framebuffer, textures | responsiveness (wake 500→30 ms), floor (−130 KB FB, −512 KB textures) [hyp] | | high | XL | P19 | **yes** |
| 13 | K1 | Deadline waits for buffering, power thread, ADC | battery (small), hygiene | ~4–5 → ~0.5 wakes/s [est] | low | S | P18 | no |
| 14 | R13 | Art builder decode on the COP | responsiveness during builds, scan wall time | −30–45 % build time [est] | med | L | P19 | no |
| 15 | F5 | 64 KiB clusters (migration plan) | USB mount time (~12 → ~3 s), FS metadata | [est from arithmetic] | high (data) | M + session | P17 | no |
| 16 | R14 | RGB565 × 4-bit alpha fast path | fewer boosted frames | 2–3× text draw [est] | low–med | M | P19 | no |
| 17 | D2 | Codec slot right-size (measure peaks) | floor | 100–300 KB [hyp] | low | S+S | P16 → P9b | no |
| 18 | D1 | Compile out unused DSP stages (keep EQ, tdspeed, resample) | size, IRAM | ~10.1 KB + 264 B IRAM [measured] | low | S | P9b | no |
| 19 | B1 | Chapter-aligned fills for spoken tracks | fewer reloads in books | | low | M | P18 | no |
| 20 | B2 | Watermark, prefill and boost chosen at runtime from the drive | one build (rule 7) | | low | S | P17 | no |
| 21 | K2 | Merge housekeeping threads (scroll goes with RUI-1) | simplicity, ~5.5 KB | | low–med | S–M | P9b | no |
| 22 | C9 | BCM decodes the video soundtrack | video CPU | | med–high | L | P14/P19 | **yes** |
| 23 | C10 | BCM compositor (after L2) | animation CPU, PP framebuffer | | high | XL | P19 | **yes** |
| 24 | F4, audit A/B statics | `MAX_OPEN_DIRS`, bidi buffers, load-on-demand rodata | small floor bytes | 3–28 KB | low–med | S each | P9b | no |

Not recommended now: a tickless kernel (K3); DVFS (PMU unknown); UDMA4 (D3); PP general-DMA into the port (D1: use E2); replacing buffering wholesale; removing diagnostic strings.

### Quick-win bundle (about a week, one mini-phase, guarded `HAVE_IPOD_UI` edits, ledger rows)
M1's firmware parts (wake counter, codec high-water log), R3, K1, D1, B2, RUI-0 and RUI-1. Then one owner session covers M1 (meter, HX-B6, DP-4), R10 and A3 as developer A/B switches.

### Medium (2–4 weeks)
R1 (needs A1's L to size the burst), R5(a), R8 with RUI-4, RUI-2/3, B1.

### Large, each with its own brief and review
R6, R4, R2, R13, F5 (with the owner's backup), L2 and the BCM chain (BCM session).

---

## 5. Device measurements this needs (procedures go in each brief)

1. **Meter session (M1):** battery full, charger current deltas: screen lit vs off; 24 vs 30 vs 80 MHz unlit; 24 vs 30 MHz lit (R10); adapter active / standby / powered off (A3); BCM on vs cut. Feeds `BC_BUSY_W` and every battery claim.
2. **HX-B6:** the same 64 MB read as 1-, 8-, 64- and 256-sector commands at UDMA2: gives L (R1, R2, F5) and settles DP-5.
3. **DP-4:** one playback; read `m0 disk sleeps=… irq=I`.
4. **Wakes per thread** (new counter): 30 min dark playback before and after K1.
5. **Scan and open timings:** `db scan ms=`, `disk_cmds` per scan, and a cold-open time for a track in a large folder (R1, R3, R2).
6. **Codec peaks:** malloc and TLSF high-water per codec on the worst files (D2).
7. **Floor trace:** buffer bytes after shell start and at the commit peak, 50k library, 32 and 64 MB (R5, R6).
8. **Play-all start:** `m0 tap2pcm why=play` for a 2,000-row Shuffle Songs (R4).
9. **RUI:** a boot with the keystones removed (logo stays, USB entry has no disk writes); each rewritten developer tool once.

## 6. Discrepancies to resolve

- **Floor margin:** audit A computes +13–15 MB above the 32 MB floor (steady state); #85's sim peak leaves ~0.77 MB (commit with two snapshots). The gate should measure the commit peak (audit C §4.1).
- **Core IRAM free:** 3,344 B (`hardware.md` budget table, 45,808 / 49,152) against 6,128 B (audit C: 43,024 used). Re-measure before planning R14 in IRAM.
- `docs/hardware-fit-audit.md` still says C1/C3/C7/C12 "on flash" in flag terms; `storage-choices.md` §10 already lists the rewording to "solid (measured) / rotating (assumed)".
