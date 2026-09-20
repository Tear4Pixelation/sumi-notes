#!/usr/bin/env bash
# One-time setup for tools/agent-display.sh. Needs sudo, so it is kept separate
# from the runtime script (which must never need elevation).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo "== packages =="
# cage         nested/headless wlroots kiosk compositor
# grim         wlr-screencopy screenshots
# wf-recorder  wlr-screencopy video capture
# xdotool      keyboard injection into cage's own Xwayland server
sudo pacman -S --needed cage grim wf-recorder xdotool

echo
echo "== agent-pointer =="
# Pointer injection over zwlr_virtual_pointer_v1. Built here rather than using
# wlrctl from the AUR, because wlrctl offers only relative motion and a click
# that cannot be split into press/release - so it cannot express a drag, which
# in a drawing application is every stroke.
make -C "$REPO_ROOT/tools/agent-pointer"

echo
echo "== verify =="
for tool in cage grim wf-recorder xdotool; do
  printf '%-14s %s\n' "$tool" "$(command -v "$tool" || echo 'MISSING')"
done
printf '%-14s %s\n' agent-pointer \
  "$([[ -x "$REPO_ROOT/tools/agent-pointer/agent-pointer" ]] && echo built || echo 'MISSING')"

echo
echo "Deliberately NOT used:"
echo "  ydotool/dotool - inject via kernel uinput, which is seat-global, so the"
echo "                   events would land in your real session, not the agent's."
echo "  wtype          - correct protocol, but its virtual-keyboard keymap does"
echo "                   not survive Xwayland, and Write runs as an X11 client"
echo "                   inside cage (SDL's Wayland driver segfaults it)."
