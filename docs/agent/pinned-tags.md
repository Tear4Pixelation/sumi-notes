# Pinned tags and the default tag

The library sidebar (`TagDocList::createUI()`, `tagdoclist.cpp`) shows **pinned tags** as a flat list straight
below "All Documents" (`pinnedTagsView`, no heading, zero height while nothing is pinned).

- **Pin / unpin**: the tag's right-click / long-press menu (`tagContextPopup`), on a row of the tree or of the
  pinned list. The item reads "Pin Tag" or "Unpin Tag"; `showTagMenu()` sets the titles each time.
- **Active by Default**: a second item, **visible only on a pinned tag** (`tagDefaultItem->setVisible`),
  reading "Active by Default" or "Not Active by Default" for the opposite of its state. At most one default tag.
- **Config** (saved, `cfgS` in `scribbleconfig.cpp`): `pinnedTags` (ids, `TagStore::formatTagList`, pin order)
  and `defaultTag` (one id). Tag ids belong to one library's `.write-tags`, so they mean nothing after a library
  switch; unknown ids are simply not shown (`pinnedTagIds()`, `defaultTagId()`).
- A pinned row is `createTagRow(id, 0, pinnedRow=true)`: no chevron, **not** registered with `tagDrag` (it is a
  shortcut, not a place in the tree). Tree and pinned rows both show the active state. Pins are not removed when
  a tag is deleted (the row just disappears), so undoing the delete brings the pin back.
- **Unpinning the default tag clears the default** (it stays active for the session as an ordinary filter).

## Filtering with a default tag

- On start (`TagDocList` constructor, and `setRoot()`) the default tag is put in `activeTags`.
- Single-select mode (`toggleTagFilter()`): picking any other tag replaces the *other* selection but keeps the
  default tag, so there is at most default + one more. Pressing the default tag toggles it off (and on again),
  leaving the other tag alone. Multi-select mode is unchanged (everything accumulates); leaving it keeps the
  default plus one other tag.
- Several tags combine as before: documents with all of them first, then those with some (page-tags.md,
  Filtering).
- **All Documents clears every filter, the default tag included** (it means "everything"). The default returns on
  the next start, or when it is pressed.
- Setting a tag as default also activates it at once.

## Not done

The default tag has no marker of its own in the list (it is just active); the menu item shows the state.
