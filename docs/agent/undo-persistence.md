# Persistent undo history

The most recent undo (and redo) steps of a document survive saving, closing and reopening. Code:
`syncscribble/undopersist.h`/`.cpp`; hooks in `ScribbleDoc::saveDocument()` and
`ScribbleDoc::openDocument()`; every undo item's `persist()` (declared through the
`UNDO_ITEM_METHODS` macros in `syncundo.h`, defined in `undopersist.cpp`).

## What is kept, and where

- **Up to `undoPersistSteps` (50) undo steps and as many redo steps**, capped at `undoPersistMaxKB`
  (4096 KB) per document. 0 for either turns it off. A step of strokes costs a few hundred bytes (live
  content is referenced, not copied), so 50 steps of drawing is a few KB; what the byte cap is for is a
  step that deletes content - a deleted page full of images is written in full. Undo steps win over
  redo steps under the cap, recent over old.
- **Redo is kept.** The mechanism handles undone content exactly like deleted content, so it cost
  nothing extra, and editor tabs save on switch: dropping redo would lose it every time the user
  looked at another document.
- **Never in the document.** A sidecar in app-private storage: `undo-history/` beside `saved/` (on
  Linux `~/.config/sumi/undo-history/`, debug builds `Debug/undo-history/`, Android
  `<app storage>/.undo-history/`, iOS `Library/undo-history/`). A shared or exported file must not carry
  content its author erased - a deleted stroke would be recoverable from it.
- **Keyed by path**, file name `<fnv1a-64 of the key>.undo`. Documents have no id of their own (no doc
  uuid exists; sync ids are per session), and adding one to the file was rejected: it changes the
  format, makes copies of a file linkable, and the path plus the fingerprint already does the job. The key
  is the canonical *folder* plus the file name, so it is the same whether or not the file still exists (a
  rename is reported after the fact). On iOS it is taken relative to HOME, because the container path
  changes with app updates. In-app renames and moves (`TagDocList::renameDoc`, `DocumentList`
  rename and cut/paste) carry the sidecar along (`UndoPersist::documentMoved`); deletes from
  `TagDocList` remove it (`documentDeleted`). DocumentList's trash delete leaves it, since that delete
  can be undone and brings the file back to the same path; age cleanup takes it otherwise.
- **Version check**: the header stores a fingerprint of the file as just saved - size, mtime (seconds)
  and FNV-1a over its first and last 64 KB (the tail is where every save of either format changes:
  the bgz index of `.svgz`, the config/thumbnail block of `.html`). Hashing the whole file was ruled out
  because imported PDFs make documents of tens of MB and this runs on every save. On open, any mismatch
  (fingerprint, page count, format version) deletes the sidecar silently. A cloud client that rewrites
  identical bytes with a new mtime costs the history; that is the safe direction.
- **Second check, per reference**: the header's `<verify>` record holds the node type and bounds of
  every on-page element a step refers to. Load reorders ruling regions to the front of a page
  (`Page::loadSVG`), and saving can in principle skip nodes, so an index alone could point at a different
  element after the round trip; a bounds mismatch drops the whole history rather than apply it to the
  wrong element.
- **Cleanup**, at most once a run on the first save: sidecars not rewritten for 60 days, leftover
  `.tmp` files, then the oldest until the folder is under 64 MB.
- **Sync**: while a doc has a `ScribbleSync` (a shared session), nothing is persisted and its sidecar is
  removed - the history then holds peers' items, and a session re-sends the whole history when it
  starts, so a persisted copy would send them back later as ours. A restored history in a document that
  then *starts* a session behaves exactly like in-memory history from before the session: it is undone,
  pages sent, and redone/sent (`ScribbleSync` start-up).

Writing happens after `Document::save()` succeeds, to `<sidecar>.tmp` then renamed over the sidecar
(on Windows the target is removed first, since rename does not replace there). Only
`ScribbleDoc::saveDocument/openDocument` with a `scribbleMode` participate - bare `Document::save`
(DocumentList's multi-file copy, imports) does not, which is right: those are not the user's editing
session. Because the hooks are in `ScribbleDoc`, every UI path gets it, including editor tabs that
unload an idle document and reopen it through the normal open path.

## Format (version 1)

A stream of top-level XML records, parsed with `pugi::parse_fragment` - deliberately no single root
element, so records can be appended:

```
<undohistory version="1" doc="<key>" fingerprint="<size> <mtime> <hash>" pages="2" undo="7" redo="1"/>
<verify><v r="L0.1" sig="16 172.66 146.60 490.30 200.23"/>...</verify>
<step page='0'><addstroke s="L0.1" pg="L0" next=""/></step>
<step page='1'><pdef id='0'><svg ...page.../></pdef><delpage pg="P0" pagenum="1"/></step>
```

One `<step>` per `UndoGroupHeader` group (`page` is the header's `pageNum`), oldest first; the last
`redo` of them are the undone ones, and `pos` goes to the first of those on restore. **References** are resolved against
the document as it is *at the moment the record is written* - for the sidecar, the file just saved:

| ref | meaning |
|---|---|
| `L3` / `L3.12` | live page 3 / its element 12 (`Page::children()` order) |
| `P2` / `P2.5` | detached page definition 2 / its element 5 |
| `E7` | detached element definition 7 |
| empty | NULL (e.g. `next` of an element that was last) |

Detached content - deleted elements and pages, and added-then-undone ones on the redo side - is written
in full as `<edef id>` (the element's SVG, as `<addstroke>` sends it) or `<pdef id>` (`Page::saveSVG`).
Each definition goes in the **first step that uses it**, so a step only ever refers back, never forward.

Item records, one per `UndoHistoryItem` type: `addstroke`, `delstroke`, `strokechanged`,
`shapechanged`, `regionchanged`, `layerchanged`, `translate`, `transform`, `pagechanged`,
`outlinechanged`, `addpage`, `delpage`, `layertable`, `themechanged`. The names follow the sync wire
format, but the **content is different**, see the first trap.

## Traps

- **`persist()` writes what the item holds, never the live state.** `serialize()` (sync) sends the state
  a peer must *reach*; an undo item holds the state undo (or, if undone, redo) will *swap in*. E.g.
  `StrokeChangedItem::props`, `StrokeLayerItem::layer/next`, `PageChangedItem::props`,
  `LayerTableItem::info`. Writing `s->layer()` instead of `layer` passes every save and fails only when
  the user undoes after reopening (the test catches exactly this - see below). Also `serialize()` has a
  side effect (`StrokeAddedItem` assigns a new uuid), another reason not to reuse it.
- **Ownership** (the `PageDeletedItem` trap, generalised): content out of the document is owned by the
  item nearest `pos` that holds it - a not-undone delete on the undo side, an undone add on the redo
  side - and freed by that item's `discard()`. The kept steps are therefore always **contiguous with
  `pos`** on each side: the count cap, the byte cap and a step that fails to write all cut there, never in
  the middle, so no kept step can refer to content whose owner was dropped. Restored items are not
  `commit()`ed (that would bump dirty counts) and on a failed restore they are deleted *without*
  `discard()`, with the definitions freed directly, since nothing was installed in the history yet.
- **A new undo item type must get a `persist()`**, and a reader branch in `UndoPersist::readItem()`.
  The base class default fails the writer, which stops the history at that step (older steps are not
  written) rather than writing a history that silently skips one. `DISABLED_ITEM`s (sync's
  `removeItems`) are skipped: they are no-ops.
- **An element must be top-level on a page or fully detached.** Anything else (nested in a group) has no
  index that finds it again; the writer fails that step. The editing paths clone their way around groups
  (`Selection::ungroup`, `setProperties`), so this should not happen.
- **Pages load lazily**: the reader calls `ensureLoaded(false)` on each referenced live page. `false`
  skips the memory check, which (under `SCRIBBLE_MEMORY_LIMIT`, not defined in any build) would unload
  clean pages - including pages the history refers to. That latent hazard predates this feature: a
  saved page has `dirtyCount == 0` while in-memory history still points into it.
- **`ScribbleTest::runAll()` disables persistence** (`UndoPersist::enabled`) so the fixtures' saves and
  reopens neither leave nor pick up sidecars; `undoPersistTest()` turns it on with its own folder.

## Tests

`ScribbleTest::undoPersistTest()`: two strokes, a new layer, both strokes moved to it, a new page, an
outline entry with every XML character, deleting page 1 (its strokes now exist only in the history), then
a stroke drawn and undone (a redo step). Save, reopen: step counts match, the document equals the saved
state, redo then undo, and each further undo yields exactly the recorded state before that edit (page
sizes, outline, every element's type/layer/bounds, the layer table); redo all the way forward. Then the
file is appended to externally: it opens with no history and the sidecar is gone. Mutation-checked:
skipping the fingerprint check, persisting the live layer in `StrokeLayerItem`, not restoring `pos`, and
not calling `restore()` each fail it (2-3 checks each). ASan with leak detection shows no leaks from it.

Verified in agent-display on a library document: strokes, Add Page, a stroke on the new page, Add
Layer, lock Layer 1, Delete Current Page, Ctrl+S, kill the app, reopen: Ctrl+Z brought the deleted page
back, then unlocked Layer 1, removed Layer 2, and so on to the original document; a file appended to by
another program opened with the undo button disabled and its sidecar deleted.

## Building the crash journal on this

The journal (append each step on pen-up, replay after a crash) can reuse the record format as is:
open the stream with the same header (fingerprint of the last saved file), then append one `<step>` per
`UndoHistory::addItem` group as it closes. Because references resolve against the document *at the time
the record is written*, a step appended right after it happened resolves correctly when replayed in order
on top of the saved file plus the earlier journal steps. Two differences from the sidecar:

- **Replay is forward** (`redo()`), so a step must carry what redo needs that is not yet in the
  document: an `addstroke`/`addpage` refers to its content as a *live* ref at write time, which does not
  exist yet at replay - the journal has to write that content as an `edef`/`pdef` instead (a writer
  flag forcing definitions for the item's own subject).
- The `<verify>` record is written once, up front, for the sidecar; a journal would put per-step
  verification in each step, or rely on replay order alone.
