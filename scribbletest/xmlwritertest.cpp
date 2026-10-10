// XmlStreamWriter (usvg/svgxml.h) writes XML as it goes instead of building a pugixml DOM and printing
//  it.  Saved documents, sync messages and undo persistence all depend on its output staying byte for
//  byte what the DOM version printed, so this checks it against that version - kept below as
//  DomXmlWriter - on random documents: nesting, attributes (numbers, style= buffering, characters that
//  need escaping), text, and unknown-node fragments of every pugi node type, in both buffered and
//  streamed mode.  No GL context or document needed:
//
//   g++ -std=c++14 -O1 -DPUGIXML_NO_XPATH -DPUGIXML_NO_EXCEPTIONS -I . -I pugixml/src -I stb
//       scribbletest/xmlwritertest.cpp pugixml/src/pugixml.cpp -o xmlwritertest && ./xmlwritertest
//   (one command, run from the repo root.)
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <random>

#define FILEUTIL_IMPLEMENTATION
#define STRINGUTIL_IMPLEMENTATION
#include "usvg/svgxml.h"

void platform_assert(bool cond, const char* expr, const char* func, const char* file, unsigned int line)
{
  if(!cond) { fprintf(stderr, "ASSERT failed: %s (%s:%u)\n", expr, file, line); abort(); }
}

// the pugixml-DOM XmlStreamWriter this replaced: the reference output
class DomXmlWriter
{
  pugi::xml_document doc;
  pugi::xml_node node;

  bool writeStyleAttr = false;
  std::string styleAttr;

  std::vector<char> temp;

public:
  int defaultFloatPrecision = 3;

  DomXmlWriter() : temp(1024) { node = doc; }

  DomXmlWriter& writeStartElement(const char* name) { node = node.append_child(name); return *this; }
  DomXmlWriter& writeEndElement() { node = node.parent();  return *this; }
  DomXmlWriter& writeAttribute(const char* name, const char* value)
  {
    if(writeStyleAttr) {
      styleAttr.append(name); styleAttr.append(":"); styleAttr.append(value); styleAttr.append(";");
    }
    else
      node.append_attribute(name).set_value(value);
    return *this;
  }

  DomXmlWriter& writeAttribute(const char* name, const std::string& value)
  {
    return writeAttribute(name, value.c_str());
  }

  DomXmlWriter& writeAttribute(const char* name, double value, int precision = -1)
  {
    // if value is an int, print as an int
    if(int(value) == value)
      return writeAttribute(name, int(value));

    char* buff = temp.data();
    int ii = realToStr(buff, value, precision < 0 ? defaultFloatPrecision : precision);
    buff[ii] = '\0';
    return writeAttribute(name, buff);
  }

  DomXmlWriter& writeAttribute(const char* name, int value)
  {
    char* buff = temp.data();
    int ii = intToStr(buff, value);
    buff[ii] = '\0';
    return writeAttribute(name, buff);
  }

  DomXmlWriter& writeCharacters(const char* text)
  {
    node.append_child(pugi::node_pcdata).set_value(text);
    return *this;
  }

  DomXmlWriter& writeFragment(const XmlFragment& fragment)
  {
    node.append_copy(fragment.doc.first_child());
    return *this;
  }

  DomXmlWriter& startStyleAttribute() { writeStyleAttr = true; return *this; }
  DomXmlWriter& endStyleAttribute(const char* attribname = "style")
  {
    writeStyleAttr = false;
    if(!styleAttr.empty())
      writeAttribute(attribname, styleAttr.c_str());
    styleAttr.clear();
    return *this;
  }

  char* getTemp(size_t reserve = 0) { if(reserve > temp.size()) { temp.resize(reserve); } return temp.data(); }
  void saveFile(const char* filename, const char* indent = "  ")
  {
    FileStream strm(filename, "wb");
    save(strm, indent);  // let FileStream handle unicode on Windows instead of using pugi save_file wstring
  }

  void save(std::ostream& strm, const char* indent = "  ")
  {
    doc.save(strm, indent, pugi::format_default | pugi::format_no_declaration);
  }

  void save(IOStream& strm, const char* indent = "  ")
  {
    PugiXMLWriter writer(strm);
    doc.save(writer, indent, pugi::format_default | pugi::format_no_declaration);
  }
};


static std::mt19937 rng(12345);
static int rnd(int n) { return int(rng() % unsigned(n)); }

static std::string randText()
{
  static const char* pieces[] = {"abc", " ", "&", "<", ">", "\"", "'", "\t", "\n", "\r", "\x01", "\x1f", "é", "1.5",
      "]]>", "x y", "url(#a)"};
  std::string text;
  int len = rnd(6);
  for(int i = 0; i < len; ++i) text += pieces[rnd(17)];
  return text;
}

static const char* names[] = {"g", "path", "svg", "text", "tspan", "rect", "a", "image"};
static const char* attrNames[] = {"d", "class", "x", "y", "style", "xlink:href", "__timestamp", "fill"};

static std::vector<XmlFragment*> fragments;

struct Op { int kind; std::string a, b; double num; int frag; };

int main()
{
  const char* fragSrc[] = {
    "<unknown a=\"1\"><child b=\"&amp;&lt;\"/>tail text<child2/></unknown>",
    "<empty/>",
    "<withtext>only text &amp; more</withtext>",
    "<!-- a comment -->",
    "<?pi some data?>",
    "<mixed><a/>text<b>t2</b><c/></mixed>",
  };
  for(const char* src : fragSrc) {
    pugi::xml_document doc;
    doc.load_string(src, pugi::parse_default | pugi::parse_comments | pugi::parse_pi);
    fragments.push_back(new XmlFragment(doc.first_child()));
  }
  {
    // pcdata and cdata fragments
    pugi::xml_document doc;
    doc.append_child(pugi::node_pcdata).set_value("frag text & <stuff>");
    fragments.push_back(new XmlFragment(doc.first_child()));
    pugi::xml_document doc2;
    doc2.append_child(pugi::node_cdata).set_value("cdata ]]> inside");
    fragments.push_back(new XmlFragment(doc2.first_child()));
  }

  int failures = 0, cases = 20000;
  for(int trial = 0; trial < cases; ++trial) {
    // random well-formed sequence: attributes only right after a start tag (as all callers do)
    std::vector<Op> ops;
    int depth = 0;
    bool canAttr = false;
    int steps = 1 + rnd(30);
    for(int i = 0; i < steps; ++i) {
      int choice = rnd(10);
      if(choice < 3) { ops.push_back({0, names[rnd(8)], "", 0, 0}); ++depth; canAttr = true; }
      else if(choice < 5 && canAttr) {
        if(rnd(3) == 0) ops.push_back({3, attrNames[rnd(8)], "", rnd(2) ? rnd(1000) - 500 : (rnd(100000) - 50000)/7.0, 0});
        else if(rnd(5) == 0) ops.push_back({6, "", randText(), 0, 0});
        else ops.push_back({2, attrNames[rnd(8)], randText(), 0, 0});
      }
      else if(choice < 7 && depth > 0) { ops.push_back({1, "", "", 0, 0}); --depth; canAttr = false; }
      else if(choice < 8 && depth > 0) { ops.push_back({4, "", randText(), 0, 0}); canAttr = false; }
      else if(choice < 9 && depth > 0) { ops.push_back({5, "", "", 0, rnd(int(fragments.size()))}); canAttr = false; }
    }
    if(rnd(2)) while(depth-- > 0) ops.push_back({1, "", "", 0, 0});

    auto apply = [&](auto& w) {
      for(const Op& op : ops) {
        switch(op.kind) {
        case 0: w.writeStartElement(op.a.c_str()); break;
        case 1: w.writeEndElement(); break;
        case 2: w.writeAttribute(op.a.c_str(), op.b.c_str()); break;
        case 3: w.writeAttribute(op.a.c_str(), op.num); break;
        case 4: w.writeCharacters(op.b.c_str()); break;
        case 5: w.writeFragment(*fragments[op.frag]); break;
        case 6: w.startStyleAttribute(); w.writeAttribute("fill", op.b.c_str()); w.writeAttribute("stroke", "none"); w.endStyleAttribute(); break;
        }
      }
    };
    DomXmlWriter oldw;
    apply(oldw);
    MemStream oldout;
    oldw.save(oldout);
    // buffered mode
    XmlStreamWriter neww;
    apply(neww);
    MemStream newout;
    neww.save(newout);
    // streamed mode
    MemStream streamout;
    XmlStreamWriter streamw(streamout);
    apply(streamw);
    streamw.save(streamout);
    std::string a(oldout.data() ? oldout.data() : "", oldout.size()), b(newout.data() ? newout.data() : "", newout.size()),
        c(streamout.data() ? streamout.data() : "", streamout.size());
    if(a != b || a != c) {
      if(failures++ < 3)
        printf("MISMATCH trial %d\n--- old ---\n%s\n--- new ---\n%s\n--- streamed ---\n%s\n", trial, a.c_str(), b.c_str(), c.c_str());
    }
  }
  printf("%s: %d mismatches in %d random documents\n", failures ? "FAIL" : "PASS", failures, cases);
  return failures != 0;
}
