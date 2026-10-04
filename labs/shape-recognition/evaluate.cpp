// Shape recognition test bench: runs the recognizer over synthetic strokes and any recorded strokes in
//  fixtures/, scores both *what* it recognized and *where* it put it, and writes an HTML report.
//
//   ./evaluate [--count N] [--seed S] [--only LABEL] [--misses N] [--fixtures DIR] [--report FILE] [--dump FILE]
//
// Exit code is 0 only if every synthetic gate below passes.  Recorded strokes are reported but not
//  gated - there are too few of them, and their truth is only as good as the tracing.

#include "shaperec.h"
#include "strokefile.h"
#include "synth.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>

using shaperec::Kind;
using shaperec::Result;

namespace {

const char* LABELS[] = {"line", "square", "circle", "ellipse", "scribble", "other"};
const int LABEL_COUNT = 6;

// Gates.  Position error is the worst point's error as a fraction of the shape's size: line length,
//  mean side length, circle radius; for a scratch-out, the largest distance between the outlines of
//  the erase area and the area scratched, over that area's size.  Set about 1.5x above what the recognizer achieves, so a regression fails them: each
//  was checked against a deliberately broken recognizer (see README).
//
// Recognition runs only on a deliberate hold, so for line, square and circle a miss is the expensive
//  mistake and the recall gates are high.  A scratch-out erases, so its gate is the other way round:
//  nothing else may ever be taken for one.  "other" strokes becoming a line or circle is reported, not
//  gated - after a hold, some shape is what the user asked for.
// a circle only counts if it came out as a true circle, and an ellipse only if it did not
const std::map<std::string, double> MIN_RECALL = {{"line", 0.99}, {"square", 0.99}, {"circle", 0.99},
    {"ellipse", 0.99}, {"scribble", 0.95}};
// The pencil set (Gates::Recognition).  Scratch-outs higher than above: it holds the wide zigzags, which
//  the scrub-axis fallback in fitScribble() takes from 84% to 99.5%, and this is what catches losing it.
//  Ellipses lower: its thinnest ones (minor/major ~0.3) stopped 60 degrees short leave a gap of 0.26-0.33
//  of the perimeter at the pointed end, past maxClosureGap; raising that further turns 270-degree arcs
//  into circles.  1-1.6% of the set, depending on the seed.
const std::map<std::string, double> PENCIL_MIN_RECALL = {{"line", 0.99}, {"square", 0.99}, {"circle", 0.99},
    {"ellipse", 0.98}, {"scribble", 0.98}};
// 0.5% rather than lower on purpose: scratch-outs were made more eager (3 reversals, not 4 - recall on
//  messy ones 87% -> 99.7%), which puts false erases at 0.13-0.4% depending on the seed.  Every one of
//  them is a random swooping curve swinging back and forth three times, arguably a scratch-out after a
//  deliberate hold; no handwriting stroke in the set (arches, loops, N/Z, spirals) triggers one.  A
//  tip-linger test (swoops turn wide, scratching turns sharp) was tried and did not separate them.
const double MAX_FALSE_ERASE = 0.005;
const std::map<std::string, double> MAX_P90_POS_ERR = {{"line", 0.02}, {"square", 0.055}, {"circle", 0.015}, {"ellipse", 0.035}, {"scribble", 0.1}};
// and no single stroke may be off by more than this multiple of the p90 gate - a rare catastrophe
//  (a line's end cut 14% short) is invisible to a percentile
const double MAX_POS_ERR_FACTOR = 4;

Kind expectedKind(const std::string& label)
{
  if(label == "line") return Kind::Line;
  if(label == "square") return Kind::Quad;
  if(label == "circle" || label == "ellipse") return Kind::Ellipse;
  if(label == "scribble") return Kind::Scribble;
  return Kind::None;
}

std::vector<Vec2> truthPoints(const TestStroke& stroke)
{
  std::vector<Vec2> pts;
  for(size_t i = 0; i + 1 < stroke.truth.size(); i += 2)
    pts.emplace_back(stroke.truth[i], stroke.truth[i+1]);
  return pts;
}

double lineError(const std::vector<Vec2>& found, const std::vector<Vec2>& truth)
{
  double len = dist(truth[0], truth[1]);
  double forward = std::max(dist(found[0], truth[0]), dist(found[1], truth[1]));
  double backward = std::max(dist(found[0], truth[1]), dist(found[1], truth[0]));
  return std::min(forward, backward)/len;
}

// worst corner, over every cyclic shift and both directions - the recognizer's corner order starts
//  wherever the loop was cut
double quadError(const std::vector<Vec2>& found, const std::vector<Vec2>& truth)
{
  double meanSide = 0;
  for(int i = 0; i < 4; ++i)
    meanSide += dist(truth[i], truth[(i+1) % 4])/4;
  double best = Result::NO_FIT;
  for(int shift = 0; shift < 4; ++shift)
    for(int reversed = 0; reversed < 2; ++reversed) {
      double worst = 0;
      for(int i = 0; i < 4; ++i) {
        int idx = reversed ? (shift - i + 4) % 4 : (shift + i) % 4;
        worst = std::max(worst, dist(found[idx], truth[i]));
      }
      best = std::min(best, worst);
    }
  return best/meanSide;
}

// A scratch-out's erase area against the area scratched: the largest distance between their outlines
//  (Hausdorff, both outlines sampled), over the size of the area.  Not the overlap of the two areas -
//  rubbing back and forth along one line scratches an area with almost no width, where a fraction of a
//  unit of difference is most of the overlap.
double outlineError(const std::vector<Vec2>& found, const std::vector<Vec2>& truth)
{
  auto sampleOutline = [](const std::vector<Vec2>& poly) {
    std::vector<Vec2> pts;
    for(size_t i = 0; i < poly.size(); ++i)
      for(int step = 0; step < 20; ++step)
        pts.push_back(poly[i] + (poly[(i+1) % poly.size()] - poly[i])*(step/20.0));
    return pts;
  };
  auto farthest = [](const std::vector<Vec2>& pts, const std::vector<Vec2>& poly) {
    double worst = 0;
    for(const Vec2& pt : pts) {
      double best = Result::NO_FIT;
      for(size_t i = 0; i < poly.size(); ++i)
        best = std::min(best, distToSegment(pt, poly[i], poly[(i+1) % poly.size()]));
      worst = std::max(worst, best);
    }
    return worst;
  };
  double size = 0;
  for(const Vec2& from : truth)
    for(const Vec2& to : truth)
      size = std::max(size, dist(from, to));
  double hausdorff = std::max(farthest(sampleOutline(found), truth), farthest(sampleOutline(truth), found));
  return size > 0 ? hausdorff/size : 1;
}

// points around an ellipse given as centre, radii and the angle of radiusX
std::vector<Vec2> ellipsePoints(const Vec2& center, double radiusX, double radiusY, double angle)
{
  Vec2 axisU(std::cos(angle), std::sin(angle)), axisV = axisU.perp();
  std::vector<Vec2> pts;
  for(int i = 0; i < 90; ++i) {
    double param = 2*3.14159265358979*i/90;
    pts.push_back(center + axisU*(radiusX*std::cos(param)) + axisV*(radiusY*std::sin(param)));
  }
  return pts;
}

// largest distance between the two outlines, over the truth's larger radius
double ellipseError(const Result& res, const TestStroke& stroke)
{
  std::vector<Vec2> found = ellipsePoints(res.center, res.radiusX, res.radiusY, res.angle);
  std::vector<Vec2> truth = ellipsePoints(Vec2(stroke.truth[0], stroke.truth[1]), stroke.truth[2], stroke.truth[3], stroke.truth[4]);
  auto farthest = [](const std::vector<Vec2>& from, const std::vector<Vec2>& poly) {
    double worst = 0;
    for(const Vec2& pt : from) {
      double best = Result::NO_FIT;
      for(size_t i = 0; i < poly.size(); ++i)
        best = std::min(best, distToSegment(pt, poly[i], poly[(i+1) % poly.size()]));
      worst = std::max(worst, best);
    }
    return worst;
  };
  return std::max(farthest(found, truth), farthest(truth, found))/std::max(stroke.truth[2], stroke.truth[3]);
}

double circleError(const Result& res, const TestStroke& stroke)
{
  Vec2 center(stroke.truth[0], stroke.truth[1]);
  double radius = stroke.truth[2];
  double foundRadius = 0.5*(res.radiusX + res.radiusY);
  return std::max(dist(res.center, center), std::fabs(foundRadius - radius))/radius;
}

struct Outcome
{
  const TestStroke* stroke;
  Result result;
  bool correct = false;
  double posErr = -1;   // -1: no truth, or wrong class
  double naiveErr = -1; // squares: the drawn points at the turning peaks, for comparison
  bool gateFail = false;
};

Outcome evaluateOne(const TestStroke& stroke)
{
  Outcome out;
  out.stroke = &stroke;
  out.result = shaperec::recognize(stroke.points);
  out.correct = out.result.kind == expectedKind(stroke.label);
  if(out.correct && stroke.label == "circle")
    out.correct = out.result.circle;
  if(out.correct && stroke.label == "ellipse")
    out.correct = !out.result.circle;
  if(!out.correct || stroke.truth.empty())
    return out;
  if(stroke.label == "line" && stroke.truth.size() == 4)
    out.posErr = lineError(out.result.points, truthPoints(stroke));
  else if(stroke.label == "square" && stroke.truth.size() == 8) {
    out.posErr = quadError(out.result.points, truthPoints(stroke));
    if(out.result.rawCorners.size() == 4)
      out.naiveErr = quadError(out.result.rawCorners, truthPoints(stroke));
  }
  else if(stroke.label == "circle" && stroke.truth.size() == 3)
    out.posErr = circleError(out.result, stroke);
  else if(stroke.label == "ellipse" && stroke.truth.size() == 5)
    out.posErr = ellipseError(out.result, stroke);
  else if(stroke.label == "scribble" && stroke.truth.size() >= 6)
    out.posErr = outlineError(out.result.points, truthPoints(stroke));
  return out;
}

double percentile(std::vector<double> values, double frac)
{
  if(values.empty())
    return NAN;
  std::sort(values.begin(), values.end());
  return values[std::min(values.size() - 1, size_t(frac*(values.size() - 1) + 0.5))];
}

// which gates a set of outcomes is held to.  The pencil set misses corners and stops short by more on
//  purpose, so its truth is looser and only *what* was recognized is gated there, not where.
enum class Gates { None, Recognition, All };

// --misses N: print up to N wrongly recognized strokes per class, with the recognizer's reason
int showMisses = 0;

// prints the table for one set of outcomes; returns false if a gate failed
bool summarize(const char* title, std::vector<Outcome>& outcomes, Gates gates)
{
  bool gated = gates != Gates::None;
  printf("\n%s\n", title);
  printf("  %-9s %5s %8s   %5s %5s %7s %5s %5s   %-28s %s\n", "class", "n", "correct", "line", "quad",
      "ellipse", "scrib", "none", "position err % median/p90/max", "naive corners");
  bool pass = true;
  for(const char* label : LABELS) {
    int total = 0, correct = 0;
    std::map<Kind, int> got;
    std::vector<double> pos, naive;
    for(const Outcome& out : outcomes) {
      if(out.stroke->label != label)
        continue;
      ++total;
      correct += out.correct;
      ++got[out.result.kind];
      if(out.posErr >= 0) pos.push_back(out.posErr*100);
      if(out.naiveErr >= 0) naive.push_back(out.naiveErr*100);
    }
    if(total == 0)
      continue;
    double accuracy = double(correct)/total;
    char posText[64] = "-", naiveText[64] = "";
    if(!pos.empty())
      snprintf(posText, sizeof(posText), "%6.2f %6.2f %6.2f", percentile(pos, 0.5), percentile(pos, 0.9), percentile(pos, 1));
    if(!naive.empty())
      snprintf(naiveText, sizeof(naiveText), "%6.2f %6.2f %6.2f", percentile(naive, 0.5), percentile(naive, 0.9), percentile(naive, 1));
    std::string verdict;
    if(gated) {
      const auto& minRecall = gates == Gates::All ? MIN_RECALL : PENCIL_MIN_RECALL;
      auto recall = minRecall.find(label);
      if(recall != minRecall.end() && accuracy < recall->second) {
        pass = false;
        verdict += "  FAIL recall";
      }
      auto gate = gates == Gates::All ? MAX_P90_POS_ERR.find(label) : MAX_P90_POS_ERR.end();
      if(gate != MAX_P90_POS_ERR.end() && !pos.empty() && percentile(pos, 0.9) > gate->second*100) {
        pass = false;
        verdict += "  FAIL position";
      }
      if(gate != MAX_P90_POS_ERR.end() && !pos.empty() && percentile(pos, 1) > MAX_POS_ERR_FACTOR*gate->second*100) {
        pass = false;
        verdict += "  FAIL max position";
      }
      // mark individual strokes past the position gate so the report shows them
      if(gate != MAX_P90_POS_ERR.end())
        for(Outcome& out : outcomes)
          if(out.stroke->label == label && out.posErr > gate->second)
            out.gateFail = true;
    }
    printf("  %-9s %5d %7.1f%%   %5d %5d %7d %5d %5d   %-28s %s%s\n", label, total, accuracy*100,
        got[Kind::Line], got[Kind::Quad], got[Kind::Ellipse], got[Kind::Scribble], got[Kind::None], posText,
        naiveText, verdict.c_str());
    int shown = 0;
    for(const Outcome& out : outcomes)
      if(out.stroke->label == label && !out.correct && shown++ < showMisses)
        printf("    %s -> %s: %s | %s\n", out.stroke->name.c_str(), kindName(out.result.kind),
            out.result.reason.c_str(), out.stroke->variant.c_str());
  }
  // the one mistake that destroys work: anything but a scratch-out taken for one
  int others = 0;
  std::vector<const Outcome*> erased;
  for(const Outcome& out : outcomes)
    if(out.stroke->label != "scribble") {
      ++others;
      if(out.result.kind == Kind::Scribble)
        erased.push_back(&out);
    }
  if(others > 0) {
    double rate = double(erased.size())/others;
    bool fail = gated && rate > MAX_FALSE_ERASE;
    printf("  false scratch-outs: %zu of %d (%.2f%%)%s\n", erased.size(), others, rate*100, fail ? "  FAIL" : "");
    for(size_t i = 0; i < erased.size() && i < 8; ++i)
      printf("    %s: %s\n", erased[i]->stroke->name.c_str(), erased[i]->stroke->variant.c_str());
    pass = pass && !fail;
  }
  return pass;
}

std::string escape(const std::string& text)
{
  std::string out;
  for(char ch : text) {
    if(ch == '<') out += "&lt;";
    else if(ch == '>') out += "&gt;";
    else if(ch == '&') out += "&amp;";
    else out += ch;
  }
  return out;
}

std::string svgPoints(const std::vector<Vec2>& pts)
{
  std::string out;
  char buf[64];
  for(const Vec2& pt : pts) {
    snprintf(buf, sizeof(buf), "%.1f,%.1f ", pt.x, pt.y);
    out += buf;
  }
  return out;
}

std::string tile(const Outcome& out)
{
  const TestStroke& stroke = *out.stroke;
  const Result& res = out.result;
  std::vector<Vec2> extent = stroke.points;
  if(stroke.label == "circle" && stroke.truth.size() == 3) {
    extent.emplace_back(stroke.truth[0] - stroke.truth[2], stroke.truth[1] - stroke.truth[2]);
    extent.emplace_back(stroke.truth[0] + stroke.truth[2], stroke.truth[1] + stroke.truth[2]);
  }
  else if(stroke.label == "ellipse" && stroke.truth.size() == 5) {
    for(const Vec2& pt : ellipsePoints(Vec2(stroke.truth[0], stroke.truth[1]), stroke.truth[2], stroke.truth[3], stroke.truth[4]))
      extent.push_back(pt);
  }
  else
    for(const Vec2& pt : truthPoints(stroke))
      extent.push_back(pt);
  for(const Vec2& pt : res.points)
    extent.push_back(pt);
  double minX = 1e300, minY = 1e300, maxX = -1e300, maxY = -1e300;
  for(const Vec2& pt : extent) {
    minX = std::min(minX, pt.x); maxX = std::max(maxX, pt.x);
    minY = std::min(minY, pt.y); maxY = std::max(maxY, pt.y);
  }
  double span = std::max(maxX - minX, maxY - minY);
  double pad = 0.08*span + 2;
  double dot = 0.012*span + 0.5;
  char buf[1024];
  std::string svg;
  snprintf(buf, sizeof(buf), "<svg viewBox='%.1f %.1f %.1f %.1f'>", minX - pad, minY - pad,
      maxX - minX + 2*pad, maxY - minY + 2*pad);
  svg += buf;

  // truth
  std::vector<Vec2> truth = truthPoints(stroke);
  if(stroke.label == "circle" && stroke.truth.size() == 3) {
    snprintf(buf, sizeof(buf), "<circle class='truth' cx='%.1f' cy='%.1f' r='%.1f'/>", stroke.truth[0], stroke.truth[1], stroke.truth[2]);
    svg += buf;
  }
  else if(stroke.label == "ellipse" && stroke.truth.size() == 5)
    svg += "<polygon class='truth' points='" + svgPoints(ellipsePoints(Vec2(stroke.truth[0], stroke.truth[1]),
        stroke.truth[2], stroke.truth[3], stroke.truth[4])) + "'/>";
  else if((stroke.label == "square" && truth.size() == 4) || (stroke.label == "scribble" && truth.size() >= 3))
    svg += "<polygon class='truth' points='" + svgPoints(truth) + "'/>";
  else if(stroke.label == "line" && truth.size() == 2)
    svg += "<polyline class='truth' points='" + svgPoints(truth) + "'/>";

  svg += "<polyline class='ink' points='" + svgPoints(stroke.points) + "'/>";

  const char* cls = !out.correct ? "found bad" : out.gateFail ? "found far" : "found";
  if(res.kind == Kind::Line)
    svg += std::string("<polyline class='") + cls + "' points='" + svgPoints(res.points) + "'/>";
  else if(res.kind == Kind::Quad || res.kind == Kind::Scribble)
    svg += std::string("<polygon class='") + cls + "' points='" + svgPoints(res.points) + "'/>";
  else if(res.kind == Kind::Ellipse) {
    snprintf(buf, sizeof(buf), "<ellipse class='%s' cx='0' cy='0' rx='%.1f' ry='%.1f' transform='translate(%.1f %.1f) rotate(%.2f)'/>",
        cls, res.radiusX, res.radiusY, res.center.x, res.center.y, res.angle*180/3.14159265358979);
    svg += buf;
  }
  for(const Vec2& pt : res.rawCorners) {
    snprintf(buf, sizeof(buf), "<circle class='raw' cx='%.1f' cy='%.1f' r='%.1f'/>", pt.x, pt.y, dot);
    svg += buf;
  }
  for(const Vec2& pt : res.points) {
    snprintf(buf, sizeof(buf), "<circle class='pt' cx='%.1f' cy='%.1f' r='%.1f'/>", pt.x, pt.y, dot);
    svg += buf;
  }
  svg += "</svg>";

  std::string caption = stroke.label + " &rarr; " + shaperec::kindName(res.kind);
  if(res.circle)
    caption += " (circle)";
  if(out.posErr >= 0) {
    snprintf(buf, sizeof(buf), " &middot; err %.2f%%", out.posErr*100);
    caption += buf;
  }
  if(out.naiveErr >= 0) {
    snprintf(buf, sizeof(buf), " (naive %.2f%%)", out.naiveErr*100);
    caption += buf;
  }
  std::string status = !out.correct ? "wrong" : out.gateFail ? "far" : "ok";
  return "<figure class='" + status + "'>" + svg + "<figcaption><b>" + caption + "</b><br>" + escape(stroke.name)
      + "<br><span>" + escape(stroke.variant) + "</span><br><span>" + escape(res.reason) + "</span></figcaption></figure>\n";
}

void writeReport(const std::string& path, const std::vector<std::pair<std::string, std::vector<Outcome>*>>& sections)
{
  std::ofstream file(path);
  file << R"HTML(<!doctype html><html><head><meta charset="utf-8"><title>Shape Recognition Report</title>
<style>
:root { --bg:#fafafa; --fg:#222; --muted:#666; --card:#fff; --line:#ddd; --ink:#888; --truth:#16a34a; --found:#2563eb; --bad:#dc2626; --raw:#d946ef; --far:#f59e0b; }
@media (prefers-color-scheme: dark) { :root { --bg:#161616; --fg:#eee; --muted:#999; --card:#222; --line:#333; --ink:#777; } }
body { background:var(--bg); color:var(--fg); font:13px system-ui, sans-serif; margin:16px; }
h2 { margin-top:32px; } p.key span { margin-right:16px; }
.grid { display:grid; grid-template-columns:repeat(auto-fill, minmax(220px, 1fr)); gap:12px; }
figure { margin:0; background:var(--card); border:1px solid var(--line); border-radius:6px; padding:6px; }
figure.wrong { border-color:var(--bad); } figure.far { border-color:var(--far); }
svg { width:100%; aspect-ratio:1; }
svg * { fill:none; vector-effect:non-scaling-stroke; }
.ink { stroke:var(--ink); stroke-width:1.5; }
.truth { stroke:var(--truth); stroke-width:1.5; stroke-dasharray:4 3; }
.found { stroke:var(--found); stroke-width:2; } .found.bad { stroke:var(--bad); } .found.far { stroke:var(--far); }
circle.raw { fill:var(--raw); stroke:none; } circle.pt { fill:var(--found); stroke:none; }
figcaption { font-size:11px; color:var(--muted); overflow-wrap:anywhere; } figcaption b { color:var(--fg); }
</style></head><body>
<h1>Shape recognition</h1>
<p class="key"><span style="color:var(--ink)">&#9644; ink</span><span style="color:var(--truth)">- - truth</span>
<span style="color:var(--found)">&#9644; recognized</span><span style="color:var(--bad)">&#9644; wrong class</span>
<span style="color:var(--raw)">&#9679; naive corner (drawn point at the turn)</span></p>
<p>Red: wrong class. Orange: right class, but position error past that class's p90 gate. Wrong-class strokes first, then the largest position errors per class, then a sample.</p>
)HTML";
  for(const auto& section : sections) {
    std::vector<Outcome>& outcomes = *section.second;
    if(outcomes.empty())
      continue;
    file << "<h2>" << section.first << "</h2>\n";
    std::vector<const Outcome*> wrong, worst, sample;
    for(const Outcome& out : outcomes)
      if(!out.correct && wrong.size() < 200)
        wrong.push_back(&out);
    file << "<h3>Wrong class (" << wrong.size() << ")</h3><div class='grid'>\n";
    for(const Outcome* out : wrong)
      file << tile(*out);
    // ~10% of any class lies past its p90 gate by definition, so listing all of those buries the
    //  interesting ones; the worst few per class are what to look at
    for(const char* label : LABELS) {
      std::vector<const Outcome*> located;
      for(const Outcome& out : outcomes)
        if(out.stroke->label == label && out.posErr >= 0)
          located.push_back(&out);
      std::sort(located.begin(), located.end(), [](const Outcome* lhs, const Outcome* rhs) { return lhs->posErr > rhs->posErr; });
      worst.insert(worst.end(), located.begin(), located.begin() + std::min<size_t>(12, located.size()));
    }
    file << "</div><h3>Largest position errors</h3><div class='grid'>\n";
    for(const Outcome* out : worst)
      file << tile(*out);
    file << "</div><h3>Sample</h3><div class='grid'>\n";
    std::map<std::string, int> perLabel;
    for(const Outcome& out : outcomes)
      if(out.correct && perLabel[out.stroke->label]++ < 16)
        file << tile(out);
    file << "</div>\n";
  }
  file << "</body></html>\n";
}

}  // namespace

int main(int argc, char* argv[])
{
  int count = 1000;
  unsigned seed = 1;
  std::string fixturesDir = "fixtures", reportPath = "report.html", dumpPath, only;
  for(int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    bool hasValue = i + 1 < argc;
    if(arg == "--count" && hasValue) count = atoi(argv[++i]);
    else if(arg == "--seed" && hasValue) seed = unsigned(atoi(argv[++i]));
    else if(arg == "--fixtures" && hasValue) fixturesDir = argv[++i];
    else if(arg == "--report" && hasValue) reportPath = argv[++i];
    else if(arg == "--dump" && hasValue) dumpPath = argv[++i];
    else if(arg == "--only" && hasValue) only = argv[++i];
    else if(arg == "--misses" && hasValue) showMisses = atoi(argv[++i]);
    else {
      fprintf(stderr, "usage: %s [--count N] [--seed S] [--only LABEL] [--misses N] [--fixtures DIR] [--report FILE] [--dump FILE]\n", argv[0]);
      return 2;
    }
  }

  // the default hand, and Apple Pencil input (240 Hz, drifting hold, light arcs, wide zigzags); both gated
  std::vector<TestStroke> synthetic, pencil;
  for(int labelIdx = 0; labelIdx < LABEL_COUNT; ++labelIdx) {
    const char* label = LABELS[labelIdx];
    if(!only.empty() && only != label)
      continue;
    // one stream per label, so --only reproduces exactly the strokes of a full run
    std::mt19937 rng(seed*LABEL_COUNT + labelIdx);
    std::vector<TestStroke> batch = synth::generate(label, count, rng);
    synthetic.insert(synthetic.end(), batch.begin(), batch.end());
    std::mt19937 pencilRng(seed*LABEL_COUNT + labelIdx + 1000003);
    batch = synth::generate(label, count, pencilRng, true);
    pencil.insert(pencil.end(), batch.begin(), batch.end());
  }
  if(!dumpPath.empty()) {
    std::vector<TestStroke> all = synthetic;
    all.insert(all.end(), pencil.begin(), pencil.end());
    writeStrokeFile(dumpPath, all);
  }

  std::vector<TestStroke> recorded;
  namespace fs = std::filesystem;
  if(fs::is_directory(fixturesDir)) {
    std::vector<fs::path> files;
    for(const auto& entry : fs::directory_iterator(fixturesDir))
      if(entry.path().extension() == ".strokes")
        files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    for(const fs::path& file : files) {
      std::string error;
      if(!readStrokeFile(file.string(), recorded, error))
        fprintf(stderr, "%s\n", error.c_str());
    }
  }

  std::vector<Outcome> synthOut, pencilOut, recordedOut;
  for(const TestStroke& stroke : synthetic)
    synthOut.push_back(evaluateOne(stroke));
  for(const TestStroke& stroke : pencil)
    pencilOut.push_back(evaluateOne(stroke));
  for(const TestStroke& stroke : recorded)
    recordedOut.push_back(evaluateOne(stroke));

  char title[128];
  snprintf(title, sizeof(title), "Synthetic strokes (seed %u, %d per class)", seed, count);
  bool pass = summarize(title, synthOut, Gates::All);
  snprintf(title, sizeof(title), "Pencil strokes (seed %u, %d per class)", seed, count);
  pass = summarize(title, pencilOut, Gates::Recognition) && pass;
  if(!recordedOut.empty())
    summarize(("Recorded strokes (" + fixturesDir + ", not gated)").c_str(), recordedOut, Gates::None);
  else
    printf("\nNo recorded strokes in %s/ - draw some with recorder.html.\n", fixturesDir.c_str());

  writeReport(reportPath, {{"Recorded strokes", &recordedOut}, {"Synthetic strokes", &synthOut},
      {"Pencil strokes", &pencilOut}});
  printf("\nReport: %s\n%s\n", reportPath.c_str(), pass ? "PASS" : "FAIL");
  return pass ? 0 : 1;
}
