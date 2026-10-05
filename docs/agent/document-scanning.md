# Document scanning

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
- `quaddetect.h`/`.cpp` - finds the page outline: downscale to ~480 px, Sobel with non-maximum
  suppression, gradient-directed Hough, then **score every left/top/right/bottom combination** of the
  strongest candidate lines and refit the winner. ~35 ms on a 4032x3024 photo. What is load bearing:
  - **No "outermost line" rule.** The first version took the outermost line of each orientation. That is
    right on a flat dark background and wrong on nearly every real photo - a table edge, plank seam,
    laptop, tablecloth or second sheet is always further out than the page - and it is why the user found
    detection "does not work at all" on the iPad. A candidate quad is scored instead by how much of each
    *side* (the segment between its corners, not the infinite line) is a page edge, minus a penalty if the
    edge runs on past the corners: page edges end at the page's corners, clutter lines do not.
  - **Polarity.** The accumulator covers 360 degrees, so a line's normal points to its brighter side, and a
    side only counts where its normal points into the quad. With 180 degrees the page edge and the outer
    edge of its own drop shadow, 2-3 px apart with opposite gradients, merged into one peak that sat on
    the shadow, and the side then found no support.
  - **Two tests per side sample**: an edge pixel within 2 px whose gradient points inward, *and* the pixel
    4 px inside brighter than the one 4 px outside. The second rejects ruled lines, text and pencil marks,
    which have edges of both polarities but the same paper on either side.
  - **Absolute edge threshold with a noise floor** (16, or 1.5x the median gradient), after thinning. The
    old percentile cut ("top 8%, never below 10% of the strongest") was set by handwriting or wood grain,
    and on a white or light desk it dropped the faint page outline entirely - those photos never
    detected at all.
  - The Hough angle alone is not accurate enough: half a degree across a 3500 px edge is ~160 px of
    corner error. Each side is refit by total least squares over the edge pixels **between its corners**
    (so a collinear table edge cannot pull it), **iterated three times** - one pass only sees pixels inside
    its own window and is biased toward the middle of the edge.

  `scribbletest/scantest.cpp` draws realistic photos procedurally (`makeScenePhoto()`: wood with seams,
  gingham, light and white desks, dark fabric; table edge, laptop, pen, second sheet, hand shadow, drop
  shadow, uneven light, noise, JPEG round trip). The old detector failed 5 of those checks. For tuning,
  the standalone build takes `SCANTEST_DUMP=dir` (writes each scene with expected/found corners) and
  `SCANTEST_ONLY=name`. On a randomized sweep of 100 such photos the old detector got 27 right, 35 wrong
  and 38 missed (every light/white desk missed); this one 97 right, 3 wrong (gingham stripes almost
  parallel to a page edge that borders a white square, and a pen touching a corner). Synthetic photos are
  not real ones: if real iPad photos still fail, save one and add it as a fixture.

Corner editing (`ScanCornerWidget`) is **relative, not absolute**: a touch anywhere on the photo grabs
the nearest corner (no hit radius), and the corner then moves `DRAG_RATIO` (0.4) of the finger's travel
from where it went down. The corner never jumps to the finger, so it is never hidden under it and can be
placed more finely than a fingertip can point. Making it follow the finger again undoes both.

Dialog layout: Back (arrow) and Cancel (cross) are in a header around the title, not the bottom row - the
template's `.window-title` node is moved into that row rather than recreated, so it keeps its theme
styling. The bottom row holds only the ways forward: Next on the corner step, **Add More** and Done on the
filter step. `cancelBtn` is still the dialog's cancel button, so Escape / Android back keep working.

**Add More** finishes with `ScanDialog::ADD_MORE`; `finishScan()` inserts that scan as usual and then calls
`captureScan()` again (the photo half of `scanDocument()`, split out so it does not reset the run). Each
follow-up goes right after the previous one: as a page at `lastScanPage + 1`, as an element stacked
below `lastScanRect` (`ScribbleArea::insertImage()` takes and returns that rect). `scanDocument()` resets
both, because a run abandoned in a mobile picker leaves them set. On desktop the next capture runs nested
inside the previous `finishScan()`; the `ScanDialog` (and its full size photo) is destroyed before it.

Where the photo comes from is per platform (`ScribbleApp::scanDocument()`); in every case a
`pendingScan` flag on `ScribbleApp` diverts it into `ScanDialog`:

- **Android** - `insertImage()`'s picker, which is already a chooser merging every
  `MediaStore.ACTION_IMAGE_CAPTURE` activity with the gallery (`MainActivity.getImage()`). The installed
  camera app does the capture, so this needs **no Play Services and no CAMERA permission**.
- **iOS** - `showScanImagePicker()` (`ios/ioshelper.m`): a Take Photo / Photo Library action sheet when
  `UIImagePickerController` reports a camera, the library directly when not (simulator). The camera
  picker cannot reach the library, hence asking first. On iPad the sheet is a popover and must be given
  an anchor, or presenting it throws. Camera photos arrive sideways with an EXIF orientation that
  stb_image ignores, so the picker delegate **redraws any non-`Up` image upright** before encoding - this
  also fixes rotated library photos for plain Insert Image. Needs `NSCameraUsageDescription` in both
  iOS plists, or iOS kills the app when the camera opens.
- **Desktop** - `Camera::list()` (`camera.h`); if it finds anything, `CameraDialog` shows a live preview
  with Capture, a camera combo when there are several, and Choose File... to fall back to the picker.
  No camera means the file picker, exactly as before. Backends, all with the largest frame size the
  camera offers, since a scan wants pixels:
  - Linux `linux/camera_v4l2.cpp` - raw V4L2 mmap streaming, MJPEG preferred over YUYV (bandwidth limited
    to a few fps at full size). Filters on `device_caps`, because a UVC camera exposes a second
    metadata-only `/dev/videoN`. Verified on an eMeet Nova: 1920x1080 MJPEG at ~26 fps, live preview,
    Capture into `ScanDialog`, and the device is closed again on both Capture and Cancel.
  - Windows `windows/camera_mf.cpp` - Media Foundation source reader asked for RGB32 with
    `MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING`, so it inserts the MJPEG/NV12 decoder itself. **Not yet
    compiled or run** - written without a Windows toolchain.
  - macOS `macos/camera_avf.m` (plain C interface, so no Objective-C++ rule) + `macos/camera_mac.cpp`.
    Needs `NSCameraUsageDescription` in `macos/Info.plist`, and the `com.apple.security.device.camera`
    entitlement once the app is signed with a hardened runtime. **Not yet compiled or run.**
  - everything else (wasm) compiles the stub in `camera.cpp`, which lists no cameras.

  **MJPEG trap:** UVC webcams may omit the Huffman tables (DHT) from each frame, and stb_image does not
  fail on that - it decodes noise. `decodeMjpegFrame()` inserts the standard Annex K tables when a frame
  has none (checked: without them stb's output differed from the original by 466832 summed byte values,
  with them by 0).

The `pendingScan` flag must be checked in **two** places: Android and iOS post an `INSERT_IMAGE` event,
but the desktop picker returns a filename and calls `insertImage(filename)` directly, never touching that
event - miss it and desktop silently inserts the raw photo. `insertImage()` clears the flag so a
cancelled picker cannot hijack a later plain image insert.

Output is either an element (`ScribbleArea::insertImage()`, unchanged) or a page background. The page
path follows the same convention as PDF import - image in `ruleNode`, `write-std-ruling` removed,
`write-no-dup` added, `isCustomRuling = true` - but goes through `ScribbleDoc::insertPage()` rather than
writing a file, so it gets undo and sync. Page size comes from the *current page's width* with the
height following the scan's aspect ratio: a scan has no physical scale (`UNITS_PER_POINT` is a PDF
points conversion and is meaningless here), and matching the document keeps scrolling and zoom consistent.

Known gap: a scan of ruled paper leaves `yRuling == 0`, so `Page::yruling(true)` falls back to
`BLANK_Y_RULING` and the ruled tools (insert space ruled, select ruled, ruled eraser) snap to a spacing
unrelated to the lines in the image. The detector's Hough pass already has the data to fix this.
