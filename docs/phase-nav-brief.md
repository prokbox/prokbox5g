# Brief: navigation model (#38) + hardening H1

Execution order 6. It starts once Phase 4b is merged to `main`. Lifecycle per `docs/process.md`: brief → **brief review** → fan-out with self-review → one full gate → end review → one owner round → close.

Read first: `CLAUDE.md`, `docs/process.md` (device-reality checklist, ownership, one gate at a time), `docs/hardware.md` (planning scale 30–40k tracks / 3–4k albums), `docs/design/spec/index.md` (**Navigation model, Transitions, List geometry, Attribution**), `docs/design/spec/main-menu.md` (**split screen, panel drift**), `docs/design/spec/settings.md` (**Settings split screen**), `docs/design/7g-catalogue.md` (§7.1 Settings panels), `docs/reference/7g/owner/VIDEO-2.md` (measured timings) and its labelled frames, `docs/design/artcache.md` §7, `docs/research/lcd-fps-ipodvideo.md`, `docs/risk-tooling.md`.

## Part 1: hardening mini-phase H1 (core; runs in parallel; its own scoped review)
Owner: **playback-audiobook** (top model). Files: `apps/tagcache.c`, `apps/ipodui/library.c`, `apps/ipodui/artcache.c`, playlist and audio paths only as needed, `tests/`.
1. **#41.1:** the art builder skips GC while `playing()` and defers it to stopped.
2. **#41.2:** the precompute block is **reserved once at shell start**, before the audio buffer is set up, sized from the tagcache entry count + 25%. The precompute runs any time: unboosted, chunked, never allocating. If the library has outgrown the block, it falls back to stopped-only, and the next boot resizes.
3. **Audio-thread stack (#42):** the worst static path is 7,968 of 5,120 B, via playlist building → `action_userabort()` → `default_event_handler()` → `gui_usb_screen_run()`.
   - Determine whether that path is reachable *on the audio thread*.
   - If it is, make sure the audio thread never runs the USB screen. Defer it to the UI thread. Prefer a guarded change to sizing the stack up.
   - Re-run `scripts/risk/stack_usage.sh` afterwards. Also review the playlist thread (80%) and the main/UI thread (73%).
4. **UBSan unblock:** `apps/ipodui/library.c:184` declares `extern clock_gettime_nsec_np(int)`, which conflicts with the macOS SDK. Use the SDK header under `SIMULATOR`. Then run `scripts/risk/sim_ubsan.sh` over **all** ipodui scripts and fix every alignment or UB finding, especially in the binary formats (`database_lib.dat`, `index.dat`, `thumbs.pak`, the changelog).
5. Device-checklist items go to `ROADMAP.md` P10. H1 merges ahead of the #38 merge.

## Part 2: navigation model (#38)

### Behaviour (from the spec and the owner's video; numbers are measured)
- **Split screens:** at each level **above the leaf**, the list is at x 0–159 with the header above it, and a **preview panel** at x 160–319 runs the full height (y 0–239).
  - **Main menu:** Music → **drifting album art**; Podcasts → podcast icon + "N Podcasts"; Extras → world map + 3D clock + date; Settings → capacity panel (device name, our own logo, capacity bar, "N GB Free"); Shuffle Songs and Now Playing → per the spec and catalogue.
  - **Music menu:** drifting art.
  - **Settings:** a tile, a reflection and a caption per item (catalogue §7.1). **Leaf lists such as Backlight stay split**, with the panel holding the tile and the value.
  - **Extras:** per-item previews, where available.
  - Artists and deeper lists are full-width (the owner confirmed where the split stops).
- **Panel updates** happen only after the selection **rests**: about 0.4 s for icon panels, about 1.1 s for covers. The arrival style varies (slide or cross-fade). Use a cross-fade unless the video says otherwise, and mark it ⚠️.
- **Panel drift (Music and main menu):**
  - each cover dwells about **7 s**;
  - it pans steadily at **5–8 px/s** in a varying direction, zooming at most about 5%;
  - covers change with a **~400 ms cross-fade**;
  - the cycle is **not reset** by a push or by Cover Flow.
  - **Implementation:** integer-pixel pan steps, i.e. a plain blit roughly every 125–200 ms. The zoom comes from a pre-scaled pair or happens only at cover changes. **Never a per-frame resample.** Drift runs only while the backlight is on and never boosts. Budget its CPU share with the instruction-count tooling and report it.
- **Transitions** (spec "Transitions"; **always a slide, never a cut**):
  - **Push/pop:** split → split ≈ **190 ms** ease-out; split → full-screen ≈ **250 ms**. The header title swaps at the start, and between split screens the **panel stays put**.
  - **Reveal** (Clocks, as measured): the background stays, the list, header and panel content slide out to the left while the destination's own parts slide in from the right, in about **250 ms**. MENU mirrors it.
  - **Split** (Cover Flow entry, for Phase 7; build the transition type now and stub it): the screen parts at x=160. The list and header slide left, the panel slides right, rigidly with an ease-in, in about **210 ms**, over a pre-rendered destination. The exit (~290 ms) is the panel in from the right first, then the list in from the left.
  - Prefer **full-width pushes** (the LCD's slow path applies at width ≠ 320).
- **Art cache:** add `ARTCACHE_PANEL` at **≈300×300** (enough for ~30 px pans), built for the **panel set** (≤32 albums: recent and random), using the existing builder rules (playback-safe, never boosting). Update `artcache.md`.

### Work split (worktrees; one owner per file)
| Agent | Owns | Must not edit |
|---|---|---|
| **A: framework and core** (ui-fidelity, top model) | `screen_stack.*`, `anim.*`, `ui_main.c`, new `preview.{c,h}` (the preview-provider API and panel scheduler), `menu.c` (the split and preview wiring), `artcache.*` (panel size and the panel-set build), `library.*` (panel-set selection) | `paint.*`, `theme.*`, `header.*`, `screens/*`, sim tooling |
| **B: panels and Settings** (ui-fidelity, top model) | new `screens/settings.c` (the stock tree as stubs that use the split and the tiles), panel renderers in `screens/panels.c` (podcast and capacity panels, the Extras map and clock as stubs), `paint.*` and `theme.*` (tiles, reflections, captions, blue inline values), `art/*.py` (original icon tiles) | `screen_stack.*`, `anim.*`, `preview.*`, `menu.c`, `artcache.*`, sim tooling |
| **C: verification** (tooling, Sonnet) | `tests/ui/*`, `scripts/*`, `uisimulator/*` | `apps/` |

### Contracts (A defines, B implements the providers)
```c
/* preview.h */
struct ui_panel { struct viewport vp; /* x160..319, y0..239 */ };
struct ui_preview_ops {
    /* Draw the preview for the highlighted item into the panel.
     * Called after the selection rests (delay per kind). Must not allocate
     * after shell start; use the reserved panel scratch. Return the next
     * wakeup in ticks for animated previews (drift), or TIMEOUT_BLOCK. */
    int  (*draw)(const struct ui_panel *p, const void *item_arg, int phase);
    int  rest_delay_ticks;          /* ~HZ*0.4 icons, ~HZ*1.1 covers */
    enum { UI_PANEL_XFADE, UI_PANEL_SLIDE } arrive;
};
/* menu item gains:  const struct ui_preview_ops *preview; const void *preview_arg;
 * leaf screens may expose draw_into_panel() so reveal has no visual jump. */
enum ui_transition { UI_T_PUSH, UI_T_REVEAL, UI_T_SPLIT };
int ui_push_t(const struct ui_screen_ops *ops, const void *arg, enum ui_transition t);
```

### C: verification
- **Retarget** the Phase 3 long-list scripts (`ipodui-p3-scroll`, `-marquee`, `-motion-dev`) from the Settings stub to **Music → Songs**, so scroll coverage isn't lost when Settings becomes split.
- **New scripts:** main-menu panels per item; drift (pin the drift phase and the clock for goldens; add a `drift_phase` sim command); the Settings split, including the Backlight leaf; push/pop/reveal timing assertions in `check_trace.py` (durations within ±1 frame of the spec); a split-transition stub test.
- **Trace rules:** drift never boosts; drift is idle while the backlight is off; panel updates only after rest; transition push widths are full-width where the spec says so.

### Device-reality checklist (answers required)
1. **Memory:** all panel scratch, drift buffers and panel-art buffers are reserved at shell start (budget lines). Nothing is allocated after playback starts.
2. **CPU:** drift's CPU share is measured with the instruction-count tooling, and the transitions' per-frame cost estimated (target: 30 fps cap held boosted, with headroom).
3. **Storage:** panel art reads happen only at a cover change (every ~7 s), and during playback only while storage is awake. If storage is asleep, keep showing the current cover.
4. **LCD:** full-width pushes; the panel-only updates (160 px wide) are accepted as the slow path, but small.
5. **Threads:** the panel scheduler runs on the UI thread; art comes from the cache reader.
6. **Persistence:** none new.
7. **Splashes and strings:** none. Settings captions use stock wording.
8. **Scale:** a panel set of ≤32 albums; Settings is static.
9. **Deferred work:** drift pauses with the backlight off and on USB; panel art waits if storage is asleep.
10. **Stock firmware unchanged.**

### Acceptance
- One full gate, with **the speed-up gate if it has merged**.
- The end review confirms; then **one owner round**: side-by-sides against the owner's photos and video frames (main-menu panels, Settings split, Clocks reveal stub, drift sequence). Goldens are regenerated after approval.

## Revision 1 (after the brief review: REVISE, 2026-10-07). This overrides anything above.

### Sequencing and ownership
- **H1 merges BEFORE A, B and C start.** H1 now owns **all** art-cache and library changes for this phase, including the panel art. A, B and C consume H1's contracts.
- H1's UBSan sweep fixes findings only in H1's own files. Findings elsewhere are reported to the orchestrator, or noted in the ROADMAP.
- Unowned files get owners: `list.c` (blue inline values) → **B**; `header.*` → **A**.

### H1: additions and corrections
1. **Panel art (moved from A):**
   - `ARTCACHE_PANEL` is ~300×300. The **panel set (≤32 albums: recent and random)** is chosen **at library update**.
   - **Panel bundles are built only while stopped**, from a **transient** block (decode ≈ FILL_MAX×300×300×2 + line buffers + `JPEG_DECODE_OVERHEAD` ≈ **786 KB**; state the exact figure) that is freed afterwards. Nothing is built during playback.
   - Contract for A:
     ```c
     int artcache_panel_count(void);
     int artcache_panel_get(int i, struct bitmap *bm, void *buf, size_t size); /* 300x300 RGB565 */
     ```
2. **Sizing the precompute block:** read the entry count **synchronously at shell start** from the `database_lib.dat` header (or the master index header). With no database, reserve nothing and run stopped-only. The block is used **only by the tagcache thread** and re-fetched after every yield.
3. **Audio-thread stack:** `action_userabort()` (action.c:1248) dequeues the global button queue, so on non-UI threads it steals input. Under `HAVE_IPOD_UI`, **off the UI thread it returns false without reading the queue.**
   - List every non-UI caller (playlist.c:857, 1013, 2259, 2616, 3284).
   - Reachable only in developer mode (`next_folder` defaults off), but fix it anyway.
   - Re-run `scripts/risk/stack_usage.sh`.
4. **UBSan blocker:** now at `library.c:226`. A small separate simulator-clock file is allowed, since `<time.h>` clashes with the apps headers there.
5. **H1 checklist answers are required.** Item 10: the precompute and the deferred GC are tested in the **ready, fresh and dirty** states.

### #38 (A, B, C): corrections
- **Drift is pan-only (owner, 2026-10-07):** no zoom. **Two 300×300 slots** (current and next, **360,000 B**) are reserved at shell start.
  - **Prefetch** the next cover only while storage is awake. If it isn't ready at the end of the dwell, re-dwell on the same cover in a new direction.
  - The ~400 ms cross-fade blends straight from the two slots, in **≤8 steps**.
  - Pan steps are integer-pixel blits.
  - C scripts the storage-asleep case with `disk_active`.
- **Highlight cross-fade:** the old and new panels go into the strip (2 × 76,800 B).
- **Split and full-height pushes:** use a full-height strip capture (153,600 B ≤ 176,000), with a static assert.
- **Reveal:** define a layer contract, `draw_at(dx)` for the moving layers over a static backdrop. If that doesn't fit cleanly, ship `UI_T_REVEAL` as a push stub marked ⚠️. **No second framebuffer.**
- **Panel phase:** `preview.c` owns it, counted in **lit-time ticks** from one global start, plus a `SIMULATOR` setter for C's `drift_phase` command.
- **Panel scratch:** `ui_panel_scratch()`, with each provider declaring its size in a header.
- **After a push,** the panel shows nothing new until the rest delay has passed.
- **Transition API:** `ui_push(ops,arg)` == `ui_push_t(ops,arg,UI_T_PUSH)`.
- **The gate runs `scripts/risk/sim_ubsan.sh` on the #38 scripts.** New blits at odd x offsets with 32-bit loads are an alignment risk.
- **Checklist numbers:**
  - Memory: drift slots 360,000 + precompute (~0.6–1.0 MB) + panel transient 786 KB (stopped only).
  - CPU: an unboosted cross-fade is ~25–40 ms per frame at 30 MHz (estimate), so cap it at 8 steps.
  - LCD: a drift step is ~15–20 ms for the 160-wide panel, 5–8 times a second while lit.

## Revision 2 (owner request, 2026-10-07): review videos
- **C** adds a sim-only `record <ms> <fps> <name>` script command, which writes frames at a steady rate (~15–30 fps, deterministic: pinned clock, battery and drift phase), plus `scripts/make_video.sh`, which stitches them with ffmpeg into a small H.264 MP4 under `out/shots/`.
- **Required review artifacts for the owner round:**
  1. `drift-review.mp4`: the main menu resting on Music for ~20–25 s, showing **three covers** (7 s pan each plus the ~400 ms cross-fades);
  2. `transitions-review.mp4`: push and pop (split→split, split→full-screen), and the Clocks reveal or its stub.
  
  Keep them small (320×240, a few MB at most).

## Revision 3 (owner, 2026-10-07): global buttons (A: input/ui_main)
- **NEXT and PREV work from every screen:** a tap skips or restarts (the 3 s rule), a hold seeks. Audiobooks move by chapter. The current screen doesn't change.
- **Hold MENU jumps to the main menu** (pop to root, slide back).
- **Hold PLAY = sleep** (power off through the normal shutdown path).
- C adds scripts: a skip while browsing (the track changes and the screen stays put), hold MENU from three levels deep, and hold PLAY (shutdown is requested).

## Revision 4 (2026-10-07): localisation
Owner COMP-6: the language system stays. Every new user-visible string (Settings items, panel labels, button hints) goes through a `LANG_*` ID in `apps/lang/english.lang`. Agent B owns the `english.lang` additions this phase, and the others request IDs through the orchestrator. No hard-coded UI text.

## Revision 5 (2026-10-07): code audit 1 (`docs/code-audit-1.md`)
- **R1 drift slot:** the "next" drift cover lives in the list strip, which is idle while a panel is at rest. This replaces the second 300×300 slot and saves 176 KB permanently. Owner: A (preview.c). If the strip can't be pinned at that moment, fall back to a cross-fade without drift. Never allocate.
- **Agent A (list and animation framework):**
  - **F8:** hiding the scrollbar repaints only its column.
  - **F10:** after a glide, art fills blit only the art box.
  - **F2:** the marquee renders its text once and scrolls a blit.
  - **F4:** a slide's first frame composes boosted with a `memcpy` swap.
  - **F3:** the quick-scroll overlay uses cached letter and slab tables.
- **F9:** the album list's art poll keys on the art index commit sequence (A, with an `artcache_commit_seq()` getter added to `artcache.h` by A in coordination with the orchestrator).
- **F1** (owner approved 2026-10-07, guarded): the fast path for transparent text pixels in `firmware/drivers/lcd-16bit-common.c`, under `#ifdef HAVE_IPOD_UI` so stock `.text` stays byte-identical. Owner: A. The goldens must stay pixel-identical.
- The lean coding rules in `docs/process.md` apply. Self-reviews give instruction counts for every per-frame path they touch.

## Revision 6 (owner, 2026-10-07, before fan-out)
- **Panels:** the new panel **cross-fades** in once the selection rests. While the wheel moves, the **old panel stays** (no per-item redraw). Drop `UI_PANEL_SLIDE` from v1, or keep it in the enum unused.
- **NAV-3:** an opened album's track list is **single-line** (~9 rows).
- **NAV-1/2/4/5:** recommended defaults (see `docs/owner-questions.md`).
- **Scope tonight:** run to the owner round. Gate passed, end review fixed, review videos and screenshots ready. Do **not** merge to main.
- **Capacity panel (B):** the device name is **"Sam's iPod"** (owner). Read it from a stored string setting (default "iPod" for public builds; never hard-coded), and set it in this unit's config. The logo on the panel is the chosen **wheel** glyph (`docs/design/boot-logo.md`), scaled down, in the metallic treatment. Its "N GB Free" comes from the real FAT free space (cached, not recomputed per draw).

## Contracts (committed, 2026-10-07, branch `nav`)
Committed before fan-out (brief template rule 3). Later revisions to this section go to running agents as deltas.

### Files
| File | What | Owner after fan-out |
|---|---|---|
| `apps/ipodui/preview.h` | `struct ui_panel` (`struct viewport *vp`, `offscreen`), `struct ui_preview_ops` (`name`, `draw(p, item_arg, long phase)`, `rest_delay_ticks`, `arrive`), `enum ui_panel_arrive` (`XFADE`; `SLIDE` reserved, unused), `ui_preview_drift`, scheduler API (`ui_preview_reserve`, `ui_panel_select`, `ui_panel_select_screen`, `ui_panel_invalidate`, `ui_panel_hide`, `ui_panel_draw_now`, `ui_panel_tick`), `ui_panel_scratch()` + `UI_PANEL_SCRATCH_SIZE` (16,384) + `UI_PANEL_SCRATCH_CHECK()`, `ui_panel_phase()` + `ui_panel_phase_set()` (`SIMULATOR`), and the strip-borrow rules in its last comment | A |
| `apps/ipodui/nav_core.{c,h}` | Pure logic, no Rockbox headers: rest scheduling (`nav_sched_*`), lit-time phase (`nav_phase_*`), drift framing (`nav_drift_pos`, `nav_drift_wake`), drift strip plan (`nav_drift_plan`), in-place cross-fade (`nav_xfade_weight`, `nav_xfade_chan`), transition durations (`nav_transition_ticks`) | A |
| `apps/ipodui/screen_stack.{c,h}` | `enum ui_transition {UI_T_PUSH=0, UI_T_REVEAL=1, UI_T_SPLIT=2}`, `ui_push_t()`, `ui_push()` == `ui_push_t(…, UI_T_PUSH)`, `ui_transition_kind()` (a pop reports the popped screen's kind), `struct ui_screen.transition`, optional `ui_screen_ops.draw_into_panel(s, p)` | A |
| `apps/ipodui/menu.{c,h}` | `ui_menu_item` gains `preview`, `preview_arg`; main and Music items wired (D10); the Settings tree moved out (`ui_settings_menu`, D11) | A |
| `apps/ipodui/screens/panels.h` | B's providers `ui_preview_podcasts`, `_extras`, `_capacity`, `_settings_tile`; `ui_settings_menu`; `UI_PANEL_SCRATCH_<NAME>` sizes | B |
| `apps/ipodui/anim.h` | Strip sized for one 300×300 panel image (180,000 B, +4,000 B); owner ids `UI_STRIP_OWNER_PANEL` (-2), `UI_STRIP_OWNER_DRIFT` (-3) and `UI_STRIP_OWNER_NP` (-4, Phase 5) | A |
| `apps/ipodui/artcache.{h,c}` | `int artcache_commit_seq(void)`: returns the existing `commit_seq` (+1 per index commit). Six lines appended at the end of `artcache.c`, nothing else touched | A wires the F9 poller; H1's agent keeps the rest of `artcache.c` |
| `apps/ipodui/nav_stub.c` | The one shared stub. SECTION A: no-op scheduler, `ui_preview_drift` draws plain `panel_bg`. SECTION B: providers draw plain `panel_bg` and return `TIMEOUT_BLOCK`; `ui_settings_menu` = the Phase 3 stub list, unchanged. **A deletes SECTION A in the commit that adds `preview.c`; B deletes SECTION B in the commit that adds `panels.c`/`settings.c`.** The orchestrator deletes the file and its `SOURCES` line when both are gone | A / B (own section only) |
| `apps/ipodui/screens/nowplaying.{h,c}` | Phase 5 hooks (`docs/phase5-brief.md` §4): `ui_nowplaying_screen` and `int ui_nowplaying_idle(void)`. The `.c` is the shared stub: `enter()` runs today's `gui_wps_show()` path and returns -1; `idle()` returns `TIMEOUT_BLOCK` | **Phase 5** (not A, B or C) |
| `apps/SOURCES` | `ipodui/nav_core.c`, `ipodui/nav_stub.c`, `ipodui/screens/nowplaying.c` in the `HAVE_IPOD_UI` block. A adds `preview.c`, B `screens/panels.c` and `screens/settings.c` (additions only) | orchestrator merges |
| `tests/nav/{run.sh,test_nav.c}` | Round-trip host test, picked up by `scripts/test.sh` (`tests/*/run.sh`): scheduling, phase, drift bounds and wakes, the strip plan table, fade weights (exact end, monotonic, ≤ 1.25 levels off linear for n = 1..8), the transition kind round-trip and durations | C (A extends it when it changes `nav_core.c`) |

Behaviour on `nav` is unchanged: the stub panel draws what `menu.c` drew before (plain `panel_bg`), and Settings is the same list.

### Decisions (D1–D15; ⚠️ = default until the owner round)
- **D1 Panel key.** A panel is `(provider, arg)`. The same key means nothing to do, so moving between Music items never interrupts the drift. A `NULL` preview keeps the old panel. Every selection move and every push or pop onto a split screen restarts the rest delay (`nav_sched_select`). A full repaint draws the wanted panel at once, without a fade (`nav_sched_forget`).
- **D2 Arrival.** Cross-fade only (Rev 6), ≈200 ms (spec), `NAV_PANEL_XFADE_STEPS` = 6 frames, **boosted** (a response to input, lean rule 6). The drift's cover fade is 400 ms, 8 steps, **unboosted**.
- **D3 In-place fades.** A fade blends the LCD framebuffer's panel toward the target. Step *k* of *n* moves each channel by 1/(n−k+1) of what's left (`nav_xfade_weight`, `nav_xfade_chan`), and the last step is exact. No copy of the old picture is kept, so a highlight fade needs only the new panel in the strip (76,800 B, not 2 × 76,800 as Rev 1 said), and a drift fade needs only the next cover. A dropped frame still converges.
- **D4 Drift (pan only).** Cycle 7 s = 400 ms fade + 6.6 s pan at 1 px per 150 ms (6.7 px/s, 44 px per dwell), in a direction from an 8-entry table (cycle 0 left, 1 right+down, 2 up, as seen). The window is centred in the 300×300 source and stays inside it (tested on every tick of 16 cycles). The phase counts lit ticks only and is never reset.
- **D5 Drift strip contract (Rev 5 R1).** The current cover lives in the **drift slot** (180,000 B, reserved by `ui_preview_reserve()` at shell start). The **next** cover is prefetched into the strip (owner `UI_STRIP_OWNER_DRIFT`) only when `nav_drift_plan` says `PREFETCH`: audio playing, storage already awake, panel at rest, ≥ 2 s into the dwell. The strip stays pinned across that one 180,000 B read, the only pin held across I/O; the UI thread is the strip's only user. (End review: pins are also held across `slot_read()` (the drift slot, during the cover read), `mq_step()`'s glyph loads (the strip, while the marquee renders) and `hl_start()`'s provider draw (the strip and the panel scratch). Harmless while nothing on those paths allocates; the sim's "paneldraw" trace rule checks draws during playback for allocations.) At the cycle end the drift reclaims the strip:
  - `kept` → `FADE_STRIP`, then `memcpy` into the slot.
  - Not kept, and storage may be read (stopped, or awake) → `FADE_DIRECT`: read straight into the slot, since the old cover is still on the LCD.
  - Otherwise → `REDWELL`: fade to the same cover at the new cycle's framing.

  One cover or none means `REDWELL`. When stopped, no prefetch is made and the cycle end reads directly. Not at rest → drop frames; a late cycle change is still a fade, never a cut. **Nothing allocates.** This replaces Rev 1's "two 300×300 slots".
- **D6 Reveal ⚠️ stubbed as a push.** `UI_T_REVEAL` exists, is recorded and is mirrored on pop, but `ui_main.c` runs it as `UI_T_PUSH` in v1. Reasons:
  - A `draw_at(dx)` layer contract either re-renders text every frame (lean rule 3) or needs two full-body captures with transparency (2 × 140,800 B > the strip; "no second framebuffer").
  - Its only caller (Extras › Clocks) is itself a stub this phase.

  The leaf hook `draw_into_panel` is in place, so a later reveal can start from the panel picture without a jump. `UI_T_SPLIT` is likewise a push stub until Phase 7.
- **D7 Durations** (`nav_transition_ticks`): push and pop 190 ms; split → full push 250 ms, its pop 290 ms; reveal 250 ms both ways; split 210 ms in, 290 ms out.
- **D8 Leaf panels.** A split leaf screen (Settings › Backlight) sets `ui_screen_ops.draw_into_panel` and calls `ui_panel_select_screen(s)` on enter, and `ui_panel_invalidate()` when its value changes. The rest delay is `NAV_REST_ICON`, and the draw rules are the same as a provider's. B may instead build a leaf as a split `ui_menu` with per-item previews: B's choice.
- **D9 Panel scratch:** 16,384 B, one movable block reserved with the drift slot, valid only inside `draw()`. It can't share with the strip (which holds the panel being drawn) or with the drift slot (whose current cover keeps drifting). Each provider defines `UI_PANEL_SCRATCH_<NAME>` and checks it with `UI_PANEL_SCRATCH_CHECK()`. Raising the size is A's call, with a budget line.
- **D10 Item wiring** (in `menu.c` now):
  - Music, Shuffle Songs, Now Playing and every Music-menu item → `ui_preview_drift`. Shuffle Songs and Now Playing as art panels are ⚠️ (spec: "Music and the other art panels"; Now Playing could later show the current track's cover).
  - Podcasts → `ui_preview_podcasts`.
  - Extras → `ui_preview_extras`.
  - Settings → `ui_preview_capacity`.
- **D11 Settings tree ownership.** `ui_settings_menu` (`screens/panels.h`) belongs to B. `menu.c` no longer defines the Settings list. The Phase 3 stub list sits in `nav_stub.c` SECTION B until B's `settings.c` replaces it, so C's p3 scripts keep working until they're retargeted.
- **D13 Capacity panel (owner delta, Rev 6):** `ui_preview_capacity` draws the device name from a stored string setting (default "iPod", never hard-coded; "Sam's iPod" set in this unit's config), the wheel glyph scaled down, and "N GB Free" from a cached FAT free-space figure (never computed per draw, no storage wake from `draw()`). Owner: B. The contract signature is unchanged (arg `NULL`).
- **D14 Phase 5 hooks (NAV-H1–H3, from `docs/phase5-brief.md` §4 and Rev 1 items 5 and 9):**
  - **NAV-H1:** `ui_show_wps()` pushes `ui_nowplaying_screen` once audio plays or a resume succeeds. If the push is refused (`enter() < 0`, as the stub always does, or a full stack), the screen below repaints in full, exactly as before.
  - **NAV-H2:** the main loop calls `ui_nowplaying_idle()` once per pass, on the UI thread, and folds it into the timeout, including while animating (at least 1 tick).
  - **NAV-H3:** `artcache_commit_seq()` is real on `nav` (it returns the existing commit counter).
- **D15 Strip exclusivity:** the panel and drift code claim the strip (`PANEL`/`DRIFT`) **only while a split screen is on top**. Once `ui_panel_hide()` is called, they claim nothing, and `ui_panel_tick()` isn't called. While Now Playing (or any full-screen screen) is on top, the strip is Now Playing's (`UI_STRIP_OWNER_NP`, fixed offsets). The drift phase keeps counting, and the first cycle change after the return is a fade. `preview.c` traces every claim as `panel strip claim owner=<PANEL|DRIFT> top=<screen name>`. **C adds a trace rule:** any such line whose `top` isn't a split screen (`menu` of a split menu, or a split leaf) fails, and no such line may appear between a `push nowplaying` and its pop.
- **D12 Strip size** +4,000 B (176,000 → 180,000) to hold a panel image. The row-slot counts of every list style are unchanged (11, 6 and 5 slots). The orchestrator updates the `hardware.md` budget line at close.

### Per-agent read list (≤3 each)
- **A (framework):**
  1. This section, plus Revisions 1, 3, 5 and 6 above.
  2. `apps/ipodui/preview.h` (it points into `nav_core.h` and `screen_stack.h` as needed).
  3. `docs/design/spec/main-menu.md`, the "split screen" bullets (rest, arrival, drift), and `docs/design/spec/index.md` → "Transitions".
- **B (panels and Settings):**
  1. This section, plus Revision 4 (`english.lang`) and Revision 6's "Capacity panel" bullet.
  2. `apps/ipodui/screens/panels.h`, with the "providers" and "panel scratch" parts of `apps/ipodui/preview.h`.
  3. `docs/design/spec/settings.md` → "Split screen" (it cites catalogue §7.1).
- **C (verification):**
  1. This section, plus "C: verification" and Revisions 2 and 3.
  2. `apps/ipodui/preview.h`: `ui_panel_phase_set()` for `drift_phase`, and the strip-borrow and exclusivity comments, which the trace rules check (D15).
  3. `tests/ui/README.md`.

### Final ownership (supersedes the tables above where they differ)
| Agent | Owns | Must not edit |
|---|---|---|
| **A: framework** | `screen_stack.*`, `anim.*`, `ui_main.c`, `input.*`, `header.*`, `preview.{c,h}`, `nav_core.{c,h}`, `menu.{c,h}`, `list.h` and the list's F2/F8/F10 internals, `qscroll.*` (F3), `rowart.*` (the F9 poller on `artcache_commit_seq()`), `nav_stub.c` SECTION A; **F1**: the guarded text-blend fast path in `firmware/drivers/lcd-16bit-common.c` (`#ifdef HAVE_IPOD_UI`, stock `.text` byte-identical, goldens pixel-identical) | `paint.*`, `theme.*`, `screens/*`, `english.lang`, sim tooling, `artcache.c` beyond `artcache_commit_seq` |
| **B: panels and Settings** | `screens/panels.{c,h}`, `screens/settings.c`, `paint.*`, `theme.*`, `art/*.py`, `nav_stub.c` SECTION B, `list.c` blue inline values (only the value-drawing hook; coordinate any other `list.c` hunk with A through the orchestrator), **`apps/lang/english.lang` additions** (Rev 4; A and C request IDs through the orchestrator); the capacity panel's **device-name string setting** (Rev 6: default "iPod", this unit's config sets "Sam's iPod"), as a minimal `#ifdef HAVE_IPOD_UI` hook in `apps/settings.h` / `apps/settings_list.c` if no shell-side store fits, plus the cached FAT free-space figure | `screen_stack.*`, `anim.*`, `preview.*`, `nav_core.*`, `menu.*`, `artcache.*`, sim tooling |
| **C: verification** | `tests/ui/*`, `tests/nav/*`, `scripts/*`, `uisimulator/*` (the `drift_phase`, `record` and `disk_active` commands; trace rules in `check_trace.py`) | `apps/` |
| **Phase 5 (Now Playing)** | `screens/nowplaying.{h,c}` (it replaces the stub). The call sites in `ui_main.c` (`ui_show_wps`, the `ui_nowplaying_idle` fold) stay A's | everything else on `nav` |
| **H1 / playback-audiobook** | `artcache.c` (all but the appended `artcache_commit_seq`), `library.*`, `tagcache.c` | none |
| **orchestrator** | `apps/SOURCES` merges, `docs/hardware.md` budget table, deleting `nav_stub.c` | none |

Contracts-step cost (on `nav`, fw-ui): `nav_core.o` 1,012 B and `nav_stub.o` 1,470 B of `.text`, no `.data`/`.bss`; the strip +4,000 B (heap, shell start).
