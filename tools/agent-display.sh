#!/usr/bin/env bash
# agent-display.sh - run Sumi in an isolated compositor so an agent can drive it
# without touching the user's real session.
#
# Two separate isolated displays are involved, and which one a command uses is
# not cosmetic (see CLAUDE.md, "Agent display"):
#
#   WAYLAND_DISPLAY  cage's own Wayland socket  -> screenshots, video, pointer
#   DISPLAY          cage's own Xwayland server -> keyboard
#
# Both are private to the nested session, so neither can reach the user's niri
# session. Do NOT replace either with ydotool/dotool: those inject through kernel
# uinput, which is seat-global, and would land in the user's windows instead.
#
# Keyboard goes through Xwayland because Sumi is an X11 client here - SDL's
# Wayland video driver segfaults it (verified: exit 139), so SDL falls back to
# x11, and wtype's virtual-keyboard keymap does not survive the Xwayland
# translation. xdotool talks to cage's own X server and does work.
#
# Usage:
#   agent-display.sh run [--windowed] [--debug] [-- <app args>]
#   agent-display.sh test                 # ./Debug/Sumi --test
#   agent-display.sh shot [out.png]
#   agent-display.sh click <x> <y> [left|right|middle]
#   agent-display.sh move <x> <y>
#   agent-display.sh drag <x1> <y1> <x2> <y2> [<steps>] [left|right|middle]
#   agent-display.sh stroke <x1> <y1> <x2> <y2> [<x3> <y3> ...]
#   agent-display.sh scroll [<x> <y>] <dy> [<dx>]   (wheel notches)
#   agent-display.sh type <text>
#   agent-display.sh key <keyname>        # e.g. ctrl+z, Escape, Return
#   agent-display.sh record start|stop [out.mp4]
#   agent-display.sh status
#   agent-display.sh stop

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP_DIR="$REPO_ROOT/syncscribble"
POINTER="$REPO_ROOT/tools/agent-pointer/agent-pointer"
RUNDIR="${XDG_RUNTIME_DIR:-/tmp}/write-agent-display"
DISPLAY_FILE="$RUNDIR/wayland-display"
XDISPLAY_FILE="$RUNDIR/x-display"
PID_FILE="$RUNDIR/compositor.pid"
REC_PID_FILE="$RUNDIR/recorder.pid"
APP_PID_FILE="$RUNDIR/app.pid"
LOG_FILE="$RUNDIR/session.log"
SHOT_DIR="${AGENT_DISPLAY_SHOT_DIR:-$RUNDIR/shots}"

# The headless output is wlroots' default 1280x720; cage exposes no way to set a
# mode. Use headless sway (swaymsg output ... mode) if a different size matters.

mkdir -p "$RUNDIR" "$SHOT_DIR"

die() { echo "agent-display: $*" >&2; exit 1; }

need() { command -v "$1" >/dev/null 2>&1 || die "missing '$1'. $2"; }

session_display() {
  [[ -s "$DISPLAY_FILE" ]] || die "no agent display running; start one with: $0 run"
  cat "$DISPLAY_FILE"
}

# Run a Wayland client against the agent session rather than the user's.
# -u must precede the assignment; `env VAR=x -u DISPLAY cmd` would run `-u`.
in_session() {
  env -u DISPLAY WAYLAND_DISPLAY="$(session_display)" "$@"
}

# Run an X client against cage's own Xwayland server.
#
# The empty check is a safety interlock, not defensiveness: if cage failed to
# start Xwayland this file is empty, and without the check DISPLAY would fall
# through to the user's :1 and the agent would type into their windows. Refuse
# instead.
in_session_x() {
  [[ -s "$XDISPLAY_FILE" ]] || die "the session has no Xwayland display; refusing to \
send keystrokes, as they would go to your real session. Check $LOG_FILE."
  local xdisp
  xdisp="$(cat "$XDISPLAY_FILE")"
  [[ "$xdisp" == ":"* ]] || die "implausible X display '$xdisp'; refusing to send keystrokes"
  env -u WAYLAND_DISPLAY DISPLAY="$xdisp" "$@"
}

wait_for_socket() {
  local deadline=$((SECONDS + 15))
  while (( SECONDS < deadline )); do
    if [[ -s "$DISPLAY_FILE" ]] && [[ -S "${XDG_RUNTIME_DIR:-/tmp}/$(cat "$DISPLAY_FILE")" ]]; then
      return 0
    fi
    if [[ -f "$PID_FILE" ]] && ! kill -0 "$(cat "$PID_FILE")" 2>/dev/null; then
      echo "--- compositor log ---" >&2
      tail -n 30 "$LOG_FILE" >&2 || true
      die "compositor exited during startup"
    fi
    sleep 0.2
  done
  die "timed out waiting for the nested compositor socket"
}

is_running() { [[ -f "$PID_FILE" ]] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null; }

require_pointer() {
  [[ -x "$POINTER" ]] || die "agent-pointer not built; run: make -C tools/agent-pointer"
}

cmd_run() {
  local windowed=0 build=Release
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --windowed) windowed=1; shift ;;
      --debug)    build=Debug; shift ;;
      --build)    build="$2"; shift 2 ;;   # any BUILDDIR under syncscribble/, e.g. --build ReleaseSDL
      --)         shift; break ;;
      *)          break ;;
    esac
  done

  is_running && die "an agent display is already running (pid $(cat "$PID_FILE")); stop it first"

  local app="$APP_DIR/$build/Sumi"
  [[ -x "$app" ]] || die "no binary at $app - build it first"
  need cage "install with: sudo pacman -S cage"

  rm -f "$DISPLAY_FILE" "$XDISPLAY_FILE"

  # Both socket names are assigned by cage and cannot be queried from outside,
  # so the child reports them before exec'ing the app - and its own pid, which
  # exec keeps, so stop can reap exactly this app and never the user's own copy.
  local inner="printf '%s' \"\$WAYLAND_DISPLAY\" > '$DISPLAY_FILE'; \
printf '%s' \"\$DISPLAY\" > '$XDISPLAY_FILE'; printf '%s' \"\$\$\" > '$APP_PID_FILE'; exec '$app' $*"

  if (( windowed )); then
    WLR_BACKENDS=wayland cage -- bash -c "$inner" >"$LOG_FILE" 2>&1 &
  else
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1 \
      cage -- bash -c "$inner" >"$LOG_FILE" 2>&1 &
  fi
  echo $! > "$PID_FILE"

  wait_for_socket
  echo "agent display up: WAYLAND_DISPLAY=$(cat "$DISPLAY_FILE") \
DISPLAY=$(cat "$XDISPLAY_FILE" 2>/dev/null || echo none) \
pid=$(cat "$PID_FILE") mode=$([[ $windowed == 1 ]] && echo windowed || echo headless)"
}

cmd_test() {
  local app="$APP_DIR/Debug/Sumi"
  [[ -x "$app" ]] || die "no binary at $app - build with: cd syncscribble && make DEBUG=1"
  need cage "install with: sudo pacman -S cage"

  local out="$RUNDIR/test-output.txt" rc=0
  # ScribbleTest exits with the number of failed thumbnails, which is the signal
  # worth having. ASan's leak check overrides that with its own exitcode (1), and
  # every leak it reports here is inside NVIDIA's GL driver and libdbus, not
  # Sumi - so leak detection is off by default. Set AGENT_DISPLAY_ASAN_LEAKS=1
  # to get it back when the leaks are the point.
  local asan="${ASAN_OPTIONS:-}"
  [[ "${AGENT_DISPLAY_ASAN_LEAKS:-0}" == "1" ]] || asan="detect_leaks=0${asan:+,$asan}"

  # `set -e` would abort before $? could be read, so capture it inline.
  WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1 \
  ASAN_OPTIONS="$asan" \
    cage -- "$app" --test >"$out" 2>&1 || rc=$?

  # The verdict is one line in the middle of pages of wlroots/EGL/xkbcomp noise,
  # so surface it rather than tailing the end of the file.
  local verdict
  verdict="$(grep -m1 'Tests completed' "$out" || true)"
  echo "exit=$rc output=$out"
  if [[ -n "$verdict" ]]; then
    echo "$verdict"
    # Thumbnails are a rendering-environment check and are known-fragile (see the
    # comment at scribbletest.cpp:470); a mismatch there is not a failed test.
    echo "note: 'failed tests'/'failed unit checks' are the real signal; failed"
    echo "      thumbnails compare rendered pixels and differ across GPUs/drivers."
  else
    echo "no verdict line found - the run did not reach the end:"
    grep -vE '^\s+#[0-9]+ 0x|BuildId|Supported (EGL|GLES2)' "$out" | tail -n 20
  fi
  return $rc
}

cmd_shot() {
  local out="${1:-$SHOT_DIR/shot-$(date +%H%M%S).png}"
  need grim "install with: sudo pacman -S grim"
  in_session grim "$out"
  echo "$out"
}

cmd_move()   { [[ $# -ge 2 ]] || die "usage: $0 move <x> <y>"; require_pointer; in_session "$POINTER" move "$@"; }
cmd_scroll() { [[ $# -ge 1 ]] || die "usage: $0 scroll [<x> <y>] <dy> [<dx>]"; require_pointer; in_session "$POINTER" scroll "$@"; }

# Position and button must be one invocation - see the note in agent-pointer.c.
cmd_click() {
  [[ $# -ge 2 ]] || die "usage: $0 click <x> <y> [left|right|middle]"
  require_pointer
  in_session "$POINTER" click "$@"
}

cmd_drag() {
  [[ $# -ge 4 ]] || die "usage: $0 drag <x1> <y1> <x2> <y2> [<steps>] [left|right|middle]"
  require_pointer
  in_session "$POINTER" drag "$@"
}

cmd_stroke() {
  [[ $# -ge 4 ]] || die "usage: $0 stroke <x1> <y1> <x2> <y2> [<x3> <y3> ...]"
  require_pointer
  in_session "$POINTER" stroke "$@"
}

cmd_type() {
  [[ $# -ge 1 ]] || die "usage: $0 type <text>"
  need xdotool "install with: sudo pacman -S xdotool"
  in_session_x xdotool type --clearmodifiers -- "$*"
}

# Keystrokes for a *native Wayland* client (Sumi built on sdl2-compat with linuxWayland=1 is one);
#  xdotool only reaches X clients.  Same syntax as `key`: modifiers and key joined by '+'.
cmd_wkey() {
  [[ $# -ge 1 ]] || die "usage: $0 wkey <keyname>   e.g. ctrl+s, Escape"
  need wtype "install with: sudo pacman -S wtype"
  local chord="$1" args=() mods=() part
  IFS='+' read -ra parts <<< "$chord"
  local key="${parts[-1]}"
  unset 'parts[-1]'
  for part in "${parts[@]}"; do args+=(-M "$part"); mods+=("$part"); done
  args+=(-k "$key")
  for part in "${mods[@]}"; do args+=(-m "$part"); done
  in_session wtype "${args[@]}"
}

cmd_key() {
  [[ $# -ge 1 ]] || die "usage: $0 key <keyname>   e.g. ctrl+z, Escape, Return"
  need xdotool "install with: sudo pacman -S xdotool"
  in_session_x xdotool key --clearmodifiers "$@"
}

# keydown/keyup exist so a modifier can be *held* across other commands - the only way to exercise
# Ctrl+wheel or Shift+wheel, since `key` sends a complete chord.  Always pair them: a modifier left
# down stays down for the rest of the session.
cmd_keydown() {
  [[ $# -ge 1 ]] || die "usage: $0 keydown <keyname>   e.g. ctrl, shift"
  need xdotool "install with: sudo pacman -S xdotool"
  in_session_x xdotool keydown "$@"
}
cmd_keyup() {
  [[ $# -ge 1 ]] || die "usage: $0 keyup <keyname>   e.g. ctrl, shift"
  need xdotool "install with: sudo pacman -S xdotool"
  in_session_x xdotool keyup "$@"
}

cmd_record() {
  local action="${1:-}"
  case "$action" in
    start)
      need wf-recorder "install with: sudo pacman -S wf-recorder"
      [[ -f "$REC_PID_FILE" ]] && kill -0 "$(cat "$REC_PID_FILE")" 2>/dev/null \
        && die "already recording (pid $(cat "$REC_PID_FILE"))"
      local out="${2:-$SHOT_DIR/rec-$(date +%H%M%S).mp4}"
      in_session wf-recorder -f "$out" >"$RUNDIR/recorder.log" 2>&1 &
      echo $! > "$REC_PID_FILE"
      echo "$out" > "$RUNDIR/recorder.path"
      echo "recording to $out (pid $(cat "$REC_PID_FILE"))"
      ;;
    stop)
      [[ -f "$REC_PID_FILE" ]] || die "not recording"
      # wf-recorder needs SIGINT to finalise the container; SIGKILL corrupts it.
      kill -INT "$(cat "$REC_PID_FILE")" 2>/dev/null || true
      sleep 1
      rm -f "$REC_PID_FILE"
      echo "stopped: $(cat "$RUNDIR/recorder.path" 2>/dev/null || echo unknown)"
      ;;
    *) die "usage: $0 record start|stop [out.mp4]" ;;
  esac
}

cmd_status() {
  if is_running; then
    echo "running  pid=$(cat "$PID_FILE")  WAYLAND_DISPLAY=$(cat "$DISPLAY_FILE" 2>/dev/null || echo '?')  DISPLAY=$(cat "$XDISPLAY_FILE" 2>/dev/null || echo '?')"
    echo "log=$LOG_FILE  shots=$SHOT_DIR"
  else
    echo "not running"
  fi
  local stray
  stray="$(pgrep -cx 'Sumi|Write' 2>/dev/null || true)"
  [[ "${stray:-0}" != "0" ]] && echo "Sumi processes alive: $stray"
  return 0
}

cmd_stop() {
  [[ -f "$REC_PID_FILE" ]] && cmd_record stop || true
  if is_running; then
    kill "$(cat "$PID_FILE")" 2>/dev/null || true
    sleep 0.5
    kill -9 "$(cat "$PID_FILE")" 2>/dev/null || true
  fi
  # Killing cage does not necessarily take the app with it: a Sumi that
  # outlives its compositor keeps running headless forever, holding the document
  # and its lock. Reap it explicitly - by the pid the session recorded, never by
  # name: the user may have their own Sumi open, and a name match killed it.
  local app_pid
  app_pid="$(cat "$APP_PID_FILE" 2>/dev/null || true)"
  if [[ -n "$app_pid" ]] && [[ "$(ps -o comm= -p "$app_pid" 2>/dev/null)" =~ ^(Sumi|Write)$ ]]; then
    kill "$app_pid" 2>/dev/null || true
    sleep 0.3
    kill -9 "$app_pid" 2>/dev/null || true
  fi
  rm -f "$PID_FILE" "$APP_PID_FILE" "$DISPLAY_FILE" "$XDISPLAY_FILE"
  echo "stopped"
}

case "${1:-}" in
  run)    shift; cmd_run "$@" ;;
  test)   shift; cmd_test "$@" ;;
  shot)   shift; cmd_shot "$@" ;;
  move)   shift; cmd_move "$@" ;;
  click)  shift; cmd_click "$@" ;;
  drag)   shift; cmd_drag "$@" ;;
  stroke) shift; cmd_stroke "$@" ;;
  scroll) shift; cmd_scroll "$@" ;;
  type)   shift; cmd_type "$@" ;;
  key)    shift; cmd_key "$@" ;;
  wkey)   shift; cmd_wkey "$@" ;;
  keydown) shift; cmd_keydown "$@" ;;
  keyup)  shift; cmd_keyup "$@" ;;
  record) shift; cmd_record "$@" ;;
  status) shift; cmd_status "$@" ;;
  stop)   shift; cmd_stop "$@" ;;
  *) grep -E '^# ' "${BASH_SOURCE[0]}" | sed 's/^# \?//' ;;
esac
