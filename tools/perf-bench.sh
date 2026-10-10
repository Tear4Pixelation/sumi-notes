#!/usr/bin/env bash
# perf-bench.sh - run Sumi's in-app benchmark headless and report timings + peak memory.
#
#   tools/perf-bench.sh [--build Bench] [--runs 5] [--mode perftest] [--timeout 300]
#                       [--wrap "valgrind --tool=callgrind --callgrind-out-file=/path/cg.out"]
#                       [-- extra app args]
#
# The binary must have the in-app tests compiled in: Release builds define NDEBUG, which
# compiles them out (basics.h), and the app then ignores --perftest and sits in the document
# list forever.  Build the measured binary with: cd syncscribble && make SCRIBBLE_TEST=1 (-> Bench/).
#
# Each run starts a fresh headless cage (invisible, cannot take focus - see CLAUDE.md,
# Agent display) with the app in a sandbox: a throwaway write.xml beside the binary (so
# the user's ~/.config/sumi/sumi.xml is never read or written) and a throwaway library.
# The app prints its result block via PLATFORM_LOG and then keeps running, so the run is
# over when the last result line appears; VmHWM (peak RSS) is read from /proc just before
# the app is killed.  Output: every run's numbers, then the median of each metric.
#
# The run is event-paced without help: the benchmarks call processEvents(), which blocks in
# SDL_WaitEvent, and a headless compositor delivers almost no events. perf-waitshim.c (built
# here on first use, LD_PRELOADed) makes that wait a poll. --no-shim disables it.
# --updateCheck=0 is always passed so no run touches the network.
#
# Note: the classic perftest never calls layoutAndDraw, so it measures input and document-model
# work but no painting (it reports 0 frames), and its PeakRSS is dominated by a 512 MB
# calibration buffer - do not judge memory changes by perftest's PeakRSS.
#
# Timings are wall-clock on a shared machine (other agents compile here): compare medians
# of >= 5 runs, before vs after, from the same build type, and distrust < 3% differences.
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP_DIR="$ROOT/syncscribble"
build=Bench runs=5 mode=perftest timeout=300 wrap="" shim=1
while [[ $# -gt 0 ]]; do
  case "$1" in
    --build)   build="$2"; shift 2 ;;
    --runs)    runs="$2"; shift 2 ;;
    --mode)    mode="$2"; shift 2 ;;
    --timeout) timeout="$2"; shift 2 ;;
    --wrap)    wrap="$2"; shift 2 ;;
    --no-shim) shim=0; shift ;;
    --)        shift; break ;;
    *)         break ;;
  esac
done
extra="--updateCheck=0 $*"

app="$APP_DIR/$build/Sumi"
[[ -x "$app" ]] || { echo "no binary at $app - build it: cd syncscribble && make SCRIBBLE_TEST=1 [BUILDDIR=$build]" >&2; exit 1; }
grep -q "ScribbleTest" "$app" || {
  echo "$app has no in-app tests compiled in (NDEBUG without SCRIBBLE_TEST) - --$mode would hang;" \
       "build with: make SCRIBBLE_TEST=1 [BUILDDIR=$build]" >&2; exit 1; }
command -v cage >/dev/null || { echo "cage missing (sudo pacman -S cage)" >&2; exit 1; }

# fonts are looked up beside the binary
[[ -d "$APP_DIR/$build/fonts" ]] || cp -r "$ROOT/scribbleres/fonts" "$APP_DIR/$build/" 2>/dev/null

preload=""
if (( shim )); then
  shimso="${XDG_RUNTIME_DIR:-/tmp}/sumi-perf-waitshim.so"
  if [[ ! -f "$shimso" || "$ROOT/tools/perf-waitshim.c" -nt "$shimso" ]]; then
    cc -shared -fPIC -O2 $(pkg-config --cflags sdl2 2>/dev/null) -o "$shimso.tmp" "$ROOT/tools/perf-waitshim.c" -ldl \
      && mv -f "$shimso.tmp" "$shimso" || { echo "failed to build perf-waitshim.so" >&2; exit 1; }
  fi
  preload="env LD_PRELOAD=$shimso"
fi

work="$(mktemp -d "${TMPDIR:-/tmp}/sumi-bench.XXXXXX")"
trap 'rm -rf "$work"' EXIT

# sandbox config: a writable write.xml beside the binary wins over ~/.config/sumi/sumi.xml
cfg="$APP_DIR/$build/write.xml"
cfg_created=0
if [[ ! -e "$cfg" ]]; then
  cfg_created=1
  cat > "$cfg" <<'EOF'
<?xml version="1.0"?>
<map>
  <int name="libraryMigrated" value="1" />
  <int name="pageSizeAsked" value="1" />
  <int name="lastUpdateCheck" value="2147483000" />
  <float name="pageHeight" value="1755" />
  <float name="pageWidth" value="1240" />
</map>
EOF
  trap 'rm -rf "$work"; rm -f "$cfg"' EXIT
fi

# the last line each mode prints - the run is complete once it appears
case "$mode" in
  perftest) last_re='^Load: ' ;;
  *)        last_re="${BENCH_LAST_LINE:-^BENCH_DONE}" ;;
esac

declare -a all_results=()
for (( run=1; run<=runs; run++ )); do
  rm -rf "$work/lib" && mkdir -p "$work/lib"
  out="$work/run$run.log" pidf="$work/app.pid"
  rm -f "$pidf"
  inner="printf '%s' \"\$\$\" > '$pidf'; exec $preload $wrap '$app' --$mode --library '$work/lib' $extra"
  WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1 \
    cage -- bash -c "$inner" >"$out" 2>&1 &
  cage_pid=$!
  hwm="" start=$SECONDS
  while (( SECONDS - start < timeout )); do
    if grep -qE "$last_re" "$out" 2>/dev/null; then break; fi
    kill -0 $cage_pid 2>/dev/null || break
    if [[ -f "$pidf" ]]; then
      cur=$(awk '/^VmHWM:/ {print $2}' "/proc/$(cat "$pidf")/status" 2>/dev/null)
      [[ -n "$cur" ]] && hwm=$cur
    fi
    sleep 0.2
  done
  if [[ -f "$pidf" ]]; then
    cur=$(awk '/^VmHWM:/ {print $2}' "/proc/$(cat "$pidf")/status" 2>/dev/null)
    [[ -n "$cur" ]] && hwm=$cur
    kill -INT "$(cat "$pidf")" 2>/dev/null   # valgrind writes its output on SIGINT/exit
    sleep 1
  fi
  kill $cage_pid 2>/dev/null; wait $cage_pid 2>/dev/null
  if ! grep -qE "$last_re" "$out"; then
    echo "run $run: no result (timeout or crash) - log tail:"; grep -vE 'wlr|EGL|xkb' "$out" | tail -15
    continue
  fi
  echo "--- run $run"
  # result lines look like "Name: ... <number> ms ..."; keep everything after the mode banner
  res="$(grep -E '^(Calibration|Draw|Move|Pan|Save|Load|BENCH)[A-Za-z_ ]*:' "$out")"
  echo "$res"
  echo "PeakRSS: ${hwm:-?} kB"
  all_results+=("$res"$'\n'"PeakRSS: ${hwm:-0} kB")
done

(( ${#all_results[@]} )) || exit 1
echo "=== median of ${#all_results[@]} runs"
# median of the "in N ms" value of each line (or the first number for lines without one)
printf '%s\n' "${all_results[@]}" | awk -F': ' '
  { key=$1; line=$2
    if (match(line, /in [0-9.]+ ms/)) { val=substr(line, RSTART+3, RLENGTH-6) }
    else if (match(line, /[0-9.]+/)) { val=substr(line, RSTART, RLENGTH) }
    else next
    vals[key]=vals[key] " " val; order[key]=++n }
  END {
    for (k in vals) {
      cnt=split(substr(vals[k],2), arr, " ")
      for (i=1;i<=cnt;i++) for (j=i+1;j<=cnt;j++) if (arr[j]+0<arr[i]+0) { t=arr[i]; arr[i]=arr[j]; arr[j]=t }
      med = (cnt%2) ? arr[(cnt+1)/2] : (arr[cnt/2]+arr[cnt/2+1])/2
      printf "%s: %s\n", k, med
    }
  }' | sort
