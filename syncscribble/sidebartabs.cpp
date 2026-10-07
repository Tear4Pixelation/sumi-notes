// The sidebar's Tabs view (docs/agent/editor-tabs.md): one row per open document, over
//  ScribbleApp::tabs.  Kept out of sidebar.cpp so the view is self-contained; the row shape, sizes and
//  classes are sidebar.cpp's own, so a tab row is built exactly like an outline or layer row.

#include "sidebar.h"

#include "mainwindow.h"
#include "scribbleapp.h"
#include "scribbledoc.h"
#include "ulib/stringutil.h"
#include "ulib/fileutil.h"

// sidebar.cpp's sizes for an outline row, its icons and titles (all from the same Penpot board)
static const Dim tabRowH      = 50*floatUIScale;
static const Dim tabPad       = 12*floatUIScale;
static const Dim tabIconSize  = 24*floatUIScale;
static const Dim tabTitleSize = 24*floatUIScale;
// on touch the close button and the grip are full toolbar cells, as the layer grip is
static const Dim tabCellSize  = floatTouchUI ? floatBtnSize : tabIconSize;

static Button* createRowButton(const char* icon, const char* title)
{
  Button* btn = createToolbutton(SvgGui::useFile(icon), title);
  static_cast<SvgUse*>(btn->selectFirst(".icon")->node)->setViewport(Rect::wh(tabIconSize, tabIconSize));
  // the stock toolbutton background is a full toolbar hit target and would set the row's height
  Widget* bg = btn->selectFirst(".background");
  if(bg && bg->node->type() == SvgNode::RECT)
    static_cast<SvgRect*>(bg->node)->setRect(Rect::wh(tabCellSize, tabCellSize));
  return btn;
}

void Sidebar::setupTabDrag()
{
  tabDrag.reset(new RowDrag(this));
  tabDrag->canDrop = [](int src, int dst){ return src != dst && dst != RowDrag::ROOT; };
  tabDrag->onDrop = [this](int src, int dst){
    if(ScribbleApp::app)
      ScribbleApp::app->moveTab(src, dst);
    rebuildList();
  };
  // out of the list and onto the canvas: a new pane on the nearest edge, or the pane it lands on
  tabDrag->onDragOutside = [](int src, Point pos){
    if(ScribbleApp::app)
      ScribbleApp::app->previewTabDrop(pos);
  };
  tabDrag->onDropOutside = [this](int src, Point pos){
    if(ScribbleApp::app && ScribbleApp::app->dropTabOnCanvas(src, pos))
      tabChosen();
    else
      rebuildList();
  };
}

std::string Sidebar::tabsState() const
{
  ScribbleApp* app = ScribbleApp::app;
  if(!app)
    return "none";
  std::string state = fstring("tabs %d|", app->activeTabIndex());
  for(const EditorTab& tab : app->tabs.tabs) {
    bool modified = tab.doc && tab.doc->document && tab.doc->isModified();
    state += fstring("%s %d %d\n", tab.path.c_str(), tab.doc ? int(tab.doc->nViews) : -1, modified ? 1 : 0);
  }
  return state;
}

// A tab was picked (tapped, or dropped on the canvas): a temporary showing has done its job, and an
//  unpinned sidebar is in the way of the document it just brought up - the outline rows do the same.
void Sidebar::tabChosen()
{
  if(temporary || !pinned)
    setOpen(false);
  else
    rebuildList();
}

void Sidebar::buildTabRows()
{
  ScribbleApp* app = ScribbleApp::app;
  if(!app)
    return;
  int activeIdx = app->activeTabIndex();
  for(int idx = 0; idx < app->tabs.size(); ++idx) {
    const EditorTab& tab = app->tabs[idx];
    std::string title = FSPath(tab.path).baseName();
    if(!matchesSearch(title))
      continue;
    // a background tab's changes are saved when it is left, so this marks the shown ones being edited
    bool modified = tab.doc && tab.doc->document && tab.doc->isModified();

    Widget* row = createRow(tabRowH, tabPad);
    Widget* content = row->selectFirst(".sb-row-content");
    SvgText* titleNode = createTextNode((modified ? title + " *" : title).c_str());
    titleNode->addClass("sb-title");
    titleNode->setAttr<float>("font-size", tabTitleSize);
    Widget* titleWidget = new Widget(titleNode);
    titleWidget->node->setAttribute("box-anchor", "left");
    content->addWidget(titleWidget);
    content->addWidget(createStretch());

    // Closing saves and may open the library (the last tab), so it runs after this press is handled -
    //  and off MainWindow, since a tab picked from a temporary showing hides the sidebar, and hiding a
    //  widget drops its timers.  The tab is found again by path: the list can change in between.
    std::string path = tab.path;
    Button* closeBtn = createRowButton("icons/ic_menu_cancel.svg", _("Close Tab"));
    closeBtn->onClicked = [this, path](){
      gui()->setTimer(1, mainWindow, [this, path](){
        ScribbleApp* scribbleApp = ScribbleApp::app;
        int current = scribbleApp ? scribbleApp->tabs.find(path) : -1;
        if(current >= 0)
          scribbleApp->closeTab(current);
        rebuildList();
        return 0;
      });
    };
    closeBtn->setMargins(0, tabPad, 0, 0);
    content->addWidget(closeBtn);

    // Reorder by the grip, as the layer rows do: on a tablet the list scrolls vertically, so a vertical
    //  drag on the row itself can only ever scroll it.  The row stays draggable too (rowToo), which is
    //  the sideways drag onto the canvas with a pen or finger, and any drag with a mouse.
    Button* grip = createRowButton("icons/ic_menu_reorder.svg", _("Drag to Reorder"));
    grip->node->addClass("sb-grip");
    grip->setMargins(0, 0, 0, floatTouchUI ? 0 : tabPad);
    content->addWidget(grip);
    tabDrag->addRow(static_cast<Button*>(row), idx, app->tabs.size() > 1 ? grip : NULL, true);
    if(app->tabs.size() < 2)
      grip->setVisible(false);

    if(idx == activeIdx)
      row->node->addClass("checked");
    static_cast<Button*>(row)->onClicked = [this, path](){
      gui()->setTimer(1, mainWindow, [this, path](){
        ScribbleApp* scribbleApp = ScribbleApp::app;
        int current = scribbleApp ? scribbleApp->tabs.find(path) : -1;
        if(current >= 0 && scribbleApp->switchToTab(current))
          tabChosen();
        else
          rebuildList();
        return 0;
      });
    };
    listView->addWidget(row);
  }
}
