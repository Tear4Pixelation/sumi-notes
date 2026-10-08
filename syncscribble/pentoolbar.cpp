#include "pentoolbar.h"
#include "scribbleapp.h"
#include "scribbledoc.h"
#include "themedialog.h"
#include "scribblearea.h"
#include "scribblemode.h"
#include "page.h"
#include "touchwidgets.h"
#include "configdialog.h"
#include "ugui/textedit.h"
#include "usvg/svgparser.h" // for parseNumbersList

// or have spinbox buttons step by 1.25x or 0.8x of current value?
const Dim PenToolbar::PEN_WIDTHS[] = {
    0.1, 0.25, 0.5, 0.75, 1.0, 1.2, 1.4, 1.6, 1.8, 2.0, 2.4, 2.8,
    3.2, 3.6,  4.0, 5.0,  6.0, 8.0, 10,  12,  16,  20,  25,  30,
    35,  40,   48,  60,   72,  86,  100, 125, 150, 175, 200};

// the width display's line spacing, in layout units: how much room one line of the page gets.  Four
//  lines are drawn, and the stroke lies in the gap between the middle two - writing sits between the
//  rulings, not on one, so that gap is what a thickness has to be judged against
static const Dim RULING_PREVIEW_SPACING = 30;
static const int RULING_PREVIEW_LINES = 4;
static const Dim RULING_PREVIEW_WIDTH = 190;
// initial marker thickness presets, in line heights: three of them, matching the pen's row in the
//  design mockup, and all far thicker than a pen preset because a marker has to cover text
static const Dim MARKER_WIDTHS[] = {0.5, 0.75, 1.0};
// initial presets for every other tool, in document units - the same values as the pen's config default
static const Dim DEFAULT_WIDTHS[] = {1.4, 3.0, 6.0};
// one step of the relative width spinbox - a twentieth of a line height is about as fine as this is
//  worth setting, and it keeps the value readable at the "%.3g" the spinbox formats with
static const Dim RELATIVE_WIDTH_STEP = 0.05;

// The add-color grid: 4 columns of smaller-than-toolbar swatches, so the theme's 16 entries
//  (neutral + 15 families) land as a compact 4x4 block rather than a long strip.
static const Dim PALETTE_CELL = 30;
static const Dim PALETTE_DOT = 11;   // swatch radius within the cell
static const int PALETTE_GRID_COLS = 4;

// The active document's palette, or NULL when there is no document yet (the toolbar is built before
//  the first one is opened).  Defined up here because the constructor's lambdas need it.
static const Palette *docPalette() {
  ScribbleDoc *doc = ScribbleApp::app ? ScribbleApp::app->activeDoc() : NULL;
  return doc ? &doc->palette() : NULL;
}

static bool offPaletteAllowed() {
  ScribbleDoc *doc = ScribbleApp::app ? ScribbleApp::app->activeDoc() : NULL;
  return doc && doc->cfg->Bool("themeOffPalette");
}

std::unique_ptr<SvgNode> PenToolbar::widthBtnNode;
std::unique_ptr<SvgNode> PenToolbar::compactWidthBtnNode;
std::unique_ptr<SvgNode> PenToolbar::compactColorBtnNode;

// PaletteWidget::PaletteWidget(SvgNode* n, Widget* main, Widget* overflow,
// Widget* btn) : Widget(n),
//     mainGroup(main), overflowGroup(overflow), overflowBtn(btn) {}

// setupMenuItem requires a button
void PaletteWidget::addButton(Button *w) {
  w->setMargins(0, 1);
  setupMenuItem(
      w); // in case widget ends up on overflow menu; does no harm if not
  items.push_back(w);
  overflowBtn->setVisible(int(items.size()) > numMainGroup);

  int ii = items.size() - 1;
  (ii < numMainGroup ? mainGroup : overflowGroup)->addWidget(w);
  if (ii > numMainGroup && (ii - numMainGroup) % 4 == 0)
    w->node->setAttribute("flex-break", "before");
}

// this will typically be used with AutoAdjContainer
void PaletteWidget::setNumVisible(int n) {
  SvgGui *gui = window() ? window()->gui() : NULL;
  if (!gui || (numMainGroup >= int(items.size()) && n >= int(items.size()))) {
    numMainGroup = n;
    return;
  }

  numMainGroup = n;
  overflowBtn->setVisible(int(items.size()) > numMainGroup);
  for (Widget *w : items) {
    gui->onHideWidget(w);
    w->removeFromParent();
  }
  for (int ii = 0; ii < int(items.size()); ++ii) {
    items[ii]->node->removeAttr("flex-break");
    (ii < numMainGroup ? mainGroup : overflowGroup)->addWidget(items[ii]);
    if (ii > numMainGroup && (ii - numMainGroup) % 4 == 0)
      items[ii]->node->setAttribute("flex-break", "before");
  }
}

void PaletteWidget::clear() {
  SvgGui *gui = window() ? window()->gui() : NULL;
  if (gui) {
    gui->deleteContents(mainGroup);
    gui->deleteContents(overflowGroup);
    items.clear();
  }
}

PaletteWidget *createPaletteWidget() {
  static const char *moreMenuSVG = R"#(
    <g id="menu" class="menu" display="none" position="absolute" box-anchor="fill" layout="box">
      <rect box-anchor="fill" width="20" height="20"/>
      <g class="child-container" box-anchor="fill"
          layout="flex" flex-direction="row" flex-wrap="wrap" justify-content="flex-start" margin="6 6">
      </g>
    </g>
  )#";

  static std::unique_ptr<SvgNode> moreMenuNode;
  if (!moreMenuNode)
    moreMenuNode.reset(loadSVGFragment(moreMenuSVG));

  PaletteWidget *container = new PaletteWidget(new SvgG());
  container->node->setAttribute("layout", "flex");
  container->node->setAttribute("flex-direction", "row");

  Widget *mainGroup = createRow();
  mainGroup->node->setAttribute("box-anchor", ""); // no stretch
  Button *overflowBtn =
      createToolbutton(SvgGui::useFile("icons/chevron_down.svg"), _("More"));
  Menu *overflowMenu = new Menu(moreMenuNode->clone(), Menu::VERT_LEFT);
  overflowBtn->setMenu(overflowMenu);
  overflowBtn->setVisible(false); // hidden initially

  container->addWidget(mainGroup);
  container->addWidget(overflowBtn);

  // this hacky mess exposes some fundamental problems with our gui design
  container->mainGroup = mainGroup;
  container->overflowGroup = overflowMenu->selectFirst(".child-container");
  container->overflowBtn = overflowBtn;
  container->numMainGroup = 16;

  return container;
}

AutoAdjContainer *createPenToolbarAutoAdj(bool compact) {
  PenToolbar *tb = new PenToolbar(compact);

  AutoAdjContainer *adjtb = new AutoAdjContainer(new SvgG(), tb);
  adjtb->node->setAttribute("box-anchor", "hfill");
  adjtb->node->addClass("pen-toolbar-autoadj");

  adjtb->adjFn = [tb, adjtb](const Rect &src, const Rect &dest) {
    if (dest.width() <= src.width() + 0.5 &&
        tb->stretch->node->bounds().width() >= 1)
      return;
    // reset
    if (!tb->closeBtn->isVisible()) {
      for (Widget *sep : tb->select(".toolbar-separator"))
        sep->setVisible(true);
      tb->closeBtn->setVisible(true);
      tb->colorPalette->setVisible(true);
      tb->widthPalette->setVisible(true);
    }
    tb->colorPicker->setVisible(true);
    tb->colorPalette->setNumVisible(16);
    tb->widthPalette->setNumVisible(16);
    adjtb->repeatLayout(dest);
    // the saved color swatches are the primary control of this row, so they are
    // the last thing to be
    //  shrunk: first drop the hex/picker box, then the thickness presets, then
    //  the swatches
    if (tb->stretch->node->bounds().width() < 1) {
      tb->colorPicker->setVisible(false);
      adjtb->repeatLayout(dest);
    }
    while (tb->stretch->node->bounds().width() < 1 &&
           tb->widthPalette->numMainGroup > 0) {
      tb->widthPalette->setNumVisible(tb->widthPalette->numMainGroup - 1);
      adjtb->repeatLayout(dest);
    }
    while (tb->stretch->node->bounds().width() < 1 &&
           tb->colorPalette->numMainGroup > 0) {
      tb->colorPalette->setNumVisible(tb->colorPalette->numMainGroup - 1);
      adjtb->repeatLayout(dest);
    }
    if (tb->stretch->node->bounds().width() < 1) {
      for (Widget *sep : tb->select(".toolbar-separator"))
        sep->setVisible(false);
      tb->closeBtn->setVisible(false);
      tb->colorPalette->setVisible(false);
      tb->widthPalette->setVisible(false);
      adjtb->repeatLayout(dest);
    }
  };

  // feels hacky; should PenToolbar inherit from auto adj container instead?  We
  // need more examples!
  tb->closeBtn->onClicked = [adjtb]() { adjtb->setVisible(false); };

  return adjtb;
}

PenToolbar::PenToolbar(bool _compact)
    : Toolbar(widgetNode("#toolbar")), pen(Color::BLACK, 0), compact(_compact) {
  // For width, I don't think we should change fill color to match currently
  // selected color - would be
  //  distracting and we'd have to change background to match page color
  static const char *widthBtnSVG = R"#(
    <g class="previewbtn" transform="translate(17,17)">
      <rect fill="white" stroke="currentColor" stroke-width="2" x="-17" y="-17" width="34" height="34"/>
      <line class="width-line" x1="-4" y1="0" x2="4" y2="0" stroke="black" stroke-width="1" stroke-linecap="round"/>
    </g>
  )#";

  // compact swatches follow the mockup: a SWATCH_W x SWATCH_H hit cell (so they
  // sit on the same
  //  baseline as the row's icon buttons) holding a bare 13px line or a 25px
  //  circle, drawn in the icon color so that .toolbutton.checked turns the
  //  selected one blue like every other tool
  // the cell geometry follows floatUIScale like the rest of the floating panels
  std::string compactWidthBtnSVG = fstring(R"#(
    <g class="toolbutton swatch-btn" layout="box">
      <rect class="background" width="%g" height="%g"/>
      <line class="icon width-line" x1="%g" y1="%g" x2="%g" y2="%g"
          fill="none" stroke="currentColor" stroke-width="1" stroke-linecap="round"/>
    </g>
  )#", 44*floatUIScale, 64*floatUIScale,
      15.5*floatUIScale, 32*floatUIScale, 28.5*floatUIScale, 32*floatUIScale);
  std::string compactColorBtnSVG = fstring(R"#(
    <g class="toolbutton swatch-btn" layout="box">
      <rect class="background" width="%g" height="%g"/>
      <circle class="sel-ring icon" cx="%g" cy="%g" r="%g" display="none"
          fill="none" stroke="currentColor" stroke-width="2"/>
      <circle class="btn-color" cx="%g" cy="%g" r="%g" fill="blue"/>
    </g>
  )#", 44*floatUIScale, 64*floatUIScale,
      22*floatUIScale, 32*floatUIScale, 15.5*floatUIScale,
      22*floatUIScale, 32*floatUIScale, 12.5*floatUIScale);

  // compact and roomy toolbars can coexist (e.g. desktop panel + a phone-style
  // row), so the two
  //  prototypes are cached separately rather than in one shared node
  auto &widthProto = compact ? compactWidthBtnNode : widthBtnNode;
  if (!widthProto)
    widthProto.reset(loadSVGFragment(
        compact ? compactWidthBtnSVG.c_str() : widthBtnSVG));
  // built even for a roomy toolbar: the single swatch and width (createSingleSwatch/Width) always use
  //  the compact cells
  if (!compactColorBtnNode)
    compactColorBtnNode.reset(loadSVGFragment(compactColorBtnSVG.c_str()));
  if (!compactWidthBtnNode)
    compactWidthBtnNode.reset(loadSVGFragment(compactWidthBtnSVG.c_str()));

  const char *colorStr =
      ScribbleApp::cfg->String("savedColors", "black,red,green,blue");
  auto colorStrSplit = splitStringRef(colorStr, ',', true);
  for (auto &s : colorStrSplit) {
    Color color = parseColor(s);
    if (color.isValid())
      savedColors.push_back(color);
  }

  // I think we may want to increase size of circles or use a short stroke
  // instead of circle
  preScale = ScribbleApp::getPreScale();
  // each list is stored with its unit ("rel:" / "abs:"), in the same config value as the numbers, so
  //  the two cannot be saved or loaded apart.  A list from an older config has no unit and gets that
  //  tool's pen's on first use; an empty one is seeded then (see prepareWidths)
  savedWidths.parse(ScribbleApp::cfg->String("savedWidths", ""));
  savedMarkerWidths.parse(ScribbleApp::cfg->String("savedMarkerWidths", ""));
  savedEphemeralWidths.parse(ScribbleApp::cfg->String("savedEphemeralWidths", ""));

  colorPalette = createPaletteWidget();
  widthPalette = createPaletteWidget();

  // context menus for saved colors, widths
  // ... instead of disabling delete when only one item, maybe we should add
  // context menu for preview button?
  colorCtxMenu = createMenu(Menu::FLOATING, false);
  colorCtxMenu->addItem(_("Insert Current"), [this]() {
    savedColors.erase(
        std::remove(savedColors.begin(), savedColors.end(), pen.color),
        savedColors.end());
    int pos = std::min(contextMenuIdx + 1, int(savedColors.size()));
    savedColors.insert(savedColors.begin() + pos, pen.color);
    rebuildGrids();
  });
  colorMenuDelete = colorCtxMenu->addItem(_("Delete"), [this]() {
    savedColors.erase(savedColors.begin() + contextMenuIdx);
    rebuildGrids();
  });

  // The thickness presets have no "insert current" or "delete": there are always exactly
  //  WidthPresets::COUNT of them, each edited in place (tap the selected one).  Deleting was the only
  //  way out of a preset that could not be edited, and it lost the slot for good.

  colorPicker = createColorEditBox();
  colorPicker->onColorChanged = [this](Color) { updateColor(); };
  // clamp length? ... user may want to paste in longer string and then cut down
  // colorPicker->hexEdit->maxLength = strlen("lightgoldenrodyellow") + 1;
  // in the compact row the picker is hidden entirely, so drop its hex text
  // field - it reserves a
  //  150px min-width even when invisible, which would pad the row out
  if (compact)
    colorPicker->selectFirst(".colorbox_text")->removeFromParent();

  spinWidth = createTextSpinBox(1.25, 0.01, 0, 200, "%.3g",
                                120); // 3 significant digits
  spinWidth->onValueChanged = [this](Dim w) { updateWidth(); };
  // override spinbox onStep to step though PEN_WIDTHS
  spinWidth->onStep = [this](int nsteps) {
    Dim w = 0, oldw = spinWidth->value();
    // PEN_WIDTHS is a list of absolute pen sizes; in fractions of a line height it means nothing, so
    //  relative widths step by a fixed fraction instead
    if (relativeWidths())
      return std::max(RELATIVE_WIDTH_STEP,
          (std::round(oldw/RELATIVE_WIDTH_STEP) + nsteps)*RELATIVE_WIDTH_STEP);
    if (nsteps > 0) {
      const Dim *next =
          std::upper_bound(std::begin(PEN_WIDTHS), std::end(PEN_WIDTHS), oldw);
      w = next != std::end(PEN_WIDTHS) ? *next
                                       : PEN_WIDTHS[NELEM(PEN_WIDTHS) - 1];
    } else {
      const Dim *next =
          std::lower_bound(std::begin(PEN_WIDTHS), std::end(PEN_WIDTHS), oldw);
      w = next != std::begin(PEN_WIDTHS) ? *(next - 1) : PEN_WIDTHS[0];
    }
    return w;
  };

  // detail editor for the selected thickness preset; more content (tip shape,
  // adding presets) is future work
  widthPopup = createArrowPopup(Menu::VERT_LEFT);
  // the width display: the page's ruling with the outer lines only half there, and a stroke of the
  //  pen's width and color lying in the gap between the middle two.  What matters is how much of a
  //  line the pen covers, which a number in document units cannot show; and it is the *gap* that
  //  matters, since writing sits between the rulings rather than on one - a stroke centred on a rule
  //  line would be showing coverage of the wrong band.  A width of one line height therefore fills
  //  the gap exactly.  Shown for every pen and in every unit, so the number always has a picture
  //  beside it; it is only the unit of the number that the relative size toggle changes.
  //  The rule lines are drawn after the stroke because the marker draws under the writing.
  //  No layout attribute: a box layout centres children that carry no box-anchor, which would stack
  //  all the lines and the stroke on top of each other - the coordinates here are the display.
  //  It is drawn in the page's own colors, set per page in updateWidthPopup(): the display is a
  //  picture of the page, and the usual pen is black, which on the dark popup would be invisible.
  //  Opacity rather than a dimmer gray for the outer two, so "half there" reads that way whatever
  //  the page and rule colors are.
  const Dim previewSpan = RULING_PREVIEW_LINES*RULING_PREVIEW_SPACING;
  std::string rulingPreviewSVG = fstring(R"#(
    <g class="ruling-preview" margin="0 8 8 8">
      <rect class="page-bg" fill="white" width="%g" height="%g"/>
      <line class="pen-line" x1="%g" y1="%g" x2="%g" y2="%g"
          stroke="black" stroke-width="%g" stroke-linecap="butt"/>
      <line class="rule-line" x1="0" y1="%g" x2="%g" y2="%g"
          stroke="#808080" stroke-width="1.5" stroke-opacity="0.35"/>
      <line class="rule-line" x1="0" y1="%g" x2="%g" y2="%g"
          stroke="#808080" stroke-width="1.5"/>
      <line class="rule-line" x1="0" y1="%g" x2="%g" y2="%g"
          stroke="#808080" stroke-width="1.5"/>
      <line class="rule-line" x1="0" y1="%g" x2="%g" y2="%g"
          stroke="#808080" stroke-width="1.5" stroke-opacity="0.35"/>
    </g>
  )#", RULING_PREVIEW_WIDTH, previewSpan,
      10.0, previewSpan/2, RULING_PREVIEW_WIDTH - 10, previewSpan/2, RULING_PREVIEW_SPACING,
      0.5*RULING_PREVIEW_SPACING, RULING_PREVIEW_WIDTH, 0.5*RULING_PREVIEW_SPACING,
      1.5*RULING_PREVIEW_SPACING, RULING_PREVIEW_WIDTH, 1.5*RULING_PREVIEW_SPACING,
      2.5*RULING_PREVIEW_SPACING, RULING_PREVIEW_WIDTH, 2.5*RULING_PREVIEW_SPACING,
      3.5*RULING_PREVIEW_SPACING, RULING_PREVIEW_WIDTH, 3.5*RULING_PREVIEW_SPACING);
  rulingPreview = new Widget(loadSVGFragment(rulingPreviewSVG.c_str()));
  // every pen gets this toggle, but only the marker has it on by default: it is the one tool sized to
  //  cover a line of text rather than to make a mark of a particular size, so it is the one for which
  //  a line height is the obvious unit - but a pen ruled to a fraction of a line is just as coherent
  cbRelWidth = createCheckBox("", true);
  cbRelWidth->onToggled = [this](bool on) { setRelativeWidth(on); };
  relWidthRow = createTitledRow(_("Relative size"), createStretch(), cbRelWidth);
  widthSpinRow = createTitledRow(_("Width"), spinWidth);
  widthPopup->addWidget(relWidthRow);
  widthPopup->addWidget(rulingPreview);
  widthPopup->addWidget(widthSpinRow);
  setupAutoClosePopup(widthPopup);

  // detail editor for a saved color swatch: tapping the already-selected
  // swatch, or the "+" button,
  //  opens this to edit that swatch's color in place; unlike the main
  //  colorPicker, the RGB/HSV sliders are placed directly in the popup instead
  //  of behind another menu on the preview button
  colorPopup = createArrowPopup(Menu::VERT_LEFT);
  ColorSliders *colorPopupSliders = new ColorSliders(new SvgG());
  colorPopupPicker = createColorEditBox(true, colorPopupSliders);
  colorPopupPicker->setMargins(0, 0, 0, 4);
  colorPopupPicker->onColorChanged = [this](Color c) {
    if (colorPopupIdx < 0 || colorPopupIdx >= int(savedColors.size()))
      return;
    // This edits a swatch directly, bypassing updateColor(), so it needs its own snap - otherwise the
    //  custom-color route is a hole straight through the palette.
    if (themed && !offPaletteAllowed()) {
      const Palette *pal = docPalette();
      if (pal) {
        int alpha = c.alpha();
        c = pal->nearest(c);
        c.setAlpha(alpha);
      }
    }
    savedColors[colorPopupIdx] = c;
    colorPalette->items[colorPopupIdx]
        ->selectFirst(".btn-color")
        ->node->setAttr<color_t>("fill", ScribbleApp::displayColor(c).color);
    if (colorPalette->items[colorPopupIdx]->isChecked()) {
      colorPicker->setColor(c);
      updateColor();
    }
  };
  Widget *colorPopupTabs =
      createTabBar({"RGB", "HSV"}, [colorPopupSliders](int tabnum) {
        colorPopupSliders->setVisibleGroup(tabnum != 0);
      });
  colorPopupTabs->setMargins(12, 0, 0, 0);
  Widget *colorPopupTitle = createTitledRow(_("Color"), colorPopupPicker);
  colorPopupTitle->selectFirst(".row-text")
      ->node->addClass("color-popup-title");
  colorPopup->addWidget(colorPopupTitle);
  colorPopup->addWidget(colorPopupTabs);
  colorPopup->addWidget(colorPopupSliders);
  setupAutoClosePopup(colorPopup);

  trackEditFocus(colorPicker);
  trackEditFocus(spinWidth);

  Menu *overflowMenu = createMenu(Menu::VERT_LEFT);
  cbSnaptoGrid = createCheckBoxMenuItem(_("Snap to Grid"));
  cbSnaptoGrid->onClicked = [this]() {
    cbSnaptoGrid->setChecked(!cbSnaptoGrid->checked());
    updatePen();
  };
  cbLineDrawing = createCheckBoxMenuItem(_("Draw Lines"));
  cbLineDrawing->onClicked = [this]() {
    cbLineDrawing->setChecked(!cbLineDrawing->checked());
    updatePen();
  };
  // in the compact (floating) toolbar the overflow button is hidden, so these two go into the pen
  //  settings popup instead - a widget can only have one parent, so it is one or the other
  if (!compact) {
    overflowMenu->addItem(cbSnaptoGrid);
    overflowMenu->addItem(cbLineDrawing);
  }

  overflowBtn = createToolbutton(SvgGui::useFile("icons/ic_menu_overflow.svg"),
                                 _("More Options"));
  overflowBtn->setMenu(overflowMenu);

  Menu *selOverflowMenu = createMenu(Menu::VERT_LEFT);
  Button *useAsPenItem = createMenuItem(_("Use as Pen"));
  useAsPenItem->onClicked = [this]() {
    pen.color = colorPicker->color();
    pen.width = spinWidth->value();
    if (pen.color.alpha() > 0 && pen.width > 0 && onChanged)
      onChanged(PEN_CHANGED);
  };
  selOverflowMenu->addItem(useAsPenItem);
  selOverflowBtn = createToolbutton(
      SvgGui::useFile("icons/ic_menu_overflow.svg"), _("Selection Options"));
  selOverflowBtn->setMenu(selOverflowMenu);
  selOverflowBtn->setVisible(false);

  closeBtn = createToolbutton(SvgGui::useFile("icons/ic_menu_cancel.svg"), "");
  // closeBtn->onClicked = [this](){ setVisible(false); };  -- must be set by
  // auto adj container

  // The theme's colors, as a 4x4 grid. Rows are forced with an explicit `flex-break` on every fourth
  //  cell - setting a width on the container does *not* wrap it, which is the same mechanism
  //  PaletteWidget::addButton() already uses for the overflow menu.
  palettePopup = createArrowPopup(Menu::VERT_LEFT);
  // the swatch cells carry their own spacing, so the popup's usual side padding would double up on it
  palettePopup->selectFirst(".child-container")->setMargins(8);
  // Cancel and delete for the edit route.  They live on a header row rather than as two more cells in
  //  the grid because they are not colors: mixing a destructive action in among the swatches would put
  //  it one slip away from every ordinary pick.
  Button *paletteCancelBtn = createToolbutton(
      SvgGui::useFile("icons/ic_menu_cancel.svg"), _("Cancel"));
  paletteCancelBtn->onClicked = [this]() { closeAutoClosePopup(palettePopup); };
  paletteDeleteBtn = createToolbutton(
      SvgGui::useFile("icons/ic_menu_discard.svg"), _("Delete Swatch"));
  paletteDeleteBtn->onClicked = [this]() {
    if (paletteEditIdx < 0 || paletteEditIdx >= int(savedColors.size()))
      return;
    closeAutoClosePopup(palettePopup);
    int deleted = paletteEditIdx;
    savedColors.erase(savedColors.begin() + deleted);
    paletteEditIdx = -1;
    rebuildGrids();
    // The deleted swatch is the selected one - this route is only reachable by tapping it - so the pen
    //  would otherwise be left on a color the row no longer offers, with no swatch ringed.
    int next = std::min(deleted, int(savedColors.size()) - 1);
    if (next >= 0) {
      colorPicker->setColor(savedColors[next]);
      updateColor();
    }
  };
  paletteHeader = createRow();
  paletteHeader->addWidget(paletteCancelBtn);
  paletteHeader->addWidget(createStretch());
  paletteHeader->addWidget(paletteDeleteBtn);
  palettePopup->addWidget(paletteHeader);
  paletteGrid = createRow({}, "0 0", "flex-start");
  paletteGrid->node->setAttribute("flex-wrap", "wrap");
  paletteGrid->node->setAttribute("margin", "4 4");
  palettePopup->addWidget(paletteGrid);
  setupAutoClosePopup(palettePopup);

  addColorBtn = createToolbutton(
      SvgGui::useFile(":/icons/ic_menu_add_color.svg"), _("Add Color"));
  addColorBtn->onClicked = [this]() { openPaletteGrid(-1); };

  // The pen tip is a flag on the pen, not a pref, so it is passed as an extra row rather than by name.
  // It picks the stroke builder: flat extrudes a quad per input segment and so has no defined direction
  //  when the pen is not moving, round sweeps a disc and does not care, chisel is a fixed-aspect nib.
  comboPenTip = createComboBox({_("Flat"), _("Round"), _("Chisel")});
  comboPenTip->onChanged = [this](const char *) {
    pen.setFlag(ScribblePen::TIP_MASK, false);
    pen.setFlag(comboPenTip->index() == 0 ? ScribblePen::TIP_FLAT
        : (comboPenTip->index() == 2 ? ScribblePen::TIP_CHISEL : ScribblePen::TIP_ROUND), true);
    updatePen();
  };
  penTipRow = createTitledRow(_("Pen Tip"), comboPenTip);

  // Like the tip, a value on the pen rather than a pref.  0% is a pen whose width never follows pressure
  //  (the marker's default); the default pens are 90%.  Shown in percent because the stored fraction,
  //  wRatio, means nothing to anyone who has not read strokebuilder.cpp.
  spinPressure = createTextSpinBox(0, 10, 0, 100, "%.0f%%", 120);
  spinPressure->onValueChanged = [this](Dim percent) {
    // setPen() pushing the pen's own (rounded) value into the box, which must not rewrite the pen
    if (percent == std::round(100*pen.pressureSensitivity()))
      return;
    pen.setPressureSensitivity(percent/100);
    updatePen();
  };
  trackEditFocus(spinPressure);
  pressureRow = createTitledRow(_("Pressure"), spinPressure);
  setupTooltip(pressureRow, _("How much thinner a light touch draws than a full press"));

  settingsBtn = createToolSettingsButton(
      "Pen Settings", {"inputSmoothing", "inputSimplify", "shapeDrawThrough", "shapeSnapDelay", "liftScratchOut", "liftScratchOutLevel",
          "applyPenToSel", "savePenMode"},
      compact ? std::vector<Button *>{cbSnaptoGrid, cbLineDrawing} : std::vector<Button *>{},
      {penTipRow, pressureRow});

  Button *helpBtn = createHelpButton(
      {{"ic_menu_add_color.svg", "Add Color",
        "Adds the current color to the saved colors."},
       {"ic_menu_set_pen.svg", "Thickness",
        "Sets stroke width; tap the selected preset again to edit its value."},
       {"ic_menu_settings2.svg", "Pen Settings",
        "Smoothing, simplification and other pen preferences."},
       {"ic_menu_toggle_ruled.svg", "Center on Line",
        "Marker only: draws a straight line along the middle of the ruled line you start in."},
       {"ic_menu_overflow.svg", "More Options",
        "Snap strokes to the grid or draw straight lines."},
       {"ic_menu_cancel.svg", "Close", "Hides this options row."}});

  // a marker covers a line of text, so it can be made to run straight along the middle of one wherever
  //  the stroke is started within it; a toggle on the row itself, like the ruled eraser's
  centerLineToggle = createToolbutton(SvgGui::useFile(":/icons/ic_menu_toggle_ruled.svg"),
                                      _("Center on Line"));
  centerLineToggle->onClicked = [this]() {
    centerLineToggle->setChecked(!centerLineToggle->isChecked());
    pen.setFlag(ScribblePen::CENTER_ON_LINE, centerLineToggle->isChecked());
    if (onChanged)
      onChanged(PEN_CHANGED);
  };
  setupTooltip(centerLineToggle, _("Draw along the middle of the ruled line"));
  centerLineToggle->setVisible(false);

  static const char *spacerSVG =
      R"(<rect fill="none" width="12" height="20"/>)";
  widthGroup = createRow();
  widthGroup->node->setAttribute("box-anchor",
                                 ""); // no stretching for this subtoolbar!
  widthGroup->addWidget(widthPalette);
  widthGroup->addWidget(widthPopup);

  colorGroup = createRow();
  colorGroup->node->setAttribute("box-anchor",
                                 ""); // no stretching for this subtoolbar!
  // saved colors + "add color" are the primary controls of this row and always
  // come first; the
  //  hex/picker box is a secondary, advanced control and so goes last (it is
  //  the first thing to be pushed out of view when the row is too narrow for
  //  its contents)
  colorGroup->addWidget(colorPalette);
  colorGroup->addWidget(colorPopup);
  colorGroup->addWidget(palettePopup);
  colorGroup->addWidget(addColorBtn);
  if (!compact) {
    colorGroup->addWidget(new Widget(loadSVGFragment(spacerSVG)));
    colorGroup->addWidget(colorPicker);
  }

  stretch = createStretch();

  addWidget(stretch);
  addWidget(colorGroup);
  addSeparator();
  addWidget(widthGroup);
  addWidget(centerLineToggle);
  if (!compact)
    addSeparator();
  if (!compact)
    addWidget(settingsBtn);
  addWidget(overflowBtn);
  addWidget(selOverflowBtn);
  addWidget(helpBtn);
  if (!compact)
    addSeparator();
  addWidget(closeBtn);
  if (compact) {
    // the mockup's draw options row is just swatches | thicknesses (plus the
    // settings button we add),
    //  so the rest stays in the tree - to keep ownership and the auto-adjust
    //  wiring simple - but hidden
    for (Widget *w : {(Widget *)colorPicker, (Widget *)overflowBtn,
                      (Widget *)helpBtn, (Widget *)closeBtn})
      w->setVisible(false);
    addWidget(colorPicker);
    addWidget(createStretch()); // with the leading stretch, this centres the
                                // row under the tools
    addWidget(settingsBtn);     // past the stretch, so it sits at the very end of the row
    // "add color" sits in the swatch grid, so it gets the swatch cell rather
    // than a full icon cell
    addColorBtn->node->addClass("swatch-btn");
    Widget *addBg = addColorBtn->selectFirst(".background");
    if (addBg && addBg->node->type() == SvgNode::RECT)
      static_cast<SvgRect *>(addBg->node)
          ->setRect(Rect::wh(44*floatUIScale, 64*floatUIScale));
    Widget *addIcon = addColorBtn->selectFirst(".icon");
    if (addIcon && addIcon->node->type() == SvgNode::USE)
      static_cast<SvgUse *>(addIcon->node)
          ->setViewport(Rect::wh(25*floatUIScale, 25*floatUIScale));
    addColorBtn->setMargins(0);
    settingsBtn->node->addClass("float-small-btn");
  }
  addWidget(colorCtxMenu);

  addHandler([this](SvgGui *gui, SDL_Event *event) {
    // Return focus to ScribbleArea if Enter or Esc pressed (these keys are
    // propagated above TextEdit)
    if (event->type == SDL_KEYDOWN) {
      SDL_Keycode key = event->key.keysym.sym;
      // Uint16 mods = event->key.keysym.mod;
      if (key == SDLK_ESCAPE || key == SDLK_RETURN) {
        if (onChanged)
          onChanged(YIELD_FOCUS);
        return true;
      }
    }
    return false;
  });

  // populate color and width grids (rows)
  rebuildGrids();
  // hides the relative size toggle and its display until a marker pen is loaded
  updateWidthPopup();
}

// Is this color one the current theme can produce? Any variant of any family counts, plus the neutral.
static bool isPaletteMember(const Palette *pal, Color c) {
  if (!pal)
    return false;
  if (c == pal->neutral)
    return true;
  for (const PaletteFamily &fam : pal->families) {
    for (int vv = 0; vv < PALETTE_NUM_VARIANTS; ++vv) {
      if (fam.variant(vv) == c)
        return true;
    }
  }
  return false;
}

// The toolbar's *default* working set: the neutral plus a handful of families spread around the wheel.
// Deliberately small. The whole palette belongs in the add-color grid, not on the options row - a row
//  of thirteen swatches is a row nobody reads, and picking from it is slower than picking from five.
static std::vector<Color> defaultThemeSwatches(const Palette *pal) {
  std::vector<Color> out;
  out.push_back(pal->neutral);
  const int want = 5;
  int n = int(pal->families.size());
  for (int kk = 0; kk < want && n > 0; ++kk)
    out.push_back(pal->families[(kk * n) / want].base);
  return out;
}

// every swatch and preview shows its color through ScribbleApp::displayColor(), so a dark mode toggle
//  has to repaint them all
void PenToolbar::refreshDisplayColors() {
  rebuildGrids();
  updateSingles();
  updateWidthPopup();
}

void PenToolbar::refreshPalette() {
  const Palette *pal = docPalette();
  themed = pal && !pal->families.empty();
  if (themed) {
    // Unlike the widths, these are the user's picks, not the theme's - the theme only supplies the
    //  menu to pick from. So the list is left alone unless it plainly belongs to some other theme,
    //  which is what happens after a theme change (and on the first run, where the config still holds
    //  the old black/red/green/blue defaults). Reseeding then is what keeps a themed document from
    //  showing swatches its own palette cannot produce.
    // Every swatch must belong to the theme, not merely one of them: the legacy default is
    //  black/red/green/blue, and black *is* the neutral, so an "any member" test counts that whole
    //  list as themed and leaves three colors the palette cannot produce sitting on the row.
    // Skipped when off-palette colors are allowed, since then a deliberate custom swatch is legal and
    //  reseeding would delete it on every launch.
    bool allMembers = true;
    for (Color c : savedColors)
      allMembers = allMembers && isPaletteMember(pal, c);
    if (savedColors.empty() || (!allMembers && !offPaletteAllowed()))
      savedColors = defaultThemeSwatches(pal);
  }
  rebuildGrids();
}

int PenToolbar::addSwatchColor(Color color) {
  auto it = std::find(savedColors.begin(), savedColors.end(), color);
  if (it == savedColors.end()) {
    savedColors.push_back(color);
    it = savedColors.end() - 1;
  }
  int idx = int(it - savedColors.begin());
  rebuildGrids();
  colorPicker->setColor(toolColor(savedColors[idx]));
  updateColor();
  return idx;
}

// Replaces one swatch in place, keeping its position on the row - the edit route must never grow the
//  row, which is the thing it exists to be able to undo.
void PenToolbar::setSwatchColor(int idx, Color color) {
  if (idx < 0 || idx >= int(savedColors.size()))
    return;
  savedColors[idx] = color;
  rebuildGrids();
  colorPicker->setColor(toolColor(color));
  updateColor();
}

// The custom-color route: the hex/slider popup, on an appended swatch (editIdx < 0) or on an existing
//  one. Reached from the end of the theme grid rather than directly, so the theme's own colors are
//  always the first answer offered and a custom color is the deliberate second step.
void PenToolbar::openCustomColor(int editIdx) {
  closeAutoClosePopup(palettePopup);
  int idx = editIdx >= 0 && editIdx < int(savedColors.size())
      ? editIdx : addSwatchColor(colorPicker->color());
  // rebuildGrids() (via addSwatchColor) resets colorPopupIdx, so this must come after it
  colorPopupIdx = idx;
  colorPopupPicker->setColor(savedColors[idx]);
  openAutoClosePopup(colorPopup);
}

void PenToolbar::openPaletteGrid(int editIdx) {
  bool editing = editIdx >= 0 && editIdx < int(savedColors.size());
  const Palette *pal = docPalette();
  if (!pal || pal->families.empty()) {
    // no theme: there is no grid to show, so both routes fall through to the color editor
    openCustomColor(editing ? editIdx : -1);
    return;
  }
  paletteEditIdx = editing ? editIdx : -1;
  paletteHeader->setVisible(editing);
  // the row must keep at least one swatch, as the pen's color has to come from somewhere
  paletteDeleteBtn->setEnabled(editing && savedColors.size() > 1);

  fillColorGrid(paletteGrid, [this](Color c) {
    int idx = paletteEditIdx;
    closeAutoClosePopup(palettePopup);
    if (idx >= 0)
      setSwatchColor(idx, c);
    else
      addSwatchColor(c);
  }, [this]() { openCustomColor(paletteEditIdx); });
  openAutoClosePopup(palettePopup);
}

// The theme's colors as a grid, plus a trailing "custom color" cell; shared by the "+" and the single
//  swatch, which differ only in what a pick does.
void PenToolbar::fillColorGrid(Widget *grid, const std::function<void(Color)> &onPick,
    const std::function<void()> &onCustom) {
  const Palette *pal = docPalette();
  if (!pal)
    return;
  SvgGui *gui = window() ? window()->gui() : NULL;
  if (gui)
    gui->deleteContents(grid);
  // Bases only. Offering each family's dark variant too was tried and measured: it is better on light
  //  paper (17 cells at min pairwise dE 0.067 against 13 cells at 0.041) and much worse on dark, where
  //  the generator's `dL = min(0.92, L + 0.13)` clamps a dark onto its own base and the grid falls to
  //  0.014 - visually duplicate swatches. The variants stay reachable through restyle, which preserves
  //  them; they are just not worth a cell here.
  std::vector<Color> offer;
  offer.push_back(pal->neutral);
  for (const PaletteFamily &fam : pal->families)
    offer.push_back(fam.base);
  // A cell smaller than the toolbar's own swatches: this is a grid to scan, not a row to hit
  //  repeatedly, so density matters more than target size here.
  std::string cellSVG = fstring(R"#(
    <g class="toolbutton" layout="box">
      <rect class="background" width="%g" height="%g"/>
      <circle class="btn-color" cx="%g" cy="%g" r="%g"/>
    </g>
  )#", double(PALETTE_CELL), double(PALETTE_CELL), double(PALETTE_CELL/2),
      double(PALETTE_CELL/2), double(PALETTE_DOT));

  int cell = 0;
  auto addCell = [grid, &cell](Button *btn) {
    if (cell > 0 && cell % PALETTE_GRID_COLS == 0)
      btn->node->setAttribute("flex-break", "before");
    btn->setMargins(0);
    grid->addWidget(btn);
    ++cell;
  };
  for (Color c : offer) {
    Button *btn = new Button(loadSVGFragment(cellSVG.c_str()));
    btn->selectFirst(".btn-color")->node->setAttr<color_t>("fill", ScribbleApp::displayColor(toolColor(c)).color);
    setupTooltip(btn, colorToHex(c).c_str());
    btn->onClicked = [onPick, c]() { onPick(c); };
    addCell(btn);
  }
  // ...and the escape hatch, last, so it reads as "or something else"
  Button *customBtn = createToolbutton(
      SvgGui::useFile(":/icons/ic_menu_add_color.svg"), _("Custom Color"));
  customBtn->onClicked = onCustom;
  addCell(customBtn);
}

// filter to help reduce creation of unnecessary undo items
void PenToolbar::trackEditFocus(Widget *field) {
  field->addHandler([this](SvgGui *gui, SDL_Event *event) {
    if (event->type == SvgGui::FOCUS_GAINED)
      changesSinceFocused = 0;
    else if (event->type == SvgGui::FOCUS_LOST)
      changesSinceFocused = -1;
    return false; // continue
  });
}

// the menu a single control sits in (the selection popup), or NULL on a toolbar row
static Widget *enclosingMenu(Widget *widget) {
  for (Widget *ancestor = widget->parent(); ancestor; ancestor = ancestor->parent()) {
    if (ancestor->node->hasClass("menu"))
      return ancestor;
  }
  return NULL;
}

// Closes the popups opened from `btn`, but not the menu `btn` sits in.  closeAutoClosePopup() closes
//  every open menu, which inside the selection popup would take the selection popup down with it.
static void closePopupsFrom(Button *btn) {
  SvgGui *gui = btn->window() ? btn->window()->gui() : NULL;
  if (gui)
    gui->closeMenus(btn);
}

// setupAutoClosePopup(), except that a press elsewhere in the menu holding `btn` closes only this popup
//  and then goes on to what was pressed - the selection popup's other items stay usable with one of
//  these open.  A press on `btn` itself is swallowed: it only closes the popup, where its click would
//  otherwise open it straight back up.
static void setupSinglePopup(ArrowPopup *popup, Button *btn) {
  popup->isPressedGroupContainer = true;
  popup->addHandler([btn](SvgGui *gui, SDL_Event *event) {
    if (event->type != SvgGui::OUTSIDE_PRESSED && event->type != SvgGui::OUTSIDE_MODAL)
      return false;
    Widget *target = static_cast<Widget *>(event->user.data2);
    Widget *menu = enclosingMenu(btn);
    if (menu && target && target->isDescendantOf(menu))
      gui->closeMenus(btn);
    else
      gui->closeMenus();
    bool onBtn = target && target->isDescendantOf(btn);
    return event->type == SvgGui::OUTSIDE_PRESSED || onBtn;
  });
}

Widget *PenToolbar::createSingleSwatch(std::function<void(Color)> onPicked) {
  singleSwatches.emplace_back(new SingleSwatch());
  SingleSwatch *sw = singleSwatches.back().get();
  sw->onPicked = onPicked;
  sw->btn = new Button(compactColorBtnNode->clone());
  sw->btn->setMargins(0);  // the swatch cell already includes the mockup's spacing
  setupTooltip(sw->btn, _("Color"));
  sw->btn->onClicked = [this, sw]() { openSingleSwatch(sw); };

  sw->palettePopup = createArrowPopup(Menu::VERT_LEFT);
  sw->palettePopup->selectFirst(".child-container")->setMargins(8);  // as for palettePopup
  sw->paletteGrid = createRow({}, "0 0", "flex-start");
  sw->paletteGrid->node->setAttribute("flex-wrap", "wrap");
  sw->paletteGrid->node->setAttribute("margin", "4 4");
  sw->palettePopup->addWidget(sw->paletteGrid);
  setupSinglePopup(sw->palettePopup, sw->btn);

  // the custom color editor, as for a saved swatch, but setting the pen directly
  sw->customPopup = createArrowPopup(Menu::VERT_LEFT);
  ColorSliders *sliders = new ColorSliders(new SvgG());
  sw->customPicker = createColorEditBox(true, sliders);
  sw->customPicker->setMargins(0, 0, 0, 4);
  sw->customPicker->onColorChanged = [this, sw](Color c) { pickSingleSwatch(sw, c); };
  Widget *tabs = createTabBar({"RGB", "HSV"}, [sliders](int tabnum) {
    sliders->setVisibleGroup(tabnum != 0);
  });
  tabs->setMargins(12, 0, 0, 0);
  Widget *title = createTitledRow(_("Color"), sw->customPicker);
  title->selectFirst(".row-text")->node->addClass("color-popup-title");
  sw->customPopup->addWidget(title);
  sw->customPopup->addWidget(tabs);
  sw->customPopup->addWidget(sliders);
  setupSinglePopup(sw->customPopup, sw->btn);

  Widget *group = createRow();
  group->node->setAttribute("box-anchor", "");
  group->addWidget(sw->btn);
  group->addWidget(sw->palettePopup);
  group->addWidget(sw->customPopup);
  updateSingles();
  return group;
}

void PenToolbar::openSingleSwatch(SingleSwatch *sw) {
  if (sw->palettePopup->isVisible() || sw->customPopup->isVisible()) {
    closePopupsFrom(sw->btn);
    return;
  }
  closePopupsFrom(sw->btn);  // a sibling's popup, if one is open
  auto openCustom = [this, sw]() {
    closePopupsFrom(sw->btn);
    sw->customPicker->setColor(pen.color);
    openAutoClosePopup(sw->customPopup);
  };
  const Palette *pal = docPalette();
  // no theme: no grid to show, so straight to the color editor, as the "+" does
  if (!pal || pal->families.empty()) {
    openCustom();
    return;
  }
  fillColorGrid(sw->paletteGrid, [this, sw](Color c) {
    closePopupsFrom(sw->btn);
    pickSingleSwatch(sw, toolColor(c));
  }, openCustom);
  openAutoClosePopup(sw->palettePopup);
}

// updateColor() does the snapping and, in SELECTION_MODE, recolors the selection
void PenToolbar::pickSingleSwatch(SingleSwatch *sw, Color color) {
  colorPicker->setColor(color);
  updateColor();
  if (sw->onPicked)
    sw->onPicked(pen.color);
}

Widget *PenToolbar::createSingleWidth(std::function<void()> onWidthChanged) {
  singleWidths.emplace_back(new SingleWidth());
  SingleWidth *sw = singleWidths.back().get();
  sw->onChanged = onWidthChanged;
  // the draw row's thickness cell, with a longer line so that a dash pattern has room to show
  std::string btnSVG = fstring(R"#(
    <g class="toolbutton swatch-btn" layout="box">
      <rect class="background" width="%g" height="%g"/>
      <line class="icon width-line" x1="%g" y1="%g" x2="%g" y2="%g"
          fill="none" stroke="currentColor" stroke-width="1" stroke-linecap="round"/>
    </g>
  )#", 56*floatUIScale, 64*floatUIScale, 12*floatUIScale, 32*floatUIScale, 44*floatUIScale, 32*floatUIScale);
  sw->btn = new Button(loadSVGFragment(btnSVG.c_str()));
  sw->btn->setMargins(0);
  setupTooltip(sw->btn, _("Width"));
  sw->btn->onClicked = [this, sw]() { openSingleWidth(sw); };

  sw->popup = createArrowPopup(Menu::VERT_LEFT);
  // the presets, filled in on each open since the list can change in between (see openSingleWidth)
  sw->presetRow = createRow({}, "0 0", "flex-start");
  sw->popup->addWidget(sw->presetRow);
  sw->spin = createTextSpinBox(1.25, 0.01, 0, 200, "%.3g", 120);
  // spinWidth always holds the same value as this one (setSingleWidth), so its stepping applies as is
  sw->spin->onStep = spinWidth->onStep;
  sw->spin->onValueChanged = [this, sw](Dim width) { setSingleWidth(sw, width); };
  trackEditFocus(sw->spin);
  sw->spinRow = createTitledRow(_("Width"), sw->spin);
  sw->popup->addWidget(sw->spinRow);

  // solid, dashed, dotted: each drawn as what it gives, at a thickness where the pattern reads
  const char *titles[] = {_("Solid"), _("Dashed"), _("Dotted")};
  const Dim iconWidth = 3;
  Widget *dashRow = createRow({}, "0 0", "flex-start");
  for (int style = ScribblePen::DASH_SOLID; style <= ScribblePen::DASH_DOTTED; ++style) {
    Dim dash, gap;
    ScribblePen::dashFor(style, iconWidth, &dash, &gap);
    std::string dashAttr = style == ScribblePen::DASH_SOLID ? ""
        : fstring("stroke-dasharray=\"%g %g\"", dash, gap);
    std::string iconSVG = fstring(R"#(
      <g class="toolbutton swatch-btn" layout="box">
        <rect class="background" width="60" height="34"/>
        <line class="icon" x1="10" y1="17" x2="50" y2="17"
            fill="none" stroke="currentColor" stroke-width="%g" stroke-linecap="round" %s/>
      </g>
    )#", iconWidth, dashAttr.c_str());
    Button *btn = new Button(loadSVGFragment(iconSVG.c_str()));
    btn->setMargins(0);
    setupTooltip(btn, titles[style]);
    btn->onClicked = [this, sw, style]() {
      setDashStyle(style);
      if (sw->onChanged)
        sw->onChanged();
    };
    dashRow->addWidget(btn);
    sw->dashBtns[style] = btn;
  }
  sw->popup->addWidget(createTitledRow(_("Line"), dashRow));
  setupSinglePopup(sw->popup, sw->btn);

  Widget *group = createRow();
  group->node->setAttribute("box-anchor", "");
  group->addWidget(sw->btn);
  group->addWidget(sw->popup);
  updateWidthPopup();  // the spin box's limits and label for the width's unit
  updateSingles();
  return group;
}

void PenToolbar::openSingleWidth(SingleWidth *sw) {
  if (sw->popup->isVisible()) {
    closePopupsFrom(sw->btn);
    return;
  }
  closePopupsFrom(sw->btn);  // a sibling's popup, if one is open
  // the presets are those of the tool in hand, which differ per tool and change as they are edited
  SvgGui *gui = sw->btn->window() ? sw->btn->window()->gui() : NULL;
  if (gui)
    gui->deleteContents(sw->presetRow);
  sw->presets.clear();
  bool relwidths = relativeWidths();
  for (size_t ii = 0; ii < activeWidths().size(); ++ii) {
    Dim width = presetWidth(ii);  // a selection's presets are absolute even while the pen's are not
    Button *btn = new Button(compactWidthBtnNode->clone());
    btn->containerNode()->selectFirst(".width-line")->setAttr("stroke-width", widthPreview(width));
    btn->setMargins(0);
    btn->onClicked = [this, sw, width]() { setSingleWidth(sw, width); };
    setupTooltip(btn, relwidths ? fstring(_("Pen width: %.2g line heights"), width).c_str()
                                : fstring(_("Pen width: %.1f"), width).c_str());
    sw->presetRow->addWidget(btn);
    sw->presets.push_back(btn);
  }
  updateSingles();
  openAutoClosePopup(sw->popup);
}

// through spinWidth and updateWidth(), which resize the selection in SELECTION_MODE
void PenToolbar::setSingleWidth(SingleWidth *sw, Dim width) {
  spinWidth->setValue(width);
  updateWidth();
  if (sw->onChanged)
    sw->onChanged();
}

void PenToolbar::updateSingles() {
  for (auto &sw : singleSwatches)
    sw->btn->selectFirst(".btn-color")->node->setAttr<color_t>("fill", ScribbleApp::displayColor(pen.color).color);
  const std::vector<Dim> &widths = activeWidths();
  for (auto &sw : singleWidths) {
    // a selection of mixed widths has none to show, so it gets a hairline
    Dim preview = pen.width > 0 ? widthPreview(pen.width) : floatUIScale;
    SvgNode *line = sw->btn->containerNode()->selectFirst(".width-line");
    line->setAttr("stroke-width", preview);
    if (dashStyle > ScribblePen::DASH_SOLID) {
      Dim dash, gap;
      ScribblePen::dashFor(dashStyle, preview, &dash, &gap);
      line->setAttribute("stroke-dasharray", fstring("%g %g", dash, gap).c_str());
    }
    else
      line->removeAttr("stroke-dasharray");
    // only when it differs: this runs on every edit, including the ones typed into this very box
    if (pen.width > 0 && sw->spin->value() != pen.width)
      sw->spin->setValue(pen.width);
    for (size_t ii = 0; ii < sw->presets.size() && ii < widths.size(); ++ii)
      sw->presets[ii]->setChecked(WidthPresets::sameWidth(presetWidth(ii), pen.width));
    for (int style = ScribblePen::DASH_SOLID; style <= ScribblePen::DASH_DOTTED; ++style)
      sw->dashBtns[style]->setChecked(style == dashStyle);
  }
}

void PenToolbar::setDashStyle(int style) {
  if (mode == BOOKMARK_MODE)
    return;
  dashStyle = style;
  // a pen carries the pattern itself; a selection's is applied by the app, element by element, since
  //  each is sized by that element's own width
  if (mode != SELECTION_MODE)
    pen.setDashStyle(style);
  updateSelected();
  if (onChanged)
    onChanged(mode == SELECTION_MODE ? DASH_CHANGED : PEN_CHANGED);
}

void PenToolbar::rebuildGrids() {
  closeAutoClosePopup(colorPopup);
  colorPopupIdx = -1;
  // the indices both popups hold are into the list being rebuilt, so neither may outlive it
  closeAutoClosePopup(palettePopup);
  paletteEditIdx = -1;
  colorPalette->clear();
  widthPalette->clear();

  for (size_t ii = 0; ii < savedColors.size(); ++ii) {
    Color color = toolColor(savedColors[ii]);
    Button *btn =
        compact ? new Button(compactColorBtnNode->clone()) : createColorBtn();
    btn->selectFirst(".btn-color")->node->setAttr<color_t>("fill", ScribbleApp::displayColor(color).color);
    // setting on <g class=toolbutton gets overridden by toolbutton CSS fill -
    // do we need class=toolbutton?
    btn->onClicked = [this, ii]() { selectColor(int(ii)); };

    // A themed swatch has no "insert current" or "delete": the list is generated, so editing one
    //  entry would be undone by the next rebuild. Leaving the menu wired up would offer an action
    //  that silently does nothing.
    if (!themed) {
      SvgGui::setupRightClick(btn, [this, ii](SvgGui *gui, Widget *w, Point p) {
        contextMenuIdx = ii;
        gui->showContextMenu(colorCtxMenu, p, w);
      });
      setupTooltip(btn, altTooltip(colorToHex(color).c_str(), _("Edit")));
    }
    else
      setupTooltip(btn, altTooltip(
          ii == 0 ? _("Neutral") : colorToHex(color).c_str(), _("Edit")));
    colorPalette->addButton(btn);
    if (compact)
      btn->setMargins(
          0); // the swatch cell already includes the mockup's spacing
  }
  colorMenuDelete->setEnabled(savedColors.size() > 1);

  const std::vector<Dim> &widths = activeWidths();
  bool relwidths = relativeWidths();
  for (size_t ii = 0; ii < widths.size(); ++ii) {
    Dim width = presetWidth(ii);
    Button *btn =
        new Button((compact ? compactWidthBtnNode : widthBtnNode)->clone());
    // btn->containerNode()->selectFirst(".width-circle")->setTransform(Transform2D::scaling(sc));
    btn->containerNode()
        ->selectFirst(".width-line")
        ->setAttr("stroke-width", widthPreview(width));
    btn->onClicked = [this, ii]() { selectWidth(int(ii)); };
    setupTooltip(btn, altTooltip(
        relwidths ? fstring(_("Pen width: %.2g line heights"), width).c_str()
                  : fstring(_("Pen width: %.1f"), width).c_str(), _("Edit")));
    widthPalette->addButton(btn);
    if (compact)
      btn->setMargins(0);
  }
  updateSelected();
}

// tapping the already selected preset opens the detail editor for it
void PenToolbar::selectWidth(int idx) {
  if (WidthPresets::sameWidth(presetWidth(idx), pen.width)) {
    widthPopupIdx = idx;
    if (widthPopup->isVisible())
      closeAutoClosePopup(widthPopup);
    else
      openAutoClosePopup(widthPopup);
    return;
  }
  closeAutoClosePopup(widthPopup);
  widthPopupIdx = idx;
  spinWidth->setValue(presetWidth(idx));
  updateWidth();
}

// Tapping the already selected swatch opens the theme's grid on it: the same menu the "+" gives, plus
//  cancel and delete, so a swatch added by mistake can be replaced or taken back where it sits.
void PenToolbar::selectColor(int idx) {
  if (toolColor(savedColors[idx]) == pen.color) {
    if (palettePopup->isVisible() || colorPopup->isVisible()) {
      closeAutoClosePopup(palettePopup);
      closeAutoClosePopup(colorPopup);
    }
    else
      openPaletteGrid(idx);
    return;
  }
  closeAutoClosePopup(palettePopup);
  closeAutoClosePopup(colorPopup);
  colorPicker->setColor(toolColor(savedColors[idx]));
  updateColor();
}

void PenToolbar::updateSelected() {
  for (size_t ii = 0;
       ii < colorPalette->items.size() && ii < savedColors.size(); ++ii) {
    bool sel = toolColor(savedColors[ii]) == pen.color;
    colorPalette->items[ii]->setChecked(sel);
    // a circular swatch has no background to tint, so mark the selected one
    // with a ring instead
    Widget *ring = colorPalette->items[ii]->selectFirst(".sel-ring");
    if (ring)
      ring->setVisible(sel);
  }
  const std::vector<Dim> &widths = activeWidths();
  for (size_t ii = 0; ii < widthPalette->items.size() && ii < widths.size(); ++ii)
    widthPalette->items[ii]->setChecked(WidthPresets::sameWidth(presetWidth(ii), pen.width));
  updateSingles();
}

void PenToolbar::saveConfig(ScribbleConfig *cfg) const {
  // Always saved, themed or not: these are the user's chosen working set, picked from the theme rather
  //  than generated by it. refreshPalette() reseeds them only when they plainly belong to some other
  //  theme, so persisting them is what makes a pick stick across a restart.
  std::vector<std::string> colorStrs;
  for (Color c : savedColors)
    colorStrs.emplace_back(
        fstring("#%02X%02X%02X%02X", c.alpha(), c.red(), c.green(), c.blue()));
  cfg->set("savedColors", joinStr(colorStrs, ",").c_str());

  // the unit goes into the same string as the numbers, so no save or load can pair them wrongly.  Not
  //  "%.3f" as before: a pen width in line heights is around 0.04, and three decimals of that would
  //  move a preset off the width it was set to, unselecting it after a restart
  cfg->set("savedWidths", savedWidths.serialize().c_str());
  cfg->set("savedMarkerWidths", savedMarkerWidths.serialize().c_str());
  cfg->set("savedEphemeralWidths", savedEphemeralWidths.serialize().c_str());
}

void PenToolbar::setPen(const ScribblePen &newpen, Mode m, int selDashStyle) {
  mode = m;
  dashStyle = m == SELECTION_MODE ? selDashStyle : newpen.dashStyle();
  widthGroup->setEnabled(mode != BOOKMARK_MODE);
  overflowBtn->setEnabled(mode == PEN_MODE);
  overflowBtn->setVisible(!compact && mode != SELECTION_MODE);
  selOverflowBtn->setVisible(mode == SELECTION_MODE);
  settingsBtn->setVisible(mode == PEN_MODE);

  // each tool keeps its own thickness presets, in its own unit, so switching tools (or loading a pen
  //  whose relative size setting differs) rebuilds the palette rather than just re-checking it
  int tool = drawToolForMode(m);
  centerLineToggle->setVisible(mode == PEN_MODE && tool == ScribbleMode::DRAWTOOL_HIGHLIGHT);
  centerLineToggle->setChecked(newpen.hasFlag(ScribblePen::CENTER_ON_LINE));
  if (newpen == pen && tool == widthsTool) {
    updateSingles();  // a selection's line style is not part of its pen, so it may still have changed
    return;
  }
  bool rebuild = tool != widthsTool
      || relativeWidths() != newpen.hasFlag(ScribblePen::WIDTH_RELATIVE);
  widthsTool = tool;
  pen = newpen;
  rebuild = prepareWidths() || rebuild;
  closeAutoClosePopup(widthPopup);
  closeAutoClosePopup(colorPopup);
  colorPicker->setColor(pen.color);
  updateWidthPopup();  // sets the spinbox limits for the width's unit, so it must precede setValue
  spinWidth->setValue(pen.width);
  cbSnaptoGrid->setChecked(pen.hasFlag(ScribblePen::SNAP_TO_GRID));
  cbLineDrawing->setChecked(pen.hasFlag(ScribblePen::LINE_DRAWING));
  // a pen carrying neither flag is drawn by StrokedStrokeBuilder, whose cap and join are round - so it
  //  shows as Round, which is what it looks like.  setIndex() does not fire onChanged, unlike
  //  updateIndex(), so this cannot write the pen back to itself.
  comboPenTip->setIndex(pen.hasFlag(ScribblePen::TIP_FLAT) ? 0
      : (pen.hasFlag(ScribblePen::TIP_CHISEL) ? 2 : 1));
  // rounded, so a legacy pen's 0.85 shows as 85% and is not rewritten until the user edits it
  spinPressure->setValue(std::round(100*pen.pressureSensitivity()));
  if (rebuild)
    // refreshPalette() rather than rebuildGrids(): switching to or from the marker changes which
    //  variant the swatches show, so the color list itself has to be rebuilt, not just redrawn
    refreshPalette();
  else
    updateSelected();
}

// void PenToolbar::dragWidth(int delta)
//{
//   // 1 pixel = 1% change, but done such that only absolute change matters
//   (i.e. 1,1 gives same result as 2) pen.width = MIN(Dim(200),
//   Dim(pen.width*pow(1.01, delta)));
//   // this will result in call to updatePen()
//   spinWidth->setValue(pen.width);
// }

void PenToolbar::updateColor() {
  pen.color = colorPicker->color();
  // The reluctance (COLORS_SPEC.md §6.1): a color typed into the hex box or dialled on the sliders is
  //  answered with the nearest thing that belongs to the theme rather than refused. Snapping happens
  //  here, after the picker has reported, so every route into the pen color goes through it.
  // The picker is only pushed back if the value actually moved, or setColor() would re-enter this.
  if (themed && !offPaletteAllowed()) {
    const Palette *pal = docPalette();
    if (pal) {
      Color snapped = pal->nearest(pen.color);
      // alpha is the marker's business, not the palette's, so it is carried across
      snapped.setAlpha(pen.color.alpha());
      if (!(snapped == pen.color)) {
        pen.color = snapped;
        colorPicker->setColor(snapped);
      }
    }
  }
  updateSelected();
  updateWidthPopup();  // the relative size display draws its stroke in the pen color
  if (onChanged)
    onChanged(COLOR_CHANGED | (changesSinceFocused > 0 ? UNDO_PREV : 0));
  if (changesSinceFocused >= 0)
    ++changesSinceFocused;
}

void PenToolbar::updateWidth() {
  pen.width = spinWidth->value();
  // a pen's dash pattern is sized by its width (a selection's is rescaled element by element)
  if (mode != SELECTION_MODE)
    pen.setDashStyle(dashStyle);
  // editing the width while the detail popup is open redefines that preset
  if (widthPopup->isVisible() && widthPopupIdx >= 0 &&
      widthPopupIdx < int(activeWidths().size())) {
    // in the list's own unit: a selection's width is absolute while the pen's list may be relative,
    //  and storing it raw is what turned a 3.2 unit stroke into a 3.2 line height (144 unit) preset
    activePresets().setFrom(widthPopupIdx, pen.width, relativeWidths(), lineHeight());
    widthPalette->items[widthPopupIdx]
        ->containerNode()
        ->selectFirst(".width-line")
        ->setAttr("stroke-width", widthPreview(pen.width));
  }
  updateSelected();
  updateWidthPopup();

  if (onChanged)
    onChanged(WIDTH_CHANGED | (changesSinceFocused > 0 ? UNDO_PREV : 0));
  if (changesSinceFocused >= 0)
    ++changesSinceFocused;
}

void PenToolbar::updatePen() {
  pen.setFlag(ScribblePen::SNAP_TO_GRID, cbSnaptoGrid->isChecked());
  pen.setFlag(ScribblePen::LINE_DRAWING, cbLineDrawing->isChecked());

  if (onChanged)
    onChanged(PEN_CHANGED);
}

// which tool's presets belong on the row: only a pen has a draw tool, so a selection's or a bookmark's
//  width falls back to the plain pen's list, which is the one in absolute units by default
int PenToolbar::drawToolForMode(Mode m) const {
  ScribbleMode *scribbleMode =
      ScribbleApp::app ? ScribbleApp::app->scribbleMode : NULL;
  return m == PEN_MODE && scribbleMode ? scribbleMode->drawTool
                                       : int(ScribbleMode::DRAWTOOL_PEN);
}

WidthPresets& PenToolbar::activePresets() {
  return widthsTool == ScribbleMode::DRAWTOOL_HIGHLIGHT ? savedMarkerWidths
      : (widthsTool == ScribbleMode::DRAWTOOL_EPHEMERAL ? savedEphemeralWidths : savedWidths);
}

const WidthPresets& PenToolbar::activePresets() const {
  return const_cast<PenToolbar*>(this)->activePresets();
}

std::vector<Dim>& PenToolbar::activeWidths() { return activePresets().widths; }

const std::vector<Dim>& PenToolbar::activeWidths() const { return activePresets().widths; }

Dim PenToolbar::presetWidth(size_t idx) const {
  return activePresets().inUnit(idx, relativeWidths(), lineHeight());
}

// A preset list's numbers mean nothing without its unit, so the list carries one (WidthPresets).  Lists
//  start empty and are seeded here, once the tool is in hand and its unit is known: a fixed default in
//  the config could only be in one unit.  In PEN_MODE the list is kept in the pen's unit, converted
//  (never reinterpreted) when they differ - a saved pen recalled with the other relative setting, say.
//  A selection or a bookmark leaves the list alone and reads it through presetWidth() instead, since
//  its pen is absolute whatever the draw pen is.
bool PenToolbar::prepareWidths() {
  WidthPresets &presets = activePresets();
  Dim lh = lineHeight();
  bool changed = false;
  if (presets.unit == WidthPresets::UNIT_UNKNOWN) {
    // a list saved before the unit was: the pairing the old code assumed, with that tool's own pen
    ScribbleMode *scribbleMode = ScribbleApp::app ? ScribbleApp::app->scribbleMode : NULL;
    bool toolRelative = mode == PEN_MODE ? relativeWidths()
        : (scribbleMode && scribbleMode->penForDrawTool(widthsTool).hasFlag(ScribblePen::WIDTH_RELATIVE));
    presets.convertTo(toolRelative, lh);
    changed = true;
  }
  if (mode == PEN_MODE && presets.isRelative() != relativeWidths()) {
    presets.convertTo(relativeWidths(), lh);
    changed = true;
  }
  bool marker = widthsTool == ScribbleMode::DRAWTOOL_HIGHLIGHT;
  // MARKER_WIDTHS are line heights, DEFAULT_WIDTHS document units
  return presets.normalize(marker ? MARKER_WIDTHS : DEFAULT_WIDTHS, marker, lh) || changed;
}

// A marker stroke is its family at another alpha (Palette::indexOf ignores alpha), so this maps a
//  swatch to the highlighter variant of whatever family it belongs to.  The saved list itself is left
//  in ink colors: it is shared by every tool, and storing it per tool would mean the user's picks
//  splitting into three lists that drift apart.
// Off-palette colors are passed through untouched apart from the marker's alpha - the point of the
//  escape hatch is that the color asked for is the color drawn.
Color PenToolbar::toolColor(Color c) const {
  if (widthsTool != ScribbleMode::DRAWTOOL_HIGHLIGHT)
    return c;
  const Palette *pal = themed ? docPalette() : NULL;
  int family = -1, variant = PALETTE_BASE;
  if (pal && pal->indexOf(c, &family, &variant) && family >= 0)
    return pal->families[family].hl;
  // the neutral has no highlighter variant of its own (it is exempt from the walk), and neither does
  //  an off-palette color; both simply take the marker's alpha
  Color out = c;
  out.setAlpha(pal && !pal->families.empty() ? pal->families[0].hl.alpha() : 127);
  return out;
}

bool PenToolbar::relativeWidths() const {
  return pen.hasFlag(ScribblePen::WIDTH_RELATIVE);
}

// the page being drawn on, which the relative width and the display are both measured against
Page* PenToolbar::currentPage() const {
  ScribbleArea *area = ScribbleApp::app ? ScribbleApp::app->activeArea() : NULL;
  return area ? area->getCurrPage() : NULL;
}

Dim PenToolbar::lineHeight() const {
  Page *page = currentPage();
  // yruling(true) falls back to the blank page ruling, so this is never zero
  if (!page)
    return Page::BLANK_Y_RULING;
  // a selection inside a Paper Patch is measured in the patch's line, as drawing there is
  ScribbleArea *area = ScribbleApp::app->activeArea();
  const Selection *sel = mode == SELECTION_MODE && area ? area->selection() : NULL;
  if (sel && !sel->strokes.empty()) {
    std::vector<Point> centres;
    for (Element *stroke : sel->strokes)
      centres.push_back(stroke->com());
    return page->lineHeightFor(centres);
  }
  return page->yruling(true);
}

// a preset is previewed at the thickness it will actually draw at, so a relative one has to be
//  resolved against the line height first - otherwise every marker preset would be a hairline
Dim PenToolbar::widthPreview(Dim w) const {
  Dim unit = relativeWidths() ? lineHeight() : 1;
  Dim maxw = 0;
  for (size_t ii = 0; ii < activeWidths().size(); ++ii)
    maxw = std::max(maxw, presetWidth(ii)*unit);
  Dim sc = w*unit;
  // every marker preset is well past the cell's cap, so clamping them would draw three identical
  //  bars; once anything in the set exceeds the cap the whole set is scaled down to it instead,
  //  which keeps the presets apart.  A pen's presets are all below the cap, so nothing changes there.
  if (maxw > penWidthPreviewMax)
    sc *= penWidthPreviewMax/maxw;
  sc = std::min(penWidthPreviewMax, sc);  // the width being edited can be past the largest preset
  return compact ? sc*floatUIScale : sc;  // the compact swatch cell is scaled too
}

// The toggle converts the pen's width and its presets between units instead of reinterpreting the
//  numbers, so nothing changes thickness on screen when it is flipped: 0.75 of a 40 unit ruling
//  becomes 30, and 30 becomes 0.75 again.  The presets are one list per tool kept in that tool's
//  current unit rather than two lists per tool, since a second list would have to be kept in step
//  with this one to no benefit - the two are the same set of thicknesses either way.
void PenToolbar::setRelativeWidth(bool relative) {
  if (relative == pen.hasFlag(ScribblePen::WIDTH_RELATIVE))
    return;
  Dim lh = lineHeight();
  activePresets().convertTo(relative, lh);
  pen.setFlag(ScribblePen::WIDTH_RELATIVE, relative);
  pen.width = relative ? pen.width/lh : pen.width*lh;
  pen.setDashStyle(dashStyle);  // the pattern is in the width's unit
  updateWidthPopup();  // new limits for the new unit, so this must precede setValue
  spinWidth->setValue(pen.width);
  rebuildGrids();
  // both the width and a flag changed, so the whole pen has to go back to the app
  if (onChanged)
    onChanged(PEN_CHANGED);
}

void PenToolbar::updateWidthPopup() {
  // a selection's or a bookmark's width is a number in the document, with no pen to carry the flag
  relWidthRow->setVisible(mode == PEN_MODE);
  cbRelWidth->setChecked(pen.hasFlag(ScribblePen::WIDTH_RELATIVE));
  bool rel = relativeWidths();
  // in line heights, 4 is already absurdly thick and 200 would be nonsense
  spinWidth->setLimits(WidthPresets::MIN_WIDTH, WidthPresets::maxWidth(rel));
  widthSpinRow->selectFirst(".row-text")
      ->setText(rel ? _("Line heights") : _("Width"));
  for (auto &sw : singleWidths) {
    sw->spin->setLimits(WidthPresets::MIN_WIDTH, WidthPresets::maxWidth(rel));
    sw->spinRow->selectFirst(".row-text")->setText(rel ? _("Line heights") : _("Width"));
  }
  // the display is always up, so an absolute width is shown against the ruling too - it is drawn from
  //  the width in line heights either way, which is what the display's scale is
  Page *page = currentPage();
  rulingPreview->node->selectFirst(".page-bg")
      ->setAttr<color_t>("fill", ScribbleApp::displayColor(page ? page->props.color : Color::WHITE).color);
  Color rulecolor = ScribbleApp::displayColor(page ? page->props.ruleColor : Color(0, 0, 0xFF, 0x9F));
  for (SvgNode *rule : rulingPreview->node->select(".rule-line"))
    rule->setAttr<color_t>("stroke", rulecolor.color);
  Widget *penLine = rulingPreview->selectFirst(".pen-line");
  Dim lineheights = rel ? pen.width : pen.width/lineHeight();
  penLine->node->setAttr<color_t>("stroke", ScribbleApp::displayColor(pen.color).color);
  // a stroke wider than the display is clamped rather than allowed to spill out of it
  penLine->node->setAttr("stroke-width", std::min(
      (RULING_PREVIEW_LINES - 1)*RULING_PREVIEW_SPACING, lineheights*RULING_PREVIEW_SPACING));
}
