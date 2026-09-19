# Document scanning in Write — research & implementation plan

Goal: photo → drag 4 corners → flattened, cleaned-up page image, with **no Google Play
Services dependency**, on every platform Write ships to.

## TL;DR recommendation

**Do not take an external scanning dependency. Write it in C++ in `ulib`/`syncscribble`.**
The capture half already exists in the codebase; the missing half is ~600 lines of
well-understood math plus one ugui dialog. Everything then works identically on Android,
iOS, desktop and wasm, which is exactly the "the open source alternative for any device"
positioning.

---

## 1. Landscape of existing options

| Option | Play Services? | Verdict |
|---|---|---|
| ML Kit Document Scanner API | **Yes**, hard requirement — does not run on Huawei/de-Googled ROMs/GrapheneOS ([Google docs](https://developers.google.com/ml-kit/vision/doc-scanner/android), [Scanbot comparison](https://scanbot.io/blog/ml-kit-vs-opencv-document-scanning-software/)) | Ruled out by your constraint. Also Android-only, so it would never cover iOS/desktop. |
| Genius Scan SDK / Dynamsoft / Scanbot / Veryfi Lens | No, but **commercial + closed source** ([Genius Scan vs ML Kit](https://geniusscansdk.com/vs/ml-kit/)) | Ruled out — licensing conflicts with the project's identity. |
| OpenCV (`core` + `imgproc`) | No, Apache-2.0 | Technically viable, but a bad fit here — see §2. |
| Hand-rolled C++ in `ulib` | No | **Recommended.** |

### Existing open-source Android scanners
All the ones worth looking at are OpenCV-based Kotlin/Java apps, useful as *algorithm*
references rather than as libraries to vendor:
- [Document-Scanning-Android-SDK](https://github.com/AndroidMediaCodec/Document-Scanning-Android-SDK)
- [DocuScan](https://github.com/codegeasse1/DocuScan) — camera capture, auto-crop, filters, PDF export
- [abhisindh/document-scanner](https://github.com/abhisindh/document-scanner), [fbieberly/document_warp](https://github.com/fbieberly/document_warp) — minimal warp pipelines
- Write-ups: [GeeksforGeeks automatic document scanner](https://www.geeksforgeeks.org/computer-vision/automatic-document-scanner-using-opencv-opencv-document-scanner/), [Bret Hajek, scanning documents from photos](https://bretahajek.com/2017/01/scanning-documents-photos-opencv/)

### Why not OpenCV
- Write's build is a hand-written Makefile spanning 6 platforms (`Makefile.unix/.mac/.msvc/.ios/.wasm`).
  Bolting OpenCV's CMake build into that for all of them is days of work, permanently.
- Size: even a stripped `core+imgproc` build is ~1.5–3 MB **per ABI**; the full one is far worse.
  Write's whole APK is currently small — this would be the largest thing in it.
- You'd use maybe 5 functions: `getPerspectiveTransform`, `warpPerspective`, `Canny`,
  `findContours`, `approxPolyDP`. The first two are ~120 lines of C++ total. The last three are
  only needed for the *optional* auto-detect step (§5).
- Ratio of dependency cost to code saved is terrible. Skip it.

---

## 2. What the codebase already gives you

Good news — the hard platform-integration part is done.

**Capture (Android, already Play-free):**
`android/app/src/main/java/com/styluslabs/writeqt/MainActivity.java:434` — `getImage()` already
builds a chooser merging every `MediaStore.ACTION_IMAGE_CAPTURE` activity on the device with a
gallery `ACTION_GET_CONTENT`, writes to `getExternalCacheDir()/_camera.jpg`, and hands out the
URI via `FileProvider` (authority `com.styluslabs.writeqt.fileprovider`, already declared in
`AndroidManifest.xml`, along with the API-30+ `<queries>` block for `IMAGE_CAPTURE`).
This uses only the framework camera intent — **zero Play Services, zero CAMERA permission**
(the intent delegates to whatever camera app the user has). Nothing to change here.

**Image ingestion:**
`android/androidhelper.cpp:209` decodes to an `Image` and calls
`ScribbleApp::insertImageSync()`, which marshals onto the GUI thread via
`SvgGui::pushUserEvent(scribbleSDLEvent, INSERT_IMAGE, ...)` → `ScribbleApp::insertImage(Image)`
(`scribbleapp.cpp:2160`) → `ScribbleArea::insertImage()` → `new Element(new SvgImage(...))`.
**A scan is just an `Image`**, so once warped it drops into the page through the existing path.

**Image primitives (`ulib/image.h`):**
`Image` is plain 32-bit RGBA with `pixels()`, `scaled()`, `cropped()`, `transformed()`,
`encodeJPEG()`. Decoding is stb. Everything you need to read/write pixels is there.

**Threading:** `ulib/threadutil.h:50` has a `ThreadPool` — use it to tile the warp.

**UI:** ugui `Dialog` (`ugui/widgets.h:309`) + `createDialog()` + `SvgGui::showModal()`.
Draggable-handle precedent: `selection.h:120-122` (`scaleHandleHit`/`rotHandleHit`/`cropHandleHit`
with separate touch hit radii) — copy that hit-testing idiom for the corner grips.

### The one real gap
`Transform2D` (`ulib/geom.h:122`) is **affine only** — `real m[6]`. `Image::transformed()`
routes through `Painter`/nanovg, which is also affine. Perspective correction needs a 3×3
homography, so it cannot reuse either. This is the main piece of new infrastructure.

---

## 3. Proposed architecture

Five pieces, each independently testable.

### A. `ulib/homography.h` — `Transform3D` (~120 lines)
- `real m[9]`, `Point map(Point)` with the `w` divide, `inverse()`, `operator*`.
- `static Transform3D quadToQuad(const Point src[4], const Point dst[4])`:
  build the 8×8 system from the 4 correspondences, solve by Gaussian elimination with
  partial pivoting. Standard closed form; no linear-algebra dependency.
- Unit-testable with no GL — good fit for `scribbletest/`.

### B. `ulib/imagewarp.cpp` — `Image warpPerspective(const Image&, const Transform3D&, int w, int h)` (~100 lines)
- **Inverse mapping**: iterate destination pixels, map back into the source, bilinear sample.
  Never forward-map (leaves holes).
- Incremental evaluation along each scanline (homography is linear in numerator and denominator)
  → 2 adds + 1 divide per pixel, no per-pixel matrix multiply.
- Tile rows across `ThreadPool`. **Measured** on a 12-thread desktop, 4000×3000 → 2200×1509:
  478 ms single-threaded, **84 ms threaded**. A mid-range phone should land in the low hundreds
  of ms threaded; it runs exactly once per scan, so that is fine — but the pool is not optional.
- Box-supersamples when downscaling (grid width chosen from the source/dest area ratio, capped
  at 4×4), otherwise plain bilinear aliases badly on fine text. The 4000×3000 case above picks
  2×2, which is where most of that time goes.
- Deliberately **CPU, not GL**: nanovg paints are affine, so a GL path would need a custom shader
  and FBO readback — more code and more platform risk for a one-shot operation.

### C. `ulib/imageenhance.cpp` — the "make it look scanned" filters — **done**
This is what separates a photo from a scan, and it's cheap. **Measured** at 2200×1509, single-threaded:
`SCAN_COLOR` 119 ms, `SCAN_GREYSCALE` 72 ms, `SCAN_MONO` 73 ms — with a 183 px blur radius, which
costs no more than a 5 px one because of the integral image. No thread pool needed here.
1. **Illumination flattening** — divide by a heavily blurred copy of the image:
   `out = clamp(255 * src / (blur(src) + eps))`. Kill shadows and uneven desk lighting in one step.
   Implement the large-radius blur with a **summed-area table** (integral image) so cost is
   independent of radius.
2. **Level stretch** — histogram percentiles (e.g. 2% / 98%) → linear remap to 0..255.
3. Modes, as a segmented control in the dialog:
   - *Original* (just warp)
   - *Color document* — 1 + 2, keeps colour
   - *Greyscale* — 1 + 2 on luma
   - *B&W / text* — 1 + 2 + adaptive (mean-of-local-window) threshold. Compresses to almost
     nothing as PNG, which matters for `.svgz` document size.
4. Also expose **rotate 90°** — users hold phones sideways constantly.

### D. `syncscribble/scandialog.cpp/.h` — the corner-adjust UI (~350 lines)
Modeled on the existing `rulingdialog.cpp`/`configdialog.cpp` pattern.
- Modal `Dialog` holding a custom `Widget` that draws a fit-to-screen preview of the photo,
  the quad outline, and 4 corner grips.
- Drag handles with a generous touch radius (follow `selection.h`'s `bool touch` convention).
- **Magnifier loupe**: while dragging, show a zoomed inset of the area under the finger, offset
  so the finger doesn't cover it. Non-negotiable for usability on touch — it's the difference
  between "usable" and "infuriating".
- Constrain the quad to stay convex and inside the image bounds.
- Live preview of the flattened result at low resolution (e.g. 600 px long edge) as corners move —
  cheap with the tiled warp, makes the interaction feel immediate.
- Buttons: Rotate, filter mode, Cancel, Done.

### E. Output sizing & placement
- Estimate the output aspect ratio from the quad. Simplest honest approach: take the average of
  the two opposite-edge-pair lengths. (The "recover true aspect from focal length" method exists
  but is fragile without EXIF FOV — offer a **snap-to-paper-ratio** control instead: Auto / A4 /
  Letter / Square.)
- Cap the long edge (~2200 px) and encode JPEG for colour modes, PNG for B&W.
- Then just call the existing `ScribbleApp::insertImage(Image)`.
- Worth considering as a follow-up: an "insert as page background" variant, since a scanned page
  you want to annotate is conceptually a page, not a floating picture.

---

## 4. Wiring per platform

| Platform | Capture | Work needed |
|---|---|---|
| Android | `MainActivity.getImage()` — **already done, Play-free** | Route result into the scan dialog instead of straight to `insertImage` |
| iOS | existing image picker in `ios/` | Same routing |
| Desktop | existing file picker | "Scan from file" — same dialog, zero extra code |
| wasm | file input | Same |

Concretely: add a `ScribbleApp::insertScan()` command that sets a flag so the next
`imagePicked()` callback opens `ScanDialog` rather than inserting directly. A **new menu entry
"Scan document…"** next to "Insert image". That's the whole integration surface.

---

## 4b. Output destination — both paths

Decided: implement **both** in the backend, then expose "scan as page" and "scan as element".

An important finding for the page path: **Write draws no ruling at all on imported PDF pages**, so
there is no conflict between Write's lines and lines printed on the page.
`PdfImport::importPdf()` sets `xRuling`/`yRuling`/`marginLeft` to 0 and puts the image in `ruleNode`
above the `.pagerect`. What keeps that stable is `isCustomRuling` — see `Page::onPageSizeChange()`
(`page.cpp:137`), where `!isCustomRuling` would call `generateRuleLayer()` and throw the image away
on every page-size change. `isCustomRuling` is re-derived on load from the *absence* of the
`write-std-ruling` class, which is why `pdfimport` removes it and adds `write-no-dup`.

There is a real mismatch though, and it bites scans harder than PDFs. With `yRuling == 0`,
`Page::yruling(true)` falls back to `BLANK_Y_RULING`, and that default feeds `getLine()`,
`getYforLine()` and `cmpRuled()` — so the *ruled-mode* tools (insert space ruled, select ruled,
ruled eraser) snap to a spacing unrelated to the lines visible on the page. On a scan of ruled
notebook paper that is visibly wrong: you would write between the scanned lines, not on them.

Proposed follow-up (not yet built): the detector already runs a Hough transform, so recovering the
dominant horizontal line spacing inside the quad is nearly free — set `props.yRuling` from it so
ruled mode lines up with the scanned paper. Deliberately left out of the detector for now to keep
that step to what was agreed.

## 5. Auto corner detection — **done**

Built as `ulib/quaddetect.h/.cpp` — no OpenCV, ~380 lines. **Measured**: 19 ms on a 4000×3000 photo,
with every corner landing within 11 px of truth (0.3% of the image).

What it does, in order: box-downscale to a 480 px long edge (detection wants the page edge to
dominate, not detail), 3×3 blur, Sobel, keep the strongest edge pixels, **gradient-directed Hough**
(each edge pixel votes only for orientations near its own gradient — far cheaper than voting for all
of them, and much less prone to inventing lines out of texture), pick peaks, take the *outermost*
near-horizontal and near-vertical line of each pair, then intersect.

Two things were not obvious and are worth keeping:

- **The edge threshold needs a floor, not just a percentile.** A page outline is only ~1% of the
  pixels, so "keep the top 8%" reaches down into the flat interior — and flat pixels have gradient
  (0,0), whose `atan2` is 0. Every background pixel then votes for the same orientation, burying the
  real edges under invented lines. First version found 184 lines, all bogus.
- **The Hough angle is not accurate enough on its own.** Half a degree of quantization across a
  3500 px edge is tens of pixels of corner error. Each of the four lines is refit by total least
  squares over its supporting pixels, **iterated three times** — one pass is not enough, because the
  first fit only sees pixels within its window and is biased toward the middle of the edge. This
  took worst-corner error from 160 px to 11 px.

Still deliberately **not** recommended: real-time corner tracking in a live camera preview, which is
what ML Kit and the commercial SDKs do. That means replacing the camera intent with a full in-app
CameraX/AVFoundation stack — a large, very device-dependent surface
([one SDK vendor's account](https://geniusscansdk.com/blog/2026/06/production-camera-stack-edge-cases))
— for a marginal gain over "shoot, then adjust".

---

## 6. Suggested sequencing

1. ~~`Transform3D` + `quadToQuad` + unit tests.~~ **Done** — `ulib/homography.h/.cpp`.
2. ~~`warpPerspective`, threaded.~~ **Done** — `ulib/imagewarp.h/.cpp`.
3. ~~`ScanDialog` with handles + loupe + preview.~~ **Done** — `syncscribble/scandialog.h/.cpp`.
4. ~~Enhancement filters.~~ **Done** — `ulib/imageenhance.h/.cpp`; the mode selector is part of step 3.
4c. ~~Auto corner detection.~~ **Done** — `ulib/quaddetect.h/.cpp` (moved forward from phase 2).
5. ~~Menu entries, routing, both output paths.~~ **Done** — "Scan Document..." and "Scan Document as
   Page..." next to Insert Image (no toolbar button; a unified add-page button is planned separately).
   Scanning reuses `insertImage()`'s picker on every platform, with a `pendingScan` flag in
   `ScribbleApp` diverting the picked photo into `ScanDialog`.

**Not yet verified:** the dialog compiles, links and starts, and the scan math is unit tested, but the
dialog itself has never been driven by hand — corner dragging, the loupe, the filter preview and both
output paths are all unexercised. `ScanCornerWidget` is also the first `CustomWidget` subclass in the
tree, so that API path has no prior user to copy from.

All of the above is tested in `scribbletest/scantest.cpp`, which runs without GL or a document — so it
builds and runs standalone in CI (see the command at the top of that file), and is also called from
`ScribbleTest::runAll()` and counted in its exit code.
6. *(later)* auto corner detection.
7. *(later)* multi-page: keep the dialog open, append scans to consecutive pages.

Phase 1–5 is roughly a week, adds **zero** dependencies and **zero** APK bloat, and lands on
every platform at once.

---

## Sources
- [ML Kit document scanner (Android)](https://developers.google.com/ml-kit/vision/doc-scanner/android)
- [ML Kit vs OpenCV for document scanning — Scanbot](https://scanbot.io/blog/ml-kit-vs-opencv-document-scanning-software/)
- [Genius Scan SDK vs ML Kit](https://geniusscansdk.com/vs/ml-kit/)
- [A Production Camera Stack Is Mostly Edge Cases — Genius Scan](https://geniusscansdk.com/blog/2026/06/production-camera-stack-edge-cases)
- [Automatic Document Scanner using OpenCV — GeeksforGeeks](https://www.geeksforgeeks.org/computer-vision/automatic-document-scanner-using-opencv-opencv-document-scanner/)
- [Scanning Documents from Photos Using OpenCV — Bret Hajek](https://bretahajek.com/2017/01/scanning-documents-photos-opencv/)
- [Document-Scanning-Android-SDK](https://github.com/AndroidMediaCodec/Document-Scanning-Android-SDK)
- [DocuScan](https://github.com/codegeasse1/DocuScan)
- [abhisindh/document-scanner](https://github.com/abhisindh/document-scanner)
- [fbieberly/document_warp](https://github.com/fbieberly/document_warp)
