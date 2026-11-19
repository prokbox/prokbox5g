# Custom EQ editor (#82)

`apps/ipodui/screens/eqedit.c`. Three custom slots (Settings > EQ rows
Custom 1-3, `eq_custom.h`), one editor.

## Entry points
- Settings > EQ: choosing a Custom row makes it the EQ in use and opens the
  editor; Menu comes back to the list.
- Extras > Custom EQ: a list of the three slots; each opens the editor
  without changing the EQ in use.

## Behaviour
- The wheel picks a band; Select moves to its frequency (4 stops), then its
  level (-12..+12 dB; Hold Select toggles narrow / wide on bands 2-4); Select
  is done, Menu steps back and leaves from the band step.
- Play compares with EQ off (the same level plan, flat, so the A/B is
  level-matched). Hold Select on the band step resets the slot to flat.
- Every change plays at once (`ui_eq_live()`); the slot is stored when the
  editor is left, only if it changed (`ui_eq_custom_set()`).

## Drawing
- Corners are the real ones at the output rate: nominal x fs / filter rate
  (0.92 at 44.1 kHz, 2 at 88.2 / 96 kHz). Labels, ticks, band columns and
  the curve all move with the rate. Axis: 20 Hz to 24 kHz.
- A peak's handle sits on the curve at its centre. A shelf's (Bass,
  Treble) sits on its corner's bar at the shelf's full level, the plateau,
  so it matches the readout (owner, 2026-11-13): at the corner itself, the
  half-gain point, the curve is only half way up. The plot's labels (+12,
  -12, 100, 1k, 10k) give way to any handle that would cover them.

## Layout (owner's pick, 2026-11-13)
The readout is three captioned cells (Band / Frequency / Level); the cell
the wheel is editing is blue. Under it: the plot, the selected band's four
frequency choices, the band names, the hint. (The other two candidates, a
one-line readout and plot-first, were dropped.)

Sheet: `out/eqedit-final.png` (from `tests/ui/ipodui-eqedit.script`).

## Tests
`tests/ui/ipodui-eqedit.script` (both entry points, every band, Compare,
persistence over reboots, `# same:` redraw pairs, the handle's row as
`hy=` in each band's log line: Bass +4 / -8 dB, Treble +12 dB) and
`tests/ui/ipodui-eqedit-rate.script` (96 kHz via the simulator-only
`IPODUI_EQ_RATE` hook).
