# Noteful import

Noteful's `.noteful` export is an undocumented binary format, reverse engineered from sample exports;
`tools/noteful-dump.py` is the reference decoder and its header is the format description (container +
index, tagged big-endian records with per-field sync timestamps, collections with alive flags, the
stroke blob). `syncscribble/notefulfile.*` is the C++ reader - no app dependencies, C++14, no
exceptions, bounds-checked; tested standalone by `scribbletest/notefultest.cpp` against a notebook it
synthesizes itself (no personal file is checked in), mutation-checked. `notefulimport.*` builds a
`Document` like `PdfImport` does; `ScribbleTest::notefulImportTest()` covers it, and
`NOTEFUL_CONVERT=in.noteful NOTEFUL_OUT=out.svgz ./Debug/Kaku --test` converts a real notebook.

- **Units:** Noteful pages are pixels at 132 dpi (A4 = 1091.34 wide); Kaku's 150/inch gives a 150/132
  scale. Paper templates give line spacing in *points* (`lh:20` is 20 pt = 36.7 Noteful units) - read
  as units, the grid came out almost half size. Rule color `#9a9888`, from the templates' own PDFs.
- **Strokes with at most 4 points are raw f32 pairs**; longer ones are a box plus u16 pairs normalized to
  it. At exactly 4 both layouts are 32 bytes, so nothing breaks visibly: reading them as a box turned
  i-dots into long vertical lines. The u32 at offset 36 of a stroke header is its layer id.
- **Stroke flag 1 is a pressure pen**: after the box two f32 (the pressure range) and u16 x/y/pressure
  *triples* - the only layout under which all 87 samples span their box and draw smoothly. Imported
  through Kaku's own `StrokeBuilder` with a linear round pen (prParam 1, wRatio 1): width = Noteful width
  x pressure, each stroke scaled by its peak pressure because the builder clamps pressure at 1. So they
  are ordinary filled pen strokes (`write-round-pen`), erasable and restyleable.
- **Short pressure strokes (<= 4 points) are unconfirmed** - no sample has one. Raw floats are used while
  no larger than the compressed form: 8n <= 16 + 4n and 12n <= 24 + 6n both hold exactly up to 4 points,
  which predicts f32 x/y/pressure triples. `parseShortPressure()` tries that, then pairs-then-pressures,
  then the long form, accepting the first whose values are sane *and* which ends where a record (or the
  blob) does. The test's long-form case is chosen so only that end check can reject a wrong reading.
- **An element's frame x, y is its centre**, not its corner (w, h, rotation follow). Read as a corner,
  every shape, image and text box sat w/2, h/2 right of and below its place: highlighters (height 0)
  half their length to the right of the word, and the sample's tag boxes and pasted image ran off the
  page (890 + 382 > 1091). `parseElement()` turns it into a top-left frame once, for every kind. Checked
  on the sample's graphs: only the centre reading puts a tangent through its intercept on the hand-drawn axes.
- **Highlighter** = element field 9 (u16) = 1; only seen on straight lines (kind 20, width 24). Noteful
  stores the colour opaque, so importing it as is painted solid bars over the words. Imported at half
  alpha (Kaku's marker, `highlightPen`) and inserted first in its layer (`layerFirstElement`, as a
  `DRAW_UNDER` stroke). Noteful's own highlighter opacity is not stored anywhere found.
- **Rotated images** get a node transform about their frame centre (`SvgImage::setTransform`), the same
  convention as shapes - unconfirmed for images, which no sample had rotated.
- **Shapes without a path**: kind 6 ellipse (only circles seen), kind 3 regular polygon (field 20 =
  sides, first vertex at the top). `frame[4]` is a rotation in radians about the frame centre - both
  conventions checked on one sample only (a triangle drawn as a NOT gate points the right way).
- **Cropped pages** (template field 0 = 1): the page has its own size and field 9 (type 0x23, a rect)
  says where the whole asset page is drawn; the importer keeps only the pixels on the page.
- **Archives** (`importArchive()`): Noteful exports folders as a .zip of the folder tree plus an
  `archive.json` manifest (ignored - the tree says the same); a directory works too. One .svgz per
  notebook in the library (`DocLibrary::uniquePath`, so clashes become "Name (2)"), tags applied through
  the library's `TagStore` (created as subtags). `folderTags` adds the folder path as a tag - an option
  because folders often mirror tags already. A TagDocList open on the library must reload its store
  afterwards. Reads go through ulib (`readFile`, a `FILE*` for miniz) because ulib's `fopen` is the
  UTF-8-safe one on Windows; `miniz_zip.c` is now in the build for the reader.
- Paper templates become Kaku ruling (editable paper); imported PDF pages, photos and covers become a
  rule-layer image, as in PDF import - rendered from memory by `PdfImport::renderPage(data, ...)`,
  JPEG by default (PNG made the 13 MB sample 81 MB). Ink below a page's edge (Noteful keeps it) makes
  the page taller.
- A single `importNoteful()` returns tags rather than applying them (the archive import applies them).
  Noteful keeps them per page and as `#tags` in text boxes. Text boxes are not imported (counted in `Result::textBoxesSkipped`); a
  second outline entry on one page is dropped with a warning.
- The layer table is written into the document config node, so a directly saved `Document` keeps it.
- `pdfimport.cpp`'s catch blocks now call `fz_ignore_error()`: this MuPDF prints "UNHANDLED EXCEPTION!"
  at `fz_drop_context` for any caught error not cleared that way.

- **UI**: the document browser's bottom-right FAB row has an Import FAB (`ic_menu_import`, reicon
  `import2`) left of "+", whose ArrowPopup offers PDF and Noteful. Picking one ends the browser with
  `TagDocList::IMPORT_PDF`/`IMPORT_NOTEFUL` (the Open Whiteboard pattern) and `ScribbleApp::importFromBrowser()`
  runs `execDocumentList(CHOOSE_DOC, ...)` then the import: one PDF or notebook opens, an archive
  returns to the browser (tag index reloaded via `setRoot`) with a notice, failures in a message box.
  Handled in both `openOrCreateDocTagged()` and `execTagDocList(openResult)`. Hidden on iOS, whose picker
  is asynchronous and svg-only. A "Folders as tags" checkbox in the same popup sets `notefulFolderTags`
  (default on, remembered, not in Preferences); it is added with `addWidget`, not `addItem`, since a
  menu item closes the popup on release.
- Imported documents (PDF too) are saved with a thumbnail, `PdfImport::thumbnail()`: the first page drawn
  by the software painter, as `drawThumbnail` would for a document saved in the app - without it the
  browser showed generic file icons.
- To try the UI without touching your library: `tools/agent-display.sh run --debug -- --useTagDocList=1
  --libraryPath=<scratch dir that does not exist yet> --currFolder=<dir holding the files>/`. Debug builds
  keep their config in `syncscribble/Debug/write.xml` (not ~/.config), so back it up first; a pre-created
  empty library dir is rejected (no `.write-library` marker), which is correct.

Unknown: layer hidden/locked flags (only seen as 0), the per-segment f32 arrays on flag-2 strokes,
the layout of short pressure strokes (predicted, verified by alignment only), which of the pressure
range's two values is the top, text-box font sizes, the rotation convention for images, ellipses that are
not circles, freehand (non-straight) highlighter strokes and how they are flagged, and whether any template besides Grid, Ruled, Pattern and covers exists. New UI strings are
untranslated.
