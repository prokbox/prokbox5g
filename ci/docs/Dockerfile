# ProkBox5g docs toolchain: builds the site, its search index and the PDF
# user guide (scripts/docs_build.sh; docs/release-plan.md section 3.4).
# Same image in CI (pages.yml, release.yml) and on the owner's Linux host,
# so the site and the PDF come out the same everywhere.
#   - Chromium and fonts from a fixed Debian snapshot, digest-pinned base;
#   - Hugo extended 0.168.0 and Pagefind 1.3.0 release tarballs, SHA-256
#     pinned (checked against each project's published checksums,
#     2026-10-26).
# Changing anything here changes the docs build: bump DOCS_REV.
FROM debian:trixie-slim@sha256:62bd423aeee912639ca8ee681c2b762cb3e25137e0a92b20be455898a9f034ad

ARG SNAPSHOT=20261020T000000Z
ARG HUGO=0.168.0
ARG HUGO_SHA256=787f100057328824f38c6ad7c87ff10d6451ae0a7bc39cedaad0dd2bcde17e53
ARG PAGEFIND=1.3.0
ARG PAGEFIND_SHA256=f983dec5507d87c7c497c450d2c5de97fc967a46d26e0ac3fb7184a87c94e434
ARG DOCS_REV=2

LABEL org.opencontainers.image.title="prokbox5g-docs" \
      org.opencontainers.image.description="ProkBox5g docs toolchain: Hugo ${HUGO}, Pagefind ${PAGEFIND}, Chromium (Debian snapshot ${SNAPSHOT})" \
      org.opencontainers.image.source="https://github.com/prokbox/prokbox5g" \
      dev.prokbox.docs-rev="${DOCS_REV}"

RUN rm -f /etc/apt/sources.list.d/* \
 && echo "deb [check-valid-until=no] http://snapshot.debian.org/archive/debian/${SNAPSHOT} trixie main" > /etc/apt/sources.list \
 && apt-get -o Acquire::Retries=5 update \
 && apt-get install -y --no-install-recommends \
      chromium fonts-dejavu-core fonts-liberation2 fonts-noto-core \
      python3 ca-certificates curl git poppler-utils \
 && rm -rf /var/lib/apt/lists/*

RUN cd /tmp \
 && curl -fsSLO https://github.com/gohugoio/hugo/releases/download/v${HUGO}/hugo_extended_${HUGO}_linux-amd64.tar.gz \
 && echo "${HUGO_SHA256}  hugo_extended_${HUGO}_linux-amd64.tar.gz" | sha256sum -c - \
 && tar xzf hugo_extended_${HUGO}_linux-amd64.tar.gz -C /usr/local/bin hugo \
 && curl -fsSLO https://github.com/CloudCannon/pagefind/releases/download/v${PAGEFIND}/pagefind_extended-v${PAGEFIND}-x86_64-unknown-linux-musl.tar.gz \
 && echo "${PAGEFIND_SHA256}  pagefind_extended-v${PAGEFIND}-x86_64-unknown-linux-musl.tar.gz" | sha256sum -c - \
 && tar xzf pagefind_extended-v${PAGEFIND}-x86_64-unknown-linux-musl.tar.gz -C /usr/local/bin \
 && mv /usr/local/bin/pagefind_extended /usr/local/bin/pagefind \
 && rm -f /tmp/*.tar.gz \
 && hugo version && pagefind --version && chromium --version

ENV CHROME=chromium PAGEFIND=pagefind
