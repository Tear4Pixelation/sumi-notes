# Jagged strokes: root cause

Investigated 2026-09-20. Not a smoothing/simplification question — the geometry is jagged before
any filter would run, and on this machine no filter runs at all
(`inputSimplify = 0`, `inputSmoothing = 0` on desktop, `scribbleconfig.cpp:68,70`).

## 1. The measurement

Took a real, human-drawn stroke out of `test-docs/Sep 20 11h58.svgz` (a `write-flat-pen` filled
stroke, 660 outline points), recovered its centreline as the midpoint of the two outline sides, and
looked at the deltas between consecutive input samples:

- Every delta is an **integer multiple of 0.655 document units** (mean lattice error 0.001).
  0.655 du is exactly one *screen pixel* at the zoom the stroke was drawn at.
- Distinct small step sizes: 0.655, 1.31, 1.965, 2.62, 3.93 — i.e. 1, 2, 3, 4, 6 pixels.
- Median sample spacing: 1.31 du = **2 px**.
- Turn angle between consecutive samples: median 0.0 deg, p75 **26.6 deg**, p90 **63.4 deg**, p99 90 deg.
  Those are not arbitrary numbers: 26.57 = atan(1/2), 45, 63.43 = atan(2), 90. They are the
  directions available on an integer lattice.

So the stroke direction is quantised. A curve cannot be curved: each 2 px chord has to pick one of a
handful of lattice angles, and the "curve" is a staircase of them.

## 2. Where the precision is thrown away

The pointer path is integer-pixel end to end:

- `SDL/src/video/x11/SDL_x11xinput2.c:187` — XInput2 reports valuators as `double` (a tablet or a
  high-resolution mouse has real sub-pixel precision here); SDL truncates:
  `SDL_SendMouseMotion(..., (int)relative_coords[0], (int)relative_coords[1])`.
- `SDL/src/events/SDL_mouse_c.h:124` — `SDL_SendMouseMotion(..., int x, int y)`. SDL2's internal
  mouse state is `int`, and `SDL_MouseMotionEvent.x/y` are `Sint32` (`SDL/include/SDL_events.h:260`).
- `ugui/svggui.cpp:1555,1564` — `p = Point(event->motion.x, event->motion.y)`, already integers.
- There is **no XI2 tablet/stylus path on X11 at all** in the fork (no `XI_Motion` handler). A stylus
  arrives as a mouse: integer coordinates *and* pressure permanently 1.0.

The touch path is the exception and is fine — `SDL_SendTouchMotion` takes floats and X11
`XI_TouchUpdate` passes `event_x`/`event_y` through as doubles
(`SDL_x11xinput2.c:218-226`); Windows `WM_TOUCH` likewise (1/100 px). So this is a
**Linux/X11 mouse-and-stylus defect**, not a cross-platform one — which matches where it is being
seen.

Also checked and ruled out:

- Save precision — `SvgWriter::SVG_FLOAT_PRECISION = 3` is 3 *decimals*, not 3 significant digits
  (`ulib/stringutil.h:301`). No loss.
- Event coalescing — the app does not compress motion events.
- Renderer AA / tessellation — nanovgXC is exact-coverage; the cap arcs go through
  `Path2D::addArc` -> cubics -> nanovg's device-space flattening. Not the source.

## 3. What amplifies it

Quantised direction would be a subtle defect if the stroke were just a thin polyline. It is not:

- The default pen is `write-flat-pen`, i.e. `FilledStrokeBuilder` in `Flat` style, which extrudes the
  outline along **per-segment normals with a miter join**
  (`syncscribble/strokebuilder.cpp`, `extrude = hw*(n01 + n12)/(1 + dot(n01, n12))`). The normal is
  computed from a *single* 2 px segment, so it inherits the full ±14 deg quantisation error, and the
  miter length goes as 1/cos of it. That is both the visible corner *and* the width wobble along the
  edge of a curve (clearly visible in a 4x crop of a drawn circle).
- `Round` style emits one closed capsule subpath **per input segment** (`addRoundSubpath`), so the
  boundary of the union has a crease at every sample.
- Nothing ever fits a curve. `StrokedStrokeBuilder::addPoint` only ever calls `lineTo`. A stroke is a
  polyline in the document and stays a polyline at every zoom — zooming in magnifies each chord
  instead of refining it, which is why the facets get worse the closer you look.

## 4. Fix directions

Ordered by how close they sit to the cause.

1. **Get sub-pixel coordinates out of X11.** Add an `XI_Motion` handler alongside the existing
   `XI_Touch*` ones, read the valuators as doubles, and deliver them without truncating. The fork
   already has a float-carrying event path (`SDL_SendTouchMotion`); the alternative is widening the
   mouse path to float, which is what SDL3 did. Doing this also opens the door to real pressure and
   tilt from a tablet on Linux, which currently do not exist.
2. **Audit the other platforms for the same truncation** before assuming they are fine. Android
   (`MotionEvent.getX()`) and Windows `WM_TOUCH` are float; the Windows *pen* path and the
   `syncscribble/windows` glue were not checked.
3. **Stop deriving the outline normal from one segment.** Compute the tangent over a fixed arc length
   (a few document units) rather than from the single adjacent sample, so a one-pixel wobble in one
   sample cannot rotate the outline. This is a change to how the geometry is built, not a filter on
   the input — it does not move the centreline. Switching the flat pen's join from miter to round
   would also stop a direction jump turning into a spike, at the cost of the chisel look.
4. **Consider storing strokes as curves, not polylines**, if smoothness at high zoom matters. Today
   the document holds the raw sample chords, so no amount of input precision makes a stroke smooth
   when magnified 8x.

Items 1 and 3 are independent: 1 removes the noise, 3 stops the pen geometry from multiplying
whatever noise remains.
