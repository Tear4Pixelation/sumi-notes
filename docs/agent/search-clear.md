# Search fields and the clear (X) button

Every search box is built by `createSearchField()` (`syncscribble/searchfield.{h,cpp}`), never a bare
`createTextEdit()`: document search, tag search, doc-tags popup search, the sidebar outline/layers search,
the toolbar tag menu and the New Notebook tag popup. A new search box must use it too.

- Returns `{edit, widget}`: keep `edit` for focus/`setText()`/margins, add `widget` where the edit would
  have gone. The onChanged callback is passed in (not assigned afterwards) because the helper wraps it to
  refresh the X.
- The X is a theme toolbutton with `icons/ic_menu_cancel.svg` (existing reicon "x"), background shrunk to
  28x28 via `SvgRect::setRect`; no CSS of its own, so it follows the theme. Hidden while the field is empty.
- `SearchEdit` overrides `TextEdit::setText()` so programmatic clears (`setText("")` on reopen) hide the X
  too; `setText()` still does not fire onChanged. The X press calls `setText("")`, then the callback with
  `""`, then refocuses the field.
- The X floats over the right end of the field (box layout, `box-anchor right`); the edit has no extra
  right padding, so a very long query can run under it.
