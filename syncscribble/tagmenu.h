#pragma once

#include "ugui/widgets.h"

// The toolbar's tag button (docs/agent/page-tags.md).  Its popup asks first whether the tags are for this
//  page or the whole notebook, then shows the library's tags as a checklist with search-or-create.
//  - Whole notebook: each tick changes the open notebook's own tags at once (ScribbleDoc::setDocTags()).
//  - This page: nothing happens until Done.  Tags unticked come off the page (one undo step), and tags
//    newly ticked ride on the pointer until a press places them (ScribbleArea::startTagPlacement()).
namespace TagMenu {

Button* createTagButton();

}
