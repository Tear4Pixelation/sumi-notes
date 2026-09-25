// Line-oriented front end to the recognizer, kept running by playground.py so a stroke is recognized
//  the moment the pen lifts, with no process start per stroke.
//
// Each input line is one stroke, "x y x y ..."; each output line is the result as JSON.

#include "geom.h"
#include "shaperec.h"

#include <cmath>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>

namespace {

std::string jsonString(const std::string& text)
{
  std::string out = "\"";
  for(char ch : text) {
    if(ch == '"' || ch == '\\') { out += '\\'; out += ch; }
    else if(ch == '\n') out += "\\n";
    else if((unsigned char)ch < 0x20) out += ' ';
    else out += ch;
  }
  return out + "\"";
}

// JSON has no infinity, so a candidate that could not be fitted is reported as null
std::string jsonNumber(double value)
{
  if(!std::isfinite(value))
    return "null";
  char buf[32];
  snprintf(buf, sizeof(buf), "%.3f", value);
  return buf;
}

std::string jsonPoints(const std::vector<Vec2>& pts)
{
  std::string out = "[";
  for(size_t i = 0; i < pts.size(); ++i)
    out += (i ? "," : "") + std::string("[") + jsonNumber(pts[i].x) + "," + jsonNumber(pts[i].y) + "]";
  return out + "]";
}

}  // namespace

int main()
{
  std::ios::sync_with_stdio(false);
  std::string line;
  while(std::getline(std::cin, line)) {
    std::istringstream words(line);
    std::vector<Vec2> stroke;
    double xcoord, ycoord;
    while(words >> xcoord >> ycoord)
      stroke.emplace_back(xcoord, ycoord);
    shaperec::Result res = shaperec::recognize(stroke);
    std::cout << "{\"kind\":" << jsonString(shaperec::kindName(res.kind))
        << ",\"points\":" << jsonPoints(res.points)
        << ",\"rawCorners\":" << jsonPoints(res.rawCorners)
        << ",\"center\":[" << jsonNumber(res.center.x) << "," << jsonNumber(res.center.y) << "]"
        << ",\"radiusX\":" << jsonNumber(res.radiusX) << ",\"radiusY\":" << jsonNumber(res.radiusY)
        << ",\"angle\":" << jsonNumber(res.angle) << ",\"circle\":" << (res.circle ? "true" : "false")
        << ",\"lineErr\":" << jsonNumber(res.lineErr) << ",\"quadErr\":" << jsonNumber(res.quadErr)
        << ",\"ellipseErr\":" << jsonNumber(res.ellipseErr)
        << ",\"reason\":" << jsonString(res.reason) << "}" << std::endl;
  }
  return 0;
}
