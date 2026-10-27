> **Decision (owner, 2026-10-08): the "stock" variant.** It is tuned to the reference photo. The other variants are kept for the record only.

# Unlit clock screen: design prep (#51)

*Prep only (branch `prep-unlit`, 2026-10-07): art, mockups and a cost plan.
Nothing is wired into C yet. Spec: [`spec/system-screens.md`](spec/system-screens.md)
("Unlit clock screen"). ⚠️ marks a default chosen while the owner was away.*

When the backlight times out, the shell replaces the current screen with a
grayscale face that reads by ambient light: **the time in large block
digits**, and below it a **play or pause glyph** and a **battery icon**.
It updates once a minute, never boosts, and the LCD sleeps about 90 s later.

## Artwork

Generator: [`art/unlit/unlit.py`](../../art/unlit/unlit.py), stdlib only
(`python3 art/unlit/unlit.py [OUTDIR]`, about 30 s). Everything is drawn
procedurally from rectangles, rounded rectangles, hexagonal bars, a triangle and
circles. No font or reference pixels are used.

Output in [`unlit/`](unlit/):

- **[`contact.png`](unlit/contact.png)**: one band per variant (stock, slab,
  segment, rounded), 4 scenes across, the **lit** row above the **unlit** row.
  The 1-bit glyph sheets of all four variants are at the bottom, magnified 2x.
- `<variant>-<scene>-{lit,unlit}.png`: 320×240 exactly as the panel would show
  it. Glyphs are 1-bit planes thresholded at 50 %, and colours are quantised to
  RGB565.
- `<variant>-<scene>-{lit,unlit}@2x.png`: 640×480, drawn anti-aliased from the
  vectors, for inspection.
- `glyphs-<variant>.png`: the glyph set: digits 0–9, colon, A/P/M, play, pause,
  padlock, battery at 0/8/25/50/75/100 % and charging.

There are four scenes: `12h-playing` (4:01, 55 %), `24h-paused` (09:41, full),
`24h-stopped` (15:03, 8 %) and `12h-hold-charging` (12:58, Hold on, charging
45 %).

**Lit and unlit.** Once shown, the face stays up when the backlight comes back
on without a button press (Hold slid on, charger plugged in). That's why every
scene comes as a lit image (white paper) and an unlit image. The unlit
simulation maps paper to a warm light gray (182,177,165) and ink to (60,58,52),
so about a third of the contrast range is lost and a slight warm cast is added.
Those tones come from the owner's thumbnail (`unlit-clock.webp`).

### Variants

| Variant | Cell | Stroke | Ink | Character |
|---|---|---|---|---|
| **stock** (recommended ⚠️) | 30×68 | 9, corner radius 3 | dark gray 56, with a lighter 150 rim (1 px) | Tuned to the reference photo: condensed blocks at the photo's size and position, and a large triangle and battery |
| slab | 48×80 | 16, square | black | Solid squared blocks with square counters. The boldest; fills the middle third |
| segment | 48×80 | 13, hexagon-ended bars, gaps of 2 | black, with unlit bars as a pale ghost (200) | An LCD-watch look. The ghost almost vanishes unlit, which is intended |
| rounded | 48×80 | 14, radius 7 on the exposed corners | black | A softer block |

All four use the same construction: seven overlapping bars, unioned, so the
digits join solidly. "1" is a centred bar with a flag, and "4" has the open
top. The cells are monospaced, so a changed digit is redrawn in place. The colon
sits between fixed cells, and the whole time is centred (the width changes only
at 9→10 and 12→1, once an hour at most).

**12-hour mode** ⚠️: no AM/PM marker, and no leading zero ("4:01"), as in the
reference photo. The letters are generated anyway (`AMPM_SHOWN` in the
generator) in case the owner wants them. **24-hour mode** pads the hour with a
zero ("09:41"), like the owner's 7G.

**Status row** ⚠️: `[play|pause] [padlock if Hold] [battery]`, centred as a
group. When stopped, only the battery shows (and the padlock with Hold).
- The padlock follows the header's convention of a lock left of the battery, at
  the row's height.
- Charging knocks a bolt out of the fill (XOR): white over the fill, ink over
  the empty part, so it reads at any level.
- Low battery has no red. It's a sliver of fill (8 % = 3 px).

## Reference photo: measured layout

`docs/reference/7g/owner/unlit-clock-2.webp` (local only, never committed) shows
a black 7G at an angle, with the device rotated about 45° in frame.

**Method.** The four screen corners in the 1080×1440 photo are (702,712) TL,
(1013,1018) TR, (822,1282) BR and (477,897) BL, picked from the bezel edge. A
homography maps them to 320×240, and the photo was resampled to a rectified
640×480 with a 20 px grid (scratch only). In that frame, the screen's top edge
is the one away from the click wheel, and the time reads upright. Precision is
about ±3 px, because of glare across the right third and the soft focus.

| Element | Measured (screen px) | As a fraction | `stock` uses |
|---|---|---|---|
| Digit height | 62.5 → 130 (**≈ 67**) | 28 % of the height | 68, y 62–130 |
| Digit width ("4", "0") | ≈ 29 / 26 | — | 30 |
| Stroke | ≈ 7–9 | 11–13 % of the digit height | 9 |
| Time "4:01" extent | x 105 → ≈ 207, centre ≈ 156 | centred | centred |
| Colon dots | ≈ 9 px, at ≈ 0.42 and ≈ 0.9 of the digit height | — | 0.42 / 0.88 |
| Play triangle | x 90 → 121, y ≈ 159 → 197 (≈ 30 × 36) | — | 30×36 |
| Battery | x ≈ 161 → 222, y ≈ 162 → 197 (≈ 58 × 35 with the nub) | — | 54×32 + 4 px nub |
| Status row centre | y ≈ 179 | 75 % of the height | 179 |
| Triangle → battery gap | ≈ 40 | — | 38 |
| Row group centre | x ≈ 156 | centred | centred |

**Tones.** In the 5×5 means of the rectified photo, the paper is ≈ 120–128 and
the digit body ≈ 101–110, the triangle ≈ 114 and the battery fill ≈ 110. That's
dark gray on light gray, and contrast is low (ink ≈ 0.85 × paper, glare
included). The digits look slightly stylised, with a darker body and a lighter
edge. `stock` copies that with a 56 body and a 150 rim. The rest of the face is
grayscale only.

**What the photo settles, and what it doesn't.**
- Settled: the layout above; 12-hour without a marker; a play triangle and a
  battery with a partial fill, side by side and centred.
- Still unknown:
  - the exact digit design (the photo's digits are condensed, rounded sans
    shapes, which we deliberately don't copy);
  - whether the stock pause glyph and padlock look like this;
  - what shows when stopped.

**The first thumbnail** (`unlit-clock.webp`, 140 px) only gave the overall tone:
a light warm-gray panel with a mid-gray clock, centred a little above the
middle. That is what the unlit simulation uses.

## Cost plan

### Storage and drawing

What's linked:
- **`lcd_mono_bitmap(_part)`**: 1-bit, packed 8 pixels per byte vertically,
  `width × ceil(h/8)` bytes. It's in core, in `firmware/drivers/lcd-16bit-common.c`,
  and is drawn in the current foreground colour with `DRMODE_FG`.
- **`lcd_alpha_bitmap_part`**: 4-bit.
- **`lcd_fillrect`**.

| Variant | 1-bit planes (bytes) | Procedural (bytes of data) |
|---|---|---|
| stock (2 planes: rim + body) | digits 5 400, row glyphs 950 → **6 350** | ≈ **60**: a 10-byte segment mask, 7 rects × 4 B, and a 3-row corner span table |
| slab | digits 4 800, row glyphs 192 → 4 992 | **38**: segment masks and rects only |
| segment | 4 800 + ghost 480 + 192 → 5 472 | ≈ 50, plus per-row spans for the hex ends |
| rounded | 4 800 + 192 → 4 992 | ≈ 50, plus a 7-row corner table |
| 4-bit alpha (AA), any | 10 200 (stock) / 19 200 (48×80) | — |

**Recommendation: procedural drawing, with no bitmaps** ⚠️.
- Every bitmap set is over the 1 KB static limit, and as `const` data it would
  sit in the binary, which on this target means RAM.
- The digits are unions of at most 7 rectangles. `ui_unlit_digit(x, y, ch)`
  does this:
  1. `lcd_fillrect` each lit bar in the rim gray (stock), or in ink;
  2. fill the same bars inset by 1 px in the ink colour.
  
  Overlapping bars make "union of insets" equal "inset of the union". Rounded
  corners clear a few corner pixels from a 3-entry span table.
- Code ≈ 0.4–0.6 KB, including the triangle (per-row spans), pause bars,
  battery (rects), padlock (a small span table) and the XOR bolt.
- No `core_alloc` is needed. The face draws straight into the framebuffer.

### On screen

- **First show** (backlight-off event): clear the screen and draw ≈ 40 fillrects,
  then one full `lcd_update()`. The push is ≈ 16–20 ms at 30 MHz (the
  full-body push measured in `code-audit-1.md` / `phase5-brief.md`). Once per
  backlight-off, **no `cpu_boost`**.
- **Per minute**: redraw only the changed digit cells, then push one
  **full-width band over the digit rows**: 320×68 at y 62–129 (320×80 for the
  48×80 variants).
  - From the measured 24-row band ≈ 2 ms and 220-row body 13–20 ms, a 68-row
    band is ≈ **4–6 ms at 30 MHz**.
  - The framebuffer work is ≈ 2 040 px of clear plus ≤ 14 fillrects per digit,
    ≈ 0.1–0.2 ms (est.).
  - Usually one digit changes, at most four on the hour; it's the same band
    either way.
  - ⚠️ Device check (owner: "keep the per-minute redraw minimal"): compare a
    30×68 single-digit rect against the 320×68 band. `hardware.md` says
    full-width bands burst fastest, but the narrow rect pushes 10× fewer pixels.
    Pick whichever is cheaper on the BCM.
- **Status row** (y 161–197, 320×36 ≈ 3 ms): pushed only when the play state,
  the battery level (polled at the minute tick), Hold or charging changes.
- **While the LCD sleeps**, `lcd_update_rect()` returns immediately
  (`lcd-video.c`: `if (!lcd_state.display_on) return;`).
  - The minute tick keeps the **framebuffer** current, at ≈ 0.2 ms/min and no
    bus traffic.
  - `lcd_awake()` pushes the framebuffer itself, so the face appears up to date
    the moment the panel wakes, with no stale minute ⚠️.
  - The alternative, no tick at all while asleep, saves one wake a minute. That
    is negligible next to the 100 Hz kernel tick, and it would flash a stale
    time on wake.
- **Timing**: the shell's loop timeout becomes "ms to the next minute boundary"
  from `get_time()`. There are no seconds, no animation and no boost.

### Trigger: the backlight-off event

Today the shell only polls `is_backlight_on(true)` (`ui_main.c`), so it would
notice late. Rockbox has no backlight-off event. There are two options:

1. **Hook (recommended)** ⚠️: in `firmware/backlight.c` `do_backlight_off()`,
   add `#ifdef HAVE_IPOD_UI`, then post one button-queue system event (a new
   `SYS_IPODUI_BACKLIGHT_OFF`). That's 3 guarded lines. The shell draws the face
   as the fade-out starts.
2. **No upstream change:** the shell computes the deadline itself (last input
   tick + `backlight_timeout`, or `_plugged` on the charger) and uses it as its
   loop timeout. It's fragile, because anything else that calls `backlight_on()`
   moves the deadline.

### LCD sleep at 90 s

- Setting: `lcd_sleep_after_backlight_off` (`apps/settings_list.c:2175`, under
  `HAVE_LCD_SLEEP_SETTING`, which ipodvideo defines along with `HAVE_LCD_SLEEP`
  and `HAVE_LCD_SHUTDOWN`).
- Values come from `timeout_sec_common`, which already contains 90.
- **The current default is 5 s.** The `firmware/backlight.c` static starts at
  `10*HZ` until the settings are applied.
- Change: `#ifdef HAVE_IPOD_UI` default **90** in that `TABLE_SETTING_LIST`, a
  one-line guarded edit. The setting stays hidden (it isn't a 7G setting).
- The firmware runs the countdown itself (`backlight_lcd_sleep_countdown`, then
  `lcd_sleep()` → BCM power-down), so the shell does nothing.

### Wake and dismiss: the state machine

States: **NORMAL**, **FACE** (panel on; the backlight may be off *or on*) and
**FACE_ASLEEP** (BCM off; the framebuffer still holds the face).

| From | Event | To | Action |
|---|---|---|---|
| NORMAL | backlight-off event (not in USB mode) | FACE | Remember nothing extra: the screen stack is intact. Draw the face and push it in full |
| FACE | minute tick | FACE | Redraw the changed digits and push the band |
| FACE | play/pause/stop, battery step, charger in or out, Hold on/off *while still on* | FACE | Redraw and push the status row (padlock shown while Hold is on) |
| FACE | backlight comes **on** without a button (Hold slid **on**, charger plugged in) | FACE (lit) | Nothing: the same art, now lit. The backlight times out again later, and the LCD sleeps 90 s after that |
| FACE | LCD sleep (90 s after the backlight goes off) | FACE_ASLEEP | Nothing (the minute tick still updates the framebuffer) |
| FACE_ASLEEP | backlight on without a button (Hold on, charger) | FACE (lit) | `lcd_awake()` pushes the framebuffer, which is the current face |
| FACE / FACE_ASLEEP | **any button** (press, wheel touch or turn) | NORMAL | **Consumed** ⚠️: the press and its repeats and release are swallowed. Redraw the previous screen in full (`ui_invalidate(UI_DIRTY_ALL)`). The backlight comes on |
| FACE / FACE_ASLEEP | **Hold slid off** | NORMAL | As for a button: restore the previous screen |
| any | USB connected | (USB screen) | Leave the face first (state → NORMAL). The USB screen draws over it and keeps its own dark-time behaviour (spec: its animation stops while dark). The face is **never** shown over the USB screen ⚠️ |
| any | alarm / sleep timer / shutdown | NORMAL | The event's own screen wins |

Notes:
- **Swallowing in the shell, not with `bl_filter_first_keypress`.** That
  setting's default is off, so leave it off. It only filters while the
  backlight is off (`button.c` `filter_first_keypress_enabled`), but the face
  can be lit (Hold or charger) and must still swallow the dismissing press. The
  shell does it in `input.c`: while FACE, the first button event dismisses, and
  everything up to that button's release is dropped.
- **The Hold path needs a hook.** In stock Rockbox, `backlight_hold_changed()`
  lights the backlight only when Hold goes **off** (or with the "backlight on
  button hold" override, which keeps it on permanently). For "Hold slid on →
  the face appears lit", add an `#ifdef HAVE_IPOD_UI` that also posts
  `BACKLIGHT_ON` when Hold goes on. That's a one-condition guarded edit, next to
  the backlight-off hook above. While Hold is on, the button driver drops every
  key, so only Hold off dismisses.
- **Charger**: `SYS_CHARGER_CONNECTED` already runs `backlight_update_state()`
  in the backlight thread. Whether that lights the panel on this target must be
  checked in the simulator or on the device.
- **Wake flash**: a button press while FACE_ASLEEP calls `backlight_on()` →
  `lcd_awake()`, which pushes the framebuffer (the face) before the shell
  redraws the previous screen. So the face may show for one frame (≈ 16 ms)
  while the backlight is still fading in. ⚠️ Accept it, or have the shell
  repaint the previous screen into the framebuffer on the dismissing press
  before the backlight thread wakes the LCD (that's racy). Check on the device.
- **Threads**: everything is drawn on the shell thread. The two hooks only
  post events (no shared buffers).

## Device-reality checklist (prep)

- **Compiled features**: `HAVE_LCD_SLEEP`, `HAVE_LCD_SLEEP_SETTING`,
  `HAVE_BACKLIGHT` and `lcd_mono_bitmap` are present on ipodvideo.
  Procedural drawing needs none of the bitmap paths.
- **Memory**: 0 B of heap and about 60 B of const data, plus about 0.5 KB of
  code. No second framebuffer: the previous screen is redrawn from state.
- **CPU**: never boosts. About 5 ms a minute while the panel is awake (at most
  about 2 pushes before the LCD sleeps at 90 s), then about 0.2 ms a minute of
  framebuffer work.
- **Storage**: no disk access at all. It reads the time, `battery_level()` and
  `audio_status()`.
- **LCD**: one full push, then band pushes only.
- **Splashes and strings**: none (the face has no text).
- **Persistence**: none.
- **Upstream**: 3 guarded hooks (backlight off → event; Hold on → backlight on;
  LCD-sleep default 90 s). The stock build is unchanged.
- **Device-only checks**:
  - band versus narrow-rect push cost;
  - real unlit readability of each variant;
  - the one-frame wake flash;
  - charger → backlight.
