# Tape

The **Tape** tool lays a strip of tape over part of the page, like masking tape over the answers when
studying. A strip is either **solid** (opaque, drawn over everything on the page) or **see-through**
(a dashed outline in the same color); a press that does not drag flips it.

## The tool is a draw tool

`ScribbleMode::DRAWTOOL_TAPE`, with its own pen `ScribbleMode::tapePen` carrying the `ScribblePen::TAPE`
flag. Being a draw tool (`MODE_STROKE` as far as the toolbar and `ScribbleMode` know) is what gives it the
whole pen options row for free: color, width presets, **Relative size**, and the marker's **centre on line**
toggle (`PenToolbar::centerLineToggle`, now shown for the marker and the tape). The press is where it
diverges: `ScribbleArea::doPressEvent` sees the `TAPE` pen in the `MODE_STROKE` case and switches the
gesture to `MODE_TAPE`, which is a mode like `MODE_SMARTSCALE` - appended to the enum, never a tool,
`getModeType()` = `MODE_STROKE`.

- **Width is relative by default, like the marker** (pen-and-tools.md, "Relative pen width"). The default is
  0.9 of a line; the presets are `savedTapeWidths` (`TAPE_WIDTHS` = 0.6/0.9/1.2 line heights), a list of its
  own because two tools can be in different units at once. `resolvedPen()` multiplies, as for every draw
  tool, so the document only ever holds absolute geometry. Unlike the marker the flag is not forced on load
  (`readPen(ss, tapePen, TAPE)`): a pen saved with the toggle off holds an absolute width, and forcing the
  flag would reinterpret it in line heights.
- **Centre on line** is the marker's logic reused: the press snaps to the vertical centre of the ruled line
  it lands in (`gestureFrame.yForLine(prevLine) + yr/2`, into `centerLineLocalY`), and every move keeps that
  local y, so the strip is horizontal along the line - tilted in a tilted Paper Patch, as the marker is.
  Without it the strip follows the drag in any direction.
- The pen is saved at the end of the `toolModes` string (after everything an older version reads), and
  `drawTool` accepts `DRAWTOOL_TAPE`; an older build ignores both and falls back to the pen.

## The color is the theme's until the user picks one

`tapePen` starts with an **invalid color**, which means "the document theme's": `ScribbleDoc::tapeThemeColor()`
is the palette's first family (the seed hue), opaque, falling back to the neutral. It is resolved in two places
only: `ScribbleArea::resolvedPen()` (what gets laid) and `ScribbleApp::updatePenToolbar()` (what the row
shows). `ScribbleApp::setPen()` puts the invalid color back when the toolbar hands back exactly the theme
color for a tape pen that never had one, so changing the width does not freeze the color; picking a color
does. `PenToolbar::toolColor()` makes every swatch opaque for the tape, and `createTape()` drops alpha again,
since a translucent strip would hide nothing. No color is hardcoded.

## The element

`Element::createTape()` makes a `<path class="write-tape" __tape="1">`: a closed four-corner polygon of the
strip, with `stroke` always set to the tape color (the outline, at `tapeOutlineWidth()` = 4% of the strip
width clamped to 0.75..3). `setTapeSolid()` writes the state and the look together:

- solid (`__tape="1"`): `fill` = the stroke color, no dash.
- see-through (`__tape="0"`): `fill="none"`, `stroke-dasharray` of 4w/3w.

The look is on the node as well as the attribute, so an older build (or a browser viewing the SVG) shows the
same strip, merely without the "over everything" pass. The color lives on the stroke because a recolor
(`applyProperties`) only sets paint that is present: a see-through strip recolored then toggled back to solid
takes its new color from the stroke. There is no `ShapeParams` or other descriptor - the polygon is the
geometry, so move, scale and rotate of a selection just transform the path.

`Element::tapeHit()` is point-in-polygon or within reach of an edge, in page coordinates (node transform
applied).

## Drawn over everything

A strip has to hide ink on **every** layer, but it must stay in its own layer's run in the tree (layers.md:
z-order is the insertion point) so that lock, hide, move-to-layer, undo and sync treat it like any other
element. So the order is fixed at draw time instead: `Page::draw` installs a `SvgPainter::skipNode` that skips
tape nodes for the tree walk, then draws each visible tape node itself afterwards with the same dirty rect.
A skip already installed (`ScribbleArea::drawImage`'s floating selection skip) is chained: it applies to both
passes. `Page::hasTape()` keeps the cost to one scan of the page's children, and nothing at all is different
for a page without tape.

- A strip on a **hidden layer** is `display="none"` like everything else there, and the second pass checks
  `isVisible()`.
- A **selected** strip is drawn with the selection (floating) like any element, i.e. in z-order, until the
  selection is let go.
- Ink being drawn right now (the stroke builder) is painted incrementally and so shows on top of solid tape
  while the pen is down. **The commit fast path had to change for this:** a finished stroke on a clean page
  used to be added with `clearDirty()`, since what was painted while drawing is already right - but it was
  painted over the tape. With tape on the page the stroke's area is left dirty and repainted, so the strip
  covers it again on release (seen in agent-display before the fix: new ink stayed on top of solid tape).

## Toggling

A press that does not drag, in three ways:

- **The Tape tool itself**: `doReleaseEvent` treats `MODE_TAPE` with `MODEMOD_CLICK`, or a strip shorter than
  max(width/4, 8 px), as a tap - the stub is dropped (`doCancelAction(false)`) and the strip under the release
  point, if any, is flipped. Dragging from on top of a strip lays a new one over it.
- **The Pan tool, or a finger that pans** (the default once a pen has been seen): both end in
  `ScribbleView::panZoomFinish` -> `ScribbleArea::doClickAction`, which tests tape before hyperlinks, with the
  touch reach.
- **A finger that draws** (`singleTouchMode` draw, no pen): a `MODE_STROKE` touch release with
  `MODEMOD_CLICK` on a strip drops the dot and flips the strip instead.

The pen tip and the mouse in a drawing tool do **not** toggle: a dot there is ink.

`ScribbleArea::tapeHit()` takes the topmost strip on an editable, visible layer, so tape on a locked layer
cannot be flipped (layers.md: lock guards a layer from work done on other layers).

### Toggling is an edit (decision)

`ScribbleArea::toggleTape()` swaps in a clone with the state flipped, add and delete in one action - the
pattern `toggleTodoTag()` established for to-do tags. It is therefore **undoable, synced, and marks the
document modified**. The alternative (a view-only state that does not dirty the document) was rejected:

- the state is stored in the page (the task says it must be an attribute), so a toggle that does not mark the
  document modified would be silently lost or silently saved depending on what else changed;
- sync only carries undo items, so on a shared whiteboard a non-undoable toggle would never reach the peer
  and the two screens would disagree about what is covered;
- undo is what a user reaches for after flipping the wrong strip.

The cost is that revising (flip, check, flip back) fills the undo history with toggles and leaves the
document modified. If that proves annoying, the fix is a coalescing rule in the history (a toggle undone by
the next toggle of the same strip cancels out), not a non-undoable toggle.

## Erasing and selecting

- The **stroke eraser** and **path select** hit a strip anywhere inside it, solid or see-through:
  `isNearSegment()` treats a `write-tape` path as filled even when its fill is `none`. Rect and lasso select
  work by bounds as for any element.
- The **free eraser** and the **ruled free eraser** take a strip whole (into `tempSelection`, deleted on
  release like a stroke-erased element) rather than cutting it: `toPenPoints()` would read the rectangle as
  pen geometry, and a hole in a strip of tape means nothing.

## Toolbar and icon

`MainWindow::actionTape` ("Tape"), on the tools row after Ephemeral, same priority as the other draw tools;
`selectDrawTool(DRAWTOOL_TAPE)` and `modeToAction()` route it like the other three. The icon
`scribbleres/icons/ic_menu_tape.svg` is hand-drawn (reicon has nothing like it), `class="icon"` with
`currentColor` throughout, embedded by `python3 embed.py icons/*.svg > res_icons.cpp`.

## Known gaps

- **PDF export** (`ScribbleApp::writePDF`) walks the SVG with `PdfWriter`, not `Page::draw`, so there a strip
  is drawn in its z-order (on its layer) and ink on a higher layer shows over it. Thumbnails, Send Page Image
  and PDF-import page rendering do go through `Page::draw` and get the second pass. Whether solid tape should
  be exported at all (or exported see-through) is an open question.
- The hover cursor for the tape tool is the plain pen cursor.
- No in-app `ScribbleTest` check yet (and `--test` currently aborts in `test7` before most checks, a known
  pre-existing failure, see tests.md). Verified by hand in agent-display: laid over ink; Tape-tool tap to
  see-through and back; Pan-tool tap; Ctrl+Z of a toggle; new ink drawn across a solid strip goes under it;
  save, restart and reload keep `__tape="0"`; the toggle survives in the persisted undo history; centre on
  line snaps a slanted drag to the line; a tap off any strip lays nothing; the ruled eraser takes the strip.
  Not driven: a finger (agent-display has no touch), layers other than the default, the free eraser, sync.
