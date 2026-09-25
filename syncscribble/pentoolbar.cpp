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
  if (compact && !compactColorBtnNode)
    compactColorBtnNode.reset(loadSVGFragment(compactColorBtnSVG.c_str()));

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
  StringRef widthStr(
      ScribbleApp::cfg->String("savedWidths", "1.6,2.4,3.0,4.0"));
  parseNumbersList(widthStr, savedWidths);
  // may be empty - on a fresh config, and on one written before these lists existed, they are seeded
  //  on first use, in the unit that tool's width is in (see seedWidths)
  StringRef markerWidthStr(ScribbleApp::cfg->String("savedMarkerWidths", ""));
  parseNumbersList(markerWidthStr, savedMarkerWidths);
  StringRef ephWidthStr(ScribbleApp::cfg->String("savedEphemeralWidths", ""));
  parseNumbersList(ephWidthStr, savedEphemeralWidths);

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

  widthCtxMenu = createMenu(Menu::FLOATING, false);
  widthCtxMenu->addItem(_("Insert Current"), [this]() {
    std::vector<Dim>& widths = activeWidths();
    widths.erase(std::remove(widths.begin(), widths.end(), pen.width), widths.end());
    int pos = std::min(contextMenuIdx + 1, int(widths.size()));
    widths.insert(widths.begin() + pos, pen.width);
    rebuildGrids();
  });
  widthMenuDelete = widthCtxMenu->addItem(_("Delete"), [this]() {
    activeWidths().erase(activeWidths().begin() + contextMenuIdx);
    rebuildGrids();
  });

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
        ->node->setAttr<color_t>("fill", c.color);
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

  // filter to help reduce creation of unnecessary undo items
  auto focusFilt = [this](SvgGui *gui, SDL_Event *event) {
    if (event->type == SvgGui::FOCUS_GAINED)
      changesSinceFocused = 0;
    else if (event->type == SvgGui::FOCUS_LOST)
      changesSinceFocused = -1;
    return false; // continue
  };
  colorPicker->addHandler(focusFilt);
  spinWidth->addHandler(focusFilt);

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

  settingsBtn = createToolSettingsButton(
      "Pen Settings", {"inputSmoothing", "inputSimplify", "shapeSnapDelay", "applyPenToSel", "savePenMode"},
      compact ? std::vector<Button *>{cbSnaptoGrid, cbLineDrawing} : std::vector<Button *>{},
      {penTipRow});

  Button *helpBtn = createHelpButton(
      {{"ic_menu_add_color.svg", "Add Color",
        "Adds the current color to the saved colors."},
       {"ic_menu_set_pen.svg", "Thickness",
        "Sets stroke width; tap the selected preset again to edit its value."},
       {"ic_menu_settings2.svg", "Pen Settings",
        "Smoothing, simplification and other pen preferences."},
       {"ic_menu_overflow.svg", "More Options",
        "Snap strokes to the grid or draw straight lines."},
       {"ic_menu_cancel.svg", "Close", "Hides this options row."}});

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
  addWidget(widthCtxMenu);

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

  SvgGui *gui = window() ? window()->gui() : NULL;
  if (gui)
    gui->deleteContents(paletteGrid);
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
  auto addCell = [this, &cell](Button *btn) {
    if (cell > 0 && cell % PALETTE_GRID_COLS == 0)
      btn->node->setAttribute("flex-break", "before");
    btn->setMargins(0);
    paletteGrid->addWidget(btn);
    ++cell;
  };
  for (Color c : offer) {
    Button *btn = new Button(loadSVGFragment(cellSVG.c_str()));
    btn->selectFirst(".btn-color")->node->setAttr<color_t>("fill", toolColor(c).color);
    setupTooltip(btn, colorToHex(c).c_str());
    btn->onClicked = [this, c]() {
      int idx = paletteEditIdx;
      closeAutoClosePopup(palettePopup);
      if (idx >= 0)
        setSwatchColor(idx, c);
      else
        addSwatchColor(c);
    };
    addCell(btn);
  }
  // ...and the escape hatch, last, so it reads as "or something else"
  Button *customBtn = createToolbutton(
      SvgGui::useFile(":/icons/ic_menu_add_color.svg"), _("Custom Color"));
  customBtn->onClicked = [this]() { openCustomColor(paletteEditIdx); };
  addCell(customBtn);
  openAutoClosePopup(palettePopup);
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
    btn->selectFirst(".btn-color")->node->setAttr<color_t>("fill", color.color);
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
    Dim width = widths[ii];
    Button *btn =
        new Button((compact ? compactWidthBtnNode : widthBtnNode)->clone());
    // btn->containerNode()->selectFirst(".width-circle")->setTransform(Transform2D::scaling(sc));
    btn->containerNode()
        ->selectFirst(".width-line")
        ->setAttr("stroke-width", widthPreview(width));
    btn->onClicked = [this, ii]() { selectWidth(int(ii)); };

    SvgGui::setupRightClick(btn, [this, ii](SvgGui *gui, Widget *w, Point p) {
      contextMenuIdx = ii;
      gui->showContextMenu(widthCtxMenu, p, w);
    });
    setupTooltip(btn, altTooltip(
        relwidths ? fstring(_("Pen width: %.2g line heights"), width).c_str()
                  : fstring(_("Pen width: %.1f"), width).c_str(), _("Edit")));
    widthPalette->addButton(btn);
    if (compact)
      btn->setMargins(0);
  }
  widthMenuDelete->setEnabled(widths.size() > 1);
  updateSelected();
}

// tapping the already selected preset opens the detail editor for it
void PenToolbar::selectWidth(int idx) {
  if (activeWidths()[idx] == pen.width) {
    widthPopupIdx = idx;
    if (widthPopup->isVisible())
      closeAutoClosePopup(widthPopup);
    else
      openAutoClosePopup(widthPopup);
    return;
  }
  closeAutoClosePopup(widthPopup);
  widthPopupIdx = idx;
  spinWidth->setValue(activeWidths()[idx]);
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
    widthPalette->items[ii]->setChecked(widths[ii] == pen.width);
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

  std::vector<std::string> widthStrs;
  for (Dim w : savedWidths)
    widthStrs.push_back(fstring("%.3f", w));
  cfg->set("savedWidths", joinStr(widthStrs, ",").c_str());

  std::vector<std::string> markerWidthStrs;
  for (Dim w : savedMarkerWidths)
    markerWidthStrs.push_back(fstring("%.3f", w));
  cfg->set("savedMarkerWidths", joinStr(markerWidthStrs, ",").c_str());

  std::vector<std::string> ephWidthStrs;
  for (Dim w : savedEphemeralWidths)
    ephWidthStrs.push_back(fstring("%.3f", w));
  cfg->set("savedEphemeralWidths", joinStr(ephWidthStrs, ",").c_str());
}

void PenToolbar::setPen(const ScribblePen &newpen, Mode m) {
  mode = m;
  widthGroup->setEnabled(mode != BOOKMARK_MODE);
  overflowBtn->setEnabled(mode == PEN_MODE);
  overflowBtn->setVisible(!compact && mode != SELECTION_MODE);
  selOverflowBtn->setVisible(mode == SELECTION_MODE);
  settingsBtn->setVisible(mode == PEN_MODE);

  // each tool keeps its own thickness presets, in its own unit, so switching tools (or loading a pen
  //  whose relative size setting differs) rebuilds the palette rather than just re-checking it
  int tool = drawToolForMode(m);
  if (newpen == pen && tool == widthsTool)
    return;
  bool rebuild = tool != widthsTool
      || relativeWidths() != newpen.hasFlag(ScribblePen::WIDTH_RELATIVE);
  widthsTool = tool;
  pen = newpen;
  rebuild = seedWidths() || rebuild;
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
  // editing the width while the detail popup is open redefines that preset
  if (widthPopup->isVisible() && widthPopupIdx >= 0 &&
      widthPopupIdx < int(activeWidths().size())) {
    activeWidths()[widthPopupIdx] = pen.width;
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

std::vector<Dim>& PenToolbar::activeWidths() {
  return widthsTool == ScribbleMode::DRAWTOOL_HIGHLIGHT ? savedMarkerWidths
      : (widthsTool == ScribbleMode::DRAWTOOL_EPHEMERAL ? savedEphemeralWidths : savedWidths);
}

const std::vector<Dim>& PenToolbar::activeWidths() const {
  return const_cast<PenToolbar*>(this)->activeWidths();
}

// A preset list carries no unit of its own - the numbers mean whatever that tool's relative size
//  toggle currently means - so it cannot have a fixed default in the config: a config written before
//  this existed has absolute widths, and line-height fractions would load there as hairlines.  The
//  lists start empty and are filled here, once the tool is actually in hand and its unit is known.
bool PenToolbar::seedWidths() {
  if (!activeWidths().empty())
    return false;
  Dim lh = lineHeight();
  if (widthsTool == ScribbleMode::DRAWTOOL_HIGHLIGHT) {
    for (Dim w : MARKER_WIDTHS)  // these are line heights
      activeWidths().push_back(relativeWidths() ? w : w*lh);
  }
  else {
    for (Dim w : DEFAULT_WIDTHS)  // these are document units
      activeWidths().push_back(relativeWidths() ? w/lh : w);
  }
  return true;
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
  return page ? page->yruling(true) : Page::BLANK_Y_RULING;
}

// a preset is previewed at the thickness it will actually draw at, so a relative one has to be
//  resolved against the line height first - otherwise every marker preset would be a hairline
Dim PenToolbar::widthPreview(Dim w) const {
  Dim unit = relativeWidths() ? lineHeight() : 1;
  Dim maxw = 0;
  for (Dim preset : activeWidths())
    maxw = std::max(maxw, preset*unit);
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
  for (Dim &w : activeWidths())
    w = relative ? w/lh : w*lh;
  pen.setFlag(ScribblePen::WIDTH_RELATIVE, relative);
  pen.width = relative ? pen.width/lh : pen.width*lh;
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
  spinWidth->setLimits(0.01, rel ? 4 : 200);
  widthSpinRow->selectFirst(".row-text")
      ->setText(rel ? _("Line heights") : _("Width"));
  // the display is always up, so an absolute width is shown against the ruling too - it is drawn from
  //  the width in line heights either way, which is what the display's scale is
  Page *page = currentPage();
  rulingPreview->node->selectFirst(".page-bg")
      ->setAttr<color_t>("fill", (page ? page->props.color : Color::WHITE).color);
  Color rulecolor = page ? page->props.ruleColor : Color(0, 0, 0xFF, 0x9F);
  for (SvgNode *rule : rulingPreview->node->select(".rule-line"))
    rule->setAttr<color_t>("stroke", rulecolor.color);
  Widget *penLine = rulingPreview->selectFirst(".pen-line");
  Dim lineheights = rel ? pen.width : pen.width/lineHeight();
  penLine->node->setAttr<color_t>("stroke", pen.color.color);
  // a stroke wider than the display is clamped rather than allowed to spill out of it
  penLine->node->setAttr("stroke-width", std::min(
      (RULING_PREVIEW_LINES - 1)*RULING_PREVIEW_SPACING, lineheights*RULING_PREVIEW_SPACING));
}
