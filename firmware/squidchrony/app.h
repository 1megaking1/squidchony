// Core-0 application state, shared by the screen, the buttons and the web server.
// Implemented in squidchrony.ino.
#pragma once
#include <stdint.h>

#include "dsp_task.h"
#include "settings.h"
#include "shotlog.h"

namespace app {

extern Settings settings;
extern ShotLog shots;

struct Live {
  float batt_v = 0;
  int batt_pct = -1;
  bool batt_low = false;
  bool armed = false;         // measuring now (radar on and settled)
  bool want_measure = false;  // READ held, auto read, or the phone's switch
  bool radar_on = false;      // radar and amplifier powered
  uint32_t off_in_s = 0;      // > 0 while an idle shutdown is coming
  bool held = false;          // READ is held down
  bool web_armed = false;     // the phone asked to measure
  bool busy = false;       // a shot is being measured
  int clients = 0;         // phones on the Wi-Fi
  dsptask::Status dsp;
};
Live live();

uint32_t wallClockS();  // Unix time from the phone, 0 if never set
void setWallClockMs(uint64_t epoch_ms);

// Settings: set one key (validated), then call settingsChanged() once after a batch.
bool setSetting(const char* key, const char* value);
void settingsChanged();

void newString();
bool deleteLast();
bool undeleteLast();
bool renameString(uint16_t id, const char* name);
bool removeString(uint16_t id);
void clearAll();
// Scale the calibration so the current string's average reads ref_mps. False if there are
// too few shots or the change would be implausible (over 10%).
bool calibrate(float ref_mps);
void webArm(bool on);
void touch();     // user activity: keeps the screen on and the chrono from switching off
void powerOff();  // switch off (after saving) on the next pass through the main loop

const char* ssid();

}  // namespace app
