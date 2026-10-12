#include "cropshape.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <cmath>

static constexpr real PI_REAL = real(3.14159265358979323846);

bool CropShape::isValid() const
{
  if(kind == POLYGON)
    return points.size() >= 3;
  return rect.width() > 0 && rect.height() > 0;
}

bool CropShape::isWholeImage(int w, int h) const
{
  return kind == RECT && radius <= 0 && rect.left <= real(0.5) && rect.top <= real(0.5)
      && rect.right >= w - real(0.5) && rect.bottom >= h - real(0.5);
}

std::vector<Point> CropShape::corners() const
{
  if(kind == POLYGON)
    return points;
  return { Point(rect.left, rect.top), Point(rect.right, rect.top),
      Point(rect.right, rect.bottom), Point(rect.left, rect.bottom) };
}

std::string CropShape::serialize() const
{
  std::string out = kind == POLYGON ? "poly" : "rect";
  char buff[64];
  snprintf(buff, sizeof(buff), " %.2f", double(radius));
  out += buff;
  if(kind == RECT) {
    snprintf(buff, sizeof(buff), " %.2f %.2f %.2f %.2f", double(rect.left), double(rect.top),
        double(rect.right), double(rect.bottom));
    out += buff;
  }
  else {
    for(const Point& pt : points) {
      snprintf(buff, sizeof(buff), " %.2f %.2f", double(pt.x), double(pt.y));
      out += buff;
    }
  }
  return out;
}

bool CropShape::parse(const char* str, CropShape* out)
{
  if(!str || !out)
    return false;
  CropShape shape;
  if(strncmp(str, "rect", 4) == 0)
    shape.kind = RECT;
  else if(strncmp(str, "poly", 4) == 0)
    shape.kind = POLYGON;
  else
    return false;
  const char* pos = str + 4;
  std::vector<real> nums;
  for(;;) {
    char* end = NULL;
    double val = strtod(pos, &end);
    if(end == pos)
      break;
    nums.push_back(real(val));
    pos = end;
  }
  if(nums.empty() || nums[0] < 0)
    return false;
  shape.radius = nums[0];
  if(shape.kind == RECT) {
    if(nums.size() != 5)
      return false;
    shape.rect = Rect::ltrb(nums[1], nums[2], nums[3], nums[4]);
  }
  else {
    if(nums.size() < 7 || (nums.size() - 1) % 2 != 0)
      return false;
    for(size_t ii = 1; ii + 1 < nums.size(); ii += 2)
      shape.points.push_back(Point(nums[ii], nums[ii+1]));
  }
  if(!shape.isValid())
    return false;
  *out = shape;
  return true;
}

std::vector<Point> roundedOutline(const std::vector<Point>& input, real radius, real maxSegLen)
{
  // repeated points (a double tap places two) would make zero-length edges with no direction
  std::vector<Point> corners;
  for(const Point& pt : input) {
    if(corners.empty() || corners.back().dist(pt) > real(1E-6))
      corners.push_back(pt);
  }
  while(corners.size() > 1 && corners.back().dist(corners.front()) <= real(1E-6))
    corners.pop_back();
  if(corners.size() < 3 || radius <= 0)
    return corners;

  maxSegLen = std::max(maxSegLen, real(0.05));
  std::vector<Point> out;
  size_t count = corners.size();
  for(size_t ii = 0; ii < count; ++ii) {
    const Point& vertex = corners[ii];
    Point toPrev = corners[(ii + count - 1) % count] - vertex;
    Point toNext = corners[(ii + 1) % count] - vertex;
    real prevLen = toPrev.dist(), nextLen = toNext.dist();
    Point dirPrev = toPrev/prevLen, dirNext = toNext/nextLen;
    real cosAngle = std::min(real(1), std::max(real(-1), dot(dirPrev, dirNext)));
    real angle = std::acos(cosAngle);  // between the two edges, 0 (spike) to pi (straight)
    if(angle < real(1E-3) || angle > PI_REAL - real(1E-3)) {
      out.push_back(vertex);
      continue;
    }
    real halfTan = std::tan(angle/2);
    // distance from the corner to where the arc meets each edge; never past an edge's middle, so the
    //  fillets of two neighbouring corners cannot overlap
    real tangentDist = std::min(radius/halfTan, std::min(prevLen, nextLen)/2);
    real arcRadius = tangentDist*halfTan;
    Point start = vertex + dirPrev*tangentDist;
    Point end = vertex + dirNext*tangentDist;
    Point bisector = dirPrev + dirNext;
    bisector = bisector/bisector.dist();
    Point center = vertex + bisector*(arcRadius/std::sin(angle/2));
    real startAngle = std::atan2(start.y - center.y, start.x - center.x);
    real sweep = std::atan2(end.y - center.y, end.x - center.x) - startAngle;
    // the short way round: the arc spans pi - angle, always less than half a turn
    while(sweep > PI_REAL) sweep -= 2*PI_REAL;
    while(sweep <= -PI_REAL) sweep += 2*PI_REAL;
    int segments = std::max(1, std::min(256, int(std::ceil(std::abs(sweep)*arcRadius/maxSegLen))));
    for(int step = 0; step <= segments; ++step) {
      real theta = startAngle + sweep*step/segments;
      out.push_back(Point(center.x + arcRadius*std::cos(theta), center.y + arcRadius*std::sin(theta)));
    }
  }
  return out;
}

Rect cropPixelBounds(const std::vector<Point>& outline, int w, int h)
{
  if(outline.empty())
    return Rect::ltrb(0, 0, 0, 0);
  real left = outline[0].x, right = left, top = outline[0].y, bottom = top;
  for(const Point& pt : outline) {
    left = std::min(left, pt.x);  right = std::max(right, pt.x);
    top = std::min(top, pt.y);  bottom = std::max(bottom, pt.y);
  }
  left = std::max(real(0), std::floor(left));
  top = std::max(real(0), std::floor(top));
  right = std::min(real(w), std::ceil(right));
  bottom = std::min(real(h), std::ceil(bottom));
  if(right <= left || bottom <= top)
    return Rect::ltrb(0, 0, 0, 0);
  return Rect::ltrb(left, top, right, bottom);
}

void applyOutlineMask(unsigned char* rgba, int w, int h, const std::vector<Point>& outline, int samples)
{
  if(w <= 0 || h <= 0)
    return;
  samples = std::max(1, samples);
  size_t count = outline.size();
  std::vector<int> coverage(w);
  std::vector<std::pair<real, int>> crossings;  // x, winding direction
  const int fullCoverage = samples*samples;
  for(int row = 0; row < h; ++row) {
    std::fill(coverage.begin(), coverage.end(), 0);
    for(int sub = 0; sub < samples; ++sub) {
      real sampleY = row + (sub + real(0.5))/samples;
      crossings.clear();
      for(size_t ii = 0; ii < count && count >= 3; ++ii) {
        const Point& p0 = outline[ii];
        const Point& p1 = outline[(ii + 1) % count];
        int dir = p0.y <= sampleY && p1.y > sampleY ? 1 : p1.y <= sampleY && p0.y > sampleY ? -1 : 0;
        if(dir)
          crossings.push_back({p0.x + (sampleY - p0.y)*(p1.x - p0.x)/(p1.y - p0.y), dir});
      }
      std::sort(crossings.begin(), crossings.end());
      int winding = 0;
      real spanStart = 0;
      for(const auto& crossing : crossings) {
        int before = winding;
        winding += crossing.second;
        if(before == 0 && winding != 0)
          spanStart = crossing.first;
        else if(before != 0 && winding == 0) {
          // subsample column j sits at (j + 0.5)/samples; count those with spanStart <= x < crossing
          long first = std::max(0L, long(std::ceil(spanStart*samples - real(0.5))));
          long last = std::min(long(w)*samples, long(std::ceil(crossing.first*samples - real(0.5))));
          for(long col = first; col < last; ++col)
            ++coverage[col/samples];
        }
      }
    }
    unsigned char* pixel = rgba + size_t(row)*w*4;
    for(int col = 0; col < w; ++col, pixel += 4) {
      if(coverage[col] == 0)
        pixel[0] = pixel[1] = pixel[2] = pixel[3] = 0;
      else if(coverage[col] < fullCoverage)
        pixel[3] = (unsigned char)((pixel[3]*coverage[col] + fullCoverage/2)/fullCoverage);
    }
  }
}
