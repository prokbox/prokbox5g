# Now Playing art reflection: geometry and fade (exact spec)

Owner direction (2026-10-08): the reflection should be bigger, fade out further down and more gradually, and obey real floor-mirror optics. Reference: owner photo `np-normal-b.jpg` (photo 1203). There the reflection fills the whole gap between the art's bottom edge and the progress bar, and is still faintly visible just above the bar.

This file replaces the fixed `NP_REFL_LEN 25` / `NP_REFL_A0 90` / `NP_REFL_LIMIT 196` rule in `apps/ipodui/np_draw.{c,h}`.

## 1. The slanted card (P11 geometry, 2026-10-09)
The art is a flat card standing on a glossy floor, turned toward the text. Screen columns run over `c = 0 … 136` (x = 9 … 145); `t = c / 136`.

Measured on the owner references rectified to 320×240 (lit-area edges fitted, homography; 50 % edge crossings on the 4× warp, ±0.5 px relative, ±1 px absolute):

| Edge | np-clean-ufo | np-clean-battlecry | photo 1203 (3rd check) | Used |
|---|---|---|---|---|
| left x (first column) | 8.96 | 9.05 | 8.97 | **9** |
| right x (exclusive edge) | 145.65 | 144.78 | 145.97 | **146** (last column 145) |
| top at left / right | 32.45 / 43.70 | 32.89 / 43.61 | 31.73 / 43.42 | **33 / 44** |
| bottom (exclusive) at left / right | 168.1 / 162.6 | 167.1 / 162.4 | 167.8 / 162.1 | **168 / 163** |
| height left / right | 135.6 / 118.9 | 134.2 / 118.8 | 136.1 / 118.7 | **135 / 119** |
| vanishing point (top ∩ bottom) | — (bottom fit noisy) | (1195, 126) | (1080, 123) | (≈1150, 126) |

| Quantity | Formula | Left edge (c=0) | Right edge (c=136) |
|---|---|---|---|
| top edge `y_t(c)` | `33 + 11t` | 33 | 44 |
| bottom edge, exclusive, `y_b(c)` | `168 − 5t` | 168 | 163 |
| card height `h(c)` | `135 − 16t` | 135 | 119 |
| source rows per screen row `s(c)` | `142 / h(c)` | 1.052 | 1.193 |

- The horizontal source column is perspective-correct: `u(c) = 142 · t · h_R / ((1 − t) · h_L + t · h_R)`, with h_L = 135 and h_R = 119 (`np_slant_u256`).
- Constants: `NP_SLANT_*` in `apps/ipodui/np_draw.h`; the per-column tables in `np_draw.c` are generated from them; `art/np/npph.py` mirrors them. Any change bumps `NP_LAYOUT_HASH`, so composed bundles are rebuilt.
- For fitted non-square art, `y_t`/`y_b` are those of the fitted image inside the 142 box.

## 2. Mirror mapping (floor reflection)
Each column mirrors about **its own contact point** `y_b(c)`, the card's bottom edge on the floor.

For reflection row `d = 0, 1, 2, …` below the contact line, so screen `y = y_b(c) + d`:

```
source row  v(c, d) = v_bottom − (d + 0.5) · s(c)      (v_bottom = last source row of the image)
pixel       out     = bg · (1 − α) + art[u(c), v] · α
```

- The reflection's top edge lies exactly on the card's slanted bottom edge, with no gap or overlap.
- The reflection is **vertically compressed by the same per-column scale `s(c)`** as the card, so it shrinks toward the far side exactly as the card does.
- **Consequence ("it angles out the opposite way"):** a full mirror would end at `y_b + h`, which is 318 at the left and 293 at the right. That lower edge rises toward the right, mirroring the card's top edge about the floor line, toward the same vanishing point. We show only the faded top part of that mirror image (section 3), and its fade contour keeps that perspective.

This is what the current code already does in `col_reflect`: per-column mirroring with `vstep = s(c)`. The changes are in the fade and the limit.

## 3. Fade: exponential in floor distance, so the contour follows perspective
```
τ = d / h(c)                          (floor distance as a fraction of card height)
α(τ) = A0 · exp(−K · τ)               A0 = 0.35 (90/256),  K = 5.0 (P11; was 8.7)
```
Measured 2026-10-09 (owner request: longer, more gradual) on the rectified references, α = (bg − reflection) / (bg − mirrored art pixel), median over columns x 14–140 where the art contrast exceeds 70:

| τ | 0.01 | 0.05 | 0.11 | 0.17 | 0.23 | 0.29 | 0.33 | ≥ 0.41 (y ≥ 222) |
|---|---|---|---|---|---|---|---|---|
| np-clean-battlecry (owner photo) | 0.35 | 0.28 | 0.24 | 0.18 | 0.14 | 0.09 | 0.09 | ≈ 0 |
| np-clean-ufo (web photo, contrast boosted) | 0.68 | 0.59 | 0.48 | 0.34 | 0.24 | 0.13 | 0.09 | ≈ 0 |
| model (A0 0.35, K 5.0) | 0.35 | 0.27 | 0.20 | 0.15 | 0.11 | 0.08 | 0.07 | feathered to 0 |

Battlecry fits A0 0.35, K ≈ 4.9; ufo has the same shape at about twice the amplitude (its tone curve), so A0 stays 0.35. The constants (`NP_REFL_A0`, `NP_REFL_K_Q8`, `NP_REFL_LIMIT`, `NP_REFL_FEATHER`) live together in `np_draw.h`: a one-line retune, then `art/np/npph.py` and the NP goldens.

## 4. Limit and feather
The reflection runs down **behind the elapsed time and the bar row** (both references show it there) to **y = 219 inclusive** (`NP_REFL_LIMIT` 220: the bar's own flat reflection starts at 220). The last **12 rows** (y 208–219) are feathered by `(rows_left / 12)²`, so it fades to nothing with no edge. At the left column that is d = 0…51 (τ ≤ 0.38), at the right d = 0…56.

## 5. Optional softness (do it if cheap; owner: "feels kinda sharp")
Add a slight vertical blur that grows with distance. Sample `r = ⌊2 · d / 29⌋` extra source rows on each side (0 at the top, up to 2 at the bottom) and average them: 1, 3 or 5 taps. It is computed once per art change, so the cost is fine.

## 6. Interaction with the bar row and slides
`NP_ROW_Y` (198) is the top of the mode-row band that slides (Scrubber, Rating, Shuffle, Volume). The reflection fills rows 198–219 of x 9–145 of that band. That band background is an **alias** of the composed art's rows 165–186 (QW2: the composed image is x 9–145 × y 33–219, 137 × 187 = 51,238 B, in the "np" block, now 58,368 B). Every slide frame and every row redraw restores it (`bg_fill`) and draws over it: the bars and the shelf are opaque on top, the stars and glyphs blend over it, and the elapsed time is drawn with a multiply blend (`np_atlas_draw_mul`) over the restored band.

## 7. Tests
- **Host test** (`tests/npscreen`): render a 142×142 test card (a vertical gradient with distinct row colours), and check:
  - (a) for c ∈ {0, 68, 137}, the reflection row d samples source row `v_bottom − (d + 0.5)·s(c)` (±1 row);
  - (b) α matches the table above within ±2/256;
  - (c) rows ≥ 205 are untouched;
  - (d) a fitted 16:9 and a 2:3 image mirror about their own bottom edges.
- **Sim:** a golden of a Rating-mode slide frame shows the reflection intact under the sliding row.
- **Cost:** report the art render instruction count (budget ≤ 350k per art change; today 281k).
