# Editing Toolbar Redesign — Implementation Plan

## Context

A new Penpot mockup (page "v5") redesigns the "Editing" toolbar (the group
containing Draw/Highlight/Ephemeral/Erase/Select/Insert-Space). Today this
toolbar is a `Menubar` of top-level `Action`s (`actionDraw`, `actionErase`,
`actionSelect`, `actionInsert_Space`), each with a *floating* `Menu` of
sub-mode actions that appears/disappears on tap, plus a separate `PenToolbar`
panel for color/width. The mockup replaces this with:

- Six persistent top-row tool icons: Draw, Highlight, Ephemeral, Erase,
  Select, Insert Space — laid out inline, not as a menu-triggering group.
- Tapping one shows an **inline options row** directly below the top row
  (not a floating popup) specific to that tool; tapping the same icon again
  hides it. Only one options row is visible at a time.
- Draw / Highlight / Ephemeral share one options-row layout: saved colors +
  "+" (add new saved color) + thickness presets. Values differ per mode
  because each mode now keeps **fully independent pen state** (color,
  thickness, tip shape, and advanced calligraphy settings), matching how
  `ScribbleMode::eraserMode`/`selectMode` already remember a "last used"
  sub-choice per family. Pen presets (the old "Save Pen" menu) are removed —
  you edit the current pen directly instead.
- Erase options row: two **independent** toggle buttons — stroke-vs-free
  erase, and unruled-vs-ruled — covering all 4 combinations
  (`MODE_ERASESTROKE` / `MODE_ERASERULED` / `MODE_ERASEFREE` /
  `MODE_ERASEFREERULED`), a "switch back to previous pen after erasing"
  toggle, a settings-icon placeholder ([reicon `settings2`, outline
  weight](https://reicon.dev/icon/settings2?weight=outline); future popup for
  e.g. "erase images" / ruled-tool settings — icon only, no popup logic yet),
  plus thickness.
- Select / Insert-Space options rows: unchanged single-select behavior
  (lasso/rect/line/path; plain/ruled insert-space), now shown inline instead
  of in a floating `Menu`.
- Every options row gets a **"?" help button**. Press-and-hold shows a
  popup listing every icon in that row with its name and a one-sentence
  description (content from the Penpot mockup's "help-example-insert-space"
  example). This popup shape (rounded corners + pointer arrow to the
  triggering button) becomes the **standard popup chrome**, replacing the
  plain-rectangle `Menu`/`#tooltip` look everywhere else (overflow menu,
  future thickness-preset editor, etc.).

The backend already has all 4 eraser mode enum values
(`scribblemode.h:7-11`, `MODE_ERASEFREERULED` added in 476b3ff) and the new
icon set (`ic_menu_draw.svg`, `ic_menu_erase.svg`, `ic_menu_select.svg`,
`ic_menu_insert_space.svg`) is already redrawn in the new 24×24 outline
style. This plan wires the UI up to that backend and introduces the shared
popup widget the new interactions need.

## Sizing, theming, and icon-sourcing conventions

These apply across every new/changed piece below, not just one section:

- **Icon source**: new icons (settings/gear placeholder, any missing eraser-combo
  icon, etc.) come from [reicon](https://reicon.dev), `weight=outline` — e.g. the
  settings icon is https://reicon.dev/icon/settings2?weight=outline. Match this
  to the existing 24×24 outline style already used by the just-redrawn icons.
  Where reicon has no matching icon, compose/redesign one from other reicon
  outline icons rather than inventing a new visual language. This does **not**
  require re-doing the rest of the existing icon set right now — only new icons
  introduced by this redesign need to follow the convention.
- **No fixed pixel sizes copied from Penpot**: sizes measured in the mockup
  don't reliably carry over 1:1 into the app (units/DPI handling differs), and
  toolbar/icon height is expected to keep changing. All new layout code
  (`ArrowPopup` dimensions, options-row heights, icon sizing, arrow/corner-radius
  geometry) must be expressed **relative** to existing size variables (e.g.
  whatever drives current toolbar/icon sizing in `mainwindow.cpp`/theme, such as
  toolbar height or a base icon-size unit) rather than as new hardcoded
  constants, so a future global toolbar/icon resize doesn't require touching
  this code again.
- **Panel outline**: the border/outline thickness around toolbar panels is
  explicitly undecided ("subject to change") — don't bake a specific stroke
  width into new widgets; drive it from a single theme value so it can be
  tuned later without hunting through multiple files.
- **Canvas background as a theme variable**: today
  `ScribbleArea::BACKGROUND_COLOR` (`scribblearea.h:249`, defined
  `scribblearea.cpp:20` as `0xFF444444`) is a single hardcoded `Color` used to
  fill the area around the page (`ScribbleArea::drawImage`,
  `scribblearea.cpp:2649-2651`) — it has no light-mode counterpart today, unlike
  the rest of the UI which already has a dark/light CSS variable pair
  (`svg.window` vs `svg.window.light` in `ugui/theme.cpp:1-38`, toggled via
  `win->node->addClass("light")` in `ScribbleApp::loadConfig()`,
  `scribbleapp.cpp:459-467`, driven by `cfg->Int("uiTheme") == 2`). Add a
  `--canvas` CSS var to both blocks in `theme.cpp` (dark ≈ current
  `0xFF444444`; light: a suitable light gray, TBD value) for any new
  widget-side chrome that should match the canvas backdrop, **and** update
  `ScribbleArea::BACKGROUND_COLOR` so it's no longer a fixed `static const` —
  resolve it from the same light/dark state at the point `loadConfig()` already
  flips the `"light"` class, so both the CSS-styled UI and the raw
  `Painter::fillRect` canvas fill stay in sync from one source of truth instead
  of drifting.

## Icon extraction from the Penpot mockup

Scope: the *existing* app icon set stays as-is (out of scope). Only the
specific icons drawn in the v5 mockup for this toolbar (draw, highlight,
ephemeral, erase pen/stroke icons, select variants, insert-space variants,
help "?", the eraser toggle icons, thickness swatches, etc.) get pulled from
Penpot and used as the source for this feature's icons.

**Verified the extraction path works, with one tool caveat:**
- The dedicated `export_shape` MCP tool's **SVG format is currently broken**
  — it returned empty/whitespace output for every shape tested (a simple
  icon group, a plain ellipse, a whole board), not just complex ones. `PNG`
  format works fine and is good for visual review, but isn't usable as a
  source file.
- **Workaround (confirmed working)**: call `penpot.generateMarkup([shape],
  { type: "svg", withChildren: true })` directly via the `execute_code` tool.
  Tested successfully on: a simple multi-path icon group (pen/draw icon,
  11.9KB raw markup), a boolean-op compound shape (select-rect icon, Penpot
  resolves the boolean into plain path data server-side), and a
  medium-complexity icon with nested groups/ellipses (eraser icon) — all
  produced complete, valid SVG.
- **No coordinate translation needed**: extracted shapes keep Penpot's
  absolute canvas coordinates (e.g. `viewBox="1027.42 649.27 32 32"`); since
  `viewBox` just defines an offset+size window, this renders correctly as-is
  — no need to re-baseline paths to a 0,0 origin.
- **Cleanup still required before an icon is usable as an app resource**:
  drop the redundant literal `width`/`height` attrs in favor of the
  `viewBox`. The hidden `display:none` "base-background" `<rect>` every
  mockup icon includes is **not just an editor artifact — it's the
  icon's canonical sizing reference and must drive the extracted
  `viewBox`, not the group's own aggregate bounds.** Verified this matters:
  across 5 sampled icons, the group's overall bounds matched the
  `base-background` rect's bounds in 4 cases, but the eraser icon's group
  bounds came out `33×32` against a `32×32` background rect — one visible
  path overflows the nominal frame by a pixel. Deriving viewBox from group
  bounds (rather than the background rect specifically) would render that
  icon at a very slightly different effective scale than its siblings.
  Also confirmed the reference frame isn't uniform across icons — most
  toolbar icons use `32×32`, but e.g. the "add saved color" plus-circle
  icon is `25×25` and the help "?" icon is `24×24`, matching their smaller
  inline usage in the mockup — so each icon's own background-rect size is
  the one to preserve, not a single global constant. Practical rule: for
  each icon shape, read the `base-background` child's `bounds` and use
  that (not `shape.bounds`) as the extracted `viewBox`; the rect element
  itself is `fill:none` already so it's harmless whether left in the
  markup or stripped as an element — only its *bounds* need to survive.
  Separately, **extracted paths carry
  hardcoded fill colors from however the shape looked in the mockup** (e.g.
  the currently-active Draw icon exports with a baked-in accent blue
  `#2ea3cf`) rather than using the app's existing `.icon { fill: var(--icon)
  }` theming convention (`ugui/theme.cpp`) — each extracted icon needs its
  fill attributes normalized/stripped so it themes correctly (dark/light,
  hover/checked states) instead of staying a fixed color, except for any
  icon that's intentionally meant to always show an accent color.
- Exported icon viewBoxes vary (`32×32` for most toolbar icons, smaller for
  inline ones per above), while the existing redrawn icon set uses `24×24`
  — reinforces the "no fixed pixel sizes" guidance above; treat each
  extracted viewBox as that icon's own native size and scale it relatively
  wherever it's placed, not toward a fixed 24px or 32px target.

Practical extraction step for implementation: write a small one-off script
(run via `execute_code`, or ported to a local script) that, for each named
shape id in the mockup, does `generateMarkup` → strip the hidden background
group → strip hardcoded fills → write to `scribbleres/icons/<name>.svg`,
then run through the existing `embed.py` → `res_icons.cpp` pipeline
(`scribbleres/res_icons.cpp`, `syncscribble/resources.cpp:43-44`) exactly
like the current icon set.

## Key architectural changes

### 1. New shared "arrow popup" widget

No existing widget matches the rounded-rect-with-pointer look (`Menu` is a
plain rect; `#tooltip` is a plain rect; the disabled `oneTimeTip` is a plain
rect). Build one new class, e.g. `ArrowPopup : public AbsPosWidget` in
`ugui/widgets.h`/`.cpp` (sits next to `Menu`), reusing:
- `Menu::calcOffset()` (`ugui/widgets.cpp:339-371`) for anchor-relative
  positioning / flipping when near screen edges — extend it (or copy+adapt)
  to also compute the arrow's tip position along the popup's edge.
- `Tooltips`' show/hide/timer plumbing (`ugui/widgets.cpp:533-588`) for the
  "shown while pressed, hidden on release" interaction the help button uses.

**Shape generation**: build the background as a single SVG path per popup
instance (like the mockup's own bespoke path), generated at layout time from
`(width, height, cornerRadius=12, arrowSide, arrowOffset, arrowSize)`:
rounded rect on 3 sides, with a triangular notch replacing a flat run of the
4th side at `arrowOffset`. Clamp `arrowOffset` so the arrow's base never
overlaps a corner arc — i.e. `arrowOffset ∈ [cornerRadius + arrowHalfWidth,
edgeLength - cornerRadius - arrowHalfWidth]`; if the popup is too small for
this to hold (`edgeLength < 2*cornerRadius + arrowWidth`), drop the arrow
notch and fall back to a plain rounded rect — this directly answers the
question in the request ("rounded corners have priority over the arrow").
Regenerate the path whenever the popup's content-driven height changes
(content is a flex-column `Widget`, same as the mockup's `Board`).

Use `ArrowPopup` for: the new help popups, and retrofit it in place of the
overflow menu's `Menu` and (once help content isn't needed there) other
popups the request calls out ("all other popups from before ... should have
the UI of the help menu as well").

### 2. Inline options rows instead of floating `Menu`

Currently `Menubar::addButton` (`touchwidgets.cpp:221-266`) opens sub-mode
choices in a floating `Menu` anchored to the button. The mockup instead
keeps a fixed-position options row *inside* the same toolbar panel,
shown/hidden by visibility toggling (not a floating overlay). Restructure
`PenToolbar`/the tools toolbar assembly in `mainwindow.cpp` (~755-826) so
`Editing` becomes a flex-column container with:
```
tools row (6 icons, plain Buttons w/ checked() state, single-select)
draw-options row      (visible iff active mode ∈ {draw, highlight, ephemeral})
erase-options row     (visible iff active mode is an erase mode)
select-options row    (visible iff active mode is a select mode)
make-space-options row(visible iff active mode is an insert-space mode)
```
Tapping a top-row icon: (a) runs `MainWindow::updateMode()`'s existing
single-select bookkeeping (`checkedMode`, `Action::setChecked`) to switch
`ScribbleMode`, and (b) shows the matching options row while hiding the
others — if the tapped icon was already active, hide the row instead
(second-click-to-close), matching the existing `Menubar` toggle pattern
(`touchwidgets.cpp:242-249`) but against row visibility instead of a `Menu`
stack. Reuse the auto-adjust machinery (`AutoAdjContainer`,
`createPenToolbarAutoAdj`, `pentoolbar.cpp:103-145`) to keep the
saved-color/width palettes shrinking under narrow widths as they do today.

This absorbs `PenToolbar`'s current color/width UI (colorPalette,
widthPalette, colorPicker, widthPreview) into the new draw-options /
erase-options rows; `PenToolbar` as a separate floating panel goes away in
favor of these always-present inline rows. `PaletteWidget`
(`pentoolbar.h:14-29`) is reused as-is for the saved-colors row (its
overflow-chevron mechanism already matches "3 shown + more via chevron" if
a mode's saved-color count grows past what's visible — the "+" button just
becomes `PaletteWidget::addButton`'s insertion point instead of a picker
that replaces a swatch).

### 3. Per-mode independent pen state

Replace the single `ScribbleApp::currPen` (`scribbleapp.h:132`) usage for
these three modes with three persisted `ScribblePen`s — e.g.
`ScribblePen drawPen, highlightPen, ephemeralPen;` (naming TBD; could live on
`ScribbleMode` alongside `eraserMode` et al., or on `ScribbleApp` next to
`currPen`). `DRAW_UNDER`/`EPHEMERAL` (`scribblepen.h:19`) stay as flags baked
into `highlightPen`/`ephemeralPen` respectively (so the rest of the drawing
code, e.g. `scribblearea.cpp:2021,2037` checking `currPen()->hasFlag(...)`,
needs no change) — switching top-row mode copies the relevant stored pen
into `currPen`. Remove `cbHighlight`/`cbEphemeral` checkbox menu items
(`pentoolbar.cpp:321-332`) — they're superseded by the mode buttons.
Persist the 3 pens the same way `eraserMode` etc. are persisted
(`loadModes`/`saveModes`, `scribblemode.cpp:72-92`).

Advanced pen settings (tip shape Flat/Round/Chisel, and for variable-width
pens: ratio/angle/dash/gap — currently `comboPenTip`/`rowRatio`/`rowPrPrm`/
`rowMaxSp`/`rowAngle`/`rowDash`/`rowGap`, `pentoolbar.cpp:151-256`) move
behind: (a) a settings-icon placeholder on the draw-options row (icon only,
no popup wired up yet, same as erase's settings placeholder), and (b) an
`ArrowPopup` opened by tapping an **already-selected** thickness preset,
showing a detail editor for that preset (exact width value, and eventually
tip shape / add-more-presets — content beyond exact value TBD, flagged as
future work per the user's note "maybe we will add a way to add more
presets there as well").

### 4. Eraser toggle wiring

Bind the two independent toggle buttons directly to `ScribbleMode::eraserMode`:
- `toggle-erase-stroke` off + `toggle-line-erasor` off → `MODE_ERASEFREE`
- `toggle-erase-stroke` off + `toggle-line-erasor` on  → `MODE_ERASEFREERULED`
- `toggle-erase-stroke` on  + `toggle-line-erasor` off → `MODE_ERASESTROKE`
- `toggle-erase-stroke` on  + `toggle-line-erasor` on  → `MODE_ERASERULED`

Both are plain `Button`s using `checked()`/`setChecked()` for on/off visual
state (no new toggle-button class needed). New requirements to close the
gap called out in the Explore report:
- Add `actionRuled_Free_Eraser` (or reuse a single generic `actionErase`
  Action driven by the two toggles instead of 4 separate Actions — cleaner
  given there's no per-combo icon) and a `MODE_ERASEFREERULED` case in
  `MainWindow::modeToAction()` (`mainwindow.cpp:258-279`).
- `toggle-switch-back`: new persisted bool (e.g.
  `ScribbleMode::eraseSwitchBack` or a `ScribblePen`/config flag) gating the
  existing "switch back to previous pen after erasing" behavior — **first
  confirm whether that behavior is actually implemented today** (user
  wasn't sure); if not, this toggle should still be added to the UI per
  the user's explicit instruction, wired to a no-op/false default until the
  underlying behavior exists.
- The top-row `btn-erase` icon stays generic regardless of which combo is
  active (per clarification) — no icon-swapping logic needed there.
- New icon needed for whichever combo(s) lack one; follow the existing
  24×24 outline convention used by the just-redrawn icons.

### 5. Icon/resource housekeeping

`scribbleres/icons/backup/` (old icon set) and `scribbleres/icons/select_icons/`
(byte-identical staging copy of the new select icons already in
`scribbleres/icons/`) are untracked and unreferenced by any code — confirm
with the user whether to delete them once the new icons are verified, since
they look like leftover working copies rather than intentional additions.

## Files to touch (representative, not exhaustive)

- `ugui/widgets.h` / `ugui/widgets.cpp` — new `ArrowPopup` class.
- `ugui/theme.cpp` — any new CSS classes for arrow-popup styling (dark
  `#101010`-ish background per mockup, title/description text styles); new
  `--canvas` var (+ light variant) and a panel-outline-thickness var per the
  sizing/theming conventions above.
- `syncscribble/scribblearea.h` / `.cpp` — `BACKGROUND_COLOR` becomes
  light/dark-aware instead of a single `static const Color` (`scribblearea.h:249`,
  `scribblearea.cpp:20,2651`).
- `syncscribble/scribbleapp.cpp` — extend the existing light/dark switch in
  `loadConfig()` (~459-467) to also update the canvas color source.
- `syncscribble/pentoolbar.h` / `.cpp` — restructure into the inline
  draw/highlight/ephemeral options-row builder(s); drop
  `cbHighlight`/`cbEphemeral`/pen-preset ("Save Pen") menu code; add
  per-mode `ScribblePen` storage and swap-on-mode-change logic.
  `PaletteWidget` (pentoolbar.h:14-29) reused for colors row.
- `syncscribble/scribblemode.h` / `.cpp` — add persisted "switch back after
  erase" flag; confirm/extend `setRuled()` (`scribblemode.cpp:63`) or retire
  it in favor of the 2-axis toggle logic; ensure `MODE_ERASEFREERULED` is
  fully covered in `getModeType`/`setMode` (already mostly done per commit
  476b3ff — verify).
- `syncscribble/mainwindow.h` / `.cpp` — replace `Menubar`-based
  Draw/Erase/Select/Insert-Space submenu popups (~755-826, `updateMode()`
  283-330, `modeToAction()` 258-279) with inline-row show/hide logic; add
  eraser-combo Action(s) and `modeToAction` case; wire new help-button
  `ArrowPopup`s per row using content derived from each row's icon
  name+description strings.
- `scribbleres/icons/` + `scribbleres/res_icons.cpp` — any new icons
  (ruled-free eraser combo if a distinct icon is wanted, settings/gear
  placeholder, help "?" already exists as `ic_menu_help.svg`) added via
  `embed.py` regeneration.
- `syncscribble/scribbleconfig.cpp` — persistence for new per-mode pens /
  toggle states, alongside existing `savedColors`/`savedWidths`/mode config
  strings.

## Open items to confirm before/while implementing

1. Whether "switch back to previous pen after erasing" is an existing,
   working feature (grep for it wasn't conclusive) — determines whether
   `toggle-switch-back` wires to real behavior or is UI-only for now.
2. Exact content/scope of the future thickness-preset editor popup and the
   eraser settings-gear popup — both are explicitly deferred by the user;
   this plan only adds the icons/entry points, not the popups themselves.
3. Cleanup of `scribbleres/icons/backup/` and `scribbleres/icons/select_icons/`.

## Verification

- Build: `cd syncscribble && make USE_SYSTEM_SDL=1 DEBUG=1`, run
  `./Debug/Write` interactively (GL rendering required, so this needs a
  real display — no headless test for UI).
- Manually exercise: switching Draw/Highlight/Ephemeral and confirming each
  keeps independent color/thickness across switches and app restarts
  (config persistence); all 4 eraser toggle combinations actually erase as
  `MODE_ERASESTROKE/RULED/FREE/FREERULED` did before (whole-stroke vs
  partial, ruled-constrained vs free); second-tap-to-close on every top-row
  button; help "?" popup shows correct name+description per row and its
  rounded-corner/arrow shape holds up as the popup resizes; saved-color "+"
  appends instead of replacing, and long-press/right-click still deletes a
  saved color (`SvgGui::setupRightClick`, already supports touch long-press
  and mouse right-click — reuse, don't reimplement).
- Run existing `ScribbleTest` suite (`./Debug/Write --test`) to catch any
  regression in stroke/erase logic untouched by this plan but exercised
  indirectly through `ScribbleMode`/`ScribbleArea` changes.
