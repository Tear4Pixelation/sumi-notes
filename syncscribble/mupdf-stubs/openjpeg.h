// Stub for builds with FZ_ENABLE_JPX=0 (no JPEG 2000 support, no openjpeg submodule).
//
// MuPDF's source/fitz/encode-jpx.c includes <openjpeg.h> *outside* its `#if FZ_ENABLE_JPX`
// guard, so the file cannot be compiled at all without openjpeg headers present - even though
// with JPX disabled it contains nothing but a throwing stub for fz_write_pixmap_as_jpx().
// That stub is still needed, because source/pdf/pdf-image-rewriter.c calls the function
// without a guard of its own.  An empty header is enough to get the file compiling; none of
// the openjpeg declarations are reachable with FZ_ENABLE_JPX=0.
//
// Remove this file if the openjpeg submodule is ever enabled in mupdf.mk.

#ifndef WRITE_OPENJPEG_STUB_H
#define WRITE_OPENJPEG_STUB_H
#endif
