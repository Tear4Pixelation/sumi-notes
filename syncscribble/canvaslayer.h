#pragma once

#include <vector>
#include <functional>
#include "ulib/geom.h"

class Painter;
struct NVGcontext;

// Retained offscreen copy of a canvas (ScribbleView::prepareLayer): drawn into before the GUI frame begins
//  (nanovg frames cannot nest), then composited into that frame 1:1 as one image.  GL: two FBO textures in
//  the screen framebuffer's format, ping-ponged by shift(); SW: one pixel buffer in the screen's byte order.
//  Implemented in application.cpp, which owns the GL/SW framebuffer setup.
class CanvasLayer
{
public:
  ~CanvasLayer();
  // (re)allocate for w x h device pixels; true if the contents are now undefined
  bool resize(int w, int h);
  // move the contents by (dx, dy) device pixels; the uncovered strips are left undefined
  void shift(int dx, int dy);
  // draw into the layer clipped to each of rects (layer pixels); draw() is called once per rect with the
  //  painter's transform at identity (= layer pixels) and its clip set to the rect
  void render(Painter* p, const std::vector<Rect>& rects, const std::function<void(Painter*, const Rect&)>& draw);
  // draw the layer at device pixel (x, y), unscaled, into the frame being painted
  void composite(Painter* p, int x, int y);
  bool isValid() const { return width > 0 && height > 0; }

  int width = 0;
  int height = 0;

private:
  void release();

  NVGcontext* vg = NULL;
  void* fb[2] = {NULL, NULL};  // GL: NVGLUframebuffer*
  int cur = 0;
  unsigned char* pixels = NULL;  // SW
  int image = -1;  // SW: nanovg image over pixels
  int format = -1;  // GL: image flags; SW: packed channel shifts
};
