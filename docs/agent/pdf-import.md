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
  - On Android, gc-sections only works because `libmain.so` exports nothing but `SDL_main` and
    `JNI_OnLoad` (`-fvisibility=hidden` + `android/libmain.map` version script, Makefile Android
    branch): every exported dynamic symbol is a GC root, and before this all ~3200 MuPDF globals
    plus FreeType's `FT_*` API (forced default-visible by `FT_EXPORT`) were exported. A native
    looked up by name (a `Java_*` function, a `dlsym` target) must be added to `libmain.map`, or it
    fails at runtime, not at link time.
  - `-Os` for MuPDF's objects only, via a target-specific `CFLAGS` override next to those flags.
    MuPDF runs once per page at import, never in the drawing or input path. Worth ~0.55 MB.
  - Release builds use LTO (`Makefile.unix` `-flto=auto`, `Makefile.msvc` `/GL` + `/LTCG`; `LTO=0`
    turns it off). `load-jpx.c` alone is built `-fno-lto`: with `FZ_ENABLE_JPX=0` its stub
    `fz_load_jpx_info()` lacks the declared `defcs` parameter (upstream bug), which LTO reports as
    `-Wlto-type-mismatch`. Excluding the object avoids carrying a local patch in the mupdf
    submodule, which CI clones from upstream.
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
`Page::loadSVG` treat the ruling as custom so it survives save/load. A page with an image in its rule layer
(`Page::hasImageBackground()`: PDF, scanned page, Noteful image background) does not auto grow
(`ScribbleArea::growPage`, `growDown`/`growRight`): it is the size of its image, so writing, moving or
scaling near the edge must not add blank paper past it. Insert Space still grows it (the `insSpace`
argument), as do Expand Down/Right. `ScribbleApp::doImportPdf()`
writes a `.svgz` into the document library (next to the PDF when there is no library, or to `tempPath`
for a `--out` conversion) and then opens it like any other document, which keeps undo, views
and sync entirely out of the import path. `.pdf` paths are routed to the importer from
`doOpenDocument()` (doc list, drag-drop, Android intents) and from the command-line `argDoc` branch.
The page image carries the class `write-pdf-background` (`PdfImport::BACKGROUND_CLASS`), which keeps its
resolution through every save - see [Resolution on save](#resolution-on-save-and-file-size).

Import runs synchronously on the UI thread, so nothing is drawn until it returns: a `showNotify()`
set before it never appeared and the app looked frozen. `ProgressBox` (`scribbleapp.cpp`, also used
by Noteful import) is a buttonless popup over the top window - the browser too, which is its own
`Window` covering the main window's notify bar - that `onProgress` updates per page. Each update
calls `Application::layoutAndDraw()` itself (throttled to 100 ms) and `SDL_PumpEvents()`, which only
queues events, so nothing re-enters while the window keeps answering the compositor. The popup keeps
the size of its first layout, hence the fixed-width sizer rect: without it longer messages were
squashed horizontally. There is no separate "Saving..." step any more: each page is written as it is
made (below), so the end of an import only writes the closing block and the thumbnail.

## Resolution on save and file size

**PDF page images keep their import resolution (`pdfImportDPI`) through every save.** Before, "Reduce image
size" (`savePicScaled`, on by default) resampled every image larger than its size in the document to one pixel
per unit times `paintScale` (`SvgWriter::DEFAULT_SAVE_IMAGE_SCALED`, set in `ScribbleApp::loadConfig`; the
check is in `SvgWriter::_serialize(SvgImage*)`) - ~150 DPI on a 1x screen. The import itself was exempt
(`ImportSaver::save` sets the scale to 0 around its writes), but the first ordinary save that *rewrote* a page
resampled it, and since the verbatim block copy (`canCopyBlock()`) only rewritten pages were affected, so
documents ended up mixed 150/300 DPI. Now `SvgWriter::NO_RESAMPLE_CLASS` (usvg) is set to
`write-pdf-background` and such images are written as the bytes they were imported as, never decoded.
Tagged: Import PDF and Insert PDF pages (always had the class) and, new, Noteful backgrounds rendered from the
notebook's PDF. **Not** tagged, so still reduced exactly as before: photos/scans (scan pages, Insert Image,
Noteful photo backgrounds and pictures), a PDF page inserted *as an image* (Insert Document), and images of
notebooks imported before Noteful backgrounds got the class. `ImportSaver`'s scale override stays for those
Noteful photos. Pages an old build already reduced stay reduced; there is no convert-to-150 command (deferred).
Trap: dropping the class from the importer, or the `NO_RESAMPLE_CLASS` assignment in `loadConfig`, brings the
resampling back silently - `pdfImportTest` catches both.

Preferences > Document List > **PDF import resolution**: Standard (150 DPI) / High (300 DPI), default 300 (an
`enum`/`enumvals` combo). A value set some other way (older free entry, command line) shows as "Other (N)" so OK
does not replace it.

Measured with `--pdfsizetest` (tests.md), level-2 svgz, 1x screen. Size after import = size after any number
of saves now; "old 300 after a save" is what the old build left once every page had been rewritten:

| PDF | pages | 150 DPI | 300 DPI | 300/150 | old 300 after a save |
|---|---|---|---|---|---|
| text-heavy glossary, A4 (0.26 MB) | 12 | 2.75 MB (229 KB/page) | 5.60 MB (467 KB/page) | 2.0x | 3.66 MB |
| blackboard photos, A4 (40 MB) | 20 | 7.95 MB (398 KB/page) | 19.85 MB (992 KB/page) | 2.5x | 8.89 MB |
| 16:9 slides (1.9 MB) | 25 | 12.6 MB (504 KB/page) | 34.2 MB (1.37 MB/page) | 2.7x | 13.6 MB |

A page resampled 300 -> 150 compresses worse than one rendered at 150 (3.66 vs 2.75 MB above). Decoded page
(RGBA, also what the GL texture holds): A4 8.7 MB at 150, 34.8 MB at 300; a slide 5.1 / 20.3 MB; up to two pages
are held decoded ahead (`prefetchImages`). Import CPU (all threads) 300 vs 150: text 4.3 vs 1.4 s, photos 20.0 vs
8.8 s, slides 21.0 vs 5.5 s. The first save that rewrote all pages of a 300 DPI import took 5.6 / 12.8 / 19.5 s
(decode + resample + PNG encode); it is now 0.1 / 0.2 / 0.7 s.

## svgz compression level per page block

`compressLevel` (doc config, default 2) is the deflate level of a save. `bgzBlockLevel()` (document.cpp) uses
level 1 instead for a page block that holds >= 64 KiB of base64 image data making up >= 3/4 of the block -
base64 of PNG/JPEG gives deflate's matcher nothing, the gain is Huffman coding, which level 1 does as well.
Anything without such an image (every stroke-only document) is written byte for byte as before; an explicitly
chosen level other than 2 is respected; any inflater reads either. Measured (`--pdfsizetest --savePicScaled 0`,
whole-document save, median of 5, CPU of all threads): 20 photo pages 1504 -> 447 ms (+0.03% bytes), 25 slides
2628 -> 792 ms (-0.5%), 12 text pages at 300 DPI 396 -> 125 ms (+1.2%), two real notebooks with ink and pictures
285 -> 232 ms (+0.05%) and 240 -> 96 ms (-0.04%). Imports use it too (each page block is written by the same
save).

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

After the import, in the app:

- **Saves.** With `savePicScaled` on (the default), every ordinary save after the import resamples each page
  image to one pixel per unit (the first such save is the one that throws away `pdfImportDPI`, see above).
  That decode + resample + JPEG encode was ~98% of a save (2.6-3.6 s for Mathe.svgz's 11 scans, 9 s for a
  114-page notebook). `SvgImage::saveCache` now keeps the bytes the writer produced, keyed by
  `Image::contentId` (new on every constructor and `invalidate()`), the crop and the scaled size, so only
  the first save pays: ~150 ms / ~630 ms after. The saved bytes are identical. **Trap:** code that changes an
  image's pixels in place must call `invalidate()` (it already had to, for the texture), or the old pixels
  are saved. Pinned by `ScribbleTest::imageSaveCacheTest()`. `Image::cropped()` no longer leaves a crop's
  source decoded in RAM (it kept ~35 MB per cropped scan for good).
- **Textures.** The caching painter's image textures are evicted least recently used past
  `Painter::maxCachedBytes` (512 MB desktop, 128 MB iOS/Android/wasm, set in `application.cpp`), but never
  one drawn in the last 8 frames - it used to drop *every* texture at once, and with a cap below the visible
  set it re-uploaded every image every frame (going back and forth between two scan pages under a 64 MB cap:
  4.1 s for 40 frames before, 0.72 s after, the same as with no cap). A page that is unloaded frees its
  images' textures (`Image::~Image` -> `invalidateImage`).

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

## Speed: PNG encoding, the render/encode overlap, decoding ahead

80-90% of an import used to be stb's PNG writer: miniz level 8 (512 match probes) behind stb's
try-every-filter search, 0.6 s for a 300 DPI text page and 4.7 s for a scan. `encodePixmap` now calls
`Image::encodePNGStriped` (ulib/image.cpp) instead:

- Same per-row filter choice as stb (least sum of absolute values), in loops that vectorize.
- 192 probes with lazy matching rather than 512. Measured +0.2% bytes on text pages and +3.9% on scans, and
  *pixels identical*. Fewer probes cost scans a lot: 128 probes is +5.7%, level 3 is +11%, and a fixed Up
  filter is +13% on text. zlib (it is linked for MuPDF) was no better than miniz at equal size.
- Stripes of rows deflated independently on up to 8 threads. A filter only reads the row above in the
  source pixmap, so the stripes are independent. Each stripe is its own IDAT chunk and all but the last end
  in a sync flush, so together they form one valid zlib stream; the Adler-32s are combined. Any PNG decoder
  reads the result. The stripes cost under 0.1% in size.
- `Image::encodePNG()` (thumbnails, scans and screenshots saved into a document, a scaled or cropped image on
  save, PNG export) uses it too, with a thread per 256K pixels: a 240 px page thumbnail stays on one thread,
  where it encodes in about half stb's time at -2..+1% bytes. Striping a small image costs much more (+6-20%
  on thumbnails), hence the threshold. Saved PNG bytes differ from what stb wrote; the pixels do not.

`Renderer::renderPages` (which `importPdf` uses) renders page *n+1* on the importing thread while page *n*
is encoded on another one. Only the encoder runs off that thread: it reads the pixmap's samples and nothing
else. The MuPDF context (created with NULL locks), the page and dropping the pixmap all stay on the
renderer's thread, so MuPDF's threading rules hold without cloned contexts. **Under a memory limit
(`budget->limited()`) pages run one at a time again**: `peakBytesPerPixel` assumes a single page in flight,
and overlapping holds two pixmaps. Pages still go to `onPage` and `ImportSaver::pageDone` in order.
Measured on a busy 12-core machine, base -> new: 25-page LaTeX PDF 17.7 s -> 4.2 s, 6-page 300 DPI scan
32.7 s -> 5.7 s (CPU 16.5 -> 9.8 s and 30.4 -> 17.5 s). Rendering several pages at once would need
`fz_clone_context`, locks and display lists, and was not done: after this change rendering is a small share
for text PDFs.

**Decoding ahead.** A page image is kept encoded, so the first draw of a page used to decode its 300 DPI
PNG on the UI thread, a 50-130 ms frame. `ScribbleArea::prefetchImages()` runs after every paint (hooked in
`ScribbleView::doPaintEvent`). When the visible pages change, it loads the page next to them in the
direction of travel (the following page after a jump) and calls `Image::prefetchDecode()` on its large
images that have no texture yet (`Painter::isImageCached`). One worker thread decodes a copy of the bytes.
`Image::bytesOnce()`, which `Painter::drawImage` already calls, takes the result, or waits for it if the
decode is still running. A job keyed by the encoded buffer's address survives a move of the Image.
**`invalidate()` (so the destructor too) must drop the job before that buffer is freed or reused.** At
most 2 decoded pages are held (~35 MB each). There are no worker threads on wasm. Measured with
`SUMI_BENCH_PDF_PACED=1` (benchtest, reading pace): pdf scroll frame p95 162 -> 22 ms. The plain
`pdf_scroll_frame` scrolls a third of a screen per frame with no idle time, so the next page is needed one
frame after it is asked for, while its decode is still running. On this oversubscribed machine a worker
decode took ~1.5x as long as one on the drawing thread, so waiting for it could cost more than decoding
there. Once both speeds have been timed, `bytesOnce()` decodes the page itself when the worker's expected
remaining time is longer than that. Interleaved runs measured that fling metric as roughly neutral within the
noise: base p95 median 121 ms, new 148 ms, both ranging 100-540 ms. Guessing the worker's speed before its
first decode had been timed made it worse, because the abandoned decode keeps the CPU busy.

## Insert PDF (into the open document)

Document menu > **Insert PDF...** (all platforms; disabled when read-only in sync or without PDF support) adds
a PDF's pages to the *current* document instead of making a new one. Chosen: **after the current page** -
the same place Insert Document puts pages, so the two behave alike.

`ScribbleApp::insertPdfPages()` runs the ordinary `PdfImport::importPdf()` (same DPI/lossy prefs, memory
budget, progress box, page size from the PDF, rule-layer background, no ruling) into a scratch `Document`
with no `ImportSaver`, then `ScribbleDoc::insertPagesFrom()` moves the pages in between one
`startAction(...|MULTIPAGE)` / `endAction()` pair. That function is the body Insert Document used to have
inline and is shared with it, so the insertion is one undo step and `endAction()` sends it through
`ScribbleSync` like any page insertion. Trap: unlike Import PDF the pages are held (encoded only) until the
document is saved - nothing streams to disk, so a very long PDF costs its encoded size in memory, and the
undo history keeps it too. No cancel (Import PDF has none either). Test: `pdfImportTest` inserts into a
3-page document and checks +3 pages, placement after the current page, one undo step, undo/redo.
