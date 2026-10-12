// Unit tests for the crop dialog's shape math in syncscribble/cropshape.cpp (docs/agent/screenshot.md):
//  rounded outlines, the coverage mask, and the __crop attribute format.  No GL and no document, so they
//  also build and run on their own:
//
//   g++ -std=c++14 -O2 -DNDEBUG -I . -I syncscribble -isystem stb -DCROPSHAPETEST_MAIN
//       scribbletest/cropshapetest.cpp syncscribble/cropshape.cpp ulib/geom.cpp -o cropshapetest
//       && ./cropshapetest
//   (one command, run from the repo root; NDEBUG for the same reason as shapetest.cpp)
//
// runCropShapeTests() returns the number of failed checks and is also called from ScribbleTest::runAll().

#include <stdio.h>
#include <algorithm>
#include <vector>

#include "cropshape.h"

static int nCropChecksFailed = 0;

static void cropCheck(bool condition, const char* what)
{
  if(!condition) {
    ++nCropChecksFailed;
    printf("FAIL: %s\n", what);
  }
}

// an opaque mid-gray w x h buffer, masked by the outline; returns the alpha channel
static std::vector<int> maskedAlpha(int w, int h, const std::vector<Point>& outline)
{
  std::vector<unsigned char> rgba(size_t(w)*h*4, 128);
  for(size_t ii = 3; ii < rgba.size(); ii += 4)
    rgba[ii] = 255;
  applyOutlineMask(rgba.data(), w, h, outline);
  std::vector<int> alpha(size_t(w)*h);
  for(size_t ii = 0; ii < alpha.size(); ++ii)
    alpha[ii] = rgba[ii*4 + 3];
  return alpha;
}

int runCropShapeTests()
{
  nCropChecksFailed = 0;

  // plain rectangle: inside opaque, outside cleared (color too), right/bottom edges exclusive
  {
    CropShape shape;
    shape.rect = Rect::ltrb(2, 2, 8, 8);
    std::vector<unsigned char> rgba(10*10*4, 200);
    applyOutlineMask(rgba.data(), 10, 10, roundedOutline(shape.corners(), 0));
    auto at = [&](int x, int y, int ch){ return int(rgba[(y*10 + x)*4 + ch]); };
    cropCheck(at(5, 5, 3) == 200, "rect: inside pixel keeps its alpha");
    cropCheck(at(2, 2, 3) == 200, "rect: first pixel inside the edge is covered");
    cropCheck(at(8, 5, 3) == 0 && at(1, 1, 3) == 0, "rect: pixels outside are transparent");
    cropCheck(at(1, 1, 0) == 0, "rect: pixels outside lose their color (no stray RGB under alpha 0)");
  }

  // half-covered pixel: anti-aliased, not thresholded
  {
    std::vector<Point> rect = {Point(2.5, 0), Point(8, 0), Point(8, 10), Point(2.5, 10)};
    std::vector<int> alpha = maskedAlpha(10, 10, rect);
    cropCheck(alpha[5*10 + 2] > 64 && alpha[5*10 + 2] < 192, "mask: an edge through a pixel's middle gives partial alpha");
  }

  // a self-crossing polygon (pentagram drawn point to point) is filled solid: nonzero winding, where
  //  even-odd would punch the middle out
  {
    std::vector<Point> star;
    const int order[5] = {0, 2, 4, 1, 3};
    for(int ii = 0; ii < 5; ++ii) {
      real theta = -1.5707963 + order[ii]*2*3.14159265/5;
      star.push_back(Point(50 + 45*std::cos(theta), 50 + 45*std::sin(theta)));
    }
    std::vector<int> alpha = maskedAlpha(100, 100, star);
    cropCheck(alpha[50*100 + 50] == 255, "polygon: the middle of a self-crossing star is kept (nonzero winding)");
    cropCheck(alpha[2*100 + 2] == 0, "polygon: outside the star is transparent");
  }

  // rounded rectangle: the corner is cut along the arc
  {
    CropShape shape;
    shape.rect = Rect::ltrb(0, 0, 100, 100);
    shape.radius = 20;
    std::vector<int> alpha = maskedAlpha(100, 100, roundedOutline(shape.corners(), shape.radius, 0.5));
    cropCheck(alpha[1*100 + 1] == 0, "rounded: the very corner is transparent");
    cropCheck(alpha[4*100 + 4] == 0, "rounded: a pixel just outside the arc is transparent");
    cropCheck(alpha[7*100 + 7] > 128, "rounded: a pixel just inside the arc is kept");
    cropCheck(alpha[50*100 + 50] == 255 && alpha[0*100 + 50] == 255, "rounded: the middle and the straight edges are kept");
  }

  // a radius larger than the edges allow is clamped: a square becomes a circle, not a bow tie
  {
    std::vector<Point> square = {Point(0, 0), Point(40, 0), Point(40, 40), Point(0, 40)};
    std::vector<Point> outline = roundedOutline(square, 100, 0.5);
    real worst = 0;
    for(const Point& pt : outline)
      worst = std::max(worst, std::abs(pt.dist(Point(20, 20)) - 20));
    cropCheck(outline.size() > 8 && worst < 0.01, "rounded: an oversized radius clamps to half the shortest edge");
  }

  // a concave corner is rounded too, which fills in the notch next to it
  {
    std::vector<Point> lshape = {Point(0, 0), Point(40, 0), Point(40, 20), Point(20, 20), Point(20, 40), Point(0, 40)};
    std::vector<int> sharp = maskedAlpha(40, 40, roundedOutline(lshape, 0));
    std::vector<int> round = maskedAlpha(40, 40, roundedOutline(lshape, 8, 0.5));
    cropCheck(sharp[21*40 + 21] == 0, "concave: without rounding the notch is empty");
    cropCheck(round[21*40 + 21] == 255, "concave: rounding the inner corner fills the notch beside it");
    cropCheck(round[30*40 + 30] == 0, "concave: the notch beyond the fillet stays empty");
  }

  // pixel bounds: floor/ceil of the outline, clipped to the image
  {
    std::vector<Point> outline = {Point(2.3, 1.7), Point(7.2, 1.7), Point(7.2, 9.9)};
    Rect bounds = cropPixelBounds(outline, 8, 8);
    cropCheck(bounds.left == 2 && bounds.top == 1 && bounds.right == 8 && bounds.bottom == 8, "bounds: rounded out and clipped to the image");
  }

  // the __crop attribute round-trips, and bad input is refused
  {
    CropShape poly;
    poly.kind = CropShape::POLYGON;
    poly.radius = 12.5;
    poly.points = {Point(1, 2), Point(30.25, 4), Point(15, 40.5)};
    CropShape parsed;
    bool ok = CropShape::parse(poly.serialize().c_str(), &parsed);
    cropCheck(ok && parsed.kind == CropShape::POLYGON && parsed.points.size() == 3 && parsed.radius == real(12.5)
        && parsed.points[1] == Point(30.25, 4), "attr: a polygon round-trips");
    CropShape rect;
    rect.rect = Rect::ltrb(3, 4, 50, 60);
    ok = CropShape::parse(rect.serialize().c_str(), &parsed);
    cropCheck(ok && parsed.kind == CropShape::RECT && parsed.rect == rect.rect && parsed.radius == 0, "attr: a rect round-trips");
    cropCheck(!CropShape::parse("poly 0 1 2 3 4", &parsed), "attr: a polygon of two points is refused");
    cropCheck(!CropShape::parse("oval 0 1 2 3 4", &parsed), "attr: an unknown kind is refused");
    cropCheck(rect.isWholeImage(50, 60) == false && CropShape{CropShape::RECT, Rect::wh(50, 60)}.isWholeImage(50, 60),
        "attr: only the full, unrounded rect counts as no edit");
  }

  return nCropChecksFailed;
}

#ifdef CROPSHAPETEST_MAIN
int main()
{
  int failed = runCropShapeTests();
  printf("%d crop shape checks failed\n", failed);
  return failed ? 1 : 0;
}
#endif
