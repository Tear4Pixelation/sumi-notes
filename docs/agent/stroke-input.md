# Curve fitting the input (stroke builder)

A stroke used to be a polyline straight through the raw samples, and on the whole-pixel lattice most
platforms deliver (see below) that reads as a staircase rather than a line: at the 1-2 px spacing a mouse
gives, an integer grid only allows a handful of segment directions. `CurveFitFilter`
(`strokebuilder.h`/`.cpp`) replaces those straight segments with a curve, on by default
(`inputCurveFit`, the strength: 0 disables, 4 is strongest, default 2; exposed in the Input prefs).

- **It is a curve fit, not a low pass.** Each interior point is relaxed towards its uniform cubic B-spline
  knot `(P[i-1] + 4P[i] + P[i+1])/6` and a **centripetal** Catmull-Rom through the relaxed points is
  emitted as chords - the same construction `shape.cpp`'s Curve tool uses, and centripetal for the same
  reason (uniform overshoots on uneven spacing, which is what real input looks like). The relaxation is a
  1-4-1 kernel; for what it is actually worth per pass, see the measurements below rather than its
  Nyquist gain. Nothing is averaged over time, so the stroke does not trail the pen.
- **It must be installed first in the chain**, i.e. `addFilter`ed *last* (that call reverses order). It
  turns one input point into several, so it cannot map a `removePoints(n)` from upstream - and
  `SimplifyFilter`, which retracts points it has already emitted, would send it exactly that. Being first
  also means simplify runs *downstream* and decimates the densified curve back down.
- A segment is only final two samples later (it needs the knot past its end), so the raw current point is
  appended straight to the builder as a provisional tip and retracted on the next call - the same trick
  `LowPassIIR` and `StreamStabilizer` use. Those tips stack and pop in LIFO order, which is what keeps
  them from eating each other's points when more than one filter is installed.
- **`inputCurveFit` is the strength: how many times the relaxation is applied** (0-4, default 2), at the
  cost of one more sample of lag per pass and more rounding of genuinely sharp corners; the stencil
  widens with it, so the emit condition is `2 + passes` samples rather than 2.
  **Measured** (`curveFitTest`, 1.5 unit sample spacing), median turn angle and RMS residual against a
  fitted circle, levels 0-4: 18.4/0.294, 9.6/0.214, 5.2/0.182, 3.6/0.165, 2.4/0.154. Two things to read
  off that. The passes *do* compound - each is worth roughly another 1.7x on the turn angle, **not** the
  factor of 3 the 1-4-1 kernel's Nyquist gain suggests, because lattice noise is broadband rather than
  at Nyquist; that claim was in this document and was wrong. And the *geometric* residual **plateaus**
  while the angle keeps falling - most of it is bought by the first pass. That plateau is what is still
  visible as a lumpy outline at high zoom, and it is why strengths 1-4 are hard to tell apart on real
  handwriting, where the variation between strokes is larger than the difference between levels.
  Removing it needs a fit with a *tolerance* rather than more passes of a local smoother.
  **Tried and removed: a greedy incremental least-squares cubic Bezier fit** (`BezierFitFilter`, an
  `inputBezierFit` tolerance selecting it instead of the relaxation) - extend a run while one cubic
  fits within the tolerance, close it at the last sample that did. It lost on both metrics at every
  tolerance: 0.2/0.5/1.0/2.0 gave median turn 11.3/8.1/3.5/3.8 deg and residual 0.319/0.280/0.246/0.429
  against 2.4 and 0.154 for the relaxation, i.e. 60% worse on the residual that was the whole point.
  The cause is structural, so a better tolerance cannot rescue it: the fit **pins each run's end points
  to raw quantised samples**, so every joint carries the full half-unit error and the end tangents are
  estimated from noisy points, leaving only the interior of a run smooth. Tight tolerance gives short
  runs and therefore mostly joints; loose tolerance gives runs a single cubic cannot follow. Anything
  along these lines needs **free end points** (or joints placed on smoothed positions) to be worth
  rebuilding.
  It was first shipped as the *chord flatness tolerance* instead, which was a mistake worth not
  repeating: turning that up makes the filter do **less** (coarser chords, fewer points), so the one
  setting named after the feature ran backwards, and the strength was buried in a second pref nobody
  would find. The tolerance is now the fixed `CURVEFIT_TOL` - it only trades output points for accuracy
  of the curve and has no visible effect, so it was never a user knob.
- **This matters more than it looks, because the default pen is `TIP_FLAT | WIDTH_PR`** - so
  `hasVarWidth()` is true and strokes go to `FilledStrokeBuilder` with `style == Flat`, which extrudes a
  quad per input segment rather than sweeping a disc. The outline comes from each segment's *normal*, so
  a half-pixel centreline wobble is multiplied by the pen width: before this change a 4-unit pen drew
  visible square steps the size of its own width. Switching the tip to Round does not fix it on its own
  (the steps become scallops) - the centreline is the common cause, which is why the fix belongs here
  rather than in the tip geometry.
- Chord count per segment is `ceil(sqrt(dev/tol))` capped at 16, where `dev` is how far the curve strays
  from the chord it replaces - the error of an n-chord approximation of a cubic falls off as 1/n². `tol`
  is divided by `mZoom` at construction, so drawing zoomed in gets more points and zoomed out gets fewer.
- **Pen Tip is back in Pen Settings.** The floating-toolbar redesign (`f4f5062`) dropped the whole pen
  editor the old toolbar had - tip combo, vary-width, dash/gap, pen preview - leaving no way to reach a
  round tip at all. The tip is a flag on the pen, not a config pref, so it cannot come from
  `createToolSettingsButton`'s `prefNames`; that function grew an `extraRows` parameter for widgets like
  this. `setIndex()` rather than `updateIndex()` when syncing from the pen, or the combo writes the pen
  back to itself. A pen carrying neither `TIP_FLAT` nor `TIP_CHISEL` is drawn by `StrokedStrokeBuilder`,
  whose cap and join are round, so it shows as Round. Note the old toolbar auto-switched Flat -> Round
  above width 4; that heuristic was not restored.
- **`scribbletest.cpp` sets `inputCurveFit` to 0**, alongside `inputSmoothing`: the `testN_ref.html`
  fixtures are polylines through the input points. Baseline after this change is unchanged - 0 failed
  tests, 0 failed unit checks, 16 failed thumbnails.

# Sub-pixel input on Linux (experimental, off by default)

Hand-drawn curves render as runs of straight segments. The cause was measured, not guessed
(`tools/stroke-lattice.py` on a saved stroke): every input sample sits on a **1 screen-pixel lattice**,
and the turn angle between consecutive samples clusters at 26.6, 45 and 63.4 degrees - the only directions
an integer grid allows at the 1-2 px spacing a mouse delivers. Whole-pixel input at that spacing *is* a
staircase; nothing downstream can undo it. Full write-up in `JAGGED_STROKES.md`.

Where the precision is lost on this machine: the dev binary links **sdl2-compat** (SDL2 API on top of
SDL3). SDL3 delivers float pointer positions; sdl2-compat casts them to `Sint32` when filling the SDL2
event. And since Write links libX11 (for `linuxtablet.c`), SDL3 picks the X11 backend, so on a Wayland
desktop the app runs under **Xwayland, which itself only reports whole pixels** - there is nothing to
recover on that path at all.

What was built, all gated and inert unless enabled:

- `syncscribble/linux/sdl3input.cpp` - registers an SDL3 event watch via `dlsym` (no compile-time SDL3
  dependency, `RTLD_NOLOAD` so a real SDL2 build never touches it) and records each mouse event's float
  x/y and pen pressure before sdl2-compat truncates them. `SvgGui::subpixelHook` swaps the float back in
  when the SDL2 event is dispatched, matched by type, timestamp and truncated coordinates. Harmless on
  its own: under Xwayland it hands back the same integers.
- `linuxWayland` config (**default 0**) - switches sdl2-compat to the native Wayland backend so 1/256 px
  positions actually arrive. **Currently segfaults at startup** (`SDL_QuitSubSystem`/`SDL_InitSubSystem`
  re-init in `application.cpp`; not yet debugged). `linuxtablet.c` was given a `SDL_SYSWM_X11` guard so it
  no longer dereferences `info.x11` on a Wayland window.
- This is a **Linux/sdl2-compat fix only**. SDL2's mouse API is integer on every platform; each needs its
  own patch. It also does not change stroke geometry: a stroke is still a polyline through the raw
  samples, so a 2 px mouse spacing still gives 2 px chords and zoom still magnifies them. Fitting a curve
  in the stroke builder is the platform-independent fix; it is now built - see "Curve fitting the input"
  above - and `curveFitTest()` measures what it recovers (median turn 18.4 -> 2.4 deg at 1.5 unit sample
  spacing). It compensates for the quantisation rather than undoing it: the samples are still integers.
- Tried and reverted: measuring the flat pen's outline normal over an 8 px arc-length window instead of
  one segment. Its own test (integer-lattice circle, outline turn-angle q90) showed no improvement.

Side changes kept: the vendored SDL fork builds on Arch (`_GNU_SOURCE` in the non-system-SDL branch of
`syncscribble/Makefile`; pkg-config include paths in `scribbleres/SDL-Makefile.unix`); the SDL submodule
is on `write-linux`. `agent-pointer` sends fractional positions (256x extent) and interpolates one step
per pixel of travel; `agent-display.sh run --build DIR` selects any build dir and `wkey` sends keystrokes
to a native Wayland client. `tools/stroke-lattice.py` and `tools/outline-wobble.py` are the measurements.

To try it: `./ReleaseB/Sumi --linuxWayland=1`. To revert everything: `git checkout` the touched files,
`rm syncscribble/linux/sdl3input.*`, `git -C SDL checkout bf28970b2`.
