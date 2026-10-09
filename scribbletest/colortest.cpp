// Unit tests for the themed palette math in ulib/oklab.cpp and ulib/palettegen.cpp (COLORS_SPEC.md).
// Like scantest.cpp and shapetest.cpp these need no GL context and no document, so they can also be
//  built and run on their own - which is how they get run in CI, where no display is available:
//
//   g++ -std=c++14 -O2 -DNDEBUG -I . -DCOLORTEST_MAIN scribbletest/colortest.cpp ulib/oklab.cpp
//       ulib/palettegen.cpp ulib/geom.cpp -o colortest && ./colortest
//   (one command, run from the repo root.  NDEBUG is needed because geom.cpp's ASSERT would otherwise
//   pull in platform_assert, which lives in the application.)
//
// Run ./colortest --dump to print the golden table in the form pasted into GOLDEN_CUSPWALK1 below.
//
// runColorTests() returns the number of failed checks and is also called from ScribbleTest::runAll().

#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include "ulib/palettegen.h"
#include "syncscribble/markercolor.h"

static int nColorChecksFailed = 0;

static void colorCheckTrue(bool condition, const char* what)
{
  if(!condition) {
    ++nColorChecksFailed;
    printf("FAIL: %s\n", what);
  }
}

static void colorCheckNear(real actual, real expected, real eps, const char* what)
{
  if(!(std::abs(actual - expected) <= eps)) {
    ++nColorChecksFailed;
    printf("FAIL: %s (got %g, expected %g)\n", what, double(actual), double(expected));
  }
}

static std::string toHex(Color c)
{
  char buf[16];
  snprintf(buf, sizeof(buf), "#%02x%02x%02x", c.red(), c.green(), c.blue());
  return std::string(buf);
}

// a recipe that exercises every knob away from its default, so the golden check covers all of them
static PaletteRecipe goldenRecipe()
{
  PaletteRecipe r;
  r.gen = "cusp-walk-1";
  r.seedHue = 218;
  r.vividness = 1.0;
  r.depth = 0.10;
  r.minContrast = 3.0;
  r.jitter = 11;
  r.paperL = 0.99;
  r.paperWarm = 0.35;
  r.families = 12;
  return r;
}

// ----------------------------------------------------------------------------------------------------
// 1. The freeze (COLORS_SPEC.md §3.2).  A shipped generator's output must never change; this is what
//    makes that a property rather than an intention.  If this fails, the correct response is almost
//    always to add a new generator id, NOT to update the table.
// ----------------------------------------------------------------------------------------------------

static const char* GOLDEN_CUSPWALK1[] = {
#include "colortest_golden.inc"
};

static void testGolden()
{
  Palette pal;
  generatePalette(goldenRecipe(), &pal);

  const int nGolden = int(sizeof(GOLDEN_CUSPWALK1)/sizeof(GOLDEN_CUSPWALK1[0]));
  // paper, rule, bookmark, link, selection, neutral, then base/dark/hl per family
  const int nExpected = 6 + 3*int(pal.families.size());
  colorCheckTrue(nGolden == nExpected, "golden table size matches generated palette");
  if(nGolden != nExpected)
    return;

  int idx = 0;
  const Color fixed[6] = { pal.paper, pal.rule, pal.bookmark, pal.link, pal.selection, pal.neutral };
  const char* fixedNames[6] = { "paper", "rule", "bookmark", "link", "selection", "neutral" };
  for(int ii = 0; ii < 6; ++ii, ++idx) {
    if(toHex(fixed[ii]) != GOLDEN_CUSPWALK1[idx]) {
      ++nColorChecksFailed;
      printf("FAIL: cusp-walk-1 is FROZEN - %s changed (got %s, expected %s).\n"
             "      Add a new generator id rather than updating the golden table.\n",
          fixedNames[ii], toHex(fixed[ii]).c_str(), GOLDEN_CUSPWALK1[idx]);
    }
  }
  for(size_t ff = 0; ff < pal.families.size(); ++ff) {
    for(int vv = 0; vv < PALETTE_NUM_VARIANTS; ++vv, ++idx) {
      Color got = pal.families[ff].variant(vv);
      if(toHex(got) != GOLDEN_CUSPWALK1[idx]) {
        ++nColorChecksFailed;
        printf("FAIL: cusp-walk-1 is FROZEN - family %d variant %d changed (got %s, expected %s).\n"
               "      Add a new generator id rather than updating the golden table.\n",
            int(ff), vv, toHex(got).c_str(), GOLDEN_CUSPWALK1[idx]);
      }
    }
  }
}

// ----------------------------------------------------------------------------------------------------
// 2. The legibility invariant.  This is the whole point of the walk, so it is checked over a spread of
//    recipes rather than one - including dark paper, where the algorithm mirrors.
// ----------------------------------------------------------------------------------------------------

static void testLegibility()
{
  int nChecked = 0, nShort = 0;
  for(int seed = 0; seed < 360; seed += 7) {
    for(int paperStep = 0; paperStep < 4; ++paperStep) {
      PaletteRecipe r;
      r.gen = "cusp-walk-1";
      r.seedHue = real(seed);
      // 0.18 and 0.30 are dark paper (the mirrored walk), 0.94 and 0.99 light
      const real papers[4] = { real(0.18), real(0.30), real(0.94), real(0.99) };
      r.paperL = papers[paperStep];
      r.vividness = real(0.5) + real(0.5)*real(paperStep)/real(3);
      r.minContrast = 3.0;

      Palette pal;
      generatePalette(r, &pal);
      for(size_t ff = 0; ff < pal.families.size(); ++ff) {
        ++nChecked;
        if(srgbContrast(pal.families[ff].base, pal.paper) < r.minContrast - real(0.01))
          ++nShort;
      }
    }
  }
  colorCheckTrue(nChecked > 1000, "legibility sweep actually ran");
  if(nShort > 0) {
    ++nColorChecksFailed;
    printf("FAIL: %d of %d generated inks fall below minContrast against their own paper\n",
        nShort, nChecked);
  }
}

// ----------------------------------------------------------------------------------------------------
// 3. Everything generated must be inside sRGB - a chroma clamp going wrong shows up as a color that
//    renders differently than the generator believes it does.
// ----------------------------------------------------------------------------------------------------

static void testGamut()
{
  int nOut = 0;
  for(int seed = 0; seed < 360; seed += 13) {
    for(int vv = 0; vv <= 4; ++vv) {
      PaletteRecipe r;
      r.gen = "cusp-walk-1";
      r.seedHue = real(seed);
      r.vividness = real(0.25) + real(0.75)*real(vv)/real(4);
      Palette pal;
      generatePalette(r, &pal);
      for(size_t ff = 0; ff < pal.families.size(); ++ff) {
        for(int variant = 0; variant < PALETTE_NUM_VARIANTS; ++variant) {
          Color c = pal.families[ff].variant(variant);
          // round-tripping through OKLCh must not move the color: if the generator produced something
          //  out of gamut, oklchToColor's clamp already moved it and the round trip will disagree
          ColorOkLch lch = oklchFromColor(c);
          Color back = oklchToColor(lch, c.alpha());
          if(std::abs(back.red() - c.red()) > 1 || std::abs(back.green() - c.green()) > 1
              || std::abs(back.blue() - c.blue()) > 1)
            ++nOut;
        }
      }
    }
  }
  if(nOut > 0) {
    ++nColorChecksFailed;
    printf("FAIL: %d generated colors do not survive an OKLCh round trip (out of gamut)\n", nOut);
  }
}

// ----------------------------------------------------------------------------------------------------
// 4. Determinism.  A RNG creeping into the jitter would repaint every theme on every launch and make
//    the same document look different on two machines.
// ----------------------------------------------------------------------------------------------------

static void testDeterminism()
{
  PaletteRecipe r = goldenRecipe();
  Palette a, b;
  generatePalette(r, &a);
  generatePalette(r, &b);

  bool same = a.families.size() == b.families.size() && a.paper == b.paper;
  for(size_t ii = 0; same && ii < a.families.size(); ++ii) {
    same = a.families[ii].base == b.families[ii].base && a.families[ii].dark == b.families[ii].dark
        && a.families[ii].hl == b.families[ii].hl;
  }
  colorCheckTrue(same, "the same recipe generates the same palette twice");

  // and a different seed must actually change it, or the jitter/seed plumbing is dead
  PaletteRecipe r2 = r;
  r2.seedHue = 40;
  Palette c;
  generatePalette(r2, &c);
  colorCheckTrue(!(c.families[0].base == a.families[0].base), "a different seed gives a different palette");
}

// ----------------------------------------------------------------------------------------------------
// 5. Snap idempotence.  Without this a color drifts a little on every edit that re-snaps it.
// ----------------------------------------------------------------------------------------------------

static void testSnapIdempotent()
{
  Palette pal;
  generatePalette(goldenRecipe(), &pal);

  int nBad = 0;
  for(int rr = 0; rr < 256; rr += 37) {
    for(int gg = 0; gg < 256; gg += 29) {
      for(int bb = 0; bb < 256; bb += 41) {
        Color once = pal.nearest(Color(rr, gg, bb));
        Color twice = pal.nearest(once);
        if(!(once == twice))
          ++nBad;
      }
    }
  }
  if(nBad > 0) {
    ++nColorChecksFailed;
    printf("FAIL: snapping is not idempotent for %d sampled colors\n", nBad);
  }

  // every palette member must snap to itself, or the picker would move a color the user just picked
  int nMoved = 0;
  for(size_t ff = 0; ff < pal.families.size(); ++ff) {
    for(int vv = 0; vv < PALETTE_NUM_VARIANTS; ++vv) {
      Color c = pal.families[ff].variant(vv);
      if(!(pal.nearest(c) == c))
        ++nMoved;
    }
  }
  if(nMoved > 0) {
    ++nColorChecksFailed;
    printf("FAIL: %d palette entries do not snap to themselves\n", nMoved);
  }
}

// The marker's swatches are vividMarker() of each family's `hl`, off the palette on purpose.  The pen
//  toolbar snaps every color it is given (updateColor()), and snapping a marker swatch moved the pen off
//  it: the swatch then showed no ring and a second tap did not open its editor (pen-and-tools.md, "Marker:
//  vivid colour and alpha").  Every shipped theme, light and dark, since which families the chroma
//  boost actually changes depends on the recipe.
static void testMarkerSwatchesKeptBySnap()
{
  int nMoved = 0, nChecked = 0, nOffPalette = 0;
  for(int tt = 0; tt < paletteThemeCount(); ++tt) {
    for(int dark = 0; dark < 2; ++dark) {
      Palette pal;
      generatePalette(paletteThemeRecipe(*paletteThemeByIndex(tt), dark != 0), &pal);
      for(const PaletteFamily& family : pal.families) {
        Color swatch = vividMarker(family.hl, pal.recipe.vividness);
        ++nChecked;
        if(!(pal.nearest(swatch).opaque() == swatch.opaque()))
          ++nOffPalette;
        if(!(snapThemedPenColor(pal, swatch, true) == swatch))
          ++nMoved;
      }
      // an ink pen still snaps: a vivid marker color is not a member for it
      Color vivid0 = vividMarker(pal.families[0].hl, pal.recipe.vividness);
      Color expected = pal.nearest(vivid0);
      expected.setAlpha(vivid0.alpha());
      if(!(snapThemedPenColor(pal, vivid0, false) == expected))
        ++nMoved;
    }
  }
  // the bug needs swatches the palette does not hold; if none were, this test would test nothing
  colorCheckTrue(nOffPalette > 0, "some marker swatches are off the palette (else the snap check is vacuous)");
  if(nMoved > 0) {
    ++nColorChecksFailed;
    printf("FAIL: %d of %d marker swatches are moved by the theme's snap\n", nMoved, nChecked);
  }
}

// ----------------------------------------------------------------------------------------------------
// 6. Restyle round trip.  A → B → A must return every stroke to the color it started with, or restyling
//    is lossy and the user cannot undo a theme by re-applying the old one.
// ----------------------------------------------------------------------------------------------------

static void testRestyleRoundTrip()
{
  PaletteRecipe ra = goldenRecipe();
  PaletteRecipe rb = goldenRecipe();
  rb.seedHue = 47;
  rb.vividness = real(0.6);
  rb.paperL = real(0.22);   // and across the light/dark mirror, which is the interesting case

  Palette pa, pb;
  generatePalette(ra, &pa);
  generatePalette(rb, &pb);

  const Palette& ref = referencePalette();

  // The strong check: matching the reference palette's own colors *into the reference palette* must be
  //  the identity.  Any error in the hue matching - an off-by-one, a bad wrap at 0/360, or the classic
  //  "always returns family 0" - breaks this, while an A->B->A comparison would not, since __inkbase
  //  never changes and so A->B->A is true however wrong the matching is.
  int nNotIdentity = 0;
  for(size_t ff = 0; ff < ref.families.size(); ++ff) {
    for(int vv = 0; vv < PALETTE_NUM_VARIANTS; ++vv) {
      Color inkbase = ref.families[ff].variant(vv);
      Color got;
      if(!ref.matchReference(inkbase, vv, &got) || !(got == inkbase))
        ++nNotIdentity;
    }
  }
  if(nNotIdentity > 0) {
    ++nColorChecksFailed;
    printf("FAIL: matching the reference palette into itself moved %d of its own colors\n", nNotIdentity);
  }

  // Distinct reference families must land on distinct families in a target palette of the same size.
  //  A collapsing match still round-trips perfectly while quietly turning a twelve-color document into
  //  a three-color one.
  for(int which = 0; which < 2; ++which) {
    const Palette& target = which == 0 ? pa : pb;
    std::vector<Color> seen;
    for(size_t ff = 0; ff < ref.families.size(); ++ff) {
      Color got;
      if(!target.matchReference(ref.families[ff].base, PALETTE_BASE, &got))
        continue;
      for(size_t jj = 0; jj < seen.size(); ++jj) {
        if(seen[jj] == got) {
          ++nColorChecksFailed;
          printf("FAIL: reference families %d and %d both map to %s in target palette %d\n",
              int(jj), int(ff), toHex(got).c_str(), which);
        }
      }
      seen.push_back(got);
    }
    colorCheckTrue(seen.size() == ref.families.size(), "every reference family matched something");
  }

  // ...and the two themes must actually differ, or the mapping could be correct and inert at once
  int nDifferent = 0;
  for(size_t ff = 0; ff < ref.families.size(); ++ff) {
    Color inA, inB;
    if(pa.matchReference(ref.families[ff].base, PALETTE_BASE, &inA)
        && pb.matchReference(ref.families[ff].base, PALETTE_BASE, &inB) && !(inA == inB))
      ++nDifferent;
  }
  colorCheckTrue(nDifferent >= int(ref.families.size()) - 1,
      "restyling to a different theme actually changes the colors");
}

// ----------------------------------------------------------------------------------------------------
// 7. An unknown generator id must fall back rather than fail, and must not rewrite the recipe - a
//    document from a newer version has to round-trip back to disk unchanged.
// ----------------------------------------------------------------------------------------------------

static void testUnknownGenerator()
{
  PaletteRecipe r = goldenRecipe();
  r.gen = "cusp-walk-99";
  Palette pal;
  bool known = generatePalette(r, &pal);
  colorCheckTrue(!known, "an unknown generator id is reported as unknown");
  colorCheckTrue(!pal.families.empty(), "an unknown generator id still yields a usable palette");
  colorCheckTrue(pal.recipe.gen == "cusp-walk-99",
      "an unknown generator id is preserved in the recipe, not rewritten");

  // an empty id means "use the default" and is not an unknown-version warning
  PaletteRecipe r2 = goldenRecipe();
  r2.gen = "";
  Palette pal2;
  colorCheckTrue(generatePalette(r2, &pal2), "an empty generator id is not reported as unknown");
}

// ----------------------------------------------------------------------------------------------------
// 8. The OKLab layer itself, pinned against the measurements quoted in COLORS_SPEC.md §2.2/§2.3.  These
//    are what the whole design was argued from; if they drift, the spec is describing different code.
// ----------------------------------------------------------------------------------------------------

static void testOklabAgainstSpec()
{
  // Gunter Schuler "modern" - all three ride the gamut boundary, well below their hue's cusp
  struct { const char* name; int r, g, b; real L, C, h, frac, dCusp; } schuler[3] = {
    { "magenta", 213,  19, 121, real(0.573), real(0.226), real(357.4), real(0.97), real(-0.077) },
    { "cyan",      0, 169, 201, real(0.678), real(0.122), real(217.7), real(1.00), real(-0.132) },
    { "yellow",  221, 220,   0, real(0.867), real(0.189), real(109.4), real(1.00), real(-0.097) }
  };
  for(int ii = 0; ii < 3; ++ii) {
    Color c(schuler[ii].r, schuler[ii].g, schuler[ii].b);
    ColorOkLch lch = oklchFromColor(c);
    colorCheckNear(lch.L, schuler[ii].L, real(0.002), "Schuler lightness");
    colorCheckNear(lch.C, schuler[ii].C, real(0.002), "Schuler chroma");
    colorCheckNear(lch.h, schuler[ii].h, real(0.2), "Schuler hue");
    real maxC = oklchMaxChroma(lch.L, lch.h);
    colorCheckNear(lch.C/maxC, schuler[ii].frac, real(0.02), "Schuler fraction of gamut edge");
    colorCheckNear(lch.L - oklchCusp(lch.h).L, schuler[ii].dCusp, real(0.01), "Schuler delta to cusp");
  }

  // §2.3: at its cusp, yellow is invisible on white and blue is not.  This is why the walk exists.
  Color white(255, 255, 255);
  ColorOkLch yellowCusp = oklchCusp(109);
  ColorOkLch blueCusp = oklchCusp(264);
  colorCheckTrue(srgbContrast(oklchToColor(yellowCusp), white) < real(1.5),
      "yellow at its cusp is unreadable on white");
  colorCheckTrue(srgbContrast(oklchToColor(blueCusp), white) > real(6.0),
      "blue at its cusp is readable on white");
  // the cusp moves a long way around the wheel - the fact a fixed lightness ramp cannot serve every hue
  colorCheckTrue(yellowCusp.L - blueCusp.L > real(0.4), "cusp lightness varies widely with hue");
}

// ----------------------------------------------------------------------------------------------------
// 9. The generated palette beats the naive alternatives on the measure that matters (§2.4).  Pins the
//    claim the whole feature is justified by.
// ----------------------------------------------------------------------------------------------------

static void testBeatsNaive()
{
  Palette pal;
  generatePalette(goldenRecipe(), &pal);

  int nBad = 0;
  for(size_t ff = 0; ff < pal.families.size(); ++ff) {
    if(srgbContrast(pal.families[ff].base, pal.paper) < real(2.99))
      ++nBad;
  }
  colorCheckTrue(nBad == 0, "no generated ink is below the legibility floor");

  // the naive comparison: fixed HSV saturation/value, hue stepped.  Must be clearly worse, or the
  //  generator is not earning its complexity.  Spelled out rather than calling ColorF::fromHSV so this
  //  file stays standalone-buildable, and because being explicit is the point of the comparison.
  int nNaiveBad = 0;
  for(int ii = 0; ii < 12; ++ii) {
    real h = real(ii*30)/real(60);
    real x = real(1) - std::abs(std::fmod(h, real(2)) - real(1));
    const real rgbSeg[6][3] = { {1,x,0}, {x,1,0}, {0,1,x}, {0,x,1}, {x,0,1}, {1,0,x} };
    int s = int(h) % 6;
    Color naive(int(rgbSeg[s][0]*255 + real(0.5)), int(rgbSeg[s][1]*255 + real(0.5)),
                int(rgbSeg[s][2]*255 + real(0.5)));
    if(srgbContrast(naive, pal.paper) < real(2.99))
      ++nNaiveBad;
  }
  colorCheckTrue(nNaiveBad >= 5, "naive hue rotation leaves many colors illegible (the case for the walk)");
}

// ----------------------------------------------------------------------------------------------------

static void dumpGolden()
{
  Palette pal;
  generatePalette(goldenRecipe(), &pal);
  printf("// Generated by `colortest --dump`.  cusp-walk-1 is FROZEN; if a change makes this table\n");
  printf("//  wrong, add a new generator id instead of regenerating it.  See COLORS_SPEC.md §3.2.\n");
  printf("\"%s\", \"%s\", \"%s\", \"%s\", \"%s\", \"%s\",  // paper, rule, bookmark, link, selection, neutral\n",
      toHex(pal.paper).c_str(), toHex(pal.rule).c_str(), toHex(pal.bookmark).c_str(),
      toHex(pal.link).c_str(), toHex(pal.selection).c_str(), toHex(pal.neutral).c_str());
  for(size_t ff = 0; ff < pal.families.size(); ++ff) {
    printf("\"%s\", \"%s\", \"%s\",  // family %d  (hue %.1f)\n",
        toHex(pal.families[ff].base).c_str(), toHex(pal.families[ff].dark).c_str(),
        toHex(pal.families[ff].hl).c_str(), int(ff), double(pal.families[ff].hue));
  }
}

// Every shipped theme, in BOTH modes.  A theme is ink character plus paper tint; the dialog's toggle
// supplies the mode, so a theme that is fine on white and unusable on black is only half checked.
static void testShippedThemes()
{
  for(int ii = 0; ii < paletteThemeCount(); ++ii) {
    const PaletteTheme* theme = paletteThemeByIndex(ii);
    // The chroma budget.  Separation between 12 families 30 degrees apart tracks their mean chroma
    //  almost exactly, and BOTH of these desaturate - measured at 12 families, light paper:
    //    minContrast 3.0 -> 7.0 at vividness 1.0:  sep 0.0415 -> 0.0293  (chroma 0.180 -> 0.149)
    //    vividness 1.0 -> 0.70 at minContrast 3.0: sep 0.0415 -> 0.0294  (chroma 0.180 -> 0.126)
    //  They compound, which is how a theme at vividness 0.90 *and* 7:1 reached 0.0267.  Depth is free
    //  on light paper and only mildly costly on dark, so it is not budgeted here.
    colorCheckTrue(theme->vividness >= real(0.90),
        (std::string("theme ") + theme->name + " stays within the chroma budget (vividness)").c_str());
    colorCheckTrue(theme->minContrast <= real(5.0),
        (std::string("theme ") + theme->name + " stays within the chroma budget (contrast)").c_str());

    for(int dark = 0; dark < 2; ++dark) {
      PaletteRecipe r = paletteThemeRecipe(*theme, dark != 0);
      Palette pal;
      generatePalette(r, &pal);
      std::string where = std::string("theme ") + theme->name + (dark ? " (dark)" : " (light)");

      colorCheckTrue(int(pal.families.size()) == r.families,
          (where + ": generated the families its recipe asked for").c_str());

      // legibility, the invariant the whole generator exists to hold
      for(size_t ff = 0; ff < pal.families.size(); ++ff) {
        if(srgbContrast(pal.families[ff].base, pal.paper) < r.minContrast - real(0.01)) {
          colorCheckTrue(false, (where + ": every base is legible on its own paper").c_str());
          break;
        }
      }

      // ...and the other half of legibility: colors you cannot tell apart are no more usable than
      //  colors you cannot see.  The reference is the palette that already ships - 12 families at
      //  full chroma, which separates at 0.0414 on light paper.  The shipped themes all clear 0.0302,
      //  i.e. 73% of it; 0.029 leaves a little headroom without letting a theme drift back toward the
      //  0.025 an unbudgeted table produced.
      real worstPair = real(1e9);
      for(size_t aa = 0; aa < pal.families.size(); ++aa) {
        for(size_t bb = aa + 1; bb < pal.families.size(); ++bb)
          worstPair = std::min(worstPair,
              oklabDeltaE(pal.families[aa].base, pal.families[bb].base));
      }
      colorCheckTrue(worstPair >= real(0.029),
          (where + ": its families stay distinguishable from each other").c_str());

      // the dark-mode contrast cap - uncapped, a high floor bleaches ink toward the paper's opposite
      //  and "Deep" comes out as the palest theme in the set
      if(dark)
        colorCheckTrue(r.minContrast <= real(4.5),
            (where + ": contrast is capped so deep themes do not bleach").c_str());

      // a recipe built from a theme must resolve back to that theme, or the dialog cannot ring the
      //  tile the document is actually using
      colorCheckTrue(paletteThemeIndexOf(r) == ii,
          (where + ": resolves back to its own theme").c_str());

      // ...and it must still resolve after a trip through the config, which stores every field as a
      //  float while `real` is a double.  An exact comparison passes the line above and fails here,
      //  which is what left a saved theme with no ring in the gallery.
      PaletteRecipe roundtripped = r;
      roundtripped.seedHue     = real(float(r.seedHue));
      roundtripped.vividness   = real(float(r.vividness));
      roundtripped.depth       = real(float(r.depth));
      roundtripped.minContrast = real(float(r.minContrast));
      roundtripped.jitter      = real(float(r.jitter));
      roundtripped.paperL      = real(float(r.paperL));
      roundtripped.paperWarm   = real(float(r.paperWarm));
      colorCheckTrue(paletteThemeIndexOf(roundtripped) == ii,
          (where + ": resolves back to its own theme after a float round trip").c_str());
    }
  }

  // ids are what a theme is looked up by, so duplicates would silently shadow one another
  for(int ii = 0; ii < paletteThemeCount(); ++ii) {
    const PaletteTheme* theme = paletteThemeByIndex(ii);
    colorCheckTrue(paletteThemeById(theme->id) == theme, "theme ids are unique and resolve");
  }

  // a recipe that is not one of ours must say so rather than claiming the nearest theme
  PaletteRecipe stranger = goldenRecipe();
  stranger.vividness = real(0.123);
  colorCheckTrue(paletteThemeIndexOf(stranger) == -1,
      "a recipe from outside the shipped set resolves to no theme");
}

int runColorTests()
{
  nColorChecksFailed = 0;
  testOklabAgainstSpec();
  testGolden();
  testLegibility();
  testGamut();
  testDeterminism();
  testSnapIdempotent();
  testMarkerSwatchesKeptBySnap();
  testRestyleRoundTrip();
  testUnknownGenerator();
  testBeatsNaive();
  testShippedThemes();
  return nColorChecksFailed;
}

#ifdef COLORTEST_MAIN
int main(int argc, char* argv[])
{
  if(argc > 1 && strcmp(argv[1], "--dump") == 0) {
    dumpGolden();
    return 0;
  }
  int nFailed = runColorTests();
  printf("%d color checks failed\n", nFailed);
  return nFailed;
}
#endif
