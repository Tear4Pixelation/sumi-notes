// Unit tests for the marker's Center on Line offset (syncscribble/centeroffset.h, docs/agent/pen-and-tools.md,
//  "Marker: centre on line").  No GL and no document, so it builds on its own:
//
//   g++ -std=c++14 -O2 -DNDEBUG -I syncscribble -DMARKEROFFSETTEST_MAIN scribbletest/markeroffsettest.cpp
//       -o markeroffsettest && ./markeroffsettest      (one command, from the repo root)
//
// runMarkerOffsetTests() returns the number of failed checks and is also called from ScribbleTest::runAll(),
//  where the storage checks also run against the real ScribblePen.

#include <stdio.h>
#include <cmath>
#include <string>

#include "centeroffset.h"
#ifndef MARKEROFFSETTEST_MAIN
#include "scribblepen.h"
#endif

static int nMarkerOffsetChecksFailed = 0;

static void markerOffsetCheck(bool condition, const char* what)
{
  if(!condition) {
    ++nMarkerOffsetChecksFailed;
    printf("FAIL: %s\n", what);
  }
}

static bool markerOffsetNear(double a, double b) { return std::fabs(a - b) < 1e-9; }

static void testMarkerOffsetPlacement()
{
  const double top = 100, lineHeight = 40, width = 10;
  double bottomRule = top + lineHeight;
  // 0 is the old behaviour: the middle of the line
  markerOffsetCheck(markerOffsetNear(CenterOffset::centerY(top, lineHeight, width, 0), top + lineHeight/2),
      "offset 0 centres on the line");
  // +1: the top edge of the stroke on the top ruling; -1: the bottom edge on the bottom ruling
  markerOffsetCheck(markerOffsetNear(CenterOffset::centerY(top, lineHeight, width, 1) - width/2, top),
      "offset +1 glues the stroke's top edge to the top ruling");
  markerOffsetCheck(markerOffsetNear(CenterOffset::centerY(top, lineHeight, width, -1) + width/2, bottomRule),
      "offset -1 glues the stroke's bottom edge to the bottom ruling");
  // between: linear
  double halfway = CenterOffset::centerY(top, lineHeight, width, 0.5);
  markerOffsetCheck(markerOffsetNear(halfway, (CenterOffset::centerY(top, lineHeight, width, 0)
      + CenterOffset::centerY(top, lineHeight, width, 1))/2), "offset 0.5 is halfway between centre and top");
  // out of range clamps, NaN is centred
  markerOffsetCheck(markerOffsetNear(CenterOffset::centerY(top, lineHeight, width, 7),
      CenterOffset::centerY(top, lineHeight, width, 1)), "offset above 1 clamps");
  markerOffsetCheck(markerOffsetNear(CenterOffset::centerY(top, lineHeight, width, NAN), top + lineHeight/2),
      "NaN offset is centred");
  // a marker exactly one line tall (or taller) has no room: every offset is the centre
  for(double offset : {-1.0, -0.3, 0.0, 0.6, 1.0}) {
    markerOffsetCheck(markerOffsetNear(CenterOffset::centerY(top, lineHeight, lineHeight, offset),
        top + lineHeight/2), "a one line marker ignores the offset");
    markerOffsetCheck(markerOffsetNear(CenterOffset::centerY(top, lineHeight, 1.5*lineHeight, offset),
        top + lineHeight/2), "a marker taller than the line ignores the offset");
  }
}

// the offset lives in flags bits 16-23: exact for every hundredth, and never touching the real flags
static void testMarkerOffsetStorage()
{
  for(int steps = -100; steps <= 100; ++steps) {
    double offset = steps/100.0;
    markerOffsetCheck(CenterOffset::unpack(CenterOffset::pack(offset)) == offset,
        "every hundredth round trips exactly");
    markerOffsetCheck((CenterOffset::pack(offset) & ~CenterOffset::MASK) == 0, "pack stays inside its bits");
  }
  markerOffsetCheck(CenterOffset::unpack(0x7FFF) == 0, "a pen saved before the offset existed reads as centred");
  markerOffsetCheck(CenterOffset::unpack(CenterOffset::pack(2.5)) == 1.0, "packing clamps");
  markerOffsetCheck(CenterOffset::unpack(0x7Fu << CenterOffset::SHIFT) == 1.0, "a corrupt byte clamps on reading");
#ifndef MARKEROFFSETTEST_MAIN
  ScribblePen marker(Color::BLACK, 1,
      ScribblePen::CENTER_ON_LINE | ScribblePen::WIDTH_RELATIVE | ScribblePen::TIP_FLAT);
  marker.setCenterOffset(-0.37);
  markerOffsetCheck(marker.centerOffset() == Dim(-0.37), "the pen stores the offset");
  markerOffsetCheck(marker.hasFlag(ScribblePen::CENTER_ON_LINE) && marker.hasFlag(ScribblePen::WIDTH_RELATIVE)
      && marker.hasFlag(ScribblePen::TIP_FLAT), "setting the offset leaves the flags alone");
  marker.setFlag(ScribblePen::CENTER_ON_LINE, false);
  markerOffsetCheck(marker.centerOffset() == Dim(-0.37), "toggling centre on line leaves the offset");
  // through the config's text form: flags is written as a plain integer
  ScribblePen loaded(Color::BLACK, 1);
  loaded.flags = (unsigned int)std::stoul(std::to_string(marker.flags));
  markerOffsetCheck(loaded.centerOffset() == marker.centerOffset(), "the offset survives the saved text form");
  ScribblePen other = marker;
  other.setCenterOffset(0.2);
  markerOffsetCheck(!(other == marker), "pens with different offsets are different pens");
#endif
}

int runMarkerOffsetTests()
{
  nMarkerOffsetChecksFailed = 0;
  testMarkerOffsetPlacement();
  testMarkerOffsetStorage();
  return nMarkerOffsetChecksFailed;
}

#ifdef MARKEROFFSETTEST_MAIN
int main()
{
  int failed = runMarkerOffsetTests();
  printf(failed ? "%d marker offset checks failed\n" : "all marker offset checks passed\n", failed);
  return failed ? 1 : 0;
}
#endif
