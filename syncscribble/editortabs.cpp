// Editor tabs (docs/agent/editor-tabs.md): the ScribbleApp side of the open-document list in tablist.h.
//
// Ownership is the one rule everything here keeps: scribbleDocs holds exactly the documents shown in a
//  pane, as it always has, and a document no pane shows is owned by its tab (EditorTab::doc) or deleted.
//  A ScribbleDoc without a pane has no activeArea, which much of ScribbleDoc dereferences, so nothing
//  that iterates scribbleDocs may ever reach one.

#include "scribbleapp.h"
#include "scribbledoc.h"
#include "scribblearea.h"
#include "scribblewidget.h"
#include "scribblesync.h"
#include "scribbleconfig.h"
#include "bookmarkview.h"
#include "mainwindow.h"
#include "sidebar.h"
#include <algorithm>

bool ScribbleApp::tabsEnabled() const
{
  // iOS outside library mode edits through UIDocument streams, which cannot be reopened from a path
  return !(PLATFORM_IOS && !libraryManaged());
}

int ScribbleApp::activeTabIndex() const
{
  return activeArea() ? tabs.findDoc(activeDoc()) : -1;
}

static TabViewState viewOf(ScribbleArea* area)
{
  TabViewState view;
  DocPosition pos = area->getPos();
  view.pagenum = pos.pagenum;
  view.x = pos.pos.x;
  view.y = pos.pos.y;
  view.zoom = area->getZoom();
  return view;
}

// The pane gives up its document; where it was looking goes to the document's tab, so showing the tab
//  again - from memory or reloaded from disk - comes back to the same place.
ScribbleDoc* ScribbleApp::detachDoc(ScribbleArea* area)
{
  ScribbleDoc* doc = area->scribbleDoc;
  if(!doc)
    return NULL;
  int idx = tabs.findDoc(doc);
  if(idx >= 0 && doc->document)
    tabs[idx].view = viewOf(area);
  doc->removeArea(area);
  if(idx >= 0 && doc->nViews == 0)
    tabs[idx].lastShownMs = mSecSinceEpoch();
  return doc;
}

void ScribbleApp::attachDoc(ScribbleDoc* doc, ScribbleArea* area, const TabViewState* view)
{
  doc->addArea(area);
  if(area == mActiveArea || !doc->activeArea)
    doc->activeArea = area;
  if(view && view->isValid() && doc->document)
    area->restoreView(view->pagenum, Point(view->x, view->y), view->zoom);
}

// a document neither a pane nor a tab holds on to is gone; the caller has already offered to save it
void ScribbleApp::releaseDocIfOrphan(ScribbleDoc* doc)
{
  if(!doc || doc->nViews > 0 || tabs.findDoc(doc) >= 0)
    return;
  scribbleDocs.erase(std::remove(scribbleDocs.begin(), scribbleDocs.end(), doc), scribbleDocs.end());
  if(activeArea() && activeDoc() && activeDoc() != doc)
    bookmarkArea->setScribbleDoc(activeDoc());
  delete doc;
}

// scribbleDocs = the documents in the panes, in pane order; anything that drops out of it with no tab
//  to keep it is deleted
void ScribbleApp::rebuildShownDocs()
{
  std::vector<ScribbleDoc*> shown;
  for(ScribbleArea* area : scribbleAreas) {
    if(area->scribbleDoc && std::find(shown.begin(), shown.end(), area->scribbleDoc) == shown.end())
      shown.push_back(area->scribbleDoc);
  }
  std::vector<ScribbleDoc*> previous;
  previous.swap(scribbleDocs);
  scribbleDocs = shown;
  for(ScribbleDoc* doc : previous) {
    if(std::find(shown.begin(), shown.end(), doc) == shown.end())
      releaseDocIfOrphan(doc);
  }
}

void ScribbleApp::showUntitledInArea(ScribbleArea* area)
{
  ScribbleDoc* doc = new ScribbleDoc(this, cfg, scribbleMode);
  detachDoc(area);
  attachDoc(doc, area, NULL);
  doc->newDocument();
  rebuildShownDocs();
}

// no prompt: what a tab's document needs at the moments the tabs save (closing, exit)
bool ScribbleApp::saveTabDoc(ScribbleDoc* doc)
{
  if(!doc || !doc->document || !doc->isModified() || !doc->fileName()[0])
    return true;
  return doc->saveDocument();
}

bool ScribbleApp::showTabInArea(int idx, ScribbleArea* area)
{
  if(idx < 0 || idx >= tabs.size() || !area)
    return false;
  ScribbleDoc* prev = area->scribbleDoc;
  ScribbleDoc* next = tabs[idx].doc;
  if(prev && prev == next)
    return true;
  std::string path = tabs[idx].path;
  TabViewState view = tabs[idx].view;
  TabViewState prevView = (prev && prev->document) ? viewOf(area) : TabViewState();
  bool fresh = !next;
  if(fresh)
    next = new ScribbleDoc(this, cfg, scribbleMode);
  detachDoc(area);
  // attached before loading: openDocument() positions the views it has, and repaints the bookmarks of
  //  the active pane's document, which must not be NULL meanwhile
  attachDoc(next, area, NULL);
  if(fresh) {
    Document::loadresult_t res =
        FSPath(path).exists() ? next->openDocument(path.c_str()) : Document::LOAD_FATAL;
    if(res == Document::LOAD_FATAL) {
      next->removeArea(area);
      delete next;
      if(prev)
        attachDoc(prev, area, &prevView);
      // a tab whose file is gone or unreadable is closed rather than left to fail again
      int stale = tabs.find(path);
      if(stale >= 0)
        tabs.remove(stale);
      messageBox(Warning, _("Error opening document"), fstring(_("\"%s\" could not be opened."), path.c_str()));
      refreshUI(activeDoc(), UIState::SetDoc);
      return false;
    }
    // the tab list is unchanged - nothing ran an event loop since idx was read - but look it up by path
    //  anyway, as everything after a load does
    int loaded = tabs.find(path);
    if(loaded >= 0)
      tabs[loaded].doc = next;
  }
  if(view.isValid())
    area->restoreView(view.pagenum, Point(view.x, view.y), view.zoom);
  rebuildShownDocs();  // deletes the document left behind if it was untitled
  if(area == activeArea()) {
    setWinTitle(next->fileName());
    repaintBookmarks(true);
  }
  updateSplitLabels();
  // A document kept in memory may have been changed on disk meanwhile; one just loaded cannot have been.
  //  checkExtModified() reloads it (it is unmodified - it was saved when it was left) and says so.
  if(!fresh)
    checkExtModified(next);
  refreshUI(activeDoc(), UIState::SetDoc);
  return true;
}

bool ScribbleApp::switchToTab(int idx, ScribbleArea* area)
{
  if(!area)
    area = activeArea();
  if(idx < 0 || idx >= tabs.size() || !area)
    return false;
  if(area->scribbleDoc && area->scribbleDoc == tabs[idx].doc) {
    setActiveArea(area);
    return true;
  }
  std::string path = tabs[idx].path;
  setActiveArea(area);
  // switching away is one of the moments a tab is saved (savePrompt still asks, an untitled document
  //  still offers Save/Discard/Cancel, exactly as when opening another document always did)
  if(!maybeSave())
    return false;
  idx = tabs.find(path);  // a save prompt runs an event loop
  if(idx < 0 || !showTabInArea(idx, area))
    return false;
  onLoadFile(activeDoc()->fileName());
  return true;
}

bool ScribbleApp::closeTab(int idx)
{
  if(idx < 0 || idx >= tabs.size())
    return false;
  ScribbleDoc* doc = tabs[idx].doc;
  std::string path = tabs[idx].path;
  // closing is one of the moments a tab is saved
  if(doc && doc->isModified()) {
    if(doc->nViews > 0) {
      setActiveArea(doc->activeArea);
      if(!maybeSave())
        return false;
    }
    else if(!saveTabDoc(doc)) {
      messageBox(Warning, _("Save error"), _("An error occurred saving the document.  Please try saving"
          " to a different location.  Contact support@styluslabs.com if this error persists."));
      return false;
    }
  }
  idx = tabs.find(path);
  if(idx < 0)
    return false;
  int neighbour = tabs.neighbourAfterClose(idx);
  // neighbourAfterClose() indexes the list after the removal; read its path before removing
  std::string neighbourPath = neighbour >= 0 ? tabs[neighbour >= idx ? neighbour + 1 : neighbour].path : "";
  tabs.remove(idx);
  if(doc) {
    // A shown document is deleted by showTabInArea()'s rebuildShownDocs() as its last pane lets go of it,
    //  so `doc` must not be touched after the loop unless no pane held it (a background tab).
    bool wasShown = doc->nViews > 0;
    // every pane that shows it moves on; the second pane first, so the first pane's document is the
    //  last thing touched
    for(int ii = int(scribbleAreas.size()) - 1; ii >= 0; --ii) {
      ScribbleArea* area = scribbleAreas[ii];
      if(area->scribbleDoc != doc)
        continue;
      int next = neighbourPath.empty() ? -1 : tabs.find(neighbourPath);
      if(next < 0 || !showTabInArea(next, area))
        showUntitledInArea(area);
    }
    if(!wasShown)
      releaseDocIfOrphan(doc);  // a background tab: no pane held it
  }
  rebuildShownDocs();
  updateSplitLabels();
  onLoadFile(activeDoc()->fileName());
  refreshUI(activeDoc(), UIState::SetDoc);
  // the last tab: back to the library, as when the app starts with no document
  if(tabs.empty() && !activeDoc()->fileName()[0] && !activeDoc()->isModified())
    openOrCreateDoc(!PLATFORM_IOS || libraryManaged());
  return true;
}

void ScribbleApp::moveTab(int from, int to)
{
  tabs.move(from, to);
  refreshUI(activeDoc(), UIState::SetDoc);
}

ScribbleArea* ScribbleApp::areaAt(Point pos) const
{
  if(!win || pos.isNaN())
    return NULL;
  // By the panes' bounds rather than a hit test: this runs on every motion event of a drag, and RowDrag
  //  has already decided the point is off the sidebar.  The pane's container rather than its widget, so
  //  the split placeholder covering a fresh second pane still counts as that pane.
  for(ScribbleArea* area : scribbleAreas) {
    Widget* container = area->widget ? area->widget->parent() : NULL;
    if(area->scribbleDoc && container && container->isVisible() && container->node->bounds().contains(pos))
      return area;
  }
  return NULL;
}

static int splitStateForEdge(DropEdge edge)
{
  // the new pane is scribbleAreas[1]; H12/V12 put it below/right, H21/V21 above/left (toggleSplitView)
  switch(edge) {
  case DropEdge::LEFT: return MainWindow::SPLIT_V21;
  case DropEdge::RIGHT: return MainWindow::SPLIT_V12;
  case DropEdge::TOP: return MainWindow::SPLIT_H21;
  default: return MainWindow::SPLIT_H12;
  }
}

void ScribbleApp::previewTabDrop(Point pos)
{
  ScribbleArea* target = areaAt(pos);
  if(!target) {
    win->showTabDropPreview(NULL, -1);
    return;
  }
  if(win->splitState > 0) {
    win->showTabDropPreview(target, -1);  // the whole pane: the tab replaces what it shows
    return;
  }
  Rect bounds = target->widget->node->bounds();
  DropEdge edge = nearestDropEdge(bounds.left, bounds.top, bounds.width(), bounds.height(), pos.x, pos.y);
  win->showTabDropPreview(target, int(edge));
}

bool ScribbleApp::dropTabOnCanvas(int idx, Point pos)
{
  win->showTabDropPreview(NULL, -1);
  ScribbleArea* target = areaAt(pos);
  if(!target || idx < 0 || idx >= tabs.size())
    return false;
  if(win->splitState > 0)
    return switchToTab(idx, target);
  Rect bounds = target->widget->node->bounds();
  DropEdge edge = nearestDropEdge(bounds.left, bounds.top, bounds.width(), bounds.height(), pos.x, pos.y);
  std::string path = tabs[idx].path;
  // opens the second pane on the active document and makes it the active pane; the tab then replaces it
  //  there, so nothing is left to save - the active document stays in the first pane
  win->toggleSplitView(splitStateForEdge(edge));
  if(win->splitPlaceholder)
    win->splitPlaceholder->setVisible(false);
  if(win->splitState <= 0 || scribbleAreas.size() < 2)
    return false;
  int now = tabs.find(path);
  if(now < 0 || !showTabInArea(now, scribbleAreas[1]))
    return false;
  onLoadFile(activeDoc()->fileName());
  return true;
}

void ScribbleApp::updateSplitLabels()
{
  bool separate = scribbleAreas.size() > 1 && scribbleAreas[1]->scribbleDoc
      && scribbleAreas[0]->scribbleDoc != scribbleAreas[1]->scribbleDoc;
  for(ScribbleArea* area : scribbleAreas) {
    if(!area->widget)
      continue;
    if(separate)
      area->widget->fileNameLabel->setText(docShortName(area->scribbleDoc->fileName()).c_str());
    area->widget->fileNameLabel->setVisible(separate);
  }
}

void ScribbleApp::syncTabs()
{
  if(!tabsEnabled())
    return;
  // a loaded tab follows its document: Save As renames it, a document reset to untitled closes it
  for(int ii = tabs.size() - 1; ii >= 0; --ii) {
    ScribbleDoc* doc = tabs[ii].doc;
    if(!doc)
      continue;
    std::string name = doc->document ? doc->fileName() : "";
    if(name.empty()) {
      tabs.remove(ii);
      releaseDocIfOrphan(doc);
    }
    else
      tabs[ii].path = name;
  }
  // a pane showing a file no tab has yet: just opened, created, or saved under a name for the first time
  for(ScribbleArea* area : scribbleAreas) {
    ScribbleDoc* doc = area->scribbleDoc;
    if(!doc || !doc->document || !doc->fileName()[0] || tabs.findDoc(doc) >= 0)
      continue;
    std::string name = doc->fileName();
    // the help document is a scratch copy in the temp folder, not something to come back to
    if(!tempPath.empty() && StringRef(name).startsWith(tempPath.c_str()))
      continue;
    int existing = tabs.find(name);
    if(existing >= 0) {
      // a tab restored at startup, opened another way (the command line opens straight into the pane)
      if(!tabs[existing].doc)
        tabs[existing].doc = doc;
      continue;
    }
    EditorTab tab;
    tab.path = name;
    tab.doc = doc;
    tabs.insert(tab, area == activeArea() ? tabInsertAnchor : -1);
  }
  tabInsertAnchor = -1;
}

void ScribbleApp::restoreTabs()
{
  if(!tabsEnabled())
    return;
  for(const std::string& path : TabList::parse(cfg->String("openTabs", ""))) {
    if(tabs.find(path) < 0 && FSPath(path).exists()) {
      EditorTab tab;
      tab.path = path;
      tabs.tabs.push_back(tab);
    }
  }
}

// Background tabs are kept in memory for switching back quickly, and dropped once they have been out of
//  every pane for tabUnloadSecs; an unloaded tab is just its path and view, and is reloaded from disk.
void ScribbleApp::unloadIdleTabs()
{
  int64_t timeoutMs = int64_t(cfg->Int("tabUnloadSecs"))*1000;
  auto keep = [](const EditorTab& tab){
    // shown, unsaved (a save on leaving failed), or in a whiteboard session, whose peers keep editing it
    return tab.doc->nViews > 0 || tab.doc->isModified() || tab.doc->scribbleSync;
  };
  std::vector<int> idle = tabs.idleTabs(mSecSinceEpoch(), timeoutMs, keep);
  for(int idx : idle) {
    ScribbleDoc* doc = tabs[idx].doc;
    tabs[idx].doc = NULL;
    delete doc;
  }
  if(!idle.empty())
    refreshUI(activeDoc(), UIState::SetDoc);
}
