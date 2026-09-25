# The general-purpose sidebar

One panel with two views - the document's **outline** (table of contents) and its **layer table** -
over the backends already built (`ScribbleDoc::outline()`/`setPageOutline()`, and
`ScribbleDoc::layers()` plus the `addLayer`/`removeLayer`/`setLayerLocked`/`setLayerHidden`/
`moveLayer`/`moveSelToLayer` family; see CLAUDE.md "Outlines" and "Layers"). It is the first UI
either feature has had.

This document is the rationale; `syncscribble/sidebar.h`/`.cpp` is the implementation. Geometry comes
from the Penpot file **"General purpose sidebar"** (board `Sidebar` for the panel, board
`Write - Main View` for where it sits), the convention `tagdoclist.cpp` already follows: match the
design file, don't eyeball it.

## 1. Pinned vs floating is one widget in two parents

The user asked for both a permanent sidebar and a transient one, switchable live. These are *not*
two widgets. They are the same widget parented into one of two places in the existing main-window
tree (`res_ui.cpp`):

```
#main-container            layout="box"      <- overlay stack
  .sub-window-layout       layout="flex" flex-direction="row"
    #bookmark-panel
    #scribble-split-layout   box-anchor="fill"   <- the canvas
  #main-toolbar-container  box-anchor="top hfill"  <- floating toolbars overlay the canvas
```

- **Pinned**: a child of `.sub-window-layout`, first or last depending on which side it is on. Being
  a flex sibling of the canvas, it *takes* its width from it, and `ScribbleView` already centres the
  page within whatever width it is given - so "the document stays centred in the remaining space"
  needed no code at all. This is the whole reason for choosing the flex row over an inset.
- **Floating**: a child of `#main-container`, `box-anchor="left vfill"` or `"right vfill"`, so it
  sits in the same overlay stack the floating toolbars already use and covers the canvas without
  resizing it.

Toggling the pin therefore *reparents* one widget rather than building a second, which is what keeps
the two modes from ever drifting apart in appearance. Which side it sits on is the same mechanism
again: which end of the flex row, and which `box-anchor` when floating.

**The reparent is deferred by a 1 ms timer** (`schedulePlacement()`). Both toggles are pressed
*inside* the sidebar, so doing the move from the press handler reparents the tree out from under the
widget SvgGui is still dispatching to; observed as the panel vanishing instead of changing mode.
`SvgGui::setTimer` asserts `msec > 0`, so this cannot be a zero-delay timer. "Was it open" is
captured at schedule time, not read inside the timer, so nothing in between can change what is
reopened.

## 2. Dismiss-on-press-outside

Floating, a press anywhere else closes the sidebar. **Three mechanisms were tried; only the third
works**, and the two failures are worth recording because both look correct:

- `OUTSIDE_PRESSED` never fires. That event is delivered only to the widget that *captured* the
  press, so a panel sitting in the tree never sees it.
- Pushing the sidebar onto SvgGui's menu stack (`SvgGui::showMenu` takes a plain `Widget*`, not only
  a `Menu`) does deliver `OUTSIDE_MODAL` - but it also hands the panel to the menu machinery, which
  closed it on presses *inside* it. Its own buttons stopped working the moment it floated.
- **`Widget::eventFilter` on `#main-container`.** SvgGui walks from the event's target up through
  its ancestors and calls their filters *before* dispatching, so this sees every press in the
  window, including the ones the canvas would otherwise swallow.

The filter **returns false**, i.e. the press is not eaten. Swallowing it was tried and makes the
toolbar feel broken: with the sidebar open, the first click on a toolbar button only dismisses the
sidebar and the user has to click again. Letting it through means the button they aimed at also
does its job. Escape closes it too, via an ordinary key handler.

## 3. Geometry

Design units map to app units through `floatUIScale`, exactly as `mainwindow.cpp` and
`pentoolbar.cpp` do. Verified by measurement rather than assumed: the floating tool buttons render
the design's 64-unit button at 24.2 screen px, and at that scale the design's 372-wide sidebar comes
out at 141 px - which is what the implementation produces.

Insets come from the *toolbar's* geometry, not the design file's numbers: `floatInset` for the edge
and `floatTopInset + floatBtnSize + floatInset` for the top. The design's own 12 is smaller than
`floatInset`, which left the sidebar sitting visibly further left than the toolbar above it. Those
three constants moved to `basics.h` so both files read one definition rather than repeating it.

**The insets sit on the panel, not on the sidebar widget, and that is load bearing.** ugui margins
are *outside* a widget's own rect, so margins on the sidebar put the gutter outside its bounds,
where its own background cannot reach - and nothing else paints a strip the canvas has vacated, so
the canvas's last frame stays on screen there. The visible symptom was the page-number statusbar
stranded beside the panel, still drawn at the position it had before the canvas shrank. The sidebar
therefore takes zero margins, owns its whole column, paints it with `--canvas` (`.sb-backfill`, and
only while pinned - floating it must stay transparent over the canvas), and insets the panel within.

**The view selector's height is set from its rendered size** (74 units -> 28 px), not from the design
file's 49. It is the one control here picked by eye against a screenshot, so the number that matters
is the one on screen; the constant is written to land on it. That number only started taking effect
at all once the `.field-bg` rects became `hfill` - see the sizing trap below; as `fill` they reported
neither dimension, so the selector sized itself to its icon and rendered **12 px** no matter what the
constant said, which is why two rounds of doubling it changed nothing.

**The edge facing the canvas takes no gutter at all** (`sbCanvasGutter == 0`), where the window side
keeps `floatInset`. That inset is what lines the panel up with the toolbar; between the panel and the
document it is only dead space. It follows the sidebar, so it is the right edge on the left and the
left edge on the right.

Note *where* that gutter is visible: only once the document is zoomed past the viewport width. At
fit-page zoom the page is centred in whatever the sidebar leaves, so the ~200 px beside the panel is
that centring, not this margin - two different gaps that look like one.

With the two horizontal margins unequal the panel is **anchored to the window edge** (`left vfill`/
`right vfill`) rather than left to the box layout's default centring, which split the difference and
drifted the panel out of line again.

**One sizing trap, over and over.** `prepareLayout()` reports a widget's size only for the
dimensions that are *not* fill, so a `fill` rect declares nothing and its parent sizes to its
contents instead. So:

- the panel's background rect is `vfill`, not `fill` - its declared *width* is what gives the panel
  its width (an explicit `width` attribute on a `<g>` is ignored by the box layout). With `fill` the
  panel collapsed to the natural width of the actions row's four toolbuttons.
- each row's sizer rect is `hfill`, not `fill` - its declared *height* is what gives the row the
  design's height. With `fill` the layer rows measured ~26 px against the design's 33.
- the view selector's and search box's `.field-bg` rects, same rule, found later: both were `fill`,
  so both sized to their contents and ignored `sbSelectorH`/`sbSearchH` entirely.

This one rule has now caught five widgets here. When something in this file renders at the size of
its contents rather than the size it declares, check the anchor before changing the number.

Row spacing is the design's 12-unit gap, as a bottom margin on every row; the trailing gap sits below
the last row inside a scroll view, where it is invisible.

A separate trap: **a sizer rect must not be a flex item.** The actions
row's sizer was a child of the flex row, so `space-between` spread the four buttons against it and
they came out bunched to one side of an invisible block. It is now a sibling of the row inside a box
container, the same shape `createRow()` uses. The buttons' stock 36x42 backgrounds are also shrunk to
the icon size, so `space-between` spaces the icons rather than wider cells around them.

## 4. The two views

| view | rows from | row shows | row does |
|---|---|---|---|
| outline | `ScribbleDoc::outline(false)` | title, indented by level, page number | go to that page |
| layers | `ScribbleDoc::layers()` | preview block, name, lock toggle | make it current |

- The view is chosen from a **`Button::setMenu` dropdown**, not an `onClicked` that calls
  `SvgGui::showMenu`: a menu has to be parented to the button it drops from before it can be shown,
  and `setMenu` is what does that. Shown manually it simply never appeared.
- **`outline(false)`, not `outline(true)`.** `outline(true)` loads every page of a delay-loaded
  document, which for a large one is the whole file - far too much for a panel that refreshes on
  ordinary UI events. The cost is that a page never opened this session contributes no entry
  (`document.h`, known gap).
- Layers are listed **top-down**, i.e. the table walked backwards, since the table is bottom-first in
  z-order and a layer panel reads top-down.
- Outline rows with children collapse, keyed by page number - ephemeral view state, so an entry whose
  page moved comes back expanded rather than following the wrong row.
- The search field is hidden until the search button is pressed and clears its query on hide, exactly
  `TagDocList::toggleDocSearch()`; the rounded fill *is* the row's own background, so the `TextEdit`
  contributes no chrome of its own. Unlike the tag sidebar's, it sits at the **bottom**.

## 5. Deviations from the design, and gaps

- **The lock icon is drawn on every layer row, dimmed when unlocked.** The design draws it only on
  locked rows, which leaves an unlocked row with no way to lock it. Locked rows still read exactly
  as designed.
- **The layer preview is a plain block**, as designed. There is nothing to render in it: a layer
  spans every page, so there is no single thumbnail that means "this layer".
- **No hide/show toggle**, because the design has none - `setLayerHidden()` exists and is unused
  here. Same for rename, delete and reorder: the backend has them, the design does not place them.
- Colours are literals from the Penpot file rather than theme variables (`#101010`, `#444444`,
  `#F2F2F2`, `#CDCDCD`, `#2EA3CF`), exactly as the floating toolbar panels are: this panel is part of
  that same dark floating chrome and does not follow the document's light/dark theme.
- The modal presentation the user also wants is deliberately out of scope; §1's floating mode is the
  halfway point and may turn out to be enough.
## 6. Opening it

A button at the **left end of the page ops panel, immediately before Bookmarks**. The two are the
same kind of thing - a panel that lists a way into the document - and the sidebar itself defaults to
the left edge, so the control sits on the side of what it opens.

- Its glyph is reicon `sidebar-left`/`sidebar-right` (`ic_menu_sidebar_left`/`_right`) and **names
  the edge the sidebar is docked to**, swapped by `MainWindow::updateSidebarButton()` whenever the
  side changes. The variants are load bearing, not decorative: the button says where the panel will
  appear before it appears.
- The sidebar's *own* side toggle shows the **opposite** glyph, because it names the side the panel
  would move *to*. Two buttons, two questions, so the same pair of icons reads in opposite senses -
  worth knowing before "fixing" one of them to match the other.
- Deliberately **not** `ic_menu_split_lr`/`_rl`. Those are the same reicon glyphs, but they mean
  split view, and a shared icon name would tie the two features together for the next person.
- The action is **checkable**, so the button also reads as open/closed, and it is registered through
  `addTBWidget`, which generates the overflow-menu item automatically when the panel is too narrow -
  rather than a second, hand-placed menu entry that could drift out of sync.
