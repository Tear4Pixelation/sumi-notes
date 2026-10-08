#pragma once

#include <string>
#include <vector>

#include "ugui/widgets.h"
#include "page.h"

class ScribbleConfig;

// The "+" button that sits next to Save: the one place pages are added from.
//
// A page's ruling belongs to that page, so adding a page is also where a ruling is chosen.  Rather
//  than a combo box of ruling names (which says nothing about what the page will look like), the
//  popup shows the pages themselves.  It opens on a short row - the document default, the last layouts
//  used, and a "+" - since "another one of those" should stay a single tap.  The "+" turns the popup
//  into the full grid: three lined, three squared, three dotted and three special layouts, then the
//  user's own custom layouts and a tile to make another.  Right-click or long press on any tile edits
//  it; a built-in layout is fixed, so editing one saves a custom copy.
//
// Each tile is split in two: the whole page on the left, and the rest of the tile a window onto the
//  ruling at 1:1 - the size it will actually be on screen at the current zoom - since "Medium ruled"
//  at thumbnail size says nothing about whether it fits the user's handwriting.
namespace AddPageMenu {

// A layout is geometry only.  Paper and rule colors always come from the document (its theme), never
//  from the layout, so a layout picked in a dark-paper document is dark - the same thing a theme
//  switch does to existing pages.  A layout carrying its own colors would put light-theme blue rules
//  on dark paper, which is exactly what the old presets did.
struct PageLayout {
  const char* name = "";  // built-in layouts only; custom ones are named from their geometry
  Dim xRuling = 0;
  Dim yRuling = 0;
  Dim marginLeft = 0;
  Dim dotRadius = 0;
  bool staff = false;  // music staves; yRuling is then one staff band (rulingregion.h)
  // 0 = the document's default page size
  Dim width = 0;
  Dim height = 0;
};

PageLayout layoutFromProps(const PageProperties& props);
// the page a layout makes in this document: default size where the layout has none, theme colors
PageProperties layoutToProps(const PageLayout& layout, const ScribbleConfig* cfg);
std::string layoutDescription(const PageLayout& layout);

// custom layouts, persisted in the global config as "customPageLayouts"
std::vector<PageLayout> customLayouts(const ScribbleConfig* cfg);
void setCustomLayouts(ScribbleConfig* cfg, const std::vector<PageLayout>& layouts);

// page units -> UI units in the active view, i.e. the scale at which "1:1" previews are drawn
Dim screenScale();

// a page drawn at thumbnail size: paper, ruling and margin, exactly as the page will look.  One <g> at
//  the origin, sized to fit boxw x boxh - the caller places it.  lensScale > 0 adds a magnifying glass
//  over the page showing the ruling at that scale (the ruling dialog passes screenScale()).
SvgNode* createPagePreviewNode(const PageProperties& props, Dim boxw, Dim boxh, Dim lensScale = 0);

// the layout a new page gets from this config, size 0 so it follows the default page size
PageLayout defaultLayout(const ScribbleConfig* cfg);
bool sameLayout(const PageLayout& a, const PageLayout& b);
// one layout as a config string, in the same form as the custom layout list
std::string layoutToString(const PageLayout& layout);
bool layoutFromString(const char* str, PageLayout* layout);

// The Add Page grid - built-in layouts by category, then custom ones - added to container as rows, with
//  a tap on a tile calling onPick instead of adding a page.  For choosing a layout somewhere else (the
//  new document dialog).  Returns the tiles in grid order, e.g. to mark the selected one.
std::vector<Button*> createLayoutGrid(Widget* container, const ScribbleConfig* cfg,
    const std::function<void(const PageLayout&)>& onPick);

// the button itself, popup and all; scanPageAction supplies the "scan as page" item
Button* createAddPageButton(Action* scanPageAction);
// opens the popup of the Add Page button most recently created (no-op if there is none)
void showAddPagePopup();

// The "+" beside Add Page: a menu of things to add to this page, one entry per action given (Paper, Patch,
//  Insert Document, Insert Photo - docs/agent/paper.md)
Button* createAddMenuButton(const std::vector<Action*>& actions);

}  // namespace AddPageMenu
