#ifndef SCRIBBLEMODE_H
#define SCRIBBLEMODE_H

#include "scribbleconfig.h"

// don't change numeric values (saved to config file to remember tool modes) - only add at end!
enum { MODE_NONE = 10, MODE_PAN, MODE_STROKE, MODE_ERASE, MODE_ERASESTROKE, MODE_ERASERULED, MODE_ERASEFREE,
    MODE_SELECT, MODE_SELECTRECT, MODE_SELECTRULED, MODE_SELECTLASSO, MODE_MOVESEL, MODE_MOVESELFREE,
    MODE_MOVESELRULED, MODE_INSSPACE, MODE_INSSPACEVERT, MODE_INSSPACEHORZ, MODE_INSSPACERULED,
    MODE_BOOKMARK, MODE_SCALESEL, MODE_SCALESELW, MODE_ROTATESEL, MODE_ROTATESELW, MODE_TOOLMENU,
    MODE_SELECTPATH, MODE_CROPSEL, MODE_PAGESEL, MODE_ERASEFREERULED, MODE_DRAWSHAPE, MODE_SHAPEHANDLE,
    MODE_LAST};

constexpr int MODEMOD_NONE = 0;
constexpr int MODEMOD_ERASE = 0x01;
constexpr int MODEMOD_PENBTN = 0x02;
constexpr int MODEMOD_MOVESEL = 0x04;
constexpr int MODEMOD_SCALESEL = 0x08;
constexpr int MODEMOD_ROTATESEL = 0x10;
constexpr int MODEMOD_CROPSEL = 0x20;
constexpr int MODEMOD_SHAPEHANDLE = 0x40;
// flags indicating cursor entered from edge of screen
constexpr int MODEMOD_EDGETOP = 0x100;
constexpr int MODEMOD_EDGEBOTTOM = 0x200;
constexpr int MODEMOD_EDGELEFT = 0x400;
constexpr int MODEMOD_EDGERIGHT = 0x800;
constexpr int MODEMOD_EDGEMASK = 0xF00;
// only set for release events
constexpr int MODEMOD_CLICK = 0x10000;
constexpr int MODEMOD_FASTCLICK = 0x20000;
constexpr int MODEMOD_DBLCLICK = 0x40000;
constexpr int MODEMOD_MAYBECLICK = 0x80000;
// for now, we're just going to use upper byte of modemod to hold id of pressed button for simul. pen + touch
constexpr int MODEMOD_PRESSEDMASK = 0xFF000000;

// actions
enum {ID_UNDO = 100, ID_REDO, ID_SELALL, ID_SELSIMILAR, ID_INVSEL, ID_DELSEL, ID_COPYSEL, ID_CUTSEL,
    ID_PASTE, ID_CANCEL, ID_EXPANDDOWN, ID_EXPANDRIGHT, ID_ZOOMIN, ID_ZOOMOUT, ID_RESETZOOM, ID_PREVPAGE,
    ID_NEXTPAGE, ID_PAGEAFTER, ID_PAGEBEFORE, ID_DELPAGE, ID_NEWDOC, ID_ZOOMALL, ID_ZOOMWIDTH, ID_DUPSEL,
    ID_LINKBOOKMARK, ID_PREVVIEW, ID_NEXTVIEW, ID_SELRECENT, ID_DESELRECENT, ID_SAVESEL, ID_NEXTPAGENEW,
    ID_PREVSCREEN, ID_NEXTSCREEN, ID_STARTOFDOC, ID_ENDOFDOC, ID_SCROLLUP, ID_SCROLLDOWN, ID_UNGROUP};

constexpr int ID_EMULATEPENBTN = 3000;

class ScribbleMode {
private:
  int currMode;  // specific mode
  int stickyMode;  // for return after single use
  // the tool stickyMode replaced, so that turning switch back on for the active tool has something to
  //  return to - we'd otherwise have no record of what the user was using before
  int prevStickyMode;
  // these are used to select the specific tool if a general tool selection is made
  ScribbleConfig* cfg;

  int getSpecificMode(int mode) const;

public:
  // draw tools all use MODE_STROKE and are distinguished only by the pen they use
  enum { DRAWTOOL_PEN = 0, DRAWTOOL_HIGHLIGHT, DRAWTOOL_EPHEMERAL };

  int eraserMode;
  int selectMode;
  int insSpaceMode;
  int moveSelMode;
  int drawTool;
  // the shape row's Paper Patch tool (a ruling region): MODE_DRAWSHAPE draws a ruling region instead of shapeId.  Not
  //  saved - it is a one-off, not a tool anyone keeps in hand
  bool drawRegion = false;
  // active shape for MODE_DRAWSHAPE; one of ShapeId (see shape.h)
  int shapeId;
  // SHAPEFLAG_HEADSTART/HEADEND from the head toggles on the shape options row; applied to new shapes
  //  whose ShapeDef allows heads
  int shapeFlags;
  // Switch back: return to the previous tool after one use.  The eraser, the selection tool and insert
  //  space follow these flags; the draw tools, shapes, pan and page select always stay, and every other
  //  tool is single use.  There is no double tap to lock: tapping the active tool is how its options row
  //  is closed, so it locked tools by accident.
  bool eraseSwitchBack;
  bool selectSwitchBack;
  bool insSpaceSwitchBack;
  // rect, lasso and ruled select take everything the selection area touches instead of only what lies
  //  entirely inside it; path select always takes what it touches
  bool selectTouching;
  // ruled insert space treats the page as written on every second line: the pressed line and every
  //  second line from it are text lines, each with half the blank line either side (skippedLineFrame())
  bool insSpaceSkipLines;
  // DRAW_UNDER/EPHEMERAL are baked into these so drawing code can just check currPen()->hasFlag()
  ScribblePen drawPen;
  ScribblePen highlightPen;
  ScribblePen ephemeralPen;

  // the switch back flags are initialized here as well as in loadModes(): ScribbleTest builds a
  //  ScribbleMode directly and never calls loadModes, so setMode() would otherwise read uninitialized
  ScribbleMode(ScribbleConfig* _cfg) : currMode(MODE_NONE), stickyMode(MODE_NONE),
      prevStickyMode(MODE_STROKE), cfg(_cfg), eraseSwitchBack(true), selectSwitchBack(true),
      insSpaceSwitchBack(true), selectTouching(false), insSpaceSkipLines(false),
      drawPen(Color::BLACK, 1), highlightPen(Color::BLACK, 1), ephemeralPen(Color::BLACK, 1) {}

  ScribblePen& penForDrawTool(int tool);
  ScribblePen& currDrawPen() { return penForDrawTool(drawTool); }

  // used to show current mode in UI ... might want to return general mode, but return specific mode for
  //  now to maintain previous behavior
  int getMode() const { return getSpecificMode(currMode); }
  int getNextMode() const { return getSpecificMode(stickyMode); }
  // this should be called when user selects a tool
  void setMode(int mode, bool once=false);
  // switch back toggles; modetype is MODE_ERASE, MODE_SELECT or MODE_INSSPACE
  static bool hasSwitchBack(int modetype);
  bool switchBack(int modetype) const;
  void setSwitchBack(int modetype, bool on);
  // legacy support
  void setRuled(bool ruled);
  // interface to ScribbleArea
  int getScribbleMode(int modifier = MODEMOD_NONE) const;  // called by ScribbleArea::onPressEvent
  int scribbleDone();  // called on cursor up or cancel

  std::string saveModes();
  void loadModes(const char* modestr);

  static int getModeType(int mode);
  static bool isUndoable(int mode);
  static bool isSlow(int mode);
};

#endif // SCRIBBLEMODE_H
