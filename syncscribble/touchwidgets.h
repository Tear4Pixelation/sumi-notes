#ifndef TOUCHWIDGETS_H
#define TOUCHWIDGETS_H

#include "ugui/widgets.h"
#include "ugui/colorwidgets.h"
#include "scribblepen.h"

class ScribbleApp;

class PenPreview : public Widget
{
public:
  PenPreview();  //int n = -1);
  void setPen(const ScribblePen& pen) { mPen = pen; redraw(); }
  // assuming this is derived from SvgNodeLayout, we have access to layout transform and so can use it to set
  //  canvas size instead of scaling what we draw
  void draw(SvgPainter* svgp) const override;
  Rect bounds(SvgPainter* svgp) const override;

  Rect mBounds;
  static Color bgColor;
  // dark mode map, as on the canvas; asked for at each draw, since the view that owns it can go away
  static std::function<const ColorMap*()> colorMap;
private:
  //int penNum;
  ScribblePen mPen = {Color::INVALID_COLOR, -1};
};

// A horizontal ruler with one tick per undo step, scrolling under a fixed center mark for the current
//  position - dragging right steps forward through history (redo), left steps back (undo).
// This replaces the earlier ButtonDragDial, which was the same interaction on a circle.  A circle has no
//  ends, so the two things a user most needs while scrubbing - where am I, and how much further can I go -
//  could only be hinted at (by a wedge that grew when you over-turned it).  The ends of a line are simply
//  where the ticks stop, and past-left/future-right needs no explaining.
// This is the Widget for the ruler canvas, shown inside an ArrowPopup so it stays legible over the page;
//  the toolbutton is a regular toolbutton with a special sdlEvent handler.
class ButtonDragTimeline : public Widget
{
public:
  // align positions the popup relative to the button, as for a Menu (e.g. Menu::VERT below a horizontal
  //  toolbar, Menu::HORZ beside a vertical one)
  // the panel is split into two lanes, each its own ruler: the one you drag in decides what the ticks
  //  mean.  This is how the second mode is reached on a stylus, which has exactly one input and so cannot
  //  express "the other button"; it also means one popup teaches both features instead of two controls
  //  each needing their own explanation.
  enum Lane { LANE_HISTORY = 0, LANE_SELECT = 1, NUM_LANES = 2 };

  ButtonDragTimeline(Button* tb, int align);

  void draw(SvgPainter* svgp) const override;
  Rect bounds(SvgPainter* svgp) const override;

  // as for the old dial, these apply `delta` steps and return the number that could not be applied
  std::function<int(int delta)> onStep;
  std::function<int(int delta)> onAltStep;
  // steps available from the current position, for drawing the ends of the history ruler; either may be
  //  left as -1 for "unknown", in which case that end stays open until a step is actually rejected
  std::function<void(int& back, int& fwd)> getRange;
  const char* laneLabel[NUM_LANES] = {NULL, NULL};
  // A hint line shown inside the panel until the first successful scrub, then retired for good via
  //  onHintDone().  Teaching lives in the panel rather than in a popup of its own: this is the one moment
  //  the user is already looking at the thing being explained, with history behind them to try it on.
  const char* hintText = NULL;
  bool showHint = false;
  std::function<void()> onHintDone;
  bool hintUsed = false;  // a step landed this gesture; retires the hint once the gesture ends
  std::function<void()> onOpened;  // panel shown; lets an outside tip pointing at the button get out of the way

private:
  void applyDrag(Dim x, Dim y);
  int applySteps(int delta);  // returns the steps of `delta` that could not be applied
  void updateEdgeRepeat(Dim x);
  void endGesture();
  void drawLane(Painter* p, int ln, Dim top, Dim laneh) const;

  Button* toolBtn;
  ArrowPopup* popup;
  Rect mBounds;
  // A ruler runs out of screen where a dial did not, and the button sits in a corner, so the direction
  //  with the least room to drag is whichever one points at the near edge.  Holding against that edge
  //  keeps stepping, which also removes any ceiling on how far a single gesture can reach.
  SvgGui* mGui = NULL;
  Timer* edgeTimer = NULL;
  int edgeDir = 0;

  int offset[NUM_LANES];     // steps from the position at press; negative is into the past
  int stepsBack[NUM_LANES];  // steps available from the press position, -1 until known (see getRange)
  int stepsFwd[NUM_LANES];
  int lane;
  bool laneLocked;  // set by the first step: after that, vertical drift cannot change mode mid-gesture
  Dim prevX;
  bool dragMoved;
};

class Menubar : public Toolbar
{
public:
  Menubar(SvgNode* n);
  void addButton(Button* btn);
  Button* addAction(Action* action);

  bool autoClose = false;
};

Menubar* createMenubar();
Menubar* createVertMenubar();

class AutoAdjContainer : public Widget
{
public:
  AutoAdjContainer(SvgNode* n, Widget* _contents);
  void repeatLayout(const Rect& dest);

  std::function<void(const Rect&, const Rect&)> adjFn;
  Widget* contents;
  Rect contentsBBox;

//private:
//  int numAbsPos = 0;
};

// one line of an options row's help popup
struct HelpEntry { const char* iconfile; const char* name; const char* desc; };
// "?" button showing a popup listing every button of its options row while pressed
Button* createHelpButton(const std::vector<HelpEntry>& entries);

// tooltip for widget with long press/right click action
#define altTooltip(s1, s2) fstring("<text>%s\n<tspan y='14' class='alttext'>%s</tspan></text>", s1, s2).c_str()

#endif
