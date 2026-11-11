#include "erasegeom.h"
#include <cmath>
#include <algorithm>

static Dim dot2(Point a, Point b) { return a.x*b.x + a.y*b.y; }
static Dim cross2(Point a, Point b) { return a.x*b.y - a.y*b.x; }

static Dim pointSegDist2(Point p, Point s0, Point s1)
{
  Point d = s1 - s0;
  Dim len2 = dot2(d, d);
  Dim t = len2 > 0 ? std::min(Dim(1), std::max(Dim(0), dot2(p - s0, d)/len2)) : 0;
  Point q = s0 + t*d - p;
  return dot2(q, q);
}

Dim segmentDist2(Point a0, Point a1, Point b0, Point b1)
{
  // proper crossing: the four orientation tests have opposite signs pairwise
  Point da = a1 - a0, db = b1 - b0;
  Dim o1 = cross2(da, b0 - a0), o2 = cross2(da, b1 - a0);
  Dim o3 = cross2(db, a0 - b0), o4 = cross2(db, a1 - b0);
  if(((o1 > 0 && o2 < 0) || (o1 < 0 && o2 > 0)) && ((o3 > 0 && o4 < 0) || (o3 < 0 && o4 > 0)))
    return 0;
  // otherwise the closest pair of points has an endpoint of one of the segments
  return std::min(std::min(pointSegDist2(a0, b0, b1), pointSegDist2(a1, b0, b1)),
      std::min(pointSegDist2(b0, a0, a1), pointSegDist2(b1, a0, a1)));
}

// nonzero winding of the closed polygon pts[begin..end) around p
static bool insideSubpath(const Path2D& path, int begin, int end, Point p)
{
  if(end - begin < 3)
    return false;
  int winding = 0;
  for(int ii = begin, prev = end - 1; ii < end; prev = ii++) {
    Point p0 = path.point(prev), p1 = path.point(ii);
    if(p0.y <= p.y) {
      if(p1.y > p.y && cross2(p1 - p0, p - p0) > 0)
        ++winding;
    }
    else if(p1.y <= p.y && cross2(p1 - p0, p - p0) < 0)
      --winding;
  }
  return winding != 0;
}

bool capsuleHitsPath(Point a, Point b, Dim radius, const Path2D& path, Dim halfWidth, bool filled)
{
  const Dim reach = radius + halfWidth;
  const Dim reach2 = reach*reach;
  const int n = path.size();
  // a filled subpath is closed implicitly, so its closing edge is outline too, and inside it is ink
  auto filledHit = [&](int begin, int end) {
    return filled && end - begin > 2 && (segmentDist2(a, b, path.point(end-1), path.point(begin)) < reach2
        || insideSubpath(path, begin, end, a));
  };
  int subpathStart = 0;
  for(int ii = 0; ii < n; ++ii) {
    if(ii > 0 && path.command(ii) == Path2D::MoveTo) {
      if(filledHit(subpathStart, ii))
        return true;
      subpathStart = ii;
    }
    else if(ii > 0 && segmentDist2(a, b, path.point(ii-1), path.point(ii)) < reach2)
      return true;
    // a lone point (a dot) is ink too
    if((ii + 1 == n || path.command(ii + 1) == Path2D::MoveTo) && ii == subpathStart
        && pointSegDist2(path.point(ii), a, b) < reach2)
      return true;
  }
  return filledHit(subpathStart, n);
}

std::vector<Point> capsulePolygon(Point a, Point b, Dim radius, int capSegs)
{
  Point d = b - a;
  Dim len = std::sqrt(dot2(d, d));
  Dim angle0 = len > 0 ? std::atan2(d.y, d.x) : 0;
  std::vector<Point> poly;
  poly.reserve(2*capSegs + 2);
  // cap around b from +90 to -90 degrees relative to the direction a -> b, then around a back again
  for(int cap = 0; cap < 2; ++cap) {
    Point c = cap == 0 ? b : a;
    Dim start = angle0 + M_PI/2 + (cap == 0 ? -M_PI : 0);
    for(int ii = 0; ii <= capSegs; ++ii) {
      Dim t = start + M_PI*ii/capSegs;
      poly.push_back(c + radius*Point(std::cos(t), std::sin(t)));
    }
  }
  return poly;
}

static Dim segLength(Point s0, Point s1) { return std::sqrt(dot2(s1 - s0, s1 - s0)); }

// length of segment s0-s1 inside the polygon: cut it where it crosses the polygon's edges and test the
//  middle of each piece
static Dim segmentLengthInside(Point s0, Point s1, const std::vector<Point>& polygon)
{
  Point dir = s1 - s0;
  Dim len = segLength(s0, s1);
  if(len <= 0)
    return 0;
  std::vector<Dim> cuts = {0, 1};
  for(size_t ii = 0, prev = polygon.size() - 1; ii < polygon.size(); prev = ii++) {
    Point edge = polygon[ii] - polygon[prev];
    Dim denom = cross2(dir, edge);
    if(denom == 0)
      continue;  // parallel: the pieces either side are tested by their middles anyway
    Point offset = polygon[prev] - s0;
    Dim along = cross2(offset, edge)/denom;  // parameter on the segment
    Dim onEdge = cross2(offset, dir)/denom;  // parameter on the polygon's edge
    if(along > 0 && along < 1 && onEdge >= 0 && onEdge <= 1)
      cuts.push_back(along);
  }
  std::sort(cuts.begin(), cuts.end());
  Dim inside = 0;
  for(size_t ii = 1; ii < cuts.size(); ++ii) {
    if(cuts[ii] - cuts[ii-1] <= 0)
      continue;
    if(pointInPolygon(polygon, s0 + (0.5*(cuts[ii-1] + cuts[ii]))*dir))
      inside += (cuts[ii] - cuts[ii-1])*len;
  }
  return inside;
}

void addLengthInside(LengthInside& acc, const Path2D& path, bool closed, const std::vector<Point>& polygon)
{
  if(polygon.size() < 3)
    return;
  auto addSegment = [&](Point s0, Point s1) {
    acc.total += segLength(s0, s1);
    acc.inside += segmentLengthInside(s0, s1, polygon);
  };
  const int count = path.size();
  int subpathStart = 0;
  auto endSubpath = [&](int end) {
    if(closed && end - subpathStart > 2)
      addSegment(path.point(end - 1), path.point(subpathStart));
    Dim length = 0;
    for(int ii = subpathStart + 1; ii < end; ++ii)
      length += segLength(path.point(ii-1), path.point(ii));
    if(end > subpathStart && length <= 0) {
      ++acc.dots;
      if(pointInPolygon(polygon, path.point(subpathStart)))
        ++acc.dotsInside;
    }
  };
  for(int ii = 0; ii < count; ++ii) {
    if(ii > 0 && path.command(ii) == Path2D::MoveTo) {
      endSubpath(ii);
      subpathStart = ii;
    }
    else if(ii > 0)
      addSegment(path.point(ii-1), path.point(ii));
  }
  if(count > 0)
    endSubpath(count);
}
