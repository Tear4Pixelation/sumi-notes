// Sub-pixel pointer input on sdl2-compat.
//
// Write uses the SDL2 API.  On a distribution that ships sdl2-compat, that API is implemented on top of
// SDL3, and SDL3's mouse events carry float coordinates: on a Wayland compositor the pointer position
// arrives in 1/256 px, and with a stylus or a high-resolution mouse that fraction is real information.
// sdl2-compat then converts each event to the SDL2 struct, whose x/y are Sint32, with a plain cast -
// and that cast is where the sub-pixel position is lost.  With a mouse at 1-2 px per sample, whole-pixel
// positions limit the direction of each stroke segment to a handful of lattice angles (0, 26.6, 45,
// 63.4, 90 deg), which is what makes a hand-drawn curve render as runs of straight segments.
//
// sdl2-compat keeps libSDL3 loaded in this process, so the float values are recoverable: register an
// SDL3 event watch through dlsym (no compile-time SDL3 dependency), record each mouse event's float
// position as SDL3 pushes it, and when the truncated SDL2 event is dispatched look the sample back up by
// type, timestamp and truncated coordinates.  Pen input that SDL3 turns into mouse events is recovered
// the same way, and its pressure comes along from the SDL3 pen axis events, which the SDL2 API never
// carried on Linux at all.
//
// None of this runs on a real SDL2 (the vendored fork CI ships): libSDL3 is not loaded there, and that
// build gets sub-pixel pen input from XInput2 via linuxtablet.c instead.

#include <dlfcn.h>
#include <cstdint>
#include <cstring>
#include "sdl3input.h"

namespace {

// The SDL3 event structs, restricted to the fields read here.  SDL3 guarantees its ABI for the 3.x
//  series; SDL_Event itself is a 128-byte union and only `type` at offset 0 is read generically.
struct Sdl3MouseMotionEvent {
  uint32_t type; uint32_t reserved; uint64_t timestamp; uint32_t windowID; uint32_t which; uint32_t state;
  float x, y, xrel, yrel;
};
struct Sdl3MouseButtonEvent {
  uint32_t type; uint32_t reserved; uint64_t timestamp; uint32_t windowID; uint32_t which;
  uint8_t button; bool down; uint8_t clicks; uint8_t padding;
  float x, y;
};
struct Sdl3PenAxisEvent {
  uint32_t type; uint32_t reserved; uint64_t timestamp; uint32_t windowID; uint32_t which; uint32_t penState;
  float x, y; int32_t axis; float value;
};
enum : uint32_t {
  SDL3_EVENT_MOUSE_MOTION = 0x400, SDL3_EVENT_MOUSE_BUTTON_DOWN = 0x401, SDL3_EVENT_MOUSE_BUTTON_UP = 0x402,
  SDL3_EVENT_PEN_AXIS = 0x1306, SDL3_PEN_AXIS_PRESSURE = 0, SDL3_PEN_MOUSEID = uint32_t(-2)
};
typedef bool (*Sdl3EventFilter)(void* userdata, void* event);
typedef bool (*Sdl3AddEventWatch)(Sdl3EventFilter filter, void* userdata);

struct Sample {
  uint32_t type = 0;      // SDL2 event type this sample will surface as
  uint32_t timestampMs = 0;
  int32_t ix = 0, iy = 0; // what sdl2-compat will put in the SDL2 event
  float x = 0, y = 0, pressure = 1;
  bool used = false;
};

// Sized for a burst of events between two frames; a sample is consumed on first match so a stationary
//  pointer reporting the same integer position twice cannot be matched to the wrong event.
constexpr int RING_SIZE = 256;
Sample ring[RING_SIZE];
int ringHead = 0;
float penPressure = 1;  // most recent pressure axis value; attached to subsequent pen-as-mouse samples

void record(uint32_t type2, uint64_t timestampNs, uint32_t which, float x, float y)
{
  Sample& sample = ring[ringHead];
  ringHead = (ringHead + 1) % RING_SIZE;
  sample.type = type2;
  sample.timestampMs = uint32_t(timestampNs / 1000000);  // SDL_NS_TO_MS, as sdl2-compat does
  sample.ix = int32_t(x);  // sdl2-compat: event2->motion.x = (Sint32)event3->motion.x
  sample.iy = int32_t(y);
  sample.x = x;
  sample.y = y;
  sample.pressure = which == SDL3_PEN_MOUSEID ? penPressure : 1;
  sample.used = false;
}

bool eventWatch(void*, void* event3)
{
  const uint32_t type = *static_cast<const uint32_t*>(event3);
  if(type == SDL3_EVENT_PEN_AXIS) {
    const Sdl3PenAxisEvent* axis = static_cast<const Sdl3PenAxisEvent*>(event3);
    if(axis->axis == SDL3_PEN_AXIS_PRESSURE)
      penPressure = axis->value;
  }
  else if(type == SDL3_EVENT_MOUSE_MOTION) {
    const Sdl3MouseMotionEvent* motion = static_cast<const Sdl3MouseMotionEvent*>(event3);
    record(SDL_MOUSEMOTION, motion->timestamp, motion->which, motion->x, motion->y);
  }
  else if(type == SDL3_EVENT_MOUSE_BUTTON_DOWN || type == SDL3_EVENT_MOUSE_BUTTON_UP) {
    const Sdl3MouseButtonEvent* button = static_cast<const Sdl3MouseButtonEvent*>(event3);
    record(type == SDL3_EVENT_MOUSE_BUTTON_DOWN ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP,
        button->timestamp, button->which, button->x, button->y);
  }
  return true;  // keep the event
}

void* sdl3Library()
{
  // RTLD_NOLOAD: only find libSDL3 if something (sdl2-compat) has already loaded it; never load it
  //  ourselves, since a real SDL2 build must not start talking to an SDL3 that happens to be installed
  return dlopen("libSDL3.so.0", RTLD_NOLOAD | RTLD_NOW);
}

}  // namespace

bool sdl3InputOnCompat()
{
  return sdl3Library() != NULL;
}

bool sdl3InputInit()
{
  void* lib = sdl3Library();
  if(!lib)
    return false;
  void* symbol = dlsym(lib, "SDL_AddEventWatch");
  if(!symbol)
    return false;
  Sdl3AddEventWatch addWatch;  // memcpy rather than a cast: object-to-function pointer casts are only conditionally supported
  static_assert(sizeof(addWatch) == sizeof(symbol), "function and object pointers differ in size");
  memcpy(&addWatch, &symbol, sizeof(addWatch));
  return addWatch(eventWatch, NULL);
}

bool sdl3SubpixelPoint(const SDL_Event* event, float* x, float* y, float* pressure)
{
  int32_t ix, iy;
  if(event->type == SDL_MOUSEMOTION) {
    ix = event->motion.x;
    iy = event->motion.y;
  }
  else if(event->type == SDL_MOUSEBUTTONDOWN || event->type == SDL_MOUSEBUTTONUP) {
    ix = event->button.x;
    iy = event->button.y;
  }
  else
    return false;
  const uint32_t timestampMs = event->common.timestamp;
  // newest first: the SDL2 event being dispatched is normally the most recent sample recorded
  for(int ii = 1; ii <= RING_SIZE; ++ii) {
    Sample& sample = ring[(ringHead - ii + RING_SIZE) % RING_SIZE];
    if(sample.used || sample.type != event->type || sample.ix != ix || sample.iy != iy)
      continue;
    // a millisecond of slack covers any rounding between the two libraries' clocks
    if(sample.timestampMs + 1 < timestampMs || timestampMs + 1 < sample.timestampMs)
      continue;
    sample.used = true;
    *x = sample.x;
    *y = sample.y;
    *pressure = sample.pressure;
    return true;
  }
  return false;
}
