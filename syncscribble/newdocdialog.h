#pragma once

#include <string>
#include <vector>
#include "ugui/widgets.h"
#include "ugui/textedit.h"
#include "addpagemenu.h"

class TagStore;
class ColorEditBox;

// a cover at the size the dialogs preview it; Color(0) draws "No cover"
SvgNode* createCoverPreviewNode(Color seed);

// what the dialog chose, carried from the document browser to ScribbleApp::applyNewDocChoices()
struct NewDocChoices {
  Color coverColor = Color(0);  // no cover when 0
  AddPageMenu::PageLayout layout;
  std::vector<std::string> tagIds;
};

// "Create Notebook": what the document browser's + opens.  Everything a new document is given before
//  its first stroke, in one place - its name, its cover, the layout of its pages, and its tags - so none
//  of it has to be found in a menu afterwards.  (The theme is still asked separately, after this, by
//  ScribbleApp::askThemeForNewDoc().)
//
// The dialog decides nothing about files: it returns its choices, TagDocList creates the file, and
//  ScribbleApp applies them to the opened document (applyNewDocChoices()), since only there is a
//  ScribbleDoc to apply them to.
//
// The cover and page choices are remembered for the next new document (newDocCover, newDocCoverColor,
//  newDocLayout) - most notebooks someone makes are like the last one they made.
class CreateNotebookDialog : public PopupDialog
{
public:
  // tagStore: the browser's tag tree; tags created here are added to it (and saved) straight away
  CreateNotebookDialog(const char* initialName, TagStore* tagStore, const std::vector<std::string>& initialTags);

  std::string name() const;
  bool hasCover() const { return coverOn; }
  Color coverColor() const { return coverSeed; }
  const AddPageMenu::PageLayout& pageLayout() const { return layout; }
  const std::vector<std::string>& tagIds() const { return tags; }

private:
  Button* createTile(const char* label);
  void setTilePreview(Button* tile, SvgNode* preview);
  void updateCoverTile();
  void updatePagesTile();
  void setCoverSeed(Color seed);
  void rebuildTagChips();
  void rebuildTagList();
  void toggleTag(const std::string& tagId);
  void createTagFromQuery();
  void saveChoices();

  TagStore* tagStore;
  bool coverOn = true;
  Color coverSeed;
  AddPageMenu::PageLayout layout;
  std::vector<std::string> tags;
  std::string tagQuery;

  TextEdit* nameEdit;
  Button* coverTile;
  Button* pagesTile;
  CheckBox* coverToggle;
  ColorEditBox* customColorEdit;
  Widget* tagChips;
  Button* addTagsBtn;
  ArrowPopup* tagsPopup;
  TextEdit* tagSearchEdit;
  Widget* tagList;
};

// "Change Cover", from a document's menu in the browser: the same covers Create Notebook offers, plus
//  none.  Like that dialog it touches no file - TagDocList::changeCover() writes the choice.
class ChangeCoverDialog : public PopupDialog
{
public:
  ChangeCoverDialog(Color current);  // Color(0): the document has no cover
  Color coverColor() const { return cover; }  // Color(0) for none

private:
  void setCover(Color seed);
  void updatePreview();

  Color cover;
  Widget* previewHolder;
  ColorEditBox* customColorEdit;
};
