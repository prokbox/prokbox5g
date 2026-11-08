---
title: "FW-4 Building"
weight: 40
description: "Building the simulator, the firmware and the bootloader, and putting a build on a player."
---

Every ProkBox build starts the same way: make a directory under `build/`, run `tools/configure` in it, then `make`. The arguments to `configure` decide what you get. This chapter gives the four builds you will use, then says how to get a firmware build onto a player.

You need a toolchain from [FW-3](03-toolchain.md).

## 4.1 Three rules for every build

1. **Build directories live under `build/`**, which git ignores. One directory per kind of build, so they never disturb each other.
2. **Run `configure` with its input from `/dev/null`** (`</dev/null`). Given a full set of arguments it asks nothing, but if an argument is missing it would otherwise stop and wait for an answer you cannot see in a script.
3. **Add `--compiler-prefix=arm-none-eabi-` to every firmware and bootloader build.** `configure` otherwise looks for Rockbox's `arm-elf-eabi-gcc`, which none of our toolchains provides.

Target 22 is the iPod Video (5G and 5.5G). Add `--ipodui` to get the ProkBox shell, which is what ships. Without it you get stock Rockbox's interface, which must keep compiling too: the gate builds both, so that our changes can always be compared with upstream.

## 4.2 The simulator

The simulator runs the firmware as a desktop program, with the screen in a window and the keyboard standing in for the click wheel. Start here: it needs no player, and it is what the UI tests drive.

```sh
$ mkdir -p build/sim-ui && cd build/sim-ui
$ ../../tools/configure --target=22 --type=s --ipodui </dev/null
$ make -j8
$ ./rockboxui
```

The keys are listed in `tests/ui/README.md`: Escape is Menu, Return is the centre button, the Up and Down arrows turn the wheel. Use as many `make` jobs as your machine has cores to spare; `-j8` is only an example.

The gate's own simulator build is in `build/sim-ui`, so building there means the gate can reuse it.

> **Note.** In zsh, which is the default shell on a Mac, variables are not split into words. If you keep `configure`'s arguments in a variable, use an array, or the whole string arrives as one argument.

## 4.3 The firmware

```sh
$ mkdir -p build/fw && cd build/fw
$ ../../tools/configure --target=22 --type=n --ipodui --compiler-prefix=arm-none-eabi- </dev/null
$ make -j8 && make zip
```

`make zip` packs everything a player needs into `rockbox.zip`. Its contents unpack onto the player's disk.

The firmware's size has budgets, recorded in `docs/hardware.md`: a ceiling on the image file and a backstop on its resident size in memory. The gate checks them on the builds it makes; `scripts/validate.sh` says which limit applies to which build.

## 4.4 The bootloader and the host tests

The bootloader is the small program that starts ProkBox:

```sh
$ mkdir -p build/boot && cd build/boot
$ ../../tools/configure --target=22 --type=b --compiler-prefix=arm-none-eabi- </dev/null
$ make -j8
```

It produces `bootloader-ipodvideo.ipod`. You rarely need to rebuild it, and installing one is a bigger step than installing the firmware (§4.5).

The host tests for the codecs and metadata parsers run as a native program called `warble`:

```sh
$ mkdir -p build/warble && cd build/warble
$ ../../tools/configure --target=22 --type=w </dev/null
$ make -j8
```

You seldom run it by hand: `scripts/test.sh` and the gate do ([FW-18](18-testing.md)).

| Build | `--type` | Directory | Output |
|---|---|---|---|
| Simulator with the ProkBox shell | `s` | `build/sim-ui` | `rockboxui` |
| Firmware | `n` | `build/fw` | `rockbox.zip` |
| Bootloader | `b` | `build/boot` | `bootloader-ipodvideo.ipod` |
| Host codec and metadata tests | `w` | `build/warble` | `warble` |

## 4.5 Putting a build on a player

> **Warning: back up first.** A firmware build that has not passed the gate can fail to start, and a mistake while installing the bootloader can leave the player unable to start until it is restored. Before installing anything, copy the whole of the player's disk to your computer and check that the copy is complete.

To install the firmware, follow the [user guide's installation steps](../../user/guide/getting-started.md) with your own `rockbox.zip` in place of a release zip. That guide also explains how to [go back to Apple's firmware](../../user/guide/going-back.md), which is the way out of most mistakes.

Installing a bootloader you built yourself uses `ipodpatcher`, the install tool, from `utils/ipodpatcher`. The release zips' installation steps show how it is used. Do this only when you have changed the bootloader.

## 4.6 The VideoCore image

The video processor runs our own firmware from `bcmfw/`, which has its own build (`bcmfw/build.py`) and its own compiler ([§FW-3.4](03-toolchain.md)). You do not need to rebuild it to change anything outside `bcmfw/`.
