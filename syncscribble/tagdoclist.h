#ifndef TAGDOCLIST_H
#define TAGDOCLIST_H

#include "ugui/widgets.h"
#include "ugui/textedit.h"
#include "basics.h"
#include "tagstore.h"
#include <set>

// Small prompt for naming a new tag or renaming an existing one -- deliberately not NewDocDialog,
// which is filesystem-oriented (existence checks, invalid-path characters, a ruling combo box).
class TagNameDialog : public PopupDialog
{
public:
  TagNameDialog(const char* title, const char* initialName);
  std::string getName() const { return StringRef(nameEdit->text()).trimmed().toString(); }
  TextEdit* nameEdit = NULL;
};

// The redesigned document browser: a tag sidebar (tree of tags/subtags, multi-select filter, search)
// beside a flat grid of every document under the root, filtered by the active tags and (once toggled
// on) a text search. Replaces DocumentList's folder navigation with tags exclusively -- "no folders,
// just tags" -- so unlike DocumentList this always looks at the whole tree under docRoot, never one
// directory at a time. See tagstore.h for how the tag tree and the per-document tag cache that makes
// that affordable are split between "source of truth" (each document's own config) and "index" (the
// TagStore's cache, keyed by mtime).
class TagDocList : public Window
{
public:
  TagDocList(const char* root);

  std::string selectedFile;
  enum Result_t {REJECTED = 0, EXISTING_DOC, NEW_DOC, OPEN_WHITEBOARD} result;

  // hasCurrentNote: whether a "back to note" FAB should be offered (there is a real document open
  // behind this browser, as opposed to it being the very first thing shown at startup).
  void setup(Window* parent, bool hasCurrentNote);
  void finish(Result_t res);
  // points the browser at a different document root (the library was moved); safe while it is shown
  void setRoot(const char* root);

  std::function<void(int)> onFinished;

protected:
  void refresh();
  void rebuildTagTree();
  void rebuildDocGrid();
  void toggleTagFilter(const std::string& tagId);
  void toggleMultiSelect();
  void showTagMenu(const std::string& tagId, Widget* row);
  void addTag(const std::string& parentId);
  void renameTag(const std::string& tagId);
  void deleteTagWithUndo(const std::string& tagId, bool deleteChildren);
  void undoTagDelete();
  void hideUndo();
  void toggleDocSearch();
  void newDoc();
  void showDocMenu(const FSPath& path, Widget* cell);
  void showDocTagsPopup(const FSPath& path, Widget* cell);
  void rebuildDocTagsList();
  Widget* createDocTagRow(const std::string& tagId, int depth, const std::vector<std::string>& currentTags);
  void renameDoc(const FSPath& path);
  void deleteDoc(const FSPath& path);
  void setDocumentTags(const FSPath& path, const std::vector<std::string>& tagIds);
  void closeAllContextPopups();

private:
  struct DocEntry {
    FSPath path;
    std::vector<std::string> tagIds;
  };

  // Recursively lists every document under root (ignoring directory structure entirely -- a
  // directory is just a container to descend into, never a browsable unit), resolving each one's
  // tags from the TagStore cache when the cache's mtime still matches, else via
  // ScribbleDoc::extractDocTags() (which re-populates the cache for next time).
  std::vector<DocEntry> collectDocuments();
  Widget* createTagRow(const std::string& tagId, int depth);
  Widget* createNavRow(const char* iconPath, const char* title);
  Widget* createFab(const char* iconPath, real diameter, bool primary);
  Widget* createRoundedBg(real w, real h, real radius, const char* fillColor);

  TagStore tagStore;
  FSPath docRoot;
  std::string docFileExt;
  std::string fileExts;
  int iconWidth;

  // Single-select by default (clicking a tag replaces the filter); the "Filter Check" toolbar button
  // toggles multi-select, where clicking accumulates tags into activeTags instead. With 2+ tags active,
  // rebuildDocGrid() splits the grid into documents carrying *every* active tag (shown first) and a
  // separator, then documents carrying only *some* of them (shown after) -- see rebuildDocGrid().
  bool multiSelectMode = false;
  std::set<std::string> activeTags;
  std::string tagSearchQuery;
  std::string docSearchQuery;
  std::set<std::string> expandedTags;    // supertags currently showing their children

  // Undo for the last tag deletion (see tagstore.h's restoreTag()/reparentTag() for how the tree
  // itself is put back); nothing about documents' own tag lists needs undoing since deleteTag()
  // never touches them -- see deleteTagWithUndo()'s comment.
  struct DeleteSnapshot {
    bool valid = false;
    std::vector<TagNode> removedInOrder;      // parent-before-child; empty unless subtree was cascaded
    std::vector<std::string> reparentedBack;  // children that were reparented to removedInOrder[0]'s
                                               // old parent and need to move back under it on undo
    std::string label;
  } lastDelete;

  Widget* tagTreeView;
  Widget* docGrid;
  ScrollWidget* docScrollWidget;
  TextEdit* tagSearchEdit;
  TextEdit* docSearchEdit;
  Widget* docSearchRow;
  Button* allDocumentsBtn;
  Button* newTagBtn;
  Button* multiSelectBtn;
  Button* searchFab;
  Button* whiteboardFab;
  Button* addDocFab;
  Button* backNoteFab;
  Button* undoButton;

  // Right-click/long-press context menus in this app are ArrowPopups, not plain Menus - see
  // showTagMenu()'s comment for the mechanics (an ArrowPopup has no click-point anchoring; it has to
  // be reparented onto the triggering row/cell before each show). One popup instance per menu is
  // built once in createUI() and reused, matching DocumentList's own persistent contextMenu.
  ArrowPopup* tagContextPopup;
  std::string contextMenuTagId;

  // Document long-press menu (Manage Tags / Rename / Delete) and the tag-assignment popup it opens.
  ArrowPopup* docContextPopup;
  ArrowPopup* docTagsPopup;
  TextEdit* docTagsSearchEdit;
  Widget* docTagsList;
  std::string docTagsSearchQuery;
  std::set<std::string> docTagsExpandedTags;  // mirrors expandedTags, but for the tree in docTagsList
  FSPath contextMenuDocPath;

  std::unique_ptr<SvgNode> gridItemProto;
  std::unique_ptr<SvgUse> fileUseNode;

  void createUI();
};

#endif // TAGDOCLIST_H
