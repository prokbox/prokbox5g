# Inside Macintosh (two eras: the 1985 *Promotional Edition*, Apple Computer, March 1985, the pre-publication printing of what Addison-Wesley published later in 1985 as Volumes I–III; and the "New Inside Macintosh" books *Devices*, 1994, and *Macintosh Toolbox Essentials*, 1992, Addison-Wesley; sources: https://archive.org/details/bitsavers_applemacInionalEdition1985_53888388, https://archive.org/details/bitsavers_applemacInre198502_22173505, https://archive.org/details/inside-macintosh-1992-1994, each read from the `*_djvu.txt` full text)

Study notes for the ProkBox developer-guide style study (#161). Structure and tone only; nothing
here is quoted beyond a few words.

**Which texts, and why.** The archive.org scan of the published 1985 Volume I
(`insidemacintosh0000rose`) is a lending-library item behind a login, so I skipped it. In its place
I read the *Promotional Edition* (March 1985), which Apple sold as a cheap stand-in until the
Addison-Wesley edition appeared. It holds the same chapters in the same order, but each is still a
separately paginated draft "Programmer's Guide" with its own cover block. Its Volume I span runs
from the Road Map to Macintosh Packages. Where I describe the 1985 era below, I mean this text.
I also read the February 1985 draft of the *Macintosh Hardware* chapter (later in Volume III) because
it is the only part of the original set that documents registers. For the second era I read
*Inside Macintosh: Devices* (1994) and sampled *Macintosh Toolbox Essentials* (1992). That gave
me the Event Manager in both eras, so I could compare one topic directly. The 1984 three-ring-binder
draft (`bitsavers_applemacIn84_27699101`) is also in `refs/` but I only skimmed its front.

Read in full: the Road Map; the Toolbox Event Manager and Sound Driver chapters (1985);
the Hardware chapter's sound, video-page and VIA sections (1985 draft); the *Devices* preface and
Power Manager chapter. Sampled: the 1985 combined index; *Devices* ADB Manager chapter, glossary
and index; the *Toolbox Essentials* contents and Event Manager chapter (opening, the modifiers figure,
event-loop listings). The OCR is noisy. Figures and code tables are mangled, so where I describe
figure layout I am working from captions, legends and the prose around them.

## Structure

**1985 (Promotional Edition; order as printed).** Thirty-three numbered units, each a
self-contained "manual":

1. Road Map: the manual's own orientation chapter, with a sample program
2. User Interface Guidelines
3. Introduction to Memory Management
4. Programming in Assembly Language
5–17. Resource Manager, QuickDraw, Font Manager, Toolbox Event Manager, Window Manager,
   Control Manager, Menu Manager, TextEdit, Dialog Manager, Desk Manager, Scrap Manager,
   Toolbox Utilities, Packages *(the Volume I span)*
18–30. Memory Manager, Segment Loader, OS Event Manager, File Manager, Printing,
   Device Manager, Disk Driver, Sound Driver, Serial Drivers, AppleTalk Manager,
   Vertical Retrace Manager, System Error Handler, OS Utilities
31. Structure of a Macintosh Application (the Finder interface)
32. Apple Numerics Manual (SANE), bound in
33. Index to Technical Documentation: one index across every unit

The order goes up the software stack from user-facing Toolbox to Operating System. Every
chapter ends with its own glossary; there is no book-level glossary yet. The hardware chapter
was still being written. In the Road Map it is listed as forthcoming.

**1992–94 ("New Inside Macintosh").** The single tome is replaced by about two dozen
topic books. *Devices* runs: Preface (About This Book; Format of a Typical Chapter; Conventions
Used in This Book: special fonts, types of notes, assembly-language information; Development
Environment; For More Information) → Ch. 1 Device Manager → 2 Slot Manager → 3 SCSI Manager →
4 SCSI Manager 4.3 → 5 ADB Manager → 6 Power Manager → 7 Serial Driver → Glossary → Index.
A "Figures, Tables, and Listings" register follows the contents. Pages are numbered by chapter
(6-28, GL-3, IN-12). There are no appendices. *Toolbox Essentials* follows the same shape:
Introduction to the Toolbox, Event, Menu, Window, Control and Dialog Managers, Finder
Interface, Glossary, Index. Its Event Manager alone runs to about 130 pages, against about
33 for the 1985 chapter.

## How one block or topic is laid out

**Walk-through: the 1985 Toolbox Event Manager chapter (33 pp.).**

1. *Cover block.* A banner naming the documentation group, the title as "X: A Programmer's
   Guide", a file path code, a See Also list of the other units it depends on, a modification
   history (draft number, ROM version it describes, authors, dates), a short abstract, and a
   bulleted summary of significant changes since the last draft. That summary is candid. It
   records that a section was wrong and has been removed, and that one bit of a field is
   no longer documented because it is unreliable.
2. *Table of contents* for the chapter, with page numbers.
3. *About This Manual.* What the chapter covers, how it relates to the other Event Manager (OS
   versus Toolbox), what the short name means from here on, and a bulleted list of what you
   must already know.
4. *About the Toolbox Event Manager.* The concept: an event-driven program, the event queue,
   what "posting" means, and other things the manager does (read the mouse, count ticks,
   journaling).
5. *Concept sections, in dependency order.* Event Types (grouped by origin); Priority of Events
   (a numbered ranking followed by a paragraph explaining each rank); Keyboard Events (auto-repeat
   rules as a bulleted list of conditions, default timings in ticks, a full character-set chart, a
   key-code map of the keyboard); Event Records (the Pascal record, then one subsection per field:
   Event Code, Event Message with a bit-field figure, Modifier Flags, Event Masks).
6. *Using the Toolbox Event Manager.* Initialisation order and prerequisites, the shape of the main
   loop, then one subsection per event type saying what a typical application should do with it.
   This is where the reader learns which few routines matter.
7. *Toolbox Event Manager Routines*, grouped by task (Accessing Events, Reading the Mouse,
   Reading the Keyboard, Miscellaneous). Each routine gets its Pascal signature on its own line,
   then prose, then any notes or assembly-language notes.
8. *Advanced section.* The Journaling Mechanism. It opens with a plain explanation and an
   anecdote about an internal random-event test tool, says that only assembly programmers can
   use it, and then gives the driver-level detail (control codes, journal codes as tables).
9. *Summary of the Toolbox Event Manager.* Every constant, type and routine again as a compact
   Pascal listing, followed by an assembly-language block: equates, record field offsets, and a
   Name / Size / Contents table of low-memory globals.
10. *Glossary* of the chapter's terms.

**Same topic, 1992 (Toolbox Essentials ch. 2).** The skeleton is the same (introduction → About →
Using → Reference → Summary), but each part is much larger. The introduction now spends
several paragraphs placing the manager among its neighbours (Process Manager, Apple events, PPC
Toolbox), with cross-book pointers. "Using" breaks into about 25 task sections, each anchored by a
numbered, captioned Listing that is then walked through in prose. The modifiers field becomes a
proper bit figure, with each bit labelled "1 if …, 0 if …". The Summary is given three times: in
Pascal, in C and in assembly.

**Hardware-adjacent example, 1994 (Devices ch. 6, Power Manager).** Opening prose: what it does,
as a bulleted capability list; who needs it, and the statement that most applications do not;
which system version provides it, and that you should test for it first; prerequisites; and a
paragraph mapping out the chapter, including which interfaces are shown in Pascal and which in
C. About the Power Manager: the five power states, explained one by one, with the conditions
that count as "activity" as a bulleted list, the sleep sequence as a bulleted list of what the
system saves, and the sleep queue's four call reasons. Using: detecting presence (with a
bit list of capability flags); idle control, including five bulleted cases that pin down how
nested enable/disable calls interact; the wake-up timer; and writing a sleep procedure (Listings
6-1 to 6-9). Reference: Data Structures (a record, then "Field descriptions" as a two-column
glossary of fields); Routines grouped by task, each with a heading, a one-line summary, the
signature, a parameter list, then side-headed blocks DESCRIPTION, SPECIAL CONSIDERATIONS,
ASSEMBLY-LANGUAGE INFORMATION, RESULT CODES, SEE ALSO. Summary: Pascal, C, Assembly
(data structures, trap macros, result codes).

## Conventions

- **Register tables (1985 hardware draft).** Each VIA data register gets a sentence giving its
  base-plus-offset address and that of its direction register. A three-column table follows
  (Bit(s) / Name / Description), listed from bit 7 down to bit 0. A multi-bit field is shown
  as a range ("0-2") with "(mask)" after the name. A prose paragraph then explains every bit in
  turn. Individual bits are introduced inline as assembler equates with a trailing comment,
  followed by a one- to four-line 68000 fragment that sets or clears them.
- **Bit diagrams.** In 1985: a single box diagram of a 32-bit word with boundary bit numbers
  (31, 16, 15, 8, 7, 0) above it and field names inside, captioned "Figure n. …". In 1992: one
  column per bit, numbered across the top, with a leader from each bit to a label of the form
  "1 if X, 0 if not". Reserved bits are labelled as reserved for future use. The prose under the
  figure then explains the odd one out (bit 0 means two different things depending on event type).
- **Constants and masks** are printed as Pascal `CONST` lists with the value and a braced comment.
  The 1985 event masks are in *decimal* (2, 4, 8 … 16384, −32768), which hides the bit structure.
  Hex masks appear only for field extraction (`$000000FF`).
- **Timing.** The software chapters have no timing diagrams. Durations are given in prose with
  units: ticks (defined on first use as sixtieths of a second), microseconds (the 44.93 µs sound
  sample period), and percentages of CPU time per synthesiser. The hardware draft has a
  video-scan figure and states blanking intervals in prose. Formulas linking a parameter to a
  frequency are set on their own line, followed by a worked number.
- **Notes and warnings (1985).** The draft text puts "(note)" or "(warning)" on a line of its own
  as a placeholder for the glyph the typesetter would add, followed by an indented block. There is
  also a "(hand)" variant, used for the ROM-version applicability statement. Assembly-only material
  is a run-in paragraph headed "Assembly-language note:" so that Pascal readers can skip it. A
  bracketed tag after a routine's signature, "[No trap macro]", flags a status that affects how the
  routine can be called. Unfinished material is fenced with triple asterisks ("*** forthcoming ***")
  in the body text.
- **Notes (1992–94).** Three named tiers, each defined in the preface by its consequence:
  *Note* (interesting, not essential), *IMPORTANT* (essential to understand the text),
  *WARNING* (ignore it and you risk crashes or data loss). The preface gives a page reference to a
  real example of each. Each note ends with a small dingbat end-mark.
- **Parameter blocks (1994).** A four-column layout: a direction arrow (→ input, ← output,
  ↔ both), field name, type, short description. A cross mark flags fields the call may trash. The
  preface explains the notation once. Register-level calling conventions are shown as small
  "Registers on entry / Registers on exit" tables.
- **Code examples.** 1985: short Pascal fragments with right-aligned braced comments, a bullet
  ellipsis for elided lines, and a one-line explanation of why the fragment does what it does (for
  example, locking a handle to keep the heap tidy). 1992–94: numbered, captioned Listings
  ("Listing 6-2 Setting the wakeup timer"), each followed by prose walking through it. The preface
  disclaims them: compiled, mostly tested, not meant to be pasted into products.
- **Cross-references.** By quoted section title, plus a page in the later books ("see 'Installing a
  Sleep Procedure,' beginning on page 6-18"). Other books are cited by full title in italics.
  In 1985, other units are cited as "the File Manager manual", and the See Also list on each
  cover makes the dependencies explicit up front.
- **Appendices.** Almost none. In 1985 the per-chapter Summary does the job a register-summary
  appendix would do; the Road Map has a single appendix with the sample program's resource
  input. *Devices* has none at all, and leans on its Summary sections.
- **Glossary.** 1985: per chapter, alphabetical, "term: one-sentence definition". Terms are
  bold at first use in the body. 1994: one book-level glossary, two columns, with "See" and
  "See also" pointers between entries.
- **Index.** 1985: one combined index for all thirty-odd units. Every locator is a *two-letter
  manual code plus page* (EM-23, QD-46, SN-15), and a key at the top lists each code with the
  draft date of that unit. 1994: per book, nested two levels, with page ranges ("6-28 to 6-30") and
  a "defined" sub-entry pointing to where a term is introduced.
- **Typography quirks.** Code names in Courier from 1992. Glossary terms in bold. Chapter-relative
  page numbers. Running footers in the drafts carry date, author surnames and a file path with a
  revision suffix. Every 1985 chapter title ends "A Programmer's Guide". The 1994 Power Manager
  chapter explicitly mixes Pascal for older routines and C for newer ones, and says so.

## Voice

- **Person and tense.** Second person throughout, addressed to "you" and "your application".
  Present tense for behaviour, imperative for instructions. The manager is the grammatical
  subject of most sentences, so the software appears to act on the reader's behalf.
- **Sentence length.** Medium: mostly one main clause plus a qualifier, roughly 15–25 words.
  Lists carry anything enumerable. The 1985 text uses contractions freely ("it's", "you'll",
  "don't") and reads like a well-edited internal memo. The 1990s books use fewer, are more formal,
  and lean on stock phrases ("You can use the … function to …") at the head of every reference
  entry.
- **Humour.** Dry and rare. In 1985 the Road Map has a section warning programmers that their
  habits will not carry over, under a deliberately provocative heading. The journaling section
  tells the story of a test tool that fed the machine random events. Advanced sections are offered
  "for the curious". The 1990s books have essentially none.
- **Uncertainty.** The 1985 drafts are frank about it: they say outright that a manual does not yet
  exist, that a section was wrong, that a bit is unreliable and should not be used. The 1990s books
  handle variation instead. IMPORTANT notes say that a state or timer may be absent on some models
  and that the vendor may change internals, and readers are told to probe for capabilities at run
  time rather than assume. Neither era hedges routine facts. When something is known it is
  stated flatly.
- **Danger.** Warnings name the specific consequence and then offer the safe alternative. For
  example, a completion routine runs at interrupt level, so it must not allocate memory, so it
  should post an event and let the main loop start the next sound. The hardware draft adds a
  forward-compatibility caution to features that may not survive future models.
- **Reader management.** Both eras tell readers what they can skip, and which few routines out of
  many they will actually need. That tone, of saving the reader work, is the series' most useful habit.

## Borrow

1. **A "Using the …" section between theory and reference.** It tells the reader which handful of
   registers or calls they need for the common case before the full catalogue. *PP5022, BCM2722,
   WM8758* (as the "Programming model" section of each block); *games SDK guide*; *firmware guide*.
2. **A fixed chapter front block**: one-paragraph scope, "you should already know" list, and a
   See Also list of other chapters and manuals by prefix. *All manuals.*
3. **A family-wide index with manual-prefix locators** (`PP-6.3`, `WM-4.2`), keyed at the top of
   the index. This is exactly our two-letter prefix scheme. A combined index across the family
   could live in the firmware repo. *All, especially the three hardware references.*
4. **Fixed side-heads in every API entry**: Description, Special considerations, Result codes,
   See also, plus a "Register-level" block where relevant. *Games SDK guide; firmware guide
   (driver and kernel APIs).*
5. **Direction arrows on parameter and message fields.** → for host-to-device and ← for
   device-to-host, defined once in Notation. *BCM2722* (command protocol: words sent and responses);
   *games SDK guide* (in/out struct fields); *PP5022* (mailboxes between CPU and COP).
6. **A run-in second-audience note**, the way the "Assembly-language note" was used. For us, a
   "Hardware note:" in the SDK and firmware guides gives the register-level reality underneath an API
   (which WM8758 register a volume call writes, say), and readers who don't care can skip it.
   *Games SDK guide; firmware guide.*
7. **A status tag set immediately after a signature or register name**, like the bracketed trap tag.
   Our evidence tags (`[silicon]`, `[hypothesis]` …) should sit in exactly that spot, on the heading
   line of each register and routine. *All hardware references; firmware guide.*
8. **A per-chapter change summary that admits corrections**: what changed since the last revision,
   including "this section was wrong and has been removed". *BCM2722* (most volatile); *PP5022*;
   *WM8758*.
9. **An end-of-chapter summary** of every register, constant and field the chapter introduced, in
   addition to the book-level register appendix. *PP5022, WM8758, BCM2722.*
10. **Bit tables with a prose paragraph after them** that explains the interactions the table can't
    show. Equate names come first, then a two- to four-line read-mask-write example and a warning to
    preserve other bits. *PP5022; WM8758.*
11. **Numbered priority or ordering lists, each rank then explained**, as with event priority.
    *PP5022* (interrupt priority, DMA arbitration); *firmware guide* (scheduler).
12. **Explicit case tables for counted or nested calls**, like the five-case EnableIdle/DisableIdle list.
    *Firmware guide* (reference-counted clock and power gating); *games SDK guide*.
13. **A skippable "… hardware" subsection inside a software chapter**, flagged as optional.
    *Games SDK guide* (how a sound call reaches the codec); *user guide* is better without it.
14. **Formulas set on their own line, followed by one worked number.** *WM8758* (PLL and
    sample-rate maths); *PP5022* (timer and PLL dividers); *games SDK guide* (sound pitch).
15. **Model-scope warning at the head of a hardware chapter** ("covers only these models; addresses
    differ on …"). *5.5G teardown; PP5022; user guide* (5G versus 5.5G).
16. **Tell readers what they may skip.** *Firmware guide; games SDK guide; user guide.*

## Avoid

- **Repeating the interface in several languages.** The 1990s Pascal + C + assembly summaries triple
  the length for no gain. We have C, plus occasional ARM assembly.
- **Fourfold repetition.** "About", "Using", "Reference" and "Summary" all restating the same
  facts is the main reason the 1992 Event Manager is four times the 1985 one. One summary is enough.
- **Masks written in decimal**, especially with a negative top bit. Use hex and bit numbers.
- **Draft markers in shipped text** ("*** forthcoming ***"). Our equivalent is an evidence tag plus an
  entry in the "Open questions" appendix, not editorial scaffolding left in the prose.
- **Disclaiming the sample code.** Our examples must build and run as written.
- **Per-chapter glossaries** fragment the vocabulary. Keep one per manual.
- **Page-number cross-references.** We are Markdown. Use section numbers and anchors.
- **Corporate distance**, such as the vendor reserving the right to change internals. We are not the
  vendor. We say what we observed and tag it.
- **Legal and ordering boilerplate** (warranty pages, catalogue addresses).
- **Dialog-box screenshots as figures.** They date instantly. Our figures should be diagrams.

## Distinctive details

1. **The Road Map's warning that prior habits are wrong.** A whole short section exists only to
   unsettle experienced programmers before they start. Our *firmware guide* could open the same way
   for people who arrive from desktop Linux or from Rockbox.
2. **Two audiences in one text, separated by labelled run-in notes.** Pascal readers skip the
   "Assembly-language note" paragraphs without losing the thread.
3. **The combined index with two-letter manual codes**, each code keyed to the draft date of the unit
   it points into, so a reader can tell how fresh each reference is.
4. **Draft covers that show provenance**: who wrote it, which ROM it describes, what changed and what
   was wrong. That is the nearest thing in any classic reference to our evidence tags.
5. **A preface that teaches the layout.** "Format of a Typical Chapter" tells the reader, before
   chapter 1, which section to read for which purpose, and the note tiers are each defined with a live
   example page.
6. **The parameter-block arrow notation**, a tiny piece of visual grammar that makes direction of
   data flow obvious at a glance.
