# Code efficiency audit 1: the iPod UI shell (`apps/ipodui/**`) on `main`

Date: 2026-10-07. Auditor: firmware-platform agent, read-only. Base: `main` at `975b3d7d38`.
Scope: CPU (battery), RAM (audio buffer), binary size and storage wake-ups of the shell, plus its guarded
hooks in `apps/tagcache.c`. The H1 agent is editing `tagcache.c`, `library.c`, `artcache.c`, action
and playlist code in its own worktree (`h1`). Findings that touch those files are tagged **[H1 overlap]**.

Nothing in the repo was changed for this audit. All builds ran in the session scratchpad:
- `fwui`: `--target=22 --type=n --ipodui`.
- `fwthumb`: the same plus `--thumb`.
- `fwpatch`: `fwui` with one experimental driver change (§2.1).

## 0. Method

- **Size:** `arm-none-eabi-size` and `nm --size-sort` on `fwui` objects. Per-file compiler variants were made by compiling every
  `apps/ipodui/**/*.c` with the exact `fwui` flags, plus `-O2`, `-mthumb -mthumb-interwork` or `-mthumb-interwork`.
- **Instructions (new tool):** a Unicorn harness (Appendix A).
  - It loads the real `fwui` `rockbox.elf`: `.text`, `.rodata` and `.data` at their link addresses, and `.iram` at `0x40006aa4`.
  - It calls real functions (`ui_paint_selection`, `lcd_putsxy`, `ui_qscroll_draw`, `ui_header_draw`, `body_xfer`, ...) with real arguments and
    counts every executed instruction. `lcd_set_viewport(NULL)` initialises the default framebuffer.
  - The shell's 4-bit fonts (`apps/ipodui/fonts/*.fnt`) are parsed and installed in place of `sysfont`, so text runs through the real
    `lcd_putsxyofs` → `lcd_alpha_bitmap_part_mix` path.
  - It uses the same CPU-model substitution as `scripts/risk/` (arm926 for ARM7TDMI).
  - **Not modelled:** cache misses, SDRAM wait states and the LCD bus. Time is quoted as instructions × CPI 1.2–2.0 / clock, the
    basis from `docs/risk-tooling.md`. At 30 MHz, 100k instructions take **4.0–6.7 ms**; at 80 MHz, **1.5–2.5 ms**.
- **LCD push time** comes from the model in `docs/research/lcd-fps-ipodvideo.md` (LOW-MEDIUM confidence), about 0.25 µs per pixel at
  30 MHz. Two examples: a full-width 24-row band is about 2 ms, and the 320×220 body is 13–20 ms (30 MHz) or 10–13 ms (80 MHz).
  It is **not measured**; this is a P10 device item.

## 1. Measured primitive costs (fw-ui, ARM, -Os)

| Primitive | Instructions | Note |
|---|---:|---|
| `lcd_fillrect` 320×24 (solid) | 2,036 | `memset16` in IRAM: the floor for a row background |
| `ui_paint_selection` 320×24 / 320×44 / 320×55 | 7,430 / 13,793 / 17,297 | ~232 per row for `ui_gloss_color()` (2 divides, 2 blends), recomputed on every draw |
| `ui_paint_chevron` on the bar / on white | 14,630 / 12,642 | per-pixel `lcd_set_foreground` + `lcd_drawpixel` + `ui_paint_blend` (3 divides by 15) + a gloss row per row |
| `lcd_putsxy` Bold 15, "Music" (5 ch) | 33,941 | about 6,900 per glyph, ~30 per pixel |
| `lcd_putsxy` Bold 15, "Bohemian Rhapsody" (17 ch, 161 px) | 117,824 | 3,703 cell pixels; 83% of the time is in `lcd_alpha_bitmap_part_mix` |
| `lcd_putsxy` Bold 15, 38 ch (311 px) | 232,793 | |
| `lcd_putsxy` Regular 12, 21-char sublabel | 79,809 | |
| `font_getstringsize` 17 / 38 ch | 1,341 / 2,853 | measuring is cheap; rendering is not |
| marquee step, text only (two `putsxy` in a 236 px viewport, 38 ch) | 168,764 avg | |
| `lcd_bitmap_part` 55×55 thumb / 320×24 slot / 320×220 window | 4,814 / 2,850 / 25,390 | `memcpy` per row, IRAM: cheap |
| `ui_qscroll_draw`, letter "M" / "#" | **377,643** / 192,621 | per glide frame while the overlay shows: the slab blend ~185k plus a bilinear letter scale ~185k |
| `body_xfer(SWAP)` (slide start) / `TO_STRIP` | **429,480** / 28,640 | the per-pixel C swap is 15× a `memcpy` |
| `ui_header_draw` full / play↔pause / unchanged | 90,346 / 74,824 / 120 | per-pixel glyph blends |
| `ui_rowart_dot` (selected) | 11,964 | per-pixel |

**Composite estimates** (sums of the measured primitives):

| Draw | Instructions | Time |
|---|---:|---|
| Single-line row, unselected / selected (17-char label) | ~120k / ~141k | |
| Album row (55 px: thumb + Bold 15 label + Regular 12 sublabel), unselected / selected | ~210k / ~240k | |
| Full 9-row list | ~1.1M | 44–73 ms at 30 MHz |
| Glide frame | ~230k (blit + one new row) to ~610k (with the quick-scroll overlay) | plus the 320×220 push |

**Text dominates every shell draw.** Glyph cells are mostly empty:
- 73% of Bold-15 cell pixels are fully transparent and 9% fully opaque.
- 56% of glyph rows are blank.

The renderer still runs the full blend on every one of those pixels.

## 2. Findings

Columns: **where** · **what's wasteful** · **cost** (m = measured, e = estimated) · **leaner approach** · **saving** · **fidelity risk** · **effort**.

### 2.1 Hot pixel loops

**F1. Text blend has no transparent fast path.**
- **Where:** `firmware/drivers/lcd-16bit-common.c:625-633` (the `DRMODE_FG` case of `lcd_alpha_bitmap_part_mix`, used by every shell label). A shared driver.
- **What's wasteful:** each pixel costs ~25 instructions, including the 73% where alpha = "background". `blend_two_colors` is also not inlined at -Os: the IRAM loop calls it in DRAM through `__blend_two_colors_veneer`.
- **Change tested in `fwpatch`:**
  ```c
  unsigned a = READ_ALPHA();
  if (a != ALPHA_MASK)
      *dst = a ? blend_two_colors(*dst, fg, a) : fg;
  ```
  This is exact. With a = 15 the weight of `*dst` is 16/16, so the pixel is unchanged; with a = 0 the result is `fg`.
- **Saving (m):** text −34%.
  - "Bohemian Rhapsody": 117,824 → 77,391.
  - 38-char label: 232,793 → 153,418.
  - Marquee text step: 168,764 → 115,037.
  - Header repaint: 90,346 → 81,972.
- **IRAM cost (m):** +16 B.
- **A second variant was worse:** skipping 0xFF alpha bytes plus an `always_inline` blend measured +5% and +432 B IRAM, because glyph rows start on odd nibbles.
- **Fidelity risk:** none (pixel-identical).
- **Effort:** S.
- **Caveat:** stock `.text` must stay byte-identical (checklist item 11), so either guard it with `HAVE_IPOD_UI` or accept it as an upstreamable change (orchestrator's call).

**F2. Marquee re-renders the whole selected row at 20 Hz.**
- **Where:** `apps/ipodui/list.c:817-829` → `redraw_selected()` (`list.c:723`) → `draw_row()`.
- **What's wasteful:** every 5 ticks it repaints the bar, thumb, sublabel and chevron, renders the label twice, and pushes the full row.
- **Cost (m + e):**
  - Single-line row: ~193k instructions per step → **15–26% CPU at 30 MHz**, plus a 320×24 push (~2 ms).
  - Album row: ~288k per step → **23–38% CPU**, plus a 320×55 push (~4.5 ms).
  - It runs whenever a long label is selected with the backlight on. It parks when dark, which is good.
- **Leaner:**
  1. When the marquee starts, render the label (plus its 40 px gap and a second copy) once over the bar's row colours into an offscreen buffer: (311+40+236) × 23 × 2 B ≈ 27 KB. It fits in the strip, which is idle at rest; claiming it only invalidates glide slots, which re-render on the next glide anyway.
  2. Each step then `lcd_bitmap_part`s the 236 px window and pushes only the text cell's rows (full width, ~23 rows).
- **Saving (e):** about 95% of marquee CPU (~5k instructions per step) and 55 → 23 rows pushed for album rows.
- **Fidelity risk:** none.
- **Effort:** M.

**F3. Quick-scroll overlay recomputed per glide frame.**
- **Where:** `apps/ipodui/qscroll.c:187-214` (`draw_slab`) and `232-270` (`draw_letter`).
- **What's wasteful:**
  - It blends 7,056 pixels per frame with `blend565` and recomputes `ui_gloss_color` and `isqrt` per row.
  - It bilinear-scales the letter from the 26 px cut with 4 bounds-checked `glyph_px()` calls per pixel.
- **Cost (m):** 377,643 per frame, 33 fps during fast spins → 9–15 ms at 80 MHz, on top of the push.
- **Leaner:**
  - Build the scaled letter's alpha once per letter change: about 35×30 B, from the strip's tail or a 1 KB block in the rowart header.
  - Precompute per-row slab colour, inset and edge alpha once at load: 84 × 4 B.
  - Blend as `slab_row_pre + (bg × 3/32)` using the packed `0x07e0f81f` form (~10 instructions per pixel instead of ~26).
- **Saving (e):** 378k → ~80–100k per frame.
- **Fidelity risk:** none if the same formulas are tabulated; slab rounding must match the existing `blend565` (keep its +16 rounding).
- **Effort:** M.

**F4. Slide start composes unboosted and swaps per pixel.**
- **Where:** `apps/ipodui/ui_main.c:196-221` (`body_xfer` SWAP loop) and `225-259` (`slide_start`).
- **What's wasteful:** the main loop drops the boost before input (`ui_main.c:424`), so the old-body capture, the full new-screen draw (~1.0–1.1M) and the SWAP (429k) all run at 30 MHz. That is **60–100 ms before the first slide frame**, after `lib_open`.
- **Leaner:**
  - Swap row by row via a 640 B stack row and 3 `memcpy`s (IRAM asm): ~86k instructions.
  - `cpu_boost` from `slide_start` (the animation boosts on its first frame anyway; start the hold here instead).
- **Saving (e):** −340k instructions per push/pop; first-frame latency ~60–100 → ~15–25 ms.
- **Fidelity risk:** none (snappier).
- **Effort:** S.

**F5. Chevron, header glyphs and the unplayed dot drawn per pixel.**
- **Where:**
  - `paint.c:133-154` (chevron).
  - `header.c:202-212` `glyph_px()`, used by `draw_play`, `draw_lock` and `draw_battery`.
  - `rowart.c:360-379` (dot).
- **What's wasteful:** for every ink pixel it calls `lcd_set_foreground` + `lcd_drawpixel` (clip per pixel) + `ui_paint_blend` (3 integer divides; ARM7 has no divider). The backgrounds are fixed: the header row table, `list_bg` or the selection gloss.
- **Cost (m):** chevron 14.6k per selected-row draw; header 75–90k per push, pop or play-state change; dot 12k.
- **Leaner:** pre-render tiles once.
  - Chevron per (row height, selected): 8×13×2 = 208 B each, ≤ 6 tiles.
  - Play, pause, lock and battery over the header rows: about 1.3 KB.
  - Then `lcd_bitmap_part` them.
- **Saving (e):** chevron 14.6k → ~1.5k; header 90k → ~15k.
- **Fidelity risk:** none (same formula, computed once).
- **Effort:** S–M.
- **Where the tiles live:** in the rowart block (movable), not as statics; the >1 KB rule applies.

**F6. Gloss colours recomputed per row and draw.**
- **Where:** `paint.c:75-121`.
- **Cost (m):** ~232 instructions per row: a 24-row bar is 7.4k vs a 2.0k fill, and 55 rows 17.3k vs ~4.6k.
- **Leaner:** a per-(gloss, extent) row table (`u16[55]`), cached for the 3–4 extents in use.
- **Saving:** ~5–13k per selected row (2–5% of a row).
- **Fidelity risk:** none.
- **Effort:** S. Low value alone; do it with F5.

**F7. `memcpy` vs word copies.**
- All bulk copies already go through IRAM `memcpy`, `memmove` and `memset16` (`lcd_bitmap_part`, `slide_shift`, `lcd_hline`). The exceptions are F4's swap loop and F3's per-pixel C loops.
- Strip, slot and framebuffer rows are 640 B, 4-byte aligned. No unaligned access found.

### 2.2 IRAM

Core IRAM is 45,808 / 49,152 B (3,344 free). The shell's hot loops are **already in IRAM**:
- `lcd_alpha_bitmap_part_mix` (text);
- `lcd_bitmap_part` (blits, glide window);
- `memcpy`, `memset16` and `memmove`;
- `lcd_write_data` (push).

What remains in DRAM is shell logic, the `blend_two_colors` callee (via a veneer) and the qscroll and glyph per-pixel loops. Those should be fixed algorithmically (F3, F5), not moved; PP5022 runs DRAM code through its cache, and tight loops stay resident.

**Recommendation:**
- Keep the rule: **no shell code in IRAM**; codecs keep it (FLAC and APE are at 81.0 / 81.1 of 81.9 KB in their own window).
- The only IRAM change worth making is F1's +16 B.
- Optionally make `blend_two_colors` `ICODE_ATTR` instead of `always_inline` (~60 B, removes the IRAM→DRAM veneer). The gain is cache-dependent and only a device benchmark can show it.

### 2.3 Compiler options (measured: sum of `apps/ipodui/**` `.text`)

| Variant | Shell text | Δ |
|---|---:|---:|
| `-Os` ARM (current) | 65,820 | – |
| `-O2` ARM | 79,656 | +13,836 (+21%) |
| `-Os -mthumb -mthumb-interwork` | 46,425 | **−19,395 (−29.5%)** |
| `-Os -mthumb-interwork` (ARM objects) | 65,820 | 0 (interwork costs nothing at -Os here) |
| Mixed: Thumb except `library`, `sortkey`, `artfmt` (CPU-heavy) | ~54,700 (e) | −11.1 KB |

**Whole firmware `--thumb`** (`fwthumb`, measured):

| | ARM | Thumb | Δ |
|---|---:|---:|---:|
| `.text` | 678,904 | 516,408 | **−162,496 (−23.9%)** |
| `rockbox.ipod` | 1,225,304 | 1,060,000 | −165,304 (−13.5%) |

Resident DRAM shrinks by the same amount, which goes back to the audio buffer. IRAM is unchanged. ARM7TDMI Thumb is typically 10–30% slower on compute.

**Recommendations:**
- **-O2:** no, not for the shell. Its pixel work is in the IRAM driver.
- **Thumb (P9, after the removal list):** per-directory Thumb for cold code (settings, menus, gui, metadata parsers run at sync, USB, debug, the shell's UI files). Keep ARM for `firmware/` drivers, playback, buffering, tagcache, `library.c` and `sortkey.c`. Measure boost ratio and `lib50k` on the device first.
- **`-ffunction-sections`/`--gc-sections`:** already P9(a).
- **LTO:** not before P9 lands. Its interaction with `ICODE_ATTR`/`INIT_ATTR` section placement and the `.init`-in-codec-slot trick is untested; a scratch experiment only.

### 2.4 Redundant work and wake-ups

**F8. Scrollbar hide redraws the whole list.**
- **Where:** `list.c:786-797`, which calls `draw_band(l, l->vp.y, view_h(l))`.
- **What's wasteful:** to erase a 4 px column it re-renders all rows (with `fetch=true`, which may do art I/O). That is ~1.1M instructions (44–73 ms at 30 MHz) once per scroll session, 1 s after the motion stops.
- **Leaner:** repaint only the column (x ≥ `vp.width` − 4): each row's background (`list_bg`, the gloss row colour on the selection, the separator pixel), then push the sbar rect as now. Check first that `draw_extra` (book progress hint) never reaches those columns.
- **Saving:** ~99% of that redraw.
- **Fidelity risk:** none if the column painter matches `draw_row`; one golden-diff check.
- **Effort:** S.

**F9. The art poll forces a full list redraw each second during a build, and finds nothing new.**
- **Where:** `screens/browse.c:445-453` → `rowart.c:324-346`.
- **What's wasteful:** `ui_rowart_poll()` returns true whenever `status.done` changed and any PENDING key is remembered. But the reader only sees new art after the builder **commits** the index: `artcache.c:260-271` `reader_sync` re-opens on `commit_seq`, which happens every 32 albums or 30 s. So most redraws re-fetch PENDING and draw the same glyphs.
- **Cost:** ~0.9M instructions + up to 4 index lookups + a full 320×220 push about every 1–2 s, while an Albums list is lit during a build. That is 4–6% CPU plus the push.
- **Leaner:** put `commit_seq` in `artcache_build_status` and poll on it.
- **Saving:** about 97% of these redraws.
- **Fidelity risk:** none (art appears at the same time it can now).
- **Effort:** S. **[H1 overlap: artcache.c]**

**F10. `fill_art` re-renders whole rows to swap in a thumbnail.**
- **Where:** `list.c:600-619`.
- **What's wasteful:** after a glide, each stub row is fully redrawn (~210–240k).
- **Leaner:** fetch and blit only the art box (`draw_thumb` into the row's x 0..54), then push the row band. Nothing else overlaps x < 55: the bar starts at `st.thumb` and qscroll is centred.
- **Saving:** ~95% per filled row; up to ~0.9M instructions after each glide on art lists.
- **Fidelity risk:** none.
- **Effort:** S.

**F11. Measuring and label fetch.** `put_fit` measures each label per draw (1.3–2.9k), and `marquee_reset` fetches and measures the selected label on every move. That is ≤ 2% of a row draw: **leave it.** The label caches (`library.c` 24 × 80 B, sublabel 24 × 48 B) work.

**F12. Wake-ups per second at rest.**
- Below, "+" means in addition to the base rate. The kernel's own 100 Hz tick dwarfs every shell timer.
- All rates come from the timeouts in `ui_main.c:50-52`, `list.c:28-33`, `header.c:547-549`, `menu.c:232/335` and `browse.c:68,500`.

| State | Lit | Dark |
|---|---:|---:|
| Any screen, stopped (header hold/battery poll) | 1/s | 0.2/s |
| Playing (play-state glyph) | 2/s | 0.2/s |
| + marquee (selected label overflows) | +20/s, each a full row redraw (F2) | 0 (parked) |
| + Time in Title on | ~0.33/s swaps, + a 30 ms fade frame | 0 |
| + Music menu "Updating Library..." spinner | +8/s | +1/s (could be 0.2/s) |
| + browse view not ready | +1/s | +1/s |
| + art list during a build | redraw ~1/s (F9) | rides the wakes |
| One-shots | sbar hide 1 s after motion; qscroll hide 0.5 s after; confirm flash 1.2 s | – |

- The dark-state timers (0.2/s idle, 1/s spinner, 1/s not-ready) could block until backlight-on.
- **Saving:** under 2 wakes/s. That is negligible against the kernel tick: **later/optional**.
- The real CPU cost while lit is F2 and F9, not the wake count.

**F13. `lib_open` boosts while doing file I/O.**
- **Where:** `library.c:1478-1483`.
- **What's wasteful:** it boosts for every open with n > 1000. But unfiltered opens read `database_lib.dat`: CPU 71–173 µs in the sim, the rest is I/O (8–196 KB).
- **Leaner:** boost only when falling back to the scan.
- **Saving:** 80 MHz held for ~30–70 ms per list open avoided.
- **Fidelity risk:** none.
- **Effort:** S. **[H1 overlap]**

### 2.5 RAM: reservations and time-multiplexing

Reserved at shell start: strip 176,000 + rowart 109,428 + art pinned 43,608 + art decode 86,204 + art reader 8,256 + fonts ~102,100 + library pool (12 B/entry + 64 KB ≈ 544 KB at 40k) + precompute (H1, ~0.6–1.0 MB). The drift slots (#38, 360,000) are next. That is **≈ 2.3–2.5 MB** of shell reservations at 40k, excluding the ~5.6 MB tagcache RAM cache.

| # | Idea | Saving | Risk | Effort | Phase |
|---|---|---:|---|---|---|
| R1 | **Drift "next" slot in the strip.** The next cover is only needed for the ~400 ms cross-fade, at rest, when the strip holds only expendable glide slots. Size the strip to one 300×300 slot (180,000 B, +4,000) and keep one dedicated drift slot. A navigation mid-fade cancels the fade (the strip claim fails, `kept=false`), exactly as slides already handle it. | **−176 KB** | none | S (design) | #38 |
| R2 | **One background arena.** The precompute block (tagcache thread) and the art builder decode (86 KB) + work (28 KB) blocks are both background, run on different threads and rarely overlap (precompute after a load or deleted-file pass; art builder in fill windows). Share them through a token or mutex; the loser waits. The stopped-only panel transient (786 KB) can also borrow it instead of a fresh `core_alloc`. | **−115 KB** permanent; avoids the 786 KB transient churn | low | M | H1 follow-up / #38 |
| R3 | Library pool: size filtered-view scratch from precomputed group counts (artists' and albums' `X_CNT`) instead of 12 B × every entry. Unfiltered views already come from the file. | −300–400 KB at 40k | medium (fallback scan still needs full size: transient, stopped-only) | L | later **[H1]** |
| R4 | `language_buffer` sized to the loaded `.lng` (now sized for the largest language). Owner COMP-6: keep the lang backend and localisation; this only right-sizes it. | −18.8 KB for English (34,216 → ~15.4 KB) | none | S | P9 |
| R5 | Rowart LRU 16 → 8 thumbs (4-row lists keep 2 screens) | −48 KB | more thumb re-reads on back-scroll (at rest) | S | later (owner) |
| R6 | Fonts | 0 | – | – | **Leave as is:** every cell row is used by some glyph (Bold 15 ink rows 1–22 of 23 across 536 glyphs), and Rockbox renders only 1- or 4-bit, so 2-bit would be a new format with an anti-aliasing quality risk. Subsetting to Latin-1 breaks Latin-Ext artist names; the letters font is already A–Z. |

**Doc inconsistency:**
- `docs/hardware.md`'s budget row "Offscreen list strip: 160,000 B (10 × 25 px)" is stale.
- The actual size is 176,000 (`anim.h:75`, 5 × 55 × 320 × 2), as the Phase 4b table says.
- This is the orchestrator's table to fix.

### 2.6 Storage: reads per user action

| Action | Reads | Note |
|---|---|---|
| Open an unfiltered list (Artists, Albums, Songs, ...) | 1 open + header (544 B) + 1–3 sections (`library.c:1268-1335`) | Albums at 4k: ~64 KB; Songs at 40k: ~160 KB. Re-read on **every** open, including MENU-back-then-reopen |
| Open an artist's albums (filtered) | the header + a binary search of the album table **per row**, ~12 seeks + 24 B reads each (`library.c:1385-1440`) | 10 albums ≈ 120 small reads. The disk cache absorbs repeats, but each miss is an ATA wake while playing |
| Album list at rest | ≤ 1 index page + 1 thumb read (6 KB) per newly shown row; none while gliding | `artcache_state` then `artcache_get` = 2 lookups; the 2nd hits the cached page |
| Glide, marquee, header, drift | 0 | |

- **S1 [H1].** An artist's albums have consecutive representatives (folder order), so find the first row's representative once and read the covering range of the table in one `read()`. Fallback: bisection per miss. 120 → ~13 reads. Effort S–M.
- **S2 [H1, later].** Keep the last-closed unfiltered view per category in the pool's free space (the pool has 64 KB headroom) and the libfile header and section table in RAM per commit. Re-opening then reads nothing: one fewer flash wake per back-and-forth while playing. Effort M.

### 2.7 Code size and dead code

- **The shell is 65,820 B text, 388 data, 1,196 bss** (`fwui`). The shell build is +70,772 B text over stock (shell +65.8 KB, `tagcache.o` +6.0 KB).
- **Largest objects:** `library.o` 19,204, `artcache.o` 8,973, `list.o` 6,247, `artfmt.o` 4,079, `header.o` 3,956, `browse.o` 3,867, `rowart.o` 3,650 and `menu.o` 3,470.
- **Largest functions:** `art_thread` 3,244; `lib_precompute` 2,836; `build` 2,652; `ui_header_draw` 2,344 (GCC inlined the glyph code); `rows_build` 1,628; `ui_list_tick` 1,564; `ui_qscroll_draw` 1,520; `progress_compute` 1,452; `draw_row` 1,152.
- **Dead code:** about 1 KB of externally unreferenced globals: `artcache_get_handle`, `artcache_key_for_id3`, `artidx_get`, `artidx_hdr_ok`, `artidx_hdr_seal`, `artkey_norm`, `artcache_dim`, `artcache_build_pause`, `ui_header_set_time_in_title`, `ui_header_time_in_title`, `lib_committed` and `ui_playback_resume`. Some may be called within their own TU. Most are Phase 5/11 APIs; `--gc-sections` (P9) collects the rest. **Not worth manual work.**
- **The real size levers are upstream** (`docs/static-ram-audit.md` §4, measured lean config −540 KB resident), plus `--thumb` (§2.3). Per owner COMP-6 the language system stays, but R4 right-sizes it.

## 3. Lean backlog (ranked by saving × confidence ÷ effort)

| Rank | Item | Saving | Conf. | Effort | Phase |
|---:|---|---|---|---|---|
| 1 | F9 art poll on the commit sequence | ~97% of build-time full redraws (0.9M instr + full push per ~1–2 s) | high | S | **now (H1 owns artcache)** |
| 2 | R1 drift next-slot in the strip | −176 KB permanent | high | S | **#38** |
| 3 | F8 scrollbar-hide column repaint | ~1.1M instr per scroll session | high | S | now |
| 4 | F10 thumb-only fill after glides | up to ~0.9M instr per glide on art lists | high | S | now |
| 5 | F2 marquee from a pre-rendered text buffer | 15–38% CPU at 30 MHz while a long label is selected and lit → ~1% | high | M | now / #38 |
| 6 | F1 transparent fast path in the text blend | −34% of all text rendering (m), +16 B IRAM | high | S | now (guard for stock) |
| 7 | F4 slide start boost + `memcpy` swap | −340k instr per push/pop; 60–100 → 15–25 ms latency | high | S | now |
| 8 | F3 qscroll cached letter and slab tables | 378k → ~90k instr per glide frame | med | M | #38 |
| 9 | R2 shared background arena | −115 KB permanent | med | M | H1 follow-up |
| 10 | S1 one-range album-table read | ~120 → ~13 reads per artist open | high | S–M | H1 |
| 11 | Thumb for cold code (`--thumb` measured −162 KB text) | −100–160 KB resident (e) | med (speed check) | M | P9 |
| 12 | F5 + F6 glyph and gloss tiles | chevron 14.6k → 1.5k; header 90k → 15k | high | S–M | P9 trim / later |
| 13 | F13 boost only on the scan fallback | ~30–70 ms at 80 MHz per list open | high | S | H1 |
| 14 | R4 `language_buffer` right-sized (lang kept, COMP-6) | −18.8 KB | high | S | P9 |
| 15 | S2 keep closed views + libfile header resident | one flash wake per back-and-forth while playing | med | M | later |

Not recommended:
- `-O2` for the shell.
- IRAM for shell code.
- 2-bit or subset fonts.
- Manual dead-code removal.
- R3 (pool resizing) before H1 settles: later.
- The dark-state timers (F12): optional.

## 4. Coding rules to add (CLAUDE.md "Efficiency" / process.md self-review)

1. **No per-pixel `lcd_*` calls or divides in loops.** Pre-render glyph tiles and AA edges once, at init or first use, into a movable block and `lcd_bitmap_part` them. Per-row work may compute a colour; per-pixel work may only shift, mask, multiply or add.
2. **Precompute colour tables.** Gloss, gradients and overlays get per-(descriptor, extent) row tables, not per-draw recomputation.
3. **Never re-render text you rendered a moment ago.** Repeated frames (marquee, overlays, cross-fades) blit from a buffer; text rendering is ~30 instructions per cell pixel.
4. **Redraw the changed rectangle, not the screen.** Erasing an overlay or scrollbar repaints its rect; swapping a thumbnail blits the art box. A full-list redraw needs a written reason.
5. **One timer source.** Every periodic wake goes through the main loop's computed timeout, with its lit and dark rates stated in the brief. Nothing animates or polls while dark unless it must, and pollers trigger on the producer's *visible* change (commit sequence), not on intermediate progress.
6. **Boost while composing, never while blocked on I/O.** Hold the boost for frame composition (including the transition's first frame); don't boost around file reads.
7. **Bulk moves use `memcpy`/`memmove`/`memset16`** (IRAM asm), never per-pixel C loops.
8. **Per-frame code gets an instruction count** (the Appendix A harness) in the self-review. Budget for a 33 fps frame: ≤ ~300k instructions boosted, before the push.
9. **Every new reservation names what it shares with.** The strip, background arena or panel transient come first; a new permanent block needs a table line and a "can't share because" sentence.
10. **Count storage reads per user action.** Probe loops (binary search over a file) become one range read or a RAM table.

## Appendix A: the instruction-count harness (scratch; proposed for `scripts/risk/`, owner: tooling)

`uharn.py` (≈110 lines, Unicorn 2.1.4 + pyelftools, scratch venv):
- **Memory:** maps DRAM 0–64 MB, IRAM `0x40000000` (128 KB) and the uncached alias `0x10000000`.
- **Loading:** writes every non-empty `PT_LOAD` segment of `rockbox.elf` at its VMA, skipping `.vectors`.
- **Calls:** places a `b .` sentinel as LR, passes arguments in r0–r3 plus the stack, and counts `UC_HOOK_CODE` callbacks.
- **Fonts:** `load_font_as_sysfont(name)` parses an RB12 `.fnt` (header, bits, offsets, widths), copies it into free DRAM and overwrites the first 11 fields of `sysfont`. Font id 0 then renders through the real 4-bit path.
- **Profiling:** a per-function histogram (symbol bisect) and a per-address hot loop dump located F1.

Measurement drivers used: `measure.py` (§1 table), `marq.py` (marquee), `prof.py`/`hot.py` (profiles). Suggested home: `scripts/risk/lib/elf_call_count.py` with a `scripts/risk/draw_costs.sh` driver, so self-reviews can report per-frame instruction counts (rule 8).

## Device-only checks (P10)
- Marquee CPU% and push time (debug-menu boost ratio and CPU load with a long label selected, before and after F2).
- Glide frame time with the qscroll overlay on a 3k-album list.
- Slide first-frame latency.
- The `--thumb` speed cost on `lib50k`-equivalent opens and on the art builder.
- Whether the F1 veneer and `ICODE_ATTR` change matter with the real cache.
