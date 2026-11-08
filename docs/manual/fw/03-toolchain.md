---
title: "FW-3 Setting up the toolchain"
weight: 30
description: "The compilers and tools ProkBox needs, on macOS, on Linux and in a container."
---

ProkBox is built with GCC 16 in two forms: a cross-compiler for the player's ARM cores (`arm-none-eabi-gcc`), and a native compiler for the simulator and the host tests. The scripts expect the GNU versions of the usual command-line tools. This chapter covers three ways to get all of that.

| Route | Status in this edition | Best for |
|---|---|---|
| [macOS with Homebrew](#31-macos-with-homebrew) | In daily use by the project | A Mac |
| [Linux with Nix](#32-linux-with-nix) | Checked on Linux; macOS through Nix not yet tried | Any Linux machine |
| [A container](#33-a-container) | **Draft**: release builds use it; the steps below are not yet tested by a newcomer | Reproducing a release build exactly |

All three aim at the same compilers, so a build made by one should behave the same as a build made by another.

## 3.1 macOS with Homebrew

The project's own builds are made this way.

1. Install the compilers and the tools the simulator and the tests use:

   ```sh
   $ brew install arm-none-eabi-gcc gcc sdl2 ccache python3 ffmpeg
   ```

   `gcc` must be version 16; the native compiler is then `gcc-16`. `arm-none-eabi-gcc` must be version 16 too.
2. Check the versions:

   ```sh
   $ arm-none-eabi-gcc --version | head -1
   $ gcc-16 --version | head -1
   ```

3. The test fixtures need a few more tools, and the scripts that make them stop with a message naming a tool they cannot find (for example `ffmpeg`). Install what they name, then run them again ([FW-18](18-testing.md)).

> **Note.** `tools/configure` expects Rockbox's own cross-compiler, `arm-elf-eabi-gcc`, by default. With Homebrew's you add `--compiler-prefix=arm-none-eabi-` to every firmware and bootloader configure ([FW-4](04-building.md)).

> **Tip.** `ccache` is worth having: `tools/configure` finds it on your `PATH` and shares one cache, `$HOME/.cache/rockbox-ccache`, between every build directory, so a second worktree builds in a fraction of the time.

## 3.2 Linux with Nix

The project's Nix flake (`flake.nix` and `flake.lock` at the top of the repository) pins one environment with every build, test and gate tool in it: both compilers, `make` and `perl`, SDL2 and ALSA for the simulator and the host tests, `qemu` for the ARM instruction-count checks, Python with the imaging and numerical packages the tests use, the audio and video encoders the test fixtures need, and the GNU command-line tools, pinned so that BSD-versus-GNU flag differences never reach the scripts.

On Linux it has been checked with `arm-none-eabi-gcc` 16.2: the UI simulator, `warble` and the firmware build, and `scripts/test.sh` passes except for one known failure, `tagcache_shutdown`, which a GCC 16 warning trips and which is fixed after 0.8.0. That check ran on the development line. The scripts' Linux portability fixes arrive after 0.8.0, so on the 0.8.0 source a few more host tests may still fail on Linux; the builds are not affected.

1. Install Nix with flakes enabled, following the Nix project's own instructions for your distribution.
2. From the top of the repository:

   ```sh
   $ nix develop
   ```

   That opens a shell with everything on the `PATH`. To run one command inside it instead:

   ```sh
   $ nix develop -c scripts/test.sh
   ```

3. Build as in [FW-4](04-building.md), including `--compiler-prefix=arm-none-eabi-`. The shell also provides the native compiler under a versioned name, `gcc-16` today, as Homebrew names it on a Mac, because some scripts call it that way.

Three things the flake does that you should not undo:

- **It turns off Nix's compiler hardening** (`hardeningDisable = [ "all" ]`). Nix normally adds flags to every compile. One of them, stack-clash protection, cannot be compiled for the ARM7TDMI's Thumb instruction set, and another turns format warnings into errors in the simulator. With hardening off, both compilers see exactly the project's flags, as on a Mac.
- **It unsets the host tool variables** (`CC`, `OBJDUMP`, `NM` and the rest) that Nix's shell exports. The project's scripts read those as overrides, and one of them would otherwise run the host's `objdump` on the ARM firmware.
- **It leaves `ccache` to `tools/configure`**, which puts the cache in `~/.cache/rockbox-ccache`, as on a Mac.

The environment is pinned by `flake.lock`. Updating it (`nix flake update`) changes the toolchain, so it is done deliberately, in its own change.

> **Draft.** The flake also defines shells for macOS (Apple silicon and Intel), but nobody has used it on a Mac yet. On a Mac, use Homebrew (§3.1) for now.

## 3.3 A container

> **Draft.** These steps describe the release toolchain's container definition. The project's own built images are private, so you build the image yourself. Nobody outside the project has yet followed these steps end to end.

`ci/toolchain/Dockerfile` defines the environment every release zip is built in: a Debian base pinned by digest, host tools from a dated Debian snapshot, and GNU binutils 2.47 with GCC 16.2.0 for `arm-none-eabi`, built from the GNU tarballs and checked against their SHA-256 sums. It is configured to match Homebrew's `arm-none-eabi-gcc` 16, so code generation is the same as on a Mac.

1. Build the image (this compiles GCC, so expect it to take a while):

   ```sh
   $ docker build -t prokbox5g-toolchain ci/toolchain
   ```

   `podman build` works the same way.
2. Run a build inside it, with the repository mounted:

   ```sh
   $ docker run --rm -u "$(id -u):$(id -g)" -v "$PWD":/w -w /w prokbox5g-toolchain \
         sh -c 'mkdir -p build/fw && cd build/fw && \
                ../../tools/configure --target=22 --type=n --compiler-prefix=arm-none-eabi- </dev/null && \
                make -j"$(nproc)" && make zip'
   ```

The image builds the firmware and the bootloader. It does not include the simulator's SDL libraries or the gate's fixture tools, so use the Mac or the Nix route for day-to-day work and the gate. Changing anything in the Dockerfile changes the release toolchain, so it bumps `TOOLCHAIN_REV`.

`ci/docs/Dockerfile` is the equivalent for this documentation site.

## 3.4 The VideoCore compiler

The firmware for the BCM2722 video processor, in `bcmfw/`, needs a third compiler: `vc4-elf-gcc`, with a recompiled libgcc. None of the three routes above includes it yet. You need it only to change `bcmfw/` itself; `bcmfw/README.md` explains how to build it.
