#pragma once

#include <string>
#include <vector>
#include "ulib/geom.h"

// The shape an image is cropped to in the crop dialog (docs/agent/screenshot.md): a rectangle or a polygon
//  the user placed point by point, either with its corners rounded.  Pure math - no GL, no Image - so it
//  is tested standalone (scribbletest/cropshapetest.cpp).  All coordinates are pixels of the source image.

struct CropShape
{
  enum Kind { RECT, POLYGON };
  Kind kind = RECT;
  Rect rect;                  // RECT
  std::vector<Point> points;  // POLYGON, in placing order; closed back to the first
  real radius = 0;            // corner radius, both kinds; clamped per corner to what the edges allow

  // a polygon needs three points to enclose anything; a rect needs some area
  bool isValid() const;
  // the plain rectangle covering the whole of a w x h image, i.e. no edit at all
  bool isWholeImage(int w, int h) const;
  // the shape's corners before rounding (a rect's four, clockwise from top left on screen)
  std::vector<Point> corners() const;

  // "rect R l t r b" / "poly R x1 y1 x2 y2 ..." (R = radius); the __crop attribute of an edited image
  std::string serialize() const;
  static bool parse(const char* str, CropShape* out);
};

// The outline to fill: the corners with each one replaced by a circular arc of `radius` (shrunk where the
//  adjacent edges are too short for it - a fillet never reaches past the middle of an edge), flattened to
//  line segments at most `maxSegLen` long.  A concave corner is rounded too, which adds area there.
std::vector<Point> roundedOutline(const std::vector<Point>& corners, real radius, real maxSegLen = 1);

// integer pixel bounds of the outline, intersected with a w x h image: the size of the cropped result
Rect cropPixelBounds(const std::vector<Point>& outline, int w, int h);

// Multiplies the alpha of every pixel of a straight-alpha RGBA buffer (w x h, rows 4*w bytes) by its
//  coverage by `outline` (nonzero winding, so a self-crossing polygon is filled solid), sampled on a
//  `samples` x `samples` grid per pixel for anti-aliased edges.  The outline is in the buffer's pixel
//  coordinates (pixel (x, y) covers [x, x+1) x [y, y+1)).  Pixels with no coverage become (0,0,0,0).
void applyOutlineMask(unsigned char* rgba, int w, int h, const std::vector<Point>& outline, int samples = 4);
