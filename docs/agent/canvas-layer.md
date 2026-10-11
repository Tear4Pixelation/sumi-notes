# Canvas layer (retained content)

Panning used to redraw every visible stroke each frame: `doPan()` dirtied the whole view and
`doPaintEvent()` ran `drawImage()` over all of it. Now each `ScribbleArea` keeps its `drawImage()`
output from frame to frame in a `CanvasLayer` (`canvaslayer.h`, implemented in `application.cpp`):
two FBO textures on GL (ping-ponged by a `glBlitFramebuffer` shift), one pixel buffer on SW. A pan
shifts the layer and draws only the uncovered strip. `drawScreen()` overlays (selection background,
in-progress stroke, cursor, region handles) are never part of it.

## Frame flow

1. `SvgGui::layoutAndDraw()` lays out, computes the frame's dirty rect, then calls
   `onBeforeDraw` (set in `SDL_main`) **before** `painter->beginFrame()` - nanovg frames cannot nest,
   so the layer is drawn in its own frames on the same painter (the caching painter, so image textures
   and night-mode textures are shared).
2. `ScribbleView::prepareLayers()` -> `prepareLayer()` for each shown view the frame repaints. **Only a
   pure pan uses the layer** (`layerPanned` and `mScale` unchanged since the last paint): shift by the pan
   since the layer was drawn (whole device pixels - `doPan()` quantizes to `unitsPerPx`), draw the
   exposed strips plus the pending dirt. Every other frame (pen, eraser, selection, zoom) is drawn
   directly as before - the layer would only add a second nanovg frame and a composite - and its dirt is
   folded into `layerDirtyDim` for the next pan (`layerDeferredFrame`). A pan that would need the whole
   layer redrawn (first pan after a zoom, a jump) is also drawn directly; the next pan builds the layer
   (`layerBuildNext`).
3. `doPaintEvent()` composites the layer 1:1 over the device pixels wholly inside the view, then draws
   any fractional edge band directly (see traps), then `drawScreen()`.

## Invalidation (what has to stay true)

- **Content changes reach the view only through `dirtyRectDim` and `dirtyRectScreen`.** The GUI knows
  nothing else (`ScribbleWidget::dirtyRect()`), so both are drawn into the layer. A new way of changing
  what `drawImage()` shows must dirty one of them, as it already had to.
- **`doPan()` no longer sets `dirtyRectScreen = screenRect`.** It folds the pending screen dirty rect into
  `layerDirtyDim` (Dim coordinates - the screen position is about to change) and sets `layerPanned`,
  which makes `ScribbleWidget::dirtyRect()` report the whole view and `reqRepaint()` mark it dirty.
  Without the fold, a stroke and a scroll in the same frame leave the stroke stale (the test below fails).
- Zoom (`mScale`), origin, view size or position (painter transform), and the night-mode map (pointer and
  `imageKey()` generation) redraw the whole layer. Night mode toggling only calls `redraw()` on the
  window - nothing dirties the canvas - so the map check is what catches it.
- Anything unexpected - painter transform at paint time differs from the one the layer was drawn for, a
  view painted that was not prepared this frame (`layerFrame != guiFrame`) - falls back to drawing
  directly, exactly as before, and redraws the whole layer next time. The transform must be the same for
  two paints in a row (`paintStable`) before a layer is used, so a resize animation does not draw both.
- A view that is no longer shown (hidden tab) frees its layer.

## Traps

- **Fractional edges are drawn directly.** `Painter::clipRect()` rounds clips to whole pixels, and the
  direct path's unantialiased background fill stops at the view's (fractional) edge, leaving what is
  under the view in the rest of that pixel. A composited layer pixel does not match, so the band of
  partial pixels along such an edge is drawn directly, its outer side clipped only by the widget's own
  clip. (Drawing directly, nanovg_sw also bleeds a row past the widget clip, depending on float
  rounding of the scissor - outside the view, and erratic in the old code too.)
- **Dirty rects passed to `drawImage()` are 2 device px larger than the clip.** nanovg_sw's integer
  bounds are inclusive (`ceil(bottom)`): it paints a row past the clip, and with the old 1-unit pad the
  page fill landed there while the rule line under it was culled - a missing rule line after scrolling.
- **SW layer byte order** must match the screen surface (`NVG_IMAGE_NOCOPY` images are read as is), so
  the layer is rendered with the surface's channel shifts (`setScreenSWFramebuffer`).
- **GL shift**: sRGB conversion is turned off during the blit so it is a raw copy.

## Not bit-exact, by design

Shifted content was rasterized at another offset, and rasterizing is not invariant under whole-pixel
translation: on GL a few antialiased edge pixels differ by up to ~5/255; on SW up to ~10% on the two rows
where nanovg_sw's thread tiles meet (`h/2+1`), since its per-tile edge setup is not seam-invariant. Both
are invisible; a stale strip or a missing line is not, and the test tells them apart.

## Testing

`Sumi --canvaslayertest` (`scribbletest/canvaslayertest.cpp`, headless via `tools/perf-bench.sh --mode
canvaslayertest --runs 1`, add `-- --glRender=0` for SW): ~35 steps (scrolls of many sizes, diagonal
pans, strokes, stroke and scroll in one frame, selection, move, undo, night mode, zoom, dotted paper, a
PDF page), each compared with a direct full repaint that leaves the layer alone
(`ScribbleView::testDirectPaint`). `layer_bad_steps` must be 0. Removing the fold in `doPan()` gave 5 bad
steps, removing the fold of a deferred frame's dirt 4 - both checked. `SUMI_NO_CANVAS_LAYER=1` turns the layer off for A/B comparisons.

## Related: dotted paper

Dotted paper is one path of all its dots (paper.md). `SvgPainter::_draw(SvgPath)` drops the subpaths
whose bounds miss the dirty rect (padded by the stroke's reach) before drawing, for paths of >= 64 points
not entirely inside it - exact for fills, skipped with dashes, patterns and inside `<use>`.
`SvgPainter::cullSubpaths = false` disables it (the canvas layer test's reference draws every dot).
