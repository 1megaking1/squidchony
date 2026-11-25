// The two LED push buttons. Buttons pull the pin to ground; LEDs switch fully on or off
// (no dimming: PWM would show up as noise in the radar band).
//
// MODE is also the power button: wired through a diode to the soft power switch, it turns the
// chrono on by itself (docs/hardware.md). The press that switched it on produces no events.
#pragma once
#include <stdint.h>

namespace controls {

constexpr uint32_t LONG_MS = 700;   // a "long" press is released after at least this
constexpr uint32_t OFF_MS = 3000;   // holding MODE this long turns the chrono off

enum Event : uint8_t {
  NONE = 0,
  READ_DOWN,
  READ_UP,
  READ_CLICK,  // released before LONG_MS
  READ_LONG,   // released after LONG_MS (but before OFF_MS)
  MODE_DOWN,
  MODE_UP,
  MODE_CLICK,
  MODE_LONG,
  MODE_OFF,    // held for OFF_MS (fires once, while still held)
};

// Call first thing at power-on. Returns true if MODE was already down, meaning this power-up
// came from the power button.
bool begin();
// Debounce both buttons. Call often; returns queued events one at a time.
Event poll(uint32_t now_ms);
bool readHeld();
bool modeHeld();
uint32_t modeHeldMs(uint32_t now_ms);  // how long MODE has been down (0 if up or ignored)
void setLeds(bool read_on, bool mode_on);

}  // namespace controls
