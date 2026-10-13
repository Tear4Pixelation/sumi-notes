#include "linestraighten.h"
#include <algorithm>
#include <cmath>

// drawn length of pts[from..to]
static Dim pathLength(const std::vector<Point>& pts, int from, int to)
{
  Dim len = 0;
  for(int ii = from; ii < to; ++ii)
    len += pts[ii].dist(pts[ii+1]);
  return len;
}

// the point of pts(from, to) furthest from the chord pts[from] - pts[to]: its index (-1 if there is no
//  point between them) and, in `distOut`, its distance.  A degenerate chord (a closed loop) measures from
//  the start instead.
static int furthestFromChord(const std::vector<Point>& pts, int from, int to, Dim* distOut)
{
  Point start = pts[from];
  Point chord = pts[to] - start;
  Dim chordLen = chord.dist();
  int furthest = -1;
  Dim furthestDist = 0;
  for(int ii = from + 1; ii < to; ++ii) {
    Dim dist = chordLen > 0 ? std::abs(cross(pts[ii] - start, chord))/chordLen : pts[ii].dist(start);
    if(dist > furthestDist) {
      furthestDist = dist;
      furthest = ii;
    }
  }
  *distOut = furthestDist;
  return furthest;
}

static bool isStraightPiece(const std::vector<Point>& pts, int from, int to, const LineStraightenParams& params)
{
  Dim chordLen = pts[from].dist(pts[to]);
  if(chordLen <= 0)
    return false;
  Dim deviation = 0;
  furthestFromChord(pts, from, to, &deviation);
  return deviation <= params.maxDeviation*chordLen && pathLength(pts, from, to) <= params.maxPathRatio*chordLen;
}

// Ramer-Douglas-Peucker: the indices where pts[from..to] has to be split for every piece to stay within
//  `tol` of its chord, in order.  Gives up (false) as soon as more than maxSplits would be needed, which
//  also bounds the recursion depth - a squiggle never recurses further than that.
static bool splitAtCorners(const std::vector<Point>& pts, int from, int to, Dim tol,
    std::vector<int>& splits, int& nSplits, int maxSplits)
{
  Dim deviation = 0;
  int split = furthestFromChord(pts, from, to, &deviation);
  if(split < 0 || deviation <= tol)
    return true;
  if(++nSplits > maxSplits)
    return false;
  if(!splitAtCorners(pts, from, split, tol, splits, nSplits, maxSplits))
    return false;
  splits.push_back(split);
  return splitAtCorners(pts, split, to, tol, splits, nSplits, maxSplits);
}

// exact unit vectors for multiples of 45 degrees, so a snapped horizontal line has exactly equal y
//  (cos/sin of pi would leave 1E-16 behind)
static Point octantDir(int octant)
{
  static const Dim diag = std::sqrt(Dim(0.5));
  static const Point dirs[8] = { Point(1, 0), Point(diag, diag), Point(0, 1), Point(-diag, diag),
      Point(-1, 0), Point(-diag, -diag), Point(0, -1), Point(diag, -diag) };
  return dirs[((octant % 8) + 8) % 8];
}

StraightenedStroke straightenStroke(const std::vector<Point>& pts, const LineStraightenParams& params)
{
  StraightenedStroke result;
  int npts = int(pts.size());
  if(npts < 2)
    return result;
  Dim total = pathLength(pts, 0, npts - 1);
  if(!(total > 0) || total < params.minLength)
    return result;

  std::vector<int> corners = {0};
  if(!isStraightPiece(pts, 0, npts - 1, params)) {
    // corners are where the stroke strays from a polyline by more than the deviation allowed for a line
    //  as long as the whole stroke; every piece then has to pass on its own below
    std::vector<int> splits;
    int nSplits = 0;
    if(params.maxSegments < 2
        || !splitAtCorners(pts, 0, npts - 1, params.maxDeviation*total, splits, nSplits, params.maxSegments - 1)
        || splits.empty())
      return result;  // too many pieces (a curve or handwriting), or a gentle bow with no corner
    corners.insert(corners.end(), splits.begin(), splits.end());
  }
  corners.push_back(npts - 1);

  const Dim maxCornerCos = std::cos(degToRad(params.minCornerDeg));
  for(size_t ii = 0; ii + 1 < corners.size(); ++ii) {
    if(!isStraightPiece(pts, corners[ii], corners[ii+1], params))
      return result;
    if(pts[corners[ii]].dist(pts[corners[ii+1]]) < params.minSegmentFrac*total)
      return result;
    if(ii > 0) {
      Point before = pts[corners[ii]] - pts[corners[ii-1]];
      Point after = pts[corners[ii+1]] - pts[corners[ii]];
      if(dot(before, after)/(before.dist()*after.dist()) > maxCornerCos)
        return result;  // a bend, not a corner: part of a curve
    }
  }

  std::vector<Point> vertices;
  for(int idx : corners)
    vertices.push_back(pts[idx]);
  if(params.snapAngles) {
    // the start stays where the pen came down; each piece in turn is turned onto the axis or diagonal it
    //  is close to (its far end projected onto it), a piece that is not is just carried along
    std::vector<Point> snapped = {vertices[0]};
    for(size_t ii = 1; ii < vertices.size(); ++ii) {
      Point dir = vertices[ii] - vertices[ii-1];
      Dim deg = std::atan2(dir.y, dir.x)*180/M_PI;
      int octant = int(std::lround(deg/45));
      bool axis = octant % 2 == 0;
      Dim window = axis ? params.axisSnapDeg : params.diagonalSnapDeg;
      Point from = snapped.back();
      if(std::abs(deg - octant*45) <= window) {
        Point unit = octantDir(octant);
        snapped.push_back(from + unit*dot(vertices[ii] - from, unit));
      }
      else
        snapped.push_back(from + dir);
    }
    vertices.swap(snapped);
  }
  result.vertices = vertices;
  result.indices = corners;
  return result;
}

std::vector<Point> projectOntoStraightened(const std::vector<Point>& pts, const StraightenedStroke& line,
    std::vector<int>& kept)
{
  std::vector<Point> moved;
  kept.clear();
  if(line.empty())
    return moved;
  for(size_t seg = 0; seg + 1 < line.vertices.size(); ++seg) {
    int from = line.indices[seg];
    int to = line.indices[seg+1];
    Point start = pts[from];
    Point chord = pts[to] - start;
    Dim chordLen2 = chord.dist2();
    Point newStart = line.vertices[seg];
    Point newChord = line.vertices[seg+1] - newStart;
    moved.push_back(newStart);
    kept.push_back(from);
    Dim prevFrac = 0;
    for(int ii = from + 1; ii < to; ++ii) {
      Dim frac = chordLen2 > 0 ? dot(pts[ii] - start, chord)/chordLen2 : 0;
      if(frac <= prevFrac || frac >= 1)
        continue;
      prevFrac = frac;
      moved.push_back(newStart + newChord*frac);
      kept.push_back(ii);
    }
  }
  moved.push_back(line.vertices.back());
  kept.push_back(line.indices.back());
  return moved;
}
