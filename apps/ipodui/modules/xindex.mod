# MOD_XINDEX: the sync-time indexers (P12 W1-N; W3-K and W3-C add their
# builders to its ops). Runs only off-screen in a sync window (the
# post-USB moment, the boot window), on the storage worker, inside the
# driver's maint_take() block (xindex.c), pinned while it runs. The one
# shell module allowed a directory walk (docs/phase12-brief.md 2.3).
class   = A
min_ram = 32
kind    = shell
exit    = immediate
work    = 0
quota   = 0
boost   = burst
context = sync
exports = ops
