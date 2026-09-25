#include "strokefile.h"

#include <fstream>
#include <sstream>

bool readStrokeFile(const std::string& path, std::vector<TestStroke>& out, std::string& error)
{
  std::ifstream file(path);
  if(!file) {
    error = "cannot open " + path;
    return false;
  }
  std::string line;
  TestStroke current;
  bool inStroke = false;
  int lineNum = 0;
  while(std::getline(file, line)) {
    ++lineNum;
    std::istringstream words(line);
    std::string keyword;
    if(!(words >> keyword) || keyword[0] == '#')
      continue;
    if(keyword == "stroke") {
      current = TestStroke();
      words >> current.name;
      current.name = path + ":" + (current.name.empty() ? std::to_string(lineNum) : current.name);
      inStroke = true;
    }
    else if(!inStroke) {
      error = path + ":" + std::to_string(lineNum) + ": '" + keyword + "' outside a stroke";
      return false;
    }
    else if(keyword == "label")
      words >> current.label;
    else if(keyword == "truth") {
      double value;
      while(words >> value)
        current.truth.push_back(value);
    }
    else if(keyword == "points") {
      double xcoord, ycoord;
      while(words >> xcoord >> ycoord)
        current.points.emplace_back(xcoord, ycoord);
    }
    else if(keyword == "end") {
      out.push_back(current);
      inStroke = false;
    }
  }
  return true;
}

bool writeStrokeFile(const std::string& path, const std::vector<TestStroke>& strokes)
{
  std::ofstream file(path);
  if(!file)
    return false;
  file << "# shape-recognition strokes v1\n";
  for(const TestStroke& stroke : strokes) {
    file << "stroke " << stroke.name << "\nlabel " << stroke.label << "\n";
    if(!stroke.truth.empty()) {
      file << "truth";
      for(double value : stroke.truth)
        file << " " << value;
      file << "\n";
    }
    file << "points";
    for(const Vec2& pt : stroke.points)
      file << " " << pt.x << " " << pt.y;
    file << "\nend\n";
  }
  return bool(file);
}
