#pragma once

#include <string>
#include <vector>
#include "ulib/color.h"

// A notebook cover: one color the user picks (the seed), and a band across it generated from that
//  color.  The band is the seed moved in OKLab lightness towards the middle - darker on a light cover,
//  lighter on a dark one - keeping the hue and as much of the chroma as the new lightness can hold,
//  so it always reads as "the same color, a shade off" rather than as a second color.  Black gets a
//  gray band, white a light gray one.
//
// Stored per document as the config value "coverColor" (ARGB; 0 means the document has no cover, and
//  the document list shows its first page instead).  Only the seed is stored - the band is derived,
//  so a document never carries a cover that disagrees with itself.
namespace Cover {

Color bandColor(Color seed);

// the cover as an SVG fragment: one <g> at the origin, w x h
std::string coverSVG(Color seed, real w, real h);

// the seeds offered in the new document dialog, a spread of muted book-cloth colors plus neutrals
const std::vector<Color>& presetSeeds();

}  // namespace Cover
