#ifndef SDL3INPUT_H
#define SDL3INPUT_H

// Sub-pixel pointer input when Write is running on sdl2-compat (the SDL2 API reimplemented on top of
//  SDL3, which Arch, Fedora and Ubuntu now ship in place of SDL2).  See sdl3input.cpp for why this exists.

#include "SDL.h"

// True if the process is running on sdl2-compat, i.e. libSDL3 is loaded underneath the SDL2 API.
bool sdl3InputOnCompat();

// Must be called after SDL_Init().  Registers an SDL3 event watch that records the float coordinates of
//  every mouse motion and button event before sdl2-compat truncates them to integers.  Returns false
//  (and does nothing) when not running on sdl2-compat.
bool sdl3InputInit();

// For an SDL2 mouse motion or button event, recover the SDL3 float position (and, for a pen reaching
//  us as a mouse, its pressure) that sdl2-compat truncated.  Returns false if no matching sample is
//  known, in which case the caller keeps the integer coordinates.
bool sdl3SubpixelPoint(const SDL_Event* event, float* x, float* y, float* pressure);

#endif
