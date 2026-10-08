#include "notefulimport.h"

#include <algorithm>
#include <map>
#include <memory>
#include <string.h>
#include "notefulfile.h"
#include "pdfimport.h"
#include "document.h"
#include "page.h"
#include "scribbleconfig.h"
#include "tagstore.h"
#include "doclibrary.h"
#include "strokebuilder.h"
#include "scribblepen.h"
#include "resources.h"  // _() for translated strings
#include "ulib/fileutil.h"
#include "miniz/miniz_zip.h"

// Sumi units per Noteful unit: Sumi works at 150 units per inch, Noteful at 132
static constexpr Dim NOTEFUL_SCALE = 150.0/Noteful::UNITS_PER_INCH;
// Noteful's paper templates draw their lines in this color (read from the templates' own PDFs)
static const Color NOTEFUL_RULE_COLOR(0x9a, 0x98, 0x88);
// room left below ink that runs past the page's bottom edge, in Noteful units
static constexpr double EXTENDED_PAGE_MARGIN = 40;
static const char* NOTEFUL_BACKGROUND_CLASS = "write-noteful-background";

bool NotefulImport::isNotefulFile(const char* filename)
{
  if(!filename)
    return false;
  std::string ext = FSPath(filename).extension();
  for(char& c : ext)
    c = char(tolower((unsigned char)c));
  return ext == "noteful";
}

static Point toSumi(const Noteful::Point& point) { return Point(point.x*NOTEFUL_SCALE, point.y*NOTEFUL_SCALE); }

static Color toColor(const double rgba[4])
{
  return Color::fromFloat(float(rgba[0]), float(rgba[1]), float(rgba[2]), float(rgba[3]));
}

// A pressure pen's stroke, built by Sumi's own stroke builder so it is an ordinary variable-width
//  stroke - erasable and restyleable like one drawn here.  Linear pressure (prParam 1, wRatio 1) makes
//  width = pen width * pressure; the pen gets Noteful's width at the stroke's peak pressure and each
//  point its share of that peak, so widths follow Noteful's pressure values proportionally.
static Element* pressureStrokeElement(const Noteful::Stroke& stroke)
{
  const Dim MIN_PRESSURE = 0.1;  // a zero-width point would pinch the outline shut
  float peak = *std::max_element(stroke.pressure.begin(), stroke.pressure.end());
  if(!(peak > 0))
    peak = 1;
  ScribblePen pen(toColor(stroke.rgba), stroke.width*peak*NOTEFUL_SCALE,
      ScribblePen::WIDTH_PR | ScribblePen::TIP_ROUND, 1, 1);
  std::unique_ptr<StrokeBuilder> builder(StrokeBuilder::create(pen));
  for(size_t ii = 0; ii < stroke.points.size(); ++ii) {
    Point pos = toSumi(stroke.points[ii]);
    builder->addInputPoint(StrokePoint(pos.x, pos.y, std::max(MIN_PRESSURE, Dim(stroke.pressure[ii]/peak))));
  }
  if(stroke.points.size() == 1) {
    Point pos = toSumi(stroke.points.front());
    builder->addInputPoint(StrokePoint(pos.x, pos.y, std::max(MIN_PRESSURE, Dim(stroke.pressure[0]/peak))));
  }
  return builder->finish();
}

static Element* strokeElement(const Noteful::Stroke& stroke)
{
  if(stroke.pressure.size() == stroke.points.size() && !stroke.pressure.empty())
    return pressureStrokeElement(stroke);
  SvgPath* svgPath = new SvgPath();
  Path2D* path = svgPath->path();
  path->moveTo(toSumi(stroke.points.front()));
  for(size_t ii = 1; ii < stroke.points.size(); ++ii)
    path->lineTo(toSumi(stroke.points[ii]));
  if(stroke.points.size() == 1)
    path->lineTo(toSumi(stroke.points.front()));  // a dot; round caps draw it
  // the same attributes StrokedStrokeBuilder gives a round-tipped pen stroke
  svgPath->setAttr<color_t>("fill", Color::NONE);
  setSvgStrokeColor(svgPath, toColor(stroke.rgba));
  svgPath->setAttr<float>("stroke-width", float(stroke.width*NOTEFUL_SCALE));
  svgPath->setAttr<int>("stroke-linecap", Painter::RoundCap);
  svgPath->setAttr<int>("stroke-linejoin", Painter::RoundJoin);
  svgPath->addClass(Element::STROKE_PEN_CLASS);
  return new Element(svgPath);
}

static Element* shapeElement(const Noteful::Shape& shape)
{
  SvgPath* svgPath = new SvgPath();
  Path2D* path = svgPath->path();
  size_t next = 0;
  auto pt = [&]() { return toSumi(shape.points[next++]); };
  // notefulfile.cpp only passes shapes whose ops and points agree
  for(int op : shape.ops) {
    switch(op) {
    case Noteful::Shape::MOVE: path->moveTo(pt());  break;
    case Noteful::Shape::LINE: path->lineTo(pt());  break;
    case Noteful::Shape::QUAD: { Point c = pt();  path->quadTo(c, pt());  break; }
    case Noteful::Shape::CUBIC: { Point c1 = pt();  Point c2 = pt();  path->cubicTo(c1, c2, pt());  break; }
    case Noteful::Shape::CLOSE: path->closeSubpath();  break;
    }
  }
  svgPath->setAttr<color_t>("fill", Color::NONE);
  // Noteful stores a highlighter's colour opaque and makes it translucent when drawing; imported at
  //  the alpha of Sumi's own marker (ScribbleMode's highlightPen), or it paints over the words it marks
  double rgba[4] = {shape.rgba[0], shape.rgba[1], shape.rgba[2], shape.rgba[3]*(shape.highlighter ? 0.5 : 1)};
  setSvgStrokeColor(svgPath, toColor(rgba));
  svgPath->setAttr<float>("stroke-width", float(shape.width*NOTEFUL_SCALE));
  svgPath->setAttr<int>("stroke-linecap", Painter::RoundCap);
  svgPath->setAttr<int>("stroke-linejoin", Painter::RoundJoin);
  // and it multiplies, as Sumi's marker does (StrokeBuilder::create())
  if(shape.highlighter)
    svgPath->setAttr<int>("comp-op", Painter::CompOp_Multiply);
  // no pen class, as for Sumi's own shapes: a pen class would have toPenPoints() reinterpret the path
  //  as variable-width pen geometry
  return new Element(svgPath);
}

static bool isPdfAsset(const std::string& asset) { return asset.compare(0, 4, "%PDF") == 0; }

// Every image the import keeps holds only its encoded bytes (PdfImport::encodedOnly) until the document
//  is saved: kept decoded, a page background rendered at 300 DPI is ~35 MB, and a long notebook took more
//  memory than an iPad allows an app.  Pictures keep the format they came in.
static Image::Encoding keptEncoding(const Image& image)
{
  return image.encoding == Image::JPEG ? Image::JPEG : Image::PNG;
}

// The PDF the page backgrounds come from, kept open from one page to the next: a notebook is mostly one
//  imported PDF, page after page, and opening it for each page meant parsing all of it again each time.
struct PdfAssetCache {
  std::string assetId;
  PdfImport::Renderer renderer;
};

// A picture: JPEG/PNG as is; a PDF's first page rendered at opts.dpi
static Image decodePicture(const std::string& asset, const NotefulImport::Options& opts, std::string* error)
{
  if(isPdfAsset(asset)) {
    PdfImport::Renderer renderer;
    Image image = renderer.openMemory(asset, opts.budget)
        ? renderer.render(0, opts.dpi, opts.lossy ? Image::JPEG : Image::PNG) : Image(0, 0);
    if(image.isNull() && error)
      *error = renderer.error();
    return image;
  }
  Image image = Image::decodeBuffer(asset.data(), asset.size());
  if(image.isNull() && error)
    *error = "unreadable image";
  return image;
}

static Element* imageElement(const Noteful::ImageItem& item, const Image& full, PdfImport::MemoryBudget* budget)
{
  // the crop is in display units; the frame shows exactly the crop region
  Dim sx = item.displayW > 0 ? full.width/item.displayW : 1;
  Dim sy = item.displayH > 0 ? full.height/item.displayH : 1;
  Rect crop = Rect::ltrb(item.cropX0*sx, item.cropY0*sy, item.cropX1*sx, item.cropY1*sy)
      .rectIntersect(Rect::wh(full.width, full.height));
  if(crop.width() < 1 || crop.height() < 1)
    return NULL;
  // an uncropped picture keeps the very bytes it came in, never decoded at all
  bool whole = crop.left < 0.5 && crop.top < 0.5 && crop.right > full.width - 0.5 && crop.bottom > full.height - 0.5;
  Image kept = whole ? PdfImport::encodedOnly(full, keptEncoding(full))
      : PdfImport::encodedOnly(full.cropped(crop), keptEncoding(full));
  if(kept.isNull())
    return NULL;
  if(budget)
    budget->keep(kept.encData.size());
  Rect bounds = Rect::ltwh(item.x*NOTEFUL_SCALE, item.y*NOTEFUL_SCALE,
      item.width*NOTEFUL_SCALE, item.height*NOTEFUL_SCALE);
  SvgImage* image = new SvgImage(std::move(kept), bounds);
  // rotated about the frame centre, as shapes are; a node transform is how Sumi rotates an image too
  if(item.rotation != 0)
    image->setTransform(Transform2D::rotating(item.rotation, bounds.center()));
  return new Element(image);
}

static Page* createPage(const Noteful::Notebook& notebook, const Noteful::Page& src, const NotefulImport::Options& opts,
    PdfAssetCache* pdfCache, std::vector<std::string>* warnings)
{
  Dim width = src.width*NOTEFUL_SCALE;
  Dim nominalHeight = src.height*NOTEFUL_SCALE;
  double bottom = src.contentBottom();
  Dim height = bottom > src.height ? (bottom + EXTENDED_PAGE_MARGIN)*NOTEFUL_SCALE : nominalHeight;
  Color paper = src.hasPaperColor ? Color::fromRgb(src.paperColor) : Color(Color::WHITE);

  if(src.isPaper) {
    // Sumi's own ruling, so it stays paper rather than a picture of paper.  Noteful's spacing is in points.
    Dim pitch = src.lineHeight*Noteful::UNITS_PER_POINT*NOTEFUL_SCALE;
    PageProperties props(width, height, src.lineType == 2 ? pitch : 0, pitch, 0, paper, NOTEFUL_RULE_COLOR);
    return new Page(props);
  }

  PageProperties props(width, height, 0, 0, 0, paper, Color::BLUE);
  Page* page = new Page(props);
  // The background covers the page's nominal size; an extended page continues on plain paper below it.
  //  A cropped page shows only part of its asset page: keep the pixels that land on the page - `part`, as
  //  fractions of the asset page, so a PDF page is rendered only that far.
  Rect bounds = Rect::ltwh(0, 0, width, nominalHeight);
  Rect part;  // invalid: the whole asset page
  if(src.hasAssetRect) {
    Rect placed = Rect::ltwh(src.assetRect[0]*NOTEFUL_SCALE, src.assetRect[1]*NOTEFUL_SCALE,
        src.assetRect[2]*NOTEFUL_SCALE, src.assetRect[3]*NOTEFUL_SCALE);
    Rect visible = Rect(placed).rectIntersect(bounds);
    if(visible.width() >= 1 && visible.height() >= 1 && placed.width() > 0 && placed.height() > 0) {
      part = Rect::ltrb((visible.left - placed.left)/placed.width(), (visible.top - placed.top)/placed.height(),
          (visible.right - placed.left)/placed.width(), (visible.bottom - placed.top)/placed.height());
      bounds = visible;
    }
  }

  std::string error;
  Image background(0, 0);
  if(!src.assetId.empty()) {
    PdfImport::Renderer& renderer = pdfCache->renderer;
    // the asset is copied out of the notebook only when it is not the PDF already open
    if(pdfCache->assetId != src.assetId || !renderer.isOpen()) {
      std::string asset = notebook.asset(src.assetId);
      if(isPdfAsset(asset)) {
        pdfCache->assetId = src.assetId;
        if(!renderer.openMemory(std::move(asset), opts.budget))
          error = renderer.error();
      }
      else if(!asset.empty()) {
        // a photo or a scan: decoded (to crop it) only when cropped
        Image full = Image::decodeBuffer(asset.data(), asset.size());
        if(full.isNull())
          error = "unreadable image";
        else if(!part.isValid())
          background = PdfImport::encodedOnly(full, keptEncoding(full));
        else {
          Rect pixels = Rect::ltrb(part.left*full.width, part.top*full.height,
              part.right*full.width, part.bottom*full.height);
          if(pixels.width() >= 1 && pixels.height() >= 1)
            background = PdfImport::encodedOnly(full.cropped(pixels), keptEncoding(full));
          else {
            background = PdfImport::encodedOnly(full, keptEncoding(full));
            bounds = Rect::ltwh(0, 0, width, nominalHeight);
          }
        }
        if(!background.isNull() && opts.budget)
          opts.budget->keep(background.encData.size());
      }
    }
    if(pdfCache->assetId == src.assetId && renderer.isOpen()) {
      background = renderer.render(src.assetPage, opts.dpi, opts.lossy ? Image::JPEG : Image::PNG, NULL, NULL, part);
      if(background.isNull())
        error = renderer.error();
    }
  }
  if(background.isNull()) {
    if(!src.assetId.empty())
      warnings->push_back("page background " + src.assetId + ": " + (error.empty() ? "missing" : error));
    return page;
  }
  SvgImage* image = new SvgImage(std::move(background), bounds);
  image->addClass(NOTEFUL_BACKGROUND_CLASS);
  page->ruleNode->addChild(image);
  // as for PDF import: custom ruling survives save/load and page size changes, and is not copied to
  //  the next page added after this one
  page->ruleNode->removeClass("write-std-ruling");
  page->ruleNode->addClass("write-no-dup");
  page->isCustomRuling = true;
  return page;
}

// The document config a bare Document is saved with: the layer table (ScribbleDoc::layersChanged keeps
//  it there) and, for a library import, the document's tag ids
static void writeDocConfig(Document* doc, const std::vector<std::string>& tagIds)
{
  ScribbleConfig cfg(NULL);
  cfg.set("layers", doc->layers.serialize().c_str());
  if(!tagIds.empty())
    cfg.set("tags", TagStore::formatTagList(tagIds).c_str());
  cfg.saveConfig(doc->resetConfigNode());
}

static int importParsed(Document* doc, const Noteful::Notebook& notebook, const NotefulImport::Options& opts,
    NotefulImport::Result& res, std::string* errorOut)
{
  res.title = notebook.title;
  res.tags = notebook.tags;
  res.warnings = notebook.warnings;

  // layers, bottom first; the bottom one takes over the default layer, so nothing is ever orphaned
  doc->layers = LayerList();
  std::map<uint32_t, int> layerIds;
  for(size_t ii = 0; ii < notebook.layers.size(); ++ii) {
    const Noteful::Layer& layer = notebook.layers[ii];
    if(ii == 0) {
      doc->layers.setName(LayerList::DEFAULT_LAYER, layer.name);
      layerIds[layer.id] = LayerList::DEFAULT_LAYER;
    }
    else
      layerIds[layer.id] = doc->layers.addLayer(layer.name);
  }
  // an id missing from the table goes to the bottom layer rather than failing open to a phantom one
  auto sumiLayer = [&layerIds](uint32_t id) {
    auto it = layerIds.find(id);
    return it != layerIds.end() ? it->second : LayerList::DEFAULT_LAYER;
  };

  // the outline entries of each page, in outline order - given to the page while it is made, since an
  //  ImportSaver may have written and unloaded it by the time the loop is done
  std::map<std::string, std::vector<const Noteful::OutlineEntry*>> outlineByPage;
  for(const Noteful::OutlineEntry& entry : notebook.outline)
    outlineByPage[entry.pageId].push_back(&entry);

  PdfAssetCache pdfCache;
  int imported = 0;
  int numPages = int(notebook.pages.size());
  for(int pageNum = 0; pageNum < numPages; ++pageNum) {
    if(opts.onProgress && !opts.onProgress(pageNum, numPages))
      break;
    const Noteful::Page& src = notebook.pages[pageNum];
    Page* page = createPage(notebook, src, opts, &pdfCache, &res.warnings);
    doc->insertPage(page);  // before adding content: addStroke stacks by the document's layer table
    ++imported;

    // one outline entry per page in Sumi; the first (in outline order) wins
    auto outline = outlineByPage.find(src.id);
    if(outline != outlineByPage.end()) {
      const Noteful::OutlineEntry* first = outline->second.front();
      page->setOutlineEntry(first->title.c_str(), std::min(first->level, int(Page::MAX_OUTLINE_LEVEL)));
      for(size_t ii = 1; ii < outline->second.size(); ++ii)
        res.warnings.push_back("outline entry \"" + outline->second[ii]->title + "\" dropped: its page already has one");
    }

    for(const Noteful::Shape& shape : src.shapes) {
      int layer = sumiLayer(shape.layer);
      // a highlighter goes under everything on its layer, as a DRAW_UNDER marker stroke does
      page->addStroke(shapeElement(shape), shape.highlighter ? page->layerFirstElement(layer) : NULL, layer);
    }
    std::map<std::string, Image> images;
    for(const Noteful::ImageItem& item : src.images) {
      auto it = images.find(item.assetId);
      if(it == images.end()) {
        std::string imageError;
        it = images.emplace(item.assetId, decodePicture(notebook.asset(item.assetId), opts, &imageError)).first;
        if(it->second.isNull())
          res.warnings.push_back("image " + item.assetId + ": " + (imageError.empty() ? "missing" : imageError));
      }
      if(Element* element = it->second.isNull() ? NULL : imageElement(item, it->second, opts.budget))
        page->addStroke(element, NULL, sumiLayer(item.layer));
    }
    // ink last, so it draws over shapes and images on the same layer
    for(const Noteful::Stroke& stroke : src.strokes) {
      if(!stroke.points.empty())
        page->addStroke(strokeElement(stroke), NULL, sumiLayer(stroke.layer));
    }
    res.textBoxesSkipped += int(src.texts.size());
    if(opts.saver && !opts.saver->pageDone()) {
      if(errorOut) *errorOut = _("The imported document could not be saved.");
      return -1;
    }
  }

  writeDocConfig(doc, {});
  if(imported == 0 && errorOut)
    *errorOut = _("This notebook has no pages.");
  return imported > 0 ? imported : -1;
}

// read through ulib, whose fopen takes UTF-8 names on Windows too - Noteful::load() uses plain fopen
static bool loadNotebook(const char* filename, Noteful::Notebook* notebook, std::string* error)
{
  std::string data;
  if(!readFile(&data, filename)) {
    *error = _("The notebook could not be opened.");
    return false;
  }
  return Noteful::parse(std::move(data), notebook, error);
}

int NotefulImport::importNoteful(Document* doc, const char* filename, const Options& opts,
    Result* result, std::string* errorOut)
{
  Noteful::Notebook notebook;
  std::string error;
  if(!loadNotebook(filename, &notebook, &error)) {
    if(errorOut) *errorOut = error;
    return -1;
  }
  Result localResult;
  return importParsed(doc, notebook, opts, result ? *result : localResult, errorOut);
}

// --- archives ---

static bool hasExtension(const std::string& name, const char* ext)
{
  std::string actual = FSPath(name).extension();
  for(char& c : actual)
    c = char(tolower((unsigned char)c));
  return actual == ext;
}

bool NotefulImport::isNotefulArchive(const char* path)
{
  return path && (hasExtension(path, "zip") || isDirectory(path));
}

bool NotefulImport::isImportable(const char* path)
{
  return isNotefulFile(path) || isNotefulArchive(path);
}

namespace {

// One notebook found in an archive: where it is and how to get its bytes
struct ArchiveItem {
  std::string path;    // inside the archive, "/" separated
  std::string folder;  // its folder, "" at the top
  unsigned int zipIndex = 0;
};

// every .noteful below `dir`, depth first, folders sorted so the order does not depend on the filesystem
void findNotebooks(const FSPath& dir, const std::string& folder, std::vector<ArchiveItem>* items, int depth = 0)
{
  if(depth > 32)
    return;  // a symlink loop
  std::vector<std::string> names = lsDirectory(dir);
  std::sort(names.begin(), names.end());
  for(std::string name : names) {
    if(!name.empty() && name.back() == '/')
      name.pop_back();  // lsDirectory marks directories with a trailing slash
    if(name.empty() || name[0] == '.')
      continue;
    FSPath child = dir.child(name);
    std::string childFolder = folder.empty() ? name : folder + "/" + name;
    if(isDirectory(child.c_str()))
      findNotebooks(child, childFolder, items, depth + 1);
    else if(hasExtension(name, "noteful"))
      items->push_back(ArchiveItem{child.path, folder});
  }
}

// Tag ids for "/"-separated tag paths, creating any tag that does not exist yet
std::vector<std::string> tagIdsFor(TagStore* store, const std::vector<std::string>& paths)
{
  std::vector<std::string> ids;
  for(const std::string& path : paths) {
    std::string parentId;
    size_t start = 0;
    while(start <= path.size()) {
      size_t end = path.find('/', start);
      std::string name = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
      if(!name.empty()) {
        const TagNode* existing = store->findTagByName(name, parentId);
        parentId = existing ? existing->id : store->addTag(name, parentId);
      }
      if(end == std::string::npos)
        break;
      start = end + 1;
    }
    if(!parentId.empty() && std::find(ids.begin(), ids.end(), parentId) == ids.end())
      ids.push_back(parentId);
  }
  return ids;
}

}  // namespace

int NotefulImport::importArchive(const char* path, const char* libraryDir, const ArchiveOptions& opts,
    ArchiveResult* result, std::string* errorOut)
{
  ArchiveResult localResult;
  ArchiveResult& res = result ? *result : localResult;
  auto fail = [errorOut](const std::string& message) { if(errorOut) *errorOut = message;  return -1; };

  std::vector<ArchiveItem> items;
  mz_zip_archive zip;
  memset(&zip, 0, sizeof(zip));
  FILE* zipFile = NULL;
  bool isZip = !isDirectory(path) && !hasExtension(path, "noteful");
  if(hasExtension(path, "noteful"))
    items.push_back(ArchiveItem{path, ""});  // a single notebook: same naming, tags and saving
  else if(isZip) {
    // our own FILE*: ulib's fopen takes UTF-8 names on Windows, miniz's would not
    zipFile = fopen(path, "rb");
    if(!zipFile || !mz_zip_reader_init_cfile(&zip, zipFile, 0, 0)) {
      if(zipFile) fclose(zipFile);
      return fail(_("The archive could not be opened."));
    }
    for(mz_uint ii = 0; ii < mz_zip_reader_get_num_files(&zip); ++ii) {
      mz_zip_archive_file_stat stat;
      if(!mz_zip_reader_file_stat(&zip, ii, &stat) || stat.m_is_directory || !hasExtension(stat.m_filename, "noteful"))
        continue;
      std::string entry = stat.m_filename;
      std::replace(entry.begin(), entry.end(), '\\', '/');
      size_t slash = entry.rfind('/');
      // "__MACOSX/..." holds Finder's resource forks, not notebooks
      if(entry.compare(0, 9, "__MACOSX/") == 0 || FSPath(entry).fileName()[0] == '.')
        continue;
      items.push_back(ArchiveItem{entry, slash == std::string::npos ? "" : entry.substr(0, slash), ii});
    }
  }
  else
    findNotebooks(FSPath(path), "", &items);
  if(items.empty()) {
    if(isZip) { mz_zip_reader_end(&zip);  fclose(zipFile); }
    return fail(_("No Noteful notebooks were found."));
  }

  FSPath library(libraryDir);
  createPath(library);
  TagStore tags(library.child(".write-tags").c_str());
  tags.load();

  for(size_t ii = 0; ii < items.size(); ++ii) {
    const ArchiveItem& item = items[ii];
    if(opts.onNotebook && !opts.onNotebook(int(ii), int(items.size()), item.path))
      break;
    ArchiveEntry entry;
    entry.archivePath = item.path;
    entry.folder = item.folder;

    Noteful::Notebook notebook;
    std::string error;
    bool loaded;
    if(isZip) {
      size_t size = 0;
      void* bytes = mz_zip_reader_extract_to_heap(&zip, item.zipIndex, &size, 0);
      // freed before parsing, so a big notebook is not held twice while it is imported
      bool extracted = bytes != NULL;
      std::string data = extracted ? std::string((const char*)bytes, size) : std::string();
      mz_free(bytes);
      loaded = extracted && Noteful::parse(std::move(data), &notebook, &error);
      if(!extracted)
        error = _("The notebook could not be extracted from the archive.");
    }
    else
      loaded = loadNotebook(item.path.c_str(), &notebook, &error);

    if(loaded) {
      std::string name = notebook.title.empty() ? FSPath(item.path).baseName() : notebook.title;
      FSPath out = DocLibrary::uniquePath(library, name, "svgz");
      // the notebook is held whole while it is imported
      if(opts.budget)
        opts.budget->setInput(notebook.data.size());
      {
        Document doc;
        // written page by page, so a long notebook is never in memory all at once
        PdfImport::ImportSaver saver(&doc, out.path, opts.budget);
        Options notebookOpts = opts;
        notebookOpts.saver = &saver;
        if(importParsed(&doc, notebook, notebookOpts, entry.result, &error) > 0) {
          std::vector<std::string> tagPaths = entry.result.tags;
          if(opts.folderTags && !item.folder.empty())
            tagPaths.push_back(item.folder);
          entry.tagPaths = tagPaths;
          writeDocConfig(&doc, tagIdsFor(&tags, tagPaths));
          if(saver.finish())
            entry.docPath = out.path;
          else
            error = _("The imported document could not be saved.");
        }
      }
      // nothing half-written stays in the library (the document, which held the file open, is gone)
      if(entry.docPath.empty() && out.exists())
        removeFile(out.path);
      if(opts.budget) {
        opts.budget->setInput(0);
        opts.budget->written();
      }
    }
    entry.error = entry.docPath.empty() ? (error.empty() ? _("The notebook has no pages.") : error) : "";
    (entry.docPath.empty() ? res.failed : res.imported) += 1;
    res.entries.push_back(std::move(entry));
  }
  tags.save();

  if(isZip) { mz_zip_reader_end(&zip);  fclose(zipFile); }
  if(res.imported == 0)
    return fail(res.entries.empty() ? std::string(_("The import was cancelled.")) : res.entries.front().error);
  return res.imported;
}
