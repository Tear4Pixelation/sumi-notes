# Performance: how it is measured, and the October 2026 optimization pass

## Measuring

- `cd syncscribble && make USE_SYSTEM_SDL=1 BENCH=1` (or `SCRIBBLE_TEST=1`) builds `Bench/Sumi`: release
  flags (including LTO) with the in-app test modes compiled in. A plain Release build has none of them
  (`basics.h` enables `SCRIBBLE_TEST` only without `NDEBUG`), and `--perftest` there silently falls through
  to the document list and waits forever.
- `tools/perf-bench.sh --mode benchtest --runs 5` runs the broad benchmark headless (startup, repaint, the
  classic perftest scenarios, a 100-page document, pen/eraser/selection latency, save/load, PDF import, RSS
  checkpoints) and prints median [min-max] per metric. Details in [tests.md](tests.md).
- This machine is shared and loaded; CPU numbers vary 20-60% between runs. Compare interleaved runs of the
  old and new binary, or use instruction counts: `SUMI_BENCH_PAGES=10 tools/perf-bench.sh --mode benchtest
  --runs 1 --wrap "valgrind --tool=callgrind --callgrind-out-file=..."`.
- Make does not track compiler flags: a build dir seeded from objects built with other flags (LTO on/off)
  links a mix without warning. Rebuild clean after changing flags.

## Results of the pass (baseline = master 86ce203)

Whole benchtest at 10 pages, callgrind: **97.4G -> 41.6G instructions (-57%)**. Wall clock, median of 5
interleaved runs on the 100-page benchtest:

| metric | before | after |
|---|---|---|
| pan frame (dense page) | 27.2 ms | 2.3 ms |
| draw 1521 strokes | 1203 ms | 667 ms |
| pen event on a full page (mean) | 0.6 ms | 0.3 ms |
| selection drag event (mean) | 2.4 ms | 1.4 ms |
| stroke / free eraser sweep | 120 / 147 ms | 55 / 69 ms |
| scroll / zoom / goto frame | 1.2 / 1.4 / 1.2 ms | 0.8 / 0.9 / 0.8 ms |
| full repaint | 1.0 ms | 0.6 ms |
| save 100-page .svgz | 1011 ms | 250 ms |
| PDF import, 2 pages | 5278 ms | 2067 ms |
| startup to first frame | 442 ms | 349 ms |
| RSS at end of run | 399 MB | 296 MB |
| peak RSS | 407 MB | 429 MB (+5%, PDF import pipelining) |

Not in benchtest: startup instructions with a document open -58%; saving a 114-page notebook after editing
page 1 ~10 s -> 0.46 s (peak +1.4 GB -> +129 MB); thumbnails of image pages 15-25x faster; Element 328 -> 88
bytes. Unchanged: html load (`strToReal`: its fraction loop is serial and bit-identity rules out
reordering it; summing the integer digits as an integer measured no faster), svgz load.

Second pass (opt-small): `realToStr` writes two digits at a time from a digit pair table, in place
(byte-identical, `scribbletest/realtostrtest.cpp`), and numbers and path data skip the writer's escape scan
(`XmlStreamWriter::writeAttributeRaw`): save -18% instructions, -22% CPU for both html and svgz. Thumbnails
and other `Image::encodePNG()` output go through `encodePNGStriped` (see pdf-import.md), ~2x faster on a
thumbnail, same pixels. Parsed paths are shrunk to fit (`Path2D::shrinkToFit`), which only matters for
multi-segment or closed path data: ink as saved (`M` and one segment) is already reserved exactly, so the
measured saving is ~107 kB of UI icons at startup. The stroke eraser gets the ruled eraser's cull
(pen-and-tools.md, Interaction fast paths).

Where each change lives and its traps: [canvas-layer.md](canvas-layer.md) (retained pan layer),
[stroke-input.md](stroke-input.md) and [shapes.md](shapes.md) (incremental flat outline, curve-fit knots,
scratch-out), [pen-and-tools.md](pen-and-tools.md) (pen toolbar refresh, Element memory, interaction fast
paths, vtex upload), [themes.md](themes.md) (literal cusp table), [pdf-import.md](pdf-import.md) (import
speed, verbatim svgz block copy, Android exports), [page-management.md](page-management.md) (thumbnail
proxies and cache), [sidebar.md](sidebar.md), [layers.md](layers.md), [tests.md](tests.md).

## Open decisions (behavior changes deliberately not made)

- Curve-fit output is never simplified (`inputSimplify` = 0), so documents are ~77% larger than needed;
  simplifying changes stored geometry within tolerance.
- The first ordinary save after a PDF import resamples its pages to ~150 DPI, discarding `pdfImportDPI`.
- In-memory undo history is unbounded.
- svgz compression level 1 for image-heavy blocks would be ~4x faster for ~2% larger files.
- Dragging a large selection could use a cached layer, but that changes z-order during the drag.
- Unchanged pages of notebooks written by older builds are now copied verbatim on partial save instead of
  being re-normalized (ruling rounding, image resampling); output is semantically identical but not
  byte-identical to what the old build wrote.
- Untested on devices: Android (`libmain.so` export trimming, `android/libmain.map`), iOS/macOS, wasm, ARM
  bit-identity of the curve-fit/nanovg arithmetic, MSVC LTO.
