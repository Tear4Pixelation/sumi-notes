# Highlighter: a kind of stroke, translucent by preference

A highlighter (marker) stroke is recognized as one on the element and drawn at the global
**Highlighter opacity** preference, instead of carrying its translucency baked into its color. The bug
this fixed: the selection's Change Color wrote the picked swatch's alpha (opaque) into `fill-opacity`,
so a recolored marker came out solid - in this app and in any other viewer.

## What a highlighter is

`Element::isHighlighter(node)` is true for any of three generations, all drawn the same way:

1. **`write-highlighter` class** (`Element::HIGHLIGHTER_CLASS`) - written from now on.
2. **`comp-op="multiply"` alone** - written for markers since the multiply change
   ([pen-and-tools.md](pen-and-tools.md#marker-multiplies-text-stays-dark)); only marker paths ever
   write it (StrokeBuilder, Draw Lines / snapped marker shapes, Noteful highlighters), so it identifies
   one. This covers markers an earlier recolor had already turned opaque: they draw translucent again.
3. **`Element::isLegacyMarker()`** - chisel class, no comp-op, `fill-opacity` < 1 (before multiply).

**Recognized, not rewritten.** Nothing is converted on load: opening an old document neither dirties it
nor adds an undo step, the same choice `isLegacyMarker()` made.

`Element::markHighlighter(node)` is the one place a new highlighter is made: class, comp-op multiply and
the fallback opacity (below). Called by `StrokeBuilder::create()` (DRAW_UNDER pen),
`ScribbleArea::createShapeElement()` (DRAW_UNDER pen: Draw Lines highlighter, snapped marker shape)
and `notefulimport.cpp` (`shape.highlighter`; the Noteful 0.5 alpha hack is gone, its color is
imported opaque). A new path that makes marker elements must call it.

## Drawing: the preference wins

`Element::applyStyle()` runs after SvgPainter has read the node's own `fill-opacity`/`stroke-opacity`
into its per-node `ExtraState` (pushed per node, so the override cannot leak to siblings), and for a
highlighter sets both to `Element::HIGHLIGHTER_OPACITY`. It also still turns source-over into multiply
for the legacy kind. The color's own alpha and the stored opacity are ignored when drawing.

Multiply stays: at any opacity the print under a marker stays dark, so "nearly opaque" is safe - the
marker is also DRAW_UNDER, under the ink of its layer.

## The preference

`highlighterOpacity` (int percent, global config, default **65** = the old `MARKER_ALPHA` 165/255, so
nothing looks different until changed). Preferences > General > *Highlighter opacity*, presets Light
65 / Medium 75 / Strong 85 / Nearly opaque 95 (`enumvals`; an off-preset value set by hand is shown as
"Other (n)" by the config dialog). `ScribbleApp::loadConfig()` clamps it to 10-100 into
`Element::HIGHLIGHTER_OPACITY`; `reloadConfig()` -> `ScribbleDoc::loadConfig(true)` ->
`pageSizeChanged()` repaints every open document, so OK applies it at once.

## In the file: fallback opacity for other viewers

A highlighter still stores an opacity (`fill-opacity` for a filled chisel stroke, `stroke-opacity` for a
stroked shape/Noteful line) - only as the **fallback** for Stylus Labs Write, browsers and other SVG
viewers, which know nothing of the preference. It is the preference's value at the time the stroke was
made. Stylus Labs Write ignores comp-op multiply and the unknown class, so it shows a plain translucent
stroke, as before.

## Recolor

`applyProperties()` (element.cpp - every recolor path: selection Change Color, the pen toolbar in
selection mode, `StrokeChangedItem` undo/redo, sync's peers applying the same item) takes a highlighter's
**color, never its translucency**: after setting the color it puts back the stored opacity, or writes
the preference's value if there was none or it was 1 (a marker an older recolor made opaque). This is
only to keep the fallback sensible; drawing would be translucent either way. The class is not added on
recolor, so undo restores exactly what was there.

Undo and sync need nothing new: `StrokeChangedItem` records `getProperties()` (color with the fallback
alpha) and re-applies through the same `setProperties()`; a peer runs the same code on the same
(recognized) node.

## Tests

`ScribbleTest::highlighterTest()` (in-app): a marker stroke carries the class and a 0.65 fallback; a
Change Color through `ScribbleArea::setStrokeProperties()` with an opaque blue is one undo step, keeps it
a highlighter, keeps the fallback < 1 and samples translucent blue over white paper (red > 60; opaque
multiply leaves 0); raising the preference to 0.95 draws it stronger with no file change; a stroke made
opaque by an old recolor (comp-op, `fill-opacity` 1, no class) and a pre-multiply chisel marker with
0.4 alpha both draw at the preference; undo restores the color and leaves a highlighter.
Mutation-checked: without the recolor fix and the render override (the old behavior) it fails.
`notefulImportTest` now expects the Noteful highlighter to be a highlighter with the preference as its
fallback. `test15_ref.html` (marker strokes) gained the class and `fill-opacity="0.65"`.

## Open

- The pen toolbar's marker swatches and `PenPreview` still show the marker pen color at `MARKER_ALPHA`
  (165), not the preference: the swatch identity (`toolColor()`, `ColorRef::ofPen`) compares that color,
  so changing it is a separate, careful change.
- PDF export (`PdfWriter`, in usvg) does not run `applyStyle()`, so it uses the stored fallback opacity,
  not the current preference. Only `defaultCompOp` has a hook there.
- Page thumbnails cached in the sidebar are not invalidated when the preference changes.
- Draw Lines highlighter lines / snapped marker shapes drawn before comp-op was written for them
  (pen-and-tools.md, "Every path that makes a marker element") have no class, no comp-op and no chisel
  class: indistinguishable from a translucent pen line, so they stay as they are (stored alpha,
  source-over).
