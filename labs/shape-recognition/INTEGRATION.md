# Integrating shape recognition into Write

This is the short version of how to wire `shaperec` into the app. The README covers why it works this way.

## 1. Moving the code

- **Recognizer:** copy `shaperec.h/.cpp` and `geom.h` into `ulib/`. They need only the standard library.
  Replace `Vec2` with ulib's `Point`, or keep `geom.h` and convert at the call site. The recognizer uses
  `Vec2` only as `x`, `y`, `+ - * /`, `dot`, `cross`, `length`, `normalized` and `perp`.
- **Test suite:** keep `synth.*`, `evaluate.cpp` and `fixtures/` in the lab as the regression suite.
  Wiring `runShapeRecTests()` into `ScribbleTest::runAll()` would follow the `scantest`/`colortest`
  pattern if wanted.
- **Leave behind:** `recognize_cli.cpp`, `playground.*`, `recorder.html` and `serve.py` are lab-only.

## 2. Trigger: pen held still at the end of a stroke

Recognition is designed to run **only** on a deliberate hold, since the recall bias depends on it.

- Start a timer while the pen is down and has moved less than about 3 screen px since the last reset.
  Fire after about 400–500 ms, and restart on any larger movement.
- On fire, call `recognize()` on the stroke so far, with the hold samples left in; the recognizer
  trims them itself (`holdRadius`, `holdMinSamples`).
- Show the result as a preview straight away. Commit it when the pen lifts, or right away if no preview
  is wanted.
- Cancel the timer on pen up, on cancel, and on a pen-button change, the same escape routes as
  `finishShape()`.

## 3. Units: pass screen coordinates, not document coordinates

Several thresholds are absolute and were tuned in **screen px**:

- `holdRadius` = 3
- the 1-unit minimum resample spacing
- the 2-unit hook minimum
- the 3-unit staircase chords

Document units at a zoom other than 1 would shift all of them. So transform the stroke to screen
coordinates first (or scale the points by the zoom), recognize, then map the result back.

Everything else is relative to the shape's size.

## 4. Mapping results onto `ShapeParams`

`Result.kind` → element; all created via `ScribbleArea::createShapeElement()`, replacing the drawn stroke
in **one undo action** (delete stroke + add shape):

| result | condition | `ShapeParams` |
|---|---|---|
| `Line` | always | `SHAPE_LINE`, `points = {points[0], points[1]}` (drawn order) |
| `Quad` (rectangle) | axis-aligned (`points[1] - points[0]` has x or y ≈ 0) | `SHAPE_BOX`, `points` = two opposite corners (`points[0]`, `points[2]`) |
| `Quad` (rectangle) | rotated (>20° off axis; snap leaves it tilted) | `SHAPE_POLYLINE` + `SHAPEFLAG_CLOSED`, the 4 corners. `SHAPE_BOX` cannot rotate (spec: rotation demotes it) |
| `Ellipse` | `circle` true, or `angle` ≈ 0 mod 90° | `SHAPE_ELLIPSE`, bbox corners `center ± (radiusX, radiusY)` (swap radii if angle ≈ 90°) |
| `Ellipse` | rotated | no shape type exists; `SHAPE_CURVE` + `SHAPEFLAG_CLOSED`, tightness 1, through 8 points on the ellipse, or a plain path |
| `Scribble` | always | no shape: **erase** (below) |
| `None` | always | keep the ink as it is; optionally a small "not recognized" hint |

The snap to the axes returns an exact 0 or 90° angle, so an "≈ 0" test with a tolerance of about 1e-6
is enough.

## 5. Scratch-out: erase

`Result.points` is the erase area: a convex polygon in the same coordinates as the input.

- Select every element on the current page whose bounding box centre, or most of whose points, lie
  inside the polygon. A plain overlap test would also erase strokes the scratch-out only grazed.
- Go through the normal `Selection` path, so that locked or hidden **layers are respected**
  (`Page::isEditable()`) and the delete is **undoable and syncs**.
- Delete the scratch-out stroke itself in the same undo action.
- Consider a short flash of the area before deleting. The false-erase rate on synthetic data is 0.1–0.4%,
  and a held W or M counts as a scratch-out.

## 6. Tuning knobs (`shaperec::Params`)

Pass a `Params` and keep the defaults unless a real-pen fixture shows otherwise:

| knob | default | effect |
|---|---|---|
| `scribbleMinReversals` | 3 | 4 = stricter scratch-out (87% vs 99.7% recall, about 0.1% vs up to 0.4% false) |
| `snapDeg` | 10 | rectangle/ellipse axis snap; full at ≤10°, partial to 20° |
| `circleMinAspect` / `circleEaseAspect` | 0.8 / 0.65 | circle preference |
| `rectangle` | true | false = free quadrilateral |
| `lineMaxRms`, `closedMaxErr` | 0.035, 0.07 | overall eagerness for lines and closed shapes |

## 7. Before shipping

- **Tuned on synthetic strokes only.** Record stylus strokes with `recorder.html` (via `serve.py`), then run `make test`.
  The thresholds to watch are `ellipseMaxStraightFrac`, `snapDeg` and `scribbleMinReversals`.
- **Cost:** about 0.4 ms per stroke. Running it synchronously on the input thread is fine.
- **After changing any threshold,** run `make test` and all five seeds (`./evaluate --seed N`). A gate
  that stops failing when you break something is testing nothing; see "The gates catch regressions" in the README.
