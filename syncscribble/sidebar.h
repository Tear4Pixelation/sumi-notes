#pragma once

// The general-purpose sidebar (SIDEBAR_SPEC.md): one panel with two views - the document's outline
//  (table of contents) and its layer table - over the backends in document.h/layers.h.
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
#include <set>
#include <string>
#include <vector>

class ScribbleDoc;
class MainWindow;

class Sidebar : public Widget
{
public:
  enum View { OUTLINE = 0, LAYERS };

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
  bool isOpen() const { return isVisible(); }
  void toggleOpen() { setOpen(!isOpen()); }

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
  Widget* createRow(Dim height, Dim leftPad);
  void toggleSearch();
  void onAdd();
  // true if `text` survives the current search query
  bool matchesSearch(const std::string& text) const;
  // a signature of the document state the list is built from, for refreshIfChanged
  std::string docState(ScribbleDoc* doc) const;

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
  Button* pinBtn = NULL;
  Button* sideBtn = NULL;
  std::string searchQuery;
  // docState() of what the list was last built from; empty until it has actually been built
  std::string shownState;

  // Outline rows with children can be collapsed.  Keyed by page number, which is what the outline
  //  itself is keyed by - it is ephemeral view state, so an entry whose page moved simply comes back
  //  expanded rather than following the wrong row.
  std::set<int> collapsedPages;
};
