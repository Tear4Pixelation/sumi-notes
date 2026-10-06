#include "tagstore.h"
#include "ulib/fileutil.h"
#include "ulib/stringutil.h"

#include <cstdio>
#include <cstring>
#include <cctype>
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

// split keeping empty fields - an untitled page's record ends in an empty title
static std::vector<std::string> splitFields(const std::string& s, char sep)
{
  std::vector<std::string> fields;
  size_t start = 0;
  for(size_t end; (end = s.find(sep, start)) != std::string::npos; start = end + 1)
    fields.push_back(s.substr(start, end - start));
  fields.push_back(s.substr(start));
  return fields;
}

static std::string percentEncode(const std::string& s)
{
  std::string out;
  for(unsigned char c : s) {
    if(c < 0x20 || strchr("%|;,&<>\"'\t", c))
      out += fstring("%%%02X", c);
    else
      out += char(c);
  }
  return out;
}

static std::string percentDecode(const std::string& s)
{
  std::string out;
  for(size_t ii = 0; ii < s.size(); ++ii) {
    if(s[ii] == '%' && ii + 2 < s.size() && isxdigit((unsigned char)s[ii+1]) && isxdigit((unsigned char)s[ii+2])) {
      out += char(strtol(s.substr(ii + 1, 2).c_str(), NULL, 16));
      ii += 2;
    }
    else
      out += s[ii];
  }
  return out;
}

std::vector<TagStore::PageTags> TagStore::parsePageTags(const char* s)
{
  std::vector<PageTags> pages;
  if(!s || !s[0])
    return pages;
  for(const std::string& record : splitFields(s, ';')) {
    std::vector<std::string> fields = splitFields(record, '|');
    if(fields.size() < 2 || fields[0].empty() || !isdigit((unsigned char)fields[0][0]))
      continue;
    PageTags entry;
    entry.page = atoi(fields[0].c_str());
    entry.tagIds = parseTagList(fields[1].c_str());
    if(fields.size() > 2)
      entry.title = percentDecode(fields[2]);
    if(!entry.tagIds.empty())
      pages.push_back(entry);
  }
  return pages;
}

std::string TagStore::formatPageTags(const std::vector<PageTags>& pages)
{
  std::string out;
  for(const PageTags& entry : pages) {
    if(entry.tagIds.empty())
      continue;
    if(!out.empty())
      out += ';';
    out += fstring("%d|%s|%s", entry.page, formatTagList(entry.tagIds).c_str(), percentEncode(entry.title).c_str());
  }
  return out;
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

void TagStore::setDocTags(const std::string& relPath, time_t mtime, const std::vector<std::string>& tagIds,
    const std::string& pageTags)
{
  docCache[relPath] = DocEntry{mtime, tagIds, pageTags, true};
}

bool TagStore::cachedDocTags(const std::string& relPath, time_t mtime, std::vector<std::string>* outTagIds,
    std::string* outPageTags) const
{
  auto it = docCache.find(relPath);
  if(it == docCache.end() || it->second.mtime != mtime || !it->second.pageTagsKnown)
    return false;
  if(outTagIds)
    *outTagIds = it->second.tagIds;
  if(outPageTags)
    *outPageTags = it->second.pageTags;
  return true;
}

void TagStore::removeDoc(const std::string& relPath)
{
  docCache.erase(relPath);
}

// Simple tab-separated line format, one record per line:
//   TAG\t<id>\t<parentId>\t<name>
//   DOC\t<relPath>\t<mtime>\t<comma-separated tag ids>\t<page tags, as formatPageTags()>
// A DOC line without the fifth field was written before page tags existed; its document is reread.
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
    // keeps trailing empty fields: getline-based splitStr drops them, which made every untagged
    //  document's DOC line look short and so be reread from its file on every refresh
    std::vector<std::string> fields = splitFields(rawLine, '\t');
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
      entry.pageTagsKnown = fields.size() >= 5;
      if(entry.pageTagsKnown)
        entry.pageTags = fields[4];
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
    fprintf(f, "DOC\t%s\t%lld\t%s\t%s\n", pair.first.c_str(),
        (long long)pair.second.mtime, formatTagList(pair.second.tagIds).c_str(), pair.second.pageTags.c_str());
  }
  fclose(f);
  return true;
}

bool TagStore::isDescendant(const std::string& tagId, const std::string& ancestorId) const
{
  for(const TagNode* tagNode = tag(tagId); tagNode && !tagNode->parentId.empty(); tagNode = tag(tagNode->parentId)) {
    if(tagNode->parentId == ancestorId)
      return true;
  }
  return false;
}

TagStore::FilterMatch TagStore::matchFilter(const std::vector<std::string>& tagIds,
    const std::set<std::string>& filterTags) const
{
  FilterMatch result;
  for(const std::string& filterTag : filterTags) {
    bool exact = std::find(tagIds.begin(), tagIds.end(), filterTag) != tagIds.end();
    if(exact || std::any_of(tagIds.begin(), tagIds.end(), [&](const std::string& id){ return isDescendant(id, filterTag); }))
      ++result.matched;
    if(exact)
      ++result.exact;
  }
  return result;
}
