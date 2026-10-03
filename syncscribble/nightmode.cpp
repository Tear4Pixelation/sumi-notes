#include "nightmode.h"
#include "ulib/oklab.h"

// Lightness range the flip maps onto.  Without a theme, white paper lands on a dark gray rather than pure
//  black (easier on the eyes); a themed document uses its mirrored paper instead.  Black ink lands just
//  short of white.
static constexpr real NIGHT_PAPER_L = 0.18;
static constexpr real NIGHT_INK_L = 0.94;
// bound the cache in case something feeds a stream of distinct colors (a gradient-heavy import)
static constexpr size_t NIGHT_CACHE_MAX = 4096;

// Image detection.  A worksheet, PDF page or enhanced scan is mostly light, unsaturated paper; a photo is
//  not.  A mostly transparent image (ink on a keyed-out background, as a clip will be) is ink, so flips.
static constexpr int DETECT_GRID = 64;           // samples per axis
static constexpr int PAPER_MIN_LUMA = 190;
static constexpr int PAPER_MAX_SPREAD = 48;      // max - min channel: "unsaturated"
static constexpr int TRANSPARENT_MAX_ALPHA = 16;
static constexpr real DOCUMENT_MIN_FRACTION = 0.5;

void NightColorMap::setPalette(const Palette* pal)
{
  bool nowThemed = pal && !pal->families.empty();
  if(valid && nowThemed == themed && (!themed || pal->recipe == recipe))
    return;
  valid = true;
  themed = nowThemed;
  ++generation;
  cache.clear();
  lastValid = false;
  lumaLutValid = false;
  paperL = NIGHT_PAPER_L;
  if(!themed)
    return;
  recipe = pal->recipe;
  light = *pal;
  PaletteRecipe mirrored = recipe;
  mirrored.paperL = 1 - mirrored.paperL;
  generatePalette(mirrored, &dark);
  paperL = oklchFromColor(dark.paper).L;
}

Color NightColorMap::flipLightness(Color c) const
{
  ColorOkLch lch = oklchFromColor(c);
  lch.L = paperL + (NIGHT_INK_L - paperL)*(1 - lch.L);
  // reduce chroma to fit rather than letting oklchToColor clip channels, which would shift the hue
  lch.C = std::min(lch.C, oklchMaxChroma(lch.L, lch.h));
  return oklchToColor(lch, c.alpha());
}

Color NightColorMap::mapUncached(Color c) const
{
  if(themed) {
    Color out;
    if(dark.mapFrom(light, c, &out))
      return out;
    // the non-ink slots; alpha is carried across as mapFrom() does
    const Color Palette::* slots[] = { &Palette::paper, &Palette::rule, &Palette::bookmark,
        &Palette::link, &Palette::selection };
    for(auto slot : slots) {
      if((light.*slot).opaque() == c.opaque())
        return Color(dark.*slot).setAlpha(c.alpha());
    }
  }
  return flipLightness(c);
}

Color NightColorMap::map(Color c) const
{
  if(lastValid && c.color == lastIn)
    return Color(lastOut);
  auto it = cache.find(c.color);
  color_t out;
  if(it != cache.end())
    out = it->second;
  else {
    if(cache.size() >= NIGHT_CACHE_MAX)
      cache.clear();
    out = mapUncached(c).color;
    cache.emplace(c.color, out);
  }
  lastIn = c.color;
  lastOut = out;
  lastValid = true;
  return Color(out);
}

static int lumaOf(int r, int g, int b) { return (54*r + 183*g + 19*b) >> 8; }

bool NightColorMap::isDocumentLike(const unsigned char* rgba, int w, int h)
{
  int total = 0, transparent = 0, paper = 0;
  int stepx = std::max(1, w/DETECT_GRID), stepy = std::max(1, h/DETECT_GRID);
  for(int y = stepy/2; y < h; y += stepy) {
    for(int x = stepx/2; x < w; x += stepx) {
      const unsigned char* px = rgba + 4*(size_t(y)*w + x);
      ++total;
      if(px[3] <= TRANSPARENT_MAX_ALPHA) {
        ++transparent;
        continue;
      }
      int hi = std::max(px[0], std::max(px[1], px[2])), lo = std::min(px[0], std::min(px[1], px[2]));
      if(lumaOf(px[0], px[1], px[2]) >= PAPER_MIN_LUMA && hi - lo <= PAPER_MAX_SPREAD)
        ++paper;
    }
  }
  if(!total)
    return false;
  if(transparent >= DOCUMENT_MIN_FRACTION*total)
    return true;
  return paper >= DOCUMENT_MIN_FRACTION*(total - transparent);
}

// Shift each pixel by (flipped luma - luma): grays land exactly where flipLightness() puts them, colors
//  keep their channel differences and so their hue.  A LUT and integer math - OKLab per pixel would be
//  ~10x slower, and this runs once per image per session on the caching painter.
bool NightColorMap::mapImagePixels(unsigned char* rgba, int w, int h) const
{
  if(!isDocumentLike(rgba, w, h))
    return false;
  if(!lumaLutValid) {
    for(int v = 0; v < 256; ++v)
      lumaLut[v] = flipLightness(Color(v, v, v)).red();
    lumaLutValid = true;
  }
  size_t count = size_t(w)*h;
  for(size_t i = 0; i < count; ++i, rgba += 4) {
    if(!rgba[3])
      continue;
    int luma = lumaOf(rgba[0], rgba[1], rgba[2]);
    int shift = lumaLut[luma] - luma;
    for(int channel = 0; channel < 3; ++channel)
      rgba[channel] = (unsigned char)std::min(255, std::max(0, rgba[channel] + shift));
  }
  return true;
}
