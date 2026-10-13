#pragma once

// Draw Lines (ScribblePen::LINE_DRAWING), the straight-line assist - docs/agent/pen-and-tools.md
//  "Draw Lines: straight-line assist".  The pen draws freehand as usual; on pen-up a stroke that is
//  nearly straight - or that a few clear corners split into nearly straight pieces - is replaced by the
//  straight lines, each gently snapped to horizontal/vertical (or 45 degrees) when it is within a few
//  degrees.  Anything else (curves, handwriting) is left exactly as drawn: the test is all or nothing.
//
// Pure geometry with no dependency on Element/Page, so it tests standalone (scribbletest/straightentest.cpp).
//  The points are in a ruling frame's local coordinates, so "horizontal" is along the frame's lines.

#include <vector>
#include "basics.h"
#include "ulib/geom.h"

struct LineStraightenParams
{
  // a piece is straight when no point is further from its chord than this fraction of the chord's length
  //  (a 100 unit line may wobble 4 units).  An arc passes up to about 18 degrees of turn.
  Dim maxDeviation = 0.04;
  // ... and its drawn length is at most this times the chord: more, and it doubles back on itself
  Dim maxPathRatio = 1.15;
  // strokes with a drawn length below this (absolute; the caller passes a screen size in document units)
  //  are left alone - dots and tiny ticks
  Dim minLength = 0;
  // pieces: at most this many, every one at least this fraction of the stroke's drawn length, and the
  //  direction turning at least minCornerDeg at each corner.  A bend gentler than that is a curve.
  int maxSegments = 3;
  Dim minSegmentFrac = 0.12;
  Dim minCornerDeg = 35;
  // a piece within this many degrees of horizontal or vertical becomes exactly that; 45 degree diagonals
  //  get a tighter window, since a diagonal drawn on purpose at 42 degrees is more likely than at 88
  Dim axisSnapDeg = 4;
  Dim diagonalSnapDeg = 2.5;
  // off for a grid-snapped pen, whose corners are grid points that a rotation would move off the grid
  bool snapAngles = true;
};

// The straight stroke: its vertices (2 for a single line, one more per corner), and for each vertex the
//  index of the input point it stands for.  No vertices: leave the stroke as drawn.
struct StraightenedStroke
{
  std::vector<Point> vertices;
  std::vector<int> indices;
  bool empty() const { return vertices.empty(); }
};

StraightenedStroke straightenStroke(const std::vector<Point>& pts, const LineStraightenParams& params);

// Every input point slid onto the straight stroke, so a pressure pen keeps its pressure along the line:
//  a point between vertices i and i+1 lands at the same fraction of the new piece as its projection was
//  along the old chord.  Points that would step backwards (jitter along the line) are dropped; the vertices
//  are always kept.  Returns the positions and fills `kept` with the input index of each.
std::vector<Point> projectOntoStraightened(const std::vector<Point>& pts, const StraightenedStroke& line,
    std::vector<int>& kept);
