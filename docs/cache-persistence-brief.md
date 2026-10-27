# Durable cache and library generations (2026-10-18)

## Owner pre-flight and acceptance

The owner explicitly chose continuous browsing, not merely safer deletion.
Keep the active library and artwork usable while preparing replacements.
Do not write, install, eject or unmount the iPod in this implementation round.
Do not erase caches, change artwork keys, or bump compatible artwork formats.
Success means warm reuse, local repair, recoverable publication, and no empty
library window during structural updates. Device results remain unverified.

## Contracts and ownership

- Root owns `artcache.c`, documentation, integration and the final gate.
- Artwork-format specialist owns `artfmt.[ch]` and artwork host tests:
  explicit I/O vs missing vs corrupt results; recoverable temp/previous
  publication primitives; bundle writes never truncate the published copy.
- Library specialist owns `tagcache.[ch]`, `library.[ch]`, library tests:
  explicit active vs candidate contexts, persistent generation selection,
  candidate RAM + matching precompute before publication, stable statistics.
- Budget reviewer is read-only. It reviews this brief before implementation
  and the complete diff afterwards. Shared test tooling requires root approval.

Artwork publication uses primary, `.prev`, and `.tmp` candidates, validates
new content before publication, checks every result, and retains a recoverable
valid prior copy. Index slot allocation protects both retained generations.
Reader errors never authorize a global wipe. Temporary decode/I/O/memory
failure must not replace a successful record with NOART/FAILED.
No physical deletion inside index merge callbacks or automatic cache eviction.
All new traces use UPDATE_TRACE (shell logf is not a persistent diagnostic).

Library structural commits target a separate generation directory. Two CRC
manifest slots choose the newest fully validated generation; legacy root
database is adopted without destructive migration. Active context (RAM,
headers, path, count, commit ID, prepared views) is published together.
Builder context must never be installed globally merely to run existing
helpers while UI/playback readers can yield. Runtime statistics survive
switches via stable track identity and a separately durable journal.
Errors keep the active snapshot; a failed RAM load never proves disk corruption.

## Device-reality checklist

1. Guard changed upstream behavior under HAVE_IPOD_UI; stock builds unchanged.
2. No active-RAM stealing. Separate bounded commit scratch; free it before
   candidate loading; no allocations that shrink playing/paused audio buffers.
   Candidate fit failures defer work with active data intact. Size 32/64 MB.
3. Bulk file copies/validation, bounded yields; no O(N) UI-thread recovery.
4. No JPEG decoding or directory walks in browse/draw callbacks. HDD and
   flash have identical behavior; only timing differs.
5. No display/bus/DMA changes. Existing partial-update UI remains.
6. Tagcache thread owns structural writes; UI reads active snapshot. Pin or
   reference old handles until readers finish; publish at a short boundary.
7. No new upstream splashes; all new user-visible text localized.
8. Test USB connect, shutdown, power cut and failed I/O at each write/flush/
   rename/publication stage. Never delete the last validated generation.
9. Exercise 50k tracks/5k albums and stopped completion on 32 MB. Keep memory
   costs in hardware budget; no new large permanent buffers or IRAM.
10. Every pending update/load/repair has an explicit retry state and visible
    status; coalesce equivalent requests; never report failed work as success.
11. Stock build and existing guard/byte-identity checks remain required.
12. Account for boot/USB scans, numeric tag writers, deletion passes,
    explicit rebuild, changelog import/export, open view and playlist readers.
13. Refresh only changed/missing records; retain old art on failed replacement.

## Test and release sequence

Add deterministic host fault injection for open/read/write/flush/rename,
allocation and decode failure. Cover missing/corrupt markers, one bad slot,
interrupted publication, prior-generation fallback and non-destructive retry.
Simulator: build + warm repeat; browse/play during update; USB interruption;
selection and statistics preservation; no ready-to-empty transition on update.
Run host art/library/USB tests, relevant simulator scripts, firmware builds,
budget end review, and one integrated project gate. Gate failures are fixed
with targeted reruns. Never claim device acceptance from simulator evidence.
Update handoff with exact completion/remaining work and validation provenance.
