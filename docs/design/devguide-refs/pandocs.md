# Pan Docs (living Markdown edition, repository head 89f82b2 of 30 October 2026; maintained by the gbdev community, released under CC0; https://gbdev.io/pandocs/ and source at https://github.com/gbdev/pandocs)

Studied from a shallow clone of the source repository (refs/pandocs/repo) plus the rendered home page, one chapter page, `print.html` and `single.html` (refs/pandocs/site). The text began in January 1995 as a plain-text file by Pan of Anthrox, passed through several hands (Martin Korth's 2001 revision among them), became a wiki in 2008, and moved to Markdown in 2020 and to mdBook in 2021. The repository keeps the old `.txt` and `.pdf` versions in `historical/`.

## Structure: the real chapter and appendix order, condensed

The order is set by `src/SUMMARY.md`. Unnumbered front matter, then eight titled parts, then a references page. Nothing is numbered anywhere: there are no chapter, section, table or figure numbers.

```
Front     Foreword · Acknowledgements · History
Overview  Specifications · Memory Map
I/O Ports Summary (the full register list)
          Graphics → Tile Data, Tile Maps, OAM (→ OAM DMA Transfer), Window,
                     LCD Control, LCD Status Registers, Scrolling, Palettes,
                     Rendering (→ Pixel FIFO)
          Audio → Audio Registers, Audio Details
          Joypad Input · Serial Data Transfer
          Timer and Divider Registers (→ Timer Obscure Behaviour)
          Interrupts (→ Interrupt Sources, HALT)
          CGB Registers · Infrared Communication
Super Game Boy   Summary, unlocking, packet and VRAM transfers, palettes,
                 command summary with seven command-family pages (including
                 removed and undocumented commands)
CPU Specifications   Registers and Flags · Instruction Set · Comparison with Z80
Cartridges  The Cartridge Header · MBCs → No MBC, MBC1–MBC7, MMM01, M161,
            HuC1, HuC-3, Other MBCs
Accessories Printer · Camera · 4-Player Adapter · Game Genie/Shark cheats
Other       Power-Up Sequence · Reducing Power Consumption · Accessing VRAM
            and OAM · OAM Corruption Bug · External Connectors · GBC
            Approval Process
Back        References (a bare list of external sources) · Single-page version
```

There is no glossary, no index and no appendix in the classic sense. The "Other" part does the job of appendices: it collects material that is looked up or that cuts across blocks (power-up state, a famous hardware bug, connector pinouts). The "Summary" page under I/O Ports is the register summary appendix, placed at the front of the part instead of the back.

The nesting is shallow but deliberate: a block's ordinary documentation sits on one page and its edge-case behaviour on a child page ("Timer and Divider Registers" then "Timer Obscure Behaviour"; "Rendering" then "Pixel FIFO"; "Audio Registers" then "Audio Details").

## How one block or topic is laid out: walk through one specific chapter section by section

The timer is the cleanest example: one parent page of about 70 lines and one child page of about 90.

**Parent: Timer and Divider Registers.**

1. *Scope box.* Opens with a tip-style box that rules out a likely confusion: this timer is not the cartridge real-time clock, which lives elsewhere (linked).
2. *One heading per register, in address order.* Each heading follows a fixed house pattern: address, an em dash, mnemonic, colon, plain description, and a bracketed access note where it applies. So the four sections read like `FF04 — DIV: Divider register` through `FF07 — TAC: Timer control`.
3. *Register prose.* For the byte-wide counters there is no bit diagram, only a few sentences: the rate, what a write does, what resets it, how model and speed mode change it. A dangling research question is left in the text in parentheses, marked TODO.
4. *Worked arithmetic inside the prose.* The modulo register gets a small numerical example (set it to one value and the interrupt fires every tick; one less and it fires every second tick).
5. *Bit field register.* The control register gets a generated bit diagram (8 columns, bit 7 on the left, named fields spanning cells), then a bulleted list with each field name in bold followed by a description.
6. *Model comparison table.* The clock-select values are given in an HTML table with a two-row header so that frequencies for three model groups sit side by side, including approximate values (with a tilde) for the Super Game Boy, whose clock differs.
7. *Closing pointer.* One sentence warns that writing the control register can bump the counter, linking to the child page.

**Child: Timer Obscure Behaviour.**

1. *Concept box* naming the hidden "system counter" of which the divider register is only the visible part.
2. *Circuit diagrams* (inline SVG, separate ones for monochrome and Color models) of how the counter bits feed a multiplexer and edge detector.
3. *Consequences as a bullet list*: each odd behaviour stated as a cause and an effect, with one concrete numeric case. One bullet admits that the outcome differs between individual consoles.
4. *Cycle table.* A small table with cycles across the top and register values down the side, with the two critical cycles labelled A and B and the changed values in bold.
5. *Numbered list of surprising interactions*, each tied to cycle A or B.
6. *Collapsible explanation.* A `<details>` block explains why each surprise happens in terms of the gates, with a footnote, so the reader who only needs the rule can skip the why.

This split, "what the registers do" then "why it behaves strangely", is the strongest structural idea in the book and maps directly on to our §N.4 and §N.8.

## Conventions: register tables, bit diagrams, timing diagrams, notes/warnings/cautions, code examples, cross-references, appendices, glossary, index, typography quirks. Say exactly how each is done.

**Register summary.** One Markdown table (Hardware_Reg_List.md) with columns Address, Name, Description, Readable/Writable, Models. Names are links to the register's section. Access is `R`, `W`, `R/W` or `Mixed`; "Mixed" defers detail to the field list. The Models column uses console codenames (`All`, `DMG`, `CGB`, `Mixed`).

**Register headings.** A codified pattern in CONTRIBUTING.md: `Address{–range} — {NAME {(When)}:} Description {[read-only]|[write-only]}`, with an en dash in ranges and an em dash after the address. Section titles are sentence case. "When" carries model or mode scope, for example "(CGB Mode only)". Because the heading carries address, name, scope and access, the heading alone is a usable reference card and also becomes the anchor.

**Bit diagrams.** Written with a custom `{{#bits width direction "Row name" hi-lo:"Field" ...}}` tag that a preprocessor turns into an HTML table: one column per bit, bit numbers in the header, a field spanning its bits, unused bits shaded and left blank. Several rows can be stacked (for example, one row per model or per mode). Below the diagram comes a mandatory bulleted list: bold field name, then usage notes in italic brackets (such as which mode it applies to), then access in italic parentheses, then a colon and the meaning. Literal values go in code style. If every field has the same access it is stated once instead. Field names in the diagram are kept short so the table fits a phone.

**Timing diagrams.** Mostly inline SVG, styled with the site's CSS variables so they follow the light and dark themes. Small cycle-by-cycle behaviour is shown as a Markdown table with time across the top. The contribution guide argues against ASCII diagrams on accessibility grounds (screen readers) and against raster images (do not scale).

**Notes and warnings.** A `:::kind TITLE ... :::` container with exactly three kinds: `tip`, `warning`, `danger`. The title is free text and is used as a label: NOTE, TERMINOLOGY, SCOPE, WORKAROUND, "Research needed", "TO BE VERIFIED", "DELAY", "LFSR lockup". About 60 boxes across roughly 9,000 lines of source; `danger` appears twice (one hardware-damage warning about switching the LCD off outside vertical blank, one "not legal advice" disclaimer). So the colour encodes severity and the title encodes subject.

**Uncertainty markers.** There is no formal scheme. In practice:
- a `warning` box titled "TO BE VERIFIED" or "To be verified" around a claim not yet checked on real hardware, usually with a link to the GitHub issue tracking it;
- a `tip` box titled "Research needed" with an explicit invitation to help;
- inline "TODO:" in parentheses, sometimes with the hardware it was measured on ("only measured on a CGB rev E");
- hedging words in prose: unknown, speculated, believed, likely, "not confirmed";
- links to test ROMs (mooneye-gb, MagenTests, and others) as the evidence for an edge case;
- explicit notes where emulators commonly get something wrong, or where emulators disagree about uninitialised RAM.

**Code examples.** Fenced, tagged with the project's own assembler dialect (`rgbasm`), which the renderer highlights. Short (under 20 lines), commented at the end of lines, often with cycle counts in the comments. A "Best practices" heading introduces them. CONTRIBUTING.md fixes a pseudocode notation (`:=` for assignment, true mathematical operators, floor and ceiling brackets).

**Cross-references.** Links whose target is `#` plus the literal section title, such as `(<#FF07 — TAC: Timer control>)`; the preprocessor resolves them to the right page and slug. Reference-style link definitions at the bottom of a page keep repeated register links tidy. Writers therefore link by title, never by number or file name.

**Footnotes.** Standard Markdown footnotes for qualifications that would clutter a field description (seen heavily in Audio Registers).

**Numbers and units.** Hex is upper case with a `$` prefix (`$FF40`); the prefix is dropped where context makes hex obvious, as in addresses in headings, but digits are then zero-padded. Binary is written out with the word "binary". KiB and MiB for memory; "bits" spelled out; a space before units; en dash for ranges. Fixed spellings for jargon: VBlank and HBlank, "tile map".

**Model naming.** Console codenames (DMG, MGB, SGB, SGB2, CGB, AGB) are mandatory, which keeps model scope short and unambiguous.

**Attribution.** A partial is included at the end of sections taken from another document (the "Dan Docs"), naming the original compiler and linking upstream.

**Glossary and index.** None. Terminology is defined in `tip` boxes titled TERMINOLOGY at first use, and the full-text search stands in for the index.

**Typography quirks.** `<var>` for symbolic values in tables and prose, `<kbd>` in the "Using this document" help, inline HTML tables wherever Markdown cannot span columns, smart punctuation switched on in the build, and a pixelated-image CSS class for historic diagrams.

## Voice: person, tense, sentence length, humour, how it handles uncertainty and danger. Describe this in your own words; quote at most a few words where essential, never whole sentences.

Mostly third person about the hardware ("the PPU", "the CPU"), shifting to the reader as "you" or to "the programmer" when giving advice. Present tense throughout. Sentences are short to medium; older sections still carry the hard line breaks and slightly stiff phrasing of the 1990s text file, while rewritten sections (audio, timer, interrupts) are tighter and more exact. Several writers' hands are visible and the tone varies by page.

Humour is rare and dry: the occasional "interesting" interaction, a parenthetical aside about a game that depends on a bug. It never jokes about the reader.

Uncertainty is handled in plain words rather than a system. When something is unknown it says so flatly ("purpose is unknown"), when something is guessed it says who might think so ("speculated", "believed"), and it often names the specific hardware revision on which a measurement was made. It is willing to say that behaviour differs from one console to the next. It openly corrects the folklore of emulators. Weaker material is fenced off in labelled boxes rather than hedged sentence by sentence.

Danger is flagged in a red box with a capitalised CAUTION title, states the physical consequence (a burnt-in line on the LCD), and adds that the platform holder reportedly rejected games that did it.

## Borrow: concrete practices for our manuals, each saying which of our manuals it suits (PP5022, BCM2722, WM8758 hardware references; firmware guide; games SDK guide; user guide; 5.5G hardware teardown)

1. **Heading as reference card.** A fixed register heading pattern carrying offset, name, model scope and access, e.g. `0x6000_5000 — TMR1_CFG: Timer 1 configuration`. Becomes the anchor and works in a search result. PP5022, BCM2722, WM8758.
2. **Ordinary page plus "obscure behaviour" page.** Keep §N.4 clean and send cycle-level oddities to §N.8 or a child section, with a collapsible "why" explanation. PP5022 (DMA, interrupts, timers), BCM2722 (link timing).
3. **Severity by box kind, subject by box title.** Our Note/Tip/Warning could keep its three kinds but allow a short subject title ("Warning: data partition"). All manuals.
4. **A "To be verified" box tied to a tracker issue.** For us this is the long form of `[hypothesis]`: a claim, the reason we believe it, and a tracker issue link. Every hardware reference, especially BCM2722 and the 5.5G teardown.
5. **Test programs as evidence.** Pan Docs links test ROMs that demonstrate an edge case. Our `[silicon]` tags should point to a named test in the repository that a reader can run. PP5022, WM8758, firmware guide (chapter on testing on hardware).
6. **Model-scope in headings and in a Models column.** Fixed codenames, like DMG/CGB, for 5G and 5.5G (and capacities where they matter). Answers our open question on a 5G/5.5G marker. PP5022, 5.5G teardown, user guide.
7. **Link by section title through a preprocessor.** Writers link to `<#TMR1_CFG>` style targets and the build resolves them, so links survive moving a section to another page. All manuals.
8. **Bit diagrams generated from a compact source syntax.** One description produces an accessible HTML table; field list format is enforced. A good fit for our renderer follow-up; keeps diagrams consistent across PP5022 and WM8758 (9-bit registers).
9. **A written house-style file for contributors** (number formats, model names, fixed spellings, heading pattern) separate from the content. Firmware guide "Contributing" chapter.
10. **Explicit "emulators get this wrong" notes.** We have the analogue: "Rockbox does X here; hardware does Y". Firmware guide, PP5022.
11. **Upstream attribution partial** at the end of any section derived from another body of work. Useful for `[rockbox-derived]` chapters.
12. **Scope box at the top of a chapter** that says what the chapter is not about and links to where that lives. Games SDK guide and user guide especially.

## Avoid: what dated badly or doesn't suit us

- **No numbering at all.** Fine for a website, poor for code comments, printed copies and our gotcha IDs. Keep our numbers.
- **Unresolved TODOs in published text**, and HTML comments with TODO notes in the source. Our §N.9 and Appendix G give these a home instead.
- **Uneven voice between pages.** Inherited text was never rewritten to match newer pages; we should edit to one voice.
- **Hex with a `$` prefix and bare hex in headings.** Our audience reads C; keep `0x`.
- **Uncertainty only in prose words.** "Likely" and "speculated" are not searchable or lintable; our tags are better.
- **Hand-written HTML tables** in Markdown sources (the specifications page) which are hard to diff and broke footnote links in the single-page build.
- **Generic section titles reused across pages** (each MBC page has the same "RAM Enable" heading); these get numeric suffixes in the single page and become ambiguous link targets.
- **No glossary or index**; search alone does not tell a reader where a term is defined.

## Distinctive details: 3–6 specific things that give it its character

1. The register heading formula (address, em dash, name, scope, colon, description, access) is enforced as house style, so a whole chapter reads like a printed register card.
2. "Obscure behaviour" child pages, with circuit-level diagrams and a collapsed explanation, show how far the community went past the official documentation.
3. Labelled boxes such as "TO BE VERIFIED" and "Research needed" with a link to an open issue: the document admits gaps and asks for help in the same place.
4. Test ROMs cited as proof for edge cases: evidence is something you can run.
5. Thirty years of history kept in the repository, with the 1995 text and its original PCX diagram shown on the History page.
6. Released into the public domain, edited by pull request, with an "edit this page" button on every page.

## As a single huge document

**Division.** Eight unnumbered parts made of about 90 Markdown files; mdBook renders each file as its own page and also emits `print.html`, all chapters concatenated with page-break hints for printing. A custom renderer derives `single.html` from it by rewriting every relative link (`Page.html#frag`) to a bare `#frag` and removing the automatic print trigger. Its source comments admit this is done with crude text matching rather than real HTML parsing.

**Anchors.** IDs are slugs of the heading text (`ff41--stat-lcd-status`), so a register's anchor is stable as long as its heading is. Writers link by heading title; the preprocessor resolves the title to file and slug at build time. That is why the same source survives both the split site and the single page. In the single page I counted about 800 IDs and about 1,490 internal links, with no duplicate IDs; but where headings repeat across files (the MBC pages each have their own "RAM Enable") mdBook appends `-1`, `-2`, `-3` to keep IDs unique, so those anchors depend on page order and change if a chapter is moved. The project's own guide notes that ambiguous title links resolve arbitrarily. Five links in the single page pointed at missing targets, all footnotes inside a hand-written HTML table. Lesson: unique, descriptive headings (ours would start with the register name) give anchors that survive splitting and merging.

**Navigation aids.** Every page, including `single.html`, has mdBook's collapsible sidebar table of contents, which stays on screen while the content scrolls, remembers whether it was open, and highlights the current chapter. On the split site there are next and previous arrows and arrow-key navigation. Search is client-side (press `s`) over a prebuilt index. There are no breadcrumbs and no back-to-top links; the sidebar is the only "where am I" cue, and in the single page it highlights the chapter, not the section, so on a long chapter you lose your place. A theme picker offers five colour schemes, and every page has a print button and an "edit on GitHub" link.

**Weight and load (measured 6 November 2026 with curl, no browser).** A normal chapter page is 26–28 KB of HTML. `single.html` is 712 KB (176 KB compressed) and fetched in about 0.24–0.49 s; `print.html` is the same size. Each page also pulls 8 scripts and 9 stylesheets, plus fonts, and the single page embeds 29 inline SVGs and 18 images. The search index is a separate 2.1 MB script (340 KB compressed), loaded once and cached, which is heavier than the single page itself.

**What works.** For a reader: one file to save, print or search with the browser's find; the sidebar makes the long page usable; SVG diagrams scale and follow the theme. For maintainers: one source file per topic gives small diffs and easy review, title-based links mean files can be moved freely, and both outputs come from one build.

**What works badly.** The single page is a by-product with a hack to rewrite links; its anchors for repeated headings are fragile; there are no section numbers to orient a reader scrolling through 700 KB; search needs a 2 MB index that is a separate download.

**Glossary and index across the volume.** Neither exists. Terms are defined in TERMINOLOGY boxes at the point of first use, the register summary page acts as an index of registers, and search covers the rest.
