#pragma once

// Minimal 2D vector, so the recognizer has no dependency on ulib and still builds standalone in
//  labs/shape-recognition, whose regression suite compiles this very file.  Namespaced because ulib
//  already has a global distToSegment() with a different argument order.

#include <algorithm>
#include <cmath>
#include <vector>

namespace shaperec {

struct Vec2
{
  double x = 0, y = 0;

  Vec2() = default;
  Vec2(double xcoord, double ycoord) : x(xcoord), y(ycoord) {}

  Vec2 operator+(const Vec2& other) const { return Vec2(x + other.x, y + other.y); }
  Vec2 operator-(const Vec2& other) const { return Vec2(x - other.x, y - other.y); }
  Vec2 operator*(double scale) const { return Vec2(x*scale, y*scale); }
  Vec2 operator/(double scale) const { return Vec2(x/scale, y/scale); }
  Vec2& operator+=(const Vec2& other) { x += other.x; y += other.y; return *this; }
  Vec2& operator-=(const Vec2& other) { x -= other.x; y -= other.y; return *this; }

  double dot(const Vec2& other) const { return x*other.x + y*other.y; }
  double cross(const Vec2& other) const { return x*other.y - y*other.x; }
  double length() const { return std::sqrt(x*x + y*y); }
  Vec2 normalized() const { double len = length(); return len > 0 ? Vec2(x/len, y/len) : Vec2(); }
  // rotated +90 degrees
  Vec2 perp() const { return Vec2(-y, x); }
};

inline double dist(const Vec2& from, const Vec2& to) { return (to - from).length(); }

// distance from pt to the segment start-end, and optionally the closest point on it
inline double distToSegment(const Vec2& pt, const Vec2& start, const Vec2& end, Vec2* closest = nullptr)
{
  Vec2 seg = end - start;
  double lenSq = seg.dot(seg);
  double frac = lenSq > 0 ? (pt - start).dot(seg)/lenSq : 0;
  frac = frac < 0 ? 0 : (frac > 1 ? 1 : frac);
  Vec2 proj = start + seg*frac;
  if(closest) *closest = proj;
  return dist(pt, proj);
}

// convex hull (monotone chain), counter-clockwise in a y-up frame
inline std::vector<Vec2> convexHull(std::vector<Vec2> pts)
{
  std::sort(pts.begin(), pts.end(), [](const Vec2& lhs, const Vec2& rhs) {
    return lhs.x < rhs.x || (lhs.x == rhs.x && lhs.y < rhs.y); });
  if(pts.size() < 3)
    return pts;
  std::vector<Vec2> hull(2*pts.size());
  size_t num = 0;
  for(size_t i = 0; i < pts.size(); ++i) {
    while(num >= 2 && (hull[num-1] - hull[num-2]).cross(pts[i] - hull[num-2]) <= 0)
      --num;
    hull[num++] = pts[i];
  }
  for(size_t i = pts.size() - 1, lower = num + 1; i-- > 0;) {
    while(num >= lower && (hull[num-1] - hull[num-2]).cross(pts[i] - hull[num-2]) <= 0)
      --num;
    hull[num++] = pts[i];
  }
  hull.resize(num - 1);
  return hull;
}

}  // namespace shaperec
