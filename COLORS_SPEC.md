# Themed Color System — Specification

Every document carries a **theme**: a short recipe from which a palette of ink, paper and
accent colors is *generated*. The color pickers offer that palette, and only reluctantly
anything else.

This document is the rationale and the contract. The summary that lands in `CLAUDE.md` once
this ships should be much shorter.

The generator is prototyped and can be driven by hand in [`labs/color-lab.html`](labs/color-lab.html);
the measurements quoted below come from it.

---

## 0. The one-sentence version

**Nothing in this system takes a color at face value.** Not the seed the user picks, and not
the color they type into the hex box. Both are projected onto the set of colors that work.
That symmetry is the whole feature; if an exception is ever added at one end, the other end
stops making sense.

---

## 1. Core decision: the palette is generated, strokes are literal

Two statements that look contradictory and are not:

1. **A theme is a recipe, not a list of colors.** A document stores ~8 numbers and a
   generator id. The forty-odd colors of its palette are recomputed on load.
2. **A stroke stores its literal sRGB color**, exactly as today, and that is what renders.

The palette governs **what the picker offers**. It never governs **what is already drawn**.
So changing the generator — which we will do, this is the point of §3 — cannot alter a single
existing document's appearance. A stroke additionally records *which palette entry it came
from* (§5.3) purely so that an explicit, undoable "restyle" command can find it later.

This is the difference between a feature that is safe to iterate on and one that is not. If
rendering consulted the theme, every generator change would silently repaint every note ever
written, and we would be locked into version 1 forever.

### Why not store the resolved palette in the document

Tempting: write the 40 hex values into the file and never worry about regeneration. Rejected:

- It makes the document 40× larger for this feature and unreadable in a diff.
- It gives no way to tell a *theme* from a *pile of colors*, so "restyle this note" has
  nothing to work from.
- It doesn't actually solve the versioning problem, because the picker still has to decide
  what to show for a document written by a newer version.

The recipe is ~80 bytes, round-trips through machinery that already exists (§4.1), and is
legible when you open the file in an editor.

---

## 2. The generator

Lives in `ulib`, with no dependency on `Painter`, `SvgNode` or anything in `syncscribble`, so
it is unit-testable standalone exactly like `scantest.cpp` and `shapetest.cpp`.

```
ulib/oklab.h/.cpp      OKLab/OKLCh conversions, sRGB gamut boundary, cusp table
ulib/palettegen.h/.cpp PaletteRecipe, Palette, the generator registry
```

`Color` in `ulib/color.h` stays as it is — a packed `color_t` with RGB and HSV accessors.
The new math does **not** belong on `Color`: it needs floats, it needs to represent
out-of-gamut values during intermediate steps, and `Color` is used in the render hot path
where none of this is wanted.

### 2.1 The rule

For each hue family:

```
L = cusp(h).L, offset by `depth` away from the paper,
    then keep walking away from the paper until contrast(ink, paper) >= minContrast
C = vividness * maxChroma(L, h)
```

**Lightness is never chosen.** It is whatever legibility against this theme's paper forces.
Chroma is then whatever the sRGB gamut still permits at that lightness. The palette therefore
has no shared lightness and no shared chroma — the only thing its colors have in common is a
fact about the display gamut, which is why the relationship is not spottable by eye.

### 2.2 Why not a shared lightness/chroma envelope

The obvious alternative, and what Material/Radix/Tailwind do: pick one lightness ramp, cap
chroma to what every hue can reach, done. It is wrong here, and measurably so.

Reference point — the Günter Schuler "modern" palette (`213,19,121` / `0,169,201` /
`221,220,0`), measured in OKLCh:

|  | L | C | hue | % of max chroma at its own (L,h) | Δ to cusp |
| --- | --- | --- | --- | --- | --- |
| magenta | 0.573 | 0.226 | 357° | 97% | −0.077 |
| cyan | 0.678 | 0.122 | 218° | 100% | −0.132 |
| yellow | 0.867 | 0.189 | 109° | 100% | −0.097 |

Lightness range **0.294**. Chroma range nearly **2×**. No shared envelope whatsoever — but all
three ride the gamut boundary, consistently just below their hue's cusp. Forcing those three
hues to a common lightness caps every one of them at C ≈ 0.10–0.135, so magenta loses more
than half its chroma.

The envelope approach is right for UI surfaces, which must be tonally controlled. It is wrong
for ink, which wants to be as vivid as it can be while staying readable.

### 2.3 Why the walk is not optional

At its cusp, most of the hue wheel is unusable as ink on white paper:

| hue | cusp L | contrast vs white |
| --- | --- | --- |
| 109° yellow | 0.962 | **1.10:1** |
| 195° teal | 0.904 | **1.26:1** |
| 142° green | 0.868 | **1.37:1** |
| 218° cyan | 0.810 | **1.74:1** |
| 0° red | 0.648 | 3.74:1 |
| 264° blue | 0.492 | 6.93:1 |

Yellow at maximum chroma is *invisible*. So descending the boundary is not a stylistic
flourish borrowed from a design book; it is forced. That the designer's palette and the
legibility constraint point the same direction is the strongest evidence the rule is right.

### 2.4 What this buys over the naive alternatives

Same 12 hues, same paper, measured in the lab at seed 218°:

|  | colors below 3:1 | worst contrast | lightness spread |
| --- | --- | --- | --- |
| **this generator** | **0 of 12** | 3.00:1 | 0.256 |
| naive hue rotation (fixed HSV S/V) | 6 of 12 | 1.11:1 | 0.483 |
| the classic rainbow palette | 7 of 12 | 1.04:1 | 0.516 |

Note the lightness spread column carefully: the naive palettes have **more** variation, not
less. Variation is not the virtue. The virtue is that the generator's variation is *bounded
on both ends* — the legibility floor stops it going pale, the cusp stops it going flat —
whereas a rotated wheel has no floor at all and puts yellow at L≈0.97 next to blue at L≈0.45.

### 2.5 Paper is generated too, and the algorithm mirrors for dark paper

Paper is a low-chroma tint (hue 80°, warm) at the recipe's `paperL`. When `paperL < 0.5` the
ink walks **up** from the cusp instead of down and the palette becomes pastel on its own —
the same code, no dark-mode branch. The ruling color is derived from paper, not from ink.

### 2.6 Performance

`maxChroma()` is a binary search and `cusp()` a scan; neither may run per frame.

- `cusp()` is backed by a **static 360-entry table**, computed once (or baked as a constant
  array — it depends only on the sRGB primaries, so it can be generated at build time).
- A `Palette` is generated **once per theme change** and cached on `ScribbleDoc`. Drawing,
  hit testing and rendering never call the generator.

---

## 3. Generator versioning — the swappability contract

**This is the section that matters most.** We will change the generator. The requirement is
that changing it is cheap, and that it never disturbs an existing document.

### 3.1 The registry

Same shape as the `ShapeDef` registry in `shape.h`:

```cpp
struct PaletteGenDef {
  const char* id;        // "cusp-walk-1"  -- a STRING, never an index
  void (*generate)(const PaletteRecipe&, Palette* out);
};
const PaletteGenDef* paletteGenById(const char* id);   // NULL if unknown
const PaletteGenDef* defaultPaletteGen();              // newest
```

**The id is a string, not an integer index.** `CLAUDE.md` already records that this lesson was
learned twice with `shapeId`, where an index would have silently reassigned everyone's active
tool each time a shape was folded into a parameter. A palette generator is the same situation
with worse consequences: a misread index doesn't change a tool, it changes every color in the
document.

### 3.2 The rules

1. **A generator version, once shipped, is frozen.** Its output for a given recipe must never
   change. Enforced by a golden test (§9), not by discipline.
2. **Old generators are never deleted.** Each is ~60 lines of pure math with no dependencies.
   The cost of keeping five of them forever is trivial next to the cost of a document that
   opens with different colors than it was written with.
3. **New documents get `defaultPaletteGen()`.** Existing documents keep the id they were
   written with, forever, until the user explicitly restyles.
4. **An unknown generator id is not an error.** A document written by a newer version falls
   back to `defaultPaletteGen()` *for the picker only* — its strokes still render literally,
   so the document is correct, just not restyleable until the user upgrades. A dialog says so
   once; it does not block editing.
5. **A recipe field that a generator does not understand is ignored**, and fields absent from
   an old document take documented defaults. The recipe is a flat bag of named values (§4.1),
   not a positional string, specifically so that adding a knob in v2 does not disturb v1.

### 3.3 What a new generator version is for, and what it is not

Adding a knob, changing the walk, changing how hue families are spaced — new version.
Fixing a crash, a gamut-mapping rounding error, or a performance problem — same version, as
long as the *output is bit-identical*. If it isn't bit-identical, it is a new version. The
golden test is what decides this, and it is not negotiable, because "this fix is surely
harmless" is exactly how version 1 stops being version 1.

---

## 4. The theme recipe

```cpp
struct PaletteRecipe {
  std::string gen = "cusp-walk-1"; // generator id
  real seedHue    = 218;           // degrees; the ONLY thing taken from a seed color
  real vividness  = 1.0;           // fraction of each hue's own max chroma
  real depth      = 0.10;          // how far below the cusp to start
  real minContrast= 3.0;           // legibility floor against this theme's paper
  real jitter     = 11;            // deterministic hue scatter, degrees
  real paperL     = 0.99;
  real paperWarm  = 0.35;
  int  families   = 12;
};
```

Defaults are the lab's defaults and reproduce the screenshots in this session.

### 4.1 Storage — reuse the per-document config node

`Document` already round-trips a per-document `ScribbleConfig` through
`<script type="text/writeconfig">` inside `<defs id="write-defs">`
(`Document::getConfigNode()` / `resetConfigNode()`, driven from `ScribbleDoc` at
`scribbledoc.cpp:66` and `:201`). The recipe goes there as ordinary config values:

```
themeGen (string)      themeSeedHue  themeVividness  themeDepth
themeContrast          themeJitter   themePaperL     themePaperWarm
themeFamilies
```

No new file-format surface, no new save/load path, no new parser, and it inherits the
existing skip-defaults behavior so a document on default settings writes almost nothing.

The same keys exist in the **global** config as the defaults for new documents.

### 4.2 Determinism

`jitter` is derived from `(seedHue, familyIndex)` through a fixed integer hash, never from a
RNG. The same recipe must produce the same palette on every machine and every launch — a
random wobble would repaint the theme each time the app started, and would make the golden
test impossible.

---

## 5. The palette

### 5.1 Families and variants

`families` hue slots (12 by default), each with three variants:

| variant | derivation | for |
| --- | --- | --- |
| `base` | the rule in §2.1 | normal ink |
| `dark` | same hue, lower L, chroma scaled | emphasis, underlines |
| `hl` | near the cusp, low chroma, translucent | highlighter — tuned to sit *under* text, not over paper |

Family 0 sits exactly at `seedHue`; the rest at even steps plus jitter.

Plus non-family roles, generated from the same recipe: `paper`, `rule`, `bookmark`, `link`,
`selection`.

> **Known rough edge, carried over from the prototype:** the `dark` variant currently
> subtracts a fixed lightness, which at low `vividness` collapses to near-black. It should
> scale with the base's chroma. Fix before shipping; it is visible in the muted preset.

### 5.2 `Palette` is a value type

```cpp
struct Palette {
  PaletteRecipe recipe;
  Color paper, rule, bookmark, link, selection;
  std::vector<PaletteFamily> families;   // { Color base, dark, hl; real hue; }
  Color nearest(Color c, int* familyOut = nullptr) const;  // the snap function
};
```

Generated once, cached on `ScribbleDoc`, invalidated only when the recipe changes.

### 5.3 What a stroke records

A stroke keeps its `stroke` attribute exactly as today — **that is what renders**. Alongside
it, when the color came from the palette:

```
__inkbase = "#c81e3c"     the REFERENCE palette's color for this family+variant
__inkvar  = "dark" | "hl" omitted for base
```

`__inkbase` is a color from a fixed **reference palette** (the default recipe at seed 0°),
not from the document's own theme. That makes it self-describing — you can read the file and
see that this stroke "was the red one" — and it is what restyling matches on: convert
`__inkbase` to OKLCh, take its hue, find the nearest family hue in the target palette, apply
the same variant.

**Why a color and not a family index.** An index means "family 5", and 5 moves when the family
count changes, when jitter shifts, or when the generator changes hue spacing. Matching on hue
is robust to all three and degrades sensibly rather than wrongly. Same reasoning as §3.1.

Strokes with no `__inkbase` — every pre-existing document, every imported PDF, anything the
user typed into the hex box and kept — are **never touched by restyling**. That is correct
behavior, not a limitation.

Custom-attribute naming follows the existing convention (`__comx`, `__timestamp`, `__shape*`)
and is read in `Element::updateFromNode()` / written in `Element::serializeAttr()`.

---

## 6. The picker

### 6.1 Reluctance

The palette is the picker. Where `PenToolbar` today shows `savedColors` swatches plus a hex
box plus RGB/HSV sliders, it shows the theme's families.

The hex box and sliders remain, but **any color entered is snapped to the nearest palette
entry** (`Palette::nearest()`, nearest in OKLab ΔE), with a quiet inline note saying the color
was matched and to what. The user is not refused; they are answered with the closest thing
that belongs.

Snapping must be **idempotent** — `nearest(nearest(c)) == nearest(c)` — or a color will drift
under repeated edits. Pinned by test.

### 6.2 The one escape hatch

Exactly one, and it is deliberate: a document-level **"allow off-palette colors"** toggle.
Off by default. When on, the hex box passes through unsnapped, and any off-palette stroke gets
no `__inkbase` and is therefore skipped by restyling. This exists because someone will
legitimately need an exact brand color, and because a system with no escape hatch gets
worked around in worse ways.

It is per document, not global, so turning it on for one note does not quietly disable the
whole feature.

### 6.3 Every picker, not just the pen

`createColorEditBox()` is used by `pentoolbar.cpp`, `rulingdialog.cpp` (page and rule color)
and `linkdialog.cpp` (bookmark/link color). All of them get the palette. A picker that
escapes the system is a hole in it — the page color especially, since the theme's paper is
what every ink color was generated *against*.

### 6.4 Theme selection UI

A gallery of generated themes rendered as real paper with real strokes — not swatch chips,
which lie about how a color reads at stroke width. Seeds are quasi-evenly distributed, so the
gallery is generated, not curated. Reroll for more; a detail view exposes the recipe knobs
with live preview.

---

## 7. Restyling

`ScribbleDoc::restyleToTheme()` — an explicit menu command, **never automatic**.

1. New recipe replaces the document's.
2. Walk every element with `__inkbase`; compute the new color; collect into one undo item.
3. Page colors and ruling colors update via the existing `PAGE_CHANGED_ITEM` path.
4. One `HEADER` in the undo history, so the whole restyle is one Ctrl+Z.

Changing the theme **offers** to restyle and does not do it unasked. Both halves matter: a
theme change that silently rewrites a year of notes is the worst possible outcome of this
feature, and a theme change that visibly does nothing is the second worst.

---

## 8. Undo and sync

Undo items *are* the sync protocol (see the `ShapeChangedItem` precedent in `CLAUDE.md` §Shapes).

- The per-stroke color rewrite can reuse **`StrokeChangeItem`** (`STROKE_CHANGE_ITEM`) — it
  already carries arbitrary attribute changes, and `__inkbase` rides along inside
  `<addstroke>` for free because sync round-trips nodes through `SvgWriter`/`SvgParser`.
- The **recipe change itself** is document-level state with no existing item type. Adding
  `ThemeChangedItem` / `THEME_CHANGE_ITEM` is therefore a **sync wire-format change**: it needs
  `serialize()`, a type constant next to `SHAPE_CHANGE_ITEM` in `syncundo.h`, and a matching
  branch in `ScribbleSync::processItem()`. Missing any of the three means two clients in a
  shared whiteboard disagree about the palette, which is a confusing failure — their
  *strokes* stay in sync, so nothing looks broken, but their pickers offer different colors.

---

## 9. Tests

`scribbletest/colortest.cpp`, built and run standalone like `scantest.cpp` and `shapetest.cpp`
(needs `-DNDEBUG`), and called from `ScribbleTest::runAll()` so its failures land in the unit-check
count. No GL, no document, no fixtures.

Per the house rule in `CLAUDE.md`: **each check must be confirmed to fail against the broken
code**, not merely to pass against the fixed code.

1. **Golden output per generator version** — a fixed recipe produces an exact, hard-coded list
   of hex values. This is the §3.2 freeze, mechanized. It is the single most important test
   here; without it "frozen" is an intention rather than a property.
2. **Legibility invariant** — over a few hundred pseudo-random recipes, every family's base
   meets `minContrast` against that recipe's paper. Catches a regression in the walk,
   including the dark-paper mirror.
3. **Gamut** — every generated color is inside sRGB. Catches a chroma clamp going wrong.
4. **Determinism** — same recipe, two runs, identical palette. Catches a RNG creeping into
   jitter.
5. **Snap idempotence** — `nearest(nearest(c)) == nearest(c)` for a wide sample.
6. **Restyle round-trip** — theme A → theme B → theme A returns every stroke to its original
   literal color. Catches `__inkbase` being rewritten instead of preserved.
7. **Recipe round-trip** — recipe → config node → reload → identical recipe, including a
   recipe containing a field the loading generator does not know (§3.2 rule 5).

---

## 10. Integration traps

### 10.1 `invertColors` / `colorXorMask` will wreck the palette — **the sharp one**

`cfg["colorXorMask"] = 0x00FFFFFF` XORs stroke colors for the invert-colors feature
(`scribbleview.cpp:599`). A bitwise XOR of a carefully gamut-mapped color is not a color that
means anything — it does not preserve hue, chroma relationships, or legibility, and it will
turn this palette to mud.

The correct behavior is to invert by **regenerating the palette against inverted paper**,
which the generator already supports (§2.5) and which is what a user asking for a dark
background actually wants. But strokes render from their literal color, so inversion cannot
just consult the palette — it has to map each literal through `__inkbase`, and untagged
strokes still need the old XOR path.

**Decide this explicitly before implementing §7**, and expect it to be the fiddliest part of
the feature. Getting it wrong is very visible.

### 10.2 Page color is per page, theme paper is per document

`PageProperties::color` and `ruleColor` are per `Page`. A theme's paper must be written to
every page, and new pages must inherit it. Pages whose ruling is custom (PDF import, document
scan — both set `isCustomRuling` and `write-no-dup`) must be **skipped**: their background is
an image, and recoloring the paper under it does nothing good.

### 10.3 `bookmarkColor` / `linkColor` are global, not per document

They live in the global config, but the theme that generates them is per document. Either
promote them to the per-document config (consistent, small migration) or accept the
inconsistency explicitly. Do not leave it undecided — it will show up as a bookmark that
doesn't match the note it is in.

### 10.4 Highlighter alpha interacts with the `sRGB` pref

With `cfg["sRGB"] = 1` blending happens in linear space, where a given alpha reads far
stronger than the number suggests — this already bit the relative-width ruling preview
(`CLAUDE.md` §Text marker width). The `hl` variant must be tuned with that pref on, and its
contrast checked *under* text rather than against bare paper.

### 10.5 `ScribbleTest` constructs objects directly

`ScribbleTest` builds a `ScribbleMode` without calling `loadModes()`, which previously cost
six failing fixtures from uninitialized memory (`CLAUDE.md` §Switch back). Anything the theme
adds must be initialized in the **constructor**, not only in the load path.

### 10.6 Changing page color changes every thumbnail

Thumbnail comparison is already the flaky part of `ScribbleTest` (16 of 17 fail on this
machine for unrelated GPU reasons). Restyling a fixture document will change its thumbnail
legitimately. Don't read those failures as regressions, and don't add theme coverage that
depends on thumbnails.

### 10.7 `PenToolbar`'s saved-colors machinery is partly obsolete

`savedColors`, `addColorBtn`, `colorCtxMenu` and `colorMenuDelete` implement a user-managed
swatch list. Under a themed palette the list is generated, so "add" and "delete" no longer
mean anything. Remove them from the palette path rather than leaving dead controls that
appear to work — and note `PenToolbar` has two layouts (`compact` and full), both of which
need the change.

### 10.8 Two colors are not ink and must not be themed

Black and white. Plenty of people write in black on white and would experience a themed
near-black as a bug. Reserve a neutral family that is exempt from the walk.

---

## 11. Phasing

Each phase is independently useful and independently revertible.

1. **`ulib` math + generator + `colortest.cpp`.** No UI, no document changes. Ends with the
   golden test passing and §9.1–9.5 green. This is the load-bearing phase; everything after it
   is plumbing.
2. **Recipe storage + `Palette` on `ScribbleDoc`.** Round-trips through the config node.
   Nothing visible changes yet.
3. **Picker replacement in `PenToolbar`**, snapping, off-palette toggle. First visible phase.
4. **Paper, ruling and accents from the theme**, including §10.2 and §10.3.
5. **Restyle command**, `ThemeChangedItem`, undo + sync (§8), and the `invertColors`
   decision from §10.1.
6. **Theme gallery UI.**

---

## 12. Deferred

- **App chrome (toolbars, menus, panels).** Explicitly out of scope; `uiTheme` stays a
  dark/light switch. The recipe has room for it later.
- **A second hue seed**, bunching families toward two anchors rather than one. The data model
  should leave room; the UI should not ship it first.
- **Seed extracted from a photo.** Falls out of §2 for free — it is just a hue — but needs UI.
- **Wide-gamut (P3) displays.** The generator hard-codes the sRGB boundary. On a P3 display the
  palette is correct but conservative. Revisit only when the renderer itself is wide-gamut.

---

## 13. Open questions

1. **§10.1 inversion.** The one genuine design hole. Needs a decision before phase 5.
2. **Does `minContrast` belong in the UI?** It is the knob with the largest visual effect
   (4.5:1 costs yellow 42% of its chroma, teal 39%, green 37%) but it is also the hardest to
   name for a non-technical user. Currently specified as a fixed 3.0.
3. **What happens to a document whose theme is "none"?** Every existing file. Proposed: treat
   it as the default recipe for picker purposes and leave every stroke untagged, so nothing is
   restyleable until the user picks a theme. Needs confirming against the doc-list preview path.
