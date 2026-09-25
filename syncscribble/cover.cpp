#include "cover.h"

#include <algorithm>
#include "ulib/oklab.h"
#include "ulib/stringutil.h"

namespace Cover {

// How far the band sits from the cover in OKLab lightness.  A dark cover is given a larger step: the
//  eye separates dark shades less well, and 0.16 on black is a band nobody sees.
static const real BAND_STEP_ON_LIGHT = 0.16;
static const real BAND_STEP_ON_DARK = 0.2;

Color bandColor(Color seed)
{
  ColorOkLch lch = oklchFromColor(seed);
  lch.L = lch.L > real(0.5) ? lch.L - BAND_STEP_ON_LIGHT : lch.L + BAND_STEP_ON_DARK;
  // the hue can hold less chroma at the new lightness; keeping the old number would push it out of gamut,
  //  where oklchToColor() clamps per channel and shifts the hue
  lch.C = std::min(lch.C, oklchMaxChroma(lch.L, lch.h));
  return oklchToColor(lch);
}

std::string coverSVG(Color seed, real w, real h)
{
  Color band = bandColor(seed);
  // the band runs top to bottom near the spine, where a cloth-bound notebook has its seam
  real bandx = w*real(0.16), bandw = std::max(real(2), w*real(0.07));
  return fstring("<g class=\"cover-preview\">"
      "<rect width=\"%.2f\" height=\"%.2f\" rx=\"3\" ry=\"3\" fill=\"#%06X\"/>"
      "<rect x=\"%.2f\" width=\"%.2f\" height=\"%.2f\" fill=\"#%06X\"/>"
      // a faint edge, drawn last so the band does not cover it: a black cover on the dark document list
      //  (or a white one on a light theme) is otherwise just a hole in the background
      "<rect x=\"0.5\" y=\"0.5\" width=\"%.2f\" height=\"%.2f\" rx=\"3\" ry=\"3\" fill=\"none\""
      " stroke=\"#808080\" stroke-opacity=\"0.45\" stroke-width=\"1\"/>"
      "</g>", w, h, seed.rgb() & 0xFFFFFF, bandx, bandw, h, band.rgb() & 0xFFFFFF, w - 1, h - 1);
}

const std::vector<Color>& presetSeeds()
{
  static std::vector<Color> seeds;
  if(seeds.empty()) {
    // muted rather than vivid: these are the colors of book cloth and leather, which is what a cover is
    for(int ii = 0; ii < 8; ++ii)
      seeds.push_back(oklchToColor(ColorOkLch(0.52, 0.1, 20 + 45*ii)));
    seeds.push_back(oklchToColor(ColorOkLch(0.2, 0, 0)));        // black
    seeds.push_back(oklchToColor(ColorOkLch(0.45, 0.02, 250)));  // slate
    seeds.push_back(oklchToColor(ColorOkLch(0.9, 0.035, 85)));   // cream
    seeds.push_back(oklchToColor(ColorOkLch(0.98, 0, 0)));       // white
  }
  return seeds;
}

}  // namespace Cover
