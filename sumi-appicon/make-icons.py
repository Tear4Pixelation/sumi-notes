#!/usr/bin/env python3
"""Regenerates every platform app icon from scribbleres/icons/sumi_logo.svg (black quill on white).

Run from anywhere: python3 sumi-appicon/make-icons.py   (needs rsvg-convert and ImageMagick).
sumi_logo.svg is the themed in-app logo, cleaned from sumi-appicon/logo_v2.svg - see README.md.
"""
import os, struct, subprocess, tempfile
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
def path(*parts): return os.path.normpath(os.path.join(ROOT, *parts))
def run(*cmd): subprocess.run(cmd, check=True)

tmp = tempfile.mkdtemp()
black = os.path.join(tmp, 'black.svg')
text = open(path('scribbleres/icons/sumi_logo.svg')).read().replace('currentColor', '#000').replace('class="icon"', 'fill="#000"')
open(black, 'w').write(text)
big, glyph, master = (os.path.join(tmp, name) for name in ('big.png', 'glyph.png', 'master.png'))
run('rsvg-convert', '-h', '2400', black, '-o', big)
run('magick', big, '-trim', '+repage', '-resize', 'x640', glyph)
run('magick', '-size', '1024x1024', 'xc:white', glyph, '-gravity', 'center', '-composite', '-alpha', 'off', master)

def png(size, out):  # opaque RGB: App Store icons must have no alpha
    run('magick', master, '-resize', f'{size}x{size}', '-type', 'TrueColor', f'PNG24:{out}')

for appiconset in ('scribbleres', 'xcode/Write/Write', 'xcode/WriteIAP/Write'):
    folder = path(appiconset, 'Assets.xcassets/AppIcon.appiconset')
    for name, size in (('1024x1024', 1024), ('60x60@2x', 120), ('60x60@3x', 180), ('76x76@2x', 152), ('83.5x83.5@2x', 167)):
        png(size, f'{folder}/AppIcon{name}.png')
png(512, path('scribbleres/write_512.png'))
png(144, path('scribbleres/linux/Sumi144x144.png'))
png(144, path('syncscribble/android/app/src/main/res/mipmap-xxhdpi/icon.png'))

# Windows: ImageMagick writes a proper multi-size .ico
sizes = (16, 24, 32, 48, 64, 128, 256)
for size in sizes: png(size, os.path.join(tmp, f'ico{size}.png'))
run('magick', *[os.path.join(tmp, f'ico{size}.png') for size in sizes], path('syncscribble/windows/write.ico'))

# macOS: no icns writer here (ImageMagick emits a PNG), so assemble PNG chunks by hand
body = b''
for tag, size in (('icp4', 16), ('icp5', 32), ('icp6', 64), ('ic07', 128), ('ic08', 256), ('ic09', 512), ('ic10', 1024)):
    chunk = os.path.join(tmp, f'icns{size}.png'); png(size, chunk)
    data = open(chunk, 'rb').read()
    body += tag.encode() + struct.pack('>I', len(data) + 8) + data
open(path('scribbleres/macos/write.icns'), 'wb').write(b'icns' + struct.pack('>I', len(body) + 8) + body)
# iOS launch screen: its background is black (LaunchView.xib), so a white quill on transparent
launch = os.path.join(tmp, 'launch.png')
run('magick', big, '-trim', '+repage', '-resize', 'x400', '-background', 'none', '-gravity', 'center', '-extent', '512x512',
    '-channel', 'RGB', '-negate', '+channel', launch)
for folder in ('scribbleres', 'xcode/Write/Write', 'xcode/WriteIAP/Write'):
    run('cp', launch, path(folder, 'Assets.xcassets/LaunchImage.imageset/LaunchImage.png'))
print('icons regenerated')
