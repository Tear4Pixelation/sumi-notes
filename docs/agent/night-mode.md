# Night mode

View > Dark Mode (config `invertColors`, global, per device; the key keeps its old name so the setting survives). A **view setting, never a document
edit**: nothing is saved, synced or undoable, and exports, thumbnails and copies keep the document's
own colors. That last property is what the planned clip/screenshot feature relies on - it captures from
the document, so there is no inverted state to undo first.

## How it works

- `ulib/painter.h` has a `ColorMap` hook (`Painter::setColorMap`) applied in `colorToNVGColor()`, the one
  funnel for every solid fill, stroke and gradient stop. It replaced `colorXorMask`.
- `ScribbleView::doPaintEvent()` installs `viewColorMap()` for the canvas only; `ScribbleArea` returns its
  `NightColorMap` (`syncscribble/nightmode.cpp`).
- **Themed document:** drawn in its mirrored theme (`paperL -> 1 - paperL`). Ink maps by ordinal and
  variant through `Palette::mapFrom()`; paper/rule/bookmark/link/selection map slot to slot.
- **Everything else** (unthemed ink, off-palette strokes, PDF vector content): OKLab lightness flipped onto
  `[NIGHT_PAPER_L, NIGHT_INK_L]` with hue kept, chroma reduced to fit the gamut. Blue stays blue - the
  old XOR turned it yellow.
- **"Force dark", not "flip":** a themed document with dark paper, or an unthemed one whose current page is
  dark, is drawn unmapped.
- The gray canvas around pages is UI chrome: drawn unmapped, and always the dark gray in night mode.

## Images (PDF pages, scans, pictures)

- `ColorMap::imageKey()`/`mapImagePixels()`; `Painter::drawImage()` builds a rewritten texture once and caches
  it on the `Image` as `mappedHandle`/`mappedKey`, next to the plain `painterHandle`. **A texture, not a pixel
  copy** - keeping dark pixels in RAM would be ~9 MB per A4 page at 150 dpi, on top of the original.
- `imageKey()` is a generation bumped on every palette change, so textures made under another theme redo.
- **Detection** (`isDocumentLike()`, 64x64 samples): flip if at least half the opaque samples are light *and*
  unsaturated (paper), or at least half are transparent (ink on a keyed-out background - future clips).
  Photos and pastel illustrations are left alone; the decision is cached as `mappedHandle == UNMAPPED`.
- **Flip:** each pixel shifts by `lut[luma] - luma`, the LUT being `flipLightness()` on grays. Grays land
  exactly where ink-drawn paper does; colors keep their channel differences, so hue survives. OKLab per
  pixel was avoided on purpose (~10x slower).
- In a themed document the flip range starts at the *mirrored theme's paper* lightness, not
  `NIGHT_PAPER_L`, so a PDF page's white matches the paper around it.
- Tested standalone by `scribbletest/nightmodetest.cpp` (command in its header); each check was confirmed to
  fail against an always-flip detector and against a per-channel (XOR-style) flip.

## Other things drawn outside the map

- The page drop shadow (`Page::draw`) - flipped, black became a white glow on the page edge.
- The gray canvas between pages.
- Multi-stop gradient textures are **not cached** while a map is set: they bake the stop colors in, so a
  cached one keeps whichever mode it was first drawn in. The linear-interpolation path also used to skip
  `colorToNVGColor()` entirely; it now maps stops too.

## UI showing document colors

Pen swatches (palette row, grids, single swatches incl. the selection popup's), the relative-width preview
and `PenPreview` go through `ScribbleApp::displayColor()` / the canvas map, so they match the ink as drawn.
`toolColor()` is **not** mapped - it is compared against `pen.color`, so mapping it would break selection
of the current swatch. Custom color pickers show the real color being edited. The toggle calls
`PenToolbar::refreshDisplayColors()`. `PenPreview::colorMap` is a function, not a pointer, because the view
owning the map can be destroyed while the preview lives on.

## Cost

One map call per fill/stroke, not per pixel. A last-color memo plus a bounded hash map, so after the first
frame it is a compare or a lookup. OKLab math only runs once per distinct color.

## Known gaps

- No per-image "don't invert" override yet, for when detection guesses wrong.
- Rendering on a non-caching painter redoes the pixel flip every frame (the caching painter is the screen,
  so this is theoretical today).
- The mirrored theme's rule lines are very faint on its near-black paper.
- `BookmarkView` does not install the map.
- The theme dialog still has a "Dark paper" checkbox that edits the document; it is now authoring a dark
  document, not the way to read in the dark.
- Before this, Invert Colors on a themed document restyled the file to its dark variant. Documents saved
  that way stay dark (and night mode leaves them alone).
