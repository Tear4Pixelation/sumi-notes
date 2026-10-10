#!/usr/bin/env bash
# perf-bench.sh - run Sumi's in-app benchmark headless and report timings + peak memory.
#
#   tools/perf-bench.sh [--build Bench] [--runs 5] [--mode perftest|benchtest] [--timeout 300]
#                       [--wrap "valgrind --tool=callgrind --callgrind-out-file=/path/cg.out"]
#                       [-- extra app args]
#
# The default build is Bench/ (make BENCH=1: Release flags plus the test modes).
# Each run starts a fresh headless cage (invisible, cannot take focus - see CLAUDE.md,
# Agent display) with the app in a sandbox: a throwaway write.xml beside the binary (so
# the user's ~/.config/sumi/sumi.xml is never read or written) and a throwaway library.
# The app prints its result block via PLATFORM_LOG and then keeps running, so the run is
# over when the last result line appears; VmHWM (peak RSS) is read from /proc just before
# the app is killed.  Output: every run's numbers, then the median of each metric.
#
# --mode benchtest (scribbletest/benchtest.cpp) is the broad one: painted frames, a 100 page document,
#  scroll/zoom/goto, per pen event latency (mean/p95), selection, erasers, undo/redo, save/load .html and
#  .svgz, thumbnails, PDF import, RSS at several points.  It prints "BENCH <name>: <value> <unit>" lines;
#  the summary gives median, min and max of each over the runs.  Env passed through to the app:
#  SUMI_BENCH_PAGES (default 100), SUMI_BENCH_VSYNC=1 (keep the driver's swap interval instead of 0).
#  perftest blocks in SDL_WaitEvent between steps and paints nothing; prefer benchtest.
#
# Timings are wall-clock on a shared machine (other agents compile here): compare medians
# of >= 5 runs, before vs after, from the same build type, and distrust < 3% differences.
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP_DIR="$ROOT/syncscribble"
build=Bench runs=5 mode=perftest timeout=300 wrap=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --build)   build="$2"; shift 2 ;;
    --runs)    runs="$2"; shift 2 ;;
    --mode)    mode="$2"; shift 2 ;;
    --timeout) timeout="$2"; shift 2 ;;
    --wrap)    wrap="$2"; shift 2 ;;
    --)        shift; break ;;
    *)         break ;;
  esac
done
extra="$*"

app="$APP_DIR/$build/Sumi"
[[ -x "$app" ]] || { echo "no binary at $app - build it first (from syncscribble/: ../tools/build-gate.sh make USE_SYSTEM_SDL=1 BENCH=1)" >&2; exit 1; }
# the test modes exist only with SCRIBBLE_TEST (basics.h: not in a plain NDEBUG Release build) - without
#  them --perftest/--benchtest are ignored and every run would just time out
grep -qa "is not a valid test mode" "$app" || { echo "$app has no test modes compiled in - use a BENCH=1 (or DEBUG=1) build" >&2; exit 1; }
command -v cage >/dev/null || { echo "cage missing (sudo pacman -S cage)" >&2; exit 1; }

# fonts are looked up beside the binary
[[ -d "$APP_DIR/$build/fonts" ]] || cp -r "$ROOT/scribbleres/fonts" "$APP_DIR/$build/" 2>/dev/null

work="$(mktemp -d "${TMPDIR:-/tmp}/sumi-bench.XXXXXX")"
trap 'rm -rf "$work"' EXIT

# sandbox config: a writable write.xml beside the binary wins over ~/.config/sumi/sumi.xml
cfg="$APP_DIR/$build/write.xml"
cfg_created=0
if [[ ! -e "$cfg" ]]; then
  cfg_created=1
  # no update check (a network request mid-run); lastUpdateCheck + the 1 week interval must fit in an int,
  #  2147483000 overflowed and made the check run every time
  cat > "$cfg" <<'EOF'
<?xml version="1.0"?>
<map>
  <int name="libraryMigrated" value="1" />
  <int name="pageSizeAsked" value="1" />
  <int name="updateCheck" value="0" />
  <int name="lastUpdateCheck" value="2000000000" />
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

all_results=()
for (( run=1; run<=runs; run++ )); do
  rm -rf "$work/lib" && mkdir -p "$work/lib"
  out="$work/run$run.log" pidf="$work/app.pid"
  rm -f "$pidf"
  inner="printf '%s' \"\$\$\" > '$pidf'; exec $wrap '$app' --$mode --library '$work/lib' $extra"
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
  # result lines look like "Name: ... <number> ms ..." (perftest) or "BENCH name: value unit" (benchtest)
  if [[ "$mode" == perftest ]]; then
    res="$(grep -E '^(Calibration|Draw|Move|Pan|Save|Load)[A-Za-z_ ]*:' "$out")"
  else
    res="$(grep -E '^BENCH [A-Za-z0-9_]+: ' "$out" | sed 's/^BENCH //')"
  fi
  echo "$res"
  echo "PeakRSS: ${hwm:-?} kB"
  all_results+=("$res"$'\n'"PeakRSS: ${hwm:-0} kB")
done

(( ${#all_results[@]} )) || exit 1
echo "=== median [min max] (spread = (max-min)/median) of ${#all_results[@]} runs"
# median of the "in N ms" value of each line (or the first number for lines without one)
printf '%s\n' "${all_results[@]}" | awk -F': ' '
  { key=$1; line=$2
    if (match(line, /in [0-9.]+ ms/)) { val=substr(line, RSTART+3, RLENGTH-6); u="ms" }
    else if (match(line, /-?[0-9.]+/)) { val=substr(line, RSTART, RLENGTH); u=(match(line, /[0-9.]+ [A-Za-z]+$/) ? substr(line, RSTART + index(substr(line, RSTART), " ")) : "") }
    else next
    if (!(key in unit)) unit[key] = u
    vals[key]=vals[key] " " val }
  END {
    for (k in vals) {
      cnt=split(substr(vals[k],2), arr, " ")
      for (i=1;i<=cnt;i++) for (j=i+1;j<=cnt;j++) if (arr[j]+0<arr[i]+0) { t=arr[i]; arr[i]=arr[j]; arr[j]=t }
      med = (cnt%2) ? arr[(cnt+1)/2] : (arr[cnt/2]+arr[cnt/2+1])/2
      spread = (med != 0) ? 100*(arr[cnt]-arr[1])/med : 0
      printf "%-34s %12.3f %-5s [%.3f %.3f] %5.1f%%\n", k ":", med, unit[k], arr[1], arr[cnt], spread
    }
  }' | sort
