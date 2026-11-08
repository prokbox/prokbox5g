---
title: "FW-20 Contributing"
weight: 200
description: "How to send a change to ProkBox5g, and the licence, naming and evidence rules every contribution follows."
---

This chapter tells you how to work with the project from outside: where to send a change, what to read first, and the rules a change must meet before it is accepted. Most of the rules are legal or about naming, and scripts check several of them, so follow them from the start.

## 20.1 How to contribute

The public repository is `github.com/prokbox/prokbox5g`. Issues and pull requests there are welcome. For questions that do not fit the issue tracker, and for trademark or takedown requests, write to **prokbox.project@gmail.com**. This is the one public project address; it is never anyone's personal address.

**The public repository is a curated mirror.** Development happens on the project's own development line, and the public repository is updated only at release tags, with a scrubbed copy of the tree. A pull request is therefore not merged on GitHub in the usual way. It is reviewed there, then integrated into the development line, and your change appears in the public history with the next release that contains it. Open an issue first for anything large, so that the design can be agreed before you write it.

> **Draft.** The project plans to require a sign-off line on each commit (the Developer Certificate of Origin). Until that is confirmed, a sign-off is welcome but not required.

Rockbox's own contribution policy (`docs/CONTRIBUTING`) still applies to the code style (see 20.3). Its review route does not: you send ProkBox5g changes to ProkBox5g, not to Rockbox. A change that would also help Rockbox is recorded in `docs/upstream-backports.md`, and the project sends those upstream itself, after the firmware is stable.

## 20.2 Before you start

Read, in this order:

1. `README.md`: what the project is, the credits and the not-affiliated statement.
2. [FW-2](02-from-clone-to-release.md) to [FW-4](04-building.md) of this reference: the process, the layout of the tree, and how to build.
3. `CLAUDE.md`: the project's working rules in full.
4. `docs/process.md`: how work is briefed, reviewed and gated, including the device-reality checklist and the lean coding rules.
5. `docs/hardware.md` and `docs/design/spec/index.md`: the platform limits and the per-screen design.
6. `docs/legal.md`, before you add any file, image, font or name.

## 20.3 Code

**Where new code goes.** The new user interface lives in `apps/ipodui/`, which is compiled only when `HAVE_IPOD_UI` is set (`tools/configure --ipodui`). Chapter logic is split between `apps/audiobook.c` (interface) and `apps/audiobook_core.c` (pure logic). `lib/rbcodec` must never include anything from `apps/`. Everything else began as upstream Rockbox.

**Style.** Match the surrounding Rockbox C: C99, four-space indent, no tabs, lower-case `snake_case` identifiers, upper-case macros and enum constants, `/* */` comments only, lines under 80 columns, LF line endings. In an existing file, follow that file. Commits are small and topical, with a subsystem prefix such as `ipodui: …` or `metadata: …`.

**The file header.** Every file you add carries the ProkBox header from `docs/templates/prokbox-header.txt`. Generate it, do not type it:

```
python3 scripts/prokbox_header.py FILE
python3 scripts/prokbox_header.py --dry FILE
```

The year is the year the file was first added. The header has no email address and no Apple names. Python, shell and make files use the `#` variant, with the shebang line first. Documentation and data files get no code header. A file adapted from Rockbox code keeps every original copyright line and adds one line after yours: `Portions derived from Rockbox (rockbox.org).`

**Editing upstream files.** A file you only edit keeps its own header untouched. Add one line inside it, as the GPL requires for modified files: `Modified for ProkBox5g (2026–<year>); see docs/changes-from-rockbox.md.` (`scripts/mark_modified.py` does this). Record every edit to an upstream file, and what changed, in `docs/changes-from-rockbox.md`. Never remove or alter a copyright, licence or warranty notice.

**Stock builds.** Keep upstream edits small and guarded, so that a stock build, without `--ipodui`, still compiles and behaves as before. Where a design for this hardware is clearly better than the upstream one, you may diverge, but measure before and after, keep the stock 5G and 5.5G configuration working, and record the change in `docs/changes-from-rockbox.md`.

**Every user-visible string** goes through the language system (`LANG_*` IDs in `apps/lang/`). Do not hard-code interface text.

## 20.4 Tests and the gate

A change is not finished until it passes the gate. Run it through `scripts/gate-run`; add tests and, for screens, scripts with goldens. [FW-18](18-testing.md) describes the gate, the simulator scripts and how goldens are captured. Anything the simulator cannot show goes on the device checklist in `docs/ROADMAP.md`.

## 20.5 Documentation and hardware findings

Hardware facts are only useful if the reader knows how far to trust them, so every fact carries an evidence tag:

| Tag | Meaning |
|---|---|
| `[silicon]` | Measured on real hardware with our own code. |
| `[datasheet]` | Stated in a vendor datasheet. |
| `[apple-RE]` | Read from Apple's code, statically. It says what that code does, not what the hardware is. |
| `[rockbox-derived]` | Taken from the Rockbox source or its tracker. |
| `[external]` | Another public source, cited by name. |
| `[hypothesis]` | Your inference, not verified. |

Three markers are not evidence but a kind of fact. `[ours]` marks a convention of our own firmware, which another design could replace. `[sim]` marks a result from a simulator only. `[estimate]` marks a figure computed from measurements.

The two hardware manuals, `docs/pp5022-reference/` and `docs/bcm2722-reference/`, explain their current tags in their `index.md` files. Some older entries there use `[rockbox]`, `[apple]` or `[measured]`; read each as the base class it names, and do not rewrite them. Two rules matter more than the vocabulary. Never upgrade a tag without new evidence. When a measurement on silicon contradicts an earlier reading, state the silicon result and keep the withdrawn reading in the manual's `quirks.md`, so nobody re-derives it.

Record every device finding with its tag, in the manual that owns the chip, in the same change as the code that depends on it. Anything that bites goes into that manual's `quirks.md`. A finding about a hardware block also goes into `docs/hardware-features.md`.

## 20.6 Legal

**Licences.** Code is GNU GPL v2 or later (`docs/COPYING`). Third-party code and fonts keep their own licences, in `docs/LICENSES`, under `lib/` and in `apps/ipodui/fonts/`. Documentation and site prose are CC BY-SA 4.0. By sending a change you agree that it is released on those terms. Do not paste in code under a licence that is not compatible with the GPL.

**Trademarks and the not-affiliated rule.** ProkBox5g is not Apple software and not an Apple product, and nothing you write may suggest otherwise. Apple's marks (iPod, iTunes, Click Wheel, Cover Flow, Apple and the like) are used only to name the hardware the firmware runs on. They never appear in a product, feature, repo, file, asset or release name, and on-screen text never calls the device "iPod". Cover Flow ships as *Album Flow*. Use the disclaimer texts in `docs/legal.md` verbatim, and change them only there. Avoid "official", "theme", "skin" and "fair use" in anything public.

**Your own words.** Short labels may match the original ones ("Now Playing"), but write every full sentence yourself. Never copy Apple's wording.

**What never goes in the repository.**

- Apple's firmware, and any bytes, fonts, icons, logos or artwork taken from it. Reference material used during development stays outside the repository.
- Owner media: no copyrighted content or private data from anyone's device, including art caches, database files and library names (artists, titles, albums). Use only synthetic or openly licensed test media, and give test items synthetic names. `scripts/check_no_owner_media.py` guards commits.
- Any image, font or binary that is neither generated by a script in the tree nor listed with its source and licence in `docs/legal.md` §5.

> **Note.** The project answers a trademark or trade-dress complaint by renaming, redrawing or removing what it names, not by arguing. Anything in a change that invites one will be asked to change.

## 20.7 Naming and style

**Spelling.** Use UK spelling in on-screen text and in documentation: colour, licence (noun), behaviour, catalogue.

**One font.** The interface uses one curated typeface everywhere, ShellSans (TeX Gyre Heros), and gives users no font choice. The only exception is subtitles: Videos › Settings offers a proportional or monospaced face and a size (`docs/design/spec/videos.md` §1 and §4.4; decided 2026-11-06). Do not add another font setting, and never add a font without its licence and an entry in `docs/legal.md`.

**Chip names.** Write them the same way every time:

| Write | Meaning |
|---|---|
| PP5022 | The system-on-chip. Say "PP5021" only for the early units that carry it. |
| CPU and COP | The PP5022's two cores. |
| BCM2722 | The video processor. |
| WM8758 | The audio codec. |

**"iPod".** Use it only to name the hardware ("for the iPod Video"). Say "5G" and "5.5G" for the two revisions, and "ProkBox5g" for this build line and "ProkBox" for the brand. The default device title on screen is ProkBox.

**Documentation form.** Menu paths use ›, and button names are in bold.
