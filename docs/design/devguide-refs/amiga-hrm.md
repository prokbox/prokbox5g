# Amiga Hardware Reference Manual (3rd edition, 1991, Commodore-Amiga Inc. / Addison-Wesley, Amiga Technical Reference Series; source: https://archive.org/details/amiga-hardware-reference-manual-3rd-edition, read from `Amiga_Hardware_Reference_Manual_3rd_edition_djvu.txt`)

Study notes for the ProkBox developer-guide style study (#161). Structure and tone only; nothing
here is quoted beyond a few words. The OCR text is noisy (bit-grid tables and figures are mangled),
so remarks on table and figure layout are inferred from captions, legends and surrounding prose.

Read in full: front matter and preface, chapter 1, chapter 2 (Copper), chapter 5 (Audio).
Sampled: chapters 4 and 6 (sprite register summary, Blitter key points), appendices A, B, C, D,
the glossary and the index.

## Structure

The book is ordered by hardware block, with a short orientation chapter in front and a long tail of
reference appendices. Page numbers run through the whole book (not per chapter).

Front matter
- Title pages, contributor list (about twenty names, including the chip designers), the
  third-edition authors, copyright, and a blunt front-matter warning that the contents may contain
  errors and that full compatibility across revisions is a goal, not a promise.
- Contents, then separate lists of figures and tables (about 80 numbered figures).
- Preface: who the book is for (three audiences: assembly programmers bypassing the OS, hardware
  designers adding peripherals, the merely curious), a one-paragraph précis of every chapter, and
  reading routes by experience: newcomers read chapter 1; those who know the machine browse 2–8;
  experts live in the appendices. Ends by naming the sibling manuals in the series.

Chapters
1. Introduction: components of the machine; the custom chips and memory system (Chip, Fast and
   "slow" memory, with a model-by-model table); peripherals; expansion; video interface;
   About the Examples (which OS module owns each block, the include-file conventions, which
   assemblers the listings were tested on); General Development Guidelines; Hardware Programming
   Guidelines; Additional Assembler Guidelines; developer support; how to report errors in the book.
2. Coprocessor Hardware (the Copper).
3. Playfield Hardware (basic and dual playfields, display windows, scrolling, advanced topics,
   register summaries, colour-register summary).
4. Sprite Hardware (forming, moving, reusing DMA channels, overlapped and attached sprites, manual
   mode, hardware details, register summaries).
5. Audio Hardware.
6. Blitter Hardware (memory layout, DMA channels, function generators, shifts and masks,
   descending mode, area fill, line mode, pipeline, speed, interaction with system DMA, block
   diagram, "Key Points").
7. System Control Hardware (video priorities, collision detection, beam position, interrupts,
   DMA control, reset and early start-up).
8. Interface Hardware (controller ports, floppy controller, keyboard, serial, parallel, display
   output).

Appendices
- A Register summary, alphabetical order (declared the definitive reference).
- B Register summary, address order.
- C Enhanced Chip Set (what changed in the newer chip revision, new and extended registers).
- D System memory maps (opens by saying a true memory map cannot be given, then gives the
  general layout anyway with a caveat not to rely on it).
- E I/O connectors and interfaces (pinouts).
- F 8520 CIA (a peripheral chip datasheet: register map, functional description, timers,
  time-of-day clock, serial register, interrupt control, port signal assignments).
- G Keyboard interface (protocol, key codes, caps lock, out-of-sync recovery, power-up sequence,
  reset warning, matrix table).
- H External disk connector.
- I Hardware example include file (the header every listing in the book uses).
- J Custom chip pin allocation list.
- K Zorro expansion bus (a full bus specification bolted on: architecture, signals, arbitration,
  electrical and mechanical specs, AUTOCONFIG).
- Glossary (about a dozen pages).
- Index.

## How one block or topic is laid out: chapter 2, Coprocessor Hardware

The Copper chapter is about 18 pages and a clean model of the whole book's per-block pattern.

1. Chapter opener. A lower-case "chapter two" label over the upper-case title, then one paragraph
   saying what the reader will learn and noting that the other block chapters carry the
   block-specific Copper recipes. No objectives list, no diagram.
2. About the Copper. What it is, where it lives, how it gets instructions (DMA), and what it can
   touch. Then its bus behaviour: it is idle on the bus while waiting, uses only odd memory cycles,
   and so contends only with the CPU and the blitter. Theory of operation comes before any
   register is named.
3. What is a Copper Instruction? The three instructions in a bulleted list, the fact that every
   instruction is two 16-bit words, and the memory-cycle cost of each. Forward pointers to where
   the less common instruction is covered.
4. The MOVE instruction. Word-by-word layout given as a small labelled list per instruction word
   ("Bit 0", "Bits 8-1", "Bits 15-9" with meanings). Then the rules for which register addresses
   are reachable, then a four-line assembly example with a comment on every line. Two footnotes
   here define the `$` hex prefix and state that all fragments are assembly.
5. A short aside urging symbolic register names from the include files rather than raw offsets,
   with the same example redone that way, and an admission that the listings exist to explain the
   hardware and are not production code.
6. The WAIT instruction. Same layout treatment, then two examples. The second is the
   end-of-list idiom (wait for a beam position that can never occur), followed by a careful
   paragraph explaining *why* it can never occur, and then a pre-emptive paragraph about the
   tempting variant that does not work. This "here is the trap you are about to walk into" move
   recurs throughout the book.
7. Horizontal and vertical beam position. Ranges, resolution, blanking intervals, NTSC versus PAL
   line counts, alternating long and short lines, and the four-field interlace cadence, with the
   clock frequencies listed. A two-column "instruction / explanation" table shows how to reach
   lines beyond 255.
8. Using the Copper registers. Location registers (with a general explanation of how high/low
   register pairs work on this machine, which applies far beyond the Copper), jump strobes, and
   the control register with its protection bit. Ends with a one-line Warning (the protection bit
   resets to zero) and an indented run-in note on automatic restart at vertical blank.
9. Putting together a Copper list. Design method first (plan per subsystem, then merge in beam
   order), pseudo-code lists in plain English ("WAIT for ..., MOVE ... to ..."), then the
   consequences of the greater-or-equal comparison, including a side effect a reader might not
   expect. Then a complete sample list: a commented header of numbered notes (list must live in
   Chip RAM, addresses are arbitrary, the example assumes you own the hardware), the includes,
   and the list itself in labelled blocks.
10. Starting and stopping. The start-up sequence after reset as code; the absence of a stop
    instruction and the impossible-wait idiom; the alternative of disabling DMA, with a
    cross-reference to chapter 7.
11. Advanced Topics. SKIP; loops and the comparison-enable mask; a limitation (the top bit cannot
    be masked) explained, then demonstrated with a worked two-loop example that works around it;
    interlaced displays with a figure; using the Copper with the blitter; interrupting the CPU,
    with a small numbered table.
12. Summary of Copper Instructions. A bit-position grid (Table 2-2) covering all three
    instructions side by side, followed by a legend of abbreviations (don't-care, IR1/IR2, DA,
    VP/HP, VE/HE, BFD). A final run-in note points to the ECS appendix for later-revision changes.

So the per-block pattern is: purpose and theory, instruction or data format, registers, how to
assemble a working whole, start/stop, advanced topics, and a compact summary for later reference.
Chapter 5 (Audio) follows the same arc but opens with physics (waves, sampling, aliasing), gives
a numbered six-step recipe whose steps then become the section headings, derives a key limit (the
minimum period value) with the arithmetic shown, ends the main path with a complete example, and
closes with reference tables (musical scale, decibel levels) and a state-machine diagram whose
signal names are explained in a glossary-like list (upper case for external signals, lower case for
internal ones).

## Conventions

Register tables
- Appendix A (alphabetical) gives, per register: name, chip-space offset, read/write, which chip
  implements it, a one-line function, and then a `BIT# USE` table listing each bit number, its
  mnemonic and a short meaning. Multi-bit fields are shown as small code/value sub-tables inside
  the row. Groups of identical registers use an `x` placeholder (`AUDxLEN`) rather than repeating.
- Appendix B (address order) prefixes each entry with a legend of one-character symbols: DMA-only,
  usually-DMA, address-pair, not writable by the Copper, writable only with the danger bit set,
  plus W/R/ER/S access codes (S for strobe: the write itself is the action). The legend is printed
  once at the top of the appendix.
- Both appendices open with the rules for using the registers (compute CPU address from offset,
  pointers are longword pairs with bit 0 zero, strobe with a plain write not a clear) and a Warning
  on read-only versus write-only registers.
- Revision changes are marked with "(E)" in the chip column and explained in Appendix C, so one
  table serves both chip revisions.
- Block chapters end with a "Summary of ... Registers" section in prose-and-list form: register
  pair name, one paragraph of purpose, the numbered siblings listed beneath.

Bit diagrams
- Instruction and register word layouts in body text are labelled lists by bit range, high to low
  or as natural, with field abbreviations introduced in parentheses ("called VP").
- The chapter summary uses a full 16-bit grid with one column per instruction and a legend of
  abbreviations beneath it. Unused bits are marked "X" with the instruction to write them as zero
  for future compatibility.

Timing diagrams and timing data
- Timing is expressed mainly through derivations in prose (memory cycles per instruction, samples
  per scan line, clock periods), small two-column NTSC/PAL tables, and figures. The audio state
  machine is a state diagram with numbered states and a signal list. Formulae are typeset as
  displayed equations and then worked through with real numbers.

Notes, warnings, cautions
- "Warning:" as a paragraph prefix, indented, roughly 50 times in the book. Each is one or two
  sentences and nearly always says what breaks.
- Run-in notes: an indented paragraph that begins with a short title ending in a full stop (set
  in a distinct face in print; the OCR cannot show which)
  ("About Copper restart.", "About the sample data.", "ECS Copper.") and then explains. These
  carry side facts that would interrupt the main flow.
- "Note:" for illustrative caveats (figures not to scale and the like).
- Footnotes (numbered) for notation conventions the first time they are needed.
- There is no separate "caution" tier; danger and compatibility rules go in the Warnings and in
  the chapter 1 guideline lists ("Never ...", "Do not ...").

Code examples
- 68000 assembly throughout, with occasional C equivalents in chapter 1. Every data line carries a
  trailing comment saying what it does in English. Example headers are comment blocks with numbered
  notes. A single include file (Appendix I) defines all register names used by the examples, and
  chapter 1 lists the three assemblers the listings were checked against.
- Every example states its assumption: the program has taken over the hardware. The book says
  plainly that examples explain the hardware and are not finished code.
- Plain-English pseudo-code ("WAIT for ...", "MOVE ... to ...") is used before real code when
  explaining list construction.

Cross-references
- To chapters by number and quoted title; to sections by quoted section title; to appendices by
  letter. Forward references are common ("described in Advanced Topics below").

Appendices, glossary, index
- Appendices are of three kinds: restatements of chapter material for lookup (A, B), deltas for a
  later revision (C), and self-contained specifications of neighbouring parts (F, G, K).
- Glossary: term on its own line, definition indented beneath; multiple senses are numbered and
  prefixed by context ("1. With sprites, ... 2. In audio, ...").
- Index: two-level, with sub-entries indented beneath a head term; numbers and chip part numbers
  sort first; specific registers and concepts both get entries; uses comma-separated page lists.

Typography quirks
- Register and signal names in capitals; field abbreviations in capitals; hex with `$` prefix and
  sometimes a space in long addresses for readability.
- Running footers: chapter title plus page on one side, book title plus page on the other.
- Chapter label in lower case ("chapter five") above an upper-case chapter title; upper-case
  section sub-headings (e.g. "STOPPING THE COPPER") under mixed-case main section headings.
- Tables and figures numbered per chapter ("Table 5-3", "Figure 2-1"), caption beneath.

## Voice

- Second person, addressing a capable programmer: "you" set registers, "you" will be tempted.
  The authors appear as "we" only occasionally, mainly to explain their own choices (why the
  example include file exists).
- Present tense for how the hardware behaves; imperative for procedures and rules.
- Sentences are mostly medium length and plain, with technical terms introduced once in
  parentheses and then used freely. Paragraphs are short.
- Humour is rare and dry: a list ends by waiting for "the impossible", and the Copper's tiny
  instruction set is followed by a cheerful remark that it is enough to do a lot. The audio chapter
  admits sound work is fun but takes trial and error.
- Uncertainty is handled honestly and locally: "unpredictable results", "almost always",
  behaviour that "may" persist if a channel is off for under two samples, and a memory-map
  appendix that opens by saying a fixed map is impossible. The front matter disclaims errors
  outright and tells readers how to report them.
- Danger is handled with absolute rules plus reasons: never touch undefined bits, never read a
  write-only register (it can corrupt state), write zeros to unused bits for future chips. The
  reason is usually forward compatibility, and the cost of ignoring it is stated.
- The book repeatedly anticipates a specific wrong idea and closes it off before the reader acts on
  it, which reads as experience rather than caution for its own sake.

## Borrow

- Theory before registers. Open each block with what it does and how it behaves on the bus, then
  formats, then registers. Suits PP5022, BCM2722 and WM8758 references.
- The "tempting mistake" paragraph: after an idiom, explain why the obvious variant fails. Suits
  all three hardware references and the firmware guide; a natural home for our Gotchas.
- Instruction/register word layouts as labelled bit-range lists in the body, with a full bit grid
  plus abbreviation legend in the block summary. Suits PP5022 (DMA descriptors, mailbox words),
  BCM2722 (command words), WM8758 (9-bit register words with 7-bit address).
- Two register appendices, alphabetical and by address, sharing one symbol legend printed once
  (access type, strobe, reset value, evidence tag). Suits all three hardware references.
- A revision marker in the register tables, explained in one delta appendix, instead of
  duplicate tables. Suits 5G versus 5.5G differences in PP5022 and the 5.5G teardown.
- "About the Examples" in chapter 1: who owns each block at run time, what header defines the
  names, what toolchain the listings were built with, and the standing assumption that examples
  take over the hardware. Suits the firmware guide and games SDK guide, and every hardware
  reference preface.
- A single example header (their Appendix I) that every listing includes. Suits the games SDK
  guide and the hardware references (one `prokbox_hw.h`).
- Run-in titled notes ("About X.") for side facts, separate from Warnings. Suits every manual.
- Show derivations of limits with the arithmetic (as the audio minimum-period derivation does).
  Suits WM8758 (clocking and PLL ratios), BCM2722 (bandwidth), PP5022 (timer and DMA rates).
- Reading routes in the preface, by reader experience. Suits every manual, especially the user
  guide and games SDK guide.
- A short "Key Points" bullet list at the end of a hard chapter. Suits the games SDK guide
  (render loop, audio streaming) and BCM2722.
- A front-matter error-reporting section with what a useful report contains. Suits all manuals;
  adapt to the project issue tracker.
- Glossary senses numbered by context. Suits the shared glossary (terms like "channel" or
  "frame" mean different things in audio and video).
- A state diagram with a signal list using case to separate external from internal signals.
  Suits WM8758 and BCM2722 where we document state machines.

## Avoid

- Physics primers inside a hardware chapter (the audio chapter spends pages on what sound is).
  Put background in a short boxed aside or the bibliography. Applies to WM8758 and the SDK guide.
- Bolting a whole bus specification onto the end as an appendix (Zorro). If the 5.5G teardown
  needs board-level specs, give them their own manual or part.
- Dependence on an external operating system's arbitration that the manual cannot describe;
  every example's ownership story must be complete within our own firmware guide.
- Postal addresses, subscription offers and support-programme sales copy.
- Inline bibliography inside a chapter opener; collect references in an appendix.
- Ambiguous Warnings that mix "this will crash" with "this might matter one day". Our evidence
  tags plus a severity word can separate them.
- Assembly-only listings with commented data words; our readers expect C first, assembly only
  where the hardware demands it.
- Typography that only survives in print (lower-case chapter labels, upper-case sub-heads); our
  Markdown/HTML output should use real heading levels.

## Distinctive details

- The impossible-WAIT idiom and the paragraph that proves it can never fire: the book teaches by
  reasoning through edge cases rather than asserting them.
- Unused bits marked "X" with a standing rule to write zero, justified by future chips. It gives
  the manual a sense of stewardship over hardware not yet built.
- The appendix B legend of one-character symbols (strobe, Copper-protected, DMA-only) turning a
  dense register list into something scannable.
- The memory-map appendix that begins by declining to give a memory map, then gives the outline
  with a warning not to depend on it: candour about what is and is not fixed.
- Front matter that names the chip designers among the contributors and warns, in capitals, that
  compatibility across revisions is unlikely: an engineering document that knows it is
  provisional.
- Plain-English pseudo-code ("WAIT for line 150, MOVE ... to COLOR17") used as a stepping stone
  before real listings.
