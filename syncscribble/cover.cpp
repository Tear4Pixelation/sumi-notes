#include "cover.h"

#include <algorithm>
#include <cmath>
#include "ulib/image.h"
#include "ulib/oklab.h"
#include "ulib/stringutil.h"
#include "ugui/widgets.h"  // loadSVGFragment

namespace Cover {

// How far the band sits from the cover in OKLab lightness.  A dark cover is given a larger step: the
//  eye separates dark shades less well, and 0.16 on black is a band nobody sees.
static const real BAND_STEP_ON_LIGHT = 0.16;
static const real BAND_STEP_ON_DARK = 0.2;

static const real PREVIEW_RADIUS = 6;

real previewRadius(real w)
{
  return std::min(PREVIEW_RADIUS, w*real(0.05));
}

std::string outlineSVG(real w, real h)
{
  real radius = previewRadius(w);
  // inset half the stroke so the whole line is inside the preview's bounds and none of it is cut off
  return fstring("<rect class=\"preview-outline\" x=\"0.5\" y=\"0.5\" width=\"%.2f\" height=\"%.2f\""
      " rx=\"%.2f\" ry=\"%.2f\" fill=\"none\" stroke=\"#808080\" stroke-width=\"1\"/>",
      w - 1, h - 1, radius, radius);
}

void roundCorners(Image* image, real radius)
{
  unsigned char* bytes = image->bytes();
  int width = image->width, height = image->height;
  if(!bytes || radius <= 0)
    return;
  int extent = std::min(int(std::ceil(radius)), std::min(width, height)/2);
  for(int row = 0; row < extent; ++row) {
    for(int col = 0; col < extent; ++col) {
      // worked out for the top left corner; the other three are its mirror images
      real dx = radius - (col + real(0.5)), dy = radius - (row + real(0.5));
      real coverage = std::max(real(0), std::min(real(1), radius + real(0.5) - std::sqrt(dx*dx + dy*dy)));
      if(coverage >= 1)
        continue;
      const int cornerXs[2] = {col, width - 1 - col};
      const int cornerYs[2] = {row, height - 1 - row};
      for(int pixelY : cornerYs) {
        for(int pixelX : cornerXs) {
          // straight (not premultiplied) alpha, see Painter: only the alpha byte changes
          unsigned char& alpha = bytes[(pixelY*width + pixelX)*4 + 3];
          alpha = (unsigned char)(alpha*coverage + real(0.5));
        }
      }
    }
  }
  image->invalidate();
}

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
  real radius = previewRadius(w);
  // the band runs top to bottom near the spine, where a cloth-bound notebook has its seam
  real bandx = w*real(0.16), bandw = std::max(real(2), w*real(0.07));
  return fstring("<g class=\"cover-preview\">"
      "<rect width=\"%.2f\" height=\"%.2f\" rx=\"%.2f\" ry=\"%.2f\" fill=\"#%06X\"/>"
      "<rect x=\"%.2f\" width=\"%.2f\" height=\"%.2f\" fill=\"#%06X\"/>"
      // the same outline every preview has, drawn last so the band does not cover it: a black cover on
      //  the dark document list (or a white one on a light theme) is otherwise just a hole in the background
      "%s</g>", w, h, radius, radius, seed.rgb() & 0xFFFFFF, bandx, bandw, h, band.rgb() & 0xFFFFFF,
      outlineSVG(w, h).c_str());
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

SvgNode* framedImage(Image thumbnail, const Rect& frame, bool cropToFrame)
{
  if(cropToFrame) {
    real cropWidth = std::min(real(thumbnail.width), thumbnail.height/PREVIEW_ASPECT);
    real cropHeight = std::min(real(thumbnail.height), thumbnail.width*PREVIEW_ASPECT);
    if(int(cropWidth) < thumbnail.width || int(cropHeight) < thumbnail.height)
      thumbnail = thumbnail.cropped(Rect::wh(cropWidth, cropHeight));
  }
  roundCorners(&thumbnail, previewRadius(frame.width())*thumbnail.width/frame.width());
  SvgG* group = new SvgG();
  group->addChild(new SvgImage(std::move(thumbnail), frame.toSize()));
  group->addChild(loadSVGFragment(outlineSVG(frame.width(), frame.height()).c_str()));
  return group;
}

}  // namespace Cover
