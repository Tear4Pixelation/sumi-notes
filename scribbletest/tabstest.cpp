// Unit tests for the editor tab list in syncscribble/tablist.cpp (docs/agent/editor-tabs.md).
// Like layertest.cpp these need no GL context and no document, so they also build and run on their own:
//
//   g++ -std=c++14 -O2 -DNDEBUG -I . -I syncscribble -DTABSTEST_MAIN
//       scribbletest/tabstest.cpp syncscribble/tablist.cpp -o tabstest && ./tabstest
//   (one command, run from the repo root.)
//
// runTabTests() returns the number of failed checks and is also called from ScribbleTest::runAll().
// Showing, saving and reloading the documents behind the tabs needs the application and is driven in
//  the running app (see the doc).

#include <stdio.h>

#include "tablist.h"

static int nTabChecksFailed = 0;

static void tabCheckTrue(bool condition, const char* what)
{
  if(!condition) {
    ++nTabChecksFailed;
    printf("FAIL: %s\n", what);
  }
}

static TabList tabListOf(std::initializer_list<const char*> paths)
{
  TabList list;
  for(const char* path : paths) {
    EditorTab tab;
    tab.path = path;
    list.tabs.push_back(tab);
  }
  return list;
}

static std::string order(const TabList& list)
{
  std::string names;
  for(const EditorTab& tab : list.tabs)
    names += tab.path;
  return names;
}

// Closing the active tab hands its pane to a neighbour: the one after it, so the pane shows what was
//  next in line, unless it was the last tab.  Index is into the list after the removal.
static void testCloseNeighbour()
{
  TabList list = tabListOf({"A", "B", "C"});
  tabCheckTrue(list.neighbourAfterClose(1) == 1, "closing a middle tab picks the next one (now at its index)");
  tabCheckTrue(list.neighbourAfterClose(0) == 0, "closing the first tab picks the second");
  tabCheckTrue(list.neighbourAfterClose(2) == 1, "closing the last tab picks the one before it");
  list.remove(1);
  int neighbour = list.neighbourAfterClose(0);
  list.remove(0);
  tabCheckTrue(neighbour == 0 && list[neighbour].path == "C", "after removing B, closing A goes to C");
  TabList single = tabListOf({"A"});
  tabCheckTrue(single.neighbourAfterClose(0) == -1, "closing the only tab leaves no neighbour");
  tabCheckTrue(single.neighbourAfterClose(5) == -1, "an index out of range has no neighbour");
}

// A dropped row takes the target row's place, the target shifting towards where the dragged row came
//  from - in both directions, as the layer rows do.
static void testMove()
{
  TabList down = tabListOf({"A", "B", "C", "D"});
  int newIdx = down.move(0, 2);
  tabCheckTrue(order(down) == "BCAD" && newIdx == 2, "dragging A onto C puts A at C's place, C moves up");
  TabList up = tabListOf({"A", "B", "C", "D"});
  newIdx = up.move(3, 1);
  tabCheckTrue(order(up) == "ADBC" && newIdx == 1, "dragging D onto B puts D at B's place, B moves down");
  TabList same = tabListOf({"A", "B"});
  same.move(1, 1);
  same.move(0, 7);
  tabCheckTrue(order(same) == "AB", "onto itself or out of range changes nothing");
}

// A new tab opens right after the one that was active, or at the end when no tab was
static void testInsertAndFind()
{
  TabList list = tabListOf({"A", "B", "C"});
  EditorTab tab;
  tab.path = "N";
  tabCheckTrue(list.insert(tab, 0) == 1 && order(list) == "ANBC", "insert after A lands at index 1");
  tab.path = "M";
  tabCheckTrue(list.insert(tab, -1) == 4 && order(list) == "ANBCM", "no anchor appends");
  tab.path = "O";
  tabCheckTrue(list.insert(tab, 99) == 5, "an anchor past the end appends");
  tabCheckTrue(list.find("C") == 3 && list.find("Z") == -1, "find by path");
  ScribbleDoc* fakeDoc = reinterpret_cast<ScribbleDoc*>(&list);  // never dereferenced
  list[2].doc = fakeDoc;
  tabCheckTrue(list.findDoc(fakeDoc) == 2 && list.findDoc(nullptr) == -1, "find by loaded document, never by NULL");
}

// Only a loaded tab idle past the timeout is unloaded; the veto (shown, unsaved, synced) wins; and a
//  zero timeout means never.
static void testIdle()
{
  TabList list = tabListOf({"loadedOld", "loadedNew", "unloaded", "vetoed"});
  ScribbleDoc* fakeDoc = reinterpret_cast<ScribbleDoc*>(&list);
  list[0].doc = fakeDoc;  list[0].lastShownMs = 1000;
  list[1].doc = fakeDoc;  list[1].lastShownMs = 9000;
  list[2].lastShownMs = 0;
  list[3].doc = fakeDoc;  list[3].lastShownMs = 0;
  auto keep = [](const EditorTab& tab){ return tab.path == "vetoed"; };
  std::vector<int> idle = list.idleTabs(10000, 5000, keep);
  tabCheckTrue(idle.size() == 1 && idle[0] == 0, "only the loaded tab idle past the timeout is unloaded");
  tabCheckTrue(list.idleTabs(10000, 0, keep).empty(), "a zero timeout never unloads");
  tabCheckTrue(list.idleTabs(10000, 1000, keep).size() == 2, "a short timeout catches both loaded unvetoed tabs");
}

static void testSerialize()
{
  TabList list = tabListOf({"/a/one.svgz", "/b/two words.svgz", "/c/three.html"});
  std::vector<std::string> paths = TabList::parse(list.serialize().c_str());
  tabCheckTrue(paths.size() == 3 && paths[1] == "/b/two words.svgz" && paths[2] == "/c/three.html",
      "the tab list survives the config round trip");
  tabCheckTrue(TabList::parse("").empty() && TabList::parse(nullptr).empty(), "an empty or missing entry has no tabs");
  tabCheckTrue(TabList::parse(":::/x.svgz::::::/y.svgz:::").size() == 2, "empty entries are dropped");
}

// The edge a dropped tab splits the pane on.  The last two checks are where nearest-relative and
//  nearest-absolute disagree on a wide pane, which is the point of measuring relative to its size.
static void testDropEdge()
{
  // a 1000 x 500 pane at (100, 50)
  tabCheckTrue(nearestDropEdge(100, 50, 1000, 500, 120, 300) == DropEdge::LEFT, "near the left edge");
  tabCheckTrue(nearestDropEdge(100, 50, 1000, 500, 1080, 300) == DropEdge::RIGHT, "near the right edge");
  tabCheckTrue(nearestDropEdge(100, 50, 1000, 500, 600, 60) == DropEdge::TOP, "near the top edge");
  tabCheckTrue(nearestDropEdge(100, 50, 1000, 500, 600, 540) == DropEdge::BOTTOM, "near the bottom edge");
  // 150 from the left (0.15 of the width) and 100 from the top (0.2 of the height): absolutely the
  //  top is nearer, relatively the left is
  tabCheckTrue(nearestDropEdge(100, 50, 1000, 500, 250, 150) == DropEdge::LEFT,
      "relative distance: a point a sixth of the way in from the left splits left on a wide pane");
  tabCheckTrue(nearestDropEdge(100, 50, 1000, 500, 950, 450) == DropEdge::RIGHT,
      "relative distance: the same on the right, not the bottom");
}

int runTabTests()
{
  nTabChecksFailed = 0;
  testCloseNeighbour();
  testMove();
  testInsertAndFind();
  testIdle();
  testSerialize();
  testDropEdge();
  return nTabChecksFailed;
}

#ifdef TABSTEST_MAIN
int main()
{
  int nFailed = runTabTests();
  printf(nFailed ? "%d tab checks FAILED\n" : "All tab checks passed\n", nFailed);
  return nFailed;
}
#endif
