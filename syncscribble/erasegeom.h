#pragma once

// Eraser geometry: what an eraser of radius r, moved from one event position to the next, touches.
//
// Both erasers sweep a capsule (the set of points within r of the segment between two consecutive pointer
//  positions), never a sequence of circles, so a fast pass cannot step over a thin line between events.
//  The hit test is against the ink, not the path's centre line: a stroked path is hit within its half
//  width, a filled outline is hit anywhere inside it, and every segment of the path is tested - not just
//  its vertices, which is what used to make the stroke eraser miss the middle of a long straight line.
//
// Pure geometry with no dependency on Element/Page, so it tests standalone (scribbletest/erasetest.cpp).
//  Everything here is in one coordinate system; callers map into it first.

#include <vector>
#include "basics.h"
#include "ulib/path2d.h"

// squared distance between segments a0-a1 and b0-b1 (0 if they cross); either may be degenerate
Dim segmentDist2(Point a0, Point a1, Point b0, Point b1);

// true if the capsule of radius `radius` swept from a to b comes within `halfWidth` of any segment of
//  `path` (curves must already be flattened: only MoveTo/LineTo are read, everything else counts as
//  LineTo), or - if `filled` - if a lies inside any of path's subpaths (closed implicitly).  Testing a only
//  is enough for the inside case: if the capsule enters a subpath anywhere else it crosses its outline.
bool capsuleHitsPath(Point a, Point b, Dim radius, const Path2D& path, Dim halfWidth, bool filled);

// polygon approximating the capsule from a to b, with semicircular caps of `capSegs` edges each; vertices
//  lie on the true outline, so the polygon is never larger than the capsule and short of it by at most
//  radius*(1 - cos(pi/(2*capSegs))) - about 0.5% of the radius for the default 16
std::vector<Point> capsulePolygon(Point a, Point b, Dim radius, int capSegs = 16);
