#pragma once

#include "ugui/widgets.h"
#include "ulib/image.h"
#include "basics.h"
#include "cropshape.h"

// Screenshot of a page region and image editing (docs/agent/screenshot.md): select an area, then capture
//  what is in and around it - ink plus any PDF or scan image under it - as an image to copy or add to a
//  page; or open an image already in the document in the same dialog to crop it again.

class Page;

// what a capture includes; the paper itself never is - it belongs to the page copied from
// SHOT_KEEP_WHITE (only with SHOT_BACKGROUND) skips the white-to-alpha step, so the PDF page keeps its white
enum ScreenshotLayers { SHOT_RULING = 1, SHOT_BACKGROUND = 2, SHOT_INK = 4, SHOT_KEEP_WHITE = 8,
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
// The part of `src` inside `shape`: cropped to the shape's pixel bounds (returned in `pixelBounds`, in
//  src's pixels) and, unless the shape is a plain rectangle, transparent outside it (then always PNG).
//  A plain rectangle keeps the source's encoding, so a cropped photo stays a JPEG.
Image cropToShape(const Image& src, const CropShape& shape, Rect* pixelBounds);

// Edited images keep their original pixels so a later edit can uncrop (screenshot.md, "Original pixels"):
//  IMAGE_CROP_ATTR holds the CropShape (in the original's pixels), IMAGE_ORIG_ATTR the original as a data
//  URI.  Both go when an edit returns the image to the whole original.
extern const char* IMAGE_CROP_ATTR;
extern const char* IMAGE_ORIG_ATTR;
class SvgImage;
// Sets both attributes on `node` (or removes them when `shape` is the whole original).  `origDataUri`: the
//  original's attribute as already stored, reused as is so a re-edit does not re-encode it; else the
//  original's encoded bytes are written (its file bytes when it has them, so a JPEG is not recompressed).
void setImageOriginal(SvgImage* node, const Image& original, const CropShape& shape, const char* origDataUri = NULL);
// the original pixels and the shape of an image edited before; false for any other image
bool getImageOriginal(const SvgImage* node, Image* original, CropShape* shape);

class ImageCropWidget;
class Slider;

// The image, centered, with handles for the crop shape: a rectangle with a handle on each corner (free
//  aspect), or a polygon of points the user places, either one with rounded corners.  The shape bar below
//  the image (a floating bar, ui-floating-bar.md) switches between them and sets the radius.
//  As a screenshot it offers checkboxes for what to include and Copy / Add to page; editing an image it
//  offers Apply.
class ImageCropDialog : public PopupDialog
{
public:
  enum Choice { CHOICE_NONE, CHOICE_COPY, CHOICE_ADD, CHOICE_APPLY };
  // renders the region with the given ScreenshotLayers; called again whenever a checkbox changes, always
  //  at the same size, so the crop carries over
  typedef std::function<Image(int layers)> RenderFn;

  // a screenshot: hasBackground offers the keep-white option; initialCrop (pixels of the render) is where
  //  the crop starts - the selection, inside a capture taken wider so the crop can grow
  ImageCropDialog(RenderFn render, bool hasBackground, const Rect& initialCrop);
  // editing an image: its original pixels and the shape it is cropped to now
  ImageCropDialog(Image original, const CropShape& initial);

  // meaningful once finished with ACCEPTED
  CropShape cropShape() const;
  Image takeCropped(Rect* pixelBounds) const { return cropToShape(source, cropShape(), pixelBounds); }
  const Image& sourceImage() const { return source; }

  Choice choice = CHOICE_NONE;

private:
  void build(bool screenshot, bool hasBackground, const CropShape& initial);
  Widget* createShapeBar();
  void shapeChanged();
  void rerender();

  RenderFn renderFn;
  Image source;
  ImageCropWidget* cropWidget = NULL;
  CheckBox* cbRuling = NULL;
  CheckBox* cbBackground = NULL;
  CheckBox* cbInk = NULL;
  CheckBox* cbKeepWhite = NULL;
  std::vector<Button*> acceptButtons;
  Button* rectBtn = NULL;
  Button* polyBtn = NULL;
  Button* removePointBtn = NULL;
  Slider* radiusSlider = NULL;
  SvgText* hintText = NULL;
};

// the dialog's and its shape bar's rules (theme variables only), added to the app stylesheet
extern const char* imageCropStylesheet;
