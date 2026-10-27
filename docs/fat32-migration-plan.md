# FAT32 larger-cluster migration plan

Status: planning only, revised 2026-10-18. No device layout, backup-host files, or
firmware were changed by this investigation. Execution requires the owner's
separate go after the disk identity, geometry, backup and restore gates below
are satisfied. Worktree: `.claude/worktrees/s12`, branch `usbfix`.

## Decision and expected benefit

Stay with FAT32. **32 KiB clusters are the default target.** Evaluate 64 KiB
on disposable images as an alternative, but do not choose it for the real disk
unless Apple firmware, installed bootloader, ProkBox and native host access
have all been validated for that geometry. Do not change filesystem family,
install a bootloader, or repartition as part of this migration. Apple normal
boot is preferred; the owner accepts disk-mode recovery as the minimum, but
there is no reason to sacrifice either without a demonstrated benefit.

| Cluster size | Approximate single FAT for this volume | Transfer-only time at 21–22 MiB/s |
|---|---|---|
| 16 KiB, reported current | 250 MB / 239 MiB | 11–12 s |
| 32 KiB, default target | 125 MB / 119 MiB | 5–6 s |
| 64 KiB, optional candidate | 62.5 MB / 60 MiB | about 3 s |

These are estimates based on the logged ~62.55 million current clusters, not
mount-time promises. ATA command overhead, USB speed/reset behavior and host
work remain. Apple FSKit's published FATManager initializes by scanning FAT
statistics in 12 KiB requests. Larger clusters shrink that metadata workload;
they do not fix a failed INQUIRY or guarantee native automatic mounting.
FAT32 retains its maximum individual file size of 4 GiB minus one byte.
Cluster slack increases: estimate actual allocated bytes for each candidate
from the file inventory, including many small cache files, before selecting.

## Facts established and facts still needed

- The iPod was not mounted during this planning pass. Do not reuse an earlier
  `/dev/disk6` or any remembered Linux device name.
- Current source supports FAT, not exFAT. Its FAT sanity check allows clusters
  up to a calculated 128 KiB; that is an implementation bound, not proof of
  64 KiB compatibility in the installed bootloader or Apple's firmware. The
  USB translation additionally limits its host-visible sectors-per-cluster
  field to 128: 64 KiB at 512-byte sectors. Stay below both limits.
- Project evidence describes an MBR with Apple firmware first, FAT32 data
  second, with 2048-byte partition/BPB units over a 512-byte ATA device.
  ProkBox presents translated 512-byte MBR/BPB sectors over USB. This must be
  measured again on the actual device and in the chosen recovery mode.
- The backup host is a Linux machine on the LAN. The iPod was not
  attached there at inventory time. Its local ext4 filesystem had exactly
  1,019,677,822,976 bytes available (~950 GiB), subject to other server use.
  A backups directory exists. No iPod-named directory was found within
  its first two levels; that does not rule out older backups elsewhere.
- `rsync 3.2.7`, `mkfs.fat 4.2`, `fsck.fat`, `sfdisk`, `losetup` and SHA-256
  are installed there. `ddrescue`, `mformat` and `mcopy` were not on PATH.
  `sudo -n true` requires a password. Owner-assisted administrative commands
  will be needed; do not request or store the password in files or chat.
- The logged free-cluster count suggests ~87 GiB allocated, but it is stale
  evidence, not a verified file inventory or backup-size measurement. The
  owner's latest estimate is about **78 GB of files**. Use the fresh file
  inventory for capacity and time estimates; allocated space includes slack.

## 1. Inventory and establish a recoverable baseline

1. Owner finishes all copying and safely ejects. Connect directly to the backup host;
   use Apple's disk mode for the preferred unfiltered imaging path. Record
   how to enter disk mode and confirm it actually works before migration.
   ProkBox translated USB is suitable for file reads, but is not automatically
   a raw physical image of the disk.
2. Identify the disk by USB vendor/model/serial where available, capacity,
   `/dev/disk/by-id` identity, and both partition signatures. Record exact
   disk bytes, device logical/physical sector size, raw MBR, partition entry
   units, firmware byte range, data byte range and unused ranges. Decode the
   BPB independently: bytes/sector, sectors/cluster, reserved count, FAT size,
   FAT count, root cluster, FSInfo and backup-boot offsets, hidden sectors,
   volume ID and label. Cross-check all absolute byte offsets against capacity.
3. Explicitly label every capture **raw internal-layout view** or **translated
   ProkBox USB view**. A translated image must never be restored blindly via
   a raw path: its MBR and boot fields differ. The Linux kernel's partition
   interpretation is not authoritative if its assumed units disagree.
4. Stop source-side writers for imaging. Unmount the iPod only as part of this
   owner-authorized migration session, not during unrelated music transfers.
   Record the source identity again after every reconnect.
5. Save the entire pre-data region, including partition map, Apple firmware
   partition, embedded Rockbox bootloader and gaps. Also save all partition
   entries and critical original filesystem metadata separately for inspection.
   Hash each capture. Do not assume that saving the first 512 bytes alone
   captures a 2048-byte-sector layout or all recovery assets.
6. Baseline test and record ProkBox boot, Apple firmware boot, Apple disk mode,
   native macOS attach and representative file hashes. Keep a compatible
   known-good `rockbox.ipod` off-device. No Windows/iTunes reinstall may be
   needed for either the proposed operation or rollback.

## 2. Back up every file and the small recovery regions

Preferred session directory on the backup host:
`<backups>/ipod-migration/<timestamp>/`, created only at execution.
Use `inventory/`, `files/`, `recovery/`, `manifests/` and `logs/` subdirectories.
**Owner decision, 2026-10-18: do not acquire a full-disk or full-data-partition
image.** Protect all files with verified copies and preserve the small Apple
firmware/bootloader and layout captures separately. Rollback reconstructs the
original filesystem and restores files; it is not a sector-identical rollback.

1. Generate a complete path/type/size/mtime inventory, including dotfiles,
   `.rockbox`, all caches/databases/configs/bookmarks, `iPod_Control`, Apple
   library files, playlists, podcasts, audiobooks, video and root system files.
   Preserve AppleDouble sidecars and unusual names; do not filter apparent
   duplicates, rename songs or delete caches during the backup. Manifest
   encoding must round-trip filenames, including non-ASCII and newlines.
2. Copy the tree with resumable rsync from a read-only mounted source onto
   local ext4. Preserve timestamps with FAT's timestamp resolution accounted
   for; do not require FAT to provide Unix ownership/permissions. Log failures
   and actual bytes copied. No `--delete` or automatic cleanup of existing
   server backups. Store the manifest outside the source disk.
3. Compute SHA-256 for every regular source file and compare every backup
   path, length and hash. Cross-check representative known originals to guard
   against stable corruption in a faulty USB read path. Use the corrected
   firmware or Apple disk mode, not the old stale-64-KiB diagnostic build.
   Re-reading host-cached data is not an independent physical verification.
4. Independently re-read and hash the small recovery captures from section 1:
   partition map, pre-data region including Apple firmware/bootloader, and
   primary/backup filesystem geometry metadata. Record offset and length in
   bytes for each capture and its raw/translated view. No unreadable source
   file, incomplete copy, or unexplained hash mismatch is acceptable before
   formatting. There is no need to read unallocated data across the whole disk.
5. Check capacity in bytes **before** starting: provision file-backup allocated
   bytes + recovery captures + candidate-image physical usage + 50 GiB server
   headroom. The observed ~950 GiB available is ample for approximately 78 GB
   of files, subject to fresh inventory and current server usage. Additional
   storage or a NAS copy is optional, not a migration requirement. Do not
   assume existing archives are complete or modify unrelated server files.
6. Retain verified files and recovery captures throughout the migration.
   Recover representative Apple/system/music files from the backup tree and
   demonstrate the old-geometry reformat-and-file-restore procedure on a
   disposable image. An old `.rockbox` backup is not a full-library backup.

Direct attachment removes network traffic from the iPod-to-local-file copy.
It does not bypass the iPod's USB/storage ceiling. At 20 MiB/s, 78 GB takes
about 62 minutes per full file pass (about 67 minutes if the estimate means
78 GiB). Source verification and final restore add reads/writes; slower real
throughput increases those times. Run as a logged, resumable session with
continuous power. There is no full-terabyte imaging pass in this plan.

## 3. Rehearse the exact format on disposable images

1. Use copied metadata and sparse disposable candidate images with the real
   data partition's logical length. These contain generated fixture data, not
   a backup of the iPod. Keep all recovery captures and file backups immutable.
   Compare 32 KiB and optional 64 KiB
   cluster images with the same BPB sector size and byte-range boundaries.
2. Preserve partition map and firmware bytes, MBR type, start/end, partition
   ordering and sector interpretation. Reformat only the existing data extent.
   Default FAT settings: FAT32, two FATs, existing volume ID/label, appropriate
   reserved sectors and backup/FSInfo placement consistent with original layout.
   Retain root system directories from backup. Do not shrink or expand the
   partition, generate a new MBR/GPT, or run a whole-device erase utility.
3. Derive formatter parameters from bytes: sectors/cluster = cluster bytes /
   BPB bytes/sector. For the expected 2048-byte BPB, 32 KiB means 16 sectors
   per cluster; 64 KiB means 32. In a 512-byte BPB those values are 64 and 128.
   These are distinct from the device's 512-byte ATA addressing. `mkfs.fat`
   `-s` counts BPB sectors; its optional size operand counts 1024-byte blocks.
   `-h` and backup/FSInfo offsets must use their specified sector units.
4. Prepare a reviewed formatter command against a **bounded loop device** over
   the candidate partition image, then verify the emitted BPB and exact extent
   length. On the real disk use the same validated byte-offset/sizelimit method
   when kernel partition geometry cannot be trusted. No guessed `/dev/sdX2`
   command or unbounded `--offset` formatting on the whole device.
5. Exercise the repository's real sector translation code for each candidate:
   raw->512 host view->raw round trip, primary/backup boot fields, multi-chunk
   files, mixed writes, clean fsck and data hashes. Linux fsck success alone
   does not establish ProkBox/bootloader or Apple compatibility. Restore a
   representative system/media subset to the images and test real firmware
   FAT logic; then perform native macOS image mounts.
6. Prefer 32 KiB unless 64 KiB passes the full compatibility gates and offers
   a useful measured benefit. If neither passes, keep 16 KiB and pursue the
   serialized read-batching work instead. Reformatting cannot be used to
   silently abandon Apple recovery.

## 4. Execute in one maintenance window

Only after the backups, space and rehearsal receipts pass, present the owner
with the disk identity, exact data byte range, selected cluster size, reviewed
formatter command and rollback receipt for explicit formatting authorization.
Planning authorization does not authorize live formatting.

1. Revalidate identity, geometry and no open source files. Mark the whole disk
   as reserved for this session; stop auto-mount/sync activity for the operation
   without leaving permanent host configuration changes.
2. Keep the existing Apple firmware partition, bootloader and MBR byte-identical.
   Format only the validated data extent through the proven raw view. Do not
   format via ProkBox's translated USB path: its write filters intentionally
   reject layout changes and an incomplete format could strand the volume.
3. Verify BPB, extent limits and filesystem type; compare protected bytes to
   pre-migration hashes. Any change outside the allowed data range is a failure.
4. Restore the backed-up directory tree, including `.rockbox` and Apple data.
   Preserve case/names/timestamps. Start with boot/recovery assets and a small
   media subset for early boot checks, then restore the full library. Keep
   preserved databases/caches first; rebuild only if validation establishes
   they are stale. Existing absolute file paths should remain unchanged.
5. Restore timestamp-sensitive metadata last if the copy tool changes directory
   mtimes. Hash every restored regular file against the verified manifest;
   account explicitly for only filesystem-created metadata. No unexplained
   missing/changed file is acceptable. Run read-only fsck while unmounted.
6. Flush and safely eject, then validate ProkBox and Apple's boot/recovery.
   Do not boot both or test writes while a host still owns the disk.

## 5. Acceptance, rollback, and handoff

- ProkBox boots through the existing bootloader, sees the correct capacity,
  reads all media types and handles representative create/rename/delete and
  cache/database writes. Library/art builds complete and retain results after
  reboot. Playlists/bookmarks/resume positions remain usable.
- Apple firmware boots without a restore prompt or unexpected layout changes;
  Apple disk mode exposes the disk reliably. Record normal Apple playback
  separately from recovery. If normal playback fails, document it and obtain
  the owner's explicit acceptance before treating the migration as complete;
  disk mode remains mandatory.
- Linux and native macOS mount/read/write/eject/reattach pass; test native
  Windows when available, or record that as outstanding. The native macOS
  target is ten attach cycles with no manual mount, corruption or reset,
  measuring enumeration, native activation and completed mount separately.
- Reconfirm representative hashes after cross-host writes. Compare FAT scan
  bytes and timing to the 16 KiB baseline; record transport failures separately.
  Keep backup copies until the owner accepts the complete restored library.

Rollback: if formatting, restoration, boot or integrity validation fails, stop
all further experimentation. Reconfirm device identity and, with owner
permission, recreate the data filesystem with the recorded original geometry
and 16 KiB clusters, then restore every file from the verified tree. Restore
the protected partition map/firmware captures only if their hashes show they
changed, using their exact original byte ranges and matching raw view. Do not
write translated metadata through a raw path or paste an old BPB/FAT over a
new filesystem: file cluster locations have changed. Verify protected bytes,
restored file hashes, read-only fsck, boot and disk mode. This restores files
and boot functionality, not their original sector allocation, deleted data or
unallocated bytes. Apple firmware reinstall/iTunes is not required.

The execution handoff must include geometry in bytes and sector units, cluster
choice, file-backup/recovery-capture/manifest locations and hashes, tool versions, actual
formatter command, protected-region before/after hashes, restore/fsck results,
boot/recovery tests, native mount timings and remaining cross-host tests.

## Sources and engineering references

- Repository: `firmware/common/fat.c` (BPB checks), `bootloader/ipod.c`,
  `firmware/common/disk.c`, `firmware/usbstack/usb_storage_xlate.*`,
  `tests/usb/xlate_mount_test.sh`, `docs/pp5022-reference/boot.md`.
- [dosfstools formatter documentation](https://github.com/dosfstools/dosfstools/blob/master/manpages/mkfs.fat.8.in)
  defines cluster, BPB-sector, hidden-sector and length units; also inspect the
  installed 4.2 tool before generating the concrete execution command.
- [Apple FSKit FATManager](https://github.com/apple-oss-distributions/msdosfs/blob/main/msdos_appex/FATManager.m)
  explains the metadata workload being reduced; it is not a compatibility
  certification for Apple's iPod firmware.
