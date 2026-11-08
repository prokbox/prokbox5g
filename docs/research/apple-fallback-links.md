# Apple-fallback "links": shared-cluster FAT entries (#137), host rehearsal

Research, 2026-11-05. Host only: every result below comes from scratch FAT32
images on the development Mac (macOS 26.6.2). No iPod and no mounted iPod
volume was touched. Tool: `tools/fatlink/fatlink.py`. Tests:
`tests/fatlink/run.sh` (`DRIVER=kext` for the legacy kernel driver).

## Summary (plain English)

Apple's firmware 1.3 only plays files inside `iPod_Control/Music` [silicon,
#137]. To let it play the user's own music without a second copy, ProkBox
can add a second directory entry (a "link") inside `iPod_Control/Music` that
points at the same data as the original file. FAT was never designed for
this, so we tested what goes wrong.

What we found:

- **Reading works.** The Mac reads the link and gets exactly the original's
  bytes. Free space is counted once.
- **If the user deletes or edits the original**, nothing is lost. The link
  goes stale, and ProkBox's cleanup either rebuilds it or removes it without
  freeing any space. All six edit cases we tried were handled.
- **If the user deletes the link itself**, the Mac frees the space the
  original is using. The original vanishes from Finder straight away. During
  the same session the Mac does not reuse that space, so ProkBox can put the
  original back. **After the next mount, the very first file the Mac writes
  lands on that space and the original is overwritten for good.** The same
  happens in the same session if the disk is nearly full.
- **Locking the link stops it being deleted.** We tested a link marked
  read-only (shown as "locked" on the Mac) and hidden: `rm`, `rm -f`,
  `rm -rf` on its folder and moving it to the Trash all failed. Finder's delete blocked
  (most likely at its locked-item prompt). Only a deliberate `chflags nouchg` (unlock) got
  through.
- **Disk checkers do damage.** The Mac's disk checker sees a link as an
  error. When it repairs, it **empties the original** (0 bytes) and keeps the
  data under the link's name. The Mac runs this repair **by itself** when the
  disk was unplugged without ejecting. ProkBox can undo it afterwards, because
  the data is still there.
- **The best fix is to make links disappear whenever a computer is
  connected.** ProkBox removes every link before it shows the disk to the
  computer, and puts them back after it is unplugged. The computer then never
  sees a link. It cannot delete one, and its disk checker has nothing to cut.
  We rehearsed this "park" cycle with an unclean (dirty) mount, two sessions,
  edits and deletions, and lost nothing.

**Recommendation: go, with conditions.** Use links only with the park cycle,
behind a developer flag at first. Two cheap checks on real hardware come
first (see "Gates"). **The risk that remains:** links are only exposed while
the iPod is connected under *Apple's* firmware (its disk mode), or if ProkBox
stops running for good. In that state, unplugging without ejecting makes the
Mac empty the user's original files on the next connect. The music still
plays on the iPod, and ProkBox repairs it on its next start. But if ProkBox
never starts again, those originals stay empty, and their music survives only
under hidden names in `iPod_Control`. Deliberately unlocking and deleting a
link, then connecting again, loses that one song.

## Setup

| Item | Value |
|---|---|
| Image | 2 GiB sparse raw file (`mkfile -n`), FAT32, `newfs_msdos` |
| Cluster | 16 KiB, as on the owner's volume |
| BPB sector | `fskit` runs: 512 B (32 sectors/cluster). `kext` runs: 2,048 B (8/cluster), the owner's layout |
| Mount | `fskit`: `diskutil mount` (DiskArbitration, the FSKit msdos driver that a plugged-in iPod gets; the mount shows `fskit`). `kext`: `mount -t msdos` (legacy kernel driver) |
| Why two | DiskArbitration refused to mount the 2,048-byte-sector layout from a 512-byte image device. The legacy driver accepted it. The real iPod has a 2,048-byte logical block over USB, so the FSKit path should apply there too ([hypothesis]) |
| Test file | 200,000 B of random data = 13 clusters. Link at `/iPod_Control/Music/PKB/01 Song One.mp3` (LFN + 8.3); attributes RH (read-only + hidden) unless noted |
| Checks | after each scenario: `fatlink verify` / `cleanup`, `fsck_msdos -n`, and SHA-1 of the original and the link read through the FAT on the unmounted image |

Raw logs: `build/fatlink/results-{fskit,kext}.log` (not committed;
re-run with `tests/fatlink/run.sh`).

## Results

### H1: the host deletes the original

| Scenario | Host result | Cleanup | fsck -n after | Data |
|---|---|---|---|---|
| `rm` original, then write one 200 KB file | new file at clusters 46+13 (original was 10+13): no reuse in session | ORPHAN → link entry removed raw, FAT untouched | clean | new file intact |
| `rm` original, then 4 new files | 46+7, 54+7, 62+7, 70+7: no reuse | ORPHAN, removed | clean | intact |
| `mv` original to another folder | — | ORPHAN, link removed (the library sync relinks the new path) | clean | original intact |

The rule that matters: **the cleanup must never free a link's clusters**, only
mark its directory slots deleted (`0xE5`). If the Mac had reused those clusters
for a new file (it does after a remount, see H3), a normal delete of the
link would free the new file's clusters.

### H2: the host edits the original

| Scenario | First cluster / size | Verify | Cleanup | Result |
|---|---|---|---|---|
| append 50 KB | same / grew | STALE | link rebuilt | link = original |
| truncate to 60 KB | same / shrank | STALE | rebuilt | link = original |
| `cp` over the original (`O_TRUNC`) | 10 → 45 / same | STALE | rebuilt | link = original |
| tag editor style: temp file renamed over | 10 → 46 | STALE | rebuilt | link = original |
| rewrite 120 bytes in place | unchanged | OK | none | link sees the new bytes (shared data) |
| `xattr -w` on the original | unchanged (Mac adds `._` files) | OK | none | unchanged |

In every STALE case the old link entry was removed raw. Its old clusters
belonged to the host (freed or reused), so nothing was freed twice.

### H3: the host deletes the link (the dangerous case)

| Scenario (FSKit unless noted) | Delete succeeded? | Original during the session | Freed clusters reused? | Cleanup | Original after |
|---|---|---|---|---|---|
| plain link, `rm` | yes | **disappears from the listing** (FSKit validates the chain; access fails) | — | FREED → chain rebuilt from the table's extents, link recreated | intact |
| plain, `rm`, then write 200 KB | yes | gone | no (46+13) | FREED → repaired | intact |
| plain, `rm`, then write 3 MB | yes | gone | no (46+184) | repaired | intact |
| plain, `rm`, no repair | yes | gone | — | (left) | `fsck -n`: "starts with free cluster … should at most be 16384" |
| plain, `rm`, **eject, mount again**, write 200 KB | yes | gone | **yes: 11+12 (original was 10+13)** | DAMAGED: link removed, logged | **overwritten** |
| plain, volume filled to ~1 MB free, `rm`, write 1.2 MB | yes | gone | **yes: wrapped onto 10+13** | DAMAGED | **overwritten** |
| RH link, `rm` | **no**: "Operation not permitted" | readable | — | OK | intact |
| RH, `rm -f` | **no** | readable | — | OK | intact |
| RHS, `rm -rf iPod_Control` | **no** for the link; other files removed | readable | — | OK | intact |
| RH, `mv` to `.Trashes/502` (what Finder does) | **no** | readable | — | OK | intact |
| RH, Finder "Move to Trash" via AppleScript | **no**: the Finder call blocked (timed out at 20 s; a locked-item prompt is inferred, not seen) | readable | — | OK | intact |
| plain link, Finder "Move to Trash" | yes, moved to `.Trashes/502` (a rename, nothing freed) | readable | — | LINK_GONE: alias found in `.Trashes`, removed raw, link recreated | intact (the Trash can no longer free it) |
| RH, `chflags nouchg` then `rm` | yes | gone | no in session | FREED → repaired | intact |
| legacy kernel driver (2,048-byte sectors), plain `rm` + write | yes | **still listed, wrong bytes** | no | repaired | intact |
| legacy driver, `rm`, remount, write | yes | — | **no** (43+13: it keeps its next-free hint) | repaired | intact |

**The data-loss window, precisely.** Loss starts when the host deletes the
link: the original's clusters become free. Bytes are destroyed only when the
host allocates those clusters again. The FSKit driver allocates **forward from
where it last allocated** within a mount, and **from the lowest free cluster
after a fresh mount**. So:

1. Within the session that deleted the link, the clusters are safe unless the
   allocator wraps (volume nearly full).
2. At the **first write of the next mount**, they are reused. The original is
   lost unless ProkBox's cleanup ran in between.
3. ProkBox can repair at any point before step 2. It reads the extents from its
   own table, finds them all still free, and rewrites the FAT chain. It cannot
   repair after step 2. It detects the damage (the chain is broken or diverges
   from the recorded extents), removes the link and logs it.

**Mitigations measured:**

| Mitigation | Closes | Leaves open |
|---|---|---|
| Read-only attribute (Mac: `uchg`, "locked") | `rm`, `rm -f`, `rm -rf`, `mv`, Finder Trash (prompt) | deliberate unlock; Linux (`vfat` allows unlink in a writable dir, [expected, not measured]); Windows Explorer prompts but allows ([expected]) |
| Hidden attribute + hidden folder | Finder never shows the link (`hidden` flag measured) | Terminal, Cmd-Shift-., other OSes |
| System attribute | nothing extra seen on macOS | — |
| Cleanup before any write (boot, disconnect) with extent repair | every same-session H3 | a second mount before ProkBox runs; full volume |
| **Park: remove links before USB, recreate after** | H3 and H4 during **every ProkBox USB session** (the host never sees a link; `fsck -n` clean while parked) | sessions under Apple's firmware disk mode or if ProkBox never runs again |

### H4: disk checkers

| Case | What happens |
|---|---|
| `fsck_msdos -n` on a linked volume | two warnings per link: "*original* starts with cross-linked cluster (N) Truncate? no", "Cluster chain starting at N is cross-linked at cluster N", plus "size of *original* is 200000, should at most be 0". It names the **original**, not the link |
| `fsck_msdos -y` (own image) | "Truncate? yes": the **original** is set to first cluster 0, size 0. The link keeps the chain and all the data. The same with the link at `/iPod_Control/Music/PKB/` or at `/AAA/` (placement does not change which one survives) |
| `fsck_msdos -y`, then ProkBox cleanup | EMPTIED: original's first cluster and size restored from the link, data intact |
| Clean volume mounted by DiskArbitration | no repair (the cross-link survives the mount) |
| **Dirty volume** (FAT[1] clean bit cleared, as after an unplug without eject) mounted by DiskArbitration | **the Mac repaired it automatically during mount**: the original was 0 bytes in that session, the link intact. ProkBox's cleanup restored it afterwards |
| Volume parked (no links), dirty, mounted | nothing to repair; no damage |

The Mac clears the dirty bit on a clean unmount ("clean" after every eject in
these runs). Windows `chkdsk` is not tested. Its handling of cross-links
differs by version (old versions duplicate the data, newer ones truncate),
[not measured].

## Recommended design

1. **Where links live:** one folder, `iPod_Control/Music/PKB/`, hidden. Each
   link is **read-only + hidden** with a short 8.3 name like the ones the sync
   app uses (`ABCD.MP3`). There are no long names, which keeps them cheap: 512
   links per 16 KiB directory cluster. Put a `database.ignore` in the folder so
   the library scan never indexes a song twice (`apps/tagcache.c`
   `check_ignore`). [Gate A] confirms Apple 1.3 plays from a non-`Fxx`
   folder and from a read-only + hidden file. If it does not, use a single
   `Fxx` name such as `F49`, which the sync app does not use for a library
   this size.
2. **Table:** in ProkBox's hidden folder. One record per link: link name,
   original's path (or library id), first cluster, size, extents (start+count
   runs; usually one), plus a volume fingerprint (FSInfo free count and
   next-free, root-directory first-cluster hash, FAT[1]) taken when the links
   were made.
3. **Park on USB connect:** before the storage LUN is exposed, mark every link
   slot deleted. The cheapest way is to zero the `PKB` directory clusters (the
   directory chain itself stays allocated). The FAT is never touched.
4. **Unpark on USB disconnect:** after the library scan, and after the
   iTunesDB writer has its track list, recreate each link from the original's
   **current** directory entry. Do this only if the original's chain is healthy
   (ends in EOC, has exactly ceil(size / cluster) clusters, no free entry).
   Then record the new extents and fingerprint. Originals that are gone are
   dropped. Their links were already gone, so nothing needs freeing.
5. **Boot check (before ProkBox's first write):** compare the fingerprint. If
   it is unchanged, nobody else touched the disk, so skip the check. If it
   changed, the disk was mounted while links were live (Apple disk mode, or a
   checker). Then run the verify rule below on every record before any write
   that could allocate. **Exact verify rule** for a record `{link, orig, fst,
   size, extents}`:
   - original missing → ORPHAN: raw-remove the link if its entry still has `fst`;
   - original has `fst = 0, size = 0`, link has `fst/size`, and the link's chain
     equals `extents` → EMPTIED: restore the original's `fst/size`;
   - original's `fst/size` differ from the record, and its chain is healthy →
     STALE: raw-remove the link and recreate it;
   - `fst/size` match, but the chain is not healthy: if every extent cluster is
     free → FREED: rewrite the chain from `extents` and recreate the link;
     otherwise → DAMAGED: raw-remove the link and log it (no splash, rule 1);
   - chain healthy but different from `extents` → STALE;
   - link entry missing, chain intact → LINK_GONE: find the alias by first
     cluster (whole-tree walk, rare), raw-remove it, recreate the link;
   - otherwise OK.
6. **ProkBox's own FAT code must treat links as special:**
   - never `remove()` a link (a normal delete frees the original's clusters):
     only raw slot removal;
   - when ProkBox deletes or rewrites an original (e.g. a future
     delete-from-device), raw-remove its link in the same operation;
   - never run a free-space or orphan "fix" that frees cross-linked clusters;
   - skip `PKB` in every directory walk that could act on files.
7. **No user-facing surface** (#137: developer flag only, no settings, errors
   logged).

## What the firmware implementation would cost

| Part | Size / time (estimate) |
|---|---|
| `firmware/common/fatlink.c`: raw slot writer (8.3 only), chain check, extent repair, park/unpark, verify | ~500 lines C, ~4–6 KB code; needs `fat.c` internals, so it lives beside it |
| RAM | one 16 KiB cluster buffer during park/unpark/verify (`core_alloc`, released after); table streamed; no resident cost |
| Table | ~16–24 B per record, binary (path held by the library id): 10,000 songs ≈ 200 KB |
| Park | zero ~20 directory clusters for 10,000 links: milliseconds |
| Unpark | ~20 directory cluster writes + one FAT read per original (contiguous files: one FAT sector each): ~10,000 sector reads, roughly 1–2 s on flash and more on a hard drive. It runs after USB, alongside the library scan |
| Boot | fingerprint compare (a few sectors); a full verify only when it changed |
| Hooks | USB connect (before LUN exposure), USB disconnect (after scan), boot (before first allocating write), `fat.c` remove/rename guard |
| Host tests | `tests/fatlink` stays as the rehearsal and gains an oracle mode for the C module (as `tests/fatcrash` does for `fat.c`) |

## Gates before building it

- **Gate A [silicon]:** with one test link, does Apple fw 1.3 play a file in
  `iPod_Control/Music/PKB/` with an 8.3 name and RH attributes? Still open:
  #137 tested only `F00` copies.
- **Gate B [silicon/host]:** under Apple firmware 1.3 disk mode, does the Mac
  mount it (FSKit), and does the dirty-bit repair behave as on the image?
  And does a Finder/iTunes sync delete unknown files in `iPod_Control/Music`?
  That would be H3 in bulk, a no-go for users who sync.

## Go / no-go

**Go, with conditions:** park cycle mandatory, developer flag first, after
Gates A and B. Without parking: **no-go**. An unplug without eject leaves
every original empty during the next Mac session, and one remount after a
deleted link destroys data.

**The risk that remains, stated plainly:** while links are live (the iPod is
running Apple's firmware, or ProkBox is gone for good):

- unplugging without ejecting makes the next Mac mount empty every linked
  original. The music stays playable on the iPod, ProkBox repairs it on its
  next boot, and without ProkBox the originals stay at 0 bytes;
- a user who unlocks and deletes a hidden link loses that original at the next
  mount's first write;
- Windows and Linux behaviour was not measured.

Under ProkBox itself, connected to a computer, there is no risk we could
find: the computer never sees a link.

## Implementation (2026-11-05, branch `p19-applefallback`)

Built behind a developer switch (`docs/research/itunes-mode.md` §10). Where it differs from the recommendation above:
- **The park drops the whole `PKB` folder** (`rmdir_shallow()`: its entry, then its own clusters; never a chain a link names) instead of zeroing its clusters. A constant cost (one directory entry, the folder's few FAT entries) whatever the number of links, and the parked volume is plain FAT: `fsck_msdos -n` clean (`tests/applefb`). The unpark makes the folder again.
- **The boot check trusts the library's boot stamp** (its digest over the music tree matched, and no clusters were freed since our clean shutdown) instead of an FSInfo fingerprint of its own: ProkBox's own writes after the unpark move FSInfo, so that fingerprint would differ on nearly every boot.
- **`remove()` of a read-only + hidden file inside the link folder drops its entry only** (every IPOD_UI build; the path must start `/iPod_Control/Music/PKB/`, any case, checked before any probe): a stray chain there is leaked, never freed under a sharing entry. Everywhere else `remove()` is upstream's, read-only + hidden or not (`tests/applefb`: an RH file outside the folder and a plain file inside both free their chains).
- **Not built:** `LINK_GONE`'s whole-tree alias hunt (logged and counted only; with the park, an alias can only come from Apple's disk mode and an unlocked link).
- Coverage: `tests/applefb` runs the link steps on the firmware's own `fat.c` (links read the original, `remove()` of a link, the park, FREED, EMPTIED, a short chain refused) with `fsck_msdos -n` as the oracle; this rehearsal (`tests/fatlink`) stays the Mac-side half.
