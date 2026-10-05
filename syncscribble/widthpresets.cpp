#include "widthpresets.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

// out-of-line definitions, needed before C++17 since std::min/max take them by reference
constexpr int WidthPresets::COUNT;
constexpr Dim WidthPresets::MIN_WIDTH;
constexpr Dim WidthPresets::MAX_ABSOLUTE;
constexpr Dim WidthPresets::MAX_RELATIVE;

static const char* REL_PREFIX = "rel:";
static const char* ABS_PREFIX = "abs:";

void WidthPresets::parse(const char* str)
{
  widths.clear();
  unit = UNIT_UNKNOWN;
  if(!str)
    return;
  if(strncmp(str, REL_PREFIX, 4) == 0) {
    unit = UNIT_RELATIVE;
    str += 4;
  }
  else if(strncmp(str, ABS_PREFIX, 4) == 0) {
    unit = UNIT_ABSOLUTE;
    str += 4;
  }
  // numbers separated by commas and/or spaces; anything unreadable ends the list
  const char* pos = str;
  while(*pos) {
    while(*pos == ',' || *pos == ' ')
      ++pos;
    if(!*pos)
      break;
    char* end = NULL;
    double value = strtod(pos, &end);
    if(end == pos)
      break;
    widths.push_back(Dim(value));
    pos = end;
  }
}

std::string WidthPresets::serialize() const
{
  std::string out = unit == UNIT_RELATIVE ? REL_PREFIX : (unit == UNIT_ABSOLUTE ? ABS_PREFIX : "");
  char buf[32];
  for(size_t i = 0; i < widths.size(); ++i) {
    snprintf(buf, sizeof(buf), i > 0 ? ",%.6g" : "%.6g", double(widths[i]));
    out += buf;
  }
  return out;
}

void WidthPresets::convertTo(bool relative, Dim lineHeight)
{
  int target = relative ? UNIT_RELATIVE : UNIT_ABSOLUTE;
  if(unit != UNIT_UNKNOWN && unit != target && lineHeight > 0) {
    for(Dim& width : widths)
      width = relative ? width/lineHeight : width*lineHeight;
  }
  unit = target;
}

bool WidthPresets::normalize(const Dim* defaults, bool defaultsRelative, Dim lineHeight)
{
  std::vector<Dim> before = widths;
  bool relative = isRelative();
  Dim maxw = maxWidth(relative);
  // a non-finite or non-positive preset is not a width at all, so it is dropped and refilled; one
  //  that is merely out of range is clamped, keeping the slot and as much of the user's choice as fits
  std::vector<Dim> kept;
  for(Dim width : widths) {
    if(!std::isfinite(width) || width <= 0)
      continue;
    kept.push_back(std::min(maxw, std::max(MIN_WIDTH, width)));
  }
  if(int(kept.size()) > COUNT)
    kept.resize(COUNT);
  if(int(kept.size()) < COUNT) {
    // fill from the defaults at the slots that are missing, so [thin, mid] gets the thick default
    for(int i = int(kept.size()); i < COUNT; ++i) {
      Dim width = defaults[i];
      if(defaultsRelative != relative && lineHeight > 0)
        width = relative ? width/lineHeight : width*lineHeight;
      kept.push_back(std::min(maxw, std::max(MIN_WIDTH, width)));
    }
    std::sort(kept.begin(), kept.end());
  }
  widths = kept;
  return widths != before;
}

Dim WidthPresets::inUnit(size_t idx, bool relative, Dim lineHeight) const
{
  Dim width = widths[idx];
  if(unit == UNIT_UNKNOWN || isRelative() == relative || lineHeight <= 0)
    return width;
  return relative ? width/lineHeight : width*lineHeight;
}

void WidthPresets::setFrom(size_t idx, Dim width, bool relative, Dim lineHeight)
{
  if(unit != UNIT_UNKNOWN && isRelative() != relative && lineHeight > 0)
    width = relative ? width*lineHeight : width/lineHeight;
  else if(unit == UNIT_UNKNOWN)
    unit = relative ? UNIT_RELATIVE : UNIT_ABSOLUTE;
  widths[idx] = std::min(maxWidth(isRelative()), std::max(MIN_WIDTH, width));
}

bool WidthPresets::sameWidth(Dim a, Dim b)
{
  return std::abs(a - b) <= 1e-4*std::max(std::abs(a), std::abs(b));
}
