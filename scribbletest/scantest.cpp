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
#include <vector>

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
