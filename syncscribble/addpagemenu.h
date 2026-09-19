#pragma once

#include <vector>

#include "ugui/widgets.h"
#include "page.h"

class ScribbleConfig;

// The "+" button that sits next to Save: the one place pages are added from.
//
// A page's ruling belongs to that page, so adding a page is also where a ruling is chosen.  Rather
//  than a combo box of ruling names (which says nothing about what the page will look like), the
//  popup shows small drawings of the actual pages: the document's default ruling first, then the
//  three rulings most recently used to add a page, then a "+" that opens the page setup dialog for
//  this one new page.  Presets are future work; the recents list is what makes the common case -
//  "another one of those" - a single tap.
namespace AddPageMenu {

// recently used rulings, most recent first, persisted in the global config as "recentPageRulings"
std::vector<PageProperties> recentRulings(const ScribbleConfig* cfg);
void addRecentRuling(ScribbleConfig* cfg, const PageProperties& props);

// a page drawn at thumbnail size: paper, ruling lines and margin, exactly as the page will look.
// one <g> at the origin, sized to fit boxw x boxh - the caller places it (the ruling dialog shows the
//  same drawing at a larger size, so its numbers mean something while they are being typed)
SvgNode* createPagePreviewNode(const PageProperties& props, Dim boxw, Dim boxh);

// the button itself, popup and all; scanPageAction supplies the "scan as page" item
Button* createAddPageButton(Action* scanPageAction);

}  // namespace AddPageMenu
