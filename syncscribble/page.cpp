#include "pugixml.hpp"
#include "basics.h"
#include "document.h"
#include "strokebuilder.h"
#include "usvg/svgparser.h"


Dim Page::BLANK_Y_RULING = 40;
bool Page::enableDropShadow = true;

// for legacy support (esp. ScribbleTest); note that we force paper to be opaque
PageProperties::PageProperties(Dim w, Dim h, Dim xr, Dim yr, Dim ml, Color c, Color rc, Dim dr)
    : width(w), height(h), color(c.opaque()), xRuling(xr), yRuling(yr), marginLeft(ml), ruleColor(rc),
      dotRadius(dr) {}

void Page::initDoc()
{
  contentNode = new SvgG;
  svgDoc->addChild(contentNode);
  new Element(svgDoc.get());
  new Element(contentNode);
  contentNode->addClass("write-content");
  svgDoc->addClass("write-page");
}

// constructor for page to be loaded
Page::Page(Dim w, Dim h, int idx) : props(w, h), svgDoc(new SvgDocument(0, 0, w, h)), blockIdx(idx)
{
  initDoc();
}

// blank page constructor
// note that we don't have a Page(SvgDocument*) constructor due to delay load feature
Page::Page(const PageProperties& _props, const SvgContainerNode* ruling) : props(_props), svgDoc(new SvgDocument)
{
  loadStatus = LOAD_OK;
  isCustomRuling = ruling != NULL;
  ruleNode = ruling ? ruling->clone() : new SvgG;  // replaced immediately if not custom
  svgDoc->addChild(ruleNode);
  initDoc();
  onPageSizeChange();
}

Range<ElementIter> Page::children() const
{
  return static_cast<Element*>(contentNode->ext())->children();
}

PageProperties Page::getProperties()
{
  ensureLoaded();
  return props;
}

bool Page::setProperties(const PageProperties* newprops)
{
  ensureLoaded();
  if(document->history->undoable())
    document->history->addItem(new PageChangedItem(this));
  Dim oldwidth = props.width, oldheight = props.height;
  props = *newprops;
  // require positive dimensions
  if(props.width <= 0) props.width = oldwidth;
  if(props.height <= 0) props.height = oldheight;
  onPageSizeChange();
  Rect r = getBBox();
  return (r.right > props.width && r.right < oldwidth) || (r.bottom > props.height && r.bottom < oldheight);
}

int Page::getPageNum() const
{
  // lookup page number; this should be made a method of Document
  for(unsigned int pp = 0; pp < document->pages.size(); pp++) {
    if(document->pages[pp] == this)
      return pp;
  }
  return -1;  // should never happen
}

int Page::getLine(Dim y) const
{
  return (int)floor((y - yRuleOffset)/yruling(true));
}

int Page::getLine(const Element* s) const
{
  return getLine(s->com().y);
}

std::function<bool(const Element* a, const Element* b)> Page::cmpRuled()
{
  return [this](const Element* a, const Element* b)
  {
    int linea = getLine(a);
    int lineb = getLine(b);
    if(linea == lineb)
      return a->bbox().left < b->bbox().left;
    else
      return linea < lineb;
  };
}

Dim Page::getYforLine(int line) const
{
  return yRuleOffset + yruling(true)*line;
}

Dim Page::yruling(bool usedefault) const
{
  return props.yRuling > 0 ? props.yRuling : (usedefault ? BLANK_Y_RULING : 0);
}

RulingFrame Page::pageFrame() const
{
  return RulingFrame(Point(0, yRuleOffset), 0, props.xRuling, props.yRuling, props.dotRadius);
}

std::vector<Element*> Page::regions() const
{
  std::vector<Element*> result;
  if(!contentNode || !contentNode->hasExt())
    return result;
  // regions are kept first in the page (REGION_LAYER sorts below every layer), so stop at the first
  //  element that is not one
  for(Element* s : children()) {
    if(!s->isRulingRegion())
      break;
    result.push_back(s);
  }
  return result;
}

Element* Page::regionAt(Point pos) const
{
  Element* hit = NULL;
  for(Element* s : regions()) {
    // later regions draw over earlier ones, so the last one containing pos is the one you see
    Point local = s->node->hasTransform() ? s->node->getTransform().inverse().map(pos) : pos;
    if(s->regionParams().contains(local))
      hit = s;
  }
  return hit;
}

RulingFrame Page::rulingAt(Point pos) const
{
  Element* region = regionAt(pos);
  return region ? regionFrame(region) : pageFrame();
}

Element* Page::regionNear(Point pos, Dim lines) const
{
  Element* hit = NULL;
  for(Element* s : regions()) {
    RulingFrame frame = regionFrame(s);
    Rect extent = frameExtent(frame);
    if(extent.pad(lines*frame.yrulingOr(BLANK_Y_RULING)).contains(frame.toLocal(pos)))
      hit = s;
  }
  return hit;
}

RulingFrame Page::regionFrame(Element* region) const
{
  RulingFrame frame = region->regionParams().frame(region);
  // a region carries no node transform in practice (its transforms are applied to its parameters), but
  //  honour a translate should one arrive from a hand-edited file
  if(region->node->hasTransform())
    frame.origin = region->node->getTransform().map(frame.origin);
  return frame;
}

Rect Page::frameExtent(const RulingFrame& frame) const
{
  const Element* region = static_cast<const Element*>(frame.region);
  if(!region || !region->isRulingRegion())
    return frame.localBBox(rect());
  Rect local;
  for(const Point& p : region->regionParams().corners)
    local.rectUnion(frame.toLocal(p));
  return local;
}

RulingFrame Page::gestureFrame(Point pos, Dim nearLines) const
{
  RulingFrame frame = rulingAt(pos);
  if(!frame.region && nearLines > 0) {
    if(Element* region = regionNear(pos, nearLines))
      frame = regionFrame(region);
  }
  // the page's own blank-page phase is set by the caller through yRuleOffset, which pageFrame() reads;
  //  a blank region gets the same treatment here, on its origin
  if(frame.region && frame.yRuling <= 0) {
    Dim ly = frame.toLocal(pos).y;
    Dim phase = ly - BLANK_Y_RULING/2 - BLANK_Y_RULING*std::floor((ly - BLANK_Y_RULING/2)/BLANK_Y_RULING);
    frame.origin = frame.toPage(Point(0, phase));
  }
  return frame;
}

void Page::refreshRegions()
{
  for(Element* s : regions())
    s->rebuildRegion(props.color, props.ruleColor);
}

// The problem with MAX(...) is that bbox gets updated when dragging a selection
Dim Page::height() const
{
  return scaleFactor*props.height; //MAX(height, layer0->getBBox().height());
}

Dim Page::width() const
{
  return scaleFactor*props.width; //MAX(width, layer0->getBBox().width());
}

void Page::onPageSizeChange()
{
  svgDoc->setWidth(props.width);
  svgDoc->setHeight(props.height);
  // write page props here instead of saveSVG() so that copy+paste of pages works properly
  contentNode->setAttr<float>("xruling", props.xRuling);
  contentNode->setAttr<float>("yruling", props.yRuling);
  contentNode->setAttr<float>("marginLeft", props.marginLeft);
  // only written when set, so a lined page's SVG is unchanged and older builds just see lines
  if(props.dotRadius > 0)
    contentNode->setAttr<float>("dotradius", props.dotRadius);
  else
    contentNode->removeAttr("dotradius");
  //contentNode->setAttr<color_t>("papercolor", props.color.color);
  // write colors as strings to simplify reading back when pasting pages
  contentNode->setAttr("papercolor", fstring("#%06X", props.color.rgb()).c_str());
  // this would be the only case of #AARRGGBB for serializeColor - SVG 2 spec allows rgba() but not #AARRGGBB
  contentNode->setAttr("rulecolor", fstring("#%08X", props.ruleColor.argb()).c_str());

  if(!ruleNode && props.xRuling == 0 && props.yRuling == 0 && props.marginLeft == 0) {}
  else if(!isCustomRuling)
    generateRuleLayer(props.color, props.width, props.height);
  else if(ruleNode) {
    SvgNode* pagerect = ruleNode->selectFirst(".pagerect");
    if(pagerect && pagerect->type() == SvgNode::RECT) {
      static_cast<SvgRect*>(pagerect)->setRect(Rect::wh(props.width, props.height));
      setSvgFillColor(pagerect, props.color);
    }
    ruleNode->setAttr("color", props.ruleColor.color);  // custom rulings can use currColor
  }
  yRuleOffset = 0; //yruling > 0 ? ygroup->yOrigin : 0;
  // regions take the page's paper and rule colors, so a theme change or invert reaches them too
  refreshRegions();
}

// I think perhaps we shouldn't use <pattern> for built-in rulings, since it is not included in SVG-tiny spec!
void Page::generateRuleLayer(Color pageColor, Dim w, Dim h)
{
  SvgContainerNode* oldrulenode = ruleNode;
  ruleNode = new SvgG;
  setSvgFillColor(ruleNode, Color::NONE);
  setSvgStrokeColor(ruleNode, props.ruleColor);
  ruleNode->setAttr<float>("stroke-width", 1);
  ruleNode->setAttribute("shape-rendering", "crispEdges");
  ruleNode->setAttribute("vector-effect", "non-scaling-stroke");
  ruleNode->addClass("ruleline");
  ruleNode->addClass("write-std-ruling");
  ruleNode->addClass("write-scale-down");
  // add to document
  SvgNode* before = svgDoc->children().empty() ? NULL : svgDoc->children().front();
  svgDoc->addChild(ruleNode, oldrulenode ? oldrulenode : before);
  new Element(ruleNode);  // need an Element so we can enable AA when scale < 1 in Element::applyStyle()
  // seems like something like replaceNode(old, new) might be useful...
  if(oldrulenode) {
    svgDoc->removeChild(oldrulenode);
    delete oldrulenode;
  }

  SvgNode* s = new SvgRect(Rect::wh(w, h));
  setSvgFillColor(s, pageColor);
  setSvgStrokeColor(s, Color::NONE);
  s->addClass("pagerect");
  ruleNode->addChild(s);

  if(props.dotRadius > 0 && (props.xRuling > 0 || props.yRuling > 0)) {
    // All dots are one filled path: a node per dot would be thousands of nodes on a fine grid.  With one
    //  ruling set, the dots run along its lines at a pitch of a few radii, reading as a dotted line.
    Dim r = props.dotRadius;
    Dim linePitch = std::max(4*r, Dim(4));
    Dim dx = props.xRuling > 0 ? props.xRuling : linePitch;
    Dim dy = props.yRuling > 0 ? props.yRuling : linePitch;
    Dim x0 = props.xRuling > 0 ? props.xRuling : dx/2;
    Dim y0 = props.yRuling > 0 ? props.yRuling : dy/2;
    Path2D dots;
    for(Dim doty = y0; doty < h; doty += dy) {
      for(Dim dotx = x0; dotx < w; dotx += dx)
        dots.addEllipse(dotx, doty, r, r);
    }
    s = new SvgPath(dots);
    setSvgFillColor(s, props.ruleColor);
    setSvgStrokeColor(s, Color::NONE);
    s->setAttribute("shape-rendering", "auto");  // the group's crispEdges would square the dots off
    s->addClass("dotrule");
    ruleNode->addChild(s);
  }
  else if(props.xRuling > 0 || props.yRuling > 0) {
    if(props.yRuling > 0) {
      for(Dim ruley = props.yRuling; ruley < h; ruley += props.yRuling) {
        s = new SvgPath(Path2D().addLine(Point(0, ruley), Point(w, ruley)));
        if(ruley < 2*props.yRuling)
          s->addClass("yrule_1");
        ruleNode->addChild(s);
      }
    }
    if(props.xRuling > 0) {
      for(Dim rulex = props.xRuling; rulex < w; rulex += props.xRuling) {
        s = new SvgPath(Path2D().addLine(Point(rulex, 0), Point(rulex, h)));
        if(rulex < 2*props.xRuling)
          s->addClass("xrule_1");
        ruleNode->addChild(s);
      }
    }
  }
  // draw left margin line (red)
  if(props.marginLeft > 0) {
    s = new SvgPath(Path2D().addLine(Point(props.marginLeft, 0), Point(props.marginLeft, h)));
    s->setAttr<color_t>("stroke", Color::RED);  // we'll let this have same opacity as other rulelines
    s->addClass("leftmargin");
    ruleNode->addChild(s);
  }
}

void Page::draw(Painter* painter, const Rect& dirty, bool rulelines)
{
  // draw red border around page if it failed to load
  ensureLoaded(false);  // skip memory check in case it is slow on some platforms
  if(loadStatus != LOAD_OK)
    painter->fillRect(Rect::ltrb(-10, -10, width()+10, height()+10), Color::RED);
  // draw drop shadow
  if(enableDropShadow && (!dirty.isValid() || !rect().contains(dirty))) {
    // UI decoration, not document: night mode would flip the black shadow into a white glow
    painter->save();
    painter->setColorMap(NULL);
    const Dim d = 4;  // was 6 for non-gradient version
    Gradient grad = Gradient::box(-2, -2, props.width+4, props.height+4, 0, 2*d);  // 2 == d-2
    grad.coordMode = Gradient::userSpaceOnUseMode;
    //grad.setObjectBBox(Rect::ltwh(d, props.height, props.width, d));
    grad.addStop(0, Color(Color::BLACK).setAlphaF(1));
    grad.addStop(1, Color(Color::BLACK).setAlphaF(0));
    painter->setFillBrush(&grad);
    // all 4 sides:
    //painter->drawRect(Rect::ltwh(-2*d, props.height, props.width + 4*d, 2*d));
    //painter->drawRect(Rect::ltwh(props.width, 0, 2*d, props.height));
    //painter->drawRect(Rect::ltwh(-2*d, -2*d, props.width + 4*d, 2*d));
    //painter->drawRect(Rect::ltwh(-2*d, 0, 2*d, props.height));
    // right and bottom only:
    painter->drawRect(Rect::ltwh(0, props.height, props.width + 2*d, 2*d));
    painter->drawRect(Rect::ltwh(props.width, 0, 2*d, props.height));
    // Painter has pointer to Gradient, so we must clear before it is destroyed
    painter->setFillBrush(Color::NONE);
    painter->restore();
  }
  // no ruling group means no page BG, so draw white BG manually
  if(!ruleNode)
    painter->fillRect(svgDoc->bounds(), Color::WHITE);

  SvgPainter(painter).drawNode(svgDoc.get(), dirty);

  if(isSelected && !Element::FORCE_NORMAL_DRAW)  //docElement()->selection())
    painter->fillRect(rect(), Color(props.color.luma() > 127 ? Color::BLUE : Color::YELLOW).setAlphaF(0.4f));
}

bool Page::saveSVG(IOStream& file, Dim x, Dim y)
{
  // ensure that page is actually loaded ... not a big deal if we fail since we're not
  //   overwriting the original
  if(!ensureLoaded())
    return false;

  // write-v3 class is only set in output SVG, not internally, to enable browser-only CSS
  contentNode->addClass("write-v3");
  // move the rule node into content node for saving so document can be read by old versions of Write
  //SvgNode* rulenode = props.ruleLayer->node();
  if(ruleNode) {
    svgDoc->removeChild(ruleNode);
    SvgNode* before = contentNode->children().empty() ? NULL : contentNode->children().front();
    contentNode->addChild(ruleNode, before);
  }
  // do not use SvgDocument::m_x,m_y since those will affect bounds!
  if(x != 0 || y != 0) {
    svgDoc->setAttr("x", x);
    svgDoc->setAttr("y", y);
  }
  // write SVG
  XmlStreamWriter xmlwriter;
  SvgWriter(xmlwriter).serialize(svgDoc.get());
  xmlwriter.save(file);
  svgDoc->removeAttr("x");
  svgDoc->removeAttr("y");
  contentNode->removeClass("write-v3");
  // restore rule node
  if(ruleNode) {
    contentNode->removeChild(ruleNode);
    svgDoc->addChild(ruleNode, contentNode);
  }
  return true;
}

bool Page::saveSVGFile(const char* filename)
{
  // ensure that page is actually loaded - we must check this here because opening the file
  //  for writing will prevent us from reading from it!
  if(!ensureLoaded())
    return false;
  FileStream file(filename, "wb");
  if(!file.is_open())
    return false;
  saveSVG(file);
  //file.close();  // FileStream closes on destruction, but we should call close and *check the return value*!
  return true;
}

// Document structure:
//   <svg><g id="page_1" xruling= ...>
//     <g class=ruleline >...</g>
//     <path ...>, etc - content nodes
//   </g></svg>

static void migrateLegacyPaths(SvgContainerNode* container)
{
  for(SvgNode* node : container->children()) {
    if(node->type() == SvgNode::G && (node->hasClass("hyperref") || node->hasClass("bookmark")))
      migrateLegacyPaths(node->asContainerNode());
    else if(node->xmlClass()[0])
      continue;  // skip nodes with any other class
    else if(node->type() == SvgNode::PATH) {
      Path2D* path = static_cast<SvgPath*>(node)->path();
      if(node->getColorAttr("fill", Color::NONE) != Color::NONE) {
        node->addClass(Element::FLAT_PEN_CLASS);
        // although Write didn't close path internally, it did add 'Z' when writing SVG, so we must remove
        //  the closing point
        int n = path->size();
        if(n > 2 && n % 2 && path->point(n-1) == path->point(0))
          path->resize(n - 1);
      }
      else if(node->getColorAttr("stroke", Color::NONE) != Color::NONE) {
        node->addClass(Element::STROKE_PEN_CLASS);
        node->setAttr<color_t>("fill", Color::NONE);
        node->setAttr<int>("stroke-linecap", Painter::RoundCap);
        node->setAttr<int>("stroke-linejoin", Painter::RoundJoin);
      }
      // before stroke grouping was added, Write didn't serialize com
      if(!node->getAttr("__comx") && !node->getAttr("__comy") && !path->empty()) {
        Point com = StrokeBuilder::calcCom(node, path);
        // Element::updateFromNode() reads com as strings, unfortunately
        node->setAttr("__comx", fstring("%.3f", com.x).c_str());
        node->setAttr("__comy", fstring("%.3f", com.y).c_str());
      }
    }
  }
}

void Page::migrateLegacySVG()
{
  contentNode->setXmlId("");  // remove id="page_1"
  contentNode->addClass("write-content");

  // remove leading <defs> section with CSS style (all previous versions)
  if(!svgDoc->children().empty() && svgDoc->children().front()->type() == SvgNode::DEFS) {
    SvgNode* defs0 = svgDoc->children().front();
    svgDoc->removeChild(defs0);
    delete defs0;
  }
  // remove the CSS styling ... set empty SvgCssStylesheet instead to remove CSS attributes from nodes?
  svgDoc->setStylesheet(NULL);

  // remove <rect class="pagebg" (Write 2017) ... old versions also wrote <rect> for non-white page bg
  if(!contentNode->children().empty()) {
    SvgNode* pagebg = contentNode->children().front();
    if(pagebg->type() == SvgNode::RECT) {  //&& strcmp(pagebg->xmlId(), "pagebg") == 0) {
      contentNode->removeChild(pagebg);
      delete pagebg;
    }
  }

  // remove <path class="ruleline"... nodes from content node (Write pre-2017)
  while(!contentNode->children().empty() && contentNode->children().front()->hasClass("ruleline")) {
    SvgNode* node0 = contentNode->children().front();
    contentNode->removeChild(node0);
    delete node0;
  }

  // remove named patterns for Write 2017 ruling group, then delete ruleNode
  if(ruleNode && !ruleNode->hasClass("customruling")) {
    auto rects = ruleNode->select("rect");
    for(SvgNode* rect : rects) {
      SvgNode* ref = rect->getRefTarget(rect->getStringAttr("fill"));
      if(ref && ref->type() == SvgNode::PATTERN)
        svgDoc->removeNamedNode(ref);
    }
    svgDoc->removeChild(ruleNode);
    delete ruleNode;
    ruleNode = NULL;
    isCustomRuling = false;
  }

  // convert paths
  migrateLegacyPaths(contentNode);
}

// convert page content to ruling (for use with external SVG files)
void Page::contentToRuling()
{
  if(ruleNode) {
    ruleNode->parent()->asContainerNode()->removeChild(ruleNode);
    delete ruleNode;
  }
  ruleNode = contentNode;
  ruleNode->removeClass("write-content");
  ruleNode->addClass("ruleline");
  isCustomRuling = true;
  contentNode = new SvgG;
  new Element(contentNode);
  contentNode->addClass("write-content");
  svgDoc->addChild(contentNode);
  // add a pagerect
  SvgNode* s = new SvgRect(Rect::wh(props.width, props.height));
  s->setAttr<color_t>("fill", Color::WHITE);
  s->addClass("pagerect");
  ruleNode->addChild(s, ruleNode->firstChild());
}

bool Page::loadSVG(SvgDocument* doc)
{
  SvgNode* cn = doc->selectFirst(".write-content");  // new version
  if(doc->hasExt()) {
    if(!cn)  // should never happen, but if it does, leave as unloaded page
      return false;
    svgDoc.reset(doc);
    contentNode = cn->asContainerNode();
    SvgNode* rulenode = doc->selectFirst(".ruleline");
    ruleNode = rulenode ? rulenode->asContainerNode() : NULL;
    isCustomRuling = ruleNode && !ruleNode->hasClass("write-std-ruling");
  }
  else {
    // remove x,y attributes
    doc->m_x = 0;
    doc->m_y = 0;
    if(!cn)
      cn = doc->selectFirst("#page_1");  // old versions
    if(!cn || !cn->asContainerNode()) {
      // non-Write document
      Rect r = doc->viewBox().isValid() ? doc->viewBox() : doc->bounds();
      if(!r.isValid()) r = Rect::wh(300, 300);
      svgDoc->setWidth(doc->width().isPercent() ? r.width() : doc->width().px());
      svgDoc->setHeight(doc->height().isPercent() ? r.height() : doc->height().px());
      if(doc->getTransform().isIdentity() && !doc->viewBox().isValid()) {
        for(SvgNode* n : doc->children())
          contentNode->addChild(n);
        doc->children().clear();
        // copy attributes (only standard attributes)
        for(const SvgAttr& attr : doc->attrs) {
          if(attr.stdAttr() != SvgAttr::UNKNOWN)
            contentNode->setAttr(attr);
        }
        delete doc;
        doc = NULL;
      }
      else
        contentNode->addChild(doc);
    }
    else {
      svgDoc.reset(doc);
      new Element(svgDoc.get());  // create Element for doc node
      contentNode = cn->asContainerNode();
      contentNode->removeClass("write-v3");
      contentNode->removeAttr("width");  // remove deprecated attributes
      contentNode->removeAttr("height");
      new Element(contentNode);  // create Element for content node
      SvgNode* rulenode = contentNode->selectFirst(".ruleline");
      if(rulenode && (rulenode->type() == SvgNode::G || rulenode->type() == SvgNode::DOC)) {
        // move rule node out of content node (has to be there in SVG file for old versions of Write)
        contentNode->removeChild(rulenode);
        svgDoc->addChild(rulenode, contentNode);  // insert before contentNode
        ruleNode = rulenode->asContainerNode();
        // id="write-ruling" is from pre-release version - we can remove check in the future
        isCustomRuling = !ruleNode->hasClass("write-std-ruling") && strcmp(ruleNode->xmlId(), "write-ruling") != 0;
        new Element(ruleNode);  // needed for custom ruling (will get replaced immediately for standard ruling)
      }
      if(!svgDoc->hasClass("write-page"))
        svgDoc->addClass("write-page");  // was not added until Sept 2020 :-(
      if(strcmp(contentNode->xmlId(), "page_1") == 0)
        migrateLegacySVG();
      if(!ruleNode) {
        ruleNode = new SvgG;  // replaced immediately by onPageSizeChange()
        svgDoc->addChild(ruleNode, contentNode);
      }
    }

    // create elements for each stroke and find bookmarks - assuming implicit creation is disabled
    // the page's own elements now say what it is tagged with, not the summary it was opened with
    pageTagIds.clear();
    for(SvgNode* node : contentNode->children())
      onAddStroke(new Element(node));
    // regions go below all ink; a file (or an older build) may have put ink ahead of one
    std::vector<SvgNode*> regionNodes;
    for(SvgNode* node : contentNode->children()) {
      if(node->hasExt() && static_cast<Element*>(node->ext())->isRulingRegion())
        regionNodes.push_back(node);
    }
    SvgNode* front = NULL;
    for(SvgNode* node : contentNode->children()) {
      if(!node->hasExt() || !static_cast<Element*>(node->ext())->isRulingRegion()) { front = node; break; }
    }
    for(SvgNode* node : regionNodes) {
      if(!front) break;
      contentNode->removeChild(node);
      contentNode->addChild(node, front);
    }
  }

  if(svgDoc->width().isPercent() || svgDoc->height().isPercent()) {
    Rect r = svgDoc->bounds();  // w/ width or height as % (and no canvasRect) this will be content bounds
    if(r.isValid()) {
      props.width = r.width();
      props.height = r.height();
    }
  }
  else {
    props.width = svgDoc->width().px();
    props.height = svgDoc->height().px();
  }
  // support float and string types for pasting pages and loading files, respectively
  Dim xr = contentNode->getFloatAttr("xruling", atof(contentNode->getStringAttr("xruling", "0")));
  Dim yr = contentNode->getFloatAttr("yruling", atof(contentNode->getStringAttr("yruling", "0")));
  Dim margin = contentNode->getFloatAttr("marginLeft", atof(contentNode->getStringAttr("marginLeft", "0")));
  props.color = parseColor(contentNode->getStringAttr("papercolor", "#fff"));
  Color rulecolor = parseColor(contentNode->getStringAttr("rulecolor", "#00f"));
  if(props.width <= 0 || props.height <= 0) {
    Rect docbbox = svgDoc->bounds();
    props.width = docbbox.width();
    props.height = docbbox.height();
  }
  props.xRuling = xr;
  props.yRuling = yr;
  props.marginLeft = margin;
  props.dotRadius = contentNode->getFloatAttr("dotradius", atof(contentNode->getStringAttr("dotradius", "0")));
  props.ruleColor = rulecolor;
  props.color.setAlphaF(1);

  // outline entry - note the "__" prefix: an unprefixed "outline" would collide with the CSS property
  //  of that name, which usvg does support, so the parser could plausibly claim it as a presentation
  //  attribute.  The other page attributes above predate CSS support and are left as they are.
  outlineTitle = contentNode->getStringAttr("__outline", "");
  outlineLevel = std::min(std::max(0,
      int(contentNode->getFloatAttr("__outlinelevel", atof(contentNode->getStringAttr("__outlinelevel", "0"))))),
      int(MAX_OUTLINE_LEVEL));

  // load content
  onPageSizeChange();
  loadStatus = props.width > 0 && props.height > 0 ? LOAD_OK : LOAD_SVG_ERROR;
  // a delay-loaded page arrives after the layer table has been read from the document config, so
  //  hidden layers have to be applied here as well as when the table changes
  applyLayerState();
  return loadStatus == LOAD_OK;
}

bool Page::loadSVGFile(const char* filename, bool delayload)
{
  // don't load over a dirty page (should never happen)
  if(dirtyCount != 0)
    return false;
  if(filename)
    fileName = filename;
  // do not delay loading if dimensions missing (for legacy files w/o width and height in <object> tag)
  if(delayload && props.width > 0 && props.height > 0) {
    loadStatus = NOT_LOADED;
    return FSPath(filename).exists();
  }
  SvgDocument* doc = SvgParser().parseFile(fileName.c_str()); //pugi::parse_default | pugi::parse_comments);
  loadStatus = doc && loadSVG(doc) ? LOAD_OK : LOAD_SVG_ERROR;
  return loadStatus == LOAD_OK;
}

bool Page::ensureLoaded(bool checkmem)
{
  if(!document) return true;  // ghost page has no document
  if(checkmem)
    document->checkMemoryUsage(getPageNum());
  return loadStatus == NOT_LOADED ?
      (blockIdx >= 0 ? document->loadBgzPage(this) : loadSVGFile()) :
      loadStatus == LOAD_OK;
}

void Page::unload()
{
  ASSERT(dirtyCount == 0 && "Attempting to unload a dirty page!");
  bookmarks.clear();
  ruleNode = NULL;
  svgDoc.reset(new SvgDocument(0, 0, props.width, props.height));
  initDoc();
  loadStatus = NOT_LOADED;
}

Element* Page::layerInsertPos(int layer) const
{
  if(!document)
    return NULL;
  int z = document->layers.zIndexOf(layer);
  for(Element* s : children()) {
    if(document->layers.zIndexOf(s->layer()) > z)
      return s;
  }
  return NULL;
}

Element* Page::layerFirstElement(int layer) const
{
  if(!document)
    return NULL;
  int z = document->layers.zIndexOf(layer);
  for(Element* s : children()) {
    if(document->layers.zIndexOf(s->layer()) >= z)
      return s;
  }
  return NULL;
}

Element* Page::moveToLayer(Element* s, int layer, Element* next)
{
  if(s->isRulingRegion())
    layer = LayerList::REGION_LAYER;  // a region is on no layer, whatever the caller asked
  SvgNode* nextNode = contentNode->removeChild(s->node);
  Element* prevnext = nextNode ? static_cast<Element*>(nextNode->ext()) : NULL;
  s->setLayer(layer);
  if(document)
    s->node->setDisplayMode(document->layers.isHidden(layer) ? SvgNode::NoneMode : SvgNode::BlockMode);
  Element* at = next ? next : layerInsertPos(layer);
  contentNode->addChild(s->node, at ? at->node : NULL);
  return prevnext;
}

void Page::applyLayerState()
{
  if(!document || loadStatus != LOAD_OK)
    return;
  for(Element* s : children())
    s->node->setDisplayMode(document->layers.isHidden(s->layer()) ?
        SvgNode::NoneMode : SvgNode::BlockMode);
}

bool Page::isEditable(const Element* s) const
{
  // a ruling region is not ink: it is reached only through its own "..." button, never by ink
  //  selection or an eraser.  This has to be explicit - REGION_LAYER is not in the layer table, and
  //  an unknown layer fails open.
  if(s && s->isRulingRegion())
    return false;
  // no document means the ghost page or a clipping, neither of which has a layer table
  return !document || !s || document->layers.isEditable(s->layer());
}

void Page::addStroke(Element* s, Element* next, int layer)
{
  // a region is never stamped with a layer: it stays below every layer's ink
  if(s->isRulingRegion())
    s->setLayer(LayerList::REGION_LAYER);
  else if(document)
    s->setLayer(layer == LayerList::LAYER_CURRENT ? document->layers.currentId : layer);
  // z-order within the page follows the layer table; an explicit `next` still wins, since the
  //  callers that pass one (draw-under, free-erase subpaths, undo replay) are placing the element
  //  relative to a sibling they already know is on the right layer
  // a region goes below all ink even when a caller names a sibling that is ink
  if(!next || s->isRulingRegion() != next->isRulingRegion())
    next = layerInsertPos(s->layer());
  contentNode->addChild(s->node, next ? next->node : NULL);
  if(document->history->undoable())
    document->history->addItem(new StrokeAddedItem(s, this, next));
  if(s->timestamp() <= 0)
    s->setTimestamp(mSecSinceEpoch());
  onAddStroke(s);
}

void Page::removeStroke(Element* s)
{
  onRemoveStroke(s);
  SvgNode* nextNode = contentNode->removeChild(s->node);
  if(document->history->undoable()) {
    Element* next = nextNode ? static_cast<Element*>(nextNode->ext()) : NULL;
    document->history->addItem(new StrokeDeletedItem(s, this, next));
  }
  else
    s->deleteNode();
}

void Page::onAddStroke(Element* s)
{
  // will cause unnecessary redraw if bookmark added outside margin in MARGIN_CONTENT mode ... not a big deal
  if(s->isBookmark()) {
    //bookmarks.push_back(s);
    document->bookmarksDirty = true;
  }
  if(s->isPageTag()) {
    refreshPageTags();
    return;
  }
  // a region arriving by undo or sync may have been drawn with colors from before a theme change; and
  //  it is not writing, so it takes no part in the page's time range
  if(s->isRulingRegion()) {
    if(loadStatus == LOAD_OK)
      s->rebuildRegion(props.color, props.ruleColor);
    return;
  }
  // update timestamp range
  if(s->timestamp() < minTimestamp || strokeCount() == 1)
    minTimestamp = s->timestamp();
  if(s->timestamp() > maxTimestamp || strokeCount() == 1)
    maxTimestamp = s->timestamp();
}

void Page::onRemoveStroke(Element* s)
{
  auto it = std::find(bookmarks.begin(), bookmarks.end(), s);  // works for bookmarks or margin content
  if(it != bookmarks.end()) {
    bookmarks.erase(it);
    --numBookmarks;
    document->bookmarksDirty = true;
  }
  if(s->isPageTag())
    refreshPageTags(s);  // still in the page at this point
  // see if we must recalc timestamp range
  if(s->timestamp() == minTimestamp || s->timestamp() == maxTimestamp) {
    minTimestamp = MAX_TIMESTAMP;
    maxTimestamp = 0;
  }
}

// in the order the tags sit on the page, each id once - a page can carry the same tag twice
void Page::refreshPageTags(const Element* removing)
{
  pageTagIds.clear();
  for(Element* s : children()) {
    if(s == removing || !s->isPageTag() || !s->pageTagId()[0])
      continue;
    std::string id = s->pageTagId();
    if(std::find(pageTagIds.begin(), pageTagIds.end(), id) == pageTagIds.end())
      pageTagIds.push_back(id);
  }
}

void Page::recalcTimeRange(bool force)
{
  if(!force && maxTimestamp != 0)
    return;
  if(strokeCount() == 0) {
    minTimestamp = mSecSinceEpoch();
    maxTimestamp = minTimestamp;
    return;
  }
  minTimestamp = MAX_TIMESTAMP;
  maxTimestamp = 0;
  for(Element* s : children()) {
    if(s->isRulingRegion())
      continue;
    minTimestamp = MIN(minTimestamp, s->timestamp());
    maxTimestamp = MAX(maxTimestamp, s->timestamp());
  }
}

const char* Page::getHyperRef(Point pos) const
{
  // support any href, not just those create by Write
  SvgNode* node = contentNode->nodeAt(pos, false);
  const char* s = NULL;
  while(node && !(s = node->getStringAttr("xlink:href", NULL)) && !(s = node->getStringAttr("href", NULL)))
    node = node->parent();
  return s;
}

SvgNode* Page::findNamedNode(const char* idstr) const
{
  const_cast<Page*>(this)->ensureLoaded();
  return svgDoc->namedNode(idstr);
}

// Set or clear this page's outline entry.  Writing the attributes here rather than in
//  onPageSizeChange() (where the other page attributes are written) keeps them off the page-geometry
//  path, but like those they are written eagerly so copy+paste of a page carries the entry with it.
void Page::setOutlineEntry(const char* title, int level)
{
  // the attributes live in the page SVG, so an unloaded page has nowhere to put them; the reload
  //  would overwrite whatever we wrote into the placeholder svgDoc that unload() left behind
  if(!ensureLoaded()) return;
  outlineTitle = title ? title : "";
  outlineLevel = std::min(std::max(0, level), int(MAX_OUTLINE_LEVEL));
  if(outlineTitle.empty()) {
    outlineLevel = 0;
    contentNode->removeAttr("__outline");
    contentNode->removeAttr("__outlinelevel");
  }
  else {
    contentNode->setAttr("__outline", outlineTitle.c_str());
    contentNode->setAttr<float>("__outlinelevel", float(outlineLevel));
  }
}

void Page::setSelected(bool sel)
{
  if(isSelected != sel)
    svgDoc->setDirty(SvgNode::PIXELS_DIRTY);
  isSelected = sel;
}
