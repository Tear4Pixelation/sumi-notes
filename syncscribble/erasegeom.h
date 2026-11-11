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

// How much of some ink lies inside a polygon, as a fraction of its length - the scratch-out's measure of
//  whether a stroke was scratched out or only grazed (ScribbleArea's ScratchOutSelector).  Accumulated
//  over every path of an element (a group has several).  Length, not point count: pen points are dense in
//  curves and sparse along straight runs, so a count would weigh the same ink differently by its shape.
struct LengthInside
{
  Dim inside = 0;  // length of the path within the polygon
  Dim total = 0;  // the path's whole length
  // subpaths of no length (a tapped dot), counted only if nothing has any length
  int dots = 0;
  int dotsInside = 0;
  // 0..1; a path of no length at all is all in or all out by its points
  Dim fraction() const { return total > 0 ? inside/total : (dots > 0 ? Dim(dotsInside)/dots : 0); }
};

// adds `path` (flattened, read as in capsuleHitsPath) to `acc`; `closed` counts each subpath's closing edge
//  too (a filled outline's).  `polygon` is closed implicitly and tested even-odd.
void addLengthInside(LengthInside& acc, const Path2D& path, bool closed, const std::vector<Point>& polygon);
