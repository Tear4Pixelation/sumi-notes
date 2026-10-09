#include "undopersist.h"
#include <sstream>
#include <algorithm>
#include "syncundo.h"
#include "document.h"
#include "scribbledoc.h"
#include "scribblesync.h"
#include "basics.h"
#include "usvg/svgxml.h"
#include "usvg/svgwriter.h"
#include "usvg/svgparser.h"
#include "shape.h"
#include "rulingregion.h"

// docs/agent/undo-persistence.md has the format, the keying and the traps; read it before changing this.

std::string UndoPersist::dir;
bool UndoPersist::enabled = true;

// sidecars older than this are deleted, and the folder is kept under this size, oldest first
static constexpr Timestamp MAX_AGE_SECS = 60*24*3600;
static constexpr long MAX_TOTAL_BYTES = 64L << 20;
// how much of the head and the tail of the document the fingerprint hashes
static constexpr size_t FINGERPRINT_SPAN = 1 << 16;

static std::string realStr(double value) { return fstring("%.17g", value); }

static uint64_t fnv1a(const void* data, size_t len, uint64_t hash = 0xcbf29ce484222325ULL)
{
  const unsigned char* bytes = static_cast<const unsigned char*>(data);
  for(size_t ii = 0; ii < len; ++ii) {
    hash ^= bytes[ii];
    hash *= 0x100000001b3ULL;
  }
  return hash;
}

// type and bounds of an element, which a reference must still match when it is resolved after reopening
static std::string elementSignature(Element* s)
{
  Rect bbox = s->bbox();
  if(!bbox.isValid())
    return fstring("%d -", int(s->node->type()));
  return fstring("%d %.2f %.2f %.2f %.2f", int(s->node->type()), bbox.left, bbox.top, bbox.right, bbox.bottom);
}

static bool signaturesMatch(const std::string& saved, const std::string& live)
{
  if(saved == live)
    return true;
  int savedType = -1, liveType = -1;
  double savedBox[4], liveBox[4];
  if(sscanf(saved.c_str(), "%d %lf %lf %lf %lf", &savedType, &savedBox[0], &savedBox[1], &savedBox[2], &savedBox[3]) != 5
      || sscanf(live.c_str(), "%d %lf %lf %lf %lf", &liveType, &liveBox[0], &liveBox[1], &liveBox[2], &liveBox[3]) != 5
      || savedType != liveType)
    return false;
  // the file stores coordinates to 3 decimals, so bounds move a little across a save and reload; a
  //  different element is (almost always) somewhere else entirely
  for(int ii = 0; ii < 4; ++ii) {
    if(std::abs(savedBox[ii] - liveBox[ii]) > 0.5 + 1E-3*std::abs(savedBox[ii]))
      return false;
  }
  return true;
}

// UndoPersistWriter

void UndoPersistWriter::fail(const char* why)
{
  if(!failed)
    PLATFORM_LOG("Undo history not persisted: %s\n", why);
  failed = true;
}

void UndoPersistWriter::beginStep()
{
  stepDoc.reset();
  stepDefs.clear();
  failed = false;
}

std::string UndoPersistWriter::endStep()
{
  std::ostringstream text;
  for(pugi::xml_node node : stepDoc.children())
    node.print(text, "", pugi::format_raw);
  return text.str();
}

pugi::xml_node UndoPersistWriter::item(const char* name)
{
  return stepDoc.append_child(name);
}

pugi::xml_node UndoPersistWriter::strokeItem(const char* name, Element* s, Page* page)
{
  pugi::xml_node node = item(name);
  node.append_attribute("s") = elementRef(s, page).c_str();
  node.append_attribute("pg") = pageRef(page).c_str();
  return node;
}

int UndoPersistWriter::useDef(int id)
{
  if(std::find(stepDefs.begin(), stepDefs.end(), id) == stepDefs.end())
    stepDefs.push_back(id);
  return id;
}

int UndoPersistWriter::pageDefId(Page* page)
{
  auto it = pageDefs.find(page);
  if(it != pageDefs.end())
    return useDef(it->second);
  // a page out of the document is always loaded: Document::deletePage() loads it first
  if(page->loadStatus != Page::LOAD_OK) {
    fail("detached page is not loaded");
    return -1;
  }
  MemStream pagesvg;
  if(!page->saveSVG(pagesvg)) {
    fail("detached page could not be written");
    return -1;
  }
  int id = defs.size();
  defs.push_back({fstring("<pdef id='%d'>", id) + std::string(pagesvg.data(), pagesvg.size()) + "</pdef>"});
  pageDefs[page] = id;
  return useDef(id);
}

std::string UndoPersistWriter::pageRef(Page* page)
{
  if(!page) {
    fail("item without a page");
    return "";
  }
  if(livePages.empty()) {
    for(size_t ii = 0; ii < document->pages.size(); ++ii)
      livePages[document->pages[ii]] = int(ii);
  }
  auto live = livePages.find(page);
  if(live != livePages.end())
    return fstring("L%d", live->second);
  int id = pageDefId(page);
  return id < 0 ? "" : fstring("P%d", id);
}

int UndoPersistWriter::indexOnPage(Page* page, Element* s)
{
  auto pageit = pageIndex.find(page);
  if(pageit == pageIndex.end()) {
    std::map<Element*, int>& index = pageIndex[page];
    int idx = 0;
    for(Element* child : page->children())
      index[child] = idx++;
    pageit = pageIndex.find(page);
  }
  auto it = pageit->second.find(s);
  return it != pageit->second.end() ? it->second : -1;
}

std::string UndoPersistWriter::elementRef(Element* s, Page* page)
{
  if(!s)
    return "";
  SvgNode* parent = s->node->parent();
  if(!parent) {
    auto it = elementDefs.find(s);
    if(it != elementDefs.end())
      return fstring("E%d", useDef(it->second));
    XmlStreamWriter xmlwriter;
    SvgWriter(xmlwriter).serialize(s->node);
    std::ostringstream text;
    xmlwriter.save(text);
    int id = defs.size();
    defs.push_back({fstring("<edef id='%d'>", id) + text.str() + "</edef>"});
    elementDefs[s] = id;
    return fstring("E%d", useDef(id));
  }
  // on a page: normally the one the item holds it against; otherwise whichever live page has it
  Page* owner = page && page->contentNode == parent ? page : NULL;
  if(!owner) {
    int pagenum = document->pageNumForElement(s);
    if(pagenum >= 0 && document->pages[pagenum]->contentNode == parent)
      owner = document->pages[pagenum];
  }
  // e.g. an element nested in a group: the editing paths clone their way around that, so an undo item
  //  should never hold one, but if one does there is no index that would find it again
  if(!owner || owner->loadStatus != Page::LOAD_OK) {
    fail("element is neither on a page nor detached");
    return "";
  }
  int idx = indexOnPage(owner, s);
  std::string pref = pageRef(owner);
  if(idx < 0 || pref.empty()) {
    fail("element not found on its page");
    return "";
  }
  std::string ref = pref + fstring(".%d", idx);
  verify[ref] = elementSignature(s);
  return ref;
}

// persist() for each item type.  Every value written is the one the item *holds*, which is the state
//  undo (or, for an undone item, redo) swaps in - not the live state serialize() sends to a peer.

void UndoHistoryItem::persist(UndoPersistWriter& out)
{
  out.fail("undo item type cannot be persisted");
}

void StrokeAddedItem::persist(UndoPersistWriter& out)
{
  pugi::xml_node node = out.strokeItem("addstroke", s, page);
  node.append_attribute("next") = out.elementRef(next, page).c_str();
}

void StrokeDeletedItem::persist(UndoPersistWriter& out)
{
  pugi::xml_node node = out.strokeItem("delstroke", s, page);
  node.append_attribute("next") = out.elementRef(next, page).c_str();
}

void StrokeChangedItem::persist(UndoPersistWriter& out)
{
  pugi::xml_node node = out.strokeItem("strokechanged", s, page);
  node.append_attribute("color") = fstring("%u", props.color.argb()).c_str();
  node.append_attribute("width") = realStr(props.width).c_str();
  node.append_attribute("dash") = props.dashArray.c_str();
}

void ShapeChangedItem::persist(UndoPersistWriter& out)
{
  const ShapeDef* def = shapeDef(params.id);
  if(!def)
    return out.fail("unknown shape");
  pugi::xml_node node = out.strokeItem("shapechanged", s, page);
  node.append_attribute("shape") = def->id;
  node.append_attribute("shapepts") = serializeShapePoints(params.points).c_str();
  node.append_attribute("rx") = realStr(params.rx).c_str();
  node.append_attribute("ry") = realStr(params.ry).c_str();
  node.append_attribute("tight") = realStr(params.tightness).c_str();
  node.append_attribute("flags") = params.flags;
}

void RegionChangedItem::persist(UndoPersistWriter& out)
{
  pugi::xml_node node = out.strokeItem("regionchanged", s, page);
  node.append_attribute("pts") = serializeRegionPoints(params.corners).c_str();
  node.append_attribute("origin") = serializeRegionPoints({params.origin}).c_str();
  node.append_attribute("angle") = realStr(params.angle).c_str();
  node.append_attribute("xruling") = realStr(params.xRuling).c_str();
  node.append_attribute("yruling") = realStr(params.yRuling).c_str();
  node.append_attribute("dotradius") = realStr(params.dotRadius).c_str();
  node.append_attribute("opaque") = params.opaque ? 1 : 0;
  node.append_attribute("outline") = params.outline ? 1 : 0;
  node.append_attribute("staff") = params.staff ? 1 : 0;
  node.append_attribute("axes") = params.axes ? 1 : 0;
}

void StrokeLayerItem::persist(UndoPersistWriter& out)
{
  pugi::xml_node node = out.strokeItem("layerchanged", s, page);
  node.append_attribute("layer") = layer;
  node.append_attribute("next") = out.elementRef(next, page).c_str();
}

void StrokeTranslateItem::persist(UndoPersistWriter& out)
{
  pugi::xml_node node = out.strokeItem("translate", s, page);
  node.append_attribute("x") = realStr(xoffset).c_str();
  node.append_attribute("y") = realStr(yoffset).c_str();
}

void StrokeTransformItem::persist(UndoPersistWriter& out)
{
  pugi::xml_node node = out.strokeItem("transform", s, page);
  const real* matrix = transform.asArray();
  std::string values;
  for(int ii = 0; ii < 6; ++ii)
    values += (ii ? " " : "") + realStr(matrix[ii]);
  node.append_attribute("matrix") = values.c_str();
  node.append_attribute("internalscale") =
      (realStr(transform.internalScale[0]) + " " + realStr(transform.internalScale[1])).c_str();
}

void PageChangedItem::persist(UndoPersistWriter& out)
{
  pugi::xml_node node = out.item("pagechanged");
  node.append_attribute("pg") = out.pageRef(p).c_str();
  node.append_attribute("width") = realStr(props.width).c_str();
  node.append_attribute("height") = realStr(props.height).c_str();
  node.append_attribute("color") = fstring("%u", props.color.argb()).c_str();
  node.append_attribute("xruling") = realStr(props.xRuling).c_str();
  node.append_attribute("yruling") = realStr(props.yRuling).c_str();
  node.append_attribute("marginLeft") = realStr(props.marginLeft).c_str();
  node.append_attribute("rulecolor") = fstring("%u", props.ruleColor.argb()).c_str();
  node.append_attribute("dotradius") = realStr(props.dotRadius).c_str();
  node.append_attribute("staff") = props.staff ? 1 : 0;
}

void PageOutlineItem::persist(UndoPersistWriter& out)
{
  pugi::xml_node node = out.item("outlinechanged");
  node.append_attribute("pg") = out.pageRef(p).c_str();
  node.append_attribute("title") = title.c_str();
  node.append_attribute("level") = level;
}

void PageAddedItem::persist(UndoPersistWriter& out)
{
  pugi::xml_node node = out.item("addpage");
  node.append_attribute("pg") = out.pageRef(p).c_str();
  node.append_attribute("pagenum") = pagenum;
}

void PageDeletedItem::persist(UndoPersistWriter& out)
{
  pugi::xml_node node = out.item("delpage");
  node.append_attribute("pg") = out.pageRef(p).c_str();
  node.append_attribute("pagenum") = pagenum;
}

void LayerTableItem::persist(UndoPersistWriter& out)
{
  pugi::xml_node node = out.item("layertable");
  node.append_attribute("id") = id;
  node.append_attribute("present") = present ? 1 : 0;
  node.append_attribute("below") = belowId;
  node.append_attribute("locked") = info.locked ? 1 : 0;
  node.append_attribute("hidden") = info.hidden ? 1 : 0;
  node.append_attribute("name") = info.name.c_str();
}

void ThemeChangedItem::persist(UndoPersistWriter& out)
{
  const PaletteRecipe& recipe = state.recipe;
  pugi::xml_node node = out.item("themechanged");
  node.append_attribute("gen") = recipe.gen.c_str();
  node.append_attribute("seedhue") = realStr(recipe.seedHue).c_str();
  node.append_attribute("vividness") = realStr(recipe.vividness).c_str();
  node.append_attribute("depth") = realStr(recipe.depth).c_str();
  node.append_attribute("contrast") = realStr(recipe.minContrast).c_str();
  node.append_attribute("jitter") = realStr(recipe.jitter).c_str();
  node.append_attribute("paperl") = realStr(recipe.paperL).c_str();
  node.append_attribute("paperwarm") = realStr(recipe.paperWarm).c_str();
  node.append_attribute("families") = recipe.families;
  node.append_attribute("pagecolor") = fstring("%u", state.pageColor.argb()).c_str();
  node.append_attribute("rulecolor") = fstring("%u", state.ruleColor.argb()).c_str();
  node.append_attribute("bookmarkcolor") = fstring("%u", state.bookmarkColor.argb()).c_str();
  node.append_attribute("linkcolor") = fstring("%u", state.linkColor.argb()).c_str();
}

// reading

class UndoPersistReader
{
public:
  Document* document;
  std::map<std::string, std::string> verify;
  std::map<int, Element*> elementDefs;
  std::map<int, Page*> pageDefs;
  std::map<Page*, std::vector<Element*>> pageChildren;
  bool failed = false;

  UndoPersistReader(Document* doc) : document(doc) {}

  bool fail(const char* why)
  {
    if(!failed)
      PLATFORM_LOG("Saved undo history discarded: %s\n", why);
    failed = true;
    return false;
  }

  Page* page(const char* ref)
  {
    int idx = -1;
    if(ref[0] == 'L' && sscanf(ref + 1, "%d", &idx) == 1 && idx >= 0 && idx < int(document->pages.size())) {
      Page* livepage = document->pages[idx];
      if(!livepage->ensureLoaded(false)) {
        fail("page does not load");
        return NULL;
      }
      return livepage;
    }
    if(ref[0] == 'P' && sscanf(ref + 1, "%d", &idx) == 1 && pageDefs.count(idx))
      return pageDefs[idx];
    fail("bad page reference");
    return NULL;
  }

  // `ok` is false only for a reference that does not resolve; a NULL element (empty ref) is fine
  Element* element(const char* ref, bool& ok)
  {
    ok = true;
    if(!ref[0])
      return NULL;
    int id = -1;
    if(ref[0] == 'E') {
      if(sscanf(ref + 1, "%d", &id) == 1 && elementDefs.count(id))
        return elementDefs[id];
      ok = fail("bad element reference");
      return NULL;
    }
    const char* dot = strchr(ref, '.');
    int idx = -1;
    if(!dot || sscanf(dot + 1, "%d", &idx) != 1 || idx < 0) {
      ok = fail("bad element reference");
      return NULL;
    }
    Page* owner = page(std::string(ref, dot - ref).c_str());
    if(!owner) {
      ok = false;
      return NULL;
    }
    auto it = pageChildren.find(owner);
    if(it == pageChildren.end()) {
      std::vector<Element*>& children = pageChildren[owner];
      for(Element* child : owner->children())
        children.push_back(child);
      it = pageChildren.find(owner);
    }
    if(idx >= int(it->second.size())) {
      ok = fail("element index past end of page");
      return NULL;
    }
    Element* s = it->second[idx];
    auto sig = verify.find(ref);
    if(sig == verify.end() || !signaturesMatch(sig->second, elementSignature(s))) {
      ok = fail("element does not match the one the history was saved against");
      return NULL;
    }
    return s;
  }

  // the element ("s") and page ("pg") every stroke item has
  bool strokeRefs(const pugi::xml_node& node, Element*& s, Page*& pg)
  {
    bool ok = true;
    s = element(node.attribute("s").as_string(), ok);
    pg = ok ? page(node.attribute("pg").as_string()) : NULL;
    if(ok && pg && !s)
      return fail("stroke item without a stroke");
    return ok && pg && s;
  }

  // discard everything built so far: items never own anything until they are in the history (their
  //  discard() is what frees content), so the definitions are freed here directly
  void freeDefinitions()
  {
    for(auto& entry : elementDefs)
      entry.second->deleteNode();
    for(auto& entry : pageDefs)
      delete entry.second;
    elementDefs.clear();
    pageDefs.clear();
  }
};

UndoHistoryItem* UndoPersist::readItem(const pugi::xml_node& node, ScribbleDoc* sd, UndoPersistReader& rd)
{
  std::string name = node.name();
  Element* s = NULL;
  Page* pg = NULL;
  bool ok = true;
  if(name == "addstroke" || name == "delstroke") {
    if(!rd.strokeRefs(node, s, pg)) return NULL;
    Element* next = rd.element(node.attribute("next").as_string(), ok);
    if(!ok) return NULL;
    if(name == "addstroke")
      return new StrokeAddedItem(s, pg, next);
    return new StrokeDeletedItem(s, pg, next);
  }
  if(name == "strokechanged") {
    if(!rd.strokeRefs(node, s, pg)) return NULL;
    StrokeProperties props(Color::fromArgb(node.attribute("color").as_uint()), node.attribute("width").as_double());
    props.dashArray = node.attribute("dash").as_string();
    return new StrokeChangedItem(s, pg, props);
  }
  if(name == "shapechanged") {
    if(!rd.strokeRefs(node, s, pg)) return NULL;
    int aliasFlags = 0;
    Dim aliasTight = -1;
    ShapeParams params;
    params.id = shapeIdByStringId(node.attribute("shape").as_string(), &aliasFlags, &aliasTight);
    if(!shapeDef(params.id)) { rd.fail("unknown shape"); return NULL; }
    parseShapePoints(node.attribute("shapepts").as_string(), params.points);
    params.rx = node.attribute("rx").as_double();
    params.ry = node.attribute("ry").as_double();
    params.tightness = aliasTight >= 0 ? aliasTight : node.attribute("tight").as_double();
    params.flags = node.attribute("flags").as_int() | aliasFlags;
    return new ShapeChangedItem(s, pg, params);
  }
  if(name == "regionchanged") {
    if(!rd.strokeRefs(node, s, pg)) return NULL;
    if(!s->isRulingRegion()) { rd.fail("region item on a non-region"); return NULL; }
    RulingRegionParams params;
    parseRegionPoints(node.attribute("pts").as_string(), params.corners);
    std::vector<Point> origin;
    parseRegionPoints(node.attribute("origin").as_string(), origin);
    params.origin = origin.empty() ? Point(0, 0) : origin[0];
    params.angle = node.attribute("angle").as_double();
    params.xRuling = node.attribute("xruling").as_double();
    params.yRuling = node.attribute("yruling").as_double();
    params.dotRadius = node.attribute("dotradius").as_double();
    params.opaque = node.attribute("opaque").as_int(1) != 0;
    params.outline = node.attribute("outline").as_int(0) != 0;
    params.staff = node.attribute("staff").as_int(0) != 0;
    params.axes = node.attribute("axes").as_int(0) != 0;
    params.sanitize();
    return new RegionChangedItem(s, pg, params);
  }
  if(name == "layerchanged") {
    if(!rd.strokeRefs(node, s, pg)) return NULL;
    Element* next = rd.element(node.attribute("next").as_string(), ok);
    if(!ok) return NULL;
    return new StrokeLayerItem(s, pg, node.attribute("layer").as_int(), next);
  }
  if(name == "translate") {
    if(!rd.strokeRefs(node, s, pg)) return NULL;
    return new StrokeTranslateItem(s, pg, node.attribute("x").as_double(), node.attribute("y").as_double());
  }
  if(name == "transform") {
    if(!rd.strokeRefs(node, s, pg)) return NULL;
    std::vector<real> matrix = parseNumbersList(StringRef(node.attribute("matrix").as_string()), 6);
    std::vector<real> scale = parseNumbersList(StringRef(node.attribute("internalscale").as_string()), 2);
    if(matrix.size() != 6 || scale.size() != 2) { rd.fail("bad transform"); return NULL; }
    ScribbleTransform tf(Transform2D(matrix[0], matrix[1], matrix[2], matrix[3], matrix[4], matrix[5]),
        scale[0], scale[1]);
    return new StrokeTransformItem(s, pg, tf);
  }
  // page items
  bool pageItem = name == "pagechanged" || name == "outlinechanged" || name == "addpage" || name == "delpage";
  if(pageItem) {
    pg = rd.page(node.attribute("pg").as_string());
    if(!pg) return NULL;
  }
  if(name == "pagechanged") {
    PageProperties props(node.attribute("width").as_double(), node.attribute("height").as_double(),
        node.attribute("xruling").as_double(), node.attribute("yruling").as_double(),
        node.attribute("marginLeft").as_double(), Color::fromArgb(node.attribute("color").as_uint()),
        Color::fromArgb(node.attribute("rulecolor").as_uint()), node.attribute("dotradius").as_double(),
        node.attribute("staff").as_int(0) != 0);
    return new PageChangedItem(pg, props);
  }
  if(name == "outlinechanged")
    return new PageOutlineItem(pg, node.attribute("title").as_string(), node.attribute("level").as_int());
  if(name == "addpage")
    return new PageAddedItem(pg, node.attribute("pagenum").as_int(), rd.document);
  if(name == "delpage")
    return new PageDeletedItem(pg, node.attribute("pagenum").as_int(), rd.document);
  // document items
  if(name == "layertable") {
    int id = node.attribute("id").as_int(-1);
    if(id < 0) { rd.fail("bad layer id"); return NULL; }
    LayerInfo info(id, node.attribute("name").as_string());
    info.locked = node.attribute("locked").as_bool();
    info.hidden = node.attribute("hidden").as_bool();
    return new LayerTableItem(sd, id, node.attribute("present").as_bool(), info,
        node.attribute("below").as_int(LayerList::BELOW_NONE));
  }
  if(name == "themechanged") {
    ThemeState state;
    PaletteRecipe& recipe = state.recipe;
    recipe.gen = node.attribute("gen").as_string();
    recipe.seedHue = node.attribute("seedhue").as_double(recipe.seedHue);
    recipe.vividness = node.attribute("vividness").as_double(recipe.vividness);
    recipe.depth = node.attribute("depth").as_double(recipe.depth);
    recipe.minContrast = node.attribute("contrast").as_double(recipe.minContrast);
    recipe.jitter = node.attribute("jitter").as_double(recipe.jitter);
    recipe.paperL = node.attribute("paperl").as_double(recipe.paperL);
    recipe.paperWarm = node.attribute("paperwarm").as_double(recipe.paperWarm);
    recipe.families = std::min(std::max(node.attribute("families").as_int(recipe.families), 1), 64);
    state.pageColor = Color::fromArgb(node.attribute("pagecolor").as_uint());
    state.ruleColor = Color::fromArgb(node.attribute("rulecolor").as_uint());
    state.bookmarkColor = Color::fromArgb(node.attribute("bookmarkcolor").as_uint());
    state.linkColor = Color::fromArgb(node.attribute("linkcolor").as_uint());
    return new ThemeChangedItem(sd, state);
  }
  rd.fail("unknown item");
  return NULL;
}

// UndoPersist

// Keyed by path: a document has no id of its own (see the doc for why one was not added).  On iOS the
//  app container's path changes with app updates, so the key is taken relative to HOME there.
std::string UndoPersist::docKey(const std::string& docPath)
{
  // only the folder is canonicalized: the file itself may already be gone (a rename or delete is reported
  //  after the fact), and its key must be the same as while it existed
  FSPath docInfo(docPath);
  std::string folder = canonicalPath(docInfo.parent());
  std::string key = folder.empty() ? docInfo.path : FSPath(folder).childPath(docInfo.fileName());
#if PLATFORM_IOS
  const char* home = getenv("HOME");
  size_t homelen = home ? strlen(home) : 0;
  if(homelen > 0 && key.compare(0, homelen, home) == 0)
    key = key.substr(homelen);
#endif
  return key;
}

std::string UndoPersist::sidecarPath(const std::string& docPath)
{
  if(dir.empty() || docPath.empty())
    return "";
  std::string key = docKey(docPath);
  return FSPath(dir).childPath(fstring("%016llx.undo", (unsigned long long)fnv1a(key.data(), key.size())));
}

// size, mtime and a hash of the first and last 64KB.  The tail is where every save of either format
//  changes something (the bgz index of .svgz, the closing config/thumbnail of .html); size and mtime
//  catch the rest.  Hashing the whole file was ruled out: imported PDFs make documents of tens of MB,
//  and this runs on every save.
std::string UndoPersist::fingerprint(const std::string& docPath)
{
  FileStream file(docPath.c_str(), "rb");
  if(!file.is_open())
    return "";
  size_t size = file.size();
  std::vector<char> buffer(std::min(size, FINGERPRINT_SPAN));
  uint64_t hash = fnv1a(NULL, 0);
  size_t nread = file.read(buffer.data(), buffer.size());
  hash = fnv1a(buffer.data(), nread, hash);
  if(size > FINGERPRINT_SPAN) {
    file.seek(long(size - std::min(size, FINGERPRINT_SPAN)), SEEK_SET);
    nread = file.read(buffer.data(), buffer.size());
    hash = fnv1a(buffer.data(), nread, hash);
  }
  return fstring("%llu %lld %016llx", (unsigned long long)size, (long long)getFileMTime(FSPath(docPath)),
      (unsigned long long)hash);
}

bool UndoPersist::save(ScribbleDoc* sd)
{
  if(!enabled || dir.empty() || !sd->document || !sd->history)
    return false;
  std::string docPath = sd->fileName();
  std::string sidecar = sidecarPath(docPath);
  if(sidecar.empty())
    return false;
  int maxSteps = sd->cfg->Int("undoPersistSteps", 50);
  size_t maxBytes = size_t(std::max(0, sd->cfg->Int("undoPersistMaxKB", 4096))) << 10;
  // A shared session: the peers' items are in this history too, and a session already re-sends the whole
  //  history when it starts, so a persisted copy would make a later session send them back as ours.
  if(sd->scribbleSync || maxSteps <= 0 || maxBytes == 0) {
    removeFile(sidecar);
    return false;
  }

  UndoHistory* history = sd->history;
  std::vector<UndoHistoryItem*>& hist = history->hist;
  size_t histPos = history->pos;
  // pos is always on a group's header (or at the end) outside an action, which a save is
  if(histPos < hist.size() && !hist[histPos]->isA(UndoHistoryItem::HEADER)) {
    removeFile(sidecar);
    return false;
  }
  std::vector<size_t> headers;
  for(size_t ii = 0; ii < hist.size(); ++ii) {
    if(hist[ii]->isA(UndoHistoryItem::HEADER))
      headers.push_back(ii);
  }
  size_t firstRedo = std::lower_bound(headers.begin(), headers.end(), histPos) - headers.begin();

  struct StepRecord { size_t header; std::string text; std::vector<int> defs; };
  UndoPersistWriter writer(sd->document);
  auto writeStep = [&](size_t headerIdx, StepRecord& record) {
    size_t begin = headers[headerIdx];
    size_t end = headerIdx + 1 < headers.size() ? headers[headerIdx + 1] : hist.size();
    writer.beginStep();
    for(size_t ii = begin + 1; ii < end && !writer.failed; ++ii) {
      if(!hist[ii]->isA(UndoHistoryItem::DISABLED_ITEM))
        hist[ii]->persist(writer);
    }
    record.header = begin;
    record.text = writer.endStep();
    record.defs = writer.stepDefs;
    return !writer.failed;
  };

  // Newest first on each side of pos, stopping at the first step that cannot be written: the steps kept
  //  must stay contiguous with pos, because content out of the document is owned by the item nearest pos
  //  that holds it (a delete on the undo side, an undone add on the redo side), and keeping a step without
  //  its owner would leak that content or, worse, free it twice.
  std::vector<StepRecord> undoSteps, redoSteps;
  for(size_t idx = firstRedo; idx-- > 0 && int(undoSteps.size()) < maxSteps;) {
    StepRecord record;
    if(!writeStep(idx, record)) break;
    undoSteps.push_back(record);
  }
  for(size_t idx = firstRedo; idx < headers.size() && int(redoSteps.size()) < maxSteps; ++idx) {
    StepRecord record;
    if(!writeStep(idx, record)) break;
    redoSteps.push_back(record);
  }

  // byte cap: undo steps are worth more than redo steps, and recent ones more than old ones
  std::set<int> counted;
  size_t total = 0;
  auto stepCost = [&](const StepRecord& record) {
    size_t cost = record.text.size() + 32;
    for(int id : record.defs) {
      if(!counted.count(id))
        cost += writer.defs[id].text.size();
    }
    return cost;
  };
  auto admit = [&](const StepRecord& record) {
    total += stepCost(record);
    counted.insert(record.defs.begin(), record.defs.end());
  };
  size_t nUndo = 0, nRedo = 0;
  while(nUndo < undoSteps.size() && total + stepCost(undoSteps[nUndo]) <= maxBytes)
    admit(undoSteps[nUndo++]);
  while(nRedo < redoSteps.size() && total + stepCost(redoSteps[nRedo]) <= maxBytes)
    admit(redoSteps[nRedo++]);

  if(nUndo + nRedo == 0) {
    removeFile(sidecar);
    return true;
  }
  std::string fp = fingerprint(docPath);
  if(fp.empty())
    return false;

  // oldest to newest; each definition goes in the first step that uses it, so a step only ever refers
  //  back, never forward - what lets a journal append steps one at a time later
  std::vector<const StepRecord*> ordered;
  for(size_t ii = nUndo; ii-- > 0;)
    ordered.push_back(&undoSteps[ii]);
  for(size_t ii = 0; ii < nRedo; ++ii)
    ordered.push_back(&redoSteps[ii]);

  std::ostringstream out;
  pugi::xml_document headerDoc;
  pugi::xml_node header = headerDoc.append_child("undohistory");
  header.append_attribute("version") = FORMAT_VERSION;
  header.append_attribute("doc") = docKey(docPath).c_str();
  header.append_attribute("fingerprint") = fp.c_str();
  header.append_attribute("pages") = int(sd->document->numPages());
  header.append_attribute("undo") = int(nUndo);
  header.append_attribute("redo") = int(nRedo);
  pugi::xml_node verifyNode = headerDoc.append_child("verify");
  for(auto& entry : writer.verify) {
    pugi::xml_node vnode = verifyNode.append_child("v");
    vnode.append_attribute("r") = entry.first.c_str();
    vnode.append_attribute("sig") = entry.second.c_str();
  }
  for(pugi::xml_node node : headerDoc.children()) {
    node.print(out, "", pugi::format_raw);
    out << "\n";
  }
  std::set<int> emitted;
  for(const StepRecord* record : ordered) {
    out << fstring("<step page='%d'>", static_cast<UndoGroupHeader*>(hist[record->header])->pageNum);
    for(int id : record->defs) {
      if(emitted.insert(id).second)
        out << writer.defs[id].text;
    }
    out << record->text << "</step>\n";
  }

  // temp file then rename, so a crash mid-write leaves the old sidecar or none, never half of one
  if(!createPath(FSPath(dir)))
    return false;
  std::string tempPath = sidecar + ".tmp";
  std::string text = out.str();
  {
    FileStream tempFile(tempPath.c_str(), "wb");
    if(!tempFile.is_open())
      return false;
    if(tempFile.write(text.data(), text.size()) != text.size() || !tempFile.flush()) {
      tempFile.close();
      removeFile(tempPath);
      return false;
    }
  }
#if PLATFORM_WIN
  removeFile(sidecar);  // rename() does not replace on Windows
#endif
  bool ok = moveFile(FSPath(tempPath), FSPath(sidecar));
  if(!ok)
    removeFile(tempPath);
  cleanup();
  return ok;
}

int UndoPersist::restore(ScribbleDoc* sd)
{
  if(!enabled || dir.empty() || !sd->document || !sd->history || sd->scribbleSync)
    return 0;
  std::string docPath = sd->fileName();
  std::string sidecar = sidecarPath(docPath);
  if(sidecar.empty() || !FSPath(sidecar).exists())
    return 0;
  UndoHistory* history = sd->history;
  if(!history->hist.empty())
    return 0;
  Document* document = sd->document;

  std::string text;
  {
    FileStream file(sidecar.c_str(), "rb");
    if(!file.is_open())
      return 0;
    text.resize(file.size());
    text.resize(file.read(&text[0], text.size()));
  }
  pugi::xml_document xml;
  bool parsed = xml.load_buffer(text.data(), text.size(), pugi::parse_default | pugi::parse_fragment);
  pugi::xml_node header = xml.child("undohistory");
  if(!parsed || !header || header.attribute("version").as_int() != FORMAT_VERSION
      || header.attribute("fingerprint").as_string() != fingerprint(docPath)
      || header.attribute("pages").as_int() != document->numPages()) {
    // the file was changed by something else (another device, an older build, a copy), or this is not
    //  the history it was saved with: none of it can be trusted to apply
    PLATFORM_LOG("Saved undo history for %s does not match the file; discarded\n", docPath.c_str());
    removeFile(sidecar);
    return 0;
  }

  UndoPersistReader reader(document);
  for(pugi::xml_node vnode : xml.child("verify").children("v"))
    reader.verify[vnode.attribute("r").as_string()] = vnode.attribute("sig").as_string();

  std::vector<UndoHistoryItem*> items;
  int nSteps = 0;
  for(pugi::xml_node step : xml.children("step")) {
    if(reader.failed) break;
    items.push_back(new UndoGroupHeader(step.attribute("page").as_int()));
    ++nSteps;
    for(pugi::xml_node node : step.children()) {
      std::string name = node.name();
      if(name == "edef") {
        XmlStreamReader xmlreader(node);
        std::unique_ptr<SvgDocument> svgDoc(SvgParser().parseXmlFragment(&xmlreader));
        if(!svgDoc || svgDoc->children().empty()) { reader.fail("bad element definition"); break; }
        SvgNode* svgnode = svgDoc->children().front();
        svgDoc->removeChild(svgnode);
        reader.elementDefs[node.attribute("id").as_int()] = new Element(svgnode);
      }
      else if(name == "pdef") {
        Page* page = new Page();
        page->document = document;
        XmlStreamReader xmlreader(node.first_child());
        SvgDocument* pagesvg = SvgParser().parseXml(&xmlreader);
        if(!pagesvg || !page->loadSVG(pagesvg)) {
          delete page;
          reader.fail("bad page definition");
          break;
        }
        page->loadStatus = Page::LOAD_OK;
        reader.pageDefs[node.attribute("id").as_int()] = page;
      }
      else {
        UndoHistoryItem* item = readItem(node, sd, reader);
        if(!item) { reader.fail("unreadable item"); break; }
        items.push_back(item);
      }
    }
  }
  int nUndo = header.attribute("undo").as_int(), nRedo = header.attribute("redo").as_int();
  if(!reader.failed && nSteps != nUndo + nRedo)
    reader.fail("step count does not match");
  if(reader.failed) {
    for(UndoHistoryItem* item : items)
      delete item;
    reader.freeDefinitions();
    removeFile(sidecar);
    return 0;
  }

  history->hist = std::move(items);
  history->pos = history->hist.size();
  // pos goes to the header of the first redo step
  int headersSeen = 0;
  for(size_t ii = 0; ii < history->hist.size(); ++ii) {
    if(history->hist[ii]->isA(UndoHistoryItem::HEADER) && headersSeen++ == nUndo) {
      history->pos = ii;
      break;
    }
  }
  return nSteps;
}

void UndoPersist::documentMoved(const std::string& from, const std::string& to)
{
  // the target's own history (if any) is for a file that no longer exists there
  std::string src = sidecarPath(from), dest = sidecarPath(to);
  if(src.empty() || dest.empty() || src == dest)
    return;
  removeFile(dest);
  if(FSPath(src).exists())
    moveFile(FSPath(src), FSPath(dest));
}

void UndoPersist::documentDeleted(const std::string& path)
{
  std::string sidecar = sidecarPath(path);
  if(!sidecar.empty())
    removeFile(sidecar);
}

// at most once a run: drop sidecars not written for MAX_AGE_SECS (their documents are deleted, moved
//  outside the app, or simply not edited - all cases where the history has stopped being worth its space),
//  then the oldest until the folder is under MAX_TOTAL_BYTES
void UndoPersist::cleanup()
{
  static bool cleaned = false;
  if(cleaned || dir.empty())
    return;
  cleaned = true;
  Timestamp now = mSecSinceEpoch()/1000;
  struct SidecarFile { std::string path; Timestamp mtime; long size; };
  std::vector<SidecarFile> files;
  long total = 0;
  for(const std::string& name : lsDirectory(FSPath(dir))) {
    FSPath file = FSPath(dir).child(name);
    if(file.extension() != "undo" && file.extension() != "tmp")
      continue;
    Timestamp mtime = getFileMTime(file);
    if(now - mtime > MAX_AGE_SECS || file.extension() == "tmp") {
      removeFile(file.path);
      continue;
    }
    long size = getFileSize(file);
    files.push_back({file.path, mtime, size});
    total += size;
  }
  std::sort(files.begin(), files.end(), [](const SidecarFile& lhs, const SidecarFile& rhs) { return lhs.mtime < rhs.mtime; });
  for(size_t ii = 0; ii < files.size() && total > MAX_TOTAL_BYTES; ++ii) {
    removeFile(files[ii].path);
    total -= files[ii].size;
  }
}
