# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

**Sumi** (墨, "ink"; store name *Sumi Notes*, domain probably sumi-notes.com - not final) is a cross-platform (Windows, Mac, Linux, iOS, Android, wasm) C++ application for handwritten notes, forked from **Write** by Stylus Labs (AGPL-3.0).

### The rename (Write -> Kaku -> Sumi)

The app was briefly called Kaku before Sumi; a stray "Kaku" anywhere is a leftover. The iOS bundle id is
`de.gipflig.sumi` (`IOS_BUNDLE_ID` in the Makefile), the developer's own domain rather than the product's, and
permanent once published. Only what a user sees says Sumi: window/app titles, dialogs, the About box (which must keep crediting
Stylus Labs and naming the AGPL - section 5 requires marking a modified version), the executable
(`TARGET = Sumi`, so `Debug/Sumi`, `Sumi.app`, `Sumi.exe`, `Sumi.html`), plists, the Android label, the
installers and `scribbleres/linux/Sumi.desktop`. The docs site is `sumi-docs/`. Translations are keyed by
English text, so a renamed `_()` string must change in `scribbleres/strings/strings.xml` too, followed by
`make res_strings.cpp` in `scribbleres/`.

**Deliberately still "Write"** - each is identity or storage, and renaming it strands data or breaks upgrades:
the config file (`~/.config/styluslabs/write.xml` and the Windows/Android equivalents), the `.write-library`
marker, every `write-*` class and `text/writeconfig` inside documents, the Android package
`com.styluslabs.writeqt` (and its Java folder) plus the app-private fallback library `files/Write/`, the
`"styluslabs"` salt in sync password hashing (protocol), the `write-*` SDL branches, `xcode/Write`, and the
WiX component ids / `Software\Stylus Labs\Write` registry key. The default *new* library is `Documents/Sumi/`;
existing installs keep theirs because `libraryPath` is saved. Still pointing at Stylus Labs and needing a
decision: the update check (`styluslabs.com/write/versions.xml`), Help and share URLs, the Play Store review
link, the iOS IAP, and the Windows installer's `Manufacturer`/`UpgradeCode`.

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
git clone --recurse-submodules -c submodule.mupdf.update=none https://github.com/Tear4Pixelation/sumi-notes
cd sumi-notes/syncscribble && make mupdf-submodules
```

## Architecture (syncscribble/)

- `scribbleapp.cpp`/`.h` - `ScribbleApp`, top-level application/event loop and command dispatch
- `application.cpp`/`.h` - `Application`, thin static wrapper around SDL window/GL context setup
- `document.h`, `page.h`, `element.h` - document model: a `Document` is a vector of `Page`s, each page is one SVG `<svg>` node (`SvgDocument`), and strokes/content are `Element`s built on top of `usvg` SVG nodes
- `scribbledoc.cpp`/`.h` - `ScribbleDoc`, owns a `Document` plus undo history, load/save, and command handling; central point most other classes are `friend`ed by
- `scribblearea.cpp`/`.h` - `ScribbleArea`, the drawing/view widget (subclass of ugui `Widget`) that renders pages and handles input
- `scribbleview.cpp`/`.h`, `scribblewidget.cpp`/`.h` - view/window composition around `ScribbleArea`
- `pdfimport.cpp`/`.h` - `PdfImport`, renders PDF pages via MuPDF into the background (rule) layer of new `Page`s; see [docs/agent/pdf-import.md](docs/agent/pdf-import.md)
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
  Then `cd syncscribble && make SDL_DIR=../SDL-wasm` → `EmRelease/Sumi.{html,js,wasm}`, and
  `python3 wasm/serve.py` serves it to the LAN (no-cache, or Safari keeps running a stale `.wasm`).
  The page is `wasm/shell.html`. The web build always renders in **software** (nanovg_sw, blitted through an
  SDL surface), hence `NO_PAINTER_GL`. Documents live in Emscripten's in-memory FS - **nothing persists across
  a reload**, and Save downloads the file. Pen input is Pointer Events (`wasm/wasmhelper.c`); Chromium was
  verified to draw a pressure stroke, WebKit/iPad Safari has not been tested from here (Playwright's WebKit
  needs Ubuntu libraries this Arch machine lacks).

CI is **manual only** (`workflow_dispatch`, nothing runs on push) and builds this repository. `.github/workflows/sumi-ci.yml`
builds Linux (`make DEBUG=0 USE_SYSTEM_SDL=0 real_tgz`), Windows (`make DEBUG=0 zip`), and Android (`./gww assembleRelease`),
building the `write-linux`/`write-win` SDL branches from source each time rather than using system SDL. `.github/workflows/ios.yml`
builds the iPad/iPhone IPA, ad hoc signed when the signing secrets are set - see [docs/agent/ios-build.md](docs/agent/ios-build.md).
`ulib` and `ugui` come from the developer's own forks (branch `sumi`), since they pin commits upstream does not have.

## Feature notes (read the one you are touching)

Each feature's design rationale, traps and known gaps live in `docs/agent/`. They record *why* the code
is the way it is - several fixes there are one line that is easy to undo by accident - so read the
relevant file before changing that area.

- [pdf-import.md](docs/agent/pdf-import.md) - MuPDF compiled inline, size trimming (`NO_CJK`, `-Os`, gc-sections), rule-layer page images
- [noteful-import.md](docs/agent/noteful-import.md) - reverse-engineered `.noteful` format, units, pressure strokes, archives, the Import FAB
- [document-library.md](docs/agent/document-library.md) - the single library folder, `.write-library` marker, import/migration, iOS library mode and `/private/var`; the system file picker (`filepicker.cpp`, every platform); browser fonts; Create Notebook dialog and covers
- [paper.md](docs/agent/paper.md) - default page size prompt (A4/Letter), dotted paper (`dotRadius`), Add Page layouts popup
- [ruling-regions.md](docs/agent/ruling-regions.md) - Paper Patch: per-area ruling, `rulingAt()`, tilted frames, region panel
- [document-scanning.md](docs/agent/document-scanning.md) - homography, warp, enhance, quad detection in `ulib`; capture via the image picker
- [shapes.md](docs/agent/shapes.md) - parametric shapes, `ShapeParams` descriptor, polyline/curve family, toggles, hold-to-snap recognition, 45 degree snap
- [pen-and-tools.md](docs/agent/pen-and-tools.md) - marker centre-on-line, relative pen width, selection color/width/dash style, Switch Back, Select touching, reflow indent and word gap
- [history-panel.md](docs/agent/history-panel.md) - `ButtonDragTimeline` undo/redo ruler, lanes, `--screenDPI` layout preview
- [night-mode.md](docs/agent/night-mode.md) - Invert Colors as a render-time `ColorMap`, mirrored theme vs lightness flip, never a document edit
- [screenshot.md](docs/agent/screenshot.md) - region kept dashed after a selection (even an empty one), capture without paper/ruling, white-to-alpha, crop dialog, Copy / Add to page
- [themes.md](docs/agent/themes.md) - palette generator (`cusp-walk-1` is FROZEN), theme storage, restyle, themed picker, `ThemeChangedItem`
- [outlines.md](docs/agent/outlines.md) - per-page outline entries, `PageOutlineItem`, why no document-level index
- [page-tags.md](docs/agent/page-tags.md) - tags on pages as `write-pagetag` elements, the toolbar tag button and placing tags on the pointer, the `pagetags` summary and page thumbnails, page cards, inheritance and the blink
- [layers.md](docs/agent/layers.md) - `__layer` tag not `<g>`, edit gate, lock semantics, `LayerTableItem`, sync of the table
- [sidebar.md](docs/agent/sidebar.md) - outline/layers sidebar, pinned vs floating, sizing traps, row drag (`rowdrag.cpp`)
- [navigation.md](docs/agent/navigation.md) - wheel/zoom/pan, sdl2-compat `preciseX/Y`, `SDL_Event = {}`, modifier tracking, testing scroll in agent-display
- [stroke-input.md](docs/agent/stroke-input.md) - `CurveFitFilter` (measurements, failed Bezier fit), experimental Linux sub-pixel input
- [tests.md](docs/agent/tests.md) - standalone test binaries (scan, shape, color, layer) and what each in-app `ScribbleTest` check pins
- [agent-display.md](docs/agent/agent-display.md) - two displays (Wayland pointer vs Xwayland keys) and the traps when driving the app
- [ios-build.md](docs/agent/ios-build.md) - manual iOS workflow, ad hoc signing secrets set up from Linux, installing on the iPad

Other design documents at the repo root: `SHAPES_SPEC.md`, `COLORS_SPEC.md`, `COLORS_HANDOFF.md`,
`LAYERS_INVESTIGATION.md`, `SIDEBAR_SPEC.md`, `JAGGED_STROKES.md`.

## Tests

Test support is compiled in via the `SCRIBBLE_TEST` define (already 1 by default in `basics.h`). The binary is invoked with a `--test` flag; the GL renderer must be enabled (`--glRender=0 --test` is *not* runnable - GL rendering is required):

```
./Debug/Sumi --test          # ScribbleApp::runTest("test") -> ScribbleTest::runAll()
```

Other in-app test modes dispatched the same way (see `ScribbleApp::runTest` in `scribbleapp.cpp`): `synctest` (sync between two instances), `perftest`, `inputtest`. Test fixtures/reference output live in `scribbletest/` (`testN_in.html` inputs, `testN_ref.html` expected results). Because tests require a real GL-capable display, CI does not run them (see the commented-out step in `sumi-ci.yml`).

Pure-math parts (scan, shapes, palette, layer table, ruling regions, Noteful reader, library) also build as
standalone tests without GL - commands in each `scribbletest/*test.cpp` header; details and what every
in-app check covers are in [docs/agent/tests.md](docs/agent/tests.md).

When adding a check, confirm it **fails against the broken code**, not just that it passes against the
fixed code - seven assertions here have been written that were true either way and so tested nothing.

`usvg` and `ugui` each have their own standalone example/test build (`cd usvg && make` → `Release/usvgtest`; `cd ugui && make` → `Release/uguitest`), buildable once `Write` itself has been built (they reuse its makefile setup for `nanovgXC`/SDL).

## Agent display

Write is a GUI app, and an agent launching it puts a window on the user's own
session and steals focus. **Never run `Release/Sumi` or `Debug/Sumi` (or an old `Write` build) directly.**
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
tools/agent-display.sh test                         # ./Debug/Sumi --test
tools/agent-display.sh status | stop
```

Interaction is **blind and scripted**: act, then screenshot only when you need to
see the result. There is no continuous stream, by design - `record` exists for
capturing a specific interaction to show the user, not for watching.

Setup is `tools/install-agent-display.sh` (needs sudo). Verified working
end to end: stroke, toolbar click, tool switch and Ctrl+Z undo.

Before driving the app, read [docs/agent/agent-display.md](docs/agent/agent-display.md): keyboard goes
through cage's Xwayland and the pointer through Wayland, a click must be one invocation (`click X Y`, never
`move` then `click`), close the document list before drawing, and `--test` exits with the failed-thumbnail
count (16 of 17 is the normal baseline).
