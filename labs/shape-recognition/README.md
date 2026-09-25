# Shape recognition lab

A standalone testbed for the gesture "draw something, then hold the pen still". The stroke is recognized
as a **line**, a **rectangle**, a **circle**, an **ellipse**, a
**scratch-out** (back and forth over something to erase it), or nothing. Plain C++17 with no Write or
ulib dependency. The recognizer itself now lives in `syncscribble/shaperec.*` and is what `make test`
compiles, so this suite tests the copy Write ships (see "Hold to snap" in the top-level CLAUDE.md).
Nothing else here is built into Write. `INTEGRATION.md` is the plan the integration followed.

```
make test                     # synthetic suite + recorded strokes in fixtures/, writes report.html
./evaluate --only square      # one class
python3 playground.py         # draw live, see the result the moment the pen lifts (also from an iPad)
python3 serve.py              # recorder.html for other devices; saves straight into fixtures/
```

## The two mistakes cost different amounts

Recognition only runs on a deliberate hold, so the user has asked for *something*:

- **Line, rectangle and circle lean towards recognizing.** A wrong shape costs one undo, a missed one
  costs a redraw. Their recall gates are 99%. A stroke that is none of these (the "other" class) becoming
  a line or circle is reported, not gated.
- **A scratch-out erases whatever is under it, so it must be unmistakable.** It is tested first, with
  criteria of its own, and the gate is the other way round: at most 0.5% of everything else may come
  out as a scratch-out. The measured rate is 0.1–0.4%, which is the price of making scratch-outs eager
  (see below).

## Position

| class | position error, median / p90 | measured as |
|---|---|---|
| line | 0.6 / 1.5 % | worst end, % of length |
| rectangle | 2.8 / 4.3 % | worst corner, % of side |
| circle | 0.6 / 0.9 % | centre or radius, % of radius |
| ellipse | 1.1 / 2.2 % | largest outline distance, % of the larger radius |
| scratch-out | 3.1 / 6.0 % | largest outline distance of the erase area, % of its size |

- **Rectangles have 90° corners and snap straight.** All four sides share one orientation, the
  length-weighted mean of their drawn directions. Within 10° of the screen axes that orientation is
  turned fully straight, and between 10° and 20° it is turned back part of the way, so there is no jump
  at the threshold. Beyond 20° the rectangle keeps its angle. Each side then sits at the centroid of its
  own drawn points (10% trimmed at each end), so straightening moves the corners only as far as it has
  to. The test set models a hand that tilts a straight-meant rectangle by about 3° and misses each corner
  by about 2%:

  | fit | median | p90 |
  |---|---|---|
  | rectangle with snap | 2.8 % | 4.3 % |
  | rectangle without snap | 4.2 % | 8.0 % |
  | drawn points where the pen turned | 6.9 % | 10.4 % |
- **Round shapes prefer to be circles, and ellipses snap straight.** A hand drawing a circle draws an
  oval. At a minor/major ratio of 0.8 or more the shape becomes a true circle. From 0.65 to 0.8 the two
  radii are pulled part of the way together, so the result does not jump. Below 0.65 it stays an
  ellipse, and an ellipse gets the same 10°/20° axis snap as a rectangle. The circle test set is drawn
  up to 0.82 oval, and a circle only counts if it comes out as a true circle. The old 0.85 cutoff turned
  17% of those into ellipses. The ellipse test set (0.3–0.6, mostly straight, about 3° of hand tilt)
  only counts if it stays an ellipse. Without the snap its outline error is 2.2 / 5.2%.
- **Line ends are the drawn ends**, minus any hook (the flick as the pen lands or lifts).
- **The hold is removed first.** Holding still leaves a cluster of samples at the end, which would add
  arc length, a hook or a corner. Samples near the cluster's running centre are replaced by their median.
  Without that, line recall falls to 97.9%.
- **A scratch-out's erase area** is the convex hull of the stroke.

## How a scratch-out is told apart

A scratch-out needs at least 3 reversals (4 passes) along one axis, with passes of similar length
(each at least 40% of the median) that are nearly straight. At every reversal the pen must come
straight back: lines fitted to the passes either side meet within 20% of a pass length. That last test
does the real work, because it rejects the handwriting strokes that also go up and down (m, n: their
sides are an arch-width apart).

**Eagerness trade-off.** The test scratch-outs are deliberately messy: some have only 4 passes, turns
rounded up to 15% of a pass, and ends landing ±15% short of or past the edge.

| minimum reversals | messy scratch-outs recognized | other strokes erased |
|---|---|---|
| 3 (current) | 99.7 % | 0.1–0.4 % |
| 4 | 87 % | about 0.1 % |

To make it stricter, set `scribbleMinReversals = 4`; that single line switches back. Loosening the
reversal gap to 0.3 tripled false erases. The straightness limits made no difference either way.
A test for how widely the stroke turns at each reversal (swoops turn wide, scratching turns sharp)
was tried and did not separate the two.

**Known ambiguity:** handwritten "wwww" or "vvvv", or any zigzag of five or more strokes, is
geometrically a zigzag scratch-out, and no threshold separates them. The hold is what separates them.
The synthetic "other" class therefore holds N and Z (a held W or M, at 3 reversals, is taken for a scratch-out), m/n arches, cursive
loops, spirals and random curves. Every remaining false scratch-out is a random swooping curve that
happens to swing back and forth three or more times.

## The gates catch regressions

Each gate was checked against a deliberately broken recognizer, and every one of these fails
`make test`:

- corners from the drawn turn points
- side trim 0.3
- no hook trimming
- the hook test by turn only
- no straight-stretch veto
- the whole stroke taken as the loop
- no coarse corner scale
- no hold trim
- no scratch-out reversal-gap test
- scratch-out minimum reversals of 2 (and of 4, which fails the recall gate)
- a free quad instead of a rectangle
- no snap to the screen axes (rectangles, and ellipses separately)
- the old 0.85 circle cutoff

**Not covered by any gate:**
- the straight-stretch veto (hexagons, spirals and octagons becoming circles is reported, not gated,
  given the recall bias). It is measured against the fitted ellipse's own curvature: measured against
  a circle's steady rate, it rejected 60% of real ellipses as "straight".
- the scratch-out's straight-pass and pass-length tests (kept as a safety margin for erase)
- the staircase-proof straightness measure (synthetic sampling is not dense enough to need it; the mouse recordings were)

## Tried and removed

Each of these was measured and made no difference, or made things worse:

- fitting each corner from only half of each side
- a quad residual that ignores the corner zones
- a geometric circle refit
- counting sharp corners to veto ellipses
- "too thin" and "fifth corner" checks
- a distance-based hook test
- comparing scratch-out pass *ends* at reversals (it let 7% of mmm through)
- requiring scratch-out passes to be parallel to the axis

## Files

| file | what |
|---|---|
| `shaperec.h/.cpp` | the recognizer, `shaperec::recognize(points, params)` |
| `synth.h/.cpp` | hand-drawn stroke generator that knows the true answer for each stroke, including the hold |
| `evaluate.cpp` | scoring, gates, HTML report |
| `strokefile.h/.cpp` | the `.strokes` text format |
| `recorder.html`, `serve.py` | record real strokes against targets (a shape to trace, a block to scratch out) |
| `playground.html`, `playground.py`, `recognize_cli.cpp` | live recognition on pen lift |

## Known gaps

- **Tuned on synthetic strokes only;** no stylus data yet. Mouse input showed two things that
  synthetic data did not: whole-pixel staircases (now handled) and lopsided circles near the
  straight-stretch veto (limit loosened to 0.5).
- Spirals and octagons are often taken for circles. That is acceptable given the recall bias.
- Scratch-outs are zigzags only; a loopy scrubbing motion (round and round) is not recognized.
