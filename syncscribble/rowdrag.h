#pragma once

// Drag one list row onto another - what nests a tag under a tag in the document browser and an outline
//  entry under an entry in the sidebar.  One helper for both lists, so the gesture cannot drift apart.
//
// A row is an ordinary Button: a press followed by a release is still a click.  Only once the pointer
//  has travelled DRAG_START_DIST from the press does it become a drag, and from then on the release is
//  a drop, never a click.
//
// With a mouse any drag picks a row up.  With a pen or a finger the list's ScrollWidget owns the
//  gesture, and it hands a drag to the row only when it starts *sideways* (ScrollWidget's filter passes
//  through a drag along an axis it cannot scroll) - so vertical drags keep scrolling the list, and a
//  row is picked up by moving sideways first.

#include "ugui/widgets.h"
#include <functional>
#include <unordered_map>

class RowDrag
{
public:
  // the key reported for a drop on the root target (setRootTarget)
  static constexpr int ROOT = -1;

  // `owner` carries the timer the drop is delivered on - see drop()
  RowDrag(Widget* owner) : ownerWidget(owner) {}

  // dropped row `src` on row `dst` (or on ROOT); called after the event that caused it has been handled,
  //  so it may rebuild the list the rows live in
  std::function<void(int src, int dst)> onDrop;
  // whether `src` may be dropped on `dst`, e.g. not on its own descendant; unset allows everything
  std::function<bool(int src, int dst)> canDrop;

  // makes `row` draggable and a drop target, identified to the callbacks as `key` (>= 0)
  void addRow(Button* row, int key);
  // a widget that stands for "top level", e.g. the tag browser's All Documents row
  void setRootTarget(Widget* target) { rootTarget = target; }
  // forget every row; call before the rows are deleted
  void clear();

private:
  Widget* targetAt(SvgGui* gui, Widget* from, Point pos, int* keyOut) const;
  void setHover(Widget* target);
  void endDrag();
  void drop(SvgGui* gui, int src, int dst);

  Widget* ownerWidget;
  std::unordered_map<Widget*, int> rows;
  Widget* rootTarget = NULL;
  // the row the current press started on, and whether it has become a drag
  Widget* sourceRow = NULL;
  bool dragging = false;
  Point pressPos;
  Widget* hoverTarget = NULL;
};
