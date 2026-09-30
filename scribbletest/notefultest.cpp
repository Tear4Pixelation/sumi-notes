// Unit tests for the Noteful reader in syncscribble/notefulfile.cpp.  Like librarytest.cpp these need no
//  GL context and no document, so they also build and run on their own:
//
//   g++ -std=c++14 -O2 -I syncscribble -DNOTEFULTEST_MAIN
//       scribbletest/notefultest.cpp syncscribble/notefulfile.cpp -o notefultest && ./notefultest
//   (one command, run from the repo root.)
//
// The notebook under test is synthesized here by a small encoder, so no real (personal) notebook has to
//  be checked in.  Each check pins a detail of the format that was found the hard way and is easy to
//  break: 4-point strokes being raw floats although the box form is the same size, the pressure arrays
//  of flag-2 strokes, colour records, layer ids, page order and deletion, outline nesting.
// Given file arguments, the standalone build also prints a summary of each real notebook, for comparing
//  against tools/noteful-dump.py.
// runNotefulTests() returns the number of failed checks and is also called from ScribbleTest::runAll().

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <string>
#include <vector>

#include "notefulfile.h"

static int nNotefulChecksFailed = 0;

static void notefulCheck(bool condition, const char* what)
{
  if(!condition) {
    ++nNotefulChecksFailed;
    fprintf(stderr, "Noteful check failed: %s\n", what);
  }
}

namespace {

// --- a minimal encoder for the record format (big-endian throughout) ---

void put16(std::string* out, unsigned value) { out->push_back(char(value >> 8));  out->push_back(char(value)); }
void put32(std::string* out, uint32_t value) { put16(out, value >> 16);  put16(out, value & 0xFFFF); }
void put64(std::string* out, uint64_t value) { put32(out, uint32_t(value >> 32));  put32(out, uint32_t(value)); }
void putF32(std::string* out, float value) { uint32_t bits;  memcpy(&bits, &value, 4);  put32(out, bits); }
void putF64(std::string* out, double value) { uint64_t bits;  memcpy(&bits, &value, 8);  put64(out, bits); }

struct Rec {
  std::string bytes;

  void head(int fid, int flags, int type) { put16(&bytes, fid);  bytes.push_back(char(flags));  bytes.push_back(char(type)); }
  // timestamped, as most of Noteful's own string fields are - the reader must skip the clock
  Rec& str(int fid, const std::string& value)
    { head(fid, 0x08, 5);  put32(&bytes, uint32_t(value.size()));  bytes += value;  put64(&bytes, 811157666073062ull);  return *this; }
  Rec& strs(int fid, const std::vector<std::string>& values)
  {
    head(fid, 0x04, 5);  put32(&bytes, uint32_t(values.size()));
    for(const std::string& value : values) { put32(&bytes, uint32_t(value.size()));  bytes += value; }
    return *this;
  }
  Rec& rec(int fid, const Rec& value)
    { head(fid, 0, 7);  put32(&bytes, uint32_t(value.bytes.size()));  bytes += value.bytes;  return *this; }
  Rec& recs(int fid, const std::vector<Rec>& values)
  {
    head(fid, 0x04, 7);  put32(&bytes, uint32_t(values.size()));
    for(const Rec& value : values) { put32(&bytes, uint32_t(value.bytes.size()));  bytes += value.bytes; }
    return *this;
  }
  Rec& blob(int fid, const std::string& value)
    { head(fid, 0, 6);  put32(&bytes, uint32_t(value.size()));  bytes += value;  return *this; }
  Rec& i64(int fid, int64_t value) { head(fid, 0, 2);  put64(&bytes, uint64_t(value));  return *this; }
  Rec& i64s(int fid, const std::vector<int64_t>& values)
    { head(fid, 0x04, 2);  put32(&bytes, uint32_t(values.size()));  for(int64_t v : values) put64(&bytes, uint64_t(v));  return *this; }
  Rec& u32(int fid, uint32_t value) { head(fid, 0, 0x12);  put32(&bytes, value);  return *this; }
  Rec& u8s(int fid, const std::vector<int>& values)
    { head(fid, 0x04, 1);  put32(&bytes, uint32_t(values.size()));  for(int v : values) bytes.push_back(char(v));  return *this; }
  Rec& f64s(int fid, const std::vector<double>& values)
    { head(fid, 0x04, 0x20);  put32(&bytes, uint32_t(values.size()));  for(double v : values) putF64(&bytes, v);  return *this; }
  Rec& f64(int fid, double value) { head(fid, 0, 0x20);  putF64(&bytes, value);  return *this; }
  Rec& f32s(int fid, const std::vector<float>& values)
    { head(fid, 0x04, 3);  put32(&bytes, uint32_t(values.size()));  for(float v : values) putF32(&bytes, v);  return *this; }
  Rec& i32s(int fid, const std::vector<int>& values)
    { head(fid, 0x04, 0x14);  put32(&bytes, uint32_t(values.size()));  for(int v : values) put32(&bytes, uint32_t(v));  return *this; }
  Rec& pair(int fid, double first, double second)
    { head(fid, 0, 0x21);  putF64(&bytes, first);  putF64(&bytes, second);  return *this; }
  Rec& enum16(int fid, int value) { head(fid, 0, 0x11);  put16(&bytes, value);  return *this; }
};

// a collection: 1 ids, 2 timestamps, 3 alive flags, 0 the objects (field 1 of each is its id - except
//  for layers, whose field 1 is the name and whose ids are u32s, hence `withIds`)
Rec collection(const std::vector<std::pair<Rec, int>>& items, bool withIds = true)
{
  std::vector<std::string> ids;
  std::vector<int64_t> stamps;
  std::vector<int> alive;
  std::vector<Rec> objects;
  for(size_t ii = 0; ii < items.size(); ++ii) {
    ids.push_back("ID" + std::to_string(ii));
    stamps.push_back(811157666073062ll + ii);
    alive.push_back(items[ii].second);
    Rec object;
    if(withIds)
      object.str(1, ids.back());
    object.bytes += items[ii].first.bytes;
    objects.push_back(object);
  }
  Rec coll;
  coll.strs(1, ids).i64s(2, stamps).u8s(3, alive).recs(0, objects);
  return coll;
}

std::string idOf(int index) { return "ID" + std::to_string(index); }

// f1 01 stroke header; `flags` 2 adds the per-segment arrays
std::string strokeHead(uint32_t layer, double width, uint64_t count, int flags = 0)
{
  std::string out;
  put16(&out, 0xF101);
  put64(&out, 0x1f97fede80de5051ull);
  put16(&out, flags);
  for(int ii = 0; ii < 3; ++ii)
    put64(&out, 812103491857440ull + ii);
  put32(&out, layer);
  putF64(&out, width);
  put64(&out, count);
  if(flags & 2) {
    put64(&out, count - 1);
    for(uint64_t ii = 0; ii < 2*(count - 1); ++ii)
      putF32(&out, 1.0f);
  }
  return out;
}

std::string rawStroke(uint32_t layer, const std::vector<Noteful::Point>& points)
{
  std::string out = strokeHead(layer, 0.9, points.size());
  for(const Noteful::Point& point : points) { putF32(&out, float(point.x));  putF32(&out, float(point.y)); }
  return out;
}

// box form: x, w, y, h then u16 pairs; pairs are given normalized (0..65535)
std::string boxStroke(uint32_t layer, float x, float w, float y, float h,
    const std::vector<std::pair<int, int>>& pairs, int flags = 0)
{
  std::string out = strokeHead(layer, 1.3, pairs.size(), flags);
  putF32(&out, x);  putF32(&out, w);  putF32(&out, y);  putF32(&out, h);
  for(const auto& pair : pairs) { put16(&out, pair.first);  put16(&out, pair.second); }
  return out;
}

std::string colorRecord(double red, double green, double blue, double alpha)
{
  std::string out;
  put16(&out, 0xF102);
  putF64(&out, red);  putF64(&out, green);  putF64(&out, blue);  putF64(&out, alpha);
  out.append(10, '\0');
  return out;
}

struct Object { std::string id, bytes; };

std::string container(const std::string& notebookId, const std::vector<Object>& objects)
{
  std::string out("\xAA\xBB\xCC\xDE", 4);
  std::vector<std::string> ids;
  std::vector<int64_t> offsets, lengths;
  for(const Object& object : objects) {
    ids.push_back(object.id);
    offsets.push_back(int64_t(out.size()));
    lengths.push_back(int64_t(object.bytes.size()));
    out += object.bytes;
  }
  Rec index;
  index.strs(2, {notebookId}).strs(10, ids).i64s(11, offsets).i64s(12, lengths);
  uint64_t indexOffset = out.size();
  out += index.bytes;
  out += std::string("\xAA\xBB\xCC\xDE", 4);
  put64(&out, indexOffset);
  put32(&out, uint32_t(index.bytes.size()));
  return out;
}

Rec layerRecord(const char* name, uint32_t id, const char* order)
{
  Rec layer;
  layer.str(1, name).u32(2, id).str(3, order);
  layer.head(6, 0x08, 3);  putF32(&layer.bytes, 1.0f);  put64(&layer.bytes, 0);
  return layer;
}

Rec pageRecord(const char* inkId, const std::vector<std::string>& tags, const Rec& tmpl, const char* order)
{
  Rec inkRef;
  inkRef.str(0, inkId).strs(2, {}).strs(3, tags);
  Rec page;
  page.rec(2, inkRef).rec(4, tmpl).str(5, order);
  return page;
}

// a horizontal line (kind 20); x, y is the frame's centre, as Noteful stores it
Rec lineElement(uint32_t layer, double x, double y, double length, bool highlighter = false)
{
  Rec frame;
  frame.f64s(1, {x, y, length, 0, 0});
  Rec style;
  Rec color;
  color.f32s(0, {0.2f, 0.4f, 0.6f, 1.0f});
  style.rec(7, color).f64(2, 1.8);
  Rec path;
  path.f64s(1, {0, 0, length, 0}).i32s(2, {0, 1});
  Rec body;
  body.i64(22, 288).i64(1, 20).pair(2, length, 0).rec(7, style).rec(13, path);
  Rec element;
  element.rec(2, frame).u32(4, layer).i64(5, 0).rec(6, body).str(7, "").f64(8, 1.0).enum16(9, highlighter ? 1 : 0);
  return element;
}

// a pasted image (kind 1), rotated, with no crop path
Rec imageElement(double rotation)
{
  Rec frame;
  frame.f64s(1, {415, 64, 546, 409, rotation});
  Rec body;
  body.i64(1, 1).pair(2, 546, 409).str(10, "IMGASSET").pair(11, 3264, 2448);
  Rec element;
  element.rec(2, frame).u32(4, 0).rec(6, body);
  return element;
}

// an ellipse (kind 6) or regular polygon (kind 3, `sides`): frame and style only, no path
Rec parametricElement(int kind, double x, double y, double w, double h, double rotation, int sides = 0)
{
  Rec frame;
  frame.f64s(1, {x, y, w, h, rotation});
  Rec style;
  Rec color;
  color.f32s(0, {0, 0, 0, 1.0f});
  style.rec(7, color).f64(2, 1.8);
  Rec body;
  body.i64(22, 280).i64(1, kind).pair(2, w, h).rec(7, style);
  if(sides)
    body.f64(20, sides);
  Rec element;
  element.rec(2, frame).u32(4, 0).rec(6, body);
  return element;
}

Rec textElement(const std::vector<std::string>& runs, const std::vector<std::string>& tags)
{
  Rec frame;
  frame.f64s(1, {890, 27, 382, 35, 0});
  Rec rich;
  rich.strs(2, runs);
  Rec body;
  body.i64(1, 19).pair(2, 382, 35).rec(4, rich).strs(14, tags);
  Rec element;
  element.rec(2, frame).u32(4, 0).rec(6, body);
  return element;
}

const double A4_W = 1091.3385826771655, A4_H = 1543.464566929134;
const uint32_t LAYER2 = 0x9e1fb24c;

// Two pages stored in reverse order plus a deleted one; two layers stored top first; an outline with
//  a child stored before its parent.
std::string sampleNotebook(bool corruptSecondStroke = false)
{
  std::vector<Object> objects;

  // page "B" (second by order key): an imported PDF page, no ink record
  Rec importedTmpl;
  importedTmpl.enum16(0, 1).pair(1, 869, 1315.8).i64(3, 3).str(4, "PDFASSET").i64(5, 0);
  // cropped: the whole asset page is drawn 63.85 above the page's top edge (type 0x23, a rect)
  importedTmpl.head(9, 0, 0x23);
  for(double value : {0.0, -63.85, 869.0, 1543.46})
    putF64(&importedTmpl.bytes, value);
  Rec pageB = pageRecord("", {}, importedTmpl, "+F");

  // page "A": grid paper, strokes and elements
  Rec gridTmpl;
  gridTmpl.enum16(0, 2).pair(1, A4_W, A4_H).str(3, "GRIDPDF").str(4, "sha1").str(5, "cb.simpleline")
      .str(6, "{\"lh\":20,\"lt\":2,\"lw\":0.5,\"name\":\"Grid\",\"pc\":16710894,\"si\":[1091.3,1543.4],\"type\":0}");
  Rec pageA = pageRecord("INK", {"Jahre/11"}, gridTmpl, "+E");

  // deleted page, sorts first - must not appear
  Rec pageDeleted = pageRecord("", {}, gridTmpl, "+A");

  std::string blob;
  blob += rawStroke(0, {{10, 20}, {11, 21}, {12, 22}});                 // 3 points: raw floats
  blob += rawStroke(LAYER2, {{100, 200}, {101, 201}, {102, 202}, {103, 1800}});  // 4 points: raw floats too
  blob += colorRecord(0.886, 0.137, 0.016, 1);                           // colours the 4-point stroke
  blob += boxStroke(0, 50, 10, 60, 20, {{0, 0}, {65535, 0}, {65535, 65535}, {0, 65535}, {32768, 32768}}, 2);
  if(corruptSecondStroke)
    blob += std::string("\xF1\x07garbage", 9);
  blob += boxStroke(LAYER2, 300, 4, 400, 8, {{0, 65535}, {65535, 0}, {0, 0}, {65535, 65535}, {0, 0}});
  // pressure pen (flag 1): box, the pressure range (top, bottom), then x/y/pressure triples
  std::string pressured = strokeHead(0, 1.3, 5, 1);
  for(float value : {200.0f, 10.0f, 500.0f, 20.0f, 0.8f, 0.4f})
    putF32(&pressured, value);
  for(int ii = 0; ii < 5; ++ii) {
    put16(&pressured, ii*65535/4);  put16(&pressured, 65535 - ii*65535/4);  put16(&pressured, ii == 2 ? 65535 : 0);
  }
  blob += pressured;
  // short pressure strokes, layout unseen in any sample: f32 x/y/pressure triples (the predicted one),
  //  and f32 pairs then pressures (the fallback) - told apart by where the record ends and sane values
  std::string shortTriples = strokeHead(0, 1.3, 3, 1);
  for(float value : {700.0f, 710.0f, 0.5f,  701.0f, 711.0f, 0.9f,  702.0f, 712.0f, 0.3f})
    putF32(&shortTriples, value);
  blob += shortTriples;
  std::string shortPlanar = strokeHead(0, 1.3, 3, 1);
  for(float value : {800.0f, 810.0f,  801.0f, 811.0f,  802.0f, 812.0f,  0.25f, 0.5f, 0.75f})
    putF32(&shortPlanar, value);
  blob += shortPlanar;
  // the long form (box, range, u16 triples) with small numbers throughout: read as f32 triples every
  //  value is plausible, so only where the record ends can tell - it must not be misread
  std::string shortLong = strokeHead(0, 1.3, 3, 1);
  for(float value : {10.0f, 2.0f, 20.0f, 3.0f, 0.8f, 0.4f})
    putF32(&shortLong, value);
  for(int value : {0, 0, 0,  0, 0, 0,  65535, 65535, 65535})
    put16(&shortLong, value);
  blob += shortLong;

  Rec ink;
  ink.i64(1, 288).blob(2, blob).i64s(3, {}).i64s(4, {})
      .rec(5, collection({{lineElement(LAYER2, 568.88, 1693.38, 324.54), 1},
                          {textElement({"#Jahre/11, #Schule/Mathe", "\xE2\x80\x8B"}, {"Jahre/11", "Schule/Mathe"}), 1},
                          {parametricElement(6, 100, 100, 40, 20, 0), 1},
                          {imageElement(0.5), 1},
                          // a triangle turned a quarter turn: must point right, like a logic gate
                          {parametricElement(3, 0, 0, 10, 10, 1.5707963267948966, 3), 1},
                          {lineElement(0, 244.09, 59.79, 308.15, true), 1},
                          {lineElement(0, 0, 0, 5), 0}}));  // deleted element
  objects.push_back(Object{"INK", ink.bytes});
  objects.push_back(Object{"PDFASSET", std::string("%PDF-1.3 fake")});

  // outline: child "Kettenregel" (under page A's entry) is stored before its parent
  std::vector<std::pair<Rec, int>> outline;
  Rec child;   child.str(2, idOf(1)).str(3, "ID1").str(4, "Kettenregel").str(5, "+b");
  Rec root;    root.str(2, idOf(1)).str(3, "").str(4, "Grundlagen").str(5, "+a");
  Rec second;  second.str(2, idOf(0)).str(3, "").str(4, "Anhang").str(5, "+c");
  outline.push_back({child, 1});
  outline.push_back({root, 1});
  outline.push_back({second, 1});

  Rec doc;
  doc.str(1, "NB").rec(2, collection({{pageB, 1}, {pageA, 1}, {pageDeleted, 0}}))
      .rec(3, collection({{layerRecord("Layer 2", LAYER2, "0+7h58m"), 1}, {layerRecord("Layer 1", 0, "0"), 1}}, false))
      .rec(5, collection(outline));
  Rec header;
  header.str(1, "NB").i64(2, 0).str(3, "Mathe");

  objects.insert(objects.begin(), Object{"n:NB", header.bytes});
  objects.push_back(Object{"d:NB", doc.bytes});
  return container("NB", objects);
}

bool near(double a, double b, double tolerance = 1e-3) { return fabs(a - b) <= tolerance; }

}  // namespace

int runNotefulTests()
{
  nNotefulChecksFailed = 0;
  Noteful::Notebook notebook;
  std::string error;
  bool ok = Noteful::parse(sampleNotebook(), &notebook, &error);
  notefulCheck(ok, "sample notebook parses");
  if(!ok) {
    fprintf(stderr, "  %s\n", error.c_str());
    return nNotefulChecksFailed;
  }

  notefulCheck(notebook.title == "Mathe", "title");
  notefulCheck(notebook.warnings.empty(), "no warnings on a well formed notebook");

  // layers come back bottom first, by order key, whatever order they are stored in
  notefulCheck(notebook.layers.size() == 2, "two layers");
  if(notebook.layers.size() == 2) {
    notefulCheck(notebook.layers[0].name == "Layer 1" && notebook.layers[0].id == 0, "Layer 1 at the bottom, id 0");
    notefulCheck(notebook.layers[1].name == "Layer 2" && notebook.layers[1].id == LAYER2, "Layer 2 above, its u32 id");
  }

  // pages in order-key order, deleted one dropped
  notefulCheck(notebook.pages.size() == 2, "deleted page is dropped");
  if(notebook.pages.size() != 2)
    return nNotefulChecksFailed;
  const Noteful::Page& grid = notebook.pages[0];
  const Noteful::Page& imported = notebook.pages[1];
  notefulCheck(grid.id == idOf(1) && imported.id == idOf(0), "pages sorted by order key, not stored order");

  notefulCheck(grid.isPaper && grid.templateName == "Grid" && grid.lineType == 2, "grid template recognized");
  notefulCheck(near(grid.lineHeight, 20) && near(grid.lineWidth, 0.5), "line spacing read in points");
  notefulCheck(grid.hasPaperColor && grid.paperColor == 16710894, "paper color");
  notefulCheck(near(grid.width, A4_W) && near(grid.height, A4_H), "page size from the 0x21 pair");
  notefulCheck(!imported.isPaper && imported.assetId == "PDFASSET" && imported.assetPage == 3,
      "imported page names its asset and page");
  notefulCheck(notebook.asset("PDFASSET") == "%PDF-1.3 fake", "asset bytes");
  notefulCheck(near(imported.width, 869) && imported.hasAssetRect && near(imported.assetRect[1], -63.85)
      && near(imported.assetRect[3], 1543.46), "cropped page: its own size and where the asset page goes");
  notefulCheck(imported.strokes.empty(), "page without an ink record has no ink");

  // strokes
  notefulCheck(grid.strokes.size() == 8, "eight strokes decoded");
  if(grid.strokes.size() == 8) {
    const Noteful::Stroke& shortLong = grid.strokes[7];
    notefulCheck(shortLong.points.size() == 3 && near(shortLong.points[0].y, 20) && near(shortLong.points[2].x, 12)
        && near(shortLong.pressure[2], 0.8), "short pressure stroke in the long form: only its end tells, and it does");
    const Noteful::Stroke& triples = grid.strokes[5];
    notefulCheck(triples.points.size() == 3 && near(triples.points[1].x, 701) && near(triples.points[2].y, 712)
        && triples.pressure.size() == 3 && near(triples.pressure[1], 0.9), "short pressure stroke: f32 x/y/pressure triples");
    const Noteful::Stroke& planar = grid.strokes[6];
    notefulCheck(planar.points.size() == 3 && near(planar.points[2].x, 802) && planar.pressure.size() == 3
        && near(planar.pressure[2], 0.75), "short pressure stroke: pairs then pressures, told apart by its values");
    const Noteful::Stroke& pressured = grid.strokes[4];
    notefulCheck(pressured.points.size() == 5 && near(pressured.points[1].x, 202.5, 0.01)
        && near(pressured.points[1].y, 515, 0.01), "pressure stroke: x/y from the triples, after the range");
    notefulCheck(pressured.pressure.size() == 5 && near(pressured.pressure[0], 0.4) && near(pressured.pressure[2], 0.8),
        "pressure scaled into its range");
    notefulCheck(grid.strokes[0].pressure.empty(), "no pressure on an ordinary stroke");
    const Noteful::Stroke& three = grid.strokes[0];
    notefulCheck(three.points.size() == 3 && near(three.points[2].x, 12) && near(three.points[2].y, 22),
        "3 point stroke is raw floats");
    const Noteful::Stroke& four = grid.strokes[1];
    // read as a box, this stroke would be a line from (100, 101) about 200 units tall
    notefulCheck(four.points.size() == 4 && near(four.points[1].x, 101) && near(four.points[3].y, 1800),
        "4 point stroke is raw floats, not a box");
    notefulCheck(four.layer == LAYER2, "stroke layer id");
    notefulCheck(near(four.rgba[0], 0.886) && near(four.rgba[2], 0.016) && near(four.rgba[3], 1),
        "colour record applies to the stroke before it");
    notefulCheck(grid.strokes[0].rgba[0] == 0 && grid.strokes[2].rgba[0] == 0, "other strokes stay black");
    const Noteful::Stroke& box = grid.strokes[2];
    notefulCheck(box.points.size() == 5 && near(box.points[1].x, 60) && near(box.points[2].y, 80)
        && near(box.points[4].x, 55, 0.01), "box stroke: u16 pairs scaled into x, width, y, height");
    notefulCheck(near(box.width, 1.3), "stroke width");
    // the flag-2 arrays sit between the header and the box; skipping them wrong misreads this one
    const Noteful::Stroke& after = grid.strokes[3];
    notefulCheck(after.points.size() == 5 && near(after.points[0].x, 300) && near(after.points[0].y, 408),
        "stroke after a flag-2 stroke still aligned");
  }
  notefulCheck(near(grid.contentBottom(), 1800), "content bottom covers ink below the page");

  // elements
  // frames are stored by their centre: read as a corner, each shape lands w/2, h/2 right of and below it
  notefulCheck(grid.shapes.size() == 4, "four live shapes (the deleted one is dropped)");
  if(grid.shapes.size() == 4) {
    const Noteful::Shape& ellipse = grid.shapes[1];
    notefulCheck(ellipse.kind == 6 && ellipse.points.size() == 13 && near(ellipse.points[0].x, 120)
        && near(ellipse.points[0].y, 100) && near(ellipse.points[3].y, 110), "ellipse generated to fill its frame");
    const Noteful::Shape& triangle = grid.shapes[2];
    notefulCheck(triangle.points.size() == 3 && triangle.ops.size() == 4 && near(triangle.points[0].x, 5)
        && near(triangle.points[0].y, 0), "regular polygon: apex at the top, rotated about the frame centre");
    const Noteful::Shape& line = grid.shapes[0];
    notefulCheck(line.layer == LAYER2 && line.kind == 20, "shape layer and kind");
    notefulCheck(line.points.size() == 2 && near(line.points[0].x, 568.88 - 324.54/2)
        && near(line.points[1].x, 568.88 + 324.54/2) && near(line.points[1].y, 1693.38),
        "shape points made absolute, the frame's x, y being its centre");
    notefulCheck(near(line.rgba[1], 0.4) && near(line.width, 1.8), "shape colour and width");
    notefulCheck(!line.highlighter && grid.shapes[3].highlighter, "element field 9 marks a highlighter");
  }
  notefulCheck(grid.images.size() == 1 && near(grid.images[0].rotation, 0.5) && near(grid.images[0].cropX1, 546)
      && near(grid.images[0].cropY1, 409), "image: rotation kept, no crop path means the whole image");
  notefulCheck(grid.images.size() == 1 && near(grid.images[0].x, 415 - 546/2.0) && near(grid.images[0].y, 64 - 409/2.0),
      "image frame placed by its centre");
  notefulCheck(grid.texts.size() == 1 && grid.texts[0].text.find("#Schule/Mathe") != std::string::npos,
      "text box runs concatenated");
  // the real notebook's tag box: by its corner it would end at 1272, past the 1091 wide page
  notefulCheck(grid.texts.size() == 1 && near(grid.texts[0].x, 890 - 382/2.0) && near(grid.texts[0].y, 27 - 35/2.0),
      "text box frame placed by its centre");

  // tags: the page's own list plus those found in text boxes, once each
  notefulCheck(notebook.tags.size() == 2 && notebook.tags[0] == "Jahre/11" && notebook.tags[1] == "Schule/Mathe",
      "tags deduplicated across page and text box");

  // outline: depth first, siblings by order key, child under its parent though stored before it
  notefulCheck(notebook.outline.size() == 3, "three outline entries");
  if(notebook.outline.size() == 3) {
    notefulCheck(notebook.outline[0].title == "Grundlagen" && notebook.outline[0].level == 0, "outline root first");
    notefulCheck(notebook.outline[1].title == "Kettenregel" && notebook.outline[1].level == 1, "child nested under it");
    notefulCheck(notebook.outline[2].title == "Anhang" && notebook.outline[2].level == 0, "then the next root");
    notefulCheck(notebook.outline[0].pageId == idOf(1), "outline entry names its page");
  }

  // damage: an unknown ink record keeps what came before it and says so
  Noteful::Notebook damaged;
  notefulCheck(Noteful::parse(sampleNotebook(true), &damaged, &error), "unknown ink record is not fatal");
  notefulCheck(!damaged.pages.empty() && damaged.pages[0].strokes.size() == 3 && damaged.warnings.size() == 1,
      "strokes before an unknown ink record are kept, with a warning");
  // a truncated file is refused rather than read out of bounds
  std::string truncated = sampleNotebook();
  truncated.resize(truncated.size()/2);
  notefulCheck(!Noteful::parse(truncated, &damaged, &error), "truncated file is refused");
  notefulCheck(!Noteful::parse(std::string("PK\x03\x04 not a notebook"), &damaged, &error), "zip is refused");

  return nNotefulChecksFailed;
}

#ifdef NOTEFULTEST_MAIN
int main(int argc, char* argv[])
{
  for(int ii = 1; ii < argc; ++ii) {
    Noteful::Notebook notebook;
    std::string error;
    if(!Noteful::load(argv[ii], &notebook, &error)) {
      printf("%s: %s\n", argv[ii], error.c_str());
      continue;
    }
    size_t strokes = 0, shapes = 0, texts = 0, images = 0;
    for(const Noteful::Page& page : notebook.pages) {
      strokes += page.strokes.size();  shapes += page.shapes.size();
      texts += page.texts.size();  images += page.images.size();
    }
    printf("%s: %zu pages, %zu strokes, %zu shapes, %zu texts, %zu images, %zu outline entries, %zu layers\n",
        notebook.title.c_str(), notebook.pages.size(), strokes, shapes, texts, images, notebook.outline.size(),
        notebook.layers.size());
    for(const std::string& tag : notebook.tags) printf("  tag %s\n", tag.c_str());
    for(const Noteful::OutlineEntry& entry : notebook.outline) printf("  %*s%s\n", 2*entry.level, "", entry.title.c_str());
    for(const std::string& warning : notebook.warnings) printf("  warning: %s\n", warning.c_str());
  }
  int failed = runNotefulTests();
  printf("Noteful tests: %d failed\n", failed);
  return failed ? 1 : 0;
}
#endif
