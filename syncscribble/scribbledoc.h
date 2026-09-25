#ifndef SCRIBBLEDOC_H
#define SCRIBBLEDOC_H

#include "scribblearea.h"
#include "syncundo.h"
#include <functional>

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
  // Set or clear a page's outline (table of contents) entry, as one undoable, syncable action.
  // A NULL or empty title removes the entry.  Returns false if pagenum is out of range or the
  // entry is already exactly this, so a no-op cannot push an empty step onto the undo history.
  bool setPageOutline(int pagenum, const char* title, int level = 0);
  std::vector<OutlineEntry> outline(bool loadpages = true) { return document->outline(loadpages); }
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

  // the theme as undo and sync see it, and applying one (ThemeChangedItem)
  ThemeState themeState() const;
  void applyThemeState(const ThemeState& state);

  // Layers (LAYERS_INVESTIGATION.md).  Three kinds of state:
  //  - the *table* (names, order, locked, hidden) is persisted in the per-document config, exactly
  //    as the theme recipe is, so it needs no file-format construct.  Every edit to it is a
  //    LayerTableItem, one per layer touched, so it undoes and syncs.
  //  - which layer is *current* is also in the config but is neither undone nor synced: it is where
  //    this user's pen is, and two people on a whiteboard draw on different layers.
  //  - which layer an *element* is on is on the element, and every change to it is a
  //    StrokeLayerItem, so it undoes and syncs like any other stroke edit.
  const LayerList& layers() const { return document->layers; }
  int currentLayer() const { return document->layers.currentId; }
  bool setCurrentLayer(int id);
  // returns the new layer's id, or LayerList::DEFAULT_LAYER if it could not be added
  int addLayer(const char* name = NULL, int aboveIdx = -1);
  // Content on the removed layer is moved to `moveContentTo` (-1 = the layer that ends up current),
  //  undoably, rather than left pointing at a layer that no longer exists.  It would still be
  //  editable if it were - LayerList fails open - but it would be invisible in any layer UI.
  bool removeLayer(int id, int moveContentTo = -1);
  bool setLayerName(int id, const char* name);
  bool setLayerLocked(int id, bool locked);
  bool setLayerHidden(int id, bool hidden);
  bool moveLayer(int fromIdx, int toIdx);
  // move the current selection to a layer, as one undo action; returns the number of elements moved
  int moveSelToLayer(int id);
  // number of elements on a layer across all *loaded* pages
  int layerElementCount(int id) const;

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
  // Cheap partial read for the sidebar's tag view, same technique as extractThumbnail: scan the head
  // of the file (or first gzip block) for the "tags" string config entry rather than fully parsing
  // the document, so listing a folder of documents doesn't mean loading each one.
  static std::vector<std::string> extractDocTags(const char* filename);
  // the same partial read for any config value, as its string; empty if not found
  static std::string extractDocConfigValue(const char* filename, const char* name);
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
  // write the layer table back to the document config and refresh views; called by every layer
  //  table mutator above, which is what keeps "the config is the storage" a single fact;
  //  dirty is false when an undo item is doing the dirtying.
  void layersChanged(bool restack = false, bool dirty = true);
  // apply one layer's state from a LayerTableItem (undo, redo, or a peer); see LayerList::setLayerState
  void setLayerState(int id, const LayerInfo* info, int belowId);
  // replace the whole table (a peer's snapshot on joining a whiteboard), keeping our current layer
  void replaceLayerTable(const LayerList& table);
  // sort each loaded page's elements by their layer's z-order (stable, so order within a layer is
  //  untouched).  Only needed when the *table's* order changes; a single element's move is done by
  //  StrokeLayerItem, which places the node itself.
  void restackLayers();
  // one undoable edit to one layer's table entry: `edit` runs on a copy, and the resulting state of
  //  that layer is applied through setLayerState() with a LayerTableItem recording the prior state
  bool editLayer(int id, const std::function<bool(LayerList&)>& edit);
  // what a layer added now should be numbered - random in a shared session (LayerList::SHARED_ID_BASE)
  int newLayerId() const;
  void themeApplied();

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

