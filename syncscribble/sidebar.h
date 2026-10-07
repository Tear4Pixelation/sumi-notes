#pragma once

// The general-purpose sidebar (SIDEBAR_SPEC.md): one panel with three views - the document's outline
//  (table of contents), its layer table, and its pages as a thumbnail grid (docs/agent/page-management.md)
//  - over the backends in document.h/layers.h/scribbledoc.h.
//
// It exists in two presentations, and they are deliberately ONE widget in two parents rather than
//  two widgets:
//   - pinned: a flex sibling of the canvas inside .sub-window-layout, so it takes width from the
//     canvas and ScribbleView re-centres the page in what is left, with no code here at all;
//   - floating: a child of #main-container's box layout, anchored left or right, so it covers the
//     canvas without resizing it, and closes on an outside press.
//  Toggling the pin reparents the widget; the scroll position, the search query and the active view
//  survive because the widget itself does.  Which side it sits on is the same mechanism again: which
//  end of the flex row, and which box-anchor when floating.

#include "ugui/widgets.h"
#include "ugui/textedit.h"
#include "document.h"
#include "rowdrag.h"
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

class ScribbleDoc;
class MainWindow;

class Sidebar : public Widget
{
public:
  // TABS: the open documents (editor tabs, sidebartabs.cpp)
  enum View { OUTLINE = 0, LAYERS, PAGES, TABS };

  Sidebar(MainWindow* mw);

  // rebuild the list from the active document
  void refresh();
  // refresh() only if what the list shows has changed (another document, an undo, a peer's edit); cheap
  //  enough for MainWindow::refreshUI, which runs after every stroke
  void refreshIfChanged();
  void setView(View v);
  View view() const { return currView; }

  // pinned = flex sibling of the canvas; unpinned = overlay that closes on an outside press
  void setPinned(bool pin);
  bool isPinned() const { return pinned; }
  // which edge of the window the sidebar occupies
  void setOnLeft(bool left);
  bool isOnLeft() const { return onLeft; }

  void setOpen(bool open);
  // Open in view `v` *for now*: the stored view, pin and open state are left as they are, and the next
  //  close puts them back (the toolbar's Tabs button).  Called again while showing temporarily, it
  //  ends it.  Floating if the sidebar was closed, so the canvas is not resized for a glance.
  void showTemporary(View v);
  bool isTemporary() const { return temporary; }
  bool isOpen() const { return isVisible(); }
  void toggleOpen() { setOpen(!isOpen()); }
  // reapply the panel's margins, e.g. after ScribbleApp::topInset changed
  void updateInsets();

private:
  void createUI();
  // move this widget between the flex row (pinned) and the overlay stack (floating); the one place
  //  that knows about either parent
  void reparent();
  // run the pinned/side change on the next event-loop turn (see sidebar.cpp)
  void schedulePlacement();
  void applyPlacement(bool wasOpen);
  // press-outside dismissal for the floating mode; see sidebar.cpp
  void installDismissFilter();
  // this widget's SvgGui, or NULL before it has been parented into a window
  SvgGui* gui() const;
  void rebuildList();
  void buildOutlineRows(const std::vector<OutlineEntry>& entries);
  void buildLayerRows();
  // the Tabs view (sidebartabs.cpp): one row per open document, and the drag that reorders them or
  //  drops one onto the canvas
  void buildTabRows();
  void setupTabDrag();
  void tabChosen();
  // leave temporary mode: the stored view and pin come back, and it closes unless it was open before
  void endTemporary();
  Widget* createRow(Dim height, Dim leftPad);
  void toggleSearch();
  void onAdd();
  // true if `text` survives the current search query
  bool matchesSearch(const std::string& text) const;
  // right-click / long press on a row
  void showRowMenu(ArrowPopup* popup, Widget* row);
  void closeRowMenus();
  void renameOutline(int pagenum);
  void renameLayer(int layerId);
  // drop of the outline row on page `src` onto the row on page `dst`, or RowDrag::ROOT
  void nestOutline(int src, int dst);

  // ---- Pages view (docs/agent/page-management.md) ----
  void buildPageGrid();
  Widget* createPageSelectBar();
  void setPageSelectMode(bool on);
  void togglePageSelected(int pagenum);
  void updatePageSelectBar();
  // the selected pages' current numbers, ascending
  std::vector<int> selectedPageNums() const;
  // drop of thumbnail `src` on thumbnail `dst`; zone 1 = the leading half of the first page, i.e. before it
  void dropPages(int src, int dst, int zone);
  void deletePages(const std::vector<int>& pages);
  void exportPages(int kind, const std::vector<int>& pages);
  // the current page's mark, restyled in place
  void updateCurrentPage();
  // keep rendering thumbnails near the visible part of the grid, a few per event-loop turn
  void ensureThumbTimer();
  // renders thumbnails until a small time budget is spent; the timer's next period, 0 when none is left
  int renderThumbnails();
  void showThumbnail(size_t cellIdx, const Image& image);
  // a signature of the page order, and of what every thumbnail is drawn from, for refreshIfChanged
  std::string pageOrderState(ScribbleDoc* doc) const;
  // the layer table and its hidden flags, which every thumbnail depends on
  std::string thumbnailInputs(ScribbleDoc* doc) const;
  // a signature of the document state the list is built from, for refreshIfChanged
  std::string docState(ScribbleDoc* doc) const;
  // the same for the Tabs view (sidebartabs.cpp)
  std::string tabsState() const;

  MainWindow* mainWindow;
  ScribbleDoc* scribbleDoc = NULL;

  View currView = OUTLINE;
  bool pinned = true;
  bool onLeft = true;

  Widget* backFill = NULL;    // paints the pinned column so its margins are not stale canvas
  Widget* panel = NULL;          // the rounded #101010 card; this widget is only its positioning box
  Widget* listView = NULL;       // flex column the rows are built into
  ScrollWidget* listScroll = NULL;
  Widget* searchRow = NULL;
  TextEdit* searchEdit = NULL;
  SvgText* viewLabel = NULL;
  SvgUse* viewIcon = NULL;  // the selector shows the current view's menu icon
  Button* pinBtn = NULL;
  Button* sideBtn = NULL;
  std::string searchQuery;
  // docState() of what the list was last built from; empty until it has actually been built
  std::string shownState;

  // Outline rows with children can be collapsed.  Keyed by page number, which is what the outline
  //  itself is keyed by - it is ephemeral view state, so an entry whose page moved simply comes back
  //  expanded rather than following the wrong row.
  std::set<int> collapsedPages;

  // the outline as last built, for canDrop's "not onto its own descendant"
  std::vector<OutlineEntry> shownEntries;
  std::unique_ptr<RowDrag> outlineDrag;
  // reorders layers; keyed by layer id, picked up by each row's grip (ic_menu_reorder)
  std::unique_ptr<RowDrag> layerDrag;
  // reorders tabs, or drops one on the canvas to split it; keyed by tab index
  std::unique_ptr<RowDrag> tabDrag;

  // showTemporary(): what to put back on close
  bool temporary = false;
  View storedView = OUTLINE;
  bool storedPinned = true;
  bool storedOpen = false;
  // applyPlacement() closes and reopens around a reparent; that close is not the user closing it
  bool placing = false;

  // One persistent popup per row kind, reparented onto the row that opened it (TagDocList::showTagMenu
  //  explains why), and what it was opened for.
  ArrowPopup* outlineMenu = NULL;
  ArrowPopup* layerMenu = NULL;
  Button* outlineTopLevelItem = NULL;
  Button* layerDeleteItem = NULL;
  int menuPage = -1;
  int menuLayer = -1;

  // ---- Pages view ----
  // bottom row buttons that only belong to some views
  Button* addBtn = NULL;
  Button* searchBtn = NULL;
  Button* selectBtn = NULL;
  struct PageCell {
    Button* cell;
    Widget* holder;     // the page-shaped box inside the cell: placeholder or image, rings, badge
    unsigned int uid;   // Page::uid of the page it shows
    unsigned int shownRevision;  // Page::revision its image was rendered from
    bool upToDate;      // false: still the placeholder, or an image of an older state of the page
  };
  std::vector<PageCell> pageCells;
  // rendered thumbnails by Page::uid, so a rebuild (a move, an insert) does not render them all again
  struct Thumbnail { unsigned int revision; Image image; };
  std::unordered_map<unsigned int, Thumbnail> thumbnails;
  std::string shownThumbnailInputs;
  int thumbWidthPx = 0;
  Timer* thumbTimer = NULL;
  int shownCurrPage = -1;
  // selection by Page::uid, so it survives renumbering; pruned to the document at every rebuild
  std::set<unsigned int> selectedPageUids;
  bool pageSelectMode = false;
  std::unique_ptr<RowDrag> pageDrag;
  ArrowPopup* pageMenu = NULL;
  // select mode's floating bar, over the canvas beside the sidebar (it is wider than the panel)
  Widget* pageSelectBar = NULL;
  SvgText* pageSelectCount = NULL;
  std::vector<Button*> pageSelectActions;  // disabled while nothing is selected
};
