# Tests in detail

The document-scan math is tested in `scribbletest/scantest.cpp`. Unlike the rest of `scribbletest` it
needs neither GL nor a document, so it also builds and runs standalone - see the command in that file's
header (it needs `-DNDEBUG`, or `geom.cpp`'s `ASSERT` pulls in `platform_assert` from the application).
`ScribbleTest::runAll()` calls `runScanTests()` and counts its failures in the result string and exit
code. The dialog itself has no automated coverage. Quad detection is checked twice: on plain bright-quad
images (`testQuadDetect()`) and on procedurally drawn realistic photos (`testQuadDetectRealistic()` -
clutter, shadows, low-contrast desks, noise, JPEG), the latter added because the plain images passed
while detection failed on real iPad photos. The old outermost-line detector fails 5 of the realistic
checks; see [document-scanning.md](document-scanning.md) for the tuning env vars.

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
- `ScribbleTest::shapeTapEditTest()` - with touch panning (a pen in use), a finger tap on a shape selects
  it with handles and keeps the tool, a tap on handwriting or empty page selects nothing / clears, the
  first shape-tool drag outside a selection only deselects, a pen tap selects nothing, and the two finger
  tap still undoes. See shapes.md, "Into and out of edit mode".

The palette math is tested the same way again in `scribbletest/colortest.cpp` (`runColorTests()`), also
standalone-buildable - see the command in its header. `./colortest --dump` prints the golden table in the
form pasted into `colortest_golden.inc`, which is only ever correct to do while a generator has never
shipped. Its checks are the generator freeze, the legibility invariant over ~1700 generated inks, sRGB
gamut, determinism, snap idempotence, `matchReference` injectivity, unknown-generator fallback, and the
OKLCh measurements `COLORS_SPEC.md` argues from (the Schuler palette, and yellow being 1.1:1 at its
cusp) - so if the spec and the code ever diverge, the tests say so.

The thickness preset lists ([pen-and-tools.md](pen-and-tools.md#relative-pen-width)) are tested in
`scribbletest/widthpresettest.cpp` (`runWidthPresetTests()`), standalone-buildable - see the command in its
header. Its checks are the `rel:`/`abs:` round trip (and a legacy list reading as unit-unknown), a relative
preset read and written in document units for a selection, conversion both ways, and `normalize()`:
clamping into the spinbox limits, refilling two presets to three, dropping a fourth, replacing NaN.
Mutation-checked: storing a selection's width raw, returning presets raw, no clamp, no refill, no
truncation, the old `%.3f` format and dropping the prefix each fail it. `ScribbleTest::penWidthPresetTest()`
drives the real `PenToolbar`: a relative pen's preset applied to a selected 3.2 unit stroke, a width
edited in the selection's preset editor landing back in line heights, and a 144 line height pen clamped by
`loadModes()`. Its toolbar checks fail three ways against the pre-fix toolbar, and the load check fails
with the clamp removed.

The page tag summary and the tag index's document cache are tested in `scribbletest/pagetagtest.cpp`
(`runPageTagTests()`), standalone-buildable - see the command in its header. Its checks are the
`pagetags` round trip (a title holding every separator and XML character), that the stored form needs no
XML escaping, malformed and tagless records, and that the index keeps a DOC line whose last fields are
empty (the getline split dropped them, so untagged documents were reread on every refresh) while a
pre-page-tag four-field line is a miss. All three kinds were confirmed to fail against the broken code.
`testFilterMatchOrder()` pins the browser's tag filter: a subtag (and a grandchild) meets its supertag but
not exactly, and below the separator exact matches sort before top-level ones in otherwise folder order -
it fails with `partialMatchBefore()` returning false (the old, unordered grid).

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

- `ScribbleTest::pageTagTest()` - page tags placed with the pen through real input events: a press in the
  gap between pages, beside the page and below the last page each leave the tag inside a page; a pen
  placement comes up selected; dragging that selection onto the next page moves the page's tag count there,
  and undo/redo move it back and forth; opening another document drops tags still on the pointer. It zooms
  to 0.35 (and restores the zoom) so both pages are on screen for the drag. The placement and reset checks
  were confirmed to fail with the fixes disabled; the drag checks pin behaviour that was already right.

- `ScribbleTest::currentPageTest()` - the current page is the one taking up most of the view, in the
  vertical and the horizontal layout ([navigation.md](navigation.md#the-current-page-follows-the-page-taking-up-most-of-the-view)):
  30/70 and 70/30 splits, no switch within the 2% margin in either direction, `gotoPos` keeping its page
  where the next page shows more, and a page drawn on staying current until the view moves.
  Mutation-tested: the old rule, no margin, no hold and re-choosing on an unmoved view each fail it.

- `ScribbleTest::docStateSyncTest()` - the layer table and the theme as undo steps and on the sync
  wire. Each edit is checked three ways: it is one undo step, undo/redo restore it, and what it
  serializes reproduces the edit when fed back through `ScribbleSync::processItem()` on a bare
  `ScribbleSync` - the receive path a peer runs, needing no server - after the edit is undone
  locally. Also pins the joiner snapshot, a peer's hide moving our pen, and a peer's stroke on a
  layer we have hidden arriving hidden. Mutation-tested: dropping the re-add visibility, not
  recording the theme change, and ignoring a received removal each fail it.
- `ScribbleTest::pdfImportTest()` - import memory ([pdf-import.md](pdf-import.md#memory-shared-with-noteful-import)),
  on a three-page PDF written inline (no xref - MuPDF repairs it, printing warnings). A rendered page
  is held encoded only (`data == NULL`), its blue square lands blue where the PDF put it (a swapped
  channel in the RGB-direct encoder makes it red), a crop renders just that part with the content in
  place, a 40 MB budget gets a smaller page and counts it, a spent budget still gets the page at 72 DPI
  rather than dropping it, and `ImportSaver` leaves every written page unloaded while the file reloads
  with all three backgrounds. Mutation-tested: keeping pages decoded, never lowering the DPI, never
  unloading, and capping MuPDF at only what is left each fail it.

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

`scribbletest/nightmodetest.cpp` covers night mode's image handling standalone: which images are detected as
documents (worksheet, transparent clip) and which are left alone (photo, saturated pastel), that a declined
image is untouched byte for byte, and that the flip lands paper on the ink-drawn paper and keeps a blue
heading blue. See [night-mode.md](night-mode.md).


- `ScribbleTest::pageMoveTest()` - the sidebar's Pages view backend ([page-management.md](page-management.md)):
  `movePages` puts pages after the target (or at the front / end), keeps a multi-page move in document order,
  refuses no-ops and drops onto a moved page, keeps a page's outline entry, and is one undo step; `deletePageList`
  of a few pages and of every page is one undo step. Mutation-checked (insert before the target: 12 failures;
  reversed multi-page order: 1).
