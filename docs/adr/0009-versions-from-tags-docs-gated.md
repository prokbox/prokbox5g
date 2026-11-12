# ADR 0009: Versions come from git tags; docs and changelog are generated and gated

- Status: Accepted
- Date: 2026-11-11

## Context

Hand-typed version strings drifted (release notes left as drafts that the site skipped; a version baked into the site config). Release notes and the changelog were written by hand.

## Decision

The version is X.Y.Z at a release tag, read by `scripts/version.sh`; nothing else types it. `scripts/changelog.py` builds the changelog from commit subjects in the private history and fails closed on a scrub hit. `scripts/release_docs.py check` refuses a release whose notes, changelog section or fragment are missing, or whose notes are still drafts. The release import and export scripts run the same check.

## Consequences

- Every release needs `release_docs.py prepare` run in the private repo first.
- Commit subjects are public text, so they must be clean.

## Alternatives considered

- Hand-written changelog: drifts and cannot be checked.

## Owner decision

No quote in the docs or tracker. Recorded from the commits "build: the version comes from git tags (scripts/version.sh)" and "scripts: changelog from history, release_docs check" (2026-11-11).

## Links

- `scripts/version.sh`, `scripts/changelog.py`, `scripts/release_docs.py`, `docs/release-plan.md`
