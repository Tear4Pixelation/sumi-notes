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

Corner editing (`ScanCornerWidget`) is **relative, not absolute**: a touch anywhere on the photo grabs
the nearest corner (no hit radius), and the corner then moves `DRAG_RATIO` (0.4) of the finger's travel
from where it went down. The corner never jumps to the finger, so it is never hidden under it and can be
placed more finely than a fingertip can point. Making it follow the finger again undoes both.

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
