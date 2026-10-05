#include "scribblemode.h"
#include "shape.h"
#include "page.h"
#include "scribbleconfig.h"
#include <sstream>
#include <cstring>
#include <cmath>
#include "widthpresets.h"


// if we get rid of ruled mode, how do we determine what to do for move sel?  use insert space setting?
// select move sel tool from pen menu?  select menu?

int ScribbleMode::getSpecificMode(int mode) const
{
  switch(mode) {
  // handle general modes
  case MODE_ERASE:
    return eraserMode;
  case MODE_SELECT:
    return selectMode;
  case MODE_INSSPACE:
    return insSpaceMode;
  case MODE_MOVESEL:
    return moveSelMode;
  default:
    return mode;
  }
}

// UI code is always in flux, so there is no point going out of the way to make it elegant or general
int ScribbleMode::getModeType(int mode)
{
  switch(mode) {
  case MODE_PAN:
    return MODE_PAN;
  case MODE_STROKE:
  case MODE_BOOKMARK:
    return MODE_STROKE;
  case MODE_DRAWSHAPE:
  case MODE_SHAPEHANDLE:
    return MODE_DRAWSHAPE;
  case MODE_ERASE:
  case MODE_ERASESTROKE:
  case MODE_ERASERULED:
  case MODE_ERASEFREE:
  case MODE_ERASEFREERULED:
    return MODE_ERASE;
  case MODE_SELECT:
  case MODE_SELECTRECT:
  case MODE_SELECTRULED:
  case MODE_SELECTLASSO:
  case MODE_SELECTPATH:
    return MODE_SELECT;
  case MODE_MOVESELFREE:
  case MODE_MOVESELRULED:
    return MODE_MOVESEL;
  case MODE_INSSPACE:
  case MODE_INSSPACERULED:
  case MODE_INSSPACEDOWN:
  case MODE_INSSPACERIGHT:
  case MODE_INSSPACEVERT:
  case MODE_INSSPACEHORZ:
    return MODE_INSSPACE;
  case MODE_PAGESEL:
    return MODE_PAGESEL;
  }
  return 0;  // to suppress compiler warning
}

// alternative interface (will probably be removed)
void ScribbleMode::setRuled(bool ruled)
{
  eraserMode = ruled ? MODE_ERASERULED : MODE_ERASESTROKE;
  selectMode = ruled ? MODE_SELECTRULED : MODE_SELECTRECT;
  insSpaceMode = ruled ? MODE_INSSPACEDOWN : MODE_INSSPACEVERT;
  moveSelMode = ruled ? MODE_MOVESELRULED : MODE_MOVESELFREE;
}

ScribblePen& ScribbleMode::penForDrawTool(int tool)
{
  switch(tool) {
  case DRAWTOOL_HIGHLIGHT:  return highlightPen;
  case DRAWTOOL_EPHEMERAL:  return ephemeralPen;
  default:  return drawPen;
  }
}

static void writePen(std::ostream& ss, const ScribblePen& pen)
{
  ss << ' ' << pen.color.color << ' ' << pen.flags << ' ' << pen.width << ' ' << pen.wRatio
     << ' ' << pen.prParam << ' ' << pen.spdMax << ' ' << pen.dirAngle << ' ' << pen.dash << ' ' << pen.gap;
}

static void readPen(std::istream& ss, ScribblePen& pen, unsigned int reqflags)
{
  unsigned int argb, flags;
  ScribblePen loaded(Color::BLACK, 1);
  if(!(ss >> argb >> flags >> loaded.width >> loaded.wRatio >> loaded.prParam
      >> loaded.spdMax >> loaded.dirAngle >> loaded.dash >> loaded.gap))
    return;
  loaded.color = Color(argb);
  loaded.flags = flags | reqflags;
  // a width that is not a width keeps the default pen; one past the width spinbox's limit for its unit
  //  is clamped into it, since the spinbox can never show it and so it could never be edited (a 144 line
  //  height pen).  The dash pattern is in the width's unit, so it is rescaled with it.
  if(!std::isfinite(loaded.width) || loaded.width <= 0)
    return;
  Dim maxwidth = WidthPresets::maxWidth(loaded.hasFlag(ScribblePen::WIDTH_RELATIVE));
  if(loaded.width > maxwidth) {
    int dashstyle = loaded.dashStyle();
    loaded.width = maxwidth;
    loaded.setDashStyle(dashstyle);
  }
  else if(loaded.width < WidthPresets::MIN_WIDTH)
    loaded.width = WidthPresets::MIN_WIDTH;
  pen = loaded;
}

std::string ScribbleMode::saveModes()
{
  std::ostringstream ss;
  ss << eraserMode << ' ' << selectMode << ' ' << insSpaceMode << ' ' << moveSelMode
     << ' ' << drawTool << ' ' << int(eraseSwitchBack);
  writePen(ss, drawPen);
  writePen(ss, highlightPen);
  writePen(ss, ephemeralPen);
  // Appended after the pens so that a config string written by an older version still loads correctly.
  // The shape is stored by *string* id: shapes have been merged into flags once already, and a numeric
  //  index would have silently reassigned everyone's active tool when that happened.
  const ShapeDef* shapedef = shapeDef(shapeId);
  ss << ' ' << (shapedef ? shapedef->id : "box") << ' ' << shapeFlags
     << ' ' << int(selectSwitchBack) << ' ' << int(eraseSwitchBack) << ' ' << int(insSpaceSwitchBack)
     << ' ' << int(selectTouching) << ' ' << int(insSpaceSkipLines);
  return ss.str();
}

void ScribbleMode::loadModes(const char* modestr)
{
  std::stringstream ss(modestr ? modestr : "");
  int mode;
  // defaults
  eraserMode = MODE_ERASERULED;
  selectMode = MODE_SELECTRECT;  // MODE_SELECTRULED
  insSpaceMode = MODE_INSSPACEDOWN;
  moveSelMode = MODE_MOVESELFREE;  // tough call between ruled and free for initial
  drawTool = DRAWTOOL_PEN;
  shapeId = SHAPE_BOX;
  shapeFlags = 0;
  // all default to on, which is what the removed global doubleTapSticky pref (also on by default) did
  eraseSwitchBack = true;
  selectSwitchBack = true;
  insSpaceSwitchBack = true;
  selectTouching = false;
  insSpaceSkipLines = false;
  drawPen = ScribblePen(Color::BLACK, 1.6, ScribblePen::TIP_FLAT | ScribblePen::WIDTH_PR, 0.9, 2.0);
  // the marker's width is a fraction of the line height by default: what a marker has to do is cover a
  //  line of text, so the useful number is "three quarters of a line", not "30 units" (which covers a
  //  line on one ruling and a third of one on another).  0.75 is also the middle savedMarkerWidths
  //  preset, so the toolbar opens with a preset selected.
  highlightPen = ScribblePen(Color(255, 127, 255, 127), 0.75,
      ScribblePen::TIP_CHISEL | ScribblePen::DRAW_UNDER | ScribblePen::WIDTH_RELATIVE);
  ephemeralPen = ScribblePen(Color::RED, 1.6,
      ScribblePen::TIP_FLAT | ScribblePen::WIDTH_PR | ScribblePen::EPHEMERAL, 0.9, 2.0);
  // attempt to load from config string
  if(ss >> mode && getModeType(mode) == MODE_ERASE && mode != MODE_ERASE)
    eraserMode = mode;
  if(ss >> mode && getModeType(mode) == MODE_SELECT && mode != MODE_SELECT)
    selectMode = mode;
  // the combined ruled insert space is no longer offered: a config saved with it gets Down, the lines half
  if(ss >> mode && getModeType(mode) == MODE_INSSPACE && mode != MODE_INSSPACE)
    insSpaceMode = mode == MODE_INSSPACERULED ? MODE_INSSPACEDOWN : mode;
  if(ss >> mode && getModeType(mode) == MODE_MOVESEL && mode != MODE_MOVESEL)
    moveSelMode = mode;
  //moveSelMode = (insSpaceMode == MODE_INSSPACERULED) ? MODE_MOVESELRULED : MODE_MOVESELFREE;
  if(ss >> mode && mode >= DRAWTOOL_PEN && mode <= DRAWTOOL_EPHEMERAL)
    drawTool = mode;
  // The switch back flags are read from the trailing section below, not here: this slot held
  //  eraseSwitchBack back when the toggle did nothing, so every config written before switch back was
  //  implemented has a 0 in it, and honoring that would silently make the eraser sticky on upgrade.
  //  Still written (and consumed here) to keep the field positions that follow it.
  if(ss >> mode)
    (void)mode;
  readPen(ss, drawPen, 0);
  readPen(ss, highlightPen, ScribblePen::DRAW_UNDER);
  // The marker's width is a fraction of the line height, always - that is the number that means
  //  something for a tool whose job is to cover a line of text, and it is the default above.  A config
  //  written before relative widths existed (or with the toggle turned off) carries an absolute width,
  //  so it is converted here rather than reinterpreted: 30 units would otherwise load as 30 line
  //  heights.  The page being drawn on is not known yet, so the blank-page ruling is the unit; it is
  //  the same fallback PenToolbar::lineHeight() uses when a page has none.
  // A marker preset list saved with its unit ("rel:"/"abs:", see WidthPresets) is converted along with
  //  the pen by PenToolbar::prepareWidths().  An older one has no unit to convert by, so it is cleared
  //  and reseeded (in line heights) there.  This runs before the toolbar is built, which is what makes
  //  clearing the config value enough.
  if(!highlightPen.hasFlag(ScribblePen::WIDTH_RELATIVE)) {
    highlightPen.width = highlightPen.width/Page::BLANK_Y_RULING;
    highlightPen.setFlag(ScribblePen::WIDTH_RELATIVE, true);
    const char* markerWidths = cfg->String("savedMarkerWidths", "");
    if(strncmp(markerWidths, "rel:", 4) != 0 && strncmp(markerWidths, "abs:", 4) != 0)
      cfg->set("savedMarkerWidths", "");
  }
  readPen(ss, ephemeralPen, ScribblePen::EPHEMERAL);
  std::string shapestr;
  if(ss >> shapestr) {
    int id = shapeIdByStringId(shapestr.c_str());
    if(id != SHAPE_NONE)
      shapeId = id;
  }
  if(ss >> mode)
    shapeFlags = mode & (SHAPEFLAG_HEADSTART | SHAPEFLAG_HEADEND);
  if(ss >> mode)
    selectSwitchBack = mode != 0;
  if(ss >> mode)
    eraseSwitchBack = mode != 0;
  if(ss >> mode)
    insSpaceSwitchBack = mode != 0;
  if(ss >> mode)
    selectTouching = mode != 0;
  if(ss >> mode)
    insSpaceSkipLines = mode != 0;
}

void ScribbleMode::setMode(int mode, bool once)
{
  int newmode = mode;
  switch(mode) {
  // update saved tool modes
  case MODE_ERASESTROKE:
  case MODE_ERASERULED:
  case MODE_ERASEFREE:
  case MODE_ERASEFREERULED:
    eraserMode = mode;
  case MODE_ERASE:
    newmode = MODE_ERASE;
    break;
  case MODE_SELECTRECT:
  case MODE_SELECTRULED:
  case MODE_SELECTLASSO:
  case MODE_SELECTPATH:
    selectMode = mode;
  case MODE_SELECT:
    newmode = MODE_SELECT;
    break;
  case MODE_MOVESELFREE:
  case MODE_MOVESELRULED:
    moveSelMode = mode;
  case MODE_MOVESEL:
    newmode = MODE_MOVESEL;
    break;
  case MODE_INSSPACERULED:
  case MODE_INSSPACEDOWN:
  case MODE_INSSPACERIGHT:
    //moveSelMode = MODE_MOVESELRULED;
    insSpaceMode = mode;
    newmode = MODE_INSSPACE;
    break;
  case MODE_INSSPACEVERT:
  case MODE_INSSPACEHORZ:
    //moveSelMode = MODE_MOVESELFREE;
    insSpaceMode = mode;
    newmode = MODE_INSSPACE;
    break;
  }

  // the tools with a switch back toggle follow it; every other tool switches back.  Picking the active
  //  tool again no longer locks it (double tap to lock) - that is also how its options row is closed
  bool switchback = hasSwitchBack(newmode) ? switchBack(newmode) : true;
  if(!once && (newmode == MODE_STROKE || newmode == MODE_DRAWSHAPE || newmode == MODE_PAGESEL
      || newmode == MODE_PAN || !switchback)) {
    if(newmode != stickyMode)
      prevStickyMode = stickyMode;
    stickyMode = newmode;
  }
  else if(currMode == MODE_PAGESEL)
    stickyMode = MODE_STROKE;

  currMode = newmode;
}

// the tools whose options row carries a switch back toggle
bool ScribbleMode::hasSwitchBack(int modetype)
{
  return modetype == MODE_ERASE || modetype == MODE_SELECT || modetype == MODE_INSSPACE;
}

bool ScribbleMode::switchBack(int modetype) const
{
  return modetype == MODE_ERASE ? eraseSwitchBack :
      modetype == MODE_SELECT ? selectSwitchBack : insSpaceSwitchBack;
}

// applied to the active tool as well as to the next selection of it, so the toggle takes effect on the
//  tool the user is looking at instead of only the next time they pick it
void ScribbleMode::setSwitchBack(int modetype, bool on)
{
  (modetype == MODE_ERASE ? eraseSwitchBack :
      modetype == MODE_SELECT ? selectSwitchBack : insSpaceSwitchBack) = on;
  if(currMode != modetype)
    return;
  if(!on)
    stickyMode = currMode;
  else if(stickyMode == currMode)  // stop locking the tool; return to whatever it replaced
    stickyMode = prevStickyMode != currMode ? prevStickyMode : MODE_STROKE;
}

// Declaring this const to make it clear that it can be called multiple times for the same cursor down event
int ScribbleMode::getScribbleMode(int modifier) const
{
  int pressedmode = (modifier & MODEMOD_PRESSEDMASK) ? (modifier >> 24) : 0;
  if((modifier & MODEMOD_EDGEMASK) && cfg->Int("panFromEdge") > 0)
    return MODE_PAN;
  else if(currMode == MODE_PAGESEL)
    return MODE_PAGESEL;
  else if(modifier & MODEMOD_SCALESEL)
    return (modifier & MODEMOD_PENBTN) ? MODE_SCALESELW : MODE_SCALESEL;
  else if(modifier & MODEMOD_ROTATESEL)
    return (modifier & MODEMOD_PENBTN) ? MODE_ROTATESELW : MODE_ROTATESEL;
  else if(modifier & MODEMOD_CROPSEL)
    return MODE_CROPSEL;
  else if(modifier & MODEMOD_SHAPEHANDLE)
    return MODE_SHAPEHANDLE;
  else if(modifier & MODEMOD_ERASE)
    return eraserMode;
  else if(modifier & MODEMOD_PENBTN)
    return getSpecificMode(cfg->Int("penButtonMode"));
  else if(pressedmode > MODE_NONE && pressedmode < MODE_LAST)
    return getSpecificMode(pressedmode);
  else if(modifier == MODEMOD_MOVESEL)
    return MODE_MOVESEL;  //moveSelMode;  ... this is now determined in ScribbleArea
  else
    return getSpecificMode(currMode);
}

int ScribbleMode::scribbleDone()
{
  currMode = stickyMode;
  return currMode;
}

bool ScribbleMode::isUndoable(int mode)
{
  return (mode != MODE_NONE && mode != MODE_PAN && mode != MODE_SELECTRECT && mode != MODE_SELECTRULED &&
      mode != MODE_SELECTLASSO && mode != MODE_SELECTPATH && mode != MODE_PAGESEL);
}

// returns true for a mode which can benefit from faster, lower quality drawing
bool ScribbleMode::isSlow(int mode)
{
  return (mode >= MODE_SELECT && mode <= MODE_INSSPACERULED) || mode == MODE_INSSPACEDOWN || mode == MODE_INSSPACERIGHT;
}
