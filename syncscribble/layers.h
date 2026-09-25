#pragma once

// Layers (LAYERS_INVESTIGATION.md).  A layer is a *tag* carried by each element (Element::layer(),
//  serialized as the __layer attribute, exactly the convention __shape/__comx/__timestamp already
//  use) plus this table, which says what the tags mean: name, stacking order, locked, hidden.
//
// The table is deliberately not part of the SVG tree.  Making each layer a real <g> would mean an
//  older Write build - which reads only the first .write-content node - silently dropping every
//  other layer on save.  A tag is invisible to a build that does not know about it.
//
// Two rules the rest of the code depends on:
//  - ids are stable and never reused; z-order is the *position* in `layers`, not the id.  Reordering
//    layers must never rewrite a single element.
//  - an id not in the table FAILS OPEN: editable, visible, drawn at the bottom.  A layer can be
//    deleted while elements still point at it (undo, a peer on an older table, a hand-edited file),
//    and ink that cannot be selected, erased or seen is ink the user cannot recover.

#include <string>
#include <vector>

struct LayerInfo {
  int id;
  std::string name;
  bool locked = false;
  bool hidden = false;

  LayerInfo(int _id = 0, const std::string& _name = std::string()) : id(_id), name(_name) {}
};

class LayerList {
public:
  // z-order, bottom first - index 0 is drawn first, i.e. furthest back
  std::vector<LayerInfo> layers;

  // the layer new elements are added to
  int currentId = DEFAULT_LAYER;

  // id of the layer every element has when a document has never been layered, and the id
  //  Element::layer() reports for any element with no __layer attribute
  static constexpr int DEFAULT_LAYER = 0;
  // Page::addStroke's default: stamp the document's current layer onto the element.  Callers that
  //  must preserve an element's existing layer - free-erase subpaths, undo, sync - pass the layer
  //  explicitly instead
  static constexpr int LAYER_CURRENT = -1;
  // Ids at or above this are drawn at random, and are what a layer added during a shared whiteboard
  //  session gets: two clients adding a layer at the same moment would otherwise both take nextId()
  //  and their strokes would silently merge into one layer.  Random ids never advance the counter,
  //  so the counter-based ids below this stay dense.
  static constexpr int SHARED_ID_BASE = 1 << 24;
  // setLayerState()'s `belowId` for a layer at the bottom of the stack
  static constexpr int BELOW_NONE = -1;
  // What Element::layer() reports for a ruling region.  A region is not on any layer: it is the page's
  //  ruling, not ink, so hiding or locking a layer must not touch it.  zIndexOf() puts it below every
  //  layer, which is what keeps it first in the page through addStroke, restacking and undo replay.
  //  Never serialized - a region is recognised by its class, not by this id.
  static constexpr int REGION_LAYER = -2;

  LayerList() { reset(); }
  // a document always has at least one layer, so "no layers" and "one layer" are the same case and
  //  nothing downstream needs an empty check
  void reset();

  int size() const { return int(layers.size()); }
  const LayerInfo* find(int id) const;
  LayerInfo* find(int id);
  // position in z-order; an unknown id sorts to the bottom (see the fail-open rule above)
  int zIndexOf(int id) const;
  const LayerInfo* byIndex(int idx) const
      { return idx >= 0 && idx < size() ? &layers[idx] : nullptr; }

  // an unknown layer is editable and visible - never lock ink out of reach
  bool isLocked(int id) const { const LayerInfo* l = find(id);  return l && l->locked; }
  bool isHidden(int id) const { const LayerInfo* l = find(id);  return l && l->hidden; }
  // hidden implies uneditable: erasing or dragging something you cannot see is never intended.
  //  A lock protects a layer from work done on *other* layers, so the current layer is editable
  //  whether or not it is locked - picking a locked layer in the layer list is how it is edited.
  bool isEditable(int id) const
      { const LayerInfo* l = find(id);  return !l || (!l->hidden && (!l->locked || id == currentId)); }

  const LayerInfo* current() const { return find(currentId); }
  // refuses a hidden layer: the current layer is where new ink lands, so it has to be visible.  A
  //  locked layer is accepted, and becomes editable for as long as it stays current.
  bool setCurrent(int id);

  // returns the new layer's id, or DEFAULT_LAYER on failure; inserted directly above `aboveIdx`
  //  (-1 = on top).  `id` < 0 takes the next counter id; an explicit id fails if it is already taken.
  int addLayer(const std::string& name, int aboveIdx = -1, int id = -1);
  // the caller is responsible for the elements that pointed at `id` - they fail open to editable,
  //  and Document::purgeLayer() can reassign them.  The last layer cannot be removed.
  bool removeLayer(int id);
  bool moveLayer(int fromIdx, int toIdx);
  bool setName(int id, const std::string& name);
  // locking the current layer leaves the pen on it: the lock takes effect once another layer is picked
  bool setLocked(int id, bool locked);
  // hiding the current layer moves `currentId` to the topmost layer that can still take ink
  bool setHidden(int id, bool hidden);

  // the id the next added layer will get.  This is one past the highest id *ever* used, not one past
  //  the highest currently in use, and it is serialized for exactly that reason: undoing the delete
  //  of a layer restores elements carrying its id, and if a layer added in the meantime had been
  //  given that id back, those elements would silently join it.
  int nextId() const { return m_nextId; }

  // One layer's whole state - whether it exists, its info, and the id of the layer directly below it
  //  (BELOW_NONE at the bottom).  This is what LayerTableItem records and what goes on the sync wire:
  //  per layer rather than the whole table, because sync replays local edits over remote ones by
  //  undoing and redoing them, and a whole-table snapshot redone over a peer's concurrent edit to some
  //  *other* layer would silently revert it.
  bool layerState(int id, LayerInfo* info, int* belowId) const;
  // info == NULL removes the layer (refused for the last one).  A `belowId` no longer in the table puts
  //  the layer on top - a peer can remove the layer this one was placed above.  The current layer is
  //  moved off a layer that is removed or hidden, since it has to be able to take ink.
  void setLayerState(int id, const LayerInfo* info, int belowId);

  // "id,flags,name;id,flags,name;...", name percent-escaped for , ; and %.  Round-trips through the
  //  document config like the theme recipe does, so no new file-format construct is needed.
  std::string serialize() const;
  static LayerList parse(const char* s, int currentid = DEFAULT_LAYER);

  bool operator==(const LayerList& other) const;

private:
  int m_nextId = DEFAULT_LAYER + 1;
  // current -> the topmost layer that can still take ink (or the topmost at all, if none can)
  void fallbackCurrent();
  bool operator!=(const LayerList& other) const { return !operator==(other); }
};
