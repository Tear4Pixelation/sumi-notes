#include "syncundo.h"
#include "document.h"
#include "basics.h"
#include "usvg/svgxml.h"
#include "scribbledoc.h"

StrokeUndoItem::StrokeUndoItem(Element* s_, Page* p_) : s(s_), page(p_) {}

void StrokeUndoItem::commit()
{
  page->dirtyCount++;  page->revision++;
}

void StrokeUndoItem::undo()
{
  page->dirtyCount--;  page->revision++;
}

void StrokeUndoItem::redo()
{
  page->dirtyCount++;  page->revision++;
}

// if user saves doc, does undo, then makes a change discarding the history which includes saved state, it is
//  impossible to return to saved state, so page is irrevocably dirtied so we set dirtyCount to extreme value
void StrokeUndoItem::discard(bool undone)
{
  if(undone && page->dirtyCount < 0)  // if !undone, page may not exist
    page->dirtyCount = SAVED_STATE_DISCARDED;
}

// StrokeAddedItem
void StrokeAddedItem::undo()
{
  StrokeUndoItem::undo();
  page->onRemoveStroke(s);
  page->contentNode->removeChild(s->node);
}

// A stroke put back into a page takes its layer's visibility: the layer may have been hidden while the
//  stroke was out of the page (undo of a delete), or on this client only (a peer drawing on a layer we
//  have hidden).  Visibility is otherwise applied only when the table changes or a page loads.
static void applyLayerVisibility(Element* s, Page* page)
{
  if(page->document)
    s->node->setDisplayMode(page->document->layers.isHidden(s->layer()) ?
        SvgNode::NoneMode : SvgNode::BlockMode);
}

// Where a stroke goes back into its page.  Normally the recorded sibling; but a ruling region must end
//  up below all ink, and its recorded sibling is not enough to guarantee that: a NULL `next` (it was
//  the last element) means "on top", and under sync a peer's ink can have arrived since, or the sibling
//  can have been deleted by a peer, which also appends.  Either would put an opaque region over ink.
static SvgNode* reinsertPos(Element* s, Page* page, Element* next)
{
  if(s->isRulingRegion() && !(next && next->isRulingRegion() && next->node->parent() == page->contentNode)) {
    Element* pos = page->layerInsertPos(LayerList::REGION_LAYER);
    return pos ? pos->node : NULL;
  }
  return next ? next->node : NULL;
}

void StrokeAddedItem::redo()
{
  applyLayerVisibility(s, page);
  page->contentNode->addChild(s->node, reinsertPos(s, page, next));
  page->onAddStroke(s);
  StrokeUndoItem::redo();
}

void StrokeAddedItem::discard(bool undone)
{
  StrokeUndoItem::discard(undone);
  if(undone)
    s->deleteNode();
}

void StrokeDeletedItem::undo()
{
  applyLayerVisibility(s, page);
  page->contentNode->addChild(s->node, reinsertPos(s, page, next));
  page->onAddStroke(s);
  StrokeUndoItem::undo();
}

void StrokeDeletedItem::redo()
{
  StrokeUndoItem::redo();
  page->onRemoveStroke(s);
  page->contentNode->removeChild(s->node);
}

void StrokeDeletedItem::discard(bool undone)
{
  StrokeUndoItem::discard(undone);
  if(!undone)
    s->deleteNode();
}

// StrokeChangedItem - record change of stroke properties (color, width)
void StrokeChangedItem::swapProps()
{
  StrokeProperties temp = s->getProperties();
  s->setProperties(props);
  props = temp;
}

void StrokeChangedItem::undo()
{
  swapProps();
  StrokeUndoItem::undo();
}

void StrokeChangedItem::redo()
{
  swapProps();
  StrokeUndoItem::redo();
}

// ShapeChangedItem - record change of a shape descriptor (points, corner radii, flags)
void ShapeChangedItem::swapParams()
{
  ShapeParams temp = s->shapeParams();
  s->setShapeParams(params);
  params = temp;
}

void ShapeChangedItem::undo()
{
  swapParams();
  StrokeUndoItem::undo();
}

void ShapeChangedItem::redo()
{
  swapParams();
  StrokeUndoItem::redo();
}

// RegionChangedItem - record change of a ruling region's parameters
void RegionChangedItem::swapParams()
{
  RulingRegionParams temp = s->regionParams();
  s->setRegionParams(params);
  s->rebuildRegion(page->props.color, page->props.ruleColor);
  params = temp;
}

void RegionChangedItem::undo()
{
  swapParams();
  StrokeUndoItem::undo();
}

void RegionChangedItem::redo()
{
  swapParams();
  StrokeUndoItem::redo();
}

// StrokeLayerItem - move an element to another layer, restacking it to that layer's run
void StrokeLayerItem::swapLayer()
{
  int prevlayer = s->layer();
  // moveToLayer() hands back the successor the element had, so undo puts it back exactly where it
  //  was rather than merely on the right layer
  Element* prevnext = page->moveToLayer(s, layer, next);
  layer = prevlayer;
  next = prevnext;
}

void StrokeLayerItem::undo()
{
  swapLayer();
  StrokeUndoItem::undo();
}

void StrokeLayerItem::redo()
{
  swapLayer();
  StrokeUndoItem::redo();
}

void StrokeLayerItem::serialize(IOStream& strm)
{
  strm << fstring("<layerchanged strokeuuid='%llu' layer='%d' nextstrokeuuid='%llu'/>",
      s->uuid, s->layer(), next ? next->uuid : 0);
}

UndoHistoryItem* StrokeLayerItem::inverse()
{
  return new StrokeLayerItem(*this);
}

// StrokeTransformItem is passed Stroke prior to commit; used to efficiently save undo info from reflow
//StrokeTranslateItem::StrokeTranslateItem(Element* s_)
//    : StrokeUndoItem(s_), xoffset(s_->xOffset()), yoffset(s_->yOffset()) {}

StrokeTranslateItem::StrokeTranslateItem(Element* s_, Page* p_, Dim xoffset_, Dim yoffset_)
    : StrokeUndoItem(s_, p_), xoffset(xoffset_), yoffset(yoffset_) {}

void StrokeTranslateItem::undo()
{
  s->applyTransform(Transform2D::translating(-xoffset, -yoffset));
  s->commitTransform();
  StrokeUndoItem::undo();
}

void StrokeTranslateItem::redo()
{
  s->applyTransform(Transform2D::translating(xoffset, yoffset));
  s->commitTransform();
  StrokeUndoItem::redo();
}

// StrokeTransformItem

void StrokeTransformItem::undo()
{
  s->applyTransform(transform.inverse());
  s->commitTransform();
  StrokeUndoItem::undo();
}

void StrokeTransformItem::redo()
{
  s->applyTransform(transform);
  s->commitTransform();
  StrokeUndoItem::redo();
}

// Page level items

PageChangedItem::PageChangedItem(Page* p_) : p(p_), props(p_->getProperties()) {}

void PageChangedItem::discard(bool undone) {}

void PageChangedItem::commit()
{
  p->dirtyCount++;  p->revision++;
}

void PageChangedItem::swapProps()
{
  // save current properties
  PageProperties temp = p->getProperties();
  // replace current properties with new properties
  p->setProperties(&props);
  // now store replaced properties (except ruleLayer, if it is unchanged)
  props = temp;
}

void PageChangedItem::undo()
{
  swapProps();
  p->dirtyCount--;  p->revision++;
}

void PageChangedItem::redo()
{
  swapProps();
  p->dirtyCount++;  p->revision++;
}

void PageOutlineItem::discard(bool undone) {}

void PageOutlineItem::commit()
{
  p->dirtyCount++;  p->revision++;
}

void PageOutlineItem::swapOutline()
{
  std::string prevtitle = p->outlineTitle;
  int prevlevel = p->outlineLevel;
  p->setOutlineEntry(title.c_str(), level);
  title = prevtitle;
  level = prevlevel;
}

void PageOutlineItem::undo()
{
  swapOutline();
  p->dirtyCount--;  p->revision++;
}

void PageOutlineItem::redo()
{
  swapOutline();
  p->dirtyCount++;  p->revision++;
}

DocumentUndoItem::DocumentUndoItem(Page* p_, int pagenum_, Document* document_)
    : p(p_), pagenum(pagenum_), document(document_) {}

void DocumentUndoItem::commit()
{
  document->dirtyCount++;
}

void DocumentUndoItem::undo()
{
  document->dirtyCount--;
}

void DocumentUndoItem::redo()
{
  document->dirtyCount++;
}

void DocumentUndoItem::discard(bool undone)
{
  if(undone && document->dirtyCount < 0)
    document->dirtyCount = SAVED_STATE_DISCARDED;
}

void PageDeletedItem::undo()
{
  document->insertPage(p, pagenum);
  DocumentUndoItem::undo();
}

void PageDeletedItem::redo()
{
  DocumentUndoItem::redo();
  document->deletePage(pagenum);
}

void PageDeletedItem::discard(bool undone)
{
  DocumentUndoItem::discard(undone);
  if(!undone)
    delete p;
}

void PageAddedItem::undo()
{
  DocumentUndoItem::undo();
  document->deletePage(pagenum);
}

void PageAddedItem::redo()
{
  document->insertPage(p, pagenum);
  DocumentUndoItem::redo();
}

void PageAddedItem::discard(bool undone)
{
  DocumentUndoItem::discard(undone);
  if(undone)
    delete p;
}

// UndoHistory class

UndoHistory::UndoHistory() : pos(0), inAction(0) {}

UndoHistory::~UndoHistory()
{
  // clear the strokes that are actually undone, then the rest
  clearUndone();
  pos = 0;
  clearUndone(false);
}

void UndoHistory::startAction(int pagenum)
{
  if(inAction)
    SCRIBBLE_LOG("UndoHistory::startAction called while already in action!");
  inAction = 1;
  actionPageNum = pagenum;
}

void UndoHistory::endAction()
{
  inAction = 0;
}

void UndoHistory::addItem(UndoHistoryItem* undoItem)
{
  if(inAction) {
    if(inAction == 1) {
      if(pos < hist.size())
        clearUndone();
      hist.push_back(new UndoGroupHeader(actionPageNum));
    }
    undoItem->commit();
    hist.push_back(undoItem);
    pos = hist.size();
    inAction++;
  }
  else {
    undoItem->discard(false);
    delete undoItem;
    // not necessarily a bug, but I want to understand when this can happen
    SCRIBBLE_LOG("Undo item outside of Action\n");
  }
}

void UndoHistory::clearUndone(bool undone)
{
  // must discard newer entries before older ones
  while(hist.size() > pos) {  //unsigned int ii = pos; ii < hist.size(); ii++) {
    hist.back()->discard(undone);
    if(!hist.back()->isA(UndoHistoryItem::DISABLED_ITEM))
      delete hist.back();
    hist.pop_back();
  }
}

int UndoHistory::undo()
{
  while(pos > 0) {
    pos--;
    if(hist[pos]->isA(UndoHistoryItem::HEADER))
      break;
    hist[pos]->undo();
  }
  return ((UndoGroupHeader*) hist[pos])->pageNum;
}

int UndoHistory::redo()
{
  int pagenum = -1;
  // skip header
  if(pos < hist.size())
    pagenum = ((UndoGroupHeader*) hist[pos++])->pageNum;
  while(pos < hist.size() && !hist[pos]->isA(UndoHistoryItem::HEADER))
    hist[pos++]->redo();
  return pagenum;
}

bool UndoHistory::canUndo() const
{
  return !hist.empty() && pos > 0;
}

bool UndoHistory::canRedo() const
{
  return pos < hist.size();
}

// undo() stops at a header and leaves pos pointing at it, so every group is [header, items...) and the
//  number of steps in either direction is just the number of headers on that side of pos
size_t UndoHistory::undoSteps() const
{
  size_t n = 0;
  for(size_t ii = 0; ii < pos && ii < hist.size(); ++ii) {
    if(hist[ii]->isA(UndoHistoryItem::HEADER))
      ++n;
  }
  return n;
}

size_t UndoHistory::redoSteps() const
{
  size_t n = 0;
  for(size_t ii = pos; ii < hist.size(); ++ii) {
    if(hist[ii]->isA(UndoHistoryItem::HEADER))
      ++n;
  }
  return n;
}

// Note that this is provided for optimization purposes - items added to history when inAction == 0
//  are discarded.  Checking this flag allows us to avoid unnecessary creation of such items.
// We expect inAction to be 0 when in the process of undoing or redoing

bool UndoHistory::undoable() const
{
  return (inAction != 0);
}

// serialization and inversion - needed for shared whiteboarding

// for now, we are going to assign uuid to undo item; in the future, server may do this
// RNG is seeded with time in MainWindow ctor - this assumes we're on the same thread!
UUID_t UndoHistory::newUuid()
{
  return (UUID_t(randpp()) << 32) + UUID_t(randpp());
}

void StrokeAddedItem::serialize(IOStream& strm)
{
  s->uuid = UndoHistory::newUuid();
  strm << fstring("<addstroke strokeuuid='%llu' pagenum='%d' nextstrokeuuid='%llu'>",
     s->uuid, page->getPageNum(), next ? next->uuid : 0);
  XmlStreamWriter xmlwriter;
#if IS_DEBUG
  xmlwriter.defaultFloatPrecision = 6;  // send extra digits to eliminate test diffs for transformed strokes
#endif
  SvgWriter(xmlwriter).serialize(s->node);
  xmlwriter.save(strm);
  strm << "</addstroke>";
}

void StrokeDeletedItem::serialize(IOStream& strm)
{
  strm << fstring("<delstroke strokeuuid='%llu'/>", s->uuid);
}

void StrokeTransformItem::serialize(IOStream& strm)
{
  Dim* m = transform.asArray();
  strm << fstring("<transform strokeuuid='%llu' internalscale='%f %f'",
      s->uuid, transform.internalScale[0], transform.internalScale[1]);
  // maintain legacy 3x3 format for now
  strm << fstring(" matrix='%f %f %f %f %f %f %f %f %f'/>", m[0], m[1], 0.0, m[2], m[3], 0.0, m[4], m[5], 1.0);
}

void StrokeTranslateItem::serialize(IOStream& strm)
{
  strm << fstring("<translate strokeuuid='%llu' x='%f' y='%f'/>", s->uuid, xoffset, yoffset);
}

void StrokeChangedItem::serialize(IOStream& strm)
{
  StrokeProperties currprops = s->getProperties();
  // dash is numbers or "none", so fstring is safe; a peer that predates it ignores the attribute
  strm << fstring("<strokechanged strokeuuid='%llu' color='%u' width='%f' dash='%s'/>",
      s->uuid, currprops.color.argb(), currprops.width, currprops.dashArray.c_str());
}

// spec 7.5: undo items are the sync protocol, so a new item type needs a wire format of its own; the
//  __shape* attributes themselves ride along free inside <addstroke>, but this does not
void ShapeChangedItem::serialize(IOStream& strm)
{
  const ShapeParams& curr = s->shapeParams();
  const ShapeDef* def = shapeDef(curr.id);
  strm << fstring("<shapechanged strokeuuid='%llu' shape='%s' shapepts='%s' rx='%f' ry='%f'"
      " tight='%f' flags='%d'/>", s->uuid, def ? def->id : "",
      serializeShapePoints(curr.points).c_str(), curr.rx, curr.ry, curr.tightness, curr.flags);
}

void RegionChangedItem::serialize(IOStream& strm)
{
  // numbers only, so fstring is safe (no user string goes on the wire here)
  const RulingRegionParams& curr = s->regionParams();
  strm << fstring("<regionchanged strokeuuid='%llu' pts='%s' origin='%s' angle='%.17g' xruling='%.9g'"
      " yruling='%.9g' dotradius='%.9g' opaque='%d' outline='%d' staff='%d'/>", s->uuid,
      serializeRegionPoints(curr.corners).c_str(), serializeRegionPoints({curr.origin}).c_str(), double(curr.angle),
      double(curr.xRuling), double(curr.yRuling), double(curr.dotRadius), curr.opaque ? 1 : 0, curr.outline ? 1 : 0,
      curr.staff ? 1 : 0);
}

void PageChangedItem::serialize(IOStream& strm)
{
  // lots of parameters - just use fstring
  strm << fstring("<pagechanged pagenum='%d' width='%.3f' height='%.3f' color='%u' xruling='%.3f'"
      " yruling='%.3f' marginLeft='%.3f' rulecolor='%u' dotradius='%.3f' staff='%d'>", p->getPageNum(), p->props.width,
      p->props.height, p->props.color.argb(), p->props.xRuling, p->props.yRuling, p->props.marginLeft,
      p->props.ruleColor.argb(), p->props.dotRadius, p->props.staff ? 1 : 0);
  //if(props.ruleLayer)
  //  props.ruleLayer->saveSVG(strm);
  strm << "</pagechanged>";
}

// The title is user text and is the first arbitrary string to go on the sync wire, so it is written
//  through XmlStreamWriter (i.e. pugixml) rather than fstring - a title containing & or ' would
//  otherwise produce a malformed item and desync the stream.  pugixml unescapes on the way back in.
void PageOutlineItem::serialize(IOStream& strm)
{
  XmlStreamWriter xmlwriter;
  xmlwriter.writeStartElement("outlinechanged");
  xmlwriter.writeAttribute("pagenum", p->getPageNum());
  xmlwriter.writeAttribute("level", p->outlineLevel);
  xmlwriter.writeAttribute("title", p->outlineTitle);
  xmlwriter.writeEndElement();
  xmlwriter.save(strm);
}

void PageDeletedItem::serialize(IOStream& strm)
{
  strm << fstring("<delpage pagenum='%d'/>", pagenum);
}

void PageAddedItem::serialize(IOStream& strm)
{
  UUID_t firstuuid = p->strokeCount() > 0 ? UndoHistory::newUuid() : 0;
  strm << fstring("<addpage pagenum='%d' keepcontents='1' firstuuid='%llu'>", pagenum, firstuuid);
  for(Element* s : p->children())
    s->uuid = firstuuid++;
  p->saveSVG(strm, 0, 0);  //, true);
  strm << "</addpage>";
}

// to sync local undo, we need to be able to get the inverse of an item, which we call serialize() for
UndoHistoryItem* StrokeAddedItem::inverse()
{
  return new StrokeDeletedItem(s, page, next);
}

UndoHistoryItem* StrokeDeletedItem::inverse()
{
  return new StrokeAddedItem(s, page, next);
}

UndoHistoryItem* StrokeTranslateItem::inverse()
{
  return new StrokeTranslateItem(s, page, -xoffset, -yoffset);
}

UndoHistoryItem* StrokeTransformItem::inverse()
{
  return new StrokeTransformItem(s, page, transform.inverse());
}

UndoHistoryItem* PageAddedItem::inverse()
{
  return new PageDeletedItem(p, pagenum, document);
}

UndoHistoryItem* PageDeletedItem::inverse()
{
  return new PageAddedItem(p, pagenum, document);
}

// pretty ugly hack - we're assuming that serialize() is only called (directly) when item has not been undone,
//  while inverse() is only called when item has been undone.
UndoHistoryItem* StrokeChangedItem::inverse()
{
  return new StrokeChangedItem(*this);
}

UndoHistoryItem* RegionChangedItem::inverse()
{
  return new RegionChangedItem(*this);
}

UndoHistoryItem* ShapeChangedItem::inverse()
{
  return new ShapeChangedItem(*this);
}

UndoHistoryItem* PageChangedItem::inverse()
{
  return new PageChangedItem(*this);
}

UndoHistoryItem* PageOutlineItem::inverse()
{
  return new PageOutlineItem(*this);
}


// LayerTableItem

LayerTableItem::LayerTableItem(ScribbleDoc* sd_, int id_) : sd(sd_), id(id_), belowId(LayerList::BELOW_NONE)
{
  present = sd->document->layers.layerState(id, &info, &belowId);
}

// the table is not in any page, so the document itself is what an edit to it dirties
void LayerTableItem::commit()
{
  sd->document->dirtyCount++;
}

void LayerTableItem::swapState()
{
  LayerInfo live;
  int liveBelow = LayerList::BELOW_NONE;
  bool livePresent = sd->document->layers.layerState(id, &live, &liveBelow);
  sd->setLayerState(id, present ? &info : NULL, belowId);
  present = livePresent;
  info = live;
  belowId = liveBelow;
}

void LayerTableItem::undo()
{
  swapState();
  sd->document->dirtyCount--;
}

void LayerTableItem::redo()
{
  swapState();
  sd->document->dirtyCount++;
}

// A layer name is user text, so this goes through XmlStreamWriter for the same reason PageOutlineItem
//  does: a name containing & or ' written with fstring would desync the stream.
void LayerTableItem::serialize(IOStream& strm)
{
  LayerInfo live;
  int liveBelow = LayerList::BELOW_NONE;
  bool livePresent = sd->document->layers.layerState(id, &live, &liveBelow);
  XmlStreamWriter xmlwriter;
  xmlwriter.writeStartElement("layertable");
  xmlwriter.writeAttribute("id", id);
  xmlwriter.writeAttribute("present", livePresent ? 1 : 0);
  if(livePresent) {
    xmlwriter.writeAttribute("below", liveBelow);
    xmlwriter.writeAttribute("locked", live.locked ? 1 : 0);
    xmlwriter.writeAttribute("hidden", live.hidden ? 1 : 0);
    xmlwriter.writeAttribute("name", live.name);
  }
  xmlwriter.writeEndElement();
  xmlwriter.save(strm);
}

// serialize() reports the live state, so the inverse of an undone item is just the item
UndoHistoryItem* LayerTableItem::inverse()
{
  return new LayerTableItem(*this);
}

// ThemeChangedItem - COLORS_SPEC.md 8.  Also what makes undoing a restyle put the recipe back, not
//  just the stroke colors.

ThemeChangedItem::ThemeChangedItem(ScribbleDoc* sd_) : sd(sd_), state(sd_->themeState()) {}

void ThemeChangedItem::commit()
{
  sd->document->dirtyCount++;
}

void ThemeChangedItem::swapState()
{
  ThemeState live = sd->themeState();
  sd->applyThemeState(state);
  state = live;
}

void ThemeChangedItem::undo()
{
  swapState();
  sd->document->dirtyCount--;
}

void ThemeChangedItem::redo()
{
  swapState();
  sd->document->dirtyCount++;
}

void ThemeChangedItem::serialize(IOStream& strm)
{
  ThemeState live = sd->themeState();
  const PaletteRecipe& r = live.recipe;
  XmlStreamWriter xmlwriter;
  xmlwriter.writeStartElement("themechanged");
  // the generator id is a string, and an unknown one must survive the trip (COLORS_SPEC.md 3)
  xmlwriter.writeAttribute("gen", r.gen);
  // full float precision: the recipe is stored as float, and restyle recognises ink only by exact
  //  color, so a recipe rounded on the wire would generate a palette a peer's ink is not drawn in
  xmlwriter.writeAttribute("seedhue", r.seedHue, 9);
  xmlwriter.writeAttribute("vividness", r.vividness, 9);
  xmlwriter.writeAttribute("depth", r.depth, 9);
  xmlwriter.writeAttribute("contrast", r.minContrast, 9);
  xmlwriter.writeAttribute("jitter", r.jitter, 9);
  xmlwriter.writeAttribute("paperl", r.paperL, 9);
  xmlwriter.writeAttribute("paperwarm", r.paperWarm, 9);
  xmlwriter.writeAttribute("families", r.families);
  // colors as unsigned: ARGB with alpha set does not fit an int
  xmlwriter.writeAttribute("pagecolor", fstring("%u", live.pageColor.argb()));
  xmlwriter.writeAttribute("rulecolor", fstring("%u", live.ruleColor.argb()));
  xmlwriter.writeAttribute("bookmarkcolor", fstring("%u", live.bookmarkColor.argb()));
  xmlwriter.writeAttribute("linkcolor", fstring("%u", live.linkColor.argb()));
  xmlwriter.writeEndElement();
  xmlwriter.save(strm);
}

UndoHistoryItem* ThemeChangedItem::inverse()
{
  return new ThemeChangedItem(*this);
}
