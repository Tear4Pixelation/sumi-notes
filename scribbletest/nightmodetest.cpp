// Night mode image handling (docs/agent/night-mode.md): which images are flipped, and how.
// Build and run from the repo root:
//   g++ -std=c++14 -O2 -I . -I syncscribble -I nanovgXC/src scribbletest/nightmodetest.cpp \
//       syncscribble/nightmode.cpp ulib/oklab.cpp ulib/palettegen.cpp -o /tmp/nightmodetest && /tmp/nightmodetest

#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include "nightmode.h"

static int failures = 0;
#define CHECK(cond, ...) do { if(!(cond)) { ++failures; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while(0)

struct Rgba { unsigned char r, g, b, a; };

static std::vector<unsigned char> makeImage(int w, int h, Rgba (*pixelAt)(int x, int y))
{
  std::vector<unsigned char> pixels(size_t(w)*h*4);
  for(int y = 0; y < h; ++y) {
    for(int x = 0; x < w; ++x) {
      Rgba px = pixelAt(x, y);
      unsigned char* out = &pixels[4*(size_t(y)*w + x)];
      out[0] = px.r; out[1] = px.g; out[2] = px.b; out[3] = px.a;
    }
  }
  return pixels;
}

// white page with black text lines and a blue heading - a worksheet
static Rgba worksheet(int x, int y)
{
  if(y < 20) return {40, 60, 200, 255};
  if(y % 12 < 2 && x > 10 && x < 190) return {0, 0, 0, 255};
  return {255, 255, 255, 255};
}
// saturated, mid-lightness gradient - a photo
static Rgba photo(int x, int y) { return {(unsigned char)(x % 256), (unsigned char)(90 + y % 100), 60, 255}; }
// dark ink on a keyed-out background - what a clip will look like
static Rgba clip(int x, int y) { return y % 10 < 2 ? Rgba{0, 0, 0, 255} : Rgba{0, 0, 0, 0}; }
// light but saturated everywhere - a pastel illustration; not paper
static Rgba pastel(int x, int y) { return {255, 200, 120, 255}; }

int main()
{
  NightColorMap map;
  map.setPalette(NULL);
  const int w = 200, h = 300;

  auto page = makeImage(w, h, worksheet);
  CHECK(NightColorMap::isDocumentLike(page.data(), w, h), "worksheet not detected as a document");
  auto pic = makeImage(w, h, photo);
  CHECK(!NightColorMap::isDocumentLike(pic.data(), w, h), "photo detected as a document");
  auto ink = makeImage(w, h, clip);
  CHECK(NightColorMap::isDocumentLike(ink.data(), w, h), "transparent clip not detected as a document");
  auto warm = makeImage(w, h, pastel);
  CHECK(!NightColorMap::isDocumentLike(warm.data(), w, h), "saturated pastel detected as paper");

  // a declined image must be left byte for byte as it was - the painter uploads it as is
  auto picBefore = pic;
  CHECK(!map.mapImagePixels(pic.data(), w, h) && pic == picBefore, "photo was modified");

  CHECK(map.mapImagePixels(page.data(), w, h), "worksheet not mapped");
  auto at = [&](int x, int y) { return &page[4*(size_t(y)*w + x)]; };
  // paper must land where flipLightness() puts white, so a PDF page matches ink-drawn paper around it
  Color paper = map.flipLightness(Color(255, 255, 255));
  const unsigned char* bg = at(5, 50);
  CHECK(std::abs(bg[0] - paper.red()) <= 1 && bg[0] < 80, "paper -> %d, expected %d", bg[0], paper.red());
  const unsigned char* text = at(50, 48);
  CHECK(text[0] > 200 && text[1] > 200 && text[2] > 200, "black text -> %d %d %d", text[0], text[1], text[2]);
  // hue kept: the blue heading must stay blue (XOR would have made it yellow)
  const unsigned char* heading = at(50, 5);
  CHECK(heading[2] > heading[0] + 60 && heading[2] > heading[1] + 60, "blue heading -> %d %d %d",
      heading[0], heading[1], heading[2]);
  CHECK(heading[3] == 255, "alpha changed");

  // a transparent pixel keeps its alpha; the dark ink on it flips light
  map.mapImagePixels(ink.data(), w, h);
  CHECK(ink[4*(size_t(5)*w + 5) + 3] == 0, "transparent pixel gained alpha");
  CHECK(ink[0] > 200, "clip ink -> %d", ink[0]);

  printf(failures ? "%d FAILED\n" : "all passed\n", failures);
  return failures ? 1 : 0;
}
