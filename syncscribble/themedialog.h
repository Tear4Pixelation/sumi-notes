#pragma once

#include "ugui/widgets.h"
#include "ulib/palettegen.h"

class ScribbleDoc;

// Theme picker (COLORS_SPEC.md §6.4).
//
// The gallery is *generated*, not curated: seeds are spread evenly around the hue wheel and every
//  thumbnail is built by the same generator the document will use. There is no hand-picked list to
//  drift out of step with the algorithm.
//
// Applies to the current document, whether or not it has been saved - an untitled document has a
//  per-document config exactly like any other, so it needs no special case. "Use for new documents"
//  writes the same recipe to the global config, which is what an untitled document inherits when it
//  has no theme of its own.
class ThemeDialog : public PopupDialog
{
public:
  ThemeDialog(ScribbleDoc* doc);
  void accept();

private:
  void selectSeed(real hue);
  void rebuildGallery();
  void updatePreview();
  PaletteRecipe currentRecipe() const;

  ScribbleDoc* scribbleDoc;
  PaletteRecipe recipe;

  Widget* gallery;
  Widget* preview;
  Slider* sliderSeed;
  Slider* sliderVivid;
  Slider* sliderDepth;
  CheckBox* cbDarkPaper;
  CheckBox* cbApplyPages;
  CheckBox* cbGlobalDefault;
  CheckBox* cbOffPalette;
  CheckBox* cbRestyle;
  std::vector<Button*> galleryBtns;
};
