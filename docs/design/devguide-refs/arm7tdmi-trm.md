# ARM7TDMI Technical Reference Manual (Revision r4p1, document ARM DDI 0210C, issue C, 26 November 2004, ARM Limited; studied from a copy of ARM's own PDF)

Source used: ARM's HTML pages at https://developer.arm.com/documentation/ddi0210/c/ return 403 to scripted clients, and ARM's free-download service gave me only the older Rev 3 manual (ARM DDI 0029G, April 2001, https://documentation-service.arm.com/static/5e8e1323fd977155116a3129). The r4p1 manual itself came from an unmodified copy of ARM's FrameMaker-generated PDF hosted at https://resenv.media.mit.edu/plug/ARM7TDMI_technical_reference.pdf (276 pages, front matter numbered in roman, body numbered by chapter). Text was extracted with `pdftotext -layout`. I used Rev 3 only to check that the structure and the missing timing values are the same in both editions; they are. Local copies are in `refs/arm7tdmi-trm/`.

Read in full: front matter, Preface, Chapter 3 (Memory Interface), Chapter 6 (Instruction Cycle Timings), Chapter 7 (AC and DC Parameters), Appendix A, Appendix C, Glossary and Index. Read in part: Chapter 1 (instruction summary), Chapter 2 (status registers, interrupt latency), Chapter 4 (coprocessor absence), Appendix B (debug control register).

## Structure

The real order, condensed. Every chapter opens with a one-sentence statement of purpose and a bulleted list of its sections, each with its page number.

```
Front matter   Title page · Release information (change history table: date, issue, change)
               · Proprietary notice · Confidentiality status · Product status · Web address
               · Contents · List of Tables · List of Figures
Preface        About this manual (product revision status rNpN, intended audience,
               using this manual, conventions, further reading) · Feedback
 1  Introduction             About the core · Architecture · Block, core and functional diagrams
                             · Instruction set summary (ARM and Thumb format tables)
 2  Programmer's Model       Operating states · Memory formats · Data types · Operating modes
                             · Registers · Program status registers · Exceptions
                             · Interrupt latencies · Reset
 3  Memory Interface         Bus signals · Bus cycle types · Addressing signals · Address timing
                             · Data-timed signals · Stretching access times · Privileged access
                             · Reset sequence after power-up
 4  Coprocessor Interface    Signals · Pipeline-following · Handshaking · Connecting coprocessors
                             · If you are not using an external coprocessor · Undefined and
                             privileged instructions
 5  Debug Interface          Debug systems · Signals · Clock domains · Core and system state
                             · EmbeddedICE-RT · Disabling it · Debug Communications Channel
                             · Monitor mode
 6  Instruction Cycle Timings   How to read the tables · one section per instruction class
                             (19 of them) · Instruction speed summary
 7  AC and DC Parameters     Timing diagrams (18 of them) · Notes on AC parameters (master
                             symbol table) · DC parameters
 A  Signal and Transistor Descriptions   Output-driver transistor sizes · Signal types
                             · Signal descriptions (one alphabetical table of every pin)
 B  Debug in Depth           JTAG scan chains, TAP controller, instruction register, public
                             instructions, test data registers, watchpoint registers, debug
                             control and status registers, programming breakpoints/watchpoints,
                             timing, a programming restriction (20 sections; longer than any chapter)
 C  Differences Between Rev 3a and Rev 4   Summary list · detailed description per change
Glossary · Index
```

Observations on the ordering:

- The book moves outward from the programmer to the pins: model, then bus, then coprocessor and debug ports, then cycle-by-cycle behaviour, then electrical timing, then a pin dictionary. Each layer assumes the one before it.
- Long, deep material that most readers skip (the full JTAG treatment) is pushed into an appendix, leaving a shorter overview chapter (Chapter 5) in the body. The appendix is the largest single part of the book.
- A "differences from the previous revision" appendix sits at the back, as a summary list followed by one subsection per change.
- The Preface's "Using this manual" gives each chapter and appendix a one-line "read this chapter for…" description. This is the only place the whole book is explained as a set.

## How one block or topic is laid out: Chapter 3, Memory Interface

Chapter 3 is the nearest thing in the manual to one of our hardware-block chapters: a bus, its signals, their encodings and their timing. Walking through it:

1. **Chapter opener.** A single sentence saying what the chapter describes, then nine bullets, one per section, each with a page reference. No introductory prose beyond that.
2. **3.1 About the memory interface.** Three sentences. It states the architecture (one shared 32-bit bus for instructions and data) and the one rule that shapes everything else (only loads, stores and swaps touch data memory). This is the whole overview; it is shorter than our draft's half-page.
3. **3.2 Bus interface signals.** Groups every bus signal into four classes (clocking, address class, memory request, data timed) as plain bulleted lists of names. It states the simplifying assumption used for the rest of the chapter (the wait input held inactive) and points forward to where that assumption is lifted. This "simple case first, complication in its own section" pattern is used throughout.
4. **3.3 Bus cycle types.** The theory of operation. It explains that the bus is pipelined (request signals a whole cycle early, address half a cycle early), shows one simple cycle as a figure, then defines four cycle types in a bulleted list. A small table gives the encoding of the two request signals against the four types. One sentence then states the rule a memory-controller designer must obey. Subsections 3.3.1–3.3.6 take each cycle type in turn: a paragraph of behaviour, often a short table (burst types), a timing figure showing the cycle against a DRAM-style row and column strobe, and a Note where a designer is likely to go wrong (for example, that an internal cycle may be followed by a non-sequential one to an unrelated address). It closes with a summary figure of all four cycle types side by side.
5. **3.4 Addressing signals.** A signal-by-signal reference: one numbered subsection per signal (3.4.1 to 3.4.7), each a short paragraph saying what it encodes, when it is valid, what the memory system must do with it, and a tiny two- or three-column encoding table where the signal has values (size encoding against significant address bits; opcode/data; user/privileged). A reserved encoding is listed as reserved with a dash, not omitted.
6. **3.5 Address timing.** A configuration choice (pipelined versus depipelined address) explained as a design decision: what each mode is for, which memory types want which, how to switch between them on the fly, and a legacy alternative signal kept for compatibility. There is a firm recommendation, attributed to the company, to use the pipelined mode in new designs. Three figures compare the modes; a forward reference sends the reader to Chapter 7 for exact numbers.
7. **3.6 Data timed signals.** The longest section. Data bus configurations (one bidirectional bus or two unidirectional ones) with block diagrams of the bus-splitter and pad logic, then abort signalling, byte-lane latch enables (with a numbered step sequence for a word read from half-width memory, and two figures), then byte and halfword accesses. Here the chapter gives its most useful table: access size and low address bits against which data lanes carry the value in each endianness. A figure shows how stores replicate a byte or halfword across all lanes.
8. **3.7 Stretching access times.** The complication postponed from 3.2: two methods (stretch the clock, or use the wait input), each in its own subsection with a figure.
9. **3.8 Privileged mode access** and **3.9 Reset sequence after power up.** Short closing sections. The first explains *why* a memory system should use the user/privileged signal (an OS touching memory on an application's behalf), which is a rare security rationale in this book, plus a table of the inverted mode outputs. The reset section says what the bus does while reset is held (address undefined after power-up, still incrementing), counts the internal cycles after release before the first fetch from the reset vector, and the cycles before that instruction executes, then shows it in a figure.

What is missing compared with our standard layout: there are no registers (this is a pin-level interface), no numeric timing here (all deferred to Chapter 7, which itself has no numbers), no worked software example, and no gotchas section. Pitfalls are spread through the chapter as Notes at the point of use.

## Conventions

**Preface conventions section.** The Preface sets out four kinds of convention in turn: typographic (what italic, bold, monospace, monospace-italic, monospace-bold and angle brackets each mean), timing diagrams (a key figure of waveform shapes), signals (how assertion relates to level; prefixes for bus families and active-low) and numbering (Verilog-style sized constants such as `8'h3F`). Every later convention is defined here once and not repeated.

**Signal naming.** Signal names are set in bold in running text. Active-low signals carry a lower-case `n` prefix (`nWAIT`, `nMREQ`, `nRESET`). Buses are written `A[31:0]`, single lanes `BL[3]`. Logic levels are always written in capitals, `HIGH` and `LOW`, and the manual speaks of asserting a signal while also freely saying "driven HIGH". Clock edges in timing symbols are marked with a trailing `r` or `f` (`MCLKr`, `MCLKf`).

**Numbers.** Despite the Verilog convention announced in the Preface, the body mostly uses `0x` hexadecimal (byte-latch values `0x3`, `0xC`, `0xF`) and bare binary strings in encoding tables (`10000` for a mode). Bit positions in register tables are written in brackets, `[5]`, and fields as `M[4:0]`.

**Register and bit diagrams.** There are two forms.
- *Field drawings* (the program status register, the debug control register): a row of bit numbers across the top, field names beneath, with reserved runs shown as dots. For the status register, named single bits carry leader lines down to labels below the diagram (flag names on the left, control bit meanings on the right). The debug control register is drawn as six labelled boxes with bit numbers above. The figure caption sits below.
- *Instruction format chart* (Figure 1-5): one row per instruction class, all 32 bit positions numbered across the top and repeated along the bottom, fixed bits printed as literal 0s and 1s and variable fields as named spans. This puts every encoding on one page for comparison.
Each drawing is followed by a bit-assignment table with columns `Bit` and `Function`, where the function cell may hold several paragraphs and a bulleted list (when to set the bit, when to clear it, which clock domain may write it). A bit that must not be used says only that it must be LOW.

**Reserved bits.** Handled in prose with a Note: preserve them with read-modify-write; do not rely on their values because later processors may change them.

**Tables.** Numbered per chapter (`Table 3-1`), caption above the table, right-aligned, title in plain words. Long tables repeat the caption with "(continued)" on each page. Encoding tables are small and frequent; a two-column table is used even for a single-bit signal with two meanings.

**Figures.** Numbered per chapter (`Figure 3-7`), caption below. Timing diagrams, block diagrams and data-lane drawings all share the sequence.

**Timing diagrams.** The Preface supplies a key (clock, transitions, stable bus, high impedance, and so on) and states two rules: shaded areas are undefined and may hold any value, and the reader must not infer any timing that is not drawn explicitly. In Chapter 3 the diagrams are *conceptual*: cycle-type labels across the top (`N-cycle`, `S-cycle`, `I-cycle`, `C-cycle`), signal names down the left, symbolic address values (`a`, `a+4`) on the bus, no parameter symbols. In Chapter 7 they become *parametric*: each diagram is annotated with T-prefixed interval symbols (`Taddr`, `Tah`), followed immediately by a table of `Symbol`, `Parameter` and `Parameter type` (Minimum or Maximum). A Note under the diagram states which control inputs are assumed held inactive during the cycle shown. Section 7.2 then lists every symbol alphabetically in one master table with a cross-reference column naming each figure that uses it.

**No numeric values.** The AC tables give only whether a parameter is a minimum or a maximum; the manual says to contact the supplier for the numbers. The DC section is a two-line instruction to ask the supplier about operating conditions and ratings. The same is true of the Rev 3 manual. This is because the core is licensed IP and the numbers depend on the foundry process.

**Instruction cycle tables (Chapter 6).** The model for anything we write about bus-level behaviour:
- Section 6.1 is a legend for every table in the chapter: request signals are shown in the cycle *before* the one they describe; address-class signals are shown in the cycle they belong to; `L` is the instruction length, `i` the fetch size code; brackets mean "contents of"; a bullet in the cycle column means "zero or more cycles".
- Each instruction-class section is a paragraph per cycle (what the datapath does in cycle 1, 2, 3), then a bullet list defining the variables used in that table (`pc`, `alu`, `c`, `d`, `s`), then the table.
- Table columns are `Cycle`, `Address`, size, direction, `Data`, the two request signals and the opcode signal, with a row per cycle. A final row with only an address shows the next fetch.
- Variants of one instruction (normal, destination is the PC, register-specified shift) are stacked in one table with an `Operation type` column.
- Section 6.20 condenses everything into cycle counts in the bus-cycle alphabet: `S`, `N`, `I`, `C`, with variables (`m` for multiplier early termination, defined by which high bits of the operand are all zeros or all ones; `n` words; `b` busy-wait cycles). An "Additional" column gives the extra cost of writing the PC.

**Worked calculation.** Section 2.9 gives the worst-case interrupt latency as a short labelled sum (synchroniser, longest instruction, abort entry, interrupt entry), with each term defined in a two-column list, a total in cycles and a conversion to microseconds at a stated example clock. It is the only worked example of its kind in the book and it is very effective.

**Notes and cautions.** Two levels only. A small bold heading word ("Note" or "Caution") on its own line between horizontal rules, then one to three short paragraphs. I counted 79 Notes and 3 Cautions in the whole book, and no Warnings. Notes carry design advice, clarifications of a figure, and recommendations. The Cautions are kept for things that put the processor in an unpredictable state, break correctness silently, or might be wrongly relied on for security. The scarcity is what makes them work.

**Signal descriptions (Appendix A).** One alphabetical table of every pin with columns `Name`, `Type` and `Description`. The Name cell holds the signal name in bold and, on the line beneath, its expansion in plain text. The type is a one- or two-letter code (`IC` input with CMOS thresholds, `O` output, `P` power) defined in a small table just before it. Descriptions say what the signal does, when it changes relative to which clock edge, what to tie it to if unused, and where in the body to read more.

**Unused-feature sections.** Section 4.6, "If you are not using an external coprocessor", says exactly how to tie off the interface and what then happens to coprocessor instructions. Several signal descriptions do the same ("must be tied HIGH if…").

**Cross-references.** Always the section title in italic followed by "on page 3-28". Chapter references are "Chapter 7 AC and DC Parameters". Forward references are common and explicit about why ("for the exact timing requirements see…"). External documents are cited by title and document number (the Architecture Reference Manual, the IEEE JTAG standard).

**Revision status.** The Preface explains the `rNpN` (major/minor revision) scheme. Release information is a table of date, issue letter and one-line change. Every page footer carries the document number and copyright, and the page header carries the chapter title.

**Glossary.** Introduced by one sentence saying the definitions given are the meanings intended in this book. Entries are alphabetical: term on the left, definition on the right, abbreviations as one-line "See *expanded term*" entries, and "See also" links between related terms. Some definitions are oddly general (one is defined in purely mathematical terms rather than as the chapter uses it).

**Index.** Three columns, two levels (primary, indented secondary, occasionally a third), page numbers in chapter-page form. Signals are indexed by name; timing diagrams are gathered under one heading with every diagram as a sub-entry.

**Typography quirks and errors.** The editing is not flawless: the Appendix A section list names the same section twice and omits the signal-descriptions section; there are missing spaces and spelling slips in the Preface and Index; and the Verilog numbering convention announced in the Preface is barely used. These are small, but they show that even a corporate reference with an errata address slips, which is an argument for our having automated checks.

## Voice

- **Person.** Mixed in a consistent way. The processor is described in the third person ("the processor broadcasts…"). The reader, assumed to be a hardware or system designer, is addressed as "you" when an action is required. The company appears in the third person when it makes a recommendation ("ARM Limited strongly recommends…"), which gives the advice a formal, institutional weight.
- **Tense and mood.** Present tense throughout. Requirements use "must" and "must not" without softening; options use "can". There is very little "should".
- **Sentence length.** Short to medium, rarely more than two clauses. Paragraphs are two to five sentences. The prose is almost entirely declarative, and anything sequential is turned into a bulleted or numbered list.
- **Humour.** None at all. Not even dryness. The tone is that of a specification.
- **Uncertainty.** Handled by saying the behaviour is *unpredictable* or *undefined*, by deferring to another document (the architecture manual, the supplier), or by "might change in future implementations". It never guesses and never says how it knows something. Where a value is not given it says whom to ask, not why.
- **Danger.** Plain statements of consequence: a forbidden write leaves the core in an unrecoverable or unpredictable state, and the remedy (apply reset) follows in a few words. Cautions are rare and short. There is no emotional language.
- **Recommendations** are distinguished from requirements: "must" for the first, "recommends" for the second, and legacy features are kept but plainly marked as not for new designs.

## Borrow

1. **A chapter opener of one purpose sentence plus a section list.** Cheap to produce from headings and helps readers who arrive from the index. *PP, VC, WM; FW and GS at chapter level.*
2. **A legend section before a family of dense tables** (Chapter 6.1). For our cycle-accurate and bus-level tables, define every symbol and column once at the head of the chapter, then keep each table's preamble to a short "where:" list of its local variables. *PP (memory and DMA timing, the COP/CPU arbitration), VC (host-interface transactions), firmware guide's performance chapter.*
3. **Cycle-cost summaries in a small alphabet.** A final table that expresses each operation's cost as a formula in named cycle types and variables, with the variables' definitions above it. Ideal for SDRAM, IRAM and cache-miss costs, and for the games SDK's performance chapter (cost of a blit or a sprite per pixel, expressed symbolically). *PP, GS.*
4. **A worked latency sum.** Section 2.9's labelled-term arithmetic (each term named, defined, valued, then summed and converted to time at a stated clock) is the right form for interrupt latency, audio buffer deadlines and frame budgets. Give each term an evidence tag. *PP (interrupts), WM (DAC buffer refill deadline), GS (frame budget at the LCD refresh rate).*
5. **Parametric timing diagrams with a symbol table beneath, plus a master alphabetical symbol list in an appendix** with a column naming every figure that uses each symbol. Our draft already has the per-diagram table; add the master list to the electrical and timing appendix. Name symbols `t` + short mnemonic, with `r`/`f` for edges. *PP Appendix E, WM, VC.*
6. **Conceptual timing diagrams before parametric ones.** Chapter 3 draws cycles with symbolic addresses and cycle-type labels and no numbers; Chapter 7 adds the numbers. Do the same: theory sections show shape, timing sections show values. *PP, WM (I²S and control interface), VC.*
7. **An alphabetical pin and signal dictionary** with name, expansion, direction code and a description that always says what to tie it to and which section to read. For us this becomes the dock connector, the click-wheel lines, the BCM2722 host-bus lines and the codec's pins as wired. *PP, VC, WM, 5.5G teardown.*
8. **"If you are not using X" sections.** Say exactly how to leave a block idle or safe (clocks off, pins in a defined state, interrupts masked) and what then happens if code touches it. *PP (COP, USB, FireWire-era leftovers), WM (unused inputs and outputs), FW (drivers that must park hardware they don't use).*
9. **Two admonition levels, used very rarely.** 79 notes against 3 cautions in 276 pages shows how scarce the strongest level should be. Our three levels are fine, but we should set a budget: on the order of one Warning per chapter at most. *All manuals.*
10. **Preserve-reserved-bits guidance stated once** as a general rule in front matter, repeated only where a register is commonly written whole. *PP, VC, WM.*
11. **A revision-differences appendix**, a summary list followed by one subsection per change. This is our "5G versus 5.5G" and "PP5020/PP5024" appendix. *PP Appendix F, 5.5G teardown, VC.*
12. **Long specialist material in an appendix, with a short overview chapter in the body.** Their debug chapter versus debug appendix fits our JTAG and boot-ROM recovery material. *PP, FW.*
13. **"Using this manual" with one line per chapter**, phrased as what the reader will find there. *All manuals; especially the user guide, where readers dip in.*
14. **Unpredictable versus undefined versus reserved, used precisely.** We should define these three words in our Notation front matter and use them only in their defined senses, then layer evidence tags on top. *PP, VC, WM.*

## Avoid

- **Missing numbers.** A timing table that only says "Maximum" with no value is useless to our readers, who have no supplier to ask. Every row in our tables carries a value or `—` with an evidence tag and a §N.9 entry explaining the gap.
- **Page-number cross-references.** We have no fixed pages. Keep their habit of naming the target section *by title*, but pair it with our section number and link (`§PP-3.4, Addressing signals`) instead of a page.
- **Verilog-style constants** (`8'h3F`). Wrong audience for us; our `0x` and `0b` notation stays. Also avoid announcing a convention and then not using it.
- **Bold for signal names.** In Markdown bold competes with admonition labels and emphasis. Keep our code style for signals, with active-low marked consistently (our draft says trailing `_N`; ARM's leading `n` is the other common form; pick one and never mix).
- **HIGH/LOW as the primary language.** ARM can do this because polarity is fixed by the `n` prefix. Our draft's assert/deassert rule is safer for reverse-engineered pins whose polarity may be uncertain; use HIGH/LOW only for measured levels.
- **Institutional voice** ("the company strongly recommends"). We have no corporate author; say "we recommend" or simply give the reason.
- **Total absence of explanation of *why*.** ARM rarely says why a rule exists. Our readers are reverse engineers and benefit from the reasoning; keep Notes that explain why.
- **Generic glossary definitions** copied from general computing usage. Our glossary should define each term as it applies to this hardware.
- **Repeated copyright and document-number footers** on every page, and confidentiality boilerplate. Not needed in Markdown; one front-matter block suffices.
- **Repeating the section list at the top of every chapter by hand.** Generate it from headings, or it will drift, as their Appendix A list did.

## Distinctive details

1. **The cycle tables.** Every instruction class becomes a small grid of bus signals against cycle number, with symbolic addresses (`pc+2L`, `alu`, `(alu)`) instead of numbers. It is how the book makes the pipeline visible, and it is the most imitable thing in it.
2. **The bus-cycle alphabet.** Four letters (N, S, I, C), defined in Chapter 3, become the units in which every instruction cost in Chapter 6 is expressed. A tiny vocabulary carries the whole performance model.
3. **Timing without times.** Elaborate parametric diagrams and an exhaustive alphabetical symbol list, but every value deferred to the supplier. The structure is complete and the content is someone else's job, which is a direct result of ARM licensing IP rather than selling chips.
4. **A preface that teaches the reader how to read waveforms**, including the instruction not to infer any timing that is not drawn. This warning sits before any diagram appears.
5. **Extreme restraint with warnings.** Three Cautions in the whole book, each about a different kind of harm: unpredictable state, silent corruption, false security.
6. **Plain, signal-first prose with no humour** and short sentences, organised so that nearly every paragraph could be turned into a bulleted list without losing anything.
