#pragma once

// Smart scale ("Resize Handwriting", docs/agent/smart-scale.md): the layout behind shrinking or growing
//  selected handwriting while it stays on the ruled lines.  Every word is scaled about the point where it
//  meets its line's rule (its left edge on the baseline), so it keeps sitting on a line, and the words are
//  then laid out again as a paragraph: shrunk text pulls back onto earlier lines, grown text wraps forward.
//
// Pure geometry with no dependency on Element/Page, so it tests standalone (scribbletest/smartscaletest.cpp).
//  Everything is in a ruling frame's local coordinates, where lines are horizontal and band k is
//  y in [k*yr, (k+1)*yr) - handwriting sits on the rule at the bottom of its band, (k+1)*yr.

#include <vector>
#include "basics.h"
#include "ulib/path2d.h"

// one element (stroke) of the selection: its box and the line its centre of mass is on
struct SmartScaleItem
{
  Rect box;
  int line;
};

// a word: consecutive items on one line, closer than the reflow's word gap
struct SmartScaleWord
{
  std::vector<int> items;  // indices into the item list
  int line;
  Dim left, right;
};

struct SmartScaleParams
{
  Dim yRuling = 0;  // line pitch, > 0
  // a gap at least this wide between strokes on a line separates two words - reflow's minWordSep x pitch
  Dim minWordGap = 0;
  // lines are filled up to this local x
  Dim right = 0;
  // where in its band a line's rule is, as a fraction of the pitch: 1 = the band's bottom.  With Skip Lines
  //  a band is a text line plus the blank line below (pitch doubled) and the rule is at the middle, 0.5.
  Dim baseline = 1;
};

struct SmartScaleLayout
{
  // per item, local: p -> to + scale*(p - from)
  std::vector<Point> from, to;
  Dim scale = 1;
  int firstLine = 0;
  int oldLastLine = 0;  // last line holding ink before
  int newLastLine = 0;  // and after
};

// items grouped into words, in reading order (by line, then left edge)
std::vector<SmartScaleWord> smartScaleWords(const std::vector<SmartScaleItem>& items, Dim minWordGap);
// The writer's gap between words, as reflow measures it: the median gap between neighbouring words on a
//  line, clamped to [1.25 x minWordGap, one line height]; 1.25 x minWordGap if no line has two words.
Dim writerWordGap(const std::vector<Dim>& gaps, Dim minWordGap, Dim yRuling);
// Lay the words out again at `scale`.  Paragraphs (runs of lines with ink, split by blank lines) keep
//  their blank lines between them, their first line's start and their continuation lines' start (the
//  leftmost start of their later lines, else the first line's); word gaps scale with the ink.
SmartScaleLayout smartScaleLayout(const std::vector<SmartScaleItem>& items, Dim scale, const SmartScaleParams& params);
// Where unruled handwriting sits: the median of the boxes' bottoms - letters sit on the baseline and only
//  descenders reach below it, so most strokes end there.  NaN for no boxes.
Dim smartScaleBaseline(const std::vector<Rect>& boxes);
