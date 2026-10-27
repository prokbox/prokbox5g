# MOD_BRICKS (P12 W4, docs/design/sdk.md 13.3): the brick-breaker,
# 8 x 14 bricks. An SDK module: linked only against the SDK table.
class   = A
min_ram = 32
kind    = sdk
abi     = 0.1
exit    = graceful 200
work    = 6144
state   = 256
quota   = 64
boost   = burst
strings = bricks
exports = bricks
