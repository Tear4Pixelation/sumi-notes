# Themed Color System — Handoff

Written for the next agent picking this up. `COLORS_SPEC.md` is the rationale and the contract;
`CLAUDE.md` §Themed colors is the short summary. **This document is the state of play**: what is
built, what was learned the hard way, what is deliberately not done, and what to do next.

Phases 1–5 of `COLORS_SPEC.md` §11 are implemented, plus the theme picker (§7). What remains is
user-created/saved themes and the sync gap in §7's open decisions.

---

## 1. What exists

| file | what |
| --- | --- |
| `ulib/oklab.h`/`.cpp` | OKLab/OKLCh, sRGB gamut boundary, cusp table, ΔE, WCAG contrast |
| `ulib/palettegen.h`/`.cpp` | `PaletteRecipe`, `Palette`, the `cusp-walk-1` generator, the registry, the shipped theme table |
| `syncscribble/themedialog.h`/`.cpp` | the theme picker dialog |
| `scribbletest/colortest.cpp` + `colortest_golden.inc` | standalone palette math tests |
| `labs/color-lab.html` | browser prototype of the generator, every knob live, A/B vs naive |

Touched: `scribbleconfig` (recipe storage), `scribbledoc` (palette cache, `setTheme`,
`restyleToTheme`), `pentoolbar` (themed swatches, add-color grid, snapping), `scribbleapp` +
`mainwindow` (menu, new-document prompt), `scribbletest` (two app-level tests).

### The one-paragraph model

A **theme** is a `PaletteRecipe` — nine values — stored per document. From it a **`Palette`** is
*generated*: a paper color, a rule color, accents, a `neutral`, and N **families**, each with three
variants (`base`, `dark`, `hl`). The palette decides **what the pickers offer**. It does **not**
decide what is already drawn: strokes keep literal sRGB colors, and only the explicit
`restyleToTheme()` command rewrites them.

---

## 2. The generator, and the one rule that must not be broken

```
for each hue family h:
    L = cusp(h), stepped `depth` away from the paper,
        then stepped further until contrast(ink, paper) >= minContrast
    C = vividness * maxChroma(L, h)
```

**Lightness is never chosen.** It is whatever legibility forces; chroma is whatever the gamut then
allows. This is why the palette has no shared lightness and no shared chroma, and why its coherence
is not visible as a relationship between any two colors.

### `cusp-walk-1` is FROZEN

Its output for a given recipe must never change — not a constant, not a loop bound, not the order of
two floating-point operations. **If the output changes, it is a new generator with a new string id**,
however harmless the change looks. `colortest_golden.inc` pins every color it produces; the failure
message tells you to add an id rather than regenerate the table. Regenerating is only correct while a
generator has never shipped.

Old generators are never deleted — each is ~80 lines of dependency-free math.

### Things in the generator that look arbitrary and are not

- **Family 0 is never jittered.** The seed hue must actually appear in its own palette, or "this is
  the color you chose" is a lie and the seed control has nothing to point at.
- **`jitter` must stay below half the family spacing.** At the default 12 families the spacing is 30°,
  so jitter must stay under 15; it is 11. Above that, families stop being in hue order and
  `Palette::mapFrom()`'s ordinal mapping silently loses its meaning. This is the real reason the family
  count cannot simply be raised: 16 families would be 22.5° apart, needing jitter below 11.
- **The dark variant takes its chroma as a fraction of what is available at its own lightness**, not
  as a fraction of the base's chroma. Scaling twice collapsed it to near-black at low vividness.
- **`hash01()` is not a RNG.** A random jitter would repaint every theme on every launch, make the
  same document look different on two machines, and make the golden check impossible.
- **`referencePalette()` sets every recipe field explicitly.** It is a frozen constant that happens
  to be written as a recipe. Taking struct defaults would tie it to whatever the current build
  considers default, so changing e.g. the default family count would silently redefine it. *(This was
  a live bug until phase 5; it is fixed, do not "simplify" it back.)*

---

## 3. Storage

The recipe rides `ScribbleConfig`, as ordinary values: `themeGen`, `themeSeedHue`, `themeVividness`,
`themeDepth`, `themeContrast`, `themeJitter`, `themePaperL`, `themePaperWarm`, `themeFamilies`, plus
`themeOffPalette` and `themeAskOnNew`.

This needed **no new file-format construct**: `ScribbleDoc::cfg` is a per-document config whose
`upconfig` is the global one, and it already round-trips through the document's
`<script type="text/writeconfig">` node. Two consequences worth knowing:

- A document that has never been themed **inherits the global recipe**. That is the whole reason an
  untitled document needs no special case.
- `setThemeRecipe()` **always writes an explicit generator id**, even when it matches this build's
  default. A document recording only "whatever is current" would change palette the next time the
  default moves.

The `Palette` is cached on `ScribbleDoc` and invalidated in `loadConfig()` — the single point every
`cfg` swap passes through (plus `resetDocPrefs()`).

---

## 4. Traps (each of these cost real time)

### 4.1 `setPageProperties()` flattens pages
It applies one `PageProperties` to every page, and `Page::setProperties()` assigns the whole struct —
so it pushes the current page's ruling, and any differing page's dimensions, onto every other page.
`setTheme()` therefore loops pages itself, replacing only `color` and `ruleColor`. Pages with
`isCustomRuling` (PDF import, document scan) are skipped: their paper is an image.

### 4.2 Two new-document paths
`ScribbleApp::doNewDocument()` is only the fallback for when there is no document list. The ordinary
desktop route is `openOrCreateDoc()`'s `NEW_DOC` branch, which creates a file and opens it. Hooking
one of them makes the feature look inert. Both call `askThemeForNewDoc()`. It is deliberately *not*
hooked in `ScribbleDoc::newDocument()`, which is also the reset path used by document close and by
`ScribbleTest` — neither may open a dialog.

### 4.3 `setPen()` early-returns
It returns immediately when the pen has not changed — which a theme change does not. So
`ScribbleDoc::setTheme()` calls `penToolbar->refreshPalette()` explicitly.

### 4.4 Snapping has two entry points
`PenToolbar::updateColor()` is the single point every route into the pen color passes through, and
snapping lives there. But `colorPopupPicker->onColorChanged` writes `savedColors` **directly**,
bypassing it, and needs its own snap — otherwise the custom-color route is a hole straight through
the palette. Snapping pushes back into the picker only when the value actually moved, or
`setColor()` re-enters. Alpha is carried across, never snapped.

### 4.5 The swatch reseed test must be "all", not "any"
The legacy default is `black,red,green,blue`, and **black *is* the neutral**, so an "any member" test
counts that whole list as belonging to the theme and leaves three unreachable colors on the row.
Reseeding is skipped entirely when `themeOffPalette` is on, or a deliberate custom swatch would be
deleted on every launch.

### 4.6 The grid popup has two modes, and one index
`PenToolbar::openPaletteGrid(editIdx)` serves both the `+` (append) and the tap-the-selected-swatch
route (replace in place, with a cancel/delete header). The mode is carried in `paletteEditIdx`, which
is an index into `savedColors` — so `rebuildGrids()` must close the popup and clear it, exactly as it
already does for `colorPopupIdx`. A cell's handler reads it into a local *before* closing the popup,
for the same reason.

### 4.7 `flex-break`, not `width`, makes a grid wrap
Setting a width on the container does **not** wrap the row in this layout engine. The add-color grid
forces rows with an explicit `flex-break=before` on every 4th cell — the same mechanism
`PaletteWidget::addButton()` uses.

### 4.8 A restyle must be one undo step
`setTheme()` brackets its own undo action for the page recolor, so `restyleToTheme()` calling it
normally produced **two** Ctrl+Z steps — press once and the strokes revert while the paper stays on
the new theme. Hence `setTheme(..., ownAction=false)` with the caller bracketing both halves.

### 4.9 Ordinal mapping, never hue proximity
Both palettes carry up to ±jitter of scatter, so two offsets can differ by 2×jitter against a smaller
spacing, and two families collapse onto one — a 16-color document quietly becoming a 15-color one.
Measured: 5 collisions on absolute hue, still 2 on seed-relative hue, 0 on ordinal. The ordinal is
**derived from the color**, never stored, so a future generator can still change the family count.

---

## 5. Phase 5 (restyle) — and a deliberate deviation from the spec

`ScribbleDoc::restyleToTheme(recipe, applyPages, globalDefault)` → count of strokes changed.

**Strokes carry no `__inkbase`.** The spec had each stroke record which reference-palette entry it
came from. That turned out to be unnecessary: at the moment of restyling we know *both* palettes —
the old recipe is still in `cfg`, the new one is the argument — so a stroke is identified by its
literal color being an exact member of the **old** palette (`Palette::mapFrom`). Two things fall out,
both strictly better:

- no new file-format attribute, and
- **strokes drawn before this feature existed are restyleable**, where a tag-based scheme could only
  ever restyle strokes drawn after it shipped.

The cost: a document carried through several themes *without* restyling keeps only its most recent
theme's strokes mappable. Older ink is left alone — the same fail-safe rule that protects imported
PDFs and deliberate off-palette strokes.

Also note: the neutral maps to the neutral (**not** by ordinal), because it is exempt from the walk in
both palettes. Variants are preserved — a `dark` stroke restyles to the new theme's `dark`, or every
emphasis stroke in the document silently flattens into ordinary ink.

Groups and `<text>` elements are **skipped**: they descend recursively and need the clone dance
`Selection::setStrokeProperties()` does. That is why the function returns a count rather than
claiming to have restyled everything. Extending to groups is a genuine TODO.

---

## 6. Testing discipline — read this before adding a check

Every check in this area was mutation-tested: deliberately break the code, confirm the check fails.
**Five assertions have now been written in this feature that were true either way and tested
nothing.** Two were caught only because the mutation didn't fail:

- a page-flattening check that read the *current* page's properties — which `setPageProperties()`
  leaves untouched **by construction**, since it is the source of what gets pushed onto the others;
- a reload check that compared the palette against the themed palette — which is exactly what a
  stale cache also returns.

And two coverage gaps found the same way: the restyle test originally drew only a *base* color, so
neither the neutral nor the variant path was exercised, and both corresponding mutations passed.

Where the tests live:
- `scribbletest/colortest.cpp` — standalone (`--dump` regenerates the golden table), 10 checks
- `ScribbleTest::themeRoundTripTest()` — recipe round-trip, inheritance, page preservation, unknown gen id
- `ScribbleTest::restyleTest()` — base/dark/neutral/off-palette mapping, single undo step
- `testShippedThemes()` in `colortest.cpp` — every shipped theme in **both** modes: legibility, the
  vividness floor, within-theme separation, the dark contrast cap, and that a theme's recipe resolves
  back to that theme. Mutation-tested: dropping the dark cap fails 3 checks, vividness under the floor
  1, a duplicate theme id 1, `paletteThemeIndexOf` ignoring the cap 12, an unreachable `minContrast` 2,
  and disabling the contrast walk 30. Note that setting a theme to `minContrast = 15` does **not** fail
  — 15:1 is reachable on white paper, so the walk satisfies it honestly. That was a bad mutation, not
  an inert check; the distinction is worth keeping in mind before concluding a check is dead.

Baseline on this machine: **0 failed tests, 0 failed unit checks, 16 failed thumbnails** (the
thumbnail count is the documented GPU baseline, and is what the binary exits with).

---

## 7. The theme picker (phase 6) — built, and why it looks the way it does

`ThemeDialog` is now a 3x4 grid of twelve named themes, a dark-paper toggle, and three checkboxes.
The sliders, the generated seed gallery and "Use for new documents" are gone. The reasoning matters
more than the result, because every one of these was arrived at by measuring and three of them
reversed a decision that looked obvious first:

**Seed hue cannot differentiate themes, and never could.** With families spread over the full wheel,
palettes at seeds 180° apart differ by a mean of 10.1° per color against a 30° family spacing — less
than the ±11° jitter. The seed only decides which family is called #0. The old gallery of twelve
seeds was therefore twelve near-identical palettes, and making its thumbnails bigger would only have
made that more obvious.

**Vividness could only ever make a theme worse.** The default is 1.0; the slider ran 0.25–1.0.

**Hue arcs were prototyped and rejected — the rejection is the useful part.** Restricting families to
an arc does make the seed matter, and separation stays fine on paper. But arc and family count trade
directly:

| arc | families that stay as separated as the shipping palette |
| --- | --- |
| 120° | 4 |
| 180° | 6 |
| 240° | 8 |
| 360° | 12 |

At a 120° arc with 12 families the minimum pairwise ΔE is **0.0000** — two families are the same
color. And the fatal objection is simpler than any of that: a 120° "warm" theme has *no blue in it*.
A note-taking palette that cannot do red and blue and green on one page is not a palette. Every theme
covers the full wheel.

**A theme is ink character + paper tint, and does not own light-vs-dark.** `paperL` does two jobs —
which side of the mirror, and how light exactly — and only the second belongs to a theme. A theme
carries a `paperOffset`; `paletteThemeRecipe(theme, dark)` supplies the side. Otherwise "vivid ink on
white" and "vivid ink on black" become two tiles for one theme.

### The chroma budget — why the table looks conservative

Offering each family's **dark variant** in the picker was tried and reverted, and the measurement
that killed it also explains the table's shape. On light paper the variants are good (17 cells at
min pairwise ΔE 0.067 against 13 at 0.041). On **dark** paper they collapse to **0.0137** — for some
families the "dark" *is* its base, because the walk has already pushed ink up toward light and
`dL = min(0.92, L + 0.13)` clamps. That clamp is inside frozen `cusp-walk-1`, so it cannot be fixed
without a new generator id. Families went back to 12 and the grid back to bases only.

Reverting to 12 families then broke the separation check for nine theme/mode pairs, which exposed the
real constraint. **Separation between 12 families 30° apart tracks their mean chroma**, and two
recipe fields desaturate — measured at 12 families on light paper:

| | separation | mean chroma |
| --- | --- | --- |
| `minContrast` 3.0 → 7.0 at vividness 1.0 | 0.0415 → 0.0293 | 0.180 → 0.149 |
| vividness 1.0 → 0.70 at `minContrast` 3.0 | 0.0415 → 0.0294 | 0.180 → 0.126 |

They **compound**: a theme at vividness 0.90 *and* 7:1 measured 0.0267. `depth` is free on light paper
(0.0415 flat from depth 0 to 0.24) and only mildly costly on dark, which is why it looked like the
culprit at first and is not budgeted.

So the themes are held to **vividness ≥ 0.90 and `minContrast` ≤ 5.0**, and all twelve clear 0.0302 —
73% of the shipping palette's 0.0414. The cost is that there is no genuinely *muted* theme and no 7:1
*Contrast* theme; those were the two most distinctive, and they are the two 12 families cannot afford.
At 8 families the same themes measured 0.039–0.051, so this is a real trade between hue count and ink
character, not a tuning accident.

### Two constraints in the table that are load-bearing

- **The chroma budget above** (vividness ≥ 0.90, `minContrast` ≤ 5.0), which `colortest.cpp` checks
  per theme rather than trusting the table to stay in line.
- **`minContrast` capped at 4.5 in dark mode.** The walk steps *away* from the paper, so a high floor
  is richer ink on white and *bleached* ink on black. Uncapped, "Deep" and "Contrast" rendered as the
  palest themes in the set — identity inverted. `paletteThemeIndexOf()` applies the same cap when it
  compares, or a dark "Contrast" stops recognising itself and the dialog loses its ring.

### What is still open

- **Parchment vs Graphite** are the closest pair (ΔE 0.0224 in dark mode): both muted-with-depth,
  differing mainly in paper warmth, which collapses when both papers are near-black.
- **Dark mode reads flatter than light mode** across the set — legible and distinct, but less
  characterful.
- Named/saved *user* themes, a live preview on the real document, and seeding from a photo remain
  unbuilt. The photo seed is moot now that hue is not a theme axis.

### Known open decisions

1. **`COLORS_SPEC.md` §10.1 — resolved, with a cost worth knowing.** For a themed document, Invert
   Colors now mirrors the theme's own recipe (`paperL -> 1 - paperL`, the case the generator already
   handles by walking up from the cusp) and remaps the strokes through `Palette::mapFrom()`, instead
   of XOR-ing to a photographic negative. It round-trips exactly, and is pinned by `restyleTest()`.
   **The cost: Invert Colors is now a document edit, not a view filter.** It is one undo step, but it
   is saved and it syncs — so it is no longer a way to read in the dark without changing the file.
   Documents with no palette keep the XOR path. If that trade is wrong, the alternative is a
   color-lookup hook in `Painter` (it only has `setColorXorMask` today), which is a much larger change.

2. **Undo of a restyle does not restore the recipe** — the live defect to fix next. Stroke colors come
   back (they are `StrokeChangedItem`s) but the recipe lives in the document config, which is not an
   undo item, so after Ctrl+Z the document holds the *new* theme's recipe and the *old* theme's ink.
   Nothing looks wrong until the next restyle, which finds no strokes it recognises and silently does
   nothing. `restyleTest()` hit this and works around it with an explicit `setTheme()`; the real fix is
   `COLORS_SPEC.md` §8's `ThemeChangedItem`, which is also what §7.3 below needs for sync. Doing both
   at once is the sensible move.
2. **The recipe does not sync.** Restyle emits ordinary `StrokeChangedItem`s, which sync correctly,
   but the recipe itself lives in the document config node, which is not part of the sync protocol.
   Two clients on a shared whiteboard therefore agree on every stroke and disagree on what their
   pickers offer. `COLORS_SPEC.md` §8 specifies a `ThemeChangedItem` + `ScribbleSync::processItem()`
   branch for this; it is **not built**.
3. **Changing a theme resets the toolbar swatch picks** to the new theme's default five, because the
   old picks are not colors the new palette can make. Defensible, but it is a behavior users notice.
4. **App chrome is out of scope** — `uiTheme` remains a dark/light switch.
5. **The `ulib` submodule — resolved.** `oklab.*` and `palettegen.*` are now tracked in `ulib`, so
   changes to them do show up (as `m ulib` in the superproject, since `.gitmodules` has
   `ignore = untracked`). They no longer exist only in the working tree. Note the consequence: a
   change to the palette code needs a commit **in the submodule** as well as the superproject bump —
   committing only in `Write` leaves the generator behind.

---

## 8. Measurements worth not re-deriving

- Schuler "modern" palette in OKLCh: magenta L .573 / C .226 / h 357°, cyan .678 / .122 / 218°,
  yellow .867 / .189 / 109°. All at 97–100% of max chroma for their own lightness, all 0.08–0.13
  *below* their hue's cusp. Lightness range 0.294 — no shared envelope.
- At its cusp, yellow is **1.10:1** on white — invisible. Teal 1.26, green 1.37, cyan 1.74. Blue is
  6.93. This is why the walk exists.
- Generator vs alternatives at seed 218, 12 families: **0 of 12** below 3:1, vs 6 of 12 for naive hue
  rotation and 7 of 12 for the classic rainbow palette. The naive palettes have *more* lightness
  variation (0.48, 0.52 vs 0.26) — variation is not the virtue; bounded variation is.
- Forcing a shared envelope caps every hue at C ≈ 0.10–0.135, costing magenta more than half its
  chroma.

These are pinned by `colortest.cpp`, so if the spec and the code diverge the tests say so.
