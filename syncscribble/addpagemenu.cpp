#include "addpagemenu.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "ulib/stringutil.h"

#include "application.h"
#include "basics.h"
#include "mainwindow.h"
#include "rulingdialog.h"
#include "scribbleapp.h"
#include "scribblearea.h"
#include "scribbleconfig.h"
#include "scribbledoc.h"

namespace AddPageMenu {

// a US Letter page at Write's 150 units/inch, used whenever a page dimension is unset (an auto-grow
//  page has no fixed size, but its ruling still has to be drawn at some plausible scale)
static const Dim FALLBACK_PAGE_W = 8.5*150;
static const Dim FALLBACK_PAGE_H = 11*150;

static const char* CUSTOM_CFG_KEY = "customPageLayouts";
static const char* RECENT_CFG_KEY = "recentPageLayouts";
static const size_t MAX_RECENT = 2;

// tile geometry, in UI units: two halves side by side above a label
static const Dim TILE_W = 156;
static const Dim TILE_H = 100;
static const Dim PLUS_TILE_W = 72;  // the recent row's "more layouts" tile, which has no preview
static const Dim TILE_PAD = 4;
static const Dim TILE_PREVIEW_H = 76;
static const Dim TILE_GAP = 4;  // between the two halves
static const int TILES_PER_ROW = 3;
static const Dim CATEGORY_W = 62;

/// layouts

enum LayoutCategory { LINED, SQUARED, DOTTED, SPECIAL, NUM_CATEGORIES };

struct BuiltinLayout {
  LayoutCategory category;
  PageLayout layout;
  // the margin is what this layout is, so the Margin line checkbox leaves it alone - without it, Wide
  //  margin would be a second copy of Medium lined
  bool marginIsLayout = false;
};

static PageLayout makeLayout(const char* name, Dim xr, Dim yr, Dim margin, Dim dotradius)
{
  PageLayout layout;
  layout.name = name;
  layout.xRuling = xr;
  layout.yRuling = yr;
  layout.marginLeft = margin;
  layout.dotRadius = dotradius;
  return layout;
}

// The lined and squared spacings are the ones the ruling dialog has always offered.  Dot grids carry
//  no margin: a red line through a dot grid is what that layout exists to avoid.
static const std::vector<BuiltinLayout>& builtinLayouts()
{
  static const std::vector<BuiltinLayout> layouts = {
    {LINED, makeLayout(_("Wide"), 0, 45, 100, 0)},
    {LINED, makeLayout(_("Medium"), 0, 40, 100, 0)},
    {LINED, makeLayout(_("Narrow"), 0, 35, 100, 0)},
    {SQUARED, makeLayout(_("Coarse"), 35, 35, 35, 0)},
    {SQUARED, makeLayout(_("Medium"), 30, 30, 30, 0)},
    {SQUARED, makeLayout(_("Fine"), 20, 20, 20, 0)},
    {DOTTED, makeLayout(_("Coarse"), 35, 35, 0, 2)},
    {DOTTED, makeLayout(_("Medium"), 30, 30, 0, 1.5)},
    {DOTTED, makeLayout(_("Fine"), 20, 20, 0, 1.25)},
    {SPECIAL, makeLayout(_("Plain"), 0, 0, 0, 0)},
    {SPECIAL, makeLayout(_("Dotted lines"), 0, 40, 100, 1)},
    {SPECIAL, makeLayout(_("Wide margin"), 0, 40, 300, 0), true}
  };
  return layouts;
}

static const char* categoryName(LayoutCategory category)
{
  switch(category) {
  case LINED: return _("Lined");
  case SQUARED: return _("Squared");
  case DOTTED: return _("Dotted");
  default: return _("Special");
  }
}

PageLayout layoutFromProps(const PageProperties& props)
{
  PageLayout layout = makeLayout("", props.xRuling, props.yRuling, props.marginLeft, props.dotRadius);
  layout.width = props.width;
  layout.height = props.height;
  return layout;
}

PageProperties layoutToProps(const PageLayout& layout, const ScribbleConfig* cfg)
{
  return PageProperties(layout.width > 0 ? layout.width : cfg->Float("pageWidth"),
      layout.height > 0 ? layout.height : cfg->Float("pageHeight"), layout.xRuling, layout.yRuling,
      layout.marginLeft, Color::fromRgb(cfg->Int("pageColor")), Color::fromArgb(cfg->Int("ruleColor")),
      layout.dotRadius);
}

std::string layoutDescription(const PageLayout& layout)
{
  if(layout.dotRadius > 0) {
    if(layout.xRuling > 0 && layout.yRuling > 0)
      return fstring(_("Dot grid, %g x %g"), layout.xRuling, layout.yRuling);
    if(layout.yRuling > 0)
      return fstring(_("Dotted lines, %g"), layout.yRuling);
    if(layout.xRuling > 0)
      return fstring(_("Dotted columns, %g"), layout.xRuling);
  }
  if(layout.xRuling > 0 && layout.yRuling > 0)
    return fstring(_("Grid, %g x %g"), layout.xRuling, layout.yRuling);
  if(layout.yRuling > 0)
    return fstring(_("Ruled, %g"), layout.yRuling);
  if(layout.xRuling > 0)
    return fstring(_("Columns, %g"), layout.xRuling);
  return _("Plain");
}

// short enough for a tile label; the tooltip carries the full description
static std::string shortDescription(const PageLayout& layout)
{
  Dim spacing = layout.yRuling > 0 ? layout.yRuling : layout.xRuling;
  if(spacing <= 0)
    return _("Plain");
  const char* kind = layout.dotRadius > 0 ? _("Dots") :
      (layout.xRuling > 0 && layout.yRuling > 0 ? _("Grid") : _("Lines"));
  return fstring("%s %g", kind, spacing);
}

// layouts as config strings: "xruling,yruling,margin,dotradius,width,height;..."
static std::vector<PageLayout> parseLayouts(const char* saved)
{
  std::vector<PageLayout> layouts;
  for(const StringRef& str : splitStringRef(StringRef(saved ? saved : ""), ";", true)) {
    std::vector<StringRef> fields = splitStringRef(str, ',');
    if(fields.size() != 6)
      continue;
    auto field = [&fields](int i){ return Dim(atof(fields[i].toString().c_str())); };
    PageLayout layout = makeLayout("", field(0), field(1), field(2), field(3));
    layout.width = field(4);
    layout.height = field(5);
    layouts.push_back(layout);
  }
  return layouts;
}

static std::string serializeLayouts(const std::vector<PageLayout>& layouts)
{
  std::vector<std::string> strs;
  for(const PageLayout& layout : layouts)
    strs.push_back(fstring("%g,%g,%g,%g,%g,%g", layout.xRuling, layout.yRuling, layout.marginLeft,
        layout.dotRadius, layout.width, layout.height));
  return joinStr(strs, ";");
}

std::vector<PageLayout> customLayouts(const ScribbleConfig* cfg)
{
  return parseLayouts(cfg->String(CUSTOM_CFG_KEY, ""));
}

void setCustomLayouts(ScribbleConfig* cfg, const std::vector<PageLayout>& layouts)
{
  cfg->set(CUSTOM_CFG_KEY, serializeLayouts(layouts).c_str());
}

Dim screenScale()
{
  ScribbleArea* area = ScribbleApp::app ? ScribbleApp::app->activeArea() : NULL;
  return area && area->getScale() > 0 ? area->getScale() : 1;
}

/// drawing

static std::string colorAttrs(const char* attr, Color color)
{
  return fstring(" %s=\"#%06X\" %s-opacity=\"%.3g\"", attr, color.rgb(), attr, color.alphaF());
}

// thumbnails drop lines (never move them) until they are this far apart, or fine grids and narrow
//  rulings come out as a solid block of ink - losing the very distinction the previews exist to make.
//  1:1 drawings are exempt: showing the real density is their whole point.
static const Dim MIN_LINE_SEP = 3;

// where a page lands in a drawing: drawing = page*scale + offset
struct PageMap {
  Dim scale;
  Point offset;
  Dim pageW, pageH;
};

// what part of the drawing a ruling may occupy: a rectangle, or the circle inscribed in it (the lens)
struct Clip {
  Rect bounds;
  bool circle = false;

  Dim radius() const { return bounds.width()/2; }
  bool hspan(Dim y, Dim* x0, Dim* x1) const
  {
    if(y <= bounds.top || y >= bounds.bottom)
      return false;
    Dim halfw = circle ? std::sqrt(radius()*radius() - (y - bounds.center().y)*(y - bounds.center().y))
        : bounds.width()/2;
    *x0 = bounds.center().x - halfw;
    *x1 = bounds.center().x + halfw;
    return true;
  }
  bool vspan(Dim x, Dim* y0, Dim* y1) const
  {
    if(x <= bounds.left || x >= bounds.right)
      return false;
    Dim halfh = circle ? std::sqrt(radius()*radius() - (x - bounds.center().x)*(x - bounds.center().x))
        : bounds.height()/2;
    *y0 = bounds.center().y - halfh;
    *y1 = bounds.center().y + halfh;
    return true;
  }
  bool contains(Point pt) const
  {
    if(!circle)
      return bounds.contains(pt);
    Dim dx = pt.x - bounds.center().x, dy = pt.y - bounds.center().y;
    return dx*dx + dy*dy < radius()*radius();
  }
};

// Positions of a ruling's lines along one axis, in drawing coordinates, limited to [lo, hi] so that a
//  1:1 drawing of a small window onto a big page only ever visits the lines it shows.
static std::vector<Dim> rulePositions(Dim spacing, Dim scale, Dim origin, Dim pageExtent, Dim lo, Dim hi,
    Dim minsep)
{
  std::vector<Dim> positions;
  Dim step = spacing*scale;
  if(step <= 0)
    return positions;  // degenerate page - nothing sensible to draw, and the loop would never end
  if(step < minsep)
    step *= std::ceil(minsep/step);
  Dim end = std::min(hi, origin + pageExtent*scale - 0.5);
  for(Dim pos = origin + step*std::max(Dim(1), std::ceil((lo - origin)/step)); pos < end; pos += step)
    positions.push_back(pos);
  return positions;
}

// evenly spaced positions from the page edge, for the along-the-line direction of dotted lines
static std::vector<Dim> pitchPositions(Dim pitch, Dim origin, Dim pageExtent, Dim lo, Dim hi)
{
  std::vector<Dim> positions;
  Dim end = std::min(hi, origin + pageExtent);
  for(Dim pos = origin + pitch*(std::max(Dim(0), std::ceil((lo - origin)/pitch - 0.5)) + 0.5);
      pos < end; pos += pitch)
    positions.push_back(pos);
  return positions;
}

// The ruling of `props` - lines or dots, plus the margin - mapped by `map` and cut to `clip`.
//  mindotr keeps dots visible where the real radius would be a fraction of a pixel.
static std::string rulingSVG(const PageProperties& props, const PageMap& map, const Clip& clip,
    Dim minsep, Dim mindotr, Dim linewidth)
{
  Dim scale = map.scale;
  Dim pageLeft = map.offset.x, pageRight = pageLeft + map.pageW*scale;
  Dim pageTop = map.offset.y, pageBottom = pageTop + map.pageH*scale;
  const Rect& area = clip.bounds;
  std::string svg;

  if(props.dotRadius > 0 && (props.xRuling > 0 || props.yRuling > 0)) {
    Dim dotr = std::max(props.dotRadius*scale, mindotr);
    // must match Page::generateRuleLayer(), which spaces a dotted line's dots max(4r, 4) apart
    Dim pitch = std::max(std::max(std::max(4*props.dotRadius, Dim(4))*scale, 4*dotr), minsep);
    std::vector<Dim> xs = props.xRuling > 0 ?
        rulePositions(props.xRuling, scale, pageLeft, map.pageW, area.left, area.right, minsep) :
        pitchPositions(pitch, pageLeft, map.pageW*scale, area.left, area.right);
    std::vector<Dim> ys = props.yRuling > 0 ?
        rulePositions(props.yRuling, scale, pageTop, map.pageH, area.top, area.bottom, minsep) :
        pitchPositions(pitch, pageTop, map.pageH*scale, area.top, area.bottom);
    // filled circles as two arcs each; a zero-length round-capped segment would be shorter, but nanovg
    //  drops degenerate segments and draws nothing for it
    std::string dots;
    for(Dim doty : ys) {
      for(Dim dotx : xs) {
        if(clip.contains(Point(dotx, doty)))
          dots += fstring("M%.2f %.2fa%.2f %.2f 0 1 0 %.2f 0a%.2f %.2f 0 1 0 %.2f 0",
              dotx - dotr, doty, dotr, dotr, 2*dotr, dotr, dotr, -2*dotr);
      }
    }
    if(!dots.empty())
      svg += fstring("<path class=\"page-preview-rules\" d=\"%s\"%s stroke=\"none\"/>",
          dots.c_str(), colorAttrs("fill", props.ruleColor).c_str());
  }
  else {
    std::string lines;
    Dim start, end;
    for(Dim y : rulePositions(props.yRuling, scale, pageTop, map.pageH, area.top, area.bottom, minsep)) {
      if(clip.hspan(y, &start, &end) && std::min(end, pageRight) > std::max(start, pageLeft))
        lines += fstring("M%.2f %.2fH%.2f", std::max(start, pageLeft), y, std::min(end, pageRight));
    }
    for(Dim x : rulePositions(props.xRuling, scale, pageLeft, map.pageW, area.left, area.right, minsep)) {
      if(clip.vspan(x, &start, &end) && std::min(end, pageBottom) > std::max(start, pageTop))
        lines += fstring("M%.2f %.2fV%.2f", x, std::max(start, pageTop), std::min(end, pageBottom));
    }
    if(!lines.empty())
      svg += fstring("<path class=\"page-preview-rules\" d=\"%s\" fill=\"none\"%s stroke-width=\"%.2f\"/>",
          lines.c_str(), colorAttrs("stroke", props.ruleColor).c_str(), linewidth);
  }

  Dim marginx = pageLeft + props.marginLeft*scale, start, end;
  if(props.marginLeft > 0 && marginx < pageRight && clip.vspan(marginx, &start, &end)
      && std::min(end, pageBottom) > std::max(start, pageTop))
    svg += fstring("<path class=\"page-preview-margin\" d=\"M%.2f %.2fV%.2f\" fill=\"none\""
        " stroke=\"#FF0000\" stroke-opacity=\"0.6\" stroke-width=\"%.2f\"/>",
        marginx, std::max(start, pageTop), std::min(end, pageBottom), linewidth);
  return svg;
}

// the page's own size, or US Letter where it has none
static void pageExtent(const PageProperties& props, Dim* pagew, Dim* pageh)
{
  *pagew = props.width > 0 ? props.width : FALLBACK_PAGE_W;
  *pageh = props.height > 0 ? props.height : FALLBACK_PAGE_H;
}

// the paper's size when the whole page is fitted into boxw x boxh
static void fitPaper(const PageProperties& props, Dim boxw, Dim boxh, Dim* paperw, Dim* paperh)
{
  Dim pagew, pageh;
  pageExtent(props, &pagew, &pageh);
  Dim aspect = pagew/pageh;
  *paperw = std::min(boxw, boxh*aspect);
  *paperh = *paperw/aspect;
}

// A magnifying glass over the thumbnail: inside the lens the ruling is drawn at `lensscale`, i.e. at
//  the size it will be on screen.  It is centred on the middle of a cell, so what it shows is whole
//  squares (or four dots, or a gap between two lines) rather than whatever a fixed spot on the page
//  happens to cut through - on a coarse ruling that was a single dot, which says nothing about size.
//  On a lined page it sits just right of the margin instead, so the margin line is in view as well.
static std::string lensSVG(const PageProperties& props, const PageMap& thumb, Dim lensscale,
    Dim paperw, Dim paperh)
{
  Dim radius = std::min(Dim(90), 0.42*std::min(paperw, paperh));
  Dim span = radius/lensscale;  // page units from the lens centre to its edge
  auto midCell = [](Dim pos, Dim spacing){ return spacing > 0 ? (std::floor(pos/spacing) + 0.5)*spacing : pos; };
  Point pagept(0.4*thumb.pageW, 0.25*thumb.pageH);
  if(props.xRuling > 0)
    pagept.x = midCell(pagept.x, props.xRuling);
  else if(props.marginLeft > 0)
    pagept.x = props.marginLeft + 0.45*span;
  pagept.y = midCell(pagept.y, props.yRuling);
  // the lens goes over the part of the page it magnifies, kept inside the paper where there is room
  auto clampTo = [radius](Dim pos, Dim extent){
    return extent > 2*radius + 4 ? std::min(std::max(pos, radius + 2), extent - radius - 2) : extent/2; };
  Point center(clampTo(thumb.offset.x + pagept.x*thumb.scale, paperw),
      clampTo(thumb.offset.y + pagept.y*thumb.scale, paperh));
  PageMap lens = {lensscale, Point(center.x - pagept.x*lensscale, center.y - pagept.y*lensscale),
      thumb.pageW, thumb.pageH};
  Clip clip;
  clip.bounds = Rect::ltrb(center.x - radius, center.y - radius, center.x + radius, center.y + radius);
  clip.circle = true;

  std::string svg = fstring("<circle class=\"page-preview-lens\" cx=\"%.2f\" cy=\"%.2f\" r=\"%.2f\"%s"
      " stroke=\"none\"/>", center.x, center.y, radius, colorAttrs("fill", props.color).c_str());
  svg += rulingSVG(props, lens, clip, 0, 0.5, 1);
  Dim diag = radius*std::sqrt(0.5);
  svg += fstring("<circle cx=\"%.2f\" cy=\"%.2f\" r=\"%.2f\" fill=\"none\" stroke=\"#6E6E6E\""
      " stroke-width=\"3\"/>", center.x, center.y, radius);
  svg += fstring("<path d=\"M%.2f %.2fL%.2f %.2f\" fill=\"none\" stroke=\"#6E6E6E\" stroke-width=\"7\""
      " stroke-linecap=\"round\"/>", center.x + diag + 2, center.y + diag + 2,
      center.x + diag + 22, center.y + diag + 22);
  return svg;
}

// The whole page is one <g> drawn at the origin, never several siblings: these previews sit in a
//  layout="box" parent, which positions each child on its own, so a bare margin line - whose bounding
//  box is just that line - was being centred in the box instead of staying where it belongs on the
//  page.  Grouping makes the page a single laid-out unit, with its own coordinates left intact.
static std::string pagePreviewSVG(const PageProperties& props, Dim boxw, Dim boxh, Dim lensscale = 0)
{
  Dim paperw, paperh, pagew, pageh;
  fitPaper(props, boxw, boxh, &paperw, &paperh);
  pageExtent(props, &pagew, &pageh);
  PageMap thumb = {paperh/pageh, Point(0, 0), pagew, pageh};
  Clip clip;
  clip.bounds = Rect::wh(paperw, paperh);

  std::string svg = fstring("<g class=\"page-preview\">"
      "<rect class=\"page-preview-paper\" width=\"%.2f\" height=\"%.2f\""
      "%s stroke=\"#909090\" stroke-width=\"1\"/>",
      paperw, paperh, colorAttrs("fill", props.color).c_str());
  svg += rulingSVG(props, thumb, clip, MIN_LINE_SEP, 0.6, 0.75);
  if(lensscale > 0)
    svg += lensSVG(props, thumb, lensscale, paperw, paperh);
  svg += "</g>";
  return svg;
}

SvgNode* createPagePreviewNode(const PageProperties& props, Dim boxw, Dim boxh, Dim lensScale)
{
  return loadSVGFragment(pagePreviewSVG(props, boxw, boxh, lensScale).c_str());
}

// Where a 1:1 view `window` long should start, in page units, so that the ruling's cells sit centred
//  in it: as many whole cells as fit, with the leftover split between the two ends.  Starting from the
//  page's own origin instead put the lines wherever they happened to fall, and on a coarse grid that
//  could leave a single dot or one line in view - nothing that shows the size of a square.
static Dim cellPhase(Dim spacing, Dim window, Dim scale)
{
  Dim cell = spacing*scale;
  if(cell <= 0)
    return 0;
  // a line exactly on the window's edge is clipped away, so keep whole cells a unit clear of both ends
  Dim cells = std::floor((window - 2)/cell);
  Dim pad = cells >= 1 ? (window - cells*cell)/2 : window/2;
  return spacing - pad/scale;  // puts the first line `pad` into the window
}

// Where a 1:1 view should start so that one ruling line runs exactly through its middle.  Used for the
//  rows: tiles sit side by side, and a line at the same height in every one gives the eye a common
//  baseline to compare spacings from - with centred cells instead, a gap or a line landed there
//  depending on the spacing, and neighbouring tiles no longer lined up.
static Dim centeredLinePhase(Dim spacing, Dim window, Dim scale)
{
  Dim cell = spacing*scale;
  if(cell <= 0)
    return 0;
  Dim pad = std::fmod(window/2, cell);  // the middle line is a whole number of cells past this one
  return spacing - pad/scale;
}

// The two parts of a tile: the whole page on the left, and on the right a window onto it at 1:1.  The
//  page takes only its own width, so everything else goes to the 1:1 window - the part that needs the
//  room, since at real size a tile-sized window holds only a few cells.
static std::string tileHalvesSVG(const PageProperties& props, Dim x, Dim y, Dim w, Dim h, Dim onetoone)
{
  Dim paperw, paperh, pagew, pageh;
  fitPaper(props, 0.4*w, h, &paperw, &paperh);
  pageExtent(props, &pagew, &pageh);
  std::string svg = fstring("<g transform=\"translate(%.2f %.2f)\">", x, y + (h - paperh)/2);
  svg += pagePreviewSVG(props, 0.4*w, h);
  svg += "</g>";

  Dim right = x + paperw + TILE_GAP;
  Dim rightw = x + w - right;
  // lined pages start just left of the margin, so the margin line is in view; anything with vertical
  //  rules is phased as whole cells, and the rows so a line runs through the middle
  Dim originx = props.xRuling > 0 ? cellPhase(props.xRuling, rightw, onetoone) :
      (props.marginLeft > 0 ? std::max(Dim(0), props.marginLeft - 8/onetoone) : 0);
  Dim originy = centeredLinePhase(props.yRuling, h, onetoone);
  PageMap map = {onetoone, Point(right - originx*onetoone, y - originy*onetoone), pagew, pageh};
  Clip clip;
  clip.bounds = Rect::ltwh(right, y, rightw, h);
  svg += fstring("<rect x=\"%.2f\" y=\"%.2f\" width=\"%.2f\" height=\"%.2f\"%s stroke=\"#909090\""
      " stroke-width=\"1\"/>", right, y, rightw, h, colorAttrs("fill", props.color).c_str());
  svg += rulingSVG(props, map, clip, 0, 0.5, 1);
  svg += fstring("<text class=\"weak\" x=\"%.2f\" y=\"%.2f\" text-anchor=\"end\" font-size=\"8\">1:1</text>",
      right + rightw - 2, y + h - 3);
  return svg;
}

// A tile is one toolbutton; like the preview, its contents are a single <g> so the box layout places
//  them as a unit, and a transparent rect the size of the tile keeps that unit from being re-centred
//  on whatever it happens to contain.
static SvgNode* createTileNode(const std::string& content, const char* label, Dim tilew = TILE_W)
{
  std::string svg = fstring("<g class=\"toolbutton page-layout-btn\" layout=\"box\">"
      "<rect class=\"background\" width=\"%g\" height=\"%g\"/>"
      "<g><rect width=\"%g\" height=\"%g\" fill=\"none\"/>", tilew, TILE_H, tilew, TILE_H);
  svg += content;
  svg += fstring("<text class=\"weak\" x=\"%g\" y=\"%g\" text-anchor=\"middle\" font-size=\"11\">%s</text>",
      tilew/2, TILE_H - 8, label);
  svg += "</g></g>";
  return loadSVGFragment(svg.c_str());
}

static SvgNode* createLayoutTileNode(const PageProperties& props, const char* label)
{
  return createTileNode(tileHalvesSVG(props, TILE_PAD, TILE_PAD, TILE_W - 2*TILE_PAD, TILE_PREVIEW_H,
      screenScale()), label);
}

// a dashed tile with a "+": "more layouts" in the recent row, "new layout" in the grid
static SvgNode* createPlusTileNode(const char* label, Dim tilew)
{
  Dim w = tilew - 2*TILE_PAD, cx = tilew/2, cy = TILE_PAD + TILE_PREVIEW_H/2;
  std::string content = fstring("<rect x=\"%g\" y=\"%g\" width=\"%g\" height=\"%g\" rx=\"4\" fill=\"none\""
      " stroke=\"#808080\" stroke-width=\"1\" stroke-dasharray=\"4 3\"/>", TILE_PAD, TILE_PAD, w, TILE_PREVIEW_H);
  content += fstring("<path d=\"M%g %gv24M%g %gh24\" fill=\"none\" stroke=\"#A0A0A0\" stroke-width=\"2.5\""
      " stroke-linecap=\"round\"/>", cx, cy - 12, cx - 12, cy);
  return createTileNode(content, label, tilew);
}

/// the button


// the document's default page as a layout; size 0 so it follows the default rather than copying it
static PageLayout defaultLayout(const ScribbleDoc* doc)
{
  return defaultLayout(doc->cfg);
}

// Recently used layouts, most recent first.  Stored as layouts - geometry only - under a new key: the
//  old "recentPageRulings" entries carried colors, which is what put light-theme rules on dark paper.
static std::vector<PageLayout> recentLayouts()
{
  std::vector<PageLayout> layouts = parseLayouts(ScribbleApp::cfg->String(RECENT_CFG_KEY, ""));
  if(layouts.size() > MAX_RECENT)
    layouts.resize(MAX_RECENT);
  return layouts;
}

// every route into the popup ends here: insert after the current page (the "add page" a user means),
//  and remember the layout so the same page is one tap away next time
static void addPage(const PageLayout& layout)
{
  ScribbleDoc* doc = ScribbleApp::app->activeDoc();
  if(!doc)
    return;
  PageProperties props = layoutToProps(layout, doc->cfg);
  doc->newPage(ScribbleApp::app->activeArea()->getCurrPageNum() + 1, &props);

  std::vector<PageLayout> recents = recentLayouts();
  // re-using a layout moves it back to the front rather than adding a duplicate
  recents.erase(std::remove_if(recents.begin(), recents.end(),
      [&layout](const PageLayout& other){ return sameLayout(other, layout); }), recents.end());
  recents.insert(recents.begin(), layout);
  if(recents.size() > MAX_RECENT)
    recents.resize(MAX_RECENT);
  ScribbleApp::cfg->set(RECENT_CFG_KEY, serializeLayouts(recents).c_str());
}

// Edit a layout in the page setup dialog.  customidx >= 0 edits that custom layout in place (and offers
//  Delete); otherwise the result is saved as a new custom layout - which is how a built-in one is
//  "edited", since the built-ins are fixed.
static void editLayout(const PageLayout& layout, int customidx, const char* title)
{
  ScribbleDoc* doc = ScribbleApp::app->activeDoc();
  if(!doc)
    return;
  PageProperties initprops = layoutToProps(layout, doc->cfg);
  RulingDialog dialog(doc, &initprops, true);
  dialog.setTitle(title);
  bool deleted = false;
  if(customidx >= 0)
    dialog.addButton(_("Delete"), [&](){ deleted = true; dialog.finish(Dialog::ACCEPTED); });
  if(Application::execDialog(&dialog) != Dialog::ACCEPTED)
    return;

  std::vector<PageLayout> layouts = customLayouts(ScribbleApp::cfg);
  bool existing = customidx >= 0 && customidx < int(layouts.size());
  if(deleted) {
    if(existing)
      layouts.erase(layouts.begin() + customidx);
  }
  else {
    PageLayout edited = layoutFromProps(dialog.properties());
    // a size equal to the document default is stored as "default", so the layout follows the default
    if(edited.width == doc->cfg->Float("pageWidth")) edited.width = 0;
    if(edited.height == doc->cfg->Float("pageHeight")) edited.height = 0;
    if(existing)
      layouts[customidx] = edited;
    else
      layouts.push_back(edited);
  }
  setCustomLayouts(ScribbleApp::cfg, layouts);
}

// a layout tile: tap adds that page, right-click or long press edits the layout
static Button* createLayoutTile(ArrowPopup* popup, ScribbleDoc* doc, const PageLayout& layout,
    const char* label, int customidx)
{
  Button* tile = new Button(createLayoutTileNode(layoutToProps(layout, doc->cfg), label));
  tile->onClicked = [popup, layout](){ closeAutoClosePopup(popup); addPage(layout); };
  std::string edittitle = customidx >= 0 ? _("Edit Layout") : fstring(_("New Layout from %s"),
      layoutDescription(layout).c_str());
  SvgGui::setupRightClick(tile, [popup, layout, customidx, edittitle](SvgGui*, Widget*, Point){
    closeAutoClosePopup(popup);
    editLayout(layout, customidx, edittitle.c_str());
  });
  setupTooltip(tile, (layoutDescription(layout) + _(" - right-click or long press to edit")).c_str());
  return tile;
}

// The grid: rows of tiles, the category named at the left of its first row only.  With the margin line
//  switched off, the built-in layouts are shown - and added - without it; custom layouts keep whatever
//  margin they were given, since that was chosen for that layout specifically.
// makeTile builds each tile (what a tap does is the caller's business - add a page here, pick the layout
//  in the new document dialog); lastTile, if any, goes after the custom layouts.
typedef std::function<Widget*(const PageLayout& layout, const char* label, int customidx)> TileMaker;

static void buildLayoutGrid(Widget* layoutGrid, const TileMaker& makeTile, Widget* lastTile)
{
  bool marginline = ScribbleApp::cfg->Bool("layoutMarginLine", true);
  std::vector<Widget*> tiles;
  auto flushRows = [layoutGrid, &tiles](const char* category) {
    for(size_t i = 0; i < tiles.size(); i += TILES_PER_ROW) {
      Widget* row = createRow({}, "", "", "left");
      std::string label = fstring("<g layout=\"box\"><rect width=\"%g\" height=\"%g\" fill=\"none\"/>"
          "<text class=\"weak\" box-anchor=\"left\" font-size=\"12\">%s</text></g>",
          CATEGORY_W, TILE_H, i == 0 ? category : "");
      row->addWidget(new Widget(loadSVGFragment(label.c_str())));
      for(size_t j = i; j < std::min(tiles.size(), i + TILES_PER_ROW); ++j)
        row->addWidget(tiles[j]);
      layoutGrid->addWidget(row);
    }
    tiles.clear();
  };

  for(int category = 0; category < NUM_CATEGORIES; ++category) {
    for(const BuiltinLayout& builtin : builtinLayouts()) {
      if(builtin.category != category)
        continue;
      PageLayout layout = builtin.layout;
      if(!marginline && !builtin.marginIsLayout)
        layout.marginLeft = 0;
      tiles.push_back(makeTile(layout, layout.name, -1));
    }
    flushRows(categoryName(LayoutCategory(category)));
  }

  std::vector<PageLayout> customs = customLayouts(ScribbleApp::cfg);
  for(size_t i = 0; i < customs.size(); ++i)
    tiles.push_back(makeTile(customs[i], shortDescription(customs[i]).c_str(), int(i)));
  if(lastTile)
    tiles.push_back(lastTile);
  flushRows(_("Custom"));
}

static void buildAddPageGrid(ArrowPopup* popup, Widget* layoutGrid, ScribbleDoc* doc)
{
  Button* newtile = new Button(createPlusTileNode(_("New layout"), TILE_W));
  newtile->onClicked = [popup](){
    closeAutoClosePopup(popup);
    ScribbleDoc* activedoc = ScribbleApp::app->activeDoc();
    if(activedoc)
      editLayout(defaultLayout(activedoc), -1, _("New Layout"));
  };
  setupTooltip(newtile, _("Add a custom layout"));
  buildLayoutGrid(layoutGrid, [popup, doc](const PageLayout& layout, const char* label, int customidx) {
    return createLayoutTile(popup, doc, layout, label, customidx);
  }, newtile);
}

std::vector<Button*> createLayoutGrid(Widget* container, const ScribbleConfig* cfg,
    const std::function<void(const PageLayout&)>& onPick)
{
  std::vector<Button*> tiles;
  buildLayoutGrid(container, [cfg, &onPick, &tiles](const PageLayout& layout, const char* label, int) {
    Button* tile = new Button(createLayoutTileNode(layoutToProps(layout, cfg), label));
    tile->onClicked = [onPick, layout](){ onPick(layout); };
    setupTooltip(tile, layoutDescription(layout).c_str());
    tiles.push_back(tile);
    return tile;
  }, NULL);
  return tiles;
}

PageLayout defaultLayout(const ScribbleConfig* cfg)
{
  return makeLayout("", cfg->Float("xRuling"), cfg->Float("yRuling"), cfg->Float("marginLeft"),
      cfg->Float("dotRadius"));
}

bool sameLayout(const PageLayout& a, const PageLayout& b)
{
  return a.xRuling == b.xRuling && a.yRuling == b.yRuling && a.marginLeft == b.marginLeft
      && a.dotRadius == b.dotRadius && a.width == b.width && a.height == b.height;
}

std::string layoutToString(const PageLayout& layout)
{
  return serializeLayouts({layout});
}

bool layoutFromString(const char* str, PageLayout* layout)
{
  std::vector<PageLayout> layouts = parseLayouts(str);
  if(layouts.empty())
    return false;
  *layout = layouts[0];
  return true;
}

Button* createAddPageButton(Action* scanPageAction)
{
  Button* btn = createToolbutton(SvgGui::useFile(":/icons/ic_menu_file_plus.svg"), _("Add Page"));
  ArrowPopup* popup = createArrowPopup(Menu::VERT_LEFT);

  TextBox* title = createTextBox(_("Add Page"));
  title->node->addClass("arrowpopup-title");
  // in a row of its own so its spacing can grow with the grid view.  Space above it is an empty rect
  //  rather than a top margin: the popup's column ignores its first child's top margin (tried on the
  //  text and on this row), while a spacer's height it cannot ignore.
  Widget* titleSpacer = new Widget(loadSVGFragment("<rect width=\"1\" height=\"8\" fill=\"none\"/>"));
  Widget* titleRow = createRow({title}, "", "", "left");
  popup->addWidget(titleSpacer);
  popup->addWidget(titleRow);

  // Two views in one popup: the recent row, which is what opens - "another one of those" should stay a
  //  single tap - and the full layout grid behind its "+".  Both are built on open and only their
  //  visibility changes afterwards: "+" is pressed inside the recent row, and rebuilding from its own
  //  click handler would free the widget the press is still being dispatched to.
  Widget* recentRow = createRow({}, "", "", "left");
  Widget* layoutGrid = createColumn({}, "", "", "left");
  // the grid's options sit outside the grid for the same reason: toggling one rebuilds the grid
  CheckBox* marginCb = createCheckBox(_("Margin line"), ScribbleApp::cfg->Bool("layoutMarginLine", true));
  setupTooltip(marginCb, _("Red margin line at the left of lined and squared layouts"));
  Widget* gridOptions = createRow({marginCb}, "", "", "left");
  gridOptions->setMargins(0, 0, 8, 0);
  popup->addWidget(gridOptions);
  popup->addWidget(recentRow);
  popup->addWidget(layoutGrid);
  popup->addSeparator();
  if(scanPageAction)
    popup->addAction(scanPageAction);
  setupAutoClosePopup(popup);
  btn->addWidget(popup);

  marginCb->onToggled = [popup, layoutGrid](bool checked){
    ScribbleApp::cfg->set("layoutMarginLine", checked);
    ScribbleDoc* doc = ScribbleApp::app->activeDoc();
    SvgGui* gui = popup->window() ? popup->window()->gui() : NULL;
    if(!doc || !gui)
      return;
    gui->deleteContents(layoutGrid);
    buildAddPageGrid(popup, layoutGrid, doc);
  };

  // rebuilt on every open: recents and custom layouts change, and the 1:1 halves follow the zoom
  auto populate = [popup, title, titleSpacer, titleRow, gridOptions, recentRow, layoutGrid]() {
    SvgGui* gui = popup->window() ? popup->window()->gui() : NULL;
    if(gui) {
      gui->deleteContents(recentRow);
      gui->deleteContents(layoutGrid);
    }
    title->setText(_("Add Page"));
    titleSpacer->setVisible(false);
    titleRow->setMargins(0, 0, 0, 0);
    gridOptions->setVisible(false);
    recentRow->setVisible(true);
    layoutGrid->setVisible(false);
    ScribbleDoc* doc = ScribbleApp::app->activeDoc();
    if(!doc)
      return;

    // the recent row: the document default, the last layouts used, and "+" for everything else
    PageLayout deflayout = defaultLayout(doc);
    recentRow->addWidget(createLayoutTile(popup, doc, deflayout, _("Default"), -1));
    for(const PageLayout& layout : recentLayouts()) {
      if(!sameLayout(layout, deflayout))
        recentRow->addWidget(createLayoutTile(popup, doc, layout, shortDescription(layout).c_str(), -1));
    }
    Button* more = new Button(createPlusTileNode(_("More"), PLUS_TILE_W));
    more->onClicked = [title, titleSpacer, titleRow, gridOptions, recentRow, layoutGrid](){
      title->setText(_("Choose Layout"));
      // the grid is a much bigger view than the recent row, and its title needs room to match
      titleSpacer->setVisible(true);
      titleRow->setMargins(0, 6, 14, 6);
      gridOptions->setVisible(true);
      recentRow->setVisible(false);
      layoutGrid->setVisible(true);
    };
    setupTooltip(more, _("All layouts"));
    recentRow->addWidget(more);

    buildAddPageGrid(popup, layoutGrid, doc);
  };

  // opened on press like a menu, not from onClicked: a popup shown on release is closed again by that
  //  same release arriving as an outside press.  Mirrors the overflow button in mainwindow.cpp.
  btn->addHandler([btn, popup, populate](SvgGui* gui, SDL_Event* event){
    if(event->type != SDL_FINGERDOWN || event->tfinger.fingerId != SDL_BUTTON_LMASK)
      return false;
    // the press that closed the popup (as an outside press) must not immediately reopen it
    if(gui->lastClosedMenu != popup) {
      gui->closeMenus();
      populate();
      openAutoClosePopup(popup);
      btn->node->addClass("pressed");  // cleared by closeMenus(), which unpresses the popup's parent
    }
    return true;
  });
  return btn;
}

}  // namespace AddPageMenu
