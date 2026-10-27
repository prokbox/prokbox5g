# MOD_CHUTE (P12 W4, docs/design/sdk.md 13.3): catch the jumpers,
# an LCD game in spirit. An SDK module: linked only against the SDK table.
class   = A
min_ram = 32
kind    = sdk
abi     = 0.1
exit    = graceful 200
work    = 9216
state   = 128
quota   = 64
boost   = burst
strings = chute
exports = chute
