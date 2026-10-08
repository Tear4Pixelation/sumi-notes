#include "rulingregion.h"

#include <cstdlib>
#include <cstdio>
#include <cctype>
#include <algorithm>

// RulingFrame

Point RulingFrame::toLocal(Point p) const
{
  Point d = p - origin;
  Dim c = std::cos(angle), s = std::sin(angle);
  return Point(c*d.x + s*d.y, -s*d.x + c*d.y);
}

Point RulingFrame::toPage(Point local) const
{
  return origin + toPageDir(local);
}

Point RulingFrame::toPageDir(Point localdir) const
{
  Dim c = std::cos(angle), s = std::sin(angle);
  return Point(c*localdir.x - s*localdir.y, s*localdir.x + c*localdir.y);
}

Point RulingFrame::toLocalDir(Point pagedir) const
{
  Dim c = std::cos(angle), s = std::sin(angle);
  return Point(c*pagedir.x + s*pagedir.y, -s*pagedir.x + c*pagedir.y);
}

// Transform2D(m0..m5) maps (x, y) to (m0 x + m2 y + m4, m1 x + m3 y + m5)
Transform2D RulingFrame::localTransform() const
{
  Dim c = std::cos(angle), s = std::sin(angle);
  return Transform2D(c, -s, s, c, -(c*origin.x + s*origin.y), s*origin.x - c*origin.y);
}

Transform2D RulingFrame::pageTransform() const
{
  Dim c = std::cos(angle), s = std::sin(angle);
  return Transform2D(c, s, -s, c, origin.x, origin.y);
}

Rect RulingFrame::localBBox(const Rect& r) const
{
  if(angle == 0)
    return r.isValid() ? Rect(r).translate(-origin.x, -origin.y) : r;
  Rect result;
  for(Point p : { Point(r.left, r.top), Point(r.right, r.top), Point(r.right, r.bottom), Point(r.left, r.bottom) })
    result.rectUnion(toLocal(p));
  return result;
}

Rect RulingFrame::pageBBox(const Rect& r) const
{
  if(angle == 0)
    return r.isValid() ? Rect(r).translate(origin.x, origin.y) : r;
  Rect result;
  for(Point p : { Point(r.left, r.top), Point(r.right, r.top), Point(r.right, r.bottom), Point(r.left, r.bottom) })
    result.rectUnion(toPage(p));
  return result;
}

int RulingFrame::line(Point p, Dim fallback) const
{
  return int(std::floor(toLocal(p).y/yrulingOr(fallback)));
}

std::vector<Point> RulingFrame::bandPolygon(int line, Dim x0, Dim x1, Dim fallback) const
{
  Dim top = yForLine(line, fallback);
  Dim bottom = top + yrulingOr(fallback);
  return { toPage(Point(x0, top)), toPage(Point(x1, top)), toPage(Point(x1, bottom)), toPage(Point(x0, bottom)) };
}

Point RulingFrame::snapToGrid(Point p, Dim fallback) const
{
  Dim yr = yrulingOr(fallback);
  Dim xr = xRuling > 0 ? xRuling : yr;
  Point local = toLocal(p);
  return toPage(Point(std::floor(local.x/xr + 0.5)*xr, std::floor(local.y/yr + 0.5)*yr));
}

InsertLinesStart insertLinesStart(Dim localY, Dim yr, bool skipLines, const std::function<bool(int)>& lineHasInk)
{
  int rule = int(std::floor(localY/yr + 0.5));
  if(std::abs(localY - rule*yr) <= INSERT_LINES_SNAP*yr)
    return {rule, true};
  int line = int(std::floor(localY/yr));
  return {line, skipLines && !lineHasInk(line)};
}

// RulingRegionParams

constexpr Dim RulingRegionParams::MIN_PITCH;  // C++14: std::max takes it by reference

// a line count beyond which a region is not drawn line by line; only reachable with a pitch far below
//  anything the UI offers, but a corrupt file should not be able to hang the renderer
static constexpr int MAX_REGION_LINES = 4000;

static std::vector<Point> localCorners(const RulingRegionParams& params)
{
  RulingFrame frame = params.frame();
  std::vector<Point> local;
  local.reserve(params.corners.size());
  for(const Point& p : params.corners)
    local.push_back(frame.toLocal(p));
  return local;
}

bool RulingRegionParams::contains(Point p) const
{
  return isValid() && pointInPolygon(corners, p);
}

Point RulingRegionParams::clampInside(Point p) const
{
  if(!isValid() || contains(p))
    return p;
  Point best = p;
  Dim bestdist = MAX_DIM;
  for(size_t ii = 0; ii < corners.size(); ++ii) {
    Point a = corners[ii], b = corners[(ii + 1) % corners.size()];
    Point ab = b - a;
    Dim len2 = ab.x*ab.x + ab.y*ab.y;
    Dim t = len2 > 0 ? std::min(Dim(1), std::max(Dim(0), ((p.x - a.x)*ab.x + (p.y - a.y)*ab.y)/len2)) : 0;
    Point q = a + t*ab;
    Dim dist = q.dist(p);
    if(dist < bestdist) {
      bestdist = dist;
      best = q;
    }
  }
  return best;
}

Rect RulingRegionParams::bounds() const
{
  Rect r;
  for(const Point& p : corners)
    r.rectUnion(Rect::ltrb(p.x, p.y, p.x, p.y));
  return r;
}

Path2D RulingRegionParams::outlinePath() const
{
  Path2D path;
  if(!isValid())
    return path;
  path.moveTo(corners[0]);
  for(size_t ii = 1; ii < corners.size(); ++ii)
    path.lineTo(corners[ii]);
  path.closeSubpath();
  return path;
}

// Crossings of the horizontal line y = `level` with a polygon, as sorted x values.  Half-open on y, so
//  a line through a vertex counts it once and a line along a horizontal edge counts nothing; pairing
//  the sorted crossings then gives the inside spans of any simple polygon, convex or not.
static std::vector<Dim> spanCrossings(const std::vector<Point>& poly, Dim level)
{
  std::vector<Dim> xs;
  for(size_t ii = 0, jj = poly.size() - 1; ii < poly.size(); jj = ii++) {
    const Point& a = poly[jj];
    const Point& b = poly[ii];
    if((a.y <= level) != (b.y <= level))
      xs.push_back(a.x + (level - a.y)*(b.x - a.x)/(b.y - a.y));
  }
  std::sort(xs.begin(), xs.end());
  return xs;
}

// Lines of one ruling family, clipped to the outline.  Works in a frame where the family's lines are
//  horizontal: `local` is the outline in that frame and `toPage` maps a point back.  Lines lying on the
//  outline's own top or bottom extreme are skipped - a region's border is not one of its rule lines,
//  just as the page's ruling starts one pitch below the top edge.
template<typename ToPage>
static void addClippedLines(Path2D& path, const std::vector<Point>& local, Dim pitch, ToPage toPage)
{
  if(pitch <= 0 || local.size() < 3)
    return;
  Dim ymin = local[0].y, ymax = local[0].y;
  for(const Point& p : local) {
    ymin = std::min(ymin, p.y);
    ymax = std::max(ymax, p.y);
  }
  const Dim eps = 1E-6*std::max(Dim(1), ymax - ymin);
  int kmin = int(std::floor((ymin + eps)/pitch)) + 1;
  int kmax = int(std::ceil((ymax - eps)/pitch)) - 1;
  if(kmax - kmin > MAX_REGION_LINES)
    return;
  for(int k = kmin; k <= kmax; ++k) {
    Dim level = k*pitch;
    std::vector<Dim> xs = spanCrossings(local, level);
    for(size_t ii = 0; ii + 1 < xs.size(); ii += 2)
      path.addLine(toPage(Point(xs[ii], level)), toPage(Point(xs[ii+1], level)));
  }
}

Path2D RulingRegionParams::linesPath() const
{
  Path2D path;
  if(!isValid() || dotRadius > 0)
    return path;
  RulingFrame frame = this->frame();
  std::vector<Point> local = localCorners(*this);
  addClippedLines(path, local, yRuling, [&](Point p){ return frame.toPage(p); });
  // the x ruling's lines are horizontal in a frame turned a further quarter turn: swap the axes
  std::vector<Point> swapped;
  for(const Point& p : local)
    swapped.push_back(Point(p.y, p.x));
  addClippedLines(path, swapped, xRuling, [&](Point p){ return frame.toPage(Point(p.y, p.x)); });
  return path;
}

Path2D RulingRegionParams::dotsPath() const
{
  Path2D path;
  if(!isValid() || dotRadius <= 0 || (xRuling <= 0 && yRuling <= 0))
    return path;
  // same pattern as Page::generateRuleLayer: dots at the grid intersections with both rulings set,
  //  otherwise a dotted line along the one that is
  Dim r = dotRadius;
  Dim linePitch = std::max(4*r, Dim(4));
  Dim dx = xRuling > 0 ? xRuling : linePitch;
  Dim dy = yRuling > 0 ? yRuling : linePitch;
  RulingFrame frame = this->frame();
  std::vector<Point> local = localCorners(*this);
  Rect lb;
  for(const Point& p : local)
    lb.rectUnion(Rect::ltrb(p.x, p.y, p.x, p.y));
  Dim x0 = xRuling > 0 ? 0 : dx/2;
  Dim y0 = yRuling > 0 ? 0 : dy/2;
  int imin = int(std::ceil((lb.left - x0)/dx)), imax = int(std::floor((lb.right - x0)/dx));
  int jmin = int(std::ceil((lb.top - y0)/dy)), jmax = int(std::floor((lb.bottom - y0)/dy));
  if(double(imax - imin + 1)*double(jmax - jmin + 1) > double(MAX_REGION_LINES)*100)
    return path;
  for(int j = jmin; j <= jmax; ++j) {
    for(int i = imin; i <= imax; ++i) {
      Point lp(x0 + i*dx, y0 + j*dy);
      // keep a dot clear of the border, which a dot straddling it would blur
      if(!pointInPolygon(local, lp))
        continue;
      Point pp = frame.toPage(lp);
      path.addEllipse(pp.x, pp.y, r, r);
    }
  }
  return path;
}

void RulingRegionParams::sanitize()
{
  auto finite = [](Dim v) { return std::isfinite(v); };
  for(const Point& p : corners) {
    if(!finite(p.x) || !finite(p.y)) {
      corners.clear();
      break;
    }
  }
  if(!finite(origin.x) || !finite(origin.y))
    origin = corners.empty() ? Point(0, 0) : corners[0];
  if(!finite(angle))
    angle = 0;
  auto pitch = [](Dim v) { return std::isfinite(v) && v > 0 ? std::max(v, MIN_PITCH) : Dim(0); };
  xRuling = pitch(xRuling);
  yRuling = pitch(yRuling);
  dotRadius = std::isfinite(dotRadius) && dotRadius > 0 ? std::min(dotRadius, Dim(50)) : Dim(0);
}

void RulingRegionParams::transform(const Transform2D& tf)
{
  Point o = tf.map(origin);
  RulingFrame f = frame();
  Point ux = tf.map(origin + f.toPageDir(Point(1, 0))) - o;
  Point uy = tf.map(origin + f.toPageDir(Point(0, 1))) - o;
  for(Point& p : corners)
    p = tf.map(p);
  origin = o;
  // A mirror is not expressible (the ruling has no handedness to flip): taking the angle from the image
  //  of x alone would turn the frame upside down.  Take it from the image of y instead, so local y still
  //  points down the lines the ink was written on.
  if(cross(ux, uy) < 0)
    angle = std::atan2(-uy.x, uy.y);
  else
    angle = std::atan2(ux.y, ux.x);
  // pitches follow the scale along their own axes, so a uniformly scaled region keeps its ink on its lines
  xRuling *= ux.dist();
  yRuling *= uy.dist();
  dotRadius *= std::sqrt(ux.dist()*uy.dist());
}

int RulingRegionParams::bottomRightCorner() const
{
  if(!isValid())
    return 0;
  std::vector<Point> local = localCorners(*this);
  int best = 0;
  for(size_t ii = 1; ii < local.size(); ++ii) {
    if(local[ii].x + local[ii].y > local[best].x + local[best].y)
      best = int(ii);
  }
  return best;
}

RulingRegionParams RulingRegionParams::fromRect(const Rect& r, Dim xr, Dim yr, Dim dotr)
{
  RulingRegionParams params;
  params.corners = { Point(r.left, r.top), Point(r.right, r.top), Point(r.right, r.bottom), Point(r.left, r.bottom) };
  params.origin = Point(r.left, r.top);
  params.xRuling = xr;
  params.yRuling = yr;
  params.dotRadius = dotr;
  params.outline = true;
  return params;
}

std::string serializeRegionPoints(const std::vector<Point>& points)
{
  std::string out;
  char buff[64];
  for(const Point& p : points) {
    if(!out.empty())
      out += ' ';
    snprintf(buff, sizeof(buff), "%.9g,%.9g", double(p.x), double(p.y));
    out += buff;
  }
  return out;
}

void parseRegionPoints(const char* str, std::vector<Point>& points)
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
    points.push_back(Point(x, y));
  }
}

Dim selectionLineHeight(const std::vector<Point>& centres, const std::function<RulingFrame(Point)>& frameAt, Dim fallback)
{
  std::vector<RulingFrame> frames;
  std::vector<int> votes;
  int best = -1;
  for(const Point& centre : centres) {
    RulingFrame frame = frameAt(centre);
    size_t idx = 0;
    while(idx < frames.size() && frames[idx].region != frame.region)
      ++idx;
    if(idx == frames.size()) {
      frames.push_back(frame);
      votes.push_back(0);
    }
    ++votes[idx];
    // strictly greater: a tie stays with the region met first
    if(best < 0 || votes[idx] > votes[best])
      best = int(idx);
  }
  return best < 0 ? fallback : frames[best].yrulingOr(fallback);
}
