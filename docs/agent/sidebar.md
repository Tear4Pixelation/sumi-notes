# The general-purpose sidebar

A panel with three views - the document's **outline**, its **layer table** and its **pages** (a thumbnail
grid with select mode, drag to move, delete and export: [page-management.md](page-management.md)) - over the
backends in [outlines.md](outlines.md) and [layers.md](layers.md); it is the first UI either feature has had. Opened from a button at the
left end of the **page ops** floating panel (Bookmarks is off the toolbar for now; Ctrl+B and View > Bookmarks still open it). Designed in the Penpot file
"General purpose sidebar" and specified in `SIDEBAR_SPEC.md`; that document is the rationale, this is
the summary. `syncscribble/sidebar.cpp`.

- **Pinned and floating are one widget in two parents, not two widgets.** Pinned, it is a flex
  sibling of the canvas inside `.sub-window-layout`, so the canvas is simply given the remaining
  width and `ScribbleView` re-centres the page in it - the "document stays centred" requirement
  needed no code at all. Floating, it is a child of `#main-container`'s box layout, anchored left or
  right, so it covers the canvas without resizing it. Which side it sits on is the same mechanism
  again: which end of the flex row, and which `box-anchor` when floating.
- **The reparent is deferred by a 1 ms timer.** Both toggles are pressed *inside* the sidebar, so
  reparenting from the press handler pulls the tree out from under the widget SvgGui is still
  dispatching to - seen as the panel vanishing instead of changing mode. `SvgGui::setTimer` asserts
  `msec > 0`, so it cannot be a zero-delay timer, and "was it open" is captured at schedule time
  rather than read inside the timer.
- **Dismiss-on-press-outside is a `Widget::eventFilter` on `#main-container`**, and the two
  mechanisms that look right are both wrong. `OUTSIDE_PRESSED` is delivered only to the widget that
  *captured* the press, so a panel in the tree never sees it. Pushing the sidebar onto SvgGui's menu
  stack (`showMenu` does take a plain `Widget*`) delivers `OUTSIDE_MODAL` but hands the panel to the
  menu machinery, which then closed it on presses *inside* it - its own buttons stopped working the
  moment it floated. An event filter is called on the way *down*, from the target up through its
  ancestors, so it sees presses the canvas would otherwise swallow. It **returns false**: swallowing
  the press makes the first click on any toolbar button only dismiss the sidebar.
- **One sizing trap, which has now caught five widgets here**: `prepareLayout()` reports a size only
  for the dimensions that are *not* fill, so a `fill` rect declares nothing and its parent sizes to
  its contents. The panel background is therefore `vfill` (its width is what widens the panel; an
  explicit `width` on a `<g>` is ignored by the box layout), each row's sizer is `hfill`, and so are
  the selector's and search box's `.field-bg` rects - the last two were found only after the selector
  rendered 12 px tall against a declared 49, twice in a row, because the constant was never reaching
  the layout. **When something here renders at the size of its contents, check the anchor before
  changing the number.** A separate trap: **a sizer rect must not be a flex item** - as a child of
  the actions row it made `space-between` spread the buttons against an invisible block, bunching
  them to one side; it is now a sibling inside a box container.
- **The insets are on the panel; the sidebar widget itself has zero margins.** ugui margins sit
  *outside* a widget's rect, so margins on the sidebar put its gutter where its own background cannot
  paint - and nothing repaints a strip the canvas has vacated, so the canvas's previous frame stays
  visible there (the page-number statusbar was left stranded beside the panel). The sidebar owns its
  whole column and fills it with `--canvas` (`.sb-backfill`), pinned only.
- Insets come from the toolbar's own `floatInset`/`floatTopInset`/`floatBtnSize`, which moved to
  `basics.h` for this - the design file's 12 is narrower than `floatInset` and left the sidebar out
  of line with the toolbar above it. The view selector's height is the one number here set from its
  *rendered* size (74 units -> 28 px) rather than from the design file, since it was picked by eye.
- **The canvas-facing edge takes no gutter at all** - `floatInset` earns its keep against the window
  edge, where it aligns the panel with the toolbar, but between the panel and the document it is only
  dead space. It follows the side the sidebar is on. That margin is visible **only when the document
  is zoomed past the viewport width**: at fit-page zoom the page is centred in whatever the sidebar
  leaves, so the ~200 px beside the panel is the centring, not this. Because the two horizontal
  margins are unequal, the panel is anchored `left vfill`/`right vfill`; left to the box layout's
  centring it splits the difference and drifts out of line with the toolbar.
- Design units map through `floatUIScale` like every other floating panel, verified by measurement
  rather than assumed: the tool buttons render the design's 64-unit button at 24.2 screen px, and the
  design's 372-wide sidebar comes out at 141 px, which is what it produces. Insets are derived from
  the toolbar's own geometry (`15 + 64 + 12`) rather than written as the literal 91.
- **Touch sizing (iOS, Android): `floatTouchUI` in `basics.h`.** On iOS a layout unit is one point (paintScale is
  the pixel ratio), so desktop's `floatUIScale = 0.5` made toolbar buttons 32 pt and the sidebar's bottom
  row (add, pin, side, search) a 12 pt icon that was also its whole hit target. With `SUMI_TOUCH_UI`
  (defaults to iOS and Android, all sizes: the panels size themselves in file-static initializers, so a tablet-only runtime check is not possible) the scale is 0.625, so the 64-unit design button is 40 pt,
  and the bottom row's cells become full toolbar buttons with toolbar-sized icons. Desktop values are
  unchanged by construction. It is compile time because every panel derives its geometry from these
  constants in file-static initializers. Preview it on Linux by building with
  `CXXFLAGS=-DSUMI_TOUCH_UI=1` (not `CFLAGS`, which the Makefile assigns; touch `basics.h` first, and
  again afterwards to rebuild without it), then `--screenDPI=163` for an 1180 pt landscape iPad or `234`
  for 820 pt portrait. In portrait the tools row still fits; the file buttons go to the overflow menu.
- The view is picked with **`Button::setMenu`**, not an `onClicked` calling `SvgGui::showMenu` - a
  menu must be parented to its button before it can be shown, and shown manually it never appeared.
- **Building the list uses `outline(true)`; the change check uses `outline(false)`.** With
  `outline(false)` alone the list held only pages already loaded, and pages load lazily as the view
  scrolls, so entries popped in one at a time and each one rebuilt the list. `outline(true)` loads every
  page of a delay-loaded document once, when the list is built for it; after that it loads nothing.
  `Document::outline(false)` counts a page unloaded under the memory limit from its cached title, so
  the check does not see entries vanish either.
- Colors are literals from the Penpot file, as the floating toolbars' are - this panel is part of the
  same dark chrome and does not follow the document theme.
- **The toolbar button's glyph names the edge the sidebar is docked to** - reicon `sidebar-left`/
  `sidebar-right` as `ic_menu_sidebar_left`/`_right`, swapped by `MainWindow::updateSidebarButton()`
  whenever the side changes, and the action is checkable so the button also reads as open/closed.
  The left/right variants are therefore not decorative: they say where the panel will appear. Note
  the sidebar's *own* side toggle shows the **opposite** glyph, because that one names the side it
  would move to rather than where it is. The split view's L/R icons used to be these same glyphs, so the
  two buttons sat side by side looking identical; `ic_menu_split_lr`/`_rl` are now `sidebar-top`/
  `sidebar-bottom` rotated -90 degrees (`ROTATE` in `scribbleres/reicon_import.py`), so the four split
  icons read as one family and none of them is the sidebar's.
- It goes through `addTBWidget`, so the overflow menu item is generated automatically when the panel
  is too narrow, rather than being a second hand-placed entry.
- **Pinned, it sits *under* `#main-toolbar-container`**, and ugui hit-tests a container anywhere in its
  bounding box (`nodeAt(p, false)`), not just on its children. The toolbar row's box is the full window
  width (its stretches) by the tallest panel, so opening a tool's options row made a band that swallowed
  the top of the view selector - only its bottom part responded. The row's layout containers and
  stretches are now `Widget::hitTransparent` (ugui, honoured by `SvgGui::widgetAt`), which also lets
  presses between the floating panels reach the page.
- **It refreshes from `MainWindow::refreshUI()`** via `refreshIfChanged()`, which compares a signature
  of what the list shows (document, outline entries, layer table, current layer) and rebuilds only on a
  change, since `refreshUI` runs after every stroke. Without it the list kept the previous document's
  contents after opening another, ignored undo and peers' edits, and came up empty when open at
  startup: `MainWindow`'s constructor opens it before there is an `SvgGui` to build rows through.

- **A layer row's click and its lock toggle run on a 1 ms timer.** Either can let go of the selection,
  whose `refreshUI` rebuilds this list synchronously - deleting the button still dispatching (ASan:
  use-after-free in the lock lambda when locking with a selection).
- **Right click / long press on a row** opens an ArrowPopup reparented onto that row (the tag browser's
  pattern, and its trap: `rebuildList()` detaches both popups before deleting rows, because their own
  items rebuild the list mid-click). Outline: Rename, Move to Top Level, Delete (removes the entry,
  never the page). Layer: Rename, Delete (disabled on the last layer; `removeLayer` moves the ink onto
  the current layer, it does not delete it). Rename reuses `TagNameDialog`, and passes the page's
  *stored* level back, not the normalized one.
- **Dragging an outline row onto another nests it there** (`ScribbleDoc::nestOutlineEntry`, one undo
  step, tested by `ScribbleTest::outlineNestTest()`, mutation-checked). Nesting is positional, so it
  moves pages: the entry's section - its page through the page before the next entry not nested under
  it, untitled pages included - goes to the end of the new parent's section, and its subtree's levels
  shift by the same amount. Two traps: **clones are inserted, never the deleted pages** (a
  `PageDeletedItem` frees its page when history is discarded - paste does the same), and **levels are
  set after the move**, since `<outlinechanged>` names its page by its *current* number. Moving to the
  top level goes after the top-level section rather than staying put, or it would adopt its following
  siblings. Collapsed state is cleared on a drop: it is keyed by page number and the pages just moved.
- **Dropping an entry on its own parent takes it back out** (`OUTLINE_OUTDENT`): one level up, placed
  straight after the parent's section, so it becomes the parent's next sibling. Without this, dropping
  on the parent would only re-file it as the last child, and the one way out was the menu.

Known gaps: the lock toggle is drawn on every layer row (dimmed when unlocked) rather than only on
locked ones as designed, because the design leaves an unlocked row no way to lock it; the layer
preview is a plain block, since a layer spans every page and has no single thumbnail; hide exists in
`LayerList` but has no place in the design yet; and a drag does not autoscroll a list longer than the
panel.

- **Layers are reordered by a grip** (`ic_menu_reorder`, reicon `reorder`) at the trailing end of each
  layer row, hidden when there is only one layer. Drag it onto another row and the layer takes that row's
  place (`ScribbleDoc::moveLayer`, one undo step; the target shifts one step towards where the dragged
  layer came from, in either direction). It is a visible handle rather than a gesture on the row because
  a reorder is vertical, and on a tablet a vertical drag on a row can only ever scroll the list (see
  below) - a user on an iPad could not find any way to reorder. Pressing the grip never picks the layer.
  On touch builds its hit target is a full `floatBtnSize` toolbar cell, as the bottom row's are. Keys are
  layer ids, mapped to table indices at the drop, so a search that hides rows does not skew them.

## Dragging list rows (`rowdrag.cpp`)

`RowDrag` is the one drag-onto-a-row gesture, shared by the sidebar's outline and the tag browser
(tag onto tag = subtag, onto its own supertag = back out one level, listed right below that supertag
via `reparentTag`'s `afterId`, onto All Documents = root tag, `TagDocList::moveTagUnder`). A press then a
release is still the Button's click; past `DRAG_START_DIST` it is a drag and the release is swallowed.
With a **pen or finger the list's `ScrollWidget` owns the gesture** and passes a drag to the row only
when it starts sideways (its filter's "axis it cannot scroll" rule), so vertical drags still scroll; for
the same reason the row must accept motion events even below the threshold, or the scroll view takes the
gesture back. **`addRow(row, key, grip)`** instead makes a grip button the only place the row is picked
up from: the grip gets ugui's `draggable` class, which `ScrollWidget`'s filter checks *before* the axis
rule, so a drag on the grip passes through in any direction while a drag anywhere else on the row still
scrolls. Only the mouse path has been exercised (agent-pointer is a mouse); the touch path through
`draggable` is ugui's own and was read, not driven. The drop is delivered on
a 1 ms timer, since it rebuilds the list that owns the row still dispatching; keys must therefore stay
valid across a rebuild - page numbers for the outline, and for tags a per-window id table that is never
cleared. An optional `zoneAt`/`onDropZone` pair lets a target have parts: the Pages view uses it so the left half of
page 1 means "before" (`.drop-before` instead of `.drop-target`); without them nothing changes. Feedback is
class-based: `.drop-target` fills the row's sizing rect with `--checked` at
`fill-opacity: 0.3` (a stroke nudged the list, as it grows the bounds), `.dragging` turns the label
`--text-weak` (`opacity`, as a CSS rule or an attribute, did not show). Both rules are in the sidebar
section of `ugui/theme.cpp` and match only `.sb-row`; until they were added there was no rule at all, so
a drag showed nothing. The tag browser's rows (`.tag-row`) still have none.
