#include <time.h>
#include "mainwindow.h"
#include "scribbleapp.h"
#include "scribblearea.h"
#include "scribblewidget.h"
#include "scribblesync.h"
#include "bookmarkview.h"
#include "scribbledoc.h"
#include "pentoolbar.h"
#include "touchwidgets.h"
#include "addpagemenu.h"
#include "configdialog.h"
#include "sidebar.h"
#include "usvg/svgparser.h"

// SVG for main window, preferences info
#include "res_ui.cpp"

// TODO: figure out why just capturing mw doesn't work
#define SLOT(x) [=](){ app->x; }

// geometry of the floating main toolbar (tools row + options row form a single panel)
//  floatInset, floatTopInset and floatBtnSize are in basics.h - the sidebar aligns to them
static const real floatCorner = 12*floatUIScale;
// horizontal padding between a floating panel's rounded background and its first/last button:
//  the mockup gives the editing panel more room than the page/file ops panels, and none at all
//  to the lone overflow button
static const Dim floatPad = 12*floatUIScale;
static const Dim floatSidePad = 12*floatUIScale;
// the separator's prototype reserves a fixed 16px cell that does not scale with the panel, so next to
//  the scaled cells it reads as a gap of its own rather than as a divider between two neighbours
static const Dim floatSepWidth = 10*floatUIScale;
// Buttons in the floating panels are bigger than the app-wide #toolbutton prototype (36x42 with a
//  23px icon), per the design mockup (64x64 with a 32x32 icon).  The prototype is shared by every
//  menu, dialog and toolbar in the app, so instead of changing it we resize the buttons of these
//  panels (and only these) after they have been created.
static const Dim floatIconSize = 32*floatUIScale;
// the page/file ops panels draw their icons larger than the editing tools
static const Dim floatSideIconSize = 42*floatUIScale;
// the divider between the tools row and the options row stops short of the panel's padding; this is
//  measured from the content edge, so it carries the 10.5 the padding above gave up, leaving the
//  divider where the mockup put it while the row itself sits closer to the panel edge
static const Dim optsDividerInset = 38*floatUIScale;
// the help and settings buttons are the mockup's narrower secondary cells
static const Dim floatSmallBtnSize = 48*floatUIScale;
static const Dim floatSmallIconSize = 24*floatUIScale;

// free eraser radii offered by the eraser options row, in screen units
static const Dim ERASER_RADII[] = {3, 7, 12};
// same look as the pen thickness presets (see PenToolbar): a bare line in a swatch-sized cell
static std::string eraserWidthBtnSVG()
{
  return fstring(R"#(
  <g class="toolbutton swatch-btn" layout="box">
    <rect class="background" width="%g" height="%g"/>
    <line class="icon width-line" x1="%g" y1="%g" x2="%g" y2="%g"
        fill="none" stroke="currentColor" stroke-width="1" stroke-linecap="round"/>
  </g>
)#", 44*floatUIScale, 64*floatUIScale,
      15.5*floatUIScale, 32*floatUIScale, 28.5*floatUIScale, 32*floatUIScale);
}

static void scaleFloatToolbutton(Widget* btn, Dim btnw, Dim iconSize)
{
  Widget* bg = btn->selectFirst(".background");
  if(bg && bg->node->type() == SvgNode::RECT) {
    // the prototype's background stretches to the button, which is sized by its icon; here it is the
    //  other way round - the background is the mockup's cell and sets the button's size
    bg->node->removeAttr("box-anchor");
    static_cast<SvgRect*>(bg->node)->setRect(Rect::wh(btnw, floatBtnSize));
  }
  Widget* icon = btn->selectFirst(".icon");
  if(icon && icon->node->type() == SvgNode::USE)
    static_cast<SvgUse*>(icon->node)->setViewport(Rect::wh(iconSize, iconSize));
}

// the document title button is sized by its label rather than the grid, so its background has to
//  keep stretching horizontally; only its height, icon and text follow the scale
static void scaleFloatTitleBtn(Widget* btn, Dim iconSize)
{
  Widget* bg = btn->selectFirst(".background");
  if(bg && bg->node->type() == SvgNode::RECT)  // width here is just the minimum, bg is hfill
    static_cast<SvgRect*>(bg->node)->setRect(Rect::wh(36*floatUIScale, floatBtnSize));
  Widget* icon = btn->selectFirst(".icon");
  if(icon && icon->node->type() == SvgNode::USE)
    static_cast<SvgUse*>(icon->node)->setViewport(Rect::wh(iconSize, iconSize));
  Widget* title = btn->selectFirst(".title");
  if(title) {
    // the label shrinks with the panel, but not below the size at which the title stops being
    //  readable - it is the one piece of text in the floating toolbars
    title->node->setAttr<float>("font-size", std::max(Dim(11), 15*floatUIScale));
    title->setMargins(0, 9*floatUIScale);
  }
  btn->setMargins(0);
}

// resize every toolbutton inside one of the floating panels; the color/thickness swatches
//  (.swatch-btn) carry the mockup's geometry in their own prototype, so they are left alone
static void scaleFloatPanel(Widget* panel, Dim iconSize)
{
  for(Widget* btn : panel->select(".toolbutton")) {
    if(!btn || btn->node->hasClass("swatch-btn"))
      continue;
    // .float-wide-btn is the document title, whose cell is sized by its label rather than the grid
    if(btn->node->hasClass("float-wide-btn")) {
      scaleFloatTitleBtn(btn, iconSize);
      continue;
    }
    bool small = btn->node->hasClass("float-small-btn");
    scaleFloatToolbutton(btn, small ? floatSmallBtnSize : floatBtnSize,
        small ? floatSmallIconSize : iconSize);
    // the mockup's cells sit flush against each other - all of the spacing is inside the cell
    btn->setMargins(0);
  }
  for(Widget* sep : panel->select(".toolbar-separator")) {
    Widget* rule = sep->selectFirst(".separator");
    if(rule && rule->node->type() == SvgNode::RECT)
      static_cast<SvgRect*>(rule->node)->setRect(Rect::wh(2, 40*floatUIScale));
    // the unclassed sibling rect is the prototype's spacer, which sets how much room the separator
    //  takes; it is not scaled by the panel, so it has to be brought down with everything else
    for(SvgNode* child : sep->containerNode()->children()) {
      if(child->type() == SvgNode::RECT && !child->hasClass("separator"))
        static_cast<SvgRect*>(child)->setRect(Rect::wh(floatSepWidth, 36*floatUIScale));
    }
  }
}

MainWindow* createMainWindow()
{
  return new MainWindow(createWindowNode(mainWindowSVG));
}

MainWindow::~MainWindow()
{
  for(Action* action : actionList) {
    if(action->buttons.empty() && action->menu)
      delete action->menu->node;
    delete action;
  }
}

//template<typename T>
Action* MainWindow::createAction(const char* name,
    const char* title, const char* iconfile, const char* shortcut, const std::function<void()>& callback)
{
  const SvgNode* icon = (iconfile && iconfile[0]) ? SvgGui::useFile(iconfile) : NULL;
  // this works (but crashes due to double free) - might need it if we want to use stroked icon style for iOS
  //if(icon && !icon->parent())
  //  node->document()->namedNode("main-defs")->asContainerNode()->addChild(const_cast<SvgNode*>(icon));
  std::string titlestr(title);
  titlestr.erase(remove(titlestr.begin(), titlestr.end(), '&'), titlestr.end());

  Action* action = new Action(name, _(titlestr.c_str()), icon);
  actionList.push_back(action);
//#if !PLATFORM_MOBILE
  if(shortcut && shortcut[0])
    shortcuts.emplace(shortcut, action);
  if(callback)
    action->onTriggered = callback;
  return action;
}

Menu* MainWindow::createMenu(const char* name, const char* title, Menu::Align align, bool showicons)
{
  Menu* menu = ::createMenu(align, showicons);
  //menu->setObjectName(name);
  //menu->setTitle(_(title));
  return menu;
}

void MainWindow::refreshScribbleWidget(ScribbleWidget* w, const UIState* uiState)
{
  //pageNumLabel->setVisible(Application::cfg->Bool("displayPageNum"));
  if(w->pageNumLabel) {
    std::string s = fstring("%d / %d", uiState->pageNum, uiState->totalPages);
    // if we come up with additional use cases, we can consider moving the check into, e.g., TextBox
    if(s != w->pageNumLabel->text())
      w->pageNumLabel->setText(s.c_str());
  }
  if(w->prevPage)
    w->prevPage->setEnabled(uiState->pageNum > 1);
  if(w->nextPage) {
    bool lastpage = uiState->pageNum == uiState->totalPages;
    w->nextPage->setIcon(lastpage ? appendPageIcon : nextPageIcon);
    w->nextPage->setEnabled(!lastpage || (uiState->currPageStrokes > 0 && !uiState->rxOnly));
  }

  //if(cfg->Bool("extraPageInfo")) {
  //  int idx = sprintf(pagetext, "%s%d / %s%d (", (uiState->pagemodified != 0) ? "*" : "", currPageNum+1,
  //      uiState->docmodified ? "*" : "", numPages());
  if(w->timeRangeLabel) {
    bool showtime = ScribbleApp::cfg->Int("displayTimeRange") > 0 && w->node->bounds().width() > 325;
    w->timeRangeLabel->setVisible(uiState->minTimestamp > 0 && uiState->maxTimestamp > 0 && showtime);
    if(w->timeRangeLabel->isVisible()) {
      // we tried a mode to show max time as "3 days ago", etc. but I didn't like it - removed June 2 2019
      char tminstr[64];
      char tmaxstr[64];
      time_t tmin = uiState->minTimestamp/1000;
      time_t tmax = uiState->maxTimestamp/1000;  // in seconds
      // if we display "N days ago" etc, we want maxTimestamp, but use minTimestamp for showing/hiding year
      Timestamp ago = (mSecSinceEpoch() - uiState->minTimestamp)/1000;
      const char* timefmt = ago < 60*60*24*364 ? "%d %b %H:%M" : "%d %b %Y %H:%M";
      strftime(tminstr, sizeof(tminstr), timefmt, localtime(&tmin));
      strftime(tmaxstr, sizeof(tmaxstr), timefmt, localtime(&tmax));
      // \u2013 is en dash; u8 specifier is required for MSVC
      std::string s = fstring(u8"%s \u2013 %s", tminstr, tmaxstr);
      if(ScribbleApp::cfg->Int("displayTimeRange") > 1) {
        s += fstring(" (%.0f x %.0f)", uiState->pageWidth, uiState->pageHeight);
        s += uiState->activeSel ? fstring(" (%d/", uiState->currSelStrokes).c_str() : " (";
        s += fstring("%d strokes)", uiState->currPageStrokes);
      }
      if(s != w->timeRangeLabel->text())
        w->timeRangeLabel->setText(s.c_str());
    }
  }
  if(w->zoomLabel) {
    int zoom = int(uiState->zoom*100 + 0.5);
    // let's try hiding "100%" on status bar; need to add press-drag handler to icon!
    w->zoomLabel->setVisible(zoom != 100);  //w->pageNumLabel || zoom != 100);
    std::string s = fstring("%d%%", zoom);
    if(s != w->zoomLabel->text())
      w->zoomLabel->setText(s.c_str());
  }
}

void MainWindow::refreshCommonUI(ScribbleDoc* doc, const UIState* uiState)
{
  if(winTitle[0] != '*' && doc->isModified())
    setTitle(("*" + winTitle).c_str());
  else if(winTitle[0] == '*' && !doc->isModified())
    setTitle(winTitle.substr(1).c_str());
  //actionReset_Zoom->setEnabled(uiState->zoom != 1);  // UI doesn't always refresh on zoom
  actionSave->setEnabled(doc->isModified() || PLATFORM_EMSCRIPTEN);  // don't know if user canceled download
  actionUndo->setEnabled(doc->canUndo());
  actionRedo->setEnabled(doc->canRedo());
  // we're not going to bother changing tooltip text anymore
  undoRedoBtn->setEnabled(doc->canUndo() || doc->canRedo());
  // put the tip up the moment there is something to undo - on a blank document it would be pointing at
  //  a disabled button and explaining a gesture that does nothing, which is the mistake it exists to
  //  avoid.  Once per session; it stays until the panel is opened, and for good once actually used.
  if(historyTip && !historyTipShown && doc->canUndo()) {
    historyTipShown = true;
    historyTip->setVisible(true);
  }

  // a selected ruling region can only be deleted (which keeps its ink); the rest act on ink
  bool inkSel = uiState->activeSel && !uiState->regionSel;
  actionCut->setEnabled(inkSel || uiState->pageSel);
  actionCopy->setEnabled(inkSel || uiState->pageSel);
  actionDelete_Selection->setEnabled(uiState->activeSel || uiState->pageSel);
  actionDupSel->setEnabled(inkSel || uiState->pageSel);  // maybe only enable if 1 page selected?
  //actionSelect_Similar->setEnabled(uiState->activeSel);
  actionInvert_Selection->setEnabled(inkSel || uiState->pageSel);
  actionCreate_Link->setEnabled(inkSel);
  actionUngroup->setEnabled(uiState->selHasGroup);
  actionPaste->setEnabled(app->clipboard != NULL || !ScribbleApp::cfg->Bool("preloadClipboard"));

  actionPrevious_View->setEnabled(uiState->prevView);
  actionNext_View->setEnabled(uiState->nextView);
  action_Previous_Page->setEnabled(uiState->pageNum > 1);

  bool lastpage = uiState->pageNum == uiState->totalPages;
  action_Next_Page->setIcon(lastpage ? appendPageIcon : nextPageIcon);
  action_Next_Page->setEnabled(!lastpage || (uiState->currPageStrokes > 0 && !uiState->rxOnly));

  // disable add/insert/delete page actions for lecture mode whiteboards
  actionNew_Page_After->setEnabled(!uiState->rxOnly);
  actionNew_Page_Before->setEnabled(!uiState->rxOnly);
  actionSelect_Pages->setEnabled(!uiState->rxOnly);
  actionInsertDocument->setEnabled(!uiState->rxOnly);
  // change title button icon to a cloud when connected
  titleButton->setIcon(uiState->syncActive ? swbIcon : appIcon);

  // disable send now button if it will have no effect
  //if(actionSendNow)
  //  actionSendNow->setEnabled(uiState->syncActive && doc->scribbleSync->canSendHist());
  if(actionViewSync) {
    actionViewSync->setEnabled(uiState->syncActive);
    if(uiState->syncActive) {
      actionViewSync->setChecked(doc->scribbleSync->syncViewBox != ScribbleSync::SYNCVIEW_OFF);
      actionViewSyncMaster->setChecked(doc->scribbleSync->syncViewBox == ScribbleSync::SYNCVIEW_MASTER);
    }
  }
  // might just be temporary until I figure out a better interface
  if(uiState->syncActive != menuWhiteboardBtn->isEnabled()) {
    menuWhiteboardBtn->setEnabled(uiState->syncActive);
    menuWhiteboardBtn->setVisible(uiState->syncActive);
    actionSendImmed->setEnabled(!uiState->rxOnly);
    if(uiState->rxOnly) {
      actionViewSyncMaster->setEnabled(false);
      actionSendImmed->setChecked(false);
    }
  }

  PenPreview::bgColor = doc->getCurrPageColor();
  // setWindowModified does not exit early if no change
  /*if(isWindowModified() != uiState->docmodified)
    setWindowModified(uiState->docmodified);*/
}

// ideally, we'd avoid a full UI update on stroke finished, but lots of things can change, so let's just make
//  sure that there is no unnecessary layout or rendering
void MainWindow::refreshUI(ScribbleDoc* doc, int reason)
{
  UIState areaState;
  for(ScribbleArea* area : app->scribbleAreas) {
    ScribbleDoc* adoc = area->scribbleDoc;
    if(adoc) {
      area->updateUIState(&areaState);
      areaState.syncActive = adoc->scribbleSync && adoc->scribbleSync->isSyncActive();
      areaState.rxOnly = adoc->scribbleSync && !adoc->scribbleSync->enableTX;
      refreshScribbleWidget(area->widget, &areaState);
      if(area == app->activeArea())
        refreshCommonUI(adoc, &areaState);
    }
  }
  areaState.zoom = app->bookmarkArea->getZoom();
  refreshScribbleWidget(app->bookmarkArea->widget, &areaState);
  updateMode();
  syncRegionRow();
  if(sidebar)
    sidebar->refreshIfChanged();

  // if a pen was released, may need to save pen to recent pens list
  if(reason & (1 << UIState::PenRelease)) {
    int savepen = ScribbleApp::cfg->Int("savePenMode");
    if(savepen > 0) {
      ScribbleApp::cfg->savePen(app->currPen);
      if(savepen == 2)  // 2 = save to both doc and global
        doc->cfg->savePen(app->currPen);
    }
  }
}

Action* MainWindow::modeToAction(int mode)
{
  switch(mode) {
    case MODE_PAN:  return actionPan;
    // the three draw tools share MODE_STROKE and are distinguished by the current pen
    case MODE_STROKE:
      return app->scribbleMode->drawTool == ScribbleMode::DRAWTOOL_HIGHLIGHT ? actionHighlight :
          (app->scribbleMode->drawTool == ScribbleMode::DRAWTOOL_EPHEMERAL ? actionEphemeral : actionDraw);
    case MODE_BOOKMARK:  return actionAdd_Bookmark;
    // the active shape is shown by the options row, not by the tools row icon
    case MODE_DRAWSHAPE:  return actionShapes;
    // erase submode is shown by the two toggles on the erase options row, not by the top row icon
    case MODE_ERASE:
    case MODE_ERASEFREERULED:  return actionErase;
    case MODE_ERASESTROKE:  return actionStroke_Eraser;
    case MODE_ERASERULED:  return actionRuled_Eraser;
    case MODE_ERASEFREE:  return actionFree_Eraser;
    case MODE_SELECT:  return actionSelect;
    case MODE_SELECTRECT:  return actionRect_Select;
    case MODE_SELECTRULED:  return actionRuled_Select;
    case MODE_SELECTLASSO:  return actionLasso_Select;
    case MODE_SELECTPATH:  return actionPath_Select;
    case MODE_INSSPACE:  return actionInsert_Space;
    case MODE_INSSPACERULED:  return actionRuled_Insert_Space;
    case MODE_INSSPACEVERT:  return actionInsert_Space_Vert;
    case MODE_PAGESEL:  return actionSelect_Pages;
    default: return NULL;
  }
}

/// actions

void MainWindow::updateMode()
{
  int mode = app->scribbleMode->getMode();
  int nextmode = app->scribbleMode->getNextMode();
  //if(Application::gui->hoveredWidget == app->scribbleDoc->activeArea->widget)
  //  app->scribbleDoc->setCursorfromMode(mode);
  Action* subaction = modeToAction(mode);
  Action* action = modeToAction(ScribbleMode::getModeType(mode));
  if(!action || !subaction)
    return;
  if(action != checkedMode) {
    if(checkedMode)
      checkedMode->setChecked(false);
    action->setChecked(true);
    checkedMode = action;
  }
  mode != nextmode ? checkedMode->addClass("once") : checkedMode->removeClass("once");

  if(subaction != action) {
    if(subaction != checkedSubMode) {
      if(checkedSubMode)
        checkedSubMode->setChecked(false);
      if(subaction != actionCustom_Pen) // && subaction != actionAdd_Bookmark)
        subaction->setChecked(true);
      checkedSubMode = subaction;
    }
  }
  else {
    if(checkedSubMode)
      checkedSubMode->setChecked(false);
    checkedSubMode = NULL;
  }
  // the active shape is shown on the shape options row, like the eraser submodes below
  bool drawRegion = app->scribbleMode->drawRegion;
  for(int ii = 0; ii < SHAPE_COUNT; ++ii)
    actionShape[ii]->setChecked(!drawRegion && ii == app->scribbleMode->shapeId);
  actionRulingRegion->setChecked(drawRegion && mode == MODE_DRAWSHAPE);
  int shapeFlags = app->scribbleMode->shapeFlags;
  // the region tool draws no shape, so no shape option applies to it
  const ShapeDef* shapedef = drawRegion ? NULL : shapeDef(app->scribbleMode->shapeId);
  bool heads = shapedef && shapedef->allowsHeads;
  bool rounding = shapedef && shapedef->allowsRounding;
  shapeHeadStartToggle->setChecked(heads && (shapeFlags & SHAPEFLAG_HEADSTART));
  shapeHeadEndToggle->setChecked(heads && (shapeFlags & SHAPEFLAG_HEADEND));
  shapeRoundedToggle->setChecked(rounding && (shapeFlags & SHAPEFLAG_ROUNDED));
  // a box or ellipse has no ends to put a head on; a curve has no corners to round
  shapeHeadStartToggle->setEnabled(heads);
  shapeHeadEndToggle->setEnabled(heads);
  shapeRoundedToggle->setEnabled(rounding);
  int eraserMode = app->scribbleMode->eraserMode;
  eraseStrokeToggle->setChecked(eraserMode == MODE_ERASESTROKE || eraserMode == MODE_ERASERULED);
  eraseRuledToggle->setChecked(eraserMode == MODE_ERASERULED || eraserMode == MODE_ERASEFREERULED);
  eraseSwitchBackToggle->setChecked(app->scribbleMode->eraseSwitchBack);
  selectSwitchBackToggle->setChecked(app->scribbleMode->selectSwitchBack);
  insSpaceSwitchBackToggle->setChecked(app->scribbleMode->insSpaceSwitchBack);
  // with the master pref off no tool switches back, so the toggles would do nothing - say so instead
  bool stickyPref = ScribbleApp::cfg->Bool("doubleTapSticky");
  eraseSwitchBackToggle->setEnabled(stickyPref);
  selectSwitchBackToggle->setEnabled(stickyPref);
  insSpaceSwitchBackToggle->setEnabled(stickyPref);
  // an options row left open follows a mode change made outside the tools toolbar
  int modeType = ScribbleMode::getModeType(mode);
  if(openOptionsRow && openOptionsRow != modeType)
    showOptionsRow(modeType);
  // update pen toolbar
  app->updatePenToolbar();  //penToolbar->setPen(app->getPen());
}

// showing the row for a mode without one (e.g. pan) just hides all of them
void MainWindow::showOptionsRow(int modeType)
{
  openOptionsRow = modeType;
  penToolbarAutoAdj->setVisible(modeType == MODE_STROKE);
  eraseOptsRow->setVisible(modeType == MODE_ERASE);
  selectOptsRow->setVisible(modeType == MODE_SELECT);
  insSpaceOptsRow->setVisible(modeType == MODE_INSSPACE);
  shapeOptsRow->setVisible(modeType == MODE_DRAWSHAPE);
  if(vertToolbar)
    return;
  // tools row and options rows are children of a single panel with a single background, so all we
  //  have to do is hide the divider and the options row container when no options row is shown
  bool rowOpen = modeType == MODE_STROKE || modeType == MODE_ERASE
      || modeType == MODE_SELECT || modeType == MODE_INSSPACE || modeType == MODE_DRAWSHAPE;
  if(optsRowDivider)
    optsRowDivider->setVisible(rowOpen);
  if(optsRowContainer)
    optsRowContainer->setVisible(rowOpen);
}

// tapping the already active tool closes its options row
void MainWindow::selectTool(int modeType)
{
  bool close = openOptionsRow == modeType
      && ScribbleMode::getModeType(app->scribbleMode->getMode()) == modeType;
  // always set the mode so that double tap to lock still works
  app->setMode(modeType);
  showOptionsRow(close ? 0 : modeType);
}

void MainWindow::selectDrawTool(int tool)
{
  ScribbleMode* scribbleMode = app->scribbleMode;
  bool close = openOptionsRow == MODE_STROKE
      && scribbleMode->getMode() == MODE_STROKE && scribbleMode->drawTool == tool;
  app->setDrawTool(tool);
  showOptionsRow(close ? 0 : MODE_STROKE);
}

// tapping the already active shape closes the options row, as selectDrawTool does for the pens
void MainWindow::selectShape(int shapeid)
{
  ScribbleMode* scribbleMode = app->scribbleMode;
  bool close = openOptionsRow == MODE_DRAWSHAPE && !scribbleMode->drawRegion
      && scribbleMode->getMode() == MODE_DRAWSHAPE && scribbleMode->shapeId == shapeid;
  scribbleMode->drawRegion = false;
  scribbleMode->shapeId = shapeid;
  app->setMode(MODE_DRAWSHAPE);
  showOptionsRow(close ? 0 : MODE_DRAWSHAPE);
}

// the three option toggles (start head, end head, rounded corners) are a single set of flags, applied
//  to the shape about to be drawn and - if one is selected - to that shape as well
// The ruling region panel: a column of small floating panels beside the selected region - the kind of
//  ruling, its spacing, whether it covers what is behind it, "level" and delete.  Every change is one
//  undo step (ScribbleArea::setSelRegionParams); a spacing drag is previewed and recorded on release.
void MainWindow::editSelRegion(const std::function<void(RulingRegionParams&)>& change)
{
  ScribbleArea* area = app->activeArea();
  Element* region = area ? area->selectedRegion() : NULL;
  if(!region)
    return;
  RulingRegionParams params = region->regionParams();
  change(params);
  params.sanitize();
  area->setSelRegionParams(params);
}

// Placed on whichever side of the region has room - right, then left, below, above - which can only
//  be decided at layout time, since that is when the panel's own size is known.
class RegionPanel : public AbsPosWidget
{
public:
  using AbsPosWidget::AbsPosWidget;
  Rect anchor;  // the region's outline, in window coordinates
  Rect view;    // the part of the canvas the panel may cover

  void place(const Rect& anchorrect, const Rect& viewrect)
  {
    if(anchorrect == anchor && viewrect == view)
      return;
    anchor = anchorrect;
    view = viewrect;
    node->setDirty(SvgNode::BOUNDS_DIRTY);  // gets calcOffset called again
  }

  Point calcOffset(const Rect& parentbbox) const override
  {
    if(!anchor.isValid() || !view.isValid())
      return AbsPosWidget::calcOffset(parentbbox);
    Rect bbox = node->bounds();
    Dim width = bbox.width(), height = bbox.height(), gap = floatInset;
    auto clampX = [&](Dim x){ return std::max(view.left + gap, std::min(x, view.right - gap - width)); };
    auto clampY = [&](Dim y){ return std::max(view.top + gap, std::min(y, view.bottom - gap - height)); };
    Point pos;
    if(anchor.right + gap + width <= view.right - gap)
      pos = Point(anchor.right + gap, clampY(anchor.top));
    else if(anchor.left - gap - width >= view.left + gap)
      pos = Point(anchor.left - gap - width, clampY(anchor.top));
    else if(anchor.bottom + gap + height <= view.bottom - gap)
      pos = Point(clampX(anchor.left), anchor.bottom + gap);
    else if(anchor.top - gap - height >= view.top + gap)
      pos = Point(clampX(anchor.left), anchor.top - gap - height);
    else  // the region fills the view: over it, against the right edge
      pos = Point(view.right - gap - width, clampY(anchor.top));
    return pos - bbox.origin();
  }
};

enum { REGION_LINED = 0, REGION_SQUARED, REGION_DOTTED };

static int regionKind(const RulingRegionParams& params)
{
  return params.dotRadius > 0 ? REGION_DOTTED : params.xRuling > 0 ? REGION_SQUARED : REGION_LINED;
}

static const char* regionKindTitle(int kind)
{
  return kind == REGION_DOTTED ? _("Dotted") : kind == REGION_SQUARED ? _("Squared") : _("Lined");
}

// lined reuses the ruled icon; there is no squared or dotted glyph among the icons, so those two are
//  drawn to the same 24 unit grid here
static const SvgNode* regionKindIcon(int kind)
{
  static const char* squaredSVG = R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24">
    <g class="icon" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round">
      <path d="M3 7.5H21M3 12H21M3 16.5H21M7.5 3V21M12 3V21M16.5 3V21"/>
    </g></svg>)";
  static const char* dottedSVG = R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24">
    <g class="icon" fill="currentColor" stroke="none">
      <circle cx="6" cy="6" r="1.6"/><circle cx="12" cy="6" r="1.6"/><circle cx="18" cy="6" r="1.6"/>
      <circle cx="6" cy="12" r="1.6"/><circle cx="12" cy="12" r="1.6"/><circle cx="18" cy="12" r="1.6"/>
      <circle cx="6" cy="18" r="1.6"/><circle cx="12" cy="18" r="1.6"/><circle cx="18" cy="18" r="1.6"/>
    </g></svg>)";
  static const SvgDocument* squared =
      SvgGui::useFile("region-kind-squared", std::unique_ptr<SvgDocument>(SvgParser().parseString(squaredSVG)));
  static const SvgDocument* dotted =
      SvgGui::useFile("region-kind-dotted", std::unique_ptr<SvgDocument>(SvgParser().parseString(dottedSVG)));
  if(kind == REGION_SQUARED)
    return squared;
  if(kind == REGION_DOTTED)
    return dotted;
  return SvgGui::useFile(":/icons/ic_menu_toggle_ruled.svg");
}

// the slider is logarithmic: the useful spacings bunch up at the fine end
static const Dim regionMinSpacing = 10;
static const Dim regionMaxSpacing = 120;

static Dim regionSliderToSpacing(real pos)
{
  pos = std::min(std::max(pos, real(0)), real(1));
  return std::round(regionMinSpacing*std::pow(regionMaxSpacing/regionMinSpacing, pos));
}

static real regionSpacingToSlider(Dim spacing)
{
  spacing = std::min(std::max(spacing, regionMinSpacing), regionMaxSpacing);
  return std::log(spacing/regionMinSpacing)/std::log(regionMaxSpacing/regionMinSpacing);
}

// The selection popup's layer list, rebuilt each time the popup opens (never while it is open, since its
//  items close it).  Top layer first, as the sidebar lists them; the layer already holding the whole
//  selection is ticked and disabled, and a locked layer shows its lock - moving there is allowed (it is
//  how a locked layer is filled), but the ink is then out of reach and the selection is let go.
void MainWindow::refreshSelPopup()
{
  ScribbleArea* area = app->activeArea();
  ScribbleDoc* doc = app->activeDoc();
  const Selection* sel = area ? area->selection() : NULL;
  if(!doc || !moveLayerPopup)
    return;
  // a selection gesture that caught nothing leaves only its area, and the only thing to do with that is
  //  capture it
  bool hasInk = sel && !sel->strokes.empty();
  for(Button* btn : selInkButtons)
    btn->setVisible(hasInk);
  moveLayerBtn->setVisible(hasInk);
  actionScreenshot->setEnabled(area && (area->hasShotRegion() || hasInk));
  window()->gui()->deleteContents(moveLayerPopup->selectFirst(".child-container"));
  const LayerList& layerList = doc->layers();
  moveLayerBtn->setEnabled(layerList.size() > 1 && sel && !sel->strokes.empty());
  int selLayer = -1;  // the one layer every selected element is on, if there is one
  if(sel) {
    for(Element* element : sel->strokes) {
      if(selLayer == -1)
        selLayer = element->layer();
      else if(selLayer != element->layer()) {
        selLayer = -1;
        break;
      }
    }
  }
  for(int idx = layerList.size() - 1; idx >= 0; --idx) {
    const LayerInfo& info = layerList.layers[idx];
    int layerId = info.id;
    Button* item = moveLayerPopup->addItem(info.name.empty() ? _("Layer") : info.name.c_str(),
        info.locked ? SvgGui::useFile(":/icons/ic_menu_lock.svg") : NULL, [this, layerId](){
      if(ScribbleDoc* target = app->activeDoc())
        target->moveSelToLayer(layerId);
    });
    if(layerId == selLayer) {
      item->setChecked(true);
      item->setEnabled(false);
    }
  }
}

void MainWindow::buildRegionPanel()
{
  regionPanel = new RegionPanel(loadSVGFragment(
      "<g class='region-panel' position='absolute' layout='flex' flex-direction='column'></g>"));

  // each control is its own panel, rounded like the toolbar's, with a gap between them
  auto addPanel = [this](std::initializer_list<Widget*> contents) {
    Toolbar* panel = createToolbar();
    panel->node->setAttribute("box-anchor", "hfill");
    SvgRect* bg = static_cast<SvgRect*>(panel->selectFirst(".toolbar-bg")->node);
    bg->setRect(bg->getRect(), floatCorner, floatCorner);
    panel->selectFirst(".child-container")->setMargins(0, floatPad, 0, floatPad);
    for(Widget* widget : contents)
      panel->addWidget(widget);
    scaleFloatPanel(panel, floatIconSize);
    if(!regionPanel->containerNode()->children().empty())
      panel->setMargins(floatInset/2, 0, 0, 0);
    regionPanel->addWidget(panel);
  };
  auto titledButton = [](const SvgNode* icon, const char* title) {
    Button* btn = createToolbutton(icon, title, true);
    btn->node->addClass("float-wide-btn");  // sized by its label, as the document title is
    return btn;
  };

  // kind of ruling, a dropdown
  regionKindBtn = titledButton(regionKindIcon(REGION_LINED), regionKindTitle(REGION_LINED));
  SvgUse* chevron = new SvgUse(Rect::wh(floatSmallIconSize, floatSmallIconSize), "",
      SvgGui::useFile(":/icons/chevron_down.svg"));
  chevron->addClass("icon");
  regionKindBtn->selectFirst(".title")->node->parent()->asContainerNode()->addChild(chevron);
  ArrowPopup* kindMenu = createArrowPopup(Menu::VERT_LEFT);
  for(int kind : {REGION_LINED, REGION_SQUARED, REGION_DOTTED}) {
    kindMenu->addItem(regionKindTitle(kind), regionKindIcon(kind), [this, kind](){
      editSelRegion([kind](RulingRegionParams& p){
        Dim pitch = p.yRuling > 0 ? p.yRuling : Page::BLANK_Y_RULING;
        p.yRuling = pitch;
        p.xRuling = kind == REGION_LINED ? 0 : pitch;
        if(kind != REGION_DOTTED)
          p.dotRadius = 0;
        else if(p.dotRadius <= 0)
          p.dotRadius = std::max(Dim(1), pitch/20);
      });
    });
  }
  setupPopupMenu(regionKindBtn, kindMenu);
  setupTooltip(regionKindBtn, _("Lines, squares or dots inside the region"));
  addPanel({regionKindBtn});

  // spacing: squared and dotted keep their cells square
  // the color picker's slider in miniature: a rounded bar, and a caret with a gap in the panel's color
  //  cut around it (colors in theme.cpp, .region-panel)
  static const char* sliderSVG = R"#(
    <g class="slider" box-anchor="hfill" layout="box">
      <g box-anchor="hfill" layout="box" margin="0 6">
        <rect width="100" height="32" fill="none"/>
        <rect class="slider-bg" box-anchor="hfill" width="100" height="12" rx="6" ry="6"/>
      </g>
      <g class="slider-handle-container" box-anchor="left">
        <rect width="12" height="32" fill="none"/>
        <g class="slider-handle" transform="translate(6,0)">
          <rect class="slider-caret-gap" x="-4.5" y="3" width="9" height="26" rx="4.5" ry="4.5"/>
          <rect class="slider-caret" x="-2.5" y="6" width="5" height="20" rx="2.5" ry="2.5"/>
        </g>
      </g>
    </g>)#";
  static std::unique_ptr<SvgNode> sliderProto(loadSVGFragment(sliderSVG));
  regionSpacingSlider = new Slider(sliderProto->clone());
  regionSpacingText = createTextBox("40");
  regionSpacingText->setMargins(0, 0, 0, floatPad);
  regionSpacingSlider->onValueChanged = [this](real pos){
    ScribbleArea* area = app->activeArea();
    Element* region = area ? area->selectedRegion() : NULL;
    if(!region)
      return;
    if(!regionSlideStart)
      regionSlideStart.reset(new RulingRegionParams(region->regionParams()));
    Dim spacing = regionSliderToSpacing(pos);
    RulingRegionParams params = region->regionParams();
    params.yRuling = spacing;
    if(params.xRuling > 0)
      params.xRuling = spacing;
    params.sanitize();
    area->previewSelRegionParams(params);
    regionSpacingText->setText(fstring("%.0f", spacing).c_str());
  };
  // a drag previews; releasing it records the whole drag as one undo step
  regionSpacingSlider->selectFirst(".slider-handle")->addHandler([this](SvgGui*, SDL_Event* event){
    if(event->type == SDL_FINGERUP && regionSlideStart) {
      ScribbleArea* area = app->activeArea();
      Element* region = area ? area->selectedRegion() : NULL;
      std::unique_ptr<RulingRegionParams> start = std::move(regionSlideStart);
      if(region) {
        RulingRegionParams dragged = region->regionParams();
        area->previewSelRegionParams(*start);  // the undo item records what is there when it is made
        if(dragged.yRuling != start->yRuling || dragged.xRuling != start->xRuling)
          area->setSelRegionParams(dragged);
      }
    }
    return false;
  });
  setupTooltip(regionSpacingSlider, _("Spacing of the lines inside the region"));
  addPanel({regionSpacingSlider, regionSpacingText});

  // on/off settings get a checkbox in place of the icon: it reads as on/off, which a tinted icon did not
  auto checkButton = [&](const char* title, SvgNode*& checkNode) {
    Button* btn = titledButton(SvgGui::useFile(":/icons/ic_file_fill.svg"), title);
    btn->selectFirst(".icon")->setVisible(false);
    checkNode = loadSVGFragment(R"#(
      <g class="checkbox region-check">
        <rect fill="none" width="16" height="16"/>
        <rect x="2" y="2" width="12" height="12" rx="2.5" ry="2.5" fill="none" stroke="currentColor" stroke-width="1.25"/>
        <g class="checkmark">
          <rect x="1.5" y="1.5" width="13" height="13" rx="3" ry="3" fill="currentColor"/>
          <path class="region-check-tick" d="M4.6 8.3 L7 10.7 L11.4 5.6" fill="none" stroke-width="1.8"
              stroke-linecap="round" stroke-linejoin="round"/>
        </g>
      </g>)#");
    SvgNode* titleNode = btn->selectFirst(".title")->node;
    titleNode->parent()->asContainerNode()->addChild(checkNode, titleNode);
    return btn;
  };
  regionPaperToggle = checkButton(_("Background"), regionPaperCheck);
  regionPaperToggle->onClicked = [this](){
    editSelRegion([](RulingRegionParams& p){ p.opaque = !p.opaque; });
  };
  setupTooltip(regionPaperToggle, _("Cover what is behind the region with plain paper"));
  addPanel({regionPaperToggle});

  Button* outlineToggle = checkButton(_("Outline"), regionOutlineCheck);
  outlineToggle->onClicked = [this](){
    editSelRegion([](RulingRegionParams& p){ p.outline = !p.outline; });
  };
  setupTooltip(outlineToggle, _("Draw the region's edge, so it stands out from the page"));
  addPanel({outlineToggle});

  // lines back to horizontal, keeping the outline: the usual fix after rotating to match a tilted scan
  //  and then wanting to write level instead
  regionStraightenBtn = titledButton(SvgGui::useFile(":/icons/ic_menu_insert_space_ruled.svg"), _("Level"));
  regionStraightenBtn->onClicked = [this](){ editSelRegion([](RulingRegionParams& p){ p.angle = 0; }); };
  setupTooltip(regionStraightenBtn, _("Make the lines horizontal again"));
  addPanel({regionStraightenBtn});

  Button* deleteBtn = titledButton(SvgGui::useFile(":/icons/ic_menu_discard.svg"), _("Delete"));
  deleteBtn->onClicked = [this](){ app->activeDoc()->doCommand(ID_DELSEL); };
  setupTooltip(deleteBtn, _("Delete the region; its writing stays"));
  addPanel({deleteBtn});

  regionPanel->setVisible(false);
  selectFirst("#main-container")->addWidget(regionPanel);
}

void MainWindow::syncRegionRow()
{
  if(!regionPanel)
    return;
  ScribbleArea* area = app->activeArea();
  Element* region = area ? area->selectedRegion() : NULL;
  Rect anchor = region ? area->selRegionGlobalRect() : Rect();
  Rect view = region ? area->globalViewRect() : Rect();
  // keep clear of the floating toolbar, which lies over the top of the canvas
  if(region && mainToolbarPanel && mainToolbarPanel->isVisible())
    view.top = std::max(view.top, mainToolbarPanel->node->bounds().bottom);
  bool show = region && anchor.intersects(view);
  if(!region)
    regionSlideStart.reset();
  if(show) {
    const RulingRegionParams& p = region->regionParams();
    int kind = regionKind(p);
    regionKindBtn->setIcon(regionKindIcon(kind));
    regionKindBtn->setTitle(regionKindTitle(kind));
    if(!regionSlideStart) {
      regionSpacingSlider->setValue(regionSpacingToSlider(p.yRuling));
      regionSpacingText->setText(fstring("%.0f", p.yRuling).c_str());
    }
    for(auto check : {std::make_pair(regionPaperCheck, p.opaque), std::make_pair(regionOutlineCheck, p.outline)}) {
      if(check.second)
        check.first->addClass("checked");
      else
        check.first->removeClass("checked");
    }
    regionStraightenBtn->setEnabled(p.angle != 0);
    regionPanel->place(anchor, view);
  }
  if(regionPanel->isVisible() != show)
    regionPanel->setVisible(show);
}

void MainWindow::setShapeOptions()
{
  int flags = (shapeHeadStartToggle->isChecked() ? SHAPEFLAG_HEADSTART : 0)
      | (shapeHeadEndToggle->isChecked() ? SHAPEFLAG_HEADEND : 0)
      | (shapeRoundedToggle->isChecked() ? SHAPEFLAG_ROUNDED : 0);
  app->scribbleMode->shapeFlags = flags;
  ScribbleDoc* doc = app->activeDoc();
  if(doc)
    doc->setSelShapeOptions(flags, ScribbleApp::cfg->Float("shapeCornerRadius"));
  updateMode();
}

void MainWindow::setEraserWidth(int idx)
{
  idx = std::max(0, std::min(idx, int(NELEM(ERASER_RADII)) - 1));
  ScribbleArea::ERASEFREE_RADIUS = ERASER_RADII[idx];
  ScribbleApp::cfg->set("eraserWidth", idx);
  for(int ii = 0; ii < int(eraserWidthBtns.size()); ++ii)
    eraserWidthBtns[ii]->setChecked(ii == idx);
}

void MainWindow::setEraserMode()
{
  bool stroke = eraseStrokeToggle->isChecked();
  bool ruled = eraseRuledToggle->isChecked();
  app->setMode(stroke ? (ruled ? MODE_ERASERULED : MODE_ERASESTROKE)
      : (ruled ? MODE_ERASEFREERULED : MODE_ERASEFREE));
}

void MainWindow::toggleBookmarks()
{
  bool show = !actionShow_Bookmarks->checked();
  actionShow_Bookmarks->setChecked(show);
  if(show) {
    // we must do this before showing panel, since it could make parent bounds too big!
    Dim parentw = std::min(bookmarkPanel->parent()->node->bounds().width(), winBounds().width());
    if(bookmarkPanel->node->bounds().width() > 0.75*parentw)
      bookmarkSplitter->setSplitSize(0.75*parentw);
    app->repaintBookmarks();
  }
  bookmarkPanel->setVisible(show);
  bookmarkSplitter->setVisible(show);
}

void MainWindow::toggleFullscreen()
{
  bool fs = !(SDL_GetWindowFlags(sdlWindow) & SDL_WINDOW_FULLSCREEN);  // toggle
  if(fs)
    SDL_MaximizeWindow(sdlWindow);  // otherwise switches to some partial screen video mode!
  SDL_SetWindowFullscreen(sdlWindow, fs ? SDL_WINDOW_FULLSCREEN : 0);
  actionFullscreen->setChecked(fs);
#if PLATFORM_IOS
  selectFirst("#ios-statusbar-bg")->setVisible(!fs);
#endif
}

// COLORS_SPEC.md §10.1, resolved: for a themed document the theme's dark paper supersedes the old
//  XOR inversion.
//
// `colorXorMask` is a photographic negative - it flips every channel, so a themed cyan comes out an
//  unrelated orange. That was always true (red has always inverted to cyan), but a palette that was
//  computed to sit on a particular paper is exactly the thing a negative destroys: the contrast the
//  walk guaranteed is against the *old* paper, and the hues no longer belong to any theme.
//
// So a themed document inverts by **mirroring its own recipe** - `paperL -> 1 - paperL`, which is the
//  case the generator already handles by walking up from the cusp instead of down - and remapping the
//  strokes through `Palette::mapFrom()`. The result is a real dark-paper rendering of the same theme
//  rather than a negative of it, and round-trips exactly, since the mapping is by ordinal and variant.
//
// The cost, stated plainly: this makes Invert Colors a **document edit** rather than a view filter.
//  It is one undo step, but it is saved and it syncs - so it is no longer a way to read in the dark
//  without changing the file. Documents with no palette keep the original XOR path.
void MainWindow::toggleInvertColors()
{
  ScribbleDoc* doc = app->activeDoc();
  if(doc && !doc->palette().families.empty()) {
    PaletteRecipe mirrored = doc->cfg->themeRecipe();
    mirrored.paperL = 1 - mirrored.paperL;
    doc->restyleToTheme(mirrored, true, false);
    actionInvertColors->setChecked(doc->palette().isDarkPaper());
    redraw();
    return;
  }
  bool invert = !actionInvertColors->checked();
  actionInvertColors->setChecked(invert);
  ScribbleApp::cfg->set("invertColors", invert);
  redraw();
}

void MainWindow::togglePenToolbar()
{
  showOptionsRow(penToolbarAutoAdj->isVisible() ? 0 : MODE_STROKE);
  ScribbleApp::cfg->set("showPenToolbar", penToolbarAutoAdj->isVisible());
}

void MainWindow::toggleDisableTouch()
{
  bool disable = !actionDisable_Touch->checked();
  actionDisable_Touch->setChecked(disable);
  ScribbleInput::disableTouch = disable;
}

void MainWindow::toggleSyncView()
{
  actionViewSync->setChecked(!actionViewSync->isChecked());
  app->syncView();
}

void MainWindow::toggleSyncViewMaster()
{
  actionViewSyncMaster->setChecked(!actionViewSyncMaster->isChecked());
  // if view sync is disabled and user becomes master, automatically enable
  if(actionViewSyncMaster->isChecked())
    actionViewSync->setChecked(true);
  app->syncView();
}

// creation of second ScribbleArea is deferred until split is first opened
void MainWindow::toggleSplitView(int newstate)
{
  newstate = newstate == SPLIT_TOGGLE ? -splitState : newstate;
  static const char* icons[] = {":/icons/ic_menu_split_tb.svg", ":/icons/ic_menu_split_bt.svg",
      ":/icons/ic_menu_split_lr.svg", ":/icons/ic_menu_split_rl.svg"};
  actionSplitView->setIcon(SvgGui::useFile(icons[std::abs(newstate) - SPLIT_H12]));
  if(newstate < 0 && (splitState < 0 || !app->closeSplit()))
    return;  // saving of modified doc in split was canceled
  if(newstate < 0 && splitPlaceholder)
    splitPlaceholder->setVisible(false);

  if(app->scribbleAreas.size() < 2) {
    // create second ScribbleArea
    ScribbleArea* area = new ScribbleArea();
    app->scribbleAreas.push_back(area);
    ScribbleWidget* areaWidget = createScribbleAreaWidget(scribbleContainer2, area);
    areaWidget->focusIndicator = focusIndicator2;
    // added after the ScribbleArea so it is drawn (and hit tested) on top of it
    splitPlaceholder = createSplitPlaceholder(scribbleContainer2);
  }

  if(splitState < 0) {
    app->openSplit();
    // pane starts blank, offering a choice instead of immediately popping up the document list; set the
    //  fill here (not in CSS) so it tracks light/dark theme and invert colors like the canvas does
    Color canvas = ScribbleArea::BACKGROUND_COLOR;
    if(ScribbleApp::cfg->Bool("invertColors"))
      canvas.color ^= color_t(ScribbleApp::cfg->Int("colorXorMask"));
    splitPlaceholder->selectFirst(".split-placeholder-bg")->node->setAttr<color_t>("fill", canvas.color);
    splitPlaceholder->setVisible(true);
  }

  Widget* layoutContainer = selectFirst("#scribble-split-layout");
  Rect layoutrect = layoutContainer->node->bounds();

  bool split = newstate > 0;
  bool horz = newstate == SPLIT_H12 || newstate == SPLIT_H21;
  if(split != (splitState > 0)) {
    actionSplitView->setChecked(split);
    scribbleFocusContainer2->setVisible(split);
    scribbleSplitter->setVisible(split);
  }
  if(split) {
    static const char* flexdirs[] = {"column", "column-reverse", "row", "row-reverse"};
    static Splitter::SizerPosition sizerpos[] = {Splitter::BOTTOM, Splitter::TOP, Splitter::RIGHT, Splitter::LEFT};
    layoutContainer->node->setAttribute("flex-direction", flexdirs[newstate - SPLIT_H12]);
    scribbleFocusContainer2->node->setAttribute("box-anchor", horz ? "hfill" : "vfill");
    scribbleSplitter->setDirection(sizerpos[newstate - SPLIT_H12]);  //horz ? Splitter::BOTTOM : Splitter::RIGHT);
    scribbleSplitter->setSplitSize(horz ? layoutrect.height()/2 : layoutrect.width()/2);
  }
  splitState = newstate;
  ScribbleApp::cfg->set("splitLayout", newstate);
}

void MainWindow::orientationChanged()
{
#if PLATFORM_IOS
  // the crux here is iPhone notch - we need a big offset in portrait but none in landscape
  // note that iPhone point sizes are roughly the same as our UI units, so no conversion needed
  Widget* statusBarBG = selectFirst("#ios-statusbar-bg");
  float top, bottom;
  // iosSafeAreaInsets will return 1 for iPhone, 0 for iPad (for which we assume our default insets)
  if(iosSafeAreaInsets(&top, &bottom)) {
    // non-notch iPhone status bar doesn't seem to be included in safe area inset; unfortunately, SDL
    //  only hides status bar if fullscreen is set, whereas default iOS behavior is to hide in phone landscape
    static_cast<SvgRect*>(statusBarBG->node)->setRect(Rect::wh(20, std::max(20.0f, top)));
    // this will only matter for initial call (before ScribbleAreas are created)
    scribbleAreaStatusInset.y = std::min(std::max(bottom, 6.0f), 18.0f);
  }
  statusBarBG->setVisible(!(SDL_GetWindowFlags(sdlWindow) & SDL_WINDOW_FULLSCREEN));
#endif
}

/// Setup

// now only used for filename label
static TextBox* createTextLabel(const char* box_anchor, Dim top, Dim right, Dim bottom, Dim left)
{
  // color should be set with CSS
  static const char* textLabelSVG = R"(
    <g class="textlabel" layout="box">
      <rect box-anchor="hfill" fill="black" fill-opacity="0.65" width="20" height="24"/>
      <text font-size="14" margin="4 8"></text>
    </g>
  )";
  static std::unique_ptr<SvgNode> proto;
  if(!proto)
    proto.reset(loadSVGFragment(textLabelSVG));

  TextBox* textLabel = new TextBox(proto->clone());
  textLabel->node->setAttribute("box-anchor", box_anchor);
  textLabel->setMargins(top, right, bottom, left);
  return textLabel;
}

// add labels to container for ScribbleArea
ScribbleWidget* MainWindow::createScribbleAreaWidget(Widget* container, ScribbleArea* area)
{
  ScribbleWidget* areaWidget = ScribbleWidget::create(container, area);
  Widget* contents = container->selectFirst(".scribble-content");
  areaWidget->node->addClass("scribbleArea");
  areaWidget->fileNameLabel = createTextLabel("top left", 6, 0, 0, 6);
  areaWidget->pageNumLabel = new TextBox(createTextNode(""));
  areaWidget->timeRangeLabel = new TextBox(createTextNode(""));
  areaWidget->zoomLabel = new TextBox(createTextNode(""));
  areaWidget->pageNumLabel->setMargins(0, 4);
  areaWidget->timeRangeLabel->setMargins(0, 4);
  areaWidget->zoomLabel->setMargins(0, 4);

  Button* zoomBtn = createToolbutton(SvgGui::useFile(":/icons/ic_menu_zoom.svg"));
  zoomBtn->onClicked = SLOT(doCommand(ID_RESETZOOM));

  // typed zoom: right click or long press opens a percent field.  A typed value is applied without
  //  snapping - roundZoom() would pull a deliberate 105% to fit-width - and out of range values are
  //  clamped rather than refused, which is what SpinBox::updateValueFromText() would do
  Dim minZoomPct = 100*ScribbleApp::cfg->Float("MIN_ZOOM");
  Dim maxZoomPct = 100*ScribbleApp::cfg->Float("MAX_ZOOM");
  ArrowPopup* zoomPopup = createArrowPopup(Menu::VERT_RIGHT | Menu::ABOVE);
  SpinBox* zoomSpin = createTextSpinBox(100, 10, minZoomPct, maxZoomPct, "%.0f");
  zoomSpin->onValueChanged = [area](real pct){ area->zoomCenter(pct/100, false); };
  zoomSpin->addHandler([=](SvgGui* gui, SDL_Event* event){
    if(event->type == SDL_KEYDOWN && event->key.keysym.sym == SDLK_RETURN) {
      auto zoomEdit = static_cast<TextEdit*>(zoomSpin->selectFirst(".textbox"));
      char* end = NULL;
      std::string text = zoomEdit->text();
      real pct = strtod(text.c_str(), &end);
      if(end != text.c_str()) {
        pct = std::min(std::max(pct, minZoomPct), maxZoomPct);
        zoomSpin->setValue(pct);
        area->zoomCenter(pct/100, false);  // even if the field already showed this value
      }
      closeAutoClosePopup(zoomPopup);
      return true;
    }
    return false;
  });
  zoomPopup->addWidget(createTitledRow(_("Zoom %"), zoomSpin));
  setupAutoClosePopup(zoomPopup);
  // undo the statusbar's scale(0.5) hack, or the popup is half the size of every other one
  zoomPopup->node->setTransform(Transform2D().scale(2));
  zoomBtn->addWidget(zoomPopup);

  // press and drag on zoom label to zoom ... not sure if I'll keep this
  float initX = 0;
  Dim initZoom = 0;
  //Widget* zoomLabel = areaWidget->zoomLabel;  -- press-drag was previous on label instead of button
  zoomBtn->addHandler([initX, initZoom, area, zoomBtn, zoomPopup, zoomSpin](SvgGui* gui, SDL_Event* event) mutable {
    if(isLongPressOrRightClick(event)) {
      zoomSpin->setValue(std::round(area->getZoom()*100));
      // not gui->setPressed(zoomPopup), as the title button does with its menu: the release then lands
      //  outside the popup and OUTSIDE_PRESSED closes it at once.  Swallowing the press is enough to keep
      //  the release from clicking the button, i.e. resetting zoom
      openAutoClosePopup(zoomPopup);
      gui->setFocused(zoomSpin, SvgGui::REASON_MENU);
      static_cast<TextEdit*>(zoomSpin->selectFirst(".textbox"))->selectAll();
      return true;
    }
    if(event->type == SDL_FINGERDOWN && event->tfinger.fingerId == SDL_BUTTON_LMASK) {
      initX = event->tfinger.x;   //gui->currInputPoint.x;
      initZoom = area->getZoom();
    }
    else if(event->type == SDL_FINGERMOTION && gui->pressedWidget == zoomBtn)
      area->zoomCenter(initZoom*pow(1.25, 0.05f*(event->tfinger.x - initX)), false);  //event->motion.x
    else if(event->type == SDL_FINGERUP || (event->type == SvgGui::OUTSIDE_PRESSED)) {
      area->zoomCenter(area->getZoom(), true);
    }
    return false;  // continue to button handler
  });
  setupTooltip(zoomBtn, altTooltip(_("Zoom"), _("Enter Zoom")), Tooltips::LEFT | Tooltips::BOTTOM | Tooltips::ABOVE);

  areaWidget->prevPage = createToolbutton(SvgGui::useFile(":/icons/ic_menu_prev.svg"), _("Previous Page"));
  areaWidget->prevPage->onClicked = SLOT(doCommand(ID_PREVPAGE));
  areaWidget->nextPage = createToolbutton(SvgGui::useFile(":/icons/ic_menu_next.svg"), _("Next Page"));
  areaWidget->nextPage->onClicked = SLOT(doCommand(ID_NEXTPAGENEW));
  setupTooltip(areaWidget->prevPage, _("Previous Page"), Tooltips::LEFT | Tooltips::BOTTOM | Tooltips::ABOVE);
  setupTooltip(areaWidget->nextPage, _("Next Page"), Tooltips::LEFT | Tooltips::BOTTOM | Tooltips::ABOVE);

  Button* timeRangeBtn = createToolbutton(SvgGui::useFile(":/icons/ic_menu_clock.svg"));
  timeRangeBtn->onClicked = [this](){
    ScribbleApp::cfg->set("displayTimeRange",  // should do right click/long press, but I'm lazy
        (SDL_GetModState() & KMOD_SHIFT) ? 2 : !ScribbleApp::cfg->Int("displayTimeRange"));
    refreshUI(app->activeDoc(), UIState::Command);
  };
  setupTooltip(timeRangeBtn, _("Time Range"), Tooltips::LEFT | Tooltips::BOTTOM | Tooltips::ABOVE);

  // toolbar background opacity is set via CSS to 0.75 - w/ 0.5, disabled icons don't show up w/ white page
  Toolbar* statusbar = createToolbar();
  statusbar->node->addClass("statusbar");
  statusbar->node->setAttribute("box-anchor", "bottom left");
  statusbar->setMargins(0, 0, scribbleAreaStatusInset.y, scribbleAreaStatusInset.x);

  // hack to make toolbar smaller - alternative would be to set toolbar dimensions in toolbar widget SVG
  //  instead of toolbutton SVG, then provide a separate mini-toolbar widget (or size arg for createToolbar())
  statusbar->node->setTransform(Transform2D().scale(0.5));
  //statusbar->node->setAttr<float>("font-size", 18);  -- set by CSS

  statusbar->addWidget(areaWidget->prevPage);
  statusbar->addWidget(areaWidget->pageNumLabel);
  statusbar->addWidget(areaWidget->nextPage);
  statusbar->addSeparator();
  statusbar->addWidget(zoomBtn);
  statusbar->addWidget(areaWidget->zoomLabel);
  statusbar->addSeparator();
  statusbar->addWidget(timeRangeBtn);
  statusbar->addWidget(areaWidget->timeRangeLabel);
  contents->addWidget(statusbar);

  contents->addWidget(areaWidget->fileNameLabel);
  areaWidget->fileNameLabel->setVisible(false);  // only shown for multi-doc split

  // we want container, not ScribbleWidget to be focusable so that pressing scroll handle switches focus too
  contents->isFocusable = true;
  contents->addHandler([this, area](SvgGui* gui, SDL_Event* event){
    if(event->type == SvgGui::FOCUS_GAINED)
      app->setActiveArea(area);
    // For pinch zoom, is CTRL press+release sent for every step?  Investigate before enabling this
    // Also, for this to work, have area take focus when ctrl + scroll zoom begins
    //if(event->key.keysym.sym == SDLK_RCTRL || event->key.keysym.sym == SDLK_LCTRL)
    //  area->zoomCenter(area->getZoom(), true);
    return false;  // this is kind of a hack, so pretend we don't exist
  });
  return areaWidget;
}

// blank second pane shown when a split is opened, offering the two things the user might want there
Widget* MainWindow::createSplitPlaceholder(Widget* container)
{
  Widget* placeholder = new Widget(loadSVGFragment(splitPlaceholderSVG));
  Widget* items = placeholder->selectFirst(".split-placeholder-items");

  Button* openDocBtn = createMenuItem(_("Open Document..."), SvgGui::useFile(":/icons/ic_menu_folder.svg"));
  openDocBtn->onClicked = [this](){
    if(app->openSplitDoc())
      splitPlaceholder->setVisible(false);
  };
  Button* thisDocBtn = createMenuItem(_("This Document"), SvgGui::useFile(":/icons/ic_menu_document.svg"));
  thisDocBtn->onClicked = [this](){ splitPlaceholder->setVisible(false); };
  items->addWidget(openDocBtn);
  items->addWidget(thisDocBtn);

  container->addWidget(placeholder);
  placeholder->setVisible(false);
  return placeholder;
}

// see Qt version of this method for theme colors
void MainWindow::setupTheme()
{
  appIcon = SvgGui::useFile(":/icons/write_icon_flat.svg");
  orientationChanged();
}

static Tooltips tooltipsInst;

void MainWindow::setupUI(ScribbleApp* a)
{
  app = a;
  // too lazy to do translations for tooltips (also, probably should use keys instead of English)
  if(!app->hasI18n)
    Tooltips::inst = &tooltipsInst;

  setupActions();

  // this is set so focus is not returned to pen toolbar edit boxes if lost
  isFocusable = true;

  // create and populate toolbars
  createToolBars();

  // if split is prevented from reaching min size set for spliiter, behavior will be incorrect on next drag
  bookmarkSplitter = new Splitter(containerNode()->selectFirst("#bookmark-splitter"),
      containerNode()->selectFirst("#bookmark-split-sizer"), Splitter::LEFT, 120);
  bookmarkPanel = selectFirst("#bookmark-panel");
  //bookmarkPanel->selectFirst(".panel-title")->setText(_("Bookmarks"));

  ComboBox* combobkmk = createComboBox({_("Bookmarks"), _("Margin Content")});
  combobkmk->isFocusable = false;
  combobkmk->setIndex(ScribbleApp::cfg->Int("bookmarkMode") == BookmarkView::MARGIN_CONTENT ? 1 : 0);
  combobkmk->onChanged = [this, combobkmk](const char* s){
    int mode = combobkmk->index() == 1 ? BookmarkView::MARGIN_CONTENT : BookmarkView::BOOKMARKS;
    ScribbleApp::cfg->set("bookmarkMode", mode);
    app->repaintBookmarks();
  };
  // don't put combo box on toolbar to allow narrower bookmark panel width (with toolbar covering combo box)
  selectFirst("#bookmarks-combo-container")->addWidget(combobkmk);

  Toolbar* bookmarkstb = createToolbar();
  bookmarkstb->addAction(actionAdd_Bookmark);
  bookmarkstb->addAction(actionBookmarksPin);
  bookmarkstb->addAction(actionBookmarksClose);
  selectFirst("#bookmarks-toolbar-container")->addWidget(bookmarkstb);

  Widget* scribbleContainer = selectFirst("#scribble-container");
  ScribbleWidget* areaWidget = createScribbleAreaWidget(scribbleContainer, app->activeArea());
  areaWidget->focusIndicator = selectFirst("#scribble-focus");

  // container and splitter for split - initially hidden
  scribbleContainer2 = selectFirst("#scribble-container-2");
  scribbleFocusContainer2 = selectFirst("#scribble-focus-container-2");
  // create splitter widget now so that layout doesn't create default Widget - behavior which should be fixed
  scribbleSplitter = new Splitter(containerNode()->selectFirst("#scribble-splitter"),
      containerNode()->selectFirst("#scribble-split-sizer"), Splitter::BOTTOM, 120);
  focusIndicator2 = selectFirst("#scribble-focus-2");

  ScribbleWidget* bkmkWidget = ScribbleWidget::create(selectFirst("#bookmark-container"), app->bookmarkArea);
  bkmkWidget->zoomLabel = createTextLabel("bottom left", 0, 0, 6, 6);
  selectFirst("#bookmark-container")->addWidget(bkmkWidget->zoomLabel);

  // overlay for rendering on top of everything else
  Widget* subWinLayout = selectFirst(".sub-window-layout");
  overlayWidget = new OverlayWidget(subWinLayout);
  overlayWidget->node->addClass("overlayWidget");
  selectFirst("#main-container")->addWidget(overlayWidget);

  // The general-purpose sidebar (sidebar.h) parents itself into one of these two containers
  //  depending on whether it is pinned, so it has to be created after both exist.
  sidebar = new Sidebar(this);
  if(ScribbleApp::cfg->Int("sidebarVisible"))
    sidebar->setOpen(true);
  updateSidebarButton();

  // popup selection toolbar
  Menubar* selToolbar = createMenubar();  // Menubar used instead of Toolbar so that popup closes after use
  selInkButtons.push_back(selToolbar->addAction(actionCut)); // TouchBar::HideText is default
  selInkButtons.push_back(selToolbar->addAction(actionCopy));
  selInkButtons.push_back(selToolbar->addAction(actionDupSel));
  selInkButtons.push_back(selToolbar->addAction(actionDelete_Selection));
  selInkButtons.push_back(selToolbar->addAction(actionCreate_Link));
  selToolbar->addAction(actionScreenshot);
  // Move to Layer: a labeled dropdown listing the layers, filled in by refreshSelPopup() on each open.
  //  Added with addWidget rather than Menubar::addButton, whose release handler closes the menu tree -
  //  which would take the layer list down with it the moment it opened.
  moveLayerBtn = createToolbutton(SvgGui::useFile(":/icons/ic_menu_pagesel.svg"), _("Move to Layer"), true);
  moveLayerBtn->node->addClass("float-wide-btn");  // sized by its label
  moveLayerPopup = createArrowPopup(Menu::VERT_LEFT);
  setupPopupMenu(moveLayerBtn, moveLayerPopup);
  setupTooltip(moveLayerBtn, _("Move the selection to another layer"));
  selToolbar->addWidget(moveLayerBtn);
  // rounded like the floating toolbar panels, with their button sizes
  SvgRect* selBg = static_cast<SvgRect*>(selToolbar->selectFirst(".toolbar-bg")->node);
  selBg->setRect(selBg->getRect(), floatCorner, floatCorner);
  selToolbar->selectFirst(".child-container")->setMargins(0, floatPad, 0, floatPad);
  scaleFloatPanel(selToolbar, floatIconSize);
  //selPopup = ::createMenu(Menu::VERT_RIGHT, false); -- requires event->user.data2 = 0 hack for OUTSIDE_MODAL
  // keeps the menu class, which closeMenus() walks up by; .sel-popup swaps the menu's all-round shadow
  //  for a rounded one cast downwards (theme.cpp)
  selPopup = new AbsPosWidget(loadSVGFragment(
       "<g class='menu sel-popup' position='absolute' box-anchor='fill' layout='box'></g>"));
  selPopup->addWidget(selToolbar);
  selPopup->setVisible(false);

  // an area marked with nothing selected lives only as long as its popup
  auto dropLoneRegion = [this](){
    ScribbleArea* area = app->activeArea();
    if(area && !area->selection())
      area->clearShotRegion();
  };
  selPopup->addHandler([this, dropLoneRegion](SvgGui* gui, SDL_Event* event){
    if(event->type == SvgGui::OUTSIDE_PRESSED) {
      gui->closeMenus();
      dropLoneRegion();
      return true;
    }
    if(event->type == SvgGui::OUTSIDE_MODAL) {
      gui->closeMenus();
      return false;
    }
    // don't let repeat key events close popup (happens with Ctrl key held down for sel mode on Windows)
    if(event->type == SDL_KEYDOWN && !event->key.repeat) {
      gui->closeMenus();
      dropLoneRegion();
      if(event->key.keysym.sym == SDLK_ESCAPE)  // only swallow Esc key
        return true;
    }
    return false;
  });

  selectFirst("#main-container")->addWidget(selPopup);
  buildRegionPanel();
  // set up one-time help popups
  setupHelpTips();

  // custom shortcuts - config format is "Ctrl+Shift+Z:actionRedo|Ctrl+..." - uses '|' and ':' since both of
  //  these are shifted/uppercase, so shouldn't appear in shortcut key definition
  const char* shortcutsStr = ScribbleApp::cfg->String("shortcutKeys");
  auto shortcutsSplit = splitStringRef(StringRef(shortcutsStr), "|", true);
  for(const StringRef& s : shortcutsSplit) {
    auto keyaction = splitStringRef(s, ":");
    if(keyaction.size() != 2) {
      PLATFORM_LOG("Bad shortcut definition: %s\n", s.toString().c_str());
      continue;
    }
    std::string key = keyaction[0].trimmed().toString();
    std::string action = keyaction[1].trimmed().toString();
    if(!key.empty())
      shortcuts[key] = findAction(action.c_str());
  }
}

Action* MainWindow::findAction(const char* name)
{
  if(name && name[0]) {
    for(size_t ii = 0; ii < actionList.size(); ii++) {
      if(actionList.at(ii)->name == name)
        return actionList.at(ii);
    }
  }
  return NULL;
}

void MainWindow::createToolBars()
{
  // previously, we supported multiple toolbars, but for now, we only use the first
  auto tbscfg = splitStr<std::vector>(ScribbleApp::cfg->String("toolBars2"), ';', true);
  auto tbcfg = splitStr<std::vector>(tbscfg[0].c_str(), ',', true);

  Toolbar* tb = vertToolbar ? createVertToolbar() : createToolbar();
  Widget* stretch = NULL;
  Menubar* toolsToolbar = NULL;
  // priority orders auto-adjust: the lowest is dropped from the toolbar first.  Any actions passed here
  //  get a menu item in the overflow menu, shown while the widget is hidden (see adjFn below)
  auto addTBWidget = [this](Widget* w, int priority, std::initializer_list<Action*> actions = {}) {
    w->node->setAttr<int>("ui-priority", priority);
    tbWidgets.push_back(w);
    for(Action* action : actions) {
      Button* item = createActionMenuItem(action);
      setupMenuItem(item);
      item->setVisible(false);
      overflowHiddenGroup->addWidget(item);
      tbOverflowItems[w].push_back(item);
    }
  };
  // the six tools (plus the optional pan/IAP buttons) go into the given toolbar
  auto addTools = [&](Toolbar* dest) {
#ifdef SCRIBBLE_IAP
      if(!iosIsPaid()) {
        const char* iapButtonSVG = R"(<g class="toolbutton" layout="box">
          <rect class="background" box-anchor="hfill" width="36" height="42"/>
          <text class="title" style="fill: #1F9FFF" margin="8 8">XXX</text>
        </g>)";

        iapButton = new Button(loadSVGFragment(iapButtonSVG));
        iapButton->setText(_("Upgrade Kaku"));  // for i18n
        iapButton->onClicked = [](){ iosRequestIAP(); };  //app->openURL("https://apps.apple.com/us/app/stylus-labs-write/id1498369428"); };
        dest->addWidget(iapButton);
        addTBWidget(iapButton, Action::NormalPriority - 10);
        addTBWidget(dest->addSeparator(), -100);
      }
#endif
      // Pan is always offered (added directly to the main toolbar, ahead of the tools sub-toolbar).  It
      //  used to appear only when both touch modes were off, which left a stylus-only user - who has no
      //  second finger to pan with and no middle mouse button either - with no way to move the canvas
      //  but the scroll handle.  It is not one of the tools (it edits nothing), so it keeps its own slot
      //  and its own priority: at NormalPriority+1 it is dropped into the overflow menu before the tools
      //  row is, which is what keeps it from crowding a narrow screen.
      addTBWidget(dest->addAction(actionPan), actionPan->priority, {actionPan});
      // the six tools; submodes are on the options rows below instead of in floating menus
      toolsToolbar = vertToolbar ? createVertMenubar() : createMenubar();
      toolsToolbar->autoClose = true;  //ScribbleApp::cfg->Bool("pressOpenMenus");
      toolsToolbar->addAction(actionDraw);
      toolsToolbar->addAction(actionErase);
      toolsToolbar->addAction(actionHighlight);
      toolsToolbar->addAction(actionSelect);
      toolsToolbar->addAction(actionShapes);
      toolsToolbar->addAction(actionEphemeral);
      toolsToolbar->addAction(actionInsert_Space);
      toolsToolbar->node->setAttribute("box-anchor", "");  // no stretching for this subtoolbar!
      // a sub-toolbar is not a surface of its own - the toolbar it sits in supplies the background (and,
      //  since .toolbar-bg is outlined, would otherwise draw a second outline around just the tools)
      toolsToolbar->selectFirst(".toolbar-bg")->setVisible(false);
      dest->addWidget(toolsToolbar);
      // the tools are the point of the app, so the row is hidden only as a last resort (see adjFn)
      addTBWidget(toolsToolbar, 4, {actionDraw, actionErase, actionHighlight,
          actionSelect, actionShapes, actionEphemeral, actionInsert_Space});
  };

  // page ops, file ops and overflow are separate floating panels on desktop (see below), so the flat
  //  toolbar config is only used for the vertical (tablet/narrow) toolbar
  if(vertToolbar) {
    for(size_t jj = 0; jj < tbcfg.size(); ++jj) {
      if(tbcfg[jj] == "separator") {
        addTBWidget(tb->addSeparator(), -100);
      }
      else if(tbcfg[jj] == "stretch") {
        stretch = createStretch();
        tb->addWidget(stretch);  // not included in tbWidgets
      }
      else if(tbcfg[jj] == "docTitle") {
        tb->addWidget(titleButton);
        addTBWidget(titleButton, 2);
      }
      else if(tbcfg[jj] == "addPage") {
        Widget* addPageBtn = AddPageMenu::createAddPageButton(actionScan_Page);
        tb->addWidget(addPageBtn);
        // adding a page is the one thing this button does that nothing else on a narrow toolbar does,
        //  so it outranks everything but undo/redo and the tools themselves
        addTBWidget(addPageBtn, 3);
      }
      else if(tbcfg[jj] == "undoRedoBtn") {
        tb->addWidget(undoRedoBtn);
        addTBWidget(undoRedoBtn, 5, {actionUndo, actionRedo});
      }
      else if(tbcfg[jj] == "seltools") {
        if(!ScribbleApp::cfg->Bool("popupToolbar")) {
          addTBWidget(tb->addAction(actionCut), actionCut->priority, {actionCut});
          addTBWidget(tb->addAction(actionCopy), actionCopy->priority, {actionCopy});
        }
        addTBWidget(tb->addAction(actionPaste), actionPaste->priority, {actionPaste});
      }
      else if(tbcfg[jj] == "tools") {
        addTools(tb);
      }
      else {
        Action* action = findAction(tbcfg[jj].c_str());
        if(action)
          addTBWidget(tb->addAction(action), action->priority, {action});
      }
    }
  }
  else {
    addTools(tb);  // main toolbar holds the editing tools ...
    // ... plus History, which is not a tool: it is a verb, and never shows the checked state the tools
    //  do, so the separator is what keeps it from reading as an eighth mode.  It earns its place here by
    //  being reached for about as often as a tool switch, and by being centered - the panel is dragged
    //  horizontally, and in the file ops panel at the right edge the *undo* direction had no room at all
    //  (see "History panel" in CLAUDE.md), leaving the edge auto-repeat to carry the common case.
    addTBWidget(tb->addSeparator(), -100);
    tb->addWidget(undoRedoBtn);
    addTBWidget(undoRedoBtn, 5, {actionUndo, actionRedo});
  }

  // container to hide/show toolbar items depending on width, using adjFn
  AutoAdjContainer* adjtb = new AutoAdjContainer(new SvgG(), tb);
  adjtb->node->setAttribute("box-anchor", vertToolbar ? "vfill" : "hfill");
  adjtb->node->addClass("main-toolbar-autoadj");

  // the other three floating panels of the desktop toolbar row
  Toolbar* pageopsRow = NULL, *fileopsRow = NULL, *overflowRow = NULL;
  Widget* stretch2 = NULL;

  // each panel sizes to its contents and gets a rounded floating background inset from its contents;
  //  lmargin/rmargin are floatEdgeInset on the side facing the window edge, so the row sits as far from
  //  the left and right edges as it does from the top
  auto floatBox = [](Widget* box, Dim rmargin, Dim pad, Dim lmargin = floatInset) {
    box->node->setAttribute("box-anchor", "top");  // no stretching - panel hugs its contents
    box->setMargins(floatTopInset, rmargin, floatTopInset, lmargin);
    SvgRect* bg = static_cast<SvgRect*>(box->selectFirst(".toolbar-bg")->node);
    bg->setRect(bg->getRect(), floatCorner, floatCorner);
    // breathing room between the rounded background and the first/last button
    box->selectFirst(".child-container")->setMargins(0, pad, 0, pad);
  };

  if(!vertToolbar) {
    // the tools row sets the width of the Editing panel: it reports its measured width to layout (instead
    //  of nothing, as a hfill widget normally would), so the panel is exactly as wide as the tools row;
    //  the options rows below are inside a scroll viewport and do not contribute to the panel width
    adjtb->fillReportsSize = true;
    // the tools row is a child of the Editing panel, which supplies the single background for the panel
    tb->selectFirst(".toolbar-bg")->setVisible(false);
    // #main-toolbar-container is now an overlay above the page (see res_ui.cpp), so this accent
    //  line at the top of the page would otherwise show through above the floating toolbar
    selectFirst("#scribble-focus")->setVisible(false);
    selectFirst("#scribble-focus-2")->setVisible(false);

    // page ops: doc title, sidebar, paste, split view
    pageopsRow = createToolbar();
    titleButton->node->addClass("float-wide-btn");  // sized by its label, not the button grid
    pageopsRow->addWidget(titleButton);
    addTBWidget(titleButton, 2);
    addTBWidget(pageopsRow->addSeparator(), -100);
    // The sidebar button sits at the left end of the page ops panel: the sidebar itself defaults to
    //  the left edge, so the control is on the side of what it opens.  Bookmarks is off the toolbar
    //  for now; Ctrl+B still toggles the panel.
    addTBWidget(pageopsRow->addAction(actionShow_Sidebar), actionShow_Sidebar->priority,
        {actionShow_Sidebar});
    addTBWidget(pageopsRow->addAction(actionPaste), actionPaste->priority, {actionPaste});
    addTBWidget(pageopsRow->addAction(actionSplitView), actionSplitView->priority, {actionSplitView});
    floatBox(pageopsRow, floatInset, floatSidePad, floatEdgeInset);
    scaleFloatPanel(pageopsRow, floatSideIconSize);

    // file ops: add page, save, undo/redo (the mockup's order)
    fileopsRow = createToolbar();
    // the universal "add page" button - see addpagemenu.cpp for why the ruling is chosen here
    Widget* addPageBtn = AddPageMenu::createAddPageButton(actionScan_Page);
    fileopsRow->addWidget(addPageBtn);
    addTBWidget(addPageBtn, 3);
    addTBWidget(fileopsRow->addAction(actionSave), actionSave->priority, {actionSave});
    // History now lives at the end of the tools row instead (see addTools above)
    // the overflow panel hugs the file ops panel rather than being pushed to the window edge
    floatBox(fileopsRow, 18*floatUIScale, floatSidePad);
    scaleFloatPanel(fileopsRow, floatSideIconSize);

    // overflow menu sits alone in its own small panel at the right edge
    overflowRow = createToolbar();
    overflowRow->addAction(actionOverflow_Menu);  // never hidden: it holds everything that was
    floatBox(overflowRow, floatEdgeInset, 0);
    scaleFloatPanel(overflowRow, floatSideIconSize);

    stretch = createStretch();
    stretch2 = createStretch();
  }

  // Every widget hidden here can still be reached from the overflow menu (see addTBWidget), which is why
  //  the overflow button itself is the one thing auto-adjust never touches.
  // tbWidgets is sorted by priority once the options rows have been added too, below

  // the options rows are children of the same panel as the tools row, so they must not draw their own
  //  background; the panel's single rounded background rect shows through instead
  auto floatRow = [this](Widget* rowtb) {
    if(vertToolbar) return;
    rowtb->selectFirst(".toolbar-bg")->setVisible(false);
  };

  // help and settings get the mockup's narrower cell (see scaleFloatPanel)
  auto smallFloatBtn = [this](Button* btn) {
    if(!vertToolbar)
      btn->node->addClass("float-small-btn");
    return btn;
  };

  // the eraser size presets at the right of the eraser options row
  auto addEraserWidths = [this](Toolbar* row) {
    eraserWidthBtns.clear();
    for(int ii = 0; ii < int(NELEM(ERASER_RADII)); ++ii) {
      Button* btn = new Button(loadSVGFragment(eraserWidthBtnSVG().c_str()));
      btn->containerNode()->selectFirst(".width-line")->setAttr("stroke-width", ERASER_RADII[ii]/2*floatUIScale);
      btn->onClicked = [this, ii](){ setEraserWidth(ii); };
      setupTooltip(btn, fstring(_("Eraser size: %.0f"), 2*ERASER_RADII[ii]).c_str());
      row->addWidget(btn);
      eraserWidthBtns.push_back(btn);
    }
    setEraserWidth(ScribbleApp::cfg->Int("eraserWidth"));
  };

  penToolbarAutoAdj = createPenToolbarAutoAdj(!vertToolbar);
  if(!vertToolbar) {
    // every options row is narrower than the six-tool row above it, so nothing ever has to collapse
    //  (which is what the pen toolbar's adjFn does); the row is just centred under the tools
    penToolbarAutoAdj->adjFn = [](const Rect&, const Rect&){};
  }
  floatRow(penToolbarAutoAdj->contents);

  Toolbar* eraseRow = createToolbar();
  eraseOptsRow = eraseRow;
  eraseRow->addWidget(createStretch());
  eraseStrokeToggle = createToolbutton(
      SvgGui::useFile(":/icons/ic_menu_toggle_erase_stroke.svg"), _("Erase Whole Strokes"));
  eraseStrokeToggle->onClicked = [this](){
    eraseStrokeToggle->setChecked(!eraseStrokeToggle->isChecked());
    setEraserMode();
  };
  setupTooltip(eraseStrokeToggle, _("Erase whole strokes"));
  eraseRow->addWidget(eraseStrokeToggle);
  eraseRuledToggle = createToolbutton(SvgGui::useFile(":/icons/ic_menu_toggle_ruled.svg"), _("Ruled Eraser"));
  eraseRuledToggle->onClicked = [this](){
    eraseRuledToggle->setChecked(!eraseRuledToggle->isChecked());
    setEraserMode();
  };
  setupTooltip(eraseRuledToggle, _("Erase along ruling"));
  eraseRow->addWidget(eraseRuledToggle);
  eraseSwitchBackToggle = createToolbutton(
      SvgGui::useFile(":/icons/ic_menu_switch_back.svg"), _("Switch Back"));
  eraseSwitchBackToggle->setChecked(app->scribbleMode->eraseSwitchBack);
  eraseSwitchBackToggle->onClicked = [this](){
    app->scribbleMode->setSwitchBack(MODE_ERASE, !eraseSwitchBackToggle->isChecked());
    updateMode();  // the tools row shows the "once" state of the active tool
  };
  setupTooltip(eraseSwitchBackToggle, _("Return to previous tool after erasing"));
  eraseRow->addWidget(eraseSwitchBackToggle);
  eraseRow->addWidget(smallFloatBtn(createHelpButton({
    {"ic_menu_toggle_erase_stroke.svg", "Erase Whole Strokes", "Removes an entire stroke instead of just the part you touch."},
    {"ic_menu_toggle_ruled.svg", "Ruled Eraser", "Constrains erasing to the ruled lines; use in the margin to erase whole lines."},
    {"ic_menu_switch_back.svg", "Switch Back", "Returns to the previous tool after one erase."},
    {"ic_menu_settings2.svg", "Eraser Settings", "Preferences that affect erasing."} })));
  eraseRow->addSeparator();
  addEraserWidths(eraseRow);
  eraseRow->addWidget(createStretch());
  // settings sits at the very end of the row, past the stretch that centres the tools
  eraseRow->addWidget(smallFloatBtn(createToolSettingsButton(
      "Eraser Settings", {"eraseOnImage", "doubleTapSticky"})));
  floatRow(eraseRow);

  Toolbar* selectRow = createToolbar();
  selectOptsRow = selectRow;
  selectRow->addWidget(createStretch());
  selectRow->addAction(actionLasso_Select);
  selectRow->addAction(actionRect_Select);
  selectRow->addAction(actionRuled_Select);
  selectRow->addAction(actionPath_Select);
  selectSwitchBackToggle = createToolbutton(
      SvgGui::useFile(":/icons/ic_menu_switch_back.svg"), _("Switch Back"));
  selectSwitchBackToggle->setChecked(app->scribbleMode->selectSwitchBack);
  selectSwitchBackToggle->onClicked = [this](){
    app->scribbleMode->setSwitchBack(MODE_SELECT, !selectSwitchBackToggle->isChecked());
    updateMode();
  };
  setupTooltip(selectSwitchBackToggle, _("Return to previous tool after selecting"));
  selectRow->addWidget(selectSwitchBackToggle);
  selectRow->addWidget(smallFloatBtn(createHelpButton({
    {"ic_menu_select_lasso.svg", "Lasso Select", "Selects everything inside a freehand path."},
    {"ic_menu_select.svg", "Rect Select", "Selects everything inside a rectangle you drag."},
    {"ic_menu_select_ruled.svg", "Ruled Select", "Selects handwritten text; use in the margin to select whole lines."},
    {"ic_menu_select_path.svg", "Path Select", "Selects the strokes crossed by a freehand path."},
    {"ic_menu_switch_back.svg", "Switch Back", "Returns to the previous tool after one selection."} })));
  selectRow->addWidget(createStretch());
  selectRow->addWidget(smallFloatBtn(createToolSettingsButton("Selection Settings",
      {"popupToolbar", "applyPenToSel", "columnDetectMode", "doubleTapSticky"})));
  floatRow(selectRow);

  Toolbar* shapeRow = createToolbar();
  shapeOptsRow = shapeRow;
  shapeRow->addWidget(createStretch());
  for(int ii = 0; ii < SHAPE_COUNT; ++ii)
    shapeRow->addAction(actionShape[ii]);
  shapeRow->addAction(actionRulingRegion);
  shapeRow->addSeparator();
  // One swatch, not the draw row's saved list: that would make this row too wide.  Shapes are drawn
  //  with the current draw pen, so the swatch shows and sets that pen's color.  With a shape selected
  //  the pen toolbar is in SELECTION_MODE and the pick recolors it - so the pen is set here as well,
  //  just as the toggles below arm the next shape as well as editing the selected one.
  PenToolbar* penToolbar = static_cast<PenToolbar*>(penToolbarAutoAdj->contents);
  shapeRow->addWidget(penToolbar->createSingleSwatch());
  penToolbar->onSingleSwatchPicked = [this, penToolbar](Color color){
    if(penToolbar->mode == PenToolbar::PEN_MODE)
      return;  // updateColor() has already set the pen
    ScribblePen pen = *app->getPen();
    int alpha = pen.color.alpha();  // keep the marker translucent
    pen.color = color;
    pen.color.setAlpha(alpha);
    app->setPen(pen);
  };
  shapeHeadStartToggle = createToolbutton(
      SvgGui::useFile(":/icons/ic_menu_shape_head_start.svg"), _("Start Arrowhead"));
  shapeHeadStartToggle->onClicked = [this](){
    shapeHeadStartToggle->setChecked(!shapeHeadStartToggle->isChecked());
    setShapeOptions();
  };
  setupTooltip(shapeHeadStartToggle, _("Arrowhead at the start"));
  shapeRow->addWidget(shapeHeadStartToggle);
  shapeHeadEndToggle = createToolbutton(
      SvgGui::useFile(":/icons/ic_menu_shape_head_end.svg"), _("End Arrowhead"));
  shapeHeadEndToggle->onClicked = [this](){
    shapeHeadEndToggle->setChecked(!shapeHeadEndToggle->isChecked());
    setShapeOptions();
  };
  setupTooltip(shapeHeadEndToggle, _("Arrowhead at the end"));
  shapeRow->addWidget(shapeHeadEndToggle);
  shapeRoundedToggle = createToolbutton(
      SvgGui::useFile(":/icons/ic_menu_shape_rounded.svg"), _("Rounded Corners"));
  shapeRoundedToggle->onClicked = [this](){
    shapeRoundedToggle->setChecked(!shapeRoundedToggle->isChecked());
    setShapeOptions();
  };
  setupTooltip(shapeRoundedToggle, _("Round off the corners"));
  shapeRow->addWidget(shapeRoundedToggle);
  shapeRow->addWidget(smallFloatBtn(createHelpButton({
    {"ic_menu_shape_line.svg", "Line", "Drag to draw a straight line."},
    {"ic_menu_shape_box.svg", "Box", "Drag to draw a rectangle."},
    {"ic_menu_shape_ellipse.svg", "Ellipse", "Drag to draw an ellipse."},
    {"ic_menu_shape_polyline.svg", "Polyline", "Tap to place points; tap the last point to finish, the first to close."},
    {"ic_menu_shape_fitpoly.svg", "Curve", "A smooth curve through your points; drag the red handle to trade smoothness for following them exactly."},
    {"ic_menu_toggle_ruled.svg", "Ruling Region", "Drag a box that has its own lines. Writing inside it follows them. Select it with the ... button in its corner."},
    {"ic_menu_add_color.svg", "Color", "The pen's color, which shapes are drawn in. Recolors the selected shape if there is one."},
    {"ic_menu_shape_head_start.svg", "Start Arrowhead", "Puts an arrowhead on the start. Edits the selected shape if there is one."},
    {"ic_menu_shape_head_end.svg", "End Arrowhead", "Puts an arrowhead on the end. A line with an end arrowhead is an arrow."},
    {"ic_menu_shape_rounded.svg", "Rounded Corners", "Rounds the corners of a box or polyline; drag the red handle to set the radius."} })));
  shapeRow->addWidget(createStretch());
  shapeRow->addWidget(smallFloatBtn(createToolSettingsButton("Shape Settings",
      {"shapeCornerRadius", "shapeCurveTightness", "shapeEditAfterDraw", "shapeSnapDelay", "shapeAngleSnap", "doubleTapSticky"})));
  floatRow(shapeRow);

  Toolbar* insSpaceRow = createToolbar();
  insSpaceOptsRow = insSpaceRow;
  insSpaceRow->addWidget(createStretch());
  insSpaceRow->addAction(actionInsert_Space_Vert);
  insSpaceRow->addAction(actionRuled_Insert_Space);
  insSpaceSwitchBackToggle = createToolbutton(
      SvgGui::useFile(":/icons/ic_menu_switch_back.svg"), _("Switch Back"));
  insSpaceSwitchBackToggle->setChecked(app->scribbleMode->insSpaceSwitchBack);
  insSpaceSwitchBackToggle->onClicked = [this](){
    app->scribbleMode->setSwitchBack(MODE_INSSPACE, !insSpaceSwitchBackToggle->isChecked());
    updateMode();
  };
  setupTooltip(insSpaceSwitchBackToggle, _("Return to previous tool after inserting space"));
  insSpaceRow->addWidget(insSpaceSwitchBackToggle);
  insSpaceRow->addWidget(smallFloatBtn(createHelpButton({
    {"ic_menu_insert_space.svg", "Insert Space", "Drags everything below the line you draw up or down."},
    {"ic_menu_insert_space_ruled.svg", "Ruled Insert Space", "Inserts whole lines and reflows handwritten text."},
    {"ic_menu_switch_back.svg", "Switch Back", "Returns to the previous tool after inserting space once."} })));
  insSpaceRow->addWidget(createStretch());
  insSpaceRow->addWidget(smallFloatBtn(createToolSettingsButton("Insert Space Settings",
      {"reflow", "insSpaceErase", "minWordSep", "columnDetectMode", "blankYRuling", "doubleTapSticky"})));
  floatRow(insSpaceRow);

  // thin divider between the tools row and the open options row (both are one panel)
  optsRowDivider = createHRule(1, NULL, "separator");
  if(!vertToolbar)
    optsRowDivider->setMargins(0, optsDividerInset);  // the mockup insets it well inside the panel

  Widget* toolbarColumn = NULL;
  if(vertToolbar) {
    toolbarColumn = createColumn(
        {adjtb, optsRowDivider, penToolbarAutoAdj, eraseRow, shapeRow, selectRow, insSpaceRow},
        "", "", "vfill");
  }
  else {
    // Only one options row is visible at a time; they are stacked directly (hfill, like the tools row)
    //  so the options row shares the same width as the panel. A horizontally-scrolling viewport was
    //  tried here so the panel could stay locked to the tools row's width, but ScrollWidget is built
    //  for vertical scrolling and left visible layout artifacts when repurposed - simple stacking with
    //  the (now-compact) options content is a safer trade for now.
    Widget* optsStack = createColumn(
        {penToolbarAutoAdj, eraseRow, shapeRow, selectRow, insSpaceRow}, "", "", "hfill");
    optsRowContainer = optsStack;
    // every options row should fit the mockup's six-tool width, but the palettes are user-configurable,
    //  so let a row that has grown past it widen the panel rather than spill outside the background
    for(Widget* w : {(Widget*)penToolbarAutoAdj, (Widget*)eraseRow, (Widget*)shapeRow,
        (Widget*)selectRow, (Widget*)insSpaceRow, optsStack})
      w->fillReportsSize = true;

    // one panel, one background: the tools row, the divider and the options rows are all children of
    //  this toolbar, so its background rect (rounded on all four corners, always) wraps all of them
    Toolbar* panel = createToolbar();
    Widget* panelContents = panel->selectFirst(".child-container");
    panelContents->node->setAttribute("flex-direction", "column");
    panel->addWidget(adjtb);
    panel->addWidget(optsRowDivider);
    panel->addWidget(optsStack);
    floatBox(panel, floatInset, floatPad);
    mainToolbarPanel = panel;
    toolbarColumn = panel;
    // an options row is as wide as the tools row, so on a narrow window it has to give way just before
    //  the tools do - otherwise the tool options would be all that is left (with no way to switch tool)
    addTBWidget(optsRowDivider, 3);
    addTBWidget(optsStack, 3);
  }

  std::stable_sort(tbWidgets.begin(), tbWidgets.end(), [](Widget* a, Widget* b){
    return a->node->getIntAttr("ui-priority", 0) < b->node->getIntAttr("ui-priority", 0);
  });

  // container to hide/show toolbar items depending on width, using adjFn; on desktop, this wraps the
  //  whole row of floating panels, since it is the row (not any single panel) that runs out of space
  AutoAdjContainer* adjOuter = adjtb;
  if(!vertToolbar) {
    // the four floating panels, pushed apart by flexible gaps (overflow hugs the file ops panel)
    Widget* toolbarRow = createRow(
        {pageopsRow, stretch, toolbarColumn, stretch2, fileopsRow, overflowRow}, "", "", "top hfill");
    adjOuter = new AutoAdjContainer(new SvgG(), toolbarRow);
    adjOuter->node->setAttribute("box-anchor", "top hfill");
    adjtb->adjFn = [](const Rect&, const Rect&){};  // adjustment is done by adjOuter for the whole row
    // the floating panels use bigger buttons than the rest of the app (see scaleFloatToolbutton);
    //  the side panels were already scaled above, with their own (larger) icon size
    scaleFloatPanel(toolbarColumn, floatIconSize);
    // History gets a larger box than the tools beside it, to end up looking the same size: the tool
    //  glyphs are strokes running corner to corner of their viewBox, while a clock face is a circle
    //  inscribed in it, so at equal box sizes the circle reads smaller (measured ink: 10px vs 11-12px)
    scaleFloatToolbutton(undoRedoBtn, floatBtnSize, 40*floatUIScale);
    // The row overlays the canvas and the pinned sidebar, and a container is hit anywhere inside its
    //  bounding box - which here is the full window width (the stretches) by the height of the tallest
    //  panel.  With a tool's options row open that band is twice as deep, and it swallowed presses on
    //  the top of the pinned sidebar (its view selector responded only on its bottom part) and on the
    //  page between the panels.  Only the panels themselves should take presses.
    for(Widget* w : {selectFirst("#main-toolbar-container"), (Widget*)adjOuter, toolbarRow, stretch, stretch2})
      w->hitTransparent = true;
    selectFirst("#main-toolbar-container")->addWidget(adjOuter);
  }
  else
    selectFirst("#main-toolbar-container")->addWidget(toolbarColumn);

  adjOuter->adjFn = [=](const Rect& src, const Rect& dest) mutable {
    // Width still needed with the current visibility: laying the contents out at zero size squeezes every
    //  stretch, so what is left is the unsqueezable minimum.  We can't measure the leftover space from the
    //  stretch widgets instead: a stretch squeezed to zero width can never be scaled back up (see
    //  Widget::setLayoutBounds), so on the very first layout - which includes a zero-size measuring pass -
    //  they report zero slack however much room there is, which used to collapse the whole toolbar until
    //  the next resize.
    auto widthNeeded = [adjOuter]() {
      adjOuter->repeatLayout(Rect::wh(0, 0));
      return adjOuter->contents->node->bounds().width();
    };

    // reset: everything back on the toolbar, nothing in the overflow menu's hidden-items group
    titleButton->setShowTitle(!vertToolbar);
    titleButton->setText(titleStr.c_str());
    for(Widget* w : tbWidgets) {
      // the options row and its divider are shown only while a tool's options are open (showOptionsRow())
      w->setVisible(w == optsRowContainer || w == optsRowDivider ? openOptionsRow != 0 : true);
      for(Button* item : tbOverflowItems[w])
        item->setVisible(false);
    }
    overflowHiddenGroup->setVisible(false);
    overflowHiddenSep->setVisible(false);

    // drop widgets in priority order (lowest first) until the toolbar fits; the doc title gives up its
    //  text before the button itself goes, and the tools row is hidden last (see addTBWidget calls)
    bool anyHidden = false;
    for(size_t ii = 0; ii < tbWidgets.size() && widthNeeded() > dest.width(); ) {
      Widget* w = tbWidgets[ii];
      if(w == titleButton && titleButton->selectFirst(".title")->isVisible()) {
        titleButton->setShowTitle(false);
        continue;  // retry this widget: hide the button itself only if dropping the text wasn't enough
      }
      w->setVisible(false);
      for(Button* item : tbOverflowItems[w]) {
        item->setVisible(true);
        anyHidden = true;
      }
      ++ii;
    }
    overflowHiddenGroup->setVisible(anyHidden);
    overflowHiddenSep->setVisible(anyHidden);

    // put back as much of the doc title as the leftover space allows
    if(titleButton->isVisible() && !titleButton->selectFirst(".title")->isVisible()) {
      Dim slack = dest.width() - widthNeeded();
      SvgText* textnode = static_cast<SvgText*>(titleButton->containerNode()->selectFirst("text"));
      if(slack > 12 && textnode) {
        titleButton->setShowTitle(true);
        SvgPainter::elideText(textnode, slack - 4);
      }
    }
    adjOuter->repeatLayout(dest);
  };

  // the overflow button opens an ArrowPopup (created in setupActions()) instead of a Menu, so it can't be
  //  wired up by Action::setMenu(); the popup must be a child of the button to be positioned relative to it
  if(!actionOverflow_Menu->buttons.empty()) {
    Button* overflowBtn = actionOverflow_Menu->buttons.front();
    overflowBtn->addWidget(overflowPopup);
    // open on press, like a menu; added after the Button handler, so it runs first and swallows the event
    overflowBtn->addHandler([this, overflowBtn](SvgGui* gui, SDL_Event* event){
      if(event->type != SDL_FINGERDOWN || event->tfinger.fingerId != SDL_BUTTON_LMASK)
        return false;
      // the press that closed the popup (as an outside press) must not immediately reopen it
      if(gui->lastClosedMenu != overflowPopup) {
        gui->closeMenus();
        openAutoClosePopup(overflowPopup);
        // note we must NOT setPressed() the popup: the release over this button is not over the popup, so
        //  it would arrive as an outside press and close the popup as soon as the button is released
        overflowBtn->node->addClass("pressed");  // cleared by closeMenus(), which unpresses popup's parent
      }
      return true;
    });
  }

  showOptionsRow(ScribbleApp::cfg->Bool("showPenToolbar") ? MODE_STROKE : 0);
}

// Keep the toolbar's sidebar button in step with the sidebar: its glyph names the edge the sidebar
// is docked to (so the left/right variants are not decorative - they say where it will appear), and
// its checked state says whether it is open.  Called from Sidebar::setOpen and Sidebar::setOnLeft,
// and once after the sidebar is constructed, since the action is created before it.
void MainWindow::updateSidebarButton()
{
  if(!actionShow_Sidebar || !sidebar)
    return;
  actionShow_Sidebar->setIcon(SvgGui::useFile(
      sidebar->isOnLeft() ? ":/icons/ic_menu_sidebar_left.svg" : ":/icons/ic_menu_sidebar_right.svg"));
  actionShow_Sidebar->setChecked(sidebar->isOpen());
}

void MainWindow::setupActions()
{
  ScribbleConfig* cfg = app->cfg;
  setupTheme();

  // simple command actions
  // an alternative for connecting multiple signals to one slot is the QSignalMapper approach
  action_Previous_Page = createAction("action_Previous_Page",
      "&Previous Page", ":/icons/ic_menu_prev.svg", "Left", SLOT(doCommand(ID_PREVPAGE)));
  action_Next_Page = createAction("action_Next_Page",
      "&Next Page", ":/icons/ic_menu_next.svg", "Ctrl+Right", SLOT(doCommand(ID_NEXTPAGENEW)));

  // keyboard only actions
  createAction("action_Page_Down", "Next Page", "", "Right", SLOT(doCommand(ID_NEXTPAGE)));
  createAction("action_Prev_Screen", "Previous Screen", "", "PageUp", SLOT(doCommand(ID_PREVSCREEN)));
  createAction("action_Next_Screen", "Next Screen", "", "PageDown", SLOT(doCommand(ID_NEXTSCREEN)));
  createAction("action_Scroll_Up", "Scroll Up", "", "Up", SLOT(doCommand(ID_SCROLLUP)));
  createAction("action_Scroll_Down", "Scroll Down", "", "Down", SLOT(doCommand(ID_SCROLLDOWN)));
  // should we just use Home/End?
  createAction("action_JumpBeginning", "Beginning of Document", "", "Ctrl+Home", SLOT(doCommand(ID_STARTOFDOC)));
  createAction("action_JumpEnd", "End of Document", "", "Ctrl+End", SLOT(doCommand(ID_ENDOFDOC)));

  actionZoom_In = createAction("actionZoom_In", "Zoom &In", "", "Ctrl+=", SLOT(doCommand(ID_ZOOMIN)));
  actionZoom_Out = createAction("actionZoom_Out", "Zoom &Out", "", "Ctrl+-", SLOT(doCommand(ID_ZOOMOUT)));
  actionReset_Zoom = createAction("actionReset_Zoom", "&Reset Zoom", "", "Ctrl+0", SLOT(doCommand(ID_RESETZOOM)));
  //actionZoom_All = createAction("actionZoom_All", "Zoom All", "", "", SLOT(doCommand(ID_ZOOMALL)));
  //actionZoom_Width = createAction("actionZoom_Width", "Zoom Width", "", "", SLOT(doCommand(ID_ZOOMWIDTH)));

  actionPrevious_View = createAction("actionPrevious_View",
      "&Previous View", ":/icons/ic_menu_back.svg", "Backspace", SLOT(doCommand(ID_PREVVIEW)));
  actionNext_View = createAction("actionNext_View",
      "&Next View", ":/icons/ic_menu_forward.svg", "Shift+Backspace", SLOT(doCommand(ID_NEXTVIEW)));

  // note that any checkable menu icon should have an icon to suppress Qt's (non-dpi-indep.) checkmark
  actionFullscreen = createAction("actionFullscreen",
      "&Fullscreen", ":/icons/ic_menu_fullscreen.svg", "F11", [this](){ toggleFullscreen(); });
  actionFullscreen->setCheckable(true);

  // setup split view menu
  actionSplitView = createAction("actionSplitView",
      "&Split View", ":/icons/ic_menu_split_tb.svg", "Ctrl+T", [this](){ toggleSplitView(SPLIT_TOGGLE); });
  actionSplitView->setCheckable(true);
  actionSplitView->setPriority(Action::NormalPriority - 2);  // below bookmarks
  splitState = -std::min(std::max(1, std::abs(ScribbleApp::cfg->Int("splitLayout"))), int(SPLIT_V21));
  toggleSplitView(splitState);  // set icon for actionSplitView

  Action* actionSplitH12 = createAction("actionSplitH12", "Split T/B", ":/icons/ic_menu_split_tb.svg", "",
      [this](){ toggleSplitView(SPLIT_H12); });
  Action* actionSplitH21 = createAction("actionSplitH21", "Split B/T", ":/icons/ic_menu_split_bt.svg", "",
      [this](){ toggleSplitView(SPLIT_H21); });
  Action* actionSplitV12 = createAction("actionSplitV12", "Split L/R", ":/icons/ic_menu_split_lr.svg", "",
      [this](){ toggleSplitView(SPLIT_V12); });
  Action* actionSplitV21 = createAction("actionSplitV21", "Split R/L", ":/icons/ic_menu_split_rl.svg", "",
      [this](){ toggleSplitView(SPLIT_V21); });
  actionSplitView->addMenuAction(actionSplitH12);
  actionSplitView->addMenuAction(actionSplitH21);
  actionSplitView->addMenuAction(actionSplitV12);
  actionSplitView->addMenuAction(actionSplitV21);

  actionInvertColors = createAction("actionInvertColors",
      "Invert Colors", "", "", [this](){ toggleInvertColors(); });
  actionInvertColors->setCheckable(true);
  actionInvertColors->setChecked(ScribbleApp::cfg->Bool("invertColors"));

  actionNew_Page_Before = createAction("actionNew_Page_Before",
      "New Page &Before", "", "Ctrl+Shift+Left", SLOT(doCommand(ID_PAGEBEFORE)));
  actionNew_Page_After = createAction("actionNew_Page_After",
      "New Page &After", "", "Ctrl+Shift+Right", SLOT(doCommand(ID_PAGEAFTER)));
  actionExpand_Down = createAction("actionExpand_Down",
      "Expand Down", ":/icons/ic_menu_expanddown.svg", "", SLOT(doCommand(ID_EXPANDDOWN)));
  actionExpand_Right = createAction("actionExpand_Right",
      "Expand Right", ":/icons/ic_menu_expandright.svg", "", SLOT(doCommand(ID_EXPANDRIGHT)));
  actionSelect_Pages = createAction("actionSelect_Pages", "Select Pages", ":/icons/ic_menu_pagesel.svg", "Ctrl+Shift+P",
      SLOT(setMode(app->scribbleMode->getMode() == MODE_PAGESEL ? MODE_STROKE : MODE_PAGESEL);));

  actionUndo = createAction("actionUndo", "&Undo", ":/icons/ic_menu_undo.svg", "Ctrl+Z", SLOT(doCommand(ID_UNDO)));
  actionRedo = createAction("actionRedo", "&Redo", ":/icons/ic_menu_redo.svg", "Ctrl+Y", SLOT(doCommand(ID_REDO)));

  actionCut = createAction("actionCut", "Cu&t", ":/icons/ic_menu_cut.svg", "Ctrl+X", SLOT(doCommand(ID_CUTSEL)));
  actionCopy = createAction("actionCopy", "&Copy", ":/icons/ic_menu_copy.svg", "Ctrl+C", SLOT(doCommand(ID_COPYSEL)));
  actionPaste = createAction("actionPaste", "&Paste", ":/icons/ic_menu_paste.svg", "Ctrl+V", SLOT(pasteClipboard()));  //doCommand(ID_PASTE)));
  actionDupSel = createAction("actionDupSel", "Duplicate", ":/icons/ic_menu_duplicate.svg", "Ctrl+D", SLOT(doCommand(ID_DUPSEL)));

  actionSelect_All = createAction("actionSelect_All", "Select &All", "", "Ctrl+A", SLOT(doCommand(ID_SELALL)));
  actionSelect_Similar = createAction("actionSelect_Similar", "Select Similar", "", "", SLOT(doCommand(ID_SELSIMILAR)));
  actionInvert_Selection = createAction("actionInvert_Selection", "Invert", "", "", SLOT(doCommand(ID_INVSEL)));
  actionDelete_Selection = createAction("actionDelete_Selection",
       "Delete", ":/icons/ic_menu_discard.svg", "Delete", SLOT(doCommand(ID_DELSEL)));
  actionCreate_Link = createAction("actionCreate_Link", "Create Link...", ":/icons/ic_menu_link.svg", "Ctrl+L", SLOT(createLink()));
  actionScreenshot = createAction("actionScreenshot", "Screenshot", ":/icons/ic_menu_screenshot.svg", "",
      [this](){ if(app->activeArea()) app->activeArea()->screenshotSelection(); });
  // icon: something like https://www.brandeps.com/icon/U/Ungroup-01 but with a solid instead of dashed line
  actionUngroup = createAction("actionUngroup", "Ungroup", "", "", SLOT(doCommand(ID_UNGROUP)));

  // more
  actionNew_Document = createAction("actionNew_Document",
      "&New", ":/icons/ic_menu_document.svg", "Ctrl+N", SLOT(newDocument()));
  action_Open = createAction("action_Open", "&Open...", ":/icons/ic_menu_folder.svg", "Ctrl+O", SLOT(openDocument()));
  actionSave = createAction("actionSave", "&Save", ":/icons/ic_menu_save.svg", "Ctrl+S", SLOT(saveDocument()));
  actionSave->setPriority(Action::NormalPriority - 2);
  actionSave_As = createAction("actionSave_As", "Save &As...", "", "Ctrl+Shift+S", SLOT(doSaveAs()));
  //actionSave_Single_File = createAction("actionSave_Single_File", "Save As Single File...", "", "", SLOT(saveSingleFile()));
  actionRevert = createAction("actionRevert", "Discard Changes...", ":/icons/ic_menu_no_save.svg", "Ctrl+W", SLOT(revert()));

  // changed from "Show Bookmarks" to "Bookmarks" because it was the longest menu text - if it overflows to
  //  menu (text isn't shown otherwise), that means we're on a narrow screen and need to conserve horz. space!
  actionShow_Bookmarks = createAction("actionShow_Bookmarks",
      "Bookmarks", ":/icons/ic_menu_bookmark.svg", "Ctrl+B", [this](){ toggleBookmarks(); });
  actionShow_Bookmarks->setCheckable(true);
  // demote slightly so next page/prev page have higher priority
  actionShow_Bookmarks->setPriority(Action::NormalPriority - 1);
  actionPage_Setup = createAction("actionPage_Setup",
      "Page Setup...", ":/icons/ic_menu_document.svg", "", SLOT(showPageSetup()));
  // ic_menu_add_color is the closest thing already in scribbleres/icons; a dedicated icon would have to
  //  come from the reicon pipeline (see CLAUDE.md), not be hand-drawn here
  actionTheme = createAction("actionTheme",
      "Theme...", ":/icons/ic_menu_add_color.svg", "", SLOT(showThemePicker()));
  // the sidebar is a panel, not a document command, so it is toggled here rather than through
  //  ScribbleApp's command dispatch
  actionShow_Sidebar = createAction("actionShow_Sidebar",
      "Sidebar", ":/icons/ic_menu_sidebar_left.svg", "", [this](){ sidebar->toggleOpen(); });
  // the glyph names the edge the sidebar is docked to, so it is swapped whenever that changes
  //  (updateSidebarButton); checkable so the toolbar button reads as open/closed
  actionShow_Sidebar->setCheckable(true);
  // With the tag browser, documents live in the library, so the folder browser is no longer a way to
  //  browse; it survives only as the file picker behind Import, which copies what it opens into the
  //  library. Without it, the tag browser stays reachable from here.
  if(ScribbleApp::cfg->Bool("useTagDocList"))
    actionTagDocList = createAction("actionTagDocList",
        "Import Document...", ":/icons/ic_menu_folder.svg", "", SLOT(importDocument()));
  else
    actionTagDocList = createAction("actionTagDocList",
        "Tag Document Browser...", ":/icons/ic_tag.svg", "", SLOT(execTagDocList()));
  actionInsert_Image = createAction("actionInsert_Image",
      "Insert Image...", ":/icons/ic_menu_add_pic.svg", "", SLOT(insertImage()));
  // two entries rather than one plus a choice in the dialog: the destination is decided before the
  // photo is taken, and it keeps the scan dialog to adjusting the scan itself
  actionScan_Element = createAction("actionScan_Element",
      "Scan Document...", ":/icons/ic_menu_add_pic.svg", "", SLOT(scanDocument(false)));
  actionScan_Page = createAction("actionScan_Page",
      "Scan Document as Page...", ":/icons/ic_menu_append_page.svg", "", SLOT(scanDocument(true)));
  // insert pages from another document
  actionInsertDocument = createAction("actionInsertDocument", "Insert Document...", "", "", SLOT(insertDocument()));

  actionBookmarksClose = createAction("actionBookmarksClose",
      "Close", ":/icons/ic_menu_cancel.svg", "", [this](){ toggleBookmarks(); });
  auto tgBkmkPin = [this](){
    bool ah = !app->cfg->Bool("autoHideBookmarks");
    app->cfg->set("autoHideBookmarks", ah);
    actionBookmarksPin->setChecked(!ah);
  };
  actionBookmarksPin = createAction("actionBookmarksPin", "Keep Open", ":/icons/ic_menu_pin.svg", "", tgBkmkPin);
  actionBookmarksPin->setChecked(!cfg->Bool("autoHideBookmarks"));

  actionExport_PDF = createAction("actionExport_PDF", "Export PDF...", "", "", SLOT(exportPDF()));
  actionImport_PDF = createAction("actionImport_PDF", "Import PDF...", "", "", SLOT(importPDF()));
  // don't use direct connection for prefs because it might destroy the toolbars (which might contain menu
  //  from which it was launched)
  actionPreferences = createAction("actionPreferences",
      "Preferences...", ":/icons/ic_menu_settings.svg", "Ctrl+P", SLOT(openPreferences()));  //ic_menu_preferences

  //actionViewHelp = createAction("actionViewHelp", "Open Help Document", "", "F1", SLOT(openHelpDoc()));
  actionUpdateCheck = createAction("actionUpdateCheck", "Check for Update", "", "", SLOT(updateCheck()));
  action_About = createAction("action_About", "&About", "", "", SLOT(about()));
  actionExit = createAction("actionExit", "E&xit", "", "Ctrl+Q", SLOT(maybeQuit()));

  // disable touch - not available by default but can be added to toolbar
  actionDisable_Touch = createAction("actionDisable_Touch", "&Disable Touch",
      ":/icons/ic_menu_pan.svg", "", [this](){ toggleDisableTouch(); });
  actionDisable_Touch->setCheckable(true);

  // sync/SWB
  actionShare_Document = createAction("actionShare_Document",
      "Create Whiteboard...", ":/icons/ic_menu_add_people.svg", "", SLOT(shareDocument()));
  actionOpen_Shared_Doc = createAction("actionOpen_Shared_Doc",
      "Open Whiteboard...", ":/icons/ic_menu_people.svg", "", SLOT(openSharedDoc()));
  actionSendImmed = createAction("actionSendImmed", "Sync Edits", "", "", SLOT(syncSendImmed()));  // ":/icons/ic_menu_send_now.svg"
  actionSendImmed->setCheckable(true);
  actionSendImmed->setChecked(true);

  // shared whiteboard view sync actions
  //menuViewSync = createMenu("menuViewSync", "View Sync");
  actionViewSync = createAction("actionViewSync", "Sync View", "", "", [this](){ toggleSyncView(); });
  actionViewSync->setCheckable(true);
  //actionViewSync->setMenu(menuViewSync);
  actionViewSyncMaster = createAction("actionViewSyncMaster",
      "View Master", "", "", [this](){ toggleSyncViewMaster(); });
  actionViewSyncMaster->setCheckable(true);
  //menuViewSync->addAction(actionViewSyncMaster);
  actionSyncInfo = createAction("actionSyncInfo", "Whiteboard Info...", "", "", SLOT(showSyncInfo()));

  // email/share document
  actionSend_Page = createAction("actionSend_Page", "Send Page Image", "", "", SLOT(sendPageImage()));
  actionSend_HTML = createAction("actionSend_HTML", "Send Document", "", "", SLOT(sendDocument()));
  actionSend_PDF = createAction("actionSend_PDF", "Send PDF", "", "", SLOT(sendPDF()));

  // tool actions; submodes live on the inline options rows, so these no longer have floating menus
  auto tbMenuAlign = vertToolbar ? Menu::HORZ : Menu::VERT_RIGHT;  // open to right even if more space to left

  actionPan = createAction("actionPan", "&Pan", ":/icons/ic_menu_pan.svg", "", SLOT(setMode(MODE_PAN)));
  actionPan->setCheckable(true);

  actionDraw = createAction("actionDraw", "Draw", ":/icons/ic_menu_draw.svg", "`",
      [this](){ selectDrawTool(ScribbleMode::DRAWTOOL_PEN); });
  actionDraw->setCheckable(true);
  actionHighlight = createAction("actionHighlight", "Highlight", ":/icons/ic_menu_highlight.svg", "",
      [this](){ selectDrawTool(ScribbleMode::DRAWTOOL_HIGHLIGHT); });
  actionHighlight->setCheckable(true);
  actionHighlight->tooltip = _("Draw under existing strokes");
  actionEphemeral = createAction("actionEphemeral", "Ephemeral", ":/icons/ic_menu_ephemeral.svg", "",
      [this](){ selectDrawTool(ScribbleMode::DRAWTOOL_EPHEMERAL); });
  actionEphemeral->setCheckable(true);
  actionEphemeral->tooltip = _("Strokes are not saved with the document");
  // 1-8 select saved pens; 9 for draw options row, 0 for bookmark
  actionCustom_Pen = createAction("actionCustom_Pen",
      "Pen Setup...", ":/icons/ic_menu_set_pen.svg", "9", [this](){ togglePenToolbar(); });
  actionCustom_Pen->tooltip = _("Customize and save pens");
  actionAdd_Bookmark = createAction("actionAdd_Bookmark",
      "Add Bookmark", ":/icons/ic_menu_add_bookmark.svg", "0", SLOT(setMode(MODE_BOOKMARK)));
  actionAdd_Bookmark->setCheckable(true);
  actionAdd_Bookmark->tooltip = _("Drop beside text to show in bookmark pane");

  actionShapes = createAction("actionShapes", "Shapes", ":/icons/ic_menu_shapes.svg", "",
      [this](){ selectTool(MODE_DRAWSHAPE); });
  actionShapes->setCheckable(true);
  actionShapes->tooltip = _("Draw lines, arrows, boxes and ellipses");
  for(int ii = 0; ii < SHAPE_COUNT; ++ii) {
    const ShapeDef* def = shapeDef(ii);
    actionShape[ii] = createAction((std::string("actionShape_") + def->id).c_str(),
        def->name, def->icon, "", [this, ii](){ selectShape(ii); });
    actionShape[ii]->setCheckable(true);
  }
  // not a shape: dragging a box with it makes a ruling region, an area with its own lines
  actionRulingRegion = createAction("actionRulingRegion", "Ruling Region", ":/icons/ic_menu_toggle_ruled.svg", "",
      [this](){
        app->scribbleMode->drawRegion = true;
        app->setMode(MODE_DRAWSHAPE);
        showOptionsRow(MODE_DRAWSHAPE);
      });
  actionRulingRegion->setCheckable(true);
  actionRulingRegion->tooltip = _("Drag a box with its own lines, to write straight over uneven or tilted ones");

  actionErase = createAction("actionErase", "Erase", ":/icons/ic_menu_erase.svg", "",
      [this](){ selectTool(MODE_ERASE); });
  actionErase->setCheckable(true);
  // erase menu items
  actionStroke_Eraser = createAction("actionStroke_Eraser",
      "Stroke Eraser", ":/icons/ic_menu_erase.svg", "", SLOT(setMode(MODE_ERASESTROKE)));
  actionStroke_Eraser->setCheckable(true);
  actionStroke_Eraser->tooltip = _("Erase whole strokes");
  actionRuled_Eraser = createAction("actionRuled_Eraser",
      "Ruled Eraser", ":/icons/ic_menu_erase_ruled.svg", "", SLOT(setMode(MODE_ERASERULED)));
  actionRuled_Eraser->setCheckable(true);
  actionRuled_Eraser->tooltip = _("Erase handwritten text\nUse in margin to erase whole lines");
  actionFree_Eraser = createAction("actionFree_Eraser",
      "Free Eraser", ":/icons/ic_menu_erase_free.svg", "", SLOT(setMode(MODE_ERASEFREE)));
  actionFree_Eraser->setCheckable(true);
  actionFree_Eraser->tooltip = _("Erase parts of strokes");

  // the tools row shows the lasso (the mockup's select glyph); the rectangle icon is the Rect Select
  //  entry on the select options row
  actionSelect = createAction("actionSelect", "Select", ":/icons/ic_menu_select_tool.svg", "",
      [this](){ selectTool(MODE_SELECT); });
  actionSelect->setCheckable(true);
  // select menu items
  actionRect_Select = createAction("actionRect_Select",
      "Rect Select", ":/icons/ic_menu_select.svg", "", SLOT(setMode(MODE_SELECTRECT)));
  actionRect_Select->setCheckable(true);
  actionRect_Select->tooltip = _("Select within rectangle");
  actionRuled_Select = createAction("actionRuled_Select",
      "Ruled Select", ":/icons/ic_menu_select_ruled.svg", "", SLOT(setMode(MODE_SELECTRULED)));
  actionRuled_Select->setCheckable(true);
  actionRuled_Select->tooltip = _("Select handwritten text\nUse in margin to select whole lines");
  actionLasso_Select = createAction("actionLasso_Select",
      "Lasso Select", ":/icons/ic_menu_select_lasso.svg", "", SLOT(setMode(MODE_SELECTLASSO)));
  actionLasso_Select->setCheckable(true);
  actionLasso_Select->tooltip = _("Select within path");
  actionPath_Select = createAction("actionPath_Select",
      "Path Select", ":/icons/ic_menu_select_path.svg", "", SLOT(setMode(MODE_SELECTPATH)));
  actionPath_Select->setCheckable(true);
  actionPath_Select->tooltip = _("Select along path");

  actionInsert_Space = createAction("actionInsert_Space", "Insert Space",
      ":/icons/ic_menu_insert_space.svg", "", [this](){ selectTool(MODE_INSSPACE); });
  actionInsert_Space->setCheckable(true);
  // insert space menu items
  actionInsert_Space_Vert = createAction("actionInsert_Space_Vert",
      "Insert Space", ":/icons/ic_menu_insert_space.svg", "", SLOT(setMode(MODE_INSSPACEVERT)));
  actionInsert_Space_Vert->setCheckable(true);
  actionInsert_Space_Vert->tooltip = _("Insert vertical space");
  actionRuled_Insert_Space = createAction("actionRuled_Insert_Space",
      "Ruled Insert Space", ":/icons/ic_menu_insert_space_ruled.svg", "", SLOT(setMode(MODE_INSSPACERULED)));
  actionRuled_Insert_Space->setCheckable(true);
  actionRuled_Insert_Space->tooltip = _("Insert whole lines\nReflow handwritten text");

  // tools have priority over other toolbar items (except overflow menu); pan should hide before tools
  actionPan->setPriority(Action::NormalPriority + 1);
  actionDraw->setPriority(Action::NormalPriority + 2);
  actionHighlight->setPriority(Action::NormalPriority + 2);
  actionEphemeral->setPriority(Action::NormalPriority + 2);
  actionErase->setPriority(Action::NormalPriority + 2);
  actionSelect->setPriority(Action::NormalPriority + 2);
  actionInsert_Space->setPriority(Action::NormalPriority + 2);
  // paste is more important than copy, cut, etc. assuming popup sel menu is enabled
  actionPaste->setPriority(Action::NormalPriority + 1);

  // no combined tools menu: every submode is reachable from the tool's options row instead
  // actions menu needed even with touch UI for popup tool menu
  //menu_Actions = createMenu("menu_Actions", "&Tools");
  //menu_Actions->addAction(actionPan);  actionDraw, actionErase, actionSelect, actionInsert_Space

  // storage for other tool icons (switched in/out depending on mode)
  drawIcon = SvgGui::useFile(":/icons/ic_menu_draw.svg");
  nextPageIcon = SvgGui::useFile(":/icons/ic_menu_next.svg");
  appendPageIcon = SvgGui::useFile(":/icons/ic_menu_append_page.svg");
  editBoxIcon = SvgGui::useFile(":/icons/ic_menu_editbox.svg");
  swbIcon = SvgGui::useFile(":/icons/ic_menu_cloud.svg");

  // toolbar menus
  actionOverflow_Menu = createAction("actionOverflow_Menu", "Menu", ":/icons/ic_menu_overflow.svg", "", NULL);
  actionSelection_Menu = createAction("actionSelection_Menu", "Selection Menu", ":/icons/ic_menu_paste.svg", "", NULL);

  // the overflow menu's submenus are popups too, so the whole tree shares the arrow popup styling
  ArrowPopup* docmenu = createArrowPopup(Menu::HORZ);
  // don't rely on these actions being on toolbar when not using doc list!
  if(!cfg->Bool("useDocList")) {
    docmenu->addAction(actionNew_Document);
    docmenu->addAction(action_Open);
    docmenu->addAction(actionSave);
  }
  if(IS_DEBUG || !PLATFORM_IOS || cfg->String("syncServer", "")[0]) {
    docmenu->addAction(actionShare_Document);
#if PLATFORM_IOS
    docmenu->addAction(actionOpen_Shared_Doc);
#endif
  }
  //if(!cfg->Bool("useDocList")) docmenu->addAction(actionOpen_Shared_Doc);
  docmenu->addAction(actionRevert);
  docmenu->addAction(actionSave_As);
  docmenu->addAction(actionInsertDocument);
#if PLATFORM_MOBILE
  docmenu->addAction(actionSend_Page);
  docmenu->addAction(actionSend_HTML);
  docmenu->addAction(actionSend_PDF);
#else
  docmenu->addAction(actionImport_PDF);
  docmenu->addAction(actionExport_PDF);
#endif

  ArrowPopup* pagemenu = createArrowPopup(Menu::HORZ);
  pagemenu->addAction(actionNew_Page_Before);
  pagemenu->addAction(actionNew_Page_After);
  // would be nice if we could avoid having menu items for these expand down/right
  pagemenu->addAction(actionExpand_Down);
  pagemenu->addAction(actionExpand_Right);
  pagemenu->addAction(actionSelect_Pages);
  pagemenu->addAction(actionScan_Page);

  ArrowPopup* viewmenu = createArrowPopup(Menu::HORZ);
  viewmenu->addAction(actionPrevious_View);
  viewmenu->addAction(actionNext_View);
  // Doesn't much sense to have these buried in submenu now that we have zoom button on statusbar
  //viewmenu->addAction(actionZoom_In);
  //viewmenu->addAction(actionZoom_Out);
  //viewmenu->addAction(actionReset_Zoom);
  Button* splitviewbtn = viewmenu->addAction(actionSplitView);
  splitviewbtn->mPopup->setAlign(Menu::HORZ_LEFT);
  viewmenu->addAction(actionShow_Bookmarks);  // in case hidden from toolbar
  // Qt uses immersive mode (sticky) for fullscreen on Android 4.4+; hides status bar on earlier versions
  viewmenu->addAction(actionFullscreen);
  viewmenu->addAction(actionInvertColors);

  Action* selactions[] = {actionCut, actionCopy, actionPaste, actionDupSel, actionDelete_Selection,
      actionUngroup, actionInvert_Selection, actionSelect_All, actionCreate_Link};  //actionSelect_Similar
  ArrowPopup* selectionmenu = createArrowPopup(Menu::HORZ);
  for(Action* a : selactions)
    selectionmenu->addAction(a);
  // trying to share the same Menu between overflow menu and Selection Menu action causes problems
  Menu* selmenu2 = createMenu("selMenu2", "Selection", tbMenuAlign);
  for(Action* a : selactions)
    selmenu2->addAction(a);
  actionSelection_Menu->setMenu(selmenu2);
  // selection menu is one step above title button
  //actionSelection_Menu->setPriority(Action::NormalPriority - 2);

  // menu for shared whiteboard actions we want to always to be available - this is just a temp solution
  menuWhiteboard = createArrowPopup(Menu::HORZ);
  menuWhiteboard->addAction(actionSyncInfo);
  menuWhiteboard->addAction(actionViewSync);
  menuWhiteboard->addAction(actionViewSyncMaster);
  menuWhiteboard->addAction(actionSendImmed);

  // Note: Nexus 4 fits 10 menu items on screen (portrait)
  // the overflow menu uses the same arrow popup chrome as the help and pen option popups; its contents are
  //  still menu items, so submenus and click-to-close behave as before (opened in createToolBars())
  overflowPopup = createArrowPopup(vertToolbar ? Menu::HORZ : Menu::VERT);
  overflowPopup->node->setXmlId("overflowMenu");
  // toolbar items dropped by auto-adjust are mirrored into this group, added before anything else so the
  //  items appear at the top of the menu; both it and its separator are shown only when it has visible items
  overflowHiddenGroup = createColumn({}, "", "", "hfill");
  overflowHiddenGroup->setVisible(false);
  overflowPopup->addWidget(overflowHiddenGroup);
  overflowHiddenSep = new Widget(widgetNode("#menu-separator"));
  overflowHiddenSep->setVisible(false);
  overflowPopup->addWidget(overflowHiddenSep);
  menuWhiteboardBtn = overflowPopup->addSubmenu(_("Whiteboard"), menuWhiteboard);
  overflowPopup->addSubmenu(_("Document"), docmenu);
  overflowPopup->addSubmenu(_("Page"), pagemenu);
  overflowPopup->addSubmenu(_("View"), viewmenu);
  overflowPopup->addSubmenu(_("Selection"), selectionmenu);
  overflowPopup->addAction(actionPage_Setup);
  overflowPopup->addAction(actionTheme);
  overflowPopup->addAction(actionTagDocList);
  overflowPopup->addAction(actionInsert_Image);  // move to Document menu?
  overflowPopup->addAction(actionScan_Element);
  overflowPopup->addAction(actionPreferences);

#ifdef SCRIBBLE_TEST
  Action* actionRunTests = createAction("actionRunTests", "Run Tests", "", "", SLOT(runTestUI("test")));
  Action* actionSyncTests = createAction("actionSyncTests", "Sync Tests", "", "", SLOT(runTestUI("synctest")));
  Action* actionPerfTests = createAction("actionPerfTests", "Performance Test", "", "", SLOT(runTestUI("perftest")));
  Action* actionInputTests = createAction("actionInputTests", "Input Test", "", "", SLOT(runTestUI("inputtest")));
  ArrowPopup* testmenu = createArrowPopup(Menu::HORZ);
  testmenu->addAction(actionRunTests);
  testmenu->addAction(actionSyncTests);
  testmenu->addAction(actionPerfTests);
  testmenu->addAction(actionInputTests);
  overflowPopup->addSubmenu("Testing", testmenu);
#endif

  setupAutoClosePopup(overflowPopup);
  // never hide overflow menu
  actionOverflow_Menu->setPriority(100); //Action::HighPriority);

  // hide whiteboard menu initially
  menuWhiteboardBtn->setEnabled(false);
  menuWhiteboardBtn->setVisible(false);

  // undoRedoBtn and docTitle ... how to set priority for these?
  // - maybe create a subclass of Action that is tied to a single button?
  titleButton = createToolbutton(appIcon, "Kaku", true);
  titleButton->onClicked = SLOT(openDocument());
  menuRecent_Files = createMenu("menuRecent", "Recent Documents", tbMenuAlign, false);  //Menu::VERT_RIGHT
  // The iOS system browser has recents already (and we'd need to save secured bookmarks to open recents
  //  ourselves); library documents are plain files, so with the tag browser recents work there too
  if(!PLATFORM_IOS || ScribbleApp::cfg->Bool("useTagDocList")) {
  titleButton->addWidget(menuRecent_Files);
  titleButton->addHandler([this](SvgGui* gui, SDL_Event* event){
    if(isLongPressOrRightClick(event)) {
      gui->showMenu(menuRecent_Files);
      gui->setPressed(menuRecent_Files);
      titleButton->node->setXmlClass(
          addWord(removeWord(titleButton->node->xmlClass(), "hovered"), "pressed").c_str());
      return true;
    }
    return false;
  });
  setupTooltip(titleButton, altTooltip(_("Documents"), _("Recent Documents")));
  }
  else
    setupTooltip(titleButton, "Open Document");

  // "History", not "Undo": tapping it still undoes, but the panel behind it is a history scrubber and a
  //  selector, and the undo arrow promised only the first of those.  Plain undo is still reachable by the
  //  tap, by Ctrl+Z and from the menu, so the rename costs recognition, not capability.
  undoRedoBtn = createToolbutton(SvgGui::useFile(":/icons/ic_menu_history.svg"), _("History"));
  // the popup places itself relative to the button, so the timeline needs no manual offsets (unlike the
  //  dial it replaces, which was hand-centered under the button)
  ButtonDragTimeline* undoTimeline = new ButtonDragTimeline(undoRedoBtn, vertToolbar ? Menu::HORZ : Menu::VERT);
  // the lane labels are the only place either feature names itself, so they carry the explaining
  undoTimeline->laneLabel[ButtonDragTimeline::LANE_HISTORY] = _("Undo / Redo");
  undoTimeline->laneLabel[ButtonDragTimeline::LANE_SELECT] = _("Select recent");
  undoTimeline->hintText = _("Drag sideways to step through history");
  undoTimeline->showHint = ScribbleApp::cfg->Int("historyHintDone") == 0;
  undoTimeline->onHintDone = [this](){
    ScribbleApp::cfg->set("historyHintDone", 1);
    if(historyTip) historyTip->setVisible(false);
  };
  // the tip goes away as soon as the panel is opened - at that point it is in the way of the thing it
  //  is pointing at, and the panel's own hint takes over
  undoTimeline->onOpened = [this](){ if(historyTip) historyTip->setVisible(false); };

  // The unprompted tip: an arrow popup on the button itself, put up once there is history to act on.
  //  Text rather than an animation for now, and deliberately naming both lanes - someone who never
  //  presses the button cannot be taught by anything inside the panel.
  if(!vertToolbar && ScribbleApp::cfg->Int("historyHintDone") == 0) {
    historyTip = createArrowPopup(Menu::VERT);
    SvgText* tipNode = createTextNode(_("Tap to undo. Press and hold, then drag sideways to move "
        "through history, or up and down to switch between Undo / Redo and Select recent."));
    tipNode->addClass("arrowpopup-desc");
    tipNode->setAttribute("box-anchor", "left");
    std::string wrapped = SvgPainter::breakText(tipNode, 253 - 32);
    if(wrapped != tipNode->text()) {
      tipNode->clearText();
      tipNode->addText(wrapped.c_str());
    }
    historyTip->addWidget(new Widget(tipNode));
    undoRedoBtn->addWidget(historyTip);
    historyTip->setVisible(false);
  }
  undoTimeline->getRange = [this](int& back, int& fwd){
    UndoHistory* hist = app->activeDoc()->history;
    back = int(hist->undoSteps());
    fwd = int(hist->redoSteps());
  };
  undoTimeline->onStep = [this](int delta){
    while(delta > 0 && app->activeDoc()->canRedo()) {
      app->doCommand(ID_REDO);
      delta--;
    }
    while(delta < 0 && app->activeDoc()->canUndo()) {
      app->doCommand(ID_UNDO);
      delta++;
    }
    return delta;
  };
  undoTimeline->onAltStep = [this](int delta){
    if(delta == 0)
      app->activeArea()->recentStrokeSelDone();
    while(delta > 0 && app->activeArea()->recentStrokeDeselect())
      delta--;
    while(delta < 0 && app->activeArea()->recentStrokeSelect())
      delta++;
    return delta;
  };
  setupTooltip(undoRedoBtn, altTooltip(_("History (tap to undo)"), _("Select Recent")));
}

// one-time help popups ... disabled for now awaiting further consideration
#ifdef ONE_TIME_TIPS
#include <unordered_set>

static const char* tipPopupSVG = R"#(
  <g class="tooltip tip-dialog" position="absolute" display="none" layout="box">
    <rect box-anchor="fill" stroke-width="0.5" stroke="#000" width="36" height="36"/>
    <g class="dialog-layout" box-anchor="fill" layout="flex" flex-direction="column">
      <g class="body-container" box-anchor="fill" layout="flex" flex-direction="column">
      <text margin="5"></text>
      </g>
      <g class="button-container dialog-buttons" margin="5 4" box-anchor="hfill" layout="flex" flex-direction="row" justify-content="flex-end">

        <g class="btn-close-tip toolbutton" box-anchor="vfill" layout="box" margin="0 5">
          <rect box-anchor="hfill" stroke-width="0.8" stroke="#000" width="36" height="36"/>
          <text class="title" margin="4 4"></text>
        </g>

        <g class="btn-no-tips toolbutton" box-anchor="vfill" layout="box" margin="0 5">
          <rect box-anchor="hfill" stroke-width="0.8" stroke="#000" width="36" height="36"/>
          <text class="title" margin="4 4"></text>
        </g>

      </g>
    </g>
  </svg>
)#";

bool MainWindow::oneTimeTip(const char* id, Point pos, const char* message)
{
  static auto flags1 = splitStr<std::unordered_set>(ScribbleApp::cfg->String("flags1", ""), ',', true);
  static AbsPosWidget* dialog = NULL;

  // load flags1 if not loaded yet
  if(flags1.count(id) || flags1.count("notips"))  //!cfg->Bool("showHelpTips"))
    return false;
  if(!message)
    return true;
  // now actually show the dialog
  if(!dialog) {
    dialog = new AbsPosWidget(loadSVGFragment(tipPopupSVG));
    // currently not possible to select by id in a fragment (i.e., no document)
    Button* closebtn = new Button(dialog->containerNode()->selectFirst(".btn-close-tip"));
    closebtn->setTitle(_("CLOSE "));
    closebtn->onClicked = [](){ dialog->setVisible(false); };
    Button* notipsbtn = new Button(dialog->containerNode()->selectFirst(".btn-no-tips"));
    notipsbtn->setTitle(_("NO MORE TIPS"));
    notipsbtn->onClicked = [](){ dialog->setVisible(false); flags1.insert("notips"); };
    selectFirst("#main-container")->addWidget(dialog);
  }
  dialog->selectFirst(".body-container")->setText(message);
  Rect parentBounds = dialog->node->parent()->bounds();
  dialog->node->setAttribute("left", fstring("%g", pos.x - parentBounds.left).c_str());
  dialog->node->setAttribute("top", fstring("%g", pos.y - parentBounds.top).c_str());
  dialog->setVisible(true);
  flags1.insert(id);  // mark tip as shown
  return true;
}

void MainWindow::setupHelpTips()
{
}
#else
bool MainWindow::oneTimeTip(const char* id, Point pos, const char* message) { return false; }
void MainWindow::setupHelpTips() {}
#endif
