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
