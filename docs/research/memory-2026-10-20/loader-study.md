> Research record, 2026-10-20 (read-only audit; inputs to docs/memory-audit-2026-10-20.md, docs/design/module-loader.md and docs/hardware-fit-audit.md). Paths to `scratchpad/` refer to the session's scratch area and are not kept. Where it conflicts with the synthesis docs (notably the floor framing: the owner chose option 1, the audio file buffer itself), the synthesis wins.

# Loader study: adopt Rockbox's code loader, or build our own? (read-only, 2026-10-20)

Owner's question (2026-10-20): before we adopt the Rockbox codec/plugin loader for "shell modules", is it right for this hardware, or should we write one built for it?
Folded in from the owner the same day:
0. **Scope rule:** the main menu and every split-screen list are never snapshotted. Lists and menus are always drawn live (they are resident and fast). Right-side panel images may be pre-rendered, as `panels.pak` and the art cache do today. Snapshots apply only to heavy on-demand full screens.
1. The Rockbox UI is going away completely, so the developer UI will be **our own, written as modules**.
2. A **pre-render / snapshot** pattern ("first frame instant, the rest arrives behind motion").
3. Snapshots are rebuilt only at **three moments**: the boot hold, the post-USB busy moment, and a visible **"Updating Language…"**.

Method:
- Sources read in `.claude/worktrees/headers` (HEAD `eb2f96dea5`).
- Measurements from that tree's existing `build/fw-ui` objects and `rockbox.elf` (text/data/bss 808,060 / 8,552 / 338,200).
- A **link spike** in `scratchpad/ld/`: real shell objects linked as a Thumb module against the real `rockbox.elf` with `--just-symbols`, using the project's GNU ld 2.47.
- Nothing in the repo was edited, built or written.

Tags:

| Tag | Meaning |
|---|---|
| **[measured]** | From ELF, object or spike output |
| **[code]** | Read from source, with a citation |
| **[doc]** | From a repo doc, with that doc's own tag |
| **[est]** | My estimate; needs measuring on the device |
| **[hypothesis]** | Needs a spike or a device test |

---

## 0. Verdict in one paragraph

**Don't adopt Rockbox's plugin loader. Write a small loader built for the shell (~2 KB Thumb resident). Keep only the parts of Rockbox's path that are already right for this hardware.** Those parts:
- the FAT layer's **zero-copy, coalesced DMA read** (whole sectors go straight from IDE DMA into the destination; there is no FS copy to remove);
- the whole-cache maintenance call.

What Rockbox gets wrong for us is everything above `lc_open`:
- one fixed slot at the top of RAM;
- a 419-pointer API struct with version coupling;
- `plugin_load()`'s UI side effects (a "Wait" splash when the disk is idle, clearing the LCD, activity and skin handling);
- a synchronous read on the UI thread;
- a COP round trip on every load.

The design we should build:
- modules are **Thumb, linked at a far virtual base against the core's symbols** (`--just-symbols`). The spike shows ld generates correct ARMv4T veneers, and every remaining position dependency is a plain ABS32 word: **161 relocations for a 14.6 KB module**;
- they load into **one static, time-shared transient arena** that also holds other rebuildable working sets, with LRU eviction, moving modules, and no allocation while playing;
- they are read from a **single pack file** held open between USB sessions: one sector-aligned DMA command per load;
- they are hidden behind **first-frame snapshots** that are rebuilt in one batch at boot, after USB, and at "Updating Language…".

On flash, a module load (~2-6 ms) is shorter than one frame of the 190 ms push. The snapshot pattern earns its keep mainly by letting **big rebuildable working sets share one arena**, Cover Flow first among them. Module code is not where it pays most.

---

## 1. Rockbox's loader as it is

### 1.1 What a load actually does (per load)

`lc_open()` (`firmware/lc-rock.c:29-95`) [code]:

1. `open(path)`: a path walk through the hidden folder. dircache is compiled out (`hardware.md:242`), so each component is a directory scan through the 32 KB disk cache (`cache_buffer`, 0x8000 B [measured]). It costs 0 commands if the sectors are cached, else 1-2 sector reads.
2. **COP cache flush via `switch_core()` there and back** (`lc-rock.c:42-48`):
   - this migrates the calling thread onto the COP's run queue and back (`firmware/kernel/thread.c:1495-1518`);
   - the COP is woken only by the CPU's tick (`docs/data-paths.md` M2), so each hop can wait up to a tick: **up to ~20 ms per load** [hypothesis; measure];
   - it is needed only if the COP could hold lines of the destination. Shell modules never run on, or share memory with, the COP, so for us it is pure cost.
3. Reads the header (one sector into the file's sector cache), computes `copy_size`, checks bounds, then `lseek(0)` and reads the whole file to `hdr.load_addr`.
   - In `readwrite()` (`firmware/common/file.c:622-760`) [code] the cached first sector is `memcpy`ed (2 KB). The rest goes through `fat_readwrite()` (`fat.c:2553+`), which **coalesces contiguous sectors into one storage command** (capped at `FAT_MAX_TRANSFER_BYTES` 256 KB, `ipodvideo-variant.h:147`) and calls `storage_read_sectors()` on the destination directly.
   - `ata_dma_setup()` (`ata-pp5020.c:267-330`) DMAs straight into it when the address is 16-byte aligned, after a whole-cache `commit_discard_dcache()`.
   - So **the body is already zero-copy UDMA2 at ~21 MB/s** (`data-paths.md` §15, [silicon S36c]).
4. `commit_discard_idcache()`: a whole-cache flush plus a 512-line status rewrite with IRQs off (`system-pp502x.c:238-294`). PP5022 cache maintenance is **whole-cache only** (`data-paths.md:29`), so "clean by range" doesn't exist on this chip. The whole-cache operation is cheap (8 KB; tens of µs [est]).

Fixed cost per load:
- 0-2 directory sector reads;
- 1 header command;
- 1 body command (more if the file is fragmented);
- 2 COP hops.

On flash that is ~1-3 ms of commands + **0-20 ms of COP hops** [est]. On an awake HDD each command costs a seek plus rotation, ~15-25 ms [est], so about **40-75 ms + transfer**.

### 1.2 Memory layout constraints

- **No relocation.** A plugin is linked at `pluginbuf` and refused anywhere else (`apps/plugin.c:967-976`). A codec is linked at `codecbuf` (`apps/codecs.c:209-215`). That means **one image per slot, one slot per kind**.
- Both slots are static carve-outs **at the top of RAM** (`firmware/target/arm/pp/app-pp.lds:14-23, 182-194`). Every byte costs audio buffer, yet none of it shows in `_end`.
- The plugin slot is 0 B today (`ipodvideo-variant.h:151-156`). Giving it any size costs the floor byte for byte.
- "Keep the last module resident" exists only as Rockbox's TSR mechanism, and even then only **one** module (`plugin.c:925-947`). "Preload during the push" isn't possible: `lc_open` is synchronous and `plugin_load()` immediately *runs* the image (`plugin.c:1019`).
- Sharing the codec slot (audit C option b) is possible but needs each codec's peak measured, and the slot is the COP's working memory (Fit-2 `COP_RESERVE`, `codecs.c:55-57`): any module memory there is coherency-sensitive.

### 1.3 What it drags in, and what removing the stock UI frees

**Already gone in fw-ui:** `plugin.c`, the 1,676 B `rockbox_api` table (419 pointers), `PLUGIN_API_VERSION` coupling, and the ≥ 202 KB stock closure the API pulls back (`ipodvideo-variant.h:113-118`; audit C §1.5). `nm` shows no `rockbox_api` in fw-ui [measured]. Only `lc_open` (244 B) and `ci` (164 B, the codec API) remain [measured].

**Re-adopting the plugin path would bring all of that back.** The owner's ruling (the Rockbox UI is deprecated) makes that a non-starter: `plugin_load()` itself calls `splash()`, `lcd_clear_display()`, `skin_update()`, `viewportmanager_theme_enable()` and `tree_lock_cache()` (`plugin.c:944-1046`), all stock-UI surface.

**What removing the stock UI frees** [measured, this tree]:

| Item | Bytes |
|---|---|
| fw-ui vs fw-rel (developer mode) | **+39,368 text** (808,060 vs 768,692); +2,192 bss |
| Stock UI objects in fw-ui: `debug_menu` 7,544, `screens` 3,007, `menu` 2,379, `gui/list` 2,272, `gui/line` 2,080, `option_select` 1,827, `bitmap/list` 1,586, `splash` 1,511, `scrollbar` 1,392, `statusbar` 1,382, `icon` 1,061, `yesno` 1,058, `viewport` 721 | **27,820 text + 2,035 bss** (already Thumb, `tools/thumb-cold.txt` T2) |
| `main()` / list init hooks, splash funnel | small; audit C put the hooks at 4-6 KB |

So the ProkBox build with the stock UI deleted gets back ~28-30 KB of code + 2 KB of statics in developer builds, and nothing in release (it is already out). Our own developer UI then costs **0 resident bytes** beyond its module stubs.

**Effect on the API surface:** the loader's API becomes **the shell's own symbols**, not Rockbox's.
- The spike's cold-Settings group calls **121 distinct core symbols**; Search calls **64** [measured, `nm -u` union minus internal definitions].
- They are `ui_*` (list, push/pop, paint, panel, arena), `lcd_*`, `font_*`, settings, tagcache UI stats, libc, and `__aeabi_*` helpers.
- A full module set will probably reference ~250-350 distinct symbols [est].
- There is no struct to maintain and no version number: core and modules come from **one build**, checked by a build ID.

### 1.4 Fit summary

| Requirement | Rockbox loader |
|---|---|
| Zero-copy read | yes (the FAT/ATA layer, not the loader) |
| Several resident modules, LRU | no (one fixed slot) |
| Preload without running | no |
| Asynchronous / off the UI thread | no |
| No visible splash | no (`plugin_load` splashes `LANG_WAIT` when the disk is idle) |
| Floor-neutral memory | no (top-of-RAM slot) |
| API without a 419-entry table and version bumps | no |
| Thumb modules calling the ARM core correctly | yes for plugins (via the table) |
| Sim support | yes (`.so`), but the shell wants static linking anyway |

---

## 2. A loader built for this hardware

### 2.1 Linking: Thumb modules, far-base link, `--just-symbols`, build-ID [measured spike]

Spike setup:
- core: `build/fw-ui/rockbox.elf`;
- module objects: the eight already-Thumb cold Settings screens: `about`, `legal`, `reset`, `datetime`, `holdmenu`, `sysscreen`, `language`, `newsw` (12,670 text + 160 data + 106 bss as objects);
- linked with `arm-none-eabi-ld -T mod.lds -R rockbox.elf` at different bases:

| Link base | Image | Veneers | Words that change when the base moves 1 MB |
|---|---|---|---|
| 0x00200000 (near the core) | 13,592 B | mix of short `__x_from_thumb` (8 B) and long (16 B) | 306, of which **145 are PC-relative BLs into the core** (not relocatable) |
| **0x03000000 (far, > 4 MB from all core code)** | **14,656 B** | **113 long veneers** | **161, every one a +0x100000 delta, 0 others** |

What this shows:
- **ld generates correct ARMv4T interworking veneers for `-R` symbols.** It emits `bx pc` / `ldr ip,[pc]` / `bx ip` / `.word target|1`, for example `__ui_push_veneer` → `0x0000c091`. No BLX is used. This settles audit C's open [hypothesis].
- **Link at a far virtual base, run anywhere.** Then every external call goes through an absolute veneer and stays valid wherever the module sits. The only fixups are ABS32 words that point inside the module (literal pools, function-pointer tables, screen descriptors): **161 words, 1.1 % of the image**.
- Relocation table: a halfword word-index per fixup (modules ≤ 128 KB), **~2.2 % size overhead**. Applying it costs ~161 load-add-stores ≈ 10 µs [est].
- **Veneer overhead at the far base: 1,826 B (12.5 %)** against ~922 B near.
  - That is the price of position independence, and it is smaller than the 26 % Thumb saves.
  - Optional trim: `-mlong-calls` for modules (each call site `ldr rX,=sym; bl __call_via_rX`) removes per-callee veneers but adds ~6 B per call site. Measure both; it is not worth design time now.
- **No API table.** Resident cost is 0 B beyond the symbols themselves, which the core must keep under `--gc-sections` (`tools/root.make:190`). That needs a generated `--undefined=` list from the modules' `nm -u`; almost all of them (`ui_*`, `lcd_*`) are live in the core anyway.
- **Coupling** (the real cost of `--just-symbols`):
  - every core link changes addresses, so modules are **relinked on every build** (cheap: one `ld` each) and shipped in the same zip;
  - the header carries a 32-bit **build ID** (a hash of the core ELF's symbol table), and a mismatched module is refused and logged, never shown;
  - this replaces Rockbox's API_VERSION discipline;
  - there are no mixed-build modules, ever. That's acceptable for a firmware that is always installed as one zip.

Module rules (linker `ASSERT`s in `module.lds`):
- `SIZEOF(.iram) == 0` (codec IRAM belongs to the codec);
- **no writable `.data`** (`datetime.o`'s 160 B becomes const or bss);
- `.bss` zeroed on every entry;
- no threads, no tick tasks or timers that outlive the module.

The reason for no writable `.data`: a runtime-stored core pointer in a relocated `.data` word would be corrupted by a later move. Persistent state lives in the core, in settings, or in the screen arena.

### 2.2 Memory: one static transient arena, time-shared

Constraint:
- a `core_alloc` while playing shrinks the audio buffer, which means a gap or queue restart (`hardware.md:73, 102-103`, audit C §1.6(c));
- the paused state is normal.

So module memory must be **reserved at shell start and counted in the floor trace**, never taken on demand.

The **transient arena** is one static block managed by the loader. Its tenants are things that can be **thrown away and rebuilt without a visible change**:
- module code;
- Cover Flow textures (P7, if they stay PP-side);
- possibly the 180,000 B UI strip (`apps/ipodui/anim.h:84-101`, which is already a one-block, many-owners precedent: slides, list rows, the panel);
- possibly the second drift slot (+180,000 B, `storage-choices.md:54`);
- the Videos session module (P14).

Mechanics:
- **Allocator:** first-fit, an LRU list, and a refcount per tenant.
  - A module is held while any of its screens is on the stack or any of its callbacks is registered (the registry checks that callback addresses fall in a module range).
  - An idle module can be **evicted** (just forgotten) or **moved** (`memmove` + re-apply relocations + one whole-cache op) to defragment.
  - Both happen only at refcount 0, on the UI thread, between screens.
- **Evicting a module never needs a disk write.** Snapshots (§2.5) make a later re-entry look instant.

Arena size:
- **Before P7:** 48-64 KB holds the Settings pack (14.6 KB), Search (~30 KB with veneers), and one developer module. The cost is +48-64 KB of floor in every build.
  - Alternatively, carve it from the codec slot (0 B of floor) *after* the codec peak is measured (audit C §5).
  - Or make it the strip's tail: the strip is idle except during a 190 ms slide and list scrolls, but its owners and the module's lifetime overlap, so this is **not** recommended.
- **From P7:** the arena becomes Cover Flow's texture home, ~512 KB (audit C §0).
  - Modules, the second drift slot and module snapshots' working memory then **share** it.
  - Entering Cover Flow evicts the modules (their snapshots cover a return) and, if the owner accepts, the second drift slot (one cover re-read on the next set switch).
  - **Floor saved by sharing instead of summing:** 512 + 64 + 180 = 756 KB summed against 512 KB shared ≈ **244 KB**, more if the strip can join.
  - If the strip joins (Cover Flow never slides while its textures load, which needs checking in `ui_main.c:432-495`), up to ≈ **424 KB**.
  - If Cover Flow textures go to BCM SDRAM instead (audit C §6.3), the arena drops back to ~64-180 KB.

### 2.3 The read path: a pack file, held open, sector-aligned

There is no copy to remove (§1.1). What's left to cut is the **lookup** and the **header round trip**.

- `modules.pak` in the hidden folder (attributes hidden, like the other system files):
  - a 2 KB index: per module, `id`, `offset`, `bytes`, `reloc_off`, `bss`, `entry`, `fletcher32`;
  - then each module image starting on a 2,048 B sector boundary;
  - shipped in the zip and read-only on the device.
- At shell start, and again after each USB disconnect: open it once, read the index (one command), and **keep the fd open** until the next USB connect.
  - No disk access happens at USB entry (owner's rule): the fd is closed on the UI thread's ack path, which does no writes.
- **A load is `lseek` + one `read` into a 16-byte-aligned arena address.** That is one coalesced UDMA2 command, so no directory walk, no header pre-read and no COP hop.
- Raw extent mapping (calling `storage_read_sectors()` with a cached first cluster) would save only the FS bookkeeping (µs). It isn't worth bypassing FS locking.
  - The file's contiguity is up to the host. `fat_readwrite` splits fragmented runs on its own, and a 16 KiB-cluster volume needs at most a few FAT lookups, already in the disk cache.
- **Integrity:**
  - build ID + size + magic on every load;
  - a Fletcher-32 over the body once per **mount generation** (bumped at USB disconnect and at boot), because only USB can change the file. That is ~1-2 ms per 48 KB at 80 MHz [est];
  - a CRC32 by table would cost ~5-6 ms per 48 KB (≈ 10 cycles/B [est]), longer than the read itself.
- **Asynchronous:** the read runs on an existing background worker (the maintenance or art-builder thread, or #81's prefetch worker pattern; **no new thread**). The UI thread keeps animating and polls a ready flag.
  - On flash it barely matters (2-6 ms). On an awake HDD (20-75 ms) a blocking read on the UI thread would drop frames of the 190 ms push (`screen_stack.h:88`).

### 2.4 Latency per load [est unless noted]

| Storage | Fixed | Per KB | 16 KB module | 48 KB module |
|---|---|---|---|---|
| Flash (iFlash, UDMA2 ~21 MB/s [silicon S36c]) | ~0.5-1 ms (1 command; index already in RAM) | 48 µs | **~1.5-2 ms** | **~3-3.5 ms** |
| Flash, Rockbox `lc_open` | ~1-3 ms commands + 0-20 ms COP hops [hypothesis] | 48 µs | 2-24 ms | 4-26 ms |
| HDD awake (4200 rpm 1.8"; ~12 ms seek + 7 ms rotation; ~10-15 MB/s) | ~20 ms (1 command) | 70-100 µs | **~22-25 ms** | **~25-35 ms** |
| HDD awake, `lc_open` | ~40-75 ms (2-4 commands) | 70-100 µs | 45-80 ms | 50-90 ms |
| HDD asleep | +1-3 s spin-up (`storage-choices.md:81`) | | | |

Plus relocation (~10 µs), one whole-cache op (~tens of µs), and Fletcher on the first load per mount generation (~30 µs/KB).

**Compression: no.** On flash, LZ4-style compression at ratio r ≈ 0.6 (Thumb code) only pays if the decoder runs faster than 21 MB/s × 1/(1−r) ≈ **52 MB/s**. An ARM7TDMI at 80 MHz with an 8 KB cache and SDRAM wait states decodes LZ at ~5-15 MB/s [est]: always slower than just reading. On an awake HDD the seek dominates, and a smaller transfer saves ~1 ms out of ~25. Disk space is irrelevant (1 TB). The same holds for snapshots, with one exception noted in §2.5.

### 2.5 Snapshots: first frame instant, the rest behind motion (owner pattern)

Every observable here is user-identical on flash and HDD. Only the wait behind the motion differs.

**Scope (owner rule, 2026-10-20):**
- **No snapshots** for the main menu, any split-screen list, or any list or menu screen, including the cold Settings pack: these are always drawn live. Inside a module, a list screen still loads in ~2 ms on flash, within the 190 ms push.
- Right-side panel images may be pre-rendered (today's `panels.pak` and art cache), and the batch below may refresh them.
- **Snapshots apply only to heavy on-demand full screens:** Cover Flow's opening view, P12 Extras apps (Clock, Stopwatch, Calendar, Notes and similar), Games (#99), the first screen of our developer UI, and a Videos landing screen if it becomes a full screen.

**The rule:** the first frame of any screen appears at once; everything else may arrive within ~0.5 s, hidden behind motion (the 190 ms push, the eased first spin). There is **never** a spinner in normal use; the one sanctioned indicator is "Updating Language…" (below).

**What a snapshot is:**
- the module screen's **body** (below the header: 320 × 218 × 2 = **139,520 B**) as raw RGB565, in `snap.pak`;
- a preallocated file with fixed slots, overwritten in place (no allocation or FAT growth at write time), plus a 2 KB key table;
- the header (clock, battery, title) and any **live rectangles** a module declares (for example a checkmark that follows a setting, or Now Playing state) are drawn live over it. They are cheap and resident.

**How entry works**, with no extra RAM:
- the existing push already copies the old body into the strip and then draws the new screen **into the framebuffer** (`apps/ipodui/ui_main.c:451-495`, `slide_capture()` → `draw_dirty()`);
- with a snapshot, `draw_dirty()` for a not-yet-loaded module is replaced by **reading the snapshot by DMA straight into the framebuffer rect** (`lcd_static_framebuffer`, 0x25800 B, aligned). One command: ~7 ms on flash, ~30 ms on an awake HDD [est];
- the slide runs from that frame. Meanwhile the module loads; its live rectangles are painted when it is ready, invisibly inside the slide's 190 ms on flash or awake HDD;
- **a snapshot costs 0 B of PP RAM.** It lives on disk and lands in the framebuffer that exists anyway.

**Rebuild moments** (owner, 2026-10-20). **One batch routine, three triggers:**
1. **Boot hold**, behind the boot glyph. The budget is a few seconds for all boot work; the 7G itself takes 5-10 s.
2. **The post-USB busy moment** (#84), which already exists for the library.
3. **"Updating Language…"**, a visible, sanctioned indicator. Other rare *global* changes reuse it: theme or accent, if the 7G has them; font scale; device name, if it ever appears in a body.

**What the batch does:**
- compare each snapshot's **key** with the current state:
  - `build_id`;
  - `lang_id`;
  - `mount_gen` (bumped at USB disconnect and boot): covers library, art and file changes, since content only changes over USB;
  - `settings_gen` for that module's own settings group;
- for each stale entry: load the module into the arena, render its first frame off-screen into the framebuffer with the backlight-side screen frozen on the boot glyph or the busy screen, DMA the body rect to its `snap.pak` slot, and evict;
- also rebuild the other **pre-rendered sets** in the same pass: right-panel images (`panels.pak`), Search's letter glyph strip (`srch_glyph`/`srl_alpha` today are 6.5 KB of rodata [audit C, measured]), cached labels, and the Cover Flow opening view.

**Cost per snapshot** [est]:

| Step | Flash | HDD |
|---|---|---|
| Module load | 2-4 ms | 25 ms |
| First-frame render (UI thread, boosted) | 10-40 ms | 10-40 ms |
| Write 139.5 KB (SSD DMA writes, `ata-pp5020.c:274-276`) | ~10-15 ms | 20-40 ms (PIO) |
| **Total per snapshot** | **~25-60 ms** | **~60-110 ms** |

Under the scope rule the set is small: Cover Flow's view, ~4-6 Extras apps, a few games, and 1-2 developer screens, so **~8-10 snapshots**. A **full** rebuild is ≈ **0.25-0.6 s on flash and ≈ 0.5-1.1 s on HDD**, with no snapshots at all before P12. A typical boot rebuilds nothing (keys unchanged): one 2 KB key-table read.

**Invalidation outside the batch moments** (owner refinement): only *local* settings changes made on the device:
- clock format (the Clock app's snapshot);
- the settings an Extras app or game shows on its first screen.

Main-menu items, EQ and Shuffle/Repeat no longer matter: menus and lists are always live.

Each bumps **its own** `settings_gen` group and marks just those snapshots stale. A stale snapshot is simply **not used**: that entry renders live after the module loads, as it would without snapshots, and the next batch moment rebuilds it. Optionally, re-snapshot lazily on that screen's first live render, written only if the disk is awake.

This removes all runtime invalidation logic except one counter per settings group. A stale-but-used snapshot can't happen, because a used snapshot always has a matching key.

**Developer check:** in developer builds, hash the module's first live frame and compare it with the snapshot (`snap stale m=` to `ipodui.log`). In the sim, a `tests/ui` script that renders each module live and from its snapshot, expecting 0 differing pixels (`pngdiff`). Proposed to the tooling owner; I'm not writing goldens.

**Disk cost:** ~9 × 139.5 KB + Cover Flow 153.6 KB ≈ **1.4 MB**. Negligible.

**Intent-driven preload:**
- **when:** a highlight dwell of 0.4 s on a module's menu row starts the module load (and the snapshot is ready to DMA). A 1 s dwell for heavy tenants (Cover Flow textures);
- **gate:** only when the disk is already awake (`storage_disk_is_active()`), or on flash, where storage choices treat a wake as cheap (the decision compares the measured spin-up time with a budget, as `storage-choices.md:50` plans: same code, a different measured number);
- **opportunistic fill:** while the disk is awake anyway (the 5 s HDD hold after each audio refill), load the likely next module into a free arena slot;
- **RAM cost:** 0, because preloading uses only free arena space and never evicts a held tenant.

**Aggressive freeing after exit:** a module's refcount drops to 0 when its last screen pops. It stays resident as LRU (re-entry free) until the space is needed, then it is just forgotten; the snapshot makes the next entry look the same.

**Cover Flow** (P7):
- the opening-view snapshot (153.6 KB, full screen) goes by DMA straight into the framebuffer: instant;
- textures (~512 KB pre-scaled, from the art cache, pre-computed at sync time per `hardware.md`) load into the arena behind it: ~25 ms on flash, ~60-150 ms on an awake HDD [est];
- a spin within the first ~0.5 s gets a short eased start;
- the textures are discarded on exit, so **512 KB is free outside Cover Flow** for the arena's other tenants (§2.2);
- the 7G's own opening transition into Cover Flow should be checked in `docs/design/spec/` before choosing a cut against a slide; I didn't read that section.

**HDD asleep, honestly:** a snapshot on disk doesn't help a sleeping HDD; the snapshot read wakes it too. There the user sees what any HDD read looks like: the row stays highlighted (no spinner) and the push starts when ready.

Mitigations, in order:
1. the LRU keeps the last modules resident (the common re-entry costs nothing);
2. opportunistic fill while awake;
3. the BCM snapshot cache below.

That keeps the same behaviour as flash, with a different wait, which is the rule in `storage-choices.md`.

### 2.6 Debuggability, panics, sim

- **Panics and backtraces:**
  - each load logs `mod load id= base= bytes= build= ms=` to `ipodui.log`;
  - a panic PC or LR inside the arena is logged as `id+offset` (the registry knows each base), then `arm-none-eabi-addr2line -e <module>.elf` at link base + offset;
  - panics stay logged, not shown (rule 1).
- **No device debugger exists anyway.** The Rockbox `.rock` situation is no better.
- **Sim:** modules are **linked statically** into `rockboxui` behind the same registry API (`mod_get(id)` returns a static descriptor; load and evict are no-ops that still log). `tests/ui/*.script` run unchanged, and snapshots still work in the sim (the DMA becomes `read()`), so the batch and its pixel check run there.
- A host test in `tests/modules/` covers pack index parsing, build-ID refusal, relocation (apply to a buffer at two bases and compare with linking at those bases), LRU and move.

### 2.7 Code and RAM estimate (resident, Thumb) [est]

| Part | Bytes |
|---|---|
| Pack open, index, async read request, ready flag | ~300 |
| Relocate + verify (Fletcher, build ID) | ~150 |
| Arena: first-fit, LRU, refcount, move (memmove + re-reloc + cache op) | ~500 |
| Registry, callback-range check, panic `id+offset`, logging | ~350 |
| Snapshots: key table, DMA into framebuffer rect, batch routine, write-in-place | ~700 |
| Preload hints (dwell timer hook, awake gate) | ~150 |
| **Code total** | **~2.1 KB** |
| Statics: index (~20 × 24 B) + key table (~16 × 16 B) | ~750 B bss (or inside the arena) |
| Per module, resident: menu row stub + descriptor | 16-32 B |
| Per module, on disk: veneers +12.5 %, relocs +2.2 % | |
| **Build tooling** (not on the device): `module.lds`, a `root.make` rule (compile Thumb → link `-R rockbox.elf` at 0x03000000 → reloc extraction by two-base diff or `--emit-relocs` → pack), a `--undefined` keep list for the core link, `buildzip` entry | ~150-250 lines make + ~150 lines Python |

**Break-even:** the first moved tenant. The Settings pack alone takes 12.7 KB of resident text out; with `_end` arithmetic, the framework pays back ~6×.

### 2.8 Risks

| Risk | Mitigation |
|---|---|
| Linker coupling (build ID): a stale module from an old zip | Refused and logged, its parent screen stays. The zip is always whole |
| A module calls something the core GC'd | The core link's generated `--undefined` list; a link error, not a runtime one |
| Runtime pointer into a moved or evicted module (callback, tick task, list ops) | Moves and evictions only at refcount 0. The registry refuses (and logs) if any registered callback or timer points into the range; no threads in modules |
| Writable `.data` corrupted by a move | Linker `ASSERT` |
| Async read races with USB entry | Loads stop at USB ack (no disk access at entry); the pack fd closes on the exit path |
| HDD UI stall | All reads on a worker; the UI thread never blocks on a module |
| Snapshot mismatch (wrong pixels for 190 ms) | Keys + developer frame hash + sim pixel test |
| Fragmented `modules.pak` on HDD (host-placed) | `fat_readwrite` splits; costs extra seeks only. A defrag-on-write at the post-USB moment is possible later |
| Thumb veneer overhead grows with many tiny modules | Group cold screens into packs (one module = a related set) |

---

## 3. COP and BCM as helpers

- **COP: no.**
  - Loads are DMA-bound;
  - relocation is ~10 µs;
  - Fletcher is ~1-2 ms once per mount generation;
  - there is no compression to offload (§2.4).
  - The COP decodes audio ([silicon S36]), and handing work across needs cache maintenance on both sides plus a tick-paced wake (`data-paths.md` M2, R7).
  - The one COP fact that matters is negative: **drop `lc_open`'s COP flush** for shell modules.
- **BCM: yes, later, as a snapshot cache and slide engine** [hypothesis; depends on our image being the default display driver, #76 second frame, SDRAM speed (open question #7)].
  - **Snapshot cache in BCM SDRAM:** the idle DPB area (2 MB, audit C §6) holds ~14 body snapshots, which is more than the whole scoped set.
  - A cached entry needs **no disk read at all**, which fixes the sleeping-HDD case while the screen is lit.
  - It is wiped when the BCM powers down with the backlight (`host-driver.md:49`) and refilled lazily from `snap.pak`.
  - **Upload:** 139.5 KB in ~9.4 ms boosted by CPU copy (S8: 14.8 MB/s) [silicon-derived est]; PP DMA into the port is a dead end today (`data-paths.md` D1).
  - **Push slide on the BCM** with the second frame (#76): the PP would no longer need the strip for slides, a further step toward shrinking or freeing the 180 KB strip.
  - **Cover Flow textures** in BCM SDRAM (audit C §6.3) would remove the 512 KB arena need altogether. Then the arena returns to the 64-180 KB module/drift size.

---

## 4. Recommendation

1. **Decision:** don't adopt `plugin_load`/`.rock`, and don't re-add the plugin API. Keep the FAT/ATA read path as it is (it is already zero-copy DMA). Write the shell loader described in §2: ~2.1 KB resident, no API table, build-ID coupling.
2. **Spike (P16, `firmware-platform` + `tooling` for build rules), with no stock-UI tenant:**
   - **Tenant A, the cold Settings pack** (list screens, so **no snapshot**: drawn live by the module): About, Legal, Date & Time, Language, Reset, Hold menu, System screen, New software. 12.7 KB text out of `_end`, 121 core symbols, a measured 14,656 B module.
   - **Tenant B, a minimal ProkBox developer screen written as a module from day one:** "Loader & storage" with module list, arena map, load timings and snapshot key states, as a full (non-list) screen. It is the seed of our own developer UI, which will be one or more modules, and **the spike's one snapshot tenant**, exercising the batch path before Cover Flow and Extras exist.
   - Deliver:
     - `module.lds` with ASSERTs;
     - the pack tool;
     - the `--undefined` keep list;
     - the registry with static linking in the sim;
     - async load on an existing worker;
     - a 64 KB arena reserved at shell start and counted in the floor trace;
     - the snapshot batch at boot and after USB, with "Updating Language…" wired to the same routine;
     - the host test.
   - **Spike gates:**
     - module screens pixel-identical to today's resident screens in the sim; tenant B's snapshot pixel-identical to its live first frame;
     - `_end` drops by ≥ 12 KB;
     - floor net ≥ −52 KB (arena 64 KB − 12.7 KB moved), honest until more tenants move or the arena is shared;
     - zero splashes;
     - `cpu_boost` balanced.
3. **Then:**
   - Search, once its RAM-cache read is shown not to need resident code at Music-menu open;
   - the rest of our developer UI;
   - P12 Extras as resident engines + modules;
   - Games (#99) as modules;
   - the P14 Videos session as a tenant.
4. **P7:** make the arena Cover Flow's texture home and move the second drift slot (and, if the slide code allows, the strip) into it. Floor saved by sharing: ~244-424 KB. Add the Cover Flow opening snapshot. If our BCM image is the default display driver by then, put the textures in BCM SDRAM instead, and the snapshot cache too.
5. **Delete the stock UI** (owner ruling) after the developer module covers what the owner uses from it: ~28-30 KB text + 2 KB bss out of developer builds, plus `main()` hooks. The loader never needs Rockbox's API surface.

## 5. What must be measured on the device (procedures; results from the owner)

1. **Load latency:**
   - developer build, tenant B screen;
   - enter/leave each module 50× while playing;
   - log `mod load bytes= ms= cmd=`, plus PCM underruns (must be 0);
   - on flash, and on a `--storage=hdd` unit or sim profile if available;
   - include `lc_open` on the same file for the A/B (it shows the COP-hop cost).
2. **Snapshot batch time** at boot (from tenant B, then from P12's set): everything stale (first boot after install) against nothing stale. Log `snap batch n= ms=`. Target: a full rebuild under ~1 s on flash; under the boot budget on HDD.
3. **SSD DMA write rate** for the 139.5 KB in-place writes (the first device number for `snap.pak`).
4. **Floor trace** with the 64 KB arena on 32 and 64 MB sims, then on the device's buffer screen.
5. **Fletcher-32 cost** per KB at 80 MHz (one log line), to confirm the once-per-mount-generation choice.
6. Later (P7/BCM): the BCM SDRAM upload rate and latency for snapshots (open question #7).

Spike files kept for reference:
- `scratchpad/ld/mod.lds`
- `scratchpad/ld/f_0x03000000.elf`
- `scratchpad/ld/f_0x03100000.elf`
- `scratchpad/ld/r_*.elf`
