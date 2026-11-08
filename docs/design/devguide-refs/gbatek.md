# GBATEK (HTML edition "extracted from no$gba version 3.06", file dated 16 April 2025; written by Martin Korth, self-published as the help text of the no$gba emulator and debugger; https://problemkaputt.de/gbatek.htm)

Studied from one fetch of the single HTML page (refs/gbatek/gbatek.htm, 5,032,449 bytes, 102,293 lines once converted to text). The author's own credits date the GBA and DS work from 2001; the document has since grown DSi and 3DS parts. It is freely downloadable but carries no open licence, so it is studied here for form only. A plain-text twin (gbatek.txt, 4.15 MB) is published alongside and the author treats the 80-column text as the master.

## Structure: the real chapter and appendix order, condensed

The page opens with a centred title, a one-line subtitle naming the emulator version, and a master contents laid out as a grid of grey-headed columns. Each column is one "reference" (a part); inside, short bold group labels (Overview, Hardware Programming, Other) introduce lists of chapter links. In page order the parts are:

```
GBA Reference
  Overview          Technical Data · Memory Map · I/O Map
  Hardware          LCD Video Controller · Sound Controller · Timers · DMA
                    Transfers · Communication Ports · Keypad Input · Interrupt
                    Control · System Control · Cartridges (≈30 sub-chapters:
                    header, backup memory types, RTC, sensors, e-Reader,
                    cheat devices, Game Boy Player) · Backwards Compatibility
                    CGB Mode · Unpredictable Things
NDS Reference
  Overview          Technical Data · I/O Maps · Memory Maps
  Hardware          Memory Control · Video · 3D Video · Sound (incl. file
                    formats) · System and Built-in Peripherals · Cartridges,
                    Encryption, Firmware · Xboo · Wireless Communications
DSi Reference       Differences to NDS · I/O map · SCFG · Teak DSP (with its
                    own instruction set) · NDMA · AES · touchscreen/sound codec
                    · I2C · cameras · SD/MMC protocol and filesystem · Atheros
                    wifi (incl. Xtensa CPU) · GPIO · console IDs · exploits
3DS Reference       Marked "under construction"
CPU Reference       ARM7TDMI/ARM9/ARM11 overview, register set, exceptions,
                    ARM and THUMB opcodes, CP14/CP15, VFP, cycle times
BIOS Functions      Summary, then arithmetic, rotation/scaling, decompression,
                    copy, halt, reset, multiboot, sound, RSA, RAM usage, dumping
External Connectors AUX pinouts, chipset pinouts, Xboo cable
Emulator manual     no$gba installation, debugging, hotkeys, profiler, the XED
                    editor and the built-in assembler
Back                About this Document · Index
```

About 770 anchored sections in all. Generic material that the GBA and DS share (the ARM CPU, BIOS calls, connectors) is pulled out into common parts rather than repeated. Algorithms that turn up in many file formats (LZ, Inflate, PNG) have their own sections near the DSi filesystem material. The "Index" at the very end is not an alphabetical index: it is the complete flat list of every section title, in page order, each a link.

**How the DS is covered.** It is a separate part that leans on the GBA one. Where hardware is unchanged the DS section is a sentence or two saying "same as GBA, except" a clock or a count, with a link back (the DS Timers section is four lines). Where the GBA section's registers gained bits on the DS, the GBA bit table itself carries the alternative in place, as "Not used (0) / NDS: ..." or "DSi: ...". Model differences thus live at the field level, in the earliest part. The DSi part likewise opens with a "differences to NDS" section.

## How one block or topic is laid out: walk through one specific chapter section by section

**GBA Timers** (about 45 lines, the whole chapter):

1. *Banner.* A full-width grey bar holding the chapter title in a larger font; the anchor sits inside it.
2. *Two-line overview.* How many timers, their width, and the one non-obvious use (timers 0 and 1 clock the sound DMA). Nothing else.
3. *Grouped register headings.* Four bold lines, one per timer, each giving address with a trailing `h`, mnemonic, plain name and access in parentheses. One description then covers all four.
4. *Behaviour prose.* What a write does versus what a read returns (reload value versus live counter), and the two events that copy reload into counter.
5. *Inline "Note:".* A corner case: a single 32-bit write that both starts the timer and sets the reload uses the new reload.
6. *Second register group* (the four control registers), then a bit table in a monospace block: columns "Bit" and "Expl.", one line per field, the enumerated values in parentheses on the same line, a trailing semicolon comment where a field does not apply to timer 0, and unused bits listed as "Not used".
7. *Field explanation and constants.* A short paragraph on count-up (cascade) mode and a line defining F as the system clock.

**GBA DMA Transfers** follows the same plan at larger scale: bold "Overview" subheading with channel priorities; a short list of what is special about each channel; register groups (source, destination, count, control) with their address ranges and size limits in the heading; the control bit table; then a run of bold-titled paragraphs on each subtlety (when internal pointers reload, the repeat bit, sound FIFO mode, video capture mode, Game Pak DRQ, start-up delay).

**GBA Interrupt Control** shows two more features. It quotes two passages from the official Nintendo programming manual in double quotes, each followed by a lone question mark, flagging that the author has passed on but not verified them. It then lists the BIOS interrupt dispatcher as disassembly with addresses and end-of-line comments, followed by a dash-bulleted recipe for a user handler and a memory-usage table for the fixed RAM area the BIOS uses. An open question is left in the text as a plain "Not sure" line.

There are no explicit "theory", "programming model" or "examples" headings. The structure is: banner, a few lines of orientation, registers, then bold-titled notes in whatever order the subject needs.

## Conventions: register tables, bit diagrams, timing diagrams, notes/warnings/cautions, code examples, cross-references, appendices, glossary, index, typography quirks. Say exactly how each is done.

**Register map (I/O Map).** Plain monospace lines in columns: address, size in bytes, access (`R`, `W`, `R/W`, or `?`), mnemonic (or `-` for undocumented), description. Grouped by block under plain subheadings. Unknown registers stay in the map with question marks in the columns and a descriptive name such as "Undocumented - Purpose Unknown".

**Register heading line.** Bold: `4000004h - DISPSTAT - General LCD Status (Read/Write)`. Address in upper-case hex with a trailing `h` and no prefix, then dash-separated mnemonic and name, then access and sometimes size or range in parentheses. Registers that are the same for several channels are given as consecutive heading lines sharing one description.

**Bit tables.** Not drawn diagrams but monospace two-column lists: bit or bit range (`0-1`, `8-15`), then the explanation with values enumerated inline as `(0=..., 1=...)`. Some tables add an access column per field, either before the explanation or right-aligned at the end. Reserved bits appear as "Not used", sometimes with the value they read back. Model variants share a line, separated by a slash. 316 lines of "Not used" in the document: every bit is accounted for.

**Timing.** No drawn timing diagrams. Timing is given as aligned numeric tables in prose sections (the LCD dimensions section lists dots, microseconds, cycles and percentage of line for visible and blanking periods), and as clock-cycle counts in prose or comments. Waveforms are occasionally sketched in ASCII on one line (the square-wave duty cycles drawn with dashes and underscores).

**Notes and warnings.** No boxes and no colour. An inline "Note:" begins a paragraph; occasionally a bold subheading such as "Forward" or a capitalised word does the work of a warning ("does NOT change", "MUST"). Danger is conveyed by plain statements of consequence.

**Uncertainty.** The most distinctive convention: a question mark appended to a statement, sometimes `?!` for surprise, `(?)` after a single value, or a bare "Not sure" or "Unknown if" sentence. Over 1,700 lines match a trailing or parenthesised question mark. Second-hand claims are marked "reportedly". Official documentation is quoted in double quotes and also given a question mark when unverified. Whole sections exist for unknown registers ("DSi Unknown Registers", "DS Cartridge Unknown Commands").

**Code.** Assembly in the author's own syntax with addresses, opcodes and `;` comments; pseudocode written as terse assignments (`flagbits=[src], src=src+1`). BIOS calls follow a fixed entry: heading with SWI number per platform and name, one-line purpose, input registers as an aligned list, "Return:" and output registers, then notes and a worked numerical example.

**Cross-references.** A link is a section title on its own line, bare, placed where the reader should jump; or "see ..." in prose. Anchors are the section title lower-cased with everything except letters and digits removed (`gbatimers`). About 1,900 internal links, none broken, no duplicate anchors.

**Appendix-like matter.** The CPU, BIOS, connectors and emulator manual act as appendices but sit as peer parts. The flat "Index" acts as an exhaustive table of contents.

**Glossary.** None. Terms are explained where first used, often by analogy with the older Game Boy ("much the same as LY of older gameboys").

**Typography.** Inline CSS only: 12 px sans-serif body, 11 px monospace for blocks, page width capped at 50em, chapter banners as grey table cells. Over 6,000 `<pre>` blocks, many wrapped in a borderless table. Plain `<BR>` line breaks preserve the text file's line structure.

## Voice: person, tense, sentence length, humour, how it handles uncertainty and danger. Describe this in your own words; quote at most a few words where essential, never whole sentences.

Impersonal and telegraphic. The hardware is the subject; the reader is addressed only through imperatives in recipes ("must", "set", "wait"), and the author surfaces in first person when admitting ignorance or describing his own test unit. Present tense. Sentences are short, often verbless, and lean on abbreviations (ie., eg., Expl., approx.). The English is slightly non-native in places (some "is having", "much the same than"), which is part of its texture.

Humour is very dry and rare: a "?!" after an illogical limit, a capitalised refusal of anything but 80-column text in the About section.

Uncertainty is never hidden and never dressed up. A question mark goes on the very claim that is doubtful, so a reader can see confidence at the grain of a single line; the cost is that the meaning of a question mark has to be learnt and is not explained anywhere. Hardware-variant differences are stated with the exact models that differ. Danger and undefined behaviour are treated as more facts to record (the whole "Unpredictable Things" chapter documents what open-bus reads return per memory region and model, and opens with a caveat that none of it is promised to be stable).

## Borrow: concrete practices for our manuals, each saying which of our manuals it suits (PP5022, BCM2722, WM8758 hardware references; firmware guide; games SDK guide; user guide; 5.5G hardware teardown)

1. **"Unpredictable things" chapter.** A chapter documenting undefined behaviour as precisely as defined behaviour: what reads from unmapped space return, what narrow writes to video memory do, mirrors. PP5022 (as a chapter or appendix), BCM2722.
2. **Model variants at field level.** Put "5.5G: ..." directly on the bit-table line that differs rather than in a separate chapter. PP5022, 5.5G teardown, WM8758 if wiring differs.
3. **Delta chapters.** Where a 5.5G block is the same as 5G, a two-line section saying "as 5G, except" with a link. 5.5G teardown, and our PP5022 Appendix F on PP5020/PP5024.
4. **Grouped register headings** for identical per-channel registers (four timers, DMA channels), described once. PP5022.
5. **Values inline in the field description** (`0=Off, 1=On`) for short enumerations, saving a nested table. PP5022, WM8758.
6. **Unknown registers stay in the map** with their address and an "unknown" description, never omitted. All hardware references; tag `[hypothesis]` and link §N.9.
7. **Fixed format for BIOS/API calls**: number and name, purpose, inputs, returns, notes, numeric example. Games SDK guide Appendix A and firmware guide syscall material; matches our §5.5.
8. **Separate common parts** (CPU, shared services, connectors) referenced from each device part instead of duplicated. If our manuals merge, the ARM7TDMI and I²C/I²S material belongs once.
9. **Numeric timing tables** listing the same interval in cycles, microseconds and percentage. PP5022 timing, BCM2722 display refresh, games SDK performance budgets.
10. **Quoted vendor claims flagged as unverified.** Directly relevant to our open question on datasheet facts: quote nothing, but mark vendor-sourced statements as distinct from measured ones. WM8758 especially.
11. **Disassembled vendor code as explanation** of a fixed firmware routine (the BIOS IRQ dispatcher), with per-line comments. Our equivalent is `[apple-RE]` walk-throughs, but paraphrased, never quoted. Firmware guide boot chain, BCM2722 boot.

## Avoid: what dated badly or doesn't suit us

- **The bare question mark as the only confidence signal.** Undefined, unsearchable and easy to miss; our tags do this job explicitly.
- **No overview or theory before registers.** Works for an expert with an emulator open; poor for our guides and for first readers of the references.
- **Abbreviation-heavy, verbless English** (Expl., ie., "etc." filling a whole column of a bit table). Fine for a debugger help pane, hard for newcomers.
- **No section numbers, no figures, no examples of use** beyond BIOS code.
- **Hex with a trailing `h`.** Our readers write C.
- **Emulator manual bound into the hardware reference.** Our firmware tooling should live in the firmware guide, not in PP5022.
- **One 5 MB HTML file with layout tables and font tags**, no stylesheet to speak of, and a tracking pixel. Not a model for our renderer.
- **No open licence.** We must not borrow text, tables or diagrams from it, only habits.

## Distinctive details: 3–6 specific things that give it its character

1. The question mark as an epistemic marker, applied to single lines, values and even quotations from official manuals.
2. The "Unpredictable Things" chapter, cataloguing open-bus and mirror behaviour by model and memory region.
3. Earlier hardware chapters annotated in place with later-model bits ("Not used / NDS: ..."), so one table serves three generations.
4. Its origin as an emulator's help file: the debugger and assembler manual sits at the end, and the subtitle names the emulator version.
5. A deliberately bare look (grey banners, monospace tables, 12 px text) and the author's insistence that the 80-column text file is the true edition.
6. Enormous breadth: down to the wifi chip's internal CPU instruction set and DSi exploits, all in one page.

## As a single huge document

**Division.** Six reference parts plus the emulator manual and back matter, each shown as a column in the master contents. Within a part, a chapter "landing" section (for example "GBA LCD Video Controller") holds nothing but grouped links to its sub-sections under small headings (Registers, VRAM, Sprites, Other). So navigation is three levels: master grid, landing section, leaf section. Every leaf is a flat sibling in the HTML; there is no nesting markup, only the order.

**Anchors.** Each section has an `<A NAME>` made by lower-casing its title and dropping every non-alphanumeric character (`lcdiointerruptsandstatus`). I found 770 anchors and 1,907 internal links, with no duplicates and no broken links. Because anchors depend only on the title, and titles carry a platform prefix ("GBA", "DS", "DSi", "3DS"), they are unique across the volume and would survive splitting into one page per section, provided the splitter rewrote `#name` to `page.htm#name`. The plain-text edition shows the same structure can also be generated without HTML.

**Navigation aids.** Master contents at the top only; it is not sticky. No search beyond the browser's find. No breadcrumbs, no "where am I" cue other than the grey banner of the current section, no back-to-top or next/previous links (only one link anywhere returns to the contents). No print view, but the page has almost no styling, so it prints as it is. The reader navigates by link, browser back button and find.

**Weight and load (measured 6 November 2026 with curl).** 5,032,449 bytes uncompressed, fetched in 9.7 s; with compression 1,338,025 bytes in 2.7 s. Two requests in total: the HTML and a 1×1 tracking image. No scripts and no external CSS. The browser must lay out about 100,000 lines and 6,000 `<pre>` blocks before find works well across the page.

**What works.** One file to save, keep offline and search with Ctrl-F; links are instant because everything is already loaded; anchors are robust and descriptive; the delta style ("same as GBA, except") keeps cross-model material short. For the maintainer it is generated from one help-text source by the author's own tool, so the HTML, text and in-emulator help never diverge.

**What works badly.** Slow first load and heavy on phones; once you follow a link deep into the page you have no idea which part you are in; no search with ranking; a single author is the only maintainer, so there is no review or edit history; any change re-ships 5 MB.

**Glossary and index across the volume.** No glossary. The end "Index" is a flat, linked list of all section titles in page order, so it duplicates the contents rather than indexing terms. Register mnemonics are found by browser find or via the I/O map sections, which act as the index of registers.
