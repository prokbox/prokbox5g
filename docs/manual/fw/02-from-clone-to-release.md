---
title: "FW-2 From clone to release"
weight: 20
description: "The whole process in one chapter, and a map of the repository."
---

This chapter names every stage between getting the source and a change reaching a release, in one sentence each, and points to the chapter that covers it. It ends with a map of the repository. Read it first; then read only the chapters you need.

## 2.1 The stages

1. **Get the source.** Clone `https://github.com/prokbox/prokbox5g`. The repository carries Rockbox's full history underneath our own, so the first clone is large.
2. **Set up a toolchain.** On a Mac, with Homebrew. On Linux, with the project's Nix environment. Or in a container. See [FW-3](03-toolchain.md).
3. **Build.** The simulator first, because it needs no player and starts in seconds. Then the firmware and the bootloader for the real device. See [FW-4](04-building.md).
4. **Change something, and look at it.** Most changes are in the user interface shell, `apps/ipodui/`. A scripted run of the simulator takes screenshots, so you can compare before and after. See [§FW-18.2](18-testing.md).
5. **Pass the gate.** `scripts/validate.sh` builds every configuration, runs the host tests and every UI script, checks sizes and strings, and prints `GATE PASSED` or says what failed. See [FW-18](18-testing.md).
6. **Record what you changed.** Every edit to a file that came from Rockbox gets a line in `docs/changes-from-rockbox.md`. Every hardware finding goes into its hardware manual with an evidence tag. See [FW-20](20-contributing.md).
7. **Contribute it.** See [FW-20](20-contributing.md).
8. **Release.** A release is built from a tag by the project's release workflow. Before anyone else gets it, the exact build is installed on a player and checked. You do not need to do anything for this stage. It is listed so that you know a merged change waits for the next release.

> **Note.** Steps 3 to 5 never touch a player. Everything up to a passing gate can be done on a computer alone. Installing on a device is described in [§FW-4.5](04-building.md), and it is always your choice.

## 2.2 A map of the repository

ProkBox5g started as a copy of Rockbox and still is one, underneath. Most of the tree is upstream Rockbox, kept so that upstream fixes can be merged. Our own work is concentrated in a few places.

**Where our work lives**

| Path | What it is |
|---|---|
| `apps/ipodui/` | The user interface shell, written for this player. It is compiled only when the build is configured with `--ipodui`. Most new code goes here |
| `apps/audiobook.c`, `apps/audiobook_core.c` | Audiobook chapter navigation: the screen, and the pure logic under it |
| `bcmfw/` | Our own firmware for the BCM2722 video processor, written from scratch. It has its own build and its own README |
| `firmware/target/arm/ipod/video/` | The device drivers for this player. Part upstream, part ours; `docs/changes-from-rockbox.md` says which |
| `scripts/` | The test harness, the gate, the screenshot tool, release tooling and the documentation build |
| `tests/` | Host tests, simulator UI scripts (`tests/ui/*.script`) and their expected results |
| `tools/` | `configure` (where every build starts), and our reverse-engineering and image tools |
| `ci/` | The container definitions used to build releases and this documentation |
| `site/` | The documentation website's layouts (its content is in `docs/user/` and `docs/manual/`) |
| `docs/` | Everything written: see below |

**Where Rockbox's work lives**

| Path | What it is |
|---|---|
| `firmware/` | The kernel, drivers for every target Rockbox supports, the file system |
| `apps/` (outside the paths above) | Playback, the database, settings, plugins |
| `lib/` | Codecs and support libraries, each with its own licence |
| `uisimulator/` | The simulator, which runs the firmware as a desktop program |
| `bootloader/` | Bootloaders for every target |
| `utils/` | Host tools, including `ipodpatcher`, the installer |
| `manual/` | Rockbox's own LaTeX manual. It does not describe ProkBox |
| `android/`, `packaging/`, `backdrops/`, `wps/`, `icons/`, `fonts/` | Other ports and Rockbox's themes. Kept for upstream merges |

The public tree that GitHub carries is stripped of upstream code that no ProkBox build compiles, so some of these directories are smaller there than in Rockbox.

**Where the documents are**

| Path | What it is |
|---|---|
| `docs/manual/` | This reference |
| `docs/user/` | The user guide |
| `docs/pp5022-reference/`, `docs/bcm2722-reference/`, `docs/wm8758-reference/` | The hardware manuals, Parts II to IV of this reference, still in their source-tree form |
| `docs/data-paths.md` | Which part of the player can move bytes where, what works on real hardware, and the dead ends |
| `docs/hardware.md` | The hardware limits and budgets every change must respect |
| `docs/design/` | Design records, including `spec/` (what each screen does) and `changes-from-7g.md` (every deliberate difference from Apple's last iPod software) |
| `docs/changes-from-rockbox.md` | Every edit we made to an upstream file, and why |
| `docs/legal.md` | Licences, trademarks, and how each obligation is met |
| `docs/research/` | Working notes behind the hardware manuals |

## 2.3 What to read before you change anything

In this order:

1. `docs/hardware.md`, because every change must fit its budgets. The firmware's resident size has a budget that the gate checks, and the build sits close to it.
2. `docs/data-paths.md`, before any work on DMA, memory, the second core, the video processor or a bus.
3. The hardware manual for whatever you are touching, starting with its `quirks.md`: the list of things that do not behave as you would expect.
4. `docs/design/spec/index.md`, before changing a screen.
5. [FW-20](20-contributing.md), before you open a pull request.
