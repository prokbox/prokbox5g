# The Be Book (BeOS API reference, originally Be Inc.; Haiku's DocBook/XHTML legacy edition, revision 1.0 of 2008-03-09, distributed by the Haiku project with ACCESS Co.'s permission under CC BY-NC-ND 3.0; https://www.haiku-os.org/legacy-docs/bebook/)

Source used: the HTML edition at the URL above, fetched one page at a time with a pause between requests. 40 pages plus the stylesheet are held locally in `refs/bebook/`: the front page, legal notice, introduction, the three part-index pages, the appendix pages (About This Book, Revision History, Class Index, Function Index), nearly all of the **Game Kit** (kit introduction; the BDirectWindow, BGameSound, BPushGameSound and BWindowScreen overviews; every class reference page; global functions, constants, defined types, and graphics card hooks in both halves; I skipped the three short remaining class overviews), and a slice of the **Application Kit** (kit introduction, the Messaging topic chapter, BApplication, BLooper and BHandler in both halves). I did not fetch the Constant and Type indexes, the Special Topics chapters or the other twelve kits; their place in the structure is known from the front page.

Note on edition: this is not the original Be Inc. typesetting. Haiku's 2007–2008 conversion to DocBook reorganised parts of it (the revision history records turning lists into tables, adding a mini contents box to each class page, moving all hook functions into the hook section, moving constants and types from kit level to class level). Some of what follows describes the Haiku edition's apparatus rather than Be's.

## Structure

Front page and the three parts, condensed. Every page has Prev, Up and Next links at top and bottom and a breadcrumb title ("The Be Book – Classes And Methods – The Game Kit").

```
Legal Notice
Introduction            Welcome · What the book is · Acknowledgements · Organisation (three parts)
                        · Typographic conventions
Part 1  System Overview     One chapter per kit, each: kit Introduction page, then topic pages
                            (e.g. Messaging, Scripting) and one Overview page per class
          Kits, in alphabetical order: Application · Device · Game · Input Server · Interface
          · Kernel · Mail · Media · Midi · Network · OpenGL · Storage · Support · Translation
Part 2  Classes And Methods Same kits, same order. Per kit: one reference page per class, then
                            kit-level pages for Global Functions, Constants, Defined Types and
                            any special families (e.g. message constants, graphics card hooks)
Part 3  Special Topics      Device Drivers · The Deskbar · The Keyboard · The Tracker
Appendices                  About This Book · Revision History · Class Index · Function Index
                            · Constant Index · Type Index
```

The book is in effect two parallel books (narrative and reference) cross-linked class by class. The part-two introduction page explains what each kind of class section means (constructor, destructor, hook functions, member functions, global variables, archive fields, scripting suites) before any class is shown. The System Overview introduction tells the reader directly which two kits matter most.

Game Kit as an example of one kit's shape:

```
Overview half           Introduction · BDirectWindow · BFileGameSound · BGameSound · BPushGameSound
                        · BSimpleGameSound · BStreamingGameSound · BWindowScreen
                        · Graphics Card Hook Functions
Reference half          the same seven classes · Global Functions · Graphics Card Hook Functions
                        · Constants · Defined Types
```

## How one block or topic is laid out: the Game Kit, page by page

**Kit introduction (overview half).** About a screen. It says what the kit offers in two headline categories (direct graphics access, low-latency sound), mentions its single global function, then gives each category a heading and a short paragraph per class saying what that class is *for* and how it differs from its siblings. It ends the sound section with a frank warning against using one class at all. There are no signatures here; it is a map of the kit.

**Class overview page (BGameSound overview).** Narrative, with headed sections that answer the questions a newcomer asks in the order they ask them:
1. One paragraph: what the class is and that you never use it directly.
2. A light, scene-setting heading and paragraph about why game sound matters.
3. *Choosing a player class*: a short decision guide in bullet form, one bullet per subclass, each tied to a concrete case (short frequent effects, long music loops, pushing your own buffers). It ends by saying which two classes cover most games.
4. *Polyphony*: a real problem (two explosions at once), a small helper class in C++ that solves it, then a paragraph explaining how the helper behaves at its limit. A Note follows about the memory cost of the approach in the current release.
5. *Performance verification*: three named hardware configurations the developer should test on, with the caveat that the list is not exhaustive.

**The larger class overview (BDirectWindow overview).** Longer and more tutorial-like: what it does and how it differs from its sibling, Note and Warning boxes up front about threading, then a section on the central callback with the full C structure it receives and a two-column field table whose cells explain each state value. It finishes with a worked example built up piece by piece: the class declaration, a field table explaining each member variable, then the constructor, the callback and the drawing thread in turn, each followed by commentary.

**Class reference page (BGameSound).** The fixed apparatus:
1. Class name as title.
2. A small header table: *Derived From*, *Mix-in Classes*, *Declared In* (the header path), *Library* (the shared library), *Allocation* (for example "Constructor only", or a dash). Absent values are an en dash, never blank.
3. A mini contents box linking to: Class Overview (the narrative page), Constructor and Destructor, Hook Functions, Member Functions, Static Functions, Constants, Defined Types, Archived Fields, Scripting Suites and Properties, as applicable. Section names are fixed across all classes.
4. **Constructor and Destructor**, one entry each.
5. **Hook Functions** (BLooper, BHandler): functions you override and never call; each says so.
6. **Member Functions**, alphabetical. Related functions are grouped under one entry whose heading lists them all ("SetGain(), Gain()"; "Lock(), LockWithTimeout(), Unlock()"; five debug accessors together).
7. **Static Functions**, **Constants**, **Archived Fields** (a Field / Type code / Description table), **Scripting Suites and Properties** (per property, a Message / Specifiers / Reply Type table and one sentence).

**One function entry.** Heading is the name with empty parentheses (or the group of names). Then the C++ declaration(s) exactly as in the header, with defaults shown and a `protected` marker where it applies. Then prose: what it does, with parameter names set in italics inside the sentences rather than in a separate parameter list; units are stated in the prose (microseconds, 0.0 to 1.0, pinned range limits). Then, where relevant, a two-column **Return Code / Description** table that lists named status codes and often ends with an "other errors" row. Then optional Note or Warning boxes, sub-headed sections for anything with its own vocabulary (BLooper's constructor has sub-sections for priority levels and port capacity, each with its own constant table), a short code fragment when the usage pattern matters, and a "See also:" line of links at the end.

**Kit-level reference pages.** *Global Functions*: per function, a "Declared in:" line, the signature, then prose. *Constants*: per group, "Declared in:", a code block listing the names, a sentence, then a Constant / Description table. *Defined Types*: per type, "Declared in:", the typedef or struct as code, a sentence or two, and "See also:". *Graphics Card Hook Functions*: a separate overview page explains the mechanism (an indexed table of function pointers handed over by the driver, which indices form which groups, which must be implemented together) before the reference page lists each hook.

**For comparison, a topic chapter: Application Kit, Messaging.** Some kits carry cross-class topic chapters in the overview half that are not tied to any one class. Messaging is the longest page I fetched, and its shape is worth recording because our firmware guide needs the same kind of chapter (the event loop, inter-core mailboxes):

- It opens by naming the four classes that make up the system and giving each a short section on its role in the whole, before any one of them is explained in depth.
- It then follows a single message through the system as a story: how the loop picks a handler (a numbered three-step decision list, each step phrased as a yes/no question), how the loop picks a function (generic receive function versus dedicated hooks for system events), and how inheritance forms a chain of handlers.
- Then the sending side (post versus send, how replies come back), the receiving side, and finally the refinements: handler associations, message filters, message protocols.
- Notes in this chapter mostly tell the reader which details are implementation detail and can safely be ignored.
- Headings are plain questions or noun phrases ("Finding a Handler", "Sending a Message"), so the contents list reads as the sequence of questions a newcomer has.

The order (cast of characters, one journey through the system, then the refinements) is a useful model for any chapter whose subject is a flow rather than a single object.

## Conventions

**Typography (declared in the Introduction).** Each kind of identifier has its own style: class names, method names with trailing `()`, function names, parameters (italic), types, constants, variables, key names, key combinations joined with `+`, and file names. In the HTML these are DocBook semantic classes (`methodname`, `parameter`, `type`, `constant`, `filename`), so the styling is applied by the stylesheet, not by hand.

**Signatures.** Shown verbatim as C++ declarations with default arguments, one block per overload; parameters keep the header's names (including Be's `in`/`out`/`io` prefixes in the Game Kit, which document direction for free).

**Return codes.** A two-column table, codes in constant style. This is the only consistently tabular part of a function entry. Some tables include an unspecific "other errors, as determined by the subclass" row.

**Admonitions.** Boxed, with an icon and a one-word title. In my sample of about 30 pages: Note (18), Warning (8), Caution (1), Important (1). Notes carry version caveats and memory or performance consequences. Warnings carry crashes, deadlocks and invalid results (a server that kills an application whose callback takes more than three seconds; a function that returns garbage if called from the wrong place). The one Caution (calling Run twice drops you into the debugger) and the one Important (do not delete a message you do not own) are no more severe than the Warnings, so the four levels are not used with consistent meanings.

**Code examples.** Short C++ fragments in fixed-width blocks: a skeleton override with a `switch` and `...` for elided cases; a small helper class; an incremental worked example spread over a page. Elision is with bare `...`. Code is not visibly compiled: the polyphony helper declares a member with the wrong pointer type for how it is used.

**Tables.** Used for header facts, constants, return codes, struct fields, archive fields and scripting properties. No numbered captions; the column headings carry the meaning.

**Cross-references.** All hyperlinks, written as the target's name (a class, `Class::Function()`, or a quoted topic title). "See also:" lines close many entries and every type and constant. Overview and reference halves link to each other at the top of each class page. There are no section numbers anywhere.

**Indexes.** Four separate alphabetical indexes: classes, functions, constants and types. Each has an A–Z jump bar. The class index gives each class two links, *Overview* and *Class Methods*. There is no subject index and no glossary.

**Version-dependence.** Handled inline with phrases tying a statement to the release ("currently", "in the current version", "for the current release"), sometimes in a Note box. Unfinished parameters are documented honestly (a device argument that must at present always be NULL).

**Quirks and errors.** The book is lightly edited and it shows: a misspelt "Appendicies" used as an actual page title; "namepsace" and "proteced"; doubled full stops; a getter documented under a heading in the plural but declared in the singular; a function described as restarting a sound that is already playing while its return-code table says it fails in that case; a gain attribute described in decibels next to a setter documented as 0.0 to 1.0. These are useful as a list of what an API reference must not do.

## Voice

- **Person.** Second person throughout ("you"), with a corporate first-person plural that appears when the writers have an opinion or want to confide ("we beg you"; "we should mention"; a note admitting the book has done nothing to correct a convenient misunderstanding). The reader is a programmer and a peer.
- **Tense.** Present tense, contractions everywhere (don't, can't, it's, you'll). Imperatives for rules ("be sure to", "never").
- **Sentence length.** Short to medium in reference entries, usually one idea per sentence; longer and more conversational in overviews, with dashes for asides and frequent rhetorical questions that are then answered ("which handler does it go to? Here's the path").
- **Humour.** Real and frequent in the overviews, absent from most reference entries. It is mostly one-line asides: an arcade coin joke about using the Game Kit outside games, magic powers needed for a class the reader should leave alone, a cheerful heading parodying a film line, the hook-function idea summarised as a familiar brush-off phrase. The jokes never obscure the fact being stated; they decorate it.
- **Uncertainty.** Stated plainly and tied to the current release rather than hedged; the book prefers "currently X" to "X may not be supported". It tells the reader when something is an implementation detail they can ignore.
- **Danger.** Concrete consequence first: the application will be killed, you will land in the debugger, you will crash on close, results will be invalid. Then the remedy in one sentence. The threat is never vague.
- **Opinion.** The book recommends freely and ranks options ("most games will need only these two classes"; "always use the handler's versions if you can").

## Borrow

1. **Two halves per subsystem: narrative overview and reference, cross-linked.** Each SDK module gets an overview page (what it's for, how to choose between its parts, a worked example) and a reference page (header table, entries), linked both ways at the top. *Games SDK guide (main model); firmware guide for driver APIs.*
2. **A fixed header block on every reference page.** Adapt the five fields for us: *Header*, *Link with* (library or object), *Allocation/ownership*, *Context* (may it be called from an interrupt, the COP, the audio callback?), *Since* (SDK version), *Evidence* where behaviour depends on hardware facts. Use an en dash for none. *GS; FW.*
3. **Fixed section names in a fixed order on every reference page** (constructor/init, callbacks you implement, functions you call, constants, types), with a contents box at the top. Readers navigate by habit. *GS Appendix A; FW API appendix.*
4. **A separate "callbacks you implement" section, distinct from "functions you call".** The Be Book's hook-function idea maps directly onto our game loop: update, draw, audio-fill and input callbacks. Each entry says when it is called, on which core or context, under which lock, and what it must not do. *GS.*
5. **Group related functions in one entry** (setter with getter, lock with unlock with try-lock). Saves repetition and puts the pairing rule in one place. Our §5.5 entry format should allow a group heading. *GS; FW.*
6. **Return-code tables** with every named status code and no "other errors" catch-all; if the set is open, say which codes are reserved for future use. *GS; FW.*
7. **Units and ranges stated in the description**, including what happens out of range (clamped, rejected with which code). *GS.*
8. **A "choosing between" section** at the head of any module with several alternatives (Be's choice of sound player class). For us: which sound API for short effects versus streamed music; which blit path for sprites versus full-frame redraws. *GS; user guide for choosing install methods.*
9. **Kit-level pages for global functions, constants and defined types**, each entry stating the header it lives in. *GS; FW.*
10. **Separate indexes by kind** (functions, types, constants) as well as our general subject index; cheap to generate from the API source. *GS; FW.*
11. **"Currently…" statements tied to a version**, in our case paired with an evidence tag, for anything that may change (an argument that must be zero today; a pool size limit). *GS; FW.*
12. **A light, rationed touch of humour in overviews and none in reference entries.** One aside per overview page at most, never in a Warning. Our draft's "colleague at the next bench" voice fits this. *GS; user guide; FW teaching chapters. Not the hardware references.*
13. **Concrete-consequence warnings** (what crashes, after how long, and the one-line fix), like the three-second watchdog warning. *GS; FW.*
14. **Prev / Up / Next navigation and a breadcrumb title** on every rendered page. *All manuals when rendered to HTML.*
15. **A worked example built in stages across an overview page**, with a table explaining the example's own member variables before the code that uses them. *GS (a minimal game), FW (a minimal driver).*

## Avoid

- **No section numbers.** Fine for hyperlinked HTML, poor for print, citations in issues and talking about the book. Keep our `§GS-5.3` numbering and use the Be Book's naming in link text.
- **Contradictions between an entry's prose and its own return table, or between overview and reference.** Generate signatures and code tables from the SDK headers, and test that every documented status code can actually be returned.
- **Untested example code.** Every example in GS and FW must be compiled in CI, as our draft already requires ("runs as written").
- **Parameters only in prose.** Fine for one argument, hard to scan for four. Our §5.5 parameter list is better; keep the Be Book's habit of stating units and ranges, but in that list.
- **Ownership buried in prose** (who deletes the message, who frees the list). Make ownership an explicit line in every entry that passes or returns a pointer.
- **Admonition levels without defined meanings.** Four box types with overlapping severity; we keep our three, defined.
- **Bare `...` for elided code**; our draft's `/* ... */` is safer to copy.
- **Hardware-specific testing advice that dates** (named sound cards of the period). Put test matrices in a dated appendix or the firmware wiki, not in API overviews; for us, name hardware revisions (5G, 5.5G, 30/60/80 GB) instead of accessories.
- **No glossary and no subject index.** API indexes alone do not help someone looking up a concept such as "polyphony" or "locking".
- **Spelling slips in titles.** Run a spell-checker over headings at least.

## Distinctive details

1. **The two-book split.** Every class exists twice, once as an essay and once as an entry, and the class index links to both. It suits readers who learn and readers who look up equally well.
2. **The class header table** (derived from, mix-ins, header, library, allocation), five facts that answer "how do I get one of these and where does it live" before any function is shown.
3. **Hook functions as a named category.** Splitting "what you override" from "what you call" is the Be Book's clearest structural idea and maps directly onto event-driven game code.
4. **Grouped entries** with several related function names in one heading.
5. **Jokes in the margins of a serious reference**: the coin-slot aside, the class that needs magic powers, the film-parody heading. Each comes in passing, after the fact has been stated, and is followed straight away by plain instruction.
6. **Frank, version-stamped candour** about what does not work yet, and blunt descriptions of what will happen if you get it wrong.
