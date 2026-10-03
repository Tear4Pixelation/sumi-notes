#!/usr/bin/env python3
"""Decode a Noteful notebook export (.noteful) and render its pages to SVG.

Usage: noteful-dump.py NOTEBOOK.noteful [OUTDIR] [--tree]

Writes OUTDIR/summary.json (title, tags, outline, layers, pages) and one
OUTDIR/page-NN.svg per page: the background (JPEG embedded, PDF noted), the
paper template's ruling, and the ink. --tree prints the raw field tree of the
document record instead, for further reverse engineering.

This is the check behind a future C++ importer; the format is undocumented and
everything here was worked out from one file, so treat unknown fields as unknown.

File layout (all integers big-endian)
-------------------------------------
  "AA BB CC DE" magic, then the notebook header record, then raw objects
  (assets and records) back to back, then an index record, then a 16 byte
  trailer: magic, u64 index offset, u32 index length.

  The index maps object ids to (offset, length): field 10 ids, 11 offsets,
  12 lengths. Field 3 lists the asset ids (page backgrounds: PDF or JPEG,
  stored verbatim), field 4 the ink record ids. "n:<id>" is the notebook
  header, "d:<id>" the document record.

Records are a sequence of fields: u16 field id, u8 flags, u8 type, value.
  flags 0x04  array: u32 count, then that many values of the type
  flags 0x08  value is followed by a u64 timestamp (us since 2001) - the
              per-field last-writer-wins clock Noteful syncs with
  type 1 u8, 0x11 u16 (enum), 3/0x12/0x14 32-bit, 2/0x13 i64, 4/0x20 double,
       0x21 two doubles, 0x23 four doubles (a rect), 5 string/bytes, 6 blob, 7 nested record (the last
       three: u32 length). Types 3 and 0x14 are f32 or i32 depending on field.

Collections (pages, outline, layers, ...) are a nested record with
  1 ids, 2 timestamps, 3 alive flags (0 = deleted), 0 the live objects.
Order within a collection comes from a fractional-index string ("+EWFjoL").

Ink: an ink record's field 2 is one blob holding all strokes of a page:
  f1 01  stroke: u64 id, u16 flags, 3 x u64 timestamps, u32 layer id, f64 width,
         u64 point count n; if flags & 2, u64 m (= n-1) and two arrays of m
         f32 (per segment, mostly 1.0 - pressure?); then points:
           n <= 4: n x (f32 x, f32 y)
           n > 4:  f32 x, f32 width, f32 y, f32 height (bounding box), then
                   n x (u16 x, u16 y) normalised to that box;
                   if flags & 1 (pressure pens): f32, f32 (the pressure range),
                   then n x (u16 x, u16 y, u16 pressure) instead
  f1 02  follows a stroke and sets its colour: 4 x f64 RGBA, then 10 bytes.
"""
import base64
import json
import math
import os
import struct
import sys

MAGIC = b'\xaa\xbb\xcc\xde'
FIXED_SIZE = {1: 1, 2: 8, 3: 4, 4: 8, 0x11: 2, 0x12: 4, 0x13: 8, 0x14: 4, 0x20: 8, 0x21: 16, 0x23: 32}
LENGTH_PREFIXED = (5, 6, 7)
FLAG_ARRAY = 0x04
FLAG_TIMESTAMP = 0x08


class Field:
    __slots__ = ('fid', 'flags', 'type', 'value', 'timestamp')

    def __init__(self, fid, flags, type_, value, timestamp):
        self.fid, self.flags, self.type, self.value, self.timestamp = fid, flags, type_, value, timestamp


def read_value(buf, pos, type_):
    if type_ in FIXED_SIZE:
        size = FIXED_SIZE[type_]
        return buf[pos:pos + size], pos + size
    if type_ in LENGTH_PREFIXED:
        length = struct.unpack_from('>I', buf, pos)[0]
        return buf[pos + 4:pos + 4 + length], pos + 4 + length
    raise ValueError('unknown field type 0x%x at 0x%x' % (type_, pos))


def record(buf):
    """Parse a record into {field id: Field}."""
    fields = {}
    pos = 0
    while pos < len(buf):
        fid, flags, type_ = struct.unpack_from('>HBB', buf, pos)
        pos += 4
        if flags & FLAG_ARRAY:
            num = struct.unpack_from('>I', buf, pos)[0]
            pos += 4
            value = []
            for _ in range(num):
                item, pos = read_value(buf, pos, type_)
                value.append(item)
        else:
            value, pos = read_value(buf, pos, type_)
        timestamp = None
        if flags & FLAG_TIMESTAMP:
            timestamp = struct.unpack_from('>Q', buf, pos)[0]
            pos += 8
        fields[fid] = Field(fid, flags, type_, value, timestamp)
    return fields


def text(field):
    return field.value.decode('utf-8') if field else None


def i64(raw):
    return struct.unpack('>q', raw)[0]


def collection(field):
    """Live objects of a collection field, as parsed records, keyed by id."""
    coll = record(field.value)
    alive = {oid.decode(): flag[0] for oid, flag in zip(coll[1].value, coll[3].value)}
    objects = {}
    for raw in coll[0].value:
        obj = record(raw)
        oid = text(obj[1])
        if alive.get(oid, 1):
            objects[oid] = obj
    return objects


class Notebook:
    def __init__(self, path):
        with open(path, 'rb') as file:
            self.data = data = file.read()
        if data[:4] != MAGIC or data[-16:-12] != MAGIC:
            raise ValueError('not a .noteful file')
        index_offset, index_length = struct.unpack('>QI', data[-12:])
        index = record(data[index_offset:index_offset + index_length])
        self.objects = {}
        for oid, offset, length in zip(index[10].value, index[11].value, index[12].value):
            self.objects[oid.decode()] = data[i64(offset):i64(offset) + i64(length)]
        self.asset_ids = [oid.decode() for oid in index[3].value]
        notebook_id = index[2].value[0].decode()
        self.header = record(self.objects['n:' + notebook_id])
        self.doc = record(self.objects['d:' + notebook_id])

    def title(self):
        return text(self.header[3])

    def pages(self):
        pages = collection(self.doc[2])
        ordered = sorted(pages.values(), key=lambda page: text(page[5]))
        result = []
        for page in ordered:
            ink_ref = record(page[2].value)
            template = record(page[4].value)
            size = struct.unpack('>2d', template[1].value)
            entry = {'id': text(page[1]), 'ink': text(ink_ref[0]), 'size': size}
            if template[3].type == 5:
                # paper: 3 asset (the paper rendered as a small PDF), 4 sha1, 5 class, 6 JSON, 7 ?
                entry.update(asset=text(template[3]), assetPage=0, cls=text(template[5]),
                             template=json.loads(text(template[6])))
            else:
                # imported PDF or image: 3 page number in the asset, 4 asset, 5 ?
                entry.update(asset=text(template[4]), assetPage=i64(template[3].value), cls=None, template=None)
                if 9 in template:
                    # a cropped page: where the whole asset page is drawn (x, y, w, h), relative to this page
                    entry['assetRect'] = struct.unpack('>4d', template[9].value)
            result.append(entry)
        return result

    def outline(self):
        entries = collection(self.doc[5])
        return [{'id': oid, 'page': text(entry[2]), 'parent': text(entry[3]) or None,
                 'title': text(entry[4]), 'order': text(entry[5])}
                for oid, entry in entries.items()]

    def layers(self):
        """Bottom to top. Layer: 1 name, 2 u32 id (Layer 1 is 0; strokes and elements
        refer to it), 3 order key, 4/5/7 u8 flags (hidden/locked? - only seen as 0), 6 f32 opacity."""
        layers = []
        for raw in record(self.doc[3].value)[0].value:
            layer = record(raw)
            layers.append({'name': text(layer[1]), 'id': struct.unpack('>I', layer[2].value)[0],
                           'order': text(layer[3]), 'opacity': struct.unpack('>f', layer[6].value)[0],
                           'flags': [layer[fid].value[0] for fid in (4, 5, 7) if fid in layer]})
        return sorted(layers, key=lambda layer: layer['order'])

    def strokes(self, ink_id):
        if ink_id not in self.objects:  # a page never written on has no ink record
            return []
        ink = record(self.objects[ink_id])
        return parse_strokes(ink[2].value) if 2 in ink else []

    def elements(self, ink_id):
        """Shapes, text boxes and images: the collection in the ink record's field 5."""
        if ink_id not in self.objects:
            return []
        ink = record(self.objects[ink_id])
        return [parse_element(element) for element in collection(ink[5]).values()] if 5 in ink else []


def floats(field, fmt='>d'):
    return [struct.unpack(fmt, raw)[0] for raw in field.value]


def parse_element(element):
    """Element: 1 id, 2 {1: frame [centre x, centre y, w, h, rotation?]}, 6 body with 1 kind,
    9 u16 1 = drawn with the highlighter (stored opaque; Noteful draws it translucent).

    kind 20 line, 21 curve: 2 size, 7 style {7 RGBA f32, 2 width}, 13 path {1 xy pairs, 2 ops}
    kind 19 text box: 4 {2 text runs, 5 fonts, ...}, 14 the #tags found in the text
    kind 1 image: 10 asset, 2 display size, 11 pixel size, 12 crop path (display units; optional)
    kind 12 polygon: a path, like 20/21
    kind 6 ellipse (assumed - only circles seen) and kind 3 regular polygon (20 = side count):
        no path, just the frame and style
    The frame's x, y is its centre: read as the corner, highlighters sat half their length right of
    their word and tag boxes ran off the page; returned here as the usual top-left x, y, w, h.
    Frame[4] is a rotation in radians, applied about the frame's centre (assumed).
    Field 4 is the u32 layer id. Path ops: 0 move (1 point), 1 line (1), 3 cubic (3), 4 close (0). Path points are
    relative to the frame origin.
    """
    frame = floats(record(element[2].value)[1])
    frame[0] -= frame[2] / 2
    frame[1] -= frame[3] / 2
    body = record(element[6].value)
    result = {'id': text(element[1]), 'kind': i64(body[1].value), 'frame': frame,
              'layer': struct.unpack('>I', element[4].value)[0],
              'highlighter': 9 in element and struct.unpack('>H', element[9].value)[0] == 1}
    if 7 in body:
        style = record(body[7].value)
        result['color'] = floats(record(style[7].value)[0], '>f')
        result['width'] = struct.unpack('>d', style[2].value)[0]
    if 13 in body:
        result['path'] = parse_path(body[13])
    if 20 in body:
        result['sides'] = int(struct.unpack('>d', body[20].value)[0])
    if 4 in body:
        result['text'] = ''.join(run.decode('utf-8') for run in record(body[4].value)[2].value)
    if 14 in body:
        result['tags'] = [tag.decode('utf-8') for tag in body[14].value]
    if 10 in body:
        result['asset'] = text(body[10])
        result['displaySize'] = struct.unpack('>2d', body[2].value)
        width, height = result['displaySize']
        # no crop path: the whole image
        result['crop'] = parse_path(body[12]) if 12 in body else {'points': [(0, 0), (width, height)], 'ops': [0, 1]}
    return result


def parse_path(field):
    path = record(field.value)
    coords = floats(path[1])
    ops = [struct.unpack('>i', raw)[0] for raw in path[2].value]
    return {'points': list(zip(coords[0::2], coords[1::2])), 'ops': ops}


PATH_OPS = {0: ('M', 1), 1: ('L', 1), 2: ('Q', 2), 3: ('C', 3), 4: ('Z', 0)}
KIND_POLYGON = 3
KIND_ELLIPSE = 6


def polygon_points(sides, x, y, width, height):
    """Regular polygon inscribed in the frame's ellipse, first vertex at the top (assumed)."""
    return [(x + width / 2 * (1 + math.cos(-math.pi / 2 + 2 * math.pi * i / sides)),
             y + height / 2 * (1 + math.sin(-math.pi / 2 + 2 * math.pi * i / sides))) for i in range(sides)]


def path_d(path, offset_x=0, offset_y=0):
    parts = []
    points = iter(path['points'])
    for op in path['ops']:
        letter, count = PATH_OPS[op]
        coords = [next(points) for _ in range(count)]
        parts.append(letter + ' '.join('%.2f,%.2f' % (x + offset_x, y + offset_y) for x, y in coords))
    return ' '.join(parts)


def parse_strokes(blob):
    strokes = []
    pos = 0
    while pos < len(blob):
        tag = blob[pos:pos + 2]
        if tag == b'\xf1\x01':
            stroke_id, stroke_flags = struct.unpack_from('>QH', blob, pos + 2)
            layer, width, count = struct.unpack_from('>IdQ', blob, pos + 36)
            pos += 56
            if stroke_flags & 2:
                # two arrays of count-1 f32 per segment, mostly 1.0 - pressure? ignored
                segments, = struct.unpack_from('>Q', blob, pos)
                pos += 8 + 8 * segments
            pressure = None
            if count <= 4:  # at 4 both layouts are 32 bytes; raw floats it is (checked)
                if stroke_flags & 1:
                    raise ValueError('short pressure stroke at %d: layout not seen yet' % pos)
                points = [struct.unpack_from('>2f', blob, pos + 8 * i) for i in range(count)]
                pos += 8 * count
            else:
                box_x, box_w, box_y, box_h = struct.unpack_from('>4f', blob, pos)
                pos += 16
                if stroke_flags & 1:
                    # pressure: f32 first, f32 second (the range - order unconfirmed), then x, y, p triples
                    first, second = struct.unpack_from('>2f', blob, pos)
                    raw = struct.unpack_from('>%dH' % (3 * count), blob, pos + 8)
                    pos += 8 + 6 * count
                    pressure = [second + raw[i + 2] / 65535 * (first - second) for i in range(0, len(raw), 3)]
                    raw = [value for i in range(0, len(raw), 3) for value in raw[i:i + 2]]
                else:
                    raw = struct.unpack_from('>%dH' % (2 * count), blob, pos)
                    pos += 4 * count
                points = [(box_x + raw[i] / 65535 * box_w, box_y + raw[i + 1] / 65535 * box_h)
                          for i in range(0, len(raw), 2)]
            strokes.append({'id': stroke_id, 'layer': layer, 'width': width, 'points': points,
                            'pressure': pressure, 'color': (0, 0, 0, 1)})
        elif tag == b'\xf1\x02':
            strokes[-1]['color'] = struct.unpack_from('>4d', blob, pos + 2)
            pos += 44
        else:
            raise ValueError('unknown ink record %s at %d' % (tag.hex(), pos))
    return strokes


# Page units are pixels at 132 dpi (A4 = 1091.34 wide); the paper templates' lh/lw are in
# points, as the template's own PDF confirms: a 0.5 pt line every 20 pt, the first 20 pt
# from the top/left edge, in rgb(0.604, 0.596, 0.533).
UNITS_PER_POINT = 132 / 72
RULE_COLOR = '#9a9888'


def ruling_svg(template, width, height):
    if not template or 'lh' not in template:
        return ''
    pitch = template['lh'] * UNITS_PER_POINT
    rule_width = template.get('lw', 0.5) * UNITS_PER_POINT
    lines = ['<g stroke="%s" stroke-width="%.3g">' % (RULE_COLOR, rule_width)]
    y = pitch
    while y < height:
        lines.append('<line x1="0" y1="%.2f" x2="%.2f" y2="%.2f"/>' % (y, width, y))
        y += pitch
    if template.get('lt') == 2:  # grid; other line types not seen yet
        x = pitch
        while x < width:
            lines.append('<line x1="%.2f" y1="0" x2="%.2f" y2="%.2f"/>' % (x, x, height))
            x += pitch
    lines.append('</g>')
    return '\n'.join(lines)


def pdf_page_image(pdf, page_index, width, height, rect=None):
    """The PDF page rasterised with pdftoppm, as an <image> filling the page, or `rect` (x, y, w, h)
    for a cropped page; a note if pdftoppm is missing."""
    import subprocess
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        with open(os.path.join(tmp, 'in.pdf'), 'wb') as file:
            file.write(pdf)
        number = str(page_index + 1)
        try:
            subprocess.run(['pdftoppm', '-f', number, '-l', number, '-r', '100', '-jpeg', '-singlefile',
                            os.path.join(tmp, 'in.pdf'), os.path.join(tmp, 'out')], check=True)
            with open(os.path.join(tmp, 'out.jpg'), 'rb') as file:
                jpeg = file.read()
        except (OSError, subprocess.CalledProcessError):
            return '<text x="10" y="20" font-size="14" fill="#c00">PDF page %s (pdftoppm failed)</text>' % number
    x, y, width, height = rect or (0, 0, width, height)
    return ('<image x="%g" y="%g" width="%g" height="%g" preserveAspectRatio="none" href="data:image/jpeg;base64,%s"/>'
            % (x, y, width, height, base64.b64encode(jpeg).decode()))


def element_svg(notebook, element):
    x, y, width, height, rotation = (element['frame'] + [0])[:5]
    # frame[4] is a rotation in radians; about the frame's centre (assumed - see the docstring)
    rotate = (' transform="rotate(%.4f %.2f %.2f)"' % (math.degrees(rotation), x + width / 2, y + height / 2)
              if rotation else '')
    if 'color' in element:
        red, green, blue, alpha = element['color']
        if element['highlighter']:
            alpha *= 0.5  # Sumi's marker alpha; Noteful's own is not stored
        stroke = ('fill="none" stroke="rgb(%d,%d,%d)" stroke-opacity="%.3g" stroke-width="%.3g" stroke-linecap="round"'
                  % (red * 255, green * 255, blue * 255, alpha, element['width']))
    if 'path' in element:
        return '<path %s%s d="%s"/>' % (stroke, rotate, path_d(element['path'], x, y))
    if element['kind'] == KIND_ELLIPSE and 'color' in element:
        return ('<ellipse %s%s cx="%.2f" cy="%.2f" rx="%.2f" ry="%.2f"/>'
                % (stroke, rotate, x + width / 2, y + height / 2, width / 2, height / 2))
    if element['kind'] == KIND_POLYGON and 'color' in element:
        points = polygon_points(element['sides'], x, y, width, height)
        return '<polygon %s%s points="%s"/>' % (stroke, rotate, ' '.join('%.2f,%.2f' % point for point in points))
    if 'text' in element:
        return ('<text x="%.2f" y="%.2f" font-family="Helvetica" font-size="16">%s</text>'
                % (x, y + 16, element['text'].replace('&', '&amp;').replace('<', '&lt;')))
    if 'asset' in element:
        # the frame shows the crop region's bounding box, scaled to fit
        crop_xs = [point[0] for point in element['crop']['points']]
        crop_ys = [point[1] for point in element['crop']['points']]
        scale = width / (max(crop_xs) - min(crop_xs))
        display_w, display_h = element['displaySize']
        clip = 'clip-%s' % element['id']
        asset = notebook.objects.get(element['asset'], b'')
        return ('<clipPath id="%s"><rect x="%.2f" y="%.2f" width="%.2f" height="%.2f"/></clipPath>'
                '<image clip-path="url(#%s)" preserveAspectRatio="none" x="%.2f" y="%.2f" width="%.2f" height="%.2f" '
                'href="data:image/jpeg;base64,%s"/>'
                % (clip, x, y, width, height, clip, x - min(crop_xs) * scale, y - min(crop_ys) * scale,
                   display_w * scale, display_h * scale, base64.b64encode(asset).decode()))
    return ''


def content_bottom(strokes, elements):
    bottoms = [max(y for x, y in stroke['points']) for stroke in strokes]
    bottoms += [element['frame'][1] + element['frame'][3] for element in elements]
    return max(bottoms, default=0)


def page_svg(notebook, page):
    width, height = page['size']
    strokes = notebook.strokes(page['ink'])
    elements = notebook.elements(page['ink'])
    # ink may run past the page's nominal height; show all of it (Sumi pages grow anyway)
    page_height = height
    height = max(height, content_bottom(strokes, elements) + 20)
    parts = ['<svg xmlns="http://www.w3.org/2000/svg" width="%g" height="%g" viewBox="0 0 %g %g">'
             % (width, height, width, height)]
    template = page['template'] or {}
    paper = template.get('pc')
    parts.append('<rect width="100%%" height="100%%" fill="#%06x"/>' % (paper if paper is not None else 0xffffff))
    asset = notebook.objects.get(page['asset'] or '')
    if asset and asset[:3] == b'\xff\xd8\xff':
        parts.append('<image width="%g" height="%g" href="data:image/jpeg;base64,%s"/>'
                     % (width, page_height, base64.b64encode(asset).decode()))
    elif asset and asset[:4] == b'%PDF' and 'lh' not in template:
        parts.append(pdf_page_image(asset, page['assetPage'], width, page_height, page.get('assetRect')))
    parts.append(ruling_svg(template, width, height))
    if height > page_height:
        parts.append('<line x1="0" y1="%.2f" x2="%.2f" y2="%.2f" stroke="#c00" stroke-dasharray="8 8"/>'
                     % (page_height, width, page_height))
    layers = notebook.layers()
    known = {layer['id'] for layer in layers}
    # an id missing from the table falls back to the bottom layer rather than vanishing
    layer_of = lambda item: item['layer'] if item['layer'] in known else layers[0]['id']
    for layer in layers:
        parts.append('<g id="layer-%08x" data-name="%s" opacity="%g">' % (layer['id'], layer['name'], layer['opacity']))
        parts.extend(element_svg(notebook, element) for element in elements if layer_of(element) == layer['id'])
        for stroke in strokes:
            if layer_of(stroke) != layer['id']:
                continue
            red, green, blue, alpha = stroke['color']
            path = ' '.join('%.2f,%.2f' % point for point in stroke['points'])
            parts.append('<polyline fill="none" stroke="rgb(%d,%d,%d)" stroke-opacity="%.3g" stroke-width="%.3g" '
                         'stroke-linecap="round" stroke-linejoin="round" points="%s"/>'
                         % (red * 255, green * 255, blue * 255, alpha, stroke['width'], path))
        parts.append('</g>')
    parts.append('</svg>')
    return '\n'.join(parts)


def print_tree(buf, indent=0):
    for field in record(buf).values():
        values = field.value if isinstance(field.value, list) else [field.value]
        label = '  ' * indent + '%d t%x%s' % (field.fid, field.type, '*' if field.timestamp else '')
        if field.type == 7:
            print(label + (' [%d]' % len(values) if isinstance(field.value, list) else ''))
            for value in values[:3]:
                try:
                    print_tree(value, indent + 1)
                except (ValueError, struct.error):
                    print('  ' * (indent + 1) + 'raw ' + value[:32].hex())
        else:
            print(label, ', '.join(repr(value)[:80] for value in values[:4]))


def main():
    args = [arg for arg in sys.argv[1:] if not arg.startswith('--')]
    if not args:
        sys.exit(__doc__)
    notebook = Notebook(args[0])
    if '--tree' in sys.argv:
        print_tree(notebook.objects['d:' + notebook.header[1].value.decode()])
        return
    outdir = args[1] if len(args) > 1 else os.path.splitext(os.path.basename(args[0]))[0]
    os.makedirs(outdir, exist_ok=True)
    pages = notebook.pages()
    tags = []
    for number, page in enumerate(pages, 1):
        with open(os.path.join(outdir, 'page-%02d.svg' % number), 'w') as file:
            file.write(page_svg(notebook, page))
        page['strokes'] = len(notebook.strokes(page['ink']))
        elements = notebook.elements(page['ink'])
        page['elements'] = [element['kind'] for element in elements]
        tags += [tag for element in elements for tag in element.get('tags', []) if tag not in tags]
    summary = {'title': notebook.title(), 'tags': tags, 'layers': notebook.layers(),
               'outline': notebook.outline(), 'pages': pages}
    with open(os.path.join(outdir, 'summary.json'), 'w') as file:
        json.dump(summary, file, indent=1, ensure_ascii=False)
    print('%s: %d pages, %d strokes, %d outline entries -> %s'
          % (summary['title'], len(pages), sum(page['strokes'] for page in pages), len(summary['outline']), outdir))


if __name__ == '__main__':
    main()
