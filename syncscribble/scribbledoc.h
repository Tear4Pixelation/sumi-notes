#ifndef SCRIBBLEDOC_H
#define SCRIBBLEDOC_H

#include "scribblearea.h"

class ScribbleApp;
class ScribbleMode;
class ScribbleSync;
class StrokeBuilder;

class ScribbleDoc
{
#ifdef SCRIBBLE_TEST
  friend class ScribbleTest;
#endif
  friend class UndoHistory; // for syncing; hopefully only temporary
  friend class LinkDialog;
  friend class ScribbleArea;
public:
  ScribbleDoc(ScribbleApp* parent, ScribbleConfig* _cfg, ScribbleMode* _mode);
  ~ScribbleDoc();

  void addArea(ScribbleArea* area);
  void removeArea(ScribbleArea* area);
  void loadConfig(bool refresh);
  //void doTimerEvent(Timestamp t);
  void newDocument();
  Document::loadresult_t openDocument(const char* filename, bool delayload = true);
  Document::loadresult_t openDocument(IOStream* strm, bool delayload = true);
  bool saveDocument(const char* filename, Document::saveflags_t flags = Document::SAVE_NORMAL);
  bool saveDocument(IOStream* strm = NULL, Document::saveflags_t flags = Document::SAVE_NORMAL);
  void resetDocPrefs();
  void openSharedDoc(const char* server, const pugi::xml_node& xml, bool master);
  bool checkAndClearErrors(bool forceload = false);

  bool isModified() const { return document->isModified(); }
  bool canRedo() const;
  bool canUndo() const;

  void doCommand(int itemid);
  bool doesClipStrokes(const PageProperties* props, bool applytoall);
  bool setPageProperties(const PageProperties* props, bool applytoall, bool docdefault, bool global, bool undoable = true);
  void openURL(const char* url);
  void bookmarkHit(int pagenum, Element* bookmark);

  // a lot of stuff needs to be moved up from ScribbleDoc to application level
  int getScribbleMode(int modemod);
  int getActiveMode() const;

  // The document's themed palette (COLORS_SPEC.md), generated from cfg's recipe and cached - the
  //  generator is far too expensive to run per frame, and nothing in the drawing or input path calls it.
  const Palette& palette();
  void invalidatePalette() { paletteValid = false; }
  // False once the palette has been generated from a generator id this build does not have: the
  //  document still renders correctly from its literal stroke colors, but its palette is a fallback.
  bool isThemeGenKnown() const { return themeGenKnown; }
  // applyPages writes the theme's paper and rule colors onto the document's pages, undoably;
  //  globalDefault additionally makes this the recipe new documents get.
  // ownAction false means the caller has already opened an undo action and this must not open its
  //  own - restyleToTheme() needs the page recolor and the stroke recolor to be one Ctrl+Z.
  void setTheme(const PaletteRecipe& recipe, bool applyPages, bool globalDefault, bool ownAction = true);
  // Phase 5: change the theme *and* rewrite every stroke drawn in the old palette to the matching
  //  color in the new one, as a single undoable action.  Returns the number of strokes changed.
  //  Strokes that are not the old palette's colors are left untouched.
  int restyleToTheme(const PaletteRecipe& recipe, bool applyPages, bool globalDefault);

  Color getCurrPageColor() const;
  void scribbleDone();
  void pageSizeChanged();
  void pageCountChanged(int pagenum, int prevpages = -1);
  void startAction(int pagenum);
  void endAction();
  void clearSelection();
  void deleteSelection();
  Rect dirtyPage(int pagenum);
  void updateCurrStroke(Rect dirty);

  // some support fns for whiteboarding
  void strokesUpdated(const std::vector<Element*>& strokes);
  void invalidateStroke(Element* s) { for(ScribbleArea* view : views) view->invalidateStroke(s); }
  // commit any in-progress multi-point shape in every view (called when the tool changes)
  void finishShapes() { for(ScribbleArea* view : views) view->finishShape(); }
  bool setSelShapeOptions(int flags, Dim radius)
      { return activeArea && activeArea->setSelShapeOptions(flags, radius); }
  void invalidatePage(Page* p) { for(ScribbleArea* view : views) view->invalidatePage(p); }

  static bool deleteDocument(const char* filename);
  static Image extractThumbnail(const char* filename);
  Document::loadresult_t insertDocument(IOStream* strm);
  const char* fileName() const { return document->fileName(); }

//private:
  // props, when given, overrides both the preceding page's custom ruling and the document defaults -
  //  this is how the add-page menu inserts a page with a ruling chosen for that page alone
  Page* generatePage(int where, const PageProperties* props = NULL) const;
  void updateGhostPage();
  void newPage(int where = INT_MAX, const PageProperties* props = NULL);
  void insertPage(Page* page, int where = INT_MAX);
  void deletePages();
  void deletePage(int where);
  void pastePages(Clipboard* clipboard, int where);
  void movePage(int oldpagenum, int newpagenum);
  enum SelectPagesFlags { PAGESEL_INV=INT_MAX-1, PAGESEL_ALL=INT_MAX };
  void selectPages(int pagenum);
  void exitPageSelMode();

  void uiChanged(int reason);
  void repaintAll();
  void doRefresh();

  void setDefaultDims(ScribbleConfig* c, const PageProperties* props);
  void doUndoRedo(bool redo = false);
  void undoRedoUpdate(int pagenum, int prevpages, bool viewdirty = true);
  void closeDocument();
  void doCancelAction();
  void updateDocConfig(Document::saveflags_t flags);

  ScribbleApp* app;
  ScribbleMode* scribbleMode;
  ScribbleConfig* globalCfg;
  ScribbleConfig* cfg;
  ScribbleArea* activeArea = NULL;
  ScribbleSync* scribbleSync = NULL;
  unsigned int nViews = 0;
  int uiDirty = 0;
  int strokeCounter;
  int numSelPages = 0;
  int prevSelPageNum = 0;

  std::vector<ScribbleArea*> views;
  Document* document = NULL;
  UndoHistory* history = NULL;
  std::unique_ptr<Page> ghostPage;
  Timestamp fileLastMod = 0;
  bool autoSaveReq = false;
  Palette themePalette;
  bool paletteValid = false;
  bool themeGenKnown = true;

  StrokeBuilder* strokeBuilder = NULL;
};

#endif

