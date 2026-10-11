// Unit tests for pen-up stroke simplification (syncscribble/strokesimplify.cpp, docs/agent/stroke-input.md).
// No GL context and no document, so they also build and run on their own:
//
//   g++ -std=c++14 -O2 -DNDEBUG -I . -I syncscribble -isystem stb -DSIMPLIFYTEST_MAIN
//       scribbletest/simplifytest.cpp syncscribble/strokesimplify.cpp ulib/geom.cpp ulib/path2d.cpp
//       -o simplifytest && ./simplifytest
//   (one command, run from the repo root)
//
// runSimplifyTests() returns the number of failed checks and is also called from ScribbleTest::runAll().
// What is pinned, for every preset and every geometry (flat outline, stroked polyline, width points):
//  - every dropped point is within the preset's tolerance of the geometry that replaced it, measured
//    independently of the code under test (distance to the kept polyline on its own side)
//  - kept points are original points, in order, with the ends (flat: the cap pairs) kept
//  - on circles of every radius the angle between consecutive kept chords stays under maxTurn: round
//    stays round however small the loop
//  - Off leaves the path bit-identical
// Checked to fail against a broken bound (tolerance tripled in chordBound()).

#include <stdio.h>
#include <string.h>
#include <cmath>
#include <algorithm>

#include "strokesimplify.h"

static int nSimplifyChecksFailed = 0;

static void simplifyCheck(bool ok, const char* what)
{
  if(!ok) {
    ++nSimplifyChecksFailed;
    printf("FAIL: simplify: %s\n", what);
  }
}

// distance from p to the polyline through pts
static double distToPolyline(const std::vector<Point>& pts, const Point& p)
{
  if(pts.size() == 1) return (p - pts[0]).dist();
  double best = 1E30;
  for(size_t ii = 1; ii < pts.size(); ++ii)
    best = std::min(best, distToSegment(pts[ii-1], pts[ii], p));
  return best;
}

// is `sub` a subsequence of `full` (bitwise equal points) that keeps the first and last point?  Fills
//  `idx` with the matching indices into `full`
static bool isOrderedSubset(const std::vector<Point>& full, const std::vector<Point>& sub, std::vector<int>* idx = NULL)
{
  if(sub.empty() || full.empty() || memcmp(&sub.front(), &full.front(), sizeof(Point)) != 0
      || memcmp(&sub.back(), &full.back(), sizeof(Point)) != 0)
    return false;
  size_t jj = 0;
  for(size_t ii = 0; ii < full.size() && jj < sub.size(); ++ii) {
    if(memcmp(&full[ii], &sub[jj], sizeof(Point)) == 0) {
      if(idx) idx->push_back(int(ii));
      ++jj;
    }
  }
  return jj == sub.size();
}

// worst error of every point of `full` against the kept polyline between the kept points around it
static double worstDeviation(const std::vector<Point>& full, const std::vector<int>& keptIdx)
{
  double worst = 0;
  for(size_t k = 1; k < keptIdx.size(); ++k)
    for(int ii = keptIdx[k-1] + 1; ii < keptIdx[k]; ++ii)
      worst = std::max(worst, distToSegment(full[keptIdx[k-1]], full[keptIdx[k]], full[ii]));
  return worst;
}

// largest turn between consecutive chords of a polyline, in radians
static double maxTurnAngle(const std::vector<Point>& pts)
{
  double worst = 0;
  for(size_t ii = 1; ii + 1 < pts.size(); ++ii) {
    Point d0 = pts[ii] - pts[ii-1], d1 = pts[ii+1] - pts[ii];
    if(d0.dist() < 1E-9 || d1.dist() < 1E-9) continue;
    double c = std::min(1.0, std::max(-1.0, dot(d0, d1)/(d0.dist()*d1.dist())));
    worst = std::max(worst, std::acos(c));
  }
  return worst;
}

// a centre line the way the curve fit leaves it: chords flat to `chordTol`
struct TestStroke { const char* name; std::vector<Point> pts; std::vector<double> widths; bool closedCircle; };

static std::vector<TestStroke> makeTestStrokes()
{
  std::vector<TestStroke> strokes;
  const double chordTol = 0.15;
  for(double radius : {1.0, 3.0, 10.0, 50.0, 300.0}) {
    // chord count from the curve fit's sagitta rule, never fewer than the fit's 16-per-segment cap would
    //  give on dense input
    double dtheta = std::min(2*std::sqrt(2*chordTol/radius), 0.3);
    TestStroke s{"circle", {}, {}, true};
    for(double th = 0; th < 2*M_PI - 1E-9; th += dtheta/3) {
      s.pts.emplace_back(400 + radius*std::cos(th), 300 + radius*std::sin(th));
      s.widths.push_back(1.6*(0.7 + 0.3*std::sin(3*th)));
    }
    strokes.push_back(s);
  }
  {
    TestStroke s{"sine", {}, {}, false};
    for(double x = 0; x < 200; x += 0.25) {
      s.pts.emplace_back(100 + x, 200 + 6*std::sin(x/5));
      s.widths.push_back(1.0 + 0.5*std::sin(x/13));
    }
    strokes.push_back(s);
  }
  {
    TestStroke s{"spiral", {}, {}, false};
    for(double th = 0; th < 6*M_PI; th += 0.02) {
      double r = 1 + 2*th;
      s.pts.emplace_back(300 + r*std::cos(th), 300 + r*std::sin(th));
      s.widths.push_back(2.0);
    }
    strokes.push_back(s);
  }
  {
    TestStroke s{"line", {}, {}, false};
    for(int ii = 0; ii <= 100; ++ii) {
      s.pts.emplace_back(10 + ii*0.7, 20 + ii*0.3);
      s.widths.push_back(1.6);
    }
    strokes.push_back(s);
  }
  {
    // lattice noise: a jittery hand-drawn line, deterministic
    TestStroke s{"jitter", {}, {}, false};
    unsigned int seed = 12345;
    for(int ii = 0; ii < 400; ++ii) {
      seed = seed*1103515245u + 12345u;
      double j = ((seed >> 16) & 0x7FFF)/32767.0 - 0.5;
      s.pts.emplace_back(50 + ii*0.5, 80 + 0.08*j + 3*std::sin(ii*0.02));
      s.widths.push_back(1.2 + 0.1*j);
    }
    strokes.push_back(s);
  }
  return strokes;
}

// the flat pen's outline layout: left side, end cap pair, right side reversed (FilledStrokeBuilder)
static Path2D flatOutline(const TestStroke& s)
{
  std::vector<Point> left, right;
  size_t n = s.pts.size();
  for(size_t ii = 0; ii < n; ++ii) {
    Point d = s.pts[std::min(ii + 1, n - 1)] - s.pts[ii > 0 ? ii - 1 : 0];
    Point nrm = Point(-d.y, d.x)*(1/std::max(1E-12, d.dist()));
    left.push_back(s.pts[ii] + nrm*(s.widths[ii]/2));
    right.push_back(s.pts[ii] - nrm*(s.widths[ii]/2));
  }
  Path2D path;
  path.moveTo(left[0]);
  for(size_t ii = 1; ii < n; ++ii) path.lineTo(left[ii]);
  for(size_t ii = n; ii-- > 0;) path.lineTo(right[ii]);
  return path;
}

int runSimplifyTests()
{
  nSimplifyChecksFailed = 0;
  char buf[256];
  std::vector<TestStroke> strokes = makeTestStrokes();
  const char* presetNames[] = {"off", "invisible", "balanced", "aggressive"};

  // Off is a no-op, bit for bit
  for(const TestStroke& s : strokes) {
    Path2D outline = flatOutline(s), before = outline;
    simplifyCheck(!simplifyFlatOutline(outline, strokeSimplifyPreset(SIMPLIFY_OFF))
        && outline.points.size() == before.points.size()
        && memcmp(outline.points.data(), before.points.data(), before.points.size()*sizeof(Point)) == 0,
        "Off changed a flat outline");
    Path2D poly;
    for(const Point& p : s.pts) { if(poly.empty()) poly.moveTo(p); else poly.lineTo(p); }
    Path2D polyBefore = poly;
    simplifyCheck(!simplifyPolyline(poly, strokeSimplifyPreset(SIMPLIFY_OFF))
        && memcmp(poly.points.data(), polyBefore.points.data(), polyBefore.points.size()*sizeof(Point)) == 0,
        "Off changed a polyline");
    simplifyCheck(simplifyWidthPoints(s.pts, s.widths, strokeSimplifyPreset(SIMPLIFY_OFF)).size() == s.pts.size(),
        "Off dropped width points");
  }

  for(int preset = SIMPLIFY_INVISIBLE; preset <= SIMPLIFY_AGGRESSIVE; ++preset) {
    StrokeSimplifyParams params = strokeSimplifyPreset(preset);
    // a hair of floating point slack, nothing more: the bound is the spec
    const double tol = params.tol*(1 + 1E-9) + 1E-12;
    size_t nIn = 0, nOut = 0;
    for(const TestStroke& s : strokes) {
      // stroked polyline
      Path2D poly;
      for(const Point& p : s.pts) { if(poly.empty()) poly.moveTo(p); else poly.lineTo(p); }
      std::vector<Point> orig = poly.points;
      simplifyPolyline(poly, params);
      std::vector<int> idx;
      bool subset = isOrderedSubset(orig, poly.points, &idx);
      snprintf(buf, sizeof(buf), "%s polyline %s: kept points are not an ordered subset with both ends", presetNames[preset], s.name);
      simplifyCheck(subset, buf);
      if(subset) {
        double dev = worstDeviation(orig, idx);
        snprintf(buf, sizeof(buf), "%s polyline %s: deviation %.5f over tolerance %.5f", presetNames[preset], s.name, dev, params.tol);
        simplifyCheck(dev <= tol, buf);
      }
      if(s.closedCircle) {
        double turn = maxTurnAngle(poly.points);
        snprintf(buf, sizeof(buf), "%s polyline %s: chord turn %.1f deg over the %.1f deg cap - faceted",
            presetNames[preset], s.name, turn*180/M_PI, params.maxTurn*180/M_PI);
        simplifyCheck(turn <= params.maxTurn*1.05 + 1E-6, buf);
      }
      nIn += orig.size();
      nOut += poly.points.size();

      // flat outline: each side separately, caps untouched
      Path2D outline = flatOutline(s);
      std::vector<Point> full = outline.points;
      size_t npairs = full.size()/2;
      simplifyFlatOutline(outline, params);
      bool even = outline.size() % 2 == 0 && outline.isSimple();
      snprintf(buf, sizeof(buf), "%s flat %s: outline lost its pairing", presetNames[preset], s.name);
      simplifyCheck(even, buf);
      if(even) {
        size_t kpairs = outline.points.size()/2;
        std::vector<Point> fullL(full.begin(), full.begin() + npairs), fullR(full.rbegin(), full.rbegin() + npairs);
        std::vector<Point> keptL(outline.points.begin(), outline.points.begin() + kpairs),
            keptR(outline.points.rbegin(), outline.points.rbegin() + kpairs);
        std::vector<int> idxL, idxR;
        bool okL = isOrderedSubset(fullL, keptL, &idxL), okR = isOrderedSubset(fullR, keptR, &idxR);
        snprintf(buf, sizeof(buf), "%s flat %s: sides are not ordered subsets keeping the caps", presetNames[preset], s.name);
        simplifyCheck(okL && okR && idxL == idxR, buf);
        if(okL && okR) {
          double dev = std::max(worstDeviation(fullL, idxL), worstDeviation(fullR, idxR));
          snprintf(buf, sizeof(buf), "%s flat %s: outline deviation %.5f over tolerance %.5f", presetNames[preset], s.name, dev, params.tol);
          simplifyCheck(dev <= tol, buf);
          // also as the renderer sees it: every original outline point against the whole new outline
          double worst = 0;
          std::vector<Point> ring = outline.points;
          ring.push_back(ring.front());
          for(const Point& p : full) worst = std::max(worst, distToPolyline(ring, p));
          snprintf(buf, sizeof(buf), "%s flat %s: outline moved %.5f, over tolerance", presetNames[preset], s.name, worst);
          simplifyCheck(worst <= tol, buf);
        }
      }

      // width points (round, chisel): distance + half the width error
      std::vector<int> kept = simplifyWidthPoints(s.pts, s.widths, params);
      bool ends = !kept.empty() && kept.front() == 0 && kept.back() == int(s.pts.size()) - 1
          && std::is_sorted(kept.begin(), kept.end());
      snprintf(buf, sizeof(buf), "%s width %s: ends not kept or not in order", presetNames[preset], s.name);
      simplifyCheck(ends, buf);
      if(ends) {
        double worst = 0;
        for(size_t k = 1; k < kept.size(); ++k) {
          Point a = s.pts[kept[k-1]], b = s.pts[kept[k]];
          double wa = s.widths[kept[k-1]], wb = s.widths[kept[k]];
          for(int ii = kept[k-1] + 1; ii < kept[k]; ++ii) {
            Point d = b - a;
            double len2 = dot(d, d);
            double t = len2 > 0 ? std::min(1.0, std::max(0.0, dot(s.pts[ii] - a, d)/len2)) : 0;
            double err = (s.pts[ii] - (a + t*d)).dist() + std::abs(s.widths[ii] - (wa + t*(wb - wa)))/2;
            worst = std::max(worst, err);
          }
        }
        snprintf(buf, sizeof(buf), "%s width %s: error %.5f over tolerance %.5f", presetNames[preset], s.name, worst, params.tol);
        simplifyCheck(worst <= tol, buf);
      }
    }
    // it has to actually do something, or every check above passes vacuously
    snprintf(buf, sizeof(buf), "%s: kept %d of %d polyline points - simplification did nothing", presetNames[preset], int(nOut), int(nIn));
    simplifyCheck(nOut < nIn*0.7, buf);
    printf("simplify %s: polyline points %d -> %d\n", presetNames[preset], int(nIn), int(nOut));
  }
  // a straight line is two points at any preset
  {
    Path2D line;
    for(int ii = 0; ii <= 50; ++ii) { Point p(ii*1.0, ii*0.5); if(line.empty()) line.moveTo(p); else line.lineTo(p); }
    simplifyPolyline(line, strokeSimplifyPreset(SIMPLIFY_INVISIBLE));
    simplifyCheck(line.size() == 2, "a straight line did not reduce to its two ends");
  }
  return nSimplifyChecksFailed;
}

#ifdef SIMPLIFYTEST_MAIN
int main()
{
  int nfailed = runSimplifyTests();
  printf("%d simplify checks failed\n", nfailed);
  return nfailed ? 1 : 0;
}
#endif
