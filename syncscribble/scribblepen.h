#pragma once

#include "basics.h"
#include "ulib/painter.h"

class ScribblePen
{
public:
  Color color;
  Dim width;
  Dim wRatio;  // (maxwidth - minwidth)/maxwidth
  Dim prParam;
  Dim spdMax;
  Dim dirAngle;
  Dim dash, gap;

  unsigned int flags;
  // existing numerical values must NOT be changed - flags is serialized to config file when saving pen
  static constexpr unsigned int DRAW_UNDER = 0x1, SNAP_TO_GRID = 0x2, LINE_DRAWING = 0x4, EPHEMERAL = 0x8,
      WIDTH_PR = 0x10, WIDTH_SPEED = 0x20, WIDTH_DIR = 0x40, WIDTH_MASK = 0xF0, //WIDTH_LINPR = 0x80,
      TIP_FLAT = 0x100, TIP_ROUND = 0x200, TIP_CHISEL = 0x400, TIP_MASK = 0xF00,
      // width is a multiple of the page's line height instead of an absolute size (text marker); see
      //  ScribbleArea::resolvedPen(), which is the only place the multiplication is done
      WIDTH_RELATIVE = 0x1000,
      // draws a straight horizontal stroke through the vertical centre of the ruled line the press lands
      //  in (text marker); implies line drawing
      CENTER_ON_LINE = 0x2000;

  ScribblePen(Color c, Dim w, unsigned int _flags = 0, Dim wr = 0, Dim pr = 0, Dim spd = 0, Dim angle = 0, Dim _dash = 0, Dim _gap = 0)
      : color(c), width(w), wRatio(wr), prParam(pr), spdMax(spd), dirAngle(angle), dash(_dash), gap(_gap), flags(_flags) {}

  bool operator==(const ScribblePen& b) const { return memcmp(this, &b, sizeof(ScribblePen)) == 0; }

  bool hasFlag(unsigned int flag) const { return (flags & flag) == flag; }
  void setFlag(unsigned int flag, bool value) { if(value) flags |= flag; else flags &= ~flag; }
  void clearAndSet(unsigned int clearflag, unsigned int setflag) { flags = (flags & ~clearflag) | setflag; }
  bool hasVarWidth() const { return flags & WIDTH_MASK; }
  bool usesPressure() const { return hasFlag(WIDTH_PR); }

  // Pressure sensitivity as the user sees it: how much thinner the lightest touch draws than a full press,
  //  as a fraction of the width - wRatio, read as 0 for a pen that ignores pressure.  wRatio is shared with
  //  the speed and direction variants, so a fountain pen's direction range moves with it; 0 turns pressure
  //  off but leaves wRatio alone when another variant still needs it.
  Dim pressureSensitivity() const { return usesPressure() ? wRatio : 0; }
  void setPressureSensitivity(Dim amount)
  {
    amount = std::min(std::max(amount, Dim(0)), Dim(1));
    if(amount <= 0) {
      setFlag(WIDTH_PR, false);
      if(!hasVarWidth())
        wRatio = 0;
      return;
    }
    setFlag(WIDTH_PR, true);
    wRatio = amount;
    if(prParam == 0)
      prParam = 2;  // the default pens' curve; 0 would make the width ignore pressure entirely
  }

  // Line style.  What is stored - on a pen as dash/gap, on a path as stroke-dasharray - is the absolute
  //  pattern; the style is derived from it by measuring against the width, never stored, so a pattern
  //  written by anything else still reads as one of the three.  Dashes are sized in widths so that a
  //  thicker line gets a proportionally longer pattern.  A dot is a dash a tenth of a width long drawn
  //  with a round cap: a zero-length dash has no direction, and nanovg merges its two points and draws
  //  nothing.
  enum DashStyle { DASH_MIXED = -1, DASH_SOLID = 0, DASH_DASHED = 1, DASH_DOTTED = 2 };
  static int dashStyleOf(Dim dash, Dim gap, Dim width)
  {
    if(dash <= 0 && gap <= 0)
      return DASH_SOLID;
    return width > 0 && dash < 0.5*width ? DASH_DOTTED : DASH_DASHED;
  }
  static void dashFor(int style, Dim width, Dim* dash, Dim* gap)
  {
    *dash = style == DASH_DASHED ? 3*width : (style == DASH_DOTTED ? 0.1*width : 0);
    *gap = style == DASH_DASHED ? 3*width : (style == DASH_DOTTED ? 2*width : 0);
  }
  int dashStyle() const { return dashStyleOf(dash, gap, width); }
  void setDashStyle(int style) { dashFor(style, width, &dash, &gap); }

  Rect getBBox() const
  {
    Dim hw = 0.75*0.5*width;  //pressureparam > 0 ? 0.5*width * (1 - pow(1 - 0.66, pressureparam)) : 0.5*width;
    return Rect::ltrb(-hw, -hw, hw, hw);
  }
};
