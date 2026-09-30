#include "notefulfile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <cmath>
#include <algorithm>
#include <map>

// Layout, in brief (tools/noteful-dump.py has the full description).  All integers are big-endian.
//  file:   magic AA BB CC DE, objects back to back, index record, 16 byte trailer
//          (magic, u64 index offset, u32 index length)
//  record: fields of u16 id, u8 flags, u8 type, value; flag 0x04 = array (u32 count first),
//          0x08 = a u64 timestamp follows the value
//  types:  1 u8, 0x11 u16, 3/0x12/0x14 32 bit, 2/4/0x13/0x20 64 bit, 0x21 two doubles, 0x23 four (a rect),
//          5 string, 6 blob, 7 nested record (u32 length first)
//  collection (pages, layers, outline, a page's elements): record with 1 ids, 3 alive flags, 0 objects

namespace Noteful {

namespace {

const unsigned char MAGIC[4] = {0xAA, 0xBB, 0xCC, 0xDE};
enum { FLAG_ARRAY = 0x04, FLAG_TIMESTAMP = 0x08 };
enum { STROKE_TAG = 0xF101, STROKE_COLOR_TAG = 0xF102 };
// sanity limit on any count read from the file, so a corrupt one cannot make us allocate gigabytes
const uint64_t MAX_COUNT = 1u << 24;
const double PI = 3.14159265358979323846;

struct Bytes {
  const unsigned char* data = nullptr;
  size_t size = 0;

  Bytes() {}
  Bytes(const unsigned char* d, size_t n) : data(d), size(n) {}
  std::string str() const { return std::string((const char*)data, size); }
  Bytes sub(size_t offset, size_t length) const { return Bytes(data + offset, length); }
};

uint16_t be16(const unsigned char* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t be32(const unsigned char* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint64_t be64(const unsigned char* p) { return uint64_t(be32(p)) << 32 | be32(p + 4); }
float f32(const unsigned char* p) { uint32_t bits = be32(p);  float value;  memcpy(&value, &bits, 4);  return value; }
double f64(const unsigned char* p) { uint64_t bits = be64(p);  double value;  memcpy(&value, &bits, 8);  return value; }

int fixedSize(int type)
{
  switch(type) {
  case 1: return 1;
  case 0x11: return 2;
  case 3: case 0x12: case 0x14: return 4;
  case 2: case 4: case 0x13: case 0x20: return 8;
  case 0x21: return 16;
  case 0x23: return 32;
  default: return -1;
  }
}

bool isLengthPrefixed(int type) { return type == 5 || type == 6 || type == 7; }

struct Field {
  int fid = -1;
  int type = 0;
  std::vector<Bytes> values;

  Bytes one() const { return values.empty() ? Bytes() : values.front(); }
};

class Record {
public:
  std::vector<Field> fields;

  bool parse(Bytes buf)
  {
    size_t pos = 0;
    while(pos < buf.size) {
      if(buf.size - pos < 4)
        return false;
      Field field;
      field.fid = be16(buf.data + pos);
      int flags = buf.data[pos + 2];
      field.type = buf.data[pos + 3];
      pos += 4;
      uint64_t count = 1;
      if(flags & FLAG_ARRAY) {
        if(buf.size - pos < 4)
          return false;
        count = be32(buf.data + pos);
        pos += 4;
        if(count > buf.size - pos)  // every value takes at least a byte
          return false;
      }
      for(uint64_t ii = 0; ii < count; ++ii) {
        size_t length;
        if(isLengthPrefixed(field.type)) {
          if(buf.size - pos < 4)
            return false;
          length = be32(buf.data + pos);
          pos += 4;
        }
        else {
          int size = fixedSize(field.type);
          if(size < 0)
            return false;  // unknown type: its size, and so everything after it, is unknown
          length = size;
        }
        if(buf.size - pos < length)
          return false;
        field.values.push_back(buf.sub(pos, length));
        pos += length;
      }
      if(flags & FLAG_TIMESTAMP) {
        if(buf.size - pos < 8)
          return false;
        pos += 8;
      }
      fields.push_back(std::move(field));
    }
    return true;
  }

  const Field* get(int fid) const
  {
    for(const Field& field : fields) {
      if(field.fid == fid)
        return &field;
    }
    return nullptr;
  }

  std::string text(int fid) const { const Field* field = get(fid);  return field ? field->one().str() : std::string(); }

  int64_t i64(int fid, int64_t dflt = 0) const
  {
    const Field* field = get(fid);
    return field && field->one().size == 8 ? int64_t(be64(field->one().data)) : dflt;
  }

  int u16(int fid) const
  {
    const Field* field = get(fid);
    return field && field->one().size == 2 ? be16(field->one().data) : 0;
  }

  uint32_t u32(int fid) const
  {
    const Field* field = get(fid);
    return field && field->one().size == 4 ? be32(field->one().data) : 0;
  }

  double f64(int fid, double dflt = 0) const
  {
    const Field* field = get(fid);
    return field && field->one().size == 8 ? Noteful::f64(field->one().data) : dflt;
  }

  std::vector<double> doubles(int fid) const
  {
    std::vector<double> result;
    if(const Field* field = get(fid)) {
      for(const Bytes& value : field->values) {
        if(value.size == 8) result.push_back(Noteful::f64(value.data));
        else if(value.size == 4) result.push_back(f32(value.data));
      }
    }
    return result;
  }

  std::vector<std::string> strings(int fid) const
  {
    std::vector<std::string> result;
    if(const Field* field = get(fid)) {
      for(const Bytes& value : field->values)
        result.push_back(value.str());
    }
    return result;
  }

  // a nested record; an empty one if the field is missing or does not parse
  Record sub(int fid) const
  {
    Record record;
    const Field* field = get(fid);
    if(field && field->type == 7 && !record.parse(field->one()))
      record.fields.clear();
    return record;
  }
};

// Live objects of a collection, in stored order: field 1 ids, 3 alive flags, 0 objects
std::vector<Record> collection(const Record& parent, int fid)
{
  std::vector<Record> result;
  Record coll = parent.sub(fid);
  std::vector<std::string> ids = coll.strings(1);
  const Field* alive = coll.get(3);
  std::map<std::string, bool> isAlive;
  for(size_t ii = 0; ii < ids.size(); ++ii)
    isAlive[ids[ii]] = !alive || ii >= alive->values.size() || alive->values[ii].size < 1 || alive->values[ii].data[0];
  if(const Field* objects = coll.get(0)) {
    for(const Bytes& raw : objects->values) {
      Record object;
      if(!object.parse(raw))
        continue;
      auto it = isAlive.find(object.text(1));
      if(it == isAlive.end() || it->second)
        result.push_back(std::move(object));
    }
  }
  return result;
}

// values of a flat JSON object - the paper templates are one level deep, so this is all that is needed
bool jsonValue(const std::string& json, const char* key, std::string* out)
{
  std::string quoted = std::string("\"") + key + "\"";
  size_t pos = json.find(quoted);
  if(pos == std::string::npos)
    return false;
  pos = json.find(':', pos + quoted.size());
  if(pos == std::string::npos)
    return false;
  ++pos;
  while(pos < json.size() && json[pos] == ' ') ++pos;
  if(pos < json.size() && json[pos] == '"') {
    size_t end = json.find('"', pos + 1);
    *out = json.substr(pos + 1, end == std::string::npos ? std::string::npos : end - pos - 1);
  }
  else {
    size_t end = json.find_first_of(",}]", pos);
    *out = json.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
  }
  return true;
}

bool jsonNumber(const std::string& json, const char* key, double* out)
{
  std::string value;
  if(!jsonValue(json, key, &value) || value.empty())
    return false;
  char* end = nullptr;
  *out = strtod(value.c_str(), &end);
  return end != value.c_str();
}

void addUnique(std::vector<std::string>* list, const std::string& item)
{
  if(!item.empty() && std::find(list->begin(), list->end(), item) == list->end())
    list->push_back(item);
}

// true if `pos` is the end of the blob or the start of a stroke or colour record
bool atRecord(Bytes blob, size_t pos)
{
  if(pos == blob.size)
    return true;
  if(blob.size - pos < 2)
    return false;
  int tag = be16(blob.data + pos);
  return tag == STROKE_TAG || tag == STROKE_COLOR_TAG;
}

bool plausibleCoord(double value) { return std::isfinite(value) && value > -1e5 && value < 1e5; }
bool plausiblePressure(double value) { return std::isfinite(value) && value >= 0 && value <= 100; }

// A pressure stroke of at most 4 points, whose layout no sample has shown yet.  Tries, in order of
//  likelihood: f32 x/y/pressure triples; f32 x/y pairs followed by the f32 pressures; the long form
//  (box, range, u16 triples).  Returns the bytes consumed, or 0 if none fits.
size_t parseShortPressure(Bytes blob, size_t pos, uint64_t count, Stroke* stroke)
{
  const unsigned char* data = blob.data + pos;
  size_t available = blob.size - pos;
  std::vector<Point> points;
  std::vector<float> pressure;
  auto accept = [&](size_t length) {
    for(size_t ii = 0; ii < points.size(); ++ii) {
      if(!plausibleCoord(points[ii].x) || !plausibleCoord(points[ii].y) || !plausiblePressure(pressure[ii]))
        return false;
    }
    return atRecord(blob, pos + length);
  };
  // interleaved triples
  if(available >= 12*count) {
    points.clear();  pressure.clear();
    for(uint64_t ii = 0; ii < count; ++ii) {
      points.push_back(Point{f32(data + 12*ii), f32(data + 12*ii + 4)});
      pressure.push_back(f32(data + 12*ii + 8));
    }
    if(accept(12*count)) {
      stroke->points = points;  stroke->pressure = pressure;
      return 12*count;
    }
    // pairs, then pressures
    points.clear();  pressure.clear();
    for(uint64_t ii = 0; ii < count; ++ii) {
      points.push_back(Point{f32(data + 8*ii), f32(data + 8*ii + 4)});
      pressure.push_back(f32(data + 8*count + 4*ii));
    }
    if(accept(12*count)) {
      stroke->points = points;  stroke->pressure = pressure;
      return 12*count;
    }
  }
  // the long form, as used from 5 points up
  if(available >= 24 + 6*count) {
    double boxX = f32(data), boxW = f32(data + 4), boxY = f32(data + 8), boxH = f32(data + 12);
    double rangeTop = f32(data + 16), rangeBottom = f32(data + 20);
    points.clear();  pressure.clear();
    for(uint64_t ii = 0; ii < count; ++ii) {
      const unsigned char* sample = data + 24 + 6*ii;
      points.push_back(Point{boxX + be16(sample)/65535.0*boxW, boxY + be16(sample + 2)/65535.0*boxH});
      pressure.push_back(float(rangeBottom + be16(sample + 4)/65535.0*(rangeTop - rangeBottom)));
    }
    if(accept(24 + 6*count)) {
      stroke->points = points;  stroke->pressure = pressure;
      return 24 + 6*count;
    }
  }
  return 0;
}

// The stroke blob (ink record field 2): f1 01 stroke records, each optionally followed by an f1 02
//  colour record.  Returns false if it stopped at something it did not understand.
bool parseStrokes(Bytes blob, std::vector<Stroke>* strokes)
{
  size_t pos = 0;
  while(pos < blob.size) {
    if(blob.size - pos < 2)
      return false;
    int tag = be16(blob.data + pos);
    if(tag == STROKE_TAG) {
      if(blob.size - pos < 56)
        return false;
      const unsigned char* head = blob.data + pos;
      int flags = be16(head + 10);
      Stroke stroke;
      stroke.layer = be32(head + 36);
      stroke.width = f64(head + 40);
      uint64_t count = be64(head + 48);
      pos += 56;
      if(count > MAX_COUNT)
        return false;
      if(flags & 2) {
        // two arrays of count-1 f32, one value per segment (pressure?) - not used
        if(blob.size - pos < 8)
          return false;
        uint64_t segments = be64(blob.data + pos);
        if(segments > MAX_COUNT || blob.size - pos - 8 < 8*segments)
          return false;
        pos += 8 + 8*segments;
      }
      bool hasPressure = flags & 1;
      // up to 4 points are stored as f32 pairs; at 4 that is the same 32 bytes the box form would take
      if(count <= 4 && hasPressure) {
        // Not seen in a sample yet.  The rule above - raw floats while they are no larger than the
        //  compressed form - predicts f32 x/y/pressure triples, as 12n <= 24 + 6n exactly up to 4 points.
        //  Each candidate must end where a record (or the blob) does and hold plausible values.
        size_t found = parseShortPressure(blob, pos, count, &stroke);
        if(!found)
          return false;
        pos += found;
      }
      else if(count <= 4) {
        if(blob.size - pos < 8*count)
          return false;
        for(uint64_t ii = 0; ii < count; ++ii)
          stroke.points.push_back(Point{f32(blob.data + pos + 8*ii), f32(blob.data + pos + 8*ii + 4)});
        pos += 8*count;
      }
      else {
        // bounding box (x, width, y, height), then u16 x/y pairs normalized to it - or for a pressure
        //  pen two f32 (the pressure range) and u16 x/y/pressure triples
        size_t stride = hasPressure ? 6 : 4;
        size_t header = hasPressure ? 24 : 16;
        if(blob.size - pos < header + stride*count)
          return false;
        const unsigned char* box = blob.data + pos;
        double boxX = f32(box), boxW = f32(box + 4), boxY = f32(box + 8), boxH = f32(box + 12);
        double rangeTop = hasPressure ? f32(box + 16) : 0, rangeBottom = hasPressure ? f32(box + 20) : 0;
        const unsigned char* raw = box + header;
        for(uint64_t ii = 0; ii < count; ++ii) {
          const unsigned char* sample = raw + stride*ii;
          stroke.points.push_back(Point{boxX + be16(sample)/65535.0*boxW, boxY + be16(sample + 2)/65535.0*boxH});
          if(hasPressure)
            stroke.pressure.push_back(float(rangeBottom + be16(sample + 4)/65535.0*(rangeTop - rangeBottom)));
        }
        pos += header + stride*count;
      }
      strokes->push_back(std::move(stroke));
    }
    else if(tag == STROKE_COLOR_TAG) {
      if(blob.size - pos < 44 || strokes->empty())
        return false;
      for(int ii = 0; ii < 4; ++ii)
        strokes->back().rgba[ii] = f64(blob.data + pos + 2 + 8*ii);
      pos += 44;
    }
    else
      return false;
  }
  return true;
}

// type 0x21: a pair of doubles in one value (sizes)
bool pairOfDoubles(const Field* field, double* first, double* second)
{
  if(!field || field->one().size != 16)
    return false;
  *first = f64(field->one().data);
  *second = f64(field->one().data + 8);
  return true;
}

// An ellipse filling the frame, as four cubic arcs
void addEllipse(Shape* shape, double x, double y, double width, double height)
{
  const double KAPPA = 0.5522847498;  // control point distance for a quarter circle
  double rx = width/2, ry = height/2, cx = x + rx, cy = y + ry;
  shape->points = {{cx + rx, cy},
      {cx + rx, cy + KAPPA*ry}, {cx + KAPPA*rx, cy + ry}, {cx, cy + ry},
      {cx - KAPPA*rx, cy + ry}, {cx - rx, cy + KAPPA*ry}, {cx - rx, cy},
      {cx - rx, cy - KAPPA*ry}, {cx - KAPPA*rx, cy - ry}, {cx, cy - ry},
      {cx + KAPPA*rx, cy - ry}, {cx + rx, cy - KAPPA*ry}, {cx + rx, cy}};
  shape->ops = {Shape::MOVE, Shape::CUBIC, Shape::CUBIC, Shape::CUBIC, Shape::CUBIC, Shape::CLOSE};
}

// A regular polygon inscribed in the frame's ellipse, first vertex at the top.  Where the first vertex
//  goes is an assumption, checked on one sample: a triangle rotated a quarter turn, drawn as a logic
//  gate, only points the right way with it at the top.
void addRegularPolygon(Shape* shape, int sides, double x, double y, double width, double height)
{
  for(int ii = 0; ii < sides; ++ii) {
    double angle = -PI/2 + 2*PI*ii/sides;
    shape->points.push_back(Point{x + width/2*(1 + cos(angle)), y + height/2*(1 + sin(angle))});
    shape->ops.push_back(ii == 0 ? Shape::MOVE : Shape::LINE);
  }
  shape->ops.push_back(Shape::CLOSE);
}

// Element: 1 id, 2 {1: frame centre x, y, w, h, rotation (radians, about that centre)}, 4 layer, 6 body
//  with 1 kind, 9 u16 1 = drawn with the highlighter.  Body by what it carries: 13 path (line, curve,
//  polygon), 4 rich text (text box), 10 asset (image); an ellipse or regular polygon (20 = side count)
//  carries only its style.
void parseElement(const Record& element, Page* page, std::vector<std::string>* warnings)
{
  std::vector<double> frame = element.sub(2).doubles(1);
  frame.resize(std::max(frame.size(), size_t(5)), 0.0);
  // x, y is the frame's centre, not its corner: read as a corner, every element sat w/2, h/2 too far
  //  right and down - highlighters (height 0) visibly to the right of their word, text boxes and images
  //  past the page's edge.  From here on the frame is the usual top-left x, y, w, h.
  frame[0] -= frame[2]/2;
  frame[1] -= frame[3]/2;
  uint32_t layer = element.u32(4);
  Record body = element.sub(6);
  int kind = int(body.i64(1));
  if(body.get(13) || kind == Shape::KIND_ELLIPSE || kind == Shape::KIND_REGULAR_POLYGON) {
    Shape shape;
    shape.layer = layer;
    shape.kind = kind;
    shape.highlighter = element.u16(9) == 1;
    Record style = body.sub(7);
    std::vector<double> rgba = style.sub(7).doubles(0);
    for(size_t ii = 0; ii < 4 && ii < rgba.size(); ++ii)
      shape.rgba[ii] = rgba[ii];
    shape.width = style.f64(2, 1);
    if(kind == Shape::KIND_ELLIPSE && !body.get(13))
      addEllipse(&shape, frame[0], frame[1], frame[2], frame[3]);
    else if(kind == Shape::KIND_REGULAR_POLYGON && !body.get(13)) {
      int sides = int(body.f64(20, 0));
      if(sides < 3 || sides > 1000) {
        warnings->push_back("page " + page->id + ": polygon with " + std::to_string(sides) + " sides skipped");
        return;
      }
      addRegularPolygon(&shape, sides, frame[0], frame[1], frame[2], frame[3]);
    }
    else {
      Record path = body.sub(13);
      std::vector<double> coords = path.doubles(1);
      for(size_t ii = 0; ii + 1 < coords.size(); ii += 2)
        shape.points.push_back(Point{frame[0] + coords[ii], frame[1] + coords[ii + 1]});
      if(const Field* ops = path.get(2)) {
        for(const Bytes& op : ops->values)
          shape.ops.push_back(op.size == 4 ? int(be32(op.data)) : -1);
      }
    }
    // every op must be known and the points must add up, or the path would be drawn wrong
    static const int opPoints[] = {1, 1, 2, 3, 0};
    size_t needed = 0;
    bool valid = !shape.ops.empty() && shape.ops.front() == Shape::MOVE;
    for(int op : shape.ops) {
      if(op < 0 || op > Shape::CLOSE) { valid = false;  break; }
      needed += opPoints[op];
    }
    if(!valid || needed != shape.points.size()) {
      warnings->push_back("page " + page->id + ": malformed shape skipped");
      return;
    }
    double rotation = frame[4];
    if(rotation != 0) {
      double cx = frame[0] + frame[2]/2, cy = frame[1] + frame[3]/2;
      double cosr = cos(rotation), sinr = sin(rotation);
      for(Point& point : shape.points) {
        double dx = point.x - cx, dy = point.y - cy;
        point = Point{cx + dx*cosr - dy*sinr, cy + dx*sinr + dy*cosr};
      }
    }
    page->shapes.push_back(std::move(shape));
  }
  else if(body.get(4)) {
    TextBox text;
    text.layer = layer;
    text.x = frame[0];  text.y = frame[1];  text.width = frame[2];  text.height = frame[3];
    for(const std::string& run : body.sub(4).strings(2))
      text.text += run;
    text.tags = body.strings(14);
    page->texts.push_back(std::move(text));
  }
  else if(body.get(10)) {
    ImageItem image;
    image.layer = layer;
    image.x = frame[0];  image.y = frame[1];  image.width = frame[2];  image.height = frame[3];
    image.assetId = body.text(10);
    pairOfDoubles(body.get(2), &image.displayW, &image.displayH);
    std::vector<double> crop = body.sub(12).doubles(1);
    if(crop.size() >= 2) {
      image.cropX0 = image.cropX1 = crop[0];
      image.cropY0 = image.cropY1 = crop[1];
      for(size_t ii = 0; ii + 1 < crop.size(); ii += 2) {
        image.cropX0 = std::min(image.cropX0, crop[ii]);  image.cropX1 = std::max(image.cropX1, crop[ii]);
        image.cropY0 = std::min(image.cropY0, crop[ii + 1]);  image.cropY1 = std::max(image.cropY1, crop[ii + 1]);
      }
    }
    else {
      image.cropX1 = image.displayW;  image.cropY1 = image.displayH;
    }
    image.rotation = frame[4];
    if(!image.assetId.empty() && image.cropX1 > image.cropX0 && image.cropY1 > image.cropY0)
      page->images.push_back(std::move(image));
  }
  else
    warnings->push_back("page " + page->id + ": unknown element kind " + std::to_string(kind) + " skipped");
}

}  // namespace

double Page::contentBottom() const
{
  double bottom = 0;
  for(const Stroke& stroke : strokes) {
    for(const Point& point : stroke.points)
      bottom = std::max(bottom, point.y);
  }
  for(const Shape& shape : shapes) {
    for(const Point& point : shape.points)
      bottom = std::max(bottom, point.y);
  }
  for(const TextBox& text : texts)
    bottom = std::max(bottom, text.y + text.height);
  for(const ImageItem& image : images)
    bottom = std::max(bottom, image.y + image.height);
  return bottom;
}

std::string Notebook::asset(const std::string& id) const
{
  for(const auto& object : objects) {
    if(object.first == id)
      return data.substr(object.second.offset, object.second.length);
  }
  return std::string();
}

bool parse(std::string data, Notebook* out, std::string* error)
{
  Notebook& notebook = *out;
  notebook = Notebook();
  notebook.data = std::move(data);
  Bytes file((const unsigned char*)notebook.data.data(), notebook.data.size());
  auto fail = [error](const char* message) { if(error) *error = message;  return false; };

  if(file.size < 20 || memcmp(file.data, MAGIC, 4) != 0 || memcmp(file.data + file.size - 16, MAGIC, 4) != 0)
    return fail("This is not a Noteful notebook.");
  uint64_t indexOffset = be64(file.data + file.size - 12);
  uint32_t indexLength = be32(file.data + file.size - 4);
  if(indexOffset > file.size - 16 || indexLength > file.size - 16 - indexOffset)
    return fail("The notebook's index is damaged.");
  Record index;
  if(!index.parse(file.sub(indexOffset, indexLength)))
    return fail("The notebook's index is damaged.");

  // object table: 10 ids, 11 offsets, 12 lengths
  const Field* ids = index.get(10);
  const Field* offsets = index.get(11);
  const Field* lengths = index.get(12);
  if(!ids || !offsets || !lengths)
    return fail("The notebook's index is damaged.");
  std::map<std::string, Bytes> objects;
  for(size_t ii = 0; ii < ids->values.size() && ii < offsets->values.size() && ii < lengths->values.size(); ++ii) {
    if(offsets->values[ii].size != 8 || lengths->values[ii].size != 8)
      continue;
    uint64_t offset = be64(offsets->values[ii].data), length = be64(lengths->values[ii].data);
    if(offset > file.size || length > file.size - offset)
      continue;
    std::string id = ids->values[ii].str();
    objects[id] = file.sub(offset, length);
    notebook.objects.push_back({id, Notebook::Span{size_t(offset), size_t(length)}});
  }

  std::string notebookId = index.get(2) ? index.get(2)->one().str() : std::string();
  Record header, doc;
  if(!objects.count("n:" + notebookId) || !objects.count("d:" + notebookId)
      || !header.parse(objects["n:" + notebookId]) || !doc.parse(objects["d:" + notebookId]))
    return fail("The notebook's header is damaged.");
  notebook.title = header.text(3);

  // layers: 1 name, 2 u32 id, 3 order key, 6 f32 opacity
  std::vector<std::pair<std::string, Layer>> layers;
  for(const Record& record : collection(doc, 3)) {
    Layer layer;
    layer.name = record.text(1);
    layer.id = record.u32(2);
    std::vector<double> opacity = record.doubles(6);
    layer.opacity = opacity.empty() ? 1 : float(opacity[0]);
    layers.push_back({record.text(3), layer});
  }
  std::stable_sort(layers.begin(), layers.end(),
      [](const std::pair<std::string, Layer>& a, const std::pair<std::string, Layer>& b) { return a.first < b.first; });
  for(auto& layer : layers)
    notebook.layers.push_back(layer.second);

  // pages: 1 id, 2 ink reference {0 ink record id, 3 tags}, 4 template, 5 order key
  std::vector<Record> pageRecords = collection(doc, 2);
  std::stable_sort(pageRecords.begin(), pageRecords.end(),
      [](const Record& a, const Record& b) { return a.text(5) < b.text(5); });
  for(const Record& record : pageRecords) {
    Page page;
    page.id = record.text(1);
    Record inkRef = record.sub(2);
    page.tags = inkRef.strings(3);
    Record tmpl = record.sub(4);
    if(!pairOfDoubles(tmpl.get(1), &page.width, &page.height) || page.width <= 0 || page.height <= 0) {
      page.width = 1091.3385826771655;  // A4 - the only size seen
      page.height = 1543.464566929134;
      notebook.warnings.push_back("page " + page.id + ": no page size, assuming A4");
    }
    const Field* field3 = tmpl.get(3);
    if(field3 && field3->type == 5) {
      // paper: 3 asset (the paper rendered as a PDF), 4 sha1, 5 class, 6 JSON parameters
      page.assetId = tmpl.text(3);
      page.templateClass = tmpl.text(5);
      std::string json = tmpl.text(6);
      jsonValue(json, "name", &page.templateName);
      double number;
      if(jsonNumber(json, "lh", &number) && number > 0) {
        page.isPaper = true;
        page.lineHeight = number;
        page.lineWidth = jsonNumber(json, "lw", &number) ? number : 0.5;
        page.lineType = jsonNumber(json, "lt", &number) ? int(number) : 0;
      }
      if(jsonNumber(json, "pc", &number)) {
        page.hasPaperColor = true;
        page.paperColor = uint32_t(number) & 0xFFFFFF;
      }
    }
    else {
      // imported PDF page or image: 3 page number within the asset, 4 asset, and for a cropped page
      //  9 where the whole asset page goes (0x23: x, y, w, h)
      page.assetPage = int(tmpl.i64(3));
      page.assetId = tmpl.text(4);
      const Field* rect = tmpl.get(9);
      if(rect && rect->type == 0x23 && rect->one().size == 32) {
        for(int ii = 0; ii < 4; ++ii)
          page.assetRect[ii] = f64(rect->one().data + 8*ii);
        page.hasAssetRect = page.assetRect[2] > 0 && page.assetRect[3] > 0;
      }
    }

    Record ink;
    auto inkObject = objects.find(inkRef.text(0));
    if(inkObject != objects.end() && ink.parse(inkObject->second)) {
      if(const Field* blob = ink.get(2)) {
        if(!parseStrokes(blob->one(), &page.strokes))
          notebook.warnings.push_back("page " + page.id + ": unknown ink record, remaining strokes skipped");
      }
      for(const Record& element : collection(ink, 5))
        parseElement(element, &page, &notebook.warnings);
    }

    for(const std::string& tag : page.tags)
      addUnique(&notebook.tags, tag);
    for(const TextBox& text : page.texts) {
      for(const std::string& tag : text.tags) {
        addUnique(&notebook.tags, tag);
        addUnique(&page.tags, tag);
      }
    }
    notebook.pages.push_back(std::move(page));
  }

  // outline: 1 id, 2 page id, 3 parent id (empty = top level), 4 title, 5 order key among siblings.
  //  Flattened depth first, siblings by order key.
  struct Entry { std::string id, pageId, parentId, title, order; };
  std::vector<Entry> entries;
  for(const Record& record : collection(doc, 5))
    entries.push_back(Entry{record.text(1), record.text(2), record.text(3), record.text(4), record.text(5)});
  std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.order < b.order; });
  std::map<std::string, bool> known;
  for(const Entry& entry : entries)
    known[entry.id] = true;
  // entries whose parent is missing count as top level, so nothing is lost
  std::vector<std::pair<const Entry*, int>> stack;
  for(auto it = entries.rbegin(); it != entries.rend(); ++it) {
    if(it->parentId.empty() || !known.count(it->parentId))
      stack.push_back({&*it, 0});
  }
  std::map<std::string, bool> visited;
  while(!stack.empty()) {
    const Entry* entry = stack.back().first;
    int level = stack.back().second;
    stack.pop_back();
    if(visited[entry->id])
      continue;  // a cycle in a corrupt file
    visited[entry->id] = true;
    notebook.outline.push_back(OutlineEntry{entry->pageId, entry->title, level});
    for(auto it = entries.rbegin(); it != entries.rend(); ++it) {
      if(it->parentId == entry->id)
        stack.push_back({&*it, level + 1});
    }
  }
  return true;
}

bool load(const char* filename, Notebook* out, std::string* error)
{
  FILE* file = fopen(filename, "rb");
  if(!file) {
    if(error) *error = "The notebook could not be opened.";
    return false;
  }
  std::string data;
  char buffer[65536];
  size_t got;
  while((got = fread(buffer, 1, sizeof(buffer), file)) > 0)
    data.append(buffer, got);
  fclose(file);
  return parse(std::move(data), out, error);
}

}  // namespace Noteful
