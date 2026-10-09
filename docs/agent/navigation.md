# Moving around the canvas

Five ways to move the view, all of which already existed in some form; what follows is mostly a
record of why they did not work.

**No fling, stiff scrolling and slow page switches together** were not a navigation bug: the fling runs on
an ugui timer, and a slow periodic timer (the Pages view's thumbnails) used to starve every other timer.
See the timer trap in [page-management.md](page-management.md#thumbnails).

| gesture | what it does |
|---|---|
| mouse wheel | scroll up/down |
| Shift + wheel | scroll left/right |
| Ctrl + wheel | zoom about the pointer |
| middle mouse button drag | pan, whatever the active tool is |
| Pan tool (hand icon) | pan with an ordinary left drag |
| two fingers, or one finger once a pen has been used | pan/zoom |

- **The wheel did nothing at all on Linux, and the cause is not in Write's own logic.** `SvgGui`
  read only `SDL_MouseWheelEvent`'s integer `x`/`y`. Under **sdl2-compat** - the SDL2 API
  reimplemented on top of SDL3, which Arch and others now ship in place of SDL2 - those arrive as
  **0** and the scroll is reported solely in `preciseX`/`preciseY`. `SDL_GetVersion` reports
  something like 2.32.72, so nothing looks wrong. `sdlMouseEvent()` now prefers the float fields and
  falls back to the integers, which is also the better reading on genuine SDL2: they are the only
  fields carrying sub-notch touchpad resolution, which is the very thing the `*= 120` was added to
  preserve.
- **Never write `SDL_Event event = {0};` - write `= {}`.** For a union, `{0}` only initializes the
  first member (the 4-byte `type`), and since GCC 15 the rest really is left as stack garbage. On Linux
  the wheel events come from `linuxtablet.c` (XI2 buttons 4-7; SDL's own are disabled), which built them
  this way, so `preciseX`/`preciseY` - which the line above now *prefers* - held garbage: the wheel did
  nothing, or overflowed `wheel.y` and jumped to the end of the document. The Linux-built files were
  converted; the per-platform helpers (Android, iOS, macOS, Windows, wasm) still use `{0}` and are
  built by clang/MSVC, but are the same latent bug under GCC 15+.
- **`wheelModifiers()` is the single place the wheel's modifiers come from.** `wheel.direction >> 16`
  is a *Windows-only* convention: only there does Write synthesize the wheel event itself
  (`winhelper.cpp`) and pack the modifiers into those bits. Two callers read them unconditionally,
  and off Windows those bits are `SDL_MOUSEWHEEL_NORMAL`/`FLIPPED` - **and `FLIPPED` is 1, which is
  `KMOD_LSHIFT`**, so this did not merely disable Ctrl+wheel zoom and Shift+wheel, it could invent a
  Shift nobody held. The second caller was `ScrollWidget`'s event filter, where the same expression
  made *every* wheel event look unmodified, so a scroll container always swallowed the wheel instead
  of letting a modifier pass it to the widget underneath.
- **Modifiers are tracked from the key events, not from `SDL_GetModState()`** (`SvgGui::keyModState`,
  updated in `SvgGui::sdlEvent`). SDL updates its modifier state when events are *pumped*, while a
  widget reads it when the event is *dispatched*; a modifier released in the same pump as the wheel
  event therefore reads as already up. This was observed, not theorised: Ctrl+Z worked in the same
  session in which Ctrl+wheel scrolled instead of zooming. `keysym.mod` already accounts for the key
  in hand, so a press sets the bit and a release clears it; window focus changes resync from
  `SDL_GetModState()`, since key releases that happened elsewhere were never delivered here.
- `wheelScrollSpeed` is pixels-per-notch over 120 and was 0.2, i.e. 24 px a notch - slow enough to
  read as broken on its own. Now 0.5. Both it and `wheelZoomSpeed` are exposed in the Input prefs.
- **Middle-button pan is a flag on `ScribbleInput`, not a `MODEMOD` bit** (`midBtnPan`), because it
  must survive from the press to the release while `modemod` is rebuilt per event. It joins
  `mouseMode == INPUTMODE_PAN` in the one condition that starts `SCRIBBLING_PAN`, and it also has to
  suppress the early return for `mouseMode == INPUTMODE_NONE` - otherwise the one configuration where
  a pan gesture is most wanted is the one where it is dropped. `fingerId` is the button for down/up
  and the full mask for motion, so `SDL_BUTTON_MMASK` is only ever tested on down and up.
- **The Pan tool already existed and was simply hidden**: it was added to the toolbar only when
  `singleTouchMode` *and* `multiTouchMode` were both 0, i.e. only when touch was disabled entirely.
  That left a stylus-only user - no second finger, no middle button - with nothing but the scroll
  handle. It is now always present. It is not one of the tools (it edits nothing), so it keeps its
  own slot ahead of the tools sub-toolbar and its own `NormalPriority + 1`, which drops it into the
  overflow menu before the tools row is touched on a narrow screen.
- **Finger-pans-while-pen-writes on mobile needed no work** - `ScribbleInput::doInputEvent()` already
  switches `singleTouchMode` (and `mouseMode`) to `INPUTMODE_PAN` the first time a pen event arrives,
  and persists it via `penType`. Both are also exposed in the Input prefs.
- Space+drag, Ctrl+Shift+drag and pan-from-edge remain as they were.

## Two finger tap = undo

`ScribbleInput::doInputEvent` recognises it: exactly two touch points (`twoFingerTap`, set when the second
lands and cleared by a third), under `PANLENGTH_CLICK` of centroid travel and `MAX_CLICK_TIME` from the
second finger down to the gesture finishing. The pan the fingers started is **cancelled, not finished** -
`panZoomFinish` would treat it as a click (following a link under the centroid) or start a fling. With
`singleTouchMode == PAN` the gesture only finishes when the last finger lifts, so the first lift resets
`prevPointerCOM` to the remaining finger, or the centroid jump alone would exceed the click distance.
`ScribbleView::doTwoFingerTap()` is the hook; only `ScribbleArea` acts on it (one `ID_UNDO`).
Pinned by `twoFingerTapTest`.

## The current page follows the middle of the view

`currPageNum` is the page number in the page strip, the target of Add Page, page tags, the outline's "this
page" and everything else that says "current page". In the scrolling layouts (vertical and horizontal)
`ScribbleArea::doPan()` picks it with `dominantPageNum()` (name kept from an earlier largest-area rule): the
**page containing the point at the middle of the view**, via `dimToPageNum()` (scroll axis only).
- **History**: the first rule kept a page current until it left the view shrunk by a sixth per side; the
  second picked the largest visible area with a 2% margin. Both let a sliver of a page decide. The user
  wants the page under the middle of the screen.
- **Gaps**: `dimToPageNum()` gives the gap below (right of) a page to that page, so with the middle in a gap
  the page above stays current. Before the first page the first, past the last the last (the ghost page is
  clamped). The result depends on the view alone, so no hysteresis is needed and it cannot flicker.
- **Single page view** is untouched: there the current page *is* the view.
- **Only a view that moved chooses again** (`pageChoiceView`). `pageSizeChanged()` pans by 0, for example
  when a page grows after a stroke, and must not take the page away from the one just drawn on.
- **Drawing on or tapping a page still makes it current**, even a page showing less than its neighbour.
  It has to: input coordinates are relative to `currPage`. The press handler, `doTouchTap` and the link
  click still call `setPageNum()`. The next scroll or zoom gives the dominant page back.
- **Explicit navigation keeps its page** (`holdPageNum`, set by `HoldPageNum` in `gotoPos`, `viewPos`,
  `viewRect` and `setViewBox`). Go to page, next/previous page, bookmarks, links, page insertion
  (`pageCountChanged` uses `gotoPos`) and sync view boxes can pan to a spot where a neighbour shows more,
  for example to the end of a page. The page asked for stays current until the user scrolls.
- **`alignFitPage()` no longer makes the page under the gesture current.** It aligns that page's rect, and
  the pan or zoom picks the page at the middle as usual. Otherwise a fit snap with no shift left the gesture
  page current with nothing to correct it. A zoom snap also keeps the page at the middle (see "A snap
  keeps the page you are on" below).
- The doPan gate on `currMode` (only `MODE_NONE`/`MODE_PAN`) is unchanged, so edge auto-scroll while dragging
  a selection does not switch pages mid-drag.

Pinned by `currentPageTest`, for both layouts: middle on either side of a page start, in a gap, a sliver at the
edge, explicit jump, drawn-on page. Note that the view mode comes from the *document's* config,
so a test must set `scribbleDoc->cfg` and call `area->loadConfig()`. Setting `area->viewMode` or the global
pref quietly leaves the layout vertical.

## Zoom snapping (`ScribbleArea::roundZoom`)

At the end of a pinch (and after a zoom step) the zoom is rounded: to the nearest zoom step, then to fit
width or fit height when within 10% of either. The fit snaps used to *also* move the view: fit width put
the page's left edge at the border, fit height **centered the whole page**, and both ran whenever the
zoom ended near a fit value - including a two finger pan at fit zoom, whose zoom always wobbles by a
hair. The result was a view that jumped along the page with no zoom change anyone could see.

Now a fit snap zooms about the gesture point, like a step snap, and only then aligns the page, and only:
- when the zoom really changed (`ZOOM_SNAP_ALIGN_MIN`, 2%) - below that it is pan jitter, not a snap;
- **across** the scroll direction (horizontally in the vertical layout, vertically in the horizontal one,
  both in single-page view) - never along it, so the reading position never jumps.

Pinned by `zoomSnapTest` (a 1180x760 view, so fit width and fit height are far enough apart to snap separately).

### A snap keeps the page you are on

"Fit snapping sometimes sends me to another page" had two causes, both fixed in `roundZoom()` and
`wheelZoomFinish()`:
- **The snap zooms about the gesture point, not the middle.** A step rounding plus a fit snap changes the
  zoom by up to ~12%, which moves the document point at the middle of the view by that fraction of its
  distance from the fingers. With the middle a little above a page boundary and the fingers lower down,
  that carried the middle onto the next page, and `doPan()` duly made the neighbour current. Reproduced
  in agent-display with Ctrl+wheel: page 2 started at y=436 (middle at 360). One notch at y=700 put it at
  ~370, and the fit-width snap moved it to 351, so the indicator went from 1/3 to 2/3.
  Now **`keepPageAtMiddle()`** runs after every snap: if the middle is no longer on the page that was
  current when the gesture ended, it scrolls along the scroll axis just far enough to bring the middle
  back to that page's edge, one px inside (the gap after a page belongs to it). If the middle did not
  leave the page, nothing moves, so the "never scroll along the reading direction" rule above still holds
  in every other case.
- **The snap judged the page under the gesture point.** That is a neighbour whenever the fingers are
  over one, and past the last page it is the ghost page, which has no `Page`: `fitWidthZoom()`
  dereferenced NULL. Every Ctrl+wheel notch over the ghost page crashed, because the toast predicate
  calls it too. The snap, the toast (`nearFitWidth()`, which no longer takes a point) and the wheel snap
  now all use `dominantPageNum()`, the page at the middle, so the toast and the snap still agree. A double
  tap still fits the tapped page (clamped to the last real page).
- Not changed: `ScribbleView::zoomTo()` goes through `setZoom()` -> `pageSizeChanged()` -> `doPan(0,0)`
  while the view is zoomed about the top-left corner. For that one call the current page can flip to
  whatever is at that intermediate middle. The compensating pan straight after picks the right page
  again, so only the side effects of `setPageNum()` (stroke grouping, `finishShape()`) see it.

Pinned by `fitSnapPageTest`, in both layouts, for pinch (`roundZoom`) and Ctrl+wheel (`wheelZoomFinish`).
The middle is 12 px before the second page, the fingers are over it, and the zoom is 7% off fit; the test
checks that the first page stays current and the scroll is under 3 px. A second case puts the fingers
past the last page. Against the old code the page check fails in all four combinations and the
ghost-page case crashes.

### Fit width: gapless, a "Fit" toast, and no sideways pan at or below it

- **Fit width has no margin** (`ScribbleArea::fitWidthZoom()`): the page is exactly as wide as the view. It
  used to subtract `2*horzBorder`, so the snap target had a 10 px gray gap either side. `horzBorder` itself
  is still the over-scroll margin when zoomed in past fit.
- **One predicate, `snapsToFitWidth()`, drives both the snap and the toast**: within 10% of fit width,
  fit width not itself within 5% of 100%, and the zoom not rounding to the 100% step (100% wins). The toast
  must never promise a snap that `roundZoom()` will not make, so do not give either its own threshold.
- **The toast** is a `TextBox` built in `ScribbleWidget::create()` (`fitToastSVG`), centered in the
  scribble area's box layout, styled like the floating bar: `.fit-toast-bg` / `.fit-toast-text` in
  `ugui/theme.cpp` use `--floating-bg`, `--floating-outline` and `--text`. It is `hitTransparent`, since it
  appears mid-gesture. `ScribbleView::showFitHint()` shows or hides it: `panZoomMove()` updates it after every
  pinch step, and every way a pinch ends (`panZoomFinish`, `panZoomCancel`, two-to-one finger) hides it.
- **Ctrl+wheel has no release**, so it used to never snap at all. It now updates the toast on every notch, and
  `ScribbleWidget::scheduleWheelZoomSnap()` restarts a 400 ms timer. When the wheel goes quiet,
  `ScribbleArea::wheelZoomFinish()` makes **only** the fit width snap. It does not round to zoom steps,
  because the wheel is meant to be continuous.
- **Double tap** (`doDblClickAction`, reached only through a pan: touch, or the Pan tool with a mouse) zooms
  to the same gapless fit width for the page under the pointer. A second double tap, already at fit, goes
  to 100%, which is what double tap used to do.
- **At or below fit width, horizontal pan is locked** (`ScribbleArea::updateHorzPanLock()`), with
  `minOriginX == maxOriginX`, the visible pages centered, and flush at exactly fit. Vertical pan is
  untouched, and a fling's x component stops because both limits are hit. Traps:
  - **Judge "fit" by the widest page in view, not `contentWidth`.** `contentWidth` is the widest page in the
    *document*, so a single landscape page (a PDF import) unlocked sideways pan on every portrait page.
    That was seen in agent-display on a real document. The lock is therefore re-evaluated in
    `ScribbleArea::doPan()` on every pan, followed by `ScribbleView::doPan(0, 0)` to clamp to the new limits.
    `freeMinOriginX`/`freeMaxOriginX` keep the unlocked limits from `updateContentDim()`.
  - The lock position is "content centered" with `centerPages`, since every page is centered in
    `contentWidth`, and "widest visible page centered" without it.
  - There is a 0.5 px tolerance, because fit width is computed in floating point.
  - The horizontal layout is excluded: there, horizontal is the scroll direction.
- **This changed `test5`'s reference.** Its 600-wide page in the 600-wide test view is exactly at fit, so the
  page is now flush (`xOffset` 0, not -10) and later strokes land 10 units further right.
- **Not testable here: pinch.** agent-display has no multi-touch, so the pinch path of the toast is covered
  only by reading the code and by the shared predicate. Ctrl+wheel, double click (Pan tool) and
  Shift/horizontal wheel at fit were checked in agent-display. Pass `--wheelZoomSpeed=0.5` there: one
  injected notch arrives as two steps (1.25^2), which jumps straight past the 10% window.

## Jump history (back / forward) and the last page button

The bottom-left bar reads `<- -> | < 3 / 12 > >| | zoom ...`: back and forward over *jumps*, the page
stepper, and **Last Page** (`ID_LASTPAGE`, icon `ic_menu_last_page.svg`, hand drawn in reicon's style and
therefore not in `reicon_import.py`'s MAPPING; disabled on the last page).

- **What is a jump**: a programmatic move that skips `JumpHistory::MIN_JUMP_PAGES` (2) or more pages -
  outline entry, bookmark (`bookmarkHit` -> `viewPos`), Last Page, Home/End, a Pages-view cell, a page card
  in the document browser. Scrolling, zooming, next/prev page, tab switches and page moves are **not**
  recorded. They route through `ScribbleArea::jumpToPage()` / `ScribbleDoc::jumpToPage()` (which also
  refreshes the UI) or `gotoPos(..., savepos=true)` / `viewPos()`, all of which call `recordJumpTo()`
  *before* the view moves. A new moving-code path that should be a jump must do the same - plain
  `gotoPage()` never records.
- **Logic is `syncscribble/jumphistory.h`**, pure (no document, no GL): `backStack` / `forwardStack`
  like a browser, a recorded jump empties the forward stack, `back(current, &target)` pushes `current`
  onto forward. It lives in `ScribbleDoc::jumpHistory` - per document, so it follows a tab and is cleared
  in `closeDocument()`; not persisted. Locations are `(page, corner position in page units)`; on
  restore the page is clamped to the current page count (pages may have been added or removed since).
- This **replaces the old `posHistory`** in `ScribbleArea`, which only existed behind the Previous/Next
  View menu items (Backspace / Shift+Backspace, still wired to `ID_PREVVIEW` / `ID_NEXTVIEW`) and
  recorded any move of half a screen, which is not what a "jump" means to a user. The buttons enable
  from `UIState::prevView` / `nextView`.
- Tests: `scribbletest/jumphistorytest.cpp` (standalone command in its header; also in `runAll`).
  Mutation-checked: not clearing the forward stack on a new jump fails it. The real view (buttons
  dim/enable, position restored) was checked in agent-display: Last Page from 7/12 -> back enabled,
  Back -> 7/12 with Forward enabled, Forward -> 12/12. Note a Debug (ASAN) build is slow enough that
  queued clicks arrive many seconds late - wait and re-screenshot before concluding a click was lost.

## Testing this (agent-display)

Three gaps in `tools/agent-display.sh` had to be closed before any of the above could be checked, and
they are worth knowing about:

- **`scroll` sends `axis_discrete`, not just `axis`.** Xwayland turns discrete steps into X11 buttons
  4-7; a bare `axis` event is a continuous scroll and an X11 client - which Write is, inside cage -
  never sees it. `scroll` also takes an optional `X Y` first, for the same reason `click` does.
  `DY` is in wl axis units, so **negative scrolls up**.
- **`drag` takes a trailing button name**, so middle-button pan can be exercised at all.
- **`keydown`/`keyup`** exist so a modifier can be *held* across other commands; `key` sends a whole
  chord and cannot express Ctrl+wheel. Always pair them.
- **Do not drive the wheel with `xdotool` while agent-pointer is also in use.** Each agent-pointer run
  creates and destroys a virtual pointer, and this Xwayland miscounts the resulting enter/leave
  (`BUG: xwl_seat->pointer_enter_count == 0` in the session log). Once that fires, XTEST has no
  pointer to drive: keystrokes still arrive but wheel and motion silently do not, and the app looks
  broken when it is fine. Drive the wheel through `agent-display.sh scroll` and use `xdotool` only
  for keys. A `hold X Y` action exists on `agent-pointer` to park a persistent pointer, but it
  *suppresses* the transient pointers, so it is not a workaround - restart the session instead.
