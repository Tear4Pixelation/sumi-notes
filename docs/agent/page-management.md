# Page management (the sidebar's Pages view)

A third sidebar view, next to Outline and Layers ([sidebar.md](sidebar.md)): the document's pages as a
thumbnail grid, two per row, each with its number. Tap goes to the page; the current page has an accent
ring and number. A **Select** button (in Add's place on the bottom row) enters select mode, which works like
the document browser's ([document-library.md](document-library.md#select-mode-browser)): ring and check badge
on each card, tap toggles, and a floating bar ([ui-floating-bar.md](ui-floating-bar.md)) with the count,
Delete, Export PDF, Share, Export PNG and Done. Dragging a thumbnail onto another moves pages; right click /
long press on a thumbnail (outside select mode) offers the same four actions for that one page.
`syncscribble/sidebar.cpp` (the `Pages view` section), backend in `ScribbleDoc`, exports in `ScribbleApp`.

## Moving pages

- **A drop puts the page(s) straight *after* the page dropped on.** The one way to the front: the leading
  (left) half of page 1 means *before* it. Feedback is an accent bar on the edge where they will land -
  the trailing edge of the target, or the leading edge of page 1 (`.sb-drop-after`/`.sb-drop-before`).
  This came from extending `RowDrag` with an optional `zoneAt` (which part of the target the pointer is
  over) and `onDropZone`; zone 0 keeps the old `.drop-target` class, so outline and tag rows are unchanged.
- In select mode a **selected** thumbnail carries the whole selection, in document order (not the order
  tapped); dropping it on another selected page is refused. An unselected one carries only itself.
- `ScribbleDoc::movePages(pages, after, &dest)`: one undo step (`MULTIPAGE`), **clones inserted, never the
  deleted pages** - the `PageDeletedItem` trap from [sidebar.md](sidebar.md); `clonePage()` is now shared with
  `nestOutlineEntry`. Refused (false, nothing recorded) when `after` is one of the moved pages, out of
  range, or the pages already sit there. Sync carries it as ordinary page add/delete items.
- **`movedPageOrder()` is the single statement of where pages go**; `movePages` reads its destination off
  that order. A first version computed the destination separately, so a mutation of `movedPageOrder`
  failed only 3 checks (the no-op detection); now the same mutation fails 12.
- After a move the selection follows the clones by position (`dest`), and `ScribbleDoc::gotoPage(dest)`
  runs so the page-number strip is refreshed - the drop arrives on RowDrag's 1 ms timer, outside any input
  event, and `area->gotoPage` alone left it showing the old number.

## Deleting

`ScribbleDoc::deletePageList()` marks the pages and calls the existing `deletePages()`, which already keeps
a document that loses every page valid (a fresh blank page inside the same undo step). One undo step; no
confirmation, since it is undoable (the browser's delete is not, which is why that one asks).

## Exports (`ScribbleApp::exportPagesPDF/sharePagesDocument/exportPagesPNG`)

- **PDF**: `writePDF(strm, pages)` takes a page list now (empty = whole document). Internal links are
  remapped to the PDF's page numbers; a link to a page left out goes nowhere. Through `FilePicker::saveFile`
  like Export PDF.
- **Share** writes a new Sumi document of just those pages (`ScribbleDoc::savePagesCopy`): clones in a bare
  `Document`, this document's config copied (theme, layer table, ruling defaults, cover) with the position
  reset and the library `tags` cleared (ids mean nothing elsewhere), a fresh page-tag summary and a
  thumbnail of its first page. Mobile hands it to the share sheet (`sendFile`). **Desktop has no share
  sheet, so it goes through the save dialog instead** (an extension is appended if the name has none,
  since `Document::save` picks the format by extension).
- **PNG** renders each page with `renderPageImage()`, the code Send Page Image used (now shared): page size
  times `paintScale`, rule lines per `sendRuleLines`. Desktop: one save dialog; a single page is written
  at the chosen name, several as `name-N.png` beside it, N the page number. Mobile: all files in one share
  sheet - new `sendFiles()`, `iosSendFiles()` (UIActivityViewController with several URLs) and Android
  `sendDocuments()` (`ACTION_SEND_MULTIPLE`).
- wasm downloads every file (`jsSaveFile`).
- Every action from the bar or the row menu runs on a 1 ms timer: a file dialog pumps events, and a refresh
  let through would rebuild the grid the menu is parented to.

## Thumbnails

- Drawn from the in-memory pages by `ScribbleDoc::renderPageThumbnail()` (the page-tag card renderer, made
  public and shared), at the cell width times `paintScale`, in the document list's frame
  (`Cover::framedImage()`, moved out of `tagdoclist.cpp`). A page not rendered yet shows a placeholder: its
  paper color in its own shape, outlined like a thumbnail. The paper color is document data, not chrome,
  hence not a token.
- **Lazy, visible-first**: a timer renders what is on screen, then one screen either side, nothing further,
  with a 12 ms budget per event-loop turn; scrolling (`ScrollWidget::onScroll`) asks for more. A 300-page
  document renders only the first screens' worth on open.
- **Trap: do not ask every cell for its bounds.** The first version tested `cell->node->bounds()` for all
  cells before the budget check; with 300 pages under ASan that alone used up the budget, so each turn
  returned before rendering anything and the timer spun at 95% CPU with blank placeholders. Visible rows are
  now worked out from the first cell's position and the row pitch, and a turn always renders at least one.
- **Trap: the thumbnail timer is a 1 ms periodic timer whose ticks take 12 ms or more, and ugui's
  `SvgGui::processTimers()` used to make it starve everything else.** It advanced a periodic timer by one
  period per tick (`nextTick += period`) and fired it again while `nextTick <= now`, so a timer slower than
  its period fell further behind on every tick (12 ms of work per 1 ms of its clock), and one
  `processTimers()` call ran its whole backlog back to back. Every other timer waited until that lagging
  clock reached its own due time. Seen in real use as three things at once while thumbnails were being
  rendered: no fling on the canvas (its animation timer starved), sidebar actions that showed their press
  but did nothing for seconds (they run on 1 ms timers), and page switches of about 10 s (input queued
  behind the blocked loop). Measured in agent-display (ASan build, 60 dense pages, Pages view opened,
  then three next-page clicks 1 s apart): **before**, one `processTimers()` call fired the thumbnail timer
  24 times in a row and blocked for 7071 ms; the clicks were handled 7339/6310/5277 ms after they were sent
  and animation timers ran 6.9 s late. **After** (ugui: a periodic timer that has fallen behind is
  rescheduled from the time its callback returned, so missed ticks are skipped): every call fires one
  thumbnail tick (about 0.5 s for these pages in the ASan build, with other builds competing for the CPU),
  and the clicks were handled 642/876/1225 ms after they were sent. Pinned by
  `ScribbleTest::timerBacklogTest()`; with the reschedule removed it fails. The ugui change is the fix, not
  the thumbnail code: any periodic timer whose callback can outlast its period (camera polling, a slow
  frame during a fling) would back up the same way. What remains is the cost of one thumbnail per turn,
  which input waits behind.
- **Cache keyed by `Page::uid` + `Page::revision`**, both new. `revision` is bumped wherever the undo system
  changes `dirtyCount` (and on a layer restack) and is never reset; `dirtyCount` is zeroed by a save and
  counted back down by undo, so it cannot tell two versions apart. `uid` is unique for the process; a
  `Page*` can be reused by the next allocation. Selection is by uid too. The cache is pruned to the
  document at every rebuild and cleared when the layer table changes (hidden layers change every thumbnail).
- `refreshIfChanged()` rebuilds only when the page order changes; a drawn-on page keeps its old image until
  the new one is rendered, so nothing flickers. A move re-renders the moved pages (clones have new uids).
- **Trap: `SvgContainerNode::removeChild()` returns the node *after* the removed one**, not the removed one.
  `delete holder->removeChild(old)` freed the ring rect still in the tree (ASan use-after-free on the next
  layout).

## Layout

- Geometry from the sidebar's own constants: two `(sbWidth - 3*sbPad)/2` cells, frame `PREVIEW_ASPECT` tall,
  the page fitted and bottom-aligned as page cards are. The badge is scaled down from the browser's (18 on
  an ~84 unit card instead of 24 on ~150).
- **The select bar is wider than the panel** (about 250 units against 186), so it floats over the canvas
  beside the panel, `sbEdgeInset` from it on whichever side the panel is, 43 from the bottom as in the
  document list - which also keeps it off the page-number strip. It is a child of `#main-container`; the
  floating sidebar's dismiss filter treats presses on it as inside. The `.tagdoclist` selectbar and
  selection-mark rules in `ugui/theme.cpp` were widened to `.page-selectbar` / `.gp-sidebar .sb-page-cell`
  rather than duplicated, so no new color literal was added; the bar follows the light theme through the
  `--floating-*` tokens. The panel itself stays the sidebar's fixed dark chrome.
- In Pages view the bottom row is Select, Pin, Side; Add and Search are hidden (a page is added from the
  canvas; thumbnails have no text to search). The view selector's icon now follows the view.
- Select mode survives pin/side changes and closing the sidebar; leaving the Pages view or switching
  documents ends it. Entering/leaving rebuilds the grid (only select-mode cells carry ring and badge), from
  the cache, so nothing is rendered again.

## Verified (agent-display, 12- and 300-page documents)

Grid, current-page ring, tap to navigate (strip updates), drag after-target (2 onto 4 -> 1 3 4 2), front
drop (onto page 1's left half), multi-select drag carrying 2,5,8 after 10 in document order, undo of
moves, select mode and bar, Delete + undo, row menu Delete Page and Export PNG, PDF of 3 selected pages
(`pdfinfo`: 3 pages, rendered in order 2 5 8), Share -> `export.svgz` (3 `write-page`s, right colors,
`tags` empty), PNG -> `export-7/8/9.png`, floating mode, right side, light theme (`--uiTheme=2`), 300 pages
lazy (CPU idle afterwards, more rendered on scroll). Screenshots were in the agent's scratch folder.

Tested by `ScribbleTest::pageMoveTest()`: after-target, front, end, neighbour swap, document order of a
multi-page move, refusals, outline entry travelling with its page, one-step undo/redo of moves, deleting a
list and deleting every page. Mutation-checked: inserting before the target fails 12 checks, reversing the
moved pages' order fails the multi-page check.

## Gaps

- Touch drag is ugui's sideways-first rule: on a tablet a thumbnail is picked up by moving sideways first
  (vertical drags scroll). Only the mouse path was driven. No autoscroll while dragging near the list's edge.
- Keyboard focus: after a click in the sidebar, keys (Escape, and Ctrl+Z too - pre-existing) reach nothing
  until the canvas is clicked; Escape does leave select mode once focus is in the canvas or the sidebar.
- No Select All in the bar (the user's list did not have it); the browser's bar has Select All/None.
- Thumbnails omit the drop shadow and are not inverted in night mode; ruling lines are thin at this size.
- Thumbnail memory is not capped: ~80 KB per rendered page at desktop scale.
- PNG resolution follows `paintScale` (like Send Page Image), so a low-DPI screen gives ~150 DPI or less.
- The iOS, Android and wasm export paths were written without building those platforms.
- The outline view's row tap still calls `area->gotoPage` directly and can leave the page-number strip stale
  the same way; not changed here.
- New strings are untranslated.
