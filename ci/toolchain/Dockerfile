# ProkBox5g release toolchain (docs/release-plan.md section 3.3).
#
# One pinned build environment for every release zip, on GitHub (staging
# and public) and on the owner's Linux host for the local side of
# scripts/release_compare.py:
#   - host tools (make, perl, zip, host gcc, python3) from a fixed Debian
#     snapshot on a base image pinned by digest;
#   - GNU binutils 2.47 and GCC 16.2.0 for arm-none-eabi, built from the
#     GNU tarballs (SHA-256 below; GNU signatures checked 2026-10-26),
#     configured like Homebrew's arm-none-eabi-gcc 16 that local Mac builds
#     use, so code generation stays the same.
# Changing anything here changes the toolchain: bump TOOLCHAIN_REV.
FROM debian:trixie-slim@sha256:62bd423aeee912639ca8ee681c2b762cb3e25137e0a92b20be455898a9f034ad AS build

ARG SNAPSHOT=20261020T000000Z
ARG BINUTILS=2.47
ARG BINUTILS_SHA256=154ab23b60070e8f27013c22977f1129425d67d1e8acd6e13010e617811e4cff
ARG GCC=16.2.0
ARG GCC_SHA256=e6738e29597f733270731aa90600f37ffdc045079dfc27ec7e8192cc81085c3e

RUN rm -f /etc/apt/sources.list.d/* \
 && echo "deb [check-valid-until=no] http://snapshot.debian.org/archive/debian/${SNAPSHOT} trixie main" > /etc/apt/sources.list \
 && apt-get -o Acquire::Retries=5 update \
 && apt-get install -y --no-install-recommends \
      build-essential ca-certificates curl xz-utils \
      libgmp-dev libmpfr-dev libmpc-dev zlib1g-dev libzstd-dev \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
RUN curl -fsSLO https://ftp.gnu.org/gnu/binutils/binutils-${BINUTILS}.tar.xz \
 && echo "${BINUTILS_SHA256}  binutils-${BINUTILS}.tar.xz" | sha256sum -c - \
 && curl -fsSLO https://ftp.gnu.org/gnu/gcc/gcc-${GCC}/gcc-${GCC}.tar.xz \
 && echo "${GCC_SHA256}  gcc-${GCC}.tar.xz" | sha256sum -c - \
 && tar xf binutils-${BINUTILS}.tar.xz && tar xf gcc-${GCC}.tar.xz

RUN mkdir b-binutils && cd b-binutils \
 && ../binutils-${BINUTILS}/configure --prefix=/opt/arm --target=arm-none-eabi \
      --disable-nls --disable-werror --enable-multilib \
 && make -j"$(nproc)" && make install-strip

RUN mkdir b-gcc && cd b-gcc \
 && PATH=/opt/arm/bin:$PATH ../gcc-${GCC}/configure --prefix=/opt/arm --target=arm-none-eabi \
      --disable-nls --without-headers \
      --with-as=/opt/arm/bin/arm-none-eabi-as --with-ld=/opt/arm/bin/arm-none-eabi-ld \
      --enable-languages=c,c++,lto --enable-lto \
      --enable-multilib --with-multilib-list=aprofile,rmprofile \
      --with-system-zlib --with-zstd \
 && PATH=/opt/arm/bin:$PATH make -j"$(nproc)" all-gcc all-target-libgcc \
 && PATH=/opt/arm/bin:$PATH make install-strip-gcc install-target-libgcc

FROM debian:trixie-slim@sha256:62bd423aeee912639ca8ee681c2b762cb3e25137e0a92b20be455898a9f034ad
ARG SNAPSHOT=20261020T000000Z
ARG TOOLCHAIN_REV=1
LABEL org.opencontainers.image.title="prokbox5g-toolchain" \
      org.opencontainers.image.description="ProkBox5g release toolchain: arm-none-eabi GCC 16.2.0, binutils 2.47, Debian snapshot ${SNAPSHOT}" \
      org.opencontainers.image.source="https://github.com/prokbox/prokbox5g" \
      org.opencontainers.image.licenses="GPL-3.0-or-later" \
      dev.prokbox.toolchain-rev="${TOOLCHAIN_REV}"
RUN rm -f /etc/apt/sources.list.d/* \
 && echo "deb [check-valid-until=no] http://snapshot.debian.org/archive/debian/${SNAPSHOT} trixie main" > /etc/apt/sources.list \
 && apt-get -o Acquire::Retries=5 update \
 && apt-get install -y --no-install-recommends \
      build-essential perl python3 zip unzip git ca-certificates \
      libgmp10 libmpfr6 libmpc3 zlib1g libzstd1 binutils \
 && rm -rf /var/lib/apt/lists/*
COPY --from=build /opt/arm /opt/arm
ENV PATH=/opt/arm/bin:$PATH
RUN arm-none-eabi-gcc --version | head -1 && arm-none-eabi-gcc -print-multi-lib | head -3
