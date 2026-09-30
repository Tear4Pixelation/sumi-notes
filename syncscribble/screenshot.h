#pragma once

#include "ugui/widgets.h"
#include "ulib/image.h"
#include "basics.h"

// Screenshot of a page region (docs/agent/screenshot.md): select an area, then capture what is in its
//  bounding box - ink plus any PDF or scan image under it - as an image to copy or add to a page.

class Page;

// what a capture includes; the paper itself never is - it belongs to the page copied from
enum ScreenshotLayers { SHOT_RULING = 1, SHOT_BACKGROUND = 2, SHOT_INK = 4,
    SHOT_DEFAULT = SHOT_BACKGROUND | SHOT_INK };

// Renders `region` (page units) of `page` at `scale` pixels per unit, in the document's own colors
//  (never night mode), with white turned into transparency.  `layers`: ruling lines, the page's PDF or
//  scan image, and the ink.
Image renderPageRegion(Page* page, const Rect& region, Dim scale, int layers = SHOT_DEFAULT);
// pixels per page unit for a region: ~300 dpi, reduced so the longer side stays within a sane size
Dim screenshotScale(const Rect& region);
// Color-to-alpha against white: every pixel becomes the most transparent color that, over white, looks
//  exactly as it did.  Paper disappears, ink keeps its look on white, and anti-aliased edges stay clean
//  on any other paper.  Exposed for the standalone test.
void whiteToAlpha(unsigned char* rgba, int w, int h);

class ScreenshotCropWidget;

// The screenshot, centered, with a draggable handle on each corner for a free-aspect crop, checkboxes
//  below it for what to include, and the two ways out - Copy and Add to page - at the top right.
class ScreenshotDialog : public PopupDialog
{
public:
  enum Choice { CHOICE_NONE, CHOICE_COPY, CHOICE_ADD };
  // renders the region with the given ScreenshotLayers; called again whenever a checkbox changes, always
  //  at the same size, so the crop carries over
  typedef std::function<Image(int layers)> RenderFn;

  explicit ScreenshotDialog(RenderFn render);
  // the crop in image pixels, and the cropped image; meaningful once finished with ACCEPTED
  Rect cropRect() const;
  Image takeCropped() const;

  Choice choice = CHOICE_NONE;

private:
  void rerender();

  RenderFn renderFn;
  Image source;
  ScreenshotCropWidget* cropWidget;
  CheckBox* cbRuling;
  CheckBox* cbBackground;
  CheckBox* cbInk;
};
