// When the radar is powered, and when the chrono switches itself off.
//
// Two P-channel MOSFET switches do the work (docs/hardware.md):
// - The soft power switch feeds everything after the battery's protection board. Holding MODE
//   turns it on directly; the Pico then holds it on (PIN_POWER_HOLD) and lets go to turn off.
//   Off, the battery only feeds the protection and charger boards (about 10-20 uA).
// - The radar switch feeds the radar and its amplifier (about 45 mA), which only need power
//   while measuring. After it switches on, the amplifier needs about half a second to settle.
//
// This class only decides; squidchrony.ino drives the pins. Portable C++ (host-tested).
#pragma once
#include <stdint.h>

struct PowerConfig {
  uint32_t boot_hold_ms = 400;        // a power-button boot needs MODE held this long more
  uint32_t auto_off_ms = 15 * 60000;  // turn off after this long without use; 0 = never
  uint32_t auto_off_warn_ms = 15000;  // warn this long before an idle shutdown
  uint32_t radar_hold_ms = 60000;     // radar stays on this long after it was last needed; 0 = always on
  uint32_t radar_settle_ms = 500;     // after switching the radar on, wait this long to measure
  float low_batt_v = 6.4f;            // turn off below this (3.2 V per cell)...
  uint32_t low_batt_ms = 10000;       // ...once it has stayed there this long
  uint32_t stuck_ms = 10000;          // MODE still held this long after switching on: stuck
};

struct PowerInputs {
  uint32_t now_ms = 0;
  bool want_measure = false;  // READ held, auto read on, or the phone's Measure switch
  bool activity = false;      // a button, a shot or a phone request since the last update
  bool mode_held = false;     // MODE button state (for the power-on check)
  bool batt_valid = false;    // false while there is no battery reading (e.g. on USB power)
  float batt_v = 0.0f;
};

// BootTap: MODE let go too soon after switching on. Stuck: MODE never let go after switching
// on (something pressing it in a bag): off again, though MODE keeps the power on until released.
enum class OffReason : uint8_t { None = 0, BootTap, User, Idle, Battery, Stuck };

struct PowerOutputs {
  bool booting = false;          // still checking that MODE stays held (power-button boot)
  bool radar_on = false;         // drive the radar switch
  bool measuring = false;        // radar on, settled, and wanted: let the tracker run
  bool radar_settled = false;    // true once, when a power-up has just settled (relearn background)
  uint32_t off_in_ms = 0;        // > 0 while an idle shutdown is coming
  OffReason off = OffReason::None;  // not None: switch off now
};

class PowerPolicy {
 public:
  // power_button_boot: MODE was down at power-on, so this boot came from the power button.
  void begin(uint32_t now_ms, bool power_button_boot, const PowerConfig& cfg);
  void setConfig(const PowerConfig& cfg) { cfg_ = cfg; }
  void requestOff() { off_requested_ = true; }  // MODE held to turn off, or the phone
  PowerOutputs update(const PowerInputs& in);

 private:
  PowerConfig cfg_;
  uint32_t boot_ms_ = 0;
  bool boot_check_ = false;
  bool boot_press_ = false;  // the press that switched it on is still held
  bool off_requested_ = false;
  uint32_t last_activity_ms_ = 0;
  uint32_t last_needed_ms_ = 0;  // radar last wanted
  bool radar_on_ = false;
  bool settled_ = false;
  uint32_t radar_on_ms_ = 0;
  bool low_ = false;
  uint32_t low_since_ms_ = 0;
};

const char* offReasonText(OffReason r);
