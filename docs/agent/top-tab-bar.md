# Top tab bar

A full-width strip along the top of the window: the library (home) button, one tab per open document
([editor-tabs.md](editor-tabs.md)), and in the editor the View popup and the overflow menu. Designed in the
Penpot file **"Tab Topbar"** (page "Topbar"); the proportions are that board's, but the bar is **ninety percent of a toolbar row (`barHeightFraction`)
high** (the user prefers that to the board's 64), so every size is the board's value times `toolbarRowHeight()/64`.
Code: `syncscribble/topbar.h/.cpp` (`TopBar`), placed by `MainWindow::setupUI()` (editor) and
`TagDocList::createUI()` (library).

## The setting

`topTabBar` (Preferences > User Interface > Top tab bar, `TopBar::Mode`), **default 2**:

| Value | Library | Editor |
|---|---|---|
| 0 Off | no bar | as before: Tabs in the sidebar and on the toolbar |
| 1 Library only | bar | as before |
| 2 Library + Writing | bar | bar; it **replaces** the toolbar's Tabs button and the sidebar's Tabs view |

`ScribbleApp::topBarMode()` returns 0 whenever `tabsEnabled()` is false (iOS outside library mode).
`MainWindow::applyTopBarMode()` applies it at startup and on Preferences OK (`reloadConfig`); the library's
bar follows it in `TagDocList::setup()`, i.e. on its next opening.

## Anatomy (Penpot values, before scaling)

`metrics()` in `topbar.cpp` (`BarMetrics`) holds every size. The bar height is `toolbarRowHeight()`: the
background rect of ugui's `#toolbutton` (theme.cpp), i.e. the height of the regular toolbar's row (42 units),
read from the theme by instantiating one button, not copied. Scale `s = 42/64`: padding 4 12 -> 2.6 7.9,
home 56 -> 36.8, radius 12 -> 7.9, icon 32 -> 21, tab padding 24 -> 15.8, title 18 -> 11.8, View/overflow
icons 33/42 -> 21.7/27.6. Everything below is the board's value; multiply by `s`.

```
 bar      fill --dark, padding 4 12 (+ topInset above, see below), items 4 apart, centred
 ├─ home   56x56, radius 12, 32 icon: reicon home (outline); checked = filled icon + --tab-active fill
 ├─ tabs   AutoAdjContainer, hfill: tabs 56 high, radius 12, padding 0 24, 12 apart
 │         title Raleway 18 --text; active tab: --tab-active fill + 32 close (ic_menu_cancel = reicon x)
 ├─ view   bare 33 icon (ic_menu_view = reicon eye-scan, a PLACEHOLDER: one line in reicon_import.py)
 └─ menu   bare 42 icon (ic_menu_overflow) - editor only
```

- **Tabs share the strip equally, capped at 601** (the board's two tabs are 601 each; uncapped, one tab
  would run across the whole window) and floored at 96. `fitTabs()` runs from the tab strip's `adjFn`,
  so it re-fits on every layout - sizing the background rect and eliding the title (`SvgPainter::elideText`).
  The background rect is what sizes a tab: a `<g>`'s width is ignored by the box layout (sidebar.md trap).
- **Close is on the active tab only**, as designed. In the library no tab is active (home is), so there
  is no close button there; tabs are closed from the editor.
- Colors are tokens. `--dark` is the token's own name in ugui's theme (#101010 / #F0F0F0). The design's
  `canvas-background` (#444444 / #D0D0D0 in tokens.json) has no ugui variable, so the bar defines
  **`--tab-active`** with exactly those values. The bar's CSS (`TopBar::stylesheet`) lives in
  `topbar.cpp` and is appended in `createStylesheet()` (resources.cpp) - app-side, so the feature needed
  no ugui submodule commit. Hover is `--tab-active` at 50 %; the design has no hover state.

## Editor (mode 2)

- The bar is a sibling **above `#main-container`** in the main window's column (`#top-tabbar-container`
  in res_ui.cpp), not an overlay: the canvas starts below it and the floating toolbar floats below it.
- **View and the overflow menu move into the bar.** On desktop the toolbar's right panel (`overflowRow`,
  which now holds View + overflow) is hidden; on the vertical toolbar the two buttons get class
  `topbar-moved`, which auto-adjust's reset respects (it shows every widget before hiding what does not
  fit, so a plain `setVisible(false)` would be undone). The Tabs toolbar button is hidden the same way,
  and its mirror item in the overflow menu is never shown. The sidebar's Tabs view leaves its selector
  (`Sidebar::setTabsViewAvailable`), falling back to the outline **without storing it**, so turning the
  bar off brings Tabs back.
- **The overflow popup is one widget that moves** (`hostOverflowPopup`): it holds the auto-adjust mirror
  items, and an ArrowPopup is positioned relative to its parent, so a press on either host button
  reparents it there first. View popups are cheap, so each button gets its own (`createViewPopup`).
- Every bar press is acted on from a 1 ms timer: each one rebuilds the bar (`refreshUI` -> `refresh`), and
  home opens the library, whose modal loop must not run inside a handler of a button it deletes.
- Home = `openDocument()` (save point, then library), a tab = `switchToTab`, close = `closeTab`.
  `refresh()` rebuilds only when the tab paths or the active one changed (called from every `refreshUI`).

## Library

`TagDocList`'s window became a column: `#library-topbar-container` over the old `#main-layout` row. Home
is checked (filled icon). A tab opens its document exactly like a card (`selectedFile`, `EXISTING_DOC`);
`doOpenDocument` switches to an already open tab without reloading it. Its View popup
(`createViewPopup(true)`) has only Fullscreen and Status Bar. `closeTab()` no longer reopens the library
when the last tab is closed while the library is up (`libraryShown()`) - it would nest a second one.

## Status bar

`showStatusBar` (`ScribbleApp::statusBarShown()`, View > Status Bar), default on for iOS/Android, off on
desktop. `ScribbleApp::topInset` keeps its meaning (top safe area); what changed is who takes it:

| | inset when shown | hiding it |
|---|---|---|
| iOS | `iosTopSafeInset()` (unchanged) | `SDL_SetWindowBordered(false)`: SDL's view controller hides the status bar for a borderless window; the safe area then shrinks |
| Android | 0 - targetSdk 29 lays the window out below the status bar | fullscreen (immersive), the only way SDL has; Fullscreen's check follows |
| desktop | 24, or `SUMI_TOP_INSET` | nothing to hide; the inset is the preview |

With the bar shown, the inset is **padding inside the bar** (painted `--dark`), and what sits below the
bar takes none: `ScribbleApp::editorTopInset()` is 0 then, used for `#main-toolbar-container` and the
sidebar panel; the library's search row drops its inset the same way. **Not run on a device**: the iOS
borderless path and the Android fullscreen path are untested.

## Overflow menu cleanup

Now: (auto-adjust items) / Whiteboard (while syncing) / [New, Open, Save - only with `useDocList` off] /
**Export >** / Insert Document... / Insert PDF... / **Create Whiteboard...** (+ Open Whiteboard on iOS) / Page Setup / Theme / Preferences /
Testing (debug).

Export holds Export PDF (desktop) or Send PDF / Send Document / Send Page Image (mobile), the new
Export PNG (every page, `exportPagesPNG`) and Save As.

Removed, and where each still is:

| Item | Elsewhere |
|---|---|
| Selection: Cut, Copy, Duplicate, Delete, Create Link | selection popup; Paste on the toolbar |
| Selection: Ungroup, Invert, Select All | **nowhere visible** - Select All keeps Ctrl+A; Ungroup and Invert have no shortcut |
| View: all | View popup |
| Page: New Page After | Add Page (inserts after the current page) |
| Page: Scan Document as Page | Add Page popup |
| Page: Delete Current Page (also was top level) | sidebar Pages view (context menu, select mode) |
| Page: New Page Before | **only Ctrl+Shift+Left** |
| Page: Expand Down, Expand Right | **nowhere** |
| Page: Select Pages (`MODE_PAGESEL`) | **only Ctrl+Shift+P**; the sidebar's Pages select mode covers move/delete/export |
| Document: Discard Changes | **only Ctrl+W** |
| Document: Insert Document, Insert PDF (pages from another file) | restored as top level overflow items after Export (no icons, as before); the "+" menu's Insert Document places a PDF page as an image, which is different |
| Document: Import PDF; top level Import Document | library Import |
| Insert Image, Scan Document | "+" menu (Insert Photo, Scan Document) |

## Traps (each one was hit)

- **Hide the container, not only the bar.** With the library's bar hidden, its empty container in the
  window's flex column laid out at zero size and the division by it left the whole library unpainted -
  pure black. `TagDocList::setup()` and `applyTopBarMode()` hide `#library-topbar-container` /
  `#top-tabbar-container` themselves.
- **The background is not `box-anchor="fill"`.** ugui margins lie outside a widget's rect, so a fill rect
  stopped short of the row's bottom padding, and that strip was never painted: it kept tab titles from
  the previous padding. The rect has an explicit height (the whole bar, padding included).
- **The bar overdraws 2 units downward** (`bgOverdraw`, negative bottom margin, so the layout height is
  unchanged): the bar height is not a whole number of pixels at most scales (0.76 px/unit here), and the shared
  pixel row was antialiased from both sides over stale pixels. Whatever follows paints over the overlap.
- **Every bar button's hover/press rect is a `homeSize` square.** `createIconButton` used to size it icon wide
  by `homeSize` high, so View (33) and overflow (42) lit up as tall rectangles; the icon (any size) is centred
  in the square, the rect no longer follows it. Home, tab, close and the two buttons were each checked hovered
  and pressed: all square or the tab's rounded rect (close is a rounded square inside the active tab).
- **The row is `box-anchor="top hfill"`.** Without `top` the box layout centres it, and with the status bar
  padding on (extra top margin) it sat half the padding too low (content spilled below the bar).
- **Changing the bar's height repaints the canvas from a 1 ms timer** (`orientationChanged`): the canvas
  only repaints what it believes changed, and the strip it gained kept the bar's old pixels.
- **`TopBar::refresh()` rebuilds on a 1 ms timer once the window is shown, never synchronously.**
  `refreshUI()` is reached from inside a layout pass (`ScribbleView::doResizeEvent` -> `doRefresh`, the first
  layout after a document opens), after the bar, earlier in the window, was already laid out in that pass:
  tabs added then were never laid out, and the frame's paint cleared the dirty flags they set. Starting with
  no tabs (no `openTabs`, or a file on the command line) and opening one left the editor's bar without its
  tab - and, before the ugui zero-size fix, drew that unlaid tab over home at the window's corner. No
  `Timer*` is kept: closing the library window drops its timers while its `TopBar` lives on, so each call
  that sees a change sets its own timer and `rebuild()` re-checks the state. While the window has never
  been shown (no `SvgGui`: setupUI, the library's first `TagDocList::setup()`) the rebuild is immediate.
- The test library must not be `Debug/library`: that is also the desktop debug build's fallback library,
  and the startup move from fallback into library deletes the marker when both are the same folder
  (pre-existing, not this feature's; only an agent fixture would set it up that way).

## Verified (agent-display)

Driven in the headless display with a fixture library (`Debug/testlib`, three documents restored as
tabs from `openTabs`), the mode set in `Debug/write.xml` before each start - so reading the persisted
value is what was exercised:

- **Mode 2**: editor matches the Penpot board (home outline, active tab filled with its close X, equal
  tabs, View + overflow at the right, toolbar floating below, its right panel and Tabs button gone);
  View popup (Previous/Next View, Split View, Bookmarks, Fullscreen, Dark Mode, Status Bar); overflow
  popup (Export >, Create Whiteboard, Page Setup, Theme, Preferences, Testing) and its Export submenu;
  Status Bar on (24-unit `--dark` padding above the bar) and off again with no stale pixels; switching
  tabs; home -> library with the filled home; a library tab -> its document; closing the active tab
  (the neighbour takes over); the pinned sidebar below the bar with no Tabs in its view selector;
  Top tab bar in Preferences.
- **Mode 1**: editor unchanged (Tabs button, sidebar Tabs, View + overflow panel); library with the bar,
  its View popup (Fullscreen, Status Bar), a tab opening its document.
- **Mode 0**: editor and library as before the feature, the View popup on the toolbar's panel.

Not exercised: Preferences changing the mode live (it calls the same `applyTopBarMode()` as startup),
the vertical toolbar, light theme, and any device (status bar hiding on iOS/Android).

The UBSan float division by zero in `Widget::setLayoutBounds` (the tab strip's first, empty layout) was
not harmless: it was ugui scaling a zero-width node to a real width, which made its layout transform
inf/NaN for good. Fixed in ugui (a zero-size node is only moved), pinned by
`ScribbleTest::zeroSizeLayoutTest()` (tests.md).

## Gaps

- No close button on inactive tabs or anywhere in the library's bar (as designed, but it means the
  library cannot close tabs).
- Tabs cannot be reordered or dragged onto the canvas from the bar (the sidebar's Tabs view could; in
  mode 2 that view is gone).
- The library's bar has no overflow menu.
- View icon is a placeholder (reicon eye-scan).
