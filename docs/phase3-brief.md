# Phase 3 brief: smooth list, fonts, 7G polish

**Goal:** the shell should *feel* like a 7G:
- pixel-smooth, velocity-sensitive scrolling;
- push and pop slide transitions;
- a scrollbar while scrolling;
- marquee for long selected rows;
- TeX Gyre Heros anti-aliased fonts;
- the glossy gradient selection bar and header;
- an original chevron.

The rules in `docs/hardware.md` still apply. Animation is the one place `cpu_boost` is allowed, and only while frames are actually being drawn.

Read first: `CLAUDE.md`, `docs/hardware.md`, `docs/design/spec/index.md`, `docs/reference/7g/MEASUREMENTS.md`, `docs/design/fonts.md`, and the current `apps/ipodui/` code (Phase 2).

**Out of scope:**
- the quick-scroll letter overlay: it needs sorted tagcache lists, so it goes in Phase 4;
- album art in the panel (Phase 7);
- Now Playing (Phase 5).

## Work split (three agents in parallel, each in its own worktree)

| Agent | Owns (may edit) | Must not edit |
|---|---|---|
| **A: motion** | `list.{c,h}`, `screen_stack.{c,h}`, `ui_main.c`, `input.{c,h}`, new `scroll.{c,h}`, new `anim.{c,h}` | `theme.*`, `header.*`, `paint.*` |
| **B: look** | `theme.{c,h}`, `header.{c,h}`, new `paint.{c,h}`, fonts and their install rule, `tools/` font scripts | `list.*`, `screen_stack.*`, `ui_main.c`, `input.*`, `scroll.*` |
| **C: verification** | `tests/`, `scripts/`, `uisimulator/common/sim_script.c`, new host harness `tests/ipodui/` | `apps/` |

Shared edits to `apps/SOURCES` are expected; the orchestrator merges them.

## Contracts (fixed; implement exactly; additions OK if documented)

```c
/* paint.h (B implements; A calls; A may keep a TEMP stub in its worktree) */
/* Selection bar for one row at LCD coords; draws the 7G gradient
 * (#3A9BDC -> #226DCA, measured) in the current viewport's coordinates. */
void ui_paint_selection(int x, int y, int w, int h);
/* Chevron '>' glyph, right-aligned so its right edge is at x_right,
 * vertically centred in [y, y+h). 'on_selection' picks white vs list_fg. */
void ui_paint_chevron(int x_right, int y, int h, bool on_selection);
/* Width of the chevron glyph incl. its padding (for text truncation). */
int  ui_paint_chevron_w(void);

/* scroll.h (A implements; pure C: no lcd/kernel includes, so C can
 * compile it on the host). Units: pixels and ticks (HZ = 100). */
struct ui_scroll {
    int pos;        /* current top offset in px (rendered) */
    int target;     /* where we're heading */
    int vel;        /* px per tick, internal */
    int max;        /* max(0, content_h - view_h) */
};
void ui_scroll_init(struct ui_scroll *s, int max);
void ui_scroll_set_max(struct ui_scroll *s, int max);       /* clamps */
void ui_scroll_set_target(struct ui_scroll *s, int target); /* clamps */
/* Advance by dt ticks. Returns true while still moving. Must converge to
 * target in a bounded number of steps, never leave [0,max], and never
 * overshoot past the target by more than 0 px (7G has no bounce). */
bool ui_scroll_step(struct ui_scroll *s, int dt);
/* Wheel acceleration: raw clicks this event + wheel velocity (clicks/s,
 * from button_get_data) -> selection steps. Monotonic non-decreasing in
 * velocity; returns >= 1 for clicks >= 1. */
int  ui_wheel_steps(int clicks, unsigned velocity);
```

## Agent A: motion
- **Pixel-smooth list.**
  - Render rows into an offscreen strip: a `core_alloc` buffer of about view height plus 2 rows, RGB565, via `viewport_set_buffer`. Blit the visible window with `lcd_bitmap_part`, and push only the list band with `lcd_update_rect`.
  - Selection follows the wheel immediately. The *view* scrolls smoothly with `ui_scroll` to keep the selection visible. That's the 7G feel: the highlight jumps and the content glides.
- **Frame loop (`anim.{c,h}`).**
  - The frame budget is about 33 ms (`HZ/30`). Skip frames instead of falling behind.
  - `cpu_boost(true)` when an animation starts and `cpu_boost(false)` when the last one ends, with a balanced count.
  - Use `button_get_w_tmo(next_frame_ticks)` while animating, so input stays responsive.
- **Slide transitions:**
  - Push slides the new screen in from the right; pop slides back. About 200 ms, ease-out.
  - Only the area below the header moves; the header title just switches.
  - It must not allocate a second full framebuffer. Use the strip and a capture of the outgoing body, or document the approach and its RAM.
- **Scrollbar:**
  - It appears on the right edge while scrolling a list longer than the view, and fades or hides about 1 s after the motion stops.
  - Take its metrics from the references where available (MEASUREMENTS.md); otherwise mark them ≈.
- **Marquee:**
  - Applies when the selected row's text is wider than its space.
  - It starts after about 1 s, scrolls at a steady speed, pauses, and repeats.
  - It redraws only that row band.
  - It doesn't boost the CPU; at its low frame rate it isn't needed.
- **Wheel:** use `ui_wheel_steps` (contract above). Tune the curve so a slow turn moves one item per click and a fast spin covers about 100 items per second.
- **Hold lock latency:** fix the Phase 2 note. Use an event if the firmware sends one on a hold change; otherwise a cheap 1 s poll only while the backlight is on.

## Agent B: look
- **Fonts:**
  - Build the Heros fonts with `tools/convttf -A`: Bold 16 (list), Bold 13 (header and Now Playing secondary text), Regular 12 (small text).
  - Commit the generated `.fnt` files under `apps/ipodui/fonts/` with the GUST license, renamed so it's clear they're our build. Install them into `.rockbox/fonts/` through the normal `make install` and `make zip` path. Find where upstream installs fonts and add a guarded rule.
  - Load them in `ui_theme_init()` with `font_load` (buflib-backed), and fall back to `FONT_UI` if loading fails, with a `logf`.
  - Record the font RAM in the budget table.
- **Theme:**
  - The selection gradient as measured.
  - The header gradient should follow the reference's **non-linear** profile: sample `main.png` row by row. Store it as a small per-row colour table (20 entries, about 40 B), not a big bitmap.
  - Text colours as measured.
- **`paint.{c,h}`:** the selection bar, an original chevron drawn from the reference proportions (not copied pixels), and its width.
- **Header:** use `font_title` from the theme, now Heros Bold 13.

## Agent C: verification
- **Host harness `tests/ipodui/`:** compiles `apps/ipodui/scroll.c` on the host with gcc-16 and tests the contract:
  - convergence within a bounded number of steps;
  - bounds are respected;
  - no overshoot;
  - `set_max` clamps;
  - `ui_wheel_steps` is monotonic and ≥ 1.
  
  Also test the pure list maths if A exposes it (`ui_list_move` / clamp). Hook it into `scripts/test.sh`. Until A's `scroll.c` exists, use a trivial local reference implementation, which is thrown away at merge.
- **Animation trace assertions in `scripts/validate.sh`.** A logs, through `UPDATE_TRACE`, `anim start`, `anim frame <ms>` and `anim end` with the boost state. Assert that:
  - boost on and off are balanced by the end of each script;
  - no frame update is wider than the list band during a scroll;
  - frame intervals are at most 50 ms on average. In the simulator this is only a sanity check.
- **New UI scripts:**
  - `ipodui-p3-scroll.script`: a long list (the Music menu plus a Settings stub with about 30 items), slow and fast wheel turns, with shots mid-scroll and at rest, and the scrollbar visible;
  - `ipodui-p3-transition.script`: a shot during a push slide and after it;
  - `ipodui-p3-marquee.script`: a long-title row selected, with shots at 0 s and about 2 s.
  
  Keep shots deterministic, using fixed waits, and say which shots are timing-sensitive so they're kept out of goldens.
- **Font sanity check:** a tiny script that dumps the metrics of the installed `.fnt` files (reuse `out/fonts/fntinfo.py` ideas) and fails if a font is missing from `build/sim-ui/simdisk/.rockbox/fonts/`.

## Acceptance (orchestrator, after merge)
1. `scripts/validate.sh` passes, including the new host tests and trace assertions.
2. The Phase 2 goldens will change because of the fonts and gradient. Regenerate them only after a visual review against `docs/reference/7g/` with a side-by-side, and note the review in the commit.
3. The budget-reviewer passes. Expected costs:
   - strip buffer about 190 KB, from `core_alloc` and not static;
   - fonts under 100 KB;
   - statics: 4 KB budget in total for the shell.
4. Measured pixel differences against `main.png`: the header and selection bar are now within a 24-per-channel tolerance on more than 90% of pixels.
