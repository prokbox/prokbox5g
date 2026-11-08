#***************************************************************************
#               __________                   __    __________
#    Open       \______   \_______    ____  |  | __\______   \  ____  ___  ___
#    Source      |     ___/\_  __ \  /  _ \ |  |/ / |    |  _/ /  _ \ \  \/  /
#    Player      |    |     |  | \/ (  <_> )|    <  |    |   \(  <_> ) >    <
#    Firmware    |____|     |__|     \____/ |__|_ \ |______  / \____/ /__/\_ \
#                                                \/        \/               \/
#
# Custom firmware for the iPod Video, 5th generation (5G and 5.5G).
# Not affiliated with or endorsed by Apple Inc.
#
# Copyright (C) 2026 prokopto-dev
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
# KIND, either express or implied.
#
#***************************************************************************
# ProkBox5g developer environment (#162): one pinned toolchain for the Mac,
# Linux hosts and a Linux build host's container.
#
#   nix develop                 a shell with every build, test and gate tool
#   nix develop -c <command>    one command inside it (e.g. scripts/test.sh)
#
# Pinned by flake.lock (nix flake update bumps it, deliberately). Known good:
# arm-none-eabi-gcc 16 (the Mac's Homebrew toolchain; gcc 15 needs
# laptop/gcc15-always-inline for fw-ui-video), host gcc 16. Nothing here may
# assume a host path (no /opt/homebrew, no /usr/lib64): a Linux build host
# runs this flake as-is inside a container. Not yet in it: vc4-elf-gcc (the
# VideoCore compiler for bcmfw/build.py; a pinned prebuilt, #162).
{
  description = "ProkBox5g: build, test and gate environment";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" "aarch64-darwin" "x86_64-darwin" ];
      forAll = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in {
      devShells = forAll (pkgs:
        let
          arm = pkgs.pkgsCross.arm-embedded.buildPackages;   # arm-none-eabi-gcc, binutils
          py = pkgs.python3.withPackages (p: [ p.pillow p.numpy ]);
          # tests and scripts call the host compiler as gcc-NN (the Mac's
          # Homebrew names it so): the same name for this gcc's major
          gccMajor = pkgs.lib.versions.major pkgs.gcc.version;
          gccNN = pkgs.writeShellScriptBin "gcc-${gccMajor}" ''exec gcc "$@"'';
        in {
          default = pkgs.mkShell {
            packages = [
              # build: firmware, bootloader, simulator, warble
              arm.gcc arm.binutils
              pkgs.gcc pkgs.gnumake pkgs.perl pkgs.zip pkgs.unzip
              pkgs.pkg-config pkgs.SDL2 pkgs.alsa-lib pkgs.ccache gccNN
              # host tests (libubsan/libasan ship with gcc here)
              py pkgs.gdb
              # the ARM instruction-count and perf checks (tests/resample/
              # arm.sh, scripts/risk/arm_insn_count.sh, tools/eqfit/perf):
              # qemu and its plugin header (scripts/qemu_plugin.sh)
              pkgs.qemu
              # fixtures and the gate: audio/video encoders, image tools,
              # FAT images, hashing, the usual GNU userland (pinned too, so
              # BSD-vs-GNU flag differences never reach the scripts)
              pkgs.ffmpeg-full pkgs.libjpeg_turbo pkgs.imagemagick
              pkgs.flac pkgs.lame pkgs.vorbis-tools pkgs.opus-tools
              pkgs.mtools pkgs.dosfstools pkgs.xxd pkgs.bc pkgs.file
              pkgs.coreutils pkgs.findutils pkgs.gnused pkgs.gnugrep
              pkgs.gawk pkgs.gnutar pkgs.gzip pkgs.which pkgs.git
            ];
            # Nix's cc-wrapper adds hardening flags (-Werror=format-security,
            # _FORTIFY_SOURCE's warn_unused_result, -fstack-clash-protection,
            # which the ARM7TDMI's Thumb-1 cannot even compile): off, so both
            # compilers see exactly the project's flags, as on the Mac and Fedora
            hardeningDisable = [ "all" ];
            # tools/configure expects the arm-none-eabi- prefix
            # (--compiler-prefix=arm-none-eabi-, as on the Mac)
            # mkShell exports the HOST toolchain's names (CC=gcc,
            # OBJDUMP=objdump, NM=nm, ...). A distro or macOS shell sets
            # none, and the project's scripts read them as overrides:
            # febe_reach_check.py ran x86 objdump on the ARM firmware (its
            # call graph came back empty). Unset, so the scripts pick
            # arm-none-eabi-* or the host gcc themselves, as elsewhere.
            shellHook = ''
              unset CC CXX AR AS LD NM OBJCOPY OBJDUMP RANLIB READELF SIZE STRINGS STRIP
              export PROKBOX_ENV=nix
            '';
          };
        });
    };
}
