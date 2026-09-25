#include <sstream>
#include "scribbledoc.h"
#include "pentoolbar.h"  // the themed swatches are rebuilt from here when the theme changes
#include "scribblemode.h"
#include "scribblesync.h"
#include "strokebuilder.h"
#include "scribbleapp.h"
#include "tagstore.h"


ScribbleDoc::ScribbleDoc(ScribbleApp* parent, ScribbleConfig* _cfg, ScribbleMode* _mode)
{
  app = parent;
  globalCfg = _cfg;
  scribbleMode = _mode;
  cfg = new ScribbleConfig(globalCfg);
}

ScribbleDoc::~ScribbleDoc()
{
  closeDocument();
}

void ScribbleDoc::addArea(ScribbleArea* area)
{
  area->scribbleDoc = this;
  area->app = app;
  views.push_back(area);
  if(!activeArea)
    activeArea = area;
  nViews = views.size();
  area->loadConfig(cfg);
  if(document) {
    DocPosition docpos = activeArea ? activeArea->getPos() : DocPosition(0, 0, 0);
    area->gotoPos(docpos.pagenum, docpos.pos, false);  // this ensures currPage is set
    area->pageSizeChanged();
  }
}

void ScribbleDoc::removeArea(ScribbleArea* area)
{
  area->reset();
  views.erase(std::remove(views.begin(), views.end(), area), views.end());
  nViews = views.size();
  if(activeArea == area)
    activeArea = views.empty() ? NULL : views[nViews-1];
  area->scribbleDoc = NULL;
}

// --- Layers (LAYERS_INVESTIGATION.md) ---

void ScribbleDoc::layersChanged(bool restack, bool dirty)
{
  cfg->set("layers", document->layers.serialize().c_str());
  cfg->set("currentLayer", document->layers.currentId);
  if(restack)
    restackLayers();
  document->applyLayerState();
  // the current layer is not an undo item, so nothing else marks the document as needing to be written
  if(dirty)
    document->dirtyCount++;
  uiChanged(UIState::SetPageProps);
  doRefresh();
}

bool ScribbleDoc::editLayer(int id, const std::function<bool(LayerList&)>& edit)
{
  // the edit runs on a copy, and only the resulting state of the one layer is applied: that is what
  //  the undo item records, so applying it any other way would let the two drift.  It also means a
  //  refused edit leaves no empty action behind, and startAction() - which can block on a whiteboard
  //  reconnect - is not reached for nothing.
  LayerList trial = document->layers;
  if(!edit(trial))
    return false;
  LayerInfo info;
  int below = LayerList::BELOW_NONE;
  bool present = trial.layerState(id, &info, &below);
  startAction(activeArea->currPageNum);
  LayerTableItem* item = new LayerTableItem(this, id);
  setLayerState(id, present ? &info : NULL, below);
  history->addItem(item);
  endAction();
  return true;
}

int ScribbleDoc::newLayerId() const
{
  if(!scribbleSync)
    return -1;  // the counter
  const LayerList& layers = document->layers;
  int id;
  do {
    id = LayerList::SHARED_ID_BASE + int(UndoHistory::newUuid() % UUID_t(INT_MAX - LayerList::SHARED_ID_BASE));
  } while(layers.find(id));
  return id;
}

void ScribbleDoc::setLayerState(int id, const LayerInfo* info, int belowId)
{
  LayerList& layers = document->layers;
  LayerInfo before;
  int beforeBelow = LayerList::BELOW_NONE;
  bool had = layers.layerState(id, &before, &beforeBelow);
  layers.setLayerState(id, info, belowId);
  // a layer appearing, disappearing or changing place changes the z-order of what is on it
  bool restack = had != (info != NULL) || (info && beforeBelow != belowId);
  // whatever is selected on a layer that just went out of reach has to be let go, exactly as it is
  //  when the edit is made locally; this is also the path a peer's lock or hide arrives by
  bool lostReach = !info || (info->hidden && !before.hidden)
      || (info->locked && !before.locked && id != layers.currentId);
  if(had && lostReach)
    clearSelection();
  layersChanged(restack, false);
}

void ScribbleDoc::replaceLayerTable(const LayerList& table)
{
  document->layers = LayerList::parse(table.serialize().c_str(), document->layers.currentId);
  clearSelection();
  layersChanged(true);
}

void ScribbleDoc::restackLayers()
{
  const LayerList& layers = document->layers;
  for(Page* page : document->pages) {
    if(page->loadStatus != Page::LOAD_OK)
      continue;
    std::vector<Element*> elements;
    for(Element* s : page->children())
      elements.push_back(s);
    // stable, so the order elements were drawn in within one layer survives a reorder of the layers
    std::stable_sort(elements.begin(), elements.end(), [&layers](const Element* a, const Element* b)
        { return layers.zIndexOf(a->layer()) < layers.zIndexOf(b->layer()); });
    // reinsert in the new order; this is a pure z-order change derived from the table, outside the
    //  undo system for the same reason the table itself is
    for(Element* s : elements) {
      page->contentNode->removeChild(s->node);
      page->contentNode->addChild(s->node);
    }
    page->dirtyCount++;
  }
}

bool ScribbleDoc::setCurrentLayer(int id)
{
  int prevId = document->layers.currentId;
  if(!document->layers.setCurrent(id))
    return false;
  // a locked layer is editable only while it is current, so leaving it puts anything selected on it
  //  back out of reach - the selection has to be let go, as it is when a layer is locked
  if(prevId != id && document->layers.isLocked(prevId))
    clearSelection();
  layersChanged();
  return true;
}

int ScribbleDoc::addLayer(const char* name, int aboveIdx)
{
  int id = newLayerId();
  if(id < 0)
    id = document->layers.nextId();
  std::string layername = name ? name : "";
  if(!editLayer(id, [&](LayerList& layers) { return layers.addLayer(layername, aboveIdx, id) == id; }))
    return LayerList::DEFAULT_LAYER;
  return id;
}

int ScribbleDoc::layerElementCount(int id) const
{
  int count = 0;
  for(Page* page : document->pages) {
    if(page->loadStatus != Page::LOAD_OK)
      continue;
    for(Element* s : page->children())
      count += s->layer() == id ? 1 : 0;
  }
  return count;
}

bool ScribbleDoc::removeLayer(int id, int moveContentTo)
{
  if(!document->layers.find(id) || document->layers.size() < 2)
    return false;
  // every page has to be loaded first: an unloaded page can still hold elements on this layer, and
  //  they would be left pointing at a layer that no longer exists
  document->ensurePagesLoaded();
  LayerList after = document->layers;
  if(!after.removeLayer(id))
    return false;
  int target = moveContentTo >= 0 && after.find(moveContentTo) ? moveContentTo : after.currentId;
  startAction(activeArea->currPageNum | UndoHistory::MULTIPAGE);
  for(Page* page : document->pages) {
    std::vector<Element*> elements;
    for(Element* s : page->children()) {
      if(s->layer() == id)
        elements.push_back(s);
    }
    for(Element* s : elements) {
      // the item records the state to *restore*; the move itself is applied here, so addItem's
      //  commit() is what dirties the page - calling redo() as well would count it twice
      int oldlayer = s->layer();
      Element* oldnext = page->moveToLayer(s, target);
      history->addItem(new StrokeLayerItem(s, page, oldlayer, oldnext));
    }
  }
  // the table entry goes *last* in the action, so undo restores the layer before the elements are
  //  moved back onto it, and a peer receives the moves while the layer still exists
  LayerTableItem* item = new LayerTableItem(this, id);
  setLayerState(id, NULL, LayerList::BELOW_NONE);
  history->addItem(item);
  endAction();
  return true;
}

bool ScribbleDoc::setLayerName(int id, const char* name)
{
  std::string layername = name ? name : "";
  return editLayer(id, [&](LayerList& layers) { return layers.setName(id, layername); });
}

// Letting go of a selection on a layer that goes out of reach - anything selected on a layer being
//  locked, unless it is current (which stays editable), and anything on a layer being hidden - is done
//  in setLayerState(), which a peer's edit and an undo pass through as well.
bool ScribbleDoc::setLayerLocked(int id, bool locked)
{
  return editLayer(id, [&](LayerList& layers) { return layers.setLocked(id, locked); });
}

bool ScribbleDoc::setLayerHidden(int id, bool hidden)
{
  return editLayer(id, [&](LayerList& layers) { return layers.setHidden(id, hidden); });
}

// recorded by id and the layer below it, not by index: a peer's concurrent add or remove shifts every
//  index above it, and replaying an index would move a different layer
bool ScribbleDoc::moveLayer(int fromIdx, int toIdx)
{
  const LayerInfo* layer = document->layers.byIndex(fromIdx);
  if(!layer)
    return false;
  return editLayer(layer->id, [&](LayerList& layers) { return layers.moveLayer(fromIdx, toIdx); });
}

int ScribbleDoc::moveSelToLayer(int id)
{
  Selection* sel = activeArea ? activeArea->currSelection : NULL;
  if(!sel || sel->strokes.empty() || !document->layers.find(id)
      || !document->layers.isEditable(id))
    return 0;
  // copy the list: each move restacks the element, and StrokeLayerItem works off the page's node
  //  order, which the selection's own list does not track
  std::vector<Element*> elements(sel->strokes.begin(), sel->strokes.end());
  int nmoved = 0;
  startAction(activeArea->currPageNum);
  for(Element* s : elements) {
    if(s->layer() == id)
      continue;
    int oldlayer = s->layer();
    Element* oldnext = sel->page->moveToLayer(s, id);
    history->addItem(new StrokeLayerItem(s, sel->page, oldlayer, oldnext));
    ++nmoved;
  }
  endAction();
  if(nmoved)
    doRefresh();
  return nmoved;
}

void ScribbleDoc::loadConfig(bool refresh)
{
  // the layer table is stored in the document config, so this - the one point every cfg swap passes
  //  through - is where it has to be read back, exactly as the theme palette is invalidated here
  document->layers = LayerList::parse(cfg->String("layers", ""),
      cfg->Int("currentLayer", LayerList::DEFAULT_LAYER));
  document->applyLayerState();
  // every path that swaps cfg (newDocument, openDocument) calls this immediately afterwards, and a
  //  change to the global prefs can move the recipe too, so this is the one place the cached palette
  //  has to be dropped
  paletteValid = false;
  // a different document can carry a different theme, so the swatches follow it; guarded because
  //  loadConfig() also runs while the app is still being constructed
  if(app && app->penToolbar)
    app->penToolbar->refreshPalette();
  for(unsigned int ii = 0; ii < views.size(); ii++)
    views[ii]->loadConfig(cfg);

  if(refresh) {
    pageSizeChanged();  // most forceful way to refresh
    doRefresh();
  }
}

void ScribbleDoc::updateDocConfig(Document::saveflags_t flags)
{
  DocPosition docpos = (flags & Document::SAVE_COPY) ? DocPosition(0, 0, 0) : activeArea->getPos();
  cfg->set("pageNum", docpos.pagenum);
  cfg->set("xOffset", docpos.pos.x);
  cfg->set("yOffset", docpos.pos.y);
  cfg->set("docFormatVersion", Document::docFormatVersion);
  cfg->saveConfig(document->resetConfigNode());
}

bool ScribbleDoc::checkAndClearErrors(bool forceload)
{
  if(forceload)
    document->ensurePagesLoaded();
  return document->checkAndClearErrors();
}

void ScribbleDoc::openSharedDoc(const char* server, const pugi::xml_node& xml, bool master)
{
  if(!master)
    newDocument();
  if(scribbleSync)
    delete scribbleSync;
  scribbleSync = new ScribbleSync(this);
  scribbleSync->userMessage = [this](std::string msg, int level){ app->showNotify(msg, level); };
  scribbleSync->connectSync(server, xml, master);
  if(!scribbleSync->enableTX)
    ghostPage.reset();
}

void ScribbleDoc::closeDocument()
{
  // disconnectSync starts an event loop via waitForDisconnect, so we must disconnect before calling
  //  ScribbleArea::reset(), since ScribbleArea handling events when in an invalid state can cause crash
  exitPageSelMode();
  if(scribbleSync)
    delete scribbleSync;
  for(unsigned int ii = 0; ii < views.size(); ii++)
    views[ii]->reset();
  delete document;
  delete cfg;
  ghostPage.reset();
  scribbleSync = NULL;
  document = NULL;
  history = NULL;  // history is owned by document
  cfg = NULL;
}

void ScribbleDoc::newDocument()
{
  closeDocument();
  document = new Document();
  history = document->history;
  cfg = new ScribbleConfig(globalCfg);
  //if(props) setDefaultDims(cfg, props);
  loadConfig(false);
  document->insertPage(generatePage(0), 0);
  for(ScribbleArea* view : views) {
    view->setPageNum(0);  // updateContentDim() access currPage if VIEWMODE_SINGLE
    view->updateContentDim();
    view->gotoPos(0, Point(-10, -10), false);
  }
  pageSizeChanged();  // pageCountChanged() is only for changes due to editing
  // scribbleMode == NULL indicates we are a read-only doc, so no ghost page
  if(scribbleMode)
    ghostPage.reset(generatePage(INT_MAX));
  //fileName = "";
  fileLastMod = 0;
  uiChanged(UIState::SetDoc);
  document->bookmarksDirty = false;
  app->repaintBookmarks(true);
  doRefresh();
}

bool ScribbleDoc::deleteDocument(const char* filename)
{
  Document tempdoc;
  // OK to delay load when deleting doc
  return tempdoc.load(new FileStream(filename), true) == Document::LOAD_OK && tempdoc.deleteFiles();
}

Image ScribbleDoc::extractThumbnail(const char* filename)
{
  //static const size_t MAX_BUFF_SIZE = (1 << 20);

  StringRef buff;
  FileStream istrm(filename, "rb");  //std::ifstream istrm(filename, std::ios_base::in | std::ios::binary);
  MemStream infstrm;
  //size_t toread = 16384, bytesread = 0;

  FSPath fileinfo(filename);
  if(fileinfo.extension() == "svgz" || fileinfo.extension() == "gz") {
    minigz_io_t zistrm(istrm);
    auto blockInfo = bgz_get_index(zistrm);
    std::stringstream inf_block;
    if(blockInfo.empty() || !bgz_read_block(zistrm, &blockInfo.back() - 1, minigz_io_t(infstrm)))
      return Image(0,0);
    buff = StringRef(infstrm.data(), infstrm.size());  //buff.len = zstrm.readp((void**)&buff.str, SIZE_MAX);
  }
  else
    buff.len = istrm.readp((void**)&buff.str, 1 << 18);  // 256KB

  int idx = buff.find("id=\"thumbnail\"");
  if(idx < 0)
    idx = buff.find("id='thumbnail'");
  if(idx >= 0) {
    idx = buff.find("base64,", idx);
    if(idx >= 0) {
      int end = buff.findFirstOf("'\"", idx);
      if(end >= 0) {
        idx += 7;
        auto dec64 = base64_decode(&buff[idx], end - idx);
        Image img = Image::decodeBuffer(dec64.data(), dec64.size());
        return img;
      }
    }
  }
  return Image(0,0);
}

std::string ScribbleDoc::extractDocConfigValue(const char* filename, const char* name)
{
  StringRef buff;
  FileStream istrm(filename, "rb");
  MemStream infstrm;

  FSPath fileinfo(filename);
  if(fileinfo.extension() == "svgz" || fileinfo.extension() == "gz") {
    minigz_io_t zistrm(istrm);
    auto blockInfo = bgz_get_index(zistrm);
    std::stringstream inf_block;
    if(blockInfo.empty() || !bgz_read_block(zistrm, &blockInfo.back() - 1, minigz_io_t(infstrm)))
      return "";
    buff = StringRef(infstrm.data(), infstrm.size());
  }
  else
    buff.len = istrm.readp((void**)&buff.str, 1 << 18);  // 256KB, same window extractThumbnail uses

  // looking for e.g. <string name="tags" value="t1,t2,..."/> as written by ScribbleConfig::saveConfig()
  int idx = buff.find(fstring("name=\"%s\"", name).c_str());
  if(idx < 0)
    idx = buff.find(fstring("name='%s'", name).c_str());
  if(idx < 0)
    return "";
  idx = buff.find("value=", idx);
  if(idx < 0)
    return "";
  idx += 6;
  char quote = idx < buff.len ? buff[idx] : '\0';
  if(quote != '"' && quote != '\'')
    return "";
  ++idx;
  int end = buff.findFirstOf(quote == '"' ? "\"" : "'", idx);
  if(end < 0)
    return "";
  return std::string(&buff[idx], end - idx);
}

std::vector<std::string> ScribbleDoc::extractDocTags(const char* filename)
{
  return TagStore::parseTagList(extractDocConfigValue(filename, "tags").c_str());
}

Document::loadresult_t ScribbleDoc::openDocument(const char* filename, bool delayload)
{
  FileStream* strm = new FileStream(filename, "rb+");
  if(!strm->is_open())
    strm->open("rb");
  return openDocument(strm, delayload);
}

Document::loadresult_t ScribbleDoc::openDocument(IOStream* strm, bool delayload)
{
  Document* newdoc = new Document();
  auto res = newdoc->load(strm, delayload);
  if(res == Document::LOAD_FATAL || (res == Document::LOAD_EMPTYDOC
      && !containsWord("svg svgz html htm", FSPath(strm->name()).extension().c_str()))) {
    delete newdoc;
    return Document::LOAD_FATAL;
  }
  // replace this->document with newdoc
  closeDocument();
  document = newdoc;
  history = document->history;
  cfg = new ScribbleConfig(globalCfg);
  cfg->loadConfig(document->getConfigNode());
  loadConfig(false);
  if(res == Document::LOAD_EMPTYDOC)
    document->insertPage(generatePage(0), 0);
  res = cfg->Int("docFormatVersion", 0) > Document::docFormatVersion ? Document::LOAD_NEWERVERSION : res;
  int pagenum = cfg->Int("pageNum", 0);
  Point pos(cfg->Float("xOffset", -10.0f), cfg->Float("yOffset", -10.0f));
  // we do this before calling pageSizeChanged() to prevent unnecessary loading of page 0
  for(ScribbleArea* view : views) {
    view->setPageNum(pagenum);  // updateContentDim() access currPage if VIEWMODE_SINGLE
    view->updateContentDim();
    view->gotoPos(pagenum, pos, false);
  }
  pageSizeChanged();  // pageCountChanged() is only for changes due to editing
  // no ghost page if any load errors to suggest user not modify document
  if((res == Document::LOAD_OK || res == Document::LOAD_EMPTYDOC) && scribbleMode)
    ghostPage.reset(generatePage(INT_MAX));
  uiChanged(UIState::SetDoc);
  document->bookmarksDirty = false;
  app->repaintBookmarks(true);
  doRefresh();
#if !PLATFORM_IOS
  fileLastMod = getFileMTime(fileName());
#endif
  return res;
}

bool ScribbleDoc::saveDocument(const char* filename, Document::saveflags_t flags)
{
  return saveDocument(filename ? new FileStream(filename, "wb+") : NULL, flags);
}

bool ScribbleDoc::saveDocument(IOStream* strm, Document::saveflags_t flags)
{
  if(!strm)
    flags |= Document::SAVE_BGZ_PARTIAL;  // whole file will be written if this is not set

  doCancelAction();
  // when saving a copy for sharing, set position to start of document
  updateDocConfig(flags);
  flags |= cfg->Int("compressLevel", 2) << 24;  // ignored for uncompressed formats

  bool ok = false;
  if(cfg->Bool("saveThumbnail")) {
#if PLATFORM_IOS
    IOStream* thumbstrm = strm ? strm : document->blockStream.get();
    if(thumbstrm->type() & IOStream::UIDOCSTREAM) {
      Image iosthumb(1024, 1024);
      activeArea->drawThumbnail(&iosthumb);
      iosSetDocThumbnail(static_cast<UIDocStream*>(thumbstrm)->uiDocument,
        iosthumb.bytes(), iosthumb.width, iosthumb.height);
    }
#endif
    Image thumbnail(240, 400, Image::PNG);
    activeArea->drawThumbnail(&thumbnail);
    auto buff = base64_encode(thumbnail.encode(Image::PNG));
    ok = document->save(strm, (char*)buff.data(), flags);
  }
  else
    ok = document->save(strm, NULL, flags);
  if(ok) {
#if !PLATFORM_IOS
    fileLastMod = getFileMTime(fileName());
#endif
    app->refreshUI(this, (1 << UIState::SaveDoc));
  }
  else
    delete strm;

  return ok;
}

// insert pages from another document into the current document (immediately following current page)
Document::loadresult_t ScribbleDoc::insertDocument(IOStream* strm)
{
  Document* otherdoc = new Document();
  Document::loadresult_t res = otherdoc->load(strm, false);
  if(res == Document::LOAD_OK) {
    int where = activeArea->currPageNum + 1;
    startAction((where - 1) | UndoHistory::MULTIPAGE);
    while(otherdoc->numPages() > 0) {
      Page* page = otherdoc->deletePage(otherdoc->numPages() - 1);
      document->insertPage(page, where);
    }
    endAction();
    updateGhostPage();
    pageCountChanged(where);
    activeArea->gotoPos(where + 1, Point(-10, -10), false);
    uiChanged(UIState::InsertDoc);
    doRefresh();
  }
  delete otherdoc;
  return res;
}

void ScribbleDoc::resetDocPrefs()
{
  delete cfg;
  cfg = new ScribbleConfig(globalCfg);   //cfg->set("bookmarkSerialNum", n);
  paletteValid = false;  // the document's theme override went with the old cfg
}

void ScribbleDoc::doUndoRedo(bool redo)
{
  clearSelection();
  // clear page's dirty rect so we can jump to what was dirtied by undo
  dirtyPage(activeArea->currPageNum);
  int prevpages = document->numPages();
  // handle case of pages added and removed w/ no change in page count (currently just deletion of sole page)
  int docdirty = document->dirtyCount;
  int pagenum = redo ? history->redo() : history->undo();
  // An undone stroke stays in its view's recentStrokes, and the next undo item added - by any action,
  //  e.g. a layer or page property edit - discards it and frees it, leaving groupStrokes() reading
  //  freed memory.  Grouping only ever spans strokes just drawn, so after an undo there is nothing
  //  worth keeping.  Local undo is the only way strokes are left undone; sync restores its position.
  for(ScribbleArea* view : views)
    view->recentStrokes.clear();
  undoRedoUpdate(pagenum, document->numPages() == prevpages && document->dirtyCount != docdirty ? 0 : prevpages);
}

// this is split out as a separate fn because it is also used by SyncScribble (for which viewdirty = false)
void ScribbleDoc::undoRedoUpdate(int pagenum, int prevpages, bool viewdirty)
{
  bool multipage = pagenum & UndoHistory::MULTIPAGE;
  pagenum = pagenum & ~UndoHistory::MULTIPAGE;
  int npages = document->numPages();
  if(prevpages != npages || pagenum < 0 || pagenum >= npages) {
    // try to restore each area's position
    pageCountChanged(pagenum, prevpages);
    document->bookmarksDirty = true;
    if(viewdirty && pagenum >= 0) {
      if(pagenum < npages) {
        // page removed - show top of next page ... do this for page added too
        if(prevpages > npages || !activeArea->isPageVisible(pagenum))
          activeArea->viewRect(pagenum, Rect::wh(document->pages[pagenum]->width(), 0));
      }
      else
        activeArea->gotoPage(npages);  // last page removed - show bottom of prev page
    }
    return;
  }
  Page* page = document->pages[pagenum];
  Rect pagedirty = dirtyPage(pagenum);  // update dirty region for all views  //page->getDirty();
  if(!pagedirty.isValid())
    pagedirty = page->rect();
  else if(pagedirty.contains(page->rect())) {
    pageSizeChanged();
    document->bookmarksDirty = true;
  }
  for(size_t ii = 0; ii < nViews; ++ii) {
    if(multipage)
      views[ii]->repaintAll();
    if(viewdirty && views[ii]->currPageNum == pagenum && views[ii]->isVisible(views[ii]->pageDimToDim(pagedirty)))
      viewdirty = false;
  }
  if(viewdirty)
    activeArea->viewRect(pagenum, pagedirty);  // ensure dirty rect is visible
}

// what we want to do: view a specified pos in activeArea; don't change other views
// - problems: we'd have to save other views before making change
void ScribbleDoc::pageCountChanged(int pagenum, int prevpages)
{
  for(unsigned int ii = 0; ii < nViews; ii++)
    views[ii]->pageCountChanged(pagenum, prevpages);
}

// insert a new page before page "where"; we make the new page the current page
Page* ScribbleDoc::generatePage(int where, const PageProperties* props) const
{
  Page* newPage = NULL;
  Page* refPage = document->numPages() > 0 ?
      document->pages[std::max(0, std::min(where, document->numPages()-1))] : NULL;
  if(props)
    newPage = new Page(*props);
  // if page that will preceed page being inserted has custom ruling, duplicate it
  else if(refPage && refPage->isCustomRuling && !refPage->ruleNode->hasClass("write-no-dup"))
    newPage = new Page(refPage->getProperties(), refPage->ruleNode);
  else {
    PageProperties defprops(cfg->Float("pageWidth"), cfg->Float("pageHeight"),
        cfg->Float("xRuling"), cfg->Float("yRuling"), cfg->Float("marginLeft"),
        Color::fromRgb(cfg->Int("pageColor")), Color::fromArgb(cfg->Int("ruleColor")), cfg->Float("dotRadius"));
    newPage = new Page(defprops);
  }
  if(globalCfg->Bool("sRGB"))
    newPage->svgDoc->setAttribute("color-interpolation", "linearRGB");  // SVG spec says default is "sRGB"
  return newPage;
}

void ScribbleDoc::updateGhostPage()
{
  if(ghostPage)
    ghostPage.reset(generatePage(INT_MAX));
}

void ScribbleDoc::newPage(int where, const PageProperties* props)
{
  // if appending page, give `where` appropriate value for undo/redo
  if(where < 0 || where > document->numPages())
    where = document->numPages();
  startAction(where);
  // for double tap to add page, looks like first tap will make last page current, so currPageNum is safe
  insertPage(generatePage(activeArea->currPageNum, props), where);
  endAction();
}

void ScribbleDoc::insertPage(Page* page, int where)
{
  clearSelection();
  where = document->insertPage(page, where);
  if(where >= document->numPages() - 1)
    updateGhostPage();
  // view the new page
  pageCountChanged(where, document->numPages() - 1);
  activeArea->gotoPage(where);
  uiChanged(UIState::InsertPage);
}

void ScribbleDoc::deletePage(int where)
{
  if(document->numPages() < 2)
    return;
  clearSelection();
  document->pages[where]->isSelected = true;
  deletePages();
}

void ScribbleDoc::deletePages()
{
  int where = -1;
  int prevpages = document->numPages();
  // find first visible page that will not be deleted
  Page* prevpospage = NULL;
  Point prevpos;
  for(int ii = 0; ii < document->numPages(); ++ii) {
    Page* p = document->pages[ii];
    if(!p->isSelected && activeArea->isPageVisible(ii)) {
      prevpospage = p;
      prevpos = activeArea->screenToDim(Point(0,0)) - activeArea->getPageOrigin(ii);
      break;
    }
  }

  for(int ii = 0; ii < document->numPages();) {
    if(document->pages[ii]->isSelected) {
      if(where < 0)
        startAction(ii | UndoHistory::MULTIPAGE);
      where = ii;
      document->pages[ii]->isSelected = false;
      document->deletePage(ii);
    }
    else
      ++ii;
  }
  if(where < 0)
    return;
  if(document->numPages() < 1)
    document->insertPage(generatePage(0), 0);
  endAction();
  if(where >= document->numPages())
    updateGhostPage();
  document->bookmarksDirty = true;  // deleted page may have contained bookmarks
  pageCountChanged(where, prevpages);  // this will set curr page to "where"
  // restore view position
  // what if no deleted pages were visible initially? ... difficult to determine if this is the case
  if(prevpospage)
    activeArea->gotoPos(prevpospage->getPageNum(), prevpos, false);

  uiChanged(UIState::DeletePage);
}

void ScribbleDoc::pastePages(Clipboard* clipboard, int where)
{
  clearSelection();  // clear stroke selection/page selection
  int prevpages = document->numPages();

  // getPos() uses the current page, so make sure it's the first visible page!
  activeArea->setPageNum(activeArea->dimToPageNum(activeArea->screenToDim(Point(0,0))));
  DocPosition prevpos = activeArea->getPos();
  Page* prevpospage = document->pages[prevpos.pagenum];
  int numpaste = clipboard->content->children().size();

  startAction((where - 1) | UndoHistory::MULTIPAGE);
  for(SvgNode* n : clipboard->content->children()) {
    Page* p = new Page;
    if(n->type() == SvgNode::DOC)
      p->loadSVG(static_cast<SvgDocument*>(n->clone()));
    document->insertPage(p, where++);
  }
  endAction();
  updateGhostPage();
  document->bookmarksDirty = true;
  pageCountChanged(where - numpaste, prevpages);

  activeArea->gotoPos(prevpospage->getPageNum(), prevpos.pos, false);
  // pasted pages being visible takes precedence
  if(!activeArea->isPageVisible(where - 1) && !activeArea->isPageVisible(where - numpaste))
    activeArea->gotoPage(where - numpaste);

  uiChanged(UIState::InsertPage);
}

// pagenum < 0 to select range from previous page to -pagenum-1; 0 - npages-1 to toggle single page;
//  SelectPagesFlags for select all, invert selection
void ScribbleDoc::selectPages(int pagenum)  //, Point toolpos)
{
  int first = pagenum, last = pagenum;
  bool forcesel = pagenum < 0 || pagenum == PAGESEL_ALL;
  if(pagenum < 0) {
    pagenum = std::min(-pagenum-1, document->numPages() - 1);
    first = std::min(pagenum, prevSelPageNum);
    last = std::max(pagenum, prevSelPageNum);
  }
  else if(pagenum > document->numPages() - 1) {
    first = 0;
    last = document->numPages() - 1;
  }
  for(int ii = first; ii <= last; ++ii) {
    Page* p = document->pages[ii];
    numSelPages += !p->isSelected ? 1 : (forcesel ? 0 : -1);
    p->setSelected(forcesel ? true : !p->isSelected);
  }
  if(pagenum < document->numPages())
    prevSelPageNum = pagenum;
  if(first != last)
    repaintAll();
  // popup tools not shown for stroke selection sel all/inv sel either (tools avail on overflow menu anyway)
  //if(numSelPages > 0 && cfg->Bool("popupToolbar"))  //&& !toolpos.isNaN()
  //  app->showSelToolbar(activeArea->screenToGlobal(toolpos.isNaN() ? activeArea->screenRect.center() : toolpos));
}

void ScribbleDoc::deleteSelection()
{
  if(numSelPages > 0) {
    deletePages();
    numSelPages = 0;
    prevSelPageNum = 0;
  }
  else
    activeArea->deleteSelection();
}

void ScribbleDoc::clearSelection()
{
  if(numSelPages > 0) {
    for(Page* p : document->pages)
      p->setSelected(false);
    numSelPages = 0;
    prevSelPageNum = 0;
  }
  else {
    for(unsigned int ii = 0; ii < views.size(); ii++) {
      if(views[ii]->currSelection)
        views[ii]->clearSelection();
    }
  }
}

// can't include in clearSelection() since we also call that for, e.g., undo and pasting pages
void ScribbleDoc::exitPageSelMode()
{
  if(scribbleMode && scribbleMode->getMode() == MODE_PAGESEL) {
    clearSelection();
    scribbleMode->setMode(MODE_STROKE);
  }
}

void ScribbleDoc::doCancelAction()
{
  activeArea->doCancelAction();
}

// set dimensions for future pages
void ScribbleDoc::setDefaultDims(ScribbleConfig* c, const PageProperties* props)
{
  if(props->width > 0)
    c->set("pageWidth", props->width);
  if(props->height > 0)
    c->set("pageHeight", props->height);
  c->set("xRuling", props->xRuling);
  c->set("yRuling", props->yRuling);
  c->set("marginLeft", props->marginLeft);
  c->set("dotRadius", props->dotRadius);
  c->set("pageColor", int(props->color.argb()));
  c->set("ruleColor", int(props->ruleColor.argb()));
  //if(props->ruleLayer) {
  //  c->setRuling("default", new RuleLayer(*props->ruleLayer));
  //}
}

// should move this (these?) somewhere else; also, get rid of clip check in setPageProperties
static bool doesClipPageStrokes(const PageProperties* props, Page* page)
{
  Rect r = page->getBBox();
  return (props->width != 0 && r.right > props->width && r.right < page->props.width)
      || (props->height != 0 && r.bottom > props->height && r.bottom < page->props.height);
}

bool ScribbleDoc::doesClipStrokes(const PageProperties* props, bool applytoall)
{
  if(!applytoall)
    return doesClipPageStrokes(props, activeArea->currPage);
  for(Page* page : document->pages) {
    if((numSelPages == 0 || page->isSelected) && doesClipPageStrokes(props, page))
      return true;
  }
  return false;
}

// TODO: enum RulingFlags {RULING_ALL=1, RULING_DFLT=2, RULING_GLOBAL=4, RULING_NOUNDO=8};
bool ScribbleDoc::setPageProperties(const PageProperties* props, bool applytoall, bool docdefault, bool global, bool undoable)
{
  if(undoable)
    startAction(activeArea->currPageNum | (applytoall ? UndoHistory::MULTIPAGE : 0));
  bool clipped = false;
  if(applytoall) {
    for(Page* page : document->pages) {
      if(numSelPages == 0 || page->isSelected)
        clipped = page->setProperties(props) || clipped;
    }
  }
  else
    clipped = activeArea->currPage->setProperties(props);
  if(undoable)
    endAction();
  pageSizeChanged();
  // set doc and/or global defaults
  if(docdefault)
    setDefaultDims(cfg, props);
  if(global)
    setDefaultDims(globalCfg, props);
  //if(docdefault || global || (activeArea->currPageNum == document->numPages() - 1 && activeArea->currPage->isCustomRuling))
  updateGhostPage();
  uiChanged(UIState::SetPageProps);
  doRefresh();
  return clipped;
}

const Palette& ScribbleDoc::palette()
{
  if(!paletteValid) {
    themeGenKnown = generatePalette(cfg->themeRecipe(), &themePalette);
    paletteValid = true;
  }
  return themePalette;
}

void ScribbleDoc::setTheme(const PaletteRecipe& recipe, bool applyPages, bool globalDefault, bool ownAction)
{
  // One action for the recipe and the page recolor, so one Ctrl+Z puts both back - and so the recipe
  //  reaches a whiteboard peer at all, since undo items are the sync protocol (COLORS_SPEC.md §8).
  if(ownAction)
    startAction(activeArea->currPageNum | (applyPages ? UndoHistory::MULTIPAGE : 0));
  ThemeChangedItem* item = new ThemeChangedItem(this);  // the theme to restore
  cfg->setThemeRecipe(recipe);
  if(globalDefault)
    globalCfg->setThemeRecipe(recipe);
  paletteValid = false;
  const Palette& pal = palette();

  // the recipe is also the source of the page defaults, so a page added later matches the theme
  cfg->set("pageColor", int(pal.paper.argb()));
  cfg->set("ruleColor", int(pal.rule.argb()));
  // Accents are part of the theme too (COLORS_SPEC.md §10.3): a bookmark or link in some unrelated
  //  blue is the one thing on the page that ignores the palette.  These were global-only; writing
  //  them to the document config as well is what resolves the inconsistency the spec flagged, and
  //  costs nothing because cfg falls through to globalCfg for documents that have no theme.
  cfg->set("bookmarkColor", int(pal.bookmark.argb()));
  cfg->set("linkColor", int(pal.link.argb()));
  if(globalDefault) {
    globalCfg->set("pageColor", int(pal.paper.argb()));
    globalCfg->set("ruleColor", int(pal.rule.argb()));
    globalCfg->set("bookmarkColor", int(pal.bookmark.argb()));
    globalCfg->set("linkColor", int(pal.link.argb()));
  }
  history->addItem(item);

  if(applyPages) {
    // Deliberately not setPageProperties(): that applies one PageProperties to every page, and since
    //  Page::setProperties() assigns the whole struct, it would flatten each page's own ruling and
    //  (for any page whose size differs) its dimensions onto the current page's. A theme changes two
    //  colors and must leave the rest of each page alone, so each page gets its own props back with
    //  only color and ruleColor replaced.
    for(Page* page : document->pages) {
      // Pages whose background is an image - PDF import, document scan - are skipped: their paper is
      //  the image, so recoloring underneath it does nothing visible and only dirties the page.
      if(page->isCustomRuling)
        continue;
      PageProperties props = page->getProperties();
      props.color = pal.paper;
      props.ruleColor = pal.rule;
      page->setProperties(&props);
    }
  }
  if(ownAction)
    endAction();
  if(applyPages)
    pageSizeChanged();
  themeApplied();
}

ThemeState ScribbleDoc::themeState() const
{
  ThemeState state;
  state.recipe = cfg->themeRecipe();
  state.pageColor = Color::fromArgb(cfg->Int("pageColor"));
  state.ruleColor = Color::fromArgb(cfg->Int("ruleColor"));
  state.bookmarkColor = Color::fromArgb(cfg->Int("bookmarkColor"));
  state.linkColor = Color::fromArgb(cfg->Int("linkColor"));
  return state;
}

// The document half of setTheme() only: the machine-wide default is this user's, and the pages are
//  recolored by their own PageChangedItems.  Note that undoing the theming of a document that had
//  none leaves it with an explicit recipe equal to the inherited one - the same palette, now pinned.
void ScribbleDoc::applyThemeState(const ThemeState& state)
{
  cfg->setThemeRecipe(state.recipe);
  cfg->set("pageColor", int(state.pageColor.argb()));
  cfg->set("ruleColor", int(state.ruleColor.argb()));
  cfg->set("bookmarkColor", int(state.bookmarkColor.argb()));
  cfg->set("linkColor", int(state.linkColor.argb()));
  themeApplied();
}

void ScribbleDoc::themeApplied()
{
  paletteValid = false;
  // ScribbleApp caches the bookmark color in a member, so the config write alone would not take
  //  effect until the next restart
  app->bookmarkColor = Color::fromArgb(cfg->Int("bookmarkColor"));
  updateGhostPage();
  // the pen toolbar's swatches are generated from this palette, so they have to be rebuilt here -
  //  setPen() early-returns when the pen has not changed, which a theme change does not do
  if(app->penToolbar)
    app->penToolbar->refreshPalette();
  uiChanged(UIState::SetPageProps);
  doRefresh();
}

// Restyle (COLORS_SPEC.md §7).  Deliberate deviation from the spec: strokes carry **no `__inkbase`**.
//
// The spec had each stroke record which reference-palette entry it came from, so a restyle could look
//  it up later.  That is unnecessary here, because at the moment of restyling we know both palettes -
//  the old recipe is still in cfg, and the new one is the argument - so a stroke can be identified by
//  its literal color being an exact member of the *old* palette.  Two things fall out of that, both
//  strictly better: no new file-format attribute, and strokes drawn before this feature existed are
//  restyleable, where a tag-based scheme could only ever restyle strokes drawn after it shipped.
//
// The cost is that a document carried through several themes without restyling keeps only its most
//  recent theme's strokes mappable; older ink is left alone.  That is the same "leave it alone" rule
//  that protects imported PDFs and deliberately off-palette strokes, so it fails safe.
int ScribbleDoc::restyleToTheme(const PaletteRecipe& recipe, bool applyPages, bool globalDefault)
{
  // Copy, not reference: setTheme() regenerates the cached palette in place and would leave this
  //  dangling - and every mapping below is from the OLD palette.
  Palette oldPal = palette();
  document->ensurePagesLoaded();  // a delay-loaded page cannot be restyled

  // One action around *both* halves.  setTheme() opens its own action for the recipe and the page
  //  recolor, so letting it do that here would make a restyle two undo steps - press Ctrl+Z once and
  //  the strokes revert while the paper and the recipe stay on the new theme, which looks like a bug
  //  and is one.
  int nchanged = 0;
  startAction(activeArea->currPageNum | UndoHistory::MULTIPAGE);
  setTheme(recipe, applyPages, globalDefault, false);
  const Palette& newPal = palette();
  for(Page* page : document->pages) {
    // copy the element list: setProperties() can replace an element (see Selection::setStrokeProperties)
    std::vector<Element*> elements;
    for(Element* s : page->children())
      elements.push_back(s);
    for(Element* s : elements) {
      // Groups and text descend recursively and need the clone dance that Selection does; a plain
      //  stroke is the overwhelming case and the only one handled here.  Skipping the rest is why
      //  this reports a count rather than claiming to have restyled everything.
      if(s->containerNode() || s->node->type() == SvgNode::TEXT)
        continue;
      StrokeProperties props = s->getProperties();
      if(!props.color.isValid())
        continue;
      Color mapped;
      if(!newPal.mapFrom(oldPal, props.color, &mapped))
        continue;              // not the old theme's ink - leave it exactly as it is
      if(mapped == props.color)
        continue;              // already right; do not manufacture an undo item for a no-op
      StrokeChangedItem* undoitem = new StrokeChangedItem(s, page);
      if(s->setProperties(StrokeProperties(mapped, -1))) {
        history->addItem(undoitem);
        ++nchanged;
      }
      else
        delete undoitem;
    }
  }
  endAction();
  pageSizeChanged();
  doRefresh();
  return nchanged;
}

// Set or clear a page's outline entry.  Note this goes through the undo system rather than calling
//  Page::setOutlineEntry() directly: undo items are the sync protocol, so doing it here is also what
//  makes the entry reach other clients of a shared document.
bool ScribbleDoc::setPageOutline(int pagenum, const char* title, int level)
{
  if(pagenum < 0 || pagenum >= document->numPages())
    return false;
  Page* page = document->pages[pagenum];
  if(!page->ensureLoaded())
    return false;
  std::string newtitle(title ? title : "");
  int newlevel = newtitle.empty() ? 0 : std::min(std::max(0, level), int(Page::MAX_OUTLINE_LEVEL));
  if(page->outlineTitle == newtitle && page->outlineLevel == newlevel)
    return false;  // nothing to do; don't push an empty undo step

  startAction(pagenum);
  if(history->undoable())
    history->addItem(new PageOutlineItem(page));  // records the *previous* entry, as PageChangedItem does
  // no dirtyCount++ here: UndoHistory::addItem() calls commit(), which does it - the same convention
  //  Page::setProperties() follows
  page->setOutlineEntry(newtitle.c_str(), newlevel);
  endAction();
  return true;
}

void ScribbleDoc::openURL(const char* url)
{
  // address starting with a '.' (so '.' or '..') is assumed to point to a local file
  if(url[0] == '.') {
    FSPath urlfile = FSPath(fileName()).parent().child(url);
    std::string href;
    if(!urlfile.exists()) {
      size_t pnd = urlfile.path.find_last_of('#');
      if(pnd != std::string::npos) {
        href = urlfile.path.substr(pnd);
        urlfile.path.resize(pnd);
      }
    }
    // assume link to local HTML file is a Write document
    // QProcess::startDetached(QApplication::applicationFilePath(), QStringList(filename));
    if(urlfile.exists() && urlfile.extension() == cfg->String("docFileExt")) {
      if(app->openDocument(canonicalPath(urlfile)) && !href.empty())
        app->activeArea()->viewHref(href.c_str());
    }
    else
#if PLATFORM_OSX
      ScribbleApp::openURL(("file:///" + urlEncode(urlfile.path.c_str())).c_str());
#else
      ScribbleApp::openURL(("file:///" + urlfile.path).c_str());
#endif
  }
  else
    ScribbleApp::openURL(url);
}

bool ScribbleDoc::canUndo() const
{
  return history->canUndo() && (!scribbleSync || scribbleSync->canUndo());
}

bool ScribbleDoc::canRedo() const
{
  return history->canRedo();
}

// we could consider moving some (most) of these to Application
void ScribbleDoc::doCommand(int itemid)
{
  doCancelAction();
  switch(itemid) {
  case ID_UNDO:
    if(canUndo()) {
      doUndoRedo();
      if(scribbleSync)
        scribbleSync->sendHist();
    }
    break;
  case ID_REDO:
    if(canRedo()) {
      doUndoRedo(true);
      if(scribbleSync)
        scribbleSync->sendHist();
    }
    break;
  // clipboard related
  case ID_DELSEL:
    deleteSelection();
    break;
  case ID_INVSEL:
    if(scribbleMode->getMode() == MODE_PAGESEL)
      selectPages(PAGESEL_INV);
    else
      activeArea->invertSelection();
    break;
  case ID_SELALL:
    if(scribbleMode->getMode() == MODE_PAGESEL)
      selectPages(PAGESEL_ALL);
    else
      activeArea->selectAll();
    break;
  case ID_COPYSEL:
  case ID_CUTSEL:
  case ID_DUPSEL:
  {
    std::unique_ptr<Clipboard> cb(new Clipboard);
    Selection* sel = activeArea->currSelection;
    int flags = 0;
    if(!sel) {
      for(Page* p : document->pages) {
        if(p->isSelected) {
          SvgNode* p2 = p->svgDoc->clone();
          // make sure "write-page" class is set even for foreign docs - used to distinguish pages when pasting
          if(!p2->hasClass("write-page"))
            p2->addClass("write-page");
          cb->content->addChild(p2);  //addStroke(p->docElement());
        }
      }
    }
    else {
      flags = sel->selector && !sel->selector->drawHandles ? ScribbleArea::PasteNoHandles : 0;
      sel->toSorted(cb.get());
      if(itemid != ID_CUTSEL)
        cb->replaceIds();
    }
    if(!cb->count())  // normally prevented by button disabled in UI, but happened running test with Ctrl pressed
      break;
    if(itemid == ID_DUPSEL)
      activeArea->doPaste(cb.get(), NULL, flags | ScribbleArea::PasteMoveClipboard);
    else
      app->setClipboard(cb.release(), (sel && itemid == ID_COPYSEL) ? activeArea->getCurrPage() : NULL, flags);
    if(itemid == ID_CUTSEL)
      deleteSelection();
    break;
  }
  case ID_PASTE:
    if(app->clipboard)
      activeArea->doPaste(app->clipboard.get(), app->clipboardPage, app->clipboardFlags);
    break;
  case ID_PAGEAFTER:
    newPage(activeArea->currPageNum + 1);
    break;
  case ID_PAGEBEFORE:
    newPage(activeArea->currPageNum);
    break;
  case ID_DELPAGE:
    deletePage(activeArea->currPageNum);
    break;
  default:
    activeArea->doCommand(itemid);
    return;
  }
  uiChanged(UIState::Command);
  doRefresh();
}

// returned rect only needed for jumping view for undo
// we could consider instead scanning all pages for dirtiness, instead of only active pages of each area
Rect ScribbleDoc::dirtyPage(int pagenum)
{
  Rect dirty = document->pages[pagenum]->getDirty();
  if(dirty.isValid()) {
    for(size_t ii = 0; ii < nViews; ii++)
      views[ii]->dirtyPage(pagenum, dirty);
    document->pages[pagenum]->clearDirty();
  }
  return dirty;
}

void ScribbleDoc::pageSizeChanged()
{
  for(unsigned int ii = 0; ii < nViews; ii++)
    views[ii]->pageSizeChanged();
}

void ScribbleDoc::repaintAll()
{
  for(unsigned int ii = 0; ii < nViews; ii++)
    views[ii]->repaintAll();
}

void ScribbleDoc::doRefresh()
{
  if(autoSaveReq && getActiveMode() == MODE_NONE) {
    autoSaveReq = false;
    if(fileName()[0])
      saveDocument();
  }
  // won't actually repaint unless dirty
  // shouldn't we just check all (visible) pages? then we wouldn't need explicit repaintAll calls (?)
  for(ScribbleArea* view : views)
    dirtyPage(view->currPageNum);  // first build directRectDim from each potentially dirty page
  for(ScribbleArea* view : views)
    view->reqRepaint();  // then update dirtyRectScreen for each view and set PIXELS_DIRTY if needed

  if(document->bookmarksDirty) {
    app->repaintBookmarks();
    document->bookmarksDirty = false;
  }
  if(uiDirty)
    app->refreshUI(this, uiDirty);
  uiDirty = 0;
}

void ScribbleDoc::uiChanged(int reason)
{
  ASSERT((1 << reason) > 0 && "Too many UIChangeFlags values!");
  uiDirty |= (1 << reason);
}

void ScribbleDoc::bookmarkHit(int pagenum, Element* bookmark)
{
  Page* page = document->pages[pagenum];
  Point cornerpos(bookmark->bbox().left - 5,
      MIN(bookmark->bbox().top - 5, page->getYforLine(page->getLine(bookmark))));

  activeArea->doGotoPos(pagenum, cornerpos, false);
  if(cfg->Bool("autoHideBookmarks"))
    app->hideBookmarks();
}

int ScribbleDoc::getScribbleMode(int modemod)
{
  return scribbleMode ? scribbleMode->getScribbleMode(modemod) : MODE_NONE;
}

int ScribbleDoc::getActiveMode() const
{
  return activeArea->currMode;
}

Color ScribbleDoc::getCurrPageColor() const
{
  return activeArea->getCurrPage()->props.color;
}

// dirty is relative to activeArea's current page!
void ScribbleDoc::updateCurrStroke(Rect dirty)
{
  if(!dirty.isValid()) {}
  else if(nViews > 1) {
    // we just want to dirty screen, but this is the easiest way since `dirty` is relative to activeArea's
    //  page and other views could have different page number
    for(size_t ii = 0; ii < nViews; ii++)
      views[ii]->dirtyPage(activeArea->currPageNum, dirty);
    //activeArea->currPage->growDirtyRect(dirty);
  }
  else
    activeArea->dirtyScreen(dirty);
}

void ScribbleDoc::scribbleDone()
{
  // cursor update will now be handled by refreshUI
  if(scribbleMode)
    scribbleMode->scribbleDone();
  // process any received items that we're held back until local user input release
  if(scribbleSync)
    scribbleSync->processRecvBuff();
}

// handles update to strokes outside of the undo system - currently the only place this happens is COM update
//  by ScribbleArea::groupStrokes
void ScribbleDoc::strokesUpdated(const std::vector<Element*>& strokes)
{
  if(scribbleSync)
    scribbleSync->sendStrokeUpdate(strokes);
}

void ScribbleDoc::startAction(int pagenum)
{
  if(scribbleSync)
    scribbleSync->syncClearUndone();
  history->startAction(pagenum);
}

void ScribbleDoc::endAction()
{
  history->endAction();
  if(scribbleSync)
    scribbleSync->sendHist();
}
