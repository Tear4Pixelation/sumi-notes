#pragma once

#include "ugui/widgets.h"
#include "basics.h"
#include "scribbleconfig.h"
#include "scribblepen.h"
#include <memory>


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
  void refreshDisplayColors();
  void updateColor();
  void updateWidth();
  void updatePen();
  // selDashStyle is the selection's line style in SELECTION_MODE (a pen carries its own, as dash/gap)
  void setPen(const ScribblePen& newpen, Mode m, int selDashStyle = ScribblePen::DASH_SOLID);
  // ScribblePen::DASH_*: solid, dashed or dotted.  In SELECTION_MODE it is the selection's (DASH_MIXED
  //  when that has several) and a change is reported as DASH_CHANGED; otherwise it is the pen's.
  int dashStyle = ScribblePen::DASH_SOLID;
  void setDashStyle(int style);
  //void dragWidth(int delta);

  enum ChangedFlag { COLOR_CHANGED=1, WIDTH_CHANGED=2, PEN_CHANGED=4, YIELD_FOCUS=8, DASH_CHANGED=0x20,
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

  // A lone color swatch for a row with no room for the saved list - the shape tool's options row and
  //  the selection popup.  It shows the pen's color and opens the same theme grid as the "+"; a pick
  //  sets the pen's color (or the selection's, in SELECTION_MODE) instead of adding a swatch.  Returns
  //  the swatch plus its popups, which have to sit beside it in the tree.  onPicked is called after a
  //  pick with the color as snapped to the theme.
  Widget* createSingleSwatch(std::function<void(Color)> onPicked = nullptr);
  // Its thickness counterpart: one width swatch showing the pen's (or the selection's) width and line
  //  style, opening a popup with the width presets, the width itself and solid/dashed/dotted.
  //  onWidthChanged is called after the width or the style is changed through it.
  Widget* createSingleWidth(std::function<void()> onWidthChanged = nullptr);

private:
  void fillColorGrid(Widget* grid, const std::function<void(Color)>& onPick,
      const std::function<void()>& onCustom);
  struct SingleSwatch {
    Button* btn;
    ArrowPopup* palettePopup;
    Widget* paletteGrid;
    ArrowPopup* customPopup;
    ColorEditBox* customPicker;
    std::function<void(Color)> onPicked;
  };
  struct SingleWidth {
    Button* btn;
    ArrowPopup* popup;
    Widget* presetRow;
    std::vector<Button*> presets;
    SpinBox* spin;
    Widget* spinRow;
    Button* dashBtns[3];
    std::function<void()> onChanged;
  };
  std::vector<std::unique_ptr<SingleSwatch>> singleSwatches;
  std::vector<std::unique_ptr<SingleWidth>> singleWidths;
  void openSingleSwatch(SingleSwatch* sw);
  void pickSingleSwatch(SingleSwatch* sw, Color color);
  void openSingleWidth(SingleWidth* sw);
  void setSingleWidth(SingleWidth* sw, Dim width);
  void updateSingles();
  // counts edits since a text field took focus, so typing a width is one undo step, not one per key
  void trackEditFocus(Widget* field);
  void rebuildGrids();
  void selectWidth(int idx);
  void selectColor(int idx);
  // Opens the theme's color grid.  editIdx < 0 is the "+" route: a pick is appended to the row.
  //  editIdx >= 0 edits that swatch - a pick replaces it in place, and the popup grows a header row
  //  with cancel on the left and delete on the right, so a swatch added by mistake can be taken back.
  void openPaletteGrid(int editIdx);
  // appends `color` to the row if it is not already there, then selects it; returns its index
  int addSwatchColor(Color color);
  // the hex/slider editor, on a new swatch (editIdx < 0) or on an existing one
  void openCustomColor(int editIdx);
  void setSwatchColor(int idx, Color color);
  // The color a swatch actually gives the pen in hand: the saved list holds the theme's ink colors,
  //  but the marker draws translucent, so with it selected every swatch is offered as that family's
  //  highlighter variant instead.  Identity for every other tool, and for an unthemed document.
  Color toolColor(Color c) const;
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
  // marker only: draw along the centre of the ruled line (ScribblePen::CENTER_ON_LINE)
  Button* centerLineToggle;
  // the pen tip picks the stroke builder: flat extrudes a quad per input segment, round sweeps a disc,
  //  chisel a fixed-aspect nib.  A flag on the pen rather than a pref, so it lives in Pen Settings as
  //  an extra row rather than coming from prefNames.
  ComboBox* comboPenTip;
  Widget* penTipRow;
  Widget* rulingPreview;
  Button* addColorBtn;
  // the + opens this: a grid of the theme's own colors, with a trailing "custom color" button.  The
  //  theme's answer is offered first; a color from outside it takes one more deliberate step.
  ArrowPopup* palettePopup;
  Widget* paletteGrid;
  // cancel/delete row, shown only while the grid is editing an existing swatch
  Widget* paletteHeader;
  Button* paletteDeleteBtn;
  // which swatch the open grid is editing, or -1 when it is adding one
  int paletteEditIdx = -1;
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
