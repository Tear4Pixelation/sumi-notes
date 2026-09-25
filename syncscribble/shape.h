#pragma once

// Parametric shapes (SHAPES_SPEC.md).
//
// A shape is described entirely by a ShapeParams; the Path2D an Element renders is *generated* from it
//  and is therefore a cache, never the source of truth.  Everything shape-specific lives in the ShapeDef
//  table below - the mode handlers, the selector, the serializer and the undo item are written once
//  against ShapeDef, so adding a shape is one table entry plus two small functions.

#include "basics.h"
#include "ulib/path2d.h"

// The string ids below are what goes in a document; the numeric values here are an internal detail only
//  (ScribbleMode stores the *string* id in its config, so this enum can be reordered freely).
enum ShapeId { SHAPE_NONE = -1, SHAPE_LINE = 0, SHAPE_BOX, SHAPE_ELLIPSE,
    SHAPE_POLYLINE, SHAPE_CURVE, SHAPE_COUNT };

enum ShapeGesture { SHAPEGESTURE_DRAG_VECTOR, SHAPEGESTURE_DRAG_BBOX, SHAPEGESTURE_MULTIPOINT };

// Arrowheads and corner rounding are flags rather than shapes of their own: "arrow" is a line with an
//  end head, and a rounded box is a box with SHAPEFLAG_ROUNDED.  Each toggle collapses what would
//  otherwise be a pair of near-identical tools, and the toggles compose (a rounded polyline with a head
//  needs no third tool).  The options row disables a toggle when the active shape's ShapeDef says it
//  means nothing there.
enum ShapeFlags { SHAPEFLAG_CLOSED = 0x1, SHAPEFLAG_HEADSTART = 0x2, SHAPEFLAG_HEADEND = 0x4,
    SHAPEFLAG_SMOOTH = 0x8, SHAPEFLAG_ROUNDED = 0x10 };

struct ShapeParams
{
  int id = SHAPE_NONE;
  std::vector<Point> points;
  // corner radius, applied only when SHAPEFLAG_ROUNDED is set - keeping it when the flag is off means
  //  toggling rounding off and on again comes back to the radius you had
  Dim rx = 0;
  Dim ry = 0;
  int flags = 0;
  // SHAPE_CURVE only: 0 gives the pure approximating B-spline (roundest, ignores your points the most),
  //  1 makes the curve pass exactly through every point.  There is no second curve shape because the
  //  two ends of this range *are* the two curves - at 1 the output is bit-identical to interpolating
  //  Catmull-Rom - so the choice is a drag on one tool rather than a choice between two.
  // Per-shape rather than a global setting, so a document renders the same everywhere; the
  //  "shapeCurveTightness" preference is only the default for newly drawn curves, exactly as
  //  "shapeCornerRadius" is for rx.
  Dim tightness = 0;
  // not part of the descriptor: arrowhead size scales with the pen width, so buildPath needs it; it is
  //  read back from the node's stroke-width, not serialized separately
  Dim strokeWidth = 1;

  bool isValid() const { return id > SHAPE_NONE && id < SHAPE_COUNT; }
  bool rounded() const { return (flags & SHAPEFLAG_ROUNDED) && rx > 0; }
  // the two corners of a bbox shape, in any order, normalized to a Rect
  Rect rect() const;
};

struct ShapeHandle
{
  // RADIUS and TIGHTNESS are the "parameter" handles - they move a number rather than a point, and are
  //  drawn red to say so (see ShapeSelector::drawBG; the shape difference does not read at handle size)
  enum Type { POINT, BOX_CORNER, RADIUS, TIGHTNESS } type = POINT;
  Point pos;
  // index into ShapeParams::points for POINT, for the polyline RADIUS handle (the vertex it is
  //  anchored to) and for TIGHTNESS (the point it slides towards); corner number (0=TL, 1=TR, 2=BR,
  //  3=BL) for BOX_CORNER
  int index = 0;
};

struct ShapeDef
{
  const char* id;        // serialized in __shape
  const char* name;      // menu/tooltip text (untranslated)
  const char* icon;      // toolbar icon resource
  ShapeGesture gesture;
  int minPoints;
  int maxPoints;         // < 0 = unbounded
  // whether SHAPEFLAG_HEADSTART/HEADEND mean anything for this shape; the head toggles on the shape
  //  options row are disabled for shapes that have no ends (box, ellipse)
  bool allowsHeads;
  // likewise for SHAPEFLAG_ROUNDED: only shapes built from straight segments have corners to round
  bool allowsRounding;

  Path2D (*buildPath)(const ShapeParams& params);
  void (*getHandles)(const ShapeParams& params, std::vector<ShapeHandle>& handles);
  void (*applyConstraint)(ShapeParams& params);   // constrain modifier held: square/circle/45 degrees
};

const ShapeDef* shapeDef(int id);
// SHAPE_NONE if the string id is unknown.  Also accepts the ids of the shapes that became flags
//  ("arrow", "rbox", "rpolyline"), returning the base shape and the flags they imply in extraFlags.
int shapeIdByStringId(const char* id, int* extraFlags = NULL, Dim* tightness = NULL);
// the three multi-point shapes, which share their points, handles and arrowheads
bool shapeIsPolylineFamily(int id);

// convenience wrappers that tolerate an invalid/unknown shape id
Path2D buildShapePath(const ShapeParams& params);
void getShapeHandles(const ShapeParams& params, std::vector<ShapeHandle>& handles);
void applyShapeConstraint(ShapeParams& params);
// move the given handle to newpos, updating params accordingly
void dragShapeHandle(ShapeParams& params, const ShapeHandle& handle, Point newpos);
// Soft angle snap for a line or polyline-family point: if the segment from a neighbouring point to pos
//  lies within `tolerance` radians of a multiple of 45 degrees, pos is moved onto that direction.  With
//  both neighbours in range (an interior point) pos goes to where the two snapped directions cross, so a
//  corner can lock to a right angle.  Any other shape, or tolerance <= 0, returns pos unchanged.
Point snapShapeAngle(const ShapeParams& params, int index, Point pos, Dim tolerance);

std::string serializeShapePoints(const std::vector<Point>& points);
void parseShapePoints(const char* str, std::vector<Point>& points);

// head length is this multiple of the stroke width (SHAPES_SPEC.md: scale with width, never with length)
static constexpr Dim ARROWHEAD_WIDTHS = 5;
