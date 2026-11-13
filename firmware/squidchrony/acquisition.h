// Sampling (runs on core 1).
//
// The ADC converts the radar channel and the battery channel alternately at 400 kS/s, so
// each gets 200 kS/s. One DMA channel copies the results into a 16 KB ring that the DMA
// wraps by itself, and re-triggers itself every 1024 samples, raising an interrupt. The
// interrupt (on core 1) filters and decimates the radar samples into a 50 kS/s history,
// and averages the battery samples.
//
// Nothing can overrun memory: if core 1 is held up (for instance while core 0 writes to
// flash, which pauses the other core), the DMA simply laps the ring; the interrupt notices
// the gap, drops what it missed and starts again cleanly.
#pragma once
#include <stdint.h>

#include "dsp.h"

namespace acq {

constexpr uint32_t RAW_RATE = 200000;  // per channel
constexpr uint32_t DEC_RATE = RAW_RATE / dsp::FIR_DECIM;
constexpr uint32_t HIST_BITS = 15;
constexpr uint32_t HIST_N = 1u << HIST_BITS;  // decimated samples kept (655 ms)
constexpr uint32_t CLIP_BLOCK_BITS = 8;        // clipping is tracked per 256 decimated samples

// Start the ADC, DMA and interrupt. Call on core 1.
void begin();
// Process any samples the interrupt has not handled yet (safe to call any time on core 1;
// a backstop in case an interrupt is late).
void service();

// Decimated samples written so far (free-running sample index).
uint32_t head();
// Copy count decimated samples starting at index first. False if they are not (or no
// longer) in the history.
bool read(uint32_t first, int count, int16_t* out);
// True if the amplifier output hit a rail anywhere between these decimated sample indices.
bool clippedBetween(uint32_t first, uint32_t last);
// Number of times sampling stopped and restarted (core 1 held up).
uint32_t gaps();

struct Levels {
  float battery_counts;  // mean battery ADC reading over the last 0.1 s
  uint16_t radar_min, radar_max;
  float radar_mean, radar_rms;  // over the last 0.1 s, ADC counts
  uint32_t adc_errors;
};
Levels levels();

// SampleSource for the track fit.
class History : public dsp::SampleSource {
 public:
  bool read(uint32_t first, int count, int16_t* out) const override {
    return acq::read(first, count, out);
  }
};

}  // namespace acq
