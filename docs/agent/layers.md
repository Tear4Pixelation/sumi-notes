# Layers

A document carries a **layer table**; every element carries a layer **id**. A layer can be locked or
hidden, and content on one cannot be selected, erased, moved or otherwise edited. Designed in
`LAYERS_INVESTIGATION.md`; that document is the rationale (including why the obvious design is
wrong), this is the summary. The entry points are
`ScribbleDoc::addLayer`/`removeLayer`/`setLayerLocked`/`setLayerHidden`/`setCurrentLayer`/
`moveLayer`/`moveSelToLayer`; the UI over them is the layers view of [sidebar.md](sidebar.md), which so far reaches `addLayer`, `setCurrentLayer`, `setLayerLocked`, and through a row's
right-click menu `setLayerName` and `removeLayer`.

**A layer is a tag on the element, not a `<g>` in the SVG.** `Element::layer()` serializes as
`__layer`, the same custom-attribute convention as `__shape`/`__comx`/`__timestamp`. That buys the
whole feature for free in three places that would otherwise each need work: the **file format** (no
new construct; an older Write build opens the document and sees one flat page, which is exactly
right), **sync** (`<addstroke>` round-trips the node through `SvgWriter`/`SvgParser`, so the tag
rides along with no protocol change), and **undo** (`StrokeAdded`/`Deleted`/`Transform` are
node-based and needed no change at all). The alternative - a real `<g>` per layer - was declined:
`Page::loadSVG` does `selectFirst(".write-content")` and `Selection::sourceNode` is a single
`Element*`, so it would touch a dozen call sites *and* an older build would read only the first
layer and drop the rest on save. The attribute has no such failure mode.

- **The one rule for new content is in `Page::addStroke`**, which stamps the current layer unless the
  caller passes one. Every route by which the user adds anything - a finished stroke, a shape, a
  pasted selection, an **inserted image**, a scanned page - funnels through there, so this is one
  rule rather than one per creation site. The callers that must *preserve* a layer pass it
  explicitly: free-erase subpaths (the pieces of an erased stroke belong to that stroke's layer, not
  the pen's) and the cropped copy in `MODE_CROPSEL`.
- **Layers stack, and the mechanism is the insertion point, not the tree.** `contentNode`'s children
  are kept partitioned by layer z-index; `addStroke` inserts at the end of its own layer's run. Undo
  and sync are unaffected because both express position as a *sibling node*, which this only chooses
  differently. `DRAW_UNDER` had to become "under this layer's content" (`layerFirstElement`) rather
  than "first child of the page", or it would drop a stroke outside its own layer's run.
- **The edit gate is `Page::isEditable()`, and it is consulted in exactly three places.**
  `Selection::doSelect` covers rect/lasso/ruled select, **the stroke eraser and the ruled eraser**
  (both are implemented as selections), and insert-space/reflow (which act on a selection built the
  same way); `selectAll` and `invertSelection` need their own, since they do not go through
  `doSelect`; and the **free eraser** needs its own, because it is the one erase path that builds no
  `Selection`.
- **An unknown layer id fails open** - editable, visible, bottom of the z-order. A layer can be
  deleted while elements still point at it (undo, a peer on an older table, a hand-edited file), and
  ink that cannot be selected, erased *or seen* is ink the user has no way to recover.
- **A lock guards a layer against work done from *other* layers, not against itself.**
  `LayerList::isEditable()` treats a locked layer as editable while it is `currentId`, so picking a
  locked layer in the sidebar is how it is edited, and picking any other layer puts it out of reach
  again. `setCurrent()` therefore accepts a locked layer and refuses only a hidden one, and locking
  the current layer leaves the pen on it. Leaving a locked layer clears the selection
  (`ScribbleDoc::setCurrentLayer`), since whatever was selected on it has just become uneditable;
  locking a layer clears it only when that layer is not current. Hiding the layer being drawn on
  still moves `currentId`, preferring an unlocked layer.
- **Ids are never reused, and the counter is serialized** (the leading `#<nextId>` record). Undoing
  the delete of a layer restores elements carrying its id; if a layer added in the meantime had been
  given that id back, those elements would silently join it. The max-id fallback in `parse()` covers
  a table written without the counter, but **only** when the removed layer was not the highest - which
  is why the test removes the top one.
- **z-order is the position in the table, never the id.** Reordering layers must not rewrite a single
  element; `ScribbleDoc::restackLayers()` re-sorts each loaded page's children by z-index, stably, so
  the order *within* a layer survives.
- **`StrokeLayerItem` records the sibling position as well as the layer id**, because the two are not
  independent: changing an element's layer restacks it, so restoring only the id would leave it
  somewhere the user never put it. `Page::moveToLayer()` returns the successor the element had,
  which is exactly what the item needs.
- **Three kinds of state, handled differently.** Which layer an *element* is on is on the element, so
  it undoes and syncs like any other stroke edit (`StrokeLayerItem`). The *table* (names, order,
  locked, hidden) is persisted in the per-document config as the `layers` value - the same mechanism
  the theme recipe uses, so no new file-format construct - and every edit to it is a
  **`LayerTableItem`**, so it is an undo step and syncs. Which layer is *current* (`currentLayer`) is
  neither: it is where this user's pen is, and two people on a whiteboard draw on different layers.
- **`LayerTableItem` records one layer, never the whole table** (`LayerList::layerState()`/
  `setLayerState()`: present or not, info, and the id of the layer directly below). Sync replays a
  local edit over a peer's by undoing and redoing it, and a whole-table snapshot redone over a peer's
  concurrent edit to some *other* layer would silently revert it. Position is "the layer below" rather
  than an index for the same reason - a peer's add shifts every index above it - and a layer whose
  layer-below has since vanished lands on top. `layertest.cpp` replays exactly that rebase.
- **Every table mutator goes through `ScribbleDoc::editLayer()`**, which runs the edit on a copy and
  applies only the resulting state of that one layer through `setLayerState()` - the same path undo,
  redo and a peer's item take. So what the undo item records and what the edit did cannot drift, and
  letting go of a selection on a layer that just went out of reach (locked while not current, hidden,
  removed) happens once, for local and remote edits alike. `removeLayer()` puts its `LayerTableItem`
  *last* in the action, after the `StrokeLayerItem`s moving its content off, so undo brings the layer
  back before the elements are moved back onto it.
- **Layers added during a whiteboard session get random ids** at or above
  `LayerList::SHARED_ID_BASE` (2^24), since two clients adding at once would otherwise both take
  `nextId()` and their strokes would merge. They never advance the counter, and `parse()`'s max-id
  fallback ignores them, or one would push the counter to the top of the range.
- **A client joining a whiteboard late gets the table and the theme in a snapshot**
  (`<layersnapshot>` plus a `<themechanged>`, at the head of `startSession()`'s opening block, ahead
  of the pages). Without it no amount of syncing edits helps a joiner, because both live in the
  config rather than in any page. It is applied outside the undo system, like those pages, and keeps
  the joiner's own current layer.
- Visibility is expressed as SVG `display` on the nodes (`Page::applyLayerState`), because drawing is
  a tree walk with no access to the table. It is applied at every `loadConfig`, at every table
  change, at the end of `Page::loadSVG` - a delay-loaded page arrives after the table has been
  read, so it has to be done there too - and whenever a stroke is put back into a page
  (`StrokeAddedItem::redo`, `StrokeDeletedItem::undo`). That last one is how a peer's stroke on a
  layer *this* client has hidden arrives hidden, and how undoing the delete of a stroke whose layer
  was hidden in the meantime keeps it hidden.
- `Document::applyLayerState()` deliberately touches **only loaded pages**; `removeLayer()` is the
  one operation that calls `ensurePagesLoaded()` first, because an unloaded page can still hold
  elements on the layer being removed and they must be reassigned undoably rather than orphaned.

Known gaps: hiding a layer in a large delay-loaded document only
affects loaded pages until the rest are loaded; and `setLayerHidden` and `moveLayer` have no UI yet
(see the sidebar's own gaps in [sidebar.md](sidebar.md)).

**Move to Layer** is a labeled dropdown on the selection popup (`MainWindow::refreshSelPopup()`, rebuilt
on every open, never while open). The target may be locked or hidden - filing ink onto a locked layer is
how it is filled - and `moveSelToLayer()` then lets go of the selection. The button is added with
`addWidget`, not `Menubar::addButton`, whose release handler closes the menu tree and would take the
layer list down with it. The popup is rounded like the floating panels, and `.menu.sel-popup` swaps the
menu's all-round shadow for one cast downwards: ugui's `box-shadow` takes offset, blur and (negative)
spread, and `border-radius` rounds it.
