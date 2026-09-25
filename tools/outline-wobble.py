#!/usr/bin/env python3
"""Measure edge wobble of filled (flat-pen) strokes in a Write document.

Usage: outline-wobble.py DOC.html|DOC.svgz [--min-points N]

For every `write-flat-pen` path with at least N outline points (default 100) it
walks the first side of the outline (path1, the half before the end cap) and
reports the turn angle between consecutive outline segments at a few quantiles,
plus the distance of each outline point from a circle fitted to the centreline
(only meaningful for a drawn circle).  Turn angles on the outline are what the
eye reads as jaggedness on the edge of a stroke; the centreline is the same
before and after the flat-pen tangent window, so this is the number that the
window is expected to move.
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
    tokens = re.findall(r'[MmLlZz]|-?\d*\.?\d+(?:e-?\d+)?', pathdata)
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
        x = float(tokens[index])
        y = float(tokens[index + 1])
        index += 2
        current = [current[0] + x, current[1] + y] if mode.islower() else [x, y]
        points.append(tuple(current))
    return points


def turn_angles(points):
    turns = []
    for a, b, c in zip(points, points[1:], points[2:]):
        v1 = (b[0] - a[0], b[1] - a[1])
        v2 = (c[0] - b[0], c[1] - b[1])
        n1 = math.hypot(*v1)
        n2 = math.hypot(*v2)
        if n1 > 1e-6 and n2 > 1e-6:
            cosine = max(-1.0, min(1.0, (v1[0] * v2[0] + v1[1] * v2[1]) / (n1 * n2)))
            turns.append(math.degrees(math.acos(cosine)))
    return sorted(turns)


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
        if 'write-flat-pen' not in match.group(1):
            continue
        outline = parse_path(match.group(2))
        if len(outline) < min_points:
            continue
        found += 1
        count = len(outline)
        side = outline[:count // 2 - 1]           # path1, up to the end cap
        centre = [((outline[i][0] + outline[count - 1 - i][0]) / 2,
                   (outline[i][1] + outline[count - 1 - i][1]) / 2) for i in range(count // 2)]
        turns = turn_angles(side)
        cx = statistics.mean(p[0] for p in centre)
        cy = statistics.mean(p[1] for p in centre)
        radius = statistics.mean(math.hypot(p[0] - cx, p[1] - cy) for p in centre)
        edge_radius = [math.hypot(p[0] - cx, p[1] - cy) for p in side]
        edge_dev = statistics.pstdev(edge_radius)
        print(f'stroke {found}: {len(centre)} samples, outline side {len(side)} points')
        print(f'  outline turn angle deg  q50 {quantile(turns, .5):.1f}  q75 {quantile(turns, .75):.1f}'
              f'  q90 {quantile(turns, .9):.1f}  q99 {quantile(turns, .99):.1f}  mean {statistics.mean(turns):.1f}')
        print(f'  edge distance from fitted circle (r={radius:.1f}): stddev {edge_dev:.3f} du')
    if not found:
        print('no flat-pen stroke with enough points found')
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
