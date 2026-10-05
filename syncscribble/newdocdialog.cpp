#include "newdocdialog.h"

#include "ugui/colorwidgets.h"
#include "scribbleapp.h"
#include "scribbleconfig.h"
#include "tagstore.h"
#include "cover.h"

// The two tiles are the size of a small notebook, not of a swatch: the cover is the thing the document
//  list will show for this notebook from now on, so it is previewed at close to that size.
static const Dim TILE_PREVIEW_W = 110;
static const Dim TILE_PREVIEW_H = 156;  // TILE_PREVIEW_W*Cover::PREVIEW_ASPECT: the frame the list draws
static const Dim TILE_PAD = 5;
static const Dim SWATCH_W = 30;
static const Dim SWATCH_H = 42;
static const int SWATCH_COLS = 6;
static const Color DEFAULT_COVER = Color(0x2E, 0x4C, 0x6D);

static void closePopups(Widget* widget)
{
  SvgGui* gui = widget->window() ? widget->window()->gui() : NULL;
  if(gui)
    gui->closeMenus();
}

// A grid of the preset seeds, each drawn as the cover it makes
static Widget* createCoverSwatchGrid(const std::function<void(Color)>& onPick)
{
  Widget* swatchGrid = createRow({}, "0 0", "left");
  swatchGrid->node->setAttribute("flex-wrap", "wrap");
  swatchGrid->node->setAttribute("margin", "6 6 2 6");
  const std::vector<Color>& seeds = Cover::presetSeeds();
  for(size_t ii = 0; ii < seeds.size(); ++ii) {
    Color seed = seeds[ii];
    Button* swatch = new Button(loadSVGFragment(fstring(
        "<g class='toolbutton cover-swatch' margin='2 2'>"
          "<rect class='background' width='%g' height='%g' rx='3' ry='3'/>"
          "<g transform='translate(%g %g)'>%s</g>"
        "</g>", SWATCH_W + 6, SWATCH_H + 6, 3.0, 3.0, Cover::coverSVG(seed, SWATCH_W, SWATCH_H).c_str()).c_str()));
    if(ii > 0 && ii % SWATCH_COLS == 0)
      swatch->node->setAttribute("flex-break", "before");
    swatch->onClicked = [onPick, seed](){ onPick(seed); };
    swatchGrid->addWidget(swatch);
  }
  return swatchGrid;
}

CreateNotebookDialog::CreateNotebookDialog(const char* initialName, TagStore* store, const std::vector<std::string>& initialTags)
    : PopupDialog(createPopupDialogNode()), tagStore(store), tags(initialTags)
{
  const ScribbleConfig* cfg = ScribbleApp::cfg;
  coverOn = cfg->Bool("newDocCover", true);
  int savedcolor = cfg->Int("newDocCoverColor", 0);
  coverSeed = savedcolor ? Color::fromArgb(savedcolor) : DEFAULT_COVER;
  if(!AddPageMenu::layoutFromString(cfg->String("newDocLayout", ""), &layout))
    layout = AddPageMenu::defaultLayout(cfg);

  Widget* dialogBody = selectFirst(".body-container");
  dialogBody->setMargins(4, 8);

  // cover tile and its popup: a grid of seeds, each drawn as the cover it makes, then a custom color
  coverTile = createTile(_("Cover"));
  ArrowPopup* coverPopup = createArrowPopup(Menu::VERT_LEFT);
  Widget* swatchGrid = createCoverSwatchGrid([this](Color seed){
    setCoverSeed(seed);
    closePopups(coverTile);
  });
  coverPopup->addWidget(swatchGrid);
  // any other color: the band is generated from it the same way, so a custom cover is still one color
  // not given the seed until the popup opens: its sliders' gradients are resolved by reference, which
  //  only works once the widget is in a document, and setColor() here segfaults
  customColorEdit = createColorEditBox(false);
  customColorEdit->onColorChanged = [this](Color color){ setCoverSeed(color.opaque()); };
  Widget* customRow = createTitledRow(_("Custom"), customColorEdit);
  customRow->setMargins(4, 6, 6, 6);
  coverPopup->addWidget(customRow);
  setupPopupMenu(coverTile, coverPopup);
  coverTile->onPressed = [this](){ customColorEdit->setColor(coverSeed); };
  setupTooltip(coverTile, _("Cover color"));

  // pages tile: the same layouts the Add Page button offers
  pagesTile = createTile(_("Pages"));
  ArrowPopup* pagesPopup = createArrowPopup(Menu::VERT_LEFT);
  Widget* layoutGrid = createColumn({}, "", "", "left");
  layoutGrid->setMargins(6, 6);
  AddPageMenu::createLayoutGrid(layoutGrid, cfg, [this](const AddPageMenu::PageLayout& picked){
    layout = picked;
    updatePagesTile();
    closePopups(pagesTile);
  });
  pagesPopup->addWidget(layoutGrid);
  setupPopupMenu(pagesTile, pagesPopup);
  setupTooltip(pagesTile, _("Page layout"));

  // name, tags and the cover toggle, beside the tiles
  nameEdit = createTextEdit();
  nameEdit->setText(initialName);
  nameEdit->setEmptyText(_("Name"));
  setMinWidth(nameEdit, 240);
  nameEdit->node->setAttribute("box-anchor", "hfill");
  focusedWidget = nameEdit;
  nameEdit->selectAll();

  // chips for the tags the notebook will carry, then "Add tags", which stays put while chips come and go
  //  (its popup is its child, and rebuilding it would take the open popup with it)
  tagChips = createRow({}, "", "left", "left");
  tagChips->node->setAttribute("flex-wrap", "wrap");
  addTagsBtn = new Button(loadSVGFragment(
      "<g class='toolbutton tag-chip' layout='box' margin='2 2'>"
        "<rect class='background' box-anchor='fill' width='20' height='24' rx='12' ry='12'"
            " fill='#808080' fill-opacity='0.25'/>"
        "<text class='title' margin='4 12' font-size='12'></text>"
      "</g>"));
  addTagsBtn->setText(_("+ Add tags"));
  tagsPopup = createArrowPopup(Menu::VERT_LEFT);
  tagSearchEdit = createTextEdit(-1);
  tagSearchEdit->setEmptyText(_("Find or create a tag"));
  tagSearchEdit->setMargins(6, 8, 4, 8);
  setMinWidth(tagSearchEdit, 220);
  tagSearchEdit->onChanged = [this](const char* s){ tagQuery = s; rebuildTagList(); };
  // Enter takes the query as a tag - picking the existing one of that name, or making it
  tagSearchEdit->addHandler([this](SvgGui*, SDL_Event* event){
    if(event->type == SDL_KEYDOWN && (event->key.keysym.sym == SDLK_RETURN || event->key.keysym.sym == SDLK_KP_ENTER)) {
      createTagFromQuery();
      return true;
    }
    return false;
  });
  tagsPopup->addWidget(tagSearchEdit);
  tagList = createColumn({}, "", "", "left");
  tagList->setMargins(2, 8, 6, 8);
  tagsPopup->addWidget(tagList);
  setupPopupMenu(addTagsBtn, tagsPopup);
  addTagsBtn->onPressed = [this](){
    tagQuery.clear();
    tagSearchEdit->setText("");
    rebuildTagList();
    SvgGui* gui = window() ? window()->gui() : NULL;
    if(gui)
      gui->setFocused(tagSearchEdit, SvgGui::REASON_TAB);
  };
  Widget* tagsRow = createRow({tagChips, addTagsBtn}, "", "", "left");
  tagsRow->node->setAttribute("flex-wrap", "wrap");
  tagsRow->setMargins(14, 0, 4, 0);  // clear of the name field's selection handles

  coverToggle = createCheckBox(_("Cover"), coverOn);
  setupTooltip(coverToggle, _("Show a cover for this notebook in the document list, instead of its first page"));
  coverToggle->node->setAttribute("box-anchor", "left");
  coverToggle->onToggled = [this](bool on){ coverOn = on; updateCoverTile(); };

  Widget* fields = createColumn({nameEdit, tagsRow, coverToggle}, "", "", "left");
  fields->node->setAttribute("box-anchor", "top");
  fields->setMargins(0, 0, 0, 10);
  Widget* tiles = createRow({coverTile, pagesTile}, "0 0", "left");
  tiles->node->setAttribute("box-anchor", "top");
  dialogBody->addWidget(createRow({tiles, fields}, "0 0", "left"));

  updateCoverTile();
  updatePagesTile();
  rebuildTagChips();

  setTitle(_("Create Notebook"));
  acceptBtn = addButton(_("Create"), [this](){ saveChoices(); finish(ACCEPTED); });
  cancelBtn = addButton(_("Cancel"), [this](){ finish(CANCELLED); });
}

std::string CreateNotebookDialog::name() const
{
  return StringRef(nameEdit->text()).trimmed().toString();
}

// A tile: a fixed-size well the preview is centred in (so switching a letter page for a square one does
//  not move anything), and its label underneath
Button* CreateNotebookDialog::createTile(const char* label)
{
  Button* tile = new Button(loadSVGFragment(fstring(
      "<g class='toolbutton newdoc-tile' layout='flex' flex-direction='column' margin='0 6 0 0'>"
        "<g class='tile-holder' layout='box'>"
          "<rect class='background' width='%g' height='%g' rx='4' ry='4'/>"
        "</g>"
        "<text class='title' margin='4 0 0 0' font-size='13'></text>"
      "</g>", TILE_PREVIEW_W + 2*TILE_PAD, TILE_PREVIEW_H + 2*TILE_PAD).c_str()));
  tile->setText(label);
  return tile;
}

void CreateNotebookDialog::setTilePreview(Button* tile, SvgNode* preview)
{
  Widget* holder = tile->selectFirst(".tile-holder");
  SvgGui* gui = window() ? window()->gui() : NULL;
  if(gui)
    gui->deleteContents(holder, ".tile-preview");
  Widget* previewWidget = new Widget(preview);
  previewWidget->node->addClass("tile-preview");
  holder->addWidget(previewWidget);
}

void CreateNotebookDialog::updateCoverTile()
{
  // with the cover off the tile stays, as an outline: the choice is kept, and turning the cover back on
  //  brings it back
  setTilePreview(coverTile, createCoverPreviewNode(coverOn ? coverSeed : Color(0)));
  coverTile->setEnabled(coverOn);
}

void CreateNotebookDialog::updatePagesTile()
{
  PageProperties props = AddPageMenu::layoutToProps(layout, ScribbleApp::cfg);
  setTilePreview(pagesTile, AddPageMenu::createPagePreviewNode(props, TILE_PREVIEW_W, TILE_PREVIEW_H));
  setupTooltip(pagesTile, AddPageMenu::layoutDescription(layout).c_str());
}

void CreateNotebookDialog::setCoverSeed(Color seed)
{
  coverSeed = seed;
  if(customColorEdit->color() != seed && customColorEdit->window())
    customColorEdit->setColor(seed);
  updateCoverTile();
}

void CreateNotebookDialog::rebuildTagChips()
{
  SvgGui* gui = window() ? window()->gui() : NULL;
  if(gui)
    gui->deleteContents(tagChips);
  for(const std::string& tagId : tags) {
    const TagNode* tagNode = tagStore ? tagStore->tag(tagId) : NULL;
    if(!tagNode)
      continue;
    Button* chip = new Button(loadSVGFragment(
        "<g class='toolbutton tag-chip' layout='box' margin='2 2'>"
          "<rect class='background' box-anchor='fill' width='20' height='24' rx='12' ry='12'"
              " fill='#808080' fill-opacity='0.25'/>"
          "<text class='title' margin='4 12' font-size='12'></text>"
        "</g>"));
    chip->setText((tagNode->name + "  ×").c_str());
    setupTooltip(chip, _("Remove tag"));
    std::string id = tagId;
    // deferred: rebuilding the chips from a chip's own click would free the widget being dispatched to
    chip->onClicked = [this, id](){
      tags.erase(std::remove(tags.begin(), tags.end(), id), tags.end());
      if(window())
        window()->gui()->setTimer(1, this, [this](){ rebuildTagChips(); return 0; });
    };
    tagChips->addWidget(chip);
  }
}

// every tag, as a tree, or - with a query - the tags matching it, flat; checked if the notebook has it
void CreateNotebookDialog::rebuildTagList()
{
  SvgGui* gui = window() ? window()->gui() : NULL;
  if(gui)
    gui->deleteContents(tagList);
  if(!tagStore)
    return;
  std::string query = StringRef(tagQuery).trimmed().toString();
  std::string lquery = toLower(query);
  bool exactmatch = false;

  auto addRow = [this](const TagNode& tagNode, int depth) {
    bool checked = std::find(tags.begin(), tags.end(), tagNode.id) != tags.end();
    CheckBox* tagCheckBox = createCheckBox(tagNode.name.c_str(), checked);
    tagCheckBox->setMargins(1, 0, 1, depth*18);
    tagCheckBox->node->setAttribute("box-anchor", "left");
    std::string id = tagNode.id;
    // no rebuild of the list here - the checkbox being toggled is in it
    tagCheckBox->onToggled = [this, id](bool){ toggleTag(id); };
    tagList->addWidget(tagCheckBox);
  };

  if(query.empty()) {
    std::function<void(const std::string&, int)> addTree = [&](const std::string& id, int depth) {
      const TagNode* tagNode = tagStore->tag(id);
      if(!tagNode)
        return;
      addRow(*tagNode, depth);
      for(const std::string& child : tagNode->childIds)
        addTree(child, depth + 1);
    };
    for(const std::string& id : tagStore->rootTagIds())
      addTree(id, 0);
    if(tagStore->rootTagIds().empty()) {
      TextBox* hint = createTextBox(_("No tags yet - type a name to create one"));
      hint->node->addClass("weak");
      tagList->addWidget(hint);
    }
    return;
  }

  for(const auto& entry : tagStore->allTags()) {
    const TagNode& tagNode = entry.second;
    std::string lname = toLower(tagNode.name);
    if(lname == lquery)
      exactmatch = true;
    if(lname.find(lquery) != std::string::npos)
      addRow(tagNode, 0);
  }
  if(!exactmatch) {
    // a menu-style row: it is removed by the rebuild its own click triggers, so that rebuild is deferred
    Button* createBtn = createMenuItem(fstring(_("Create tag \"%s\""), query.c_str()).c_str());
    createBtn->onClicked = [this](){
      if(window())
        window()->gui()->setTimer(1, this, [this](){ createTagFromQuery(); return 0; });
    };
    tagList->addWidget(createBtn);
  }
}

void CreateNotebookDialog::toggleTag(const std::string& tagId)
{
  auto it = std::find(tags.begin(), tags.end(), tagId);
  if(it != tags.end())
    tags.erase(it);
  else
    tags.push_back(tagId);
  rebuildTagChips();
}

void CreateNotebookDialog::createTagFromQuery()
{
  std::string query = StringRef(tagQuery).trimmed().toString();
  if(query.empty() || !tagStore)
    return;
  std::string lquery = toLower(query);
  std::string id;
  for(const auto& entry : tagStore->allTags()) {
    if(toLower(entry.second.name) == lquery)
      id = entry.first;
  }
  if(id.empty()) {
    id = tagStore->addTag(query);
    tagStore->save();
  }
  if(std::find(tags.begin(), tags.end(), id) == tags.end())
    tags.push_back(id);
  tagQuery.clear();
  tagSearchEdit->setText("");
  rebuildTagList();
  rebuildTagChips();
}

void CreateNotebookDialog::saveChoices()
{
  ScribbleConfig* cfg = ScribbleApp::cfg;
  cfg->set("newDocCover", coverOn);
  cfg->set("newDocCoverColor", int(coverSeed.argb()));
  cfg->set("newDocLayout", AddPageMenu::layoutToString(layout).c_str());
}

// the cover as the list will draw it, at tile size; Color(0) is no cover - a dashed outline saying so
SvgNode* createCoverPreviewNode(Color seed)
{
  real radius = Cover::previewRadius(TILE_PREVIEW_W);
  std::string svg = seed.argb() ? Cover::coverSVG(seed, TILE_PREVIEW_W, TILE_PREVIEW_H)
      : fstring("<g><rect width='%g' height='%g' rx='%g' ry='%g' fill='none' stroke='#808080'"
          " stroke-width='1' stroke-dasharray='4 3'/>"
          "<text x='%g' y='%g' text-anchor='middle' font-size='12' fill='#808080'>%s</text></g>",
          TILE_PREVIEW_W, TILE_PREVIEW_H, radius, radius, TILE_PREVIEW_W/2, TILE_PREVIEW_H/2 + 4, _("No cover"));
  return loadSVGFragment(svg.c_str());
}

ChangeCoverDialog::ChangeCoverDialog(Color current) : PopupDialog(createPopupDialogNode()), cover(current)
{
  Widget* dialogBody = selectFirst(".body-container");
  dialogBody->setMargins(4, 8);

  previewHolder = new Widget(loadSVGFragment(fstring(
      "<g class='tile-holder' layout='box' margin='0 6 0 0'>"
        "<rect fill='none' width='%g' height='%g'/>"
      "</g>", TILE_PREVIEW_W + 2*TILE_PAD, TILE_PREVIEW_H + 2*TILE_PAD).c_str()));
  previewHolder->node->setAttribute("box-anchor", "top");

  Widget* swatchGrid = createCoverSwatchGrid([this](Color seed){ setCover(seed); });
  // the list then shows the notebook's first page, as Create Notebook's Cover toggle does
  Button* noCoverBtn = createPushbutton(_("No cover"));
  noCoverBtn->onClicked = [this](){ setCover(Color(0)); };
  // any other color, in a popup for the same reason as Create Notebook's: the editor cannot be given a
  //  color before it is in a document, so it gets one when the popup opens
  Button* customBtn = createPushbutton(_("Custom color..."));
  ArrowPopup* customPopup = createArrowPopup(Menu::VERT_LEFT);
  customColorEdit = createColorEditBox(false);
  customColorEdit->onColorChanged = [this](Color color){ setCover(color.opaque()); };
  customColorEdit->setMargins(6, 6);
  customPopup->addWidget(customColorEdit);
  setupPopupMenu(customBtn, customPopup);
  customBtn->onPressed = [this](){ customColorEdit->setColor(cover.argb() ? cover : DEFAULT_COVER); };

  noCoverBtn->setMargins(0, 8, 0, 0);
  Widget* buttonRow = createRow({noCoverBtn, customBtn}, "", "left", "left");
  buttonRow->setMargins(6, 8);
  Widget* choices = createColumn({swatchGrid, buttonRow}, "", "", "left");
  choices->node->setAttribute("box-anchor", "top");
  dialogBody->addWidget(createRow({previewHolder, choices}, "0 0", "left"));
  updatePreview();

  setTitle(_("Change Cover"));
  acceptBtn = addButton(_("Apply"), [this](){ finish(ACCEPTED); });
  cancelBtn = addButton(_("Cancel"), [this](){ finish(CANCELLED); });
}

void ChangeCoverDialog::setCover(Color seed)
{
  cover = seed;
  updatePreview();
}

void ChangeCoverDialog::updatePreview()
{
  SvgGui* gui = window() ? window()->gui() : NULL;
  if(gui)
    gui->deleteContents(previewHolder, ".tile-preview");
  Widget* previewWidget = new Widget(createCoverPreviewNode(cover));
  previewWidget->node->addClass("tile-preview");
  previewHolder->addWidget(previewWidget);
}
