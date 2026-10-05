# Shapes

Parametric shapes - line, box, ellipse, polyline and curve - drawn with `MODE_DRAWSHAPE` and
edited with handles. Designed in `SHAPES_SPEC.md`; that document is the rationale, this is the summary.

**Five tools, three toggles.** Arrowheads and corner rounding are flags, not shapes: an arrow is a line
with `SHAPEFLAG_HEADEND`, a rounded box is a box with `SHAPEFLAG_ROUNDED`. Each toggle collapses what
would otherwise be a pair of near-identical tools, and they compose - a rounded polyline with a head
needs no third entry. `ShapeDef::allowsHeads`/`allowsRounding` say whether a toggle means anything for
the active shape; the options row disables it otherwise.

**The parameters are the document; the `Path2D` is a cache.** Every shape is a plain `SvgPath` whose
`Element` carries a `ShapeParams` descriptor, serialized as `__shape`/`__shapepts`/`__shaperx`/`__shapery`/
`__shapeflags` - the same custom-attribute convention `Element` already uses for `__comx`/`__timestamp`.
`Element::updateFromNode()` parses it and regenerates the path; `Element::setShapeParams()` is the only
way to change a shape, and it regenerates too. Nothing edits a shape's path directly.

- `shape.h`/`.cpp` (`ulib`-style, no app dependencies) hold the `ShapeDef` registry. Adding a shape is one
  table entry plus a `buildPath` and a `getHandles` function; the mode handlers, selector, serializer and
  undo item are written once against `ShapeDef`.
- **Plain `SvgPath`, never `SvgRect`.** `Element::isPathElement()` tests `type() == PATH`, and `SvgRect`
  returns `RECT`, which would silently fail a long list of guards (selection highlight, free erase, hit
  testing, width scaling). Widening that test would change behavior for every pre-existing `<rect>` in
  every document. The cost is ~30 lines of rounded-rect construction copied from `SvgRect::updatePath()`.
- **Degradation contract.** Any operation that cannot be expressed in the descriptor calls
  `Element::dropShape()` and leaves a plain path behind - currently only rotation of a box/rbox/ellipse.
  Without this a shape silently reverts to its pre-operation geometry the next time a parameter changes.
- **`applyTransform()` has a shape branch ahead of the path branch.** The generic non-rotating-scale branch
  mutates `m_path` in place and would leave `__shapepts` describing the old size; the shape would look
  right until the next parameter change regenerated it from the stale descriptor and snapped back. The
  shape branch maps the descriptor points and scales `rx`/`ry` instead. This is the failure the
  `shapeRoundTripTest()` check exists to catch.
- **`ShapeChangedItem` is a sync wire-format change, not just an undo item.** Undo items *are* the sync
  protocol, so it needs `serialize()`, a type constant (`SHAPE_CHANGE_ITEM`) and a matching `shapechanged`
  branch in `ScribbleSync::processItem()`. The `__shape*` attributes themselves ride along free inside
  `<addstroke>`, because sync round-trips nodes through `SvgWriter`/`SvgParser`.
- Shapes are excluded from `groupStrokes()`, which would otherwise rewrite their centre of mass as if they
  were handwriting, outside the undo system.
- `MODE_DRAWSHAPE` is appended at the end of the `scribblemode.h` enum (values are serialized to config);
  `shapeId` and `shapeFlags` are appended *after the pens* in `ScribbleMode::saveModes()` so older config
  strings still load. `shapeId` is stored as its **string** id, not its index - shapes have twice been
  folded into a parameter of another shape, and an index would have silently reassigned everyone's
  active tool each time that happened. `ShapeId`'s numeric order is therefore free.
- `shapeIdByStringId()` accepts the ids of shapes that have since been folded into a parameter -
  `arrow`, `rbox`, `rpolyline` became flags, `splinepoly` became tightness 1 - and returns the base
  shape plus the flags (and tightness) the old name implied, so documents written while those were
  still shapes keep loading as what they meant. They are rewritten in canonical form on the next save.
  This has happened twice, which is why the alias table is worth keeping rather than a one-off.
- Multi-point (polyline) is the only gesture that outlives one press/move/release cycle. The in-progress
  element lives on `ScribbleArea::shapeInProgress` and `finishShape()` is called from every escape route:
  cancel, tool switch (`ScribbleApp::setMode`), page change, and `reset()` (document close/save).

## The polyline family

Two shapes share their points, their handles and their arrowheads, and differ only in how the body
between the points is drawn:

| shape | body | overshoots? | passes through the points? |
|---|---|---|---|
| `polyline` | straight segments, filleted when `SHAPEFLAG_ROUNDED` | no | yes |
| `fitpoly` ("Curve") | relaxed towards B-spline knots, then smoothed | only at tightness 1 | only at tightness 1 |

**There was briefly a third, `splinepoly`, and deleting it is the point.** It was the interpolating
curve - Catmull-Rom straight through every point - and `fitpoly` at `tightness == 1` was measured to be
*bit-identical* to it: same point count, zero deviation, on open, closed, uneven-spacing, arrowheaded
and many-point cases. So the two tools were one tool at its two extremes, and asking the user to choose
between them up front was asking them to guess. `splinepoly` is now an alias that loads as a curve at
tightness 1; the choice is a handle drag on the shape instead.

- Rounding reuses `__shaperx` and adds a radius handle on the first interior corner. `rx` is the
  distance trimmed off each edge, the same thing it means for a rounded box; `filletTrim()` clamps it
  per corner to half the shorter adjacent segment, so neighbouring fillets can never overlap. `rx` is
  kept when rounding is switched off, so toggling it back on returns the radius you had.
- `addSplineBody()` uses **centripetal**, not uniform, Catmull-Rom. The two are the same curve on evenly
  spaced points, so this is invisible on a neat rectangle; it pays off on uneven spacing, which is what
  tapped-out points look like. Measured: a 4-unit segment between neighbours of 100 and 113 overshoots
  by 10.0 units under uniform and 0.6 under centripetal. `shapetest.cpp` pins this with that exact case
  - an evenly spaced fixture would not catch a regression to uniform. It does *not* remove the bulge at
  a sharp corner: passing smoothly through a right angle requires going outside it. That bulge is what
  lowering tightness buys you, and is why the knob is worth having at all.
- The curve is built by *relaxing* each interior point towards the uniform B-spline knot
  `(P[i-1] + 4P[i] + P[i+1])/6` and then running `addSplineBody()` through the relaxed points. That
  gives one knob, `ShapeParams::tightness`: 0 is the pure B-spline (roundest, follows the points
  least), 1 relaxes nothing and so *is* the interpolating curve, and in between trades roundness for
  fidelity. Building it this way also means it inherits centripetal parameterisation rather than
  repeating it. The two end points are never relaxed, so the ends stay put and the heads stay attached.
- **Tightness has a handle**, not just a preference - `curveHandles()` puts it on the point with the
  most slack (`fitLoosestPoint()`), sliding along knot -> point, so dropping it on the point gives 1
  and on the knot gives 0. Without it, changing one curve's character would mean a trip to
  preferences, which is what made two tools look necessary in the first place.
- Tightness is **per shape** (`__shapetight`), not a global setting, or the same document would render
  differently on another machine. The `shapeCurveTightness` preference is only the default for newly
  drawn curves, exactly as `shapeCornerRadius` is for `rx`.
- Open ends: `addSplineBody()` reflects a phantom point (`2*p0 - p1`), and the B-spline relaxation
  leaves the end points alone. Both make the curve's end tangent come out exactly along the first/last segment, which
  is what the arrowheads are oriented from, so a head can never sit skew to its own curve.

## The option toggles

`SHAPEFLAG_HEADSTART`/`HEADEND`/`ROUNDED` live in `ScribbleMode::shapeFlags` and are driven by three
toggles on the shape options row. Two behaviours worth knowing:

- If a single shape is selected, the toggles edit **it** (via `ShapeChangedItem`, so undo and sync
  follow) instead of only arming the next shape drawn - adding an arrow to an existing polyline does
  not mean redrawing it. `setSelShapeOptions()` masks by the shape's own `allowsHeads`/`allowsRounding`,
  so switching tools with a shape still selected cannot strip flags that shape legitimately carries.
- The head icons are reicon `arrow-left`/`arrow-right`, i.e. the same two files already shipping as
  `ic_menu_back`/`ic_menu_forward`.

## After drawing

With `shapeEditAfterDraw` on (the default), `ScribbleArea::editShapeAfterDraw()` leaves a newly
committed shape selected with its `ShapeSelector` handles up, so the parameters can be adjusted while
the shape is still the thing being thought about. It falls back to a `RectSelector` if the shape was
demoted on the way in.

### Into and out of edit mode without the Select tool

From the first day of real iPad use: a shape that snapped a little wrong should not cost a trip to the
Select tool and back, and edit mode should not be leavable only by drawing the next shape.

- **A finger tap on a shape or image selects it** (`ScribbleArea::doTouchTap()`), with the shape's
  parameter handles up (or the scale/rotate/crop handles for an image) and the selection popup, and the
  tool is left alone. It hooks into `ScribbleView::panZoomFinish()`'s click branch, so it only exists where
  a single finger *pans* - i.e. once a pen has been detected (`singleTouchMode` PAN), which is every iPad
  with a Pencil. Where touch draws, a finger is a pen and taps nothing. Links still win (`doClickAction()`
  runs first), a double tap is still zoom, and the two finger tap is still undo: it is handled by
  `ScribbleInput` before any single tap could be seen.
- **Only shapes and images are tap targets** (`touchTapTarget()`), topmost first; handwriting, page tags
  and ruling regions are not. A fingertip cannot pick one stroke out of a word, and a stroke tapped by
  accident while panning would lose the next pen stroke to `clearSelOnly`. A shape is hit within
  `TOUCH_TAP_RADIUS` (16 screen units) of its outline - not its inside, so writing inside a box and
  tapping there does not grab the box. An image is hit anywhere inside. Locked/hidden layers are skipped.
- **A finger tap anywhere else clears the selection** (any selection, not only a shape's); a tap on the
  selection or one of its handles keeps it.
- **The first shape-tool press outside a selection only deselects**, under the same `clearSelOnly` pref
  (default on) that already made the first pen stroke do so. Before, `MODE_DRAWSHAPE` cleared the
  selection *and* drew, so after `shapeEditAfterDraw` the only exit was another shape. Cost: drawing
  several shapes in a row takes an extra press (or a finger tap) between them; turning `clearSelOnly` off
  restores draw-through for both tools.
- `selectTapped()` calls `finishShape()` first, so an open polyline is committed (not selected) rather than
  left accepting points behind a selection.

`ScribbleTest::shapeTapEditTest()` drives this through `ScribbleInput` with touch events and
`singleTouchMode` PAN; it fails 5 checks with `doTouchTap()` stubbed out and `MODE_DRAWSHAPE` back on the
plain clear-and-draw path. It must set the input modes and config *after* `newDocument()`, which reloads
both (and frees the document's `cfg`). The agent display has no touch device, so only the shape-tool half
was checked live.

**Only the paths where the *user* finished the shape may select it.** `finishShape(select)` defaults to
`false`; only the two deliberate multi-point finishes (tapping the last point, tapping the first to
close) pass `true`. Every other caller - `doPressEvent`'s guard when another gesture starts, a tool
switch, a page change - is finishing the shape as a side effect and must not leave a selection behind.
The reason is a sharp edge in `doPressEvent`: its mode switch assigns `currSelection = new Selection(...)`
**without deleting what was there**, which is safe only because the earlier "clear selection depending on
mode" pass already ran. Anything that installs a selection between those two points gets leaked, and the
elements it held keep `m_selection` pointing at the orphan - so they render as selected, are absent from
`currSelection`, and cannot be deselected, moved or deleted. `shapeInterruptTest()` pins this by
asserting no element points at a selection other than the current one; it was verified to fail against
the broken version, not just to pass against the fixed one.

Relatedly, `doPressEvent` drops any live `currStroke` at entry. The in-progress element is held on
`ScribbleArea` rather than in the page, so a gesture abandoned without a release or cancel - the input
layer restarts one when the pen button changes mid-stroke - would otherwise leave a shape painted every
frame that belongs to no page.

Two deliberate deviations from the spec, both documented at the point of deviation:

- **Arrowheads are stroked open Vs in the same path, not filled paths inside a `<g>`.** The spec's reason
  for the group was that a filled outline would bake the stroke width into the geometry - which cannot
  happen here, because the path is regenerated from the descriptor on every change. Keeping an arrow as
  one `SvgPath` means it never becomes a multi-stroke element and carries no filled part that
  `Element::scaleWidth()` could misread as pen geometry.
- **Regeneration is driven by `setShapeParams()`, not by `SvgNodeExtension::onAttrChange()`.** The
  descriptor is held in the `Element` and written to attributes only on serialize, so a handle drag does
  not round-trip through strings on every move event.

Known gap: shapes carry no pen class, so `Element::toPenPoints()` returns nothing for them and the **free
eraser does not affect a shape** - the stroke eraser deletes it whole. Giving them `STROKE_PEN_CLASS`
would make free erase work but would split curved shapes (ellipse, rounded box) at Bezier control points.

## Hold to snap (shape recognition)

With the pen tool, **hold the pen still at the end of a stroke** for `shapeSnapDelay` seconds and the ink
becomes the line, rectangle or ellipse it looks like; the rest of the gesture then scales it until the pen
lifts. A **scratch-out** (back and forth over something) erases what it was drawn over instead - held,
or, by default, as soon as the pen lifts (below). The
delay is 0.5-1.5 s, 0 turns it off (default 0.8), and is in Preferences > Shapes plus the Pen and Shape
Settings buttons. The recognizer is `syncscribble/shaperec.*` (+ `shaperecgeom.h`); its tuning, test set
and gates live in `labs/shape-recognition/`, whose `make test` compiles **this** copy - run it after
touching a threshold. It has two gated suites, and the **pencil** one (240 Hz, drifting hold, light
arcs, wide zigzags, sloppier closed shapes) is the one that found the recognizer too strict: a line
bowed over 10% of its length, a circle stopping 55 degrees short and any zigzag leaning past 45 degrees
were all refused. 240 Hz sampling by itself cost nothing.

- **A scratch-out's scrub axis has a fallback.** The mean chord direction is wrong for a wide zigzag
  (half of a line scratched out at speed), so when it fails other axes are tried - but the mean always
  goes first, and a fallback axis must carry 40% of the motion, or the wobble across a plain line reads
  as reversals and the line erases what it crosses. See the lab README before touching either.

- **Two scratch-out strengths.** The *hold* is eager (`Params` defaults: turns up to 0.3 of a pass, the
  loose long-stroke rule from 12 reversals, zigzags 3x wider than high) - holding says "erase", so it
  need not survive writing, and ~1.2% of other strokes erase. **Scratch out without holding**
  (`liftScratchOut` on/off, `liftScratchOutLevel` 1 careful / 2 normal; Pen Settings, Shape Settings and
  Preferences > Shapes) runs
  `recognizeScribble(liftParams(level))` as the pen lifts, over every stroke written: 6 or 8 passes, turns
  0.2, long rule from 24 - 0.07% / 0 false erases. Short scratch-outs are left to the hold on purpose.
  The lift path needs `shapeSnapDelay` > 0, since that is what collects `snapSamples`.
- **Long scratch-outs are judged more loosely.** Real ones over a line of text (25-45 reversals, loops,
  arches, spikes) always have a short pass or a turn over the gap limit: pass length and gap ignore
  their worst 20-25%, and long strokes get a wider turn limit. On lift, don't start that rule below 24
  or loosen the base 0.2 - long mmm arches then erase as you write.
- **Real strokes:** config `recordShapeStrokes` = 1 logs every held stroke to `shape-strokes.strokes` in
  the library (lab format, plus what it was recognized as). Wrong ones, labelled, go in
  `labs/shape-recognition/fixtures/`, where `make test` gates their class.

- **The hold is a timer, not an event.** A pen held perfectly still sends no motion at all, so a callback
  timer (`SvgGui::setTimer`, its own rather than the widget's, which autoscroll and fling share) polls
  `checkShapeSnap()`. "Still" is within `SNAP_HOLD_RADIUS` (6 screen units) of where the hold started -
  loose on purpose; the recognizer trims the hold cluster itself. One recognizer run per hold: a stroke
  that is not a shape stays ink and keeps drawing, and moving on is how to try again.
- **The recognizer sees screen units** (page coords times `mScale`), because its absolute thresholds
  were tuned in screen pixels; results are mapped back. It is fed the raw samples, not the stroke
  builder's filtered path.
- **The live shape is `currStroke`**, which is what the shape tool's drag gesture uses - so it is painted
  every frame and every escape route (the abandoned-gesture guard in `doPressEvent`, cancel) already
  cleans it up. `snapActive` says `currStroke` is a snapped shape and the stroke builder is gone.
- **Scaling:** a line's end follows the pen; anything closed is scaled uniformly about its centre by the
  ratio of the pen's distance from that centre now to when it snapped.
- **History:** the shape *as recognized* is always one undo step. If the gesture then moved any point
  more than `SNAP_CHANGE_MIN` (6 screen units), the change is a second step (`ShapeChangedItem`), so undo
  returns to the snapped shape before removing it. Below that it is one step - lifting the pen jitters.
  The add is ended as its own action before the params change, so sync sends the snapped shape first.
- **Mapping:** line -> `SHAPE_LINE`; axis-aligned rectangle -> `SHAPE_BOX`; a tilted one -> closed
  `SHAPE_POLYLINE` (a box cannot rotate); circle or axis-aligned ellipse -> `SHAPE_ELLIPSE`; a tilted
  ellipse -> closed `SHAPE_CURVE` at tightness 1 through 8 points on it. The shape tool's arrowhead and
  rounding toggles are *not* applied - they are armed for the shape tool, not for handwriting.
- **Scratch-out** erases every element whose bbox *centre* is inside the scratch-out's hull (an overlap
  test would take strokes it merely grazed), through `Selection::doSelect()` so locked and hidden layers
  are respected, as one undo step. Images are never taken. It happens the moment the hold fires; the rest
  of the gesture is `MODE_NONE`.
- **The shape is not left selected** after the pen lifts, unlike the shape tool's `shapeEditAfterDraw`:
  `clearSelOnly` is on by default, so a selection would swallow the next handwriting stroke.
- Disabled for ephemeral, line-drawing, snap-to-grid and centre-on-line pens.

Tested by `ScribbleTest::shapeSnapTest()`, which fires the hold by calling `checkShapeSnap()` with a
time past the delay (the suite runs with `shapeSnapDelay` 0, so the wall-clock timer never interferes).
Mutation-tested: no second history step, always two, no scaling, a scratch-out that erases nothing, one
that erases by overlap, and ignoring the delay each fail it. The real timer path was checked live with
`agent-pointer stroke ... @1200` (see agent-display in CLAUDE.md).

## 45 degree angle snap

`snapShapeAngle()` (`shape.cpp`) is a **soft** snap: a line or polyline-family point whose segment to a
neighbour is within `shapeAngleSnap` degrees (default 8, 0 off, Preferences > Shapes) of a multiple of
45 degrees is projected onto that direction; an interior point in range of both neighbours goes to where
the two snapped directions cross, so a corner locks to a right angle. Soft rather than the pen button's
hard `applyShapeConstraint()`, so a line deliberately drawn at 30 degrees stays there.
Skipped for snap-to-grid pens, which would be pulled off the grid. Tested in `shapetest.cpp`.

**Editing snaps by distance, not angle.** An angle-only rule pulls a long line's end across a long way
(8 degrees at 800 px is 112 px), so a long line could never sit slightly off diagonal - the first-day
report's complaint - while a short one barely moves. So `snapShapeAngle()` takes an optional `maxDist`:
pos must also lie within that perpendicular distance of the snapped line through the neighbour.
`ScribbleArea::snapShapeAngleAt()` passes `SHAPE_ANGLE_SNAP_DIST` (8 screen units) / `mScale`, so it
follows zoom - zoom in for finer control - and is used for the shape tool's drag, each placed polyline
point and `POINT` handle drags. Handle drags work in node-local units (`ShapeSelector::toLocal`), so the
distance is converted through that transform (`localPerPage`), else a scaled shape would snap at the
wrong distance. The angle stays a cap, so `shapeAngleSnap` 0 still turns it off and a short line is
never bent further than before; with both limits, lines up to ~57 px (8 / tan 8 degrees) behave as they
did, longer ones need their end within 8 px of the axis line.

**Recognition stays angle based**, at `RECOGNIZED_ANGLE_SNAP_FACTOR` (0.75, so 8 -> 6 degrees) of the
setting, via `snapRecognizedAngleAt()`, for the recognized line only: a recognized stroke is rough
intent, so an angle fits. Once snapped, dragging the end with the pen still down (`scaleSnapShape`) is
editing and snaps by distance like the handles (user decision): it starts from the already snapped end,
so jitter stays inside `SHAPE_ANGLE_SNAP_DIST` and only a deliberate move off the axis unsnaps.
- **Draw through a selection (`shapeDrawThrough`, default off):** the shape tool's press outside a selection only clears it when `clearSelOnly` is on, like the pen; with this on it clears and starts the shape. Pen unchanged. In Pen and Shape Settings.
