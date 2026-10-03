# Ruling regions

Called **Paper Patch** everywhere the user sees it (tool, help, tooltips, docs); "ruling region" is the
code's name only, and `write-ruling-region`/`__rr*`/`<regionchanged>` are storage and wire format, so they
stay. An area of a page with its own ruling, overriding the page's inside it - for scanned or imported pages
whose printed lines are uneven or tilted. Made with the **Paper Patch** entry on the shape row (a drag,
like the box; single use), selected **only** through the "..." button drawn in its bottom-right corner while
the select tool is in hand. Geometry in `syncscribble/rulingregion.*` (no app dependencies, tested
standalone by `scribbletest/regiontest.cpp`), the element in `Element` (`isRulingRegion()`), the
selection in `RegionSelector` (`selection.cpp`), the UI in `ScribbleArea` and the region panel in
`MainWindow::buildRegionPanel()`.

- **Outline and ruling are separate.** `corners` is a free quad (drag a corner: reshapes the outline only,
  lines and ink stay); `origin`/`angle`/pitches are the ruling, whose lines are always parallel and evenly
  spaced - bending them to a skewed outline would give "line N" no single height. The red handle slides
  the lines (origin); Level resets the angle.
- **The region is a `<g class="write-ruling-region">` in `contentNode`**, parameters as `__rr*` attributes,
  children (paper fill + rule lines/dots) a regenerated cache like a shape's path. So undo (`StrokeAdded/
  Deleted/Transform`), sync (`<addstroke>`) and copying pages need nothing new, and an older Write draws it
  as a plain group. Parameter edits are `RegionChangedItem` (`<regionchanged>`); move/rotate/scale are
  ordinary transform items, because `Element::applyTransform()` has a region branch that maps the
  parameters (never a node transform, which would leave `__rr*` stale).
- **Not on any layer, below all of them.** `LayerList::REGION_LAYER` (-2, never serialized) has
  `zIndexOf() == -1`, which keeps regions first through `addStroke`, restacking and `layerFirstElement`
  (draw-under would otherwise put a marker stroke under an opaque region). Undo/sync replay insert at a
  literal sibling, and a NULL or vanished sibling means "on top", so `reinsertPos()` in `syncundo.cpp`
  re-homes a region below ink in that case.
- **Not ink.** `Page::isEditable()` returns false for a region - explicitly, since an unknown layer fails
  open - which keeps it out of every selection, eraser and insert space; `findStops` and the history panel's
  select lane needed their own skips. Deleting a selected region deletes only the region.
- **Moving it carries its ink.** The region selection holds the region plus every editable element whose
  bbox centre is inside the outline, re-decided at the start of each gesture (`refreshRegionSelection`),
  drawn as ordinary ink (`STROKEDRAW_NORMAL`). A tap, or a drop off the window or on another page, is not
  a move.
- **The size handle makes more paper, not bigger lines** (`RegionSelector::resized()`). It is a shape
  handle (`resizeHandleIndex()`, a `RegionChangedItem` like a corner drag), not a scale handle: it stretches
  the outline about its top-left corner in the region's frame, freely in x and y, and leaves origin, angle,
  pitches and the ink alone - scaling ink under lines that did not scale would lift it off them. Line
  spacing is the panel's slider. `RulingRegionParams::transform()` still scales pitches, for transforms
  that move the ink with the region.
- **Every ruled tool asks `Page::rulingAt(point)`** (a `RulingFrame`: origin, angle, pitches), with the
  gesture's press point, never `yruling()`/`getLine(y)`. `ScribbleArea::gestureFrame` is fixed at the
  press, so a stroke that wanders out of a region keeps its lines; `gridFrame` is the same without the
  blank-page phase, for snapping. `Selection::ruling` carries it into ruled select, ruled erase, insert
  space and reflow, which work in the frame's local coordinates (`RuledRange` is local; for the page's own
  ruling local is the page shifted by `yRuleOffset`, so behaviour there is unchanged - the `testN` fixtures
  confirm it). Ink belongs to the ruling it sits on: a ruled gesture in a region acts on that region's ink
  only, and one on the page leaves regions' ink alone.
- **Tilted frames needed two things beyond the mapping**, both found by the test, not by reading:
  rotating an element's page bbox into the frame inflates it (a stroke along a tilted line gets a box three
  lines tall and fails `containedRuled`'s early reject), so `localElementBBox()` measures a path's own
  points in the frame; and `StrokeBuilder::calcCom()`'s heuristic leans on page "up" (first point, bbox
  top), which put a stroke along a tilted line half a line high - strokes committed in a tilted region get
  their com measured in the frame. `groupStrokes()` skips com alignment on tilted lines for the same reason.
- **Found by review, each a one-line fix that is easy to undo by accident:** `localElementBBox()` adds a
  *deferred* pending transform (`Element::pendingDeferred()`), or tilted reflow subtracts the drag offset
  twice; `Page::loadSVG()` moves regions to the front (a file may have ink ahead of one, and `regions()`
  stops at the first non-region); a region's node transform from an older build is folded into its
  parameters on load; `invalidateStroke()` drops the whole selection when a peer edits the selected region
  (its selection also holds ink, so it never empties); a mirror keeps local y along the image of y; the
  bookmark list's ruled selector opts out of the same-ruling rule (`sameRulingOnly`); ruled move steps in
  the *selection's* ruling, not the press point's; `findStops` keeps the page's old line count.
- The "..." button is a screen-unit chip (`drawRegionButtons`), shown only in select mode (or while a region
  is selected), so `ScribbleApp::setMode` repaints when select-ness changes.
- **The region panel floats beside the selected region**, not in the toolbar: a column of separate rounded
  panels (kind dropdown - Lined/Squared/Dotted in an arrow popup -, a spacing slider, Background, Level,
  Delete). `RegionPanel::calcOffset()` picks the side at *layout* time - right, left, below, above, else over
  the region at the right edge - because only then is the panel's own size known; `syncRegionRow()` (from
  every `refreshUI`, which pan and zoom also reach) hands it the region's and view's window rects and marks
  it `BOUNDS_DIRTY`. The kind is derived, not stored: dots if `dotRadius > 0`, squared if `xRuling > 0`.
  Squared and dotted keep `xRuling == yRuling`, so the one slider sets both. The slider is logarithmic
  (10-120) and **previews** a drag through `previewSelRegionParams()` (no undo item), then on the handle's
  `SDL_FINGERUP` puts the old parameters back and commits the result with `setSelRegionParams()` - the
  `RegionChangedItem` records whatever is on the region when it is made, so skipping the restore would
  make undo a no-op. Both call `doRefresh()`: panel clicks are not canvas input, which is what normally
  repaints, so without it edits only showed after the next pointer event.
- **A stroke begun in a region stays in it** (`RulingRegionParams::clampInside()` in `doMoveEvent`'s
  `MODE_STROKE`): each point outside is moved to the nearest point on the outline, so the pen slides along
  the edge like a ruler. Without it, a stroke running past the edge belonged to the region (by its centre)
  but lay partly outside its outline, where neither the region's ruled tools nor the page's could reach
  it. Strokes begun on the page may still cross into a region. Existing out-of-bounds ink is untouched.
- **Outline** is on for new regions (`fromRect()` sets it); a file or peer that does not say so still
  loads as off, so older regions keep their look. (`RulingRegionParams::outline`, `__rroutline`, written
  only when on) is a third child,
  `rr-outline`: the outline path stroked in the rule color at 2 px non-scaling, twice a rule line. It is
  on the `<regionchanged>` wire as `outline`.
- **Ruled insert space and ruled select pressed just outside a region still act on it** - within half
  the region's line height of its outline (`REGION_PRESS_SLOP`, `Page::regionNear`, the `nearLines`
  argument of `gestureFrame`). The press keeps its real position, so above the top edge it is on line -1:
  "that line and everything after" then includes the top line, and the drag is measured in the same
  frame, so nothing jumps. This needed `RuledRange::nlines()` clamped: "to the end" is `MAX_LINE_NUM`
  (INT_MAX) lines, a range starting at line -1 is one more than an int holds, and the overflow made
  every range starting above line 0 select nothing. Not covered by an automated test.

Known gaps: page-level ruled insert space does not move regions below it (vertical/horizontal insert space
do); content pushed past a region's edge by ruled insert space leaves it; a corner drag can make a
self-intersecting outline; the hover cursor for a relative-width marker uses the page's line height; regions
on PDF/scan pages keep the colors `setTheme()` leaves on those pages; no detection of a scan's lines yet
(`quaddetect`'s Hough pass could fill pitch and angle). Tested by `ScribbleTest::rulingRegionTest()`
(mutation-tested, see the file) and `runRegionTests()`.
