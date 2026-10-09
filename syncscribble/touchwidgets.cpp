#include <algorithm>
#include <climits>
#include "touchwidgets.h"
//#include "scribbledoc.h"
//#include "scribbleapp.h"
#include "resources.h"
#include "strokebuilder.h"
#include "page.h"
#include "ugui/textedit.h"
#include "usvg/svgparser.h"
#include "usvg/svgpainter.h"


static void drawCheckerboard(Painter* p, Dim w, Dim h, int nrows, Color fill)
{
  Dim checkh = h/nrows;
  Dim checkw = checkh;
  int ncols = int(w/checkw);
  if(ncols < w/checkw) ++ncols;  // ceil
  for(int row = 0; row < nrows; ++row) {
    for(int col = row % 2; col < ncols; col += 2)
      p->fillRect(Rect::ltwh(checkw*col, checkh*row, checkw, checkh), fill);
  }
}

Color PenPreview::bgColor(Color::WHITE);
std::function<const ColorMap*()> PenPreview::colorMap;

PenPreview::PenPreview() : Widget(new SvgCustomNode), mBounds(Rect::wh(200, 70)) //, penNum(n)  //int n
{
  // TODO: this is now used in three places - deduplicate!
  onApplyLayout = [this](const Rect& src, const Rect& dest){
    mBounds = dest.toSize();
    if(src != dest) {
      m_layoutTransform.translate(dest.left - src.left, dest.top - src.top);
      node->invalidate(true);
    }
    return true;
  };
}

Rect PenPreview::bounds(SvgPainter* svgp) const
{
  return svgp->p->getTransform().mapRect(mBounds);
}

// we'll assume pen can't be changed when preview is displayed, and thus never call redraw()
void PenPreview::draw(SvgPainter* svgp) const
{
  const ScribblePen* pen = &mPen;
  Painter* p = svgp->p;
  const ColorMap* prevMap = p->colorMap();
  bool prevDark = p->darkBackdrop();
  p->setColorMap(colorMap ? colorMap() : NULL);
  // a marker multiplies; on a dark page it screens instead, as on the canvas (Page::draw())
  p->setDarkBackdrop(Page::drawsDark(p, bgColor));
  int w = mBounds.width() - 4;
  int h = mBounds.height() - 4;
  p->translate(2, 2);
  p->clipRect(Rect::wh(w, h));
  // use color of current page as background
  p->fillRect(Rect::wh(w, h), bgColor);  //doc->getCurrPageColor());
  drawCheckerboard(p, w, h, 4, 0x18000000);
  // draw the pen stroke
  bool isLine = pen->hasFlag(ScribblePen::SNAP_TO_GRID) || pen->hasFlag(ScribblePen::LINE_DRAWING)
      || pen->hasFlag(ScribblePen::CENTER_ON_LINE);
  Path2D path;
  path.moveTo(0.1f*w, 0.5f*h);
  isLine ? path.lineTo(Point(0.9f*w, 0.5f*h)) : path.cubicTo(0.3f*w, 0.2f*h, 0.7f*w, 0.8f*h, 0.9f*w, 0.5f*h);
  Path2D flat = path.toFlat();
  std::unique_ptr<StrokeBuilder> sb(StrokeBuilder::create(*pen));
  Timestamp t = 0;
  for(const Point& pt : flat.points) {
    Dim a = std::abs(2*pt.x - w)/w;  // 0 at edges of widget (>0 at ends of path), 1 in middle
    sb->addInputPoint(StrokePoint(pt.x, pt.y, 1.0 - a*a, 0, 0, t));
    t += 100 - 60*a;
  }
  SvgPainter(p).drawNode(sb->getElement()->node);
  p->setColorMap(prevMap);
  p->setDarkBackdrop(prevDark);
}

// undo timeline
// Sizes are in the same fixed design units as PenPreview above (the whole GUI is scaled for DPI), so one
//  step is one tick of TIMELINE_STEP_PX both on screen and in the drag that produces it.
static constexpr Dim TIMELINE_STEP_PX = 20;
// the ruler is sized to the history it is showing, between these bounds: a two-step history in a panel
//  wide enough for fifteen reads as "there is more over there", and a one-step history in a panel exactly
//  one tick wide is a sliver you cannot aim at
static constexpr int TIMELINE_MAX_TICKS = 15;
static constexpr int TIMELINE_MIN_TICKS = 5;
static constexpr Dim TIMELINE_LANE_PX = 42;  // tall enough to aim at with a finger, label included
static constexpr Dim TIMELINE_HINT_PX = 17;  // the hint line, present only until the first scrub
static constexpr Dim TIMELINE_EDGE_PX = 24;  // how close to the window edge starts the auto-repeat
static constexpr int TIMELINE_EDGE_MS = 110;  // one step per, about the rate of a comfortable scrub
static constexpr Dim TIMELINE_HEIGHT_PX = ButtonDragTimeline::NUM_LANES*TIMELINE_LANE_PX;

ButtonDragTimeline::ButtonDragTimeline(Button* tb, int align) : Widget(new SvgCustomNode), toolBtn(tb),
    mBounds(Rect::wh(TIMELINE_MAX_TICKS*TIMELINE_STEP_PX, TIMELINE_HEIGHT_PX)),
    offset{0, 0}, stepsBack{-1, -1}, stepsFwd{-1, -1}, lane(LANE_HISTORY), laneLocked(false),
    prevX(0), dragMoved(false)
{
  // TODO: this is now used in four places - deduplicate!
  onApplyLayout = [this](const Rect& src, const Rect& dest){
    mBounds = dest.toSize();
    if(src != dest) {
      m_layoutTransform.translate(dest.left - src.left, dest.top - src.top);
      node->invalidate(true);
    }
    return true;
  };

  // the ruler lives in the standard popup chrome so it stays readable over the page; the popup is shown
  //  and hidden directly (as setupPressedPopup does) rather than through the menu stack, since it lasts
  //  exactly as long as one press
  popup = createArrowPopup(align);
  popup->addWidget(this);
  toolBtn->addWidget(popup);

  // We replace the button's main event handler; code is a little weird because `this` is the ruler widget,
  //  while toolBtn is the actual button
  // if this does work out, need to add an official Widget::clearHandlers()
  toolBtn->sdlHandlers.clear();
  toolBtn->addHandler([this](SvgGui* gui, SDL_Event* event){
    if(event->type == SDL_FINGERDOWN || isLongPressOrRightClick(event)) {
      toolBtn->node->setXmlClass(addWord(removeWord(toolBtn->node->xmlClass(), "hovered"), "pressed").c_str());
      dragMoved = false;
      for(int ii = 0; ii < NUM_LANES; ++ii) {
        offset[ii] = 0;
        stepsBack[ii] = -1;
        stepsFwd[ii] = -1;
      }
      // right click (or long press) still goes straight to the select lane, locked: it costs nothing to
      //  keep for anyone who already has it in their fingers, and it is the one case where the mode is
      //  known before the drag starts
      lane = event->tfinger.fingerId != SDL_BUTTON_LMASK ? LANE_SELECT : LANE_HISTORY;
      laneLocked = lane == LANE_SELECT;
      if(getRange)
        getRange(stepsBack[LANE_HISTORY], stepsFwd[LANE_HISTORY]);
      // The select lane walks the same history backwards from the current position, so it can reach at
      //  most as many steps back as there are undo steps - an upper bound, since a step that added no
      //  strokes is skipped rather than counted, and being refused pins the real end (see applySteps).
      //  Nothing is selected yet at press, so there is nothing to give back in the other direction:
      //  without this the lane drew an open ruler both ways, promising history that isn't there.
      stepsBack[LANE_SELECT] = stepsBack[LANE_HISTORY];
      stepsFwd[LANE_SELECT] = 0;
      // size the ruler to the history, once per press: the width has to stay put for the rest of the
      //  gesture, since it is the thing being dragged (and ends discovered mid-drag would resize it)
      // the ruler is centered on the current position, so it needs room for the farther of the two ends
      //  on *both* sides - sizing it to back+fwd+1 would push the end cap off the edge exactly when the
      //  history is lopsided (e.g. plenty to undo, nothing to redo), which is most of the time
      int reach = std::max(stepsBack[LANE_HISTORY], stepsFwd[LANE_HISTORY]);
      int total = reach >= 0 ? 2*reach + 1 : TIMELINE_MAX_TICKS;
      int visible = std::min(std::max(total, TIMELINE_MIN_TICKS), TIMELINE_MAX_TICKS);
      bool hinting = showHint && hintText;
      // a panel sized to a two-step history is narrower than the hint line it would have to carry
      Dim wpx = std::max(Dim(visible*TIMELINE_STEP_PX), Dim(hinting ? 230 : 0));
      mBounds = Rect::wh(wpx, TIMELINE_HEIGHT_PX + (hinting ? TIMELINE_HINT_PX : 0));
      node->invalidate(true);
      prevX = event->tfinger.x;
      mGui = gui;  // needed to run the edge auto-repeat timer
      endGesture();
      if(onOpened)
        onOpened();
      popup->setVisible(true);
      redraw();
      gui->setPressed(toolBtn);
    }
    else if(event->type == SDL_FINGERMOTION && popup->isVisible()) {
      applyDrag(event->tfinger.x, event->tfinger.y);
    }
    else if(event->type == SDL_FINGERUP || event->type == SvgGui::OUTSIDE_PRESSED) {
      toolBtn->node->removeClass("pressed");
      popup->setVisible(false);
      endGesture();  // an auto-repeat left running would keep undoing after the finger is gone
      if(lane == LANE_SELECT && dragMoved)
        onAltStep(0);
      else if(!dragMoved && event->type == SDL_FINGERUP)
        onStep(-1);  // plain tap: single undo, so the button still works as an undo button
    }
    else if(event->type == SvgGui::ENTER && !gui->pressedWidget)
      toolBtn->node->addClass("hovered");
    else if(event->type == SvgGui::LEAVE && !gui->pressedWidget)
      toolBtn->node->removeClass("hovered");
    else
      return false;
    return true;
  });
}

// Dragging right undoes, dragging left redoes - the opposite of what "right is the future" suggests, and
//  correct: with the mark pinned at the center, what is under the finger is the tape, not the playhead.
//  Dragging the tape to the right has to bring earlier ticks to the center, exactly as on a wheel picker;
//  the alternative slides the ticks against the finger pushing them.
void ButtonDragTimeline::applyDrag(Dim x, Dim y)
{
  // Nothing happens until the pointer is inside the panel.  The press starts on the button, above the
  //  panel, so every gesture begins by travelling down into a lane - and that travel is never purely
  //  vertical.  Acting on the sideways component of it would lock the gesture into whichever lane it
  //  happened to be crossing at the time, which is the one way a lane can be chosen by accident.
  Rect bbox = node->bounds();
  if(!laneLocked) {
    if(!bbox.isValid() || y < bbox.top || y > bbox.bottom) {
      prevX = x;  // discard travel made on the way in, so it cannot accumulate into a step
      return;
    }
    int newlane = y - bbox.top > bbox.height()/2 ? LANE_SELECT : LANE_HISTORY;
    if(newlane != lane) {
      lane = newlane;
      redraw();
    }
  }
  // event coords and layout units are the same space (the framebuffer is scaled as a whole for DPI), so
  //  one tick of drag is one tick of ruler without any conversion
  int ticks = int((x - prevX)/TIMELINE_STEP_PX);  // truncates: no step until a full tick is crossed
  if(ticks == 0)
    return;
  dragMoved = true;
  // the first step locks the lane: scrubbing sideways always drifts vertically too, and a mode that
  //  changed halfway through a gesture would leave half its steps applied in the other mode
  laneLocked = true;
  prevX += ticks*TIMELINE_STEP_PX;  // consume what the finger travelled, before the sign flip below
  applySteps(-ticks);
  updateEdgeRepeat(x);
}

int ButtonDragTimeline::applySteps(int delta)
{
  int rejected = lane == LANE_SELECT ? onAltStep(delta) : onStep(delta);
  offset[lane] += delta - rejected;
  // retired by use, not by dismissal - and only by a step that actually landed, so someone who drags
  //  against a dead end has not yet been taught anything.  Noted here but not acted on until the
  //  gesture ends (see endGesture): the first step lands about one tick into the very drag that is
  //  showing the hint, so clearing it here would retire it unread - and would shorten the panel
  //  mid-drag, moving the lanes under the finger.
  if(showHint && delta != rejected)
    hintUsed = true;
  // being refused is how we learn where an end is when getRange didn't tell us; pinning it here also keeps
  //  the ruler honest if the history changed under us since the press
  if(rejected > 0)
    stepsFwd[lane] = offset[lane];
  else if(rejected < 0)
    stepsBack[lane] = -offset[lane];
  redraw();
  return rejected;
}

// Held against the edge of the window, the gesture keeps stepping on a timer: a drag can only be as long
// as the screen, and the button is in a corner, so one of the two directions has almost no room - for the
// top right corner that is undo, which is the one that gets used.
void ButtonDragTimeline::updateEdgeRepeat(Dim x)
{
  Window* win = toolBtn->window();
  Rect wb = win ? win->node->bounds() : Rect();
  int dir = 0;
  if(laneLocked && wb.isValid()) {
    if(x > wb.right - TIMELINE_EDGE_PX)
      dir = -1;  // pushing right, which is into the past
    else if(x < wb.left + TIMELINE_EDGE_PX)
      dir = 1;
  }
  if(dir == edgeDir)
    return;
  edgeDir = dir;
  if(edgeTimer) {
    if(mGui) mGui->removeTimer(edgeTimer);
    edgeTimer = NULL;
  }
  if(dir && mGui) {
    edgeTimer = mGui->setTimer(TIMELINE_EDGE_MS, this, [this](){
      // stop at the end of the history rather than ticking silently against it
      if(!edgeDir || !popup->isVisible() || applySteps(edgeDir)) {
        edgeTimer = NULL;  // returning 0 frees the timer; endGesture() must not remove a stale handle
        return 0;
      }
      return TIMELINE_EDGE_MS;
    });
  }
}

void ButtonDragTimeline::endGesture()
{
  edgeDir = 0;
  if(edgeTimer && mGui)
    mGui->removeTimer(edgeTimer);
  edgeTimer = NULL;
  // the hint has now been on screen for a whole gesture, and the user has scrubbed, so it has both
  //  been readable and been made unnecessary
  if(hintUsed && showHint) {
    showHint = false;
    if(onHintDone)
      onHintDone();
  }
  hintUsed = false;
}

Rect ButtonDragTimeline::bounds(SvgPainter* svgp) const
{
  return svgp->p->getTransform().mapRect(mBounds);
}

void ButtonDragTimeline::draw(SvgPainter* svgp) const
{
  Painter* p = svgp->p;
  Dim w = mBounds.width(), h = mBounds.height();
  p->clipRect(Rect::wh(w, h));
  bool hinting = showHint && hintText;
  Dim laneh = (h - (hinting ? TIMELINE_HINT_PX : 0))/NUM_LANES;
  for(int ii = 0; ii < NUM_LANES; ++ii)
    drawLane(p, ii, ii*laneh, laneh);

  if(hinting) {
    p->setFontSize(11);
    p->setTextAlign(Painter::AlignHCenter | Painter::AlignTop);
    p->setFillBrush(Color(150, 150, 150));
    p->setStrokeBrush(Color::NONE);
    p->drawText(w/2, NUM_LANES*laneh + 2, hintText);
  }
}

void ButtonDragTimeline::drawLane(Painter* p, int ln, Dim top, Dim laneh) const
{
  Dim w = mBounds.width();
  // every tick shares a baseline and grows upward, so a lane reads as a row of ticks of differing height
  //  rather than as marks scattered about a center line
  Dim cx = w/2, baseY = top + laneh - 5;
  Dim tickspace = laneh - 18;  // what is left of the lane once the label has its line
  bool active = ln == lane;
  int alpha = active ? 255 : 90;  // the idle lane stays legible, so both are readable at a glance
  Dim labelEnd = 7;  // right edge of the lane label, so the step count can keep clear of it

  Color mark = ln == LANE_SELECT ? Color(225, 85, 85) : Color(60, 120, 240);

  // The armed lane is named, in bold and in its own color: the label already says what the lane does, so
  //  styling it is what tells you what is about to happen - no extra chrome needed to carry that.
  if(laneLabel[ln]) {
    p->setFontSize(11);
    p->setFontWeight(active ? 700 : 400);
    p->setTextAlign(Painter::AlignLeft | Painter::AlignTop);
    // idle labels stay a mid-gray that works on a light theme as well as this dark one; we draw these
    //  ourselves, so there is no theme color to read
    p->setFillBrush(active ? mark : Color(150, 150, 150, 170));
    p->setStrokeBrush(Color::NONE);
    labelEnd += p->drawText(7, top + 3, laneLabel[ln]);
    p->setFontWeight(400);
  }

  // How far this lane has moved, above the current tick: the ruler shows where you are, this says how
  //  many steps that is, which otherwise has to be counted off the ticks.  Only once it has moved - a
  //  "0" sitting there at rest would be noise on a panel that opens under the finger every time.
  if(offset[ln] != 0) {
    // the select lane counts strokes taken into the selection, where a minus sign would read as removing
    //  them; the history lane is signed, since which side of now you are on is the whole point
    std::string count = ln == LANE_SELECT ? fstring("%d", -offset[ln]) : fstring("%+d", offset[ln]);
    p->setFontSize(11);
    p->setFontWeight(700);
    p->setFillBrush(Color(mark).setAlpha(alpha));
    p->setStrokeBrush(Color::NONE);
    // the label shares this band; if the panel is too narrow to fit both, the count goes to the far end
    //  rather than on top of the label
    Dim half = 0.5*p->textBounds(0, 0, count.c_str());
    bool fits = cx - half > labelEnd + 6;
    p->setTextAlign((fits ? Painter::AlignHCenter : Painter::AlignRight) | Painter::AlignTop);
    p->drawText(fits ? cx : w - 7, top + 3, count.c_str());
    p->setFontWeight(400);
  }

  // ticks that exist (the history is finite) and could be on screen (the panel is not); the panel was
  //  sized to the history at press, so when it all fits, nothing is clipped
  int halfticks = int(cx/TIMELINE_STEP_PX) + 1;
  int lo = std::max(stepsBack[ln] >= 0 ? -stepsBack[ln] : INT_MIN/2, offset[ln] - halfticks);
  int hi = std::min(stepsFwd[ln] >= 0 ? stepsFwd[ln] : INT_MAX/2, offset[ln] + halfticks);

  p->setFillBrush(Color::NONE);
  for(int ii = lo; ii <= hi; ++ii) {
    if(ii == offset[ln])
      continue;  // drawn last, so it is never clipped by a neighbor
    // the ends are set apart by weight and height at the same gray as the rest, not by brightness: we
    //  draw these ourselves and so can't read the theme, and any fixed brightness that stands out on the
    //  dark theme recedes on the light one (making the two most meaningful ticks the faintest)
    bool isEnd = (stepsBack[ln] >= 0 && ii == -stepsBack[ln]) || (stepsFwd[ln] >= 0 && ii == stepsFwd[ln]);
    p->setStroke(Color(128, 128, 128, alpha), isEnd ? 3 : 1.5, Painter::RoundCap, Painter::RoundJoin);
    Dim tickh = isEnd ? 0.55*tickspace : 0.30*tickspace;
    Dim x = cx + (ii - offset[ln])*TIMELINE_STEP_PX;
    p->drawLine(Point(x, baseY), Point(x, baseY - tickh));
  }

  // the current position: always at the center, but still marked, since "the tall blue one is you" is
  //  what makes the neighboring ticks readable as steps away from it
  p->setStroke(Color(mark).setAlpha(alpha), 3, Painter::RoundCap, Painter::RoundJoin);
  p->drawLine(Point(cx, baseY), Point(cx, baseY - 0.80*tickspace));
}

// most modern applications (at least on mobile) won't have any menubars, so complicating Button class to
//  support menubar doesn't seem right
Menubar::Menubar(SvgNode* n) : Toolbar(n)
{
  // same logic as Menu except we use `this` instead of `parent()` - any way to deduplicate?
  addHandler([this](SvgGui* gui, SDL_Event* event){
    if(event->type == SvgGui::OUTSIDE_PRESSED) {
      // close unless button up over parent (assumed to include opening button)
      Widget* target = static_cast<Widget*>(event->user.data2);
      if(!target || !target->isDescendantOf(this))
        gui->closeMenus();  // close entire menu tree
      return true;
    }
    if(event->type == SvgGui::OUTSIDE_MODAL) {
      Widget* target = static_cast<Widget*>(event->user.data2);
      gui->closeMenus();  // close entire menu tree
      // swallow event (i.e. return true) if click was within menu's parent to prevent reopening
      return target && target->isDescendantOf(this);
    }
    return false;
  });

  isPressedGroupContainer = true;
}

void Menubar::addButton(Button* btn)
{
  // this will run before Button's handler
  btn->addHandler([btn, this](SvgGui* gui, SDL_Event* event){
    if(event->type == SvgGui::ENTER) {
      // if our menu is open, enter event can't close it, and we have class=pressed, so don't add hovered
      if(!btn->mMenu || !btn->mMenu->isVisible()) {
        bool isPressed = gui->pressedWidget != NULL;
        // if a menu is open, we won't be sent enter event unless we are in same pressed group container
        bool sameMenuTree = !gui->menuStack.empty();
        gui->closeMenus(btn);  // close sibling menu if any
        // note that we only receive pressed enter event if button went down in our group
        if(btn->mMenu && (isPressed || sameMenuTree)) {
          btn->node->addClass("pressed");
          gui->showMenu(btn->mMenu);
        }
        else
          btn->node->addClass(isPressed ? "pressed" : "hovered");
      }
      return true;
    }
    else if(event->type == SDL_FINGERDOWN && event->tfinger.fingerId == SDL_BUTTON_LMASK) {
      // close menu bar menu on 2nd click
      if(btn->mMenu && !gui->menuStack.empty() && btn->mMenu == gui->menuStack.front()) {
        btn->node->removeClass("hovered");
        gui->closeMenus();
        return true;  // I don't think it makes sense to send onPressed when we are clearing pressed state!
      }
    }
    else if(event->type == SDL_FINGERUP && (!btn->mMenu || autoClose)) {
      gui->closeMenus();
      return false;  // continue to button handler
    }
    else if(isLongPressOrRightClick(event) && btn->mMenu) {
      if(!btn->mMenu->isVisible()) {
        btn->node->addClass("pressed");
        gui->showMenu(btn->mMenu);
      }
      gui->pressedWidget = NULL;  //setPressed(btn->mMenu) doesn't work as menubar is pressed group container
      return true;
    }
    return false;
  });

  addWidget(btn);
}

// would be nice to deduplicate this cut and paste from Toolbar::addAction() (w/o using virtual!)
Button* Menubar::addAction(Action* action)
{
  Button* item = createToolbutton(action->icon(), action->title.c_str());
  action->addButton(item);
  addButton(item);
  // handler added in addButton() stops propagation of ENTER event, so tooltip handler must preceed it
  setupTooltip(item, action->tooltip.empty() ? action->title.c_str() : action->tooltip.c_str());
  return item;
}

Button* createHelpButton(const std::vector<HelpEntry>& entries)
{
  Button* btn = createToolbutton(SvgGui::useFile(":/icons/ic_menu_help.svg"), _("Help"));
  // icon size of the row's other buttons, so a global icon resize carries over
  SvgNode* btnIcon = btn->containerNode()->selectFirst(".icon");
  real iconSize = btnIcon ? btnIcon->getFloatAttr("width", 24) : 24;

  // cap popup width to ~2/3 of the color picker popup's ~380px design width and word-wrap the
  //  description to fit, since ArrowPopup has no built-in max-width/word-wrap of its own
  const real popupMaxWidth = 253;
  real descMaxWidth = popupMaxWidth - 2*16 /* ArrowPopup left/right content padding */
      - iconSize - 0.2*iconSize /* row margin */ - 0.4*iconSize /* text column margin */;

  ArrowPopup* popup = createArrowPopup(Menu::VERT_LEFT);
  for(const HelpEntry& entry : entries) {
    Widget* icon = new Widget(loadSVGFragment(fstring(
        "<use class='icon' width='%g' height='%g' xlink:href=':/icons/%s'/>",
        iconSize, iconSize, entry.iconfile).c_str()));
    TextBox* name = createTextBox(_(entry.name));
    name->node->addClass("arrowpopup-title");
    name->node->setAttribute("box-anchor", "left");
    SvgText* descNode = createTextNode(_(entry.desc));
    descNode->addClass("arrowpopup-desc");
    descNode->setAttribute("box-anchor", "left");
    Widget* desc = new Widget(descNode);
    Widget* text = createColumn({name, desc}, "", "", "left");
    text->setMargins(0, 0, 0, 0.4*iconSize);
    Widget* row = createRow({icon, text}, "", "", "left");
    row->setMargins(0.2*iconSize, 0);
    popup->addWidget(row);

    std::string wrapped = SvgPainter::breakText(descNode, descMaxWidth);
    if(wrapped != descNode->text()) {
      descNode->clearText();
      descNode->addText(wrapped.c_str());
    }
  }
  setupPressedPopup(btn, popup);
  return btn;
}

Menubar* createMenubar() { return new Menubar(widgetNode("#toolbar")); }
Menubar* createVertMenubar() { return new Menubar(widgetNode("#vert-toolbar")); }

// previously, adjFn only made one adjustment and was called in a loop, but having adjFn drive the layout
//  process instead allows for simplier code and potentially more efficiency
void AutoAdjContainer::repeatLayout(const Rect& dest)
{
  //window()->absPosNodes.erase(window()->absPosNodes.begin() + numAbsPos, window()->absPosNodes.end());
  window()->gui()->layoutWidget(contents, dest);
}

// if this is useful, we can move to widgets.cpp as generic recursive layout container
AutoAdjContainer::AutoAdjContainer(SvgNode* n, Widget* _contents) : Widget(n), contents(_contents)
{
  addWidget(contents);

  onApplyLayout = [this](const Rect& src, const Rect& dest){
    // fit contents to container
    //numAbsPos = window()->absPosNodes.size();
    window()->gui()->layoutWidget(contents, dest);
    adjFn(src, dest);
    contentsBBox = contents->node->bounds();
    return true;
  };

  onPrepareLayout = [this](){
    // If our size is used by the parent's layout (fillReportsSize), we must report the *minimum* size of
    //  our contents; contentsBBox is the size the contents were last stretched to, so reporting it would
    //  let the size only ratchet up (contents containing a hfill child always fill whatever they're given).
    // Laying out at zero size gives the natural (unsqueezed) size, then adjFn collapses the contents to
    //  their minimum, which is what must fit; adjFn will expand them again when we're given our real size.
    if(fillReportsSize) {
      SvgGui* gui = window()->gui();
      gui->layoutWidget(contents, Rect::wh(0, 0));
      if(adjFn)
        adjFn(contents->node->bounds(), Rect::wh(0, 0));
      contentsBBox = contents->node->bounds();
      return contentsBBox.toSize();
    }
    // we will typically only stretch along one direction - need to get content size for other direction
    if(contentsBBox.isValid())
      return contentsBBox.toSize();
    //contents->setLayoutTransform(Transform2D());
    //setLayoutTransform(Transform2D());
    window()->gui()->layoutWidget(contents, Rect::wh(0, 0));
    Rect bbox = contents->node->bounds();
    return bbox.toSize();
  };
}
