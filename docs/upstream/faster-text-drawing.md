# Faster anti-aliased text drawing (upstream patch notes)

> **Decision (owner, 2026-10-08):** the patch goes **inside `blend_two_colors()`**, so every draw mode benefits. Our firmware carries exactly the same upstream-ready change (unguarded) instead of the earlier `DRMODE_FG`-only loop. The per-loop version below is kept for reference.

## What it does
Rockbox draws anti-aliased text one glyph "cell" at a time. Each pixel in the cell has an opacity, and the driver blends every pixel with the background, even pixels where the blend can't change anything.

In real text most cell pixels are either:
- **fully transparent** (the space around and between strokes), about 73% of pixels in a bold 15 px font, where the blend just rewrites the background unchanged; or
- **fully opaque** (the inside of thick strokes), about 9%, where the blend just writes the text colour.

The patch skips the blend maths for those two cases. Partly transparent pixels (the soft anti-aliased edges) are blended exactly as before, so **the output is pixel-for-pixel identical**.

## The change
One loop in `firmware/drivers/lcd-16bit-common.c`, in `lcd_alpha_bitmap_part_mix()`, the `DRMODE_FG` case (text drawn in the foreground colour).

In this function's alpha convention, `ALPHA_MASK` (15) means "keep the background" and `0` means "full foreground colour".

```c
/* before */
do
{
    *dst = blend_two_colors(*dst, fg, READ_ALPHA());
    dst += COL_INC;
} while (--col);

/* after */
do
{
    unsigned a = READ_ALPHA();
    if (a != ALPHA_MASK)                      /* fully transparent: leave *dst alone */
        *dst = a ? blend_two_colors(*dst, fg, a)
                 : fg;                        /* fully opaque: just write fg */
    dst += COL_INC;
} while (--col);
```

In our tree the new loop sits under `#ifdef HAVE_IPOD_UI`, so our stock comparison build stays byte-identical. For upstream, drop the `#ifdef` and replace the old loop.

## Why it's safe
- `blend_two_colors(dst, fg, ALPHA_MASK)` returns `dst` exactly, and `blend_two_colors(dst, fg, 0)` returns `fg` exactly. The shortcuts produce the same values the blend would have.
- Only the text foreground path changes. Other draw modes and full-colour alpha bitmaps are untouched.
- Verified by comparing screenshots before and after: every pixel identical, including text on gradients and white text on the selection bar.

## Measured gain
ARM instruction counts, from real `rockbox.elf` functions run in an emulator, bold 15 px font:

| Text | Before | After | Saving |
|---|---|---|---|
| "Music" | 33,941 | 22,403 | 34% |
| "Bohemian Rhapsody" | 117,824 | 77,391 | 34% |
| A 38-character label | 232,793 | 153,418 | 34% |

The loop stays in IRAM; the code grows by about 16 bytes.

## Trade-off to mention in review
The patch adds a compare and branch per pixel. On the ARM7TDMI (no branch prediction) a taken branch costs a few cycles, so the patch is a clear win for text, which is mostly empty or solid. It only touches the text path, where that holds for any normal font. Small or very thin fonts gain less, but don't get slower in practice, because their cells are still mostly transparent.

## How to test
1. Build any colour target before and after.
2. Capture the same screens (menus, Now Playing with a long scrolling title) and compare the PNGs. They should be identical.
3. Optionally, time a long scrolling title on the device: it should use noticeably less CPU.

## Recommended form (owner decision): inside `blend_two_colors()`
`blend_two_colors()` is a small inline helper in `lcd-16bit-common.c`. Its only callers are the 10 draw-mode loops of `lcd_alpha_bitmap_part_mix()`. Shortcutting its two exact endpoints there covers every mode (FG and SOLID text, BG, backdrops, alpha images) with one change:

```c
static inline unsigned blend_two_colors(unsigned c1, unsigned c2, unsigned a)
{
    if (a == ALPHA_MASK)        /* fully c1: exact, no maths */
        return c1;
    if (a == 0)                 /* fully c2: exact, no maths */
        return c2;
    /* ...existing blend unchanged... */
}
```

- **Identical output:** the existing maths returns exactly `c1` at `ALPHA_MASK` and exactly `c2` at 0.
- **Keep the early returns before the `swap16()` lines** (the `RGB565SWAPPED` targets). `c1`/`c2` are already in framebuffer format, so returning them unswapped is exact, and those targets also skip three swaps per endpoint pixel. Placed after the input swaps, they would return swapped colours. Add a comment in the patch saying so.
- **What it doesn't do:** skip the store when the result equals `*dst`. That is a minor extra memory write per pixel next to the ~20+ instructions of blend maths saved.
- **IRAM:** inlined into all 10 loops, so roughly +60–100 B. Report the figure per target in the patch.
- **To measure before submitting:** FG text, SOLID text (the stock default draw mode) and an alpha icon (instruction counts), plus pixel-identical screenshots.
- **Separate, optional patches:** the same change for `lcd-24bit.c` and the plugin copies (mpegplayer, rockpaint).

## As landed (#55, 2026-10-08)
- **gcc keeps `blend_two_colors()` out of line at `-Os`.** On the iPod Video build it was never inlined into the 10 loops: it sat in `.text` (DRAM) and every pixel called it through a veneer from the IRAM loop. With the endpoint tests inside that out-of-line function, the gain was smaller (FG text 113,258 → 83,481 instructions) and FG text was slower than the old `DRMODE_FG` loop (73,481).
- So the patch splits it: `blend_two_colors()` is a `FORCE_INLINE` wrapper with the two endpoint returns, and the maths moves unchanged to `blend_two_colors_mix()` (still out of line). Common pixels never leave the loop.
- **Measured** (Unicorn on the real `rockbox.elf`, "Bohemian Rhapsody" in the 15 px bold font, per-glyph `lcd_alpha_bitmap_part()` calls as `lcd_putsxyofs()` makes them; an alpha image is three 35 px glyph masks over an RGB565 image through `lcd_alpha_bitmap_part_mix()`):

| Case | Stock before | After | Saving |
|---|---|---|---|
| FG text | 113,258 | 75,036 | 34% |
| SOLID text (stock default) | 113,326 | 76,178 | 33% |
| "Music", FG | 32,471 | 21,635 | 33% |
| Alpha image | 58,386 | 37,397 | 36% |

- Framebuffer hashes are identical before and after in every case, and every golden-covered frame of the 17 golden `ipodui-*` scripts is pixel-identical between builds with and without the patch.
- **Cost:** core IRAM +184 B (`.iram` 0x255c → 0x2614; `lcd_alpha_bitmap_part_mix` 0x454 → 0x50c), `.text` +20 B for `blend_two_colors_mix`. Core IRAM has 3.3 KB of headroom (`docs/hardware.md`).
