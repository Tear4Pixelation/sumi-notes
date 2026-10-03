# Themed colors

Each document carries a **theme**: a short recipe from which a palette of ink, paper and accent colors
is *generated*. Designed in `COLORS_SPEC.md`; that document is the rationale, this is the summary.
**Phases 1-5 are implemented.** `COLORS_HANDOFF.md` is the state-of-play document - what is built,
every trap, and the open decisions; read it before working on this. The remaining work is the theme
creation UI.

**The palette governs what the picker offers; it never governs what is already drawn.** A stroke keeps
its literal sRGB color, exactly as before, and that is what renders. Changing the generator therefore
cannot alter a single existing document. Only the explicit, undoable **Restyle existing strokes**
option rewrites them. If rendering consulted the theme, every generator change would repaint every
note ever written.

**Restyle carries no `__inkbase`** - a deliberate deviation from the spec. At the moment of restyling
both palettes are known (the old recipe is still in `cfg`, the new one is the argument), so a stroke is
identified by its literal color being an exact member of the *old* palette (`Palette::mapFrom()`). That
needs no new file-format attribute, and it makes strokes drawn *before* the feature existed
restyleable, where a tag could only ever cover strokes drawn after it shipped. The neutral maps to the
neutral rather than by ordinal, and variants are preserved, or every emphasis stroke flattens into
ordinary ink. Groups and `<text>` are skipped (they need the clone dance `Selection` does), which is
why `restyleToTheme()` returns a count rather than claiming to have restyled everything.

**A restyle is one undo step.** `setTheme()` brackets its own undo action for the page recolor, so
`restyleToTheme()` passes `ownAction=false` and brackets both halves itself - otherwise one Ctrl+Z
reverts the strokes while the paper stays on the new theme.

- `ulib/oklab.h`/`.cpp` - OKLab/OKLCh, the sRGB gamut boundary, and the **cusp table** (the lightness at
  which each hue is most saturated). One entry per degree, built once in a function-local static, and
  *interpolated* between entries - rounding to the nearest degree would make two hues a fraction of a
  degree apart share a cusp, which is visible because jitter puts families at fractional angles.
- `ulib/palettegen.h`/`.cpp` - `PaletteRecipe` (about eight numbers), `Palette`, and the generator
  registry. No dependency on `Painter`/`SvgNode`, so it unit-tests standalone like the scan and shape
  math.
- **The rule: lightness is never chosen.** For each hue, start at its cusp, step `depth` away from the
  paper, then keep stepping until the ink meets `minContrast` against *this theme's* paper; chroma is
  then `vividness * maxChroma(L, h)` - a fraction of what that hue can reach, never an absolute number
  (0.20 chroma is dull for magenta and out of gamut for cyan). Dark paper mirrors the walk with no
  separate branch. Measured against the alternatives at seed 218: 0 of 12 colors below 3:1, versus 6 of
  12 for naive hue rotation and 7 of 12 for the classic rainbow palette.
- **`cusp-walk-1` is FROZEN.** Its output for a given recipe must never change - not a constant, not a
  loop bound. If output changes it is a new generator with a new id, however harmless the change looks.
  `scribbletest/colortest_golden.inc` pins every color it produces and is what makes this a property
  rather than an intention; the failure message says to add an id rather than regenerate the table.
  Shipped generators are never deleted - each is ~80 lines of dependency-free math.
- **The generator id is a string**, never an index, and is stored per document - the same lesson
  `shapeId` was bitten by twice, except that a misread index here would change every color in the
  document rather than a tool. An unknown id is *not* an error: strokes still render literally, the
  picker falls back to the default generator, and the unknown id is preserved in the recipe so a
  document from a newer version round-trips back to disk unchanged.
- **Family 0 is never jittered**, so the seed hue actually appears in its own palette. Every other
  family is `seed + i*360/n` plus a deterministic per-`(seed, i)` hash - never a RNG, which would
  repaint the theme on every launch and make the golden check impossible.
- **`Palette::matchReference()` maps by ordinal position, not by hue proximity.** Hue proximity is the
  obvious choice and is wrong: both palettes carry up to `±jitter` of scatter, so two offsets can differ
  by 22° against a 30° spacing, and two reference families then collapse onto one target family - a
  twelve-color document quietly becoming an eleven-color one. Measured 5 collisions on absolute hue and
  still 2 on seed-relative hue before switching to ordinal. The ordinal is *derived from the color*, not
  stored, so a future generator can still change the family count or the spacing rule.
- The **dark variant takes its chroma as the same fraction of what is available at its own lightness**,
  not as a fraction of the base's chroma. Scaling the base's chroma down a second time is what collapsed
  it to near-black at low vividness in the prototype.
- Black and white are exempt from the walk (`Palette::neutral`) - plenty of people write black on white
  and would read a themed near-black as a bug.
- `labs/color-lab.html` is a standalone browser prototype of the same algorithm, with every knob live
  plus an A/B against naive hue rotation. Not part of the build; it is how the constants were chosen.

## Storage and the picker (phase 2)

- **The recipe rides the config system that already exists**, as ordinary values (`themeGen`,
  `themeSeedHue`, `themeVividness`, `themeDepth`, `themeContrast`, `themeJitter`, `themePaperL`,
  `themePaperWarm`, `themeFamilies`) in `ScribbleConfig`. `ScribbleDoc::cfg` is a per-document config
  whose `upconfig` is the global one, and it already round-trips through the document's
  `<script type="text/writeconfig">` node - so this needed no new file-format construct, no new parser,
  and no new save path. It also means a document that has never been themed **inherits the global
  recipe**, which is the whole reason an untitled document needs no special case.
- `ScribbleConfig::setThemeRecipe()` **always writes an explicit generator id**, even when it equals
  this build's default. A document recording only "whatever is current" would silently change palette
  the next time the default moves - the one thing §3 exists to prevent.
- The palette is **cached on `ScribbleDoc` and invalidated in `loadConfig()`**, which is the single
  point every `cfg` swap passes through (plus `resetDocPrefs()`). A global prefs change can move the
  recipe too, so invalidating there is correct rather than merely convenient.
- **`ScribbleDoc::setTheme()` deliberately does not call `setPageProperties()`.** That applies one
  `PageProperties` to every page, and `Page::setProperties()` assigns the whole struct - so it would
  push the current page's ruling *and*, for any page whose size differs, its dimensions onto every
  other page. A theme changes two colors; each page therefore gets its own props back with only
  `color` and `ruleColor` replaced. Pages with `isCustomRuling` (PDF import, document scan) are skipped
  entirely, since their paper is an image.
- `ThemeDialog` (`themedialog.cpp`) is reached from **Theme...** in the overflow menu: a 3x4 grid of
  the shipped themes, one dark-paper toggle, and three checkboxes. Every tile is drawn as **strokes on
  that theme's own paper** rather than as swatch chips - a color that reads fine as a 40px block can be
  invisible as a 3px line, which is the whole point of the generator.
- **The selected tile is ringed by a rect the tile carries itself** (`.theme-sel`, shown by
  `.themetile.checked` in `theme.cpp`), not by the toolbutton rules: those tint a `.icon` child
  (`.toolbutton.checked > g > .icon`) and a tile is a picture of a page, so it has none. Without the
  ring a click changed nothing visible and the dialog read as ignoring clicks - which is what it was
  reported as.
- **Restyle existing strokes is on by default.** COLORS_SPEC.md §7 argued for off; being given a theme
  on the paper but not in the ink reads as the theme half-applying, and the whole restyle is one undo
  step, so the cautious answer stays one click away.
- **A recipe round-trips through the config as `float` while `real` is `double`**, so the values read
  back are a few ulps off the table they came from. `paletteThemeIndexOf()` therefore compares fields
  with a 1e-4 tolerance (no two shipped themes differ by less than 0.01 in any field), and
  `ScribbleConfig::themeRecipe()` snaps a recognised recipe back to the canonical one. Without the
  first, a saved theme never shows its ring and the dark-paper toggle has nothing to restore; without
  the second, the palette generated after a reload differs in the last bit from the one the strokes
  were drawn in, and restyle - which matches colors exactly - would stop recognising its own ink.
- **There are no sliders, and that is measured rather than a simplification.** Seed hue moved a
  full-wheel palette by a mean of 10 degrees per color between seeds 180 apart - *less than the +/-11
  jitter* - so it could never differentiate one theme from another. Vividness already defaulted to its
  maximum of 1.0 against a slider running 0.25-1.0, so the control could only make a theme worse. What
  is left is ink character and paper tint, which is what the themes themselves vary.
- **A theme is ink character plus paper tint; it does not own light-vs-dark.** `paperL` does two jobs -
  which side of the generator's mirror, and how light exactly - and only the second belongs to a theme.
  A theme therefore carries a `paperOffset` and the toggle supplies the side, so "vivid ink on white"
  and "vivid ink on black" are one theme in two modes rather than two entries. The toggle rebuilds all
  twelve tiles.
- **`minContrast` is capped at 4.5 in dark mode.** The walk steps *away* from the paper, so a high
  floor means darker, richer ink on white and *lighter, bleached* ink on black. Uncapped, "Deep" and
  "Contrast" came out as the palest themes in dark mode - their identity inverted. Measured: 7.0 on
  near-black paper lands at 0.067 mean chroma against 0.113 in light mode.
- **Themes are held to a chroma budget: vividness >= 0.90 and `minContrast` <= 5.0.** Separation
  between 12 families 30 degrees apart tracks their mean chroma, and both of those fields desaturate -
  measured on light paper, contrast 3.0->7.0 takes separation 0.0415->0.0293, and vividness 1.0->0.70
  takes it 0.0415->0.0294. They compound, so a theme at 0.90 *and* 7:1 reached 0.0267. `depth` is free
  on light paper and is not budgeted. All twelve themes clear 0.0302, 73% of the shipping palette's
  0.0414. The cost: no genuinely muted theme and no 7:1 contrast theme - the two 12 families cannot
  afford. `colortest.cpp` checks the budget per theme rather than trusting the table.
- **The picker grid offers bases only, not the dark variants.** Offering them was tried: better on
  light paper (17 cells at dE 0.067 against 13 at 0.041) and much worse on dark, where the walk has
  already pushed ink toward light and frozen `cusp-walk-1`'s `dL = min(0.92, L + 0.13)` clamps a dark
  onto its own base - the grid falls to 0.0137, i.e. visually duplicate swatches. The variants remain
  in the data model, since restyle preserves them or every emphasis stroke flattens into ordinary ink.
- **Hue is deliberately not a theme axis.** Restricting the hue span *would* make seeds matter (at a
  120 degree arc, seeds move colors 22-28 degrees instead of 7-10), and it was prototyped - but a
  120 degree theme contains no blue at all, and a note-taking palette that cannot do red *and* blue
  *and* green on one page is not a palette. Every theme covers the full wheel; they differ by how the
  ink sits on the page. The arc measurements are in §8 of `COLORS_HANDOFF.md` so this is not
  re-derived.
- The theme table lives in `ulib/palettegen.cpp`, not in the dialog, so "every shipped theme is legible
  in both modes" is a property `colortest.cpp` pins rather than an intention.
- The icon is `ic_menu_add_color.svg`, the closest thing already in `scribbleres/icons` - a dedicated
  one would come from the reicon pipeline, not be hand-drawn.

## The themed picker (phases 3-4)

- **With the marker in hand every swatch is offered as its family's highlighter variant**
  (`PenToolbar::toolColor()`): the saved list holds ink colors, shared by all three draw tools, so
  storing it per tool would split the user's picks into three lists that drift apart. It is applied at
  the four points a swatch reaches the pen - the row, the grid, `selectColor()` and `setSwatchColor()` -
  and to the selected-swatch comparison, or the ring lands on nothing. The neutral and an off-palette
  color have no highlighter variant and simply take the marker's alpha. Without this, a themed document
  reseeded the row with opaque ink colors and the marker drew solid.
- **The theme is the menu to pick from; `savedColors` is what the user picked.** The options row holds
  a *small working set* - the neutral plus five families spread around the wheel
  (`defaultThemeSwatches()`). The whole palette on the row was tried and is wrong: thirteen swatches is
  a row nobody reads, and picking from it is slower than picking from five.
- **The `+` opens the theme's colors, not the theme picker.** It shows a grid of every family plus the
  neutral; clicking one adds it to the row. A **"custom color" button sits at the end of that grid**
  and leads to the hex/slider popup, so the theme's own answer is always offered first and an outside
  color costs one more deliberate step. With no theme, the `+` keeps its original meaning and goes
  straight to the color editor.
- `refreshPalette()` **reseeds the row only when every swatch fails to belong to the theme** - not when
  merely one does. The legacy default is black/red/green/blue and black *is* the neutral, so an
  "any member" test counts that whole list as themed and leaves three unreachable colors on the row.
  Reseeding is skipped entirely when `themeOffPalette` is on, or a deliberate custom swatch would be
  deleted on every launch.
- **Snapping lives in `updateColor()`**, the single point every route into the pen color passes
  through, and it pushes the snapped value back into the picker only when it actually moved - otherwise
  `setColor()` re-enters. `colorPopupPicker->onColorChanged` needs its **own** snap, because it writes
  `savedColors` directly and would otherwise be a hole straight through the palette. Alpha is carried
  across rather than snapped: transparency is the marker's business, not the palette's.
- **Tapping the already-selected swatch opens that same grid, on that swatch** - the gesture that used
  to open the hex/slider popup. In this mode the popup grows a header row: cancel at the left, delete
  at the right, and a pick **replaces the swatch in place** rather than appending. That is what makes
  the `+` recoverable: adding a colour by mistake is otherwise a one-way trip to a row too long to
  read. The header is hidden on the `+` route, where there is no swatch to cancel out of or delete.
  Delete is disabled at one swatch (the pen's colour has to come from somewhere) and, since this route
  is only reachable by tapping the *selected* swatch, it moves the pen to the next remaining one -
  otherwise the pen is left on a colour the row no longer offers, with nothing ringed.
- The right-click **"Insert Current"/"Delete" menu is suppressed while themed**: the list is
  regenerated, so editing one entry would be undone by the next rebuild. Delete is reached through the
  swatch's own grid instead, which needs no right button and so exists on a stylus too.
- **The theme is chosen when a document is created** (`ScribbleApp::askThemeForNewDoc()`, preference
  `themeAskOnNew`) - the one moment the choice costs nothing, since a theme picked later has to be
  reconciled with what is already drawn. It must be called from **both** new-document paths:
  `doNewDocument()` is only the fallback for when there is no document list, while the ordinary desktop
  route is `openOrCreateDoc()`'s `NEW_DOC` branch, which creates a file and opens it. Hooking one of
  them looks like the feature does nothing. It is *not* hooked in `ScribbleDoc::newDocument()`, which
  is also the reset path used by document close and by `ScribbleTest`.
- **Theme... stays in the overflow menu** for changing it afterwards.
- **`ScribbleDoc::setTheme()` rebuilds the toolbar explicitly**, because `setPen()` early-returns when
  the pen has not changed, and a theme change does not change the pen.
- Bookmark and link colors are now written to the **document** config as well as the global one,
  resolving the inconsistency `COLORS_SPEC.md` §10.3 flagged. `ScribbleApp::bookmarkColor` is a cached
  member, so the config write alone would not take effect until restart - `setTheme()` assigns it too.
- `themeOffPalette` is the single escape hatch, per document (§6.2) and exposed in the theme dialog.

Two open decisions, both recorded in `COLORS_HANDOFF.md` §7:

- **§10.1 is resolved: the theme's dark paper supersedes the XOR.** Invert Colors draws a themed
  document in its mirrored recipe (`paperL -> 1 - paperL`), remapping ink through `Palette::mapFrom()`
  at render time. It was briefly a document edit (restyle to the dark variant); it is now a view setting
  again - see [night-mode.md](night-mode.md).
- **A theme change is an undo item, `ThemeChangedItem`** (`COLORS_SPEC.md` §8), recorded by
  `setTheme()` inside the same action as the page recolor. This closed two gaps at once: undoing a
  restyle used to restore the stroke colors but leave the *new* recipe in place (so the next restyle
  recognised nothing), and the recipe did not reach whiteboard peers, whose pickers then offered a
  different palette. It carries the recipe plus `pageColor`/`ruleColor`/`bookmarkColor`/`linkColor` -
  the document half only; the machine-wide default a theme can also be saved as is the user's own and
  is never undone or synced. The recipe goes on the wire at full float precision, because restyle
  recognises ink only by exact color. Undoing the theming of a document that had none leaves it with
  an explicit recipe equal to the inherited one: the same palette, now pinned.
