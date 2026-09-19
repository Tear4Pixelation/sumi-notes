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

`usvg` and `ugui` each have their own standalone example/test build (`cd usvg && make` → `Release/usvgtest`; `cd ugui && make` → `Release/uguitest`), buildable once `Write` itself has been built (they reuse its makefile setup for `nanovgXC`/SDL).
