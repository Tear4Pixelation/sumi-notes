#ifndef STROKEBUILDER_H
#define STROKEBUILDER_H

#include <algorithm>

#include "page.h"
#include "scribblepen.h"

// abstractions for Stroke construction with support for filtering

struct StrokePoint : public Point
{
  Dim pr;
  Dim tiltX;
  Dim tiltY;
  Timestamp t;
  //Dim velocity;
  StrokePoint(Dim _x = 0, Dim _y = 0, Dim _p = 1.0, Dim _tiltX = 0, Dim _tiltY = 0, Timestamp _t = 0)
      : Point(_x, _y), pr(_p), tiltX(_tiltX), tiltY(_tiltY), t(_t) {}
};

class InputProcessor
{
  friend class StrokeBuilder;
public:
  InputProcessor() : next(NULL) {}
  virtual ~InputProcessor() {}

  virtual void addPoint(const StrokePoint& pt) = 0;  //{ next->addPoint(x, y, pr); }
  virtual void removePoints(int n) = 0;  //{ next->removePoints(n); }
  virtual void finalize() = 0;  //{ return next->finalize(); }

  InputProcessor* next;
};

class StrokeBuilder : public InputProcessor
{
public:
  StrokeBuilder() : firstProcessor(this) {}
  ~StrokeBuilder() override;
  void addFilter(InputProcessor* p);
  void addInputPoint(const StrokePoint& pt) { firstProcessor->addPoint(pt); }
  Element* getElement() { return element; }
  Path2D* getPath() { return stroke; }
  virtual Rect getDirty() { return element->bbox(); }
  Element* finish();  // caller assumes ownership of returned Element

  static StrokeBuilder* create(const ScribblePen& pen);
  static Point calcCom(SvgNode* node, Path2D* path);
  // the same heuristic measured in another frame (a tilted ruling region's), where "up" is not page up
  static Point calcCom(SvgNode* node, Path2D* path, const Transform2D& toLocal, const Transform2D& toPage);

protected:
  Element* element;
  Path2D* stroke;
  void finalize() override;

private:
  InputProcessor* firstProcessor;
};

class StrokedStrokeBuilder : public StrokeBuilder
{
public:
  StrokedStrokeBuilder(const ScribblePen& pen);
  Rect getDirty() override;

protected:
  void addPoint(const StrokePoint& pt) override;
  void removePoints(int n) override;

private:
  Dim width;
  Rect dirty;
};

class FilledStrokeBuilder : public StrokeBuilder {
public:
  FilledStrokeBuilder(const ScribblePen& _pen);
  Rect getDirty() override;

  static void addRoundSubpath(Path2D& path, Point pt1, Dim w1, Point pt2, Dim w2);
  static void addChiselSubpath(Path2D& path, Point pt1, Dim w1, Point pt2, Dim w2);
  static constexpr Dim CHISEL_ASPECT_RATIO = 4;

protected:
  void addPoint(const StrokePoint& pt) override;
  void removePoints(int n) override;

private:
  void assembleFlatStroke();

  ScribblePen pen;
  enum Style { Flat, Round, Chisel } style;
  Rect dirty;
  Path2D path1;
  Path2D path2;
  std::vector<Point> points;
  std::vector<Dim> widths;

  // for WIDTH_VEL
  Dim vel = 0;
  Dim totalDist = 0;
  Timestamp prevt = 0;
  Point prevVelPt;
};

// filters

class SimplifyFilter : public InputProcessor
{
public:
  SimplifyFilter(Dim dist = 0.1, Dim pr = 0.1) : threshDist(dist), threshPr(pr) {}

protected:
  void addPoint(const StrokePoint& pt) override;
  void removePoints(int n) override;
  void finalize() override;

private:
  Dim threshDist;
  Dim threshPr;
  std::vector<StrokePoint> pts;
  //int origPts = 0;  // for testing
  //int simpPts = 0;
};

class LowPassIIR : public InputProcessor
{
public:
  LowPassIIR(Dim tau) : prevPt(NaN, NaN), invTau(1/tau) {}
  void addPoint(const StrokePoint& pt) override;
  void removePoints(int n) override;
  void finalize() override;  // { next->finalize(); }

  StrokePoint prevPt;
  StrokePoint filtPt;
  Dim invTau;
};

// pulls the rendered point toward the raw input point once the raw point strays further than `radius`
//  away, like a stroke tethered by a rope of that length - stronger stabilization = larger radius
class StreamStabilizer : public InputProcessor
{
public:
  StreamStabilizer(Dim _radius) : prevPt(NaN, NaN), filtPt(NaN, NaN), radius(_radius) {}
  void addPoint(const StrokePoint& pt) override;
  void removePoints(int n) override;
  void finalize() override;

  StrokePoint prevPt;
  StrokePoint filtPt;
  Dim radius;
};

// chord flatness tolerance, in document units at zoom 1 - divided by the zoom at construction, so
//  drawing zoomed in gets more chords.  Not a user setting: it only trades output points for accuracy
//  of the curve, where what anyone reaching for this wants is the strength (passes).
static constexpr Dim CURVEFIT_TOL = 0.15;

// replaces the straight segments between input samples with a smooth curve
// input positions arrive on a whole-pixel lattice on most platforms, and at the 1-2 px spacing a mouse
//  delivers, a polyline through them can only step in the handful of directions that lattice allows - so
//  the stroke reads as a staircase rather than a line.  Each interior point is relaxed towards its uniform
//  cubic B-spline knot (which attenuates lattice noise to a third of its amplitude while leaving the shape
//  of the stroke alone), then a centripetal Catmull-Rom curve through the relaxed points is emitted as
//  chords flat to `tol`.  This is a curve fit, not a low pass: nothing is averaged over time and the
//  stroke does not trail the pen.
// A curve segment is only final once two further samples have arrived, so the raw current point is
//  appended to the builder as a provisional tip and retracted on the next call - the same trick LowPassIIR
//  uses.  Because this filter turns one input point into several output points, it cannot map a
//  removePoints(n) from upstream, so it must be installed first in the chain (i.e. added last).
// `passes` is the strength: how many times the relaxation is applied, each pass taking another factor
//  of 3 off the lattice noise.  `tol` is not a user knob - see CURVEFIT_TOL.
class CurveFitFilter : public InputProcessor
{
public:
  CurveFitFilter(Dim _tol = 0.15, int _passes = 1, Dim _relax = 1.0)
      : tol(std::max(_tol, Dim(1E-4))), passes(std::min(4, std::max(0, _passes))),
        relax(std::min(Dim(1), std::max(Dim(0), _relax))) {}
  void addPoint(const StrokePoint& pt) override;
  void removePoints(int n) override;
  void finalize() override;

private:
  StrokePoint knotAt(int ii, int n, int level) const;
  void emitSegment(int ii, int n);

  std::vector<StrokePoint> pts;
  int emitted = -1;  // index of the last point up to which the curve has been emitted
  bool tipAdded = false;
  Dim tol;
  int passes;
  Dim relax;
};

class SymmetricFIR : public InputProcessor
{
public:
  SymmetricFIR(int ncoeffs);

protected:
  void addPoint(const StrokePoint& pt) override;
  void removePoints(int n) override;
  void finalize() override;

private:
  void applyFilter(Dim in[], Dim out[], int N);

  std::vector<Dim> X;
  std::vector<Dim> Y;
  std::vector<Dim> P;
  //std::vector<StrokePoint> points;
  std::vector<Dim> coeffs;

  static Dim identity[];
  static Dim gaussian5[];
  static Dim gaussian11[];
  static Dim savitzky_golay5[];
  static Dim savitzky_golay11[];
};

#endif
