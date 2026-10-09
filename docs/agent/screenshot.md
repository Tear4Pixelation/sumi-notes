# Screenshot

Capture an area of a page - ink plus any PDF or scan image under it - and copy it or add it to a page as
an image. Made for copying an exercise off a worksheet onto the page you are working on.

## Flow

1. **Lasso or rectangle select.** On pen-up the gesture's outline is kept as `ScribbleArea::shotRegion`
   (page coordinates, `shotRegionPage`) and drawn dashed in `drawScreen`. It stays where it was drawn when
   the selection is moved - the "original selection intact" the feature asked for.
2. **A gesture that caught no ink** is not thrown away as before: the region is kept and the selection
   popup opens with only the camera button (`refreshSelPopup()` hides `selInkButtons`, Move to Layer and the
   color/width items when there is no ink).
3. **Camera button** (`actionScreenshot`, icon `ic_menu_screenshot` = reicon `camera`) ->
   `ScribbleArea::screenshotSelection()`. Captures the region's **bounding box**. A ruled or path selection
   has no region, so it falls back to the selection's bbox padded by `SCREENSHOT_INK_PADDING` (~4 mm) -
   unpadded, the capture ends exactly at the strokes and looks cut off.
4. **`ScreenshotDialog`** (`screenshot.cpp`): the capture centered, four corner handles, free-aspect crop,
   an X (cancel) and the title top left, Copy and Add to page top right (a header row built in the body;
   the popup dialog's own button row and title are hidden, and its vertical padding trimmed to
   `HEADER_PADDING`). The pushbuttons get `box-anchor="vfill"`: `#pushbutton` is `fill`, which in a row
   makes them share the free width with the stretch and look over-padded. Enter = Add to page, Esc = cancel. Checkboxes below: **Ruling** (off), **PDF / image
   background** (on), **Annotations** (on), and **Keep white** (off; only offered when the page has a PDF/scan image); each change re-renders through the dialog's `RenderFn` at the same
   size, so the crop (in pixels) carries over.
5. **Copy** -> app clipboard as one `SvgImage` element at its real size and original position, so Paste in
   another page lands where it was. **Add to page** -> pasted at the middle of the view, selected.

## Rendering (`renderPageRegion`)

- A fresh software `Painter`, so **no night mode map**: the capture is always the document's own colors,
  whatever the view shows. That is why there is no "uninvert first" step.
- `ScreenshotLayers`: `SHOT_RULING` draws `ruleNode`'s non-image children except `.pagerect`,
  `SHOT_BACKGROUND` its images (PDF page, scan), `SHOT_INK` the `contentNode`. The **paper is never drawn** -
  it belongs to the page copied *from*. Hidden layers stay hidden (node-level display).
- `Element::FORCE_NORMAL_DRAW` so selected ink is not drawn with selection styling.
- ~300 dpi (`SHOT_SCALE = 2` pixels per page unit, 150 units per inch), capped at 4000 px on the long side.
- Rendered on white, then **`whiteToAlpha()`**: color-to-alpha against white, so the clip looks identical
  on white paper, sits cleanly on tinted paper, and night mode's image detection sees it as mostly
  transparent - a document - and flips it.
- **Keep white** (`SHOT_KEEP_WHITE`, effective only with the background layer) skips `whiteToAlpha()`, so the
  PDF page keeps its opaque white (and the area outside the image is white too).

## Traps

- **Crop handles must sit inside the widget.** With the image filling the widget, each handle was half
  outside it and presses there never arrived. `IMAGE_INSET` fixes it.
- **Copy must call `app->refreshUI(doc, UIState::ClipboardChange)`**, not `uiChanged(Command)`: the Paste
  action (and Ctrl+V) stays disabled otherwise until some other command refreshes it.
- **The region is an overlay**, so clearing it needs `dirtyScreen()` + `doRefresh()`; `dirtyPage()` left
  the dashed outline on screen.
- A region with nothing selected is dropped on the next canvas press, Esc (`doCancelAction`), an outside
  press on the popup, or after the screenshot. Don't clear it on the popup's `INVISIBLE` - the menu may
  close before the camera button's action runs.
- A debug build quits on an Esc nothing else handles (`SCRIBBLE_DEBUG` in `keyPressEvent`) - not a crash.

## Not done

- No clipping to the lasso shape; the capture is always the bounding box (as specified).
- Add to page uses the active view; in split view, Copy and paste in the other pane.
- `whiteToAlpha()` has no standalone test yet (it lives with the dialog, which needs the GUI to link).
