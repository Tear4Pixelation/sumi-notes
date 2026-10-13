// Unit tests for the Draw Lines straight-line assist in syncscribble/linestraighten.cpp
//  (docs/agent/pen-and-tools.md, "Draw Lines: straight-line assist").  Like smartscaletest.cpp these need
//  no GL context and no document, so they also build and run on their own:
//
//   g++ -std=c++14 -O2 -DNDEBUG -I . -I syncscribble -isystem stb -DSTRAIGHTENTEST_MAIN
//       scribbletest/straightentest.cpp syncscribble/linestraighten.cpp ulib/geom.cpp ulib/path2d.cpp
//       -o straightentest && ./straightentest
//   (one command, run from the repo root)
//
// runStraightenTests() returns the number of failed checks and is also called from ScribbleTest::runAll().
//  Each check says what an implementation without the feature (or without that part of it) would give:
//  "as drawn" means the stroke's own points, which is what the pen produced before the assist, and the old
//  rubber band (one segment, pen-down to pen-up, for every stroke) is the "straightens everything" case.

#include <stdio.h>
#include <cmath>

#include "linestraighten.h"

static int nStraightenChecksFailed = 0;

static void straightenCheck(bool condition, const char* what)
{
  if(!condition) {
    ++nStraightenChecksFailed;
    printf("FAIL: %s\n", what);
  }
}

// `count` samples from `from` to `to`, pushed sideways by a sine wobble of `wobble` units with `waves`
//  whole periods - zero at both ends, so the ends are exactly `from` and `to`
static std::vector<Point> straightenWobblyLine(Point from, Point to, int count, Dim wobble, int waves = 3)
{
  std::vector<Point> pts;
  Point side = normal(to - from);
  for(int ii = 0; ii < count; ++ii) {
    Dim frac = Dim(ii)/(count - 1);
    pts.push_back(from + (to - from)*frac + side*(wobble*std::sin(frac*waves*2*M_PI)));
  }
  pts.back() = to;
  return pts;
}

static std::vector<Point> straightenArc(Point centre, Dim radius, Dim startDeg, Dim endDeg, int count)
{
  std::vector<Point> pts;
  for(int ii = 0; ii < count; ++ii) {
    Dim rad = degToRad(startDeg + (endDeg - startDeg)*ii/(count - 1));
    pts.push_back(centre + Point(std::cos(rad), std::sin(rad))*radius);
  }
  return pts;
}

int runStraightenTests()
{
  nStraightenChecksFailed = 0;
  LineStraightenParams params;

  // a nearly straight stroke at an angle no snap applies to (16.7 degrees): one line, pen-down to pen-up.
  //  As drawn it would be its 41 points.
  {
    std::vector<Point> pts = straightenWobblyLine(Point(0, 0), Point(100, 30), 41, 1.0);
    StraightenedStroke line = straightenStroke(pts, params);
    straightenCheck(line.vertices.size() == 2, "nearly straight: becomes a 2 point line");
    straightenCheck(line.vertices.size() == 2 && line.vertices[0] == pts.front() && line.vertices[1] == pts.back(),
        "nearly straight, unsnapped angle: the line runs from the first to the last point");
    straightenCheck(line.indices.size() == 2 && line.indices[0] == 0 && line.indices[1] == 40,
        "nearly straight: vertices stand for the first and last input point");
    // the points a pressure pen is rebuilt from all lie on that line, in order, ends included
    std::vector<int> kept;
    std::vector<Point> moved = projectOntoStraightened(pts, line, kept);
    bool onLine = moved.size() == kept.size() && moved.size() >= 2;
    for(size_t ii = 0; onLine && ii < moved.size(); ++ii) {
      onLine = std::abs(cross(moved[ii] - pts.front(), pts.back() - pts.front())) < 1E-9;
      if(ii > 0)
        onLine = onLine && kept[ii] > kept[ii-1] && moved[ii].x > moved[ii-1].x;
    }
    straightenCheck(onLine, "projected points lie on the line, in order");
    straightenCheck(!kept.empty() && kept.front() == 0 && kept.back() == 40, "projection keeps both ends");
  }

  // 2.3 degrees off horizontal: exactly horizontal, the start where the pen came down.  Without the snap
  //  (only the straightening) the end would stay at y = 4.
  {
    std::vector<Point> pts = straightenWobblyLine(Point(10, 20), Point(110, 24), 41, 0.5);
    StraightenedStroke line = straightenStroke(pts, params);
    straightenCheck(line.vertices.size() == 2 && line.vertices[0] == Point(10, 20) && line.vertices[1].y == 20
        && std::abs(line.vertices[1].x - 110) < 1E-9, "near horizontal: snapped to exactly horizontal");
    // a grid pen keeps its (grid point) ends: straightened, not turned
    LineStraightenParams gridParams = params;
    gridParams.snapAngles = false;
    StraightenedStroke gridLine = straightenStroke(pts, gridParams);
    straightenCheck(gridLine.vertices.size() == 2 && gridLine.vertices[1] == Point(110, 24),
        "angle snapping off: the end stays put");
  }

  // near vertical (88.3 degrees, drawn upwards) and near 45 degrees (45.6): exactly vertical, exactly diagonal
  {
    StraightenedStroke up = straightenStroke(straightenWobblyLine(Point(0, 0), Point(3, -100), 41, 0.5), params);
    straightenCheck(up.vertices.size() == 2 && up.vertices[1].x == 0 && up.vertices[1].y < -99,
        "near vertical: snapped to exactly vertical");
    StraightenedStroke diag = straightenStroke(straightenWobblyLine(Point(0, 0), Point(100, 102), 41, 0.5), params);
    straightenCheck(diag.vertices.size() == 2 && diag.vertices[1].x == diag.vertices[1].y,
        "near diagonal: snapped to exactly 45 degrees");
    // 8 degrees off horizontal is a slope drawn on purpose
    StraightenedStroke slope = straightenStroke(straightenWobblyLine(Point(0, 0), Point(100, 14), 41, 0.5), params);
    straightenCheck(slope.vertices.size() == 2 && slope.vertices[1] == Point(100, 14),
        "8 degrees off horizontal: straightened but not snapped");
  }

  // curves stay as drawn.  The rubber band (or a straightener with no deviation test) made each a line.
  {
    straightenCheck(straightenStroke(straightenArc(Point(0, 0), 50, 0, 90, 40), params).empty(),
        "quarter circle: left as drawn");
    straightenCheck(straightenStroke(straightenArc(Point(0, 0), 50, 0, 180, 60), params).empty(),
        "half circle: left as drawn");
    straightenCheck(straightenStroke(straightenArc(Point(0, 0), 50, 0, 360, 80), params).empty(),
        "closed circle: left as drawn");
    // a gentle bow (30 degrees of arc): bent too much for one line, and splitting it would need a corner
    //  of 15 degrees - without the corner angle test it would come out as two straight pieces
    straightenCheck(straightenStroke(straightenArc(Point(0, 0), 200, 75, 105, 40), params).empty(),
        "gentle bow: left as drawn, no false corner");
  }

  // handwriting-like squiggles stay as drawn
  {
    std::vector<Point> wave;
    for(int ii = 0; ii < 200; ++ii)
      wave.push_back(Point(ii*0.5, 10*std::sin(ii*0.5*2*M_PI/20)));
    straightenCheck(straightenStroke(wave, params).empty(), "wavy handwriting: left as drawn");
    // cursive loops: a prolate cycloid, like "llll"
    std::vector<Point> loops;
    for(int ii = 0; ii < 240; ++ii) {
      Dim phase = ii*4*2*M_PI/239;
      loops.push_back(Point(4*phase - 12*std::sin(phase), -12*std::cos(phase)));
    }
    straightenCheck(straightenStroke(loops, params).empty(), "looped handwriting: left as drawn");
    // a stroke that goes out and comes back along the same line (an underline scrubbed back and forth)
    std::vector<Point> back = straightenWobblyLine(Point(0, 0), Point(100, 0), 21, 0.3);
    std::vector<Point> forth = straightenWobblyLine(Point(100, 0), Point(30, 0), 15, 0.3);
    back.insert(back.end(), forth.begin() + 1, forth.end());
    straightenCheck(straightenStroke(back, params).empty(), "doubling back: left as drawn");
  }

  // an "L": two pieces meeting at the corner, each snapped - the vertical one ends at x = 0 rather than 2,
  //  the horizontal one stays at the corner's y rather than rising to 103.  Without corner support the
  //  stroke is not straight as a whole and would be left as drawn (empty).
  {
    std::vector<Point> pts = straightenWobblyLine(Point(0, 0), Point(2, 100), 41, 0.5);
    std::vector<Point> foot = straightenWobblyLine(Point(2, 100), Point(82, 103), 33, 0.5);
    pts.insert(pts.end(), foot.begin() + 1, foot.end());
    StraightenedStroke line = straightenStroke(pts, params);
    straightenCheck(line.vertices.size() == 3, "L: two straight pieces");
    straightenCheck(line.indices.size() == 3 && line.indices[1] == 40, "L: the corner is the corner sample");
    straightenCheck(line.vertices.size() == 3 && line.vertices[1].x == 0 && line.vertices[2].y == line.vertices[1].y,
        "L: vertical and horizontal exactly");
    std::vector<int> kept;
    std::vector<Point> moved = projectOntoStraightened(pts, line, kept);
    bool cornerKept = false;
    for(size_t ii = 0; ii < kept.size(); ++ii)
      cornerKept = cornerKept || (kept[ii] == 40 && moved[ii] == line.vertices[1]);
    straightenCheck(cornerKept && moved.back() == line.vertices[2], "L: projection passes through the corner");
  }

  // too short to be meant as a line (minLength), and a single point
  {
    LineStraightenParams minParams = params;
    minParams.minLength = 20;
    straightenCheck(straightenStroke(straightenWobblyLine(Point(0, 0), Point(10, 1), 11, 0.1), minParams).empty(),
        "a tick shorter than minLength: left as drawn");
    straightenCheck(straightenStroke({Point(5, 5)}, params).empty(), "a single point: left as drawn");
    straightenCheck(straightenStroke({Point(5, 5), Point(5, 5)}, params).empty(), "a dot: left as drawn");
  }

  return nStraightenChecksFailed;
}

#ifdef STRAIGHTENTEST_MAIN
int main()
{
  int failed = runStraightenTests();
  printf(failed ? "%d straighten checks failed\n" : "all straighten checks passed\n", failed);
  return failed ? 1 : 0;
}
#endif
