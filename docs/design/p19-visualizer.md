# Package: Now Playing visualizer (P19 follow-on, after 19-2)

*Status: planned, 2026-10-24 (owner). Depends on 19-2, the BCM2722 taking drawing jobs from the shell. A deliberate difference from the 7G, which had no visualizer: list it in `docs/design/changes-from-7g.md` when it lands.*

## Owner requirements (2026-10-24)
1. **Where:** in Now Playing, as one more view in the SELECT cycle (with the scrubber and the rating).
2. **Choosing a style:** a hold menu (a long SELECT, the shell's existing hold-menu pattern) switches between visualization methods. The choice is remembered.
3. **Screen:** while the visualizer is showing, the screen stays on: no backlight timeout, and no soft sleep entry while it is the top screen.
4. **Audiobooks never use the visualizer** (owner 2026-10-24). The visualizer view isn't offered in the audiobook Now Playing cycle, and on a charger an audiobook always gets the Clock face, whatever the setting. The shell already knows when the playing track is an audiobook (the audiobook Now Playing and resume), so this costs only one check.
5. **Plugged in and playing:** the "screensaver" or lock-screen face the iPod shows on a charger is a user choice between **Clock** (the default) and **Visualizer**. On battery nothing changes, so the default behaviour is untouched. **Where it's chosen (owner, 2026-10-25):** not in Settings but in the visualizer's hold list, under the styles: Bars, Mirror, then **While Charging**, which opens its own short list (Clock / Visualizer). Its choice, like a style's, goes back to the view. Storage (`ipodui_charge_face`) and behaviour are unchanged.
6. **Full screen (owner, 2026-10-25):** "The visualizer should be full screen, no title bar, and no playback counter." See below.

7. **Dark theme and five styles (owner, 2026-10-25):** the view (and the charger face) is dark, near-black with the bars and colours glowing on it; v1 offers five styles, VC first.

## Dark theme and the v1 styles (as built, 2026-10-25)
- **Colours:** background RGB(4, 6, 12); the bars a deep blue foot (16, 72, 216) to an ice-blue top (134, 230, 255), white caps, each bar in a halo one pixel wider each side and one row taller, a third lit (GLOW_W 96/256). The volume overlay over the bars' foot is dark too (the background, light speakers, the usual bar).
- **The five (hold list order):** Bars (the default), Mirror, Scope, Spectrogram, Radial; then While Charging. Of the four candidates beyond Scope, **Spectrogram** and **Radial** were picked: both come from the bands the view already has, need no cover pixels and no sync-time data, and Spectrogram runs on the VC today with the core ops. Cover needs a scaled BLIT (or cached scale steps of the cover) and Aura a dominant-colour table at sync time, so both stay hidden for later (`viz_core.h` VIZ_STYLES_SHOWN). The setting's values: 0 Bars, 1 Mirror, 2 Scope, 3 Spectrogram, 4 Radial (5 Cover and 6 Aura were never selectable).
- **Who draws (VC first):**

| Style | On the BCM | Jobs a frame | Until then |
|---|---|---|---|
| Bars | yes | BARS (halo, with the background), BARS (bars, caps), PRESENT, FENCE | |
| Spectrogram | yes | COMMIT of a 320×3 row slot, SCROLL (+1-3 rows, 60 rows/s), BLIT of the new rows, PRESENT, FENCE; one FILL starts the history | |
| Mirror | yes, when `bcmjob_bars_flip()` (the overlay offers J_VPU_FLIP) | BARS (halo), BARS (bars) in 167 rows, the gap row's FILL, BARS with J_BARS_BG\|J_BARS_FLIP for the reflection (72 rows, or clip − 168), PRESENT, FENCE | the CPU, the same flipped draw (JVIZ_BARS_FLIP), logged `viz FALLBACK mirror on CPU why=noflip` |
| Scope | when a POLYLINE op ships | FILL, POLYLINE (glow), POLYLINE (core), PRESENT | the CPU (logged) |
| Radial | when a polar SEGMENTS op ships | FILL, SEGMENTS (48 rays), PRESENT | the CPU (logged) |

  The ops wanted were sent to the BCM session (2026-10-25); it is building POLYLINE (Scope, with a "y from slot samples" form), polar SEGMENTS (Radial) and PALROW (Spectrogram rows without the upload) on `p19-viz-ops`, each behind a capability bit, called through `bcmjob_viz_polyline/segments/palrow`. The shell switches a style to the BCM in `style_on_bcm()` once its call exists; until then the CPU draws it, logged. A CPU-drawn style logs `viz CPU FALLBACK why=style style=N fps=15` once a start; the analysis (SPECTRUM) stays on the BCM. A Spectrogram drawn as one FILL per band (24 jobs) timed the model out (2 ms per draw job), hence the row slot.
- **Scope:** the window's newest half across the panel, held still by a rising zero crossing (after a dip under -1024), a 3-px dim halo under a bright 2-px core, 88 px at full scale (the tap normalises the window). **Spectrogram:** newest rows at the top, the 24 bands as columns across the width, a five-stop heat ramp (background, navy, blue, cyan, white); its history lives in the frame and restarts at each start. **Radial:** 48 rays 7.5 degrees apart round a 34-px ring, the bands down the right and mirrored down the left (lows at the top), at most 78 px long, a white dot at each peak and a dim dotted ring.

## Full screen (as built, 2026-10-25)
- **The page:** the bars' rect is the whole panel, (0, 0) 320×240. No header (Now Playing sets `UI_HEADER_NONE` while the view shows, so the main loop draws and pushes none, and the battery, Hold and Time in Title redraws never land on it), no title, no progress bar and no time counters. The 24 bars (309 px) stay centred; a 12 px headroom keeps the loudest bar's cap off the top edge. Mirror's split scales with the height (bars 167 rows, a 1-row gap, a 72-row reflection).
- **The BCM path:** BARS into FRONT at (0, 0) 320×240, then PRESENT and FENCE, the rect owned while the view shows. Every row starts at x 0 with a 640-byte pitch, so each row is 32-byte aligned (memory-access.md 11.5). The CPU path draws the same rows into the framebuffer and pushes one full-width band per frame: 153,600 bytes at 15 fps (was 102,400 for the 160-row rect). Device: measure the push time at 15 fps.
- **The volume overlay:** the wheel still sets the volume. Now Playing's volume bar (2 s, `NP_VOLUME_SHOW_MS`) shows over the bars' foot (rows 198–239, white, the bar at its usual place), and **the header doesn't come back with it**: the owner's request is no title bar, and the overlay is a brief control, not a page. While it shows, the bars clip to rows 0–197 with the same geometry and colours (heights less the hidden 42 rows, and the gradient's foot colour taken from the full rect's row 42), and the BCM's owned rect shrinks to 198 rows before the CPU draws the foot (the last PRESENT waited). When it times out, the bars take the foot back at their next frame.
- **Leaving:** SELECT goes on to Normal (MENU as usual). On the way out the view stops (the BCM's rect handed back, its FENCE waited), `ui_fb_settle()` runs on the whole panel, and the header and the Normal page come back in one full redraw (`ui_invalidate(UI_DIRTY_ALL)`, so one `lcd_update`). The hold list (long SELECT) is a normal split list screen with its header; returning from it slides back to the headerless page.
- **The charger face** (While Charging: Visualizer) is the same full-screen view.
- **Scripts:** `ipodui-p19-viz` (bars, the volume overlay and its timeout, the five styles, back to Normal), `ipodui-p19-viz-face` (the hold list's While Charging, then the face). `# np: viz-full` tells `check_trace.py` that Now Playing's bands may cover the panel; `# reset: viz` starts a script from Bars and Clock.

## Design sketch
- **Samples:** PP1 taps the PCM after the DSP (the mixer hook Rockbox's oscilloscope and FFT plugins use) and decimates it to about 512 samples per frame. That's about 30 KB/s to the BCM2722.
- **Analysis on the BCM2722:** a fixed-point FFT (radix-2 or split-radix, 256–512 points) using the exact 16×16→32 vector MAC (x15 + vmulhd, decoded on p19-bcm-prep). Log-spaced bins become 24–32 bars, with peak hold and decay. Software fallback: the same FFT on PP1 (estimated 3–5 % CPU at 30 fps). It stays exact either way, so both paths draw identical frames.
- **Drawing:** the BCM2722's FILL/BLIT vector kernels, pushed as a screen region through the job ring (19-2, `J_PRES_WAIT` before any redraw of the region).
- **Sync:** each frame is keyed to the playback position (`pcmbuf` play pointer), not the decode position, so the picture follows what is heard.
- **Styles (first set):** surveyed in `p19-visualizer-styles.md` (open-source references, licences, shared pipeline, cost). Recommended v1 set, each a small parameter set over the same pipeline: **Bars** (classic, the default), **Mirror** (mirrored bars with a faded reflection), **Scope** (a triggered oscilloscope line), **Cover** (album art pulsing with bass and beat) and **Aura** (ambient blurred cover colours breathing with loudness). Later: spectrogram, radial, retro LED and VU, particles, XY, warp feedback.
- **Power:** lit only; the BCM2722 stays off in dark playback (P19 D-3). On a charger the screen-stays-on cost is fine. On battery, the visualizer view holds the screen on only while it is open, the same rule as video.

## Hardware features and state of the art (`docs/process.md` items 6 and 7)
- The BCM2722 vector unit carries the FFT and the drawing. PP1 only taps and decimates the samples. The CPU fallback is a full software path.
- State of the art: a fixed-point split-radix FFT with a windowed input (Hann), log-frequency binning and an exponential decay for the bars (the common music-visualizer pipeline). Cite `docs/algorithms-survey.md` when the brief is written.

## Tests
- Host: the FFT matches a double-precision reference within a tolerance, and bar values are identical on the PP and BCM paths.
- Sim: a UI script cycles the views, uses the hold menu to switch styles, checks the backlight stays on, and checks that the charger face honours the setting.
- Device: frame rate, sync against audio, and lit current with and without the visualizer.
