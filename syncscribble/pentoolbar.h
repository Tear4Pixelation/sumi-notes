#pragma once

#include "ugui/widgets.h"
#include "basics.h"
#include "scribbleconfig.h"


class ScribbleApp;
class Page;
class ColorEditBox;
class PenPreview;

// idea here is to abstract a container for a group of widgets that handles overflow, currently w/ an overflow
//  menu, but scrolling could be another option
class PaletteWidget : public Widget
{
public:
  //PaletteWidget(SvgNode* n, Widget* main, Widget* overflow, Widget* btn);
  PaletteWidget(SvgNode* n) : Widget(n) {}
  // we could add insert and delete methods in the future if needed
  void addButton(Button* w);
  void setNumVisible(int n);
  void clear();

  std::vector<Button*> items;
  Widget* mainGroup = NULL;
  Widget* overflowGroup = NULL;
  Widget* overflowBtn = NULL;
  int numMainGroup;
};

class PenToolbar : public Toolbar
{
public:
  // compact: the desktop floating panel's draw options row, which follows the design mockup - saved
  //  colors as circles, thickness presets as bare lines, and none of the hex box/overflow/help/close
  //  buttons that the roomier phone and vertical-toolbar layouts show
  PenToolbar(bool compact = false);

  ScribblePen pen;
  enum Mode { PEN_MODE, BOOKMARK_MODE, SELECTION_MODE } mode = PEN_MODE;

  void saveConfig(ScribbleConfig* cfg) const;
  // Repopulates the swatches from the active document's theme (COLORS_SPEC.md §6). Called when the
  //  theme changes and when the document changes; harmless when theming is off.
  void refreshPalette();
  void updateColor();
  void updateWidth();
  void updatePen();
  void setPen(const ScribblePen& newpen, Mode m);
  //void dragWidth(int delta);

  enum ChangedFlag { COLOR_CHANGED=1, WIDTH_CHANGED=2, PEN_CHANGED=4, YIELD_FOCUS=8,
      UNDO_PREV=0x10000 };
  std::function<void(int)> onChanged;

  // access needded for auto adjust
  PaletteWidget* colorPalette;
  PaletteWidget* widthPalette;
  Widget* stretch;
  Button* closeBtn;
  ColorEditBox* colorPicker;  // secondary/advanced control - first thing dropped when space runs out

  const Dim penWidthPreviewMax = 22;  // was 30 for circle instead of line; static constexpr only works for int
  const bool compact;

private:
  void rebuildGrids();
  void selectWidth(int idx);
  void selectColor(int idx);
  void updateSelected();
  // any pen's thickness can be expressed as a multiple of the line height (the relative size toggle in
  //  the width popup); it is on by default for the text marker only, which is the tool whose job is to
  //  cover a line of text.  Each draw tool carries its own presets, in its own unit.
  bool relativeWidths() const;
  void setRelativeWidth(bool relative);
  void updateWidthPopup();
  // ScribbleMode::DRAWTOOL_* whose presets belong on the row for the given toolbar mode
  int drawToolForMode(Mode m) const;
  std::vector<Dim>& activeWidths();
  const std::vector<Dim>& activeWidths() const;
  // fills the active preset list if it is empty, in the active pen's unit; true if it did
  bool seedWidths();
  // line height of the page being drawn on; the unit relative widths are measured in
  Page* currentPage() const;
  Dim lineHeight() const;
  // preview line thickness for a preset, in the swatch's units
  Dim widthPreview(Dim w) const;

  Button* cbSnaptoGrid;
  Button* cbLineDrawing;
  //Button* comboPressure;
  SpinBox* spinWidth;
  Widget* widthSpinRow;
  CheckBox* cbRelWidth;
  Widget* relWidthRow;
  Widget* rulingPreview;
  Button* addColorBtn;
  // the + opens this: a grid of the theme's own colors, with a trailing "custom color" button.  The
  //  theme's answer is offered first; a color from outside it takes one more deliberate step.
  ArrowPopup* palettePopup;
  Widget* paletteGrid;
  // true while a document theme is supplying the colors on offer
  bool themed = false;
  Button* settingsBtn;
  ArrowPopup* widthPopup;
  int widthPopupIdx = -1;
  ArrowPopup* colorPopup;
  ColorEditBox* colorPopupPicker;
  int colorPopupIdx = -1;
  Widget* colorGroup;
  Widget* widthGroup;

  Button* overflowBtn;
  Button* selOverflowBtn;
  Menu* colorCtxMenu;
  Menu* widthCtxMenu;
  Button* colorMenuDelete;
  Button* widthMenuDelete;
  Dim preScale;

  int changesSinceFocused = -1;
  int contextMenuIdx;
  std::vector<Color> savedColors;
  // One preset list per draw tool, each held in that tool's own unit - absolute, or multiples of the
  //  line height while that tool's relative size toggle is on (see setRelativeWidth).  They cannot
  //  share a list: two tools can be in different units at the same time, and one list would then be
  //  read as the wrong one by whichever tool is not holding it.
  std::vector<Dim> savedWidths;           // DRAWTOOL_PEN, and every non-pen toolbar mode
  std::vector<Dim> savedMarkerWidths;     // DRAWTOOL_HIGHLIGHT
  std::vector<Dim> savedEphemeralWidths;  // DRAWTOOL_EPHEMERAL
  // which of those lists is currently on the row
  int widthsTool = 0;
  static std::unique_ptr<SvgNode> widthBtnNode;
  static std::unique_ptr<SvgNode> compactWidthBtnNode;
  static std::unique_ptr<SvgNode> compactColorBtnNode;
  static const Dim PEN_WIDTHS[];
};

class AutoAdjContainer;
AutoAdjContainer* createPenToolbarAutoAdj(bool compact = false);
