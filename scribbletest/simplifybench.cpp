// Measurement harness for pen-up stroke simplification (docs/agent/stroke-input.md): replays handwriting
//  through today's input path (curve fit at strength 2) and saves the result once per preset, so the
//  documents can be compared by size and rendered against each other.  Not a pass/fail test.
//
//   SUMI_SIMPLIFY_IN=copy-of-notebook.svgz SUMI_SIMPLIFY_OUT=dir [SUMI_SIMPLIFY_FIRST=0] [SUMI_SIMPLIFY_PAGES=6]
//     [SUMI_SIMPLIFY_ZOOM=1] Sumi --simplifybenchtest     (headless, via cage - never the user's display)
//   without SUMI_SIMPLIFY_IN it uses generated strokes: circles of radius 0.5-32 and loopy lines, one
//   row per pen kind.
//
// Each stroke's centre line and widths are read back with toPenPoints() (old notebooks hold raw input -
//  Write had no curve fit), the width is turned back into pressure, and the points are fed to a new
//  builder with the curve fit installed exactly as ScribbleArea does - so the strokes are as dense as a
//  stroke drawn in Sumi today.  Writes dir/preset{0..3}.html and .svgz and prints points, sizes and the
//  time simplifyFinished() takes (the whole cost added at pen-up).
// Never point SUMI_SIMPLIFY_IN at an original: the document is opened, edited and only saved elsewhere, but
//  use a copy anyway.

#include <chrono>
#include "usvg/svgwriter.h"

struct BenchStrokeInput { ScribblePen pen = ScribblePen(Color::BLACK, 1); std::vector<StrokePoint> pts; };

static void benchPressureFromWidth(const ScribblePen& pen, const std::vector<Dim>& widths, std::vector<Dim>& prs)
{
  prs.clear();
  for(Dim w : widths) {
    // inverse of FilledStrokeBuilder's width: w = W*(1 - wRatio*(1 - wscale)), wscale = 1 - (1 - pr)^prParam
    Dim wscale = pen.wRatio > 0 ? 1 - (1 - std::min(Dim(1), w/pen.width))/pen.wRatio : 1;
    wscale = std::min(Dim(1), std::max(Dim(0), wscale));
    prs.push_back(pen.prParam > 0 ? 1 - std::pow(1 - wscale, 1/pen.prParam) : 1);
  }
}

// a pen and input samples that reproduce `s` through today's builder; false if it is not a pen stroke
static bool benchStrokeInput(Element* s, std::vector<PenPoint> pps, BenchStrokeInput& out)
{
  if(!s->isPathElement()) return false;
  SvgNode* node = s->node;
  bool flat = node->hasClass(Element::FLAT_PEN_CLASS), round = node->hasClass(Element::ROUND_PEN_CLASS);
  bool chisel = node->hasClass(Element::CHISEL_PEN_CLASS), stroked = node->hasClass(Element::STROKE_PEN_CLASS);
  if(!flat && !round && !chisel && !stroked) return false;
  // a flat outline's first and last pairs are the square caps, pushed out by half a width - not samples
  if(flat && pps.size() >= 4)
    pps = std::vector<PenPoint>(pps.begin() + 1, pps.end() - 1);
  if(pps.size() < 2) return false;
  std::vector<Dim> widths;
  Dim wmax = 0;
  for(const PenPoint& pp : pps) { widths.push_back(pp.dr.dist()); wmax = std::max(wmax, widths.back()); }
  Color color = stroked ? node->getColorAttr("stroke", Color::BLACK) : node->getColorAttr("fill", Color::BLACK);
  if(stroked) {
    Dim sw = node->getFloatAttr("stroke-width", 1);
    out.pen = ScribblePen(color, sw, ScribblePen::TIP_ROUND);
  }
  else if(chisel)
    out.pen = ScribblePen(color, std::max(Dim(1), wmax), ScribblePen::TIP_CHISEL);
  else
    out.pen = ScribblePen(color, std::max(Dim(0.2), wmax), (flat ? ScribblePen::TIP_FLAT : ScribblePen::TIP_ROUND)
        | ScribblePen::WIDTH_PR, 0.9, 2.0);
  std::vector<Dim> prs;
  if(out.pen.usesPressure()) benchPressureFromWidth(out.pen, widths, prs);
  out.pts.clear();
  for(size_t ii = 0; ii < pps.size(); ++ii)
    out.pts.push_back(StrokePoint(pps[ii].p.x, pps[ii].p.y, prs.empty() ? 1 : prs[ii], 0, 0, Timestamp(ii*4)));
  return true;
}

// generated strokes, sampled on the whole-unit lattice at ~1 unit spacing like a mouse at 100% zoom
static std::vector<BenchStrokeInput> benchSyntheticStrokes()
{
  std::vector<BenchStrokeInput> out;
  const ScribblePen pens[] = {
    ScribblePen(Color::BLACK, 1.6, ScribblePen::TIP_FLAT | ScribblePen::WIDTH_PR, 0.9, 2.0),
    ScribblePen(Color::BLACK, 3.0, ScribblePen::TIP_FLAT | ScribblePen::WIDTH_PR | ScribblePen::WIDTH_DIR, 0.8, 2.0, 0, 45),
    ScribblePen(Color::BLACK, 4.0, ScribblePen::TIP_ROUND),
    ScribblePen(Color::BLACK, 2.0, ScribblePen::TIP_ROUND | ScribblePen::WIDTH_PR, 0.8, 2.0),
    ScribblePen(Color(255, 127, 255, 127), 34, ScribblePen::TIP_CHISEL),
  };
  int row = 0;
  for(const ScribblePen& pen : pens) {
    Dim y0 = 60 + row*150;
    Dim x = 30;
    for(Dim radius : {0.5, 1.0, 2.0, 4.0, 8.0, 16.0, 32.0}) {
      BenchStrokeInput in{pen, {}};
      Dim cx = x + radius, cy = y0 + 40;
      int nsamp = std::max(8, int(2*M_PI*radius));
      for(int ii = 0; ii <= nsamp; ++ii) {
        Dim th = 2*M_PI*ii/nsamp;
        in.pts.push_back(StrokePoint(std::floor(cx + radius*std::cos(th) + 0.5), std::floor(cy + radius*std::sin(th) + 0.5),
            0.5 + 0.4*std::sin(3*th), 0, 0, Timestamp(ii*8)));
      }
      out.push_back(in);
      x += 2*radius + 25;
    }
    BenchStrokeInput loops{pen, {}};
    for(int ii = 0; ii <= 500; ++ii) {
      Dim t = ii*0.05;
      loops.pts.push_back(StrokePoint(std::floor(30 + 25*t + 9*std::cos(t*2.1) + 0.5), std::floor(y0 + 110 - 12*std::sin(t*2.1) + 0.5),
          0.4 + 0.35*std::sin(t*0.8), 0, 0, Timestamp(ii*8)));
    }
    out.push_back(loops);
    ++row;
  }
  return out;
}


// The legacy notebooks hold sparse samples (Write stored what the device gave, 2-3.5 units apart), which
//  the curve fit barely densifies.  A stroke drawn in Sumi today on a Pencil or a mouse is much denser, so
//  SUMI_SIMPLIFY_SPACING > 0 redraws the same handwriting: a smooth curve through the samples, resampled
//  every `spacing` units, with +-`jitter` of noise, optionally rounded to the whole-unit lattice (what
//  most platforms deliver - stroke-input.md).  Deterministic.
static std::vector<StrokePoint> benchResample(const std::vector<StrokePoint>& in, Dim spacing, bool lattice,
    Dim jitter, unsigned int& seed)
{
  if(in.size() < 2 || spacing <= 0) return in;
  std::vector<StrokePoint> fine;
  int n = int(in.size());
  for(int ii = 0; ii + 1 < n; ++ii) {
    const StrokePoint& p1 = in[ii];
    const StrokePoint& p2 = in[ii + 1];
    Point p0 = ii > 0 ? Point(in[ii - 1]) : 2*Point(p1) - Point(p2);
    Point p3 = ii + 2 < n ? Point(in[ii + 2]) : 2*Point(p2) - Point(p1);
    for(int jj = 0; jj < 16; ++jj) {
      Dim t = jj/16.0, t2 = t*t, t3 = t2*t;
      Point c = 0.5*((2*Point(p1)) + (Point(p2) - p0)*t + (2*p0 - 5*Point(p1) + 4*Point(p2) - p3)*t2
          + (3*Point(p1) - p0 - 3*Point(p2) + p3)*t3);
      fine.push_back(StrokePoint(c.x, c.y, p1.pr + t*(p2.pr - p1.pr)));
    }
  }
  fine.push_back(in.back());
  auto noise = [&]() { seed = seed*1103515245u + 12345u; return (((seed >> 16) & 0x7FFF)/32767.0 - 0.5)*2*jitter; };
  std::vector<StrokePoint> out;
  auto emit = [&](StrokePoint p) {
    p.x += noise();  p.y += noise();
    if(lattice) { p.x = std::floor(p.x + 0.5);  p.y = std::floor(p.y + 0.5); }
    p.t = Timestamp(out.size()*4);
    out.push_back(p);
  };
  emit(fine[0]);
  Dim carry = 0;
  for(size_t ii = 1; ii < fine.size(); ++ii) {
    Dim seg = fine[ii].dist(fine[ii - 1]);
    Dim pos = spacing - carry;
    while(pos <= seg) {
      Dim u = pos/seg;
      StrokePoint q = fine[ii - 1];
      q.x += u*(fine[ii].x - q.x);  q.y += u*(fine[ii].y - q.y);  q.pr += u*(fine[ii].pr - q.pr);
      emit(q);
      pos += spacing;
    }
    carry = seg - (pos - spacing);
  }
  emit(in.back());
  return out;
}

void ScribbleTest::simplifyBench()
{
  const char* in = getenv("SUMI_SIMPLIFY_IN");
  const char* outdir = getenv("SUMI_SIMPLIFY_OUT") ? getenv("SUMI_SIMPLIFY_OUT") : "/tmp";
  int first = getenv("SUMI_SIMPLIFY_FIRST") ? atoi(getenv("SUMI_SIMPLIFY_FIRST")) : 0;
  int npages = getenv("SUMI_SIMPLIFY_PAGES") ? atoi(getenv("SUMI_SIMPLIFY_PAGES")) : 6;
  Dim zoom = getenv("SUMI_SIMPLIFY_ZOOM") ? atof(getenv("SUMI_SIMPLIFY_ZOOM")) : 1;
  Dim spacing = getenv("SUMI_SIMPLIFY_SPACING") ? atof(getenv("SUMI_SIMPLIFY_SPACING")) : 0;
  Dim jitter = getenv("SUMI_SIMPLIFY_JITTER") ? atof(getenv("SUMI_SIMPLIFY_JITTER")) : 0;
  bool lattice = getenv("SUMI_SIMPLIFY_LATTICE") && atoi(getenv("SUMI_SIMPLIFY_LATTICE")) > 0;
  // SUMI_SIMPLIFY_PEN=flat redraws every stroke with Sumi's default pen (flat tip, pressure; the pressure
  //  swells and fades along the stroke) instead of the pen it was written with
  bool forceFlat = getenv("SUMI_SIMPLIFY_PEN") && std::string(getenv("SUMI_SIMPLIFY_PEN")) == "flat";
  char buf[512];

  for(int preset = SIMPLIFY_OFF; preset <= SIMPLIFY_AGGRESSIVE; ++preset) {
    StrokeSimplifyParams params = strokeSimplifyPreset(preset);
    std::vector<double> times;  // ms per stroke in simplifyFinished()
    size_t nPoints = 0, nStrokes = 0, nInput = 0;
    unsigned int seed = 12345;
    BenchStrokeInput largest;  // the longest stroke, timed again below without scheduler noise
    auto makeStroke = [&](const BenchStrokeInput& input) {
      StrokeBuilder* builder = StrokeBuilder::create(input.pen);
      builder->addFilter(new CurveFitFilter(CURVEFIT_TOL/zoom, 2));
      std::vector<StrokePoint> samples = benchResample(input.pts, spacing, lattice, jitter, seed);
      for(const StrokePoint& p : samples)
        builder->addInputPoint(p);
      if(samples.size() > largest.pts.size()) { largest.pen = input.pen;  largest.pts = samples; }
      Element* s = builder->finish();
      auto t0 = std::chrono::steady_clock::now();
      builder->simplifyFinished(s, params);
      times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
      delete builder;
      nPoints += static_cast<SvgPath*>(s->node)->path()->size();
      nInput += samples.size();
      ++nStrokes;
      return s;
    };

    if(in) {
      if(scribbleDoc->openDocument(in) != Document::LOAD_OK) {
        resultStr = std::string("simplifybench: cannot open ") + in;
        return;
      }
      Document* doc = scribbleDoc->document;
      int last = std::min(doc->numPages(), first + npages);
      for(int pp = first; pp < last; ++pp) {
        Page* page = doc->pages[pp];
        page->ensureLoaded(false);
        std::vector<Element*> strokes;
        for(Element* s : page->children()) strokes.push_back(s);
        for(Element* s : strokes) {
          BenchStrokeInput input;
          if(!s->isPathElement() || !benchStrokeInput(s, s->toPenPoints(), input)) continue;
          if(forceFlat) {
            input.pen = ScribblePen(input.pen.color, 1.6, ScribblePen::TIP_FLAT | ScribblePen::WIDTH_PR, 0.9, 2.0);
            Dim arc = 0;
            for(size_t ii = 0; ii < input.pts.size(); ++ii) {
              if(ii > 0) arc += input.pts[ii].dist(input.pts[ii - 1]);
              input.pts[ii].pr = 0.55 + 0.3*std::sin(arc/9);
            }
          }
          Element* fresh = makeStroke(input);
          if(forceFlat) {
            // a different kind of path: the new element takes the old one's place (removeStroke() hands
            //  the old one to the undo history or deletes it)
            page->addStroke(fresh, s);
            page->removeStroke(s);
            continue;
          }
          // swap the rebuilt geometry into the original element, keeping its place, layer and attributes
          SvgPath* dst = static_cast<SvgPath*>(s->node);
          *dst->path() = *static_cast<SvgPath*>(fresh->node)->path();
          dst->invalidate(false);
          fresh->deleteNode();
        }
      }
      std::vector<int> drop;
      for(int pp = 0; pp < doc->numPages(); ++pp)
        if(pp < first || pp >= last) drop.push_back(pp);
      if(!drop.empty()) scribbleDoc->deletePageList(drop);
    }
    else {
      scribbleDoc->newDocument();
      for(const BenchStrokeInput& input : benchSyntheticStrokes())
        scribbleArea->currPage->addStroke(makeStroke(input));
    }
    std::string base = std::string(outdir) + "/preset" + std::to_string(preset);
    scribbleDoc->saveDocument((base + ".html").c_str());
    scribbleDoc->saveDocument((base + ".svgz").c_str());
    // for rendering comparisons: path data is normally written as 3 decimal *relative* coordinates, and
    //  the rounding accumulates along a path (a few thousandths of a unit over a long stroke) differently
    //  for every preset - absolute coordinates keep each point within 0.0005 of the geometry in memory
    SvgWriter::DEFAULT_PATH_DATA_REL = false;
    scribbleDoc->saveDocument((base + ".abs.html").c_str());
    SvgWriter::DEFAULT_PATH_DATA_REL = true;
    // the worst case on its own: the longest stroke, best of 20 (a single timing on this shared machine
    //  can be a preemption, which is what the max above mostly is)
    double longestMs = 1E9;
    size_t longestPts = 0;
    for(int rep = 0; rep < 20 && !largest.pts.empty(); ++rep) {
      StrokeBuilder* builder = StrokeBuilder::create(largest.pen);
      builder->addFilter(new CurveFitFilter(CURVEFIT_TOL/zoom, 2));
      for(const StrokePoint& p : largest.pts)
        builder->addInputPoint(p);
      Element* s = builder->finish();
      longestPts = static_cast<SvgPath*>(s->node)->path()->size();
      auto t0 = std::chrono::steady_clock::now();
      builder->simplifyFinished(s, params);
      longestMs = std::min(longestMs, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
      delete builder;
      s->deleteNode();
    }
    std::sort(times.begin(), times.end());
    double total = 0;
    for(double t : times) total += t;
    snprintf(buf, sizeof(buf), "simplifybench preset %d (tol %.3f, turn %.0f deg): %d strokes, %d input samples, "
        "%d path points; simplify ms per stroke: mean %.4f median %.4f p99 %.4f max %.4f\n",
        preset, params.tol, params.maxTurn*180/M_PI, int(nStrokes), int(nInput), int(nPoints),
        times.empty() ? 0 : total/times.size(), times.empty() ? 0 : times[times.size()/2],
        times.empty() ? 0 : times[std::min(times.size() - 1, size_t(times.size()*0.99))], times.empty() ? 0 : times.back());
    printf("%s", buf);
    resultStr += buf;
    snprintf(buf, sizeof(buf), "simplifybench preset %d longest stroke: %d samples, %d path points before, simplify %.4f ms (best of 20)\n",
        preset, int(largest.pts.size()), int(longestPts), longestMs);
    printf("%s", buf);
    resultStr += buf;
  }
  scribbleDoc->newDocument();
}
