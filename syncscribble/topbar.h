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
#include <string>
#include <vector>

class AutoAdjContainer;

class TopBar : public Widget
{
public:
  // the topTabBar setting
  enum Mode { OFF = 0, LIBRARY_ONLY = 1, LIBRARY_AND_WRITING = 2 };

  // inLibrary: the library's bar - home is the active "tab", and there is no overflow button
  explicit TopBar(bool inLibrary);

  // rebuilds the tab buttons if the tabs, the active one or a title changed since last time (always with
  //  force); cheap enough to call from every MainWindow::refreshUI()
  void refresh(bool force = false);
  // room above the bar's contents, painted in the bar's color: the status bar inset (ScribbleApp::topInset)
  void setTopPadding(Dim pad);

  Button* homeBtn = NULL;
  Button* viewBtn = NULL;
  Button* overflowBtn = NULL;  // the editor's bar only

  std::function<void()> onHome;
  std::function<void(const std::string& path)> onTab;
  std::function<void(const std::string& path)> onCloseTab;

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

  Button* createIconButton(const char* icon, Dim hitWidth, Dim iconSize, const char* tooltip);
  TabCell createTab(const std::string& path, bool active);
  // tabs share the strip's width equally, up to the design's tab width; titles are elided to fit
  void fitTabs(Dim width);

  bool inLibrary;
  SvgRect* bg = NULL;
  Widget* row = NULL;
  Widget* tabsRow = NULL;
  AutoAdjContainer* tabsAdj = NULL;
  std::vector<TabCell> cells;
  std::string shownState;
};
