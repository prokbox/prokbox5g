# Hardware diagrams

Diagrams for the ProkBox hardware manual (iPod Video 5G/5.5G, the owner's modded unit). Each one is a
D2 source (`.d2`) rendered to SVG (for print and docs) and PNG (for previews).

| Diagram | Shows |
|---|---|
| `system.d2` | The whole system: PP5022, SDRAM, flash, USB-C, PMU and battery, click wheel, haptics, WM8758 and headphone jack, BCM2722 with its SRAM, SDRAM, host port and LCD/TV outputs, the I²C links and the shared I²S clocks |
| `audio-bus.d2` | The I²S bus as proven in device sessions 32–34: the WM8758 as clock master, its clocks reaching both the PP's I²S block and the BCM's GPIO 2/3, the measured edge rates, GPIO 1 floating, and where the two data lines meet [hypothesis] |
| `video-path.d2` | A video frame's journey: flash → PP (demux, AAC to I²S, A/V clock) → host port → BCM SDRAM FIFO → H.264 decode → colour conversion → LCD DMA → panel, plus the overlay path and the status/events back to the host |
| `data-paths.d2` | **Data paths (a), the overview:** the chips, their memories, the outside world (computer over USB-C, the flash, the panel, the headphone jack, the absent dock line out) and only the main paths, coloured by status. Start here; it links the three below |
| `data-paths-pp.d2` | **Data paths (b), the PP5022 side:** every PP master (CPU, COP, general DMA ch 0 and ch 1–3, the second DMA engine, IDE, USB) and what it reaches, in two panels (into the PP's own memory; out to the host port and the I2S FIFO), with status and measured rates |
| `data-paths-bcm.d2` | **Data paths (c), the BCM2722 side:** the host port's window onto SRAM, SDRAM and the registers; the scalar core, vector unit and DMA channels against the BCM's memories; the way out to the LCD and the audio block |
| `data-paths-audio.d2` | **Data paths (d), audio:** PP DMA ch 0 → I2S → WM8758; the WM8758 as clock master (BCLK/LRCK to the PP and to the BCM's GPIO 2/3, only while its PLL runs; MCLK from the PP); I2C control; the BCM's audio block; OUT1 → the headphone jack (with GPIOA bit 7 headphone detect) and OUT2 → the dock line out, not fitted on this unit |
| `memory-maps.d2` | The PP5022 address space, the PP's 64 MB DRAM layout, and our video image's BCM2722 SRAM and SDRAM maps as address bars |

## Rendering

```sh
docs/diagrams/render.sh                 # all diagrams
docs/diagrams/render.sh audio-bus       # one
LAYOUT=elk docs/diagrams/render.sh      # another layout engine
```

Needs **D2 v0.9.0**. The layout engine is **TALA** (bundled with d2 0.9.0; it gave the cleanest
orthogonal routing with the fewest crossings here). ELK and dagre work but route the system diagram worse.
The data-path set (`data-paths*.d2`) is the exception: `render.sh` lays it out with **ELK**, whose layered
routing kept every edge label clear of the others there, where TALA's moved between runs.
`render.sh` uses `--pad 40`. The PNGs come out at 2× the SVG's size, so they stay sharp when zoomed.
The system diagram is the densest: read it at about 2000 px wide, or as the SVG. The others are legible
at smaller sizes. The data-path set is drawn to be read at a laptop's normal zoom and on paper (portrait):
block text 26 px on pages about 1500–1900 px wide.

## Style conventions

The data-path set has its own, larger style: `paths-style.d2` (imported with `...@paths-style`) and the
status key `paths-legend.d2` (`legend: @paths-legend`). Edge colour is the status from `docs/data-paths.md`
§4: green solid = works on our unit [silicon]; green dotted = a clock line; red dashed = dead end;
purple dashed = Apple only; grey dotted = unknown; pale grey dashed = not fitted to this unit. Chips are
soft rounded containers in their subsystem colour; the outside world (computer, flash, panel, jack) is a
warm, rounded block; anything not fitted is pale and dashed. Arrows run from the master to the target it
reaches; a number in brackets is the matrix cell. Titles 40 px, chip names 30 px, blocks 26 px, edge labels
and notes 24 px. The rest of this section is the style of the other diagrams.

Shared styles live in `style.d2` (imported with `...@style`). The shared legend is in `legend.d2` (imported
with `legend: @legend`). The memory maps have their own key, since their colours mean who writes a region.

- **Font:** D2's built-in Source Sans Pro (SIL OFL), embedded in each SVG. Title 36 px; cluster titles 24 px;
  blocks 20 px; edge labels 19 px; notes 20 px; footnotes 18 px.
- **Colours, one per subsystem** (pale fill, darker stroke and text):

  | Subsystem | Fill | Stroke |
  |---|---|---|
  | PP5022 (main processor) | `#EAF2FB` | `#2B6CB0` |
  | BCM2722 (video co-processor) | `#F1EAFB` | `#6B46C1` |
  | Audio | `#E6F5EC` | `#2F855A` |
  | Power | `#FDF3DE` | `#B7791F` |
  | Storage & USB | `#E2F4F4` | `#2C7A7B` |
  | Input | `#FCE8EE` | `#B83280` |
  | Memory / display | `#F2F4F7` | `#4A5568` |

- **Lines:** solid black = data; dashed orange = control / I²C; dotted green = clock; thick amber = power;
  dashed grey = uncertain link.
- **Uncertain or absent parts:** a dashed grey outline in italics. Uncertain links and blocks also say
  "[hypothesis]" in the label.
- **Every diagram has** a title and subtitle (top), a legend (bottom right) and a source and evidence
  footnote (bottom).

## Evidence

Facts come from the repo, not from memory, and use the BCM2722 manual's tags
(`docs/bcm2722-reference/index.md`): **[silicon Sn]** measured on this unit in device session n;
**[apple]** read from Apple's firmware; **[rockbox]** from Rockbox's source; **[ours]** a convention of our
own image or protocol, not of the chip; **[hypothesis]** not verified.

| Diagram | Sources |
|---|---|
| system | `docs/hardware.md`; `docs/bcm2722-reference/` (overview, memory-map, memory-access, host-interface, audio, lcd); `firmware/export/config/ipodvideo.h` (PCF50605, WM8758, PP5020 I²C, ARC USB, scroll wheel, hardware click); `firmware/export/pp5020.h` (peripheral addresses); `firmware/target/arm/pp/wmcodec-pp.c` (I²C 0x1A); `firmware/drivers/pcf50605.c` (I²C 0x08); `firmware/target/arm/ipod/` (adc-ipod-pcf.c, power-ipod.c, piezo.c, button-clickwheel.c) |
| audio-bus | `docs/bcm2722-reference/audio.md` §2–§5, `open-questions.md` #35/#39, `docs/research/bcm2722/audio-spike.md` §8; the device logs `session log, session 32`, `session33-*/` and `session34-*/bcmtest*.log`. Stage 8a-0 gave GPIO 2 at 284,726 / 285,106 / 257,208 edges/s and GPIO 3 at 88,198, sampled at 630–635 k samples/s. Stage 8a-0b (S34) showed GPIO 1 following the pulls with the PP playing, its TX stopped and audio off |
| video-path | `docs/bcm2722-reference/host-driver.md`, `memory-access.md` §7b (the PP's DMA into the port delivered nothing in S34; the CPU copy is the default), `docs/research/bcm2722/pipeline.md`, `player.md` |
| data-paths, -pp, -bcm | `docs/data-paths.md` (each edge is a matrix cell there, with its session or commit) |
| data-paths-audio | `docs/data-paths.md` §4 cells 6, 7, 10, 22, 26, 27, 29, R6, R9, D6; `docs/pp5022-reference/audio.md` (OUT1 = headphone amp, OUT2 = line out, MCLK 24 MHz) and `gpio-i2c.md` §3 (GPIOA bit 7 = headphone detect). **Unconfirmed, and said so on the diagram:** which WM8758 pins are wired to the jack, and how the BCM's GPIO 1 joins DACDAT |
| memory-maps | `docs/bcm2722-reference/memory-access.md` §2–§3 (on branch `p14-dev41` and later), `memory-map.md`; `firmware/export/pp5020.h`, `firmware/target/arm/pp/app-pp.lds`; `docs/hardware.md` (`_end`, audio-buffer size) |

**Not in the docs (drawn as unknown):** the charger IC, how the TV output reaches a connector, the exact
haptic drive (the owner's "most likely the piezo line"), and how the BCM's GPIO 1 reaches the WM8758's
DACDAT. The S33/S34 audio results are in the session logs, not yet in `audio.md`; the manual should take
them in at its next update.

## Later: a board illustration (not done yet)

A top-down drawing of the logic board, traced using iFixit's iPod 5th generation teardown photos as
reference only (our own artwork, no copied photos). It would need:

- reference photos of both board sides at a known scale, from the teardown and from the owner's unit,
  which is the authority, since the mods move parts (iFlash adapter, USB-C board, 3000 mAh cell, haptic motor);
- part identification for each package: PP5022, the SDRAM, BCM2722, WM8758, PCF50605, the charger,
  the click-wheel flex connector, the LCD flex, the headphone jack, and where the 30-pin connector used to be;
- the board outline and connector positions measured, in mm;
- the owner's photos of the mod wiring (USB-C, haptics, battery);
- a drawing tool with layers (Inkscape) and the colour code above, with labels kept off the parts;
- a check that nothing in it is copied from iFixit's images (CC BY-NC-SA): trace only positions and outlines, then attribute the reference.
