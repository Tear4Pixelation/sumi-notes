#include "pdfimport.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "document.h"
#include "page.h"
#include "resources.h"  // _() for translated strings
#include "ulib/fileutil.h"
#include "ulib/painter.h"
#include "ulib/stringutil.h"
#include "usvg/svgwriter.h"

#ifdef SCRIBBLE_PDF
// fitz.h provides its own extern "C" guard
#include "mupdf/fitz.h"
// declarations only: the implementation is compiled in ulib/image.cpp, with miniz doing PNG's deflate
#include "stb_image_write.h"
#endif

// The rendered page image goes into the page's *rule* layer rather than its content layer.  That is
// the same place scribbleres/pdf2write.sh has always put PDF page images, and it is the right one:
// the rule layer is the page background, so the image cannot be selected, dragged or erased by
// accident, while everything in the content layer (i.e. the user's ink) draws on top of it.
static const char* PDF_BACKGROUND_CLASS = "write-pdf-background";

size_t PdfImport::MemoryBudget::remaining(size_t inUse) const
{
  if(!limit)
    return SIZE_MAX;
  size_t used = input + pending + inUse;
  return used < limit ? limit - used : 0;
}

Image PdfImport::encodedOnly(const Image& img, Image::Encoding encoding)
{
  // returns the bytes img already holds when they are in this format, without decoding anything
  Image::EncodeBuff encoded = img.encode(encoding);
  return Image::decodeBuffer(encoded.data(), encoded.size());
}

std::string PdfImport::thumbnail(Document* doc)
{
  if(doc->numPages() < 1)
    return std::string();
  Page* page = doc->pages.front();
  // ScribbleDoc saves 240x400; the height follows the page instead, or a short page gets a blank strip
  const int width = 240;
  Dim scale = width/page->width();
  Image image(width, std::max(1, std::min(400, int(page->height()*scale + 0.5))), Image::PNG);
  Painter painter(Painter::PAINT_SW, &image);
  painter.beginFrame();
  painter.fillRect(Rect::wh(image.width, image.height), Color::WHITE);
  painter.scale(scale, scale);
  page->draw(&painter, Rect::wh(page->width(), std::min(page->height(), image.height/scale)));
  painter.endFrame();
  return base64_encode(image.encode(Image::PNG));
}

PdfImport::ImportSaver::ImportSaver(Document* document, const std::string& path, MemoryBudget* memoryBudget,
    int compressLevel) : doc(document), budget(memoryBudget), savePath(path),
    flags(Document::SAVE_FORCE | (Document::saveflags_t(compressLevel) << 24)) {}

bool PdfImport::ImportSaver::save(const char* thumb)
{
  // savePicScaled would resample the page images down to one pixel per Write unit (i.e. 150 DPI),
  //  throwing away exactly the resolution the import was asked for
  float savedImageScale = SvgWriter::DEFAULT_SAVE_IMAGE_SCALED;
  SvgWriter::DEFAULT_SAVE_IMAGE_SCALED = 0;
  // the first write creates the file; the later ones write from the first page not yet in it, plus the
  //  closing block - the same partial save the app does
  bool saved = started ? doc->save(NULL, thumb, flags | Document::SAVE_BGZ_PARTIAL)
      : doc->save(new FileStream(savePath.c_str(), "wb+"), thumb, flags);
  SvgWriter::DEFAULT_SAVE_IMAGE_SCALED = savedImageScale;
  started = started || saved;
  ok = ok && saved;
  return saved;
}

bool PdfImport::ImportSaver::pageDone()
{
  if(!ok || doc->numPages() < 1 || !save(NULL))
    return false;
  for(Page* page : doc->pages) {
    if(page->loadStatus == Page::LOAD_OK && page->dirtyCount == 0 && page->blockIdx >= 0)
      page->unload();
  }
  if(budget)
    budget->written();
  return true;
}

bool PdfImport::ImportSaver::finish()
{
  if(!ok || doc->numPages() < 1)
    return false;
  // reads the first page back from the file
  std::string thumb = thumbnail(doc);
  return save(thumb.c_str());
}

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
    *errorOut = _("This build of Sumi does not include PDF support.");
  return -1;
}

struct PdfImport::Renderer::Impl {};

PdfImport::Renderer::Renderer() {}
PdfImport::Renderer::~Renderer() {}
bool PdfImport::Renderer::open(const char*, const char*, MemoryBudget*) { return false; }
bool PdfImport::Renderer::openMemory(std::string, MemoryBudget*) { return false; }
bool PdfImport::Renderer::isOpen() const { return false; }
int PdfImport::Renderer::numPages() { return -1; }
Image PdfImport::Renderer::render(int, Dim, Image::Encoding, Dim*, Dim*, const Rect&) { return Image(0, 0); }
const char* PdfImport::Renderer::error() const { return _("This build of Sumi does not include PDF support."); }

#else

bool PdfImport::isAvailable() { return true; }

namespace {

// Everything below is careful about one thing: MuPDF implements exceptions with setjmp/longjmp, and
// longjmp-ing past a live C++ object with a destructor is undefined behavior.  So no fz_try block
// here constructs a non-trivial C++ object - the C++ side of each operation happens after the
// matching fz_catch has run.

// MuPDF's allocations, counted so a MemoryBudget can cap them: past `cap` an allocation fails, MuPDF
//  empties its cache and tries again, and failing that throws - which fails the one page, not the app.
//  Each block carries its size in front, since free() is not told it.
struct AllocTracker {
  size_t live = 0;
  size_t cap = SIZE_MAX;
};

static constexpr size_t ALLOC_HEADER = 16;  // keeps the alignment malloc gives

static void* trackedMalloc(void* user, size_t size)
{
  AllocTracker* tracker = static_cast<AllocTracker*>(user);
  if(size > tracker->cap || tracker->live > tracker->cap - size)
    return NULL;
  char* block = (char*)malloc(size + ALLOC_HEADER);
  if(!block)
    return NULL;
  *(size_t*)block = size;
  tracker->live += size;
  return block + ALLOC_HEADER;
}

static void trackedFree(void* user, void* ptr)
{
  if(!ptr)
    return;
  AllocTracker* tracker = static_cast<AllocTracker*>(user);
  char* block = (char*)ptr - ALLOC_HEADER;
  tracker->live -= *(size_t*)block;
  free(block);
}

static void* trackedRealloc(void* user, void* old, size_t size)
{
  if(!old)
    return trackedMalloc(user, size);
  if(size == 0) {
    trackedFree(user, old);
    return NULL;
  }
  AllocTracker* tracker = static_cast<AllocTracker*>(user);
  char* block = (char*)old - ALLOC_HEADER;
  size_t oldSize = *(size_t*)block;
  if(size > oldSize && (size - oldSize > tracker->cap || tracker->live > tracker->cap - (size - oldSize)))
    return NULL;
  char* grown = (char*)realloc(block, size + ALLOC_HEADER);
  if(!grown)
    return NULL;
  *(size_t*)grown = size;
  tracker->live = tracker->live - oldSize + size;
  return grown + ALLOC_HEADER;
}

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

  // `alloc` NULL for MuPDF's own allocator
  bool open(const char* filename, const char* password, const fz_alloc_context* alloc = NULL,
      size_t storeSize = FZ_STORE_DEFAULT)
  {
    if(!newContext(alloc, storeSize))
      return false;
    bool ok = false;
    fz_try(ctx) {
      fz_register_document_handlers(ctx);
      doc = fz_open_document(ctx, filename);
      ok = true;
    }
    fz_catch(ctx) {
      snprintf(err, sizeof(err), "%s", fz_caught_message(ctx));
      fz_ignore_error(ctx);
      return false;
    }
    return ok && authenticate(password);
  }

  // `data` is not copied and must outlive this object
  bool openMemory(const std::string& data, const char* password, const fz_alloc_context* alloc = NULL,
      size_t storeSize = FZ_STORE_DEFAULT)
  {
    if(!newContext(alloc, storeSize))
      return false;
    bool ok = false;
    fz_stream* stream = NULL;
    fz_var(stream);
    fz_try(ctx) {
      fz_register_document_handlers(ctx);
      stream = fz_open_memory(ctx, (const unsigned char*)data.data(), data.size());
      doc = fz_open_document_with_stream(ctx, "application/pdf", stream);
      ok = true;
    }
    fz_always(ctx) {
      fz_drop_stream(ctx, stream);  // the document keeps its own reference
    }
    fz_catch(ctx) {
      snprintf(err, sizeof(err), "%s", fz_caught_message(ctx));
      fz_ignore_error(ctx);
      return false;
    }
    return ok && authenticate(password);
  }

  bool newContext(const fz_alloc_context* alloc, size_t storeSize)
  {
    // NULL locks: the context is only ever used from the thread that created it
    ctx = fz_new_context(alloc, NULL, storeSize);
    if(!ctx)
      snprintf(err, sizeof(err), "Out of memory initializing PDF support.");
    return ctx != NULL;
  }

  bool authenticate(const char* password)
  {
    bool ok = false;
    fz_try(ctx) {
      ok = !fz_needs_password(ctx, doc)
          || fz_authenticate_password(ctx, doc, password ? password : "");
    }
    fz_catch(ctx) {
      snprintf(err, sizeof(err), "%s", fz_caught_message(ctx));
      fz_ignore_error(ctx);
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
    fz_catch(ctx) { snprintf(err, sizeof(err), "%s", fz_caught_message(ctx)); fz_ignore_error(ctx); n = -1; }
    return n;
  }
};

// Loads page `pageNum` and its bounds in points (fz_bound_page already accounts for the page's /Rotate
//  entry).  NULL on failure, with fzdoc.err set; the caller drops the page.
fz_page* loadPage(FzDoc& fzdoc, int pageNum, fz_rect* boundsOut)
{
  fz_context* ctx = fzdoc.ctx;
  fz_page* page = NULL;
  fz_var(page);
  fz_try(ctx) {
    page = fz_load_page(ctx, fzdoc.doc, pageNum);
    *boundsOut = fz_bound_page(ctx, page);
  }
  fz_catch(ctx) {
    snprintf(fzdoc.err, sizeof(fzdoc.err), "%s", fz_caught_message(ctx));
    fz_ignore_error(ctx);
    fz_drop_page(ctx, page);
    page = NULL;
  }
  return page;
}

// Renders `area` (in points, as fz_bound_page gives them) of a loaded page at `scale` pixels per point,
//  as an RGB pixmap on white - what fz_new_pixmap_from_page does for the whole page.  NULL on failure,
//  with fzdoc.err set; the caller drops the pixmap.
fz_pixmap* renderArea(FzDoc& fzdoc, fz_page* page, fz_rect area, float scale)
{
  fz_context* ctx = fzdoc.ctx;
  fz_pixmap* pix = NULL;
  fz_device* dev = NULL;
  fz_var(pix);
  fz_var(dev);
  fz_try(ctx) {
    fz_matrix ctm = fz_scale(scale, scale);
    fz_irect bbox = fz_round_rect(fz_transform_rect(area, ctm));
    pix = fz_new_pixmap_with_bbox(ctx, fz_device_rgb(ctx), bbox, NULL, 0);
    fz_clear_pixmap_with_value(ctx, pix, 0xFF);
    dev = fz_new_draw_device(ctx, ctm, pix);
    fz_run_page(ctx, page, dev, fz_identity, NULL);
    fz_close_device(ctx, dev);
  }
  fz_always(ctx) {
    fz_drop_device(ctx, dev);
  }
  fz_catch(ctx) {
    snprintf(fzdoc.err, sizeof(fzdoc.err), "%s", fz_caught_message(ctx));
    fz_ignore_error(ctx);
    fz_drop_pixmap(ctx, pix);
    pix = NULL;
  }
  return pix;
}

void appendBytes(void* context, void* data, int size)
{
  Image::EncodeBuff* out = static_cast<Image::EncodeBuff*>(context);
  const unsigned char* bytes = static_cast<const unsigned char*>(data);
  out->insert(out->end(), bytes, bytes + size);
}

// MuPDF's tightly packed 8-bit samples (`n` per pixel, a per-row stride) straight to PNG or JPEG.  Going
//  through a 32-bit Image first, as this used to, took 4 bytes a pixel on top of the pixmap's 3.
Image encodePixmap(const fz_pixmap* pix, Image::Encoding encoding)
{
  const int w = pix->w, h = pix->h, n = pix->n;
  Image::EncodeBuff encoded;
  bool ok = false;
  if(encoding == Image::JPEG) {
    // stb's JPEG writer takes no stride; MuPDF's pixmaps have none to speak of, but don't assume it
    if(pix->stride == ptrdiff_t(w)*n)
      ok = stbi_write_jpg_to_func(&appendBytes, &encoded, w, h, n, pix->samples, 75);
    else {
      std::vector<unsigned char> packed(size_t(w)*n*h);
      for(int y = 0; y < h; ++y)
        memcpy(&packed[size_t(y)*w*n], pix->samples + size_t(y)*pix->stride, size_t(w)*n);
      ok = stbi_write_jpg_to_func(&appendBytes, &encoded, w, h, n, packed.data(), 75);
    }
  }
  else
    ok = stbi_write_png_to_func(&appendBytes, &encoded, w, h, n, pix->samples, int(pix->stride));
  return ok ? Image::decodeBuffer(encoded.data(), encoded.size()) : Image(0, 0);
}

// Peak bytes per rendered pixel while one page is rendered and encoded: the RGB pixmap (3); for PNG also
//  stb's filtered copy (3) and the compressed result, held twice for a moment (stb's, then ours).
size_t peakBytesPerPixel(Image::Encoding encoding) { return encoding == Image::JPEG ? 4 : 9; }

// what MuPDF needs besides the pixmap - its cache, fonts, the page's own images - kept out of what the
//  page's resolution may use
constexpr size_t MUPDF_RESERVE = size_t(32) << 20;

// a page is never rendered coarser than this to fit a budget: rather a page that fails on its own
constexpr Dim MIN_BUDGET_DPI = 72;

}  // namespace

struct PdfImport::Renderer::Impl {
  AllocTracker tracker;
  fz_alloc_context alloc;
  MemoryBudget* budget = NULL;
  // a PDF opened from memory, which MuPDF reads in place: declared before fzdoc, so it is freed after it
  std::string data;
  FzDoc fzdoc;

  Impl(MemoryBudget* memoryBudget) : budget(memoryBudget)
  {
    alloc.user = &tracker;
    alloc.malloc = &trackedMalloc;
    alloc.realloc = &trackedRealloc;
    alloc.free = &trackedFree;
  }

  // what this renderer holds: MuPDF's allocations and the PDF it was given in memory
  size_t inUse() const { return tracker.live + data.size(); }

  // Opening a PDF parses it, which the budget covers too - but never with less than MuPDF's reserve, or
  //  a budget already spent would fail the open itself.  render() moves the cap for each page.
  void capForOpen()
  {
    if(budget && budget->limited())
      tracker.cap = std::max(budget->remaining(inUse()), MUPDF_RESERVE);
  }

  // MuPDF's cache only speeds up pages sharing fonts and images, and an import renders each page once, in
  //  order: its default 256 MB filled up with the scans of an image-heavy PDF to no use.  Under a budget
  //  it gets a share of that instead.
  size_t storeSize() const
  {
    const size_t importStore = size_t(64) << 20;
    if(!budget || !budget->limited())
      return importStore;
    return std::max(size_t(8) << 20, std::min(importStore, budget->limit/8));
  }
};

PdfImport::Renderer::Renderer() {}
PdfImport::Renderer::~Renderer() {}

bool PdfImport::Renderer::open(const char* filename, const char* password, MemoryBudget* budget)
{
  impl.reset(new Impl(budget));
  impl->capForOpen();
  return impl->fzdoc.open(filename, password, &impl->alloc, impl->storeSize());
}

bool PdfImport::Renderer::openMemory(std::string data, MemoryBudget* budget)
{
  impl.reset(new Impl(budget));
  impl->data = std::move(data);
  impl->capForOpen();
  return impl->fzdoc.openMemory(impl->data, NULL, &impl->alloc, impl->storeSize());
}

bool PdfImport::Renderer::isOpen() const { return impl && impl->fzdoc.doc; }

int PdfImport::Renderer::numPages() { return isOpen() ? impl->fzdoc.numPages() : -1; }

const char* PdfImport::Renderer::error() const { return impl ? impl->fzdoc.err : ""; }

Image PdfImport::Renderer::render(int pageNum, Dim dpi, Image::Encoding encoding, Dim* widthPtOut,
    Dim* heightPtOut, const Rect& crop)
{
  if(!isOpen())
    return Image(0, 0);
  FzDoc& fzdoc = impl->fzdoc;
  MemoryBudget* budget = impl->budget;
  bool limited = budget && budget->limited();
  if(limited)  // loading the page; the cap is set again for rendering it below
    impl->tracker.cap = impl->tracker.live + std::max(budget->remaining(impl->inUse()), MUPDF_RESERVE);

  fz_rect bounds;
  fz_page* page = loadPage(fzdoc, pageNum, &bounds);
  if(!page)
    return Image(0, 0);
  float widthPt = bounds.x1 - bounds.x0, heightPt = bounds.y1 - bounds.y0;
  if(widthPt <= 0 || heightPt <= 0) {
    fz_drop_page(fzdoc.ctx, page);
    snprintf(fzdoc.err, sizeof(fzdoc.err), "The page is empty.");
    return Image(0, 0);
  }
  Rect part = crop.isValid() ? Rect(crop).rectIntersect(Rect::ltrb(0, 0, 1, 1)) : Rect::ltrb(0, 0, 1, 1);
  fz_rect area = fz_make_rect(bounds.x0 + part.left*widthPt, bounds.y0 + part.top*heightPt,
      bounds.x0 + part.right*widthPt, bounds.y0 + part.bottom*heightPt);

  // the resolution asked for, unless the budget cannot hold the page at it
  Dim renderDpi = dpi;
  Dim floorDpi = std::min(dpi, MIN_BUDGET_DPI);
  if(limited) {
    double pointsSq = double(area.x1 - area.x0)*(area.y1 - area.y0);
    double pixels = pointsSq*(dpi/72)*(dpi/72);
    size_t remaining = budget->remaining(impl->inUse());
    double maxPixels = double(remaining > MUPDF_RESERVE ? remaining - MUPDF_RESERVE : 0)/peakBytesPerPixel(encoding);
    if(pixels > maxPixels)
      renderDpi = std::max(floorDpi, Dim(dpi*sqrt(maxPixels/pixels)));
    // what the coarsest rendering takes is always allowed, so a page comes out coarse rather than not at
    //  all, however little is left; the limit gives way by that much
    size_t floorBytes = size_t(pointsSq*(floorDpi/72)*(floorDpi/72)*peakBytesPerPixel(encoding)) + MUPDF_RESERVE;
    impl->tracker.cap = impl->tracker.live + std::max(remaining, floorBytes);
  }
  fz_pixmap* pix = renderArea(fzdoc, page, area, float(renderDpi/72));
  // MuPDF ran out of what the budget left it: one more try at the coarsest resolution
  if(!pix && limited && renderDpi > floorDpi) {
    renderDpi = floorDpi;
    pix = renderArea(fzdoc, page, area, float(renderDpi/72));
  }
  fz_drop_page(fzdoc.ctx, page);
  if(!pix)
    return Image(0, 0);

  Image img = encodePixmap(pix, encoding);
  fz_drop_pixmap(fzdoc.ctx, pix);
  if(img.isNull()) {
    snprintf(fzdoc.err, sizeof(fzdoc.err), "The page could not be compressed.");
    return Image(0, 0);
  }
  if(budget) {
    budget->keep(img.encData.size());
    if(renderDpi < dpi - 0.5) {
      ++budget->reducedPages;
      budget->lowestDpi = budget->lowestDpi > 0 ? std::min(budget->lowestDpi, renderDpi) : renderDpi;
    }
  }
  if(widthPtOut) *widthPtOut = widthPt;
  if(heightPtOut) *heightPtOut = heightPt;
  return img;
}

int PdfImport::pageCount(const char* filename, const char* password)
{
  FzDoc fzdoc;
  if(!fzdoc.open(filename, password))
    return -1;
  return fzdoc.numPages();
}

int PdfImport::importPdf(Document* doc, const char* filename, const Options& opts, std::string* errorOut)
{
  Renderer renderer;
  if(!renderer.open(filename, opts.password.empty() ? NULL : opts.password.c_str(), opts.budget)) {
    if(errorOut) *errorOut = renderer.error();
    return -1;
  }
  int numPages = renderer.numPages();
  if(numPages < 0) {
    if(errorOut) *errorOut = renderer.error();
    return -1;
  }

  int imported = 0;
  for(int ii = 0; ii < numPages; ++ii) {
    if(opts.onProgress && !opts.onProgress(ii, numPages))
      break;  // user cancelled; keep the pages imported so far

    Dim widthPt = 0, heightPt = 0;
    Image img = renderer.render(ii, opts.dpi, opts.lossy ? Image::JPEG : Image::PNG, &widthPt, &heightPt);
    if(img.isNull()) {
      // a single unrenderable page shouldn't abort the whole import
      SCRIBBLE_LOG("PDF import: page %d of %s failed to render: %s\n", ii + 1, filename, renderer.error());
      continue;
    }

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
    if(opts.saver && !opts.saver->pageDone()) {
      if(errorOut) *errorOut = _("The imported document could not be saved.");
      return -1;
    }
  }

  if(imported == 0 && errorOut) {
    const char* err = renderer.error();
    *errorOut = err[0] ? err : "No pages could be read from this PDF.";
  }
  return imported > 0 ? imported : -1;
}

#endif  // SCRIBBLE_PDF
