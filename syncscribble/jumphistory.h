#pragma once

// Back / forward history of *jumps* through a document (docs/agent/navigation.md, "Jump history").
// Pure data and logic - no document, no GL - so scribbletest/jumphistorytest.cpp can run it on its own.
//
// Browser model: `backStack` holds where we were before each recorded jump, `forwardStack` holds what
// back() left behind.  A new jump empties the forward stack.  Only jumps that skip at least
// MIN_JUMP_PAGES pages are recorded, so ordinary scrolling and page-by-page paging never pile up entries.

#include <vector>
#include <cstdlib>

struct JumpLocation
{
  int pagenum = 0;
  double x = 0;  // position of the viewport corner in the page's own units (Dim)
  double y = 0;
};

class JumpHistory
{
public:
  static constexpr int MIN_JUMP_PAGES = 2;

  // `from` is where the view is now, destPage the page about to be shown.  Returns true if recorded.
  bool recordJump(const JumpLocation& from, int destPage)
  {
    if(std::abs(destPage - from.pagenum) < MIN_JUMP_PAGES)
      return false;
    backStack.push_back(from);
    forwardStack.clear();
    return true;
  }

  bool canBack() const { return !backStack.empty(); }
  bool canForward() const { return !forwardStack.empty(); }

  // `current` is where the view is now; it becomes the forward target.  On false nothing changed.
  bool back(const JumpLocation& current, JumpLocation* target)
  {
    if(backStack.empty())
      return false;
    *target = backStack.back();
    backStack.pop_back();
    forwardStack.push_back(current);
    return true;
  }

  bool forward(const JumpLocation& current, JumpLocation* target)
  {
    if(forwardStack.empty())
      return false;
    *target = forwardStack.back();
    forwardStack.pop_back();
    backStack.push_back(current);
    return true;
  }

  void clear() { backStack.clear(); forwardStack.clear(); }

private:
  std::vector<JumpLocation> backStack;
  std::vector<JumpLocation> forwardStack;
};
