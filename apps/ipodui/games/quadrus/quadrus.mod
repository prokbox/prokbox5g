# MOD_QUADRUS (P12 W4, docs/design/sdk.md 13.2): the falling-block game,
# a 12 x 18 well. An SDK module: linked only against the SDK table.
class   = A
min_ram = 32
kind    = sdk
abi     = 0.1
exit    = graceful 200
work    = 8192
state   = 384
quota   = 64
boost   = burst
strings = quadrus
exports = quadrus
