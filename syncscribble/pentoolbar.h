#pragma once

#include "ugui/widgets.h"
#include "basics.h"
#include "scribbleconfig.h"
#include "scribblepen.h"
#include "widthpresets.h"
#include "colorref.h"
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
  // refreshPalette() if the active document's palette is not the one the row was resolved against;
  //  true if it was not
  bool syncPalette();
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
  //  onWidthChanged is called after the width or the style is changed through it.  selectionOnly: the
  //  selection popup's, which edits a selection and never the pen (pen-and-tools.md, "Pen width is
  //  written in one place") - with the toolbar on the pen it does nothing.
  Widget* createSingleWidth(std::function<void()> onWidthChanged = nullptr, bool selectionOnly = false);
  // line height relative widths are measured in; in SELECTION_MODE that of the Paper Patch holding most of
  //  the selection (Page::lineHeightFor), else the page's
  Dim lineHeight() const;

private:
  friend class ScribbleTest;  // penWidthPresetTest() drives the preset row directly
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
    bool selectionOnly = false;
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
  WidthPresets& activePresets();
  const WidthPresets& activePresets() const;
  // the raw numbers of the active list, in *its* unit (activePresets().unit) - which is the pen's in
  //  PEN_MODE, but not for a selection while the pen is relative; use presetWidth() to compare or apply
  std::vector<Dim>& activeWidths();
  const std::vector<Dim>& activeWidths() const;
  // preset idx in the unit of `pen` (absolute for a selection, whatever the draw tool's list is in)
  Dim presetWidth(size_t idx) const;
  // makes the active list usable: resolves a legacy list's unit, converts it to the pen's unit in
  //  PEN_MODE, seeds an empty one and keeps it at WidthPresets::COUNT in-range presets; true if changed
  bool prepareWidths();
  // line height of the page being drawn on; the unit relative widths are measured in
  Page* currentPage() const;
  // preview line thickness for a preset, in the swatch's units
  Dim widthPreview(Dim w) const;

  Button* cbSnaptoGrid;
  Button* cbLineDrawing;
  // how much thinner a light touch draws than a full press, in percent (ScribblePen::pressureSensitivity)
  SpinBox* spinPressure;
  Widget* pressureRow;
  SpinBox* spinWidth;
  Widget* widthSpinRow;
  CheckBox* cbRelWidth;
  Widget* relWidthRow;
  // marker only: draw along the centre of the ruled line (ScribblePen::CENTER_ON_LINE)
  Button* centerLineToggle;
  // long press / right click on the toggle: nudge the centring up or down (ScribblePen::centerOffset)
  ArrowPopup* centerOffsetPopup;
  Slider* centerOffsetSlider;
  SpinBox* spinCenterOffset;
  void setCenterOffset(Dim offset, bool fromSlider = false);
  void showCenterOffset(Dim offset, bool keepSlider = false);
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
  Menu* colorCtxMenu;
  Button* colorMenuDelete;
  Dim preScale;

  int changesSinceFocused = -1;
  int contextMenuIdx;
  // The swatch row (docs/agent/pen-storage.md).  Each is a theme slot (family + variant) or a literal
  //  colour, so the same row shows each document theme's own colours in the same places; swatchColor()
  //  resolves one in the active document's palette.  Saved as `savedSwatches`; `savedColors` (literal
  //  colours, every build before this) is only read, once, when there is no `savedSwatches` yet.
  std::vector<ColorRef> swatches;
  // the row came from legacy `savedColors`: the first themed refresh may still seed the theme's own row
  //  over it (the old black/red/green/blue default), and then never again - after that the row is the
  //  user's and nothing replaces it
  bool swatchesLegacy = false;
  Color swatchColor(size_t idx) const;
  // the recipe the row was last resolved against, so a change of active palette (a tab switch, which
  //  does not run ScribbleDoc::loadConfig()) re-resolves it; see syncPalette()
  PaletteRecipe swatchRecipe;
  bool swatchRecipeValid = false;
  // One preset list per draw tool, each held in that tool's own unit - absolute, or multiples of the
  //  line height while that tool's relative size toggle is on (see setRelativeWidth).  They cannot
  //  share a list: two tools can be in different units at the same time, and one list would then be
  //  read as the wrong one by whichever tool is not holding it.
  //  Each list also carries its unit, since a selection shows the pen's list while being absolute.
  WidthPresets savedWidths;           // DRAWTOOL_PEN, and every non-pen toolbar mode
  WidthPresets savedMarkerWidths;     // DRAWTOOL_HIGHLIGHT
  WidthPresets savedEphemeralWidths;  // DRAWTOOL_EPHEMERAL
  WidthPresets savedTapeWidths;       // DRAWTOOL_TAPE
  // which of those lists is currently on the row
  int widthsTool = 0;
  // both renderings of the active tool's pen width and presets while the Relative size toggle is flipped
  //  with no edit in between, so flipping it back is exact (setRelativeWidth)
  struct RelToggle {
    bool valid = false;
    int tool = 0;
    struct Rendering { Dim pen = 0; std::vector<Dim> presets; } absolute, relative;
  } relToggle;
  // What setPen()'s full refresh last drew besides the pen itself, so an unchanged pen can skip it only
  //  when none of these moved either (see setPen)
  struct ShownContext {
    const void* doc = NULL;  // the swatches' palette follows the active document
    Mode mode = PEN_MODE;
    Color pageColor, ruleColor;  // the width display draws the current page's ruling
    Dim lineHeight = 0;  // relative widths, presets in the other unit, the display's stroke
    bool operator==(const ShownContext& b) const { return doc == b.doc && mode == b.mode
        && pageColor == b.pageColor && ruleColor == b.ruleColor && lineHeight == b.lineHeight; }
  };
  ShownContext shownContext;
  bool shownValid = false;
  // set while setPen() swaps the pen in: closing the width popup there hands the spin box a focus-lost
  //  commit of whatever was typed, which would land on the pen being switched *to* (updateWidth())
  bool settingPen = false;
  // whether a width the spin box reports is the pen's own as the box shows it ("%.3g"): Return without an
  //  edit re-parses the rounded text, and that must not round the pen
  bool showsPenWidth(Dim w) const;
  void trackScrub(SpinBox *spin);
  int singlesDashStyle = -2;  // the line style updateSingles() last drew (-2: never)
  static std::unique_ptr<SvgNode> widthBtnNode;
  static std::unique_ptr<SvgNode> compactWidthBtnNode;
  static std::unique_ptr<SvgNode> compactColorBtnNode;
  static const Dim PEN_WIDTHS[];
};

class AutoAdjContainer;
AutoAdjContainer* createPenToolbarAutoAdj(bool compact = false);
