# Editor tabs

Every open document with a file is a **tab**; a pane (one, or two in a split) shows one tab's document.
The tabs are listed in the sidebar's **Tabs** view and reached from a toolbar button. Code:
`syncscribble/tablist.h/.cpp` (the list itself, no app dependencies), `editortabs.cpp` (`ScribbleApp`
methods: loading, showing, saving, closing, dropping on the canvas), `sidebartabs.cpp` (the rows),
plus hooks in `scribbleapp.cpp` (`doOpenDocument`, `closeSplit`, `onLoadFile`, quit/suspend, library moves).

## Ownership - the one rule

`scribbleDocs` keeps its old meaning: **the documents shown in a pane**, nothing else. A document no pane
shows is owned by its tab (`EditorTab::doc`) or deleted (`releaseDocIfOrphan`, `rebuildShownDocs`). This
is load bearing: a `ScribbleDoc` without a pane has `activeArea == NULL`, and `doCancelAction`,
`updateDocConfig`, the thumbnail in `saveDocument`, `getActiveMode` and more dereference it. Keeping
background documents out of `scribbleDocs` means none of the existing loops (`setMode`, autosave,
`checkExtModified()`, `appSuspending`, ...) can meet one. Those four `ScribbleDoc` spots were still made
NULL-safe, for the two cases that do touch a background document: a whiteboard peer editing it, and
saving it on exit (it is then saved without a thumbnail; its position is already in its cfg).

An **untitled** document is not a tab. Opening a file while one is shown opens over it, as before (the
usual Save/Discard prompt); Save As makes it a tab (`syncTabs()`, called from `onLoadFile`). The help
document (a temp copy) is never a tab.

## Opening, switching, closing

- `doOpenDocument(IOStream*)`: a path already in a tab switches to it (no reload, which would drop its
  undo history); any other file gets a **new `ScribbleDoc`** when the document being left is a tab (or is
  also shown in the other pane), so the old one stays open. New tabs go right after the active one
  (`tabInsertAnchor`). LOAD_FATAL puts the old document back where it was.
- `switchToTab()` = `maybeSave()` on the document being left (switching away is a save point; savePrompt
  and the untitled prompt behave as they always did) + `showTabInArea()`.
- `showTabInArea()` attaches *before* loading: `ScribbleDoc::openDocument` positions its views and repaints
  the bookmarks of the active pane's document, which must not be NULL meanwhile. A tab kept in memory gets
  `checkExtModified(doc)` after it is shown - verified: touching a loaded background tab's file and
  switching to it shows "Document reloaded".
- `closeTab()` saves first, then every pane showing it moves to the neighbour (`neighbourAfterClose`: the
  next tab, the previous one for the last). **Trap**: a shown document is deleted by `showTabInArea`'s
  `rebuildShownDocs()` as its last pane lets go, so `closeTab` must not touch it after the loop unless no
  pane held it (`wasShown`) - the first version was a use-after-free. Closing the last tab shows a blank
  untitled document and opens the library.
- `closeSplit()` no longer resets the second pane's document: it stays open as a background tab.
- Saving: switching away, closing, going to the library (`openDocument()` already did `maybeSave()`), and
  quit (`maybeQuit` also saves background tabs, which only a whiteboard peer can have changed). Nothing else.

## Memory

A background tab stays loaded; `unloadIdleTabs()` (timer started in `loadConfig()`, every quarter of the
timeout, 1-60 s) deletes documents out of every pane for longer than **`tabUnloadSecs`** (default 600,
0 = never). Kept regardless: shown, modified (a save failed), or in a sync session (`scribbleSync`). An
unloaded tab is its path plus `TabViewState` (page, corner position, zoom, captured in `detachDoc()`),
restored by `ScribbleArea::restoreView()` after the reload. Verified with `tabUnloadSecs=8`: a tab left
15 s showed no reload notice when its file was touched (it had been unloaded) and came back
pixel-identical at its 125 % zoom and scroll.

## Persistence

`saveConfig()` writes `openTabs` (paths joined by `:::`, like `recentDocs`) and `activeTab`;
`restoreTabs()` (after `initLibrary()`, not for `--out`) recreates them unloaded. `reopenLastDoc` still
decides whether a document is shown at start - if so, the active tab. **`--library DIR` disables config
saving** (by design, `scribbleapp.cpp`), so persistence can only be tried with a `write.xml` beside the
debug binary that sets `libraryPath`. View state is not persisted; the file's own saved position covers it.

## Sidebar: Tabs view and the temporary showing

`Sidebar::TABS` (rows in `sidebartabs.cpp`, same row shape as the outline). Tap = switch (deferred 1 ms,
timer on MainWindow because a temporary showing hides the sidebar and hiding drops a widget's timers);
`x` = close; grip + row are both draggable (`RowDrag::addRow(..., rowToo)`): the grip for vertical
reorders on touch, the row for sideways drags onto the canvas. `+` opens the library.

The toolbar **Tabs** button (`actionShow_Tabs`, reicon `files` as `ic_menu_tabs`, next to the Sidebar
button) calls `Sidebar::showTemporary(TABS)`: stores view/pinned/open, shows Tabs - floating if the
sidebar was closed - and writes nothing to the config. Any close (outside press, a tab picked, the button
again) runs `endTemporary()`, which puts all three back (the reparent deferred, as in `schedulePlacement`).
Picking a view while temporary only changes what is shown; pressing the pin is a real choice and becomes
the stored pin. **Trap**: the press-outside dismiss filter used to see the press on the toolbar button
itself, so the floating sidebar closed on press and the click reopened it - the button could never close
it (the plain Sidebar button had the same bug when floating). Both buttons carry class `sb-toggle`, which
the filter skips.

## Dragging a tab onto the canvas

`RowDrag` gained `onDragOutside`/`onDropOutside` (additive; unset = old behaviour). "Outside" means off
the list's owner widget entirely, decided from the one `widgetAt()` hit test the drag already does - each
costs ~4 ms in the ASan build and motion arrives every 8 ms, and a second hit test plus a relayout per
event put the debug build seconds behind a slow drag. The preview (`MainWindow::showTabDropPreview`,
`.tab-drop-preview` in ugui's theme, `--checked` at 0.3) is only touched when its pane or edge changes.
`ScribbleApp::areaAt()` uses pane container bounds. Unsplit, the drop opens the second pane on the
**nearest edge relative to the pane's size** (`nearestDropEdge`: left = `SPLIT_V21`, right = `V12`,
top = `H21`, bottom = `H12`) and shows the tab there; split, it replaces the document of the pane under the
point. All four edges and the onto-a-pane case were driven in agent-display.

## Tests

`scribbletest/tabstest.cpp` (`runTabTests()`, standalone command in its header): neighbour on close, move
in both directions, insert after the active tab, idle selection with the veto and a zero timeout, the
config round trip, and the relative nearest edge (two points where relative and absolute disagree).
Six mutations each fail it: absolute distance, neighbour off by one, move landing before the target,
insert ignoring the anchor, idle ignoring the veto, parse keeping empty entries.

## Gaps

- With the temporary showing open, the Sidebar button closes it (restoring the stored state) rather than
  switching to the normal sidebar in one press.
- A whiteboard document left in the background and saved on exit gets no thumbnail; deleting a closed tab's
  document that has a sync session relies on `closeDocument()`'s disconnect, as `newDocument()` did.
- iOS outside library mode (UIDocument streams) has no tabs (`tabsEnabled()`): old single-document code path.
- Touch/pen drags of tab rows go through ugui's `draggable` path, read but not driven (agent-pointer is a mouse).
- The `.drop-target`/`.dragging` row feedback rules that `sidebar.md` describes are uncommitted work in
  the main checkout's ugui, not in this branch's ugui pin, so in this branch a reorder drag shows no row
  highlight; the canvas preview rule was committed to ugui separately.
