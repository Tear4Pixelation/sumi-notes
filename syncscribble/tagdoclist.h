#ifndef TAGDOCLIST_H
#define TAGDOCLIST_H

#include "ugui/widgets.h"
#include "ugui/textedit.h"
#include "basics.h"
#include "tagstore.h"
#include "newdocdialog.h"
#include "rowdrag.h"
#include <map>
#include <memory>
#include <set>

class Document;
class ScribbleConfig;

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
  // with EXISTING_DOC, the page to open selectedFile at: a page card was picked (-1 = where it was left)
  int selectedPage = -1;
  std::vector<std::string> selectedPageTags;  // and the tags on it to point out (ScribbleArea::flashPageTags())
  // IMPORT_*: the import FAB's menu picked a format; ScribbleApp runs the system picker and the import
  enum Result_t {REJECTED = 0, EXISTING_DOC, NEW_DOC, OPEN_WHITEBOARD, IMPORT_PDF, IMPORT_NOTEFUL, IMPORT_DOC} result;
  static bool isImport(int res) { return res == IMPORT_PDF || res == IMPORT_NOTEFUL || res == IMPORT_DOC; }

  // hasCurrentNote: whether a "back to note" FAB should be offered (there is a real document open
  // behind this browser, as opposed to it being the very first thing shown at startup).
  void setup(Window* parent, bool hasCurrentNote);
  void finish(Result_t res);
  // points the browser at a different document root (the library was moved); safe while it is shown
  void setRoot(const char* root);
  const FSPath& root() const { return docRoot; }

  std::function<void(int)> onFinished;

  // what the Create Notebook dialog chose, for ScribbleApp to apply once the NEW_DOC file is open
  NewDocChoices newDocChoices;

protected:
  void refresh();
  void rebuildTagTree();
  void elideTagRowTitle(Widget* row, int depth);
  void rebuildDocGrid();
  void toggleTagFilter(const std::string& tagId);
  void toggleMultiSelect();
  void showTagMenu(const std::string& tagId, Widget* row);
  void addTag(const std::string& parentId);
  void renameTag(const std::string& tagId);
  void deleteTagWithUndo(const std::string& tagId, bool deleteChildren,
      const std::vector<FSPath>& retagDocs = {});
  std::vector<FSPath> docsWithTag(const std::string& tagId, bool subtree);
  void undoTagDelete();
  void hideUndo();
  void newDoc();
  void showDocMenu(const FSPath& path, Widget* cell);
  void showDocTagsPopup(const FSPath& path, Widget* cell);
  void rebuildDocTagsList();
  Widget* createDocTagRow(const std::string& tagId, int depth, const std::vector<std::string>& currentTags);
  void renameDoc(const FSPath& path);
  void deleteDoc(const FSPath& path);
  // select mode: tapping a document toggles it instead of opening it, and selectBar's actions replace
  //  the FABs - see setSelectMode()
  void setSelectMode(bool on);
  void toggleDocSelected(const std::string& path);
  void setAllSelected(bool selected);
  void deleteSelectedDocs();
  void updateSelectBar();
  void setDocumentTags(const FSPath& path, const std::vector<std::string>& tagIds);
  // Load a document whole, let `edit` change it and its config, and save it back, keeping its
  //  thumbnail and bringing the page tag summary and the tag cache up to date.  See setDocumentTags()
  //  for why every page has to be loaded first.  False if it could not be loaded or saved.
  bool rewriteDocument(const FSPath& path, const std::function<void(Document&, ScribbleConfig&)>& edit);
  void closeAllContextPopups();

private:
  struct DocEntry {
    FSPath path;
    std::vector<std::string> tagIds;
    std::vector<TagStore::PageTags> pageTags;  // docs/agent/page-tags.md
  };
  // documents with a page carrying one of tagIds
  std::vector<FSPath> docsWithPageTag(const std::vector<std::string>& tagIds);

  // Recursively lists every document under root (ignoring directory structure entirely -- a
  // directory is just a container to descend into, never a browsable unit), resolving each one's
  // tags from the TagStore cache when the cache's mtime still matches, else via
  // ScribbleDoc::extractDocTags() (which re-populates the cache for next time).
  std::vector<DocEntry> collectDocuments();
  Widget* createTagRow(const std::string& tagId, int depth);
  Widget* createNavRow(const char* iconPath, const char* title);
  Widget* createFab(const char* iconPath, real diameter, bool primary);
  Button* createSelectBarButton(const char* iconPath, const char* tooltip);
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

  // Drag a tag row onto another to make it a subtag, or onto All Documents to make it a root tag.
  //  RowDrag identifies rows by int; a tag's key is its index in dragTagIds, and it keeps that key for
  //  the life of the window, since the drop is delivered after the event (RowDrag::drop) and a rebuild
  //  in between must not make a key name a different tag.
  std::unique_ptr<RowDrag> tagDrag;
  std::vector<std::string> dragTagIds;
  std::map<std::string, int> dragTagKeys;
  void moveTagUnder(const std::string& tagId, const std::string& parentId, const std::string& afterId = "");
  bool isTagDescendant(const std::string& tagId, const std::string& ancestorId) const;

  // Undo for the last tag deletion (see tagstore.h's restoreTag()/reparentTag() for how the tree
  // itself is put back); documents' own tag lists need undoing only where the deleted tag was replaced
  // by its parent (parentAddedTo) -- see deleteTagWithUndo()'s comment.
  struct DeleteSnapshot {
    bool valid = false;
    std::vector<TagNode> removedInOrder;      // parent-before-child; empty unless subtree was cascaded
    std::vector<std::string> reparentedBack;  // children that were reparented to removedInOrder[0]'s
                                               // old parent and need to move back under it on undo
    std::string label;
    std::string parentId;
    std::vector<FSPath> parentAddedTo;  // documents given parentId in place of the deleted tag
    // Page tags are removed from the pages when their tag is deleted (unlike a document's own tags,
    //  which go inert), so undo puts the files back as they were - unless one has changed since.
    struct FileBackup { FSPath path; std::string contents; time_t mtime; };
    std::vector<FileBackup> pageTagFiles;
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
  Button* whiteboardFab;
  Button* importFab;
  Button* addDocFab;
  Button* backNoteFab;
  Button* selectFab;
  Button* undoButton;
  Widget* fabRow;

  // Select mode.  Selection is by path and survives a grid rebuild, but is pruned to the documents
  //  the grid shows (search, tag filter), so Delete never touches a document the user cannot see.
  //  Page cards are not selectable: whether deleting one means the page or its notebook is unclear.
  bool selectMode = false;
  std::set<std::string> selectedDocs;
  std::vector<std::string> shownDocs;  // documents in the grid, in grid order
  std::multimap<std::string, Button*> docCells;  // a notebook can be in both groups of the grid
  Widget* selectBar;
  SvgText* selectCountText;
  Button* selectAllBtn;
  Button* selectNoneBtn;
  Button* deleteSelectedBtn;

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
