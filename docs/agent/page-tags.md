# Page tags

A page can carry tags from the library's tag tree (`#homework`, `#review-again`), and the document browser
then lists that page as a card of its own - its preview, its name, "in Notebook" - beside (not instead of)
its notebook. Taking a tag off is selecting it on the page and deleting it.

## The tag button

`#` in the top toolbar, left of Add Page (`TagMenu::createTagButton()`, `tagmenu.cpp`; both toolbar layouts).
An ArrowPopup opened on press, like Add Page's, with two views built once and switched by visibility (the
choice is a press inside the first view, so rebuilding would free the widget being dispatched to):

1. **Add Tags To: This Page / Whole Notebook.**
2. The checklist - the Manage Tags look from the browser: title, blue Done, search-or-create, tree or matches.
   - **Whole Notebook** applies each tick at once to the open notebook's own `tags`
     (`ScribbleDoc::setDocTags()`: `cfg->set`, `dirtyCount++`, `uiChanged` - the `layersChanged()` recipe;
     not undoable, like the layer choice).
   - **This Page** does nothing until Done. Tags unticked come off (`ScribbleDoc::removePageTags()`, one undo
     step); tags newly ticked go **onto the pointer** (below). Clicking outside discards the edit.

This replaced a modal "Tag Page..." dialog in the overflow menu, which is gone.

## Placing tags on the pointer

`ScribbleArea::startTagPlacement()` makes the elements but keeps them out of every page (`pendingTags`);
`drawScreen` paints them at the pointer like `regionInProgress`, the first pill's centre under it.

- Hover moves them (`doMotionEvent`, which receives buttonless motion because `enableHoverEvents` is on).
  Until the pointer moves - and a finger never hovers - they wait a third of the way down the view.
- A press is intercepted in `doPressEvent` right after its page switch; the drag follows; the release places
  them as one undo step. **The release picks the page again** (`dimToPageNum` of the pointer, the ghost page
  past the end clamped to the last) **and clamps the stack inside it**, `PENDING_TAG_MARGIN` from the edge:
  the press only switches page once well clear of the current one, so a press in the gap between pages,
  beside a page, below the last page, or a drag onto the next page used to leave the tag off the page -
  still counted, but missing from the thumbnail, and the pulse pointed at empty space.
- `ScribbleArea::reset()` (every document open) drops tags still on the pointer, or tags picked for one
  notebook landed in the next. It deletes them directly rather than calling `cancelTagPlacement()`, whose
  refresh would touch the document mid-switch. That release returns early, so it
  must call `uiChanged(UIState::ReleaseEvent)` itself - without it the undo and save buttons stayed greyed.
- `ScribbleView::capturesPointer()` lets a single finger reach `doPressEvent` even when one finger pans.
- **Esc** cancels, in `ScribbleApp::keyPressEvent`: a hovering pointer never presses the canvas, so
  ScribbleWidget's Esc handling never sees it (and in a debug build Esc would otherwise quit).
  `doCancelAction()` must *not* drop them - it runs on every save, so an autosave would.
- Pressing the tag button again drops any tags still on the pointer and starts over.
- **"Place your tag: tap where it goes, or drag it there"** sits under the toolbar for exactly as long as tags
  ride on the pointer (`MainWindow::syncTagPlaceHint()`, from `refreshUI`), with an X that cancels - the
  touch equivalent of Esc. Start and cancel run outside canvas events (the popup, Esc, the X), so they call
  `uiChanged()` + `doRefresh()` themselves; placing is a release, which refreshes anyway.
- **A pen leaves the placed tags selected** (`placePendingTags(select)`, from `event.source ==
  INPUTSOURCE_PEN`): a pen that does not hover had no preview until it touched down, so the tags come up
  ready to drag into place. Mouse and finger placements are not selected. agent-display has no pen, so this
  was checked with a throwaway build forcing `select` on; the pen condition itself is untested on hardware.

## Showing the tag when a page card is opened

The card carries the page's own tags that the filter matched (`TagDocList::selectedPageTags`), and after
the jump `ScribbleArea::flashPageTags()` points them out:

- It **scrolls to them** first (`viewRect()`, which moves only if they are off screen). `gotoPage()` alone
  leaves the page's top in view, so a tag lower down - or any tag when the notebook is zoomed in - pulsed
  unseen.
- Then it **pulses** a box in the theme's active blue (`--checked`, `#2EA3CF`, the same in light and dark;
  hardcoded because the canvas does not read theme CSS): two fade-in, fade-out cycles, 48 frames of 30 ms
  (`flashTicks` counts down a gui timer; `drawScreen` scales fill and stroke alpha by `0.5 - 0.5cos`).
  Verified in agent-display by counting blue pixels per frame: 21 -> 757 -> 21 -> 709 -> 21.
- It stores rects, not Element pointers, so an edit during the pulse cannot leave it pointing at a freed
  element.

## The tag is an element on the page

`<g class="write-pagetag" __pagetag="t5">` holding a pill `<rect>` and the `<text>` `#name`
(`Element::createPageTag()`, `isPageTag()`, `pageTagId()`). This was the user's call and it is the whole
design: **there is no page-tag panel, and no page-tag undo item**. Selecting and deleting is how a tag comes
off; undo, redo, copy/paste of a page and sync are the ordinary add/delete-element paths, so they needed no
code. The `__pagetag` id is what counts; the text is a label (see rename below).

- It is not ink, but it is on a layer (the current one when added) and selectable like any `<g>` - a tap hits
  its bounding box (`isNearPoint()` treats non-path children as bbox hits).
- The color is on the `<g>` only; the pill is the same fill at `fill-opacity` 0.16. That is what makes the
  selection's color button restyle the whole tag.
- **`SvgRect(rect, rx)` does not default `ry` to `rx`** - it defaults to 0, which renders no pill at all.
  Pass both. (This shipped broken for one build and was caught in agent-display.)
- The font is set on the `<text>` itself, not the `<g>`, because its bounds are measured before it has a
  parent. `satoshi, ui-sans, sans-serif`: the first loaded face wins here, and a browser viewing the SVG
  falls through to `sans-serif`.
- It is the first text inside a *document* (the app's own text is all UI). usvg rendered it without help.

## To-do tags

**Add it as a to-do**, under the checklist in This Page mode, gives the tags placed by that Done a checkbox
left of the name. It starts unticked every time the list opens (a choice for these tags, not a setting).

- The element is the same `write-pagetag` `<g>` with `__todo="0"` or `"1"`, plus two children: the box,
  drawn as a ring (two rects, `fill-rule="evenodd"`), and the tick, a filled polygon of class
  `write-todo-check` that is always there and hidden by `display="none"` while open. Neither has a color
  of its own: the selection's recolor sets every fill and stroke a child already has, so a stroked box would
  have been fine, but a white tick on a filled box would turn the tag color and vanish. A ticked tag also
  dims its text (`opacity` 0.5 on the `write-pagetag-text`).
- **Ticked means the page is no longer tagged**: `Page::refreshPageTags()` skips it, so the summary, the
  page card and the thumbnail go, but the element stays. Unticking brings them back. `flashPageTags()`
  skips a ticked copy too.
- **Ticking is a tap on the box** in any tool but the eraser (and not with the pen button or the eraser
  end), tested before the tool gets the press, like a region's "..." button. The press only remembers the
  tag (`todoPressed`); the release ticks it if it still hits the same box, so a drag that starts on the
  box does nothing, and a tag deleted in between (sync) is found by a fresh hit test, never through the
  stored pointer. `capturesPointer()` now takes the press, so a finger that would pan still ticks a box.
  A tag on a locked or hidden layer cannot be ticked (`Page::isEditable()`).
- `ScribbleArea::toggleTodoTag()` swaps in a **clone** with the box flipped, add and delete in one action -
  still no page-tag undo item. The clone keeps any move, scale or color the tag was given; rebuilding it
  with `createPageTag()` would not. Rename does rebuild (`renamePageTagElements()`), and passes the
  to-do state through.
- `ScribbleTest::pageTagTest()` pins tick, untick, undo, redo, rename and that the tap draws nothing; the
  counting checks were confirmed to fail with the `refreshPageTags()` skip removed.

## Moving a tag to another page is safe

Audited path by path. Everything that puts an element on or takes it off a page goes through
`onAddStroke`/`onRemoveStroke`, which rebuild `pageTagIds` from the page (strings, never `Element*`):
dragging a selection onto another page (copy to a clipboard, `deleteStrokes`, paste - one MULTIPAGE action;
a drop outside every page does nothing), cut/paste, undo/redo (`StrokeAdded/DeletedItem`), and sync (the same
items; a whole remote page comes through `loadSVG`). Page numbers exist only in the `pagetags` summary and
`pagethumb-N`, both rebuilt from the pages on every save, so insert/delete/reorder cannot leave them stale;
a page that loses its last tag has its thumbnail and entry dropped, one that gains a tag is dirtied and
re-rendered. `flashPageTags` returns quietly if the tag is not on the page, and `gotoSelectedPage` range-checks.
`ScribbleTest::pageTagTest()` pins the drag to the next page, its undo and redo, the off-page placements and
the reset; the placement and reset checks were confirmed to fail with the fixes disabled.

Left alone: **Ungroup** on a selected tag turns it into a plain rect and text (the tag is gone, ids stay
consistent); a stale tag index opens the card at the wrong page and nothing pulses; `Page::unload()` would
take the summary title from `pageTagTitle` rather than `outlineTitle`, but unloading only happens under
`SCRIBBLE_MEMORY_LIMIT`, which nothing defines.

## Why not a document-level list

Same reason as outlines ([outlines.md](outlines.md)): `{page 7: homework}` on the document would have to be
fixed up by insert/delete page, their undo, and sync, and would name the wrong page whenever one was missed.

## Summary for the browser: `pagetags`

The browser cannot open every file, so on save the pages' tags are summarized into the document config,
`pagetags="1|t1,t5|Quadratics;6|t2|"` - 0-based page, tag ids, outline title (`TagStore::formatPageTags()`).
Page numbers are fine *here* because it is a snapshot rewritten from the pages on every save.

- **The title is percent-encoded**, including `& < > " '`: `extractDocConfigValue()` reads the value raw
  out of the file without XML-unescaping, and the value also goes into the tab-separated tag index.
- `Page::pageTagIds` caches a page's tag ids (kept by `onAddStroke`/`onRemoveStroke`, reset in `loadSVG`).
  **A page that is not loaded** - `.svgz` loads lazily - gets its ids and title from the summary when the
  document is opened (`openDocument()`), so saving without ever loading a tagged page keeps it in the
  summary. Verified: open at page 1, save, `pagetags` and its thumbnail survive.
- Thumbnails: each tagged page gets `<image id="pagethumb-N">` (200 px wide) next to the document's own
  thumbnail, in all three formats (`writePageThumbs()` in `document.cpp`). A plain `.svg` writes them *after*
  the config, because the browser reads config from a fixed-size head of the file. `.html` bodies are kept
  across a load, so `pagethumb-*` `<img>`s are stripped on load like the main thumbnail, or every save would
  add another copy. `Page::pageTagThumb` keeps the base64: re-rendered on save only when the page is dirty
  or has none, read back from the final block on a `.svgz` load, so an unloaded page is never loaded just to
  draw it again.
- The tag index's DOC line gained a fifth field (the summary). A four-field line is an index from before
  page tags and is treated as a cache miss (`pageTagsKnown`). Writing this surfaced an older bug: the index
  was split with getline-based `splitStr`, which drops a trailing empty field, so **every untagged document
  was being reread from its file on every refresh**. `splitFields()` keeps empty fields.

## Filtering

- Pages are listed only when filtering by a tag (not under All Documents), and only for a tag **of their
  own** - otherwise filtering `#math` would list every tagged page of every math notebook.
- A page **inherits its notebook's tags** for matching (user's call): `#math` + `#homework` finds the
  homework page in a math notebook. In multi-select, the page lands above the separator (matches all) and
  the notebook below it (matches some).
- A page card opens its notebook at that page (`TagDocList::selectedPage`, `ScribbleDoc::gotoPage()`). The
  `doRefresh()` there is needed - outside an input event nothing else updates the page counter, which
  otherwise kept saying 1/2 on page 2.

## Deleting and renaming a tag in the browser

- **Delete removes the tag's elements from every page** (`ScribbleDoc::removePageTagElements()`), unlike a
  document's own tags, which go inert. A page tag is visible content; one left behind would show a name no tag
  has. Page tags do **not** fall back to the supertag (user's call) - that offer is for documents' own tags.
  Undo writes the files back byte for byte from a copy taken before the rewrite, skipping any file changed
  since (`DeleteSnapshot::pageTagFiles`). Verified by checksum.
- **Rename relabels** the elements (`renamePageTagElements()`), rebuilding each so the pill fits the new
  name, keeping its right edge and any color the user gave it.
- Both go through `TagDocList::rewriteDocument()`, which `setDocumentTags()` now uses too: load with every
  page forced in (see its comment on why a lazy page would be blanked), edit, refresh the summary and
  thumbnails, save with the old document thumbnail, update the cache. A notebook that is open in the editor
  is rewritten underneath it and reloads with the "modified outside of Sumi" notice - the same thing that
  already happened for a document's own tags.

## Known gaps

- In agent-display the **first key of a session is lost** ("exam" -> "xam", "science" -> "cience"); later keys
  all arrive, and a Shift sent first absorbs the loss. It happened in the Tag Page dialog and the tag
  popup alike, and a Save dialog opened later in a session did not do it, so it looks like the harness's
  Xwayland keyboard rather than the app. Not yet checked on a real display.
- The document's own thumbnail is not re-rendered by a browser rewrite, so after a tag is deleted it can
  still show the tag until the notebook is next saved from the editor.
- Tags from another library (a notebook copied between libraries) stay on the page but are not listed in the
  popup, so they are removed only by deleting the element.
