# Shape recognition lab

A standalone testbed for the gesture "draw something, then hold the pen still". The stroke is recognized
as a **line**, a **rectangle**, a **circle**, an **ellipse**, a
**scratch-out** (back and forth over something to erase it), or nothing. Plain C++17 with no Write or
ulib dependency. The recognizer itself now lives in `syncscribble/shaperec.*` and is what `make test`
compiles, so this suite tests the copy Write ships (see "Hold to snap" in the top-level CLAUDE.md).
Nothing else here is built into Write. `INTEGRATION.md` is the plan the integration followed.

```
make test                     # both synthetic suites + recorded strokes in fixtures/, writes report.html
./evaluate --only square      # one class
./evaluate --misses 5         # also print up to 5 wrongly recognized strokes per class, with the reason
python3 playground.py         # draw live, see the result the moment the pen lifts (also from an iPad)
python3 serve.py              # recorder.html for other devices; saves straight into fixtures/
```

## Two synthetic suites

Both are gated. The **default** set is the one every number below was measured on. The **pencil** set
draws the same classes the way Apple Pencil input arrives, and adds strokes the default set never had:

- every coalesced touch at 240 Hz (0.6–3 units apart), low jitter, no whole-unit rounding
- a hold of 120–360 samples that creeps up to 5.5 units, inside the app's 6-unit hold radius, rather
  than a few samples jittering in place
- lines bowed into a light arc, up to 15% of their length off straight
- wide zigzag scratch-outs whose passes lean 30–60° because the hand travels fast along what it erases
- sloppier closed shapes: sides bowed 3%, corners missed by 3.5%, round shapes stopping up to 60° short

Its corners are missed by more on purpose, so it gates *what* is recognized (recall, false erases) and
only reports position. Before the pencil set existed it measured line 74%, circle 96.5%, ellipse 92%,
scratch-out 84%; the 240 Hz sampling and the drifting hold turned out to cost nothing by themselves. What
failed was the bow, the stopping short, and the leaning passes:

| change | why |
|---|---|
| `lineMaxRms` 0.035 → 0.05, `lineMaxDev` 0.1 → 0.13 | a bow over 10–11% of the length was never a line; now up to ~16% |
| `maxClosureGap` 0.15 → 0.25 | quick circles stop 55–60° short |
| `closedMaxErr` 0.07 → 0.09 | bowed sides and missed corners |
| scrub-axis fallback (below) | wide zigzags had no scrub axis at all |

The cost is in the "other" class (reported, not gated): 94–95% of it stays ink rather than 98.5%, the
difference mostly spirals and wanders taken for ellipses. After a deliberate hold that is one undo.

## The two mistakes cost different amounts

Recognition only runs on a deliberate hold, so the user has asked for *something*:

- **Line, rectangle and circle lean towards recognizing.** A wrong shape costs one undo, a missed one
  costs a redraw. Their recall gates are 99%. A stroke that is none of these (the "other" class) becoming
  a line or circle is reported, not gated.
- **A held scratch-out is eager.** Holding still after going back and forth plainly says "erase", and
  the hold need not survive normal writing (a held mmm may erase). It is tested first, and at most 2% of
  everything else may come out as a scratch-out; it measures 1.2-1.3%, nearly all mmm arches and random
  wanders. Its defaults are `Params`'; it was 0.5% while the hold had to protect writing.
- **The scratch-out on pen lift must leave writing alone** (`liftParams`, the app's "Scratch out without
  holding": Off, Careful, Normal). It runs over every stroke written, with no hold to say the user meant
  anything, so it needs a longer scratch-out (6 passes normal, 8 careful) and keeps the strict turn
  limits. Gated at 0.1% false erases over both suites, and every recorded scratch-out must pass at both
  levels:

  | | scratch-outs caught | false erases |
  |---|---|---|
  | hold | 99.9% | 1.2-1.3% |
  | lift, normal | 78-80% (the rest are 4-5 passes: hold for those) | 0.06-0.07% |
  | lift, careful | 57-58% | 0 |

  Mutation-tested: lift with the hold's parameters (1.26%), and lift's long rule from 12 reversals
  (0.6%), each fail.

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

**The scrub axis.** It is the mean direction of the stroke's short chords, which is the scrub axis only
while the passes lean less than 45°. A wide zigzag (scratching out half of a line, moving fast along
it) leans further, the mean then points along the travel, and along the travel the stroke never turns
back: 0 reversals. So when the mean axis fails, 11 more axes spread over 180° are tried, those the
stroke reverses along most often first, and each must pass every test above. Two things keep that from
erasing more than before:

- The mean axis is still tried first and alone decides whenever it passes, so nothing that was a
  scratch-out changes. Picking the axis with the most reversals outright took 4% of scratch-outs away.
- A fallback axis must carry at least 40% of the stroke's motion. Across a line its wobble reverses
  plenty of times; without this, 2.6% of the default set's lines became scratch-outs and false erases went
  to 0.68%.

On a leaning pass the reversal gap is measured across the passes, not across the axis (times the cosine
of the lean): a turn rounded a little along the axis otherwise puts the lines 1/cos(lean) further apart.
Passes along the axis, which is where m and n are, are unaffected.

**Eagerness trade-off.** The test scratch-outs are deliberately messy: some have only 4 passes, turns
rounded up to 15% of a pass, and ends landing ±15% short of or past the edge.

| minimum reversals | messy scratch-outs recognized | other strokes erased |
|---|---|---|
| 3 (current) | 99.7 % | 0.1–0.4 % |
| 4 | 87 % | about 0.1 % |

To make it stricter, set `scribbleMinReversals = 4`; that single line switches back. Loosening the
reversal gap to 0.3 tripled false erases. The straightness limits made no difference either way.

**Long scratch-outs are judged more loosely.** Real ones recorded in the app (`fixtures/app-*`) run
along a whole line of text with 25-45 reversals, and are nothing like the tidy synthetic zigzags: a
sawtooth with rounded bottoms, or loops (ll, e), arches and spikes all in one stroke.

- The pass-length and reversal tests ignore their worst 20% (`scribbleOutlierFrac`): a long stroke
  always has a pass cut short or a turn just over the limit. With few reversals the worst one still
  counts, and mmm cannot slip through - its wide arches are every other reversal.
- From 20 reversals (`scribbleLongReversals`) a turn may be up to 0.6 of a pass wide instead of 0.2.
  Below that, mmm and cursive are what turns wide.

| relax the turn limit from | false erases, default / pencil |
|---|---|
| never | 0.20 / 0.16 % |
| 12 reversals | 0.82 / 0.72 % (fails the gate) |
| 16 reversals | 0.68 / 0.46 % (fails the gate) |
| 20 reversals (current) | 0.34 / 0.36 % |

The extra false erases are synthetic mmm of 10-12 humps; a held "minimum" stays under 20 reversals.
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
- (pencil set) no scrub-axis fallback, no 40%-of-motion test on a fallback axis (false erases), the
  reversal gap measured across the axis instead of across leaning passes, and each of the old line,
  closure-gap and closed-error limits

**Not covered by any gate:**
- `maxClosureGap` 0.25 rather than 0.2: 0.2 still passes the gate, while 0.25 recovers about half of the
  thin ellipses that stop short (the pencil ellipse gate is 98% for those, see `PENCIL_MIN_RECALL`)
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

- **Recorded strokes in `fixtures/` are class-gated:** each must come out as its label (position is
  only reported). Set `recordShapeStrokes` to 1 in the app's config and every held stroke is appended to
  `shape-strokes.strokes` in the library with what it was recognized as; label the ones it got wrong
  (`label scribble`, ...) and move them here.
- **Tuned mostly on synthetic strokes;** the only real ones are the app recordings in `fixtures/`. Mouse input showed two things that
  synthetic data did not: whole-pixel staircases (now handled) and lopsided circles near the
  straight-stretch veto (limit loosened to 0.5). The pencil set models Apple Pencil from how iOS
  delivers it (`coalescedTouchesForTouch`), not from a recording.
- Thin ellipses (minor/major ~0.3) stopped 60° short are refused: the gap at the pointed end is
  0.26–0.33 of the perimeter. 1–1.6% of the pencil ellipses.
- Spirals and octagons are often taken for circles. That is acceptable given the recall bias.
- Scratch-outs are zigzags only; a loopy scrubbing motion (round and round) is not recognized.
