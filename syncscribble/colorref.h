#pragma once

// A swatch, or a pen's colour, as a SLOT in the document theme rather than as a literal sRGB value
//  (docs/agent/pen-storage.md).  Swatches used to be stored as literal colours, so a red picked under one
//  theme was a colour no other theme has: opening a document with another theme found the row "not of
//  this theme" and reseeded it (the user's picks gone), or - on a switch to a tab already in memory, which
//  does not rebuild the row - left a stale row whose taps snapped the pen to a colour no swatch held (no
//  selection ring).  A slot is the family's ordinal and variant, so the same row shows each theme's own
//  red in the same place.  The ordinal is the family's index, the same rule Palette::mapFrom() and
//  matchReference() use; every shipped theme has the same family count and seed, so index i is the same
//  tone in each.
// Pure colour math, no GUI: built standalone by scribbletest/colortest.cpp.

#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "markercolor.h"

struct ColorRef
{
  static constexpr int LITERAL = -2;  // a colour of its own: a custom colour, or one from no theme
  static constexpr int NEUTRAL = -1;  // the theme's neutral (black on light paper, white on dark)

  int family = LITERAL;
  int variant = PALETTE_BASE;
  // the colour itself for a literal; for a slot, what it last resolved to - shown where there is no
  //  theme to resolve it in, and how a pen tells that it still holds the slot's colour
  Color literal = Color(0, 0, 0);

  bool isSlot() const { return family != LITERAL; }

  static ColorRef ofLiteral(Color c) { ColorRef ref; ref.literal = c; return ref; }

  // the slot c occupies in pal, exactly (alpha ignored, as Palette::indexOf does), else a literal
  static ColorRef inPalette(const Palette* pal, Color c)
  {
    ColorRef ref = ofLiteral(c);
    int fam = 0, var = PALETTE_BASE;
    if(pal && !pal->families.empty() && pal->indexOf(c, &fam, &var)) {
      ref.family = fam;
      ref.variant = var;
    }
    return ref;
  }

  // a pen's colour: the marker draws each family as its vivid highlighter (vividMarker()), which is
  //  deliberately not a palette member, so for the marker those count as the family's slot too
  static ColorRef ofPen(const Palette* pal, Color c, bool marker)
  {
    if(marker && pal) {
      for(size_t ii = 0; ii < pal->families.size(); ++ii) {
        if(vividMarker(pal->families[ii].hl, pal->recipe.vividness).opaque() == c.opaque()) {
          ColorRef ref = ofLiteral(c);
          ref.family = int(ii);
          ref.variant = PALETTE_HL;
          return ref;
        }
      }
    }
    return inPalette(pal, c);
  }

  // the colour this is in pal; a slot pal cannot hold (no theme, fewer families) falls back to the
  //  colour it last resolved to.  Alpha is the tool's (the marker's, the user's), never the palette's,
  //  except for the highlighter variant, whose translucency is what it is.
  Color resolve(const Palette* pal) const
  {
    if(!isSlot() || !pal || pal->families.empty() || family >= int(pal->families.size()))
      return literal;
    Color out = family == NEUTRAL ? pal->neutral : pal->families[family].variant(variant);
    if(variant != PALETTE_HL)
      out.setAlpha(literal.alpha());
    return out;
  }

  // a pen's colour in pal: as resolve(), but the marker's families are their vivid highlighter, and the
  //  pen keeps its own alpha (the marker's 165, tape's opaque)
  Color resolvePen(const Palette* pal, bool marker, int alpha) const
  {
    Color out = literal;
    if(isSlot() && pal && !pal->families.empty() && family < int(pal->families.size())) {
      if(family == NEUTRAL)
        out = pal->neutral;
      else if(marker)
        out = vividMarker(pal->families[family].hl, pal->recipe.vividness);
      else
        out = pal->families[family].variant(variant);
    }
    out.setAlpha(alpha);
    return out;
  }

  // "#AARRGGBB" for a literal (what savedColors always held), "@n#AARRGGBB" for the neutral,
  //  "@3#AARRGGBB" for family 3's base, "@3.2#AARRGGBB" for another variant; the hex after a slot is the
  //  fallback colour.  Written with every digit: nothing here is rounded.
  std::string serialize() const
  {
    char buf[32];
    unsigned int argb = literal.argb();
    if(!isSlot())
      snprintf(buf, sizeof(buf), "#%08X", argb);
    else if(family == NEUTRAL)
      snprintf(buf, sizeof(buf), "@n#%08X", argb);
    else if(variant == PALETTE_BASE)
      snprintf(buf, sizeof(buf), "@%d#%08X", family, argb);
    else
      snprintf(buf, sizeof(buf), "@%d.%d#%08X", family, variant, argb);
    return buf;
  }

  // false if s is not in serialize()'s form (a legacy colour name such as "black" is the caller's to read)
  static bool parse(const char* s, ColorRef* out)
  {
    ColorRef ref;
    const char* pos = s;
    if(*pos == '@') {
      ++pos;
      if(*pos == 'n') {
        ref.family = NEUTRAL;
        ++pos;
      }
      else {
        char* end = NULL;
        long fam = strtol(pos, &end, 10);
        if(end == pos || fam < 0 || fam > 1000)
          return false;
        ref.family = int(fam);
        pos = end;
        if(*pos == '.') {
          ++pos;
          long var = strtol(pos, &end, 10);
          if(end == pos || var < 0 || var >= PALETTE_NUM_VARIANTS)
            return false;
          ref.variant = int(var);
          pos = end;
        }
      }
    }
    if(*pos != '#' || strlen(pos + 1) != 8)
      return false;
    char* end = NULL;
    unsigned long argb = strtoul(pos + 1, &end, 16);
    if(!end || *end != '\0')
      return false;
    ref.literal = Color::fromArgb((unsigned int)argb);
    *out = ref;
    return true;
  }
};
