#!/usr/bin/env python3
"""Measure whether a Write document's strokes sit on an integer screen-pixel lattice.

Usage: stroke-lattice.py DOC.svgz|DOC.html [--min-points N]

For each stroke path with at least N points (default 100) it reconstructs the
centreline (filled strokes: midpoint of the two outline sides; stroked paths: the
path itself) and reports

  - the best-fitting lattice pitch and the mean residual against it
    (residual near 0 = every sample is on a whole-pixel grid = quantised input)
  - the turn angle between consecutive samples at a few quantiles
    (a lattice shows up as peaks at 26.6, 45, 63.4 and 90 degrees)

This is the check behind JAGGED_STROKES.md; run it on a stroke drawn with the
build under test to see whether the input path is delivering sub-pixel coordinates.
"""
import math
import re
import statistics
import sys
import zlib


def load_text(path):
    raw = open(path, 'rb').read()
    if raw[:2] == b'\x1f\x8b':
        raw = zlib.decompressobj(zlib.MAX_WBITS | 32).decompress(raw)
    return raw.decode('utf8', 'ignore')


def parse_path(pathdata):
    tokens = re.findall(r'[MmLlZzCcQq]|-?\d*\.?\d+(?:e-?\d+)?', pathdata)
    points = []
    current = [0.0, 0.0]
    mode = 'M'
    index = 0
    while index < len(tokens):
        token = tokens[index]
        if token.isalpha():
            mode = token
            index += 1
            continue
        if mode in 'CcQq':
            return None  # curves: not a hand-drawn stroke
        x = float(tokens[index])
        y = float(tokens[index + 1])
        index += 2
        current = [current[0] + x, current[1] + y] if mode.islower() else [x, y]
        points.append(tuple(current))
    return points


def centreline(points, filled):
    if not filled:
        return points
    count = len(points)
    return [((points[i][0] + points[count - 1 - i][0]) / 2, (points[i][1] + points[count - 1 - i][1]) / 2)
            for i in range(count // 2)]


def lattice_residual(deltas, pitch):
    residuals = [abs(value / pitch - round(value / pitch)) for value in deltas]
    return statistics.mean(residuals) if residuals else 1.0


def analyse(points):
    deltas = []
    for a, b in zip(points, points[1:]):
        for value in (b[0] - a[0], b[1] - a[1]):
            if abs(value) > 1e-6:
                deltas.append(abs(value))
    if len(deltas) < 20:
        return None
    # candidate pitch: the smallest common step, refined by scanning around it
    smallest = sorted(deltas)[len(deltas) // 20]
    best_pitch, best_residual = smallest, 1.0
    for step in range(-200, 201):
        pitch = smallest * (1 + step / 1000.0)
        if pitch <= 0:
            continue
        residual = lattice_residual(deltas, pitch)
        if residual < best_residual:
            best_pitch, best_residual = pitch, residual
    turns = []
    for a, b, c in zip(points, points[1:], points[2:]):
        v1 = (b[0] - a[0], b[1] - a[1])
        v2 = (c[0] - b[0], c[1] - b[1])
        n1 = math.hypot(*v1)
        n2 = math.hypot(*v2)
        if n1 > 0.05 and n2 > 0.05:
            cosine = max(-1.0, min(1.0, (v1[0] * v2[0] + v1[1] * v2[1]) / (n1 * n2)))
            turns.append(math.degrees(math.acos(cosine)))
    turns.sort()
    segments = sorted(math.hypot(b[0] - a[0], b[1] - a[1]) for a, b in zip(points, points[1:]))
    return best_pitch, best_residual, turns, segments


def quantile(values, fraction):
    return values[min(len(values) - 1, int(fraction * len(values)))] if values else float('nan')


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    min_points = 100
    if '--min-points' in sys.argv:
        min_points = int(sys.argv[sys.argv.index('--min-points') + 1])
    text = load_text(sys.argv[1])
    found = 0
    for match in re.finditer(r'<path([^>]*)\sd="([^"]+)"', text):
        attrs = match.group(1)
        points = parse_path(match.group(2))
        if not points or len(points) < min_points:
            continue
        filled = 'write-flat-pen' in attrs or 'write-round-pen' in attrs or 'write-chisel-pen' in attrs
        line = centreline(points, filled)
        result = analyse(line)
        if not result:
            continue
        found += 1
        pitch, residual, turns, segments = result
        verdict = 'QUANTISED (integer-pixel input)' if residual < 0.02 else 'sub-pixel'
        print(f'stroke {found}: {len(line)} samples, {"filled" if filled else "stroked"}')
        print(f'  lattice pitch {pitch:.4f} du, mean residual {residual:.3f}  -> {verdict}')
        print(f'  segment length  q50 {quantile(segments, .5):.2f}  q90 {quantile(segments, .9):.2f}')
        print(f'  turn angle deg  q50 {quantile(turns, .5):.1f}  q75 {quantile(turns, .75):.1f}'
              f'  q90 {quantile(turns, .9):.1f}  q99 {quantile(turns, .99):.1f}')
    if not found:
        print('no stroke with enough points found')
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
