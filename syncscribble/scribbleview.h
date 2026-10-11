#pragma once

#include <deque>
#include <memory>
#include "ulib/painter.h"
#include "scribbleconfig.h"
#include "scribblemode.h"
#include "scribbleinput.h"
#include "canvaslayer.h"

struct InputPoint;
class ScribbleWidget;

class ScribbleView
{
friend class ScribbleInput;
public:
  ScribbleView();
  virtual ~ScribbleView();

  virtual void loadConfig(ScribbleConfig* _cfg);
  //Dim getPreScale() const { return preScale; }
  void scrollBy(int dx, int dy);
  void scrollFrac(Dim dx, Dim dy);
  Point getScrollFrac();

  Dim getZoom() const { return mZoom; }
  Dim getScale() const { return mScale; }
  int getViewWidth() const { return screenRect.right; }
  int getViewHeight() const { return screenRect.bottom; }
  void repaintAll(bool imagedirty = true);

  virtual void doPressEvent(const InputEvent& event) {}
  virtual void doMoveEvent(const InputEvent& event) {}
  virtual void doReleaseEvent(const InputEvent& event) {}
  virtual bool doClickAction(Point pos) { return true; }
  virtual void doDblClickAction(Point pos) {}
  virtual void doLongPressAction(Point pos) {}
  // two fingers tapped together without moving; the pan they started has already been cancelled
  virtual void doTwoFingerTap() {}
  // a single finger tapped without moving, where touch pans (a pen was detected) and no link took the tap
  virtual bool doTouchTap(Point pos) { return false; }
  virtual void doMotionEvent(const InputEvent& event, inputevent_t eventtype) {}
  // true while a single pointer must reach doPressEvent whatever the input mode - e.g. a finger that
  //  would otherwise pan places page tags instead (ScribbleArea::startTagPlacement()), or ticks a to-do tag
  virtual bool capturesPointer(const InputEvent& event) const { return false; }
  virtual void doCancelAction(bool refresh = true);
  virtual bool doTimerEvent(Timestamp t);
  virtual void doRefresh() { reqRepaint(); }

  void panZoomStart(const InputEvent& event);
  void panZoomMove(const InputEvent& event, int prevpoints, int nextpoints);
  void panZoomFinish(const InputEvent& event);
  void panZoomCancel();
  void doKineticScroll();
  void cancelScrolling();

  Rect dirtyRectScreen;
  Rect dirtyRectDim;
  std::unique_ptr<ScribbleInput> scribbleInput;
  ScribbleConfig* cfg = NULL;
  ScribbleWidget* widget = NULL;

  static Dim unitsPerPx;

  // called by SvgGui before each frame begins (Application::SDL_main): every shown view that uses a canvas
  //  layer brings it up to date for the frame - see prepareLayer()
  static void prepareLayers(Painter* p, const Rect& guiDirty);
  // tests: paint every view directly, leaving the canvas layers untouched (ScribbleTest::canvasLayerTest)
  static bool testDirectPaint;
  // tests: leave the float layer out of the frame, showing the canvas layer under it
  static bool testSkipFloat;
  // tests: direct drawing leaves the float content out too (what the canvas layer under it should hold)
  static bool testHideFloat;

protected:
  friend class ScribbleWidget;

  virtual void drawImage(Painter* imgpaint, const Rect& dirty) {}
  virtual void drawScreen(Painter* painter, const Rect& dirty) {}
  // after each paint: get ready what the next ones may need (ScribbleArea: page images to decode)
  virtual void prefetchImages() {}
  // render-time color transform for night mode; NULL draws the document's own colors
  virtual const ColorMap* viewColorMap() { return NULL; }
  virtual void doPan(Dim dx, Dim dy);
  virtual void pageSizeChanged();
  // a view keeping a retained copy of its drawImage() output, which a pan shifts instead of redrawing
  virtual bool usesCanvasLayer() const { return false; }
  // Float layer (prepareFloat()): content that moves as a whole for a while (ScribbleArea: a dragged
  //  selection) is drawn once into its own transparent layer and composited over the canvas layer at its
  //  current offset, rounded to whole device pixels; while drawingLayerContent is set, drawImage() must
  //  leave it out.  floatWanted() turns it on; changes to where it is go to overlayDirtyScreen, not the
  //  content dirty rects, since the canvas layer under it does not change.
  virtual bool floatWanted() { return false; }
  // where the float content is now, in Dim coordinates (including any stroke widening), and how far it
  //  has been moved, in Dim; drawFloat() draws it at its current position (painter as for drawImage())
  virtual Rect floatBoundsDim() { return Rect(); }
  virtual Point floatOffsetDim() { return Point(0, 0); }
  virtual void drawFloat(Painter* painter) {}
  // the float layer is drawn again by the next frame that uses it (and its memory freed if !keep)
  void resetFloat(bool keep = false);
  bool drawingLayerContent = false;
  // repainted by the GUI, but not drawn into the canvas layer: only the float layer or drawScreen()
  //  overlays changed there
  Rect overlayDirtyScreen;

  void doPaintEvent(Painter* qpainter, const Rect& dirty = Rect());
  void doResizeEvent(const Rect& newsize);

  static const Dim zoomSteps[];
  static const unsigned int DEFAULT_ZOOM_IDX;
  unsigned int zoomStepsIdx = 0;

  void setCornerPos(Point pos);
  void setCenterPos(Point pos);
  void setVisiblePos(Point pos);
  int nearestZoomStep(Dim zoom) const;
  // zoom stuff is only called via ScribbleArea
  void setZoom(Dim newZoom);
  void zoomTo(Dim newZoom, Dim px, Dim py);
  void zoomBy(Dim s, Dim px, Dim py);
  virtual void roundZoom(Dim px, Dim py);
  // true if a zoom gesture ending now would snap to fit width (judged for the page at the middle of the
  //  view, the one the snap fits) - drives the "Fit" toast
  virtual bool nearFitWidth() const { return false; }
  // Ctrl+wheel has no release: ScribbleWidget calls this once the wheel has paused
  virtual void wheelZoomFinish(Dim px, Dim py) {}
  void showFitHint(bool show);
  void zoomIn();
  void zoomOut();
  void resetZoom();

  Dim pointerDist(const std::vector<InputPoint>& points);
  Point dimToScreen(const Point& p) const;
  Rect dimToScreen(const Rect& r) const;
  Point screenToDim(const Point& p) const;
  Rect screenToDim(const Rect& r) const;
  //Rect imageToDim(const Rect& r) const;
  //Rect dimToImage(const Rect& r) const;
  Point screenToGlobal(const Point& p) const;
  Point globalToScreen(const Point& p) const;
  bool isVisible(const Rect& r) const;
  void reqRepaint();
  //void updateImage(Rect dirty);
  //void scrollImage();
  //Rect drawTextwithBG(Painter* p, const char* str, Dim x, Dim y, Color textcolor, Color boxcolor);

  //Image* contentImage = NULL;
  //Painter* imgPaint = NULL;
  //Painter* screenPaint = NULL;
  bool panning = false;
  Rect viewportRect;
  Rect screenRect;
  Point screenOrigin;
  Dim mZoom = 1;
  Dim preScale = 1;
  Dim mScale = 1;
  //Dim fontSize;
  Dim xorigin = 0;
  Dim yorigin = 0;
  Dim rawxoffset = 0;
  Dim rawyoffset = 0;
  Dim panxoffset = 0;
  Dim panyoffset = 0;
  Dim minOriginX = 0;
  Dim minOriginY = 0;
  Dim maxOriginX = 0;
  Dim maxOriginY = 0;
  // for FPS display
  int frameCount = 0;
  // for pan/zoom
  Point prevPointerCOM;
  Dim prevPointerDist = 0;
  Point initPanOrigin;
  Dim initPanZoom = 0;
  Dim currPanLength = 0;
  Dim maxPointerDist = 0;
  // for kinetic scrolling
  Point flingV;
  Point prevFlingV;
  Timestamp initPointerTime = 0;
  Timestamp prevPointerTime = 0;
  std::deque<InputPoint> prevInput;
  // kinetic scrolling constants
  Dim timerPeriod = 50;  // in ms

  static const int IMAGEBUFFER_BORDER;

  // retained canvas layer (prepareLayer()); a pan sets layerPanned instead of dirtying the whole screen
  bool layerPanned = false;
private:
  void prepareLayer(Painter* p, const Rect& guiDirty);
  bool prepareFloat(Painter* p);
  bool isShown() const;

  std::unique_ptr<CanvasLayer> layer;
  Rect layerDirtyDim;  // content changed since the layer was drawn, in Dim (pan-independent) coordinates
  bool layerValid = false;
  uint64_t layerFrame = 0;  // GUI frame the layer was last brought up to date for
  uint64_t layerDeferredFrame = 0;  // GUI frame drawn directly on purpose (not a pan), layer kept
  Dim paintedScale = 0, paintedPanX = 0, paintedPanY = 0;  // mScale and pan at the last doPaintEvent
  bool layerBuildNext = false;  // a pan wanted a full layer redraw and was drawn directly; the next builds it
  Transform2D layerDevXf;  // painter transform (screen -> device) the layer was drawn for
  Transform2D layerXf;  // screen -> layer pixels
  int layerX = 0, layerY = 0;  // device pixel of the layer's top left
  Rect layerInner;  // device pixels wholly inside the view: the layer is composited there, the fractional edges are drawn directly
  Dim layerPanX = 0, layerPanY = 0, layerScale = 0, layerXorigin = 0, layerYorigin = 0;
  const ColorMap* layerColorMap = NULL;
  int layerColorKey = 0;
  // float layer: drawn for floatXf/floatScale/color map at floatX/Y (device px) with the content's device
  //  anchor at floatAnchor; composited this frame (floatFrame) at floatCompX/Y
  std::unique_ptr<CanvasLayer> floatLayer;
  bool floatValid = false;
  bool floatEmpty = false;  // nothing of it is in the view: nothing to composite
  uint64_t floatFrame = 0;
  int floatX = 0, floatY = 0, floatCompX = 0, floatCompY = 0;
  Point floatAnchor;
  Transform2D floatXf;
  Dim floatScale = 0;
  const ColorMap* floatColorMap = NULL;
  int floatColorKey = 0;
  // painter transform and size at the last doPaintEvent, and how many paints in a row it has not changed
  Transform2D paintXf;
  Rect paintScreenRect;
  int paintStable = 0;

  static uint64_t guiFrame;
  static std::vector<ScribbleView*> allViews;
};
