#pragma once

#include "scribbleview.h"
#include "document.h"
#include "selection.h"
#include "nightmode.h"

struct Timer;

struct UIState {
  enum UIChangeFlags { SetDoc = 0, SaveDoc, InsertDoc, InsertPage, DeletePage, MovePage, SetPageProps,
      Command, ReleaseEvent, PenRelease, CancelEvent, SyncEvent, Zoom, Pan, PageSizeChange, PageNumChange,
      SelChange, SetSelProps, Paste, ClipboardChange };

  bool activeSel;
  bool pageSel;
  bool selHasGroup;
  // a ruling region is selected: the ink-only selection actions (cut, copy, duplicate, link) do not apply
  bool regionSel = false;
  bool pagemodified;
  bool nextView;
  bool prevView;
  bool syncActive;
  bool rxOnly;  // true if SWB active and enableTX false
  int pageNum;
  int totalPages;
  int currPageStrokes;
  int currSelStrokes;
  Dim pageWidth;
  Dim pageHeight;
  Dim zoom;
  Timestamp minTimestamp;
  Timestamp maxTimestamp;
};

class ScribbleDoc;
class ScribbleApp;
struct SDL_Cursor;
struct SDL_Cursor_Deleter;

class ScribbleArea : public ScribbleView
{
#ifdef SCRIBBLE_TEST
  friend class ScribbleTest;
#endif
  friend class LinkDialog;
  // the outline sidebar navigates to a page, exactly as a link does (sidebar.cpp)
  friend class Sidebar;
  friend class ScribbleDoc;
public:
  ScribbleArea();
  //~ScribbleArea();

  void loadConfig(ScribbleConfig* _cfg) override;
  Page* getCurrPage() const { return currPage; }
  int getCurrPageNum() const { return currPageNum; }
  void createHyperRef(Element* b, const StrokeProperties* props = NULL) { setSelProperties(props, NULL, b); }
  void createHyperRef(const char* target, const StrokeProperties* props = NULL) { setSelProperties(props, target); }
  const char* getHyperRef() const;
  const char* getIdStr() const;
  void setSelProperties(const StrokeProperties* props, const char* target = NULL,
      Element* bkmktarget = NULL, const char* idstr = NULL, bool forcenormal = false);
  // returns where the image went, in document coordinates; passing that back as `below` places the next
  //  image underneath it rather than on top of it (scanning several pages with Add more)
  Rect insertImage(Image image, const Rect& below = Rect()); //, bool lossy = false);
  // Screenshot (docs/agent/screenshot.md): the area the last selection gesture covered stays marked,
  //  dashed, until the selection is cleared - even when the gesture caught no ink
  bool hasShotRegion() const { return !shotRegion.empty(); }
  void clearShotRegion();
  void screenshotSelection();
  void captureScreenshot();
  void dirtyShotRegion();

  // some of these need to be made private
  DocPosition getPos() const;
  // put the view back where an editor tab left it (editortabs.cpp): zoom, then page and corner position
  void restoreView(int pagenum, Point pos, Dim zoom);
  void doGotoPos(int pagenum, Point pos, bool exact = true);
  void doCommand(int itemid);
  bool doTimerEvent(Timestamp t) override;
  void reset();

  bool recentStrokeSelect();
  bool recentStrokeDeselect();
  void recentStrokeSelDone();
  void zoomCenter(Dim newZoom, bool snap = false);

  void setViewBox(const DocViewBox &vb);
  DocViewBox getViewBox() const;

  enum viewmode_t {VIEWMODE_SINGLE=0, VIEWMODE_VERT=1, VIEWMODE_HORZ=2};
  enum PasteFlags {PasteOrigPos=0, PasteCenter=2, PasteCorner=4, PasteOrigin=8, PasteOffsetExisting=0x10,
      PasteRulingShift=0x20, PasteUndoable=0x40, PasteMoveClipboard=0x80, PasteNoHandles=0x100};

  static Image* watermark;  // only for iOS IAP
  // made public to support multi-doc split
  ScribbleDoc* scribbleDoc = NULL;
  ScribbleApp* app = NULL;
  int strokeCounter = 0;
  void updateUIState(UIState* state);
  bool hasSelection() const { return currSelection != NULL; }
  const Selection* selection() const { return currSelection; }
  void setStrokeProperties(const StrokeProperties& props, bool undoable = true);
  // dashStyle, if given, receives the selection's ScribblePen::DASH_* (DASH_MIXED if it has several)
  ScribblePen getPenForSelection(int* dashStyle = NULL) const;
  // accept external selection
  virtual bool selectionDropped(Selection* clip, Point globalPos, Point offset, bool replaceids = false);
  // for file dropped from OS
  bool clipboardDropped(Clipboard* selection, Point globalPos, Point offset);

protected:
  void doPressEvent(const InputEvent& event) override;
  void doMoveEvent(const InputEvent& event) override;
  void doReleaseEvent(const InputEvent& event) override;
  bool doClickAction(Point pos) override;
  void doDblClickAction(Point pos) override;
  void doTwoFingerTap() override;
  bool doTouchTap(Point pos) override;
  void doMotionEvent(const InputEvent& event, inputevent_t eventtype) override;
  void doCancelAction(bool refresh = true) override;

  Page* page(int n) const;
  int numPages() const;
  const ScribblePen* currPen() const;
  // `at` picks the ruling whose line height a relative width is measured against (a ruling region's,
  //  if it lies in one); without it, the page's
  ScribblePen resolvedPen(Point at = Point(NaN, NaN)) const;

  void setPageNum(int pagenum);
  void nextPage(bool appendnew = false);
  void prevPage();
  void selectAll(Selection::StrokeDrawType drawtype = Selection::STROKEDRAW_SEL);
  void selectSimilar();
  void invertSelection();
  void deleteSelection();
  void ungroupSelection();
  void doPaste(Clipboard* clipboard, const Page* srcpage, int flags = 0);
  void doPasteAt(Clipboard* clipboard, Point pos, PasteFlags flags);
  void expandDown();
  void expandRight();
  void prevView();
  void nextView();
  void viewPos(int pagenum, Point pos);
  void gotoPos(int pagenum, Point pos, bool savepos = true);
  void gotoPage(int pagenum);
  void viewRect(int pagenum, const Rect& r);
  bool viewHref(const char* href);
  // Commit any in-progress multi-point shape; safe to call in any mode (SHAPES_SPEC.md 4).
  // select=true only for the gestures where the *user* finished the shape (tapping the last or first
  //  point).  Every other caller is finishing it as a side effect of something else starting - another
  //  gesture, a tool switch, a page change - and must not leave a selection behind, because the caller
  //  may be about to install its own (doPressEvent's mode switch assigns currSelection directly, having
  //  already done its clearSelection() pass by the time we get here).
  void finishShape(bool select = false);
  // apply the shape options row's toggles to a selected shape; false if the selection isn't one shape
  bool setSelShapeOptions(int flags, Dim radius);

  bool uiDirty;
  void uiChanged(int reason);
  void roundZoom(Dim px, Dim py) override;
  // fit width: the page exactly as wide as the view, no margin (see docs/agent/navigation.md)
  Dim fitWidthZoom(int pagenum) const;
  bool snapsToFitWidth(Dim zoom, Dim wzoom) const;
  bool nearFitWidth() const override;
  void wheelZoomFinish(Dim px, Dim py) override;
  void alignFitPage(int pagenum, bool fitWidth);
  void keepPageAtMiddle(int pagenum);
  void doPan(Dim dx, Dim dy) override;
  void doRefresh() override;
  void pageSizeChanged() override;
  void pageCountChanged(int pagenum, int prevpages = -1);

  void invalidateStroke(Element* s);
  void invalidatePage(Page* p);
  void setPageDims(Dim width, Dim height, bool pending = false);
  void growPage(Rect bbox, Dim maxdx = -1, Dim maxdy = -1, bool pending = false);
  void reqRepaint();
  void drawStrokeOnImage(Element* stroke);
  bool clearSelection();
  bool clearTempSelection();
  void groupStrokes(Element* b = NULL);
  // Insert space moves whatever lies past the press, and the ink inside a region is part of that; the
  //  region is not ink, so doSelect() never takes it, and without this the ink would slide out from
  //  under its lines.  Adds every region whose bbox satisfies `past` to `sel`.
  void addRegionsToInsertSpace(Selection* sel, const std::function<bool(const Rect&)>& past);
  int selectionHit(Point pos, bool touch);

  void viewSelection();
  void freeErase(Point prevpos, Point pos);
  void freeEraseRuled(Dim xmin, Dim xmax, int line);
  bool saveCurrPos(int newpagenum, Point newpos);

  Point getPageOrigin(int pagenum) const;
  int dimToPageNum(const Point& pos) const;
  Point dimToPageDim(const Point& p) const;
  Rect dimToPageDim(Rect r) const;
  Point pageDimToDim(const Point& p) const;
  Rect pageDimToDim(Rect r) const;
  bool isPageVisible(int pagenum);
  void dirtyScreen(const Rect &dirty);
  void dirtyPage(int pagenum, Rect dirty);

  void updateContentDim();
  bool updateHorzPanLock();
  // the page at the middle of the view, which becomes the current page on a scroll or zoom (doPan())
  int dominantPageNum() const;
  void drawThumbnail(Image* dest);
  void drawWatermark(Painter* painter, Page* page, const Rect& dirty);  // for iOS IAP
  void drawImage(Painter* imgpaint, const Rect& dirty) override;
  void drawScreen(Painter* painter, const Rect& dirty) override;
  const ColorMap* viewColorMap() override;
  NightColorMap nightMap;

  int currMode;
  Point prevPos;
  Point initialPos;
  Point rawPos;
  Point prevRawPos;
  Point releasePos = { NAN, NAN };
  int prevLine;
  int initialLine;
  Rect initialPageSize;
  Dim prevXScale;
  Dim prevYScale;
  bool scaleLockRatio;
  Timestamp prevHoverDrawTime;
  Rect hoverRect;
  int cursorMode = 0;  // CURSORMODE_SYSTEM
  int drawCursor = 0;
  Dim lineDrawPressure = 1;
  Dim centerLineLocalY = 0;  // local y (in gestureFrame) of a CENTER_ON_LINE stroke, fixed at the press
  // The ruling a gesture works in, fixed at the press: a ruling region's if the press lands in one, else
  //  the page's.  Every ruled mode reads lines and along-the-line positions from this, never from the
  //  page directly, so a gesture that wanders out of a region keeps the region's lines.
  RulingFrame gestureFrame;
  // gestureFrame without the blank-page phase, for snapping to the grid (the page's grid is anchored at
  //  its origin, not at the press)
  RulingFrame gridFrame;
  bool showHelpTips = false;

  // for erase ruled
  int eraseCurrLine;
  Dim eraseXmax;
  Dim eraseXmin;
  // for insert space
  bool insertSpaceX;
  // which ruled insert space tool started the gesture: MODE_INSSPACEDOWN (lines only), MODE_INSSPACERIGHT
  //  (along the line only) or MODE_INSSPACERULED (both); the gesture itself runs as MODE_INSSPACERULED
  int insSpaceAxis = MODE_INSSPACERULED;
  // what ruled insert space moves, in gestureFrame: everything after local x insSpaceSelX on line
  //  insSpaceSelLine (MIN_DIM: the whole line).  The press's own line and x, except where Insert Lines picks
  //  another start (insertLinesStart()); insSpaceEraseX is where Insert Lines dragged up starts erasing on
  //  the line its text lands on
  int insSpaceSelLine = 0;
  Dim insSpaceSelX = 0;
  Dim insSpaceEraseX = 0;
  // the pen's local x at the press: the side of a vertical line (column stop) Insert Lines keeps to when it
  //  moves whole lines (insSpaceSelX == MIN_DIM)
  Dim insSpaceColX = 0;
  // for resize selection
  Point scaleOrigin;
  Dim bookmarkSnapX;

  viewmode_t viewMode;
  bool centerPages;
  Dim reflowWordSep;
  RuledSelector::ColMode selColMode = RuledSelector::COL_NORMAL;
  Dim pageSpacing;
  Dim currPageXOrigin = 0;
  Dim currPageYOrigin = 0;
  Dim contentHeight = 0;
  Dim contentWidth = 0;
  // horizontal scroll limits before updateHorzPanLock() pins them at or below fit width
  Dim freeMinOriginX = 0;
  Dim freeMaxOriginX = 0;

  // we'll only display one page at a time for now (like OneNote)
  int currPageNum = INT_MAX;
  Page* currPage = NULL;
  // set while an explicit navigation (go to page, bookmark, ...) pans, so the page it chose stays current
  bool holdPageNum = false;
  // the view (in Dim) when doPan() last chose the current page - it only chooses again once the view moves
  Rect pageChoiceView;
  Selection* tempSelection = NULL;
  Selection* currSelection = NULL;
  // selection for stroke fragments produced by free eraser
  Selection* freeErasePieces = NULL;
  int currSelPageNum = 0;
  // lasso or rectangle of the last selection gesture, in page coordinates of shotRegionPage
  Path2D shotRegion;
  int shotRegionPage = -1;
  float shotRegionDashes[3];
  Rect selBGRect;

  PathSelector* pathSelector = NULL;
  RuledSelector* ruledSelector = NULL;
  RectSelector* rectSelector = NULL;
  ShapeSelector* shapeSelector = NULL;
  LassoSelector* lassoSelector = NULL;
  RuledSelector* insSpaceEraseSelector = NULL;
  Selection* insSpaceEraseSelection = NULL;
  Element* currStroke = NULL;
  // shape tool: multi-point shapes are the only gesture that outlives one press->move->release cycle
  Element* shapeInProgress = NULL;
  int shapeHandleIdx = -1;
  ShapeParams shapeHandleStart;
  // ruling regions (rulingregion.h): selected only through the "..." button in their bottom-right corner
public:
  RegionSelector* regionSelector = NULL;
  Element* selectedRegion() const;
  void selectRegion(Element* region);
  // edit the selected region's ruling (region panel); one undo step
  void setSelRegionParams(const RulingRegionParams& params);
  // show `params` on the selected region without recording anything - for a slider drag, which is
  //  committed with setSelRegionParams once it ends
  void previewSelRegionParams(const RulingRegionParams& params);
  // the selected region's outline and this view, in window coordinates, to place the region panel
  Rect selRegionGlobalRect() const;
  Rect globalViewRect() const;
  // create a region over `r` on the current page, with the page's ruling, and select it
  Element* addRulingRegion(const Rect& r);

  // Page tags (docs/agent/page-tags.md).  Tags picked in the toolbar's tag popup ride on the pointer until
  //  a press puts them on the page under it (one undo step); Esc drops them.  `tags` is {id, name}; `todo`
  //  gives each a checkbox.
  void startTagPlacement(const std::vector<std::pair<std::string, std::string>>& tags, bool todo = false);
  void cancelTagPlacement();
  bool placingTags() const { return !pendingTags.empty(); }
  size_t numPendingTags() const { return pendingTags.size(); }
  bool capturesPointer(const InputEvent& event) const override;
  // tick or untick a to-do tag on the current page, as one undo step: a ticked to-do stays on the page but
  //  no longer tags it.  Returns the element now on the page.
  Element* toggleTodoTag(Element* tag);
  // scroll to the tags with these ids on the current page and pulse them, to show where they are (a page
  //  card was opened)
  void flashPageTags(const std::vector<std::string>& tagIds);
protected:
  // the tags being placed: not in any page, painted by drawScreen with their anchor at pendingTagsPos
  std::vector<Element*> pendingTags;
  Point pendingTagsAnchor;  // the point of the stack that sits under the pointer, in the stack's own coords
  Point pendingTagsPos;  // the pointer, in the current page's coordinates
  bool placingPressed = false;
  Rect pendingTagsRect() const;  // current page coordinates
  void movePendingTags(Point pos);
  void placePendingTags(bool select);
  // a press on a to-do tag's checkbox; the release ticks it if it is still on the box
  Element* todoPressed = NULL;
  Element* todoBoxHit(Point pos, bool touch) const;  // current page coordinates
  std::vector<Rect> flashRects;  // page coordinates on flashPageNum
  int flashPageNum = -1;
  int flashTicks = 0;  // frames left of the pulse animation, counting down
  Timer* flashTimer = NULL;
  RulingRegionParams regionHandleStart;
  Point regionHandleStartPos;
  Point regionHandleStartHandle;
  // the region under construction by the region tool (a drag, like the box shape)
  Element* regionInProgress = NULL;
  bool regionButtonsShown() const;
  Point regionButtonPos(const Element* region) const;
  Element* regionButtonHit(Point pos, bool touch) const;
  void drawRegionButtons(Painter* painter);
  // re-decide which ink a selected region carries - at the start of every gesture on it, since a corner
  //  drag or new writing changes that
  void refreshRegionSelection();
  ShapeParams newShapeParams(int shapeid, Point pos) const;
  void editShapeAfterDraw(Element* shape);
  Element* touchTapTarget(Point pos) const;
  void selectTapped(Element* target);
  Element* createShapeElement(const ShapeParams& params);
  void cancelShape();
  Point snapShapePoint(Point pos) const;
  // soft 45 degree snap of point index of a line or polyline (shapeAngleSnap); see snapShapeAngle().
  //  Drawing with the shape tool and dragging a handle snap by distance from the axis line
  //  (SHAPE_ANGLE_SNAP_DIST screen units, so it follows zoom), capped at shapeAngleSnap degrees;
  //  localPerPage converts page units to the params' units (a transformed shape's node-local ones).
  //  A recognized (hold-to-snap) line snaps by angle alone, at a fraction of shapeAngleSnap.
  Point snapShapeAngleAt(const ShapeParams& params, int index, Point pos, Dim localPerPage = 1) const;
  Point snapRecognizedAngleAt(const ShapeParams& params, int index, Point pos) const;
  // swap in a ShapeSelector when the settled selection is exactly one shape (spec 5)
  bool useShapeSelector();

  // hold-to-snap: a pen stroke held still for shapeSnapDelay seconds is recognized (shaperec.h) and
  //  replaced by a line, box or ellipse that the rest of the gesture scales; a scratch-out erases.
  //  While a snapped shape is live it is held in currStroke, so it is painted and cleaned up exactly
  //  like the shape tool's drag gesture.
  Timer* snapTimer = NULL;
  std::vector<Point> snapSamples;  // the stroke's raw input in page coordinates
  Point snapHoldPos;  // where the pen has been (roughly) still since snapHoldStart
  Timestamp snapHoldStart = 0;
  bool snapHoldUsed = false;  // this hold already ran the recognizer; wait for the pen to move on
  bool snapActive = false;  // currStroke is a snapped shape following the pen
  ShapeParams snapParams;  // the shape as recognized, before the gesture scaled it
  Point snapAnchor;  // pen position when the shape snapped
  void startShapeSnap(Point pos);
  void trackShapeSnap(Point pos);
  void stopShapeSnap();
  // returns false once there is nothing left to wait for, which stops the timer
  bool checkShapeSnap(Timestamp now);
  bool snapStroke();
  bool scratchOutOnLift(Selection& erased);
  void discardStrokeBuilder();
  void scaleSnapShape(Point pos);
  void commitSnapShape();
  int scratchOut(const std::vector<Point>& area, bool ownAction = true);

  // for groupStrokes
  std::vector<Element*> recentStrokes;
  Dim strokeGroupYCenter = 0;
  typedef std::vector<Element*>::iterator RecentStrokesIter;

  // for back/fwd navigation
  std::vector<DocPosition> posHistory;
  std::vector<DocPosition>::iterator posHistoryPos;

  // experimental feature to select N most recent strokes
  int recentStrokeSelPos = -1;
#if !PLATFORM_MOBILE
  static bool staticInited;
  static std::unique_ptr<SDL_Cursor, SDL_Cursor_Deleter> penCursor;
  static std::unique_ptr<SDL_Cursor, SDL_Cursor_Deleter> panCursor;
  static std::unique_ptr<SDL_Cursor, SDL_Cursor_Deleter> eraseCursor;
#endif
  // some constants
  static const Dim ERASESTROKE_RADIUS;
public:
  // radius of the free eraser, in screen units; settable from the eraser options row
  static Dim ERASEFREE_RADIUS;
protected:
  static const Dim PATHSELECT_RADIUS;
  static const Dim MIN_LASSO_POINT_DIST;
  static const Dim GROW_STEP;
  static const Dim GROW_TRIGGER;  // in multiples of GROW_STEP or ruling
  static const Dim GROW_EXTRA;  // in multiples of GROW_STEP or ruling
  static const Dim AUTOSCROLL_BORDER;
  static const Dim MIN_CURSOR_RADIUS;
public:
  // must match --canvas in ugui/theme.cpp; set from ScribbleApp::loadConfig()
  static const Color BACKGROUND_COLOR_DARK;
  static const Color BACKGROUND_COLOR_LIGHT;
  static Color BACKGROUND_COLOR;
  // the canvas's night mode map, or NULL - for UI that shows document colors (pen swatches)
  const ColorMap* nightColorMap() { return viewColorMap(); }
};
