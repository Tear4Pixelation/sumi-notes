#!/usr/bin/python3
# Regenerate icons/*.svg from the reicon icon library (https://reicon.dev, MIT).
#
# reicon ships its icons as a single JSON blob (data/icon-data.json in the repo at
# github.com/dqev/reicon), two weights per icon; we only ever use "Outline", 24x24.
#
#   git clone --depth 1 https://github.com/dqev/reicon /tmp/reicon
#   python3 reicon_import.py /tmp/reicon/data/icon-data.json
#   python3 embed.py icons/*.svg > res_icons.cpp
#
# Icons drawn for the toolbar redesign in Penpot (draw, ephemeral, highlight, erase*,
# select*, insert_space*, toggle_*, help) are deliberately NOT in the table below:
# reicon has no equivalent for most of them, and the ones it does have were traced
# from reicon in the first place.

import json
import os
import re
import sys

# app icon name (icons/<name>.svg) -> reicon icon name
MAPPING = {
  'arrow_down': 'arrow-down',
  'arrow_up': 'arrow-up',
  'chevron_down': 'chevron-down',
  'chevron_left': 'chevron-left',
  'chevron_right': 'chevron-right',
  'chevron_up': 'chevron-up',
  'ic_drive': 'hard-drive',
  'ic_file': 'file',
  'ic_folder': 'folder2',
  'ic_menu_accept': 'check',
  'ic_menu_add_bookmark': 'bookmark-add',
  'ic_menu_add_color': 'plus-circle',
  'ic_menu_add_doc': 'file-plus',
  'ic_menu_add_folder': 'folder-plus',
  'ic_menu_add_people': 'user-add',
  'ic_menu_add_pic': 'image-plus',
  'ic_menu_append_page': 'page',
  'ic_menu_back': 'arrow-left',
  'ic_menu_bookmark': 'bookmark',
  'ic_menu_cancel': 'x',
  'ic_menu_clock': 'clock',
  'ic_menu_file_plus': 'file-plus',
  'ic_menu_history': 'history',
  'ic_menu_import': 'import2',
  'ic_menu_cloud': 'cloud',
  'ic_menu_copy': 'copy',
  'ic_menu_screenshot': 'camera',
  'ic_menu_cut': 'scissors',
  'ic_menu_discard': 'trash2',
  'ic_menu_document': 'document-text',
  'ic_menu_doctext': 'doc-text',
  'ic_menu_drawer': 'menu',
  'ic_menu_duplicate': 'document-copy',
  'ic_menu_editbox': 'edit',
  'ic_menu_ellipsis': 'more-h',
  'ic_menu_expanddown': 'chevron-down',
  'ic_menu_expandright': 'chevron-right',
  'ic_menu_filter': 'filter',
  'ic_menu_filter_tick': 'filter-tick',
  'ic_menu_folder': 'folder2',
  'ic_menu_forward': 'arrow-right',
  'ic_menu_fullscreen': 'fullscreen',
  'ic_menu_link': 'link',
  'ic_menu_lock': 'lock',
  'ic_menu_unlock': 'lock-open',
  'ic_menu_hidden': 'eye-off',
  'ic_menu_visible': 'eye',
  'ic_menu_outline': 'list3',
  # the sidebar toolbar button, whose glyph names the edge the sidebar is docked to.  Deliberately
  # not reusing ic_menu_split_lr/_rl, which are the same reicon glyphs but mean *split view*
  'ic_menu_sidebar_left': 'sidebar-left',
  'ic_menu_sidebar_right': 'sidebar-right',
  'ic_menu_minus': 'minus',
  'ic_menu_next': 'chevron-right',
  'ic_menu_pagesel': 'layers',
  'ic_menu_pan': 'hand',
  'ic_menu_paste': 'clipboard-text',
  'ic_menu_people': 'people',
  'ic_menu_pin': 'thumbtack',
  'ic_menu_plus': 'plus',
  'ic_menu_preferences': 'sliders',
  'ic_menu_prev': 'chevron-left',
  'ic_menu_redo': 'redo',
  'ic_menu_refresh': 'refresh',
  'ic_menu_save': 'floppy2',
  'ic_menu_send': 'send',
  'ic_menu_send_now': 'plane2',
  'ic_menu_set_pen': 'pen-add',
  'ic_menu_settings': 'gear',
  'ic_menu_settings2': 'settings2',
  'ic_menu_shapes': 'shapes',
  'ic_menu_shape_head_start': 'arrow-left',
  'ic_menu_shape_head_end': 'arrow-right',
  'ic_menu_share': 'share',
  'ic_menu_split_bt': 'sidebar-bottom',
  'ic_menu_split_lr': 'sidebar-left',
  'ic_menu_split_rl': 'sidebar-right',
  'ic_menu_split_tb': 'sidebar-top',
  'ic_menu_stretch': 'scale',
  'ic_menu_undo': 'undo',
  'ic_menu_zoom': 'magnifier',
  'ic_menu_search2': 'search2',
  'ic_tag': 'hashtag2',
}

# reicon has no vertical-dots icon, so the horizontal one is rotated in place
ROTATE = {'ic_menu_overflow': ('more-h', 90)}

# the document list draws these at thumbnail size rather than toolbar size, where the outline weight
# reads as spindly; everything else in the app stays outline
FILLED = {'ic_file_fill': 'file', 'ic_folder_fill': 'folder2'}

TEMPLATE = """<?xml version="1.0" encoding="utf-8"?>
<svg version="1.2" baseProfile="tiny" xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink"
  x="0px" y="0px" viewBox="0 0 24 24" xml:space="preserve">
<!-- reicon "%s" (%s), https://reicon.dev - MIT -->
<g class="icon"%s>
  %s
</g>
</svg>
"""


def shapes(code):
  # stroke-drawn reicon icons leave fill unset, which would pick up the fill: var(--icon)
  # the theme sets on .icon and flood the icon; the fill-drawn ones must keep inheriting it
  code = re.sub(r'<(path|circle|rect|ellipse|line|polyline|polygon) (?![^>]*fill=)',
                r'<\1 fill="none" ', code) if 'stroke="currentColor"' in code else code
  return code.replace('/><', '/>\n  <')


def main(datafile):
  data = json.load(open(datafile))
  icons = {name: icon for group in data['categories'].values() for name, icon in group['icons'].items()}
  outdir = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'icons')
  jobs = [(app, src, 'Outline', None) for app, src in MAPPING.items()]
  jobs += [(app, src, 'Outline', angle) for app, (src, angle) in ROTATE.items()]
  jobs += [(app, src, 'Filled', None) for app, src in FILLED.items()]
  for app, src, weight, angle in sorted(jobs):
    if src not in icons:
      sys.exit('reicon icon not found: ' + src)
    if weight not in icons[src]['weights']:
      sys.exit('reicon icon %s has no %s weight' % (src, weight))
    transform = ' transform="rotate(%d 12 12)"' % angle if angle else ''
    svg = TEMPLATE % (src, weight.lower(), transform, shapes(icons[src]['weights'][weight]['code']))
    with open(os.path.join(outdir, app + '.svg'), 'w') as f:
      f.write(svg)
  print('wrote %d icons to %s' % (len(jobs), outdir))


if __name__ == '__main__':
  main(sys.argv[1] if len(sys.argv) > 1 else '/tmp/reicon/data/icon-data.json')
