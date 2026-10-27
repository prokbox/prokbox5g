# Hardware features catalog: everything the silicon offers, and who should take it

*Owner directive, 2026-10-24: "we want to really have our software be natively, thoroughly, taking advantage of the full piece of hardware, even more than Apple ever did, if possible."*

This is the one list every phase brief cites. It covers the PP5022 (CPU and COP), the BCM2722 and the WM8758, including features from the product briefs and factsheets that Apple never used. **A brief that does not name the catalog blocks that could carry its work is rejected** (`docs/process.md`, brief template item 6).

## Sources and limits

| Chip | Read first | Notes |
|---|---|---|
| PP5022 | `docs/pp5022-reference/datasheet-features.md` (43 rows, ranked shortlist); `docs/hardware-fit-audit.md`; `docs/pp5022-reference/` | Row numbers below are that file's `#`. The PP briefs are 3-page marketing sheets: "up to" numbers, die features that may have no pins on the 5.5G. |
| BCM2722 | On branch `p19-bcm-prep`: `docs/bcm2722-reference/` (`overview.md` "Product brief claims", `graphics.md`, `power.md`, `audio.md`, `open-questions.md` #56-#59) and `docs/research/bcm2722/` (`jobs.md`, `memory-contract.md`, `power-contract.md`, `offload.md`, `display-offload-plan.md`). On `p19-brief-periph`: `brief-map-peripherals.md` (24-row claim table). | **`docs/bcm2722-reference/datasheet-features.md` (pending from the BCM session) will be linked here when it merges to `p19-bcm-prep`.** `brief-map-power.md` (power modes, clocks, caches, DMA, host bus) is still being written on `p19-brief-power`; rows B-9 to B-12 below come from the open questions until it lands. |
| WM8758 | `docs/wm8758-reference/README.md` (features table, power, open questions), `docs/pp5022-reference/audio.md`, `docs/phase15-brief.md`, `docs/data-paths.md` (M13, "The WM8758") | Verified 2026-10-24 against the public WM8758B datasheet, Rev 4.4 (not a sibling). We do not keep a copy in the repo; the URL is in the reference README. Wiring-dependent rows stay [hypothesis]. |
| Buses | `docs/data-paths.md` | Who can move bytes where. Read it before any cross-chip row below. |

**Status words:** *used* (shipping), *known-unused* (our manuals know the block; no firmware uses it), *unknown* (claimed by a sheet, absent from our docs), *unverifiable* (die may have it; no pins or no way to reach it on the 5.5G).
**Evidence tags:** [silicon] measured on the device; [datasheet] a product brief says so; [rockbox-derived]; [apple] seen in Apple's code; [hypothesis].
**Rule (ROADMAP architecture decisions):** a job moves to a hardware block only when a device measurement shows it faster or cheaper on battery at the same fidelity; the CPU path stays as the fallback. Every row says what that fallback is and which probe settles it.

## 1. PP5022 (CPU / COP)

| ID | Feature | Status | Evidence | Where used / could be used | ProkBox benefit | Phase | Probe |
|---|---|---|---|---|---|---|---|
| P-1 | **Integrated MAC** (every PP502x brief; row 11) | unknown | [datasheet]; whether software-visible is [hypothesis] | EQ, resample, time-stretch (audiobook speed), MP3/AAC inner loops | CPU cycles, so a lower clock: power, smoothness | P19 | Static search of Apple's `osos` plus a read-only register-space scan. No device risk |
| P-2 | **100 MHz per core** (row 2); we stop at 80 | known-unused | [datasheet]; limit by SDRAM timing or 1.2 V is [hypothesis] | Heavy codecs (APE extra high, large FLAC), race to idle | Smoothness, power | P19 / P9b | PLL to 100 MHz for 1 s, memory pattern plus CRC loop on both cores. Fallback: 80 MHz. Risk of hang: bench build only |
| P-3 | **Core voltage scaling** (1.14-1.32 V window; rows 5, 6) | known-unused (core fixed 1.2 V) | [datasheet]; [rockbox-derived] fixed value | 30 MHz playback and idle at a lower `DCDC1` step | The largest controllable power term; dominant playback state | P18 | PMU `DCDC1` step probe on a bench build (blocked on PP open question #20). Fallback: 1.2 V |
| P-4 | **Module clock gating** (`DEV_EN`/`DEV_EN2`; row 6) | known, partly used | [datasheet]; [rockbox-derived] | Gate FireWire link, second USB port, unused UART, PWM when idle | Small standby saving, free | P18 / P9b | Dump `DEV_EN*`, clear one bit at a time in a test build. Fallback: leave on |
| P-5 | **COP clock skipping** (row 3) | known-unused | [datasheet]; [rockbox-derived] register comment | A COP codec needing about 40 % of 30 MHz | Finer power step with no PLL change | P19 | `COP_CTL` skip counts timed against `USEC_TIMER` |
| P-6 | **Intermediate clocks, 32 kHz to 100 MHz** (row 4; open question #9) | partly used (24/30/80 MHz) | [datasheet]; [rockbox-derived] | A 48 MHz step; lit-screen 24 MHz (R10) | Power steps | P18 | Selector switch timed against `USEC_TIMER` |
| P-7 | **COP as compute engine** (row 1; data-paths cell 8) | used (codec on COP, Fit-2) | [silicon] | Art decode (#77), DSP, tag work, EQ | CPU idle 88-92 % vs 45 % on FLAC | P19, P12 | A/B per job (#72). Fallback: CPU |
| P-8 | **IRAM cross-bar banking** (row 9) | unknown | [datasheet] zero-wait cross-bar; banking [hypothesis] | COP and CPU hot data in different banks | Throughput; fewer stalls | P19 | Two-core read loop, same region vs different 32 KB regions |
| P-9 | **Second DMA engine for the BCM port** (rows 12; data-paths cell 14, R1) | used (E2 pushes) | [silicon] S37: about 11 MB/s, CPU 97 % free | LCD pushes, slot uploads | CPU sleeps through pushes | P19 | done; extend to slot uploads |
| P-10 | **ATA-66 controller** (row 23) | used (UDMA2); UDMA4 closed | [silicon] 21 MB/s either way | Nothing left; limit is overhead, not silicon | none | P17 | closed (S36c) |
| P-11 | **IDE completion interrupt** (`HAVE_ATA_DMA_IRQ`) | built, unknown if source 23 fires | [rockbox-derived] | CPU sleeps during reads | Power, fewer wakes | P17 | DP-4 / PP #29 |
| P-12 | **USB 2.0 OTG host** (row 24) | known-unused | [datasheet]; VBUS source on the USB-C board unknown | Import and export music from a USB stick with no computer; a USB DAC | Mission: copy off or share with no computer | P17 / P21 | Read `OTGSC` / `PORTSC1`; check VBUS on the board (hardware question) |
| P-13 | **Hardware PWM, 4 channels** (row 33) | one channel used (click) | [datasheet]; [rockbox-derived] | Backlight dimming and haptic without the software-PWM timer | Fewer wake-ups (audit K1), smoother fades | P18 | PWM1-3 registers plus GPIO mux for the backlight line |
| P-14 | **Battery-backed RTC scratch** (row 39; PP5024 sheet) | unknown on PP5022 | [datasheet] for PP5024 only | Drained-latch byte for the boot guard (open question #33) | Boot-time state without storage | P18 | Write a word near `RTC`, survive PMU standby |
| P-15 | **On-die 4-channel ADC** (row 34) | known-unused | [datasheet]; pins unknown | Battery or temperature read without I2C | Fewer PMU transactions; charge derating | P18 | Enable `ADC_CLOCK_SRC`, read the block |
| P-16 | **Unique ID block** (row 41; PP5024 diagram) | unknown | [datasheet] | Per-unit serial without disk | Minor | P20 | Read near `PP_VER` |
| P-17 | Clickwheel opto block, 13 nav GPIOs, key matrix (row 37) | used / not wired | [rockbox-derived] | Wheel (used); matrix absent | none | - | none |
| P-18 | DTV / CCIR TV out, video in, STN LCD, SD/MMC, CompactFlash/NAND, FireWire, S/P-DIF, JTAG, IR (rows 13, 25-32, 38) | unverifiable on this board | [datasheet] | No pins or parts. Panel is on the BCM; TV-out is the BCM's. | none | - | not worth chasing (see datasheet-features.md section 5) |

## 2. BCM2722 (VideoCore II)

The BCM has no fixed-function codec, 3D or scaler block: the brief's media and graphics features are **software on the 16-lane vector unit** (`bcm2722-reference/index.md` "Scope"). So every row is "our code, our budget".

| ID | Feature | Status | Evidence | Where used / could be used | ProkBox benefit | Phase | Probe |
|---|---|---|---|---|---|---|---|
| B-1 | **Vector unit** (16 lanes, about 15.7 element ops per cycle; 150 MHz after PLL) | used (H.264 decode, kernels) | [silicon] S6-S8, S27 | Everything below | 2D, decode, compositing at 0.46 ms for a 160x240 fade vs 15-40 ms on the PP | P19 | stage 3d / 5b on silicon |
| B-2 | **Display compositor / job interface** (`jobs.md`; slots, fade, average, scroll, blit, present) | designed, not run on silicon | [ours]; kernels bit-exact in vc2sim | Main-menu drift, marquee, slides, glides, list bands | Drift costs the PP 10-15 % of 30 MHz while lit; offloaded about 0.3 % | P19 | stage 5b (drift on silicon). Fallback: PP renderers (every job has a CPU path) |
| B-3 | **Second frame / double buffer in SRAM** (#76) | designed | [ours]; 1.25 MB SRAM [silicon] | Tear-free transitions, hide the 14.3 ms push | Smoothness | P19 | merged-image layout (`memory-contract.md`) |
| B-4 | **Texture slots** (SRAM 603 KB UI pools; SDRAM 4 MB on demand) | designed | [ours]; SDRAM [silicon] S11, bring-up quirk S32-S34 | Cover Flow textures (16 x 32 KB), panel pool, game assets | Cover Flow without the PP's pixel work | Cover Flow, P19, P12 | SDRAM bandwidth and refresh power unknown (#7, #12) |
| B-5 | **TE-synced present** (GPIO 49) | built in sim only | [apple] Apple races the scan; [silicon] no visible tearing without it | 30 fps video and animation locked to the glass | Smoothness; no tearing | P19, P14 | stage 3d measures panel refresh (#52) |
| B-6 | **Draw-on-chip scene model** (tiles, sprites, blends, affine, column renderers, triangles; `graphics.md`) | engine test only (`r3d`), sim-verified | [ours]; estimates tagged | Games (Class B 2D at 27 MHz; Class C 3D at 150 MHz); Cover Flow | Games beyond what the PP can push at 27 ms per frame | P12, Cover Flow | stage 3d on silicon |
| B-7 | **H.264 Baseline decode** | used (silicon, 320x240) | [silicon] S27-S30: 18 ms per frame on real footage | Videos (P14) | Video with the PP nearly idle | P14 | done; more profiles later |
| B-8 | **MPEG-4 SP / H.263; AAC on chip** (Apple ships both on the BCM; C9) | planned (`mpeg4.md`) | [apple] decoders exist; ours open | Video soundtrack off the PP | PP decode CPU, power | P14 | audio spike S35-S36c; AAC stage 9 |
| B-9 | **Sleep / hibernate power modes** (0.6 V hibernation; #57) | unknown | [datasheet] | Dark standby that keeps SRAM, instead of the cut (C3/C4 in `power-contract.md`) | Faster wake (about 137 ms vs 0.37-0.62 s) at an unknown current | P18, P19 | stage 5s: wake picture, SRAM held, current (#49, #50, #12) |
| B-10 | **Bus bypass** (host reaches peripherals with the BCM powered down; #56) | unknown | [datasheet] | Panel or audio path without full BCM power | Dark-screen power | P18 | read the brief map; probe in stage 5s |
| B-11 | **Idle `sleep` instruction + 1 ms tick** (stage 5i; #3) | built, not run on silicon | [apple] Apple idles this way | Core sleeps between jobs when lit | Lit-idle power | P18, P19 | stage 5i |
| B-12 | **Caches 128 Kbit I and D** (#59) | known-unused (every image runs cache off) | [datasheet]; register `0x10000C00` [apple] | Faster scalar code for the decoder and renderers | Decode speed, power | P19 | enable and benchmark a kernel |
| B-13 | **LCD-controller scaler** ("pixel-level interpolation", #58) | unknown; probably the TV path | [datasheet]; [apple] no LCD scaling writes | Free scaling for art or video | PP/vector cycles | P19, P14 | brief-map PB-9 |
| B-14 | **Audio out block** (I2S, `0x1000_4800`, DMA ch 5) | used by Apple for video; tone on silicon (S35) | [apple]; [silicon] S35 | Video soundtrack; the BCM as a second audio source (clock-slave on the WM8758 clocks, data-paths R6) | Video audio without PP decode | P14 | #35/#39; D6 caveats on shared I2S clocks |
| B-15 | **TV out** (PAL/NTSC, composite or S-Video; timing generator, video DAC, DMA ch 6) | wired and used by Apple | [apple] sequence recovered, run in vc2sim | Videos and Photos on a TV via the headphone-jack AV cable (no dock on the owner's unit) | A feature Apple's 7G had | P14 | PB-1, PB-6 to PB-9 in `brief-map-peripherals.md` |
| B-16 | **64-voice MIDI synth** (brief) | software, not shipped by Apple | [datasheet]; [apple] none | `.mid` and game music on the chip | Extras audio | P12 | none until #54 |
| B-17 | **2-megapixel JPEG decode** | software; our spike (`jpeg-spike.md`) | [ours] | Album-art decode (#77), Photos | CPU/battery in the art builder | P19, P14 | JPEG spike on silicon |
| B-18 | **DMA, 8 channels, 2-D transfers** | used (LCD, audio) | [apple]; [silicon] S5 | Slot-to-LCD pans with the core idle | Pans at 8.6 ms, bus bound | P19 | done for LCD |
| B-19 | Camera, SD/MMC/SDIO, I2C/SPI/UART, USB 1.1, 52 GPIO, JTAG, multi-display, DRM keys (brief-map rows 3, 11-19) | absent or unwired on the iPod | [apple] pin map | A UART for Bluetooth is better on the PP's dock UART | none | - | PB-4 pin census |
| B-20 | **Dual-issue scalar core** (about 1.9 IPC) | used implicitly | [silicon] S6, S7 | Compiler scheduling in `vc2cc` | Decode speed | P19 | done |

## 3. WM8758 (codec / DAC)

| ID | Feature | Status | Evidence | Where used / could be used | ProkBox benefit | Phase | Probe |
|---|---|---|---|---|---|---|---|
| W-1 | **5-band hardware EQ** (EQ1-EQ5, +/-12 dB, 4 frequencies per band, narrow/wide peaks) | used (P15 presets; custom curves to follow) | [datasheet] bands and frequencies; [silicon] S1 audible (owner). The datasheet says EQ2-4 need `M128ENB`=1, but [silicon] S1 heard EQ2 and EQ3 at +12 dB with `M128ENB`=0 (eq.c `IPODUI_HW_EQ` comment), so the shipping low-power mode keeps the peaks on this unit | Settings EQ presets, custom curves (#82) | Zero-CPU EQ | P15 (done), P12 | Read the S1 `eqt` log lines or repeat; set `M128ENB` from `dac_eq_set()` when a band 2-4 gain is non-zero (`wm8758-reference` section 4) |
| W-2 | **DAC / amp / PLL power states** (R1-R3 per-block enables, `SLEEP`, VMID/BIAS) | used (P15 standby) | [silicon] wake 6.4 ms median; [datasheet] playback 24 mW vs standby 0.3 mW typical | Pause standby; the PLL is the I2S clock source for the PP and the BCM (data-paths M13) | Power while paused | P15, P18 | done; BCM audio must hold the PLL (D6); `SLEEP`/MCLK gating open (W-12) |
| W-3 | **Headphone amp: zero-cross volume, split DAC/amp attenuation** | used | [datasheet]; [rockbox-derived] | Volume curve (P15) | Quality, no zipper noise | P15 | done |
| W-4 | **3D enhancement** (`DEPTH3D` R41, 16 steps, 0-100 %) | known-unused (`THREEDCTRL` defined, never written) | [datasheet]. Allowed in the low-power DAC core (no M128ENB cost); wants about 6 dB of headroom, which the EQ precut path gives | A hidden "spatial" setting if a 7G-idiom place exists | Quality, zero CPU | P12 | listening test: write `0x29` |
| W-5 | **DAC digital limiter plus 0..+12 dB boost** (R24-R25: threshold -1..-6 dB, attack 94 us-192 ms, decay 0.75 ms-1.5 s) | known-unused (registers defined, never written) | [datasheet] | A hardware limiter under Volume Limit and EQ boosts; a free digital +12 dB for quiet files / Sound Check; hearing safety | Quality, safety, zero CPU | P12 | tone-burst check of the timings (they scale with the sample rate) |
| W-6 | **DAC filters and mutes**: soft mute (used), auto-mute `AMUTE` (unused), DAC polarity. **No de-emphasis, no filter-shape choice in this part** | partly (soft mute used) | [datasheet]; the earlier de-emphasis hypothesis is false | Auto-mute for idle hiss; de-click ramp on skips (deferred in P15) | Quality | P12 | listen for idle hiss; ramp time not published |
| W-7 | **Mic / line-in ADC, input PGAs, ALC and noise gate, ADC high-pass and notch** | unwired as far as we know (no mic on the 5.5G; Rockbox's line-in was a dock input). ALC, noise gate and filters sit in the ADC path only | [datasheet]; wiring [rockbox-derived] | Voice notes (Notes in P12) only if a mic or accessory input exists | Extras | P12 | check board wiring first |
| W-8 | **Word length and sample rate**: 16/20/24/32-bit I2S/LJ/RJ/DSP; rates 8-48 kHz only (**no 88.2/96 kHz**) | used at 16-bit | [datasheet] codec side ready for 24-bit; PP I2S size field unknown (`audio.md`) | 24-bit depth for hi-res FLAC; rate above 48 kHz is never possible (software resample) | Quality for hi-res FLAC | P19 / P12 | I2S size field `[9:8]` probe |
| W-9 | **OUT2, OUT3/OUT4, mixers, bypass** | not wired / unused (OUT2 fed the dock line-out, which is gone; OUT3/4 muted and off). No beep generator exists in this part | [datasheet]; [rockbox-derived] | none | none | - | - |
| W-10 | **DAC oversampling `DACOSR128`** (0 = 64x lowest power, 1 = 128x) | used at 128x (`audiohw_mute(false)`) | [datasheet] calls 64x lowest power (its noise note is inconsistent); saving not published | Fewer digital milliwatts during playback | Power, if inaudible | P18 / P12 | A/B listen and USB-C meter, 64x vs 128x |
| W-11 | **DSP core low-power mode `M128ENB`=0** (halves core digital power; EQ1/EQ5/3D only) | used (default); EQ2-4 interplay in W-1 | [datasheet] | Keep it for flat/bass/treble; raise it only for peak presets | Power | P15 follow-up | see W-1 |
| W-12 | **`SLEEP` bit (R2) and MCLK gating** | known-unused | [datasheet]; PP-side MCLK gating [hypothesis] | A deeper codec standby than PLL-off (the PP clocks the codec's MCLK) | Small power while paused | P18 | needs the PP's `DEV_EXTCLOCKS` gating tested with the codec in standby |
| W-13 | **Thermal shutdown** (`TSDEN`+`TSOPCTRL`, about 150 C) | used | [datasheet]; [rockbox-derived] | Protection | Safety | - | none |
| W-14 | **GPIO1/CSB pin and jack-detect output switching** ("PLL lock", "Temp ok", "Amute active", per-pin output enables) | unknown board wiring; unused | [datasheet]; wiring [hypothesis] | Measure PLL lock time; hardware output switching | Power, polish | P18 | trace the pin on the board |
| W-15 | **Digital supply scaling**: DCVDD specified 1.71-3.6 V (24.0 mW at 2.5 V vs 17.8 mW at 1.8 V, typical quiescent playback) | unknown which rail feeds DCVDD; PMU sets 2.5 V today | [datasheet]; board and PMU [hypothesis] | The codec's digital current falls about 26 % with a lower DCVDD if the board allows it | Power, the largest codec-side lever | P18 | PMU rail map, then a bench step |
| W-16 | **Bias trims `HALFOPBIAS`, `BIASCUT`; `BUFBIAS`/`ADCBIAS`** | `BIASCUT` used during start-up only; `HALFOPBIAS` unused | [datasheet] "do not use" for HALFOPBIAS (-0.5 mA, worse THD+N); BUFBIAS/ADCBIAS undocumented | none recommended | - | - | none |

## 4. Cross-chip paths (from `docs/data-paths.md`)

| ID | Path | Status | Evidence | Opportunity | Phase |
|---|---|---|---|---|---|
| X-1 | **PP E2 DMA to BCM port** (cell 14) | used | [silicon] S37 | Slot uploads, 16+-row pushes with the CPU asleep | P19 |
| X-2 | **PP COP to do the BCM's feeding** (video, jobs, art decode) | partly | [silicon] S36 | Keep the CPU free for UI | P19 |
| X-3 | **BCM audio on the shared I2S clocks** (R6) | tone on silicon | [silicon] S35, D6 | Video audio entirely off the PP | P14 |
| X-4 | **BCM SRAM as extra RAM** (1.25 MB; SDRAM 4 MB) | used by the video image | [silicon] | Cover Flow textures, Class B/C games, relief for the 32 MB unit | P19, P12 |
| X-5 | **USB-C OTG host + storage** | unknown | [datasheet] | Stick-to-iPod import without a computer | P17, P21 |

## 5. Top cross-chip opportunities (ranked)

1. **BCM display job interface (B-2, B-3) with E2 uploads (P-9, X-1).** The shell sends jobs, not pixels. P19.
2. **BCM as the Cover Flow / compositor engine (B-4, B-6, B-17).** Textures in SRAM or SDRAM, 3D on the vectors. Cover Flow and P19.
3. **Dark and idle power: BCM standby (B-9, B-10, B-11) with PP gating, voltage and clocks (P-3, P-4, P-6, P-14).** P18, with P19 for the BCM side.
4. **COP compute (P-7, P-5).** Art decode, EQ, time-stretch; skipping for finer steps. P19.
5. **PP MAC and 100 MHz (P-1, P-2).** Only if probes confirm. P19, P9b.
6. **Video chain: BCM H.264, BCM audio on the WM8758 clocks, TE sync, TV out (B-7, B-8, B-14, B-15, B-5, X-3).** P14.
7. **WM8758 EQ plus limiter and 3D (W-1, W-4, W-5), and codec power levers (W-10, W-11, W-15).** Quality at zero CPU; the `M128ENB` question in W-1 is resolved on silicon (peaks work at 0). P12, P18.
8. **USB OTG host (P-12, X-5).** Copy off the iPod with no computer. P17, P21.
9. **Hardware PWM backlight/haptic and RTC scratch (P-13, P-14).** Fewer wake-ups; boot guard. P18.
10. **BCM MIDI synth and games on the vectors (B-16, B-6) with WM8758 mixing.** Extras. P12.

## 6. By phase

| Phase | Hardware features to evaluate |
|---|---|
| **P12 Extras** | B-6 (game classes A/B/C), B-4, B-16, W-1 (custom EQ, hardware engine, #82), W-4, W-5, W-6, W-7 (Notes by voice, only if wired), W-10, P-7 (COP for game logic or synths) |
| **P13 Genius mixes** | P-7 (COP for similarity scoring), P-1 (MAC for audio feature extraction), pre-compute at sync time per rule 3 |
| **P14 Photos / Videos** | B-7, B-8, B-14, X-3, B-5, B-13, B-15 (TV out), B-17 (Photos decode), W-2 (keep the PLL for BCM audio), B-1 |
| **P17 Storage & USB** | P-10 (closed), P-11 (IDE IRQ), P-12 / X-5 (USB host), P-4 (gate unused USB port) |
| **P18 Power & instant resume** | P-3, P-4, P-6, P-13, P-14, P-15, B-9, B-10, B-11, W-2, W-10, W-12, W-15, power-contract states C0-C5 |
| **P19 Hardware offload** | B-1 to B-6, B-12, B-13, B-17, B-18, P-1, P-2, P-5, P-7, P-8, P-9, X-1, X-2, X-4, W-8 |
| **P22 Languages** | P-7 (COP for glyph-cache work), B-2 (strip slots for non-Latin marquee text); no unique silicon |
| **Cover Flow (P7)** | B-4, B-6, B-3, B-5, B-17, X-4, P-9 |
| **P9b Final trim** | P-2, P-4, P-8, B-12; re-check every row marked "used" is still gated correctly |
| **P20 Release** | P-16 (unit ID), B-19 (DRM note only) |
| **P21 Sync** | P-12 (USB host as a sync path), X-5 |

## 7. Keeping it current

- Any phase that probes a row updates its status, evidence tag and probe result here in the same change, and records the finding in the owning manual (`pp5022-reference/quirks.md`, `bcm2722-reference/quirks.md`, `data-paths.md`).
- When `bcm2722-reference/datasheet-features.md` merges, replace section 2's source pointer with a link and reconcile row IDs.
