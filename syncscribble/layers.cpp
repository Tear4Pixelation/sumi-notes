#include "layers.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static constexpr int LAYER_FLAG_LOCKED = 0x1;
static constexpr int LAYER_FLAG_HIDDEN = 0x2;

void LayerList::reset()
{
  layers.clear();
  layers.push_back(LayerInfo(DEFAULT_LAYER, "Layer 1"));
  currentId = DEFAULT_LAYER;
  m_nextId = DEFAULT_LAYER + 1;
}

const LayerInfo* LayerList::find(int id) const
{
  for(const LayerInfo& layer : layers) {
    if(layer.id == id)
      return &layer;
  }
  return nullptr;
}

LayerInfo* LayerList::find(int id)
{
  return const_cast<LayerInfo*>(const_cast<const LayerList*>(this)->find(id));
}

int LayerList::zIndexOf(int id) const
{
  if(id == REGION_LAYER)
    return -1;
  for(int idx = 0; idx < size(); ++idx) {
    if(layers[idx].id == id)
      return idx;
  }
  return 0;  // unknown id draws at the bottom rather than vanishing
}

bool LayerList::setCurrent(int id)
{
  if(!find(id) || isHidden(id))
    return false;
  currentId = id;
  return true;
}

int LayerList::addLayer(const std::string& name, int aboveIdx, int id)
{
  if(id >= 0 && find(id))
    return DEFAULT_LAYER;
  LayerInfo layer(id >= 0 ? id : m_nextId++, name);
  if(layer.id < SHARED_ID_BASE)
    m_nextId = std::max(m_nextId, layer.id + 1);
  if(layer.name.empty())
    layer.name = "Layer " + std::to_string(size() + 1);
  int at = (aboveIdx < 0 || aboveIdx >= size()) ? size() : aboveIdx + 1;
  layers.insert(layers.begin() + at, layer);
  return layer.id;
}

bool LayerList::removeLayer(int id)
{
  if(size() < 2)
    return false;  // a document always has at least one layer
  auto it = std::find_if(layers.begin(), layers.end(),
      [id](const LayerInfo& layer) { return layer.id == id; });
  if(it == layers.end())
    return false;
  layers.erase(it);
  if(currentId == id)
    fallbackCurrent();
  return true;
}

void LayerList::fallbackCurrent()
{
  currentId = layers.back().id;
  // the fallback must still be able to take ink
  for(auto ii = layers.rbegin(); ii != layers.rend(); ++ii) {
    if(!ii->locked && !ii->hidden) { currentId = ii->id;  break; }
  }
}

bool LayerList::layerState(int id, LayerInfo* info, int* belowId) const
{
  for(int idx = 0; idx < size(); ++idx) {
    if(layers[idx].id == id) {
      if(info) *info = layers[idx];
      if(belowId) *belowId = idx > 0 ? layers[idx-1].id : BELOW_NONE;
      return true;
    }
  }
  return false;
}

void LayerList::setLayerState(int id, const LayerInfo* info, int belowId)
{
  auto it = std::find_if(layers.begin(), layers.end(),
      [id](const LayerInfo& layer) { return layer.id == id; });
  if(!info) {
    if(it != layers.end() && size() > 1)
      layers.erase(it);
  }
  else {
    if(it != layers.end())
      layers.erase(it);
    LayerInfo layer = *info;
    layer.id = id;
    int at = size();  // an unknown `belowId` puts it on top
    if(belowId == BELOW_NONE)
      at = 0;
    else {
      for(int idx = 0; idx < size(); ++idx) {
        if(layers[idx].id == belowId) { at = idx + 1;  break; }
      }
    }
    layers.insert(layers.begin() + at, layer);
    // restoring a removed layer must not let the counter hand its id out again
    if(id < SHARED_ID_BASE)
      m_nextId = std::max(m_nextId, id + 1);
  }
  if(!find(currentId) || isHidden(currentId))
    fallbackCurrent();
}

bool LayerList::moveLayer(int fromIdx, int toIdx)
{
  if(fromIdx < 0 || fromIdx >= size() || toIdx < 0 || toIdx >= size() || fromIdx == toIdx)
    return false;
  LayerInfo layer = layers[fromIdx];
  layers.erase(layers.begin() + fromIdx);
  layers.insert(layers.begin() + toIdx, layer);
  return true;
}

bool LayerList::setName(int id, const std::string& name)
{
  LayerInfo* layer = find(id);
  if(!layer || name.empty() || layer->name == name)
    return false;
  layer->name = name;
  return true;
}

bool LayerList::setLocked(int id, bool locked)
{
  LayerInfo* layer = find(id);
  if(!layer || layer->locked == locked)
    return false;
  layer->locked = locked;
  return true;
}

bool LayerList::setHidden(int id, bool hidden)
{
  LayerInfo* layer = find(id);
  if(!layer || layer->hidden == hidden)
    return false;
  layer->hidden = hidden;
  if(hidden && currentId == id) {
    for(auto ii = layers.rbegin(); ii != layers.rend(); ++ii) {
      if(!ii->locked && !ii->hidden) { currentId = ii->id;  break; }
    }
  }
  return true;
}

// names are user text and the config value is a flat string, so the two separators and the escape
//  character itself have to be escaped - a layer called "Notes, draft" must not become two layers
static std::string escapeName(const std::string& name)
{
  std::string out;
  for(char c : name) {
    if(c == '%' || c == ',' || c == ';') {
      char buf[8];
      snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
      out += buf;
    }
    else
      out += c;
  }
  return out;
}

static std::string unescapeName(const std::string& name)
{
  std::string out;
  for(size_t ii = 0; ii < name.size(); ++ii) {
    if(name[ii] == '%' && ii + 2 < name.size()) {
      char buf[3] = {name[ii+1], name[ii+2], '\0'};
      out += char(strtol(buf, NULL, 16));
      ii += 2;
    }
    else
      out += name[ii];
  }
  return out;
}

std::string LayerList::serialize() const
{
  // leading "#<nextId>" record: the id counter has to survive a save, or reopening a document and
  //  adding a layer could hand out an id a deleted layer's elements still carry
  std::string out = "#" + std::to_string(m_nextId);
  for(const LayerInfo& layer : layers) {
    int flags = (layer.locked ? LAYER_FLAG_LOCKED : 0) | (layer.hidden ? LAYER_FLAG_HIDDEN : 0);
    if(!out.empty())
      out += ';';
    out += std::to_string(layer.id) + "," + std::to_string(flags) + "," + escapeName(layer.name);
  }
  return out;
}

LayerList LayerList::parse(const char* s, int currentid)
{
  LayerList list;
  if(!s || !s[0])
    return list;  // reset() already put the default layer there
  list.layers.clear();
  std::string str(s);
  size_t pos = 0;
  while(pos <= str.size()) {
    size_t end = str.find(';', pos);
    if(end == std::string::npos)
      end = str.size();
    std::string rec = str.substr(pos, end - pos);
    pos = end + 1;
    if(rec.empty())
      continue;
    if(rec[0] == '#') {
      list.m_nextId = atoi(rec.c_str() + 1);
      continue;
    }
    size_t c1 = rec.find(',');
    size_t c2 = c1 == std::string::npos ? c1 : rec.find(',', c1 + 1);
    if(c1 == std::string::npos || c2 == std::string::npos)
      continue;
    LayerInfo layer(atoi(rec.substr(0, c1).c_str()), unescapeName(rec.substr(c2 + 1)));
    int flags = atoi(rec.substr(c1 + 1, c2 - c1 - 1).c_str());
    layer.locked = (flags & LAYER_FLAG_LOCKED) != 0;
    layer.hidden = (flags & LAYER_FLAG_HIDDEN) != 0;
    // a duplicate id would make zIndexOf() and find() disagree about which layer an element is on
    if(!list.find(layer.id))
      list.layers.push_back(layer);
  }
  if(list.layers.empty())
    list.reset();
  // a table written by a build without the counter, or a corrupt one, must still never hand out an
  //  id a layer already holds.  Random shared-session ids are exempt, or one would push the counter
  //  to the top of the range.
  for(const LayerInfo& layer : list.layers) {
    if(layer.id < SHARED_ID_BASE)
      list.m_nextId = std::max(list.m_nextId, layer.id + 1);
  }
  list.currentId = currentid;
  if(!list.setCurrent(currentid))
    list.fallbackCurrent();  // the saved current layer is gone or hidden
  return list;
}

bool LayerList::operator==(const LayerList& other) const
{
  if(size() != other.size() || currentId != other.currentId || m_nextId != other.m_nextId)
    return false;
  for(int ii = 0; ii < size(); ++ii) {
    const LayerInfo& a = layers[ii];
    const LayerInfo& b = other.layers[ii];
    if(a.id != b.id || a.name != b.name || a.locked != b.locked || a.hidden != b.hidden)
      return false;
  }
  return true;
}
