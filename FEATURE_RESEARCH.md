# Feature Feasibility Research

Research only — no implementation. Covers the five features blocking daily use:
shape recognition, editable shapes, shape menu, library, PDF import.

Baseline facts that shape every answer below:

- The document model is **already vector**: a `Document` is `Page`s, each page is an
  `SvgDocument`, each stroke/object is an `Element` (`element.h:141`) wrapping an
  `SvgNode`. `SvgPath`, `SvgRect`, `SvgImage` node types all exist in `usvg/svgnode.h`.
- The repo is **AGPL-3.0** (`LICENSE`). This is the single most important fact for PDF:
  it makes MuPDF (AGPL) usable without buying a commercial license.
- PDF **export** already exists (`usvg/pdfwriter.cpp`, 886 lines). There is **no PDF
  reader anywhere** in the tree — `pdfwriter` is write-only and gives you nothing for
  import.
- Undo/sync are one system (`syncundo.h` `UndoHistory` + `scribblesync.cpp`). Any new
  document mutation must be expressed as an `UndoHistoryItem`, or it silently breaks
  the shared-whiteboard sync.

Difficulty summary:

| Feature | Difficulty | Rough effort | Main risk |
|---|---|---|---|
| Shape tool (drag-to-draw rect/arrow/etc.) | **Easy–Medium** | 500–800 LOC | grouped arrows vs. `isMultiStroke()` |
| Editable vector shapes | **Medium** | 600–900 LOC | data-model design, SVG round-trip |
| Library as managed vault | **Medium** | 800–1200 LOC | import-on-open flows, not the browser itself |
| PDF import (raster background) | **Medium–Hard** | 2–4 weeks | build system across 6 platforms, binary size |
| PDF import (true vector) | **Hard** | +4–8 weeks | unbounded fidelity tail |
| Shape recognition | **Hard** | 1500–2500 LOC + weeks of tuning | the algorithm itself, as you suspected |

---

## 1. Shape recognition (draw, hold, snap)

### Where it hooks in

Two touch points, both in `scribblearea.cpp`:

- **Dwell detection** — `ScribbleArea::doMoveEvent`, `case MODE_STROKE`
  (`scribblearea.cpp:~1700`). Reset a timer on every move event; when it fires with the
  pointer still down and barely moving, run recognition.
  Note `scribblearea.cpp:1695` currently reads
  `if(currMode != MODE_STROKE && widget) widget->startTimer(timerPeriod);` — the stroke
  mode is explicitly excluded from the timer today, so that line changes.
- **Commit** — `ScribbleArea::doReleaseEvent`, `case MODE_STROKE`, right after
  `currStroke = scribbleDoc->strokeBuilder->finish();`. This is where the stroke becomes
  a real `Element`.

`SvgGui::LONG_PRESS` (`ugui/svggui.h:266`, handled at `scribblewidget.cpp:143`) exists but
is a *press-without-move* gesture. It will not fire for "draw a shape, then hold", so you
need your own dwell timer. That is a small amount of code, not a blocker.

### What already exists and is reusable

- `SimplifyFilter` (RDP) in `strokebuilder.cpp`, and standalone `simplifyRDP()` used by
  `LassoSelector` (`selection.cpp:1140`).
- `Path2D` in `ulib/path2d.h` with `addEllipse`, `addRect`, `addArc`, `addLine`,
  `pathLength()`, `positionAlongPath()`.
- `ScribblePen::SNAP_TO_GRID` and `ScribblePen::LINE_DRAWING` flags — precedent for
  constrained/snapped geometry already wired through press and move events.
- Page ruling (`Page::xruling()`, `yruling()`) for snapping shapes to the grid.

### The actual hard part, and one non-obvious trap

The recognizer should be **rule-based, not ML**. A pipeline that gets to "on point":

1. Resample the raw input to uniform arc length.
2. RDP-simplify at a zoom-independent tolerance.
3. Classify open vs closed (endpoint distance / path length).
4. Detect corners (curvature peaks, or vertex count after aggressive RDP).
5. Run candidate fitters in parallel, each returning a **normalized residual**:
   line (least squares), circle (Taubin/Kåsa algebraic fit), ellipse (conic fit),
   rectangle (oriented bbox + corner-angle check), triangle/polygon (from corners),
   arrow (long shaft + two short tails at the end).
6. Pick the lowest residual under a threshold; otherwise keep the ink untouched.
7. Snap: quantize angles to 15°, align edges to page ruling, snap endpoints to nearby
   existing shape endpoints.

**The trap:** for pressure-sensitive pens, `StrokedStrokeBuilder` stores the stroke as a
*filled outline*, not a centerline. You cannot simply replace the geometry of an existing
stroke with a clean circle — you'd be deforming an outline. Recognition must work on the
input centerline (`Element::toPenPoints()` / `fromPenPoints()`, `element.h:209`) and then
**regenerate** the element, ideally as a constant-width `SvgPath` with `stroke-width`
(`Element::STROKE_PEN_CLASS`) rather than a filled outline. That choice also makes
feature 2 (editing) almost free, so it is the right call — but it means a recognized shape
is visually a different kind of object from a hand stroke. Decide this up front.

### Undo

If recognition fires before release, the stroke was never committed — no undo item needed,
and the user can keep drawing. If you want "snap, then Ctrl+Z reverts to the raw ink"
(better UX), emit a delete+add pair or a `StrokeChangeItem` (`syncundo.h:39`).

### Verdict

Integration is easy; the algorithm is the whole job. Budget 2–4 weeks of tuning against
your own handwriting. New file `shaperecognizer.cpp` (~1500–2500 LOC) plus ~150 LOC of
integration. **Do this last** — it should consume the shape data model from feature 2,
not invent its own.

---

## 2. Editable vector shapes

### Current state

Better than you'd expect. Selection already supports move, scale, rotate, and crop with
grab handles: `RectSelector::scaleHandleHit` / `rotHandleHit` / `cropHandleHit`
(`selection.cpp:748–800`), drawn at `selection.cpp:1320–1350`, driven by
`MODE_SCALESEL` / `MODE_ROTATESEL` / `MODE_CROPSEL` in `doReleaseEvent`.

What's missing is **per-shape parameter editing** — dragging an ellipse's radius, an
arrow's tip, a rectangle's corner independently.

### Two designs

**(a) Parametric shapes (recommended).** Store shape type and parameters as attributes on
the node (`write:shape="ellipse"`, cx/cy/rx/ry) and regenerate the `Path2D` whenever a
parameter changes. `Element` is already an `SvgNodeExtension` with a
`serializeAttr(SvgWriter*)` hook (`element.h`), which is exactly the mechanism for
round-tripping custom data through the SVG file. Needs:
- a `ShapeSelector : Selector` with per-shape-type handles (mirror `RectSelector`),
- new modes alongside `MODE_SCALESEL` (`scribblemode.h` — note the comment: *only add at
  the end*, values are persisted to config),
- one new undo item type in `syncundo.h`.

**(b) Generic path-vertex editing.** Drag any point of any `SvgPath`. More general, but
much more UI work and strictly worse for arrows and rectangles, where you want
semantic handles, not vertices.

Go with (a). Unrecognized/hand-drawn strokes keep the existing bbox-handle behavior.

### Verdict

**Medium.** ~600–900 LOC across a new `shapeselector.cpp`, `selection.cpp`,
`scribblemode.h`, `syncundo.h`. No architectural obstacles — the SVG model, the extension
mechanism, and the handle UI all already exist. Design this **together with** feature 1
and **before** it.

---

## 3. Shape tool — drag to draw rectangles, arrows, lines, ellipses

Not a stamp/drop-in palette. The interaction is: pick a shape from the menu, then **drag on
the canvas to define it** — press sets one corner/endpoint, drag previews live, release
commits. An arrow is a line with a head at the release end, so its direction and length
come straight out of the drag.

### Interaction model

Add `MODE_DRAWSHAPE` to the end of the enum in `scribblemode.h` (the file warns:
*don't change numeric values — only add at end*, since modes are persisted to config), with
the active shape type held on `ScribbleMode` next to `drawTool`.

Then three handlers in `scribblearea.cpp`, all mirroring existing code:

- **Press** — record `initialPos`, create the live `Element`. Do **not** route this through
  `StrokeBuilder`; that machinery is for sampled input. Just hold an `Element*` on
  `ScribbleArea` the way `MODE_BOOKMARK` already does (`scribblearea.cpp:~1560`).
- **Move** — regenerate the `Path2D` from `(initialPos, pos)` and call
  `scribbleDoc->updateCurrStroke(dirty)` to repaint. The in-progress object is painted at
  `scribblearea.cpp:2733` via `SvgPainter(painter).drawNode(...)` — a live shape element can
  be drawn through the identical call, so live preview is nearly free.
- **Release** — `currPage->addStroke()` inside `startAction`/`endAction`, exactly as
  `MODE_BOOKMARK` does.

Modifier behavior worth copying from existing code: `MODEMOD_PENBTN` is already used as
"constrain" elsewhere (15° rotation steps), so pen-button-held → square/circle/45°-snapped
line. Page ruling (`Page::xruling()` / `yruling()`) gives grid snapping for free.

### Geometry generators

Small and self-contained — one function per shape, `(Point start, Point end) -> Path2D`,
using `Path2D::addRect`, `addEllipse`, `addLine`, `addArc` which all already exist:

- line, rectangle, ellipse, rounded rect: one call each.
- **arrow**: shaft plus head. The head must scale with stroke width, not with arrow length,
  or long arrows get absurd heads. Two representations:
  - **Single filled path** outlining the whole arrow. One node, but the "stroke width"
    is baked into the outline — bad for later editing, same trap as pressure strokes.
  - **A group** (`SvgG`) of a stroked shaft path plus a filled head path. Keeps the shaft
    a real stroked line that stays editable. **Recommended.**

### The one real integration detail

If arrows are groups, they hit `Element::isMultiStroke()` (`element.h:168`), which today is
just `return isHyperRef();`. Selection, erase, and transform logic branch on it. The author
anticipated this — the adjacent comment reads *"all children of multi-stroke have Element
exts; may extend to include bookmark groups later"* — so extending it to shape groups is
the intended path, but it must be done deliberately or grouped arrows will misbehave under
the stroke eraser and selection.

### Menu

`PaletteWidget` / `PenToolbar` (`pentoolbar.h`) already handle an icon grid with overflow;
`mainwindow.cpp:1327` / `1402` show the action wiring. The menu sets the active shape type
and switches to `MODE_DRAWSHAPE`.

**Verdict: Easy–Medium.** ~500–800 LOC plus icons. No new rendering, no new undo machinery,
no new file format concerns. Start here — the shape types and their parameters defined in
this step *are* the data model that feature 2 edits and feature 1 emits, so building it
first means designing that model once instead of three times.

---

## 4. Library as a managed vault

The goal is **not** a file browser pointed at a default folder. It's an app-owned store:
one location on disk that the app controls, with folders inside it, and a UI built around
"my notebooks" rather than around navigating a filesystem. Nothing outside the vault is
browsable. This is the Notability/GoodNotes model and it's the only one that behaves
sanely on mobile.

### The useful surprise: this is already half-shipped

`docListSiloed` (`documentlist.cpp:28`) is exactly this restriction, and it **already
defaults to on for iOS** (`scribbleconfig.cpp:126`: `cfg["docListSiloed"] = PLATFORM_IOS;`).
It's enforced in the breadcrumb loop in `setCurrDir` — breadcrumbs stop being generated
once the parent path escapes `docRoot`:

```
if(docListSiloed && !StringRef(pathinfo.c_str()).startsWith(docRoot.c_str()))
  ok = false;
```

It's also exposed as a user pref (`res_ui.cpp:152`, `group="Document List"`,
`exclude="desktop,ios"`). So the containment concept exists and is tested in production on
one platform. You are not fighting the architecture — you're promoting an existing mobile
special case to the primary model.

### What's genuinely missing

The gap is not navigation containment. It's everything around it:

1. **A guaranteed, app-owned vault path.** `docRoot` is currently per-platform and
   *discovered* (`scribbleapp.cpp:52–99`) — Android probes `/sdcard/styluslabs/write/` and
   falls back to app storage on permission failure; **Windows falls back to `C:/`**, which
   is the clearest evidence that today's model is "browse the filesystem," not "own a
   store." The vault must be created on first run and guaranteed writable, with `C:/`-style
   fallbacks deleted.
2. **Import-on-open instead of open-in-place.** This is the real work. Today external files
   open from wherever they live: CLI `argDoc` (`scribbleapp.cpp:168`, `353`), Android
   intents, `openDocument(filename)` (`1640`), drag-and-drop (`1068`). In a vault model each
   of these must *copy into the vault first*, then open the copy. The primitive already
   exists — `DocumentList::copyDocument()` loads and re-saves through the `Document` class
   (with `SAVE_MULTIFILE`), so it normalizes format on import rather than blindly copying
   bytes. That's exactly the right behavior for an import path.
3. **Notebook identity.** Today a notebook is just a file, titled by its filename
   (`setCurrDir` derives the label from `fileinfo.baseName()`). A vault wants stable
   identity, display title independent of filename, cover/color, and sort order.
4. **Vault-shaped UI.** Breadcrumbs are a fixed two-button array (`breadCrumbs[0]`,
   `breadCrumbs[1]`) plus a `Drives` button — a filesystem affordance. A vault wants a
   root "Library" view, folders as cards, and no drive/path concepts at all. Thumbnails
   already work (`ScribbleDoc::extractThumbnail`, `showThumbnail` pref), which is the
   expensive part of a card-grid UI and it's done.

### Metadata: sidecar, not database

Keep the filesystem as the source of truth and store display metadata in a per-folder
sidecar inside the vault (title, color, cover, order). Rationale: cloud sync, external
edits, and mobile backup/restore all mutate the vault behind the app's back, and a central
index would silently desync. A sidecar degrades gracefully — worst case you lose a custom
title, not a notebook. If you later want tags and cross-folder search, add an index as a
*cache* that can always be rebuilt by scanning, never as the authority.

Note `DocumentList::isMultiFileDoc()` already exists, so folder-shaped documents are an
understood concept in this codebase — a "notebook = folder" design won't surprise it, but
sidecar files need naming that won't be mistaken for document parts.

### Caveats

- **Don't delete desktop file access.** On desktop, "open a .svgz from anywhere" is a
  reasonable thing to want. Vault-primary with an explicit "Import…" action covers both;
  the import just copies in.
- **Android storage is the perennial trap.** `hasAndroidPermission()` gating and the
  `styluslabs/write` vs. `Android/data` drive menu exist because of exactly this. A vault in
  app-private storage is permission-free and robust, but invisible to other apps and deleted
  on uninstall. A vault in shared storage is visible and survives uninstall, but needs
  permissions. Pick deliberately — it's a product decision, not a technical one.
- Existing users' documents live outside the new vault; a one-time migration prompt is
  needed. There's precedent: the `askConvertDocs` / `convertDocuments()` flow at the bottom
  of `setCurrDir` already does a one-time bulk conversion with a prompt.

**Verdict: Medium.** ~800–1200 LOC — higher than my first estimate, because the work is not
in the browser widget (which largely survives) but in rerouting every entry point that can
open a document, plus a new notebook metadata layer and a migration. `documentlist.cpp` is
1007 lines today and a vault UI rewrites a good fraction of it. Still fully independent of
the other features and parallelizable.

---

## 5. PDF support

### What exists

Nothing usable. `usvg/pdfwriter.cpp` is an export-only writer. `DocumentList::SAVE_PDF`,
`ScribbleApp::exportPDF()`, `writePDF()` are all output paths. There is no PDF parser,
no font machinery for reading, no content-stream interpreter.

Writing a PDF reader yourself is not realistic — it means xref tables and object streams,
encryption, content-stream interpretation, and font handling (Type1/CFF/TrueType/CID with
embedded subsets). This is a library decision, not an implementation decision.

### Library choice — the license settles it

The repo is **AGPL-3.0**, so:

- **MuPDF (AGPL-3.0)** — compatible, no commercial license needed. Pure C, builds with a
  plain makefile on Linux/Windows/macOS/iOS/Android/wasm, which matches this project's
  hand-rolled per-platform Makefiles. **Recommended.**
- **PDFium (BSD)** — also license-fine, but requires GN/depot_tools and is a very large
  dependency to graft onto this build system.
- **Poppler** — GPL-2-only components conflict with AGPL-3. Avoid.
- **OS-native renderers** — `CGPDF*` on iOS/macOS, `android.graphics.pdf.PdfRenderer` on
  Android. Zero binary-size cost on mobile, but then you maintain three backends plus one
  for Linux/Windows/wasm anyway. Only worth it if MuPDF's size becomes a shipping problem.

Cost either way: MuPDF plus its deps (freetype, harfbuzz, jbig2dec, openjpeg) adds roughly
10–20 MB to the binary. That matters for mobile and matters a lot for wasm.

### Integration model A — raster page background (recommended)

This is the GoodNotes/Notability model and it is what your school workflow actually needs.

Per PDF page: render to an `Image` at 2–3× display resolution, wrap it in an `SvgImage`,
insert it as the first child of `Page::contentNode` (or better, a dedicated background
layer alongside the existing `ruleNode`), and set `PageProperties` width/height from the
PDF MediaBox.

Why this is cheap: **`SvgImage` elements are already fully supported end to end.**
`insertImage` (`scribbleapp.cpp:2051`), crop mode (`MODE_CROPSEL`), erase-images
(`Element::ERASE_IMAGES`), SVG save/load, and PDF *export* (images are embedded by
`PdfWriter`) all work today. Ink, selection, undo, and sync need no changes at all.

Real costs:
- **File size.** Images are embedded in the `.svg`/`.svgz`; a 200-page scanned PDF becomes
  enormous. Mitigations: JPEG encoding, multi-file documents (one SVG per page — already
  supported, see `DocumentList::isMultiFileDoc`), or keeping a reference to the source PDF
  and rendering pages lazily. Lazy rendering fits the existing delayed-load machinery
  (`Page::loadStatus` / `NOT_LOADED`).
- Zooming past the rasterization resolution looks soft unless you re-render on zoom.

### Integration model B — true vector import

MuPDF can emit SVG per page, which you'd then feed to the existing usvg parser. Gives small
files, selectable/editable content, and real text. But usvg is a *subset* SVG renderer —
clip paths, blend modes, and filters are not comprehensively supported, and PDF text → SVG
text needs font embedding that usvg's `SvgFont` path may not handle for arbitrary embedded
subsets. Expect visual regressions on real-world PDFs. Ship it as an option, not the
default.

### On exporting back to "intact" PDF

Your instinct is right, with one correction. After import the content lives in Write's
model, and `PdfWriter` re-emits whatever is there:

- With model A, the page is an image forever — export produces a PDF with a picture of the
  original page plus your ink. Fine for handing in homework, useless for text search.
- With model B, if text survives as usvg text nodes, `PdfWriter` already has text and font
  support (`hasText`, `FONTS[]` in `pdfwriter.h`), so text *could* round-trip. That's the
  only path to a genuinely intact PDF, and it inherits all of model B's fidelity risk.

### Verdict

**Model A: Medium–Hard, 2–4 weeks**, and most of that is build-system work — wiring MuPDF
into six hand-maintained platform Makefiles is the bulk of the effort, not the C++ glue,
which is maybe 300–500 LOC behind a small `PdfImporter` interface.

**Model B: Hard, +4–8 weeks** with an open-ended fidelity tail.

Do A. It gets ~90% of the scanned-document workflow for a fraction of the risk, and it
leaves B available later as an "import as vector" checkbox.

---

## Recommended order

1. **Shape tool (drag-to-draw)** — easy, immediately useful, and it defines the shape types
   and parameters that features 2 and 1 both consume.
2. **Editable shapes** — builds directly on 1; finalize the parametric model here.
3. **PDF import, raster** — highest value per unit risk for the school workflow. Independent
   of 1/2, so it can also run in parallel.
4. **Library vault** — fully independent; slot in whenever. Note its cost is in rerouting
   document-open entry points, so doing it before PDF import means PDF import's own import
   path lands in an already-correct place.
5. **Shape recognition** — hardest, and it should emit the model from 1/2 rather than
   inventing its own. Doing it first would mean designing the shape representation twice.

## Open questions worth deciding before any code

- **Constant-width vs. pressure outline for shapes.** Constant-width `SvgPath` objects are
  clean and editable but visually distinct from pressure ink; pressure outlines match the
  ink look but are painful to edit. This propagates through features 1, 2, and 3 — decide
  once, first.
- **Arrow as a group or a single filled path.** Group keeps the shaft editable but requires
  extending `Element::isMultiStroke()`; single path is simpler but bakes in the width.
- **Where does the vault live on Android** — app-private (permission-free, invisible to
  other apps, deleted on uninstall) or shared storage (visible and durable, needs
  permissions)? Product decision, blocks the library work.
- **Is binary size a hard constraint?** Decides MuPDF-everywhere vs. OS-native PDF renderers
  on mobile.
