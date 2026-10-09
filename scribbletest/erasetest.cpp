// Unit tests for the eraser geometry in syncscribble/erasegeom.cpp.  Like regiontest.cpp these need no GL
//  context and no document, so they also build and run on their own:
//
//   g++ -std=c++14 -O2 -DNDEBUG -I . -I syncscribble -isystem stb -DERASETEST_MAIN
//       scribbletest/erasetest.cpp syncscribble/erasegeom.cpp ulib/geom.cpp ulib/path2d.cpp
//       -o erasetest && ./erasetest
//   (one command, run from the repo root)
//
// runEraseTests() returns the number of failed checks and is also called from ScribbleTest::runAll().
//  What needs a document - the two erasers driven by input events at the real radius - is in
//  ScribbleTest::eraserHitTest().

#include <stdio.h>
#include <cmath>

#include "erasegeom.h"

static int nEraseChecksFailed = 0;

static void eraseCheck(bool condition, const char* what)
{
  if(!condition) {
    ++nEraseChecksFailed;
    printf("FAIL: erase geometry: %s\n", what);
  }
}

int runEraseTests()
{
  nEraseChecksFailed = 0;

  // segment distance
  eraseCheck(segmentDist2(Point(0, -1), Point(0, 1), Point(-1, 0), Point(1, 0)) == 0, "crossing segments touch");
  eraseCheck(std::abs(segmentDist2(Point(0, 2), Point(0, 5), Point(-100, 0), Point(100, 0)) - 4) < 1E-9,
      "distance to the middle of a long segment, not to its ends");
  eraseCheck(std::abs(segmentDist2(Point(3, 4), Point(3, 4), Point(0, 0), Point(0, 0)) - 25) < 1E-9,
      "two degenerate segments are two points");

  // a long line given by its two end points only: vertex-only tests miss its middle
  Path2D longLine;
  longLine.moveTo(Point(0, 0));
  longLine.lineTo(Point(1000, 0));
  eraseCheck(capsuleHitsPath(Point(500, 6), Point(500, 6), 7, longLine, 0, false),
      "a tap 6 from the middle of a long line hits it with radius 7");
  eraseCheck(!capsuleHitsPath(Point(500, 8), Point(500, 8), 7, longLine, 0, false),
      "...and 8 away does not");
  eraseCheck(capsuleHitsPath(Point(500, -50), Point(500, 50), 1, longLine, 0, false),
      "a single fast move straight across a long line hits it");
  // the ink, not the centre line: a stroke 10 wide is reached 5 further out
  eraseCheck(capsuleHitsPath(Point(400, 11), Point(600, 11), 7, longLine, 5, false),
      "running along the edge of a wide line hits it");
  eraseCheck(!capsuleHitsPath(Point(400, 13), Point(600, 13), 7, longLine, 5, false),
      "...and clear of its edge does not");

  // a filled outline is ink inside too, however far its edges are
  Path2D square;
  square.moveTo(Point(0, 0));
  square.lineTo(Point(100, 0));
  square.lineTo(Point(100, 100));
  square.lineTo(Point(0, 100));
  eraseCheck(capsuleHitsPath(Point(50, 50), Point(51, 50), 7, square, 0, true), "inside a filled outline hits it");
  eraseCheck(!capsuleHitsPath(Point(50, 50), Point(51, 50), 7, square, 0, false), "...but not an unfilled one");
  // the closing edge (100,100) -> (0,100) -> (0,0) is implicit
  eraseCheck(capsuleHitsPath(Point(-5, 50), Point(-5, 50), 7, square, 0, true), "a filled outline's closing edge counts");

  // a second subpath: the inside test is per subpath, so a later one is found too
  Path2D twoSquares = square;
  twoSquares.moveTo(Point(300, 0));
  twoSquares.lineTo(Point(400, 0));
  twoSquares.lineTo(Point(400, 100));
  twoSquares.lineTo(Point(300, 100));
  eraseCheck(capsuleHitsPath(Point(350, 50), Point(350, 50), 1, twoSquares, 0, true), "inside the second subpath hits");
  eraseCheck(!capsuleHitsPath(Point(200, 50), Point(200, 50), 1, twoSquares, 0, true), "between subpaths does not");

  // a dot (a lone point) is ink
  Path2D dot;
  dot.moveTo(Point(10, 10));
  eraseCheck(capsuleHitsPath(Point(0, 10), Point(5, 10), 6, dot, 0, false), "a dot is hit");

  // the capsule polygon: vertices on the outline, rounded (not square) ends
  std::vector<Point> poly = capsulePolygon(Point(0, 0), Point(100, 0), 10);
  bool onOutline = true;
  Dim maxX = -1E9;
  for(const Point& p : poly) {
    Dim d = std::sqrt(segmentDist2(p, p, Point(0, 0), Point(100, 0)));
    onOutline = onOutline && std::abs(d - 10) < 1E-9;
    maxX = std::max(maxX, p.x);
  }
  eraseCheck(onOutline, "every capsule vertex is radius from the swept segment");
  eraseCheck(std::abs(maxX - 110) < 1E-9, "the capsule reaches radius past its end, along its axis");
  eraseCheck(!pointInPolygon(poly, Point(108, 8)), "the capsule's end is round: its corner is not inside");
  eraseCheck(pointInPolygon(poly, Point(50, 9.9)), "the capsule's side is inside");

  return nEraseChecksFailed;
}

#ifdef ERASETEST_MAIN
int main()
{
  int nfailed = runEraseTests();
  printf(nfailed ? "%d erase geometry checks FAILED\n" : "All erase geometry checks passed\n", nfailed);
  return nfailed ? 1 : 0;
}
#endif
