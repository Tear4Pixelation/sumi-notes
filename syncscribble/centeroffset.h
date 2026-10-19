#pragma once

#include <cmath>
#include <cstdint>

// Marker "Center on Line" offset (docs/agent/pen-and-tools.md, "Marker: centre on line").  Dependency free so
//  scribbletest/markeroffsettest.cpp can build it standalone.
//
// The offset is a number in -1..1 kept on the pen as a signed byte of hundredths in bits 16-23 of
//  ScribblePen::flags: flags is already written to the config as a decimal integer, so the offset round
//  trips exactly (no float text) and a pen saved before this existed reads as 0 (centred).
namespace CenterOffset {

constexpr int SHIFT = 16;
constexpr unsigned int MASK = 0xFFu << SHIFT;
constexpr int STEPS = 100;  // resolution: hundredths

inline double clampOffset(double offset)
{
  return std::isfinite(offset) ? std::fmin(std::fmax(offset, -1.0), 1.0) : 0.0;
}

// offset -> the bits to OR into flags
inline unsigned int pack(double offset)
{
  int steps = int(std::lround(clampOffset(offset)*STEPS));
  return (unsigned int)(steps & 0xFF) << SHIFT;
}

inline double unpack(unsigned int flags)
{
  int steps = int(int8_t((flags >> SHIFT) & 0xFF));
  steps = steps < -STEPS ? -STEPS : (steps > STEPS ? STEPS : steps);
  return double(steps)/STEPS;
}

// y of the stroke for a line whose top ruling is at `top` and whose height is `lineHeight`, for a stroke
//  `width` thick: 0 centres it, +1 puts its top edge on the top ruling, -1 its bottom edge on the bottom
//  ruling, values between interpolate.  A stroke as thick as the line (or thicker) has no room to move.
inline double centerY(double top, double lineHeight, double width, double offset)
{
  double room = std::fmax(lineHeight - width, 0.0)/2;
  return top + lineHeight/2 - clampOffset(offset)*room;
}

}  // namespace CenterOffset
