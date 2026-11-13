// The measuring loop on core 1, and what core 0 sees of it.
//
// Core 1 analyzes a frame every 1.28 ms, follows tracks and fits finished ones; each shot goes
// into a queue for core 0. Core 0 hands over the configuration and whether to measure
// ("armed"), and asks whether a track is in progress before doing anything noisy (redrawing
// the screen, switching LEDs, writing flash).
#pragma once
#include <stdint.h>

#include "dsp.h"

namespace dsptask {

struct ShotMsg {
  dsp::ShotResult r;
  uint32_t seq;  // 1, 2, 3, ... per shot since power-up
};

struct Status {
  float noise_db = 0;     // smoothed noise floor per FFT bin, dB re 1 ADC count^2
  float peak_snr_db = 0;  // strongest tone in the latest frame
  float peak_hz = 0;
  uint32_t frames = 0;    // frames analyzed
  uint32_t shots = 0;
  uint32_t lost = 0;      // tracks too short or unfittable (weak echoes)
  uint32_t rejected = 0;  // steady tones dropped
  uint32_t gaps = 0;      // sampling restarts (core 1 was held up)
  uint32_t dropped = 0;   // shots lost because core 0 did not collect them
  uint32_t lag_frames = 0;
};

// Shared primitives. Core 0 calls this before letting core 1 start.
void init(const dsp::Config& cfg);
// Core 1
void begin();
void loop();
// Core 0
void setConfig(const dsp::Config& cfg);
void setArmed(bool armed);
bool busy();  // a track is in progress, or analysis is catching up
bool popShot(ShotMsg* out);
Status status();
// Decimated samples (50 kS/s) around the last shot, for the WAV download. While locked,
// core 1 leaves the buffer alone (a shot fired meanwhile just isn't kept).
bool lockLastShot(const int16_t** samples, uint32_t* count, uint32_t* seq);
void unlockLastShot();

}  // namespace dsptask
