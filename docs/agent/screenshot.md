# Screenshot and image editing

Capture an area of a page - ink plus any PDF or scan image under it - and copy it or add it to a page as
an image. Made for copying an exercise off a worksheet onto the page you are working on. The same dialog
(`ImageCropDialog`, `screenshot.cpp`) also edits images already in the document: crop, reshape, uncrop.

## Flow

1. **Lasso or rectangle select.** On pen-up the gesture's outline is kept as `ScribbleArea::shotRegion`
   (page coordinates, `shotRegionPage`) and drawn dashed in `drawScreen`. It stays where it was drawn when
   the selection is moved - the "original selection intact" the feature asked for.
2. **A gesture that caught no ink** is not thrown away as before: the region is kept and the selection
   popup opens with only the camera button (`refreshSelPopup()` hides `selInkButtons`, Move to Layer and the
   color/width items when there is no ink).
3. **Camera button** (`actionScreenshot`, icon `ic_menu_screenshot` = reicon `camera`) ->
   `ScribbleArea::screenshotSelection()`. The **selection** is the region's bounding box; a ruled or path
   selection has no region, so it is the selection's bbox padded by `SCREENSHOT_INK_PADDING` (~4 mm) -
   unpadded, the capture ends exactly at the strokes and looks cut off.
4. **The capture is wider than the selection**: padded by half the selection's longer side, at least
   `SCREENSHOT_MARGIN_MIN` (75 units, ~1/2 inch) and at most `SCREENSHOT_MARGIN_MAX` (300, ~2 inches), then
   clamped to the page. The crop starts **on the selection** (`initialCrop`, in capture pixels), so Add to
   page right away gives exactly what was selected, and dragging a handle outwards takes in the
   surroundings (a heading above an exercise, a figure beside it). The scale is worked out for the wider
   region, so a large selection loses a little resolution to the margin (`SHOT_MAX_DIM` caps the long side).
5. **The dialog** (below). Copy -> app clipboard as one `SvgImage` element at its real size and original
   position, so Paste in another page lands where it was. Add to page -> pasted at the middle of the view,
   selected. Either way the whole capture goes along as the image's kept original (see below), so Edit
   image can still widen it later.

## The dialog (`ImageCropDialog`)

One class, two constructors: a screenshot (`RenderFn`, layer checkboxes, Copy / Add to page) and editing
an image (its original pixels and current shape, Apply). Everything else - the crop widget, the shape bar,
the header row - is shared, not copied.

- **Header row** built in the body: X (cancel) and the title top left, the choices top right. The popup
  dialog's own button row and title are hidden, and its vertical padding trimmed to `HEADER_PADDING`. The
  pushbuttons get `box-anchor="vfill"`: `#pushbutton` is `fill`, which in a row makes them share the free
  width with the stretch and look over-padded. Enter = Add to page / Apply, Esc = cancel. The choice
  buttons are disabled while the shape encloses nothing (a polygon of fewer than three points).
- **`ImageCropWidget`**: the image centered (never blown up past 2x), on white - paper, not UI: the image
  is transparent where its paper was, and this is how it looks on light paper. What the shape cuts away is
  shaded (an even-odd path: the image rect with the outline as a hole). Handles are `HANDLE_RADIUS` circles,
  hit within `TOUCH_RADIUS`.
- **Shape bar** under the image, built as the reference floating bar ([ui-floating-bar.md](ui-floating-bar.md)):
  rounded container, 44x44 `TagDocList::createSelectBarButton` buttons, separators. From the left: a
  fixed-width hint that carries the state ("Drag the corners" / "Tap to place points" / "Drag a point, tap
  to add"; `HINT_WIDTH` so the bar never changes width), **Rectangle** and **Polygon** (checked state shows
  the current one), the corner-radius icon and slider (the ruling region panel's slider), and **Remove last
  point** (disabled, not hidden, when there is none).
- **Checkboxes** (screenshot only): **Ruling** (off), **PDF / image background** (on), **Annotations** (on),
  and **Keep white** (off; only offered when the page has a PDF/scan image); each change re-renders through
  the dialog's `RenderFn` at the same size, so the crop (in pixels) and the polygon carry over.

### Crop shapes (`cropshape.h`, pure math)

- **Rectangle**: four corner handles, free aspect; a corner may not cross the opposite edges
  (`MIN_CROP_PX`).
- **Polygon**: a press adds a point at the end, or - within `EDGE_TOUCH_RADIUS` of an edge - inserts one
  into that edge; a press on a point drags it. A new point is dragged too, so press-and-slide places it
  precisely. The polygon closes back to its first point. It is filled **nonzero**, so a self-crossing
  polygon (a star drawn point to point) is solid rather than hollow. Switching shapes keeps both: the
  polygon's points stay for when it is picked again, and a polygon opened for editing gives the rectangle
  its bounds.
- **Rounded corners** for both: every corner (concave ones too, which then fill in the notch beside them)
  becomes a circular arc of the radius, shrunk per corner so a fillet never passes the middle of an edge
  (`roundedOutline()`). The slider is 0 to half the shape's shorter side; the radius itself is kept in
  image pixels, so it survives a re-edit.
- **Result** (`cropToShape()`): cropped to the outline's pixel bounds, then `applyOutlineMask()` multiplies
  alpha by coverage sampled 4x4 per pixel (anti-aliased edges); pixels with no coverage become (0,0,0,0).
  Pixels outside the shape are transparent, so the result is a PNG. A plain rectangle with no radius is
  only cropped and keeps the source's encoding (a cropped photo stays a JPEG).

## Image editing

**Entry point**: the selection popup's **Edit Image** (`actionEditImage`, icon `ic_menu_image`), shown only
when the selection is exactly one image whose pixels are in the document (`ScribbleArea::selectedImage()`;
a linked image is not offered - SvgWriter writes its link back, not its pixels). The selection handles
were not touched; the button sits after the camera.

`editSelectedImage()` opens the dialog on the image's **original pixels** and **current shape**: the kept
original if the image has one, else the image itself with any crop its handles made (`srcRect`) as the
starting rectangle. `applyImageEdit()` then swaps a clone with the new pixels in for the image as **one
undo step** (`addStroke(clone, old, layer)` + `removeStroke(old)` in one action, as `toggleTodoTag` does),
so undo, redo and sync are the ordinary add and delete, and the clone keeps the transform and the layer.
The new bounds keep the original's pixel grid where it was on the page: `shownPx` (the original's pixels
the old bounds showed) maps pixels to the image's coordinates, so shrinking the crop leaves the rest in
place and widening it brings back what was cut away, in its old position.

### Original pixels (decision)

An edited image keeps its original **in the document**, as two attributes on the `<image>`:

- `__crop` (`IMAGE_CROP_ATTR`): the `CropShape` in the original's pixels -
  `rect R l t r b` or `poly R x1 y1 x2 y2 ...` (R = corner radius).
- `__croporig` (`IMAGE_ORIG_ATTR`): the original as a `data:image/...;base64,` URI - its file bytes when it
  has them (a photo's JPEG is not recompressed), else PNG.

Underscored like `__layer`: our attributes on a standard element, ignored by other SVG readers, and kept
by usvg's generic attribute handling through save, load, clone, undo and sync. A later edit reuses the
stored URI as is rather than re-encoding it, and an edit back to the whole, unrounded original drops both.
The cost is size: an edited image is stored about twice (the original plus the cut), and the original is
never reduced by "Reduce image size" (`saveImageScaled`) - that applies to the displayed pixels only.
Accepted because uncropping is the point, and the alternative - keeping it only in memory - loses it at
the first save. `memSize()` (undo budget) counts the attribute.

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

## Theming

No literal UI colors. `imageCropStylesheet` (in `screenshot.cpp`, added to the app stylesheet in
`createStylesheet()` like `TopBar::stylesheet`) sets, on the widget's node `.imagecrop-view`, `fill`
(backdrop, and the shade at `OUTSIDE_SHADE_ALPHA`) = `--canvas`, `stroke` (outline, handle rims) =
`--checked`, `color` (handles) = `--on-checked`; `draw()` reads them back with `getColorAttr()` - the way
a `CustomWidget` gets theme variables, since it paints with a `Painter` rather than SVG nodes. The shape
bar's rules are the select-mode bar's scoped to `.imagecrop-bar`, plus `.checked` and the slider colors
from the floating-bar variables. Only the white behind the image is fixed, as it stands for paper.

## Tests

- `scribbletest/cropshapetest.cpp` (standalone, command in its header; also in `runAll()`): rect mask and
  exclusive edges, anti-aliased partial pixels, nonzero fill of a self-crossing star, the rounded corner
  cut along the arc, an oversized radius clamped to a circle, a concave corner filling its notch, pixel
  bounds, and the `__crop` format. Mutation-checked: even-odd fill, no clamp, rounding skipped, and one
  corner direction skipped each fail at least one check.
- `ScribbleTest::imageCropTest()` (in-app): the shaped result's bounds and transparency, the kept original
  and shape through `SvgWriter`/`SvgParser`, both attributes dropped for the whole image, and
  `applyImageEdit()` as one undo step that keeps the kept pixels in place.
- The dialog's interaction has no automated coverage; verified in agent-display (screenshot with widened
  rounded crop, Add to page, Edit Image reopening the whole capture with its shape, polygon with rounded
  corners, Apply).

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
- **Edit re-checks the selection after the dialog** (`selectedImage() != target`): the dialog pumps
  events, and sync or another view could have taken the image away meanwhile.
- A debug build quits on an Esc nothing else handles (`SCRIBBLE_DEBUG` in `keyPressEvent`) - not a crash.
- **Driving it in agent-display**: run with `-- --historyHintDone=1`. The history hint is an arrow popup
  that comes back with new history and swallows the next canvas press as an outside press, so lasso and
  rectangle selections silently fail.

## Not done

- No clipping to the lasso shape; the capture is always the bounding box (the polygon crop can trace it).
- Add to page uses the active view; in split view, Copy and paste in the other pane.
- `whiteToAlpha()` has no standalone test yet (it lives with the dialog, which needs the GUI to link).
- Erasing pixels of an image (`ERASE_IMAGES`) changes only the displayed pixels; a later Edit image
  starts again from the kept original without the erasure.
- A rotated or mirrored image (a transform on the node) is edited in its own pixel frame; the dialog
  shows it unrotated.
