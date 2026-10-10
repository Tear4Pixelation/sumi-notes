// Unit tests for the smart scale layout in syncscribble/smartscale.cpp (docs/agent/smart-scale.md).  Like
//  erasetest.cpp these need no GL context and no document, so they also build and run on their own:
//
//   g++ -std=c++14 -O2 -DNDEBUG -I . -I syncscribble -isystem stb -DSMARTSCALETEST_MAIN
//       scribbletest/smartscaletest.cpp syncscribble/smartscale.cpp ulib/geom.cpp ulib/path2d.cpp
//       -o smartscaletest && ./smartscaletest
//   (one command, run from the repo root)
//
// runSmartScaleTests() returns the number of failed checks and is also called from ScribbleTest::runAll().
//  What needs a document - the gesture, its one undo step, later content pulling up - is in
//  ScribbleTest::smartScaleTest().

#include <stdio.h>
#include <cmath>

#include "smartscale.h"

static int nSmartScaleChecksFailed = 0;

static void smartScaleCheck(bool condition, const char* what)
{
  if(!condition) {
    ++nSmartScaleChecksFailed;
    printf("FAIL: %s\n", what);
  }
}

static bool nearDim(Dim a, Dim b, Dim eps = 1E-6) { return std::abs(a - b) <= eps; }

// where the layout puts item `idx`
static Rect placedBox(const std::vector<SmartScaleItem>& items, const SmartScaleLayout& layout, int idx)
{
  const Rect& box = items[idx].box;
  const Point& from = layout.from[idx];
  const Point& to = layout.to[idx];
  const Dim scale = layout.scale;
  return Rect::ltrb(to.x + scale*(box.left - from.x), to.y + scale*(box.top - from.y),
      to.x + scale*(box.right - from.x), to.y + scale*(box.bottom - from.y));
}

// a word of handwriting on `line` of a yr ruling: two letters sitting on the rule, from x0 to x0 + width
static void addWord(std::vector<SmartScaleItem>& items, int line, Dim x0, Dim width, Dim yr)
{
  const Dim base = (line + 1)*yr;
  items.push_back(SmartScaleItem{Rect::ltrb(x0, base - 0.6*yr, x0 + width/2 - 1, base), line});
  items.push_back(SmartScaleItem{Rect::ltrb(x0 + width/2, base - 0.5*yr, x0 + width, base), line});
}

int runSmartScaleTests()
{
  nSmartScaleChecksFailed = 0;
  const Dim yr = 40;
  SmartScaleParams params;
  params.yRuling = yr;
  params.minWordGap = 0.3*yr;  // reflow's default minWordSep
  params.right = 680;  // the paragraph's right edge, as the gesture takes it from the ink

  // a paragraph of three full lines: five 100 wide words with 20 gaps, 100 to 680
  std::vector<SmartScaleItem> para;
  for(int line = 2; line < 5; ++line) {
    for(int word = 0; word < 5; ++word)
      addWord(para, line, 100 + 120*word, 100, yr);
  }

  // words come from the reflow's word break: 15 words, two strokes each
  std::vector<SmartScaleWord> words = smartScaleWords(para, params.minWordGap);
  smartScaleCheck(words.size() == 15 && words[0].items.size() == 2, "two strokes 1 apart are one word");
  smartScaleCheck(nearDim(writerWordGap({20, 20, 30}, params.minWordGap, yr), 20), "word gap is the median");
  smartScaleCheck(nearDim(writerWordGap({2}, params.minWordGap, yr), 15), "word gap at least 1.25 x min");
  smartScaleCheck(nearDim(writerWordGap({200}, params.minWordGap, yr), yr), "word gap at most a line");

  // scale 1 leaves a full paragraph where it is
  {
    SmartScaleLayout layout = smartScaleLayout(para, 1, params);
    bool same = true;
    for(size_t i = 0; i < para.size(); ++i) {
      Rect placed = placedBox(para, layout, int(i));
      same = same && nearDim(placed.left, para[i].box.left) && nearDim(placed.bottom, para[i].box.bottom);
    }
    smartScaleCheck(same, "scale 1 moves nothing");
    smartScaleCheck(layout.newLastLine == 4 && layout.oldLastLine == 4, "scale 1 keeps the line count");
  }

  // shrinking to half: ink stays on the rules, words pull back onto earlier lines, fewer lines
  {
    SmartScaleLayout layout = smartScaleLayout(para, 0.5, params);
    bool onRule = true, onLine = true;
    for(size_t i = 0; i < para.size(); ++i) {
      Rect placed = placedBox(para, layout, int(i));
      // every letter here sits on its rule, so after scaling it still ends exactly on a rule line
      Dim lines = placed.bottom/yr;
      onRule = onRule && nearDim(lines, std::round(lines));
      // and is half as tall
      onLine = onLine && nearDim(placed.height(), 0.5*para[i].box.height());
    }
    smartScaleCheck(onRule, "shrunk ink still sits on the rule lines");
    smartScaleCheck(onLine, "shrunk ink is scaled by the factor");
    // 50 wide words with 10 gaps: (680 - 100 + 10)/60 = 9 words fit on a line, so 15 words need 2 lines
    smartScaleCheck(layout.newLastLine == 3, "half size text takes two lines, not three");
    smartScaleCheck(layout.firstLine == 2 && layout.oldLastLine == 4, "line range reported");
    // the first word of the original second line is pulled back to the end of the first line
    int firstOfLine3 = words[5].items[0];
    Rect pulled = placedBox(para, layout, firstOfLine3);
    smartScaleCheck(nearDim(pulled.bottom, 3*yr), "a later line's word pulled back onto the first line");
    smartScaleCheck(nearDim(pulled.left, 100 + 5*60), "pulled-back word follows at the scaled word gap");
    // the first line still starts where it did
    smartScaleCheck(nearDim(placedBox(para, layout, 0).left, 100), "first line keeps its start");
  }

  // a paragraph break (blank line) is kept, and the second paragraph follows the first one up
  {
    std::vector<SmartScaleItem> two = para;
    for(int word = 0; word < 3; ++word)
      addWord(two, 6, 100 + 120*word, 100, yr);  // line 5 left blank
    SmartScaleLayout layout = smartScaleLayout(two, 0.5, params);
    Rect second = placedBox(two, layout, int(para.size()));
    smartScaleCheck(nearDim(second.bottom, 6*yr), "second paragraph starts one blank line below the first");
    smartScaleCheck(nearDim(second.left, 100), "second paragraph starts at its own left edge");
    smartScaleCheck(layout.newLastLine == 5 && layout.oldLastLine == 6, "paragraph break line count");
  }

  // growing wraps forward, onto the continuation lines' start (an indented first line keeps its indent)
  {
    std::vector<SmartScaleItem> indented;
    addWord(indented, 0, 160, 100, yr);
    addWord(indented, 0, 280, 100, yr);
    addWord(indented, 0, 400, 100, yr);
    addWord(indented, 1, 100, 100, yr);
    addWord(indented, 1, 220, 100, yr);
    SmartScaleParams narrow = params;
    narrow.right = 600;
    SmartScaleLayout layout = smartScaleLayout(indented, 1.5, narrow);
    // 150 wide words: 160 + 150 + 30 + 150 = 490, a third would end at 820 > 600
    Rect third = placedBox(indented, layout, 4);
    smartScaleCheck(nearDim(third.bottom, 2*yr) && nearDim(third.left, 100), "grown text wraps to the indent");
    smartScaleCheck(nearDim(placedBox(indented, layout, 0).left, 160), "first line keeps its indent");
    smartScaleCheck(layout.newLastLine > layout.oldLastLine, "grown text takes more lines");
  }

  // unruled: the baseline estimate ignores descenders
  {
    std::vector<Rect> boxes = {Rect::ltrb(0, 10, 5, 50), Rect::ltrb(6, 20, 9, 50), Rect::ltrb(10, 30, 14, 70),
        Rect::ltrb(15, 15, 20, 51)};
    smartScaleCheck(nearDim(smartScaleBaseline(boxes), 51) || nearDim(smartScaleBaseline(boxes), 50),
        "baseline is where most letters end, not the descender");
    smartScaleCheck(std::isnan(smartScaleBaseline({})), "no baseline without ink");
  }

  return nSmartScaleChecksFailed;
}

#ifdef SMARTSCALETEST_MAIN
int main()
{
  int failed = runSmartScaleTests();
  printf(failed ? "%d smart scale checks failed\n" : "all smart scale checks passed\n", failed);
  return failed ? 1 : 0;
}
#endif
