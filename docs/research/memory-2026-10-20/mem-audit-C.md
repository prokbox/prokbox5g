> Research record, 2026-10-20 (read-only audit; inputs to docs/memory-audit-2026-10-20.md, docs/design/module-loader.md and docs/hardware-fit-audit.md). Paths to `scratchpad/` refer to the session's scratch area and are not kept. Where it conflicts with the synthesis docs (notably the floor framing: the owner chose option 1, the audio file buffer itself), the synthesis wins.

# Memory audit C: can cold features stop being resident? (read-only, 2026-10-20)

Scope: the PP5022 RAM layout, Rockbox's code-loading path, how a "shell module" (overlay) could work, the alternatives, and (after the owner's two decisions of 2026-10-20) where the BCM2722's memory and IRAM fit in.
Sources were read in `.claude/worktrees/headers`. Measurements come from snapshots of `build/fw-ui` (dev, owner variant, `USE_THUMB`, trims on) and `build/fw-rel` (sealed release), copied to `scratchpad/snap/` and taken at 15:47/15:51. Nothing was built and no files were edited.

Evidence tags: **[measured]** = from the ELF, map or files; **[code]** = read from source; **[doc]** = from a repo doc and its own tag; **[est]** = my estimate, needs measuring on the device.

---

## 0. The new rule and what it means (owner, 2026-10-20)

- The `_end` cap goes from 1,100,000 to **1,500,000 B**. The binding rule is now the **audio-buffer floor**, measured after every runtime reservation the shell makes:
  - **≥ 7,500,000 B** on 32 MB units;
  - **≥ 39,000,000 B** on 64 MB units.
- Today, using the brief's figures (~8.3 MB on 32 MB, ~40 MB on 64 MB), the **margin is about 0.8 MB on 32 MB units and about 1 MB on 64 MB units** [est, from the brief's numbers; not re-measured here]. That margin is the whole budget for:
  - resident code growth (every byte of `_end` is a byte of audio buffer: `app-pp.lds:160-181`, the buffer runs from `_end` to `ENDAUDIOADDR`);
  - new static slots (plugin or module slot, codec slot);
  - new runtime reservations made at shell start (fonts, art blocks, drift slot, library pool, RAM cache).
- Demand ahead:

  | Item | Size |
  |---|---|
  | P16 | 8-12 KB |
  | P12 | 60-100 KB |
  | P13 | ~15 KB |
  | P7 code | ~30 KB |
  | P7 Cover Flow textures | **~512 KB** |
  | P14 player + Videos shell, today | ~43 KB (`hardware.md:229`: +23,675 player, +19,590 shell) |
  | **Total if everything is resident ARM and textures sit in PP RAM** | **~0.7-0.75 MB** |

  On a 32 MB unit that uses almost the whole margin. **The cap raise does not remove the pressure on 32 MB units; it moves it to the floor.** The levers below still matter, ranked by bytes saved per unit of risk.

---

## 1. Rockbox's loadable-code mechanism on this target

### 1.1 RAM layout (`firmware/target/arm/pp/app-pp.lds`)

- `DRAMSIZE = MEMORYSIZE·1 MiB − PLUGIN_BUFFER_SIZE − CODEC_SIZE` (`:14`). From the bottom up:
  - `.text/.rodata/.data/.bss/.ncbss`, then `_end` (`:160-165`);
  - the audio buffer, `_end` → `ENDAUDIOADDR` (`:167-181`);
  - the codec slot `codecbuf` at `ENDAUDIOADDR` (`:184-188`);
  - the plugin slot `pluginbuf` at `ENDADDR = ENDAUDIOADDR + CODEC_SIZE` (`:23`, `:190-194`).
- Both slots are **static carve-outs at the top of RAM, outside `_end`**. Each costs audio buffer byte for byte, but neither shows in the `_end` gate (`validate.sh:1030-1060` reads only `_end`). **Any module slot must be counted in the floor measurement, or it games the metric.**
- `.init` (19,520 B [measured]) is linked at `ENDAUDIOADDR` (`:110-117`), inside the codec slot, and is overwritten after boot. Already free.
- Measured fw-ui:

  | Item | Bytes |
  |---|---|
  | `.text` | 645,696 |
  | `.rodata` | 133,516 |
  | `.data` | 8,472 |
  | `.bss` | 299,644 |
  | `.ncbss` | 4,852 |
  | `_end` | **1,092,208 (0x10AA70)** |
  | `codecbuf` | 0x03F00000 |
  | `pluginbuf` | 0x04000000 (slot size 0) |

  fw-rel `_end` is **1,050,656**, so developer mode costs **41,552 B of `_end`** (sizemap diff: devmode-stock-ui +34,191, shell +13,539, stock-menus +2,303, firmware +1,800 image).
- The largest bss items [measured]:

  | Symbol | Bytes |
  |---|---|
  | `lcd_static_framebuffer` | 153,600 |
  | `cache_buffer` | 32,768 |
  | `tagcache_stack` | 17,408 |
  | `open_streams` | 11,520 |
  | `buffering_stack` | 9,216 |
  | `codec_stack` (IRAM) | 9,216 |
  | `static_id3_entries` | 8,352 |

  These belong to other audits, but the framebuffer matters for the BCM section (§6.3).

### 1.2 Plugin slot

- `firmware/export/config/ipodvideo.h:112-118`: with `IPODUI_TRIM_PLUGINS`, `PLUGIN_BUFFER_SIZE = IPODUI_SCRATCH_SIZE`, otherwise 0x80000 (512 KB).
- `ipodvideo-variant.h:151-156`: `IPODUI_SCRATCH_SIZE` is **0** when PLUGINS, BROWSER, SKIN and TAGTREE are all trimmed (the shipped set), else 0x14000.
- So today the **plugin slot is 0 B**. Phase 9 returned 524,288 B to the audio buffer (`hardware.md:108`).
- `apps/scratchbuf.c:36-60` provides `plugin_get_buffer()` over that slot.

### 1.3 How a `.rock` loads (`apps/plugin.c:900-1052`, `firmware/lc-rock.c:29-95`)

- **No relocation.** Each plugin is linked at the fixed address `pluginbuf` (`apps/plugins/plugin.lds`). `lc_open()` reads the header, checks `load_addr..load_addr+copy_size` against the buffer, reads the whole file to `hdr.load_addr`, and calls `commit_discard_idcache()`. It also flushes the COP cache first (`lc-rock.c:42-48`).
- `plugin_load()` then checks four things: `PLUGIN_MAGIC`, `TARGET_ID`, `load_addr == pluginbuf`, `end_addr ≤ pluginbuf + size` (`:967-976`). It also checks `PLUGIN_API_VERSION` (285) and `api_size` (`:979-987`).
- It writes `&rockbox_api` into the plugin's `rb` pointer (`:997`) and calls `entry_point` on the **UI thread**. Playback threads keep running.
- `plugin_crt0.c:79-96` copies the plugin's IRAM and zeroes its bss.
- The API table `rockbox_api` (`plugin.c:189`) is **1,676 B** (0x68c, 419 pointers) in the stock `fw` [measured]. The codec table `ci` is 164 B in fw-ui [measured].
- **Big-memory plugins take the audio buffer.** `plugin_get_audio_buffer()` calls `core_alloc_maximum()` (`plugin.c:1166-1177`, "Playback gets stopped"). That breaks "paused is normal", so no shell module may use it.
- **IRAM:**
  - plugin and codec IRAM share 0x4000C000-0x40020000 (80 KB, `plugin.lds:33-37`);
  - core IRAM is 0x40000000-0x4000C000 (48 KB, `app-pp.lds:17-18`);
  - a shell module must have **no IRAM** (enforce with `ASSERT(SIZEOF(.iram)==0)`), because the codec owns that region during playback.

### 1.4 Codec slot (the precedent and a lever)

- `CODEC_SIZE 0x100000` (`ipodvideo.h:109`).
- Codecs load at the fixed `codecbuf` (`apps/codecs.c:209-233`).
- `codec_get_buffer_callback()` gives the codec **everything left in the slot** (`codecs.c:169-181`):
  - `codec_malloc()` is a bump allocator over that (`lib/rbcodec/codecs/lib/codeclib.c:35-73`);
  - Vorbis puts a TLSF pool over all of it (`libtremor/oggmalloc.c:16-24`).
- So the **real peak use per codec is not known**; only code + bss is [measured]:

  | Codec | Code + bss |
  |---|---|
  | wma | 293,752 |
  | aac | 198,900 |
  | raac | 194,564 |
  | aac_bsf | 190,988 |
  | opus | 189,184 |
  | vorbis | 151,748 |

- Fit-2 already keeps back the top of the slot for the COP worker (`codec_slot_top()`, `COP_RESERVE`, `codecs.c:55-57, 183-188`). That is a precedent for carving from it.

### 1.5 What the shell build trimmed, and `--ipodui-plugins`

- `--ipodui-plugins` defines `HAVE_IPOD_UI_PLUGINS` (`tools/configure:4918-4924`). It unsets the PLUGINS block (`ipodvideo-variant.h:66-80`), which restores the 512 KB slot.
- Rockbox's plugin API pulls back its closure: BROWSER, STOCK_MENUS, KEYBOARD, SKIN, QUICKSCREEN, VOICE (+TAGTREE) (`ipodvideo-variant.h:113-118`, `scripts/trim-deps.txt`).
- Phase 9's cuts for that closure (`hardware.md:91`): browser 74,332 + skin 50,590 + voice 38,189 + stock menus 26,265 + tagtree 12,804 ≈ **202 KB image**, plus keyboard and quickscreen.
- **So Rockbox plugins as such cost ≥ 200 KB resident + a 512 KB slot ≈ 0.7 MB of audio buffer**, which equals the whole 32 MB margin. Stock plugins also draw Rockbox UI (lists, splashes). **For #99: don't use Rockbox plugins for Games; build games as shell modules** (§2).
- Stock plugin sizes for scale [measured, `build/fw/apps/plugins`]:

  | Plugin | Bytes |
  |---|---|
  | stopwatch | 3,100 |
  | alarmclock | 2,624 |
  | calendar | 8,496 |
  | brickmania | 53,068 |
  | jewels | 53,964 |
  | rockblox | 164,204 |
  | solitaire | 218,668 |
  | clock | 480,720 (mostly bitmaps) |

### 1.6 Shell overlays: feasible, with one memory model that works

- **Where the code lives while loaded:**
  - **(a) A dedicated static slot (reuse `pluginbuf`).** The linker script and `lc_open` need no change: set the slot to e.g. 48 KB. It costs the slot permanently, and is counted in the floor.
  - **(b) Carved from the codec slot.** `CODEC_SIZE = 1 MiB − S`, `PLUGIN_BUFFER_SIZE = S`; the total static carve is unchanged, so it costs **0 B of audio buffer**. It is valid only if the worst codec's peak (code + bss + malloc high-water) fits in `1 MiB − S` with margin. That needs the measurement in §5.
  - **(c) A `core_alloc` on entry.** Rejected: once playback has a buffer, any new allocation shrinks it, which is a stop or gap (`hardware.md:73, 102-103`: a 340 ms gap; "an allocation while paused restarts the queue"). It breaks "paused is normal".
  - The one allowed exception is the video session: video start is a deliberate stop of music, not a pause.
  - **(d) The screen arena** (8 KB, `screen_stack.h:170-198`) is too small. Module screen states still use it as they do today.
- **Playback while a module runs:** it keeps playing.
  - The module runs on the UI thread from SDRAM.
  - Its slot never touches the audio buffer, the codec slot (with (a)) or codec IRAM.
  - `cpu_boost` discipline is the same as for resident screens.
- **Load time:**

  | Storage | Time |
  |---|---|
  | Flash (adapter ~21 MB/s [doc `hardware.md:319`]), 48 KB | ~2.3 ms of transfer + FAT open in the hidden folder (~1-3 ms) + header/size/checksum check (~2-6 ms boosted) ≈ **5-15 ms** [est] |
  | Awake HDD | ~20-30 ms [est] |
  | Sleeping HDD | **+1-3 s spin-up** (`storage-choices.md:81`) |

  - Mitigations:
    - single-slot caching: the last module stays loaded, so re-entry is free;
    - opportunistic fill: when the disk is spinning anyway (boot window, after each audio refill), load the likely module if the slot is empty;
    - prefetch when the parent menu opens, only if `storage_disk_is_active()`.
  - Then a sleeping-disk first entry is rare, and the user sees the same thing on both storage types, only the wait differs, as for any read.
- **USB screen:** stays resident (no disk access at USB entry, the owner's rule). No module loads during a USB session.
  - A module whose file the host replaced is caught by a **build-ID** check: core and module come from one build and one zip.
  - On a mismatch the module is refused, the failure is logged, and the screen stays on its parent with no splash.
- **Art builder, library commits:** no interplay with a static slot. The builder has its own decode block; commits use transient allocations only when stopped.
  - Background jobs must **not** share the UI slot (two owners). If the art builder, P16 maintenance and the P13 mix generator ever move out, that is a second "job slot". It isn't worth it today: the art builder's ~62 KB (artcache 35,042 + artfmt 9,333 + jpeg_load 11,808 + resize 3,404 + bmp 2,952 [measured]) ≈ its slot size.
- **Codecs:** with (a) they are untouched. With (b) the codec slot shrinks for good; codecs are never asked to give memory back mid-track, because the slot is fixed at boot.

---

## 2. Costs

| Item | Cost | Basis |
|---|---|---|
| Loader | ~0 new (`lc_open` is resident for codecs) + ~0.6-1 KB glue (registry, refcount, build-ID/size/checksum, logging) | [code] + [est] |
| API, option T: generated table | ~4 B per core callee resident; a dev UI with ~300 callees → 1.2 KB; module side ~12-16 B per callee for a v4T `ldr ip; bx ip` stub (no `ldr pc` interworking on ARMv4T) | [est] |
| API, option J: `-Wl,--just-symbols=rockbox.elf` | **0 B table**, direct calls. The linker adds long-branch/interworking veneers (8-12 B per callee, in the module). The slot at top of RAM is >32 MB from the core on 64 MB units. The core must keep module-only callees alive under `--gc-sections` (a generated `--undefined` list) | [hypothesis]: that ld makes correct v4T Thumb→ARM veneers for `-R` absolute symbols needs a spike |
| Interworking | Modules Thumb (`-mthumb -mthumb-interwork`). Core already built `USE_THUMB` with 375 stubs today (`hardware.md:91`) | [doc] |
| Thumb density | **−26.5%** `.o` text (92,688 → 68,104, 8b-T) | [doc] |
| Build | `tools/root.make` rule for `apps/ipodui/modules/<m>/` (compile Thumb → nm -u → keep list → core link → module link at slot origin → objcopy → zip into the hidden folder); a `module.lds` derived from `plugin.lds` (no IRAM, `ASSERT`s); `buildzip` entry. ~150 lines of make/lds | [est] |
| Sim | Plugins load as shared libraries (`firmware/SOURCES:94`, `load_code-sdl.c`). For modules, link them **statically** into `rockboxui` behind the same registry, so `tests/ui/*.script` run unchanged; host test `tests/modules/` for header/build-ID refusal; device procedure for real loads | [est] |
| Per module, resident | menu row + launch stub ~16-32 B; strings stay in the core LANG table (rule 8) | [est] |
| Safety | At unload, refuse (and log) if any tick task, event callback or timer points into the slot range; a module may not start threads; refcount the screens it pushed | [design] |

**Framework total resident ≈ 1-2.5 KB.** Its break-even is the first cold module.

### Candidates that exist now [measured image bytes]

- **Developer stock UI:** `_end` delta **41,552** (debug_menu 26,812, gui list/line/option_select/splash/scrollbar/icon/viewport ~9.6 K, devmode 4,446, udmatest 1,028, stock menu 2,303).
  - Realistic module: ~35-38 KB out of dev builds; about 4-6 KB of `#ifdef` hooks stay.
  - **0 in release builds.** It is the ideal first tenant: owner-only, no illusion risk, many core calls (a stress test for option J).
- **Cold Settings sub-screens** (already Thumb): about 1,946, legal 664, datetime 3,280, language 790, reset 550, newsw 324, holdmenu 1,861, sysscreen 3,216, checklist 1,368 ≈ **14 KB**.
- **Search:** search 12,486 + search_core 1,854 + `srch_glyph` 3,570 + `srl_alpha` 2,954 ≈ **21 KB**.
  - Warm: Music › Search. It reads the RAM cache, not the disk, so a module only pays off with prefetch at Music-menu open.
- **Not candidates:**
  - usbscreen 6,161 (USB rule);
  - otg 7,200 (Now Playing hold path);
  - batcal/powerlog (background);
  - library/np_draw/nowplaying/panels (hot);
  - Cover Flow (hot, ARM).

### Realistic savings

| Case | Saving |
|---|---|
| Today, option (b) | dev UI 35 + settings 14 + search 21 ≈ **70 KB** out of dev builds (≈ 35 KB in release) |
| Today, option (a) with a 48 KB slot | ≈ +22 KB net in dev builds, **−13 KB in release** (the slot costs more than release moves). Option (a) only pays once P12 lands |
| Future | P12 Extras 60-100 KB → resident engines only (alarm check on the RTC alarm, sleep-timer state, stopwatch start tick, lock state: ~4-6 KB) = **55-95 KB saved**. Games (#99): 100% in modules. P14 Videos (player 23.7 K + shell 19.6 K) fits one 48 KB slot as a session module |

---

## 3. Alternatives

### 3.1 Spending audio buffer (now legal up to the floors)

- Refill frequency scales as 1/buffer. The bytes read per hour are fixed by the bitrate; only the wake count changes.
- Per +64 KB of resident code [est]:

  | Unit | Bitrate | Buffer | Fill time | Change |
  |---|---|---|---|---|
  | 64 MB | 256 kbps (32 KB/s) | 40 MB | 1,311 s | −2.0 s per fill, **+0.16% wakes** (2.75 → 2.75/h) |
  | 32 MB | 256 kbps | 8.3 MB | 272 s | −2.0 s, **+0.77% wakes** (13.2 → 13.3/h) |
  | 32 MB | FLAC ~900 kbps | 8.3 MB | 77 s | 47 → 47.4 wakes/h |

- Cost per wake [est]:
  - flash: the ~1 s idle hold (`storage-choices.md:50`), so negligible: under 0.05% of runtime on the owner's unit;
  - HDD: spin-up 1-3 s plus a 5 s hold, roughly 3-5 J. On a stock 32 MB HDD unit (stock cell ~8 kJ, ~400 J/h playing), +0.4 wakes/h × 5 J ≈ 2 J/h ≈ **0.5% runtime (~5 min of 20 h) per +64 KB with FLAC**.
- The binding constraint is the **floor margin (~0.8 MB on 32 MB)**, not battery.

### 3.2 Other levers

- **Thumb for everything cold, by default.**
  - Already 42 objects (`tools/thumb-cold.txt`).
  - Next: search, dbjournal (7,380), ab_store (5,689), powerlog (4,324), batcal_core (3,534), otg (7,200; only if timing allows) ≈ 40 KB ARM → **~10 KB saved**.
  - All P12/P16/P13 cold code Thumb from day one: −25%.
  - Free, low risk. Proof: the 8b-T timing method.
- **Rodata.** 110,526 B of sized symbols; **49,868 B** are shell bitmaps (panelart_*, glyph_*, srch/srl, npph_*, caldig, rdigit) and 8,043 nfc tables [measured].
  - LZ/RLE with a ~400 B decoder into the arena at draw time could plausibly save 15-25 KB [est], with some CPU per draw.
  - Moving them to disk only helps if they load into a **transient** place. Loading at boot into a pinned `core_alloc` just moves the bytes out of `_end` and still costs the floor.
  - With the BCM as UI compositor (§6) these assets become BCM-side copies; the PP copy must still exist (source of truth, §6.3).
- **Codec slot right-sizing:** potentially the largest single lever (100-300 KB) [hypothesis until measured, §5].
- **IRAM is not a capacity lever:**
  - core 48 KB: 43,024 used (ibss 25,256 + iram 9,320 + idle stacks 256 + stack 8,192), so **6,128 B free**;
  - codec region 80 KB: flac uses 81,648 of 81,920 (ape 81,104, opus 80,272), so **~272 B free** at worst [measured];
  - use the 6 KB for one hot loop (alpha blend or the port copy loop), for speed only.

---

## 4. Recommendation: strategy P16 → P14 (re-targeted to the floors)

**Principles:**
- stay ≥ 256 KB above each floor as a working margin;
- each phase brief states its floor cost in bytes;
- no reclaim from the audio buffer while paused;
- one build for flash and HDD, with only latency differing;
- no visible loading (no splash; the push animation starts when the module is ready);
- prefer load-on-demand and BCM memory where measured to be better.

1. **Gate first (P16, tooling + orchestrator).**
   - Replace "`_end` ≤ cap" with a floor check on both RAM sizes:
     - `_end + PLUGIN_BUFFER_SIZE + CODEC_SIZE + runtime reservations` from a shell-start trace (sim builds for 32 and 64 MB);
     - keep `_end ≤ 1,500,000` as a backstop.
   - Report the margin in every gate.
2. **P16 (8-12 KB):**
   - Thumb by default for cold code; extend `thumb-cold.txt` (~10 KB back);
   - measure each codec's peak (§5);
   - no framework yet.
3. **Before P12: the shell-module spike** (`firmware-platform`), with the developer UI as tenant.
   - Decide J vs T on the spike's result.
   - Slot: option (b) if §5 shows ≥ 128 KB codec headroom, otherwise (a) sized to the largest planned module (start at 48 KB).
   - Then move the cold Settings sub-screens.
4. **P12 Extras:**
   - every app = a resident engine (≤ 1 KB each) + a Thumb module;
   - Games (#99) = shell modules, never Rockbox plugins (§1.5: ≥ 0.7 MB);
   - the custom EQ editor is a module; EQ application stays resident;
   - city/time-zone tables, contact, calendar and notes data live on disk, parsed inside modules.
5. **P13 mixes:** generate at maintenance time (P16 job, resident Thumb); browsing is ordinary resident lists.
6. **P7 Cover Flow:** code resident ARM (~30 KB).
   - Textures (~512 KB) are the biggest floor item. **Put them in BCM memory if our BCM image is the display driver by then** (§6). Otherwise, a `core_alloc` at shell start is allowed only if the floor margin still holds on 32 MB units after it.
   - Lacking that margin, consider fewer or smaller textures on 32 MB units: same look, smaller cache. Visible behaviour must not change; owner call.
7. **P14 Videos:**
   - the player and Videos screens as one session module;
   - the stream buffer stays the PP audio buffer (§6.2);
   - the BCM image file is read at session start.
8. **P19 hardware offload** (and Fit-3e #76/#75 earlier if the BCM session delivers): UI assets, second frame and strip framebuffer on the BCM (§6).

**Exception for the owner:** none needed under the new floor rule. If the module spike fails, the fallback is honest resident growth with Thumb (P12 ~45-75 KB resident). That is ~6-9% of the 32 MB margin, acceptable while Cover Flow textures go to the BCM.

---

## 5. Device and sim checks (procedures; results come from the owner)

- **Codec peak (decides slot option b and a possible `CODEC_SIZE` cut):**
  - developer build with a high-water log: `mem_ptr` max in `codec_malloc`; for Vorbis, TLSF's used high-water; plus `end_addr − codecbuf`;
  - play the worst file of each of the 39 decoders (APE extra high/insane, WMA Pro, Opus, Vorbis q10, HE-AAC, a large MOD/SID/SPC);
  - read `ipodui.log`;
  - also run in warble for logic, though 64-bit pointers there inflate sizes.
- **Module load timing:** flash and `--storage=hdd` (cold disk): `module load name= bytes= ms=` logged; 50 enter/leave cycles while playing, checking for no gap (devstats PCM underrun count = 0).
- **Floor trace:** buffer bytes after shell start, with a 50k library, on 32 and 64 MB sims; on the device, the devmode buffer screen (`hardware.md:108`).

---

## 6. BCM2722 memory and roles (owner, 2026-10-20)

**Inventory** (`docs/bcm2722-reference/memory-access.md` §2-3, the "Our image's SRAM/SDRAM map" tables):

- **SRAM, 1.25 MB:**

  | Region | Size |
  |---|---|
  | Image | 127.5 KB (bss limit 0x20000) |
  | Decoder arena | 688 KB (video only) |
  | Stack | 16 KB |
  | Rings | 12 KB + 4 KB reserved |
  | Skeleton FIFO | 32 KB (non-video) |
  | Scaler strip + self-test | 12 KB |
  | Host frame | 150 KB |
  | `VIDEO_FB` | 150 KB |
  | Tone | 35 KB |
  | **Free** | **46 KB** |

- **SDRAM, 4 MB:**

  | Region | Size |
  |---|---|
  | Bitstream FIFO | 1 MB |
  | DPB (only > 320×240 or diagnostic) | 2 MB − 16 KB |
  | Wrap | 256 KB |
  | Audio window | 256 KB |
  | AAC far | 112 KB |
  | AAC I/O | 16 KB |
  | Dumps (diagnostic) | 302 KB |
  | **Free** | **40 KB** |

- **Outside a video session, the 688 KB arena and almost all of SDRAM are idle**, provided our image is the resident display driver (`host-driver.md:54-60`).
- **Wiped on sleep:** Rockbox cuts BCM power with the backlight (`host-driver.md:49`). Every wake re-uploads the image: 370-616 ms to "image up" [silicon S30].
- **Upload cost:** 153,600 B by CPU copy in 10.4 ms boosted (~14.8 MB/s) or 27.6 ms unboosted (~5.6 MB/s) [silicon S8, CPU methods]. PP DMA into the port has landed **nothing** so far (S34/S35, `data-paths.md` D1). The second DMA engine is Fit-3d, waiting on the RE trace.
- **SDRAM:** latency and bandwidth from the core are **not measured** (open question #7). Block-write anomalies were seen with an address per block (S32/S33). The player's CPU copy writes correctly while the core decodes [silicon].

### 6.1 Video codecs and working memory

- H.264 is in place:
  - code in the image region;
  - reference frames in the SRAM arena at 320×240;
  - AAC tables parked in SDRAM (dev42).
- MPEG-4 Part 2 (SP/ASP, 320×240): 3 YUV420 frames × 115,200 B ≈ 346 KB + MB info, so it fits the same 688 KB arena [est].
- Its code will not fit beside H.264 in 127.5 KB. Options:
  - one image per format, chosen at session start. The PP reads it from disk at start, when the disk is awake for the video anyway; holding it in PP RAM is needed only during the session, for backlight wakes;
  - or cold tables and code in SDRAM, like `BCMFW_AAC_FAR` (SDRAM execution speed unmeasured).
- **PP cost:** `read_image()` pins a 206,532 B core_alloc (`apps/video/vplayer.c:511-551`, `BCMVIDEO_IMAGE_BYTES` 206,532). Taken at session start, after music stops, so the floor is unaffected.
- **If our image becomes the always-on display driver, the PP must keep it resident to re-upload at every backlight wake.** A flash re-read would be a storage wake per screen wake: a spin-up on HDD, which breaks one-build parity.
- **So the display-driver image must be small:** a UI image ≤ ~64-100 KB held in PP RAM, with codec images loaded only per session. That resident copy is a floor line item.

### 6.2 Compressed stream buffer in BCM SDRAM

- Today the file streams disk → **PP audio buffer** (buffering handles; `vpcore.c:569-585` "not buffered yet") → CPU copy → BCM SDRAM FIFO (1 MB, ≈ 10 s at 800 kbps).
- **Moving the stream buffer to the BCM frees no PP RAM in practice:**
  - music is stopped during video, so the idle audio buffer is the right home: 39-40 MB ≫ 4 MB means fewer storage wakes;
  - every byte crosses the port once either way.
- What can grow: the FIFO into the unused DPB area at 320×240 (to ~3 MB ≈ 30 s). That gives fewer, larger port bursts: less boost and polling, a CPU and battery win. Measure with Fit-1 per-thread CPU.
- **Soundtrack:** AAC AUs go to the BCM audio window (stage 9) or the PP codec. They interleave in the same PP-buffered file, so nothing changes.

### 6.3 UI assets on the BCM

- **Candidates:**

  | Asset | Size |
  |---|---|
  | Glyph atlases | fonts ~102 KB (`hardware.md:99`) |
  | Panel art / glyph bitmaps | ~50 KB [measured rodata] |
  | Material tables and tiles | a few KB |
  | Cover Flow textures | ~512 KB |
  | Cached covers | drift slot 180 KB; art decode block 164 KB is PP work memory, not a candidate |

- **Fit:** the idle arena (688 KB) and free SRAM hold atlases + panel art + second frame (≈ 300 KB). Cover Flow textures go in SDRAM's idle DPB region (2 MB).
  - During video, SRAM UI assets are evicted (the decoder owns the arena) and re-uploaded after.
  - Assets in the SDRAM DPB region survive a 320×240 session.
- **Source of truth and the floor:**
  - Assets the screen needs immediately after every wake (atlases, panel art, materials) **must stay in PP memory**. One build means no spin-up per wake.
  - So for them the BCM copy saves **CPU and port traffic, not PP RAM**: Fit-3 measured 272 UI updates = 19.7 MB in 2.1 s per busy minute; full frames take 27-34 ms (`fit3-brief.md:78`).
  - Re-upload at wake ≈ 152 KB in ~10 ms boosted, on top of the ~370 ms image boot: invisible.
- **Real PP RAM savings come from derived, rebuildable data:**
  - **Cover Flow textures** (~512 KB): rebuilt lazily from the on-disk art cache on entering Cover Flow. **Saves ~512 KB of floor**, the single biggest item.
  - Cached covers beyond the current one (part of the drift slot), re-read after a wake. The latency differs by storage, the visible result does not (#73's rule: the panel never goes empty).
  - **The PP framebuffer.** With #75 (the BCM composes; the PP sends only new rows), the PP's 153,600 B `lcd_static_framebuffer` could shrink to a strip (e.g., 32 rows = 20 KB), with a full shell repaint after each BCM wake. ≈ 130 KB saved [hypothesis: needs #75, and the blending paths reading back from a strip].
- **CPU:** a compose or blit on the BCM vector unit (~31 B/cycle in SRAM, [silicon] S7) instead of the PP at 30-80 MHz. The PP renders only text and changed rows.

### 6.4 Second frame (#76)

- 150 KB in BCM SRAM (it fits in the idle arena outside video, or the 46 KB free + skeleton FIFO areas will not; use the arena).
- **0 PP RAM.** A PP-side double buffer would cost 150 KB of floor, so #76 must be BCM-side.

### Dependencies

- Our BCM image as the default display driver: today `--bcmfw`/`--bcmfw-default` are not in the owner's fw-ui (EXTRA_DEFINES show no `HAVE_BCMFW`).
- #76 and #75 (Fit-3e, own brief, "after dev46 merges"); #78 (port-read costs).
- Fit-3d / the second DMA engine: uploads are CPU copies until then.
- The BCM session's #60 / `p14-bcm-re`; SDRAM speed (open question #7); MPEG-4 Part 2 decoder work.

### Phase mapping

| Phase | BCM work |
|---|---|
| P16-P12 | none (PP-side levers only) |
| P7 | Cover Flow textures on the BCM **if** the display driver is default by then, else PP textures only within the margin |
| P14 | per-session codec images, FIFO growth |
| Fit-3e (as early as the BCM session allows) | #76 then #75 |
| **P19 hardware offload** | glyph/panel atlases, strip framebuffer, cached covers on the BCM |
