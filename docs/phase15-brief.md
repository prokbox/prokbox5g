# Brief: Phase 15, the DAC: hardware EQ, idle power-down, fades, volume curve

*Orchestrator brief, drafted 2026-10-11 against `p9-unified` `21b48a8546` (on the device as `99c9a3781c` plus docs). Branch `p15-dac`, worktree `.claude/worktrees/p15-dac`. Owner, 2026-10-11: "Let's do those DAC improvements, and we should do them before the extras." Scheduled right after Phase 9 closes and before 8b and Extras (ROADMAP order 11a). Process: `docs/process.md` (brief template, checklist 1–13, lean rules 1–11, one gate per machine). Open owner questions are frozen as ⚠️ with a default (§9); the brief is not revised mid-phase for them. **Owner pre-flight done:** the four DAC ideas were offered on 2026-10-11 and the owner chose all of them ("Let's do those DAC improvements"); no captures are needed (no visual change beyond the volume bar's fill).*

*Revision 1 (2026-10-11, brief review: 6 High, 7 Medium, 5 Low). Every finding is applied inline; the table at the end maps them. Main changes: the EQ goes through a private codec API (`firmware/export/dac_ext.h`, committed with stubs), not upstream's `EQ_CAP` path; standby keys off `!pcm_is_playing()` with the wake hook in `pcm_mixer.c` outside `pcm_play_lock`; the skip/scrub de-click is deferred; the volume table is 48 integer-dB steps.*

**Goal.** Make the headphone output cheaper and nicer without changing what the owner hears unless it is an improvement:
1. **Hardware EQ.** The Settings › EQ presets run on the WM8758's own 5-band EQ instead of Rockbox's 10-band software EQ, wherever the hardware can match the preset closely. The CPU cost of the EQ drops to zero.
2. **DAC idle power-down.** While paused or stopped, the codec's DAC, mixers, headphone amp and PLL power down after a short delay and come back without a pop.
3. **Fades.** Pause and resume fade on a perceptual curve. Today's ramp is linear in amplitude: it stays loud until near the end, then drops away, so a pause sounds like a late cut. (A skip/scrub de-click is **deferred** to a follow-up: at HZ=100 the tick can't ramp 15 ms smoothly and the flush points sit in A's file; review H5.)
4. **Volume curve.** The wheel and the volume bar follow loudness, not raw dB. Today one click is 1 dB across −90…+6 dB and the bar is linear in dB, so the bottom half of the bar is all but silent.

**The WM8758's EQ today:** 5 bands (EQ1 low shelf, EQ2–4 peaks, EQ5 high shelf). Stock Rockbox uses only EQ1 and EQ5, as the Bass and Treble settings (default 0 dB, so flat); EQ2–4 are never used. The shell hides Bass and Treble, so all five are idle and every EQ preset runs in software.

**Split by risk.** The EQ and the power-down touch the codec driver (`firmware/`), which needs device proof. The fade and volume work is logic in `apps/`, provable in host tests and the sim. Each has its own agent and its own device check (§7).

---

## 0. What already exists (read; reuse it, do not rebuild)

| Fact | Where | P15 does |
|---|---|---|
| **The codec** is a Wolfson **WM8758** (`HAVE_WM8758`, `firmware/export/config/ipodvideo.h` :121), the **clock master** (`CLKCTRL_MS`): its PLL makes BCLK/LRCLK for the PP5022's I2S. The PLL and the clocks keep running while paused | `firmware/drivers/audio/wm8758.c` | Powered down in standby (§3.2) |
| **Its 5-band EQ** (EQ1 low shelf, EQ2–4 peaks with narrow/wide, EQ5 high shelf; ±12 dB in 1 dB steps; 4 frequencies per band) is used **only as bass and treble** (EQ1, EQ5 via `audiohw_set_bass/treble*`). A comment in `wm8758.h` :170 says the 3 peaking filters and 3D "will work when M128ENB is enabled + proper code" (`ADDCTRL_M128ENB`, bit 8 of `ADDCTRL`) | `wm8758.c` :186–208, `wm8758.h` :164–176 | All 5 bands driven (§3.1). **The M128ENB claim is unproven: device spike S1 first** |
| **Upstream's hardware-EQ API** (`AUDIOHW_HAVE_EQ`, `audiohw_set_eq_band_gain/frequency/width`, `AUDIOHW_EQ_BAND_CAPS`), implemented for the **WM8978**, a sibling with the same EQ block | `firmware/export/audiohw.h` :274–300, :525–560; `firmware/drivers/audio/wm8978.c` :368–400, :488–492; `firmware/export/wm8978.h` :26–59 | Reused: the WM8758 gets the same API under `HAVE_IPOD_UI` |
| **Settings › EQ** (P11): Off + the 22 iTunes presets as 10-band gains, applied through `dsp_set_eq_coefs()` / `dsp_eq_enable()` with a precut equal to the largest boost | `apps/ipodui/screens/eq.c` | Gains a hardware path per preset (§3.1); the software path stays for presets the hardware can't match |
| **Volume** steps `sound_steps(SOUND_VOLUME)` = 1 dB over −90…+6 (`AUDIOHW_SETTING(VOLUME, "dB", 0, 1, -90, 6, -25)`). `np_pb_volume_step()` → `adjust_volume()` (`apps/misc.c` :1137, `VOLUME_ADJUST_DIRECT` unless the perceptual mode is set). The bar is `(volume − min) / (max − min)` (`np_playback.c` :614–624); Volume Limit uses the same linear mapping in 1/1024 (`vlimit.c`, `np_playback.c` :627) | as listed | Both move to one table (§3.4) |
| **Upstream's perceptual volume** (`HAVE_PERCEPTUAL_VOLUME`, on for every target; `set_normalized_volume()`/`get_normalized_volume()` over a `norm_tab` built from `volume_adjust_norm_steps` and the limit) | `apps/misc.c` :1040–1135, `apps/settings_list.c` :1488 | **Evaluated, not used as is**: its curve shape is a log-ish map with a user step count. P15's table is its own (§3.4) and replaces it in the shell; upstream's stays for stock |
| **Hardware volume split**: above −57 dB the DAC sits at 0 dB and the headphone amp (OUT1, 1 dB steps, zero-cross) attenuates; below that the amp is at −57 and the DAC attenuates in 0.5 dB steps | `wm8758.c` :41–82, :138–156 | Kept. P15 adds the EQ precut on the DAC (§3.1) |
| **Pause/resume fade** already exists: `pcmbuf_fade()` ramps the mixer amplitude **linearly** over ~1/3 s from the 100 Hz tick (`pcmbuf_fade_tick`), on pause, resume and stop when `fade_on_stop` is on (default on, hidden in the shell) | `apps/pcmbuf.c` :1337–1395, `apps/playback.c` :3360, :3423 | Curve and length change (§3.3); the mechanism stays |
| **Shutdown** already has a careful anti-pop sequence (soft mute, VMIDTOG discharge, power off) | `audiohw_close()`, `wm8758.c` :210–228 | Untouched. Standby is a lighter state that keeps VMID charged (§3.2) |
| **The clicker** can play through the headphones: `keyclick_click()` → the PCM beep mixer channel when Settings › Clicker includes headphones (`keyclick_hardware` off). The piezo/haptic path doesn't use the DAC | `apps/ipodui/input.c` :38–65, `settings.c` :159–168 | A click must wake the DAC from standby (§3.2) |
| **Sound Check** (ReplayGain track mode) is in the DSP, before the EQ | `apps/ipodui/screens/settings.c` :143–152 | Unchanged |
| **Upstream's prescaler** (`TONE_PRESCALER`, the WM8758's bass/treble caps): `set_prescaled_volume()` recomputes a prescale from bass/treble/`eq_gain[]` on every volume change, calls `audiohw_set_prescaler()` (a no-op stub here, `audiohw-swcodec.c` :53–63) and adds it to the volume | `firmware/sound.c` :237–279 | **Not used for the EQ precut** (review H1): with bass/treble held at 0 it stays 0 and the stub stays a no-op. The precut lives inside `wm8758.c` (§2.1) |
| **PCM stop and the mixer's idle tail**: the mixer keeps DMA running with silence for 3 s after the last channel stops (`pcm_mixer.c` :84–87, :247–255); `sink_stop_pcm()` then spins on `IIS_TX_IS_EMPTY`, from FIQ (`pcm-pp.c` :466–471, `pcm.c` :152–161). With the codec as clock master, a PLL off while DMA runs would hang that loop | as listed | Standby only when `!pcm_is_playing()` (§3.2) |
| **`ADDCTRL` is rewritten whole** by `audiohw_set_frequency()` on every sample-rate change (`wm8758.c` :274–275); 44.1 kHz runs with `ADDCTRL_SR_48kHz` | as listed | Shadowed so `M128ENB` survives (§3.1) |
| **The audio thread blocks without a timeout** when idle (`STATE_IDLE`/`ENDED`: `queue_wait()`, `playback.c` :3945; active states use `queue_wait_w_tmo`, :3938) | as listed | A timed wait only while a standby is pending (§3.2) |

---

## 1. Out of scope
- Line-out (OUT2) and the dock: already off without `HAVE_IPOD_DOCK30` (P9).
- Bass, treble, 3D, the limiter and the notch filter as user settings. Stock 7G has none; they stay developer-only (and are forced neutral while a hardware preset runs, §3.1).
- Recording and the ADC.
- Changing the sample-rate or PLL plan, or DAC oversampling (`DACOSR128` stays on: fidelity first).
- Crossfade between tracks (a stock 7G doesn't have it).

---

## 2. Contracts (committed to `p15-dac` before fan-out, with host tests and one shared stub)

### 2.1 `firmware/export/dac_ext.h` (new, committed with this revision)
A private codec API under `HAVE_IPOD_UI`, **not** upstream's `EQ_CAP` (review H1, option B): no `settings_list.c` hardware-EQ entries, no `global_settings.hw_eq_bands`, no plugin-API field, no new `sound.c` code, and `wm8758.h`'s `AUDIOHW_CAPS` stays exactly as it is.
```c
struct dac_eq_band { int8_t gain; uint8_t freq; uint8_t wide; };  /* dB, 0..3, 0/1 */
void dac_eq_set(const struct dac_eq_band *band, int precut_db);   /* NULL = off */
void dac_eq_set_m128(bool on);                                    /* S1 only */
#define DAC_IDLE_MS 5000                                          /* ⚠️ Q1 */
void dac_idle_poll(void);      /* audio thread */
bool dac_idle_pending(void);   /* audio thread: needs a timed wait? */
void dac_wake(void);           /* before any PCM start, outside pcm_play_lock */
struct dac_stats { bool standby, hw_eq, m128; unsigned sleeps, wakes,
                   last_wake_us, eq_writes; };
void dac_get_stats(struct dac_stats *s);
```
- **Stubs are committed with the contract:** `wm8758.c` (no-ops) and the simulator's `sdl.c` (keeps the state, so the engine choice and the standby timer run in the sim). A replaces the `wm8758.c` stubs; nobody writes their own.
- **Precut:** whole dB of DAC digital attenuation inside `wm8758.c`, applied in `audiohw_set_volume()` on top of the volume split, **no make-up gain**. The level drops by the precut exactly as the software EQ's precut does today, so switching engines changes nothing the listener hears except the curve. `sound.c`'s prescaler isn't involved (it stays 0).
- **EQ1/EQ5 ownership, both directions:** while the hardware EQ is on, `audiohw_set_bass/treble*` only update their shadows; while it is off, the bands are flat and bass/treble own EQ1/EQ5 as today. `sound_settings_apply()` can't override a hardware preset.
- **Firmware can't trace** (`UPDATE_TRACE` lives in `apps/ipodui/anim.h`): the shell reads `dac_get_stats()` and logs it (review M3). The S1 register log is a small ring in `wm8758.c` that the developer item copies to `ipodui.log`.

### 2.2 `apps/ipodui/eq_hw.h` (new): the preset table format
```c
struct eq_hw_band { int8_t gain; uint8_t freq; uint8_t wide; };  /* dB, 0..3, 0/1 */
struct eq_hw_preset {
    struct eq_hw_band band[5];
    uint8_t precut;     /* whole dB, applied on the DAC (dac_eq_set) */
    uint8_t maxerr;     /* 0.1 dB: worst deviation from the iTunes curve, 20 Hz..20 kHz */
};
/* Generated by tools/eqfit (host), indexed like presets[] in screens/eq.c;
 * a preset with maxerr > EQ_HW_MAXERR stays on the software EQ */
extern const struct eq_hw_preset eq_hw_presets[22];
#define EQ_HW_MAXERR 15   /* ⚠️ 1.5 dB, §9 Q4 */
```

### 2.3 `apps/ipodui/volcurve.h` (new): one volume table for the wheel, the bar and the limit
```c
#define VOL_STEPS 48            /* ⚠️ §9 Q2: positions 0 (mute) .. 47 (+6 dB) */
int vol_step_to_db(int step);   /* whole dB (SOUND_VOLUME has 0 decimals); step 0 = mute */
int vol_db_to_step(int db);     /* nearest step at or below */
int vol_step_to_bar1024(int step);  /* the bar and Volume Limit: linear in step */
```

### 2.4 Trace lines (`UPDATE_TRACE`, rules in `scripts/check_trace.py`; a rule that finds none of its lines fails)
- `eq engine=hw|sw preset=<n> maxerr=<0.1dB> precut=<dB>`: a **separate** line after the existing `eq preset=` line, whose format `tests/ui/ipodui-p11-eq.script` :7–8 checks and which stays unchanged (review M5)
- `dac standby=<0|1> sleeps=<n> wakes=<n> wake_us=<n>`: logged by the shell from `dac_get_stats()` when the count changes (main loop, existing timeout)
- `vol step=<n> db=<v> bar=<0..1024>`
- `fade dir=in|out steps=<n>`

---

## 3. Work items

### 3.1 Hardware EQ (agents A + B)
- **S1, device spike first (agent A, ≤ 1 day).** A developer-menu item, "EQ test", that sets **EQ3 to +12 dB at 1.1 kHz** (wide; only EQ3 has 1.1 kHz, review M6) with a 12 dB precut, then flat, then EQ2 at 385 Hz and EQ4 at 2.4 kHz the same way, then +12 dB on EQ1 and EQ5, three seconds each, copying the register ring to `ipodui.log`. **The owner plays a 48 kHz track for one round as well,** proving `M128ENB` survives a rate change (review H6). The owner plays a track and reports whether the mid boost is audible (proves the peaks work, with or without `M128ENB`). It tries `M128ENB` off first, then on, and logs which state was set. **If the peaks don't work in either state, hardware EQ is limited to the presets the two shelves can match, and the rest stay software.**
- **Driver (agent A):** the WM8978 pattern behind `dac_ext.h`. Shadow registers per band and for `ADDCTRL` (so `audiohw_set_frequency()` ORs `M128ENB` back in, review H6) and `PWRMGMT1–3` (review M7: `audiohw_enable_lineout()` writes PWRMGMT3 whole). `EQ1_EQ3DMODE` stays set (EQ on the DAC path, written before DAC enable as the WM8978 notes). Precut as §2.1. **A confirms from the WM8758/WM8983 datasheet that the EQ sits after the DAC digital volume** (or the precut can't stop clipping inside the codec; if it sits before, the precut moves to the DSP's `dsp_set_eq_precut()`, which costs nothing per sample when the EQ itself is off) and records `M128ENB`'s extra playback current from the datasheet, so its cost is netted against the CPU saving (review M6).
- **Fit tool (agent B):** `tools/eqfit/eqfit.c`, host C, no deps. It models Rockbox's 10-band software EQ (the same RBJ biquads `dsp_set_eq_coefs` builds, low shelf / 8 peaks / high shelf at the iTunes frequencies) and the WM8758's 5 bands (shelf and peak biquads at the datasheet frequencies, narrow/wide Q from the WM8978 datasheet table; the model's assumptions are written at the top of the file). **The datasheet frequencies are for 48 kHz; at 44.1 kHz the codec runs with `ADDCTRL_SR_48kHz`, so every corner scales by 44.1/48 = 0.919** (review M6). The fit is made at 44.1 kHz (most of the library) and the tool also prints each preset's error at 48 kHz; `maxerr` is the worse of the two. For each preset it searches gain × frequency × width (exhaustive: 25 gains per band is small) to minimise the worst deviation over 20 Hz–20 kHz on a 1/12-octave grid, then writes `apps/ipodui/eq_hw_presets.c` (generated; header comment says so and how to regenerate) and prints the error table. **The generated file is committed;** the build never runs the tool.
- **Engine choice (agent B, `screens/eq.c`):** apply = if `eq_hw_presets[i].maxerr ≤ EQ_HW_MAXERR` and S1 passed (a compile-time `IPODUI_HW_EQ` flag the orchestrator sets from S1's result), disable the DSP EQ and call `dac_eq_set()`; else today's software path with `dac_eq_set(NULL, 0)`. Off = both flat. One path writes each, so there is never a double EQ. **A hardware preset saves `eq_enabled=false`** (so `settings_apply` can't bring the DSP EQ back), and `ui_eq_apply()` (`ui_main.c` :778) stays the last apply after `audio_init`'s `sound_settings_apply()` (`audio_thread.c` :186) (review M5).
- **Benefit, measured not guessed:** the software EQ's cost per second of 44.1 kHz stereo, from the ELF instruction-count harness over `eq_process()` (lean rule 8), reported in MHz. The hardware EQ's CPU cost is zero by construction.

### 3.2 DAC idle power-down (agent A)
- **States:** *on* → (PCM stopped, `!pcm_is_playing()`, for `DAC_IDLE_MS`, ⚠️ 5 s, §9 Q1) → *standby* → (any PCM start: play, resume, a headphone click) → *on*. **"Paused" is not the predicate** (review H2): the mixer idles DMA with silence for 3 s after the last channel stops, and a PLL off while DMA runs hangs `sink_stop_pcm()` on the FIQ. So standby comes about 3 s + `DAC_IDLE_MS` after a pause.
- **Power-down order** (WM8758/WM8983 datasheet anti-pop guidance; agent A cites the page in the code): soft mute (already muted by the fade) → OUT1 amps off → mixers off → DACs off → PLL off. VMID (500 k) and BIASEN stay on, so the output stays at mid-rail and no pop reaches the headphones.
- **Wake order:** PLL on and locked (lock time measured on the device, in `last_wake_us`) → DACs → mixers → OUT1 → unmute. Target ≤ 20 ms, so the headphone clicker still feels instant.
- **Wake hook (review H3):** `dac_wake()` at the top of `mixer_channel_play_data()` and `mixer_channel_play_pause()` in `firmware/pcm_mixer.c` (:348, :395), **before** `pcm_play_lock`, under `HAVE_DAC_STANDBY`, owned by A. `mixer_start_pcm()` runs under `pcm_play_lock`, which only masks the DMA interrupt on a cooperative scheduler; sleeping or waiting there would let a second thread re-enter the half-updated mixer. The same hook runs in the sim.
- **One codec mutex** in `wm8758.c` around enter, wake and every register write path; entering re-checks `!pcm_is_playing()` under it, so a beep that starts during the enter sequence waits and then wakes the codec. It nests over `i2c_mtx` (`i2c-pp.c` :51); Rockbox mutexes are recursive and pass priority to the holder.
- **Idle timer (review H4):** the audio thread calls `dac_idle_poll()` from its loop; while `dac_idle_pending()` it uses `queue_wait_w_tmo(..., HZ/2)` even in the idle states, then goes back to the blocking `queue_wait()` (`playback.c` :3938–3945). Not a new thread, not a tick task (I2C needs a thread).
- **Shutdown from standby:** `audiohw_close()` keeps its order; from standby it skips the steps already done. It sets PLLEN itself (:220), so "powers up nothing" is wrong: A checks no pop with the PLL briefly back on (review M7).
- **Guard:** `HAVE_DAC_STANDBY` in `ipodvideo.h` under `HAVE_IPOD_UI`, documented in `docs/hardware.md`. Off reproduces today exactly.
- **Benefit:** the WM8758 datasheet's supply currents for "playback, headphones, no signal" vs "standby, VMID 500 k" go in the brief's report; the device check (§7) is a 30-minute paused rundown A/B if the owner wants one (⚠️ optional), otherwise the datasheet figure stands.

### 3.3 Fades (agent B)
- **Curve:** the fade steps the mixer gain in equal **dB** steps from 0 down to −60 dB, then mutes (and the reverse for fade-in), instead of equal amplitude steps. One precomputed `const` table of `MIX_AMP` values, stepped by the existing `pcmbuf_fade_tick` (100 Hz); no new timer. **The fade state becomes an index** into the table, so a fade reversed mid-way (pause then quick resume) continues from where it is, and `soft_mode` still scales the result (review L).
- **Length:** ⚠️ 250 ms out and 250 ms in (25 ticks, §9 Q3).
- **De-click on manual skip and scrub start: deferred** (review H5). Recorded in ROADMAP → Later with the review's design (a per-frame ramp in `pcmbuf.c`, first seek of a scrub only).
- **Upstream edit, small and guarded:** `apps/pcmbuf.c` under `#ifdef HAVE_IPOD_UI`. `fade_on_stop` stays on (default on, `settings_list.c` :1716) and hidden.

### 3.4 Volume curve (agent B)
- **One table** (`apps/ipodui/volcurve.c`), **whole dB** (`SOUND_VOLUME` has 0 decimals and `vol_tenthdb2hw` truncates to whole dB, so the DAC's 0.5 dB steps aren't reachable; review M1). 48 positions: step 0 mute; steps 17–47 are 1 dB each from −24 to +6 dB (31 steps, where listening happens); steps 1–16 cover −60 to −25 dB with gaps that grow toward the bottom, 1 dB just below −24 up to at most 4 dB near −60 (sum 35 dB over 16 gaps). The 48 values go in the source as a table with the rule in a comment, and a host test (`tests/volcurve/`) checks monotonicity, the end points, the gap rule and that no two steps share a dB value.
- **Wheel:** one wheel click moves one step (no change to the input layer). `np_pb_volume_step()` moves along the table; `global_status.volume` still stores dB (so upstream's `setvol()`, Volume Limit enforcement and the saved setting keep working), snapped to the table.
- **Bar and Volume Limit:** both read `vol_step_to_bar1024()`, so the bar fills evenly with each click and the limit's slider means the same thing it shows. `vlimit.c` (including its own 1 dB step, :372–381) and `np_playback.c` :614–640 move to the table; their tests move with them.
- **Migration:** a saved volume between table values snaps to the nearest step at or below it on first use (never louder).

### 3.5 Wave 2 (owner go 2026-10-11): a clean digital path
*Split by file ownership:* **wave 2a** (agent D, starts now, touches none of A's or B's files): automatic dither (`lib/rbcodec/dsp/dsp_sample_output.c`), the Sound Check default (`apps/settings_list.c`), and the host bit-perfect test (`tests/bitperfect/`, warble). **Wave 2b** (after A and B merge): hardware Sound Check (`wm8758.c`, the volume path) and the device "audio path" hash check (the mixer hook).

Feasibility checked 2026-10-11 against the code and the owner's library (FLAC headers on the unit: 374 × 16-bit/44.1, 11 × 24-bit/44.1, 15 × 24-bit/48; plus 125 m4b, 42 mp3, 3 wav). The owner's `config.cfg` has EQ preset Electronic (software EQ, 4.8 dB precut) and Sound Check off.
- **Bit-perfect today, by reading the chain:** 16-bit input is shifted up 12 bits (`dsp_sample_input.c` :94–151), and output adds half an LSB then shifts back (`dsp_sample_output.c` C path and `dsp_arm.S` :725–747), which returns the input exactly; the mixer copies a lone unity channel word for word (`pcm-mixer-armv4.c` :131–157); volume is the codec's (`HAVE_SW_VOLUME_CONTROL` is sim-only); playback frequency is "auto" (`settings_list.c` :1343), so 44.1 and 48 kHz files play at their own rate with no resampling. **What breaks it:** the software EQ (the owner's case today, fixed by a hardware preset), Sound Check, an audiobook speed other than 1×, a headphone click mixed in, a fade in progress. **Proof to add:** a host test through warble (16-bit noise in, identical samples out, every DSP stage at its shell default) and a developer-menu "audio path" check that hashes the mixer's output for a bundled test file on the device (covers the ARM assembly).
- **Dither, automatic:** upstream's noise-shaped triangular dither (`dsp_sample_output.c` :89–150) exists but is off (`dithering_enabled` default off, `settings_list.c` :2119). Always on would add noise to bit-perfect 16-bit material, so P15 enables it **only when the samples carry more than 16 bits**: a 24-bit or lossy source (the decoder's `frac_bits` above 16-bit PCM's) or any active DSP stage. Small guarded edit in `dsp_sample_output_format_change()` under `HAVE_IPOD_UI` (a config define; no `apps/` include). Cost: the C dither loop instead of the assembly copy, measured with the ELF harness; it only runs when dither does.
- **24-bit to the codec: not feasible cheaply.** The WM8758 accepts 24-bit words, but the PP5022's I2S word sizes beyond 16-bit are undocumented (`pp5020.h` :439–441, "other sizes not yet known") and the whole PCM chain (pcmbuf, mixer, beep, DMA) is 16-bit. Dither gets most of the audible benefit. Not pursued.
- **Hardware Sound Check:** apply the ReplayGain gain through the codec's volume (amp 1 dB, DAC 0.5 dB) instead of the DSP, so Sound Check no longer breaks bit-perfect playback or costs CPU. The gain must change when a track starts playing, not when it is decoded seconds earlier: it follows `PLAYBACK_EVENT_TRACK_CHANGE`. Rounding ≤ 0.25 dB. Positive gains use the amp's headroom, capped by Volume Limit.
- **Bug found: Sound Check's default is on.** Upstream's default is "track shuffle" (`settings_list.c` :2013–2014) and the shell reads any non-off value as On, so a fresh install or Reset Settings turns Sound Check on; the 7G's default is off. Fix: default off under `HAVE_IPOD_UI`. The owner's unit is already off.

---

## 4. Ownership

| Files | Owner (model) |
|---|---|
| `firmware/drivers/audio/wm8758.c`, `firmware/export/wm8758.h`, `firmware/export/dac_ext.h` (changes go through the orchestrator), `firmware/export/config/ipodvideo.h` (the `HAVE_DAC_STANDBY` line only), the idle poll in `apps/playback.c`, the wake hook in `firmware/pcm_mixer.c`, the S1 developer item in `apps/ipodui/devmode.c`, `firmware/drivers/audio/sdl.c`, the shell's `dac` trace line (one call in `apps/ipodui/ui_main.c`'s main loop) | **A: firmware-platform (opus)** |
| `tools/eqfit/`, `apps/ipodui/eq_hw.h`, `eq_hw_presets.c` (generated), `apps/ipodui/screens/eq.c`, `apps/pcmbuf.c` (fade, guarded), `apps/ipodui/volcurve.{c,h}`, `apps/ipodui/np_playback.c` (volume only), `apps/ipodui/vlimit.c`, `tests/volcurve/`, `tests/eqfit/` | **B: playback-audiobook (opus)** |
| `scripts/check_trace.py` rules, `tests/ui/ipodui-p15-*.script`, `scripts/gate_map.txt` rows | **C: tooling (sonnet)** |
| `apps/SOURCES`, `docs/hardware.md` budget table and flag table, both ledgers, ROADMAP | orchestrator |

A and B share no file (the de-click, which would have put B in `playback.c`, is deferred). The contract headers (§2) and both stubs (`wm8758.c`, `sdl.c`) are committed before fan-out (review M4).

## 5. Read list per agent (≤ 3)
- **A:** this brief §0, §2.1, §3.1–3.2; `firmware/drivers/audio/wm8978.c`; the WM8758 (or WM8983) datasheet power-up/down sequence section.
- **B:** this brief §0, §2, §3.1, §3.3–3.4; `docs/design/spec/now-playing.md` (the volume bar) and `docs/design/spec/settings.md` (EQ); `apps/pcmbuf.c` :1300–1410.
- **C:** this brief §2.4, §6; `tests/ui/README.md`.

---

## 6. Device-reality checklist

1. **Compiled features:** `HAVE_IPOD_UI` selects the new caps and `HAVE_DAC_STANDBY`. The sim builds the same shell code; its `sdl.c` audiohw prints the hardware-EQ and standby traces, so the engine choice and the timer are tested in the sim. What the sim **can't** test (the codec actually doing it, pops, lock time) is the device check list in §7.
2. **Memory:** no allocation. The preset table is `const` (22 × 17 B ≈ 374 B rodata), the fade table ≤ 256 B, the volume table 64 × 2 B. No `core_alloc`.
3. **CPU:** the hardware EQ removes the software EQ's per-sample cost (measured, §3.1). The fade table replaces per-tick arithmetic with a lookup. The fit runs at build time on the Mac, never on the device.
4. **Storage:** none. The EQ choice is the existing setting; no new file.
5. **LCD:** the volume bar redraws exactly as today (same rectangle); only its fill value changes.
6. **Threads and locks:** I2C writes happen only in thread context: the audio thread (idle poll), the UI thread (EQ apply) and any thread starting a mixer channel (codec, UI beep), always through `dac_wake()` before `pcm_play_lock`. One codec mutex in `wm8758.c` serialises them (§3.2). Never taken from an IRQ, FIQ or tick task. The EQ registers survive standby, so an EQ write doesn't need a wake.
7. **Upstream splashes and prompts:** none reachable.
8. **Persistence:** the EQ preset and the volume are existing settings, saved as today. Standby is not persisted; boot starts *on* (preinit/postinit unchanged). A power-off in standby runs `audiohw_close()` from standby (tested by script `ipodui-p15-standby-poweroff`).
9. **Scale:** independent of library size.
10. **Deferred work:** the standby timer runs while PCM is stopped and is cancelled by any PCM start. States: playing (never armed), paused with the mixer's DMA tail still running (not armed), PCM stopped < 5 s (armed), ≥ 5 s (standby), stopped by the user (armed), **end of playlist** (armed; the audio thread is in `STATE_ENDED`), USB connected (standby is fine; USB exit returns to the previous state), headphone click while in standby (wake, click, re-arm). One script each.
11. **Stock firmware unchanged:** every `wm8758.*`, `sdl.c` and `pcmbuf.c` edit is under `HAVE_IPOD_UI` (configure defines it for the firmware tree too); the `playback.c` and `pcm_mixer.c` hooks under `HAVE_DAC_STANDBY`. Stock `.text` byte-identical to `build/fw/.size-baseline` (english.lang rule as usual; P15 adds no strings except the developer-only "EQ test" item, which is not localised: developer menu).
12. **Existing upstream paths doing the same job:**
    - EQ: Rockbox's software EQ (`dsp_eq`) vs the hardware EQ: **one wins per preset** by the `maxerr` rule; the loser is held flat/disabled in the same apply call. Upstream's bass/treble (EQ1/EQ5 through `audiohw_set_bass/treble`): shadow-only while a hardware preset runs (§2.1); upstream's `EQ_CAP` settings path isn't compiled in. `sound.c`'s prescaler stays 0.
    - Volume: upstream's `HAVE_PERCEPTUAL_VOLUME` / `volume_adjust_mode`: not used by the shell (it calls the table directly); stock unchanged.
    - Fades: upstream's `pcmbuf_fade` is the mechanism, reused; no second fader.
    - Power: `audiohw_close()` at shutdown (kept); 8a/P9's poweroff and USB handlers don't touch the codec.
    - The shell's own handlers: `ui_main.c` :185–187 and :628 and `vlimit.c` :362–364 call `audio_pause()`/`audio_resume()`; they need no change (standby hangs off the PCM layer, below them).
13. **Builder iteration predicate:** `eqfit` iterates `presets[]` in `screens/eq.c` order; a static assert checks `ARRAYLEN(eq_hw_presets) == ARRAYLEN(presets)`.

---

## 7. Acceptance

**Gate:** `GATE PASSED` on `p15-dac` after integration; budget-reviewer end review clean of High findings.

**Host tests:** `tests/fade/` (the dB table: monotonic, 25 steps, last step mute; reversal arithmetic), `tests/eqfit/` (the generated table matches a fresh run of the tool; every hardware preset's `maxerr` ≤ the threshold; Flat is all zeros), `tests/volcurve/` (as §3.4).

**Sim scripts (C):** `ipodui-p15-eq-engine` (each preset logs the expected engine; switching hw → sw → Off leaves exactly one EQ active), `ipodui-p15-standby` (the 6 states of checklist 10), `ipodui-p15-standby-poweroff`, `ipodui-p15-fade` (pause, then resume mid-fade: the trace shows the index reversing, not restarting), `ipodui-p15-volume` (wheel clicks walk the table; bar trace evenly spaced; Volume Limit at 50 % caps at step 24).

**Device checks** (added to ROADMAP → Device checklist; one owner session, ~10 min):
1. S1 EQ test: is the mid boost audible? (decides `IPODUI_HW_EQ` scope)
2. Bass Booster, Treble Booster, Vocal Booster and Off sound different and undistorted at full volume.
3. Pause, wait 10 s (standby), resume: no pop, no click, no audible delay. Same with the headphone clicker on.
4. Volume: a full wheel turn from silence is a smooth rise; the bottom of the bar is audible and quiet, the top unchanged.
5. Pause and resume sound like a fade, not a cut.
7. S1 round at 48 kHz: the mid boost is still there.
6. `ipodui.log` shows the standby/wake lines and the PLL lock time.

**Numbers reported at close:** software EQ cost (MHz) removed; datasheet standby saving (mA); image size delta (expected < 3 KB); stock `.text` identity.

---

## 8. Review media
- Before/after plot of each preset's response, iTunes curve vs hardware fit, from `eqfit --plot` (PNG via the existing `scripts/` Python, no new dependency), opened for the owner.
- A one-image plot of the old vs new volume curve (dB vs click).

## 9. Frozen owner questions (⚠️ default until the owner round)
- **Q1, standby delay:** ⚠️ 5 s after pause or stop. Shorter saves more; longer avoids a wake on quick pause/resume (which should be silent anyway).
- **Q2, volume steps:** ⚠️ 48 from mute to max (today: 96 one-dB steps), 1 dB each over the top 30 dB. Tuned by ear on the device.
- **Q3, fade length:** ⚠️ 250 ms out, 250 ms in, dB curve.

## Revision 1: review findings → resolution
| Finding | Resolution | § |
|---|---|---|
| H1 precut fights `sound.c` | Private `dac_ext.h` API, no `EQ_CAP`; precut inside `wm8758.c`, no make-up; EQ1/EQ5 ownership both ways | 0, 2.1, 3.1 |
| H2 standby while DMA runs hangs | Predicate `!pcm_is_playing()`, re-checked under the mutex; DMA-tail state scripted | 3.2, 6.10 |
| H3 wake inside `mixer_start_pcm()` | Hook in `pcm_mixer.c` before `pcm_play_lock`; one codec mutex | 3.2, 4 |
| H4 idle wait has no timeout | Timed wait only while pending; end-of-playlist state | 0, 3.2, 6.10 |
| H5 15 ms de-click | Deferred to ROADMAP → Later | 3.3 |
| H6 `M128ENB` clobbered | `ADDCTRL` shadow; S1 at 48 kHz | 3.1, 7 |
| M1 volume arithmetic | 48 whole-dB steps, explicit gap rule; vlimit step remapped | 2.3, 3.4 |
| M2 caps list | Moot: caps unchanged | 2.1 |
| M3 firmware tracing | `dac_get_stats()`, shell logs; S1 ring | 2.1, 2.4 |
| M4 stub before fan-out | `wm8758.c` and `sdl.c` stubs committed | 2.1, 4 |
| M5 persistence, trace format | `eq_enabled=false` saved; apply order; separate `eq engine=` line | 2.4, 3.1 |
| M6 EQ model gaps | ×0.919 at 44.1 kHz; EQ3 for 1.1 kHz; EQ position and M128ENB current from the datasheet | 3.1 |
| M7 PWRMGMT shadows, close sets PLLEN | Shadows; close-from-standby check | 3.1, 3.2 |
| L line numbers, fade rationale, `reason=eq`, constant-check test, template | Corrected; index-based fade; wake reason dropped; `tests/fade/`; pre-flight line and spec read list | 0, 2.4, 3.3, 5, 7 |
- **Q4, EQ fidelity threshold:** ⚠️ a preset runs on the hardware only if its worst-case deviation from the iTunes curve is ≤ 1.5 dB; others stay software, so no preset changes how it sounds by more than that.
  - **Answered (owner, 2026-10-11): every preset runs on the codec.** "I'm not looking for fidelity to the software versions of these EQ, people will pick what they prefer." `EQ_HW_MAXERR` is 255; each preset's `maxerr` stays in the table as a record of its fit (worst: R&B 2.9 dB).
