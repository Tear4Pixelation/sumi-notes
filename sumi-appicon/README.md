# Sumi app icon

- `logo_v2.svg` - the current logo (quill in an inkwell), as drawn in Inkscape. It carries boolean-op leftovers
  and an embedded reference image; only three shapes render (feather, bowl, rim arc).
- `../scribbleres/icons/sumi_logo.svg` - those three shapes cleaned up, themed through `.icon`/`currentColor`.
  This is the in-app logo (toolbar title button) and the source of every app icon. After redrawing the logo,
  redo this file by hand, then `make res_icons.cpp` in `scribbleres/`.
- `make-icons.py` - renders the black-on-white iOS (3 asset catalogs), macOS, Windows, Android and Linux icons
  and the white-on-black iOS launch image from `sumi_logo.svg`. iOS PNGs must stay opaque RGB.
- `drafts/` - earlier versions (`logo_v1.svg` was the first shipped one).
