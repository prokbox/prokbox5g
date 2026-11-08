# NeXTSTEP Developer's Library, Release 3: *NeXTSTEP Development Tools and Techniques* (third printing, November 1993) and *NeXTSTEP General Reference*, Volumes 1 and 2 (third printing, November 1993); NeXT Computer, Inc., written by NeXT Publications, published by Addison-Wesley

Sources used (bitsavers scans on archive.org, read from the full-text `_djvu.txt` OCR files):

- Development Tools and Techniques (ISBN 0-201-63249-7):
  `https://archive.org/details/bitsavers_nextReleaslopmentToolsandTechniquesNov93_18939555`
- General Reference Volume 1 (ISBN 0-201-62220-3):
  `https://archive.org/details/bitsavers_nextReleasralReferenceVolume1Nov93_41468752`
- General Reference Volume 2:
  `https://archive.org/details/bitsavers_nextReleasralReferenceVolume2Nov93_44351426`

Local copies in `refs/next-devdocs/` (`devtools-nov93.txt`, `genref-v1-nov93.txt`,
`genref-v2-nov93.txt`). The OCR mangles tables, screenshots and some code into columns, so
layout claims below are reconstructed from the text flow and page footers; I have flagged where
that matters. Release 1 (1990) "Concepts" and "Reference" volumes also exist on archive.org; I
did not use them.

Read in full: the Development Tools introduction and conventions, its complete contents,
chapter 1 (Putting Together a NeXTSTEP Application) and chapter 8 (The MallocDebug
Application); the General Reference introduction (how the manual and each chapter are
organised, the class, protocol, function and type specification formats, conventions) and its
complete contents. Read in part: Development Tools chapter 14 (Mach Object Files), chapter 15
(first tutorial), chapter 16 (warnings), the index; General Reference chapter 1 (Root Class,
introduction and the start of the Object class specification), chapter 16 (Sound: introduction
and NXPlayStream), Appendix E (Details of the DSP), Suggested Reading and the opening of the
Glossary.

Relevance: these are the closest thing in our reference set to a **firmware guide** and **games
SDK guide** pair. Development Tools is a toolchain-and-tutorial book (our firmware guide's
build, debug and tools chapters; our games SDK guide's tutorials). The General Reference is a
strict API reference with a fixed entry template (our SDK API appendices) that also, in its
appendices, drops to hardware level (memory maps, connector pinouts), which is a model for
mixing the two in our **PP5022**, **BCM2722** and **WM8758** references.

## Structure

**Development Tools and Techniques** (18 chapters plus Index; pages numbered chapter-page,
such as 8-5):

- Introduction: what the book covers and assumes; that an online copy exists; "How This Manual
  Is Organized" (one bullet per chapter); Conventions (syntax notation, special-key notation,
  notes and warnings).
- Chapter 1, Putting Together a NeXTSTEP Application: the whole development process in eight
  pages, from design to installation, pointing to the tool chapters.
- Chapters 2 to 10, one per tool, each ending in a "Command Reference": Project Builder,
  Interface Builder, Edit (the editor), Terminal, Icon Builder, DBModeler, MallocDebug, Process
  Monitor, the PostScript previewers.
- Chapters 11 to 13, the GNU compiler, preprocessor and debugger: adapted from the Free Software
  Foundation's manuals, each ending in its own legal section with the GNU licence wording.
- Chapter 14, Mach Object Files: a file-format specification (header, load commands, relocation,
  how executables are put together).
- Chapters 15 to 18, tutorials of rising difficulty: a content-free application, a one-button
  calculator, a text editor using several interface files, a custom palette that extends the
  interface builder itself.
- Index, then a back cover listing the other titles in the library.

**General Reference** (two volumes, 19 chapters, one per software kit):

- Introduction (Volume 1): the library context; a policy on documented versus undocumented API;
  precompiled headers; "How the Manual Is Organized" (one paragraph per chapter and appendix);
  "How the Chapters Are Organized" (the template, explained in detail); conventions.
- Volume 1: 1 Root Class; 2 Application Kit (over a thousand pages); 3 Common Classes and
  Functions.
- Volume 2: 4 Database Kit; 5 Display PostScript; 6 Distributed Objects; 7 Indexing Kit;
  8 Interface Builder; 9 Mach Kit; 10 MIDI Driver API; 11 NetInfo Kit; 12 Novell NetWare;
  13 Phone Kit; 14 Preferences; 15 Run-Time System; 16 Sound; 17 3D Graphics Kit; 18 Video;
  19 Workspace Manager.
- Appendices (Volume 2): A Data Formats; B Default Parameters; C Keyboard Event Information;
  D System Bitmaps; E Details of the DSP.
- Suggested Reading, Glossary, Index (Volume 2, covering both).

Every General Reference chapter has the same internal order: a three-line header block
(library, header directory, import line), then Introduction, Classes (alphabetical), Protocols,
Functions, Types and Constants (subdivided into defined types, symbolic constants,
enumerations, structures, globals), and Other Features.

## How one block or topic is laid out: Development Tools chapter 8, "The MallocDebug Application"

1. **Chapter opener page.** A small table of the chapter's sections with page numbers
   (Preparing, Using, Identifying Damaged Nodes, Finding Memory Leaks, Measuring Memory Usage,
   Command Reference), then the chapter title on its own.
2. **Purpose in three sentences.** What the tool measures (dynamic memory use), the two ways to
   measure (everything, or since a moment you choose), and that it also finds leaks.
3. **Components.** A two-bullet list: a replacement allocator library that gathers statistics,
   and the application that displays them. The reader learns the architecture before the steps.
4. **"Preparing Your Application".** The link flag, and the one ordering constraint that matters
   (it must come before the system library, or the allocator is not overridden). Then the
   equivalent route through the project tool's inspector.
5. **"Using MallocDebug".** How to open a running process (only prepared processes appear), then
   a full-width screenshot of the panel. The text below explains each column (zone, address,
   size, allocating function), names every allocation call the tool can see, and lists the three
   sort orders. Small pictures of the relevant buttons sit beside the paragraphs that describe
   them (the OCR shows them as stray bracketed labels; in print they are margin images).
6. **"Identifying Damaged Nodes".** Two symbols mark writes past either end of a block. The next
   sentence gives the commonest cause in practice (allocating a string's length without room for
   its terminator). A `Note:` adds that damaged blocks always sort first.
7. **"Finding Memory Leaks".** Explains the detector's method (scan memory for pointers to each
   block), then why it is called conservative: an integer that happens to look like a pointer can
   hide a leak, though the book says this is rare in practice. A second `Note:` gives the
   opposite failure (a pointer only into the middle of a block produces a false leak).
8. **"Measuring Memory Usage".** Two buttons, two sentences: set a mark, exercise the program,
   ask for new blocks. One clarifying sentence on what "new" means (still allocated, not merely
   allocated).
9. **"MallocDebug Command Reference".** Standard menus deferred to the user's guide; the
   application-specific menu as a two-column Command/Effect table.

The whole chapter is about six pages. Chapter 1 follows a similar shape at book level: a
discursive opening (how programs differ from applications), then the development process as
a sequence of short imperative-headed sections (Design, Create a Project, Write Code, Connect
Objects, Add Resources, Choose Document Extensions, Compile, Debug, Add Help, Translate, Make
Available), each a paragraph or two that names the tool and the chapter that covers it.

For contrast, a General Reference class entry (Object, chapter 1; NXPlayStream, chapter 16)
runs: name as heading; Inherits From; Declared In; Class Description (prose, possibly with
subheadings such as "Scaling and Peak Detection" and "Error Codes"); Instance Variables (each
with a one-line gloss); Method Types (a directory grouping methods by purpose); Class Methods;
Instance Methods. Each method gives its name, its full declaration, a description that starts
with what it returns, an optional `Note:` or `Warning:`, and a closing "See also:" line.

## Conventions

- **Notes and warnings, defined once.** The Development Tools introduction defines exactly two
  levels: a Note is incidental and can safely be skipped; a Warning must be read. Both are
  run-in paragraphs starting with the bold label and a colon, with no box or icon. Counts in the
  OCR: Development Tools about 26 notes and 9 warnings; General Reference Volume 1 about 23 notes
  and 7 warnings, so warnings are rare. The warnings in the reference are often admissions of
  current bugs or limits, opening with "Currently".
- **Syntax notation**, defined in both books with a small table: bold means type it literally,
  italic means substitute, square brackets mean optional (unless the brackets are bold), an
  ellipsis means repeatable. A separate table defines modifier-key notation, including how case
  and the lock key interact with each modifier.
- **Entry templates.** Every class, protocol, function group and type follows a labelled
  template, and the introduction documents the template itself with the meaning of each label.
  Function entries use lower-case side labels (declared in, synopsis, description) then
  capitalised optional sections (examples, return, exceptions, see also).
- **What is left out, and why.** Private instance variables and methods (leading underscore) are
  deliberately not documented, and the introduction says why and how to recognise them in a
  debugger. Structure fields shown in plain rather than bold must not be touched.
- **Tables.** Mostly two-column (Command/Effect, Header File/Contents, Notation/Meaning,
  Argument/Default Value), unnumbered, with plain column heads. Appendix E has a four-column
  memory map (start, end, on- or off-chip, description) using `$` for hex and a memory-space
  prefix on each address, and a pin table giving signal names and supply currents.
- **Figures.** Numbered chapter-hyphen-number ("Figure 15-1."), caption below, full stop after
  the number. Many are screenshots; the reference has few figures (inheritance trees, a circuit
  diagram in Appendix E).
- **Code.** Monospaced blocks with right-aligned trailing comments; structures shown as in the
  header with a comment per field, then each field explained in prose (Mach-O chapter).
  Constants shown as `#define` lines with value and comment.
- **Cross-references.** By chapter number and quoted title, or by section title in quotes; the
  "See also:" line ends almost every reference entry, with the owning class in parentheses when
  the target lives elsewhere. Chapter 1 of Development Tools is essentially a map of
  cross-references to later chapters.
- **Page furniture.** Page numbers are chapter-page; footers carry the chapter or section name.
  Each chapter opens with its own mini-contents with page numbers.
- **Appendices.** Reference material that cuts across kits: data formats, default parameters
  (including debug-only ones that make hidden behaviour visible), keyboard codes, system bitmaps,
  and hardware detail of the DSP.
- **Suggested Reading.** An annotated bibliography grouped by subject, with a sentence of
  judgement on some entries and instructions on how to obtain standards documents.
- **Glossary.** Alphabetical; each definition starts by naming the context when the term is
  kit-specific ("In the Indexing Kit, ..."), then a crisp definition, then "See also" terms.
  Synonyms point at the main entry.
- **Index.** Two-level, chapter-page locators; entries are typed by kind (a debugger command, a
  PostScript operator, an application), which makes scanning easy.
- **Colophon.** The copyright page says the books were written, proofed and imageset on NeXT
  machines.

## Voice

Second person, present tense, with contractions throughout (you'll, isn't, it's, don't), which
makes even the reference feel conversational. Sentences are medium length (around 15 to 25
words) and the paragraphs short. Reference entries switch to a clipped third-person declarative
that starts with the verb (the method "Returns..."), which is quick to scan.

The tutorials use "let's" and "we'll" and walk the reader through the screen, telling them what
will appear and what to leave alone. The tool chapters are imperative and procedural. Chapter
openings sometimes start with a reflective paragraph (the contrast between programs and
applications) before getting down to work.

Humour is dry and lives in the examples rather than the prose: an example selector named for a
gamble, a protocol example built from the two halves of a winter sport, a method described as
calling its "grander" sibling. Never jokes at the reader.

Uncertainty is handled candidly. Known limitations are stated as current fact in warnings; a
tool's own failure modes are explained in both directions (false negatives and false
positives) with an estimate of how often they matter. Danger gets a Warning paragraph and a
concrete consequence (files overwritten, a connection removed by a double-click). The
reference's introduction sets out a clear policy: documented behaviour that fails is a bug the
vendor will fix; undocumented behaviour may change without notice and is unsupported.

## Borrow

1. **Define the callout levels once, in the front matter, with what each obliges the reader to
   do** (skip-safe versus must-read), and keep warnings rare. Suits **all manuals**.
2. **Document the entry template in the introduction**, labelling every field and saying what it
   means, so the reader learns the shape once. For hardware references the fields would be the
   register's address, width, reset value, access, evidence tag, description, see also; for the
   SDK, the header, synopsis, description, returns, errors, see also. Suits **PP5022**,
   **BCM2722**, **WM8758** and the **games SDK guide**'s API appendix.
3. **A chapter header block** stating, in three fixed lines, what the chapter's subject needs:
   for SDK chapters the header, library and link option; for hardware chapters the base address,
   clock/reset control and interrupt line. Suits the **games SDK guide** and **hardware
   references**.
4. **A "method types" directory** before alphabetical entries: group by purpose, then describe
   alphabetically. The same works for registers (grouped by function before the address-ordered
   detail). Suits **hardware references** and the **games SDK guide**.
5. **"See also" at the end of every reference entry**, with the owning chapter in parentheses
   when it points elsewhere. Suits **all reference sections**.
6. **A published policy on documented versus undocumented behaviour**: what counts as a bug,
   what may change. For the SDK this is a stability promise; for the hardware references it maps
   onto evidence tags (an `[apple-RE]` or `[hypothesis]` fact is not a promise). Suits the
   **games SDK guide** and **firmware guide**.
7. **Say what is deliberately left undocumented, and how to recognise it** (here: a naming
   prefix). Suits the **games SDK guide** (private SDK symbols) and **firmware guide**.
8. **Tool chapters that end in a Command/Effect reference table.** Suits the **firmware guide**
   (build scripts, loader, serial console commands) and the **user guide** (menus, button
   combinations, with a notation table like the modifier-key one for click-wheel gestures).
9. **Explain each debugging tool's failure modes**: what makes it miss a problem and what makes
   it report a false one, and how often each happens. Suits the **firmware guide**.
10. **Pair a diagnostic symptom with its commonest cause** in the same breath (the
    string-terminator example). Suits the **firmware guide**, **games SDK guide** and **WM8758**
    gotchas.
11. **Graduated tutorials at the end of the book**, each building on the last and the last one
    extending the tools themselves. For us: draw a pixel, a one-button game, a multi-screen game
    with saves, then a custom SDK extension. Suits the **games SDK guide**.
12. **A one-chapter map of the whole process** (chapter 1 of Development Tools) that names each
    stage in a sentence and points to the chapter covering it. Suits the **firmware guide**
    (from clone to release) and the **games SDK guide**.
13. **Hardware memory map and connector tables in an appendix**, with start/end/name columns, a
    stated hex notation, aliases ("images") explained in prose, and supply current on power pins.
    Suits **PP5022** (memory map, dock connector), **BCM2722**, and the **5.5G hardware
    teardown**.
14. **Context-prefixed glossary entries** ("In the codec, ..."), so one shared glossary can
    serve the whole family. Suits **all manuals**.
15. **An annotated Suggested Reading list** grouped by subject, with how to obtain each item.
    For us: chip datasheets, the ARM7TDMI manual, Rockbox sources and wiki. Suits **all
    manuals**.
16. **Debug-only parameters listed as reference** (the defaults appendix includes switches that
    make hidden behaviour visible). Suits the **firmware guide** (debug build flags, verbose
    logging switches).

## Avoid

- **Screenshot-led tutorials.** Large portions of the tool chapters only make sense with the
  screenshots, which date fast and do not survive scanning or plain text. Prefer commands and
  text output.
- **Chapter-page numbering** (8-5) is a print convenience; in Markdown use numbered sections.
- **Imported third-party manuals with their own voice and legal boilerplate** in the middle of
  the book (the three GNU chapters). Summarise the parts specific to our toolchain and link out.
- **Splitting volumes by bulk** so that the glossary, appendices and the only index live in the
  second volume. Each of our manuals carries its own.
- **Purely alphabetical ordering** as the only route into a large kit; it works only because the
  grouped directory precedes it. Keep the directory.
- **Assuming another book has been read** (the user interface conventions, the language book).
  Our manuals should state prerequisites but not depend on them.
- **Warnings for trivial interface slips** (a double-click that removes a connection). This
  dilutes the must-read level.
- **Trademark marks on every kit name** at first mention, and a long trademark list up front.
- **Prices, ordering telephone numbers and postal addresses** in the reading list; they date
  immediately.

## Distinctive details

1. **Notes are officially skippable.** The front matter tells the reader that notes can be
   ignored and warnings cannot, which makes the levels mean something.
2. **The template is part of the documentation.** Several pages of the reference's introduction
   explain each label of a class, protocol, function and type entry before any entry appears.
3. **Candid "Currently" warnings** in the reference itself, admitting what a class cannot yet do.
4. **Dry wit in examples only**: a selector named for gambling, a protocol made of skiing and
   shooting, a "grander" method.
5. **A software API reference that ends in a DSP appendix** with a memory map, a 15-pin
   connector pinout with supply currents, a small circuit diagram and an instruction-set
   notation table: hardware detail where the programmer needs it.
6. **The colophon** boasting that the books were produced end to end on the company's own
   machines, a small statement of pride that suits a project writing its manuals on its own
   firmware.
