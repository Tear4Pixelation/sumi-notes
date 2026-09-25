# Layers - investigation and effort estimate (backend only)

**Status: the backend described here is built** (option A, with layer-ordered stacking). This
document is the rationale and the survey of the alternatives; `CLAUDE.md`'s *Layers* section is the
summary of what exists. There is no UI yet - the entry points are `ScribbleDoc`'s layer methods.

Goal: a document can have several layers; one is current; strokes on a non-current layer that is
locked cannot be selected, erased, moved or otherwise edited. Backend only - no UI here.

## 1. What the document model looks like today

A page is one `<svg>` (`Page::svgDoc`) with exactly two container children:

- `ruleNode` - `.ruleline`, the background (also where PDF import and document scan put their image)
- `contentNode` - `<g class="write-content">`, **a flat list of every stroke on the page**

`Page::children()` (`page.cpp:43`) is just `contentNode`'s children wrapped in `ElementIter`. There
is no grouping in between; `Element::parent()` is the content node for every stroke.

Everything that edits ink goes through three surfaces:

| surface | where |
|---|---|
| insert/remove | `Page::addStroke`/`removeStroke` → `contentNode->addChild(node, next)` |
| undo/redo | `StrokeAddedItem`/`StrokeDeletedItem` → `page->contentNode->addChild/removeChild` (`syncundo.cpp:36-66`) |
| sync | `<addstroke strokeuuid= pageuuid= nextstrokeuuid=>` (`scribblesync.cpp:514`) - position is a *sibling uuid*, not an index or a parent |

Selection and erase are pure iteration over that flat list:

- `Selection::doSelect` and friends - `selection.cpp:114, 126, 203, 339, 1042`
- free eraser / ruled free eraser - `scribblearea.cpp:1405, 1448, 2384`
- `groupStrokes` - `scribbledoc.cpp:776`; bookmarks - `bookmarkview.cpp:270`;
  sync full-page walk - `scribblesync.cpp:658, 863, 867`

Stroke eraser and ruled eraser are *implemented as selections* (comment at the top of
`selection.cpp`), so they are covered by whatever guards the selection loops.

## 2. Two ways to add layers

### A. Layer as an attribute on the element (recommended)

A stroke carries `__layer` (index or short string id), exactly the way it already carries
`__shape`, `__comx`, `__timestamp`. `Element::updateFromNode()` reads it, `Element::serializeAttr()`
writes it.

What this buys, all of it for free:

- **File format**: no new construct. Unknown-to-old-Write attributes are preserved by usvg round
  trip; an older Write build opens the document and sees one flat page, which is exactly what it
  should do.
- **Sync**: `<addstroke>` round-trips the node through `SvgWriter`/`SvgParser`, so `__layer` rides
  along with no protocol change - the same reason `__shape*` needed none.
- **Undo**: `StrokeAdded`/`Deleted`/`Transform` are all node-based and need no change at all. Only
  *moving a stroke between layers* needs a new item, modelled on `ShapeChangedItem`
  (+ one `layerchanged` branch in `ScribbleSync::processItem`, since undo items are the wire format).
- **Everything that draws, saves, hit-tests or walks the page keeps working unmodified.**

Z-order: two sub-options.
1. Layers are a *filter*, not a stacking order - z stays insertion order. Cheapest, and honest.
2. Keep `contentNode`'s children partitioned by layer index; `Page::addStroke` computes the
   insertion point as the end of its layer's run. Undo's and sync's `next`-sibling semantics are
   untouched because they are node-based. Costs ~30 lines plus a reorder path.
   Recommend (2) - real layers are expected to stack, and it is cheap here.

### B. Layer as a real `<g>` under the page svg

Structurally "correct", and considerably more expensive:

- `Page::contentNode` becomes a list; `Page::children()`, `strokeCount()`, `getBBox()` all change
- `Page::loadSVG` currently does `doc->selectFirst(".write-content")` - one node, by construction
- every `page->contentNode->addChild/removeChild` in `syncundo.cpp` and `scribblearea.cpp`
  (12 sites) must learn which parent
- `Selection::sourceNode` is *one* `Element*` (`selection.cpp:43`); selections would have to span
  parents, and `scribblearea.cpp:597` tests `s->node->parent() == page->contentNode`
- sync's `<addstroke>` gains a layer/parent field - a protocol change
- **file-format hazard**: an older Write build reads only the first `.write-content` and writes the
  document back; the other layers are dropped or orphaned. Option A has no such failure.

Not worth it for this feature.

## 3. Where "locked" actually has to be enforced (option A)

One predicate - `Element::layer()`, `Document::isLayerEditable(layer)` - consulted at these points.
Locking is a *edit* gate, not a draw gate: locked layers still render normally.

1. Selection hit loops - `selection.cpp:114, 126, 203, 339, 1042`. Covers rect/lasso/ruled select,
   **stroke eraser and ruled eraser**, `selectAll`, `invertSelection`, `selectbyProps`.
2. Free eraser and ruled free eraser - `scribblearea.cpp:1405, 1448`.
3. Drawing: new strokes get the current layer (`ScribbleArea` commit path → `Page::addStroke`).
4. Paste: `Clipboard::paste` must stamp the current layer onto pasted nodes, next to
   `replaceIds()` - otherwise a copy from a locked layer lands back on it, uneditable.
5. `Selection::insertSpace`/`reflowStrokes` and the ruled insert-space tool - these move strokes the
   user did not select. Decision needed: a locked layer should not shift.
6. `ScribbleDoc::groupStrokes` (`scribbledoc.cpp:776`) and `restyleToTheme` - both rewrite strokes
   wholesale; both need the guard (restyle is arguably a document operation that should ignore
   locks - decide explicitly).
7. Hyperlink hit (`Page::getHyperRef`) - following a link is not an edit; leave it.

Optional and nearly free once the attribute exists: per-layer **visibility** via
`Element::applyStyle()`, the same hook `isSelected` already uses.

## 4. Where the layer table lives

Layer definitions (name, order, locked, visible) are document data, not per-stroke. The cheapest
correct home is the per-document config (`ScribbleDoc::cfg`), which already round-trips through the
document's `<script type="text/writeconfig">` node - the same mechanism the theme recipe uses, so
no new file-format construct and no new parser. Caveat, inherited from the theme work: **the config
is not part of the sync protocol**, so on a shared whiteboard two clients would agree on every
stroke's `__layer` and disagree on the layer *names and locks*. Same known gap as the theme recipe;
fixing it properly means the `ThemeChangedItem`-style undo item `COLORS_SPEC.md` §8 already
specifies.

## 5. Effort

Option A, backend only, with tests:

| piece | est. |
|---|---|
| `__layer` on `Element` (parse/serialize), layer table in doc config, `Document`/`Page` accessors | 0.5 d |
| layer-ordered insertion in `Page::addStroke` + reorder path | 0.3 d |
| `LayerChangedItem` undo item + `layerchanged` sync branch | 0.4 d |
| enforcement at the ~8 sites in §3 + audit for missed edit paths | 1.0 d |
| `ScribbleTest::layerTest()`, each check mutation-verified per CLAUDE.md | 0.5 d |

**≈ 2.5-3 days, ~600-900 lines.** Option B is roughly 2-3x that plus a sync protocol change and the
old-build data-loss hazard.

Riskiest part is not the model, it is §3 item 5-6: the operations that touch strokes the user never
selected. Those are where a lock silently fails to hold.

## 6. Open decisions before implementing

- Layers per **document** or per **page**? Per document (Photoshop-like) is what "switch layer" implies;
  per page is simpler but means a layer list that changes as you scroll.
- Does a locked layer block *insert space* / *reflow* shifting its strokes? (recommend: yes)
- Does **Restyle to theme** ignore locks? (recommend: yes - it is a document operation)
- Ship visibility alongside lock, or lock only?
