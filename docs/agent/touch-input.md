# Touch input: fingers, pen and the stuck states between them

How a finger gets from the platform to `ScribbleInput`, and the places where it can be dropped. Written
after a report (most likely iPad + Pencil) that fingers are sometimes ignored completely and pinch zoom is
unreliable. Pinch and multi-touch cannot be driven in agent-display, so everything here comes from reading
the code, plus in-app checks that call `ScribbleInput` directly.

## The path

- **Platform → SDL queue.** iOS (`ios/ioshelper.m`, a category on SDL's view) and Android (`androidhelper.cpp`
  `jniTouchEvent`) push their own `SDL_FINGER*` events with `SDL_PeepEvents`. The pen is marked by
  `touchId == PenPointerPen/PenPointerEraser`; a cancel is `SVGGUI_FINGERCANCEL`. iOS finger ids are the
  `UITouch` pointers; the Pencil is always `fingerId = SDL_BUTTON_LMASK`.
- **`SvgGui::sdlTouchEvent` (ugui).** Fingers, not the pen, are tracked in `touchPoints`. One finger is sent
  as an ordinary `SDL_FINGER*` event with `fingerId` rewritten to `SDL_BUTTON_LMASK`. A second finger, **any
  cancel**, or any finger while `penDown` turns on `multiTouchActive`, and from then on until the last finger
  lifts every finger event goes out as one `SvgGui::MULTITOUCH` event carrying all of `touchPoints`. Most
  widgets ignore `MULTITOUCH`, so a tap on a toolbar button during that time does nothing.
- **`ScribbleWidget`'s handler** passes finger and `MULTITOUCH` events to `ScribbleInput::sdlEvent`, and
  `OUTSIDE_PRESSED` too (it carries the release that ended outside), and claims `pressedWidget` when a gesture
  starts.
- **`ScribbleInput::doInputEvent`** refuses touch outright while another source is mid-gesture
  (`isTouchAccepted()`: `currInputSource == TOUCH || scribbling == NOT_SCRIBBLING`). That is the gate every
  "fingers ignored" state below ends at.

## Fixed

### A cancelled pen stroke left every finger refused

iOS cancels a Pencil touch (`touchesCancelled`) for a system gesture, Control Center, a Pencil corner swipe
screenshot, the app resigning active; Android sends `ACTION_CANCEL`. SvgGui sends every cancel as
`MULTITOUCH`, whose points are the *fingers* - never the pen. `ScribbleInput` built it as a touch event, so it
was either dropped (no finger down: no points) or refused by `isTouchAccepted()` (the pen still "drawing").
The stroke was never cancelled: `scribbling` stayed `SCRIBBLING_DRAW` with `currInputSource == PEN`, so
**every finger was ignored until the pen touched down again**, and that next pen stroke carried on the
cancelled one (its press was swallowed as a repeated press, its moves extended the old stroke).

`ScribbleInput::sdlEvent` now recognises a pen cancel in the `MULTITOUCH` branch and calls `cancelAction()`.

Second half, in ugui: when the last touch point goes, `sdlTouchEvent` clears `pressedWidget` only
`if(!penDown)`, and clears `penDown` for the pen cancel only *after* that check. With no finger down, the pen's
press kept holding `pressedWidget`, so the next finger press anywhere - a toolbar button - was sent to the
canvas instead (one tap lost, and on the canvas it started a pan). The real fix is to move
`if(isPen) penDown = false;` above the `touchPoints.empty()` block in `ugui/svggui.cpp`; it is worked around
in `ScribbleInput` (clear `gui->pressedWidget` on a pen cancel with no fingers) so that this fix does not
need a ugui commit. Drop the workaround if ugui gets the reorder.

### A quick pinch was a two finger tap: zoom reverted and the last stroke undone

The two finger tap (see [navigation.md](navigation.md)) measured travel by the centroid only. A pinch moves
the fingers apart about a centroid that hardly moves, so any pinch finished within `MAX_CLICK_TIME` (500 ms)
qualified: `panZoomCancel()` threw the zoom away and `doTwoFingerTap()` undid a step. Now the change in the
distance between the two fingers (`pointerSpread`, accumulated by `addSpreadTravel`) counts toward
`pointerPathLen`, so a pinch is neither a tap nor a click. Only events with exactly two points contribute,
and the spread is reset at every press, so a third finger never adds a jump.

Both pinned by `twoFingerTapTest` ("a quick pinch") and `penCancelTest`, each confirmed to fail on the old code.

## Not changed - hypotheses for the device

Each of these is real code behaviour, but whether it is what the user hits needs the iPad. None was changed.

- **Pinch revert on a fast or close pinch.** `ScribbleView::panZoomMove` calls `scribbleInput->cancelAction()`
  when the fingers come closer than `touchMinPtrDist` (40) or the spread changes by more than 2x between two
  events. `cancelAction` → `panZoomCancel` restores the zoom *and pan* from the start of the gesture, and the
  rest of the gesture is dead (no further events act until a new finger lands). Seen as "pinch jumps back and
  stops". A 2x jump between consecutive events is plausible when a third finger lifts and `points[0..1]` become
  a different pair.
- **No zoom at all when touch draws.** With `singleTouchMode != PAN` (no pen ever detected, or touch set to
  draw), a pinch whose fingers start closer than `touchMinZoomPtrDist` (150 px) only pans. On an iPad never
  used with a Pencil, `penType` is still -1, so this is the default.
- **Palm threshold kills gestures.** iOS has `palmThreshold = 140` (touch diameter, `2*majorRadius`, times
  `inputScale`). Any finger event wider than that calls `cancelAction()`, reverting the current pan/zoom; with a
  resting palm that keeps reporting motion, every gesture is cancelled each time the palm moves. Note that
  the width tested for a `MULTITOUCH` event is that of the finger whose event it is, not the widest.
- **A resting palm joins the pinch.** Touches that land while the pen is down go into `touchPoints` and stay.
  After the pen lifts, one finger added beside a resting palm is a two point gesture: a pinch about the palm
  rather than a pan.
- **Window focus loss mid-gesture.** `SDL_WINDOWEVENT_FOCUS_LOST` clears `touchPoints` but not
  `multiTouchActive`; the next single finger is then sent as `MULTITOUCH`, so a toolbar tap right after is
  ignored once. Cancels arriving after the clear carry no points and never reach `ScribbleInput`, which then
  sits in `SCRIBBLING_PAN` until the next finger press (recovered there, but a pen press in between runs
  `panZoomCancel` and jumps the view back to where that pan started).
- **`touchMinPtrDist` / `touchMinZoomPtrDist` / `palmThreshold`** are prefs, so a device test can tune them
  without a build.

Dead code noticed: under `SDL_FINGER_NORMALIZED` in `sdlTouchEvent`, `event->tfinger.x *= h` should be `.y`;
nothing defines that macro.
