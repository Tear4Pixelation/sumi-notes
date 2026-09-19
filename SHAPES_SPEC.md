# Shapes Tool — Specification

Covers two phases of one feature:

- **Phase 1 — Shape tool.** Draw arrows, boxes, rounded boxes, ellipses/circles and lines
  by dragging on the canvas.
- **Phase 2 — Editable shapes.** Select a shape and edit its defining parameters with
  handles, including the corner radius of a rounded box.

They are specified together because Phase 1's data model *is* the thing Phase 2 edits.
Designing them separately would mean designing the shape representation twice. The same
model is later what the shape recognizer emits, so it should be settled here.

---

## 1. Core decision: parameters are the source of truth

Every shape is an `Element` whose `SvgNode` carries a **shape descriptor** in custom
attributes, and whose rendered `Path2D` is *generated* from that descriptor. The geometry
is a cache; the parameters are the document.

```
__shape      = "arrow" | "box" | "rbox" | "ellipse" | "line" | "polyline"
__shapepts   = "x0,y0 x1,y1 ..."      defining points, shape-specific meaning
__shaperx    = <real>                  corner radius x      (rbox)
__shapery    = <real>                  corner radius y      (rbox)
__shapeflags = <int>                   bit flags: closed, headStart, headEnd, smooth
```

### Why custom attributes rather than native SVG shape nodes

This is not an arbitrary choice — native round-trip is inconsistent in this codebase:

- `SvgRect` **does** round-trip natively. The parser builds one from `x/y/width/height/rx/ry`
  (`svgparser.cpp:732–739`) and `SvgWriter::_serialize(SvgRect*)` writes those attributes
  back (`svgwriter.cpp:441`).
- **Ellipses do not.** The parser produces `new SvgPath(Path2D().addEllipse(...),
  SvgNode::ELLIPSE)` (`svgparser.cpp:510`) — an `SvgPath` carrying an `ELLIPSE` *pathType*,
  not a distinct node class. The writer dispatches on `type()`, which is `PATH`, so it is
  written out as `<path d="...">` with the curve data baked in. The parametric ellipse is
  lost on save.
- **Arrows and polylines have no native SVG node at all.**

So a "use native nodes" design would round-trip one shape out of five. Custom attributes
give uniform behavior, and the convention already exists in this file format — `Element`
stores `__comx`, `__comy` and `__timestamp` exactly this way (`Element::serializeAttr`,
read back in `Element::updateFromNode`).

### Node type: use `SvgPath` for every shape, including boxes

Tempting to build boxes as `SvgRect`, since its `updatePath()` already implements
rounded-corner geometry with correct radius clamping. **Don't.** `SvgRect::type()` returns
`RECT`, and `Element::isPathElement()` tests `type() == SvgNode::PATH`, so an `SvgRect`
silently fails a long list of guards — see §7.1 for the full blast radius.

Widening `isPathElement()` to accept `RECT` looks like a one-line fix but changes behavior
for **every** existing `RECT` node in every document, not just new shapes — including the
invisible hit-target rect that `Element::serializeAttr()` generates for hyperlinks and any
`<rect>` in an imported SVG. That is a much larger change than it appears.

Building every shape as a plain `SvgPath` avoids the problem entirely: `isPathElement()` is
true with no change to it, all existing guards behave, and the descriptor stays the single
uniform source of truth. The only cost is reimplementing rounded-rect path construction —
about 30 lines, and `SvgRect::updatePath()` can be copied directly, including its
`min(w,h)/2` radius clamping.

### Load path

`Element::updateFromNode()` is extended to parse the `__shape*` attributes and, when
present, regenerate the path. `SvgNodeExtension::onAttrChange(const char*)` — already
wired, `svgnode.cpp:333` — is the hook for regenerating whenever a parameter changes, so
editing is "set attribute, geometry follows."

### Degradation contract

A shape is parametric **only while every operation preserves its parameters**. Any
destructive operation that cannot be expressed in the parameters must **drop the
descriptor and leave a plain path behind**. This keeps a rounded box from silently
un-erasing itself the next time a parameter changes.

This idiom is already in the codebase: `Element::applyTransform` demotes a circle to a
generic path when scaled non-uniformly — `pathnode->m_pathType = SvgNode::PATH`
(`element.cpp:598`). Same principle, applied consistently.

Demote on: free erase, stroke-eraser splitting, rotation (for box/rbox — a rotated
axis-aligned rect is not representable in the descriptor), and ungroup.
Do **not** demote on: translate, uniform or axis-aligned scale, color/width change.

---

## 2. Shape catalog

One table entry per shape. `p[]` is the `__shapepts` list.

| id | Params | Creation gesture | Notes |
|---|---|---|---|
| `line` | `p[0]`, `p[1]` | drag vector | |
| `arrow` | `p[0]` tail, `p[1]` head | drag vector | `headEnd` flag set; `headStart` gives double-arrow |
| `box` | `p[0]`, `p[1]` opposite corners | drag bbox | axis-aligned |
| `rbox` | `p[0]`, `p[1]` + `rx`, `ry` | drag bbox | radius defaults to a config value, clamped to `min(w,h)/2` |
| `ellipse` | `p[0]`, `p[1]` bbox corners | drag bbox | constrain modifier → circle |
| `polyline` | `p[0..n]` | multi-point | `closed` flag → polygon; heads allowed on open ends |

`box` is `rbox` with `rx == ry == 0`. Keep them as separate menu entries but one
implementation.

### Arrowheads

Head length scales with **stroke width** (suggest `4–6 ×`), never with shaft length, or
long arrows grow absurd heads. Head direction comes from the last segment's tangent.

Representation: a `<g>` holding a stroked shaft path plus a filled head path. A single
filled outline would bake the stroke width into the geometry — the same trap that makes
pressure strokes hard to edit — and would defeat Phase 2.

**This has a consequence, see §7.2:** groups hit `Element::isMultiStroke()`.

---

## 3. Extensibility

Adding a shape must not mean touching the event handlers. Define a registry:

```cpp
struct ShapeDef {
  const char* id;                  // serialized in __shape
  const char* icon;                // toolbar icon resource
  ShapeGesture gesture;            // DRAG_VECTOR | DRAG_BBOX | MULTIPOINT
  int minPoints, maxPoints;        // maxPoints < 0 = unbounded

  Path2D (*buildPath)(const ShapeParams&);
  void   (*getHandles)(const ShapeParams&, std::vector<ShapeHandle>&);
  void   (*applyConstraint)(ShapeParams&);   // modifier held: square/circle/45°
};
```

`buildPath` and `getHandles` are the only shape-specific code. The mode handlers, the
serializer, the selector and the undo item are all written once against `ShapeDef`.

A new shape = one table entry plus two small functions. Later, user-defined shapes could be
loaded as SVG snippets from resources (`resources.cpp` / `res_ui.cpp` already embed SVG this
way) with a generic bbox-fit builder, without further engine changes.

---

## 4. Creation interaction

New mode `MODE_DRAWSHAPE`, appended **at the end** of the enum in `scribblemode.h` — the
file warns that values are serialized to config and must only be added at the end. Active
shape id lives on `ScribbleMode` next to `drawTool`.

### Drag gestures (`DRAG_VECTOR`, `DRAG_BBOX`)

- **Press** — record `initialPos`, create the live `Element`. Do **not** route through
  `StrokeBuilder`; that pipeline is for sampled input. Hold an `Element*` on `ScribbleArea`,
  the way `MODE_BOOKMARK` already does.
- **Move** — update params from `(initialPos, pos)`, rebuild the path, call
  `scribbleDoc->updateCurrStroke(dirty)`.
- **Release** — commit inside `startAction`/`endAction` via `currPage->addStroke()`.

Live preview is nearly free: the in-progress object is already painted by
`SvgPainter(painter).drawNode(...)` at `scribblearea.cpp:2733`, and a live shape element
draws through the identical call.

**Constrain modifier:** `MODEMOD_PENBTN` is already the "constrain" convention elsewhere
(15° rotation steps). Held → square, circle, or 45°-snapped vector. Grid snapping reuses
`Page::xruling()` / `yruling()`.

### Multi-point gesture (`MULTIPOINT`)

- Tap places a point. Live preview follows the cursor from the last placed point.
- **Tap the last placed point again to finish.** Hit radius should be generous —
  suggest the same `PATHSELECT_RADIUS/mZoom` scaling used elsewhere, so it works at any zoom.
- Also finish on: Esc / Enter (desktop), tool switch, or a "done" button in the popup
  toolbar. The popup mechanism exists — `cfg->Bool("popupToolbar")` plus
  `app->showSelToolbar(...)` — and on mobile it will be the one that actually gets used.
- Tapping the *first* point closes the shape (sets `closed`) and finishes.

**This is the first mode that outlives a single press→move→release cycle.** Every existing
mode resets `currMode` per gesture. Multi-point needs persistent in-progress state on
`ScribbleArea` plus teardown on every escape route: `doCancelAction()`, tool switch, page
change, document save/close, and window close. That bookkeeping — not the geometry — is the
real cost of this gesture, which is why §8 schedules it after the drag gestures.

---

## 5. Editing (Phase 2)

New `ShapeSelector : Selector`, chosen instead of `RectSelector` when the selection is
exactly one shape element. `Selector` already declares the needed virtuals — `drawBG`,
`scaleHandleHit`, `rotHandleHit`, `cropHandleHit`, `setZoom`, `transform`.

### Handles

- **Point handles** — one per entry in `__shapepts`. Covers line/arrow endpoints and every
  polyline vertex.
- **Box handles** — the four corners, for `box`/`rbox`/`ellipse`.
- **Radius handle (`rbox`)** — the conventional placement: a small handle inset from the
  top-left corner along the diagonal. Dragging it sets `rx`/`ry`. Clamp to `min(w,h)/2`;
  `SvgRect::updatePath()` already clamps internally, but the handle should clamp too so the
  handle position never disagrees with what is drawn.
  - Uniform `rx == ry` by default. A non-uniform axis-aligned scale of an `rbox` makes them
    diverge, which is representable and should be preserved rather than re-uniformed.

Handle hit-testing and drawing follow `RectSelector` (`selection.cpp:748–800` for hits,
`1320–1350` for drawing), including the `HANDLE_PAD` touch-vs-pen distinction.

### Editing color, width, dash

Free. `Element::getProperties()` / `setProperties()` operate on the node's `fill` /
`stroke` / `stroke-width` attributes, so the existing selection toolbar works on shapes with
no new code. `ScribblePen` already carries `dash` and `gap`, so dashed shapes come along
for the ride.

---

## 6. Undo and sync

Every mutation must be an `UndoHistoryItem`, or the shared-whiteboard sync silently misses
it (`ScribbleSync` is built on the undo change log).

- **Creation** — `StrokeAddedItem`. Nothing new needed.
- **Parameter edit** — **needs a new item.** The existing `StrokeChangedItem` stores a
  `StrokeProperties`, which is only colour + width — it cannot carry geometry. Add a
  `ShapeChangedItem` holding the previous `__shape*` descriptor and swapping it, mirroring
  `StrokeChangedItem::swapProps()`.
- **Move / scale / rotate** — existing `StrokeTranslateItem` / `StrokeTransformItem`, with
  the §1 demotion rule applied for rotation of box-like shapes.

Coalescing: a handle drag should produce **one** undo item for the whole drag, not one per
move event. Follow the existing pattern — mutate live, commit on release inside a single
`startAction`/`endAction`.

---

## 7. Integration traps

These are specific, verified, and each one produces a real bug if missed.

**Checked and found safe**, so they need no special handling:

- **Clipboard copy/paste** — `Clipboard::paste` reuses an existing ext or constructs
  `new Element(m)`, so `updateFromNode()` runs and descriptors survive.
- **`Element::cloneNode()`** — clones the node *and* its extension.
- **Sync transport of `__shape*` attributes** — rides along free (§7.5).
- **PDF export, including grouped arrows** — `PdfWriter` dispatches `SvgNode::G` to
  `_draw(const SvgG*)` → `drawChildren()` (`pdfwriter.cpp:368`, `612`), and `RECT` falls
  through to the `PATH` case, so both stroked and filled shape parts export correctly.
- **Dashed shapes** — `stroke-dasharray` is a plain attribute (`strokebuilder.cpp:87`), so
  `ScribblePen::dash`/`gap` apply to shapes with no new mechanism.

Everything in §7 below has been verified against the source. The one area deliberately left
unaudited is the interaction between shapes and the *clippings* document
(`clippingview.cpp`), which is a niche path and cheap to check later.

### 7.1 `isPathElement()` excludes `SvgRect`

`Element::isPathElement()` is `node->type() == SvgNode::PATH` (`element.h:163`), but
`SvgRect::type()` returns `RECT`. A box built as an `SvgRect` silently fails the guard in:

- `Element::freeErase()` (both overloads) — free eraser does nothing to it
- `Element::getEraseSubPaths()` and the free-erase branch at `scribblearea.cpp:2062`
- `Element::scaleWidth()` — stroke width won't scale when resizing the selection
- `Element::applyStyle()` (`element.cpp:683`) — **the selection highlight never draws**
- `selection.cpp:650` hit test — falls back to `return true` (whole-bbox hit), so the stroke
  eraser and path-select grab the box from anywhere inside it
- `selection.cpp:1215` `isEnclosedBy` — bbox test instead of true containment
- the ruled-mode bbox fallbacks at `selection.cpp:874`, `921`, `1052`

**Resolution: don't use `SvgRect`** (§1). Widening `isPathElement()` would change behavior
for every pre-existing `RECT` node in every document — hyperlink hit-target rects, rects in
imported SVGs — which is a far wider change than it looks. Using `SvgPath` for boxes makes
all of the above correct with no change to shared code.

Note also that `freeErase` mutates `m_path` in place. Any shape that gets free-erased must
be demoted per §1, or the next parameter change regenerates the path and silently undoes the
erase.

### 7.5 A new undo item type is a sync wire-format change

Undo items are **the sync protocol**. `StrokeAddedItem::serialize()` writes the node with
`SvgWriter(xmlwriter).serialize(s->node)` (`syncundo.cpp:342`) and the receiving side parses
it with `SvgParser` (`scribblesync.cpp:517`, `599`). The `type()` constants in `syncundo.h`
(`STROKE_CHANGE_ITEM = 0x00050002`, …) are wire tags.

So the `ShapeChangedItem` from §6 is not a local concern: it needs a `serialize()`
implementation, a new type constant, and matching handling on the receive path. Skip any of
those and shape edits are silently dropped between synced clients — with no local symptom,
which is the worst way for this to fail.

**The good news, verified:** because sync round-trips nodes through `SvgWriter`/`SvgParser`,
the `__shape*` attributes themselves travel for free, exactly as `__comx` and `__timestamp`
already do. Only the new *item type* needs work.

### 7.6 `groupStrokes()` will sweep shapes into handwriting groups

`ScribbleArea::groupStrokes()` runs on every mode except `MODE_STROKE` and clusters recent
strokes by timestamp and bbox proximity to align handwriting. For groups of four or more it
calls `s->setCom(...)`, overwriting each element's centre of mass — **outside the undo
system**, which is why it has to call `scribbleDoc->strokesUpdated()` to tell sync by hand.

A shape drawn amongst handwriting would be pulled into a word group and have its COM
rewritten. Exclude shapes from `recentStrokes`.

### 7.7 Ruled mode treats shapes as words

`Selection::sortRuled()` / `reflowStrokes()` and `Page::cmpRuled()` order and reflow elements
as if they were handwriting on ruled lines. Insert-space-ruled and reflow will move shapes
around accordingly. Decide deliberately whether a shape participates in reflow or is anchored;
"anchored" is almost certainly what's wanted for a box drawn around a paragraph.

### 7.8 Transform vs. descriptor desync — **the most dangerous one**

`Element::applyTransform()` handles transforms two different ways, and only one of them is
compatible with a regenerated path:

- **Translate and rotate** fall through to `m_applyPending = true`, and
  `Element::commitTransform()` bakes them into the *node transform attribute*
  (`node->setTransform(m_pendingTransform.tf() * node->getTransform())`). The path is
  untouched. Descriptor points stay in node-local coordinates with the transform layered on
  top — **consistent**, nothing to do.
- **Non-rotating scale of a path element** takes a different branch and mutates the geometry
  in place: `pathnode->path()->transform(pttf)` (`element.cpp:595`). The descriptor is *not*
  updated.

So resizing a shape rewrites the path but leaves `__shapepts` describing the old size. The
shape looks correct — until the next parameter change fires `onAttrChange`, regeneration
runs from the stale descriptor, and **the shape snaps back to its pre-resize dimensions**.
The failure is delayed and looks unrelated to the resize that caused it, which makes it
nasty to debug after the fact.

**Fix:** `applyTransform()` needs a shape branch *ahead of* the `isPathElement()` branch that
transforms the descriptor — map every point in `__shapepts`, scale `rx` by `sx` and `ry` by
`sy` — and then regenerates. Never let the generic path-transform branch touch a shape.

Corollary for §5: a non-uniform scale legitimately makes `rx` and `ry` diverge. That is
representable and should be preserved, not re-uniformed.

### 7.9 Pen classes must not be applied to filled shape parts

`StrokeBuilder` tags stroked paths with `Element::STROKE_PEN_CLASS`
(`strokebuilder.cpp:89`), and `Element::toPenPoints()` / `fromPenPoints()` branch on these
classes (`element.cpp:215–320`) to reconstruct variable-width pen geometry.

`Element::scaleWidth()` returns early once it has scaled `stroke-width` **only if
`stroke` != `NONE`**. For a *filled* node with a pen class — an arrowhead — it instead falls
into the `toPenPoints()` path and tries to reinterpret the filled outline as pen strokes,
producing garbage.

So: stroked shape paths may carry `STROKE_PEN_CLASS`; **filled parts (arrowheads) must carry
no pen class at all.**

### 7.10 Test fixtures compare serialized output

`ScribbleTest` compares saved documents against `testN_ref.html` textually via
`testCompareFiles()`, and falls back to comparing rendered thumbnails. Existing fixtures
contain no shapes, so new `__shape*` attributes won't perturb them — but any change to
*shared* serialization or class assignment would. The suite already covers free-erase of
"stroke, filled stroke, hyperref", which is precisely the demotion path from §1, so shape
cases belong there. There's an existing workflow for regenerating refs (`scribbletest.cpp`
around line 248 writes `_new.html`).

### 7.11 `Page::addStroke()` already creates the undo item

`Page::addStroke()` adds a `StrokeAddedItem` itself when `history->undoable()`. Commit inside
`startAction`/`endAction` and let it do that — adding one manually as well produces a
double-undo.

### 7.2 Grouped arrows hit `isMultiStroke()`

`Element::isMultiStroke()` is currently `return isHyperRef();` (`element.h:168`) and
selection, erase and transform all branch on it. An arrow built as a `<g>` must be covered.
The author anticipated this — the adjacent comment reads *"all children of multi-stroke have
Element exts; may extend to include bookmark groups later"* — and `Element::Element()`
already constructs child `Element` exts for multi-stroke nodes, asserting that the parent is
created first. Extend deliberately; skip it and grouped arrows misbehave under the stroke
eraser.

### 7.3 Double-click already creates a page

`ScribbleArea::doReleaseEvent` opens with a `MODEMOD_DBLCLICK` check that appends a new page
when you double-click past the last page, with `MODE_PAGESEL` explicitly excluded.
`MODE_DRAWSHAPE` needs the same exclusion, or finishing a polyline near the end of the
document spawns a page. (Tap-last-point is the primary finish gesture, but double-click is
a supported secondary one on desktop — and the bare double-click still reaches this code.)

### 7.4 Per-corner radii do not serialize

`SvgRect::setCornerRadii()` supports four independent corner radii, but
`SvgWriter::_serialize(SvgRect*)` writes only `rx` and `ry` — the `m_radii[4]` array is
dropped on save. Keep Phase 2 to uniform `rx`/`ry`. If per-corner radii are ever wanted,
they must go in the `__shape*` descriptor like everything else, not in the native
attributes.

---

## 8. Phasing

| Step | Content | Gate |
|---|---|---|
| 1 | Descriptor + serialize/parse round-trip; `box` only, drag-bbox, built as `SvgPath`; **`applyTransform()` shape branch (§7.8)** | Draw a box, resize it, change a param, save, reload — still correct |
| 2 | `line`, `arrow` (drag-vector), `ellipse`; `isMultiStroke()` extended for arrow groups | Stroke eraser and selection behave on all four |
| 3 | `rbox` + shape menu UI + constrain modifier; exclude shapes from `groupStrokes()` | |
| 4 | `ShapeSelector`, point/box handles, `ShapeChangedItem` **+ its sync wire support** | Edit a shape on one synced client, see it on the other |
| 5 | Radius handle | |
| 6 | `polyline` / multi-point gesture + persistent-state teardown | Cancel from every escape route leaves no orphan |

Steps 1–3 are Phase 1, 4–6 are Phase 2. Step 6 is deliberately last: it is the only step
that changes `ScribbleArea`'s gesture lifetime assumptions, and everything before it is
useful without it.

## 9. Deferred

- **Bézier / smooth points.** The `smooth` flag is reserved in `__shapeflags` but unused.
  When wanted, prefer fitting a spline through the existing points over adding
  Illustrator-style per-point control handles — most of the visual benefit, a fraction of the
  UI and data-model work.
- **Freehand arrow** (draw any squiggle, arrowhead at the end). This is a `ScribblePen` flag
  plus a head appended in `StrokeBuilder::finish()`, reusing the whole stroke pipeline — a
  different mechanism from this spec, and its output is ink, not an editable shape. Orient
  the head from the last several points' tangent, not the final two, or pen jitter at liftoff
  makes it wobble.
- **Text in shapes**, connectors that stay attached to shapes, fill patterns.

## 10. Open questions

- **Default fill:** none (outline only) or a translucent fill? Affects hit-testing
  expectations — a filled box should be grabbable from its interior, an outlined one
  arguably not.
- **Does the shape tool stay active after one shape**, or revert to the pen? `ScribbleMode`
  already has `stickyMode` / `setMode(mode, once)` for exactly this; just needs a decision.
- **Should shapes use the current pen's colour and width**, or carry their own persisted
  style? Reusing the pen is less UI and probably what you want.
