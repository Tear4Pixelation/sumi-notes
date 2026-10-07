#include "tablist.h"

#include <algorithm>

static const char* TAB_SEPARATOR = ":::";  // the separator recentDocs uses, so a path cannot contain it either

int TabList::find(const std::string& path) const
{
  for(size_t ii = 0; ii < tabs.size(); ++ii) {
    if(tabs[ii].path == path)
      return int(ii);
  }
  return -1;
}

int TabList::findDoc(const ScribbleDoc* doc) const
{
  if(!doc)
    return -1;
  for(size_t ii = 0; ii < tabs.size(); ++ii) {
    if(tabs[ii].doc == doc)
      return int(ii);
  }
  return -1;
}

int TabList::insert(const EditorTab& tab, int after)
{
  int where = (after >= 0 && after < size()) ? after + 1 : size();
  tabs.insert(tabs.begin() + where, tab);
  return where;
}

void TabList::remove(int idx)
{
  if(idx >= 0 && idx < size())
    tabs.erase(tabs.begin() + idx);
}

int TabList::neighbourAfterClose(int idx) const
{
  if(idx < 0 || idx >= size() || size() < 2)
    return -1;
  // the tab after it slides into its index; the last tab has none after it, so the one before takes over
  return idx < size() - 1 ? idx : idx - 1;
}

int TabList::move(int from, int to)
{
  if(from < 0 || from >= size() || to < 0 || to >= size() || from == to)
    return from;
  EditorTab moved = tabs[from];
  tabs.erase(tabs.begin() + from);
  tabs.insert(tabs.begin() + to, moved);
  return to;
}

std::vector<int> TabList::idleTabs(int64_t nowMs, int64_t timeoutMs,
    const std::function<bool(const EditorTab&)>& keep) const
{
  std::vector<int> idle;
  if(timeoutMs <= 0)
    return idle;
  for(size_t ii = 0; ii < tabs.size(); ++ii) {
    const EditorTab& tab = tabs[ii];
    if(!tab.doc || nowMs - tab.lastShownMs < timeoutMs)
      continue;
    if(keep && keep(tab))
      continue;
    idle.push_back(int(ii));
  }
  return idle;
}

std::string TabList::serialize() const
{
  std::string joined;
  for(size_t ii = 0; ii < tabs.size(); ++ii) {
    if(ii > 0)
      joined += TAB_SEPARATOR;
    joined += tabs[ii].path;
  }
  return joined;
}

std::vector<std::string> TabList::parse(const char* str)
{
  std::vector<std::string> paths;
  if(!str)
    return paths;
  std::string remaining(str);
  const size_t sepLen = std::char_traits<char>::length(TAB_SEPARATOR);
  size_t start = 0;
  while(start <= remaining.size()) {
    size_t end = remaining.find(TAB_SEPARATOR, start);
    if(end == std::string::npos)
      end = remaining.size();
    if(end > start)
      paths.push_back(remaining.substr(start, end - start));
    start = end + sepLen;
  }
  return paths;
}

DropEdge nearestDropEdge(double left, double top, double width, double height, double x, double y)
{
  if(width <= 0 || height <= 0)
    return DropEdge::RIGHT;
  double fromLeft = (x - left)/width;
  double fromTop = (y - top)/height;
  double distances[] = {fromLeft, 1 - fromLeft, fromTop, 1 - fromTop};
  int nearest = int(std::min_element(std::begin(distances), std::end(distances)) - std::begin(distances));
  static const DropEdge edges[] = {DropEdge::LEFT, DropEdge::RIGHT, DropEdge::TOP, DropEdge::BOTTOM};
  return edges[nearest];
}
