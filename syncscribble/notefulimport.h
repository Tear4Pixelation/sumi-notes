#pragma once

// Noteful import: turn a Noteful notebook export (.noteful) into Sumi pages.  notefulfile.cpp does the
//  decoding; this maps the result onto a Document the same way PDF import does - a finished set of
//  ordinary pages, so undo, views and sync never see the import.
//
//  - Paper templates (Grid, Ruled) become Sumi's own ruling at Noteful's spacing, so the paper stays
//    editable; everything else (imported PDF pages, photos, covers) becomes a background image in the
//    rule layer, exactly as PdfImport's pages are.
//  - Strokes, lines and curves become paths, pasted images SvgImages, each on its layer.
//  - Ink below the page's bottom edge (Noteful keeps it) makes the page taller rather than being lost.
//  - Outline entries go onto their pages; Sumi keeps one entry per page, so a second is dropped.
//  - Tags are returned, not applied: they live in the library's TagStore, which the caller owns.
//  - Text boxes are not imported yet (Sumi has no editable text element); they are counted.

#include <string>
#include <vector>
#include <functional>
#include "basics.h"

class Document;
namespace PdfImport { struct MemoryBudget; class ImportSaver; }

namespace NotefulImport {

struct Options {
  // resolution for PDF backgrounds (imported pages, covers)
  Dim dpi = 300;
  // store rendered backgrounds as JPEG.  On by default, unlike PdfImport: what Noteful users import
  //  is mostly scanned worksheets and photos, and as PNG the Mathe sample grew from 13 MB to 81 MB
  bool lossy = true;
  // called before each page; return false to stop, keeping the pages imported so far
  std::function<bool(int pageNum, int numPages)> onProgress;
  // optional memory limit for rendering PDF backgrounds, owned by the caller (see PdfImport::MemoryBudget)
  PdfImport::MemoryBudget* budget = NULL;
  // optional: write each page as soon as it is made (PdfImport::ImportSaver); importArchive sets its own
  PdfImport::ImportSaver* saver = NULL;
};

struct Result {
  std::string title;
  // tag paths as Noteful names them, "/" separating levels ("Schule/Naturwissenschaften/Mathe")
  std::vector<std::string> tags;
  int textBoxesSkipped = 0;
  // pages or parts of pages that could not be read completely
  std::vector<std::string> warnings;
};

// true if the filename looks like a Noteful notebook (extension check only, no I/O)
bool isNotefulFile(const char* filename);

// Append the notebook's pages to `doc`, whose layer table is replaced by the notebook's.  The layer
//  table is also written to the document's config node, so a Document saved directly (as a PDF import
//  is) keeps it.  Returns the number of pages imported, or -1 with a user-presentable `errorOut`.
int importNoteful(Document* doc, const char* filename, const Options& opts = Options(),
    Result* result = NULL, std::string* errorOut = NULL);

// --- whole folders ---
// Noteful exports a folder as a .zip: the folder tree with a .noteful per notebook (plus an
//  archive.json manifest, which the tree makes redundant).  An unpacked export, or any directory of
//  notebooks, works the same way.

struct ArchiveOptions : Options {
  // Give each notebook a tag for the folder it was in ("KA/Test" - a subtag of "KA"), next to its own
  //  tags.  Off for anyone whose folders only mirror their tags, who would otherwise get every tag twice.
  bool folderTags = true;
  // called before each notebook with its path in the archive; return false to stop, keeping the
  //  notebooks imported so far
  std::function<bool(int index, int count, const std::string& path)> onNotebook;
};

struct ArchiveEntry {
  std::string archivePath;  // the notebook's path in the archive
  std::string folder;       // its folder there, "" at the top
  std::string docPath;      // the document written, empty if the notebook failed
  std::string error;        // why it failed
  std::vector<std::string> tagPaths;  // tags given to the document (its own, plus its folder)
  Result result;
};

struct ArchiveResult {
  std::vector<ArchiveEntry> entries;
  int imported = 0, failed = 0;
};

// true for a .zip (extension check only) or a directory
bool isNotefulArchive(const char* path);
// true for anything importArchive() takes: an archive or a single .noteful
bool isImportable(const char* path);

// Import every notebook in a Noteful archive (a .zip or a directory, searched recursively) - or a
//  single .noteful, which is then the only entry - into
//  `libraryDir`, one .svgz each, named after the notebook (" (2)", " (3)", ... on a clash).  Tags go
//  through the library's own tag index, libraryDir/.write-tags: missing tags are created, subtags
//  under their parents.  A notebook that fails is recorded and the rest carry on.  Returns the number
//  of documents written, or -1 with `errorOut` set if none were.
// The tag index is rewritten, so a TagDocList open on the same library must reload it afterwards
//  (TagDocList::setRoot) or its next save would drop the new tags.
int importArchive(const char* path, const char* libraryDir, const ArchiveOptions& opts = ArchiveOptions(),
    ArchiveResult* result = NULL, std::string* errorOut = NULL);

}  // namespace NotefulImport
