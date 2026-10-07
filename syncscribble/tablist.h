#pragma once

// Editor tabs (docs/agent/editor-tabs.md): the list of open documents behind the sidebar's Tabs view.
//
// This is only the list - order, which tab takes over a pane when one is closed, which background tabs
//  have been idle long enough to unload, and the config round trip - with no dependency on the
//  application, so it builds and is tested on its own (scribbletest/tabstest.cpp).  Loading, showing
//  and saving documents is ScribbleApp's, in editortabs.cpp.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class ScribbleDoc;

// Where a tab's pane was looking when the tab was last left: restored when it is shown again, whether
//  its document stayed loaded or was reloaded from disk.  Not persisted across restarts - the file's
//  own saved position covers that.
struct TabViewState
{
  int pagenum = -1;
  double x = 0, y = 0;  // page coordinates of the pane's top left corner (ScribbleArea::getPos)
  double zoom = 0;
  bool isValid() const { return pagenum >= 0 && zoom > 0; }
};

struct EditorTab
{
  std::string path;
  // the loaded document, or NULL while the tab is unloaded; owned by the tab while no pane shows it
  ScribbleDoc* doc = nullptr;
  TabViewState view;
  // when it was last taken out of a pane; the idle unload counts from here
  int64_t lastShownMs = 0;
};

class TabList
{
public:
  std::vector<EditorTab> tabs;

  int size() const { return int(tabs.size()); }
  bool empty() const { return tabs.empty(); }
  EditorTab& operator[](int idx) { return tabs[idx]; }
  const EditorTab& operator[](int idx) const { return tabs[idx]; }

  // index of the tab for this file / this loaded document, -1 if none
  int find(const std::string& path) const;
  int findDoc(const ScribbleDoc* doc) const;
  // inserts right after tab `after`, or at the end when `after` is not a tab; returns the new index
  int insert(const EditorTab& tab, int after = -1);
  void remove(int idx);
  // The tab that takes over a pane when tab `idx` is closed, as an index into the list *after* the
  //  removal: the one after it, or the one before when it was the last; -1 when it was the only tab.
  int neighbourAfterClose(int idx) const;
  // A row dropped on another row: tab `from` takes the place of tab `to`, which moves one step towards
  //  where `from` came from - the same rule as the sidebar's layer rows.  Returns `from`'s new index.
  int move(int from, int to);
  // Loaded tabs whose document has been out of every pane for longer than timeoutMs.  `keep` vetoes a
  //  tab (shown in a pane, unsaved, in a sync session); a timeout <= 0 unloads nothing.
  std::vector<int> idleTabs(int64_t nowMs, int64_t timeoutMs,
      const std::function<bool(const EditorTab&)>& keep) const;

  // the paths in order, joined like recentDocs; parse() is the inverse and drops empty entries
  std::string serialize() const;
  static std::vector<std::string> parse(const char* str);
};

// Which edge of a pane a tab dropped at (x, y) splits it on.  Nearest *relative* to the pane's size,
//  i.e. the pane is cut along its diagonals: on a wide canvas an absolute distance would make nearly
//  every drop a top/bottom split, since the top and bottom edges are so much closer than the sides.
enum class DropEdge { LEFT, RIGHT, TOP, BOTTOM };
DropEdge nearestDropEdge(double left, double top, double width, double height, double x, double y);
