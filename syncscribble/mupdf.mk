# MuPDF (PDF import) - makefile fragment shared by all platforms
#
# MuPDF is compiled *inline* into Write rather than linked as a prebuilt library, the same way
# nanovg/miniz/pugixml are.  This is deliberate: Write maintains six hand-written platform
# makefiles (unix, mac, ios, msvc, wasm, android/ndk-build) and building MuPDF's own makefile
# for each of those toolchains is where all the cost of a PDF dependency would go.  Compiling
# the C sources with Write's existing per-platform rules means PDF support lands on every
# platform at once with no new toolchain.
#
# This is only possible because MuPDF commits its *generated* sources to git
# (generated/resources/fonts/urw/*.c, source/pdf/cmaps/*.h, source/fitz/icc/*.h,
# source/pdf/js/util.js.h, source/html/css-properties.h), so no host codegen step is required.
#
# Build is trimmed to "PDF reader only": no XPS/SVG/CBZ/EPUB/HTML/office handlers, no
# JavaScript, no barcodes, no OCR, no ICC (drops lcms2), no JPEG 2000 (drops openjpeg), no
# brotli, and no bundled fonts beyond the base 14 (the Noto/CJK/SIL font dumps are *not* in
# git, so TOFU* is mandatory here, not merely a size choice).
#
# Required submodules (see `make mupdf-submodules`):
#   mupdf, mupdf/thirdparty/{freetype,libjpeg,zlib,jbig2dec,mujs}
#
# mujs is required even with FZ_ENABLE_JS=0: fitz compiles its regexp engine in for text search.
#
# Note zlib and miniz coexist safely: miniz only ever defines mz_* symbols, its zlib-compatible
# names are preprocessor macros, and no translation unit includes both headers.

# Directory of this file, so wildcards work regardless of make's cwd (ndk-build runs from
# android/app and includes syncscribble/Makefile by absolute path).
MUPDF_MK_DIR := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
MUPDF_DIR := $(MUPDF_MK_DIR)/../mupdf
MUPDF_TP := $(MUPDF_DIR)/thirdparty

# emit paths relative to syncscribble/ (i.e. ../mupdf/...) so they match the rest of SOURCES
mupdf_rel = $(patsubst $(MUPDF_MK_DIR)/%,%,$(1))

MUPDF_SRC := $(sort $(wildcard $(MUPDF_DIR)/source/fitz/*.c))
MUPDF_SRC += $(sort $(wildcard $(MUPDF_DIR)/source/pdf/*.c))
MUPDF_SRC += $(sort $(wildcard $(MUPDF_DIR)/generated/resources/fonts/urw/*.c))

# freetype: module list mirrors mupdf/Makelists (slim build - no bdf/pcf/pfr/type42/winfonts)
MUPDF_SRC += $(addprefix $(MUPDF_TP)/freetype/src/, \
  base/ftbase.c base/ftbbox.c base/ftbitmap.c base/ftdebug.c base/ftfstype.c base/ftgasp.c \
  base/ftglyph.c base/ftinit.c base/ftstroke.c base/ftsynth.c base/ftsystem.c base/fttype1.c \
  cff/cff.c cid/type1cid.c psaux/psaux.c pshinter/pshinter.c psnames/psnames.c \
  raster/raster.c sfnt/sfnt.c smooth/smooth.c truetype/truetype.c type1/type1.c)

# zlib (FlateDecode); gz*.c deliberately omitted - mupdf does not use the gzFile API
MUPDF_SRC += $(addprefix $(MUPDF_TP)/zlib/, \
  adler32.c compress.c crc32.c deflate.c inffast.c inflate.c inftrees.c trees.c uncompr.c zutil.c)

# libjpeg (DCTDecode)
MUPDF_SRC += $(addprefix $(MUPDF_TP)/libjpeg/, \
  jaricom.c jcapimin.c jcapistd.c jcarith.c jccoefct.c jccolor.c jcdctmgr.c jchuff.c jcinit.c \
  jcmainct.c jcmarker.c jcmaster.c jcomapi.c jcparam.c jcprepct.c jcsample.c jdapimin.c \
  jdapistd.c jdarith.c jdatadst.c jdatasrc.c jdcoefct.c jdcolor.c jddctmgr.c jdhuff.c jdinput.c \
  jdmainct.c jdmarker.c jdmaster.c jdmerge.c jdpostct.c jdsample.c jdtrans.c jerror.c \
  jfdctflt.c jfdctfst.c jfdctint.c jidctflt.c jidctfst.c jidctint.c jmemmgr.c jquant1.c \
  jquant2.c jutils.c)

# jbig2dec (JBIG2Decode - common in scanned PDFs)
MUPDF_SRC += $(addprefix $(MUPDF_TP)/jbig2dec/, \
  jbig2.c jbig2_arith.c jbig2_arith_iaid.c jbig2_arith_int.c jbig2_generic.c jbig2_halftone.c \
  jbig2_huffman.c jbig2_hufftab.c jbig2_image.c jbig2_mmr.c jbig2_page.c jbig2_refinement.c \
  jbig2_segment.c jbig2_symbol_dict.c jbig2_text.c)

SOURCES += $(call mupdf_rel,$(MUPDF_SRC))

# $(MUPDF_DIR)/source is required: fitz sources reach mujs via "../thirdparty/mujs/regexp.h"
MUPDF_INC := $(MUPDF_DIR) $(MUPDF_DIR)/source $(MUPDF_DIR)/include $(MUPDF_DIR)/scripts/freetype \
  $(MUPDF_DIR)/scripts/libjpeg $(MUPDF_TP)/freetype/include $(MUPDF_TP)/zlib \
  $(MUPDF_TP)/libjpeg $(MUPDF_TP)/jbig2dec $(MUPDF_MK_DIR)/mupdf-stubs
# -isystem: mupdf and its thirdparty libs are not warning-clean under Write's -Wall -Wshadow
INCSYS += $(call mupdf_rel,$(MUPDF_INC))

# document handlers / features we do not ship
DEFS += FZ_ENABLE_XPS=0 FZ_ENABLE_SVG=0 FZ_ENABLE_CBZ=0 FZ_ENABLE_IMG=0
DEFS += FZ_ENABLE_HTML=0 FZ_ENABLE_EPUB=0 FZ_ENABLE_FB2=0 FZ_ENABLE_MOBI=0
DEFS += FZ_ENABLE_OFFICE=0 FZ_ENABLE_TXT=0 FZ_ENABLE_MD=0 FZ_ENABLE_HTML_ENGINE=0
DEFS += FZ_ENABLE_JS=0 FZ_ENABLE_BARCODE=0
DEFS += FZ_ENABLE_OCR_OUTPUT=0 FZ_ENABLE_DOCX_OUTPUT=0 FZ_ENABLE_ODT_OUTPUT=0
# ICC drops lcms2, JPX drops openjpeg, BROTLI drops brotli - none of them are submodules here
DEFS += FZ_ENABLE_ICC=0 FZ_ENABLE_JPX=0 FZ_ENABLE_BROTLI=0
# spot rendering forces the slow generic N-component plotter; we only ever render to RGB
DEFS += FZ_ENABLE_SPOT_RENDERING=0
# bundled fonts: only the base 14 (URW) are pre-generated in git, so everything else must be off
DEFS += TOFU TOFU_CJK TOFU_EMOJI TOFU_HISTORIC TOFU_SYMBOL TOFU_SIL
# NO_CJK (undocumented, see source/pdf/pdf-cmap-load.c) drops the 60-odd predefined CJK CMap tables,
# which are 0.84 MB of pure data - by far the largest single item left in the binary.  It only
# matters for CJK text using a *predefined* encoding (UniGB-UCS2-H and friends); Identity-H/V, which
# is what essentially all modern PDFs with embedded CJK fonts use, still works.  This goes with
# TOFU_CJK above: with no CJK font data bundled, non-embedded CJK is already unrenderable, so the
# CMaps could only have helped embedded fonts that also use a predefined encoding.  Remove both if
# CJK PDFs ever matter.
DEFS += NO_CJK
# thirdparty build defines
DEFS += FT2_BUILD_LIBRARY HAVE_UNISTD_H HAVE_STDARG_H HAVE_STDINT_H
# tell Write's own code that PDF import is available
DEFS += SCRIBBLE_PDF

# Bionic only declares fseeko/ftello for 32-bit Android from API 24, and the app targets API 21
# (minSdkVersion in android/app/build.gradle, for GLES 3.1).  64-bit ABIs are unaffected because
# off_t is always 64-bit there.  MuPDF uses these purely for file offsets while reading, so falling
# back to the long variants only costs us PDFs larger than 2 GB on 32-bit devices.  Remove this once
# minSdkVersion reaches 24.
ifneq ($(BUILD_SHARED_LIBRARY),)
  ifeq ($(filter arm64-v8a x86_64,$(TARGET_ARCH_ABI)),)
    DEFS += fseeko=fseek ftello=ftell
  endif
endif

# quoting for -DX="y.h" differs between the POSIX shell and cmd.exe
ifeq ($(OS),Windows_NT)
  DEFS += FT_CONFIG_MODULES_H="\"slimftmodules.h\""
  DEFS += FT_CONFIG_OPTIONS_H="\"slimftoptions.h\""
  DEFS += JBIG_EXTERNAL_MEMENTO_H="\"mupdf/memento.h\""
else
  DEFS += FT_CONFIG_MODULES_H=\"slimftmodules.h\"
  DEFS += FT_CONFIG_OPTIONS_H=\"slimftoptions.h\"
  DEFS += JBIG_EXTERNAL_MEMENTO_H=\"mupdf/memento.h\"
endif
