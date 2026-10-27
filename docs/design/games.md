# Games for P12 Extras: delivery, framework, threads, state, first titles

> **Superseded in part by `docs/design/sdk.md` (rev 0, 2026-10-23):** games are SDK modules with resource classes; the input ownership, storage (a private folder per game), drawing and budgets there take precedence over §1–§4 here.

Research spike for #99, 2026-10-21. Read-only: no game code was written. It feeds the owner discussion before P12's games (#32).

Inputs: `CLAUDE.md`, `docs/design/module-loader.md`, `docs/memory-audit-2026-10-20.md`, `docs/hardware.md`, `docs/data-paths.md`, `docs/hardware-fit-audit.md`, `docs/design/spec/extras.md` and `index.md`, `docs/design/7g-catalogue.md` (Games row), the owner photo `extras-games` (`docs/reference/7g/owner/INDEX-b.md`, photo 1199), `docs/owner-questions.md` EXT-4, #23 (haptic mod and Clicker), #32 (P12), `apps/ipodui/{material,input,click,anim}.h`, the safefile primitive (#91, `docs/phase16-brief.md` §3.3).

Every number below is tagged: **[measured]** (from the cited doc), **[est]** (arithmetic from measured rates) or **[device]** (only the device can tell; on the P10/M1 list in §9).

## 0. What is already decided

- **EXT-4 (owner, 2026-10-07):** "original lookalikes, written natively for our UI: Klondike-style solitaire, a brick-breaker, a parachute-style shooter and a music quiz. No Rockbox plugins." Ledger row `changes-from-7g.md` 2.24.
- **Module loader (2026-10-20):** games are shell modules (#99 named there), not `.rock` plugins. `--ipodui-plugins` would bring back ~202 KB resident plus a 512 KB plugin slot (~0.7 MB of audio buffer) and Rockbox's UI.
- **Look of the list (owner photo 1199):** Extras › Games is a full-width list, two-line rows at ~55 px pitch, a ~44 px icon on the left, thin separators. The subtitle is a play count: "Never Played", "Played Once", "Played 6 Times". The selection bar starts right of the icon.
- **Snapshot-first:** games are on the list of heavy screens that get a snapshot (module-loader.md "What gets snapshots").

This doc answers the questions that are still open: how big, how fast, what the framework does, what a game may touch, where state lives, and the order of the titles.

## 1. Delivery: shell modules through our loader

### Recommendation

**Each game is one shell module in `modules.pak`.** None is built in. Nothing of a game stays resident except its row in the Games list and the 4 bytes of its play counter in the shared state block (§4).

| Option | Resident cost | Audio-buffer cost | Look | Verdict |
|---|---|---|---|---|
| Rockbox plugins (`--ipodui-plugins`) | ~202 KB `_end` [measured, loader study] | + 512 KB fixed slot | Rockbox's splash on load, its API table, its UI calls | Rejected (EXT-4, loader decision) |
| Built into the shell | 4 games × 15-30 KB ≈ 80-110 KB [est] | the same bytes, permanently | ours | Rejected: pays all the time for something used rarely, against the "no idle statics" spirit |
| **Shell modules** | 0 B per game; the game kit is linked into each module (§2) | the shared transient arena, already counted in the floor | ours | **Recommended** |

Why modules fit games well:
- Only one game runs at a time, and nothing else needs the arena while it runs (Cover Flow and a game are never both on screen).
- The loader's rules (no writable `.data`, no IRAM, Thumb, build-ID checked) are exactly what a game needs. A game's mutable state lives in an arena block it asks for at start.
- The simulator links modules statically (module-loader.md "Simulator"), so the UI scripts drive games unchanged.

### Module size budget per game

| Game | Code (Thumb, incl. ~12.5 % veneer/reloc overhead) | Read-only art | Working set (arena block) | Image total |
|---|---|---|---|---|
| Klondike-style solitaire | 12-16 KB | 5-7 KB (suits, ranks, card edge, back tile) | ~4 KB (deal, piles, undo of 1 move, 1 card back cached 40×56 RGB565 = 4,480 B) | **≤ 24 KB** |
| Brick-breaker | 8-11 KB | 4-6 KB (brick bevel rows, ball, paddle caps) | ~2 KB (field 14×8 bricks, ball, levels) | **≤ 18 KB** |
| Parachute-style shooter | 10-13 KB | 6-9 KB (turret frames, craft, jumper, burst) | ~3 KB (object pool of 48) | **≤ 22 KB** |
| Music quiz | 9-12 KB | 2-3 KB (answer tiles use the material kit) | ~6 KB (10 questions × 5 candidate ids, titles cut to 64 B) | **≤ 16 KB + working set** |
| Game kit (inside each module) | 3-4 KB | shares the shell's fonts and materials (resident) | — | counted above |

**Budget rule proposed for P12:** a game's image ≤ **32 KB**, its working set ≤ **24 KB**, so any one game fits the P16 arena of **64 KB** with room left. All figures [est] from comparable shell code (the P16 spike's 14,656 B module from 12,830 B of objects); the build reports the real sizes, and P12's gate checks them.

**Art is never stored as whole bitmaps.** 52 card faces at 40×56 RGB565 would be 233 KB. Faces are composed at draw time from a white rounded card (rows from a material table), a rank glyph (ShellSans, already resident) and a suit glyph (four 1-bit 12×12 masks, 72 B). Art is drawn by us, in the shell's gel/metal style, from `apps/ipodui/art/` sources converted at build time like the other glyphs. No Apple icons, no Apple card designs, no Apple fonts.

### Arena sizing for P12

- **Games need no growth of the arena.** 64 KB covers the largest game (≤ 56 KB image + working set).
- **Cover Flow (P7) shares the same arena** (~512 KB of textures, module-loader.md "Memory"). A game start evicts idle Cover Flow textures. Nothing is written; they come back by re-fetching from the art cache when Cover Flow opens again. If the textures move to BCM SDRAM (P19), the arena drops back to ~64-180 KB and games still fit.
- **The floor:** the arena is reserved at shell start and already counted in the 7.5 MB / 39 MB audio-buffer floors. A game never allocates from the audio buffer while music plays, and never calls `audio_buffer_*` or `plugin_get_audio_buffer`.

### Load time

From the loader's table (module-loader.md "Read path", [est]; device check in §9):

| Storage | 24 KB game | What the user sees |
|---|---|---|
| Flash | ~2-2.5 ms | instant |
| HDD awake | ~25 ms | instant (inside the push animation) |
| HDD asleep | + 1-3 s spin-up | the snapshot shows at once; the game goes live when the read lands |

How it's hidden:
1. **Intent preload:** the load starts when a game's row is highlighted, not when it is selected. Highlighting the Games row in Extras starts #104's predictive spin-up on HDD units.
2. **The snapshot:** each game has one pre-rendered first frame (its title or board view), DMA-read into the framebuffer during the push animation. It costs 0 B of RAM and ~150 KB on disk per game (~600 KB for four). It is rebuilt in the existing snapshot batch (boot hold, post-USB, language change).
3. **Live within ~0.5 s:** the game takes over the frame without a visible change, because the snapshot is pixel-identical to its live first frame (a P12 gate, like the P16 spike's tenant B).

Same behaviour on flash and HDD; only the wait behind the snapshot differs (one-build rule).

## 2. The game kit: a small framework in the shell's look

`apps/ipodui/games/gamekit.{c,h}`, compiled into each module (the loader links modules only against `rockbox.elf`, so there is no module-to-module linking; 3-4 KB duplicated in the pak only, never twice in RAM).

### Drawing

The LCD path sets the rules. A full-screen update costs **27 ms of CPU at 30 MHz** (10.5 PP cycles per 16-bit transfer into BCM SRAM) and 9.6 ms at 80 MHz [measured, S8, `data-paths.md`]. A 30 fps frame is 33 ms, so a game that pushes the whole screen every frame spends all of its frame on the bus. Hence:

- **Dirty rectangles, merged into full-width bands.** The kit keeps up to 8 dirty rects per frame. It merges any that overlap vertically into one band (full-width bands go in one burst, `hardware.md`) and pushes with `lcd_update_rect`. Budget: **≤ 25 KB pushed per frame on average (~4.5 ms at 30 MHz)**; a full-screen push only on a scene change.
- **Draw straight into the shell's framebuffer** (already resident, 153,600 B). No back buffer.
- **The background is a function of y.** Every game background is a vertical gradient or flat fill, drawn from a pre-computed row-colour table (`ui_mat_rows()` or a game table of 240 entries, 480 B). Erasing a sprite = refilling its rows from the table. No saved-background buffer.
- **Materials:** gel (sky/blue/green/red), metal, groove and the selection bar come from `material.h`, so the games share the shell's light and colour. Header bar: the shell's header (title = game name, clock/battery as everywhere). Text through ShellSans and `LANG_*` only.
- **Sprites:** 1-bit masks plus a material (`gk_sprite_mask(x, y, mask, mat)`, colours per row from the material table), or small RGB565 tiles with a colour key (`gk_sprite_key()`, via `lcd_bitmap_transparent_part`). Tiles are rendered once into the working set at load (e.g. the card back), never per frame.
- **Overlays** (pause, game over, quiz results): the shell's dimmed panel and list rows, the same look as a system screen.

### Click-wheel input

Two modes, chosen per game:

- **Menu mode** (Klondike, quiz): the normal `ui_input_get()` events, wheel steps after the list acceleration, SELECT/MENU on release, long press as everywhere. Feels like the rest of the shell.
- **Paddle mode** (brick-breaker, shooter): the kit polls `wheel_status()` once per frame. It returns the absolute finger angle, 0-95 per turn (96 positions, `button-clickwheel.c`), or -1 when the wheel is not touched. The kit turns the change since the last frame (wrapped to ±48) into a signed delta at full resolution, 4× finer than the button events (sensitivity 4). This is "wheel as paddle":
  - **Brick:** paddle x += delta × gain. Gain 2 px per position below ~1 turn/s, rising linearly to 4 px at ~3 turns/s, so a slow finger is precise and a flick crosses the field (320 px ≈ ¾ turn slow, ⅓ turn fast). Velocity comes from the same deltas (and matches `button_get_data()`'s velocity for the event path).
  - **Shooter:** turret angle += delta × 1.875° (a full wheel turn = 180° of turret travel, clamped at the ends).
  - Lifting the finger stops motion at once (no inertia), as the hardware has no momentum.
- **Buttons in paddle mode:** SELECT = launch/fire, PLAY = pause, MENU = pause menu. Holding SELECT auto-fires at a capped rate.
- **Every pause menu ends with Exit** (owner, 2026-10-24, a standing rule): the SDK's `SDK_STR_EXIT` row, which leaves the game through its graceful exit with the game saved (`sdk.md` §5.4). Solitaire, Bricks, Chute and Quadrus have it.

### Clicker and haptic feedback

- **One feedback channel: the existing clicker** (`ipodui_click_play()`; Clicker setting Off/On, On = headphone click + piezo, which drives the haptic mod, #23). Games never call `piezo_*` directly and never add a mixer channel.
- **Events, not frames:** a click on a brick hit, a card dropped on a legal pile, a turret step every 15°, a quiz answer. Off when Clicker is Off.
- **Rate limit:** at most one click per 50 ms (20/s), so the motor is never asked for more than it can render (#23 "never click more than the motor can render"). Cascades (a brick combo) collapse into one click.
- **No sound effects in v1** (stock 7G games' audio is not ours to copy, and a mixer channel would compete with the music). Owner question G-5.

### Pause, interruptions, return

| Event | Game does |
|---|---|
| Hold switch on | Pause at once; draw the pause overlay; release boost; stop the frame loop (block on input) |
| Headphones unplugged | The player pauses music (existing behaviour); an action game pauses too, a board game carries on |
| PLAY pressed | Action games: pause the game. Board games: play/pause the music as everywhere |
| MENU | Pause overlay with **Resume**, **New Game** and the game's own items; MENU again returns to the Games list |
| USB connected | The system USB screen takes over (owner rule: no disk writes at USB entry). The game's screen stays on the stack, its state in its arena block; when USB ends, the game reappears paused. If a library update after USB changed the database, the music quiz restarts its round (other games don't care) |
| Backlight timeout | Normal timeout in board games and while paused. In an action game, wheel activity keeps it lit; a paused action game times out normally |
| Battery critical / shutdown | Saved by the shutdown path's existing flush (§4) |

**Return to Extras cleanly:** popping the game screen calls the module's `exit()`, which the kit wraps: release any boost, cancel the frame timer, write state if dirty (§4), free the working set. The loader then drops the module's refcount; it stays cached in the arena until evicted. The Games list redraws live (never a snapshot) with the new "Played N Times" subtitle.

### Frame pacing and boost policy

- **Board games are event-driven:** no frame loop at all while nothing moves. A card glide or a deal animation uses `ui_anim_*` (≤ 30 fps, `UI_ANIM_FRAME_TICKS`) and stops.
- **Action games run a 30 fps loop** on `ui_anim_frame_due()`/`ui_anim_wait()`, and **drop frames** instead of slowing down: the game advances by elapsed ticks (fixed 1/30 s steps, at most 2 catch-up steps, then the extra time is dropped).
- **Unboosted by default.** Budget per frame at 30 MHz: ~1,000,000 cycles. A brick frame pushes the paddle band (320×8, 5 KB, ~0.9 ms) plus the ball's union rect (~0.2 ms) plus logic (< 0.5 ms): **~2 ms of 33** [est]. The codec runs on the COP since Fit-2 (CPU ~88-92 % idle while playing FLAC [measured, S36]), so the game has the CPU. It also fits 24 MHz (800 k cycles) when the idle clock policy has chosen it.
- **Boost only for full-screen changes** (level start, game over panel, the push into the game), ≤ 0.5 s each, through `ui_anim_begin()`/`ui_anim_idle()`, which already pair `cpu_boost`. Every kit exit path releases it; the P12 gate checks the balance.
- **If a frame needs more than 20 ms twice in a row at 30 MHz**, that game has a design problem (too much pushed); the kit logs `gk slow` so it shows in the trace, rather than boosting.

### Battery cost per hour

No current sensing exists on this hardware (`hardware.md` power log), so these are estimates to be replaced by the P10 A/B method (30 min each, `powerlog` `boost_pct`/`bl_pct`):

| Session | What runs | Estimate |
|---|---|---|
| Klondike, music playing, backlight on | 30 MHz, mostly idle; codec on COP; backlight lit | roughly the cost of browsing with music: **≈ 1-1.5 % of 3,000 mAh per hour more than music with the screen off** [est] |
| Brick, music playing | 30 fps loop at ~6 % CPU, backlight lit continuously | **≈ 1.5-2.5 % per hour more** [est]; dominated by the backlight and LCD, not the CPU |
| Any game, paused, backlight off | nothing but playback | = playback alone |

The dominant cost is the backlight (lit for the whole session), not the CPU, which is why staying unboosted matters less than keeping the bus traffic small (each push is CPU-busy time). Device measurement in §9.

## 3. Threads: what a game may start

**Nothing.** A game runs entirely on the UI thread (`PRIORITY_USER_INTERFACE`, 16).

| Item | Rule | Why |
|---|---|---|
| Threads | **None.** `create_thread` is not reachable from a module (a link-time assert on the symbol) | The codec (13), buffering (15) and video feed (14) must always win; a game thread would also need its own stack (static or allocated) and teardown |
| Timers | Frame pacing through `ui_anim_*` and the input timeout only. **No `tick_add_task`, no `timer_register`** (both run in interrupt context) | An IRQ-time callback into an evictable module is a crash waiting to happen |
| COP jobs | **None.** The COP runs the codec since Fit-2 | Games must never cost audio a decoded frame |
| Background work | If a game needs I/O (state file, quiz candidates), it uses the **existing background worker** the loader uses, by posting a job and continuing to animate | One worker, no new stack |
| Audio | No `audio_buffer` grab, no PCM playback of its own. Only the music quiz uses playback, through the borrowed-session API in §5.4 | The floors; music keeps playing |
| Locks | None beyond what the shell's APIs take | No new contention with the codec/buffering threads |

**Priority versus audio:** at priority 16 the game is pre-empted by buffering (15) and the codec (13 on CPU paths). If buffering refills during a frame, the frame is late and is dropped; audio never underruns because of a game. That's the intended direction.

**Cleanup:** the kit's `exit()` runs on every exit path: MENU, USB (only when the screen is actually popped, never at USB entry), shutdown, a panic-free error. Since a game owns no thread, timer or lock, cleanup is: boost released, working set freed, state written if dirty. The loader refuses to evict a module while its screen is on the stack, so nothing can run into freed code.

## 4. State: scores, saved games, play counters

### One small file for every game

`games.dat` in the hidden system folder (never named on screen). Fixed layout, ~1 KB:

```
header   16 B   magic "PKGM", version, build-independent layout id, CRC-32
counters 4 × 8 B   per game: plays (u32), best score (u32)
saves    4 × 224 B per game: a saved-game blob (Klondike: 52 card bytes +
                   pile lengths + score + time ≈ 120 B; brick: level, lives,
                   bricks bitmap ≈ 40 B; others: unused)
```

- **Resident part:** only the 4 play counters (16 B), loaded at shell start with the snapshot batch, because the Games list shows them live without loading any module. Everything else is read by the game when it starts.
- **"Played N Times":** incremented when a game is entered from the list (stock counts launches), in RAM at once (the list subtitle updates on return), written with the next save.
- **Strings:** three `LANG_*` IDs: `LANG_GAME_NEVER_PLAYED` "Never Played", `LANG_GAME_PLAYED_ONCE` "Played Once", `LANG_GAME_PLAYED_N` "Played %d Times" (short labels, matching stock wording, CLAUDE.md rule 2). Three IDs instead of a printf plural, so other languages can phrase each case.

### Safe writes

- **`safefile_replace()`** (#91: tmp written and closed, remove prev, dest → prev, tmp → dest, remove prev) for every write; **`safefile_recover()`** at shell start before the counters are read. A power cut at any step leaves the old or the new file, never a torn one; the CRC catches anything else (then counters reset to 0 rather than showing garbage).
- **When it writes:** once on game exit if anything changed, and on the pause overlay of a game in progress (so a USB plug-in after a pause loses nothing). **Never** per move, per frame, at USB entry (owner rule), or while the disk is asleep just for a counter: on HDD units a write that would wake the disk waits for the next natural spin-up or for exit, whichever comes first (`docs/storage-choices.md` gets a line if flash tuning ever differs; behaviour is identical).
- **Shutdown and sleep:** the existing shutdown flush calls the games' save hook if a game is on the stack.

### Surviving USB and library updates

- The file is in the hidden folder, which survives drag-and-drop over USB like the other system files. If the user deletes it, every counter reads "Never Played" again; nothing else breaks.
- **Library updates do not touch it.** Only the music quiz depends on the library, and it keeps no library ids in the file: each round draws fresh candidates. Its best score stays meaningful.
- **Layout id, not build id:** a firmware update keeps scores and saves. A layout change migrates or resets only the affected block.

## 5. Which games, in what order

Order: what the owner listed in EXT-4, sorted by the risk each one retires.

| # | Title (working names, owner to pick) | Mode | Why this order |
|---|---|---|---|
| 1 | **Solitaire** (Klondike rules) | menu mode, event-driven | Proves the module path, the kit's drawing and overlays, saves and counters. No real-time risk |
| 2 | **Bricks** (brick-breaker) | paddle mode, 30 fps | Proves paddle input, frame pacing, dirty bands, haptic rate limit |
| 3 | **Music Quiz** | menu mode + playback | The only game touching playback; needs the borrowed-session API (§5.4), so it goes after the kit is stable |
| 4 | Parachute-style shooter (built as **Chute**, §5.3) | paddle mode, 30 fps, many objects | Reuses 2's loop; adds an object pool |
| 5 | **Quadrus** (falling blocks; the owner's name, 2026-10-23) | real time; Class A in P12, Class B after P19 | Our own name and art, never "Tetris" or its trade dress. Settings: Start Level, Ghost Piece. Controls (owner, 2026-10-24): wheel moves, SELECT tap turns at once, double tap drops hard, SELECT held soft-drops, MENU tap holds or swaps, long MENU pauses. Class choice and details: `sdk.md` §13.2 |
| long term | a step-chart rhythm game (working name WildWheel; owner idea, on the tracker) | Class C, wheel regions, its own songs | Plays `.sm`/`.ssc` charts with their music, our own parser, name and art. Not built; `sdk.md` §13.4 lists what the SDK provides for it |
| later | a Vortex-like tube shooter | 30 fps, full-screen motion | Not in EXT-4's list; the only one that would want BCM help (§6) |

The list order in Extras › Games is alphabetical by the localised name, as on stock (iQuiz, Klondike, Vortex is alphabetical).

### 5.1 Solitaire (Klondike rules)

- **Board (320×240 below the 20 px header):** 7 tableau columns at 44 px pitch, cards 40×56 with a 2 px radius; face-down cards step 5 px, face-up 14 px (the overlap rules below). Stock and waste top-left, 4 foundations top-right.
- **Card faces (owner, 2026-10-25: "the top left should have the number, the top right, the suit icon, so you can see the icon easily from the stacks... the colors are hard to see at times"):** every face-up card carries its **rank top left and its suit top right**, both in rows 3-12 of the card, the strip a stacked card keeps showing; one big suit sits in the middle. Our own pixel art, drawn bold: ranks 10 rows high with 2 px strokes (8 px wide, "10" 11 px), corner suits 9×9 that differ by shape as well as colour (the spade pointed with a stem, the heart notched on top, the diamond pointed at both ends, the club three round lobes on a stem), bottom-aligned with the rank. Inks: red #D8000C (5.2:1 on the face) and near-black #08080C (19:1) on a #FFFFFC face, edge #484C54.
- **Overlap (as built, 2026-10-25):** face-down cards step 5 px and face-up cards 14 px, so every face-up card shows its whole strip. A pile too long for the screen first squeezes its face-down step to 3 px, then the face-up cards just after the run's first one to 5 px (the run's top card and the newest cards keep their whole strip); only if even that is not enough does the run squeeze evenly. The deepest legal pile (6 down + 13 up) ends at row 238 [host-checked over every length]. The draw-three fan steps 14 px, so a "10" shows.
- **Input: two shelves (owner, 2026-10-25: "everything should move like a shelf rather than a rotational thing"; as built):** the **top shelf** holds the stock, the waste and the four foundations, the **bottom shelf** the seven tableau piles. Clockwise moves the cursor **right** along its shelf, counter-clockwise left. Past the right end of the top shelf it carries on at the **left end of the bottom shelf**, and past the bottom shelf's right end back to the stock, like reading lines of text (and the reverse the other way). *Why not "the other shelf at the matching column":* with only a wheel, landing on the bottom shelf's right end would put the next clockwise click past its end again, so the cursor would bounce between the two right ends, and clockwise would have to mean "left" on one of the shelves (which is what made the old ring feel rotational). Reading order keeps clockwise = right everywhere; the farthest pile is 12 clicks away, half a turn.
  - **SELECT** on the stock draws. On a pile it picks up what the cursor's blue frame shows: a tableau pile's **whole face-up run**, else the top card. While cards are held, the gold frame shows **the part of the run that would go to the pile under the cursor** (in Klondike only one part ever can: the ranks decide it), and the cursor's frame turns grey where nothing fits. SELECT drops that part; SELECT on the pile it came from puts it back. No second "how many" step is needed.
  - **Double SELECT** (two presses within 350 ms, wheel still) or a **long SELECT** sends the top card to its foundation. **Short MENU** puts held cards back, else opens the pause menu (Resume, New Game, Undo, Settings, Exit); **long MENU** leaves the game (the deal saved). PLAY toggles music as everywhere.
  - *The other option, for the owner:* a **picking mode**. SELECT on a column enters it with the deepest movable run marked; the wheel then chooses how many cards (one click per card, the gold frame growing and shrinking), SELECT takes them and the wheel moves along the shelves again. It costs an extra step on every multi-card move and only helps where a run could go two places at once, which Klondike never allows, so the auto-fit above was built. Easy to add if wanted.
  - The cursor is not saved (a resumed deal starts on the stock); the state record is unchanged (`KL_VER` 1).
- **Classic feel (owner, 2026-10-23):** plain Klondike, as close to the familiar game as we can make it; unlimited passes through the stock.
- **Pause overlay:** Resume, New Game, Undo (one move), **Settings**.
- **Settings (owner, 2026-10-23):** its own Settings screen with **Draw: One Card / Three Cards**, kept in the game's private folder through the SDK's per-game settings (`sdk.md` §9.4, the worked example). A change applies from the next deal.
- **Feel:** a picked card lifts 3 px with the shell's 10 px shadow strip; drops glide in 6 frames (~0.2 s); the win cascades cards to the foundations at 30 fps (boosted ≤ 2 s, the one long boost).
- **Art:** our own card back (metal with a ring motif from our boot glyph), suits drawn by us, ranks in ShellSans.
- **Saved game:** yes, every exit; "Resume" is the first item when a save exists.

### 5.2 Bricks (brick-breaker)

- **Field:** 8 rows × 14 bricks (22×10 px, gel materials in four colours by row, 1 px bevel), paddle 48×8 at y 222, ball 6×6.
- **Input:** wheel as paddle (§2); SELECT launches the ball; PLAY pauses.
- **Rules:** 3 lives; ball speed steps up each 12 bricks and each level; the angle off the paddle depends on where it hits (5 zones). Levels: 6 hand-made layouts, then they repeat faster.
- **Settings (P12, owner feedback 2026-10-24: the paddle felt sluggish):** Speed (ball: Slow, Normal, Fast), Balls (3 or 5) and **Paddle Speed**, five levels as a value list: Slow, Normal (default), Fast, Faster, Fastest; applies at once. The game takes the raw wheel (`SDK_WHEEL_REL_RAW`, one step per 15 degree click, 24 a turn), sums every click pending in a frame and moves the paddle by `bk_wheel_px()` (`bricks_core.c`): each click moves `lo` px when turned slowly, rising linearly with the wheel's clicks/s to `hi` px at 40 clicks/s and above. Per level (lo..hi px a click): Slow 8..18, Normal 14..30, Fast 20..42, Faster 28..56, Fastest 40..80. The paddle's travel is 260 px; clicks to cross it (host test, constant speed): Slow 29 / 20 / 15, Normal 17 / 12 / 9, Fast 12 / 9 / 7, Faster 9 / 7 / 5, Fastest 6 / 5 / 4, at 5 / 20 / 40 clicks/s. Normal at a brisk 20 clicks/s crosses in 12 clicks, half a turn; the old fixed 6 px per accelerated step took about 43 clicks (nearly two turns). The mapping is clamped at both walls and odd in the clicks. A settings change resets saved settings once (table version 2).
- **Push budget per frame:** paddle band (5 KB) + ball union rect + changed bricks: ~6-8 KB typical, < 25 KB worst case.
- **Feedback:** click per brick (rate-limited), longer pulse (2 clicks 60 ms apart) on a lost ball.
- **Score:** best score saved; no mid-game save in v1 (a paused game survives USB in RAM). *As built (P12 W4): the game in progress is saved in the SDK state record too, and Speed and Balls are settings; see `sdk.md` §13.3.*

### 5.3 Parachute-style shooter

> **As built (P12 W4):** the owner's brief asked for a Game & Watch-style arcade game with our own theme, so it shipped as **Chute** (named Parachute, then Airdrop, until the owner's renames, 2026-10-24), a lane catch game on an LCD look, not this turret shooter; see `sdk.md` §13.3. The sketch below is kept for the record.

- **Scene:** a turret at the bottom centre on a metal base; aircraft cross the sky in lanes at 3 heights and drop jumpers who drift down on canopies. Jumpers that land pile up beside the base; 4 on one side ends the game. Our own shapes and theme (owner to pick; default: neutral aircraft and paper-white canopies on the sky gel).
- **Input:** wheel turns the turret (§2); SELECT fires (capped at 6 shots on screen); PLAY pauses.
- **Objects:** a fixed pool of 48 (shots, craft, jumpers, bursts), each 8-16 px; dirty rects merged per band. Worst case ~30 moving objects ≈ 30 × 2 × 16×16 × 2 B ≈ 30 KB if pushed separately; merged into bands it stays under the 25 KB average by limiting craft to 3 lanes [est; the sim's push trace confirms].
- **Score:** points per craft and jumper; best saved.

### 5.4 Music Quiz (using the owner's own library)

The game: a short clip of a song plays; pick its title from five. Faster answers score more (the score counts down while the clip plays, ~10 s max). Ten questions per round. Our own presentation: the five answers as list rows, the countdown as a groove capsule bar.

**The hard part is playback.** It must not disturb the user's queue, resume points, statistics or settings. Designs considered:

| Option | Problem |
|---|---|
| A. A second decoder inside the game | Needs codec memory, a codec slot and PCM output alongside playback: against the floors, and the COP already runs the codec. Rejected |
| B. Insert the quiz tracks into the user's queue | Changes the queue and position, writes play counts and "last played", moves resume points, may touch shuffle. Rejected |
| **C. A borrowed playback session (recommended)** | Needs a small playback hook (below) |

**C, the borrowed session.** A new playback-side API in the shell (`apps/ipodui/np_borrow.{c,h}`, using P16's R4 queue-by-database-index):

1. `pb_borrow_begin()`:
   - records the user's session: the queue as database indices (R4, a few bytes per entry, already the in-RAM form), the current position, the elapsed time, play/pause state, shuffle and repeat;
   - pauses the user's music (default; owner question G-3), keeping its buffered data where possible;
   - turns on **statistics suppression** for the borrowed session: no play counts, no last-played, no skip counts, no resume-point or bookmark writes, no `.playlist_control` writes, no history entries, no "On-The-Go" changes.
2. The quiz plays its clips through the normal engine as a **temporary queue of the round's 10 tracks**, each started at an offset (35 % of its length, at least 15 s in, clamped so ≥ 12 s remain). Buffering prefetches the next clip by itself while the current question runs, so later questions start without a disk wait.
3. `pb_borrow_end()` (on every exit path, including USB once the screen is popped, and shutdown): restores the queue by indices, the position and the elapsed time, shuffle and repeat, and leaves the music **paused** at the exact point (default; G-3). Statistics suppression ends. The user's next PLAY continues as if nothing happened.

**Question building**, from tagcache and the shell's library precompute only (never a directory walk):
- Candidates: music only (no audiobooks, podcasts, or anything the library marks as spoken word), length ≥ 45 s, a format the device decodes at the current clock without a boost (`clockpol`'s "light" table), a title that differs from the other four after case folding.
- The four wrong answers come from other artists, preferring the same genre so it is not trivial (fallback: any genre).
- Libraries under 25 eligible tracks: Music Quiz shows a one-line notice in our own words (`LANG_*`) on its first screen and offers no round. Exact wording to be written in P12.
- **Variants that need no audio** (owner question G-4): "Name the album" from cover art (art cache, pre-scaled, no JPEG decode) and "Name the artist". These don't borrow playback at all and work even while the user wants their music untouched.

**Costs:** the borrow record is ~16 B + the queue's index array, which already exists in RAM (R4), so setting it aside means keeping its block and recording a length; the quiz queue and the saved header take a ~8 KB block from the arena [est]. On HDD the first clip may need a spin-up (1-3 s): the round's intro screen (rules, "Ready") covers it, and the first clip is buffered before the countdown starts.

**What needs a hook outside `apps/ipodui/`:** statistics suppression and a "don't save the playlist control file" flag, each a few lines in `apps/playback.c`/`apps/playlist.c` (or the shell's own statistics journal path), guarded by `HAVE_IPOD_UI` and logged in `docs/changes-from-rockbox.md`. Owned by playback-audiobook in P12.

### 5.5 Later: a Vortex-like tube shooter

A ship circles the rim of a tube drawn in perspective; enemies come up the tube. The wheel maps naturally to the ship's angle (96 positions = 96 rim stops). It is the only candidate whose every frame changes most of the screen: a full push is 27 ms at 30 MHz, so it would need **boosting for the whole session** (9.6 ms pushes at 80 MHz) or BCM help (§6). Deferred until after the four EXT-4 games, and only if the owner wants it.

### 5.6 Chess (#132; Class A, event-driven, a sliced engine)

**Owner requirements (2026-10-24):** the ring's absolute position as taps and holds, the wheel to move the cursor, SELECT to pick and to place, MENU tap to undo or cancel, a long MENU for the pause menu with Exit; three engine levels; the full rules; our own board, pieces and a cover in the Games list's box-art set; the game saved in the state record. Built in `apps/ipodui/games/chess/` (rules `chess_core.c`, engine `chess_engine.c`, pad `chess_pad.c`, game `chess.c`); host tests `tests/chess` (perft, rules, engine, pad); UI scripts `tests/ui/ipodui-p12-chess-{start,pad,pause}.script`.

**Class: A, not B.** Class B (a borrow from the audio buffer) is unbuilt until #108/P19, and an unbuilt class is *hidden* on every unit (`sdk.md` §0). So the engine is sized to the module arena instead: image 12,964 B + `.bss` 68 B (footprint 13,360 B, with the two-player mode) + work 17,408 B = **30,768 B**, under the release arena's 31,744 B (33,792 - the 2 KB index stage; developer 37,888 B). `min_ram = 32`. The one knob for a later Class B build is the table size (`CH_TT_BITS`): with a borrow it would grow from 512 to 64 K entries and the search a ply deeper; nothing else changes. Chess is now the arena's largest tenant (xindex was 24,720 B): the sizing rule (largest + stage + 25 %) would ask for 41,984 B; `modpak.py check` passes, the headroom figure is for the orchestrator (`docs/hardware.md`'s table is theirs).

**State of the art (process item 7; `docs/algorithms-survey.md` §5, "Chess search").**

| Part | Chosen | Not chosen, and why |
|---|---|---|
| Board | **0x88 mailbox** (128 B; off-board test `s & 0x88`; ray scans) | Bitboards with magic multipliers: 64-bit shifts and multiplies take two registers each on the ARM7TDMI, there is no CLZ for bit scans (ARMv5), and the magic tables cost KBs the arena doesn't have |
| Search | **alpha-beta, iterative deepening**, a transposition table (8 B entries, 512 of them, depth-preferred), ordering by table move, MVV-LVA captures, two killers a ply; a **check extension**; a **captures-only quiescence** (queen promotions included) | PVS, null-move pruning and late-move reductions: later, measured; at the depths the PP reaches in a few seconds, ordering matters more than pruning |
| Evaluation | **tapered** middlegame/endgame piece-square tables (the PeSTO idea), **our own values** from simple formulas (centralisation, pawn advance, a castled king), packed as two 16-bit halves in one word so make/unmake updates both with one add; a bishop pair; a tempo. Blended by phase without a divide (`x * 1365 >> 15` = x / 24) | PeSTO's published tables themselves (we write our own numbers); NNUE (tens of KB of weights, multiply-heavy) |
| Hash | 32-bit keys from an integer mixing function (two multiply-xorshift rounds) instead of a 3 KB Zobrist table; the en passant square is in the key only when a capture there is possible, so repetitions are exact | 64-bit Zobrist: twice the work on a 32-bit core; a 16-bit check plus the table index gives 25 bits of verification, and a table move is always checked for legality before use |
| Scheduling | **a resumable search**: an explicit stack (a frame a ply) in the working block, run `THINK_NODES` = 16 nodes between clock reads for at most 10 ms of a step (the SDK's slice is 20 ms), at <= 30 steps a second; the UI thread returns to the scheduler between steps, so buffering and the codec are never starved | a recursive search inside one call: would hold the UI thread for seconds and trip the SDK's slice watchdog |

**Two players (owner, 2026-10-25).** The first setting is **Opponent: CPU, Player** (CPU the default; applies from the next New Game, as Play As does). With Player, **Level and Play As are greyed out** and the cursor skips them: the SDK's settings rows gained a dependency (`sdk_setting.off_if`/`off_val`, in what was padding, so the struct and the ABI keep their size; `sdk.md` §9.1) drawn with the list's new disabled look (`ui_list.is_off`, the theme's `list_fg_off`, #A8A8A8 [~ no reference: the 7G greys rows out nowhere we have a photo of]). The game is then passed hand to hand: the engine never starts (`cs_person_moves()`), White and Black move in turn with the same controls, the panel says **White to move** / **Black to move** (the small font: the list font does not fit the 92 px panel) and drops the level line, and a MENU tap takes back **one ply** (`cs_take_back()`, host-tested with the engine's rule in `tests/chess`). Checkmate names the winning colour (White wins / Black wins) in both modes; stalemate and the draws name their reason. The board keeps **White at the bottom** for the whole game; turning it round for each player's move could be an option later (a setting, or automatic at each turn: it redraws all 64 squares a move, ~one full board push). The mode is saved in the state record (`ch_rec.mode`, the old reserved byte, so a version 1 record resumes as a game against the engine); the settings table is version 2 (Opponent in front), so earlier settings reset to their defaults once. Scripts: `tests/ui/ipodui-p12-chess-two.script` (the greyed rows, four alternating moves, a one-ply take-back).

**Levels** (`levels[]` in `chess.c`): Easy depth 2, 1.5 s, root noise +-60 cp (a fixed per-move offset from the SDK's random number: weaker and varied); Medium depth 4, 4 s, +-6 cp (the default); Hard depth 6, 10 s. The time cap wins over the depth: when it passes, the engine plays the last complete iteration's best (or a better root move of the iteration under way, which searched the old best first). A reply never lands sooner than 350 ms after your move.

**Speed [est].** Host (Apple M-series): ~5 M nodes/s. At the device's ~400x (process checklist item 3) and with the engine running ~10 ms of every 33 ms step, about 4-5 k nodes/s: Easy (~1-3 k nodes) under a second; Medium reaches depth 3 in typical middlegames (9-80 k nodes for depth 3-4) before its 4 s cap; Hard depth 4. **[device]** nodes/s at 30 MHz, the depth each level reaches.

**Never while dark or asleep.** The SDK sets `SDK_INF_DARK` while the backlight is off: the game stops searching and returns `SDK_IDLE`; the SDK steps it again when the screen lights (`SDK_INF_LIT`). A soft sleep suspends the game (§2.2 of `sdk.md`). No boost is taken.

**Input.** `sdk_wheel_pos()` (the SDK's absolute position, 0-95 from the top, clockwise; built for every class for this game, Q30's default) feeds `chess_pad.c`: a finger set down in a quarter and lifted within 400 ms is one step that way; held still past 450 ms it steps every 160 ms; a slide of more than 4 positions is a turn, and a button pressed under the finger (`SDK_INF_PRESSED`) is a press: neither steps. The wheel's own clicks (`SDK_WHEEL_REL_RAW`) step the cursor through the squares that matter in reading order. **[device]** the controller's packet rate while a finger rests (D-W1): if it sends nothing while still, a hold is seen only through the SDK's 50 ms poll of `wheel_status()`, which keeps the last position until a lift packet.

**Memory** (the 17,408 B working block): the engine 5.8 KB (its position copy, 1,024 moves and scores on its stack, 36 frames and undo records, killers), the table 4 KB, the piece-square tables 1.5 KB, the repetition ring 1 KB (256 keys), the game position, the legal moves, the row tables and what each square shows (~1.4 KB), the state record 2,048 B (header, then up to 1,020 plies as 16-bit moves; 8-aligned via `SDK_REC_OFF`) and the SDK's own context. A game longer than 1,020 plies is drawn. Resume replays the moves in slices (at most 40 ms in `init`, 8 ms a step after), so even a full record never trips the slice watchdog.

**Drawing.** 26 px squares, a 208 px board at the left, the panel at the right (the side to move's king, Your Move / Thinking, or White / Black to Move with two players, / the result, Check, the level, the move number). Pieces are 20 x 18 one-bit silhouettes of our own; the outline is the silhouette minus its 4-neighbour erosion, the inside a two-tone row gradient (`sdk_rows`), a few details in the outline's colour. Each square remembers what it shows; only squares whose look changed are drawn and pushed (a move: 4-8 squares, at most 4 full-width bands), the full board only at entry, a new game, a take-back and under a closed menu.

**Device-reality checklist (#132).**

| Item | Answer |
|---|---|
| Compiled features | Module builds only (`IPODUI_WITH_MODULES`); the sim links the game statically. `wheel_status()` is target-only (`HAVE_WHEEL_POSITION`); the sim reads `wheel_at`'s position (`sim_cmd_extras.c`) |
| Memory during playback | No allocation outside the arena's working block (17,408 B, inside the floor); no audio-buffer use |
| CPU at 50k | Not library-dependent. The engine: <= 10 ms of game time a step, <= 30 steps a second, unboosted |
| Storage wake-ups | One state record read at entry; the record written at suspend/exit by the SDK (never per move: `sdk_state_commit` only marks it) |
| LCD | Changed squares only (<= 8 per move, bands merged); the full board at scene changes |
| Threads and locks | None; UI thread only |
| Splashes | None; a bad record starts a new game (logged `bad record at`) |
| Persistence | The move list and the mode (engine or two players), through the SDK's state file (safefile); resume replays it |
| Scale | Game length only (<= 1,020 plies, replayed in slices) |
| Stock firmware unchanged | Only `english.lang` grows (28 phrases appended) |
| Hardware features | None apply: the search is branchy integer code with a tiny working set, a poor fit for the BCM2722's vector unit, and the COP runs the codec (Fit-2) |

## 6. COP and BCM2722 help

- **COP: no.** It runs the codec (Fit-2, [measured] S36). Games are light enough for the CPU at 30 MHz; moving drawing to the COP would add cache-coherency work (no coherency between the cores, `data-paths.md`) and take headroom from audio.
- **BCM2722: not for the four EXT-4 games.** Their pushes average ≤ 25 KB a frame (~4.5 ms CPU at 30 MHz); there's nothing worth offloading.
- **Possibly for a Vortex-like game (later, P19+):** with our own VideoCore image resident (`HAVE_BCMFW_DEFAULT`), the BCM could hold the static tube in its own SRAM/SDRAM and compose sprites itself, so the PP sends a ~64-byte scene per frame instead of 153.6 KB. That turns a 27 ms CPU push into microseconds and removes the need to boost. Cost: a new host command and BCM-side code in the image (#60 territory), plus the BCM staying awake while playing. Only worth it if the owner wants the Vortex-like game; recorded here as the one clear BCM case.
- **Engine-2 DMA for full-frame pushes** (`data-paths.md` E2-7, unproven on silicon) would also help the Vortex-like case and full-screen transitions, without game-specific code. Nothing in the games depends on it.

## 7. Device-reality checklist (for the P12 brief)

| Item | Answer for games |
|---|---|
| Compiled features | Games only with `HAVE_IPOD_UI` and the module loader on (P12 turns it on by default). Stock build unchanged |
| Memory during playback | No audio-buffer allocation: image + working set ≤ 56 KB in the 64 KB arena, already in the floor; the quiz's borrowed queue comes from the arena too |
| CPU at 50k | Only the quiz's candidate draw scales with the library: 10 × 5 random picks from the precompute (O(1) each, retry on a filtered pick), < 5 ms at 50k [est] |
| Storage wake-ups | Load: one read (preloaded on highlight). State: one write on exit/pause. Quiz: clips via normal buffering. No wake per move |
| LCD | Dirty bands only, ≤ 25 KB/frame average, full pushes only on scene changes |
| Threads and locks | None created; UI thread only |
| Splashes | None. Errors logged; a failed module load shows the Games list again, with the row's subtitle unchanged, and logs `gk load fail` |
| Persistence | `games.dat` via safefile; recovered at start; CRC |
| Scale | Library size only affects the quiz's eligibility filter |
| Stock firmware unchanged | Yes |

## 8. Open owner questions (defaults let P12 start)

| ID | Question | Default |
|---|---|---|
| **G-1** | Game names on screen: our own names (e.g. "Solitaire", "Bricks", "Music Quiz", "Chute"), not Apple's "Klondike/iQuiz/Vortex". Pick or rename | Solitaire, Bricks, Music Quiz, Chute (owner rename of Parachute, then Airdrop, 2026-10-24) (generic words, no trademarks) |
| **G-2** | Row icons: 44 px original icons in the shell's gel/metal style, one per game | Yes, drawn by us; dice-placeholder style (as the photo's grey dice) until art lands |
| **G-3** | Music Quiz and your music: pause your music during a round and leave it paused at the same spot afterwards, or resume it playing automatically? | Pause, restore paused at the exact spot |
| **G-4** | Music Quiz variants without audio (name the album from its cover, name the artist) | Include "Name the Album" as a second mode; artist mode later |
| **G-5** | Sound effects through the headphones (a separate mixer channel) beyond the clicker? | No: clicker/haptic only |
| **G-6** | MENU in a game: pause overlay first (Resume/New Game), second MENU back to the list; or straight back to the list with an auto-save? | Overlay first |
| **G-7** | Vortex-like game: wanted at all? If yes, accept a boosted session or wait for BCM help (P19)? | Not in P12; revisit after P19 |
| **G-8** | Played counter: count every launch (stock) or only finished games? | Every launch |

## 9. Device measurements needed (P10/M1 list)

- Module load latency for a 24 KB game on flash, on an awake HDD and on a sleeping HDD, while music plays, with 0 underruns.
- Push time per frame for Bricks and Chute at 30 MHz and 24 MHz (sim trace for bytes, device for ms).
- Battery: 30 min A/B of music with screen off vs Solitaire vs Bricks (`powerlog` method), to replace §2's estimates.
- Haptic: whether 20 clicks/s renders cleanly on the motor, and the minimum gap between two distinguishable pulses.
- Music Quiz: time from "question shown" to sound on flash and HDD; that the user's queue, position, play counts and resume points are byte-identical before and after a round (a host test can check the statistics journal; the device checks the queue file).
- Wheel paddle feel: gain curve by feel on the device.
