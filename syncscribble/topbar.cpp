// The top tab bar - see topbar.h and docs/agent/top-tab-bar.md

#include "topbar.h"

#include "scribbleapp.h"
#include "scribbledoc.h"
#include "touchwidgets.h"
#include "usvg/svgpainter.h"
#include "ulib/fileutil.h"
#include "ulib/stringutil.h"

// Penpot "Tab Topbar", board "Board" (1440 x 64): flex row, padding 4 12, gap 4, items centred; home 56 x 56,
//  radius 12, 32 icon; tabs gap 12, 56 high, padding 0 24, title Raleway 18, close 32 icon, 601 wide at most;
//  View a bare 33 icon, overflow a bare 42 one.  The user wants the bar exactly as high as a toolbar rather
//  than 64, so every one of those values is scaled by (toolbar row height)/64 and the proportions stay.
static const Dim designBarHeight = 64;
// The bar is a little shorter than a toolbar row (user: strip 10 percent off the first toolbar-height version).
static const Dim barHeightFraction = 0.9;

struct BarMetrics
{
  Dim barHeight;  // = one toolbar row
  Dim barPadV, barPadH, barGap;
  Dim homeSize, btnRadius, iconSize;
  Dim tabGap, tabHeight, tabPadH, closeSize, closeRadius, titleFontSize;
  Dim tabMaxWidth, tabMinWidth;
  Dim viewIconSize, overflowIconSize;
  Dim bgOverdraw;  // how far the bar's background reaches under whatever follows it (see the constructor)
};

// The height of the app's regular toolbar row: a toolbar is a flex row of "toolbutton"s (ugui theme.cpp,
//  #toolbutton), and the background rect of one is as tall as the row.  Read from the theme, not copied.
static Dim toolbarRowHeight()
{
  Button* probe = createToolbutton(SvgGui::useFile(":/icons/ic_menu_view.svg"));
  SvgNode* bgRect = probe->node->selectFirst(".background");
  Dim height = bgRect ? static_cast<SvgRect*>(bgRect)->getRect().height() : 0;
  delete probe->node;
  return height > 0 ? height : designBarHeight;
}

static const BarMetrics& metrics()
{
  static BarMetrics m = [](){
    BarMetrics m;
    m.barHeight = toolbarRowHeight() * barHeightFraction;
    Dim s = m.barHeight/designBarHeight;
    m.barPadV = 4*s;  m.barPadH = 12*s;  m.barGap = 4*s;
    m.homeSize = 56*s;  m.btnRadius = 12*s;  m.iconSize = 32*s;
    m.tabGap = 12*s;  m.tabHeight = 56*s;  m.tabPadH = 24*s;
    m.closeSize = 32*s;  m.closeRadius = 8*s;  m.titleFontSize = 18*s/barHeightFraction;  // titles keep the size they had at full toolbar height
    m.tabMaxWidth = 601*s;  m.tabMinWidth = 96*s;
    m.viewIconSize = 33*s;  m.overflowIconSize = 42*s;
    m.bgOverdraw = 2;
    return m;
  }();
  return m;
}

const char* TopBar::stylesheet = R"#(
svg.window { --tab-active: #444444; }  /* tokens.json canvas-background (theme-dark) */
svg.window.light { --tab-active: #D0D0D0; }  /* tokens.json canvas-background (theme-light) */
.topbar-bg { fill: var(--dark); }
.topbar .icon { fill: var(--icon); color: var(--icon); }
.topbar-btn-bg { fill: none; }
.topbar-btn.hovered > .topbar-btn-bg { fill: var(--tab-active); fill-opacity: 0.5; }
.topbar-btn.pressed > .topbar-btn-bg { fill: var(--tab-active); fill-opacity: 1; }
.topbar-btn.checked > .topbar-btn-bg { fill: var(--tab-active); fill-opacity: 1; }
.topbar-tab-title { font-family: raleway; fill: var(--text); }  /* font-size: attribute, scaled (metrics) */
.topbar-home .topbar-icon-fill { display: none; }
.topbar-home.checked .topbar-icon-fill { display: block; }
.topbar-home.checked .topbar-icon-outline { display: none; }
.topbar-close-bg { fill: none; }
.topbar-close.hovered > .topbar-close-bg { fill: var(--dark); fill-opacity: 0.4; }
.topbar-close.pressed > .topbar-close-bg { fill: var(--dark); fill-opacity: 0.7; }
)#";

static Widget* createFlexRow()
{
  Widget* flexRow = new Widget(new SvgG());
  flexRow->node->setAttribute("layout", "flex");
  flexRow->node->setAttribute("flex-direction", "row");
  flexRow->node->setAttribute("align-items", "center");
  return flexRow;
}

TopBar::TopBar(bool library) : Widget(new SvgG()), inLibrary(library)
{
  const BarMetrics& m = metrics();
  node->addClass("topbar");
  node->setAttribute("box-anchor", "hfill");
  node->setAttribute("layout", "box");
  // the bar's color, under the contents and under the status bar padding above them (setTopPadding)
  // Not box-anchor fill: ugui margins lie outside a widget's rect, so a fill rect stops short of the row's
  //  padding and the strip under it was never painted (it showed whatever was there before - tab titles
  //  from the previous padding).  The rect's own height is the whole bar, padding included - plus
  //  bgOverdraw, taken back by a negative margin so the bar's layout height is unchanged: the bar height is not a
  //  whole number of pixels at most scales, and the pixel row the bar shares with what follows was
  //  antialiased from both sides over stale pixels, never cleared.  What follows paints over the overlap.
  bg = new SvgRect(Rect::wh(20, m.barHeight + m.bgOverdraw));
  bg->addClass("topbar-bg");
  bg->setAttribute("box-anchor", "top hfill");
  bg->setAttribute("margin", fstring("0 0 %g 0", -m.bgOverdraw).c_str());
  containerNode()->addChild(bg);

  row = createFlexRow();
  row->node->setAttribute("box-anchor", "top hfill");
  row->setMargins(m.barPadV, m.barPadH, m.barPadV, m.barPadH);
  addWidget(row);

  // home: the library.  Its glyph is filled while the library is what is shown (the library's own bar)
  homeBtn = createIconButton(NULL, m.iconSize, _("Library"));
  homeBtn->node->addClass("topbar-home");
  SvgUse* outline = new SvgUse(Rect::wh(m.iconSize, m.iconSize), "", SvgGui::useFile(":/icons/ic_menu_home.svg"));
  outline->addClass("icon");
  outline->addClass("topbar-icon-outline");
  homeBtn->containerNode()->addChild(outline);
  SvgUse* filled = new SvgUse(Rect::wh(m.iconSize, m.iconSize), "", SvgGui::useFile(":/icons/ic_menu_home_fill.svg"));
  filled->addClass("icon");
  filled->addClass("topbar-icon-fill");
  homeBtn->containerNode()->addChild(filled);
  homeBtn->setChecked(inLibrary);
  homeBtn->onClicked = [this](){ if(onHome) onHome(); };
  row->addWidget(homeBtn);

  // the tabs take whatever is left between home and the buttons at the right end; fitTabs() divides it
  tabsRow = createFlexRow();
  tabsRow->node->setAttribute("box-anchor", "left");
  tabsAdj = new AutoAdjContainer(new SvgG(), tabsRow);
  tabsAdj->node->setAttribute("box-anchor", "hfill");
  tabsAdj->adjFn = [this](const Rect& src, const Rect& dest){
    fitTabs(dest.width());
    tabsAdj->repeatLayout(dest);
  };
  tabsAdj->setMargins(0, 0, 0, m.barGap);
  row->addWidget(tabsAdj);

  // View: the old overflow menu's View submenu, as a popup of its own (the owner attaches it)
  viewBtn = createIconButton(":/icons/ic_menu_view.svg", m.viewIconSize, _("View"));
  viewBtn->setMargins(0, 0, 0, m.barGap);
  row->addWidget(viewBtn);
  if(!inLibrary) {
    overflowBtn = createIconButton(":/icons/ic_menu_overflow.svg", m.overflowIconSize, _("Menu"));
    overflowBtn->setMargins(0, 0, 0, m.barGap);
    row->addWidget(overflowBtn);
  }
}

void TopBar::setTopPadding(Dim pad)
{
  topPad = pad;
  const BarMetrics& m = metrics();
  row->setMargins(m.barPadV + pad, m.barPadH, m.barPadV, m.barPadH);
  bg->setRect(Rect::wh(bg->getRect().width(), pad + m.barHeight + m.bgOverdraw));
}

// a button with no background of its own at rest: hovering and pressing light up its rounded rect, and
//  checked (home in the library) fills it like an active tab.  The rect is always a homeSize SQUARE whatever
//  the icon's size (it was icon wide and homeSize high: a tall rectangle for View and the overflow button);
//  the icon is centred in it by the box layout.
Button* TopBar::createIconButton(const char* icon, Dim iconSz, const char* tooltip)
{
  const BarMetrics& m = metrics();
  Button* btn = new Button(new SvgG());
  btn->node->addClass("topbar-btn");
  btn->node->setAttribute("layout", "box");
  SvgRect* bg = new SvgRect(Rect::wh(m.homeSize, m.homeSize), m.btnRadius, m.btnRadius);
  bg->addClass("topbar-btn-bg");
  btn->containerNode()->addChild(bg);
  if(icon) {
    SvgUse* use = new SvgUse(Rect::wh(iconSz, iconSz), "", SvgGui::useFile(icon));
    use->addClass("icon");
    btn->containerNode()->addChild(use);
  }
  setupTooltip(btn, tooltip);
  return btn;
}

TopBar::TabCell TopBar::createTab(const std::string& path, bool active)
{
  const BarMetrics& m = metrics();
  TabCell cell;
  cell.fullTitle = FSPath(path).baseName();
  cell.active = active;
  cell.btn = new Button(new SvgG());
  cell.btn->node->addClass("topbar-btn");
  cell.btn->node->addClass("topbar-tab");
  cell.btn->node->setAttribute("layout", "box");
  cell.bg = new SvgRect(Rect::wh(m.tabMaxWidth, m.tabHeight), m.btnRadius, m.btnRadius);
  cell.bg->addClass("topbar-btn-bg");
  cell.btn->containerNode()->addChild(cell.bg);

  Widget* content = createFlexRow();
  content->node->setAttribute("box-anchor", "fill");
  content->setMargins(0, m.tabPadH, 0, m.tabPadH);
  cell.title = createTextNode(cell.fullTitle.c_str());
  cell.title->addClass("topbar-tab-title");
  cell.title->setAttribute("font-size", fstring("%g", m.titleFontSize).c_str());
  content->addWidget(new Widget(cell.title));
  content->addWidget(createStretch());
  if(active) {
    // closing is offered on the active tab only, as designed: the others are one tap from being active
    Button* closeBtn = new Button(new SvgG());
    closeBtn->node->addClass("topbar-close");
    closeBtn->node->setAttribute("layout", "box");
    SvgRect* closeBg = new SvgRect(Rect::wh(m.closeSize, m.closeSize), m.closeRadius, m.closeRadius);
    closeBg->addClass("topbar-close-bg");
    closeBtn->containerNode()->addChild(closeBg);
    SvgUse* closeIcon = new SvgUse(Rect::wh(m.closeSize, m.closeSize), "", SvgGui::useFile(":/icons/ic_menu_cancel.svg"));
    closeIcon->addClass("icon");
    closeBtn->containerNode()->addChild(closeIcon);
    setupTooltip(closeBtn, _("Close Tab"));
    closeBtn->onClicked = [this, path](){ if(onCloseTab) onCloseTab(path); };
    content->addWidget(closeBtn);
  }
  cell.btn->addWidget(content);
  cell.btn->setChecked(active);
  cell.btn->onClicked = [this, path](){ if(onTab) onTab(path); };
  setupTooltip(cell.btn, cell.fullTitle.c_str());
  return cell;
}

std::string TopBar::tabState() const
{
  ScribbleApp* app = ScribbleApp::app;
  // in the library, home is the active "tab"; the document behind it is not
  std::string state = fstring("%d|", inLibrary ? -1 : app->activeTabIndex());
  for(const EditorTab& tab : app->tabs.tabs)
    state += tab.path + "\n";
  return state;
}

// Once the window is shown the rebuild is deferred.  refreshUI() reaches here from inside a layout pass
//  (ScribbleView::doResizeEvent -> doRefresh, the first layout after a document is opened), and the bar,
//  earlier in the window, has already been laid out in that pass: tabs added then were never laid out (the
//  frame's paint clears the dirty flags they set), so a bar that started with no tabs stayed empty - or,
//  before the zero-size layout fix in ugui, drew an unlaid tab over home at the window's corner.  Each call
//  that sees a change sets its own 1 ms timer (no Timer* kept: closing the library window drops its
//  timers while the bar lives on); rebuild() re-checks, so extra timers do nothing.
//  A window that is not on screen - never shown (setupUI, the library's first setup()) or closed (the
//  library is cached between openings, and keeps its SvgGui) - is rebuilt at once: nothing is laid out or
//  dispatching there.  Deferring that case left the library showing the tabs of its previous opening
//  whenever the timer never ran (a library opened from a timer callback; see ScribbleApp::postDeferred).
void TopBar::refresh(bool force)
{
  if(!ScribbleApp::app)
    return;
  Window* win = window();
  SvgGui* gui = win ? win->gui() : NULL;
  if(!gui || !win->isVisible()) {
    rebuild(force);
    return;
  }
  if(!force && tabState() == shownState)
    return;
  forcePending = forcePending || force;
  gui->setTimer(1, this, [this](){
    bool forced = forcePending;
    forcePending = false;
    rebuild(forced);
    return 0;
  });
}

void TopBar::rebuild(bool force)
{
  ScribbleApp* app = ScribbleApp::app;
  if(!app)
    return;
  std::string state = tabState();
  if(!force && state == shownState)
    return;
  shownState = state;
  int activeIdx = inLibrary ? -1 : app->activeTabIndex();

  // the press that got us here may be on one of these buttons: owners defer their actions (onTab etc.)
  //  by a timer, so by now nothing is dispatching to a tab being deleted
  // a window not shown yet (setupUI) has no SvgGui: gui() is NULL there
  SvgGui* gui = window() ? window()->gui() : NULL;
  if(gui)
    gui->deleteContents(tabsRow);
  else {
    // not shown yet: nothing can be hovered or pressed, so plain deletion is enough
    for(TabCell& cell : cells) {
      cell.btn->removeFromParent();
      delete cell.btn->node;
    }
  }
  cells.clear();
  for(int idx = 0; idx < app->tabs.size(); ++idx) {
    TabCell cell = createTab(app->tabs[idx].path, idx == activeIdx);
    if(idx > 0)
      cell.btn->setMargins(0, 0, 0, metrics().tabGap);
    tabsRow->addWidget(cell.btn);
    cells.push_back(cell);
  }
  homeBtn->setChecked(inLibrary);
  tabsAdj->node->setDirty(SvgNode::BOUNDS_DIRTY);
}

void TopBar::fitTabs(Dim width)
{
  const BarMetrics& m = metrics();
  int count = int(cells.size());
  if(count == 0 || width <= 0)
    return;
  Dim share = (width - m.tabGap*(count - 1))/count;
  Dim tabWidth = std::max(m.tabMinWidth, std::min(m.tabMaxWidth, share));
  for(TabCell& cell : cells) {
    cell.bg->setRect(Rect::wh(tabWidth, m.tabHeight), m.btnRadius, m.btnRadius);
    // the title gets what the padding and the close button leave
    Dim room = tabWidth - 2*m.tabPadH - (cell.active ? m.closeSize + m.barGap*2 : 0);
    cell.title->clearText();
    cell.title->addText(cell.fullTitle.c_str());
    if(room > 0)
      SvgPainter::elideText(cell.title, room);
  }
}
