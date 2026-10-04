# PDF import (MuPDF)

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
Note the import temporarily forces `SvgWriter::DEFAULT_SAVE_IMAGE_SCALED = 0` (in `ImportSaver::save`),
otherwise `savePicScaled` resamples the pages back down to 150 DPI and discards the chosen `pdfImportDPI`.

Import runs synchronously on the UI thread, so nothing is drawn until it returns: a `showNotify()`
set before it never appeared and the app looked frozen. `ProgressBox` (`scribbleapp.cpp`, also used
by Noteful import) is a buttonless popup over the top window - the browser too, which is its own
`Window` covering the main window's notify bar - that `onProgress` updates per page. Each update
calls `Application::layoutAndDraw()` itself (throttled to 100 ms) and `SDL_PumpEvents()`, which only
queues events, so nothing re-enters while the window keeps answering the compositor. The popup keeps
the size of its first layout, hence the fixed-width sizer rect: without it longer messages were
squashed horizontally. There is no separate "Saving..." step any more: each page is written as it is
made (below), so the end of an import only writes the closing block and the thumbnail.

## Memory (shared with Noteful import)

An iPad (A12, 3 GB) killed the app mid-import (jetsam `per-process-limit` at 1.94 GB, no crash report -
look for `JetsamEvent-*.ips` with `idevicecrashreport -k -e <dir>`). Every page had been rendered to a
**decoded** 32-bit bitmap - ~35 MB for A4 at 300 DPI - and kept that way until the save after the last
page; `pdfImportLossy`/JPEG changed nothing, it only picks the format at save time. Measured on a
21-page Noteful notebook: memory grew 17 MB a page, 395 MB at the end. What fixed it, in layers:

- **Encoded-only images.** `PdfImport::Renderer::render()` encodes MuPDF's RGB pixmap straight to PNG/JPEG
  (stb, 3 channels, no 32-bit copy) and returns an `Image` holding only `encData` (`data == NULL`) -
  `Image::decodeBuffer` of the bytes. The writer saves those bytes as they are: `encode(fmt)` returns
  `encData` when it is already that format, and `hasTransparency()` is only asked of JPEG, which answers
  without decoding. `PdfImport::encodedOnly()` does the same for any image (Noteful pictures, crops).
  A picture kept uncropped keeps the very bytes it came in.
- **Streaming save.** `PdfImport::ImportSaver` saves after every page with Document's partial save
  (`SAVE_BGZ_PARTIAL`, which the app's own saves use) and `Page::unload()`s what is written, so an import
  holds about one page however long. An importer must not touch a page after `pageDone()` - Noteful's
  outline entries are therefore set while each page is made, not after the loop: setting one on an
  unloaded page reloads it, dirties it, and the final save rewrites everything from there. The saver
  also compresses (`compressLevel`, 2): imports used to be saved with level 0, i.e. stored, base64 and
  all - the same notebook is now 7.4 MB instead of 11.2.
- **One open PDF.** `Renderer` keeps the document open across pages; Noteful used to re-open and re-parse
  its embedded PDF for every page. A crop renders only that part (`renderArea`), not the page then a crop.
- **MuPDF's cache at 64 MB** instead of `FZ_STORE_DEFAULT` (256 MB): an import renders each page once,
  in order, and the store filled with an image-heavy PDF's scans to no use (measured: same 87 s either
  way, 150 MB less).

Result: the 31-page scanned `Geschichte.pdf` stays flat at ~187 MB (was 477 MB and climbing), Noteful
peaks at 148 MB (was 395). Quality is unchanged - same pixel sizes and formats.

**Limit memory** (Import popup, `importLimitMemory` + `importMemoryLimitMB`, 128 MB-2 GB): a
`MemoryBudget` counts what the import *holds* - its input (`setInput`, a whole notebook), each
`Renderer`'s MuPDF allocations (exact, through a tracking `fz_alloc_context`) and its in-memory PDF, and
page images not yet written (`keep`/`written`). Not the process footprint: that was tried first and is
wrong here - glibc keeps freed mid-size blocks and ASan quarantines everything, so a measured footprint
only rose, starved later pages and (with the cap below) dropped them. A page that would not fit is
rendered at a lower DPI, chosen from a peak of 9 bytes/pixel for PNG (pixmap, stb's filtered copy, the
output twice) or 4 for JPEG plus a 32 MB reserve for MuPDF itself. MuPDF's allocations are capped at
what is left - but never below what the page takes at 72 DPI, so a page comes out coarse rather than
not at all, and the limit gives way by that much. MuPDF reacts to a failed allocation by emptying its
cache and retrying, then throws; that fails the one page (logged), never the app. The app reports
reduced pages in a message box (`reportReducedPages`, not for `--out`). 128 MB holds the 300 DPI pages
of both test files with at most one reduced; at 48 MB every page still imports, at >= 94 DPI.
