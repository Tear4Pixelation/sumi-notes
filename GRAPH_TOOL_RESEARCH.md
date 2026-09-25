# Graph / Maths Tool — Feasibility Research

Research only, no implementation. Companion to `FEATURE_RESEARCH.md` and `SHAPES_SPEC.md`.

**Ask:** a maths/graphing tool. Enter one or more functions, set the x/y range; or skip the
function and just get a coordinate system — sized so its grid matches the page ruling times a
factor (2x, 0.5x).

## Verdict

**Feasible, Medium difficulty.** The shapes system built for `SHAPES_SPEC.md` is almost exactly
the right home: a graph *is* a parametric object whose descriptor is the document and whose
`Path2D` is a cache. Nothing in the architecture fights this.

The ask is really **two features** with very different costs, and they are separable:

| | effort | risk |
|---|---|---|
| **A. Coordinate system only** (axes, ticks, grid snapped to ruling × factor) | ~500–700 LOC, ~1 week | low — nearly all reuse |
| **B. + function plotting** (expression entry, sampling, multiple curves) | +800–1200 LOC, +1.5–2 weeks | medium — parser and asymptotes are the work |

A is worth doing on its own and shipping first; B builds on it without rework.

Three things need a decision before coding — **tick labels**, **multi-curve colour**, and **what
"the ruling" means when `xRuling == 0`**. All three are covered below.

---

## Why the shapes system is the right home

`ShapeParams` (`syncscribble/shape.h`) already gives, for free, everything a graph needs from the
document model:

- **Descriptor is the document.** `Element::setShapeParams()` regenerates the path on every
  change (`element.cpp:677`). Changing `ymax` or an expression is the same operation as dragging
  a box corner — no new mutation pathway.
- **Serialization is already additive.** `__shape`/`__shapepts`/`__shaperx`/`__shapery`/
  `__shapetight`/`__shapeflags` are parsed and written field-by-field at `element.cpp:657–744`.
  Adding `__graphwin` (the data window) and `__graphfn` (the expressions) is more of the same,
  and per `CLAUDE.md` these attributes ride through sync free inside `<addstroke>` because sync
  round-trips nodes through `SvgWriter`/`SvgParser`.
- **Undo and sync already exist for parameter edits.** `ShapeChangedItem` (`syncundo.h:121`) is
  already a sync wire-format item with a `SHAPE_CHANGE_ITEM` constant and a `shapechanged` branch
  in `ScribbleSync::processItem()`. It stores a whole `ShapeParams` by value, so extending the
  struct extends the undo item with no new code — *provided* its `serialize()` is updated to
  carry the new fields. That is the one easy thing to forget.
- **No new gesture.** `ShapeGesture` already has `SHAPEGESTURE_DRAG_BBOX` (used by box and
  ellipse): drag out the plot rectangle. `ShapeHandle::BOX_CORNER` resizes it. `minPoints`/
  `maxPoints` of 2 apply unchanged.
- **The registry is one table entry.** `shapeDefs[]` (`shape.cpp:393`) plus a `buildPath`, a
  `getHandles` and an `applyConstraint`. The mode handlers, selector, serializer and undo item
  are already written once against `ShapeDef`.

### What has to change in `ShapeParams`

`ShapeParams` is today points + `rx`/`ry`/`flags`/`tightness` — all numeric. A graph adds:

```
Rect   window;                      // data-space xmin/xmax/ymin/ymax
Dim    rulingFactor;                // 1, 2, 0.5 ... ; 0 = free (window drives the grid)
std::vector<std::string> exprs;     // empty = coordinate system only
```

This is additive and low risk, but it is the first **non-POD, variable-length** field in the
descriptor. `ShapeChangedItem` copies `ShapeParams` by value (fine), and `serializeShapePoints()`
has an obvious analogue for a string list — but expressions must be escaped (they contain `,`,
`*`, `^`, and `<`/`>` are illegal raw in XML attributes).

---

## The three decisions

### 1. Tick labels = text, and Write has no text element

This is the single biggest finding. **`Element` has no text support at all:**

- `isPathElement()` is `node->type() == SvgNode::PATH` (`element.h`), and every branch in
  `element.cpp` is `isPathElement()` or `SvgNode::IMAGE` — hit testing, `applyTransform`,
  `scaleWidth`, `freeErase`, `toPenPoints`.
- `SvgText` exists in `usvg/svgnode.h:544` and fonts are loaded, and `page.cpp:472` wraps *every*
  content child in an `Element`, so a `<text>` node would render and `selection.cpp:372` even has
  a `SvgNode::TEXT` branch for the selection highlight — but it would fall through every
  path-shaped guard above.
- Worse, `CLAUDE.md` records a deliberate invariant: **shapes are a plain `SvgPath`, never a
  container**, because `isMultiStroke()` is literally `isHyperRef()`. A `<g>` graph containing
  text would be misread as a hyperref group.

Three options:

- **(a) No numeric labels — ticks and grid only.** Cheapest, and arguably correct for a
  handwriting app: you write the numbers yourself, the same way you'd annotate graph paper. Also
  the only option that needs zero new machinery. **Recommended for v1.**
- **(b) Labels as glyph outlines baked into the same `Path2D`.** Keeps the one-path invariant and
  needs no new element type. This is genuinely available: `stb/stb_truetype.h` is vendored and
  exposes `stbtt_GetCodepointShape()` (line 845), which is exactly a digit → contours API, and
  `nanovgXC/src/fontstash.h` already uses stb_truetype so a font handle is in reach. Cost is a
  small glyph→`Path2D` adapter plus digit layout. Good v2. Note the labels then scale and stroke
  like pen strokes, which for hairline digits at small sizes may look wrong.
- **(c) Real text element support in the document model.** Cross-cutting through `element.cpp`,
  `selection.cpp`, erase, undo. Out of scope for this feature — it is its own project.

### 2. Multiple functions, one colour

An `SvgPath` carries one stroke colour, so a single-element graph draws every curve in the
current pen colour. Options:

- **v1: everything in the pen colour**, curves distinguished by nothing. Acceptable for one or
  two functions, poor for four.
- **One element per curve, plus one for the frame**, linked by a shared graph id. Each curve then
  gets its own pen colour and can be deleted individually — but selection, move and delete would
  have to keep them together, which is exactly the grouping machinery that does not exist
  (`isMultiStroke() == isHyperRef()`). Significant.
- **Dash patterns instead of colour** on the same path (`stroke-dasharray` per subpath is not
  possible in one path — so this does not actually work).

Recommendation: v1 ships one colour and accepts it; revisit only if multi-function turns out to
be the common case rather than the exception.

### 3. "Scaled to the ruling" — what happens when `xRuling == 0`

`Page::yruling(true)` and `Page::xruling()` (`page.h:67–68`) give the spacing directly, and
`Page::onPageSizeChange` (`page.cpp:180–200`) shows the ruling is drawn by simple stepping over
`props.xRuling`/`props.yRuling`. Snapping a graph's grid to `factor * ruling` is arithmetic.

But **lined paper has `xRuling == 0`** — horizontal rules only — and a graph needs square cells.
And per `CLAUDE.md`, a scanned page leaves `yRuling == 0` too, where `yruling(true)` falls back to
`BLANK_Y_RULING` (40). So the rule needs to be explicit:

> grid spacing = `factor × (xRuling > 0 ? xRuling : yruling(true))` on both axes

i.e. fall back to the y-spacing for x, giving square cells on lined paper, and to
`BLANK_Y_RULING` on blank paper. Worth stating in the code, because silently getting a 40-unit
grid on a page the user thinks is unruled is confusing.

---

## Function plotting (feature B)

### Expression parser — must be written or vendored

There is **no expression evaluator anywhere in the tree** (grepped `syncscribble`, `ulib`,
`usvg`, `ugui`). Two routes:

- **Write it.** ~250–350 LOC shunting-yard: `+ - * / ^`, unary minus, `sin cos tan asin acos atan
  sinh cosh tanh exp ln log sqrt abs floor`, constants `pi`/`e`, variable `x`. This matches the
  precedent set by `homography.cpp`/`quaddetect.cpp`/`shape.cpp`: put it in `ulib` with no app
  dependencies so it **builds and runs standalone in `scribbletest/`**, exactly like
  `scantest.cpp` and `shapetest.cpp`. Given how cheap unit tests then are, this is the
  recommended route.
- **Vendor tinyexpr** (single C file, zlib licence — compatible with the repo's AGPL-3.0). Saves
  a couple of days but adds a submodule/vendored file and its error reporting is thin; the
  compile-once-evaluate-N-times design is a genuine advantage for sampling.

### Sampling and rendering — where the real bugs live

A naive `for(x = xmin; x <= xmax; x += step) lineTo(...)` gets four things wrong, and every one
is visible:

- **Poles.** `tan(x)`, `1/x` produce a near-vertical line joining +∞ to −∞. Needs a sign-change +
  magnitude test between samples and a `moveTo` instead of a `lineTo`.
- **Domain gaps.** `sqrt(x)`, `ln(x)` return NaN below zero; NaN must break the subpath, not
  enter it.
- **Clipping to the plot rect.** **`Path2D` has no clipping** — `Path2D::subtracted()` is a stub
  returning `*this` and `intersects()` returns `false` (`ulib/path2d.h:79–80`). Segments crossing
  the frame must be clipped by hand (Liang–Barsky, ~40 LOC) or the curve draws outside its own
  box. This is a real, easily-missed item.
- **Undersampling.** Fixed steps miss sharp features. Adaptive subdivision on angle between
  consecutive segments is the standard fix, ~60 LOC.

Budget ~250–350 LOC for the plotter and expect to spend real time on the pole/NaN cases. This is
the part that looks done long before it is.

### Interaction subtleties inherited from shapes

- **`applyTransform` needs a graph branch**, exactly as `CLAUDE.md` warns for shapes generally:
  scaling the element must scale the plot rect and **leave the data window alone** (a bigger box
  still shows −10..10, at more units per x). Getting this backwards produces the classic
  "shape snaps back on the next parameter change" bug that `shapeRoundTripTest()` exists to
  catch — a graph version of that test is cheap and should be written.
- **Rotation must `dropShape()`** under the same degradation contract as box/ellipse.
- **Extra handles.** Beyond the four `BOX_CORNER` handles, dragging inside the plot to pan the
  data window and a pinch/handle to change the window is natural — `dragShapeHandle()` gains a
  graph branch. Optional for v1.
- Graphs must be excluded from `groupStrokes()` for the same reason shapes are.

### UI

An expression-entry dialog. `createTextEdit()` (`ugui/textedit.h:74`) with `onChanged` gives live
re-plot as you type; `rulingdialog.cpp` is the closest model for layout (it also shows the
`createTextSpinBox` pattern for the range fields and a live preview). A ruling-factor combo
(`0.5x / 1x / 2x / free`) mirrors `comboPaperSize`. Budget ~250–400 LOC plus a toolbar entry and
an icon (per memory: from reicon JSON via `scribbleres/reicon_import.py`, not hand-drawn).

---

## Effort breakdown

| Piece | LOC | Notes |
|---|---|---|
| `ShapeParams` extension + serialize + `ShapeChangedItem::serialize()` | 150–250 | additive; string escaping is the fiddly bit |
| Graph `buildPath`: frame, axes, ticks, grid, ruling snap | 200–300 | **A** |
| Registry entry, handles, `applyConstraint`, `applyTransform` branch | 100–150 | **A** |
| Dialog + toolbar + icon | 250–400 | **A** (range/factor) then **B** (expressions) |
| Expression parser + standalone unit tests | 300–400 | **B** |
| Sampler: adaptive, poles, NaN, Liang–Barsky clip | 250–350 | **B** |
| Round-trip / interrupt tests in `scribbletest` | 100–150 | mirror `shapeRoundTripTest()` |

**A alone: ~500–700 LOC, ~1 week. A + B: ~1350–2000 LOC, ~2.5–3 weeks.**

## Alternative worth considering: a graph-paper *ruling*

"Scaled to the size of the ruling" hints at graph paper, and there is a much cheaper version of
part of this ask: add a **ruling type** rather than an element. `Page::onPageSizeChange()`
(`page.cpp:175–215`) already builds the grid by stepping over `xRuling`/`yRuling`; adding axes
through the origin plus heavier every-5th lines is ~50 LOC there, plus a `rulingdialog` entry.

It is not a substitute — it is page-wide, not a placeable object you can put two of on a page,
and it interacts with `isCustomRuling`. But if the real need is "give me graph paper to draw on",
it is a day's work rather than a week's, and it is a reasonable thing to ship alongside.

## Recommended sequence

1. **Graph-paper ruling** (~1 day) — immediate value, no new subsystem.
2. **A: coordinate-system shape** — descriptor, registry entry, ruling snap, corner handles,
   range dialog. Ship with ticks but no numeric labels.
3. **B: function plotting** — parser in `ulib` with standalone tests first, then the sampler,
   then expression entry in the dialog.
4. **Glyph-outline tick labels** via `stbtt_GetCodepointShape`, if labels prove necessary.
