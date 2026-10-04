#include "tagmenu.h"

#include <algorithm>
#include <functional>
#include <memory>
#include "ugui/textedit.h"
#include "scribbleapp.h"
#include "scribbledoc.h"
#include "tagstore.h"
#include "resources.h"
#include "ulib/stringutil.h"

namespace TagMenu {

// a long tag tree is found faster by typing than by scrolling a popup
static const int MAX_ROWS = 14;

// Everything the popup's widgets share.  Owned by the lambdas on those widgets, which live as long as the
//  button; the widgets themselves belong to the popup.
struct State {
  ArrowPopup* popup = NULL;
  Widget* choiceView = NULL;
  Widget* tagsView = NULL;
  TextBox* tagsTitle = NULL;
  TextEdit* searchEdit = NULL;
  Widget* tagList = NULL;
  CheckBox* todoCheck = NULL;  // This Page only: the tags placed get a checkbox
  std::unique_ptr<TagStore> store;
  bool pageMode = false;
  int pagenum = -1;
  std::vector<std::string> initialTags;  // what the page or notebook had when the list was opened
  std::vector<std::string> tags;  // what is ticked now
  std::string query;

  SvgGui* gui() const { return popup->window() ? popup->window()->gui() : NULL; }
  void showChoice();
  void showTags(bool forPage);
  void rebuildList();
  void toggle(const std::string& tagId);
  void createFromQuery();
  void done();
};

void State::showChoice()
{
  // tags made in the document browser since the last open
  store.reset(new TagStore(FSPath(ScribbleApp::app->tagBrowserRoot()).child(".write-tags").c_str()));
  store->load();
  choiceView->setVisible(true);
  tagsView->setVisible(false);
}

void State::showTags(bool forPage)
{
  ScribbleDoc* doc = ScribbleApp::app->activeDoc();
  if(!doc)
    return;
  pageMode = forPage;
  if(forPage) {
    pagenum = ScribbleApp::app->activeArea()->getCurrPageNum();
    if(pagenum < 0 || pagenum >= doc->document->numPages())
      return;
    initialTags = doc->document->pages[pagenum]->pageTagIds;
    tagsTitle->setText(fstring(_("Tag Page %d"), pagenum + 1).c_str());
  }
  else {
    initialTags = TagStore::parseTagList(doc->cfg->String("tags", ""));
    tagsTitle->setText(_("Tag Notebook"));
  }
  tags = initialTags;
  // a to-do is a choice for these tags, not a setting, so it starts unticked each time
  todoCheck->setChecked(false);
  todoCheck->setVisible(forPage);
  query.clear();
  searchEdit->setText("");
  choiceView->setVisible(false);
  tagsView->setVisible(true);
  rebuildList();
  if(gui())
    gui()->setFocused(searchEdit, SvgGui::REASON_TAB);
}

// every tag, as a tree, or - with a query - the tags matching it, flat; ticked if the page or notebook has it.
//  A tag it has that is not in this library's index is not listed, and so stays where it is.
void State::rebuildList()
{
  if(gui())
    gui()->deleteContents(tagList);
  std::string trimmed = StringRef(query).trimmed().toString();
  std::string lquery = toLower(trimmed);
  int shown = 0;
  bool truncated = false;

  auto addRow = [&](const TagNode& tagNode, int depth) {
    if(shown >= MAX_ROWS) {
      truncated = true;
      return;
    }
    bool checked = std::find(tags.begin(), tags.end(), tagNode.id) != tags.end();
    CheckBox* checkBox = createCheckBox(tagNode.name.c_str(), checked);
    checkBox->setMargins(1, 0, 1, depth*18);
    checkBox->node->setAttribute("box-anchor", "left");
    std::string id = tagNode.id;
    // no rebuild here - the checkbox being toggled is in the list
    checkBox->onToggled = [this, id](bool){ toggle(id); };
    tagList->addWidget(checkBox);
    ++shown;
  };

  if(trimmed.empty()) {
    std::function<void(const std::string&, int)> addTree = [&](const std::string& id, int depth) {
      const TagNode* tagNode = store->tag(id);
      if(!tagNode)
        return;
      addRow(*tagNode, depth);
      for(const std::string& child : tagNode->childIds)
        addTree(child, depth + 1);
    };
    for(const std::string& id : store->rootTagIds())
      addTree(id, 0);
    if(store->rootTagIds().empty()) {
      TextBox* hint = createTextBox(_("No tags yet - type a name to create one"));
      hint->node->addClass("weak");
      tagList->addWidget(hint);
    }
  }
  else {
    bool exactmatch = false;
    for(const auto& entry : store->allTags()) {
      std::string lname = toLower(entry.second.name);
      if(lname == lquery)
        exactmatch = true;
      if(lname.find(lquery) != std::string::npos)
        addRow(entry.second, 0);
    }
    if(!exactmatch) {
      // removed by the rebuild its own click triggers, so that rebuild is deferred
      Button* createBtn = createMenuItem(fstring(_("Create tag \"%s\""), trimmed.c_str()).c_str());
      createBtn->onClicked = [this](){
        if(gui())
          gui()->setTimer(1, popup, [this](){ createFromQuery(); return 0; });
      };
      tagList->addWidget(createBtn);
    }
  }
  if(truncated) {
    TextBox* hint = createTextBox(_("More tags - type to search"));
    hint->node->addClass("weak");
    tagList->addWidget(hint);
  }
}

void State::toggle(const std::string& tagId)
{
  auto it = std::find(tags.begin(), tags.end(), tagId);
  if(it != tags.end())
    tags.erase(it);
  else
    tags.push_back(tagId);
  // the notebook's tags change as they are ticked, like the browser's Manage Tags
  if(!pageMode && ScribbleApp::app->activeDoc())
    ScribbleApp::app->activeDoc()->setDocTags(tags);
}

void State::createFromQuery()
{
  std::string trimmed = StringRef(query).trimmed().toString();
  if(trimmed.empty())
    return;
  std::string lquery = toLower(trimmed);
  std::string id;
  for(const auto& entry : store->allTags()) {
    if(toLower(entry.second.name) == lquery)
      id = entry.first;
  }
  if(id.empty()) {
    store->load();  // the browser may have changed the index since it was read
    id = store->addTag(trimmed);
    store->save();
  }
  if(std::find(tags.begin(), tags.end(), id) == tags.end())
    toggle(id);
  query.clear();
  searchEdit->setText("");
  rebuildList();
}

void State::done()
{
  bool forPage = pageMode;
  std::vector<std::string> removed, added;
  for(const std::string& id : initialTags) {
    if(std::find(tags.begin(), tags.end(), id) == tags.end())
      removed.push_back(id);
  }
  bool todo = todoCheck->isChecked();
  std::vector<std::pair<std::string, std::string>> toPlace;
  for(const std::string& id : tags) {
    if(std::find(initialTags.begin(), initialTags.end(), id) == initialTags.end()) {
      const TagNode* tagNode = store->tag(id);
      toPlace.emplace_back(id, tagNode ? tagNode->name : id);
    }
  }
  if(gui())
    gui()->closeMenus();
  ScribbleDoc* doc = ScribbleApp::app->activeDoc();
  if(!forPage || !doc)
    return;
  doc->removePageTags(pagenum, removed);
  ScribbleApp::app->activeArea()->startTagPlacement(toPlace, todo);
}

Button* createTagButton()
{
  Button* btn = createToolbutton(SvgGui::useFile(":/icons/ic_tag.svg"), _("Tags"));
  ArrowPopup* popup = createArrowPopup(Menu::VERT_LEFT);
  auto state = std::make_shared<State>();
  state->popup = popup;

  // first view: what the tags are for.  Both views are built once and only their visibility changes, since
  //  the choice is made by a press inside the first view.
  state->choiceView = createColumn({}, "", "", "left");
  TextBox* choiceTitle = createTextBox(_("Add Tags To"));
  choiceTitle->node->addClass("arrowpopup-title");
  choiceTitle->setMargins(0, 8, 4, 8);
  state->choiceView->addWidget(choiceTitle);
  Button* pageBtn = createMenuItem(_("This Page"));
  pageBtn->onClicked = [state](){ state->showTags(true); };
  state->choiceView->addWidget(pageBtn);
  Button* docBtn = createMenuItem(_("Whole Notebook"));
  docBtn->onClicked = [state](){ state->showTags(false); };
  state->choiceView->addWidget(docBtn);
  popup->addWidget(state->choiceView);

  // second view: the checklist, as in the document browser's Manage Tags
  state->tagsView = createColumn({}, "", "", "left");
  Widget* header = createRow();
  header->setMargins(0, 8, 8, 8);
  state->tagsTitle = createTextBox("");
  state->tagsTitle->node->addClass("arrowpopup-title");
  header->addWidget(state->tagsTitle);
  header->addWidget(createStretch());
  Button* doneBtn = createPushbutton(_("Done"));
  doneBtn->node->setAttribute("box-anchor", "vfill");  // see TagDocList's Done for why not fill
  doneBtn->selectFirst(".pushbtn-bg")->node->setAttribute("fill", "#2EA3CF");
  doneBtn->onClicked = [state](){ state->done(); };
  header->addWidget(doneBtn);
  state->tagsView->addWidget(header);

  state->searchEdit = createTextEdit(-1);
  state->searchEdit->setEmptyText(_("Find or create a tag"));
  state->searchEdit->setMargins(4, 12, 6, 12);
  setMinWidth(state->searchEdit, 240);
  state->searchEdit->onChanged = [state](const char* s){ state->query = s; state->rebuildList(); };
  // Enter takes the query as a tag - picking the existing one of that name, or making it
  state->searchEdit->addHandler([state](SvgGui*, SDL_Event* event){
    if(event->type == SDL_KEYDOWN && (event->key.keysym.sym == SDLK_RETURN || event->key.keysym.sym == SDLK_KP_ENTER)) {
      state->createFromQuery();
      return true;
    }
    return false;
  });
  state->tagsView->addWidget(state->searchEdit);
  state->tagList = createColumn({}, "", "", "left");
  state->tagList->setMargins(0, 12, 8, 12);
  state->tagsView->addWidget(state->tagList);
  // a ticked to-do stays on the page, but the page no longer counts as tagged (docs/agent/page-tags.md)
  state->todoCheck = createCheckBox(_("Add it as a to-do"), false);
  state->todoCheck->setMargins(0, 12, 10, 12);
  state->todoCheck->node->setAttribute("box-anchor", "left");
  state->tagsView->addWidget(state->todoCheck);
  popup->addWidget(state->tagsView);

  setupAutoClosePopup(popup);
  btn->addWidget(popup);
  setupTooltip(btn, _("Tag this page or notebook"));

  // opened on press like the Add Page button, for the same reason: shown on release, the release itself
  //  would close it again as an outside press
  btn->addHandler([btn, popup, state](SvgGui* gui, SDL_Event* event){
    if(event->type != SDL_FINGERDOWN || event->tfinger.fingerId != SDL_BUTTON_LMASK)
      return false;
    if(gui->lastClosedMenu != popup) {
      gui->closeMenus();
      // pressing it again while tags are on the pointer puts them down and starts over
      if(ScribbleApp::app->activeArea())
        ScribbleApp::app->activeArea()->cancelTagPlacement();
      state->showChoice();
      openAutoClosePopup(popup);
      btn->node->addClass("pressed");
    }
    return true;
  });
  return btn;
}

}  // namespace TagMenu
