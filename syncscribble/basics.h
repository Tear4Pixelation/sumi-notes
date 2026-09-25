#ifndef BASICS_H
#define BASICS_H

#include "ulib/platformutil.h"
#include "ulib/stringutil.h"
#include "ulib/fileutil.h"


#define SCRIBBLE_LOG PLATFORM_LOG

typedef double Dim;

typedef int64_t Timestamp;

// Single knob for the size of the floating toolbar panels (buttons, icons, padding, insets).
//  1.0 is the original design-mockup size; 0.5 halves the whole toolbar.
static const Dim floatUIScale = 0.5;

// Where the floating panels sit relative to the window, and how tall one panel row is.  These live
//  here rather than in mainwindow.cpp (where the rest of the toolbar geometry is file-static)
//  because the sidebar has to line up with the toolbar: it takes its own inset from floatInset and
//  its top from floatTopInset + floatBtnSize + floatInset.  Duplicating the numbers there is how
//  the two drifted apart in the first place.
static const Dim floatInset = 27*floatUIScale;
static const Dim floatTopInset = 15*floatUIScale;
static const Dim floatBtnSize = 64*floatUIScale;
// The gap to the *window* edge, as opposed to floatInset, which is the gap between two panels.  The
//  two were the same number, which left the row noticeably further from the left and right edges
//  than from the top; the edge gap is now the top gap, so the chrome is inset equally on all three
//  sides.  The sidebar takes its own edge inset (and the gap below the toolbar) from here too.
static const Dim floatEdgeInset = floatTopInset;


#define MIN std::min
#define MAX std::max
#define ABS std::abs
#define SGN(x) ((x) >= 0 ? 1 : -1)
//#define CLAMP(x, min, max) std::min(MAX(x, min), max)
// number of elements in an array
#define NELEM(a) (sizeof(a)/sizeof(a[0]))
#define NELEMI(a) ((int)(sizeof(a)/sizeof(a[0])))

#define MAX_DIM REAL_MAX
#define MIN_DIM REAL_MIN
// previously, we had MIN/MAX_X/Y_DIM to provide better annotation, but they weren't used consistently

#ifndef NDEBUG
#define SCRIBBLE_TEST 1
#endif

extern bool SCRIBBLE_DEBUG;

#endif
