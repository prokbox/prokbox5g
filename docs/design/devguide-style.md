# ProkBox Developer Manuals: Style Guide

Status: draft 3 for tracker #161. Imported into the firmware repo as `docs/design/devguide-style.md`.

This guide sets the shape, conventions and voice for the ProkBox manuals: one large developer reference, published as a single web page, plus a separate user guide. It contains no hardware facts of its own. Every register, address and figure in an example is there to show the *form*. Each one carries an evidence tag like any other statement, and none may be copied into a manual without checking it against its source.

**What changed in draft 3**

The owner's decisions on draft 2 are folded in:
- A sixth evidence tag, `[external]`, for community research (§4.1, §4.5).
- `HB` covers both the teardown tour and platform theory, and comes first (§2.2).
- Parts run hardware first: `HB`, `PP`, `VC`, `WM`, `FW`, `GS` (§2.2).
- The volume's source is `docs/manual/` in the firmware repo, published through the curated GitHub mirror at big releases (§8.1).

The warning budget of about one per chapter stays (§5.2).

**What changed in draft 2**

- **Grounded in the actual documents.** Every convention now names the classic manuals it comes from. The study notes are in `refs-notes/`; §11 lists them.
- **One volume.** The developer manuals become one volume made of Parts, designed to be read as one huge web page (§2, §7). The user guide stays separate.
- **Two more manuals.** The release set gains the user guide (`UG`) and the 5.5G hardware breakdown (`HB`).
- **Owner decisions folded in.** There is a fifth tag, `[datasheet]`, a marker for 5G and 5.5G differences, and Markdown with a renderer as the output (§4, §5.11, §8).
- **Fits the existing manuals.** The tag scheme now maps the tags already used in `docs/pp5022-reference/` and `docs/bcm2722-reference/` (§4.5). The Part outlines follow the files those manuals already have. The BCM2722 Part gains the instruction-set chapters that draft 1 left out.
- **Rewritten examples.** The example paragraphs (§9.4) are rewritten to sit closer to the best of the references, still entirely in our own words.

---

## 1. What we are borrowing, and from whom

We borrow habits, never text. The table names the habit, where it is strongest, and where it lands in this guide.

| Habit | Strongest in | Where it lands here |
|---|---|---|
| Explain the machine before tabulating it | Amiga HRM; the IIgs Hardware Reference shows the cost of not doing so | §3.2 |
| Anticipate the tempting mistake and close it off before the reader acts | Amiga HRM; Palm OS Companion's worked trap | §3.8, §9 |
| A register heading that works as a reference card | Pan Docs; IIgs (address in every caption) | §3.4 |
| One full sentence for each value of a tricky bit | IIgs Hardware Reference | §3.4 |
| Tell the reader which few registers or calls they actually need | Inside Macintosh ("Using the…") | §3.6 |
| Say how to leave a block safely idle | ARM7TDMI TRM | §3.6 |
| Document undefined behaviour as carefully as defined behaviour | GBATEK ("Unpredictable Things"); ARM's exact use of *unpredictable* and *undefined* | §3.9, §4.4 |
| Model differences marked at field level, with a delta appendix | GBATEK; IIgs; ARM revision appendix | §5.11 |
| Timing shown first as shape, then as numbers; cost in a small alphabet; a worked latency sum | ARM7TDMI TRM | §3.5 |
| Formula on its own line, then one worked number at our clock | Inside Macintosh; C64 PRG; Amiga HRM | §5.4 |
| Overview and reference for each module, cross-linked | Be Book; Palm Companion with its Reference; C64 (tutorial chapter with datasheet appendix) | §2.3 (`GS`), §5.5 |
| Callbacks you implement kept apart from functions you call | Be Book (hook functions) | §5.5 |
| An entry template that is itself documented before any entry appears | NeXTSTEP General Reference; Inside Macintosh's "Format of a Typical Chapter" | §5.5, front matter |
| Callout levels defined once, with what each obliges the reader to do; warnings rare | NeXTSTEP (notes can be skipped, warnings cannot); ARM (3 cautions in 276 pages) | §5.2 |
| A line-by-line table after every teaching listing | C64 PRG | §5.6 |
| Real shipped code as the canonical listing | Palm OS Companion | §3.7, §5.6 |
| "Why this platform is different" as chapter 1 of a programming guide | Palm OS Companion; Inside Macintosh's Road Map | §2.3 (`FW`, `GS`) |
| Symptom, then commonest cause, then fix | NeXTSTEP; Palm OS Companion | §3.8, `UG` |
| Index locators carrying a part prefix | Inside Macintosh's combined index | §5.9 |
| Glossary senses numbered or prefixed by context | Amiga HRM; NeXTSTEP | §5.8 |
| Claims that need checking, boxed and linked to an open issue | Pan Docs ("To be verified") | §4.3 |
| Evidence you can run | Pan Docs (test ROMs) | §4.2 |
| Anchors made from unique, descriptive headings, so links survive splitting | GBATEK (770 anchors, none broken); Pan Docs (fragile `-1`, `-2` suffixes on repeated headings) | §7.3 |

And what we refuse, with the manual that taught us to:

- **No sales voice.** Avoid the C64 PRG's chapters and the IIgs chapter openers.
- **No reprinted vendor datasheets or schematics.** The C64 and the IIgs both bound them in.
- **No page-number cross-references.** Every print-era book used them.
- **No missing values in timing tables.** The ARM TRM sends every value to "your supplier".
- **No bare question mark as a confidence marker** (GBATEK), and no hedge words standing in for evidence (Pan Docs).
- **No restating the same facts in About, Using, Reference and Summary sections** (1990s Inside Macintosh).
- **No callout levels without defined meanings.** Palm's IMPORTANT and the Be Book's four box types both lacked them.
- **No untested example code** (the Be Book), and no disclaimers on sample code (Inside Macintosh).
- **No unnumbered sections** (the Be Book, Pan Docs, GBATEK).
- **No trademark saturation** (Palm, NeXT).

---

## 2. The volume and the user guide

### 2.1 Two publications

| Publication | Readers | Form |
|---|---|---|
| ***The ProkBox Reference: iPod Video 5G and 5.5G*** | Developers, reverse engineers, repairers, the curious | One volume of six Parts, published as one large web page with a split per-chapter edition from the same source (§7) |
| ***ProkBox User Guide*** | Owners; no technical knowledge assumed | Its own small site, task-led, with screenshots (§2.4) |

The user guide stays separate because its readers, voice and conventions share almost nothing with the reference. It links into the reference only from its "Getting help" appendix.

### 2.2 The Parts of the reference

Each Part keeps a two-letter prefix. The prefix is the Part's identity in every cross-reference, anchor and index locator. If the volume is ever split back into separate books, each book simply keeps its prefix and nothing needs renumbering (§7.2).

| Order | Prefix | Part | Purpose | Voice model |
|---|---|---|---|---|
| I | `HB` | **The 5.5G Platform: a Hardware Breakdown** | Front door to the volume. A teardown-style tour of the board, then the platform as a whole: how the chips work together, bring-up order, and a feature map pointing into the chip Parts | IIgs Hardware Reference chapter 1 (written for a reader who may pick up a soldering iron) |
| II | `PP` | **The PP5022** | The main SoC, both ARM7TDMI cores and the board peripherals it drives | Amiga HRM for theory; ARM TRM for reference entries |
| III | `VC` | **The BCM2722** | The VideoCore II: host interface, boot, instruction sets, memory, display, TV out | As `PP` |
| IV | `WM` | **The WM8758** | The audio codec as wired in this machine | As `PP`; IIgs sound chapter for "correcting the name" |
| V | `FW` | **Writing Firmware** | The ProkBox firmware: how it is built, how it works, how to change it | Inside Macintosh (1985) and NeXTSTEP *Development Tools* |
| VI | `GS` | **Writing Games** | The games SDK, from first program to finished game | Be Book overviews; C64 PRG tutorials, without the sales voice |
| — | — | **Back matter** | Shared appendices, glossary and index for the whole volume (§2.3, end) | — |

**Why `HB` comes first** (owner decision, draft 3). The owner's brief for the breakdown asks it to bring the three chip Parts together into a general, software-agnostic account of the platform for someone starting from scratch. `HB` opens with the teardown tour (HB-1 to HB-10) and then gives the platform theory (HB-11 to HB-13). The Parts run hardware first, then the two guides. A reader who opens the volume at the top should meet the whole machine before its parts, which is the Amiga HRM's order at the scale of a book. Readers who already know the board skip straight to the Part they need, and the front matter says so (Inside Macintosh's habit of telling readers what they may skip).

**Front matter of the volume.**

```
About this reference      Who it is for; reading routes by reader (newcomer, driver author, game author, repairer)
How to read an entry      The block layout (§3), the register heading, the API entry template (§5.5), shown once with a live example
Evidence tags             The six tags, the markers, the rule for upgrading a tag (§4)
Notation                  Numbers, units, signals, unpredictable/undefined/reserved (§4.4, §5.4)
Models                    5G and 5.5G: what differs and how differences are marked (§5.11)
Reporting errors          What a useful report contains; where to file it
Revision history          Per Part, admitting corrections (§5.10)
```

### 2.3 Part skeletons

Chapter numbers are fixed once published. New chapters go after the last body chapter of their Part, never in the middle. Chapters marked † use the standard block layout (§3), and chapters marked ‡ use the tour layout (§3.11). Where a chapter grows from an existing seed file in the firmware repo, the file is named in brackets.

#### Part I · HB · The 5.5G Platform: a Hardware Breakdown

```
HB-1   Opening the case              Tools, ESD, the battery, the clips; disassembly as photographs              ‡
HB-2   The board at a glance         Both sides annotated; every callout numbered for the whole Part             ‡
HB-3   The PP5022 and its memory     SDRAM, flash, IRAM; pointers into PP                                        ‡
HB-4   The BCM2722 and the display   Its SDRAM, the LCD and connector; pointers into VC                          ‡
HB-5   The WM8758 and the audio path Codec to headphone socket and dock; pointers into WM                        ‡
HB-6   Storage                       Hard disk or flash mod, connector, cable                                    ‡
HB-7   Power                         Battery, PMU, charging, rails, test points                                  ‡
HB-8   Inputs                        Click wheel, its controller and flex; the hold switch                       ‡
HB-9   The dock connector            Pin by pin, with what each pin reaches on the board                          ‡
HB-10  Test points and debug access  Serial, JTAG candidates, where to probe safely                              ‡
HB-11  How the parts work together   Data paths between the chips, who masters which bus, clocks shared across chips  [data-paths.md]
HB-12  Bringing up the platform      The order in which a from-scratch kernel must wake each part, and why
HB-13  Feature map                   Every feature of the device (boot, clocks, memory, cores, LCD, video, TV out, audio,
                                     headphone detect, remote, dock and serial, USB, click wheel, hold, backlight, PMU,
                                     battery, RTC, storage, sleep) → the chapters that document it, with coverage status
HB-14  5G versus 5.5G                The board-level differences, side by side
```

HB-13 is the definition-of-done checklist in the owner's words: someone who reads the volume can build a kernel that drives every feature. It is generated from the Parts' headings and evidence tags, so gaps show as gaps.

#### Part II · PP · The PP5022

```
PP-1   Overview                       The SoC in one diagram; what each later chapter covers         [overview.md]
PP-2   The ARM7TDMI cores             What we rely on from ARM's manual; the CPU and the COP; ARM and Thumb;
                                      the multiplier and long multiply-accumulate                    [cores-and-threads.md, mac-and-dvs.md]
PP-3   Memory map                                                                                     [memory-map.md] †
PP-4   Boot                           Boot ROM, loaders, the first instruction of loaded code          [boot.md]
PP-5   Clocks, PLLs and power                                                                         [clocks-power.md] †
PP-6   Interrupts and timers                                                                          [interrupts-timers.md] †
PP-7   The two cores together         Start-up of the COP, mailboxes, sharing, cache coherency       [cores-and-threads.md, apple-dual-core.md] †
PP-8   DMA                                                                                            [dma.md] †
PP-9   GPIO and I²C                                                                                   [gpio-i2c.md] †
PP-10  Storage interface                                                                              [storage.md] †
PP-11  USB                                                                                            [usb.md] †
PP-12  The display port to the BCM2722                                                                [display.md] †
PP-13  Audio interface (I²S)                                                                          [audio.md] †
PP-14  Input: the click wheel and buttons                                                             [input.md] †
PP-15  Power management as Apple does it   Sleep states, DVS, what Apple's firmware does and when    [apple-power.md, mac-and-dvs.md]
PP-16  Unpredictable behaviour        Open-bus reads, mirrors, narrow writes, what happens out of spec  [quirks.md]
Appendices
PP-A   Register summary by address    · PP-B  Register summary by name (one shared legend)
PP-C   Interrupt sources               · PP-D  GPIO assignments (5G and 5.5G)
PP-E   Electrical and timing summary, with a master symbol list
PP-F   Differences from the PP5020 and PP5024
PP-G   Features in the product briefs that we have not yet used                                       [datasheet-features.md]
PP-H   Frequently used tables          · PP-I  Open questions                                         [open-questions.md]
PP-J   Sources                                                                                        [sources.md]
```

#### Part III · VC · The BCM2722

```
VC-1   Overview and how to program it  What the VideoCore II does in this machine; the programming model in one chapter  [overview.md, programming.md]
VC-2   Memory map                                                                                     [memory-map.md] †
VC-3   Memory access                   Caches, alignment, what each master sees                       [memory-access.md] †
VC-4   Boot and clocks                                                                                [boot-and-clocks.md] †
VC-5   The host interface              The link to the PP5022, seen from both sides                   [host-interface.md] †
VC-6   Scalar instruction set          Encoding, every instruction, cycle costs                       [isa-scalar.md]
VC-7   Vector instruction set          The 2-D register file, encodings, cycle costs                  [isa-vector.md]
VC-8   DMA                                                                                            [dma.md] †
VC-9   The LCD                                                                                        [lcd.md] †
VC-10  Interrupts, timers and GPIO                                                                    [interrupts-timers-gpio.md] †
VC-11  SDRAM                                                                                          [sdram.md] †
VC-12  Audio on the VideoCore                                                                         [audio.md] †
VC-13  Power                                                                                          [power.md] †
VC-14  Unpredictable behaviour and withdrawn readings                                                 [quirks.md]
VC-15  Writing VideoCore programs     Toolchain, our host protocol, program swapping                  [programming-guide.md, program-swap.md]
VC-16  The host driver                Session lifecycle, panel ownership, failure signatures          [host-driver.md]
VC-17  Graphics                       2-D and 3-D on the chip; the reference engine as worked example  [graphics.md]
Appendices
VC-A   Instruction summary by mnemonic and by encoding · VC-B  Register summary
VC-C   Pixel format cards               · VC-D  Timing diagrams
VC-E   Open questions                                                                                  [open-questions.md]
VC-F   Sources                                                                                         [sources.md]
```

The ISA chapters do not use the block layout. They follow the ARM TRM's instruction chapters: a legend first, then one entry per instruction (mnemonic, encoding diagram, operation, flags, cycles, evidence), then a cost summary.

#### Part IV · WM · The WM8758

```
WM-1   Overview                       The codec's role, how it is wired here, block diagram
WM-2   Control interface              Register writes over I²C; why every register needs a shadow copy  †
WM-3   Clocking                       MCLK, PLL, sample rates, with the arithmetic for our clock          †
WM-4   Digital audio interface        Formats, word length, master and slave                              †
WM-5   DAC path and digital filters                                                                       †
WM-6   ADC path and microphone input                                                                      †
WM-7   Mixers and routing                                                                                 †
WM-8   Output stages                  Headphone, line out, dock                                           †
WM-9   Equaliser and 3-D enhancement                                                                      †
WM-10  Power-up, power-down and pop suppression   Sequences as "you do / the codec does" tables          †
WM-11  Recipes                        Application notes: volume ramps, EQ presets, headphone detect
Appendices
WM-A   Register summary               · WM-B  Power sequences as tables
WM-C   Measured output characteristics on this board
WM-D   Open questions                 · WM-E  Sources
```

#### Part V · FW · Writing Firmware

```
Part V opens with "Before you start": what you must unlearn if you come from desktop Linux or from Rockbox (Inside Macintosh's Road Map).
FW-1   Why this platform is different   Two cores, a video processor you program yourself, a slow disk, little RAM, a battery
FW-2   The whole process in one chapter  From clone to release, one sentence per stage, each pointing to its chapter (NeXTSTEP)
FW-3   Setting up the toolchain
FW-4   Building and installing          Including recovering a device you have made unbootable
FW-5   A first change                   A visible change on real hardware, walked step by step
FW-6   The boot chain
FW-7   Memory layout and linking
FW-8   The kernel: threads, scheduling, the CPU and the COP
FW-9   Interrupts and timing
FW-10  Drivers: the pattern every driver follows
FW-11  Storage and the file system
FW-12  Display and the VideoCore
FW-13  Audio
FW-14  Input
FW-15  Power, battery and sleep
FW-16  USB
FW-17  Debugging without a debugger     Each tool with how it fails: what it misses, what it falsely reports (NeXTSTEP)
FW-18  Testing on hardware              Bench sittings, recording evidence, test programs as evidence
FW-19  Performance work
FW-20  Contributing                     House style (this guide), review, how to tag what you learnt
Appendices
FW-A   Build options                    · FW-B  Memory map as used by ProkBox
FW-C   Crash dump format                · FW-D  Reset and recovery combinations: what each clears, what survives (Palm)
FW-E   Troubleshooting by symptom       · FW-F  Command and effect tables for every tool
```

#### Part VI · GS · Writing Games

Each module chapter has two halves, as in the Be Book: an overview (what it is for, how to choose between its parts, a worked example built in stages) and a reference (the entries, §5.5). The halves link to each other at the top. The reference halves are gathered in GS-A.

```
GS-1   Why this platform is different   A wheel, not a joystick; a small screen; a battery; what the chips can do for you
GS-2   Installing the SDK
GS-3   Hello, wheel                     A complete small program with a line-by-line table
GS-4   The game loop                    Callbacks you implement; frames, time, staying responsive
GS-5   Resource classes                 Lightweight, borrows RAM, exclusive; declaring your class and minimum RAM
GS-6   Drawing on the PP5022            Pixels, sprites, tiles, text
GS-7   Drawing on the BCM2722           Offloading to the VideoCore; the reference engine as worked example, not a mandate
GS-8   Input                            Angle, speed, buttons; control schemes that suit a wheel
GS-9   Sound                            Choosing between effects and streamed music; mixing within budget
GS-10  Saving                           Files, settings, high scores
GS-11  Performance budgets              Where the frame time goes, as a worked sum
GS-12  Battery-friendly games
GS-13  Packaging and distribution
GS-14  Graduated projects               Draw a pixel → a one-button game → a multi-screen game with saves → extending the SDK
Appendices
GS-A   API reference (all modules)      · GS-B  Limits and budgets
GS-C   Result codes                     · GS-D  Porting notes
GS-E   Indexes by kind: functions, types, constants (Be Book)
```

#### Back matter (whole volume)

```
Appendix Z1   Suggested reading, annotated: datasheets, the ARM7TDMI TRM, Rockbox sources, community research (NeXTSTEP)
Appendix Z2   Volume-wide register index by name, with Part prefix
Appendix Z3   Volume-wide open questions, gathered from every chapter's last section
Glossary      One for the volume (§5.8)
Index         One for the volume, with Part-prefixed locators (§5.9)
```

### 2.4 The user guide (separate)

Task-led. Every chapter title names something an owner wants to do, and every section is one task.

```
Front matter  Welcome · Using this guide (one line per chapter) · Safety and care
 1  Meet your ProkBox          The controls and screen; a labelled photograph; how to hold the wheel
 2  Installing ProkBox         Backing up first; installing; going back to the original firmware
 3  Getting music on           Copying, playlists, album art
 4  Listening                  Playing, the wheel while playing, queues, shuffle, equaliser
 5  Watching video             Supported files, converting, TV out
 6  Playing games              Finding, installing and playing; controls on the wheel
 7  Settings                   One section per settings screen, in menu order
 8  Battery and charging       Getting the most from a charge; what the icons mean
 9  When something goes wrong  By symptom ("the screen stays black"), then cause, then fix; resets and recovery
Appendices
 A  Menu map                   · B  Supported file formats
 C  Specifications in plain words, with measured battery life
 D  Button combinations: you hold / the player does
 E  Getting help
Glossary in plain words · Index
```

---

## 3. The standard block layout

Every † chapter uses these sections in this order. Readers learn the layout once and navigate by habit, as with the NeXTSTEP and Be Book entry templates. A section with nothing to say keeps its heading and one line explaining why.

```
<Prefix>-N     <Block name>                        chapter header block (§3.1)
<Prefix>-N.1   Overview
<Prefix>-N.2   Theory of operation
<Prefix>-N.3   Register map
<Prefix>-N.4   Register descriptions
<Prefix>-N.5   Timing
<Prefix>-N.6   Programming model                   ends with "Leaving the block idle"
<Prefix>-N.7   Worked examples
<Prefix>-N.8   Gotchas
<Prefix>-N.9   Evidence and open questions
               Key points                          unnumbered, optional, for hard chapters
```

This is the brief's order. Draft 1 added §N.4 and §N.9, and draft 2 keeps them. The study confirmed the order: the IIgs puts theory after its registers, and its sound chapter is hard going because of it.

### 3.1 Chapter header block

Three fixed lines before §N.1. The idea comes from NeXTSTEP's chapter header and ARM's opening sentence plus section list.

```markdown
## PP-6 Interrupts and timers

| Base | `0x6000_4000` (interrupt controller), `0x6000_5000` (timers) [rockbox-derived] |
|---|---|
| Clock and reset | Always clocked; no reset control [hypothesis] |
| Interrupt lines | Timer 1 and Timer 2, one each, to either core [rockbox-derived] |
| Models | Same on 5G and 5.5G [rockbox-derived] |

How each core is interrupted, how to steer and mask a source, and the timers that are its commonest users.
This chapter does not cover the COP mailboxes; see §PP-7.4, Mailboxes.
```

The last line is the Palm and Pan Docs scope sentence: what the chapter is not about, and where that lives.

### 3.2 Overview (N.1)

Half a page at most. What the block is for, what it connects to, and the one thing the reader must know before going further. It ends with the whole-system block diagram with this block highlighted (IIgs: every subsystem chapter opens with the same diagram, with the subject picked out).

### 3.3 Theory of operation (N.2)

Prose and diagrams. Register names appear only in passing. Describe the block as a machine: where data enters, what state it keeps, what changes that state, where data leaves. State diagrams and conceptual timing diagrams belong here (ARM: shape first, numbers in §N.5).

Three habits from the study:

- **Correct a misleading name early.** The IIgs sound chapter opens by pointing out that its "oscillators" are really address generators. When a block's official name misleads, say so in the first paragraph of theory.
- **Pseudo-code before code.** The Amiga HRM teaches the Copper in plain-English steps before any listing. Use this wherever a block is driven by a sequence.
- **End with "the registers you need".** Inside Macintosh's "Using the…" habit, in one paragraph: of the block's twenty registers, the common case touches these three.

The test of a good theory section: someone who has read only this section should be able to guess roughly what registers the block will have.

### 3.4 Register map and register descriptions (N.3, N.4)

**The map** is one table in address order, with every register, including unknown ones (GBATEK keeps unknowns in the map rather than omitting them). Unknowns carry `[hypothesis]` and a §N.9 line.

```markdown
Table PP-6.1. Timer registers, base `0x6000_5000` [rockbox-derived]

| Offset | Name         | Access | Reset       | Summary                                     | Evidence          |
|--------|--------------|--------|-------------|---------------------------------------------|-------------------|
| 0x000  | `TMR1_CFG`   | RW     | 0x0000_0000 | Timer 1 configuration                       | [rockbox-derived] |
| 0x004  | `TMR1_VAL`   | RC     | —           | Timer 1 count; reading clears the interrupt | [silicon S12]     |
| 0x008  | `TMR2_CFG`   | RW     | 0x0000_0000 | Timer 2 configuration                       | [rockbox-derived] |
| 0x00C  | `TMR2_VAL`   | RC     | —           | Timer 2 count; reading clears the interrupt | [rockbox-derived] |
| 0x010  | `USEC`       | R      | —           | Free-running microsecond counter            | [rockbox-derived] |
```

Access codes: `R`, `W`, `RW`, `RC` (read clears), `W1C`, `W1S`, `WO` (write-only: reading returns nothing useful). The codes are defined once in front matter, in one legend shared by the map tables and the register appendices (Amiga Appendix B's single legend). A register that is replicated per channel is drawn as a grid, channel against register (IIgs), and described once (GBATEK).

**Each description** opens with a heading that works as a reference card (Pan Docs' heading formula, IIgs' address-in-every-caption):

```markdown
#### `TMR1_CFG` — Timer 1 configuration · `0x6000_5000` · RW · [rockbox-derived]
```

The order is fixed: name, purpose, address (or the register index for the WM8758), access, evidence tag. The heading text is unique in the whole volume, because it starts with the register name, so the anchor generated from it survives any reshuffle (§7.3). Model scope goes in the heading when the register exists on one model only: `· (5.5G only)`.

Then the bit diagram, the field table, and any prose:

```
TMR1_CFG   0x6000_5000   RW   reset 0x0000_0000

 31  30  29  28  27  26  25  24  23  22  21  20  19  18  17  16
┌───┬───┬───────────────────────────────────────────────────────┐
│EN │RPT│                      reserved                         │
└───┴───┴───────────────────────────────────────────────────────┘
 15  14  13  12  11  10   9   8   7   6   5   4   3   2   1   0
┌───────────────────────────────────────────────────────────────┐
│                         RELOAD[15:0]                          │
└───────────────────────────────────────────────────────────────┘
```

| Bits | Field | Access | Reset | Description | Evidence |
|---|---|---|---|---|---|
| 31 | `EN` | RW | 0 | Enable. `0` = stopped, `1` = counting down from `RELOAD`. | [silicon S12] |
| 30 | `RPT` | RW | 0 | Repeat. `0` = stop at zero, `1` = reload at zero and continue. | [rockbox-derived] |
| 29:16 | — | — | 0 | Reserved. Write 0. | [hypothesis] |
| 15:0 | `RELOAD` | RW | 0 | Count in microseconds, loaded on start and on each repeat. | [rockbox-derived] |

Rules:

- **Short enumerations go inline** in the description (`0` = off, `1` = on), as GBATEK does. Longer ones get a nested table.
- **Give each value its own full-sentence row** when a bit is active-low, inhibits something, or is otherwise easy to misread (the IIgs "Bits in…" table):

  | `HOLD_N` | Meaning |
  |---|---|
  | `0` | The hold switch is on. The click wheel and buttons are ignored by Apple's firmware; the hardware still reports them. [apple-RE] |
  | `1` | The hold switch is off. |
- **A paragraph after the table** explains the interactions the table can't show (Inside Macintosh), followed by a two- to four-line read-modify-write example if fields share the register.
- **Every bit appears** in the table, reserved bits included. The volume-wide rule for writing reserved bits is stated once, in front matter (the IIgs "Altering registers" policy; ARM states it once too). Registers that break the rule say so in their own Warning: the IIgs' sound register, where read-modify-write is forbidden, is the model.
- **Bit diagrams are generated** from a compact source in the renderer (Pan Docs), with the box-drawing form above as the fallback rendering, so every diagram in the volume is drawn the same way. The WM8758's registers draw one 9-bit row with the 7-bit register address in the heading.
- **The end of the chapter** has a one-line-per-register summary (Inside Macintosh's chapter summary, with descriptions, not the bare names Palm used). The Part appendices then repeat all registers sorted by address and by name (Amiga).

### 3.5 Timing (N.5)

Three forms, from the ARM TRM:

1. **Parametric diagrams** in monospace, time running left to right, each interval named `t` plus a mnemonic, with `r` and `f` for rising and falling edges. Each diagram is followed by its table:

   ```
            ┌──────────┐
   SCL  ────┘          └──────────
          ┌───────────────┐
   SDA  ──┘               └───────
          │◄─ tSU ─►│
   ```

   | Symbol | Parameter | Min | Typ | Max | Unit | Evidence |
   |---|---|---|---|---|---|---|
   | tSU | Data set-up before clock rise | 250 | — | — | ns | [datasheet] |

   Every row has a value, or `—` with an evidence tag and a §N.9 line saying why it is missing. ARM leaves every value to "your supplier". We have no supplier, so we never do that.
2. **Cost in a small alphabet.** Where operations cost bus cycles, define a few cycle types once at the head of the Part (ARM's N, S, I and C) and give each operation's cost as a formula in them. This suits SDRAM against IRAM accesses, cache misses and BCM2722 host transfers.
3. **The worked sum.** Latencies and deadlines are given as labelled terms, each defined, valued and tagged, then added and converted to time at a stated clock (ARM §2.9). See §9.4 for an example.

Where the same interval matters in several units, give cycles, microseconds and a percentage of the budget side by side (GBATEK). If a block's timing is invisible to software, say so in one sentence and stop.

### 3.6 Programming model (N.6)

How to drive the block, as numbered steps for each operation: initialise, use, shut down, recover. Ordering constraints, required delays and locking rules go here. Enumerated cases for counted calls, such as nested enable and disable of a clock, get an explicit case table (Inside Macintosh's EnableIdle list).

It ends with a fixed subsection:

> **Leaving the block idle.** What to write so the block draws no power and raises no interrupts when you are not using it, and what happens if code touches it afterwards. *(ARM's "If you are not using…" sections.)*

### 3.7 Worked examples (N.7)

One to four per block, each self-contained and runnable:

- **Titled by what it achieves**, not by what it uses: "Waiting 10 ms without spinning".
- **C first**; assembly only when the hardware demands it.
- **Every listing includes one header**, `prokbox_hw.h`, and uses register names, never bare addresses (Amiga Appendix I).
- **Prefer real ProkBox code**, captioned with its source file (Palm's use of the shipped calendar's event loop). A toy example is acceptable only when real code would hide the point.
- **Built in CI.** No example is printed unless it compiles with the firmware's warning flags. This is the answer to the Be Book's untested examples.
- **Say what you should observe** on the hardware, so the reader can tell it worked.
- **About 40 lines at most.**

### 3.8 Gotchas (N.8)

Numbered, titled, each a short entry. Gotchas are first-class content: the chips are no longer made, so the traps we find are the most valuable thing we write down. Dead ends and features we abandoned belong here too, with what they taught us.

Two forms, chosen by the gotcha:

- **The tempting mistake** (Amiga HRM, Palm's worked trap): the obvious approach, why it fails, one specific case, the defence.
- **Symptom, cause, fix** (NeXTSTEP, Palm): what you see, the commonest cause, what to do.

> **PP-6.8.2. A stopped timer can still be pending.**
> The obvious way to restart a timer is to clear `EN`, write the new reload, and unmask the source. It fails the first time the old timer had already expired: clearing `EN` stops the count but leaves the interrupt raised, so it is delivered the moment you unmask, one full period early. Read `TMR1_VAL` before unmasking; the read clears it. [silicon S12]

Gotchas are numbered so code comments can cite them: `/* PP-6.8.2 */`. Where a "To be verified" box (§4.3) applies, it sits under the gotcha.

### 3.9 Evidence and open questions (N.9)

A list of everything in the chapter that is unproven, what would prove it, and the tracker issue tracking it. Every `[hypothesis]` in the chapter has a line here. When a question is settled, the tag is upgraded where it appears, the line moves to the revision history, and the withdrawn reading is kept in the Part's Unpredictable behaviour chapter (`PP-16`, `VC-14`), so nobody works it out again.

### 3.10 Key points (optional)

A hard chapter may close with five or six bullets that say what to remember (Amiga HRM). It is a summary, not a restatement: no new facts and no tags.

### 3.11 The tour layout (HB-1 to HB-10)

```
HB-N     <Area of the board>
HB-N.1   What you are looking at     Annotated photograph or line drawing; numbered callouts
HB-N.2   The parts                   Table: callout, designator, part, function, evidence, where documented (§Part-N)
HB-N.3   How it connects             Signals in and out; a small block diagram
HB-N.4   Electrical                  Signal / pin / measured / max / unit / evidence (IIgs connector tables)
HB-N.5   Things to notice            Repair hazards, fragile connectors, where people go wrong
HB-N.6   Evidence and open questions
```

Callout numbers run through the whole Part, so that the parts list can name every part once. `HB` never documents registers. It points to the Part that does.

---

## 4. Evidence

### 4.1 The six tags

Every statement of fact about the hardware, Apple's firmware or measured behaviour carries one tag. Statements about ProkBox's own code, which the reader can check in the source, carry the `[ours]` marker (§4.5) instead.

| Tag | Meaning | Required support |
|---|---|---|
| `[silicon]` | Measured on a real device with our own code | The sitting or session (`[silicon S12]`, "sitting 3, 2026-10-26, session log"); the test program, by name, that a reader can run |
| `[datasheet]` | From a published vendor datasheet, product brief, application note or technical reference manual (including ARM's) | Document title, revision, section. Note when our board wires the part differently from the vendor's reference design |
| `[apple-RE]` | Read from Apple's firmware by reverse engineering, statically or under our simulator | Firmware version and the address (`osos 0x1000_xxxx`); never quoted code. It says what Apple's code *does*; any meaning we attach is tagged separately |
| `[rockbox-derived]` | From Rockbox source or its tracker | File and commit. A value Rockbox writes is known to work; the meaning Rockbox gives it is only as good as its comment |
| `[external]` | From community research: the iPodLinux project, the siggifly emulator notes, freemyipod and similar | The source and its own numbering ("siggi 08"). Where the source keeps its own provenance (what came from a hardware read, what from a guess), carry it over: their guesses are our `[hypothesis]` |
| `[hypothesis]` | Our inference, unproven | A §N.9 line saying how it could be tested |

**Strength order:** `silicon`, `datasheet`, `apple-RE`, `rockbox-derived`, `external`, `hypothesis`. A datasheet describes the part, not our board. A datasheet fact about how a pin is used in this machine is only as good as our knowledge of the wiring, so pair it with `[silicon]` once measured.

### 4.2 Placement and citation

- **In prose**, the tag goes after the full stop of the sentence it supports.
- **In tables**, use an **Evidence** column.
- **In headings**, the tag goes on the heading line, after the name, so it reads like the Inside Macintosh trap tag sitting beside a routine's name (§3.4).
- **Several sources:** give the strongest first, `[silicon] [rockbox-derived]`. Where sources disagree, tag each claim separately and say so in prose. Never silently pick one.
- **Session qualifiers** are allowed: `[silicon S7]`, `[rockbox comment]`, `[apple strings]`. A qualified tag counts as its base tag.
- **Citations of device evidence** use "sitting N, date, session log", never a backup path, a home directory, a host name or the name of a photograph.
- **Test programs as evidence.** A `[silicon]` claim should name the test in the repository that demonstrates it, so a reader with a device can run it (Pan Docs' test ROMs).

### 4.3 "To be verified"

The long form of `[hypothesis]`, from Pan Docs, for claims important enough to need explaining:

> **To be verified.** Writes to `TMR2_CFG` while Timer 2 is counting appear to take effect at the next reload rather than at once. We believe this because Apple's firmware always stops the timer first. Tracked in issue #NNN. [hypothesis]

The box says what we believe, why, and where the work is tracked. Do not leave TODOs in the text. Pan Docs and the 1985 Inside Macintosh drafts both shipped them.

### 4.4 Words with fixed meanings

Defined once in Notation and used only in these senses (ARM):

| Word | Meaning |
|---|---|
| *reserved* | Has no documented function. Write the value the front-matter rule gives; do not rely on what it reads |
| *undefined* | We do not know, or the result varies between runs or units. Do not depend on it |
| *unpredictable* | The result is some value, but which one is not fixed. The machine stays in a usable state |
| *must*, *must not* | A requirement: breaking it causes the stated failure |
| *we recommend* | Advice: the reason is always given |

When we *do* know what undefined behaviour does on our units, it goes in the Part's Unpredictable behaviour chapter with its tag. That is GBATEK's open-bus chapter. It is documentation, not a promise.

### 4.5 Markers and the existing manuals

The PP5022 and BCM2722 manuals already in the firmware repo use more tags than the six agreed for the volume. At import, the renderer accepts the old forms as aliases and a one-off sweep rewrites the source:

| Found in the repo | Count (2026-11-06) | Becomes |
|---|---|---|
| `[apple]` | 625 | `[apple-RE]` |
| `[rockbox]` | 453 | `[rockbox-derived]` |
| `[measured]` | 10 | `[silicon]` |
| `[external]` where the source is a vendor document (product briefs, ARM TRM) | part of 155 | `[datasheet]` |
| `[external]` where the source is community research (iPodLinux, the siggifly emulator notes, freemyipod) | rest of 155 | `[external]` (unchanged) |

Some markers are not evidence tags. They stay, styled differently from evidence (grey, not coloured):

| Marker | Meaning |
|---|---|
| `[ours]` | A convention our firmware defines: a memory split, our host protocol, a driver policy. Not a property of the hardware; another design can replace it |
| `[sim]` | A result from our simulator or another model only. Never cited as evidence about silicon |
| `[estimate]` | Computed from measurements or instruction counts, not itself measured |
| `[unknown]`, `[dead end]` | Status markers in data-path tables: not yet tried; tried and abandoned (with the reason) |

**Rule for editors** (already in both manuals, kept): never upgrade a tag without new evidence. When silicon contradicts an earlier reading, state the silicon result and keep the withdrawn reading in the Unpredictable behaviour chapter.

### 4.6 Rendering of tags

Tags render in small type in a muted colour per tag, as the existing renderer already does. `[hypothesis]` is also set in italic, so unproven claims stand out at a glance. The renderer never drops a tag. A tag hovered or tapped shows its full citation from `docs/evidence/` or the Part's Sources appendix.

---

## 5. Conventions

### 5.1 Numbering and cross-references

- **Section numbers** carry the Part prefix and go three levels deep: `PP-6`, `PP-6.4`, `PP-6.4.2`. Deeper headings are unnumbered.
- **Cross-references** always give the number and the title, never "above" or "see page": `§PP-6.4, Steering a source`. The number makes it citable in code and issues; the title makes it readable and survives a renumbering mistake (ARM names targets by title; we add the number).
- **Figures, tables and examples** are numbered per chapter: `Figure PP-6.1`, `Table WM-3.2`, `Example GS-6.3`. Gotchas take their section number: `PP-6.8.2`.
- **Registers in prose** are always in code style and always link to their heading.
- **Writers link by target, not by file.** `[§PP-6.4](#pp-6.4)`, or `` [`TMR1_CFG`](#reg-tmr1_cfg) ``. The renderer resolves the target wherever the section lives (Pan Docs' title-link preprocessor). A link that does not resolve fails the build.

### 5.2 Callouts

Three kinds, each defined in front matter by what it asks of the reader (NeXTSTEP):

| Kind | What it asks | Budget |
|---|---|---|
| **Note** | Nothing. Safe to skip. Background, usually *why* | As needed, but prefer prose |
| **Tip** | Consider it. A better way, learnt the hard way | A few per chapter |
| **Warning** | Read it. Doing this wrong loses data, damages hardware or leaves the device unbootable. Say which, and how to recover | About one per chapter at most (ARM: 3 cautions in 276 pages) |

A callout may carry a subject after its kind, `**Warning: data partition.**` (Pan Docs: severity by kind, subject by title). There are no other kinds. "Important" is not a level, which Palm shows: it ends up used for everything from trivia to data loss. Never repeat a callout verbatim within a chapter. Link back to the first one.

**Run-in notes** are a lighter device, separate from callouts. They are a bold label opening an ordinary paragraph, which a reader can skip without losing the thread (Inside Macintosh's "Assembly-language note", the Amiga's "About X."):

> **Hardware note.** `pb_set_volume()` writes the WM8758's headphone volume registers for both channels and then sets the update bit, so the two sides change on the same sample. See §WM-8.4.

Use "Hardware note" in `FW` and `GS` for the register-level reality under an API, and "Model note" for 5G and 5.5G asides.

### 5.3 Tables

- **Caption** above the table, numbered: "Table PP-6.1. Timer registers, base `0x6000_5000`".
- **Column order:** identifier, then the values a reader looks up, then explanation, then evidence.
- **No empty cells.** `—` means "not applicable", unless a table note says it also means "unknown" in that table.
- **Footnotes on one cell or column** use a symbol (`*`, `†`) and sit directly under the table (C64).
- **State and action tables** may use sentence-pair headers, "You do… / The codec does…", which read well for power sequences, button combinations and build options (Palm).
- **Turn a table round, or split it,** rather than crowd it with model columns (Palm's version tables).

### 5.4 Numbers, units and notation

| Thing | Convention | Example |
|---|---|---|
| Hexadecimal | `0x`, upper-case digits, `_` every four digits in 32-bit values. Never `$`, a trailing `h`, or decimal-first | `0x6000_5000`, `0x1F` |
| Binary | `0b`, `_` every four digits; `x` for don't-care in patterns (C64) | `0b1010_0001`, `0bxxx1_0000` |
| Decimal | Plain in code; comma thousands separator in prose | `65536`; "65,536 entries" |
| Bit fields | `NAME[high:low]`; one bit is `NAME.FIELD` | `RELOAD[15:0]`, `TMR1_CFG.EN` |
| Memory sizes | KiB, MiB; kB, GB only for decimal quantities such as disk capacity as sold | `96 KiB`, `30 GB` |
| Frequencies and times | SI, space before the unit | `80 MHz`, `200 µs` |
| Signals | Upper case, code style; active-low with a trailing `_N`; never ARM's leading `n` | `LCD_CS_N` |
| Levels | *assert* and *deassert* for logic; HIGH and LOW only for measured voltages | — |
| Byte order | Stated in one sentence wherever data crosses a boundary (Palm) | "The host sends each 32-bit word low half first." |
| Direction | `→` host to device, `←` device to host, on protocol and mailbox fields (Inside Macintosh's parameter-block arrows) | `→ cmd`, `← status` |
| Ranges | En dash in prose; `..` in code-like contexts | "bits 15–0"; `0x00..0x7F` |

**Formulas** go on their own line, given generally, then specialised to our clock with one worked number (Inside Macintosh, C64, Amiga):

```
f_sample = f_MCLK / (256 × D)

With f_MCLK = 11.2896 MHz and D = 1:  f_sample = 44.1 kHz   [datasheet]
```

"Set" means write 1 and "clear" means write 0.

### 5.5 API entries (`GS`, `FW`)

**The template is documented first.** The front matter of `GS` explains every field of an entry, with one live example, before any entry appears (NeXTSTEP, Inside Macintosh).

**Each module's reference page** opens with a header block (the Be Book's class header, adapted):

| Header | `<prokbox/sprite.h>` |
|---|---|
| Link with | `libpb` |
| Context | Main loop only. Not from the audio callback or the COP |
| Ownership | Sprites belong to the caller; surfaces belong to the SDK |
| Since | SDK 0.3 |

Then fixed sections in a fixed order, each with a contents box at the top:

1. **Callbacks you implement.** For each: when it is called, on which core, under what lock, and what it must not do. This category is the Be Book's clearest idea.
2. **Functions you call.** Related functions are grouped in one entry, such as a setter with its getter, or lock with unlock with try-lock.
3. **Constants and types.**

**Each entry:**

```
pb_draw_sprite
    void pb_draw_sprite(pb_surface *dst, const pb_sprite *spr, int x, int y);

Draws spr onto dst with its top-left corner at (x, y), clipped to dst.

Parameters   → dst  Destination surface. Must not be NULL.
             → spr  Sprite to draw. Its palette must match dst's format.
             → x,y  Position in pixels. Any value; off-surface parts are clipped.
Returns      Nothing.
Context      Main loop.
Prerequisites  pb_video_init() has returned PB_OK.
Errors       None. Debug builds assert on a palette mismatch.
Cost         About 2 cycles per opaque pixel; transparent runs are skipped. [estimate]
Notes        Opaque runs longer than 32 pixels are copied by DMA.
See also     pb_draw_tile, §GS-6.3 Sprites.
```

The fields borrow from three sources:

- **Prerequisites** and the cost line take the place of the C64 KERNAL entry's prerequisites and registers-affected fields.
- **Units and ranges** are stated in the parameter list, including what happens out of range (Be Book).
- **Result codes** are a complete table, with no "other errors" catch-all. The SDK tests that every documented code can actually be returned, which answers the Be Book's entries that contradicted their own tables.
- **Version-dependent statements** begin "Currently…" and name the SDK version (Be Book, Palm).

Signatures are generated from the SDK headers, so the reference cannot drift from the code.

**Indexes by kind** (functions, types, constants) are generated for `GS-E` and sit alongside the subject index (Be Book).

### 5.6 Code listings

- Fenced, with a language: `c`, `asm`, `sh`, `text`.
- Shell commands show a `$ ` prompt, and output has none. Never mix the two in a block meant for copying.
- Elided code is `/* ... */` on its own line, never a bare `...`.
- **Teaching listings in `GS` and `FW` part 1** are followed by a two-column table explaining the lines that matter (the C64's line-by-line tables):

  | Line | What it does |
  |---|---|
  | 4 | Asks for the wheel's position as an angle, 0 to 95, where 0 is the top of the wheel. |
  | 7 | Converts the change in angle since the last frame into the ship's turn. |
- **Example chains.** A later example may be shown as an edit of an earlier one, "Example GS-8.2 modified", with only the changed lines. Each chain has at most three steps.

### 5.7 Appendices

Lettered within each Part (`PP-A`, `PP-B`…). The volume's shared appendices are `Z1` onwards, at the back. Every hardware Part has these:

- register summaries by address and by name, sharing one legend (Amiga);
- frequently used tables, each pointing back to its original table number (IIgs);
- differences between models and chip revisions: a summary list, then one subsection per change (ARM, GBATEK's delta style);
- open questions;
- sources.

### 5.8 Glossary

- **One glossary for the volume**, in one source file.
- **Entries** give the term in bold, its expansion if it is an abbreviation, and a definition of two or three sentences written for this hardware rather than for computing in general. The ARM and IIgs glossaries both padded themselves with general terms. Each entry ends with the section where the term is properly introduced.
- **A term with different senses in different Parts** numbers its senses, each prefixed by context (Amiga's numbered senses; NeXTSTEP's "In the codec, …"):

  > **frame**
  > 1. *In the display:* one complete image sent to the LCD. §VC-9.2.
  > 2. *In audio:* one sample for every channel at one instant. §WM-4.2.
  > 3. *In a game:* one pass of the game loop. §GS-4.1.

- **First use** of a glossary term in each chapter is in italics, and the renderer links it to the glossary.
- **Chip names** are glossary entries too. The volume uses the technical names (PP5022, CPU and COP, BCM2722 or VideoCore, WM8758). The project's shorthand (PP1, PP2, VC, DAC) appears once, in a glossary line mapping it to them.

### 5.9 Index

One index for the whole volume. Its locators carry the Part prefix, `PP-6.4` and `WM-3.2`, with the key printed at the head of the index. This is the Inside Macintosh combined index, which used exactly this two-letter scheme.

Entries are marked in the source:

```markdown
The microsecond counter<!-- idx: timers; microsecond counter --> runs whenever...
```

- `<!-- idx: primary; secondary -->` goes straight after the word.
- Register names, API names and gotchas are indexed automatically. Gotchas are indexed under the thing that bites and under "gotchas".
- Index terms are singular nouns in the form a reader would look up: "interrupts, masking", not "masking interrupts".
- GBATEK's "index" is really a second table of contents, and Pan Docs has none. Search does not replace an index: it cannot tell a reader where a term is *defined*.

### 5.10 Revision history

Per Part, in front matter. It admits corrections in plain words, such as "§VC-5.3 said the link was 16 bits wide; it is 8. Withdrawn reading kept in §VC-14.6." The 1985 Inside Macintosh drafts did this on their covers, and it is the nearest thing in any classic manual to our evidence tags.

### 5.11 5G and 5.5G differences

The owner's decision, with the three-level scheme from the IIgs and field-level marking from GBATEK:

1. **Volume-wide rule** in front matter: everything applies to both models unless marked.
2. **One-line differences:** an inline marker straight after the thing that differs: "64 MiB of SDRAM (5.5G only)", "(5G only)". It may sit on a bit-table row or in a register heading. Always this exact form; never "5.5G-only" or "5.5G models only".
3. **Whole blocks:** a two-column table headed **5G** and **5.5G**, with one row per difference, introduced by a sentence saying what does not differ.
4. **Whole chapters that differ little:** a delta section, "As on the 5G, except…", with a link (GBATEK).
5. **Where nothing differs in a chapter,** say so once, in the chapter header block's Models line, rather than marking every line.
6. **An unproven difference** carries its tag: "(5.5G only) [hypothesis]".

Capacities (30, 60, 80 GB) and the flash mod are named the same way where they matter: "(flash only)".

---

## 6. The user guide's conventions

- **Numbered steps,** one action each, naming the control in bold as it appears on screen: "Choose **Settings**, then **Playback**."
- **A screenshot** at each point where the reader should check they are in the right place. Screenshots come from the simulator's shot script (`scripts/shots.sh`) using open or synthetic media only, never the owner's library. NeXTSTEP shows the risk of leaning on screenshots: its tutorials can't be followed without them, and screenshots date. So every step must still make sense with its screenshot missing.
- **Only Note and Warning.** A Warning is kept for losing music or data, or for damaging the device.
- **No register names, firmware paths or evidence tags** in the body. Measured claims, such as battery life, carry a footnote naming the test, so the evidence is still there for anyone who looks.
- **Troubleshooting by symptom,** then cause, then fix (NeXTSTEP, Palm). Button combinations go in a "You hold / The player does" table (Palm's reset chapter).
- **"Using this guide"** gives one line per chapter, phrased as what the reader will find there (ARM).
- The hidden firmware folder on the disk is never named. Call it "the system folder" if it must be mentioned.

---

## 7. One volume, one huge page

### 7.1 What the study found

| | Pan Docs `single.html` | GBATEK |
|---|---|---|
| Size | 712 KB (176 KB compressed) | 5.0 MB (1.3 MB compressed) |
| Load (curl, no browser) | 0.24–0.49 s | 9.7 s uncompressed, 2.7 s compressed |
| Requests | 8 scripts, 9 stylesheets, fonts; search index 2.1 MB separate | 2 |
| How it is made | Afterthought: `print.html` with links rewritten by text matching | Generated from one help-text source with the author's own tool |
| Anchors | Heading slugs; repeated headings get `-1`, `-2` and break when chapters move; 5 broken links | 770, made from unique titles; none duplicated or broken |
| Sticky table of contents | Yes, highlights the current chapter (not section) | No; contents only at the top |
| Search | Client-side, press `s` | Browser find only |
| Breadcrumb / "where am I" | None | None |
| Section numbers | None | None |
| Glossary / index | Neither | Neither (its "index" repeats the contents) |

**What works.** One file to keep offline and search with the browser's find. Links are instant. Anchors made from unique titles survive splitting and merging. Both outputs come from one source.

**What works badly.** Deep in a 5 MB page, a reader has no idea where they are. A first load over a slow link is long, and heavy on a phone. Pan Docs' single page is a by-product, held together with link rewriting. Neither has section numbers to orient a scrolling reader, and neither has an index.

**Our size, for scale.** The PP5022 and BCM2722 seed manuals are already about 0.94 MB of Markdown together. The full volume, with `WM`, `HB`, `FW` and `GS`, will plausibly be 2–3 MB of Markdown, or about 3–4 MB of HTML, roughly 0.8–1 MB compressed [estimate]. Uncompressed, that is closer to GBATEK than to Pan Docs, so the page must be built with that weight in mind.

### 7.2 Structure and numbering

- **Parts and prefixes** as in §2.2. A section's number *is* its identity: `PP-6.4` names the same section in the single page, in the split edition, in a code comment and in a tracker issue.
- **Splitting later.** If the owner ever wants separate books again, each Part becomes a book with the same prefix and the same numbers. Cross-references keep working, because they name the Part.
- **The volume's back matter** (glossary, index, Z appendices) is shared. In a split edition it becomes a seventh small book that every other book links to.

### 7.3 Anchors

- **Every numbered section** has the anchor `#pp-6.4`, made from its number, not its title. Numbers are fixed once published, so these anchors never move.
- **Every register** has `#reg-<name>`, and every API entry has `#api-<name>`. Register and function names are unique in the volume, which the build checks.
- **Every gotcha** has `#pp-6.8.2`, the same scheme as sections.
- **Headings may also carry a title slug** as a secondary anchor for readability. That slug is never the target of an internal link.
- **The build fails** on a duplicate anchor or an unresolved link, so Pan Docs' `-1`/`-2` drift cannot happen.
- **Every heading shows a link symbol** on hover that copies its permanent URL.

### 7.4 Navigation on one huge page

- **Sticky, collapsible table of contents** on the left. It shows Parts, then chapters, then sections of the current chapter only, and highlights the current *section*. Pan Docs only highlights the chapter, and a reader loses their place in a long chapter.
- **Breadcrumb bar** fixed at the top: `Part II · PP › PP-6 Interrupts and timers › PP-6.4 Steering a source`. It updates as the reader scrolls. It answers "where am I" without the table of contents open, which matters on a phone.
- **Visible section numbers** in every heading, so a reader scrolling without the table of contents can still orient themselves.
- **Search.** A search box over a prebuilt index of headings, register names, API names, glossary terms and index entries, not full text. Full text is what the browser's find already does well on a single page. A focused index stays small, and it is loaded only when the search box is first used. Results show the section number and breadcrumb.
- **Previous and next chapter links** at the end of each chapter, and a "top of Part" link in the breadcrumb.
- **The page works without JavaScript.** Every anchor and link works as plain HTML. Scripts add the sticky contents highlighting, the breadcrumb and search, and nothing else.

### 7.5 Weight and load

- **Inline SVG** for diagrams, never raster images of diagrams.
- **Photographs** (mostly in `HB`) load lazily, use modern compressed formats, and appear as thumbnails that open full size.
- **One stylesheet, one small script, no web fonts.** A system font stack and a monospace stack.
- **Server compression on.** At about 1 MB compressed, the first load is closer to Pan Docs than to GBATEK.
- **No tracking** of any kind. GBATEK carries a tracking pixel.

### 7.6 Recommendation

**Build one source into two outputs on every build: the single page and a split edition of one page per chapter.** The renderer emits both from the same parsed document, with identical anchors. The split edition is what search engines and phones land on. The single page is the one to read at a desk, save offline and search with the browser's find. Each page links to the same section in the other edition.

Why both rather than the single page alone:

- At an estimated 3–4 MB, the single page is acceptable on a desk and poor on a phone.
- The split edition costs almost nothing once the renderer exists.
- It also gives the safety valve if the volume ever grows past what a browser lays out comfortably.

The existing `scripts/bcmref_html.py` already renders both seed manuals into one site, one page per file. It is the natural starting point: extend it, rather than adopt mdBook and inherit Pan Docs' single-page hack.

**Printing** is deferred, by owner decision. Meanwhile:

- the single page carries a plain print stylesheet that hides the navigation and breaks pages before each Part;
- nothing in the source may depend on paper: no page numbers and no "on the facing page".

---

## 8. File layout, rendering, decisions and follow-up

### 8.1 Layout

The two seed manuals already live in `docs/pp5022-reference/` and `docs/bcm2722-reference/`, one Markdown file per topic. The volume keeps that shape and adds the rest:

```
docs/manual/                 the volume (public source)
  front/                     front matter (§2.2)
  hb/ pp/ vc/ wm/ fw/ gs/    one file per chapter: NN-short-name.md
  back/                      Z appendices, glossary.md, index terms
docs/evidence/               files resolved from tag citations
docs/user/                   the user guide, with screens/ and its own glossary
```

**Source path and publication** (owner decision, draft 3). The volume's source lives in `docs/manual/` in the firmware repo. It reaches the public only through the curated GitHub mirror, at big releases, and goes through the same scrub as everything else in the mirror. Nothing in `docs/manual/` may depend on files that stay private. That includes `docs/reference/`, Apple firmware, bench logs and the owner's media. Citations point to `docs/evidence/` or to public sources.

Moving `docs/pp5022-reference/` and `docs/bcm2722-reference/` into `docs/manual/pp/` and `docs/manual/vc/` is a later, mechanical step. It needs:
- the renderer's alias support (§4.5);
- a redirect map for the old file names;
- the tag sweep;
- a check that no moved file cites a private path.

### 8.2 Decisions

Settled by the owner after draft 1:

- **Datasheet facts** get their own tag, `[datasheet]` (§4.1).
- **5G and 5.5G differences** use an inline marker for one-liners and a two-column table for whole blocks (§5.11).
- **Output** is Markdown plus a renderer; PDF layout rules come later (§7.6).
- **The developer manuals become one volume** of Parts, published as one large page; the user guide stays separate (§2, §7).

Settled by the owner after draft 2:

- **Community research** gets a sixth tag, `[external]`, ranked between `rockbox-derived` and `hypothesis` (§4.1).
- **`HB` covers both** a teardown tour (HB-1 to HB-10) and platform theory (HB-11 to HB-13) (§2.3).
- **Part order is hardware first:** `HB`, `PP`, `VC`, `WM`, `FW`, `GS` (§2.2).
- **The volume's source is `docs/manual/`**, published through the curated GitHub mirror at big releases (§8.1).

No questions are open.

### 8.3 Follow-up work this guide implies

1. **Renderer.** Extend `bcmref_html.py` to emit the single page and the split edition from one parse. It also needs to resolve `§` cross-references and `#reg-` and `#api-` links, accept the tag aliases, generate bit diagrams from compact source, and fail on broken or duplicate anchors (§7).
2. **Index and glossary generators.** Build the index from `<!-- idx: -->` markers, register names, API names and gotchas, and link first uses to the glossary.
3. **Lint.** Fail on:
   - an untagged fact row;
   - a `[hypothesis]` with no §N.9 line;
   - a TODO in published text;
   - a US spelling from a short deny-list;
   - a home path or backup path in a citation;
   - a link or citation from `docs/manual/` to a private file;
   - the hidden folder's name.
4. **Feature-coverage report** (`HB-13`), generated from headings and tags.
5. **One-off tag sweep** of the two seed manuals (§4.5).
6. **Chapter templates** generated from the skeletons in §2.3.

---

## 9. Tone and voice

### 9.1 Principles

1. **Second person, present tense, active voice** for the reader's actions; the hardware as the subject when describing its behaviour (Amiga, IIgs, ARM). "You write the reload value, then set `EN`. The timer counts down from it."
2. **Plain and precise.** "Use", not "utilise". A number wherever one is known; "we do not know" wherever it is not.
3. **Confident where we know, candid where we do not.** The tags carry uncertainty, so the prose does not hedge sentence by sentence. Pan Docs' "speculated" and "believed" are exactly what our tags replace.
4. **Explain why.** ARM almost never does, and our readers are reverse engineers who need the reasoning. Amiga and IIgs do it constantly, and they are better books for it.
5. **Anticipate the wrong idea** and close it off before the reader acts (Amiga).
6. **Tell readers what they can skip** (Inside Macintosh, both eras).
7. **Rationed dry humour.** At most one aside per overview or tutorial page, placed after the fact it decorates, never in a Warning or a reference entry, and never at the reader's expense (Be Book, NeXTSTEP's examples). None in the hardware Parts' reference sections.
8. **No marketing.** No "powerful", "seamless", "easy", "outstanding". If it is easy, the reader will notice.
9. **"iPod" names the hardware once per chapter.** After that, write "the player" or "the 5.5G" (Palm and NeXT show the effect of saturating a trademark).
10. **UK spelling throughout:** behaviour, colour, initialise, centre, analogue, visualiser, favourites; licence (noun) and license (verb); disc for optical, disk for magnetic; "program" for software.

### 9.2 Voice by Part

| Part | Register | Sentences | Modelled on |
|---|---|---|---|
| `HB` tour | A guide at your shoulder while the case is open | Medium | IIgs chapter 1: the reader may hold a soldering iron |
| `HB` platform | Explanatory, system-level, unhurried | Medium to long | Amiga HRM's system chapters |
| `PP`, `VC`, `WM` theory | Experienced colleague: why, then what | Medium | Amiga HRM |
| `PP`, `VC`, `WM` entries | Terse and exact; verb-first descriptions | Short | ARM TRM; NeXTSTEP's "Returns…" entries |
| `FW` | Senior engineer briefing a newcomer, with opinions and reasons | Medium | Inside Macintosh (1985); Palm Companion |
| `GS` overviews | Patient teacher who also makes games; contractions allowed | Medium, varied | Be Book overviews |
| `GS` tutorials | Tutor beside you at the keyboard; "let's" allowed | Short to medium | C64 PRG, without the exclamation marks |
| `UG` | Plain, kind, exact; no jargon | Short | — |

### 9.3 Things to avoid

- **Hedging stacks** like "it may possibly be the case that…". Pick a tag and state the claim.
- **"Simply", "just", "obviously", "of course".**
- **Paragraphs that announce the next paragraph.**
- **Passives that hide the actor.** "The bit is cleared" must say whether software or hardware clears it.
- **Future tense for normal behaviour.** "The timer counts down", not "will count down".
- **Physics primers inside hardware chapters.** The Amiga and C64 sound chapters both spend pages on what sound is. Give one short aside and a reading-list entry instead.
- **Copied wording from any source**, including the manuals in §11, datasheets, Rockbox comments, Apple strings and community notes. Paraphrase and cite. GBATEK has no open licence, and the Be Book's is non-derivative, so even their table layouts are habits to learn from, not material to adapt.

### 9.4 Example paragraphs

Written fresh for this guide to show the voice in each register. Figures, register names, session numbers and tags in these examples are illustrative: they show where a tag goes, not a citation. None may be copied into a manual as fact.

**`HB` tour:**

> With the back shell off and the battery folded aside, you are looking at the underside of the main board. The large square part near the centre, callout 12, is the PP5022; everything else on this side exists to feed it, power it or carry its signals somewhere. Before you go further, find the two flex cables along the top edge. The left one carries the click wheel and the right one the display, and the right one tears if you lift the board by its corner. Slide a fingernail under the board's opposite edge instead.

**`PP` theory (tempting mistake):**

> Each core has its own interrupt mask, and a source can be steered to the CPU, the COP, both or neither. [rockbox-derived] It is tempting to treat the mask as an off switch: mask a source, do your work, unmask it, and carry on. That works until the source fires while it is masked. The request is not thrown away. It waits, and it is delivered the instant you unmask, from whatever state your code happens to be in. [silicon S9] So decide, before you mask anything, what should happen to a request that arrives in the meantime, and clear it deliberately if the answer is "nothing".

**`VC` reference entry (terse):**

> #### `HOST_CMD` — Command word from the host · `0x1F8` · W · [rockbox-derived]
> Starts a command. Write the parameters first, then this word; the write is what the video processor waits for. Reads return the last word written, not the command's status. [silicon S4] Status is in `HOST_STAT`, §VC-5.4.

**`WM` theory (correcting the name):**

> The datasheet calls two of the output pairs "headphone" and "speaker", and neither name describes this board. Both are ordinary output drivers; on the player one pair feeds the headphone socket and the other the line-level pins of the dock connector. [hypothesis] Read every "speaker" register in this Part as "dock line out", and you will not be surprised when a speaker setting changes what your car stereo hears.

**`PP` gotcha (symptom, cause, fix):**

> **PP-9.8.3. I²C reads return the previous byte.**
> *Symptom:* each value you read from the codec's neighbour on the bus is the one you expected last time. *Cause:* reading the data register before the controller reports the transfer complete; you get whatever the register held before. [silicon S15] *Fix:* poll the busy bit to clear before reading, never a fixed delay, which works at one clock speed and fails at another.

**A worked latency sum (`PP-6.5`):**

> Worst case from a timer interrupt being raised to the first instruction of its handler, with the CPU at 80 MHz [rockbox-derived]:
>
> | Term | What it is | Cycles | Evidence |
> |---|---|---|---|
> | T_sync | Interrupt synchronised into the core | 2 | [datasheet] |
> | T_ldm | Longest instruction already running (`LDM` of 16 registers from SDRAM) | 40 | [estimate] |
> | T_exc | Exception entry and the vector fetch | 3 | [datasheet] |
> | T_dispatch | Our dispatcher reading the status and branching | 18 | [estimate] |
> | **Total** | | **63** | **≈ 0.79 µs** [estimate] |

**`FW` opening ("Before you start"):**

> If you come to this firmware from desktop Linux, three habits will hurt you here. You cannot allocate memory whenever you like: almost everything is sized at build time, and what is left is counted in kilobytes. You cannot wait for the disk: it sleeps to save the battery, takes a second or more to spin up [silicon], and a thread that blocks on it freezes everything that shares its core. And you cannot assume one processor: two cores share one bus, and the video processor runs code you wrote yourself. Chapter FW-1 explains each of these in turn. If you come from Rockbox, read it anyway: some of what you know is still true, and the chapter says which.

**`GS` overview (one rationed aside):**

> The click wheel is not a joystick, and the games that feel best on it stop pretending it is. It reports where a thumb sits on a circle and how fast it is moving, and it does both very well. Steering a ship by angle feels natural within a minute. Steering it with four directions carved out of the circle feels like playing the piano in oven gloves. This chapter starts with the raw angle, builds up to speed, and ends with a scheme you can drop into most games unchanged. If you only want that scheme, skip to §GS-8.5.

**`GS` tutorial, with its line table:**

> Type the program in, build it, and copy it to the player. When it starts, run your thumb round the wheel: the dot follows it, a little behind.
>
> | Line | What it does |
> |---|---|
> | 6 | Reads the wheel as an angle, 0 to 95, where 0 is the top. |
> | 9 | Moves the dot a quarter of the way towards that angle each frame. That is why it lags. Try a half, then a whole, and feel the difference. |

**`UG` task:**

> **Make a playlist on the player**
> 1. Find a song you want, and hold the centre button until a menu appears.
> 2. Choose **Add to playlist**, then **New playlist**.
> 3. Type a name with the wheel, then press the centre button.
>
> The song is now in your new playlist. To add more, repeat from step 1 and choose the playlist by name.
>
> **Note.** Playlists you make on the player stay on the player. They are not copied back to your computer.

---

## 10. Naming and assets

- **"iPod Video", "iPod 5G" and "iPod 5.5G"** name the hardware and nothing else. They are never styled as Apple branding, and never used to suggest that ProkBox is an Apple product.
- **No Apple logos, icons, product photographs, fonts or interface artwork.** Hardware illustrations are our own line drawings; photographs are our own, of our own devices, with logos cropped out or simply not made the subject.
- **Chip vendors' names** (PortalPlayer, Broadcom, Wolfson) appear only to identify parts. No vendor logos and no reproduced datasheet figures, tables or schematics.
- **Screenshots** in `GS` and `UG` show ProkBox screens, rendered with open or synthetic media.
- **Nothing from the owner's library or logs** appears anywhere: no track, album or artist names, no paths, no host names.
- **Trademark marks** appear once, in front matter, not throughout.

---

## 11. Sources studied for this draft

Study notes, in our own words, are in `refs-notes/`. The documents themselves were fetched for local study only, into `refs/`, which is git-ignored and never committed or redistributed.

| Note | Document studied | Taken from it, mainly |
|---|---|---|
| `amiga-hrm.md` | *Amiga Hardware Reference Manual*, 3rd ed. (1991) | Theory before registers; the tempting mistake; two register appendices with one legend; key points; numbered glossary senses |
| `apple-iigs-hwref.md` | *Apple IIGS Hardware Reference*, 2nd ed. (1989) | "Bits in…" tables; address in every heading; the three-level model marking; one rule for altering registers with stated exceptions; correcting a misleading name; and, as a warning, theory placed after registers |
| `arm7tdmi-trm.md` | *ARM7TDMI Technical Reference Manual*, r4p1 (2004); also the Rev 3 manual from ARM's own service | Conceptual then parametric timing; the cycle alphabet; worked latency sums; "if you are not using…"; *unpredictable* and *undefined* defined; warnings rationed |
| `bebook.md` | *The Be Book* (Haiku's 2008 edition) | Overview and reference halves; the header block; callbacks you implement; grouped entries; complete result tables; rationed humour |
| `c64-prg.md` | *Commodore 64 Programmer's Reference Guide* (1982) | Line-by-line tables; example chains; the KERNAL entry fields; `x` don't-care patterns; and, as a warning, the sales voice |
| `gbatek.md` | *GBATEK* (no$gba 3.06 edition) | Unpredictable behaviour as a chapter; field-level model marks; delta sections; unknown registers kept in the map; unique-title anchors in one huge page |
| `inside-macintosh.md` | *Inside Macintosh* (1985 Promotional Edition; *Devices*, 1994; *Toolbox Essentials*, 1992) | "Using the…"; the combined index with two-letter locators; run-in second-audience notes; direction arrows; revision notes that admit corrections; and, as a warning, 1990s repetition |
| `next-devdocs.md` | NeXTSTEP Release 3 *Development Tools and Techniques*; *General Reference* vols 1–2 (1993) | Callout levels defined by what they oblige; the template documented first; tools described with their failure modes; graduated tutorials; context-prefixed glossary |
| `palmos-companion.md` | *Palm OS Programmer's Companion*, vols I–II (2003) | "Why this platform is different"; real shipped code as listings; the worked trap; reset combinations as reference; and, as warnings, undefined IMPORTANT and brand saturation |
| `pandocs.md` | *Pan Docs* (gbdev, 2026 head) | The register heading formula; "To be verified" boxes tied to issues; test programs as evidence; title-resolved links; and the single page's strengths and fragile anchors |

Sources that were not reachable in the edition first asked for are recorded in each note. In particular, the published 1985 *Inside Macintosh* Volume I and the first edition of the IIgs reference need an archive.org login, so the nearest open editions were used.
