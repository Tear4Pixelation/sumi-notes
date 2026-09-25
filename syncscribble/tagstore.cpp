#include "tagstore.h"
#include "ulib/fileutil.h"
#include "ulib/stringutil.h"

#include <cstdio>
#include <algorithm>

TagStore::TagStore(std::string _indexPath) : indexPath(std::move(_indexPath)) {}

const TagNode* TagStore::tag(const std::string& id) const
{
  auto it = tags.find(id);
  return it != tags.end() ? &it->second : nullptr;
}

const TagNode* TagStore::findTagByName(const std::string& name, const std::string& parentId) const
{
  for(const auto& pair : tags) {
    if(pair.second.parentId == parentId && pair.second.name == name)
      return &pair.second;
  }
  return nullptr;
}

std::vector<std::string> TagStore::parseTagList(const char* s)
{
  return s && s[0] ? splitStr<std::vector>(s, ',', true) : std::vector<std::string>();
}

std::string TagStore::formatTagList(const std::vector<std::string>& tagIds)
{
  return joinStr(tagIds, ",");
}

std::string TagStore::addTag(const std::string& name, const std::string& parentId)
{
  std::string id = "t" + std::to_string(nextId++);
  TagNode node;
  node.id = id;
  node.name = name;
  node.parentId = parentId;
  tags[id] = node;
  if(parentId.empty())
    rootIds.push_back(id);
  else if(TagNode* parent = const_cast<TagNode*>(tag(parentId)))
    parent->childIds.push_back(id);
  return id;
}

bool TagStore::renameTag(const std::string& id, const std::string& name)
{
  auto it = tags.find(id);
  if(it == tags.end())
    return false;
  it->second.name = name;
  return true;
}

void TagStore::unlinkFromParent(const TagNode& node)
{
  std::vector<std::string>& siblings = node.parentId.empty() ? rootIds : tags[node.parentId].childIds;
  siblings.erase(std::remove(siblings.begin(), siblings.end(), node.id), siblings.end());
}

std::vector<std::string> TagStore::deleteTag(const std::string& id, bool deleteChildren)
{
  std::vector<std::string> removed;
  auto it = tags.find(id);
  if(it == tags.end())
    return removed;

  unlinkFromParent(it->second);
  std::vector<std::string> children = it->second.childIds;  // copy: about to mutate the map
  std::string parentId = it->second.parentId;
  tags.erase(it);
  removed.push_back(id);

  for(const std::string& childId : children) {
    if(deleteChildren) {
      std::vector<std::string> childRemoved = deleteTag(childId, true);
      removed.insert(removed.end(), childRemoved.begin(), childRemoved.end());
    }
    else {
      // Reparent onto the deleted tag's own parent, preserving the subtag itself.
      TagNode& child = tags[childId];
      child.parentId = parentId;
      if(parentId.empty())
        rootIds.push_back(childId);
      else
        tags[parentId].childIds.push_back(childId);
    }
  }
  return removed;
}

void TagStore::restoreTag(const TagNode& node)
{
  TagNode restored = node;
  restored.childIds.clear();  // rebuilt incrementally as children are themselves restored/reparented
  tags[restored.id] = restored;
  if(restored.parentId.empty())
    rootIds.push_back(restored.id);
  else if(tags.count(restored.parentId))
    tags[restored.parentId].childIds.push_back(restored.id);
}

void TagStore::reparentTag(const std::string& id, const std::string& newParentId, const std::string& afterId)
{
  auto it = tags.find(id);
  if(it == tags.end())
    return;
  unlinkFromParent(it->second);
  it->second.parentId = newParentId;
  std::vector<std::string>* siblings = NULL;
  if(newParentId.empty())
    siblings = &rootIds;
  else if(tags.count(newParentId))
    siblings = &tags[newParentId].childIds;
  if(!siblings)
    return;
  auto after = std::find(siblings->begin(), siblings->end(), afterId);
  siblings->insert(after == siblings->end() ? siblings->end() : after + 1, id);
}

void TagStore::setDocTags(const std::string& relPath, time_t mtime, const std::vector<std::string>& tagIds)
{
  docCache[relPath] = DocEntry{mtime, tagIds};
}

bool TagStore::cachedDocTags(const std::string& relPath, time_t mtime, std::vector<std::string>* outTagIds) const
{
  auto it = docCache.find(relPath);
  if(it == docCache.end() || it->second.mtime != mtime)
    return false;
  if(outTagIds)
    *outTagIds = it->second.tagIds;
  return true;
}

void TagStore::removeDoc(const std::string& relPath)
{
  docCache.erase(relPath);
}

// Simple tab-separated line format, one record per line:
//   TAG\t<id>\t<parentId>\t<name>
//   DOC\t<relPath>\t<mtime>\t<comma-separated tag ids>
// Tags are written before docs, and in an order where a parent always precedes children that
// reference it, so a single top-to-bottom pass can link everything without a second pass.
bool TagStore::load()
{
  tags.clear();
  rootIds.clear();
  docCache.clear();
  nextId = 1;

  std::string contents;
  if(!readFile(&contents, indexPath.c_str()))
    return false;

  for(const std::string& rawLine : splitStr<std::vector>(contents.c_str(), '\n')) {
    if(rawLine.empty())
      continue;
    std::vector<std::string> fields = splitStr<std::vector>(rawLine.c_str(), '\t');
    if(fields.empty())
      continue;
    if(fields[0] == "TAG" && fields.size() >= 4) {
      TagNode node;
      node.id = fields[1];
      node.parentId = fields[2];
      node.name = fields[3];
      if(node.id.size() > 1 && node.id[0] == 't') {
        unsigned int num = strtoul(node.id.c_str() + 1, nullptr, 10);
        nextId = std::max(nextId, num + 1);
      }
      if(node.parentId.empty())
        rootIds.push_back(node.id);
      else if(tags.count(node.parentId))
        tags[node.parentId].childIds.push_back(node.id);
      tags[node.id] = node;
    }
    else if(fields[0] == "DOC" && fields.size() >= 4) {
      DocEntry entry;
      entry.mtime = strtoll(fields[2].c_str(), nullptr, 10);
      entry.tagIds = parseTagList(fields[3].c_str());
      docCache[fields[1]] = entry;
    }
  }
  return true;
}

bool TagStore::save() const
{
  FILE* f = fopen(indexPath.c_str(), "wb");
  if(!f)
    return false;
  // Emit parents before children by walking the tree from the roots, so load() never needs a
  // second pass to resolve a parentId it hasn't seen yet.
  std::vector<std::string> stack(rootIds.rbegin(), rootIds.rend());
  while(!stack.empty()) {
    std::string id = stack.back();
    stack.pop_back();
    auto it = tags.find(id);
    if(it == tags.end())
      continue;
    const TagNode& node = it->second;
    fprintf(f, "TAG\t%s\t%s\t%s\n", node.id.c_str(), node.parentId.c_str(), node.name.c_str());
    for(auto childIt = node.childIds.rbegin(); childIt != node.childIds.rend(); ++childIt)
      stack.push_back(*childIt);
  }
  for(const auto& pair : docCache) {
    fprintf(f, "DOC\t%s\t%lld\t%s\n", pair.first.c_str(),
        (long long)pair.second.mtime, formatTagList(pair.second.tagIds).c_str());
  }
  fclose(f);
  return true;
}
