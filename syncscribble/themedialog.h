#pragma once

#include "ugui/widgets.h"
#include "ulib/palettegen.h"

class ScribbleDoc;

// Theme picker (COLORS_SPEC.md §6.4).
//
// A grid of the shipped themes, plus one toggle for light/dark paper.  No sliders: of the recipe's
//  knobs, seed hue was measured to move a full-wheel palette by less than its own jitter, and
//  vividness already defaults to its maximum, so both could only make a theme worse.  What is left -
//  ink character and paper tint - is what the themes themselves vary, and picking one from rendered
//  strokes is a better question to ask than "how vivid, 0 to 1".
//
// Every tile is drawn as strokes on that theme's own paper, never as swatch chips: a color that reads
//  fine as a 40px block can be invisible as a 3px line, which is the whole point of the generator.
// The dark toggle rebuilds all of them, so the grid always shows what you will actually get.
//
// Applies to the current document, whether or not it has been saved - an untitled document has a
//  per-document config exactly like any other, so it needs no special case.
class ThemeDialog : public PopupDialog
{
public:
  ThemeDialog(ScribbleDoc* doc);
  void accept();

private:
  void selectTheme(int index);
  void rebuildGallery();

  ScribbleDoc* scribbleDoc;
  PaletteRecipe recipe;
  // which shipped theme is selected, or -1 for a recipe that is not one of ours (a document themed by
  //  an older build, or by a newer one)
  int themeIndex = -1;

  Widget* gallery;
  CheckBox* cbDarkPaper;
  CheckBox* cbApplyPages;
  CheckBox* cbOffPalette;
  CheckBox* cbRestyle;
  std::vector<Button*> galleryBtns;
};
