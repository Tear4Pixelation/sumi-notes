# Paper: page size, dotted paper, page layouts

# Default page size

New pages take `pageWidth`/`pageHeight` from the config. `askDefaultPageSize()` asks once at startup
(`pageSizeAsked`, so existing installs are asked too) whether that is **A4 or Letter**, with the one the
locale suggests first (`localeUsesLetter()`: `LC_ALL`/`LC_PAPER`/`LANG`, then `SDL_GetPreferredLocales`
where the SDL branch has it - the Windows and macOS branches do not, so those fall back to A4 first).
It replaced a screen-derived default, the display turned portrait, which on a 16:9 monitor is a 0.56
strip. Since pages grow as they are written on, the width, i.e. the shape, is all this really decides,
and a tablet gains nothing from the screen's shape either - so the prompt offers paper only.
Preferences > General > Default page size (`type="pagesize"`, handled specially in `ConfigDialog`, as it
writes two values) offers A4, Letter, their landscape forms and the screen size; a size set any other
way shows as Custom and is left alone on OK. Paper sizes are `RulingDialog::predefSizes`, shared with
Page Setup. The startup Untitled document predates the answer, so it is recreated if still untouched.

# Dotted paper

`PageProperties::dotRadius` > 0 turns a standard ruling into dots of that radius (page units):
dots at the intersections when both `xRuling` and `yRuling` are set, dotted lines along the one
ruling that is otherwise (pitch `max(4r, 4)`). `Page::generateRuleLayer()` builds all dots as **one**
filled `SvgPath` (a node per dot is thousands of nodes on a fine grid), with `shape-rendering="auto"`
overriding the rule group's `crispEdges`. Stored as `dotradius` on `contentNode` - written only when
set, so lined pages are byte-identical and older builds simply show lines - plus the `dotRadius`
config default, a `dotradius` attribute on `<pagechanged>` (sync), and an optional 8th field in
`recentPageRulings`. Page Setup has a Dot Radius spin box and four dotted presets, appended *after*
index 7 of `predefRulings` because the document list indexes that table by its own 1-7; radii live in
the parallel `predefDotRadii` since the table is `unsigned int`.

# Music staves

`PageProperties::staff` (and `RulingFrame::staff`, `RulingRegionParams::staff`, `PageLayout::staff`) turns
the y ruling into music paper. **`yRuling` is then the height of one staff band, not a line pitch**: a band
is `STAFF_BAND_SPACES` (9) staff spaces tall, the staff of `STAFF_LINES` (5) lines sits centred in it
(`staffLineOffset()`, `rulingregion.h`, shared by page, region and previews), so the staff space is
`yRuling/9` and a band is one "line of writing" for every ruled tool (insert lines, ruled select, reflow). The
presets use 144 (space 16 = 2.7 mm at 150 units/inch, 12 staves on A4, 11 on Letter). A staff has no x
ruling, no dots (`sanitize()` and the page loader force both to 0) and no margin line in the presets.
- Storage: `staff="1"` on the page's `contentNode` and `__rrstaff` on a region, **written only when set**, so
  other pages are byte-identical and an older build just shows plain lines at the band height. Also on
  `<pagechanged>`/`<regionchanged>` (sync) and in the undo persistence items, the `staffRuling` config
  default, and an optional 7th field of a saved layout string (`customPageLayouts`, `recentPageLayouts`).
- `Page::generateRuleLayer()` draws all staves as **one** path (class `staffrule`) in the ordinary rule group,
  so night mode (render-time `ColorMap`), PDF/SVG export and page thumbnails need nothing of their own.
  Regions use `RulingRegionParams::linesPath()` (clipped staff lines, phased from the region's top-left like
  its other lines; unlike plain lines a line on the outline's extreme edge is dropped, not kept).
- UI: Page Setup's ruling combo has "Music staves" (`RulingDialog::STAFF_PRESET`; the flag lives in
  `staffPreset` since the presets are numbers and survives editing Y Ruling), the Add Page grid has a "Music"
  tile under Special, and the Paper Patch panel's kind dropdown has Music (switching converts the number
  between a line pitch and a band by `9/2.5`; its slider has its own 63-270 range).
- Previews (`rulingSVG`): a thumbnail whose staff space is under `MIN_LINE_SEP` draws only each staff's
  middle line, since five lines a pixel apart are a smear; the 1:1 window and lens draw all five.
- Known gaps: the centre-on-line marker and snap to grid still snap to band boundaries (the gap between
  staves), not to a staff line - they follow `yRuling` like everything ruled. Page Setup has no spinner for the
  staff space itself, only the band height. The strings are not in `strings.xml` (neither are the other
  layout names), so they are English until translated.

# Page layouts (Add Page popup)

`addpagemenu.cpp`. The popup opens on a short row - document default, the last two layouts used
(`recentPageLayouts`), and a "+". The "+" switches the same popup to the full grid: 13 built-in
layouts (three each of Lined, Squared, Dotted, then Special's four - Plain, Dotted lines, Wide margin, Music), then the user's custom layouts
(`customPageLayouts`) and a "New layout" tile. Both views are built on open and only toggled visible
afterwards - rebuilding from the "+" handler would free the widget being dispatched to.
Right-click / long press edits a tile in `RulingDialog` layout mode; a built-in is fixed, so its edit
is saved as a new custom layout.

- **A layout is geometry only; colors come from the document** (`layoutToProps()` reads
  `pageColor`/`ruleColor` from `doc->cfg`, which the theme writes). The old presets hardcoded blue,
  which put light-theme blue rules on dark paper. Page Setup's presets now take the rule color the
  same way, and layout mode hides the color pickers.
- **Each tile is split**: the whole page on the left (only its own width), the rest a window onto the
  ruling at 1:1 - page units times `ScribbleView::getScale()`, which maps straight to UI units.
  `rulingSVG()` is one renderer for thumbnail, 1:1 window and lens, parameterised by a `PageMap` and a
  rect-or-circle `Clip`; only thumbnails thin lines.
- **1:1 views are phased to the cells, not the page origin** (`cellPhase()`, and the lens centres on
  a cell middle). From the page origin a coarse grid could show a single dot or one line - nothing
  that conveys the size of a square. Lined pages without vertical rules start at the margin instead.
- `RulingDialog` puts the preview **beside** the controls (it sat above them and was squeezed), with a
  magnifier lens at 1:1, except in the scrolling phone layout, where it stays on top.
