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

// thumbnail geometry: the paper is drawn inside this box, letterboxed to the page's aspect ratio
static const Dim PREVIEW_W = 52;
static const Dim PREVIEW_H = 66;
static const Dim PREVIEW_PAD = 6;
static const size_t MAX_RECENT = 3;
// a US Letter page at Write's 150 units/inch, used whenever a page dimension is unset (an auto-grow
//  page has no fixed size, but its ruling still has to be drawn at some plausible scale)
static const Dim FALLBACK_ASPECT = 8.5/11.0;
static const Dim FALLBACK_PAGE_H = 11*150;

static const char* RECENT_CFG_KEY = "recentPageRulings";

/// recently used rulings

static bool sameRuling(const PageProperties& a, const PageProperties& b)
{
  return a.width == b.width && a.height == b.height && a.xRuling == b.xRuling && a.yRuling == b.yRuling
      && a.marginLeft == b.marginLeft && a.color == b.color && a.ruleColor == b.ruleColor;
}

static std::string serializeRuling(const PageProperties& props)
{
  return fstring("%g,%g,%g,%g,%g,%08X,%08X", props.width, props.height, props.xRuling, props.yRuling,
      props.marginLeft, props.color.argb(), props.ruleColor.argb());
}

static bool deserializeRuling(const StringRef& str, PageProperties* props)
{
  std::vector<StringRef> fields = splitStringRef(str, ',');
  if(fields.size() != 7)
    return false;
  props->width = atof(fields[0].toString().c_str());
  props->height = atof(fields[1].toString().c_str());
  props->xRuling = atof(fields[2].toString().c_str());
  props->yRuling = atof(fields[3].toString().c_str());
  props->marginLeft = atof(fields[4].toString().c_str());
  props->color = Color::fromArgb(strtoul(fields[5].toString().c_str(), NULL, 16));
  props->ruleColor = Color::fromArgb(strtoul(fields[6].toString().c_str(), NULL, 16));
  return true;
}

std::vector<PageProperties> recentRulings(const ScribbleConfig* cfg)
{
  std::vector<PageProperties> rulings;
  for(const StringRef& s : splitStringRef(StringRef(cfg->String(RECENT_CFG_KEY, "")), ";", true)) {
    PageProperties props;
    if(deserializeRuling(s, &props))
      rulings.push_back(props);
    if(rulings.size() >= MAX_RECENT)
      break;
  }
  return rulings;
}

void addRecentRuling(ScribbleConfig* cfg, const PageProperties& props)
{
  std::vector<PageProperties> rulings = recentRulings(cfg);
  // re-using a ruling moves it back to the front rather than adding a duplicate
  rulings.erase(std::remove_if(rulings.begin(), rulings.end(),
      [&props](const PageProperties& p){ return sameRuling(p, props); }), rulings.end());
  rulings.insert(rulings.begin(), props);
  if(rulings.size() > MAX_RECENT)
    rulings.resize(MAX_RECENT);
  std::vector<std::string> strs;
  for(const PageProperties& p : rulings)
    strs.push_back(serializeRuling(p));
  cfg->set(RECENT_CFG_KEY, joinStr(strs, ";").c_str());
}

/// preview

static std::string colorAttrs(const char* attr, Color color)
{
  return fstring(" %s=\"#%06X\" %s-opacity=\"%.3g\"", attr, color.rgb(), attr, color.alphaF());
}

// The thumbnail draws the page's real ruling rather than a stylized stand-in, so what the button shows
//  is what the page will be.  The one liberty taken is the minimum spacing below: fine grids and narrow
//  rulings would otherwise come out as a solid block of ink at this size, losing the very distinction
//  the previews exist to make, so lines are dropped (never moved) until they are far enough apart.
static const Dim MIN_LINE_SEP = 3;

static void appendRuleLines(std::string* path, Dim spacing, Dim scale, bool horz,
    Dim paperw, Dim paperh)
{
  if(spacing <= 0)
    return;
  Dim step = spacing*scale;
  if(step <= 0)
    return;  // degenerate page - nothing sensible to draw, and the loop below would never end
  if(step < MIN_LINE_SEP)
    step *= std::ceil(MIN_LINE_SEP/step);
  for(Dim pos = step; pos < (horz ? paperh : paperw) - 0.5; pos += step) {
    if(horz)
      *path += fstring("M0 %.2fH%.2f", pos, paperw);
    else
      *path += fstring("M%.2f 0V%.2f", pos, paperh);
  }
}

// The whole page is one <g> drawn at the origin, never several siblings: these previews sit in a
//  layout="box" parent, which positions each child on its own, so a bare margin line - whose bounding
//  box is just that line - was being centred in the box instead of staying where it belongs on the
//  page.  Grouping makes the page a single laid-out unit, with its own coordinates left intact.
static std::string pagePreviewSVG(const PageProperties& props, Dim boxw, Dim boxh)
{
  Dim aspect = (props.width > 0 && props.height > 0) ? props.width/props.height : FALLBACK_ASPECT;
  Dim maxw = boxw - 2*PREVIEW_PAD, maxh = boxh - 2*PREVIEW_PAD;
  Dim paperw = std::min(maxw, maxh*aspect), paperh = paperw/aspect;
  // the page's own coordinates map onto the thumbnail through this one scale factor
  Dim scale = paperh/(props.height > 0 ? props.height : FALLBACK_PAGE_H);

  std::string svg = fstring("<g class=\"page-preview\">"
      "<rect class=\"page-preview-paper\" width=\"%.2f\" height=\"%.2f\""
      "%s stroke=\"#909090\" stroke-width=\"1\"/>",
      paperw, paperh, colorAttrs("fill", props.color).c_str());

  std::string rules;
  appendRuleLines(&rules, props.yRuling, scale, true, paperw, paperh);
  appendRuleLines(&rules, props.xRuling, scale, false, paperw, paperh);
  if(!rules.empty())
    svg += fstring("<path class=\"page-preview-rules\" d=\"%s\" fill=\"none\"%s stroke-width=\"0.75\"/>",
        rules.c_str(), colorAttrs("stroke", props.ruleColor).c_str());

  if(props.marginLeft > 0 && props.marginLeft*scale < paperw)
    svg += fstring("<path class=\"page-preview-margin\" d=\"M%.2f 0V%.2f\" fill=\"none\""
        " stroke=\"#FF0000\" stroke-opacity=\"0.6\" stroke-width=\"0.75\"/>",
        props.marginLeft*scale, paperh);

  svg += "</g>";
  return svg;
}

SvgNode* createPagePreviewNode(const PageProperties& props, Dim boxw, Dim boxh)
{
  return loadSVGFragment(pagePreviewSVG(props, boxw, boxh).c_str());
}

// the preview as a toolbutton: the background rect is the button's cell, the page sits centred in it
static SvgNode* createPresetButtonNode(const PageProperties& props)
{
  std::string svg = fstring("<g class=\"toolbutton page-preset-btn\" layout=\"box\">"
      "<rect class=\"background\" width=\"%g\" height=\"%g\"/>", PREVIEW_W, PREVIEW_H);
  svg += pagePreviewSVG(props, PREVIEW_W, PREVIEW_H);
  svg += "</g>";
  return loadSVGFragment(svg.c_str());
}

/// the button

static PageProperties defaultPageProps(const ScribbleDoc* doc)
{
  const ScribbleConfig* cfg = doc->cfg;
  return PageProperties(cfg->Float("pageWidth"), cfg->Float("pageHeight"), cfg->Float("xRuling"),
      cfg->Float("yRuling"), cfg->Float("marginLeft"), Color::fromRgb(cfg->Int("pageColor")),
      Color::fromArgb(cfg->Int("ruleColor")));
}

// every route into the popup ends here: insert after the current page (the "add page" a user means),
//  and remember the ruling so the same page is one tap away next time
static void addPage(const PageProperties& props)
{
  ScribbleDoc* doc = ScribbleApp::app->activeDoc();
  if(!doc)
    return;
  doc->newPage(ScribbleApp::app->activeArea()->getCurrPageNum() + 1, &props);
  addRecentRuling(ScribbleApp::cfg, props);
}

static std::string rulingTooltip(const PageProperties& props)
{
  if(props.xRuling > 0 && props.yRuling > 0)
    return fstring(_("Grid, %g x %g"), props.xRuling, props.yRuling);
  if(props.yRuling > 0)
    return fstring(_("Ruled, %g"), props.yRuling);
  if(props.xRuling > 0)
    return fstring(_("Columns, %g"), props.xRuling);
  return _("Plain");
}

Button* createAddPageButton(Action* scanPageAction)
{
  Button* btn = createToolbutton(SvgGui::useFile(":/icons/ic_menu_plus.svg"), _("Add Page"));
  ArrowPopup* popup = createArrowPopup(Menu::VERT_LEFT);

  TextBox* title = createTextBox(_("Add Page"));
  title->node->addClass("arrowpopup-title");
  title->node->setAttribute("box-anchor", "left");
  popup->addWidget(title);

  Widget* presetRow = createRow({}, "", "", "left");
  popup->addWidget(presetRow);
  popup->addSeparator();
  if(scanPageAction)
    popup->addAction(scanPageAction);
  setupAutoClosePopup(popup);
  btn->addWidget(popup);

  // the row is rebuilt on every open: the recents change as pages are added, including from this popup
  auto populate = [popup, presetRow]() {
    SvgGui* gui = presetRow->window() ? presetRow->window()->gui() : NULL;
    if(gui)
      gui->deleteContents(presetRow);
    ScribbleDoc* doc = ScribbleApp::app->activeDoc();
    if(!doc)
      return;

    auto addPresetBtn = [popup, presetRow](const PageProperties& props, const char* tiptext) {
      Button* preset = new Button(createPresetButtonNode(props));
      preset->onClicked = [popup, props](){ closeAutoClosePopup(popup); addPage(props); };
      setupTooltip(preset, tiptext);
      presetRow->addWidget(preset);
    };

    PageProperties defprops = defaultPageProps(doc);
    addPresetBtn(defprops, _("Default"));
    for(const PageProperties& props : recentRulings(ScribbleApp::cfg)) {
      if(!sameRuling(props, defprops))
        addPresetBtn(props, rulingTooltip(props).c_str());
    }

    // "+" opens page setup for this one page; whatever comes out of it becomes the newest recent,
    //  so a ruling made here is a one-tap preset from then on
    Button* custom = createToolbutton(SvgGui::useFile(":/icons/ic_menu_plus.svg"), _("Custom Ruling"));
    custom->onClicked = [popup, doc](){
      closeAutoClosePopup(popup);
      PageProperties initprops = defaultPageProps(doc);
      RulingDialog dialog(doc, &initprops);
      if(Application::execDialog(&dialog) == Dialog::ACCEPTED)
        addPage(dialog.properties());
    };
    setupTooltip(custom, _("Choose a ruling for the new page"));
    presetRow->addWidget(custom);
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
