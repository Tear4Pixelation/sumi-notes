#pragma once

// Stroke simplification at pen-up - see docs/agent/stroke-input.md, "Simplifying the finished stroke".
// Pure geometry (no Element, no document) so it can be unit tested without GL: scribbletest/simplifytest.cpp.
//
// The curve fit emits chords flat to CURVEFIT_TOL/zoom, about 5.5 outline points per input sample, and every
//  cost of a stroke (render, save, load, memory, eraser) scales with that.  This removes the points that do
//  not change the shape, once, after the stroke is complete - never on the live path, so pen-to-ink latency
//  is unchanged.  It runs on the *final* geometry (after the curve fit), which is what makes it safe:
//  simplifying the input before the fit, or with a loose tolerance, is what turns round strokes into facets.
//
// Douglas-Peucker with two bounds on every removed point, both measured on the geometry that is drawn:
//  - deviation <= tol: how far the dropped point is from the chord that replaces it (for an outline, both
//    sides separately; for a centre line + width, distance plus half the width error)
//  - deviation <= chord length * maxTurn/8: on a circular arc the sagitta of a chord of length L is
//    L^2/(8R) and the chord subtends L/R, so this caps the angle between consecutive kept chords at maxTurn
//    whatever the radius.  It is what keeps a small loop round at high zoom, where a pure distance bound
//    would happily replace it with a few long chords.
// End points (and for an outline, the cap pairs) are always kept, so caps are bit-identical; every kept
//  point is an original point, never a moved one.

#include <vector>
#include "ulib/path2d.h"

struct StrokeSimplifyParams
{
  double tol = 0;  // document units; 0 disables
  double maxTurn = 0;  // radians
  bool enabled() const { return tol > 0; }
};

// the user-facing presets (the "strokeSimplify" pref): 0 Off, 1 Invisible, 2 Balanced, 3 Aggressive
enum StrokeSimplifyPreset { SIMPLIFY_OFF = 0, SIMPLIFY_INVISIBLE, SIMPLIFY_BALANCED, SIMPLIFY_AGGRESSIVE };
StrokeSimplifyParams strokeSimplifyPreset(int preset);

// a flat pen's filled outline: path1 (n points) + end cap pair + reversed path2 (n points), simple path of
//  even size, point i paired with point size-1-i (the layout Element::toPenPoints() relies on).  Removes
//  pairs; returns true if anything was removed.
bool simplifyFlatOutline(Path2D& path, const StrokeSimplifyParams& params);

// a constant width stroked centre line (simple path: moveTo + lineTos).  Returns true if changed.
bool simplifyPolyline(Path2D& path, const StrokeSimplifyParams& params);

// centre line with a width per point (round and chisel pens): returns the indices to keep, always including
//  the first and last.  Error at a dropped point = distance to the chord + |width - interpolated width|/2.
std::vector<int> simplifyWidthPoints(const std::vector<Point>& pts, const std::vector<double>& widths,
    const StrokeSimplifyParams& params);
