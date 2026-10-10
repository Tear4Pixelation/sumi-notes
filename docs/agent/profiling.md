# Profiling (perf + flame graph)

A recipe for the optimisation work, not a record of results. Profile an optimised build: the Debug build
is `-O0` with ASan/UBSan (`SANITIZE=1` by default), which multiplies the cost of everything and puts the
sanitizer runtime at the top of every profile.

## Build

`Makefile.unix` already adds `-g` to every build, and `PROFILE=1` adds `-fno-omit-frame-pointer` and
`-rdynamic`. Give it its own output folder so it neither replaces `Release/` nor mixes objects with it:

```
cd syncscribble
../tools/build-gate.sh make USE_SYSTEM_SDL=1 DEBUG=0 PROFILE=1 BUILDDIR=Profile
cp -r ../scribbleres/fonts Profile/      # as for Release
```

`DEBUG=0` keeps `-O2 -DNDEBUG` and `SANITIZE=0`. MuPDF is still built `-Os` (`mupdf.mk`), so its frames
look different from the rest; that is expected.

## Run it (agent display only)

Never start `Profile/Sumi` directly (see CLAUDE.md, "Agent display"): `agent-display.sh run` takes any
build folder under `syncscribble/`.

```
AGENT_DISPLAY_NAME=perf tools/agent-display.sh run --build Profile
```

**Create `Profile/write.xml` first** (copy `Debug/write.xml`): a writable `write.xml` beside the binary is
used by any Linux build, and without one the app reads and writes the user's real `~/.config/sumi/sumi.xml`
and library. Set `libraryPath` to a scratch folder there, plus whatever the measurement needs (`openTabs`,
`topTabBar`) - see top-tab-bar.md, "Verified".

## Record

`perf` is not installed by default on Arch: `sudo pacman -S perf`. It needs
`/proc/sys/kernel/perf_event_paranoid` <= 2 (the default) to profile your own process in user space; only
kernel frames need 1 or lower.

```
RUNDIR=/run/user/$UID/write-agent-display-perf
perf record -F 999 --call-graph fp -p "$(cat $RUNDIR/app.pid)" -o /tmp/sumi.perf.data -- sleep 20
```

Drive the interaction to measure during those 20 s from another shell (`tools/agent-display.sh stroke ...`,
`click`, `scroll`, a tab switch, opening the Pages view...). `--call-graph fp` relies on the frame
pointers from `PROFILE=1`; if stacks look truncated (system libraries are built without them), use
`--call-graph dwarf,16384` instead - much larger files, but it unwinds through libc, SDL and Mesa/NVIDIA.

Quick look without a graph:

```
perf report -i /tmp/sumi.perf.data --no-children --sort symbol | head -60
```

## Flame graph

Either Brendan Gregg's scripts:

```
git clone https://github.com/brendangregg/FlameGraph ~/FlameGraph     # once
perf script -i /tmp/sumi.perf.data | ~/FlameGraph/stackcollapse-perf.pl | ~/FlameGraph/flamegraph.pl > /tmp/sumi.svg
```

or `inferno` (`cargo install inferno`): `perf script -i /tmp/sumi.perf.data | inferno-collapse-perf | inferno-flamegraph > /tmp/sumi.svg`.
Open the SVG in a browser; it is searchable (Ctrl+F) and zoomable.

## Reading it

- The main thread does layout, painting and input; `SvgGui::layoutAndDraw` -> `layoutWindow`/`layoutWidget`
  is layout, `Painter`/`nanovg`/`glnvg__*` is drawing. The timer thread (`timerThreadFn`) should be idle.
- The Pages view renders one thumbnail per event-loop turn from a 1 ms timer (page-management.md), so
  `processTimers` under a scroll is not necessarily the scroll itself.
- With `debugDirty`/`debugLayout` off, a frame that lays out the whole window (`layoutWindow` rather than
  `layoutWidget`) means something marked the window itself dirty.
- For allocation-heavy paths, `perf record -e page-faults` or valgrind's massif (on a standalone test, see
  tests.md) is more telling than CPU samples.
