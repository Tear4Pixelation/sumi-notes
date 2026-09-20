# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

**Write** is a cross-platform (Windows, Mac, Linux, iOS, Android, wasm) C++ application for handwritten notes, by Stylus Labs.

## Repository layout

The main application lives in `syncscribble/`. Everything else is a git submodule providing a layer of the stack:

- `ulib` - support library (Path2D, Transform2D, image I/O, threading, sockets, document-scan imaging)
- `usvg` - SVG library (parsing/writing/rendering, CSS support) used both as the document format and as the basis for the GUI
- `ugui` - SVG-based retained-mode GUI toolkit (`SvgGui`/`Widget`/`Window` in `svggui.cpp`; widgets in `widgets.cpp`, `textedit.cpp`, `colorwidgets.cpp`; default theme as SVG+CSS in `theme.cpp`)
- `nanovgXC` - GL rendering backend
- `pugixml`, `stb`, `miniz` - vendored XML, image, and compression libraries
- `SDL` - platform layer (uses branches `write-linux`/`write-mac`/`write-win`, not upstream master - see per-platform build steps below)
- `mupdf` - PDF reader used for PDF import; compiled inline rather than linked (see `syncscribble/mupdf.mk`)

Because everything is included via `../` relative paths in the Makefile, always run builds from `syncscribble/`, and clone with `git clone --recurse-submodules`.

`mupdf` needs extra care: it has ~17 submodules of its own and only five are built here, so clone
with mupdf skipped and init it separately (`make mupdf-submodules` from `syncscribble/` does both):

```
git clone --recurse-submodules -c submodule.mupdf.update=none https://github.com/styluslabs/Write
cd Write/syncscribble && make mupdf-submodules
```

## Architecture (syncscribble/)

- `scribbleapp.cpp`/`.h` - `ScribbleApp`, top-level application/event loop and command dispatch
- `application.cpp`/`.h` - `Application`, thin static wrapper around SDL window/GL context setup
- `document.h`, `page.h`, `element.h` - document model: a `Document` is a vector of `Page`s, each page is one SVG `<svg>` node (`SvgDocument`), and strokes/content are `Element`s built on top of `usvg` SVG nodes
- `scribbledoc.cpp`/`.h` - `ScribbleDoc`, owns a `Document` plus undo history, load/save, and command handling; central point most other classes are `friend`ed by
- `scribblearea.cpp`/`.h` - `ScribbleArea`, the drawing/view widget (subclass of ugui `Widget`) that renders pages and handles input
- `scribbleview.cpp`/`.h`, `scribblewidget.cpp`/`.h` - view/window composition around `ScribbleArea`
- `pdfimport.cpp`/`.h` - `PdfImport`, renders PDF pages via MuPDF into the background (rule) layer of new `Page`s; see "PDF import" below
- `scribblemode.cpp`/`.h`, `scribbleinput.cpp`/`.h`, `strokebuilder.cpp`/`.h` - pen/touch input handling and stroke construction
- `syncundo.cpp`/`.h` - `UndoHistory`, undo/redo and the change representation shared with sync
- `scribblesync.cpp`/`.h` - `ScribbleSync`, real-time document sync between clients (shared whiteboard) over a `syncServer`, built on `unet.h` and the undo history's change log
- `selection.cpp`/`.h`, `documentlist.cpp`/`.h`, `bookmarkview.cpp`/`.h`, `clippingview.cpp`/`.h`, `pentoolbar.cpp`/`.h`, `configdialog.cpp`/`.h`, `rulingdialog.cpp`/`.h`, `linkdialog.cpp`/`.h`, `syncdialog.cpp`/`.h`, `scandialog.cpp`/`.h`, `touchwidgets.cpp`/`.h` - selection tool, document browser/bookmarks, and the various dialogs/toolbars, all built as ugui `Widget`/`Dialog` trees
- `mainwindow.cpp`/`.h` - top-level window assembly and menu/toolbar wiring
- `resources.cpp`/`.h`, `res_ui.cpp` - embedded resources (icons, default theme SVG, strings)
- Per-platform subdirs (`android/`, `ios/`, `linux/`, `macos/`, `windows/`, `wasm/`) - platform glue code and packaging
- `scribbleres/` (sibling dir) - shared resources: fonts, icons, strings, platform Makefiles for SDL (`SDL-Makefile.*`)

Build/test entry point conventions: files under `SCRIBBLE_TEST` (`basics.h`) compile in `ScribbleTest` (`scribbletest/scribbletest.cpp`), which is `#include`d directly into `scribbleapp.cpp` for test builds rather than compiled as a separate translation unit.

## Build

Always build from `syncscribble/`. The Makefile auto-selects platform via `Makefile.unix`/`.mac`/`.msvc`/`.ios`/`.wasm`, all pulled in from the top-level `Makefile`.

- **Linux**: `cd syncscribble && make USE_SYSTEM_SDL=1` (needs `libsdl2-dev`); or build SDL from the vendored submodule first (`cd SDL && git switch write-linux && make -f ../scribbleres/SDL-Makefile.unix`) and `make` without `USE_SYSTEM_SDL`. Copy `scribbleres/fonts` into `syncscribble/Release` before running. Add `DEBUG=1` for a debug build (output goes to `Debug/` instead of `Release/`).
- **macOS**: `cd syncscribble && make MACOS=1`
- **Windows**: build SDL first (`cd SDL && git switch write-win && make -f ../scribbleres/SDL-Makefile.msvc`), then from an MSVC dev prompt, `cd syncscribble && make`. Requires GNU make on PATH and `DEPENDBASE` set in `syncscribble/Makefile`.
- **iOS**: build SDL (`cd SDL && git checkout write-mac && make -f ../scribbleres/SDL-Makefile.ios`), then `cd syncscribble && make` or use `xcode/Write`.
- **Android**: `cd syncscribble/android && ./gww installRelease` (`./gww --install-sdk` to bootstrap SDK/NDK first).

CI (`.github/workflows/write-ci.yml`) builds Linux (`make DEBUG=0 USE_SYSTEM_SDL=0 real_tgz`), Windows (`make DEBUG=0 zip`), and Android (`./gww assembleRelease`), building the `write-linux`/`write-win` SDL branches from source each time rather than using system SDL.

## PDF import (MuPDF)

`syncscribble/mupdf.mk` adds MuPDF's C sources straight to `SOURCES`, so they are compiled by
whichever platform makefile is already in use - the same treatment as nanovg/miniz/pugixml. This is
deliberate: building MuPDF's own makefile for six toolchains is where the cost of a PDF dependency
would otherwise go. It works only because MuPDF commits its *generated* sources (cmaps, ICC
profiles, base-14 font dumps) to git, so no host codegen step is needed.

- The build is trimmed to a PDF reader: no XPS/SVG/CBZ/EPUB/HTML handlers, no JS, barcodes, OCR,
  ICC (drops lcms2), JPEG 2000 (drops openjpeg) or brotli.
- Size: PDF support costs about **+2.2 MB stripped** (+1.2 MB gzipped) on x86-64 Linux, i.e. the
  binary goes from ~2.0 MB to ~4.2 MB. Three things get it there from the ~+5 MB a naive build
  costs, and all three are worth preserving:
  - `-ffunction-sections -fdata-sections` + `--gc-sections` (`-dead_strip` on Apple, `/Gw` +
    MSVC's existing `/OPT:REF`; ndk-build already does this). Only a sliver of MuPDF is reachable
    from `pdfimport.cpp`, but it arrives as ~310 objects containing a PDF *writer*, text
    extraction, subsetting, signatures, forms and a dozen output formats. Worth ~1.4 MB. These
    flags live at the *end* of `syncscribble/Makefile` because `Makefile.mac/.ios/.wasm/.msvc`
    assign `CFLAGS` with `=`, which would discard anything set before the platform include.
  - `-Os` for MuPDF's objects only, via a target-specific `CFLAGS` override next to those flags.
    MuPDF runs once per page at import, never in the drawing or input path. Worth ~0.55 MB.
  - `NO_CJK` (see below). Worth ~0.85 MB.
  - Considered and declined: the base-14 URW font data is ~0.58 MB. `TOFU_BASE14` would drop it, but
    then PDFs that reference Helvetica/Times/Courier *without embedding them* need a substitute
    installed via `fz_install_load_system_font_funcs` (Write already bundles Roboto and
    DroidSansFallback). Glyph shapes would change; positions would not, since non-embedded base-14
    text is positioned from the PDF's `Widths` array or MuPDF's built-in metrics tables. Kept for
    fidelity on arbitrary PDFs - revisit only if binary size becomes a hard constraint.
- Only the base-14 fonts are bundled - the Noto/CJK/SIL dumps are *not* in git, so the `TOFU*`
  defines are mandatory, not optional. A PDF relying on non-embedded CJK fonts will not render them.
- `NO_CJK` (undocumented; see `source/pdf/pdf-cmap-load.c`) additionally drops the ~60 predefined
  CJK CMap tables, 0.85 MB of pure data. `Identity-H`/`Identity-V` - what essentially every modern
  PDF with an embedded CJK font uses - still work; only *predefined* encodings like `UniGB-UCS2-H`
  are lost. This pairs with `TOFU_CJK`: with no CJK font data bundled, non-embedded CJK is already
  unrenderable, so the CMaps could only have helped embedded fonts using a predefined encoding.
  Drop both defines together if CJK PDFs ever matter.
- `mujs` is required even with `FZ_ENABLE_JS=0`: fitz compiles its regexp engine in for text search.
- `syncscribble/mupdf-stubs/openjpeg.h` exists because upstream `encode-jpx.c` includes
  `<openjpeg.h>` outside its own `#if FZ_ENABLE_JPX` guard.
- zlib (from mupdf) and miniz coexist safely: miniz only ever defines `mz_*` symbols, its
  zlib-compatible names are macros, and no translation unit includes both headers.
- Build without it via `make PDF_IMPORT=0`; `pdfimport.cpp` still compiles and
  `PdfImport::isAvailable()` returns false, so callers need no `#ifdef`s.

On the app side, `PdfImport::importPdf()` renders each page and puts the image in the page's *rule*
(background) layer with `write-no-dup` and without `write-std-ruling` - the same convention as
`scribbleres/pdf2write.sh`. That keeps the image unselectable/unerasable and makes
`Page::loadSVG` treat the ruling as custom so it survives save/load. `ScribbleApp::doImportPdf()`
writes a `.svgz` next to the PDF and then opens it like any other document, which keeps undo, views
and sync entirely out of the import path. `.pdf` paths are routed to the importer from
`doOpenDocument()` (doc list, drag-drop, Android intents) and from the command-line `argDoc` branch.
Note the import temporarily forces `SvgWriter::DEFAULT_SAVE_IMAGE_SCALED = 0`, otherwise
`savePicScaled` resamples the pages back down to 150 DPI and discards the chosen `pdfImportDPI`.

## Document scanning

Photo of a page → adjust four corners → flattened, cleaned-up image, inserted either as an element or
as a new page. Implemented from scratch in C++ rather than via a dependency: ML Kit requires Google
Play Services (ruled out - Write must run on de-Googled devices, and it would be Android-only anyway),
and OpenCV would cost ~1.5-3 MB per ABI plus a CMake build to wire into six toolchains, to use about
five functions. Writing it in `ulib` also means it works identically on Android, iOS, desktop and wasm.

Imaging lives in `ulib` (no dependency on `Painter`/nanovg, so it is unit testable on its own), UI in
`syncscribble/scandialog.cpp`:

- `homography.h`/`.cpp` - `Transform3D`, a 3x3 projective transform. `Transform2D` stays affine (6
  elements) since that is all the renderer needs; perspective correction cannot reuse it. Square-to-quad
  uses Heckbert's closed form, so no general linear solver is needed.
- `imagewarp.h`/`.cpp` - inverse-mapped bilinear warp, rows tiled across `ThreadPool`. **The pool is not
  optional**: 4000x3000 → 2200x1509 is 478 ms single-threaded vs 84 ms threaded. Box-supersamples when
  downscaling (grid from the source/dest area ratio, capped 4x4), or fine text aliases badly. CPU rather
  than GL because nanovg paints are affine, so a GL path would need a custom shader plus FBO readback.
- `imageenhance.h`/`.cpp` - what makes a photo read as a scan is even lighting, not resolution, so the
  core step divides the image by a heavily blurred copy of itself (the blurred copy *is* the
  illumination). A summed-area table makes the blur radius free - 183 px costs the same as 5 px. Then a
  percentile level stretch, and for `SCAN_MONO` a local threshold. 72-119 ms at 2200x1509, unthreaded.
- `quaddetect.h`/`.cpp` - finds the page outline: downscale, Sobel, gradient-directed Hough, take the
  *outermost* line of each near-horizontal/near-vertical pair, intersect. 19 ms and ~11 px accuracy on a
  4000x3000 photo. Two non-obvious parts, both load bearing:
  - The edge threshold needs a **floor as well as a percentile**. A page outline is only ~1% of the
    pixels, so "keep the top 8%" reaches into the flat interior, and flat pixels have gradient (0,0)
    whose `atan2` is 0 - sending the whole background to one orientation bin and burying the real edges.
  - The Hough angle alone is not accurate enough: half a degree across a 3500 px edge is ~160 px of
    corner error. Each of the four lines is refit by total least squares over its supporting pixels,
    **iterated three times** - one pass only sees pixels inside its own window and is biased toward the
    middle of the edge. This is what takes worst-corner error from 160 px to 11 px.

Capture needs no new platform code and **no Play Services and no CAMERA permission**: `scanDocument()`
reuses `insertImage()`'s picker, which on Android is already a chooser merging every
`MediaStore.ACTION_IMAGE_CAPTURE` activity with the gallery (`MainActivity.getImage()`), delegating the
actual capture to whatever camera app is installed. A `pendingScan` flag on `ScribbleApp` diverts the
picked photo into `ScanDialog`. Note that flag must be checked in **two** places: Android and iOS post
an `INSERT_IMAGE` event, but the desktop picker returns a filename and calls `insertImage(filename)`
directly, never touching that event - miss it and desktop silently inserts the raw photo. `insertImage()`
clears the flag so a cancelled picker cannot hijack a later plain image insert.

Output is either an element (`ScribbleArea::insertImage()`, unchanged) or a page background. The page
path follows the same convention as PDF import - image in `ruleNode`, `write-std-ruling` removed,
`write-no-dup` added, `isCustomRuling = true` - but goes through `ScribbleDoc::insertPage()` rather than
writing a file, so it gets undo and sync. Page size comes from the *current page's width* with the
height following the scan's aspect ratio: a scan has no physical scale (`UNITS_PER_POINT` is a PDF
points conversion and is meaningless here), and matching the document keeps scrolling and zoom consistent.

Known gap: a scan of ruled paper leaves `yRuling == 0`, so `Page::yruling(true)` falls back to
`BLANK_Y_RULING` and the ruled tools (insert space ruled, select ruled, ruled eraser) snap to a spacing
unrelated to the lines in the image. The detector's Hough pass already has the data to fix this.

## Shapes

Parametric shapes - line, box, ellipse, polyline and curve - drawn with `MODE_DRAWSHAPE` and
edited with handles. Designed in `SHAPES_SPEC.md`; that document is the rationale, this is the summary.

**Five tools, three toggles.** Arrowheads and corner rounding are flags, not shapes: an arrow is a line
with `SHAPEFLAG_HEADEND`, a rounded box is a box with `SHAPEFLAG_ROUNDED`. Each toggle collapses what
would otherwise be a pair of near-identical tools, and they compose - a rounded polyline with a head
needs no third entry. `ShapeDef::allowsHeads`/`allowsRounding` say whether a toggle means anything for
the active shape; the options row disables it otherwise.

**The parameters are the document; the `Path2D` is a cache.** Every shape is a plain `SvgPath` whose
`Element` carries a `ShapeParams` descriptor, serialized as `__shape`/`__shapepts`/`__shaperx`/`__shapery`/
`__shapeflags` - the same custom-attribute convention `Element` already uses for `__comx`/`__timestamp`.
`Element::updateFromNode()` parses it and regenerates the path; `Element::setShapeParams()` is the only
way to change a shape, and it regenerates too. Nothing edits a shape's path directly.

- `shape.h`/`.cpp` (`ulib`-style, no app dependencies) hold the `ShapeDef` registry. Adding a shape is one
  table entry plus a `buildPath` and a `getHandles` function; the mode handlers, selector, serializer and
  undo item are written once against `ShapeDef`.
- **Plain `SvgPath`, never `SvgRect`.** `Element::isPathElement()` tests `type() == PATH`, and `SvgRect`
  returns `RECT`, which would silently fail a long list of guards (selection highlight, free erase, hit
  testing, width scaling). Widening that test would change behavior for every pre-existing `<rect>` in
  every document. The cost is ~30 lines of rounded-rect construction copied from `SvgRect::updatePath()`.
- **Degradation contract.** Any operation that cannot be expressed in the descriptor calls
  `Element::dropShape()` and leaves a plain path behind - currently only rotation of a box/rbox/ellipse.
  Without this a shape silently reverts to its pre-operation geometry the next time a parameter changes.
- **`applyTransform()` has a shape branch ahead of the path branch.** The generic non-rotating-scale branch
  mutates `m_path` in place and would leave `__shapepts` describing the old size; the shape would look
  right until the next parameter change regenerated it from the stale descriptor and snapped back. The
  shape branch maps the descriptor points and scales `rx`/`ry` instead. This is the failure the
  `shapeRoundTripTest()` check exists to catch.
- **`ShapeChangedItem` is a sync wire-format change, not just an undo item.** Undo items *are* the sync
  protocol, so it needs `serialize()`, a type constant (`SHAPE_CHANGE_ITEM`) and a matching `shapechanged`
  branch in `ScribbleSync::processItem()`. The `__shape*` attributes themselves ride along free inside
  `<addstroke>`, because sync round-trips nodes through `SvgWriter`/`SvgParser`.
- Shapes are excluded from `groupStrokes()`, which would otherwise rewrite their centre of mass as if they
  were handwriting, outside the undo system.
- `MODE_DRAWSHAPE` is appended at the end of the `scribblemode.h` enum (values are serialized to config);
  `shapeId` and `shapeFlags` are appended *after the pens* in `ScribbleMode::saveModes()` so older config
  strings still load. `shapeId` is stored as its **string** id, not its index - shapes have twice been
  folded into a parameter of another shape, and an index would have silently reassigned everyone's
  active tool each time that happened. `ShapeId`'s numeric order is therefore free.
- `shapeIdByStringId()` accepts the ids of shapes that have since been folded into a parameter -
  `arrow`, `rbox`, `rpolyline` became flags, `splinepoly` became tightness 1 - and returns the base
  shape plus the flags (and tightness) the old name implied, so documents written while those were
  still shapes keep loading as what they meant. They are rewritten in canonical form on the next save.
  This has happened twice, which is why the alias table is worth keeping rather than a one-off.
- Multi-point (polyline) is the only gesture that outlives one press/move/release cycle. The in-progress
  element lives on `ScribbleArea::shapeInProgress` and `finishShape()` is called from every escape route:
  cancel, tool switch (`ScribbleApp::setMode`), page change, and `reset()` (document close/save).

### The polyline family

Two shapes share their points, their handles and their arrowheads, and differ only in how the body
between the points is drawn:

| shape | body | overshoots? | passes through the points? |
|---|---|---|---|
| `polyline` | straight segments, filleted when `SHAPEFLAG_ROUNDED` | no | yes |
| `fitpoly` ("Curve") | relaxed towards B-spline knots, then smoothed | only at tightness 1 | only at tightness 1 |

**There was briefly a third, `splinepoly`, and deleting it is the point.** It was the interpolating
curve - Catmull-Rom straight through every point - and `fitpoly` at `tightness == 1` was measured to be
*bit-identical* to it: same point count, zero deviation, on open, closed, uneven-spacing, arrowheaded
and many-point cases. So the two tools were one tool at its two extremes, and asking the user to choose
between them up front was asking them to guess. `splinepoly` is now an alias that loads as a curve at
tightness 1; the choice is a handle drag on the shape instead.

- Rounding reuses `__shaperx` and adds a radius handle on the first interior corner. `rx` is the
  distance trimmed off each edge, the same thing it means for a rounded box; `filletTrim()` clamps it
  per corner to half the shorter adjacent segment, so neighbouring fillets can never overlap. `rx` is
  kept when rounding is switched off, so toggling it back on returns the radius you had.
- `addSplineBody()` uses **centripetal**, not uniform, Catmull-Rom. The two are the same curve on evenly
  spaced points, so this is invisible on a neat rectangle; it pays off on uneven spacing, which is what
  tapped-out points look like. Measured: a 4-unit segment between neighbours of 100 and 113 overshoots
  by 10.0 units under uniform and 0.6 under centripetal. `shapetest.cpp` pins this with that exact case
  - an evenly spaced fixture would not catch a regression to uniform. It does *not* remove the bulge at
  a sharp corner: passing smoothly through a right angle requires going outside it. That bulge is what
  lowering tightness buys you, and is why the knob is worth having at all.
- The curve is built by *relaxing* each interior point towards the uniform B-spline knot
  `(P[i-1] + 4P[i] + P[i+1])/6` and then running `addSplineBody()` through the relaxed points. That
  gives one knob, `ShapeParams::tightness`: 0 is the pure B-spline (roundest, follows the points
  least), 1 relaxes nothing and so *is* the interpolating curve, and in between trades roundness for
  fidelity. Building it this way also means it inherits centripetal parameterisation rather than
  repeating it. The two end points are never relaxed, so the ends stay put and the heads stay attached.
- **Tightness has a handle**, not just a preference - `curveHandles()` puts it on the point with the
  most slack (`fitLoosestPoint()`), sliding along knot -> point, so dropping it on the point gives 1
  and on the knot gives 0. Without it, changing one curve's character would mean a trip to
  preferences, which is what made two tools look necessary in the first place.
- Tightness is **per shape** (`__shapetight`), not a global setting, or the same document would render
  differently on another machine. The `shapeCurveTightness` preference is only the default for newly
  drawn curves, exactly as `shapeCornerRadius` is for `rx`.
- Open ends: `addSplineBody()` reflects a phantom point (`2*p0 - p1`), and the B-spline relaxation
  leaves the end points alone. Both make the curve's end tangent come out exactly along the first/last segment, which
  is what the arrowheads are oriented from, so a head can never sit skew to its own curve.

### The option toggles

`SHAPEFLAG_HEADSTART`/`HEADEND`/`ROUNDED` live in `ScribbleMode::shapeFlags` and are driven by three
toggles on the shape options row. Two behaviours worth knowing:

- If a single shape is selected, the toggles edit **it** (via `ShapeChangedItem`, so undo and sync
  follow) instead of only arming the next shape drawn - adding an arrow to an existing polyline does
  not mean redrawing it. `setSelShapeOptions()` masks by the shape's own `allowsHeads`/`allowsRounding`,
  so switching tools with a shape still selected cannot strip flags that shape legitimately carries.
- The head icons are reicon `arrow-left`/`arrow-right`, i.e. the same two files already shipping as
  `ic_menu_back`/`ic_menu_forward`.

### After drawing

With `shapeEditAfterDraw` on (the default), `ScribbleArea::editShapeAfterDraw()` leaves a newly
committed shape selected with its `ShapeSelector` handles up, so the parameters can be adjusted while
the shape is still the thing being thought about. It falls back to a `RectSelector` if the shape was
demoted on the way in.

**Only the paths where the *user* finished the shape may select it.** `finishShape(select)` defaults to
`false`; only the two deliberate multi-point finishes (tapping the last point, tapping the first to
close) pass `true`. Every other caller - `doPressEvent`'s guard when another gesture starts, a tool
switch, a page change - is finishing the shape as a side effect and must not leave a selection behind.
The reason is a sharp edge in `doPressEvent`: its mode switch assigns `currSelection = new Selection(...)`
**without deleting what was there**, which is safe only because the earlier "clear selection depending on
mode" pass already ran. Anything that installs a selection between those two points gets leaked, and the
elements it held keep `m_selection` pointing at the orphan - so they render as selected, are absent from
`currSelection`, and cannot be deselected, moved or deleted. `shapeInterruptTest()` pins this by
asserting no element points at a selection other than the current one; it was verified to fail against
the broken version, not just to pass against the fixed one.

Relatedly, `doPressEvent` drops any live `currStroke` at entry. The in-progress element is held on
`ScribbleArea` rather than in the page, so a gesture abandoned without a release or cancel - the input
layer restarts one when the pen button changes mid-stroke - would otherwise leave a shape painted every
frame that belongs to no page.

Two deliberate deviations from the spec, both documented at the point of deviation:

- **Arrowheads are stroked open Vs in the same path, not filled paths inside a `<g>`.** The spec's reason
  for the group was that a filled outline would bake the stroke width into the geometry - which cannot
  happen here, because the path is regenerated from the descriptor on every change. Keeping an arrow as
  one `SvgPath` means it never becomes a multi-stroke element and carries no filled part that
  `Element::scaleWidth()` could misread as pen geometry.
- **Regeneration is driven by `setShapeParams()`, not by `SvgNodeExtension::onAttrChange()`.** The
  descriptor is held in the `Element` and written to attributes only on serialize, so a handle drag does
  not round-trip through strings on every move event.

Known gap: shapes carry no pen class, so `Element::toPenPoints()` returns nothing for them and the **free
eraser does not affect a shape** - the stroke eraser deletes it whole. Giving them `STROKE_PEN_CLASS`
would make free erase work but would split curved shapes (ellipse, rounded box) at Bezier control points.

## Switch back (single-use tools)

Picking a tool uses it once and returns to the previous one; double tapping the tool locks it. The
eraser, the selection tool and insert space each have a **Switch Back** toggle on their options row
(`ScribbleMode::eraseSwitchBack`/`selectSwitchBack`/`insSpaceSwitchBack`, `setSwitchBack()`), so any of
them can be made to stick without touching a preference. `hasSwitchBack()` is the one place that says
which tools have a toggle. `doubleTapSticky` remains the master switch - with it off nothing switches
back and the toggles are disabled, since they would otherwise silently do nothing.

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

## History panel

The toolbar's **History** button (`ic_menu_history`, formerly the undo arrow) opens a panel of ticks -
one per undo step - that scroll under a fixed mark at the centre. `ButtonDragTimeline` in
`touchwidgets.cpp` replaces the older `ButtonDragDial`, which was the same interaction on a circle. A
circle has no ends, so "where am I" and "how much further can I go" could only be hinted at, by a wedge
that grew when you over-turned it; the ends of a line are simply where the ticks stop.

The button still behaves like an undo button on a plain tap (`onStep(-1)`), and `Ctrl+Z` plus the menu
entries are untouched - which is what makes the rename cost recognition rather than capability.

- **Drag right undoes, left redoes** - the opposite of "right is the future", and deliberate. With the
  mark pinned at the centre, what is under the finger is the tape, not the playhead, so dragging the tape
  right must bring earlier ticks to the centre. The other sign slides the ticks against the finger
  pushing them.
- **Event coords and layout units are the same space** (the framebuffer is scaled as a whole for DPI), so
  one tick of drag is one tick of ruler with no conversion. A screenshot will disagree - the capture is
  downscaled from the app's larger framebuffer - so measure this from a trace, not from pixels.
- **Two lanes, chosen by where the drag is, not by which button.** A stylus has one input and cannot
  express "the other button", and one panel showing both features explains them once, where two separate
  controls would each need their own explanation. Steps **only fire once the pointer is inside the
  panel**: the press starts on the button above it, so every gesture travels down into a lane, and that
  travel is never purely vertical - acting on its sideways component would lock the gesture into whichever
  lane it happened to be crossing. The first step that lands locks the lane, since scrubbing sideways
  always drifts vertically too. Right click (or long press) still goes straight to the select lane,
  locked, for anyone who already has it in their fingers.
- **Panel width is `2*max(back, fwd) + 1` ticks**, clamped to `TIMELINE_MIN/MAX_TICKS`, fixed for the
  whole gesture. The ruler is centred on the current position, so sizing it to `back + fwd + 1` pushes the
  end cap off the edge exactly when the history is lopsided - which is most of the time. The cost is that
  a lopsided history looks off-centre at rest.
- **The select lane's range is derived, not measured.** It walks the same history backwards, so it reaches
  at most `undoSteps()` - an upper bound, since a step that added no strokes is skipped rather than
  counted - and forward nothing, as nothing is selected at press. Without this it drew an open ruler both
  ways, promising history that wasn't there. Being refused pins the true end (`applySteps`), which is also
  how any range left at -1 is discovered.
- **Holding against the window edge keeps stepping** on a timer. A ruler runs out of screen where a dial
  did not, so whichever direction points at the near edge has little room. This is why the button sits at
  the end of the *tools* row rather than in the file ops panel: there it was at the right edge, where the
  rightward drag - *undo*, the common case - had almost no travel. It is still needed, since a vertical
  toolbar cramps redo instead, and pointer capture would fix only the mouse. The timer must be torn down
  on release and on a fresh press, or it keeps undoing after the finger is gone.
- The button is a verb, not a tool, and never shows the tools' checked state; the separator before it is
  what keeps it from reading as an eighth mode. This is the layout on *every* platform: `vertToolbar`
  (`mainwindow.h`) is initialised false and never assigned, so the `tbcfg`-driven vertical toolbar beside
  it is dead code, and phones and tablets get these same floating panels. Narrow screens are handled by
  `AutoAdjContainer` hiding widgets in `ui-priority` order instead.
- **`undoRedoBtn` is priority 5, above the tools' 4**, so the tools are dropped *first*. At phone density
  (preview with `--screenDPI=522`, see below) that leaves the Editing panel holding History and nothing
  else, with all seven tools in the overflow menu. The priority predates the move - it made more sense
  when the button lived in the file ops panel - but the result now looks like a bug.

To preview another device's layout, override the config from the command line: `screenDPI` sets
`paintScale = dpi/150`, so a higher value shrinks the window's logical width exactly as a denser screen
does, and `agent-display.sh run` passes extra args through to the app:

```
tools/agent-display.sh run --debug -- --screenDPI=141   # ~iPad landscape
tools/agent-display.sh run --debug -- --screenDPI=202   # ~iPad portrait
tools/agent-display.sh run --debug -- --screenDPI=522   # ~phone
```

The headless output stays 1280x720, so this reproduces width-driven layout, not portrait aspect.
- **`UndoHistory::undoSteps()/redoSteps()`** count `HEADER` items either side of `pos`: one step is one
  action, so this is not derivable from `hist.size()` and `pos`.
- **Colors and weights are hardcoded, and must stay theme-neutral.** The panel is custom-drawn onto an
  `SvgCustomNode`, so there is no theme color to read: ticks and idle labels are one mid-gray, and the
  ends are set apart by weight and height rather than brightness, since anything tuned to the dark theme
  recedes on the light one. An earlier attempt to highlight the armed lane with a translucent filled rect
  rendered far brighter than its alpha suggested and swallowed the label on it; the armed lane is named
  in bold and in its own color instead.
- The first-run hint is retired **by use** - the first step that actually lands - not by dismissal, so it
  cannot be waved away without being understood, and dragging against a dead end does not count. Stored
  as `historyHintDone` in `ScribbleConfig`.

## Tests

Test support is compiled in via the `SCRIBBLE_TEST` define (already 1 by default in `basics.h`). The binary is invoked with a `--test` flag; the GL renderer must be enabled (`--glRender=0 --test` is *not* runnable - GL rendering is required):

```
./Debug/Write --test          # ScribbleApp::runTest("test") -> ScribbleTest::runAll()
```

Other in-app test modes dispatched the same way (see `ScribbleApp::runTest` in `scribbleapp.cpp`): `synctest` (sync between two instances), `perftest`, `inputtest`. Test fixtures/reference output live in `scribbletest/` (`testN_in.html` inputs, `testN_ref.html` expected results). Because tests require a real GL-capable display, CI does not run them (see the commented-out step in `write-ci.yml`).

The document-scan math is tested in `scribbletest/scantest.cpp`. Unlike the rest of `scribbletest` it
needs neither GL nor a document, so it also builds and runs standalone - see the command in that file's
header (it needs `-DNDEBUG`, or `geom.cpp`'s `ASSERT` pulls in `platform_assert` from the application).
`ScribbleTest::runAll()` calls `runScanTests()` and counts its failures in the result string and exit
code. The dialog itself has no automated coverage.

The shape math is tested the same way in `scribbletest/shapetest.cpp` (`runShapeTests()`), which likewise
builds and runs standalone - see the command in that file's header. On top of it,
two checks that do need the application, both running after the `testN` loop and reporting through the
unit-check count, so neither needs a reference file:

- `ScribbleTest::shapeRoundTripTest()` - the spec's step-1 gate. Drives input events to draw a box,
  saves, reloads, resizes and then changes a parameter, checking the shape does not snap back to its
  pre-resize size.
- `ScribbleTest::shapeInterruptTest()` - interrupting a shape gesture (pen button down mid-drag, or a
  right-click between polyline taps) must leave neither an orphaned live element nor an element pointing
  at an orphaned `Selection`. Both failure modes look the same to the user: a shape that is drawn but
  belongs to nothing, so it cannot be selected, erased or deleted. It also pins the *positive* half:
  a shape the user deliberately finished must come back selected with handles, since the fix for the
  orphan bug made `finishShape()` default to not selecting and could otherwise silence those paths too.

When adding a check here, confirm it **fails against the broken code**, not just that it passes against
the fixed code - two assertions in this area have been written that were true either way and so tested
nothing.

`usvg` and `ugui` each have their own standalone example/test build (`cd usvg && make` → `Release/usvgtest`; `cd ugui && make` → `Release/uguitest`), buildable once `Write` itself has been built (they reuse its makefile setup for `nanovgXC`/SDL).

## Agent display

Write is a GUI app, and an agent launching it puts a window on the user's own
session and steals focus. **Never run `Release/Write` or `Debug/Write` directly.**
Everything goes through `tools/agent-display.sh`, which runs it inside a nested
`cage` compositor - headless by default, so it is invisible and cannot take focus.
A `PreToolUse` hook (`.claude/hooks/guard-agent-display.py`) denies direct launches
rather than relying on this paragraph being read.

```
tools/agent-display.sh run [--windowed] [--debug]   # start session + app
tools/agent-display.sh shot [out.png]               # grim capture, on demand
tools/agent-display.sh click X Y [left|right|middle]
tools/agent-display.sh move X Y | scroll DY [DX]
tools/agent-display.sh drag X1 Y1 X2 Y2
tools/agent-display.sh stroke X1 Y1 X2 Y2 [X3 Y3 ...]
tools/agent-display.sh type "text" | key ctrl+z
tools/agent-display.sh record start|stop [out.mp4]
tools/agent-display.sh test                         # ./Debug/Write --test
tools/agent-display.sh status | stop
```

Interaction is **blind and scripted**: act, then screenshot only when you need to
see the result. There is no continuous stream, by design - `record` exists for
capturing a specific interaction to show the user, not for watching.

Setup is `tools/install-agent-display.sh` (needs sudo). Verified working
end to end: stroke, toolbar click, tool switch and Ctrl+Z undo.

### Two displays, and which one each command uses

cage provides both a Wayland socket **and** its own Xwayland server, and the
split is not cosmetic:

| | display | used for |
|---|---|---|
| pointer, screenshots, video | `WAYLAND_DISPLAY` (cage's socket) | `agent-pointer`, `grim`, `wf-recorder` |
| keyboard | `DISPLAY` (cage's Xwayland, e.g. `:2`) | `xdotool` |

Keyboard goes through X because **Write runs as an X11 client inside cage**:
forcing `SDL_VIDEODRIVER=wayland` segfaults it (verified, exit 139), so SDL falls
back to x11. `wtype` speaks the right Wayland protocol and works on native
Wayland clients in the same cage (verified against `foot`), but its
virtual-keyboard keymap does not survive the Xwayland translation, so it silently
does nothing to Write. Pointer events do survive that translation, which is why
the two halves use different mechanisms.

Both displays are private to the nested session, so neither can reach the user's
niri session. `in_session_x()` refuses to run if the recorded X display is empty
or implausible, because falling back to the inherited `DISPLAY` would type into
the user's real windows - the one failure this whole setup exists to prevent.
For the same reason, never substitute `ydotool`/`dotool`: they inject through
kernel `uinput`, which is seat-global, and a headless cage
(`WLR_LIBINPUT_NO_DEVICES=1`) would never see them while niri would.

### Things that bite

- **A click must be one invocation.** `agent-pointer` creates a virtual pointer
  and destroys it on exit, and the cursor position does not survive that, so a
  separate `move` then `click` clicks wherever the cursor defaulted to. The
  symptom is confusing: canvas drags work (one invocation) while toolbar buttons
  appear to ignore clicks entirely. Hence `click X Y`, never `move` then `click`.
- **`wlrctl` cannot express a drag.** It offers only relative motion and an
  atomic click, so there is no way to hold a button down - and in a drawing app
  every stroke is a press, a path, and a release. That is why
  `tools/agent-pointer/` exists instead; it also does absolute motion, so callers
  pass coordinates read straight off a screenshot.
- **The document list opens over the canvas on a fresh start.** Strokes sent
  before dismissing it go to the overlay and nothing appears to happen. Close it
  (the X at the top right) before drawing.
- **Killing cage does not kill Write.** A Write that outlives its compositor
  keeps running headless forever, holding its document. `stop` reaps it by exact
  process name - `-x Write`, never `pkill -f .../Release/Write`, which would also
  match the command line of the shell doing the killing.
- The headless output is 1280x720 (wlroots' default); cage exposes no way to
  change it. Use headless `sway` if a specific size matters.
- `--test` works here: the headless backend renders on the GPU via
  `/dev/dri/renderD128`, so it is a real GL run, not llvmpipe. Result as of this
  setup: **0 failed tests, 0 failed unit checks, 16 of 17 failed thumbnails.**
  Read that exit code carefully - `ScribbleTest` exits with the *failed thumbnail
  count*, so a clean run of the checks that matter still exits non-zero. The
  thumbnail comparison renders pixels and differs across GPUs and drivers;
  `scribbletest.cpp:470` says as much ("we've had so many problems with
  thumbnails"), and 16 of 17 failing uniformly is that, not 16 regressions - the
  document content matched for every fixture, only the embedded thumbnail image
  differed.
- **Leak detection is off for `test`.** ASan reports ~2.2 MB leaked entirely
  inside NVIDIA's GL driver and libdbus, and worse, its exitcode (1) masks the
  thumbnail count. `AGENT_DISPLAY_ASAN_LEAKS=1` restores it.
