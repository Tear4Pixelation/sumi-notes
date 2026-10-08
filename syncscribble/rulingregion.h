#pragma once

// Ruling regions: an area of a page with its own ruling, overriding the page's inside it.
//
// The motivating case is a scanned or imported page whose printed lines are uneven or tilted: an
//  opaque region covers them with clean paper and draws its own lines, and every ruled tool - the
//  centre-on-line marker, ruled select and erase, insert space ruled, snap to grid - follows the
//  region's lines while the pen is inside it.
//
// This file is the geometry only, with no dependency on Element/Page, so it tests standalone like
//  shape.cpp.  The region itself is an Element (see Element::isRulingRegion()); what a ruled tool asks
//  is Page::rulingAt(point), which answers with a RulingFrame - the region's, or the page's own.
//
// Two separate things describe a region, deliberately:
//  - the outline (`corners`), where the region applies and what its paper covers.  Its corners are
//    dragged freely, so it can fit whatever space the page leaves.
//  - the ruling (`origin`, `angle`, pitches), which way the lines run.  Lines are always parallel and
//    evenly spaced: bending them to follow a skewed outline would give "line N" no single height, and
//    every ruled tool depends on that.
//  Reshaping the outline therefore never moves a line; transforming the region (move, rotate, scale)
//  moves both, together with the ink on it, so handwriting stays registered to its lines.

#include <functional>
#include "basics.h"
#include "ulib/path2d.h"

// The ruling in effect at a point.  Local coordinates are the frame's own: lines of the y ruling are
//  horizontal there, at local y = k*yRuling, and the x ruling's are vertical at local x = k*xRuling.
//  The page's own ruling is the frame with angle 0 and origin (0, yRuleOffset).
struct RulingFrame
{
  Point origin;
  Dim angle = 0;  // radians, direction of the y ruling's lines
  Dim xRuling = 0;
  Dim yRuling = 0;
  Dim dotRadius = 0;
  // music staves (see staffLineOffsets): yRuling is then the height of one staff band, not a line pitch
  bool staff = false;
  // the region this frame belongs to (an Element*, opaque here); NULL for the page's own ruling
  const void* region = NULL;

  RulingFrame() {}
  RulingFrame(Point o, Dim a, Dim xr, Dim yr, Dim dr = 0, const void* rg = NULL, bool st = false)
      : origin(o), angle(a), xRuling(xr), yRuling(yr), dotRadius(dr), staff(st), region(rg) {}

  Point toLocal(Point p) const;
  Point toPage(Point local) const;
  // a direction (not a position) in page coords, e.g. toPageDir(Point(0, 1)) is "down the lines"
  Point toPageDir(Point localdir) const;
  Point toLocalDir(Point pagedir) const;
  // the same maps as transforms, for PathPointIter and friends
  Transform2D localTransform() const;
  Transform2D pageTransform() const;
  // bounding box, in local coordinates, of a page-space rect (exact for an unrotated frame)
  Rect localBBox(const Rect& pagerect) const;
  // bounding box, in page coordinates, of a local rect
  Rect pageBBox(const Rect& localrect) const;
  bool isRotated() const { return angle != 0; }
  // line height, with the blank-page fallback every ruled tool has always used
  Dim yrulingOr(Dim fallback) const { return yRuling > 0 ? yRuling : fallback; }
  // index of the line band containing p: band k is local y in [k*yr, (k+1)*yr)
  int line(Point p, Dim fallback) const;
  // local y of the top of band `line`
  Dim yForLine(int line, Dim fallback) const { return line*yrulingOr(fallback); }
  // the four page-space corners of band `line` spanning local x in [x0, x1]
  std::vector<Point> bandPolygon(int line, Dim x0, Dim x1, Dim fallback) const;
  // nearest grid point, in page coords; pitches <= 0 fall back to yr
  Point snapToGrid(Point p, Dim fallback) const;
};

// Where Insert Lines (ruled insert space held to Down) takes the text from (second-day report).  `localY`
//  is the press in a frame of single pitch `yr`; the result is the line (band) the moved text starts on,
//  in that frame, and whether that whole line moves (a block) or only its part right of the pen (a split).
//  - within INSERT_LINES_SNAP*yr of a rule line: the whole block from the line below that rule - pressed
//    on a rule you mean "everything from here down", whichever side of it the pen landed
//  - otherwise the line the pen is in, split at the pen; with Skip Lines (text on every second line) a
//    press on a line without ink is in the blank line above a text line, which moves that text line as a
//    block - it used to split it, so a block move needed the pen on the text line above, two lines up
//  `lineHasInk(line)` answers for a line of the same single-pitch frame; only asked with skipLines.
struct InsertLinesStart
{
  int line;
  bool wholeLine;
};
constexpr Dim INSERT_LINES_SNAP = 0.2;
InsertLinesStart insertLinesStart(Dim localY, Dim yr, bool skipLines, const std::function<bool(int)>& lineHasInk);

// The line height a selection's relative width is measured in: the ruling of the region most of its
//  strokes' centres lie in (frameAt answers per centre, as Page::rulingAt does), the page's own when that
//  is the plurality.  A tie goes to whichever of the tied regions holds the earliest stroke.  Strokes
//  spanning regions are therefore resolved by where most of the ink is, never by an average - a mean of
//  two pitches would be a line height that exists nowhere.  `fallback` stands in for an unruled frame.
Dim selectionLineHeight(const std::vector<Point>& centres, const std::function<RulingFrame(Point)>& frameAt, Dim fallback);

// Music paper ("staff" ruling).  The y ruling is the height of one band, which holds one staff of
//  STAFF_LINES lines; the staff spacing is yRuling/STAFF_BAND_SPACES and the staff sits centred in its
//  band, so a band is one "line of writing" for every ruled tool.  Lines of band k, as local y offsets
//  from k*yRuling.  A music ruling has no x ruling and no dots.
constexpr int STAFF_LINES = 5;
constexpr Dim STAFF_BAND_SPACES = 9;  // 4 spaces of staff + 5 of gap, so ~11 staves on A4 at yRuling 72
inline Dim staffLineOffset(int lineInStaff, Dim yRuling)
{
  return (STAFF_BAND_SPACES - (STAFF_LINES - 1))/2*yRuling/STAFF_BAND_SPACES + lineInStaff*yRuling/STAFF_BAND_SPACES;
}

struct RulingRegionParams
{
  std::vector<Point> corners;  // outline, in order around it; normally 4
  Point origin;                // a point every y ruling line is an integer number of pitches from
  Dim angle = 0;
  Dim xRuling = 0;
  Dim yRuling = 0;
  Dim dotRadius = 0;
  bool staff = false;          // music staves instead of lines (see staffLineOffset)
  bool opaque = true;          // paper-colored fill hides the page's ruling (or scan) behind it
  bool outline = false;        // a border in the rule color, a little heavier than a rule line

  bool isValid() const { return corners.size() >= 3; }
  // Clamp what came from a file or a peer to something drawable: finite numbers, and a pitch no finer
  //  than MIN_PITCH (a pitch of 1e-9 would otherwise ask for billions of lines).  A non-finite outline
  //  is cleared, which makes the params invalid.
  void sanitize();
  static constexpr Dim MIN_PITCH = 2;
  RulingFrame frame(const void* region = NULL) const
      { return RulingFrame(origin, angle, xRuling, yRuling, dotRadius, region, staff); }
  bool contains(Point p) const;
  // p itself when inside the outline, else the nearest point on it - what keeps a stroke begun in the
  //  region from leaving it
  Point clampInside(Point p) const;
  Rect bounds() const;
  // the outline as a closed path (the paper fill)
  Path2D outlinePath() const;
  // rule lines clipped to the outline, as a path to be stroked; empty when dotRadius > 0
  Path2D linesPath() const;
  // dots clipped to the outline, as a path to be filled; empty when dotRadius <= 0
  Path2D dotsPath() const;
  // apply a similarity transform (move/rotate/uniform scale) to outline and ruling alike
  void transform(const Transform2D& tf);
  // corner index (0-3) of the outline's bottom-right corner as seen in the ruling's frame - where the
  //  region's "..." button sits
  int bottomRightCorner() const;

  // an axis-aligned region over `r` with the given ruling, lines phased from r's top-left corner; new
  //  regions are outlined, so they can be told apart from the page
  static RulingRegionParams fromRect(const Rect& r, Dim xr, Dim yr, Dim dotr = 0, bool staff = false);
};

// serialization helpers for the __rr* attributes
std::string serializeRegionPoints(const std::vector<Point>& points);
void parseRegionPoints(const char* str, std::vector<Point>& points);
