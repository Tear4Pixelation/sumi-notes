#!/usr/bin/env bash
# build-gate.sh - run a build command under a machine-wide resource cap.
#
# Every agent (and subagent) must run builds through this, never bare `make`:
#   tools/build-gate.sh make USE_SYSTEM_SDL=1 DEBUG=1      (from syncscribble/)
#   tools/build-gate.sh ./gww installRelease
#
# All gated commands share ONE systemd user slice (sumi-build.slice), so the cap is
# total across all agents, not per build:
#   CPU    <= 90% of all cores   (CPUQuota)
#   memory <= 80% of RAM         (MemoryMax; MemoryHigh at 72% throttles first), swap off
# On top of that at most BUILD_SLOTS (default 2) builds run at once; the rest queue on a
# lock, so the box is not thrashed by a dozen compilers fighting over the same cores.
# Builds also run at nice 10 / idle IO so the desktop stays usable.
#
# Env: BUILD_SLOTS=2  CPU_PCT=90  MEM_PCT=80
# If the command is `make` and no -j is given, -j is set to the core count (the CPU quota
# does the limiting; this just lets make use its share).
set -u

[[ $# -gt 0 ]] || { echo "usage: $0 <command> [args...]" >&2; exit 2; }

SLOTS="${BUILD_SLOTS:-2}"
CPU_PCT="${CPU_PCT:-90}"
MEM_PCT="${MEM_PCT:-80}"
SLICE=sumi-build.slice
LOCKDIR="${XDG_RUNTIME_DIR:-/tmp}/sumi-build-gate"
mkdir -p "$LOCKDIR"

cores=$(nproc)
mem_kb=$(awk '/^MemTotal:/ {print $2}' /proc/meminfo)
cpu_quota=$(( cores * CPU_PCT ))                   # systemd: 100% = one core
mem_max=$(( mem_kb * 1024 / 100 * MEM_PCT ))
mem_high=$(( mem_max / 100 * 90 ))

# (Re)write the shared slice unit when the limits change.
unit_dir="$HOME/.config/systemd/user"
unit="$unit_dir/$SLICE"
want="[Unit]
Description=Sumi builds (shared cap for all agents)

[Slice]
CPUQuota=${cpu_quota}%
MemoryMax=${mem_max}
MemoryHigh=${mem_high}
MemorySwapMax=0
"
if [[ "$(cat "$unit" 2>/dev/null)" != "$want" ]]; then
  mkdir -p "$unit_dir"
  printf '%s' "$want" > "$unit"
  systemctl --user daemon-reload
fi

# Take a build slot (queue if all are busy). Slots are held by an open fd, so a crashed
# build releases its slot automatically.
slot=-1
while :; do
  for (( i = 0; i < SLOTS; i++ )); do
    exec {fd}>"$LOCKDIR/slot$i"
    if flock -n "$fd"; then slot=$i; break 2; fi
    exec {fd}>&-
  done
  [[ -n "${waited:-}" ]] || { echo "build-gate: all $SLOTS build slots busy, queued..." >&2; waited=1; }
  sleep 2
done
[[ -n "${waited:-}" ]] && echo "build-gate: slot $slot acquired" >&2

cmd=("$@")
if [[ "${cmd[0]}" == make || "${cmd[0]}" == */make ]]; then
  has_jobs=0
  for arg in "${cmd[@]}"; do [[ "$arg" == -j* || "$arg" == --jobs* ]] && has_jobs=1; done
  (( has_jobs )) || cmd+=("-j$cores")
fi

if systemctl --user is-system-running >/dev/null 2>&1 || systemctl --user status >/dev/null 2>&1; then
  exec systemd-run --user --scope --quiet --collect --slice="$SLICE" \
    nice -n 10 ionice -c 3 "${cmd[@]}"
fi
echo "build-gate: no systemd user manager - running with nice only, caps NOT enforced" >&2
exec nice -n 10 ionice -c 3 "${cmd[@]}"
