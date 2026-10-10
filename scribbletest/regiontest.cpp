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
#include <algorithm>

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
    Point mirroredInk(50, 3*20 + 5);  // band 3
    Transform2D flip = Transform2D().scale(-1, 1);
    m.transform(flip);
    regionCheck(m.frame().line(flip.map(mirroredInk), 40) == 3, "a horizontally mirrored region keeps its line numbers");
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

  // Insert Lines' zones (second-day report), pitch 40 so the snap band (1/8) is 5 either side of a rule
  {
    const Dim yr = 40;
    InsertLinesStart start = insertLinesStart(3.5*yr, yr);
    regionCheck(start.line == 3 && !start.wholeLine, "mid-line: split line 3 at the pen");
    start = insertLinesStart(3*yr + 3, yr);
    regionCheck(start.line == 3 && start.wholeLine, "just below rule 3: the whole of line 3 down");
    start = insertLinesStart(3*yr - 3, yr);
    regionCheck(start.line == 3 && start.wholeLine, "just above rule 3 (bottom of line 2): the whole of line 3 down");
    start = insertLinesStart(4*yr - 0.1*yr, yr);
    regionCheck(start.line == 4 && start.wholeLine, "near the rule the text sits on: the block below it");
    start = insertLinesStart(3*yr + 0.3*yr, yr);
    regionCheck(start.line == 3 && !start.wholeLine, "past the snap band: a split again");
    start = insertLinesStart(-3, yr);
    regionCheck(start.line == 0 && start.wholeLine, "just above a region's top rule: its first line as a block");
    // exactly 1/8 either side still counts, just past it does not; and nothing about the lines matters
    start = insertLinesStart(3*yr + 0.125*yr, yr);
    regionCheck(start.line == 3 && start.wholeLine, "1/8 line below a rule: the whole line");
    start = insertLinesStart(3*yr - 0.125*yr, yr);
    regionCheck(start.line == 3 && start.wholeLine, "1/8 line above a rule: the whole line below it, not the line above");
    start = insertLinesStart(3*yr + 0.13*yr, yr);
    regionCheck(start.line == 3 && !start.wholeLine, "just past 1/8 below a rule: a split");
    start = insertLinesStart(3*yr - 0.13*yr, yr);
    regionCheck(start.line == 2 && !start.wholeLine, "just past 1/8 above a rule: a split of the line above");
    start = insertLinesStart(4.5*yr, yr);
    regionCheck(start.line == 4 && !start.wholeLine, "mid-line is a split, empty line or not (Skip Lines makes no difference)");
  }

  // A selection's relative width is measured in the line of the patch it lies in (pen-and-tools.md)
  {
    RulingRegionParams patch = RulingRegionParams::fromRect(Rect::ltrb(100, 0, 300, 200), 0, 20);
    int patchTag = 0;
    const Dim pageYr = 40;
    auto frameAt = [&](Point pos) {
      return patch.contains(pos) ? patch.frame(&patchTag) : RulingFrame(Point(0, 0), 0, 0, pageYr, 0, NULL);
    };
    regionCheck(selectionLineHeight({Point(150, 100), Point(200, 120)}, frameAt, 40) == 20,
        "strokes inside a patch: the patch's pitch, not the page's");
    regionCheck(selectionLineHeight({Point(10, 10), Point(20, 30)}, frameAt, 40) == 40,
        "strokes off the patch: the page's pitch");
    regionCheck(selectionLineHeight({Point(10, 10), Point(150, 100), Point(200, 120)}, frameAt, 40) == 20,
        "spanning the page and a patch: where most of the ink is");
    regionCheck(selectionLineHeight({Point(150, 100), Point(10, 10)}, frameAt, 40) == 20,
        "a tie goes to the earliest stroke (patch first)");
    regionCheck(selectionLineHeight({Point(10, 10), Point(150, 100)}, frameAt, 40) == 40,
        "a tie goes to the earliest stroke (page first)");
    regionCheck(selectionLineHeight({}, frameAt, 33) == 33, "nothing selected: the fallback");
    auto blank = [&](Point) { return RulingFrame(); };
    regionCheck(selectionLineHeight({Point(1, 1)}, blank, 33) == 33, "an unruled frame: the fallback");
  }

  {
    // music staves: yRuling is a band of STAFF_BAND_SPACES (9) spaces, a staff of 5 lines centred in it
    RulingRegionParams staffBox = RulingRegionParams::fromRect(Rect::ltrb(0, 0, 200, 720), 0, 72, 0, true);
    Path2D staves = staffBox.linesPath();
    regionCheck(segmentCount(staves) == 10*STAFF_LINES, "720 unit box at band 72 has 10 staves of 5 lines");
    regionCheck(staves.size() >= 2 && nearPt(staves.point(0), Point(0, 20)) && nearPt(staves.point(1), Point(200, 20)),
        "first staff line is 2.5 spaces (20) into the first band, full width");
    regionCheck(staves.size() >= 10 && nearPt(staves.point(8), Point(0, 52)),
        "fifth line of the first staff is 4 spaces (32) below the first");
    regionCheck(staves.size() >= 12 && nearPt(staves.point(10), Point(0, 92)),
        "the next staff starts a band (72) below the first");
    regionCheck(staffBox.frame().staff, "the frame carries the staff flag");
    RulingRegionParams lined = RulingRegionParams::fromRect(Rect::ltrb(0, 0, 200, 720), 0, 72);
    regionCheck(segmentCount(lined.linesPath()) == 9 && !lined.frame().staff, "without it the same box is plain lines");
    // a staff has no x ruling and no dots, whatever a file or peer says
    RulingRegionParams mixed = staffBox;
    mixed.xRuling = 30;
    mixed.dotRadius = 2;
    mixed.sanitize();
    regionCheck(mixed.xRuling == 0 && mixed.dotRadius == 0 && segmentCount(mixed.linesPath()) == 50,
        "sanitize drops a staff's x ruling and dots");
    // lines are phased from the region's own top-left, and a short box cuts the staff that does not fit:
    //  box 30..100 is 70 tall, so only band 0's five lines (local 20..52) fit, not band 1's (92..124)
    RulingRegionParams cut = RulingRegionParams::fromRect(Rect::ltrb(0, 30, 200, 100), 0, 72, 0, true);
    regionCheck(segmentCount(cut.linesPath()) == 5, "box 70 tall: one staff, phased from the box's top");
    RulingRegionParams cut2 = RulingRegionParams::fromRect(Rect::ltrb(0, 30, 200, 50), 0, 72, 0, true);
    regionCheck(segmentCount(cut2.linesPath()) == 0 && segmentCount(cut.linesPath()) == 5,
        "box 20 tall ends before the first line (20) crosses it: none");
    // a tilted staff region stays 5 parallel lines per band
    RulingRegionParams tilted = staffBox;
    tilted.angle = 0.3;
    regionCheck(segmentCount(tilted.linesPath()) >= 40, "a tilted staff region still draws its staves");
  }

  // spacing changes scale about the red handle (setPitchesAbout): the handle keeps its phase among the lines
  {
    // phase of a point in a frame, in bands: local / pitch
    auto phase = [](const RulingRegionParams& params, Point pt) {
      Point local = params.frame().toLocal(pt);
      return Point(params.xRuling > 0 ? local.x/params.xRuling : 0, local.y/params.yRuling);
    };
    RulingRegionParams squared = RulingRegionParams::fromRect(Rect::ltrb(0, 0, 300, 300), 20, 20);
    Point midBand(50, 130);  // 2.5 cells right, 6.5 bands down
    Point before = phase(squared, midBand);
    RulingRegionParams scaled = squared;
    scaled.setPitchesAbout(midBand, 30, 30);
    regionCheck(nearPt(phase(scaled, midBand), before), "a pitch change keeps the handle's phase (mid band)");
    regionCheck(scaled.xRuling == 30 && scaled.yRuling == 30 && scaled.corners == squared.corners,
        "setPitchesAbout sets the pitches and leaves the outline");
    // a handle on a line (the default one sits on the first line) stays on a line
    Point onLine(0, 40);
    RulingRegionParams finer = squared;
    finer.setPitchesAbout(onLine, 25, 25);
    Dim bands = finer.frame().toLocal(onLine).y/25;
    regionCheck(std::abs(bands - std::round(bands)) < 1E-9, "a handle on a line is still on a line at pitch 25");
    // tilted, and lined only (no x pitch: x is left alone)
    RulingRegionParams tiltedLined = RulingRegionParams::fromRect(Rect::ltrb(10, 10, 210, 210), 0, 24);
    tiltedLined.angle = 0.35;
    Point handle(70, 95);
    Point tiltedBefore = tiltedLined.frame().toLocal(handle);
    tiltedLined.setPitchesAbout(handle, 0, 36);
    Point tiltedAfter = tiltedLined.frame().toLocal(handle);
    regionCheck(std::abs(tiltedAfter.y/36 - tiltedBefore.y/24) < 1E-9 && std::abs(tiltedAfter.x - tiltedBefore.x) < 1E-9,
        "tilted: y phase kept, x (no x ruling) unchanged");
  }

  // coordinate system patch: grid cell 21 puts a tick (and number) every 42, x right, y up the page
  {
    RulingRegionParams plot = RulingRegionParams::fromRect(Rect::ltrb(-100, -100, 100, 100), 21, 21);
    plot.origin = Point(0, 0);
    regionCheck(plot.axesPath().empty() && plot.axisLabels().empty(), "no axes unless asked for");
    plot.axes = true;
    regionCheck(!plot.axesPath().empty(), "a coordinate system draws its axes");
    std::vector<AxisLabel> labels = plot.axisLabels();
    auto findLabel = [&](int value, bool yAxis) -> const AxisLabel* {
      for(const AxisLabel& label : labels) {
        if(label.value == value && label.alignEnd == yAxis && (value != 0 || yAxis))
          return &label;
      }
      return NULL;
    };
    const AxisLabel* xOne = findLabel(1, false);
    regionCheck(xOne && std::abs(xOne->local.x - 2*21) < 1E-9, "x = 1 is two grid cells (42) right of the origin");
    const AxisLabel* xMinusTwo = findLabel(-2, false);
    regionCheck(xMinusTwo && std::abs(xMinusTwo->local.x + 4*21) < 1E-9, "x = -2 is 84 left of the origin");
    const AxisLabel* yOne = findLabel(1, true);
    regionCheck(yOne && yOne->local.y < 0 && std::abs(yOne->local.y + 42) < plot.axisLabelFontSize(),
        "y = 1 is 42 *up* the page from the origin (math orientation)");
    regionCheck(!findLabel(3, false), "no tick at 126: past the edge (100)");
    int xCount = 0, yCount = 0, zeroCount = 0;
    for(const AxisLabel& label : labels)
      (label.value == 0 ? zeroCount : label.alignEnd ? yCount : xCount)++;
    regionCheck(xCount == 4 && yCount == 4 && zeroCount == 1, "ticks -2, -1, 1, 2 on each axis and a single 0");
    // the origin moved off-centre: numbers follow it, and those outside the outline are dropped
    RulingRegionParams shifted = plot;
    shifted.origin = Point(-80, 80);
    int positives = 0;
    for(const AxisLabel& label : shifted.axisLabels())
      positives += label.value > 0;
    regionCheck(positives == 6, "origin near the bottom-left corner: x 1..3 and y 1..3, nothing negative fits");
    // sanitize makes a coordinate system squared and dotless, and a staff is never one
    RulingRegionParams lined = RulingRegionParams::fromRect(Rect::ltrb(0, 0, 100, 100), 0, 30, 2);
    lined.axes = true;
    lined.sanitize();
    regionCheck(lined.xRuling == 30 && lined.dotRadius == 0, "sanitize: axes get a square grid and no dots");
  }

  // soft rectangle snap of a dragged corner (snapCornerToRect): relative to the neighbours, in the frame
  {
    const Dim tol = 5;
    // corners TL, TR, BR, BL of a 200 x 100 rectangle that is not at the origin
    std::vector<Point> rect = {Point(50, 40), Point(250, 40), Point(250, 140), Point(50, 140)};
    RulingFrame flat(Point(0, 0), 0, 0, 30);
    // drag BR: 3 off in x and 2 off in y -> back to the rectangle corner
    Point snapped = snapCornerToRect(flat, rect, 2, Point(253, 138), tol);
    regionCheck(nearPt(snapped, Point(250, 140)), "snap: a corner dragged near the rectangle corner returns to it");
    // each axis on its own
    snapped = snapCornerToRect(flat, rect, 2, Point(253, 160), tol);
    regionCheck(nearPt(snapped, Point(250, 160)), "snap: only the axis within tolerance snaps (x)");
    snapped = snapCornerToRect(flat, rect, 2, Point(280, 138), tol);
    regionCheck(nearPt(snapped, Point(280, 140)), "snap: only the axis within tolerance snaps (y)");
    // beyond the tolerance nothing moves
    snapped = snapCornerToRect(flat, rect, 2, Point(257, 147), tol);
    regionCheck(nearPt(snapped, Point(257, 147)), "snap: outside the tolerance the corner is left alone");
    // every corner, not just one: TL dragged
    snapped = snapCornerToRect(flat, rect, 0, Point(48, 43), tol);
    regionCheck(nearPt(snapped, Point(50, 40)), "snap: works for the first corner too");
    // relative, not absolute: the same drag with the whole patch moved snaps to the moved neighbours
    std::vector<Point> moved;
    for(const Point& corner : rect)
      moved.push_back(corner + Point(1000.5, -333.25));
    snapped = snapCornerToRect(flat, moved, 2, Point(253, 138) + Point(1000.5, -333.25), tol);
    regionCheck(nearPt(snapped, Point(250, 140) + Point(1000.5, -333.25)), "snap: relative to neighbours, not absolute x/y");
    // a tilted frame: the rectangle is axis-aligned in the frame, not on the page
    RulingFrame tilted(Point(10, 20), 0.4, 0, 30);
    std::vector<Point> tiltedRect;
    for(const Point& corner : rect)
      tiltedRect.push_back(tilted.toPage(corner));
    Point drag = tilted.toPage(Point(253, 138));
    snapped = snapCornerToRect(tilted, tiltedRect, 2, drag, tol);
    regionCheck(nearPt(snapped, tiltedRect[2], 1E-6), "snap: measured in the tilted frame");
    // one that is NOT a rectangle: the horizontal edge goes level and the vertical one plumb, whatever the
    //  opposite corner does
    std::vector<Point> trapezoid = {Point(50, 40), Point(250, 55), Point(250, 140), Point(50, 140)};
    snapped = snapCornerToRect(flat, trapezoid, 2, Point(252, 143), tol);
    regionCheck(nearPt(snapped, Point(250, 140)), "snap: the dragged corner lines up with its neighbours only");
    // not a quad: untouched
    std::vector<Point> triangle = {Point(0, 0), Point(10, 0), Point(10, 10)};
    regionCheck(nearPt(snapCornerToRect(flat, triangle, 2, Point(11, 11), tol), Point(11, 11)), "snap: not four corners, no snap");
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
