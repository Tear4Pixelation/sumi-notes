# Pen storage: widths and colours stay as set

**Rule: a pen stays exactly as the user set it until the user changes it, or the app changes it on
purpose.** Exactly means bit for bit: no rounding on save, no conversion on load. The one deliberate app
change is described under "Colours follow the theme by slot". A future command such as "all widths +1"
goes through the same writers as the user's edits (below).

Read with [pen-and-tools.md](pen-and-tools.md) ("Relative pen width", "Pen width is written in one
place", "Only the quick settings write a pen") and [themes.md](themes.md).

## Where a pen lives

| What | Source of truth | Saved as (global config) | Copies |
|---|---|---|---|
| each draw tool's pen (width, unit flag, colour, tip...) | `ScribbleMode::drawPen` / `highlightPen` / `ephemeralPen` / `tapePen` | `toolModes` (`saveModes()`) | `ScribbleApp::currPen` (the active tool's), `PenToolbar::pen` (what the row shows) |
| width presets, one list per draw tool, with its unit | `PenToolbar::savedWidths` / `savedMarkerWidths` / `savedEphemeralWidths` / `savedTapeWidths` (`WidthPresets`) | same names, `rel:`/`abs:` + numbers | none |
| swatch row (shared by all draw tools) | `PenToolbar::swatches` (`ColorRef`) | `savedSwatches` | none |
| each draw tool's colour as a theme slot | `ScribbleApp::penColorRefs[DRAWTOOL_*]` | `penColorRefs` | none |

Documents never hold a pen: what lands in a document is a stroke's absolute `stroke-width` and literal
colour. Pens and swatches are global, shared by every tab.

Writers of a pen's width: `PenToolbar::updateWidth()` (the spin box, a preset tap, the shape row's
width) and `PenToolbar::setRelativeWidth()` (the Relative size toggle), both reaching the app through
`ScribbleApp::penChanged()` with `WIDTH_CHANGED`. Nothing else may write one.

## Exact numbers

- **Every number is written with `exactNumber()`** (`widthpresets.cpp`): the shortest `%.*g` that
  `strtod` reads back as the same double, from 6 digits up to 17. `1.6` stays `"1.6"`; `1.6/45` gets all
  the digits it needs. `writePen()` (toolModes) used an ostream's default 6 significant digits and
  `WidthPresets::serialize()` used `%.6g`: any width that came out of a unit conversion, or was typed with
  more digits, reloaded as a different number, so after a restart the pen no longer equalled the preset it
  was set from, and every later comparison was against a value the user never set.
- **The Relative size toggle switches between two kept renderings, it does not convert back.**
  `setRelativeWidth()` keeps the pen's width and the tool's presets in both units (`PenToolbar::relToggle`)
  from the first flip on; while neither is edited, every later flip just puts the other rendering back, on
  any ruling, however many times. Converting back multiplied by the line height of the page in hand:
  turned relative on a 40 unit ruling and back on a blank page (60), a Paper Patch, or another tab's
  paper, 1.6 came back 1.59 or 2.4, and its three presets with it. Even on the same ruling `w/lh*lh` is
  not always `w`. A width edit, a preset tap or another tool ends the pair; the next flip converts.
- What is still a conversion, and why it is allowed: the first toggle (the user asks for a unit change; the
  thickness on screen is kept), a legacy unit-less preset list taking its tool's unit once
  (`prepareWidths()`), and `loadModes()` converting a pre-relative-width marker once. `prepareWidths()` also
  converts a list whose unit differs from its pen's in PEN_MODE; nothing in the app can produce that pairing
  any more (the toggle converts both, each tool has its own list), so it only ever repairs a hand-edited or
  very old config.
- `readPen()` clamps a loaded width into the spin box's range for its unit (0.0005-4 line heights,
  0.01-200 units), as before: a width outside it could never be shown or edited.

## Colours follow the theme by slot

A swatch is a **slot**, `ColorRef` (`syncscribble/colorref.h`): a family ordinal plus variant
(`@3#AARRGGBB`), the neutral (`@n#...`), or a literal colour (`#AARRGGBB`, a custom colour or one picked
with no theme). The hex after a slot is the colour it last resolved to, used where there is no theme.
`PenToolbar::swatchColor()` resolves a swatch in the active document's palette, so the same row shows each
theme's own red in the same place. Slots are by ordinal, the same rule as `Palette::mapFrom()`; every
shipped theme has 12 families from the same seed, so index i is the same tone in all of them.
`cusp-walk-1` is untouched: this is lookup, not generation.

- **Nothing reseeds the row** except an empty row, or once, a legacy `savedColors` list in which no family
  made it (the old black/red/green/blue default). A literal that is exactly a member of the active theme is
  adopted as its slot (`refreshPalette()`); one that is not is shown, in a themed document that does not
  allow off-palette colours, as the theme's nearest, which is what a tap gives the pen anyway.
- **The pen's colour follows the same way** (`ScribbleApp::followPalette()`, from `updatePenToolbar()` and
  `setPen()`): `penColorRefs` holds each draw tool's slot, recorded whenever its colour changes (for the
  marker, a family's vivid highlighter counts as that family's slot - `ColorRef::ofPen()`). When the
  active palette changes, a pen still on its slot's colour moves to the new theme's colour at that slot,
  keeping its alpha. A pen whose colour is not its slot's (a custom colour, an older config) is left
  alone. This is the app changing a pen's colour on purpose; widths are never touched by it.
- **The active palette is checked on every toolbar refresh** (`PenToolbar::syncPalette()` compares the
  recipe the row was resolved in). A switch to a tab already in memory runs no `ScribbleDoc::loadConfig()`,
  so `refreshPalette()` alone was never enough.
- `savedSwatches` is the row; `savedColors` is read only when `savedSwatches` is empty (upgrade), and is
  no longer written - a second copy would be a second source of truth.

## Root causes found (Oct 2026)

Widths drifting, "often 0.01 smaller than set":
1. **The Relative size toggle converted back at the current page's line height** (above). This is the
   one path found that moves a width by a visible step: on and off on the same ruling is a few ulps, but
   off on another ruling is that ruling's ratio - 1.6 through a 40 unit ruling and back on a 39.7 one
   (a Paper Patch, an imported page) is 1.587, shown as 1.59; back on a blank page (60) it is 2.4. Tabs make
   it easy to hit - the toggle on in one document, off in another - and the presets move with it.
2. **Lossy saving**: 6 significant digits in `toolModes` and the preset lists (above). Invisible in the
   `%.3g` box by itself, but it meant no width that had been through a conversion survived a restart
   intact, and the pen stopped equalling its preset.
3. **Return on an unedited relative width rounded it** (after the box began showing line heights as a
   percent). `showsPenWidth()`, which tells "Return without an edit" from an edit, compared `"%.3g"` of the
   fraction while the box shows `"%.3g%%"` of the fraction times 100. The two round differently in binary:
   0.4365 shows as "43.6%" but its own `"%.3g"` is "0.437", so the unedited text read back as 0.436, an
   edit. It now compares through the box's own scale and format; any new display scale must go there too.
4. **A selection's width edit redefined the pen's preset** (found after the user said Relative size is
   always on for them, widths 3-7%). With the select tool in hand the toolbar row is the selection's and
   shows the pen's presets resolved to units; the stroke drawn at a preset is ringed, and tapping the
   ringed one opens the same detail editor as on the pen. A step, a scrub or a typed width there went
   through `updateWidth()`, which "redefines that preset while the editor is open" - in any mode, so the
   pen's preset became the stroke's new absolute width over the selection's line height. The pen itself
   was untouched until its next tap on that preset, which then gave it a width the user never set for the
   pen: a 5% preset stepped down on a selection on a 40 unit ruling (2.0 -> 1.8 units in `PEN_WIDTHS`)
   came back 4.5%, and in a Paper Patch the step is measured against the patch's line instead. It looks exactly like "the pen lost a bit while Relative size stayed on". Fix: `updateWidth()` redefines
   a preset in `PEN_MODE` only; from a selection the editor edits the selection's width and nothing else.
   This was a deliberate design from the 144 line height fix (store the selection's width "in the list's
   own unit") - the unit was right, writing the pen's list from a selection at all was the bug.
Not writers, checked again: tab switches, rulings, tool switches, Switch Back with ink still selected,
selections on the page and in a Paper Patch, the selection popup's width item (presets, steps), undo of a
selection's width, preset taps on the selected preset, Return without an edit (pen and selection), the
width editor opening and closing with focus in the box, theme changes, restarts. Also read and ruled out:
`ScribbleArea::resolvedPen()` (resolves for drawing, never writes back), `readPen()`/`loadModes()` (exact,
the marker conversion is once), number keys and Pencil double tap (tools, not widths), sync and undo (no
pen in a document or the history), the relative step (whole percents, `round(w*100) +- n`, so 3% stays the
literal 0.03). Unproven, left alone: an accidental -/+ tap or scrub on iPad would be a 1% step, which is a
real edit as far as the code can tell.
`penPersistTest()` drives the first group 300 times, and part three drives a relative pen through real
selections 1200 times; both require every width and preset bit-identical.

Swatches after switching tabs:
3. **`refreshPalette()` reseeded the whole row when any swatch was not an exact member of the active
   theme.** Its comment said "only when every swatch fails"; the code was an AND over membership. Swatches
   were literal colours, so a document with another theme has none of them: opening it (a fresh load runs
   `loadConfig()` -> `refreshPalette()`) threw the user's row away, and a swatch edited there was thrown
   away again by the next document with another theme. Every tool.
4. **A switch to a tab already in memory never rebuilt the row**, so the row (and `themed`) stayed the
   previous document's while `docPalette()` and the snap in `updateColor()` used the new one. A tap snapped
   the pen to the new theme's nearest colour, which no swatch held: no ring, "picking a swatch no longer
   shows the selection ring". An edit then stored a new-theme colour into an old-theme row, so the next
   fresh load reseeded it (3).
5. **Worse for the marker**: `toolColor()` maps a swatch to its family's vivid highlighter through
   `Palette::indexOf()`, which fails for the previous theme's colours, so every marker swatch turned into
   "that colour at marker alpha" - different from the pen at once, before any tap, and every tap snapped
   again. The marker's ring was gone right after the switch; the pen's only after a tap. Separately, the
   custom colour editor and Delete Swatch gave the marker the opaque ink colour instead of `toolColor()`:
   it drew solid and never matched its swatch.

## Tests

- `ScribbleTest::penPersistTest()` (in-app): part one switches the active document's theme in memory and
  by a fresh load, for the pen and the marker - the row shows the other theme's colours in the same slots,
  an edited swatch survives the round trip, the pen follows its slot, the ring stays and a tap after the
  switch is ringed; the custom editor keeps the marker's colour; the row is saved as slots. Part two is the
  300 step width run (set, switch tool, other ruling, other theme, selection and back, restart, toggle off
  and on, toggle off/other ruling/on, preset tap, Return, width editor open/close, single toggle). Part
  three keeps Relative size on with a 3-7% pen and puts real strokes into the active document, on the page
  and in a Paper Patch with its own line, each drawn at the pen's resolved width; it selects them with the
  select tool and edits their width every way the UI offers (a preset, the ringed preset's editor with a
  step, a typed width, a scrub, Return; the selection popup's presets and steps), undoes, deselects, puts
  the pen back with ink still selected, changes the ruling and restarts, 1200 steps. Only the pen's own
  edits (width set, preset tap, the pen's preset editor) may move the pen or its presets.
  `penWidthPresetTest()` checks the same rule once: a width typed into the editor opened from a selection
  goes to the selection, and the pen's presets are bit-identical afterwards.
- `scribbletest/widthpresettest.cpp`: `exactNumber()` round trips and is short; a converted list survives
  50 save/load cycles. `scribbletest/colortest.cpp` `testColorRefSlots()`: slots resolve across themes and
  back, the marker's vivid colour is its family's slot, config text round trips, legacy entries parse.
- Mutation-checked (see the commit): 6 digit `writePen()`, no toggle restore, no `followPalette()`, no
  `syncPalette()`, the custom editor without `toolColor()` - each fails a check. `updateWidth()` without
  its `mode == PEN_MODE` gate fails part three (step 43, a typed width on a selection: the 6% preset became
  1.86%) and the `penWidthPresetTest()` check.

## Open

- A literal custom swatch shows the theme's nearest colour in a themed document; it is not converted to a
  slot (that would lose the custom colour for unthemed and off-palette documents).
- A theme with a different family count than the shipped 12 resolves an out-of-range slot to its last
  colour rather than proportionally.
