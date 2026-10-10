/* perf-waitshim.c - LD_PRELOAD shim used by tools/perf-bench.sh, never shipped.
 * The in-app benchmarks call ScribbleApp::processEvents() between steps, which blocks in
 * SDL_WaitEvent until an event arrives; a headless cage delivers almost none, so a run took
 * more than 10 minutes of pure waiting. This makes SDL_WaitEvent poll instead and return an
 * inert event when the queue is empty, so the timings contain no sleeping at all. Type 0 would crash: ScribbleSync::sdlEventType is 0 until a sync is
 * registered, so a zero-type event is taken for a sync event. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <string.h>
#include <SDL2/SDL.h>

int SDL_WaitEvent(SDL_Event* event)
{
  static int (*pollEvent)(SDL_Event*) = 0;
  if(!pollEvent)
    pollEvent = (int (*)(SDL_Event*))dlsym(RTLD_NEXT, "SDL_PollEvent");
  if(pollEvent(event))
    return 1;
  memset(event, 0, sizeof(*event));
  event->type = 0xFFFE;
  return 1;
}
