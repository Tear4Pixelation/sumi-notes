#include "pdfimport.h"

#include "document.h"
#include "page.h"
#include "resources.h"  // _() for translated strings
#include "ulib/fileutil.h"

#ifdef SCRIBBLE_PDF
// fitz.h provides its own extern "C" guard
#include "mupdf/fitz.h"
#endif

// The rendered page image goes into the page's *rule* layer rather than its content layer.  That is
// the same place scribbleres/pdf2write.sh has always put PDF page images, and it is the right one:
// the rule layer is the page background, so the image cannot be selected, dragged or erased by
// accident, while everything in the content layer (i.e. the user's ink) draws on top of it.
static const char* PDF_BACKGROUND_CLASS = "write-pdf-background";

bool PdfImport::isPdfFile(const char* filename)
{
  if(!filename)
    return false;
  std::string ext = FSPath(filename).extension();
  return ext.size() == 3
      && (ext[0] == 'p' || ext[0] == 'P') && (ext[1] == 'd' || ext[1] == 'D')
      && (ext[2] == 'f' || ext[2] == 'F');
}

#ifndef SCRIBBLE_PDF

// built with PDF_IMPORT=0

bool PdfImport::isAvailable() { return false; }

int PdfImport::pageCount(const char*, const char*) { return -1; }

int PdfImport::importPdf(Document*, const char*, const Options&, std::string* errorOut)
{
  if(errorOut)
    *errorOut = _("This build of Kaku does not include PDF support.");
  return -1;
}

#else

bool PdfImport::isAvailable() { return true; }

namespace {

// Everything below is careful about one thing: MuPDF implements exceptions with setjmp/longjmp, and
// longjmp-ing past a live C++ object with a destructor is undefined behavior.  So no fz_try block
// here constructs a non-trivial C++ object - the C++ side of each operation happens after the
// matching fz_catch has run.

struct FzDoc {
  fz_context* ctx = NULL;
  fz_document* doc = NULL;
  char err[256];

  FzDoc() { err[0] = '\0'; }

  ~FzDoc()
  {
    if(ctx && doc)
      fz_drop_document(ctx, doc);
    if(ctx)
      fz_drop_context(ctx);
  }

  bool open(const char* filename, const char* password)
  {
    // NULL locks: the context is only ever used from the thread that created it
    ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
    if(!ctx) {
      snprintf(err, sizeof(err), "Out of memory initializing PDF support.");
      return false;
    }
    bool ok = false;
    fz_try(ctx) {
      fz_register_document_handlers(ctx);
      doc = fz_open_document(ctx, filename);
      ok = true;
    }
    fz_catch(ctx) {
      snprintf(err, sizeof(err), "%s", fz_caught_message(ctx));
      return false;
    }
    if(!ok)
      return false;

    fz_try(ctx) {
      ok = !fz_needs_password(ctx, doc)
          || fz_authenticate_password(ctx, doc, password ? password : "");
    }
    fz_catch(ctx) {
      snprintf(err, sizeof(err), "%s", fz_caught_message(ctx));
      return false;
    }
    if(!ok)
      snprintf(err, sizeof(err), "This PDF is password protected.");
    return ok;
  }

  int numPages()
  {
    int n = -1;
    fz_try(ctx) { n = fz_count_pages(ctx, doc); }
    fz_catch(ctx) { snprintf(err, sizeof(err), "%s", fz_caught_message(ctx)); n = -1; }
    return n;
  }
};

// Renders page `pageNum` at `dpi` and returns it as an Image; sizeOut receives the page size in PDF
// points (which is what determines the Write page size, independent of the render resolution).
// Returns a null Image on failure, with fzdoc.err set.
Image renderPage(FzDoc& fzdoc, int pageNum, Dim dpi, Dim* widthPtOut, Dim* heightPtOut)
{
  fz_context* ctx = fzdoc.ctx;
  fz_page* page = NULL;
  fz_pixmap* pix = NULL;
  float widthPt = 0, heightPt = 0;
  bool ok = false;

  fz_var(page);
  fz_var(pix);
  fz_try(ctx) {
    page = fz_load_page(ctx, fzdoc.doc, pageNum);
    // fz_bound_page already accounts for the page's /Rotate entry
    fz_rect bounds = fz_bound_page(ctx, page);
    widthPt = bounds.x1 - bounds.x0;
    heightPt = bounds.y1 - bounds.y0;
    float scale = float(dpi)/72.0f;
    pix = fz_new_pixmap_from_page(ctx, page, fz_scale(scale, scale), fz_device_rgb(ctx), 0);
    ok = true;
  }
  fz_always(ctx) {
    fz_drop_page(ctx, page);
  }
  fz_catch(ctx) {
    snprintf(fzdoc.err, sizeof(fzdoc.err), "%s", fz_caught_message(ctx));
    ok = false;
  }

  if(!ok || !pix || widthPt <= 0 || heightPt <= 0) {
    if(pix)
      fz_drop_pixmap(ctx, pix);
    return Image(0, 0);
  }

  *widthPtOut = widthPt;
  *heightPtOut = heightPt;

  // MuPDF gives us tightly packed 8-bit samples with `n` components per pixel and a per-row stride;
  // Write's Image is always 32-bit RGBA.
  const int w = pix->w, h = pix->h, n = pix->n;
  Image img(w, h);
  unsigned char* dst = img.bytes();
  for(int y = 0; y < h; ++y) {
    const unsigned char* src = pix->samples + size_t(y)*pix->stride;
    unsigned char* row = dst + size_t(y)*w*4;
    for(int x = 0; x < w; ++x, src += n, row += 4) {
      row[0] = src[0];
      row[1] = n > 2 ? src[1] : src[0];
      row[2] = n > 2 ? src[2] : src[0];
      row[3] = pix->alpha ? src[n-1] : 255;
    }
  }
  fz_drop_pixmap(ctx, pix);
  return img;
}

}  // namespace

int PdfImport::pageCount(const char* filename, const char* password)
{
  FzDoc fzdoc;
  if(!fzdoc.open(filename, password))
    return -1;
  return fzdoc.numPages();
}

int PdfImport::importPdf(Document* doc, const char* filename, const Options& opts, std::string* errorOut)
{
  FzDoc fzdoc;
  if(!fzdoc.open(filename, opts.password.empty() ? NULL : opts.password.c_str())) {
    if(errorOut) *errorOut = fzdoc.err;
    return -1;
  }
  int numPages = fzdoc.numPages();
  if(numPages < 0) {
    if(errorOut) *errorOut = fzdoc.err;
    return -1;
  }

  int imported = 0;
  for(int ii = 0; ii < numPages; ++ii) {
    if(opts.onProgress && !opts.onProgress(ii, numPages))
      break;  // user cancelled; keep the pages imported so far

    Dim widthPt = 0, heightPt = 0;
    Image img = renderPage(fzdoc, ii, opts.dpi, &widthPt, &heightPt);
    if(img.isNull()) {
      // a single unrenderable page shouldn't abort the whole import
      SCRIBBLE_LOG("PDF import: page %d of %s failed to render: %s\n", ii + 1, filename, fzdoc.err);
      continue;
    }
    img.encoding = opts.lossy ? Image::JPEG : Image::PNG;

    Dim width = widthPt*UNITS_PER_POINT, height = heightPt*UNITS_PER_POINT;
    // no ruling and no margin line - the PDF page is the background
    PageProperties props(width, height, 0, 0, 0, Color::WHITE, Color::BLUE);
    Page* page = new Page(props);
    // the ctor generated a rule layer holding just the .pagerect; add the page image over it
    SvgImage* background = new SvgImage(std::move(img), Rect::ltwh(0, 0, width, height));
    background->addClass(PDF_BACKGROUND_CLASS);
    page->ruleNode->addChild(background);
    // Mark the ruling as custom so a page-size change doesn't regenerate the layer and drop the
    // image.  Dropping "write-std-ruling" is what makes that stick across save/load - Page::loadSVG
    // derives isCustomRuling from the absence of that class.  "write-no-dup" then stops a new page
    // added after this one from inheriting a copy of this page's PDF image as its ruling.
    page->ruleNode->removeClass("write-std-ruling");
    page->ruleNode->addClass("write-no-dup");
    page->isCustomRuling = true;

    doc->insertPage(page);
    ++imported;
  }

  if(imported == 0 && errorOut)
    *errorOut = fzdoc.err[0] ? fzdoc.err : "No pages could be read from this PDF.";
  return imported > 0 ? imported : -1;
}

#endif  // SCRIBBLE_PDF
