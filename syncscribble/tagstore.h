#pragma once

#include <string>
#include <vector>
#include <map>
#include <ctime>

// A tag (or subtag) in the sidebar's tag tree. Tags are strictly a tree, not folders on disk --
// see tagstore.cpp for why the tree itself has to live in an index file rather than be derived
// from per-document tags alone.
struct TagNode {
  std::string id;
  std::string name;
  std::string parentId;  // empty = root tag
  std::vector<std::string> childIds;
};

// Owns the tag tree (add/rename/delete, including subtags) and a per-document tag cache, both
// persisted in one index file alongside the document root. A document's own tags are stored on
// the document itself (ScribbleConfig's "tags" field, round-tripping through the document exactly
// like the theme recipe does) and are always authoritative; this cache exists only so the sidebar
// can filter/search without reopening every document on every refresh, and is rebuilt lazily by
// mtime whenever it disagrees with the file on disk.
class TagStore {
public:
  explicit TagStore(std::string indexPath);

  bool load();
  bool save() const;

  const std::map<std::string, TagNode>& allTags() const { return tags; }
  const std::vector<std::string>& rootTagIds() const { return rootIds; }
  const TagNode* tag(const std::string& id) const;
  const TagNode* findTagByName(const std::string& name, const std::string& parentId = "") const;

  // Returns the new tag's id, generated from a counter -- not derived from the name, since
  // deleteTag() with reparented children, and future renames, both need an id that never changes
  // out from under a document's stored tag list.
  std::string addTag(const std::string& name, const std::string& parentId = "");
  bool renameTag(const std::string& id, const std::string& name);
  // Deletes a tag. Children default to being reparented to the deleted tag's own parent (so a
  // supertag can be removed while keeping its subtags, per the sidebar's delete popup); pass
  // deleteChildren=true to drop the whole subtree instead. Returns the ids of every tag actually
  // removed, in case a caller needs to pull them out of documents' tag lists too.
  std::vector<std::string> deleteTag(const std::string& id, bool deleteChildren = false);

  // Undo support for deleteTag(): the caller snapshots the TagNode(s) it's about to remove (their
  // childIds field is ignored -- reinstating a subtree is a sequence of restoreTag() calls in
  // parent-before-child order, each of which relinks itself into its own parent's childIds, exactly
  // like addTag() does for a brand new tag) and replays them here to put the tree back.
  void restoreTag(const TagNode& node);
  // Moves an existing tag to a different parent, used by undo to put reparented children back under
  // the tag that was deleted (and restored) around them, and by dragging tags.  It goes right after
  // `afterId` among its new siblings if that is one of them, else last.
  void reparentTag(const std::string& id, const std::string& newParentId, const std::string& afterId = "");

  // Per-document tag cache, keyed by path relative to the index file's own directory.
  void setDocTags(const std::string& relPath, time_t mtime, const std::vector<std::string>& tagIds);
  // Returns true and fills outTagIds only if the cache entry's mtime still matches; a mismatch
  // (or missing entry) means the caller must reread the document's own tags and call setDocTags().
  bool cachedDocTags(const std::string& relPath, time_t mtime, std::vector<std::string>* outTagIds) const;
  void removeDoc(const std::string& relPath);

  static std::vector<std::string> parseTagList(const char* s);
  static std::string formatTagList(const std::vector<std::string>& tagIds);

private:
  std::string indexPath;
  std::map<std::string, TagNode> tags;
  std::vector<std::string> rootIds;
  struct DocEntry { time_t mtime; std::vector<std::string> tagIds; };
  std::map<std::string, DocEntry> docCache;
  unsigned int nextId = 1;

  void unlinkFromParent(const TagNode& node);
};
