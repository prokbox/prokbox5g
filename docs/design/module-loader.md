# Shell modules: our own loader, and snapshot-first screens

Decision record, 2026-10-20. The full study, with measurements and citations, is `docs/research/memory-2026-10-20/loader-study.md`. The budget context is in `docs/memory-audit-2026-10-20.md`.

## Decision

**Don't adopt Rockbox's plugin/codec loader (`plugin_load`, `lc_open`, `.rock`, the 419-pointer `rockbox_api`) for shell modules. Write a small loader built for this hardware**, about 2.1 KB Thumb resident (estimate).

Owner, 2026-10-20: "Before we adopt the Rockbox codec loader, examine if it is appropriate here, and consider if we should write a purpose built one for our hardware."

What we keep from Rockbox's path: the FAT/ATA read. Whole sectors already go by IDE DMA straight into the destination, coalesced into one command, at ~21 MB/s, with no file-system copy (`file.c:622-760`, `fat.c:2553+`, `ata-pp5020.c:267`).

What we reject is everything above `lc_open`:
- one fixed slot at the top of RAM, and no relocation;
- `plugin_load()`'s UI side effects (a "Wait" splash, clearing the LCD, stock UI calls);
- synchronous reads on the UI thread;
- a COP round trip per load to flush its cache (cost unmeasured, possibly ~20 ms);
- an API table with version coupling.

## Design

**Linking**, proven by a spike on the real build:
- Modules are Thumb, linked at a far virtual base against `rockbox.elf` with `--just-symbols`.
- GNU ld 2.47 generates correct ARMv4T `bx` veneers.
- Every remaining position dependency is a plain ABS32 word: 161 relocations for a 14,656 B module (12,830 B of objects, 113 veneers, ~12.5 % overhead).
- No API table. Core and modules come from one build, checked by a build ID.
- Linker asserts forbid writable `.data` and IRAM in modules.

**Memory:**
- One static **transient arena**, reserved at shell start and counted in the floor (an allocation while playing would cut into the audio buffer).
- First-fit allocation, an LRU list and a refcount per tenant. A module is held while any of its screens is on the stack or any of its callbacks is registered.
- Idle modules can be evicted (nothing is written) or moved (memmove + re-apply relocations + one cache op), only between screens on the UI thread.
- *Forward note (P12 brief rev 2, §2.4, 2026-10-23): P12 recommends reversing the Cover Flow plan below. Keep the Class A arena small (40 KB, 72 KB with games) and give Cover Flow its textures as a Class B borrow or in BCM SDRAM, decided in P7's brief.*
- **Size:** 64 KB until P7. From P7 the same arena is Cover Flow's texture home (~512 KB), shared with modules and the second drift slot: 244-424 KB of floor saved against summing. If the textures go to BCM SDRAM, the arena drops back to ~64-180 KB.

**Read path:**
- All modules ship in one `modules.pak` in the hidden folder: a 2 KB index, then each image starting on a 2,048 B sector boundary.
- It is opened at shell start and after each USB disconnect, and the fd is kept open. It is closed on the USB ack path, with no writes at USB entry (owner rule).
- A load is `lseek` + one aligned `read`, run on an existing background worker (no new thread); the UI keeps animating.
- Estimated latency:

  | Storage | 16 KB module | 48 KB module |
  |---|---|---|
  | Flash | 1.5-2 ms | 3-3.5 ms |
  | HDD, awake | 22-25 ms | 25-35 ms |
  | HDD, asleep | + 1-3 s spin-up (hence #104's predictive spin-up) | |

- No compression: decoding on the ARM7 is slower than simply reading.
- **Integrity:** build ID, size and magic on every load; a Fletcher-32 once per mount generation (boot and USB disconnect).

**Simulator:** modules link statically, so the UI scripts run unchanged.

## Snapshot-first screens (owner pattern)

Owner, 2026-10-20: pre-render the first screen of a heavy feature, show it at once, "and then, in the background, render/load the remaining bits, … so it looks like the user's actions are responded to instantaneously."

Rules:
- **Scope (owner):** no snapshots for the main menu or any split-screen list. Lists and menus are always drawn live. Right-side panel images may be pre-rendered (`panels.pak`, the art cache).
- **What gets snapshots:** heavy on-demand full screens only: Cover Flow's opening view, Extras apps, games, and the developer UI's first screen.
- **How they show:** a snapshot is read by DMA straight into the framebuffer during the push animation, so it costs 0 B of PP RAM. The module or working set loads behind it, and the screen goes live within ~0.5 s, hidden by motion. There is never a spinner in normal use.
- **Three rebuild moments (owner), one batch routine:**
  1. **The boot hold:** "a few seconds" is acceptable; stock takes 5-10 s before the main menu, and sleep/instant resume (#86) makes full boots rare.
  2. **The post-USB busy moment (#84).**
  3. **A visible "Updating Language…"** with an activity indicator, after a language change.
- **On-device settings:** changes that a snapshotted screen shows mark only that snapshot stale. It is skipped, and the screen draws live until the next batch.
- **Cost:** a full rebuild of ~8-10 snapshots takes ~0.25-0.6 s on flash and ~0.5-1.1 s on HDD (est), about 1.4 MB on disk. A typical boot rebuilds nothing.
- **Intent preload:** loading starts when a row is highlighted, not when it is selected. The same "reachable next" map drives #104's predictive spin-up.

## First step: the P16 spike

- **Tenant A: the cold Settings pack.** About, Legal, Date & Time, Language, Reset, Hold menu, System screen, New Software. Drawn live (list screens: no snapshot). 12.7 KB of text out of `_end`, 121 core symbols.
- **Tenant B: a ProkBox "Loader & storage" developer screen,** written as a module from day one. It shows the module list, arena map, load timings and snapshot states. It's the seed of ProkBox's own developer UI (#103), in the black-and-white disk-mode look (owner), and the spike's one snapshot tenant.
- **Gates:**
  - module screens pixel-identical to today's resident screens in the sim, and tenant B's snapshot identical to its live first frame;
  - `_end` drops by ≥ 12 KB;
  - the floor trace holds;
  - zero splashes;
  - `cpu_boost` balanced.
- **Device:**
  - load latency over 50 entries while playing, with 0 underruns, plus an `lc_open` A/B;
  - the snapshot batch time;
  - the floor trace on 32 and 64 MB;
  - the Fletcher cost.

## Spike result (P16, #106, measured 2026-10-20)

Built on branch `p16-loader` as designed: `apps/ipodui/modload.{c,h}` (arena, LRU, moves, stand-in screens, error page, snapshots), `modload_core.c` (pack checks, relocation, Fletcher-32, file growth; host test `tests/modload`), `tools/modpak.py` + `apps/ipodui/modules/` (Thumb modules linked twice with `--just-symbols`, relocations by diff, `.data`/IRAM asserts, the core's `--undefined` keep list, a build ID that hashes the core image and every module, written into `rockbox.bin` after the objcopy), reads on the storage thread (`storage_post_job()`).

Changes from the plan above, after the reviews:
- **Tenants:** Legal, Date & Time, the Main/Music Menu checklists and the "Loader & storage" developer screen. About, Reset Settings and Volume Limit (+ roller) stay resident: they must never depend on a file loading (About is also the developer chord's entry). The hold menu, the system screens, the language list (shared with EQ) and New Software (shown exactly when core and pack disagree) were never candidates.
- **No blocking push:** a module screen is pushed as a stand-in with the real title and header shape (the snapshot, or the empty list); the real screen takes over when the read lands. Intent preload on highlight (disk awake) and a warm-up on entering Settings (#104) usually make the swap instant. A pack that is missing, refused or from another build gives a styled "Not Available" page (LANG_*), retried on the next attempt.
- **Arena sized to the tenants:** 11,008 B side by side; 14 KB developer, 8 KB release (+~25 %).

**Measured cost (modules on vs off):** developer build text +1,588, bss +2,640, `_end` +4,096 (the 4 KiB `inquiry_static` step), arena 14 KB: −18.4 KB of audio buffer; release text −736, bss +736, `_end` 0, arena 8 KB: −8.2 KB. The ~5.2 KB resident loader outweighs the ~5.6 KB moved. Module images: settings 6,076 B (92 relocations), devloader 4,360 B (55).

**Decision (orchestrator, 2026-10-20): merged, OFF by default** (`-DIPODUI_WITH_MODULES` turns it on). The default build is the resident one, byte for byte. The default flips after the device bench passes (owner 2026-10-24: games need modules; two device-only loader bugs found and fixed at the first bench). Until then P12 develops and gates with the define, and the gate builds `sim-ui-mod` and `fw-ui-mod` for it (`tests/ui/README.md`). The moved code (Extras apps, games, the ProkBox developer UI and later Videos) exceeds the loader many times over; P7 then shares the arena with Cover Flow. "Updating Language..." is not built yet (a language change only makes the snapshot key stale).

**Device checks still open:** load latency over 50 entries while playing (flash and HDD) with 0 underruns, the stand-in-to-real swap time, the storage thread's `stk=%`, the snapshot batch time, the Fletcher cost, the error page with the pack removed.

**Then:**
- Search;
- the rest of the developer UI, after which the stock UI is deleted (RUI-3, #103);
- P12 Extras as resident engines + modules;
- Games as modules (#99), not Rockbox plugins: restoring the plugin path would cost ~0.7 MB of audio buffer and brings Rockbox's UI back;
- P14 Videos as a session tenant.
