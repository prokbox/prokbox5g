# Plan: better sample-rate conversion

*Status: R-1 and R-2 built and host-measured on branch `p19-resample`, 2026-10-25 (§8); R-0's device half, R-3, R-4 and R-5 are open. Planned 2026-10-24 as P19 package 19-13. It is independent of the BCM2722 work, so it can run in parallel with any other P19 package. Evidence tags as in `docs/process.md`.*

## 1. What happens today

- **The DAC's rates.** The WM8758 plays 8, 11.025, 12, 16, 22.05, 24, 32, 44.1 and 48 kHz (`HW_SAMPR_CAPS` in `ipodvideo.h`) and nothing above 48 kHz [datasheet, `docs/wm8758-reference/`].
- **Choosing the output rate.** In "auto" (`play_frequency` = 0, our default), `audio_guess_frequency()` in `apps/playback.c` decides per track [code]:
  - 44.1 and 48 kHz files play natively, with no conversion;
  - any other rate goes to its family: a multiple of 4 kHz goes to 48 kHz (96, 192, 32, 24, 16, 12, 8), and everything else goes to 44.1 kHz (88.2, 176.4, 22.05, 11.025).
- **So every conversion we do is a whole-number ratio,** except 32 → 48 kHz (3:2):

  | Source | Output | Ratio | Typical content |
  |---|---|---|---|
  | 88.2 / 96 kHz | 44.1 / 48 | ÷2 | hi-res FLAC |
  | 176.4 / 192 kHz | 44.1 / 48 | ÷4 | hi-res FLAC |
  | 22.05 / 24 kHz | 44.1 / 48 | ×2 | audiobooks, podcasts |
  | 11.025 / 12 kHz | 44.1 / 48 | ×4 | old audiobooks, speech |
  | 16 kHz | 48 | ×3 | speech, voice memos |
  | 8 kHz | 48 | ×6 | telephone-quality speech |
  | 32 kHz | 48 | ×3/2 | DAB/DVB rips, some podcasts |

- **The converter.** The DSP's `RESAMPLE` stage (`lib/rbcodec/dsp/resample.c`, ARM assembly in `dsp_arm.S`) is a 4-point Hermite interpolator: cheap, but not band-limited [code].
  - When decimating (hi-res), ultrasonic content above the new Nyquist folds back into the audible band as aliasing.
  - When interpolating (audiobooks), images of the speech band are left above the old Nyquist. Hermite also droops the top octave a little.
  - Both effects are small, but avoidable.
- **The 16-bit output.** Output to the DAC is 16-bit with dither, so 24-bit sources lose their low bits (catalog row W-8).

## 2. Goals

1. **Clean.** Hi-res decimation keeps aliasing below about −90 dBFS in 0–20 kHz, with a flat passband to 20 kHz (±0.1 dB). Audiobook interpolation has no audible images.
2. **Cheap.** At or below today's Hermite cost per output sample where possible. Never enough to break 19-1's dark-playback clock (24 MHz, PLL off) for content that runs there today.
3. **Invisible.** No setting, no change to the user's files, no new resident RAM worth tabling. Our own files stay ours, and drag-and-drop stays untouched.
4. **Upstreamable.** A generic improvement to `lib/rbcodec`, suitable for the standing backport track (`upstreamable`).

## 3. Design

### 3.1 Integer-ratio filters (the main win)
- **÷2: a half-band FIR decimator.** Every other coefficient of a half-band filter is zero and the taps are symmetric, so a 31–47 tap filter costs only 8–12 multiplies per output sample per channel. The polyphase form computes only the samples that are kept.
- **÷4: two half-band stages in cascade.** The second stage runs at half the rate, so it costs about half as much.
- **×2 and ×4: the same half-band filters used as interpolators** (zero-stuffing never computed, i.e. polyphase), in one or two stages.
- **×3, ×6 and ×3/2: a small polyphase FIR** with 3 phases (or 2×3), 8–12 taps per phase. These rates are rare, so the code stays small and correctness matters more than speed.
- **Fallback:** today's Hermite stays for any ratio not listed, and for `play_frequency` set to a fixed rate.
- **Arithmetic:** Q31 coefficients with 64-bit accumulation (SMULL/SMLAL), the same idiom as the codecs' hot loops. The PP5022's "integrated MAC" is the ARM7TDMI's own multiplier (study on `pp-mac-dvs`, catalog row P-1), so these instructions are the hardware path. There is no separate accelerator to target.

### 3.2 Where it lives
- **One file.** The filters go in `lib/rbcodec/dsp/resample.c`, as a second worker chosen in `resample_new_delta()` when the ratio is one of those in §1. There is an ARM version in `dsp_arm.S` and a C reference for the sim and warble. No `apps/` include (rule 4).
- **Memory:**
  - the history per channel grows from 3 samples to about 24–48 samples, roughly 400 B in the existing `IBSS_ATTR` `resample_data`;
  - the coefficient tables total a few hundred bytes of `.rodata`;
  - the output buffer is the existing 192-sample `resample_out_bufs`, and nothing new is allocated.

  The IRAM growth needs a line in `hardware.md`. The DSP is part of the codec path, so it falls under the IRAM rule's codec exception.
- **Stage order with time-stretch:** the order stays `TIMESTRETCH` before `RESAMPLE` (`dsp_proc_database.h`). Audiobook speed changes the time-stretch, not the resample ratio, so the integer ratios hold at every speed.

### 3.3 Optional: native low rates for speech (owner question, Q-R1)
- **Why it's attractive.** The DAC can play 22.05, 16 or 11.025 kHz natively. Playing an audiobook at its own rate removes the conversion entirely and quarters or halves the I2S traffic. It may also allow a lower PLL or MCLK rate. Over hours of audiobook listening, that is a real saving.
- **Costs:**
  - a DAC rate switch at each rate change, done muted, with the existing pop-free sequence;
  - UI sounds (clicks, haptics audio) mix at the playback rate, so they would be band-limited at 11 kHz;
  - crossfade between tracks of different rates is impossible (it isn't used for audiobooks anyway).
- **Default ⚠️:** off. Evaluate with an R-0 measurement before deciding. It is gated by content type (audiobook folders and spoken-word files only), never for music.

### 3.4 Optional: 24-bit to the DAC (catalog W-8)
- **The opportunity.** The codec accepts 24-bit I2S. Whether the PP5022's I2S size field (`[9:8]`) carries 24-bit is unverified.
- **Value.** It is small for listening on this DAC's analogue stage, but it is honest to the source, and it drops a dither stage for 24-bit files.
- **Cost.** It doubles the PCM buffer bytes per second, which matters for the pcmbuf and the audio buffer floors.
- **Default ⚠️:** probe only (a read-only check of the field, then a test tone). Decide after the probe.

## 4. Hardware features (process.md brief item 6)

| Catalog row | Block | Use here | CPU fallback | Probe |
|---|---|---|---|---|
| P-1 | PP5022 "integrated MAC" | Resolved: it is the ARM7TDMI multiplier, so SMULL/SMLAL is the hardware path | n/a | done (`pp-mac-dvs`) |
| COP (P19 19-1c) | PP5022 second core | **Not a target.** The filter is a DSP stage inside the codec's own pipeline, so it runs inline on whichever core decodes: the CPU in dark playback (19-1c), with the COP parked (19-1d). Handing it to the COP would wake a parked core, add a cross-core handoff per PCM block, and need cache flushes (the PP5022's caches are not coherent between cores). All of that costs more than the filter, and it undoes P19's single-core dark playback. When lit, it follows the decode wherever 19-1c places it | same core as decode | none |
| P-5 | IRAM crossbar banking | If the two cores don't stall each other's IRAM, the filter's history in IBSS is free to touch from either core | DRAM history (slower) | the IRAM two-core read-loop probe from the catalog |
| W-8 | WM8758 word length | 24-bit output (§3.4) | 16-bit with dither (today) | the I2S size field probe |
| W-1/W-10 | WM8758 EQ, oversampling | Untouched; conversion happens before the DAC | n/a | n/a |
| BCM2722 | VideoCore | **Not used:** powered off in dark playback (19-1, D-3), and a round trip per PCM block costs more than the filter | n/a | n/a |

Where software remains: all of it, by necessity. The DAC tops out at 48 kHz and has no rate converter, and the BCM2722 is the wrong tool for a continuous low-latency stream on battery.

## 5. Packages

| Step | Work | Output | Gate |
|---|---|---|---|
| **R-0** | Measure today: cycles per second for Hermite at each ratio; 96/24 and 192/24 FLAC decode load at 24 and 30 MHz (underruns, boost share); a 22.05 kHz MP3 audiobook. Developer `HAVE_DEVSTATS` build plus warble. | a table in this doc | numbers, no code change |
| **R-1** | C reference filters plus host tests in warble (`tests/resample/`):<br>• sine sweeps → passband ripple, stopband, aliasing;<br>• impulse response;<br>• a null against a double-precision reference;<br>• continuity across buffer boundaries and flushes (seek, track change);<br>• mono and stereo;<br>• the time-stretch chain. | `resample.c` C path | host tests green; the specs in §2 met |
| **R-2** | ARM assembly for ÷2, ÷4, ×2, ×4 in `dsp_arm.S`, bit-exact with R-1. | ARM path | bit-exact; cycles ≤ the R-0 Hermite figure, or a stated, budgeted cost |
| **R-3** | Device A/B (owner listening, plus current over a fixed playlist): Hermite vs new, on hi-res FLAC and a 22.05 kHz audiobook. | `hw-experiments.md` rows | no underruns at 24 MHz for content that ran there before; owner hears no regression |
| **R-4** | Optional §3.3 native low rates for speech, behind Q-R1. | | power A/B |
| **R-5** | Optional §3.4 24-bit probe, behind the probe result. | | probe |

**Ledger:** `docs/changes-from-rockbox.md` gets a row for `resample.c` and `dsp_arm.S`, marked `upstreamable`, plus a §4 backport note.

## 6. Risks

- **Cost at 192 kHz.** Decoding 192/24 FLAC may already need boost. The ÷4 cascade adds roughly one ÷2 stage's cost. R-0 tells us the headroom, and R-2's budget is set from it.
- **Latency and seeks.** Longer filters add a few samples of delay (a 47-tap half-band at 96 kHz is about 0.25 ms) and need their history flushed on seek, just like Hermite's. That is covered by R-1's continuity tests.
- **Gapless playback.** History across a gapless track boundary at the same rate must be kept, not flushed, so there's no click. A rate change between tracks flushes, as today.

## 7. Owner questions

- **Q-R1:** native low rates for audiobooks (§3.3)? Default: off until R-0 shows the saving.
- **Q-R2:** 24-bit output if the probe passes (§3.4)? Default: probe only.

## 8. Results (R-1, R-2; branch `p19-resample`, 2026-10-25)

### 8.1 What was built, and where it departs from §3.1

- **Half-band stages are polyphase IIR, not FIR** [host-measured]. Each half-band is two parallel chains of first-order allpass sections in z^-2 (Valenzuela and Constantinides 1983; elliptic closed-form design as in Laurent de Soras' HIIR). A linear-phase FIR that meets §2's spec at 44.1 kHz (flat to 20 kHz, 90 dB from 24.1 kHz) needs about 123 taps, 31 multiplies per output with symmetry; the IIR needs **7 multiplies** for 91 dB. On the ARM7TDMI that is the difference between roughly 300 and 52 cycles per output. The cost is non-linear phase close to the band edge (group delay varies only above about 18 kHz, inaudible) [design].
- **Three designs** (`tests/resample/hb_design.py` writes `lib/rbcodec/dsp/resample_hb_coefs.h`, Q31): `hb_t44`, 7 coefficients, flat to 0.2268 fs, 91.1 dB (decimation to the 44.1 kHz family, and every interpolator: flat to 0.4535 of the input rate); `hb_t48`, 6 coefficients, flat to 0.2083 fs, 96.4 dB (decimation to the 48 kHz family); `hb_r`, 3 coefficients, 95.1 dB, the outer stage of a /4 or x4 cascade.
- **Ratios:** /2 = one tight stage; /4 = `hb_r` then the tight stage; x2 = `hb_t44`; x4 = `hb_t44` then `hb_r`.
- **x3, x6 and 3:2 are not polyphase FIR.** A polyphase FIR that keeps x3's images 60 dB down needs about 20 taps per phase (C cost about 260 cycles per output; 3:2 needs 24 per phase for 0.5 dB ripple). Instead the stage oversamples by half-bands and lets the existing Hermite interpolator take the last, now gentle, step (oversampled polynomial interpolation, the approach of Niemitalo's 2001 note on interpolators for oversampled audio): x3 = x4 then Hermite 4:3, x6 = x4 then Hermite 2:3. Images ~50 dB down, flat within 0.02 dB, at 166 / 99 cycles per output.
- **3:2 stays Hermite by default.** The best cheap path (x2 then Hermite 4:3, images 44 dB down, 0.25 dB droop at 13 kHz) costs 156 cycles per output against Hermite's 60; for stereo 32 kHz music that is half the 30 MHz budget. It is behind `RESAMPLE_Q_ALL` (warble `rsq=2`).
- **Fallback switch:** `dsp_resample_set_quality()` (`resample.h`): `RESAMPLE_Q_HERMITE` restores upstream behaviour for every ratio, `RESAMPLE_Q_INTEGER` is the default, `RESAMPLE_Q_ALL` adds 3:2; it takes effect at the next rate change. Pitch changes, fixed output rates that give a non-whole ratio, and the voice DSP keep Hermite.
- **Where it lives:** `resample.c` (mode choice, cascade, odd-tail and queue handling), `resample_hb.{c,h}` (C reference, used by the simulator and warble), `resample_hb_arm.S` (ARM, used on the device). No `apps/` include.

### 8.2 Quality: measured through warble's real DSP (`tests/resample/run.sh`)

Multitone in (24-bit WAV), 16-bit dithered output, Blackman-Harris DFT. "Alias" is the worst ultrasonic tone folded into 0-20 kHz; "image" the worst image below the output Nyquist; both relative to the tone. Targets: decimation and x2/x4 at least 88 dB down and flat within 0.1 dB; x3/x6 at least 40 dB down.

| Ratio | Band-limited (new) | Hermite (today) |
|---|---|---|
| 88.2 -> 44.1 | alias -91.9 dB, flat 0.000 dB to 20 kHz | alias **0.0 dB** (no rejection) |
| 96 -> 48 | alias -97.0 dB, flat | alias 0.0 dB |
| 176.4 -> 44.1 | alias -93.0 dB, flat | alias 0.0 dB |
| 192 -> 48 | alias -95.4 dB, flat | alias 0.0 dB |
| 22.05 -> 44.1 | image -90.5 dB, flat to 10 kHz | image -8.1 dB, droop 4.3 dB at 10 kHz |
| 24 -> 48 | image -91.3 dB | image -8.3 dB, droop 4.2 dB |
| 11.025 -> 44.1 | image -91.0 dB | image -8.3 dB, droop 4.4 dB |
| 12 -> 48 | image -91.8 dB | image -8.4 dB, droop 4.3 dB |
| 16 -> 48 | image -49.9 dB, flat 0.02 dB | image -10.9 dB, droop 3.1 dB |
| 8 -> 48 | image -50.9 dB, flat 0.02 dB | image -11.3 dB, droop 2.9 dB |
| 32 -> 48 (`rsq=2`) | image -43.9 dB, droop 0.25 dB at 13 kHz | image -10.9 dB, droop 3.0 dB |

Today's Hermite does not filter at all when decimating: an ultrasonic tone in a hi-res file comes out at full level, folded into the audible band. Also checked: the ARM code equals the C reference bit for bit (qemu, every design, both directions, blocks of 1-97 samples); `resample.c`'s worker gives identical output for one block and for irregular 1-37-sample blocks at every ratio, mono and stereo; mono; time-stretch before the resampler; golden output hashes (`tests/resample/golden.txt`).

### 8.3 Cost: cycles per output sample per channel

ARM7TDMI model: executed instructions from qemu (`tests/resample/arm.sh`, `cyc_plugin.c`), each weighted by its ARM7TDMI timing with zero-wait memory and the multiplier's worst case [model, not silicon]. Hermite is the firmware build's own `dsp_arm.S` code in the same harness. CPU % = cycles x output rate x channels / 30 MHz.

| Ratio | Stages | New (cycles) | Hermite (cycles) | New / Hermite CPU % at 30 MHz |
|---|---|---|---|---|
| 88.2 -> 44.1 stereo | /2 t44 | 108.5 | 101.4 | 31.9 / 29.8 |
| 96 -> 48 stereo | /2 t48 | 99.5 | 101.4 | 31.8 / 32.4 |
| 176.4 -> 44.1 stereo | /2 r, /2 t44 | 253.5 | 129.9 | 74.5 / 38.2 |
| 192 -> 48 stereo | /2 r, /2 t48 | 244.5 | 129.9 | 78.2 / 41.6 |
| 22.05 -> 44.1 mono | x2 t44 | 51.7 | 53.9 | 7.6 / 7.9 |
| 22.05 -> 44.1 stereo | x2 t44 | 51.7 | 53.9 | 15.2 / 15.8 |
| 11.025 -> 44.1 mono | x2 t44, x2 r | 59.6 | 45.8 | 8.8 / 6.7 |
| 16 -> 48 mono | x2 t44, x2 r, Hermite 4:3 | 166.3 | 48.8 | 26.6 / 7.8 |
| 8 -> 48 mono | x2 t44, x2 r, Hermite 2:3 | 99.3 | 43.2 | 15.9 / 6.9 |
| 32 -> 48 stereo, `rsq=2` only | x2 t44, Hermite 4:3 | 155.8 | 59.6 | 49.9 / 19.1 |

Per stage (ARM, instructions / cycles per output): x2 `hb_t44` 23.7 / 51.7, `hb_t48` 21.7 / 47.2, `hb_r` 15.7 / 33.7; /2 `hb_t44` 50.4 / 108.5, `hb_t48` 46.4 / 99.5, `hb_r` 34.4 / 72.5. The C reference compiled for the ARM (-O2) costs 2.2x as much, so the assembly is what ships. Host (warble) wall time is not a useful proxy and is not tabled.

**Effect on the 19-1a/19-1c dark clock** (24 MHz, PLL off; FLAC at 44.1 kHz already ~55 % [silicon]): nothing changes for 44.1 and 48 kHz files, which are not converted. The 22.05 kHz audiobook, the case that matters, gets slightly cheaper (-0.1 M cycles/s mono). 96 kHz hi-res is even; 88.2 kHz costs +0.6 M cycles/s (+2.6 points at 24 MHz). The /4 cascades double the conversion cost (+11 M cycles/s); 176.4/192 kHz files already need about 40 % of 30 MHz for Hermite alone plus their decode, so they were never 24 MHz content, and the band governor's `LOAD` hold covers them. 16 kHz speech costs +5.6 M cycles/s (+23 points at 24 MHz) and 8 kHz +2.7 M; both still fit 24 MHz with a light speech codec, but they are R-3's items to confirm (no underruns at 24 MHz). A cheaper x3 (x2 then Hermite 2:3: 94 cycles, images 30 dB down, 0.25 dB droop) is the fallback if R-3 shows otherwise.

**Memory:** +208 B IRAM (`rs_bl`: the stage states, IBSS, codec path), +1,536 B DRAM `.bss` (two 768 B cascade buffers), +3.7 KB `.text` (ARM stages 1,164 B; `resample.c` +2.8 KB), +120 B `.rodata` (fw-ui-mod, against p19 799ff0f8d7). Nothing on the stack beyond a 16-byte group. The `docs/hardware.md` budget line for the IRAM is the orchestrator's.

### 8.4 Q-R1, native low rates for speech: measured proposal, not built

- **What it saves** [estimate]: for a 22.05 kHz mono audiobook, the conversion (51.7 cycles x 44,100 = 2.3 M cycles/s, about 9.5 % of 24 MHz), plus half the output stage's per-sample work and half the I2S FIFO refills. The DAC's own digital filters run at the lower rate. If the PP5022 core draws roughly 20-30 mW active at 24 MHz [hypothesis, unmeasured], that is about 2-3 mW, a few percent of dark audiobook playback.
- **What it costs:** a DAC rate switch at each rate change (the existing muted sequence), UI clicks mixed at 22.05 kHz, and no crossfade across rates.
- **Decision rule:** a default-off switch in `audio_guess_frequency()` gated on spoken-word content, built only when an R-0 device log (current over a fixed 22.05 kHz audiobook, 44.1 kHz output vs native) shows the saving is worth it. This package does not build the switch: the conversion it would remove is now as cheap as Hermite's.

### 8.5 State of the art (process.md brief item 7)

`docs/algorithms-survey.md` (sample-rate conversion row, rank 1) names half-band polyphase filters as the established method for whole-number ratios. **Adopted, in its IIR form** (polyphase allpass half-bands), which the survey's FIR wording does not distinguish: same structure, 4-5x fewer multiplies at this spec on a CPU without SIMD. Not adopted: Farrow and soxr-style arbitrary-ratio resampling (this product's ratios are whole numbers, except the optional 3:2); polyphase FIR for x3/3:2 (2-4x the cost of oversampling plus Hermite for this quality).
