#pragma once

// Reader for Noteful's notebook export (.noteful).  The format is undocumented; everything here was
//  worked out from sample exports, and tools/noteful-dump.py is the reference implementation it was
//  checked against (its header describes the layout in full).  This file only decodes - it has no
//  dependency on the rest of Sumi, so it is testable on its own (scribbletest/notefultest.cpp);
//  notefulimport.cpp turns the result into a Document.
//
// Coordinates are Noteful's: pixels at 132 dpi, origin top left (an A4 page is 1091.34 x 1543.46).
// Anything the decoder does not understand is skipped rather than failing the whole notebook, except
//  a malformed container or stroke blob, which means the rest of that page cannot be trusted either.

#include <stdint.h>
#include <string>
#include <vector>

namespace Noteful {

// Noteful's page units per inch, and per PDF point (the unit of paper template line spacing)
static constexpr double UNITS_PER_INCH = 132;
static constexpr double UNITS_PER_POINT = UNITS_PER_INCH/72;

struct Point { double x, y; };

struct Stroke {
  uint32_t layer = 0;
  double width = 1;
  double rgba[4] = {0, 0, 0, 1};
  std::vector<Point> points;
  // one value per point for pressure pens, else empty.  The scale is Noteful's (seen 0.2 - 1.3); which
  //  of the two range values stored with it is the top is unconfirmed.
  std::vector<float> pressure;
};

// a line, curve or other vector shape; points are absolute - the element's origin and rotation applied,
//  and ellipses and regular polygons, which Noteful stores as parameters only, generated as paths
struct Shape {
  // op codes, each consuming that many points: 0 move (1), 1 line (1), 2 quad (2), 3 cubic (3), 4 close (0)
  enum { MOVE = 0, LINE = 1, QUAD = 2, CUBIC = 3, CLOSE = 4 };
  // 20 line, 21 curve, 12 polygon (all three carry a path), 6 ellipse, 3 regular polygon
  enum { KIND_REGULAR_POLYGON = 3, KIND_ELLIPSE = 6, KIND_POLYGON = 12, KIND_LINE = 20, KIND_CURVE = 21 };
  uint32_t layer = 0;
  int kind = 0;
  // drawn with Noteful's highlighter (element field 9): stored opaque, rendered translucent under the ink
  bool highlighter = false;
  double width = 1;
  double rgba[4] = {0, 0, 0, 1};
  std::vector<Point> points;
  std::vector<int> ops;
};

struct TextBox {
  uint32_t layer = 0;
  double x = 0, y = 0, width = 0, height = 0;
  std::string text;
  std::vector<std::string> tags;  // the #tags Noteful found in the text
};

// a pasted image, shown cropped: `crop` (in the image's display units, displayW x displayH) is scaled
//  to fill the element's frame
struct ImageItem {
  uint32_t layer = 0;
  double x = 0, y = 0, width = 0, height = 0;
  double rotation = 0;  // radians, about the frame centre - as for shapes
  std::string assetId;
  double displayW = 0, displayH = 0;
  double cropX0 = 0, cropY0 = 0, cropX1 = 0, cropY1 = 0;
};

struct Page {
  std::string id;
  double width = 0, height = 0;
  // Paper templates (Grid, Ruled, ...) carry their spacing; lineHeight and lineWidth are in PDF points.
  //  Anything else - an imported PDF page or photo, a cover - is the asset rendered as the background.
  bool isPaper = false;
  std::string templateName, templateClass;
  double lineHeight = 0, lineWidth = 0;
  int lineType = 0;  // 2 = grid; otherwise horizontal lines only
  bool hasPaperColor = false;
  uint32_t paperColor = 0xFFFFFF;  // 0xRRGGBB
  std::string assetId;  // background (the paper's own PDF rendering, for paper templates)
  int assetPage = 0;    // page within a PDF asset
  // A cropped page shows part of its asset: the whole asset page is drawn at this rect (x, y, w, h,
  //  page coordinates - x/y may be negative).  Without it the asset fills the page.
  bool hasAssetRect = false;
  double assetRect[4] = {0, 0, 0, 0};
  std::vector<std::string> tags;
  std::vector<Stroke> strokes;
  std::vector<Shape> shapes;
  std::vector<TextBox> texts;
  std::vector<ImageItem> images;
  // lowest point of the content - ink may run past `height`, and Noteful keeps it
  double contentBottom() const;
};

struct Layer {
  uint32_t id = 0;
  std::string name;
  float opacity = 1;
};

struct OutlineEntry {
  std::string pageId;
  std::string title;
  int level = 0;  // depth in the outline tree, 0 = top level
};

struct Notebook {
  std::string title;
  std::vector<Layer> layers;  // bottom first
  std::vector<Page> pages;    // in order
  std::vector<OutlineEntry> outline;  // in order
  std::vector<std::string> tags;  // every tag in the notebook, deduplicated ("Schule/Mathe" is one tag path)
  // things that were skipped (an unknown stroke record ends that page's ink early, for instance)
  std::vector<std::string> warnings;
  // raw bytes of an asset (PDF or JPEG/PNG), or an empty string if there is none with that id
  std::string asset(const std::string& id) const;

  // the whole file; assets are sliced out of it on demand
  std::string data;
  struct Span { size_t offset, length; };
  std::vector<std::pair<std::string, Span>> objects;
};

// false on failure, with a message in *error
bool parse(std::string data, Notebook* out, std::string* error);
bool load(const char* filename, Notebook* out, std::string* error);

}  // namespace Noteful
