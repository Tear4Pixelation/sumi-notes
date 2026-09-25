#include "shape.h"

#include <cstdlib>
#include <cstdio>
#include <cctype>
#include <cstring>

// SHAPES_SPEC.md deviation, deliberate: the arrowhead is drawn as two stroked lines forming an open V
//  rather than as a filled path inside a <g>.  The spec's reason for the group was that a single filled
//  outline would bake the stroke width into the geometry; that does not apply here because the whole path
//  is regenerated from the descriptor on every change, so nothing is ever baked.  Keeping an arrow as one
//  plain SvgPath means it is not a multi-stroke element (spec 7.2) and carries no filled part that
//  Element::scaleWidth() could misread as pen geometry (spec 7.9) - both traps disappear rather than
//  having to be handled.

Rect ShapeParams::rect() const
{
  if(points.size() < 2)
    return Rect();
  return Rect::ltrb(std::min(points[0].x, points[1].x), std::min(points[0].y, points[1].y),
      std::max(points[0].x, points[1].x), std::max(points[0].y, points[1].y));
}

// path builders

static void addArrowHead(Path2D& path, const Point& tip, const Point& from, Dim strokeWidth)
{
  Point along = tip - from;
  Dim shaftLen = along.dist();
  if(shaftLen <= 0)
    return;
  along = along/shaftLen;
  // head must not swallow a short shaft, but must not grow with a long one either
  Dim headLen = std::min(ARROWHEAD_WIDTHS*std::max(strokeWidth, Dim(0.01)), Dim(0.4)*shaftLen);
  const Dim halfAngle = 25*M_PI/180;
  Dim cosa = std::cos(halfAngle), sina = std::sin(halfAngle);
  Point back(-along.x, -along.y);
  Point left(back.x*cosa - back.y*sina, back.x*sina + back.y*cosa);
  Point right(back.x*cosa + back.y*sina, -back.x*sina + back.y*cosa);
  path.moveTo(tip + headLen*left);
  path.lineTo(tip);
  path.lineTo(tip + headLen*right);
}

static Path2D buildLinePath(const ShapeParams& params)
{
  Path2D path;
  if(params.points.size() >= 2)
    path.addLine(params.points[0], params.points[1]);
  return path;
}

static Path2D buildLineFamilyPath(const ShapeParams& params)
{
  Path2D path;
  if(params.points.size() < 2)
    return path;
  const Point& tail = params.points[0];
  const Point& head = params.points[1];
  path.addLine(tail, head);
  if(params.flags & SHAPEFLAG_HEADEND)
    addArrowHead(path, head, tail, params.strokeWidth);
  if(params.flags & SHAPEFLAG_HEADSTART)
    addArrowHead(path, tail, head, params.strokeWidth);
  return path;
}

// rounded-rect construction copied from SvgRect::updatePath(), including its min(w,h)/2 radius clamping;
//  see SHAPES_SPEC.md 1 for why boxes are plain SvgPaths rather than SvgRects
static Path2D buildBoxPath(const ShapeParams& params)
{
  Path2D path;
  Rect r = params.rect();
  Dim w = r.width(), h = r.height();
  if(w <= 0 || h <= 0)
    return path;
  Dim x = r.left, y = r.top;
  Dim rmax = std::min(w, h)/2;
  Dim rx = params.rounded() ? std::min(std::max(params.rx, Dim(0)), rmax) : 0;
  Dim ry = params.rounded() ? std::min(std::max(params.ry, Dim(0)), rmax) : 0;
  if(rx <= 0 && ry <= 0) {
    path.addRect(r);
    return path;
  }
  // a single non-zero radius behaves as in SVG: the other takes its value
  if(rx <= 0) rx = ry;
  if(ry <= 0) ry = rx;
  path.moveTo(x, y + ry);
  path.addArc(x + rx, y + ry, rx, ry, M_PI, M_PI/2);
  path.lineTo(x + w - rx, y);
  path.addArc(x + w - rx, y + ry, rx, ry, -M_PI/2, M_PI/2);
  path.lineTo(x + w, y + h - ry);
  path.addArc(x + w - rx, y + h - ry, rx, ry, 0, M_PI/2);
  path.lineTo(x + rx, y + h);
  path.addArc(x + rx, y + h - ry, rx, ry, M_PI/2, M_PI/2);
  path.closeSubpath();
  return path;
}

static Path2D buildEllipsePath(const ShapeParams& params)
{
  Path2D path;
  Rect r = params.rect();
  if(r.width() <= 0 || r.height() <= 0)
    return path;
  path.addEllipse(r.center().x, r.center().y, r.width()/2, r.height()/2);
  return path;
}

// The four polyline flavours share their points, their handles and their arrowheads, and differ only in
//  how the body between the points is drawn.  They are separate shapes rather than a flag on one shape
//  because they are separate tools: a filleted route and a smoothed curve get reached for in different
//  situations, so each deserves its own button.
//
// The arrowhead direction is the same for all four: the first/last *segment* direction.  For the two
//  spline flavours that is not a coincidence - each pins its ends (see below) in a way that makes the
//  curve's end tangent come out exactly along that segment, so a head never sits skew to its curve.

// how much of each adjacent segment a fillet is allowed to eat; past 1/2 neighbouring fillets would
//  overlap and the corner geometry would fold back on itself
static Dim filletTrim(const ShapeParams& params, const Point& vertex, const Point& a, const Point& b)
{
  Dim trim = std::max(params.rx, Dim(0));
  return std::min(trim, std::min((a - vertex).dist(), (b - vertex).dist())/2);
}

// circular fillet at vertex v between the edges towards a and b; rx is the distance trimmed off each
//  edge, matching what rx means for a rounded box (where a 90 degree corner makes the two equal)
static void addFillet(Path2D& path, const Point& v, const Point& a, const Point& b, Dim trim, bool start)
{
  Point d1 = a - v, d2 = b - v;
  Dim l1 = d1.dist(), l2 = d2.dist();
  if(trim <= 0 || l1 <= 0 || l2 <= 0) {
    start ? path.moveTo(v) : path.lineTo(v);
    return;
  }
  d1 = d1/l1;
  d2 = d2/l2;
  Point p1 = v + trim*d1, p2 = v + trim*d2;
  Point bisector = d1 + d2;
  Dim blen = bisector.dist();
  // collinear edges (a straight-through vertex) have no corner to round
  if(blen < 1e-9) {
    start ? path.moveTo(p1) : path.lineTo(p1);
    path.lineTo(p2);
    return;
  }
  bisector = bisector/blen;
  Dim halfAngle = std::acos(std::min(Dim(1), std::max(Dim(-1), dot(d1, bisector))));
  Dim radius = trim*std::tan(halfAngle);
  Point center = v + bisector*(trim/std::max(std::cos(halfAngle), Dim(1e-9)));
  Dim a1 = std::atan2(p1.y - center.y, p1.x - center.x);
  Dim a2 = std::atan2(p2.y - center.y, p2.x - center.x);
  Dim sweep = a2 - a1;
  while(sweep > M_PI) sweep -= 2*M_PI;
  while(sweep < -M_PI) sweep += 2*M_PI;
  start ? path.moveTo(p1) : path.lineTo(p1);
  path.addArc(center.x, center.y, radius, radius, a1, sweep);
}

static void addPolylineBody(Path2D& path, const ShapeParams& params)
{
  const std::vector<Point>& pts = params.points;
  size_t n = pts.size();
  path.moveTo(pts[0]);
  for(size_t ii = 1; ii < n; ++ii)
    path.lineTo(pts[ii]);
  if(params.flags & SHAPEFLAG_CLOSED)
    path.closeSubpath();
}

static void addFilletBody(Path2D& path, const ShapeParams& params)
{
  const std::vector<Point>& pts = params.points;
  size_t n = pts.size();
  bool closed = (params.flags & SHAPEFLAG_CLOSED) != 0;
  if(!closed) {
    path.moveTo(pts[0]);
    for(size_t ii = 1; ii + 1 < n; ++ii)
      addFillet(path, pts[ii], pts[ii-1], pts[ii+1], filletTrim(params, pts[ii], pts[ii-1], pts[ii+1]), false);
    path.lineTo(pts[n-1]);
    return;
  }
  // closed: every vertex is interior, including the one the path starts at
  for(size_t ii = 0; ii < n; ++ii) {
    const Point& prev = pts[(ii + n - 1) % n];
    const Point& next = pts[(ii + 1) % n];
    addFillet(path, pts[ii], prev, next, filletTrim(params, pts[ii], prev, next), ii == 0);
  }
  path.closeSubpath();
}

// Catmull-Rom converted to cubic Beziers: the curve passes through every point.
//
// *Centripetal* parameterisation (the square root below), not uniform.  The two are identical when the
//  points are evenly spaced, so this changes nothing on a neat rectangle - it pays off when the spacing
//  is uneven, which is what tapped-out points actually look like.  Measured on four points whose middle
//  segment is 4 units against neighbours of 100 and 113, worst overshoot past the points goes from 10.0
//  units (uniform) to 0.6; at a 20:1 spacing ratio, from 20.0 to 9.9.  Uniform Catmull-Rom can also cusp
//  or self-intersect within a segment on input like that; centripetal provably cannot.
//
// The open ends use a *reflected* phantom point rather than a duplicated one, because a duplicated end
//  point gives a zero-length segment and centripetal has to divide by segment length.  Reflecting also
//  makes the end tangent come out exactly along the first/last segment, which is what the arrowheads
//  are oriented from.
//
// Note this does not (and cannot) remove the bulge where the curve rounds a sharp corner: passing
//  smoothly *through* a right angle requires going outside it.  That is what the fitted curve is for.
static void addSplineBody(Path2D& path, const ShapeParams& params)
{
  const std::vector<Point>& pts = params.points;
  int n = int(pts.size());
  bool closed = (params.flags & SHAPEFLAG_CLOSED) != 0;
  auto at = [&](int ii) -> Point {
    if(closed)
      return pts[((ii % n) + n) % n];
    if(ii < 0)
      return 2*pts[0] - pts[1];
    if(ii > n - 1)
      return 2*pts[n-1] - pts[n-2];
    return pts[ii];
  };
  // knot spacing; 0.5 is centripetal, 0 would be uniform and 1 chordal
  auto knotDelta = [](const Point& a, const Point& b) {
    return std::sqrt(std::max((b - a).dist(), Dim(0)));
  };
  path.moveTo(pts[0]);
  int last = closed ? n : n - 1;
  for(int ii = 0; ii < last; ++ii) {
    Point p0 = at(ii - 1), p1 = at(ii), p2 = at(ii + 1), p3 = at(ii + 2);
    Dim d1 = knotDelta(p0, p1), d2 = knotDelta(p1, p2), d3 = knotDelta(p2, p3);
    // coincident points collapse the parameterisation; uniform is the right fallback there
    if(d1 < 1e-9 || d2 < 1e-9 || d3 < 1e-9) {
      path.cubicTo(p1 + (p2 - p0)/6, p2 - (p3 - p1)/6, p2);
      continue;
    }
    Point m1 = d2*((p1 - p0)/d1 - (p2 - p0)/(d1 + d2) + (p2 - p1)/d2);
    Point m2 = d2*((p2 - p1)/d2 - (p3 - p1)/(d2 + d3) + (p3 - p2)/d3);
    path.cubicTo(p1 + m1/3, p2 - m2/3, p2);
  }
  if(closed)
    path.closeSubpath();
}

// The fitted curve is pulled towards each point without passing through it, which is what makes it read
//  as a smoothed version of the input rather than a curve threaded through noise.
//
// It is built by *relaxing* each point towards where a uniform cubic B-spline would put its knot,
//  (P[i-1] + 4P[i] + P[i+1])/6, and then running the smooth curve through the relaxed points.  That
//  gives a single knob: tightness 0 is the pure B-spline, 1 is the smooth curve, and anything between
//  trades roundness for fidelity to the original points.  Building it this way also means the fitted
//  curve inherits centripetal parameterisation from addSplineBody() rather than repeating it.
//
// The two end points are never relaxed, so the ends stay exactly where they were put - which is what
//  keeps the arrowheads attached.
// where the pure B-spline would put its knot for point ii - the fully-relaxed position.  The curve's
//  actual position for that point is knot + tightness*(P - knot), so tightness 0 sits here and 1 sits
//  on P itself.  End points of an open curve are never relaxed, so their knot is the point.
static Point fitKnot(const ShapeParams& params, int ii)
{
  const std::vector<Point>& pts = params.points;
  int n = int(pts.size());
  bool closed = (params.flags & SHAPEFLAG_CLOSED) != 0;
  if(ii < 0 || ii >= n)
    return Point(0, 0);
  if(!closed && (ii == 0 || ii == n - 1))
    return pts[ii];
  return (pts[((ii - 1) % n + n) % n] + 4*pts[ii] + pts[(ii + 1) % n])/6;
}

static void addFitBody(Path2D& path, const ShapeParams& params)
{
  int n = int(params.points.size());
  Dim tight = std::min(Dim(1), std::max(Dim(0), params.tightness));

  ShapeParams relaxed = params;
  relaxed.points.resize(n);
  for(int ii = 0; ii < n; ++ii) {
    Point knot = fitKnot(params, ii);
    relaxed.points[ii] = knot + tight*(params.points[ii] - knot);
  }
  addSplineBody(path, relaxed);
}

// the point whose knot is furthest from it - the one where the tightness handle has the most travel,
//  and so the one where dragging it reads most clearly
static int fitLoosestPoint(const ShapeParams& params)
{
  int best = -1;
  Dim bestDist = 0;
  for(int ii = 0; ii < int(params.points.size()); ++ii) {
    Dim d = (params.points[ii] - fitKnot(params, ii)).dist();
    if(d > bestDist) {
      bestDist = d;
      best = ii;
    }
  }
  return bestDist > 1e-6 ? best : -1;
}

static Path2D buildPolylineFamilyPath(const ShapeParams& params)
{
  Path2D path;
  if(params.points.empty())
    return path;
  if(params.points.size() < 2) {
    path.moveTo(params.points[0]);
    return path;
  }
  switch(params.id) {
  case SHAPE_CURVE:       addFitBody(path, params);  break;
  default:
    params.rounded() ? addFilletBody(path, params) : addPolylineBody(path, params);
    break;
  }
  if(params.flags & SHAPEFLAG_CLOSED)
    return path;  // heads are only allowed on open ends
  size_t n = params.points.size();
  if(params.flags & SHAPEFLAG_HEADEND)
    addArrowHead(path, params.points[n-1], params.points[n-2], params.strokeWidth);
  if(params.flags & SHAPEFLAG_HEADSTART)
    addArrowHead(path, params.points[0], params.points[1], params.strokeWidth);
  return path;
}

// handles

static void pointHandles(const ShapeParams& params, std::vector<ShapeHandle>& handles)
{
  for(size_t ii = 0; ii < params.points.size(); ++ii)
    handles.push_back({ShapeHandle::POINT, params.points[ii], int(ii)});
}

// the radius handle appears only while rounding is switched on, so there is never a handle for a
//  parameter that is not currently affecting the shape
static void boxHandles(const ShapeParams& params, std::vector<ShapeHandle>& handles)
{
  Rect r = params.rect();
  if(!r.isValid())
    return;
  handles.push_back({ShapeHandle::BOX_CORNER, Point(r.left, r.top), 0});
  handles.push_back({ShapeHandle::BOX_CORNER, Point(r.right, r.top), 1});
  handles.push_back({ShapeHandle::BOX_CORNER, Point(r.right, r.bottom), 2});
  handles.push_back({ShapeHandle::BOX_CORNER, Point(r.left, r.bottom), 3});
  if(!params.rounded())
    return;
  // conventional placement: inset from the top-left corner along the corner arc
  Dim rmax = std::min(r.width(), r.height())/2;
  Dim rx = std::min(std::max(params.rx, Dim(0)), rmax);
  Dim ry = std::min(std::max(params.ry, Dim(0)), rmax);
  handles.push_back({ShapeHandle::RADIUS, Point(r.left + rx, r.top + ry), 0});
}

// a filleted polyline gets its radius handle on the first corner that actually has one, sitting at the
//  trim point on the incoming edge - the same place the rounded box's handle sits relative to its corner
static void polylineHandles(const ShapeParams& params, std::vector<ShapeHandle>& handles)
{
  pointHandles(params, handles);
  if(!params.rounded())
    return;
  const std::vector<Point>& pts = params.points;
  size_t n = pts.size();
  bool closed = (params.flags & SHAPEFLAG_CLOSED) != 0;
  if(n < 3 && !(closed && n >= 3))
    return;  // no interior vertex, so no corner to round
  size_t vertex = closed ? 0 : 1;
  const Point& prev = pts[(vertex + n - 1) % n];
  const Point& next = pts[(vertex + 1) % n];
  Point d = prev - pts[vertex];
  Dim len = d.dist();
  if(len <= 0)
    return;
  Dim trim = filletTrim(params, pts[vertex], prev, next);
  handles.push_back({ShapeHandle::RADIUS, pts[vertex] + (d/len)*trim, int(vertex)});
}

// point handles plus a tightness handle sliding between the fully-relaxed position and the point
//  itself, so the smooth/faithful trade-off is a drag on the curve rather than a trip to preferences
static void curveHandles(const ShapeParams& params, std::vector<ShapeHandle>& handles)
{
  pointHandles(params, handles);
  int ii = fitLoosestPoint(params);
  if(ii < 0)
    return;
  Point knot = fitKnot(params, ii);
  Dim tight = std::min(Dim(1), std::max(Dim(0), params.tightness));
  handles.push_back({ShapeHandle::TIGHTNESS, knot + tight*(params.points[ii] - knot), ii});
}

// constraints (constrain modifier held)

static void constrainVector(ShapeParams& params)
{
  if(params.points.size() < 2)
    return;
  Point dr = params.points[1] - params.points[0];
  Dim len = dr.dist();
  if(len <= 0)
    return;
  Dim angle = std::atan2(dr.y, dr.x);
  Dim snapped = std::floor(angle/(M_PI/4) + 0.5)*(M_PI/4);
  params.points[1] = params.points[0] + len*Point(std::cos(snapped), std::sin(snapped));
}

static void constrainBox(ShapeParams& params)
{
  if(params.points.size() < 2)
    return;
  Point dr = params.points[1] - params.points[0];
  Dim side = std::max(std::abs(dr.x), std::abs(dr.y));
  params.points[1] = params.points[0] + Point(dr.x < 0 ? -side : side, dr.y < 0 ? -side : side);
}

static void constrainPolyline(ShapeParams& params)
{
  size_t n = params.points.size();
  if(n < 2)
    return;
  Point dr = params.points[n-1] - params.points[n-2];
  Dim len = dr.dist();
  if(len <= 0)
    return;
  Dim snapped = std::floor(std::atan2(dr.y, dr.x)/(M_PI/4) + 0.5)*(M_PI/4);
  params.points[n-1] = params.points[n-2] + len*Point(std::cos(snapped), std::sin(snapped));
}

// the registry; order must match ShapeId
static const ShapeDef shapeDefs[SHAPE_COUNT] = {
  {"line", "Line", ":/icons/ic_menu_shape_line.svg", SHAPEGESTURE_DRAG_VECTOR, 2, 2, true, false,
      buildLineFamilyPath, pointHandles, constrainVector},
  {"box", "Box", ":/icons/ic_menu_shape_box.svg", SHAPEGESTURE_DRAG_BBOX, 2, 2, false, true,
      buildBoxPath, boxHandles, constrainBox},
  {"ellipse", "Ellipse", ":/icons/ic_menu_shape_ellipse.svg", SHAPEGESTURE_DRAG_BBOX, 2, 2, false, false,
      buildEllipsePath, boxHandles, constrainBox},
  {"polyline", "Polyline", ":/icons/ic_menu_shape_polyline.svg",
      SHAPEGESTURE_MULTIPOINT, 2, -1, true, true,
      buildPolylineFamilyPath, polylineHandles, constrainPolyline},
  {"fitpoly", "Curve", ":/icons/ic_menu_shape_fitpoly.svg",
      SHAPEGESTURE_MULTIPOINT, 2, -1, true, false,
      buildPolylineFamilyPath, curveHandles, constrainPolyline},
};

// "arrow", "rbox" and "rpolyline" were shapes of their own before they became flags; documents written
//  then still name them, and the flags are exactly what those names used to mean
// tightness < 0 means "leave it alone"; "splinepoly" was the interpolating curve, which is exactly
//  SHAPE_CURVE at tightness 1 - verified bit-identical, which is why it is no longer a shape of its own
static const struct { const char* id; int shape; int flags; Dim tightness; } shapeAliases[] = {
  {"arrow", SHAPE_LINE, SHAPEFLAG_HEADEND, -1},
  {"rbox", SHAPE_BOX, SHAPEFLAG_ROUNDED, -1},
  {"rpolyline", SHAPE_POLYLINE, SHAPEFLAG_ROUNDED, -1},
  {"splinepoly", SHAPE_CURVE, 0, 1},
};

bool shapeIsPolylineFamily(int id)
{
  return id == SHAPE_POLYLINE || id == SHAPE_CURVE;
}

const ShapeDef* shapeDef(int id)
{
  return id > SHAPE_NONE && id < SHAPE_COUNT ? &shapeDefs[id] : NULL;
}

int shapeIdByStringId(const char* id, int* extraFlags, Dim* tightness)
{
  if(extraFlags)
    *extraFlags = 0;
  if(tightness)
    *tightness = -1;
  if(!id || !id[0])
    return SHAPE_NONE;
  for(int ii = 0; ii < SHAPE_COUNT; ++ii) {
    if(strcmp(shapeDefs[ii].id, id) == 0)
      return ii;
  }
  for(const auto& alias : shapeAliases) {
    if(strcmp(alias.id, id) == 0) {
      if(extraFlags)
        *extraFlags = alias.flags;
      if(tightness)
        *tightness = alias.tightness;
      return alias.shape;
    }
  }
  return SHAPE_NONE;
}

Path2D buildShapePath(const ShapeParams& params)
{
  const ShapeDef* def = shapeDef(params.id);
  return def ? def->buildPath(params) : Path2D();
}

void getShapeHandles(const ShapeParams& params, std::vector<ShapeHandle>& handles)
{
  const ShapeDef* def = shapeDef(params.id);
  if(def)
    def->getHandles(params, handles);
}

void applyShapeConstraint(ShapeParams& params)
{
  const ShapeDef* def = shapeDef(params.id);
  if(def)
    def->applyConstraint(params);
}

// the multiple of 45 degrees the direction anchor -> pos is within tolerance of, as a unit vector; false
//  if there is none (or pos is on the anchor, where there is no direction)
static bool snappedDirection(Point anchor, Point pos, Dim tolerance, Point& dir)
{
  Point dr = pos - anchor;
  if(dr.dist() <= 0)
    return false;
  Dim angle = std::atan2(dr.y, dr.x);
  Dim snapped = std::floor(angle/(M_PI/4) + 0.5)*(M_PI/4);
  if(std::abs(angle - snapped) > tolerance)
    return false;
  dir = Point(std::cos(snapped), std::sin(snapped));
  return true;
}

Point snapShapeAngle(const ShapeParams& params, int index, Point pos, Dim tolerance)
{
  int npts = int(params.points.size());
  if(tolerance <= 0 || index < 0 || index >= npts || npts < 2)
    return pos;
  if(params.id != SHAPE_LINE && !shapeIsPolylineFamily(params.id))
    return pos;
  bool closed = params.id != SHAPE_LINE && (params.flags & SHAPEFLAG_CLOSED);
  int prevIdx = index > 0 ? index - 1 : (closed ? npts - 1 : -1);
  int nextIdx = index < npts - 1 ? index + 1 : (closed ? 0 : -1);
  if(nextIdx == prevIdx)
    nextIdx = -1;  // two points: the one neighbour is both

  Point anchors[2];
  Point dirs[2];
  int nsnapped = 0;
  for(int neighbour : {prevIdx, nextIdx}) {
    if(neighbour < 0)
      continue;
    Point anchor = params.points[neighbour];
    if(snappedDirection(anchor, pos, tolerance, dirs[nsnapped])) {
      anchors[nsnapped] = anchor;
      ++nsnapped;
    }
  }
  // project onto the snapped ray, which keeps the distance along it and drops only the sideways error
  auto project = [&](int i) { return anchors[i] + dot(pos - anchors[i], dirs[i])*dirs[i]; };
  if(nsnapped == 0)
    return pos;
  if(nsnapped == 2) {
    Dim denom = dirs[0].x*dirs[1].y - dirs[0].y*dirs[1].x;
    if(std::abs(denom) > 1E-9) {
      Point delta = anchors[1] - anchors[0];
      Dim along = (delta.x*dirs[1].y - delta.y*dirs[1].x)/denom;
      return anchors[0] + along*dirs[0];
    }
    // parallel (the neighbours are collinear with pos): either projection lands on the same line
  }
  return project(0);
}

void dragShapeHandle(ShapeParams& params, const ShapeHandle& handle, Point newpos)
{
  switch(handle.type) {
  case ShapeHandle::POINT:
    if(handle.index >= 0 && size_t(handle.index) < params.points.size())
      params.points[handle.index] = newpos;
    break;
  case ShapeHandle::BOX_CORNER:
  {
    Rect r = params.rect();
    if(!r.isValid())
      break;
    // drag one corner; the diagonally opposite one is the anchor
    switch(handle.index) {
    case 0:  r = Rect::ltrb(newpos.x, newpos.y, r.right, r.bottom);  break;
    case 1:  r = Rect::ltrb(r.left, newpos.y, newpos.x, r.bottom);  break;
    case 2:  r = Rect::ltrb(r.left, r.top, newpos.x, newpos.y);  break;
    default: r = Rect::ltrb(newpos.x, r.top, r.right, newpos.y);  break;
    }
    params.points.resize(2);
    params.points[0] = Point(r.left, r.top);
    params.points[1] = Point(r.right, r.bottom);
    break;
  }
  case ShapeHandle::TIGHTNESS:
  {
    if(handle.index < 0 || size_t(handle.index) >= params.points.size())
      break;
    Point knot = fitKnot(params, handle.index);
    Point along = params.points[handle.index] - knot;
    Dim len2 = dot(along, along);
    if(len2 <= 0)
      break;
    // project the drag onto the knot->point segment: at the knot the curve is fully relaxed, at the
    //  point it passes exactly through
    params.tightness = std::min(Dim(1), std::max(Dim(0), dot(newpos - knot, along)/len2));
    break;
  }
  case ShapeHandle::RADIUS:
  {
    if(shapeIsPolylineFamily(params.id)) {
      // the polyline fillet radius is simply the distance back from the vertex the handle hangs off;
      // filletTrim() does the per-corner clamping, so every corner stays well formed at any radius
      if(handle.index >= 0 && size_t(handle.index) < params.points.size()) {
        params.rx = std::max(Dim(0), (newpos - params.points[handle.index]).dist());
        params.ry = params.rx;
      }
      break;
    }
    Rect r = params.rect();
    if(!r.isValid())
      break;
    // clamp here as well as in buildPath, so the handle can never sit somewhere the shape isn't drawn
    Dim rmax = std::min(r.width(), r.height())/2;
    Dim rx = std::min(std::max(newpos.x - r.left, Dim(0)), rmax);
    Dim ry = std::min(std::max(newpos.y - r.top, Dim(0)), rmax);
    // uniform by default; a non-uniform scale is what legitimately makes the two diverge (spec 7.8)
    Dim radius = std::max(rx, ry);
    params.rx = radius;
    params.ry = radius;
    break;
  }
  }
}

// serialization: "x0,y0 x1,y1 ..."

std::string serializeShapePoints(const std::vector<Point>& points)
{
  std::string out;
  char buff[64];
  for(const Point& p : points) {
    if(!out.empty())
      out += ' ';
    // plain snprintf rather than fstring so that shape.cpp stays free of the application's string
    //  helpers and shapetest.cpp can be built standalone (see its header)
    snprintf(buff, sizeof(buff), "%.6g,%.6g", double(p.x), double(p.y));
    out += buff;
  }
  return out;
}

void parseShapePoints(const char* str, std::vector<Point>& points)
{
  points.clear();
  if(!str)
    return;
  const char* s = str;
  while(*s) {
    char* end = NULL;
    double x = strtod(s, &end);
    if(end == s)
      break;
    s = end;
    while(*s == ',' || isspace((unsigned char)*s)) ++s;
    double y = strtod(s, &end);
    if(end == s)
      break;
    s = end;
    while(*s == ',' || isspace((unsigned char)*s)) ++s;
    points.emplace_back(x, y);
  }
}
