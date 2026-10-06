#include "scribblearea.h"

#include "usvg/svgpainter.h"
#include "scribbledoc.h"
#include "scribbleapp.h"
#include "screenshot.h"
#include "scribblewidget.h"
#include "strokebuilder.h"
#include "bookmarkview.h"
#include "shaperec.h"
#include <fstream>


const Dim ScribbleArea::ERASESTROKE_RADIUS = 7;
Dim ScribbleArea::ERASEFREE_RADIUS = 7;
const Dim ScribbleArea::PATHSELECT_RADIUS = 7;
const Dim ScribbleArea::MIN_LASSO_POINT_DIST = 2;
const Dim ScribbleArea::GROW_STEP = 40;
const Dim ScribbleArea::GROW_TRIGGER = 1.625;  // in multiples of GROW_STEP or ruling
const Dim ScribbleArea::GROW_EXTRA = 2.5;  // in multiples of GROW_STEP or ruling
const Dim ScribbleArea::AUTOSCROLL_BORDER = 60;
const Dim ScribbleArea::MIN_CURSOR_RADIUS = 2;
const Color ScribbleArea::BACKGROUND_COLOR_DARK = 0xFF333333;
const Color ScribbleArea::BACKGROUND_COLOR_LIGHT = 0xFFBBBBBB;
Color ScribbleArea::BACKGROUND_COLOR = ScribbleArea::BACKGROUND_COLOR_DARK;

Image* ScribbleArea::watermark = NULL;
#if !PLATFORM_MOBILE
bool ScribbleArea::staticInited = false;
struct SDL_Cursor_Deleter { void operator()(void* x) { if(x) SDL_FreeCursor(static_cast<SDL_Cursor*>(x)); } };
std::unique_ptr<SDL_Cursor, SDL_Cursor_Deleter> ScribbleArea::penCursor;
std::unique_ptr<SDL_Cursor, SDL_Cursor_Deleter> ScribbleArea::panCursor;
std::unique_ptr<SDL_Cursor, SDL_Cursor_Deleter> ScribbleArea::eraseCursor;
#endif

ScribbleArea::ScribbleArea() : ScribbleView()
{
  currMode = MODE_NONE;
  posHistoryPos = posHistory.begin();
  // always need hover events to check for mouse motion so we can reset cursor to default
  scribbleInput->enableHoverEvents = true;
}

// Members that are affected by config values get set here.  Right now, these are just config values that are
//  accessed very frequently and so "cached"
void ScribbleArea::loadConfig(ScribbleConfig* _cfg)
{
  ScribbleView::loadConfig(_cfg);
  pageSpacing = cfg->Float("pageSpacing");
  viewMode = (viewmode_t)cfg->Int("viewMode");
  centerPages = cfg->Bool("centerPages");
  reflowWordSep = cfg->Float("minWordSep", 0.3f);
  selColMode = RuledSelector::ColMode(cfg->Int("columnDetectMode"));
  drawCursor = cfg->Int("drawCursor");
  //scribbleInput->enableHoverEvents = (drawCursor == 2);
#ifdef ONE_TIME_TIPS
  showHelpTips = scribbleDoc->scribbleMode && (app->oneTimeTip("ghostpage") || app->oneTimeTip("scalesel") ||
      app->oneTimeTip("rotatesel") || app->oneTimeTip("movesel") || app->oneTimeTip("cropsel") ||
      app->oneTimeTip("rectsel"));
#endif
  // this is done here because we need to update immediately when pen is detected
  RectSelector::HANDLE_PAD = cfg->Int("singleTouchMode") == INPUTMODE_DRAW ? 8 : 4;

#if !PLATFORM_MOBILE
  if(!staticInited) {
    staticInited = true;
    // cursors
    const Dim penradius = MIN_CURSOR_RADIUS/unitsPerPx;
    const Dim eraserradius = ERASESTROKE_RADIUS/unitsPerPx;

    Image penimg(int(2*penradius + 3.5), int(2*penradius + 3.5));
    Painter penpaint(Painter::PAINT_SW | Painter::NO_TEXT, &penimg);
    penpaint.setBackgroundColor(Color::TRANSPARENT_COLOR);
    penpaint.beginFrame();
    penpaint.setStroke(Color::WHITE, 1);
    penpaint.setFillBrush(Color::BLACK);
    penpaint.drawPath(Path2D().addEllipse(penradius + 1, penradius + 1, penradius+0.5, penradius+0.5));
    penpaint.endFrame();
    SDL_Surface* pensurf = SDL_CreateRGBSurfaceFrom((void*)penimg.bytes(),
        penimg.width, penimg.height, 32, 4*penimg.width, Color::R, Color::G, Color::B, Color::A);
    penCursor.reset(SDL_CreateColorCursor(pensurf, int(penradius + 1.5), int(penradius + 1.5)));
    SDL_FreeSurface(pensurf);

    Image eraserimg(int(2*eraserradius + 3), int(2*eraserradius + 3));
    Painter eraserpaint(Painter::PAINT_SW | Painter::NO_TEXT, &eraserimg);
    eraserpaint.setBackgroundColor(Color::TRANSPARENT_COLOR);
    eraserpaint.beginFrame();
    eraserpaint.setAntiAlias(true);
    eraserpaint.setFillBrush(Color::WHITE);
    eraserpaint.setStrokeBrush(Color::BLACK);
    eraserpaint.setStrokeWidth(1);
    eraserpaint.drawPath(Path2D().addEllipse(eraserradius + 1, eraserradius + 1, eraserradius, eraserradius));
    eraserpaint.endFrame();
    SDL_Surface* erasersurf = SDL_CreateRGBSurfaceFrom((void*)eraserimg.bytes(),
        eraserimg.width, eraserimg.height, 32, 4*eraserimg.width, Color::R, Color::G, Color::B, Color::A);
    eraseCursor.reset(SDL_CreateColorCursor(erasersurf, int(eraserradius + 1.5), int(eraserradius + 1.5)));
    SDL_FreeSurface(erasersurf);

    panCursor.reset(SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND));
  }
#endif
#ifdef SCRIBBLE_IAP
  if(!watermark && !iosIsPaid()) {
    Dim w = 227, h = 113;
    watermark = new Image(w, h, Image::PNG);
    Painter wmpaint(Painter::PAINT_SW, watermark);
    wmpaint.setBackgroundColor(0x00FFFFFF);  //Color::TRANSPARENT_COLOR);
    wmpaint.beginFrame();
    wmpaint.setFontSize(40);
    //wmpaint.setCompOp(Painter::CompOp_Src);  // don't blend w/ BG
    //nvgGlobalCompositeBlendFuncSeparate(Painter::vg, NVG_ONE, NVG_ZERO, NVG_ONE, NVG_ONE_MINUS_SRC_ALPHA);
    wmpaint.setFillBrush(Color(0xFF000000));
    wmpaint.setStroke(Color(0xFFFFFFFF), 1.0);
    wmpaint.rotate(-45*M_PI/180.0);
    for(Dim y = 0; y < h + w; y += 40) {
      for(Dim x = 0; x < 2*y && x < 1.414*w; x += 400)
        wmpaint.drawText(x - y + 20, y, "Sumi");
    }
    wmpaint.endFrame();
    // fill + stroke w/ alpha < 1 doesn't give desired effect...
    int nbytes = watermark->dataLen();
    unsigned char* bytes = watermark->bytes();
    for(int ii = 3; ii < nbytes; ii += 4)
      bytes[ii] >>= 3;  // alpha /= 8
  }
#endif
}

void ScribbleArea::reset()
{
  doCancelAction();
  recentStrokes.clear();  // occasionally saw crashes ... document being opened while still in MODE_STROKE?
  posHistory.clear();
  posHistoryPos = posHistory.begin();
  if(currSelection)
    clearSelection();
  // tags picked for the old notebook must not land in the one being opened; not cancelTagPlacement(), whose
  //  refresh would touch the document mid-switch - the SetDoc refresh takes the hint down
  for(Element* s : pendingTags)
    s->deleteNode();
  pendingTags.clear();
  placingPressed = false;
  todoPressed = NULL;
  currPage = NULL;
}

Page* ScribbleArea::page(int n) const
{
  return n < numPages() ? scribbleDoc->document->pages[n] : NULL;
}

int ScribbleArea::numPages() const
{
  return scribbleDoc->document->numPages();
}

// changed from 375x625 and 0.5 zoom to 240x400 and 0.32 zoom to decrease thumbnail size
void ScribbleArea::drawThumbnail(Image* dest)
{
  bool thumbtest = cfg->Int("saveThumbnail") == 2;
  int srgb = thumbtest ? Painter::SRGB_AWARE : 0;  // disable sRGB for better contrast
  Painter thumbpaint(Painter::PAINT_SW | srgb, dest);
  thumbpaint.beginFrame();
  thumbpaint.save();
  // aliasing in thumbnails is quite noticible on high-DPI displays
  thumbpaint.setAntiAlias(true);
  // thumbPaint.reset();  -- should make thumbPaint a member of ScribbleArea
  Rect dirty = thumbpaint.deviceRect;
  // the whole page, not the view: the document list draws every preview in a page-shaped frame
  //  (cover.h), so the page is scaled to fill dest and what does not fit is cut off at the right or
  //  bottom - a page of dest's proportions loses nothing, and there is never gray space around it.  The
  //  page is the one being viewed, or the first with thumbFirstPage.
  int pagenum = cfg->Bool("thumbFirstPage") ? 0 : currPageNum;
  Page* thumbPage = viewMode == VIEWMODE_SINGLE ? currPage : page(pagenum);
  Dim scale = std::max(dirty.width()/thumbPage->width(), dirty.height()/thumbPage->height());
  dirty.right /= scale;
  dirty.bottom /= scale;
  Point dimpos = getPageOrigin(viewMode == VIEWMODE_SINGLE ? currPageNum : pagenum);
  dirty.translate(dimpos.x, dimpos.y);
  // this doesn't handle split view case in general!
  Element::FORCE_NORMAL_DRAW = true;  // suppress STROKEDRAW_SEL
  // setup painter
  thumbpaint.scale(scale, scale);
  thumbpaint.translate(-dimpos.x, -dimpos.y);
  drawImage(&thumbpaint, dirty);  // draw content
  // draw page number
  thumbpaint.restore();
  Element::FORCE_NORMAL_DRAW = false;
  // saveThumbnail == 2 for tests to hide page number
  /*if(cfg->Int("saveThumbnail") != 2) {
    dirty = thumbpaint.getSize();
    int dd = int(5*preScale + 0.5);  // text offset
    int n = cfg->Bool("thumbFirstPage") ? 0 : currPageNum;  // or hide page num if thumbFirstPage?
    thumbpaint.setBrush(Brush(TEXT_FG_COLOR));
    thumbpaint.setFontSize(fontSize);
    thumbpaint.setTextAlign(Painter::AlignRight | Painter::AlignBottom);
    drawTextwithBG(&thumbpaint, fstring("%d / %d", n+1, numPages()).c_str(),
        dirty.right - dd, dirty.bottom - dd, TEXT_FG_COLOR, TEXT_BG_COLOR);
  }*/
  thumbpaint.endFrame();
}

Point ScribbleArea::getPageOrigin(int pagenum) const
{
  Point origin(0, 0);
  Page* p = pagenum == numPages() ? scribbleDoc->ghostPage.get() : page(pagenum);
  if(viewMode == VIEWMODE_SINGLE || !p) {}
  else if(viewMode == VIEWMODE_VERT) {
    if(centerPages)
      origin.x = (contentWidth - p->width())/2;
    for(int ii = 0; ii < pagenum; ii++)
      origin.y += page(ii)->height() + pageSpacing;
  }
  else if(viewMode == VIEWMODE_HORZ) {
    if(centerPages)
      origin.y = (contentHeight - p->height())/2;
    for(int ii = 0; ii < pagenum; ii++)
      origin.x += page(ii)->width() + pageSpacing;
  }
  return origin;
}

void ScribbleArea::zoomCenter(Dim newZoom, bool snap)
{
  zoomTo(newZoom, getViewWidth()/2, getViewHeight()/2);
  if(snap)
    roundZoom(getViewWidth()/2, getViewHeight()/2);
  uiChanged(UIState::Zoom);
  doRefresh();
}

// a fit width/height snap that changes the zoom by less than this is jitter from a two finger pan, not a
//  snap the user can see, so it must not move the view (see roundZoom())
static constexpr Dim ZOOM_SNAP_ALIGN_MIN = 0.02;

// the page exactly as wide as the view - no horzBorder margin either side (it used to leave one, so the
//  snap target had a gap on both sides; see docs/agent/navigation.md)
Dim ScribbleArea::fitWidthZoom(int pagenum) const
{
  return getViewWidth()/page(pagenum)->width()/preScale;
}

// whether a zoom gesture ending at `zoom` snaps to the fit width zoom `wzoom`: within 10% of it, unless fit
//  width is itself within 5% of 100%, or zoom rounds to the 100% step - 100% always has priority.  This is
//  the one predicate behind the snap (roundZoom()) and the "Fit" toast, so the toast never promises a snap
//  that does not happen
bool ScribbleArea::snapsToFitWidth(Dim zoom, Dim wzoom) const
{
  if(cfg->Bool("continuousZoom"))
    return false;
  if(!(wzoom < 1.1*zoom && zoom < 1.1*wzoom && (wzoom > 1.05 || wzoom < 0.95)))
    return false;
  return zoomSteps[nearestZoomStep(zoom)] != 1;
}

bool ScribbleArea::nearFitWidth(Dim px, Dim py) const
{
  int pagenum = dimToPageNum(screenToDim(Point(px, py)));
  return snapsToFitWidth(mZoom, fitWidthZoom(pagenum));
}

// line the page up with the view after a fit snap, but only across the scroll direction (horizontally in
//  the usual vertical layout) - never along it, so the reading position never jumps
void ScribbleArea::alignFitPage(int pagenum, bool fitWidth)
{
  // ensure that page under center of gesture is active
  setPageNum(pagenum);
  Rect pageRect = pageDimToDim(currPage->rect());
  Point screenCenter = screenToDim(Point(getViewWidth()/2, getViewHeight()/2));
  Point shift(0, 0);  // in dim units, the amount the page moves on screen
  if(viewMode != VIEWMODE_HORZ)
    shift.x = fitWidth ? (screenToDim(Point(0, 0)).x - pageRect.left) : (screenCenter.x - pageRect.center().x);
  if(viewMode != VIEWMODE_VERT && !fitWidth)
    shift.y = screenCenter.y - pageRect.center().y;
  if(shift.x != 0 || shift.y != 0)
    doPan(shift.x*mScale, shift.y*mScale);
}

void ScribbleArea::roundZoom(Dim px, Dim py)
{
  // should we still snap to width, height if continuous zoom enabled? use continuousZoom > 1?
  if(cfg->Bool("continuousZoom"))
    return;
  // use page under center of gesture for snapping to width, height
  int pagenum = dimToPageNum(screenToDim(Point(px, py)));
  Dim wzoom = fitWidthZoom(pagenum);
  Dim hzoom = getViewHeight()/page(pagenum)->height()/preScale;
  Dim zoom = mZoom;
  bool fitWidth = snapsToFitWidth(zoom, wzoom);
  ScribbleView::roundZoom(px, py);
  // zoom = 100% always has priority
  if(mZoom == 1)
    return;
  bool fitHeight = !fitWidth && hzoom < 1.1*zoom && zoom < 1.1*hzoom && (hzoom > 1.05 || hzoom < 0.95);
  if(!fitWidth && !fitHeight)
    return;
  Dim fitZoom = fitWidth ? wzoom : hzoom;
  // like a zoom step, the fit zoom is taken about the gesture point, so the content under the fingers stays
  //  put. Aligning the page to the view is then done only across the scroll direction (horizontally in the
  //  usual vertical layout), and only when the zoom really changed: a two finger pan at fit zoom always
  //  changes the zoom by a hair, and fit height used to center the whole page, so the view jumped along
  //  the page with no visible zoom snap (see docs/agent/navigation.md).  At or below fit width the
  //  horizontal pan is locked anyway (updateContentDim()), so a fit width page is flush regardless
  zoomTo(fitZoom, px, py);
  if(std::abs(fitZoom/zoom - 1) < ZOOM_SNAP_ALIGN_MIN)
    return;
  alignFitPage(pagenum, fitWidth);
}

// Ctrl+wheel: no step rounding (the wheel is continuous), only the snap to fit width the toast promised
void ScribbleArea::wheelZoomFinish(Dim px, Dim py)
{
  bool snap = nearFitWidth(px, py);
  showFitHint(false);
  if(!snap)
    return;
  int pagenum = dimToPageNum(screenToDim(Point(px, py)));
  zoomTo(fitWidthZoom(pagenum), px, py);
  zoomStepsIdx = nearestZoomStep(mZoom);
  alignFitPage(pagenum, true);
  uiChanged(UIState::Zoom);
}

// At or below fit width there is no horizontal pan at all: the pages in view are centered, which at exactly
//  fit is flush with both edges.  Without this the horzBorder margins left 2*horzBorder of sideways play
//  between fit width and the zoom where the page is that much narrower than the view.  "Fit" is judged by
//  the widest page *in view*, not contentWidth (the widest page in the document): one landscape page must
//  not unlock sideways panning on every portrait page, so this is rechecked on every pan.  Not in the
//  horizontal layout, where horizontal is the scroll direction.  Half a pixel of tolerance, as fit width is
//  computed in floating point.  Returns true if the limits changed
bool ScribbleArea::updateHorzPanLock()
{
  Dim minx = freeMinOriginX, maxx = freeMaxOriginX;
  Dim viewwidth = getViewWidth();
  if(viewMode != VIEWMODE_HORZ && numPages() > 0) {
    Dim viswidth = 0;
    if(viewMode == VIEWMODE_SINGLE)
      viswidth = currPage->width();
    else {
      int firstvis = dimToPageNum(Point(0, viewportRect.top));
      int lastvis = std::min(dimToPageNum(Point(0, viewportRect.bottom)), numPages() - 1);
      for(int ii = firstvis; ii <= lastvis; ++ii)
        viswidth = std::max(viswidth, page(ii)->width());
    }
    // pages are centered in contentWidth with centerPages, else left aligned at 0
    if(viswidth > 0 && viswidth*mScale <= viewwidth + 0.5)
      minx = maxx = (viewwidth - (centerPages ? contentWidth : viswidth)*mScale)/2;
  }
  bool changed = minx != minOriginX || maxx != maxOriginX;
  minOriginX = minx;
  maxOriginX = maxx;
  return changed;
}

void ScribbleArea::doPan(Dim dx, Dim dy)
{
  ScribbleView::doPan(dx, dy);
  // the pages now in view may lock (or unlock) sideways panning; pan by 0 to clamp to the new limits
  if(updateHorzPanLock())
    ScribbleView::doPan(0, 0);
  if(viewMode == VIEWMODE_SINGLE || (currMode != MODE_NONE && currMode != MODE_PAN))
    return;

  Rect screenrect = screenToDim(screenRect);
  Rect pagerect = pageDimToDim(currPage->rect());
  // shrink screenrect so page changes before prev page is completely invisible
  screenrect.pad(-screenrect.width()/6, -screenrect.height()/6);
  if(!pagerect.overlaps(screenrect)) {
    int firstvispage = dimToPageNum(Point(screenrect.left, screenrect.top));
    if(currPageNum < firstvispage)
      setPageNum(firstvispage);
    else {
      int lastvispage = dimToPageNum(Point(screenrect.right, screenrect.bottom));
      if(currPageNum > lastvispage)
        setPageNum(lastvispage);
      else
        return;
    }
    uiChanged(UIState::Pan);
  }
#ifdef ONE_TIME_TIPS
  if(showHelpTips && dimToPageNum(Point(screenrect.right, screenrect.bottom + 80)) == numPages()) {
    app->oneTimeTip("ghostpage", Point(screenRect.center().x, screenRect.bottom + 80),
      _("Double tap or drag selection here to add a new page."));
  }
#endif
}

void ScribbleArea::pageSizeChanged()
{
  repaintAll();
  updateContentDim();
  ScribbleView::pageSizeChanged();
  Point origin = getPageOrigin(currPageNum);
  currPageXOrigin = origin.x;
  currPageYOrigin = origin.y;
  // zoom, page number now displayed by UI, so must update
  uiChanged(UIState::PageSizeChange);
}

// pageCountChanged handles change in number or ordering of pages - tries to preserve view position
// pagenum indicates that page that was inserted or removed
void ScribbleArea::pageCountChanged(int pagenum, int prevpages)
{
  int prevpagenum = currPageNum;
  Point prevpos = dimToPageDim(screenToDim(Point(0,0)));
  // make sure current page is valid and consistent with page number
  setPageNum(currPageNum);
  pageSizeChanged();
  int currpages = numPages();
  if(prevpages >= 0 && pagenum >= 0 && prevpages != currpages && pagenum <= prevpagenum)
    prevpagenum += prevpages < currpages ? 1 : -1;
  // TODO: update items in posHistory!
  // don't jump if first or last page removed
  if(prevpagenum < 0)
    gotoPage(-1);  // go to beginning of first page
  else if(currpages < prevpages && pagenum >= currpages)
    gotoPage(currpages);  // go to end of last page
  else
    gotoPos(prevpagenum, prevpos, false);
}

void ScribbleArea::setPageDims(Dim width, Dim height, bool pending)
{
  PageProperties props = currPage->getProperties();
  if((width <= 0 || props.width == width) && (height <= 0 || props.height == height))
    return;
  props.width = width > 0 ? width : props.width;
  props.height = height > 0 ? height : props.height;
  currPage->setProperties(&props);
  if(pending)
    scribbleDoc->repaintAll();
  else
    scribbleDoc->pageSizeChanged();
}

void ScribbleArea::nextPage(bool appendnew)
{
  // Note that we don't append a new page if the current page is empty
  if(currPageNum < numPages() - 1)
    gotoPage(currPageNum + 1);
  else if(appendnew && currPage->strokeCount() > 0)
    scribbleDoc->newPage();
}

void ScribbleArea::prevPage()
{
  if(currPageNum > 0)
    gotoPage(currPageNum - 1);
}

void ScribbleArea::setPageNum(int pagenum)
{
  // should be a Document::getPageCount() method!
  int totalpages = numPages();
  if(totalpages < 1) return;
  pagenum = std::max(0, std::min(pagenum, totalpages - 1));
  // on page insertion and deletion, page numbering changes, so also compare pointers
  if(currPage != page(pagenum) || currPageNum != pagenum) {
    // process recent strokes before changing page
    groupStrokes();
    finishShape();  // a multi-point shape belongs to the page it was started on
    currPage = page(pagenum);
    currPageNum = pagenum;
    currPage->ensureLoaded();  // needed for memory usage check if nothing else
    if(viewMode == VIEWMODE_SINGLE)
      pageSizeChanged();
    else {
      Point origin = getPageOrigin(currPageNum);
      currPageXOrigin = origin.x;
      currPageYOrigin = origin.y;
    }
    if(!currSelection)
      currSelPageNum = currPageNum;
    // page number must be updated in UI
    uiChanged(UIState::PageNumChange);
  }
}

void ScribbleArea::expandDown()
{
  Dim ystep = currPage->yruling() > 0 ? currPage->yruling() : GROW_STEP;
  scribbleDoc->startAction(currPageNum);
  setPageDims(-1, currPage->height() + (GROW_EXTRA+1)*ystep);
  scribbleDoc->endAction();
}

void ScribbleArea::expandRight()
{
  Dim xstep = currPage->xruling() > 0 ? currPage->xruling() : GROW_STEP;
  scribbleDoc->startAction(currPageNum);
  setPageDims(currPage->width() + (GROW_EXTRA+1)*xstep, -1);
  scribbleDoc->endAction();
}

// seems like some of the code here belongs in Page class
void ScribbleArea::growPage(Rect bbox, Dim maxdx, Dim maxdy, bool pending)
{
  Dim newwidth = -1;
  Dim newheight = -1;
  Dim xstep = currPage->xruling() > 0 ? currPage->xruling() : GROW_STEP;
  Dim ystep = currPage->yruling() > 0 ? currPage->yruling() : GROW_STEP;
  if(cfg->Bool("growDown") && currPage->height() - bbox.bottom < GROW_TRIGGER*ystep)
    newheight = ystep * (int)(bbox.bottom/ystep + 1) + GROW_EXTRA*ystep;
  if(cfg->Bool("growRight") && currPage->width() - bbox.right < GROW_TRIGGER*xstep)
    newwidth = xstep * (int)(bbox.right/xstep + 1) + GROW_EXTRA*xstep;
  if(maxdx >= 0) newwidth = std::min(currPage->width() + maxdx, newwidth);
  if(maxdy >= 0) newheight = std::min(currPage->height() + maxdy, newheight);
  if(newheight > currPage->height() || newwidth > currPage->width())
    setPageDims(newwidth, newheight, pending);
}

void ScribbleArea::setStrokeProperties(const StrokeProperties& props, bool undoable)
{
  // a region's selection carries ink but is not an ink selection: nothing here recolors it
  if(!currSelection || regionSelector)
    return;
  viewSelection();
  if(undoable) {
    scribbleDoc->startAction(currPageNum);
    currSelection->setStrokeProperties(props);
    scribbleDoc->endAction();
  }
  else {
    for(Element* s : currSelection->strokes)
      s->setProperties(props);
  }
  // adjust selection region to account for possible stroke width change
  currSelection->shrink();
  doRefresh();
}

ScribblePen ScribbleArea::getPenForSelection(int* dashStyle) const
{
  if(dashStyle)
    *dashStyle = ScribblePen::DASH_SOLID;
  if(!currSelection)
    return ScribblePen(Color::INVALID_COLOR, -1);
  StrokeProperties props = currSelection->getStrokeProperties();
  if(dashStyle)
    *dashStyle = props.dashStyle;
  //auto isPressurePenFn = [](const Element* t) {
  //  return t->node->hasClass(Element::FLAT_PEN_CLASS) || t->node->hasClass(Element::ROUND_PEN_CLASS);
  //};
  //bool haspressure = currSelection->getFirstElement(isPressurePenFn) != NULL;
  return ScribblePen(props.color, props.width);  //, haspressure ? ScribblePen::WIDTH_PR : 0);
}

// The following two fns are used only for shared whiteboarding at the moment (specifically, when a stroke or
//  page is deleted by another user) ... also called when stroke is moved, since that can invalidate sel BG
void ScribbleArea::invalidateStroke(Element* s)
{
  RecentStrokesIter it = std::find(recentStrokes.begin(), recentStrokes.end(), s);
  if(it != recentStrokes.end())
    recentStrokes.erase(it);
  // A peer edited or deleted the region selected here.  Its selection also holds the ink it carries, so
  //  it would not empty below and would be left pointing at a region it no longer holds - drop it all,
  //  and any gesture on it, so no undo item is made for a region that may be gone.
  if(s && s == selectedRegion()) {
    regionHandleStart = RulingRegionParams();
    int modetype = ScribbleMode::getModeType(currMode);
    if(currMode == MODE_SHAPEHANDLE || modetype == MODE_MOVESEL || currMode == MODE_SCALESEL
        || currMode == MODE_SCALESELW || currMode == MODE_ROTATESEL || currMode == MODE_ROTATESELW)
      currMode = MODE_NONE;
    clearSelection();
    return;
  }
  if(currSelection && currSelection->removeStroke(s)) {
    if(currSelection->count() == 0)
      clearSelection();
    else
      currSelection->shrink();
  }
}

// note that we do not handle setting new page - that is done in the same way as local undo (pageCountChanged)
void ScribbleArea::invalidatePage(Page* p)
{
  if(currSelection && currSelection->page == p)
    clearSelection();
  // recent strokes are always on currPage since groupStrokes() is called in setPageNum()
  if(currPage == p)
    recentStrokes.clear();
}

void ScribbleArea::clearShotRegion()
{
  if(shotRegion.empty())
    return;
  dirtyShotRegion();
  shotRegion.clear();
  shotRegionPage = -1;
  doRefresh();
}

// the outline is an overlay (drawScreen), so it is the screen that needs repainting, not the page
void ScribbleArea::dirtyShotRegion()
{
  if(shotRegion.empty() || shotRegionPage < 0 || shotRegionPage >= numPages())
    return;
  if(shotRegionPage == currPageNum)
    dirtyScreen(shotRegion.getBBox().pad(4/mZoom));
  else
    scribbleDoc->dirtyPage(shotRegionPage);
}

bool ScribbleArea::clearSelection()
{
  clearShotRegion();
  if(!currSelection) {
    scribbleDoc->clearSelection();
    return false;
  }
  //scribbleDoc->showSelToolbar(false); ... selection toolbar is closed like a regular popup menu
  int pagenum = currPageNum;
  if(currSelPageNum != currPageNum && viewMode != VIEWMODE_SINGLE)
    setPageNum(currSelPageNum);
  // necessary for case of selection on different page since dirtyScreen
  //  uses pageDimToDim
  dirtyScreen(selBGRect.rectUnion(currSelection->getBGBBox()));
  selBGRect = Rect();
  bool hadRegion = regionSelector != NULL;
  delete currSelection;
  currSelection = NULL;
  shapeSelector = NULL;
  regionSelector = NULL;
  if(hadRegion) {
    uiChanged(UIState::SelChange);  // the region options row goes away with it
    // outside select mode the "..." chips were shown only because a region was selected
    if(!regionButtonsShown())
      scribbleDoc->repaintAll();
  }
  if(pagenum != currPageNum) {
    scribbleDoc->dirtyPage(currPageNum);
    setPageNum(pagenum);
  }
  recentStrokeSelPos = -1;
  currSelPageNum = currPageNum;
  return true;
}

bool ScribbleArea::clearTempSelection()
{
  // selectors are owned by selection
  pathSelector = NULL;
  ruledSelector = NULL;
  rectSelector = NULL;
  lassoSelector = NULL;
  insSpaceEraseSelector = NULL;
  if(insSpaceEraseSelection) {
    insSpaceEraseSelection->clear();
    delete insSpaceEraseSelection;
    insSpaceEraseSelection = NULL;
  }
  if(tempSelection) {
    // must clear selection to update dirtyRect
    tempSelection->clear();
    delete tempSelection;
    tempSelection = NULL;
    return true;
  }
  return false;
}

void ScribbleArea::selectAll(Selection::StrokeDrawType drawtype)
{
  clearSelection();
  // select strokes from current page...
  currSelection = new Selection(currPage, drawtype);
  new RectSelector(currSelection, mZoom, true);  //isMoveSelFree());
  currSelection->selectAll();
  if(currSelection->count() == 0) {
    delete currSelection;
    currSelection = NULL;
  }
  else
    currSelection->shrink();
}

// TODO: option to display dialog box to allow user to select which properties to use
void ScribbleArea::selectSimilar()
{
  if(currSelection) {
    viewSelection();
    currSelection->selectbyProps(NULL, true, true);
    currSelection->shrink();
    repaintAll();
  }
}

void ScribbleArea::invertSelection()
{
  // inverting a region's selection would hand the region all the ink outside it
  if(regionSelector)
    clearSelection();
  if(currSelection) {
    //doCancelAction();
    viewSelection();
    currSelection->invertSelection();
    if(currSelection->count() > 0)
      currSelection->shrink();
    else
      clearSelection();
    repaintAll();
  }
  else
    selectAll();
}

void ScribbleArea::deleteSelection()
{
  // deleting a region deletes the region only: the ink it carried stays where it is, and the page's
  //  own lines come back under it
  if(Element* region = selectedRegion()) {
    Page* page = currSelection->page;
    scribbleDoc->startAction(currSelPageNum);
    page->removeStroke(region);
    scribbleDoc->endAction();
    currSelection->removeStroke(region);
    clearSelection();
    return;
  }
  if(currSelection) {
    viewSelection();
    scribbleDoc->startAction(currPageNum);
    currSelection->deleteStrokes();
    scribbleDoc->endAction();
    clearSelection();
  }
}

// Recent stroke select: selecting N most recent strokes with undo dial alternative mode
// it is safe to assume undo history is const, as any undo or redo will clear selection
bool ScribbleArea::recentStrokeSelect()
{
  bool selcleared = recentStrokeSelPos < 0;  // && currSelection;  -- split view may have selection
  if(selcleared)
    clearSelection();
  scribbleDoc->exitPageSelMode();
  if(!currSelection)
    recentStrokeSelPos = scribbleDoc->history->pos;
  // continue until we hit beginning of history or find a StrokeAddedItem
  bool strokeadded = false;
  UndoHistoryItem* item;
  while(recentStrokeSelPos > 0
      && (!(item = scribbleDoc->history->hist[--recentStrokeSelPos])->isA(UndoHistoryItem::HEADER) || !strokeadded)) {
    if(item->isA(UndoHistoryItem::STROKE_ADDED_ITEM)) {
      Element* s = static_cast<StrokeAddedItem*>(item)->getStroke();
      // we may have skipped over a StrokeDeletedItem for this stroke; a ruling region is not ink
      if(!s->parent() || s->isRulingRegion())
        continue;
      if(!currSelection) {
        // need to get page number from stroke - we could have skipped over add/delete page items, so page
        //  number in header may not be valid
        setPageNum(scribbleDoc->document->pageForElement(s)->getPageNum());
        currSelection = new Selection(currPage);
        rectSelector = new RectSelector(currSelection, mZoom, true);  //isMoveSelFree());
      }
      if(s->node->parent() == currSelection->page->contentNode) {
        currSelection->addStroke(s);
        strokeadded = true;
      }
    }
  }
  if(strokeadded) {
    currSelection->shrink();
    viewSelection();  // ensure selection is visible
  }
  else if(!currSelection)
    recentStrokeSelPos = -1;  // keep state consistent (!currSelection implies recentStrokeSelPos == -1)
  if(strokeadded || selcleared) {
    uiChanged(UIState::SelChange);
    doRefresh();
  }
  return strokeadded;
}

bool ScribbleArea::recentStrokeDeselect()
{
  int histsize = scribbleDoc->history->hist.size();
  if(!currSelection || recentStrokeSelPos < 0 || recentStrokeSelPos >= histsize)
    return false;
  UndoHistoryItem* item;
  bool strokeremoved = false;
  while(++recentStrokeSelPos < histsize
      && (!(item = scribbleDoc->history->hist[recentStrokeSelPos])->isA(UndoHistoryItem::HEADER) || !strokeremoved)) {
    if(item->isA(UndoHistoryItem::STROKE_ADDED_ITEM)) {
      Element* s = static_cast<StrokeAddedItem*>(item)->getStroke();
      if(s->isSelected(currSelection)) {
        currSelection->removeStroke(s);
        strokeremoved = true;
      }
    }
  }
  if(currSelection->count() == 0)
    clearSelection();
  else
    currSelection->shrink();
  uiChanged(UIState::SelChange);
  doRefresh();
  return strokeremoved;
}

void ScribbleArea::recentStrokeSelDone()
{
  if(currSelection && recentStrokeSelPos >= 0 && cfg->Bool("popupToolbar")) {
    //recentStrokeSelPos = -1; ... I think we actually don't want to do this
    Rect r = currSelection->getBGBBox();
    app->showSelToolbar(screenToGlobal(dimToScreen(pageDimToDim(Point(r.right, r.bottom)))));
  }
}

// we take Selection instead of Clipboard so we can access source page
bool ScribbleArea::selectionDropped(Selection* selection, Point globalPos, Point offset, bool replaceids)
{
  // for now, selection is not shifted at all from drop position; no consideration of ruling
  Point pos = screenToDim(globalToScreen(globalPos));
  int pagenum = dimToPageNum(pos);
  Point pagepos = pos - getPageOrigin(pagenum);
  Page* newpage = page(pagenum);
  if(!newpage || !newpage->rect().contains(pagepos))
    return false;

  Clipboard clip;
  selection->toSorted(&clip);  // this clones strokes, so use PasteMoveClipboard so we don't clone again
  if(replaceids) {
    clip.replaceIds();
    // This has the effect of clearing timestamp for drops from another ScribbleArea; are there other
    //  instances where we should clear timestamp of pasted content?  Whenever ids are replaced?
    // since clipbboard came from selection, we know every node has an Element
    for(SvgNode* node : clip.content->children())
      static_cast<Element*>(node->ext())->setTimestamp(0);
  }
  return clipboardDropped(&clip, globalPos, offset);
}

bool ScribbleArea::clipboardDropped(Clipboard* clip, Point globalPos, Point offset)
{
  Point pos = screenToDim(globalToScreen(globalPos));
  // if already in undo action (as for drop on same doc split), don't call start/end in doPasteAt
  int flags = scribbleDoc->history->undoable() ? 0 : PasteUndoable;
  flags |= offset.isNaN() ? PasteCenter : PasteOrigin;
  // drag and drop selection currently doesn't support ruling, so don't use PasteNoHandles in any case
  doPasteAt(clip, offset.isNaN() ? pos : pos + offset, PasteFlags(flags | PasteMoveClipboard));
  uiChanged(UIState::Paste);
  doRefresh();
  return true;
}

void ScribbleArea::doPaste(Clipboard* clipboard, const Page* srcpage, int flags)
{
  Point pos = screenToDim(screenRect.center());
  // "write-page" class used to identify whole pages; works even pasting between separate processes
  if(clipboard->content->firstChild()->hasClass("write-page")) {
    // paste into page break nearest center of screen
    int pagenum = dimToPageNum(pos);
    Point wherep = (pos - getPageOrigin(pagenum)) - page(pagenum)->rect().center();
    int where = (viewMode == VIEWMODE_HORZ ? wherep.x : wherep.y) < 0 ? pagenum : pagenum + 1;
    scribbleDoc->pastePages(clipboard, where);
  }
  else {
    // paste onto the currently centered page
    bool origpos = page(std::min(dimToPageNum(pos), numPages() - 1)) != srcpage;
    doPasteAt(clipboard, pos, PasteFlags(flags |
        (origpos ? PasteOrigPos : PasteCenter) | PasteOffsetExisting | PasteRulingShift | PasteUndoable));
  }
}

// paste position:
// - PasteCenter: place center of clipboard bbox at specified position
// - PasteCorner: place upper left corner of clipboard bbox at specified position
// - PasteOrigin: place the (0,0) of the clipboard contents at specified position
// - PasteOrigPos: specified position is ignored (falls back to PasteCenter if not visible or off-page)
// PasteUndoable flag is a horrible hack - but it's the same hack used for insertPage, etc, so at least we're consistent
// instead of PasteMoveClipboard, we could consider passing Clipboard instead of Clipboard* and using
//  Clipboard::clone() (w/ copy ctor hidden) if we don't want to move strokes
void ScribbleArea::doPasteAt(Clipboard* clipboard, Point pos, PasteFlags flags)
{
  static constexpr Dim MIN_PASTE_OFFSET = 20;

  setPageNum(dimToPageNum(pos));
  Point p = dimToPageDim(pos);
  Point dr;
  bool offsetexisting = (flags & PasteOffsetExisting) && currSelection && currSelPageNum == currPageNum;
  Rect selbbox = offsetexisting ? currSelection->getBBox() : Rect();
  clearSelection();
  scribbleDoc->exitPageSelMode();
  currSelection = new Selection(currPage);
  rectSelector = new RectSelector(currSelection, mZoom, !(flags & PasteNoHandles));  //isMoveSelFree());
  if(flags & PasteUndoable)
    scribbleDoc->startAction(currPageNum);
  clipboard->paste(currSelection, flags & PasteMoveClipboard);
  Rect bbox = currSelection->getBBox();
  // scale external content to fit on page
  if(clipboard->content->hasClass("external")) {
    Dim scale = 0.75*std::min(currPage->width()/bbox.width(), currPage->height()/bbox.height());
    if(scale < 0.75) {
      Point tfxy = (1 - scale)*bbox.center();
      currSelection->stealthTransform(Transform2D(scale, 0, 0, scale, tfxy.x, tfxy.y));
      bbox = currSelection->getBBox();
    }
  }
  if(flags & PasteCorner)
    dr = p - bbox.origin();  // top left
  else if(flags & PasteOrigin)
    dr = p;
  else if((flags & PasteCenter) || !isVisible(pageDimToDim(bbox))
      || !currPage->rect().contains(bbox.center()) || !currPage->rect().contains(bbox.origin())) {
    // make sure bottom right corner is on page
    Dim minx = currPage->xruling()/2 + bbox.width()/2;
    Dim miny = currPage->yruling()/2 + bbox.height()/2;
    Dim maxx = currPage->width() - currPage->xruling()/2 - bbox.width()/2;
    Dim maxy = currPage->height() - currPage->yruling()/2 - bbox.height()/2;
    dr = Point(std::min(std::max(minx, p.x), maxx), std::min(std::max(miny, p.y), maxy)) - bbox.center();
  }
  // round dx and dy so that we translate by multiple of ruling
  // obviously not going to accomplish anything if source and target have different rulings
  if(flags & PasteRulingShift) {
    if(currPage->xruling() > 0)
      dr.x = quantize(dr.x, currPage->xruling());
    if(currPage->yruling() > 0)
      dr.y = quantize(dr.y, currPage->yruling());
  }
  // offset from existing selection, mainly to prevent exact overlap if paste is accidentily hit twice
  if(offsetexisting) {
    Dim offset = MAX(MIN_PASTE_OFFSET, MAX(currPage->xruling(), currPage->yruling()));
    if(ABS(bbox.left + dr.x - selbbox.left) < MIN_PASTE_OFFSET && ABS(bbox.top + dr.y - selbbox.top) < MIN_PASTE_OFFSET) {
      dr.x += currPage->xruling() > 0 ? currPage->xruling() : offset;
      dr.y += currPage->yruling() > 0 ? currPage->yruling() : offset;
    }
  }
  // apply new position to each stroke
  currSelection->stealthTransform(Transform2D::translating(dr.x, dr.y));
  currSelection->shrink();
  // endAction sends sync undo items, so must be done after stealthTransform()
  if(flags & PasteUndoable)
    scribbleDoc->endAction();
  // shrink should not be necessary
  //currSelection->shrink();
  clipboard->replaceIds();
#ifdef ONE_TIME_TIPS
  if(showHelpTips && rectSelector->enableCrop) {
    Rect b = dimToScreen(pageDimToDim(currSelection->getBGBBox()));
    app->oneTimeTip("cropsel", Point(b.right, b.bottom), _("Drag red handles to crop image."));
  }
#endif
}

// shape tool (SHAPES_SPEC.md)

// The descriptor a freshly started shape begins with.  Arrowheads and corner rounding come from the
//  options row toggles rather than from the shape id, so the same two toggles serve a line, a box and
//  every polyline flavour; the preferences supply the starting radius and curve tightness, exactly as
//  the pen supplies the starting colour and width.
ShapeParams ScribbleArea::newShapeParams(int shapeid, Point pos) const
{
  const ShapeDef* def = shapeDef(shapeid);
  int modeFlags = scribbleDoc->scribbleMode->shapeFlags;
  ShapeParams params;
  params.id = shapeid;
  params.points = {pos, pos};
  if(def && def->allowsHeads)
    params.flags |= modeFlags & (SHAPEFLAG_HEADSTART | SHAPEFLAG_HEADEND);
  if(def && def->allowsRounding && (modeFlags & SHAPEFLAG_ROUNDED)) {
    params.flags |= SHAPEFLAG_ROUNDED;
    params.rx = cfg->Float("shapeCornerRadius");
    params.ry = params.rx;
  }
  if(shapeid == SHAPE_CURVE)
    params.tightness = cfg->Float("shapeCurveTightness");
  return params;
}

// spec 5: leave a newly drawn shape selected with its handles up, so the parameters can be adjusted
//  while the shape is still the thing being thought about
// ruling regions

// the "..." button: a chip in screen units, sitting inside the region's bottom-right corner (as seen in
//  the region's own frame, so it follows a turned region round), drawn upright
static constexpr Dim REGION_BTN_SIZE = 22;
static constexpr Dim REGION_BTN_INSET = 4;
// how far outside a region, in its own line heights, a ruled insert space or select press still counts
//  as in it
static constexpr Dim REGION_PRESS_SLOP = 0.5;

Element* ScribbleArea::selectedRegion() const
{
  return regionSelector ? regionSelector->region : NULL;
}

bool ScribbleArea::regionButtonsShown() const
{
  return regionSelector || ScribbleMode::getModeType(scribbleDoc->scribbleMode->getMode()) == MODE_SELECT;
}

Point ScribbleArea::regionButtonPos(const Element* region) const
{
  const RulingRegionParams& params = region->regionParams();
  RulingFrame f = params.frame();
  Point corner = f.toLocal(params.corners[params.bottomRightCorner()]);
  Dim inset = (REGION_BTN_SIZE/2 + REGION_BTN_INSET)/mZoom;
  return f.toPage(corner - Point(inset, inset));
}

Element* ScribbleArea::regionButtonHit(Point pos, bool touch) const
{
  Dim reach = (REGION_BTN_SIZE/2 + (touch ? 6 : 2))/mZoom;
  // topmost region first, as it is the one drawn over the others
  std::vector<Element*> regions = currPage->regions();
  for(auto it = regions.rbegin(); it != regions.rend(); ++it) {
    Point c = regionButtonPos(*it);
    if(std::abs(pos.x - c.x) <= reach && std::abs(pos.y - c.y) <= reach)
      return *it;
  }
  return NULL;
}

void ScribbleArea::drawRegionButtons(Painter* painter)
{
  if(!regionButtonsShown())
    return;
  // the pages either side of the current one can be on screen too in a scrolling view
  int first = viewMode == VIEWMODE_SINGLE ? currPageNum : std::max(0, currPageNum - 1);
  int last = viewMode == VIEWMODE_SINGLE ? currPageNum : std::min(numPages() - 1, currPageNum + 1);
  Dim half = REGION_BTN_SIZE/2/mZoom;
  for(int pagenum = first; pagenum <= last; ++pagenum) {
    Page* pg = page(pagenum);
    if(!pg || pg->loadStatus != Page::LOAD_OK)
      continue;
    std::vector<Element*> regions = pg->regions();
    if(regions.empty())
      continue;
    painter->save();
    if(pagenum != currPageNum) {
      Point origin = getPageOrigin(pagenum);
      painter->translate(origin.x - currPageXOrigin, origin.y - currPageYOrigin);
    }
    painter->setAntiAlias(true);
    for(Element* region : regions) {
      Point c = regionButtonPos(region);
      bool selected = region == selectedRegion();
      // a dark chip with its own border reads the same over paper, a scan or ink - the page behind it is
      //  never known to be one color
      painter->setFillBrush(selected ? Color(0x20, 0x60, 0xC0, 230) : Color(0x30, 0x30, 0x30, 230));
      painter->setStroke(Color::WHITE, 1.25/mZoom);
      painter->drawPath(Path2D().addRect(Rect::centerwh(c, 2*half, 2*half)));
      painter->setStrokeBrush(Color::NONE);
      painter->setFillBrush(Color::WHITE);
      Dim dotr = 1.6/mZoom, gap = 5/mZoom;
      for(int ii = -1; ii <= 1; ++ii)
        painter->drawPath(Path2D().addEllipse(c.x + ii*gap, c.y, dotr, dotr));
    }
    painter->restore();
  }
}

void ScribbleArea::refreshRegionSelection()
{
  Element* region = selectedRegion();
  if(!region)
    return;
  const RulingRegionParams& params = region->regionParams();
  std::vector<Element*> gone;
  for(Element* s : currSelection->strokes) {
    if(s != region && !RegionSelector::carries(params, s))
      gone.push_back(s);
  }
  for(Element* s : gone) {
    s->setSelected(NULL);
    currSelection->removeStroke(s);
  }
  Page* pg = currSelection->page;
  for(Element* s : pg->children()) {
    // ink on a locked layer stays where it is, as it would under any other selection
    // as doSelect: never take an element another selection (another view's) holds
    if(s != region && !s->selection() && pg->isEditable(s) && RegionSelector::carries(params, s))
      currSelection->addStroke(s);
  }
  currSelection->invalidateBBox();
}

void ScribbleArea::selectRegion(Element* region)
{
  // every view's selection, not only this one's: another view may hold the region or its ink
  scribbleDoc->clearSelection();
  clearSelection();
  bool chipsWereShown = regionButtonsShown();
  // the ink comes along but is not shown as selected: it is being carried, not picked
  currSelection = new Selection(currPage, Selection::STROKEDRAW_NORMAL);
  currSelPageNum = currPageNum;
  regionSelector = new RegionSelector(currSelection, region, mZoom);
  currSelection->addStroke(region);
  refreshRegionSelection();
  dirtyScreen(currSelection->getBGBBox());
  if(!chipsWereShown)
    scribbleDoc->repaintAll();  // the chips appear on every region, not only this one
  uiChanged(UIState::SelChange);
}

void ScribbleArea::setSelRegionParams(const RulingRegionParams& params)
{
  Element* region = selectedRegion();
  if(!region)
    return;
  doCancelAction();
  dirtyScreen(currSelection->getBGBBox());
  scribbleDoc->startAction(currSelPageNum);
  // the item holds the parameters as they were; undo swaps them back
  scribbleDoc->history->addItem(new RegionChangedItem(region, currSelection->page));
  region->setRegionParams(params);
  scribbleDoc->endAction();
  currSelection->invalidateBBox();
  currSelection->xchgBGDirty(true);
  dirtyScreen(currSelection->getBGBBox());
  uiChanged(UIState::SetSelProps);
  doRefresh();  // edits come from the region panel, not from input on the canvas, which would refresh
}

void ScribbleArea::previewSelRegionParams(const RulingRegionParams& params)
{
  Element* region = selectedRegion();
  if(!region)
    return;
  dirtyScreen(currSelection->getBGBBox());
  region->setRegionParams(params);
  currSelection->invalidateBBox();
  currSelection->xchgBGDirty(true);
  dirtyScreen(currSelection->getBGBBox());
  doRefresh();
}

Rect ScribbleArea::selRegionGlobalRect() const
{
  Element* region = selectedRegion();
  if(!region)
    return Rect();
  Rect bounds;
  for(const Point& corner : region->regionParams().corners)
    bounds.rectUnion(screenToGlobal(dimToScreen(pageDimToDim(corner))));
  return bounds;
}

Rect ScribbleArea::globalViewRect() const
{
  return Rect::corners(screenToGlobal(Point(0, 0)), screenToGlobal(Point(getViewWidth(), getViewHeight())));
}

Element* ScribbleArea::addRulingRegion(const Rect& r)
{
  Dim yr = currPage->yruling() > 0 ? currPage->yruling() : Page::BLANK_Y_RULING;
  RulingRegionParams params = RulingRegionParams::fromRect(r, currPage->xruling(), yr, currPage->props.dotRadius);
  Element* region = Element::createRulingRegion(params, currPage->props.color, currPage->props.ruleColor);
  scribbleDoc->startAction(currPageNum);
  currPage->addStroke(region);
  scribbleDoc->endAction();
  selectRegion(region);
  return region;
}

// Page tags (docs/agent/page-tags.md)

static const Dim PENDING_TAG_STEP = 36;  // same stacking as ScribbleDoc's
static const Dim PENDING_TAG_MARGIN = 8;  // how far inside the page edge a tag placed beyond it ends up

void ScribbleArea::startTagPlacement(const std::vector<std::pair<std::string, std::string>>& tags, bool todo)
{
  cancelTagPlacement();
  if(tags.empty())
    return;
  // right-aligned under each other, the first one's centre under the pointer
  for(size_t ii = 0; ii < tags.size(); ++ii)
    pendingTags.push_back(Element::createPageTag(tags[ii].first.c_str(), tags[ii].second.c_str(),
        Point(0, ii*PENDING_TAG_STEP), todo));
  pendingTagsAnchor = pendingTags.front()->bbox().center();
  // until the pointer moves - and a finger never hovers - they wait in the middle of the view
  pendingTagsPos = dimToPageDim(screenToDim(Point(getViewWidth()/2, getViewHeight()/3)));
  dirtyScreen(pendingTagsRect());
  // puts up the "Place your tag" hint (MainWindow::syncTagPlaceHint()); this runs from the tag popup, outside
  //  any canvas event, so nothing else would refresh the UI
  uiChanged(UIState::Command);
  doRefresh();
}

void ScribbleArea::cancelTagPlacement()
{
  if(pendingTags.empty())
    return;
  dirtyScreen(pendingTagsRect());
  for(Element* s : pendingTags)
    s->deleteNode();
  pendingTags.clear();
  placingPressed = false;
  // takes the hint down; Esc and the hint's own cancel button are not canvas events either
  uiChanged(UIState::Command);
  doRefresh();
}

Rect ScribbleArea::pendingTagsRect() const
{
  Rect r;
  for(Element* s : pendingTags)
    r.rectUnion(s->bbox());
  Point offset = pendingTagsPos - pendingTagsAnchor;
  r.translate(offset.x, offset.y);
  return r.pad(2);
}

void ScribbleArea::movePendingTags(Point pos)
{
  if(pos == pendingTagsPos)
    return;
  dirtyScreen(pendingTagsRect());
  pendingTagsPos = pos;
  dirtyScreen(pendingTagsRect());
}

void ScribbleArea::placePendingTags(bool select)
{
  dirtyScreen(pendingTagsRect());
  // The press only switches page once it is well clear of the current one, and the drag can leave it, so the
  //  pointer may be in the gap between pages, beside a page or below the last one.  The tags go onto the page
  //  nearest the pointer, kept inside it: off the page they would still count, but no thumbnail would show
  //  them and opening the card would pulse empty space.
  Point gpos = pageDimToDim(pendingTagsPos);
  setPageNum(dimToPageNum(gpos));  // clamps the ghost page past the end to the last page
  pendingTagsPos = dimToPageDim(gpos);
  Point offset = pendingTagsPos - pendingTagsAnchor;
  Rect tagsRect;
  for(Element* s : pendingTags)
    tagsRect.rectUnion(s->bbox());
  tagsRect.translate(offset.x, offset.y);
  Rect pageRect = currPage->rect().pad(-PENDING_TAG_MARGIN);  // off the very edge
  Point shift(0, 0);
  if(tagsRect.right > pageRect.right)
    shift.x = pageRect.right - tagsRect.right;
  if(tagsRect.left + shift.x < pageRect.left)  // left and top win if the stack is bigger than the page
    shift.x = pageRect.left - tagsRect.left;
  if(tagsRect.bottom > pageRect.bottom)
    shift.y = pageRect.bottom - tagsRect.bottom;
  if(tagsRect.top + shift.y < pageRect.top)
    shift.y = pageRect.top - tagsRect.top;
  offset += shift;
  scribbleDoc->startAction(currPageNum);
  for(Element* s : pendingTags) {
    s->applyTransform(ScribbleTransform(Transform2D::translating(offset.x, offset.y)));
    s->commitTransform();
    currPage->addStroke(s);
  }
  scribbleDoc->endAction();
  if(select) {
    // a pen that does not hover had no preview until it touched down, so leave the tags ready to nudge
    clearSelection();
    currSelection = new Selection(currPage);
    currSelPageNum = currPageNum;
    rectSelector = new RectSelector(currSelection, mZoom, true);
    for(Element* s : pendingTags)
      currSelection->addStroke(s);
    currSelection->shrink();
    dirtyScreen(currSelection->getBGBBox());
    uiChanged(UIState::SelChange);
  }
  pendingTags.clear();  // the page owns them now
  placingPressed = false;
}

bool ScribbleArea::capturesPointer(const InputEvent& event) const
{
  if(placingTags())
    return true;
  // a finger that would pan still ticks a to-do it lands on
  Point pos = dimToPageDim(screenToDim(Point(event.points[0].x, event.points[0].y)));
  return todoBoxHit(pos, event.source == INPUTSOURCE_TOUCH) != NULL;
}

// the topmost, as it is the one drawn over the others; a tag on a locked or hidden layer is out of reach
//  like any other element there
Element* ScribbleArea::todoBoxHit(Point pos, bool touch) const
{
  Dim reach = (touch ? 6 : 2)/mZoom;
  Element* hit = NULL;
  for(Element* s : currPage->children()) {
    if(s->isTodoTag() && currPage->isEditable(s) && s->todoBoxRect().pad(reach).contains(pos))
      hit = s;  // later in the list is higher up
  }
  return hit;
}

// The ticked tag is a copy of the old one with the box flipped, swapped in as one action: undo, redo and sync
//  are then the ordinary add and delete, and Page::onAddStroke/onRemoveStroke recount the page's tags.
Element* ScribbleArea::toggleTodoTag(Element* tag)
{
  if(!tag || !tag->isTodoTag() || tag->node->parent() != currPage->contentNode)
    return NULL;
  if(tag->selection())
    clearSelection();
  Element* toggled = tag->cloneNode();
  toggled->setTodoDone(!tag->isTodoDone());
  scribbleDoc->startAction(currPageNum);
  currPage->addStroke(toggled, tag, tag->layer());
  currPage->removeStroke(tag);
  scribbleDoc->endAction();
  // a page that gains or loses a tag needs its thumbnail in the browser redrawn
  currPage->pageTagThumb.clear();
  return toggled;
}

// two fade-in, fade-out pulses of the active (--checked) blue, ~1.4 s in all
static const int PAGETAG_FLASH_FRAME_MS = 30;
static const int PAGETAG_FLASH_FRAMES = 48;
static const int PAGETAG_FLASH_PULSES = 2;
static const Dim PAGETAG_FLASH_STROKE = 3;  // screen pixels

void ScribbleArea::flashPageTags(const std::vector<std::string>& tagIds)
{
  flashRects.clear();
  for(Element* s : currPage->children()) {
    // a ticked to-do of the same tag is not what the card was listed for
    if(s->isPageTag() && !s->isTodoDone()
        && std::find(tagIds.begin(), tagIds.end(), s->pageTagId()) != tagIds.end())
      flashRects.push_back(s->bbox().pad(4));
  }
  if(flashRects.empty())
    return;
  flashPageNum = currPageNum;
  // opening at the page leaves its top in view; a tag further down would pulse off screen
  Rect tagsRect;
  for(const Rect& r : flashRects)
    tagsRect.rectUnion(r);
  viewRect(currPageNum, tagsRect);
  flashTicks = PAGETAG_FLASH_FRAMES;
  for(const Rect& r : flashRects)
    dirtyScreen(Rect(r).pad(PAGETAG_FLASH_STROKE/mScale));
  reqRepaint();
  flashTimer = ScribbleApp::gui->setTimer(PAGETAG_FLASH_FRAME_MS, NULL, flashTimer, [this]() {
    for(const Rect& r : flashRects)
      dirtyScreen(Rect(r).pad(PAGETAG_FLASH_STROKE/mScale));
    reqRepaint();
    if(--flashTicks > 0)
      return PAGETAG_FLASH_FRAME_MS;
    flashRects.clear();
    flashTimer = NULL;
    return 0;
  });
}

void ScribbleArea::editShapeAfterDraw(Element* shape)
{
  if(!shape || !shape->isShape() || !cfg->Bool("shapeEditAfterDraw"))
    return;
  clearSelection();
  currSelection = new Selection(currPage);
  currSelPageNum = currPageNum;
  currSelection->addStroke(shape);
  if(!useShapeSelector())  // falls back to the usual scale/rotate handles if the shape was demoted
    rectSelector = new RectSelector(currSelection, mZoom, true);
  currSelection->shrink();
  dirtyScreen(currSelection->getBGBBox());
  uiChanged(UIState::SelChange);
}

// shapes use the current pen's colour, width and dash - reusing the pen is less UI and is what the
//  selection toolbar already knows how to edit (spec 5, 10)
Element* ScribbleArea::createShapeElement(const ShapeParams& params)
{
  // the marker's relative width has to be in document units
  ScribblePen resolved = resolvedPen(params.points.empty() ? Point(NaN, NaN) : params.points.front());
  const ScribblePen* pen = &resolved;
  SvgPath* svgPath = new SvgPath();
  svgPath->setAttr<color_t>("fill", Color::NONE);
  setSvgStrokeColor(svgPath, pen->color);
  svgPath->setAttr<float>("stroke-width", pen->width);
  svgPath->setAttr<int>("stroke-linecap", Painter::RoundCap);
  svgPath->setAttr<int>("stroke-linejoin", Painter::RoundJoin);
  if(pen->dash > 0) {
    std::string dashes(pen->gap > 0 ? fstring("%f %f", pen->dash, pen->gap) : fstring("%f", pen->dash));
    svgPath->setAttribute("stroke-dasharray", dashes.c_str());
  }
  // deliberately no pen class: the pen classes drive toPenPoints(), which reinterprets a path as
  //  variable-width pen geometry - meaningless for a generated shape path (spec 7.9)
  Element* shape = new Element(svgPath);
  shape->setShapeParams(params);
  return shape;
}

Point ScribbleArea::snapShapePoint(Point pos) const
{
  if(!currPen()->hasFlag(ScribblePen::SNAP_TO_GRID))
    return pos;
  // the grid of the ruling the shape gesture started in (a region's, rotated or not)
  return gridFrame.snapToGrid(pos, Page::BLANK_Y_RULING);
}

// Editing snaps by how far the point is from the 0/45/90 degree line, not by the angle: by angle a long
//  line's end gets pulled across a long way, so it can never sit slightly off diagonal, while a short one
//  barely moves.  The angle cap (shapeAngleSnap) still applies, so 0 turns it off and a very short line
//  is not bent further than before.  In screen units, so zooming in gives finer control.
static constexpr Dim SHAPE_ANGLE_SNAP_DIST = 8;
// A recognized line is the pen's rough intent and is snapped by angle, a little less eagerly than the
//  setting allows (8 degrees -> 6).
static constexpr Dim RECOGNIZED_ANGLE_SNAP_FACTOR = 0.75;

static Dim angleSnapTolerance(Dim degrees)
{
  return std::min(Dim(22.5), degrees)*M_PI/180;
}

Point ScribbleArea::snapShapeAngleAt(const ShapeParams& params, int index, Point pos, Dim localPerPage) const
{
  // a grid-snapped pen already lands on grid points; bending those to 45 degrees would pull them off it
  if(currPen()->hasFlag(ScribblePen::SNAP_TO_GRID))
    return pos;
  Dim tolerance = angleSnapTolerance(cfg->Float("shapeAngleSnap"));
  return snapShapeAngle(params, index, pos, tolerance, SHAPE_ANGLE_SNAP_DIST/mScale*localPerPage);
}

Point ScribbleArea::snapRecognizedAngleAt(const ShapeParams& params, int index, Point pos) const
{
  if(currPen()->hasFlag(ScribblePen::SNAP_TO_GRID))
    return pos;
  Dim tolerance = angleSnapTolerance(cfg->Float("shapeAngleSnap"))*RECOGNIZED_ANGLE_SNAP_FACTOR;
  return snapShapeAngle(params, index, pos, tolerance);
}

// commit an in-progress multi-point shape; a no-op in every other case, so it is safe to call from any
//  of the escape routes a multi-point gesture has to survive (spec 4)
void ScribbleArea::finishShape(bool select)
{
  Element* shape = shapeInProgress;
  if(!shape)
    return;
  shapeInProgress = NULL;
  scribbleDoc->updateCurrStroke(shape->bbox());
  Rect bbox = shape->bbox();
  const ShapeParams& params = shape->shapeParams();
  const ShapeDef* def = shapeDef(params.id);
  if(!def || int(params.points.size()) < def->minPoints || !bbox.isValid()
      || !currPage->rect().intersects(bbox)) {
    shape->deleteNode();
    return;
  }
  if(!currPage->rect().contains(bbox))
    dirtyScreen(bbox);
  if(cfg->Bool("growWithPen"))
    growPage(bbox);
  scribbleDoc->startAction(currPageNum);
  currPage->addStroke(shape);  // creates the StrokeAddedItem itself (spec 7.11)
  scribbleDoc->endAction();
  scribbleDoc->updateCurrStroke(bbox);
  if(select)
    editShapeAfterDraw(shape);
}

void ScribbleArea::cancelShape()
{
  if(!shapeInProgress)
    return;
  scribbleDoc->updateCurrStroke(shapeInProgress->bbox());
  shapeInProgress->deleteNode();
  shapeInProgress = NULL;
}

bool ScribbleArea::useShapeSelector()
{
  shapeSelector = NULL;
  if(!currSelection || currSelection->count() != 1 || !currSelection->strokes.front()->isShape())
    return false;
  delete currSelection->selector;  // Selector dtor clears Selection::selector
  pathSelector = NULL;
  ruledSelector = NULL;
  rectSelector = NULL;
  lassoSelector = NULL;
  shapeSelector = new ShapeSelector(currSelection, mZoom);
  currSelection->xchgBGDirty(true);
  return true;
}

// hold-to-snap: hold the pen still at the end of a stroke and it becomes the shape it looks like

// How far the pen may wander and still count as held, in screen units.  Loose on purpose: nobody holds a
//  pen perfectly still, and the recognizer trims the cluster of hold samples itself.
static constexpr Dim SNAP_HOLD_RADIUS = 6;
static constexpr int SNAP_POLL_MS = 40;
// once any point of a snapped shape has moved this far (screen units) under the pen, the shape as it
//  snapped gets an undo step of its own; less than this is jitter from lifting the pen
static constexpr Dim SNAP_CHANGE_MIN = 6;
// a tilted ellipse has no shape of its own; it becomes a closed curve through this many points on it
static constexpr int SNAP_ELLIPSE_POINTS = 8;

// the hold in ms, or 0 when snapping is off; any other value is kept inside the 0.5 - 1.5 s range
static int shapeSnapDelayMs(ScribbleConfig* cfg)
{
  Dim delay = cfg->Float("shapeSnapDelay");
  return delay > 0 ? int(1000*std::min(Dim(1.5), std::max(Dim(0.5), delay)) + 0.5) : 0;
}

// Erases what a scratch-out was drawn over: every element whose bbox centre lies inside the scratch-out's
//  hull.  An overlap test would also take strokes the scratch-out merely grazed.  Going through
//  Selection::doSelect() is what keeps locked and hidden layers out of reach.
class ScratchOutSelector : public Selector
{
public:
  ScratchOutSelector(Selection* sel, const std::vector<Point>& area) : Selector(sel), polygon(area) {}

  bool selectHit(Element* s) override
  {
    if(s->node->type() == SvgNode::IMAGE)
      return false;  // an image under a scratch-out is far more likely background than the target
    Point center = s->bbox().center();
    bool inside = false;
    for(size_t i = 0, prev = polygon.size() - 1; i < polygon.size(); prev = i++) {
      const Point& pt0 = polygon[prev];
      const Point& pt1 = polygon[i];
      if((pt1.y > center.y) != (pt0.y > center.y)
          && center.x < pt0.x + (center.y - pt0.y)*(pt1.x - pt0.x)/(pt1.y - pt0.y))
        inside = !inside;
    }
    return inside;
  }

private:
  std::vector<Point> polygon;
};

void ScribbleArea::startShapeSnap(Point pos)
{
  stopShapeSnap();
  snapActive = false;
  const ScribblePen* pen = currPen();
  // a grid-snapped or line-drawing pen is already making straight lines its own way
  if(shapeSnapDelayMs(cfg) <= 0 || pen->hasFlag(ScribblePen::EPHEMERAL)
      || pen->hasFlag(ScribblePen::LINE_DRAWING) || pen->hasFlag(ScribblePen::SNAP_TO_GRID)
      || pen->hasFlag(ScribblePen::CENTER_ON_LINE))
    return;
  snapSamples.push_back(pos);
  snapHoldPos = pos;
  snapHoldStart = mSecSinceEpoch();
  snapHoldUsed = false;
  // a callback timer of its own rather than the widget's, which autoscroll and fling already share -
  //  the pen held perfectly still sends no events at all, so something has to notice the time passing
  if(ScribbleApp::gui) {
    snapTimer = ScribbleApp::gui->setTimer(SNAP_POLL_MS, NULL, [this]() {
      if(checkShapeSnap(mSecSinceEpoch()))
        return SNAP_POLL_MS;
      snapTimer = NULL;  // returning 0 removes it
      return 0;
    });
  }
}

void ScribbleArea::trackShapeSnap(Point pos)
{
  if(snapSamples.empty())
    return;
  snapSamples.push_back(pos);
  if(pos.dist(snapHoldPos)*mScale > SNAP_HOLD_RADIUS) {
    snapHoldPos = pos;
    snapHoldStart = mSecSinceEpoch();
    snapHoldUsed = false;
  }
}

void ScribbleArea::stopShapeSnap()
{
  if(snapTimer && ScribbleApp::gui)
    ScribbleApp::gui->removeTimer(snapTimer);
  snapTimer = NULL;
  snapSamples.clear();
}

bool ScribbleArea::checkShapeSnap(Timestamp now)
{
  if(snapSamples.empty() || snapActive || currMode != MODE_STROKE || !scribbleDoc->strokeBuilder)
    return false;
  int delay = shapeSnapDelayMs(cfg);
  if(delay <= 0)
    return false;
  if(snapHoldUsed || now - snapHoldStart < delay)
    return true;
  // one try per hold: a stroke that is not a shape stays ink, and drawing on is how to try again
  snapHoldUsed = true;
  if(!snapStroke())
    return true;
  snapSamples.clear();
  doRefresh();
  return false;
}

void ScribbleArea::discardStrokeBuilder()
{
  StrokeBuilder* builder = scribbleDoc->strokeBuilder;
  if(!builder)
    return;
  Element* stroke = builder->finish();
  scribbleDoc->updateCurrStroke(builder->getDirty());
  delete builder;
  scribbleDoc->strokeBuilder = NULL;
  if(stroke) {
    scribbleDoc->updateCurrStroke(stroke->bbox());
    stroke->deleteNode();
  }
}

// The scratch-out test alone over a stroke just finished; if it is one, its ink is dropped and what it
//  was drawn over erased.
bool ScribbleArea::scratchOutOnLift()
{
  std::vector<shaperec::Vec2> stroke;
  stroke.reserve(snapSamples.size());
  for(const Point& pt : snapSamples)
    stroke.emplace_back(pt.x*mScale, pt.y*mScale);
  shaperec::Result result = shaperec::recognizeScribble(stroke, shaperec::liftParams(cfg->Int("liftScratchOutLevel")));
  if(result.kind != shaperec::Kind::Scribble)
    return false;
  std::vector<Point> area;
  for(const shaperec::Vec2& pt : result.points)
    area.push_back(Point(pt.x/mScale, pt.y/mScale));
  discardStrokeBuilder();
  // doReleaseEvent has already started the action for this stroke and ends it
  scratchOut(area, false);
  return true;
}

// Runs the recognizer over the stroke so far and, if it is a shape, swaps the ink for it.  Returns false
//  (and changes nothing) if the stroke is not recognized.
bool ScribbleArea::snapStroke()
{
  // the recognizer's absolute thresholds (hold radius, hook length, staircase chords) are in screen
  //  pixels, so it has to see the stroke at the size it appears on screen
  std::vector<shaperec::Vec2> stroke;
  stroke.reserve(snapSamples.size());
  for(const Point& pt : snapSamples)
    stroke.emplace_back(pt.x*mScale, pt.y*mScale);
  shaperec::Result result = shaperec::recognize(stroke);
  if(cfg->Int("recordShapeStrokes") && !ScribbleApp::app->libraryRoot.empty()) {
    // the lab's .strokes format (labs/shape-recognition/strokefile.h); the label is left for a person to
    //  fill in with what was meant, the comment says what the recognizer made of it
    std::ofstream file(FSPath(ScribbleApp::app->libraryRoot, "shape-strokes.strokes").path, std::ios::app);
    file << "# recognized " << shaperec::kindName(result.kind) << ": " << result.reason << "\n"
         << "stroke " << mSecSinceEpoch() << "\nlabel unlabeled\npoints";
    for(const shaperec::Vec2& pt : stroke)
      file << " " << pt.x << " " << pt.y;
    file << "\nend\n";
  }
  auto toPage = [this](const shaperec::Vec2& pt) { return Point(pt.x/mScale, pt.y/mScale); };

  ShapeParams params;
  switch(result.kind) {
  case shaperec::Kind::Scribble:
  {
    std::vector<Point> area;
    for(const shaperec::Vec2& pt : result.points)
      area.push_back(toPage(pt));
    discardStrokeBuilder();
    scratchOut(area);
    // the rest of the gesture does nothing; release sees MODE_NONE
    currMode = MODE_NONE;
    return true;
  }
  case shaperec::Kind::Line:
    if(result.points.size() != 2)
      return false;
    params.id = SHAPE_LINE;
    params.points = {toPage(result.points[0]), toPage(result.points[1])};
    params.points[1] = snapRecognizedAngleAt(params, 1, params.points[1]);
    break;
  case shaperec::Kind::Quad:
  {
    if(result.points.size() != 4)
      return false;
    std::vector<Point> corners;
    for(const shaperec::Vec2& pt : result.points)
      corners.push_back(toPage(pt));
    // the recognizer snaps a nearly straight rectangle exactly onto the axes, so an exact test is enough
    Point side = corners[1] - corners[0];
    Dim eps = 1E-6*side.dist();
    if(std::abs(side.x) <= eps || std::abs(side.y) <= eps) {
      params.id = SHAPE_BOX;
      params.points = {corners[0], corners[2]};
    }
    else {
      // a box cannot be rotated (rotation demotes it), so a tilted rectangle is a closed polyline
      params.id = SHAPE_POLYLINE;
      params.flags = SHAPEFLAG_CLOSED;
      params.points = corners;
    }
    break;
  }
  case shaperec::Kind::Ellipse:
  {
    Point center = toPage(result.center);
    Dim radiusX = result.radiusX/mScale, radiusY = result.radiusY/mScale;
    Dim cosAngle = std::cos(result.angle), sinAngle = std::sin(result.angle);
    if(result.circle || std::abs(sinAngle) < 1E-6 || std::abs(cosAngle) < 1E-6) {
      if(!result.circle && std::abs(cosAngle) < 1E-6)
        std::swap(radiusX, radiusY);
      params.id = SHAPE_ELLIPSE;
      params.points = {center - Point(radiusX, radiusY), center + Point(radiusX, radiusY)};
    }
    else {
      // tightness 1 passes exactly through the points, which all lie on the ellipse
      params.id = SHAPE_CURVE;
      params.flags = SHAPEFLAG_CLOSED;
      params.tightness = 1;
      for(int i = 0; i < SNAP_ELLIPSE_POINTS; ++i) {
        Dim theta = 2*M_PI*i/SNAP_ELLIPSE_POINTS;
        Point local(radiusX*std::cos(theta), radiusY*std::sin(theta));
        params.points.push_back(center + Point(local.x*cosAngle - local.y*sinAngle,
            local.x*sinAngle + local.y*cosAngle));
      }
    }
    break;
  }
  default:
    return false;
  }

  discardStrokeBuilder();
  currStroke = createShapeElement(params);
  snapParams = currStroke->shapeParams();
  snapAnchor = snapSamples.back();
  snapActive = true;
  scribbleDoc->updateCurrStroke(currStroke->bbox());
  return true;
}

// After the snap the pen keeps control of the shape until it lifts: a line's end follows it, anything
//  closed is scaled about its centre by how far the pen has moved towards or away from that centre.
void ScribbleArea::scaleSnapShape(Point pos)
{
  if(!currStroke || snapParams.points.empty())
    return;
  ShapeParams params = snapParams;
  if(params.id == SHAPE_LINE) {
    params.points.back() += pos - snapAnchor;
    // by distance, like editing: the angle snap is only for the recognized shape itself
    params.points.back() = snapShapeAngleAt(params, 1, params.points.back());
  }
  else {
    Point lo = params.points.front(), hi = lo;
    for(const Point& pt : params.points) {
      lo = Point(std::min(lo.x, pt.x), std::min(lo.y, pt.y));
      hi = Point(std::max(hi.x, pt.x), std::max(hi.y, pt.y));
    }
    Point center = (lo + hi)/2;
    Dim startDist = snapAnchor.dist(center);
    if(startDist*mScale < 1)
      return;  // the pen is on the centre, so there is no distance to scale by
    Dim scale = std::max(Dim(0.05), pos.dist(center)/startDist);
    for(Point& pt : params.points)
      pt = center + (pt - center)*scale;
  }
  scribbleDoc->updateCurrStroke(currStroke->bbox());
  currStroke->setShapeParams(params);
  scribbleDoc->updateCurrStroke(currStroke->bbox());
}

// Called from the release with the undo action already open.  The shape as it snapped is always one
//  history step; if the gesture then changed it meaningfully, that change is a second step, so undo goes
//  back to what was recognized before it goes back to nothing.
void ScribbleArea::commitSnapShape()
{
  snapActive = false;
  Element* shape = currStroke;
  currStroke = NULL;
  if(!shape)
    return;
  Rect bbox = shape->bbox();
  scribbleDoc->updateCurrStroke(bbox);
  if(!bbox.isValid() || !currPage->rect().intersects(bbox) || (bbox.width() < 1 && bbox.height() < 1)) {
    shape->deleteNode();
    return;
  }
  if(!currPage->rect().contains(bbox))
    dirtyScreen(bbox);
  if(cfg->Bool("growWithPen"))
    growPage(bbox);

  ShapeParams scaled = shape->shapeParams();
  Dim moved = 0;
  if(scaled.points.size() == snapParams.points.size()) {
    for(size_t i = 0; i < scaled.points.size(); ++i)
      moved = std::max(moved, scaled.points[i].dist(snapParams.points[i]));
  }
  bool changed = moved*mScale > SNAP_CHANGE_MIN;
  if(changed)
    shape->setShapeParams(snapParams);
  currPage->addStroke(shape);  // creates the StrokeAddedItem
  // a shape is not handwriting, but it does end any stroke group in progress
  groupStrokes();
  if(changed) {
    scribbleDoc->endAction();
    scribbleDoc->startAction(currPageNum);  // closed by doReleaseEvent
    shape->setShapeParams(scaled);
    scribbleDoc->history->addItem(new ShapeChangedItem(shape, currPage, snapParams));
  }
  scribbleDoc->updateCurrStroke(shape->bbox());
}

// returns the number of elements erased
int ScribbleArea::scratchOut(const std::vector<Point>& area, bool ownAction)
{
  if(area.size() < 3)
    return 0;
  // declared in this order so the selector is destroyed first, while the selection it points to lives
  Selection erased(currPage, Selection::STROKEDRAW_NONE);
  ScratchOutSelector selector(&erased, area);
  erased.doSelect();
  int count = erased.count();
  if(count > 0) {
    if(ownAction)
      scribbleDoc->startAction(currPageNum);
    erased.deleteStrokes();
    if(ownAction)
      scribbleDoc->endAction();
  }
  return count;
}

// groupStrokes():
//  called with Stroke* to add to recent stroke list; if passed NULL pointer (the default param) or if
//  stroke is not estimated to be on the same rule line as the previous strokes, all strokes in the recent
//  strokes list will be processed and the list cleared.
// Originally, I had tried to use the undo list instead of saving the Stroke ptrs to separate list; this
//  became very messy; really, it was a dumb idea from the start - what was I thinking?!?

void ScribbleArea::groupStrokes(Element* b)
{
  if(!cfg->Bool("groupStrokes"))
    return;
  // a shape is not handwriting; passing NULL still ends any open group, which is what we want
  if(b && b->isShape())
    b = NULL;

  if(!recentStrokes.empty()) {
    const Timestamp GROUP_DT_MAX = 2500;  // 2.5 seconds
    // a group is measured against the ruling it was written on, and a stroke on a different ruling (in
    //  or out of a region) ends it
    const RulingFrame groupFrame = currPage->rulingAt(recentStrokes.front()->com());
    const Dim yruling = groupFrame.yrulingOr(Page::BLANK_Y_RULING);
    const Dim GROUP_DX_MIN = -0.25*yruling;
    const Dim GROUP_DX_MAX = 1.5*yruling;
    const Dim GROUP_DY_MIN = -1.25*yruling;
    const Dim GROUP_DY_MAX = 1.25*yruling;
    const Dim TIGHT_DY_MIN = -1.0*yruling;
    const Dim TIGHT_DY_MAX = 0.8*yruling;

    Element* a = recentStrokes.back();
    strokeGroupYCenter += a->bbox().center().y;
    Dim ycenter = strokeGroupYCenter/recentStrokes.size();
    Dim miny = ycenter + (recentStrokes.size() > 3 ? TIGHT_DY_MIN : GROUP_DY_MIN);
    Dim maxy = ycenter + (recentStrokes.size() > 3 ? TIGHT_DY_MAX : GROUP_DY_MAX);
    if(!b || a->timestamp() + GROUP_DT_MAX < b->timestamp()
          || currPage->regionAt(b->com()) != groupFrame.region
          || a->bbox().right + GROUP_DX_MAX < b->bbox().left
          || a->bbox().left + GROUP_DX_MIN > b->bbox().right
          || b->bbox().top < miny
          || b->bbox().bottom > maxy) {
      // end of group ... set com.y for each stroke to group's average
      // group must contain at least 4 strokes; the page-y alignment below means nothing on tilted lines,
      //  so a tilted region's handwriting is left ungrouped
      if(recentStrokes.size() > 3 && !groupFrame.isRotated()) {
        for(Element* s : recentStrokes)
          s->setCom(Point(s->com().x, ycenter));
        // special handling needed for whiteboarding since we are making a change outside the undo system
        scribbleDoc->strokesUpdated(recentStrokes);
      }
      // check for strokes in margin
      if(cfg->Int("bookmarkMode") == BookmarkView::MARGIN_CONTENT) {
        for(Element* s : recentStrokes) {
          if(s->bbox().right < currPage->marginLeft()) {
            scribbleDoc->document->bookmarksDirty = true;
            break;
          }
        }
      }
      // all done!
      recentStrokes.clear();
      strokeGroupYCenter = 0;
    }
  }
  if(b)
    recentStrokes.push_back(b);
}

void ScribbleArea::addRegionsToInsertSpace(Selection* sel, const std::function<bool(const Rect&)>& past)
{
  for(Element* region : sel->page->regions()) {
    if(past(region->bbox()))
      sel->addStroke(region);
  }
}

int ScribbleArea::dimToPageNum(const Point& pos) const
{
  if(viewMode == VIEWMODE_SINGLE)
    return currPageNum;

  Dim d = 0;
  unsigned int npages = numPages();
  if(viewMode == VIEWMODE_VERT) {
    for(unsigned int ii = 0; ii < npages; ii++) {
      d += page(ii)->height() + pageSpacing;
      if(d > pos.y)
        return ii;
    }
  }
  else if(viewMode == VIEWMODE_HORZ) {
    for(unsigned int ii = 0; ii < npages; ii++) {
      d += page(ii)->width() + pageSpacing;
      if(d > pos.x)
        return ii;
    }
  }
  return npages;  // past end (ghost page)
}

Point ScribbleArea::dimToPageDim(const Point& p) const
{
  return Point(p.x - currPageXOrigin, p.y - currPageYOrigin);
}

Rect ScribbleArea::dimToPageDim(Rect r) const
{
  return r.translate(-currPageXOrigin, -currPageYOrigin);
}

Point ScribbleArea::pageDimToDim(const Point& p) const
{
  return Point(p.x + currPageXOrigin, p.y + currPageYOrigin);
}

Rect ScribbleArea::pageDimToDim(Rect r) const
{
  return r.translate(currPageXOrigin, currPageYOrigin);
}

bool ScribbleArea::isPageVisible(int pagenum)
{
  return viewMode == VIEWMODE_SINGLE ? currPageNum == pagenum
      : pagenum < numPages() && isVisible(page(pagenum)->rect().translate(getPageOrigin(pagenum)));
}

// used by bookmarkview and mainwindow (for jumping to page)
// pos is in page Dim of specified page
void ScribbleArea::doGotoPos(int pagenum, Point pos, bool exact)
{
  doCancelAction();
  exact ? gotoPos(pagenum, pos) : viewPos(pagenum, pos);
  doRefresh();
}

void ScribbleArea::viewPos(int pagenum, Point pos)
{
  saveCurrPos(pagenum, pos);
  setPageNum(pagenum);
  setVisiblePos(pageDimToDim(pos));
}

void ScribbleArea::gotoPos(int pagenum, Point pos, bool savepos)
{
  if(savepos)
    saveCurrPos(pagenum, pos);
  setPageNum(pagenum);
  setCornerPos(pageDimToDim(pos));
}

// used for next page, prev page, so we never save previous position
void ScribbleArea::gotoPage(int pagenum)
{
  Point currpos = screenToDim(Point(0,0));
  if(pagenum >= numPages())  // goto end of last page
    gotoPos(numPages()-1, Point(viewMode == VIEWMODE_VERT ? currpos.x : -10, INT_MAX), false);
  else
    gotoPos(pagenum, Point(viewMode == VIEWMODE_VERT ? currpos.x : -10, -10), false);
}

// r is in page Dim of specified page
void ScribbleArea::viewRect(int pagenum, const Rect& r)
{
  if(pagenum != currPageNum)
    setPageNum(pagenum);
  if(!isVisible(pageDimToDim(r)))
    setCenterPos(pageDimToDim(r.center()));
}

// adjust position and zoom to view entire rect r - used for optional view syncing for shared whiteboarding
void ScribbleArea::setViewBox(const DocViewBox& vb)
{
  setPageNum(vb.pagenum);
  Rect r = pageDimToDim(vb.box);
  Rect s = screenToDim(screenRect);
  // ensure viewbox fits on screen
  Dim scale = std::min(s.width()/r.width(), s.height()/r.height());
  // don't set zoom level higher than master (e.g., in case our screen is bigger than master)
  Dim newzoom = zoomSteps[nearestZoomStep(std::min(scale*mZoom, vb.zoom))];
  if(newzoom != mZoom)
    setZoom(newzoom);
  setCenterPos(r.center());
}

// returns box enclosing page content only (not off-page regions); uses pagenum and page dim instead of
//  global dim to handle page growth on slaves due to annotations in SWB lecture mode
DocViewBox ScribbleArea::getViewBox() const
{
  Rect s = dimToPageDim(screenToDim(screenRect));
  // TODO: we don't account for multiple pages visible with different dimensions!
  if(viewMode != VIEWMODE_HORZ && s.width() > currPage->width()) {
    s.left = 0;
    s.right = currPage->width();
  }
  if(viewMode != VIEWMODE_VERT && s.height() > currPage->height()) {
    s.top = 0;
    s.bottom = currPage->height();
  }
  return DocViewBox(currPageNum, s, mZoom);
}

// ensure selection is visible
void ScribbleArea::viewSelection()
{
  if(currSelection)
    viewRect(currSelPageNum, currSelection->getBGBBox());
}

// save current position to history if major change
// posHistoryPos points to "current" pos / next avail slot to insert new pos
bool ScribbleArea::saveCurrPos(int newpagenum, Point newpos)
{
  Point origin = getPageOrigin(newpagenum);
  newpos.x += origin.x;
  newpos.y += origin.y;
  Point currpos = screenToDim(Point(0,0));
  Point lastpos = newpos;
  // this is to avoid creating sequential history entries near the same position
  if(posHistoryPos != posHistory.begin()) {
    DocPosition lastdocpos = *(posHistoryPos - 1);
    origin = getPageOrigin(lastdocpos.pagenum);
    lastpos.x = lastdocpos.pos.x + origin.x;
    lastpos.y = lastdocpos.pos.y + origin.y;
  }
  Dim mindx = getViewWidth()/2;
  Dim mindy = getViewHeight()/2;
  if((ABS(newpos.x - currpos.x) > mindx || ABS(newpos.y - currpos.y) > mindy)
      && (ABS(currpos.x - lastpos.x) > mindx || ABS(currpos.y - lastpos.y) > mindy)) {
    posHistory.erase(posHistoryPos, posHistory.end());
    posHistory.push_back(getPos());
    posHistoryPos = posHistory.end();
    return true;
  }
  return false;
}

void ScribbleArea::prevView()
{
  if(posHistoryPos != posHistory.begin()) {
    DocPosition docpos = *(posHistoryPos - 1);
    if(posHistoryPos == posHistory.end()) {
      if(saveCurrPos(docpos.pagenum, docpos.pos))
        posHistoryPos--;
    }
    posHistoryPos--;
    gotoPos(docpos.pagenum, docpos.pos, false);
  }
}

void ScribbleArea::nextView()
{
  if(posHistoryPos != posHistory.end())
    posHistoryPos++;
  if(posHistoryPos != posHistory.end()) {
    DocPosition docpos = *posHistoryPos;
    gotoPos(docpos.pagenum, docpos.pos, false);
  }
}

DocPosition ScribbleArea::getPos() const
{
  return DocPosition(currPageNum, dimToPageDim(screenToDim(Point(0,0))));
}

// double tap zooms to the same gapless fit width a pinch snaps to; a second one, at fit, back to 100%
void ScribbleArea::doDblClickAction(Point pos)
{
  //scribbleDoc->setActiveArea(this);
  int pagenum = dimToPageNum(screenToDim(pos));
  Dim wzoom = fitWidthZoom(pagenum);
  if(std::abs(mZoom/wzoom - 1) < 1E-3) {
    zoomTo(1, pos.x, pos.y);
    roundZoom(pos.x, pos.y); // update zoom step index
  }
  else {
    zoomTo(wzoom, pos.x, pos.y);
    zoomStepsIdx = nearestZoomStep(mZoom);
    alignFitPage(pagenum, true);
  }
  uiChanged(UIState::Zoom);
}

bool ScribbleArea::viewHref(const char* href)
{
  int targetpagenum = currPageNum;  // search is done backwards from this page
  SvgNode* n = scribbleDoc->document->findNamedNode(href, &targetpagenum);
  if(!n)
    return false;
  Rect r = n->bounds();
  Page* pg = page(targetpagenum);
  Point cornerpos(r.left - 5, std::min(r.top, pg->getYforLine(pg->getLine(r.center().y)) - 5));
  viewPos(targetpagenum, cornerpos);
  return true;
}

// handle clicking on links
bool ScribbleArea::doClickAction(Point pos)
{
  //scribbleDoc->setActiveArea(this);
  // check for click on link
  Point gpos = screenToDim(pos);
  int pagenum = dimToPageNum(gpos);
  if(pagenum >= numPages())
    return false;
  // change the page ... should we be doing this? (Note that the pan tool already did this)
  if(pagenum != currPageNum)
    setPageNum(pagenum);
  const char* href = page(pagenum)->getHyperRef(dimToPageDim(gpos));
  // check for bookmark link
  if(!href || !href[0]) {}
  else if(href[0] == '#')
    viewHref(href);
  else
    scribbleDoc->openURL(href);
  return href != NULL;
}

// a two finger tap undoes one step
void ScribbleArea::doTwoFingerTap()
{
  scribbleDoc->doCommand(ID_UNDO);
}

// how near (screen units) a finger tap must land to a shape's outline: a fingertip is far blunter than
//  the pen PATHSELECT_RADIUS is sized for
static constexpr Dim TOUCH_TAP_RADIUS = 16;

// the shape or image a finger tap at pos (page coords) lands on, topmost first; handwriting is not
//  taken - a tap is too blunt to pick one stroke out of a word, and that is the Select tool's job
Element* ScribbleArea::touchTapTarget(Point pos) const
{
  Dim radius = TOUCH_TAP_RADIUS/mZoom;
  Element* hit = NULL;  // the last hit is the topmost, as it is drawn over the others
  for(Element* s : currPage->children()) {
    // a locked or hidden layer is out of reach here as it is for the Select tool
    if(!currPage->isEditable(s) || s->isRulingRegion() || s->isPageTag())
      continue;
    if(s->node->type() == SvgNode::IMAGE) {
      if(s->bbox().contains(pos))
        hit = s;
    }
    else if(s->isShape() && s->node->type() == SvgNode::PATH && Rect(s->bbox()).pad(radius).contains(pos)) {
      const Path2D& path = *static_cast<SvgPath*>(s->node)->path();
      const Transform2D tf = s->node->totalTransform();
      Dim dist = tf.isIdentity() ? path.distToPoint(pos) : Path2D(path).transform(tf).distToPoint(pos);
      if(dist < radius)
        hit = s;
    }
  }
  return hit;
}

// select one element as the Select tool would, with a shape's parameter handles up - without changing
//  the active tool, which is the point: fixing a shape that snapped a little wrong should not cost a trip
//  to the Select tool and back
void ScribbleArea::selectTapped(Element* target)
{
  // an open polyline is finished (not selected) first, as any other tool switch would - after this the
  //  shape tool must not go on adding points to it behind the selection
  finishShape();
  clearSelection();
  currSelection = new Selection(currPage);
  currSelPageNum = currPageNum;
  currSelection->addStroke(target);
  rectSelector = new RectSelector(currSelection, mZoom, true);
  useShapeSelector();
  currSelection->shrink();
  dirtyScreen(currSelection->getBGBBox());
  uiChanged(UIState::SelChange);
  if(cfg->Bool("popupToolbar")) {
    Rect r = currSelection->getBGBBox();
    app->showSelToolbar(screenToGlobal(dimToScreen(pageDimToDim(Point(r.right, r.bottom)))));
  }
}

// A finger tap where touch pans (i.e. a pen is in use) and no link took it: on a shape or image it selects
//  that element and enters its edit mode, whatever the tool; anywhere else it clears the selection - the
//  pen-free way out of shape edit mode.  A tap on the selection itself keeps it.
bool ScribbleArea::doTouchTap(Point pos)
{
  if(placingTags())
    return false;
  Point gpos = screenToDim(pos);
  int pagenum = dimToPageNum(gpos);
  Element* target = NULL;
  if(pagenum < numPages()) {
    if(pagenum != currPageNum)
      setPageNum(pagenum);
    Point pagepos = dimToPageDim(gpos);
    if(currSelection && currSelPageNum == currPageNum && selectionHit(pagepos, true))
      return true;
    target = touchTapTarget(pagepos);
  }
  if(target) {
    selectTapped(target);
    return true;
  }
  if(!currSelection)
    return false;
  clearSelection();
  uiChanged(UIState::SelChange);
  return true;
}

// consolidated fn for setting properties of current selection, including hyperref and bookmark creation
// initial motivation was to ensure only one undo item was created; also reduces code duplication
void ScribbleArea::setSelProperties(const StrokeProperties* props, const char* target, Element* bkmktarget,
    const char* idstr, bool forcenormal)
{
  doCancelAction();
  if(!currSelection)
    return;
  viewSelection();
  scribbleDoc->startAction(currPageNum);

  if(props && (props->color != Color::INVALID_COLOR || props->width > 0))
    currSelection->setStrokeProperties(*props);

  // create sorted list of strokes, flattening any hyperrefs and reverting any bookmarks
  std::vector<Element*> sorted;
  if(target || bkmktarget || idstr || forcenormal) {
    std::vector<Element*> sorted0;
    sorted0.reserve(currSelection->count());
    for(Element* s : currSelection->sourceNode->children()) {
      if(s->isSelected(currSelection))
        sorted0.push_back(s);
    }

    // can't (easily) add/remove strokes from page or selection while iterating, so do in 2 stages
    sorted.reserve(sorted0.size());
    for(Element* s : sorted0) {
      // flatten our structure nodes (but not external ones)
      if(s->isMultiStroke()) {
        for(Element* ss : s->children()) {
          Element* t = ss->cloneNode();
          currPage->addStroke(t, s);
          if(s->node->hasTransform()) {
            t->applyTransform(s->node->getTransform());
            t->commitTransform();
          }
          currSelection->addStroke(t);
          sorted.push_back(t);
        }
        currSelection->removeStroke(s);
        currPage->removeStroke(s);  // delete original
      }
      else if(s->isBookmark()) {
        Element* t = s->cloneNode();
        t->node->removeClass("bookmark");
        currPage->addStroke(t, s);
        currSelection->addStroke(t);
        currSelection->removeStroke(s);
        currPage->removeStroke(s);  // delete original
        sorted.push_back(t);
      }
      else
        sorted.push_back(s);
    }
  }

  if(target || bkmktarget) {
    // create hyperref, then *replace* strokes with it
    Element* h = new Element(new SvgG());  //SvgNode::A));
    h->node->addClass("hyperref");
    for(Element* s : sorted)
      h->addChild(s->cloneNode());
    // invisible rect (for browsers) is now inserted in Element::serializeAttr
    if(bkmktarget) {
      // bookmark pasted from clipboard (or in legacy document) may not have id set
      if(!bkmktarget->nodeId() || !bkmktarget->nodeId()[0] ) {
        // replace bookmark w/ labeled copy since we don't have a way to undo setting id string of original
        Element* newbkmk = bkmktarget->cloneNode();
        Page* page = scribbleDoc->document->pageForElement(bkmktarget);
        newbkmk->setNodeId(("b-" + randomStr(10)).c_str());
        page->addStroke(newbkmk, bkmktarget);
        page->removeStroke(bkmktarget);
        bkmktarget = newbkmk;
      }
      h->sethref((std::string("#") + bkmktarget->nodeId()).c_str());
    }
    else
      h->sethref(target);
    currPage->addStroke(h, sorted.back());
    currSelection->deleteStrokes();
    currSelection->addStroke(h);
  }
  else if(idstr) {
    // convert to bookmark with id
    Element* bkmk;
    if(sorted.size() > 1) {
      //SvgG* bkmkNode = new SvgG(NULL);
      bkmk = new Element(new SvgG());  //static_cast<Element*>(bkmkNode->ext());
      for(Element* s : sorted)
        bkmk->containerNode()->addChild(s->cloneNode()->node);
    }
    else
      bkmk = sorted.front()->cloneNode();
    bkmk->setNodeId(idstr);
    bkmk->node->addClass("bookmark");
    currPage->addStroke(bkmk, sorted.back());
    currSelection->deleteStrokes();
    currSelection->addStroke(bkmk);
  }
  scribbleDoc->endAction();
  uiChanged(UIState::SetSelProps);
  doRefresh();
}

// apply the head toggles to an already-drawn shape, so adding an arrow to an existing polyline does not
//  mean redrawing it.  Returns false if the selection is not a single shape that can carry heads.
bool ScribbleArea::setSelShapeOptions(int flags, Dim radius)
{
  doCancelAction();
  if(!currSelection || currSelection->count() != 1)
    return false;
  Element* shape = currSelection->strokes.front();
  const ShapeDef* def = shape->isShape() ? shapeDef(shape->shapeParams().id) : NULL;
  if(!def)
    return false;
  // only the toggles that mean something for this shape may touch it, so switching tools with a shape
  //  still selected cannot strip flags the shape legitimately carries
  int mask = (def->allowsHeads ? (SHAPEFLAG_HEADSTART | SHAPEFLAG_HEADEND) : 0)
      | (def->allowsRounding ? SHAPEFLAG_ROUNDED : 0);
  if(!mask)
    return false;
  ShapeParams params = shape->shapeParams();
  int newflags = (params.flags & ~mask) | (flags & mask);
  // a shape switched to rounded for the first time has no radius yet; give it the configured one
  Dim newrx = params.rx, newry = params.ry;
  if((newflags & SHAPEFLAG_ROUNDED) && newrx <= 0 && newry <= 0) {
    newrx = radius;
    newry = radius;
  }
  if(newflags == params.flags && newrx == params.rx && newry == params.ry)
    return true;
  viewSelection();
  dirtyScreen(currSelection->getBGBBox());
  scribbleDoc->startAction(currPageNum);
  scribbleDoc->history->addItem(new ShapeChangedItem(shape, currPage, params));
  params.flags = newflags;
  params.rx = newrx;
  params.ry = newry;
  shape->setShapeParams(params);
  currSelection->invalidateBBox();
  if(shapeSelector)
    shapeSelector->updateHandles();
  currSelection->xchgBGDirty(true);
  scribbleDoc->endAction();
  dirtyScreen(currSelection->getBGBBox());
  uiChanged(UIState::SetSelProps);
  doRefresh();
  return true;
}

// return the target of the first HyperRef found in currSelection or NULL
const char* ScribbleArea::getHyperRef() const
{
  Element* s;
  if(currSelection && (s = currSelection->getFirstElement( [](const Element* t){ return t->isHyperRef(); } )))
    return s->href();
  return NULL;
}

// return the idStr of the first bookmark in currSelection
const char* ScribbleArea::getIdStr() const
{
  Element* s;
  if(currSelection && (s = currSelection->getFirstElement( [](const Element* t){ return t->isBookmark(); } )))
    return s->nodeId();
  return NULL;
}

void ScribbleArea::ungroupSelection()
{
  if(!currSelection)
    return;
  viewSelection();
  scribbleDoc->startAction(currPageNum);
  currSelection->ungroup();
  scribbleDoc->endAction();
}

Rect ScribbleArea::insertImage(Image image, const Rect& below)
{
  static constexpr Dim STACK_GAP = 20;

  doCancelAction();
  Clipboard clip;
  Dim imgw = image.getWidth()*unitsPerPx, imgh = image.getHeight()*unitsPerPx;
  Dim s = std::min(Dim(1), std::min(currPage->width()/2/imgw, currPage->height()/2/imgh));
  // doPasteAt pulls an image that would hang off the page back onto it, so a stack that runs out of
  //  room overlaps at the bottom of the page rather than disappearing
  Point center = below.isValid() ? Point(below.center().x, below.bottom + STACK_GAP + imgh*s/2)
      : screenToDim(screenRect.center());
  Rect bbox = Rect::centerwh(center, imgw*s, imgh*s);
  clip.addStroke(new Element(new SvgImage(std::move(image), bbox)));
  doPasteAt(&clip, center, PasteFlags(PasteOrigPos | PasteMoveClipboard | PasteUndoable));
  uiChanged(UIState::Paste);
  doRefresh();
  return currSelection ? pageDimToDim(currSelection->getBBox()) : Rect();
}

// page units around ink captured without a drawn outline: ~4 mm, enough that the capture does not look
//  cut off at the strokes
static constexpr Dim SCREENSHOT_INK_PADDING = 25;

// Captures the bounding box of the selection gesture (or, for a ruled selection, of the selected ink) in
//  the document's own colors, offers a crop, then copies it or adds it to the current page as an image.
void ScribbleArea::screenshotSelection()
{
  captureScreenshot();
  // an area marked with nothing selected existed only to be captured; its popup is gone now too
  if(!currSelection)
    clearShotRegion();
}

void ScribbleArea::captureScreenshot()
{
  int pagenum = !shotRegion.empty() ? shotRegionPage : currSelection ? currSelPageNum : -1;
  // A ruled or path selection has no outline of its own, only the ink it caught, whose bbox runs right up
  //  to the strokes; the padding gives the capture a margin.  A drawn outline is taken as the user drew it.
  Rect region = !shotRegion.empty() ? shotRegion.getBBox()
      : currSelection ? currSelection->getBBox().pad(SCREENSHOT_INK_PADDING) : Rect();
  Page* srcPage = pagenum >= 0 && pagenum < numPages() ? page(pagenum) : NULL;
  if(!srcPage || !region.isValid())
    return;
  region = region.rectIntersect(srcPage->rect());
  if(!region.isValid() || region.width() < 1 || region.height() < 1)
    return;
  Dim scale = screenshotScale(region);
  ScreenshotDialog dialog([srcPage, region, scale](int layers){
    return renderPageRegion(srcPage, region, scale, layers);
  });
  if(Application::execDialog(&dialog) != Dialog::ACCEPTED)
    return;
  Rect crop = dialog.cropRect();
  Image shot = dialog.takeCropped();
  if(shot.width <= 0 || shot.height <= 0)
    return;
  // at its real size: the crop in page units, where it was on the source page
  Rect bbox = Rect::ltwh(region.left + crop.left/scale, region.top + crop.top/scale,
      crop.width()/scale, crop.height()/scale);
  if(dialog.choice == ScreenshotDialog::CHOICE_COPY) {
    Clipboard* clip = new Clipboard;
    clip->addStroke(new Element(new SvgImage(std::move(shot), bbox)));
    app->setClipboard(clip, srcPage, 0);
    app->refreshUI(scribbleDoc, UIState::ClipboardChange);  // enables Paste, which may have had nothing to paste
    return;
  }
  // Add to page: dropped at the middle of the view, selected, so it can be dragged where it belongs
  clearSelection();
  doCancelAction();
  Clipboard clip;
  Point center = screenToDim(screenRect.center());
  clip.addStroke(new Element(new SvgImage(std::move(shot), Rect::centerwh(center, bbox.width(), bbox.height()))));
  doPasteAt(&clip, center, PasteFlags(PasteOrigPos | PasteMoveClipboard | PasteUndoable));
  uiChanged(UIState::Paste);
  doRefresh();
}

void ScribbleArea::freeErase(Point prevpos, Point pos)
{
  // for now, let's fix the eraser size in screen space so that user can zoom to adjust how much is erased
  //  rather than selecting from different sized erasers
  Dim radius = ERASEFREE_RADIUS/mZoom;
  bool touched = false;
  Rect erasebox = Rect::corners(prevpos, pos).pad(radius);
  auto strokes = currPage->children();
  for(auto ii = strokes.begin(); ii != strokes.end();) {
    Element* s = *ii++;
    // the free eraser is the one erase path that is not a Selection, so the lock check that covers
    //  the stroke and ruled erasers in Selection::doSelect does not reach it
    if(!currPage->isEditable(s))
      continue;
    if(!s->isSelected(tempSelection) && erasebox.intersects(s->bbox())) {
      if(s->isSelected(freeErasePieces)) {
        //Rect oldbbox = s->bbox();
        touched = s->freeErase(prevpos, pos, radius) || touched;
          //currPage->growDirtyRect(oldbbox.rectUnion(s->bbox()));
      }
      else {
        Element* s2 = s->cloneNode();
        if(s2->freeErase(prevpos, pos, radius)) {
          Element* nexts = ii != strokes.end() ? *ii : NULL;
          currPage->contentNode->addChild(s2->node, nexts ? nexts->node : NULL);
          ii = std::find(strokes.begin(), strokes.end(), nexts);
          freeErasePieces->addStroke(s2);
          // hide original stroke
          tempSelection->addStroke(s);
          //currPage->growDirtyRect(s->bbox());
          touched = true;
        }
        else
          s2->deleteNode();  //delete s2;
      }
    }
  }
  // Element::freeErase() does not set anything dirty
  if(touched)
    scribbleDoc->updateCurrStroke(erasebox.pad(1));
}

// ruled free eraser: like freeErase() above, but instead of a capsule swept between two points, erases
//  against the rect spanning [xmin,xmax] (padded by radius) x the current ruled line's row - so only the
//  portion of a stroke's ink that falls within that line's row is removed (e.g. a descender dipping into
//  the line below is left untouched), combining ruled eraser's line-sweep gesture with free eraser's
//  partial (non-whole-stroke) removal
void ScribbleArea::freeEraseRuled(Dim xmin, Dim xmax, int line)
{
  Dim radius = ERASEFREE_RADIUS/mZoom;
  // the line's band in the gesture's ruling (xmin/xmax are along its lines); a plain rect for the page's
  //  own ruling, a tilted quad in a tilted region
  std::vector<Point> band = gestureFrame.bandPolygon(line, xmin - radius, xmax + radius, Page::BLANK_Y_RULING);
  Rect erasebox;
  for(const Point& p : band)
    erasebox.rectUnion(p);
  bool touched = false;
  auto strokes = currPage->children();
  for(auto ii = strokes.begin(); ii != strokes.end();) {
    Element* s = *ii++;
    if(!currPage->isEditable(s))
      continue;  // see freeErase()
    // ink belongs to the ruling it sits on, as for RuledSelector::selectHit; pieces this gesture already
    //  cut are exempt, since cutting can move a piece's centre across a region's edge
    if(!s->isSelected(freeErasePieces) && currPage->regionAt(s->com()) != gestureFrame.region)
      continue;
    if(!s->isSelected(tempSelection) && erasebox.intersects(s->bbox())) {
      if(s->isSelected(freeErasePieces)) {
        touched = s->freeErase(band) || touched;
      }
      else {
        Element* s2 = s->cloneNode();
        if(s2->freeErase(band)) {
          Element* nexts = ii != strokes.end() ? *ii : NULL;
          currPage->contentNode->addChild(s2->node, nexts ? nexts->node : NULL);
          ii = std::find(strokes.begin(), strokes.end(), nexts);
          freeErasePieces->addStroke(s2);
          // hide original stroke
          tempSelection->addStroke(s);
          touched = true;
        }
        else
          s2->deleteNode();  //delete s2;
      }
    }
  }
  if(touched)
    scribbleDoc->updateCurrStroke(erasebox.pad(1));
}

// dispatch fn for commands

void ScribbleArea::doCommand(int itemid)
{
  doCancelAction();
  switch(itemid) {
  case ID_SELRECENT: recentStrokeSelect();  break;
  case ID_DESELRECENT:  recentStrokeDeselect();  break;
  case ID_SELALL:  selectAll();  break;
  case ID_SELSIMILAR:  selectSimilar();  break;
  case ID_INVSEL:  invertSelection();  break;
  case ID_EXPANDDOWN:  expandDown();  break;
  case ID_EXPANDRIGHT:  expandRight();  break;
  case ID_ZOOMIN:  zoomIn();  break;
  case ID_ZOOMOUT:  zoomOut();  break;
  // Zoom all and zoom width (restored from r610) are not exposed in UI
  case ID_ZOOMALL:
    setZoom(std::min(getViewWidth()/currPage->width()/preScale, getViewHeight()/currPage->height()/preScale));
    setCenterPos(pageDimToDim(Point(currPage->width()/2/preScale, currPage->height()/2/preScale)));
    break;
  case ID_ZOOMWIDTH:
    setZoom(getViewWidth()/currPage->width()/preScale);
    setCornerPos(pageDimToDim(Point(0, 0)));
    break;
  case ID_RESETZOOM:  resetZoom();  break;
  case ID_PREVVIEW:  prevView();  break;
  case ID_NEXTVIEW:  nextView();  break;
  case ID_PREVPAGE:  prevPage();  break;
  case ID_NEXTPAGE:  nextPage(false);  break;
  case ID_NEXTPAGENEW:  nextPage(true);  break;
  case ID_PREVSCREEN:  doPan(0, screenRect.height());  break;  // doPan dx,dy are in screen, not dim units!
  case ID_NEXTSCREEN:  doPan(0, -screenRect.height());  break;
  case ID_SCROLLUP:  doPan(0, 20);  break;
  case ID_SCROLLDOWN:  doPan(0, -20);  break;
  case ID_STARTOFDOC:  gotoPage(0);  break;
  case ID_ENDOFDOC:  gotoPage(numPages());  break;  // >=numPages() takes us to end of last page
  case ID_UNGROUP:  ungroupSelection();  break;
  default:  return;
  }
  uiChanged(UIState::Command);
  doRefresh();
}

void ScribbleArea::uiChanged(int reason)
{
  scribbleDoc->uiChanged(reason);
}

void ScribbleArea::doRefresh()
{
  scribbleDoc->doRefresh();
}

void ScribbleArea::updateUIState(UIState* state)
{
  if(!currPage) return;  // prevent crash ... saw on Android, not sure how
  // move,invert selection, select similar, delete, copy require an active selection
  state->activeSel = (currSelection != NULL);
  state->pageSel = scribbleDoc->numSelPages > 0;
  state->regionSel = regionSelector != NULL;
  // a region's selection holds a <g> (the region), which is not a group the user can ungroup
  state->selHasGroup = currSelection && !regionSelector && currSelection->containsGroup();
  state->pagemodified = (currPage->dirtyCount != 0);
  state->pageNum = currPageNum+1;
  state->totalPages = numPages();
  state->zoom = mZoom;
  state->currPageStrokes = currPage->strokeCount();
  state->currSelStrokes = currSelection ? currSelection->count() : 0;
  state->prevView = posHistoryPos != posHistory.begin();
  state->nextView = posHistoryPos != posHistory.end() && (posHistoryPos + 1) != posHistory.end();
  state->pageWidth = currPage->width();
  state->pageHeight = currPage->height();

  if(currSelection)
    currSelection->recalcTimeRange();  // will only calculate if not set
  if(currSelection && currSelection->maxTimestamp > 0) {
    state->minTimestamp = currSelection->minTimestamp;
    state->maxTimestamp = currSelection->maxTimestamp;
  }
  else {
    currPage->recalcTimeRange();
    state->minTimestamp = currPage->minTimestamp;
    state->maxTimestamp = currPage->maxTimestamp;
  }
}

const ScribblePen* ScribbleArea::currPen() const
{
  return app->getPen();
}

// the text marker stores its width as a fraction of the page's line height (ScribblePen::
//  WIDTH_RELATIVE); everything downstream - stroke builders, shapes, the hover cursor - wants
//  document units, so this is the single place the multiplication happens.  Note the resolved width is
//  what lands in the document: strokes are always saved with an absolute stroke-width, so nothing in
//  the file format, the undo history or sync has to know about relative widths.
ScribblePen ScribbleArea::resolvedPen(Point at) const
{
  ScribblePen pen = *currPen();
  if(pen.hasFlag(ScribblePen::WIDTH_RELATIVE)) {
    if(!currPage)
      pen.width *= Page::BLANK_Y_RULING;
    else if(at.isNaN())
      pen.width *= currPage->yruling(true);
    else  // inside a ruling region, "a line" means the region's line
      pen.width *= currPage->rulingAt(at).yrulingOr(Page::BLANK_Y_RULING);
    // the dash pattern is sized in the width's unit (ScribblePen::dashFor), so it resolves with it
    pen.setDashStyle(currPen()->dashStyle());
  }
  return pen;
}

int ScribbleArea::selectionHit(Point pos, bool touch)
{
  if(!currSelection || !currSelection->selector)
    return 0;
  // a shape's parameter handles replace the scale/rotate handles, so check them first
  shapeHandleIdx = currSelection->selector->shapeHandleHit(pos, touch);
  if(shapeHandleIdx >= 0)
    return MODEMOD_SHAPEHANDLE;
  // check for selection scale handle hit
  scaleOrigin = currSelection->selector->scaleHandleHit(pos, touch);
  if(!scaleOrigin.isNaN()) {
    // bottom right corner scales with fixed aspect ratio; others scale freely.  A region never gets here:
    //  its size handle is a shape handle (RegionSelector::resized).
    scaleLockRatio = scaleOrigin.x < pos.x && scaleOrigin.y < pos.y;
    prevXScale = 1;
    prevYScale = 1;
    return MODEMOD_SCALESEL;
  }

  // next check for rotate handle hit
  scaleOrigin = currSelection->selector->rotHandleHit(pos, touch);
  if(!scaleOrigin.isNaN())
    return MODEMOD_ROTATESEL;

  scaleOrigin = currSelection->selector->cropHandleHit(pos, touch);
  if(!std::isnan(scaleOrigin.x) || !std::isnan(scaleOrigin.y)) {
    prevXScale = 1;
    prevYScale = 1;
    return MODEMOD_CROPSEL;
  }

  // hackish way to see if cursor is down within the (possibly complex) selection region
  SvgPath testStroke(Path2D().addLine(pos, pos), SvgNode::LINE);
  if(currSelection->selector->selectHit(new Element(&testStroke)))
    return MODEMOD_MOVESEL;

  return 0;
}

// For handling touch events, see the touch/fingerpaint example
void ScribbleArea::doPressEvent(const InputEvent& event)
{
  // an area marked with nothing selected is only there for its popup; any press on the canvas moves on
  if(!currSelection)
    clearShotRegion();
  int modemod = event.modemod;
  Point rawpos = Point(event.points[0].x, event.points[0].y);
  Point gpos = screenToDim(rawpos);
  Point pos = dimToPageDim(gpos);
  // take focus
  //scribbleDoc->setActiveArea(this);
  // TODO: maybe refactor this, move logic into setCurrPos
  if((viewMode == VIEWMODE_VERT && (pos.y < -pageSpacing || pos.y > currPage->height() + pageSpacing))
      || (viewMode == VIEWMODE_HORZ && (pos.x < -pageSpacing || pos.x > currPage->width() + pageSpacing))) {
    int newpagenum = dimToPageNum(gpos);
    if(newpagenum != currPageNum) {
      setPageNum(newpagenum);
      pos = dimToPageDim(gpos);
    }
  }

  // tags waiting to be placed go where this press is, on the page it is on (the release puts them there)
  if(placingTags()) {
    placingPressed = true;
    currMode = MODE_NONE;
    movePendingTags(pos);
    prevPos = initialPos = pos;
    prevRawPos = rawpos;
    return;
  }

  // a to-do tag's checkbox, like a region's "..." button, is tested before the tool gets the press - but
  //  not for the eraser, which should be able to start on one
  todoPressed = NULL;
  if(!(modemod & (MODEMOD_PENBTN | MODEMOD_ERASE | MODEMOD_EDGEMASK))) {
    int toolMode = scribbleDoc->getScribbleMode(modemod);
    bool erasing = toolMode == MODE_ERASE || toolMode == MODE_ERASESTROKE || toolMode == MODE_ERASERULED
        || toolMode == MODE_ERASEFREE || toolMode == MODE_ERASEFREERULED;
    if(!erasing && (todoPressed = todoBoxHit(pos, event.source == INPUTSOURCE_TOUCH))) {
      currMode = MODE_NONE;
      prevPos = initialPos = pos;
      prevRawPos = rawpos;
      return;
    }
  }

  // offset should be a property of RuledSelector, not Page, but this is easier for now
  if(currPage->yruling() == 0)
    currPage->yRuleOffset = fmod(pos.y - Page::BLANK_Y_RULING/2, Page::BLANK_Y_RULING);

  switch(cfg->Int("panFromEdge")) {
  case 1: {
    Dim border = cfg->Float("panBorder") * preScale;
    // all edges are treated the same for now, so just set EDGEMASK
    if(rawpos.x < border || rawpos.x > getViewWidth() - border || rawpos.y > getViewHeight() - border)
      modemod |= MODEMOD_EDGEMASK;
    break;
  }
  case 2:
    // pointer down off page pans instead of draws
    if(pos.x < 0 || pos.x > currPage->width() || pos.y < 0 || pos.y > currPage->height())
      modemod |= MODEMOD_EDGEMASK;
  }
  // A live shape or bookmark element belongs to the gesture that created it and is held here rather than
  //  in the page, so it is painted every frame but is not part of the document.  If a new press arrives
  //  with one still held, that gesture was abandoned without going through release or cancel - the input
  //  layer restarts a gesture when the pen button changes mid-stroke, for one.  Dropping it here is the
  //  invariant that keeps an abandoned gesture from leaving a shape on screen that belongs to no page
  //  and so cannot be selected, erased or deleted.
  stopShapeSnap();
  snapActive = false;
  if(currStroke) {
    scribbleDoc->updateCurrStroke(currStroke->bbox());
    currStroke->deleteNode();
    currStroke = NULL;
  }
  gestureFrame = currPage->gestureFrame(pos);
  gridFrame = currPage->rulingAt(pos);
  if(!gridFrame.region)
    gridFrame.origin = Point(0, 0);  // the page's grid is anchored at its origin, never at yRuleOffset
  prevLine = gestureFrame.line(pos, Page::BLANK_Y_RULING);
  initialLine = prevLine;
  initialPageSize = currPage->rect();
  // position along the ruled lines, which for the page's own ruling is just pos.x
  Dim lx = gestureFrame.toLocal(pos).x;
  // a region has no margin, so nothing in it counts as "in the left margin"
  Dim marginLeft = gestureFrame.region ? MIN_DIM : currPage->marginLeft();

  // a region's "..." button is the one way to select the region; it sits over the region's ink, so it is
  //  tested before anything else, and only with no modifier (pen button, eraser end) that asks otherwise
  if(regionButtonsShown() && !(modemod & (MODEMOD_PENBTN | MODEMOD_ERASE | MODEMOD_EDGEMASK))) {
    if(Element* region = regionButtonHit(pos, event.source == INPUTSOURCE_TOUCH)) {
      if(selectedRegion() != region)
        selectRegion(region);
      currMode = MODE_NONE;
      prevPos = initialPos = pos;
      prevRawPos = rawpos;
      return;
    }
  }

  bool selvisible = currSelection &&
      currSelPageNum == currPageNum && isVisible(pageDimToDim(currSelection->getBGBBox()));
  if(selvisible)
    modemod |= selectionHit(pos, event.source == INPUTSOURCE_TOUCH);
  // get the mode!
  currMode = scribbleDoc->getScribbleMode(modemod);
  // Down and Right are ruled insert space held to one axis: everything about the gesture - selection, Skip
  //  Lines, region slop, erase, page growth - is ruled insert space's, only the drag is cut to one direction
  insSpaceAxis = MODE_INSSPACERULED;
  if(currMode == MODE_INSSPACEDOWN || currMode == MODE_INSSPACERIGHT) {
    insSpaceAxis = currMode;
    currMode = MODE_INSSPACERULED;
  }
  // do pan-from-edge through ScribbleInput to avoid inappropriately reverting to sticky tool after panning
  if(currMode == MODE_PAN && modemod & MODEMOD_EDGEMASK) {
    currMode = MODE_NONE;
    scribbleInput->cancelAction();
    scribbleInput->forcePanMode(event);
  }
  // Ruled insert space and ruled select pressed just outside a region still act on it: with the top line
  //  on the region's edge, a press must otherwise land inside the edge yet not below that line.  The
  //  press keeps its real position, so it is on line -1 of the region - "this line and everything after"
  //  then takes the top line too, and the drag is measured in the same frame, so nothing jumps.
  if(!gestureFrame.region && (currMode == MODE_INSSPACERULED || currMode == MODE_SELECTRULED)) {
    RulingFrame nearFrame = currPage->gestureFrame(pos, REGION_PRESS_SLOP);
    if(nearFrame.region) {
      gestureFrame = nearFrame;
      prevLine = initialLine = gestureFrame.line(pos, Page::BLANK_Y_RULING);
      lx = gestureFrame.toLocal(pos).x;
      marginLeft = MIN_DIM;
    }
  }
  // what ruled insert space moves: the line it starts on and the local x it starts at (MIN_DIM: that whole
  //  line); the press's own line and x for Right and the combined tool
  insSpaceSelLine = initialLine;
  insSpaceSelX = lx;
  const bool skipLines = currMode == MODE_INSSPACERULED && scribbleDoc->scribbleMode->insSpaceSkipLines;
  if(currMode == MODE_INSSPACERULED && insSpaceAxis == MODE_INSSPACEDOWN) {
    // Insert Lines: near a rule line moves the block below it, mid-line splits the line at the pen, and with
    //  Skip Lines a press on a blank line moves the text line below it as a block (see insertLinesStart())
    const RulingFrame lineFrame = gestureFrame;
    const Dim yr = lineFrame.yrulingOr(Page::BLANK_Y_RULING);
    auto lineHasInk = [&](int line) {
      for(Element* s : currPage->children()) {
        if(s->isRulingRegion() || currPage->regionAt(s->com()) != lineFrame.region)
          continue;
        Point local = lineFrame.toLocal(s->com());
        if(int(std::floor(local.y/yr)) == line && local.x >= marginLeft)  // not a bookmark in the margin
          return true;
      }
      return false;
    };
    InsertLinesStart start = insertLinesStart(lineFrame.toLocal(pos).y, yr, skipLines, lineHasInk);
    if(skipLines)
      gestureFrame = skippedLineFrame(lineFrame, start.line);
    prevLine = initialLine = gestureFrame.line(pos, Page::BLANK_Y_RULING);
    insSpaceSelLine = skipLines ? 0 : start.line;
    if(start.wholeLine)
      insSpaceSelX = MIN_DIM;
  }
  // text written on every second line: insert space and reflow work in text lines (see skippedLineFrame)
  else if(skipLines) {
    gestureFrame = skippedLineFrame(gestureFrame, pos);
    prevLine = initialLine = insSpaceSelLine = gestureFrame.line(pos, Page::BLANK_Y_RULING);
  }
  if(currSelection) {
    // clear selection depending on mode
    switch(currMode) {
    case MODE_STROKE:
    case MODE_DRAWSHAPE:
      // ignore this stroke if it clears selection (optionally) - the shape tool too, or a shape left in
      //  edit mode after drawing (shapeEditAfterDraw) could only be left by drawing another one; unless
      //  shapeDrawThrough lets the shape tool draw straight through that press
      if(cfg->Bool("clearSelOnly") && !(currMode == MODE_DRAWSHAPE && cfg->Bool("shapeDrawThrough")))
        currMode = MODE_NONE;
    case MODE_SELECTRECT:
    case MODE_SELECTRULED:
    case MODE_SELECTLASSO:
    case MODE_SELECTPATH:
    case MODE_BOOKMARK:
    case MODE_ERASESTROKE:
    case MODE_ERASERULED:
    case MODE_ERASEFREE:
    case MODE_ERASEFREERULED:
    case MODE_INSSPACEVERT:
    case MODE_INSSPACEHORZ:
    case MODE_INSSPACERULED:
      clearSelection();
      break;
    case MODE_MOVESEL:
      currMode = currSelection->selector->drawHandles ? MODE_MOVESELFREE : MODE_MOVESELRULED;
      // ruled move steps in the ruling of the ink being moved, not of wherever it was grabbed
      if(currSelection->ruling.region)
        gridFrame = currSelection->ruling;
      else {
        gridFrame = currPage->pageFrame();
        gridFrame.origin = Point(0, 0);
      }
      break;
    default:
      break;
    }
  }
  else if(currMode != MODE_PAN && currMode != MODE_NONE && currMode != MODE_PAGESEL)
    scribbleDoc->clearSelection();
  Page* selsource = currPage;

  // process any recent strokes
  if(currMode != MODE_STROKE)
    groupStrokes();
  // a multi-point shape is abandoned by switching to any other tool
  if(shapeInProgress && currMode != MODE_DRAWSHAPE)
    finishShape();
  // second pass
  switch(currMode) {
  case MODE_PAN:
    panZoomStart(event);
    break;
  case MODE_STROKE:
  {
    ScribblePen resolved = resolvedPen(pos);  // the marker's relative width has to be in document units
    const ScribblePen* pen = &resolved;
    if(pen->hasFlag(ScribblePen::SNAP_TO_GRID))
      pos = gridFrame.snapToGrid(pos, Page::BLANK_Y_RULING);
    // the whole stroke runs along the middle of the ruled line the press lands in - in a tilted region,
    //  that line is tilted too, and so is the stroke
    if(pen->hasFlag(ScribblePen::CENTER_ON_LINE)) {
      Dim yr = gestureFrame.yrulingOr(Page::BLANK_Y_RULING);
      centerLineLocalY = gestureFrame.yForLine(prevLine, Page::BLANK_Y_RULING) + yr/2;
      pos = gestureFrame.toPage(Point(lx, centerLineLocalY));
    }
    //Dim w = cfg->Bool("scalePenWithZoom") ? pen->width/mZoom : pen->width;
    bool straightLine = pen->hasFlag(ScribblePen::LINE_DRAWING) || pen->hasFlag(ScribblePen::CENTER_ON_LINE);
    bool lineDrawing = straightLine || pen->hasFlag(ScribblePen::SNAP_TO_GRID);
    StrokeBuilder* builder = StrokeBuilder::create(*pen);
    // install filters
    if(!lineDrawing) {
      // reasonable values are inputSimplify = 2 (0.1 pixel) and inputSmoothing = 5
      Dim simp = cfg->Int("inputSimplify")*0.05/mZoom;
      if(simp > 0)
        builder->addFilter(new SimplifyFilter(simp, pen->usesPressure() ? simp/pen->width : 1.0));
      // low pass preceeds simplify
      int smooth = cfg->Int("inputSmoothing");
      if(smooth > 0)
        builder->addFilter(new LowPassIIR(smooth*0.5/mZoom));   //SymmetricFIR(smooth));
      // added last so it runs first: it has to see the raw samples, and simplify - which retracts points
      //  it has already emitted - cannot be upstream of a filter that turns one point into several
      // inputCurveFit is the *strength* (how many times the relaxation is applied); the chord flatness
      //  tolerance is not a user knob - it only trades points for accuracy and has no visible effect
      int fit = cfg->Int("inputCurveFit");
      if(fit > 0)
        builder->addFilter(new CurveFitFilter(CURVEFIT_TOL/mZoom, fit));
    }
    if(!cfg->Bool("dropFirstPenPoint") || event.source != INPUTSOURCE_PEN) {
      // add two points for line drawing, since second will be removed
      if(straightLine)
        builder->addInputPoint(StrokePoint(pos.x, pos.y, lineDrawPressure, 0, 0, event.t));
      builder->addInputPoint(StrokePoint(pos.x, pos.y,
          event.points[0].pressure, event.points[0].tiltX, event.points[0].tiltY, event.t));
    }
    scribbleDoc->strokeBuilder = builder;
    scribbleDoc->updateCurrStroke(builder->getDirty());  // necessary for single point stroke to show up
    startShapeSnap(pos);
    break;
  }
  case MODE_DRAWSHAPE:
  {
    if(scribbleDoc->scribbleMode->drawRegion) {
      // the region tool is a drag, like the box: the region is live (painted each frame) until release
      finishShape();
      Dim yr = currPage->yruling() > 0 ? currPage->yruling() : Page::BLANK_Y_RULING;
      RulingRegionParams params = RulingRegionParams::fromRect(Rect::corners(pos, pos), currPage->xruling(),
          yr, currPage->props.dotRadius);
      regionInProgress = Element::createRulingRegion(params, currPage->props.color, currPage->props.ruleColor);
      break;
    }
    int shapeid = scribbleDoc->scribbleMode->shapeId;
    const ShapeDef* def = shapeDef(shapeid);
    if(!def)
      break;
    if(shapeInProgress && shapeInProgress->shapeParams().id != shapeid)
      finishShape();  // changing shape while a polyline is open commits the polyline
    pos = snapShapePoint(pos);
    if(def->gesture == SHAPEGESTURE_MULTIPOINT) {
      if(shapeInProgress) {
        ShapeParams params = shapeInProgress->shapeParams();
        // generous hit radius, scaled with zoom so it works the same at any magnification
        Dim hitr = PATHSELECT_RADIUS/mZoom;
        if((pos - params.points.back()).dist() < hitr) {
          finishShape(true);  // tap the last placed point to finish
          break;
        }
        if(params.points.size() > 2 && (pos - params.points.front()).dist() < hitr) {
          params.flags |= SHAPEFLAG_CLOSED;  // tap the first point to close the shape and finish
          shapeInProgress->setShapeParams(params);
          finishShape(true);
          break;
        }
        scribbleDoc->updateCurrStroke(shapeInProgress->bbox());
        params.points.push_back(pos);
        params.points.back() = snapShapeAngleAt(params, int(params.points.size()) - 1, pos);
        shapeInProgress->setShapeParams(params);
        scribbleDoc->updateCurrStroke(shapeInProgress->bbox());
      }
      else {
        shapeInProgress = createShapeElement(newShapeParams(shapeid, pos));
        scribbleDoc->updateCurrStroke(shapeInProgress->bbox());
      }
      break;
    }
    currStroke = createShapeElement(newShapeParams(shapeid, pos));
    break;
  }
  case MODE_SHAPEHANDLE:
    // remember the descriptor so the whole drag collapses into a single undo item
    if(regionSelector) {
      regionHandleStart = regionSelector->region->regionParams();
      regionHandleStartPos = pos;
    }
    else if(shapeSelector && shapeSelector->shapeElement())
      shapeHandleStart = shapeSelector->shapeElement()->shapeParams();
    break;
  case MODE_BOOKMARK:
  {
    const Dim BKMK_W = 16;
    const Dim BKMK_H = 30;
    const Point BOOKMARK_POINTS[] = {{0,0}, {BKMK_W,0}, {BKMK_W,BKMK_H}, {BKMK_W/2,(5*BKMK_H)/6}, {0,BKMK_H}, {0,0}};
    Path2D bkmk;
    for(const Point& p : BOOKMARK_POINTS)
      bkmk.addPoint(p);

    currStroke = new Element(new SvgPath(bkmk));
    currStroke->node->addClass("bookmark");
    setSvgFillColor(currStroke->node, app->bookmarkColor);
    //currStroke->setCom(currStroke->bbox().center());
    // note no background for this selection
    currSelection = new Selection(currPage);
    currSelection->addStroke(currStroke);
    Rect bbox = currStroke->bbox();
    bookmarkSnapX = cfg->Bool("snapBookmarks") ? currPage->marginLeft() - 1.5*bbox.width() : 0;
    if(currPage->xruling() > 0)
      pos.x = currPage->xruling() * int(pos.x/currPage->xruling());
    else if(bookmarkSnapX > 0 && pos.x > bookmarkSnapX)
      pos.x -= std::min(pos.x - bookmarkSnapX, bbox.width());
    if(currPage->yruling() > 0)
      pos.y = currPage->yruling() * int(pos.y/currPage->yruling());
    // note the adjustment of dy to move to center vertically in ruleline
    // I'm not sure there is anyway to avoid bookmark not being centered in x if bookmark snap enabled
    currSelection->translate(pos.x, pos.y + (currPage->yruling() - bbox.height())/2);
    break;
  }
  case MODE_ERASESTROKE:
    tempSelection = new Selection(selsource, Selection::STROKEDRAW_NONE);
    tempSelection->selMode = Selection::SELMODE_UNION;
    pathSelector = new PathSelector(tempSelection);
    pathSelector->selectPath(pos, ERASESTROKE_RADIUS/mZoom);
    break;
  case MODE_ERASERULED:
    tempSelection = new Selection(selsource, Selection::STROKEDRAW_NONE);
    tempSelection->selMode = Selection::SELMODE_UNION;
    tempSelection->ruling = gestureFrame;
    ruledSelector = new RuledSelector(tempSelection, selColMode);
    if(cfg->Bool("greedyRuledErase"))
      ruledSelector->selMode = RuledSelector::SEL_OVERLAP;
    eraseCurrLine = prevLine;
    eraseXmax = lx;
    eraseXmin = lx;
    break;
  case MODE_ERASEFREE:
    // use tempSelection to track strokes touched by free eraser
    tempSelection = new Selection(selsource, Selection::STROKEDRAW_NONE);
    tempSelection->selMode = Selection::SELMODE_UNION;
    freeErasePieces = new Selection(selsource, Selection::STROKEDRAW_NORMAL);
    freeErase(pos, pos);
    break;
  case MODE_ERASEFREERULED:
    // use tempSelection to track strokes touched by free eraser
    tempSelection = new Selection(selsource, Selection::STROKEDRAW_NONE);
    tempSelection->selMode = Selection::SELMODE_UNION;
    freeErasePieces = new Selection(selsource, Selection::STROKEDRAW_NORMAL);
    eraseCurrLine = prevLine;
    eraseXmax = lx;
    eraseXmin = lx;
    freeEraseRuled(eraseXmin, eraseXmax, eraseCurrLine);
    break;
  case MODE_SELECTRECT:
    currSelection = new Selection(currPage);  // cfg->Bool("liveSelect") ? Selection::SELMODE_NONE
    rectSelector = new RectSelector(currSelection, mZoom, false);  // no handles while selecting
    if(scribbleDoc->scribbleMode->selectTouching)
      rectSelector->rectSelMode = RectSelector::RECTSEL_ANY;
    break;
  case MODE_SELECTRULED:
    currSelection = new Selection(currPage);
    currSelection->ruling = gestureFrame;
    ruledSelector = new RuledSelector(currSelection, selColMode);
    if(scribbleDoc->scribbleMode->selectTouching)
      ruledSelector->selMode = RuledSelector::SEL_OVERLAP;
    break;
  case MODE_SELECTLASSO:
    currSelection = new Selection(currPage);
    lassoSelector = new LassoSelector(currSelection, 0.5/mZoom);
    lassoSelector->touching = scribbleDoc->scribbleMode->selectTouching;
    lassoSelector->addPoint(pos.x, pos.y);
    break;
  case MODE_SELECTPATH:
    currSelection = new Selection(currPage);
    currSelection->selMode = Selection::SELMODE_UNION;
    pathSelector = new PathSelector(currSelection);
    pathSelector->selectPath(pos, PATHSELECT_RADIUS/mZoom);
    break;
  case MODE_CROPSEL:
  {
    Element* s = currSelection->strokes.front();
    const Transform2D& tf = s->node->getTransform();
    if(tf.isRotating() || tf.xscale() < 0 || tf.yscale() < 0) {
      Element* t = s->cloneNode();
      SvgImage* svgimg = static_cast<SvgImage*>(t->node);
      if(svgimg->srcRect.isValid() && svgimg->srcRect != Rect::wh(svgimg->m_image.width, svgimg->m_image.height))
        svgimg->m_image = svgimg->m_image.cropped(svgimg->srcRect);
      svgimg->srcRect = Rect();
      // actually, we should extract just the rotation from tf, and apply scale to viewport rect
      svgimg->m_image = svgimg->m_image.transformed(tf);
      svgimg->m_bounds = tf.mapRect(svgimg->m_bounds);
      t->node->setTransform(Transform2D());
      // show copy, hide original
      currPage->contentNode->addChild(t->node, s->node);
      currSelection->removeStroke(s);
      currSelection->addStroke(t);
      tempSelection = new Selection(currSelection->page, Selection::STROKEDRAW_NONE);
      tempSelection->addStroke(s);
    }
    break;
  }
  case MODE_SCALESEL:
  case MODE_SCALESELW:
  case MODE_ROTATESEL:
  case MODE_ROTATESELW:
  case MODE_MOVESELFREE:
  case MODE_MOVESELRULED:
    // protect against crash if UI allows move sel mode when it shouldn't
    if(!currSelection)
      currMode = MODE_NONE;
    // a region carries whatever ink is inside it *now*
    else if(regionSelector)
      refreshRegionSelection();
    // nothing to do until mouse actually moves
    break;
  case MODE_INSSPACEVERT:
    tempSelection = new Selection(selsource);
    rectSelector = new RectSelector(tempSelection);
    rectSelector->selectRect(MIN_DIM, pos.y, MAX_DIM, MAX_DIM);
    addRegionsToInsertSpace(tempSelection, [&](const Rect& r) { return r.top >= pos.y; });
    break;
  case MODE_INSSPACEHORZ:
    tempSelection = new Selection(selsource);
    rectSelector = new RectSelector(tempSelection);
    rectSelector->selectRect(pos.x, MIN_DIM, MAX_DIM, MAX_DIM);
    addRegionsToInsertSpace(tempSelection, [&](const Rect& r) { return r.left >= pos.x; });
    break;
  case MODE_INSSPACERULED:
    tempSelection = new Selection(selsource);
    tempSelection->ruling = gestureFrame;
  {
    // a whole line moves whatever columns it has (as a press in the margin does); a region has no margin
    //  to make findStops() stand down, so the selector is told directly
    const RuledSelector::ColMode colMode = insSpaceSelX == MIN_DIM ? RuledSelector::COL_NONE : selColMode;
    tempSelection = new Selection(selsource);
    tempSelection->ruling = gestureFrame;
    ruledSelector = new RuledSelector(tempSelection, colMode);
    ruledSelector->selectRuledAfter(insSpaceSelX, insSpaceSelLine);
    // if cursor down past left margin, we sort strokes, but we'll only
    //  enable inserting horz space if there are strokes on the first line
    int firstLine = tempSelection->sortRuled();
    if(insSpaceSelX > marginLeft && firstLine == insSpaceSelLine)
      insertSpaceX = true;
    else
      insertSpaceX = false;
    // Insert Lines dragged back up erases the ink the moved text lands on, from where that text starts
    //  rather than from the pen: pressed anywhere left of a line's rest that an earlier Insert Lines split
    //  off, the drag up rejoins it without eating the start of the line it rejoins (which ends left of the
    //  split, so short of the rest)
    insSpaceEraseX = insSpaceSelX;
    if(insSpaceAxis == MODE_INSSPACEDOWN && insSpaceSelX > marginLeft) {
      insSpaceEraseX = MAX_DIM;
      if(firstLine == insSpaceSelLine)
        insSpaceEraseX = std::max(insSpaceSelX, gestureFrame.localBBox(tempSelection->strokes.front()->bbox()).left);
    }
    // erase strokes convered by negative ruled insert space
    if(cfg->Bool("insSpaceErase")) {
      // second ruled selector for erasing strokes covered by negative insert space
      insSpaceEraseSelection = new Selection(selsource, Selection::STROKEDRAW_NONE);
      insSpaceEraseSelection->ruling = gestureFrame;
      insSpaceEraseSelector = new RuledSelector(insSpaceEraseSelection, colMode);
    }
  }
  default:
    break;
  }
  prevPos = pos;
  initialPos = pos;
  prevRawPos = rawpos;

  if(currMode != MODE_STROKE && widget)
    widget->startTimer(timerPeriod);  // for autoscroll
}

void ScribbleArea::doMoveEvent(const InputEvent& event)
{
  if(placingPressed || todoPressed)
    return;  // doMotionEvent moves the tags; a press on a to-do box waits for its release
  Point rawpos = Point(event.points[0].x, event.points[0].y);
  Point pos = dimToPageDim(screenToDim(rawpos));
  // tablet will happily send many points with same position
  if(pos.x == prevPos.x && pos.y == prevPos.y)
    return;

  Dim dx = pos.x - prevPos.x;
  Dim dy = pos.y - prevPos.y;
  // ruled modes work in the ruling the gesture started in (see gestureFrame)
  int line = gestureFrame.line(pos, Page::BLANK_Y_RULING);
  Dim lx = gestureFrame.toLocal(pos).x;
  Dim ldx = lx - gestureFrame.toLocal(prevPos).x;
  // ruled insert space held to one axis: Right never leaves the pressed line, Down never moves along it
  if(currMode == MODE_INSSPACERULED && insSpaceAxis == MODE_INSSPACERIGHT)
    line = initialLine;
  else if(currMode == MODE_INSSPACERULED && insSpaceAxis == MODE_INSSPACEDOWN) {
    lx = gestureFrame.toLocal(initialPos).x;
    ldx = 0;
  }
  const Dim marginLeft = gestureFrame.region ? MIN_DIM : currPage->marginLeft();
  switch(currMode) {
  case MODE_PAN:
    panZoomMove(event, event.points.size(), event.points.size());
    break;
  case MODE_STROKE:
    if(snapActive) {
      scaleSnapShape(pos);
      break;
    }
    if(currPen()->hasFlag(ScribblePen::SNAP_TO_GRID)) {
      Point snappos = gridFrame.snapToGrid(pos, Page::BLANK_Y_RULING);
      // second check provides some hysteresis
      if(approxEq(snappos, prevPos, 1E-9) || 3*pos.dist(snappos) > pos.dist(prevPos))
        return;
      pos = snappos;
    }
    if(currPen()->hasFlag(ScribblePen::CENTER_ON_LINE))
      pos = gestureFrame.toPage(Point(lx, centerLineLocalY));
    // A stroke begun in a region stays in it, as against a ruler.  Ink belongs to the ruling its centre
    //  is on and the ruled tools only reach ink inside that ruling's outline, so a stroke running past
    //  the edge could be reached by neither the region's ruled tools nor the page's.
    if(gestureFrame.region)
      pos = static_cast<const Element*>(gestureFrame.region)->regionParams().clampInside(pos);
    if(currPen()->hasFlag(ScribblePen::LINE_DRAWING) || currPen()->hasFlag(ScribblePen::CENTER_ON_LINE)) {
      // some stuff to facilitate debugging of stroke builder
      if(ScribbleInput::pressedKey == SDLK_LEFTBRACKET)
        lineDrawPressure = std::max(Dim(0), 0.8*lineDrawPressure);
      else if(ScribbleInput::pressedKey == SDLK_RIGHTBRACKET)
        lineDrawPressure = std::max(Dim(0), 1.25*lineDrawPressure);
      // press '\' to insert a vertex in line
      if(ScribbleInput::pressedKey == SDLK_BACKSLASH)
        ScribbleInput::pressedKey = 0;
      else
        scribbleDoc->strokeBuilder->removePoints(1);
      if(scribbleDoc->strokeBuilder->getPath()->empty())
        scribbleDoc->strokeBuilder->addInputPoint(StrokePoint(pos.x, pos.y, lineDrawPressure, 0, 0, event.t));
      scribbleDoc->strokeBuilder->addInputPoint(StrokePoint(pos.x, pos.y, lineDrawPressure, 0, 0, event.t));
    }
    else
      scribbleDoc->strokeBuilder->addInputPoint(StrokePoint(pos.x, pos.y,
          event.points[0].pressure, event.points[0].tiltX, event.points[0].tiltY, event.t));
    // current stroke is drawn directly to screen; note that in reqRepaint(),
    //  currPage->getDirty() will return invalid rect since currStroke is not added
    //  to page until cursor release.
    scribbleDoc->updateCurrStroke(scribbleDoc->strokeBuilder->getDirty());
    trackShapeSnap(pos);
    break;
  case MODE_ERASESTROKE:
    pathSelector->selectPath(pos, ERASESTROKE_RADIUS/mZoom);
    break;
  case MODE_ERASERULED:
    if(lx < marginLeft) {
      // behave like select ruled if cursor in left margin - allows for very fast erasing
      ruledSelector->selectRuled(eraseXmin, eraseCurrLine, lx, line);
      break;
    }
    else if(eraseCurrLine == line) {
      eraseXmax = std::max(eraseXmax, lx);
      eraseXmin = std::min(eraseXmin, lx);
    }
    else {
      eraseCurrLine = line;
      eraseXmax = lx;
      eraseXmin = lx;
    }
    ruledSelector->selectRuled(eraseXmin, line, eraseXmax, line);
    break;
  case MODE_ERASEFREE:
    freeErase(prevPos, pos);
    break;
  case MODE_ERASEFREERULED:
    if(eraseCurrLine == line) {
      eraseXmax = std::max(eraseXmax, lx);
      eraseXmin = std::min(eraseXmin, lx);
    }
    else {
      eraseCurrLine = line;
      eraseXmax = lx;
      eraseXmin = lx;
    }
    freeEraseRuled(eraseXmin, eraseXmax, eraseCurrLine);
    break;
  case MODE_SELECTRECT:
    // selection can either grow or shrink - dirty rect of union of before and after!
    rectSelector->selectRect(initialPos.x, initialPos.y, pos.x, pos.y);
    break;
  case MODE_SELECTRULED:
    // force redraw of selection BG
    ruledSelector->selectRuled(gestureFrame.toLocal(initialPos).x, initialLine, lx, line);
    break;
  case MODE_SELECTLASSO:
    // limit number of points in lasso and number of calls to doSelect
    // we can't use rawpos here because that doesn't work with autoscroll
    if(pos.dist(prevPos) < MIN_LASSO_POINT_DIST/mScale)
      return;
    lassoSelector->addPoint(pos.x, pos.y);
    break;
  case MODE_SELECTPATH:
    pathSelector->selectPath(pos, PATHSELECT_RADIUS/mZoom);
    break;
  case MODE_SCALESEL:
  case MODE_SCALESELW:
  {
    Dim xscale = (pos.x - scaleOrigin.x)/(initialPos.x - scaleOrigin.x);
    Dim yscale = (pos.y - scaleOrigin.y)/(initialPos.y - scaleOrigin.y);
    if(regionSelector) {
      // one positive factor, from the drag projected on the handle's diagonal: a region's ruling can
      //  neither stretch one way nor be mirrored
      Point d0 = initialPos - scaleOrigin;
      xscale = yscale = std::max(Dim(0.01), dot(pos - scaleOrigin, d0)/std::max(dot(d0, d0), Dim(1E-9)));
    }
    else if(scaleLockRatio) {
      Dim scale = std::max(Dim(0.01), std::min(std::abs(xscale), std::abs(yscale)));
      xscale = SGN(xscale)*scale;
      yscale = SGN(yscale)*scale;
    }
    // prevent divide by zero (and excessive shrinkage in a single action)
    if(ABS(xscale) < 0.01) xscale = SGN(xscale)*0.01;
    if(ABS(yscale) < 0.01) yscale = SGN(yscale)*0.01;
    // last arg: scale stroke widths too?
    currSelection->scale(xscale/prevXScale, yscale/prevYScale, scaleOrigin, currMode == MODE_SCALESELW);
    prevXScale = xscale;
    prevYScale = yscale;
    break;
  }
  case MODE_ROTATESEL:
    currSelection->rotate(calcAngle(prevPos, scaleOrigin, pos), scaleOrigin);
    break;
  case MODE_ROTATESELW:
  {
    // alternate (pen button) mode: rotate in 15 deg increments
    Dim prevangle = (M_PI/12)*int(calcAngle(initialPos, scaleOrigin, prevPos)/(M_PI/12));
    Dim angle = (M_PI/12)*int(calcAngle(initialPos, scaleOrigin, pos)/(M_PI/12));
    if(angle != prevangle)
      currSelection->rotate(angle - prevangle, scaleOrigin);
    break;
  }
  case MODE_CROPSEL:
  {
    SvgImage* svgimg = static_cast<SvgImage*>(currSelection->strokes.front()->node);
    if(!svgimg->srcRect.isValid())
      svgimg->srcRect = Rect::wh(svgimg->image()->width, svgimg->image()->height);
    if(std::isnan(scaleOrigin.x)) {
      Dim yscale = (pos.y - scaleOrigin.y)/(initialPos.y - scaleOrigin.y);
      if(yscale <= 0) break;
      Dim maxheight = scaleOrigin.y < svgimg->m_bounds.center().y ?
            svgimg->image()->height - svgimg->srcRect.top : svgimg->srcRect.bottom;
      Dim sy = std::min(yscale/prevYScale, maxheight/svgimg->srcRect.height());
      // internalScale == 0 indicates crop
      currSelection->applyTransform(ScribbleTransform(Transform2D(1, 0, 0, sy, 0, (1 - sy)*scaleOrigin.y), 0, 0));
      prevYScale *= sy;
    }
    else {
      Dim xscale = (pos.x - scaleOrigin.x)/(initialPos.x - scaleOrigin.x);
      if(xscale <= 0) break;
      Dim maxwidth = scaleOrigin.x < svgimg->m_bounds.center().x ?
            svgimg->image()->width - svgimg->srcRect.left : svgimg->srcRect.right;
      Dim sx = std::min(xscale/prevXScale, maxwidth/svgimg->srcRect.width());
      currSelection->applyTransform(ScribbleTransform(Transform2D(sx, 0, 0, 1, (1 - sx)*scaleOrigin.x, 0), 0, 0));
      prevXScale *= sx;
    }
    currSelection->shrink();
    break;
  }
  case MODE_DRAWSHAPE:
  {
    if(regionInProgress) {
      scribbleDoc->updateCurrStroke(regionInProgress->bbox());
      RulingRegionParams params = regionInProgress->regionParams();
      Rect r = Rect::corners(initialPos, pos);
      params.corners = { Point(r.left, r.top), Point(r.right, r.top), Point(r.right, r.bottom), Point(r.left, r.bottom) };
      // lines are phased from the top edge, so the first line sits one pitch below it
      params.origin = Point(r.left, r.top);
      regionInProgress->setRegionParams(params);
      scribbleDoc->updateCurrStroke(regionInProgress->bbox());
      break;
    }
    Element* shape = currStroke ? currStroke : shapeInProgress;
    if(!shape || shape->shapeParams().points.size() < 2)
      break;
    scribbleDoc->updateCurrStroke(shape->bbox());
    ShapeParams params = shape->shapeParams();
    params.points.back() = snapShapePoint(pos);
    if(event.modemod & MODEMOD_PENBTN)
      applyShapeConstraint(params);
    else
      params.points.back() = snapShapeAngleAt(params, int(params.points.size()) - 1, params.points.back());
    shape->setShapeParams(params);
    scribbleDoc->updateCurrStroke(shape->bbox());
    break;
  }
  case MODE_SHAPEHANDLE:
  {
    if(regionSelector) {
      // a corner reshapes the outline only, the size handle stretches it, and the origin handle slides
      //  the lines.  None of them moves ink.
      Element* region = regionSelector->region;
      RulingRegionParams params = regionHandleStart;
      if(shapeHandleIdx >= 0 && shapeHandleIdx < int(params.corners.size()))
        params.corners[shapeHandleIdx] = pos;
      else if(shapeHandleIdx == int(params.corners.size()))
        params.origin = params.origin + (pos - regionHandleStartPos);
      else if(shapeHandleIdx == int(params.corners.size()) + 1)
        params = regionSelector->resized(regionHandleStart, regionHandleStartPos, pos);
      else
        break;
      dirtyScreen(currSelection->getBGBBox());
      region->setRegionParams(params);
      currSelection->invalidateBBox();
      currSelection->xchgBGDirty(true);
      scribbleDoc->updateCurrStroke(currSelection->getBGBBox());
      break;
    }
    Element* shape = shapeSelector ? shapeSelector->shapeElement() : NULL;
    if(!shape || shapeHandleIdx < 0 || shapeHandleIdx >= int(shapeSelector->handles().size()))
      break;
    dirtyScreen(currSelection->getBGBBox());
    ShapeParams params = shape->shapeParams();
    const ShapeHandle& handle = shapeSelector->handles()[shapeHandleIdx];
    Point local = shapeSelector->toLocal(snapShapePoint(pos));
    if(handle.type == ShapeHandle::POINT) {
      // the points are node-local, so a scaled shape needs the screen distance in its own units
      Dim localPerPage = shapeSelector->toLocal(pos + Point(1, 0)).dist(shapeSelector->toLocal(pos));
      local = snapShapeAngleAt(params, handle.index, local, localPerPage > 0 ? localPerPage : 1);
    }
    dragShapeHandle(params, handle, local);
    shape->setShapeParams(params);
    currSelection->invalidateBBox();
    shapeSelector->updateHandles();
    currSelection->xchgBGDirty(true);
    scribbleDoc->updateCurrStroke(currSelection->getBGBBox());
    break;
  }
  case MODE_BOOKMARK:
    if(currPage->xruling() == 0 && bookmarkSnapX > 0 && pos.x > bookmarkSnapX) {
      pos.x -= std::min(pos.x - bookmarkSnapX, currStroke->bbox().width());
      dx = pos.x - prevPos.x;
    }
    scribbleDoc->updateCurrStroke(currStroke->bbox());
    if(currPage->xruling() > 0)
      dx = currPage->xruling() * (int(pos.x/currPage->xruling()) - int(prevPos.x/currPage->xruling()));
    if(currPage->yruling() > 0)
      dy = currPage->yruling() * (int(pos.y/currPage->yruling()) - int(prevPos.y/currPage->yruling()));
    currSelection->translate(dx, dy);
    scribbleDoc->updateCurrStroke(currStroke->bbox());
    break;  // note that we don't draw new bookmark on overlay (crashes anyway due to lack of selector)
  case MODE_MOVESELRULED:
  case MODE_MOVESELFREE:
    // we had a comment saying setOffset didn't work as well as translate, but we've arrogantly ignored it to
    //  fix a minor issue with dragging selections between pages
    dx = pos.x - initialPos.x;
    dy = pos.y - initialPos.y;
    // if pos is outside page, draw selection on overlay - we don't use overlay to draw selection inside
    //  page to preserve z-order (relative to unselected strokes)
    if(!currPage->rect().contains(pos) || !screenRect.contains(rawpos)) {
      currSelection->setOffset(dx, dy);
      // if we encounter further issues here, consider using a copy of selection (cloning strokes) for overlay
      currSelection->setDrawType(Selection::STROKEDRAW_NONE);
      app->overlayWidget->drawSelection(currSelection, screenToGlobal(rawpos), -pos, mScale);
      break;
    }
    if(currSelection->drawType() == Selection::STROKEDRAW_NONE) {
      // a region's ink is not shown as selected - it is being carried, not picked
      currSelection->setDrawType(regionSelector ? Selection::STROKEDRAW_NORMAL : Selection::STROKEDRAW_SEL);
      app->overlayWidget->drawSelection(NULL);
    }
    if(currMode == MODE_MOVESELRULED) {
      // quantized in the ruling the drag started in; for the page's own that is exactly the old
      //  int(pos/ruling) arithmetic, and in a tilted region the steps run along its lines
      const RulingFrame& f = gridFrame;
      Point l = f.toLocal(pos), l0 = f.toLocal(initialPos);
      Point step(f.xRuling > 0 ? f.xRuling * (int(l.x/f.xRuling) - int(l0.x/f.xRuling)) : l.x - l0.x,
          f.yRuling > 0 ? f.yRuling * (int(l.y/f.yRuling) - int(l0.y/f.yRuling)) : l.y - l0.y);
      Point d = f.toPageDir(step);
      dx = d.x;
      dy = d.y;
    }
    currSelection->setOffset(dx, dy);
    break;
  case MODE_INSSPACEVERT:
  {
    tempSelection->translate(0, dy);
    Dim ystep = currPage->yruling() > 0 ? currPage->yruling() : GROW_STEP;
    Dim h = tempSelection->count() > 0 ?
        std::max(quantize(tempSelection->getBBox().bottom + 2*ystep, ystep), initialPageSize.height()) :
        quantize(initialPageSize.height() + (pos.y - initialPos.y), ystep/2);
    setPageDims(-1, h, true);
    break;
  }
  case MODE_INSSPACEHORZ:
  {
    tempSelection->translate(dx, 0);
    Dim xstep = currPage->xruling() > 0 ? currPage->xruling() : GROW_STEP;
    Dim w = tempSelection->count() > 0 ?
        std::max(quantize(tempSelection->getBBox().right + 2*xstep, xstep), initialPageSize.width()) :
        quantize(initialPageSize.width() + (pos.x - initialPos.x), xstep/2);
    setPageDims(w, -1, true);
    break;
  }
  case MODE_INSSPACERULED:
  {
    Dim dy0 = tempSelection->count() > 0 ? tempSelection->strokes.back()->pendingTransform().yoffset() : 0;
    if(insertSpaceX && cfg->Bool("reflow"))
      tempSelection->reflowStrokes(lx - gestureFrame.toLocal(initialPos).x, line - initialLine, reflowWordSep);
    else
      tempSelection->insertSpace(insertSpaceX ? ldx : 0, line - prevLine);
    // resizing page - a region's lines say nothing about the page's height, so only the page's own
    //  ruling grows the page by lines
    if(gestureFrame.region) {}
    else if(tempSelection->count() > 0) {
      // this could be greater due to reflow
      Dim maxdy = tempSelection->strokes.back()->pendingTransform().yoffset();
      if(dy0 != maxdy) {
        setPageDims(initialPageSize.width(), initialPageSize.height(), true);
        growPage(tempSelection->getBBox(), 0, std::max(Dim(0), maxdy), true);
      }
    }
    else
      setPageDims(-1, initialPageSize.height() + gestureFrame.yrulingOr(Page::BLANK_Y_RULING) * (line - initialLine), true);
    // erase for negative insert space
    if(insSpaceEraseSelection && insSpaceAxis == MODE_INSSPACEDOWN) {
      // Insert Lines moves what it selected (from insSpaceSelX on insSpaceSelLine) by whole lines; the ink
      //  it lands on runs from insSpaceEraseX on its new first line to where it started
      int target = insSpaceSelLine + (line - initialLine);
      if(target < insSpaceSelLine) {
        insSpaceEraseSelector->findStops(insSpaceSelX, target);  // columns found from the pen, as always
        insSpaceEraseSelector->selectRuled(insSpaceEraseX, target, insSpaceSelX, insSpaceSelLine);
      }
      else
        insSpaceEraseSelection->clear();
    }
    else if(insSpaceEraseSelection) {
      Dim lx0 = gestureFrame.toLocal(initialPos).x;
      if(line < initialLine || (line == initialLine && lx < lx0))
        insSpaceEraseSelector->selectRuled(lx, line, lx0, initialLine);
      else
        insSpaceEraseSelection->clear();
    }
  }
  default:
    break;
  }
  prevPos = pos;
  prevRawPos = rawpos;
  // note that we don't actually need prevLine since it is just getLine(prevPos.y)
  prevLine = line;
}

void ScribbleArea::doReleaseEvent(const InputEvent& event)
{
  if(placingPressed) {
    placePendingTags(event.source == INPUTSOURCE_PEN);
    uiChanged(UIState::ReleaseEvent);  // undo and save buttons, as at the end of every other release
    return;
  }
  if(todoPressed) {
    // only if the release is still on the box it pressed - a hit test, so a tag deleted meanwhile (sync)
    //  is never touched through a stale pointer
    Point pos = dimToPageDim(screenToDim(Point(event.points[0].x, event.points[0].y)));
    Element* tag = todoBoxHit(pos, event.source == INPUTSOURCE_TOUCH);
    if(tag == todoPressed)
      toggleTodoTag(tag);
    todoPressed = NULL;
    uiChanged(UIState::ReleaseEvent);
    return;
  }
  // MODE_PAGESEL is unique in that it involves also clicking on pages
  if(event.modemod & MODEMOD_DBLCLICK && dimToPageNum(screenToDim(prevRawPos)) == numPages()
      && currMode != MODE_PAGESEL && currMode != MODE_DRAWSHAPE) {
    doCancelAction();
    Point oldpos = screenToDim(Point(0, 0));  // save before newPage() jumps position
    scribbleDoc->newPage();
    Point pagetop = pageDimToDim(Point(0,0));  // top-left of new page
    // net effect to scroll up so new page takes 2/3 of screen
    if(viewMode == VIEWMODE_VERT)
      setCornerPos(Point(oldpos.x, pagetop.y - getViewHeight()/mScale/3));
    return;
  }

  // restore original page size (MODE_INSERTSPACE)
  setPageDims(initialPageSize.width(), initialPageSize.height(), true);
  // no changes are made to persistent data structures until release event
  auto currModeType = ScribbleMode::getModeType(currMode);
  // move sel has several cases that require special handling for undo, e.g. drag to different page
  bool undoable = ScribbleMode::isUndoable(currMode) && currModeType != MODE_MOVESEL;
  if(undoable)
    scribbleDoc->startAction(currPageNum);

  // what the selection gesture covered, kept for Screenshot once the selectors are gone
  Path2D gestureRegion;
  switch(currMode) {
  case MODE_PAN:
    panZoomFinish(event);
    break;
  case MODE_PAGESEL:
    if(event.modemod & MODEMOD_CLICK) {
      int pagenum = dimToPageNum(screenToDim(prevRawPos));
      Page* p = page(pagenum);
      if(p && p->rect().contains(prevPos)) {
        // select page range; MODE_ERASE used to detect Shift key pressed
        bool isRange = (event.modemod & MODEMOD_PENBTN) || (event.modemod >> 24) == MODE_ERASE;
        scribbleDoc->selectPages(isRange ? -pagenum-1 : pagenum);  //, prevRawPos + Point(4,4));
      }
      if(scribbleDoc->numSelPages > 0 && cfg->Bool("popupToolbar"))
        app->showSelToolbar(screenToGlobal(prevRawPos + Point(4,4)));  // shift slightly from tap point
    }
    break;
  case MODE_DRAWSHAPE:
    if(regionInProgress) {
      Element* region = regionInProgress;
      regionInProgress = NULL;
      scribbleDoc->updateCurrStroke(region->bbox());
      Rect r = region->regionParams().bounds();
      // a tap, or a sliver no line fits in, is not a region
      Dim yr = region->regionParams().yRuling;
      if(!r.isValid() || r.width() < 2*RegionSelector::HANDLE_SIZE/mZoom || r.height() < std::max(yr, Dim(8)/mZoom)
          || !currPage->rect().intersects(r)) {
        region->deleteNode();
        break;
      }
      currPage->addStroke(region);
      // a region is edited right after it is drawn: its lines usually need lining up with the page's
      selectRegion(region);
      // the region tool is single use - a second region is rare, and the next press is almost always
      //  writing in the one just made
      scribbleDoc->scribbleMode->drawRegion = false;
      break;
    }
    // the multi-point gesture deliberately survives the release; only drag gestures commit here
    if(currStroke) {
      scribbleDoc->updateCurrStroke(currStroke->bbox());
      Rect bbox = currStroke->bbox();
      // discard a shape that is degenerate or entirely off the page
      if(!bbox.isValid() || !currPage->rect().intersects(bbox)
          || (bbox.width() < 1 && bbox.height() < 1)) {
        currStroke->deleteNode();
        currStroke = NULL;
        break;
      }
      if(!currPage->rect().contains(bbox))
        dirtyScreen(bbox);
      if(cfg->Bool("growWithPen"))
        growPage(bbox);
      // Page::addStroke() creates the StrokeAddedItem itself (spec 7.11)
      currPage->addStroke(currStroke);
      // shapes are deliberately kept out of recentStrokes: groupStrokes() would rewrite their
      //  centre of mass as if they were handwriting (spec 7.6)
      groupStrokes();
      Element* drawn = currStroke;
      currStroke = NULL;
      editShapeAfterDraw(drawn);
    }
    break;
  case MODE_SHAPEHANDLE:
  {
    if(regionSelector) {
      if(regionHandleStart.isValid())
        scribbleDoc->history->addItem(new RegionChangedItem(regionSelector->region, currPage, regionHandleStart));
      regionHandleStart = RulingRegionParams();
      shapeHandleIdx = -1;
      break;
    }
    Element* shape = shapeSelector ? shapeSelector->shapeElement() : NULL;
    // one undo item for the whole drag: we mutated live, and record the starting descriptor here
    if(shape && shapeHandleStart.isValid())
      scribbleDoc->history->addItem(new ShapeChangedItem(shape, currPage, shapeHandleStart));
    shapeHandleStart = ShapeParams();
    shapeHandleIdx = -1;
    break;
  }
  case MODE_BOOKMARK:
    // previously, we fell through to MODE_STROKE, but now bookmarks are handled separately
    scribbleDoc->updateCurrStroke(currStroke->bbox());
    if(currStroke->bbox().overlaps(currPage->rect())) {
      // set id for bookmark (otherwise, bookmark page will be dirtied when link to bookmark is created)
      currStroke->setNodeId(("b-" + randomStr(10)).c_str());
      // just commit translation directly instead of using Selection::commitTransform() so that we don't
      //  unnecessarily create undo item
      currStroke->commitTransform();
      currPage->addStroke(currStroke);
      delete currSelection;
    }
    else {
      // bookmark dropped off page
      currSelection->strokes.clear();
      delete currSelection;
      currStroke->deleteNode();
    }
    app->overlayWidget->drawSelection(NULL);
    currStroke = NULL;
    currSelection = NULL;
    // we're going to set bookmark id only when it's actually linked to
    break;
  case MODE_STROKE:
  {
    // a scratch-out needs no hold: it erases as the pen lifts
    if(!snapActive && snapSamples.size() > 2 && cfg->Bool("liftScratchOut") && scratchOutOnLift()) {
      stopShapeSnap();
      break;
    }
    stopShapeSnap();
    if(snapActive) {
      commitSnapShape();
      break;
    }
    strokeCounter++;
    currStroke = scribbleDoc->strokeBuilder->finish();
    // The centre of mass is what says which line a stroke is on, and its heuristic leans on "up" (first
    //  point, bbox top).  On a tilted region's lines up is the region's up, so measure it there - in page
    //  axes a stroke along a tilted line comes out half a line high.
    if(gestureFrame.isRotated() && currStroke->isPathElement())
      currStroke->setCom(StrokeBuilder::calcCom(currStroke->node, static_cast<SvgPath*>(currStroke->node)->path(),
          gestureFrame.localTransform(), gestureFrame.pageTransform()));
    scribbleDoc->updateCurrStroke(scribbleDoc->strokeBuilder->getDirty());
    delete scribbleDoc->strokeBuilder;  // we now own the stroke
    scribbleDoc->strokeBuilder = NULL;
    // discard stroke if entirely off page
    if(!currPage->rect().intersects(currStroke->bbox()) || currPen()->hasFlag(ScribblePen::EPHEMERAL)) {
      // no more growing page w/ off-page strokes - interferes w/ ghost page
      scribbleDoc->updateCurrStroke(currStroke->bbox());  //dirtyScreen(currStroke->bbox());
      currStroke->deleteNode();
      currStroke = NULL;
      break;
    }
    // if stroke is partially off-page, we need to dirty its bbox since strokes on page are clipped to page
    if(!currPage->rect().contains(currStroke->bbox()))
      dirtyScreen(currStroke->bbox());
    if(cfg->Bool("growWithPen"))
      growPage(currStroke->bbox());  // grow page if needed

    // if page is clean, clear page dirty after adding normal stroke so we don't redraw it unnecessarily
    // addChild clears m_renderedBounds, so restore since these are needed if stroke is immediately changed
    Rect r = currStroke->node->m_renderedBounds;
    // DRAW_UNDER means under the other ink on this layer, not under the whole page: going under a
    //  lower layer would put the stroke outside its own layer's run and break the stacking
    Element* under = currPage->layerFirstElement(scribbleDoc->document->layers.currentId);
    if(currPen()->hasFlag(ScribblePen::DRAW_UNDER) && under)
      currPage->addStroke(currStroke, under);
    else if(currPage->getDirty().isValid())  // we expect page to usually be clean, so this call is cheap
      currPage->addStroke(currStroke);
    else {
      currPage->addStroke(currStroke);
      currPage->clearDirty();
    }
    currStroke->node->m_renderedBounds = r;
    // if this is the first stroke created since last call to groupStrokes(), record it
    groupStrokes(currStroke);
    currStroke = NULL;
    break;
  }
  case MODE_ERASEFREE:
  case MODE_ERASEFREERULED:
  {
    auto strokes = currPage->children();
    for(auto ii = strokes.begin(); ii != strokes.end();) {
      Element* s = *ii++;
      if(s->isSelected(freeErasePieces)) {
        Element* nexts = ii != strokes.end() ? *ii : NULL;
        s->setSelected(NULL);
        if(s->isPathElement() || s->isMultiStroke()) {
          currPage->contentNode->removeChild(s->node);
          // the pieces of an erased stroke belong to the layer that stroke was on, which need not be
          //  the current one - without the explicit layer, free-erasing on a two-layer page would
          //  quietly migrate what it cut into whichever layer the pen happens to be on
          for(Element* ss : s->getEraseSubPaths())
            currPage->addStroke(ss, nexts, s->layer());
          freeErasePieces->removeStroke(s);
          s->deleteNode();
          ii = std::find(strokes.begin(), strokes.end(), nexts);
          continue;
        }
        else
          scribbleDoc->history->addItem(new StrokeAddedItem(s, currPage, nexts));
      }
    }
    delete freeErasePieces;
    freeErasePieces = NULL;
    // fall through to delete tempSelection
  }
  case MODE_ERASESTROKE:
  case MODE_ERASERULED:
    tempSelection->deleteStrokes();
    break;
  case MODE_SELECTRECT:
    gestureRegion.addRect(rectSelector->rect());
    rectSelector->drawHandles = true;
    useShapeSelector();
    break;
  case MODE_SELECTRULED:
    // lasso and ruled selectors don't support any interaction, so always convert to rect sel for now
    if(currSelection->count() > 0) {
      // replace ruled selector with rect selector
      delete ruledSelector;
      ruledSelector = NULL;
      rectSelector = new RectSelector(currSelection, mZoom, false);  // ruled sel starts in move ruled mode
      useShapeSelector();
    }
    break;
  case MODE_SELECTLASSO:
    gestureRegion = lassoSelector->path();
    if(currSelection->count() > 0) {
      // replace lasso selector with rect selector
      delete lassoSelector;
      lassoSelector = NULL;
      rectSelector = new RectSelector(currSelection, mZoom, true);
      useShapeSelector();
    }
    break;
  case MODE_SELECTPATH:
    if(currSelection->count() > 0) {
      delete pathSelector;
      pathSelector = NULL;
      rectSelector = new RectSelector(currSelection, mZoom, true);
      useShapeSelector();
    }
    break;
  case MODE_MOVESELFREE:
  case MODE_MOVESELRULED:
  {
    // NOTE: move sel modes must handle undo (startAction, endAction) explicitly!
    // remove from overlay
    currSelection->setDrawType(regionSelector ? Selection::STROKEDRAW_NORMAL : Selection::STROKEDRAW_SEL);
    app->overlayWidget->drawSelection(NULL);
    // A region is part of its page: a tap does not toggle ruled move, and a drop outside the window or on
    //  another page is not a move at all (the cut-and-paste those do would lose the region's identity).
    if(regionSelector && ((event.modemod & MODEMOD_FASTCLICK) || !screenRect.contains(prevRawPos)
        || dimToPageNum(screenToDim(prevRawPos)) != currPageNum)) {
      currSelection->setOffset(0, 0);
      currSelection->xchgBGDirty(true);
      break;
    }
    // use fast click thresholds to minimize interference with small intentional movements
    if(event.modemod & MODEMOD_FASTCLICK) {
      // tap selection to toggle move free/ruled
      currSelection->selector->drawHandles = !currSelection->selector->drawHandles;
      scribbleDoc->scribbleMode->moveSelMode =
          currSelection->selector->drawHandles ? MODE_MOVESELFREE : MODE_MOVESELRULED;
      // cancel any translation
      currSelection->setOffset(0, 0);
      // force redraw of selection BG
      currSelection->xchgBGDirty(true);
      // redisplay tools
      if(cfg->Bool("popupToolbar")) {
        Rect r = currSelection->getBGBBox();
        app->showSelToolbar(screenToGlobal(dimToScreen(pageDimToDim(Point(r.right, r.bottom)))));
      }
      break;
    }

    // handle drop outside of widget
    if(!screenRect.contains(prevRawPos)) {
      currSelection->setOffset(0, 0);  // leave selected
      scribbleDoc->startAction(currPageNum | UndoHistory::MULTIPAGE);
      // if we pass offset to dropSelection, pos + offset is used by doPasteAt to determine page; instead,
      //  pos and offset should be passed separately to doPasteAt, and only pos used to determine page
      currSelection->setOffset(-initialPos);
      // hack to prevent selection from being cleared in case of same-doc drop
      Selection* sel = currSelection;
      currSelection = NULL;
      bool accepted = app->overlayWidget->dropSelection(sel, screenToGlobal(prevRawPos), Point(0,0));  //-initialPos);
      currSelection = sel;
      currSelection->setOffset(0, 0);
      if(accepted) {
        currSelection->deleteStrokes();
        clearSelection();
      }
      scribbleDoc->endAction();
      break;
    }

    Point pos = screenToDim(prevRawPos);
    int newpagenum = dimToPageNum(pos);
    Point pagepos = pos - getPageOrigin(newpagenum);  // getPageOrigin now works for ghost page
    Page* newpage = page(newpagenum);
    // dropped on ghost page - create the page ... after starting undo action!
    if(newpagenum == numPages() && scribbleDoc->ghostPage && scribbleDoc->ghostPage->rect().contains(pagepos)) {}
    else if(!newpage || !newpage->rect().contains(pagepos)) {
      // dropped outside page - do nothing
      currSelection->setOffset(0, 0);
      break;
    }
    if(newpagenum != currPageNum) {
      // handle drop outside of current page
      bool isMoveFree = currSelection->selector->drawHandles;  // preserve mode
      Point origin1 = pageDimToDim(currSelection->getBBox().origin());
      if(!newpage) {
        scribbleDoc->startAction(newpagenum | UndoHistory::MULTIPAGE);
        scribbleDoc->document->insertPage(scribbleDoc->generatePage(newpagenum), newpagenum);
        pageSizeChanged();
        uiChanged(UIState::InsertPage);
      }
      else
        scribbleDoc->startAction(currPageNum | UndoHistory::MULTIPAGE);
      Clipboard clipboard;
      currSelection->setOffset(0, 0);
      currSelection->toSorted(&clipboard);
      currSelection->deleteStrokes();
      clearSelection();
      setPageNum(newpagenum);
      currSelection = new Selection(currPage);
      rectSelector = new RectSelector(currSelection, mZoom, isMoveFree);
      clipboard.paste(currSelection, true);
      Point origin2 = pageDimToDim(currSelection->getBBox().origin());
      Point dr = origin1 - origin2;

      if(currMode == MODE_MOVESELRULED && currPage->xruling() > 0)
        dr.x = quantize(dr.x, currPage->xruling());
      if(currMode == MODE_MOVESELRULED && currPage->yruling() > 0)
        dr.y = quantize(dr.y, currPage->yruling());
      currSelection->stealthTransform(Transform2D::translating(dr));
    }
    else {
      scribbleDoc->startAction(currPageNum);
      currSelection->commitTransform();
    }
    growPage(currSelection->getBBox());
    scribbleDoc->endAction();
    break;
  }
  case MODE_SCALESEL:
  case MODE_SCALESELW:
  case MODE_ROTATESEL:
  case MODE_ROTATESELW:
    currSelection->commitTransform();
    // possible shrink of image on commit() can invalidate bbox
    currSelection->invalidateBBox();
    growPage(currSelection->getBBox());
    break;
  case MODE_CROPSEL:
    if(tempSelection) {
      Element* s = tempSelection->strokes.front();
      Element* t = currSelection->strokes.front();
      currPage->contentNode->removeChild(t->node);
      // the cropped copy replaces the original in place, so it keeps the original's layer rather
      //  than being treated as newly drawn content
      currPage->addStroke(t, s, s->layer());
      currPage->removeStroke(s);
    }
    // internalScale == 0 defeats check for identity tf in commitTransform()
    if(approxEq(currSelection->transform.tf(), Transform2D(), 1E-7))
      currSelection->resetTransform();
    else
      currSelection->commitTransform();
    break;
  case MODE_INSSPACEVERT:
    // TODO: enforcing a minimum page size; maybe in Page::setProperties()
    // use insert space tool above empty space to change page size
    // This probably should happen in real time (i.e. in moveEvent!)
    if(tempSelection->count() > 0) {
      tempSelection->commitTransform();
      growPage(tempSelection->getBBox(), 0, std::max(Dim(0), prevPos.y - initialPos.y));
    }
    else
      setPageDims(-1, currPage->height() + (prevPos.y - initialPos.y));
    break;
  case MODE_INSSPACEHORZ:
    if(tempSelection->count() > 0) {
      tempSelection->commitTransform();
      growPage(tempSelection->getBBox(), std::max(Dim(0), prevPos.x - initialPos.x), 0);
    }
    else
      setPageDims(currPage->width() + (prevPos.x - initialPos.x), -1);
    break;
  case MODE_INSSPACERULED:
    // use insert space tool above empty space to change page size
    if(tempSelection->count() > 0) {
      // this could be greater due to reflow
      Dim maxdy = tempSelection->strokes.back()->pendingTransform().yoffset();
      tempSelection->commitTransform();
      growPage(tempSelection->getBBox(), 0, std::max(Dim(0), maxdy));
    }
    else if(!gestureFrame.region)  // a region's lines say nothing about the page's height
      setPageDims(-1, currPage->height() + gestureFrame.yrulingOr(Page::BLANK_Y_RULING) * (prevLine - initialLine));
    if(insSpaceEraseSelection)
      insSpaceEraseSelection->deleteStrokes();
  default:
    break;
  }
  // a gesture that caught nothing still marks an area, which Screenshot can capture
  bool regionOnly = currSelection && currSelection->count() == 0 && currModeType == MODE_SELECT
      && gestureRegion.getBBox().isValid() && gestureRegion.getBBox().width() > 0
      && gestureRegion.getBBox().height() > 0;
  int gesturePage = currSelPageNum;
  auto keepRegion = [&](){
    shotRegion = gestureRegion;
    shotRegionPage = gesturePage;
    dirtyShotRegion();
  };
  // set before the popup opens, which offers Screenshot only when there is a region
  if(currSelection && currSelection->count() > 0 && currModeType == MODE_SELECT && !gestureRegion.empty())
    keepRegion();
  if(currSelection) {
    if(currSelection->count() == 0) {
      clearSelection();  // for select or erase within selection
      if(regionOnly) {
        keepRegion();
        if(cfg->Bool("popupToolbar"))
          app->showSelToolbar(screenToGlobal(prevRawPos));
      }
    }
    else {
      currSelection->shrink();
      Rect b = dimToScreen(pageDimToDim(currSelection->getBGBBox()));
      if(currModeType == MODE_SELECT) {
        if(cfg->Bool("popupToolbar")) {
          // prevent overlap w/ selection; might need to revisit to handle being shifted to stay on-screen
          Dim y = b.right + 2 > prevRawPos.x && b.bottom + 2 > prevRawPos.y ? b.bottom + 2 : prevRawPos.y;
          if(cfg->Bool("popupToolbar"))
            app->showSelToolbar(screenToGlobal(Point(prevRawPos.x, y)));
        }
      }
#ifdef ONE_TIME_TIPS
      // one-time help tips for selection - on mobile, there is no hover to allow for regular tooltips
      if(!showHelpTips) {}
      else if(currMode == MODE_SCALESEL)
        app->oneTimeTip("scalesel", Point(b.right, b.bottom), _("Bottom right handle scales proportionally.\n"
            "Use other corners to scale freely.\nHold pen button to also scale stroke widths.\n"));
      else if(currMode == MODE_ROTATESEL)
        app->oneTimeTip("rotatesel", Point(b.right, b.bottom), _("Hold pen button to rotate in 15 degree steps.\n"));
      else if(currModeType != MODE_SELECT) {}
      else if(!rectSelector->drawHandles || !rectSelector)
        app->oneTimeTip("movesel", Point(b.right, b.bottom), _("Tap selection to toggle ruled/free mode."));
      else if(rectSelector->enableCrop)
        app->oneTimeTip("cropsel", Point(b.right, b.bottom), _("Drag red handles to crop image."));
      else  // sel toolbar will be shown in this case, so shift the help tip up
        app->oneTimeTip("rectsel", Point(b.right, b.bottom + 50), _("Scale with square handles, rotate with round handle."));
#endif
    }
  }
  clearTempSelection();
  if(undoable)
    scribbleDoc->endAction();

  if(currPage->yruling() == 0)
    currPage->yRuleOffset = 0;
  uiChanged(UIState::ReleaseEvent | (currMode == MODE_STROKE ? UIState::PenRelease : 0));
  currMode = MODE_NONE;
  scribbleDoc->scribbleDone();
}

// For stroke, erase, insert space, and move selection, cancel is equiv to release + undo
// But we still need cancel for pan and select
void ScribbleArea::doCancelAction(bool refresh)
{
  // Should we also call BookmarkView's doCancelAction() in case it's scrolling?
  ScribbleView::doCancelAction();
  cancelShape();
  if(!currSelection)
    clearShotRegion();  // Esc on an area marked with nothing selected
  // a press placing tags is over, but the tags stay on the pointer: this runs on every save too, and an
  //  autosave must not drop them - only Esc does (ScribbleApp::keyPressEvent)
  placingPressed = false;
  todoPressed = NULL;
  switch(currMode) {
  case MODE_DRAWSHAPE:
    if(currStroke) {
      scribbleDoc->updateCurrStroke(currStroke->bbox());
      currStroke->deleteNode();
      currStroke = NULL;
    }
    if(regionInProgress) {
      scribbleDoc->updateCurrStroke(regionInProgress->bbox());
      regionInProgress->deleteNode();
      regionInProgress = NULL;
    }
    break;
  case MODE_SHAPEHANDLE:
  {
    if(regionSelector && regionHandleStart.isValid()) {
      dirtyScreen(currSelection->getBGBBox());
      regionSelector->region->setRegionParams(regionHandleStart);
      currSelection->invalidateBBox();
      currSelection->xchgBGDirty(true);
      dirtyScreen(currSelection->getBGBBox());
      regionHandleStart = RulingRegionParams();
      shapeHandleIdx = -1;
      break;
    }
    Element* shape = shapeSelector ? shapeSelector->shapeElement() : NULL;
    if(shape && shapeHandleStart.isValid()) {
      dirtyScreen(currSelection->getBGBBox());
      shape->setShapeParams(shapeHandleStart);
      currSelection->invalidateBBox();
      shapeSelector->updateHandles();
      currSelection->xchgBGDirty(true);
      dirtyScreen(currSelection->getBGBBox());
    }
    shapeHandleStart = ShapeParams();
    shapeHandleIdx = -1;
    break;
  }
  case MODE_NONE:
    groupStrokes();
    return;
  case MODE_PAN:
    panZoomCancel();
    break;
  case MODE_BOOKMARK:
    // must commit selection so that stroke bbox is correct for repaint
    currSelection->commitTransform();
    delete currSelection;
    app->overlayWidget->drawSelection(NULL);
    currSelection = NULL;
    // fall through
  case MODE_STROKE:
    // a snapped shape is in currStroke, so it is discarded below like the ink it replaced
    stopShapeSnap();
    snapActive = false;
    if(scribbleDoc->strokeBuilder) {
      currStroke = scribbleDoc->strokeBuilder->finish();
      delete scribbleDoc->strokeBuilder;
      scribbleDoc->strokeBuilder = NULL;
    }
    if(currStroke) {
      // need to redraw screen (but not image)
      scribbleDoc->updateCurrStroke(currStroke->bbox());
      currStroke->deleteNode();
      currStroke = NULL;
    }
    break;
  case MODE_ERASEFREE:
  case MODE_ERASEFREERULED:
    if(freeErasePieces)  // should never be NULL, but was in one case due to a bug
      freeErasePieces->deleteStrokes();
    delete freeErasePieces;
    freeErasePieces = NULL;
  case MODE_ERASESTROKE:
  case MODE_ERASERULED:
    // handled by clearTempSelection()
    break;
  case MODE_SELECTRECT:
  case MODE_SELECTRULED:
  case MODE_SELECTLASSO:
  case MODE_SELECTPATH:
    // note that we have not called shrink() yet here!
    clearSelection();
    break;
  case MODE_MOVESELFREE:
  case MODE_MOVESELRULED:
    // return to previous position but leave selected
    currSelection->setOffset(0, 0);
    currSelection->setDrawType(Selection::STROKEDRAW_SEL);
    app->overlayWidget->drawSelection(NULL);
    break;
  case MODE_SCALESEL:
  case MODE_SCALESELW:
  case MODE_ROTATESEL:
  case MODE_ROTATESELW:
    currSelection->resetTransform();
    break;
  case MODE_CROPSEL:
    if(tempSelection) {
      Element* s = tempSelection->strokes.front();
      Element* t = currSelection->strokes.front();
      currPage->contentNode->removeChild(t->node);
      currSelection->removeStroke(t);
      currSelection->addStroke(s);
      t->deleteNode();
    }
    currSelection->resetTransform();
    break;
  case MODE_INSSPACEVERT:
  case MODE_INSSPACEHORZ:
  case MODE_INSSPACERULED:
    setPageDims(initialPageSize.width(), initialPageSize.height());
    // delete selection without commiting - will return strokes to original position
    // setOffset(0, 0) is still necessary to update dirtyRect properly
    tempSelection->setOffset(0, 0);
    break;
  default:
    break;
  }
  clearTempSelection();

  if(currPage->yruling() == 0)
    currPage->yRuleOffset = 0;
  currMode = MODE_NONE;
  if(refresh) {  // && currMode != MODE_NONE ???
    scribbleDoc->scribbleDone();
    uiChanged(UIState::CancelEvent);
  }
}

bool ScribbleArea::doTimerEvent(Timestamp t)
{
  bool res = ScribbleView::doTimerEvent(t);
  // previously, we had config flags for erase, select, move sel and ins space - but these were never touched,
  //  so just hard code for select and move sel w/ a single flag
  if(!cfg->Bool("autoScrollSelect"))
    return res;
  // since selection can be dragged between areas, only auto scroll for move sel if still within current area
  int modetype = ScribbleMode::getModeType(currMode);
  if(modetype != MODE_MOVESEL && modetype != MODE_SELECT)
    return res;
  if(modetype == MODE_MOVESEL && !screenRect.contains(prevRawPos)
       && app->overlayWidget->canDrop(screenToGlobal(prevRawPos)))
    return true;  // don't autoscroll if over a drop target ... but don't stop timer

  Dim autoScrollJump = -cfg->Float("autoScrollSpeed");
  Dim dxright = std::min(AUTOSCROLL_BORDER, prevRawPos.x - (getViewWidth() - AUTOSCROLL_BORDER));
  Dim dxleft = std::max(-AUTOSCROLL_BORDER, prevRawPos.x - AUTOSCROLL_BORDER);
  int pandx = (dxright > 0 ? dxright : (dxleft < 0 ? dxleft : 0)) * autoScrollJump;
  Dim dyright = std::min(AUTOSCROLL_BORDER, prevRawPos.y - (getViewHeight() - AUTOSCROLL_BORDER));
  Dim dyleft = std::max(-AUTOSCROLL_BORDER, prevRawPos.y - AUTOSCROLL_BORDER);
  int pandy = (dyright > 0 ? dyright : (dyleft < 0 ? dyleft : 0)) * autoScrollJump;
  // do the actual panning
  if(pandx != 0 || pandy != 0) {
    doPan(pandx, pandy);
    // position of cursor in Dim space changes to keep screen position the same...
    InputEvent inevent;
    inevent.points.push_back(InputPoint(INPUTEVENT_MOVE, prevRawPos.x, prevRawPos.y));
    doMoveEvent(inevent);
  }
  // kinetic scrolling and "autoscroll" never happen simultaneously, so doRefresh() is only called once
  doRefresh();
  return true;
}

// cursor mode values (in addition to scribble modes, which start at 10)
static constexpr int CURSORMODE_SYSTEM = 0;
static constexpr int CURSORMODE_HIDE = 1;

// generic input event handler called for all motion (pressed or not) over area
void ScribbleArea::doMotionEvent(const InputEvent& event, inputevent_t eventtype)
{
  if(!event.points.empty() && eventtype != INPUTEVENT_RELEASE)
    rawPos = Point(event.points[0].x, event.points[0].y);
  // hover and drag alike carry tags being placed
  if(placingTags() && !event.points.empty() && eventtype != INPUTEVENT_RELEASE)
    movePendingTags(dimToPageDim(screenToDim(rawPos)));

  // previously, this was a separate fn, but we need input source information to fully determine cursorMode
  // ideally, we would hide cursor and reset releasePos on pen proximity exit
  int newCursorMode = cursorMode;
  // show system cursor as soon as the mouse moves unless it is enabled for drawing; also use system cursor if
  //  selection present (e.g., to make grabbing handles easier) - also hides frozen cursor bug w/ popup
  //  toolbar after clicking selection; consider using SvgGui.hoveredWidget instead of enter/leave events
  if(currSelection || eventtype == INPUTEVENT_LEAVE ||
      (event.source == INPUTSOURCE_MOUSE && scribbleInput->mouseMode != INPUTMODE_DRAW))
    newCursorMode = CURSORMODE_SYSTEM;
  // hide cursor on finger up; also need to do this for pen on mobile since we don't get hover events
  else if(eventtype == INPUTEVENT_RELEASE && (PLATFORM_MOBILE || event.source == INPUTSOURCE_TOUCH))
    newCursorMode = CURSORMODE_HIDE;
  // we need to set cursor on enter but we need to wait for the motion event with input source set
  // there are too many complications with touch+pen+mouse+window focus gain/lost to not check every event
  else if(eventtype == INPUTEVENT_PRESS || eventtype == INPUTEVENT_RELEASE || eventtype == INPUTEVENT_MOVE) {
    // except for above cases, cursor can only change on press, release, and enter
    int mode = scribbleDoc->getScribbleMode(event.modemod);
    // never hide mouse cursor
    if(currMode == MODE_STROKE)
      newCursorMode = CURSORMODE_HIDE;
    else if(drawCursor == 0 && mode == MODE_STROKE) {
      if(eventtype == INPUTEVENT_RELEASE) {
        releasePos = rawPos;
        newCursorMode = CURSORMODE_HIDE;
      }
      else if(!releasePos.isNaN() && releasePos.dist(rawPos) < 100)
        newCursorMode = CURSORMODE_HIDE;
      else {
        releasePos = Point(NAN, NAN);
        newCursorMode = MODE_STROKE;
      }
    }
    else {
      // current mode overrides mode passed in
      if(scribbleInput->scribbling == ScribbleInput::SCRIBBLING_PAN)
        mode = MODE_PAN;
      else if(scribbleInput->scribbling == ScribbleInput::SCRIBBLING_DRAW)
        mode = currMode;
      // override MODE_BOOKMARK mode type of MODE_STROKE
      newCursorMode = mode == MODE_BOOKMARK ? CURSORMODE_SYSTEM : ScribbleMode::getModeType(mode);
    }
  }
  // mouse cursor can NEVER be hidden; CURSORMODE_HIDE implies MODE_STROKE; must check this every time in
  //  case, e.g., mouse is moved after pen input with drawCursor == 0
  if(event.source == INPUTSOURCE_MOUSE && newCursorMode == CURSORMODE_HIDE) // || scribbleInput->currInputSource == INPUTSOURCE_MOUSE
    newCursorMode = MODE_STROKE;
  if(newCursorMode != cursorMode) {
    cursorMode = newCursorMode;
#if !PLATFORM_MOBILE
    // in the future, we will have custom cursors for other modes to support pen hover on iOS/Android
    if(cursorMode == CURSORMODE_HIDE || (drawCursor == 2 && (cursorMode == MODE_STROKE || cursorMode == MODE_ERASE))) {
      SDL_ShowCursor(SDL_DISABLE);
    }
    else {
      if(cursorMode == MODE_PAN)
        SDL_SetCursor(panCursor.get());
      else if(cursorMode == MODE_STROKE)
        SDL_SetCursor(penCursor.get());
      else if(cursorMode == MODE_ERASE)
        SDL_SetCursor(eraseCursor.get());
      else
        SDL_SetCursor(SDL_GetDefaultCursor());
      SDL_ShowCursor(SDL_ENABLE);
    }
#endif
  }

  if(drawCursor == 2 && (cursorMode == MODE_STROKE || cursorMode == MODE_ERASE)) {
    Dim hw = MIN_CURSOR_RADIUS*preScale, hh = MIN_CURSOR_RADIUS*preScale;
    if(cursorMode == MODE_STROKE) {
      Rect penbox = resolvedPen().getBBox();
      hw = std::max(hw, mScale*penbox.width()/2);
      hh = std::max(hh, mScale*penbox.height()/2);
    }
    else {
      hw = preScale*(ERASEFREE_RADIUS + 1);
      hh = hw;
    }
    Rect r = Rect::centerwh(rawPos, 2*hw, 2*hh).pad(1.5);  //ltrb(-hw, -hh, hw, hh).pad(1.5).translate(rawPos);
    dirtyRectScreen.rectUnion(hoverRect.rectUnion(r));
    hoverRect = r;
  }
  else if(hoverRect.isValid()) {
    // hide custom cursor
    dirtyRectScreen.rectUnion(hoverRect);
    hoverRect = Rect();
  }
}

// drawing related methods

void ScribbleArea::reqRepaint()
{
  //scribbleDoc->dirtyPage(currPageNum); ... this has to be done for all views before reqRepaint!
  // see if we need to redraw selection background
  Rect newbg = currSelection ? currSelection->getBGBBox() : Rect();
  if((currSelection && currSelection->xchgBGDirty(false)) || newbg != selBGRect)
    dirtyScreen(selBGRect.rectUnion(newbg));
  selBGRect = newbg;
  ScribbleView::reqRepaint();
}

void ScribbleArea::dirtyPage(int pagenum, Rect dirty)
{
  //Rect dirty = page(pagenum)->getDirty();
  dirty = (pagenum == currPageNum) ? pageDimToDim(dirty) : dirty.translate(getPageOrigin(pagenum));
  // pad by 2 units in Dim space since stroke bbox does not include widening of selected stroke
  // TODO: it would be better to do this in StrokeGroup if STROKEDRAW_SEL; also why 2 instead of 1???
  // this is only needed after union with dirtyBG for selections < min sel size and zoom > 1
  dirty.pad(2);
  // avoid merging in a dirty rect outside viewport - it would unnecessarily enlarge dirtyRectDim
  if(dirty.overlaps(viewportRect))
    dirtyRectDim.rectUnion(dirty);
}

void ScribbleArea::dirtyScreen(const Rect& dirty)
{
  // pad dirty rect by 2 pixels to account for antialiasing ... padding for AA now handled at a higher level
  dirtyRectScreen.rectUnion(dimToScreen(pageDimToDim(dirty)));  //.pad(2));
}

// drawing strokes must be as fast as possible, so just draw on top without
//  using the dirty rect mechanism when possible
void ScribbleArea::drawStrokeOnImage(Element* stroke)
{
#ifdef QT_CORE_LIB
  imgPaint->reset();
  imgPaint->setAntiAlias(true);
  imgPaint->translate(xorigin, yorigin);
  imgPaint->scale(mScale, mScale);
  imgPaint->translate(currPageXOrigin, currPageYOrigin);
  //stroke->draw(&imgPaint, Rect());
  SvgExtraStates extrastates;
  stroke->node->draw(imgPaint, extrastates);
#endif
}

void ScribbleArea::updateContentDim()
{
  contentHeight = 0;
  contentWidth = 0;
  // determine content dimensions based on view mode
  // TODO: I think we can cache these values and update in pageSizeChanged()
  switch(viewMode) {
  case VIEWMODE_SINGLE:
    contentHeight = currPage->height();
    contentWidth = currPage->width();
    break;
  case VIEWMODE_VERT:
    for(int ii = 0; ii < numPages(); ii++) {
      contentHeight += page(ii)->height() + pageSpacing;
      // TODO: this should be based on visible pages, not all pages!
      contentWidth = std::max(contentWidth, page(ii)->width());
    }
    break;
  case VIEWMODE_HORZ:
    for(int ii = 0; ii < numPages(); ii++) {
      // TODO: this should be based on visible pages, not all pages!
      contentHeight = std::max(contentHeight, page(ii)->height());
      contentWidth += page(ii)->width() + pageSpacing;
    }
    //default: break;
  }
  // calculate scroll limits from content and view dimensions
  // our approach to limiting horz scrolling is a bit cheesy ... can we improve it?
  Dim viewwidth = getViewWidth();
  Dim viewheight = getViewHeight();
  Dim border = cfg->Float("BORDER_SIZE");
  maxOriginX = cfg->Float("horzBorder");
  if(maxOriginX <= 0 || viewMode == VIEWMODE_HORZ)  // disable for horz scroll - need room for ghost page
    maxOriginX = border * viewwidth;
  //maxOriginX = MAX(maxOriginX, cfg->Float("panBorder")*preScale + 5);
  maxOriginY = border * viewheight;
  minOriginX = -contentWidth*mScale - maxOriginX + viewwidth;
  minOriginY = -contentHeight*mScale - maxOriginY + viewheight;
  // minOriginX/Y == maxOriginX/Y means no scrolling is possible, so make sure
  //  content is centered in window
  if(minOriginX > maxOriginX)
    minOriginX = maxOriginX = (minOriginX + maxOriginX)/2;
  freeMinOriginX = minOriginX;
  freeMaxOriginX = maxOriginX;
  updateHorzPanLock();
  // +10 to prevent very small scroll range which causes undesired behavior with scroll handle
  if(minOriginY + 10 > maxOriginY)
    minOriginY = maxOriginY = (minOriginY + maxOriginY)/2;
}

void ScribbleArea::drawWatermark(Painter* painter, Page* page, const Rect& dirty)
{
#ifdef SCRIBBLE_IAP
  if(!watermark)
    return;
  painter->clipRect(Rect::wh(page->props.width, page->props.height));
  Dim w = watermark->width, h = watermark->height;
  for(Dim y = 0; y < page->props.height; y += h) {
    for(Dim x = 0; x < page->props.width; x += w) {
      Rect dest = Rect::ltwh(x, y, w, h);
      if(dest.intersects(dirty))
        painter->drawImage(dest, *watermark);
    }
  }
#endif
}

// Note that we assume dirty rect is larger than clip rect so we don't have to deal with weirdness at the
//  boundaries from some stuff being antialiased but other stuff not, non-pixel aligned boundaries, etc.
// Night mode is "force dark": a document that is already dark is drawn as it is, not flipped to light.
//  It is a per-device reading preference, hence the global config rather than the document's.
const ColorMap* ScribbleArea::viewColorMap()
{
  if(!ScribbleApp::cfg->Bool("invertColors") || !scribbleDoc)
    return NULL;
  const Palette& pal = scribbleDoc->palette();
  if(!pal.families.empty()) {
    if(pal.isDarkPaper())
      return NULL;
    nightMap.setPalette(&pal);
  }
  else {
    if(currPage && oklchFromColor(currPage->props.color).L < 0.5)
      return NULL;
    nightMap.setPalette(NULL);
  }
  return &nightMap;
}

void ScribbleArea::drawImage(Painter* painter, const Rect& dirty)
{
  // draw the gray background - UI chrome, not document, so it bypasses the night mode map; night mode
  //  still wants the dark gray around dark pages whatever the UI theme is
  painter->save();
  Color background = painter->colorMap() ? BACKGROUND_COLOR_DARK : BACKGROUND_COLOR;
  painter->setColorMap(NULL);
  painter->setAntiAlias(false);
  painter->fillRect(dirty, background);
  painter->restore();

  // handle special case of dirty rect limited to current page
  Rect pagedirty = dimToPageDim(dirty);
  if(viewMode == VIEWMODE_SINGLE ||
     Rect::ltwh(0, 0, currPage->width() + pageSpacing, currPage->height() + pageSpacing).contains(pagedirty)) {
    painter->save();
    painter->translate(currPageXOrigin, currPageYOrigin);
    if(currPage->scaleFactor != 1) {
      painter->scale(currPage->scaleFactor);
      pagedirty = Transform2D::scaling(1/currPage->scaleFactor).mult(pagedirty);
    }
    currPage->draw(painter, pagedirty);
    drawWatermark(painter, currPage, pagedirty);
    painter->restore();
    return;
  }
  // general case
  Dim w = 0;
  Dim h = 0;
  for(int ii = 0; ii <= numPages(); ii++) {
    bool ghost = ii == numPages();
    Page* pg = ghost ? scribbleDoc->ghostPage.get() : page(ii);
    if(!pg) break;  // a read-only ScribbleArea, e.g., has no ghost page
    if((viewMode == VIEWMODE_VERT && h + pg->height() > dirty.top)
       || (viewMode == VIEWMODE_HORZ && w + pg->width() > dirty.left))  {
      // account for page centering
      if(!centerPages) {}
      else if(viewMode == VIEWMODE_VERT)
        w = (contentWidth - pg->width())/2;
      else if(viewMode == VIEWMODE_HORZ)
        h = (contentHeight - pg->height())/2;
      // convert global dirty rect to page dirty rect
      pagedirty = dirty;
      pagedirty.translate(-w, -h);
      // draw
      painter->save();
      painter->translate(w, h);
      if(pg->scaleFactor != 1) {
        painter->scale(pg->scaleFactor);
        pagedirty = Transform2D::scaling(1/pg->scaleFactor).mult(pagedirty);
      }
      if(ghost)
        painter->setOpacity(0.25);
      pg->draw(painter, pagedirty);
      drawWatermark(painter, pg, pagedirty);
      painter->restore();
    }
    if(viewMode == VIEWMODE_VERT)
      h += pg->height() + pageSpacing;
    else if(viewMode == VIEWMODE_HORZ)
      w += pg->width() + pageSpacing;
    if((viewMode == VIEWMODE_VERT && h > dirty.bottom) || (viewMode == VIEWMODE_HORZ && w > dirty.right))
      break;
  }
}

void ScribbleArea::drawScreen(Painter* painter, const Rect& dirty)
{
  // we want the option of not having to redraw strokes while selection is being
  //  made (this is liveSelect = false), so selection background must be drawn on top of
  //  paper and strokes
  // Note that background is not drawn for tempSelection
  painter->save();
  painter->translate(xorigin + panxoffset, yorigin + panyoffset);
  painter->scale(mScale, mScale);
  painter->translate(currPageXOrigin, currPageYOrigin);
  // draw in-progress stroke
  if(scribbleDoc->strokeBuilder) {
    painter->save();
    // this is a hack to fix the drawing of current stroke on top of any page except the correct one in other
    //  views; an alternative fix would be to just add the current stroke to the page (also allowing
    //  strokeBuilder to be moved back to ScribbleArea, among other simplifications), but I was concerned
    //  (probably prematurely and irrationally) about performance for very big pages
    if(scribbleDoc->activeArea->currPageNum != currPageNum) {
      Point origin = getPageOrigin(scribbleDoc->activeArea->currPageNum);
      painter->translate(origin.x - currPageXOrigin, origin.y - currPageYOrigin);
    }
    //scribbleDoc->strokeBuilder->draw(painter, Rect());
    SvgPainter(painter).drawNode(scribbleDoc->strokeBuilder->getElement()->node);
    painter->restore();
  }
  // currStroke is used for Add Bookmark and for the in-progress drag-gesture shape
  if(currStroke)
    SvgPainter(painter).drawNode(currStroke->node);
  if(shapeInProgress)
    SvgPainter(painter).drawNode(shapeInProgress->node);
  if(regionInProgress)
    SvgPainter(painter).drawNode(regionInProgress->node);
  drawRegionButtons(painter);
  // a tag found from the document browser: fades in and out PAGETAG_FLASH_PULSES times
  if(!flashRects.empty() && flashPageNum < numPages()) {
    Dim progress = Dim(PAGETAG_FLASH_FRAMES - flashTicks)/PAGETAG_FLASH_FRAMES;
    float strength = float(0.5 - 0.5*std::cos(2*M_PI*PAGETAG_FLASH_PULSES*progress));
    painter->save();
    if(flashPageNum != currPageNum) {
      Point origin = getPageOrigin(flashPageNum);
      painter->translate(origin.x - currPageXOrigin, origin.y - currPageYOrigin);
    }
    Color checked(0x2E, 0xA3, 0xCF);  // the theme's --checked, same in light and dark (ugui/theme.cpp)
    painter->setFillBrush(Color(checked).setAlphaF(0.3f*strength));
    painter->setStroke(Color(checked).setAlphaF(strength), PAGETAG_FLASH_STROKE/mScale,
        Painter::RoundCap, Painter::RoundJoin);
    for(const Rect& r : flashRects)
      painter->drawPath(Path2D().addRect(r));
    painter->restore();
  }
  if(!pendingTags.empty()) {
    painter->save();
    Point offset = pendingTagsPos - pendingTagsAnchor;
    painter->translate(offset.x, offset.y);
    for(Element* s : pendingTags)
      SvgPainter(painter).drawNode(s->node);
    painter->restore();
  }

  // the last selection gesture's area, dashed; it stays where it was drawn when the selection is moved
  if(!shotRegion.empty() && shotRegionPage < numPages()
      && (shotRegionPage == currPageNum || viewMode != VIEWMODE_SINGLE)) {
    painter->save();
    if(shotRegionPage != currPageNum) {
      Point origin = getPageOrigin(shotRegionPage);
      painter->translate(origin.x - currPageXOrigin, origin.y - currPageYOrigin);
    }
    // the dash array is read when the path is stroked, so it has to outlive this block's locals
    shotRegionDashes[0] = 6/mZoom;
    shotRegionDashes[1] = 4/mZoom;
    shotRegionDashes[2] = -1;
    bool lightPaper = page(shotRegionPage)->props.color.luma() > 127;
    painter->setFillBrush(Color::NONE);
    painter->setStroke(lightPaper ? Color::BLUE : Color::YELLOW, 1.5/mZoom, Painter::FlatCap, Painter::MiterJoin);
    painter->setDashArray(shotRegionDashes);
    painter->drawPath(shotRegion);
    painter->setDashArray(NULL);
    painter->restore();
  }

  // a region's selection draws its ink normally but still has handles
  if(currSelection && (currSelection->drawType() == Selection::STROKEDRAW_SEL || regionSelector)
      && (currSelPageNum == currPageNum || viewMode != VIEWMODE_SINGLE)) {
    if(currSelPageNum != currPageNum) {
      Point origin = getPageOrigin(currSelPageNum);
      painter->translate(origin.x - currPageXOrigin, origin.y - currPageYOrigin);
    }
    currSelection->setZoom(mZoom);
    currSelection->drawBG(painter);
  }
  painter->restore();

  // draw the current pen or tool when hovering - note different scaling for pen vs. eraser
  if(dirty.overlaps(hoverRect)) {
    painter->save();
    painter->translate(rawPos.x, rawPos.y);
    if(cursorMode == MODE_STROKE) {
      Rect penbox = resolvedPen().getBBox();
      // minimum pen cursor size = 2 pix * preScale
      Dim hw = std::max(MIN_CURSOR_RADIUS, mScale*penbox.width()/2);
      Dim hh = std::max(MIN_CURSOR_RADIUS, mScale*penbox.height()/2);
      painter->setStroke(Color::WHITE, 1);
      painter->setFillBrush(currPen()->color);
      if(currPen()->hasFlag(ScribblePen::TIP_CHISEL))
        painter->drawPath(Path2D().addRect(
            Rect::centerwh(Point(0,0), 2*hw/FilledStrokeBuilder::CHISEL_ASPECT_RATIO, 2*hw)));
      else
        painter->drawPath(Path2D().addEllipse(0, 0, hw + 0.5, hh + 0.5));
      // if rawPos is ever updated w/o updating hoverRect, this needs to become a rectUnion
      hoverRect = Rect::centerwh(rawPos, 2*hw, 2*hh).pad(1.5);
    }
    else if(cursorMode == MODE_ERASE) {
      Dim d = 2*ERASEFREE_RADIUS;
      //painter->scale(preScale, preScale);
      painter->setStroke(Color::BLACK, 1);
      painter->setFillBrush(Color::WHITE);
      painter->drawPath(Path2D().addEllipse(0, 0, d/2, d/2));
      hoverRect = Rect::centerwh(rawPos, d, d).pad(1);
    }
    painter->restore();
  }
}
