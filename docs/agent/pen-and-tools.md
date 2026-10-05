# Marker: centre on line

`ScribblePen::CENTER_ON_LINE` (a pen flag, so saved with the pen) turns a stroke into a straight
horizontal line along the vertical centre of the ruled line the press lands in
(`getYforLine(getLine(y)) + yruling(true)/2`, fixed for the gesture in `ScribbleArea::centerLineY`); it
reuses the `LINE_DRAWING` path, so x follows the pen and no filters are installed. The toggle is a
toolbutton on the pen options row itself, after the widths (`PenToolbar::centerLineToggle`, the ruled
eraser's `ic_menu_toggle_ruled` icon), shown only while the marker is the active tool - visibility is set
in `setPen()` *before* its early return, since switching tools with an identical pen still changes it.
Not covered by automated tests.

# Relative pen width

Any pen's thickness can be expressed as a **multiple of the page's line height** rather than in
document units - `ScribblePen::WIDTH_RELATIVE`, with `width` holding the fraction. The **Relative size**
toggle in the Width popup (`PenToolbar`) switches a pen between the two, and is **on by default for the
text marker only** (`DRAWTOOL_HIGHLIGHT`): a marker's job is to cover a line of text, so "three quarters
of a line" is the number that means something, where a pen is drawing a mark of a particular size. The
mechanism is the same for all three draw tools, and only the default differs.

- **The marker is relative, always.** The flag is the constructor's default *and* `loadModes()`
  converts a marker pen read without it (width divided by `Page::BLANK_Y_RULING`, since the page being
  drawn on is not known yet) and clears `savedMarkerWidths` so the presets reseed in the new unit -
  that runs before the toolbar is built, which is what makes clearing the config value enough. The
  numbers are converted, never reinterpreted: 30 units would otherwise load as 30 line heights.
- **`ScribbleArea::resolvedPen()` is the only place the multiplication happens**, and every consumer of
  the pen's width goes through it - the stroke builder, `createShapeElement()` and the hover cursor.
  What lands in the document is therefore always an absolute `stroke-width`, so the file format, the
  undo history and sync know nothing about relative widths.
- **The toggle converts, it does not reinterpret.** Flipping it multiplies or divides the pen's width
  *and its presets* by the current line height, so nothing changes thickness on screen: 0.75 of a 40 unit
  ruling becomes 30, and back. Without that, turning it off would leave a 0.75 unit hairline.
- **One preset list per draw tool** (`savedWidths`, `savedMarkerWidths`, `savedEphemeralWidths`), each
  held in that tool's current unit. They cannot share a list: two tools can be in different units at the
  same time, and the shared list would then be read in the wrong one by whichever tool is not holding
  it - three identical-looking hairlines, or three identical-looking slabs. For the same reason a list
  cannot have a fixed default in the config (a config written before this has *absolute* widths, and
  line-height fractions would load there as hairlines), so the marker's and the ephemeral pen's lists
  start empty and `seedWidths()` fills them on first use, once the unit is known. The marker seeds from
  `MARKER_WIDTHS` (line heights), everything else from `DEFAULT_WIDTHS` (document units).
- `PenToolbar::widthPreview()` **scales the whole preset set down** once any of them exceeds the swatch
  cap (`penWidthPreviewMax`), instead of clamping each. Every marker preset is past the cap, so clamping
  drew three identical bars. A pen's presets are all below it, so pen swatches are unchanged.
- The width display in the popup is a plain SVG fragment - four rule lines with the outer two faded,
  and the stroke lying **in the gap between the middle two**. The gap, not a line, because writing sits
  between the rulings: a stroke centred on a rule line would be showing coverage of the wrong band. One
  line height therefore fills the gap exactly. It is **always shown**, in both units (an absolute width
  is simply divided by the line height first), so the number always has a picture beside it.
  Three things about it: it carries **no `layout` attribute**, because a box layout centres children
  that have no `box-anchor` and would stack all five lines on top of each other; it is drawn in the
  **page's own colors** (`props.color`/`props.ruleColor`, set per page in `updateWidthPopup()`), since
  the usual pen is black and would be invisible against the dark popup; and the outer lines' 0.35
  `stroke-opacity` is opacity rather than a dimmer gray so that "half there" reads that way whatever the
  page and rule colors are.

Also fixed here: `.toolbutton.checked > g > .icon` in `theme.cpp` never matched the swatch prototypes
(pen thickness, eraser radius), which put `.icon` directly in the button rather than in the icon/title
row, so a selected thickness stayed gray. The added rules set only `color`, never `fill`: these icons
are strokes with `fill="none"`, CSS outranks presentation attributes in usvg, and filling them would
blot out the color swatch's selection ring.

Known gap: the toggle converts against the *current page's* ruling, so flipping a pen to absolute on a
blank page and then drawing on a ruled one gives a width picked for the wrong line height. Leaving it on
is the answer, which is why it is the marker's default.

# Pressure sensitivity

**Pen Settings → Pressure** (`PenToolbar::spinPressure`, next to Pen Tip) sets, per pen, how much
thinner the lightest touch draws than a full press, from 0% to 100%. It is `ScribblePen::wRatio`
shown as a percentage: `FilledStrokeBuilder::addPoint()` draws `width*(1 - wRatio*(1 - wscale))`, and
`wscale` runs from 0 at no pressure to 1 at full pressure. The default pens are 90%. The marker is 0%
(no `WIDTH_PR`, `wRatio` 0), so its width never follows pressure. That is deliberate, and nothing here
changes it.

- **The rules live in `ScribblePen::pressureSensitivity()`/`setPressureSensitivity()`**, not in the
  toolbar. 0% clears `WIDTH_PR`. A pen without that flag reads as 0% whatever its `wRatio` is.
- **0% leaves `wRatio` alone if a speed or direction variant still uses it**, since the fountain pen
  (`WIDTH_DIR`) and the speed pen share the one ratio. The other side of sharing: raising pressure on
  those pens also changes their direction or speed range.
- **Turning pressure on sets `prParam` to 2 if it was 0.** `prParam` is the curve's exponent, and at 0
  `1 - pow(1 - p, 0)` is 0 for every pressure, so the pen would draw at its thinnest everywhere.
  `prParam` itself is not exposed; 2 is what upstream picked after trying several curves (see the
  comment above `FilledStrokeBuilder`).
- **The box is rounded on display and ignores its own echo.** `setPen()` pushes the rounded percentage
  in, and `onValueChanged` returns early when the value already matches the pen. A legacy 0.85 shows
  as 85% and is only rewritten once someone edits it.
- The value is saved with the pen like every other field (`ScribbleMode` serializes `wRatio`, `flags`
  and `prParam`), so it survives restarts and saved tool modes.

Not covered by automated tests.

# Selection color, width and line style

The selection popup carries a color swatch and a width item, and the shape tool's options row has the
same width item next to its swatch. Both are `PenToolbar::createSingleSwatch()`/`createSingleWidth()`,
which may be called any number of times; they edit `PenToolbar::pen`, so with ink selected
(`SELECTION_MODE`) they edit the selection through `ScribbleApp::penChanged`, and otherwise the pen. The
width popup has the presets, the width, and **Solid / Dashed / Dotted**.

- **The style is derived, never stored.** A path stores `stroke-dasharray`, a pen `dash`/`gap`, both
  absolute; `ScribblePen::dashStyleOf()` reads the style back against the width (a dash shorter than half
  the width is dotted). Patterns are sized in widths (`dashFor()`: dashed 3w/3w, dotted 0.1w/2w with a
  round cap - a zero-length dash has no direction and nanovg merges it away).
- **`StrokeProperties` carries both a `dashStyle` and the raw `dashArray`.** The UI sends a style, which
  `applyProperties()` sizes per element from its own width; `getProperties()` returns the raw pattern too,
  so `StrokeChangedItem` and `<strokechanged dash=...>` restore exactly. Older peers ignore `dash`.
- **`Element::scaleWidth()` scales the dash pattern**, so a width change or a width-scaling transform keeps
  a line's look.
- **A filled pen stroke is converted to take a style** (`Element::convertToStroked()`): dasharray only acts
  on a stroke, and the default pressure pen draws a filled outline. It becomes a stroked centreline of its
  mean width, losing the width variation; `Selection::setStrokeProperties()` replaces the element, so undo
  brings the original back. Setting it back to solid does not re-fill it.
- The popups close only up to the menu holding their button (`closeMenus(btn)`, `setupSinglePopup()`):
  `closeAutoClosePopup()` would take the selection popup down with them. The selection popup's key
  handler no longer closes it while its own width box has focus.
- On the shape row, a change with a shape selected also sets the pen (as the arrowhead toggles do), so the
  next shape matches. Shapes share the draw pen, so a dashed shape pen also dashes non-pressure pen strokes.

Known gaps: undoing a dotted style leaves the round cap it set; the width item's preview only shows a
pattern clearly at thin widths. Tested by `ScribbleTest::dashStyleTest()` (mutation-checked: no
conversion, no dash scaling).

# Switch back (single-use tools)

The eraser, the selection tool and insert space each have a **Switch Back** toggle on their options row
(`ScribbleMode::eraseSwitchBack`/`selectSwitchBack`/`insSpaceSwitchBack`, `setSwitchBack()`); on, the
tool is used once and hands back to the previous one. `hasSwitchBack()` is the one place that says
which tools have a toggle. The draw tools, shapes, pan and page select always stay; anything else
(Add Bookmark) is single use. **There is no double tap to lock, and no `doubleTapSticky` pref** - both
were removed: picking the active tool again is also how its options row is closed, so closing the row
locked the tool by accident, and nothing on screen explained it. Pan joined the always-stay list then,
since without the double tap it would have had no way to stick at all.

- **The toggle also edits the active tool**, not just the next selection of it: turning it off makes the
  tool sticky immediately, turning it on hands the tool back to `prevStickyMode`, which `setMode()`
  records for exactly this reason. Without that, the toggle appears inert until the tool is reselected -
  which is how the eraser's toggle looked for as long as it was unimplemented.
- **The flags are initialized in the `ScribbleMode` constructor as well as in `loadModes()`.**
  `ScribbleTest` builds a `ScribbleMode` directly and never calls `loadModes`, so anything read by
  `setMode()` but only initialized there is uninitialized memory during tests. That cost six failing
  `testN` fixtures with no obvious connection to the change.
- `eraseSwitchBack` had a slot in the `toolModes` config string from when the toggle did nothing, so
  every config written before this has a 0 in it. That slot is still written (positions after it depend
  on it) but is **not** read back; all three flags are read from tokens appended at the end of the string
  instead, so upgrading does not silently make everyone's eraser sticky.

# Select touching

A toggle on the select options row (`ScribbleMode::selectTouching`, appended at the end of `toolModes`,
off by default) makes lasso, rect and ruled select take everything the selection area touches instead of only
what lies entirely inside it. Disabled while Path Select is the submode, which always selects by touch.
Rect uses the formerly unused `RECTSEL_ANY`, ruled the existing `SEL_OVERLAP` (what `greedyRuledErase` uses),
lasso a `touching` flag. Rect and lasso test the flattened outline (`flatOutline()`, since a curve's
control points lie outside it) segment by segment, not just its points: a straight stroke has no points in
its middle, so a small area across it would miss. The lasso's quick "no change" path stays valid, since the
lasso only changes inside the triangle it tests. Tested by `ScribbleTest::selectTouchingTest()`
(mutation-checked: dropping the segment tests fails it). The new string is untranslated.

# Reflow (ruled insert space)

**Ruled insert space is two tools, one per direction** (first-day report: "split it into moving down and
moving to the side"). The options row offers **Insert Lines** (`MODE_INSSPACEDOWN`, the existing ruled icon,
action `actionRuled_Insert_Space`) and **Insert Space in Line** (`MODE_INSSPACERIGHT`,
`ic_menu_insert_space_ruled_right.svg`, `actionRuled_Insert_Space_Right`). The combined tool moved the text
down a line whenever the pen drifted while pushing it right, and pushed it sideways while dragging it down;
nothing it did needs both at once, so it is no longer offered. Both new modes run as the old
`MODE_INSSPACERULED` gesture - `doPressEvent()` records the tool in `ScribbleArea::insSpaceAxis` and swaps
`currMode` before anything else looks at it, so selection, Skip Lines, region slop, the negative-space erase
and page growth are unchanged - and `doMoveEvent()` only cuts the drag to one axis: Right pins `line` to
`initialLine`, Down pins `lx` to the press and `ldx` to 0 (so with reflow on it is `reflowStrokes(0, dline)`,
exactly what a straight-down drag of the old tool did). Down pressed inside a line moves the rest of it to a
new line (a line break); Right pressed in the margin or on an empty line does nothing. `MODE_INSSPACERULED`
still exists for the tests and as the internal mode; a config that saved it as the insert space mode loads
as Down. Tested by `ScribbleTest::insSpaceAxisTest()` (the same diagonal drag with each tool), mutation-
checked: running both as the combined tool fails three of its checks. The new strings are untranslated, and
the Right icon is hand-made in the style of the ruled one (not from the reicon set).

Where wrapped words go and how far apart they sit are measured from the writing, not the page
(`Selection::measureReflowInk()`, once per gesture, from the ink before it moved):

- **Onto an empty line, wrapped words start where the text does** - the leftmost ink on the reflow's line
  and the line above (so an indented first line does not indent the rest of its paragraph), never left of
  the margin or a column stop, ignoring ink in the margin (bookmarks). It used to be margin + minWordGap,
  which on a dot grid (no margin) put words against the page edge, left of the text.
- **Onto a line with text, the gap is the writer's**: the median gap between words in the selection,
  clamped to [1.25 x minWordGap, one line height]. It used to be a fixed 1.25 x 0.3 x pitch - 11 units on
  a 30 unit grid, tighter than most handwriting.

Both apply to every paper; `test7`/`test11`/`test15` refs were regenerated for it (7-8 unit sideways
shifts). Tested by `ScribbleTest::reflowIndentTest()`, mutation-checked (reverting either fails it).
Not fixed: on dot paper, ink straddling a dot row is split between rows by centre of mass, which can
leave strokes out of a ruled selection and make reflow see text on an "empty" next line.

**Skip Lines** (a toggle on the insert space options row, `ScribbleMode::insSpaceSkipLines`, appended at
the end of `toolModes`; icon `ic_menu_toggle_skip_lines.svg`, the ruled icon with writing on the first and
third lines). For text written on every second line: the line pressed on is a text line, and so is every
second line from it. `skippedLineFrame()` (`selection.cpp`) gives the gesture a frame with the pitch doubled
and its origin on the top of the pressed line, so each "line" is a text line plus the blank line below it.
Nothing downstream changed: reflow wraps to the next text line, vertical steps are two lines (a step happens
when the pen reaches the next text line), stops and the indent's "line above" follow, and an underline in a
blank line stays with the text above it.

**The origin must not be above the pressed line.** It used to be half a line above (each "line" = text line
plus half the blank line either side). That is fine for a press on a text line, but the natural place to put
the pen to push text down is the blank line *between* two text lines, and then that half line is the lower
half of the text line above - where handwriting sitting on the rule has its centre (`calcCom`), so the line
above the press moved too (first-day report: "selects stuff in the line above as well"). With the origin on
the pressed line's top, nothing above the line you press on ever moves, wherever you press; a press on the
blank line pairs it with the text line below, which is the text you meant to move. Cost: a tall ascender
whose centre lies in the blank line above a pressed text line stays behind - press on the blank line above
instead. Pinned by the "pressed on a blank line" checks in `skippedLinesTest()` (fails against the old
origin; letters there sit on the rule, the zigzag letters of the other checks are centred high enough to
pass either way).
Only ruled insert space (Insert Lines, Insert Space in Line) uses it (the toggle is disabled for vertical insert space); ruled select, ruled
erase and ruled move still step single lines.

**It is a toggle, not detected, on purpose.** Detection by counting strokes per line parity was built and
worked in tests, and was removed: when a guess misfires, text lands on the wrong line with no visible
reason, and a user would have to understand the heuristic to use the tool. (For the record, the stray ink
that reaches blank lines is underlines, not descenders - `calcCom` leans on a stroke's first point and top,
so descenders stay on their line.) Tested by `ScribbleTest::skippedLinesTest()`, mutation-checked: ignoring
the toggle and forcing it on each fail it.
