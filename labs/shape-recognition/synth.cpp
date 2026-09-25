#include "synth.h"

#include <algorithm>
#include <cmath>

namespace synth {

namespace {

constexpr double PI = 3.14159265358979323846;
// spacing of the ideal path before the hand model samples it
constexpr double STEP = 0.5;

using Path = std::vector<Vec2>;

struct Rand
{
  std::mt19937& engine;
  double uniform(double lo, double hi) { return std::uniform_real_distribution<double>(lo, hi)(engine); }
  int integer(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(engine); }
  bool chance(double prob) { return uniform(0, 1) < prob; }
  double gaussian(double sigma) { return std::normal_distribution<double>(0, sigma)(engine); }
  double sign() { return chance(0.5) ? 1 : -1; }
};

Vec2 fromAngle(double angle) { return Vec2(std::cos(angle), std::sin(angle)); }

void lineTo(Path& path, const Vec2& to)
{
  Vec2 from = path.back();
  int steps = std::max(1, int(std::ceil(dist(from, to)/STEP)));
  for(int i = 1; i <= steps; ++i)
    path.push_back(from + (to - from)*(double(i)/steps));
}

// a straight segment bowed sideways by `sag` at its middle
void bowTo(Path& path, const Vec2& to, double sag)
{
  Vec2 from = path.back();
  Vec2 side = (to - from).normalized().perp();
  int steps = std::max(1, int(std::ceil(dist(from, to)/STEP)));
  for(int i = 1; i <= steps; ++i) {
    double frac = double(i)/steps;
    path.push_back(from + (to - from)*frac + side*(4*sag*frac*(1 - frac)));
  }
}

void quadTo(Path& path, const Vec2& control, const Vec2& to)
{
  Vec2 from = path.back();
  double approxLen = dist(from, control) + dist(control, to);
  int steps = std::max(2, int(std::ceil(approxLen/STEP)));
  for(int i = 1; i <= steps; ++i) {
    double frac = double(i)/steps, inv = 1 - frac;
    path.push_back(from*(inv*inv) + control*(2*inv*frac) + to*(frac*frac));
  }
}

enum CornerStyle { SHARP, ROUNDED, OVERSHOOT };

// Draw from the current point to `corner` and turn towards `next`, the way a hand does it; afterwards
//  the path sits at the start of the next side.  The perceived corner is `corner` in every style.
void cornerTo(Path& path, const Vec2& corner, const Vec2& next, double size, Rand& rand, std::string& variant)
{
  Vec2 from = path.back();
  double lenIn = dist(from, corner), lenOut = dist(corner, next);
  Vec2 dirIn = (corner - from).normalized(), dirOut = (next - corner).normalized();
  double sag = size*rand.uniform(-0.008, 0.008);
  double roll = rand.uniform(0, 1);
  if(roll < 0.3) {
    bowTo(path, corner, sag);
    variant += "S";
  }
  else if(roll < 0.7) {
    double radius = std::min(size*rand.uniform(0.03, 0.18), 0.3*std::min(lenIn, lenOut));
    bowTo(path, corner - dirIn*radius, sag);
    quadTo(path, corner, corner + dirOut*radius);
    variant += "R";
  }
  else {
    // run past the corner, then cut back onto the next side - the little "fish tail"
    double overshoot = std::min(size*rand.uniform(0.02, 0.06), 0.3*lenOut);
    bowTo(path, corner + dirIn*overshoot, sag);
    lineTo(path, corner + dirOut*overshoot);
    variant += "O";
  }
}

// A polygon as a hand draws it.  Closed: starts at a corner or part way along the first side, and
//  finishes short of the start or runs on past it.  Open: the ends are exact, only interior corners
//  get a style.
Path polygonPath(const std::vector<Vec2>& corners, bool closed, double size, Rand& rand, std::string& variant)
{
  size_t num = corners.size();
  Path path;
  if(!closed) {
    path.push_back(corners[0]);
    for(size_t i = 1; i + 1 < num; ++i)
      cornerTo(path, corners[i], corners[i+1], size, rand, variant);
    bowTo(path, corners[num-1], size*rand.uniform(-0.008, 0.008));
    return path;
  }
  double startFrac = rand.chance(0.5) ? 0 : rand.uniform(0.2, 0.8);
  Vec2 start = corners[0] + (corners[1] - corners[0])*startFrac;
  path.push_back(start);
  variant += startFrac == 0 ? "start@corner " : "start@side ";
  for(size_t i = 1; i <= num; ++i) {
    const Vec2& corner = corners[i % num];
    if(i == num && startFrac == 0)
      break;
    cornerTo(path, corner, corners[(i+1) % num], size, rand, variant);
  }
  Vec2 firstDir = (corners[1] - corners[0]).normalized();
  if(startFrac > 0) {
    // back along the first side to (around) the start: negative stops short, positive runs past
    double sideLen = dist(corners[0], corners[1]);
    double endAt = std::clamp(startFrac*sideLen + size*rand.uniform(-0.12, 0.25), 0.15*sideLen, 0.95*sideLen);
    bowTo(path, corners[0] + firstDir*endAt, size*rand.uniform(-0.008, 0.008));
    variant += endAt < startFrac*sideLen ? " gap" : " overlap";
  }
  else {
    Vec2 lastDir = (corners[0] - corners[num-1]).normalized();
    if(rand.chance(0.3)) {
      // round the start corner and continue a little way along the first side
      bowTo(path, corners[0], size*rand.uniform(-0.008, 0.008));
      lineTo(path, corners[0] + firstDir*(size*rand.uniform(0.05, 0.25)));
      variant += " overlap";
    }
    else {
      double endAt = size*rand.uniform(-0.1, 0.05);
      bowTo(path, corners[0] + lastDir*endAt, size*rand.uniform(-0.008, 0.008));
      variant += endAt < 0 ? " gap" : " overshoot";
    }
  }
  return path;
}

// hook: the pen lands off the line and flicks into it (or out of it at the end)
Path hookInto(const Vec2& end, const Vec2& dir, double size, Rand& rand, std::string& variant)
{
  double hookLen = size*rand.uniform(0.015, 0.04);
  Vec2 hookStart = end + dir.perp()*(rand.sign()*hookLen) + dir*(rand.uniform(-0.5, 0.3)*hookLen);
  Path hook{hookStart};
  if(rand.chance(0.5)) {
    quadTo(hook, end - dir*(0.5*hookLen), end);
    variant += "curved-hook ";
  }
  else {
    lineTo(hook, end);
    variant += "sharp-hook ";
  }
  return hook;
}

struct HandStyle
{
  double wobble;    // low-frequency sideways drift, units
  double jitter;    // per-sample noise, units
  double maxSpeed;  // units per sample at full speed
  bool quantize;    // round samples to whole units
};

HandStyle randomStyle(double size, Rand& rand)
{
  return HandStyle{size*rand.uniform(0.002, 0.006), rand.uniform(0.1, 0.4), rand.uniform(1.5, 8),
      rand.chance(0.5)};
}

std::vector<double> cumulative(const Path& path)
{
  std::vector<double> cum(path.size(), 0.0);
  for(size_t i = 1; i < path.size(); ++i)
    cum[i] = cum[i-1] + dist(path[i-1], path[i]);
  return cum;
}

// The hand model: displace the ideal path, then sample it in time with a speed that drops in curves.
Path drawPath(const Path& ideal, double size, const HandStyle& style, Rand& rand, std::string& variant)
{
  int num = int(ideal.size());
  std::vector<double> cum = cumulative(ideal);
  double total = cum.back();

  // sideways wobble: three slow sinusoids along the arc length, normal from a smoothed tangent
  double wavelength[3], phase[3], amp[3];
  for(int k = 0; k < 3; ++k) {
    wavelength[k] = size*rand.uniform(0.4, 1.5);
    phase[k] = rand.uniform(0, 2*PI);
    amp[k] = style.wobble*rand.uniform(0.2, 1.0);
  }
  int smooth = std::max(2, int(0.02*size/STEP));
  Path displaced(num);
  for(int i = 0; i < num; ++i) {
    Vec2 tangent = ideal[std::min(num - 1, i + smooth)] - ideal[std::max(0, i - smooth)];
    double offset = 0;
    for(int k = 0; k < 3; ++k)
      offset += amp[k]*std::sin(2*PI*cum[i]/wavelength[k] + phase[k]);
    displaced[i] = ideal[i] + tangent.normalized().perp()*offset;
  }

  // speed: slower where the path turns, and ramping up/down at the ends
  int turnWin = 6;
  std::vector<double> speed(num);
  double ramp = std::max(4.0, 0.05*total);
  for(int i = 0; i < num; ++i) {
    Vec2 inDir = ideal[i] - ideal[std::max(0, i - turnWin)];
    Vec2 outDir = ideal[std::min(num - 1, i + turnWin)] - ideal[i];
    double turn = (inDir.length() > 0 && outDir.length() > 0)
        ? std::fabs(std::atan2(inDir.cross(outDir), inDir.dot(outDir))) : 0;
    double rampFactor = std::min({1.0, 0.3 + 0.7*cum[i]/ramp, 0.3 + 0.7*(total - cum[i])/ramp});
    speed[i] = style.maxSpeed*(0.15 + 0.85/(1 + 6*turn))*rampFactor;
  }

  std::vector<double> dcum = cumulative(displaced);
  double dtotal = dcum.back();
  Path out;
  size_t seg = 1;
  double pos = 0;
  while(true) {
    pos = std::min(pos, dtotal);
    while(seg < displaced.size() - 1 && dcum[seg] < pos)
      ++seg;
    double segLen = dcum[seg] - dcum[seg-1];
    double frac = segLen > 0 ? (pos - dcum[seg-1])/segLen : 0;
    Vec2 pt = displaced[seg-1] + (displaced[seg] - displaced[seg-1])*frac;
    pt += Vec2(rand.gaussian(style.jitter), rand.gaussian(style.jitter));
    if(style.quantize)
      pt = Vec2(std::round(pt.x), std::round(pt.y));
    out.push_back(pt);
    if(pos >= dtotal)
      break;
    pos += std::max(0.3, speed[seg]);
  }
  char buf[128];
  snprintf(buf, sizeof(buf), " | wobble %.1f jitter %.2f speed %.1f%s", style.wobble, style.jitter,
      style.maxSpeed, style.quantize ? " int" : "");
  variant += buf;
  return out;
}

Vec2 randomCenter(Rand& rand) { return Vec2(rand.uniform(300, 700), rand.uniform(300, 700)); }

TestStroke makeLine(Rand& rand)
{
  TestStroke stroke;
  double size = rand.uniform(50, 450);
  Vec2 dir = fromAngle(rand.uniform(0, 2*PI));
  Vec2 center = randomCenter(rand);
  Vec2 start = center - dir*(size/2), end = center + dir*(size/2);
  Path path = rand.chance(0.3) ? hookInto(start, dir, size, rand, stroke.variant) : Path{start};
  bowTo(path, end, size*rand.uniform(-0.012, 0.012));
  if(rand.chance(0.3)) {
    Path hook = hookInto(end, dir*-1, size, rand, stroke.variant);
    path.insert(path.end(), hook.rbegin() + 1, hook.rend());
  }
  stroke.points = drawPath(path, size, randomStyle(size, rand), rand, stroke.variant);
  stroke.truth = {start.x, start.y, end.x, end.y};
  return stroke;
}

TestStroke makeSquare(Rand& rand)
{
  TestStroke stroke;
  double width = rand.uniform(50, 350);
  double height = rand.chance(0.5) ? width : width*rand.uniform(0.6, 1.0);
  if(rand.chance(0.5))
    std::swap(width, height);
  // mostly meant straight; the rest deliberately rotated, which means clearly rotated
  double rotation = rand.chance(0.7) ? 0 : rand.sign()*rand.uniform(25, 65)*PI/180;
  Vec2 center = randomCenter(rand);
  Vec2 axisU = fromAngle(rotation), axisV = axisU.perp();
  double size = std::max(width, height);
  std::vector<Vec2> corners;
  const double signs[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
  for(const auto& sign : signs)
    corners.push_back(center + axisU*(sign[0]*width/2) + axisV*(sign[1]*height/2));
  std::rotate(corners.begin(), corners.begin() + rand.integer(0, 3), corners.end());
  if(rand.chance(0.5))
    std::reverse(corners.begin(), corners.end());
  // the truth is the rectangle meant; the hand misses each of its corners a little, so the drawn quad
  //  is never quite rectangular
  //  - and tilts the whole thing a few degrees, which is what a straight rectangle must be snapped back
  //  from.  On a rectangle meant to be rotated the same tilt cannot be told from intent, so there it is
  //  part of the truth (below) rather than an error to undo.
  double handTilt = rand.gaussian(3*PI/180);
  if(rotation != 0) {
    for(Vec2& corner : corners) {
      Vec2 offset = corner - center;
      corner = center + Vec2(offset.x*std::cos(handTilt) - offset.y*std::sin(handTilt),
          offset.x*std::sin(handTilt) + offset.y*std::cos(handTilt));
    }
    handTilt = 0;
  }
  std::vector<Vec2> drawn;
  for(const Vec2& corner : corners) {
    Vec2 offset = corner - center;
    Vec2 tilted = center + Vec2(offset.x*std::cos(handTilt) - offset.y*std::sin(handTilt),
        offset.x*std::sin(handTilt) + offset.y*std::cos(handTilt));
    drawn.push_back(tilted + Vec2(rand.gaussian(0.02*size), rand.gaussian(0.02*size)));
  }
  Path path = polygonPath(drawn, true, size, rand, stroke.variant);
  stroke.points = drawPath(path, size, randomStyle(size, rand), rand, stroke.variant);
  for(const Vec2& corner : corners) {
    stroke.truth.push_back(corner.x);
    stroke.truth.push_back(corner.y);
  }
  return stroke;
}

TestStroke makeCircle(Rand& rand)
{
  TestStroke stroke;
  double radius = rand.uniform(25, 200);
  Vec2 center = randomCenter(rand);
  double startAngle = rand.uniform(0, 2*PI), dirSign = rand.sign();
  double sweep = rand.uniform(330, 410)*PI/180;
  // a circle drawn by hand comes out oval: up to 0.82 minor/major here, which must still become a circle
  double ecc = rand.uniform(0, 0.1), eccAngle = rand.uniform(0, PI);
  double spiral = rand.uniform(-0.03, 0.03);
  double harmAmp[3], harmPhase[3];
  for(int k = 0; k < 3; ++k) {
    harmAmp[k] = rand.uniform(0, 0.006);
    harmPhase[k] = rand.uniform(0, 2*PI);
  }
  int steps = int(std::ceil(sweep*radius/STEP));
  Path path;
  for(int i = 0; i <= steps; ++i) {
    double rel = sweep*i/steps;
    double angle = startAngle + dirSign*rel;
    // every term is zero-mean around the loop, so the true circle stays the best fit
    double scale = 1 + ecc*std::cos(2*(angle - eccAngle)) + spiral*(rel/sweep - 0.5);
    for(int k = 0; k < 3; ++k)
      scale += harmAmp[k]*std::cos((k + 3)*angle + harmPhase[k]);
    path.push_back(center + fromAngle(angle)*(radius*scale));
  }
  char buf[96];
  snprintf(buf, sizeof(buf), "sweep %.0f ecc %.3f spiral %+.3f", sweep*180/PI, ecc, spiral);
  stroke.variant = buf;
  stroke.points = drawPath(path, 2*radius, randomStyle(2*radius, rand), rand, stroke.variant);
  stroke.truth = {center.x, center.y, radius};
  return stroke;
}

// An ellipse meant as one - clearly oval, 0.3 to 0.6 minor/major - mostly straight, drawn with the same
//  few degrees of hand tilt as a rectangle; the rest deliberately rotated, where the tilt is part of the
//  truth.  Truth: centre x, centre y, radiusX, radiusY, angle of radiusX in radians.
TestStroke makeEllipse(Rand& rand)
{
  TestStroke stroke;
  double radiusX = rand.uniform(40, 200), radiusY = radiusX*rand.uniform(0.3, 0.6);
  if(rand.chance(0.5))
    std::swap(radiusX, radiusY);
  Vec2 center = randomCenter(rand);
  double handTilt = rand.gaussian(3*PI/180);
  double angle = rand.chance(0.7) ? 0 : rand.sign()*rand.uniform(25, 65)*PI/180;
  double drawnAngle = angle + handTilt;
  if(angle != 0)
    angle = drawnAngle;
  double startAngle = rand.uniform(0, 2*PI), dirSign = rand.sign();
  double sweep = rand.uniform(330, 410)*PI/180;
  double spiral = rand.uniform(-0.03, 0.03);
  double harmAmp[3], harmPhase[3];
  for(int k = 0; k < 3; ++k) {
    harmAmp[k] = rand.uniform(0, 0.006);
    harmPhase[k] = rand.uniform(0, 2*PI);
  }
  Vec2 axisU = fromAngle(drawnAngle), axisV = axisU.perp();
  int steps = int(std::ceil(sweep*std::max(radiusX, radiusY)/STEP));
  Path path;
  for(int i = 0; i <= steps; ++i) {
    double rel = sweep*i/steps;
    double param = startAngle + dirSign*rel;
    double scale = 1 + spiral*(rel/sweep - 0.5);
    for(int k = 0; k < 3; ++k)
      scale += harmAmp[k]*std::cos((k + 3)*param + harmPhase[k]);
    path.push_back(center + (axisU*(radiusX*std::cos(param)) + axisV*(radiusY*std::sin(param)))*scale);
  }
  char buf[96];
  snprintf(buf, sizeof(buf), "aspect %.2f %s tilt %+.1f", std::min(radiusX, radiusY)/std::max(radiusX, radiusY),
      angle == 0 ? "straight" : "rotated", handTilt*180/PI);
  stroke.variant = buf;
  double size = 2*std::max(radiusX, radiusY);
  stroke.points = drawPath(path, size, randomStyle(size, rand), rand, stroke.variant);
  stroke.truth = {center.x, center.y, radiusX, radiusY, angle};
  return stroke;
}

std::vector<Vec2> regularPolygon(const Vec2& center, double radius, int sides, double rotation, double innerRatio = 0)
{
  std::vector<Vec2> pts;
  int count = innerRatio > 0 ? 2*sides : sides;
  for(int i = 0; i < count; ++i) {
    double scale = (innerRatio > 0 && i % 2) ? innerRatio : 1;
    pts.push_back(center + fromAngle(rotation + 2*PI*i/count)*(radius*scale));
  }
  return pts;
}

// A scratch-out: back and forth over an area to erase it.  The truth is the area the passes span, as a
//  rotated rectangle.  Three ways people do it: short strokes zigzagging across a word, long strokes
//  stacked down over a block, and rubbing back and forth in place.
TestStroke makeScribble(Rand& rand)
{
  TestStroke stroke;
  int mode = rand.integer(0, 2);
  double passLen, advance;
  int passes;
  if(mode == 0) {
    passLen = rand.uniform(25, 120);
    advance = passLen*rand.uniform(0.08, 0.35);
    passes = std::clamp(int(rand.uniform(60, 400)/advance), 4, 20);
    stroke.variant = "zigzag-across: ";
  }
  else if(mode == 1) {
    passLen = rand.uniform(60, 300);
    passes = rand.integer(4, 10);
    advance = passLen*rand.uniform(0.03, 0.12);
    stroke.variant = "stacked: ";
  }
  else {
    passLen = rand.uniform(40, 200);
    passes = rand.integer(4, 12);
    advance = rand.uniform(0, 3);
    stroke.variant = "in-place: ";
  }
  double rotation = rand.chance(0.7) ? rand.uniform(-0.25, 0.25) : rand.uniform(0, PI);
  Vec2 along = fromAngle(rotation), across = along.perp();
  Vec2 origin = randomCenter(rand) - along*(passLen/2) - across*(advance*(passes - 1)/2);
  std::vector<Vec2> ends;
  for(int i = 0; i <= passes; ++i) {
    // each end lands near, not on, the edge of the area - a scratching hand is not careful
    double reach = (i % 2 ? passLen : 0) + passLen*rand.uniform(-0.15, 0.15);
    ends.push_back(origin + along*reach + across*(advance*std::max(0, i - 1) + rand.gaussian(0.02*passLen)));
  }
  Path path{ends[0]};
  for(size_t i = 1; i + 1 < ends.size(); ++i) {
    Vec2 dirIn = (ends[i] - path.back()).normalized(), dirOut = (ends[i+1] - ends[i]).normalized();
    if(rand.chance(0.6))
      bowTo(path, ends[i], passLen*rand.uniform(-0.02, 0.02));
    else {
      double radius = passLen*rand.uniform(0.02, 0.15);
      bowTo(path, ends[i] - dirIn*radius, passLen*rand.uniform(-0.02, 0.02));
      quadTo(path, ends[i], ends[i] + dirOut*radius);
    }
  }
  bowTo(path, ends.back(), passLen*rand.uniform(-0.02, 0.02));
  double size = std::max(passLen, advance*(passes - 1));
  HandStyle style = randomStyle(size, rand);
  style.maxSpeed = rand.uniform(4, 12);  // scratching is fast
  stroke.points = drawPath(path, size, style, rand, stroke.variant);
  // the truth is the area the pen actually turned around - its outline, not an idealized rectangle,
  //  since each end lands a little short of or past the edge
  for(const Vec2& corner : convexHull(ends)) {
    stroke.truth.push_back(corner.x);
    stroke.truth.push_back(corner.y);
  }
  return stroke;
}

// Things that are none of the above, weighted towards near misses: a triangle, an arc that does not
//  close, three sides of a box, a spiral - and, for the scratch-out, the strokes of handwriting that go
//  back and forth: cursive loops, m and n arches, short zigzags.
TestStroke makeOther(Rand& rand)
{
  TestStroke stroke;
  double size = rand.uniform(60, 300);
  Vec2 center = randomCenter(rand);
  Path path;
  static const char* kinds[] = {"wander", "zigzag", "spiral", "cursive", "triangle", "arc", "u-shape",
      "l-shape", "star", "pentagon", "figure8", "arches", "chevron", "hexagon", "retrace"};
  int kind = rand.integer(0, 14);
  stroke.variant = std::string(kinds[kind]) + ": ";
  switch(kind) {
  case 0: {  // smooth random wander
    double freq[2][3], phase[2][3];
    for(int axis = 0; axis < 2; ++axis)
      for(int k = 0; k < 3; ++k) {
        freq[axis][k] = rand.uniform(0.5, 3.0);
        phase[axis][k] = rand.uniform(0, 2*PI);
      }
    for(int i = 0; i <= 2000; ++i) {
      double param = i/2000.0;
      Vec2 pt = center;
      for(int k = 0; k < 3; ++k) {
        pt.x += size/(2*(k + 1))*std::sin(2*PI*freq[0][k]*param + phase[0][k]);
        pt.y += size/(2*(k + 1))*std::sin(2*PI*freq[1][k]*param + phase[1][k]);
      }
      path.push_back(pt);
    }
    break;
  }
  case 1: {  // zigzag too short to be a scratch-out: N, Z (a held W or M is taken for one)
    int teeth = rand.integer(2, 3);
    Vec2 dir = fromAngle(rand.uniform(0, 2*PI));
    std::vector<Vec2> pts;
    for(int i = 0; i <= teeth; ++i)
      pts.push_back(center + dir*(size*(double(i)/teeth - 0.5)) + dir.perp()*((i % 2 ? 0.25 : -0.25)*size));
    path = polygonPath(pts, false, size, rand, stroke.variant);
    break;
  }
  case 2: {  // spiral, growing at least 50% per turn
    double turns = rand.uniform(1.5, 3), growth = rand.uniform(0.5, 1.2), start = rand.uniform(0, 2*PI);
    double baseRadius = size/(2*(1 + growth*turns));
    int steps = 3000;
    for(int i = 0; i <= steps; ++i) {
      double rel = turns*2*PI*i/steps;
      path.push_back(center + fromAngle(start + rel)*(baseRadius*(1 + growth*rel/(2*PI))));
    }
    break;
  }
  case 3: {  // cursive loops along a baseline (a trochoid)
    int loops = rand.integer(3, 6);
    double pitch = size/loops, loopSize = pitch*rand.uniform(0.6, 1.0);
    for(int i = 0; i <= 3000; ++i) {
      double angle = 2*PI*loops*i/3000.0;
      path.push_back(center + Vec2(pitch*angle/(2*PI) - size/2 - loopSize*std::sin(angle), -loopSize*std::cos(angle)));
    }
    break;
  }
  case 4:
    path = polygonPath(regularPolygon(center, size/2, 3, rand.uniform(0, 2*PI)), true, size, rand, stroke.variant);
    break;
  case 5: {  // arc that does not close
    double sweep = rand.uniform(150, 270)*PI/180, start = rand.uniform(0, 2*PI);
    for(int i = 0; i <= 1000; ++i)
      path.push_back(center + fromAngle(start + sweep*i/1000)*(size/2));
    break;
  }
  case 6: {  // three sides of a box
    std::vector<Vec2> box = regularPolygon(center, size/2, 4, PI/4 + rand.uniform(-0.1, 0.1));
    path = polygonPath({box[0], box[1], box[2], box[3]}, false, size, rand, stroke.variant);
    break;
  }
  case 7: {  // L
    std::vector<Vec2> box = regularPolygon(center, size/2, 4, PI/4 + rand.uniform(-0.1, 0.1));
    path = polygonPath({box[0], box[1], box[2]}, false, size, rand, stroke.variant);
    break;
  }
  case 8:
    path = polygonPath(regularPolygon(center, size/2, 5, rand.uniform(0, 2*PI), 0.4), true, size, rand, stroke.variant);
    break;
  case 9:
    path = polygonPath(regularPolygon(center, size/2, 5, rand.uniform(0, 2*PI)), true, size, rand, stroke.variant);
    break;
  case 10:  // figure eight
    for(int i = 0; i <= 2000; ++i) {
      double angle = 2*PI*i/2000.0;
      path.push_back(center + Vec2(size/2*std::sin(angle), size/4*std::sin(2*angle)));
    }
    break;
  case 11: {  // mmm / nnn: straight up and down strokes joined by arches at the top
    int letters = rand.integer(3, 6), humps = letters*rand.integer(1, 2);
    double height = size/2, archWidth = height*rand.uniform(0.35, 0.7);
    Vec2 pos = center - Vec2(archWidth*humps/2, -height/2);
    path.push_back(pos);
    for(int i = 0; i < humps; ++i) {
      lineTo(path, pos - Vec2(0, height));
      Vec2 top = pos - Vec2(0, height);
      int steps = 60;
      for(int step = 1; step <= steps; ++step) {
        double angle = PI - PI*step/steps;
        path.push_back(top + Vec2(archWidth/2 + archWidth/2*std::cos(angle), -archWidth/2*std::sin(angle)));
      }
      pos = pos + Vec2(archWidth, 0);
      lineTo(path, pos);
    }
    break;
  }
  case 14: {  // out and straight back, slightly offset - underlining twice
    Vec2 dir = fromAngle(rand.uniform(0, 2*PI));
    Vec2 start = center - dir*(size/2);
    path = polygonPath({start, center + dir*(size/2), start + dir.perp()*(size*rand.uniform(0.01, 0.06))},
        false, size, rand, stroke.variant);
    break;
  }
  case 13:
    // the near miss the ellipse's vetoes exist for: a hexagon fits an ellipse about as well as a
    //  hand-drawn circle does, so only its corners and straight stretches give it away
    path = polygonPath(regularPolygon(center, size/2, 6, rand.uniform(0, 2*PI)), true, size, rand, stroke.variant);
    break;
  default: {  // chevron / check mark
    Vec2 dir = fromAngle(rand.uniform(0, 2*PI));
    path = polygonPath({center - dir*(size/2) + dir.perp()*(size/3), center, center + dir*(size/2) + dir.perp()*(size/2)},
        false, size, rand, stroke.variant);
    break;
  }
  }
  stroke.points = drawPath(path, size, randomStyle(size, rand), rand, stroke.variant);
  return stroke;
}

}  // namespace

std::vector<TestStroke> generate(const std::string& label, int count, std::mt19937& rng)
{
  Rand rand{rng};
  std::vector<TestStroke> out;
  for(int i = 0; i < count; ++i) {
    TestStroke stroke = label == "line" ? makeLine(rand) : label == "square" ? makeSquare(rand)
        : label == "circle" ? makeCircle(rand) : label == "ellipse" ? makeEllipse(rand)
        : label == "scribble" ? makeScribble(rand) : makeOther(rand);
    // recognition is triggered by holding the pen still at the end, which leaves a cluster of samples
    //  around the last point (unless the app strips them first, hence not always)
    if(rand.chance(0.7) && !stroke.points.empty()) {
      Vec2 rest = stroke.points.back();
      double drift = rand.uniform(0.2, 0.8);
      bool whole = rand.chance(0.5);
      int held = rand.integer(8, 60);
      for(int k = 0; k < held; ++k) {
        Vec2 pt = rest + Vec2(rand.gaussian(drift), rand.gaussian(drift));
        stroke.points.push_back(whole ? Vec2(std::round(pt.x), std::round(pt.y)) : pt);
      }
      stroke.variant += " +hold";
    }
    stroke.label = label;
    stroke.name = "synth-" + label + "-" + std::to_string(i);
    out.push_back(stroke);
  }
  return out;
}

}  // namespace synth
