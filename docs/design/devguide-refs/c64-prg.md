# Commodore 64 Programmer's Reference Guide (first edition, eleventh printing 1984, © 1982; Commodore Business Machines with Howard W. Sams; source: https://archive.org/details/commodore-64-programmers-reference-guide_202205, read from `C64_Programmer's_Reference_Guide_djvu.txt`)

Study notes for the ProkBox developer-guide style study (#161). Structure and tone only; nothing
here is quoted beyond a few words. The OCR is rough in places (BASIC listings and bit-grid tables are
partly garbled), so table layouts are described from their headers and surrounding prose.

Read in full: front matter and introduction, chapter 4 (sound and music), appendix O (SID chip
specification). Read in part: chapter 3 (graphics: locations, bank selection, raster and interrupt
registers, sprite summary), chapter 5 (KERNAL routine entries, memory map), chapter 2 (a keyword
entry). Sampled: appendix G (VIC register map), the glossary and the index.

## Structure

The book is ordered by what a home programmer wants to do, not by chip. It moves from BASIC to
machine code to I/O, and the chip-level specifications are pushed to the back as reproduced
datasheets. Page numbers run through the whole book; the introduction uses roman numerals.

Front matter
- Copyright page, table of contents (chapters, then a lettered appendix list, then glossary,
  index, a quick reference card and a fold-out schematic).
- Introduction: what the book is and is not (a reference, not a course; go to the user's guide
  first), what is included, how to read the syntax notation (a numbered list of rules plus a
  symbol table with examples), an "applications guide" listing things you might do with the
  machine, and how to reach the user community.

Chapters
1. BASIC Programming Rules: the operating system's three parts, the character set, constants,
   variables and arrays, expressions and operator precedence, string handling, input techniques,
   how to "crunch" programs to save memory.
2. BASIC Language Vocabulary: keyword and abbreviation list, then an alphabetical dictionary of
   every keyword, then the keyboard and the screen editor.
3. Programming Graphics: overview of modes, graphics locations (bank selection, screen, colour
   and character memory), each character and bitmap mode, smooth scrolling, sprites in depth,
   other features (blanking, raster register, interrupt status, colour combinations), and then
   a second, gentler tutorial pass on sprites ("another look") with a sprite-making chart.
4. Programming Sound and Music: introduction, volume, frequency, multiple voices, waveforms,
   envelope generator, filtering, advanced techniques, synchronisation and ring modulation.
5. BASIC to Machine Language: what machine code is, a simple memory map, the CPU registers, a
   monitor, hexadecimal, addressing modes, branches, subroutines, beginner tips, the instruction
   set with timings, memory management, the KERNAL (power-up, how to use it, every user-callable
   routine, error codes), mixing machine code with BASIC, the full memory map and I/O assignments.
6. Input/Output Guide: screen, printers, modem, cassette, disk, game ports (joystick, paddles,
   light pen), RS-232, user port, serial bus, expansion port, a Z-80 cartridge and CP/M.

Appendices (A–P)
- A keyword abbreviations; B screen display codes; C ASCII and CHR$ codes; D screen and colour
  memory maps; E music note values; F bibliography; G VIC chip register map; H deriving
  mathematical functions; I pinouts; J converting other BASIC dialects; K error messages;
  L 6510 CPU specification; M 6526 CIA specification; N VIC-II specification; O SID
  specification; P glossary.
- Then index, quick reference card, schematic.

## How one block or topic is laid out: chapter 4, Programming Sound and Music (with appendix O)

The sound chip is documented twice: a hands-on tutorial chapter and a datasheet appendix. The pair
is the interesting design, so both are walked through.

Chapter 4 (tutorial)
1. Chapter opener page: the chapter number, a three-line title, and a bulleted mini-contents of
   the chapter's sections. Every chapter opens this way.
2. Introduction: an enthusiastic description of what the chip can do, a promise that neither
   programming nor music expertise is needed, and a statement of method: every section starts
   with a program and a line-by-line explanation, and the technical explanation is optional
   reading. The register map is deferred to appendix O by reference.
3. Access mechanism: how to reach the chip (writing to memory), the base address, and the trick
   of base-plus-offset so the reader remembers one number.
4. Example Program 1 then a line-by-line explanation table (line number in one column, a short
   imperative gloss in the other: set this, clear that, loop for duration).
5. Volume control: one register, its range, and a pointer to the line in the example that sets it.
6. Frequencies of sound waves: a plain-language physics analogy (ripples in a pond), then the
   conversion formula and the arithmetic for splitting a value into high and low bytes, and a
   pointer to the note table in appendix E.
7. Using multiple voices: Example Program 2 and its explanation, then a note-encoding formula
   given as numbered arithmetic steps and as a single expression.
8. Controlling multiple voices: the timing scheme in numbered steps, with an explicit remark that
   it is only one way to do it.
9. Changing waveforms: the reader modifies Example 1 by changing two numbers, runs it, and is told
   what to listen for. Each waveform gets a small sketch and a one-line character description.
10. Understanding waveforms: harmonics in plain terms, with figures.
11. The envelope generator: the four phases defined by what you hear, a sketch, then which nybble
    of which register holds each (the word "nybble" is defined on the spot), a rate table, and
    preset values that imitate instruments.
12. Filtering: add one line to the example, hear the difference, then the three filter types and
    their combination, then the bit-level detail of the mode/volume register.
13. Advanced techniques: reading back oscillator 3 and envelope 3, and three more example programs
    (vibrato, siren, hand clap), each with its line-by-line table.
14. Synchronisation and ring modulation: short, with a final example.

Appendix O (datasheet)
1. One-paragraph product description, then a bulleted feature list with key numbers (envelope
   rate ranges, filter range and slope).
2. Block diagram.
3. Functional description of a voice, the filter, and the readback and analogue inputs.
4. Register map table: a grid with one row per register and one column per data bit, plus
   address and name columns.
5. Register descriptions in address order. Each register gets a heading with its name and number,
   then prose. Control registers are described bit by bit with run-in labels in the form
   "NAME (Bit n):". Formulae for frequency and pulse width are given generally and then for the
   standard clock. Voices 2 and 3 are described only as differences from voice 1.
6. NOTE paragraphs for non-obvious behaviour: waveforms combine by logical AND rather than mixing,
   and one combination can lock the noise generator until reset; the gate bit can be toggled
   mid-envelope; filter modes are additive.
7. Pin descriptions, electrical characteristics and bus timing (cycle time, access time).
8. Application notes: how to build typical instrument envelopes (a sustained instrument, cymbal-like percussion, piano and harpsichord, organ,
   "backwards") with sketches and suggested values.

The result: a reader can learn by ear in chapter 4 and look up exact behaviour in appendix O,
and the chapter keeps pointing to the appendix rather than repeating it.

## Conventions

Register tables
- Appendix G and the datasheet appendices use a bit grid: register number in decimal and hex,
  then eight columns headed by data-bit names from the most significant (`DB7`) down, then a
  name/purpose column. Bits are labelled with short mnemonics; unconnected bits say so.
- In the chapters, register settings are taught through small value tables: decimal value, bit
  pattern with `X` for bits you must not disturb, the resulting address or bank, and a marker for
  the default. These tables pair with a read-modify-write one-liner showing how to change only
  the relevant bits.
- Interrupt registers get a latch table: mnemonic, bit number, description.

Bit diagrams
- No separate drawn bit diagrams; the grid tables do that job. Bit patterns appear inline as
  binary strings with `X` placeholders.

Timing diagrams and timing data
- Timing is mostly absent from the tutorial chapters (loops "for duration"). The datasheet
  appendices carry read/write cycle tables with symbol, parameter, min/max and units, and
  timing figures. Envelope rates are a table of values with time per cycle.

Notes, warnings, cautions
- "NOTE:" paragraphs set in a box or rule-delimited block, in the chapters and appendices alike.
  They carry both helpful asides (blanking the screen speeds the CPU slightly) and real hazards
  (noise lock-up). There is no separate warning tier.
- Starred footnotes under tables for caveats (an address must have the bank base added; the ROM
  character set is not visible in some banks).
- In KERNAL entries, a capitalised "REMEMBER:" line flags the common misuse.

Code examples
- BASIC listings with numbered lines, followed by a two-column line-by-line explanation table.
  Later examples are labelled as modifications of earlier ones ("Example 1 modified") so the reader
  edits rather than retypes. Machine-code chapters use monitor-style listings.
- One-liner idioms (read, mask, OR in, write back) appear repeatedly as the standard way to touch a
  shared register.

Reference entry templates
- BASIC keyword: name as heading, type, format (in the notation from the introduction), an
  "Action" description, then examples.
- KERNAL routine: numbered entry, function name, purpose, call address (hex and decimal),
  communication registers, preparatory routines, error returns, stack requirements, registers
  affected, description, numbered "How to use" steps, then an example. This is the closest thing
  in the book to a modern API reference page.
- Memory map rows: label, hex address or range, decimal, short description.

Cross-references
- To appendices by letter ("see Appendix O"), to sections by name, and to specific lines of an
  example program by number. Rarely to page numbers.

Appendices, glossary, index
- Appendices mix lookup tables (codes, notes, error messages), conversion help, and reproduced
  chip datasheets that keep their own internal numbering (their own "Table 1", "Table 2").
- Glossary: very short (a few dozen terms), one-line definitions, laid out as two columns.
- Index: single level with long comma-separated page lists, page ranges, roman numerals for the
  introduction, and "see" references to a preferred term.

Typography quirks
- Keywords, register names, chip names and emphasis in capitals; capitals also used for stress in
  running text ("NOT").
- Addresses usually in decimal with hex in parentheses, because the BASIC reader works in
  decimal; the appendices switch to hex first.
- Running footers carry the chapter's short title in capitals plus page number.
- Section headings in capitals; chapter openers with a bulleted mini-contents.

## Voice

- Second person throughout, warm and encouraging, with "we" for the authors and frequent "let's".
  It talks to a hobbyist at a keyboard: asks if the computer is ready, tells you to type the
  program in, save it, run it, and listen.
- Present tense and imperatives in the tutorial parts; short to medium sentences; jargon defined
  in passing in the same sentence.
- Humour and salesmanship blend: superlatives about the chip, exclamation marks, a reader cast as
  a conductor of a home orchestra. The datasheet appendices switch to a flat, neutral engineering
  voice, so the book has two distinct registers.
- Uncertainty is rarely acknowledged in the chapters; methods are offered as "one way" and the
  reader is invited to find their own, which is humility about technique rather than about facts.
  The datasheets state limits plainly with numbers.
- Danger is handled lightly: advice to be careful not to disturb neighbouring bits, a NOTE about a
  lock-up, a reminder of which call is unnecessary. Nothing in the chapters says the machine can be
  damaged or left in a bad state.

## Borrow

- Two layers per chip: a task-led tutorial chapter that points to a spec-style section for exact
  behaviour, and never duplicates the bit-level table. Suits the games SDK guide (tutorial) paired
  with the WM8758 and BCM2722 references (spec); also the firmware guide paired with PP5022.
- Line-by-line explanation tables after each listing. Suits the games SDK guide and the user
  guide's scripting or homebrew sections; a lighter form fits firmware guide walkthroughs.
- "Example N modified" chains: later examples are edits of an earlier one, with only changed lines
  shown. Suits the games SDK guide (sound, sprites, input) and the WM8758 worked examples.
- Value tables with `X` don't-care bit patterns next to a read-modify-write idiom. Suits PP5022
  and WM8758 register descriptions where several fields share one register.
- Datasheet-style register descriptions with run-in "NAME (Bit n):" labels, and later instances
  described only as differences from the first. Suits WM8758 (left/right channel pairs) and
  BCM2722 (repeated layer or channel blocks).
- Formulae given generally and then specialised to our actual clock. Suits WM8758 (PLL, sample
  rates) and PP5022 (timer, I2S clocks).
- The KERNAL entry template (purpose, entry address, inputs, prerequisites, errors, stack,
  clobbered registers, description, how to use, example). Suits the games SDK API reference and
  firmware guide service calls; the "prerequisites" and "registers affected" fields are worth
  keeping.
- Chapter openers with a mini-contents list. Suits every manual, especially long ones.
- Application notes after the spec (instrument envelopes) to show the reference in use. Suits
  WM8758 (equaliser and output-stage recipes) and the games SDK guide's audio chapter.
- Starred table footnotes for caveats that apply to one cell or column. Suits all hardware
  references.
- Syntax-notation rules stated once in the introduction with a symbol table. Suits the firmware
  guide (shell or build commands) and games SDK guide.

## Avoid

- Sales voice: superlatives, exclamation marks, claims of being the best available. Our manuals
  describe a reverse-engineered device and must stay measured.
- Decimal-first addresses and magic POKE numbers in teaching material; we are hex-first and use
  symbolic names.
- Teaching the same topic twice in two places (sprites appear in two tutorials in one chapter) and
  inconsistent register numbering between chapter and appendix (decimal register indices in one,
  hex offsets in the other).
- Reproduced vendor datasheets with their own internal numbering dropped into the appendix
  sequence; ours should share the manual's numbering and evidence tags.
- Physics-by-analogy at length in a reference chapter; keep a short aside.
- A glossary of one-line definitions too thin to help; ours should explain enough to be useful.
- Silence on uncertainty and hazard in the chapters. Our evidence tags and Warnings must appear
  in tutorials too, not only in the reference sections.
- Timing loops "for duration" in examples; examples should use real timers.
- Capitals for emphasis in running prose.

## Distinctive details

- Every program is followed by a two-column line-by-line table, which makes the book feel like a
  patient tutor sitting beside the reader.
- The same chip is taught twice in two voices: playful chapter, sober datasheet appendix.
- Instrument presets for the envelope generator (violin, xylophone, piano) turn abstract register
  values into something you can hear and remember.
- The KERNAL entries' fixed field list, including stack use and clobbered registers, is a
  surprisingly modern API reference format for a 1982 consumer book.
- Bit patterns with `X` placeholders next to read-modify-write one-liners: a compact way to teach
  "change only your field".
- The book closes with physical extras: a quick reference card and a fold-out schematic of the
  whole machine.
