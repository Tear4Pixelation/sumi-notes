#include "shaperec.h"

#include <algorithm>
#include <functional>
#include <cmath>
#include <cstdio>

namespace shaperec {

const char* kindName(Kind kind)
{
  switch(kind) {
  case Kind::Line: return "line";
  case Kind::Quad: return "quad";
  case Kind::Ellipse: return "ellipse";
  case Kind::Scribble: return "scribble";
  default: return "none";
  }
}

namespace {

constexpr double PI = 3.14159265358979323846;

double degToRad(double deg) { return deg*PI/180; }

std::string format(const char* fmt, double value1, double value2 = 0)
{
  char buf[256];
  snprintf(buf, sizeof(buf), fmt, value1, value2);
  return buf;
}

std::vector<double> cumulativeLengths(const std::vector<Vec2>& pts)
{
  std::vector<double> cum(pts.size(), 0.0);
  for(size_t i = 1; i < pts.size(); ++i)
    cum[i] = cum[i-1] + dist(pts[i-1], pts[i]);
  return cum;
}

// Resample uniformly in arc length.  Input sampling density follows pen speed - dense where the hand
//  slowed down, which is exactly at corners - so every fit below works on resampled points, or slow
//  corners would outvote fast sides.  For a closed loop the closing chord (end back to start) is
//  resampled too, and `synthetic` marks those points: they fill the gap for the turning-angle scan
//  but were never drawn, so no fit may use them.
std::vector<Vec2> resampleUniform(const std::vector<Vec2>& input, int count, bool closed,
    std::vector<bool>* synthetic = nullptr)
{
  std::vector<Vec2> pts = input;
  if(closed)
    pts.push_back(input.front());
  std::vector<double> cum = cumulativeLengths(pts);
  double total = cum.back();
  double spacing = total/(closed ? count : count - 1);
  std::vector<Vec2> out;
  out.reserve(count);
  if(synthetic)
    synthetic->assign(count, false);
  size_t seg = 1;
  for(int i = 0; i < count; ++i) {
    double pos = std::min(spacing*i, total);
    while(seg < pts.size() - 1 && cum[seg] < pos)
      ++seg;
    double segLen = cum[seg] - cum[seg-1];
    double frac = segLen > 0 ? (pos - cum[seg-1])/segLen : 0;
    out.push_back(pts[seg-1] + (pts[seg] - pts[seg-1])*frac);
    if(synthetic && closed && seg == pts.size() - 1)
      (*synthetic)[i] = true;
  }
  return out;
}

struct LineFit { Vec2 centroid; Vec2 dir; };

// total least squares: the line through the centroid along the covariance's principal axis
LineFit fitLine(const std::vector<Vec2>& pts)
{
  Vec2 centroid;
  for(const Vec2& pt : pts)
    centroid += pt;
  centroid = centroid/double(pts.size());
  double sxx = 0, sxy = 0, syy = 0;
  for(const Vec2& pt : pts) {
    Vec2 offset = pt - centroid;
    sxx += offset.x*offset.x;
    sxy += offset.x*offset.y;
    syy += offset.y*offset.y;
  }
  double angle = 0.5*std::atan2(2*sxy, sxx - syy);
  return LineFit{centroid, Vec2(std::cos(angle), std::sin(angle))};
}

// Gaussian elimination with partial pivoting; matrix is row-major size x size
bool solveLinear(std::vector<double> matrix, std::vector<double> rhs, std::vector<double>& solution)
{
  int size = int(rhs.size());
  double scale = 0;
  for(double value : matrix)
    scale = std::max(scale, std::fabs(value));
  for(int col = 0; col < size; ++col) {
    int pivot = col;
    for(int row = col + 1; row < size; ++row)
      if(std::fabs(matrix[row*size + col]) > std::fabs(matrix[pivot*size + col]))
        pivot = row;
    if(std::fabs(matrix[pivot*size + col]) <= 1e-12*scale)
      return false;
    if(pivot != col) {
      for(int k = 0; k < size; ++k)
        std::swap(matrix[col*size + k], matrix[pivot*size + k]);
      std::swap(rhs[col], rhs[pivot]);
    }
    for(int row = col + 1; row < size; ++row) {
      double factor = matrix[row*size + col]/matrix[col*size + col];
      for(int k = col; k < size; ++k)
        matrix[row*size + k] -= factor*matrix[col*size + k];
      rhs[row] -= factor*rhs[col];
    }
  }
  solution.assign(size, 0);
  for(int row = size - 1; row >= 0; --row) {
    double sum = rhs[row];
    for(int k = row + 1; k < size; ++k)
      sum -= matrix[row*size + k]*solution[k];
    solution[row] = sum/matrix[row*size + row];
  }
  return true;
}

// turning angle between the incoming and outgoing chords around samples[idx], in degrees
double turnAt(const std::vector<Vec2>& samples, int prevIdx, int idx, int nextIdx, double winding = 0)
{
  Vec2 inDir = samples[idx] - samples[prevIdx];
  Vec2 outDir = samples[nextIdx] - samples[idx];
  double cross = inDir.cross(outDir);
  if(winding != 0)
    cross *= winding;
  else
    cross = std::fabs(cross);
  return std::atan2(cross, inDir.dot(outDir))*180/PI;
}

// ---------------------------------------------------------------------------------------------------
// Line.  The end points are the drawn ends, not projections onto a fitted line: when someone draws a
//  line from one box to another, where the pen went down and came up is the whole point, and a least
//  squares line would move both ends sideways by a slightly bowed stroke's sagitta.  What *is* removed
//  is a hook - the little flick as the pen lands or lifts - since nobody perceives that as the end.

bool fitStrokeLine(const std::vector<Vec2>& pts, double arcLen, const Params& params, Result& res)
{
  // ~1% of the length per sample, but never finer than 1 unit, or quantization noise dominates
  double spacing = std::max(arcLen/100, 1.0);
  int count = std::max(3, int(arcLen/spacing) + 1);
  std::vector<Vec2> samples = resampleUniform(pts, count, false);

  // direction from the middle 80%, so a hook cannot tilt it
  int trim = count/10;
  std::vector<Vec2> middle(samples.begin() + trim, samples.end() - trim);
  if(middle.size() < 2)
    middle = samples;
  LineFit fit = fitLine(middle);
  Vec2 dir = fit.dir;
  if((samples.back() - samples.front()).dot(dir) < 0)
    dir = dir*-1;

  int window = std::max(3, int(std::lround(count*0.03)));
  int maxHook = int(params.hookMaxFrac*(count - 1));
  // A hook is cut at its sharpest turn, and only if the piece cut off points away from the line: the
  //  chord from the cut to the stroke's tip must leave the line's direction by hookMinAngleDeg too.
  //  The turn alone is not enough - at slow speed on an integer lattice, a straight stroke's samples
  //  form one-unit staircase steps that turn by 45 degrees, and cutting at one of those took 10-14%
  //  off plain lines.  A distance test (tip at least N units off the line) was tried as well, and any
  //  N large enough to ignore the staircase also ignored real hooks (p90 end error 1.3% -> 2.3%).
  double cosHook = std::cos(degToRad(params.hookMinAngleDeg));
  auto leavesLine = [&](const Vec2& end, const Vec2& cut, double outward) {
    Vec2 offset = end - cut;
    double len = offset.length();
    return len >= 2.0 && offset.dot(dir)*outward < cosHook*len;
  };
  int startIdx = 0, endIdx = count - 1;
  double bestTurn = params.hookMinAngleDeg;
  for(int idx = 2; idx <= maxHook && idx + window < count; ++idx) {
    double turn = turnAt(samples, std::max(0, idx - window), idx, idx + window);
    if(turn > bestTurn) { bestTurn = turn; startIdx = idx; }
  }
  if(startIdx > 0 && !leavesLine(samples.front(), samples[startIdx], -1))
    startIdx = 0;
  bestTurn = params.hookMinAngleDeg;
  for(int idx = count - 3; idx >= count - 1 - maxHook && idx - window >= 0; --idx) {
    double turn = turnAt(samples, idx - window, idx, std::min(count - 1, idx + window));
    if(turn > bestTurn) { bestTurn = turn; endIdx = idx; }
  }
  if(endIdx < count - 1 && !leavesLine(samples.back(), samples[endIdx], 1))
    endIdx = count - 1;
  if(endIdx - startIdx < 2)
    return false;

  std::vector<Vec2> kept(samples.begin() + startIdx, samples.begin() + endIdx + 1);
  fit = fitLine(kept);
  double sumSq = 0, maxDev = 0;
  for(const Vec2& pt : kept) {
    double dev = std::fabs((pt - fit.centroid).cross(fit.dir));
    sumSq += dev*dev;
    maxDev = std::max(maxDev, dev);
  }
  Vec2 start = kept.front(), end = kept.back();
  double length = dist(start, end);
  if(length <= 0)
    return false;
  // arc length over chords a few units long, not over the samples: on whole-unit input a diagonal is
  //  a staircase whose sample-to-sample length is up to sqrt(2) times its real length, which failed
  //  plainly straight mouse lines at 0.73-0.80
  int stride = std::max(1, int(std::max(3.0, length/25)/spacing));
  double keptArc = 0;
  for(int idx = startIdx; idx < endIdx; idx += stride)
    keptArc += dist(samples[idx], samples[std::min(endIdx, idx + stride)]);
  double straightness = length/keptArc;
  res.lineErr = std::sqrt(sumSq/kept.size())/length;
  if(straightness < params.lineMinStraightness) {
    res.reason = format("line: straightness %.2f", straightness);
    return false;
  }
  if(res.lineErr > params.lineMaxRms || maxDev/length > params.lineMaxDev) {
    res.reason = format("line: rms %.3f, max dev %.3f", res.lineErr, maxDev/length);
    return false;
  }
  res.points = {start, end};
  return true;
}

// ---------------------------------------------------------------------------------------------------
// Scratch-out.  Erases whatever lies under it, so every test here is a reason to refuse.  A stroke is a
//  scratch-out only if it goes back and forth along one axis at least scribbleMinReversals times, in
//  passes of similar length that are each nearly straight, turning straight back at every reversal.
//  Handwriting is what it has to be told from: cursive loops fail the straight passes, and m, n, w and
//  v fail the reversals - an arch or a leaning V turns a good fraction of its height away from where it
//  came up, where a scratching hand comes back down on top of itself.

bool fitScribble(const std::vector<Vec2>& pts, double arcLen, const Params& params, Result& res, std::string& why)
{
  double spacing = std::max(arcLen/600, 1.0);
  int count = std::max(3, int(arcLen/spacing) + 1);
  std::vector<Vec2> samples = resampleUniform(pts, count, false);

  // scrub axis: the length-weighted mean direction of short chords, averaged modulo 180 degrees so
  //  passes in both directions agree.  Not the points' principal axis - scratching out a long word
  //  with short vertical strokes spreads the points horizontally.
  int chord = std::max(1, int(3/spacing));
  double sumCos = 0, sumSin = 0;
  for(int i = 0; i + chord < count; i += chord) {
    Vec2 seg = samples[i + chord] - samples[i];
    double angle = 2*std::atan2(seg.y, seg.x);
    sumCos += seg.length()*std::cos(angle);
    sumSin += seg.length()*std::sin(angle);
  }
  double meanAngle = 0.5*std::atan2(sumSin, sumCos);

  // the pass ends along `angle`: extrema of the projection, with hysteresis so jitter is not a reversal
  auto findEnds = [&](double angle, std::vector<double>& proj, std::vector<int>& ends) {
    Vec2 dir(std::cos(angle), std::sin(angle));
    proj.resize(count);
    for(int i = 0; i < count; ++i)
      proj[i] = samples[i].dot(dir);
    std::vector<double> sorted = proj;
    std::sort(sorted.begin(), sorted.end());
    double extent = sorted[size_t(0.95*(count - 1))] - sorted[size_t(0.05*(count - 1))];
    ends.clear();
    if(extent <= 0)
      return;
    double hyst = params.scribbleHysteresis*extent;
    int dirSign = 0, candidate = 0, lowest = 0, highest = 0;
    for(int i = 1; i < count; ++i) {
      if(dirSign == 0) {
        if(proj[i] < proj[lowest]) lowest = i;
        if(proj[i] > proj[highest]) highest = i;
        if(proj[i] - proj[lowest] > hyst) { ends.push_back(lowest); dirSign = 1; candidate = i; }
        else if(proj[highest] - proj[i] > hyst) { ends.push_back(highest); dirSign = -1; candidate = i; }
      }
      else if((proj[i] - proj[candidate])*dirSign > 0)
        candidate = i;
      else if((proj[candidate] - proj[i])*dirSign > hyst) {
        ends.push_back(candidate);
        dirSign = -dirSign;
        candidate = i;
      }
    }
    if(dirSign != 0)
      ends.push_back(candidate);
  };

  // Everything below, along one candidate scrub axis; fills `res` and returns true if it is a scratch-out.
  auto tryAxis = [&](double axisAngle, std::string& axisWhy) {
    std::vector<double> proj;
    std::vector<int> ends;
    findEnds(axisAngle, proj, ends);
    Vec2 axis(std::cos(axisAngle), std::sin(axisAngle)), across = axis.perp();
    int reversals = int(ends.size()) - 2;
    if(reversals < params.scribbleMinReversals) {
      axisWhy = format("scribble: %.0f reversals", std::max(0, reversals));
      return false;
    }

    // interior passes: similar lengths, straight.  Neither test rejects anything in the synthetic set
    //  that the reversal test below does not, so neither is covered by a gate; they stay because a false
    //  scratch-out destroys work, and real handwriting is more varied than the generator.  Requiring
    //  passes to be parallel to the axis was also tried: it rejected no false scratch-out and cost recall.
    std::vector<double> passLen, straightness;
    for(size_t k = 1; k + 2 < ends.size(); ++k) {
      passLen.push_back(std::fabs(proj[ends[k+1]] - proj[ends[k]]));
      double arc = 0;
      for(int i = ends[k]; i < ends[k+1]; i += chord)
        arc += dist(samples[i], samples[std::min(ends[k+1], i + chord)]);
      straightness.push_back(arc > 0 ? dist(samples[ends[k]], samples[ends[k+1]])/arc : 0);
    }
    std::vector<double> sortedLen = passLen, sortedStraight = straightness;
    std::sort(sortedLen.begin(), sortedLen.end());
    std::sort(sortedStraight.begin(), sortedStraight.end());
    double medianLen = sortedLen[sortedLen.size()/2];
    double shortestLen = sortedLen[size_t(params.scribbleOutlierFrac*sortedLen.size())];
    if(shortestLen < params.scribbleMinPassFrac*medianLen) {
      axisWhy = format("scribble: uneven passes (%.2f of median)", shortestLen/medianLen);
      return false;
    }
    if(sortedStraight[sortedStraight.size()/2] < params.scribbleMinPassStraightness
        || sortedStraight.front() < params.scribbleMinWorstStraightness) {
      axisWhy = format("scribble: curved passes (median %.2f, worst %.2f)", sortedStraight[sortedStraight.size()/2], sortedStraight.front());
      return false;
    }

    // Reversals turn straight back: fit a line to the middle of the passes either side of each reversal
    //  and compare where the two lines are at the reversal, across the axis.  Comparing the pass ends
    //  themselves was tried first and let 7% of mmm through - an arch's rounded top brings its two halves
    //  together at the apex even though its sides are an arch-width apart.
    auto passLine = [&](int from, int to) {
      int span = to - from;
      std::vector<Vec2> mid(samples.begin() + from + span/5, samples.begin() + to - span/5 + 1);
      return fitLine(mid);
    };
    auto acrossAt = [&](const LineFit& line, double tip) {
      // where the line crosses the plane proj == tip, as an across coordinate
      double along = line.dir.dot(axis);
      Vec2 pt = std::fabs(along) > 1e-6 ? line.centroid + line.dir*((tip - line.centroid.dot(axis))/along) : line.centroid;
      return pt.dot(across);
    };
    std::vector<double> gaps;
    for(size_t k = 1; k + 1 < ends.size(); ++k) {
      if(ends[k] - ends[k-1] < 5 || ends[k+1] - ends[k] < 5)
        continue;
      double tip = proj[ends[k]];
      LineFit before = passLine(ends[k-1], ends[k]), after = passLine(ends[k], ends[k+1]);
      // across the passes rather than across the axis: on passes leaning far over (a wide zigzag), a
      //  turn rounded a little along the axis puts the lines' crossings 1/cos(lean) further apart across
      //  it.  Passes along the axis - the arches this test is for - are unaffected.
      double lean = 0.5*(std::fabs(before.dir.dot(axis)) + std::fabs(after.dir.dot(axis)));
      double gap = std::fabs(acrossAt(before, tip) - acrossAt(after, tip))*lean;
      gaps.push_back(gap/medianLen);
    }
    std::sort(gaps.begin(), gaps.end(), std::greater<double>());
    double worstGap = gaps.empty() ? 0 : gaps[size_t(params.scribbleOutlierFrac*gaps.size())];
    double maxGap = reversals >= params.scribbleLongReversals ? params.scribbleLongMaxReversalGap : params.scribbleMaxReversalGap;
    if(worstGap > maxGap) {
      axisWhy = format("scribble: reversal turns wide (%.2f of a pass)", worstGap);
      return false;
    }
    res.points = convexHull(samples);
    res.reason = format("scribble: %.0f reversals, turns %.2f (bar outliers)", reversals, worstGap);
    return true;
  };

  if(tryAxis(meanAngle, why))
    return true;
  // The mean chord direction is the scrub axis only while the passes lean less than 45 degrees.  A hand
  //  travelling fast along what it erases (half of a line, say) draws passes leaning further over than
  //  that, the mean then points along the travel, and along the travel the stroke never turns back - so
  //  wide zigzags were never scratch-outs.  When the mean fails, other axes are tried, the ones the
  //  stroke reverses along most often first, and each still has to pass every test above.  An axis
  //  only counts if most of the motion is along it: across a line, its wobble reverses plenty of times,
  //  and picking the axis with the most reversals outright took plain lines for scratch-outs.
  std::vector<std::pair<int, double>> candidates;  // (pass ends, angle)
  for(int step = 0; step < params.scribbleAxisSteps; ++step) {
    double angle = meanAngle + PI*(step + 1)/(params.scribbleAxisSteps + 1);
    std::vector<double> proj;
    std::vector<int> ends;
    findEnds(angle, proj, ends);
    if(int(ends.size()) - 2 < params.scribbleMinReversals)
      continue;
    double along = 0;
    for(int i = 1; i < count; ++i)
      along += std::fabs(proj[i] - proj[i-1]);
    if(along < params.scribbleMinAlongFrac*spacing*(count - 1))
      continue;
    candidates.emplace_back(int(ends.size()), angle);
  }
  std::stable_sort(candidates.begin(), candidates.end(),
      [](const auto& lhs, const auto& rhs) { return lhs.first > rhs.first; });
  for(const auto& candidate : candidates) {
    std::string ignored;
    if(tryAxis(candidate.second, ignored))
      return true;
  }
  return false;
}

// Replace the samples left by holding the pen still at the end with a single point where it rested.
std::vector<Vec2> trimHold(const std::vector<Vec2>& pts, const Params& params)
{
  // walk back while samples stay near the running centre of those already taken: measured from the
  //  last sample alone (itself a jittered one) the walk stopped early and left most of the hold in
  size_t first = pts.size() - 1;
  Vec2 sum = pts.back();
  while(first > 0 && dist(pts[first-1], sum/double(pts.size() - first)) <= params.holdRadius) {
    --first;
    sum += pts[first];
  }
  size_t held = pts.size() - first;
  if(held < size_t(params.holdMinSamples) || first == 0)
    return pts;
  std::vector<double> xs, ys;
  for(size_t i = first; i < pts.size(); ++i) {
    xs.push_back(pts[i].x);
    ys.push_back(pts[i].y);
  }
  std::nth_element(xs.begin(), xs.begin() + held/2, xs.end());
  std::nth_element(ys.begin(), ys.begin() + held/2, ys.end());
  std::vector<Vec2> out(pts.begin(), pts.begin() + first);
  out.emplace_back(xs[held/2], ys[held/2]);
  return out;
}

// ---------------------------------------------------------------------------------------------------
// Closed loop.  A hand-drawn closed shape rarely ends where it started: it stops short, or runs on past
//  the start along the first side.  The loop is cut where the second half of the stroke passes closest
//  to the start point, which handles both - the overlap is discarded, a gap is closed by a chord.

bool findLoop(const std::vector<Vec2>& pts, const std::vector<double>& cum, const Params& params,
    std::vector<Vec2>& loop, double& perimeter, double& gapFrac)
{
  double total = cum.back();
  const Vec2& start = pts.front();
  double bestDist = Result::NO_FIT;
  size_t bestSeg = 0;
  Vec2 bestPt;
  for(size_t i = 1; i < pts.size(); ++i) {
    if(cum[i] < 0.5*total)
      continue;
    Vec2 closest;
    double gap = distToSegment(start, pts[i-1], pts[i], &closest);
    if(gap < bestDist) {
      bestDist = gap;
      bestSeg = i;
      bestPt = closest;
    }
  }
  if(bestSeg == 0)
    return false;
  loop.assign(pts.begin(), pts.begin() + bestSeg);
  if(dist(loop.back(), bestPt) > 1e-9)
    loop.push_back(bestPt);
  perimeter = cum[bestSeg-1] + dist(pts[bestSeg-1], bestPt) + bestDist;
  gapFrac = perimeter > 0 ? bestDist/perimeter : 1;
  return loop.size() >= 4 && gapFrac <= params.maxClosureGap;
}

struct Peak { int index; double turnDeg; };

// How far to turn a shape at `orient` (radians) towards the nearest screen axis: all the way within
//  snapDeg, then less and less up to 2*snapDeg, and not at all beyond - a hand drawing something straight
//  tilts it a few degrees, a shape meant to be rotated is rotated by more, and the ramp keeps a stroke
//  near the threshold from jumping between the two.
double axisSnapCorrection(double orient, double snapDeg)
{
  double tilt = std::remainder(orient, PI/2);
  double snap = degToRad(snapDeg), absTilt = std::fabs(tilt);
  return absTilt <= snap ? tilt : absTilt < 2*snap ? std::copysign(2*snap - absTilt, tilt) : 0;
}

// local maxima of the signed turning angle around the loop, strongest first.  Signed by the loop's
//  winding, so a corner turning the "wrong" way (an overshoot tick, a reflex vertex) is never a peak.
std::vector<Peak> turningPeaks(const std::vector<Vec2>& samples, int window, int suppress,
    std::vector<double>* turnOut = nullptr)
{
  int count = int(samples.size());
  double area = 0;
  for(int i = 0; i < count; ++i)
    area += samples[i].cross(samples[(i+1) % count]);
  double winding = area >= 0 ? 1 : -1;
  std::vector<double> turn(count);
  for(int i = 0; i < count; ++i)
    turn[i] = turnAt(samples, (i - window + count) % count, i, (i + window) % count, winding);

  std::vector<Peak> peaks;
  for(int i = 0; i < count; ++i) {
    if(turn[i] <= 0)
      continue;
    bool isMax = true;
    for(int offset = 1; offset <= suppress && isMax; ++offset) {
      // strict on one side and not the other, so a flat-topped peak is reported exactly once
      if(turn[(i + offset) % count] > turn[i] || turn[(i - offset + count) % count] >= turn[i])
        isMax = false;
    }
    if(isMax)
      peaks.push_back(Peak{i, turn[i]});
  }
  std::sort(peaks.begin(), peaks.end(), [](const Peak& lhs, const Peak& rhs) { return lhs.turnDeg > rhs.turnDeg; });
  if(turnOut)
    turnOut->swap(turn);
  return peaks;
}

// Quad.  The turning peaks only say roughly where the corners are - a drawn corner is rounded, or the
//  pen overshoots and comes back, so the sample at the peak can be well inside or outside the corner the
//  user meant.  Instead each side's line is fitted to its straight middle (sideTrim cut off each end, so
//  the corner's rounding never enters the fit) and the corners are where adjacent lines intersect.
//  That is the corner as it is perceived: where the sides meet.
bool fitQuad(const std::vector<Vec2>& samples, const std::vector<bool>& synthetic,
    const std::vector<Peak>& peaks, double minTurnDeg, double eqRadius, const Params& params, Result& res,
    std::string& why)
{
  std::vector<Peak> strong;
  for(const Peak& peak : peaks)
    if(peak.turnDeg >= minTurnDeg)
      strong.push_back(peak);
  if(strong.size() < 4) {
    why = format("quad: %.0f corners", double(strong.size()));
    return false;
  }
  std::vector<int> cornerIdx;
  for(int i = 0; i < 4; ++i)
    cornerIdx.push_back(strong[i].index);
  std::sort(cornerIdx.begin(), cornerIdx.end());

  // line through the part of `side` between fractions fromFrac and toFrac of its length
  int count = int(samples.size());
  auto fitSidePart = [&](int side, double fromFrac, double toFrac, LineFit& fit) {
    int from = cornerIdx[side], to = cornerIdx[(side + 1) % 4];
    int span = (to - from + count) % count;
    std::vector<Vec2> sidePts;
    for(int step = int(std::lround(span*fromFrac)); step <= int(std::lround(span*toFrac)); ++step) {
      int idx = (from + step) % count;
      if(!synthetic[idx])
        sidePts.push_back(samples[idx]);
    }
    if(sidePts.size() < 3)
      return false;
    fit = fitLine(sidePts);
    // refit without points well off the line: what is left of a generously rounded corner inside
    //  the trimmed range, which would otherwise pull the side towards the corner's centre
    for(int pass = 0; pass < 2; ++pass) {
      double sumSq = 0;
      for(const Vec2& pt : sidePts)
        sumSq += std::pow((pt - fit.centroid).cross(fit.dir), 2);
      double limit = 2.5*std::sqrt(sumSq/sidePts.size()) + 1e-6;
      std::vector<Vec2> inliers;
      for(const Vec2& pt : sidePts)
        if(std::fabs((pt - fit.centroid).cross(fit.dir)) <= limit)
          inliers.push_back(pt);
      if(inliers.size() < 3 || inliers.size() == sidePts.size())
        break;
      sidePts.swap(inliers);
      fit = fitLine(sidePts);
    }
    return true;
  };

  double minSin = std::sin(degToRad(params.minCornerAngleDeg));
  double maxShift = 0.4*(2*PI*eqRadius/4);
  // Fitting each corner from only the half of each side nearest it was tried, to follow a bowed side
  //  into its corner; it was worse at every reach tried (p90 2.2% -> 2.6% at half a side), since the
  //  noise of fitting fewer points outweighs the bow.
  LineFit sides[4];
  for(int side = 0; side < 4; ++side) {
    if(!fitSidePart(side, params.sideTrim, 1 - params.sideTrim, sides[side])) {
      why = "quad: side too short to fit";
      return false;
    }
  }
  if(params.rectangle) {
    // A rectangle: one orientation shared by all four sides - the length-weighted mean of their own
    //  directions, the odd sides turned by 90 degrees and averaged modulo 180 - and each side kept at
    //  the centroid of its own points.  Intersecting four independently fitted lines instead gives a
    //  quad whose every corner inherits the tilt of both hand-drawn sides meeting there.
    double sumCos = 0, sumSin = 0, spans[4];
    for(int side = 0; side < 4; ++side) {
      spans[side] = (cornerIdx[(side + 1) % 4] - cornerIdx[side] + count) % count;
      double angle = std::atan2(sides[side].dir.y, sides[side].dir.x) - (side % 2 ? PI/2 : 0);
      sumCos += spans[side]*std::cos(2*angle);
      sumSin += spans[side]*std::sin(2*angle);
    }
    double orient = 0.5*std::atan2(sumSin, sumCos);
    // Snap towards the screen axes.  Each side stays at the centroid of its own points, so the corners
    //  move only as much as straightening forces them to.
    orient -= axisSnapCorrection(orient, params.snapDeg);
    Vec2 axisU(std::cos(orient), std::sin(orient)), axisV = axisU.perp();
    double cosMaxSkew = std::cos(degToRad(params.rectMaxSideSkewDeg));
    for(int side = 0; side < 4; ++side) {
      Vec2 rectDir = side % 2 ? axisV : axisU;
      if(std::fabs(sides[side].dir.dot(rectDir)) < cosMaxSkew) {
        why = "quad: sides too far from square to be a rectangle";
        return false;
      }
      sides[side].dir = rectDir;
    }
  }
  std::vector<Vec2> corners, raw;
  for(int side = 0; side < 4; ++side) {
    // corner `side` joins the side ending there to the side starting there
    const LineFit& before = sides[(side + 3) % 4];
    const LineFit& after = sides[side];
    double denom = before.dir.cross(after.dir);
    if(std::fabs(denom) < minSin) {
      why = "quad: adjacent sides nearly parallel";
      return false;
    }
    double along = (after.centroid - before.centroid).cross(after.dir)/denom;
    Vec2 corner = before.centroid + before.dir*along;
    Vec2 peakPt = samples[cornerIdx[side]];
    if(dist(corner, peakPt) > maxShift) {
      why = "quad: side lines meet far from the drawn corner";
      return false;
    }
    corners.push_back(corner);
    raw.push_back(peakPt);
  }

  double turnSign = 0;
  for(int i = 0; i < 4; ++i) {
    double cross = (corners[(i+1) % 4] - corners[i]).cross(corners[(i+2) % 4] - corners[(i+1) % 4]);
    if(turnSign == 0)
      turnSign = cross;
    if(cross*turnSign <= 0) {
      why = "quad: not convex";
      return false;
    }
  }

  // Rounded corners count against this residual, and excluding the corner zones from it was tried; it
  //  changed nothing, even with corners rounded 25% of the way along each side - a rounded square
  //  still fits a quad far better than an ellipse.
  double sumSq = 0;
  int used = 0;
  for(int i = 0; i < count; ++i) {
    if(synthetic[i])
      continue;
    double best = Result::NO_FIT;
    for(int side = 0; side < 4; ++side)
      best = std::min(best, distToSegment(samples[i], corners[side], corners[(side + 1) % 4]));
    sumSq += best*best;
    ++used;
  }
  res.quadErr = std::sqrt(sumSq/used)/eqRadius;
  res.points = corners;
  res.rawCorners = raw;
  return true;
}

// ---------------------------------------------------------------------------------------------------
// Ellipse, in the frame of the points' principal axes (by symmetry these are the ellipse's axes), so
//  only an axis-aligned conic has to be fitted: an algebraic least squares start, then alternating
//  refinement - assign each point its parametric angle, refit centre and radius per axis linearly -
//  which approaches the geometric fit that centre accuracy needs.

struct EllipseFit
{
  bool valid = false;
  Vec2 center;
  double radiusX = 0, radiusY = 0, angle = 0, err = Result::NO_FIT;
};

EllipseFit fitEllipse(const std::vector<Vec2>& pts, double eqRadius)
{
  EllipseFit out;
  LineFit axes = fitLine(pts);
  Vec2 axisU = axes.dir, axisV = axisU.perp();
  size_t num = pts.size();
  std::vector<double> us(num), vs(num);
  for(size_t i = 0; i < num; ++i) {
    Vec2 offset = (pts[i] - axes.centroid)/eqRadius;
    us[i] = offset.dot(axisU);
    vs[i] = offset.dot(axisV);
  }

  // A u^2 + C v^2 + D u + E v = 1
  std::vector<double> normal(16, 0), rhs(4, 0), coef;
  for(size_t i = 0; i < num; ++i) {
    double row[4] = {us[i]*us[i], vs[i]*vs[i], us[i], vs[i]};
    for(int j = 0; j < 4; ++j) {
      rhs[j] += row[j];
      for(int k = 0; k < 4; ++k)
        normal[j*4 + k] += row[j]*row[k];
    }
  }
  if(!solveLinear(normal, rhs, coef) || coef[0] <= 0 || coef[1] <= 0)
    return out;
  double centerU = -coef[2]/(2*coef[0]), centerV = -coef[3]/(2*coef[1]);
  double rhsConst = 1 + coef[0]*centerU*centerU + coef[1]*centerV*centerV;
  double radiusU = std::sqrt(rhsConst/coef[0]), radiusV = std::sqrt(rhsConst/coef[1]);

  std::vector<double> phase(num);
  for(int iter = 0; iter < 20; ++iter) {
    for(size_t i = 0; i < num; ++i)
      phase[i] = std::atan2((vs[i] - centerV)/radiusV, (us[i] - centerU)/radiusU);
    // regress u on cos(phase) and v on sin(phase)
    double meanCos = 0, meanSin = 0, meanU = 0, meanV = 0;
    for(size_t i = 0; i < num; ++i) {
      meanCos += std::cos(phase[i]); meanSin += std::sin(phase[i]);
      meanU += us[i]; meanV += vs[i];
    }
    meanCos /= num; meanSin /= num; meanU /= num; meanV /= num;
    double varCos = 0, varSin = 0, covU = 0, covV = 0;
    for(size_t i = 0; i < num; ++i) {
      double dcos = std::cos(phase[i]) - meanCos, dsin = std::sin(phase[i]) - meanSin;
      varCos += dcos*dcos; varSin += dsin*dsin;
      covU += dcos*(us[i] - meanU); covV += dsin*(vs[i] - meanV);
    }
    if(varCos < 1e-9*num || varSin < 1e-9*num)
      return out;
    radiusU = covU/varCos;
    radiusV = covV/varSin;
    centerU = meanU - radiusU*meanCos;
    centerV = meanV - radiusV*meanSin;
    if(std::fabs(radiusU) < 1e-6 || std::fabs(radiusV) < 1e-6)
      return out;
  }

  double sumSq = 0;
  for(size_t i = 0; i < num; ++i) {
    double du = us[i] - (centerU + radiusU*std::cos(phase[i]));
    double dv = vs[i] - (centerV + radiusV*std::sin(phase[i]));
    sumSq += du*du + dv*dv;
  }
  out.valid = true;
  out.err = std::sqrt(sumSq/num);  // already in units of eqRadius
  out.center = axes.centroid + (axisU*centerU + axisV*centerV)*eqRadius;
  out.radiusX = std::fabs(radiusU)*eqRadius;
  out.radiusY = std::fabs(radiusV)*eqRadius;
  out.angle = std::atan2(axisU.y, axisU.x);
  return out;
}

}  // namespace

Result recognize(const std::vector<Vec2>& stroke, const Params& params)
{
  Result res;
  std::vector<Vec2> pts;
  for(const Vec2& pt : stroke)
    if(pts.empty() || dist(pts.back(), pt) > 1e-9)
      pts.push_back(pt);
  pts = trimHold(pts, params);
  if(pts.size() < 3) {
    res.reason = "too few points";
    return res;
  }
  std::vector<double> cum = cumulativeLengths(pts);
  double arcLen = cum.back();

  // scratch-out first: it is the strict one, and a scratch-out also passes for a (bad) closed shape
  Result scribbleRes;
  std::string scribbleWhy;
  if(fitScribble(pts, arcLen, params, scribbleRes, scribbleWhy)) {
    scribbleRes.kind = Kind::Scribble;
    return scribbleRes;
  }

  Result lineRes;
  if(fitStrokeLine(pts, arcLen, params, lineRes)) {
    lineRes.kind = Kind::Line;
    lineRes.reason = format("line: rms %.3f", lineRes.lineErr);
    return lineRes;
  }
  res.lineErr = lineRes.lineErr;

  std::vector<Vec2> loop;
  double perimeter = 0, gapFrac = 1;
  if(!findLoop(pts, cum, params, loop, perimeter, gapFrac)) {
    res.reason = scribbleWhy + "; " + lineRes.reason + format("; open (gap %.2f)", gapFrac);
    return res;
  }
  double eqRadius = perimeter/(2*PI);
  int count = params.resampleCount;
  std::vector<bool> synthetic;
  std::vector<Vec2> samples = resampleUniform(loop, count, true, &synthetic);
  std::vector<Vec2> realPts;
  for(int i = 0; i < count; ++i)
    if(!synthetic[i])
      realPts.push_back(samples[i]);

  int window = std::max(2, count/32);
  std::vector<double> turn;
  std::vector<Peak> peaks = turningPeaks(samples, window, std::max(3, count/12), &turn);

  // A generously rounded corner spreads its turn over more of the loop than the fine window spans, so
  //  none of its peaks reaches cornerMinTurnDeg.  The coarse scale sees it - but it also finds four
  //  "corners" on any circle, so what keeps a circle from becoming a quad here is only that an
  //  ellipse fits it far better.
  Result quadRes;
  std::string quadWhy;
  bool quadOk = fitQuad(samples, synthetic, peaks, params.cornerMinTurnDeg, eqRadius, params, quadRes, quadWhy);
  if(!quadOk) {
    std::vector<Peak> coarse = turningPeaks(samples, std::max(3, count/13), std::max(3, count/12));
    Result coarseRes;
    std::string coarseWhy;
    if(fitQuad(samples, synthetic, coarse, params.coarseCornerMinTurnDeg, eqRadius, params, coarseRes, coarseWhy)) {
      quadOk = true;
      quadRes = coarseRes;
    }
    else
      quadWhy += "; coarse " + coarseWhy;
  }
  res.quadErr = quadRes.quadErr;
  res.rawCorners = quadRes.rawCorners;

  EllipseFit ellipse = fitEllipse(realPts, eqRadius);
  // An ellipse turns steadily all the way round; a polygon, however rounded its corners, has stretches
  //  that barely turn at all.  This is what tells a hexagon from a circle - both fit an ellipse about
  //  equally well.  "Barely" is relative to how much the fitted ellipse itself turns at that point
  //  (its curvature over the window), not to a circle's steady rate, or the long flat sides of an
  //  elongated ellipse count as straight - that rejected 60% of ellipses.  Counting sharp corners
  //  instead was tried first: it let a third of hexagons through, and once this test existed it no
  //  longer rejected anything this one did not.
  int straightCount = 0;
  if(ellipse.valid) {
    Vec2 axisU(std::cos(ellipse.angle), std::sin(ellipse.angle)), axisV = axisU.perp();
    double windowArc = 2*window*perimeter/count;
    for(int i = 0; i < count; ++i) {
      Vec2 offset = samples[i] - ellipse.center;
      double phase = std::atan2(offset.dot(axisV)/ellipse.radiusY, offset.dot(axisU)/ellipse.radiusX);
      double speed = std::hypot(ellipse.radiusX*std::sin(phase), ellipse.radiusY*std::cos(phase));
      double expectedTurn = ellipse.radiusX*ellipse.radiusY/(speed*speed*speed)*windowArc*180/PI;
      if(turn[i] < params.straightTurnFrac*expectedTurn)
        ++straightCount;
    }
  }
  double straightFrac = double(straightCount)/count;
  res.ellipseErr = ellipse.err;
  std::string ellipseWhy;
  bool ellipseOk = ellipse.valid;
  if(!ellipseOk)
    ellipseWhy = "ellipse: fit failed";
  else if(straightFrac > params.ellipseMaxStraightFrac) {
    ellipseOk = false;
    ellipseWhy = format("ellipse: %.0f%% of the loop is straight", straightFrac*100);
  }

  double quadScore = quadOk ? res.quadErr : Result::NO_FIT;
  double ellipseScore = ellipseOk ? res.ellipseErr : Result::NO_FIT;
  double best = std::min(quadScore, ellipseScore);
  if(best > params.closedMaxErr) {
    res.reason = format("closed, but quad err %.3f, ellipse err %.3f", res.quadErr, res.ellipseErr)
        + (quadWhy.empty() ? "" : "; " + quadWhy) + (ellipseWhy.empty() ? "" : "; " + ellipseWhy);
    return res;
  }
  if(quadScore <= ellipseScore) {
    res.kind = Kind::Quad;
    res.points = quadRes.points;
    res.reason = format("quad: err %.3f (ellipse %.3f)", res.quadErr, res.ellipseErr);
    return res;
  }
  res.kind = Kind::Ellipse;
  res.center = ellipse.center;
  res.radiusX = ellipse.radiusX;
  res.radiusY = ellipse.radiusY;
  res.angle = ellipse.angle;
  // Prefer a circle, the way a rectangle prefers to be straight: a hand drawing a circle draws an oval.
  //  At circleMinAspect or rounder it is a circle; between circleEaseAspect and that, the two radii are
  //  pulled part of the way towards their mean, so a stroke near the threshold does not jump.  The
  //  centre is the ellipse's own: a geometric circle refit was tried and moved no centre or radius
  //  error by more than a few hundredths of a percent.
  double aspect = std::min(ellipse.radiusX, ellipse.radiusY)/std::max(ellipse.radiusX, ellipse.radiusY);
  double meanRadius = 0.5*(ellipse.radiusX + ellipse.radiusY);
  if(aspect >= params.circleMinAspect) {
    res.circle = true;
    res.radiusX = res.radiusY = meanRadius;
    res.angle = 0;
  }
  else {
    if(aspect > params.circleEaseAspect) {
      double pull = (aspect - params.circleEaseAspect)/(params.circleMinAspect - params.circleEaseAspect);
      res.radiusX += (meanRadius - res.radiusX)*pull;
      res.radiusY += (meanRadius - res.radiusY)*pull;
    }
    res.angle -= axisSnapCorrection(res.angle, params.snapDeg);
  }
  res.reason = format("ellipse: err %.3f (quad %.3f)", res.ellipseErr, res.quadErr);
  return res;
}

}  // namespace shaperec
