# MOD_SOLITAIRE (P12 W4-1, docs/design/sdk.md 13.1): Klondike, draw one or
# three. An SDK module: linked only against the SDK table (sdk.sym).
class   = A
min_ram = 32
kind    = sdk
abi     = 0.1
exit    = graceful 200
work    = 12288
state   = 640
quota   = 64
boost   = burst
strings = solitaire
exports = solitaire
