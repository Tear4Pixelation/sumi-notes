// Unit tests for the thickness preset lists in syncscribble/widthpresets.cpp (docs/agent/pen-and-tools.md,
//  "Relative pen width").  Like layertest.cpp they need no GL context and no document, so they also build
//  and run on their own:
//
//   g++ -std=c++14 -O2 -DNDEBUG -I . -I syncscribble -DWIDTHPRESETTEST_MAIN
//       scribbletest/widthpresettest.cpp syncscribble/widthpresets.cpp -o widthpresettest && ./widthpresettest
//   (one command, run from the repo root.)
//
// runWidthPresetTests() returns the number of failed checks and is also called from ScribbleTest::runAll().
// What needs the toolbar itself - a selection showing a relative pen's presets as absolute, and a
//  width edited there landing back in line heights - is ScribbleTest::penWidthPresetTest().

#include <stdio.h>
#include <cmath>

#include "widthpresets.h"

static int nWidthPresetChecksFailed = 0;

static void widthPresetCheck(bool condition, const char* what)
{
  if(!condition) {
    ++nWidthPresetChecksFailed;
    printf("FAIL: %s\n", what);
  }
}

static bool nearlyEqual(Dim a, Dim b) { return std::abs(a - b) <= 1e-6*std::max(Dim(1), std::abs(b)); }

static const Dim TEST_DEFAULTS[] = {1.4, 3.0, 6.0};  // document units, like PenToolbar's DEFAULT_WIDTHS
static const Dim LINE_HEIGHT = 45;

// The unit travels in the same string as the numbers, so a save can never write one without the other
//  and a load can never pair a list with the wrong unit - the root of the 144 / 0.6 wide pens.
static void testWidthPresetRoundTrip()
{
  WidthPresets rel;
  rel.parse("rel:0.04,0.075,0.15");
  widthPresetCheck(rel.unit == WidthPresets::UNIT_RELATIVE, "rel: prefix reads as relative");
  widthPresetCheck(rel.widths.size() == 3 && nearlyEqual(rel.widths[1], 0.075), "rel: numbers parse");
  WidthPresets back;
  back.parse(rel.serialize().c_str());
  widthPresetCheck(back.unit == WidthPresets::UNIT_RELATIVE && back.widths == rel.widths,
      "a relative list survives serialize/parse with its unit");

  WidthPresets abs;
  abs.parse("abs:1.4,3,6");
  widthPresetCheck(abs.unit == WidthPresets::UNIT_ABSOLUTE && abs.widths.size() == 3, "abs: prefix reads as absolute");

  WidthPresets legacy;
  legacy.parse("1.4, 3.0, 6.0");
  widthPresetCheck(legacy.unit == WidthPresets::UNIT_UNKNOWN && legacy.widths.size() == 3,
      "a list without a prefix is legacy: unit unknown, numbers kept");
  // an unknown list adopts the unit it is first used in rather than being converted from a guess
  legacy.convertTo(true, LINE_HEIGHT);
  widthPresetCheck(legacy.isRelative() && nearlyEqual(legacy.widths[0], 1.4), "an unknown unit is adopted, not converted");

  // a pen width in line heights is around 0.04: "%.3f" (the old format) moved 1.6 units to 0.036 lines,
  //  which comes back as 1.62 and no longer selects its preset
  WidthPresets fine;
  fine.unit = WidthPresets::UNIT_RELATIVE;
  fine.widths = {1.6/LINE_HEIGHT, 3.0/LINE_HEIGHT, 6.0/LINE_HEIGHT};
  WidthPresets fineBack;
  fineBack.parse(fine.serialize().c_str());
  widthPresetCheck(WidthPresets::sameWidth(fineBack.inUnit(0, false, LINE_HEIGHT), 1.6),
      "a small relative preset survives the config round trip precisely enough to stay selected");
}

// A selection is absolute even while the pen's list is relative: the row shows the presets resolved,
//  and a width edited there goes back into the list in line heights.  Storing it raw is what made a
//  3.2 unit stroke into a 3.2 line height (144 unit) pen preset.
static void testWidthPresetSelectionUnits()
{
  WidthPresets presets;
  presets.parse("rel:0.04,0.08,0.16");
  widthPresetCheck(nearlyEqual(presets.inUnit(1, false, LINE_HEIGHT), 0.08*LINE_HEIGHT),
      "a relative preset shown to a selection is in document units");
  widthPresetCheck(nearlyEqual(presets.inUnit(1, true, LINE_HEIGHT), 0.08), "and to a relative pen as is");
  presets.setFrom(1, 3.2, false, LINE_HEIGHT);
  widthPresetCheck(nearlyEqual(presets.widths[1], 3.2/LINE_HEIGHT),
      "an absolute width stored into a relative list is converted to line heights");
  widthPresetCheck(presets.isRelative(), "storing a selection's width leaves the list relative");

  WidthPresets absPresets;
  absPresets.parse("abs:1.4,3,6");
  absPresets.setFrom(0, 0.1, true, LINE_HEIGHT);
  widthPresetCheck(nearlyEqual(absPresets.widths[0], 0.1*LINE_HEIGHT),
      "a relative width stored into an absolute list is converted to units");
}

// The toggle converts, it does not reinterpret: nothing changes thickness when the unit flips
static void testWidthPresetConvert()
{
  WidthPresets presets;
  presets.parse("abs:1.8,3.6,7.2");
  presets.convertTo(true, LINE_HEIGHT);
  widthPresetCheck(presets.isRelative() && nearlyEqual(presets.widths[2], 7.2/LINE_HEIGHT), "absolute to relative divides");
  presets.convertTo(true, LINE_HEIGHT);
  widthPresetCheck(nearlyEqual(presets.widths[2], 7.2/LINE_HEIGHT), "converting to the unit it is already in is a no-op");
  presets.convertTo(false, LINE_HEIGHT);
  widthPresetCheck(!presets.isRelative() && nearlyEqual(presets.widths[2], 7.2), "relative to absolute multiplies back");
}

// Always three, always editable: a preset outside the spinbox limits can never be selected exactly
//  (the spinbox clamps it), so tapping it never opens its editor - the only way out was Delete, which
//  lost the slot for good.
static void testWidthPresetNormalize()
{
  WidthPresets stuck;
  stuck.parse("rel:0.04,144,0.16");
  widthPresetCheck(stuck.normalize(TEST_DEFAULTS, false, LINE_HEIGHT), "normalize reports a clamp");
  widthPresetCheck(stuck.widths.size() == 3 && nearlyEqual(stuck.widths[1], WidthPresets::MAX_RELATIVE),
      "a relative preset past the spinbox limit is clamped into it, in place");

  WidthPresets absStuck;
  absStuck.parse("abs:1.4,270,0.001");
  absStuck.normalize(TEST_DEFAULTS, false, LINE_HEIGHT);
  widthPresetCheck(nearlyEqual(absStuck.widths[1], WidthPresets::MAX_ABSOLUTE)
      && nearlyEqual(absStuck.widths[2], WidthPresets::MIN_WIDTH), "absolute presets are clamped to both limits");

  WidthPresets two;
  two.parse("abs:1.0,2.0");
  widthPresetCheck(two.normalize(TEST_DEFAULTS, false, LINE_HEIGHT), "normalize reports a refill");
  widthPresetCheck(two.widths.size() == WidthPresets::COUNT, "two presets are recovered to three");
  widthPresetCheck(two.widths.size() == 3 && nearlyEqual(two.widths[0], 1.0) && nearlyEqual(two.widths[1], 2.0)
      && nearlyEqual(two.widths[2], 6.0),
      "the missing slot gets that slot's default and the user's two are kept");

  WidthPresets twoRel;
  twoRel.parse("rel:0.05,0.1");
  twoRel.normalize(TEST_DEFAULTS, false, LINE_HEIGHT);
  widthPresetCheck(twoRel.widths.size() == 3 && nearlyEqual(twoRel.widths[2], 6.0/LINE_HEIGHT),
      "a refilled default is converted into the list's unit");

  WidthPresets four;
  four.parse("abs:1.6,2.4,3.0,4.0");  // the old built-in fallback had four
  four.normalize(TEST_DEFAULTS, false, LINE_HEIGHT);
  widthPresetCheck(four.widths.size() == WidthPresets::COUNT && nearlyEqual(four.widths[2], 3.0), "extra presets are dropped");

  WidthPresets empty;
  empty.unit = WidthPresets::UNIT_RELATIVE;
  empty.normalize(TEST_DEFAULTS, false, LINE_HEIGHT);
  widthPresetCheck(empty.widths.size() == 3 && nearlyEqual(empty.widths[0], 1.4/LINE_HEIGHT), "an empty list is seeded");

  WidthPresets junk;
  junk.unit = WidthPresets::UNIT_ABSOLUTE;
  junk.widths = {NAN, 3.0, -1};
  junk.normalize(TEST_DEFAULTS, false, LINE_HEIGHT);
  bool allFinite = junk.widths.size() == 3;
  for(Dim width : junk.widths)
    allFinite = allFinite && std::isfinite(width) && width > 0;
  widthPresetCheck(allFinite, "non-finite and negative presets are replaced");

  WidthPresets fine;
  fine.parse("abs:1.4,3,6");
  widthPresetCheck(!fine.normalize(TEST_DEFAULTS, false, LINE_HEIGHT), "a good list is left alone");
}

int runWidthPresetTests()
{
  nWidthPresetChecksFailed = 0;
  testWidthPresetRoundTrip();
  testWidthPresetSelectionUnits();
  testWidthPresetConvert();
  testWidthPresetNormalize();
  return nWidthPresetChecksFailed;
}

#ifdef WIDTHPRESETTEST_MAIN
int main()
{
  int failed = runWidthPresetTests();
  printf(failed ? "%d width preset checks failed\n" : "all width preset checks passed\n", failed);
  return failed ? 1 : 0;
}
#endif
