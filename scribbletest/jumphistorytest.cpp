// Unit tests for JumpHistory (syncscribble/jumphistory.h, docs/agent/navigation.md "Jump history").  No GL
//  context and no document needed, so it also builds and runs on its own:
//
//   g++ -std=c++14 -O2 -I syncscribble -DJUMPHISTORYTEST_MAIN scribbletest/jumphistorytest.cpp
//       -o jumphistorytest && ./jumphistorytest
//   (one command, run from the repo root.)
//
// runJumpHistoryTests() returns the number of failed checks and is also called from ScribbleTest::runAll().
// What needs the real view - the buttons' enabled state and the scroll position coming back - is checked
//  by hand in agent-display; see navigation.md.

#include <stdio.h>

#include "jumphistory.h"

static int nJumpHistoryChecksFailed = 0;

static void jumpHistoryCheck(bool condition, const char* what)
{
  if(!condition) {
    ++nJumpHistoryChecksFailed;
    printf("FAIL: %s\n", what);
  }
}

static JumpLocation jumpAt(int pagenum, double y = 0)
{
  JumpLocation location;
  location.pagenum = pagenum;
  location.y = y;
  return location;
}

int runJumpHistoryTests()
{
  nJumpHistoryChecksFailed = 0;
  JumpLocation target;

  // short moves are not jumps
  {
    JumpHistory history;
    jumpHistoryCheck(!history.recordJump(jumpAt(3), 3), "same page is not recorded");
    jumpHistoryCheck(!history.recordJump(jumpAt(3), 4), "one page forward is not recorded");
    jumpHistoryCheck(!history.recordJump(jumpAt(3), 2), "one page back is not recorded");
    jumpHistoryCheck(!history.canBack(), "nothing to go back to after short moves");
    jumpHistoryCheck(!history.back(jumpAt(3), &target), "back() with empty history fails");
    jumpHistoryCheck(!history.forward(jumpAt(3), &target), "forward() with empty history fails");
  }

  // back returns to the spot we left, with its scroll position; forward redoes
  {
    JumpHistory history;
    jumpHistoryCheck(history.recordJump(jumpAt(1, 123), 9), "a jump of eight pages is recorded");
    jumpHistoryCheck(history.canBack() && !history.canForward(), "after a jump: back yes, forward no");
    jumpHistoryCheck(history.back(jumpAt(9, 5), &target), "back succeeds");
    jumpHistoryCheck(target.pagenum == 1 && target.y == 123, "back returns the position before the jump");
    jumpHistoryCheck(!history.canBack() && history.canForward(), "after back: forward yes, back no");
    jumpHistoryCheck(history.forward(jumpAt(1, 123), &target), "forward succeeds");
    jumpHistoryCheck(target.pagenum == 9 && target.y == 5, "forward returns the position back() left");
    jumpHistoryCheck(history.canBack() && !history.canForward(), "after forward: back yes, forward no");
  }

  // a new jump after going back truncates the forward history
  {
    JumpHistory history;
    history.recordJump(jumpAt(0), 10);
    history.recordJump(jumpAt(10), 20);
    history.back(jumpAt(20), &target);  // now at 10, forward holds 20
    jumpHistoryCheck(history.canForward(), "forward available after back");
    jumpHistoryCheck(history.recordJump(jumpAt(10), 30), "new jump after back is recorded");
    jumpHistoryCheck(!history.canForward(), "new jump truncates forward history");
    history.back(jumpAt(30), &target);
    jumpHistoryCheck(target.pagenum == 10, "back after truncating goes to the position the new jump left");
    history.back(jumpAt(10), &target);
    jumpHistoryCheck(target.pagenum == 0, "and then to the first one");
    jumpHistoryCheck(!history.canBack(), "stack exhausted");
  }

  // clear() forgets everything (a different document is opened)
  {
    JumpHistory history;
    history.recordJump(jumpAt(0), 5);
    history.back(jumpAt(5), &target);
    history.clear();
    jumpHistoryCheck(!history.canBack() && !history.canForward(), "clear empties both directions");
  }

  return nJumpHistoryChecksFailed;
}

#ifdef JUMPHISTORYTEST_MAIN
int main()
{
  int nFailed = runJumpHistoryTests();
  printf(nFailed ? "%d FAILED\n" : "jumphistory: all passed\n", nFailed);
  return nFailed ? 1 : 0;
}
#endif
