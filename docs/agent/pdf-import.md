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
Note the import temporarily forces `SvgWriter::DEFAULT_SAVE_IMAGE_SCALED = 0`, otherwise
`savePicScaled` resamples the pages back down to 150 DPI and discards the chosen `pdfImportDPI`.
