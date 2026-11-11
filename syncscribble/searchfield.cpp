#include "searchfield.h"
#include "ugui/svggui.h"
#include "resources.h"

namespace {

// The X is a theme toolbutton, so its colors, hover and pressed states are the theme's own.  Its background
//  is shrunk to fit inside a search box (SvgRect::setRect: setAttribute does not move the cached geometry).
const real CLEAR_BUTTON_SIZE = 28;

class SearchEdit : public TextEdit
{
public:
  SearchEdit(SvgNode* node) : TextEdit(node) {}

  void setText(const char* text) override
  {
    TextEdit::setText(text);
    updateClearButton();
  }

  void updateClearButton()
  {
    if(clearButton)
      clearButton->setVisible(!u32string().empty());
  }

  Widget* clearButton = NULL;
};

}

SearchField createSearchField(const char* emptyText, const std::function<void(const char*)>& onQueryChanged)
{
  SvgNode* editNode = widgetNode("#textedit");
  editNode->asContainerNode()->addChild(textEditInnerNode());
  editNode->setAttribute("box-anchor", "hfill");  // as createTextEdit(-1)
  SearchEdit* edit = new SearchEdit(editNode);
  setupFocusable(edit);
  edit->setEmptyText(emptyText);

  Button* clearButton = createToolbutton(SvgGui::useFile("icons/ic_menu_cancel.svg"), _("Clear"));
  SvgRect* background = static_cast<SvgRect*>(clearButton->selectFirst(".background")->node);
  Rect bounds = background->getRect();
  background->setRect(Rect::ltwh(bounds.left, bounds.top, CLEAR_BUTTON_SIZE, CLEAR_BUTTON_SIZE));
  clearButton->node->setAttribute("box-anchor", "right");
  clearButton->setMargins(0, 8, 0, 0);
  clearButton->setVisible(false);
  edit->clearButton = clearButton;

  edit->onChanged = [edit, onQueryChanged](const char* text){
    edit->updateClearButton();
    if(onQueryChanged)
      onQueryChanged(text);
  };
  clearButton->onClicked = [edit, onQueryChanged](){
    edit->setText("");
    if(onQueryChanged)
      onQueryChanged("");
    Window* window = edit->window();
    if(window && window->gui())
      window->gui()->setFocused(edit, SvgGui::REASON_TAB);
  };

  Widget* box = new Widget(new SvgG());
  box->node->setAttribute("box-anchor", "hfill");
  box->node->setAttribute("layout", "box");
  box->addWidget(edit);
  box->addWidget(clearButton);
  return {edit, box};
}
