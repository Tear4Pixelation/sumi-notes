#include "themedialog.h"

#include "scribbleapp.h"
#include "scribbledoc.h"
#include "mainwindow.h"
#include "touchwidgets.h"
#include "page.h"

// Gallery thumbnails: big enough that a stroke reads as a stroke rather than as a chip.  Swatch grids
//  lie about ink - a color that looks fine as a 40px block can be invisible as a 3px line - so every
//  preview here is drawn as strokes on that theme's own paper, at the weight writing actually has.
static const Dim THUMB_W = 92;
static const Dim THUMB_H = 58;
static const Dim PREVIEW_W = 268;
static const Dim PREVIEW_H = 108;
static const int GALLERY_COUNT = 12;

// the gallery's seeds: evenly spread around the wheel so the set covers every hue family, with a small
//  fixed offset per row so it does not read as a color wheel
static real gallerySeed(int i)
{
  return std::fmod(real(i)*real(360)/real(GALLERY_COUNT) + real(7)*real(i % 3), real(360));
}

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
  // one family per row, cycling if there are more families than rows
  for(int ii = 0; ii < nrows; ++ii) {
    const PaletteFamily& fam = pal.families[(ii*int(pal.families.size())/nrows) % pal.families.size()];
    Dim y = rowh*(ii + 1);
    svg += fstring("<path d='%s' fill='none' stroke='#%06X' stroke-width='%g' stroke-linecap='round'/>",
        squigglePath(5, y, boxw*0.62 - 5, ii + 1).c_str(), fam.base.rgb() & 0xFFFFFF,
        double(boxw > 150 ? 2.4 : 1.7));
    svg += fstring("<path d='%s' fill='none' stroke='#%06X' stroke-width='%g' stroke-linecap='round'/>",
        squigglePath(boxw*0.66, y, boxw*0.3, ii + 40).c_str(), fam.dark.rgb() & 0xFFFFFF,
        double(boxw > 150 ? 4.4 : 3.0));
  }
  svg += "</svg>";
  return svg;
}

ThemeDialog::ThemeDialog(ScribbleDoc* doc)
    : PopupDialog(createPopupDialogNode()), scribbleDoc(doc)
{
  recipe = doc->cfg->themeRecipe();

  Widget* dialogBody = selectFirst(".body-container");
  dialogBody->setMargins(0, 8);

  preview = createRow({}, "8 0", "center");
  preview->addWidget(new Widget(loadSVGFragment(
      palettePreviewSVG(doc->palette(), PREVIEW_W, PREVIEW_H, 4).c_str())));
  dialogBody->addWidget(preview);

  gallery = createRow({}, "0 0", "center");
  gallery->node->setAttribute("flex-wrap", "wrap");
  gallery->node->setAttribute("box-anchor", "hfill");
  dialogBody->addWidget(gallery);

  sliderSeed = createSlider();
  sliderSeed->onValueChanged = [this](real v){ recipe.seedHue = v*real(359); updatePreview(); };
  sliderVivid = createSlider();
  sliderVivid->onValueChanged =
      [this](real v){ recipe.vividness = real(0.25) + v*real(0.75); updatePreview(); };
  sliderDepth = createSlider();
  sliderDepth->onValueChanged = [this](real v){ recipe.depth = v*real(0.30); updatePreview(); };

  cbDarkPaper = createCheckBox("", recipe.paperL <= real(0.5));
  cbDarkPaper->onToggled = [this](bool dark){
    // one control, not a lightness slider: the walk mirrors at 0.5, so the only thing between the two
    //  ends of that slider that matters is which side of the mirror you are on
    recipe.paperL = dark ? real(0.22) : real(0.99);
    updatePreview();
  };

  cbApplyPages = createCheckBox("", true);
  cbGlobalDefault = createCheckBox("", false);
  cbOffPalette = createCheckBox("", doc->cfg->Bool("themeOffPalette"));
  // Off by default, always: a theme change that silently rewrote a year of notes is the worst possible
  //  outcome of this feature (COLORS_SPEC.md §7).  It is offered, never assumed.
  cbRestyle = createCheckBox("", false);

  dialogBody->addWidget(createTitledRow(_("Seed hue"), sliderSeed));
  dialogBody->addWidget(createTitledRow(_("Vividness"), sliderVivid));
  dialogBody->addWidget(createTitledRow(_("Depth"), sliderDepth));
  dialogBody->addWidget(createTitledRow(_("Dark paper"), cbDarkPaper));
  dialogBody->addWidget(createTitledRow(_("Apply to pages"), cbApplyPages));
  dialogBody->addWidget(createTitledRow(_("Use for new documents"), cbGlobalDefault));
  dialogBody->addWidget(createTitledRow(_("Restyle existing strokes"), cbRestyle));
  dialogBody->addWidget(createTitledRow(_("Allow off-palette colors"), cbOffPalette));

  // sliders must be in the document before setValue can measure the track
  sliderSeed->setValue(recipe.seedHue/real(359));
  sliderVivid->setValue((recipe.vividness - real(0.25))/real(0.75));
  sliderDepth->setValue(recipe.depth/real(0.30));

  rebuildGallery();

  setTitle(_("Theme"));
  acceptBtn = addButton(_("OK"), [this](){ accept(); finish(ACCEPTED); });
  addButton(_("Cancel"), [this](){ finish(CANCELLED); });
}

PaletteRecipe ThemeDialog::currentRecipe() const
{
  return recipe;
}

void ThemeDialog::selectSeed(real hue)
{
  recipe.seedHue = hue;
  sliderSeed->setValue(hue/real(359));
  updatePreview();
}

void ThemeDialog::rebuildGallery()
{
  SvgGui* gui = window() ? window()->gui() : NULL;
  if(gui)
    gui->deleteContents(gallery->containerNode() ? gallery : gallery);
  galleryBtns.clear();

  for(int ii = 0; ii < GALLERY_COUNT; ++ii) {
    PaletteRecipe r = recipe;
    r.seedHue = gallerySeed(ii);
    Palette pal;
    generatePalette(r, &pal);

    Button* btn = new Button(loadSVGFragment(fstring(
        "<g class='toolbutton' layout='box' margin='2 2'>"
          "<rect class='background' width='%g' height='%g'/>"
          "%s"
        "</g>", double(THUMB_W), double(THUMB_H),
        palettePreviewSVG(pal, THUMB_W, THUMB_H, 3).c_str()).c_str()));
    real hue = r.seedHue;
    btn->onClicked = [this, hue](){ selectSeed(hue); };
    gallery->addWidget(btn);
    galleryBtns.push_back(btn);
  }
}

void ThemeDialog::updatePreview()
{
  Palette pal;
  generatePalette(recipe, &pal);
  SvgGui* gui = window() ? window()->gui() : NULL;
  if(gui)
    gui->deleteContents(preview);
  preview->addWidget(new Widget(loadSVGFragment(
      palettePreviewSVG(pal, PREVIEW_W, PREVIEW_H, 4).c_str())));
}

void ThemeDialog::accept()
{
  // the recipe always names its generator explicitly - see ScribbleConfig::setThemeRecipe()
  if(recipe.gen.empty())
    recipe.gen = defaultPaletteGen()->id;
  // set before setTheme(), which rebuilds the pen toolbar - that reads this flag to decide whether
  //  the hex box snaps
  scribbleDoc->cfg->set("themeOffPalette", cbOffPalette->isChecked());
  // restyleToTheme() calls setTheme() itself, so this is either/or - calling both would apply the
  //  theme twice and leave the second pass with nothing of the old palette left to map from
  if(cbRestyle->isChecked())
    scribbleDoc->restyleToTheme(recipe, cbApplyPages->isChecked(), cbGlobalDefault->isChecked());
  else
    scribbleDoc->setTheme(recipe, cbApplyPages->isChecked(), cbGlobalDefault->isChecked());
}
