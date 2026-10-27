# Now Playing visualizer: styles survey

*Status: research, 2026-10-24. Companion to `p19-visualizer.md` (requirements, plumbing). Owner ask: look at open-source visualizer styles and offer a few.*

**Method and honesty note.** This is a desk survey from the Rockbox tree (read directly) and from knowledge of the other projects. No third-party source was downloaded or executed. Licences below are from memory of each project's published terms; **re-verify the exact licence file of any project before leaning on it for more than an idea.** We write our own code, clean-room. Algorithms (FFT, banding, exponential decay, energy-based beat detection) are not copyrightable; code and presets are.

## 1. What we have to work with

- 320x240 RGB565, ~30 fps, drawn by the BCM2722 (FILL/BLIT/keyed-BLIT/FADE/AVG kernels, 16-lane 16-bit vector unit, exact 16x16->32 MAC, ~1.25 MB SRAM). No shaders, no floats. PP5022 ARM7TDMI (no FPU) is the exact-equal fallback.
- Input per frame: ~512 PCM samples (stereo, after DSP) and a 256-512 point FFT.
- Look: iPod Classic 7G. Flat, soft, Helvetica-clean, a restrained palette. The 7G had no visualizer and no click-wheel iPod ever shipped one, so there is no Apple idiom to copy; we invent one in the 7G's language (the same blue-grey and white of its list and progress bar, soft rounded caps, gentle motion).

Rockbox in-tree references (all GPL-2.0-or-later, so even their code is licence-compatible, though they are written for a mono LCD and floating-free `rb->` plugin API and are not worth porting):

| File | What it does |
|---|---|
| `apps/plugins/oscilloscope.c` (Jens Arnold, 2004-06) | Waveform and graphical modes, scrolling or wrapping, from the PCM mixer tap |
| `apps/plugins/fft/` (Delyan Kratunov 2009; bundles KISS FFT, BSD-3-Clause) | Spectrum plot, linear/log axes, amplitude scale options, windows |
| `apps/plugins/vu_meter.c` (Lee Pilgrim 2003) | Analogue-style needle VU, peak LED |
| `plasma.c`, `fire.c`, `starfield.c`, `snow.c`, `matrix.c`, `fireworks.c`, `bubbles.c`, `lamp.c` | Non-audio-reactive eye candy (palette cycling, particles); useful only as ambient motion ideas |

## 2. Shared pipeline (one engine, many parameter sets)

All styles read the same per-frame struct, produced once on the BCM (or PP1 fallback), so a style is only a draw routine plus a few constants.

**Frame inputs**
- `pcm[512]` mono mix `(L+R)/2` (Q15), plus `L[]`, `R[]` for XY scope.
- `spec[N]` magnitudes, `bands[B]` smoothed band levels, `peaks[B]`, `bass`, `mid`, `treb` energies, `beat` (0..1 envelope), `rms`.

**Windowing and FFT**
- Take the last 512 samples; decimate nothing further (PCM is already ~44.1 kHz, so 512 samples = 11.6 ms; bin width 86 Hz).
- Hann window, Q15 table (512 entries, or 256 mirrored): `x[i] = (pcm[i] * w[i]) >> 15`.
- Real FFT of 512 points done as a 256-point complex FFT plus split step. Q15 radix-2 (or radix-4 on the vector unit) with **per-stage >>1 scaling** (block fixed-point, no overflow, ~-48 dB floor is plenty for a display). Twiddles Q15. Magnitude by `|re|+|im|` max-plus-half-min approximation (error <4 %, display-only; both paths use the same integer formula so they stay bit-identical) or an integer sqrt table if exactness against a reference is wanted.
- 11.6 ms resolution is poor below ~200 Hz (bins 0-2). Options: (a) accept it and give the bass bars fewer, wider bands; (b) use a 1024 window (23 ms, 43 Hz bins) updated every frame by sliding 512. **Recommend 1024-sample window at 512 hop**: costs ~2.2x the FFT but gives usable bass bars; still ~1-2 % ARM at 30 fps. Decide on the first measurement.

**Banding (log / octave)**
- 24 bands for the mobile layout, 32 optional, spaced 1/3 octave from ~50 Hz to ~16 kHz (`f_k = 50 * 2^(k/3.6)`; tune so the last edge is at 16 kHz). Precompute `lo[k], hi[k]` bin edges at build time. Band level = the **max** of bins in range for narrow bands (keeps peaks crisp) and the **power mean** for wide bands.
- Perceptual scaling: `level = clamp((20*log10(mag) + 60) / 60)` via a 256-entry dB table on the 8-bit-compressed magnitude, then a gentle pink-tilt of +3 dB/octave so highs are not dead. All table lookups, no floats.

**Smoothing, decay, peaks** (per frame at 30 fps; constants as shifts so the ARM can use them)
- Attack/release one-pole: `if (x > y) y += (x-y)*0.6 else y += (x-y)*0.18` (shift/add equivalents: >>1 + >>3, and >>3 + >>5 ). Gives the "bounce" without jitter.
- Gravity fall (cava-style) as an alternative to release: `v -= g; g += 0.5/frame^2`; reset on a new rise. Looks livelier than exponential; costs one more int per band.
- Neighbour smoothing, "monstercat" filter (cava): each bar is lifted to at least `neighbour / 1.5^distance` so one loud bar draws a gentle triangular halo; 2 passes over 24 bands, trivial. This is the single biggest "looks professional" step for the bar styles.
- Peak hold: a cap sits at the band's max, holds ~15 frames (0.5 s), then falls at 1 px/frame accelerating.
- Auto-gain: running max of `rms` with slow decay (~5 s), normalising the display so quiet audiobooks and loud masters both fill the picture. Essential, and also the audiobook story (voice-range emphasis on `speech` is not needed, just gain).

**Beat detection (cheap, two-stage)**
1. *Energy-based* (Patin): instantaneous bass energy `E = sum(bass bins^2)` vs a ~1 s history (43 frames) average `<E>` with a threshold `C = 1.3-1.5`; beat if `E > C*<E>` and at least ~250 ms since the last. Integer, 43-entry ring. Add a variance-adjusted `C = -0.0025714*var + 1.5142857` only if false triggers show up.
2. *Spectral flux* (Bello/Dixon): `flux = sum(max(0, mag_t[k] - mag_{t-1}[k]))` over the low ~1.5 kHz, adaptive median threshold. Better on busy mixes, costs one more array. Ship energy-based first; flux is a later upgrade.
- Output: `beat` envelope that jumps to 1 on detection and decays with a ~150 ms time constant, so styles animate off a smooth value rather than a trigger. Also `bass`, `mid`, `treb` as three smoothed band-group energies for art-reactive and ambient styles.

**Cost per frame, shared stage** (30 fps)
- ARM7 @ ~80 MHz boosted: window 512 mults ~5k cycles, 1024-pt real FFT ~60-90k cycles, magnitude+banding+smoothing ~15k: **about 100k cycles = ~1.3 ms, ~4 % CPU**. Matches the 3-5 % in `p19-visualizer.md`. Without cpu_boost it is higher; this is a lit-only feature so boost is acceptable.
- BCM vector: the butterflies map to 16-lane vmulhd passes; estimate a few tens of microseconds. Headroom is huge; the limit is the job-ring round trip, not compute.
- Memory (shared): Hann table 1 KB, twiddles 2 KB, FFT work buffer 1024x2x2 B = 4 KB, band tables ~0.2 KB, history ~0.5 KB. **~8 KB** all-in.

## 3. Candidate styles

Drawing cost is for the BCM FILL/BLIT kernels (a rect fill is one job-ring command); ARM costs assume a software rect fill at roughly 1-2 cycles per pixel on RGB565 in the framebuffer (~0.3-1 ms for a 150x100 region). Screen area for visualization: the full 320x240 (Now Playing view), or a ~320x150 region if the title lines stay visible. Memory is on top of the shared ~8 KB.

| # | Style | Looks like | References (licence) | Core algorithm | Fixed-point | Per-frame cost BCM / ARM | Extra mem | Rec. |
|---|---|---|---|---|---|---|---|---|
| 1 | **Bars** | 24-32 rounded vertical bars rising from a baseline, small floating peak caps, soft blue-white gradient | cava (MIT); audioMotion-analyzer (AGPL-3.0, idea only); Rockbox `fft` plugin (GPL-2+); XMMS/Audacious spectrum (GPL/BSD, verify) | log bands, monstercat filter, gravity fall, peak hold | trivially | 32 FILLs + 32 caps: ~70 jobs / ~0.5 ms | ~0.3 KB (bar state) | **v1** |
| 2 | **Mirrored bars** | Bars grow up and down from a centre line (or left/right from the middle), reflection fading 30 % below | cava "mirror" is commonly forked; audioMotion "reflex" mode (AGPL, idea only) | same bands, drawn twice; FADE kernel for the reflection | trivially | ~100 jobs / ~0.8 ms | ~0.3 KB | **v1** |
| 3 | **Oscilloscope** | One smooth antialiased-feeling line across the screen, soft glow, triggered so it holds still | Rockbox `oscilloscope.c` (GPL-2+); Audacious/XMMS "scope" (GPL); Winamp "Nullsoft Tiny Fullscreen" concept | rising-edge zero-cross trigger with hysteresis, then 320 samples decimated to one y per column; line = pixel columns joined by vertical runs | trivially | 320 column fills: ~320 jobs, or a vector line kernel; ARM ~1 ms | 0.7 KB | **v1** |
| 4 | **Cover pulse** | The album art centred, large, scaling up ~3-6 % and brightening with `bass`/`beat`; a soft blurred copy behind it tinted by `treb` | no direct code source; the "sound-reactive album art" idea is common (Spotify Canvas, Kodi skins, Plexamp, all proprietary or skin-licensed, ideas only) | `scale = 1 + 0.05*bass_env`, `bright = 1 + 0.2*beat`; BLIT with a stretch kernel, AVG/FADE for the glow | fixed-point scale in Q8; art already decoded for Now Playing | 1 stretched BLIT + 1 FADE ~ few jobs / ARM stretch of 140x140 art ~3-4 ms (**too slow, BCM strongly preferred**; fallback = brightness only, no scale) | art already resident | **v1** |
| 5 | **Ambient aura (blurred cover colours)** | Slow drifting soft colour blobs derived from the cover's dominant colours, intensity breathing with `rms`; very 7G/modern-Apple-Music "mesh" feel; legible title kept | concept: Apple Music's animated background (proprietary; idea only), Plexamp "Sonic" backdrop (proprietary) | at track change, reduce the cover to 4 dominant colours (done at sync/cache time, per project rule "pre-compute at sync"); per frame: 4 large pre-blurred sprites moved by Lissajous-ish offsets, keyed-BLIT + AVG into a 2x-down buffer, FADE to blend | positions via sine table; colours from a cached 4x RGB565 | ~6 jobs; ARM impossible at 30 fps full screen, fall back to a static gradient from the 4 colours | 4 sprites 80x80x2 B = 51 KB if rendered at runtime, or precomputed by the library tool | **v1** |
| 6 | **Radial spectrum** | Bars on a circle (ring of 48 rays), art or logo in the hole, rotates slowly | audioMotion radial (AGPL, idea only); projectM/Kodi similar; Rockbox none | polar placement from a 48-entry sin/cos Q15 table; each ray a thick line or rotated rect | table lookups, no trig | 48 line jobs; BCM needs a line or polygon kernel (not in the current list: **+1 kernel**), ARM ~2 ms with Bresenham | 0.4 KB | **later** |
| 7 | **Stereo XY / Lissajous** | A tilting glowing blob/figure tracing L vs R; phosphor persistence | Rockbox `oscilloscope.c` has no XY; Audacious "Goom"-style and `xxscope` ideas; analogue vectorscope tradition | plot `(L, R)` rotated 45 deg (`x=(L-R)/2`, `y=(L+R)/2`), 256 points, persistence by FADE of the previous frame | trivial | 256 plots + 1 FADE: ~260 jobs (batched into a point-list kernel preferred) / ~1 ms | 150 KB if full-res persistence buffer (320x240x2) or 38 KB at half-res | **later** (needs a point/line kernel; also dull on mono-ish audio, a poor fit for audiobooks) |
| 8 | **Spectrogram / waterfall** | Scrolling time-frequency heat map, vertical or horizontal | many; Audacity/Sonic Visualiser (GPL-2+); foobar2000 spectrogram components (proprietary) | column of dB-scaled bins per frame mapped through a palette LUT, scroll via BLIT | palette LUT, no maths | 1 column write + 1 scroll BLIT / ARM scroll of a 320x150 region ~2-3 ms | 96 KB if kept as bitmap (scrolled in place, no extra) | **later**, a power-user pick; reads as technical, not 7G |
| 9 | **VU needle meters** | Two skeuomorphic needle gauges, cream face, red zone | Rockbox `vu_meter.c` (GPL-2+); foobar2000 VU components (various) | RMS or quasi-peak integrator with ~300 ms (1 VU) rise and fall time constants; needle angle -> sprite choice | integer only | needle as a pre-rendered 64-frame sprite sheet per channel: 2 BLITs / trivial on ARM | 64 frames x 2 KB = ~130 KB sprites on storage, loaded on demand, or draw lines | **later**; a retro option, deliberately 1990s-hi-fi, which fits the 7G's aesthetic only if styled as a minimal dial |
| 10 | **LED segment bars** | Bars built from discrete segmented blocks, green-amber-red | audioMotion LED mode (AGPL, idea only); every hi-fi VU | same as Bars with quantised heights and a colour per row | trivial | ~200 small fills, or one keyed-BLIT of a mask over a gradient: ~34 jobs | 0.2 KB | **later** (retro option) |
| 11 | **Particle / starfield beat** | Soft dots flying out from the centre; speed and spawn rate scaled by `beat`/`bass` | Rockbox `starfield.c` (GPL-2+); xscreensaver (BSD-like, idea only); AVS "particle" concepts | integer 3D-to-2D projection with a reciprocal table, `z -= speed*(1+beat)`, respawn on exit | table reciprocal | 40-80 dots as small BLITs/fills; BCM 80 jobs / ARM 1 ms | ~1 KB | **later** |
| 12 | **Milkdrop/projectM-style feedback** | Warping, zooming, hue-shifting feedback of the previous frame under a moving waveform: the "psychedelic" look | projectM (LGPL-2.1+); Butterchurn (MIT) ; original MilkDrop (Nullsoft, licence changed to a Winamp-specific collaborative licence in 2024: **do not copy**); preset packs are mixed and often unlicensed: **never ship them** | previous frame sampled through a per-pixel warp map (a 2D grid of source offsets, bilinear), then FADE/decay, then draw new waveform on top | feasible: integer warp via a coarse (20x15) mesh precomputed per preset with bilinear interpolation of offsets; no equations evaluated per pixel | one warp pass ~ 76 800 pixels x BCM: needs a **mesh-warp blit kernel**, which does not exist; ARM ~25-40 ms, far over budget | 2 x 150 KB frame buffers | **later, only if a warp kernel is built**; a showpiece that sells the BCM, and the "reference engine" story |
| 13 | **Goom-like** | Fractal-ish trails and rotating plasma | goom2k4 (LGPL-2.1+), Kodi `visualization.goom` (GPL-2+) | zoom/rotation feedback plus line draws, like #12 | as #12 | as #12 | as #12 | **skip** (same kernel dependency, noisier look, not native) |
| 14 | **Palette plasma / fire / matrix** | Non-reactive colour-cycling eye candy | Rockbox plugins (GPL-2+) | palette cycling | trivial | cheap | small | **skip** (decorative, not reactive, 1990s feel) |

### Notes on specific sources

- **cava** (MIT, Karl Stavestrand and contributors): the reference for the bar post-processing. Its pipeline is: FFT, per-bar frequency cut-offs on a log scale, auto-sensitivity, "monstercat" neighbour filter, gravity fall-off, optional integral smoothing. We adopt the *ideas*; cava's code is MIT and could be adapted with attribution, but we do not need to.
- **Winamp AVS**: concept source for "superscope" (formula-driven scopes: tiny scripts evaluated per sample) and "dynamic movement" (the per-pixel warp, #12). Original source was released with the Winamp code in 2024 under a restrictive custom licence (not OSI, no derivatives): treat as read-never. Concept use is fine.
- **MilkDrop / projectM / Butterchurn**: per-pixel equations and a warp mesh in a shader. The tractable approximation is a coarse warp mesh plus a decay and a drawn waveform; per-vertex equation evaluation on a 24x18 mesh is cheap in fixed point (a few hundred ops), the expensive part is the 76 800-pixel warped blit, which needs a new BCM kernel. projectM LGPL-2.1+ is link-compatible with GPL-2+ and Butterchurn is MIT, but we would still write our own.
- **XMMS / Audacious**: XMMS and Audacious (BSD-2 core, plugins mixed GPL/BSD, verify each) ship "spectrum analyzer", "scope" and "VU" built-ins. Good for scope-triggering conventions; nothing we need to copy.
- **Kodi**: `visualization.spectrum` (GPL-2+), `visualization.goom` (GPL-2+), `visualization.waveform`. Concept parity with #1, #3.
- **foobar2000**: proprietary core; third-party spectrum and peak-meter components exist with various licences. Concept only; the *peak meter with RMS and true-peak* is a well-known layout (maybe a later "two-bar level meter" mini view).
- **audioMotion-analyzer** (Henrique Avila Vianna): **AGPL-3.0**. It is the best reference for octave bands (1/1 to 1/24), A/B/C/D/ITU weighting, LED, radial, reflex, and mirror modes. **Ideas only; do not copy code** (AGPL-3 imposes a network-use clause that is not compatible in spirit with a GPL-2.0-or-later firmware, and combining with GPL-2-only code is impossible).
- **iTunes "Magnetosphere"** (Apple, proprietary): a particle-field visualizer where Apple's visuals react to loudness and tempo (coloured glowing streaks). Concept: `beat`-driven spawn and bass-driven brightness. No code, and no Apple artwork. No iPod ever shipped a visualizer, so there is no "iPod visualizer" to clone; the closest family look is Apple Music's blurred-cover backdrop (our #5).
- **Beat detection literature**: Patin, "Beat Detection Algorithms" (GameDev.net, 2003); Bello et al., "A Tutorial on Onset Detection in Music Signals" (IEEE TSAP 2005); Dixon, "Onset Detection Revisited" (DAFx 2006). Algorithms, not code.

## 4. Recommended v1 set (five styles)

*As built (owner, 2026-10-25): Bars, Mirror, Scope, **Spectrogram** and **Radial**; Cover and Aura moved to later (they need the cover's pixels and a sync-time colour table). See `p19-visualizer.md`, "Dark theme and the v1 styles".*

Chosen for a native look and for every one being a small parameter set over the shared engine. The default is **Bars**.

1. **Bars** (classic): 24 soft-capped bars, monstercat smoothing, floating peak caps, 7G blue gradient. Cheapest, always correct.
2. **Mirrored bars** (classic variant): same bands drawn about a centre line with a faded reflection; the more modern-looking of the two.
3. **Oscilloscope** (scope): a single triggered waveform line, white on dark with a soft glow (FADE of the previous frame at ~25 % gives persistence at no extra cost).
4. **Cover pulse** (art-reactive): the album art breathes with `bass` and flashes subtly with `beat`; no scaling on the ARM fallback (brightness only), full scale on the BCM.
5. **Aura** (ambient): slow blurred colour fields from the cover's dominant colours, intensity from `rms`. Cheap and the most "Apple Music" of the set. Needs a dominant-colour extraction at library/sync time (per the pre-compute rule), or on first play and then cached.

This gives bars x2, a scope, art-reactive and ambient. A style picker (the hold menu) lists them in that order. Names are localised short labels: "Bars", "Mirror", "Scope", "Cover", "Aura".

## 5. Later set (in order of value)

1. **Spectrogram** (cheap, useful for audiobooks too, no new kernel).
2. **Radial spectrum** (needs a line kernel on the BCM).
3. **LED bars** and **VU needles**: deliberate retro options, grouped under a single "Retro" entry.
4. **Particle beat field**.
5. **Stereo XY** (needs a point/line list kernel).
6. **Warp feedback ("Milkdrop-lite")**: build only with a warp-blit kernel; a showcase of the BCM.

## 6. Engine and kernel needs implied

- The v1 set uses **only existing kernels** (FILL, BLIT, keyed BLIT, FADE, AVG) except: a **scaled BLIT** for Cover pulse (check whether the BCM blit kernel can stretch; otherwise pre-render 4-6 cached scale steps of the cover in BCM SRAM at track change; 6 steps of 140x140x2 B = 235 KB, fits 1.25 MB but is a large share; 4 steps at ~160 KB is a better trade).
- Later styles want one **line/point-list** kernel (radial, XY, scope with real lines) and one **mesh-warp** kernel (feedback).
- The frame struct is the contract; styles may not do their own FFT.
- All styles honour `docs/hardware.md`: partial updates (only the visualizer region), no static buffers (`core_alloc` at open, free at close), and nothing running when the screen is off (lit-only).

## 7. Licensing notes

- Our code is written from scratch under the ProkBox GPL-2.0-or-later header. Reference projects supply algorithms and look-and-feel ideas only.
- **Compatible if ever reused (still we do not plan to):** Rockbox plugins (GPL-2.0-or-later, our own upstream), KISS FFT (BSD-3-Clause), cava (MIT), Butterchurn (MIT), Kodi visualizers (GPL-2.0-or-later), projectM and Goom 2k4 (LGPL-2.1+).
- **Incompatible or restrictive, do not copy code, presets or data:** audioMotion-analyzer (AGPL-3.0), Winamp/AVS/MilkDrop original source (Winamp Collaborative Licence, 2024: custom, no derivatives), MilkDrop/projectM preset packs (many unlicensed or author-owned), foobar2000 and its components (proprietary), iTunes Magnetosphere and Apple Music visuals (proprietary; no Apple artwork or assets, rule 2).
- Verify per-file licences for XMMS/Audacious plugin code before any reuse (mixed GPL and BSD).
- Do not use the name "Magnetosphere", "MilkDrop" or "Winamp" for any style.

## 8. Open questions

- FFT window: 512 (11.6 ms, poor bass) or 1024 (23 ms) at 512 hop. Measure bass-bar quality and cost on device; default to 1024.
- Does the BCM blit kernel stretch? (Decides Cover pulse's quality; see section 6.)
- Where do dominant colours for Aura come from: sync-time tool (`scripts/prokbox-library.py`, the library pipeline) or first play and cache? Prefer sync time.
- Should audiobooks default to a different style (Scope or Aura, which look fine on speech) than music (Bars)? Owner call.
