#pragma once

#include <unordered_map>
#include "ulib/painter.h"
#include "ulib/palettegen.h"

// Night mode: dark paper as a *view*, never a document edit (docs/agent/night-mode.md).
//
// Installed on the Painter for the canvas only, so the file, exports, thumbnails and anything copied out
//  keep the document's own colors - which is why a clip taken in night mode needs no "uninvert" step.
//
// - A themed document is drawn in its mirrored theme (paperL -> 1 - paperL): each ink maps by ordinal and
//   variant through Palette::mapFrom(), the paper/rule/link/bookmark/selection slots map to theirs.
// - Anything else (unthemed ink, off-palette strokes, PDF vector content) has its OKLab lightness flipped
//   with hue kept, so blue stays blue - unlike the old XOR, which turned it yellow.
class NightColorMap : public ColorMap
{
public:
  // pal == NULL for a document without a theme; cheap when nothing changed
  void setPalette(const Palette* pal);
  Color map(Color c) const override;
  // images (PDF pages, scans): flipped only if they look like a document - see isDocumentLike()
  int imageKey() const override { return generation; }
  bool mapImagePixels(unsigned char* rgba, int w, int h) const override;

  Color flipLightness(Color c) const;
  static bool isDocumentLike(const unsigned char* rgba, int w, int h);

private:
  Color mapUncached(Color c) const;

  bool themed = false;
  bool valid = false;
  // bumped whenever the mapping changes, so cached image textures made under the old one are redone
  int generation = 0;
  // where white paper lands: the mirrored theme's own paper, so a PDF page matches the page around it
  real paperL = 0;
  PaletteRecipe recipe;
  Palette light;
  Palette dark;
  // strokes share a handful of colors, so a frame is almost all hits on the last-color memo
  mutable std::unordered_map<color_t, color_t> cache;
  mutable color_t lastIn = 0, lastOut = 0;
  mutable bool lastValid = false;
  // luma -> flipped luma for images, matching flipLightness() on grays
  mutable unsigned char lumaLut[256];
  mutable bool lumaLutValid = false;
};
