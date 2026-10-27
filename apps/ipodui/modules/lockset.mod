# MOD_LOCKSET: Extras > Screen Lock's setup screens (New and Confirm
# Combination, Lock, Reset Combination). The gate and the locked screen
# stay resident (lock.c). Read by tools/modpak.py into the module's v2
# pack entry (docs/phase12-brief.md 2.2); keys and values there.
class   = A
min_ram = 32
kind    = shell
exit    = immediate
work    = 0
quota   = 0
boost   = burst
exports = lockset
