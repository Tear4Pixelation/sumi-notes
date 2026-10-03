# Agent display in detail

## Two displays, and which one each command uses

cage provides both a Wayland socket **and** its own Xwayland server, and the
split is not cosmetic:

| | display | used for |
|---|---|---|
| pointer, screenshots, video | `WAYLAND_DISPLAY` (cage's socket) | `agent-pointer`, `grim`, `wf-recorder` |
| keyboard | `DISPLAY` (cage's Xwayland, e.g. `:2`) | `xdotool` |

Keyboard goes through X because **Write runs as an X11 client inside cage**:
forcing `SDL_VIDEODRIVER=wayland` segfaults it (verified, exit 139), so SDL falls
back to x11. `wtype` speaks the right Wayland protocol and works on native
Wayland clients in the same cage (verified against `foot`), but its
virtual-keyboard keymap does not survive the Xwayland translation, so it silently
does nothing to Write. Pointer events do survive that translation, which is why
the two halves use different mechanisms.

Both displays are private to the nested session, so neither can reach the user's
niri session. `in_session_x()` refuses to run if the recorded X display is empty
or implausible, because falling back to the inherited `DISPLAY` would type into
the user's real windows - the one failure this whole setup exists to prevent.
For the same reason, never substitute `ydotool`/`dotool`: they inject through
kernel `uinput`, which is seat-global, and a headless cage
(`WLR_LIBINPUT_NO_DEVICES=1`) would never see them while niri would.

## Things that bite

- **A click must be one invocation.** `agent-pointer` creates a virtual pointer
  and destroys it on exit, and the cursor position does not survive that, so a
  separate `move` then `click` clicks wherever the cursor defaulted to. The
  symptom is confusing: canvas drags work (one invocation) while toolbar buttons
  appear to ignore clicks entirely. Hence `click X Y`, never `move` then `click`.
- **`wlrctl` cannot express a drag.** It offers only relative motion and an
  atomic click, so there is no way to hold a button down - and in a drawing app
  every stroke is a press, a path, and a release. That is why
  `tools/agent-pointer/` exists instead; it also does absolute motion, so callers
  pass coordinates read straight off a screenshot.
- **The document list opens over the canvas on a fresh start.** Strokes sent
  before dismissing it go to the overlay and nothing appears to happen. Close it
  (the X at the top right) before drawing.
- **Killing cage does not kill Write.** A Write that outlives its compositor
  keeps running headless forever, holding its document. `stop` reaps it by exact
  process name - `-x 'Sumi|Write'`, never `pkill -f .../Release/Sumi`, which would also
  match the command line of the shell doing the killing.
- The headless output is 1280x720 (wlroots' default); cage exposes no way to
  change it. Use headless `sway` if a specific size matters.
- `--test` works here: the headless backend renders on the GPU via
  `/dev/dri/renderD128`, so it is a real GL run, not llvmpipe. Result as of this
  setup: **0 failed tests, 0 failed unit checks, 16 of 17 failed thumbnails.**
  Read that exit code carefully - `ScribbleTest` exits with the *failed thumbnail
  count*, so a clean run of the checks that matter still exits non-zero. The
  thumbnail comparison renders pixels and differs across GPUs and drivers;
  `scribbletest.cpp:470` says as much ("we've had so many problems with
  thumbnails"), and 16 of 17 failing uniformly is that, not 16 regressions - the
  document content matched for every fixture, only the embedded thumbnail image
  differed.
- **Leak detection is off for `test`.** ASan reports ~2.2 MB leaked entirely
  inside NVIDIA's GL driver and libdbus, and worse, its exitcode (1) masks the
  thumbnail count. `AGENT_DISPLAY_ASAN_LEAKS=1` restores it.
- **`move` produces no hover.** It creates a virtual pointer, moves it and destroys it at once, and the
  app never sees the motion. To test hover (e.g. page tags riding on the pointer), keep a pointer alive:
  `WAYLAND_DISPLAY=wayland-0 XDG_RUNTIME_DIR=/run/user/1000 timeout 3 tools/agent-pointer/agent-pointer hold X Y &`,
  then `shot`. `record` produced an empty file here; a loop of `shot`s comes out about 60 ms apart, enough
  to see a blink.
