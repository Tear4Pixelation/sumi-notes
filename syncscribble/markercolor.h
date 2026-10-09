#pragma once

// The marker's colors and the theme's snapping, kept apart from PenToolbar so the standalone color test
//  (scribbletest/colortest.cpp) can check them without a GUI.

#include <algorithm>
#include "ulib/oklab.h"
#include "ulib/palettegen.h"

// The generator's `hl` is deliberately timid (chroma capped at 0.13, alpha 97): it was tuned to sit under
//  text, and on white paper it read as a pale wash.  cusp-walk-1 is frozen, so the marker is made vivid
//  here instead - the same hue and lightness with the chroma cap lifted (still never out of gamut) and
//  a heavier alpha.  The marker is drawn *under* the ink (DRAW_UNDER), so more alpha does not cost
//  legibility.  It only changes the pen's colour, so strokes already in a document are untouched.
static const int MARKER_ALPHA = 165;
static const double MARKER_MAX_CHROMA = 0.19;

inline Color vividMarker(Color hl, double vividness)
{
  ColorOkLch lch = oklchFromColor(Color(hl.red(), hl.green(), hl.blue()));
  lch.C = std::min(oklchMaxChroma(lch.L, lch.h), real(MARKER_MAX_CHROMA))*vividness;
  return oklchToColor(lch, MARKER_ALPHA);
}

// The theme's reluctance (COLORS_SPEC.md §6.1): a pen color is answered with the nearest palette member,
//  keeping its alpha.  But a marker swatch is *deliberately* off the palette - vividMarker() lifts the
//  chroma past the generator's `hl` - so for the marker those colors are members too.  Snapping one
//  (which every swatch tap does, through PenToolbar::updateColor()) moved the pen to the nearest `hl` or
//  base, so the pen no longer equalled the swatch: no selection ring, and the second tap that opens the
//  swatch's editor never fired.  Only the families whose `hl` the boost actually changes were hit, which is
//  why it looked like "some" highlighter colors.  Compared without alpha, as Palette::indexOf() does.
inline Color snapThemedPenColor(const Palette& pal, Color c, bool marker)
{
  if(marker) {
    for(const PaletteFamily& family : pal.families) {
      if(vividMarker(family.hl, pal.recipe.vividness).opaque() == c.opaque())
        return c;
    }
  }
  Color snapped = pal.nearest(c);
  snapped.setAlpha(c.alpha());
  return snapped;
}
