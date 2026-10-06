#include "sidebar.h"

#include "mainwindow.h"
#include "scribbleapp.h"
#include "scribbledoc.h"
#include "scribblearea.h"
#include "scribbleconfig.h"
#include "layers.h"
#include "tagdoclist.h"  // TagNameDialog, the app's small "enter a name" prompt
#include "application.h"
#include "ulib/stringutil.h"
#include <algorithm>

// Every number below is read off the Penpot "General purpose sidebar" file (board "Sidebar" for the
//  panel itself, board "Write - Main View" for where it sits), the convention tagdoclist.cpp already
//  follows: match the design file, don't eyeball it.  The design board is 1440x960 against the
//  floating toolbars' own 1440x960 mockup, so design units are the same units floatUIScale converts -
//  *floatUIScale everywhere, exactly as mainwindow.cpp and pentoolbar.cpp do.
static const Dim sbWidth        = 372*floatUIScale;
static const Dim sbPad          = 12*floatUIScale;   // panel padding, and the gap between its sections
static const real sbCorner      = 12*floatUIScale;
// Chosen to render at 28 px rather than taken from the design's 49, which lands at ~19.  Set from the
//  rendered size deliberately: this is the one control whose height was picked by eye against a
//  screenshot, so the number that matters is the one on screen.
//  NB it only takes effect at all because the .field-bg rects are "hfill" - as "fill" neither
//  dimension is reported to the layout (see createRow), so the box sized to its icon and rendered
//  12 px however large this number was.
static const Dim sbSelectorH    = 74*floatUIScale;
static const Dim sbSelectorIcon = 32*floatUIScale;
static const Dim sbSearchH      = 55*floatUIScale;
static const Dim sbIconSize     = 24*floatUIScale;
// A layer row's drag grip: the icon is the row's icon size, but on touch its hit target is a full
//  toolbar button, as the bottom row's are - a 15 pt grip is not something a finger can find.
static const Dim sbGripCell     = floatTouchUI ? floatBtnSize : sbIconSize;
// The bottom row's buttons (add, pin, side, search).  On desktop each cell is just its icon, as in the
//  design; for touch that is far too small to hit, so the cell becomes a full toolbar button and the
//  icon the toolbar's icon size (see floatTouchUI in basics.h).
static const Dim sbActionIcon   = floatTouchUI ? 32*floatUIScale : sbIconSize;
static const Dim sbActionCell   = floatTouchUI ? floatBtnSize : sbIconSize;
static const Dim sbActionsH     = std::max(Dim(61*floatUIScale), sbActionCell);
static const Dim sbRowH         = 50*floatUIScale;   // an outline row
static const Dim sbLayerRowH    = 88*floatUIScale;   // a layer row, which carries a preview
static const Dim sbPreview      = 70*floatUIScale;
static const Dim sbIndent       = 20*floatUIScale;   // per outline level (design: 12 -> 32 left pad)
static const Dim sbTitleSize    = 24*floatUIScale;   // row titles, search text
static const Dim sbSubSize      = 18*floatUIScale;   // view label, page numbers, layer names

// The sidebar's inset from the window, taken from the floating toolbar's own geometry (basics.h)
//  rather than from the design file's numbers.  The panel has to *line up* with the toolbar above
//  it, so its edge inset is the toolbar's edge inset, and the gap below the toolbar is the same
//  measurement again - the design's own 12 was smaller than floatInset and left the sidebar sitting
//  visibly further left than the toolbar.
static const Dim sbEdgeInset    = floatEdgeInset;
static const Dim sbTopInset     = floatTopInset + floatBtnSize + floatEdgeInset;
// The edge facing the canvas gets *no* gutter: floatInset there is dead space between the panel and
//  the document, where on the window side it is what lines the panel up with the toolbar.  This is
//  only visible once the document is zoomed past the viewport width - at fit-page zoom the page is
//  centred in what is left and the gap is that centring, not this.  It follows the sidebar, so it is
//  the right edge on the left and the left edge on the right.
static const Dim sbCanvasGutter = 0;

Sidebar::Sidebar(MainWindow* mw) : Widget(new SvgG()), mainWindow(mw)
{
  node->addClass("gp-sidebar");
  node->setAttribute("layout", "box");
  currView = View(ScribbleApp::cfg->Int("sidebarView", OUTLINE));
  pinned = ScribbleApp::cfg->Int("sidebarPinned", 1) != 0;
  onLeft = ScribbleApp::cfg->Int("sidebarLeft", 1) != 0;
  createUI();
  setVisible(false);
  reparent();
  installDismissFilter();
}

// A row is a <g> whose own sizing rect gives it the design's fixed height; children are laid out in a
//  centered flex row inside it.  Same shape as TagDocList::createNavRow(), which this deliberately
//  mirrors - the two sidebars should not diverge in how a list row is built.
//
// The sizer is anchored "hfill", not "fill", for the same reason the panel's background is "vfill"
//  (see createUI): prepareLayout() reports a size only for the dimensions that do *not* fill, so a
//  "fill" sizer contributes neither and the row collapses to the height of its own content.  That is
//  what happened here first - layer rows measured ~26 px against the design's 33.
//
// The bottom margin is the design's 12-unit gap between rows (its list boards space 50-tall outline
//  rows 62 apart and 88-tall layer rows 100 apart).  It goes on every row rather than on all but the
//  first, because the trailing gap is below the last row inside a scroll view, where it is invisible.
Widget* Sidebar::createRow(Dim height, Dim leftPad)
{
  std::string svg = fstring(R"(
    <g class="listitem sb-row" layout="box" box-anchor="hfill">
      <rect class="sb-row-sizer" box-anchor="hfill" width="20" height="%g"/>
      <g class="sb-row-content" layout="flex" flex-direction="row" align-items="center" box-anchor="hfill"
         margin="0 %g 0 %g"></g>
    </g>
  )", height, sbPad, leftPad);
  std::unique_ptr<SvgNode> proto(loadSVGFragment(svg.c_str()));
  Button* row = new Button(proto->clone());
  row->setMargins(0, 0, sbPad, 0);
  return row;
}

void Sidebar::createUI()
{
  // ---- the rounded card ----
  panel = new Widget(new SvgG());
  panel->node->setAttribute("box-anchor", "vfill");
  panel->node->setAttribute("layout", "box");

  // A <g> paints nothing on its own, so the card's fill is an explicit rounded rect behind the
  //  content (the same fix tagdoclist.cpp's sidebar needed).  It is anchored "vfill", not "fill",
  //  because that rect's own *width* is also what gives the panel its width: an explicit width
  //  attribute on the <g> is silently ignored by the box layout (see TagDocList::createFab), and
  //  without it the panel collapsed to the natural width of the actions row's four toolbuttons.
  //  prepareLayout() reports a widget's size only for the dimensions that are *not* fill, so a
  //  vfill rect still contributes its declared width to the box container while filling vertically.
  //  Verified by measurement: the tool buttons render the design's 64 units at 24.2 screen px, and
  //  at that scale the design's 372-wide sidebar comes out at 141 px, which is what this produces.
  SvgRect* panelBg = new SvgRect(Rect::wh(sbWidth, 20), sbCorner, sbCorner);
  panelBg->addClass("panel-bg");
  panelBg->setAttribute("box-anchor", "vfill");
  panel->addWidget(new Widget(panelBg));

  Widget* panelContent = new Widget(new SvgG());
  panelContent->node->setAttribute("box-anchor", "fill");
  panelContent->node->setAttribute("layout", "flex");
  panelContent->node->setAttribute("flex-direction", "column");
  panelContent->setMargins(sbPad, sbPad, sbPad, sbPad);
  panel->addWidget(panelContent);

  // ---- view selector: [list icon] Outline/Layers [chevron] ----
  std::string selSvg = fstring(R"(
    <g class="sb-view-selector" layout="box" box-anchor="hfill">
      <rect class="field-bg" box-anchor="hfill" width="20" height="%g" rx="%g" ry="%g"/>
      <g layout="flex" flex-direction="row" align-items="center" box-anchor="hfill" margin="0 %g">
        <g class="sb-view-icon" margin="0 %g 0 0"></g>
        <text class="sb-view-label" box-anchor="left"></text>
        <g class="spacer" box-anchor="fill" layout="box"></g>
        <g class="sb-view-chevron"></g>
      </g>
    </g>
  )", sbSelectorH, sbCorner, sbCorner, sbPad, sbPad);
  std::unique_ptr<SvgNode> selProto(loadSVGFragment(selSvg.c_str()));
  Button* viewSelector = new Button(selProto->clone());
  SvgUse* viewIcon = new SvgUse(Rect::wh(sbSelectorIcon, sbSelectorIcon), "", SvgGui::useFile("icons/ic_menu_outline.svg"));
  viewIcon->addClass("icon");
  viewSelector->selectFirst(".sb-view-icon")->containerNode()->addChild(viewIcon);
  SvgUse* viewChevron = new SvgUse(Rect::wh(sbSelectorIcon, sbSelectorIcon), "", SvgGui::useFile("icons/chevron_down.svg"));
  viewChevron->addClass("icon");
  viewSelector->selectFirst(".sb-view-chevron")->containerNode()->addChild(viewChevron);
  viewLabel = static_cast<SvgText*>(viewSelector->selectFirst(".sb-view-label")->node);
  viewLabel->setAttr<float>("font-size", sbTitleSize);
  // An arrow popup like every other dropdown in the floating chrome, attached with setupPopupMenu
  //  rather than an onClicked that calls SvgGui::showMenu: it has to be parented to the button it
  //  drops down from before it can be shown, and setupPopupMenu does that (plus the press and
  //  outside-press bookkeeping).  Shown manually it simply never appeared.
  ArrowPopup* viewMenu = createArrowPopup(Menu::VERT_LEFT);
  viewMenu->addItem(_("Outline"), SvgGui::useFile("icons/ic_menu_outline.svg"), [this](){ setView(OUTLINE); });
  viewMenu->addItem(_("Layers"), SvgGui::useFile("icons/ic_menu_pagesel.svg"), [this](){ setView(LAYERS); });
  setupPopupMenu(viewSelector, viewMenu);
  panelContent->addWidget(viewSelector);

  // ---- the list ----
  listView = new Widget(new SvgG());
  listView->node->setAttribute("box-anchor", "hfill");
  listView->node->setAttribute("layout", "flex");
  listView->node->setAttribute("flex-direction", "column");
  listScroll = new ScrollWidget(new SvgDocument(), listView);
  listScroll->node->setAttribute("box-anchor", "fill");
  Widget* listContainer = new Widget(new SvgG());
  listContainer->node->setAttribute("box-anchor", "fill");
  listContainer->node->setAttribute("layout", "box");
  listContainer->setMargins(sbPad, 0, 0, 0);
  listContainer->addWidget(listScroll);
  panelContent->addWidget(listContainer);

  // drag an outline row onto another to nest it there (layers have no nesting)
  outlineDrag.reset(new RowDrag(this));
  outlineDrag->onDrop = [this](int src, int dst){ nestOutline(src, dst); };
  outlineDrag->canDrop = [this](int src, int dst){
    // not onto itself or anything nested under it
    for(size_t ii = 0; ii < shownEntries.size(); ++ii) {
      if(shownEntries[ii].pagenum != src)
        continue;
      for(size_t jj = ii + 1; jj < shownEntries.size() && shownEntries[jj].level > shownEntries[ii].level; ++jj) {
        if(shownEntries[jj].pagenum == dst)
          return false;
      }
    }
    return src != dst;
  };

  // drag a layer row by its grip onto another row to take that row's place in the stack.  moveLayer
  //  works in table indices, which are looked up at the drop: the keys are ids, since a search can
  //  hide rows and the indices of what is shown would not be the table's.
  layerDrag.reset(new RowDrag(this));
  layerDrag->canDrop = [](int src, int dst){ return src != dst && dst != RowDrag::ROOT; };
  layerDrag->onDrop = [this](int src, int dst){
    if(!scribbleDoc)
      return;
    const LayerList& layerList = scribbleDoc->layers();
    int fromIdx = -1, toIdx = -1;
    for(int idx = 0; idx < layerList.size(); ++idx) {
      if(layerList.layers[idx].id == src) fromIdx = idx;
      if(layerList.layers[idx].id == dst) toIdx = idx;
    }
    // erase-then-insert at the target's index puts the layer where the target was, the target moving
    //  one step towards where the dragged layer came from - in either direction
    if(fromIdx >= 0 && toIdx >= 0)
      scribbleDoc->moveLayer(fromIdx, toIdx);
    rebuildList();
  };

  outlineMenu = createArrowPopup(Menu::VERT_LEFT);
  outlineMenu->addItem(_("Rename"), NULL, [this](){ renameOutline(menuPage); });
  // Dragging nests an entry; this is the way back out, since there is no row to drop on for "none"
  outlineTopLevelItem = outlineMenu->addItem(_("Move to Top Level"), NULL, [this](){
    closeRowMenus();
    nestOutline(menuPage, RowDrag::ROOT);
  });
  outlineMenu->addItem(_("Delete"), NULL, [this](){
    closeRowMenus();
    // removes the entry only; the page and everything on it stay
    if(scribbleDoc)
      scribbleDoc->setPageOutline(menuPage, NULL);
    rebuildList();
  });
  setupAutoClosePopup(outlineMenu);

  layerMenu = createArrowPopup(Menu::VERT_LEFT);
  layerMenu->addItem(_("Rename"), NULL, [this](){ renameLayer(menuLayer); });
  layerDeleteItem = layerMenu->addItem(_("Delete"), NULL, [this](){
    closeRowMenus();
    // the layer's ink is not deleted with it: removeLayer moves it onto the current layer, undoably
    if(scribbleDoc)
      scribbleDoc->removeLayer(menuLayer);
    rebuildList();
  });
  setupAutoClosePopup(layerMenu);

  // ---- search: hidden until the search button is pressed, exactly like TagDocList::toggleDocSearch ----
  std::string searchSvg = fstring(R"(
    <g class="sb-search-row" layout="box" box-anchor="hfill" margin="%g 0 0 0">
      <rect class="field-bg" box-anchor="hfill" width="20" height="%g" rx="%g" ry="%g"/>
      <g class="sb-search-box" layout="box" box-anchor="hfill" margin="0 %g"></g>
    </g>
  )", sbPad, sbSearchH, sbCorner, sbCorner, sbPad);
  std::unique_ptr<SvgNode> searchProto(loadSVGFragment(searchSvg.c_str()));
  searchRow = new Widget(searchProto->clone());
  searchEdit = createTextEdit(-1);  // -1 = hfill, so it fills the row
  searchEdit->onChanged = [this](const char* s){ searchQuery = s; rebuildList(); };
  searchRow->selectFirst(".sb-search-box")->addWidget(searchEdit);
  searchRow->setVisible(false);
  panelContent->addWidget(searchRow);

  // ---- actions: add, pin, side, search ----
  // The row's height comes from a sizer rect that is a sibling of the flex row, not a child of it:
  //  as a child it was a flex *item*, so space-between spread the four buttons against it and they
  //  ended up bunched to the right of an invisible 20px block instead of across the panel.
  std::string actionsSvg = fstring(R"(
    <g class="sb-actions" layout="box" box-anchor="hfill" margin="%g 0 0 0">
      <rect class="sb-actions-sizer" box-anchor="hfill" width="20" height="%g"/>
      <g class="sb-actions-row" layout="flex" flex-direction="row" align-items="center"
         justify-content="space-between" box-anchor="hfill" margin="0 %g"></g>
    </g>
  )", sbPad, sbActionsH, sbPad);
  std::unique_ptr<SvgNode> actionsProto(loadSVGFragment(actionsSvg.c_str()));
  Widget* actions = new Widget(actionsProto->clone());
  Widget* actionsRow = actions->selectFirst(".sb-actions-row");

  auto addAction = [&](const char* icon, const char* title, const std::function<void()>& cb) {
    Button* btn = createToolbutton(SvgGui::useFile(icon), title);
    static_cast<SvgUse*>(btn->selectFirst(".icon")->node)->setViewport(Rect::wh(sbActionIcon, sbActionIcon));
    // the stock toolbutton background is 36x42, a full toolbar hit target - left at that size the
    //  cells are wider than their icons and space-between spaces the *cells*, so the icons inside
    //  them do not read as evenly spread.  Square cells fix that at the source: the icon's size on
    //  desktop, a full button for touch (all four cells are the same, so the spread stays even).
    Widget* bg = btn->selectFirst(".background");
    if(bg && bg->node->type() == SvgNode::RECT)
      static_cast<SvgRect*>(bg->node)->setRect(Rect::wh(sbActionCell, sbActionCell));
    btn->onClicked = cb;
    actionsRow->addWidget(btn);
    return btn;
  };
  addAction("icons/ic_menu_plus.svg", _("Add"), [this](){ onAdd(); });
  pinBtn = addAction("icons/ic_menu_pin.svg", _("Pin Sidebar"), [this](){ setPinned(!pinned); });
  // the icon names the side the sidebar would move *to*, so it is the opposite of where it is now
  sideBtn = addAction(onLeft ? "icons/ic_menu_sidebar_right.svg" : "icons/ic_menu_sidebar_left.svg",
      _("Move Sidebar"), [this](){ setOnLeft(!onLeft); });
  addAction("icons/ic_menu_search2.svg", _("Search"), [this](){ toggleSearch(); });
  pinBtn->setChecked(pinned);
  panelContent->addWidget(actions);

  // The inset strip around the panel is the sidebar's own bounds, and nothing else paints there:
  //  when the canvas shrinks to make room, its vacated pixels are never repainted, so whatever it
  //  last drew in that column stays on screen (the page-number statusbar was left stranded behind
  //  the panel's left margin).  This rect paints the column in the canvas surround colour, so the
  //  margins read as background instead of as stale canvas.  Pinned only - floating, the sidebar
  //  must stay transparent over the canvas it is covering.
  backFill = createFillRect();  // already a Widget
  backFill->node->addClass("sb-backfill");
  addWidget(backFill);
  addWidget(panel);

  // Escape closes a floating sidebar; the press-outside case is handled by the event filter below.
  addHandler([this](SvgGui* g, SDL_Event* event){
    if(pinned || !isVisible())
      return false;
    if(event->type == SDL_KEYDOWN && !event->key.repeat && event->key.keysym.sym == SDLK_ESCAPE) {
      setOpen(false);
      return true;
    }
    return false;
  });
}

// Dismiss-on-press-outside.
//
// Two mechanisms were tried before this one and both are wrong for a panel like this.  An
// OUTSIDE_PRESSED handler never fires: that event goes only to the widget that captured the press.
// Pushing the sidebar onto SvgGui's menu stack (SvgGui::showMenu takes a plain Widget*) does deliver
// OUTSIDE_MODAL, but it also hands the panel to the menu machinery, which closed it on presses
// *inside* it - so its own buttons stopped working the moment it was floating.
//
// Widget::eventFilter is the right hook: SvgGui walks from the event's target up through its
// ancestors and calls their filters before dispatching, so a filter on #main-container sees every
// press in the window, including the ones the canvas would otherwise swallow.
//
// It returns false rather than true, i.e. the press is *not* eaten.  Swallowing it was tried and
// makes the toolbar feel broken: with the sidebar open, the first click on a toolbar button only
// dismisses the sidebar and the user has to click again.  Letting it through means the button they
// aimed at also does its job.
void Sidebar::installDismissFilter()
{
  Widget* container = mainWindow->selectFirst("#main-container");
  if(!container)
    return;
  container->eventFilter = [this](SvgGui* g, Widget* target, SDL_Event* event){
    if(event->type != SDL_FINGERDOWN || pinned || !isVisible())
      return false;
    if(target && (target == this || target->isDescendantOf(this)))
      return false;
    setOpen(false);
    return false;
  };
}

SvgGui* Sidebar::gui() const
{
  Window* win = window();
  return win ? win->gui() : NULL;
}

void Sidebar::setView(View v)
{
  currView = v;
  ScribbleApp::cfg->set("sidebarView", int(v));
  refresh();
}

// The pin is the only thing that moves the widget between the two parents; everything else about the
//  sidebar is identical in both modes, which is the point of it being one widget (SIDEBAR_SPEC.md §1).
void Sidebar::setPinned(bool pin)
{
  if(pinned == pin)
    return;
  pinned = pin;
  ScribbleApp::cfg->set("sidebarPinned", pin ? 1 : 0);
  if(pinBtn)
    pinBtn->setChecked(pin);
  schedulePlacement();
}

void Sidebar::setOnLeft(bool left)
{
  if(onLeft == left)
    return;
  onLeft = left;
  ScribbleApp::cfg->set("sidebarLeft", left ? 1 : 0);
  if(sideBtn) {
    // the icon names the side it would move to, so it flips with the sidebar
    static_cast<SvgUse*>(sideBtn->selectFirst(".icon")->node)->setTarget(
        SvgGui::useFile(left ? "icons/ic_menu_sidebar_right.svg" : "icons/ic_menu_sidebar_left.svg"));
  }
  // the toolbar button's glyph names the edge the sidebar is docked to, so it flips the other way
  mainWindow->updateSidebarButton();
  schedulePlacement();
}

// Both toggles are pressed *inside* the sidebar, and both have to move it to a different parent.
// Doing that from within the press handler does not work: the press is still being dispatched, the
// pressed widget is a descendant of the node being reparented, and SvgGui's own bookkeeping for the
// release then lands on a tree that no longer matches - observed as the panel vanishing instead of
// changing mode.  A 1 ms timer runs the move on the next turn of the event loop, once the press has
// been fully handled - 1 rather than 0 because SvgGui::setTimer asserts msec > 0.  The timer hangs off MainWindow, not off this widget, because
// applyPlacement() hides the sidebar on its way through and hiding a widget removes its timers.
void Sidebar::schedulePlacement()
{
  // Capture "was it open" now rather than inside the timer, so nothing that happens between the
  //  press and the next event-loop turn can change what we reopen to.
  bool wasOpen = isVisible();
  if(!gui()) {
    applyPlacement(wasOpen);
    return;
  }
  gui()->setTimer(1, mainWindow, [this, wasOpen](){ applyPlacement(wasOpen); return 0; });
}

// Close before reparenting: floating, the widget is on SvgGui's menu stack, and pulling it out from
// under that would leave a stale entry there.  Reopening afterwards puts it into whichever of the
// two presentations it now belongs to.
void Sidebar::applyPlacement(bool wasOpen)
{
  setOpen(false);
  reparent();
  if(wasOpen)
    setOpen(true);
}

void Sidebar::updateInsets()
{
  if(!panel)
    return;
  // Anchored to the window edge, not left to the box layout's default centring: with the two
  //  horizontal margins deliberately unequal (window edge vs canvas gutter) a centred panel splits
  //  the difference and drifts out of line with the toolbar above it.
  panel->node->setAttribute("box-anchor", onLeft ? "left vfill" : "right vfill");
  // below the toolbar, which steps down past the status bar (MainWindow::orientationChanged())
  panel->setMargins(sbTopInset + ScribbleApp::topInset, onLeft ? sbCanvasGutter : sbEdgeInset,
      sbEdgeInset, onLeft ? sbEdgeInset : sbCanvasGutter);
}

void Sidebar::reparent()
{
  removeFromParent();
  // Pinned: a flex sibling of the canvas, so the canvas is given the remaining width and
  //  ScribbleView re-centres the page in it for free.  Floating: the overlay box #main-container,
  //  where the floating toolbars already live, so nothing is resized.
  Widget* flexRow = mainWindow->selectFirst(".sub-window-layout");
  Widget* overlay = mainWindow->selectFirst("#main-container");
  node->setAttribute("box-anchor", pinned ? "vfill" : (onLeft ? "left vfill" : "right vfill"));
  if(backFill)
    backFill->setVisible(pinned);
  // The insets live on the *panel*, not on this widget, and that is load bearing.  ugui margins sit
  //  outside a widget's own rect, so margins here would put the gutter outside the sidebar's bounds
  //  where its backfill cannot reach it - and since nothing else paints a vacated strip, the canvas's
  //  last frame stays visible there (this is what stranded the page-number statusbar beside the
  //  panel).  With zero margins the sidebar owns its whole column and paints all of it.
  //  Same insets in both modes, so the panel lines up with the toolbar whether or not it is pinned.
  setMargins(0);
  updateInsets();
  if(pinned) {
    // first or last child of the flex row decides which side it occupies
    if(onLeft)
      flexRow->containerNode()->addChild(node, flexRow->containerNode()->children().front());
    else
      flexRow->addWidget(this);
  }
  else
    overlay->addWidget(this);
}

// Floating, the sidebar is pushed onto SvgGui's *menu stack*.  That is the toolkit's mechanism for
//  "dismiss when the user presses somewhere else" - SvgGui::showMenu() takes a plain Widget*, not
//  only a Menu.  An OUTSIDE_PRESSED handler does not work here and was tried first: that event is
//  delivered only to the widget that captured the press, so a panel sitting in the tree never sees
//  it and the sidebar simply stayed open.  Pinned, it is part of the layout and must survive every
//  press on the canvas, so it is shown as an ordinary widget instead.
void Sidebar::setOpen(bool open)
{
  if(open == isVisible()) {
    if(open)
      refresh();
    return;
  }
  setVisible(open);
  if(open)
    refresh();
  ScribbleApp::cfg->set("sidebarVisible", open ? 1 : 0);
  mainWindow->updateSidebarButton();
}

void Sidebar::toggleSearch()
{
  bool visible = !searchRow->isVisible();
  searchRow->setVisible(visible);
  if(visible) {
    window()->gui()->setFocused(searchEdit, SvgGui::REASON_TAB);
  }
  else {
    searchQuery.clear();
    searchEdit->setText("");
    rebuildList();
  }
}

bool Sidebar::matchesSearch(const std::string& text) const
{
  if(searchQuery.empty())
    return true;
  return toLower(text).find(toLower(searchQuery)) != std::string::npos;
}

void Sidebar::refresh()
{
  scribbleDoc = ScribbleApp::app ? ScribbleApp::app->activeDoc() : NULL;
  const char* label = currView == OUTLINE ? _("Outline") : _("Layers");
  if(viewLabel)
    viewLabel->setText(label);
  searchEdit->setEmptyText(currView == OUTLINE ? _("Search outlines") : _("Search layers"));
  rebuildList();
}

std::string Sidebar::docState(ScribbleDoc* doc) const
{
  if(!doc)
    return "none";
  std::string state = fstring("%p %d|", (void*)doc, int(currView));
  if(currView == OUTLINE) {
    for(const OutlineEntry& entry : doc->outline(false))
      state += fstring("%d %d %s\n", entry.pagenum, entry.level, entry.title.c_str());
  }
  else
    state += doc->layers().serialize() + fstring("|%d", doc->currentLayer());
  return state;
}

// Nothing else tells the sidebar the document changed: opening or creating another one kept showing
//  the previous document's list, and so did an undo or a peer's edit to the layers or the outline.
void Sidebar::refreshIfChanged()
{
  if(!isVisible())
    return;
  ScribbleDoc* doc = ScribbleApp::app ? ScribbleApp::app->activeDoc() : NULL;
  if(doc != scribbleDoc || docState(doc) != shownState)
    refresh();
}

void Sidebar::rebuildList()
{
  // Before the window is shown there is no SvgGui to delete rows through - the case when the sidebar
  //  is opened from MainWindow's constructor at startup.  shownState stays empty, so the first
  //  refreshIfChanged() after the window is up builds the list instead of leaving it blank.
  if(!window() || !window()->gui() || !listView)
    return;
  // The row menus are parented to the row that opened them and would be deleted with it - and this runs
  //  from their own items (rename, delete), mid-click.  Detached, they are only parentless until the
  //  next show reparents them.  Same fix as TagDocList::rebuildTagTree.
  closeRowMenus();
  outlineMenu->removeFromParent();
  layerMenu->removeFromParent();
  outlineDrag->clear();
  layerDrag->clear();
  window()->gui()->deleteContents(listView);
  // Building the outline loads every page not yet loaded, so the list is complete the moment it is
  //  shown.  With outline(false) here it held only the pages that happened to be loaded, and entries
  //  popped in one by one as scrolling loaded their pages.  That is a one-time cost per document:
  //  after it, outline(true) loads nothing and this is as cheap as the check in refreshIfChanged().
  //  It runs before shownState is taken, since the loading is what changes docState().
  std::vector<OutlineEntry> entries;
  if(scribbleDoc && currView == OUTLINE)
    entries = scribbleDoc->outline(true);
  shownEntries = entries;
  shownState = docState(scribbleDoc);
  if(!scribbleDoc)
    return;
  if(currView == OUTLINE)
    buildOutlineRows(entries);
  else
    buildLayerRows();
}

void Sidebar::buildOutlineRows(const std::vector<OutlineEntry>& entries)
{
  // A row is collapsed by its *parent* being collapsed, so walk with a "hide everything deeper than
  //  this" level rather than testing each entry against the set.
  int hideBelow = -1;
  for(size_t ii = 0; ii < entries.size(); ++ii) {
    const OutlineEntry& entry = entries[ii];
    if(hideBelow >= 0 && entry.level > hideBelow)
      continue;
    hideBelow = -1;
    bool hasChildren = ii + 1 < entries.size() && entries[ii+1].level > entry.level;
    bool collapsed = collapsedPages.count(entry.pagenum) > 0;
    if(hasChildren && collapsed)
      hideBelow = entry.level;
    // searching flattens the list to matches, the same way the tag tree does
    if(!matchesSearch(entry.title))
      continue;

    Widget* row = createRow(sbRowH, sbPad + entry.level*sbIndent);
    Widget* content = row->selectFirst(".sb-row-content");

    SvgText* title = createTextNode(entry.title.c_str());
    title->addClass("sb-title");
    title->setAttr<float>("font-size", sbTitleSize);
    Widget* titleWidget = new Widget(title);
    titleWidget->node->setAttribute("box-anchor", "left");
    content->addWidget(titleWidget);
    content->addWidget(createStretch());

    if(hasChildren && searchQuery.empty()) {
      Button* chevron = createToolbutton(
          SvgGui::useFile(collapsed ? "icons/chevron_down.svg" : "icons/chevron_up.svg"), "");
      static_cast<SvgUse*>(chevron->selectFirst(".icon")->node)->setViewport(Rect::wh(sbIconSize, sbIconSize));
      // the stock toolbutton background is a full toolbar hit target and would set the row's height
      //  (see TagDocList::createTagRow for the measurement); shrink it to the row's own content
      Widget* bg = chevron->selectFirst(".background");
      if(bg && bg->node->type() == SvgNode::RECT)
        static_cast<SvgRect*>(bg->node)->setRect(Rect::wh(sbIconSize, sbIconSize));
      int pagenum = entry.pagenum;
      chevron->onClicked = [this, pagenum](){
        if(collapsedPages.count(pagenum))
          collapsedPages.erase(pagenum);
        else
          collapsedPages.insert(pagenum);
        rebuildList();
      };
      chevron->setMargins(0, sbPad, 0, 0);
      content->addWidget(chevron);
    }

    SvgText* pageNum = createTextNode(fstring("%d", entry.pagenum + 1).c_str());
    pageNum->addClass("sb-sub");
    pageNum->setAttr<float>("font-size", sbSubSize);
    content->addWidget(new Widget(pageNum));

    int pagenum = entry.pagenum;
    static_cast<Button*>(row)->onClicked = [this, pagenum](){
      ScribbleArea* area = ScribbleApp::app ? ScribbleApp::app->activeArea() : NULL;
      if(area)
        area->gotoPage(pagenum);
      // an unpinned sidebar is in the way of the page it just navigated to
      if(!pinned)
        setOpen(false);
    };
    outlineDrag->addRow(static_cast<Button*>(row), pagenum);
    SvgGui::setupRightClick(row, [this, pagenum, row](SvgGui*, Widget*, Point){
      menuPage = pagenum;
      int level = 0;
      for(const OutlineEntry& shown : shownEntries) {
        if(shown.pagenum == pagenum)
          level = shown.level;
      }
      outlineTopLevelItem->setEnabled(level > 0);
      showRowMenu(outlineMenu, row);
    });
    listView->addWidget(row);
  }
}

void Sidebar::buildLayerRows()
{
  const LayerList& layerList = scribbleDoc->layers();
  int currId = scribbleDoc->currentLayer();
  // the table is bottom-first (z-order); a layer panel reads top-down, so walk it backwards
  for(int idx = layerList.size() - 1; idx >= 0; --idx) {
    const LayerInfo& info = layerList.layers[idx];
    if(!matchesSearch(info.name))
      continue;
    Widget* row = createRow(sbLayerRowH, sbPad);
    Widget* content = row->selectFirst(".sb-row-content");

    // The design's preview is a plain gray block; there is no per-layer rendering to put in it (a
    //  layer spans every page), so it stays a block rather than being faked from the current page.
    SvgRect* preview = new SvgRect(Rect::wh(sbPreview, sbPreview));
    preview->addClass("sb-preview");
    Widget* previewWidget = new Widget(preview);
    previewWidget->setMargins(0, sbPad, 0, 0);
    content->addWidget(previewWidget);

    SvgText* name = createTextNode(info.name.empty() ? _("Layer") : info.name.c_str());
    name->addClass("sb-title");
    name->setAttr<float>("font-size", sbSubSize);
    Widget* nameWidget = new Widget(name);
    nameWidget->node->setAttribute("box-anchor", "left");
    content->addWidget(nameWidget);
    content->addWidget(createStretch());

    // Deviation from the design, which draws the lock only on locked rows: an unlocked row would
    //  then carry no way to lock it.  The icon is always present and dimmed when unlocked, so the
    //  locked state still reads exactly as designed.
    Button* lockBtn = createToolbutton(
        SvgGui::useFile(info.locked ? "icons/ic_menu_lock.svg" : "icons/ic_menu_unlock.svg"),
        info.locked ? _("Unlock Layer") : _("Lock Layer"));
    static_cast<SvgUse*>(lockBtn->selectFirst(".icon")->node)->setViewport(Rect::wh(sbIconSize, sbIconSize));
    Widget* lockBg = lockBtn->selectFirst(".background");
    if(lockBg && lockBg->node->type() == SvgNode::RECT)
      static_cast<SvgRect*>(lockBg->node)->setRect(Rect::wh(sbIconSize, sbIconSize));
    if(!info.locked)
      lockBtn->node->addClass("sb-lock-off");
    int layerId = info.id;
    bool locked = info.locked;
    // Deferred like the drop in rowdrag.cpp: locking can let go of the selection, and the refreshUI
    //  that follows rebuilds this list - deleting the button whose handler is still running.
    lockBtn->onClicked = [this, layerId, locked](){
      gui()->setTimer(1, this, [this, layerId, locked](){
        scribbleDoc->setLayerLocked(layerId, !locked);
        rebuildList();
        return 0;
      });
    };
    content->addWidget(lockBtn);

    // The reorder grip, at the row's trailing end as on iOS.  A visible handle rather than a gesture on
    //  the row itself: on a tablet the list scrolls vertically, so a vertical drag of the whole row can
    //  only ever scroll it (rowdrag.h).  The grip is pressed instead of the row, so it never picks the
    //  layer, and with a mouse it still needs DRAG_START_DIST of travel before it lifts anything.
    Button* grip = createToolbutton(SvgGui::useFile("icons/ic_menu_reorder.svg"), _("Drag to Reorder"));
    static_cast<SvgUse*>(grip->selectFirst(".icon")->node)->setViewport(Rect::wh(sbIconSize, sbIconSize));
    Widget* gripBg = grip->selectFirst(".background");
    if(gripBg && gripBg->node->type() == SvgNode::RECT)
      static_cast<SvgRect*>(gripBg->node)->setRect(Rect::wh(sbGripCell, sbGripCell));
    grip->node->addClass("sb-grip");
    grip->setMargins(0, 0, 0, floatTouchUI ? 0 : sbPad);
    content->addWidget(grip);
    if(layerList.size() > 1)
      layerDrag->addRow(static_cast<Button*>(row), layerId, grip);
    else
      grip->setVisible(false);  // nothing to reorder against

    if(info.id == currId)
      row->node->addClass("checked");
    // a locked layer is still picked here: the lock guards it from edits made on other layers, and
    //  making it current is how it is edited (LayerList::isEditable)
    // deferred for the same reason: leaving a locked layer clears the selection
    static_cast<Button*>(row)->onClicked = [this, layerId](){
      gui()->setTimer(1, this, [this, layerId](){
        scribbleDoc->setCurrentLayer(layerId);
        rebuildList();
        return 0;
      });
    };
    SvgGui::setupRightClick(row, [this, layerId, row](SvgGui*, Widget*, Point){
      menuLayer = layerId;
      // a document always keeps one layer (ScribbleDoc::removeLayer refuses the last)
      layerDeleteItem->setEnabled(scribbleDoc && scribbleDoc->layers().size() > 1);
      showRowMenu(layerMenu, row);
    });
    listView->addWidget(row);
  }
}

void Sidebar::onAdd()
{
  if(!scribbleDoc)
    return;
  if(currView == LAYERS) {
    scribbleDoc->addLayer();
    rebuildList();
  }
  else {
    ScribbleArea* area = ScribbleApp::app ? ScribbleApp::app->activeArea() : NULL;
    if(!area)
      return;
    // the outline entry names the page it is on, so the only sensible default is the current page
    scribbleDoc->setPageOutline(area->getCurrPageNum(), _("Untitled"), 0);
    rebuildList();
  }
}

// An ArrowPopup has no point anchoring - it positions itself against its parent - so it is moved onto
//  the row it is for before each show, exactly as TagDocList::showTagMenu does.
void Sidebar::showRowMenu(ArrowPopup* popup, Widget* row)
{
  closeRowMenus();
  popup->removeFromParent();
  row->addWidget(popup);
  openAutoClosePopup(popup);
}

// A modal dialog does not close a popup still open behind it, which then takes the dialog's first
//  keystroke (see TagDocList::closeAllContextPopups), so this runs before every rename dialog too.
void Sidebar::closeRowMenus()
{
  closeAutoClosePopup(outlineMenu);
  closeAutoClosePopup(layerMenu);
}

void Sidebar::renameOutline(int pagenum)
{
  if(!scribbleDoc)
    return;
  const OutlineEntry* entry = NULL;
  for(const OutlineEntry& shown : shownEntries) {
    if(shown.pagenum == pagenum)
      entry = &shown;
  }
  if(!entry)
    return;
  closeRowMenus();
  TagNameDialog dialog(_("Rename"), entry->title.c_str());
  std::string title = Application::execDialog(&dialog) == Dialog::ACCEPTED ? dialog.getName() : "";
  // an empty title would delete the entry, which is Delete's job, not Rename's.  The stored level is
  //  passed back unchanged rather than the displayed one, which is normalized (Document::outline)
  if(!title.empty() && pagenum < scribbleDoc->document->numPages())
    scribbleDoc->setPageOutline(pagenum, title.c_str(), scribbleDoc->document->pages[pagenum]->outlineLevel);
  rebuildList();
}

void Sidebar::renameLayer(int layerId)
{
  if(!scribbleDoc)
    return;
  const LayerInfo* info = scribbleDoc->layers().find(layerId);
  if(!info)
    return;
  closeRowMenus();
  TagNameDialog dialog(_("Rename Layer"), info->name.empty() ? _("Layer") : info->name.c_str());
  std::string name = Application::execDialog(&dialog) == Dialog::ACCEPTED ? dialog.getName() : "";
  if(!name.empty())
    scribbleDoc->setLayerName(layerId, name.c_str());
  rebuildList();
}

void Sidebar::nestOutline(int src, int dst)
{
  if(!scribbleDoc)
    return;
  // dropped on its own parent = take it back out of that parent, the way to undo a nesting by dragging
  int parentPage = -1;
  for(size_t ii = 0; ii < shownEntries.size(); ++ii) {
    if(shownEntries[ii].pagenum != src)
      continue;
    for(size_t jj = ii; jj-- > 0;) {
      if(shownEntries[jj].level < shownEntries[ii].level) {
        parentPage = shownEntries[jj].pagenum;
        break;
      }
    }
  }
  int target = dst == RowDrag::ROOT ? -1 : (dst == parentPage ? ScribbleDoc::OUTLINE_OUTDENT : dst);
  if(scribbleDoc->nestOutlineEntry(src, target)) {
    // Collapsed rows are remembered by page number, and the pages have just moved; the new parent in
    //  particular must not be collapsed over the entry that was dropped on it, where it would vanish.
    collapsedPages.clear();
  }
  rebuildList();
}
