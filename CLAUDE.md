# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

**Kaku** (store name *Kaku Ink*, kakuink.app) is a cross-platform (Windows, Mac, Linux, iOS, Android, wasm) C++ application for handwritten notes, forked from **Write** by Stylus Labs (AGPL-3.0).

### The rename (Write -> Kaku)

Only what a user sees says Kaku: window/app titles, dialogs, the About box (which must keep crediting
Stylus Labs and naming the AGPL - section 5 requires marking a modified version), the executable
(`TARGET = Kaku`, so `Debug/Kaku`, `Kaku.app`, `Kaku.exe`, `Kaku.html`), plists, the Android label, the
installers and `scribbleres/linux/Kaku.desktop`. The docs site is `kaku-docs/`. Translations are keyed by
English text, so a renamed `_()` string must change in `scribbleres/strings/strings.xml` too, followed by
`make res_strings.cpp` in `scribbleres/`.

**Deliberately still "Write"** - each is identity or storage, and renaming it strands data or breaks upgrades:
the config file (`~/.config/styluslabs/write.xml` and the Windows/Android equivalents), the `.write-library`
marker, every `write-*` class and `text/writeconfig` inside documents, the Android package
`com.styluslabs.writeqt` (and its Java folder) plus the app-private fallback library `files/Write/`, the
`"styluslabs"` salt in sync password hashing (protocol), the `write-*` SDL branches, `xcode/Write`, and the
WiX component ids / `Software\Stylus Labs\Write` registry key. The default *new* library is `Documents/Kaku/`;
existing installs keep theirs because `libraryPath` is saved. Still pointing at Stylus Labs and needing a
decision: the update check (`styluslabs.com/write/versions.xml`), Help and share URLs, the Play Store review
link, the iOS IAP, the Windows installer's `Manufacturer`/`UpgradeCode`, and CI cloning upstream.

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
- **Web (wasm)**: needs emsdk (`~/emsdk`, `source ~/emsdk/emsdk_env.sh` in bash - the Makefile picks the wasm
  branch from `$EMSDK`). SDL is the `write-wasm` branch, kept in its own worktree so `SDL/` stays on
  `write-linux`: `git -C SDL worktree add ../SDL-wasm origin/write-wasm`, then in `SDL-wasm/build`
  `emconfigure ../configure --host=wasm32-unknown-emscripten --disable-assembly --disable-threads
  --disable-cpuinfo --enable-shared=no`, delete `-Wdeclaration-after-statement -Werror=declaration-after-statement`
  from the generated `Makefile` (current clang rejects SDL 2.0.x's code under it), `emmake make`. The worktree
  also carries a local patch to `SDL_emscriptenframebuffer.c`: `Module.createContext` no longer exists in
  Emscripten, so it calls `canvas.getContext('2d')` itself - without it the page loads and then throws.
  Then `cd syncscribble && make SDL_DIR=../SDL-wasm` → `EmRelease/Kaku.{html,js,wasm}`, and
  `python3 wasm/serve.py` serves it to the LAN (no-cache, or Safari keeps running a stale `.wasm`).
  The page is `wasm/shell.html`. The web build always renders in **software** (nanovg_sw, blitted through an
  SDL surface), hence `NO_PAINTER_GL`. Documents live in Emscripten's in-memory FS - **nothing persists across
  a reload**, and Save downloads the file. Pen input is Pointer Events (`wasm/wasmhelper.c`); Chromium was
  verified to draw a pressure stroke, WebKit/iPad Safari has not been tested from here (Playwright's WebKit
  needs Ubuntu libraries this Arch machine lacks).

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
writes a `.svgz` into the document library (next to the PDF when there is no library, or to `tempPath`
for a `--out` conversion) and then opens it like any other document, which keeps undo, views
and sync entirely out of the import path. `.pdf` paths are routed to the importer from
`doOpenDocument()` (doc list, drag-drop, Android intents) and from the command-line `argDoc` branch.
Note the import temporarily forces `SvgWriter::DEFAULT_SAVE_IMAGE_SCALED = 0`, otherwise
`savePicScaled` resamples the pages back down to 150 DPI and discards the chosen `pdfImportDPI`.

## Document library

The tag document browser owns **one directory, the library**, and every document opened from anywhere
else is copied into it - the library is the only place Write writes documents. Filesystem logic is in
`syncscribble/doclibrary.cpp` (no app dependencies, tested standalone by `scribbletest/librarytest.cpp`),
the policy in `ScribbleApp::initLibrary()` and its neighbours. Active whenever `useTagDocList` is on, on
every platform but wasm (`libraryManaged()`); with it off, everything behaves as before - on iOS that means
the system `UIDocumentBrowserViewController`.

- **Acquired, never assumed.** `DocLibrary::acquire(base)` takes `base`, `base 2`, ... - the first that is
  already a library, empty (OS litter like `.DS_Store` ignored), or creatable - and marks it with a
  `.write-library` file. The marker is what tells "our library from last run" from "a folder the user
  happens to call Write"; without it the browser would recurse into the user's folder and write its tag
  index there. The result is stored as `libraryPath` so later runs reuse it rather than re-searching.
- **Locations:** desktop `<documents>/Write` (XDG_DOCUMENTS_DIR on Linux, so it follows localized names),
  changeable under Preferences > Document List > Library folder (a text field; changing it *moves* the
  documents via `relocateLibrary()`, and a non-empty target gets a `Write` folder inside it). Android
  `/sdcard/Documents/Write`, i.e. shared storage that survives uninstall - **only once all-files access is
  granted**, because without it a reinstall cannot read the files it left there. Until then documents go
  to `Android/data/.../files/Write/` with a warning each time the browser opens, and
  `adoptPermanentLibrary()` moves them on the `STORAGE_PERMISSION` grant. `acquire()` rather than a plain
  create there is also how a reinstall finds the previous install's library again. iOS: `$HOME/Documents/Write`,
  which the Files app shows under Write (`UIFileSharingEnabled`), so the library stays reachable from outside.
- **An unreachable saved library is not forgotten.** A saved `libraryPath` whose parent is missing (an
  unmounted drive) puts the session in the fallback next to the config file without overwriting
  `libraryPath`; the next run that can reach it moves the fallback's documents back in. A library the user
  deleted (parent still there) is simply recreated.
- **Import is one branch in `doOpenDocument(std::string)`**, which covers drag and drop, Android intents,
  Import Document... and recent files: open the original read-only, then `importActiveDocToLibrary()`
  saves it into the library as `docFileExt` and continues in the copy. Saving through the document rather
  than copying bytes is deliberate - it converts multi-file HTML, which the browser does not list. The
  original is dropped from recent documents, or picking it there would import a second copy. The command
  line opens via `activeDoc()->openDocument()` directly and does **not** import automatically: a file named
  there was picked deliberately from where it is, so a dialog offers Copy to Library or Edit Original, and
  declining edits the file in place. A `--out` conversion never asks or imports. PDF import writes straight into the library.
- **iOS runs in "library mode"** (`initLibraryMode()` in `ioshelper.m`): SDL's view controller stays the
  root instead of being presented over a `UIDocumentBrowserViewController`, and library documents are plain
  `FileStream`s rather than `UIDocument`s - so recents work there too (the title button's menu was hidden
  on iOS only because recents needed security-scoped bookmarks), and the browser is cancelable. Anything
  arriving from outside - Files, "Open in", Import Document... (the system picker, `iosPickDocument`) -
  still comes in as a `UIDocument`-backed `UIDocStream`, since that is what handles security-scoped and
  coordinated reads; `dropEvent()` then imports it and never keeps editing through that `UIDocument`,
  which would write the original back. A picked file that is already in the library is reopened as a
  plain file instead of copied. `isInLibrary()` strips a leading `/private` on iOS: `/var` is a symlink to
  `/private/var`, `$HOME` comes without the prefix and `UIDocument` URLs sometimes with it, and
  `canonicalPath()` does not resolve symlinks, so otherwise library files would be imported again.
- **The classic folder browser is gone as a browser**; its menu entry is now **Import Document...**, the file
  picker whose result `doOpenDocument()` then copies in.
- **Migration is a one-time offer, copying not moving** (`offerLibraryMigration()`, `libraryMigrated`):
  documents under the old `currFolder` (plus the legacy Android folders), depth 3 since that may be a home
  directory, excluding multi-file HTML. `.write-tags` is carried over so tag names survive; recent
  documents are remapped to the copies.
- Moving the library (`moveLibraryTo()`) saves and reopens open documents from their new path and
  repoints a live browser with `TagDocList::setRoot()` - deleting it could free a window still inside
  `execWindow`.

Known gaps: Save As still writes wherever it is pointed and the document then lives there; the library
folder pref is a text field, not a folder picker; merging a fallback into a library that already has a
`.write-tags` keeps the destination's and leaves the other behind; PDFs cannot be picked on iOS (the
picker offers `public.svg-image` only); none of this has run on Android, iOS, Windows or macOS yet (verified
on Linux only; the iOS C++ was syntax-checked with the platform forced, `ioshelper.m` not at all).

## Default page size

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

## Dotted paper

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

## Ruling regions

An area of a page with its own ruling, overriding the page's inside it - for scanned or imported pages
whose printed lines are uneven or tilted. Made with the **Ruling Region** entry on the shape row (a drag,
like the box; single use), selected **only** through the "..." button drawn in its bottom-right corner while
the select tool is in hand. Geometry in `syncscribble/rulingregion.*` (no app dependencies, tested
standalone by `scribbletest/regiontest.cpp`), the element in `Element` (`isRulingRegion()`), the
selection in `RegionSelector` (`selection.cpp`), the UI in `ScribbleArea` and the region panel in
`MainWindow::buildRegionPanel()`.

- **Outline and ruling are separate.** `corners` is a free quad (drag a corner: reshapes the outline only,
  lines and ink stay); `origin`/`angle`/pitches are the ruling, whose lines are always parallel and evenly
  spaced - bending them to a skewed outline would give "line N" no single height. The red handle slides
  the lines (origin); Level resets the angle.
- **The region is a `<g class="write-ruling-region">` in `contentNode`**, parameters as `__rr*` attributes,
  children (paper fill + rule lines/dots) a regenerated cache like a shape's path. So undo (`StrokeAdded/
  Deleted/Transform`), sync (`<addstroke>`) and copying pages need nothing new, and an older Write draws it
  as a plain group. Parameter edits are `RegionChangedItem` (`<regionchanged>`); move/rotate/scale are
  ordinary transform items, because `Element::applyTransform()` has a region branch that maps the
  parameters (never a node transform, which would leave `__rr*` stale).
- **Not on any layer, below all of them.** `LayerList::REGION_LAYER` (-2, never serialized) has
  `zIndexOf() == -1`, which keeps regions first through `addStroke`, restacking and `layerFirstElement`
  (draw-under would otherwise put a marker stroke under an opaque region). Undo/sync replay insert at a
  literal sibling, and a NULL or vanished sibling means "on top", so `reinsertPos()` in `syncundo.cpp`
  re-homes a region below ink in that case.
- **Not ink.** `Page::isEditable()` returns false for a region - explicitly, since an unknown layer fails
  open - which keeps it out of every selection, eraser and insert space; `findStops` and the history panel's
  select lane needed their own skips. Deleting a selected region deletes only the region.
- **Moving it carries its ink.** The region selection holds the region plus every editable element whose
  bbox centre is inside the outline, re-decided at the start of each gesture (`refreshRegionSelection`),
  drawn as ordinary ink (`STROKEDRAW_NORMAL`). A tap, or a drop off the window or on another page, is not
  a move. Scaling is always uniform (a region's ruling cannot stretch one way).
- **Every ruled tool asks `Page::rulingAt(point)`** (a `RulingFrame`: origin, angle, pitches), with the
  gesture's press point, never `yruling()`/`getLine(y)`. `ScribbleArea::gestureFrame` is fixed at the
  press, so a stroke that wanders out of a region keeps its lines; `gridFrame` is the same without the
  blank-page phase, for snapping. `Selection::ruling` carries it into ruled select, ruled erase, insert
  space and reflow, which work in the frame's local coordinates (`RuledRange` is local; for the page's own
  ruling local is the page shifted by `yRuleOffset`, so behaviour there is unchanged - the `testN` fixtures
  confirm it). Ink belongs to the ruling it sits on: a ruled gesture in a region acts on that region's ink
  only, and one on the page leaves regions' ink alone.
- **Tilted frames needed two things beyond the mapping**, both found by the test, not by reading:
  rotating an element's page bbox into the frame inflates it (a stroke along a tilted line gets a box three
  lines tall and fails `containedRuled`'s early reject), so `localElementBBox()` measures a path's own
  points in the frame; and `StrokeBuilder::calcCom()`'s heuristic leans on page "up" (first point, bbox
  top), which put a stroke along a tilted line half a line high - strokes committed in a tilted region get
  their com measured in the frame. `groupStrokes()` skips com alignment on tilted lines for the same reason.
- **Found by review, each a one-line fix that is easy to undo by accident:** `localElementBBox()` adds a
  *deferred* pending transform (`Element::pendingDeferred()`), or tilted reflow subtracts the drag offset
  twice; `Page::loadSVG()` moves regions to the front (a file may have ink ahead of one, and `regions()`
  stops at the first non-region); a region's node transform from an older build is folded into its
  parameters on load; `invalidateStroke()` drops the whole selection when a peer edits the selected region
  (its selection also holds ink, so it never empties); a mirror keeps local y along the image of y; the
  bookmark list's ruled selector opts out of the same-ruling rule (`sameRulingOnly`); ruled move steps in
  the *selection's* ruling, not the press point's; `findStops` keeps the page's old line count.
- The "..." button is a screen-unit chip (`drawRegionButtons`), shown only in select mode (or while a region
  is selected), so `ScribbleApp::setMode` repaints when select-ness changes.
- **The region panel floats beside the selected region**, not in the toolbar: a column of separate rounded
  panels (kind dropdown - Lined/Squared/Dotted in an arrow popup -, a spacing slider, Background, Level,
  Delete). `RegionPanel::calcOffset()` picks the side at *layout* time - right, left, below, above, else over
  the region at the right edge - because only then is the panel's own size known; `syncRegionRow()` (from
  every `refreshUI`, which pan and zoom also reach) hands it the region's and view's window rects and marks
  it `BOUNDS_DIRTY`. The kind is derived, not stored: dots if `dotRadius > 0`, squared if `xRuling > 0`.
  Squared and dotted keep `xRuling == yRuling`, so the one slider sets both. The slider is logarithmic
  (10-120) and **previews** a drag through `previewSelRegionParams()` (no undo item), then on the handle's
  `SDL_FINGERUP` puts the old parameters back and commits the result with `setSelRegionParams()` - the
  `RegionChangedItem` records whatever is on the region when it is made, so skipping the restore would
  make undo a no-op. Both call `doRefresh()`: panel clicks are not canvas input, which is what normally
  repaints, so without it edits only showed after the next pointer event.
- **A stroke begun in a region stays in it** (`RulingRegionParams::clampInside()` in `doMoveEvent`'s
  `MODE_STROKE`): each point outside is moved to the nearest point on the outline, so the pen slides along
  the edge like a ruler. Without it, a stroke running past the edge belonged to the region (by its centre)
  but lay partly outside its outline, where neither the region's ruled tools nor the page's could reach
  it. Strokes begun on the page may still cross into a region. Existing out-of-bounds ink is untouched.
- **Outline** is on for new regions (`fromRect()` sets it); a file or peer that does not say so still
  loads as off, so older regions keep their look. (`RulingRegionParams::outline`, `__rroutline`, written
  only when on) is a third child,
  `rr-outline`: the outline path stroked in the rule color at 2 px non-scaling, twice a rule line. It is
  on the `<regionchanged>` wire as `outline`.
- **Ruled insert space and ruled select pressed just outside a region still act on it** - within half
  the region's line height of its outline (`REGION_PRESS_SLOP`, `Page::regionNear`, the `nearLines`
  argument of `gestureFrame`). The press keeps its real position, so above the top edge it is on line -1:
  "that line and everything after" then includes the top line, and the drag is measured in the same
  frame, so nothing jumps. This needed `RuledRange::nlines()` clamped: "to the end" is `MAX_LINE_NUM`
  (INT_MAX) lines, a range starting at line -1 is one more than an int holds, and the overflow made
  every range starting above line 0 select nothing. Not covered by an automated test.

Known gaps: page-level ruled insert space does not move regions below it (vertical/horizontal insert space
do); content pushed past a region's edge by ruled insert space leaves it; a corner drag can make a
self-intersecting outline; the hover cursor for a relative-width marker uses the page's line height; regions
on PDF/scan pages keep the colors `setTheme()` leaves on those pages; no detection of a scan's lines yet
(`quaddetect`'s Hough pass could fill pitch and angle). Tested by `ScribbleTest::rulingRegionTest()`
(mutation-tested, see the file) and `runRegionTests()`.

## Page layouts (Add Page popup)

`addpagemenu.cpp`. The popup opens on a short row - document default, the last two layouts used
(`recentPageLayouts`), and a "+". The "+" switches the same popup to the full grid: 12 built-in
layouts (three each of Lined, Squared, Dotted, Special), then the user's custom layouts
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

### Hold to snap (shape recognition)

With the pen tool, **hold the pen still at the end of a stroke** for `shapeSnapDelay` seconds and the ink
becomes the line, rectangle or ellipse it looks like; the rest of the gesture then scales it until the pen
lifts. A held **scratch-out** (back and forth over something) erases what it was drawn over instead. The
delay is 0.5-1.5 s, 0 turns it off (default 0.8), and is in Preferences > Shapes plus the Pen and Shape
Settings buttons. The recognizer is `syncscribble/shaperec.*` (+ `shaperecgeom.h`); its tuning, test set
and gates live in `labs/shape-recognition/`, whose `make test` compiles **this** copy - run it after
touching a threshold.

- **The hold is a timer, not an event.** A pen held perfectly still sends no motion at all, so a callback
  timer (`SvgGui::setTimer`, its own rather than the widget's, which autoscroll and fling share) polls
  `checkShapeSnap()`. "Still" is within `SNAP_HOLD_RADIUS` (6 screen units) of where the hold started -
  loose on purpose; the recognizer trims the hold cluster itself. One recognizer run per hold: a stroke
  that is not a shape stays ink and keeps drawing, and moving on is how to try again.
- **The recognizer sees screen units** (page coords times `mScale`), because its absolute thresholds
  were tuned in screen pixels; results are mapped back. It is fed the raw samples, not the stroke
  builder's filtered path.
- **The live shape is `currStroke`**, which is what the shape tool's drag gesture uses - so it is painted
  every frame and every escape route (the abandoned-gesture guard in `doPressEvent`, cancel) already
  cleans it up. `snapActive` says `currStroke` is a snapped shape and the stroke builder is gone.
- **Scaling:** a line's end follows the pen; anything closed is scaled uniformly about its centre by the
  ratio of the pen's distance from that centre now to when it snapped.
- **History:** the shape *as recognized* is always one undo step. If the gesture then moved any point
  more than `SNAP_CHANGE_MIN` (6 screen units), the change is a second step (`ShapeChangedItem`), so undo
  returns to the snapped shape before removing it. Below that it is one step - lifting the pen jitters.
  The add is ended as its own action before the params change, so sync sends the snapped shape first.
- **Mapping:** line -> `SHAPE_LINE`; axis-aligned rectangle -> `SHAPE_BOX`; a tilted one -> closed
  `SHAPE_POLYLINE` (a box cannot rotate); circle or axis-aligned ellipse -> `SHAPE_ELLIPSE`; a tilted
  ellipse -> closed `SHAPE_CURVE` at tightness 1 through 8 points on it. The shape tool's arrowhead and
  rounding toggles are *not* applied - they are armed for the shape tool, not for handwriting.
- **Scratch-out** erases every element whose bbox *centre* is inside the scratch-out's hull (an overlap
  test would take strokes it merely grazed), through `Selection::doSelect()` so locked and hidden layers
  are respected, as one undo step. Images are never taken. It happens the moment the hold fires; the rest
  of the gesture is `MODE_NONE`.
- **The shape is not left selected** after the pen lifts, unlike the shape tool's `shapeEditAfterDraw`:
  `clearSelOnly` is on by default, so a selection would swallow the next handwriting stroke.
- Disabled for ephemeral, line-drawing, snap-to-grid and centre-on-line pens.

### 45 degree angle snap

`snapShapeAngle()` (`shape.cpp`) is a **soft** snap: a line or polyline-family point whose segment to a
neighbour is within `shapeAngleSnap` degrees (default 8, 0 off, Preferences > Shapes) of a multiple of
45 degrees is projected onto that direction; an interior point in range of both neighbours goes to where
the two snapped directions cross, so a corner locks to a right angle. Soft rather than the pen button's
hard `applyShapeConstraint()`, so a line deliberately drawn at 30 degrees stays there.
`ScribbleArea::snapShapeAngleAt()` applies it at four sites: the recognized line and its end as the pen
scales it (hold to snap), the shape tool's drag and each placed polyline point, and `POINT` handle drags.
Skipped for snap-to-grid pens, which would be pulled off the grid. Tested in `shapetest.cpp`.

## Marker: centre on line

`ScribblePen::CENTER_ON_LINE` (a pen flag, so saved with the pen) turns a stroke into a straight
horizontal line along the vertical centre of the ruled line the press lands in
(`getYforLine(getLine(y)) + yruling(true)/2`, fixed for the gesture in `ScribbleArea::centerLineY`); it
reuses the `LINE_DRAWING` path, so x follows the pen and no filters are installed. The toggle is a
toolbutton on the pen options row itself, after the widths (`PenToolbar::centerLineToggle`, the ruled
eraser's `ic_menu_toggle_ruled` icon), shown only while the marker is the active tool - visibility is set
in `setPen()` *before* its early return, since switching tools with an identical pen still changes it.
Not covered by automated tests.

Tested by `ScribbleTest::shapeSnapTest()`, which fires the hold by calling `checkShapeSnap()` with a
time past the delay (the suite runs with `shapeSnapDelay` 0, so the wall-clock timer never interferes).
Mutation-tested: no second history step, always two, no scaling, a scratch-out that erases nothing, one
that erases by overlap, and ignoring the delay each fail it. The real timer path was checked live with
`agent-pointer stroke ... @1200` (below).

## Relative pen width

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

## Themed colors

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

### Storage and the picker (phase 2)

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

### The themed picker (phases 3-4)

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

- **§10.1 is resolved: the theme's dark paper supersedes the XOR.** For a themed document, Invert
  Colors mirrors the recipe (`paperL -> 1 - paperL`) and remaps strokes through `Palette::mapFrom()`,
  giving a real dark-paper rendering of the same theme rather than a negative of it; it round-trips
  exactly. **The cost is that Invert Colors is now a document edit, not a view filter** - one undo
  step, but saved and synced, so it is no longer a way to read in the dark without changing the file.
  Documents with no palette keep the XOR path.
- **A theme change is an undo item, `ThemeChangedItem`** (`COLORS_SPEC.md` §8), recorded by
  `setTheme()` inside the same action as the page recolor. This closed two gaps at once: undoing a
  restyle used to restore the stroke colors but leave the *new* recipe in place (so the next restyle
  recognised nothing), and the recipe did not reach whiteboard peers, whose pickers then offered a
  different palette. It carries the recipe plus `pageColor`/`ruleColor`/`bookmarkColor`/`linkColor` -
  the document half only; the machine-wide default a theme can also be saved as is the user's own and
  is never undone or synced. The recipe goes on the wire at full float precision, because restyle
  recognises ink only by exact color. Undoing the theming of a document that had none leaves it with
  an explicit recipe equal to the inherited one: the same palette, now pinned.

## Outlines (table of contents)

A page can carry one **outline entry**: a title plus a nesting level, from which the document's table
of contents is built. The entry points are `ScribbleDoc::setPageOutline()` and
`ScribbleDoc::outline()`; the UI over them is the outline view of "The general-purpose sidebar"
below.

Outlines are *not* a replacement for bookmarks, and the two are deliberately different things. A
bookmark is an `Element` with class `bookmark` living at a position *within* a page, labelled by the
user's own ink (`BookmarkView` re-draws the actual strokes; in `MARGIN_CONTENT` mode anything written
in the margin *is* a bookmark). It is also the **link target system** - `setSelProperties` turns
strokes into a bookmark with id `b-xxxxx` and hyperrefs are `href="#b-xxxxx"` - so removing bookmarks
would break internal links in every existing document. An outline entry is page-level, text-labelled
and nestable. One page, one entry: the intended way to work is a page per subject.

- **The entry lives in the page's own SVG**, as `__outline`/`__outlinelevel` on `contentNode`,
  alongside `xruling`/`papercolor`. `Page::outlineTitle`/`outlineLevel` are a cache of those, read in
  `loadSVG()` and written through by `Page::setOutlineEntry()`. This is the whole design: **a
  document-level list of `{page number, title}` would have to be fixed up by `insertPage`,
  `deletePage`, undo of either, and sync**, and would silently name the wrong page whenever one of
  those was missed. Living on the page, an entry moves with its page for free - and is carried along
  by copy+paste of a page, since the attributes are written eagerly rather than at save time.
- **`Document::outline()` caches nothing.** Page numbers are read off the pages' current positions
  every call, which is what makes the result correct by construction after any edit. It is also why
  the entry points return a fresh `std::vector<OutlineEntry>` rather than handing out a member.
- **The `__` prefix is load bearing.** An unprefixed `outline` would collide with the CSS property of
  that name, which usvg supports, so the parser could plausibly claim it as a presentation attribute.
  The neighbouring page attributes predate CSS support and are left as they are.
- **Levels are normalized when the outline is built, not when it is stored.** An entry may not sit
  more than one level deeper than the entry before it. Without that, deleting the page holding a
  level-0 entry leaves its level-2 children under nothing and a renderer has a hole it cannot draw.
  The *stored* level stays as the user asked, so restoring the parent restores the intent.
- **`PageOutlineItem` is a new undo item, deliberately not part of `PageProperties`.** Folding the
  title into `PageProperties` would have given undo for free, but
  `ScribbleDoc::setPageProperties(applytoall)` assigns one struct to *every* page - so an unrelated
  ruling or theme change would stamp one page's title across the whole document. Same trap
  `setTheme()` documents above.
- **The title is the first arbitrary user string on the sync wire**, so `PageOutlineItem::serialize()`
  writes through `XmlStreamWriter` (pugixml) rather than `fstring` - a title containing `&` or `'`
  would otherwise emit a malformed item and desync the stream. Undo items *are* the sync protocol, so
  this also needed an `outlinechanged` branch in `ScribbleSync::processItem()`.
- `setPageOutline()` returns false for a no-op, so retyping the same title cannot push an empty step
  onto the undo history. `UndoHistory::addItem()` calls `commit()`, which does the `dirtyCount++` - it
  must not also be done at the call site (the convention `Page::setProperties()` follows).

Known gap: **`Document::outline(loadpages=true)` loads every page** that has never been loaded, since
an unloaded page cannot be known to carry an entry. For a large delay-loaded document that is the
whole file. `loadpages=false` skips them instead, at the cost of an incomplete outline. Doing better
needs the titles in the document wrapper (next to the `<object>` dimensions) or in the bgz index - a
per-page index that is *derived* on save, not a second source of truth.

Tested by `ScribbleTest::outlineTest()` (`runAll`'s unit-check count). Each check was verified to fail
against a deliberately broken version - normalization removed, the attribute not read back, the undo
item not swapping, the wire format written unescaped with `fstring`, and `Document::outline()` caching
its result (i.e. the document-level index this design exists to avoid). That last mutation is the one
that pins the headline requirement, and the unescaped-title mutation confirmed the escaping is a real
hazard rather than a theoretical one.

## Layers

A document carries a **layer table**; every element carries a layer **id**. A layer can be locked or
hidden, and content on one cannot be selected, erased, moved or otherwise edited. Designed in
`LAYERS_INVESTIGATION.md`; that document is the rationale (including why the obvious design is
wrong), this is the summary. The entry points are
`ScribbleDoc::addLayer`/`removeLayer`/`setLayerLocked`/`setLayerHidden`/`setCurrentLayer`/
`moveLayer`/`moveSelToLayer`; the UI over them is the layers view of "The general-purpose sidebar"
below, which so far reaches only `addLayer`, `setCurrentLayer` and `setLayerLocked`.

**A layer is a tag on the element, not a `<g>` in the SVG.** `Element::layer()` serializes as
`__layer`, the same custom-attribute convention as `__shape`/`__comx`/`__timestamp`. That buys the
whole feature for free in three places that would otherwise each need work: the **file format** (no
new construct; an older Write build opens the document and sees one flat page, which is exactly
right), **sync** (`<addstroke>` round-trips the node through `SvgWriter`/`SvgParser`, so the tag
rides along with no protocol change), and **undo** (`StrokeAdded`/`Deleted`/`Transform` are
node-based and needed no change at all). The alternative - a real `<g>` per layer - was declined:
`Page::loadSVG` does `selectFirst(".write-content")` and `Selection::sourceNode` is a single
`Element*`, so it would touch a dozen call sites *and* an older build would read only the first
layer and drop the rest on save. The attribute has no such failure mode.

- **The one rule for new content is in `Page::addStroke`**, which stamps the current layer unless the
  caller passes one. Every route by which the user adds anything - a finished stroke, a shape, a
  pasted selection, an **inserted image**, a scanned page - funnels through there, so this is one
  rule rather than one per creation site. The callers that must *preserve* a layer pass it
  explicitly: free-erase subpaths (the pieces of an erased stroke belong to that stroke's layer, not
  the pen's) and the cropped copy in `MODE_CROPSEL`.
- **Layers stack, and the mechanism is the insertion point, not the tree.** `contentNode`'s children
  are kept partitioned by layer z-index; `addStroke` inserts at the end of its own layer's run. Undo
  and sync are unaffected because both express position as a *sibling node*, which this only chooses
  differently. `DRAW_UNDER` had to become "under this layer's content" (`layerFirstElement`) rather
  than "first child of the page", or it would drop a stroke outside its own layer's run.
- **The edit gate is `Page::isEditable()`, and it is consulted in exactly three places.**
  `Selection::doSelect` covers rect/lasso/ruled select, **the stroke eraser and the ruled eraser**
  (both are implemented as selections), and insert-space/reflow (which act on a selection built the
  same way); `selectAll` and `invertSelection` need their own, since they do not go through
  `doSelect`; and the **free eraser** needs its own, because it is the one erase path that builds no
  `Selection`.
- **An unknown layer id fails open** - editable, visible, bottom of the z-order. A layer can be
  deleted while elements still point at it (undo, a peer on an older table, a hand-edited file), and
  ink that cannot be selected, erased *or seen* is ink the user has no way to recover.
- **A lock guards a layer against work done from *other* layers, not against itself.**
  `LayerList::isEditable()` treats a locked layer as editable while it is `currentId`, so picking a
  locked layer in the sidebar is how it is edited, and picking any other layer puts it out of reach
  again. `setCurrent()` therefore accepts a locked layer and refuses only a hidden one, and locking
  the current layer leaves the pen on it. Leaving a locked layer clears the selection
  (`ScribbleDoc::setCurrentLayer`), since whatever was selected on it has just become uneditable;
  locking a layer clears it only when that layer is not current. Hiding the layer being drawn on
  still moves `currentId`, preferring an unlocked layer.
- **Ids are never reused, and the counter is serialized** (the leading `#<nextId>` record). Undoing
  the delete of a layer restores elements carrying its id; if a layer added in the meantime had been
  given that id back, those elements would silently join it. The max-id fallback in `parse()` covers
  a table written without the counter, but **only** when the removed layer was not the highest - which
  is why the test removes the top one.
- **z-order is the position in the table, never the id.** Reordering layers must not rewrite a single
  element; `ScribbleDoc::restackLayers()` re-sorts each loaded page's children by z-index, stably, so
  the order *within* a layer survives.
- **`StrokeLayerItem` records the sibling position as well as the layer id**, because the two are not
  independent: changing an element's layer restacks it, so restoring only the id would leave it
  somewhere the user never put it. `Page::moveToLayer()` returns the successor the element had,
  which is exactly what the item needs.
- **Three kinds of state, handled differently.** Which layer an *element* is on is on the element, so
  it undoes and syncs like any other stroke edit (`StrokeLayerItem`). The *table* (names, order,
  locked, hidden) is persisted in the per-document config as the `layers` value - the same mechanism
  the theme recipe uses, so no new file-format construct - and every edit to it is a
  **`LayerTableItem`**, so it is an undo step and syncs. Which layer is *current* (`currentLayer`) is
  neither: it is where this user's pen is, and two people on a whiteboard draw on different layers.
- **`LayerTableItem` records one layer, never the whole table** (`LayerList::layerState()`/
  `setLayerState()`: present or not, info, and the id of the layer directly below). Sync replays a
  local edit over a peer's by undoing and redoing it, and a whole-table snapshot redone over a peer's
  concurrent edit to some *other* layer would silently revert it. Position is "the layer below" rather
  than an index for the same reason - a peer's add shifts every index above it - and a layer whose
  layer-below has since vanished lands on top. `layertest.cpp` replays exactly that rebase.
- **Every table mutator goes through `ScribbleDoc::editLayer()`**, which runs the edit on a copy and
  applies only the resulting state of that one layer through `setLayerState()` - the same path undo,
  redo and a peer's item take. So what the undo item records and what the edit did cannot drift, and
  letting go of a selection on a layer that just went out of reach (locked while not current, hidden,
  removed) happens once, for local and remote edits alike. `removeLayer()` puts its `LayerTableItem`
  *last* in the action, after the `StrokeLayerItem`s moving its content off, so undo brings the layer
  back before the elements are moved back onto it.
- **Layers added during a whiteboard session get random ids** at or above
  `LayerList::SHARED_ID_BASE` (2^24), since two clients adding at once would otherwise both take
  `nextId()` and their strokes would merge. They never advance the counter, and `parse()`'s max-id
  fallback ignores them, or one would push the counter to the top of the range.
- **A client joining a whiteboard late gets the table and the theme in a snapshot**
  (`<layersnapshot>` plus a `<themechanged>`, at the head of `startSession()`'s opening block, ahead
  of the pages). Without it no amount of syncing edits helps a joiner, because both live in the
  config rather than in any page. It is applied outside the undo system, like those pages, and keeps
  the joiner's own current layer.
- Visibility is expressed as SVG `display` on the nodes (`Page::applyLayerState`), because drawing is
  a tree walk with no access to the table. It is applied at every `loadConfig`, at every table
  change, at the end of `Page::loadSVG` - a delay-loaded page arrives after the table has been
  read, so it has to be done there too - and whenever a stroke is put back into a page
  (`StrokeAddedItem::redo`, `StrokeDeletedItem::undo`). That last one is how a peer's stroke on a
  layer *this* client has hidden arrives hidden, and how undoing the delete of a stroke whose layer
  was hidden in the meantime keeps it hidden.
- `Document::applyLayerState()` deliberately touches **only loaded pages**; `removeLayer()` is the
  one operation that calls `ensurePagesLoaded()` first, because an unloaded page can still hold
  elements on the layer being removed and they must be reassigned undoably rather than orphaned.

Known gaps: hiding a layer in a large delay-loaded document only
affects loaded pages until the rest are loaded; and `setLayerHidden`, `setLayerName`, `removeLayer`,
`moveLayer` and `moveSelToLayer` have no UI yet (see the sidebar's own gaps below).

## The general-purpose sidebar

A panel with two views - the document's **outline** and its **layer table** - over the backends in
"Outlines" and "Layers" above; it is the first UI either feature has had. Opened from a button at the
left end of the **page ops** floating panel (Bookmarks is off the toolbar for now; Ctrl+B and View > Bookmarks still open it). Designed in the Penpot file
"General purpose sidebar" and specified in `SIDEBAR_SPEC.md`; that document is the rationale, this is
the summary. `syncscribble/sidebar.cpp`.

- **Pinned and floating are one widget in two parents, not two widgets.** Pinned, it is a flex
  sibling of the canvas inside `.sub-window-layout`, so the canvas is simply given the remaining
  width and `ScribbleView` re-centres the page in it - the "document stays centred" requirement
  needed no code at all. Floating, it is a child of `#main-container`'s box layout, anchored left or
  right, so it covers the canvas without resizing it. Which side it sits on is the same mechanism
  again: which end of the flex row, and which `box-anchor` when floating.
- **The reparent is deferred by a 1 ms timer.** Both toggles are pressed *inside* the sidebar, so
  reparenting from the press handler pulls the tree out from under the widget SvgGui is still
  dispatching to - seen as the panel vanishing instead of changing mode. `SvgGui::setTimer` asserts
  `msec > 0`, so it cannot be a zero-delay timer, and "was it open" is captured at schedule time
  rather than read inside the timer.
- **Dismiss-on-press-outside is a `Widget::eventFilter` on `#main-container`**, and the two
  mechanisms that look right are both wrong. `OUTSIDE_PRESSED` is delivered only to the widget that
  *captured* the press, so a panel in the tree never sees it. Pushing the sidebar onto SvgGui's menu
  stack (`showMenu` does take a plain `Widget*`) delivers `OUTSIDE_MODAL` but hands the panel to the
  menu machinery, which then closed it on presses *inside* it - its own buttons stopped working the
  moment it floated. An event filter is called on the way *down*, from the target up through its
  ancestors, so it sees presses the canvas would otherwise swallow. It **returns false**: swallowing
  the press makes the first click on any toolbar button only dismiss the sidebar.
- **One sizing trap, which has now caught five widgets here**: `prepareLayout()` reports a size only
  for the dimensions that are *not* fill, so a `fill` rect declares nothing and its parent sizes to
  its contents. The panel background is therefore `vfill` (its width is what widens the panel; an
  explicit `width` on a `<g>` is ignored by the box layout), each row's sizer is `hfill`, and so are
  the selector's and search box's `.field-bg` rects - the last two were found only after the selector
  rendered 12 px tall against a declared 49, twice in a row, because the constant was never reaching
  the layout. **When something here renders at the size of its contents, check the anchor before
  changing the number.** A separate trap: **a sizer rect must not be a flex item** - as a child of
  the actions row it made `space-between` spread the buttons against an invisible block, bunching
  them to one side; it is now a sibling inside a box container.
- **The insets are on the panel; the sidebar widget itself has zero margins.** ugui margins sit
  *outside* a widget's rect, so margins on the sidebar put its gutter where its own background cannot
  paint - and nothing repaints a strip the canvas has vacated, so the canvas's previous frame stays
  visible there (the page-number statusbar was left stranded beside the panel). The sidebar owns its
  whole column and fills it with `--canvas` (`.sb-backfill`), pinned only.
- Insets come from the toolbar's own `floatInset`/`floatTopInset`/`floatBtnSize`, which moved to
  `basics.h` for this - the design file's 12 is narrower than `floatInset` and left the sidebar out
  of line with the toolbar above it. The view selector's height is the one number here set from its
  *rendered* size (74 units -> 28 px) rather than from the design file, since it was picked by eye.
- **The canvas-facing edge takes no gutter at all** - `floatInset` earns its keep against the window
  edge, where it aligns the panel with the toolbar, but between the panel and the document it is only
  dead space. It follows the side the sidebar is on. That margin is visible **only when the document
  is zoomed past the viewport width**: at fit-page zoom the page is centred in whatever the sidebar
  leaves, so the ~200 px beside the panel is the centring, not this. Because the two horizontal
  margins are unequal, the panel is anchored `left vfill`/`right vfill`; left to the box layout's
  centring it splits the difference and drifts out of line with the toolbar.
- Design units map through `floatUIScale` like every other floating panel, verified by measurement
  rather than assumed: the tool buttons render the design's 64-unit button at 24.2 screen px, and the
  design's 372-wide sidebar comes out at 141 px, which is what it produces. Insets are derived from
  the toolbar's own geometry (`15 + 64 + 12`) rather than written as the literal 91.
- The view is picked with **`Button::setMenu`**, not an `onClicked` calling `SvgGui::showMenu` - a
  menu must be parented to its button before it can be shown, and shown manually it never appeared.
- **Building the list uses `outline(true)`; the change check uses `outline(false)`.** With
  `outline(false)` alone the list held only pages already loaded, and pages load lazily as the view
  scrolls, so entries popped in one at a time and each one rebuilt the list. `outline(true)` loads every
  page of a delay-loaded document once, when the list is built for it; after that it loads nothing.
  `Document::outline(false)` counts a page unloaded under the memory limit from its cached title, so
  the check does not see entries vanish either.
- Colors are literals from the Penpot file, as the floating toolbars' are - this panel is part of the
  same dark chrome and does not follow the document theme.
- **The toolbar button's glyph names the edge the sidebar is docked to** - reicon `sidebar-left`/
  `sidebar-right` as `ic_menu_sidebar_left`/`_right`, swapped by `MainWindow::updateSidebarButton()`
  whenever the side changes, and the action is checkable so the button also reads as open/closed.
  The left/right variants are therefore not decorative: they say where the panel will appear. Note
  the sidebar's *own* side toggle shows the **opposite** glyph, because that one names the side it
  would move to rather than where it is. These are deliberately **not** `ic_menu_split_lr`/`_rl`,
  which are the same reicon glyphs but mean split view.
- It goes through `addTBWidget`, so the overflow menu item is generated automatically when the panel
  is too narrow, rather than being a second hand-placed entry.
- **Pinned, it sits *under* `#main-toolbar-container`**, and ugui hit-tests a container anywhere in its
  bounding box (`nodeAt(p, false)`), not just on its children. The toolbar row's box is the full window
  width (its stretches) by the tallest panel, so opening a tool's options row made a band that swallowed
  the top of the view selector - only its bottom part responded. The row's layout containers and
  stretches are now `Widget::hitTransparent` (ugui, honoured by `SvgGui::widgetAt`), which also lets
  presses between the floating panels reach the page.
- **It refreshes from `MainWindow::refreshUI()`** via `refreshIfChanged()`, which compares a signature
  of what the list shows (document, outline entries, layer table, current layer) and rebuilds only on a
  change, since `refreshUI` runs after every stroke. Without it the list kept the previous document's
  contents after opening another, ignored undo and peers' edits, and came up empty when open at
  startup: `MainWindow`'s constructor opens it before there is an `SvgGui` to build rows through.

Known gaps: the lock toggle is drawn on every layer row (dimmed when unlocked) rather than only on
locked ones as designed, because the design leaves an unlocked row no way to lock it; the layer
preview is a plain block, since a layer spans every page and has no single thumbnail; and hide,
rename, delete and reorder exist in `LayerList` but have no place in the design yet.

## Moving around the canvas

Five ways to move the view, all of which already existed in some form; what follows is mostly a
record of why they did not work.

| gesture | what it does |
|---|---|
| mouse wheel | scroll up/down |
| Shift + wheel | scroll left/right |
| Ctrl + wheel | zoom about the pointer |
| middle mouse button drag | pan, whatever the active tool is |
| Pan tool (hand icon) | pan with an ordinary left drag |
| two fingers, or one finger once a pen has been used | pan/zoom |

- **The wheel did nothing at all on Linux, and the cause is not in Write's own logic.** `SvgGui`
  read only `SDL_MouseWheelEvent`'s integer `x`/`y`. Under **sdl2-compat** - the SDL2 API
  reimplemented on top of SDL3, which Arch and others now ship in place of SDL2 - those arrive as
  **0** and the scroll is reported solely in `preciseX`/`preciseY`. `SDL_GetVersion` reports
  something like 2.32.72, so nothing looks wrong. `sdlMouseEvent()` now prefers the float fields and
  falls back to the integers, which is also the better reading on genuine SDL2: they are the only
  fields carrying sub-notch touchpad resolution, which is the very thing the `*= 120` was added to
  preserve.
- **Never write `SDL_Event event = {0};` - write `= {}`.** For a union, `{0}` only initializes the
  first member (the 4-byte `type`), and since GCC 15 the rest really is left as stack garbage. On Linux
  the wheel events come from `linuxtablet.c` (XI2 buttons 4-7; SDL's own are disabled), which built them
  this way, so `preciseX`/`preciseY` - which the line above now *prefers* - held garbage: the wheel did
  nothing, or overflowed `wheel.y` and jumped to the end of the document. The Linux-built files were
  converted; the per-platform helpers (Android, iOS, macOS, Windows, wasm) still use `{0}` and are
  built by clang/MSVC, but are the same latent bug under GCC 15+.
- **`wheelModifiers()` is the single place the wheel's modifiers come from.** `wheel.direction >> 16`
  is a *Windows-only* convention: only there does Write synthesize the wheel event itself
  (`winhelper.cpp`) and pack the modifiers into those bits. Two callers read them unconditionally,
  and off Windows those bits are `SDL_MOUSEWHEEL_NORMAL`/`FLIPPED` - **and `FLIPPED` is 1, which is
  `KMOD_LSHIFT`**, so this did not merely disable Ctrl+wheel zoom and Shift+wheel, it could invent a
  Shift nobody held. The second caller was `ScrollWidget`'s event filter, where the same expression
  made *every* wheel event look unmodified, so a scroll container always swallowed the wheel instead
  of letting a modifier pass it to the widget underneath.
- **Modifiers are tracked from the key events, not from `SDL_GetModState()`** (`SvgGui::keyModState`,
  updated in `SvgGui::sdlEvent`). SDL updates its modifier state when events are *pumped*, while a
  widget reads it when the event is *dispatched*; a modifier released in the same pump as the wheel
  event therefore reads as already up. This was observed, not theorised: Ctrl+Z worked in the same
  session in which Ctrl+wheel scrolled instead of zooming. `keysym.mod` already accounts for the key
  in hand, so a press sets the bit and a release clears it; window focus changes resync from
  `SDL_GetModState()`, since key releases that happened elsewhere were never delivered here.
- `wheelScrollSpeed` is pixels-per-notch over 120 and was 0.2, i.e. 24 px a notch - slow enough to
  read as broken on its own. Now 0.5. Both it and `wheelZoomSpeed` are exposed in the Input prefs.
- **Middle-button pan is a flag on `ScribbleInput`, not a `MODEMOD` bit** (`midBtnPan`), because it
  must survive from the press to the release while `modemod` is rebuilt per event. It joins
  `mouseMode == INPUTMODE_PAN` in the one condition that starts `SCRIBBLING_PAN`, and it also has to
  suppress the early return for `mouseMode == INPUTMODE_NONE` - otherwise the one configuration where
  a pan gesture is most wanted is the one where it is dropped. `fingerId` is the button for down/up
  and the full mask for motion, so `SDL_BUTTON_MMASK` is only ever tested on down and up.
- **The Pan tool already existed and was simply hidden**: it was added to the toolbar only when
  `singleTouchMode` *and* `multiTouchMode` were both 0, i.e. only when touch was disabled entirely.
  That left a stylus-only user - no second finger, no middle button - with nothing but the scroll
  handle. It is now always present. It is not one of the tools (it edits nothing), so it keeps its
  own slot ahead of the tools sub-toolbar and its own `NormalPriority + 1`, which drops it into the
  overflow menu before the tools row is touched on a narrow screen.
- **Finger-pans-while-pen-writes on mobile needed no work** - `ScribbleInput::doInputEvent()` already
  switches `singleTouchMode` (and `mouseMode`) to `INPUTMODE_PAN` the first time a pen event arrives,
  and persists it via `penType`. Both are also exposed in the Input prefs.
- Space+drag, Ctrl+Shift+drag and pan-from-edge remain as they were.

### Testing this (agent-display)

Three gaps in `tools/agent-display.sh` had to be closed before any of the above could be checked, and
they are worth knowing about:

- **`scroll` sends `axis_discrete`, not just `axis`.** Xwayland turns discrete steps into X11 buttons
  4-7; a bare `axis` event is a continuous scroll and an X11 client - which Write is, inside cage -
  never sees it. `scroll` also takes an optional `X Y` first, for the same reason `click` does.
  `DY` is in wl axis units, so **negative scrolls up**.
- **`drag` takes a trailing button name**, so middle-button pan can be exercised at all.
- **`keydown`/`keyup`** exist so a modifier can be *held* across other commands; `key` sends a whole
  chord and cannot express Ctrl+wheel. Always pair them.
- **Do not drive the wheel with `xdotool` while agent-pointer is also in use.** Each agent-pointer run
  creates and destroys a virtual pointer, and this Xwayland miscounts the resulting enter/leave
  (`BUG: xwl_seat->pointer_enter_count == 0` in the session log). Once that fires, XTEST has no
  pointer to drive: keystrokes still arrive but wheel and motion silently do not, and the app looks
  broken when it is fine. Drive the wheel through `agent-display.sh scroll` and use `xdotool` only
  for keys. A `hold X Y` action exists on `agent-pointer` to park a persistent pointer, but it
  *suppresses* the transient pointers, so it is not a workaround - restart the session instead.

## Curve fitting the input (stroke builder)

A stroke used to be a polyline straight through the raw samples, and on the whole-pixel lattice most
platforms deliver (see below) that reads as a staircase rather than a line: at the 1-2 px spacing a mouse
gives, an integer grid only allows a handful of segment directions. `CurveFitFilter`
(`strokebuilder.h`/`.cpp`) replaces those straight segments with a curve, on by default
(`inputCurveFit`, the strength: 0 disables, 4 is strongest, default 2; exposed in the Input prefs).

- **It is a curve fit, not a low pass.** Each interior point is relaxed towards its uniform cubic B-spline
  knot `(P[i-1] + 4P[i] + P[i+1])/6` and a **centripetal** Catmull-Rom through the relaxed points is
  emitted as chords - the same construction `shape.cpp`'s Curve tool uses, and centripetal for the same
  reason (uniform overshoots on uneven spacing, which is what real input looks like). The relaxation is a
  1-4-1 kernel; for what it is actually worth per pass, see the measurements below rather than its
  Nyquist gain. Nothing is averaged over time, so the stroke does not trail the pen.
- **It must be installed first in the chain**, i.e. `addFilter`ed *last* (that call reverses order). It
  turns one input point into several, so it cannot map a `removePoints(n)` from upstream - and
  `SimplifyFilter`, which retracts points it has already emitted, would send it exactly that. Being first
  also means simplify runs *downstream* and decimates the densified curve back down.
- A segment is only final two samples later (it needs the knot past its end), so the raw current point is
  appended straight to the builder as a provisional tip and retracted on the next call - the same trick
  `LowPassIIR` and `StreamStabilizer` use. Those tips stack and pop in LIFO order, which is what keeps
  them from eating each other's points when more than one filter is installed.
- **`inputCurveFit` is the strength: how many times the relaxation is applied** (0-4, default 2), at the
  cost of one more sample of lag per pass and more rounding of genuinely sharp corners; the stencil
  widens with it, so the emit condition is `2 + passes` samples rather than 2.
  **Measured** (`curveFitTest`, 1.5 unit sample spacing), median turn angle and RMS residual against a
  fitted circle, levels 0-4: 18.4/0.294, 9.6/0.214, 5.2/0.182, 3.6/0.165, 2.4/0.154. Two things to read
  off that. The passes *do* compound - each is worth roughly another 1.7x on the turn angle, **not** the
  factor of 3 the 1-4-1 kernel's Nyquist gain suggests, because lattice noise is broadband rather than
  at Nyquist; that claim was in this document and was wrong. And the *geometric* residual **plateaus**
  while the angle keeps falling - most of it is bought by the first pass. That plateau is what is still
  visible as a lumpy outline at high zoom, and it is why strengths 1-4 are hard to tell apart on real
  handwriting, where the variation between strokes is larger than the difference between levels.
  Removing it needs a fit with a *tolerance* rather than more passes of a local smoother.
  **Tried and removed: a greedy incremental least-squares cubic Bezier fit** (`BezierFitFilter`, an
  `inputBezierFit` tolerance selecting it instead of the relaxation) - extend a run while one cubic
  fits within the tolerance, close it at the last sample that did. It lost on both metrics at every
  tolerance: 0.2/0.5/1.0/2.0 gave median turn 11.3/8.1/3.5/3.8 deg and residual 0.319/0.280/0.246/0.429
  against 2.4 and 0.154 for the relaxation, i.e. 60% worse on the residual that was the whole point.
  The cause is structural, so a better tolerance cannot rescue it: the fit **pins each run's end points
  to raw quantised samples**, so every joint carries the full half-unit error and the end tangents are
  estimated from noisy points, leaving only the interior of a run smooth. Tight tolerance gives short
  runs and therefore mostly joints; loose tolerance gives runs a single cubic cannot follow. Anything
  along these lines needs **free end points** (or joints placed on smoothed positions) to be worth
  rebuilding.
  It was first shipped as the *chord flatness tolerance* instead, which was a mistake worth not
  repeating: turning that up makes the filter do **less** (coarser chords, fewer points), so the one
  setting named after the feature ran backwards, and the strength was buried in a second pref nobody
  would find. The tolerance is now the fixed `CURVEFIT_TOL` - it only trades output points for accuracy
  of the curve and has no visible effect, so it was never a user knob.
- **This matters more than it looks, because the default pen is `TIP_FLAT | WIDTH_PR`** - so
  `hasVarWidth()` is true and strokes go to `FilledStrokeBuilder` with `style == Flat`, which extrudes a
  quad per input segment rather than sweeping a disc. The outline comes from each segment's *normal*, so
  a half-pixel centreline wobble is multiplied by the pen width: before this change a 4-unit pen drew
  visible square steps the size of its own width. Switching the tip to Round does not fix it on its own
  (the steps become scallops) - the centreline is the common cause, which is why the fix belongs here
  rather than in the tip geometry.
- Chord count per segment is `ceil(sqrt(dev/tol))` capped at 16, where `dev` is how far the curve strays
  from the chord it replaces - the error of an n-chord approximation of a cubic falls off as 1/n². `tol`
  is divided by `mZoom` at construction, so drawing zoomed in gets more points and zoomed out gets fewer.
- **Pen Tip is back in Pen Settings.** The floating-toolbar redesign (`f4f5062`) dropped the whole pen
  editor the old toolbar had - tip combo, vary-width, dash/gap, pen preview - leaving no way to reach a
  round tip at all. The tip is a flag on the pen, not a config pref, so it cannot come from
  `createToolSettingsButton`'s `prefNames`; that function grew an `extraRows` parameter for widgets like
  this. `setIndex()` rather than `updateIndex()` when syncing from the pen, or the combo writes the pen
  back to itself. A pen carrying neither `TIP_FLAT` nor `TIP_CHISEL` is drawn by `StrokedStrokeBuilder`,
  whose cap and join are round, so it shows as Round. Note the old toolbar auto-switched Flat -> Round
  above width 4; that heuristic was not restored.
- **`scribbletest.cpp` sets `inputCurveFit` to 0**, alongside `inputSmoothing`: the `testN_ref.html`
  fixtures are polylines through the input points. Baseline after this change is unchanged - 0 failed
  tests, 0 failed unit checks, 16 failed thumbnails.

## Sub-pixel input on Linux (experimental, off by default)

Hand-drawn curves render as runs of straight segments. The cause was measured, not guessed
(`tools/stroke-lattice.py` on a saved stroke): every input sample sits on a **1 screen-pixel lattice**,
and the turn angle between consecutive samples clusters at 26.6, 45 and 63.4 degrees - the only directions
an integer grid allows at the 1-2 px spacing a mouse delivers. Whole-pixel input at that spacing *is* a
staircase; nothing downstream can undo it. Full write-up in `JAGGED_STROKES.md`.

Where the precision is lost on this machine: the dev binary links **sdl2-compat** (SDL2 API on top of
SDL3). SDL3 delivers float pointer positions; sdl2-compat casts them to `Sint32` when filling the SDL2
event. And since Write links libX11 (for `linuxtablet.c`), SDL3 picks the X11 backend, so on a Wayland
desktop the app runs under **Xwayland, which itself only reports whole pixels** - there is nothing to
recover on that path at all.

What was built, all gated and inert unless enabled:

- `syncscribble/linux/sdl3input.cpp` - registers an SDL3 event watch via `dlsym` (no compile-time SDL3
  dependency, `RTLD_NOLOAD` so a real SDL2 build never touches it) and records each mouse event's float
  x/y and pen pressure before sdl2-compat truncates them. `SvgGui::subpixelHook` swaps the float back in
  when the SDL2 event is dispatched, matched by type, timestamp and truncated coordinates. Harmless on
  its own: under Xwayland it hands back the same integers.
- `linuxWayland` config (**default 0**) - switches sdl2-compat to the native Wayland backend so 1/256 px
  positions actually arrive. **Currently segfaults at startup** (`SDL_QuitSubSystem`/`SDL_InitSubSystem`
  re-init in `application.cpp`; not yet debugged). `linuxtablet.c` was given a `SDL_SYSWM_X11` guard so it
  no longer dereferences `info.x11` on a Wayland window.
- This is a **Linux/sdl2-compat fix only**. SDL2's mouse API is integer on every platform; each needs its
  own patch. It also does not change stroke geometry: a stroke is still a polyline through the raw
  samples, so a 2 px mouse spacing still gives 2 px chords and zoom still magnifies them. Fitting a curve
  in the stroke builder is the platform-independent fix; it is now built - see "Curve fitting the input"
  above - and `curveFitTest()` measures what it recovers (median turn 18.4 -> 2.4 deg at 1.5 unit sample
  spacing). It compensates for the quantisation rather than undoing it: the samples are still integers.
- Tried and reverted: measuring the flat pen's outline normal over an 8 px arc-length window instead of
  one segment. Its own test (integer-lattice circle, outline turn-angle q90) showed no improvement.

Side changes kept: the vendored SDL fork builds on Arch (`_GNU_SOURCE` in the non-system-SDL branch of
`syncscribble/Makefile`; pkg-config include paths in `scribbleres/SDL-Makefile.unix`); the SDL submodule
is on `write-linux`. `agent-pointer` sends fractional positions (256x extent) and interpolates one step
per pixel of travel; `agent-display.sh run --build DIR` selects any build dir and `wkey` sends keystrokes
to a native Wayland client. `tools/stroke-lattice.py` and `tools/outline-wobble.py` are the measurements.

To try it: `./ReleaseB/Kaku --linuxWayland=1`. To revert everything: `git checkout` the touched files,
`rm syncscribble/linux/sdl3input.*`, `git -C SDL checkout bf28970b2`.

## Tests

Test support is compiled in via the `SCRIBBLE_TEST` define (already 1 by default in `basics.h`). The binary is invoked with a `--test` flag; the GL renderer must be enabled (`--glRender=0 --test` is *not* runnable - GL rendering is required):

```
./Debug/Kaku --test          # ScribbleApp::runTest("test") -> ScribbleTest::runAll()
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

The palette math is tested the same way again in `scribbletest/colortest.cpp` (`runColorTests()`), also
standalone-buildable - see the command in its header. `./colortest --dump` prints the golden table in the
form pasted into `colortest_golden.inc`, which is only ever correct to do while a generator has never
shipped. Its checks are the generator freeze, the legibility invariant over ~1700 generated inks, sRGB
gamut, determinism, snap idempotence, `matchReference` injectivity, unknown-generator fallback, and the
OKLCh measurements `COLORS_SPEC.md` argues from (the Schuler palette, and yellow being 1.1:1 at its
cusp) - so if the spec and the code ever diverge, the tests say so.

The layer table is tested the same way again in `scribbletest/layertest.cpp` (`runLayerTests()`),
standalone-buildable - see the command in its header. Its checks are the fail-open rule, the current
layer always being writable, z-order being independent of ids, ids never being reused across a save,
and the serialization round trip including a name containing the separators.

- `ScribbleTest::layerTest()` - everything that needs a document: new content (including an
  **inserted image**) landing on the current layer, a later stroke on a lower layer stacking *under*
  the upper layer's content, a locked layer surviving Select All, a rect selection, the stroke eraser
  **and** the free eraser while another layer is current, the same layer becoming selectable once
  picked and out of reach again once left, a hidden layer not being drawn, reordering layers restacking the page, and
  the whole table plus every element's id surviving save/reload with the lock still biting
  afterwards. Two of its checks had to be rewritten after mutation testing proved them inert: the
  z-order half of the undo check passed against an item that dropped the recorded sibling position,
  because with one element on the layer the recomputed insertion point lands in the same place -
  it now moves the *first of two*; and the id-reuse check passed against a table that did not
  serialize its counter, because `parse()`'s max-id fallback covers every case except removing the
  *highest* id - it now removes that one.

- `ScribbleTest::docStateSyncTest()` - the layer table and the theme as undo steps and on the sync
  wire. Each edit is checked three ways: it is one undo step, undo/redo restore it, and what it
  serializes reproduces the edit when fed back through `ScribbleSync::processItem()` on a bare
  `ScribbleSync` - the receive path a peer runs, needing no server - after the edit is undone
  locally. Also pins the joiner snapshot, a peer's hide moving our pen, and a peer's stroke on a
  layer we have hidden arriving hidden. Mutation-tested: dropping the re-add visibility, not
  recording the theme change, and ignoring a received removal each fail it.

- **Two-client sync (`--synctest`)** needs a real server: run stylusboard
  (github.com/styluslabs/stylusboard, Node; `npm install moment minimist` is enough without `--db`)
  with `node whiteboard.js --enable-test`, which accepts the test token on port 7001, then
  `tools/agent-display.sh run --debug -- --synctest --syncServer=localhost` and read the verdict from
  the session log. Its API server also wants port 7000; if that is taken, edit the `listen(7000)` in
  a scratch copy - the sync test never uses it. **Baseline: 4 failed tests (test9, 12, 14, 16)**,
  confirmed independent of the layer/theme sync by rerunning with it disabled: test9 is bookmark ids
  (each sync undo group draws a UUID from the same RNG, so ids cannot match refs recorded without
  sync), 12 and 14 are last-digit float rounding, and 16 (`synctest01`) differs in one translate and
  a page's width/height attributes - the one worth investigating.
- `ScribbleTest::themeRoundTripTest()` - the theme recipe must survive save/reload, an unthemed document
  must inherit a usable palette, applying a theme must not flatten page rulings or widths, and an
  unknown generator id must be preserved rather than rewritten. Two of its checks had to be rewritten
  after mutation testing proved them inert: one read the *current* page's properties, which
  `setPageProperties()` leaves untouched by construction (it is the source of what gets pushed onto the
  others), so it passed against the flattening bug; the other compared the reloaded palette against the
  themed palette, which is exactly what a stale cache also returns. Both now check somewhere the broken
  and fixed answers actually differ.

- `ScribbleTest::restyleTest()` - draws four strokes (a family base, a *dark variant*, the *neutral*
  and an off-palette color), restyles, and checks each one behaves differently: base and dark move,
  the dark stays a dark rather than flattening to the base, the neutral stays the neutral rather than
  being mapped by ordinal, and the off-palette stroke is not touched at all. Then it checks the whole
  restyle is a single undo step. Its first version drew only a base color, so the neutral and variant
  paths were never exercised and both corresponding mutations passed - the extra strokes exist
  precisely because of that.

- `ScribbleTest::curveFitTest()` - drives `ie()` with samples taken off a circular arc and **rounded to
  whole units**, which is the lattice the curve fit exists to compensate for, and measures the committed
  stroke's median turn angle and its RMS residual against a circle fitted to its own points. It uses a
  pen with no width flags so the path *is* the centreline, and runs at two sample spacings, because they
  are different problems: quantisation is half a unit either way, so it is a large *angle* between
  samples 1.5 units apart and a small one at 10 units. Measured, fit 0 -> 4: **18.4 -> 2.4 deg** median
  turn at 1.5 units, but only **3.0 -> 1.7 deg** at 10 units. **The staircase is a slow-writing
  artifact** - a fast stroke's samples are far enough apart that the lattice barely bends them. The
  residual halves in both cases (0.294 -> 0.154, 0.250 -> 0.131), which is what says the fit removes
  noise rather than cutting corners. The checks are therefore written per regime; asserting a large
  improvement at both would assert something untrue of the sparse one. The fit-0 column measured in the
  same run is the control for the fit-4 assertion.
  Note the residual is measured against a *fitted* circle, not the arc the samples came from: `ie()`
  takes screen coordinates and the path is in document coordinates, so the original centre and radius
  do not describe the output. The first version compared against them and reported a meaningless 14.7.
  It also prints every relaxation level 0-4 as a diagnostic rather than a check; those lines are where
  the per-level numbers quoted above come from, and they are what a Bezier fitter was compared against
  before being removed.

When adding a check here, confirm it **fails against the broken code**, not just that it passes against
the fixed code - **seven** assertions in this area have now been written that were true either way and so
tested nothing. `colortest.cpp` was built this way: every check was run against a deliberately broken generator
(walk removed, constant changed, jitter made random, matcher collapsed to family 0, matcher off by one,
cusp interpolation dropped, an OKLab matrix coefficient corrupted, family 0 jittered, unknown generator
id silently rewritten) and each mutation was confirmed to fail. Doing this found a real defect: the
first version of the restyle check compared `matchReference` against itself, which is true however wrong
the matching is, and the injectivity check that replaced it found the hue-proximity collision above.

`usvg` and `ugui` each have their own standalone example/test build (`cd usvg && make` → `Release/usvgtest`; `cd ugui && make` → `Release/uguitest`), buildable once `Write` itself has been built (they reuse its makefile setup for `nanovgXC`/SDL).

## Agent display

Write is a GUI app, and an agent launching it puts a window on the user's own
session and steals focus. **Never run `Release/Kaku` or `Debug/Kaku` (or an old `Write` build) directly.**
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
tools/agent-display.sh stroke X1 Y1 X2 Y2 [X3 Y3 ...] [@MS ...]   # @MS pauses with the button held
tools/agent-display.sh type "text" | key ctrl+z
tools/agent-display.sh record start|stop [out.mp4]
tools/agent-display.sh test                         # ./Debug/Kaku --test
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
  process name - `-x 'Kaku|Write'`, never `pkill -f .../Release/Kaku`, which would also
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
