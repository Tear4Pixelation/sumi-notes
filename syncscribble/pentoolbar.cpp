#include "pentoolbar.h"
#include "scribbleapp.h"
#include "touchwidgets.h"
#include "configdialog.h"
#include "ugui/textedit.h"
#include "usvg/svgparser.h" // for parseNumbersList

// or have spinbox buttons step by 1.25x or 0.8x of current value?
const Dim PenToolbar::PEN_WIDTHS[] = {
    0.1, 0.25, 0.5, 0.75, 1.0, 1.2, 1.4, 1.6, 1.8, 2.0, 2.4, 2.8,
    3.2, 3.6,  4.0, 5.0,  6.0, 8.0, 10,  12,  16,  20,  25,  30,
    35,  40,   48,  60,   72,  86,  100, 125, 150, 175, 200};

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
    savedWidths.erase(
        std::remove(savedWidths.begin(), savedWidths.end(), pen.width),
        savedWidths.end());
    int pos = std::min(contextMenuIdx + 1, int(savedWidths.size()));
    savedWidths.insert(savedWidths.begin() + pos, pen.width);
    rebuildGrids();
  });
  widthMenuDelete = widthCtxMenu->addItem(_("Delete"), [this]() {
    savedWidths.erase(savedWidths.begin() + contextMenuIdx);
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
  widthPopup->addWidget(createTitledRow(_("Width"), spinWidth));
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

  addColorBtn = createToolbutton(
      SvgGui::useFile(":/icons/ic_menu_add_color.svg"), _("Add Color"));
  addColorBtn->onClicked = [this]() {
    Color color = colorPicker->color();
    auto it = std::find(savedColors.begin(), savedColors.end(), color);
    if (it == savedColors.end()) {
      savedColors.push_back(color);
      rebuildGrids();
      it = savedColors.end() - 1;
    }
    colorPopupIdx = int(it - savedColors.begin());
    colorPopupPicker->setColor(*it);
    openAutoClosePopup(colorPopup);
  };

  settingsBtn = createToolSettingsButton(
      "Pen Settings", {"inputSmoothing", "inputSimplify", "applyPenToSel", "savePenMode"},
      compact ? std::vector<Button *>{cbSnaptoGrid, cbLineDrawing} : std::vector<Button *>{});

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
}

void PenToolbar::rebuildGrids() {
  closeAutoClosePopup(colorPopup);
  colorPopupIdx = -1;
  colorPalette->clear();
  widthPalette->clear();

  for (size_t ii = 0; ii < savedColors.size(); ++ii) {
    Color color = savedColors[ii];
    Button *btn =
        compact ? new Button(compactColorBtnNode->clone()) : createColorBtn();
    btn->selectFirst(".btn-color")->node->setAttr<color_t>("fill", color.color);
    // setting on <g class=toolbutton gets overridden by toolbutton CSS fill -
    // do we need class=toolbutton?
    btn->onClicked = [this, ii]() { selectColor(int(ii)); };

    SvgGui::setupRightClick(btn, [this, ii](SvgGui *gui, Widget *w, Point p) {
      contextMenuIdx = ii;
      gui->showContextMenu(colorCtxMenu, p, w);
    });
    setupTooltip(btn, altTooltip(colorToHex(color).c_str(), _("Edit")));
    colorPalette->addButton(btn);
    if (compact)
      btn->setMargins(
          0); // the swatch cell already includes the mockup's spacing
  }
  colorMenuDelete->setEnabled(savedColors.size() > 1);

  for (size_t ii = 0; ii < savedWidths.size(); ++ii) {
    Dim width = savedWidths[ii];
    Button *btn =
        new Button((compact ? compactWidthBtnNode : widthBtnNode)->clone());
    Dim sc = std::min(penWidthPreviewMax,
                      width); // * preScale/ScribbleApp::gui->globalScale);
    if (compact)
      sc *= floatUIScale;  // the compact swatch cell is scaled too
    // btn->containerNode()->selectFirst(".width-circle")->setTransform(Transform2D::scaling(sc));
    btn->containerNode()
        ->selectFirst(".width-line")
        ->setAttr("stroke-width", sc);
    btn->onClicked = [this, ii]() { selectWidth(int(ii)); };

    SvgGui::setupRightClick(btn, [this, ii](SvgGui *gui, Widget *w, Point p) {
      contextMenuIdx = ii;
      gui->showContextMenu(widthCtxMenu, p, w);
    });
    setupTooltip(btn, altTooltip(fstring(_("Pen width: %.1f"), width).c_str(),
                                 _("Edit")));
    widthPalette->addButton(btn);
    if (compact)
      btn->setMargins(0);
  }
  widthMenuDelete->setEnabled(savedWidths.size() > 1);
  updateSelected();
}

// tapping the already selected preset opens the detail editor for it
void PenToolbar::selectWidth(int idx) {
  if (savedWidths[idx] == pen.width) {
    widthPopupIdx = idx;
    if (widthPopup->isVisible())
      closeAutoClosePopup(widthPopup);
    else
      openAutoClosePopup(widthPopup);
    return;
  }
  closeAutoClosePopup(widthPopup);
  widthPopupIdx = idx;
  spinWidth->setValue(savedWidths[idx]);
  updateWidth();
}

// tapping the already selected swatch opens the detail editor for it
void PenToolbar::selectColor(int idx) {
  if (savedColors[idx] == pen.color) {
    colorPopupIdx = idx;
    colorPopupPicker->setColor(savedColors[idx]);
    if (colorPopup->isVisible())
      closeAutoClosePopup(colorPopup);
    else
      openAutoClosePopup(colorPopup);
    return;
  }
  closeAutoClosePopup(colorPopup);
  colorPicker->setColor(savedColors[idx]);
  updateColor();
}

void PenToolbar::updateSelected() {
  for (size_t ii = 0;
       ii < colorPalette->items.size() && ii < savedColors.size(); ++ii) {
    bool sel = savedColors[ii] == pen.color;
    colorPalette->items[ii]->setChecked(sel);
    // a circular swatch has no background to tint, so mark the selected one
    // with a ring instead
    Widget *ring = colorPalette->items[ii]->selectFirst(".sel-ring");
    if (ring)
      ring->setVisible(sel);
  }
  for (size_t ii = 0;
       ii < widthPalette->items.size() && ii < savedWidths.size(); ++ii)
    widthPalette->items[ii]->setChecked(savedWidths[ii] == pen.width);
}

void PenToolbar::saveConfig(ScribbleConfig *cfg) const {
  std::vector<std::string> colorStrs;
  for (Color c : savedColors)
    colorStrs.emplace_back(
        fstring("#%02X%02X%02X%02X", c.alpha(), c.red(), c.green(), c.blue()));
  cfg->set("savedColors", joinStr(colorStrs, ",").c_str());

  std::vector<std::string> widthStrs;
  for (Dim w : savedWidths)
    widthStrs.push_back(fstring("%.3f", w));
  cfg->set("savedWidths", joinStr(widthStrs, ",").c_str());
}

void PenToolbar::setPen(const ScribblePen &newpen, Mode m) {
  mode = m;
  widthGroup->setEnabled(mode != BOOKMARK_MODE);
  overflowBtn->setEnabled(mode == PEN_MODE);
  overflowBtn->setVisible(!compact && mode != SELECTION_MODE);
  selOverflowBtn->setVisible(mode == SELECTION_MODE);
  settingsBtn->setVisible(mode == PEN_MODE);

  if (newpen == pen)
    return;
  pen = newpen;
  closeAutoClosePopup(widthPopup);
  closeAutoClosePopup(colorPopup);
  colorPicker->setColor(pen.color);
  spinWidth->setValue(pen.width);
  cbSnaptoGrid->setChecked(pen.hasFlag(ScribblePen::SNAP_TO_GRID));
  cbLineDrawing->setChecked(pen.hasFlag(ScribblePen::LINE_DRAWING));
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
  updateSelected();
  if (onChanged)
    onChanged(COLOR_CHANGED | (changesSinceFocused > 0 ? UNDO_PREV : 0));
  if (changesSinceFocused >= 0)
    ++changesSinceFocused;
}

void PenToolbar::updateWidth() {
  pen.width = spinWidth->value();
  // editing the width while the detail popup is open redefines that preset
  if (widthPopup->isVisible() && widthPopupIdx >= 0 &&
      widthPopupIdx < int(savedWidths.size())) {
    savedWidths[widthPopupIdx] = pen.width;
    Dim sc = std::min(penWidthPreviewMax, pen.width);
    if (compact)
      sc *= floatUIScale;
    widthPalette->items[widthPopupIdx]
        ->containerNode()
        ->selectFirst(".width-line")
        ->setAttr("stroke-width", sc);
  }
  updateSelected();

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
