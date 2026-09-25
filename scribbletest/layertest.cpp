// Unit tests for the layer table in syncscribble/layers.cpp (LAYERS_INVESTIGATION.md).
// Like scantest.cpp, shapetest.cpp and colortest.cpp these need no GL context and no document, so
//  they can also be built and run on their own - which is how they get run in CI, where no display
//  is available:
//
//   g++ -std=c++14 -O2 -DNDEBUG -I . -I syncscribble -DLAYERTEST_MAIN
//       scribbletest/layertest.cpp syncscribble/layers.cpp -o layertest && ./layertest
//   (one command, run from the repo root.)
//
// runLayerTests() returns the number of failed checks and is also called from ScribbleTest::runAll().
// The behaviour that needs a document - a lock actually blocking selection and erase, the undo item,
//  the round trip through the document config - is in ScribbleTest::layerTest().

#include <stdio.h>

#include "layers.h"

static int nLayerChecksFailed = 0;

static void layerCheckTrue(bool condition, const char* what)
{
  if(!condition) {
    ++nLayerChecksFailed;
    printf("FAIL: %s\n", what);
  }
}

// An unknown layer id must be editable, visible and sort to the bottom.  This is the rule that keeps
//  ink reachable when a layer is deleted while elements still point at it - from undo, from a peer
//  with an older table, or from a hand-edited file.  Failing closed here means ink that cannot be
//  selected, erased or seen, and no way for the user to find out why.
static void testFailOpen()
{
  LayerList layers;
  layers.addLayer("Second");
  const int bogus = 4242;
  layerCheckTrue(layers.find(bogus) == NULL, "the bogus id really is unknown");
  layerCheckTrue(layers.isEditable(bogus), "an unknown layer is editable");
  layerCheckTrue(!layers.isLocked(bogus), "an unknown layer is not locked");
  layerCheckTrue(!layers.isHidden(bogus), "an unknown layer is not hidden");
  layerCheckTrue(layers.zIndexOf(bogus) == 0, "an unknown layer sorts to the bottom");
}

static void testLockAndHide()
{
  LayerList layers;
  int second = layers.addLayer("Second");
  layerCheckTrue(layers.setLocked(second, true), "locking a layer succeeds");
  layerCheckTrue(!layers.isEditable(second), "a locked layer is not editable");
  layerCheckTrue(!layers.setLocked(second, true), "locking an already locked layer is a no-op");
  layerCheckTrue(layers.setLocked(second, false), "unlocking succeeds");
  layerCheckTrue(layers.isEditable(second), "an unlocked layer is editable again");

  // hidden implies uneditable: erasing or dragging something invisible is never what was meant
  layerCheckTrue(layers.setHidden(second, true), "hiding a layer succeeds");
  layerCheckTrue(!layers.isEditable(second), "a hidden layer is not editable");
  layerCheckTrue(layers.isHidden(second), "...and reports itself hidden");
}

// A lock guards a layer against work done on *other* layers; it does not guard it against itself.
//  Picking a locked layer as current is how it is edited, so the current layer is editable whether or
//  not it is locked, and stops being editable the moment another layer is picked.  Hidden is still a
//  hard no: new ink cannot land somewhere invisible.
static void testLockedLayerEditableWhenCurrent()
{
  LayerList layers;
  int base = layers.currentId;
  int second = layers.addLayer("Second");
  layerCheckTrue(layers.setLocked(second, true), "locking a non-current layer succeeds");
  layerCheckTrue(!layers.isEditable(second), "a locked layer is not editable from another layer");

  layerCheckTrue(layers.setCurrent(second), "a locked layer can still be made current");
  layerCheckTrue(layers.currentId == second, "...and is current");
  layerCheckTrue(layers.isEditable(second), "a locked layer is editable while it is current");
  layerCheckTrue(layers.isEditable(base), "...and the unlocked layer beside it still is too");

  layerCheckTrue(layers.setCurrent(base), "picking another layer succeeds");
  layerCheckTrue(!layers.isEditable(second), "the locked layer is out of reach again once left");

  // locking the layer the pen is on leaves the pen there; the lock bites once another is picked
  layerCheckTrue(layers.setLocked(base, true), "locking the current layer succeeds");
  layerCheckTrue(layers.currentId == base, "locking the current layer does not move the pen");
  layerCheckTrue(layers.isEditable(base), "...and the current layer stays editable");

  layers.setLocked(base, false);
  layers.setLocked(second, false);
  layers.setCurrent(second);
  layers.setHidden(second, true);
  layerCheckTrue(layers.currentId != second, "hiding the current layer moves the pen off it");
  layerCheckTrue(!layers.setCurrent(second), "a hidden layer cannot be made current");
  layerCheckTrue(layers.currentId != second, "...and the attempt does not move the pen there");
}

// z-order is the position in the list, never the id.  Reordering must not change a single id, or
//  every element in the document would have to be rewritten to move one layer up.
static void testOrderIsIndependentOfIds()
{
  LayerList layers;
  int base = layers.layers[0].id;
  int second = layers.addLayer("Second");
  int third = layers.addLayer("Third");
  layerCheckTrue(layers.zIndexOf(base) == 0 && layers.zIndexOf(second) == 1
      && layers.zIndexOf(third) == 2, "layers stack in the order they were added");

  layerCheckTrue(layers.moveLayer(2, 0), "moving the top layer to the bottom succeeds");
  layerCheckTrue(layers.zIndexOf(third) == 0, "the moved layer is now at the bottom");
  layerCheckTrue(layers.zIndexOf(base) == 1 && layers.zIndexOf(second) == 2,
      "the others shifted up");
  layerCheckTrue(layers.find(third) && layers.find(third)->name == "Third",
      "reordering did not change the moved layer's identity");
  layerCheckTrue(!layers.moveLayer(0, 99), "an out-of-range move is refused");
}

// Ids are never reused.  If they were, deleting a layer and adding another would hand the new layer
//  every element the deleted one owned - including elements restored by an undo of the delete.
static void testIdsAreNeverReused()
{
  LayerList layers;
  int second = layers.addLayer("Second");
  layerCheckTrue(layers.removeLayer(second), "removing a layer succeeds");
  int third = layers.addLayer("Third");
  layerCheckTrue(third != second, "a new layer does not reuse a removed layer's id");

  // The counter has to survive a save too, or reopening the document undoes the protection above.
  //  Note this only bites when the *highest* id is the one removed: any other case is covered by the
  //  max-id fallback in parse(), so removing `second` above would not have caught a missing counter.
  layerCheckTrue(layers.removeLayer(third), "removing the highest-numbered layer succeeds");
  LayerList back = LayerList::parse(layers.serialize().c_str(), layers.currentId);
  layerCheckTrue(back.nextId() == layers.nextId(), "the id counter survives serialization");
  layerCheckTrue(back.addLayer("Fourth") != third,
      "a reopened table does not reuse the id of the removed top layer");
  layerCheckTrue(back.addLayer("Fifth") != second, "...nor of one removed from the middle");

  // and the last layer can never be removed - a document with no layers has nowhere to put ink
  LayerList one;
  layerCheckTrue(!one.removeLayer(one.layers[0].id), "the last remaining layer cannot be removed");
  layerCheckTrue(one.size() == 1, "...and is still there");
}

static void testRoundTrip()
{
  LayerList layers;
  layers.setName(layers.layers[0].id, "Base");
  int second = layers.addLayer("Ink, notes; draft 100%");  // every character that needs escaping
  int third = layers.addLayer("Third");
  layers.setLocked(second, true);
  layers.setHidden(third, true);
  layers.setCurrent(layers.layers[0].id);

  LayerList back = LayerList::parse(layers.serialize().c_str(), layers.currentId);
  layerCheckTrue(back == layers, "a layer table round-trips through its serialized form");
  layerCheckTrue(back.size() == 3, "all three layers come back");
  layerCheckTrue(back.find(second) && back.find(second)->name == "Ink, notes; draft 100%",
      "a name containing the separators and the escape character survives");
  layerCheckTrue(back.isLocked(second), "the locked flag survives");
  layerCheckTrue(back.isHidden(third), "the hidden flag survives");
  layerCheckTrue(back.zIndexOf(second) == 1 && back.zIndexOf(third) == 2, "z-order survives");

  // An empty string is what every document written before layers existed has, and it has to read
  //  back as one ordinary layer holding everything - that is what makes the feature need no
  //  migration step anywhere.
  LayerList legacy = LayerList::parse("", LayerList::DEFAULT_LAYER);
  layerCheckTrue(legacy.size() == 1, "an unlayered document reads back as a single layer");
  layerCheckTrue(legacy.layers[0].id == LayerList::DEFAULT_LAYER,
      "...whose id is the one elements with no __layer attribute report");
  layerCheckTrue(legacy.isEditable(LayerList::DEFAULT_LAYER), "...and it is editable");
  layerCheckTrue(legacy.currentId == LayerList::DEFAULT_LAYER, "...and current");

  // a locked layer is a legitimate current layer and comes back as one; a hidden one does not
  LayerList reopened = LayerList::parse(layers.serialize().c_str(), second);
  layerCheckTrue(reopened.currentId == second, "a saved current layer that is locked comes back as current");
  layerCheckTrue(reopened.isEditable(second), "...and is editable, being current");
  LayerList reopenedHidden = LayerList::parse(layers.serialize().c_str(), third);
  layerCheckTrue(reopenedHidden.currentId != third,
      "a saved current layer that is now hidden does not come back as current");
  layerCheckTrue(reopenedHidden.isEditable(reopenedHidden.currentId),
      "the fallback current layer accepts ink");

  // garbage must not produce a table that lies about itself
  LayerList junk = LayerList::parse("not;a;table", LayerList::DEFAULT_LAYER);
  layerCheckTrue(junk.size() >= 1, "a malformed table still yields at least one layer");
  layerCheckTrue(junk.isEditable(junk.currentId), "...and its current layer accepts ink");
}

// Per-layer state is what LayerTableItem records and what goes on the sync wire.  The property that
//  matters is the one a whole-table snapshot would break: sync replays a local edit over a peer's by
//  undoing it, applying the peer's, and redoing it - and that replay must not revert what the peer did
//  to some *other* layer.
static void testLayerState()
{
  LayerList layers;
  int second = layers.addLayer("Second");
  int third = layers.addLayer("Third");

  // a remove followed by restoring the recorded state puts the layer back exactly - name, flags and
  //  place in the stack, which is recorded as the layer below it rather than as an index
  layers.setLocked(second, true);
  LayerList before = layers;
  LayerInfo info;
  int below = -99;
  layerCheckTrue(layers.layerState(second, &info, &below), "an existing layer reports its state");
  layerCheckTrue(below == layers.layers[0].id, "...with the layer directly below it");
  layers.setLayerState(second, NULL, LayerList::BELOW_NONE);
  layerCheckTrue(!layers.find(second), "applying an absent state removes the layer");
  layers.setLayerState(second, &info, below);
  layerCheckTrue(layers == before, "restoring the recorded state restores the table exactly");

  // the rebase: local rename of `second`, then a peer's lock of `third` arrives
  LayerInfo localBefore, localAfter;
  int localBelow;
  layers.layerState(second, &localBefore, &localBelow);
  layers.setName(second, "Renamed here");
  layers.layerState(second, &localAfter, &localBelow);
  layers.setLayerState(second, &localBefore, localBelow);       // undo ours
  LayerInfo peer;
  int peerBelow;
  layers.layerState(third, &peer, &peerBelow);
  peer.locked = true;
  layers.setLayerState(third, &peer, peerBelow);                // apply theirs
  layers.setLayerState(second, &localAfter, localBelow);        // redo ours
  layerCheckTrue(layers.isLocked(third), "replaying a local edit keeps a peer's edit to another layer");
  layerCheckTrue(layers.find(second)->name == "Renamed here", "...and the local edit itself");

  // a layer placed above one a peer has since removed lands on top, rather than nowhere
  LayerInfo orphan(4242, "Orphan");
  layers.setLayerState(4242, &orphan, 777);
  layerCheckTrue(layers.zIndexOf(4242) == layers.size() - 1, "an unknown layer-below puts it on top");

  // the pen is moved off a layer that disappears or is hidden under it
  layers.setCurrent(third);
  layers.setLayerState(third, NULL, LayerList::BELOW_NONE);
  layerCheckTrue(layers.find(layers.currentId) && layers.isEditable(layers.currentId),
      "removing the current layer by state moves the pen to a layer that takes ink");
  LayerList one;
  one.setLayerState(one.layers[0].id, NULL, LayerList::BELOW_NONE);
  layerCheckTrue(one.size() == 1, "the last layer cannot be removed by state either");

  // Shared-session ids: random, and outside the counter, or one of them would push nextId() to the
  //  top of the range.  An id already taken is refused rather than silently merged.
  LayerList shared;
  int counterBefore = shared.nextId();
  int big = LayerList::SHARED_ID_BASE + 12345;
  layerCheckTrue(shared.addLayer("Shared", -1, big) == big, "a layer can be added with an explicit id");
  layerCheckTrue(shared.nextId() == counterBefore, "...and a shared-range id does not advance the counter");
  layerCheckTrue(shared.addLayer("Clash", -1, big) != big, "an explicit id already taken is refused");
  LayerList reread = LayerList::parse(shared.serialize().c_str());
  layerCheckTrue(reread.nextId() == counterBefore, "...nor does it advance the counter after a reload");
  layerCheckTrue(reread.find(big) != NULL, "...and the layer itself survives the reload");
  // restoring a counter-range layer by state must not let the counter hand its id out again
  LayerList restored;
  LayerInfo high(40, "High");
  restored.setLayerState(40, &high, LayerList::BELOW_NONE);
  layerCheckTrue(restored.nextId() > 40, "restoring a layer by state advances the counter past it");
}

int runLayerTests()
{
  nLayerChecksFailed = 0;
  testFailOpen();
  testLockAndHide();
  testLockedLayerEditableWhenCurrent();
  testOrderIsIndependentOfIds();
  testIdsAreNeverReused();
  testRoundTrip();
  testLayerState();
  return nLayerChecksFailed;
}

#ifdef LAYERTEST_MAIN
int main()
{
  int nFailed = runLayerTests();
  printf(nFailed ? "%d layer checks FAILED\n" : "All layer checks passed\n", nFailed);
  return nFailed;
}
#endif
