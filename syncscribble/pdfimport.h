#pragma once

// PDF import: render each page of a PDF to an image and use it as the background of a Write page,
// so the user can annotate on top of it.  This is the "raster page background" model used by
// GoodNotes/Notability; the imported page is an ordinary Write page, so ink, selection, undo and
// sync need no changes at all.
//
// Backed by MuPDF (AGPL-3.0, same license as Write), compiled inline - see mupdf.mk.  When Write is
// built with PDF_IMPORT=0 the whole implementation compiles out and isAvailable() returns false.

#include <string>
#include <memory>
#include <functional>
#include "basics.h"
#include "ulib/image.h"

class Document;

namespace PdfImport {

// Write uses 150 units per inch as its reference resolution; PDF is 72 points per inch.
static constexpr Dim UNITS_PER_POINT = 150.0/72.0;

// How much memory an import may hold (the "Limit memory" option of the Import menu): its input (a
//  notebook is read whole), MuPDF's allocations, which are counted exactly, and the page images not yet
//  written - none, once an ImportSaver writes each page as it is made.  What is counted is what the
//  import holds, not the process footprint: an allocator keeps freed memory (glibc, ASan), so a measured
//  footprint only ever rose and starved the later pages of a long import.
// A page whose rendering would not fit what is left is rendered at a lower resolution rather than
//  letting the OS end the app; never below 72 DPI, which a page always gets even when nothing is left, so
//  no page is ever dropped for the limit - it gives way by one coarse page instead.
struct MemoryBudget {
  explicit MemoryBudget(size_t limitBytes = 0) : limit(limitBytes) {}
  bool limited() const { return limit > 0; }
  // bytes left once `inUse` more are counted (a renderer's own); SIZE_MAX when unlimited
  size_t remaining(size_t inUse = 0) const;
  // the import's input held in memory (a whole notebook), replacing the previous one
  void setInput(size_t bytes) { input = bytes; }
  // a page image the import holds until the page is written
  void keep(size_t bytes) { pending += bytes; }
  // the pages held so far are written and unloaded (ImportSaver::pageDone)
  void written() { pending = 0; }

  size_t limit;
  size_t input = 0;
  size_t pending = 0;
  // pages rendered below the resolution asked for to stay within the limit, and the lowest used
  int reducedPages = 0;
  Dim lowestDpi = 0;
};

// Saves an imported document while it is being made: after each page, the pages not yet written go to
//  the .svgz (Document's partial save, as the app's own saves do) and are unloaded, to be read back from
//  the file if anything needs them.  So an import holds one page at a time, however long the document -
//  merely encoded, the pages of a 300-page PDF were still ~1.6 GB of PNG by the time it was saved.
//  An importer must not touch a page again once pageDone() has run for it (an outline entry, say), or
//  it is reloaded and everything after it written again.
class ImportSaver
{
public:
  // `budget` (optional) is told when the pages it counts are written
  ImportSaver(Document* doc, const std::string& path, MemoryBudget* budget = NULL, int compressLevel = 2);
  // after each page added to the document: write the new pages and unload them
  bool pageDone();
  // the last write, with the thumbnail of the first page; false if any write failed
  bool finish();
  const std::string& path() const { return savePath; }

private:
  bool save(const char* thumb);
  Document* doc;
  MemoryBudget* budget;
  std::string savePath;
  unsigned int flags;  // Document::saveflags_t
  bool started = false;
  bool ok = true;
};

struct Options {
  // resolution to rasterize at; 300 gives ~2x the on-screen page size, so moderate zoom stays sharp
  Dim dpi = 300;
  // JPEG is much smaller for scans and photos, PNG keeps rendered text crisp
  bool lossy = false;
  // password for encrypted PDFs
  std::string password;
  // called before each page is rendered; return false to abort the import
  std::function<bool(int pageNum, int numPages)> onProgress;
  // optional memory limit (see MemoryBudget), owned by the caller, which reads reducedPages afterwards
  MemoryBudget* budget = NULL;
  // optional: write each page as soon as it is made (see ImportSaver), owned by the caller
  ImportSaver* saver = NULL;
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

// The thumbnail an imported document is saved with (Document::save's `thumb`): its first page, width
//  filling the image, as base64 PNG - what ScribbleArea::drawThumbnail gives a document saved in the
//  app, which an import never passes through.  Empty for a document without pages.
std::string thumbnail(Document* doc);

// `img` as an Image holding only its encoded bytes (data == NULL), decoded again only if drawn.  An
//  import keeps every page image until the document is saved, and a decoded 300 DPI page is ~35 MB -
//  kept decoded, a notebook of a hundred pages was more than an iPad allows an app.  The writer saves
//  these bytes as they are, with no second compression.
Image encodedOnly(const Image& img, Image::Encoding encoding);

// One open PDF, rendering page after page - opened once rather than for every page, which for a PDF
//  embedded in a notebook meant parsing the whole file again per page.  Pages come out encodedOnly(),
//  rendered straight from MuPDF's RGB pixmap, so a page never exists as a 32-bit bitmap at all.
class Renderer
{
public:
  Renderer();
  ~Renderer();
  // `budget` (optional) caps MuPDF's allocations and picks the resolution of each page
  bool open(const char* filename, const char* password, MemoryBudget* budget);
  // from memory - for PDFs embedded in another format (a Noteful notebook's page backgrounds); the
  //  data is kept for as long as the document is open
  bool openMemory(std::string data, MemoryBudget* budget);
  bool isOpen() const;
  int numPages();
  // Page `pageNum` at `dpi` (or less, to stay within the budget), as `encoding`.  `crop`, as fractions
  //  of the page (0..1), renders just that part; an invalid Rect is the whole page.  The page's size in
  //  points goes to widthPt/heightPt.  A null Image on failure, with error() set.
  Image render(int pageNum, Dim dpi, Image::Encoding encoding, Dim* widthPt = NULL, Dim* heightPt = NULL,
      const Rect& crop = Rect());
  const char* error() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl;
};

}  // namespace PdfImport
