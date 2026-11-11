// Unit tests for the parametric shape math in syncscribble/shape.cpp (SHAPES_SPEC.md).
// Like scantest.cpp these need no GL context and no document, so they can also be built and run on their
//  own - which is how they get run in CI, where no display is available:
//
//   g++ -std=c++14 -O2 -DNDEBUG -I . -I syncscribble -isystem stb -DSHAPETEST_MAIN
//       scribbletest/shapetest.cpp syncscribble/shape.cpp syncscribble/shaperec.cpp ulib/geom.cpp
//       ulib/path2d.cpp -o shapetest && ./shapetest
//   (one command, run from the repo root.  NDEBUG is needed because geom.cpp's ASSERT would otherwise
//   pull in platform_assert, which lives in the application.)
//
// The hold-to-snap recognizer (shaperec.cpp) has its own large gated suite in labs/shape-recognition;
//  testShapeRecognition() here only pins the triangle and polyline cases and that line/rectangle/curve
//  results are unchanged by them.
//
// runShapeTests() returns the number of failed checks and is also called from ScribbleTest::runAll().

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shape.h"
#include "shaperec.h"

static int nShapeChecksFailed = 0;

static void shapeCheckTrue(bool condition, const char* what)
{
  if(!condition) {
    ++nShapeChecksFailed;
    printf("FAIL: %s\n", what);
  }
}

static void shapeCheckNear(Dim actual, Dim expected, Dim eps, const char* what)
{
  if(!(std::abs(actual - expected) <= eps)) {
    ++nShapeChecksFailed;
    printf("FAIL: %s (got %g, expected %g)\n", what, double(actual), double(expected));
  }
}

static void shapeCheckPointNear(Point actual, Point expected, Dim eps, const char* what)
{
  if(!(std::abs(actual.x - expected.x) <= eps && std::abs(actual.y - expected.y) <= eps)) {
    ++nShapeChecksFailed;
    printf("FAIL: %s (got %g,%g, expected %g,%g)\n", what,
        double(actual.x), double(actual.y), double(expected.x), double(expected.y));
  }
}

static ShapeParams makeShape(int id, Point p0, Point p1, int flags = 0)
{
  ShapeParams params;
  params.id = id;
  params.points = {p0, p1};
  params.flags = flags;
  return params;
}

// "arrow", "rbox" and "rpolyline" are flags now, not shapes
static ShapeParams makeRounded(int id, Point p0, Point p1, Dim radius)
{
  ShapeParams params = makeShape(id, p0, p1, SHAPEFLAG_ROUNDED);
  params.rx = radius;
  params.ry = radius;
  return params;
}

// every shape id must round-trip through its string id, or a saved document reloads as a plain path
static void testRegistry()
{
  for(int ii = 0; ii < SHAPE_COUNT; ++ii) {
    const ShapeDef* def = shapeDef(ii);
    shapeCheckTrue(def != NULL, "shapeDef must be defined for every id");
    if(!def)
      continue;
    shapeCheckTrue(shapeIdByStringId(def->id) == ii, "string id must round-trip to the same shape id");
    shapeCheckTrue(def->buildPath && def->getHandles && def->applyConstraint,
        "every ShapeDef needs all three function pointers");
  }
  shapeCheckTrue(shapeIdByStringId("nosuchshape") == SHAPE_NONE, "unknown string id gives SHAPE_NONE");
  shapeCheckTrue(shapeIdByStringId("") == SHAPE_NONE, "empty string id gives SHAPE_NONE");

  // documents written before arrow/rbox/rpolyline became flags must still load as what they meant
  int extra = -1;
  shapeCheckTrue(shapeIdByStringId("arrow", &extra) == SHAPE_LINE && extra == SHAPEFLAG_HEADEND,
      "the legacy \"arrow\" id is a line with an end head");
  shapeCheckTrue(shapeIdByStringId("rbox", &extra) == SHAPE_BOX && extra == SHAPEFLAG_ROUNDED,
      "the legacy \"rbox\" id is a rounded box");
  shapeCheckTrue(shapeIdByStringId("rpolyline", &extra) == SHAPE_POLYLINE && extra == SHAPEFLAG_ROUNDED,
      "the legacy \"rpolyline\" id is a rounded polyline");
  // "splinepoly" was the interpolating curve; it is the one curve shape at tightness 1
  Dim tight = -1;
  shapeCheckTrue(shapeIdByStringId("splinepoly", &extra, &tight) == SHAPE_CURVE && tight == 1,
      "the legacy \"splinepoly\" id is a curve at tightness 1");
  shapeIdByStringId("box", &extra, &tight);
  shapeCheckTrue(extra == 0 && tight < 0, "a current id implies no extra flags and no tightness");
  shapeCheckTrue(shapeDef(SHAPE_NONE) == NULL && shapeDef(SHAPE_COUNT) == NULL,
      "out-of-range shape ids give no definition");
}

static void testPointSerialization()
{
  std::vector<Point> points = {Point(1.5, -2.25), Point(300, 0), Point(-0.5, 17)};
  std::vector<Point> parsed;
  parseShapePoints(serializeShapePoints(points).c_str(), parsed);
  shapeCheckTrue(parsed.size() == points.size(), "point list round-trip keeps the point count");
  for(size_t ii = 0; ii < parsed.size() && ii < points.size(); ++ii)
    shapeCheckPointNear(parsed[ii], points[ii], 1e-3, "point list round-trip keeps the values");

  parseShapePoints("", parsed);
  shapeCheckTrue(parsed.empty(), "empty descriptor parses to no points");
  parseShapePoints("garbage", parsed);
  shapeCheckTrue(parsed.empty(), "unparseable descriptor parses to no points");
  // a truncated final pair must not produce a half-read point
  parseShapePoints("1,2 3", parsed);
  shapeCheckTrue(parsed.size() == 1, "a trailing unpaired coordinate is dropped");
}

static void testBoxGeometry()
{
  // corners may be given in any order; the rect is normalized
  ShapeParams box = makeShape(SHAPE_BOX, Point(80, 60), Point(20, 10));
  Rect r = box.rect();
  shapeCheckPointNear(Point(r.left, r.top), Point(20, 10), 1e-9, "rect() normalizes the corners");
  shapeCheckPointNear(Point(r.right, r.bottom), Point(80, 60), 1e-9, "rect() normalizes the corners");

  Path2D path = buildShapePath(box);
  shapeCheckTrue(!path.empty(), "a box with non-zero area builds a path");
  Rect bbox = path.getBBox();
  shapeCheckPointNear(Point(bbox.left, bbox.top), Point(20, 10), 1e-6, "box path matches the descriptor");
  shapeCheckPointNear(Point(bbox.right, bbox.bottom), Point(80, 60), 1e-6, "box path matches the descriptor");

  // a degenerate box builds nothing rather than a zero-area artefact
  shapeCheckTrue(buildShapePath(makeShape(SHAPE_BOX, Point(5, 5), Point(5, 40))).empty(),
      "a zero-width box builds no path");
}

// the radius must be clamped to min(w,h)/2 both when building the path and when dragging the handle,
//  so the handle can never sit somewhere the shape is not drawn (spec 5)
static void testRoundedBoxRadius()
{
  ShapeParams rbox = makeRounded(SHAPE_BOX, Point(0, 0), Point(100, 40), 500);
  Rect bbox = buildShapePath(rbox).getBBox();
  shapeCheckPointNear(Point(bbox.left, bbox.top), Point(0, 0), 1e-6, "an over-large radius is clamped");
  shapeCheckPointNear(Point(bbox.right, bbox.bottom), Point(100, 40), 1e-6,
      "an over-large radius is clamped");

  // a radius of zero is not rounded, so it gets no radius handle - there is never a handle for a
  //  parameter that is not currently affecting the shape
  rbox.rx = 0;
  rbox.ry = 0;
  std::vector<ShapeHandle> handles;
  getShapeHandles(rbox, handles);
  shapeCheckTrue(handles.size() == 4, "a zero-radius box has no radius handle");
  handles.clear();
  getShapeHandles(makeShape(SHAPE_BOX, Point(0, 0), Point(100, 40)), handles);
  shapeCheckTrue(handles.size() == 4, "a box with rounding switched off has no radius handle");

  rbox.rx = 5;
  rbox.ry = 5;
  handles.clear();
  getShapeHandles(rbox, handles);
  shapeCheckTrue(handles.size() == 5, "a rounded box has four corner handles plus the radius handle");
  const ShapeHandle* radiusHandle = NULL;
  for(const ShapeHandle& handle : handles) {
    if(handle.type == ShapeHandle::RADIUS)
      radiusHandle = &handle;
  }
  shapeCheckTrue(radiusHandle != NULL, "a rounded box has a radius handle");
  if(radiusHandle) {
    dragShapeHandle(rbox, *radiusHandle, Point(1000, 1000));
    shapeCheckNear(rbox.rx, 20, 1e-9, "dragging the radius handle clamps to min(w,h)/2");
    shapeCheckNear(rbox.ry, 20, 1e-9, "dragging the radius handle clamps to min(w,h)/2");
    dragShapeHandle(rbox, *radiusHandle, Point(-50, -50));
    shapeCheckNear(rbox.rx, 0, 1e-9, "dragging the radius handle back past the corner clamps to zero");
  }

  // box and rbox share one implementation; a zero radius must give exactly the box path
  ShapeParams box = makeShape(SHAPE_BOX, Point(0, 0), Point(100, 40));
  ShapeParams flat = makeRounded(SHAPE_BOX, Point(0, 0), Point(100, 40), 0);
  shapeCheckTrue(buildShapePath(box).size() == buildShapePath(flat).size(),
      "a zero-radius rounded box is the same path as a box");
  // the radius survives switching rounding off, so toggling back on returns the radius you had
  ShapeParams toggledOff = makeRounded(SHAPE_BOX, Point(0, 0), Point(100, 40), 12);
  toggledOff.flags &= ~SHAPEFLAG_ROUNDED;
  shapeCheckTrue(buildShapePath(toggledOff).size() == buildShapePath(box).size(),
      "rounding switched off draws square corners");
  shapeCheckNear(toggledOff.rx, 12, 1e-9, "rounding switched off keeps the radius for next time");
  toggledOff.flags |= SHAPEFLAG_ROUNDED;
  shapeCheckTrue(buildShapePath(toggledOff).size() > buildShapePath(box).size(),
      "rounding switched back on uses the radius it kept");
}

static void testBoxCornerHandles()
{
  ShapeParams box = makeShape(SHAPE_BOX, Point(10, 20), Point(110, 220));
  std::vector<ShapeHandle> handles;
  getShapeHandles(box, handles);
  shapeCheckTrue(handles.size() == 4, "a box has four corner handles");
  if(handles.size() != 4)
    return;
  // dragging the top-left corner must leave the bottom-right one where it was
  dragShapeHandle(box, handles[0], Point(0, 0));
  Rect r = box.rect();
  shapeCheckPointNear(Point(r.left, r.top), Point(0, 0), 1e-9, "dragged corner follows the cursor");
  shapeCheckPointNear(Point(r.right, r.bottom), Point(110, 220), 1e-9, "opposite corner is the anchor");

  handles.clear();
  getShapeHandles(box, handles);
  dragShapeHandle(box, handles[1], Point(200, 5));  // top-right
  r = box.rect();
  shapeCheckPointNear(Point(r.left, r.bottom), Point(0, 220), 1e-9, "opposite corner is the anchor");
  shapeCheckPointNear(Point(r.right, r.top), Point(200, 5), 1e-9, "dragged corner follows the cursor");
}

// the head scales with the stroke width, never with the shaft length (spec 2)
static void testArrowHead()
{
  ShapeParams shortArrow = makeShape(SHAPE_LINE, Point(0, 0), Point(100, 0), SHAPEFLAG_HEADEND);
  shortArrow.strokeWidth = 2;
  ShapeParams longArrow = shortArrow;
  longArrow.points[1] = Point(1000, 0);

  Rect shortBBox = buildShapePath(shortArrow).getBBox();
  Rect longBBox = buildShapePath(longArrow).getBBox();
  shapeCheckNear(longBBox.height(), shortBBox.height(), 1e-6,
      "arrowhead height must not grow with shaft length");

  ShapeParams thick = shortArrow;
  thick.strokeWidth = 8;
  shapeCheckTrue(buildShapePath(thick).getBBox().height() > shortBBox.height() + 1,
      "arrowhead must grow with stroke width");

  // a head must never swallow a very short shaft
  ShapeParams tiny = makeShape(SHAPE_LINE, Point(0, 0), Point(4, 0), SHAPEFLAG_HEADEND);
  tiny.strokeWidth = 20;
  Rect tinyBBox = buildShapePath(tiny).getBBox();
  shapeCheckTrue(tinyBBox.width() <= 4 + 1e-6, "arrowhead is capped at a fraction of the shaft");

  // no head flag, no head
  ShapeParams plain = makeShape(SHAPE_LINE, Point(0, 0), Point(100, 0));
  shapeCheckTrue(buildShapePath(plain).size() < buildShapePath(shortArrow).size(),
      "a line with no head flag is shorter than the same line with one");

  ShapeParams doubleHead = shortArrow;
  doubleHead.flags = SHAPEFLAG_HEADEND | SHAPEFLAG_HEADSTART;
  shapeCheckTrue(buildShapePath(doubleHead).size() > buildShapePath(shortArrow).size(),
      "headStart adds a second arrowhead");
}

// a fitted curve's head follows the end tangent of the curve as drawn, at any tightness (the head used to
//  stay along the original last segment, so it sat skew once the curve was relaxed)
static void testCurveArrowHead()
{
  for(Dim tightness : {Dim(0), Dim(0.4), Dim(1)}) {
    ShapeParams curve;
    curve.id = SHAPE_CURVE;
    curve.flags = SHAPEFLAG_HEADEND;
    curve.strokeWidth = 2;
    curve.tightness = tightness;
    curve.points = {Point(0, 0), Point(60, 80), Point(120, 0), Point(180, 90), Point(200, 20)};
    Path2D path = buildShapePath(curve);
    int count = int(path.size());
    // the path ends with the head's three points, before them the last cubic: ... c1, c2, end
    Point tip = path.point(count - 2);
    Point leftTip = path.point(count - 3), rightTip = path.point(count - 1);
    Point lastControl = path.point(count - 5);
    Point headAxis = tip - (leftTip + rightTip)/2;
    Point tangent = tip - lastControl;
    Dim cross = headAxis.x*tangent.y - headAxis.y*tangent.x;
    Dim sinAngle = cross/(headAxis.dist()*tangent.dist());
    shapeCheckNear(sinAngle, 0, 1e-6, "the curve's arrowhead is along its end tangent");
  }
}

static void testPolyline()
{
  ShapeParams poly;
  poly.id = SHAPE_POLYLINE;
  poly.points = {Point(0, 0), Point(50, 0), Point(50, 50)};
  std::vector<ShapeHandle> handles;
  getShapeHandles(poly, handles);
  shapeCheckTrue(handles.size() == 3, "a polyline has one handle per point");

  Path2D open = buildShapePath(poly);
  poly.flags = SHAPEFLAG_CLOSED;
  shapeCheckTrue(buildShapePath(poly).size() >= open.size(), "closing a polyline does not lose points");

  // heads are only allowed on open ends
  ShapeParams closedWithHead = poly;
  closedWithHead.flags = SHAPEFLAG_CLOSED | SHAPEFLAG_HEADEND;
  closedWithHead.strokeWidth = 2;
  shapeCheckTrue(buildShapePath(closedWithHead).size() == buildShapePath(poly).size(),
      "a closed polyline gets no arrowhead");

  // dragging a vertex moves only that vertex
  handles.clear();
  getShapeHandles(poly, handles);
  dragShapeHandle(poly, handles[1], Point(77, -3));
  shapeCheckPointNear(poly.points[1], Point(77, -3), 1e-9, "dragging a vertex moves that vertex");
  shapeCheckPointNear(poly.points[0], Point(0, 0), 1e-9, "dragging a vertex leaves the others alone");
  shapeCheckPointNear(poly.points[2], Point(50, 50), 1e-9, "dragging a vertex leaves the others alone");
}

// the polyline family (sharp, filleted, smooth, fitted) shares points, handles and heads, so the checks
//  below are mostly about what each flavour does differently between the points

static bool pathHasPoint(const Path2D& path, Point p, Dim eps)
{
  for(int ii = 0; ii < path.size(); ++ii) {
    if(std::abs(path.point(ii).x - p.x) <= eps && std::abs(path.point(ii).y - p.y) <= eps)
      return true;
  }
  return false;
}

static ShapeParams makePoly(int id)
{
  ShapeParams params;
  params.id = id;
  params.points = {Point(0, 0), Point(100, 0), Point(100, 100), Point(0, 100)};
  return params;
}

static void testFilletPolyline()
{
  ShapeParams sharp = makePoly(SHAPE_POLYLINE);
  ShapeParams fillet = makePoly(SHAPE_POLYLINE);
  fillet.flags |= SHAPEFLAG_ROUNDED;

  // zero radius must be indistinguishable from the sharp polyline, as a rounded box is from a box
  shapeCheckTrue(buildShapePath(fillet).size() == buildShapePath(sharp).size(),
      "a zero-radius rounded polyline is the same path as a polyline");

  fillet.rx = 20;
  fillet.ry = 20;
  Path2D rounded = buildShapePath(fillet);
  shapeCheckTrue(rounded.size() > buildShapePath(sharp).size(),
      "rounding the corners adds curve segments");
  // the corners are cut, never bulged: a fillet stays inside the sharp polyline's bbox
  Rect sharpBBox = buildShapePath(sharp).getBBox();
  Rect roundBBox = rounded.getBBox();
  shapeCheckTrue(roundBBox.left >= sharpBBox.left - 1e-6 && roundBBox.top >= sharpBBox.top - 1e-6
      && roundBBox.right <= sharpBBox.right + 1e-6 && roundBBox.bottom <= sharpBBox.bottom + 1e-6,
      "a fillet must stay inside the sharp polyline");
  // an interior vertex is cut away; the end points are not, since they have no corner
  shapeCheckTrue(!pathHasPoint(rounded, Point(100, 0), 1e-6), "the interior corner is rounded away");
  shapeCheckTrue(pathHasPoint(rounded, Point(0, 0), 1e-6), "the first point is not a corner");

  // a radius far larger than the segments must clamp per corner rather than fold the path over
  ShapeParams huge = makePoly(SHAPE_POLYLINE);
  huge.flags |= SHAPEFLAG_ROUNDED;
  huge.rx = 10000;
  huge.ry = 10000;
  Rect hugeBBox = buildShapePath(huge).getBBox();
  shapeCheckTrue(hugeBBox.left >= sharpBBox.left - 1e-6 && hugeBBox.top >= sharpBBox.top - 1e-6
      && hugeBBox.right <= sharpBBox.right + 1e-6 && hugeBBox.bottom <= sharpBBox.bottom + 1e-6,
      "an over-large fillet radius is clamped per corner");

  // the radius handle sits on the incoming edge of the first interior vertex and drags the radius
  std::vector<ShapeHandle> handles;
  getShapeHandles(fillet, handles);
  shapeCheckTrue(handles.size() == fillet.points.size() + 1,
      "a rounded polyline has one handle per point plus the radius handle");
  const ShapeHandle* radiusHandle = NULL;
  for(const ShapeHandle& handle : handles) {
    if(handle.type == ShapeHandle::RADIUS)
      radiusHandle = &handle;
  }
  shapeCheckTrue(radiusHandle != NULL, "a rounded polyline has a radius handle");
  if(radiusHandle) {
    shapeCheckPointNear(radiusHandle->pos, Point(80, 0), 1e-6,
        "the radius handle sits on the incoming edge of the first interior corner");
    dragShapeHandle(fillet, *radiusHandle, Point(70, 0));
    shapeCheckNear(fillet.rx, 30, 1e-6, "dragging the radius handle sets the radius");
  }

  // a polyline with no interior vertex has no corner and so no radius handle
  ShapeParams twoPoint = fillet;
  twoPoint.points.resize(2);
  handles.clear();
  getShapeHandles(twoPoint, handles);
  shapeCheckTrue(handles.size() == 2, "a two-point rounded polyline has no radius handle");
}

static void testCurvePolylines()
{
  // There is one curve shape, not two: tightness 1 is the interpolating curve (through every point)
  // and 0 is the approximating one.  These were separate tools until it was measured that tightness 1
  // is *bit-identical* to interpolating Catmull-Rom, at which point the second tool was only a preset.
  ShapeParams spline = makePoly(SHAPE_CURVE);
  spline.tightness = 1;
  Path2D splinePath = buildShapePath(spline);
  for(const Point& p : spline.points) {
    shapeCheckTrue(pathHasPoint(splinePath, p, 1e-6),
        "at tightness 1 the curve must pass through every point");
  }

  ShapeParams fit = makePoly(SHAPE_CURVE);
  Path2D fitPath = buildShapePath(fit);

  // tightness slides the fitted curve between the two extremes: 0 is the roundest, 1 reaches the
  //  points (becoming the smooth curve), and raising it must move monotonically towards them
  Dim prevDist = -1;
  for(Dim tight = 0; tight <= 1.0001; tight += 0.25) {
    ShapeParams tuned = fit;
    tuned.tightness = tight;
    Path2D tunedPath = buildShapePath(tuned);
    // how far the curve stays from an interior point it is meant to approximate
    Dim best = MAX_DIM;
    for(int ii = 0; ii < tunedPath.size(); ++ii)
      best = std::min(best, (tunedPath.point(ii) - fit.points[1]).dist());
    if(prevDist >= 0)
      shapeCheckTrue(best <= prevDist + 1e-6, "raising tightness must not move the curve further away");
    prevDist = best;
  }
  shapeCheckNear(prevDist, 0, 1e-6, "tightness 1 makes the fitted curve reach its points");

  ShapeParams loose = fit;
  loose.tightness = 0;
  ShapeParams tightened = fit;
  tightened.tightness = 1;
  shapeCheckTrue(buildShapePath(loose).getBBox().width()
      < buildShapePath(tightened).getBBox().width(),
      "a tighter fitted curve covers more of the points' extent");
  // out-of-range tightness must clamp rather than fly apart
  ShapeParams silly = fit;
  silly.tightness = 5;
  shapeCheckTrue(approxEq(buildShapePath(silly).getBBox(),
      buildShapePath(tightened).getBBox(), 1e-6), "tightness above 1 clamps");
  silly.tightness = -3;
  shapeCheckTrue(approxEq(buildShapePath(silly).getBBox(),
      buildShapePath(loose).getBBox(), 1e-6), "tightness below 0 clamps");

  // The tightness handle is what makes one tool enough, so it has to actually reach both ends.  It
  // slides along knot -> point: dropping it on the point gives 1, on the knot gives 0.
  std::vector<ShapeHandle> handles;
  getShapeHandles(fit, handles);
  const ShapeHandle* tightHandle = NULL;
  for(const ShapeHandle& handle : handles) {
    if(handle.type == ShapeHandle::TIGHTNESS)
      tightHandle = &handle;
  }
  shapeCheckTrue(tightHandle != NULL, "a curve has a tightness handle");
  shapeCheckTrue(handles.size() == fit.points.size() + 1,
      "a curve has one handle per point plus the tightness handle");
  if(tightHandle) {
    int idx = tightHandle->index;
    ShapeParams dragged = fit;
    dragShapeHandle(dragged, *tightHandle, fit.points[idx]);
    shapeCheckNear(dragged.tightness, 1, 1e-6, "dropping the handle on the point gives tightness 1");
    Path2D through = buildShapePath(dragged);
    for(const Point& p : dragged.points)
      shapeCheckTrue(pathHasPoint(through, p, 1e-6), "and the curve then passes through every point");
    // far past the point clamps rather than overshooting
    dragShapeHandle(dragged, *tightHandle, fit.points[idx] + 10*(fit.points[idx] - Point(0, 0)));
    shapeCheckTrue(dragged.tightness <= 1 + 1e-9, "dragging past the point clamps at 1");

    ShapeParams slack = fit;
    slack.tightness = 1;
    std::vector<ShapeHandle> h2;
    getShapeHandles(slack, h2);
    for(const ShapeHandle& handle : h2) {
      if(handle.type != ShapeHandle::TIGHTNESS)
        continue;
      // at tightness 1 the handle sits on its point; dragging it back to the knot must relax fully
      ShapeParams relaxed = slack;
      dragShapeHandle(relaxed, handle, buildShapePath(fit).point(0));  // somewhere well off the segment
      shapeCheckTrue(relaxed.tightness >= 0 && relaxed.tightness <= 1,
          "the tightness handle always yields a value in range");
    }
  }

  // a curve with no slack anywhere (collinear points) has no meaningful tightness handle to offer
  ShapeParams straight;
  straight.id = SHAPE_CURVE;
  straight.points = {Point(0, 0), Point(50, 0), Point(100, 0)};
  handles.clear();
  getShapeHandles(straight, handles);
  shapeCheckTrue(handles.size() == straight.points.size(),
      "a straight curve offers no tightness handle");
  // the defining property of the fitted curve is the opposite: interior points are approximated
  shapeCheckTrue(!pathHasPoint(fitPath, fit.points[1], 1e-6),
      "the fitted curve must not pass through its interior points");
  shapeCheckTrue(!pathHasPoint(fitPath, fit.points[2], 1e-6),
      "the fitted curve must not pass through its interior points");
  // ...but the ends stay pinned, or an arrowhead would float away from the curve
  shapeCheckPointNear(fitPath.point(0), fit.points[0], 1e-6, "the fitted curve starts on the first point");
  shapeCheckPointNear(fitPath.point(fitPath.size() - 1), fit.points.back(), 1e-6,
      "the fitted curve ends on the last point");

  Rect hull = buildShapePath(makePoly(SHAPE_POLYLINE)).getBBox();
  // the fitted curve is an approximating B-spline, so it stays strictly inside the hull of its points
  Rect fitBBox = fitPath.getBBox();
  shapeCheckTrue(fitBBox.left >= hull.left - 1e-6 && fitBBox.right <= hull.right + 1e-6
      && fitBBox.top >= hull.top - 1e-6 && fitBBox.bottom <= hull.bottom + 1e-6,
      "the fitted curve must stay inside the hull of its points");

  // An interpolating curve has to bulge to get smoothly through a right angle, so the smooth curve does
  //  overshoot at these corners and that is not a defect - it just has to stay proportionate.
  Rect splineBBox = splinePath.getBBox();
  shapeCheckTrue(splineBBox.left >= hull.left - 0.25*hull.width()
      && splineBBox.right <= hull.right + 0.25*hull.width()
      && splineBBox.top >= hull.top - 0.25*hull.height()
      && splineBBox.bottom <= hull.bottom + 0.25*hull.height(),
      "the smooth curve's corner overshoot must stay proportionate");

  // This is the check that pins centripetal parameterisation.  On evenly spaced points centripetal and
  //  uniform Catmull-Rom are the same curve, so a regression to uniform only shows up on *uneven*
  //  spacing: with a 4-unit segment between neighbours of 100 and 113, uniform overshoots the points by
  //  10 units where centripetal manages 0.6.
  ShapeParams uneven;
  uneven.id = SHAPE_CURVE;
  uneven.points = {Point(0, 0), Point(100, 0), Point(104, 0), Point(200, 60)};
  Rect unevenHull;
  for(const Point& p : uneven.points)
    unevenHull.rectUnion(p);
  Rect unevenBBox = buildShapePath(uneven).getBBox();
  Dim overshoot = std::max(std::max(unevenHull.left - unevenBBox.left, unevenHull.top - unevenBBox.top),
      std::max(unevenBBox.right - unevenHull.right, unevenBBox.bottom - unevenHull.bottom));
  shapeCheckTrue(overshoot < 0.02*unevenHull.width(),
      "the smooth curve must not overshoot on unevenly spaced points (uniform Catmull-Rom does)");

  // the end tangent must lie along the end segment, or an arrowhead would sit skew to its own curve
  ShapeParams straightEnd = spline;
  straightEnd.points = {Point(0, 0), Point(100, 0), Point(140, 80), Point(40, 140)};
  Path2D tangentPath = buildShapePath(straightEnd);
  shapeCheckNear(tangentPath.point(1).y, 0, 1e-6,
      "the smooth curve must leave its first point along the first segment");
}

// every polyline flavour takes heads, and takes them from the same tangent - the first/last segment
static void testPolylineFamilyHeads()
{
  const int ids[] = {SHAPE_POLYLINE, SHAPE_CURVE};
  for(int id : ids) {
    shapeCheckTrue(shapeIsPolylineFamily(id), "every polyline flavour is in the polyline family");
    shapeCheckTrue(shapeDef(id)->allowsHeads, "every polyline flavour allows heads");

    ShapeParams plain = makePoly(id);
    plain.strokeWidth = 2;
    ShapeParams headEnd = plain;
    headEnd.flags = SHAPEFLAG_HEADEND;
    ShapeParams bothHeads = plain;
    bothHeads.flags = SHAPEFLAG_HEADEND | SHAPEFLAG_HEADSTART;

    size_t plainSize = buildShapePath(plain).size();
    size_t endSize = buildShapePath(headEnd).size();
    size_t bothSize = buildShapePath(bothHeads).size();
    shapeCheckTrue(endSize > plainSize, "an end head adds geometry to every polyline flavour");
    shapeCheckTrue(bothSize > endSize, "a start head adds geometry to every polyline flavour");

    // a closed shape has no ends, so the head flags must be ignored rather than drawn somewhere odd
    ShapeParams closed = plain;
    closed.flags = SHAPEFLAG_CLOSED;
    ShapeParams closedWithHeads = plain;
    closedWithHeads.flags = SHAPEFLAG_CLOSED | SHAPEFLAG_HEADEND | SHAPEFLAG_HEADSTART;
    shapeCheckTrue(buildShapePath(closedWithHeads).size() == buildShapePath(closed).size(),
        "a closed polyline flavour gets no arrowhead");
    // and closing must still produce a path
    shapeCheckTrue(!buildShapePath(closed).empty(), "a closed polyline flavour still builds a path");
  }

  // box-like shapes have no ends at all, so the head toggles must be inert for them
  for(int id : {SHAPE_BOX, SHAPE_ELLIPSE}) {
    shapeCheckTrue(!shapeDef(id)->allowsHeads, "a bbox shape must not allow heads");
    shapeCheckTrue(!shapeIsPolylineFamily(id), "a bbox shape is not in the polyline family");
  }
}

static void testConstraints()
{
  // a vector shape snaps to the nearest 45 degrees, keeping its length
  ShapeParams line = makeShape(SHAPE_LINE, Point(0, 0), Point(100, 10));
  Dim len = (line.points[1] - line.points[0]).dist();
  applyShapeConstraint(line);
  shapeCheckNear((line.points[1] - line.points[0]).dist(), len, 1e-9,
      "the 45 degree constraint keeps the vector length");
  shapeCheckNear(line.points[1].y, 0, 1e-9, "a near-horizontal vector snaps to horizontal");

  ShapeParams diagonal = makeShape(SHAPE_LINE, Point(0, 0), Point(100, 95));
  applyShapeConstraint(diagonal);
  shapeCheckNear(diagonal.points[1].x, diagonal.points[1].y, 1e-6, "a near-diagonal vector snaps to 45");

  // a bbox shape becomes square, growing to the larger side and keeping the drag direction
  ShapeParams box = makeShape(SHAPE_BOX, Point(10, 10), Point(-90, 40));
  applyShapeConstraint(box);
  Rect r = box.rect();
  shapeCheckNear(r.width(), r.height(), 1e-9, "the constraint makes a box square");
  shapeCheckNear(r.width(), 100, 1e-9, "the square uses the larger of the two sides");
  shapeCheckTrue(box.points[1].x < box.points[0].x && box.points[1].y > box.points[0].y,
      "the constraint keeps the drag direction");
}

static void testAngleSnap()
{
  const Dim tolerance = 8*M_PI/180;
  // within tolerance: onto the direction, keeping the distance along it
  ShapeParams line = makeShape(SHAPE_LINE, Point(0, 0), Point(100, 10));
  Point snapped = snapShapeAngle(line, 1, line.points[1], tolerance);
  shapeCheckPointNear(snapped, Point(100, 0), 1e-9, "a line 5.7 degrees off horizontal snaps to it");
  snapped = snapShapeAngle(line, 0, Point(40, 65), tolerance);
  shapeCheckNear(snapped.x - line.points[1].x, -(snapped.y - line.points[1].y), 1e-9,
      "the start point snaps to the diagonal about the end point");
  // outside tolerance: untouched
  snapped = snapShapeAngle(line, 1, Point(100, 30), tolerance);
  shapeCheckPointNear(snapped, Point(100, 30), 0, "a line 16.7 degrees off horizontal is left alone");
  shapeCheckPointNear(snapShapeAngle(line, 1, Point(100, 10), 0), Point(100, 10), 0,
      "zero tolerance turns snapping off");
  // other shapes are not lines
  ShapeParams box = makeShape(SHAPE_BOX, Point(0, 0), Point(100, 10));
  shapeCheckPointNear(snapShapeAngle(box, 1, Point(100, 10), tolerance), Point(100, 10), 0,
      "a box point is not angle snapped");

  // an interior polyline point near both a horizontal and a vertical locks onto the right angle
  ShapeParams poly = makeShape(SHAPE_POLYLINE, Point(0, 0), Point(97, 6));
  poly.points.push_back(Point(100, 100));
  snapped = snapShapeAngle(poly, 1, poly.points[1], tolerance);
  shapeCheckPointNear(snapped, Point(100, 0), 1e-9, "an interior point near a corner snaps to a right angle");
  // only one neighbour in range: snaps to that one alone
  snapped = snapShapeAngle(poly, 1, Point(60, 5), tolerance);
  shapeCheckPointNear(snapped, Point(60, 0), 1e-9, "an interior point near one direction snaps to it");
  // a closed polyline wraps: the first point's neighbours are the second and the last
  poly.flags |= SHAPEFLAG_CLOSED;
  //  - horizontal from (97, 6) and diagonal from (100, 100) cross at (6, 6)
  snapped = snapShapeAngle(poly, 0, Point(4, 3), tolerance);
  shapeCheckPointNear(snapped, Point(6, 6), 1e-9, "a closed polyline's first point snaps against the last too");

  // editing snaps by distance from the axis line, capped by the angle: the same 3 degrees off horizontal
  //  snaps on a short line but not on a long one, whose end is far from the horizontal
  const Dim maxDist = 8;
  Dim offAngle = 3*M_PI/180;
  ShapeParams shortLine = makeShape(SHAPE_LINE, Point(0, 0), Point(100*std::cos(offAngle), 100*std::sin(offAngle)));
  snapped = snapShapeAngle(shortLine, 1, shortLine.points[1], tolerance, maxDist);
  shapeCheckNear(snapped.y, 0, 1e-9, "a 100 long line 3 degrees off (5.2 from the axis) snaps by distance");
  ShapeParams longLine = makeShape(SHAPE_LINE, Point(0, 0), Point(1000*std::cos(offAngle), 1000*std::sin(offAngle)));
  snapped = snapShapeAngle(longLine, 1, longLine.points[1], tolerance, maxDist);
  shapeCheckPointNear(snapped, longLine.points[1], 0,
      "a 1000 long line 3 degrees off (52 from the axis) stays slightly diagonal");
  // the angle still caps it: a short line well off the axis but close to it in distance is left alone
  snapped = snapShapeAngle(line, 1, Point(20, 7), tolerance, maxDist);
  shapeCheckPointNear(snapped, Point(20, 7), 0, "a short line 19 degrees off is not snapped by distance alone");
  // a long line whose end is within the distance does snap
  snapped = snapShapeAngle(longLine, 1, Point(1000, 6), tolerance, maxDist);
  shapeCheckPointNear(snapped, Point(1000, 0), 1e-9, "a long line with its end 6 from the axis snaps");
}

static void testEllipse()
{
  ShapeParams ellipse = makeShape(SHAPE_ELLIPSE, Point(0, 0), Point(100, 50));
  Rect bbox = buildShapePath(ellipse).getBBox();
  shapeCheckPointNear(Point(bbox.left, bbox.top), Point(0, 0), 1e-6, "ellipse fills its bbox");
  shapeCheckPointNear(Point(bbox.right, bbox.bottom), Point(100, 50), 1e-6, "ellipse fills its bbox");
  shapeCheckTrue(buildShapePath(makeShape(SHAPE_ELLIPSE, Point(0, 0), Point(0, 50))).empty(),
      "a zero-width ellipse builds no path");
}

// an unusable descriptor must never crash a builder - a truncated point list is what a hand-edited or
//  partially-written file looks like
static void testDegenerateDescriptors()
{
  for(int ii = 0; ii < SHAPE_COUNT; ++ii) {
    ShapeParams params;
    params.id = ii;
    buildShapePath(params);            // no points at all
    params.points = {Point(3, 4)};     // one point
    buildShapePath(params);
    std::vector<ShapeHandle> handles;
    getShapeHandles(params, handles);
    applyShapeConstraint(params);
  }
  ShapeParams invalid;
  shapeCheckTrue(!invalid.isValid(), "a default-constructed descriptor is not a shape");
  shapeCheckTrue(buildShapePath(invalid).empty(), "an invalid descriptor builds no path");
}

// A hand-drawn stroke through `vertices` in screen units: samples about 1.5 apart with a little
//  deterministic jitter, then the cluster of samples a hold leaves at the end.  `cornerCut` > 0 rounds
//  each interior corner by starting the turn that far before it.
static std::vector<shaperec::Vec2> heldStroke(std::vector<shaperec::Vec2> vertices, double cornerCut = 0)
{
  using shaperec::Vec2;
  if(cornerCut > 0 && vertices.size() > 2) {
    std::vector<Vec2> rounded = {vertices.front()};
    for(size_t i = 1; i + 1 < vertices.size(); ++i) {
      Vec2 before = vertices[i] + (vertices[i-1] - vertices[i]).normalized()*cornerCut;
      Vec2 after = vertices[i] + (vertices[i+1] - vertices[i]).normalized()*cornerCut;
      rounded.push_back(before);
      rounded.push_back((before + after)*0.25 + vertices[i]*0.5);
      rounded.push_back(after);
    }
    rounded.push_back(vertices.back());
    vertices.swap(rounded);
  }
  std::vector<Vec2> stroke;
  int sampleIdx = 0;
  for(size_t seg = 0; seg + 1 < vertices.size(); ++seg) {
    Vec2 from = vertices[seg], to = vertices[seg+1];
    int steps = std::max(1, int(shaperec::dist(from, to)/1.5));
    for(int step = 0; step < steps; ++step, ++sampleIdx) {
      // a hand wobbles slowly; sample noise is a fraction of the sample spacing
      Vec2 jitter(0.6*std::sin(sampleIdx*0.11) + 0.08*std::sin(sampleIdx*1.7),
          0.6*std::cos(sampleIdx*0.07 + 1) + 0.08*std::sin(sampleIdx*2.3 + 1));
      stroke.push_back(from + (to - from)*(double(step)/steps) + jitter);
    }
  }
  for(int hold = 0; hold < 12; ++hold)
    stroke.push_back(vertices.back() + shaperec::Vec2(0.3*std::sin(hold*2.1), 0.3*std::cos(hold*1.3)));
  return stroke;
}

static std::vector<shaperec::Vec2> sampledCurve(double (*curveY)(double), double fromX, double toX)
{
  std::vector<shaperec::Vec2> vertices;
  for(double posX = fromX; posX <= toX; posX += 2)
    vertices.emplace_back(posX, curveY(posX));
  return heldStroke(vertices);
}

static double sineWaveY(double posX) { return 30*std::sin(2*M_PI*posX/150); }
// cursive "uuuu": arches meeting in cusps - corners, but nothing straight between them
static double cursiveUY(double posX) { return 25*std::abs(std::sin(M_PI*posX/40)); }
// a shallow arc, about 90 degrees of a circle of radius 85
static double arcY(double posX) { return -std::sqrt(85*85 - posX*posX); }
// a handwritten "e"-like loop opening to the right
static std::vector<shaperec::Vec2> cursiveLoop()
{
  std::vector<shaperec::Vec2> vertices;
  for(double theta = 0; theta <= 1.7*M_PI; theta += 0.08)
    vertices.emplace_back(40 + 40*std::cos(theta + M_PI/2) + 25*theta, 40 - 40*std::sin(theta + M_PI/2));
  return heldStroke(vertices);
}

static bool recognizedAs(const shaperec::Result& result, const char* kind)
{
  if(getenv("SHAPETEST_VERBOSE"))
    printf("  recognized %s (wanted %s): %s\n", shaperec::kindName(result.kind), kind, result.reason.c_str());
  return strcmp(shaperec::kindName(result.kind), kind) == 0;
}

static void testShapeRecognition()
{
  using shaperec::Vec2;
  using shaperec::recognizeHold;

  // square root sign: short stroke down, long stroke up, long horizontal
  std::vector<Vec2> sqrtVerts = {Vec2(0, 55), Vec2(12, 80), Vec2(35, 0), Vec2(180, 0)};
  shaperec::Result sqrtRes = recognizeHold(heldStroke(sqrtVerts));
  shapeCheckTrue(recognizedAs(sqrtRes, "polyline"), "square root sign snaps to a polyline");
  if(sqrtRes.points.size() == 4) {
    for(int i = 0; i < 4; ++i)
      shapeCheckTrue(shaperec::dist(sqrtRes.points[i], sqrtVerts[i]) < 4, "square root polyline vertex where drawn");
  }
  else
    shapeCheckTrue(false, "square root polyline has 4 points");
  shapeCheckTrue(recognizedAs(recognizeHold(heldStroke(sqrtVerts, 4)), "polyline"),
      "square root sign with rounded corners snaps to a polyline");
  std::vector<Vec2> smallSqrt;
  for(const Vec2& vertex : sqrtVerts)
    smallSqrt.push_back(vertex*0.5 + Vec2(300, 300));
  shapeCheckTrue(recognizedAs(recognizeHold(heldStroke(smallSqrt)), "polyline"), "small square root snaps to a polyline");
  shapeCheckTrue(recognizedAs(recognizeHold(heldStroke({Vec2(0, 0), Vec2(0, 120), Vec2(90, 120)})), "polyline"),
      "an L snaps to a polyline");

  // triangle, closed with a small overshoot past the start
  std::vector<Vec2> triVerts = {Vec2(100, 0), Vec2(180, 140), Vec2(20, 140), Vec2(100, 0), Vec2(104, 7)};
  shaperec::Result triRes = recognizeHold(heldStroke(triVerts));
  shapeCheckTrue(recognizedAs(triRes, "triangle"), "triangle snaps to a triangle");
  if(triRes.points.size() == 3) {
    for(const Vec2& corner : {triVerts[0], triVerts[1], triVerts[2]}) {
      double nearest = 1E9;
      for(const Vec2& pt : triRes.points)
        nearest = std::min(nearest, shaperec::dist(pt, corner));
      shapeCheckTrue(nearest < 5, "triangle corner where drawn");
    }
  }
  else
    shapeCheckTrue(false, "triangle has 3 corners");
  // started mid-side, rounded corners, stopping a little short
  shapeCheckTrue(recognizedAs(recognizeHold(heldStroke(
      {Vec2(60, 140), Vec2(0, 140), Vec2(70, 10), Vec2(150, 140), Vec2(70, 141)}, 6)), "triangle"),
      "rounded triangle started mid-side snaps to a triangle");

  // Curves and handwriting are never polylines or triangles.  What they are otherwise is the older
  //  recognizer's business (the loose hold pass takes an S for a bowed line and a loop for an ellipse),
  //  so these check that switching the two new kinds off changes nothing.
  std::vector<std::vector<Vec2>> curves = {sampledCurve(sineWaveY, 0, 150), sampledCurve(cursiveUY, 0, 160),
      cursiveLoop(), sampledCurve(arcY, -60, 60), sampledCurve(sineWaveY, 0, 300)};
  for(const std::vector<Vec2>& curve : curves) {
    shaperec::Result curveRes = recognizeHold(curve);
    shapeCheckTrue(!recognizedAs(curveRes, "polyline") && !recognizedAs(curveRes, "triangle"),
        "a curve is not a polyline or triangle");
    shapeCheckTrue(curveRes.kind == recognizeHold(curve, false, false).kind,
        "a curve is recognized as it was before triangles and polylines");
  }

  // the two settings
  shapeCheckTrue(!recognizedAs(recognizeHold(heldStroke(sqrtVerts), true, false), "polyline"),
      "polyline snapping off: no polyline");
  shapeCheckTrue(!recognizedAs(recognizeHold(heldStroke(triVerts), false, true), "triangle"),
      "triangle snapping off: no triangle");

  // unchanged: line and rectangle
  shapeCheckTrue(recognizedAs(recognizeHold(heldStroke({Vec2(0, 0), Vec2(200, 30)})), "line"), "line still a line");
  shapeCheckTrue(recognizedAs(recognizeHold(heldStroke(
      {Vec2(0, 0), Vec2(160, 0), Vec2(160, 100), Vec2(0, 100), Vec2(0, 0), Vec2(10, 0)})), "quad"),
      "rectangle still a rectangle");
  shapeCheckTrue(recognizedAs(recognizeHold(heldStroke(
      {Vec2(0, 0), Vec2(160, 0), Vec2(160, 100), Vec2(0, 100), Vec2(0, 0), Vec2(10, 0)}, 8)), "quad"),
      "rounded rectangle still a rectangle");
}

int runShapeTests()
{
  nShapeChecksFailed = 0;
  testRegistry();
  testPointSerialization();
  testBoxGeometry();
  testRoundedBoxRadius();
  testBoxCornerHandles();
  testArrowHead();
  testCurveArrowHead();
  testPolyline();
  testFilletPolyline();
  testCurvePolylines();
  testPolylineFamilyHeads();
  testConstraints();
  testAngleSnap();
  testEllipse();
  testDegenerateDescriptors();
  testShapeRecognition();
  return nShapeChecksFailed;
}

#ifdef SHAPETEST_MAIN
int main()
{
  int nFailed = runShapeTests();
  printf(nFailed ? "shapetest: %d checks FAILED\n" : "shapetest: all checks passed\n", nFailed);
  return nFailed ? 1 : 0;
}
#endif
