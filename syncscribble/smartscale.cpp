#include "smartscale.h"
#include <algorithm>
#include <cmath>
#include <numeric>

std::vector<SmartScaleWord> smartScaleWords(const std::vector<SmartScaleItem>& items, Dim minWordGap)
{
  std::vector<int> order(items.size());
  std::iota(order.begin(), order.end(), 0);
  std::stable_sort(order.begin(), order.end(), [&items](int a, int b) {
    return items[a].line != items[b].line ? items[a].line < items[b].line : items[a].box.left < items[b].box.left;
  });
  // the same word break as Selection::reflowStrokes(): a stroke starting at least minWordGap right of
  //  everything before it on the line starts a new word
  std::vector<SmartScaleWord> words;
  for(int idx : order) {
    const SmartScaleItem& item = items[idx];
    if(words.empty() || words.back().line != item.line || item.box.left - words.back().right >= minWordGap)
      words.push_back(SmartScaleWord{{}, item.line, item.box.left, item.box.right});
    SmartScaleWord& word = words.back();
    word.items.push_back(idx);
    word.left = std::min(word.left, item.box.left);
    word.right = std::max(word.right, item.box.right);
  }
  return words;
}

Dim writerWordGap(const std::vector<Dim>& gaps, Dim minWordGap, Dim yRuling)
{
  if(gaps.empty())
    return 1.25*minWordGap;
  std::vector<Dim> sorted(gaps);
  std::nth_element(sorted.begin(), sorted.begin() + sorted.size()/2, sorted.end());
  return std::min(std::max(sorted[sorted.size()/2], 1.25*minWordGap), yRuling);
}

SmartScaleLayout smartScaleLayout(const std::vector<SmartScaleItem>& items, Dim scale, const SmartScaleParams& params)
{
  SmartScaleLayout layout;
  layout.scale = scale;
  layout.from.resize(items.size());
  layout.to.resize(items.size());
  std::vector<SmartScaleWord> words = smartScaleWords(items, params.minWordGap);
  if(words.empty() || params.yRuling <= 0)
    return layout;
  const Dim yr = params.yRuling;
  std::vector<Dim> gaps;
  for(size_t i = 1; i < words.size(); ++i) {
    if(words[i].line == words[i-1].line)
      gaps.push_back(words[i].left - words[i-1].right);
  }
  const Dim gap = scale*writerWordGap(gaps, params.minWordGap, yr);
  layout.firstLine = words.front().line;
  layout.oldLastLine = words.back().line;

  int newLine = words.front().line;
  size_t para = 0;
  while(para < words.size()) {
    // a paragraph: words on consecutive lines
    size_t paraEnd = para + 1;
    while(paraEnd < words.size() && words[paraEnd].line - words[paraEnd-1].line <= 1)
      ++paraEnd;
    // its blank lines above stay (none for the first paragraph, which starts where it was)
    if(para > 0)
      newLine += words[para].line - words[para-1].line;
    const Dim firstStart = words[para].left;
    Dim indent = MAX_DIM;
    for(size_t i = para; i < paraEnd; ++i) {
      if(words[i].line != words[para].line)
        indent = std::min(indent, words[i].left);
    }
    if(indent == MAX_DIM)
      indent = firstStart;
    Dim x = firstStart;
    bool lineEmpty = true;
    for(size_t i = para; i < paraEnd; ++i) {
      const SmartScaleWord& word = words[i];
      const Dim width = scale*(word.right - word.left);
      // a word too wide for any line still goes on a line of its own
      if(!lineEmpty && x + width > params.right) {
        ++newLine;
        x = indent;
        lineEmpty = true;
      }
      const Point from(word.left, (word.line + params.baseline)*yr);
      const Point to(x, (newLine + params.baseline)*yr);
      for(int idx : word.items) {
        layout.from[idx] = from;
        layout.to[idx] = to;
      }
      x += width + gap;
      lineEmpty = false;
    }
    para = paraEnd;
  }
  layout.newLastLine = newLine;
  return layout;
}

Dim smartScaleBaseline(const std::vector<Rect>& boxes)
{
  if(boxes.empty())
    return NAN;
  std::vector<Dim> bottoms;
  for(const Rect& box : boxes)
    bottoms.push_back(box.bottom);
  std::nth_element(bottoms.begin(), bottoms.begin() + bottoms.size()/2, bottoms.end());
  return bottoms[bottoms.size()/2];
}
