#include "strokesimplify.h"
#include <cmath>
#include <limits>

// Tolerances were picked by measuring real handwriting (the user's notebooks replayed through the curve
//  fit) and generated strokes, rendering original vs simplified at 1x, 4x and the maximum zoom (11x) - the
//  numbers are in docs/agent/stroke-input.md.  1 document unit is 1 logical pixel at 100% zoom.
//  - Invisible: 0.01 units is 0.11 logical px at 1100% (0.33 device px on a 3x screen): below what
//    antialiasing can show anywhere.
//  - Balanced: invisible at normal zoom, where it is under 0.1 px.
//  - Aggressive: about the curve fit's own chord tolerance; the turn cap keeps curves free of facets.
StrokeSimplifyParams strokeSimplifyPreset(int preset)
{
  StrokeSimplifyParams params;
  switch(preset) {
  case SIMPLIFY_INVISIBLE:  params.tol = 0.01;  params.maxTurn = 6*M_PI/180;  break;
  case SIMPLIFY_BALANCED:   params.tol = 0.04;  params.maxTurn = 10*M_PI/180;  break;
  case SIMPLIFY_AGGRESSIVE: params.tol = 0.10;  params.maxTurn = 12*M_PI/180;  break;
  default: break;  // Off
  }
  return params;
}

// how far over its bound a dropped point is: <= 1 means within it
static double overBound(double err, double bound)
{
  if(err <= 0) return 0;
  return bound > 0 ? err/bound : std::numeric_limits<double>::infinity();
}

static double chordBound(const StrokeSimplifyParams& params, double chordLen)
{
  return std::min(params.tol, chordLen*params.maxTurn/8);
}

// Douglas-Peucker over indices 0..n-1, iterative (a long stroke has thousands of points).  ratio(i, a, b)
//  is how far over its bound point i is when a..b is replaced by one chord.  Returns kept flags.
template<class RatioFn>
static std::vector<char> douglasPeucker(int n, RatioFn ratio)
{
  std::vector<char> keep(n, 0);
  if(n <= 0) return keep;
  keep[0] = keep[n-1] = 1;
  std::vector<std::pair<int, int>> stack;
  stack.emplace_back(0, n - 1);
  while(!stack.empty()) {
    int a = stack.back().first, b = stack.back().second;
    stack.pop_back();
    if(b - a < 2) continue;
    double worst = 1;
    int split = -1;
    for(int i = a + 1; i < b; ++i) {
      double r = ratio(i, a, b);
      if(r > worst) { worst = r; split = i; }
    }
    if(split < 0) continue;  // every point within its bound
    keep[split] = 1;
    stack.emplace_back(a, split);
    stack.emplace_back(split, b);
  }
  return keep;
}

bool simplifyFlatOutline(Path2D& path, const StrokeSimplifyParams& params)
{
  if(!params.enabled() || !path.isSimple()) return false;
  int size = path.size();
  if(size % 2 != 0) return false;
  int npairs = size/2;
  if(npairs < 4) return false;
  const std::vector<Point>& pts = path.points;
  // pair i is (left = pts[i], right = pts[size-1-i]); pairs 0 and npairs-1 are the start and end caps
  auto ratio = [&](int i, int a, int b) {
    const Point &la = pts[a], &lb = pts[b], &ra = pts[size-1-a], &rb = pts[size-1-b];
    double errL = distToSegment(la, lb, pts[i]);
    double errR = distToSegment(ra, rb, pts[size-1-i]);
    return std::max(overBound(errL, chordBound(params, (lb - la).dist())),
        overBound(errR, chordBound(params, (rb - ra).dist())));
  };
  std::vector<char> keep = douglasPeucker(npairs, ratio);
  int nkept = 0;
  for(char k : keep) nkept += k;
  if(nkept == npairs) return false;
  std::vector<Point> out;
  out.reserve(2*nkept);
  for(int i = 0; i < npairs; ++i)
    if(keep[i]) out.push_back(pts[i]);
  for(int i = npairs; i-- > 0;)
    if(keep[i]) out.push_back(pts[size-1-i]);
  path.points.swap(out);
  return true;
}

bool simplifyPolyline(Path2D& path, const StrokeSimplifyParams& params)
{
  if(!params.enabled() || !path.isSimple()) return false;
  int n = path.size();
  if(n < 3) return false;
  const std::vector<Point>& pts = path.points;
  auto ratio = [&](int i, int a, int b) {
    return overBound(distToSegment(pts[a], pts[b], pts[i]), chordBound(params, (pts[b] - pts[a]).dist()));
  };
  std::vector<char> keep = douglasPeucker(n, ratio);
  std::vector<Point> out;
  for(int i = 0; i < n; ++i)
    if(keep[i]) out.push_back(pts[i]);
  if(int(out.size()) == n) return false;
  path.points.swap(out);
  return true;
}

std::vector<int> simplifyWidthPoints(const std::vector<Point>& pts, const std::vector<double>& widths,
    const StrokeSimplifyParams& params)
{
  int n = int(pts.size());
  std::vector<int> kept;
  if(!params.enabled() || n < 3 || int(widths.size()) != n) {
    for(int i = 0; i < n; ++i) kept.push_back(i);
    return kept;
  }
  auto ratio = [&](int i, int a, int b) {
    Point d = pts[b] - pts[a];
    double len2 = d.x*d.x + d.y*d.y;
    double t = len2 > 0 ? std::min(1.0, std::max(0.0, dot(pts[i] - pts[a], d)/len2)) : 0;
    Point onChord = pts[a] + t*d;
    double werr = std::abs(widths[i] - (widths[a] + t*(widths[b] - widths[a])))/2;
    return overBound((pts[i] - onChord).dist() + werr, chordBound(params, std::sqrt(len2)));
  };
  std::vector<char> keep = douglasPeucker(n, ratio);
  for(int i = 0; i < n; ++i)
    if(keep[i]) kept.push_back(i);
  return kept;
}
