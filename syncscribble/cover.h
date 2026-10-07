#pragma once

#include <string>
#include <vector>
#include "ulib/color.h"

class Image;
class SvgNode;
struct Rect;

// A notebook cover: one color the user picks (the seed), and a band across it generated from that
//  color.  The band is the seed moved in OKLab lightness towards the middle - darker on a light cover,
//  lighter on a dark one - keeping the hue and as much of the chroma as the new lightness can hold,
//  so it always reads as "the same color, a shade off" rather than as a second color.  Black gets a
//  gray band, white a light gray one.
//
// Stored per document as the config value "coverColor" (ARGB; 0 means the document has no cover, and
//  the document list shows its thumbnail instead).  Only the seed is stored - the band is derived,
//  so a document never carries a cover that disagrees with itself.
namespace Cover {

// Every preview in the document list - a cover, a notebook's thumbnail, a tagged page's card - is drawn
//  in the same frame: rounded corners and a faint outline (class preview-outline, --preview-outline in
//  theme.cpp).  A notebook's frame is always PREVIEW_ASPECT tall, so the grid lines up whatever its pages
//  are; a page card keeps its page's own shape inside that box.
static constexpr real PREVIEW_ASPECT = real(1.41421356);  // height / width - A-series paper

// corner radius of a w-wide preview: a full-size cell gets PREVIEW_RADIUS, a small swatch proportionally less
real previewRadius(real w);

// the frame's outline as an SVG fragment, w x h at the origin - drawn over the preview
std::string outlineSVG(real w, real h);

// makes the corners outside a radius-px rounded rectangle transparent (antialiased), so a thumbnail
//  bitmap has the frame's shape; usvg can only clip to rectangles
void roundCorners(Image* image, real radius);

// a thumbnail in the frame: corners rounded, outline on top, drawn at `frame` (its own size: a page keeps
//  its proportions); cropToFrame first cuts it from its top left to PREVIEW_ASPECT, for a document
//  thumbnail of the older 240 x 400 kind.  Shared by the document list and the sidebar's Pages view.
SvgNode* framedImage(Image thumbnail, const Rect& frame, bool cropToFrame);

Color bandColor(Color seed);

// the cover as an SVG fragment: one <g> at the origin, w x h
std::string coverSVG(Color seed, real w, real h);

// the seeds offered in the new document dialog, a spread of muted book-cloth colors plus neutrals
const std::vector<Color>& presetSeeds();

}  // namespace Cover
