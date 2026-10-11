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
   folded into `layerDirtyDim` for the next pan (`layerDeferredFrame`). A pan that moves the view more
   than half (GL) or a quarter (SW) of its size since the last frame is drawn directly too: little
   survives, and on SW the composite - nanovg_sw blends the image pixel by pixel - costs about as much
   as drawing sparse content (callgrind: SW pan at 16 px/frame -91%, but thirds of the view +24% before
   this rule). A pan that would need the whole layer redrawn (first pan after a zoom) is drawn
   directly as well; the next pan builds the layer (`layerBuildNext`).
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

## Selection drag layer (float layer)

Dragging a selection by a move used to redraw every selected stroke every frame, twice each in the
selected style (outline, then white): ~57 ms a frame (GL, 213 ms SW) for the 1521 perftest strokes. Now
the selection is drawn **once** into a second, transparent `CanvasLayer` (the *float layer*,
`ScribbleView::prepareFloat()`), and the canvas layer is drawn **without** it; a frame is then the two
composited, plus the overlays. ~3 ms GL, 8 ms SW. Released, everything is drawn normally again.

- **When** (`ScribbleArea::wantSelFloat()`): on the first move of a `MODE_MOVESELFREE/RULED` drag inside
  the page, for a plain selection (`STROKEDRAW_SEL`, not a ruling region's carry - that moves ruling too)
  of at least `selLayerMinStrokes` (48) elements, a group counting its members, none blending with what is
  under it (`comp-op`, legacy markers: multiply over transparent would not be multiply over the paper),
  page `scaleFactor` 1. Smaller selections, scale and rotate handles, and the overlay drag off the page
  (which calls `endSelFloat()` and comes back to it inside) behave exactly as before. `SUMI_SEL_LAYER_MIN`
  overrides the threshold, `-1` turns it off (A/B).
- **Visible while it floats, approved**: the selection is above all other ink (z-order), and is moved by
  whole device pixels - drawn at the offset of its first frame, composited at the rounded difference, so it
  can sit up to half a pixel from where it is (sharp, not resampled). The release draws it exactly.
- **Leaving it out of the canvas**: `SvgPainter::skipNode` (usvg) skips a node and its subtree without
  touching its rendered bounds; `ScribbleArea::drawImage()` sets it, for the selection's top level
  elements, only while `drawingLayerContent` (the canvas layer render and its edge strips) - never for
  other views, thumbnails or the direct path. A frame that falls back to drawing directly draws the
  selection in place as before, so correctness never depends on the layer being used.
- **Dirt** - the invalidation contract above still holds, plus:
  - Moving the floating strokes changes nothing in the canvas, so their page dirt is **left out of this
    view** (`ScribbleArea::dirtyPage()`): dirt contained in where the drag started, where the float was last
    drawn and where the selection is now (their bounding box) is kept in `selFloatDiscarded` instead.
    Anything else passes through whole (redrawn, correct, just slower). Other views get it as always.
  - Where the float layer and the selection background move goes to `overlayDirtyScreen`: repainted by the
    GUI (`ScribbleWidget::dirtyRect()`), **not** drawn into the canvas layer. Routing it to
    `dirtyRectScreen` instead would redraw the canvas under the selection every frame - correct, and
    most of the gain lost.
  - `startSelFloat()` flushes the page's earlier dirt to every view first, then dirties the selection's
    bbox as content (the canvas there, now without it - needed when the canvas layer was built with the
    selection in it, e.g. after a scroll). `endSelFloat()` (release, `clearSelection()`, or `reqRepaint()`
    noticing the drag ended some other way) dirties start and end bbox and the selection background with
    its handles as content, plus `selFloatDiscarded` if the page's `revision` changed meanwhile (a change
    from elsewhere, e.g. sync, may be in what was left out).
- **Layer details**: covers the selection's device bbox (+3 px) clipped to the view plus half its size
  around it, so a selection much bigger than the view needs no huge texture; drawn again when the offset
  would expose a part it lacks, or zoom, painter transform or night mode map change. GL: one FBO texture,
  cleared to transparent (scissored `glClear`) before drawing, premultiplied like nanovg's FBO images.
- **SW trap - premultiplied vs straight**: nanovg_sw blends as if the destination were premultiplied, so
  drawing over transparent pixels leaves premultiplied color, but it reads every image (NOCOPY ones as
  they are) as straight alpha. Composited as is, antialiased edges get their alpha twice - thin dark edges,
  up to ~60 levels in night mode. `CanvasLayer::unpremultiplySW()` divides the partly covered pixels by
  their alpha in the space nanovg_sw blended in (linear with its 2.31 gamma for an sRGB painter).
- What is left after it: per move, `Selection::setOffset()` over every stroke and the page dirt walk -
  the same as before, and the reason a press-move-release without frames in between (`perf_move_selection`)
  is unchanged. The first frame of a drag builds both layers (SW: `big_move_sel_event_max` 8 -> 16 ms).

## Testing

`Sumi --canvaslayertest` (`scribbletest/canvaslayertest.cpp`, headless via `tools/perf-bench.sh --mode
canvaslayertest --runs 1`, add `-- --glRender=0` for SW): ~35 steps (scrolls of many sizes, diagonal
pans, strokes, stroke and scroll in one frame, selection, move, undo, night mode, zoom, dotted paper, a
PDF page), each compared with a direct full repaint that leaves the layer alone
(`ScribbleView::testDirectPaint`). `layer_bad_steps` must be 0. Removing the fold in `doPan()` gave 5 bad
steps, removing the fold of a deferred frame's dirt 4 - both checked. `SUMI_NO_CANVAS_LAYER=1` turns the layer off for A/B comparisons.

The selection drag layer has its own steps (`selDragLayerTest()`, `layer_seldrag_*`), on GL and SW: mid
drag the frame against a direct repaint (select all, moves of whole device pixels - `paintScale` is not 1
headless, so screen pixels are not device pixels - and differences under 3 levels ignored: 8-bit
premultiplied storage); the canvas layer alone (`testSkipFloat`) against a direct repaint without the
selection (`testHideFloat`), which must differ from one with it; the drop against a direct repaint; the
page after the drop byte-identical to the same drag with the layer off, and after its undo. Also a
fractional zoom, night mode, a partial selection among other ink, a drag partly off the page and onto the
next, and strokes added from elsewhere mid drag (outside the selection: shown at once; inside: after the
drop). Each drag starts from a canvas layer built with the selection in it (two small scrolls). Checked to
fail: no `skipNode` (9 bad), no float composite (3), a drop offset by 0.01 (1), no start dirt (3),
discarding all page dirt (1). Popups are kept off the view there: on SW the view drawn under a popup bleeds
a row into it, with or without the layer.

## Related: dotted paper

Dotted paper is one path of all its dots (paper.md). `SvgPainter::_draw(SvgPath)` drops the subpaths
whose bounds miss the dirty rect (padded by the stroke's reach) before drawing, for paths of >= 64 points
not entirely inside it - exact for fills, skipped with dashes, patterns and inside `<use>`.
`SvgPainter::cullSubpaths = false` disables it (the canvas layer test's reference draws every dot).
