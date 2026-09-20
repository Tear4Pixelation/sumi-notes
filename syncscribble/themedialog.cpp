#include "themedialog.h"

#include "scribbleapp.h"
#include "scribbledoc.h"
#include "mainwindow.h"
#include "touchwidgets.h"
#include "page.h"

// Gallery thumbnails: big enough that a stroke reads as a stroke rather than as a chip.  Swatch grids
//  lie about ink - a color that looks fine as a 40px block can be invisible as a 3px line - so every
//  preview here is drawn as strokes on that theme's own paper, at the weight writing actually has.
// The old 92x58 tiles were half of why the themes looked alike: at that size the ink is a hairline
//  and only the paper is legible.
static const Dim THUMB_W = 150;
static const Dim THUMB_H = 112;
static const Dim THUMB_LABEL_H = 17;   // room under the preview for the theme's name
static const int GALLERY_COLS = 3;
// Eight rows against twelve families: palettePreviewSVG() samples them evenly, so this is a spread
//  across the wheel rather than the first eight.  Not one row per family - twelve rows in 112px puts
//  the strokes ~9px apart, and thinning them back to fit is exactly what made the old tiles unreadable.
static const int GALLERY_ROWS = 8;

// a handwriting-ish squiggle; deterministic so previews do not dance while a slider is dragged
static std::string squigglePath(Dim x, Dim y, Dim w, int seed)
{
  std::string d = fstring("M %g %g", double(x), double(y));
  const int n = std::max(3, int(w/14));
  for(int ii = 0; ii < n; ++ii) {
    // cheap deterministic hash of (seed, ii) in [0,1)
    auto rnd = [&](int k) {
      unsigned int h = (unsigned int)(seed*2654435761u + (ii*3 + k)*40503u);
      h ^= h >> 13; h *= 2246822519u; h ^= h >> 16;
      return real(h % 1000)/real(1000);
    };
    Dim x0 = x + (w*ii)/n, x1 = x + (w*(ii + 1))/n;
    d += fstring(" C %g %g, %g %g, %g %g",
        double(x0 + (x1 - x0)*0.33), double(y - 5*(0.4 + rnd(0))),
        double(x0 + (x1 - x0)*0.66), double(y + 5*(0.4 + rnd(1))),
        double(x1), double(y + (rnd(2) - 0.5)*3));
  }
  return d;
}

// Renders a palette the way it will actually be used: strokes on its own paper, over its own ruling.
static std::string palettePreviewSVG(const Palette& pal, Dim boxw, Dim boxh, int nrows)
{
  std::string svg = fstring("<svg width='%g' height='%g' viewBox='0 0 %g %g'>", double(boxw),
      double(boxh), double(boxw), double(boxh));
  svg += fstring("<rect x='0' y='0' width='%g' height='%g' fill='#%06X'/>",
      double(boxw), double(boxh), pal.paper.rgb() & 0xFFFFFF);

  Dim rowh = boxh/(nrows + 1);
  for(int ii = 0; ii < nrows; ++ii) {
    Dim y = rowh*(ii + 1);
    svg += fstring("<line x1='4' y1='%g' x2='%g' y2='%g' stroke='#%06X' stroke-opacity='0.5'"
        " stroke-width='0.75'/>", double(y + rowh*0.35), double(boxw - 4), double(y + rowh*0.35),
        pal.rule.rgb() & 0xFFFFFF);
  }
  // Stroke weights: the wide preview draws at the weight writing actually has, the narrow one has to
  //  thin down to fit.  Keyed off a named threshold rather than a bare `boxw > 150`, which the tile
  //  width silently failed by one unit when it was raised to exactly 150.
  bool wide = boxw >= 140;
  // one family per row, cycling if there are more families than rows
  for(int ii = 0; ii < nrows; ++ii) {
    const PaletteFamily& fam = pal.families[(ii*int(pal.families.size())/nrows) % pal.families.size()];
    Dim y = rowh*(ii + 1);
    svg += fstring("<path d='%s' fill='none' stroke='#%06X' stroke-width='%g' stroke-linecap='round'/>",
        squigglePath(5, y, boxw*0.62 - 5, ii + 1).c_str(), fam.base.rgb() & 0xFFFFFF,
        double(wide ? 2.4 : 1.7));
    svg += fstring("<path d='%s' fill='none' stroke='#%06X' stroke-width='%g' stroke-linecap='round'/>",
        squigglePath(boxw*0.66, y, boxw*0.3, ii + 40).c_str(), fam.dark.rgb() & 0xFFFFFF,
        double(wide ? 4.4 : 3.0));
  }
  svg += "</svg>";
  return svg;
}

ThemeDialog::ThemeDialog(ScribbleDoc* doc)
    : PopupDialog(createPopupDialogNode()), scribbleDoc(doc)
{
  recipe = doc->cfg->themeRecipe();
  themeIndex = paletteThemeIndexOf(recipe);

  Widget* dialogBody = selectFirst(".body-container");
  dialogBody->setMargins(0, 8);

  // Three columns, not "as many as fit": the dialog is sized by its widest row, so a wrapping grid
  //  with no column count takes the whole screen and the tiles shrink to chips.  Rows are forced with
  //  an explicit flex-break on every third cell - setting a width on the container does not wrap it.
  gallery = createRow({}, "0 0", "center");
  gallery->node->setAttribute("flex-wrap", "wrap");
  dialogBody->addWidget(gallery);

  cbDarkPaper = createCheckBox("", recipe.paperL <= real(0.5));
  cbDarkPaper->onToggled = [this](bool){
    // not a lightness slider: the walk mirrors at 0.5, so the only thing that matters is which side
    //  of the mirror you are on.  Every tile is rebuilt, since each theme has its own dark rendering.
    rebuildGallery();
    if(themeIndex >= 0)
      selectTheme(themeIndex);
  };

  cbApplyPages = createCheckBox("", true);
  cbOffPalette = createCheckBox("", doc->cfg->Bool("themeOffPalette"));
  // On by default (deviating from COLORS_SPEC.md §7, at the user's request): picking a theme and being
  //  given it on the paper but not in the ink reads as the theme half-applying.  It stays a checkbox,
  //  and it is one undo step, so the cautious answer is still one click (or one Ctrl+Z) away.
  cbRestyle = createCheckBox("", true);

  dialogBody->addWidget(createTitledRow(_("Dark paper"), cbDarkPaper));
  dialogBody->addWidget(createTitledRow(_("Apply to pages"), cbApplyPages));
  dialogBody->addWidget(createTitledRow(_("Restyle existing strokes"), cbRestyle));
  dialogBody->addWidget(createTitledRow(_("Allow off-palette colors"), cbOffPalette));

  rebuildGallery();

  setTitle(_("Theme"));
  acceptBtn = addButton(_("OK"), [this](){ accept(); finish(ACCEPTED); });
  addButton(_("Cancel"), [this](){ finish(CANCELLED); });
}

void ThemeDialog::selectTheme(int index)
{
  const PaletteTheme* theme = paletteThemeByIndex(index);
  if(!theme)
    return;
  themeIndex = index;
  recipe = paletteThemeRecipe(*theme, cbDarkPaper->isChecked());
  for(size_t ii = 0; ii < galleryBtns.size(); ++ii)
    galleryBtns[ii]->setChecked(int(ii) == index);
}

void ThemeDialog::rebuildGallery()
{
  SvgGui* gui = window() ? window()->gui() : NULL;
  if(gui)
    gui->deleteContents(gallery);
  galleryBtns.clear();

  bool dark = cbDarkPaper && cbDarkPaper->isChecked();
  for(int ii = 0; ii < paletteThemeCount(); ++ii) {
    const PaletteTheme* theme = paletteThemeByIndex(ii);
    Palette pal;
    generatePalette(paletteThemeRecipe(*theme, dark), &pal);

    // No `layout` attribute, deliberately: a box layout centres every child that has no box-anchor,
    //  which would stack the label on top of the preview.  The same trap the width popup's ruling
    //  preview documents.  Children are placed by their own coordinates instead.
    // The selection ring is drawn here rather than left to the toolbutton CSS: those rules tint a
    //  `.icon` child (`.toolbutton.checked > g > .icon`) and a tile has none - it is a picture of a
    //  page - so a checked tile was indistinguishable from an unchecked one and the dialog looked
    //  like it was ignoring clicks.  Stroked inset by half its width so it lands on the preview
    //  rather than half outside it, and hidden until `.checked` (see theme.cpp).
    Button* btn = new Button(loadSVGFragment(fstring(
        "<g class='toolbutton themetile' margin='3 3'>"
          "<rect class='background' width='%g' height='%g'/>"
          "%s"
          "<rect class='theme-sel' x='1.5' y='1.5' width='%g' height='%g' rx='2' ry='2'"
              " fill='none' stroke-width='3' display='none'/>"
          "<text class='title' x='%g' y='%g' text-anchor='middle' font-size='11'>%s</text>"
        "</g>", double(THUMB_W), double(THUMB_H + THUMB_LABEL_H),
        palettePreviewSVG(pal, THUMB_W, THUMB_H, GALLERY_ROWS).c_str(),
        double(THUMB_W - 3), double(THUMB_H - 3),
        double(THUMB_W/2), double(THUMB_H + 12), theme->name).c_str()));
    if(ii > 0 && ii % GALLERY_COLS == 0)
      btn->node->setAttribute("flex-break", "before");
    setupTooltip(btn, theme->name);
    btn->setChecked(ii == themeIndex);
    btn->onClicked = [this, ii](){ selectTheme(ii); };
    gallery->addWidget(btn);
    galleryBtns.push_back(btn);
  }
}

void ThemeDialog::accept()
{
  // the recipe always names its generator explicitly - see ScribbleConfig::setThemeRecipe()
  if(recipe.gen.empty())
    recipe.gen = defaultPaletteGen()->id;
  // set before setTheme(), which rebuilds the pen toolbar - that reads this flag to decide whether
  //  the hex box snaps
  scribbleDoc->cfg->set("themeOffPalette", cbOffPalette->isChecked());
  // "Use for new documents" is gone, and the recipe is written globally as well as to the document:
  //  with a short list of named themes the old checkbox only offered a way to pick a theme and then
  //  be given a different one on the next new document.
  // restyleToTheme() calls setTheme() itself, so this is either/or - calling both would apply the
  //  theme twice and leave the second pass with nothing of the old palette left to map from
  if(cbRestyle->isChecked())
    scribbleDoc->restyleToTheme(recipe, cbApplyPages->isChecked(), true);
  else
    scribbleDoc->setTheme(recipe, cbApplyPages->isChecked(), true);
}
