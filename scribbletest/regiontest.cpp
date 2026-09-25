// Unit tests for the ruling-region geometry in syncscribble/rulingregion.cpp.  Like shapetest.cpp these
//  need no GL context and no document, so they also build and run on their own:
//
//   g++ -std=c++14 -O2 -DNDEBUG -I . -I syncscribble -isystem stb -DREGIONTEST_MAIN
//       scribbletest/regiontest.cpp syncscribble/rulingregion.cpp ulib/geom.cpp ulib/path2d.cpp
//       -o regiontest && ./regiontest
//   (one command, run from the repo root; NDEBUG for the same reason as shapetest.cpp)
//
// runRegionTests() returns the number of failed checks and is also called from ScribbleTest::runAll().

#include <stdio.h>

#include "rulingregion.h"

static int nRegionChecksFailed = 0;

static void regionCheck(bool condition, const char* what)
{
  if(!condition) {
    ++nRegionChecksFailed;
    printf("FAIL: %s\n", what);
  }
}

static bool nearPt(Point a, Point b, Dim eps = 1E-6) { return std::abs(a.x - b.x) <= eps && std::abs(a.y - b.y) <= eps; }

// number of line segments (moveTo/lineTo pairs) in a path built from addLine
static int segmentCount(const Path2D& path) { return path.size()/2; }

int runRegionTests()
{
  nRegionChecksFailed = 0;

  // an axis-aligned region: lines strictly inside, never on the border
  RulingRegionParams box = RulingRegionParams::fromRect(Rect::ltrb(0, 0, 100, 100), 0, 25);
  Path2D lines = box.linesPath();
  regionCheck(segmentCount(lines) == 3, "100 unit box at pitch 25 has 3 lines (25, 50, 75), not its borders");
  regionCheck(lines.size() >= 2 && nearPt(lines.point(0), Point(0, 25)) && nearPt(lines.point(1), Point(100, 25)),
      "first line spans the full width at y = 25");
  box.xRuling = 20;
  regionCheck(segmentCount(box.linesPath()) == 3 + 4, "x ruling adds 4 vertical lines at pitch 20");

  // the frame round trips, rotated or not
  RulingFrame frame(Point(10, 20), 0.3, 15, 25);
  Point p(37, -12);
  regionCheck(nearPt(frame.toPage(frame.toLocal(p)), p), "toPage(toLocal(p)) == p");
  regionCheck(frame.line(frame.toPage(Point(5, 2*25 + 1)), 40) == 2, "a point just inside band 2 is on line 2");
  regionCheck(frame.line(frame.toPage(Point(5, -1)), 40) == -1, "a point above the origin is on line -1");

  // transform: whatever the region does, a point on a band stays on that band ("ink stays on its lines")
  RulingRegionParams region = RulingRegionParams::fromRect(Rect::ltrb(50, 50, 250, 150), 0, 20);
  Point ink(120, 50 + 3*20 + 7);  // on band 3
  int before = region.frame().line(ink, 40);
  Transform2D tf = Transform2D().rotate(0.4, Point(150, 100));
  tf = Transform2D::translating(Point(33, -8)) * Transform2D().scale(1.5) * tf;
  region.transform(tf);
  regionCheck(region.frame().line(tf.map(ink), 40) == before, "ink transformed with its region stays on its band");
  regionCheck(std::abs(region.yRuling - 30) < 1E-6, "uniform scale 1.5 scales the pitch 20 -> 30");
  regionCheck(std::abs(region.angle - 0.4) < 1E-9, "rotation 0.4 rad lands in the ruling's angle");
  regionCheck(segmentCount(region.linesPath()) == 4, "a transformed 200x100 region at pitch 20 still has 4 lines");

  // rotated region: every line segment is parallel to the ruling and inside the outline
  {
    Path2D rl = region.linesPath();
    bool ok = rl.size() > 0;
    Point dir = region.frame().toPageDir(Point(1, 0));
    for(int ii = 0; ii + 1 < rl.size(); ii += 2) {
      Point seg = rl.point(ii+1) - rl.point(ii);
      ok = ok && std::abs(cross(seg, dir)) < 1E-6*seg.dist();
      Point mid = (rl.point(ii) + rl.point(ii+1))/2;
      ok = ok && region.contains(mid);
    }
    regionCheck(ok, "rotated region's lines are parallel to its ruling and inside its outline");
  }

  // a free-cornered (non-rectangular) outline keeps the lines parallel and evenly spaced - the lines
  //  never bend to follow the outline
  {
    RulingRegionParams skew = RulingRegionParams::fromRect(Rect::ltrb(0, 0, 100, 100), 0, 10);
    skew.corners[2] = Point(140, 120);  // drag the bottom-right corner out
    Path2D sl = skew.linesPath();
    bool ok = segmentCount(sl) > 0;
    for(int ii = 0; ii + 1 < sl.size(); ii += 2)
      ok = ok && std::abs(sl.point(ii).y - sl.point(ii+1).y) < 1E-9 && std::abs(std::fmod(sl.point(ii).y, 10)) < 1E-9;
    regionCheck(ok, "dragging a corner leaves the lines horizontal and on the 10 unit pitch");
  }

  // a non-convex outline: one line crossing a notch gives two segments
  {
    RulingRegionParams notch;
    notch.corners = { Point(0,0), Point(100,0), Point(100,100), Point(60,100), Point(60,40), Point(40,40),
        Point(40,100), Point(0,100) };
    notch.yRuling = 50;
    Path2D nl = notch.linesPath();
    regionCheck(segmentCount(nl) == 2, "a line across a notch is clipped into two segments");
  }

  // dots: 4x4 interior grid points of a 100 box at 20, none on the border
  {
    RulingRegionParams dots = RulingRegionParams::fromRect(Rect::ltrb(0, 0, 100, 100), 20, 20, 1.5);
    regionCheck(dots.linesPath().empty(), "dotted region has no lines");
    regionCheck(!dots.dotsPath().empty(), "dotted region has dots");
  }

  // the "..." button corner follows rotation: after a half turn the stored corner 0 is bottom right
  {
    RulingRegionParams turn = RulingRegionParams::fromRect(Rect::ltrb(0, 0, 100, 50), 0, 10);
    regionCheck(turn.bottomRightCorner() == 2, "unrotated: corner 2 is bottom right");
    turn.transform(Transform2D().rotate(M_PI, Point(50, 25)));
    regionCheck(turn.bottomRightCorner() == 2, "rotated with its ruling, the same corner stays bottom right");
    turn.angle = 0;  // ruling reset upright while the outline stays turned
    regionCheck(turn.bottomRightCorner() == 0, "ruling upright over a turned outline: corner 0 is now bottom right");
  }

  // a mirror cannot be expressed; the frame must keep local y pointing down the (mirrored) ink's lines
  //  rather than turn upside down, or a band's line numbers would run up the page
  {
    RulingRegionParams m = RulingRegionParams::fromRect(Rect::ltrb(0, 0, 100, 100), 0, 20);
    Point ink(50, 3*20 + 5);  // band 3
    Transform2D flip = Transform2D().scale(-1, 1);
    m.transform(flip);
    regionCheck(m.frame().line(flip.map(ink), 40) == 3, "a horizontally mirrored region keeps its line numbers");
  }

  // serialization
  {
    std::vector<Point> pts = { Point(1.5, -2.25), Point(1234.5678, 0.001) }, back;
    parseRegionPoints(serializeRegionPoints(pts).c_str(), back);
    regionCheck(back.size() == 2 && nearPt(back[0], pts[0]) && nearPt(back[1], pts[1], 1E-3), "points round trip");
  }

  // snap to grid in a rotated frame lands on a grid point of that frame
  {
    RulingFrame gf(Point(0, 0), 0.5, 10, 10);
    Point snapped = gf.snapToGrid(Point(23, 41), 40);
    Point local = gf.toLocal(snapped);
    regionCheck(std::abs(local.x - 10*std::round(local.x/10)) < 1E-9 && std::abs(local.y - 10*std::round(local.y/10)) < 1E-9,
        "snapToGrid lands on the rotated grid");
  }

  // a stroke begun in a region is held at its edge
  {
    RulingRegionParams r = RulingRegionParams::fromRect(Rect::ltrb(0, 0, 100, 50), 0, 25);
    regionCheck(nearPt(r.clampInside(Point(30, 20)), Point(30, 20)), "clampInside leaves an inside point alone");
    regionCheck(nearPt(r.clampInside(Point(130, 20)), Point(100, 20)), "past the right edge lands on it, same height");
    regionCheck(nearPt(r.clampInside(Point(40, -9)), Point(40, 0)), "above the top edge lands on it, same x");
    regionCheck(nearPt(r.clampInside(Point(120, 70)), Point(100, 50)), "off a corner lands on the corner");
    r.transform(Transform2D().rotate(0.5, Point(50, 25)));
    Point outside = r.frame().toPage(Point(50, -30));
    Point held = r.clampInside(outside);
    regionCheck(std::abs(r.frame().toLocal(held).y) < 1E-9 && std::abs(r.frame().toLocal(held).x - 50) < 1E-9,
        "on a rotated region, the nearest point is on the rotated edge");
    regionCheck(r.outline, "new regions are outlined");
  }

  return nRegionChecksFailed;
}

#ifdef REGIONTEST_MAIN
int main()
{
  int failed = runRegionTests();
  printf("%d region checks failed\n", failed);
  return failed ? 1 : 0;
}
#endif
