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
- **On its layer, over the images bigger than it.** A region gets the layer it was made on (`addStroke`
  stamps the current layer like any element, `__layer` is written like any element's). Across layers it
  follows layer order: over every lower layer, under every higher one. Within its own layer's run,
  `Page::regionInsertPos()` puts it just above the topmost image of the run that is **bigger than it**,
  or at the bottom of the run if there is none - above regions already there, below every other
  element. So a patch laid on a scan draws over the scan, and a sticker or photo smaller than the patch
  still sits on it; ink of the layer stays over the patch.
  - **"Bigger" = area** (`Page::imageUnderRegion()`): the image's bounding-box area against the area of
    the region's own outline (shoelace over its corners, so a tilted patch is not inflated by its bbox);
    a tie counts as smaller (image on top). Area, not "the image covers the patch": a patch laid over a
    scan but poking past its edge would otherwise go *under* the scan and vanish, and an area test does
    not depend on position, so moving an image or patch never restacks - only adding, removing,
    resizing or changing layer does. Only top-level `<image>` elements count (not an image in a group),
    as in `layerFirstElement`.
  - **Only regions move.** Ink and images keep the order the user gave them; the regions' place is
    derived. `Page::restackRegions(layer)` re-places a layer's regions by the rule (document order kept
    among regions that land at the same spot; a no-op when already in place), outside the undo system,
    and every path that can change the outcome calls it or `restackRegionsFor(s)` (images and regions
    only): `addStroke`/`removeStroke` (create, paste, insert, Edit Image's replace), `moveToLayer` (both
    layers), `Selection::commitTransform` and smart scale (a scale), the region panel and size handle
    (`setSelRegionParams`, `MODE_SHAPEHANDLE` release), the undo items (`StrokeAdded`/`Deleted`,
    `StrokeTransform`, `RegionChanged`; `StrokeLayerItem` via `moveToLayer`), which sync and persisted
    undo replay too, and page load. Since the order is a function of the state, every client and every
    undo/redo arrives at the same one.
  - **Recorded siblings skip regions.** An undo item's `next` for ink or an image is taken past the
    layer's regions (`skipLayerRegions()` in `removeStroke` and `moveToLayer`), because a restack may
    move them before the item replays; replay goes through `Page::restorePos()` (`reinsertPos()` in
    `syncundo.cpp` and `moveToLayer` with a `next`): a region goes back beside a same-layer region it was
    recorded before, else where the rule puts it; anything else goes before its sibling, past regions,
    and to the end of its own run if the sibling is now on another layer (it was the first element of
    the layer above, possibly a region that has since moved up inside its own run).
  `layerFirstElement` skips a layer's regions as it skips its images, so draw-under lands above the
  patch's paper. `regions()`/`regionAt()` scan the whole page, as regions are no longer contiguous;
  `regionAt()` ignores a hidden layer's region (not drawn, so it rules nothing). The "..." button and
  insert space obey the layer's lock and visibility (`Page::isRegionEditable()`).
  *Why:* regions used to be on no layer (`REGION_LAYER`, -2, `zIndexOf() == -1`), always first in the page.
  That put every patch below **every** image, so a patch drawn on an upper layer vanished behind a scan
  on a lower (typically locked) layer - the very case a patch exists for. Then (47bf513) a patch went to
  the bottom of its own layer, so on the scan's own layer it still drew under the scan; the size rule
  fixes that while keeping small images on top. Pinned by the layer block at the end of
  `ScribbleTest::rulingRegionTest()` (big image under, small image over, undo/redo, resize past the
  images). An older file's region has no `__layer`, so it lands on the default layer and is restacked
  by the rule on load.
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
  it `BOUNDS_DIRTY`. The kind is derived, not stored: music if `staff` (its `yRuling` is a staff band, see paper.md), dots if `dotRadius > 0`, squared if `xRuling > 0`.
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
- **The red origin handle can be relocated** (`RegionSelector::originHandlePos()`, `moveGripPos()`): a small
  hollow grip tied to it by a stem (up-right, screen units) drags the handle anywhere inside the outline, so
  it can sit on something visible in the scan and the offset be dragged from there. Relocating is shape
  handle index `n+2` (`moveGripIndex()`); it changes no parameter and makes no undo item (the move case
  clears `regionHandleStart`, which is what release uses to decide to add a `RegionChangedItem` - do not
  drop that). Dragging the handle itself still slides the origin, and a relocated handle rides along with
  the pointer. **Transient by decision**: the position lives in the selector (page coordinates, mapped by
  `transform()` when the region moves, clamped inside the outline by `clampInside` at use), so deselecting
  resets it to the left edge. Storing it would need an attribute plus a `<regionchanged>` field for no
  persistent meaning. The standalone regiontest does not cover it (selector needs the app); checked with
  agent-display. The grip is black-on-white/bgStroke like the rotate grip, no new colors.

- **Spacing changes scale about the red handle** (`RulingRegionParams::setPitchesAbout()`, the slider and
  the kind menu's pitch conversions): the origin is moved so the handle keeps its local position *in
  pitches*, i.e. the lines keep their phase at the handle - a line through it stays through it, a handle
  mid-band stays mid-band. Before, origin stayed put, so the lines scaled about the region's top edge and
  slid off whatever the handle had been lined up with. Two traps: the slider computes every step from
  `regionSlideStart`, never from the previous step (the integer-rounded pitches would creep the phase), and
  `ScribbleArea::pinSelRegionHandle()` pins a *default* handle at its current spot before the first step -
  the default sits on "the first line", which a finer pitch replaces with a new first line above, so
  unpinned it would hop. Exactness limits: an axis whose old pitch is 0 (lined -> squared) has no phase,
  so x stays as it is; the outline does not scale (the size handle still only makes more paper, above).
  Mutation-checked in `regiontest.cpp`.
- **Coordinate System** (`RulingRegionParams::axes`, `__rraxes`, `axes` on `<regionchanged>` and in undo
  persistence): a squared grid plus x and y axes through `origin`, which is the plot's (0, 0). Made from the
  "+" menu's Coordinate System entry (same drag tool as Patch, `ScribbleMode::drawAxes`; origin starts at
  the box's centre) or the kind dropdown (origin moved to the centre on switching to it). Cell = the grid
  pitch (the slider); ticks and numbers every `AXIS_TICK_CELLS` = 2 cells, so grid 21 ticks every 42. Math
  orientation: y grows *up* the page (local -y), negative numbers use U+2212. The red handle *is* the origin
  (`defaultOriginHandlePos` returns it, clamped inside), so dragging it moves the axes; the relocation grip
  is hidden and `handleMoved` ignored for axes - a handle that was not the origin would plot nothing. Drawn
  as two more children: `rr-axes` (path: axes, open arrowheads, ticks; 3 px non-scaling, 3x a grid line) and `rr-labels`
  (a `<g>` of `<text>`, upright in the frame so they tilt with the patch, `satoshi` like page tags), both
  in the page's rule color made opaque - the theme's rule color, never a literal, and opaque so the axes
  read above the translucent grid. Numbers whose anchor falls outside the outline, and ticks within an
  arrowhead's length of the tip, are dropped. `sanitize()` keeps it squared and dotless and a staff never
  has axes. Geometry (tick step, y up, clipping, sanitize) tested in `regiontest.cpp`, mutation-checked.

- **Corner drag snaps softly back to a rectangle** (`snapCornerToRect()`, `REGION_CORNER_SNAP_SCREEN` = 5
  screen units / `mScale`, so a few pixels at any zoom). Relative to the *neighbouring corners*, measured in
  the region's frame (so tilted patches work), never absolute x/y: the neighbour whose edge runs more nearly
  along x gives the dragged corner its local y, the other gives its local x, each axis only when within
  tolerance. Two such snaps make a rectangle if the other three corners were one. Only four-corner outlines.
  Tested in `regiontest.cpp` (mutation-checked: stubbing the function fails 7 checks).
- **Handles are tested before pan-from-edge** (`doPressEvent`: `selectionHit` now runs before the
  `panFromEdge` switch, and a handle hit skips it). The size handle sits past the bottom-right corner, so
  for a patch spanning the page it is off the page and, with `panFromEdge` on (default on phones/tablets;
  2 = any press off the page) the press became `MODE_PAN`, and the pan's tap then cleared the selection.
  All handles (shape, scale, rotate, crop) are hit-tested in page coordinates with no page-bounds check, so
  they work off-page once the edge pan is out of the way. Not covered by an automated test (needs the app).

## Convert to Subpage

Overflow menu > **Convert to Subpage...** (`actionConvert_Subpage` -> `ScribbleApp::convertToSubpage()` ->
`ScribbleDoc::convertToSubpage(pagenum, props)`) puts the current page, as it looks, onto a bigger new page of
another ruling, so there is room around it for notes.

- **The ruling is chosen with the Add Page chooser itself**, not a copy: `AddPageMenu::chooseLayout(title,
  onChosen)` hosts the same `LayoutChooser` (recent row with "Same as previous" = the current page's ruling,
  "+" for the full grid, Margin line, right-click to edit a layout) in a `createPopupDialog` with Cancel. The
  popup and the dialog differ only in their `pick`/`close` callbacks (`addpagemenu.cpp`). A dialog keeps the
  size of its first layout, so "More" calls `onViewChanged`, which sets the window bounds to 0 x 0 to make it
  fit the grid (the RulingDialog trick); without it the grid is clipped to the recent row's size. A pick goes
  into `recentPageLayouts` like an Add Page pick.
- **The old page becomes a Paper Patch** over its own outline (`fromRect(orig->rect())`) with its ruling
  phased as on the page (`origin` = `pageFrame().origin`, i.e. including the blank-page `yRuleOffset`),
  outline on. **A page with an image background** (`hasImageBackground()`: PDF, scan, Noteful) has no ruling a
  patch could draw, so each `<image>` of its rule layer comes across as an ordinary image element instead; it
  keeps `write-pdf-background`, so it keeps its import resolution on save (`NO_RESAMPLE_CLASS`). It becomes
  selectable/erasable ink-layer content, which is the point - it is a picture on the page now.
- **Placement** is `subpagePlacement()` (`rulingregion.cpp`, tested standalone in `regiontest.cpp`,
  mutation-checked): the new page is at least the chosen layout's size and grows to hold the old page with
  `SUBPAGE_MARGIN` (75 units, 1/2 inch) on every side; the old page is centred across and `margin` from the
  top, both edges moved out to the new page's own lines (top on a rule line, left on a grid line) so the patch
  lines up with the paper around it; snapping can eat into the far margins, so the page grows again after.
- **Layers and order**: the patch (or image) goes on the **bottom layer** (`layers.byIndex(0)`), added first;
  then every element of the old page is cloned in document order onto *its own* layer (`addStroke(clone,
  NULL, layer)`), shifted by the placement (`applyTransform` + `commitTransform`, as
  `Selection::stealthTransform`; a region's branch maps its parameters). So ink on the bottom layer stays over
  the patch, higher layers stay higher, and patches already on the page restack over the new one by the usual
  rule. The outline entry is copied; page tags and bookmarks are elements and come along.
- **One undo step, sync for free**: the new page is built completely while no action is open (with
  `page->document` set so `addStroke` can read the layer table; nothing is recorded since `undoable()` is
  false), then swapped in by `deletePage` + `insertPage` in one action. Undo restores the original `Page`
  object. Building the page *after* inserting it would be wrong for sync: `<addpage>` serializes its page's
  content when it is sent, so stroke items on top would arrive twice. Trap: `setOutlineEntry()` is called
  before `page->document` is set, since with a document `ensureLoaded()` runs the memory check for a page that
  is not in it.
- The patch's colors are the new page's paper and rule colors (`onAddStroke` rebuilds a region with its
  page's colors, as on undo and sync) - the theme's, same as the old page's in practice.
- Tested by `ScribbleTest::subpageTest()` (lined page: size, patch rect/ruling/phase/layer, ink offset and
  layer, ink ruled by the patch, outline kept, undo/redo; PDF page: image not patch, class kept, positions,
  undo). Mutation-checked: dropping the shift fails 5 checks. Checked in agent-display for a lined page with
  strokes onto Medium squared, and a PDF-style page onto the default lined layout, with undo.
- Known gaps: the old page's **margin line** is not reproduced (a region has no margin); a custom ruling
  that is not an image (pdf2write's vector rulings, an inherited custom rule layer) falls back to the page's
  standard ruling numbers; the old page's own paper color, if it differed from the theme, is not kept.

Known gaps: page-level ruled insert space does not move regions below it (vertical/horizontal insert space
do); content pushed past a region's edge by ruled insert space leaves it; a corner drag can make a
self-intersecting outline; the hover cursor for a relative-width marker uses the page's line height; regions
on PDF/scan pages keep the colors `setTheme()` leaves on those pages; no detection of a scan's lines yet
(`quaddetect`'s Hough pass could fill pitch and angle). Tested by `ScribbleTest::rulingRegionTest()`
(mutation-tested, see the file) and `runRegionTests()`.
