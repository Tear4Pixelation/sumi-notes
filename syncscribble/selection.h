#ifndef SELECTION_H
#define SELECTION_H

#include "page.h"

// Initially, I tried separate RectSelection, RuledSelection, etc
//  objects, but didn't like it
// To support multiple selections, we may need to have a single
//  selection object to which we add rectangles, lasso paths, etc.
class Selector;
class Clipboard;

class Selection {
public:
  Rect bbox;
  Page* page;
  ScribbleTransform transform;
  Element* sourceNode;
  std::list<Element*> strokes;
  // SELMODE_PASSIVE does not take ownership of strokes
  enum {SELMODE_REPLACE, SELMODE_UNION, SELMODE_PASSIVE, SELMODE_NONE} selMode = SELMODE_REPLACE;
  Selector* selector = NULL;
  Timestamp minTimestamp = MAX_TIMESTAMP;
  Timestamp maxTimestamp = 0;
  enum StrokeDrawType {STROKEDRAW_NORMAL, STROKEDRAW_SEL, STROKEDRAW_NONE};

  Selection(Page* source, StrokeDrawType drawtype = STROKEDRAW_SEL);
  ~Selection();  // destructor calls clear() to deselect all strokes

  template <class Predicate>
  Element* getFirstElement(Predicate pred)
  {
    // previously, we descended into groups to find a matching stroke
    auto it = find_if(strokes.begin(), strokes.end(), pred);
    return it != strokes.end() ? *it : NULL;
  }

  void addStroke(Element* s, Element* next = NULL);
  bool removeStroke(Element* s);

  void clear();
  void deleteStrokes();
  void selectAll();
  void invertSelection();
  void selectbyProps(Element* elproto, bool useColor, bool useWidth);
  // the ruling the ruled operations below work in - the page's own, or a ruling region's; set by
  //  whoever starts a ruled gesture, from Page::rulingAt() at the press point
  RulingFrame ruling;
  int sortRuled();
  void insertSpace(Dim dx, int dline);
  void reflowStrokes(Dim dx, int dline, Dim minWordSep);
  // measured from the ink as it was before the gesture, on the first reflowStrokes() call: where the
  //  paragraph's text starts (MAX_DIM if unknown) and the writer's gap between words (0 if unknown); with
  //  skippedLineFrame() these are per text line, so "the line above" is the previous text line
  bool reflowMeasured = false;
  Dim reflowIndent = MAX_DIM;
  Dim reflowWordGap = 0;
  void measureReflowInk(Dim minWordGap);
  int count() const { return int(strokes.size()); }
  void recalcTimeRange();
  void invalidateBBox() { bbox = Rect(); }
  // the default, STROKEDRAW_NONE uses the current draw type (since none doesn't make sense)
  void draw(Painter* painter, StrokeDrawType drawtype = STROKEDRAW_NONE);  //const Rect& dirty = Rect());

  void translate(Dim dx, Dim dy);
  void setOffset(Dim x, Dim y);
  void setOffset(Point p) { setOffset(p.x, p.y); }
  Point getOffset() const { return Point(transform.xoffset(), transform.yoffset()); }
  void rotate(Dim radians, Point origin);
  void scale(Dim sx, Dim sy, Point origin, bool scalewidths = false);
  void applyTransform(const ScribbleTransform& tf);
  void resetTransform();
  void stealthTransform(const ScribbleTransform& tf);
  void setStrokeProperties(const StrokeProperties& props);
  StrokeProperties getStrokeProperties() const;
  bool containsGroup();
  void ungroup();
  void toSorted(Clipboard* dest) const;

  //void growDirtyRect(const Rect& r);
  Rect getBBox();
  Rect getBGBBox();
  void setZoom(Dim zoom);
  void shrink();
  void commitTransform();
  // by default, nothing to draw (e.g. PathSelection)
  void drawBG(Painter* painter);
  void setDrawType(StrokeDrawType newtype);
  StrokeDrawType drawType() const { return m_drawType; }
  bool xchgBGDirty(bool b);

//private:
  void doSelect();

  StrokeDrawType m_drawType;
};

// for text written on every second line: a frame whose lines are the line at pos and every second one from it
RulingFrame skippedLineFrame(const RulingFrame& frame, Point pos);
// the same with its first text line given as a line of `frame` (Insert Lines picks it, see insertLinesStart())
RulingFrame skippedLineFrame(const RulingFrame& frame, int line);

// clipboard owns its strokes, unlike Selection
class Clipboard
{
public:
  std::unique_ptr<SvgDocument> content;

  Clipboard() : content(new SvgDocument) {}
  Clipboard(SvgDocument* doc) : content(doc) {}
  int count() const { return content->children().size(); }
  void addStroke(Element* s) { content->addChild(s->node); }
  void paste(Selection* sel, bool move);
  void replaceIds() { content->replaceIds(); }
  void saveSVG(std::ostream& file);
};

// Selection object manages the Selector - it will delete selector in its destructor

class Selector
{
public:
  Selection* selection;

  Selector(Selection* _sel);
  virtual ~Selector();

  virtual bool selectHit(Element* s) = 0;
  virtual Rect getBGBBox() { return Rect(); }
  virtual void shrink() {}
  virtual void drawBG(Painter* painter) {}
  virtual void setZoom(Dim zoom) {}
  virtual void transform(const Transform2D& tf) {}

  virtual Point scaleHandleHit(Point pos, bool touch) { return Point(NaN, NaN); }
  // index into the shape handle list, or -1; only ShapeSelector ever returns a hit
  virtual int shapeHandleHit(Point pos, bool touch) { return -1; }
  virtual Point rotHandleHit(Point pos, bool touch) { return Point(NaN, NaN); }
  virtual Point cropHandleHit(Point pos, bool touch) { return Point(NaN, NaN); }

  bool drawHandles = true;
  bool bgDirty = false;

protected:
  Color bgStroke;
  Color bgFill;

};

class PathSelector : public Selector
{
public:
  PathSelector(Selection* _sel) : Selector(_sel) {}

  void selectPath(Point p, Dim radius);
  bool selectHit(Element* s) override;
  Rect getBGBBox() override;
  void drawBG(Painter* painter) override;

private:
  Path2D path;

  Point selPos = Point(NaN, NaN);
  Point selPrev = Point(NaN, NaN);
  Dim selRadius;
};

class RectSelector : public Selector
{
public:
  // RECTSEL_BBOX takes what lies entirely inside the rect, RECTSEL_ANY what it touches
  enum {RECTSEL_BBOX, RECTSEL_COM, RECTSEL_ANY} rectSelMode = RECTSEL_BBOX;
  bool enableCrop = false;

  RectSelector(Selection* _sel, Dim zoom = 1, bool handles = true)
      : Selector(_sel), mZoom(zoom) { drawHandles = handles;  }
  void selectRect(Dim x0, Dim y0, Dim x1, Dim y1);
  Rect rect() const { return selRect; }
  bool selectHit(Element* s) override;
  void shrink() override;
  Rect getBGBBox() override;
  void drawBG(Painter* painter) override;
  void setZoom(Dim zoom) override;
  void transform(const Transform2D& tf) override;

  Point scaleHandleHit(Point pos, bool touch) override;
  Point rotHandleHit(Point pos, bool touch) override;
  Point cropHandleHit(Point pos, bool touch) override;

  static Dim HANDLE_PAD;

private:
  Rect selRect;
  Dim mZoom;
  static Dim HANDLE_SIZE;
};

// selector for a selection holding exactly one parametric shape (SHAPES_SPEC.md 5): instead of the
//  scale/rotate handles of a RectSelector, it offers one handle per defining parameter
class ShapeSelector : public Selector
{
public:
  ShapeSelector(Selection* _sel, Dim zoom = 1) : Selector(_sel), mZoom(zoom) { updateHandles(); }

  bool selectHit(Element* s) override;
  void shrink() override;
  Rect getBGBBox() override;
  void drawBG(Painter* painter) override;
  void setZoom(Dim zoom) override { mZoom = zoom; }
  void transform(const Transform2D& tf) override;
  int shapeHandleHit(Point pos, bool touch) override;

  // the element being edited, or NULL if the selection is no longer a single shape
  Element* shapeElement() const;
  const std::vector<ShapeHandle>& handles() const { return m_handles; }
  void updateHandles();
  // descriptor points are node-local; handle positions are in page coordinates
  Point toLocal(Point pagepos) const;

  static Dim HANDLE_SIZE;

private:
  std::vector<ShapeHandle> m_handles;
  Rect selRect;
  Dim mZoom;
};

// x0/x1 and ymin/ymax are in the frame's local coordinates, where its lines are horizontal
// Selector for a ruling region, reached only through the region's "..." button.  The Selection holds
//  the region *and* the ink inside it (bbox centre in the outline), drawn as ordinary ink: moving,
//  rotating or scaling the region carries its handwriting with it through the usual Selection
//  transforms, whose undo items already know how to transform a region (Element::applyTransform).
//  What the handles do:
//   - a corner (shapeHandleHit index 0..n-1) reshapes the outline only - the lines and the ink stay put
//   - the red origin handle (index n) slides the lines, to line them up with a scan's
//   - the rotate handle above the top edge rotates everything; the grip past the bottom-right corner
//     scales everything, uniformly (a region's ruling cannot stretch one way only)
class RegionSelector : public Selector
{
public:
  RegionSelector(Selection* _sel, Element* _region, Dim zoom = 1) : Selector(_sel), region(_region), mZoom(zoom) {}

  bool selectHit(Element* s) override;
  Rect getBGBBox() override;
  void drawBG(Painter* painter) override;
  void setZoom(Dim zoom) override { mZoom = zoom; }
  void transform(const Transform2D& tf) override { bgDirty = true; if(handleMoved) handlePage = tf.map(handlePage); }
  int shapeHandleHit(Point pos, bool touch) override;
  Point rotHandleHit(Point pos, bool touch) override;
  // Where the red origin handle sits.  By default on the left edge at the first line, but the small grip
  //  beside it (moveGripIndex()) relocates it anywhere inside the outline, onto something recognisable in
  //  the content to line the ruling up with.  Relocating changes no parameter and makes no undo item.
  //  Transient: lives in this selector (page coordinates), so it is gone when the region is deselected.
  Point originHandlePos() const;
  Point moveGripPos() const;
  int moveGripIndex() const { return originHandleIndex() + 2; }
  void setHandlePos(Point pos) { handlePage = pos; handleMoved = true; bgDirty = true; }
  bool handleMoved = false;
  Point handlePage;
  // the size handle is a shape handle (resizeHandleIndex()), not a scale handle: see resized()
  Point scaleHandleHit(Point pos, bool touch) override { return Point(NaN, NaN); }

  int originHandleIndex() const { return int(region->regionParams().corners.size()); }
  int resizeHandleIndex() const { return originHandleIndex() + 1; }
  // the outline of `start` stretched about its top-left corner (in its own frame) by a size handle drag
  //  from startPos to pos; the ruling and the ink stay where they are
  RulingRegionParams resized(const RulingRegionParams& start, Point startPos, Point pos) const;
  // the ink a region carries: its bbox centre is inside the outline
  static bool carries(const RulingRegionParams& params, Element* s);

  Point scaleHandlePos() const;

  Element* region;
  static Dim HANDLE_SIZE;

private:
  Point rotHandlePos() const;
  Dim mZoom;
};

struct RuledRange
{
  RulingFrame frame;
  Dim x0;
  Dim ymin;
  Dim x1;
  Dim ymax;
  Dim xruling;
  Dim yruling;
  std::vector<Dim> lstops;
  std::vector<Dim> rstops;

  RuledRange(Dim _x0, Dim _ymin, Dim _x1, Dim _ymax, Dim _xruling, Dim _yruling,
      const RulingFrame& f = RulingFrame()) :
      frame(f), x0(_x0), ymin(_ymin), x1(_x1), ymax(_ymax), xruling(_xruling), yruling(_yruling) {}

  bool isSingleLine() const;
  bool isValid() const;
  RuledRange& translate(Dim dx, Dim dy);

  Dim lstop(int idx) const;
  Dim rstop(int idx) const;
  // clamped: "to the end" is MAX_LINE_NUM (INT_MAX) lines, so a range starting above line 0 - a press just
  //  outside a region's top edge - spans one more than an int holds, and the overflow selected nothing
  int nlines() const { Dim n = (ymax - ymin)/yruling; return n >= Dim(INT_MAX) ? INT_MAX : int(n); }
};

class RuledSelector : public Selector
{
public:
  enum SelMode {SEL_CONTAINED, SEL_OVERLAP} selMode;
  enum ColMode {COL_NONE=0, COL_NORMAL=1, COL_AGGRESSIVE=2} colMode;
  std::vector<Dim> lstops;
  std::vector<Dim> rstops;
  // a ruled gesture acts only on ink of its own ruling (region or page); display-only callers that just
  //  want "what is on this line" (the bookmark list) turn it off
  bool sameRulingOnly = true;

  RuledSelector(Selection* _sel, ColMode cm = COL_NORMAL) : Selector(_sel), selMode(SEL_CONTAINED),
      colMode(cm), selRange(MAX_DIM, MAX_DIM, MIN_DIM, MIN_DIM, 0, 0) {}
  void selectRuled(Dim x0, int line0, Dim x1, int line1);
  void selectRuled(const RuledRange& range);
  // selections for ruled insert space
  void selectRuledAfter(Dim x, int linenum);
  //void selectRuledBelow(int linenum);
  void findStops(Dim x, int linenum);
  bool selectHit(Element* s) override;
  void shrink() override;
  Rect getBGBBox() override;
  void drawBG(Painter* painter) override;

  static Dim MIN_DIV_HEIGHT;

private:
  RuledRange selRange;
};

// Note that a base class (here, Selector) destructor must be virtual if any derived classes so much as
//  contain members which could allocate memory.  In this case, LassoSelector contains Stroke which contains
//  vector<Points>, which may dynamically allocate memory (to grow the vector).  Without a virtual destructor,
//  "delete (Selector*)selector" will call ~Selector() so the destructors for Stroke and hence vector<> never
//  get called, resulting in a memory leak.

class LassoSelector : public Selector
{
public:
  // take everything the lasso touches rather than only what it encloses
  bool touching = false;

  LassoSelector(Selection* sel, Dim simplify = 0);
  //void selectRect(Dim x0, Dim y0, Dim x1, Dim y1);
  void addPoint(Dim x, Dim y);
  bool selectHit(Element* s) override;
  //void shrink();
  Rect getBGBBox() override;
  void drawBG(Painter* painter) override;

  const Path2D& path() const { return lasso; }
private:
  Path2D lasso;
  Rect lassoBBox;

  int nextSimpStart = 0;
  Dim simplifyThresh;
  bool checkCollision = false;
  Point norm1, norm2, norm3;
  Dim txmin, txmax, tymin, tymax;
  Dim s1min, s1max, s2min, s2max, s3min, s3max;
  bool projectRect(const Point& p, const Rect& r, Dim smin, Dim smax);
  bool rectCollide(const Rect& r);
  //bool encloses(const PainterPath& path) const;
};

#endif // SELECTION_H
