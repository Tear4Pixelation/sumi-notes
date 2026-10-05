# Moving around the canvas

Five ways to move the view, all of which already existed in some form; what follows is mostly a
record of why they did not work.

| gesture | what it does |
|---|---|
| mouse wheel | scroll up/down |
| Shift + wheel | scroll left/right |
| Ctrl + wheel | zoom about the pointer |
| middle mouse button drag | pan, whatever the active tool is |
| Pan tool (hand icon) | pan with an ordinary left drag |
| two fingers, or one finger once a pen has been used | pan/zoom |

- **The wheel did nothing at all on Linux, and the cause is not in Write's own logic.** `SvgGui`
  read only `SDL_MouseWheelEvent`'s integer `x`/`y`. Under **sdl2-compat** - the SDL2 API
  reimplemented on top of SDL3, which Arch and others now ship in place of SDL2 - those arrive as
  **0** and the scroll is reported solely in `preciseX`/`preciseY`. `SDL_GetVersion` reports
  something like 2.32.72, so nothing looks wrong. `sdlMouseEvent()` now prefers the float fields and
  falls back to the integers, which is also the better reading on genuine SDL2: they are the only
  fields carrying sub-notch touchpad resolution, which is the very thing the `*= 120` was added to
  preserve.
- **Never write `SDL_Event event = {0};` - write `= {}`.** For a union, `{0}` only initializes the
  first member (the 4-byte `type`), and since GCC 15 the rest really is left as stack garbage. On Linux
  the wheel events come from `linuxtablet.c` (XI2 buttons 4-7; SDL's own are disabled), which built them
  this way, so `preciseX`/`preciseY` - which the line above now *prefers* - held garbage: the wheel did
  nothing, or overflowed `wheel.y` and jumped to the end of the document. The Linux-built files were
  converted; the per-platform helpers (Android, iOS, macOS, Windows, wasm) still use `{0}` and are
  built by clang/MSVC, but are the same latent bug under GCC 15+.
- **`wheelModifiers()` is the single place the wheel's modifiers come from.** `wheel.direction >> 16`
  is a *Windows-only* convention: only there does Write synthesize the wheel event itself
  (`winhelper.cpp`) and pack the modifiers into those bits. Two callers read them unconditionally,
  and off Windows those bits are `SDL_MOUSEWHEEL_NORMAL`/`FLIPPED` - **and `FLIPPED` is 1, which is
  `KMOD_LSHIFT`**, so this did not merely disable Ctrl+wheel zoom and Shift+wheel, it could invent a
  Shift nobody held. The second caller was `ScrollWidget`'s event filter, where the same expression
  made *every* wheel event look unmodified, so a scroll container always swallowed the wheel instead
  of letting a modifier pass it to the widget underneath.
- **Modifiers are tracked from the key events, not from `SDL_GetModState()`** (`SvgGui::keyModState`,
  updated in `SvgGui::sdlEvent`). SDL updates its modifier state when events are *pumped*, while a
  widget reads it when the event is *dispatched*; a modifier released in the same pump as the wheel
  event therefore reads as already up. This was observed, not theorised: Ctrl+Z worked in the same
  session in which Ctrl+wheel scrolled instead of zooming. `keysym.mod` already accounts for the key
  in hand, so a press sets the bit and a release clears it; window focus changes resync from
  `SDL_GetModState()`, since key releases that happened elsewhere were never delivered here.
- `wheelScrollSpeed` is pixels-per-notch over 120 and was 0.2, i.e. 24 px a notch - slow enough to
  read as broken on its own. Now 0.5. Both it and `wheelZoomSpeed` are exposed in the Input prefs.
- **Middle-button pan is a flag on `ScribbleInput`, not a `MODEMOD` bit** (`midBtnPan`), because it
  must survive from the press to the release while `modemod` is rebuilt per event. It joins
  `mouseMode == INPUTMODE_PAN` in the one condition that starts `SCRIBBLING_PAN`, and it also has to
  suppress the early return for `mouseMode == INPUTMODE_NONE` - otherwise the one configuration where
  a pan gesture is most wanted is the one where it is dropped. `fingerId` is the button for down/up
  and the full mask for motion, so `SDL_BUTTON_MMASK` is only ever tested on down and up.
- **The Pan tool already existed and was simply hidden**: it was added to the toolbar only when
  `singleTouchMode` *and* `multiTouchMode` were both 0, i.e. only when touch was disabled entirely.
  That left a stylus-only user - no second finger, no middle button - with nothing but the scroll
  handle. It is now always present. It is not one of the tools (it edits nothing), so it keeps its
  own slot ahead of the tools sub-toolbar and its own `NormalPriority + 1`, which drops it into the
  overflow menu before the tools row is touched on a narrow screen.
- **Finger-pans-while-pen-writes on mobile needed no work** - `ScribbleInput::doInputEvent()` already
  switches `singleTouchMode` (and `mouseMode`) to `INPUTMODE_PAN` the first time a pen event arrives,
  and persists it via `penType`. Both are also exposed in the Input prefs.
- Space+drag, Ctrl+Shift+drag and pan-from-edge remain as they were.

## Two finger tap = undo

`ScribbleInput::doInputEvent` recognises it: exactly two touch points (`twoFingerTap`, set when the second
lands and cleared by a third), under `PANLENGTH_CLICK` of centroid travel and `MAX_CLICK_TIME` from the
second finger down to the gesture finishing. The pan the fingers started is **cancelled, not finished** -
`panZoomFinish` would treat it as a click (following a link under the centroid) or start a fling. With
`singleTouchMode == PAN` the gesture only finishes when the last finger lifts, so the first lift resets
`prevPointerCOM` to the remaining finger, or the centroid jump alone would exceed the click distance.
`ScribbleView::doTwoFingerTap()` is the hook; only `ScribbleArea` acts on it (one `ID_UNDO`).
Pinned by `twoFingerTapTest`.

## Testing this (agent-display)

Three gaps in `tools/agent-display.sh` had to be closed before any of the above could be checked, and
they are worth knowing about:

- **`scroll` sends `axis_discrete`, not just `axis`.** Xwayland turns discrete steps into X11 buttons
  4-7; a bare `axis` event is a continuous scroll and an X11 client - which Write is, inside cage -
  never sees it. `scroll` also takes an optional `X Y` first, for the same reason `click` does.
  `DY` is in wl axis units, so **negative scrolls up**.
- **`drag` takes a trailing button name**, so middle-button pan can be exercised at all.
- **`keydown`/`keyup`** exist so a modifier can be *held* across other commands; `key` sends a whole
  chord and cannot express Ctrl+wheel. Always pair them.
- **Do not drive the wheel with `xdotool` while agent-pointer is also in use.** Each agent-pointer run
  creates and destroys a virtual pointer, and this Xwayland miscounts the resulting enter/leave
  (`BUG: xwl_seat->pointer_enter_count == 0` in the session log). Once that fires, XTEST has no
  pointer to drive: keystrokes still arrive but wheel and motion silently do not, and the app looks
  broken when it is fine. Drive the wheel through `agent-display.sh scroll` and use `xdotool` only
  for keys. A `hold X Y` action exists on `agent-pointer` to park a persistent pointer, but it
  *suppresses* the transient pointers, so it is not a workaround - restart the session instead.
