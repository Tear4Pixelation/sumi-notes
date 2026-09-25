#pragma once

// A test stroke plus what the user meant by it, and the plain-text file format recorder.html writes:
//
//   # shape-recognition strokes v1
//   stroke <name>
//   label square
//   truth x0 y0 x1 y1 x2 y2 x3 y3
//   points x y x y x y ...
//   end
//
// label is line, square, circle or scribble.  truth is optional (a stroke drawn freely has a label but
//  no known position) and holds: line - start and end; square - four corners in drawing order;
//  circle - centre x, centre y, radius; ellipse - centre x, centre y, radius x, radius y, angle of
//  radius x in radians; scribble - the outline of the area scratched out; other - nothing.

#include "geom.h"
#include <string>
#include <vector>

struct TestStroke
{
  std::string name;
  std::string label;
  std::vector<double> truth;
  std::vector<Vec2> points;
  std::string variant;  // synthetic only: how the generator drew it, for the report
};

bool readStrokeFile(const std::string& path, std::vector<TestStroke>& out, std::string& error);
bool writeStrokeFile(const std::string& path, const std::vector<TestStroke>& strokes);
