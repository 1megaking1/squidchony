// The 2.0" 320x240 ST7789 screen (Adafruit 4311), in landscape.
//
//   +--------------------------------------------------+
//   | MUZZLE  PAINT              Wi 1   7.6V 58%       |  status line
//   |  2 8 5 . 4                                fps    |  last shot, LCD-style digits
//   | #12  28 dB  +1.2 vs avg                          |  shot details or a message
//   | AVG      | SD       | ES                         |
//   | LOW      | HIGH     | RATE                       |  string statistics (or the menu)
//   +--------------------------------------------------+
//
// The caller redraws only while no shot is being measured: SPI traffic and the LED current
// steps can couple into the radar amplifier.
#pragma once
#include <stdint.h>

namespace display {

struct Model {
  // status line
  const char* reading = "MUZZLE";  // or "AT CHRONO"
  const char* proj = "PAINT";
  const char* units = "fps";
  bool armed = false;
  bool auto_read = false;
  int wifi = 0;  // 0 off, 1 waiting for the phone, 2 phone connected
  float batt_v = 0;
  int batt_pct = -1;  // -1 = unknown
  bool batt_low = false;
  // readout
  bool have_shot = false;
  float value = 0;   // last shot in user units
  int decimals = 1;  // 1 for fps, 2 for m/s
  int shot_no = 0;
  float snr_db = 0;
  bool weak = false;
  bool has_vs_avg = false;
  float vs_avg = 0;
  // statistics in user units
  int n = 0;
  float avg = 0, sd = 0, es = 0, lo = 0, hi = 0, rof = 0;
  // message instead of the shot details (null = none)
  const char* message = nullptr;
  // menu instead of the statistics
  bool menu = false;
  int menu_index = 0, menu_count = 0;
  const char* menu_item = "";
  const char* menu_value = "";
};

void begin();
void setBacklight(bool on);
bool backlight();
// Draw one section that changed since it was last drawn. Returns true while more sections
// are waiting, so the caller can stop in between (for instance when a shot starts).
bool update(const Model& m);
void invalidate();  // redraw everything
void splash(const char* line1, const char* line2);

}  // namespace display
