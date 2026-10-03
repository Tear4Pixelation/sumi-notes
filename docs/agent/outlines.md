# Outlines (table of contents)

A page can carry one **outline entry**: a title plus a nesting level, from which the document's table
of contents is built. The entry points are `ScribbleDoc::setPageOutline()` and
`ScribbleDoc::outline()`; the UI over them is the outline view of the sidebar
([sidebar.md](sidebar.md)).

Outlines are *not* a replacement for bookmarks, and the two are deliberately different things. A
bookmark is an `Element` with class `bookmark` living at a position *within* a page, labelled by the
user's own ink (`BookmarkView` re-draws the actual strokes; in `MARGIN_CONTENT` mode anything written
in the margin *is* a bookmark). It is also the **link target system** - `setSelProperties` turns
strokes into a bookmark with id `b-xxxxx` and hyperrefs are `href="#b-xxxxx"` - so removing bookmarks
would break internal links in every existing document. An outline entry is page-level, text-labelled
and nestable. One page, one entry: the intended way to work is a page per subject.

- **The entry lives in the page's own SVG**, as `__outline`/`__outlinelevel` on `contentNode`,
  alongside `xruling`/`papercolor`. `Page::outlineTitle`/`outlineLevel` are a cache of those, read in
  `loadSVG()` and written through by `Page::setOutlineEntry()`. This is the whole design: **a
  document-level list of `{page number, title}` would have to be fixed up by `insertPage`,
  `deletePage`, undo of either, and sync**, and would silently name the wrong page whenever one of
  those was missed. Living on the page, an entry moves with its page for free - and is carried along
  by copy+paste of a page, since the attributes are written eagerly rather than at save time.
- **`Document::outline()` caches nothing.** Page numbers are read off the pages' current positions
  every call, which is what makes the result correct by construction after any edit. It is also why
  the entry points return a fresh `std::vector<OutlineEntry>` rather than handing out a member.
- **The `__` prefix is load bearing.** An unprefixed `outline` would collide with the CSS property of
  that name, which usvg supports, so the parser could plausibly claim it as a presentation attribute.
  The neighbouring page attributes predate CSS support and are left as they are.
- **Levels are normalized when the outline is built, not when it is stored.** An entry may not sit
  more than one level deeper than the entry before it. Without that, deleting the page holding a
  level-0 entry leaves its level-2 children under nothing and a renderer has a hole it cannot draw.
  The *stored* level stays as the user asked, so restoring the parent restores the intent.
- **`PageOutlineItem` is a new undo item, deliberately not part of `PageProperties`.** Folding the
  title into `PageProperties` would have given undo for free, but
  `ScribbleDoc::setPageProperties(applytoall)` assigns one struct to *every* page - so an unrelated
  ruling or theme change would stamp one page's title across the whole document. Same trap
  `setTheme()` documents above.
- **The title is the first arbitrary user string on the sync wire**, so `PageOutlineItem::serialize()`
  writes through `XmlStreamWriter` (pugixml) rather than `fstring` - a title containing `&` or `'`
  would otherwise emit a malformed item and desync the stream. Undo items *are* the sync protocol, so
  this also needed an `outlinechanged` branch in `ScribbleSync::processItem()`.
- `setPageOutline()` returns false for a no-op, so retyping the same title cannot push an empty step
  onto the undo history. `UndoHistory::addItem()` calls `commit()`, which does the `dirtyCount++` - it
  must not also be done at the call site (the convention `Page::setProperties()` follows).

Known gap: **`Document::outline(loadpages=true)` loads every page** that has never been loaded, since
an unloaded page cannot be known to carry an entry. For a large delay-loaded document that is the
whole file. `loadpages=false` skips them instead, at the cost of an incomplete outline. Doing better
needs the titles in the document wrapper (next to the `<object>` dimensions) or in the bgz index - a
per-page index that is *derived* on save, not a second source of truth.

Tested by `ScribbleTest::outlineTest()` (`runAll`'s unit-check count). Each check was verified to fail
against a deliberately broken version - normalization removed, the attribute not read back, the undo
item not swapping, the wire format written unescaped with `fstring`, and `Document::outline()` caching
its result (i.e. the document-level index this design exists to avoid). That last mutation is the one
that pins the headline requirement, and the unescaped-title mutation confirmed the escaping is a real
hazard rather than a theoretical one.
