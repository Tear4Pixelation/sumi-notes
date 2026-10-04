#pragma once

// Single-stroke recognition, run when the user holds the pen still at the end of a stroke: a line, a
//  quadrilateral ("square"), an ellipse/circle, a scratch-out ("scribble": back and forth over something
//  to erase it), or nothing.
//
// The two kinds of mistake cost very different amounts.  A line, square or circle recognized when it was
//  not meant costs one undo, and the hold means the user asked for *something* - so those lean towards
//  recognizing.  A scratch-out erases everything under it, so it has to be unmistakable and is tested
//  first, with strict criteria of its own.
//
// Recognition is two separate problems and this file treats them separately:
//  - *what* was drawn: each candidate is fitted and scored by its RMS residual, the best one under a
//    threshold wins, and anything else is left as ink
//  - *where* it is: the fitted shape must sit where the user put it, which is a different question
//    from how well it fits.  In particular a quad's corners are NOT the drawn points where the pen
//    turned - a hand-drawn corner is rounded or overshot - they are the intersections of lines fitted
//    to the straight middle portions of adjacent sides.  See fitQuad() in shaperec.cpp.

#include "shaperecgeom.h"
#include <limits>
#include <string>
#include <vector>

namespace shaperec {

enum class Kind { None, Line, Quad, Ellipse, Scribble };
const char* kindName(Kind kind);

struct Params
{
  // --- hold ---
  // the pen held still at the end (the trigger) leaves a cluster of samples around one point; at least
  //  holdMinSamples within holdRadius units of the last sample are taken as the hold and replaced by
  //  their median, so they cannot add arc length, a hook or a corner
  double holdRadius = 3;
  int holdMinSamples = 6;

  // --- scratch-out ---
  // back-and-forth passes: at least this many reversals (3 = four passes); 4 took recall on messy
  //  scratch-outs from 99.7% down to 87%
  int scribbleMinReversals = 3;
  // a reversal must retreat this fraction of the stroke's extent along the scrub axis
  double scribbleHysteresis = 0.3;
  // when the mean pass direction is not a scratch-out, this many other scrub axes evenly spread over
  //  180 degrees are tried, for zigzags whose passes lean more than 45 degrees (0: only the mean) ...
  int scribbleAxisSteps = 11;
  // ... each only if the stroke's motion along it is at least this fraction of its arc length
  //  (passes leaning 60 degrees: 0.5; the wobble across a line: a few percent)
  double scribbleMinAlongFrac = 0.4;
  // every interior pass at least this fraction of the median pass length
  double scribbleMinPassFrac = 0.4;
  // passes are straight: median chord/arc, and the worst pass
  double scribbleMinPassStraightness = 0.85;
  double scribbleMinWorstStraightness = 0.65;
  // at a reversal the pen comes straight back: the lines of the two passes either side of it meet
  //  within this fraction of the pass length of each other, across the axis.  The sides of an arch
  //  (m, n) are an arch-width apart
  double scribbleMaxReversalGap = 0.2;
  // ... and at least this many reversals may turn up to the second limit: a scribble along a whole line
  //  of text is loose - loops, arches, spikes.  Below it, mmm and cursive are the strokes that turn
  //  wide; 16 reversals already let long synthetic mmm through above the false-erase gate
  int scribbleLongReversals = 20;
  double scribbleLongMaxReversalGap = 0.6;
  // the pass-length and reversal tests ignore their worst fraction: a scratch-out over a line of text
  //  (30 reversals) always has a few passes cut short and turns just over the limit.  The arches of mmm
  //  are every other reversal, far more than this
  double scribbleOutlierFrac = 0.2;

  // --- line ---
  // RMS distance from the fitted line, as a fraction of line length
  //  0.035 refused any stroke bowed more than 10-11% of its length, which a long line swung from the
  //  wrist easily is; 0.05 takes a light arc up to ~16% (about 70 degrees of arc)
  double lineMaxRms = 0.05;
  // largest distance of the middle part of the stroke from the line, as a fraction of line length
  double lineMaxDev = 0.13;
  // extent along the line / arc length; rejects back-and-forth strokes along one line
  double lineMinStraightness = 0.8;
  // a hook is a flick at either end whose direction differs from the line by more than this
  double hookMinAngleDeg = 35;
  // ... and which lies within this fraction of the arc length from the end
  double hookMaxFrac = 0.15;

  // --- closed shapes ---
  // distance between the stroke's end and its start (or the nearest pass by it) over the perimeter
  //  (0.2: a circle may stop ~70 degrees short; 0.15 refused quick circles stopping 55-60 short)
  double maxClosureGap = 0.25;
  int resampleCount = 128;
  // RMS distance from the fitted shape over the perimeter's equivalent radius (perimeter / 2pi);
  //  0.07 refused rectangles with bowed sides and corners missed by 3-4%
  double closedMaxErr = 0.09;
  // ellipses at least this round (minor/major) become circles; between circleEaseAspect and that, the
  //  radii are pulled part of the way together
  double circleMinAspect = 0.8;
  double circleEaseAspect = 0.65;
  // a turning-angle peak must exceed this to count as a quad corner
  double cornerMinTurnDeg = 45;
  // the same over a window 2.5x wider, tried when the fine scale does not find four corners.  Low on
  //  purpose: a circle turns ~50 degrees across this window everywhere, so this cannot tell a rounded
  //  corner from a circle - the residual comparison against the ellipse does that
  double coarseCornerMinTurnDeg = 40;
  // fraction of each side ignored at both ends when fitting its line - the rounded or overshot corner
  double sideTrim = 0.1;
  // fit a rectangle (right-angled corners, opposite sides parallel) rather than any quad
  bool rectangle = true;
  // ... refusing a stroke if one side leaves the rectangle's direction by more than this
  double rectMaxSideSkewDeg = 30;
  // a rectangle or ellipse tilted by up to this much is turned straight; up to twice this, turned part
  //  of the way
  double snapDeg = 10;
  // adjacent sides closer to parallel than this cannot form a corner
  double minCornerAngleDeg = 25;
  // an ellipse is refused if more than this fraction of the loop turns at less than straightTurnFrac
  //  of the rate a circle turns at (hexagons, rounded or not).  Synthetic circles reach ~0.3 and lopsided
  //  mouse circles 0.45; set loose, since a hexagon taken for a circle costs an undo, a circle missed
  //  costs a redraw
  double ellipseMaxStraightFrac = 0.5;
  double straightTurnFrac = 0.35;
};

struct Result
{
  Kind kind = Kind::None;
  // Line: [start, end] in drawing order.  Quad: 4 corners in drawing order.  Scribble: the area to
  //  erase, as a convex polygon (counter-clockwise in a y-up frame).
  std::vector<Vec2> points;
  // Ellipse: angle is the direction of radiusX in radians; circle == true means radiusX == radiusY
  Vec2 center;
  double radiusX = 0, radiusY = 0, angle = 0;
  bool circle = false;

  // diagnostics: the normalized residual of each candidate (infinity if it could not be fitted),
  //  the drawn points at the quad's turning peaks (what a naive recognizer would call the corners),
  //  and why the result was chosen or rejected
  static constexpr double NO_FIT = std::numeric_limits<double>::infinity();
  double lineErr = NO_FIT, quadErr = NO_FIT, ellipseErr = NO_FIT;
  std::vector<Vec2> rawCorners;
  std::string reason;
};

Result recognize(const std::vector<Vec2>& stroke, const Params& params = Params());

}  // namespace shaperec
