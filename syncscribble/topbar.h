#pragma once

// The top tab bar (docs/agent/top-tab-bar.md): a full-width strip along the top of the window with the
//  library (home) button, one tab per open document (ScribbleApp::tabs), and - in the editor - the View
//  popup and the overflow menu.  The same widget is built twice: once by MainWindow above the canvas,
//  and once by TagDocList above the library, which is a modal window of its own.  The owner decides what
//  a press does (onHome/onTab/onCloseTab); the bar only reflects ScribbleApp's tab list.  Designed in the
//  Penpot file "Tab Topbar"; every size below is that board's, 1:1, like the library's.

#include "ugui/widgets.h"
#include "basics.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

class AutoAdjContainer;
class ArrowPopup;
class RowDrag;

class TopBar : public Widget
{
public:
  // the topTabBar setting
  enum Mode { OFF = 0, LIBRARY_ONLY = 1, LIBRARY_AND_WRITING = 2 };

  // inLibrary: the library's bar - home is the active "tab", and there is no overflow button
  explicit TopBar(bool inLibrary);
  ~TopBar() override;

  // rebuilds the tab buttons if the tabs, the active one or a title changed since last time (always with
  //  force); cheap enough to call from every MainWindow::refreshUI().  Once the window is shown, the
  //  rebuild happens on a 1 ms timer, never inside the layout pass that may be calling this
  void refresh(bool force = false);
  // room above the bar's contents, painted in the bar's color: the status bar inset (ScribbleApp::topInset)
  void setTopPadding(Dim pad);
  Dim topPadding() const { return topPad; }
  // tabs currently built (ScribbleTest)
  int tabCount() const { return int(cells.size()); }

  Button* homeBtn = NULL;
  Button* viewBtn = NULL;
  Button* overflowBtn = NULL;  // the editor's bar only

  std::function<void()> onHome;
  std::function<void(const std::string& path)> onTab;
  std::function<void(const std::string& path)> onCloseTab;
  // The editor's bar only: a tab dragged along the bar onto another (reorder), or off the bar - pos in
  //  window coordinates, NaN once it is back over the bar - and released there (onto the canvas: a split).
  //  Indices are into ScribbleApp::tabs.  The drop callbacks run from a timer; defer anything modal.
  std::function<void(int from, int to)> onMoveTab;
  std::function<void(int idx, Point pos)> onDragTabOutside;
  std::function<void(int idx, Point pos)> onDropTabOutside;

  // the button of the tab for `path`, NULL if it has none (a popup anchors on it: hostPopup)
  Button* tabButton(const std::string& path) const;
  // Parents `popup` to the tab for `path`, so it points at that tab; false if there is none.  A rebuild
  //  closes and detaches every hosted popup before deleting the tabs, so the popup outlives them.
  bool hostPopup(ArrowPopup* popup, const std::string& path);

  // the bar's CSS, added to the window stylesheet by createStylesheet() (resources.cpp)
  static const char* stylesheet;

private:
  struct TabCell
  {
    Button* btn;
    SvgRect* bg;  // also what sizes the tab (fitTabs)
    SvgText* title;
    std::string fullTitle;
    bool active;
  };

  Button* createIconButton(const char* icon, Dim iconSize, const char* tooltip);
  TabCell createTab(const std::string& path, bool active);
  std::string tabState() const;
  void rebuild(bool force);
  // tabs share the strip's width equally, up to the design's tab width; titles are elided to fit
  void fitTabs(Dim width);

  bool inLibrary;
  SvgRect* bg = NULL;
  Widget* row = NULL;
  Widget* tabsRow = NULL;
  AutoAdjContainer* tabsAdj = NULL;
  std::vector<TabCell> cells;
  std::vector<ArrowPopup*> hostedPopups;
  std::unique_ptr<RowDrag> tabDrag;  // editor only
  std::string shownState;
  bool forcePending = false;
  Dim topPad = 0;
};
