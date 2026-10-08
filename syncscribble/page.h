#pragma once

#include <vector>
#include <string>
#include <functional>
#include "ulib/fileutil.h"
#include "element.h"

class PageProperties {
public:
  Dim width;
  Dim height;
  Color color;  // paper color
  Dim xRuling;
  Dim yRuling;
  Dim marginLeft;
  Color ruleColor;
  // > 0 draws the ruling as dots of this radius instead of lines: at the grid intersections when both
  //  xRuling and yRuling are set, otherwise as dotted lines along the one ruling that is
  Dim dotRadius;

  PageProperties(Dim w=0, Dim h=0, Dim xr=0, Dim yr=0, Dim ml=0, Color c=Color::WHITE, Color rc=Color::BLUE,
      Dim dr=0);
};

// 1 page = 1 <svg> node
class Document;

class Page {
public:
  PageProperties props;
  std::unique_ptr<SvgDocument> svgDoc;
  SvgContainerNode* contentNode = NULL;
  SvgContainerNode* ruleNode = NULL;
  bool isCustomRuling = false;
  Timestamp minTimestamp = MAX_TIMESTAMP;
  Timestamp maxTimestamp = 0;
  Document* document = NULL;  // parent document
  std::list<Element*> bookmarks;
  Dim maxBookmarkWidth = 0;
  int numBookmarks = -1;
  // Outline (table of contents) entry for this page; an empty title means the page has no entry.
  // These are a cache of the __outline/__outlinelevel attributes on contentNode - the page's own SVG
  // is the storage, deliberately, rather than a list of {page number, title} on Document.  A
  // document-level list keyed by page number would have to be fixed up by insertPage/deletePage,
  // by undo of either, and by sync, and would silently point at the wrong page whenever one of
  // those was missed.  Living on the page, an entry moves with its page for free.
  std::string outlineTitle;
  int outlineLevel = 0;
  // Page tags (docs/agent/page-tags.md).  The write-pagetag elements are the storage; pageTagIds is a
  //  cache of their ids, kept by onAddStroke/onRemoveStroke, so the document's summary can be written
  //  on save without loading every page.  A page not loaded yet has whatever the summary said when the
  //  document was opened, along with the outline title (unknown until the page loads) and thumbnail.
  std::vector<std::string> pageTagIds;
  std::string pageTagTitle;
  std::string pageTagThumb;  // base64 PNG; empty means it must be rendered on the next save
  void refreshPageTags(const Element* removing = NULL);
  enum loadstatus_t {LOAD_SVG_ERROR=-1, NOT_LOADED=0, LOAD_OK=1} loadStatus = NOT_LOADED;
  // dirtyCount is managed by undo system; page needs to be written out if != 0
  int dirtyCount = 0;
  // Bumped by every change the undo system records - undo and redo included - and never reset.  Unlike
  //  dirtyCount, which a save zeroes and an undo counts back down, it tells two versions of a page apart,
  //  so the sidebar's page thumbnails (docs/agent/page-management.md) key their cache on it.
  unsigned int revision = 0;
  // Unique for the life of the process, never reused - unlike the Page*, whose address a page allocated
  //  after this one is freed can take.  The sidebar's page thumbnails and selection are keyed by it.
  unsigned int uid = nextUid();
  static unsigned int nextUid() { static unsigned int lastUid = 0; return ++lastUid; }
  //int autoSavedDirtyCount = NOT_AUTO_SAVED;  // dirtyCount value of last autosave
  int blockIdx = -1;
  std::string fileName;
  std::string autoSaveFileName;

  Dim yRuleOffset = 0;
  // this is a hack for drawing clippings, but in the new SVG model, pages can have an arbitrary 2D
  //  transform, so this is really just a special case of that
  Dim scaleFactor = 1;
  bool isSelected = false;

  Page(Dim w=0, Dim h=0, int idx = -1);
  Page(const PageProperties& _props, const SvgContainerNode* ruling = NULL);
  void initDoc();
  PageProperties getProperties();
  bool setProperties(const PageProperties* props);
  //void changeSpacing(Dim newxruling, Dim newyruling, Dim newmarginLeft);
  void onPageSizeChange();
  void generateRuleLayer(Color pageColor, Dim w, Dim h);
  int getPageNum() const;
  int getLine(Dim y) const;
  int getLine(const Element* s) const;
  std::function<bool(const Element* a, const Element* b)> cmpRuled();
  Dim getYforLine(int line) const;
  Dim height() const;
  Dim width() const;
  Dim yruling(bool usedefault = false) const;
  // The ruling in effect at `pos`: the topmost ruling region containing it, else the page's own.  This
  //  is what every ruled tool should ask, with the gesture's anchor point (usually where it was pressed),
  //  rather than yruling()/getLine(y), which only know the page's ruling.
  RulingFrame rulingAt(Point pos) const;
  // line height relative widths of strokes with these centres are measured in (selectionLineHeight())
  Dim lineHeightFor(const std::vector<Point>& centres) const;
  RulingFrame pageFrame() const;
  // the topmost region containing `pos`, or NULL
  Element* regionAt(Point pos) const;
  std::vector<Element*> regions() const;
  // the area a frame's ruled tools may act over, in the frame's local coordinates: the page for the
  //  page's own ruling, the region's bounding box for a region's
  Rect frameExtent(const RulingFrame& frame) const;
  // the frame a ruled gesture starting at `pos` works in: rulingAt(pos), and on an unruled page (or
  //  region) the blank line height phased so `pos` sits in the middle of a line - what yRuleOffset
  //  does for the page's own ruling
  //  `nearLines` > 0 lets a press that misses every region still take the frame of one whose outline is
  //  within that many of its own line heights (regionNear)
  RulingFrame gestureFrame(Point pos, Dim nearLines = 0) const;
  // the topmost region whose outline, grown by `lines` of its own line height, contains `pos`
  Element* regionNear(Point pos, Dim lines) const;
  RulingFrame regionFrame(Element* region) const;
  // re-color every region from the page's paper and rule colors (theme change, invert, load)
  void refreshRegions();
  Dim xruling() const { return props.xRuling; }
  Dim marginLeft() const { return props.marginLeft; }
  Rect rect() const { return Rect::ltwh(0, 0, width(), height()); }
  Color color() const { return props.color; }
  void draw(Painter* painter, const Rect& dirty, bool rulelines = true);

  Rect getDirty() const { return SvgPainter::calcDirtyRect(svgDoc.get()); }
  void clearDirty() { SvgPainter::clearDirty(svgDoc.get()); }
  int strokeCount() const { return contentNode ? contentNode->children().size() : 0; }
  Rect getBBox() const { return contentNode->bounds(); }
  void recalcTimeRange(bool force = false);

  Range<ElementIter> children() const;
  // `layer` defaults to LAYER_CURRENT, i.e. the element is stamped with the document's current
  //  layer.  Every route by which the *user* adds content - a finished stroke, a shape, a pasted
  //  selection, an inserted image, a scanned page - funnels through here, which is what makes "new
  //  things land on the current layer" one rule rather than one per creation site.  Callers that
  //  must preserve an existing layer (free-erase subpaths) pass it explicitly.
  // With no `next`, the insertion point is the end of the element's own layer's run, not the end of
  //  the page, so layers stack.  Undo and sync are unaffected: both express position as a sibling
  //  node, which this only chooses differently.
  void addStroke(Element* s, Element* next = NULL, int layer = LayerList::LAYER_CURRENT);
  void removeStroke(Element* s);
  // first element that sorts above `layer` in z-order, i.e. where a new element on `layer` goes
  Element* layerInsertPos(int layer) const;
  // first element at or above `layer` - where a DRAW_UNDER pen puts its stroke, which must mean
  //  "under all the ink on my layer", not under the whole page.  Images on the layer (scans) are
  //  skipped: the stroke goes above the last one, or a marker would vanish behind the picture
  Element* layerFirstElement(int layer) const;
  // Move an element to another layer, restacking it into that layer's run (at `next` if given).
  //  Returns the element it used to precede (NULL = it was last), which is exactly what an undo item
  //  needs to put it back: a layer change is also a z-order change, so restoring only the layer id
  //  would leave the element somewhere the user never moved it to.
  Element* moveToLayer(Element* s, int layer, Element* next = NULL);
  // push the layer table's hidden flags onto the nodes; drawing is a tree walk with no access to
  //  the table, so visibility has to be expressed as SVG display
  void applyLayerState();
  // the one gate the editing paths consult: selection, all four erasers, insert space, group,
  //  restyle.  An element on an unknown layer is editable - see LayerList's fail-open rule.
  bool isEditable(const Element* s) const;
  void onAddStroke(Element* s);
  void onRemoveStroke(Element* s);
  const char* getHyperRef(Point pos) const;
  SvgNode* findNamedNode(const char* idstr) const;

  bool saveSVG(IOStream& file, Dim x = 0, Dim y = 0);
  bool saveSVGFile(const char* filename);
  bool loadSVG(SvgDocument* doc);
  bool loadSVGFile(const char* filename = NULL, bool delayload = false);
  bool ensureLoaded(bool checkmem = true);
  void migrateLegacySVG();
  void contentToRuling();
  bool hasOutlineEntry() const { return !outlineTitle.empty(); }
  // set (or clear, with a NULL/empty title) this page's outline entry, writing through to the SVG
  void setOutlineEntry(const char* title, int level);
  void setSelected(bool sel);
  void unload();

  // deepest nesting level an entry may claim; keeps a malformed document from producing a silly tree
  static const int MAX_OUTLINE_LEVEL = 8;
  static Dim BLANK_Y_RULING;
  static const color_t DEFAULT_RULE_COLOR = Color::BLUE;
  //static const int NOT_AUTO_SAVED = INT_MAX;
  static bool enableDropShadow;
};
