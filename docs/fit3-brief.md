# Fit-3 brief: efficiency from the data-path map, and the owner's polish notes

**Goal (owner, 2026-10-15):** "Let's plan and see if we can get started on any of our code efficiency bits soon … we're hoping to really nail this thing down and be able to provide a very elegant experience here." The work comes from the read-only audit of our code against `docs/data-paths.md` (issues #75–#78), the PP's second DMA engine (open question PP #11), and two device notes (#73, #74).

**Rules that apply throughout:** measure before and after, never claim a gain without a device number; fidelity first (`docs/process.md`, `memory/efficiency-without-fidelity-loss`); every path used must be a [silicon] cell of `docs/data-paths.md`, or the experiment that settles its [unknown] cell comes first.

## 0. Split by risk

Revised after the brief review (2026-10-15: five High findings; art on the COP needs its own design, the slideshow lives in `preview.c`).

| Mini-phase | What | Risk | Device dependency | Owner of the code |
|---|---|---|---|---|
| **Fit-3a** | #74 the scroll bar as a transparent overlay that never moves text; #73 the slideshow's second set not disappearing (diagnose, then fix) | UI only | none to build; a device look | `ui-fidelity` agent |
| **Fit-3b** | Measurement counters only (the contract below, committed with its host test `tests/devstats/`), shipped in the next device build | developer stats, one FIQ add | one device session for baselines | `firmware-platform` agent |
| **Fit-3c** | After Fit-3b's numbers: whichever of art decode on the COP (its own design: a file buffer, a memory decoder, core-safe resize, stack sizes), the PCM write-back out of the FIQ, the per-tick port read, packed partial rectangles the numbers justify | memory, COP, audio interrupt, LCD driver | Fit-3b's numbers | orchestrator, a delta brief per item |
| **Fit-3d** | **Waits for the second DMA engine's RE trace to finish** (owner, 2026-10-15: "we'll probably have a new pattern to both test and improve"). Then the lab, built from the pattern the trace shows (written by us into dev45's devtest, coordinated with the bcm session), and only if it lands bytes, the video feed on it | DMA into the BCM | the RE result, then the lab's device result | orchestrator + bcm session |
| **Fit-3e** | #76 a second frame in BCM SRAM (no tearing, no wait), then #75 the BCM composing UI updates (scroll and slide by moving rows on the chip; the PP sends only new rows) | the display image, every screen | after dev46 merges; vc2sim, then device | own brief, with the bcm session |

Fit-3a and Fit-3b start now (worktrees `s12o` and `s12`, to stay above the 5 GB disk floor).

## 1. Owner pre-flight
- Answered: the goals above; #73 and #74 wording (the issue texts, owner's words); "the second DMA engine thing does seem important" and its lab waits for the trace.
- ⚠️ Frozen with defaults until the owner round:
  - the scroll bar's look: the existing thumb (`SBAR_W` 7, rounded, no track) blended at 60 % over whatever is under it;
  - #73's fix: **the same visible behaviour on flash and hard-drive units** (owner, 2026-10-15: one build, identical behaviour, the flash mod only a raw benefit; `docs/storage-choices.md`). Whichever the diagnosis supports, within today's memory: the panel never goes empty when the set changes (it keeps a frame of that set, or reads it at once, on both storage types); if the mechanism costs more on a spinning disk (a spin-up), that is a tuning difference recorded in `docs/storage-choices.md`, never a different look. Anything needing new memory comes to the owner round.

## 2. Read list per agent (at most 3)
- **Fit-3a:** `apps/ipodui/list.c` (§ "A split list's scrollbar", `draw_scrollbar`, `row_w`, `sbar_reflow`), `apps/ipodui/preview.c` (the drift slot, `slot_read`, the scheduler), `docs/design/spec/` main menu and list files.
- **Fit-3b:** `firmware/export/devstats.h` (the contract), `firmware/common/devstats.c` and the stats rows in `apps/ipodui/devmode.c`, `docs/data-paths.md` §0 and R1/R2/R5/R7.

## 3. Contracts

### Fit-3a
1. **Text geometry never depends on the scroll bar.** `row_w()` returns the full row width; `sbar_reflow` goes. A forced-bar toggle (a sim command, `list_force_bar on|off`, in its own `sim_cmd_*.c`) lets a script take the same list with and without the bar; `scripts/pngdiff` must report zero differing pixels outside the bar's rectangle.
2. **The bar is drawn last, over everything**, blended over pixels that were drawn fresh in the same frame (rows are always redrawn under it before the blend, so the thumb never compounds). Hiding it is one full-width band over the bar's height (the LCD rule), never a full update.
3. **#73:** first a trace (`UPDATE_TRACE("panel set read set=%d why=%s")` at every read, deferral and skip of the drift slot) and a script that moves between Music and Audiobooks rows for 60 s on a sleeping disk, showing why the first set goes empty and how long until it returns. Then the fix (defaults in §1). The script runs in the default (flash, `HAVE_FLASH_STORAGE`) sim **and** a `--storage=hdd` sim (`IPOD_STORAGE_HDD` → `HAVE_DISK_STORAGE`), and both must show the same panel behaviour. No allocation from a panel or draw callback (checklist 2). A trace rule that finds none of its lines fails.

### Fit-3b: counters (committed before fan-out: `firmware/export/devstats.h` `struct devstats_fit3` and the `DEVSTATS_LCD/OVL/RD_TICK/RD_POLL/ART/PCM_WB` hooks; host test `tests/devstats/run.sh`)
- **Sites:** `DEVSTATS_LCD` around the UI's port push in `lcd-video.c` (bytes and µs per update); `DEVSTATS_OVL(w*h*2)` in `ovl_apply()` (`apps/video/vpcore.c`, video thread only; it also counts the re-sends after an OPEN); `DEVSTATS_RD_TICK` at `lcd_tick`'s port read; `DEVSTATS_RD_POLL` counted at the caller of `bvp_poll()` in `vpcore.c`/`vplayer.c` (not inside `bcmvp.c`); `DEVSTATS_ART` once per decoded image in `artcache.c` (bytes, read calls, read µs, decode µs, `ART_BOOST` delta); `DEVSTATS_PCM_WB` at the call site in `fiq_playback()` (`pcm-pp.c`, ICODE), **not** in the shared inline `dma_tx_buf_prepare()` (its other caller, `sink_dma_start()`, runs in thread context): a `USEC_TIMER` delta around the prepare, adds only, no calls; the FIQ is then the only writer. `devstats_snapshot()` masks IRQs, not the FIQ, so the two `pcm_wb` words can tear by one event: negligible for deltas.
- **Writers:** `lcd_update_rect()` may run from more than one thread; that is safe because scheduling is cooperative and nothing on the COP pushes. `lcd_max_us` excludes `bcm_poweron_wait()` (a backlight wake), measured from after it.
- **Off:** compiled out without `HAVE_DEVSTATS` (every non-developer build); in developer builds the cost is the adds.
- **Readout:** the developer stats screen shows per-minute rates (deltas, never clearing). The once-a-minute `ipodui.log` line follows S36's flush-on-awake rule: it is buffered and written only when the disk is already awake, never waking it.

## 4. File ownership

| File | Owner |
|---|---|
| `apps/ipodui/list.c`, `apps/ipodui/preview.c`, a new `uisimulator/common/sim_cmd_fit3a.c` (+ its hook line in `sim_script.c`, via the orchestrator), `tests/ui/ipodui-fit3a-*.script` | Fit-3a agent |
| `firmware/common/devstats.c`, the stats rows in `apps/ipodui/devmode.c`, counter lines only in `firmware/target/arm/pp/pcm-pp.c`, `firmware/target/arm/ipod/video/lcd-video.c`, `apps/ipodui/artcache.c`, `apps/video/vplayer.c`/`vpcore.c` (callers of `bvp_poll` and the overlay post; the bcm session edits only comments there this week, so counter lines go in separate hunks) | Fit-3b agent |
| `firmware/export/devstats.h`, `tests/devstats/` (the contract), `docs/hardware.md`, `apps/SOURCES`, ledgers | orchestrator |
| `bcmfw/*`, `apps/ipodui/devbcm.c`, `bcm-devtest-*.c`, `apps/video/bcmvp.c` | bcm session (dev45/dev46); Fit-3d appends only, as agreed |

## 5. Device-reality checklist answers
1. **Compiled features:** the counters exist only with `HAVE_DEVSTATS` (developer builds); `USEC_TIMER` times are target-only (the sim records counts, times as 0 or tick-based). Fit-3a runs the same code in the sim and on the device; #73's disk-sleep behaviour depends on the sim modelling the disk's sleep timing (checklist 4): the script uses the existing disk-active model.
2. **Memory:** no new allocation in either mini-phase. `struct devstats_fit3` adds 64 B to a static that already exists in developer builds. #73 stays within the drift slot and panel scratch, or goes to the owner round.
3. **CPU:** counters are a few adds per event; the FIQ add is two loads, two adds, two stores. The bar's blend is one pass over its own rectangle per frame.
4. **Storage:** the log line never wakes the disk. #73 on a flash unit reads one 180,000 B panel at once instead of waiting: one flash wake per set change at most, counted in the trace.
5. **LCD:** the bar's hide is one band; no full updates added.
6. **Threads and locks:** counters are single-writer per field (the FIQ, the UI thread, the art thread, the video thread). The UI reads with `devstats_snapshot()`.
7. **Splashes:** none.
8. **Persistence:** none.
9. **Scale:** independent of library size.
10. **Deferred work:** #73's deferred read is exactly this: its states are disk asleep, disk awake, USB connected, playing; the script covers asleep and awake, and the trace names each deferral.
11. **Stock firmware unchanged:** everything is under `HAVE_IPOD_UI` or `HAVE_DEVSTATS`.
12. **Existing paths:** the drift scheduler's own deferral rules (bulk-I/O, disk-awake) are the path #73 changes; no second reader is added.
13. **Builder predicate:** not touched.

## 6. Acceptance
- **Fit-3a:** the pixel-identity check (contract 1) on a split list and a full list; shots shown and hidden, opened for the owner; #73's script and trace showing both sets present throughout (or the diagnosis and the owner question if the fix needs memory); gate passes.
- **Fit-3b:** the stats screen in the sim with counts moving (in the sim only ART, OVL and RD_POLL can move; LCD, RD_TICK and PCM_WB live in target-only files and are checked by building `fw-ui-video` and on the device); the log line format with a parser dry run; `tests/devstats/run.sh` passes; gate passes; in the next device build with a one-line test card for the owner ("play a song, open a few lists, play a video for a minute, leave it on the main menu for a minute"). The numbers decide Fit-3c's order.
- **Review:** `budget-reviewer` on the integrated diff at the end; brief re-review on this revision before fan-out.

## 7. Delta after the device numbers (S37, 2026-10-15)

Fit-3b's counters on the device (`session log, session 37b ipodui.log`) and the E2 lab reorder Fit-3c/d/e:
- **The PCM write-back from the FIQ is negligible:** ~7,400 a minute costing 7.7 ms a minute (~0.01 %). Dropped from Fit-3c (#78 item c).
- **Port reads:** ~1,000 a minute (lcd_tick ~430, video poll ~625). At R2's 35–85 µs each, about 0.1 % of CPU. Low priority (#78 a).
- **UI frames over the port are the cost:** one busy minute pushed 272 updates, 19.7 MB in 2.1 s, with full frames taking 27–34 ms. This is the job for the second DMA engine, which now works into the BCM (`data-paths.md` cell 14: the CPU keeps 77–97 %). Fit-3e (#75/#76) gains an E2-fed frame path, after the bcm session's player backend (`VPLAYER_TUNE_E2`) proves it.
- **Art:** no art builds ran in the window, so there are no numbers yet. Art-on-COP stays deferred until a window with builds shows its cost.
- **Storage detection:** this adapter reports w217=0 but w83 bit 2 and ssd=1 (`storage-choices.md` §1).
