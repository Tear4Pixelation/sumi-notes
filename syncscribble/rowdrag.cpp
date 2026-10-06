#include "rowdrag.h"

#include "ugui/svggui.h"

// in UI units, the same space event coordinates are in; about a third of a list row
static constexpr real DRAG_START_DIST = 10;

void RowDrag::addRow(Button* row, int key, Button* grip)
{
  rows[row] = key;
  // the widget the gesture starts on; a grip is "draggable" so a ScrollWidget passes it drags along
  //  the axis it scrolls, too (see the header)
  Button* source = grip ? grip : row;
  if(grip)
    grip->node->addClass("draggable");
  // Added after Button's own handler, so it runs first (later handlers have priority) - which is what
  //  lets a drop swallow the release before Button turns it into a click.
  source->addHandler([this, row, source](SvgGui* gui, SDL_Event* event){
    if(event->type == SDL_FINGERDOWN && event->tfinger.fingerId == SDL_BUTTON_LMASK) {
      sourceRow = row;
      sourceWidget = source;
      dragging = false;
      pressPos = Point(event->tfinger.x, event->tfinger.y);
      return false;  // Button still handles the press
    }
    if(sourceWidget != source)
      return false;
    if(event->type == SDL_FINGERMOTION && gui->pressedWidget == source) {
      Point pos(event->tfinger.x, event->tfinger.y);
      if(!dragging && pos.dist(pressPos) >= DRAG_START_DIST) {
        dragging = true;
        row->node->addClass("dragging");
      }
      if(dragging) {
        int targetKey;
        Widget* target = targetAt(gui, row, pos, &targetKey);
        bool allowed = target && target != row && (!canDrop || canDrop(rows[row], targetKey));
        setHover(allowed ? target : NULL);
      }
      // Accepted even below the threshold: when a ScrollWidget passes a pen or touch drag through to
      //  this row it checks that the row takes the first motion event, and takes the gesture back for
      //  scrolling if it does not.
      return true;
    }
    bool released = event->type == SDL_FINGERUP;
    // a release outside the row arrives as OUTSIDE_PRESSED carrying the release; anything else sending
    //  OUTSIDE_PRESSED (a second finger, the scroll view reclaiming the gesture) cancels the drag
    if(event->type == SvgGui::OUTSIDE_PRESSED) {
      SDL_Event* cause = static_cast<SDL_Event*>(event->user.data1);
      released = cause && cause->type == SDL_FINGERUP;
    }
    else if(!released)
      return false;
    if(!dragging) {
      sourceRow = NULL;
      sourceWidget = NULL;
      return false;  // a plain click
    }
    int src = rows[row];
    int dst = 0;
    // hoverTarget is only ever set to a target canDrop allowed
    bool doDrop = released && hoverTarget && (hoverTarget == rootTarget || rows.count(hoverTarget));
    if(doDrop)
      dst = hoverTarget == rootTarget ? ROOT : rows[hoverTarget];
    source->node->removeClass("pressed");
    source->node->removeClass("hovered");
    endDrag();
    if(doDrop)
      drop(gui, src, dst);
    return true;  // swallowed, so Button does not also report a click
  });
}

Widget* RowDrag::targetAt(SvgGui* gui, Widget* from, Point pos, int* keyOut) const
{
  if(!from->window())
    return NULL;
  for(Widget* widget = gui->widgetAt(from->window(), pos); widget; widget = widget->parent()) {
    if(widget == rootTarget) {
      if(keyOut) *keyOut = ROOT;
      return widget;
    }
    auto it = rows.find(widget);
    if(it != rows.end()) {
      if(keyOut) *keyOut = it->second;
      return widget;
    }
  }
  return NULL;
}

void RowDrag::setHover(Widget* target)
{
  if(target == hoverTarget)
    return;
  if(hoverTarget)
    hoverTarget->node->removeClass("drop-target");
  hoverTarget = target;
  if(hoverTarget)
    hoverTarget->node->addClass("drop-target");
}

void RowDrag::endDrag()
{
  setHover(NULL);
  if(sourceRow)
    sourceRow->node->removeClass("dragging");
  sourceRow = NULL;
  sourceWidget = NULL;
  dragging = false;
}

// Deferred to the next turn of the event loop: a drop changes what the list shows, so the callback
//  rebuilds it - deleting the row whose handler is still running.  The same reason Sidebar defers its
//  reparenting and TagDocList detaches its popup before a rebuild.
void RowDrag::drop(SvgGui* gui, int src, int dst)
{
  gui->setTimer(1, ownerWidget, [this, src, dst](){
    if(onDrop)
      onDrop(src, dst);
    return 0;
  });
}

void RowDrag::clear()
{
  // the root target outlives the rows, so only its highlight is dropped
  if(hoverTarget == rootTarget)
    setHover(NULL);
  hoverTarget = NULL;
  sourceRow = NULL;
  sourceWidget = NULL;
  dragging = false;
  rows.clear();
}
