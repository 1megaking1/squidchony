// The two LED push buttons. Buttons pull the pin to ground; LEDs switch fully on or off
// (no dimming: PWM would show up as noise in the radar band).
#pragma once
#include <stdint.h>

namespace controls {

enum Event : uint8_t {
  NONE = 0,
  READ_DOWN,
  READ_UP,
  READ_CLICK,  // released before the long-press time
  READ_LONG,   // held for the long-press time (fires once, while still held)
  MODE_DOWN,
  MODE_UP,
  MODE_CLICK,
  MODE_LONG,
};

void begin();
// Debounce both buttons. Call often; returns queued events one at a time.
Event poll(uint32_t now_ms);
bool readHeld();
bool modeHeld();
bool modePressedRaw();  // undebounced, for the "held at power-on" check
void setLeds(bool read_on, bool mode_on);

}  // namespace controls
