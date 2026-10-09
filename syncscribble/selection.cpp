#include "selection.h"
#include "document.h"
#include "basics.h"
#include "usvg/svgxml.h"

// Selection:
// Basic procedure:
// - search target Page for Elements not yet selected and within new selection region
// - search selection list for Elements outside new selection region
// In order to draw selected strokes with the proper z-order, they are drawn normally, but Element:applyStyle
//  checks to see if the strokes are selected and alters style if so.

// Erasing: - stoke and ruled erase are implemented as selections from which strokes cannot be removed
// Stroke eraser: we use Selection::selectPath which for now uses only the end points of the path
//  (so we should be breaking up long lines), which, in turn, calls Stroke::isNearPoint, which also only
//  considers the stroke's points, so we need to avoid long lines here too; this will also make implementing
//  free erase easier (we just delete the nearby segments instead of the whole stroke.
// Ruled eraser - currently, this works exactly like Ruled selection, but we may want an option to not jump
//  lines
// Free eraser: implementation is in ScribbleArea mostly
// backtracking on erasure: only makes sense for ruled erase (basically just deselect = true), and not worth
//  the extra complication (for the user) - one can just do ruled select + cut or delete

// Dealing with different types of selections:  For now, all selections are converted to rect selections once
//  they are made (i.e. on release of selection tool).  The different drawing of selected strokes still makes
//  it clear which strokes within the rectangle are selected.  Reasons:
// - every other drawing program does it
// - makes available the resize and rotate functionality of rect selections
// This also solves several problems with manipulating other selection types:
// - moving ruled selections by any offset in x, and by y offset not a multiple of y ruling
// - what background/type to use for pasted selection, inverted selection

// Selector approach - a bit ugly, but ...
// - allows us to change selection background/type without copying stroke list
// - allows us to separate quite different code found in different selectors
// Alternatives:
// - rectSelector = new RectSelector(); currSelection->setSelector(rectSelector); ... pretty minor change
// - currSelection->select(new RuledSelector(...)); ... but this doesn't allow for history!

Selection::Selection(Page* source, StrokeDrawType drawtype) : m_drawType(drawtype)
{
  page = source;
  sourceNode = static_cast<Element*>(page->contentNode->ext());
  ruling = page->pageFrame();
}

/*Selection::Selection(const Selection* sel, Page* page) : Selection(page)
{
  for(Element* s : sel->strokes) {
    strokes.push_back(s->cloneNode());
    strokes.back()->node->setParent(NULL);
  }
}*/

Selection::~Selection()
{
  if(selector)
    delete selector;
  clear();
}

void Selection::clear()
{
  // unselect all strokes
  invalidateBBox();
  if(selMode != SELMODE_PASSIVE) {
    for(Element* node : strokes) {
      node->setSelected(NULL);
      node->resetTransform();
    }
  }
  strokes.clear();
}

void Selection::deleteStrokes()
{
  for(Element* s : strokes) {
    s->setSelected(NULL);
    page->removeStroke(s);
  }
  // delete our list of now invalid strokes
  strokes.clear();
  invalidateBBox();
  maxTimestamp = 0;
}

void Selection::addStroke(Element* s, Element* next)
{
  s->setSelected(this);
  if(next)
    strokes.insert(std::find(strokes.begin(), strokes.end(), next), s);
  else
    strokes.push_back(s);
  if(bbox.isValid())
    bbox.rectUnion(s->bbox());
  maxTimestamp = 0;
}

bool Selection::removeStroke(Element* s)
{
  s->setSelected(NULL);
  auto it = std::find(strokes.begin(), strokes.end(), s);
  if(it == strokes.end())
    return false;
  strokes.erase(it);
  invalidateBBox();
  maxTimestamp = 0;
  return true;
}

void Selection::selectAll()
{
  invalidateBBox();
  maxTimestamp = 0;
  for(Element* s : sourceNode->children()) {
    if(!page->isEditable(s))
      continue;  // Select All must not pick up a locked layer - the selection is an editing handle
    s->setSelected(this);
    strokes.push_back(s);
  }
}

void Selection::invertSelection()
{
  strokes.clear();
  invalidateBBox();
  maxTimestamp = 0;
  // invert selection flag and add newly selected strokes to list
  for(Element* s : sourceNode->children()) {
    if(!s->isSelected(this)) {
      if(!page->isEditable(s))
        continue;
      s->setSelected(this);
      strokes.push_back(s);
    }
    else
      s->setSelected(NULL);
  }
}

// ideally, would like to avoid handling each stroke property explicitly, but don't see any
//  easy way to do this
// If current strokes have multiple values for a property, we ignore that property
// - e.g. to select all red strokes, we select one, then do select similar, notice that some are not
//  selected (due to difference in other properties), select one of those and run select similar again
//  ... interesting, but need a way for user to specify which properties to ignore manually
void Selection::selectbyProps(Element* elproto, bool useColor, bool useWidth)
{
  SCRIBBLE_LOG("selectByProps disabled!!!");
  /*if(!elproto) {
    if(strokes.empty())
      return;
    // TODO: iterate over all strokes and clear color, width flags if any differ from proto
    elproto = strokes.front();
  }
  if(!elproto->isA(Element::STROKE))
    return;
  Stroke* proto = (Stroke*)elproto;

  Rect dirty;
  invalidateBBox();
  // not really possible to deselect, I guess
  for(Element* s : sourceNode->renderers()) {
    if(s->strokeFlags == proto->strokeFlags && !s->isSelected(this)) {
      if((!useColor || s->color == proto->color) && (!useWidth || s->width == proto->width)) {
        s->setSelected(this);
        dirty.rectUnion(s->bbox);
        strokes.push_back(s);
      }
    }
  }
  growDirtyRect(dirty);*/
}

void Selection::recalcTimeRange()
{
  if(maxTimestamp != 0)
    return;
  minTimestamp = MAX_TIMESTAMP;
  maxTimestamp = 0;
  for(Element* s : strokes) {
    minTimestamp = std::min(minTimestamp, s->timestamp());
    maxTimestamp = std::max(maxTimestamp, s->timestamp());
  }
}

// Note that dirtyRect can be larger than select region - consider ruled selection; also fat strokes!
void Selection::doSelect()
{
  if(selMode == SELMODE_NONE)
    return;
  // remove deselected strokes from list and clear selected flag
  Rect dirty;
  invalidateBBox();
  maxTimestamp = 0;
  if(selMode != SELMODE_UNION) {
    for(auto ii = strokes.begin(); ii != strokes.end(); ) {
      if(!selector->selectHit(*ii)) {
        (*ii)->setSelected(NULL);
        dirty.rectUnion((*ii)->bbox());
        ii = strokes.erase(ii);  // returns iterator pointing to next element
      }
      else
        ++ii;
    }
  }
  // set selected flag for selected strokes and add to list
  for(Element* node : sourceNode->children()) {
    // An element on a locked or hidden layer is not selectable, and this one test is what makes the
    //  stroke eraser and the ruled eraser respect a lock too: both are implemented as selections
    //  (see the notes at the top of this file), so neither needs a check of its own.
    if(!page->isEditable(node))
      continue;
    // fetch bbox and compare ourselves or Stroke::isContained(...)
    // note that we don't steal elements from another selection (selection() must be NULL)
    if((!node->selection() || selMode == SELMODE_PASSIVE) && selector->selectHit(node)) {
      if(selMode != SELMODE_PASSIVE)
        node->setSelected(this);
      dirty.rectUnion(node->bbox());
      strokes.push_back(node);
    }
  }
}

Rect Selection::getBBox()
{
  if(!bbox.isValid()) {
    bbox = Rect();
    for(Element* s : strokes)
      bbox.rectUnion(s->bbox());
  }
  return bbox;
}

void Selection::translate(Dim dx, Dim dy)
{
  applyTransform(ScribbleTransform().translate(dx, dy));
}

void Selection::setOffset(Dim x, Dim y)
{
  translate(x - transform.xoffset(), y - transform.yoffset());
}

void Selection::rotate(Dim radians, Point origin)
{
  applyTransform(ScribbleTransform().rotate(radians, origin));
}

void Selection::scale(Dim sx, Dim sy, Point origin, bool scalewidths)
{
  ScribbleTransform tf(Transform2D(sx, 0, 0, sy, (1 - sx)*origin.x, (1 - sy)*origin.y),
      scalewidths ? 1 : 1/sx, scalewidths ? 1 : 1/sy);
      //scalewidths ? sx : SGN(sx), scalewidths ? sy : SGN(sy));
  applyTransform(tf);
}

void Selection::resetTransform()
{
  if(selector)
    selector->transform(transform.inverse());
  transform.reset();
  for(Element* s : strokes)
    s->resetTransform();
  invalidateBBox();
  // we used to just do this, but it doesn't handle reseting reflowed text
  //applyTransform(transform.inverse());
}

void Selection::applyTransform(const ScribbleTransform& tf)
{
  if(tf.isIdentity())
    return;
  transform = tf * transform;
  for(Element* stroke : strokes)
    stroke->applyTransform(tf);
  invalidateBBox();
  if(selector)
    selector->transform(tf);
}

Rect Selection::getBGBBox()
{
  return selector ? selector->getBGBBox() : Rect();
}

void Selection::setZoom(Dim zoom)
{
  if(selector)
    selector->setZoom(zoom);
}

// NOTE: caller is expected to call shrink() after this to update selection background!
void Selection::commitTransform()
{
  UndoHistory* hist = page->document->history;
  bool istf = !transform.isIdentity() && !transform.isTranslate();
  for(Element* node : strokes) {
    // complex transform (istf) percludes possibility of per-stroke offsets
    if(hist->undoable()) {
      if(istf)
        hist->addItem(new StrokeTransformItem(node, page, transform));
      else if(node->pendingTransform().xoffset() != 0 || node->pendingTransform().yoffset() != 0)
        hist->addItem(new StrokeTranslateItem(node, page, node->pendingTransform().xoffset(), node->pendingTransform().yoffset()));
    }
    node->commitTransform();
  }
  transform.reset();
}

// transform strokes without dirtying page or writing undo items
void Selection::stealthTransform(const ScribbleTransform& tf)
{
  //transform = tf * transform; -- don't think we want this
  for(Element* s : strokes) {
    s->applyTransform(tf);
    s->commitTransform();
  }
  invalidateBBox();
}

void Selection::shrink()
{
  if(selector)
    selector->shrink();
}

bool Selection::xchgBGDirty(bool b)
{
  return selector ? std::exchange(selector->bgDirty, b) : false;
}

void Selection::setDrawType(StrokeDrawType newtype)
{
  if(m_drawType != newtype) {
    for(Element* s : strokes) {
      if(newtype == STROKEDRAW_NONE || m_drawType == STROKEDRAW_NONE)
        s->node->setDisplayMode(newtype == STROKEDRAW_NONE ? SvgNode::AbsoluteMode : SvgNode::BlockMode);
      s->node->setDirty(SvgNode::PIXELS_DIRTY);
    }
    m_drawType = newtype;
  }
}

// clipboard must preserve z-order (doesn't matter for other Selection operations, which is why we don't
//  keep our stroke list sorted)
void Selection::toSorted(Clipboard* dest) const
{
  for(Element* s : sourceNode->children()) {
    if(s->isSelected(this)) {
      if(s->node->type() == SvgNode::USE) {
        // make sure <use> target is available in clipboard
        const char* href = static_cast<SvgUse*>(s->node)->href();
        const SvgNode* target = static_cast<SvgUse*>(s->node)->target();
        if(target && !dest->content->namedNode(href)) {
          // wrap in <defs> to prevent separate drawing of target, transform being added to <symbol>, etc.
          SvgDefs* wrap = new SvgDefs();
          wrap->addChild(target->clone());
          dest->addStroke(new Element(wrap));
        }
      }
      dest->addStroke(s->cloneNode());
    }
  }
}

StrokeProperties Selection::getStrokeProperties() const
{
  return StrokeProperties::get(strokes.begin(), strokes.end());
}

void Selection::setStrokeProperties(const StrokeProperties& props)
{
  // make a copy of strokes rather than trying to modify while iterating; could consider instead a
  //  Selection::replaceStroke() to replace a stroke with a clone
  std::vector<Element*> strokescopy(strokes.begin(), strokes.end());
  for(Element* s : strokescopy) {
    // if node doesn't have fill attribute, easier to replace it than handle it as a special case
    bool nofill = props.color.alpha() > 0 && !s->node->getAttr("fill");
    // a dash pattern applies only to a stroke, so a filled pen stroke is redrawn as a stroked centreline;
    //  that changes its geometry, which StrokeChangedItem cannot record, so it is replaced like a group
    bool restroke = props.dashStyle > ScribblePen::DASH_SOLID && s->isFilledPenStroke();
    // since setProperties descends recursively into groups (container or <text> w/ <tspan>s) we need to
    //  replace group so we can undo properly
    if(s->containerNode() || s->node->type() == SvgNode::TEXT || nofill || restroke) {
      Element* t = s->cloneNode();
      page->addStroke(t, s);
      addStroke(t);
      removeStroke(s);
      page->removeStroke(s);
      if(restroke)
        t->convertToStroked();
      t->setProperties(props);
    }
    else {
      StrokeChangedItem* undoitem = new StrokeChangedItem(s, page);
      if(s->setProperties(props))
        page->document->history->addItem(undoitem);
      else
        delete undoitem;
    }
  }
}

bool Selection::containsGroup()
{
  for(Element* s : strokes) {
    if(s->containerNode() && (s->node->type() == SvgNode::G || s->node->type() == SvgNode::DOC))
      return true;
  }
  return false;
}

void Selection::ungroup()
{
  // easiest way to avoid complication of modifying strokes list while iterating is to make a copy
  std::vector<Element*> strokescopy(strokes.begin(), strokes.end());
  for(Element* s : strokescopy) {
    // don't break up <defs> or <pattern>!
    if(s->containerNode() && (s->node->type() == SvgNode::G || s->node->type() == SvgNode::DOC)) {
      Transform2D tf = s->node->totalTransform();
      for(SvgNode* n : s->containerNode()->children()) {
        SvgNode* m = n->clone();
        // multistroke children will have Elements, but nodes in other (i.e. opaque) groups won't
        Element* t = m->hasExt() ? static_cast<Element*>(m->ext()) : new Element(m);
        page->addStroke(t, s);
        if(!tf.isIdentity()) {  //s->node->hasTransform()) {
          t->applyTransform(tf);  //s->node->getTransform());
          t->commitTransform();
        }
        // transfer attributes not overridden - but only standard attributes
        for(const SvgAttr& attr : s->node->attrs) {
          if(!t->node->getAttr(attr.name()) && attr.stdAttr() != SvgAttr::UNKNOWN)
            t->node->setAttr(attr);
        }
        // should we also copy class?
        addStroke(t);
      }
      removeStroke(s);
      page->removeStroke(s);  // delete original
    }
  }
}

// for overlay
void Selection::draw(Painter* painter, StrokeDrawType drawtype)  //const Rect& dirty)
{
  auto oldDrawType = m_drawType;
  m_drawType = drawtype != STROKEDRAW_NONE ? drawtype : m_drawType;
  SvgPainter svgp(painter);
  for(Element* s : strokes)
    svgp.drawNode(s->node);  //, dirty);
  m_drawType = oldDrawType;
}

// stuff for reflow

// All the ruled operations below work in `ruling`'s local coordinates, where its lines are horizontal:
//  for the page's own ruling that is the page shifted by yRuleOffset, so behaviour there is exactly what
//  it always was; for a ruling region it is the region's frame, rotated or not.
static Dim rulingYr(const RulingFrame& f) { return f.yrulingOr(Page::BLANK_Y_RULING); }
static int rulingLine(const RulingFrame& f, const Element* s) { return f.line(s->com(), Page::BLANK_Y_RULING); }

// An element's bounding box in a frame's local coordinates.  Unrotated, that is the page bbox shifted,
//  exactly as before.  Tilted, rotating the page bbox would inflate it - a stroke written along a tilted
//  line would get a box several lines tall and fail every "is it on this line" test - so a path's box
//  is taken from its own points in the frame instead.
static Rect localElementBBox(const RulingFrame& f, Element* s)
{
  if(!f.isRotated() || !s->isPathElement())
    return f.localBBox(s->bbox());
  Path2D path = *static_cast<SvgPath*>(s->node)->path();
  if(path.empty())
    return f.localBBox(s->bbox());
  // a deferred pending move (a translate mid-gesture) is in bbox() but not in the path, so add it here
  //  too, or reflow - which subtracts the pending offset from this box - would subtract it twice
  Transform2D pending = s->pendingDeferred() ? s->pendingTransform().tf() : Transform2D();
  path.transform(f.localTransform() * pending * s->node->totalTransform());
  Rect r = path.getBBox();
  Dim sw = s->node->getFloatAttr("stroke-width", 0);
  return sw > 0 ? r.pad(sw/2) : r;
}

// this is a bit of a hack ... we return the ruleline of the first stroke after sorting
int Selection::sortRuled()
{
  if(strokes.empty()) return -1;
  const RulingFrame& f = ruling;
  strokes.sort([&f](const Element* a, const Element* b) {
    int linea = rulingLine(f, a), lineb = rulingLine(f, b);
    return linea == lineb ? localElementBBox(f, const_cast<Element*>(a)).left
        < localElementBBox(f, const_cast<Element*>(b)).left : linea < lineb;
  });
  return rulingLine(ruling, strokes.front());
}

// strokes should be sorted if dx != 0
void Selection::insertSpace(Dim dx, int dline)
{
  if(strokes.size() > 0 && dx != 0) {
    auto ii = strokes.begin();
    int currline = rulingLine(ruling, *ii);
    ScribbleTransform tf(Transform2D::translating(ruling.toPageDir(Point(dx, 0))));
    for(; ii != strokes.end() && rulingLine(ruling, *ii) == currline; ++ii)
      (*ii)->applyTransform(tf);  //translateStroke(*ii, dx, 0);
    invalidateBBox();
  }
  Point down = ruling.toPageDir(Point(0, dline * rulingYr(ruling)));
  translate(down.x, down.y);
}

// To enable realtime reflow w/o unnecessary drawing and bounds update, we use Element.scratch to store offset
//  for in-progress reflow, then compare to previous offset (from pendingTransform), updating iff different.
//  scratch is in the ruling's local coordinates; the pending transform is a page-space translation.
static Point pendingLocalOffset(Element* s, const RulingFrame& f)
{
  const ScribbleTransform& oldtf = s->pendingTransform();
  return f.toLocalDir(Point(oldtf.xoffset(), oldtf.yoffset()));
}

static Rect workingBBox(Element* s, const RulingFrame& f)
{
  return localElementBBox(f, s).translate(s->scratch - pendingLocalOffset(s, f));
}

static int workingLine(Element* s, const RulingFrame& f)
{
  Point local = f.toLocal(s->com()) + s->scratch - pendingLocalOffset(s, f);
  return int(std::floor(local.y/rulingYr(f)));
}

// Writing on every second line (the Skip Lines toggle): the line pressed on is a text line, and so is every
//  second line from it.  The returned frame makes each text line plus the blank line below it one "line" -
//  pitch doubled, origin on the top of the pressed line - so reflow wraps to the next text line rather than
//  the blank one, insert space steps two lines, and an underline in a blank line still belongs to the text
//  above it.  Nothing downstream needs to know.  It is a toggle rather than detected from the ink: a guess
//  that can misfire moves text onto the wrong line with no visible reason.
// The origin is the pressed line's top, never above it.  It used to be half a line above, which is fine for
//  a press on a text line but not for one on the blank line between two (where the pen goes to push the
//  text below down): that half line is the lower half of the text line above, where handwriting sitting on
//  the rule has its centre, so the letters of the line above the press moved too.
RulingFrame skippedLineFrame(const RulingFrame& frame, Point pos)
{
  return skippedLineFrame(frame, frame.line(pos, Page::BLANK_Y_RULING));
}

RulingFrame skippedLineFrame(const RulingFrame& frame, int line)
{
  const Dim yr = frame.yrulingOr(Page::BLANK_Y_RULING);
  RulingFrame skipped = frame;
  skipped.origin = frame.toPage(Point(0, line*yr));
  skipped.yRuling = 2*yr;
  return skipped;
}

// Where wrapped words start and how far apart they sit come from the writing, not the page: a dot grid has
//  no margin, so margin + gap put wrapped words against the page edge, left of the text, and a gap sized
//  from the pitch is tighter than most people's word spacing on a fine grid.  Measured once per gesture,
//  from the ink as it was before the gesture moved any of it.
void Selection::measureReflowInk(Dim minWordGap)
{
  reflowMeasured = true;
  if(strokes.empty())
    return;
  const RulingFrame& f = ruling;
  const std::vector<Dim>& lstops = static_cast<RuledSelector*>(selector)->lstops;
  Dim marginLeft = f.region ? page->frameExtent(f).left : page->props.marginLeft;
  // the text's left edge: leftmost ink on the line the reflow starts on and on the line above, so that an
  //  indented first line does not indent the rest of its paragraph.  Ink in the margin (bookmarks) or left
  //  of a column stop belongs to something else.
  int line0 = rulingLine(f, strokes.front());
  for(Element* s : page->children()) {
    if(s->isRulingRegion() || page->regionAt(s->com()) != f.region)
      continue;
    int line = rulingLine(f, s);
    if(line != line0 && line != line0 - 1)
      continue;
    Dim left = localElementBBox(f, s).left - pendingLocalOffset(s, f).x;
    Dim lstop = line >= 0 && line < int(lstops.size()) ? MAX(marginLeft, lstops[line]) : marginLeft;
    if(left >= lstop)
      reflowIndent = MIN(reflowIndent, left);
  }
  // the writer's word gap: median of the gaps between words in the selection (sorted by line, then left)
  std::vector<Dim> gaps;
  int currline = INT_MIN;
  Dim currRight = 0;
  for(Element* s : strokes) {
    Rect bbox = localElementBBox(f, s).translate(-pendingLocalOffset(s, f));
    int line = rulingLine(f, s);
    if(line != currline) {
      currline = line;
      currRight = bbox.right;
      continue;
    }
    if(bbox.left - currRight >= minWordGap)
      gaps.push_back(bbox.left - currRight);
    currRight = MAX(currRight, bbox.right);
  }
  if(!gaps.empty()) {
    std::nth_element(gaps.begin(), gaps.begin() + gaps.size()/2, gaps.end());
    reflowWordGap = gaps[gaps.size()/2];
  }
}

// Although reflow might not work well for long paragraphs on unruled pages, don't really see any harm
//  enabling it since alternative is a bunch of strokes past edge of page; if it doesn't work, don't use it!
// TODO: consider rounding dx, nextdx to integers!
void Selection::reflowStrokes(Dim dx, int dline, Dim minWordSep)
{
  const Dim yruling = rulingYr(ruling);
  if(yruling == 0 || strokes.empty() || workingLine(strokes.front(), ruling) + dline < 0)
    return;
  if(dx != 0) {
    int currline = workingLine(strokes.front(), ruling);
    for(auto ii = strokes.begin(); ii != strokes.end() && workingLine(*ii, ruling) == currline; ++ii)
      (*ii)->scratch.x = dx;
  }
  if(dline != 0) {
    Dim dy = dline * yruling;
    for(Element* s : strokes)
      s->scratch.y = dy;
  }

  const Dim minWordGap = minWordSep * yruling;
  if(!reflowMeasured)
    measureReflowInk(minWordGap);
  // the gap put between wrapped words and text already on the line: the writer's own, but at least the old
  //  fixed 1.25 x minWordGap and at most a line height, so one wide gap cannot open a column
  const Dim wordGap = reflowWordGap > 0 ?
      std::min(std::max(reflowWordGap, 1.25*minWordGap), yruling) : 1.25*minWordGap;
  auto curr = strokes.begin();
  auto wordbreak = strokes.begin();
  // TODO: should we init currRight to left instead of 0?
  dx = 0;
  Dim nextdx = 0, currRight = 0;
  // rule line of first stroke
  int currline = workingLine(*curr, ruling);
  // a region has no margin; its extent is the page's width for its lines
  Rect extent = page->frameExtent(ruling);
  Dim marginLeft = ruling.region ? extent.left : page->props.marginLeft;
  Dim left = marginLeft;
  Dim right = extent.right - 0.5*minWordGap;
  while(1) {
    if(currline+1 < int(((RuledSelector*)selector)->lstops.size())) {
      left = MAX(marginLeft, static_cast<RuledSelector*>(selector)->lstops[currline+1]);
      right = static_cast<RuledSelector*>(selector)->rstops[currline] - 0.5*minWordGap;
    }
    // Step 1: find last word break before overflow
    while(1) {
      currRight = MAX(currRight, workingBBox(*curr, ruling).right);
      if(currRight >= right)
        break;
      curr++;
      // no strokes beyond right on this line means we are all done reflowing
      if(curr == strokes.end() || workingLine(*curr, ruling) != currline)
        goto alldone;
      if(workingBBox(*curr, ruling).left - currRight >= minWordGap)
        wordbreak = curr;
    }
    // no word break found - abort; probably should notify user
    if(wordbreak == strokes.end())
      goto alldone;
    // Step 1.5: find first stroke of next line so we can start strokes moved down at same position
    //  if the next line is empty, strokes moved down start where the paragraph's text does (never left of
    //  the margin or column stop), or at (left + minWordGap) if that is unknown
    dx = reflowIndent < MAX_DIM ? MAX(left, reflowIndent) : left + minWordGap;
    while(++curr != strokes.end()) {
      if(workingLine(*curr, ruling) == currline)
        continue;
      if(workingLine(*curr, ruling) == currline + 1)
        dx = workingBBox(*curr, ruling).left;
      break;
    }
    // Step 2: move all strokes incl and beyond wordbreak down to next line
    curr = wordbreak;
    dx -= workingBBox(*wordbreak, ruling).left;
    nextdx = 0;
    bool insblankline = false;
    while(curr != strokes.end() && workingLine(*curr, ruling) == currline) {
      (*curr)->scratch += Point(dx, yruling);  //translateStroke(*curr, dx, yruling);
      // note we want post-translation bbox here
      nextdx = MAX(nextdx, workingBBox(*curr, ruling).right);
      curr++;
      insblankline = true;  // 1 or more strokes moved down
    }
    // Step 3: shift all strokes on next line right to accommodate strokes moved down
    currline++;
    while(curr != strokes.end() && workingLine(*curr, ruling) == currline) {
      // we use insblankline here to test for first pass through loop.  We insert a wordGap space
      //  between strokes moved down (which end at nextdx) and strokes already on the line. Recall that
      //  strokes are sorted by bbox.left
      if(insblankline)
        nextdx += wordGap - workingBBox(*curr, ruling).left;
      (*curr)->scratch.x += nextdx;  //translateStroke(*curr, nextdx, 0);
      curr++;
      insblankline = false;  // line is not blank
    }
    // if strokes were moved down to a blank line, insert a new blank line below
    if(insblankline) {
      for(auto rest = curr; rest != strokes.end(); rest++)
        (*rest)->scratch.y += yruling;  //translateStroke(*rest, 0, yruling);
    }
    // start next iteration from first stroke of next line because we need to find
    //  next word break; result is that all strokes moved down will be processed twice
    curr = wordbreak;
    currRight = MIN_DIM;
    wordbreak = strokes.end();
  }
alldone:
  for(Element* s : strokes) {
    Point olddr = Point(s->pendingTransform().xoffset(), s->pendingTransform().yoffset());
    Point newdr = ruling.toPageDir(s->scratch);
    if(!approxEq(newdr, olddr, 0.01))  // numerical errors seem to accumulate for x
      s->applyTransform(ScribbleTransform().translate(newdr - olddr));
    s->scratch = Point(0, 0);
  }
  invalidateBBox();
}

// Clipboard

void Clipboard::paste(Selection* sel, bool move)
{
  content->replaceIds(sel->page->svgDoc.get());  // replace any ids that already exist in target document
  for(SvgNode* n : content->children()) {
    SvgNode* m = move ? n : n->clone();
    Element* t = m->hasExt() ? static_cast<Element*>(m->ext()) : new Element(m);
    sel->page->addStroke(t);
    sel->addStroke(t);
  }
  if(move) {
    content->children().clear();
    content.reset(new SvgDocument);
  }
}

void Clipboard::saveSVG(std::ostream& file)
{
  // set document size so that nothing is clipped when pasted into, e.g., Jupyter; no effect on pasting back
  //  into Write as text since outermost <svg> is removed unless it has a viewBox
  Rect bbox = content->bounds();
  content->setWidth(std::ceil(bbox.right));
  content->setHeight(std::ceil(bbox.bottom));

  XmlStreamWriter xmlwriter;
  SvgWriter writer(xmlwriter);
  writer.serialize(content.get());
  xmlwriter.save(file);
}

// Selector
// note that failure to define all virtual methods may result in
//  "undefined reference to vtable" errors attributed to the constructor and destructor

Selector::Selector(Selection* _selection) : selection(_selection)
{
  selection->selector = this;
  // use blue selection for light backgrounds, yellow for dark
  bgStroke = selection->page->props.color.luma() > 127 ? Color::BLUE : Color::YELLOW;
  bgFill = Color(bgStroke).setAlphaF(0.20f);
}

Selector::~Selector()
{
  if(selection)
    selection->selector = NULL;
}

// PathSelector class

// no reason not to have reasonable behavior for all structure nodes
static bool isNearPoint(Point p, Dim radius, SvgNode* node)
{
  if(!node->bounds().pad(radius).contains(p))
    return false;
  if(node->asContainerNode()) {
    for(SvgNode* child : node->asContainerNode()->children())
      if(isNearPoint(p, radius, child))
        return true;
    return false;
  }
  if(node->type() == SvgNode::IMAGE) {  //&& !Element::ERASE_IMAGES)
    Rect bbox = node->bounds();
    return std::abs(p.x - bbox.left) < radius || std::abs(p.x - bbox.right) < radius
        || std::abs(p.y - bbox.top) < radius || std::abs(p.y - bbox.bottom) < radius;
  }
  if(node->type() != SvgNode::PATH)
    return true;  // bbox hit for non-path node

  const Path2D& path = *static_cast<SvgPath*>(node)->path();
  const Transform2D tf = node->totalTransform();
  Dim dist = tf.isIdentity() ? path.distToPoint(p) : Path2D(path).transform(tf).distToPoint(p);
  return dist < radius;
}

bool PathSelector::selectHit(Element* s)
{
  return isNearPoint(selPos, selRadius, s->node);
}

void PathSelector::selectPath(Point p, Dim radius)
{
  selRadius = radius;
  path.addPoint(p);
  // subdivide as necessary to get sufficient point density
  if(selPos.isNaN())
    selPos = p;
  Point dr = p - selPos;
  Dim d = dr.dist();
  int nsteps = int(d/(radius/2)) + 1;
  Point step = dr/nsteps;
  for(int ii = 0; ii < nsteps; ++ii) {
    selPos += step;
    selection->doSelect();
  }
  bgDirty = true;
}

Rect PathSelector::getBGBBox()
{
  return path.empty() ? Rect() : selection->transform.mult(path.controlPointRect().pad(selRadius));
}

// RectSelection class
Dim RectSelector::HANDLE_SIZE = 4;
Dim RectSelector::HANDLE_PAD = 4;  // can be changed from ScribbleArea::loadConfig()

// "touching" selection: an element is hit if any part of its outline lies in the selection area.  Paths
//  are flattened (the control points of a shape's curve lie outside it); anything else is its bounds.

static Path2D flatOutline(SvgNode* node)
{
  if(node->type() != SvgNode::PATH)
    return Path2D().addRect(node->bounds());
  const Path2D* path = static_cast<SvgPath*>(node)->path();
  Path2D flat = path->isSimple() ? *path : path->toFlat();
  flat.transform(node->totalTransform());
  return flat;
}

// Liang-Barsky: clip the segment to the rect and see if anything is left
static bool segmentHitsRect(Point a, Point b, const Rect& r)
{
  Dim t0 = 0, t1 = 1;
  const Dim dx = b.x - a.x, dy = b.y - a.y;
  const Dim dir[4] = {-dx, dx, -dy, dy};
  const Dim dist[4] = {a.x - r.left, r.right - a.x, a.y - r.top, r.bottom - a.y};
  for(int i = 0; i < 4; ++i) {
    if(dir[i] == 0) {
      if(dist[i] < 0)
        return false;  // parallel to this edge and outside it
    }
    else {
      Dim t = dist[i]/dir[i];
      if(dir[i] < 0)
        t0 = std::max(t0, t);
      else
        t1 = std::min(t1, t);
      if(t0 > t1)
        return false;
    }
  }
  return true;
}

static bool touchesRect(const Rect& r, SvgNode* node)
{
  if(node->asContainerNode()) {
    for(SvgNode* child : node->asContainerNode()->children())
      if(touchesRect(r, child))
        return true;
    return false;
  }
  if(node->type() != SvgNode::PATH)
    return r.overlaps(node->bounds());
  Path2D outline = flatOutline(node);
  for(int ii = 0; ii < outline.size(); ++ii) {
    if(r.contains(outline.point(ii)))
      return true;
    if(ii > 0 && outline.command(ii) != Path2D::MoveTo && segmentHitsRect(outline.point(ii-1), outline.point(ii), r))
      return true;
  }
  return false;
}

bool RectSelector::selectHit(Element* s)
{
  switch(rectSelMode) {
  case RECTSEL_BBOX:
    return selRect.contains(s->bbox());
  case RECTSEL_ANY:
    if(!selRect.overlaps(s->bbox()))
      return false;
    return selRect.contains(s->bbox()) || touchesRect(selRect, s->node);
  default:
    return false;
  }
}

Rect RectSelector::getBGBBox()
{
  if(!selRect.isValid())
    return selRect;

  // assume scale is never combined with rotation
  if(selection->transform.isRotating()) {
    Rect r = selRect;
    r.pad(HANDLE_SIZE/mZoom);
    r.top -= 7*HANDLE_SIZE/mZoom;
    Point center = selection->transform.mult(r.center());
    Dim maxwidth = sqrt(r.width()*r.width() + r.height()*r.height());
    return Rect::centerwh(center, maxwidth, maxwidth);
  }
  // padding accounts for the grab handles
  Rect r = Rect(selRect).pad(HANDLE_SIZE/mZoom);  //selection->transform.mult(selRect).pad(HANDLE_SIZE/mZoom);
  if(drawHandles)
    r.top -= 7*HANDLE_SIZE/mZoom;  // rotation handle
  return r;
}

// In the future, we can optimize selection by only looking for strokes in previously unselected region
// RectSelector does not use bgDirty - instead, user needs to check if BG bbox has changed
void RectSelector::selectRect(Dim x0, Dim y0, Dim x1, Dim y1)
{
  // ensure correct corners for selection rectangle (left, top, right, bottom)
  selRect = Rect::ltrb(MIN(x0, x1), MIN(y0, y1), MAX(x0, x1), MAX(y0, y1));
  selection->doSelect();
}

void RectSelector::shrink()
{
  selRect = selection->getBBox();
  // The absence of this check was causing a strange bug that only manifested on Android (could not reproduce
  //  on PC, even with Dim defined as a float).  Perhaps some subtle floating point thing, since an invalid
  //  rect uses +/-FLT_MAX (maybe we should change this)
  if(selRect.isValid()) {
    // min selection size is a multiple of resize handle size (to ensure there is a place to drag)
    Dim padx = (4*HANDLE_PAD/mZoom - selRect.width())/2;
    Dim pady = (4*HANDLE_PAD/mZoom - selRect.height())/2;
    selRect.pad(std::max(Dim(1.0), padx + 1), std::max(Dim(1.0), pady + 1));
  }
  enableCrop = selection->count() == 1 && selection->strokes.front()->node->type() == SvgNode::IMAGE;
}

// check for hit on rect selection handles used for resizing
// We return the scaling origin point, which is the corner opposite the handle grabbed!
Point RectSelector::scaleHandleHit(Point pos, bool touch)
{
  if(!drawHandles)
    return Point(NaN, NaN);
  Dim h = touch ? 2*HANDLE_SIZE : HANDLE_SIZE;
  Dim a = (h+3)/mZoom; // +3 is to make it easier to grab handle
  // use selection bbox, not BG bbox for scaling origin!
  Rect bbox = selRect;  //selection->getBBox();
  Rect handlerect = Rect::ltrb(-a, -a, a, a);
  if(Rect(handlerect).translate(selRect.left, selRect.top).contains(pos))
    return Point(bbox.right, bbox.bottom);
  if(Rect(handlerect).translate(selRect.right, selRect.top).contains(pos))
    return Point(bbox.left, bbox.bottom);
  if(Rect(handlerect).translate(selRect.left, selRect.bottom).contains(pos))
    return Point(bbox.right, bbox.top);
  if(Rect(handlerect).translate(selRect.right, selRect.bottom).contains(pos))
    return Point(bbox.left, bbox.top);
  return Point(NaN, NaN);
}

Point RectSelector::rotHandleHit(Point pos, bool touch)
{
  if(!drawHandles)
    return Point(NaN, NaN);
  Dim h = touch ? 2*HANDLE_SIZE : HANDLE_SIZE;
  Dim a = (h+3)/mZoom;
  Rect handlerect = Rect::ltrb(-a, -a, a, a);
  if(handlerect.translate(selRect.center().x, selRect.top - 6*HANDLE_SIZE/mZoom).contains(pos))
    return selRect.center();
  return Point(NaN, NaN);
}

Point RectSelector::cropHandleHit(Point pos, bool touch)
{
  if(!drawHandles || !enableCrop)
    return Point(NaN, NaN);
  Dim h = touch ? 2*HANDLE_SIZE : HANDLE_SIZE;
  Dim a = (h+3)/mZoom; // +3 is to make it easier to grab handle
  Rect handlerect = Rect::ltrb(-a, -a, a, a);
  Rect bbox = selection->getBBox();  //.toSize();
  if(Rect(handlerect).translate(selRect.left, selRect.center().y).contains(pos))
    return Point(bbox.right, NaN);  //Point(-1, 0);
  if(Rect(handlerect).translate(selRect.right, selRect.center().y).contains(pos))
    return Point(bbox.left, NaN);  //Point(1, 0);
  if(Rect(handlerect).translate(selRect.center().x, selRect.top).contains(pos))
    return Point(NaN, bbox.bottom);  //Point(0, -1);
  if(Rect(handlerect).translate(selRect.center().x, selRect.bottom).contains(pos))
    return Point(NaN, bbox.top);  //Point(0, 1);
  return Point(NaN, NaN);
}

void RectSelector::setZoom(Dim zoom)
{
  if(mZoom != zoom) {
    mZoom = zoom;
    shrink();
  }
}

// We tried using corner of contents bbox instead of sel BG bbox as scaling origin, and shrink()ing BG during
//  scaling to handle content growing larger than bbox (when not scaling stroke width), but this then causes
//  sel BG to move significantly when scaling stroke width, and cursor to become separated from scale handle.
// Given the complexity with stroke width, I think the best approach is to make sel BG to behave nicely when
//  scaling.
void RectSelector::transform(const Transform2D& tf)
{
  if(!tf.isRotating())
    selRect = tf.mapRect(selRect);  //shrink();
}

// RuledSelection class

bool RuledRange::isSingleLine() const
{
  return ymin + yruling >= ymax;
}

bool RuledRange::isValid() const
{
  return !isSingleLine() || (ymin < ymax && x0 <= x1);
}

RuledRange& RuledRange::translate(Dim dx, Dim dy)
{
  x0 += dx;
  x1 += dx;
  ymin += dy;
  ymax += dy;
  lstops.clear();
  rstops.clear();
  return *this;
}

Dim RuledRange::lstop(int idx) const
{
  if(idx >= 0 && idx < int(lstops.size()))
    return lstops[idx];
  if(idx == 0)
    return x0;
  return (idx >= 0 && idx < nlines()) ? MIN_DIM : MIN_DIM;  //MAX_X_DIM
}

Dim RuledRange::rstop(int idx) const
{
  if(idx >= 0 && idx < int(rstops.size()))
    return rstops[idx];
  if(idx == nlines() - 1)
    return x1;
  return (idx >= 0 && idx < nlines()) ? MAX_DIM : MAX_DIM;  //MIN_X_DIM
}

Dim RuledSelector::MIN_DIV_HEIGHT = 2;

// If findStops() did not run (e.g., due to cursor down in left margin) left = MIN_X_DIM and strokes in the
//  left margin will be selected and manipulated.  If findStops() did run, strokes in left margin will not be
//  selected - I think this is the desired behavior

static bool containedRuled(const RuledRange& range, Element* s)
{
  if(s->isMultiStroke()) {
    for(Element* ss : s->children())
      if(!containedRuled(range, ss))
        return false;
    return true;
  }
  Rect bbox = localElementBBox(range.frame, s);
  if(!s->isPathElement()) {
    if(bbox.top < range.ymin || bbox.bottom >= range.ymax)
      return false;

    int lastline = floor((bbox.bottom - range.ymin)/range.yruling);
    int ii = floor((bbox.top - range.ymin)/range.yruling);
    for(; ii <= lastline; ++ii) {
      if(bbox.left < range.lstop(ii) || bbox.right > range.rstop(ii))
        return false;
    }
    return true;
  }

  if(bbox.height() < 1.75*range.yruling) {
    int dl = floor((range.frame.toLocal(s->com()).y - range.ymin)/range.yruling);
    return dl >= 0 && dl < range.nlines() && bbox.left >= range.lstop(dl) && bbox.right <= range.rstop(dl);
  }
  if(bbox.top < range.ymin - 0.25*range.yruling || bbox.bottom > range.ymax + 0.25*range.yruling)
    return false;

  const Path2D& path = *static_cast<SvgPath*>(s->node)->path();
  PathPointIter pts(path, range.frame.localTransform() * s->node->totalTransform(), range.yruling/8);
  while(pts.hasNext()) {
    Point p = pts.next();
    if(p.y < range.ymin || p.y >= range.ymax) {
      // require that stroke extend at least 25% of the way through line to be considered on it
      if(p.y < range.ymin - 0.25*range.yruling || p.y > range.ymax + 0.25*range.yruling)
        return false;
    }
    else {
      int dl = floor((p.y - range.ymin)/range.yruling);
      if(p.x < range.lstop(dl) || p.x > range.rstop(dl))
        return false;
    }
  }
  return true;
}

static bool overlapRuled(const RuledRange& range, Element* s)
{
  if(s->isMultiStroke()) {
    for(Element* ss : s->children())
      if(overlapRuled(range, ss))
        return true;
    return false;
  }
  Rect bbox = localElementBBox(range.frame, s);
  if(!s->isPathElement()) {
    if(bbox.bottom < range.ymin || bbox.top >= range.ymax)
      return false;
    int l0 = int(floor((bbox.top - range.ymin)/range.yruling));
    int l1 = int(floor((bbox.bottom - range.ymin)/range.yruling));
    if(s->node->type() == SvgNode::IMAGE)
      return (l0 >= 0 && l0 < range.nlines() && bbox.left < range.rstop(l0) && bbox.right > range.lstop(l0))
          || (l1 >= 0 && l1 < range.nlines() && bbox.left < range.rstop(l1) && bbox.right > range.lstop(l1));
    // this would apply to text and opaque <g>s
    int lastline = std::min(l1, range.nlines() - 1);
    for(int ii = std::max(0, l0); ii <= lastline; ++ii) {
      if(bbox.left < range.rstop(ii) && bbox.right > range.lstop(ii))
        return true;
    }
    return false;
  }

  if(bbox.height() < 1.75*range.yruling) {
    int dl = floor((range.frame.toLocal(s->com()).y - range.ymin)/range.yruling);
    return dl >= 0 && dl < range.nlines() && bbox.left < range.rstop(dl) && bbox.right > range.lstop(dl);
  }
  if(bbox.bottom < range.ymin + 0.25*range.yruling || bbox.top > range.ymax - 0.25*range.yruling)
    return false;

  const Path2D& path = *static_cast<SvgPath*>(s->node)->path();
  PathPointIter pts(path, range.frame.localTransform() * s->node->totalTransform(), range.yruling/8);
  while(pts.hasNext()) {
    Point p = pts.next();
    if(p.y >= range.ymin && p.y < range.ymax) {
      int dl = floor((p.y - range.ymin)/range.yruling);
      if(p.x < range.rstop(dl) && p.x > range.lstop(dl))
        return true;
    }
  }
  return false;
}

bool RuledSelector::selectHit(Element* s)
{
  // ink belongs to the ruling it was written on: a ruled gesture in a region acts on that region's ink
  //  only, and one on the page's ruling leaves every region's ink alone
  if(sameRulingOnly && selRange.frame.region != selection->page->regionAt(s->com()))
    return false;
  switch(selMode) {
  case SEL_CONTAINED:
    return containedRuled(selRange, s);
  case SEL_OVERLAP:
    return overlapRuled(selRange, s);
  }
  return false;
}

Rect RuledSelector::getBGBBox()
{
  const RulingFrame& f = selRange.frame;
  if(selRange.isSingleLine())
    return f.pageBBox(Rect::ltrb(selRange.x0, selRange.ymin, selRange.x1, selRange.ymax));
  Rect extent = selection->page->frameExtent(f);
  return f.pageBBox(Rect::ltrb(extent.left, selRange.ymin, extent.right, selRange.ymax));
}

void RuledSelector::selectRuled(Dim x0, int line0, Dim x1, int line1)
{
  // always use x0,line0 as origin for findStops, even if it come after x1,line1
  if(line0 != line1)
    findStops(x0, line0);
  // see if we need to swap selection limits
  if(line0 > line1 || (line0 == line1 && x0 > x1)) {
    selectRuled(x1, line1, x0, line0);
    return;
  }

  const RulingFrame& f = selection->ruling;
  selRange = RuledRange(x0, f.yForLine(line0, Page::BLANK_Y_RULING), x1, f.yForLine(line1 + 1, Page::BLANK_Y_RULING),
      f.xRuling, f.yrulingOr(Page::BLANK_Y_RULING), f);
  // if stops are present, add to range
  if(line0 >= 0 && line1 >= 0 && line0 < int(lstops.size())) {
    if(line1 >= int(rstops.size())) {
      line1 = rstops.size() - 1;
      x1 = rstops[line1];
    }
    selRange.lstops.push_back(MAX(lstops[line0], x0));
    for(int line = line0; line < line1; ) {
      selRange.rstops.push_back(rstops[line]);
      selRange.lstops.push_back(lstops[++line]);
    }
    selRange.rstops.push_back(MIN(rstops[line1], x1));
  }

  selection->doSelect();
  bgDirty = true;
}

void RuledSelector::selectRuled(const RuledRange& range)
{
  selRange = range;
  selection->doSelect();
  bgDirty = true;
}

// select all strokes to the right of x on rule line linenum, down to the end of the page
void RuledSelector::selectRuledAfter(Dim x, int linenum)
{
  selectRuled(x, linenum, MAX_DIM, MAX_LINE_NUM - 1);
}

// find column bounds for specified point
// must cover entire page because selection could be moved up (i.e. negative insert space)
// only run once, when ruled selection is created (should be fine as long as ruled selection gets converted
//  to rect selection on release)
void RuledSelector::findStops(Dim x, int linenum)
{
  Page* page = selection->page;
  const RulingFrame& f = selection->ruling;
  // a region has no margin
  Dim marginLeft = f.region ? MIN_DIM : page->props.marginLeft;
  // preserve old behavior (applying to entire line) when cursor down in left margin and don't run twice
  // also, disable stops for eraser (SELMODE_UNION)
  if(colMode == COL_NONE || selection->selMode == Selection::SELMODE_UNION
      || x < marginLeft || !lstops.empty())
    return;

  // everything here is in the frame's local coordinates; for the page's own ruling that is the page
  Rect margins = page->frameExtent(f);
  const Dim yruling = f.yrulingOr(Page::BLANK_Y_RULING);
  // lstops/rstops are indexed by line number from 0, so lines above the frame's origin (only possible
  //  for a region whose ruling was dragged down inside it) cannot take stops
  //  (the page's own table keeps its old size, page height over pitch, whatever yRuleOffset is)
  const int nlines = f.region ? int(margins.bottom/yruling) : int(page->height()/yruling);
  if(nlines < 1) return;
  lstops.resize(nlines, margins.left);
  rstops.resize(nlines, margins.right);
  auto localLine = [&](Dim localy) { return int(std::floor(localy/yruling)); };
  const Transform2D localtf = f.localTransform();
  // only want to scan stroke list once
  for(Element* s : selection->sourceNode->children()) {
    // a region is not a barrier, and ink on another ruling is not in this one's columns
    if(s->isRulingRegion() || page->regionAt(s->com()) != f.region)
      continue;
    Rect bbox = localElementBBox(f, s);
    if(bbox.height() > MIN_DIV_HEIGHT*yruling) {
      // conservative mode requires that stroke passes through linenum
      if((colMode == COL_NORMAL)
          && (localLine(bbox.top) > linenum || localLine(bbox.bottom) < linenum))
        continue;  // go to next stroke
      // require stroke extend at least 1/4 of the way into line to form a barrier
      int line0 = MAX(0, localLine(bbox.top + 0.25*yruling));
      int line1 = MIN((int)lstops.size()-1, localLine(bbox.bottom - 0.25*yruling));
      for(int ll = line0; ll <= line1; ll++) {
        if(!s->isPathElement()) {
          if(bbox.left > x)
            rstops[ll] = MIN(bbox.left, rstops[ll]);
          else if(bbox.right < x)
            lstops[ll] = MAX(bbox.right, lstops[ll]);
          else
            break;  // go to next stroke
        }
        // before processing each point of stroke, check bbox to see if it can move bounds
        else if(bbox.left < rstops[ll] || bbox.right > lstops[ll]) {
          const Path2D& path = *static_cast<SvgPath*>(s->node)->path();
          PathPointIter pts(path, localtf * s->node->totalTransform(), yruling/8);
          while(pts.hasNext()) {
            Point p = pts.next();
            int pline = localLine(p.y);
            if(pline >= line0 && pline <= line1) {
              if(p.x > x)
                rstops[pline] = MIN(p.x, rstops[pline]);
              else if(p.x < x)
                lstops[pline] = MAX(p.x, lstops[pline]);
            }
          }
          break;  // stroke has been completely processed - go to next stroke
        }
      }
    }
  }
  // post processing - clear all stops below a line w/o stops
  // TODO: we may want to set stops so nothing below end of column is selected!
  bool clearstops = false;
  for(int ll = std::max(0, linenum); ll < int(lstops.size()); ll++) {
    if(clearstops) {
      lstops[ll] = margins.left;
      rstops[ll] = margins.right;
    }
    else if(lstops[ll] == margins.left && rstops[ll] == margins.right)
      clearstops = true;
  }
}

void RuledSelector::shrink()
{
  // not used since selections converted to rect sel, but if we did impl ...
  // ... sort stroke list, use first and last strokes to determine selection range
}

// Lasso selection

LassoSelector::LassoSelector(Selection* sel, Dim simplify) : Selector(sel), simplifyThresh(simplify)
{
  lasso.setFillRule(Path2D::EvenOddFill);  // just because this was the previous behavior
}

void LassoSelector::addPoint(Dim x, Dim y)
{
  // we store lasso as a closed path - last element is same as first
  if(lasso.size() > 0)
    lasso.points.pop_back();
  // calculate normals of the triangle defining the changed region of the selection
  if(lasso.size() > 1 && selection->selMode != Selection::SELMODE_NONE) {
    Dim sa, sb;
    Point a = lasso.points.front();
    Point b = lasso.points.back();
    norm1 = Point(a.y - y, x - a.x).normalize();
    norm2 = Point(b.y - y, x - b.x).normalize();
    norm3 = Point(a.y - b.y, b.x - a.x).normalize();

    // compute range of triangle along it's normals; sc := sanity check (ha ha)
    sa = norm1.x*x + norm1.y*y;  sb = norm1.x*b.x + norm1.y*b.y;  //sc = norm1.x*a.x + norm1.y*a.y;
    s1min = MIN(sa, sb);  s1max = MAX(sa, sb);
    sa = norm2.x*x + norm2.y*y;  sb = norm2.x*a.x + norm2.y*a.y;  //sc = norm2.x*b.x + norm2.y*b.y;
    s2min = MIN(sa, sb);  s2max = MAX(sa, sb);
    sa = norm3.x*a.x + norm3.y*a.y;  sb = norm3.x*x + norm3.y*y;  //sc = norm3.x*b.x + norm3.y*b.y;
    s3min = MIN(sa, sb);  s3max = MAX(sa, sb);

    // compute range of triangle along axes (which are bbox normals)
    txmin = MIN(x, MIN(a.x, b.x));
    txmax = MAX(x, MAX(a.x, b.x));
    tymin = MIN(y, MIN(a.y, b.y));
    tymax = MAX(y, MAX(a.y, b.y));

    checkCollision = !norm1.isZero() && !norm2.isZero() && !norm3.isZero();

    lasso.addPoint(x, y);
    // idea here is to process path in chunks (since selection is live, we have to simplify live to get any
    //  benefit) - we keep collecting points until at least one point (since last simplification) exceeds
    //  threshold, then simplify the path between 2nd to last point of prev simplication (not last point!)
    //  and current end point
    std::vector<Point> simp = simplifyRDP(lasso.points, nextSimpStart, lasso.size() - 1, simplifyThresh);
    if(simp.size() > 2) {
      lasso.points.erase(std::copy(simp.begin(), simp.end(), lasso.points.begin() + nextSimpStart), lasso.points.end());
      nextSimpStart = lasso.size() - 2;
    }
  }
  else
    lasso.addPoint(x, y);

  lasso.closeSubpath();
  lassoBBox.rectUnion(Point(x, y));
  selection->doSelect();
  bgDirty = true;
  checkCollision = false;
}

// Strokes contained entirely within selection region are "hits."  Two approaches:
// 1. even-odd rule: count the number of times a line segment from each point to infinity crosses the
//  the selection boundary.  If the number of crossings is even, the point and thus the stroke is rejected
// Counting crossings is accomplished by iterating over all line segments of the lasso
// 2. non-zero rule: determine the winding number of the lasso around each point.  This is always a multiple
//  of 2*pi for a closed path.  The point is enclosed iff the winding != 0.  We only have to keep track of
//  quadrants, not the exact angle:
// 2|3
// -+->x
// 1|0
//  v y
// We use the even-odd rule for lasso selection, mostly for legacy reasons (not to say non-zero rule is better)

// Rejected strokes will usually get rejected quickly, but normally we'd have to iterate over every point
//  of a stroke to accept it.  We've implemented the following optimization: in addPoint, we determine the
//  triangle representing the changed region of the lasso.  If a stroke's bbox does not intersect this
//  triangle, the stroke's selection state is unchanged.  Since both the triangle and the bbox are convex, we
//  check for itersection by projecting both shapes onto directions normal to an edge.  These are just the
//  x and y axes for the bbox.  The triangle normals can be in any direction, which is the main source of
//  complication here.  If the projections along any of the directions are disjoint, there is no intersection
// An alternate optimization is to check if lasso passes through stroke bbox; if not, we only have to test
//  one point of stroke.  We tried doing this by checking for lasso points inside bbox - this is incorrect,
//  especially with the long line segment closing the lasso.
// We we're also unnecessarily checking to see if each point is inside lasso bbox - this is always true if
//  the stroke bbox is inside lasso bbox

bool LassoSelector::projectRect(const Point& p, const Rect& r, Dim smin, Dim smax)
{
  Dim pr0 = p.x*r.left + p.y*r.top;
  Dim pr1 = p.x*r.left + p.y*r.bottom;
  Dim pr2 = p.x*r.right + p.y*r.top;
  Dim pr3 = p.x*r.right + p.y*r.bottom;
  Dim pmin = MIN(MIN(pr0, pr1), MIN(pr2, pr3));
  Dim pmax = MAX(MAX(pr0, pr1), MAX(pr2, pr3));
  return pmin < smax && pmax > smin;  // overlap?
}

bool LassoSelector::rectCollide(const Rect& r)
{
  // project triangle onto rect normals (i.e., the x and y axes)
  if(txmin > r.right || txmax < r.left || tymin > r.bottom || tymax < r.top)
    return false;  // no collision
  // project rectangle onto triangle normals
  return projectRect(norm1, r, s1min, s1max)
      && projectRect(norm2, r, s2min, s2max) && projectRect(norm3, r, s3min, s3max);
}

// for now, we will just just PainterPath::isEnclosedBy which only considers path points, and so may
//  incorrectly include some paths w/ parts of segments outside a concave lasso - if this turns out to be
//  a real issue, the easiest solution is probably to just use PathPointIter for subject path (easy since
//  subject path points are iterated over in outer loop)
static bool isEnclosedBy(const Path2D& lasso, SvgNode* node)
{
  if(node->asContainerNode()) {
    for(SvgNode* child : node->asContainerNode()->children())
      if(!isEnclosedBy(lasso, child))
        return false;
    return true;
  }
  if(node->type() == SvgNode::PATH) {
    const Path2D* path = static_cast<SvgPath*>(node)->path();
    Transform2D tf = node->totalTransform();
    return tf.isIdentity() ? path->isEnclosedBy(lasso) : Path2D(*path).transform(tf).isEnclosedBy(lasso);
  }
  else
    return Path2D().addRect(node->bounds()).isEnclosedBy(lasso);
}

static bool segmentsCross(Point a, Point b, Point c, Point d)
{
  auto side = [](Point p, Point q, Point r) { return (q.x - p.x)*(r.y - p.y) - (q.y - p.y)*(r.x - p.x); };
  Dim d1 = side(c, d, a), d2 = side(c, d, b), d3 = side(a, b, c), d4 = side(a, b, d);
  return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0));
}

// with no crossing between an outline and the lasso, each subpath of the outline is entirely inside or
//  entirely outside, so one point per subpath decides it - and the lasso may instead be entirely inside
//  an image or text box
static bool touchesLasso(const Path2D& lasso, const Rect& lassoBBox, SvgNode* node)
{
  if(node->asContainerNode()) {
    for(SvgNode* child : node->asContainerNode()->children())
      if(touchesLasso(lasso, lassoBBox, child))
        return true;
    return false;
  }
  Path2D outline = flatOutline(node);
  for(int ii = 0; ii < outline.size(); ++ii) {
    Point p = outline.point(ii);
    if(ii == 0 || outline.command(ii) == Path2D::MoveTo) {
      Path2D single;
      single.addPoint(p);
      if(single.isEnclosedBy(lasso))
        return true;
      continue;
    }
    Point prev = outline.point(ii-1);
    if(!lassoBBox.overlaps(Rect::corners(prev, p)))
      continue;
    for(int jj = 1; jj < lasso.size(); ++jj)
      if(segmentsCross(prev, p, lasso.point(jj-1), lasso.point(jj)))
        return true;
  }
  return node->type() != SvgNode::PATH && node->bounds().contains(lasso.point(0));
}

// even-odd rule approach - we use a horizontal line from the stroke point to x = +infinity
bool LassoSelector::selectHit(Element* s)
{
  // reject if lasso bbox does not contain (or, touching, overlap) stroke bbox
  if(lasso.size() < 1 || !(touching ? lassoBBox.overlaps(s->bbox()) : lassoBBox.contains(s->bbox())))
    return false;
  // still valid when touching: the lasso's region only changed inside the triangle
  if(checkCollision && !rectCollide(s->bbox())) {
    //SCRIBBLE_LOG("Quick accept %f %f %f %f", txmin, tymin, txmax, tymax);
    return s->isSelected(selection);  // no change in selection state
  }
  return touching ? touchesLasso(lasso, lassoBBox, s->node) : isEnclosedBy(lasso, s->node);
}

Rect LassoSelector::getBGBBox()
{
  return lassoBBox.isValid() ? selection->transform.mult(lassoBBox) : lassoBBox;
}

// Draw selection background - for now a semi-transparent region on top of strokes;
//  selected strokes are drawn in order by Layer object (not Selection as before)

void Selection::drawBG(Painter* painter)
{
  if(selector) //&& drawType() == STROKEDRAW_SEL)
    selector->drawBG(painter);
}

void PathSelector::drawBG(Painter* painter)
{
  if(path.points.size() < 1)
    return;
  painter->save();
  painter->translate(selection->transform.xoffset(), selection->transform.yoffset());
  painter->setFillBrush(Color::NONE);
  painter->setStroke(bgFill, selRadius*2, Painter::RoundCap, Painter::RoundJoin);
  painter->drawPath(path);
  painter->restore();
}

void LassoSelector::drawBG(Painter* painter)
{
  if(lasso.points.size() < 1)
    return;
  painter->save();
  painter->translate(selection->transform.xoffset(), selection->transform.yoffset());
  painter->setFillBrush(bgFill);
  // dashed border looks better, but is much, much slower, on both Qt and Android
  //painter->setDashCount(1);
  painter->setStroke(bgStroke, 1, Painter::SquareCap, Painter::BevelJoin);
  painter->setVectorEffect(Painter::NonScalingStroke);
  painter->drawPath(lasso);
  //painter->setDashCount(0);
  painter->restore();
}

void RuledSelector::drawBG(Painter* painter)
{
  if(!selRange.isValid())
    return;
  painter->save();
  painter->setFillBrush(bgFill);
  painter->setStroke(bgStroke, 0);
  // note that we're assuming ruled sel doesn't coexist with scaling
  painter->translate(selection->transform.xoffset(), selection->transform.yoffset());
  // the staircase is built in the frame's local coordinates; only a tilted one wants antialiasing
  painter->transform(selRange.frame.pageTransform());
  bool antialias = painter->setAntiAlias(selRange.frame.isRotated());

  Rect extent = selection->page->frameExtent(selRange.frame);
  Path2D path;
  int nlines = selRange.nlines();
  Dim x;
  Dim y = selRange.ymin;
  int ii = 0;
  for(; ii < nlines; ++ii) {
    x = MIN(extent.right, MAX(extent.left, selRange.lstop(ii)));
    path.addPoint(x, y);
    y += selRange.yruling;
    path.addPoint(x, y);
  }
  for(--ii; ii >= 0; --ii) {
    x = MIN(extent.right, MAX(extent.left, selRange.rstop(ii)));
    path.addPoint(x, y);
    y -= selRange.yruling;
    path.addPoint(x, y);
  }
  // close path
  path.closeSubpath();
  painter->drawPath(path);
  painter->setAntiAlias(antialias);
  painter->restore();
}

void RectSelector::drawBG(Painter* painter)
{
  if(!selRect.isValid())
    return;
  Dim a = HANDLE_SIZE/mZoom;
  Rect handlerect = Rect::ltrb(-a, -a, a, a);
  Rect r = selRect;
  painter->save();
  painter->setFillBrush(bgFill);
  painter->setStrokeBrush(bgStroke);
  painter->setStrokeWidth(1.0/mZoom);
  if(selection->transform.isRotating())
    painter->transform(selection->transform);

  // Rect sel looks much better w/o antialiasing
  painter->drawRect(r);
  if(drawHandles) {
    // draw resize handles on top of border
    painter->fillRect(Rect(handlerect).translate(r.left, r.top), Color::BLACK);
    painter->fillRect(Rect(handlerect).translate(r.right, r.top), Color::BLACK);
    painter->fillRect(Rect(handlerect).translate(r.left, r.bottom), Color::BLACK);
    painter->fillRect(Rect(handlerect).translate(r.right, r.bottom), Color::BLACK);
    // draw rotation handle
    painter->setAntiAlias(true);
    painter->setFillBrush(Color::NONE);
    //painter->setPen(bgStroke, 0);
    Path2D rh;
    rh.moveTo(r.center().x, r.top);
    rh.lineTo(r.center().x, r.top - 5*a);
    rh.addEllipse(r.center().x, r.top - 6*a, a, a);
    painter->drawPath(rh);
    // cropping handles
    if(enableCrop) {
      painter->setFillBrush(Color::RED);
      painter->setStrokeBrush(Color::NONE);
      painter->drawPath(Path2D().addEllipse(r.left, r.center().y, a, a));
      painter->drawPath(Path2D().addEllipse(r.right, r.center().y, a, a));
      painter->drawPath(Path2D().addEllipse(r.center().x, r.top, a, a));
      painter->drawPath(Path2D().addEllipse(r.center().x, r.bottom, a, a));
    }
  }
  painter->restore();
}

// ShapeSelector - editing handles for a single parametric shape (SHAPES_SPEC.md 5)

Dim ShapeSelector::HANDLE_SIZE = 4;

Element* ShapeSelector::shapeElement() const
{
  if(selection->count() != 1)
    return NULL;
  Element* s = selection->strokes.front();
  return s->isShape() ? s : NULL;
}

// descriptor points are node-local; everything the user interacts with is in page coordinates
static Transform2D shapePageTransform(const Selection* sel, const Element* s)
{
  return sel->transform.tf() * s->node->getTransform();
}

Point ShapeSelector::toLocal(Point pagepos) const
{
  Element* s = shapeElement();
  return s ? shapePageTransform(selection, s).inverse().map(pagepos) : pagepos;
}

void ShapeSelector::updateHandles()
{
  m_handles.clear();
  selRect = Rect();
  Element* s = shapeElement();
  if(!s)
    return;
  getShapeHandles(s->shapeParams(), m_handles);
  Transform2D tf = shapePageTransform(selection, s);
  for(ShapeHandle& h : m_handles) {
    h.pos = tf.map(h.pos);
    selRect.rectUnion(h.pos);
  }
  selRect.rectUnion(s->bbox());
}

bool ShapeSelector::selectHit(Element* s)
{
  // a shape is grabbable anywhere in its bounding box, as a rect selection is
  return selRect.isValid() && selRect.contains(s->bbox());
}

void ShapeSelector::shrink()
{
  updateHandles();
}

void ShapeSelector::transform(const Transform2D& tf)
{
  updateHandles();
}

Rect ShapeSelector::getBGBBox()
{
  if(!selRect.isValid())
    return selRect;
  return Rect(selRect).pad(2*HANDLE_SIZE/mZoom);
}

int ShapeSelector::shapeHandleHit(Point pos, bool touch)
{
  if(!drawHandles)
    return -1;
  Dim h = touch ? 2*HANDLE_SIZE : HANDLE_SIZE;
  Dim a = (h + 3)/mZoom;  // +3 is to make it easier to grab a handle, as for RectSelector
  Rect handlerect = Rect::ltrb(-a, -a, a, a);
  // last handle wins, so the radius handle (added last) stays reachable when it sits on a corner
  for(int ii = int(m_handles.size()); ii-- > 0;) {
    if(Rect(handlerect).translate(m_handles[ii].pos.x, m_handles[ii].pos.y).contains(pos))
      return ii;
  }
  return -1;
}

void ShapeSelector::drawBG(Painter* painter)
{
  if(m_handles.empty())
    return;
  Dim a = HANDLE_SIZE/mZoom;
  Rect handlerect = Rect::ltrb(-a, -a, a, a);
  painter->save();
  painter->setFillBrush(bgFill);
  painter->setStrokeBrush(bgStroke);
  painter->setStrokeWidth(1.0/mZoom);
  if(drawHandles) {
    painter->setAntiAlias(true);
    for(const ShapeHandle& handle : m_handles) {
      if(handle.type == ShapeHandle::RADIUS || handle.type == ShapeHandle::TIGHTNESS) {
        // Parameter handles are red so they are not mistaken for the point handles.  They are drawn as
        //  ellipses rather than rects too, but do not rely on that to tell them apart: at the ~6 px a
        //  handle actually occupies on screen the two shapes are indistinguishable - the colour is
        //  what carries the distinction.
        painter->setFillBrush(Color::RED);
        painter->setStrokeBrush(Color::NONE);
        painter->drawPath(Path2D().addEllipse(handle.pos.x, handle.pos.y, a, a));
      }
      else
        painter->fillRect(Rect(handlerect).translate(handle.pos.x, handle.pos.y), Color::BLACK);
    }
  }
  painter->restore();
}

// RegionSelector

Dim RegionSelector::HANDLE_SIZE = 4;

bool RegionSelector::carries(const RulingRegionParams& params, Element* s)
{
  return !s->isRulingRegion() && params.contains(s->bbox().center());
}

// used by ScribbleArea::selectionHit() to decide whether a press starts a move: anywhere inside the outline
bool RegionSelector::selectHit(Element* s)
{
  return s == region || region->regionParams().contains(s->bbox().center());
}

// the local bounding box of the outline, in the region's own frame
static Rect regionLocalBBox(const RulingRegionParams& params)
{
  RulingFrame f = params.frame();
  Rect r;
  for(const Point& p : params.corners)
    r.rectUnion(f.toLocal(p));
  return r;
}

Point RegionSelector::rotHandlePos() const
{
  const RulingRegionParams& params = region->regionParams();
  RulingFrame f = params.frame();
  Rect r = regionLocalBBox(params);
  // above the middle of the top edge, as seen in the region's frame, so it turns with the region
  return f.toPage(Point(r.center().x, r.top - 6*HANDLE_SIZE/mZoom));
}

Point RegionSelector::scaleHandlePos() const
{
  const RulingRegionParams& params = region->regionParams();
  RulingFrame f = params.frame();
  Rect r = regionLocalBBox(params);
  // just past the bottom-right corner, clear of the "..." button that sits inside it
  Dim d = 4*HANDLE_SIZE/mZoom;
  return f.toPage(Point(r.right + d, r.bottom + d));
}

// the origin handle sits on the left edge, on the first line inside the region, and drags the lines
//  - except on a coordinate system, where it is the plot's (0, 0) itself (kept inside the outline)
static Point defaultOriginHandlePos(const RulingRegionParams& params)
{
  if(params.axes)
    return params.clampInside(params.origin);
  RulingFrame f = params.frame();
  Rect r = regionLocalBBox(params);
  Dim yr = f.yrulingOr(Page::BLANK_Y_RULING);
  Dim firstLine = (std::floor(r.top/yr) + 1)*yr;
  return f.toPage(Point(r.left, firstLine < r.bottom ? firstLine : r.top));
}

Point RegionSelector::originHandlePos() const
{
  const RulingRegionParams& params = region->regionParams();
  // a relocated handle stays where the user put it, but never outside the outline (a corner drag or a
  //  peer's edit may have moved the outline away from it).  A coordinate system's handle is its origin,
  //  so it is never relocated (no grip either): a handle that was not the origin would plot nothing.
  return handleMoved && !params.axes ? params.clampInside(handlePage) : defaultOriginHandlePos(params);
}

Point RegionSelector::moveGripPos() const
{
  // a small grip up and to the right of the handle, in screen units
  Dim d = 3.2*(HANDLE_SIZE + 3)/mZoom;
  return originHandlePos() + Point(d, -d);
}

Rect RegionSelector::getBGBBox()
{
  Rect r = region->regionParams().bounds();
  r.rectUnion(rotHandlePos());
  r.rectUnion(scaleHandlePos());
  r.rectUnion(originHandlePos());
  r.rectUnion(moveGripPos());
  return r.pad(2*(HANDLE_SIZE + 3)/mZoom);
}

int RegionSelector::shapeHandleHit(Point pos, bool touch)
{
  const RulingRegionParams& params = region->regionParams();
  Dim a = ((touch ? 2*HANDLE_SIZE : HANDLE_SIZE) + 3)/mZoom;
  // the origin handle wins over a corner it happens to sit on: a corner can also be reached by
  //  dragging along its edges, the origin cannot
  if((pos - originHandlePos()).dist() <= a*1.2)
    return originHandleIndex();
  if(!params.axes && (pos - moveGripPos()).dist() <= a*1.2)
    return moveGripIndex();
  if((pos - scaleHandlePos()).dist() <= a*1.2)
    return resizeHandleIndex();
  for(int ii = int(params.corners.size()); ii-- > 0;) {
    if(std::abs(pos.x - params.corners[ii].x) <= a && std::abs(pos.y - params.corners[ii].y) <= a)
      return ii;
  }
  return -1;
}

Point RegionSelector::rotHandleHit(Point pos, bool touch)
{
  Dim a = ((touch ? 2*HANDLE_SIZE : HANDLE_SIZE) + 3)/mZoom;
  if((pos - rotHandlePos()).dist() <= a*1.2) {
    // rotate about the centre of the outline
    Rect r = regionLocalBBox(region->regionParams());
    return region->regionParams().frame().toPage(r.center());
  }
  return Point(NaN, NaN);
}

// A patch is a piece of paper: making it bigger gives more paper, not bigger lines.  So the size handle
//  moves the outline only - spacing is the panel's slider - and, since the lines stay put, the ink on
//  them stays put too.  Scaling ink with lines that did not scale would lift it off them.  Being free
//  of the ruling, the size need not be uniform either.
RulingRegionParams RegionSelector::resized(const RulingRegionParams& start, Point startPos, Point pos) const
{
  RulingRegionParams params = start;
  RulingFrame f = start.frame();
  Rect r = regionLocalBBox(start);
  if(r.width() <= 0 || r.height() <= 0)
    return params;
  Point d = f.toLocalDir(pos - startPos);
  Dim minsize = 4*HANDLE_SIZE/mZoom;
  Dim sx = std::max(r.width() + d.x, minsize)/r.width();
  Dim sy = std::max(r.height() + d.y, minsize)/r.height();
  Point anchor(r.left, r.top);
  for(Point& p : params.corners) {
    Point local = f.toLocal(p) - anchor;
    p = f.toPage(anchor + Point(local.x*sx, local.y*sy));
  }
  return params;
}

void RegionSelector::drawBG(Painter* painter)
{
  const RulingRegionParams& params = region->regionParams();
  if(!params.isValid())
    return;
  Dim a = HANDLE_SIZE/mZoom;
  painter->save();
  painter->setAntiAlias(true);
  // the outline, so a region with no paper is still visible as a thing while selected
  painter->setFillBrush(Color::NONE);
  painter->setStroke(bgStroke, 1.5/mZoom);
  painter->drawPath(params.outlinePath());
  // stem to the rotate handle, from the middle of the top edge
  RulingFrame f = params.frame();
  Rect r = regionLocalBBox(params);
  painter->drawLine(f.toPage(Point(r.center().x, r.top)), rotHandlePos());
  // corner handles (reshape only): black squares, as for a shape's point handles
  for(const Point& p : params.corners)
    painter->fillRect(Rect::centerwh(p, 2*a, 2*a), Color::BLACK);
  // the origin handle moves a number (the lines' phase), so it is red like the shape parameter handles
  Point o = originHandlePos();
  painter->setStrokeBrush(Color::NONE);
  painter->setFillBrush(Color::RED);
  painter->drawPath(Path2D().addEllipse(o.x, o.y, a, a));
  // the grip that relocates it (no parameter changes): small hollow circle tied to the handle by a stem
  if(!params.axes) {
    Point grip = moveGripPos();
    painter->setStroke(bgStroke, 1/mZoom);
    painter->drawLine(o, grip);
    painter->setFillBrush(Color::WHITE);
    painter->setStroke(Color::BLACK, 1/mZoom);
    painter->drawPath(Path2D().addEllipse(grip.x, grip.y, a, a));
  }
  // rotate and scale: hollow circles, which RectSelector's handles also are
  painter->setFillBrush(Color::WHITE);
  painter->setStroke(Color::BLACK, 1/mZoom);
  Point rh = rotHandlePos(), sh = scaleHandlePos();
  painter->drawPath(Path2D().addEllipse(rh.x, rh.y, 1.5*a, 1.5*a));
  painter->drawPath(Path2D().addEllipse(sh.x, sh.y, 1.5*a, 1.5*a));
  painter->restore();
  bgDirty = false;
}
