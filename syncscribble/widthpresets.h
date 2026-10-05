#pragma once

// A draw tool's thickness presets, together with the unit they are in (docs/agent/pen-and-tools.md,
//  "Relative pen width").  The unit used to be implied by the pen's WIDTH_RELATIVE flag, which lives in
//  a different config value (toolModes) and a different object (the pen) - and every place the two
//  could disagree (a selection's absolute pen on the row, a width edited there, a saved pen with the
//  other flag, a pen that failed to parse) turned the whole row into hairlines or slabs.  So the list
//  carries its own unit, it is written to the config in the same string as the numbers, and anything
//  that wants a preset in some other unit asks for it in that unit (inUnit) instead of reading the raw
//  number and hoping.
// Pure math with no GUI dependency, so it builds standalone (scribbletest/widthpresettest.cpp).

#include <string>
#include <vector>

// the same typedef as basics.h (repeating an identical typedef is legal), so this builds without it
typedef double Dim;

struct WidthPresets
{
  // prefixed: wingdi.h #defines ABSOLUTE and RELATIVE
  enum Unit { UNIT_UNKNOWN = -1, UNIT_ABSOLUTE = 0, UNIT_RELATIVE = 1 };
  // the row always has exactly this many: the presets are edited in place and never added or deleted
  static constexpr int COUNT = 3;
  // the width spinbox's limits, which a preset must stay within - one outside them can never be
  //  selected exactly (the spinbox clamps it), so tapping it never opens its editor and it is stuck
  static constexpr Dim MIN_WIDTH = 0.01;
  static constexpr Dim MAX_ABSOLUTE = 200;
  static constexpr Dim MAX_RELATIVE = 4;  // in line heights, 4 is already absurdly thick
  static Dim maxWidth(bool relative) { return relative ? MAX_RELATIVE : MAX_ABSOLUTE; }

  std::vector<Dim> widths;
  // UNIT_UNKNOWN only for a list read from a config written before the unit was stored with it; it is
  //  resolved (once) from that tool's pen, which is the pairing the old code relied on
  int unit = UNIT_UNKNOWN;

  bool isRelative() const { return unit == UNIT_RELATIVE; }
  // "rel:0.035,0.075,0.150" / "abs:1.400,3.000,6.000"; a plain number list is a legacy one, UNIT_UNKNOWN
  void parse(const char* str);
  std::string serialize() const;
  // re-expresses the list in the given unit, so nothing changes thickness: a list of unknown unit simply
  //  adopts the unit, since there is nothing to convert from
  void convertTo(bool relative, Dim lineHeight);
  // exactly COUNT presets, each a finite number within the spinbox limits of the list's unit.  Missing
  //  slots are filled from `defaults` (COUNT values, in `defaultsRelative` units) and the row is then
  //  sorted thin to thick; extra ones are dropped.  Returns true if anything changed.
  bool normalize(const Dim* defaults, bool defaultsRelative, Dim lineHeight);
  // preset idx expressed in the asked unit, e.g. absolute for a selection while the pen is relative
  Dim inUnit(size_t idx, bool relative, Dim lineHeight) const;
  // sets preset idx from a width given in the asked unit
  void setFrom(size_t idx, Dim width, bool relative, Dim lineHeight);
  // whether two widths are the same preset: a width converted to and from line heights is not
  //  bit-identical, and an exact compare would leave the row with nothing selected
  static bool sameWidth(Dim a, Dim b);
};
