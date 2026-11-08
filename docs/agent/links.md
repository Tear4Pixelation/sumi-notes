# Links

A link is a `<g class="hyperref" href="...">` around the strokes it was made from
(`ScribbleArea::setSelProperties()`), created with the Create Link dialog (`linkdialog.cpp`, Ctrl+L or the
selection menu). Clicking one with the Pan tool or a finger goes through `ScribbleArea::doClickAction()`:
an href starting with `#` is inside this document (`viewHref()`), anything else goes to
`ScribbleDoc::openURL()`.

## Bookmarks are gone from the UI

The bookmark tool (`MODE_BOOKMARK` / "Add Bookmark", key 0), the bookmark panel (`BookmarkView`, Ctrl+B,
its Keep Open / Margin Content controls) and picking a bookmark as a link target were removed; outlines
([outlines.md](outlines.md)) and page links replace them. What stays, deliberately:

- **Old documents keep working.** A bookmark is an ordinary element with class `bookmark` and an id
  `b-xxxxxxxxxx`; it still loads, draws and can be selected, moved and erased like any ink. A link to one
  (`href="#b-..."`) still navigates (`Document::findNamedNode()`) and still exports to PDF.
- **`MODE_BOOKMARK` still exists in `ScribbleArea`, with no UI.** ScribbleTest's numbered fixtures (test9,
  test12) create bookmarks with it and link to them, and their reference output depends on exactly that
  gesture. Removing the mode means rewriting those fixtures and their `_ref.html`; it was kept so the
  legacy-bookmark path stays covered.
- `bookmarkColor` (config, theme palette slot, sync/undo doc state) stays: it colors legacy bookmarks and
  is on the sync wire. `snapBookmarks` stays for `MODE_BOOKMARK`. `autoHideBookmarks` and `bookmarkMode`
  were removed; an old config carrying them is ignored. `actionShow_Bookmarks` in a saved toolbar config
  is skipped by `MainWindow::findAction()` returning NULL.

## The dialog

"Link to" is a dropdown with three types; only the chosen type's rows are shown:

- **Outline** - an entry of this document's outline (`ScribbleDoc::outline(true)`, so every page is
  loaded once - the same known cost as the sidebar). Disabled with "No outline entries" if there are none,
  and the default type is Page then.
- **Page** - a document ("This Document", the other open tabs, or "Other Document..." through
  `FilePicker::openFile`) and a page number of it. Another document is loaded only to count its pages.
- **URL** - free text.

Reopening the dialog on an existing link parses its href back into a type (`LinkDialog::parseHref()`); a
page anchor on a page that carries an outline entry shows as Outline. **OK with nothing changed keeps the
original href** - otherwise reopening a link to an old bookmark would silently retarget it to the top of
its page.

## Href formats

| href | meaning | made by |
|---|---|---|
| `#pg-xxxxxxxxxx` | a page of this document, by its anchor | Outline, Page (this document) |
| `<path>#page=N` | page N (1-based) of another document | Page (other document) |
| `#page=N` | page N of this document | hand-typed only; followed |
| `#b-xxxxxxxxxx` | a legacy bookmark | old documents only |
| anything else | a URL | URL |

**Why a page anchor and not a page number.** Pages move (insert, delete, Pages view drag, nesting outline
entries), and a link must follow its page. Like the outline entry, the anchor lives on the page itself:
`Page::anchorId`, a cache of `__pageid` on `contentNode`, read in `loadSVG()`. `Document::findPageTarget()`
resolves `#pg-` and `#page=` (loaded pages first, then loads the rest) and `viewHref()` jumps to the top of
the page (`jumpToPage`, so it is in the back/forward history). Outline links use the same anchor: the entry
*is* its page, so a renamed entry keeps its links.

**The anchor is assigned lazily, in the link's own undo step.** `setSelProperties(..., anchorpage)` adds a
`PageAnchorItem` (type `0x00040008` - not `0x30008`, which `isA(PAGE_CHANGED_ITEM)` would match and
`ScribbleSync::removeItems()` would then cast to the wrong class) before setting the id, so one Ctrl+Z
removes both link and anchor, the page is dirtied and saved, the undo history persists it (`pageanchor`),
and a sync peer gets the anchor (`<pageanchor pagenum id>`) its copy of the link points at. A second link to
the same page reuses the anchor.

**Another document is never written to**, so a link into it names the page by number and does not follow
the page if that document is rearranged. The path is relative to the linking document (`./`, `../`) when
both share more than the root, else absolute; an unsaved document links absolutely. `openURL()` accepts
`.`-relative, `/`-absolute and `file:///` paths to `.svgz`/`.svg`/`.html` documents, opens them (a tab) and
then follows the fragment; a link into the same file is just a jump.

**Copy + paste of a page** would carry the anchor with it; `ScribbleDoc::pastePages()` drops a pasted
page's anchor if the document already has a page with it (cut + paste keeps it, the original is gone).
PDF export (`ScribbleApp::writePDF`) resolves `#pg-`/`#page=` to the top of the target page.

## Tests

`ScribbleTest::linkTest()` (unit-check count): `#page=N` parsing, anchor assigned and named by the link,
one undo removes it and redo restores the same id, a second link reuses it, it follows its page after an
insert above, `viewHref()` lands there, save/reload, sync wire format, `isA()` distinctness, paste does not
duplicate. Mutation-checked: `PageAnchorItem::undo()` not swapping and `pastePages()` not dropping the
duplicate each fail their checks.

## Known gaps

- Windows absolute paths (`C:\...`) are not recognized by `openURL()` as local documents; cross-document
  links between drives on Windows go to the system handler.
- Relative paths are computed with `/` separators.
- The dialog lists the outline in one combo box; a very long outline makes a long menu.
- An Outline-type link with no entries does nothing on OK (the dialog stays open).
