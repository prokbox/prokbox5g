# Tagcache: commit can pin RAM-cache handle 0 after a failed cache load (upstream patch notes)

## Bug
In `apps/tagcache.c`, the failure branch of `load_ramcache()` frees the RAM cache and sets `tcramcache.handle = 0`, but never clears `tc_stat.ramcache_allocated`.

A later `commit()` sees `tempbuf_size == 0 && tc_stat.ramcache_allocated > 0` and decides to borrow the RAM cache. It then calls `tcrc_buffer_lock()`, which is `core_pin(0)`, and sets `tempbuf = hdr + 1`, i.e. it writes through `NULL + 1`.

## How to trigger it in stock Rockbox
1. Make the boot RAM-cache load fail: plug in USB (or trigger a power-off or stop-scan) during the load, or start with a corrupt `database_4.tcd`.
2. Run Database → "Update Now" or "Initialize Now".

Builds with the pinning check panic with "invalid handle pin: 0". Builds without it silently corrupt low memory.

## Fix
1. In the `load_ramcache()` failure branch, also set `tc_stat.ramcache_allocated = 0`.
2. In `commit()`, only borrow the RAM cache when `tcramcache.handle > 0`.
3. Optional hardening: in `build_lookup_list()`, treat a search as invalid if `!tc_stat.ramcache || tcramcache.handle <= 0` instead of pinning.

## Status in our tree
Fixed under `UI_COMMIT` (commit 1dfec3c7d4). The regression script is `tests/ui/ipodui-tcrc-load-fail.script`.
