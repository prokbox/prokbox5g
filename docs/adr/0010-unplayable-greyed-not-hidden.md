# ADR 0010: Unplayable files are shown greyed out, never hidden

- Status: Accepted
- Date: 2026-11-10

## Context

A 32-bit FLAC (#285) or a DRM-protected purchase synced by iTunes was listed but silently failed; an all-unplayable album just stopped playback (#281).

## Decision

Index every file we can recognise. A metadata parser marks a file unplayable, with a reason, at index time from its headers; lists draw it greyed with a sign, without opening the file. One shared record and one visual treatment cover Music and, later, Videos and Photos.

## Consequences

- A new field (planned as the last member of `struct mp3entry`) and a tagcache upgrade path.
- Design options are rendered at real size for the owner before building.

## Alternatives considered

- Hide unplayable files: the library looks incomplete and the cause is invisible.
- Show an error on selection: against "errors are logged, not shown".

## Owner decision

"songs the iPod can't play stay in the library, but show greyed out with a clear sign that they can't play. Selecting one doesn't start playback, and a queue skips them." (#286, 2026-11-10)

## Links

- #286, #285, #281, #225
