#pragma once

#include "ugui/textedit.h"

// A search text field with an X button that clears it (docs/agent/search-clear.md).  Every search box in the
//  app is made here, so they all behave alike: the X shows only while there is text, and pressing it empties
//  the field and runs onQueryChanged("") - the same callback the user's typing runs - then keeps focus in the field.
struct SearchField
{
  TextEdit* edit;   // the text field, for focus / setText() / margins
  Widget* widget;   // put this where the plain TextEdit would have gone (it holds the edit and the X)
};

// onQueryChanged runs for typing and for the X; setText() on the edit updates the X but does not call it
//  (as for any TextEdit).
SearchField createSearchField(const char* emptyText, const std::function<void(const char*)>& onQueryChanged);
