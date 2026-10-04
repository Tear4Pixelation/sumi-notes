#pragma once

// Synthetic hand-drawn strokes with a known answer.
//
// Each generator first decides what the user *meant* (the truth: a line's two ends, a square's four
//  corners, a circle's centre and radius), then builds the path a hand would take - rounded or
//  overshot corners, hooks where the pen lands and lifts, a loop that stops short of its start or runs
//  on past it, slightly bowed sides - and finally runs it through a hand model: low-frequency wobble,
//  slowing down in curves (so samples bunch up at corners, as they do on a real tablet), jitter, and
//  optionally rounding to whole units, which is what most platforms deliver (see JAGGED_STROKES.md).
//
// The deviations are all ones a person would not perceive as moving the shape: a rounded corner is
//  still perceived where the sides meet, so the truth stays at that intersection.  Real strokes traced
//  in recorder.html are the check that this model is not flattering the recognizer.

#include "strokefile.h"
#include <random>

namespace synth {

// label: line, square, circle, ellipse, scribble (a scratch-out) or other.  `pencil` draws them as
//  Apple Pencil input - 240 Hz, a long drifting hold - plus light arcs and wide zigzags (see synth.cpp)
std::vector<TestStroke> generate(const std::string& label, int count, std::mt19937& rng, bool pencil = false);

}  // namespace synth
