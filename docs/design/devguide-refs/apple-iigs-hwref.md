# Apple IIGS Hardware Reference (second edition, "for Apple IIGS and 1 MB Apple IIGS", first printing September 1989, Apple Computer, Inc. / Addison-Wesley, ISBN 0-201-52389-2; source: https://archive.org/details/Apple_IIgs_Hardware_Reference_HiRes, read from `Apple_IIgs_Hardware_Reference_HiRes_djvu.txt`)

Study notes for the ProkBox developer-guide style study (#161). Structure and tone only; nothing
here is quoted beyond a few words.

The same text is also on archive.org as `Apple_IIgs_Hardware_Reference` and as a mislabelled
"1988" upload. Their full texts are the same second edition, so I kept only one copy. A
lending-library scan (`appleiigshardwar00appl`) needs a login and was skipped. The first edition
(1987) was not available as open full text, so these notes describe the 1989 book only.

Read in full: front matter and preface, chapter 1, chapter 2 (The Core of the Apple IIGS),
chapter 5 (Apple IIGS Sound). Sampled: chapter 8 (interrupts, timing diagrams, I/O switch tables),
chapter 10 (65C816) and its data-sheet reprint, appendices A–E, the glossary and the index. The OCR
flattens tables and bit figures into one cell per line, so table and figure layout below is
reconstructed from captions, column headings and the prose.

## Structure

Front matter: title, copyright and warranty, Contents, then **Figures and tables** (a full list
grouped by chapter), then the **Preface**: who the book is for; About this manual; What this
manual contains (one sentence per chapter and appendix); Conventions and usage (what "standard
Apple II" means, and the rule that facts apply to both logic boards unless marked); Special
messages; Memory sizes (K and MB are binary, with a worked example); **Altering register contents**
(a book-wide rule for touching registers).

1. **Introduction to the Apple IIGS.** Compatibility list, new features, what the 1 MB board adds,
   *removing the cover*, expansion slots, each rear connector, then "A closer look" at the two
   subsystems
2. **The Core of the Apple IIGS.** The two custom chips (Mega II and FPI), synchronisation between
   slow and fast sides, the Mega II cycle, memory allocation, shadowing, the Shadow and Speed
   registers, RAM control, I/O address space
3. **Memory.** Memory map, bank allocation, address wrapping, ROM, bank $00 and the language card,
   the State register, auxiliary memory, banks $E0/$E1, Apple II program memory use, memory
   expansion slot and signals
4. **The Video Displays.** The Video Graphics Controller and its interrupts, legacy text and
   graphics modes, display soft switches, colour registers, Super Hi-Res (New-Video register,
   scan-line control bytes, palettes, pixels, dithering, fill mode)
5. **Apple IIGS Sound.** Speaker, one-bit sound, Sound GLU, Ensoniq DOC registers, making sound,
   I/O specifications, further reading
6. **The Apple Desktop Bus.** Keyboard, ADB microcontroller and GLU registers, bus protocol
   (commands, signals, errors), device registers and addressing, 1 MB-board features
7. **Built-in I/O Ports and Clock.** Disk port and IWM registers, serial ports (SCC), game I/O,
   secondary I/O summary, real-time clock
8. **I/O Expansion Slots.** Slot signals, DMA, loading and driving rules, peripheral-card address
   spaces, programming suggestions, interrupts, **timing diagrams**
9. **Power Supply.** Description, specifications, connector
10. **The 65C816 Microprocessor.** Features, registers, emulation, speed, further reading, then the
    *manufacturer's data sheet reprinted whole*

Appendices: **A** Roadmap to the IIGS technical manual suite; **B** International keyboards;
**C** The character generator; **D** Conversion tables (bits and bytes, hex/decimal/negative
decimal, peripheral IDs, ASCII); **E** Frequently used tables. Then the **Glossary**, the **Index**,
and an **Addendum** of main-board schematics for both board revisions.

The order is a tour from the chips that define the machine, through memory, to each subsystem,
then the expansion bus, power, and the CPU last.

## How one block or topic is laid out

**Walk-through: Chapter 5, Apple IIGS Sound (pp. 101–120).**

1. *Chapter opener.* One paragraph, in a slightly promotional tone, naming the chips the chapter
   covers. Then Figure 5-1, the whole-machine block diagram reused with the caption "Sound
   components". Each subsystem chapter opens with this same diagram (Video, ADB, Slots), so the
   reader always sees the block in context. Chapter 2 says its copy shades the parts under
   discussion. The OCR can't show whether the later copies do too.
2. *The built-in speaker.* Two sentences on where it physically is and what drives it.
3. *One-bit sound.* The legacy method first: toggling a soft switch, how access rate maps to pitch,
   the lowest usable frequency. A Note gives a classic trap: an indexed-mode access touches the
   switch twice, so the speaker makes no sound.
4. *Sound synthesis.* The principle (wavetables in RAM, read by oscillators), plus a firm nudge to
   use the toolbox for future compatibility.
5. *The Sound GLU.* What the interface chip is for; a bulleted list of its registers; **Table 5-1**
   (register / address / R/W). Then one subsection per register:
   - *Sound Control register*: purpose in prose; a **Warning** *not* to use read-modify-write on
     this one (which contradicts the preface rule); **Figure 5-2** bit diagram; **Table 5-2** bits.
   - *Data register* and *Address Pointer registers*, with a small figure numbering all 16
     pointer bits across the two bytes.
6. *Write operation.* A numbered procedure, with sub-bullets, then the auto-increment behaviour.
   A Note bans indexed addressing because the false read desynchronises the chip.
7. *Read operation.* "Same as write except …": reads lag by one cycle, so to read N bytes you
   read N+1 and discard the first. This is precise, operational gotcha writing.
8. *The Ensoniq DOC.* Part number, oscillator count, RAM size, outputs; a bulleted list of the three
   global registers and the seven per-oscillator registers.
9. *Global registers*, each with its address in the heading ("The Oscillator Interrupt register
   ($E0)"), prose, a bit figure and a bits table. The Oscillator Enable register is explained by
   an arithmetic rule (count × 2), the reset default and the minimum. The A/D register gives
   the conversion time and what happens if you read too early.
10. *Per-oscillator registers.* **Table 5-4** is an address grid with one row per oscillator (32)
    and one column per register type. A footnote marks the last oscillators as reserved for
    system use, with a crash warning. The oscillator modes are a bulleted list, then the control
    register's figure and table. The frequency registers carry the output-frequency formula with
    every symbol defined. The wavetable size register has a bits table plus **Table 5-7** mapping
    the 3-bit code to size.
11. *Making sound with the DOC.* Theory of operation comes *after* the registers. It opens by
    correcting the reader: these oscillators don't oscillate, they generate addresses. Then
    Figure 5-7 (how table size and resolution choose which accumulator and pointer bits form the
    address), Figure 5-8 (adder → accumulator → multiplexer data flow), Figure 5-9 (time-multiplexed
    output of four oscillators, with a 1.2 µs slot marked), and the cost of servicing all 32.
12. *Sound input and output specifications.* **Table 5-8** for the 7-pin connector:
    signal / pin / maximum / units, with a footnote on strobe polarity. Then Figure 5-10, a
    two-channel demultiplexer *schematic* the reader could build.
13. *Further reading.* One book, full citation.

Chapter 2 follows the same pattern. Opening question (how can it run old software faster?) →
block diagram → one section per chip → synchronisation as numbered steps with nanosecond figures
→ memory map figure → concept (shadowing, why it exists, its cost) → Shadow register (warning,
figure, table) → Speed register (reset and power-on state stated first, warning, figure, table).

## Conventions

- **Register figure.** The caption carries the name *and address* ("Shadow register at $C035").
  Bit numbers 7–0 run across the top, and each bit or field has a leader line to its name.
  Reserved bits are labelled "Reserved; do not modify" (or "must be 0" where that is the rule);
  one figure uses a single letter "r" with a key in the caption.
- **Bits table.** Caption "Bits in the X register". Columns Bit / Value / Description, with
  **one row per value**: a bit gets a row for 1 and a row for 0, each a complete sentence ("When this
  bit is 1, …"). Multi-bit fields get an inner truth table (bit 2, bit 1 → mode name). A
  bit available only on the newer board says so in parentheses at the start of its description.
  Tables that cross a page repeat the caption with "(Continued)".
- **Address tables.** Register / address / access type. Soft-switch tables give Name / Function /
  Location in **hex and decimal** columns (for BASIC users), plus Notes on whether read or write
  triggers it.
- **Address grids** for replicated registers: oscillator number down, register type across, every
  cell an address.
- **Hex notation.** `$` prefix, declared once in the preface. Memory sizes are binary, with a worked
  example to make the point.
- **Timing diagrams** (chapter 8). The waveform figure has signals down the side (the clock phases,
  7M, Q3, interrupt and ready lines) and numbered intervals marked on it. A companion table lists
  Number / Description / Minimum / Maximum, with a footnote giving the unit (nanoseconds) once. In
  prose (chapter 2), cycle behaviour is a numbered step list with approximate nanosecond delays,
  and a small figure shows the stretched clock cycle.
- **Special messages.** Three tiers, each marked with a symbol: ♦ *Note* (interesting, not
  essential), ▲ *Important* (especially important), ▲ *Warning* (ignoring it risks data loss,
  damage to equipment, or bodily injury). Important and Warning end with a closing ▲.
  Warnings are short and point back to the preface rule where it applies.
- **Book-wide register rule.** The preface section on altering registers says that reserved bits
  must be preserved, names the two CPU instructions that make that easy, and points to an external
  book for their use. Individual registers then repeat it, or (for one sound register) override it.
- **Model variants.** The preface says everything applies to both boards unless marked. Then there
  are whole sections headed "1 MB Apple IIGS …", parenthetical "(available only on the 1 MB logic
  board)" inside bit tables, and paired figures showing both boards.
- **Code.** Almost none. Procedures are numbered steps naming addresses. The book defers to the
  firmware and toolbox manuals for software and to an outside book for assembly.
- **Cross-references.** By chapter number and quoted title, by "later in this chapter", and by full
  title to sibling manuals. Appendix A is a whole roadmap of the manual suite, saying which book to
  read for what.
- **Appendices.** Reference material only. Appendix E reprints the most-used tables from the body,
  each with a footnote giving its original table number. Appendix D is a generic conversion
  toolkit. Third-party CPU data sheets and full schematics are reproduced.
- **Glossary.** About sixteen pages. Entries are a bold term, a colon and a definition. Senses are
  numbered (1), (2) where a word means two things. "Compare" and "See" link entries. Many entries
  are general computing terms (accumulator, ASCII), and some point to the chapter with the detail.
  Terms are bold at first use in the body.
- **Index.** Two levels (ADB → addresses, collision detection, Status register …), page ranges,
  and "See" redirections for acronyms.
- **Typography quirks.** Register names are capitalised as proper nouns ("the Speed register"),
  and soft switches are in capitals (SETINTCXROM). Every subsystem chapter opens with the
  whole-machine diagram. Captions number figures and tables per chapter (Figure 5-2, Table 5-2),
  and the "Figures and tables" list at the front makes them findable.

## Voice

- **Person and tense.** Second person, present tense. Descriptive for hardware behaviour,
  imperative in procedures. Hardware is the subject of most sentences ("the FPI holds the clock
  high until …").
- **Sentence length.** Medium. Bit descriptions are short, complete sentences, and theory sections
  run longer. Lists are used heavily for features, registers and modes.
- **Tone and humour.** Earnest and confident, with a little brochure gloss in chapter openers
  (best-yet, outstanding, your imagination is the limit) and the odd practical aside (for building
  wavetables, trial and error works best). Chapter 2 opens with a rhetorical question it then
  answers. No jokes as such.
- **Uncertainty.** Hardly any. Everything is stated as fact, which is fair for the manufacturer.
  The hedges are about the future: use the toolbox so that later models still work, and don't rely
  on unpublished entry points.
- **Danger.** Concrete and consequence-led: a system crash if you touch reserved oscillators or
  shadow the wrong banks, lost synchronisation if you use indexed addressing on the sound data
  register, lost conversions if you read the A/D too early. The warnings define the hazard and
  say exactly which instruction or access pattern to use instead.
- **Teaching stance.** It explains *why* a design exists (shadowing exists to keep video writes
  from slowing everything down), not just what the bits do. It corrects misleading names
  (oscillators that are really address generators).

## Borrow

1. **Register figure plus a "Bits in …" table with one row per value**, each a full sentence.
   It reads slowly but leaves nothing ambiguous, especially for active-low and inhibit bits.
   *PP5022, WM8758, BCM2722.*
2. **Name and address together in every register heading and caption** ("… at 0x6000_xxxx").
   *PP5022, WM8758* (register index rather than address).
3. **Address grids for replicated registers** (channel × register type). *PP5022* (DMA channels,
   timers, GPIO ports); *BCM2722* if it has per-layer register banks.
4. **A preface-level "Altering registers" policy, then per-register exceptions called out as
   Warnings.** For us: what to do with reserved bits, and shadow copies for the WM8758's write-only
   registers. *PP5022; WM8758; firmware guide.*
5. **Operational gotchas stated as recipes**: read N+1 and discard the first; don't use the access
   pattern that causes a false read. *PP5022* (I²C, IDE, the BCM2722 host interface); *BCM2722*.
6. **Timing diagram with numbered intervals plus a min/max parameter table**, and the unit given
   once in a footnote. *PP5022* (I²S, IDE, LCD bus); *BCM2722* (host-interface cycles);
   *WM8758* (control and audio interfaces). Our numbers will carry evidence tags in an extra column.
7. **Variant marking at three levels**: a preface rule (applies to both unless marked), section
   headings for variant-only material, and parenthetical marks inside bit tables. This maps directly
   onto 5G versus 5.5G. *PP5022; 5.5G hardware teardown; user guide; firmware guide.*
8. **Every subsystem chapter opens with the same whole-system block diagram, with the subject
   highlighted.** *All three hardware references; 5.5G teardown.*
9. **"Removing the cover" and a connectors figure in chapter 1.** *5.5G teardown* (opening procedure
   with clip locations); *user guide* (dock connector and ports).
10. **A theory section that corrects a misleading name**, as with "oscillators" that are really
    address generators. *WM8758* (any confusingly named mixer or boost stages); *PP5022*.
11. **Start with the simplest legacy method, then the full mechanism**, as with one-bit sound before
    the synthesiser. *Games SDK guide* (simple beeps or blitting before the full API); *firmware guide*.
12. **Electrical spec tables** (signal / pin / max / units) for connectors. *5.5G teardown; PP5022*
    electrical appendix; *WM8758* (measured output characteristics).
13. **A "frequently used tables" appendix whose entries point back to the original table numbers.**
    *PP5022, WM8758* (quick-reference appendix); *games SDK guide* (a cheat-sheet appendix).
14. **A manual-family roadmap appendix.** *All manuals* (one shared page, prefixes and audiences).
15. **"Further reading" with full citations at chapter end.** *All*, including datasheet and Rockbox
    source pointers, which are also where our evidence comes from.

## Avoid

- **Theory after the registers.** Chapter 5 explains how the DOC actually works only after
  cataloguing every register. Our block layout puts theory first, and this chapter shows why:
  the register tables are hard going without it.
- **Brochure language** in chapter openers.
- **Reprinting third-party data sheets and schematics.** That is copyright and redistribution
  trouble for us. Link to them and summarise.
- **Hex and decimal side by side** for every address. Use hex only, and give decimal where a count
  is meant.
- **A padded glossary** of general computing terms (ASCII, accumulator). Keep ours to project and
  hardware vocabulary.
- **Deferring to a higher-level API "for compatibility" without documenting it.** We document both
  the API and what it does to the hardware.
- **Almost no code.** A hardware reference for programmers needs short worked examples. This one
  leaves them to other books.
- **Page-number cross-references and a front "Figures and tables" list.** In Markdown, anchors and
  captions do this job, though a generated list of figures could still be useful in the PDF build.

## Distinctive details

1. **A book-wide rule in the preface that one register then overrides.** Read-modify-write is
   mandatory everywhere except the sound control register, where it is forbidden. Putting the
   contradiction in writing is exactly the kind of trap our manuals exist to record.
2. **Bit tables that spend a whole row on each value**, so that "inhibit when set" bits can't be
   misread.
3. **The footnote that fences off system-reserved oscillators** in the middle of an address grid,
   with a crash warning attached.
4. **The read-lag recipe** (read one extra byte and discard the first). It is short, exact and
   immediately usable.
5. **"These oscillators don't oscillate"**: the theory section opens by correcting the chip's own
   vocabulary.
6. **Physical and electrical grounding.** Cover-removal steps in chapter 1, a buildable stereo
   demultiplexer schematic, connector pin tables, and full board schematics at the back. The book
   assumes its reader may pick up a soldering iron.
