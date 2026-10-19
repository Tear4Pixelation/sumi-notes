// Smart scale ("Resize Handwriting"): the ScribbleArea side of the gesture - see docs/agent/smart-scale.md.
//  The layout itself is pure (smartscale.cpp); this gathers the selection's ink into it, applies the result
//  live as per-stroke pending transforms, and commits it as one undo step.

#include "scribblearea.h"
#include "scribbledoc.h"
#include "syncundo.h"
#include "rulingregion.h"

// drag range: 0.2 is about where handwriting stops being legible, 3x is past any sensible enlargement
static constexpr Dim SMARTSCALE_MIN = 0.2;
static constexpr Dim SMARTSCALE_MAX = 3;

bool ScribbleArea::canSmartScale() const
{
  if(!currSelection || !currSelection->selector || regionSelector || shapeSelector)
    return false;
  if(!currSelection->selector->offersSmartScale())  // a RectSelector (no RTTI in this build)
    return false;
  return currSelection->getFirstElement([](Element* s) { return !s->isRulingRegion(); }) != NULL;
}

bool ScribbleArea::smartScaleOn() const
{
  return currSelection && currSelection->selector && currSelection->selector->smartScale;
}

void ScribbleArea::toggleSmartScale()
{
  if(!canSmartScale())
    return;
  Selector* selector = currSelection->selector;
  dirtyScreen(currSelection->getBGBBox());
  selector->smartScale = !selector->smartScale;
  // ruled move mode hides the handles; the smart scale handle is asked for, so it is always shown
  selector->bgDirty = true;
  dirtyScreen(currSelection->getBGBBox());
  uiChanged(UIState::SelChange);
}

void ScribbleArea::smartScaleStart()
{
  smartScale.reset(new SmartScaleGesture);
  SmartScaleGesture& gesture = *smartScale;
  Page* page = currSelection->page;
  std::vector<Point> centres;
  for(Element* s : currSelection->strokes) {
    if(s->isRulingRegion())
      continue;  // a region's own handles size it; it is not handwriting
    gesture.strokes.push_back(s);
    centres.push_back(s->com());
  }
  Rect bbox = currSelection->getBBox();
  gesture.corner = bbox.origin();
  // the ruling the most strokes sit in, as for a selection's line height (selectionLineHeight())
  gesture.frame = selectionFrame(centres, [page](Point p) { return page->rulingAt(p); });
  // Skip Lines (the Insert Space tool's toggle, same flag): text is on every second line, so a "line" is a
  //  text line plus the blank one below, starting at the selection's first line (skippedLineFrame)
  const bool skipLines = scribbleDoc->scribbleMode->insSpaceSkipLines && gesture.frame.yRuling > 0
      && !gesture.frame.staff;
  if(skipLines) {
    int topLine = INT_MAX;
    for(const Point& centre : centres)
      topLine = std::min(topLine, gesture.frame.line(centre, Page::BLANK_Y_RULING));
    gesture.frame = skippedLineFrame(gesture.frame, topLine);
  }
  // a staff band is not a line handwriting sits on, so music paper scales plainly too
  gesture.ruled = gesture.frame.yRuling > 0 && !gesture.frame.staff;
  if(!gesture.ruled) {
    std::vector<Rect> boxes;
    for(Element* s : gesture.strokes)
      boxes.push_back(s->bbox());
    gesture.anchor = Point(bbox.left, smartScaleBaseline(boxes));
    return;
  }
  const RulingFrame& frame = gesture.frame;
  const Dim yr = frame.yRuling;
  gesture.params.yRuling = yr;
  gesture.params.baseline = skipLines ? 0.5 : 1;
  gesture.params.minWordGap = reflowWordSep*yr;
  Dim right = MIN_DIM;
  int firstLine = INT_MAX, lastLine = INT_MIN;
  for(Element* s : gesture.strokes) {
    SmartScaleItem item{localElementBBox(frame, s), frame.line(s->com(), Page::BLANK_Y_RULING)};
    right = std::max(right, item.box.right);
    firstLine = std::min(firstLine, item.line);
    lastLine = std::max(lastLine, item.line);
    gesture.items.push_back(item);
  }
  // lines fill up to the text's own right edge: the column the writer used, wherever the page ends
  gesture.params.right = right;
  // The column the text is in: separators (vertical, diagonal) found from the first word as ruled select
  //  finds them, held as shapes so a reflowed word is kept clear of a diagonal where it actually is.
  const Dim marginLeft = frame.region ? MIN_DIM : page->props.marginLeft;
  const SmartScaleItem* firstItem = &gesture.items.front();
  for(const SmartScaleItem& item : gesture.items) {
    if(item.line < firstItem->line || (item.line == firstItem->line && item.box.left < firstItem->box.left))
      firstItem = &item;
  }
  const Dim colX = firstItem->box.center().x;
  tempSelection = new Selection(page, Selection::STROKEDRAW_NORMAL);
  tempSelection->ruling = frame;
  RuledSelector* later = new RuledSelector(tempSelection, selColMode);
  later->findStops(colX, firstLine);
  gesture.ledges = later->ledges;
  gesture.redges = later->redges;
  if(!gesture.redges.empty()) {
    for(Element* s : gesture.strokes) {
      std::vector<Point> pts;
      if(s->isPathElement()) {
        const Path2D* path = static_cast<SvgPath*>(s->node)->path();
        Path2D flat = path->isSimple() ? *path : path->toFlat();
        flat.transform(frame.localTransform() * s->node->totalTransform());
        pts = flat.points;
      }
      else {
        Rect box = localElementBBox(frame, s);
        pts = {box.origin(), Point(box.right, box.top), Point(box.left, box.bottom), Point(box.right, box.bottom)};
      }
      gesture.ink.push_back(std::move(pts));
    }
    SmartScaleGesture* g = &gesture;
    // how far each word's ink is between the separators on line `line` when its rule is at ruleY
    auto wordBounds = [g](const SmartScaleWord& word, Dim scale, Dim ruleY, Dim& minX, Dim& maxX) {
      const Dim pitch = g->params.yRuling;
      const int line = int(std::floor(ruleY/pitch - g->params.baseline + 0.5));
      minX = MIN_DIM;
      maxX = MAX_DIM;
      if(line < 0 || line >= int(g->redges.size()))
        return;
      const Dim oldRuleY = (word.line + g->params.baseline)*pitch;
      for(int idx : word.items) {
        for(const Point& p : g->ink[idx]) {
          Dim dx = scale*(p.x - word.left), y = ruleY + scale*(p.y - oldRuleY);
          minX = std::max(minX, g->ledges[line].at(y, false) - dx);
          maxX = std::min(maxX, g->redges[line].at(y, true) - dx);
        }
      }
    };
    // ink the writer already put across a separator is not pushed off its line for it: each word keeps
    //  the slack it had where it was, so scale 1 is still exactly where it started
    for(const SmartScaleWord& word : smartScaleWords(gesture.items, gesture.params.minWordGap)) {
      Dim minX, maxX;
      wordBounds(word, 1, (word.line + gesture.params.baseline)*yr, minX, maxX);
      gesture.slack[word.items.front()] = Point(std::max(Dim(0), minX - word.left), std::max(Dim(0), word.left - maxX));
    }
    gesture.params.bounds = [g, wordBounds](const SmartScaleWord& word, Dim scale, Dim ruleY, Dim& minX, Dim& maxX) {
      wordBounds(word, scale, ruleY, minX, maxX);
      auto it = g->slack.find(word.items.front());
      if(it != g->slack.end()) {
        minX -= it->second.x;
        maxX += it->second.y;
      }
    };
  }
  // What follows the text moves with it, by whole lines, like Insert Lines: everything editable on this
  //  ruling below the selection's last line, in its column, down to a horizontal separator (the end of
  //  the page for reflow, findSectionEnd()).  Other ink in the column on the selection's own lines stays,
  //  and the text below is never pulled up past it (blockedLine); ink in the page margin (bookmarks) does
  //  not block.
  int endRule = RuledSelector::separatorEndsSection ? later->findSectionEnd(colX, lastLine) : -1;
  if(endRule != lastLine + 1)
    later->selectRuled(MIN_DIM, lastLine + 1, MAX_DIM, endRule > lastLine ? endRule - 1 : MAX_LINE_NUM - 1);
  for(Element* s : page->children()) {
    if(s->selection() || s->isRulingRegion() || !page->isEditable(s))
      continue;
    if(page->rulingAt(s->com()).region != frame.region)
      continue;
    int line = frame.line(s->com(), Page::BLANK_Y_RULING);
    if(line < firstLine || line > lastLine)
      continue;
    Rect box = localElementBBox(frame, s);
    // a separator through these lines bounds the column, it is not text on them
    if(box.height() > RuledSelector::MIN_DIV_HEIGHT*yr)
      continue;
    if(box.right > std::max(marginLeft, later->lstopOver(line, box.top, box.bottom))
        && box.left < later->rstopOver(line, box.top, box.bottom))
      gesture.blockedLine = std::max(gesture.blockedLine, line);
  }
}

void ScribbleArea::smartScaleMove(Point pos)
{
  if(!smartScale || !currSelection)
    return;
  SmartScaleGesture& gesture = *smartScale;
  // the drag projected on the handle's diagonal from the selection's top left, as a region's size grip
  const Point d0 = initialPos - gesture.corner;
  Dim scale = dot(pos - gesture.corner, d0)/std::max(dot(d0, d0), Dim(1E-9));
  scale = std::min(std::max(scale, SMARTSCALE_MIN), SMARTSCALE_MAX);
  if(std::abs(scale - gesture.scale) < 1E-4)
    return;
  gesture.scale = scale;
  // every stroke is set to its target transform from where it started (not stepped from the last move),
  //  so a live preview never accumulates error and a drag back to 1 puts everything back exactly
  auto place = [](Element* s, const Transform2D& target) {
    ScribbleTransform delta = ScribbleTransform(target) * s->pendingTransform().inverse();
    if(!approxEq(delta, ScribbleTransform(), 1E-9))
      s->applyTransform(delta);  // internal scale 1: stroke widths scale with the ink
  };
  if(!gesture.ruled) {
    const Point& anchor = gesture.anchor;
    Transform2D tf(scale, 0, 0, scale, (1 - scale)*anchor.x, (1 - scale)*anchor.y);
    for(Element* s : gesture.strokes)
      place(s, tf);
  }
  else {
    const RulingFrame& frame = gesture.frame;
    SmartScaleLayout layout = smartScaleLayout(gesture.items, scale, gesture.params);
    for(size_t i = 0; i < gesture.strokes.size(); ++i) {
      const Point& from = layout.from[i];
      const Point& to = layout.to[i];
      Transform2D local(scale, 0, 0, scale, to.x - scale*from.x, to.y - scale*from.y);
      place(gesture.strokes[i], frame.pageTransform() * local * frame.localTransform());
    }
    // later content follows: down by the lines grown text took, up by the lines shrunk text freed - but
    //  not past other ink left on those lines
    int dLines = layout.newLastLine - layout.oldLastLine;
    if(dLines < 0)
      dLines = std::min(0, std::max(layout.newLastLine, gesture.blockedLine) - layout.oldLastLine);
    if(tempSelection) {
      Transform2D down = Transform2D::translating(frame.toPageDir(Point(0, dLines*frame.yRuling)));
      for(Element* s : tempSelection->strokes)
        place(s, down);
      tempSelection->invalidateBBox();
    }
  }
  currSelection->invalidateBBox();
  currSelection->shrink();
}

// one undo step: the release's startAction()/endAction() wrap these items
void ScribbleArea::smartScaleFinish()
{
  // released first: an item added below can reach invalidateStroke() (sync), which cancels a live gesture
  std::unique_ptr<SmartScaleGesture> gesture = std::move(smartScale);
  if(!gesture || !currSelection)
    return;
  Page* page = currSelection->page;
  UndoHistory* hist = scribbleDoc->history;
  auto commit = [&](Element* s) {
    ScribbleTransform tf = s->pendingTransform();
    if(tf.isIdentity())
      return;
    if(hist->undoable()) {
      if(tf.isTranslate())
        hist->addItem(new StrokeTranslateItem(s, page, tf.xoffset(), tf.yoffset()));
      else
        hist->addItem(new StrokeTransformItem(s, page, tf));
    }
    s->commitTransform();
    // a scaled image can change which side of a region of its layer it belongs on (Page::regionInsertPos)
    if(!tf.isTranslate())
      page->restackRegionsFor(s);
  };
  for(Element* s : gesture->strokes)
    commit(s);
  if(tempSelection) {
    for(Element* s : tempSelection->strokes)
      commit(s);
    tempSelection->invalidateBBox();
    if(tempSelection->count() > 0)
      growPage(tempSelection->getBBox());
  }
  currSelection->invalidateBBox();
  currSelection->shrink();
  growPage(currSelection->getBBox());
}

void ScribbleArea::smartScaleCancel()
{
  if(!smartScale)
    return;
  for(Element* s : smartScale->strokes)
    s->resetTransform();
  if(tempSelection) {
    for(Element* s : tempSelection->strokes)
      s->resetTransform();
    tempSelection->invalidateBBox();
  }
  smartScale.reset();
  if(currSelection) {
    currSelection->invalidateBBox();
    currSelection->shrink();
  }
}
