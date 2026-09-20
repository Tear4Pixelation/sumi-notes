# Themed Color System — Handoff

Written for the next agent picking this up. `COLORS_SPEC.md` is the rationale and the contract;
`CLAUDE.md` §Themed colors is the short summary. **This document is the state of play**: what is
built, what was learned the hard way, what is deliberately not done, and what to do next.

Phases 1–5 of `COLORS_SPEC.md` §11 are implemented. The remaining work is the **theme creation UI**,
which is large — see §7.

---

## 1. What exists

| file | what |
| --- | --- |
| `ulib/oklab.h`/`.cpp` | OKLab/OKLCh, sRGB gamut boundary, cusp table, ΔE, WCAG contrast |
| `ulib/palettegen.h`/`.cpp` | `PaletteRecipe`, `Palette`, the `cusp-walk-1` generator, the registry |
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
- **`jitter` must stay below half the family spacing.** At 15 families the spacing is 24°, so jitter
  must stay under 12; it is 11. Above that, families stop being in hue order and
  `Palette::mapFrom()`'s ordinal mapping silently loses its meaning.
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

### 4.6 `flex-break`, not `width`, makes a grid wrap
Setting a width on the container does **not** wrap the row in this layout engine. The add-color grid
forces rows with an explicit `flex-break=before` on every 4th cell — the same mechanism
`PaletteWidget::addButton()` uses.

### 4.7 A restyle must be one undo step
`setTheme()` brackets its own undo action for the page recolor, so `restyleToTheme()` calling it
normally produced **two** Ctrl+Z steps — press once and the strokes revert while the paper stays on
the new theme. Hence `setTheme(..., ownAction=false)` with the caller bracketing both halves.

### 4.8 Ordinal mapping, never hue proximity
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
- `scribbletest/colortest.cpp` — standalone (`--dump` regenerates the golden table), 9 checks
- `ScribbleTest::themeRoundTripTest()` — recipe round-trip, inheritance, page preservation, unknown gen id
- `ScribbleTest::restyleTest()` — base/dark/neutral/off-palette mapping, single undo step

Baseline on this machine: **0 failed tests, 0 failed unit checks, 16 failed thumbnails** (the
thumbnail count is the documented GPU baseline, and is what the binary exits with).

---

## 7. Next: the theme creation UI (the big one)

`ThemeDialog` today is a working stopgap, not the designed experience. It has a generated gallery,
three sliders, and four checkboxes. What it lacks:

- **`themeFamilies` is not exposed.** It is a recipe field; the default is 15 (so the add-color grid
  is 16 cells including the neutral). Changing it changes the whole palette's structure.
- **No named/saved themes.** A recipe is nine numbers; letting people name and reuse one is cheap and
  obviously wanted.
- **No live preview on the real document** — the preview is a synthetic strip of squiggles.
- **The gallery is one flat row of 12.** Rerolling, and seeding from a photo (the spec notes a seed
  from an image falls out for free — it is just a hue), are both unimplemented.
- **The dark-paper control is a checkbox**, which is right for the mirror but gives no control over
  *how* dark.
- Deciding a theme by sliders is the wrong verb for most people. The gallery is the good path; it
  should probably grow, and the sliders shrink into an "adjust" disclosure.

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
5. **The `ulib` submodule.** `oklab.*` and `palettegen.*` are **untracked** in the `ulib` submodule,
   which has `ignore = untracked` in `.gitmodules` — so they are invisible to `git status` in the
   superproject. This is the same state as the existing document-scan files (`homography.cpp` etc.),
   so it is pre-existing practice rather than a new problem, but it means this code currently exists
   only in the working tree. **Resolve this before relying on the work.**

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
