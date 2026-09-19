#pragma once

// PDF import: render each page of a PDF to an image and use it as the background of a Write page,
// so the user can annotate on top of it.  This is the "raster page background" model used by
// GoodNotes/Notability; the imported page is an ordinary Write page, so ink, selection, undo and
// sync need no changes at all.
//
// Backed by MuPDF (AGPL-3.0, same license as Write), compiled inline - see mupdf.mk.  When Write is
// built with PDF_IMPORT=0 the whole implementation compiles out and isAvailable() returns false.

#include <string>
#include <functional>
#include "basics.h"

class Document;

namespace PdfImport {

// Write uses 150 units per inch as its reference resolution; PDF is 72 points per inch.
static constexpr Dim UNITS_PER_POINT = 150.0/72.0;

struct Options {
  // resolution to rasterize at; 300 gives ~2x the on-screen page size, so moderate zoom stays sharp
  Dim dpi = 300;
  // JPEG is much smaller for scans and photos, PNG keeps rendered text crisp
  bool lossy = false;
  // password for encrypted PDFs
  std::string password;
  // called before each page is rendered; return false to abort the import
  std::function<bool(int pageNum, int numPages)> onProgress;
};

// false if this build has no PDF support (PDF_IMPORT=0)
bool isAvailable();

// true if the filename looks like a PDF (extension check only, no I/O)
bool isPdfFile(const char* filename);

// Append every page of `filename` to `doc`.  Returns the number of pages imported, or -1 on error,
// in which case `errorOut` (if given) is set to a user-presentable message.  Pages already in `doc`
// are left alone.  This runs synchronously and can take a while for a large PDF - use
// opts.onProgress to drive a progress dialog and to let the user cancel.
int importPdf(Document* doc, const char* filename, const Options& opts = Options(),
    std::string* errorOut = NULL);

// Number of pages in `filename`, or -1 if it cannot be opened.  Cheap - does not render anything.
int pageCount(const char* filename, const char* password = NULL);

}  // namespace PdfImport
