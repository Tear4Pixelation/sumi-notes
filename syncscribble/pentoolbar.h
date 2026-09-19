#pragma once

#include "ugui/widgets.h"
#include "basics.h"
#include "scribbleconfig.h"


class ScribbleApp;
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

  Button* cbSnaptoGrid;
  Button* cbLineDrawing;
  //Button* comboPressure;
  SpinBox* spinWidth;
  Button* addColorBtn;
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
  std::vector<Dim> savedWidths;
  static std::unique_ptr<SvgNode> widthBtnNode;
  static std::unique_ptr<SvgNode> compactWidthBtnNode;
  static std::unique_ptr<SvgNode> compactColorBtnNode;
  static const Dim PEN_WIDTHS[];
};

class AutoAdjContainer;
AutoAdjContainer* createPenToolbarAutoAdj(bool compact = false);
