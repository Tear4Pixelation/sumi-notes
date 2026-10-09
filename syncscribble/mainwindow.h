#pragma once

#include <map>

#include "ugui/widgets.h"
#include "rulingregion.h"
#include "shape.h"

class ScribbleApp;
struct UIState;
class RegionPanel;

class ScribbleWidget;
class ScribbleView;
class ScribbleArea;
class ScribbleDoc;
class OverlayWidget;
class Sidebar;
class AutoAdjContainer;

class MainWindow : public Window
{
public:
  MainWindow(SvgDocument* n) : Window(n) {}
  ~MainWindow();
  void setupUI(ScribbleApp* a);
  void setupActions();
  void createToolBars();
  Action* modeToAction(int mode);
  void updateMode();
  void refreshScribbleWidget(ScribbleWidget* w, const UIState* uiState);
  void refreshCommonUI(ScribbleDoc* doc, const UIState* uiState);
  // sync the toolbar sidebar button's glyph (which edge) and checked state (open) with the sidebar
  void updateSidebarButton();
  // Editor tabs: highlight the half of `area` a dropped tab would open a new pane in (edge is a DropEdge,
  //  tablist.h), or the whole pane for edge -1; area NULL hides it
  void showTabDropPreview(ScribbleArea* area, int edge);
  void refreshUI(ScribbleDoc* doc, int reason);
  void orientationChanged();
  bool oneTimeTip(const char* id, Point pos = {}, const char* message = NULL);

  void selectTool(int modeType);
  void selectDrawTool(int tool);
  void selectShape(int shapeid);
  void setShapeOptions();
  void showOptionsRow(int modeType);
  void setEraserMode();
  void setEraserWidth(int idx);

  void togglePenToolbar();
  void toggleFullscreen();
  void toggleBookmarks();
  void toggleDisableTouch();
  void toggleSyncView();
  void toggleSyncViewMaster();
  void toggleSplitView(int newstate);
  void toggleInvertColors();

  Action* findAction(const char* name);

  //static ScribbleWidget* createScribbleWidget(Widget* container, ScribbleView* area);
  ScribbleWidget* createScribbleAreaWidget(Widget* container, ScribbleArea* area);
  Widget* createSplitPlaceholder(Widget* container);

  std::vector<Action*> actionList;
  std::unordered_map<std::string, Action*> shortcuts;

  Action* checkedMode = NULL;
  Action* checkedSubMode = NULL;

  Action* action_Previous_Page;
  Action* action_Next_Page;
  Action* actionZoom_In;
  Action* actionZoom_Out;
  Action* actionPan;
  Action* actionNew_Page_Before;
  Action* actionNew_Page_After;
  Action* action_About;
  Action* action_Open;
  Action* actionSave_As;
  Action* actionExit;
  Action* actionSelect_All;
  Action* actionSelect_Similar;
  Action* actionInvert_Selection;
  Action* actionDelete_Selection;
  Action* actionCopy;
  Action* actionPaste;
  Action* actionDupSel;
  Action* actionInsertDocument;
  Action* actionInsert_PDF;
  Action* actionSave;
  Action* actionPage_Setup;
  Action* actionTheme;
  Action* actionShow_Sidebar = NULL;
  // opens the sidebar in its Tabs view for a moment, leaving its own view, pin and open state alone
  Action* actionShow_Tabs = NULL;
  Action* actionTagDocList;
  Action* actionUndo;
  Action* actionRedo;
  Action* actionExpand_Down;
  Action* actionExpand_Right;
  Action* actionCut;
  Action* actionRevert;
  Action* actionReset_Zoom;
  Action* actionNew_Document;
  Action* actionShow_Bookmarks;
  Action* actionSend_Page;
  Action* actionStroke_Eraser;
  Action* actionRuled_Eraser;
  Action* actionRect_Select;
  Action* actionRuled_Select;
  Action* actionInsert_Space_Vert;
  Action* actionRuled_Insert_Space;  // Down: whole lines
  Action* actionRuled_Insert_Space_Right;
  Action* actionCustom_Pen;
  Action* actionAdd_Bookmark;
  Action* actionDraw;
  Action* actionHighlight;
  Action* actionEphemeral;
  Action* actionErase;
  Action* actionSelect;
  Action* actionInsert_Space;
  Action* actionLasso_Select;
  Action* actionPath_Select;
  Action* actionShapes;
  Action* actionShape[SHAPE_COUNT];
  Action* actionRulingRegion;
  // the "+" beside Add Page (AddPageMenu::createAddMenuButton)
  Action* actionAddPaper;
  Action* actionAddPatch;
  Action* actionAddAxesPatch;  // a coordinate system patch (RulingRegionParams::axes)
  Action* actionAddDocument;
  Action* actionAddPhoto;
  std::vector<Action*> addMenuActions() const { return {actionAddPaper, actionAddPatch, actionAddAxesPatch, actionAddDocument, actionAddPhoto}; }
  static const SvgNode* axesPatchIcon();
  Action* actionExport_PDF;
  Action* actionImport_PDF;
  Action* actionPreferences;
  Action* actionCreate_Link;
  Action* actionScreenshot;
  Action* actionUngroup;
  Action* actionUpdateCheck;
  Action* actionFree_Eraser;
  Action* actionPrevious_View;
  Action* actionNext_View;
  Action* actionSend_HTML;
  Action* actionSend_PDF;
  Action* actionInsert_Image;
  Action* actionScan_Element;
  Action* actionScan_Page;
  Action* actionShare_Document;
  Action* actionOpen_Shared_Doc;
  Action* actionSendImmed;
  Action* actionOverflow_Menu;
  Action* actionSelection_Menu;
  Action* actionFullscreen;
  Action* actionSplitView;
  Action* actionSelect_Pages;
  Action* actionDelete_Page;
  Action* actionViewSync;
  Action* actionViewSyncMaster;
  Action* actionBookmarksClose;
  Action* actionBookmarksPin;
  Action* actionDisable_Touch;
  Action* actionSyncInfo;
  Action* actionInvertColors;

  Menu* menuRecent_Files;
  ArrowPopup* overflowPopup;
  ArrowPopup* menuWhiteboard;
  Button* menuWhiteboardBtn;
  Button* undoRedoBtn;
  // unprompted tip on the History button, shown once history exists (see refreshCommonUI); the panel's
  //  own hint only reaches someone who already pressed the button, which is not who needs telling
  ArrowPopup* historyTip = NULL;
  bool historyTipShown = false;
  Button* titleButton;
  Button* iapButton = NULL;  // iOS IAP
  Widget* toolBarStretch;
  Widget* selPopup;
  // "Move to Layer" on the selection popup, and its list of layers, rebuilt each time the popup opens
  Button* moveLayerBtn = NULL;
  // selection popup buttons that act on ink, hidden when the gesture caught none (Screenshot remains)
  std::vector<Button*> selInkButtons;
  ArrowPopup* moveLayerPopup = NULL;
  // color and width on the selection popup, shown only for an ink selection (PenToolbar::SELECTION_MODE)
  Widget* selColorItem = NULL;
  Widget* selWidthItem = NULL;
  void refreshSelPopup();
  AutoAdjContainer* penToolbarAutoAdj;
  Widget* eraseOptsRow;
  Widget* selectOptsRow;
  Widget* insSpaceOptsRow;
  Widget* shapeOptsRow;
  // floating column of panels beside the selected ruling region (see buildRegionPanel)
  RegionPanel* regionPanel = NULL;
  Button* regionKindBtn = NULL;
  Slider* regionSpacingSlider = NULL;
  TextBox* regionSpacingText = NULL;
  Button* regionPaperToggle = NULL;
  // the checkboxes inside the Background and Outline buttons
  SvgNode* regionPaperCheck = NULL;
  SvgNode* regionOutlineCheck = NULL;
  Button* regionStraightenBtn = NULL;
  // parameters from before a spacing slider drag, which is previewed and committed as one step on release
  std::unique_ptr<RulingRegionParams> regionSlideStart;
  Point regionSlideHandle;  // what the drag scales about: the red handle when it began
  void buildRegionPanel();
  void syncRegionRow();
  void editSelRegion(const std::function<void(RulingRegionParams&)>& change);
  // "Place your tag" under the toolbar while page tags ride on the pointer (docs/agent/page-tags.md)
  Toolbar* tagPlaceHint = NULL;
  TextBox* tagPlaceHintText = NULL;
  void syncTagPlaceHint();
  Button* shapeHeadStartToggle = NULL;
  Button* shapeHeadEndToggle = NULL;
  Button* shapeRoundedToggle = NULL;
  // the tools row, the divider and the open options row are children of one floating panel with a
  //  single (fully rounded) background, so showing/hiding an options row is just a visibility change
  Widget* mainToolbarPanel = NULL;
  Widget* optsRowDivider = NULL;
  // horizontally scrolling viewport holding the four options rows (only one is ever visible)
  Widget* optsRowContainer = NULL;
  std::vector<Button*> eraserWidthBtns;
  Button* eraseStrokeToggle;
  Button* eraseRuledToggle;
  Button* eraseSwitchBackToggle;
  Button* selectSwitchBackToggle;
  Button* selectTouchingToggle;
  Button* insSpaceSwitchBackToggle;
  Button* insSpaceSkipLinesToggle;
  // mode type of the options row currently shown, 0 if none
  int openOptionsRow = 0;
  std::vector<Widget*> tbWidgets;
  // when auto-adjust hides a toolbar widget, the menu items mirroring its action(s) are shown at the top
  //  of the overflow menu, so nothing becomes unreachable on a narrow window
  std::map<Widget*, std::vector<Button*>> tbOverflowItems;
  Widget* overflowHiddenGroup = NULL;
  Widget* overflowHiddenSep = NULL;
  std::string titleStr;

  const SvgNode* appIcon;
  const SvgNode* drawIcon;
  const SvgNode* nextPageIcon;
  const SvgNode* appendPageIcon;
  const SvgNode* editBoxIcon;
  const SvgNode* swbIcon;

  Splitter* bookmarkSplitter;
  Widget* bookmarkPanel;
  OverlayWidget* overlayWidget;
  // the outline/layers sidebar (sidebar.h); owned by the widget tree, not by MainWindow
  Sidebar* sidebar = NULL;

  Widget* scribbleContainer2 = NULL;
  Widget* scribbleFocusContainer2 = NULL;
  Splitter* scribbleSplitter = NULL;
  Widget* focusIndicator2 = NULL;
  Widget* splitPlaceholder = NULL;
  // what a tab dragged onto the canvas would do, one per pane (showTabDropPreview)
  Widget* tabDropPreview[2] = {NULL, NULL};
  ScribbleArea* tabDropPreviewArea = NULL;
  int tabDropPreviewEdge = -1;
  //enum {SplitNone, SplitHorz, SplitVert, SplitNumStates};
  // splitState = +/- SPLIT_XXX : > 0 if open, < 0 if not; toggle just flips sign
  enum SplitState { SPLIT_TOGGLE=0, SPLIT_H12, SPLIT_H21, SPLIT_V12, SPLIT_V21 };
  int splitState = -SPLIT_H12;
  Point scribbleAreaStatusInset = {6, 6};
  bool vertToolbar = false;  // probably will have to become toolbarPos = top/left/right/bottom

private:
  ScribbleApp* app;

  void setupTheme();
  void setupHelpTips();
  Action* createAction(const char* name,
      const char* title, const char* iconfile, const char* shortcut, const std::function<void()>& callback = NULL);
  Menu* createMenu(const char* name, const char* title, Menu::Align = Menu::VERT_RIGHT, bool showicons = true); //Widget* parent = 0);
};

MainWindow* createMainWindow();
