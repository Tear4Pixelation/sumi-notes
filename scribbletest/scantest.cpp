// Unit tests for the document scanning math: ulib/homography.cpp, ulib/imagewarp.cpp,
//  ulib/imageenhance.cpp and ulib/quaddetect.cpp.
// Unlike the rest of scribbletest, these need no GL context and no document, so they can also be built and
//  run on their own - which is how they get run in CI, where no display is available:
//
//   g++ -std=c++14 -O2 -DNDEBUG -I . -DSCANTEST_MAIN scribbletest/scantest.cpp ulib/geom.cpp
//       ulib/homography.cpp ulib/imagewarp.cpp ulib/imageenhance.cpp ulib/quaddetect.cpp
//       -o scantest -lpthread && ./scantest
//   (one command, run from the repo root.  NDEBUG is needed because geom.cpp's ASSERT would otherwise
//   pull in platform_assert, which lives in the application.)
//
// runScanTests() returns the number of failed checks and is also called from ScribbleTest::runAll().

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <chrono>

// stb is compiled into the app already (application.cpp, ulib/image.cpp); the standalone build brings its own
#ifdef SCANTEST_MAIN
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#endif
#include "stb/stb_image.h"
#include "stb/stb_image_write.h"

#include "ulib/imageenhance.h"
#include "ulib/imagewarp.h"
#include "ulib/quaddetect.h"

static int nChecksFailed = 0;

static void checkTrue(bool condition, const char* what)
{
  if(!condition) {
    ++nChecksFailed;
    printf("FAIL: %s\n", what);
  }
}

static void checkNear(real actual, real expected, real eps, const char* what)
{
  if(!(std::abs(actual - expected) <= eps)) {
    ++nChecksFailed;
    printf("FAIL: %s (got %g, expected %g)\n", what, double(actual), double(expected));
  }
}

static void checkPointNear(Point actual, Point expected, real eps, const char* what)
{
  if(!(std::abs(actual.x - expected.x) <= eps && std::abs(actual.y - expected.y) <= eps)) {
    ++nChecksFailed;
    printf("FAIL: %s (got %g,%g, expected %g,%g)\n", what,
        double(actual.x), double(actual.y), double(expected.x), double(expected.y));
  }
}

// a quad with real perspective in it - opposite edges are not parallel
static const Point PERSPECTIVE_QUAD[4] = { Point(120, 80), Point(540, 140), Point(600, 720), Point(60, 640) };

static void testHomography()
{
  // unitToQuad must land the unit square's corners exactly on the quad's corners
  Transform3D unitToQuad = Transform3D::unitToQuad(PERSPECTIVE_QUAD);
  const Point unitCorners[4] = { Point(0,0), Point(1,0), Point(1,1), Point(0,1) };
  for(int ii = 0; ii < 4; ++ii)
    checkPointNear(unitToQuad.map(unitCorners[ii]), PERSPECTIVE_QUAD[ii], 1e-9, "unitToQuad corner");
  checkTrue(!unitToQuad.isAffine(), "perspective quad should not give an affine transform");

  // inverse round trip
  Transform3D quadToUnit = unitToQuad.inverse();
  for(int ii = 0; ii < 4; ++ii)
    checkPointNear(quadToUnit.map(PERSPECTIVE_QUAD[ii]), unitCorners[ii], 1e-9, "quadToUnit corner");
  checkTrue(approxEq(unitToQuad*quadToUnit, Transform3D(), 1e-9), "T * T^-1 should be identity");

  // a parallelogram is an affine mapping, and must agree with the equivalent Transform2D
  const Point parallelogram[4] = { Point(10, 20), Point(110, 40), Point(130, 140), Point(30, 120) };
  Transform3D affine = Transform3D::unitToQuad(parallelogram);
  checkTrue(affine.isAffine(), "parallelogram should give an affine transform");
  Transform2D equivalent(100, 20, 20, 100, 10, 20);  // columns (1,0)->(100,20), (0,1)->(20,100), origin (10,20)
  checkTrue(approxEq(affine, Transform3D(equivalent), 1e-9), "affine quad should match Transform2D");

  // quadToQuad of a quad onto itself is the identity
  checkTrue(approxEq(Transform3D::quadToQuad(PERSPECTIVE_QUAD, PERSPECTIVE_QUAD), Transform3D(), 1e-9),
      "quadToQuad onto itself should be identity");

  // quadToQuad must map each source corner onto the matching destination corner
  const Point destQuad[4] = { Point(0,0), Point(400,0), Point(400,560), Point(0,560) };
  Transform3D srcToDest = Transform3D::quadToQuad(PERSPECTIVE_QUAD, destQuad);
  for(int ii = 0; ii < 4; ++ii)
    checkPointNear(srcToDest.map(PERSPECTIVE_QUAD[ii]), destQuad[ii], 1e-6, "quadToQuad corner");

  // straight lines must stay straight: the image of a midpoint of an edge is on the image of that edge
  Point srcEdgeMid = (PERSPECTIVE_QUAD[0] + PERSPECTIVE_QUAD[1])*real(0.5);
  Point mappedMid = srcToDest.map(srcEdgeMid);
  checkNear(mappedMid.y, 0, 1e-6, "top edge should map onto the top edge");

  // degenerate input must be reported, not silently produce garbage
  const Point collinear[4] = { Point(0,0), Point(10,10), Point(20,20), Point(30,30) };
  checkTrue(!Transform3D::unitToQuad(collinear).isValid(), "collinear quad should be invalid");
  checkTrue(!isConvexQuad(collinear), "collinear quad should not be convex");
  const Point bowtie[4] = { Point(0,0), Point(100,0), Point(0,100), Point(100,100) };
  checkTrue(!isConvexQuad(bowtie), "self-intersecting quad should not be convex");
  checkTrue(isConvexQuad(PERSPECTIVE_QUAD), "perspective quad should be convex");
}

static std::vector<unsigned int> makeGradient(int width, int height)
{
  std::vector<unsigned int> pixels(size_t(width)*height);
  unsigned char* bytes = (unsigned char*)pixels.data();
  for(int y = 0; y < height; ++y) {
    for(int x = 0; x < width; ++x) {
      unsigned char* pixel = bytes + (size_t(y)*width + x)*4;
      pixel[0] = (unsigned char)(x*255/(width - 1));
      pixel[1] = (unsigned char)(y*255/(height - 1));
      pixel[2] = (unsigned char)((x + y) & 0xFF);
      pixel[3] = 255;
    }
  }
  return pixels;
}

static const unsigned char* pixelAt(const std::vector<unsigned int>& pixels, int width, int x, int y)
{
  return (const unsigned char*)pixels.data() + (size_t(y)*width + x)*4;
}

static void testWarp()
{
  const int srcw = 64, srch = 48;
  std::vector<unsigned int> source = makeGradient(srcw, srch);

  // 1. warping the whole image onto an identical-size destination must reproduce it exactly
  {
    const Point wholeImage[4] =
        { Point(0,0), Point(srcw,0), Point(srcw,srch), Point(0,srch) };
    std::vector<unsigned int> dest(size_t(srcw)*srch, 0u);
    warpQuadRGBA(source.data(), srcw, srch, dest.data(), srcw, srch, wholeImage);
    int nDiff = 0;
    for(size_t ii = 0; ii < source.size(); ++ii)
      nDiff += (source[ii] != dest[ii]) ? 1 : 0;
    checkTrue(nDiff == 0, "identity warp should reproduce the source exactly");
  }

  // 2. a solid-color source must come out solid, whatever the perspective - this is what catches
  //    premultiply and sample-weighting mistakes, which show up as darkening or fringing
  {
    std::vector<unsigned int> solid(size_t(srcw)*srch);
    unsigned char* solidBytes = (unsigned char*)solid.data();
    for(int ii = 0; ii < srcw*srch; ++ii) {
      solidBytes[ii*4 + 0] = 200;  solidBytes[ii*4 + 1] = 100;
      solidBytes[ii*4 + 2] = 50;   solidBytes[ii*4 + 3] = 255;
    }
    // stay well inside the source so that no destination pixel samples off the edge
    const Point insetQuad[4] = { Point(8,6), Point(52,10), Point(56,40), Point(6,38) };
    const int dstw = 40, dsth = 30;
    std::vector<unsigned int> dest(size_t(dstw)*dsth, 0u);
    warpQuadRGBA(solid.data(), srcw, srch, dest.data(), dstw, dsth, insetQuad);
    int nBad = 0;
    for(int y = 0; y < dsth; ++y) {
      for(int x = 0; x < dstw; ++x) {
        const unsigned char* pixel = pixelAt(dest, dstw, x, y);
        if(pixel[0] != 200 || pixel[1] != 100 || pixel[2] != 50 || pixel[3] != 255)
          ++nBad;
      }
    }
    checkTrue(nBad == 0, "perspective warp of a solid color should stay solid");
  }

  // 3. an axis-aligned quad is a plain crop, so the result must match the source pixels one for one
  {
    const Point cropQuad[4] = { Point(10,8), Point(42,8), Point(42,32), Point(10,32) };
    const int dstw = 32, dsth = 24;
    std::vector<unsigned int> dest(size_t(dstw)*dsth, 0u);
    warpQuadRGBA(source.data(), srcw, srch, dest.data(), dstw, dsth, cropQuad);
    int nDiff = 0;
    for(int y = 0; y < dsth; ++y) {
      for(int x = 0; x < dstw; ++x) {
        const unsigned char* got = pixelAt(dest, dstw, x, y);
        const unsigned char* want = pixelAt(source, srcw, x + 10, y + 8);
        for(int channel = 0; channel < 4; ++channel)
          nDiff += (got[channel] != want[channel]) ? 1 : 0;
      }
    }
    checkTrue(nDiff == 0, "axis-aligned unit-scale warp should be an exact crop");
  }

  // 4. round trip: flatten a perspective quad, then push the result back through the inverse mapping and
  //    check it lands back on the original pixels
  {
    const Point quad[4] = { Point(12,9), Point(50,14), Point(54,38), Point(9,35) };
    int dstw = 0, dsth = 0;
    quadOutputSize(quad, 0, &dstw, &dsth);
    checkTrue(dstw > 0 && dsth > 0, "quadOutputSize should give a usable size");
    std::vector<unsigned int> flattened(size_t(dstw)*dsth, 0u);
    warpQuadRGBA(source.data(), srcw, srch, flattened.data(), dstw, dsth, quad, NULL);

    // map back: destination is the full source, source is the flattened image
    const Point flatQuad[4] = { Point(0,0), Point(dstw,0), Point(dstw,dsth), Point(0,dsth) };
    std::vector<unsigned int> restored(size_t(srcw)*srch, 0u);
    warpPerspectiveRGBA(flattened.data(), dstw, dsth, restored.data(), srcw, srch,
        Transform3D::quadToQuad(quad, flatQuad), 1, NULL);

    // only compare well inside the quad; resampling twice blurs the boundary, and the gradient is smooth
    //  so a couple of levels of error is expected
    real worstError = 0;
    for(int y = 14; y < 32; ++y) {
      for(int x = 18; x < 46; ++x) {
        const unsigned char* got = pixelAt(restored, srcw, x, y);
        const unsigned char* want = pixelAt(source, srcw, x, y);
        for(int channel = 0; channel < 3; ++channel)
          worstError = std::max(worstError, real(std::abs(int(got[channel]) - int(want[channel]))));
      }
    }
    checkTrue(worstError <= 6, "round trip through the warp should recover the original");
    if(worstError > 6)
      printf("   worst round trip error was %g\n", double(worstError));
  }

  // 5. destination pixels that sample off the edge of the source must come out transparent, not black
  {
    const Point overhangQuad[4] =
        { Point(-20,-20), Point(srcw + 20,-20), Point(srcw + 20, srch + 20), Point(-20, srch + 20) };
    const int dstw = 40, dsth = 30;
    std::vector<unsigned int> dest(size_t(dstw)*dsth, 0xFFFFFFFFu);
    warpQuadRGBA(source.data(), srcw, srch, dest.data(), dstw, dsth, overhangQuad);
    checkTrue(pixelAt(dest, dstw, 0, 0)[3] == 0, "corner outside the source should be transparent");
    checkTrue(pixelAt(dest, dstw, dstw/2, dsth/2)[3] == 255, "center should still be opaque");
  }
}

static void testOutputSize()
{
  // a 2:1 quad should give a 2:1 output, capped at maxdim
  const Point quad[4] = { Point(0,0), Point(400,0), Point(400,200), Point(0,200) };
  int width = 0, height = 0;
  quadOutputSize(quad, 100, &width, &height);
  checkTrue(width == 100 && height == 50, "quadOutputSize should cap the long edge and keep the ratio");
  // maxdim larger than the quad must not upscale
  quadOutputSize(quad, 4000, &width, &height);
  checkTrue(width == 400 && height == 200, "quadOutputSize should never upscale");
  // degenerate quad
  const Point degenerate[4] = { Point(5,5), Point(5,5), Point(5,5), Point(5,5) };
  quadOutputSize(degenerate, 100, &width, &height);
  checkTrue(width == 0 && height == 0, "degenerate quad should give an empty size");
}

// a page lit unevenly from the left: brightness ramps across the image, which is what flattenIllumination
//  is supposed to remove.  Optionally stamps dark squares on it to stand in for text.
static std::vector<unsigned int> makeShadedPage(int width, int height, bool withText)
{
  std::vector<unsigned int> pixels(size_t(width)*height);
  unsigned char* bytes = (unsigned char*)pixels.data();
  for(int y = 0; y < height; ++y) {
    for(int x = 0; x < width; ++x) {
      // 40% brightness on the left rising to 100% on the right
      float shading = 0.4f + 0.6f*x/(width - 1);
      unsigned char value = (unsigned char)(220*shading);
      unsigned char* pixel = bytes + (size_t(y)*width + x)*4;
      pixel[0] = value;  pixel[1] = value;  pixel[2] = value;  pixel[3] = 255;
    }
  }
  if(withText) {
    // a row of dark blocks across the middle, so they meet the same range of background brightness
    for(int blockX = 20; blockX + 8 < width; blockX += 40) {
      for(int y = height/2; y < height/2 + 8; ++y) {
        for(int x = blockX; x < blockX + 8; ++x) {
          unsigned char* pixel = bytes + (size_t(y)*width + x)*4;
          pixel[0] = pixel[0]/4;  pixel[1] = pixel[1]/4;  pixel[2] = pixel[2]/4;
        }
      }
    }
  }
  return pixels;
}

static void testFlattenIllumination()
{
  const int width = 240, height = 180;
  const int radius = defaultBlurRadius(width, height);

  // 1. a pure shading ramp is entirely illumination, so all of it should divide out to white.  Only the
  //    interior is checked: near the edges the box is clipped and no longer centered on the pixel, so the
  //    background estimate is legitimately biased there.
  {
    std::vector<unsigned int> page = makeShadedPage(width, height, false);
    flattenIllumination(page.data(), width, height, radius, true);
    const unsigned char* bytes = (const unsigned char*)page.data();
    int worstError = 0;
    for(int y = 0; y < height; ++y) {
      for(int x = radius; x < width - radius; ++x) {
        int value = bytes[(size_t(y)*width + x)*4];
        worstError = std::max(worstError, std::abs(value - 255));
      }
    }
    checkTrue(worstError <= 3, "flattening a pure shading ramp should give white");
    if(worstError > 3)
      printf("   worst deviation from white was %d\n", worstError);
  }

  // 2. with text on the page, the background must still flatten to white while the text stays dark -
  //    including the text on the dim left side, which is the whole point of doing this locally
  {
    std::vector<unsigned int> page = makeShadedPage(width, height, true);
    // before: the darkest background on the right is brighter than the text on the left, so no single
    //  global threshold could separate them.  This is the condition the filter has to fix.
    const unsigned char* before = (const unsigned char*)page.data();
    int leftText = before[(size_t(height/2)*width + 22)*4];
    int rightBackground = before[(size_t(10)*width + width - radius - 1)*4];
    checkTrue(leftText < rightBackground, "test image should have overlapping text and background levels");

    flattenIllumination(page.data(), width, height, radius, true);
    const unsigned char* bytes = (const unsigned char*)page.data();
    // background sampled well away from the blocks
    int worstBackground = 0;
    for(int x = radius; x < width - radius; ++x) {
      int value = bytes[(size_t(5)*width + x)*4];
      worstBackground = std::max(worstBackground, std::abs(value - 255));
    }
    checkTrue(worstBackground <= 20, "background should flatten to near white with text present");
    // every block, on the dim side and the bright side alike, must still be clearly dark
    int brightestText = 0;
    for(int blockX = 60; blockX + 8 < width - radius; blockX += 40) {
      for(int x = blockX + 2; x < blockX + 6; ++x)
        brightestText = std::max(brightestText, int(bytes[(size_t(height/2 + 4)*width + x)*4]));
    }
    checkTrue(brightestText < 180, "text should stay dark after flattening");
    if(brightestText >= 180)
      printf("   brightest text pixel was %d\n", brightestText);
  }

  // 3. alpha must be left alone
  {
    std::vector<unsigned int> page = makeShadedPage(width, height, true);
    unsigned char* bytes = (unsigned char*)page.data();
    bytes[3] = 0;  // make one pixel transparent
    flattenIllumination(page.data(), width, height, radius, false);
    checkTrue(bytes[3] == 0, "flattening should not touch alpha");
    checkTrue(bytes[(size_t(5)*width + 100)*4 + 3] == 255, "opaque pixels should stay opaque");
  }
}

static void testLevelsAndGreyscale()
{
  const int width = 64, height = 48;
  // an image whose levels all sit in a narrow band, which is what a flat, low contrast scan looks like
  std::vector<unsigned int> pixels(size_t(width)*height);
  unsigned char* bytes = (unsigned char*)pixels.data();
  for(int ii = 0; ii < width*height; ++ii) {
    unsigned char value = (unsigned char)(100 + (ii % 51));  // 100 .. 150
    bytes[ii*4 + 0] = value;  bytes[ii*4 + 1] = value;  bytes[ii*4 + 2] = value;  bytes[ii*4 + 3] = 255;
  }
  stretchLevels(pixels.data(), width, height, 0.5, 99.5);
  int lowest = 255, highest = 0;
  for(int ii = 0; ii < width*height; ++ii) {
    lowest = std::min(lowest, int(bytes[ii*4]));
    highest = std::max(highest, int(bytes[ii*4]));
  }
  checkTrue(lowest <= 8, "stretchLevels should push the darkest pixels to black");
  checkTrue(highest >= 247, "stretchLevels should push the brightest pixels to white");

  // greyscale must set all three channels to the luma, and leave alpha alone
  std::vector<unsigned int> colored(4);
  unsigned char* colorBytes = (unsigned char*)colored.data();
  for(int ii = 0; ii < 4; ++ii) {
    colorBytes[ii*4 + 0] = 255;  colorBytes[ii*4 + 1] = 0;
    colorBytes[ii*4 + 2] = 0;    colorBytes[ii*4 + 3] = 128;
  }
  toGreyscale(colored.data(), 4, 1);
  checkTrue(colorBytes[0] == colorBytes[1] && colorBytes[1] == colorBytes[2],
      "greyscale should equalize the channels");
  checkTrue(colorBytes[0] > 60 && colorBytes[0] < 90, "pure red should give roughly 0.3 luma");
  checkTrue(colorBytes[3] == 128, "greyscale should not touch alpha");
}

static void testAdaptiveThreshold()
{
  const int width = 240, height = 180;
  std::vector<unsigned int> page = makeShadedPage(width, height, true);
  adaptiveThreshold(page.data(), width, height, std::max(4, width/40), 10);
  const unsigned char* bytes = (const unsigned char*)page.data();
  int nNotBinary = 0;
  for(int ii = 0; ii < width*height; ++ii)
    nNotBinary += (bytes[ii*4] != 0 && bytes[ii*4] != 255) ? 1 : 0;
  checkTrue(nNotBinary == 0, "adaptive threshold should output only black or white");
  // the blocks must come out black and the paper white, on the dim side as well as the bright side
  checkTrue(bytes[(size_t(height/2 + 4)*width + 24)*4] == 0, "text on the dim side should be black");
  checkTrue(bytes[(size_t(height/2 + 4)*width + 184)*4] == 0, "text on the bright side should be black");
  checkTrue(bytes[(size_t(5)*width + 10)*4] == 255, "blank paper on the dim side should be white");
  checkTrue(bytes[(size_t(5)*width + 200)*4] == 255, "blank paper on the bright side should be white");
}

static void testRotate()
{
  const int width = 5, height = 3;
  std::vector<unsigned int> source(size_t(width)*height);
  for(int ii = 0; ii < width*height; ++ii)
    source[ii] = 0xFF000000u | ii;  // each pixel carries its own index

  // a quarter turn swaps the axes and moves the top left corner to the top right
  std::vector<unsigned int> turned(size_t(width)*height);
  rotateQuarterTurns(source.data(), width, height, turned.data(), 1);
  checkTrue(turned[height - 1] == source[0], "90 degrees should move the top left to the top right");

  // four quarter turns, one at a time, must come back to exactly the original
  std::vector<unsigned int> scratch(size_t(width)*height), current = source;
  int curw = width, curh = height;
  for(int turn = 0; turn < 4; ++turn) {
    rotateQuarterTurns(current.data(), curw, curh, scratch.data(), 1);
    current = scratch;
    std::swap(curw, curh);
  }
  checkTrue(curw == width && curh == height, "four quarter turns should restore the dimensions");
  checkTrue(current == source, "four quarter turns should restore the image");

  // 180 must equal two 90s, and -90 must equal three 90s
  std::vector<unsigned int> half(size_t(width)*height), viaTwo(size_t(width)*height);
  rotateQuarterTurns(source.data(), width, height, half.data(), 2);
  rotateQuarterTurns(source.data(), width, height, scratch.data(), 1);
  rotateQuarterTurns(scratch.data(), height, width, viaTwo.data(), 1);
  checkTrue(half == viaTwo, "one 180 should equal two 90s");
  std::vector<unsigned int> negative(size_t(width)*height), viaThree(size_t(width)*height);
  rotateQuarterTurns(source.data(), width, height, negative.data(), -1);
  rotateQuarterTurns(half.data(), width, height, viaThree.data(), 1);
  checkTrue(negative == viaThree, "-90 should equal 270");
}

static void testEnhanceDocument()
{
  const int width = 240, height = 180;
  // every filter must leave the buffer intact and, except for SCAN_ORIGINAL, actually change it
  const ScanFilter filters[4] = { SCAN_ORIGINAL, SCAN_COLOR, SCAN_GREYSCALE, SCAN_MONO };
  for(int ii = 0; ii < 4; ++ii) {
    std::vector<unsigned int> page = makeShadedPage(width, height, true);
    const std::vector<unsigned int> original = page;
    enhanceDocument(page.data(), width, height, filters[ii]);
    if(filters[ii] == SCAN_ORIGINAL)
      checkTrue(page == original, "SCAN_ORIGINAL should leave the image untouched");
    else {
      checkTrue(page != original, "filters should change the image");
      // the background should end up bright whichever filter was used
      const unsigned char* bytes = (const unsigned char*)page.data();
      checkTrue(bytes[(size_t(5)*width + 120)*4] > 200, "background should end up bright");
    }
  }
}

// render a bright page on a dark background, optionally with dark bars on it standing in for text
static std::vector<unsigned int> makePhotoOfPage(int width, int height, const Point quad[4], bool withText)
{
  std::vector<unsigned int> pixels(size_t(width)*height);
  unsigned char* bytes = (unsigned char*)pixels.data();
  for(int y = 0; y < height; ++y) {
    for(int x = 0; x < width; ++x) {
      Point sample(x + 0.5, y + 0.5);
      // TL, TR, BR, BL winds clockwise on screen, and y grows downward, so an interior point is on the
      //  positive side of every edge
      bool inside = true;
      for(int corner = 0; corner < 4 && inside; ++corner) {
        Point edge = quad[(corner + 1) % 4] - quad[corner];
        inside = cross(edge, sample - quad[corner]) >= 0;
      }
      unsigned char value = inside ? 235 : 40;
      unsigned char* pixel = bytes + (size_t(y)*width + x)*4;
      pixel[0] = value;  pixel[1] = value;  pixel[2] = value;  pixel[3] = 255;
    }
  }
  if(withText) {
    // bars spanning the middle of the page: strong horizontal edges that are *inside* the page, so they
    //  will out-vote nothing but would fool a detector that picked the strongest lines instead of the
    //  outermost ones
    Point center(0, 0);
    for(int corner = 0; corner < 4; ++corner)
      center += quad[corner]*0.25;
    for(int bar = -3; bar <= 3; ++bar) {
      int barY = int(center.y + bar*12);
      for(int y = barY; y < barY + 4 && y < height; ++y) {
        if(y < 0) continue;
        for(int x = int(center.x) - 60; x < int(center.x) + 60; ++x) {
          if(x < 0 || x >= width) continue;
          unsigned char* pixel = bytes + (size_t(y)*width + x)*4;
          if(pixel[0] > 200) { pixel[0] = 30;  pixel[1] = 30;  pixel[2] = 30; }
        }
      }
    }
  }
  return pixels;
}

static void checkQuadNear(const Point found[4], const Point expected[4], real tolerance, const char* what)
{
  for(int ii = 0; ii < 4; ++ii) {
    if(!(std::abs(found[ii].x - expected[ii].x) <= tolerance
        && std::abs(found[ii].y - expected[ii].y) <= tolerance)) {
      ++nChecksFailed;
      printf("FAIL: %s - corner %d at %g,%g, expected %g,%g\n", what, ii,
          double(found[ii].x), double(found[ii].y), double(expected[ii].x), double(expected[ii].y));
      return;
    }
  }
}

static void testQuadDetect()
{
  const int width = 600, height = 450;
  // Detection runs on a downscaled copy, so some slop is expected - but the least squares refit in
  //  quaddetect should keep it well under a downscaled pixel's worth.  Kept tight deliberately: a loose
  //  bound here would let an angular regression through, and a small angular error at the middle of an
  //  edge becomes a large corner error at its ends.
  const real tolerance = 4;

  // 1. an axis-aligned page
  {
    const Point expected[4] =
        { Point(90, 70), Point(510, 70), Point(510, 380), Point(90, 380) };
    std::vector<unsigned int> photo = makePhotoOfPage(width, height, expected, false);
    Point found[4];
    checkTrue(detectDocumentQuad(photo.data(), width, height, found), "should detect an axis-aligned page");
    checkQuadNear(found, expected, tolerance, "axis-aligned page");
  }

  // 2. a page photographed at an angle - the case the whole feature exists for
  {
    const Point expected[4] =
        { Point(110, 60), Point(505, 105), Point(470, 390), Point(75, 340) };
    std::vector<unsigned int> photo = makePhotoOfPage(width, height, expected, false);
    Point found[4];
    checkTrue(detectDocumentQuad(photo.data(), width, height, found), "should detect a skewed page");
    checkQuadNear(found, expected, tolerance, "skewed page");
  }

  // 3. the same page with text on it: the detector must still return the page outline, not a text block
  {
    const Point expected[4] =
        { Point(110, 60), Point(505, 105), Point(470, 390), Point(75, 340) };
    std::vector<unsigned int> photo = makePhotoOfPage(width, height, expected, true);
    Point found[4];
    checkTrue(detectDocumentQuad(photo.data(), width, height, found), "should detect a page with text");
    checkQuadNear(found, expected, tolerance, "page with text");
  }

  // 4. nothing to find: a blank image must be refused rather than guessed at
  {
    std::vector<unsigned int> blank(size_t(width)*height, 0xFF808080u);
    Point found[4] = { Point(1,1), Point(2,2), Point(3,3), Point(4,4) };
    const Point untouched[4] = { Point(1,1), Point(2,2), Point(3,3), Point(4,4) };
    checkTrue(!detectDocumentQuad(blank.data(), width, height, found), "blank image should not detect");
    checkQuadNear(found, untouched, 0, "failed detection should leave the quad alone");
  }

  // 5. a page too small to be the subject of the photo should be refused, not returned
  {
    const Point tiny[4] = { Point(280, 200), Point(340, 200), Point(340, 250), Point(280, 250) };
    std::vector<unsigned int> photo = makePhotoOfPage(width, height, tiny, false);
    Point found[4];
    checkTrue(!detectDocumentQuad(photo.data(), width, height, found), "a tiny page should be refused");
  }

  // 6. the fallback quad
  {
    Point fallback[4];
    defaultDocumentQuad(400, 300, 0.1, fallback);
    checkTrue(fallback[0] == Point(40, 30) && fallback[2] == Point(360, 270), "default quad should inset");
    checkTrue(isConvexQuad(fallback), "default quad should be convex");
  }

  // 7. a detected quad must be usable as-is by the rest of the pipeline
  {
    const Point expected[4] =
        { Point(110, 60), Point(505, 105), Point(470, 390), Point(75, 340) };
    std::vector<unsigned int> photo = makePhotoOfPage(width, height, expected, false);
    Point found[4];
    if(detectDocumentQuad(photo.data(), width, height, found)) {
      checkTrue(isConvexQuad(found), "a detected quad should be convex");
      int dstw = 0, dsth = 0;
      quadOutputSize(found, 1200, &dstw, &dsth);
      checkTrue(dstw > 0 && dsth > 0, "a detected quad should give a usable output size");
      std::vector<unsigned int> flattened(size_t(dstw)*dsth, 0u);
      warpQuadRGBA(photo.data(), width, height, flattened.data(), dstw, dsth, found);
      // the flattened page should be the bright page, not the dark background
      const unsigned char* bytes = (const unsigned char*)flattened.data();
      int centerValue = bytes[(size_t(dsth/2)*dstw + dstw/2)*4];
      checkTrue(centerValue > 200, "flattening a detected quad should give the page, not the background");
    }
  }
}

// Realistic photos for quad detection.  The plain test pages above are a bright quad on a flat dark
//  background, which every detector gets right - including one that the user reported "does not work at
//  all" on real iPad photos.  What real photos add, and what each scene below exercises:
//  - a background with its own straight lines (plank seams, a checked tablecloth, a laptop edge): any
//    "take the outermost line" rule picks those instead of the page
//  - low contrast between the page and a light desk
//  - a soft shadow across the page, ruled lines and handwriting: edges inside the page
//  - uneven light, sensor noise and JPEG blocking
// The scenes are drawn procedurally (no fixture files) and deterministically, from a fixed seed.

struct SceneRandom {
  uint32_t state;
  explicit SceneRandom(uint32_t seed) : state(seed*2654435761u + 1) {}
  float uniform() {  // xorshift32, [0, 1)
    state ^= state << 13;  state ^= state >> 17;  state ^= state << 5;
    return (state >> 8)*(1.0f/16777216.0f);
  }
  float gauss() {  // sum of uniforms is close enough to normal for sensor noise
    return (uniform() + uniform() + uniform() + uniform() - 2.0f)*1.732f;
  }
};

enum SceneBackground { WOOD_TABLE, CHECKED_CLOTH, LIGHT_DESK, WHITE_DESK, DARK_FABRIC };

struct PhotoScene {
  Point page[4];
  SceneBackground background = WOOD_TABLE;
  bool handwriting = true;
  bool handShadow = false;   // a soft shadow (hand, phone, tablet) across part of the page
  bool laptopEdge = false;   // a dark straight-edged object outside the page
  bool pen = false;
  bool tableEdge = false;    // the far edge of the table across the top of the photo, wall behind it
  bool secondSheet = false;  // another sheet of paper, half out of frame at the left
  float lightFalloff = 0.25f;  // brightness lost from one corner of the photo to the opposite one
  float noiseSigma = 4;
  int jpegQuality = 0;       // 0 skips the JPEG round trip
  uint32_t seed = 1;
};

struct SceneColor { float r, g, b; };

static SceneColor mixColor(SceneColor a, SceneColor b, float t)
{
  return { a.r + (b.r - a.r)*t, a.g + (b.g - a.g)*t, a.b + (b.b - a.b)*t };
}

static float clamp01(float value) { return std::min(1.0f, std::max(0.0f, value)); }

static SceneColor sceneBackground(const PhotoScene& scene, float x, float y)
{
  switch(scene.background) {
  case WOOD_TABLE: {
    // grain running along y, and plank seams every 260 px: long straight lines that are not the page
    float grain = std::sin(0.05f*(x + 22*std::sin(y*0.004f))) + 0.5f*std::sin(0.31f*x + 3*std::sin(y*0.01f));
    SceneColor wood = { 150 + 14*grain, 102 + 10*grain, 62 + 6*grain };
    float seam = std::abs(std::fmod(x + 0.05f*y + 1000, 260.0f) - 130);
    if(seam > 126) wood = mixColor(wood, SceneColor{60, 38, 22}, 0.8f);
    return wood;
  }
  case CHECKED_CLOTH: {
    // gingham turned 8 degrees: two families of strong straight lines, nearly parallel to the page edges
    float cosA = 0.990f, sinA = 0.139f;
    float u = x*cosA + y*sinA, v = -x*sinA + y*cosA;
    int stripeU = int(std::floor(u/70)) & 1, stripeV = int(std::floor(v/70)) & 1;
    float red = 0.35f*(stripeU + stripeV);
    return mixColor(SceneColor{235, 230, 225}, SceneColor{170, 30, 40}, red);
  }
  case LIGHT_DESK:
    return { 196, 198, 200 };  // clearly darker than the paper, but only just
  case WHITE_DESK:
    return { 222, 223, 225 };  // nearly as bright as the paper: the outline is mostly its shadow
  case DARK_FABRIC: default: {
    float weave = std::sin(1.3f*x)*std::sin(1.1f*y);
    return { 52 + 8*weave, 56 + 8*weave, 70 + 8*weave };
  }
  }
}

// paper at page coordinates (u, v) in [0,1]: ruled lines, a margin, and words of "handwriting"
static SceneColor scenePaper(const PhotoScene& scene, float u, float v)
{
  SceneColor paper = { 240, 238, 230 };
  const float lineSpacing = 1/32.0f;
  if(v > 0.1f) {
    float lineOffset = std::fmod(v - 0.1f, lineSpacing);
    float distance = std::min(lineOffset, lineSpacing - lineOffset);
    paper = mixColor(paper, SceneColor{150, 185, 225}, 0.8f*clamp01(1 - distance/0.0012f));
  }
  paper = mixColor(paper, SceneColor{225, 120, 120}, 0.7f*clamp01(1 - std::abs(u - 0.12f)/0.0015f));
  if(scene.handwriting && v > 0.1f && v < 0.85f && u > 0.15f && u < 0.9f) {
    int lineIndex = int((v - 0.1f)/lineSpacing);
    float withinLine = (v - 0.1f)/lineSpacing - lineIndex;
    // words are runs along u, a hash of line and word index deciding which runs are ink
    int wordIndex = int(u*14);
    uint32_t hash = (uint32_t(lineIndex)*73856093u) ^ (uint32_t(wordIndex)*19349663u) ^ scene.seed;
    hash = hash*2654435761u;
    bool isWord = (hash >> 28) < 11 && lineIndex % 5 != 4;
    float stroke = std::sin(u*900 + lineIndex)*0.5f + 0.5f;  // letters: ink comes and goes along the word
    if(isWord && withinLine > 0.35f && withinLine < 0.9f && stroke > 0.55f)
      paper = mixColor(paper, SceneColor{35, 40, 80}, 0.9f);
  }
  return paper;
}

// distance inside a convex quad (negative outside), for soft shadows
static float signedDistanceInQuad(const Point quad[4], Point point)
{
  float inside = 1e9f;
  for(int corner = 0; corner < 4; ++corner) {
    Point edge = quad[(corner + 1) % 4] - quad[corner];
    float distance = float(cross(edge, point - quad[corner])/edge.dist());
    inside = std::min(inside, distance);
  }
  return inside;
}

static bool pointInQuad(const Point quad[4], Point point) { return signedDistanceInQuad(quad, point) >= 0; }

static std::vector<unsigned int> makeScenePhoto(int width, int height, const PhotoScene& scene)
{
  std::vector<unsigned int> pixels(size_t(width)*height);
  unsigned char* bytes = (unsigned char*)pixels.data();
  Transform3D photoToPage = Transform3D::quadToUnit(scene.page);
  SceneRandom random(scene.seed);
  // straight-edged clutter: a laptop entering from the right, and a pen lying on the table
  Point laptop[4] = { Point(width*0.93, -50), Point(width + 50.0, -50), Point(width + 50.0, height + 50.0),
      Point(width*0.90, height + 50.0) };
  Point secondSheet[4] = { Point(-300, height*0.55), Point(width*0.1, height*0.5),
      Point(width*0.14, height*0.85), Point(-260, height*0.9) };
  Point penCenter(width*0.12, height*0.9);
  Point penAxis(0.94, -0.34);
  Point penNormal(-penAxis.y, penAxis.x);
  Point penQuad[4] = { penCenter - penAxis*180 - penNormal*7, penCenter + penAxis*180 - penNormal*7,
      penCenter + penAxis*180 + penNormal*7, penCenter - penAxis*180 + penNormal*7 };
  float diagonal = std::sqrt(float(width*width + height*height));
  for(int y = 0; y < height; ++y) {
    for(int x = 0; x < width; ++x) {
      Point center(x + 0.5, y + 0.5);
      SceneColor background = sceneBackground(scene, center.x, center.y);
      // drop shadow: the page casts a soft shadow down and to the right
      float shadowDepth = signedDistanceInQuad(scene.page, center - Point(9, 12));
      background = mixColor(background, SceneColor{0, 0, 0}, 0.3f*clamp01((shadowDepth + 14)/20));
      if(scene.tableEdge && center.y < 0.07*center.x + height*0.08)
        background = { 205, 196, 180 };  // a beige wall, well lit, behind a straight table edge
      if(scene.secondSheet && pointInQuad(secondSheet, center))
        background = scenePaper(scene, 0.5f, 0.05f);
      if(scene.laptopEdge && pointInQuad(laptop, center))
        background = { 70, 72, 78 };
      if(scene.pen && pointInQuad(penQuad, center))
        background = { 30, 40, 110 };
      // page coverage from a 2x2 grid, so the outline is anti-aliased like a real lens would draw it
      int covered = 0;
      for(int sub = 0; sub < 4; ++sub)
        covered += pointInQuad(scene.page, Point(x + 0.25 + 0.5*(sub & 1), y + 0.25 + 0.5*(sub >> 1)));
      SceneColor color = background;
      if(covered > 0) {
        Point pageCoord = photoToPage.map(center);
        SceneColor paper = scenePaper(scene, float(clamp01(float(pageCoord.x))), float(clamp01(float(pageCoord.y))));
        color = mixColor(background, paper, covered/4.0f);
      }
      // uneven light: falloff across the frame, a vignette, and optionally a soft shadow over the page
      float light = 1 - scene.lightFalloff*(0.6f*x/width + 0.4f*y/height);
      float radius = float((center - Point(width/2.0, height/2.0)).dist())/(diagonal/2);
      light *= 1 - 0.2f*radius*radius;
      if(scene.handShadow) {
        float across = float(center.x*0.8 + center.y*0.6) - 0.75f*(width*0.8f + height*0.6f);
        light *= 1 - 0.4f*clamp01((across + 25)/50);
      }
      unsigned char* pixel = bytes + (size_t(y)*width + x)*4;
      float channels[3] = { color.r, color.g, color.b };
      for(int channel = 0; channel < 3; ++channel) {
        float value = channels[channel]*light + scene.noiseSigma*random.gauss();
        pixel[channel] = (unsigned char)std::min(255.0f, std::max(0.0f, value + 0.5f));
      }
      pixel[3] = 255;
    }
  }
  if(scene.jpegQuality > 0) {
    std::vector<unsigned char> encoded;
    stbi_write_jpg_to_func([](void* context, void* data, int size) {
      auto* output = static_cast<std::vector<unsigned char>*>(context);
      output->insert(output->end(), (unsigned char*)data, (unsigned char*)data + size);
    }, &encoded, width, height, 4, pixels.data(), scene.jpegQuality);
    int decodedWidth = 0, decodedHeight = 0;
    unsigned char* decoded = stbi_load_from_memory(encoded.data(), int(encoded.size()),
        &decodedWidth, &decodedHeight, NULL, 4);
    if(decoded && decodedWidth == width && decodedHeight == height)
      memcpy(pixels.data(), decoded, pixels.size()*4);
    stbi_image_free(decoded);
  }
  return pixels;
}

#ifdef SCANTEST_MAIN
// SCANTEST_DUMP=dir writes each scene with the expected (green) and detected (red) corners, for tuning
static void dumpScene(const char* name, std::vector<unsigned int> pixels, int width, int height,
    const Point expected[4], const Point found[4], bool detected)
{
  const char* directory = getenv("SCANTEST_DUMP");
  if(!directory)
    return;
  auto mark = [&](Point corner, unsigned int abgr) {
    for(int dy = -6; dy <= 6; ++dy) {
      for(int dx = -6; dx <= 6; ++dx) {
        int x = int(corner.x) + dx, y = int(corner.y) + dy;
        if(x >= 0 && y >= 0 && x < width && y < height && (std::abs(dx) < 2 || std::abs(dy) < 2))
          pixels[size_t(y)*width + x] = abgr;
      }
    }
  };
  for(int ii = 0; ii < 4; ++ii) {
    mark(expected[ii], 0xFF00FF00u);
    if(detected)
      mark(found[ii], 0xFF0000FFu);
  }
  char path[1024];
  snprintf(path, sizeof(path), "%s/%s.png", directory, name);
  stbi_write_png(path, width, height, 4, pixels.data(), width*4);
}
#else
static void dumpScene(const char*, std::vector<unsigned int>, int, int, const Point*, const Point*, bool) {}
#endif

static void testQuadDetectRealistic()
{
  // a portrait photo, as an iPad held over a portrait page takes it
  const int width = 1200, height = 1600;
  // one and a half percent of the long edge: about one fingertip of correction on the iPad
  const real tolerance = 24;
  struct NamedScene { const char* name; PhotoScene scene; };
  std::vector<NamedScene> scenes;

  {  // wood table with plank seams on both sides of the page, mild perspective
    PhotoScene scene;
    const Point page[4] = { Point(230, 210), Point(990, 250), Point(1020, 1360), Point(170, 1330) };
    std::copy(page, page + 4, scene.page);
    scene.jpegQuality = 80;
    scenes.push_back({"wood", scene});
  }
  {  // checked tablecloth, page rotated about 10 degrees
    PhotoScene scene;
    const Point page[4] = { Point(300, 170), Point(1050, 300), Point(900, 1420), Point(140, 1290) };
    std::copy(page, page + 4, scene.page);
    scene.background = CHECKED_CLOTH;
    scene.jpegQuality = 70;
    scene.seed = 2;
    scenes.push_back({"cloth", scene});
  }
  {  // a white desk: the page is barely brighter, and a hand shadow falls across it
    PhotoScene scene;
    const Point page[4] = { Point(210, 190), Point(1000, 170), Point(1050, 1400), Point(160, 1430) };
    std::copy(page, page + 4, scene.page);
    scene.background = WHITE_DESK;
    scene.handShadow = true;
    scene.jpegQuality = 80;
    scene.seed = 7;
    scenes.push_back({"whitedesk", scene});
  }
  {  // the table's far edge and the wall behind it across the top: a long straight line outside the page
    PhotoScene scene;
    const Point page[4] = { Point(240, 330), Point(980, 300), Point(1060, 1440), Point(180, 1480) };
    std::copy(page, page + 4, scene.page);
    scene.background = LIGHT_DESK;
    scene.tableEdge = true;
    scene.jpegQuality = 75;
    scene.seed = 8;
    scenes.push_back({"tableedge", scene});
  }
  {  // a light desk - low contrast - with a hand shadow across the bottom of the page
    PhotoScene scene;
    const Point page[4] = { Point(190, 160), Point(1010, 190), Point(1040, 1420), Point(150, 1400) };
    std::copy(page, page + 4, scene.page);
    scene.background = LIGHT_DESK;
    scene.handShadow = true;
    scene.jpegQuality = 75;
    scene.seed = 3;
    scenes.push_back({"desk", scene});
  }
  {  // dark fabric, strong keystone (top edge further away), a laptop and a pen next to the page
    PhotoScene scene;
    const Point page[4] = { Point(330, 300), Point(880, 290), Point(1030, 1450), Point(150, 1470) };
    std::copy(page, page + 4, scene.page);
    scene.background = DARK_FABRIC;
    scene.laptopEdge = true;
    scene.pen = true;
    scene.noiseSigma = 7;
    scene.jpegQuality = 60;
    scene.seed = 4;
    scenes.push_back({"fabric", scene});
  }
  {  // the page filling nearly the whole frame, as when the iPad is held close
    PhotoScene scene;
    const Point page[4] = { Point(60, 70), Point(1150, 50), Point(1160, 1540), Point(40, 1560) };
    std::copy(page, page + 4, scene.page);
    scene.lightFalloff = 0.4f;
    scene.jpegQuality = 80;
    scene.seed = 5;
    scenes.push_back({"close", scene});
  }
  {  // wood again, with every distraction: laptop, pen, shadow, heavy noise and JPEG
    PhotoScene scene;
    const Point page[4] = { Point(260, 280), Point(960, 230), Point(1000, 1290), Point(220, 1340) };
    std::copy(page, page + 4, scene.page);
    scene.laptopEdge = true;
    scene.pen = true;
    scene.handShadow = true;
    scene.tableEdge = true;
    scene.secondSheet = true;
    scene.noiseSigma = 8;
    scene.jpegQuality = 55;
    scene.seed = 6;
    scenes.push_back({"cluttered", scene});
  }

  for(const NamedScene& named : scenes) {
#ifdef SCANTEST_MAIN
    // SCANTEST_ONLY=name runs just that scene, for tuning
    if(getenv("SCANTEST_ONLY") && strcmp(getenv("SCANTEST_ONLY"), named.name) != 0)
      continue;
#endif
    std::vector<unsigned int> photo = makeScenePhoto(width, height, named.scene);
    Point found[4];
    auto start = std::chrono::steady_clock::now();
    bool detected = detectDocumentQuad(photo.data(), width, height, found);
    double elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    dumpScene(named.name, photo, width, height, named.scene.page, found, detected);
#ifdef SCANTEST_MAIN
    // per scene: detection time and worst corner error as a fraction of the long image side
    real worstError = 0;
    for(int ii = 0; ii < 4 && detected; ++ii)
      worstError = std::max(worstError, (found[ii] - named.scene.page[ii]).dist());
    printf("   %-10s %s in %.1f ms, worst corner off by %.1f%% of the image\n", named.name,
        detected ? "found" : "MISSED", elapsedMs, double(100*worstError/std::max(width, height)));
#else
    (void)elapsedMs;
#endif
    char what[128];
    snprintf(what, sizeof(what), "should detect the page in a realistic photo (%s)", named.name);
    checkTrue(detected, what);
    if(detected) {
      snprintf(what, sizeof(what), "realistic photo (%s)", named.name);
      checkQuadNear(found, named.scene.page, tolerance, what);
    }
  }

  // a photo of the table alone: background lines must not be passed off as a page
  {
    PhotoScene scene;
    const Point offscreen[4] = { Point(-900, -900), Point(-800, -900), Point(-800, -800), Point(-900, -800) };
    std::copy(offscreen, offscreen + 4, scene.page);
    scene.jpegQuality = 80;
    std::vector<unsigned int> photo = makeScenePhoto(width, height, scene);
    Point found[4];
    bool detected = detectDocumentQuad(photo.data(), width, height, found);
    dumpScene("nopage", photo, width, height, offscreen, found, detected);
    checkTrue(!detected, "a photo of an empty wooden table should not detect a page");
  }
}

int runScanTests()
{
  nChecksFailed = 0;
  testHomography();
  testWarp();
  testOutputSize();
  testFlattenIllumination();
  testLevelsAndGreyscale();
  testAdaptiveThreshold();
  testRotate();
  testEnhanceDocument();
  testQuadDetect();
  testQuadDetectRealistic();
  return nChecksFailed;
}

#ifdef SCANTEST_MAIN
int main()
{
  int nFailed = runScanTests();
  printf(nFailed ? "scantest: %d checks FAILED\n" : "scantest: all checks passed\n", nFailed);
  return nFailed ? 1 : 0;
}
#endif
